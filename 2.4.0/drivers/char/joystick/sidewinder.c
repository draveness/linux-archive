/*
 * $Id: sidewinder.c,v 1.16 2000/07/14 09:02:41 vojtech Exp $
 *
 *  Copyright (c) 1998-2000 Vojtech Pavlik
 *
 *  Sponsored by SuSE
 */

/*
 * Microsoft SideWinder joystick family driver for Linux
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
#include <linux/module.h>
#include <linux/malloc.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/gameport.h>

/*
 * These are really magic values. Changing them can make a problem go away,
 * as well as break everything.
 */

#undef SW_DEBUG

#define SW_START	400	/* The time we wait for the first bit [400 us] */
#define SW_STROBE	45	/* Max time per bit [45 us] */
#define SW_TIMEOUT	4000	/* Wait for everything to settle [4 ms] */
#define SW_KICK		45	/* Wait after A0 fall till kick [45 us] */
#define SW_END		8	/* Number of bits before end of packet to kick */
#define SW_FAIL		16	/* Number of packet read errors to fail and reinitialize */
#define SW_BAD		2	/* Number of packet read errors to switch off 3d Pro optimization */
#define SW_OK		64	/* Number of packet read successes to switch optimization back on */
#define SW_LENGTH	512	/* Max number of bits in a packet */
#define SW_REFRESH	HZ/50	/* Time to wait between updates of joystick data [20 ms] */

#ifdef SW_DEBUG
#define dbg(format, arg...) printk(KERN_DEBUG __FILE__ ": " format "\n" , ## arg)
#else
#define dbg(format, arg...) do {} while (0)
#endif

/*
 * SideWinder joystick types ...
 */

#define SW_ID_3DP	0
#define SW_ID_GP	1
#define SW_ID_PP	2
#define SW_ID_FFP	3
#define SW_ID_FSP	4
#define SW_ID_FFW	5

/*
 * Names, buttons, axes ...
 */

static char *sw_name[] = {	"3D Pro", "GamePad", "Precision Pro", "Force Feedback Pro", "FreeStyle Pro",
				"Force Feedback Wheel" };

static char sw_abs[][7] = {
	{ ABS_X, ABS_Y, ABS_RZ, ABS_THROTTLE, ABS_HAT0X, ABS_HAT0Y },
	{ ABS_X, ABS_Y },
	{ ABS_X, ABS_Y, ABS_RZ, ABS_THROTTLE, ABS_HAT0X, ABS_HAT0Y },
	{ ABS_X, ABS_Y, ABS_RZ, ABS_THROTTLE, ABS_HAT0X, ABS_HAT0Y },
	{ ABS_X, ABS_Y,         ABS_THROTTLE, ABS_HAT0X, ABS_HAT0Y },
	{ ABS_RX, ABS_RUDDER,   ABS_THROTTLE }};

static char sw_bit[][7] = {
	{ 10, 10,  9, 10,  1,  1 },
	{  1,  1                 },
	{ 10, 10,  6,  7,  1,  1 },
	{ 10, 10,  6,  7,  1,  1 },
	{ 10, 10,  6,  1,  1     },
	{ 10,  7,  7,  1,  1     }};

static short sw_btn[][12] = {
	{ BTN_TRIGGER, BTN_TOP, BTN_THUMB, BTN_THUMB2, BTN_BASE, BTN_BASE2, BTN_BASE3, BTN_BASE4, BTN_MODE },
	{ BTN_A, BTN_B, BTN_C, BTN_X, BTN_Y, BTN_Z, BTN_TL, BTN_TR, BTN_START, BTN_MODE },
	{ BTN_TRIGGER, BTN_THUMB, BTN_TOP, BTN_TOP2, BTN_BASE, BTN_BASE2, BTN_BASE3, BTN_BASE4, BTN_SELECT },
	{ BTN_TRIGGER, BTN_THUMB, BTN_TOP, BTN_TOP2, BTN_BASE, BTN_BASE2, BTN_BASE3, BTN_BASE4, BTN_SELECT },
	{ BTN_A, BTN_B, BTN_C, BTN_X, BTN_Y, BTN_Z, BTN_TL, BTN_TR, BTN_START, BTN_MODE, BTN_SELECT },
	{ BTN_TRIGGER, BTN_TOP, BTN_THUMB, BTN_THUMB2, BTN_BASE, BTN_BASE2, BTN_BASE3, BTN_BASE4 }};

