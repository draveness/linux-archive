/*
 *	Low-Level PCI Access for i386 machines
 *
 * Copyright 1993, 1994 Drew Eckhardt
 *      Visionary Computing
 *      (Unix and Linux consulting and custom programming)
 *      Drew@Colorado.EDU
 *      +1 (303) 786-7975
 *
 * Drew's work was sponsored by:
 *	iX Multiuser Multitasking Magazine
 *	Hannover, Germany
 *	hm@ix.de
 *
 * Copyright 1997--2000 Martin Mares <mj@ucw.cz>
 *
 * For more information, please consult the following manuals (look at
 * http://www.pcisig.com/ for how to get them):
 *
 * PCI BIOS Specification
 * PCI Local Bus Specification
 * PCI to PCI Bridge Specification
 * PCI System Design Guide
 *
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/errno.h>

#include "pci.h"

/*
 * We need to avoid collisions with `mirrored' VGA ports
 * and other strange ISA hardware, so we always want the
 * addresses to be allocated in the 0x000-0x0ff region
 * modulo 0x400.
 *
 * Why? Because some silly external IO cards only decode
 * the low 10 bits of the IO address. The 0x00-0xff region
 * is reserved for motherboard devices that decode all 16
 * bits, so it's ok to allocate at, say, 0x2800-0x28ff,
 * but we want to try to avoid allocating at 0x2900-0x2bff
 * which might have be mirrored at 0x0100-0x03ff..
 */
void
pcibios_align_resource(void *data, struct resource *res,
		       unsigned long size, unsigned long align)
{
	if (res->flags & IORESOURCE_IO) {
		unsigned long start = res->start;

		if (start & 0x300) {
			start = (start + 0x3ff) & ~0x3ff;
			res->start = start;
		}
	}
}


/*
 *  Handle resources of PCI devices.  If the world were perfect, we could
 *  just allocate all the resource regions and do nothing more.  It isn't.
 *  On the other hand, we cannot just re-allocate all devices, as it would
 *  require us to know lots of host bridge internals.  So we attempt to
 *  keep as much of the original configuration as possible, but tweak it
 *  when it's found to be wrong.
 *
 *  Known BIOS problems we have to work around:
 *	- I/O or memory regions not configured
 *	- regions configured, but not enabled in the command register
 *	- bogus I/O addresses above 64K used
 *	- expansion ROMs left enabled (this may sound harmless, but given
 *	  the fact the PCI specs explicitly allow address decoders to be
 *	  shared between expansion ROMs and other resource regions, it's
 *	  at least dangerous)
 *
 *  Our solution:
 *	(1) Allocate resources for all buses behind PCI-to-PCI bridges.
 *	    This gives us fixed barriers on where we can allocate.
 *	(2) Allocate resources for all enabled devices.  If there is
 *	    a collision, just mark the resource as unallocated. Also
 *	    disable expansion ROMs during this step.
 *	(3) Try to allocate resources for disabled devices.  If the
 *	    resources were assigned correctly, everything goes well,
 *	    if they weren't, they won't disturb allocation of other
 *	    resources.
 *	(4) Assign new addresses to resources which were either
 *	    not configured at all or misconfigured.  If explicitly
 *	    requested by the user, configure expansion ROM address
 *	    as well.
 */

static void __init pcibios_allocate_bus_resources(struct list_head *bus_list)
{
	struct list_head *ln;
	struct pci_bus *bus;
	struct pci_dev *dev;
	int idx;
	struct resource *r, *pr;

	/* Depth-First Search on bus tree */
	for (ln=bus_list->next; ln != bus_list; ln=ln->next) {
		bus = pci_bus_b(ln);
		if ((dev = bus->self)) {
			for (idx = PCI_BRIDGE_RESOURCES; idx < PCI_NUM_RESOURCES; idx++) {
				r = &dev->resource[idx];
				if (!r->start)
					continue;
				pr = pci_find_parent_resource(dev, r);
				if (!pr || request_resource(pr, r) < 0)
					printk(KERN_ERR "PCI: Cannot allocate resource region %d of bridge %s\n", idx, pci_name(dev));
			}
		}
		pcibios_allocate_bus_resources(&bus->children);
	}
}

static void __init pcibios_allocate_resources(int pass)
{
	struct pci_dev *dev = NULL;
	int idx, disabled;
	u16 command;
	struct resource *r, *pr;

	while ((dev = pci_find_device(PCI_ANY_ID, PCI_ANY_ID, dev)) != NULL) {
		pci_read_config_word(dev, PCI_COMMAND, &command);
		for(idx = 0; idx < 6; idx++) {
			r = &dev->resource[idx];
			if (r->parent)		/* Already allocated */
				continue;
			if (!r->start)		/* Address not assigned at all */
				continue;
			if (r->flags & IORESOURCE_IO)
				disabled = !(command & PCI_COMMAND_IO);
			else
				disabled = !(command & PCI_COMMAND_MEMORY);
			if (pass == disabled) {
				DBG("PCI: Resource %08lx-%08lx (f=%lx, d=%d, p=%d)\n",
				    r->start, r->end, r->flags, disabled, pass);
				pr = pci_find_parent_resource(dev, r);
				if (!pr || request_resource(pr, r) < 0) {
					printk(KERN_ERR "PCI: Cannot allocate resource region %d of device %s\n", idx, pci_name(dev));
					/* We'll assign a new address later */
					r->end -= r->start;
					r->start = 0;
				}
			}
		}
		if (!pass) {
			r = &dev->resource[PCI_ROM_RESOURCE];
			if (r->flags & PCI_ROM_ADDRESS_ENABLE) {
				/* Turn the ROM off, leave the resource region, but keep it unregistered. */
				u32 reg;
				DBG("PCI: Switching off ROM of %s\n", pci_name(dev));
				r->flags &= ~PCI_ROM_ADDRESS_ENABLE;
				pci_read_config_dword(dev, dev->rom_base_reg, &reg);
				pci_write_config_dword(dev, dev->rom_base_reg, reg & ~PCI_ROM_ADDRESS_ENABLE);
			}
		}
	}
}

