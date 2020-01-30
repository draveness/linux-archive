/*
 * drivers/input/adbhid.c
 *
 * ADB HID driver for Power Macintosh computers.
 *
 * Adapted from drivers/macintosh/mac_keyb.c by Franz Sirl
 * (see that file for its authors and contributors).
 *
 * Copyright (C) 2000 Franz Sirl.
 *
 * Adapted to ADB changes and support for more devices by
 * Benjamin Herrenschmidt. Adapted from code in MkLinux
 * and reworked.
 * 
 * Supported devices:
 *
 * - Standard 1 button mouse
 * - All standard Apple Extended protocol (handler ID 4)
 * - mouseman and trackman mice & trackballs 
 * - PowerBook Trackpad (default setup: enable tapping)
 * - MicroSpeed mouse & trackball (needs testing)
 * - CH Products Trackball Pro (needs testing)
 * - Contour Design (Contour Mouse)
 * - Hunter digital (NoHandsMouse)
 * - Kensignton TurboMouse 5 (needs testing)
 * - Mouse Systems A3 mice and trackballs <aidan@kublai.com>
 * - MacAlly 2-buttons mouse (needs testing) <pochini@denise.shiny.it>
 *
 * To do:
 *
 * Improve Kensington support.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/malloc.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/input.h>
#include <linux/kbd_ll.h>

#include <linux/adb.h>
#include <linux/cuda.h>
#include <linux/pmu.h>
#ifdef CONFIG_PMAC_BACKLIGHT
#include <asm/backlight.h>
#endif

MODULE_AUTHOR("Franz Sirl <Franz.Sirl-kernel@lauterbach.com>");

#define KEYB_KEYREG	0	/* register # for key up/down data */
#define KEYB_LEDREG	2	/* register # for leds on ADB keyboard */
#define MOUSE_DATAREG	0	/* reg# for movement/button codes from mouse */

static int adb_message_handler(struct notifier_block *, unsigned long, void *);
static struct notifier_block adbhid_adb_notifier = {
	notifier_call:	adb_message_handler,
};

unsigned char adb_to_linux_keycodes[128] = {
	 30, 31, 32, 33, 35, 34, 44, 45, 46, 47, 86, 48, 16, 17, 18, 19,
	 21, 20,  2,  3,  4,  5,  7,  6, 13, 10,  8, 12,  9, 11, 27, 24,
	 22, 26, 23, 25, 28, 38, 36, 40, 37, 39, 43, 51, 53, 49, 50, 52,
	 15, 57, 41, 14, 96,  1, 29,125, 42, 58, 56,105,106,108,103,  0,
	  0, 83,  0, 55,  0, 78,  0, 69,  0,  0,  0, 98, 96,  0, 74,  0,
	  0,117, 82, 79, 80, 81, 75, 76, 77, 71,  0, 72, 73,183,181,124,
	 63, 64, 65, 61, 66, 67,191, 87,190, 99,  0, 70,  0, 68,101, 88,
	  0,119,110,102,104,111, 62,107, 60,109, 59, 54,100, 97,116,116
};

struct adbhid {
	struct input_dev input;
	int id;
	int default_id;
	int original_handler_id;
	int current_handler_id;
	int mouse_kind;
	unsigned char *keycode;
	char name[64];
};

static struct adbhid *adbhid[16] = { 0 };

static void adbhid_probe(void);

static void adbhid_input_keycode(int, int, int);
static void leds_done(struct adb_request *);

static void init_trackpad(int id);
static void init_trackball(int id);
static void init_turbomouse(int id);
static void init_microspeed(int id);
static void init_ms_a3(int id);

static struct adb_ids keyboard_ids;
static struct adb_ids mouse_ids;
static struct adb_ids buttons_ids;

/* Kind of keyboard, see Apple technote 1152  */
#define ADB_KEYBOARD_UNKNOWN	0
#define ADB_KEYBOARD_ANSI	0x0100
#define ADB_KEYBOARD_ISO	0x0200
#define ADB_KEYBOARD_JIS	0x0300

