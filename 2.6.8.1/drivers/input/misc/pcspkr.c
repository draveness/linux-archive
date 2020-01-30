/*
 *  PC Speaker beeper driver for Linux
 *
 *  Copyright (c) 2002 Vojtech Pavlik
 *  Copyright (c) 1992 Orest Zborowski
 *
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <asm/8253pit.h>
#include <asm/io.h>

MODULE_AUTHOR("Vojtech Pavlik <vojtech@ucw.cz>");
MODULE_DESCRIPTION("PC Speaker beeper driver");
MODULE_LICENSE("GPL");

static char pcspkr_name[] = "PC Speaker";
static char pcspkr_phys[] = "isa0061/input0";
static struct input_dev pcspkr_dev;

spinlock_t i8253_beep_lock = SPIN_LOCK_UNLOCKED;

static int pcspkr_event(struct input_dev *dev, unsigned int type, unsigned int code, int value)
{
	unsigned int count = 0;
	unsigned long flags;

	if (type != EV_SND)
		return -1;

	switch (code) {
		case SND_BELL: if (value) value = 1000;
		case SND_TONE: break;
		default: return -1;
	}

	if (value > 20 && value < 32767)
		count = PIT_TICK_RATE / value;

	spin_lock_irqsave(&i8253_beep_lock, flags);

	if (count) {
		/* enable counter 2 */
		outb_p(inb_p(0x61) | 3, 0x61);
		/* set command for counter 2, 2 byte write */
		outb_p(0xB6, 0x43);
		/* select desired HZ */
		outb_p(count & 0xff, 0x42);
		outb((count >> 8) & 0xff, 0x42);
	} else {
		/* disable counter 2 */
		outb(inb_p(0x61) & 0xFC, 0x61);
	}

	spin_unlock_irqrestore(&i8253_beep_lock, flags);

	return 0;
}

static int __init pcspkr_init(void)
{
	pcspkr_dev.evbit[0] = BIT(EV_SND);
	pcspkr_dev.sndbit[0] = BIT(SND_BELL) | BIT(SND_TONE);
	pcspkr_dev.event = pcspkr_event;

	pcspkr_dev.name = pcspkr_name;
	pcspkr_dev.phys = pcspkr_phys;
	pcspkr_dev.id.bustype = BUS_ISA;
	pcspkr_dev.id.vendor = 0x001f;
	pcspkr_dev.id.product = 0x0001;
	pcspkr_dev.id.version = 0x0100;

	input_register_device(&pcspkr_dev);

        printk(KERN_INFO "input: %s\n", pcspkr_name);

	return 0;
}

static void __exit pcspkr_exit(void)
{
        input_unregister_device(&pcspkr_dev);
}

module_init(pcspkr_init);
module_exit(pcspkr_exit);
