/*
 * experimental driver for simple i2c audio chips.
 *
 * Copyright (c) 2000 Gerd Knorr
 * based on code by:
 *   Eric Sandeen (eric_sandeen@bigfoot.com) 
 *   Steve VanDeBogart (vandebo@uclink.berkeley.edu)
 *   Greg Alexander (galexand@acm.org)
 *
 * This code is placed under the terms of the GNU General Public License
 * 
 * OPTIONS:
 *   debug - set to 1 if you'd like to see debug messages
 *
 */

#include <linux/config.h>
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
#include <linux/init.h>
#include <linux/smp_lock.h>

#include "audiochip.h"
#include "tvaudio.h"
#include "id.h"


/* ---------------------------------------------------------------------- */
/* insmod args                                                            */

MODULE_PARM(debug,"i");
static int debug = 0;	/* insmod parameter */

#define dprintk  if (debug) printk


/* ---------------------------------------------------------------------- */
/* our structs                                                            */

#define MAXREGS 64

struct CHIPSTATE;
typedef int  (*getvalue)(int);
typedef int  (*checkit)(struct CHIPSTATE*);
typedef int  (*getmode)(struct CHIPSTATE*);
typedef void (*setmode)(struct CHIPSTATE*, int mode);
typedef void (*checkmode)(struct CHIPSTATE*);

/* i2c command */
typedef struct AUDIOCMD {
	int             count;             /* # of bytes to send */
	unsigned char   bytes[MAXREGS+1];  /* addr, data, data, ... */
} audiocmd;

/* chip description */
struct CHIPDESC {
	char       *name;             /* chip name         */
	int        id;                /* ID */
	int        addr_lo, addr_hi;  /* i2c address range */
	int        registers;         /* # of registers    */

	int        *insmodopt;
	checkit    checkit;
	int        flags;
#define CHIP_HAS_VOLUME      1
#define CHIP_HAS_BASSTREBLE  2
#define CHIP_HAS_INPUTSEL    4

	/* various i2c command sequences */
	audiocmd   init;

	/* which register has which value */
	int    leftreg,rightreg,treblereg,bassreg;

	/* initialize with (defaults to 65535/65535/32768/32768 */
	int    leftinit,rightinit,trebleinit,bassinit;

	/* functions to convert the values (v4l -> chip) */
	getvalue volfunc,treblefunc,bassfunc;

	/* get/set mode */
	getmode  getmode;
	setmode  setmode;

	/* check / autoswitch audio after channel switches */
	checkmode  checkmode;

	/* input switch register + values for v4l inputs */
	int  inputreg;
	int  inputmap[8];
	int  inputmute;
};
static struct CHIPDESC chiplist[];

/* current state of the chip */
struct CHIPSTATE {
	struct i2c_client c;

	/* index into CHIPDESC array */
	int type;

	/* shadow register set */
	audiocmd   shadow;

	/* current settings */
	__u16 left,right,treble,bass;

	/* thread */
	struct task_struct  *thread;
	struct semaphore    *notify;
	wait_queue_head_t    wq;
	int                  wake,done;
};


/* ---------------------------------------------------------------------- */
/* i2c adresses                                                           */

static unsigned short normal_i2c[] = {
	I2C_TDA8425   >> 1,
	I2C_TEA6300   >> 1,
	I2C_TEA6420   >> 1,
	I2C_TDA9840   >> 1,
	I2C_TDA985x_L >> 1,
	I2C_TDA985x_H >> 1,
	I2C_PIC16C54  >> 1,
	I2C_CLIENT_END };
static unsigned short normal_i2c_range[2] = { I2C_CLIENT_END, I2C_CLIENT_END };
static unsigned short probe[2]            = { I2C_CLIENT_END, I2C_CLIENT_END };
static unsigned short probe_range[2]      = { I2C_CLIENT_END, I2C_CLIENT_END };
static unsigned short ignore[2]           = { I2C_CLIENT_END, I2C_CLIENT_END };
static unsigned short ignore_range[2]     = { I2C_CLIENT_END, I2C_CLIENT_END };
static unsigned short force[2]            = { I2C_CLIENT_END, I2C_CLIENT_END };
static struct i2c_client_address_data addr_data = {
	normal_i2c, normal_i2c_range, 
	probe, probe_range, 
	ignore, ignore_range, 
	force
};

static struct i2c_driver driver;
static struct i2c_client client_template;


/* ---------------------------------------------------------------------- */
/* i2c I/O functions                                                      */

static int chip_write(struct CHIPSTATE *chip, int subaddr, int val)
{
	unsigned char buffer[2];

	if (-1 == subaddr) {
		dprintk("%s: chip_write: 0x%x\n", chip->c.name, val);
		chip->shadow.bytes[1] = val;
		buffer[0] = val;
		if (1 != i2c_master_send(&chip->c,buffer,1)) {
			printk(KERN_WARNING "%s: I/O error (write 0x%x)\n",
			       chip->c.name, val);
			return -1;
		}
	} else {
		dprintk("%s: chip_write: reg%d=0x%x\n", chip->c.name, subaddr, val);
		chip->shadow.bytes[subaddr+1] = val;
		buffer[0] = subaddr;
		buffer[1] = val;
		if (2 != i2c_master_send(&chip->c,buffer,2)) {
			printk(KERN_WARNING "%s: I/O error (write reg%d=0x%x)\n",
			       chip->c.name, subaddr, val);
			return -1;
		}
	}
	return 0;
}

