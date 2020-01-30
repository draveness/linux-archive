/*
 *  pci_irq.c - ACPI PCI Interrupt Routing ($Revision: 11 $)
 *
 *  Copyright (C) 2001, 2002 Andy Grover <andrew.grover@intel.com>
 *  Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 *  Copyright (C) 2002       Dominik Brodowski <devel@brodo.de>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <linux/config.h>

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
#include <linux/pm.h>
#include <linux/pci.h>
#include <linux/acpi.h>
#include <acpi/acpi_bus.h>
#include <acpi/acpi_drivers.h>


#define _COMPONENT		ACPI_PCI_COMPONENT
ACPI_MODULE_NAME		("pci_irq")

struct acpi_prt_list		acpi_prt;


/* --------------------------------------------------------------------------
                         PCI IRQ Routing Table (PRT) Support
   -------------------------------------------------------------------------- */

static struct acpi_prt_entry *
acpi_pci_irq_find_prt_entry (
	int			segment,
	int			bus,
	int			device,
	int			pin)
{
	struct list_head	*node = NULL;
	struct acpi_prt_entry	*entry = NULL;

	ACPI_FUNCTION_TRACE("acpi_pci_irq_find_prt_entry");

	if (!acpi_prt.count)
		return_PTR(NULL);

	/*
	 * Parse through all PRT entries looking for a match on the specified
	 * PCI device's segment, bus, device, and pin (don't care about func).
	 *
	 * TBD: Acquire/release lock
	 */
	list_for_each(node, &acpi_prt.entries) {
		entry = list_entry(node, struct acpi_prt_entry, node);
		if ((segment == entry->id.segment) 
			&& (bus == entry->id.bus) 
			&& (device == entry->id.device)
			&& (pin == entry->pin)) {
			return_PTR(entry);
		}
	}

	return_PTR(NULL);
}


static int
acpi_pci_irq_add_entry (
	acpi_handle			handle,
	int				segment,
	int				bus,
	struct acpi_pci_routing_table	*prt)
{
	struct acpi_prt_entry	*entry = NULL;

	ACPI_FUNCTION_TRACE("acpi_pci_irq_add_entry");

	if (!prt)
		return_VALUE(-EINVAL);

	entry = kmalloc(sizeof(struct acpi_prt_entry), GFP_KERNEL);
	if (!entry)
		return_VALUE(-ENOMEM);
	memset(entry, 0, sizeof(struct acpi_prt_entry));

	entry->id.segment = segment;
	entry->id.bus = bus;
	entry->id.device = (prt->address >> 16) & 0xFFFF;
	entry->id.function = prt->address & 0xFFFF;
	entry->pin = prt->pin;

	/*
	 * Type 1: Dynamic
	 * ---------------
	 * The 'source' field specifies the PCI interrupt link device used to
	 * configure the IRQ assigned to this slot|dev|pin.  The 'source_index'
	 * indicates which resource descriptor in the resource template (of
	 * the link device) this interrupt is allocated from.
	 * 
	 * NOTE: Don't query the Link Device for IRQ information at this time
	 *       because Link Device enumeration may not have occurred yet
	 *       (e.g. exists somewhere 'below' this _PRT entry in the ACPI
	 *       namespace).
	 */
	if (prt->source[0]) {
		acpi_get_handle(handle, prt->source, &entry->link.handle);
		entry->link.index = prt->source_index;
	}
	/*
	 * Type 2: Static
	 * --------------
	 * The 'source' field is NULL, and the 'source_index' field specifies
	 * the IRQ value, which is hardwired to specific interrupt inputs on
	 * the interrupt controller.
	 */
	else
		entry->link.index = prt->source_index;

	ACPI_DEBUG_PRINT_RAW((ACPI_DB_INFO,
		"      %02X:%02X:%02X[%c] -> %s[%d]\n", 
		entry->id.segment, entry->id.bus, entry->id.device, 
		('A' + entry->pin), prt->source, entry->link.index));

	/* TBD: Acquire/release lock */
	list_add_tail(&entry->node, &acpi_prt.entries);
	acpi_prt.count++;

	return_VALUE(0);
}


