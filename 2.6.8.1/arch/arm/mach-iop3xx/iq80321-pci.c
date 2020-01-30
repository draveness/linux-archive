/*
 * arch/arm/mach-iop3xx/iq80321-pci.c
 *
 * PCI support for the Intel IQ80321 reference board
 *
 * Author: Rory Bolt <rorybolt@pacbell.net>
 * Copyright (C) 2002 Rory Bolt
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/init.h>

#include <asm/hardware.h>
#include <asm/irq.h>
#include <asm/mach/pci.h>
#include <asm/mach-types.h>

/*
 * The following macro is used to lookup irqs in a standard table
 * format for those systems that do not already have PCI
 * interrupts properly routed.  We assume 1 <= pin <= 4
 */
#define PCI_IRQ_TABLE_LOOKUP(minid,maxid)	\
({ int _ctl_ = -1;				\
   unsigned int _idsel = idsel - minid;		\
   if (_idsel <= maxid)				\
      _ctl_ = pci_irq_table[_idsel][pin-1];	\
   _ctl_; })

#define INTA	IRQ_IQ80321_INTA
#define INTB	IRQ_IQ80321_INTB
#define INTC	IRQ_IQ80321_INTC
#define INTD	IRQ_IQ80321_INTD

#define INTE	IRQ_IQ80321_I82544

typedef u8 irq_table[4];

static irq_table pci_irq_table[] = {
	/*
	 * PCI IDSEL/INTPIN->INTLINE
	 * A       B       C       D
	 */
	{INTE, INTE, INTE, INTE}, /* Gig-E */
	{INTD, INTC, INTD, INTA}, /* Unused */
	{INTC, INTD, INTA, INTB}, /* PCI-X Slot */
};

static inline int __init
iq80321_map_irq(struct pci_dev *dev, u8 idsel, u8 pin)
{
	BUG_ON(pin < 1 || pin > 4);

	return PCI_IRQ_TABLE_LOOKUP(2, 3);
}

static int iq80321_setup(int nr, struct pci_sys_data *sys)
{
	switch (nr) {
	case 0:
		sys->map_irq = iq80321_map_irq;
		break;
	default:
		return 0;
	}

	return iop321_setup(nr, sys);
}

static void iq80321_preinit(void)
{
	iop321_init();
}

static struct hw_pci iq80321_pci __initdata = {
	.swizzle	= pci_std_swizzle,
	.nr_controllers = 1,
	.setup		= iq80321_setup,
	.scan		= iop321_scan_bus,
	.preinit	= iq80321_preinit,
};

static int __init iq80321_pci_init(void)
{
	if (machine_is_iq80321())
		pci_common_init(&iq80321_pci);
	return 0;
}

subsys_initcall(iq80321_pci_init);




