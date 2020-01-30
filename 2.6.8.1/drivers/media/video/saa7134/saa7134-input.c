/*
 * handle saa7134 IR remotes via linux kernel input layer.
 *
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
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/input.h>

#include "saa7134-reg.h"
#include "saa7134.h"

static unsigned int disable_ir = 0;
MODULE_PARM(disable_ir,"i");
MODULE_PARM_DESC(disable_ir,"disable infrared remote support");

static unsigned int ir_debug = 0;
MODULE_PARM(ir_debug,"i");
MODULE_PARM_DESC(ir_debug,"enable debug messages [IR]");

#define dprintk(fmt, arg...)	if (ir_debug) \
	printk(KERN_DEBUG "%s/ir: " fmt, dev->name , ## arg)

/* ---------------------------------------------------------------------- */

static IR_KEYTAB_TYPE flyvideo_codes[IR_KEYTAB_SIZE] = {
	[   15 ] = KEY_KP0,
	[    3 ] = KEY_KP1,
	[    4 ] = KEY_KP2,
	[    5 ] = KEY_KP3,
	[    7 ] = KEY_KP4,
	[    8 ] = KEY_KP5,
	[    9 ] = KEY_KP6,
	[   11 ] = KEY_KP7,
	[   12 ] = KEY_KP8,
	[   13 ] = KEY_KP9,

	[   14 ] = KEY_TUNER,        // Air/Cable
	[   17 ] = KEY_VIDEO,        // Video
	[   21 ] = KEY_AUDIO,        // Audio
	[    0 ] = KEY_POWER,        // Pover
	[    2 ] = KEY_ZOOM,         // Fullscreen
	[   27 ] = KEY_MUTE,         // Mute
	[   20 ] = KEY_VOLUMEUP,
	[   23 ] = KEY_VOLUMEDOWN,
	[   18 ] = KEY_CHANNELUP,    // Channel +
	[   19 ] = KEY_CHANNELDOWN,  // Channel - 
	[    6 ] = KEY_AGAIN,        // Recal
	[   16 ] = KEY_KPENTER,      // Enter

#if 1 /* FIXME */
	[   26 ] = KEY_F22,          // Stereo
	[   24 ] = KEY_EDIT,         // AV Source
#endif
};

static IR_KEYTAB_TYPE cinergy_codes[IR_KEYTAB_SIZE] = {
	[    0 ] = KEY_KP0,
	[    1 ] = KEY_KP1,
	[    2 ] = KEY_KP2,
	[    3 ] = KEY_KP3,
	[    4 ] = KEY_KP4,
	[    5 ] = KEY_KP5,
	[    6 ] = KEY_KP6,
	[    7 ] = KEY_KP7,
	[    8 ] = KEY_KP8,
	[    9 ] = KEY_KP9,

	[ 0x0a ] = KEY_POWER,
	[ 0x0b ] = KEY_PROG1,           // app
	[ 0x0c ] = KEY_ZOOM,            // zoom/fullscreen
	[ 0x0d ] = KEY_CHANNELUP,       // channel
	[ 0x0e ] = KEY_CHANNELDOWN,     // channel-
	[ 0x0f ] = KEY_VOLUMEUP,
	[ 0x10 ] = KEY_VOLUMEDOWN,
	[ 0x11 ] = KEY_TUNER,           // AV
	[ 0x12 ] = KEY_NUMLOCK,         // -/--
	[ 0x13 ] = KEY_AUDIO,           // audio
	[ 0x14 ] = KEY_MUTE,
	[ 0x15 ] = KEY_UP,
	[ 0x16 ] = KEY_DOWN,
	[ 0x17 ] = KEY_LEFT,
	[ 0x18 ] = KEY_RIGHT,
	[ 0x19 ] = BTN_LEFT,
	[ 0x1a ] = BTN_RIGHT,
	[ 0x1b ] = KEY_WWW,             // text
	[ 0x1c ] = KEY_REWIND,
	[ 0x1d ] = KEY_FORWARD,
	[ 0x1e ] = KEY_RECORD,
	[ 0x1f ] = KEY_PLAY,
	[ 0x20 ] = KEY_PREVIOUSSONG,
	[ 0x21 ] = KEY_NEXTSONG,
	[ 0x22 ] = KEY_PAUSE,
	[ 0x23 ] = KEY_STOP,
};

/* Alfons Geser <a.geser@cox.net>
 * updates from Job D. R. Borges <jobdrb@ig.com.br> */
