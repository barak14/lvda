// SPDX-License-Identifier: GPL-2.0
/*
 * lvda module entry: /dev/lvda miscdevice, ioctl dispatch, and per-fd
 * monitor ownership. The DRM card is persistent (registered at module load);
 * a monitor lives exactly as long as the /dev/lvda file that added it
 * (lvda-SPEC.md §5.5, §8).
 */

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/err.h>
#include <linux/minmax.h>

#include "lvda.h"

unsigned int lvda_max_monitors = 1;
module_param(lvda_max_monitors, uint, 0444);
MODULE_PARM_DESC(lvda_max_monitors,
		 "virtual monitors the card exposes (1..32, default 1; raise only for multiple simultaneous streaming clients)");

/* DRM parent for the persistent card; outlives it (§8). */
static struct platform_device *lvda_parent;

static int lvda_release(struct inode *inode, struct file *file)
{
	lvda_release_owner(file);
	return 0;
}

static long lvda_ioctl_add(struct file *file, unsigned long arg)
{
	struct lvda_add req;
	__u64 generation;
	long ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	ret = lvda_monitor_add(file, &req, &req.monitor_id, &generation,
				&req.drm_card_minor, req.connector_name);
	if (ret)
		return ret;

	if (copy_to_user((void __user *)arg, &req, sizeof(req))) {
		lvda_monitor_abort_add(file, req.monitor_id, generation);
		return -EFAULT;
	}

	return 0;
}

static long lvda_ioctl_remove(struct file *file, unsigned long arg)
{
	struct lvda_remove req;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (req.reserved)
		return -EINVAL;

	return lvda_monitor_remove(file, req.monitor_id);
}

static long lvda_ioctl_version(unsigned long arg)
{
	struct lvda_version ver = {
		.major = LVDA_PROTOCOL_MAJOR,
		.minor = LVDA_PROTOCOL_MINOR,
		.patch = LVDA_PROTOCOL_PATCH,
		.flags = LVDA_F_ALL,
	};

	if (copy_to_user((void __user *)arg, &ver, sizeof(ver)))
		return -EFAULT;

	return 0;
}

static long lvda_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case LVDA_IOC_ADD:
		return lvda_ioctl_add(file, arg);
	case LVDA_IOC_REMOVE:
		return lvda_ioctl_remove(file, arg);
	case LVDA_IOC_VERSION:
		return lvda_ioctl_version(arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations lvda_misc_fops = {
	.owner		= THIS_MODULE,
	.release	= lvda_release,
	.unlocked_ioctl	= lvda_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static struct miscdevice lvda_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "lvda",
	.fops	= &lvda_misc_fops,
	.mode	= 0660,
};

static int __init lvda_init(void)
{
	int ret;

	lvda_max_monitors = clamp(lvda_max_monitors, 1u, LVDA_MAX_MONITORS);

	lvda_parent = platform_device_register_simple("lvda", -1, NULL, 0);
	if (IS_ERR(lvda_parent))
		return PTR_ERR(lvda_parent);

	ret = lvda_card_register(&lvda_parent->dev, lvda_max_monitors);
	if (ret)
		goto err_parent;

	ret = misc_register(&lvda_misc);
	if (ret)
		goto err_card;

	return 0;

err_card:
	lvda_card_unregister();
err_parent:
	platform_device_unregister(lvda_parent);
	return ret;
}

static void __exit lvda_exit(void)
{
	misc_deregister(&lvda_misc);
	lvda_card_unregister();
	platform_device_unregister(lvda_parent);
}

module_init(lvda_init);
module_exit(lvda_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("lvda virtual display driver (persistent card + ioctl)");
MODULE_AUTHOR("lvda authors");
