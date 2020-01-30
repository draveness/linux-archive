/**
 * \file drm_ioctl.h 
 * IOCTL processing for DRM
 *
 * \author Rickard E. (Rik) Faith <faith@valinux.com>
 * \author Gareth Hughes <gareth@valinux.com>
 */

/*
 * Created: Fri Jan  8 09:01:26 1999 by faith@valinux.com
 *
 * Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "drmP.h"

#include "linux/pci.h"

/**
 * Get the bus id.
 * 
 * \param inode device inode.
 * \param filp file pointer.
 * \param cmd command.
 * \param arg user argument, pointing to a drm_unique structure.
 * \return zero on success or a negative number on failure.
 *
 * Copies the bus id from drm_device::unique into user space.
 */
int DRM(getunique)(struct inode *inode, struct file *filp,
		   unsigned int cmd, unsigned long arg)
{
	drm_file_t	 *priv	 = filp->private_data;
	drm_device_t	 *dev	 = priv->dev;
	drm_unique_t	 __user *argp = (void __user *)arg;
	drm_unique_t	 u;

	if (copy_from_user(&u, argp, sizeof(u)))
		return -EFAULT;
	if (u.unique_len >= dev->unique_len) {
		if (copy_to_user(u.unique, dev->unique, dev->unique_len))
			return -EFAULT;
	}
	u.unique_len = dev->unique_len;
	if (copy_to_user(argp, &u, sizeof(u)))
		return -EFAULT;
	return 0;
}

/**
 * Set the bus id.
 * 
 * \param inode device inode.
 * \param filp file pointer.
 * \param cmd command.
 * \param arg user argument, pointing to a drm_unique structure.
 * \return zero on success or a negative number on failure.
 *
 * Copies the bus id from userspace into drm_device::unique, and verifies that
 * it matches the device this DRM is attached to (EINVAL otherwise).  Deprecated
 * in interface version 1.1 and will return EBUSY when setversion has requested
 * version 1.1 or greater.
 */
int DRM(setunique)(struct inode *inode, struct file *filp,
		   unsigned int cmd, unsigned long arg)
{
	drm_file_t	 *priv	 = filp->private_data;
	drm_device_t	 *dev	 = priv->dev;
	drm_unique_t	 u;
	int		 domain, bus, slot, func, ret;

	if (dev->unique_len || dev->unique) return -EBUSY;

	if (copy_from_user(&u, (drm_unique_t __user *)arg, sizeof(u)))
		return -EFAULT;

	if (!u.unique_len || u.unique_len > 1024) return -EINVAL;

	dev->unique_len = u.unique_len;
	dev->unique	= DRM(alloc)(u.unique_len + 1, DRM_MEM_DRIVER);
	if(!dev->unique) return -ENOMEM;
	if (copy_from_user(dev->unique, u.unique, dev->unique_len))
		return -EFAULT;

	dev->unique[dev->unique_len] = '\0';

	dev->devname = DRM(alloc)(strlen(dev->name) + strlen(dev->unique) + 2,
				  DRM_MEM_DRIVER);
	if (!dev->devname)
		return -ENOMEM;

	sprintf(dev->devname, "%s@%s", dev->name, dev->unique);

	/* Return error if the busid submitted doesn't match the device's actual
	 * busid.
	 */
	ret = sscanf(dev->unique, "PCI:%d:%d:%d", &bus, &slot, &func);
	if (ret != 3)
		return DRM_ERR(EINVAL);
	domain = bus >> 8;
	bus &= 0xff;
	
	if ((domain != dev->pci_domain) ||
	    (bus != dev->pci_bus) ||
	    (slot != dev->pci_slot) ||
	    (func != dev->pci_func))
		return -EINVAL;

	return 0;
}

static int
DRM(set_busid)(drm_device_t *dev)
{
	if (dev->unique != NULL)
		return EBUSY;

	dev->unique_len = 20;
	dev->unique = DRM(alloc)(dev->unique_len + 1, DRM_MEM_DRIVER);
	if (dev->unique == NULL)
		return ENOMEM;

	snprintf(dev->unique, dev->unique_len, "pci:%04x:%02x:%02x.%d",
		dev->pci_domain, dev->pci_bus, dev->pci_slot, dev->pci_func);

	dev->devname = DRM(alloc)(strlen(dev->name) + dev->unique_len + 2,
				DRM_MEM_DRIVER);
	if (dev->devname == NULL)
		return ENOMEM;

	sprintf(dev->devname, "%s@%s", dev->name, dev->unique);

	return 0;
}


/**
 * Get a mapping information.
 *
 * \param inode device inode.
 * \param filp file pointer.
 * \param cmd command.
 * \param arg user argument, pointing to a drm_map structure.
 * 
 * \return zero on success or a negative number on failure.
 *
 * Searches for the mapping with the specified offset and copies its information
 * into userspace
 */
