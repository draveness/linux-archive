/*
 *  Copyright (C) 2004 by Jan-Benedict Glaw <jbglaw@lug-owl.de>
 */

/*
 * LK keyboard driver for Linux, based on sunkbd.c (C) by Vojtech Pavlik
 */

/*
 * DEC LK201 and LK401 keyboard driver for Linux (primary for DECstations
 * and VAXstations, but can also be used on any standard RS232 with an
 * adaptor).
 *
 * DISCLAIMER: This works for _me_. If you break anything by using the
 * information given below, I will _not_ be liable!
 *
 * RJ11 pinout:		To DB9:		Or DB25:
 * 	1 - RxD <---->	Pin 3 (TxD) <->	Pin 2 (TxD)
 * 	2 - GND <---->	Pin 5 (GND) <->	Pin 7 (GND)
 * 	4 - TxD <---->	Pin 2 (RxD) <->	Pin 3 (RxD)
 * 	3 - +12V (from HDD drive connector), DON'T connect to DB9 or DB25!!!
 *
 * Pin numbers for DB9 and DB25 are noted on the plug (quite small:). For
 * RJ11, it's like this:
 *
 *      __=__	Hold the plug in front of you, cable downwards,
 *     /___/|	nose is hidden behind the plug. Now, pin 1 is at
 *    |1234||	the left side, pin 4 at the right and 2 and 3 are
 *    |IIII||	in between, of course:)
 *    |    ||
 *    |____|/
 *      ||	So the adaptor consists of three connected cables
 *      ||	for data transmission (RxD and TxD) and signal ground.
 *		Additionally, you have to get +12V from somewhere.
 * Most easily, you'll get that from a floppy or HDD power connector.
 * It's the yellow cable there (black is ground and red is +5V).
 *
 * The keyboard and all the commands it understands are documented in
 * "VCB02 Video Subsystem - Technical Manual", EK-104AA-TM-001. This
 * document is LK201 specific, but LK401 is mostly compatible. It comes
 * up in LK201 mode and doesn't report any of the additional keys it
 * has. These need to be switched on with the LK_CMD_ENABLE_LK401
 * command. You'll find this document (scanned .pdf file) on MANX,
 * a search engine specific to DEC documentation. Try
 * http://www.vt100.net/manx/details?pn=EK-104AA-TM-001;id=21;cp=1
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
 * email or by paper mail:
 * Jan-Benedict Glaw, Lilienstra�e 16, 33790 H�rste (near Halle/Westf.),
 * Germany.
 */

#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/serio.h>
#include <linux/workqueue.h>

MODULE_AUTHOR ("Jan-Benedict Glaw <jbglaw@lug-owl.de>");
MODULE_DESCRIPTION ("LK keyboard driver");
MODULE_LICENSE ("GPL");

/*
 * Known parameters:
 *	bell_volume
 *	keyclick_volume
 *	ctrlclick_volume
 *
 * Please notice that there's not yet an API to set these at runtime.
 */
static int bell_volume = 100; /* % */
module_param (bell_volume, int, 0);
MODULE_PARM_DESC (bell_volume, "Bell volume (in %). default is 100%");

static int keyclick_volume = 100; /* % */
module_param (keyclick_volume, int, 0);
MODULE_PARM_DESC (keyclick_volume, "Keyclick volume (in %), default is 100%");

static int ctrlclick_volume = 100; /* % */
module_param (ctrlclick_volume, int, 0);
MODULE_PARM_DESC (ctrlclick_volume, "Ctrlclick volume (in %), default is 100%");

static int lk201_compose_is_alt = 0;
module_param (lk201_compose_is_alt, int, 0);
MODULE_PARM_DESC (lk201_compose_is_alt, "If set non-zero, LK201' Compose key "
		"will act as an Alt key");



#undef LKKBD_DEBUG
#ifdef LKKBD_DEBUG
#define DBG(x...) printk (x)
#else
#define DBG(x...) do {} while (0)
#endif

/* LED control */
#define LK_LED_WAIT		0x81
#define LK_LED_COMPOSE		0x82
#define LK_LED_SHIFTLOCK	0x84
#define LK_LED_SCROLLLOCK	0x88
#define LK_CMD_LED_ON		0x13
#define LK_CMD_LED_OFF		0x11

/* Mode control */
#define LK_MODE_DOWN		0x80
#define LK_MODE_AUTODOWN	0x82
#define LK_MODE_UPDOWN		0x86
#define LK_CMD_SET_MODE(mode,div)	((mode) | ((div) << 3))