/* Kind of mouse  */
#define ADBMOUSE_STANDARD_100	0	/* Standard 100cpi mouse (handler 1) */
#define ADBMOUSE_STANDARD_200	1	/* Standard 200cpi mouse (handler 2) */
#define ADBMOUSE_EXTENDED	2	/* Apple Extended mouse (handler 4) */
#define ADBMOUSE_TRACKBALL	3	/* TrackBall (handler 4) */
#define ADBMOUSE_TRACKPAD       4	/* Apple's PowerBook trackpad (handler 4) */
#define ADBMOUSE_TURBOMOUSE5    5	/* Turbomouse 5 (previously req. mousehack) */
#define ADBMOUSE_MICROSPEED	6	/* Microspeed mouse (&trackball ?), MacPoint */
#define ADBMOUSE_TRACKBALLPRO	7	/* Trackball Pro (special buttons) */
#define ADBMOUSE_MS_A3		8	/* Mouse systems A3 trackball (handler 3) */
#define ADBMOUSE_MACALLY2	9	/* MacAlly 2-button mouse */

static void
adbhid_keyboard_input(unsigned char *data, int nb, struct pt_regs *regs, int apoll)
{
	int id = (data[0] >> 4) & 0x0f;

	if (!adbhid[id]) {
		printk(KERN_ERR "ADB HID on ID %d not yet registered, packet %#02x, %#02x, %#02x, %#02x\n",
		       id, data[0], data[1], data[2], data[3]);
		return;
	}

	/* first check this is from register 0 */
	if (nb != 3 || (data[0] & 3) != KEYB_KEYREG)
		return;		/* ignore it */
	kbd_pt_regs = regs;
	adbhid_input_keycode(id, data[1], 0);
	if (!(data[2] == 0xff || (data[2] == 0x7f && data[1] == 0x7f)))
		adbhid_input_keycode(id, data[2], 0);
}

static void
adbhid_input_keycode(int id, int keycode, int repeat)
{
	int up_flag;

	up_flag = (keycode & 0x80);
	keycode &= 0x7f;

	switch (keycode) {
	case 0x39: /* Generate down/up events for CapsLock everytime. */
		input_report_key(&adbhid[id]->input, KEY_CAPSLOCK, 1);
		input_report_key(&adbhid[id]->input, KEY_CAPSLOCK, 0);
		return;
	case 0x3f: /* ignore Powerbook Fn key */
		return;
	}

	if (adbhid[id]->keycode[keycode])
		input_report_key(&adbhid[id]->input,
				 adbhid[id]->keycode[keycode], !up_flag);
	else
		printk(KERN_INFO "Unhandled ADB key (scancode %#02x) %s.\n", keycode,
		       up_flag ? "released" : "pressed");
}

