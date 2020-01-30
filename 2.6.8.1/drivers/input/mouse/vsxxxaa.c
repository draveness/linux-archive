/*
 * DEC VSXXX-AA and VSXXX-GA mouse driver.
 *
 * Copyright (C) 2003-2004 by Jan-Benedict Glaw <jbglaw@lug-owl.de>
 *
 * The packet format was taken from a patch to GPM which is (C) 2001
 * by	Karsten Merker <merker@linuxtag.org>
 * and	Maciej W. Rozycki <macro@ds2.pg.gda.pl>
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
 */

/*
 * Building an adaptor to DB9 / DB25 RS232
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 * DISCLAIMER: Use this description AT YOUR OWN RISK! I'll not pay for
 * anything if you break your mouse, your computer or whatever!
 *
 * In theory, this mouse is a simple RS232 device. In practice, it has got
 * a quite uncommon plug and the requirement to additionally get a power
 * supply at +5V and -12V.
 *
 * If you look at the socket/jack (_not_ at the plug), we use this pin
 * numbering:
 *    _______
 *   / 7 6 5 \
 *  | 4 --- 3 |
 *   \  2 1  /
 *    -------
 * 
 *	DEC socket	DB9	DB25	Note
 *	1 (GND)		5	7	-
 *	2 (RxD)		2	3	-
 *	3 (TxD)		3	2	-
 *	4 (-12V)	-	-	Somewhere from the PSU. At ATX, it's
 *					the thin blue wire at pin 12 of the
 *					ATX power connector. Only required for
 *					VSXXX-AA/-GA mice.
 *	5 (+5V)		-	-	PSU (red wires of ATX power connector
 *					on pin 4, 6, 19 or 20) or HDD power
 *					connector (also red wire).
 *	6 (+12V)	-	-	HDD power connector, yellow wire. Only
 *					required for VSXXX-AB digitizer.
 *	7 (dev. avail.)	-	-	The mouse shorts this one to pin 1.
 *					This way, the host computer can detect
 *					the mouse. To use it with the adaptor,
 *					simply don't connect this pin.
 *
 * So to get a working adaptor, you need to connect the mouse with three
 * wires to a RS232 port and two or three additional wires for +5V, +12V and
 * -12V to the PSU.
 *
 * Flow specification for the link is 4800, 8o1.
 *
 * The mice and tablet are described in "VCB02 Video Subsystem - Technical
 * Manual", DEC EK-104AA-TM-001. You'll find it at MANX, a search engine
 * specific for DEC documentation. Try
 * http://www.vt100.net/manx/details?pn=EK-104AA-TM-001;id=21;cp=1
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/config.h>
#include <linux/serio.h>
#include <linux/init.h>

MODULE_AUTHOR ("Jan-Benedict Glaw <jbglaw@lug-owl.de>");
MODULE_DESCRIPTION ("Serial DEC VSXXX-AA/GA mouse / DEC tablet driver");
MODULE_LICENSE ("GPL");

#undef VSXXXAA_DEBUG
#ifdef VSXXXAA_DEBUG
#define DBG(x...) printk (x)
#else
#define DBG(x...) do {} while (0)
#endif

#define VSXXXAA_INTRO_MASK	0x80
#define VSXXXAA_INTRO_HEAD	0x80
#define IS_HDR_BYTE(x)		(((x) & VSXXXAA_INTRO_MASK)	\
					== VSXXXAA_INTRO_HEAD)

#define VSXXXAA_PACKET_MASK	0xe0
#define VSXXXAA_PACKET_REL	0x80
#define VSXXXAA_PACKET_ABS	0xc0
#define VSXXXAA_PACKET_POR	0xa0
#define MATCH_PACKET_TYPE(data, type)	(((data) & VSXXXAA_PACKET_MASK) == type)



struct vsxxxaa {
	struct input_dev dev;
	struct serio *serio;
#define BUFLEN 15 /* At least 5 is needed for a full tablet packet */
	unsigned char buf[BUFLEN];
	unsigned char count;
	unsigned char version;
	unsigned char country;
	unsigned char type;
	char name[64];
	char phys[32];
};