static int chip_read(struct CHIPSTATE *chip)
{
	unsigned char buffer;

	if (1 != i2c_master_recv(&chip->c,&buffer,1)) {
		printk(KERN_WARNING "%s: I/O error (read)\n",
		       chip->c.name);
		return -1;
	}
	dprintk("%s: chip_read: 0x%x\n",chip->c.name,buffer); 
	return buffer;
}

static int chip_read2(struct CHIPSTATE *chip, int subaddr)
{
        unsigned char write[1];
        unsigned char read[1];
        struct i2c_msg msgs[2] = {
                { chip->c.addr, 0,        1, write },
                { chip->c.addr, I2C_M_RD, 1, read  }
        };
        write[1] = subaddr;

	if (2 != i2c_transfer(chip->c.adapter,msgs,2)) {
		printk(KERN_WARNING "%s: I/O error (read2)\n",
		       chip->c.name);
		return -1;
	}
	dprintk("%s: chip_read2: reg%d=0x%x\n",
		chip->c.name,subaddr,read[0]); 
	return read[0];
}

static int chip_cmd(struct CHIPSTATE *chip, char *name, audiocmd *cmd)
{
	int i;
	
	if (0 == cmd->count)
		return 0;

	/* update our shadow register set; print bytes if (debug > 0) */
	dprintk("%s: chip_cmd(%s): reg=%d, data:",
		chip->c.name,name,cmd->bytes[0]);
	for (i = 1; i < cmd->count; i++) {
		dprintk(" 0x%x",cmd->bytes[i]);
		chip->shadow.bytes[i+cmd->bytes[0]] = cmd->bytes[i];
	}
	dprintk("\n");

	/* send data to the chip */
	if (cmd->count != i2c_master_send(&chip->c,cmd->bytes,cmd->count)) {
		printk(KERN_WARNING "%s: I/O error (%s)\n", chip->c.name, name);
		return -1;
	}
	return 0;
}

/* ---------------------------------------------------------------------- */
/* kernel thread for doing i2c stuff asyncronly
 *   right now it is used only to check the audio mode (mono/stereo/whatever)
 *   some time after switching to another TV channel, then turn on stereo
 *   if available, ...
 */

static int chip_thread(void *data)
{
        struct CHIPSTATE *chip = data;
	struct CHIPDESC  *desc = chiplist + chip->type;
	
#ifdef CONFIG_SMP
	lock_kernel();
#endif
	daemonize();
	sigfillset(&current->blocked);
	strcpy(current->comm,chip->c.name);
	chip->thread = current;
#ifdef CONFIG_SMP
	unlock_kernel();
#endif

	dprintk("%s: thread started\n", chip->c.name);
	if(chip->notify != NULL)
		up(chip->notify);

	for (;;) {
		if (chip->done)
			break;
		if (!chip->wake) {
			interruptible_sleep_on(&chip->wq);
			dprintk("%s: thread wakeup\n", chip->c.name);
			if (chip->done || signal_pending(current))
				break;
		}
		chip->wake = 0;

		/* wait some time -- let the audio hardware
		   figure the current mode */
		current->state   = TASK_INTERRUPTIBLE;
		schedule_timeout(HZ);
		if (signal_pending(current))
			break;
		if (chip->wake)
			continue;

		dprintk("%s: thread checkmode\n", chip->c.name);
		desc->checkmode(chip);
	}

	chip->thread = NULL;
	dprintk("%s: thread exiting\n", chip->c.name);
	if(chip->notify != NULL)
		up_and_exit(chip->notify,0);

	return 0;
}

/* ---------------------------------------------------------------------- */
/* audio chip descriptions - defines+functions for tda9840                */

#define TDA9840_SW         0x00
#define TDA9840_LVADJ      0x02
#define TDA9840_STADJ      0x03
#define TDA9840_TEST       0x04

#define TDA9840_MONO       0x10
#define TDA9840_STEREO     0x2a
#define TDA9840_DUALA      0x12
#define TDA9840_DUALB      0x1e
#define TDA9840_DUALAB     0x1a
#define TDA9840_DUALBA     0x16
#define TDA9840_EXTERNAL   0x7a

int  tda9840_getmode(struct CHIPSTATE *chip)
{
	return VIDEO_SOUND_MONO | VIDEO_SOUND_STEREO |
		VIDEO_SOUND_LANG1 | VIDEO_SOUND_LANG2;
}

void tda9840_setmode(struct CHIPSTATE *chip, int mode)
{
	int update = 1;
	int t = chip->shadow.bytes[TDA9840_SW + 1] & ~0x7e;
	
	switch (mode) {
	case VIDEO_SOUND_MONO:
		t |= TDA9840_MONO;
		break;
	case VIDEO_SOUND_STEREO:
		t |= TDA9840_STEREO;
		break;
	case VIDEO_SOUND_LANG1:
		t |= TDA9840_DUALA;
		break;
	case VIDEO_SOUND_LANG2:
		t |= TDA9840_DUALB;
		break;
	default:
		update = 0;
	}

	if (update)
		chip_write(chip, TDA9840_SW, t);
}

