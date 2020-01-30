/* arch/arm/mach-lh7a40x/arch-kev7a400.c
 *
 *  Copyright (C) 2004 Logic Product Development
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  version 2 as published by the Free Software Foundation.
 *
 */

#include <linux/tty.h>
#include <linux/init.h>
#include <linux/device.h>

#include <asm/hardware.h>
#include <asm/setup.h>
#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/hardware.h>	/* io_p2v() */
#include <asm/irq.h>
#include <asm/mach/irq.h>
#include <asm/mach/map.h>

#include <linux/interrupt.h>

      /* This function calls the board specific IRQ initialization function. */
extern void lh7a400_init_irq (void);
extern void lh7a40x_init_time (void);

static struct map_desc kev7a400_io_desc[] __initdata = {
	{ IO_VIRT,    IO_PHYS,    IO_SIZE,    MT_DEVICE },
	{ CPLD_VIRT,  CPLD_PHYS,  CPLD_SIZE,  MT_DEVICE },
};

void __init kev7a400_map_io(void)
{
	iotable_init (kev7a400_io_desc, ARRAY_SIZE (kev7a400_io_desc));
}

static u16 CPLD_IRQ_mask;	/* Mask for CPLD IRQs, 1 == unmasked */

static void kev7a400_ack_cpld_irq (u32 irq)
{
	CPLD_CL_INT = 1 << (irq - IRQ_KEV7A400_CPLD);
}

static void kev7a400_mask_cpld_irq (u32 irq)
{
	CPLD_IRQ_mask &= ~(1 << (irq - IRQ_KEV7A400_CPLD));
	CPLD_WR_PB_INT_MASK = CPLD_IRQ_mask;
}

static void kev7a400_unmask_cpld_irq (u32 irq)
{
	CPLD_IRQ_mask |= 1 << (irq - IRQ_KEV7A400_CPLD);
	CPLD_WR_PB_INT_MASK = CPLD_IRQ_mask;
}

static struct irqchip kev7a400_cpld_chip = {
	.ack	= kev7a400_ack_cpld_irq,
	.mask	= kev7a400_mask_cpld_irq,
	.unmask	= kev7a400_unmask_cpld_irq,
};


static void kev7a400_cpld_handler (unsigned int irq, struct irqdesc *desc,
				  struct pt_regs *regs)
{
	u32 mask = CPLD_LATCHED_INTS;
	irq = IRQ_KEV7A400_CPLD;
	for (; mask; mask >>= 1, ++irq) {
		if (mask & 1)
			desc[irq].handle (irq, desc, regs);
	}
}

void __init lh7a40x_init_board_irq (void)
{
	int irq;

	for (irq = IRQ_KEV7A400_CPLD;
	     irq < IRQ_KEV7A400_CPLD + NR_IRQ_BOARD; ++irq) {
		set_irq_chip (irq, &kev7a400_cpld_chip);
		set_irq_handler (irq, do_edge_IRQ);
		set_irq_flags (irq, IRQF_VALID);
	}
	set_irq_chained_handler (IRQ_CPLD, kev7a400_cpld_handler);

		/* Clear all CPLD interrupts */
	CPLD_CL_INT = 0xff; /* CPLD_INTR_MMC_CD | CPLD_INTR_ETH_INT; */

	GPIO_GPIOINTEN = 0;		/* Disable all GPIO interrupts */
	barrier();

#if 0
	GPIO_INTTYPE1
		= (GPIO_INTR_PCC1_CD | GPIO_INTR_PCC1_CD); /* Edge trig. */
	GPIO_INTTYPE2 = 0;		/* Falling edge & low-level */
	GPIO_GPIOFEOI = 0xff;		/* Clear all GPIO interrupts */
	GPIO_GPIOINTEN = 0xff;		/* Enable all GPIO interrupts */

	init_FIQ();
#endif
}

MACHINE_START (KEV7A400, "Sharp KEV7a400")
	MAINTAINER ("Marc Singer")
	BOOT_MEM (0xc0000000, 0x80000000, io_p2v (0x80000000))
	BOOT_PARAMS (0xc0000100)
	MAPIO (kev7a400_map_io)
	INITIRQ (lh7a400_init_irq)
	INITTIME (lh7a40x_init_time)
MACHINE_END