static void
vsxxxaa_drop_bytes (struct vsxxxaa *mouse, int num)
{
	if (num >= mouse->count)
		mouse->count = 0;
	else {
		memmove (mouse->buf, mouse->buf + num - 1, BUFLEN - num);
		mouse->count -= num;
	}
}

static void
vsxxxaa_queue_byte (struct vsxxxaa *mouse, unsigned char byte)
{
	if (mouse->count == BUFLEN) {
		printk (KERN_ERR "%s on %s: Dropping a byte of full buffer.\n",
				mouse->name, mouse->phys);
		vsxxxaa_drop_bytes (mouse, 1);
	}
	DBG (KERN_INFO "Queueing byte 0x%02x\n", byte);

	mouse->buf[mouse->count++] = byte;
}

static void
vsxxxaa_detection_done (struct vsxxxaa *mouse)
{
	switch (mouse->type) {
		case 0x02:
			sprintf (mouse->name, "DEC VSXXX-AA/GA mouse");
			break;

		case 0x04:
			sprintf (mouse->name, "DEC VSXXX-AB digitizer");
			break;

		default:
			sprintf (mouse->name, "unknown DEC pointer device");
			break;
	}

	printk (KERN_INFO "Found %s version 0x%02x from country 0x%02x "
			"on port %s\n", mouse->name, mouse->version,
			mouse->country, mouse->phys);
}

/*
 * Returns number of bytes to be dropped, 0 if packet is okay.
 */
static int
vsxxxaa_check_packet (struct vsxxxaa *mouse, int packet_len)
{
	int i;

	/* First byte must be a header byte */
	if (!IS_HDR_BYTE (mouse->buf[0])) {
		DBG ("vsck: len=%d, 1st=0x%02x\n", packet_len, mouse->buf[0]);
		return 1;
	}

	/* Check all following bytes */
	if (packet_len > 1) {
		for (i = 1; i < packet_len; i++) {
			if (IS_HDR_BYTE (mouse->buf[i])) {
				printk (KERN_ERR "Need to drop %d bytes "
						"of a broken packet.\n",
						i - 1);
				DBG (KERN_INFO "check: len=%d, b[%d]=0x%02x\n",
						packet_len, i, mouse->buf[i]);
				return i - 1;
			}
		}
	}

	return 0;
}

static __inline__ int
vsxxxaa_smells_like_packet (struct vsxxxaa *mouse, unsigned char type, size_t len)
{
	return (mouse->count >= len) && MATCH_PACKET_TYPE (mouse->buf[0], type);
}

static void
vsxxxaa_handle_REL_packet (struct vsxxxaa *mouse, struct pt_regs *regs)
{
	struct input_dev *dev = &mouse->dev;
	unsigned char *buf = mouse->buf;
	int left, middle, right;
	int dx, dy;

	/*
	 * Check for normal stream packets. This is three bytes,
	 * with the first byte's 3 MSB set to 100.
	 *
	 * [0]:	1	0	0	SignX	SignY	Left	Middle	Right
	 * [1]: 0	dx	dx	dx	dx	dx	dx	dx
	 * [2]:	0	dy	dy	dy	dy	dy	dy	dy
	 */

	/*
	 * Low 7 bit of byte 1 are abs(dx), bit 7 is
	 * 0, bit 4 of byte 0 is direction.
	 */
	dx = buf[1] & 0x7f;
	dx *= ((buf[0] >> 4) & 0x01)? 1: -1;

	/*
	 * Low 7 bit of byte 2 are abs(dy), bit 7 is
	 * 0, bit 3 of byte 0 is direction.
	 */
	dy = buf[2] & 0x7f;
	dy *= ((buf[0] >> 3) & 0x01)? -1: 1;

	/*
	 * Get button state. It's the low three bits
	 * (for three buttons) of byte 0.
	 */
	left	= (buf[0] & 0x04)? 1: 0;
	middle	= (buf[0] & 0x02)? 1: 0;
	right	= (buf[0] & 0x01)? 1: 0;

	vsxxxaa_drop_bytes (mouse, 3);

	DBG (KERN_INFO "%s on %s: dx=%d, dy=%d, buttons=%s%s%s\n",
			mouse->name, mouse->phys, dx, dy,
			left? "L": "l", middle? "M": "m", right? "R": "r");

	/*
	 * Report what we've found so far...
	 */
	input_regs (dev, regs);
	input_report_key (dev, BTN_LEFT, left);
	input_report_key (dev, BTN_MIDDLE, middle);
	input_report_key (dev, BTN_RIGHT, right);
	input_report_key (dev, BTN_TOUCH, 0);
	input_report_rel (dev, REL_X, dx);
	input_report_rel (dev, REL_Y, dy);
	input_sync (dev);
}