int
acpi_pci_irq_add_prt (
	acpi_handle		handle,
	int			segment,
	int			bus)
{
	acpi_status			status = AE_OK;
	char				pathname[ACPI_PATHNAME_MAX] = {0};
	struct acpi_buffer		buffer = {0, NULL};
	struct acpi_pci_routing_table	*prt = NULL;
	struct acpi_pci_routing_table	*entry = NULL;
	static int			first_time = 1;

	ACPI_FUNCTION_TRACE("acpi_pci_irq_add_prt");

	if (first_time) {
		acpi_prt.count = 0;
		INIT_LIST_HEAD(&acpi_prt.entries);
		first_time = 0;
	}

	/* 
	 * NOTE: We're given a 'handle' to the _PRT object's parent device
	 *       (either a PCI root bridge or PCI-PCI bridge).
	 */

	buffer.length = sizeof(pathname);
	buffer.pointer = pathname;
	acpi_get_name(handle, ACPI_FULL_PATHNAME, &buffer);

	printk(KERN_DEBUG "ACPI: PCI Interrupt Routing Table [%s._PRT]\n",
		pathname);

	/* 
	 * Evaluate this _PRT and add its entries to our global list (acpi_prt).
	 */

	buffer.length = 0;
	buffer.pointer = NULL;
	status = acpi_get_irq_routing_table(handle, &buffer);
	if (status != AE_BUFFER_OVERFLOW) {
		ACPI_DEBUG_PRINT((ACPI_DB_ERROR, "Error evaluating _PRT [%s]\n",
			acpi_format_exception(status)));
		return_VALUE(-ENODEV);
	}

	prt = kmalloc(buffer.length, GFP_KERNEL);
	if (!prt)
		return_VALUE(-ENOMEM);
	memset(prt, 0, buffer.length);
	buffer.pointer = prt;

	status = acpi_get_irq_routing_table(handle, &buffer);
	if (ACPI_FAILURE(status)) {
		ACPI_DEBUG_PRINT((ACPI_DB_ERROR, "Error evaluating _PRT [%s]\n",
			acpi_format_exception(status)));
		kfree(buffer.pointer);
		return_VALUE(-ENODEV);
	}

	entry = prt;

	while (entry && (entry->length > 0)) {
		acpi_pci_irq_add_entry(handle, segment, bus, entry);
		entry = (struct acpi_pci_routing_table *)
			((unsigned long) entry + entry->length);
	}

	kfree(prt);

	return_VALUE(0);
}


/* --------------------------------------------------------------------------
                          PCI Interrupt Routing Support
   -------------------------------------------------------------------------- */

static int
acpi_pci_irq_lookup (
	struct pci_bus		*bus,
	int			device,
	int			pin,
	int			*edge_level,
	int			*active_high_low)
{
	struct acpi_prt_entry	*entry = NULL;
	int segment = pci_domain_nr(bus);
	int bus_nr = bus->number;
	int irq;

	ACPI_FUNCTION_TRACE("acpi_pci_irq_lookup");

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, 
		"Searching for PRT entry for %02x:%02x:%02x[%c]\n", 
		segment, bus_nr, device, ('A' + pin)));

	entry = acpi_pci_irq_find_prt_entry(segment, bus_nr, device, pin); 
	if (!entry) {
		ACPI_DEBUG_PRINT((ACPI_DB_INFO, "PRT entry not found\n"));
		return_VALUE(0);
	}
	
	if (entry->link.handle) {
		irq = acpi_pci_link_get_irq(entry->link.handle, entry->link.index, edge_level, active_high_low);
		if (!irq) {
			ACPI_DEBUG_PRINT((ACPI_DB_WARN, "Invalid IRQ link routing entry\n"));
			return_VALUE(0);
		}
	} else {
		irq = entry->link.index;
		*edge_level = ACPI_LEVEL_SENSITIVE;
		*active_high_low = ACPI_ACTIVE_LOW;
	}

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, "Found IRQ %d\n", irq));

	return_VALUE(irq);
}

