/*
 * $Id: sunkbd.c,v 1.14 2001/09/25 10:12:07 vojtech Exp $
 *
 *  Copyright (c) 1999-2001 Vojtech Pavlik
 */

/*
 * Sun keyboard driver for Linux
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

#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/serio.h>
#include <linux/workqueue.h>

MODULE_AUTHOR("Vojtech Pavlik <vojtech@ucw.cz>");
MODULE_DESCRIPTION("Sun keyboard driver");
MODULE_LICENSE("GPL");

static unsigned char sunkbd_keycode[128] = {
	  0,128,114,129,115, 59, 60, 68, 61, 87, 62, 88, 63,100, 64,  0,
	 65, 66, 67, 56,103,119, 99, 70,105,130,131,108,106,  1,  2,  3,
	  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 41, 14,110,113, 98, 55,
	116,132, 83,133,102, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
	 26, 27,111,127, 71, 72, 73, 74,134,135,107,  0, 29, 30, 31, 32,
	 33, 34, 35, 36, 37, 38, 39, 40, 43, 28, 96, 75, 76, 77, 82,136,
	104,137, 69, 42, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54,101,
	 79, 80, 81,  0,  0,  0,138, 58,125, 57,126,109, 86, 78
};

#define SUNKBD_CMD_RESET	0x1
#define SUNKBD_CMD_BELLON	0x2
#define SUNKBD_CMD_BELLOFF	0x3
#define SUNKBD_CMD_CLICK	0xa
#define SUNKBD_CMD_NOCLICK	0xb
#define SUNKBD_CMD_SETLED	0xe
#define SUNKBD_CMD_LAYOUT	0xf

#define SUNKBD_RET_RESET	0xff
#define SUNKBD_RET_ALLUP	0x7f
#define SUNKBD_RET_LAYOUT	0xfe

#define SUNKBD_LAYOUT_5_MASK	0x20
#define SUNKBD_RELEASE		0x80
#define SUNKBD_KEY		0x7f

/*
 * Per-keyboard data.
 */

struct sunkbd {
	unsigned char keycode[128];
	struct input_dev dev;
	struct serio *serio;
	struct work_struct tq;
	wait_queue_head_t wait;
	char name[64];
	char phys[32];
	char type;
	volatile s8 reset;
	volatile s8 layout;
};

/*
 * sunkbd_interrupt() is called by the low level driver when a character
 * is received.
 */

static irqreturn_t sunkbd_interrupt(struct serio *serio,
		unsigned char data, unsigned int flags, struct pt_regs *regs)
{
	struct sunkbd* sunkbd = serio->private;

	if (sunkbd->reset <= -1) {		/* If cp[i] is 0xff, sunkbd->reset will stay -1. */
		sunkbd->reset = data;		/* The keyboard sends 0xff 0xff 0xID on powerup */
		wake_up_interruptible(&sunkbd->wait);
		goto out;
	}

	if (sunkbd->layout == -1) {
		sunkbd->layout = data;
		wake_up_interruptible(&sunkbd->wait);
		goto out;
	}

	switch (data) {

		case SUNKBD_RET_RESET:
			schedule_work(&sunkbd->tq);
			sunkbd->reset = -1;
			break;

		case SUNKBD_RET_LAYOUT:
			sunkbd->layout = -1;
			break;

		case SUNKBD_RET_ALLUP: /* All keys released */
			break;

		default:
			if (sunkbd->keycode[data & SUNKBD_KEY]) {
				input_regs(&sunkbd->dev, regs);
                                input_report_key(&sunkbd->dev, sunkbd->keycode[data & SUNKBD_KEY], !(data & SUNKBD_RELEASE));
				input_sync(&sunkbd->dev);
                        } else {
                                printk(KERN_WARNING "sunkbd.c: Unknown key (scancode %#x) %s.\n",
                                        data & SUNKBD_KEY, data & SUNKBD_RELEASE ? "released" : "pressed");
                        }
	}
out:
	return IRQ_HANDLED;
}

/*
 * sunkbd_event() handles events from the input module.
 */

static int sunkbd_event(struct input_dev *dev, unsigned int type, unsigned int code, int value)
{
	struct sunkbd *sunkbd = dev->private;

	switch (type) {

		case EV_LED:

			sunkbd->serio->write(sunkbd->serio, SUNKBD_CMD_SETLED);
			sunkbd->serio->write(sunkbd->serio,
				(!!test_bit(LED_CAPSL, dev->led) << 3) | (!!test_bit(LED_SCROLLL, dev->led) << 2) |
				(!!test_bit(LED_COMPOSE, dev->led) << 1) | !!test_bit(LED_NUML, dev->led));
			return 0;

		case EV_SND:

			switch (code) {

				case SND_CLICK:
					sunkbd->serio->write(sunkbd->serio, SUNKBD_CMD_NOCLICK - value);
					return 0;

				case SND_BELL:
					sunkbd->serio->write(sunkbd->serio, SUNKBD_CMD_BELLOFF - value);
					return 0;
			}

			break;
	}

	return -1;
}

/*
 * sunkbd_initialize() checks for a Sun keyboard attached, and determines
 * its type.
 */

