/* SPDX-License-Identifier: GPL-2.0 */
/* A25 virtual build owns /dev/gsensor; other drivers keep native factory.
 * Epoch validation and hardware/cache action are one mir_lock transaction.
 * Both directions of uaccess are outside that lock. A late old input copy
 * can only return ENODEV; a late output copy contains an old stack snapshot.
 */
static u64 mir_factory_generation;
static bool mir_factory_live;
static int mir_factory_open(struct inode *inode, struct file *file)
{
	u64 *generation = kmalloc(sizeof(*generation), GFP_KERNEL);
	int ret;

	if (!generation)
		return -ENOMEM;
	mutex_lock(&mir_lock);
	ret = mir_available();
	if (!ret && !mir_factory_live)
		ret = -ENODEV;
	if (!ret) {
		*generation = mir_factory_generation;
		ret = nonseekable_open(inode, file);
		if (!ret)
			file->private_data = generation;
	}
	mutex_unlock(&mir_lock);
	if (ret)
		kfree(generation);
	return ret;
}
static int mir_factory_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	return 0;
}
static long mir_factory_ioctl(struct file *file, unsigned int cmd,
	unsigned long arg)
{
	void __user *ptr = (void __user *)arg;
	u32 flag = 0;
	short raw[3];
	int data[3], i, ret;
	char output[64];
	bool copy = false;

	if (cmd == GSENSOR_IOCTL_INIT &&
	    copy_from_user(&flag, ptr, sizeof(flag)))
		return -EFAULT;
	mutex_lock(&mir_lock);
	if (!mir_factory_live || !file->private_data ||
	    *(u64 *)file->private_data != mir_factory_generation) {
		ret = -ENODEV;
		goto out;
	}
	ret = mir_available();
	if (ret)
		goto out;
	switch (cmd) {
	case GSENSOR_IOCTL_INIT:
		ret = flag > 1 ? -EINVAL : mir_factory_enable_locked(flag, 5);
		break;
	case GSENSOR_IOCTL_READ_SENSORDATA:
		if (!mir->cache_valid) {
			ret = -EAGAIN;
			break;
		}
		for (i = 0; i < 3; i++)
			data[i] = mir->cache[i];
		copy = true;
		break;
	case GSENSOR_IOCTL_READ_RAW_DATA:
		ret = mir_read_sample(raw);
		if (!ret) {
			for (i = 0; i < 3; i++)
				data[i] = raw[i];
			copy = true;
		}
		break;
	case GSENSOR_IOCTL_READ_CHIPINFO:
	case GSENSOR_IOCTL_SET_CALI:
	case GSENSOR_IOCTL_CLR_CALI:
	case GSENSOR_IOCTL_GET_CALI:
	case GSENSOR_IOCTL_ENABLE_CALI:
	case GSENSOR_IOCTL_SELF_TEST:
		ret = -EOPNOTSUPP;
		break;
	default:
		ret = -ENOIOCTLCMD;
	}
out:
	mutex_unlock(&mir_lock);
	if (!ret && copy) {
		/* Native factory wire representation, including sign as hex. */
		snprintf(output, sizeof(output), "%x %x %x", data[0], data[1], data[2]);
		if (copy_to_user(ptr, output, strlen(output) + 1))
			ret = -EFAULT;
	}
	return ret;
}
#ifdef CONFIG_COMPAT
static long mir_factory_compat_ioctl(struct file *file, unsigned int cmd,
	unsigned long arg)
{
	/* These native MTK commands encode fixed-width payloads in both ABIs. */
	return mir_factory_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif
static const struct file_operations mir_factory_fops = {
	.owner = THIS_MODULE,
	.open = mir_factory_open, .release = mir_factory_release,
	.unlocked_ioctl = mir_factory_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = mir_factory_compat_ioctl,
#endif
	.llseek = no_llseek,
};
static struct miscdevice mir_factory_device = {
	.minor = MISC_DYNAMIC_MINOR, .name = "gsensor", .fops = &mir_factory_fops,
};
static int mir_factory_register(void)
{
	int ret;

	mutex_lock(&mir_lock);
	mir_factory_generation++;
	mir_factory_live = true;
	mutex_unlock(&mir_lock);
	ret = misc_register(&mir_factory_device);
	if (ret) {
		mutex_lock(&mir_lock);
		mir_factory_live = false;
		mutex_unlock(&mir_lock);
	}
	return ret;
}
static void mir_factory_unregister(void)
{
	mutex_lock(&mir_lock);
	mir_factory_live = false;
	mutex_unlock(&mir_lock);
	misc_deregister(&mir_factory_device);
}
