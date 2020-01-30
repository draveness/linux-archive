/*
 *  Copyright (c) 2000 Justin Cormack
 */

/*
 * Newton keyboard driver for Linux
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Should you need to contact me, the author, you can do so either by
 * e-mail - mail your message to <j.cormack@doc.ic.ac.uk>, or by paper mail:
 * Justin Cormack, 68 Dartmouth Park Road, London NW5 1SN, UK.
 */

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/init.h>
#include <linux/serio.h>

MODULE_AUTHOR("Justin Cormack <j.cormack@doc.ic.ac.uk>");
MODULE_DESCRIPTION("Newton keyboard driver");
MODULE_LICENSE("GPL");

#define NKBD_KEY	0x7f
#define NKBD_PRESS	0x80

static unsigned char nkbd_keycode[128] = {
	KEY_A, KEY_S, KEY_D, KEY_F, KEY_H, KEY_G, KEY_Z, KEY_X,
	KEY_C, KEY_V, 0, KEY_B, KEY_Q, KEY_W, KEY_E, KEY_R,
	KEY_Y, KEY_T, KEY_1, KEY_2, KEY_3, KEY_4, KEY_6, KEY_5,
	KEY_EQUAL, KEY_9, KEY_7, KEY_MINUS, KEY_8, KEY_0, KEY_RIGHTBRACE, KEY_O,
	KEY_U, KEY_LEFTBRACE, KEY_I, KEY_P, KEY_ENTER, KEY_L, KEY_J, KEY_APOSTROPHE,
	KEY_K, KEY_SEMICOLON, KEY_BACKSLASH, KEY_COMMA, KEY_SLASH, KEY_N, KEY_M, KEY_DOT,
	KEY_TAB, KEY_SPACE, KEY_GRAVE, KEY_DELETE, 0, 0, 0, KEY_LEFTMETA,
	KEY_LEFTSHIFT, KEY_CAPSLOCK, KEY_LEFTALT, KEY_LEFTCTRL, KEY_RIGHTSHIFT, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	KEY_LEFT, KEY_RIGHT, KEY_DOWN, KEY_UP, 0
};

static char *nkbd_name = "Newton Keyboard";

struct nkbd {
	unsigned char keycode[128];
	struct input_dev dev;
	struct serio *serio;
	char phys[32];
};

irqreturn_t nkbd_interrupt(struct serio *serio,
		unsigned char data, unsigned int flags, struct pt_regs *regs)
{
	struct nkbd *nkbd = serio->private;

	/* invalid scan codes are probably the init sequence, so we ignore them */
	if (nkbd->keycode[data & NKBD_KEY]) {
		input_regs(&nkbd->dev, regs);
		input_report_key(&nkbd->dev, nkbd->keycode[data & NKBD_KEY], data & NKBD_PRESS);
		input_sync(&nkbd->dev);
	}

	else if (data == 0xe7) /* end of init sequence */
		printk(KERN_INFO "input: %s on %s\n", nkbd_name, serio->phys);
	return IRQ_HANDLED;

}

void nkbd_connect(struct serio *serio, struct serio_dev *dev)
{
	struct nkbd *nkbd;
	int i;

	if (serio->type != (SERIO_RS232 | SERIO_NEWTON))
		return;

	if (!(nkbd = kmalloc(sizeof(struct nkbd), GFP_KERNEL)))
		return;

	memset(nkbd, 0, sizeof(struct nkbd));

	nkbd->dev.evbit[0] = BIT(EV_KEY) | BIT(EV_REP);

	nkbd->serio = serio;

	init_input_dev(&nkbd->dev);
	nkbd->dev.keycode = nkbd->keycode;
	nkbd->dev.keycodesize = sizeof(unsigned char);
	nkbd->dev.keycodemax = ARRAY_SIZE(nkbd_keycode);
	nkbd->dev.private = nkbd;
	serio->private = nkbd;

	if (serio_open(serio, dev)) {
		kfree(nkbd);
		return;
	}

	memcpy(nkbd->keycode, nkbd_keycode, sizeof(nkbd->keycode));
	for (i = 0; i < 128; i++)
		set_bit(nkbd->keycode[i], nkbd->dev.keybit);
	clear_bit(0, nkbd->dev.keybit);

	sprintf(nkbd->phys, "%s/input0", serio->phys);

	nkbd->dev.name = nkbd_name;
	nkbd->dev.phys = nkbd->phys;
	nkbd->dev.id.bustype = BUS_RS232;
	nkbd->dev.id.vendor = SERIO_NEWTON;
	nkbd->dev.id.product = 0x0001;
	nkbd->dev.id.version = 0x0100;

	input_register_device(&nkbd->dev);

	printk(KERN_INFO "input: %s on %s\n", nkbd_name, serio->phys);
}

void nkbd_disconnect(struct serio *serio)
{
	struct nkbd *nkbd = serio->private;
	input_unregister_device(&nkbd->dev);
	serio_close(serio);
	kfree(nkbd);
}

struct serio_dev nkbd_dev = {
	.interrupt =	nkbd_interrupt,
	.connect =	nkbd_connect,
	.disconnect =	nkbd_disconnect
};

int __init nkbd_init(void)
{
	serio_register_device(&nkbd_dev);
	return 0;
}

void __exit nkbd_exit(void)
{
	serio_unregister_device(&nkbd_dev);
}

module_init(nkbd_init);
module_exit(nkbd_exit);
