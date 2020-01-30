/***************************************************************************/

/*
 *	pit.c -- Motorola ColdFire PIT timer. Currently this type of
 *	         hardware timer only exists in the Motorola ColdFire
 *		 5282 CPU.
 *
 *	Copyright (C) 1999-2003, Greg Ungerer (gerg@snapgear.com)
 *	Copyright (C) 2001-2003, SnapGear Inc. (www.snapgear.com)
 *
 */

/***************************************************************************/

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/param.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <asm/irq.h>
#include <asm/coldfire.h>
#include <asm/mcfpit.h>
#include <asm/mcfsim.h>

/***************************************************************************/

void coldfire_pit_tick(void)
{
	volatile struct mcfpit *tp;

	/* Reset the ColdFire timer */
	tp = (volatile struct mcfpit *) (MCF_IPSBAR + MCFPIT_BASE1);
	tp->pcsr |= MCFPIT_PCSR_PIF;
}

/***************************************************************************/

void coldfire_pit_init(irqreturn_t (*handler)(int, void *, struct pt_regs *))
{
	volatile unsigned char *icrp;
	volatile unsigned long *imrp;
	volatile struct mcfpit *tp;

	request_irq(64+55, handler, SA_INTERRUPT, "ColdFire Timer", NULL);

	icrp = (volatile unsigned char *) (MCF_IPSBAR + MCFICM_INTC0 +
		MCFINTC_ICR0 + MCFINT_PIT1);
	*icrp = 0x2b; /* PIT1 with level 5, priority 3 */

	imrp = (volatile unsigned long *) (MCF_IPSBAR + MCFICM_INTC0 + MCFINTC_IMRH);
	*imrp &= ~(1 << (55 - 32));

	/* Set up PIT timer 1 as poll clock */
	tp = (volatile struct mcfpit *) (MCF_IPSBAR + MCFPIT_BASE1);
	tp->pcsr = MCFPIT_PCSR_DISABLE;

	tp->pmr = ((MCF_CLK / 2) / 64) / HZ;
	tp->pcsr = MCFPIT_PCSR_EN | MCFPIT_PCSR_PIE | MCFPIT_PCSR_OVW |
		MCFPIT_PCSR_RLD | MCFPIT_PCSR_CLK64;
}

/***************************************************************************/

unsigned long coldfire_pit_offset(void)
{
	volatile struct mcfpit *tp;
	volatile unsigned long *ipr;
	unsigned long pmr, pcntr, offset;

	tp = (volatile struct mcfpit *) (MCF_IPSBAR + MCFPIT_BASE1);
	ipr = (volatile unsigned long *) (MCF_IPSBAR + MCFICM_INTC0 + MCFINTC_IPRH);

	pmr = tp->pmr;
	pcntr = tp->pcntr;

	/*
	 * If we are still in the first half of the upcount and a
	 * timer interupt is pending, then add on a ticks worth of time.
	 */
	offset = ((pcntr * (1000000 / HZ)) / pmr);
	if ((offset < (1000000 / HZ / 2)) && (*ipr & (1 << (55 - 32))))
		offset += 1000000 / HZ;
	return offset;	
}

/***************************************************************************/
