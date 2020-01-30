/*
 * linux/kernel/power/user.c
 *
 * This file provides the user space interface for software suspend/resume.
 *
 * Copyright (C) 2006 Rafael J. Wysocki <rjw@sisk.pl>
 *
 * This file is released under the GPLv2.
 *
 */

#include <linux/suspend.h>
#include <linux/syscalls.h>
#include <linux/reboot.h>
#include <linux/string.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/pm.h>
#include <linux/fs.h>
#include <linux/console.h>
#include <linux/cpu.h>
#include <linux/freezer.h>

#include <asm/uaccess.h>

#include "power.h"

#define SNAPSHOT_MINOR	231

static struct snapshot_data {
	struct snapshot_handle handle;
	int swap;
	int mode;
	char frozen;
	char ready;
	char platform_suspend;
} snapshot_state;

atomic_t snapshot_device_available = ATOMIC_INIT(1);

static int snapshot_open(struct inode *inode, struct file *filp)
{
	struct snapshot_data *data;

	if (!atomic_add_unless(&snapshot_device_available, -1, 0))
		return -EBUSY;

	if ((filp->f_flags & O_ACCMODE) == O_RDWR) {
		atomic_inc(&snapshot_device_available);
		return -ENOSYS;
	}
	if(create_basic_memory_bitmaps()) {
		atomic_inc(&snapshot_device_available);
		return -ENOMEM;
	}
	nonseekable_open(inode, filp);
	data = &snapshot_state;
	filp->private_data = data;
	memset(&data->handle, 0, sizeof(struct snapshot_handle));
	if ((filp->f_flags & O_ACCMODE) == O_RDONLY) {
		data->swap = swsusp_resume_device ?
			swap_type_of(swsusp_resume_device, 0, NULL) : -1;
		data->mode = O_RDONLY;
	} else {
		data->swap = -1;
		data->mode = O_WRONLY;
	}
	data->frozen = 0;
	data->ready = 0;
	data->platform_suspend = 0;

	return 0;
}

static int snapshot_release(struct inode *inode, struct file *filp)
{
	struct snapshot_data *data;

	swsusp_free();
	free_basic_memory_bitmaps();
	data = filp->private_data;
	free_all_swap_pages(data->swap);
	if (data->frozen) {
		mutex_lock(&pm_mutex);
		thaw_processes();
		mutex_unlock(&pm_mutex);
	}
	atomic_inc(&snapshot_device_available);
	return 0;
}

static ssize_t snapshot_read(struct file *filp, char __user *buf,
                             size_t count, loff_t *offp)
{
	struct snapshot_data *data;
	ssize_t res;

	data = filp->private_data;
	if (!data->ready)
		return -ENODATA;
	res = snapshot_read_next(&data->handle, count);
	if (res > 0) {
		if (copy_to_user(buf, data_of(data->handle), res))
			res = -EFAULT;
		else
			*offp = data->handle.offset;
	}
	return res;
}

static ssize_t snapshot_write(struct file *filp, const char __user *buf,
                              size_t count, loff_t *offp)
{
	struct snapshot_data *data;
	ssize_t res;

	data = filp->private_data;
	res = snapshot_write_next(&data->handle, count);
	if (res > 0) {
		if (copy_from_user(data_of(data->handle), buf, res))
			res = -EFAULT;
		else
			*offp = data->handle.offset;
	}
	return res;
}