/* Misc commands */
#define LK_CMD_ENABLE_KEYCLICK	0x1b
#define LK_CMD_DISABLE_KEYCLICK	0x99
#define LK_CMD_DISABLE_BELL	0xa1
#define LK_CMD_SOUND_BELL	0xa7
#define LK_CMD_ENABLE_BELL	0x23
#define LK_CMD_DISABLE_CTRCLICK	0xb9
#define LK_CMD_ENABLE_CTRCLICK	0xbb
#define LK_CMD_SET_DEFAULTS	0xd3
#define LK_CMD_POWERCYCLE_RESET	0xfd
#define LK_CMD_ENABLE_LK401	0xe9
#define LK_CMD_REQUEST_ID	0xab

/* Misc responses from keyboard */
#define LK_STUCK_KEY		0x3d
#define LK_SELFTEST_FAILED	0x3e
#define LK_ALL_KEYS_UP		0xb3
#define LK_METRONOME		0xb4
#define LK_OUTPUT_ERROR		0xb5
#define LK_INPUT_ERROR		0xb6
#define LK_KBD_LOCKED		0xb7
#define LK_KBD_TEST_MODE_ACK	0xb8
#define LK_PREFIX_KEY_DOWN	0xb9
#define LK_MODE_CHANGE_ACK	0xba
#define LK_RESPONSE_RESERVED	0xbb

#define LK_NUM_KEYCODES		256
#define LK_NUM_IGNORE_BYTES	6
typedef u_int16_t lk_keycode_t;



static lk_keycode_t lkkbd_keycode[LK_NUM_KEYCODES] = {
	[0x56] = KEY_F1,
	[0x57] = KEY_F2,
	[0x58] = KEY_F3,
	[0x59] = KEY_F4,
	[0x5a] = KEY_F5,
	[0x64] = KEY_F6,
	[0x65] = KEY_F7,
	[0x66] = KEY_F8,
	[0x67] = KEY_F9,
	[0x68] = KEY_F10,
	[0x71] = KEY_F11,
	[0x72] = KEY_F12,
	[0x73] = KEY_F13,
	[0x74] = KEY_F14,
	[0x7c] = KEY_F15,
	[0x7d] = KEY_F16,
	[0x80] = KEY_F17,
	[0x81] = KEY_F18,
	[0x82] = KEY_F19,
	[0x83] = KEY_F20,
	[0x8a] = KEY_FIND,
	[0x8b] = KEY_INSERT,
	[0x8c] = KEY_DELETE,
	[0x8d] = KEY_SELECT,
	[0x8e] = KEY_PAGEUP,
	[0x8f] = KEY_PAGEDOWN,
	[0x92] = KEY_KP0,
	[0x94] = KEY_KPDOT,
	[0x95] = KEY_KPENTER,
	[0x96] = KEY_KP1,
	[0x97] = KEY_KP2,
	[0x98] = KEY_KP3,
	[0x99] = KEY_KP4,
	[0x9a] = KEY_KP5,
	[0x9b] = KEY_KP6,
	[0x9c] = KEY_KPCOMMA,
	[0x9d] = KEY_KP7,
	[0x9e] = KEY_KP8,
	[0x9f] = KEY_KP9,
	[0xa0] = KEY_KPMINUS,
	[0xa1] = KEY_PROG1,
	[0xa2] = KEY_PROG2,
	[0xa3] = KEY_PROG3,
	[0xa4] = KEY_PROG4,
	[0xa7] = KEY_LEFT,
	[0xa8] = KEY_RIGHT,
	[0xa9] = KEY_DOWN,
	[0xaa] = KEY_UP,
	[0xab] = KEY_RIGHTSHIFT,
	[0xac] = KEY_LEFTALT,
	[0xad] = KEY_COMPOSE, /* Right Compose, that is. */
	[0xae] = KEY_LEFTSHIFT, /* Same as KEY_RIGHTSHIFT on LK201 */
	[0xaf] = KEY_LEFTCTRL,
	[0xb0] = KEY_CAPSLOCK,
	[0xb1] = KEY_COMPOSE, /* Left Compose, that is. */
	[0xb2] = KEY_RIGHTALT,
	[0xbc] = KEY_BACKSPACE,
	[0xbd] = KEY_ENTER,
	[0xbe] = KEY_TAB,
	[0xbf] = KEY_ESC,
	[0xc0] = KEY_1,
	[0xc1] = KEY_Q,
	[0xc2] = KEY_A,
	[0xc3] = KEY_Z,
	[0xc5] = KEY_2,
	[0xc6] = KEY_W,
	[0xc7] = KEY_S,
	[0xc8] = KEY_X,
	[0xc9] = KEY_102ND,
	[0xcb] = KEY_3,
	[0xcc] = KEY_E,
	[0xcd] = KEY_D,
	[0xce] = KEY_C,
	[0xd0] = KEY_4,
	[0xd1] = KEY_R,
	[0xd2] = KEY_F,
	[0xd3] = KEY_V,
	[0xd4] = KEY_SPACE,
	[0xd6] = KEY_5,
	[0xd7] = KEY_T,
	[0xd8] = KEY_G,
	[0xd9] = KEY_B,
	[0xdb] = KEY_6,
	[0xdc] = KEY_Y,
	[0xdd] = KEY_H,
	[0xde] = KEY_N,
	[0xe0] = KEY_7,
	[0xe1] = KEY_U,
	[0xe2] = KEY_J,
	[0xe3] = KEY_M,
	[0xe5] = KEY_8,
	[0xe6] = KEY_I,
	[0xe7] = KEY_K,
	[0xe8] = KEY_COMMA,
	[0xea] = KEY_9,
	[0xeb] = KEY_O,
	[0xec] = KEY_L,
	[0xed] = KEY_DOT,
	[0xef] = KEY_0,
	[0xf0] = KEY_P,
	[0xf2] = KEY_SEMICOLON,
	[0xf3] = KEY_SLASH,
	[0xf5] = KEY_EQUAL,
	[0xf6] = KEY_RIGHTBRACE,
	[0xf7] = KEY_BACKSLASH,
	[0xf9] = KEY_MINUS,
	[0xfa] = KEY_LEFTBRACE,
	[0xfb] = KEY_APOSTROPHE,
};