static struct {
	int x;
	int y;
} sw_hat_to_axis[] = {{ 0, 0}, { 0,-1}, { 1,-1}, { 1, 0}, { 1, 1}, { 0, 1}, {-1, 1}, {-1, 0}, {-1,-1}};

struct sw {
	struct gameport *gameport;
	struct timer_list timer;
	struct input_dev dev[4];
	char name[64];
	int length;
	int type;
	int bits;
	int number;
	int fail;
	int ok;
	int reads;
	int bads;
	int used;
};

/*
 * sw_read_packet() is a function which reads either a data packet, or an
 * identification packet from a SideWinder joystick. The protocol is very,
 * very, very braindamaged. Microsoft patented it in US patent #5628686.
 */

static int sw_read_packet(struct gameport *gameport, unsigned char *buf, int length, int id)
{
	unsigned long flags;
	int timeout, bitout, sched, i, kick, start, strobe;
	unsigned char pending, u, v;

	i = -id;						/* Don't care about data, only want ID */
	timeout = id ? gameport_time(gameport, SW_TIMEOUT) : 0;	/* Set up global timeout for ID packet */
	kick = id ? gameport_time(gameport, SW_KICK) : 0;	/* Set up kick timeout for ID packet */
	start = gameport_time(gameport, SW_START);
	strobe = gameport_time(gameport, SW_STROBE);
	bitout = start;
	pending = 0;
	sched = 0;

        __save_flags(flags);					/* Quiet, please */
        __cli();

	gameport_trigger(gameport);				/* Trigger */
	v = gameport_read(gameport);

	do {
		bitout--;
		u = v;
		v = gameport_read(gameport);
	} while (!(~v & u & 0x10) && (bitout > 0));		/* Wait for first falling edge on clock */

	if (bitout > 0) bitout = strobe;			/* Extend time if not timed out */

	while ((timeout > 0 || bitout > 0) && (i < length)) {

		timeout--;
		bitout--;					/* Decrement timers */
		sched--;

		u = v;
		v = gameport_read(gameport);

		if ((~u & v & 0x10) && (bitout > 0)) {		/* Rising edge on clock - data bit */
			if (i >= 0)				/* Want this data */
				buf[i] = v >> 5;		/* Store it */
			i++;					/* Advance index */
			bitout = strobe;			/* Extend timeout for next bit */
		} 

		if (kick && (~v & u & 0x01)) {			/* Falling edge on axis 0 */
			sched = kick;				/* Schedule second trigger */
			kick = 0;				/* Don't schedule next time on falling edge */
			pending = 1;				/* Mark schedule */
		} 

		if (pending && sched < 0 && (i > -SW_END)) {	/* Second trigger time */
			gameport_trigger(gameport);		/* Trigger */
			bitout = start;				/* Long bit timeout */
			pending = 0;				/* Unmark schedule */
			timeout = 0;				/* Switch from global to bit timeouts */ 
		}
	}

	__restore_flags(flags);					/* Done - relax */

#ifdef SW_DEBUG
	{
		int j;
		printk(KERN_DEBUG "sidewinder.c: Read %d triplets. [", i);
		for (j = 0; j < i; j++) printk("%d", buf[j]);
		printk("]\n");
	}
#endif

	return i;
}

/*
 * sw_get_bits() and GB() compose bits from the triplet buffer into a __u64.
 * Parameter 'pos' is bit number inside packet where to start at, 'num' is number
 * of bits to be read, 'shift' is offset in the resulting __u64 to start at, bits
 * is number of bits per triplet.
 */

#define GB(pos,num) sw_get_bits(buf, pos, num, sw->bits)

static __u64 sw_get_bits(unsigned char *buf, int pos, int num, char bits)
{
	__u64 data = 0;
	int tri = pos % bits;						/* Start position */
	int i   = pos / bits;
	int bit = 0;

	while (num--) {
		data |= (__u64)((buf[i] >> tri++) & 1) << bit++;	/* Transfer bit */
		if (tri == bits) {
			i++;						/* Next triplet */
			tri = 0;
		}
	}

	return data;
}

/*
 * sw_init_digital() initializes a SideWinder 3D Pro joystick
 * into digital mode.
 */

static void sw_init_digital(struct gameport *gameport)
{
	int seq[] = { 140, 140+725, 140+300, 0 };
	unsigned long flags;
	int i, t;

        __save_flags(flags);
        __cli();

	i = 0;
        do {
                gameport_trigger(gameport);			/* Trigger */
		t = gameport_time(gameport, SW_TIMEOUT);
		while ((gameport_read(gameport) & 1) && t) t--;	/* Wait for axis to fall back to 0 */
                udelay(seq[i]);					/* Delay magic time */
        } while (seq[++i]);

	gameport_trigger(gameport);				/* Last trigger */

	__restore_flags(flags);
}

