/*
 * arch/sh/boards/dreamcast/dma-pvr2.c
 *
 * NEC PowerVR 2 (Dreamcast) DMA support
 *
 * Copyright (C) 2003  Paul Mundt
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <asm/mach/sysasic.h>
#include <asm/mach/dma.h>
#include <asm/dma.h>
#include <asm/io.h>

static unsigned int xfer_complete = 0;
static int count = 0;

static irqreturn_t pvr2_dma_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	if (get_dma_residue(PVR2_CASCADE_CHAN)) {
		printk(KERN_WARNING "DMA: SH DMAC did not complete transfer "
		       "on channel %d, waiting..\n", PVR2_CASCADE_CHAN);
		dma_wait_for_completion(PVR2_CASCADE_CHAN);
	}

	if (count++ < 10)
		pr_debug("Got a pvr2 dma interrupt for channel %d\n",
			 irq - HW_EVENT_PVR2_DMA);

	xfer_complete = 1;

	return IRQ_HANDLED;
}

static int pvr2_request_dma(struct dma_info *info)
{
	if (ctrl_inl(PVR2_DMA_MODE) != 0)
		return -EBUSY;

	ctrl_outl(0, PVR2_DMA_LMMODE0);

	return 0;
}

static int pvr2_get_dma_residue(struct dma_info *info)
{
	return xfer_complete == 0;
}

static int pvr2_xfer_dma(struct dma_info *info)
{
	if (info->sar || !info->dar)
		return -EINVAL;

	xfer_complete = 0;

	ctrl_outl(info->dar, PVR2_DMA_ADDR);
	ctrl_outl(info->count, PVR2_DMA_COUNT);
	ctrl_outl(info->mode & DMA_MODE_MASK, PVR2_DMA_MODE);

	return 0;
}

static struct irqaction pvr2_dma_irq = {
	.name		= "pvr2 DMA handler",
	.handler	= pvr2_dma_interrupt,
	.flags		= SA_INTERRUPT,
};

static struct dma_ops pvr2_dma_ops = {
	.name		= "PowerVR 2 DMA",
	.request	= pvr2_request_dma,
	.get_residue	= pvr2_get_dma_residue,
	.xfer		= pvr2_xfer_dma,
};

static int __init pvr2_dma_init(void)
{
	int i, base;

	setup_irq(HW_EVENT_PVR2_DMA, &pvr2_dma_irq);
	request_dma(PVR2_CASCADE_CHAN, "pvr2 cascade");

	/* PVR2 cascade comes after on-chip DMAC */
	base = ONCHIP_NR_DMA_CHANNELS;

	for (i = 0; i < PVR2_NR_DMA_CHANNELS; i++)
		dma_info[base + i].ops = &pvr2_dma_ops;

	return register_dmac(&pvr2_dma_ops);
}

static void __exit pvr2_dma_exit(void)
{
	free_dma(PVR2_CASCADE_CHAN);
	free_irq(HW_EVENT_PVR2_DMA, 0);
}

subsys_initcall(pvr2_dma_init);
module_exit(pvr2_dma_exit);

MODULE_AUTHOR("Paul Mundt <lethal@linux-sh.org>");
MODULE_DESCRIPTION("NEC PowerVR 2 DMA driver");
MODULE_LICENSE("GPL");