#define CHECK_LED(LED, BITS) do {		\
	if (test_bit (LED, lk->dev.led))	\
		leds_on |= BITS;		\
	else					\
		leds_off |= BITS;		\
	} while (0)

/*
 * Per-keyboard data
 */
struct lkkbd {
	lk_keycode_t keycode[LK_NUM_KEYCODES];
	int ignore_bytes;
	unsigned char id[LK_NUM_IGNORE_BYTES];
	struct input_dev dev;
	struct serio *serio;
	struct work_struct tq;
	char name[64];
	char phys[32];
	char type;
	int bell_volume;
	int keyclick_volume;
	int ctrlclick_volume;
};

/*
 * Calculate volume parameter byte for a given volume.
 */
static unsigned char
volume_to_hw (int volume_percent)
{
	unsigned char ret = 0;

	if (volume_percent < 0)
		volume_percent = 0;
	if (volume_percent > 100)
		volume_percent = 100;

	if (volume_percent >= 0)
		ret = 7;
	if (volume_percent >= 13)	/* 12.5 */
		ret = 6;
	if (volume_percent >= 25)
		ret = 5;
	if (volume_percent >= 38)	/* 37.5 */
		ret = 4;
	if (volume_percent >= 50)
		ret = 3;
	if (volume_percent >= 63)	/* 62.5 */
		ret = 2;		/* This is the default volume */
	if (volume_percent >= 75)
		ret = 1;
	if (volume_percent >= 88)	/* 87.5 */
		ret = 0;

	ret |= 0x80;

	return ret;
}

