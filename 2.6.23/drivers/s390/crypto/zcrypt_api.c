/*
 *  linux/drivers/s390/crypto/zcrypt_api.c
 *
 *  zcrypt 2.1.0
 *
 *  Copyright (C)  2001, 2006 IBM Corporation
 *  Author(s): Robert Burroughs
 *	       Eric Rossman (edrossma@us.ibm.com)
 *	       Cornelia Huck <cornelia.huck@de.ibm.com>
 *
 *  Hotplug & misc device support: Jochen Roehrig (roehrig@de.ibm.com)
 *  Major cleanup & driver split: Martin Schwidefsky <schwidefsky@de.ibm.com>
 *				  Ralph Wuerthner <rwuerthn@de.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/compat.h>
#include <asm/atomic.h>
#include <asm/uaccess.h>

#include "zcrypt_api.h"

/**
 * Module description.
 */
MODULE_AUTHOR("IBM Corporation");
MODULE_DESCRIPTION("Cryptographic Coprocessor interface, "
		   "Copyright 2001, 2006 IBM Corporation");
MODULE_LICENSE("GPL");

static DEFINE_SPINLOCK(zcrypt_device_lock);
static LIST_HEAD(zcrypt_device_list);
static int zcrypt_device_count = 0;
static atomic_t zcrypt_open_count = ATOMIC_INIT(0);

/**
 * Device attributes common for all crypto devices.
 */
static ssize_t zcrypt_type_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct zcrypt_device *zdev = to_ap_dev(dev)->private;
	return snprintf(buf, PAGE_SIZE, "%s\n", zdev->type_string);
}

static DEVICE_ATTR(type, 0444, zcrypt_type_show, NULL);

static ssize_t zcrypt_online_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct zcrypt_device *zdev = to_ap_dev(dev)->private;
	return snprintf(buf, PAGE_SIZE, "%d\n", zdev->online);
}

static ssize_t zcrypt_online_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct zcrypt_device *zdev = to_ap_dev(dev)->private;
	int online;

	if (sscanf(buf, "%d\n", &online) != 1 || online < 0 || online > 1)
		return -EINVAL;
	zdev->online = online;
	if (!online)
		ap_flush_queue(zdev->ap_dev);
	return count;
}

static DEVICE_ATTR(online, 0644, zcrypt_online_show, zcrypt_online_store);

static struct attribute * zcrypt_device_attrs[] = {
	&dev_attr_type.attr,
	&dev_attr_online.attr,
	NULL,
};

static struct attribute_group zcrypt_device_attr_group = {
	.attrs = zcrypt_device_attrs,
};

/**
 * Move the device towards the head of the device list.
 * Need to be called while holding the zcrypt device list lock.
 * Note: cards with speed_rating of 0 are kept at the end of the list.
 */
static void __zcrypt_increase_preference(struct zcrypt_device *zdev)
{
	struct zcrypt_device *tmp;
	struct list_head *l;

	if (zdev->speed_rating == 0)
		return;
	for (l = zdev->list.prev; l != &zcrypt_device_list; l = l->prev) {
		tmp = list_entry(l, struct zcrypt_device, list);
		if ((tmp->request_count + 1) * tmp->speed_rating <=
		    (zdev->request_count + 1) * zdev->speed_rating &&
		    tmp->speed_rating != 0)
			break;
	}
	if (l == zdev->list.prev)
		return;
	/* Move zdev behind l */
	list_del(&zdev->list);
	list_add(&zdev->list, l);
}

/**
 * Move the device towards the tail of the device list.
 * Need to be called while holding the zcrypt device list lock.
 * Note: cards with speed_rating of 0 are kept at the end of the list.
 */
static void __zcrypt_decrease_preference(struct zcrypt_device *zdev)
{
	struct zcrypt_device *tmp;
	struct list_head *l;

	if (zdev->speed_rating == 0)
		return;
	for (l = zdev->list.next; l != &zcrypt_device_list; l = l->next) {
		tmp = list_entry(l, struct zcrypt_device, list);
		if ((tmp->request_count + 1) * tmp->speed_rating >
		    (zdev->request_count + 1) * zdev->speed_rating ||
		    tmp->speed_rating == 0)
			break;
	}
	if (l == zdev->list.next)
		return;
	/* Move zdev before l */
	list_del(&zdev->list);
	list_add_tail(&zdev->list, l);
}

static void zcrypt_device_release(struct kref *kref)
{
	struct zcrypt_device *zdev =
		container_of(kref, struct zcrypt_device, refcount);
	zcrypt_device_free(zdev);
}

void zcrypt_device_get(struct zcrypt_device *zdev)
{
	kref_get(&zdev->refcount);
}
EXPORT_SYMBOL(zcrypt_device_get);

int zcrypt_device_put(struct zcrypt_device *zdev)
{
	return kref_put(&zdev->refcount, zcrypt_device_release);
}
EXPORT_SYMBOL(zcrypt_device_put);

