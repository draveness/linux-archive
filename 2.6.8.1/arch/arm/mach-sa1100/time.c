/*
 * linux/arch/arm/mach-sa1100/time.c
 *
 * Copyright (C) 1998 Deborah Wallach.
 * Twiddles  (C) 1999 	Hugo Fiennes <hugo@empeg.com>
 * 
 * 2000/03/29 (C) Nicolas Pitre <nico@cam.org>
 *	Rewritten: big cleanup, much simpler, better HZ accuracy.
 *
 */
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/timex.h>
#include <linux/signal.h>

#include <asm/mach/time.h>
#include <asm/hardware.h>

#define RTC_DEF_DIVIDER		(32768 - 1)
#define RTC_DEF_TRIM            0

static unsigned long __init sa1100_get_rtc_time(void)
{
	/*
	 * According to the manual we should be able to let RTTR be zero
	 * and then a default diviser for a 32.768KHz clock is used.
	 * Apparently this doesn't work, at least for my SA1110 rev 5.
	 * If the clock divider is uninitialized then reset it to the
	 * default value to get the 1Hz clock.
	 */
	if (RTTR == 0) {
		RTTR = RTC_DEF_DIVIDER + (RTC_DEF_TRIM << 16);
		printk(KERN_WARNING "Warning: uninitialized Real Time Clock\n");
		/* The current RTC value probably doesn't make sense either */
		RCNR = 0;
		return 0;
	}
	return RCNR;
}

static int sa1100_set_rtc(void)
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
static unsigned long sa1100_gettimeoffset (void)
{
	unsigned long ticks_to_match, elapsed, usec;

	/* Get ticks before next timer match */
	ticks_to_match = OSMR0 - OSCR;

	/* We need elapsed ticks since last match */
	elapsed = LATCH - ticks_to_match;

	/* Now convert them to usec */
	usec = (unsigned long)(elapsed * (tick_nsec / 1000))/LATCH;

	return usec;
}

/*
 * We will be entered with IRQs enabled.
 *
 * Loop until we get ahead of the free running timer.
 * This ensures an exact clock tick count and time accuracy.
 * IRQs are disabled inside the loop to ensure coherence between
 * lost_ticks (updated in do_timer()) and the match reg value, so we
 * can use do_gettimeofday() from interrupt handlers.
 */
static irqreturn_t
sa1100_timer_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	unsigned int next_match;

	do {
		timer_tick(regs);
		OSSR = OSSR_M0;  /* Clear match on timer 0 */
		next_match = (OSMR0 += LATCH);
	} while ((signed long)(next_match - OSCR) <= 0);

	return IRQ_HANDLED;
}

static struct irqaction sa1100_timer_irq = {
	.name		= "SA11xx Timer Tick",
	.flags		= SA_INTERRUPT,
	.handler	= sa1100_timer_interrupt
};

void __init sa1100_init_time(void)
{
	struct timespec tv;

	gettimeoffset = sa1100_gettimeoffset;
	set_rtc = sa1100_set_rtc;

	tv.tv_nsec = 0;
	tv.tv_sec = sa1100_get_rtc_time();
	do_settimeofday(&tv);

	OSMR0 = 0;		/* set initial match at 0 */
	OSSR = 0xf;		/* clear status on all timers */
	setup_irq(IRQ_OST0, &sa1100_timer_irq);
	OIER |= OIER_E0;	/* enable match on timer 0 to cause interrupts */
	OSCR = 0;		/* initialize free-running timer, force first match */
}

