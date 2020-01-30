/*
 * For the STS-Thompson TDA7432 audio processor chip
 * 
 * Handles audio functions: volume, balance, tone, loudness
 * This driver will not complain if used with any 
 * other i2c device with the same address.
 *
 * Copyright (c) 2000 Eric Sandeen <eric_sandeen@bigfoot.com>
 * This code is placed under the terms of the GNU General Public License
 * Based on tda9855.c by Steve VanDeBogart (vandebo@uclink.berkeley.edu)
 * Which was based on tda8425.c by Greg Alexander (c) 1998
 *
 * OPTIONS:
 * debug    - set to 1 if you'd like to see debug messages
 *            set to 2 if you'd like to be inundated with debug messages
 *
 * loudness - set between 0 and 15 for varying degrees of loudness effect
 *
 * TODO:
 *  Implement tone controls
 *
 *  Revision: 0.3 - Fixed silly reversed volume controls.  :)
 *  Revision: 0.2 - Cleaned up #defines
 *			fixed volume control
 *                  Added I2C_DRIVERID_TDA7432
 *			added loudness insmod control
 *  Revision: 0.1 - initial version
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/malloc.h>
#include <linux/videodev.h>
#include <linux/i2c.h>
#include <linux/i2c-algo-bit.h>

#include "bttv.h"
#include "audiochip.h"
#include "id.h"

MODULE_AUTHOR("Eric Sandeen <eric_sandeen@bigfoot.com>");
MODULE_DESCRIPTION("bttv driver for the tda7432 audio processor chip");

MODULE_PARM(debug,"i");
MODULE_PARM(loudness,"i");
static int loudness = 0; /* disable loudness by default */
static int debug = 0;	 /* insmod parameter */


/* Address to scan (I2C address of this chip) */
static unsigned short normal_i2c[] = {
	I2C_TDA7432 >> 1,
	I2C_CLIENT_END};
static unsigned short normal_i2c_range[] = {I2C_CLIENT_END};
static unsigned short probe[2]        = { I2C_CLIENT_END, I2C_CLIENT_END };
static unsigned short probe_range[2]  = { I2C_CLIENT_END, I2C_CLIENT_END };
static unsigned short ignore[2]       = { I2C_CLIENT_END, I2C_CLIENT_END };
static unsigned short ignore_range[2] = { I2C_CLIENT_END, I2C_CLIENT_END };
static unsigned short force[2]        = { I2C_CLIENT_END, I2C_CLIENT_END };
static struct i2c_client_address_data addr_data = {
	normal_i2c, normal_i2c_range, 
	probe, probe_range, 
	ignore, ignore_range, 
	force
};

/* Structure of address and subaddresses for the tda7432 */

struct tda7432 {
	int addr;
	int input;
	int volume;
	int tone;
	int lf, lr, rf, rr;
	int loud;
	struct i2c_client c;
};

static struct i2c_driver driver;
static struct i2c_client client_template;

#define dprintk  if (debug) printk
#define d2printk if (debug > 1) printk

/* The TDA7432 is made by STS-Thompson
 * http://www.st.com
 * http://us.st.com/stonline/books/pdf/docs/4056.pdf
 *
 * TDA7432: I2C-bus controlled basic audio processor
 *
 * The TDA7432 controls basic audio functions like volume, balance,
 * and tone control (including loudness).  It also has four channel
 * output (for front and rear).  Since most vidcap cards probably
 * don't have 4 channel output, this driver will set front & rear
 * together (no independent control).
 */ 

		/* Subaddresses for TDA7432 */

#define TDA7432_IN	0x00 /* Input select                 */
#define TDA7432_VL	0x01 /* Volume                       */
#define TDA7432_TN	0x02 /* Bass, Treble (Tone)          */
#define TDA7432_LF	0x03 /* Attenuation LF (Left Front)  */
#define TDA7432_LR	0x04 /* Attenuation LR (Left Rear)   */
#define TDA7432_RF	0x05 /* Attenuation RF (Right Front) */
#define TDA7432_RR	0x06 /* Attenuation RR (Right Rear)  */
#define TDA7432_LD	0x07 /* Loudness                     */


		/* Masks for bits in TDA7432 subaddresses */

