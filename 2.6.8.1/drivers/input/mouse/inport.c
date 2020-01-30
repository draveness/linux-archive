/*
 * $Id: inport.c,v 1.11 2001/09/25 10:12:07 vojtech Exp $
 *
 *  Copyright (c) 1999-2001 Vojtech Pavlik
 *
 *  Based on the work of:
 *	Teemu Rantanen		Derrick Cole
 *	Peter Cervasio		Christoph Niemann
 *	Philip Blundell		Russell King
 *	Bob Harris
 */

/*
 * Inport (ATI XL and Microsoft) busmouse driver for Linux
 */

/*
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 * 
 * Should you need to contact me, the author, you can do so either by
 * e-mail - mail your message to <vojtech@ucw.cz>, or by paper mail:
 * Vojtech Pavlik, Simunkova 1594, Prague 8, 182 00 Czech Republic
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/config.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/input.h>

#include <asm/io.h>
#include <asm/irq.h>

MODULE_AUTHOR("Vojtech Pavlik <vojtech@ucw.cz>");
MODULE_DESCRIPTION("Inport (ATI XL and Microsoft) busmouse driver");
MODULE_LICENSE("GPL");

#define INPORT_BASE		0x23c
#define INPORT_EXTENT		4

#define INPORT_CONTROL_PORT	INPORT_BASE + 0
#define INPORT_DATA_PORT	INPORT_BASE + 1
#define INPORT_SIGNATURE_PORT	INPORT_BASE + 2

#define INPORT_REG_BTNS	0x00
#define INPORT_REG_X		0x01
#define INPORT_REG_Y		0x02
#define INPORT_REG_MODE		0x07
#define INPORT_RESET		0x80

#ifdef CONFIG_INPUT_ATIXL
#define INPORT_NAME		"ATI XL Mouse"
#define INPORT_VENDOR		0x0002
#define INPORT_SPEED_30HZ	0x01
#define INPORT_SPEED_50HZ	0x02
#define INPORT_SPEED_100HZ	0x03
#define INPORT_SPEED_200HZ	0x04
#define INPORT_MODE_BASE	INPORT_SPEED_100HZ
#define INPORT_MODE_IRQ		0x08
#else
#define INPORT_NAME		"Microsoft InPort Mouse"
#define INPORT_VENDOR		0x0001
#define INPORT_MODE_BASE	0x10
#define INPORT_MODE_IRQ		0x01
#endif
#define INPORT_MODE_HOLD	0x20

#define INPORT_IRQ		5

static int inport_irq = INPORT_IRQ;
module_param_named(irq, inport_irq, uint, 0);
MODULE_PARM_DESC(irq, "IRQ number (5=default)");

__obsolete_setup("inport_irq=");

static int inport_used;

static irqreturn_t inport_interrupt(int irq, void *dev_id, struct pt_regs *regs);

static int inport_open(struct input_dev *dev)
{
	if (!inport_used++) {
		if (request_irq(inport_irq, inport_interrupt, 0, "inport", NULL))
			return -EBUSY;
		outb(INPORT_REG_MODE, INPORT_CONTROL_PORT);
		outb(INPORT_MODE_IRQ | INPORT_MODE_BASE, INPORT_DATA_PORT);
	}

	return 0;
}

static void inport_close(struct input_dev *dev)
{
	if (!--inport_used) {
		outb(INPORT_REG_MODE, INPORT_CONTROL_PORT);
		outb(INPORT_MODE_BASE, INPORT_DATA_PORT);
		free_irq(inport_irq, NULL);
	}
}

static struct input_dev inport_dev = {
	.evbit	= { BIT(EV_KEY) | BIT(EV_REL) },
	.keybit	= { [LONG(BTN_LEFT)] = BIT(BTN_LEFT) | BIT(BTN_MIDDLE) | BIT(BTN_RIGHT) },
	.relbit	= { BIT(REL_X) | BIT(REL_Y) },
	.open	= inport_open,
	.close	= inport_close,
	.name	= INPORT_NAME,
	.phys	= "isa023c/input0",
	.id = { 
 		.bustype = BUS_ISA,
        	.vendor  = INPORT_VENDOR,
        	.product = 0x0001,
        	.version = 0x0100,
	},
};

static irqreturn_t inport_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	unsigned char buttons;

	outb(INPORT_REG_MODE, INPORT_CONTROL_PORT);
	outb(INPORT_MODE_HOLD | INPORT_MODE_IRQ | INPORT_MODE_BASE, INPORT_DATA_PORT);

	input_regs(&inport_dev, regs);

	outb(INPORT_REG_X, INPORT_CONTROL_PORT);
	input_report_rel(&inport_dev, REL_X, inb(INPORT_DATA_PORT));

	outb(INPORT_REG_Y, INPORT_CONTROL_PORT);
	input_report_rel(&inport_dev, REL_Y, inb(INPORT_DATA_PORT));

	outb(INPORT_REG_BTNS, INPORT_CONTROL_PORT);
	buttons = inb(INPORT_DATA_PORT);

	input_report_key(&inport_dev, BTN_MIDDLE, buttons & 1);
	input_report_key(&inport_dev, BTN_LEFT,   buttons & 2);
	input_report_key(&inport_dev, BTN_RIGHT,  buttons & 4);

	outb(INPORT_REG_MODE, INPORT_CONTROL_PORT);
	outb(INPORT_MODE_IRQ | INPORT_MODE_BASE, INPORT_DATA_PORT);

	input_sync(&inport_dev);
	return IRQ_HANDLED;
}

static int __init inport_init(void)
{
	unsigned char a,b,c;

	if (!request_region(INPORT_BASE, INPORT_EXTENT, "inport")) {
		printk(KERN_ERR "inport.c: Can't allocate ports at %#x\n", INPORT_BASE);
		return -EBUSY;
	}

	a = inb(INPORT_SIGNATURE_PORT);
	b = inb(INPORT_SIGNATURE_PORT);
	c = inb(INPORT_SIGNATURE_PORT);
	if (( a == b ) || ( a != c )) {
		release_region(INPORT_BASE, INPORT_EXTENT);
		printk(KERN_ERR "inport.c: Didn't find InPort mouse at %#x\n", INPORT_BASE);
		return -ENODEV;
	}

	outb(INPORT_RESET, INPORT_CONTROL_PORT);
	outb(INPORT_REG_MODE, INPORT_CONTROL_PORT);
	outb(INPORT_MODE_BASE, INPORT_DATA_PORT);

	input_register_device(&inport_dev);

	printk(KERN_INFO "input: " INPORT_NAME " at %#x irq %d\n", INPORT_BASE, inport_irq);

	return 0;
}

static void __exit inport_exit(void)
{
	input_unregister_device(&inport_dev);
	release_region(INPORT_BASE, INPORT_EXTENT);
}

module_init(inport_init);
module_exit(inport_exit);