static void
adbhid_mouse_input(unsigned char *data, int nb, struct pt_regs *regs, int autopoll)
{
	int id = (data[0] >> 4) & 0x0f;

	if (!adbhid[id]) {
		printk(KERN_ERR "ADB HID on ID %d not yet registered\n", id);
		return;
	}

  /*
    Handler 1 -- 100cpi original Apple mouse protocol.
    Handler 2 -- 200cpi original Apple mouse protocol.

    For Apple's standard one-button mouse protocol the data array will
    contain the following values:

                BITS    COMMENTS
    data[0] = dddd 1100 ADB command: Talk, register 0, for device dddd.
    data[1] = bxxx xxxx First button and x-axis motion.
    data[2] = byyy yyyy Second button and y-axis motion.

    Handler 4 -- Apple Extended mouse protocol.

    For Apple's 3-button mouse protocol the data array will contain the
    following values:

		BITS    COMMENTS
    data[0] = dddd 1100 ADB command: Talk, register 0, for device dddd.
    data[1] = bxxx xxxx Left button and x-axis motion.
    data[2] = byyy yyyy Second button and y-axis motion.
    data[3] = byyy bxxx Third button and fourth button.  Y is additional
	      high bits of y-axis motion.  XY is additional
	      high bits of x-axis motion.

    MacAlly 2-button mouse protocol.

    For MacAlly 2-button mouse protocol the data array will contain the
    following values:

		BITS    COMMENTS
    data[0] = dddd 1100 ADB command: Talk, register 0, for device dddd.
    data[1] = bxxx xxxx Left button and x-axis motion.
    data[2] = byyy yyyy Right button and y-axis motion.
    data[3] = ???? ???? unknown
    data[4] = ???? ???? unknown

  */

	/* If it's a trackpad, we alias the second button to the first.
	   NOTE: Apple sends an ADB flush command to the trackpad when
	         the first (the real) button is released. We could do
		 this here using async flush requests.
	*/
	switch (adbhid[id]->mouse_kind)
	{
	    case ADBMOUSE_TRACKPAD:
		data[1] = (data[1] & 0x7f) | ((data[1] & data[2]) & 0x80);
		data[2] = data[2] | 0x80;
		break;
	    case ADBMOUSE_MICROSPEED:
		data[1] = (data[1] & 0x7f) | ((data[3] & 0x01) << 7);
		data[2] = (data[2] & 0x7f) | ((data[3] & 0x02) << 6);
		data[3] = (data[3] & 0x77) | ((data[3] & 0x04) << 5)
			| (data[3] & 0x08);
		break;
	    case ADBMOUSE_TRACKBALLPRO:
		data[1] = (data[1] & 0x7f) | (((data[3] & 0x04) << 5)
			& ((data[3] & 0x08) << 4));
		data[2] = (data[2] & 0x7f) | ((data[3] & 0x01) << 7);
		data[3] = (data[3] & 0x77) | ((data[3] & 0x02) << 6);
		break;
	    case ADBMOUSE_MS_A3:
		data[1] = (data[1] & 0x7f) | ((data[3] & 0x01) << 7);
		data[2] = (data[2] & 0x7f) | ((data[3] & 0x02) << 6);
		data[3] = ((data[3] & 0x04) << 5);
		break;
            case ADBMOUSE_MACALLY2:
		data[3] = (data[2] & 0x80) ? 0x80 : 0x00;
		data[2] |= 0x80;  /* Right button is mapped as button 3 */
		nb=4;
                break;
	}

	input_report_key(&adbhid[id]->input, BTN_LEFT,   !((data[1] >> 7) & 1));
	input_report_key(&adbhid[id]->input, BTN_MIDDLE, !((data[2] >> 7) & 1));

	if (nb >= 4)
		input_report_key(&adbhid[id]->input, BTN_RIGHT,  !((data[3] >> 7) & 1));

	input_report_rel(&adbhid[id]->input, REL_X,
			 ((data[2]&0x7f) < 64 ? (data[2]&0x7f) : (data[2]&0x7f)-128 ));
	input_report_rel(&adbhid[id]->input, REL_Y,
			 ((data[1]&0x7f) < 64 ? (data[1]&0x7f) : (data[1]&0x7f)-128 ));
}

static void
adbhid_buttons_input(unsigned char *data, int nb, struct pt_regs *regs, int autopoll)
{
	int id = (data[0] >> 4) & 0x0f;

	if (!adbhid[id]) {
		printk(KERN_ERR "ADB HID on ID %d not yet registered\n", id);
		return;
	}

	switch (adbhid[id]->original_handler_id) {
	default:
	case 0x02: /* Adjustable keyboard button device */
		printk(KERN_INFO "Unhandled ADB_MISC event %02x, %02x, %02x, %02x\n",
		       data[0], data[1], data[2], data[3]);
		break;
	case 0x1f: /* Powerbook button device */
	  {
#ifdef CONFIG_PMAC_BACKLIGHT
		int backlight = get_backlight_level();

		/*
		 * XXX: Where is the contrast control for the passive?
		 *  -- Cort
		 */

		switch (data[1]) {
		case 0x8:	/* mute */
			break;

		case 0x7:	/* contrast decrease */
			break;

		case 0x6:	/* contrast increase */
			break;

		case 0xa:	/* brightness decrease */
			if (backlight < 0)
				break;
			if (backlight > BACKLIGHT_OFF)
				set_backlight_level(backlight-1);
			else
				set_backlight_level(BACKLIGHT_OFF);
			break;

		case 0x9:	/* brightness increase */
			if (backlight < 0)
				break;
			if (backlight < BACKLIGHT_MAX)
				set_backlight_level(backlight+1);
			else 
				set_backlight_level(BACKLIGHT_MAX);
			break;
		}
#endif /* CONFIG_PMAC_BACKLIGHT */
	  }
	  break;
	}
}

