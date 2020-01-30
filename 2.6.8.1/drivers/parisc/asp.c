/*
 *	ASP Device Driver
 *
 *	(c) Copyright 2000 The Puffin Group Inc.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *	by Helge Deller <deller@gmx.de>
 */

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <asm/io.h>
#include <asm/led.h>

#include "gsc.h"

#define ASP_GSC_IRQ	3		/* hardcoded interrupt for GSC */

#define ASP_VER_OFFSET 	0x20		/* offset of ASP version */

#define ASP_LED_ADDR	0xf0800020

#define VIPER_INT_WORD  0xFFFBF088      /* addr of viper interrupt word */

static int asp_choose_irq(struct parisc_device *dev)
{
	int irq = -1;

	switch (dev->id.sversion) {
	case 0x71:	irq = 22; break; /* SCSI */
	case 0x72:	irq = 23; break; /* LAN */
	case 0x73:	irq = 30; break; /* HIL */
	case 0x74:	irq = 24; break; /* Centronics */
	case 0x75:	irq = (dev->hw_path == 4) ? 26 : 25; break; /* RS232 */
	case 0x76:	irq = 21; break; /* EISA BA */
	case 0x77:	irq = 20; break; /* Graphics1 */
	case 0x7a:	irq = 18; break; /* Audio (Bushmaster) */
	case 0x7b:	irq = 18; break; /* Audio (Scorpio) */
	case 0x7c:	irq = 28; break; /* FW SCSI */
	case 0x7d:	irq = 27; break; /* FDDI */
	case 0x7f:	irq = 18; break; /* Audio (Outfield) */
	}
	return irq;
}

/* There are two register ranges we're interested in.  Interrupt /
 * Status / LED are at 0xf080xxxx and Asp special registers are at
 * 0xf082fxxx.  PDC only tells us that Asp is at 0xf082f000, so for
 * the purposes of interrupt handling, we have to tell other bits of
 * the kernel to look at the other registers.
 */
#define ASP_INTERRUPT_ADDR 0xf0800000

int __init
asp_init_chip(struct parisc_device *dev)
{
	struct busdevice *asp;
	struct gsc_irq gsc_irq;
	int irq, ret;

	asp = kmalloc(sizeof(struct busdevice), GFP_KERNEL);
	if(!asp)
		return -ENOMEM;

	asp->version = gsc_readb(dev->hpa + ASP_VER_OFFSET) & 0xf;
	asp->name = (asp->version == 1) ? "Asp" : "Cutoff";
	asp->hpa = ASP_INTERRUPT_ADDR;

	printk(KERN_INFO "%s version %d at 0x%lx found.\n", 
		asp->name, asp->version, dev->hpa);

	/* the IRQ ASP should use */
	ret = -EBUSY;
	irq = gsc_claim_irq(&gsc_irq, ASP_GSC_IRQ);
	if (irq < 0) {
		printk(KERN_ERR "%s(): cannot get GSC irq\n", __FUNCTION__);
		goto out;
	}

	ret = request_irq(gsc_irq.irq, busdev_barked, 0, "asp", asp);
	if (ret < 0)
		goto out;

	/* Save this for debugging later */
	asp->parent_irq = gsc_irq.irq;
	asp->eim = ((u32) gsc_irq.txn_addr) | gsc_irq.txn_data;

	/* Program VIPER to interrupt on the ASP irq */
	gsc_writel((1 << (31 - ASP_GSC_IRQ)),VIPER_INT_WORD);

	/* Done init'ing, register this driver */
	ret = gsc_common_irqsetup(dev, asp);
	if (ret)
		goto out;

	fixup_child_irqs(dev, asp->busdev_region->data.irqbase, asp_choose_irq);
	/* Mongoose is a sibling of Asp, not a child... */
	fixup_child_irqs(dev->parent, asp->busdev_region->data.irqbase,
			asp_choose_irq);

	/* initialize the chassis LEDs */ 
#ifdef CONFIG_CHASSIS_LCD_LED	
	register_led_driver(DISPLAY_MODEL_OLD_ASP, LED_CMD_REG_NONE, 
		    (char *)ASP_LED_ADDR);
#endif

	return 0;

out:
	kfree(asp);
	return ret;
}

static struct parisc_device_id asp_tbl[] = {
	{ HPHW_BA, HVERSION_REV_ANY_ID, HVERSION_ANY_ID, 0x00070 },
	{ 0, }
};

struct parisc_driver asp_driver = {
	.name =		"Asp",
	.id_table =	asp_tbl,
	.probe =	asp_init_chip,
};
