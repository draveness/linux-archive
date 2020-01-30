/*
 * arch/arm/mach-pxa/time.c
 *
 * Author:	Nicolas Pitre
 * Created:	Jun 15, 2001
 * Copyright:	MontaVista Software Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/time.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/sched.h>

#include <asm/system.h>
#include <asm/hardware.h>
#include <asm/io.h>
#include <asm/leds.h>
#include <asm/irq.h>
#include <asm/mach/irq.h>
#include <asm/mach/time.h>


static inline unsigned long pxa_get_rtc_time(void)
{
	return RCNR;
}

static int pxa_set_rtc(void)
{
	unsigned long current_time = xtime.tv_sec;

	if (RTSR & RTSR_ALE) {
		/* make sure not to forward the clock over an alarm */
		unsigned long alarm = RTAR;
		if (current_time >= alarm && alarm >= RCNR)
			return -ERESTARTSYS;
	}
	RCNR = current_time;
	return 0;
}

/* IRQs are disabled before entering here from do_gettimeofday() */
static unsigned long pxa_gettimeoffset (void)
{
	long ticks_to_match, elapsed, usec;

	/* Get ticks before next timer match */
	ticks_to_match = OSMR0 - OSCR;

	/* We need elapsed ticks since last match */
	elapsed = LATCH - ticks_to_match;

	/* don't get fooled by the workaround in pxa_timer_interrupt() */
	if (elapsed <= 0)
		return 0;

	/* Now convert them to usec */
	usec = (unsigned long)(elapsed * (tick_nsec / 1000))/LATCH;

	return usec;
}

static irqreturn_t
pxa_timer_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	int next_match;

	/* Loop until we get ahead of the free running timer.
	 * This ensures an exact clock tick count and time accuracy.
	 * IRQs are disabled inside the loop to ensure coherence between
	 * lost_ticks (updated in do_timer()) and the match reg value, so we
	 * can use do_gettimeofday() from interrupt handlers.
	 *
	 * HACK ALERT: it seems that the PXA timer regs aren't updated right
	 * away in all cases when a write occurs.  We therefore compare with
	 * 8 instead of 0 in the while() condition below to avoid missing a
	 * match if OSCR has already reached the next OSMR value.
	 * Experience has shown that up to 6 ticks are needed to work around
	 * this problem, but let's use 8 to be conservative.  Note that this
	 * affect things only when the timer IRQ has been delayed by nearly
	 * exactly one tick period which should be a pretty rare event.
	 */
	do {
		timer_tick(regs);
		OSSR = OSSR_M0;  /* Clear match on timer 0 */
		next_match = (OSMR0 += LATCH);
	} while( (signed long)(next_match - OSCR) <= 8 );

	return IRQ_HANDLED;
}

static struct irqaction pxa_timer_irq = {
	.name		= "PXA Timer Tick",
	.flags		= SA_INTERRUPT,
	.handler	= pxa_timer_interrupt
};

void __init pxa_init_time(void)
{
	struct timespec tv;

	gettimeoffset = pxa_gettimeoffset;
	set_rtc = pxa_set_rtc;

	tv.tv_nsec = 0;
	tv.tv_sec = pxa_get_rtc_time();
	do_settimeofday(&tv);

	OSMR0 = 0;		/* set initial match at 0 */
	OSSR = 0xf;		/* clear status on all timers */
	setup_irq(IRQ_OST0, &pxa_timer_irq);
	OIER |= OIER_E0;	/* enable match on timer 0 to cause interrupts */
	OSCR = 0;		/* initialize free-running timer, force first match */
}

