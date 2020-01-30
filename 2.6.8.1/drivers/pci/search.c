/*
 * 	PCI searching functions.
 *
 *	Copyright 1993 -- 1997 Drew Eckhardt, Frederic Potter,
 *				David Mosberger-Tang
 *	Copyright 1997 -- 2000 Martin Mares <mj@ucw.cz>
 *	Copyright 2003 -- Greg Kroah-Hartman <greg@kroah.com>
 */

#include <linux/init.h>
#include <linux/pci.h>
#include <linux/module.h>
#include <linux/interrupt.h>

spinlock_t pci_bus_lock = SPIN_LOCK_UNLOCKED;

static struct pci_bus * __devinit
pci_do_find_bus(struct pci_bus* bus, unsigned char busnr)
{
	struct pci_bus* child;
	struct list_head *tmp;

	if(bus->number == busnr)
		return bus;

	list_for_each(tmp, &bus->children) {
		child = pci_do_find_bus(pci_bus_b(tmp), busnr);
		if(child)
			return child;
	}
	return NULL;
}

/**
 * pci_find_bus - locate PCI bus from a given domain and bus number
 * @domain: number of PCI domain to search
 * @busnr: number of desired PCI bus
 *
 * Given a PCI bus number and domain number, the desired PCI bus is located
 * in the global list of PCI buses.  If the bus is found, a pointer to its
 * data structure is returned.  If no bus is found, %NULL is returned.
 */
struct pci_bus * __devinit pci_find_bus(int domain, int busnr)
{
	struct pci_bus *bus = NULL;
	struct pci_bus *tmp_bus;

	while ((bus = pci_find_next_bus(bus)) != NULL)  {
		if (pci_domain_nr(bus) != domain)
			continue;
		tmp_bus = pci_do_find_bus(bus, busnr);
		if (tmp_bus)
			return tmp_bus;
	}
	return NULL;
}

/**
 * pci_find_next_bus - begin or continue searching for a PCI bus
 * @from: Previous PCI bus found, or %NULL for new search.
 *
 * Iterates through the list of known PCI busses.  A new search is
 * initiated by passing %NULL to the @from argument.  Otherwise if
 * @from is not %NULL, searches continue from next device on the
 * global list.
 */
struct pci_bus * 
pci_find_next_bus(const struct pci_bus *from)
{
	struct list_head *n;
	struct pci_bus *b = NULL;

	WARN_ON(in_interrupt());
	spin_lock(&pci_bus_lock);
	n = from ? from->node.next : pci_root_buses.next;
	if (n != &pci_root_buses)
		b = pci_bus_b(n);
	spin_unlock(&pci_bus_lock);
	return b;
}

/**
 * pci_find_slot - locate PCI device from a given PCI slot
 * @bus: number of PCI bus on which desired PCI device resides
 * @devfn: encodes number of PCI slot in which the desired PCI 
 * device resides and the logical device number within that slot 
 * in case of multi-function devices.
 *
 * Given a PCI bus and slot/function number, the desired PCI device 
 * is located in system global list of PCI devices.  If the device
 * is found, a pointer to its data structure is returned.  If no 
 * device is found, %NULL is returned.
 */
struct pci_dev *
pci_find_slot(unsigned int bus, unsigned int devfn)
{
	struct pci_dev *dev = NULL;

	while ((dev = pci_find_device(PCI_ANY_ID, PCI_ANY_ID, dev)) != NULL) {
		if (dev->bus->number == bus && dev->devfn == devfn)
			return dev;
	}
	return NULL;
}

/**
 * pci_get_slot - locate PCI device for a given PCI slot
 * @bus: PCI bus on which desired PCI device resides
 * @devfn: encodes number of PCI slot in which the desired PCI 
 * device resides and the logical device number within that slot 
 * in case of multi-function devices.
 *
 * Given a PCI bus and slot/function number, the desired PCI device 
 * is located in the list of PCI devices.
 * If the device is found, its reference count is increased and this
 * function returns a pointer to its data structure.  The caller must
 * decrement the reference count by calling pci_dev_put().
 * If no device is found, %NULL is returned.
 */
struct pci_dev * pci_get_slot(struct pci_bus *bus, unsigned int devfn)
{
	struct list_head *tmp;
	struct pci_dev *dev;

	WARN_ON(in_interrupt());
	spin_lock(&pci_bus_lock);

	list_for_each(tmp, &bus->devices) {
		dev = pci_dev_b(tmp);
		if (dev->devfn == devfn)
			goto out;
	}

	dev = NULL;
 out:
	pci_dev_get(dev);
	spin_unlock(&pci_bus_lock);
	return dev;
}

