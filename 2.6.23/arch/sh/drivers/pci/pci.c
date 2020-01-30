/*
 * arch/sh/drivers/pci/pci.c
 *
 * Copyright (c) 2002 M. R. Brown  <mrbrown@linux-sh.org>
 * Copyright (c) 2004 - 2006 Paul Mundt  <lethal@linux-sh.org>
 *
 * These functions are collected here to reduce duplication of common
 * code amongst the many platform-specific PCI support code files.
 *
 * These routines require the following board-specific routines:
 * void pcibios_fixup_irqs();
 *
 * See include/asm-sh/pci.h for more information.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <asm/io.h>

static inline u8 bridge_swizzle(u8 pin, u8 slot)
{
	return (((pin - 1) + slot) % 4) + 1;
}

static u8 __init simple_swizzle(struct pci_dev *dev, u8 *pinp)
{
	u8 pin = *pinp;

	while (dev->bus->parent) {
		pin = bridge_swizzle(pin, PCI_SLOT(dev->devfn));
		/* Move up the chain of bridges. */
		dev = dev->bus->self;
	}
	*pinp = pin;

	/* The slot is the slot of the last bridge. */
	return PCI_SLOT(dev->devfn);
}

static int __init pcibios_init(void)
{
	struct pci_channel *p;
	struct pci_bus *bus;
	int busno;

#ifdef CONFIG_PCI_AUTO
	/* assign resources */
	busno = 0;
	for (p = board_pci_channels; p->pci_ops != NULL; p++)
		busno = pciauto_assign_resources(busno, p) + 1;
#endif

	/* scan the buses */
	busno = 0;
	for (p = board_pci_channels; p->pci_ops != NULL; p++) {
		bus = pci_scan_bus(busno, p->pci_ops, p);
		busno = bus->subordinate + 1;
	}

	pci_fixup_irqs(simple_swizzle, pcibios_map_platform_irq);

	return 0;
}
subsys_initcall(pcibios_init);

/*
 *  Called after each bus is probed, but before its children
 *  are examined.
 */
void __devinit pcibios_fixup_bus(struct pci_bus *bus)
{
	pci_read_bridge_bases(bus);
}

void
pcibios_update_resource(struct pci_dev *dev, struct resource *root,
			struct resource *res, int resource)
{
	u32 new, check;
	int reg;

	new = res->start | (res->flags & PCI_REGION_FLAG_MASK);
	if (resource < 6) {
		reg = PCI_BASE_ADDRESS_0 + 4*resource;
	} else if (resource == PCI_ROM_RESOURCE) {
		res->flags |= IORESOURCE_ROM_ENABLE;
		new |= PCI_ROM_ADDRESS_ENABLE;
		reg = dev->rom_base_reg;
	} else {
		/*
		 * Somebody might have asked allocation of a non-standard
		 * resource
		 */
		return;
	}

	pci_write_config_dword(dev, reg, new);
	pci_read_config_dword(dev, reg, &check);
	if ((new ^ check) & ((new & PCI_BASE_ADDRESS_SPACE_IO) ?
		PCI_BASE_ADDRESS_IO_MASK : PCI_BASE_ADDRESS_MEM_MASK)) {
		printk(KERN_ERR "PCI: Error while updating region "
		       "%s/%d (%08x != %08x)\n", pci_name(dev), resource,
		       new, check);
	}
}

void pcibios_align_resource(void *data, struct resource *res,
			    resource_size_t size, resource_size_t align)
			    __attribute__ ((weak));

/*
 * We need to avoid collisions with `mirrored' VGA ports
 * and other strange ISA hardware, so we always want the
 * addresses to be allocated in the 0x000-0x0ff region
 * modulo 0x400.
 */
void pcibios_align_resource(void *data, struct resource *res,
			    resource_size_t size, resource_size_t align)
{
	if (res->flags & IORESOURCE_IO) {
		resource_size_t start = res->start;

		if (start & 0x300) {
			start = (start + 0x3ff) & ~0x3ff;
			res->start = start;
		}
	}
}

int pcibios_enable_device(struct pci_dev *dev, int mask)
{
	u16 cmd, old_cmd;
	int idx;
	struct resource *r;

	pci_read_config_word(dev, PCI_COMMAND, &cmd);
	old_cmd = cmd;
	for(idx=0; idx<6; idx++) {
		if (!(mask & (1 << idx)))
			continue;
		r = &dev->resource[idx];
		if (!r->start && r->end) {
			printk(KERN_ERR "PCI: Device %s not available because "
			       "of resource collisions\n", pci_name(dev));
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
		printk(KERN_INFO "PCI: Enabling device %s (%04x -> %04x)\n",
		       pci_name(dev), old_cmd, cmd);
		pci_write_config_word(dev, PCI_COMMAND, cmd);
	}
	return 0;
}

/*
 *  If we set up a device for bus mastering, we need to check and set
 *  the latency timer as it may not be properly set.
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
	printk(KERN_INFO "PCI: Setting latency timer of device %s to %d\n",
	       pci_name(dev), lat);
	pci_write_config_byte(dev, PCI_LATENCY_TIMER, lat);
}

void __init pcibios_update_irq(struct pci_dev *dev, int irq)
{
	pci_write_config_byte(dev, PCI_INTERRUPT_LINE, irq);
}

void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long maxlen)
{
	unsigned long start = pci_resource_start(dev, bar);
	unsigned long len = pci_resource_len(dev, bar);
	unsigned long flags = pci_resource_flags(dev, bar);

	if (unlikely(!len || !start))
		return NULL;
	if (maxlen && len > maxlen)
		len = maxlen;

	/*
	 * Presently the IORESOURCE_MEM case is a bit special, most
	 * SH7751 style PCI controllers have PCI memory at a fixed
	 * location in the address space where no remapping is desired
	 * (typically at 0xfd000000, but is_pci_memaddr() will know
	 * best). With the IORESOURCE_MEM case more care has to be taken
	 * to inhibit page table mapping for legacy cores, but this is
	 * punted off to __ioremap().
	 *					-- PFM.
	 */
	if (flags & IORESOURCE_IO)
		return ioport_map(start, len);
	if (flags & IORESOURCE_MEM)
		return ioremap(start, len);

	return NULL;
}
EXPORT_SYMBOL(pci_iomap);

void pci_iounmap(struct pci_dev *dev, void __iomem *addr)
{
	iounmap(addr);
}
EXPORT_SYMBOL(pci_iounmap);
