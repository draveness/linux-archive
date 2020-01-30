/*
 * $Id: gf2k.c,v 1.12 2000/06/04 14:53:44 vojtech Exp $
 *
 *  Copyright (c) 1998-2000 Vojtech Pavlik
 *
 *  Sponsored by SuSE
 */

/*
 * Genius Flight 2000 joystick driver for Linux
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
 * e-mail - mail your message to <vojtech@suse.cz>, or by paper mail:
 * Vojtech Pavlik, Ucitelska 1576, Prague 8, 182 00 Czech Republic
 */

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/malloc.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/gameport.h>

#define GF2K_START		400	/* The time we wait for the first bit [400 us] */
#define GF2K_STROBE		40	/* The time we wait for the first bit [40 us] */
#define GF2K_TIMEOUT		4	/* Wait for everything to settle [4 ms] */
#define GF2K_LENGTH		80	/* Max number of triplets in a packet */
#define GF2K_REFRESH		HZ/50	/* Time between joystick polls [20 ms] */

/*
 * Genius joystick ids ...
 */

#define GF2K_ID_G09		1
#define GF2K_ID_F30D		2
#define GF2K_ID_F30		3
#define GF2K_ID_F31D		4
#define GF2K_ID_F305		5
#define GF2K_ID_F23P		6
#define GF2K_ID_F31		7
#define GF2K_ID_MAX		7

static char gf2k_length[] = { 40, 40, 40, 40, 40, 40, 40, 40 };
static char gf2k_hat_to_axis[][2] = {{ 0, 0}, { 0,-1}, { 1,-1}, { 1, 0}, { 1, 1}, { 0, 1}, {-1, 1}, {-1, 0}, {-1,-1}};

static char *gf2k_names[] = {"", "Genius G-09D", "Genius F-30D", "Genius F-30", "Genius MaxFighter F-31D",
				"Genius F-30-5", "Genius Flight2000 F-23", "Genius F-31"};
static unsigned char gf2k_hats[] = { 0, 2, 0, 0, 2, 0, 2, 0 };
static unsigned char gf2k_axes[] = { 0, 2, 0, 0, 4, 0, 4, 0 };
static unsigned char gf2k_joys[] = { 0, 0, 0, 0,10, 0, 8, 0 };
static unsigned char gf2k_pads[] = { 0, 6, 0, 0, 0, 0, 0, 0 };
static unsigned char gf2k_lens[] = { 0,18, 0, 0,18, 0,18, 0 };

static unsigned char gf2k_abs[] = { ABS_X, ABS_Y, ABS_THROTTLE, ABS_RUDDER, ABS_GAS, ABS_BRAKE };
static short gf2k_btn_joy[] = { BTN_TRIGGER, BTN_THUMB, BTN_TOP, BTN_TOP2, BTN_BASE, BTN_BASE2, BTN_BASE3, BTN_BASE4 };
static short gf2k_btn_pad[] = { BTN_A, BTN_B, BTN_C, BTN_X, BTN_Y, BTN_Z, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2, BTN_START, BTN_SELECT };


static short gf2k_seq_reset[] = { 240, 340, 0 };
static short gf2k_seq_digital[] = { 590, 320, 860, 0 };

struct gf2k {
	struct gameport *gameport;
	struct timer_list timer;
	struct input_dev dev;
	int reads;
	int bads;
	int used;
	unsigned char id;
	unsigned char length;
};

/*
 * gf2k_read_packet() reads a Genius Flight2000 packet.
 */

static int gf2k_read_packet(struct gameport *gameport, int length, char *data)
{
	unsigned char u, v;
	int i;
	unsigned int t, p;
	unsigned long flags;

	t = gameport_time(gameport, GF2K_START);
	p = gameport_time(gameport, GF2K_STROBE);

	i = 0;

	__save_flags(flags);
	__cli();

	gameport_trigger(gameport);
	v = gameport_read(gameport);;

	while (t > 0 && i < length) {
		t--; u = v;
		v = gameport_read(gameport);
		if (v & ~u & 0x10) {
			data[i++] = v >> 5;
			t = p;
		}
	}

	__restore_flags(flags);

	return i;
}

/*
 * gf2k_trigger_seq() initializes a Genius Flight2000 joystick
 * into digital mode.
 */