struct zcrypt_device *zcrypt_device_alloc(size_t max_response_size)
{
	struct zcrypt_device *zdev;

	zdev = kzalloc(sizeof(struct zcrypt_device), GFP_KERNEL);
	if (!zdev)
		return NULL;
	zdev->reply.message = kmalloc(max_response_size, GFP_KERNEL);
	if (!zdev->reply.message)
		goto out_free;
	zdev->reply.length = max_response_size;
	spin_lock_init(&zdev->lock);
	INIT_LIST_HEAD(&zdev->list);
	return zdev;

out_free:
	kfree(zdev);
	return NULL;
}
EXPORT_SYMBOL(zcrypt_device_alloc);

void zcrypt_device_free(struct zcrypt_device *zdev)
{
	kfree(zdev->reply.message);
	kfree(zdev);
}
EXPORT_SYMBOL(zcrypt_device_free);

/**
 * Register a crypto device.
 */
int zcrypt_device_register(struct zcrypt_device *zdev)
{
	int rc;

	rc = sysfs_create_group(&zdev->ap_dev->device.kobj,
				&zcrypt_device_attr_group);
	if (rc)
		goto out;
	get_device(&zdev->ap_dev->device);
	kref_init(&zdev->refcount);
	spin_lock_bh(&zcrypt_device_lock);
	zdev->online = 1;	/* New devices are online by default. */
	list_add_tail(&zdev->list, &zcrypt_device_list);
	__zcrypt_increase_preference(zdev);
	zcrypt_device_count++;
	spin_unlock_bh(&zcrypt_device_lock);
out:
	return rc;
}
EXPORT_SYMBOL(zcrypt_device_register);

/**
 * Unregister a crypto device.
 */
void zcrypt_device_unregister(struct zcrypt_device *zdev)
{
	spin_lock_bh(&zcrypt_device_lock);
	zcrypt_device_count--;
	list_del_init(&zdev->list);
	spin_unlock_bh(&zcrypt_device_lock);
	sysfs_remove_group(&zdev->ap_dev->device.kobj,
			   &zcrypt_device_attr_group);
	put_device(&zdev->ap_dev->device);
	zcrypt_device_put(zdev);
}
EXPORT_SYMBOL(zcrypt_device_unregister);

/**
 * zcrypt_read is not be supported beyond zcrypt 1.3.1
 */
static ssize_t zcrypt_read(struct file *filp, char __user *buf,
			   size_t count, loff_t *f_pos)
{
	return -EPERM;
}

/**
 * Write is is not allowed
 */
static ssize_t zcrypt_write(struct file *filp, const char __user *buf,
			    size_t count, loff_t *f_pos)
{
	return -EPERM;
}

/**
 * Device open/close functions to count number of users.
 */
static int zcrypt_open(struct inode *inode, struct file *filp)
{
	atomic_inc(&zcrypt_open_count);
	return 0;
}

static int zcrypt_release(struct inode *inode, struct file *filp)
{
	atomic_dec(&zcrypt_open_count);
	return 0;
}

/**
 * zcrypt ioctls.
 */
static long zcrypt_rsa_modexpo(struct ica_rsa_modexpo *mex)
{
	struct zcrypt_device *zdev;
	int rc;

	if (mex->outputdatalength < mex->inputdatalength)
		return -EINVAL;
	/**
	 * As long as outputdatalength is big enough, we can set the
	 * outputdatalength equal to the inputdatalength, since that is the
	 * number of bytes we will copy in any case
	 */
	mex->outputdatalength = mex->inputdatalength;

	spin_lock_bh(&zcrypt_device_lock);
	list_for_each_entry(zdev, &zcrypt_device_list, list) {
		if (!zdev->online ||
		    !zdev->ops->rsa_modexpo ||
		    zdev->min_mod_size > mex->inputdatalength ||
		    zdev->max_mod_size < mex->inputdatalength)
			continue;
		zcrypt_device_get(zdev);
		get_device(&zdev->ap_dev->device);
		zdev->request_count++;
		__zcrypt_decrease_preference(zdev);
		if (try_module_get(zdev->ap_dev->drv->driver.owner)) {
			spin_unlock_bh(&zcrypt_device_lock);
			rc = zdev->ops->rsa_modexpo(zdev, mex);
			spin_lock_bh(&zcrypt_device_lock);
			module_put(zdev->ap_dev->drv->driver.owner);
		}
		else
			rc = -EAGAIN;
		zdev->request_count--;
		__zcrypt_increase_preference(zdev);
		put_device(&zdev->ap_dev->device);
		zcrypt_device_put(zdev);
		spin_unlock_bh(&zcrypt_device_lock);
		return rc;
	}
	spin_unlock_bh(&zcrypt_device_lock);
	return -ENODEV;
}