int DRM(getmap)( struct inode *inode, struct file *filp,
		 unsigned int cmd, unsigned long arg )
{
	drm_file_t   *priv = filp->private_data;
	drm_device_t *dev  = priv->dev;
	drm_map_t    __user *argp = (void __user *)arg;
	drm_map_t    map;
	drm_map_list_t *r_list = NULL;
	struct list_head *list;
	int          idx;
	int	     i;

	if (copy_from_user(&map, argp, sizeof(map)))
		return -EFAULT;
	idx = map.offset;

	down(&dev->struct_sem);
	if (idx < 0) {
		up(&dev->struct_sem);
		return -EINVAL;
	}

	i = 0;
	list_for_each(list, &dev->maplist->head) {
		if(i == idx) {
			r_list = list_entry(list, drm_map_list_t, head);
			break;
		}
		i++;
	}
	if(!r_list || !r_list->map) {
		up(&dev->struct_sem);
		return -EINVAL;
	}

	map.offset = r_list->map->offset;
	map.size   = r_list->map->size;
	map.type   = r_list->map->type;
	map.flags  = r_list->map->flags;
	map.handle = r_list->map->handle;
	map.mtrr   = r_list->map->mtrr;
	up(&dev->struct_sem);

	if (copy_to_user(argp, &map, sizeof(map))) return -EFAULT;
	return 0;
}

/**
 * Get client information.
 *
 * \param inode device inode.
 * \param filp file pointer.
 * \param cmd command.
 * \param arg user argument, pointing to a drm_client structure.
 * 
 * \return zero on success or a negative number on failure.
 *
 * Searches for the client with the specified index and copies its information
 * into userspace
 */
int DRM(getclient)( struct inode *inode, struct file *filp,
		    unsigned int cmd, unsigned long arg )
{
	drm_file_t   *priv = filp->private_data;
	drm_device_t *dev  = priv->dev;
	drm_client_t __user *argp = (void __user *)arg;
	drm_client_t client;
	drm_file_t   *pt;
	int          idx;
	int          i;

	if (copy_from_user(&client, argp, sizeof(client)))
		return -EFAULT;
	idx = client.idx;
	down(&dev->struct_sem);
	for (i = 0, pt = dev->file_first; i < idx && pt; i++, pt = pt->next)
		;

	if (!pt) {
		up(&dev->struct_sem);
		return -EINVAL;
	}
	client.auth  = pt->authenticated;
	client.pid   = pt->pid;
	client.uid   = pt->uid;
	client.magic = pt->magic;
	client.iocs  = pt->ioctl_count;
	up(&dev->struct_sem);

	if (copy_to_user((drm_client_t __user *)arg, &client, sizeof(client)))
		return -EFAULT;
	return 0;
}

/** 
 * Get statistics information. 
 * 
 * \param inode device inode.
 * \param filp file pointer.
 * \param cmd command.
 * \param arg user argument, pointing to a drm_stats structure.
 * 
 * \return zero on success or a negative number on failure.
 */
int DRM(getstats)( struct inode *inode, struct file *filp,
		   unsigned int cmd, unsigned long arg )
{
	drm_file_t   *priv = filp->private_data;
	drm_device_t *dev  = priv->dev;
	drm_stats_t  stats;
	int          i;

	memset(&stats, 0, sizeof(stats));
	
	down(&dev->struct_sem);

	for (i = 0; i < dev->counters; i++) {
		if (dev->types[i] == _DRM_STAT_LOCK)
			stats.data[i].value
				= (dev->lock.hw_lock
				   ? dev->lock.hw_lock->lock : 0);
		else 
			stats.data[i].value = atomic_read(&dev->counts[i]);
		stats.data[i].type  = dev->types[i];
	}
	
	stats.count = dev->counters;

	up(&dev->struct_sem);

	if (copy_to_user((drm_stats_t __user *)arg, &stats, sizeof(stats)))
		return -EFAULT;
	return 0;
}

#define DRM_IF_MAJOR	1
#define DRM_IF_MINOR	2

int DRM(setversion)(DRM_IOCTL_ARGS)
{
	DRM_DEVICE;
	drm_set_version_t sv;
	drm_set_version_t retv;
	int if_version;
	drm_set_version_t __user *argp = (void __user *)data;

	DRM_COPY_FROM_USER_IOCTL(sv, argp, sizeof(sv));

	retv.drm_di_major = DRM_IF_MAJOR;
	retv.drm_di_minor = DRM_IF_MINOR;
	retv.drm_dd_major = DRIVER_MAJOR;
	retv.drm_dd_minor = DRIVER_MINOR;

	DRM_COPY_TO_USER_IOCTL(argp, retv, sizeof(sv));

	if (sv.drm_di_major != -1) {
		if (sv.drm_di_major != DRM_IF_MAJOR ||
		    sv.drm_di_minor < 0 || sv.drm_di_minor > DRM_IF_MINOR)
			return EINVAL;
		if_version = DRM_IF_VERSION(sv.drm_di_major, sv.drm_dd_minor);
		dev->if_version = DRM_MAX(if_version, dev->if_version);
		if (sv.drm_di_minor >= 1) {
			/*
			 * Version 1.1 includes tying of DRM to specific device
			 */
			DRM(set_busid)(dev);
		}
	}

	if (sv.drm_dd_major != -1) {
		if (sv.drm_dd_major != DRIVER_MAJOR ||
		    sv.drm_dd_minor < 0 || sv.drm_dd_minor > DRIVER_MINOR)
			return EINVAL;
#ifdef DRIVER_SETVERSION
		DRIVER_SETVERSION(dev, &sv);
#endif
	}
	return 0;
}
