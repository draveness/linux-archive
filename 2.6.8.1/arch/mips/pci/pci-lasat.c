/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2000, 2001 Keith M Wesolowski
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <asm/pci_channel.h>
#include <linux/delay.h>
#include <asm/bootinfo.h>

extern struct pci_ops nile4_pci_ops;
extern struct pci_ops gt64120_pci_ops;
static struct resource lasat_pci_mem_resource = {
	.name	= "LASAT PCI MEM",
	.start	= 0x18000000,
	.end	= 0x19FFFFFF,
	.flags	= IORESOURCE_MEM,
};

static struct resource lasat_pci_io_resource = {
	.name	= "LASAT PCI IO",
	.start	= 0x1a000000,
	.end	= 0x1bFFFFFF,
	.flags	= IORESOURCE_IO,
};

static struct pci_controller lasat_pci_controller = {
	.mem_resource	= &lasat_pci_mem_resource,
	.io_resource	= &lasat_pci_io_resource,
};

static int __init lasat_pci_setup(void)
{
 	printk("PCI: starting\n");

        switch (mips_machtype) {
            case MACH_LASAT_100:
                lasat_pci_controller.pci_ops = &gt64120_pci_ops;
                break;
            case MACH_LASAT_200:
                lasat_pci_controller.pci_ops = &nile4_pci_ops;
                break;
            default:
                panic("pcibios_init: mips_machtype incorrect");
        }

	register_pci_controller(&lasat_pci_controller);
        return 0;
}
early_initcall(lasat_pci_setup);

#define LASATINT_ETH1   0
#define LASATINT_ETH0   1
#define LASATINT_HDC    2
#define LASATINT_COMP   3
#define LASATINT_HDLC   4
#define LASATINT_PCIA   5
#define LASATINT_PCIB   6
#define LASATINT_PCIC   7
#define LASATINT_PCID   8
int __init pcibios_map_irq(struct pci_dev *dev, u8 slot, u8 pin)
{
    switch (slot) {
        case 1:
            return LASATINT_PCIA;   /* Expansion Module 0 */
        case 2:
            return LASATINT_PCIB;   /* Expansion Module 1 */
        case 3:
            return LASATINT_PCIC;   /* Expansion Module 2 */
        case 4:
            return LASATINT_ETH1;   /* Ethernet 1 (LAN 2) */
        case 5:
            return LASATINT_ETH0;   /* Ethernet 0 (LAN 1) */
        case 6:
            return LASATINT_HDC;    /* IDE controller */
        default:
            return 0xff;            /* Illegal */
    }

    return -1;
}