static int
acpi_pci_irq_derive (
	struct pci_dev		*dev,
	int			pin,
	int			*edge_level,
	int			*active_high_low)
{
	struct pci_dev		*bridge = dev;
	int			irq = 0;
	u8			bridge_pin = 0;

	ACPI_FUNCTION_TRACE("acpi_pci_irq_derive");

	if (!dev)
		return_VALUE(-EINVAL);

	/* 
	 * Attempt to derive an IRQ for this device from a parent bridge's
	 * PCI interrupt routing entry (eg. yenta bridge and add-in card bridge).
	 */
	while (!irq && bridge->bus->self) {
		pin = (pin + PCI_SLOT(bridge->devfn)) % 4;
		bridge = bridge->bus->self;

		if ((bridge->class >> 8) == PCI_CLASS_BRIDGE_CARDBUS) {
			/* PC card has the same IRQ as its cardbridge */
			pci_read_config_byte(bridge, PCI_INTERRUPT_PIN, &bridge_pin);
			if (!bridge_pin) {
				ACPI_DEBUG_PRINT((ACPI_DB_INFO, 
					"No interrupt pin configured for device %s\n", pci_name(bridge)));
				return_VALUE(0);
			}
			/* Pin is from 0 to 3 */
			bridge_pin --;
			pin = bridge_pin;
		}

		irq = acpi_pci_irq_lookup(bridge->bus, PCI_SLOT(bridge->devfn),
			pin, edge_level, active_high_low);
	}

	if (!irq) {
		ACPI_DEBUG_PRINT((ACPI_DB_WARN, "Unable to derive IRQ for device %s\n", pci_name(dev)));
		return_VALUE(0);
	}

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, "Derive IRQ %d for device %s from %s\n",
		irq, pci_name(dev), pci_name(bridge)));

	return_VALUE(irq);
}


int
acpi_pci_irq_enable (
	struct pci_dev		*dev)
{
	int			irq = 0;
	u8			pin = 0;
	int			edge_level = ACPI_LEVEL_SENSITIVE;
	int			active_high_low = ACPI_ACTIVE_LOW;

	ACPI_FUNCTION_TRACE("acpi_pci_irq_enable");

	if (!dev)
		return_VALUE(-EINVAL);
	
	pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &pin);
	if (!pin) {
		ACPI_DEBUG_PRINT((ACPI_DB_INFO, "No interrupt pin configured for device %s\n", pci_name(dev)));
		return_VALUE(0);
	}
	pin--;

	if (!dev->bus) {
		ACPI_DEBUG_PRINT((ACPI_DB_ERROR, "Invalid (NULL) 'bus' field\n"));
		return_VALUE(-ENODEV);
	}

	/* 
	 * First we check the PCI IRQ routing table (PRT) for an IRQ.  PRT
	 * values override any BIOS-assigned IRQs set during boot.
	 */
 	irq = acpi_pci_irq_lookup(dev->bus, PCI_SLOT(dev->devfn), pin, &edge_level, &active_high_low);

	/*
	 * If no PRT entry was found, we'll try to derive an IRQ from the
	 * device's parent bridge.
	 */
	if (!irq)
 		irq = acpi_pci_irq_derive(dev, pin, &edge_level, &active_high_low);
 
	/*
	 * No IRQ known to the ACPI subsystem - maybe the BIOS / 
	 * driver reported one, then use it. Exit in any case.
	 */
	if (!irq) {
		printk(KERN_WARNING PREFIX "PCI interrupt %s[%c]: no GSI",
			pci_name(dev), ('A' + pin));
		/* Interrupt Line values above 0xF are forbidden */
		if (dev->irq && (dev->irq <= 0xF)) {
			printk(" - using IRQ %d\n", dev->irq);
			return_VALUE(dev->irq);
		}
		else {
			printk("\n");
			return_VALUE(0);
		}
 	}

	dev->irq = acpi_register_gsi(irq, edge_level, active_high_low);

	printk(KERN_INFO PREFIX "PCI interrupt %s[%c] -> GSI %u "
		"(%s, %s) -> IRQ %d\n",
		pci_name(dev), 'A' + pin, irq,
		(edge_level == ACPI_LEVEL_SENSITIVE) ? "level" : "edge",
		(active_high_low == ACPI_ACTIVE_LOW) ? "low" : "high",
		dev->irq);

	return_VALUE(dev->irq);
}