/* ---------------------------------------------------------------------- */
/* audio chip descriptions - defines+functions for tda985x                */

/* subaddresses for TDA9855 */
#define TDA9855_VR	0x00 /* Volume, right */
#define TDA9855_VL	0x01 /* Volume, left */
#define TDA9855_BA	0x02 /* Bass */
#define TDA9855_TR	0x03 /* Treble */
#define TDA9855_SW	0x04 /* Subwoofer - not connected on DTV2000 */

/* subaddresses for TDA9850 */
#define TDA9850_C4	0x04 /* Control 1 for TDA9850 */

/* subaddesses for both chips */
#define TDA985x_C5	0x05 /* Control 2 for TDA9850, Control 1 for TDA9855 */
#define TDA985x_C6	0x06 /* Control 3 for TDA9850, Control 2 for TDA9855 */
#define TDA985x_C7	0x07 /* Control 4 for TDA9850, Control 3 for TDA9855 */
#define TDA985x_A1	0x08 /* Alignment 1 for both chips */
#define TDA985x_A2	0x09 /* Alignment 2 for both chips */
#define TDA985x_A3	0x0a /* Alignment 3 for both chips */

/* Masks for bits in TDA9855 subaddresses */
/* 0x00 - VR in TDA9855 */
/* 0x01 - VL in TDA9855 */
/* lower 7 bits control gain from -71dB (0x28) to 16dB (0x7f)
 * in 1dB steps - mute is 0x27 */


/* 0x02 - BA in TDA9855 */ 
/* lower 5 bits control bass gain from -12dB (0x06) to 16.5dB (0x19)
 * in .5dB steps - 0 is 0x0E */


/* 0x03 - TR in TDA9855 */
/* 4 bits << 1 control treble gain from -12dB (0x3) to 12dB (0xb)
 * in 3dB steps - 0 is 0x7 */

/* Masks for bits in both chips' subaddresses */
/* 0x04 - SW in TDA9855, C4/Control 1 in TDA9850 */
/* Unique to TDA9855: */
/* 4 bits << 2 control subwoofer/surround gain from -14db (0x1) to 14db (0xf)
 * in 3dB steps - mute is 0x0 */
 
/* Unique to TDA9850: */
/* lower 4 bits control stereo noise threshold, over which stereo turns off
 * set to values of 0x00 through 0x0f for Ster1 through Ster16 */


/* 0x05 - C5 - Control 1 in TDA9855 , Control 2 in TDA9850*/
/* Unique to TDA9855: */
#define TDA9855_MUTE	1<<7 /* GMU, Mute at outputs */
#define TDA9855_AVL	1<<6 /* AVL, Automatic Volume Level */
#define TDA9855_LOUD	1<<5 /* Loudness, 1==off */
#define TDA9855_SUR	1<<3 /* Surround / Subwoofer 1==.5(L-R) 0==.5(L+R) */
			     /* Bits 0 to 3 select various combinations
                              * of line in and line out, only the 
                              * interesting ones are defined */
#define TDA9855_EXT	1<<2 /* Selects inputs LIR and LIL.  Pins 41 & 12 */
#define TDA9855_INT	0    /* Selects inputs LOR and LOL.  (internal) */

/* Unique to TDA9850:  */
/* lower 4 bits contol SAP noise threshold, over which SAP turns off
 * set to values of 0x00 through 0x0f for SAP1 through SAP16 */


/* 0x06 - C6 - Control 2 in TDA9855, Control 3 in TDA9850 */
/* Common to TDA9855 and TDA9850: */
#define TDA985x_SAP	3<<6 /* Selects SAP output, mute if not received */
#define TDA985x_STEREO	1<<6 /* Selects Stereo ouput, mono if not received */
#define TDA985x_MONO	0    /* Forces Mono output */
#define TDA985x_LMU	1<<3 /* Mute (LOR/LOL for 9855, OUTL/OUTR for 9850) */

/* Unique to TDA9855: */
#define TDA9855_TZCM	1<<5 /* If set, don't mute till zero crossing */
#define TDA9855_VZCM	1<<4 /* If set, don't change volume till zero crossing*/
#define TDA9855_LINEAR	0    /* Linear Stereo */
#define TDA9855_PSEUDO	1    /* Pseudo Stereo */
#define TDA9855_SPAT_30	2    /* Spatial Stereo, 30% anti-phase crosstalk */
#define TDA9855_SPAT_50	3    /* Spatial Stereo, 52% anti-phase crosstalk */
#define TDA9855_E_MONO	7    /* Forced mono - mono select elseware, so useless*/

/* 0x07 - C7 - Control 3 in TDA9855, Control 4 in TDA9850 */
/* Common to both TDA9855 and TDA9850: */
/* lower 4 bits control input gain from -3.5dB (0x0) to 4dB (0xF)
 * in .5dB steps -  0dB is 0x7 */

/* 0x08, 0x09 - A1 and A2 (read/write) */
/* Common to both TDA9855 and TDA9850: */
/* lower 5 bites are wideband and spectral expander alignment
 * from 0x00 to 0x1f - nominal at 0x0f and 0x10 (read/write) */