static long zcrypt_rsa_crt(struct ica_rsa_modexpo_crt *crt)
{
	struct zcrypt_device *zdev;
	unsigned long long z1, z2, z3;
	int rc, copied;

	if (crt->outputdatalength < crt->inputdatalength ||
	    (crt->inputdatalength & 1))
		return -EINVAL;
	/**
	 * As long as outputdatalength is big enough, we can set the
	 * outputdatalength equal to the inputdatalength, since that is the
	 * number of bytes we will copy in any case
	 */
	crt->outputdatalength = crt->inputdatalength;

	copied = 0;
 restart:
	spin_lock_bh(&zcrypt_device_lock);
	list_for_each_entry(zdev, &zcrypt_device_list, list) {
		if (!zdev->online ||
		    !zdev->ops->rsa_modexpo_crt ||
		    zdev->min_mod_size > crt->inputdatalength ||
		    zdev->max_mod_size < crt->inputdatalength)
			continue;
		if (zdev->short_crt && crt->inputdatalength > 240) {
			/**
			 * Check inputdata for leading zeros for cards
			 * that can't handle np_prime, bp_key, or
			 * u_mult_inv > 128 bytes.
			 */
			if (copied == 0) {
				int len;
				spin_unlock_bh(&zcrypt_device_lock);
				/* len is max 256 / 2 - 120 = 8 */
				len = crt->inputdatalength / 2 - 120;
				z1 = z2 = z3 = 0;
				if (copy_from_user(&z1, crt->np_prime, len) ||
				    copy_from_user(&z2, crt->bp_key, len) ||
				    copy_from_user(&z3, crt->u_mult_inv, len))
					return -EFAULT;
				copied = 1;
				/**
				 * We have to restart device lookup -
				 * the device list may have changed by now.
				 */
				goto restart;
			}
			if (z1 != 0ULL || z2 != 0ULL || z3 != 0ULL)
				/* The device can't handle this request. */
				continue;
		}
		zcrypt_device_get(zdev);
		get_device(&zdev->ap_dev->device);
		zdev->request_count++;
		__zcrypt_decrease_preference(zdev);
		if (try_module_get(zdev->ap_dev->drv->driver.owner)) {
			spin_unlock_bh(&zcrypt_device_lock);
			rc = zdev->ops->rsa_modexpo_crt(zdev, crt);
			spin_lock_bh(&zcrypt_device_lock);
			module_put(zdev->ap_dev->drv->driver.owner);
		}
		else
			rc = -EAGAIN;
		zdev->request_count--;
		__zcrypt_increase_preference(zdev);
		put_device(&zdev->ap_dev->device);
		zcrypt_device_put(zdev);
		spin_unlock_bh(&zcrypt_device_lock);
		return rc;
	}
	spin_unlock_bh(&zcrypt_device_lock);
	return -ENODEV;
}

static long zcrypt_send_cprb(struct ica_xcRB *xcRB)
{
	struct zcrypt_device *zdev;
	int rc;

	spin_lock_bh(&zcrypt_device_lock);
	list_for_each_entry(zdev, &zcrypt_device_list, list) {
		if (!zdev->online || !zdev->ops->send_cprb ||
		    (xcRB->user_defined != AUTOSELECT &&
			AP_QID_DEVICE(zdev->ap_dev->qid) != xcRB->user_defined)
		    )
			continue;
		zcrypt_device_get(zdev);
		get_device(&zdev->ap_dev->device);
		zdev->request_count++;
		__zcrypt_decrease_preference(zdev);
		if (try_module_get(zdev->ap_dev->drv->driver.owner)) {
			spin_unlock_bh(&zcrypt_device_lock);
			rc = zdev->ops->send_cprb(zdev, xcRB);
			spin_lock_bh(&zcrypt_device_lock);
			module_put(zdev->ap_dev->drv->driver.owner);
		}
		else
			rc = -EAGAIN;
		zdev->request_count--;
		__zcrypt_increase_preference(zdev);
		put_device(&zdev->ap_dev->device);
		zcrypt_device_put(zdev);
		spin_unlock_bh(&zcrypt_device_lock);
		return rc;
	}
	spin_unlock_bh(&zcrypt_device_lock);
	return -ENODEV;
}

static void zcrypt_status_mask(char status[AP_DEVICES])
{
	struct zcrypt_device *zdev;

	memset(status, 0, sizeof(char) * AP_DEVICES);
	spin_lock_bh(&zcrypt_device_lock);
	list_for_each_entry(zdev, &zcrypt_device_list, list)
		status[AP_QID_DEVICE(zdev->ap_dev->qid)] =
			zdev->online ? zdev->user_space_type : 0x0d;
	spin_unlock_bh(&zcrypt_device_lock);
}

