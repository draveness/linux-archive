/*
 *  linux/arch/arm/mach-ebsa110/core.c
 *
 *  Copyright (C) 1998-2001 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  Extra MM routines for the EBSA-110 architecture
 */
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/init.h>

#include <asm/hardware.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/setup.h>
#include <asm/mach-types.h>
#include <asm/pgtable.h>
#include <asm/page.h>
#include <asm/system.h>

#include <asm/mach/arch.h>
#include <asm/mach/irq.h>
#include <asm/mach/map.h>

#include <asm/mach/time.h>

#define IRQ_MASK		0xfe000000	/* read */
#define IRQ_MSET		0xfe000000	/* write */
#define IRQ_STAT		0xff000000	/* read */
#define IRQ_MCLR		0xff000000	/* write */

static void ebsa110_mask_irq(unsigned int irq)
{
	__raw_writeb(1 << irq, IRQ_MCLR);
}

static void ebsa110_unmask_irq(unsigned int irq)
{
	__raw_writeb(1 << irq, IRQ_MSET);
}

static struct irqchip ebsa110_irq_chip = {
	.ack	= ebsa110_mask_irq,
	.mask	= ebsa110_mask_irq,
	.unmask = ebsa110_unmask_irq,
};
 
static void __init ebsa110_init_irq(void)
{
	unsigned long flags;
	unsigned int irq;

	local_irq_save(flags);
	__raw_writeb(0xff, IRQ_MCLR);
	__raw_writeb(0x55, IRQ_MSET);
	__raw_writeb(0x00, IRQ_MSET);
	if (__raw_readb(IRQ_MASK) != 0x55)
		while (1);
	__raw_writeb(0xff, IRQ_MCLR);	/* clear all interrupt enables */
	local_irq_restore(flags);

	for (irq = 0; irq < NR_IRQS; irq++) {
		set_irq_chip(irq, &ebsa110_irq_chip);
		set_irq_handler(irq, do_level_IRQ);
		set_irq_flags(irq, IRQF_VALID | IRQF_PROBE);
	}
}

static struct map_desc ebsa110_io_desc[] __initdata = {
	/*
	 * sparse external-decode ISAIO space
	 */
	{ IRQ_STAT,    TRICK4_PHYS, PGDIR_SIZE,  MT_DEVICE }, /* IRQ_STAT/IRQ_MCLR */
	{ IRQ_MASK,    TRICK3_PHYS, PGDIR_SIZE,  MT_DEVICE }, /* IRQ_MASK/IRQ_MSET */
	{ SOFT_BASE,   TRICK1_PHYS, PGDIR_SIZE,  MT_DEVICE }, /* SOFT_BASE */
	{ PIT_BASE,    TRICK0_PHYS, PGDIR_SIZE,  MT_DEVICE }, /* PIT_BASE */

	/*
	 * self-decode ISAIO space
	 */
	{ ISAIO_BASE,  ISAIO_PHYS,  ISAIO_SIZE,  MT_DEVICE },
	{ ISAMEM_BASE, ISAMEM_PHYS, ISAMEM_SIZE, MT_DEVICE }
};

static void __init ebsa110_map_io(void)
{
	iotable_init(ebsa110_io_desc, ARRAY_SIZE(ebsa110_io_desc));
}


#define PIT_CTRL		(PIT_BASE + 0x0d)
#define PIT_T2			(PIT_BASE + 0x09)
#define PIT_T1			(PIT_BASE + 0x05)
#define PIT_T0			(PIT_BASE + 0x01)

/*
 * This is the rate at which your MCLK signal toggles (in Hz)
 * This was measured on a 10 digit frequency counter sampling
 * over 1 second.
 */
#define MCLK	47894000

/*
 * This is the rate at which the PIT timers get clocked
 */
#define CLKBY7	(MCLK / 7)

/*
 * This is the counter value.  We tick at 200Hz on this platform.
 */
#define COUNT	((CLKBY7 + (HZ / 2)) / HZ)

/*
 * Get the time offset from the system PIT.  Note that if we have missed an
 * interrupt, then the PIT counter will roll over (ie, be negative).
 * This actually works out to be convenient.
 */
static unsigned long ebsa110_gettimeoffset(void)
{
	unsigned long offset, count;

	__raw_writeb(0x40, PIT_CTRL);
	count = __raw_readb(PIT_T1);
	count |= __raw_readb(PIT_T1) << 8;

	/*
	 * If count > COUNT, make the number negative.
	 */
	if (count > COUNT)
		count |= 0xffff0000;

	offset = COUNT;
	offset -= count;

	/*
	 * `offset' is in units of timer counts.  Convert
	 * offset to units of microseconds.
	 */
	offset = offset * (1000000 / HZ) / COUNT;

	return offset;
}

static irqreturn_t
ebsa110_timer_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	u32 count;

	/* latch and read timer 1 */
	__raw_writeb(0x40, PIT_CTRL);
	count = __raw_readb(PIT_T1);
	count |= __raw_readb(PIT_T1) << 8;

	count += COUNT;

	__raw_writeb(count & 0xff, PIT_T1);
	__raw_writeb(count >> 8, PIT_T1);

	timer_tick(regs);

	return IRQ_HANDLED;
}

static struct irqaction ebsa110_timer_irq = {
	.name		= "EBSA110 Timer Tick",
	.flags		= SA_INTERRUPT,
	.handler	= ebsa110_timer_interrupt
};

/*
 * Set up timer interrupt.
 */
static void __init ebsa110_init_time(void)
{
	/*
	 * Timer 1, mode 2, LSB/MSB
	 */
	__raw_writeb(0x70, PIT_CTRL);
	__raw_writeb(COUNT & 0xff, PIT_T1);
	__raw_writeb(COUNT >> 8, PIT_T1);

	gettimeoffset = ebsa110_gettimeoffset;

	setup_irq(IRQ_EBSA110_TIMER0, &ebsa110_timer_irq);
}

MACHINE_START(EBSA110, "EBSA110")
	MAINTAINER("Russell King")
	BOOT_MEM(0x00000000, 0xe0000000, 0xe0000000)
	BOOT_PARAMS(0x00000400)
	DISABLE_PARPORT(0)
	DISABLE_PARPORT(2)
	SOFT_REBOOT
	MAPIO(ebsa110_map_io)
	INITIRQ(ebsa110_init_irq)
	INITTIME(ebsa110_init_time)
MACHINE_END