static IR_KEYTAB_TYPE eztv_codes[IR_KEYTAB_SIZE] = {
        [ 18 ] = KEY_POWER,
        [  1 ] = KEY_TV,             // DVR
        [ 21 ] = KEY_DVD,            // DVD
        [ 23 ] = KEY_AUDIO,          // music
                                     // DVR mode / DVD mode / music mode

        [ 27 ] = KEY_MUTE,           // mute
        [  2 ] = KEY_LANGUAGE,       // MTS/SAP / audio / autoseek
        [ 30 ] = KEY_SUBTITLE,       // closed captioning / subtitle / seek
        [ 22 ] = KEY_ZOOM,           // full screen
        [ 28 ] = KEY_VIDEO,          // video source / eject / delall
        [ 29 ] = KEY_RESTART,        // playback / angle / del
        [ 47 ] = KEY_SEARCH,         // scan / menu / playlist
        [ 48 ] = KEY_CHANNEL,        // CH surfing / bookmark / memo

        [ 49 ] = KEY_HELP,           // help
        [ 50 ] = KEY_MODE,           // num/memo
        [ 51 ] = KEY_ESC,            // cancel

	[ 12 ] = KEY_UP,             // up
	[ 16 ] = KEY_DOWN,           // down
	[  8 ] = KEY_LEFT,           // left
	[  4 ] = KEY_RIGHT,          // right
	[  3 ] = KEY_SELECT,         // select

	[ 31 ] = KEY_REWIND,         // rewind
	[ 32 ] = KEY_PLAYPAUSE,      // play/pause
	[ 41 ] = KEY_FORWARD,        // forward
	[ 20 ] = KEY_AGAIN,          // repeat
	[ 43 ] = KEY_RECORD,         // recording
	[ 44 ] = KEY_STOP,           // stop
	[ 45 ] = KEY_PLAY,           // play
	[ 46 ] = KEY_SHUFFLE,        // snapshot / shuffle

        [  0 ] = KEY_KP0,
        [  5 ] = KEY_KP1,
        [  6 ] = KEY_KP2,
        [  7 ] = KEY_KP3,
        [  9 ] = KEY_KP4,
        [ 10 ] = KEY_KP5,
        [ 11 ] = KEY_KP6,
        [ 13 ] = KEY_KP7,
        [ 14 ] = KEY_KP8,
        [ 15 ] = KEY_KP9,

        [ 42 ] = KEY_VOLUMEUP,
        [ 17 ] = KEY_VOLUMEDOWN,
        [ 24 ] = KEY_CHANNELUP,      // CH.tracking up
        [ 25 ] = KEY_CHANNELDOWN,    // CH.tracking down

        [ 19 ] = KEY_KPENTER,        // enter
        [ 33 ] = KEY_KPDOT,          // . (decimal dot)
};

static IR_KEYTAB_TYPE avacssmart_codes[IR_KEYTAB_SIZE] = {
        [ 30 ] = KEY_POWER,		// power
	[ 28 ] = KEY_SEARCH,		// scan
        [  7 ] = KEY_SELECT,		// source

	[ 22 ] = KEY_VOLUMEUP,
	[ 20 ] = KEY_VOLUMEDOWN,
        [ 31 ] = KEY_CHANNELUP,
	[ 23 ] = KEY_CHANNELDOWN,
	[ 24 ] = KEY_MUTE,

	[  2 ] = KEY_KP0,
        [  1 ] = KEY_KP1,
        [ 11 ] = KEY_KP2,
        [ 27 ] = KEY_KP3,
        [  5 ] = KEY_KP4,
        [  9 ] = KEY_KP5,
        [ 21 ] = KEY_KP6,
	[  6 ] = KEY_KP7,
        [ 10 ] = KEY_KP8,
	[ 18 ] = KEY_KP9,
	[ 16 ] = KEY_KPDOT,

	[  3 ] = KEY_TUNER,		// tv/fm
        [  4 ] = KEY_REWIND,		// fm tuning left or function left
        [ 12 ] = KEY_FORWARD,		// fm tuning right or function right

	[  0 ] = KEY_RECORD,
        [  8 ] = KEY_STOP,
        [ 17 ] = KEY_PLAY,

	[ 25 ] = KEY_ZOOM,
	[ 14 ] = KEY_MENU,		// function
	[ 19 ] = KEY_AGAIN,		// recall
	[ 29 ] = KEY_RESTART,		// reset

// FIXME
	[ 13 ] = KEY_F21,		// mts
        [ 15 ] = KEY_F22,		// min
	[ 26 ] = KEY_F23,		// freeze
};
/* ---------------------------------------------------------------------- */

static int build_key(struct saa7134_dev *dev)
{
	struct saa7134_ir *ir = dev->remote;
	u32 gpio, data;

	/* rising SAA7134_GPIO_GPRESCAN reads the status */
	saa_clearb(SAA7134_GPIO_GPMODE3,SAA7134_GPIO_GPRESCAN);
	saa_setb(SAA7134_GPIO_GPMODE3,SAA7134_GPIO_GPRESCAN);

	gpio = saa_readl(SAA7134_GPIO_GPSTATUS0 >> 2);
        if (ir->polling) {
                if (ir->last_gpio == gpio)
                        return 0;
                ir->last_gpio = gpio;
        }

 	data = ir_extract_bits(gpio, ir->mask_keycode);
	dprintk("build_key gpio=0x%x mask=0x%x data=%d\n",
		gpio, ir->mask_keycode, data);

	if ((ir->mask_keydown  &&  (0 != (gpio & ir->mask_keydown))) ||
	    (ir->mask_keyup    &&  (0 == (gpio & ir->mask_keyup)))) {
		ir_input_keydown(&ir->dev,&ir->ir,data,data);
	} else {
		ir_input_nokey(&ir->dev,&ir->ir);
	}
	return 0;
}