#define TDA985x_STP	1<<5 /* Stereo Pilot/detect (read-only) */
#define TDA985x_SAPP	1<<6 /* SAP Pilot/detect (read-only) */
#define TDA985x_STS	1<<7 /* Stereo trigger 1= <35mV 0= <30mV (write-only)*/

/* 0x0a - A3 */
/* Common to both TDA9855 and TDA9850: */
/* lower 3 bits control timing current for alignment: -30% (0x0), -20% (0x1),
 * -10% (0x2), nominal (0x3), +10% (0x6), +20% (0x5), +30% (0x4) */
#define TDA985x_ADJ	1<<7 /* Stereo adjust on/off (wideband and spectral */

int tda9855_volume(int val) { return val/0x2e8+0x27; }
int tda9855_bass(int val)   { return val/0xccc+0x06; }
int tda9855_treble(int val) { return (val/0x1c71+0x3)<<1; }

int  tda985x_getmode(struct CHIPSTATE *chip)
{
	int mode;

	mode = ((TDA985x_STP | TDA985x_SAPP) & 
		chip_read(chip)) >> 4;
	/* Add mono mode regardless of SAP and stereo */
	/* Allows forced mono */
	return mode | VIDEO_SOUND_MONO;
}

void tda985x_setmode(struct CHIPSTATE *chip, int mode)
{
	int update = 1;
	int c6 = chip->shadow.bytes[TDA985x_C6+1] & 0x3f;
	
	switch (mode) {
	case VIDEO_SOUND_MONO:
		c6 |= TDA985x_MONO;
		break;
	case VIDEO_SOUND_STEREO:
		c6 |= TDA985x_STEREO;
		break;
	case VIDEO_SOUND_LANG1:
		c6 |= TDA985x_SAP;
		break;
	default:
		update = 0;
	}
	if (update)
		chip_write(chip,TDA985x_C6,c6);
}


/* ---------------------------------------------------------------------- */
/* audio chip descriptions - defines+functions for tda9873h               */

/* Subaddresses for TDA9873H */

#define TDA9873_SW	0x00 /* Switching                    */
#define TDA9873_AD	0x01 /* Adjust                       */
#define TDA9873_PT	0x02 /* Port                         */

/* Subaddress 0x00: Switching Data 
 * B7..B0:
 *
 * B1, B0: Input source selection
 *  0,  0  internal
 *  1,  0  external stereo
 *  0,  1  external mono
 */
#define TDA9873_INTERNAL    0
#define TDA9873_EXT_STEREO  2
#define TDA9873_EXT_MONO    1

/*    B3, B2: output signal select
 * B4    : transmission mode 
 *  0, 0, 1   Mono
 *  1, 0, 0   Stereo
 *  1, 1, 1   Stereo (reversed channel)    
 *  0, 0, 0   Dual AB
 *  0, 0, 1   Dual AA
 *  0, 1, 0   Dual BB
 *  0, 1, 1   Dual BA
 */

#define TDA9873_TR_MONO     4
#define TDA9873_TR_STEREO   1 << 4
#define TDA9873_TR_REVERSE  (1 << 3) & (1 << 2)
#define TDA9873_TR_DUALA    1 << 2
#define TDA9873_TR_DUALB    1 << 3

/* output level controls
 * B5:  output level switch (0 = reduced gain, 1 = normal gain)
 * B6:  mute                (1 = muted)
 * B7:  auto-mute           (1 = auto-mute enabled)
 */

#define TDA9873_GAIN_NORMAL 1 << 5
#define TDA9873_MUTE        1 << 6
#define TDA9873_AUTOMUTE    1 << 7

/* Subaddress 0x01:  Adjust/standard */

/* Lower 4 bits (C3..C0) control stereo adjustment on R channel (-0.6 - +0.7 dB)
 * Recommended value is +0 dB
 */

#define	TDA9873_STEREO_ADJ	0x06 /* 0dB gain */

/* Bits C6..C4 control FM stantard  
 * C6, C5, C4
 *  0,  0,  0   B/G (PAL FM)
 *  0,  0,  1   M
 *  0,  1,  0   D/K(1)
 *  0,  1,  1   D/K(2)
 *  1,  0,  0   D/K(3)
 *  1,  0,  1   I
 */
#define TDA9873_BG		0
#define TDA9873_M       1
#define TDA9873_DK1     2
#define TDA9873_DK2     3
#define TDA9873_DK3     4
#define TDA9873_I       5

/* C7 controls identification response time (1=fast/0=normal)
 */
#define TDA9873_IDR_NORM 0
#define TDA9873_IDR_FAST 1 << 7


/* Subaddress 0x02: Port data */ 

/* E1, E0   free programmable ports P1/P2
    0,  0   both ports low
    0,  1   P1 high
    1,  0   P2 high
    1,  1   both ports high
*/

#define TDA9873_PORTS    3

/* E2: test port */
#define TDA9873_TST_PORT 1 << 2

/* E5..E3 control mono output channel (together with transmission mode bit B4)
 *
 * E5 E4 E3 B4     OUTM
 *  0  0  0  0     mono
 *  0  0  1  0     DUAL B
 *  0  1  0  1     mono (from stereo decoder)
 */