static struct adb_request led_request;
static int leds_pending[16];
static int pending_devs[16];
static int pending_led_start=0;
static int pending_led_end=0;

static void real_leds(unsigned char leds, int device)
{
    if (led_request.complete) {
	adb_request(&led_request, leds_done, 0, 3,
		    ADB_WRITEREG(device, KEYB_LEDREG), 0xff,
		    ~leds);
    } else {
	if (!(leds_pending[device] & 0x100)) {
	    pending_devs[pending_led_end] = device;
	    pending_led_end++;
	    pending_led_end = (pending_led_end < 16) ? pending_led_end : 0;
	}
	leds_pending[device] = leds | 0x100;
    }
}

/*
 * Event callback from the input module. Events that change the state of
 * the hardware are processed here.
 */
static int adbhid_kbd_event(struct input_dev *dev, unsigned int type, unsigned int code, int value)
{
	struct adbhid *adbhid = dev->private;
	unsigned char leds;

	switch (type) {
	case EV_LED:
	  leds = (test_bit(LED_SCROLLL, dev->led) ? 4 : 0)
	       | (test_bit(LED_NUML,    dev->led) ? 1 : 0)
	       | (test_bit(LED_CAPSL,   dev->led) ? 2 : 0);
	  real_leds(leds, adbhid->id);
	  return 0;
	}

	return -1;
}

static void leds_done(struct adb_request *req)
{
    int leds,device;

    if (pending_led_start != pending_led_end) {
	device = pending_devs[pending_led_start];
	leds = leds_pending[device] & 0xff;
	leds_pending[device] = 0;
	pending_led_start++;
	pending_led_start = (pending_led_start < 16) ? pending_led_start : 0;
	real_leds(leds,device);
    }

}

static int
adb_message_handler(struct notifier_block *this, unsigned long code, void *x)
{
	unsigned long flags;

	switch (code) {
	case ADB_MSG_PRE_RESET:
	case ADB_MSG_POWERDOWN:
	    	/* Stop the repeat timer. Autopoll is already off at this point */
		save_flags(flags);
		cli();
		{
			int i;
			for (i = 1; i < 16; i++) {
				if (adbhid[i])
					del_timer(&adbhid[i]->input.timer);
			}
		}
		restore_flags(flags);

		/* Stop pending led requests */
		while(!led_request.complete)
			adb_poll();
		break;

	case ADB_MSG_POST_RESET:
		adbhid_probe();
		break;
	}
	return NOTIFY_DONE;
}