/**
 * pci_find_subsys - begin or continue searching for a PCI device by vendor/subvendor/device/subdevice id
 * @vendor: PCI vendor id to match, or %PCI_ANY_ID to match all vendor ids
 * @device: PCI device id to match, or %PCI_ANY_ID to match all device ids
 * @ss_vendor: PCI subsystem vendor id to match, or %PCI_ANY_ID to match all vendor ids
 * @ss_device: PCI subsystem device id to match, or %PCI_ANY_ID to match all device ids
 * @from: Previous PCI device found in search, or %NULL for new search.
 *
 * Iterates through the list of known PCI devices.  If a PCI device is
 * found with a matching @vendor, @device, @ss_vendor and @ss_device, a pointer to its
 * device structure is returned.  Otherwise, %NULL is returned.
 * A new search is initiated by passing %NULL to the @from argument.
 * Otherwise if @from is not %NULL, searches continue from next device on the global list.
 *
 * NOTE: Do not use this function anymore, use pci_get_subsys() instead, as
 * the pci device returned by this function can disappear at any moment in
 * time.
 */
struct pci_dev *
pci_find_subsys(unsigned int vendor, unsigned int device,
		unsigned int ss_vendor, unsigned int ss_device,
		const struct pci_dev *from)
{
	struct list_head *n;
	struct pci_dev *dev;

	WARN_ON(in_interrupt());
	spin_lock(&pci_bus_lock);
	n = from ? from->global_list.next : pci_devices.next;

	while (n && (n != &pci_devices)) {
		dev = pci_dev_g(n);
		if ((vendor == PCI_ANY_ID || dev->vendor == vendor) &&
		    (device == PCI_ANY_ID || dev->device == device) &&
		    (ss_vendor == PCI_ANY_ID || dev->subsystem_vendor == ss_vendor) &&
		    (ss_device == PCI_ANY_ID || dev->subsystem_device == ss_device))
			goto exit;
		n = n->next;
	}
	dev = NULL;
exit:
	spin_unlock(&pci_bus_lock);
	return dev;
}

/**
 * pci_find_device - begin or continue searching for a PCI device by vendor/device id
 * @vendor: PCI vendor id to match, or %PCI_ANY_ID to match all vendor ids
 * @device: PCI device id to match, or %PCI_ANY_ID to match all device ids
 * @from: Previous PCI device found in search, or %NULL for new search.
 *
 * Iterates through the list of known PCI devices.  If a PCI device is
 * found with a matching @vendor and @device, a pointer to its device structure is
 * returned.  Otherwise, %NULL is returned.
 * A new search is initiated by passing %NULL to the @from argument.
 * Otherwise if @from is not %NULL, searches continue from next device on the global list.
 * 
 * NOTE: Do not use this function anymore, use pci_get_device() instead, as
 * the pci device returned by this function can disappear at any moment in
 * time.
 */
struct pci_dev *
pci_find_device(unsigned int vendor, unsigned int device, const struct pci_dev *from)
{
	return pci_find_subsys(vendor, device, PCI_ANY_ID, PCI_ANY_ID, from);
}

/**
 * pci_get_subsys - begin or continue searching for a PCI device by vendor/subvendor/device/subdevice id
 * @vendor: PCI vendor id to match, or %PCI_ANY_ID to match all vendor ids
 * @device: PCI device id to match, or %PCI_ANY_ID to match all device ids
 * @ss_vendor: PCI subsystem vendor id to match, or %PCI_ANY_ID to match all vendor ids
 * @ss_device: PCI subsystem device id to match, or %PCI_ANY_ID to match all device ids
 * @from: Previous PCI device found in search, or %NULL for new search.
 *
 * Iterates through the list of known PCI devices.  If a PCI device is
 * found with a matching @vendor, @device, @ss_vendor and @ss_device, a pointer to its
 * device structure is returned, and the reference count to the device is
 * incremented.  Otherwise, %NULL is returned.  A new search is initiated by
 * passing %NULL to the @from argument.  Otherwise if @from is not %NULL,
 * searches continue from next device on the global list.
 * The reference count for @from is always decremented if it is not %NULL.
 */
struct pci_dev * 
pci_get_subsys(unsigned int vendor, unsigned int device,
	       unsigned int ss_vendor, unsigned int ss_device,
	       struct pci_dev *from)
{
	struct list_head *n;
	struct pci_dev *dev;

	WARN_ON(in_interrupt());
	spin_lock(&pci_bus_lock);
	n = from ? from->global_list.next : pci_devices.next;