static void
lkkbd_detection_done (struct lkkbd *lk)
{
	int i;

	/*
	 * Reset setting for Compose key. Let Compose be KEY_COMPOSE.
	 */
	lk->keycode[0xb1] = KEY_COMPOSE;

	/*
	 * Print keyboard name and modify Compose=Alt on user's request.
	 */
	switch (lk->id[4]) {
		case 1:
			sprintf (lk->name, "DEC LK201 keyboard");

			if (lk201_compose_is_alt)
				lk->keycode[0xb1] = KEY_LEFTALT;
			break;

		case 2:
			sprintf (lk->name, "DEC LK401 keyboard");
			break;

		default:
			sprintf (lk->name, "Unknown DEC keyboard");
			printk (KERN_ERR "lkkbd: keyboard on %s is unknown, "
					"please report to Jan-Benedict Glaw "
					"<jbglaw@lug-owl.de>\n", lk->phys);
			printk (KERN_ERR "lkkbd: keyboard ID'ed as:");
			for (i = 0; i < LK_NUM_IGNORE_BYTES; i++)
				printk (" 0x%02x", lk->id[i]);
			printk ("\n");
			break;
	}
	printk (KERN_INFO "lkkbd: keyboard on %s identified as: %s\n",
			lk->phys, lk->name);

	/*
	 * Report errors during keyboard boot-up.
	 */
	switch (lk->id[2]) {
		case 0x00:
			/* All okay */
			break;

		case LK_STUCK_KEY:
			printk (KERN_ERR "lkkbd: Stuck key on keyboard at "
					"%s\n", lk->phys);
			break;

		case LK_SELFTEST_FAILED:
			printk (KERN_ERR "lkkbd: Selftest failed on keyboard "
					"at %s, keyboard may not work "
					"properly\n", lk->phys);
			break;

		default:
			printk (KERN_ERR "lkkbd: Unknown error %02x on "
					"keyboard at %s\n", lk->id[2],
					lk->phys);
			break;
	}

	/*
	 * Try to hint user if there's a stuck key.
	 */
	if (lk->id[2] == LK_STUCK_KEY && lk->id[3] != 0)
		printk (KERN_ERR "Scancode of stuck key is 0x%02x, keycode "
				"is 0x%04x\n", lk->id[3],
				lk->keycode[lk->id[3]]);

	return;
}

/*
 * lkkbd_interrupt() is called by the low level driver when a character
 * is received.
 */
static irqreturn_t
lkkbd_interrupt (struct serio *serio, unsigned char data, unsigned int flags,
		struct pt_regs *regs)
{
	struct lkkbd *lk = serio->private;
	int i;

	DBG (KERN_INFO "Got byte 0x%02x\n", data);

	if (lk->ignore_bytes > 0) {
		DBG (KERN_INFO "Ignoring a byte on %s\n",
				lk->name);
		lk->id[LK_NUM_IGNORE_BYTES - lk->ignore_bytes--] = data;

		if (lk->ignore_bytes == 0)
			lkkbd_detection_done (lk);

		return IRQ_HANDLED;
	}

	switch (data) {
		case LK_ALL_KEYS_UP:
			input_regs (&lk->dev, regs);
			for (i = 0; i < ARRAY_SIZE (lkkbd_keycode); i++)
				if (lk->keycode[i] != KEY_RESERVED)
					input_report_key (&lk->dev, lk->keycode[i], 0);
			input_sync (&lk->dev);
			break;
		case LK_METRONOME:
			DBG (KERN_INFO "Got LK_METRONOME and don't "
					"know how to handle...\n");
			break;
		case LK_OUTPUT_ERROR:
			DBG (KERN_INFO "Got LK_OUTPUT_ERROR and don't "
					"know how to handle...\n");
			break;
		case LK_INPUT_ERROR:
			DBG (KERN_INFO "Got LK_INPUT_ERROR and don't "
					"know how to handle...\n");
			break;
		case LK_KBD_LOCKED:
			DBG (KERN_INFO "Got LK_KBD_LOCKED and don't "
					"know how to handle...\n");
			break;
		case LK_KBD_TEST_MODE_ACK:
			DBG (KERN_INFO "Got LK_KBD_TEST_MODE_ACK and don't "
					"know how to handle...\n");
			break;
		case LK_PREFIX_KEY_DOWN:
			DBG (KERN_INFO "Got LK_PREFIX_KEY_DOWN and don't "
					"know how to handle...\n");
			break;
		case LK_MODE_CHANGE_ACK:
			DBG (KERN_INFO "Got LK_MODE_CHANGE_ACK and ignored "
					"it properly...\n");
			break;
		case LK_RESPONSE_RESERVED:
			DBG (KERN_INFO "Got LK_RESPONSE_RESERVED and don't "
					"know how to handle...\n");
			break;
		case 0x01:
			DBG (KERN_INFO "Got 0x01, scheduling re-initialization\n");
			lk->ignore_bytes = LK_NUM_IGNORE_BYTES;
			lk->id[LK_NUM_IGNORE_BYTES - lk->ignore_bytes--] = data;
			schedule_work (&lk->tq);
			break;

		default:
			if (lk->keycode[data] != KEY_RESERVED) {
				input_regs (&lk->dev, regs);
				if (!test_bit (lk->keycode[data], lk->dev.key))
					input_report_key (&lk->dev, lk->keycode[data], 1);
				else
					input_report_key (&lk->dev, lk->keycode[data], 0);
				input_sync (&lk->dev);
                        } else
                                printk (KERN_WARNING "%s: Unknown key with "
						"scancode 0x%02x on %s.\n",
						__FILE__, data, lk->name);
	}

