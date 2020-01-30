/*
 *	drivers/pci/bus.c
 *
 * From setup-res.c, by:
 *	Dave Rusling (david.rusling@reo.mts.dec.com)
 *	David Mosberger (davidm@cs.arizona.edu)
 *	David Miller (davem@redhat.com)
 *	Ivan Kokshaysky (ink@jurassic.park.msu.ru)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/proc_fs.h>
#include <linux/init.h>

#include "pci.h"

/**
 * pci_bus_alloc_resource - allocate a resource from a parent bus
 * @bus: PCI bus
 * @res: resource to allocate
 * @size: size of resource to allocate
 * @align: alignment of resource to allocate
 * @min: minimum /proc/iomem address to allocate
 * @type_mask: IORESOURCE_* type flags
 * @alignf: resource alignment function
 * @alignf_data: data argument for resource alignment function
 *
 * Given the PCI bus a device resides on, the size, minimum address,
 * alignment and type, try to find an acceptable resource allocation
 * for a specific device resource.
 */
int
pci_bus_alloc_resource(struct pci_bus *bus, struct resource *res,
	unsigned long size, unsigned long align, unsigned long min,
	unsigned int type_mask,
	void (*alignf)(void *, struct resource *,
			unsigned long, unsigned long),
	void *alignf_data)
{
	int i, ret = -ENOMEM;

	type_mask |= IORESOURCE_IO | IORESOURCE_MEM;

	for (i = 0; i < PCI_BUS_NUM_RESOURCES; i++) {
		struct resource *r = bus->resource[i];
		if (!r)
			continue;

		/* type_mask must match */
		if ((res->flags ^ r->flags) & type_mask)
			continue;

		/* We cannot allocate a non-prefetching resource
		   from a pre-fetching area */
		if ((r->flags & IORESOURCE_PREFETCH) &&
		    !(res->flags & IORESOURCE_PREFETCH))
			continue;

		/* Ok, try it out.. */
		ret = allocate_resource(r, res, size, min, -1, align,
					alignf, alignf_data);
		if (ret == 0)
			break;
	}
	return ret;
}

/**
 * pci_bus_add_devices - insert newly discovered PCI devices
 * @bus: bus to check for new devices
 *
 * Add newly discovered PCI devices (which are on the bus->devices
 * list) to the global PCI device list, add the sysfs and procfs
 * entries.  Where a bridge is found, add the discovered bus to
 * the parents list of child buses, and recurse (breadth-first
 * to be compatible with 2.4)
 *
 * Call hotplug for each new devices.
 */
void __devinit pci_bus_add_devices(struct pci_bus *bus)
{
	struct pci_dev *dev;

	list_for_each_entry(dev, &bus->devices, bus_list) {
		/*
		 * Skip already-present devices (which are on the
		 * global device list.)
		 */
		if (!list_empty(&dev->global_list))
			continue;

		device_add(&dev->dev);

		spin_lock(&pci_bus_lock);
		list_add_tail(&dev->global_list, &pci_devices);
		spin_unlock(&pci_bus_lock);

		pci_proc_attach_device(dev);
		pci_create_sysfs_dev_files(dev);

	}

	list_for_each_entry(dev, &bus->devices, bus_list) {

		BUG_ON(list_empty(&dev->global_list));

		/*
		 * If there is an unattached subordinate bus, attach
		 * it and then scan for unattached PCI devices.
		 */
		if (dev->subordinate && list_empty(&dev->subordinate->node)) {
			spin_lock(&pci_bus_lock);
			list_add_tail(&dev->subordinate->node, &dev->bus->children);
			spin_unlock(&pci_bus_lock);
			pci_bus_add_devices(dev->subordinate);

			sysfs_create_link(&dev->subordinate->class_dev.kobj, &dev->dev.kobj, "bridge");
		}
	}
}

void pci_enable_bridges(struct pci_bus *bus)
{
	struct pci_dev *dev;

	list_for_each_entry(dev, &bus->devices, bus_list) {
		if (dev->subordinate) {
			pci_enable_device(dev);
			pci_set_master(dev);
			pci_enable_bridges(dev->subordinate);
		}
	}
}

EXPORT_SYMBOL(pci_bus_alloc_resource);
EXPORT_SYMBOL(pci_bus_add_devices);
EXPORT_SYMBOL(pci_enable_bridges);
