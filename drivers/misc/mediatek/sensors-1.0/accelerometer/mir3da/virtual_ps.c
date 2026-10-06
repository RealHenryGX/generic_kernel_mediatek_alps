/* SPDX-License-Identifier: GPL-2.0 */
/* A25-only PS endpoint. It owns no ALSPS context and never calls
 * ps_register_control_path. Static queue/locks outlive open file descriptors;
 * a generation cookie makes descriptors from a removed device stay dead.
 * Lock order: ops_lock -> mir_lock -> queue_lock. remove_group outside locks.
 */
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "sensor_attr.h"
#include "sensor_event.h"
#include "hwmsensor.h"
#include "virtual_ps.h"

#define A25_PS_QUEUE_SIZE 64
static DEFINE_MUTEX(ps_ops_lock);
#include "../a25_queue_reader.h"
static struct a25_read_epoch *ps_epoch;
static DEFINE_MUTEX(ps_queue_lock);
static DECLARE_WAIT_QUEUE_HEAD(ps_wait);
static struct a25_ps_ops ps_ops;
static struct sensor_event ps_queue[A25_PS_QUEUE_SIZE];
static unsigned int ps_head, ps_tail, ps_count;
static u64 ps_generation;
static bool ps_live, ps_registered;

int a25_ps_event(int value, s64 timestamp, bool flush)
{
	struct sensor_event event = {0};
	int ret = 0;

	event.time_stamp = timestamp;
	event.flush_action = flush ? FLUSH_ACTION : DATA_ACTION;
	event.status = 2;
	/* HAL subtracts one; keep the stock wire domain, not raw near. */
	event.word[0] = flush ? 0 : value + 1;
	mutex_lock(&ps_queue_lock);
	if (!ps_live)
		ret = -ENODEV;
	else if (ps_count == A25_PS_QUEUE_SIZE)
		ret = -ENOSPC;
	else {
		ps_queue[ps_head] = event;
		ps_head = (ps_head + 1) % A25_PS_QUEUE_SIZE;
		ps_count++;
	}
	mutex_unlock(&ps_queue_lock);
	wake_up_interruptible(&ps_wait);
	return ret;
}

static int a25_ps_open(struct inode *inode, struct file *file)
{
	int ret;

	mutex_lock(&ps_queue_lock);
	if (!ps_live) {
		mutex_unlock(&ps_queue_lock);
		return -ENODEV;
	}
	ret = nonseekable_open(inode, file);
	if (!ret) {
		ps_epoch->refs++;
		file->private_data = ps_epoch;
	}
	mutex_unlock(&ps_queue_lock);
	return ret;
}
static int a25_ps_release(struct inode *inode, struct file *file)
{
	mutex_lock(&ps_queue_lock);
	a25_read_epoch_put(file->private_data);
	mutex_unlock(&ps_queue_lock);
	return 0;
}
static ssize_t a25_ps_read(struct file *file, char __user *buf,
	size_t count, loff_t *pos)
{
	struct a25_read_epoch *epoch = file->private_data;
	struct sensor_event snapshot;
	ssize_t done = 0;
	int fault;

	if (count && count < sizeof(snapshot))
		return -EINVAL;
	/* Serialization is scoped to this generation, not a static global.
	 * Do not hold the metadata lock across a user page fault.
	 */
	mutex_lock(&epoch->readers);
	mutex_lock(&ps_queue_lock);
	if (!ps_live || epoch != ps_epoch) {
		done = -ENODEV;
		goto out;
	}
	while (ps_count && count - done >= sizeof(snapshot)) {
		snapshot = ps_queue[ps_tail];
		mutex_unlock(&ps_queue_lock);
		fault = copy_to_user(buf + done, &snapshot, sizeof(snapshot));
		mutex_lock(&ps_queue_lock);
		if (fault) {
			if (!done)
				done = -EFAULT;
			break;
		}
		done += sizeof(snapshot);
		/* A completed old snapshot can be returned, but never committed
		 * to a replacement ring. EFAULT never commits a reserved item.
		 */
		if (!ps_live || epoch != ps_epoch)
			break;
		ps_tail = (ps_tail + 1) % A25_PS_QUEUE_SIZE;
		ps_count--;
	}
out:
	mutex_unlock(&ps_queue_lock);
	mutex_unlock(&epoch->readers);
	return done;
}
static unsigned int a25_ps_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = 0;

	poll_wait(file, &ps_wait, wait);
	mutex_lock(&ps_queue_lock);
	if (!ps_live || file->private_data != ps_epoch)
		mask = POLLHUP | POLLERR;
	else if (ps_count)
		mask = POLLIN | POLLRDNORM;
	mutex_unlock(&ps_queue_lock);
	return mask;
}
static const struct file_operations a25_ps_fops = {
	.owner = THIS_MODULE, .open = a25_ps_open, .release = a25_ps_release,
	.read = a25_ps_read, .poll = a25_ps_poll, .llseek = no_llseek,
};
static struct sensor_attr_t ps_endpoint = {
	.minor = ID_PROXIMITY, .name = "m_ps_misc", .fops = &a25_ps_fops,
};
static ssize_t psactive_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "100\n");
}
static ssize_t psactive_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int en, ret;

	ret = kstrtoint(buf, 10, &en);
	if (ret || (en != 0 && en != 1))
		return -EINVAL;
	mutex_lock(&ps_ops_lock);
	ret = ps_registered ? ps_ops.enable(en) : -ENODEV;
	mutex_unlock(&ps_ops_lock);
	return ret ? ret : count;
}
static ssize_t psbatch_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int handle, flag, ret;
	long long ns, latency;

	if (sscanf(buf, "%d,%d,%lld,%lld", &handle, &flag, &ns, &latency) != 4)
		return -EINVAL;
	mutex_lock(&ps_ops_lock);
	ret = ps_registered ? ps_ops.batch(flag, ns, latency) : -ENODEV;
	mutex_unlock(&ps_ops_lock);
	return ret ? ret : count;
}
static ssize_t psdelay_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	s64 ns;
	int ret = kstrtoll(buf, 10, &ns);

	if (ret)
		return ret;
	mutex_lock(&ps_ops_lock);
	ret = ps_registered ? ps_ops.batch(0, ns, 0) : -ENODEV;
	mutex_unlock(&ps_ops_lock);
	return ret ? ret : count;
}
static ssize_t psflush_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int handle, ret = kstrtoint(buf, 10, &handle);

	if (ret)
		return ret;
	mutex_lock(&ps_ops_lock);
	ret = !ps_registered ? -ENODEV :
		(ps_ops.flush ? ps_ops.flush() : -EOPNOTSUPP);
	mutex_unlock(&ps_ops_lock);
	return ret ? ret : count;
}
static ssize_t pscali_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;

	/* Binary 2 x int32, no truncation to u8 and no NULL-success. */
	if (count != 8)
		return -EINVAL;
	mutex_lock(&ps_ops_lock);
	ret = !ps_registered ? -ENODEV :
		(ps_ops.cali ? ps_ops.cali(buf, count) : -EOPNOTSUPP);
	mutex_unlock(&ps_ops_lock);
	return ret ? ret : count;
}
static DEVICE_ATTR_RW(psactive);
static DEVICE_ATTR_WO(psbatch);
static DEVICE_ATTR_WO(psdelay);
static DEVICE_ATTR_WO(psflush);
static DEVICE_ATTR_WO(pscali);
static struct attribute *ps_attributes[] = {
	&dev_attr_psactive.attr, &dev_attr_psbatch.attr, &dev_attr_psdelay.attr,
	&dev_attr_psflush.attr, &dev_attr_pscali.attr, NULL,
};
static const struct attribute_group ps_group = { .attrs = ps_attributes };
static const struct attribute_group *ps_groups[] = { &ps_group, NULL };