static void
vsxxxaa_handle_ABS_packet (struct vsxxxaa *mouse, struct pt_regs *regs)
{
	struct input_dev *dev = &mouse->dev;
	unsigned char *buf = mouse->buf;
	int left, middle, right, touch;
	int x, y;

	/*
	 * Tablet position / button packet
	 *
	 * [0]:	1	1	0	B4	B3	B2	B1	Pr
	 * [1]:	0	0	X5	X4	X3	X2	X1	X0
	 * [2]:	0	0	X11	X10	X9	X8	X7	X6
	 * [3]:	0	0	Y5	Y4	Y3	Y2	Y1	Y0
	 * [4]:	0	0	Y11	Y10	Y9	Y8	Y7	Y6
	 */

	/*
	 * Get X/Y position. Y axis needs to be inverted since VSXXX-AB
	 * counts down->top while monitor counts top->bottom.
	 */
	x = ((buf[2] & 0x3f) << 6) | (buf[1] & 0x3f);
	y = ((buf[4] & 0x3f) << 6) | (buf[3] & 0x3f);
	y = 1023 - y;

	/*
	 * Get button state. It's bits <4..1> of byte 0.
	 */
	left	= (buf[0] & 0x02)? 1: 0;
	middle	= (buf[0] & 0x04)? 1: 0;
	right	= (buf[0] & 0x08)? 1: 0;
	touch	= (buf[0] & 0x10)? 1: 0;

	vsxxxaa_drop_bytes (mouse, 5);

	DBG (KERN_INFO "%s on %s: x=%d, y=%d, buttons=%s%s%s%s\n",
			mouse->name, mouse->phys, x, y,
			left? "L": "l", middle? "M": "m",
			right? "R": "r", touch? "T": "t");

	/*
	 * Report what we've found so far...
	 */
	input_regs (dev, regs);
	input_report_key (dev, BTN_LEFT, left);
	input_report_key (dev, BTN_MIDDLE, middle);
	input_report_key (dev, BTN_RIGHT, right);
	input_report_key (dev, BTN_TOUCH, touch);
	input_report_abs (dev, ABS_X, x);
	input_report_abs (dev, ABS_Y, y);
	input_sync (dev);
}