/* Many of these not used - just for documentation */

/* Subaddress 0x00 - Input selection and bass control */

/* Bits 0,1,2 control input:
 * 0x00 - Stereo input
 * 0x02 - Mono input
 * 0x03 - Mute
 * Mono probably isn't used - I'm guessing only the stereo
 * input is connected on most cards, so we'll set it to stereo.
 * 
 * Bit 3 controls bass cut: 0/1 is non-symmetric/symmetric bass cut
 * Bit 4 controls bass range: 0/1 is extended/standard bass range
 * 
 * Highest 3 bits not used
 */
 
#define TDA7432_STEREO_IN	0
#define TDA7432_MONO_IN		2	/* Probably won't be used */
#define TDA7432_MUTE		3	/* Probably won't be used */
#define TDA7432_BASS_SYM	1 << 3
#define TDA7432_BASS_NORM	1 << 4

/* Subaddress 0x01 - Volume */

/* Lower 7 bits control volume from -79dB to +32dB in 1dB steps
 * Recommended maximum is +20 dB
 *
 * +32dB: 0x00
 * +20dB: 0x0c
 *   0dB: 0x20
 * -79dB: 0x6f
 *
 * MSB (bit 7) controls loudness: 1/0 is loudness on/off
 */

#define	TDA7432_VOL_0DB		0x20
#define TDA7432_LD_ON		1 << 7


/* Subaddress 0x02 - Tone control */ 

/* Bits 0,1,2 control absolute treble gain from 0dB to 14dB
 * 0x0 is 14dB, 0x7 is 0dB
 *
 * Bit 3 controls treble attenuation/gain (sign)
 * 1 = gain (+)
 * 0 = attenuation (-)
 *
 * Bits 4,5,6 control absolute bass gain from 0dB to 14dB
 * (This is only true for normal base range, set in 0x00)
 * 0x0 << 4 is 14dB, 0x7 is 0dB
 * 
 * Bit 7 controls bass attenuation/gain (sign)
 * 1 << 7 = gain (+)
 * 0 << 7 = attenuation (-) 
 *
 * Example:
 * 1 1 0 1 0 1 0 1 is +4dB bass, -4dB treble
 */

#define TDA7432_TREBLE_0DB		0xf 
#define TDA7432_TREBLE			7
#define TDA7432_TREBLE_GAIN		1 << 3
#define TDA7432_BASS_0DB		0xf << 4
#define TDA7432_BASS			7 << 4
#define TDA7432_BASS_GAIN		1 << 7

 
/* Subaddress 0x03 - Left  Front attenuation */
/* Subaddress 0x04 - Left  Rear  attenuation */
/* Subaddress 0x05 - Right Front attenuation */
/* Subaddress 0x06 - Right Rear  attenuation */

/* Bits 0,1,2,3,4 control attenuation from 0dB to -37.5dB
 * in 1.5dB steps.
 *
 * 0x00 is     0dB
 * 0x1f is -37.5dB
 *
 * Bit 5 mutes that channel when set (1 = mute, 0 = unmute)
 * We'll use the mute on the input, though (above) 
 * Bits 6,7 unused
 */

#define TDA7432_ATTEN_0DB	0x00


/* Subaddress 0x07 - Loudness Control */

/* Bits 0,1,2,3 control loudness from 0dB to -15dB in 1dB steps
 * when bit 4 is NOT set
 *
 * 0x0 is   0dB
 * 0xf is -15dB
 *
 * If bit 4 is set, then there is a flat attenuation according to
 * the lower 4 bits, as above.
 *
 * Bits 5,6,7 unused
 */
 
 

/* Begin code */

static int tda7432_write(struct i2c_client *client, int subaddr, int val)
{
	unsigned char buffer[2];
	d2printk("tda7432: In tda7432_write\n");
	dprintk("tda7432: Writing %d 0x%x\n", subaddr, val);
	buffer[0] = subaddr;
	buffer[1] = val;
	if (2 != i2c_master_send(client,buffer,2)) {
		printk(KERN_WARNING "tda7432: I/O error, trying (write %d 0x%x)\n",
		       subaddr, val);
		return -1;
	}
	return 0;
}