/*
 * sw_parity() computes parity of __u64
 */

static int sw_parity(__u64 t)
{
	int x = t ^ (t >> 32);
	x ^= x >> 16;
	x ^= x >> 8;
	x ^= x >> 4;
	x ^= x >> 2;
	x ^= x >> 1;
	return x & 1;
}

/*
 * sw_ccheck() checks synchronization bits and computes checksum of nibbles.
 */

static int sw_check(__u64 t)
{
	unsigned char sum = 0;

	if ((t & 0x8080808080808080ULL) ^ 0x80)			/* Sync */
		return -1;

	while (t) {						/* Sum */
		sum += t & 0xf;
		t >>= 4;
	}

	return sum & 0xf;
}

/*
 * sw_parse() analyzes SideWinder joystick data, and writes the results into
 * the axes and buttons arrays.
 */

static int sw_parse(unsigned char *buf, struct sw *sw)
{
	int hat, i, j;
	struct input_dev *dev = sw->dev;

	switch (sw->type) {

		case SW_ID_3DP:

			if (sw_check(GB(0,64)) || (hat = (GB(6,1) << 3) | GB(60,3)) > 8) return -1;

			input_report_abs(dev, ABS_X,        (GB( 3,3) << 7) | GB(16,7));
			input_report_abs(dev, ABS_Y,        (GB( 0,3) << 7) | GB(24,7));
			input_report_abs(dev, ABS_RZ,       (GB(35,2) << 7) | GB(40,7));
			input_report_abs(dev, ABS_THROTTLE, (GB(32,3) << 7) | GB(48,7));

			input_report_abs(dev, ABS_HAT0X, sw_hat_to_axis[hat].x);
			input_report_abs(dev, ABS_HAT0Y, sw_hat_to_axis[hat].y);

			for (j = 0; j < 7; j++)
				input_report_key(dev, sw_btn[SW_ID_3DP][j], !GB(j+8,1));

			input_report_key(dev, BTN_BASE4, !GB(38,1));
			input_report_key(dev, BTN_BASE5, !GB(37,1));

			return 0;

		case SW_ID_GP:

			for (i = 0; i < sw->number; i ++) {

				if (sw_parity(GB(i*15,15))) return -1;

				input_report_abs(dev + i, ABS_X, GB(i*15+3,1) - GB(i*15+2,1));
				input_report_abs(dev + i, ABS_Y, GB(i*15+0,1) - GB(i*15+1,1));

				for (j = 0; j < 10; j++)
					input_report_key(dev, sw_btn[SW_ID_GP][j], !GB(i*15+j+4,1));
			}

			return 0;

		case SW_ID_PP:
		case SW_ID_FFP:

			if (!sw_parity(GB(0,48)) || (hat = GB(42,4)) > 8) return -1;

			input_report_abs(dev, ABS_X,        GB( 9,10));
			input_report_abs(dev, ABS_Y,        GB(19,10));
			input_report_abs(dev, ABS_RZ,       GB(36, 6));
			input_report_abs(dev, ABS_THROTTLE, GB(29, 7));

			input_report_abs(dev, ABS_HAT0X, sw_hat_to_axis[hat].x);
			input_report_abs(dev, ABS_HAT0Y, sw_hat_to_axis[hat].y);

			for (j = 0; j < 9; j++)
				input_report_key(dev, sw_btn[SW_ID_PP][j], !GB(j,1));

			return 0;

		case SW_ID_FSP:

			if (!sw_parity(GB(0,43)) || (hat = GB(28,4)) > 8) return -1;

			input_report_abs(dev, ABS_X,        GB( 0,10));
			input_report_abs(dev, ABS_Y,        GB(16,10));
			input_report_abs(dev, ABS_THROTTLE, GB(32, 6));

			input_report_abs(dev, ABS_HAT0X, sw_hat_to_axis[hat].x);
			input_report_abs(dev, ABS_HAT0Y, sw_hat_to_axis[hat].y);

			for (j = 0; j < 6; j++)
				input_report_key(dev, sw_btn[SW_ID_FSP][j], !GB(j+10,1));

			input_report_key(dev, BTN_TR,     GB(26,1));
			input_report_key(dev, BTN_START,  GB(27,1));
			input_report_key(dev, BTN_MODE,   GB(38,1));
			input_report_key(dev, BTN_SELECT, GB(39,1));

			return 0;

		case SW_ID_FFW:

			if (!sw_parity(GB(0,33))) return -1;

			input_report_abs(dev, ABS_RX,       GB( 0,10));
			input_report_abs(dev, ABS_RUDDER,   GB(10, 6));
			input_report_abs(dev, ABS_THROTTLE, GB(16, 6));

			for (j = 0; j < 8; j++)
				input_report_key(dev, sw_btn[SW_ID_FFW][j], !GB(j+22,1));

			return 0;
	}

	return -1;
}

