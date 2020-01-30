/*
 * arch/sh/drivers/pci/ops-dreamcast.c
 *
 * PCI operations for the Sega Dreamcast
 *
 * Copyright (C) 2001, 2002  M. R. Brown
 * Copyright (C) 2002, 2003  Paul Mundt
 *
 * This file originally bore the message (with enclosed-$):
 *	Id: pci.c,v 1.3 2003/05/04 19:29:46 lethal Exp
 *	Dreamcast PCI: Supports SEGA Broadband Adaptor only.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/param.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/pci.h>

#include <asm/io.h>
#include <asm/irq.h>
#include <asm/mach/pci.h>

static struct resource gapspci_io_resource = {
	.name	= "GAPSPCI IO",
	.start	= GAPSPCI_BBA_CONFIG,
	.end	= GAPSPCI_BBA_CONFIG + GAPSPCI_BBA_CONFIG_SIZE - 1,
	.flags	= IORESOURCE_IO,
};

static struct resource gapspci_mem_resource = {
	.name	= "GAPSPCI mem",
	.start	= GAPSPCI_DMA_BASE,
	.end	= GAPSPCI_DMA_BASE + GAPSPCI_DMA_SIZE - 1,
	.flags	= IORESOURCE_MEM,
};

static struct pci_ops gapspci_pci_ops;

struct pci_channel board_pci_channels[] = {
	{ &gapspci_pci_ops, &gapspci_io_resource,
	  &gapspci_mem_resource, 0, 1 },
	{ 0, }
};

/*
 * The !gapspci_config_access case really shouldn't happen, ever, unless
 * someone implicitly messes around with the last devfn value.. otherwise we
 * only support a single device anyways, and if we didn't have a BBA, we
 * wouldn't make it terribly far through the PCI setup anyways.
 *
 * Also, we could very easily support both Type 0 and Type 1 configurations
 * here, but since it doesn't seem that there is any such implementation in
 * existence, we don't bother.
 *
 * I suppose if someone actually gets around to ripping the chip out of
 * the BBA and hanging some more devices off of it, then this might be
 * something to take into consideration. However, due to the cost of the BBA,
 * and the general lack of activity by DC hardware hackers, this doesn't seem
 * likely to happen anytime soon.
 */
static int gapspci_config_access(unsigned char bus, unsigned int devfn)
{
	return (bus == 0) && (devfn == 0);
}

/*
 * We can also actually read and write in b/w/l sizes! Thankfully this part
 * was at least done right, and we don't have to do the stupid masking and
 * shifting that we do on the 7751! Small wonders never cease to amaze.
 */
static int gapspci_read(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 *val)
{
	*val = 0xffffffff;

	if (!gapspci_config_access(bus->number, devfn))
		return PCIBIOS_DEVICE_NOT_FOUND;

	switch (size) {
		case 1: *val = inb(GAPSPCI_BBA_CONFIG+where); break;
		case 2: *val = inw(GAPSPCI_BBA_CONFIG+where); break;
		case 4: *val = inl(GAPSPCI_BBA_CONFIG+where); break;
	}	

        return PCIBIOS_SUCCESSFUL;
}

static int gapspci_write(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 val)
{
	if (!gapspci_config_access(bus->number, devfn))
		return PCIBIOS_DEVICE_NOT_FOUND;

	switch (size) {
		case 1: outb(( u8)val, GAPSPCI_BBA_CONFIG+where); break;
		case 2: outw((u16)val, GAPSPCI_BBA_CONFIG+where); break;
		case 4: outl((u32)val, GAPSPCI_BBA_CONFIG+where); break;
	}

        return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops gapspci_pci_ops = {
	.read	= gapspci_read,
	.write	= gapspci_write,
};

/*
 * gapspci init
 */

int __init gapspci_init(void)
{
	char idbuf[16];
	int i;

	/*
	 * FIXME: All of this wants documenting to some degree,
	 * even some basic register definitions would be nice.
	 *
	 * I haven't seen anything this ugly since.. maple.
	 */

	for (i=0; i<16; i++)
		idbuf[i] = inb(GAPSPCI_REGS+i);

	if (strncmp(idbuf, "GAPSPCI_BRIDGE_2", 16))
		return -ENODEV;

	outl(0x5a14a501, GAPSPCI_REGS+0x18);

	for (i=0; i<1000000; i++)
		;

	if (inl(GAPSPCI_REGS+0x18) != 1)
		return -EINVAL;

	outl(0x01000000, GAPSPCI_REGS+0x20);
	outl(0x01000000, GAPSPCI_REGS+0x24);

	outl(GAPSPCI_DMA_BASE, GAPSPCI_REGS+0x28);
	outl(GAPSPCI_DMA_BASE+GAPSPCI_DMA_SIZE, GAPSPCI_REGS+0x2c);

	outl(1, GAPSPCI_REGS+0x14);
	outl(1, GAPSPCI_REGS+0x34);

	/* Setting Broadband Adapter */
	outw(0xf900, GAPSPCI_BBA_CONFIG+0x06);
	outl(0x00000000, GAPSPCI_BBA_CONFIG+0x30);
	outb(0x00, GAPSPCI_BBA_CONFIG+0x3c);
	outb(0xf0, GAPSPCI_BBA_CONFIG+0x0d);
	outw(0x0006, GAPSPCI_BBA_CONFIG+0x04);
	outl(0x00002001, GAPSPCI_BBA_CONFIG+0x10);
	outl(0x01000000, GAPSPCI_BBA_CONFIG+0x14);

	return 0;
}

/* Haven't done anything here as yet */
char * __devinit pcibios_setup(char *str)
{
	return str;
}