static void
vsxxxaa_handle_POR_packet (struct vsxxxaa *mouse, struct pt_regs *regs)
{
	struct input_dev *dev = &mouse->dev;
	unsigned char *buf = mouse->buf;
	int left, middle, right;
	unsigned char error;

	/*
	 * Check for Power-On-Reset packets. These are sent out
	 * after plugging the mouse in, or when explicitely
	 * requested by sending 'T'.
	 *
	 * [0]:	1	0	1	0	R3	R2	R1	R0
	 * [1]:	0	M2	M1	M0	D3	D2	D1	D0
	 * [2]:	0	E6	E5	E4	E3	E2	E1	E0
	 * [3]:	0	0	0	0	0	Left	Middle	Right
	 *
	 * M: manufacturer location code
	 * R: revision code
	 * E: Error code. I'm not sure about these, but gpm's sources,
	 *    which support this mouse, too, tell about them:
	 *	E = [0x00 .. 0x1f]: no error, byte #3 is button state
	 *	E = 0x3d: button error, byte #3 tells which one.
	 *	E = <else>: other error
	 * D: <0010> == mouse, <0100> == tablet
	 *
	 */

	mouse->version = buf[0] & 0x0f;
	mouse->country = (buf[1] >> 4) & 0x07;
	mouse->type = buf[1] & 0x0f;
	error = buf[2] & 0x7f;

	/*
	 * Get button state. It's the low three bits
	 * (for three buttons) of byte 0. Maybe even the bit <3>
	 * has some meaning if a tablet is attached.
	 */
	left	= (buf[0] & 0x04)? 1: 0;
	middle	= (buf[0] & 0x02)? 1: 0;
	right	= (buf[0] & 0x01)? 1: 0;

	vsxxxaa_drop_bytes (mouse, 4);
	vsxxxaa_detection_done (mouse);

	if (error <= 0x1f) {
		/* No error. Report buttons */
		input_regs (dev, regs);
		input_report_key (dev, BTN_LEFT, left);
		input_report_key (dev, BTN_MIDDLE, middle);
		input_report_key (dev, BTN_RIGHT, right);
		input_report_key (dev, BTN_TOUCH, 0);
		input_sync (dev);
	} else {
		printk (KERN_ERR "Your %s on %s reports an undefined error, "
				"please check it...\n", mouse->name,
				mouse->phys);
	}

	/*
	 * If the mouse was hot-plugged, we need to force differential mode
	 * now... However, give it a second to recover from it's reset.
	 */
	printk (KERN_NOTICE "%s on %s: Forceing standard packet format and "
			"streaming mode\n", mouse->name, mouse->phys);
	mouse->serio->write (mouse->serio, 'S');
	mdelay (50);
	mouse->serio->write (mouse->serio, 'R');
}

static void
vsxxxaa_parse_buffer (struct vsxxxaa *mouse, struct pt_regs *regs)
{
	unsigned char *buf = mouse->buf;
	int stray_bytes;

	/*
	 * Parse buffer to death...
	 */
	do {
		/*
		 * Out of sync? Throw away what we don't understand. Each
		 * packet starts with a byte whose bit 7 is set. Unhandled
		 * packets (ie. which we don't know about or simply b0rk3d
		 * data...) will get shifted out of the buffer after some
		 * activity on the mouse.
		 */
		while (mouse->count > 0 && !IS_HDR_BYTE(buf[0])) {
			printk (KERN_ERR "%s on %s: Dropping a byte to regain "
					"sync with mouse data stream...\n",
					mouse->name, mouse->phys);
			vsxxxaa_drop_bytes (mouse, 1);
		}

		/*
		 * Check for packets we know about.
		 */

		if (vsxxxaa_smells_like_packet (mouse, VSXXXAA_PACKET_REL, 3)) {
			/* Check for broken packet */
			stray_bytes = vsxxxaa_check_packet (mouse, 3);
			if (stray_bytes > 0) {
				printk (KERN_ERR "Dropping %d bytes now...\n",
						stray_bytes);
				vsxxxaa_drop_bytes (mouse, stray_bytes);
				continue;
			}

			vsxxxaa_handle_REL_packet (mouse, regs);
			continue; /* More to parse? */
		}

		if (vsxxxaa_smells_like_packet (mouse, VSXXXAA_PACKET_ABS, 5)) {
			/* Check for broken packet */
			stray_bytes = vsxxxaa_check_packet (mouse, 5);
			if (stray_bytes > 0) {
				printk (KERN_ERR "Dropping %d bytes now...\n",
						stray_bytes);
				vsxxxaa_drop_bytes (mouse, stray_bytes);
				continue;
			}

			vsxxxaa_handle_ABS_packet (mouse, regs);
			continue; /* More to parse? */
		}

		if (vsxxxaa_smells_like_packet (mouse, VSXXXAA_PACKET_POR, 4)) {
			/* Check for broken packet */
			stray_bytes = vsxxxaa_check_packet (mouse, 4);
			if (stray_bytes > 0) {
				printk (KERN_ERR "Dropping %d bytes now...\n",
						stray_bytes);
				vsxxxaa_drop_bytes (mouse, stray_bytes);
				continue;
			}

			vsxxxaa_handle_POR_packet (mouse, regs);
			continue; /* More to parse? */
		}

		break; /* No REL, ABS or POR packet found */
	} while (1);
}