/*
 * sw_read() reads SideWinder joystick data, and reinitializes
 * the joystick in case of persistent problems. This is the function that is
 * called from the generic code to poll the joystick.
 */

static int sw_read(struct sw *sw)
{
	unsigned char buf[SW_LENGTH];
	int i;

	i = sw_read_packet(sw->gameport, buf, sw->length, 0);

	if (sw->type == SW_ID_3DP && sw->length == 66 && i != 66) {		/* Broken packet, try to fix */

		if (i == 64 && !sw_check(sw_get_bits(buf,0,64,1))) {		/* Last init failed, 1 bit mode */
			printk(KERN_WARNING "sidewinder.c: Joystick in wrong mode on gameport%d"
				" - going to reinitialize.\n", sw->gameport->number);
			sw->fail = SW_FAIL;					/* Reinitialize */
			i = 128;						/* Bogus value */
		}

		if (i < 66 && GB(0,64) == GB(i*3-66,64))			/* 1 == 3 */
			i = 66;							/* Everything is fine */

		if (i < 66 && GB(0,64) == GB(66,64))				/* 1 == 2 */
			i = 66;							/* Everything is fine */

		if (i < 66 && GB(i*3-132,64) == GB(i*3-66,64)) {		/* 2 == 3 */
			memmove(buf, buf + i - 22, 22);				/* Move data */
			i = 66;							/* Carry on */
		}
	}

	if (i == sw->length && !sw_parse(buf, sw)) {				/* Parse data */

		sw->fail = 0;
		sw->ok++;

		if (sw->type == SW_ID_3DP && sw->length == 66			/* Many packets OK */
			&& sw->ok > SW_OK) {

			printk(KERN_INFO "sidewinder.c: No more trouble on gameport%d"
				" - enabling optimization again.\n", sw->gameport->number);
			sw->length = 22;
		}

		return 0;
	}

	sw->ok = 0;
	sw->fail++;

	if (sw->type == SW_ID_3DP && sw->length == 22 && sw->fail > SW_BAD) {	/* Consecutive bad packets */

		printk(KERN_INFO "sidewinder.c: Many bit errors on gameport%d"
			" - disabling optimization.\n", sw->gameport->number);
		sw->length = 66;
	}

	if (sw->fail < SW_FAIL) return -1;					/* Not enough, don't reinitialize yet */

	printk(KERN_WARNING "sidewinder.c: Too many bit errors on gameport%d"
		" - reinitializing joystick.\n", sw->gameport->number);

	if (!i && sw->type == SW_ID_3DP) {					/* 3D Pro can be in analog mode */
		udelay(3 * SW_TIMEOUT);
		sw_init_digital(sw->gameport);
	}

	udelay(SW_TIMEOUT);
	i = sw_read_packet(sw->gameport, buf, SW_LENGTH, 0);			/* Read normal data packet */
	udelay(SW_TIMEOUT);
	sw_read_packet(sw->gameport, buf, SW_LENGTH, i);			/* Read ID packet, this initializes the stick */

	sw->fail = SW_FAIL;
	
	return -1;
}

static void sw_timer(unsigned long private)
{
	struct sw *sw = (void *) private;
	
	sw->reads++;
	if (sw_read(sw)) sw->bads++;
	mod_timer(&sw->timer, jiffies + SW_REFRESH);
}

static int sw_open(struct input_dev *dev)
{
	struct sw *sw = dev->private;
	if (!sw->used++)
		mod_timer(&sw->timer, jiffies + SW_REFRESH);
	return 0;
}

static void sw_close(struct input_dev *dev)
{
	struct sw *sw = dev->private;
	if (!--sw->used)
		del_timer(&sw->timer);
}

/*
 * sw_print_packet() prints the contents of a SideWinder packet.
 */