#define TDA9873_MOUT_MONO   0
#define TDA9873_MOUT_FMONO  0
#define TDA9873_MOUT_DUALA  0 
#define TDA9873_MOUT_DUALB  1 << 3 
#define TDA9873_MOUT_ST     1 << 4 
#define TDA9873_MOUT_EXTM   (1 << 4 ) & (1 << 3)
#define TDA9873_MOUT_EXTL   1 << 5 
#define TDA9873_MOUT_EXTR   (1 << 5 ) & (1 << 3)
#define TDA9873_MOUT_EXTLR  (1 << 5 ) & (1 << 4)
#define TDA9873_MOUT_MUTE   (1 << 5 ) & (1 << 4) & (1 << 3)

/* Status bits: (chip read) */
#define TDA9873_PONR        0 /* Power-on reset detected if = 1 */
#define TDA9873_STEREO      2 /* Stereo sound is identified     */
#define TDA9873_DUAL        4 /* Dual sound is identified       */

int tda9873_getmode(struct CHIPSTATE *chip)
{
	int val,mode;

	val = chip_read(chip);
	mode = VIDEO_SOUND_MONO;
	if (val & TDA9873_STEREO)
		mode |= VIDEO_SOUND_STEREO;
	if (val & TDA9873_DUAL)
		mode |= VIDEO_SOUND_LANG1 | VIDEO_SOUND_LANG2;
	dprintk ("tda9873_getmode(): raw chip read: %d, return: %d\n",
		 val, mode);
	return mode;
}

void tda9873_setmode(struct CHIPSTATE *chip, int mode)
{
	int sw_data  = chip->shadow.bytes[TDA9873_SW+1] & 0xe3;
	/*	int adj_data = chip->shadow.bytes[TDA9873_AD+1] ; */
	dprintk("tda9873_setmode(): chip->shadow.bytes[%d] = %d\n", TDA9873_SW+1, chip->shadow.bytes[TDA9873_SW+1]);
	dprintk("tda9873_setmode(): sw_data  = %d\n", sw_data);

	switch (mode) {
	case VIDEO_SOUND_MONO:
		sw_data |= TDA9873_TR_MONO;   
		break;
	case VIDEO_SOUND_STEREO:
		sw_data |= TDA9873_TR_STEREO;
		break;
	case VIDEO_SOUND_LANG1:
		sw_data |= TDA9873_TR_DUALA;
		break;
	case VIDEO_SOUND_LANG2:
		sw_data |= TDA9873_TR_DUALB;
		break;
	}
	dprintk("tda9873_setmode(): req. mode %d; chip_write: %d\n", mode, sw_data);
	chip_write(chip,TDA9873_SW,sw_data);
}

void tda9873_checkmode(struct CHIPSTATE *chip)
{
	int mode = tda9873_getmode(chip);

	if (mode & VIDEO_SOUND_STEREO)
		tda9873_setmode(chip,VIDEO_SOUND_STEREO);
	if (mode & VIDEO_SOUND_LANG1)
		tda9873_setmode(chip,VIDEO_SOUND_LANG1);
}

int tda9873_checkit(struct CHIPSTATE *chip)
{
	int rc;

	if (-1 == (rc = chip_read2(chip,254)))
		return 0;
	return (rc & ~0x1f) == 0x80;
}


/* ---------------------------------------------------------------------- */
/* audio chip descriptions - defines+functions for tea6420                */

#define TEA6300_VL         0x00  /* volume left */
#define TEA6300_VR         0x01  /* volume right */
#define TEA6300_BA         0x02  /* bass */
#define TEA6300_TR         0x03  /* treble */
#define TEA6300_FA         0x04  /* fader control */
#define TEA6300_S          0x05  /* switch register */
                                 /* values for those registers: */
#define TEA6300_S_SA       0x01  /* stereo A input */
#define TEA6300_S_SB       0x02  /* stereo B */
#define TEA6300_S_SC       0x04  /* stereo C */
#define TEA6300_S_GMU      0x80  /* general mute */

#define TEA6420_S_SA       0x00  /* stereo A input */
#define TEA6420_S_SB       0x01  /* stereo B */
#define TEA6420_S_SC       0x02  /* stereo C */
#define TEA6420_S_SD       0x03  /* stereo D */
#define TEA6420_S_SE       0x04  /* stereo E */
#define TEA6420_S_GMU      0x05  /* general mute */

int tea6300_shift10(int val) { return val >> 10; }
int tea6300_shift12(int val) { return val >> 12; }


/* ---------------------------------------------------------------------- */
/* audio chip descriptions - defines+functions for tda8425                */

#define TDA8425_VL         0x00  /* volume left */
#define TDA8425_VR         0x01  /* volume right */
#define TDA8425_BA         0x02  /* bass */
#define TDA8425_TR         0x03  /* treble */
#define TDA8425_S1         0x08  /* switch functions */
                                 /* values for those registers: */
#define TDA8425_S1_OFF     0xEE  /* audio off (mute on) */
#define TDA8425_S1_ON      0xCE  /* audio on (mute off) - "linear stereo" mode */