/* I don't think we ever actually _read_ the chip... */
#if 0
static int tda7432_read(struct i2c_client *client)
{
	unsigned char buffer;
	d2printk("tda7432: In tda7432_read\n");
	if (1 != i2c_master_recv(client,&buffer,1)) {
		printk(KERN_WARNING "tda7432: I/O error, trying (read)\n");
		return -1;
	}
	dprintk("tda7432: Read 0x%02x\n", buffer); 
	return buffer;
}
#endif

static int tda7432_set(struct i2c_client *client)
{
	struct tda7432 *t = client->data;
	unsigned char buf[16];
	d2printk("tda7432: In tda7432_set\n");
	
	dprintk(KERN_INFO 
		"tda7432: 7432_set(0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x,0x%02x)\n",
		t->input,t->volume,t->tone,t->lf,t->lr,t->rf,t->rr,t->loud);
	buf[0]  = TDA7432_IN;
	buf[1]  = t->input;
	buf[2]  = t->volume;
	buf[3]  = t->tone;
	buf[4]  = t->lf;
	buf[5]  = t->lr;
	buf[6]  = t->rf;
	buf[7]  = t->rr;
	buf[8]  = t->loud;
	if (9 != i2c_master_send(client,buf,9)) {
		printk(KERN_WARNING "tda7432: I/O error, trying tda7432_set\n");
		return -1;
	}

	return 0;
}

static void do_tda7432_init(struct i2c_client *client)
{
	struct tda7432 *t = client->data;
	d2printk("tda7432: In tda7432_init\n");

	t->input  = TDA7432_STEREO_IN |  /* Main (stereo) input   */
		    TDA7432_BASS_SYM  |  /* Symmetric bass cut    */
		    TDA7432_BASS_NORM;   /* Normal bass range     */ 
	t->volume = TDA7432_VOL_0DB;	 /* 0dB Volume            */
	if (loudness)			 /* Turn loudness on?     */
		t->volume |= TDA7432_LD_ON;	
	t->tone   = TDA7432_TREBLE_0DB | /* 0dB Treble            */
		    TDA7432_BASS_0DB; 	 /* 0dB Bass              */
	t->lf     = TDA7432_ATTEN_0DB;	 /* 0dB attenuation       */
	t->lr     = TDA7432_ATTEN_0DB;	 /* 0dB attenuation       */
	t->rf     = TDA7432_ATTEN_0DB;	 /* 0dB attenuation       */
	t->rr     = TDA7432_ATTEN_0DB;	 /* 0dB attenuation       */
	t->loud   = loudness;		 /* insmod parameter      */

	tda7432_set(client);
}

/* *********************** *
 * i2c interface functions *
 * *********************** */

