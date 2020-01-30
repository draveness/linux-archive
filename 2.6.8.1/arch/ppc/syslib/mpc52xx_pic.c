/*
 * arch/ppc/syslib/mpc52xx_pic.c
 *
 * Programmable Interrupt Controller functions for the Freescale MPC52xx 
 * embedded CPU.
 *
 * 
 * Maintainer : Sylvain Munaut <tnt@246tNt.com>
 *
 * Based on (well, mostly copied from) the code from the 2.4 kernel by
 * Dale Farnsworth <dfarnsworth@mvista.com> and Kent Borg.
 * 
 * Copyright (C) 2004 Sylvain Munaut <tnt@246tNt.com>
 * Copyright (C) 2003 Montavista Software, Inc
 * 
 * This file is licensed under the terms of the GNU General Public License
 * version 2. This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#include <linux/stddef.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/stddef.h>
#include <linux/delay.h>
#include <linux/irq.h>

#include <asm/io.h>
#include <asm/processor.h>
#include <asm/system.h>
#include <asm/irq.h>
#include <asm/mpc52xx.h>


static struct mpc52xx_intr *intr;
static struct mpc52xx_sdma *sdma;

static void
mpc52xx_ic_disable(unsigned int irq)
{
	u32 val;

	if (irq == MPC52xx_IRQ0) {
		val = in_be32(&intr->ctrl);
		val &= ~(1 << 11);
		out_be32(&intr->ctrl, val);
	}
	else if (irq < MPC52xx_IRQ1) {
		BUG();
	}
	else if (irq <= MPC52xx_IRQ3) {
		val = in_be32(&intr->ctrl);
		val &= ~(1 << (10 - (irq - MPC52xx_IRQ1)));
		out_be32(&intr->ctrl, val);
	}
	else if (irq < MPC52xx_SDMA_IRQ_BASE) {
		val = in_be32(&intr->main_mask);
		val |= 1 << (16 - (irq - MPC52xx_MAIN_IRQ_BASE));
		out_be32(&intr->main_mask, val);
	}
	else if (irq < MPC52xx_PERP_IRQ_BASE) {
		val = in_be32(&sdma->IntMask);
		val |= 1 << (irq - MPC52xx_SDMA_IRQ_BASE);
		out_be32(&sdma->IntMask, val);
	}
	else {
		val = in_be32(&intr->per_mask);
		val |= 1 << (31 - (irq - MPC52xx_PERP_IRQ_BASE));
		out_be32(&intr->per_mask, val);
	}
}

static void
mpc52xx_ic_enable(unsigned int irq)
{
	u32 val;

	if (irq == MPC52xx_IRQ0) {
		val = in_be32(&intr->ctrl);
		val |= 1 << 11;
		out_be32(&intr->ctrl, val);
	}
	else if (irq < MPC52xx_IRQ1) {
		BUG();
	}
	else if (irq <= MPC52xx_IRQ3) {
		val = in_be32(&intr->ctrl);
		val |= 1 << (10 - (irq - MPC52xx_IRQ1));
		out_be32(&intr->ctrl, val);
	}
	else if (irq < MPC52xx_SDMA_IRQ_BASE) {
		val = in_be32(&intr->main_mask);
		val &= ~(1 << (16 - (irq - MPC52xx_MAIN_IRQ_BASE)));
		out_be32(&intr->main_mask, val);
	}
	else if (irq < MPC52xx_PERP_IRQ_BASE) {
		val = in_be32(&sdma->IntMask);
		val &= ~(1 << (irq - MPC52xx_SDMA_IRQ_BASE));
		out_be32(&sdma->IntMask, val);
	}
	else {
		val = in_be32(&intr->per_mask);
		val &= ~(1 << (31 - (irq - MPC52xx_PERP_IRQ_BASE)));
		out_be32(&intr->per_mask, val);
	}
}

static void
mpc52xx_ic_ack(unsigned int irq)
{
	u32 val;

	/*
	 * Only some irqs are reset here, others in interrupting hardware.
	 */
			
	switch (irq) {
	case MPC52xx_IRQ0:
		val = in_be32(&intr->ctrl);
		val |= 0x08000000;
		out_be32(&intr->ctrl, val);
		break;
	case MPC52xx_CCS_IRQ:
		val = in_be32(&intr->enc_status);
		val |= 0x00000400;
		out_be32(&intr->enc_status, val);
		break;
	case MPC52xx_IRQ1:
		val = in_be32(&intr->ctrl);
		val |= 0x04000000;
		out_be32(&intr->ctrl, val);
		break;
	case MPC52xx_IRQ2:
		val = in_be32(&intr->ctrl);
		val |= 0x02000000;
		out_be32(&intr->ctrl, val);
		break;
	case MPC52xx_IRQ3:
		val = in_be32(&intr->ctrl);
		val |= 0x01000000;
		out_be32(&intr->ctrl, val);
		break;
	default:
		if (irq >= MPC52xx_SDMA_IRQ_BASE
		    && irq < (MPC52xx_SDMA_IRQ_BASE + MPC52xx_SDMA_IRQ_NUM)) {
			out_be32(&sdma->IntPend,
				 1 << (irq - MPC52xx_SDMA_IRQ_BASE));
		}
		break;
	}
}