static void gf2k_trigger_seq(struct gameport *gameport, short *seq)
{

	unsigned long flags;
	int i, t;

        __save_flags(flags);
        __cli();

	i = 0;
        do {
		gameport_trigger(gameport);
		t = gameport_time(gameport, GF2K_TIMEOUT * 1000);
		while ((gameport_read(gameport) & 1) && t) t--;
                udelay(seq[i]);
        } while (seq[++i]);

	gameport_trigger(gameport);

	__restore_flags(flags);
}

/*
 * js_sw_get_bits() composes bits from the triplet buffer into a __u64.
 * Parameter 'pos' is bit number inside packet where to start at, 'num' is number
 * of bits to be read, 'shift' is offset in the resulting __u64 to start at, bits
 * is number of bits per triplet.
 */

#define GB(p,n,s)	gf2k_get_bits(data, p, n, s)

static int gf2k_get_bits(unsigned char *buf, int pos, int num, int shift)
{
	__u64 data = 0;
	int i;

	for (i = 0; i < num / 3 + 2; i++)
		data |= buf[pos / 3 + i] << (i * 3);
	data >>= pos % 3;
	data &= (1 << num) - 1;
	data <<= shift;

	return data;
}

static void gf2k_read(struct gf2k *gf2k, unsigned char *data)
{
	struct input_dev *dev = &gf2k->dev;
	int i, t;

	for (i = 0; i < 4 && i < gf2k_axes[gf2k->id]; i++)
		input_report_abs(dev, gf2k_abs[i], GB(i<<3,8,0) | GB(i+46,1,8) | GB(i+50,1,9));

	for (i = 0; i < 2 && i < gf2k_axes[gf2k->id] - 4; i++)
		input_report_abs(dev, gf2k_abs[i], GB(i*9+60,8,0) | GB(i+54,1,9));

	t = GB(40,4,0);

	for (i = 0; i < gf2k_hats[gf2k->id]; i++)
		input_report_abs(dev, ABS_HAT0X + i, gf2k_hat_to_axis[t][i]);

	t = GB(44,2,0) | GB(32,8,2) | GB(78,2,10);

	for (i = 0; i < gf2k_joys[gf2k->id]; i++)
		input_report_key(dev, gf2k_btn_joy[i], (t >> i) & 1);

	for (i = 0; i < gf2k_pads[gf2k->id]; i++)
		input_report_key(dev, gf2k_btn_pad[i], (t >> i) & 1);
}

/*
 * gf2k_timer() reads and analyzes Genius joystick data.
 */

static void gf2k_timer(unsigned long private)
{
	struct gf2k *gf2k = (void *) private;
	unsigned char data[GF2K_LENGTH];

	gf2k->reads++;

	if (gf2k_read_packet(gf2k->gameport, gf2k_length[gf2k->id], data) < gf2k_length[gf2k->id]) {
		gf2k->bads++;
	} else gf2k_read(gf2k, data);

	mod_timer(&gf2k->timer, jiffies + GF2K_REFRESH);
}

static int gf2k_open(struct input_dev *dev)
{
	struct gf2k *gf2k = dev->private;
	if (!gf2k->used++)
		mod_timer(&gf2k->timer, jiffies + GF2K_REFRESH);	
	return 0;
}

static void gf2k_close(struct input_dev *dev)
{
	struct gf2k *gf2k = dev->private;
	if (!--gf2k->used)
		del_timer(&gf2k->timer);
}

/*
 * gf2k_connect() probes for Genius id joysticks.
 */

