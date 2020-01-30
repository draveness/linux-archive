/*
 * include/linux/uio_driver.h
 *
 * Copyright(C) 2005, Benedikt Spranger <b.spranger@linutronix.de>
 * Copyright(C) 2005, Thomas Gleixner <tglx@linutronix.de>
 * Copyright(C) 2006, Hans J. Koch <hjk@linutronix.de>
 * Copyright(C) 2006, Greg Kroah-Hartman <greg@kroah.com>
 *
 * Userspace IO driver.
 *
 * Licensed under the GPLv2 only.
 */

#ifndef _UIO_DRIVER_H_
#define _UIO_DRIVER_H_

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/interrupt.h>

/**
 * struct uio_mem - description of a UIO memory region
 * @kobj:		kobject for this mapping
 * @addr:		address of the device's memory
 * @size:		size of IO
 * @memtype:		type of memory addr points to
 * @internal_addr:	ioremap-ped version of addr, for driver internal use
 */
struct uio_mem {
	struct kobject		kobj;
	unsigned long		addr;
	unsigned long		size;
	int			memtype;
	void __iomem		*internal_addr;
};

#define MAX_UIO_MAPS 	5

struct uio_device;

/**
 * struct uio_info - UIO device capabilities
 * @uio_dev:		the UIO device this info belongs to
 * @name:		device name
 * @version:		device driver version
 * @mem:		list of mappable memory regions, size==0 for end of list
 * @irq:		interrupt number or UIO_IRQ_CUSTOM
 * @irq_flags:		flags for request_irq()
 * @priv:		optional private data
 * @handler:		the device's irq handler
 * @mmap:		mmap operation for this uio device
 * @open:		open operation for this uio device
 * @release:		release operation for this uio device
 */
struct uio_info {
	struct uio_device	*uio_dev;
	char			*name;
	char			*version;
	struct uio_mem		mem[MAX_UIO_MAPS];
	long			irq;
	unsigned long		irq_flags;
	void			*priv;
	irqreturn_t (*handler)(int irq, struct uio_info *dev_info);
	int (*mmap)(struct uio_info *info, struct vm_area_struct *vma);
	int (*open)(struct uio_info *info, struct inode *inode);
	int (*release)(struct uio_info *info, struct inode *inode);
};

extern int __must_check
	__uio_register_device(struct module *owner,
			      struct device *parent,
			      struct uio_info *info);
static inline int __must_check
	uio_register_device(struct device *parent, struct uio_info *info)
{
	return __uio_register_device(THIS_MODULE, parent, info);
}
extern void uio_unregister_device(struct uio_info *info);
extern void uio_event_notify(struct uio_info *info);

/* defines for uio_device->irq */
#define UIO_IRQ_CUSTOM	-1
#define UIO_IRQ_NONE	-2

/* defines for uio_device->memtype */
#define UIO_MEM_NONE	0
#define UIO_MEM_PHYS	1
#define UIO_MEM_LOGICAL	2
#define UIO_MEM_VIRTUAL 3

#endif /* _LINUX_UIO_DRIVER_H_ */