int a25_ps_register(const struct a25_ps_ops *ops)
{
	int ret;
	struct a25_read_epoch *epoch;

	if (!ops || !ops->enable || !ops->batch || !ops->flush)
		return -EINVAL;
	mutex_lock(&ps_ops_lock);
	if (ps_registered) {
		mutex_unlock(&ps_ops_lock);
		return -EBUSY;
	}
	epoch = a25_read_epoch_new();
	if (!epoch) {
		mutex_unlock(&ps_ops_lock);
		return -ENOMEM;
	}
	ps_ops = *ops;
	mutex_lock(&ps_queue_lock);
	ps_epoch = epoch;
	ps_generation++;
	ps_epoch->generation = ps_generation;
	ps_head = ps_tail = ps_count = 0;
	ps_live = true;
	mutex_unlock(&ps_queue_lock);
	mutex_unlock(&ps_ops_lock);
	/* Shares the sensor class/minor arbitration, NOT ALSPS's context or
	 * sensor_event's unrefcounted dynamic buffers. Publish all attrs at once.
	 * Do not hold ops_lock during device_create's partial-group rollback:
	 * kernfs may wait for a store that itself needs ops_lock. MIR probe's
	 * reserved singleton serializes register/unregister; early stores fail.
	 */
	ret = sensor_attr_register_owned(&ps_endpoint, ps_groups);
	mutex_lock(&ps_ops_lock);
	if (!ret)
		ps_registered = true;
	else {
		mutex_lock(&ps_queue_lock);
		ps_live = false;
		a25_read_epoch_put(ps_epoch);
		ps_epoch = NULL;
		mutex_unlock(&ps_queue_lock);
		memset(&ps_ops, 0, sizeof(ps_ops));
	}
	mutex_unlock(&ps_ops_lock);
	return ret;
}
void a25_ps_unregister(void)
{
	/* Caller has first stopped the producer and synchronously drained work.
	 * sysfs removal waits active stores: do NOT hold ops_lock here.
	 */
	if (!ps_registered)
		return;
	sysfs_remove_group(&ps_endpoint.this_device->kobj, &ps_group);
	mutex_lock(&ps_ops_lock);
	ps_registered = false;
	memset(&ps_ops, 0, sizeof(ps_ops));
	mutex_lock(&ps_queue_lock);
	ps_live = false;
	a25_read_epoch_put(ps_epoch);
	ps_epoch = NULL;
	ps_count = 0;
	mutex_unlock(&ps_queue_lock);
	mutex_unlock(&ps_ops_lock);
	wake_up_interruptible(&ps_wait);
	sensor_attr_deregister_owned(&ps_endpoint);
}