	return IRQ_HANDLED;
}

/*
 * lkkbd_event() handles events from the input module.
 */
static int
lkkbd_event (struct input_dev *dev, unsigned int type, unsigned int code,
		int value)
{
	struct lkkbd *lk = dev->private;
	unsigned char leds_on = 0;
	unsigned char leds_off = 0;

	switch (type) {
		case EV_LED:
			CHECK_LED (LED_CAPSL, LK_LED_SHIFTLOCK);
			CHECK_LED (LED_COMPOSE, LK_LED_COMPOSE);
			CHECK_LED (LED_SCROLLL, LK_LED_SCROLLLOCK);
			CHECK_LED (LED_SLEEP, LK_LED_WAIT);
			if (leds_on != 0) {
				lk->serio->write (lk->serio, LK_CMD_LED_ON);
				lk->serio->write (lk->serio, leds_on);
			}
			if (leds_off != 0) {
				lk->serio->write (lk->serio, LK_CMD_LED_OFF);
				lk->serio->write (lk->serio, leds_off);
			}
			return 0;

		case EV_SND:
			switch (code) {
				case SND_CLICK:
					if (value == 0) {
						DBG ("%s: Deactivating key clicks\n", __FUNCTION__);
						lk->serio->write (lk->serio, LK_CMD_DISABLE_KEYCLICK);
						lk->serio->write (lk->serio, LK_CMD_DISABLE_CTRCLICK);
					} else {
						DBG ("%s: Activating key clicks\n", __FUNCTION__);
						lk->serio->write (lk->serio, LK_CMD_ENABLE_KEYCLICK);
						lk->serio->write (lk->serio, volume_to_hw (lk->keyclick_volume));
						lk->serio->write (lk->serio, LK_CMD_ENABLE_CTRCLICK);
						lk->serio->write (lk->serio, volume_to_hw (lk->ctrlclick_volume));
					}
					return 0;

				case SND_BELL:
					if (value != 0)
						lk->serio->write (lk->serio, LK_CMD_SOUND_BELL);

					return 0;
			}
			break;

		default:
			printk (KERN_ERR "%s (): Got unknown type %d, code %d, value %d\n",
					__FUNCTION__, type, code, value);
	}

	return -1;
}

/*
 * lkkbd_reinit() sets leds and beeps to a state the computer remembers they
 * were in.
 */
static void
lkkbd_reinit (void *data)
{
	struct lkkbd *lk = data;
	int division;
	unsigned char leds_on = 0;
	unsigned char leds_off = 0;

	/* Ask for ID */
	lk->serio->write (lk->serio, LK_CMD_REQUEST_ID);

	/* Reset parameters */
	lk->serio->write (lk->serio, LK_CMD_SET_DEFAULTS);

	/* Set LEDs */
	CHECK_LED (LED_CAPSL, LK_LED_SHIFTLOCK);
	CHECK_LED (LED_COMPOSE, LK_LED_COMPOSE);
	CHECK_LED (LED_SCROLLL, LK_LED_SCROLLLOCK);
	CHECK_LED (LED_SLEEP, LK_LED_WAIT);
	if (leds_on != 0) {
		lk->serio->write (lk->serio, LK_CMD_LED_ON);
		lk->serio->write (lk->serio, leds_on);
	}
	if (leds_off != 0) {
		lk->serio->write (lk->serio, LK_CMD_LED_OFF);
		lk->serio->write (lk->serio, leds_off);
	}

	/*
	 * Try to activate extended LK401 mode. This command will
	 * only work with a LK401 keyboard and grants access to
	 * LAlt, RAlt, RCompose and RShift.
	 */
	lk->serio->write (lk->serio, LK_CMD_ENABLE_LK401);

	/* Set all keys to UPDOWN mode */
	for (division = 1; division <= 14; division++)
		lk->serio->write (lk->serio, LK_CMD_SET_MODE (LK_MODE_UPDOWN,
					division));

	/* Enable bell and set volume */
	lk->serio->write (lk->serio, LK_CMD_ENABLE_BELL);
	lk->serio->write (lk->serio, volume_to_hw (lk->bell_volume));

	/* Enable/disable keyclick (and possibly set volume) */
	if (test_bit (SND_CLICK, lk->dev.snd)) {
		lk->serio->write (lk->serio, LK_CMD_ENABLE_KEYCLICK);
		lk->serio->write (lk->serio, volume_to_hw (lk->keyclick_volume));
		lk->serio->write (lk->serio, LK_CMD_ENABLE_CTRCLICK);
		lk->serio->write (lk->serio, volume_to_hw (lk->ctrlclick_volume));
	} else {
		lk->serio->write (lk->serio, LK_CMD_DISABLE_KEYCLICK);
		lk->serio->write (lk->serio, LK_CMD_DISABLE_CTRCLICK);
	}

	/* Sound the bell if needed */
	if (test_bit (SND_BELL, lk->dev.snd))
		lk->serio->write (lk->serio, LK_CMD_SOUND_BELL);
}

