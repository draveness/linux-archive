/*
 * device driver for philips saa7134 based TV cards
 * tv audio decoder (fm stereo, nicam, ...)
 *
 * (c) 2001-03 Gerd Knorr <kraxel@bytesex.org> [SuSE Labs]
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
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/smp_lock.h>
#include <asm/div64.h>

#include "saa7134-reg.h"
#include "saa7134.h"

/* ------------------------------------------------------------------ */

static unsigned int audio_debug = 0;
MODULE_PARM(audio_debug,"i");
MODULE_PARM_DESC(audio_debug,"enable debug messages [tv audio]");

static unsigned int audio_ddep = 0;
MODULE_PARM(audio_ddep,"i");
MODULE_PARM_DESC(audio_ddep,"audio ddep overwrite");

static int audio_clock_override = UNSET;
MODULE_PARM(audio_clock_override, "i");

static int audio_clock_tweak = 0;
MODULE_PARM(audio_clock_tweak, "i");
MODULE_PARM_DESC(audio_clock_tweak, "Audio clock tick fine tuning for cards with audio crystal that's slightly off (range [-1024 .. 1024])");

#define dprintk(fmt, arg...)	if (audio_debug) \
	printk(KERN_DEBUG "%s/audio: " fmt, dev->name , ## arg)
#define d2printk(fmt, arg...)	if (audio_debug > 1) \
	printk(KERN_DEBUG "%s/audio: " fmt, dev->name, ## arg)

#define print_regb(reg) printk("%s:   reg 0x%03x [%-16s]: 0x%02x\n", \
		dev->name,(SAA7134_##reg),(#reg),saa_readb((SAA7134_##reg)))

#define SCAN_INITIAL_DELAY     (HZ)
#define SCAN_SAMPLE_DELAY      (HZ/5)
#define SCAN_SUBCARRIER_DELAY  (HZ*2)

/* ------------------------------------------------------------------ */
/* saa7134 code                                                       */

static struct mainscan {
	char         *name;
	v4l2_std_id  std;
	int          carr;
} mainscan[] = {
	{
		.name = "M",
		.std  = V4L2_STD_NTSC | V4L2_STD_PAL_M,
		.carr = 4500,
	},{
		.name = "BG",
		.std  = V4L2_STD_PAL_BG,
		.carr = 5500,
	},{
		.name = "I",
		.std  = V4L2_STD_PAL_I,
		.carr = 6000,
	},{
		.name = "DKL",
		.std  = V4L2_STD_PAL_DK | V4L2_STD_SECAM,
		.carr = 6500,
	}
};

static struct saa7134_tvaudio tvaudio[] = {
	{
		.name          = "PAL-B/G FM-stereo",
		.std           = V4L2_STD_PAL,
		.mode          = TVAUDIO_FM_BG_STEREO,
		.carr1         = 5500,
		.carr2         = 5742,
	},{
		.name          = "PAL-D/K1 FM-stereo",
		.std           = V4L2_STD_PAL,
		.carr1         = 6500,
		.carr2         = 6258,
		.mode          = TVAUDIO_FM_BG_STEREO,
	},{
		.name          = "PAL-D/K2 FM-stereo",
		.std           = V4L2_STD_PAL,
		.carr1         = 6500,
		.carr2         = 6742,
		.mode          = TVAUDIO_FM_BG_STEREO,
	},{
		.name          = "PAL-D/K3 FM-stereo",
		.std           = V4L2_STD_PAL,
		.carr1         = 6500,
		.carr2         = 5742,
		.mode          = TVAUDIO_FM_BG_STEREO,
	},{
		.name          = "PAL-B/G NICAM",
		.std           = V4L2_STD_PAL,
		.carr1         = 5500,
		.carr2         = 5850,
		.mode          = TVAUDIO_NICAM_FM,
	},{
		.name          = "PAL-I NICAM",
		.std           = V4L2_STD_PAL,
		.carr1         = 6000,
		.carr2         = 6552,
		.mode          = TVAUDIO_NICAM_FM,
	},{
		.name          = "PAL-D/K NICAM",
		.std           = V4L2_STD_PAL,
		.carr1         = 6500,
		.carr2         = 5850,
		.mode          = TVAUDIO_NICAM_FM,
	},{
		.name          = "SECAM-L NICAM",
		.std           = V4L2_STD_SECAM,
		.carr1         = 6500,
		.carr2         = 5850,
		.mode          = TVAUDIO_NICAM_AM,
	},{
		.name          = "SECAM-D/K",
		.std           = V4L2_STD_SECAM,
		.carr1         = 6500,
		.carr2         = -1,
		.mode          = TVAUDIO_FM_MONO,
	},{
		.name          = "NTSC-M",
		.std           = V4L2_STD_NTSC,
		.carr1         = 4500,
		.carr2         = -1,
		.mode          = TVAUDIO_FM_MONO,
	},{
		.name          = "NTSC-A2 FM-stereo",
		.std           = V4L2_STD_NTSC,
		.carr1         = 4500,
		.carr2         = 4724,
		.mode          = TVAUDIO_FM_K_STEREO,
	}
};
#define TVAUDIO (sizeof(tvaudio)/sizeof(struct saa7134_tvaudio))

/* ------------------------------------------------------------------ */

static void tvaudio_init(struct saa7134_dev *dev)
{
	int clock = saa7134_boards[dev->board].audio_clock;

	if (UNSET != audio_clock_override)
	        clock = audio_clock_override;

	/* init all audio registers */
	saa_writeb(SAA7134_AUDIO_PLL_CTRL,   0x00);
	if (need_resched())
		schedule();
	else
		udelay(10);

	saa_writeb(SAA7134_AUDIO_CLOCK0,      clock        & 0xff);
	saa_writeb(SAA7134_AUDIO_CLOCK1,     (clock >>  8) & 0xff);
	saa_writeb(SAA7134_AUDIO_CLOCK2,     (clock >> 16) & 0xff);
	saa_writeb(SAA7134_AUDIO_PLL_CTRL,   0x01);

	saa_writeb(SAA7134_NICAM_ERROR_LOW,  0x14);
	saa_writeb(SAA7134_NICAM_ERROR_HIGH, 0x50);
	saa_writeb(SAA7134_MONITOR_SELECT,   0xa0);
	saa_writeb(SAA7134_FM_DEMATRIX,      0x80);
}

static u32 tvaudio_carr2reg(u32 carrier)
{
	u64 a = carrier;

	a <<= 24;
	do_div(a,12288);
	return a;
}

static void tvaudio_setcarrier(struct saa7134_dev *dev,
			       int primary, int secondary)
{
	if (-1 == secondary)
		secondary = primary;
	saa_writel(SAA7134_CARRIER1_FREQ0 >> 2, tvaudio_carr2reg(primary));
	saa_writel(SAA7134_CARRIER2_FREQ0 >> 2, tvaudio_carr2reg(secondary));
}

static void mute_input_7134(struct saa7134_dev *dev)
{
	unsigned int mute;
	struct saa7134_input *in;
	int ausel=0, ics=0, ocs=0;
	int mask;

	/* look what is to do ... */
	in   = dev->input;
	mute = (dev->ctl_mute ||
		(dev->automute  &&  (&card(dev).radio) != in));
	if (PCI_DEVICE_ID_PHILIPS_SAA7130 == dev->pci->device &&
	    card(dev).mute.name) {
		/* 7130 - we'll mute using some unconnected audio input */
		if (mute)
			in = &card(dev).mute;
	}
	if (dev->hw_mute  == mute &&
	    dev->hw_input == in) {
		dprintk("mute/input: nothing to do [mute=%d,input=%s]\n",
			mute,in->name);
		return;
	}

	dprintk("ctl_mute=%d automute=%d input=%s  =>  mute=%d input=%s\n",
		dev->ctl_mute,dev->automute,dev->input->name,mute,in->name);
	dev->hw_mute  = mute;
	dev->hw_input = in;

	if (PCI_DEVICE_ID_PHILIPS_SAA7134 == dev->pci->device)
		/* 7134 mute */
		saa_writeb(SAA7134_AUDIO_MUTE_CTRL, mute ? 0xff : 0xbb);

	/* switch internal audio mux */
	switch (in->amux) {
	case TV:    ausel=0xc0; ics=0x00; ocs=0x02; break;
	case LINE1: ausel=0x80; ics=0x00; ocs=0x00; break;
	case LINE2: ausel=0x80; ics=0x08; ocs=0x01; break;
	}
	saa_andorb(SAA7134_AUDIO_FORMAT_CTRL, 0xc0, ausel);
	saa_andorb(SAA7134_ANALOG_IO_SELECT, 0x08, ics);
	saa_andorb(SAA7134_ANALOG_IO_SELECT, 0x07, ocs);

	/* switch gpio-connected external audio mux */
	if (0 == card(dev).gpiomask)
		return;
	mask = card(dev).gpiomask;
	saa_andorl(SAA7134_GPIO_GPMODE0 >> 2,   mask, mask);
	saa_andorl(SAA7134_GPIO_GPSTATUS0 >> 2, mask, in->gpio);
	saa7134_track_gpio(dev,in->name);
}

static void tvaudio_setmode(struct saa7134_dev *dev,
			    struct saa7134_tvaudio *audio,
			    char *note)
{
	int acpf, tweak = 0;

	if (dev->tvnorm->id == V4L2_STD_NTSC) {
		acpf = 0x19066;
	} else {
		acpf = 0x1e000;
	}
	if (audio_clock_tweak > -1024 && audio_clock_tweak < 1024)
		tweak = audio_clock_tweak;

	if (note)
		dprintk("tvaudio_setmode: %s %s [%d.%03d/%d.%03d MHz] acpf=%d%+d\n",
			note,audio->name,
			audio->carr1 / 1000, audio->carr1 % 1000,
			audio->carr2 / 1000, audio->carr2 % 1000,
			acpf, tweak);

	acpf += tweak;
	saa_writeb(SAA7134_AUDIO_CLOCKS_PER_FIELD0, (acpf & 0x0000ff) >> 0);
	saa_writeb(SAA7134_AUDIO_CLOCKS_PER_FIELD1, (acpf & 0x00ff00) >> 8);
	saa_writeb(SAA7134_AUDIO_CLOCKS_PER_FIELD2, (acpf & 0x030000) >> 16);
	tvaudio_setcarrier(dev,audio->carr1,audio->carr2);
	
	switch (audio->mode) {
	case TVAUDIO_FM_MONO:
	case TVAUDIO_FM_BG_STEREO:
		saa_writeb(SAA7134_DEMODULATOR,               0x00);
		saa_writeb(SAA7134_DCXO_IDENT_CTRL,           0x00);
		saa_writeb(SAA7134_FM_DEEMPHASIS,             0x22);
		saa_writeb(SAA7134_FM_DEMATRIX,               0x80);
		saa_writeb(SAA7134_STEREO_DAC_OUTPUT_SELECT,  0xa0);
		break;
	case TVAUDIO_FM_K_STEREO:
		saa_writeb(SAA7134_DEMODULATOR,               0x00);
		saa_writeb(SAA7134_DCXO_IDENT_CTRL,           0x01);
		saa_writeb(SAA7134_FM_DEEMPHASIS,             0x22);
		saa_writeb(SAA7134_FM_DEMATRIX,               0x80);
		saa_writeb(SAA7134_STEREO_DAC_OUTPUT_SELECT,  0xa0);
		break;
	case TVAUDIO_NICAM_FM:
		saa_writeb(SAA7134_DEMODULATOR,               0x10);
		saa_writeb(SAA7134_DCXO_IDENT_CTRL,           0x00);
		saa_writeb(SAA7134_FM_DEEMPHASIS,             0x44);
		saa_writeb(SAA7134_STEREO_DAC_OUTPUT_SELECT,  0xa1);
		saa_writeb(SAA7134_NICAM_CONFIG,              0x00);
		break;
	case TVAUDIO_NICAM_AM:
		saa_writeb(SAA7134_DEMODULATOR,               0x12);
		saa_writeb(SAA7134_DCXO_IDENT_CTRL,           0x00);
		saa_writeb(SAA7134_FM_DEEMPHASIS,             0x44);
		saa_writeb(SAA7134_STEREO_DAC_OUTPUT_SELECT,  0xa1);
		saa_writeb(SAA7134_NICAM_CONFIG,              0x00);
		break;
	case TVAUDIO_FM_SAT_STEREO:
		/* not implemented (yet) */
		break;
	}
}

static int tvaudio_sleep(struct saa7134_dev *dev, int timeout)
{
	DECLARE_WAITQUEUE(wait, current);
	
	add_wait_queue(&dev->thread.wq, &wait);
	if (dev->thread.scan1 == dev->thread.scan2 && !dev->thread.shutdown) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (timeout < 0)
			schedule();
		else
			schedule_timeout(timeout);
	}
	remove_wait_queue(&dev->thread.wq, &wait);
	return dev->thread.scan1 != dev->thread.scan2;
}

static int tvaudio_checkcarrier(struct saa7134_dev *dev, struct mainscan *scan)
{
	__s32 left,right,value;

	if (audio_debug > 1) {
		int i;
		dprintk("debug %d:",scan->carr);
		for (i = -150; i <= 150; i += 30) {
			tvaudio_setcarrier(dev,scan->carr+i,scan->carr+i);
			saa_readl(SAA7134_LEVEL_READOUT1 >> 2);
			if (tvaudio_sleep(dev,SCAN_SAMPLE_DELAY))
				return -1;
			value = saa_readl(SAA7134_LEVEL_READOUT1 >> 2);
			if (0 == i)
				printk("  #  %6d  # ",value >> 16);
			else
				printk(" %6d",value >> 16);
		}
		printk("\n");
	}

	if (dev->tvnorm->id & scan->std) {
		tvaudio_setcarrier(dev,scan->carr-90,scan->carr-90);
		saa_readl(SAA7134_LEVEL_READOUT1 >> 2);
		if (tvaudio_sleep(dev,SCAN_SAMPLE_DELAY))
			return -1;
		left = saa_readl(SAA7134_LEVEL_READOUT1 >> 2);

		tvaudio_setcarrier(dev,scan->carr+90,scan->carr+90);
		saa_readl(SAA7134_LEVEL_READOUT1 >> 2);
		if (tvaudio_sleep(dev,SCAN_SAMPLE_DELAY))
			return -1;
		right = saa_readl(SAA7134_LEVEL_READOUT1 >> 2);

		left >>= 16;
		right >>= 16;
		value = left > right ? left - right : right - left;
		dprintk("scanning %d.%03d MHz [%4s] =>  dc is %5d [%d/%d]\n",
			scan->carr / 1000, scan->carr % 1000,
			scan->name, value, left, right);
	} else {
		value = 0;
		dprintk("skipping %d.%03d MHz [%4s]\n",
			scan->carr / 1000, scan->carr % 1000, scan->name);
	}
	return value;
}

#if 0
static void sifdebug_dump_regs(struct saa7134_dev *dev)
{
	print_regb(AUDIO_STATUS);
	print_regb(IDENT_SIF);
	print_regb(LEVEL_READOUT1);
	print_regb(LEVEL_READOUT2);
	print_regb(DCXO_IDENT_CTRL);
	print_regb(DEMODULATOR);
	print_regb(AGC_GAIN_SELECT);
	print_regb(MONITOR_SELECT);
	print_regb(FM_DEEMPHASIS);
	print_regb(FM_DEMATRIX);
	print_regb(SIF_SAMPLE_FREQ);
	print_regb(ANALOG_IO_SELECT);
}
#endif

static int tvaudio_getstereo(struct saa7134_dev *dev, struct saa7134_tvaudio *audio)
{
	__u32 idp,nicam;
	int retval = -1;
	
	switch (audio->mode) {
	case TVAUDIO_FM_MONO:
		return V4L2_TUNER_SUB_MONO;
	case TVAUDIO_FM_K_STEREO:
	case TVAUDIO_FM_BG_STEREO:
		idp = (saa_readb(SAA7134_IDENT_SIF) & 0xe0) >> 5;
		dprintk("getstereo: fm/stereo: idp=0x%x\n",idp);
		if (0x03 == (idp & 0x03))
			retval = V4L2_TUNER_SUB_LANG1 | V4L2_TUNER_SUB_LANG2;
		else if (0x05 == (idp & 0x05))
			retval = V4L2_TUNER_SUB_MONO | V4L2_TUNER_SUB_STEREO;
		else if (0x01 == (idp & 0x01))
			retval = V4L2_TUNER_SUB_MONO;
		break;
	case TVAUDIO_FM_SAT_STEREO:
		/* not implemented (yet) */
		break;
	case TVAUDIO_NICAM_FM:
	case TVAUDIO_NICAM_AM:
		nicam = saa_readb(SAA7134_NICAM_STATUS);
		dprintk("getstereo: nicam=0x%x\n",nicam);
		switch (nicam & 0x0b) {
		case 0x08:
			retval = V4L2_TUNER_SUB_MONO;
			break;
		case 0x09:
			retval = V4L2_TUNER_SUB_LANG1 | V4L2_TUNER_SUB_LANG2;
			break;
		case 0x0a:
			retval = V4L2_TUNER_SUB_MONO | V4L2_TUNER_SUB_STEREO;
			break;
		}
		break;
	}
	if (retval != -1)
		dprintk("found audio subchannels:%s%s%s%s\n",
			(retval & V4L2_TUNER_SUB_MONO)   ? " mono"   : "",
			(retval & V4L2_TUNER_SUB_STEREO) ? " stereo" : "",
			(retval & V4L2_TUNER_SUB_LANG1)  ? " lang1"  : "",
			(retval & V4L2_TUNER_SUB_LANG2)  ? " lang2"  : "");
	return retval;
}

static int tvaudio_setstereo(struct saa7134_dev *dev, struct saa7134_tvaudio *audio,
			     u32 mode)
{
	static char *name[] = {
		[ V4L2_TUNER_MODE_MONO   ] = "mono",
		[ V4L2_TUNER_MODE_STEREO ] = "stereo",
		[ V4L2_TUNER_MODE_LANG1  ] = "lang1",
		[ V4L2_TUNER_MODE_LANG2  ] = "lang2",
	};
	static u32 fm[] = {
		[ V4L2_TUNER_MODE_MONO   ] = 0x00,  /* ch1  */
		[ V4L2_TUNER_MODE_STEREO ] = 0x80,  /* auto */
		[ V4L2_TUNER_MODE_LANG1  ] = 0x00,  /* ch1  */
		[ V4L2_TUNER_MODE_LANG2  ] = 0x01,  /* ch2  */
	};
	u32 reg;

	switch (audio->mode) {
	case TVAUDIO_FM_MONO:
		/* nothing to do ... */
		break;
	case TVAUDIO_FM_K_STEREO:
	case TVAUDIO_FM_BG_STEREO:
		dprintk("setstereo [fm] => %s\n",
			name[ mode % ARRAY_SIZE(name) ]);
		reg = fm[ mode % ARRAY_SIZE(fm) ];
		saa_writeb(SAA7134_FM_DEMATRIX, reg);
		break;
	case TVAUDIO_FM_SAT_STEREO:
	case TVAUDIO_NICAM_AM:
	case TVAUDIO_NICAM_FM:
		/* FIXME */
		break;
	}
	return 0;
}

static int tvaudio_thread(void *data)
{
	struct saa7134_dev *dev = data;
	int carr_vals[ARRAY_SIZE(mainscan)];
	unsigned int i, audio, nscan;
	int max1,max2,carrier,rx,mode,lastmode,default_carrier;

	daemonize("%s", dev->name);
	allow_signal(SIGTERM);
	for (;;) {
		tvaudio_sleep(dev,-1);
		if (dev->thread.shutdown || signal_pending(current))
			goto done;

	restart:
		dev->thread.scan1 = dev->thread.scan2;
		dprintk("tvaudio thread scan start [%d]\n",dev->thread.scan1);
		dev->tvaudio  = NULL;
		tvaudio_init(dev);
		dev->automute = 1;
		mute_input_7134(dev);

		/* give the tuner some time */
		if (tvaudio_sleep(dev,SCAN_INITIAL_DELAY))
			goto restart;

		max1 = 0;
		max2 = 0;
		nscan = 0;
		carrier = 0;
		default_carrier = 0;
		for (i = 0; i < ARRAY_SIZE(mainscan); i++) {
			if (!(dev->tvnorm->id & mainscan[i].std))
				continue;
			if (!default_carrier)
				default_carrier = mainscan[i].carr;
			nscan++;
		}

		if (1 == nscan) {
			/* only one candidate -- skip scan ;) */
			max1 = 12345;
			carrier = default_carrier;
		} else {
			/* scan for the main carrier */
			saa_writeb(SAA7134_MONITOR_SELECT,0x00);
			tvaudio_setmode(dev,&tvaudio[0],NULL);
			for (i = 0; i < ARRAY_SIZE(mainscan); i++) {
				carr_vals[i] = tvaudio_checkcarrier(dev, mainscan+i);
				if (dev->thread.scan1 != dev->thread.scan2)
					goto restart;
			}
			for (max1 = 0, max2 = 0, i = 0; i < ARRAY_SIZE(mainscan); i++) {
				if (max1 < carr_vals[i]) {
					max2 = max1;
					max1 = carr_vals[i];
					carrier = mainscan[i].carr;
				} else if (max2 < carr_vals[i]) {
					max2 = carr_vals[i];
				}
			}
		}

		if (0 != carrier && max1 > 2000 && max1 > max2*3) {
			/* found good carrier */
			dprintk("found %s main sound carrier @ %d.%03d MHz [%d/%d]\n",
				dev->tvnorm->name, carrier/1000, carrier%1000,
				max1, max2);
			dev->last_carrier = carrier;

		} else if (0 != dev->last_carrier) {
			/* no carrier -- try last detected one as fallback */
			carrier = dev->last_carrier;
			printk(KERN_WARNING "%s/audio: audio carrier scan failed, "
			       "using %d.%03d MHz [last detected]\n",
			       dev->name, carrier/1000, carrier%1000);

		} else {
			/* no carrier + no fallback -- use default */
			carrier = default_carrier;
			printk(KERN_WARNING "%s/audio: audio carrier scan failed, "
			       "using %d.%03d MHz [default]\n",
			       dev->name, carrier/1000, carrier%1000);
		}
		tvaudio_setcarrier(dev,carrier,carrier);
		dev->automute = 0;
		saa_andorb(SAA7134_STEREO_DAC_OUTPUT_SELECT, 0x30, 0x00);
		saa7134_tvaudio_setmute(dev);

		/* find the exact tv audio norm */
		for (audio = UNSET, i = 0; i < TVAUDIO; i++) {
			if (dev->tvnorm->id != UNSET &&
			    !(dev->tvnorm->id & tvaudio[i].std))
				continue;
			if (tvaudio[i].carr1 != carrier)
				continue;

			if (UNSET == audio)
				audio = i;
			tvaudio_setmode(dev,&tvaudio[i],"trying");
			if (tvaudio_sleep(dev,SCAN_SUBCARRIER_DELAY))
				goto restart;
			if (-1 != tvaudio_getstereo(dev,&tvaudio[i])) {
				audio = i;
				break;
			}
		}
		saa_andorb(SAA7134_STEREO_DAC_OUTPUT_SELECT, 0x30, 0x30);
		if (UNSET == audio)
			continue;
		tvaudio_setmode(dev,&tvaudio[audio],"using");
		tvaudio_setstereo(dev,&tvaudio[audio],V4L2_TUNER_MODE_MONO);
		dev->tvaudio = &tvaudio[audio];

		lastmode = 42;
		for (;;) {
			if (tvaudio_sleep(dev,5*HZ))
				goto restart;
			if (dev->thread.shutdown || signal_pending(current))
				break;
			if (UNSET == dev->thread.mode) {
				rx = tvaudio_getstereo(dev,&tvaudio[i]);
				mode = saa7134_tvaudio_rx2mode(rx);
			} else {
				mode = dev->thread.mode;
			}
			if (lastmode != mode) {
				tvaudio_setstereo(dev,&tvaudio[audio],mode);
				lastmode = mode;
			}
		}
	}

 done:
	complete_and_exit(&dev->thread.exit, 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* saa7133 / saa7135 code                                             */

static char *stdres[0x20] = {
	[0x00] = "no standard detected",
	[0x01] = "B/G (in progress)",
	[0x02] = "D/K (in progress)",
	[0x03] = "M (in progress)",

	[0x04] = "B/G A2",
	[0x05] = "B/G NICAM",
	[0x06] = "D/K A2 (1)",
	[0x07] = "D/K A2 (2)",
	[0x08] = "D/K A2 (3)",
	[0x09] = "D/K NICAM",
	[0x0a] = "L NICAM",
	[0x0b] = "I NICAM",
	
	[0x0c] = "M Korea",
	[0x0d] = "M BTSC ",
	[0x0e] = "M EIAJ",

	[0x0f] = "FM radio / IF 10.7 / 50 deemp",
	[0x10] = "FM radio / IF 10.7 / 75 deemp",
	[0x11] = "FM radio / IF sel / 50 deemp",
	[0x12] = "FM radio / IF sel / 75 deemp",

	[0x13 ... 0x1e ] = "unknown",
	[0x1f] = "??? [in progress]",
};

#define DSP_RETRY 32
#define DSP_DELAY 16

static inline int saa_dsp_wait_bit(struct saa7134_dev *dev, int bit)
{
	int state, count = DSP_RETRY;

	state = saa_readb(SAA7135_DSP_RWSTATE);
	if (unlikely(state & SAA7135_DSP_RWSTATE_ERR)) {
		printk("%s: dsp access error\n",dev->name);
		/* FIXME: send ack ... */
		return -EIO;
	}
	while (0 == (state & bit)) {
		if (unlikely(0 == count)) {
			printk("%s: dsp access wait timeout [bit=%s]\n",
			       dev->name,
			       (bit & SAA7135_DSP_RWSTATE_WRR) ? "WRR" :
			       (bit & SAA7135_DSP_RWSTATE_RDB) ? "RDB" :
			       (bit & SAA7135_DSP_RWSTATE_IDA) ? "IDA" :
			       "???");
			return -EIO;
		}
		saa_wait(DSP_DELAY);
		state = saa_readb(SAA7135_DSP_RWSTATE);
		count--;
	}
	return 0;
}

#if 0
static int saa_dsp_readl(struct saa7134_dev *dev, int reg, u32 *value)
{
	int err;

	d2printk("dsp read reg 0x%x\n", reg<<2);
	saa_readl(reg);
	err = saa_dsp_wait_bit(dev,SAA7135_DSP_RWSTATE_RDB);
	if (err < 0)
		return err;
	*value = saa_readl(reg);
	d2printk("dsp read   => 0x%06x\n", *value & 0xffffff);
	err = saa_dsp_wait_bit(dev,SAA7135_DSP_RWSTATE_IDA);
	if (err < 0)
		return err;
	return 0;
}
#endif

int saa_dsp_writel(struct saa7134_dev *dev, int reg, u32 value)
{
	int err;

	d2printk("dsp write reg 0x%x = 0x%06x\n",reg<<2,value);
	err = saa_dsp_wait_bit(dev,SAA7135_DSP_RWSTATE_WRR);
	if (err < 0)
		return err;
	saa_writel(reg,value);
	err = saa_dsp_wait_bit(dev,SAA7135_DSP_RWSTATE_WRR);
	if (err < 0)
		return err;
	return 0;
}

static int getstereo_7133(struct saa7134_dev *dev)
{
	int retval = V4L2_TUNER_SUB_MONO;
	u32 value;

	value = saa_readl(0x528 >> 2);
	if (value & 0x20)
		retval = V4L2_TUNER_SUB_MONO | V4L2_TUNER_SUB_STEREO;
	if (value & 0x40)
		retval = V4L2_TUNER_SUB_LANG1 | V4L2_TUNER_SUB_LANG2;
	return retval;
}

static int mute_input_7133(struct saa7134_dev *dev)
{
	u32 reg = 0;
	
	switch (dev->input->amux) {
	case TV:    reg = 0x02; break;
	case LINE1: reg = 0x00; break;
	case LINE2: reg = 0x01; break;
	}
	if (dev->ctl_mute)
		reg = 0x07;
	saa_writel(0x594 >> 2, reg);
	return 0;
}

static int tvaudio_thread_ddep(void *data)
{
	struct saa7134_dev *dev = data;
	u32 value, norms, clock;

	daemonize("%s", dev->name);
	allow_signal(SIGTERM);

	clock = saa7134_boards[dev->board].audio_clock;
	if (UNSET != audio_clock_override)
		clock = audio_clock_override;
	saa_writel(0x598 >> 2, clock);

	/* unmute */
	saa_dsp_writel(dev, 0x474 >> 2, 0x00);
	saa_dsp_writel(dev, 0x450 >> 2, 0x00);

	for (;;) {
		tvaudio_sleep(dev,-1);
		if (dev->thread.shutdown || signal_pending(current))
			goto done;

	restart:
		dev->thread.scan1 = dev->thread.scan2;
		dprintk("tvaudio thread scan start [%d]\n",dev->thread.scan1);

		if (audio_ddep >= 0x04 && audio_ddep <= 0x0e) {
			/* insmod option override */
			norms = (audio_ddep << 2) | 0x01;
			dprintk("ddep override: %s\n",stdres[audio_ddep]);
		} else{
			/* (let chip) scan for sound carrier */
			norms = 0;
			if (dev->tvnorm->id & V4L2_STD_PAL) {
				dprintk("PAL scan\n");
				norms |= 0x2c; /* B/G + D/K + I */
			}
			if (dev->tvnorm->id & V4L2_STD_NTSC) {
				dprintk("NTSC scan\n");
				norms |= 0x40; /* M */
			}
			if (dev->tvnorm->id & V4L2_STD_SECAM) {
				dprintk("SECAM scan\n");
				norms |= 0x18; /* L + D/K */
			}
			if (0 == norms)
				norms = 0x7c; /* all */
			dprintk("scanning:%s%s%s%s%s\n",
				(norms & 0x04) ? " B/G"  : "",
				(norms & 0x08) ? " D/K"  : "",
				(norms & 0x10) ? " L/L'" : "",
				(norms & 0x20) ? " I"    : "",
				(norms & 0x40) ? " M"    : "");
		}

		/* kick automatic standard detection */
		saa_dsp_writel(dev, 0x454 >> 2, 0);
		saa_dsp_writel(dev, 0x454 >> 2, norms | 0x80);

		/* setup crossbars */
		saa_dsp_writel(dev, 0x464 >> 2, 0x000000);
		saa_dsp_writel(dev, 0x470 >> 2, 0x101010);

		if (tvaudio_sleep(dev,3*HZ))
			goto restart;
		value = saa_readl(0x528 >> 2) & 0xffffff;

		dprintk("tvaudio thread status: 0x%x [%s%s%s]\n",
			value, stdres[value & 0x1f],
			(value & 0x000020) ? ",stereo" : "",
			(value & 0x000040) ? ",dual"   : "");
		dprintk("detailed status: "
			"%s#%s#%s#%s#%s#%s#%s#%s#%s#%s#%s#%s#%s#%s\n",
			(value & 0x000080) ? " A2/EIAJ pilot tone "     : "",
			(value & 0x000100) ? " A2/EIAJ dual "           : "",
			(value & 0x000200) ? " A2/EIAJ stereo "         : "",
			(value & 0x000400) ? " A2/EIAJ noise mute "     : "",

			(value & 0x000800) ? " BTSC/FM radio pilot "    : "",
			(value & 0x001000) ? " SAP carrier "            : "",
			(value & 0x002000) ? " BTSC stereo noise mute " : "",
			(value & 0x004000) ? " SAP noise mute "         : "",
			(value & 0x008000) ? " VDSP "                   : "",
			
			(value & 0x010000) ? " NICST "                  : "",
			(value & 0x020000) ? " NICDU "                  : "",
			(value & 0x040000) ? " NICAM muted "            : "",
			(value & 0x080000) ? " NICAM reserve sound "    : "",
			
			(value & 0x100000) ? " init done "              : "");
	}

 done:
	complete_and_exit(&dev->thread.exit, 0);
	return 0;
}

/* ------------------------------------------------------------------ */
/* common stuff + external entry points                               */

int saa7134_tvaudio_rx2mode(u32 rx)
{
	u32 mode;
	
	mode = V4L2_TUNER_MODE_MONO;
	if (rx & V4L2_TUNER_SUB_STEREO)
		mode = V4L2_TUNER_MODE_STEREO;
	else if (rx & V4L2_TUNER_SUB_LANG1)
		mode = V4L2_TUNER_MODE_LANG1;
	else if (rx & V4L2_TUNER_SUB_LANG2)
		mode = V4L2_TUNER_MODE_LANG2;
	return mode;
}
	
void saa7134_tvaudio_setmute(struct saa7134_dev *dev)
{
	switch (dev->pci->device) {
	case PCI_DEVICE_ID_PHILIPS_SAA7130:
	case PCI_DEVICE_ID_PHILIPS_SAA7134:
		mute_input_7134(dev);
		break;
	case PCI_DEVICE_ID_PHILIPS_SAA7133:
	case PCI_DEVICE_ID_PHILIPS_SAA7135:
		mute_input_7133(dev);
		break;
	}
}

void saa7134_tvaudio_setinput(struct saa7134_dev *dev,
			      struct saa7134_input *in)
{
	dev->input = in;
	switch (dev->pci->device) {
	case PCI_DEVICE_ID_PHILIPS_SAA7130:
	case PCI_DEVICE_ID_PHILIPS_SAA7134:
		mute_input_7134(dev);
		break;
	case PCI_DEVICE_ID_PHILIPS_SAA7133:
	case PCI_DEVICE_ID_PHILIPS_SAA7135:
		mute_input_7133(dev);
		break;
	}
}

void saa7134_tvaudio_setvolume(struct saa7134_dev *dev, int level)
{
	switch (dev->pci->device) {
	case PCI_DEVICE_ID_PHILIPS_SAA7134:
		saa_writeb(SAA7134_CHANNEL1_LEVEL,     level & 0x1f);
		saa_writeb(SAA7134_CHANNEL2_LEVEL,     level & 0x1f);
		saa_writeb(SAA7134_NICAM_LEVEL_ADJUST, level & 0x1f);
		break;
	}
}

int saa7134_tvaudio_getstereo(struct saa7134_dev *dev)
{
	int retval = V4L2_TUNER_SUB_MONO;

	switch (dev->pci->device) {
	case PCI_DEVICE_ID_PHILIPS_SAA7134:
		if (dev->tvaudio)
			retval = tvaudio_getstereo(dev,dev->tvaudio);
		break;
	case PCI_DEVICE_ID_PHILIPS_SAA7133:
	case PCI_DEVICE_ID_PHILIPS_SAA7135:
		retval = getstereo_7133(dev);
		break;
	}
	return retval;
}

int saa7134_tvaudio_init2(struct saa7134_dev *dev)
{
	DECLARE_MUTEX_LOCKED(sem);
	int (*my_thread)(void *data) = NULL;

	/* enable I2S audio output */
	if (saa7134_boards[dev->board].i2s_rate) {
		int i2sform = (32000 == saa7134_boards[dev->board].i2s_rate) ? 0x00 : 0x01;
		
		/* enable I2S output */
		saa_writeb(SAA7134_I2S_OUTPUT_SELECT,  0x80); 
		saa_writeb(SAA7134_I2S_OUTPUT_FORMAT,  i2sform); 
		saa_writeb(SAA7134_I2S_OUTPUT_LEVEL,   0x0F);	
		saa_writeb(SAA7134_I2S_AUDIO_OUTPUT,   0x01);
	}

	switch (dev->pci->device) {
	case PCI_DEVICE_ID_PHILIPS_SAA7134:
		my_thread = tvaudio_thread;
		break;
	case PCI_DEVICE_ID_PHILIPS_SAA7133:
	case PCI_DEVICE_ID_PHILIPS_SAA7135:
		my_thread = tvaudio_thread_ddep;
		break;
	}

	dev->thread.pid = -1;
	if (my_thread) {
		/* start tvaudio thread */
		init_waitqueue_head(&dev->thread.wq);
		init_completion(&dev->thread.exit);
		dev->thread.pid = kernel_thread(my_thread,dev,0);
		if (dev->thread.pid < 0)
			printk(KERN_WARNING "%s: kernel_thread() failed\n",
			       dev->name);
		wake_up_interruptible(&dev->thread.wq);
	}

	return 0;
}

int saa7134_tvaudio_fini(struct saa7134_dev *dev)
{
	/* shutdown tvaudio thread */
	if (dev->thread.pid >= 0) {
		dev->thread.shutdown = 1;
		wake_up_interruptible(&dev->thread.wq);
		wait_for_completion(&dev->thread.exit);
	}
	saa_andorb(SAA7134_ANALOG_IO_SELECT, 0x07, 0x00); /* LINE1 */
	return 0;
}

int saa7134_tvaudio_do_scan(struct saa7134_dev *dev)
{
	if (dev->thread.pid >= 0) {
		dev->thread.mode = UNSET;
		dev->thread.scan2++;
		wake_up_interruptible(&dev->thread.wq);
	} else {
		dev->automute = 0;
		saa7134_tvaudio_setmute(dev);
	}
	return 0;
}

/* ----------------------------------------------------------- */
/*
 * Local variables:
 * c-basic-offset: 8
 * End:
 */
