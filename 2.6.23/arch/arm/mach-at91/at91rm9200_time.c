/*
 * linux/arch/arm/mach-at91/at91rm9200_time.c
 *
 *  Copyright (C) 2003 SAN People
 *  Copyright (C) 2003 ATMEL
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/time.h>

#include <asm/hardware.h>
#include <asm/io.h>
#include <asm/mach/time.h>

#include <asm/arch/at91_st.h>

static unsigned long last_crtr;

/*
 * The ST_CRTR is updated asynchronously to the master clock.  It is therefore
 *  necessary to read it twice (with the same value) to ensure accuracy.
 */
static inline unsigned long read_CRTR(void) {
	unsigned long x1, x2;

	do {
		x1 = at91_sys_read(AT91_ST_CRTR);
		x2 = at91_sys_read(AT91_ST_CRTR);
	} while (x1 != x2);

	return x1;
}

/*
 * Returns number of microseconds since last timer interrupt.  Note that interrupts
 * will have been disabled by do_gettimeofday()
 *  'LATCH' is hwclock ticks (see CLOCK_TICK_RATE in timex.h) per jiffy.
 *  'tick' is usecs per jiffy (linux/timex.h).
 */
static unsigned long at91rm9200_gettimeoffset(void)
{
	unsigned long elapsed;

	elapsed = (read_CRTR() - last_crtr) & AT91_ST_ALMV;

	return (unsigned long)(elapsed * (tick_nsec / 1000)) / LATCH;
}

/*
 * IRQ handler for the timer.
 */
static irqreturn_t at91rm9200_timer_interrupt(int irq, void *dev_id)
{
	if (at91_sys_read(AT91_ST_SR) & AT91_ST_PITS) {	/* This is a shared interrupt */
		write_seqlock(&xtime_lock);

		while (((read_CRTR() - last_crtr) & AT91_ST_ALMV) >= LATCH) {
			timer_tick();
			last_crtr = (last_crtr + LATCH) & AT91_ST_ALMV;
		}

		write_sequnlock(&xtime_lock);

		return IRQ_HANDLED;
	}
	else
		return IRQ_NONE;		/* not handled */
}

static struct irqaction at91rm9200_timer_irq = {
	.name		= "at91_tick",
	.flags		= IRQF_SHARED | IRQF_DISABLED | IRQF_TIMER | IRQF_IRQPOLL,
	.handler	= at91rm9200_timer_interrupt
};

void at91rm9200_timer_reset(void)
{
	last_crtr = 0;

	/* Real time counter incremented every 30.51758 microseconds */
	at91_sys_write(AT91_ST_RTMR, 1);

	/* Set Period Interval timer */
	at91_sys_write(AT91_ST_PIMR, LATCH);

	/* Clear any pending interrupts */
	(void) at91_sys_read(AT91_ST_SR);

	/* Enable Period Interval Timer interrupt */
	at91_sys_write(AT91_ST_IER, AT91_ST_PITS);
}

/*
 * Set up timer interrupt.
 */
void __init at91rm9200_timer_init(void)
{
	/* Disable all timer interrupts */
	at91_sys_write(AT91_ST_IDR, AT91_ST_PITS | AT91_ST_WDOVF | AT91_ST_RTTINC | AT91_ST_ALMS);
	(void) at91_sys_read(AT91_ST_SR);	/* Clear any pending interrupts */

	/* Make IRQs happen for the system timer */
	setup_irq(AT91_ID_SYS, &at91rm9200_timer_irq);

	/* Change the kernel's 'tick' value to 10009 usec. (the default is 10000) */
	tick_usec = (LATCH * 1000000) / CLOCK_TICK_RATE;

	/* Initialize and enable the timer interrupt */
	at91rm9200_timer_reset();
}

#ifdef CONFIG_PM
static void at91rm9200_timer_suspend(void)
{
	/* disable Period Interval Timer interrupt */
	at91_sys_write(AT91_ST_IDR, AT91_ST_PITS);
}
#else
#define at91rm9200_timer_suspend	NULL
#endif

struct sys_timer at91rm9200_timer = {
	.init		= at91rm9200_timer_init,
	.offset		= at91rm9200_gettimeoffset,
	.suspend	= at91rm9200_timer_suspend,
	.resume		= at91rm9200_timer_reset,
};