static void
mpc52xx_ic_disable_and_ack(unsigned int irq)
{
	mpc52xx_ic_disable(irq);
	mpc52xx_ic_ack(irq);
}

static void
mpc52xx_ic_end(unsigned int irq)
{
	if (!(irq_desc[irq].status & (IRQ_DISABLED | IRQ_INPROGRESS)))
		mpc52xx_ic_enable(irq);
}

static struct hw_interrupt_type mpc52xx_ic = {
	"MPC52xx",
	NULL,				/* startup(irq) */
	NULL,				/* shutdown(irq) */
	mpc52xx_ic_enable,		/* enable(irq) */
	mpc52xx_ic_disable,		/* disable(irq) */
	mpc52xx_ic_disable_and_ack,	/* disable_and_ack(irq) */
	mpc52xx_ic_end,			/* end(irq) */
	0				/* set_affinity(irq, cpumask) SMP. */
};

void __init
mpc52xx_init_irq(void)
{
	int i;

	/* Remap the necessary zones */
	intr = (struct mpc52xx_intr *)
		ioremap(MPC52xx_INTR, sizeof(struct mpc52xx_intr));
	sdma = (struct mpc52xx_sdma *)
		ioremap(MPC52xx_SDMA, sizeof(struct mpc52xx_sdma));
	
	if ((intr==NULL) || (sdma==NULL))
		panic("Can't ioremap PIC/SDMA register for init_irq !");

	/* Disable all interrupt sources. */
	out_be32(&sdma->IntPend, 0xffffffff);	/* 1 means clear pending */
	out_be32(&sdma->IntMask, 0xffffffff);	/* 1 means disabled */
	out_be32(&intr->per_mask, 0x7ffffc00);	/* 1 means disabled */
	out_be32(&intr->main_mask, 0x00010fff);	/* 1 means disabled */
	out_be32(&intr->ctrl,
			0x0f000000 |	/* clear IRQ 0-3 */
			0x00c00000 |	/* IRQ0: level-sensitive, active low */
			0x00001000 |	/* MEE master external enable */
			0x00000000 |	/* 0 means disable IRQ 0-3 */
			0x00000001);	/* CEb route critical normally */

	/* Zero a bunch of the priority settings.  */
	out_be32(&intr->per_pri1, 0);
	out_be32(&intr->per_pri2, 0);
	out_be32(&intr->per_pri3, 0);
	out_be32(&intr->main_pri1, 0);
	out_be32(&intr->main_pri2, 0);

	/* Initialize irq_desc[i].handler's with mpc52xx_ic. */
	for (i = 0; i < NR_IRQS; i++) {
		irq_desc[i].handler = &mpc52xx_ic;
		irq_desc[i].status = IRQ_LEVEL;
	}
}

int
mpc52xx_get_irq(struct pt_regs *regs)
{
	u32 status;
	int irq = -1;

	status = in_be32(&intr->enc_status);

	if (status & 0x00000400) {		/* critical */
		irq = (status >> 8) & 0x3;
		if (irq == 2)			/* high priority peripheral */
			goto peripheral;
		irq += MPC52xx_CRIT_IRQ_BASE;
	}
	else if (status & 0x00200000) {		/* main */
		irq = (status >> 16) & 0x1f;
		if (irq == 4)			/* low priority peripheral */
			goto peripheral;
		irq += MPC52xx_MAIN_IRQ_BASE;
	}
	else if (status & 0x20000000) {		/* peripheral */
peripheral:
		irq = (status >> 24) & 0x1f;
		if (irq == 0) {			/* bestcomm */
			status = in_be32(&sdma->IntPend);
			irq = ffs(status) + MPC52xx_SDMA_IRQ_BASE-1;
		}
		else
			irq += MPC52xx_PERP_IRQ_BASE;
	}

	return irq;
}