	while (n && (n != &pci_devices)) {
		dev = pci_dev_g(n);
		if ((vendor == PCI_ANY_ID || dev->vendor == vendor) &&
		    (device == PCI_ANY_ID || dev->device == device) &&
		    (ss_vendor == PCI_ANY_ID || dev->subsystem_vendor == ss_vendor) &&
		    (ss_device == PCI_ANY_ID || dev->subsystem_device == ss_device))
			goto exit;
		n = n->next;
	}
	dev = NULL;
exit:
	pci_dev_put(from);
	dev = pci_dev_get(dev);
	spin_unlock(&pci_bus_lock);
	return dev;
}

/**
 * pci_get_device - begin or continue searching for a PCI device by vendor/device id
 * @vendor: PCI vendor id to match, or %PCI_ANY_ID to match all vendor ids
 * @device: PCI device id to match, or %PCI_ANY_ID to match all device ids
 * @from: Previous PCI device found in search, or %NULL for new search.
 *
 * Iterates through the list of known PCI devices.  If a PCI device is
 * found with a matching @vendor and @device, a pointer to its device structure is
 * returned.  Otherwise, %NULL is returned.
 * A new search is initiated by passing %NULL to the @from argument.
 * Otherwise if @from is not %NULL, searches continue from next device on the global list.
 *
 * Iterates through the list of known PCI devices.  If a PCI device is
 * found with a matching @vendor and @device, the reference count to the
 * device is incremented and a pointer to its device structure is returned.
 * Otherwise, %NULL is returned.  A new search is initiated by passing %NULL
 * to the @from argument.  Otherwise if @from is not %NULL, searches continue
 * from next device on the global list.  The reference count for @from is
 * always decremented if it is not %NULL.
 */
struct pci_dev *
pci_get_device(unsigned int vendor, unsigned int device, struct pci_dev *from)
{
	return pci_get_subsys(vendor, device, PCI_ANY_ID, PCI_ANY_ID, from);
}


/**
 * pci_find_device_reverse - begin or continue searching for a PCI device by vendor/device id
 * @vendor: PCI vendor id to match, or %PCI_ANY_ID to match all vendor ids
 * @device: PCI device id to match, or %PCI_ANY_ID to match all device ids
 * @from: Previous PCI device found in search, or %NULL for new search.
 *
 * Iterates through the list of known PCI devices in the reverse order of pci_find_device().
 * If a PCI device is found with a matching @vendor and @device, a pointer to
 * its device structure is returned.  Otherwise, %NULL is returned.
 * A new search is initiated by passing %NULL to the @from argument.
 * Otherwise if @from is not %NULL, searches continue from previous device on the global list.
 */
struct pci_dev *
pci_find_device_reverse(unsigned int vendor, unsigned int device, const struct pci_dev *from)
{
	struct list_head *n;
	struct pci_dev *dev;

	WARN_ON(in_interrupt());
	spin_lock(&pci_bus_lock);
	n = from ? from->global_list.prev : pci_devices.prev;

	while (n && (n != &pci_devices)) {
		dev = pci_dev_g(n);
		if ((vendor == PCI_ANY_ID || dev->vendor == vendor) &&
		    (device == PCI_ANY_ID || dev->device == device))
			goto exit;
		n = n->prev;
	}
	dev = NULL;
exit:
	spin_unlock(&pci_bus_lock);
	return dev;
}


/**
 * pci_find_class - begin or continue searching for a PCI device by class
 * @class: search for a PCI device with this class designation
 * @from: Previous PCI device found in search, or %NULL for new search.
 *
 * Iterates through the list of known PCI devices.  If a PCI device is
 * found with a matching @class, a pointer to its device structure is
 * returned.  Otherwise, %NULL is returned.
 * A new search is initiated by passing %NULL to the @from argument.
 * Otherwise if @from is not %NULL, searches continue from next device
 * on the global list.
 */
struct pci_dev *
pci_find_class(unsigned int class, const struct pci_dev *from)
{
	struct list_head *n;
	struct pci_dev *dev;

	spin_lock(&pci_bus_lock);
	n = from ? from->global_list.next : pci_devices.next;

	while (n && (n != &pci_devices)) {
		dev = pci_dev_g(n);
		if (dev->class == class)
			goto exit;
		n = n->next;
	}
	dev = NULL;
exit:
	spin_unlock(&pci_bus_lock);
	return dev;
}

EXPORT_SYMBOL(pci_find_bus);
EXPORT_SYMBOL(pci_find_class);
EXPORT_SYMBOL(pci_find_device);
EXPORT_SYMBOL(pci_find_device_reverse);
EXPORT_SYMBOL(pci_find_slot);
EXPORT_SYMBOL(pci_find_subsys);
EXPORT_SYMBOL(pci_get_device);
EXPORT_SYMBOL(pci_get_subsys);
EXPORT_SYMBOL(pci_get_slot);