/* ---------------------------------------------------------------------- */

void saa7134_input_irq(struct saa7134_dev *dev)
{
        struct saa7134_ir *ir = dev->remote;

        if (!ir->polling)
		build_key(dev);
}

static void saa7134_input_timer(unsigned long data)
{
	struct saa7134_dev *dev = (struct saa7134_dev*)data;
	struct saa7134_ir *ir = dev->remote;
	unsigned long timeout;

	build_key(dev);
	timeout = jiffies + (ir->polling * HZ / 1000);
	mod_timer(&ir->timer, timeout);
}

int saa7134_input_init1(struct saa7134_dev *dev)
{
	struct saa7134_ir *ir;
	IR_KEYTAB_TYPE *ir_codes = NULL;
	u32 mask_keycode = 0;
	u32 mask_keydown = 0;
	u32 mask_keyup   = 0;
	int polling      = 0;
	int ir_type      = IR_TYPE_OTHER;

	if (!dev->has_remote)
		return -ENODEV;
	if (disable_ir)
		return -ENODEV;

	/* detect & configure */
	switch (dev->board) {
	case SAA7134_BOARD_FLYVIDEO2000:
	case SAA7134_BOARD_FLYVIDEO3000:
		ir_codes     = flyvideo_codes;
		mask_keycode = 0xEC00000;
		mask_keydown = 0x0040000;
		break;
	case SAA7134_BOARD_CINERGY400:
	case SAA7134_BOARD_CINERGY600:
		ir_codes     = cinergy_codes;
		mask_keycode = 0x00003f;
		mask_keyup   = 0x040000;
		break;
	case SAA7134_BOARD_ECS_TVP3XP:
	case SAA7134_BOARD_ECS_TVP3XP_4CB5:
                ir_codes     = eztv_codes;
                mask_keycode = 0x00017c;
                mask_keyup   = 0x000002;
		polling      = 50; // ms
                break;
	case SAA7134_BOARD_AVACSSMARTTV:
	        ir_codes     = avacssmart_codes;
		mask_keycode = 0x00001F;
		mask_keyup   = 0x000020;
		polling      = 50; // ms
		break;
	}
	if (NULL == ir_codes) {
		printk("%s: Oops: IR config error [card=%d]\n",
		       dev->name, dev->board);
		return -ENODEV;
	}

	ir = kmalloc(sizeof(*ir),GFP_KERNEL);
	if (NULL == ir)
		return -ENOMEM;
	memset(ir,0,sizeof(*ir));

	/* init hardware-specific stuff */
	ir->mask_keycode = mask_keycode;
	ir->mask_keydown = mask_keydown;
	ir->mask_keyup   = mask_keyup;
        ir->polling      = polling;
	
	/* init input device */
	snprintf(ir->name, sizeof(ir->name), "saa7134 IR (%s)",
		 saa7134_boards[dev->board].name);
	snprintf(ir->phys, sizeof(ir->phys), "pci-%s/ir0",
		 pci_name(dev->pci));

	ir_input_init(&ir->dev, &ir->ir, ir_type, ir_codes);
	ir->dev.name = ir->name;
	ir->dev.phys = ir->phys;
	ir->dev.id.bustype = BUS_PCI;
	ir->dev.id.version = 1;
	if (dev->pci->subsystem_vendor) {
		ir->dev.id.vendor  = dev->pci->subsystem_vendor;
		ir->dev.id.product = dev->pci->subsystem_device;
	} else {
		ir->dev.id.vendor  = dev->pci->vendor;
		ir->dev.id.product = dev->pci->device;
	}

	/* all done */
	dev->remote = ir;
	if (ir->polling) {
		init_timer(&ir->timer);
		ir->timer.function = saa7134_input_timer;
		ir->timer.data     = (unsigned long)dev;
		ir->timer.expires  = jiffies + HZ;
		add_timer(&ir->timer);
	}

	input_register_device(&dev->remote->dev);
	printk("%s: registered input device for IR\n",dev->name);
	return 0;
}

void saa7134_input_fini(struct saa7134_dev *dev)
{
	if (NULL == dev->remote)
		return;
	
	input_unregister_device(&dev->remote->dev);
	if (dev->remote->polling)
		del_timer_sync(&dev->remote->timer);
	kfree(dev->remote);
	dev->remote = NULL;
}

/* ----------------------------------------------------------------------
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