static void sw_print_packet(char *name, int length, unsigned char *buf, char bits)
{
	int i;

	printk(KERN_INFO "sidewinder.c: %s packet, %d bits. [", name, length);
	for (i = (((length + 3) >> 2) - 1); i >= 0; i--)
		printk("%x", (int)sw_get_bits(buf, i << 2, 4, bits));
	printk("]\n");
}

/*
 * sw_3dp_id() translates the 3DP id into a human legible string.
 * Unfortunately I don't know how to do this for the other SW types.
 */

static void sw_3dp_id(unsigned char *buf, char *comment)
{
	int i;
	char pnp[8], rev[9];

	for (i = 0; i < 7; i++)						/* ASCII PnP ID */
		pnp[i] = sw_get_bits(buf, 24+8*i, 8, 1);

	for (i = 0; i < 8; i++)						/* ASCII firmware revision */
		rev[i] = sw_get_bits(buf, 88+8*i, 8, 1);

	pnp[7] = rev[8] = 0;

	sprintf(comment, " [PnP %d.%02d id %s rev %s]",
		(int) ((sw_get_bits(buf, 8, 6, 1) << 6) |		/* Two 6-bit values */
			sw_get_bits(buf, 16, 6, 1)) / 100,
		(int) ((sw_get_bits(buf, 8, 6, 1) << 6) |
			sw_get_bits(buf, 16, 6, 1)) % 100,
		 pnp, rev);
}

/*
 * sw_guess_mode() checks the upper two button bits for toggling -
 * indication of that the joystick is in 3-bit mode. This is documented
 * behavior for 3DP ID packet, and for example the FSP does this in
 * normal packets instead. Fun ...
 */

static int sw_guess_mode(unsigned char *buf, int len)
{
	int i;
	unsigned char xor = 0;
	for (i = 1; i < len; i++) xor |= (buf[i - 1] ^ buf[i]) & 6;
	return !!xor * 2 + 1;
}

/*
 * sw_connect() probes for SideWinder type joysticks.
 */