static int tda7432_attach(struct i2c_adapter *adap, int addr,
			  unsigned short flags, int kind)
{
	struct tda7432 *t;
	struct i2c_client *client;
	d2printk("tda7432: In tda7432_attach\n");

	t = kmalloc(sizeof *t,GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	memset(t,0,sizeof *t);

	client = &t->c;
        memcpy(client,&client_template,sizeof(struct i2c_client));
        client->adapter = adap;
        client->addr = addr;
	client->data = t;
	
	do_tda7432_init(client);
	MOD_INC_USE_COUNT;
	strcpy(client->name,"TDA7432");
	printk(KERN_INFO "tda7432: init\n");

	i2c_attach_client(client);
	return 0;
}

static int tda7432_probe(struct i2c_adapter *adap)
{
	if (adap->id == (I2C_ALGO_BIT | I2C_HW_B_BT848))
		return i2c_probe(adap, &addr_data, tda7432_attach);
	return 0;
}

static int tda7432_detach(struct i2c_client *client)
{
	struct tda7432 *t  = client->data;

	do_tda7432_init(client);
	i2c_detach_client(client);
	
	kfree(t);
	MOD_DEC_USE_COUNT;
	return 0;
}

static int tda7432_command(struct i2c_client *client,
			   unsigned int cmd, void *arg)
{
	struct tda7432 *t = client->data;
	d2printk("tda7432: In tda7432_command\n");
#if 0
	__u16 *sarg = arg;
#endif

	switch (cmd) {
	/* --- v4l ioctls --- */
	/* take care: bttv does userspace copying, we'll get a
	   kernel pointer here... */
	
	/* Query card - scale from TDA7432 settings to V4L settings */
	case VIDIOCGAUDIO:
	{
		struct video_audio *va = arg;
		dprintk("tda7432: VIDIOCGAUDIO\n");

		va->flags |= VIDEO_AUDIO_VOLUME |
			VIDEO_AUDIO_BASS |
			VIDEO_AUDIO_TREBLE;

		/* Master volume control
		 * V4L volume is min 0, max 65535
		 * TDA7432 Volume: 
		 * Min (-79dB) is 0x6f
		 * Max (+20dB) is 0x07
		 * (Mask out bit 7 of vol - it's for the loudness setting)
		 */

		va->volume = ( 0x6f - (t->volume & 0x7F) ) * 630;
		
		/* Balance depends on L,R attenuation
		 * V4L balance is 0 to 65535, middle is 32768
		 * TDA7432 attenuation: min (0dB) is 0, max (-37.5dB) is 0x1f
		 * to scale up to V4L numbers, mult by 1057
		 * attenuation exists for lf, lr, rf, rr
		 * we use only lf and rf (front channels)
		 */
		 
		if ( (t->lf) < (t->rf) )
			/* right is attenuated, balance shifted left */
			va->balance = (32768 - 1057*(t->rf));
		else
			/* left is attenuated, balance shifted right */
			va->balance = (32768 + 1057*(t->lf));

		/* Bass/treble */
		va->bass = 32768;   /* brain hurts... set to middle for now */
		va->treble = 32768; /* brain hurts... set to middle for now */
		
		break; /* VIDIOCGAUDIO case */
	}

	/* Set card - scale from V4L settings to TDA7432 settings */
	case VIDIOCSAUDIO:
	{
		struct video_audio *va = arg;
		dprintk("tda7432: VIDEOCSAUDIO\n");

		t->volume = 0x6f - ( (va->volume)/630 );
		
		if (loudness)		/* Turn on the loudness bit */
			t->volume |= TDA7432_LD_ON;
		
		if (va->balance < 32768) {
			/* shifted to left, attenuate right */
			t->rr = (32768 - va->balance)/1057;
			t->rf = t->rr;
		}
		else {
			/* shifted to right, attenuate left */
			t->lf = (va->balance - 32768)/1057;
			t->lr = t->lf;
		}
		
		/* t->tone = 0xff; */ /* Brain hurts - no tone control for now... */

		tda7432_write(client,TDA7432_VL, t->volume);
		/* tda7432_write(client,TDA7432_TN, t->tone); */
		tda7432_write(client,TDA7432_LF, t->lf);
		tda7432_write(client,TDA7432_LR, t->lr);
		tda7432_write(client,TDA7432_RF, t->rf);
		tda7432_write(client,TDA7432_RR, t->rr);

		break;

	} /* end of VIDEOCSAUDIO case */

	default: /* Not VIDEOCGAUDIO or VIDEOCSAUDIO */

		/* nothing */
		d2printk("tda7432: Default\n");

	} /* end of (cmd) switch */

	return 0;
}


static struct i2c_driver driver = {
        "i2c tda7432 driver",
	I2C_DRIVERID_TDA7432,
        I2C_DF_NOTIFY,
	tda7432_probe,
        tda7432_detach,
        tda7432_command,
};

static struct i2c_client client_template =
{
        "(unset)",		/* name */
        -1,
        0,
        0,
        NULL,
        &driver
};

#ifdef MODULE
int init_module(void)
#else
int tda7432_init(void)
#endif
{

	if ( (loudness < 0) || (loudness > 15) )
	{
		printk(KERN_ERR "tda7432: loudness parameter must be between 0 and 15\n");
		return -EINVAL;
	}


	i2c_add_driver(&driver);
	return 0;
}

#ifdef MODULE
void cleanup_module(void)
{
	i2c_del_driver(&driver);
}
#endif

/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
