/*
 * some common structs and functions to handle infrared remotes via
 * input layer ...
 *
 * (c) 2003 Gerd Knorr <kraxel@bytesex.org> [SuSE Labs]
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/module.h>

#include <media/ir-common.h>

/* -------------------------------------------------------------------------- */

MODULE_AUTHOR("Gerd Knorr <kraxel@bytesex.org> [SuSE Labs]");
MODULE_LICENSE("GPL");

static int repeat = 1;
MODULE_PARM(repeat,"i");
MODULE_PARM_DESC(repeat,"auto-repeat for IR keys (default: on)");

static int debug = 0;    /* debug level (0,1,2) */
MODULE_PARM(debug,"i");

#define dprintk(level, fmt, arg...)	if (debug >= level) \
	printk(KERN_DEBUG fmt , ## arg)

/* -------------------------------------------------------------------------- */

/* generic RC5 keytable                                          */
/* see http://users.pandora.be/nenya/electronics/rc5/codes00.htm */
IR_KEYTAB_TYPE ir_codes_rc5_tv[IR_KEYTAB_SIZE] = {
	[ 0x00 ] = KEY_KP0,             // 0
	[ 0x01 ] = KEY_KP1,             // 1
	[ 0x02 ] = KEY_KP2,             // 2
	[ 0x03 ] = KEY_KP3,             // 3
	[ 0x04 ] = KEY_KP4,             // 4
	[ 0x05 ] = KEY_KP5,             // 5
	[ 0x06 ] = KEY_KP6,             // 6
	[ 0x07 ] = KEY_KP7,             // 7
	[ 0x08 ] = KEY_KP8,             // 8
	[ 0x09 ] = KEY_KP9,             // 9

	[ 0x0b ] = KEY_CHANNEL,         // channel / program (japan: 11)
	[ 0x0c ] = KEY_POWER,           // standby
	[ 0x0d ] = KEY_MUTE,            // mute / demute
	[ 0x0f ] = KEY_TV,              // display
	[ 0x10 ] = KEY_VOLUMEUP,        // volume +
	[ 0x11 ] = KEY_VOLUMEDOWN,      // volume -
	[ 0x12 ] = KEY_BRIGHTNESSUP,    // brightness +
	[ 0x13 ] = KEY_BRIGHTNESSDOWN,  // brightness -
	[ 0x1e ] = KEY_SEARCH,          // search +
	[ 0x20 ] = KEY_CHANNELUP,       // channel / program +
	[ 0x21 ] = KEY_CHANNELDOWN,     // channel / program -
	[ 0x22 ] = KEY_CHANNEL,         // alt / channel
	[ 0x23 ] = KEY_LANGUAGE,        // 1st / 2nd language
	[ 0x26 ] = KEY_SLEEP,           // sleeptimer
	[ 0x2e ] = KEY_MENU,            // 2nd controls (USA: menu)
	[ 0x30 ] = KEY_PAUSE,           // pause
	[ 0x32 ] = KEY_REWIND,          // rewind
	[ 0x33 ] = KEY_GOTO,            // go to
	[ 0x35 ] = KEY_PLAY,            // play
	[ 0x36 ] = KEY_STOP,            // stop
	[ 0x37 ] = KEY_RECORD,          // recording
	[ 0x3c ] = KEY_TEXT,            // teletext submode (Japan: 12)
	[ 0x3d ] = KEY_SUSPEND,         // system standby

#if 0 /* FIXME */
	[ 0x0a ] = KEY_RESERVED,        // 1/2/3 digits (japan: 10)
	[ 0x0e ] = KEY_RESERVED,        // P.P. (personal preference)
	[ 0x14 ] = KEY_RESERVED,        // colour saturation +
	[ 0x15 ] = KEY_RESERVED,        // colour saturation -
	[ 0x16 ] = KEY_RESERVED,        // bass +
	[ 0x17 ] = KEY_RESERVED,        // bass -
	[ 0x18 ] = KEY_RESERVED,        // treble +
	[ 0x19 ] = KEY_RESERVED,        // treble -
	[ 0x1a ] = KEY_RESERVED,        // balance right
	[ 0x1b ] = KEY_RESERVED,        // balance left
	[ 0x1c ] = KEY_RESERVED,        // contrast +
	[ 0x1d ] = KEY_RESERVED,        // contrast -
	[ 0x1f ] = KEY_RESERVED,        // tint/hue +
	[ 0x24 ] = KEY_RESERVED,        // spacial stereo on/off
	[ 0x25 ] = KEY_RESERVED,        // mono / stereo (USA)
	[ 0x27 ] = KEY_RESERVED,        // tint / hue -
	[ 0x28 ] = KEY_RESERVED,        // RF switch/PIP select
	[ 0x29 ] = KEY_RESERVED,        // vote
	[ 0x2a ] = KEY_RESERVED,        // timed page/channel clck
	[ 0x2b ] = KEY_RESERVED,        // increment (USA)
	[ 0x2c ] = KEY_RESERVED,        // decrement (USA)
	[ 0x2d ] = KEY_RESERVED,        // 
	[ 0x2f ] = KEY_RESERVED,        // PIP shift
	[ 0x31 ] = KEY_RESERVED,        // erase
	[ 0x34 ] = KEY_RESERVED,        // wind
	[ 0x38 ] = KEY_RESERVED,        // external 1
	[ 0x39 ] = KEY_RESERVED,        // external 2
	[ 0x3a ] = KEY_RESERVED,        // PIP display mode
	[ 0x3b ] = KEY_RESERVED,        // view data mode / advance
	[ 0x3e ] = KEY_RESERVED,        // crispener on/off
	[ 0x3f ] = KEY_RESERVED,        // system select
#endif
};
EXPORT_SYMBOL_GPL(ir_codes_rc5_tv);

/* empty keytable, can be used as placeholder for not-yet created keytables */
IR_KEYTAB_TYPE ir_codes_empty[IR_KEYTAB_SIZE] = {
	[ 42 ] = KEY_COFFEE,
};
EXPORT_SYMBOL_GPL(ir_codes_empty);

/* -------------------------------------------------------------------------- */

static void ir_input_key_event(struct input_dev *dev, struct ir_input_state *ir)
{
	if (KEY_RESERVED == ir->keycode) {
		printk(KERN_INFO "%s: unknown key: key=0x%02x raw=0x%02x down=%d\n",
		       dev->name,ir->ir_key,ir->ir_raw,ir->keypressed);
		return;
	}
	dprintk(1,"%s: key event code=%d down=%d\n",
		dev->name,ir->keycode,ir->keypressed);
	input_report_key(dev,ir->keycode,ir->keypressed);
        input_sync(dev);
}

/* -------------------------------------------------------------------------- */

void ir_input_init(struct input_dev *dev, struct ir_input_state *ir,
		   int ir_type, IR_KEYTAB_TYPE *ir_codes)
{
	int i;
	
	ir->ir_type = ir_type;
	if (ir_codes)
		memcpy(ir->ir_codes, ir_codes, sizeof(ir->ir_codes));

        init_input_dev(dev);
	dev->keycode     = ir->ir_codes;
	dev->keycodesize = sizeof(IR_KEYTAB_TYPE);
	dev->keycodemax  = IR_KEYTAB_SIZE;
	for (i = 0; i < IR_KEYTAB_SIZE; i++)
		set_bit(ir->ir_codes[i], dev->keybit);
	clear_bit(0, dev->keybit);

	set_bit(EV_KEY, dev->evbit);
	if (repeat)
		set_bit(EV_REP, dev->evbit);
}

void ir_input_nokey(struct input_dev *dev, struct ir_input_state *ir)
{
	if (ir->keypressed) {
		ir->keypressed = 0;
		ir_input_key_event(dev,ir);
	}
}

void ir_input_keydown(struct input_dev *dev, struct ir_input_state *ir,
		      u32 ir_key, u32 ir_raw)
{
	u32 keycode = IR_KEYCODE(ir->ir_codes, ir_key);
	
	if (ir->keypressed && ir->keycode != keycode) {
		ir->keypressed = 0;
		ir_input_key_event(dev,ir);
	}
	if (!ir->keypressed) {
		ir->ir_key  = ir_key;
		ir->ir_raw  = ir_raw;
		ir->keycode = keycode;
		ir->keypressed = 1;
		ir_input_key_event(dev,ir);
	}
#if 0
	/* maybe do something like this ??? */
	input_event(a, EV_IR, ir->ir_type, ir->ir_raw);
#endif
}

u32 ir_extract_bits(u32 data, u32 mask)
{
	int mbit, vbit;
	u32 value;

	value = 0;
	vbit  = 0;
	for (mbit = 0; mbit < 32; mbit++) {
		if (!(mask & ((u32)1 << mbit)))
			continue;
		if (data & ((u32)1 << mbit))
			value |= (1 << vbit);
		vbit++;
	}
	return value;
}

EXPORT_SYMBOL_GPL(ir_input_init);
EXPORT_SYMBOL_GPL(ir_input_nokey);
EXPORT_SYMBOL_GPL(ir_input_keydown);
EXPORT_SYMBOL_GPL(ir_extract_bits);

/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