static void __init pcibios_assign_resources(void)
{
	struct pci_dev *dev = NULL;
	int idx;
	struct resource *r;

	while ((dev = pci_find_device(PCI_ANY_ID, PCI_ANY_ID, dev)) != NULL) {
		int class = dev->class >> 8;

		/* Don't touch classless devices and host bridges */
		if (!class || class == PCI_CLASS_BRIDGE_HOST)
			continue;

		for(idx=0; idx<6; idx++) {
			r = &dev->resource[idx];

			/*
			 *  Don't touch IDE controllers and I/O ports of video cards!
			 */
			if ((class == PCI_CLASS_STORAGE_IDE && idx < 4) ||
			    (class == PCI_CLASS_DISPLAY_VGA && (r->flags & IORESOURCE_IO)))
				continue;

			/*
			 *  We shall assign a new address to this resource, either because
			 *  the BIOS forgot to do so or because we have decided the old
			 *  address was unusable for some reason.
			 */
			if (!r->start && r->end)
				pci_assign_resource(dev, idx);
		}

		if (pci_probe & PCI_ASSIGN_ROMS) {
			r = &dev->resource[PCI_ROM_RESOURCE];
			r->end -= r->start;
			r->start = 0;
			if (r->end)
				pci_assign_resource(dev, PCI_ROM_RESOURCE);
		}
	}
}

void __init pcibios_resource_survey(void)
{
	DBG("PCI: Allocating resources\n");
	pcibios_allocate_bus_resources(&pci_root_buses);
	pcibios_allocate_resources(0);
	pcibios_allocate_resources(1);
	pcibios_assign_resources();
}

int pcibios_enable_resources(struct pci_dev *dev, int mask)
{
	u16 cmd, old_cmd;
	int idx;
	struct resource *r;

	pci_read_config_word(dev, PCI_COMMAND, &cmd);
	old_cmd = cmd;
	for(idx=0; idx<6; idx++) {
		/* Only set up the requested stuff */
		if (!(mask & (1<<idx)))
			continue;

		r = &dev->resource[idx];
		if (!r->start && r->end) {
			printk(KERN_ERR "PCI: Device %s not available because of resource collisions\n", pci_name(dev));
			return -EINVAL;
		}
		if (r->flags & IORESOURCE_IO)
			cmd |= PCI_COMMAND_IO;
		if (r->flags & IORESOURCE_MEM)
			cmd |= PCI_COMMAND_MEMORY;
	}
	if (dev->resource[PCI_ROM_RESOURCE].start)
		cmd |= PCI_COMMAND_MEMORY;
	if (cmd != old_cmd) {
		printk("PCI: Enabling device %s (%04x -> %04x)\n", pci_name(dev), old_cmd, cmd);
		pci_write_config_word(dev, PCI_COMMAND, cmd);
	}
	return 0;
}

/*
 *  If we set up a device for bus mastering, we need to check the latency
 *  timer as certain crappy BIOSes forget to set it properly.
 */
unsigned int pcibios_max_latency = 255;

void pcibios_set_master(struct pci_dev *dev)
{
	u8 lat;
	pci_read_config_byte(dev, PCI_LATENCY_TIMER, &lat);
	if (lat < 16)
		lat = (64 <= pcibios_max_latency) ? 64 : pcibios_max_latency;
	else if (lat > pcibios_max_latency)
		lat = pcibios_max_latency;
	else
		return;
	printk(KERN_DEBUG "PCI: Setting latency timer of device %s to %d\n", pci_name(dev), lat);
	pci_write_config_byte(dev, PCI_LATENCY_TIMER, lat);
}

int pci_mmap_page_range(struct pci_dev *dev, struct vm_area_struct *vma,
			enum pci_mmap_state mmap_state, int write_combine)
{
	unsigned long prot;

	/* I/O space cannot be accessed via normal processor loads and
	 * stores on this platform.
	 */
	if (mmap_state == pci_mmap_io)
		return -EINVAL;

	/* Leave vm_pgoff as-is, the PCI space address is the physical
	 * address on this platform.
	 */
	vma->vm_flags |= (VM_SHM | VM_LOCKED | VM_IO);

	prot = pgprot_val(vma->vm_page_prot);
	if (boot_cpu_data.x86 > 3)
		prot |= _PAGE_PCD | _PAGE_PWT;
	vma->vm_page_prot = __pgprot(prot);

	/* Write-combine setting is ignored, it is changed via the mtrr
	 * interfaces on this platform.
	 */
	if (remap_page_range(vma, vma->vm_start, vma->vm_pgoff << PAGE_SHIFT,
			     vma->vm_end - vma->vm_start,
			     vma->vm_page_prot))
		return -EAGAIN;

	return 0;
}