static void zcrypt_qdepth_mask(char qdepth[AP_DEVICES])
{
	struct zcrypt_device *zdev;

	memset(qdepth, 0, sizeof(char)	* AP_DEVICES);
	spin_lock_bh(&zcrypt_device_lock);
	list_for_each_entry(zdev, &zcrypt_device_list, list) {
		spin_lock(&zdev->ap_dev->lock);
		qdepth[AP_QID_DEVICE(zdev->ap_dev->qid)] =
			zdev->ap_dev->pendingq_count +
			zdev->ap_dev->requestq_count;
		spin_unlock(&zdev->ap_dev->lock);
	}
	spin_unlock_bh(&zcrypt_device_lock);
}

static void zcrypt_perdev_reqcnt(int reqcnt[AP_DEVICES])
{
	struct zcrypt_device *zdev;

	memset(reqcnt, 0, sizeof(int) * AP_DEVICES);
	spin_lock_bh(&zcrypt_device_lock);
	list_for_each_entry(zdev, &zcrypt_device_list, list) {
		spin_lock(&zdev->ap_dev->lock);
		reqcnt[AP_QID_DEVICE(zdev->ap_dev->qid)] =
			zdev->ap_dev->total_request_count;
		spin_unlock(&zdev->ap_dev->lock);
	}
	spin_unlock_bh(&zcrypt_device_lock);
}

static int zcrypt_pendingq_count(void)
{
	struct zcrypt_device *zdev;
	int pendingq_count = 0;

	spin_lock_bh(&zcrypt_device_lock);
	list_for_each_entry(zdev, &zcrypt_device_list, list) {
		spin_lock(&zdev->ap_dev->lock);
		pendingq_count += zdev->ap_dev->pendingq_count;
		spin_unlock(&zdev->ap_dev->lock);
	}
	spin_unlock_bh(&zcrypt_device_lock);
	return pendingq_count;
}

static int zcrypt_requestq_count(void)
{
	struct zcrypt_device *zdev;
	int requestq_count = 0;

	spin_lock_bh(&zcrypt_device_lock);
	list_for_each_entry(zdev, &zcrypt_device_list, list) {
		spin_lock(&zdev->ap_dev->lock);
		requestq_count += zdev->ap_dev->requestq_count;
		spin_unlock(&zdev->ap_dev->lock);
	}
	spin_unlock_bh(&zcrypt_device_lock);
	return requestq_count;
}

static int zcrypt_count_type(int type)
{
	struct zcrypt_device *zdev;
	int device_count = 0;

	spin_lock_bh(&zcrypt_device_lock);
	list_for_each_entry(zdev, &zcrypt_device_list, list)
		if (zdev->user_space_type == type)
			device_count++;
	spin_unlock_bh(&zcrypt_device_lock);
	return device_count;
}

/**
 * Old, deprecated combi status call.
 */
static long zcrypt_ica_status(struct file *filp, unsigned long arg)
{
	struct ica_z90_status *pstat;
	int ret;

	pstat = kzalloc(sizeof(*pstat), GFP_KERNEL);
	if (!pstat)
		return -ENOMEM;
	pstat->totalcount = zcrypt_device_count;
	pstat->leedslitecount = zcrypt_count_type(ZCRYPT_PCICA);
	pstat->leeds2count = zcrypt_count_type(ZCRYPT_PCICC);
	pstat->requestqWaitCount = zcrypt_requestq_count();
	pstat->pendingqWaitCount = zcrypt_pendingq_count();
	pstat->totalOpenCount = atomic_read(&zcrypt_open_count);
	pstat->cryptoDomain = ap_domain_index;
	zcrypt_status_mask(pstat->status);
	zcrypt_qdepth_mask(pstat->qdepth);
	ret = 0;
	if (copy_to_user((void __user *) arg, pstat, sizeof(*pstat)))
		ret = -EFAULT;
	kfree(pstat);
	return ret;
}