static void
adbhid_input_register(int id, int default_id, int original_handler_id,
		      int current_handler_id, int mouse_kind)
{
	int i;

	if (adbhid[id]) {
		printk(KERN_ERR "Trying to reregister ADB HID on ID %d\n", id);
		return;
	}

	if (!(adbhid[id] = kmalloc(sizeof(struct adbhid), GFP_KERNEL)))
		return;

	memset(adbhid[id], 0, sizeof(struct adbhid));

	adbhid[id]->id = default_id;
	adbhid[id]->original_handler_id = original_handler_id;
	adbhid[id]->current_handler_id = current_handler_id;
	adbhid[id]->mouse_kind = mouse_kind;
	adbhid[id]->input.private = adbhid[id];
	adbhid[id]->input.name = adbhid[id]->name;
	adbhid[id]->input.idbus = BUS_ADB;
	adbhid[id]->input.idvendor = 0x0001;
	adbhid[id]->input.idproduct = (id << 12) | (default_id << 8) | original_handler_id;
	adbhid[id]->input.idversion = 0x0100;

	switch (default_id) {
	case ADB_KEYBOARD:
		if (!(adbhid[id]->keycode = kmalloc(sizeof(adb_to_linux_keycodes), GFP_KERNEL))) {
			kfree(adbhid[id]);
			return;
		}

		sprintf(adbhid[id]->name, "ADB keyboard on ID %d:%d.%02x",
			id, default_id, original_handler_id);

		memcpy(adbhid[id]->keycode, adb_to_linux_keycodes, sizeof(adb_to_linux_keycodes));

		printk(KERN_INFO "Detected ADB keyboard, type ");
		switch (original_handler_id) {
		default:
			printk("<unknown>.\n");
			adbhid[id]->input.idversion = ADB_KEYBOARD_UNKNOWN;
			break;

		case 0x01: case 0x02: case 0x03: case 0x06: case 0x08:
		case 0x0C: case 0x10: case 0x18: case 0x1B: case 0x1C:
		case 0xC0: case 0xC3: case 0xC6:
			printk("ANSI.\n");
			adbhid[id]->input.idversion = ADB_KEYBOARD_ANSI;
			break;

		case 0x04: case 0x05: case 0x07: case 0x09: case 0x0D:
		case 0x11: case 0x14: case 0x19: case 0x1D: case 0xC1:
		case 0xC4: case 0xC7:
			printk("ISO, swapping keys.\n");
			adbhid[id]->input.idversion = ADB_KEYBOARD_ISO;
			i = adbhid[id]->keycode[10];
			adbhid[id]->keycode[10] = adbhid[id]->keycode[50];
			adbhid[id]->keycode[50] = i;
			break;

		case 0x12: case 0x15: case 0x16: case 0x17: case 0x1A:
		case 0x1E: case 0xC2: case 0xC5: case 0xC8: case 0xC9:
			printk("JIS.\n");
			adbhid[id]->input.idversion = ADB_KEYBOARD_JIS;
			break;
		}

		for (i = 0; i < 128; i++)
			if (adbhid[id]->keycode[i])
				set_bit(adbhid[id]->keycode[i], adbhid[id]->input.keybit);

		adbhid[id]->input.evbit[0] = BIT(EV_KEY) | BIT(EV_LED) | BIT(EV_REP);
		adbhid[id]->input.ledbit[0] = BIT(LED_SCROLLL) | BIT(LED_CAPSL) | BIT(LED_NUML);
		adbhid[id]->input.event = adbhid_kbd_event;
		adbhid[id]->input.keycodemax = 127;
		adbhid[id]->input.keycodesize = 1;
		break;

	case ADB_MOUSE:
		sprintf(adbhid[id]->name, "ADB mouse on ID %d:%d.%02x",
			id, default_id, original_handler_id);

		adbhid[id]->input.evbit[0] = BIT(EV_KEY) | BIT(EV_REL);
		adbhid[id]->input.keybit[LONG(BTN_MOUSE)] = BIT(BTN_LEFT) | BIT(BTN_MIDDLE) | BIT(BTN_RIGHT);
		adbhid[id]->input.relbit[0] = BIT(REL_X) | BIT(REL_Y);
		break;

	case ADB_MISC:
		switch (original_handler_id) {
		case 0x02: /* Adjustable keyboard button device */
			sprintf(adbhid[id]->name, "ADB adjustable keyboard buttons on ID %d:%d.%02x",
				id, default_id, original_handler_id);
			break;
		case 0x1f: /* Powerbook button device */
			sprintf(adbhid[id]->name, "ADB Powerbook buttons on ID %d:%d.%02x",
				id, default_id, original_handler_id);
			break;
		}
		if (adbhid[id]->name[0])
			break;
		/* else fall through */

	default:
		printk(KERN_INFO "Trying to register unknown ADB device to input layer.\n");
		kfree(adbhid[id]);
		return;
	}

	adbhid[id]->input.keycode = adbhid[id]->keycode;

	input_register_device(&adbhid[id]->input);

	printk(KERN_INFO "input%d: ADB HID on ID %d:%d.%02x\n",
	       adbhid[id]->input.number, id, default_id, original_handler_id);

	if (default_id == ADB_KEYBOARD) {
		/* HACK WARNING!! This should go away as soon there is an utility
		 * to control that for event devices.
		 */
		adbhid[id]->input.rep[REP_DELAY] = HZ/2;   /* input layer default: HZ/4 */
		adbhid[id]->input.rep[REP_PERIOD] = HZ/15; /* input layer default: HZ/33 */
	}
}

