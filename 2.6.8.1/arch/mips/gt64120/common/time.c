/*
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * Galileo Technology chip interrupt handler
 */
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel_stat.h>
#include <asm/ptrace.h>
#include <asm/gt64120.h>

/*
 * These are interrupt handlers for the GT on-chip interrupts.  They all come
 * in to the MIPS on a single interrupt line, and have to be handled and ack'ed
 * differently than other MIPS interrupts.
 */

static void gt64120_irq(int irq, void *dev_id, struct pt_regs *regs)
{
	unsigned int irq_src, int_high_src, irq_src_mask, int_high_src_mask;
	int handled = 0;

	irq_src = GT_READ(GT_INTRCAUSE_OFS);
	irq_src_mask = GT_READ(GT_INTRMASK_OFS);
	int_high_src = GT_READ(GT_HINTRCAUSE_OFS);
	int_high_src_mask = GT_READ(GT_HINTRMASK_OFS);
	irq_src = irq_src & irq_src_mask;
	int_high_src = int_high_src & int_high_src_mask;

	if (irq_src & 0x00000800) {	/* Check for timer interrupt */
		handled = 1;
		irq_src &= ~0x00000800;
		do_timer(regs);
	}

	GT_WRITE(GT_INTRCAUSE_OFS, 0);
	GT_WRITE(GT_HINTRCAUSE_OFS, 0);
}

/*
 * Initializes timer using galileo's built in timer.
 */
#ifdef CONFIG_SYSCLK_100
#define Sys_clock (100 * 1000000)	// 100 MHz
#endif
#ifdef CONFIG_SYSCLK_83
#define Sys_clock (83.333 * 1000000)	// 83.333 MHz
#endif
#ifdef CONFIG_SYSCLK_75
#define Sys_clock (75 * 1000000)	// 75 MHz
#endif

/*
 * This will ignore the standard MIPS timer interrupt handler that is passed in
 * as *irq (=irq0 in ../kernel/time.c).  We will do our own timer interrupt
 * handling.
 */
void gt64120_time_init(void)
{
	extern irq_desc_t irq_desc[NR_IRQS];
	static struct irqaction timer;

	/* Disable timer first */
	GT_WRITE(GT_TC_CONTROL_OFS, 0);
	/* Load timer value for 100 Hz */
	GT_WRITE(GT_TC3_OFS, Sys_clock / 100);

	/*
	 * Create the IRQ structure entry for the timer.  Since we're too early
	 * in the boot process to use the "request_irq()" call, we'll hard-code
	 * the values to the correct interrupt line.
	 */
	timer.handler = gt64120_irq;
	timer.flags = SA_SHIRQ | SA_INTERRUPT;
	timer.name = "timer";
	timer.dev_id = NULL;
	timer.next = NULL;
	timer.mask = CPU_MASK_NONE;
	irq_desc[GT_TIMER].action = &timer;

	enable_irq(GT_TIMER);

	/* Enable timer ints */
	GT_WRITE(GT_TC_CONTROL_OFS, 0xc0);
	/* clear Cause register first */
	GT_WRITE(GT_INTRCAUSE_OFS, 0x0);
	/* Unmask timer int */
	GT_WRITE(GT_INTRMASK_OFS, 0x800);
	/* Clear High int register */
	GT_WRITE(GT_HINTRCAUSE_OFS, 0x0);
	/* Mask All interrupts at High cause interrupt */
	GT_WRITE(GT_HINTRMASK_OFS, 0x0);
}