static long zcrypt_unlocked_ioctl(struct file *filp, unsigned int cmd,
				  unsigned long arg)
{
	int rc;

	switch (cmd) {
	case ICARSAMODEXPO: {
		struct ica_rsa_modexpo __user *umex = (void __user *) arg;
		struct ica_rsa_modexpo mex;
		if (copy_from_user(&mex, umex, sizeof(mex)))
			return -EFAULT;
		do {
			rc = zcrypt_rsa_modexpo(&mex);
		} while (rc == -EAGAIN);
		if (rc)
			return rc;
		return put_user(mex.outputdatalength, &umex->outputdatalength);
	}
	case ICARSACRT: {
		struct ica_rsa_modexpo_crt __user *ucrt = (void __user *) arg;
		struct ica_rsa_modexpo_crt crt;
		if (copy_from_user(&crt, ucrt, sizeof(crt)))
			return -EFAULT;
		do {
			rc = zcrypt_rsa_crt(&crt);
		} while (rc == -EAGAIN);
		if (rc)
			return rc;
		return put_user(crt.outputdatalength, &ucrt->outputdatalength);
	}
	case ZSECSENDCPRB: {
		struct ica_xcRB __user *uxcRB = (void __user *) arg;
		struct ica_xcRB xcRB;
		if (copy_from_user(&xcRB, uxcRB, sizeof(xcRB)))
			return -EFAULT;
		do {
			rc = zcrypt_send_cprb(&xcRB);
		} while (rc == -EAGAIN);
		if (copy_to_user(uxcRB, &xcRB, sizeof(xcRB)))
			return -EFAULT;
		return rc;
	}
	case Z90STAT_STATUS_MASK: {
		char status[AP_DEVICES];
		zcrypt_status_mask(status);
		if (copy_to_user((char __user *) arg, status,
				 sizeof(char) * AP_DEVICES))
			return -EFAULT;
		return 0;
	}
	case Z90STAT_QDEPTH_MASK: {
		char qdepth[AP_DEVICES];
		zcrypt_qdepth_mask(qdepth);
		if (copy_to_user((char __user *) arg, qdepth,
				 sizeof(char) * AP_DEVICES))
			return -EFAULT;
		return 0;
	}
	case Z90STAT_PERDEV_REQCNT: {
		int reqcnt[AP_DEVICES];
		zcrypt_perdev_reqcnt(reqcnt);
		if (copy_to_user((int __user *) arg, reqcnt,
				 sizeof(int) * AP_DEVICES))
			return -EFAULT;
		return 0;
	}
	case Z90STAT_REQUESTQ_COUNT:
		return put_user(zcrypt_requestq_count(), (int __user *) arg);
	case Z90STAT_PENDINGQ_COUNT:
		return put_user(zcrypt_pendingq_count(), (int __user *) arg);
	case Z90STAT_TOTALOPEN_COUNT:
		return put_user(atomic_read(&zcrypt_open_count),
				(int __user *) arg);
	case Z90STAT_DOMAIN_INDEX:
		return put_user(ap_domain_index, (int __user *) arg);
	/**
	 * Deprecated ioctls. Don't add another device count ioctl,
	 * you can count them yourself in the user space with the
	 * output of the Z90STAT_STATUS_MASK ioctl.
	 */
	case ICAZ90STATUS:
		return zcrypt_ica_status(filp, arg);
	case Z90STAT_TOTALCOUNT:
		return put_user(zcrypt_device_count, (int __user *) arg);
	case Z90STAT_PCICACOUNT:
		return put_user(zcrypt_count_type(ZCRYPT_PCICA),
				(int __user *) arg);
	case Z90STAT_PCICCCOUNT:
		return put_user(zcrypt_count_type(ZCRYPT_PCICC),
				(int __user *) arg);
	case Z90STAT_PCIXCCMCL2COUNT:
		return put_user(zcrypt_count_type(ZCRYPT_PCIXCC_MCL2),
				(int __user *) arg);
	case Z90STAT_PCIXCCMCL3COUNT:
		return put_user(zcrypt_count_type(ZCRYPT_PCIXCC_MCL3),
				(int __user *) arg);
	case Z90STAT_PCIXCCCOUNT:
		return put_user(zcrypt_count_type(ZCRYPT_PCIXCC_MCL2) +
				zcrypt_count_type(ZCRYPT_PCIXCC_MCL3),
				(int __user *) arg);
	case Z90STAT_CEX2CCOUNT:
		return put_user(zcrypt_count_type(ZCRYPT_CEX2C),
				(int __user *) arg);
	case Z90STAT_CEX2ACOUNT:
		return put_user(zcrypt_count_type(ZCRYPT_CEX2A),
				(int __user *) arg);
	default:
		/* unknown ioctl number */
		return -ENOIOCTLCMD;
	}
}

#ifdef CONFIG_COMPAT
/**
 * ioctl32 conversion routines
 */
struct compat_ica_rsa_modexpo {
	compat_uptr_t	inputdata;
	unsigned int	inputdatalength;
	compat_uptr_t	outputdata;
	unsigned int	outputdatalength;
	compat_uptr_t	b_key;
	compat_uptr_t	n_modulus;
};

static long trans_modexpo32(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	struct compat_ica_rsa_modexpo __user *umex32 = compat_ptr(arg);
	struct compat_ica_rsa_modexpo mex32;
	struct ica_rsa_modexpo mex64;
	long rc;

	if (copy_from_user(&mex32, umex32, sizeof(mex32)))
		return -EFAULT;
	mex64.inputdata = compat_ptr(mex32.inputdata);
	mex64.inputdatalength = mex32.inputdatalength;
	mex64.outputdata = compat_ptr(mex32.outputdata);
	mex64.outputdatalength = mex32.outputdatalength;
	mex64.b_key = compat_ptr(mex32.b_key);
	mex64.n_modulus = compat_ptr(mex32.n_modulus);
	do {
		rc = zcrypt_rsa_modexpo(&mex64);
	} while (rc == -EAGAIN);
	if (!rc)
		rc = put_user(mex64.outputdatalength,
			      &umex32->outputdatalength);
	return rc;
}

