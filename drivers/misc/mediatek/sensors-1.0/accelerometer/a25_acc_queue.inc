/* SPDX-License-Identifier: GPL-2.0 */
/* A25-only ACC queue: static storage survives old descriptors on remove.
 * Included after acc_attribute_group, with helpers declared before fops.
 */
#include "a25_queue_reader.h"
static struct a25_read_epoch *a25_acc_epoch;
static DEFINE_MUTEX(a25_acc_queue_lock);
static DECLARE_WAIT_QUEUE_HEAD(a25_acc_wait);
static struct sensor_event a25_acc_queue[256];
static unsigned int a25_acc_head, a25_acc_tail, a25_acc_count;
static u64 a25_acc_generation;
static bool a25_acc_live, a25_acc_registered;
static int a25_acc_emit(const struct sensor_event *event)
{
	int ret = 0;

	mutex_lock(&a25_acc_queue_lock);
	if (!a25_acc_live)
		ret = -ENODEV;
	else if (a25_acc_count == ARRAY_SIZE(a25_acc_queue))
		ret = -ENOSPC;
	else {
		a25_acc_queue[a25_acc_head] = *event;
		a25_acc_head = (a25_acc_head + 1) % ARRAY_SIZE(a25_acc_queue);
		a25_acc_count++;
	}
	mutex_unlock(&a25_acc_queue_lock);
	wake_up_interruptible(&a25_acc_wait);
	return ret;
}
static int a25_acc_open(struct inode *inode, struct file *file)
{
	int ret;

	mutex_lock(&a25_acc_queue_lock);
	if (!a25_acc_live) {
		mutex_unlock(&a25_acc_queue_lock);
		return -ENODEV;
	}
	ret = nonseekable_open(inode, file);
	if (!ret) {
		a25_acc_epoch->refs++;
		file->private_data = a25_acc_epoch;
	}
	mutex_unlock(&a25_acc_queue_lock);
	return ret;
}
static int a25_acc_release(struct inode *inode, struct file *file)
{
	mutex_lock(&a25_acc_queue_lock);
	a25_read_epoch_put(file->private_data);
	mutex_unlock(&a25_acc_queue_lock);
	return 0;
}
static ssize_t a25_acc_read(struct file *file, char __user *buf,
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
	mutex_lock(&a25_acc_queue_lock);
	if (!a25_acc_live || epoch != a25_acc_epoch) {
		done = -ENODEV;
		goto out;
	}
	while (a25_acc_count && count - done >= sizeof(snapshot)) {
		snapshot = a25_acc_queue[a25_acc_tail];
		mutex_unlock(&a25_acc_queue_lock);
		fault = copy_to_user(buf + done, &snapshot, sizeof(snapshot));
		mutex_lock(&a25_acc_queue_lock);
		if (fault) {
			if (!done)
				done = -EFAULT;
			break;
		}
		done += sizeof(snapshot);
		/* A completed old snapshot can be returned, but never committed
		 * to a replacement ring. EFAULT never commits a reserved item.
		 */
		if (!a25_acc_live || epoch != a25_acc_epoch)
			break;
		a25_acc_tail = (a25_acc_tail + 1) % ARRAY_SIZE(a25_acc_queue);
		a25_acc_count--;
	}
out:
	mutex_unlock(&a25_acc_queue_lock);
	mutex_unlock(&epoch->readers);
	return done;
}
static unsigned int a25_acc_poll(struct file *file, poll_table *wait)
{
	unsigned int mask = 0;

	poll_wait(file, &a25_acc_wait, wait);
	mutex_lock(&a25_acc_queue_lock);
	if (!a25_acc_live || file->private_data != a25_acc_epoch)
		mask = POLLHUP | POLLERR;
	else if (a25_acc_count)
		mask = POLLIN | POLLRDNORM;
	mutex_unlock(&a25_acc_queue_lock);
	return mask;
}
static const struct file_operations a25_acc_fops = {
	.owner = THIS_MODULE, .open = a25_acc_open, .release = a25_acc_release,
	.read = a25_acc_read, .poll = a25_acc_poll, .llseek = no_llseek,
};
static void acc_a25_reset(struct acc_context *cxt)
{
	/* Before publication, or after kernfs drained every old store. */
	mutex_lock(&cxt->acc_op_mutex);
	cxt->is_active_data = false;
	cxt->is_active_nodata = false;
	cxt->enable = cxt->power = 0;
	cxt->is_first_data_after_enable = false;
	cxt->is_polling_run = false;
	cxt->is_batch_enable = false;
	cxt->delay_ns = 20000000;
	cxt->latency_ns = 0;
	mutex_unlock(&cxt->acc_op_mutex);
}
static int acc_register_a25_path(struct acc_control_path *ctl)
{
	static const struct attribute_group *groups[] = {
		&acc_attribute_group, NULL
	};
	struct acc_context *cxt = acc_context_obj;
	int ret;
	struct a25_read_epoch *epoch;

	if (!cxt || !ctl->enable_nodata || !ctl->open_report_data ||
	    !ctl->batch || !ctl->flush ||
	    !ctl->is_report_input_direct || a25_acc_registered)
		return -EINVAL;
	epoch = a25_read_epoch_new();
	if (!epoch)
		return -ENOMEM;
	acc_a25_reset(cxt);
	cxt->acc_ctl = *ctl;
	cxt->mdev.minor = ID_ACCELEROMETER;
	cxt->mdev.name = ACC_MISC_DEV_NAME;
	cxt->mdev.fops = &a25_acc_fops;
	mutex_lock(&a25_acc_queue_lock);
	a25_acc_epoch = epoch;
	a25_acc_generation++;
	a25_acc_epoch->generation = a25_acc_generation;
	a25_acc_head = a25_acc_tail = a25_acc_count = 0;
	a25_acc_live = true;
	mutex_unlock(&a25_acc_queue_lock);
	ret = sensor_attr_register_owned(&cxt->mdev, groups);
	if (!ret)
		a25_acc_registered = true;
	else {
		mutex_lock(&a25_acc_queue_lock);
		a25_acc_live = false;
		a25_read_epoch_put(a25_acc_epoch);
		a25_acc_epoch = NULL;
		mutex_unlock(&a25_acc_queue_lock);
	}
	return ret;
}
void acc_unregister_a25_path(void)
{
	struct acc_context *cxt = acc_context_obj;

	if (!cxt || !a25_acc_registered)
		return;
	sysfs_remove_group(&cxt->mdev.this_device->kobj, &acc_attribute_group);
	acc_a25_reset(cxt);
	mutex_lock(&a25_acc_queue_lock);
	a25_acc_live = false;
	a25_read_epoch_put(a25_acc_epoch);
	a25_acc_epoch = NULL;
	a25_acc_count = 0;
	mutex_unlock(&a25_acc_queue_lock);
	wake_up_interruptible(&a25_acc_wait);
	sensor_attr_deregister_owned(&cxt->mdev);
	a25_acc_registered = false;
	/* Keep static driver callbacks: they return ENODEV after producer gone.
	 * Keep owner flag so framework exit does not double-deregister.
	 */
}