int tda8425_shift10(int val) { return val >> 10 | 0xc0; }
int tda8425_shift12(int val) { return val >> 12 | 0xf0; }


/* ---------------------------------------------------------------------- */
/* audio chip descriptions - defines+functions for pic16c54 (PV951)       */

/* the registers of 16C54, I2C sub address. */
#define PIC16C54_REG_KEY_CODE     0x01	       /* Not use. */
#define PIC16C54_REG_MISC         0x02

/* bit definition of the RESET register, I2C data. */
#define PIC16C54_MISC_RESET_REMOTE_CTL 0x01 /* bit 0, Reset to receive the key */
                                            /*        code of remote controller */
#define PIC16C54_MISC_MTS_MAIN         0x02 /* bit 1 */
#define PIC16C54_MISC_MTS_SAP          0x04 /* bit 2 */
#define PIC16C54_MISC_MTS_BOTH         0x08 /* bit 3 */
#define PIC16C54_MISC_SND_MUTE         0x10 /* bit 4, Mute Audio(Line-in and Tuner) */
#define PIC16C54_MISC_SND_NOTMUTE      0x20 /* bit 5 */
#define PIC16C54_MISC_SWITCH_TUNER     0x40 /* bit 6	, Switch to Line-in */
#define PIC16C54_MISC_SWITCH_LINE      0x80 /* bit 7	, Switch to Tuner */


/* ---------------------------------------------------------------------- */
/* audio chip descriptions - struct CHIPDESC                              */

/* insmod options to enable/disable individual audio chips */
int tda8425  = 1;
int tda9840  = 1;
int tda9850  = 1;
int tda9855  = 1;
int tda9873  = 1;
int tea6300  = 0;
int tea6420  = 1;
int pic16c54 = 1;
MODULE_PARM(tda8425,"i");
MODULE_PARM(tda9840,"i");
MODULE_PARM(tda9850,"i");
MODULE_PARM(tda9855,"i");
MODULE_PARM(tda9873,"i");
MODULE_PARM(tea6300,"i");
MODULE_PARM(tea6420,"i");
MODULE_PARM(pic16c54,"i");

static struct CHIPDESC chiplist[] = {
	{
		name:       "tda9840",
		id:         I2C_DRIVERID_TDA9840,
		insmodopt:  &tda9840,
		addr_lo:    I2C_TDA9840 >> 1,
		addr_hi:    I2C_TDA9840 >> 1,
		registers:  5,

		getmode:    tda9840_getmode,
		setmode:    tda9840_setmode,

		init:       { 2, { TDA9840_SW, 0x2a } }
	},
	{
		name:       "tda9873h",
		id:         I2C_DRIVERID_TDA9873,
		checkit:    tda9873_checkit,
		insmodopt:  &tda9873,
		addr_lo:    I2C_TDA985x_L >> 1,
		addr_hi:    I2C_TDA985x_H >> 1,
		registers:  3,

		getmode:    tda9873_getmode,
		setmode:    tda9873_setmode,
		checkmode:  tda9873_checkmode,

		init:       { 4, { TDA9873_SW, 0xa0, 0x06, 0x03 } }
	},
	{
		name:       "tda9850",
		id:         I2C_DRIVERID_TDA9850,
		insmodopt:  &tda9850,
		addr_lo:    I2C_TDA985x_L >> 1,
		addr_hi:    I2C_TDA985x_H >> 1,
		registers:  11,

		getmode:    tda985x_getmode,
		setmode:    tda985x_setmode,

		init:       { 8, { TDA9850_C4, 0x08, 0x08, TDA985x_STEREO, 0x07, 0x10, 0x10, 0x03 } }
	},
	{
		name:       "tda9855",
		id:         I2C_DRIVERID_TDA9855,
		insmodopt:  &tda9855,
		addr_lo:    I2C_TDA985x_L >> 1,
		addr_hi:    I2C_TDA985x_H >> 1,
		registers:  11,
		flags:      CHIP_HAS_VOLUME | CHIP_HAS_BASSTREBLE,

		leftreg:    TDA9855_VR,
		rightreg:   TDA9855_VL,
		bassreg:    TDA9855_BA,
		treblereg:  TDA9855_TR,
		volfunc:    tda9855_volume,
		bassfunc:   tda9855_bass,
		treblefunc: tda9855_treble,

		getmode:    tda985x_getmode,
		setmode:    tda985x_setmode,

		init:       { 12, { 0, 0x6f, 0x6f, 0x0e, 0x07<<1, 0x8<<2,
				    TDA9855_MUTE | TDA9855_AVL | TDA9855_LOUD | TDA9855_INT,
				    TDA985x_STEREO | TDA9855_LINEAR | TDA9855_TZCM | TDA9855_VZCM,
				    0x07, 0x10, 0x10, 0x03 }}
	},
	{
		name:       "tea6300",
		id:         I2C_DRIVERID_TEA6300,
		insmodopt:  &tea6300,
		addr_lo:    I2C_TEA6300 >> 1,
		addr_hi:    I2C_TEA6300 >> 1,
		registers:  6,
		flags:      CHIP_HAS_VOLUME | CHIP_HAS_BASSTREBLE | CHIP_HAS_INPUTSEL,

		leftreg:    TEA6300_VR,
		rightreg:   TEA6300_VL,
		bassreg:    TEA6300_BA,
		treblereg:  TEA6300_TR,
		volfunc:    tea6300_shift10,
		bassfunc:   tea6300_shift12,
		treblefunc: tea6300_shift12,

		inputreg:   TEA6300_S,
		inputmap:   { TEA6300_S_SA, TEA6300_S_SB, TEA6300_S_SC },
		inputmute:  TEA6300_S_GMU,
	},
	{
		name:       "tea6420",
		id:         I2C_DRIVERID_TEA6420,
		insmodopt:  &tea6420,
		addr_lo:    I2C_TEA6420 >> 1,
		addr_hi:    I2C_TEA6420 >> 1,
		registers:  1,
		flags:      CHIP_HAS_INPUTSEL,

		inputreg:   -1,
		inputmap:   { TEA6420_S_SA, TEA6420_S_SB, TEA6420_S_SC },
		inputmute:  TEA6300_S_GMU,
	},
	{
		name:       "tda8425",
		id:         I2C_DRIVERID_TDA8425,
		insmodopt:  &tda8425,
		addr_lo:    I2C_TDA8425 >> 1,
		addr_hi:    I2C_TDA8425 >> 1,
		registers:  9,
		flags:      CHIP_HAS_VOLUME | CHIP_HAS_BASSTREBLE | CHIP_HAS_INPUTSEL,

		leftreg:    TDA8425_VR,
		rightreg:   TDA8425_VL,
		bassreg:    TDA8425_BA,
		treblereg:  TDA8425_TR,
		volfunc:    tda8425_shift10,
		bassfunc:   tda8425_shift12,
		treblefunc: tda8425_shift12,

		inputreg:   TDA8425_S1,
		inputmap:   { TDA8425_S1_ON, TDA8425_S1_ON, TDA8425_S1_ON },
		inputmute:  TDA8425_S1_OFF,
	},
	{
		name:       "pic16c54 (PV951)",
		id:         I2C_DRIVERID_PIC16C54_PV951,
		insmodopt:  &pic16c54,
		addr_lo:    I2C_PIC16C54 >> 1,
		addr_hi:    I2C_PIC16C54>> 1,
		registers:  2,
		flags:      CHIP_HAS_INPUTSEL,

		inputreg:   PIC16C54_REG_MISC,
		inputmap:   {PIC16C54_MISC_SND_NOTMUTE|PIC16C54_MISC_SWITCH_TUNER,
			     PIC16C54_MISC_SND_NOTMUTE|PIC16C54_MISC_SWITCH_LINE},
		inputmute:  PIC16C54_MISC_SND_MUTE,
	},
	{ name: NULL } /* EOF */
};