/*
 * lkkbd_connect() probes for a LK keyboard and fills the necessary structures.
 */
static void
lkkbd_connect (struct serio *serio, struct serio_dev *dev)
{
	struct lkkbd *lk;
	int i;

	if ((serio->type & SERIO_TYPE) != SERIO_RS232)
		return;
	if ((serio->type & SERIO_PROTO) != SERIO_LKKBD)
		return;

	if (!(lk = kmalloc (sizeof (struct lkkbd), GFP_KERNEL)))
		return;
	memset (lk, 0, sizeof (struct lkkbd));

	init_input_dev (&lk->dev);
	set_bit (EV_KEY, lk->dev.evbit);
	set_bit (EV_LED, lk->dev.evbit);
	set_bit (EV_SND, lk->dev.evbit);
	set_bit (EV_REP, lk->dev.evbit);
	set_bit (LED_CAPSL, lk->dev.ledbit);
	set_bit (LED_SLEEP, lk->dev.ledbit);
	set_bit (LED_COMPOSE, lk->dev.ledbit);
	set_bit (LED_SCROLLL, lk->dev.ledbit);
	set_bit (SND_BELL, lk->dev.sndbit);
	set_bit (SND_CLICK, lk->dev.sndbit);

	lk->serio = serio;

	INIT_WORK (&lk->tq, lkkbd_reinit, lk);

	lk->bell_volume = bell_volume;
	lk->keyclick_volume = keyclick_volume;
	lk->ctrlclick_volume = ctrlclick_volume;

	lk->dev.keycode = lk->keycode;
	lk->dev.keycodesize = sizeof (lk_keycode_t);
	lk->dev.keycodemax = LK_NUM_KEYCODES;

	lk->dev.event = lkkbd_event;
	lk->dev.private = lk;

	serio->private = lk;

	if (serio_open (serio, dev)) {
		kfree (lk);
		return;
	}

	sprintf (lk->name, "DEC LK keyboard");
	sprintf (lk->phys, "%s/input0", serio->phys);

	memcpy (lk->keycode, lkkbd_keycode, sizeof (lk_keycode_t) * LK_NUM_KEYCODES);
	for (i = 0; i < LK_NUM_KEYCODES; i++)
		set_bit (lk->keycode[i], lk->dev.keybit);

	lk->dev.name = lk->name;
	lk->dev.phys = lk->phys;
	lk->dev.id.bustype = BUS_RS232;
	lk->dev.id.vendor = SERIO_LKKBD;
	lk->dev.id.product = 0;
	lk->dev.id.version = 0x0100;

	input_register_device (&lk->dev);

	printk (KERN_INFO "input: %s on %s, initiating reset\n", lk->name, serio->phys);
	lk->serio->write (lk->serio, LK_CMD_POWERCYCLE_RESET);
}

/*
 * lkkbd_disconnect() unregisters and closes behind us.
 */
static void
lkkbd_disconnect (struct serio *serio)
{
	struct lkkbd *lk = serio->private;

	input_unregister_device (&lk->dev);
	serio_close (serio);
	kfree (lk);
}

static struct serio_dev lkkbd_dev = {
	.connect = lkkbd_connect,
	.disconnect = lkkbd_disconnect,
	.interrupt = lkkbd_interrupt,
};

/*
 * The functions for insering/removing us as a module.
 */
int __init
lkkbd_init (void)
{
	serio_register_device (&lkkbd_dev);
	return 0;
}

void __exit
lkkbd_exit (void)
{
	serio_unregister_device (&lkkbd_dev);
}

module_init (lkkbd_init);
module_exit (lkkbd_exit);