struct compat_ica_rsa_modexpo_crt {
	compat_uptr_t	inputdata;
	unsigned int	inputdatalength;
	compat_uptr_t	outputdata;
	unsigned int	outputdatalength;
	compat_uptr_t	bp_key;
	compat_uptr_t	bq_key;
	compat_uptr_t	np_prime;
	compat_uptr_t	nq_prime;
	compat_uptr_t	u_mult_inv;
};

static long trans_modexpo_crt32(struct file *filp, unsigned int cmd,
				unsigned long arg)
{
	struct compat_ica_rsa_modexpo_crt __user *ucrt32 = compat_ptr(arg);
	struct compat_ica_rsa_modexpo_crt crt32;
	struct ica_rsa_modexpo_crt crt64;
	long rc;

	if (copy_from_user(&crt32, ucrt32, sizeof(crt32)))
		return -EFAULT;
	crt64.inputdata = compat_ptr(crt32.inputdata);
	crt64.inputdatalength = crt32.inputdatalength;
	crt64.outputdata=  compat_ptr(crt32.outputdata);
	crt64.outputdatalength = crt32.outputdatalength;
	crt64.bp_key = compat_ptr(crt32.bp_key);
	crt64.bq_key = compat_ptr(crt32.bq_key);
	crt64.np_prime = compat_ptr(crt32.np_prime);
	crt64.nq_prime = compat_ptr(crt32.nq_prime);
	crt64.u_mult_inv = compat_ptr(crt32.u_mult_inv);
	do {
		rc = zcrypt_rsa_crt(&crt64);
	} while (rc == -EAGAIN);
	if (!rc)
		rc = put_user(crt64.outputdatalength,
			      &ucrt32->outputdatalength);
	return rc;
}

struct compat_ica_xcRB {
	unsigned short	agent_ID;
	unsigned int	user_defined;
	unsigned short	request_ID;
	unsigned int	request_control_blk_length;
	unsigned char	padding1[16 - sizeof (compat_uptr_t)];
	compat_uptr_t	request_control_blk_addr;
	unsigned int	request_data_length;
	char		padding2[16 - sizeof (compat_uptr_t)];
	compat_uptr_t	request_data_address;
	unsigned int	reply_control_blk_length;
	char		padding3[16 - sizeof (compat_uptr_t)];
	compat_uptr_t	reply_control_blk_addr;
	unsigned int	reply_data_length;
	char		padding4[16 - sizeof (compat_uptr_t)];
	compat_uptr_t	reply_data_addr;
	unsigned short	priority_window;
	unsigned int	status;
} __attribute__((packed));

static long trans_xcRB32(struct file *filp, unsigned int cmd,
			 unsigned long arg)
{
	struct compat_ica_xcRB __user *uxcRB32 = compat_ptr(arg);
	struct compat_ica_xcRB xcRB32;
	struct ica_xcRB xcRB64;
	long rc;

	if (copy_from_user(&xcRB32, uxcRB32, sizeof(xcRB32)))
		return -EFAULT;
	xcRB64.agent_ID = xcRB32.agent_ID;
	xcRB64.user_defined = xcRB32.user_defined;
	xcRB64.request_ID = xcRB32.request_ID;
	xcRB64.request_control_blk_length =
		xcRB32.request_control_blk_length;
	xcRB64.request_control_blk_addr =
		compat_ptr(xcRB32.request_control_blk_addr);
	xcRB64.request_data_length =
		xcRB32.request_data_length;
	xcRB64.request_data_address =
		compat_ptr(xcRB32.request_data_address);
	xcRB64.reply_control_blk_length =
		xcRB32.reply_control_blk_length;
	xcRB64.reply_control_blk_addr =
		compat_ptr(xcRB32.reply_control_blk_addr);
	xcRB64.reply_data_length = xcRB32.reply_data_length;
	xcRB64.reply_data_addr =
		compat_ptr(xcRB32.reply_data_addr);
	xcRB64.priority_window = xcRB32.priority_window;
	xcRB64.status = xcRB32.status;
	do {
		rc = zcrypt_send_cprb(&xcRB64);
	} while (rc == -EAGAIN);
	xcRB32.reply_control_blk_length = xcRB64.reply_control_blk_length;
	xcRB32.reply_data_length = xcRB64.reply_data_length;
	xcRB32.status = xcRB64.status;
	if (copy_to_user(uxcRB32, &xcRB32, sizeof(xcRB32)))
			return -EFAULT;
	return rc;
}

static long zcrypt_compat_ioctl(struct file *filp, unsigned int cmd,
			 unsigned long arg)
{
	if (cmd == ICARSAMODEXPO)
		return trans_modexpo32(filp, cmd, arg);
	if (cmd == ICARSACRT)
		return trans_modexpo_crt32(filp, cmd, arg);
	if (cmd == ZSECSENDCPRB)
		return trans_xcRB32(filp, cmd, arg);
	return zcrypt_unlocked_ioctl(filp, cmd, arg);
}
#endif