static void gf2k_connect(struct gameport *gameport, struct gameport_dev *dev)
{
	struct gf2k *gf2k;
	unsigned char data[GF2K_LENGTH];
	int i;

	if (!(gf2k = kmalloc(sizeof(struct gf2k), GFP_KERNEL)))
		return;
	memset(gf2k, 0, sizeof(struct gf2k));

	gameport->private = gf2k;

	gf2k->gameport = gameport;
	init_timer(&gf2k->timer);
	gf2k->timer.data = (long) gf2k;
	gf2k->timer.function = gf2k_timer;

	if (gameport_open(gameport, dev, GAMEPORT_MODE_RAW))
		goto fail1;

	gf2k_trigger_seq(gameport, gf2k_seq_reset);

	wait_ms(GF2K_TIMEOUT);

	gf2k_trigger_seq(gameport, gf2k_seq_digital);

	wait_ms(GF2K_TIMEOUT);

	if (gf2k_read_packet(gameport, GF2K_LENGTH, data) < 12)
		goto fail2;

	if (!(gf2k->id = GB(7,2,0) | GB(3,3,2) | GB(0,3,5)))
		goto fail2;

#ifdef RESET_WORKS
	if ((gf2k->id != (GB(19,2,0) | GB(15,3,2) | GB(12,3,5))) ||
	    (gf2k->id != (GB(31,2,0) | GB(27,3,2) | GB(24,3,5))))
		goto fail2;
#else
	gf2k->id = 6;
#endif

	if (gf2k->id > GF2K_ID_MAX || !gf2k_axes[gf2k->id]) {
		printk(KERN_WARNING "gf2k.c: Not yet supported joystick on gameport%d. [id: %d type:%s]\n",
			gameport->number, gf2k->id, gf2k->id > GF2K_ID_MAX ? "Unknown" : gf2k_names[gf2k->id]);
		goto fail2;
	}

	gf2k->length = gf2k_lens[gf2k->id];

	gf2k->dev.private = gf2k;
	gf2k->dev.open = gf2k_open;
	gf2k->dev.close = gf2k_close;
	gf2k->dev.evbit[0] = BIT(EV_KEY) | BIT(EV_ABS);

	gf2k->dev.name = gf2k_names[gf2k->id];
	gf2k->dev.idbus = BUS_GAMEPORT;
	gf2k->dev.idvendor = GAMEPORT_ID_VENDOR_GENIUS;
	gf2k->dev.idproduct = gf2k->id;
	gf2k->dev.idversion = 0x0100;

	for (i = 0; i < gf2k_axes[gf2k->id]; i++)
		set_bit(gf2k_abs[i], gf2k->dev.absbit);

	for (i = 0; i < gf2k_hats[gf2k->id]; i++) {
		set_bit(ABS_HAT0X + i, gf2k->dev.absbit);
		gf2k->dev.absmin[ABS_HAT0X + i] = -1;
		gf2k->dev.absmax[ABS_HAT0X + i] = 1;
	}

	for (i = 0; i < gf2k_joys[gf2k->id]; i++)
		set_bit(gf2k_btn_joy[i], gf2k->dev.keybit);

	for (i = 0; i < gf2k_pads[gf2k->id]; i++)
		set_bit(gf2k_btn_pad[i], gf2k->dev.keybit);

	gf2k_read_packet(gameport, gf2k->length, data);
	gf2k_read(gf2k, data);

	for (i = 0; i < gf2k_axes[gf2k->id]; i++) {
		gf2k->dev.absmax[gf2k_abs[i]] = (i < 2) ? gf2k->dev.abs[gf2k_abs[i]] * 2 - 32 :
	      		  gf2k->dev.abs[gf2k_abs[0]] + gf2k->dev.abs[gf2k_abs[1]] - 32; 
		gf2k->dev.absmin[gf2k_abs[i]] = 32;
		gf2k->dev.absfuzz[gf2k_abs[i]] = 8;
		gf2k->dev.absflat[gf2k_abs[i]] = (i < 2) ? 24 : 0;
	}

	input_register_device(&gf2k->dev);
	printk(KERN_INFO "input%d: %s on gameport%d.0\n",
		gf2k->dev.number, gf2k_names[gf2k->id], gameport->number);

	return;
fail2:	gameport_close(gameport);
fail1:	kfree(gf2k);
}

static void gf2k_disconnect(struct gameport *gameport)
{
	struct gf2k *gf2k = gameport->private;
	input_unregister_device(&gf2k->dev);
	gameport_close(gameport);
	kfree(gf2k);
}

static struct gameport_dev gf2k_dev = {
	connect:	gf2k_connect,
	disconnect:	gf2k_disconnect,
};

int __init gf2k_init(void)
{
	gameport_register_device(&gf2k_dev);
	return 0;
}

void __exit gf2k_exit(void)
{
	gameport_unregister_device(&gf2k_dev);
}

module_init(gf2k_init);
module_exit(gf2k_exit);
