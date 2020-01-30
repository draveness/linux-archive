/*
 * $Id: amijoy.c,v 1.13 2002/01/22 20:26:32 vojtech Exp $
 *
 *  Copyright (c) 1998-2001 Vojtech Pavlik
 */

/*
 * Driver for Amiga joysticks for Linux/m68k
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

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>

#include <asm/system.h>
#include <asm/amigahw.h>
#include <asm/amigaints.h>

MODULE_AUTHOR("Vojtech Pavlik <vojtech@ucw.cz>");
MODULE_DESCRIPTION("Driver for Amiga joysticks");
MODULE_LICENSE("GPL");

static int amijoy[2] = { 0, 1 };
static int amijoy_nargs;
module_param_array_named(map, amijoy, uint, amijoy_nargs, 0);
MODULE_PARM_DESC(map, "Map of attached joysticks in form of <a>,<b> (default is 0,1)");

__obsolete_setup("amijoy=");

static int amijoy_used[2] = { 0, 0 };
static struct input_dev amijoy_dev[2];
static char *amijoy_phys[2] = { "amijoy/input0", "amijoy/input1" };

static char *amijoy_name = "Amiga joystick";

static irqreturn_t amijoy_interrupt(int irq, void *dummy, struct pt_regs *fp)
{
	int i, data = 0, button = 0;

	for (i = 0; i < 2; i++)
		if (amijoy[i]) {

			switch (i) {
				case 0: data = ~custom.joy0dat; button = (~ciaa.pra >> 6) & 1; break;
				case 1: data = ~custom.joy1dat; button = (~ciaa.pra >> 7) & 1; break;
			}

			input_regs(amijoy_dev + i, fp);

			input_report_key(amijoy_dev + i, BTN_TRIGGER, button);

			input_report_abs(amijoy_dev + i, ABS_X, ((data >> 1) & 1) - ((data >> 9) & 1));
			data = ~(data ^ (data << 1));
			input_report_abs(amijoy_dev + i, ABS_Y, ((data >> 1) & 1) - ((data >> 9) & 1));

			input_sync(amijoy_dev + i);
		}
	return IRQ_HANDLED;
}

static int amijoy_open(struct input_dev *dev)
{
	int *used = dev->private;

	if ((*used)++)
		return 0;

	if (request_irq(IRQ_AMIGA_VERTB, amijoy_interrupt, 0, "amijoy", amijoy_interrupt)) {
		(*used)--;
		printk(KERN_ERR "amijoy.c: Can't allocate irq %d\n", IRQ_AMIGA_VERTB);
		return -EBUSY;
	}

	return 0;
}

static void amijoy_close(struct input_dev *dev)
{
	int *used = dev->private;

	if (!--(*used))
		free_irq(IRQ_AMIGA_VERTB, amijoy_interrupt);
}

static int __init amijoy_init(void)
{
	int i, j;

	for (i = 0; i < 2; i++)
		if (amijoy[i]) {
			if (!request_mem_region(CUSTOM_PHYSADDR+10+i*2, 2,
						"amijoy [Denise]")) {
				if (i == 1 && amijoy[0]) {
					input_unregister_device(amijoy_dev);
					release_mem_region(CUSTOM_PHYSADDR+10, 2);
				}
				return -EBUSY;
			}

			amijoy_dev[i].open = amijoy_open;
			amijoy_dev[i].close = amijoy_close;
			amijoy_dev[i].evbit[0] = BIT(EV_KEY) | BIT(EV_ABS);
			amijoy_dev[i].absbit[0] = BIT(ABS_X) | BIT(ABS_Y);
			amijoy_dev[i].keybit[LONG(BTN_LEFT)] = BIT(BTN_LEFT) | BIT(BTN_MIDDLE) | BIT(BTN_RIGHT);
			for (j = 0; j < 2; j++) {
				amijoy_dev[i].absmin[ABS_X + j] = -1;
				amijoy_dev[i].absmax[ABS_X + j] = 1;
			}

			amijoy_dev[i].name = amijoy_name;
			amijoy_dev[i].phys = amijoy_phys[i];
			amijoy_dev[i].id.bustype = BUS_AMIGA;
			amijoy_dev[i].id.vendor = 0x0001;
			amijoy_dev[i].id.product = 0x0003;
			amijoy_dev[i].id.version = 0x0100;

			amijoy_dev[i].private = amijoy_used + i;

			input_register_device(amijoy_dev + i);
			printk(KERN_INFO "input: %s at joy%ddat\n", amijoy_name, i);
		}
	return 0;
}

static void __exit amijoy_exit(void)
{
	int i;

	for (i = 0; i < 2; i++)
		if (amijoy[i]) {
			input_unregister_device(amijoy_dev + i);
			release_mem_region(CUSTOM_PHYSADDR+10+i*2, 2);
		}
}

module_init(amijoy_init);
module_exit(amijoy_exit);