/**
 * Misc device file operations.
 */
static const struct file_operations zcrypt_fops = {
	.owner		= THIS_MODULE,
	.read		= zcrypt_read,
	.write		= zcrypt_write,
	.unlocked_ioctl	= zcrypt_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= zcrypt_compat_ioctl,
#endif
	.open		= zcrypt_open,
	.release	= zcrypt_release
};

/**
 * Misc device.
 */
static struct miscdevice zcrypt_misc_device = {
	.minor	    = MISC_DYNAMIC_MINOR,
	.name	    = "z90crypt",
	.fops	    = &zcrypt_fops,
};

/**
 * Deprecated /proc entry support.
 */
static struct proc_dir_entry *zcrypt_entry;

static int sprintcl(unsigned char *outaddr, unsigned char *addr,
		    unsigned int len)
{
	int hl, i;

	hl = 0;
	for (i = 0; i < len; i++)
		hl += sprintf(outaddr+hl, "%01x", (unsigned int) addr[i]);
	hl += sprintf(outaddr+hl, " ");
	return hl;
}

static int sprintrw(unsigned char *outaddr, unsigned char *addr,
		    unsigned int len)
{
	int hl, inl, c, cx;

	hl = sprintf(outaddr, "	   ");
	inl = 0;
	for (c = 0; c < (len / 16); c++) {
		hl += sprintcl(outaddr+hl, addr+inl, 16);
		inl += 16;
	}
	cx = len%16;
	if (cx) {
		hl += sprintcl(outaddr+hl, addr+inl, cx);
		inl += cx;
	}
	hl += sprintf(outaddr+hl, "\n");
	return hl;
}

static int sprinthx(unsigned char *title, unsigned char *outaddr,
		    unsigned char *addr, unsigned int len)
{
	int hl, inl, r, rx;

	hl = sprintf(outaddr, "\n%s\n", title);
	inl = 0;
	for (r = 0; r < (len / 64); r++) {
		hl += sprintrw(outaddr+hl, addr+inl, 64);
		inl += 64;
	}
	rx = len % 64;
	if (rx) {
		hl += sprintrw(outaddr+hl, addr+inl, rx);
		inl += rx;
	}
	hl += sprintf(outaddr+hl, "\n");
	return hl;
}

static int sprinthx4(unsigned char *title, unsigned char *outaddr,
		     unsigned int *array, unsigned int len)
{
	int hl, r;

	hl = sprintf(outaddr, "\n%s\n", title);
	for (r = 0; r < len; r++) {
		if ((r % 8) == 0)
			hl += sprintf(outaddr+hl, "    ");
		hl += sprintf(outaddr+hl, "%08X ", array[r]);
		if ((r % 8) == 7)
			hl += sprintf(outaddr+hl, "\n");
	}
	hl += sprintf(outaddr+hl, "\n");
	return hl;
}

static int zcrypt_status_read(char *resp_buff, char **start, off_t offset,
			      int count, int *eof, void *data)
{
	unsigned char *workarea;
	int len;

	len = 0;

	/* resp_buff is a page. Use the right half for a work area */
	workarea = resp_buff + 2000;
	len += sprintf(resp_buff + len, "\nzcrypt version: %d.%d.%d\n",
		ZCRYPT_VERSION, ZCRYPT_RELEASE, ZCRYPT_VARIANT);
	len += sprintf(resp_buff + len, "Cryptographic domain: %d\n",
		       ap_domain_index);
	len += sprintf(resp_buff + len, "Total device count: %d\n",
		       zcrypt_device_count);
	len += sprintf(resp_buff + len, "PCICA count: %d\n",
		       zcrypt_count_type(ZCRYPT_PCICA));
	len += sprintf(resp_buff + len, "PCICC count: %d\n",
		       zcrypt_count_type(ZCRYPT_PCICC));
	len += sprintf(resp_buff + len, "PCIXCC MCL2 count: %d\n",
		       zcrypt_count_type(ZCRYPT_PCIXCC_MCL2));
	len += sprintf(resp_buff + len, "PCIXCC MCL3 count: %d\n",
		       zcrypt_count_type(ZCRYPT_PCIXCC_MCL3));
	len += sprintf(resp_buff + len, "CEX2C count: %d\n",
		       zcrypt_count_type(ZCRYPT_CEX2C));
	len += sprintf(resp_buff + len, "CEX2A count: %d\n",
		       zcrypt_count_type(ZCRYPT_CEX2A));
	len += sprintf(resp_buff + len, "requestq count: %d\n",
		       zcrypt_requestq_count());
	len += sprintf(resp_buff + len, "pendingq count: %d\n",
		       zcrypt_pendingq_count());
	len += sprintf(resp_buff + len, "Total open handles: %d\n\n",
		       atomic_read(&zcrypt_open_count));
	zcrypt_status_mask(workarea);
	len += sprinthx("Online devices: 1=PCICA 2=PCICC 3=PCIXCC(MCL2) "
			"4=PCIXCC(MCL3) 5=CEX2C 6=CEX2A",
			resp_buff+len, workarea, AP_DEVICES);
	zcrypt_qdepth_mask(workarea);
	len += sprinthx("Waiting work element counts",
			resp_buff+len, workarea, AP_DEVICES);
	zcrypt_perdev_reqcnt((int *) workarea);
	len += sprinthx4("Per-device successfully completed request counts",
			 resp_buff+len,(unsigned int *) workarea, AP_DEVICES);
	*eof = 1;
	memset((void *) workarea, 0x00, AP_DEVICES * sizeof(unsigned int));
	return len;
}