static irqreturn_t
vsxxxaa_interrupt (struct serio *serio, unsigned char data, unsigned int flags,
		struct pt_regs *regs)
{
	struct vsxxxaa *mouse = serio->private;

	vsxxxaa_queue_byte (mouse, data);
	vsxxxaa_parse_buffer (mouse, regs);

	return IRQ_HANDLED;
}

static void
vsxxxaa_disconnect (struct serio *serio)
{
	struct vsxxxaa *mouse = serio->private;

	input_unregister_device (&mouse->dev);
	serio_close (serio);
	kfree (mouse);
}

static void
vsxxxaa_connect (struct serio *serio, struct serio_dev *dev)
{
	struct vsxxxaa *mouse;

	if ((serio->type & SERIO_TYPE) != SERIO_RS232)
		return;
	if ((serio->type & SERIO_PROTO) != SERIO_VSXXXAA)
		return;

	if (!(mouse = kmalloc (sizeof (struct vsxxxaa), GFP_KERNEL)))
		return;

	memset (mouse, 0, sizeof (struct vsxxxaa));

	init_input_dev (&mouse->dev);
	set_bit (EV_KEY, mouse->dev.evbit);		/* We have buttons */
	set_bit (EV_REL, mouse->dev.evbit);
	set_bit (EV_ABS, mouse->dev.evbit);
	set_bit (BTN_LEFT, mouse->dev.keybit);		/* We have 3 buttons */
	set_bit (BTN_MIDDLE, mouse->dev.keybit);
	set_bit (BTN_RIGHT, mouse->dev.keybit);
	set_bit (BTN_TOUCH, mouse->dev.keybit);		/* ...and Tablet */
	set_bit (REL_X, mouse->dev.relbit);
	set_bit (REL_Y, mouse->dev.relbit);
	set_bit (ABS_X, mouse->dev.absbit);
	set_bit (ABS_Y, mouse->dev.absbit);

	mouse->dev.absmin[ABS_X] = 0;
	mouse->dev.absmax[ABS_X] = 1023;
	mouse->dev.absmin[ABS_Y] = 0;
	mouse->dev.absmax[ABS_Y] = 1023;

	mouse->dev.private = mouse;
	serio->private = mouse;

	sprintf (mouse->name, "DEC VSXXX-AA/GA mouse or VSXXX-AB digitizer");
	sprintf (mouse->phys, "%s/input0", serio->phys);
	mouse->dev.name = mouse->name;
	mouse->dev.phys = mouse->phys;
	mouse->dev.id.bustype = BUS_RS232;
	mouse->serio = serio;

	if (serio_open (serio, dev)) {
		kfree (mouse);
		return;
	}

	/*
	 * Request selftest. Standard packet format and differential
	 * mode will be requested after the device ID'ed successfully.
	 */
	mouse->serio->write (mouse->serio, 'T'); /* Test */

	input_register_device (&mouse->dev);

	printk (KERN_INFO "input: %s on %s\n", mouse->name, mouse->phys);
}

static struct serio_dev vsxxxaa_dev = {
	.connect = vsxxxaa_connect,
	.interrupt = vsxxxaa_interrupt,
	.disconnect = vsxxxaa_disconnect,
};

int __init
vsxxxaa_init (void)
{
	serio_register_device (&vsxxxaa_dev);
	return 0;
}

void __exit
vsxxxaa_exit (void)
{
	serio_unregister_device (&vsxxxaa_dev);
}

module_init (vsxxxaa_init);
module_exit (vsxxxaa_exit);

