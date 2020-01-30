/*
 * For the TDA9875 chip
 * (The TDA9875 is used on the Diamond DTV2000 french version 
 * Other cards probably use these chips as well.)
 * This driver will not complain if used with any 
 * other i2c device with the same address.
 *
 * Copyright (c) 2000 Guillaume Delvit based on Gerd Knorr source and
 * Eric Sandeen 
 * This code is placed under the terms of the GNU General Public License
 * Based on tda9855.c by Steve VanDeBogart (vandebo@uclink.berkeley.edu)
 * Which was based on tda8425.c by Greg Alexander (c) 1998
 *
 * OPTIONS:
 * debug   - set to 1 if you'd like to see debug messages
 * 
 *  Revision: 0.1 - original version
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

MODULE_PARM(debug,"i");

static int debug = 0;	/* insmod parameter */

/* Addresses to scan */
static unsigned short normal_i2c[] =  {
    I2C_TDA9875 >> 1,
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

/* This is a superset of the TDA9875 */
struct tda9875 {
	int mode;
	int rvol, lvol;
	int bass, treble;
	struct i2c_client c;
};


static struct i2c_driver driver;
static struct i2c_client client_template;

#define dprintk  if (debug) printk

/* The TDA9875 is made by Philips Semiconductor
 * http://www.semiconductors.philips.com
 * TDA9875: I2C-bus controlled DSP audio processor, FM demodulator
 *
 */ 

		/* subaddresses for TDA9875 */
#define TDA9875_MUT         0x12  /*General mute  (value --> 0b11001100*/
#define TDA9875_CFG         0x01  /* Config register (value --> 0b00000000 */
#define TDA9875_DACOS       0x13  /*DAC i/o select (ADC) 0b0000100*/
#define TDA9875_LOSR        0x16  /*Line output select regirter 0b0100 0001*/

#define TDA9875_CH1V        0x0c  /*Chanel 1 volume (mute)*/
#define TDA9875_CH2V        0x0d  /*Chanel 2 volume (mute)*/
#define TDA9875_SC1         0x14  /*SCART 1 in (mono)*/
#define TDA9875_SC2         0x15  /*SCART 2 in (mono)*/

#define TDA9875_ADCIS       0x17  /*ADC input select (mono) 0b0110 000*/
#define TDA9875_AER         0x19  /*Audio effect (AVL+Pseudo) 0b0000 0110*/
#define TDA9875_MCS         0x18  /*Main channel select (DAC) 0b0000100*/
#define TDA9875_MVL         0x1a  /* Main volume gauche */
#define TDA9875_MVR         0x1b  /* Main volume droite */
#define TDA9875_MBA         0x1d  /* Main Basse */
#define TDA9875_MTR         0x1e  /* Main treble */
#define TDA9875_ACS         0x1f  /* Auxilary channel select (FM) 0b0000000*/
#define TDA9875_AVL         0x20  /* Auxilary volume gauche */
#define TDA9875_AVR         0x21  /* Auxilary volume droite */
#define TDA9875_ABA         0x22  /* Auxilary Basse */
#define TDA9875_ATR         0x23  /* Auxilary treble */

#define TDA9875_MSR         0x02  /* Monitor select register */
#define TDA9875_C1MSB       0x03  /* Carrier 1 (FM) frequency register MSB */
#define TDA9875_C1MIB       0x04  /* Carrier 1 (FM) frequency register (16-8]b */
#define TDA9875_C1LSB       0x05  /* Carrier 1 (FM) frequency register LSB */
#define TDA9875_C2MSB       0x06  /* Carrier 2 (nicam) frequency register MSB */
#define TDA9875_C2MIB       0x07  /* Carrier 2 (nicam) frequency register (16-8]b */
#define TDA9875_C2LSB       0x08  /* Carrier 2 (nicam) frequency register LSB */
#define TDA9875_DCR         0x09  /* Demodulateur configuration regirter*/
#define TDA9875_DEEM        0x0a  /* FM de-emphasis regirter*/
#define TDA9875_FMAT        0x0b  /* FM Matrix regirter*/

/* values */
#define TDA9875_MUTE_ON	    0xff /* general mute */
#define TDA9875_MUTE_OFF    0xcc /* general no mute */



/* Begin code */

static int tda9875_write(struct i2c_client *client, int subaddr, unsigned char val)
{
	unsigned char buffer[2];
	dprintk("In tda9875_write\n");
	dprintk("Writing %d 0x%x\n", subaddr, val);
	buffer[0] = subaddr;
	buffer[1] = val;
	if (2 != i2c_master_send(client,buffer,2)) {
		printk(KERN_WARNING "tda9875: I/O error, trying (write %d 0x%x)\n",
		       subaddr, val);
		return -1;
	}
	return 0;
}

#if 0
static int tda9875_read(struct i2c_client *client)
{
	unsigned char buffer;
	dprintk("In tda9875_read\n");
	if (1 != i2c_master_recv(client,&buffer,1)) {
		printk(KERN_WARNING "tda9875: I/O error, trying (read)\n");
		return -1;
	}
	dprintk("Read 0x%02x\n", buffer); 
	return buffer;
}
#endif

static void tda9875_set(struct i2c_client *client)
{
	struct tda9875 *tda = client->data;
	unsigned char a;

	dprintk(KERN_DEBUG "tda9875_set(%04x,%04x,%04x,%04x)\n",tda->lvol,tda->rvol,tda->bass,tda->treble);


	a = tda->lvol & 0xff;
	tda9875_write(client, TDA9875_MVL, a);
	a =tda->rvol & 0xff;
	tda9875_write(client, TDA9875_MVR, a);
	a =tda->bass & 0xff;
	tda9875_write(client, TDA9875_MBA, a);
	a =tda->treble  & 0xff;
	tda9875_write(client, TDA9875_MTR, a);
}

static void do_tda9875_init(struct i2c_client *client)
{
	struct tda9875 *t = client->data;
	dprintk("In tda9875_init\n"); 
	tda9875_write(client, TDA9875_CFG, 0xd0 ); /*reg de config 0 (reset)*/
    	tda9875_write(client, TDA9875_MSR, 0x03 );    /* Monitor 0b00000XXX*/
	tda9875_write(client, TDA9875_C1MSB, 0x00 );  /*Car1(FM) MSB XMHz*/
	tda9875_write(client, TDA9875_C1MIB, 0x00 );  /*Car1(FM) MIB XMHz*/
	tda9875_write(client, TDA9875_C1LSB, 0x00 );  /*Car1(FM) LSB XMHz*/
	tda9875_write(client, TDA9875_C2MSB, 0x00 );  /*Car2(NICAM) MSB XMHz*/
	tda9875_write(client, TDA9875_C2MIB, 0x00 );  /*Car2(NICAM) MIB XMHz*/
	tda9875_write(client, TDA9875_C2LSB, 0x00 );  /*Car2(NICAM) LSB XMHz*/
	tda9875_write(client, TDA9875_DCR, 0x00 );    /*Demod config 0x00*/
	tda9875_write(client, TDA9875_DEEM, 0x44 );   /*DE-Emph 0b0100 0100*/
	tda9875_write(client, TDA9875_FMAT, 0x00 );   /*FM Matrix reg 0x00*/
	tda9875_write(client, TDA9875_SC1, 0x00 );    /* SCART 1 (SC1)*/
	tda9875_write(client, TDA9875_SC2, 0x01 );    /* SCART 2 (sc2)*/
        
	tda9875_write(client, TDA9875_CH1V, 0x10 );  /* Chanel volume 1 mute*/
	tda9875_write(client, TDA9875_CH2V, 0x10 );  /* Chanel volume 2 mute */
	tda9875_write(client, TDA9875_DACOS, 0x02 ); /* sig DAC i/o(in:nicam)*/
	tda9875_write(client, TDA9875_ADCIS, 0x6f ); /* sig ADC input(in:mono)*/
	tda9875_write(client, TDA9875_LOSR, 0x00 );  /* line out (in:mono)*/
 	tda9875_write(client, TDA9875_AER, 0x00 );   /*06 Effect (AVL+PSEUDO) */
	tda9875_write(client, TDA9875_MCS, 0x44 );   /* Main ch select (DAC) */
	tda9875_write(client, TDA9875_MVL, 0x03 );   /* Vol Main left 10dB */
	tda9875_write(client, TDA9875_MVR, 0x03 );   /* Vol Main right 10dB*/
	tda9875_write(client, TDA9875_MBA, 0x00 );   /* Main Bass Main 0dB*/
	tda9875_write(client, TDA9875_MTR, 0x00 );   /* Main Treble Main 0dB*/
	tda9875_write(client, TDA9875_ACS, 0x44 );   /* Aux chan select (dac)*/	
	tda9875_write(client, TDA9875_AVL, 0x00 );   /* Vol Aux left 0dB*/
	tda9875_write(client, TDA9875_AVR, 0x00 );   /* Vol Aux right 0dB*/
	tda9875_write(client, TDA9875_ABA, 0x00 );   /* Aux Bass Main 0dB*/
	tda9875_write(client, TDA9875_ATR, 0x00 );   /* Aux Aigus Main 0dB*/
	
	tda9875_write(client, TDA9875_MUT, 0xcc );   /* General mute  */
        
	t->mode=AUDIO_UNMUTE;
	t->lvol=t->rvol =0;  	/* 0dB */
	t->bass=0; 		        /* 0dB */
	t->treble=0;  		/* 0dB */
	tda9875_set(client);

}


/* *********************** *
 * i2c interface functions *
 * *********************** */

static int tda9875_attach(struct i2c_adapter *adap, int addr,
			  unsigned short flags, int kind)
{
	struct tda9875 *t;
	struct i2c_client *client;
	dprintk("In tda9875_attach\n");

	t = kmalloc(sizeof *t,GFP_KERNEL);
	if (!t)
		return -ENOMEM;
	memset(t,0,sizeof *t);

	client = &t->c;
        memcpy(client,&client_template,sizeof(struct i2c_client));
        client->adapter = adap;
        client->addr = addr;
	client->data = t;
	
	do_tda9875_init(client);
	MOD_INC_USE_COUNT;
	strcpy(client->name,"TDA9875");
	printk(KERN_INFO "tda9875: init\n");

	i2c_attach_client(client);
	return 0;
}

static int tda9875_probe(struct i2c_adapter *adap)
{
	if (adap->id == (I2C_ALGO_BIT | I2C_HW_B_BT848))
		return i2c_probe(adap, &addr_data, tda9875_attach);
	return 0;
}

static int tda9875_detach(struct i2c_client *client)
{
	struct tda9875 *t  = client->data;

	do_tda9875_init(client);
	i2c_detach_client(client);
	
	kfree(t);
	MOD_DEC_USE_COUNT;
	return 0;
}

static int tda9875_command(struct i2c_client *client,
				unsigned int cmd, void *arg)
{
	struct tda9875 *t = client->data;

	dprintk("In tda9875_command...\n"); 

	switch (cmd) {	
        /* --- v4l ioctls --- */
	/* take care: bttv does userspace copying, we'll get a
	   kernel pointer here... */
	case VIDIOCGAUDIO:
	{
		struct video_audio *va = arg;
		int left,right;

		dprintk("VIDIOCGAUDIO\n");

		va->flags |= VIDEO_AUDIO_VOLUME |
			VIDEO_AUDIO_BASS |
			VIDEO_AUDIO_TREBLE;

		/* min is -84 max is 24 */
		left = (t->lvol+84)*606;
		right = (t->rvol+84)*606;
		va->volume=MAX(left,right);
		va->balance=(32768*MIN(left,right))/
			(va->volume ? va->volume : 1);
		va->balance=(left<right)?
			(65535-va->balance) : va->balance;
		va->bass = (t->bass+12)*2427;    /* min -12 max +15 */
		va->treble = (t->treble+12)*2730;/* min -12 max +12 */

		va->mode |= VIDEO_SOUND_MONO;


		break; /* VIDIOCGAUDIO case */
	}

	case VIDIOCSAUDIO:
	{
		struct video_audio *va = arg;
		int left,right;

		dprintk("VIDEOCSAUDIO...\n"); 
		left = (MIN(65536 - va->balance,32768) *
			va->volume) / 32768;
		right = (MIN(va->balance,32768) *
			 va->volume) / 32768;
		t->lvol = ((left/606)-84) & 0xff;
		if (t->lvol > 24)
		 t->lvol = 24;
		if (t->lvol < -84)
		 t->lvol = -84 & 0xff;

		t->rvol = ((right/606)-84) & 0xff;
		if (t->rvol > 24)
		 t->rvol = 24;
		if (t->rvol < -84)
		 t->rvol = -84 & 0xff;

		t->bass = ((va->bass/2400)-12) & 0xff;
		if (t->bass > 15)
		 t->bass = 15;
		if (t->bass < -12)
		 t->bass = -12 & 0xff;
		
		t->treble = ((va->treble/2700)-12) & 0xff;
		if (t->treble > 12)
		 t->treble = 12;
		if (t->treble < -12)
		 t->treble = -12 & 0xff;



//printk("tda9875 bal:%04x vol:%04x bass:%04x treble:%04x\n",va->balance,va->volume,va->bass,va->treble);
		

                tda9875_set(client);

		break;

	} /* end of VIDEOCSAUDIO case */

	default: /* Not VIDEOCGAUDIO or VIDEOCSAUDIO */

		/* nothing */
		dprintk("Default\n");

	} /* end of (cmd) switch */

	return 0;
}


static struct i2c_driver driver = {
        "i2c tda9875 driver",
        I2C_DRIVERID_TDA9875, /* Get new one for TDA9875 */
        I2C_DF_NOTIFY,
	tda9875_probe,
        tda9875_detach,
        tda9875_command,
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
int tda9875_init(void)
#endif
{
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