static void zcrypt_disable_card(int index)
{
	struct zcrypt_device *zdev;

	spin_lock_bh(&zcrypt_device_lock);
	list_for_each_entry(zdev, &zcrypt_device_list, list)
		if (AP_QID_DEVICE(zdev->ap_dev->qid) == index) {
			zdev->online = 0;
			ap_flush_queue(zdev->ap_dev);
			break;
		}
	spin_unlock_bh(&zcrypt_device_lock);
}

static void zcrypt_enable_card(int index)
{
	struct zcrypt_device *zdev;

	spin_lock_bh(&zcrypt_device_lock);
	list_for_each_entry(zdev, &zcrypt_device_list, list)
		if (AP_QID_DEVICE(zdev->ap_dev->qid) == index) {
			zdev->online = 1;
			break;
		}
	spin_unlock_bh(&zcrypt_device_lock);
}

static int zcrypt_status_write(struct file *file, const char __user *buffer,
			       unsigned long count, void *data)
{
	unsigned char *lbuf, *ptr;
	unsigned long local_count;
	int j;

	if (count <= 0)
		return 0;

#define LBUFSIZE 1200UL
	lbuf = kmalloc(LBUFSIZE, GFP_KERNEL);
	if (!lbuf) {
		PRINTK("kmalloc failed!\n");
		return 0;
	}

	local_count = min(LBUFSIZE - 1, count);
	if (copy_from_user(lbuf, buffer, local_count) != 0) {
		kfree(lbuf);
		return -EFAULT;
	}
	lbuf[local_count] = '\0';

	ptr = strstr(lbuf, "Online devices");
	if (!ptr) {
		PRINTK("Unable to parse data (missing \"Online devices\")\n");
		goto out;
	}
	ptr = strstr(ptr, "\n");
	if (!ptr) {
		PRINTK("Unable to parse data (missing newline "
		       "after \"Online devices\")\n");
		goto out;
	}
	ptr++;

	if (strstr(ptr, "Waiting work element counts") == NULL) {
		PRINTK("Unable to parse data (missing "
		       "\"Waiting work element counts\")\n");
		goto out;
	}

	for (j = 0; j < 64 && *ptr; ptr++) {
		/**
		 * '0' for no device, '1' for PCICA, '2' for PCICC,
		 * '3' for PCIXCC_MCL2, '4' for PCIXCC_MCL3,
		 * '5' for CEX2C and '6' for CEX2A'
		 */
		if (*ptr >= '0' && *ptr <= '6')
			j++;
		else if (*ptr == 'd' || *ptr == 'D')
			zcrypt_disable_card(j++);
		else if (*ptr == 'e' || *ptr == 'E')
			zcrypt_enable_card(j++);
		else if (*ptr != ' ' && *ptr != '\t')
			break;
	}
out:
	kfree(lbuf);
	return count;
}

/**
 * The module initialization code.
 */
int __init zcrypt_api_init(void)
{
	int rc;

	/* Register the request sprayer. */
	rc = misc_register(&zcrypt_misc_device);
	if (rc < 0) {
		PRINTKW(KERN_ERR "misc_register (minor %d) failed with %d\n",
			zcrypt_misc_device.minor, rc);
		goto out;
	}

	/* Set up the proc file system */
	zcrypt_entry = create_proc_entry("driver/z90crypt", 0644, NULL);
	if (!zcrypt_entry) {
		PRINTK("Couldn't create z90crypt proc entry\n");
		rc = -ENOMEM;
		goto out_misc;
	}
	zcrypt_entry->data = NULL;
	zcrypt_entry->read_proc = zcrypt_status_read;
	zcrypt_entry->write_proc = zcrypt_status_write;

	return 0;

out_misc:
	misc_deregister(&zcrypt_misc_device);
out:
	return rc;
}

/**
 * The module termination code.
 */
void zcrypt_api_exit(void)
{
	remove_proc_entry("driver/z90crypt", NULL);
	misc_deregister(&zcrypt_misc_device);
}

#ifndef CONFIG_ZCRYPT_MONOLITHIC
module_init(zcrypt_api_init);
module_exit(zcrypt_api_exit);
#endif