static void sw_connect(struct gameport *gameport, struct gameport_dev *dev)
{
	struct sw *sw;
	int i, j, k, l;
	unsigned char buf[SW_LENGTH];
	unsigned char idbuf[SW_LENGTH];
	unsigned char m = 1;
	char comment[40];

	comment[0] = 0;

	if (!(sw = kmalloc(sizeof(struct sw), GFP_KERNEL))) return;
	memset(sw, 0, sizeof(struct sw));

	gameport->private = sw;

	sw->gameport = gameport;
	init_timer(&sw->timer);
	sw->timer.data = (long) sw;
	sw->timer.function = sw_timer;

	if (gameport_open(gameport, dev, GAMEPORT_MODE_RAW))
		goto fail1;

	i = sw_read_packet(gameport, buf, SW_LENGTH, 0);		/* Read normal packet */
	m |= sw_guess_mode(buf, i);					/* Data packet (1-bit) can carry mode info [FSP] */
	udelay(SW_TIMEOUT);
	dbg("Init 1: Mode %d. Length %d.", m , i);

	if (!i) {							/* No data. 3d Pro analog mode? */
		sw_init_digital(gameport);				/* Switch to digital */
		udelay(SW_TIMEOUT);
		i = sw_read_packet(gameport, buf, SW_LENGTH, 0);	/* Retry reading packet */
		udelay(SW_TIMEOUT);
		dbg("Init 1b: Length %d.", i);
		if (!i) goto fail2;					/* No data -> FAIL */
	}

	j = sw_read_packet(gameport, idbuf, SW_LENGTH, i);		/* Read ID. This initializes the stick */
	m |= sw_guess_mode(idbuf, j);					/* ID packet should carry mode info [3DP] */
	dbg("Init 2: Mode %d. ID Length %d.", m , j);

	if (!j) {							/* Read ID failed. Happens in 1-bit mode on PP */
		udelay(SW_TIMEOUT);
		i = sw_read_packet(gameport, buf, SW_LENGTH, 0);	/* Retry reading packet */
		dbg("Init 2b: Mode %d. Length %d.", m, i);
		if (!i) goto fail2;
		udelay(SW_TIMEOUT);
		j = sw_read_packet(gameport, idbuf, SW_LENGTH, i);	/* Retry reading ID */
		dbg("Init 2c: ID Length %d.", j);
	}

	sw->type = -1;
	k = SW_FAIL;							/* Try SW_FAIL times */
	l = 0;

	do {
		k--;
		udelay(SW_TIMEOUT);
		i = sw_read_packet(gameport, buf, SW_LENGTH, 0);	/* Read data packet */
		dbg("Init 3: Mode %d. Length %d. Last %d. Tries %d.", m, i, l, k);

		if (i > l) {						/* Longer? As we can only lose bits, it makes */
									/* no sense to try detection for a packet shorter */
			l = i;						/* than the previous one */

			sw->number = 1;
			sw->gameport = gameport;
			sw->length = i;
			sw->bits = m;

			dbg("Init 3a: Case %d.\n", i * m);

			switch (i * m) {
				case 60:
					sw->number++;
				case 45:				/* Ambiguous packet length */
					if (j <= 40) {			/* ID length less or eq 40 -> FSP */	
				case 43:
						sw->type = SW_ID_FSP;
						break;
					}
					sw->number++;
				case 30:
					sw->number++;
				case 15:
					sw->type = SW_ID_GP;
					break;
				case 33:
				case 31:
					sw->type = SW_ID_FFW;
					break;
				case 48:				/* Ambiguous */
					if (j == 14) {			/* ID length 14*3 -> FFP */
						sw->type = SW_ID_FFP;
						sprintf(comment, " [AC %s]", sw_get_bits(idbuf,38,1,3) ? "off" : "on");
					} else
					sw->type = SW_ID_PP;
					break;
				case 198:
					sw->length = 22;
				case 64:
					sw->type = SW_ID_3DP;
					if (j == 160) sw_3dp_id(idbuf, comment);
					break;
			}
		}

	} while (k && (sw->type == -1));

	if (sw->type == -1) {
		printk(KERN_WARNING "sidewinder.c: unknown joystick device detected "
			"on gameport%d, contact <vojtech@suse.cz>\n", gameport->number);
		sw_print_packet("ID", j * 3, idbuf, 3);
		sw_print_packet("Data", i * m, buf, m);
		goto fail2;
	}

#ifdef SW_DEBUG
	sw_print_packet("ID", j * 3, idbuf, 3);
	sw_print_packet("Data", i * m, buf, m);
#endif

	k = i;
	l = j;

	for (i = 0; i < sw->number; i++) {
		int bits, code;

		sprintf(sw->name, "Microsoft SideWinder %s", sw_name[sw->type]);

		sw->dev[i].private = sw;

		sw->dev[i].open = sw_open;
		sw->dev[i].close = sw_close;

		sw->dev[i].name = sw->name;
		sw->dev[i].idbus = BUS_GAMEPORT;
		sw->dev[i].idvendor = GAMEPORT_ID_VENDOR_MICROSOFT;
		sw->dev[i].idproduct = sw->type;
		sw->dev[i].idversion = 0x0100;

		sw->dev[i].evbit[0] = BIT(EV_KEY) | BIT(EV_ABS);

		for (j = 0; (bits = sw_bit[sw->type][j]); j++) {
			code = sw_abs[sw->type][j];
			set_bit(code, sw->dev[i].absbit);
			sw->dev[i].absmax[code] = (1 << bits) - 1;
			sw->dev[i].absmin[code] = (bits == 1) ? -1 : 0;
			sw->dev[i].absfuzz[code] = ((bits >> 1) >= 2) ? (1 << ((bits >> 1) - 2)) : 0;
			if (code != ABS_THROTTLE)
				sw->dev[i].absflat[code] = (bits >= 5) ? (1 << (bits - 5)) : 0;
		}

		for (j = 0; (code = sw_btn[sw->type][j]); j++)
			set_bit(code, sw->dev[i].keybit);

		input_register_device(sw->dev + i);
		printk(KERN_INFO "input%d: %s%s on gameport%d.%d [%d-bit id %d data %d]\n",
			sw->dev[i].number, sw->name, comment, gameport->number, i, m, l, k);
	}

	return;
fail2:	gameport_close(gameport);
fail1:	kfree(sw);
}

static void sw_disconnect(struct gameport *gameport)
{
	int i;

	struct sw *sw = gameport->private;
	for (i = 0; i < sw->number; i++)
		input_unregister_device(sw->dev + i);
	gameport_close(gameport);
	kfree(sw);
}

static struct gameport_dev sw_dev = {
	connect:	sw_connect,
	disconnect:	sw_disconnect,
};

int __init sw_init(void)
{
	gameport_register_device(&sw_dev);
	return 0;
}

void __exit sw_exit(void)
{
	gameport_unregister_device(&sw_dev);
}

module_init(sw_init);
module_exit(sw_exit);