static void adbhid_input_unregister(int id)
{
	input_unregister_device(&adbhid[id]->input);
	if (adbhid[id]->keycode)
		kfree(adbhid[id]->keycode);
	kfree(adbhid[id]);
	adbhid[id] = 0;
}


static void
adbhid_probe(void)
{
	struct adb_request req;
	int i, default_id, org_handler_id, cur_handler_id;

	for (i = 1; i < 16; i++) {
		if (adbhid[i])
			adbhid_input_unregister(i);
	}

	adb_register(ADB_MOUSE, 0, &mouse_ids, adbhid_mouse_input);
	adb_register(ADB_KEYBOARD, 0, &keyboard_ids, adbhid_keyboard_input);
	adb_register(ADB_MISC, 0, &buttons_ids, adbhid_buttons_input);

	for (i = 0; i < keyboard_ids.nids; i++) {
		int id = keyboard_ids.id[i];

		adb_get_infos(id, &default_id, &org_handler_id);

		/* turn off all leds */
		adb_request(&req, NULL, ADBREQ_SYNC, 3,
			    ADB_WRITEREG(id, KEYB_LEDREG), 0xff, 0xff);

		/* Enable full feature set of the keyboard
		   ->get it to send separate codes for left and right shift,
		   control, option keys */
#if 0		/* handler 5 doesn't send separate codes for R modifiers */
		if (adb_try_handler_change(id, 5))
			printk("ADB keyboard at %d, handler set to 5\n", id);
		else
#endif
		if (adb_try_handler_change(id, 3))
			printk("ADB keyboard at %d, handler set to 3\n", id);
		else
			printk("ADB keyboard at %d, handler 1\n", id);

		adb_get_infos(id, &default_id, &cur_handler_id);
		adbhid_input_register(id, default_id, org_handler_id, cur_handler_id, 0);
	}

	for (i = 0; i < buttons_ids.nids; i++) {
		int id = buttons_ids.id[i];

		adb_get_infos(id, &default_id, &org_handler_id);
		adbhid_input_register(id, default_id, org_handler_id, org_handler_id, 0);
	}

	/* Try to switch all mice to handler 4, or 2 for three-button
	   mode and full resolution. */
	for (i = 0; i < mouse_ids.nids; i++) {
		int id = mouse_ids.id[i];
		int mouse_kind;

		adb_get_infos(id, &default_id, &org_handler_id);

		if (adb_try_handler_change(id, 4)) {
			printk("ADB mouse at %d, handler set to 4", id);
			mouse_kind = ADBMOUSE_EXTENDED;
		}
		else if (adb_try_handler_change(id, 0x2F)) {
			printk("ADB mouse at %d, handler set to 0x2F", id);
			mouse_kind = ADBMOUSE_MICROSPEED;
		}
		else if (adb_try_handler_change(id, 0x42)) {
			printk("ADB mouse at %d, handler set to 0x42", id);
			mouse_kind = ADBMOUSE_TRACKBALLPRO;
		}
		else if (adb_try_handler_change(id, 0x66)) {
			printk("ADB mouse at %d, handler set to 0x66", id);
			mouse_kind = ADBMOUSE_MICROSPEED;
		}
		else if (adb_try_handler_change(id, 0x5F)) {
			printk("ADB mouse at %d, handler set to 0x5F", id);
			mouse_kind = ADBMOUSE_MICROSPEED;
		}
		else if (adb_try_handler_change(id, 3)) {
			printk("ADB mouse at %d, handler set to 3", id);
			mouse_kind = ADBMOUSE_MS_A3;
		}
		else if (adb_try_handler_change(id, 2)) {
			printk("ADB mouse at %d, handler set to 2", id);
			mouse_kind = ADBMOUSE_STANDARD_200;
		}
		else {
			printk("ADB mouse at %d, handler 1", id);
			mouse_kind = ADBMOUSE_STANDARD_100;
		}

		if ((mouse_kind == ADBMOUSE_TRACKBALLPRO)
		    || (mouse_kind == ADBMOUSE_MICROSPEED)) {
			init_microspeed(id);
		} else if (mouse_kind == ADBMOUSE_MS_A3) {
			init_ms_a3(id);
		} else if (mouse_kind ==  ADBMOUSE_EXTENDED) {
			/*
			 * Register 1 is usually used for device
			 * identification.  Here, we try to identify
			 * a known device and call the appropriate
			 * init function.
			 */
			adb_request(&req, NULL, ADBREQ_SYNC | ADBREQ_REPLY, 1,
				    ADB_READREG(id, 1));

			if ((req.reply_len) &&
			    (req.reply[1] == 0x9a) && ((req.reply[2] == 0x21)
			    	|| (req.reply[2] == 0x20))) {
				mouse_kind = ADBMOUSE_TRACKBALL;
				init_trackball(id);
			}
			else if ((req.reply_len >= 4) &&
			    (req.reply[1] == 0x74) && (req.reply[2] == 0x70) &&
			    (req.reply[3] == 0x61) && (req.reply[4] == 0x64)) {
				mouse_kind = ADBMOUSE_TRACKPAD;
				init_trackpad(id);
			}
			else if ((req.reply_len >= 4) &&
			    (req.reply[1] == 0x4b) && (req.reply[2] == 0x4d) &&
			    (req.reply[3] == 0x4c) && (req.reply[4] == 0x31)) {
				mouse_kind = ADBMOUSE_TURBOMOUSE5;
				init_turbomouse(id);
			}
			else if ((req.reply_len == 9) &&
			    (req.reply[1] == 0x4b) && (req.reply[2] == 0x4f) &&
			    (req.reply[3] == 0x49) && (req.reply[4] == 0x54)) {
				if (adb_try_handler_change(id, 0x42)) {
					printk("\nADB MacAlly 2-button mouse at %d, handler set to 0x42", id);
					mouse_kind = ADBMOUSE_MACALLY2;
				}
			}
		}
		printk("\n");

		adb_get_infos(id, &default_id, &cur_handler_id);
		adbhid_input_register(id, default_id, org_handler_id,
				      cur_handler_id, mouse_kind);
        }
}

