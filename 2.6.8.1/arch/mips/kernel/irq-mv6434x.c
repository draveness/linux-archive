/*
 * Copyright 2002 Momentum Computer
 * Author: mdharm@momenco.com
 * Copyright (C) 2004 Ralf Baechle <ralf@linux-mips.org>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <asm/ptrace.h>
#include <linux/sched.h>
#include <linux/kernel_stat.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/mv64340.h>

static unsigned int irq_base;

static inline int ls1bit32(unsigned int x)
{
        int b = 31, s;

        s = 16; if (x << 16 == 0) s = 0; b -= s; x <<= s;
        s =  8; if (x <<  8 == 0) s = 0; b -= s; x <<= s;
        s =  4; if (x <<  4 == 0) s = 0; b -= s; x <<= s;
        s =  2; if (x <<  2 == 0) s = 0; b -= s; x <<= s;
        s =  1; if (x <<  1 == 0) s = 0; b -= s;

        return b;
}

/* mask off an interrupt -- 1 is enable, 0 is disable */
static inline void mask_mv64340_irq(unsigned int irq)
{
	uint32_t value;

	if (irq < (irq_base + 32)) {
		value = MV_READ(MV64340_INTERRUPT0_MASK_0_LOW);
		value &= ~(1 << (irq - irq_base));
		MV_WRITE(MV64340_INTERRUPT0_MASK_0_LOW, value);
	} else {
		value = MV_READ(MV64340_INTERRUPT0_MASK_0_HIGH);
		value &= ~(1 << (irq - irq_base - 32));
		MV_WRITE(MV64340_INTERRUPT0_MASK_0_HIGH, value);
	}
}

/* unmask an interrupt -- 1 is enable, 0 is disable */
static inline void unmask_mv64340_irq(unsigned int irq)
{
	uint32_t value;

	if (irq < (irq_base + 32)) {
		value = MV_READ(MV64340_INTERRUPT0_MASK_0_LOW);
		value |= 1 << (irq - irq_base);
		MV_WRITE(MV64340_INTERRUPT0_MASK_0_LOW, value);
	} else {
		value = MV_READ(MV64340_INTERRUPT0_MASK_0_HIGH);
		value |= 1 << (irq - irq_base - 32);
		MV_WRITE(MV64340_INTERRUPT0_MASK_0_HIGH, value);
	}
}

/*
 * Enables the IRQ on Marvell Chip
 */
static void enable_mv64340_irq(unsigned int irq)
{
	unmask_mv64340_irq(irq);
}

/*
 * Initialize the IRQ on Marvell Chip
 */
static unsigned int startup_mv64340_irq(unsigned int irq)
{
	unmask_mv64340_irq(irq);
	return 0;
}

/*
 * Disables the IRQ on Marvell Chip
 */
static void disable_mv64340_irq(unsigned int irq)
{
	mask_mv64340_irq(irq);
}

/*
 * Masks and ACKs an IRQ
 */
static void mask_and_ack_mv64340_irq(unsigned int irq)
{
	mask_mv64340_irq(irq);
}

/*
 * End IRQ processing
 */
static void end_mv64340_irq(unsigned int irq)
{
	if (!(irq_desc[irq].status & (IRQ_DISABLED|IRQ_INPROGRESS)))
		unmask_mv64340_irq(irq);
}

/*
 * Interrupt handler for interrupts coming from the Marvell chip.
 * It could be built in ethernet ports etc...
 */
void ll_mv64340_irq(struct pt_regs *regs)
{
	unsigned int irq_src_low, irq_src_high;
 	unsigned int irq_mask_low, irq_mask_high;

	/* read the interrupt status registers */
	irq_mask_low = MV_READ(MV64340_INTERRUPT0_MASK_0_LOW);
	irq_mask_high = MV_READ(MV64340_INTERRUPT0_MASK_0_HIGH);
	irq_src_low = MV_READ(MV64340_MAIN_INTERRUPT_CAUSE_LOW);
	irq_src_high = MV_READ(MV64340_MAIN_INTERRUPT_CAUSE_HIGH);

	/* mask for just the interrupts we want */
	irq_src_low &= irq_mask_low;
	irq_src_high &= irq_mask_high;

	if (irq_src_low)
		do_IRQ(ls1bit32(irq_src_low) + irq_base, regs);
	else
		do_IRQ(ls1bit32(irq_src_high) + irq_base + 32, regs);
}

#define shutdown_mv64340_irq	disable_mv64340_irq

struct hw_interrupt_type mv64340_irq_type = {
	"MV-64340",
	startup_mv64340_irq,
	shutdown_mv64340_irq,
	enable_mv64340_irq,
	disable_mv64340_irq,
	mask_and_ack_mv64340_irq,
	end_mv64340_irq,
	NULL
};

void __init mv64340_irq_init(unsigned int base)
{
	int i;

	/* Reset irq handlers pointers to NULL */
	for (i = base; i < base + 64; i++) {
		irq_desc[i].status = IRQ_DISABLED;
		irq_desc[i].action = 0;
		irq_desc[i].depth = 2;
		irq_desc[i].handler = &mv64340_irq_type;
	}

	irq_base = base;
}