static int snapshot_ioctl(struct inode *inode, struct file *filp,
                          unsigned int cmd, unsigned long arg)
{
	int error = 0;
	struct snapshot_data *data;
	loff_t avail;
	sector_t offset;

	if (_IOC_TYPE(cmd) != SNAPSHOT_IOC_MAGIC)
		return -ENOTTY;
	if (_IOC_NR(cmd) > SNAPSHOT_IOC_MAXNR)
		return -ENOTTY;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	data = filp->private_data;

	switch (cmd) {

	case SNAPSHOT_FREEZE:
		if (data->frozen)
			break;
		mutex_lock(&pm_mutex);
		error = pm_notifier_call_chain(PM_HIBERNATION_PREPARE);
		if (!error) {
			error = freeze_processes();
			if (error)
				thaw_processes();
		}
		if (error)
			pm_notifier_call_chain(PM_POST_HIBERNATION);
		mutex_unlock(&pm_mutex);
		if (!error)
			data->frozen = 1;
		break;

	case SNAPSHOT_UNFREEZE:
		if (!data->frozen || data->ready)
			break;
		mutex_lock(&pm_mutex);
		thaw_processes();
		pm_notifier_call_chain(PM_POST_HIBERNATION);
		mutex_unlock(&pm_mutex);
		data->frozen = 0;
		break;

	case SNAPSHOT_ATOMIC_SNAPSHOT:
		if (data->mode != O_RDONLY || !data->frozen  || data->ready) {
			error = -EPERM;
			break;
		}
		error = hibernation_snapshot(data->platform_suspend);
		if (!error)
			error = put_user(in_suspend, (unsigned int __user *)arg);
		if (!error)
			data->ready = 1;
		break;

	case SNAPSHOT_ATOMIC_RESTORE:
		snapshot_write_finalize(&data->handle);
		if (data->mode != O_WRONLY || !data->frozen ||
		    !snapshot_image_loaded(&data->handle)) {
			error = -EPERM;
			break;
		}
		error = hibernation_restore(data->platform_suspend);
		break;

	case SNAPSHOT_FREE:
		swsusp_free();
		memset(&data->handle, 0, sizeof(struct snapshot_handle));
		data->ready = 0;
		break;

	case SNAPSHOT_SET_IMAGE_SIZE:
		image_size = arg;
		break;

	case SNAPSHOT_AVAIL_SWAP:
		avail = count_swap_pages(data->swap, 1);
		avail <<= PAGE_SHIFT;
		error = put_user(avail, (loff_t __user *)arg);
		break;

	case SNAPSHOT_GET_SWAP_PAGE:
		if (data->swap < 0 || data->swap >= MAX_SWAPFILES) {
			error = -ENODEV;
			break;
		}
		offset = alloc_swapdev_block(data->swap);
		if (offset) {
			offset <<= PAGE_SHIFT;
			error = put_user(offset, (sector_t __user *)arg);
		} else {
			error = -ENOSPC;
		}
		break;

	case SNAPSHOT_FREE_SWAP_PAGES:
		if (data->swap < 0 || data->swap >= MAX_SWAPFILES) {
			error = -ENODEV;
			break;
		}
		free_all_swap_pages(data->swap);
		break;

	case SNAPSHOT_SET_SWAP_FILE:
		if (!swsusp_swap_in_use()) {
			/*
			 * User space encodes device types as two-byte values,
			 * so we need to recode them
			 */
			if (old_decode_dev(arg)) {
				data->swap = swap_type_of(old_decode_dev(arg),
							0, NULL);
				if (data->swap < 0)
					error = -ENODEV;
			} else {
				data->swap = -1;
				error = -EINVAL;
			}
		} else {
			error = -EPERM;
		}
		break;

	case SNAPSHOT_S2RAM:
		if (!data->frozen) {
			error = -EPERM;
			break;
		}
		if (!mutex_trylock(&pm_mutex)) {
			error = -EBUSY;
			break;
		}
		/*
		 * Tasks are frozen and the notifiers have been called with
		 * PM_HIBERNATION_PREPARE
		 */
		error = suspend_devices_and_enter(PM_SUSPEND_MEM);
		mutex_unlock(&pm_mutex);
		break;

	case SNAPSHOT_PMOPS:
		error = -EINVAL;

		switch (arg) {

		case PMOPS_PREPARE:
			data->platform_suspend = 1;
			error = 0;
			break;

		case PMOPS_ENTER:
			if (data->platform_suspend)
				error = hibernation_platform_enter();

			break;

		case PMOPS_FINISH:
			if (data->platform_suspend)
				error = 0;

			break;

		default:
			printk(KERN_ERR "SNAPSHOT_PMOPS: invalid argument %ld\n", arg);

		}
		break;

	case SNAPSHOT_SET_SWAP_AREA:
		if (swsusp_swap_in_use()) {
			error = -EPERM;
		} else {
			struct resume_swap_area swap_area;
			dev_t swdev;

			error = copy_from_user(&swap_area, (void __user *)arg,
					sizeof(struct resume_swap_area));
			if (error) {
				error = -EFAULT;
				break;
			}

			/*
			 * User space encodes device types as two-byte values,
			 * so we need to recode them
			 */
			swdev = old_decode_dev(swap_area.dev);
			if (swdev) {
				offset = swap_area.offset;
				data->swap = swap_type_of(swdev, offset, NULL);
				if (data->swap < 0)
					error = -ENODEV;
			} else {
				data->swap = -1;
				error = -EINVAL;
			}
		}
		break;

	default:
		error = -ENOTTY;

	}

	return error;
}

static const struct file_operations snapshot_fops = {
	.open = snapshot_open,
	.release = snapshot_release,
	.read = snapshot_read,
	.write = snapshot_write,
	.llseek = no_llseek,
	.ioctl = snapshot_ioctl,
};

static struct miscdevice snapshot_device = {
	.minor = SNAPSHOT_MINOR,
	.name = "snapshot",
	.fops = &snapshot_fops,
};

static int __init snapshot_device_init(void)
{
	return misc_register(&snapshot_device);
};

device_initcall(snapshot_device_init);