static void 
init_trackpad(int id)
{
	struct adb_request req;
	unsigned char r1_buffer[8];

	printk(" (trackpad)");

	adb_request(&req, NULL, ADBREQ_SYNC | ADBREQ_REPLY, 1,
	ADB_READREG(id,1));
	if (req.reply_len < 8)
	    printk("bad length for reg. 1\n");
	else
	{
	    memcpy(r1_buffer, &req.reply[1], 8);
	    adb_request(&req, NULL, ADBREQ_SYNC, 9,
	        ADB_WRITEREG(id,1),
	            r1_buffer[0],
	            r1_buffer[1],
	            r1_buffer[2],
	            r1_buffer[3],
	            r1_buffer[4],
	            r1_buffer[5],
	            0x0d, /*r1_buffer[6],*/
	            r1_buffer[7]);

            adb_request(&req, NULL, ADBREQ_SYNC, 9,
	        ADB_WRITEREG(id,2),
	    	    0x99,
	    	    0x94,
	    	    0x19,
	    	    0xff,
	    	    0xb2,
	    	    0x8a,
	    	    0x1b,
	    	    0x50);
	    
	    adb_request(&req, NULL, ADBREQ_SYNC, 9,
	        ADB_WRITEREG(id,1),
	            r1_buffer[0],
	            r1_buffer[1],
	            r1_buffer[2],
	            r1_buffer[3],
	            r1_buffer[4],
	            r1_buffer[5],
	            0x03, /*r1_buffer[6],*/
	            r1_buffer[7]);
        }
}