/* ---------------------------------------------------------------------- */
/* i2c registration                                                       */

static int chip_attach(struct i2c_adapter *adap, int addr,
		       unsigned short flags, int kind)
{
	struct CHIPSTATE *chip;
	struct CHIPDESC  *desc;

	chip = kmalloc(sizeof(*chip),GFP_KERNEL);
	if (!chip)
		return -ENOMEM;
	memset(chip,0,sizeof(*chip));
	memcpy(&chip->c,&client_template,sizeof(struct i2c_client));
        chip->c.adapter = adap;
        chip->c.addr = addr;
	chip->c.data = chip;

	/* find description for the chip */
	dprintk("tvaudio: chip @ addr=0x%x\n",addr<<1);
	for (desc = chiplist; desc->name != NULL; desc++) {
		if (0 == *(desc->insmodopt))
			continue;
		if (addr < desc->addr_lo ||
		    addr > desc->addr_hi)
			continue;
		if (desc->checkit && !desc->checkit(chip))
			continue;
		break;
	}
	if (desc->name == NULL) {
		dprintk("tvaudio: no matching chip description found\n");
		return -EIO;
	}
	dprintk("tvaudio: %s matches:%s%s%s\n",desc->name,
		(desc->flags & CHIP_HAS_VOLUME)     ? " volume"      : "",
		(desc->flags & CHIP_HAS_BASSTREBLE) ? " bass/treble" : "",
		(desc->flags & CHIP_HAS_INPUTSEL)   ? " audiomux"    : "");

	/* fill required data structures */
	strcpy(chip->c.name,desc->name);
	chip->type = desc-chiplist;
	chip->shadow.count = desc->registers+1;

	/* register */
	MOD_INC_USE_COUNT;
	i2c_attach_client(&chip->c);

	/* initialization  */
	chip_cmd(chip,"init",&desc->init);
	if (desc->flags & CHIP_HAS_VOLUME) {
		chip->left   = desc->leftinit   ? desc->leftinit   : 65536;
		chip->right  = desc->rightinit  ? desc->rightinit  : 65536;
		chip_write(chip,desc->leftreg,desc->volfunc(chip->left));
		chip_write(chip,desc->rightreg,desc->volfunc(chip->right));
	}
	if (desc->flags & CHIP_HAS_BASSTREBLE) {
		chip->treble = desc->trebleinit ? desc->trebleinit : 32768;
		chip->bass   = desc->bassinit   ? desc->bassinit   : 32768;
		chip_write(chip,desc->bassreg,desc->bassfunc(chip->bass));
		chip_write(chip,desc->treblereg,desc->treblefunc(chip->treble));
	}

	if (desc->checkmode) {
		/* start async thread */
		DECLARE_MUTEX_LOCKED(sem);
		chip->notify = &sem;
		init_waitqueue_head(&chip->wq);
		kernel_thread(chip_thread,(void *)chip,0);
		down(&sem);
		chip->notify = NULL;
		chip->wake++;
		wake_up_interruptible(&chip->wq);
	}
	return 0;
}