static int sunkbd_initialize(struct sunkbd *sunkbd)
{
	sunkbd->reset = -2;
	sunkbd->serio->write(sunkbd->serio, SUNKBD_CMD_RESET);
	wait_event_interruptible_timeout(sunkbd->wait, sunkbd->reset >= 0, HZ);
	if (sunkbd->reset <0)
		return -1;

	sunkbd->type = sunkbd->reset;

	if (sunkbd->type == 4) {	/* Type 4 keyboard */
		sunkbd->layout = -2;
		sunkbd->serio->write(sunkbd->serio, SUNKBD_CMD_LAYOUT);
		wait_event_interruptible_timeout(sunkbd->wait, sunkbd->layout >= 0, HZ/4);
		if (sunkbd->layout < 0) return -1;
		if (sunkbd->layout & SUNKBD_LAYOUT_5_MASK) sunkbd->type = 5;
	}

	return 0;
}

/*
 * sunkbd_reinit() sets leds and beeps to a state the computer remembers they
 * were in.
 */

static void sunkbd_reinit(void *data)
{
	struct sunkbd *sunkbd = data;

	wait_event_interruptible_timeout(sunkbd->wait, sunkbd->reset >= 0, HZ);

	sunkbd->serio->write(sunkbd->serio, SUNKBD_CMD_SETLED);
	sunkbd->serio->write(sunkbd->serio,
		(!!test_bit(LED_CAPSL, sunkbd->dev.led) << 3) | (!!test_bit(LED_SCROLLL, sunkbd->dev.led) << 2) |
		(!!test_bit(LED_COMPOSE, sunkbd->dev.led) << 1) | !!test_bit(LED_NUML, sunkbd->dev.led));
	sunkbd->serio->write(sunkbd->serio, SUNKBD_CMD_NOCLICK - !!test_bit(SND_CLICK, sunkbd->dev.snd));
	sunkbd->serio->write(sunkbd->serio, SUNKBD_CMD_BELLOFF - !!test_bit(SND_BELL, sunkbd->dev.snd));
}

/*
 * sunkbd_connect() probes for a Sun keyboard and fills the necessary structures.
 */

static void sunkbd_connect(struct serio *serio, struct serio_dev *dev)
{
	struct sunkbd *sunkbd;
	int i;

	if ((serio->type & SERIO_TYPE) != SERIO_RS232)
		return;

	if ((serio->type & SERIO_PROTO) && (serio->type & SERIO_PROTO) != SERIO_SUNKBD)
		return;

	if (!(sunkbd = kmalloc(sizeof(struct sunkbd), GFP_KERNEL)))
		return;

	memset(sunkbd, 0, sizeof(struct sunkbd));

	init_input_dev(&sunkbd->dev);
	init_waitqueue_head(&sunkbd->wait);

	sunkbd->dev.evbit[0] = BIT(EV_KEY) | BIT(EV_LED) | BIT(EV_SND) | BIT(EV_REP);
	sunkbd->dev.ledbit[0] = BIT(LED_CAPSL) | BIT(LED_COMPOSE) | BIT(LED_SCROLLL) | BIT(LED_NUML);
	sunkbd->dev.sndbit[0] = BIT(SND_CLICK) | BIT(SND_BELL);

	sunkbd->serio = serio;

	INIT_WORK(&sunkbd->tq, sunkbd_reinit, sunkbd);

	sunkbd->dev.keycode = sunkbd->keycode;
	sunkbd->dev.keycodesize = sizeof(unsigned char);
	sunkbd->dev.keycodemax = ARRAY_SIZE(sunkbd_keycode);

	sunkbd->dev.event = sunkbd_event;
	sunkbd->dev.private = sunkbd;

	serio->private = sunkbd;

	if (serio_open(serio, dev)) {
		kfree(sunkbd);
		return;
	}

	if (sunkbd_initialize(sunkbd) < 0) {
		serio_close(serio);
		kfree(sunkbd);
		return;
	}

	sprintf(sunkbd->name, "Sun Type %d keyboard", sunkbd->type);

	memcpy(sunkbd->keycode, sunkbd_keycode, sizeof(sunkbd->keycode));
	for (i = 0; i < 128; i++)
		set_bit(sunkbd->keycode[i], sunkbd->dev.keybit);
	clear_bit(0, sunkbd->dev.keybit);

	sprintf(sunkbd->phys, "%s/input0", serio->phys);

	sunkbd->dev.name = sunkbd->name;
	sunkbd->dev.phys = sunkbd->phys;
	sunkbd->dev.id.bustype = BUS_RS232;
	sunkbd->dev.id.vendor = SERIO_SUNKBD;
	sunkbd->dev.id.product = sunkbd->type;
	sunkbd->dev.id.version = 0x0100;

	input_register_device(&sunkbd->dev);

	printk(KERN_INFO "input: %s on %s\n", sunkbd->name, serio->phys);
}

/*
 * sunkbd_disconnect() unregisters and closes behind us.
 */

static void sunkbd_disconnect(struct serio *serio)
{
	struct sunkbd *sunkbd = serio->private;
	input_unregister_device(&sunkbd->dev);
	serio_close(serio);
	kfree(sunkbd);
}

static struct serio_dev sunkbd_dev = {
	.interrupt =	sunkbd_interrupt,
	.connect =	sunkbd_connect,
	.disconnect =	sunkbd_disconnect
};

/*
 * The functions for insering/removing us as a module.
 */

int __init sunkbd_init(void)
{
	serio_register_device(&sunkbd_dev);
	return 0;
}

void __exit sunkbd_exit(void)
{
	serio_unregister_device(&sunkbd_dev);
}

module_init(sunkbd_init);
module_exit(sunkbd_exit);