static void 
init_trackball(int id)
{
	struct adb_request req;

	printk(" (trackman/mouseman)");

	adb_request(&req, NULL, ADBREQ_SYNC, 3,
	ADB_WRITEREG(id,1), 00,0x81);

	adb_request(&req, NULL, ADBREQ_SYNC, 3,
	ADB_WRITEREG(id,1), 01,0x81);

	adb_request(&req, NULL, ADBREQ_SYNC, 3,
	ADB_WRITEREG(id,1), 02,0x81);

	adb_request(&req, NULL, ADBREQ_SYNC, 3,
	ADB_WRITEREG(id,1), 03,0x38);

	adb_request(&req, NULL, ADBREQ_SYNC, 3,
	ADB_WRITEREG(id,1), 00,0x81);

	adb_request(&req, NULL, ADBREQ_SYNC, 3,
	ADB_WRITEREG(id,1), 01,0x81);

	adb_request(&req, NULL, ADBREQ_SYNC, 3,
	ADB_WRITEREG(id,1), 02,0x81);

	adb_request(&req, NULL, ADBREQ_SYNC, 3,
	ADB_WRITEREG(id,1), 03,0x38);
}

static void
init_turbomouse(int id)
{
	struct adb_request req;

        printk(" (TurboMouse 5)");

	adb_request(&req, NULL, ADBREQ_SYNC, 1, ADB_FLUSH(id));

	adb_request(&req, NULL, ADBREQ_SYNC, 1, ADB_FLUSH(3));

	adb_request(&req, NULL, ADBREQ_SYNC, 9,
	ADB_WRITEREG(3,2),
	    0xe7,
	    0x8c,
	    0,
	    0,
	    0,
	    0xff,
	    0xff,
	    0x94);

	adb_request(&req, NULL, ADBREQ_SYNC, 1, ADB_FLUSH(3));

	adb_request(&req, NULL, ADBREQ_SYNC, 9,
	ADB_WRITEREG(3,2),
	    0xa5,
	    0x14,
	    0,
	    0,
	    0x69,
	    0xff,
	    0xff,
	    0x27);
}

static void
init_microspeed(int id)
{
	struct adb_request req;

        printk(" (Microspeed/MacPoint or compatible)");

	adb_request(&req, NULL, ADBREQ_SYNC, 1, ADB_FLUSH(id));

	/* This will initialize mice using the Microspeed, MacPoint and
	   other compatible firmware. Bit 12 enables extended protocol.
	   
	   Register 1 Listen (4 Bytes)
            0 -  3     Button is mouse (set also for double clicking!!!)
            4 -  7     Button is locking (affects change speed also)
            8 - 11     Button changes speed
           12          1 = Extended mouse mode, 0 = normal mouse mode
           13 - 15     unused 0
           16 - 23     normal speed
           24 - 31     changed speed

       Register 1 talk holds version and product identification information.
       Register 1 Talk (4 Bytes):
            0 -  7     Product code
            8 - 23     undefined, reserved
           24 - 31     Version number
        
       Speed 0 is max. 1 to 255 set speed in increments of 1/256 of max.
 */
	adb_request(&req, NULL, ADBREQ_SYNC, 5,
	ADB_WRITEREG(id,1),
	    0x20,	/* alt speed = 0x20 (rather slow) */
	    0x00,	/* norm speed = 0x00 (fastest) */
	    0x10,	/* extended protocol, no speed change */
	    0x07);	/* all buttons enabled as mouse buttons, no locking */


	adb_request(&req, NULL, ADBREQ_SYNC, 1, ADB_FLUSH(id));
}

static void
init_ms_a3(int id)
{
	struct adb_request req;

	printk(" (Mouse Systems A3 Mouse, or compatible)");
	adb_request(&req, NULL, ADBREQ_SYNC, 3,
	ADB_WRITEREG(id, 0x2),
	    0x00,
	    0x07);
 
 	adb_request(&req, NULL, ADBREQ_SYNC, 1, ADB_FLUSH(id));
}

static int __init adbhid_init(void)
{
	if ( (_machine != _MACH_chrp) && (_machine != _MACH_Pmac) )
	    return 0;

	led_request.complete = 1;

	adbhid_probe();

	notifier_chain_register(&adb_client_list, &adbhid_adb_notifier);

	return 0;
}

static void __exit adbhid_exit(void)
{
}
 
module_init(adbhid_init);
module_exit(adbhid_exit);