static int chip_probe(struct i2c_adapter *adap)
{
	if (adap->id == (I2C_ALGO_BIT | I2C_HW_B_BT848))
		return i2c_probe(adap, &addr_data, chip_attach);
	return 0;
}

static int chip_detach(struct i2c_client *client)
{
	struct CHIPSTATE *chip = client->data;

	if (NULL != chip->thread) {
		/* shutdown async thread */
		DECLARE_MUTEX_LOCKED(sem);
		chip->notify = &sem;
		chip->done = 1;
		wake_up_interruptible(&chip->wq);
		down(&sem);
		chip->notify = NULL;
	}
	
	i2c_detach_client(&chip->c);
	kfree(chip);
	MOD_DEC_USE_COUNT;
	return 0;
}

/* ---------------------------------------------------------------------- */
/* video4linux interface                                                  */

static int chip_command(struct i2c_client *client,
			unsigned int cmd, void *arg)
{
        __u16 *sarg = arg;
	struct CHIPSTATE *chip = client->data;
	struct CHIPDESC  *desc = chiplist + chip->type;

	dprintk("%s: chip_command 0x%x\n",chip->c.name,cmd);

	switch (cmd) {
	case AUDC_SET_INPUT:
		if (desc->flags & CHIP_HAS_INPUTSEL) {
			if (*sarg & 0x80)
				chip_write(chip,desc->inputreg,desc->inputmute);
			else
				chip_write(chip,desc->inputreg,desc->inputmap[*sarg]);
		}
		break;
	/* --- v4l ioctls --- */
	/* take care: bttv does userspace copying, we'll get a
	   kernel pointer here... */
	case VIDIOCGAUDIO:
	{
		struct video_audio *va = arg;

		if (desc->flags & CHIP_HAS_VOLUME) {
			va->flags  |= VIDEO_AUDIO_VOLUME;
			va->volume  = MAX(chip->left,chip->right);
			va->balance = (32768*MIN(chip->left,chip->right))/
				(va->volume ? va->volume : 1);
		}
		if (desc->flags & CHIP_HAS_BASSTREBLE) {
			va->flags |= VIDEO_AUDIO_BASS | VIDEO_AUDIO_TREBLE;
			va->bass   = chip->bass;
			va->treble = chip->treble;
		}
		if (desc->getmode)
			va->mode = desc->getmode(chip);
		else
			va->mode = VIDEO_SOUND_MONO;
		break;
	}

	case VIDIOCSAUDIO:
	{
		struct video_audio *va = arg;
		
		if (desc->flags & CHIP_HAS_VOLUME) {
			chip->left = (MIN(65536 - va->balance,32768) *
				      va->volume) / 32768;
			chip->right = (MIN(va->balance,32768) *
				       va->volume) / 32768;
			chip_write(chip,desc->leftreg,desc->volfunc(chip->left));
			chip_write(chip,desc->rightreg,desc->volfunc(chip->right));
		}
		if (desc->flags & CHIP_HAS_BASSTREBLE) {
			chip->bass = va->bass;
			chip->treble = va->treble;
			chip_write(chip,desc->bassreg,desc->bassfunc(chip->bass));
			chip_write(chip,desc->treblereg,desc->treblefunc(chip->treble));
		}
		if (desc->setmode && va->mode)
			desc->setmode(chip,va->mode);
		break;
	}
	case VIDIOCSFREQ:
	{
		if (desc->checkmode) {
			desc->setmode(chip,VIDEO_SOUND_MONO);
			chip->wake++;
			wake_up_interruptible(&chip->wq);
			/* the thread will call checkmode() a second later */
		}
	}
	}
	return 0;
}


static struct i2c_driver driver = {
        name:            "generic i2c audio driver",
        id:              I2C_DRIVERID_TVAUDIO, /* FIXME */
        flags:           I2C_DF_NOTIFY,
        attach_adapter:  chip_probe,
        detach_client:   chip_detach,
        command:         chip_command,
};

static struct i2c_client client_template =
{
        name:   "(unset)",
        driver: &driver,
};

int audiochip_init_module(void)
{
	struct CHIPDESC  *desc;
	printk(KERN_INFO "tvaudio: TV audio decoder + audio/video mux driver\n");
	printk(KERN_INFO "tvaudio: known chips: ");
	for (desc = chiplist; desc->name != NULL; desc++)
		printk("%s%s", (desc == chiplist) ? "" : ",",desc->name);
	printk("\n");
	i2c_add_driver(&driver);
	return 0;
}

void audiochip_cleanup_module(void)
{
	i2c_del_driver(&driver);
}

module_init(audiochip_init_module);
module_exit(audiochip_cleanup_module);

/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
