/*
 * Copyright 2001 MontaVista Software Inc.
 * Author: Jun Sun, jsun@mvista.com or jsun@junsun.net
 *
 * Copyright (C) 2001 Ralf Baechle
 *
 * This file define the irq handler for MIPS CPU interrupts.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

/*
 * Almost all MIPS CPUs define 8 interrupt sources.  They are typically
 * level triggered (i.e., cannot be cleared from CPU; must be cleared from
 * device).  The first two are software interrupts which we don't really
 * use or support.  The last one is usually the CPU timer interrupt if
 * counter register is present or, for CPUs with an external FPU, by
 * convention it's the FPU exception interrupt.
 *
 * Don't even think about using this on SMP.  You have been warned.
 *
 * This file exports one global function:
 *	void mips_cpu_irq_init(int irq_base);
 */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>

#include <asm/irq_cpu.h>
#include <asm/mipsregs.h>
#include <asm/system.h>

static int mips_cpu_irq_base;

static inline void unmask_mips_irq(unsigned int irq)
{
	clear_c0_cause(0x100 << (irq - mips_cpu_irq_base));
	set_c0_status(0x100 << (irq - mips_cpu_irq_base));
}

static inline void mask_mips_irq(unsigned int irq)
{
	clear_c0_status(0x100 << (irq - mips_cpu_irq_base));
}

static inline void mips_cpu_irq_enable(unsigned int irq)
{
	unsigned long flags;

	local_irq_save(flags);
	unmask_mips_irq(irq);
	local_irq_restore(flags);
}

static void mips_cpu_irq_disable(unsigned int irq)
{
	unsigned long flags;

	local_irq_save(flags);
	mask_mips_irq(irq);
	local_irq_restore(flags);
}

static unsigned int mips_cpu_irq_startup(unsigned int irq)
{
	mips_cpu_irq_enable(irq);

	return 0;
}

#define	mips_cpu_irq_shutdown	mips_cpu_irq_disable

/*
 * While we ack the interrupt interrupts are disabled and thus we don't need
 * to deal with concurrency issues.  Same for mips_cpu_irq_end.
 */
static void mips_cpu_irq_ack(unsigned int irq)
{
	/* Only necessary for soft interrupts */
	clear_c0_cause(0x100 << (irq - mips_cpu_irq_base));

	mask_mips_irq(irq);
}

static void mips_cpu_irq_end(unsigned int irq)
{
	if (!(irq_desc[irq].status & (IRQ_DISABLED | IRQ_INPROGRESS)))
		unmask_mips_irq(irq);
}

static hw_irq_controller mips_cpu_irq_controller = {
	"MIPS",
	mips_cpu_irq_startup,
	mips_cpu_irq_shutdown,
	mips_cpu_irq_enable,
	mips_cpu_irq_disable,
	mips_cpu_irq_ack,
	mips_cpu_irq_end,
	NULL			/* no affinity stuff for UP */
};


void __init mips_cpu_irq_init(int irq_base)
{
	int i;

	for (i = irq_base; i < irq_base + 8; i++) {
		irq_desc[i].status = IRQ_DISABLED;
		irq_desc[i].action = NULL;
		irq_desc[i].depth = 1;
		irq_desc[i].handler = &mips_cpu_irq_controller;
	}

	mips_cpu_irq_base = irq_base;
}
