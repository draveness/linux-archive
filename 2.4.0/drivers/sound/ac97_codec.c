
/*
 * ac97_codec.c: Generic AC97 mixer/modem module
 *
 * Derived from ac97 mixer in maestro and trident driver.
 *
 * Copyright 2000 Silicon Integrated System Corporation
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU General Public License for more details.
 *
 *	You should have received a copy of the GNU General Public License
 *	along with this program; if not, write to the Free Software
 *	Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * History
 * v0.4 Mar 15 2000 Ollie Lho
 *	dual codecs support verified with 4 channels output
 * v0.3 Feb 22 2000 Ollie Lho
 *	bug fix for record mask setting
 * v0.2 Feb 10 2000 Ollie Lho
 *	add ac97_read_proc for /proc/driver/{vendor}/ac97
 * v0.1 Jan 14 2000 Ollie Lho <ollie@sis.com.tw> 
 *	Isolated from trident.c to support multiple ac97 codec
 */
#include <linux/module.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/ac97_codec.h>
#include <asm/uaccess.h>

static int ac97_read_mixer(struct ac97_codec *codec, int oss_channel);
static void ac97_write_mixer(struct ac97_codec *codec, int oss_channel, 
			     unsigned int left, unsigned int right);
static void ac97_set_mixer(struct ac97_codec *codec, unsigned int oss_mixer, unsigned int val );
static int ac97_recmask_io(struct ac97_codec *codec, int rw, int mask);
static int ac97_mixer_ioctl(struct ac97_codec *codec, unsigned int cmd, unsigned long arg);

static int ac97_init_mixer(struct ac97_codec *codec);

static int sigmatel_init(struct ac97_codec *codec);
static int enable_eapd(struct ac97_codec *codec);

#define arraysize(x)   (sizeof(x)/sizeof((x)[0]))

static struct {
	unsigned int id;
	char *name;
	int  (*init)  (struct ac97_codec *codec);
} ac97_codec_ids[] = {
	{0x414B4D00, "Asahi Kasei AK4540 rev 0", NULL},
	{0x414B4D01, "Asahi Kasei AK4540 rev 1", NULL},
	{0x41445340, "Analog Devices AD1881"  , NULL},
	{0x41445360, "Analog Devices AD1885"  , enable_eapd},
	{0x43525900, "Cirrus Logic CS4297"    , NULL},
	{0x43525903, "Cirrus Logic CS4297"  ,	NULL},
	{0x43525913, "Cirrus Logic CS4297A"   , NULL},
	{0x43525923, "Cirrus Logic CS4298"    , NULL},
	{0x4352592B, "Cirrus Logic CS4294"    , NULL},
	{0x43525931, "Cirrus Logic CS4299"    , NULL},
	{0x43525934, "Cirrus Logic CS4299"    , NULL},
	{0x4e534331, "National Semiconductor LM4549" ,	NULL},
	{0x53494c22, "Silicon Laboratory Si3036"     ,	NULL},
	{0x53494c23, "Silicon Laboratory Si3038"     ,  NULL},
	{0x83847600, "SigmaTel STAC????"      , NULL},
	{0x83847604, "SigmaTel STAC9701/3/4/5", NULL},
	{0x83847605, "SigmaTel STAC9704"      , NULL},
	{0x83847608, "SigmaTel STAC9708"      , NULL},
	{0x83847609, "SigmaTel STAC9721/23"   , sigmatel_init},
	{0x54524103, "TriTech TR?????"	      , NULL},
	{0x54524106, "TriTech TR28026"        , NULL},
	{0x54524108, "TriTech TR28028"        , NULL},
	{0x54524123, "TriTech TR?????"	      , NULL},	
	{0x574D4C00, "Wolfson WM9704"         , NULL},
	{0x00000000, NULL, NULL}
};

static const char *ac97_stereo_enhancements[] =
{
	/*   0 */ "No 3D Stereo Enhancement",
	/*   1 */ "Analog Devices Phat Stereo",
	/*   2 */ "Creative Stereo Enhancement",
	/*   3 */ "National Semi 3D Stereo Enhancement",
	/*   4 */ "YAMAHA Ymersion",
	/*   5 */ "BBE 3D Stereo Enhancement",
	/*   6 */ "Crystal Semi 3D Stereo Enhancement",
	/*   7 */ "Qsound QXpander",
	/*   8 */ "Spatializer 3D Stereo Enhancement",
	/*   9 */ "SRS 3D Stereo Enhancement",
	/*  10 */ "Platform Tech 3D Stereo Enhancement",
	/*  11 */ "AKM 3D Audio",
	/*  12 */ "Aureal Stereo Enhancement",
	/*  13 */ "Aztech 3D Enhancement",
	/*  14 */ "Binaura 3D Audio Enhancement",
	/*  15 */ "ESS Technology Stereo Enhancement",
	/*  16 */ "Harman International VMAx",
	/*  17 */ "Nvidea 3D Stereo Enhancement",
	/*  18 */ "Philips Incredible Sound",
	/*  19 */ "Texas Instruments 3D Stereo Enhancement",
	/*  20 */ "VLSI Technology 3D Stereo Enhancement",
	/*  21 */ "TriTech 3D Stereo Enhancement",
	/*  22 */ "Realtek 3D Stereo Enhancement",
	/*  23 */ "Samsung 3D Stereo Enhancement",
	/*  24 */ "Wolfson Microelectronics 3D Enhancement",
	/*  25 */ "Delta Integration 3D Enhancement",
	/*  26 */ "SigmaTel 3D Enhancement",
	/*  27 */ "Reserved 27",
	/*  28 */ "Rockwell 3D Stereo Enhancement",
	/*  29 */ "Reserved 29",
	/*  30 */ "Reserved 30",
	/*  31 */ "Reserved 31"
};

/* this table has default mixer values for all OSS mixers. */
static struct mixer_defaults {
	int mixer;
	unsigned int value;
} mixer_defaults[SOUND_MIXER_NRDEVICES] = {
	/* all values 0 -> 100 in bytes */
	{SOUND_MIXER_VOLUME,	0x4343},
	{SOUND_MIXER_BASS,	0x4343},
	{SOUND_MIXER_TREBLE,	0x4343},
	{SOUND_MIXER_PCM,	0x4343},
	{SOUND_MIXER_SPEAKER,	0x4343},
	{SOUND_MIXER_LINE,	0x4343},
	{SOUND_MIXER_MIC,	0x0000},
	{SOUND_MIXER_CD,	0x4343},
	{SOUND_MIXER_ALTPCM,	0x4343},
	{SOUND_MIXER_IGAIN,	0x4343},
	{SOUND_MIXER_LINE1,	0x4343},
	{SOUND_MIXER_PHONEIN,	0x4343},
	{SOUND_MIXER_PHONEOUT,	0x4343},
	{SOUND_MIXER_VIDEO,	0x4343},
	{-1,0}
};

/* table to scale scale from OSS mixer value to AC97 mixer register value */	
static struct ac97_mixer_hw {
	unsigned char offset;
	int scale;
} ac97_hw[SOUND_MIXER_NRDEVICES]= {
	[SOUND_MIXER_VOLUME]	=	{AC97_MASTER_VOL_STEREO,64},
	[SOUND_MIXER_BASS]	=	{AC97_MASTER_TONE,	16},
	[SOUND_MIXER_TREBLE]	=	{AC97_MASTER_TONE,	16},
	[SOUND_MIXER_PCM]	=	{AC97_PCMOUT_VOL,	32},
	[SOUND_MIXER_SPEAKER]	=	{AC97_PCBEEP_VOL,	16},
	[SOUND_MIXER_LINE]	=	{AC97_LINEIN_VOL,	32},
	[SOUND_MIXER_MIC]	=	{AC97_MIC_VOL,		32},
	[SOUND_MIXER_CD]	=	{AC97_CD_VOL,		32},
	[SOUND_MIXER_ALTPCM]	=	{AC97_HEADPHONE_VOL,	64},
	[SOUND_MIXER_IGAIN]	=	{AC97_RECORD_GAIN,	16},
	[SOUND_MIXER_LINE1]	=	{AC97_AUX_VOL,		32},
	[SOUND_MIXER_PHONEIN]	= 	{AC97_PHONE_VOL,	32},
	[SOUND_MIXER_PHONEOUT]	= 	{AC97_MASTER_VOL_MONO,	64},
	[SOUND_MIXER_VIDEO]	=	{AC97_VIDEO_VOL,	32},
};

/* the following tables allow us to go from OSS <-> ac97 quickly. */
enum ac97_recsettings {
	AC97_REC_MIC=0,
	AC97_REC_CD,
	AC97_REC_VIDEO,
	AC97_REC_AUX,
	AC97_REC_LINE,
	AC97_REC_STEREO, /* combination of all enabled outputs..  */
	AC97_REC_MONO,	      /*.. or the mono equivalent */
	AC97_REC_PHONE	      
};

static unsigned int ac97_rm2oss[] = {
	[AC97_REC_MIC] 	 = SOUND_MIXER_MIC,
	[AC97_REC_CD] 	 = SOUND_MIXER_CD,
	[AC97_REC_VIDEO] = SOUND_MIXER_VIDEO,
	[AC97_REC_AUX] 	 = SOUND_MIXER_LINE1,
	[AC97_REC_LINE]  = SOUND_MIXER_LINE,
	[AC97_REC_STEREO]= SOUND_MIXER_IGAIN,
	[AC97_REC_PHONE] = SOUND_MIXER_PHONEIN
};

/* indexed by bit position */
static unsigned int ac97_oss_rm[] = {
	[SOUND_MIXER_MIC] 	= AC97_REC_MIC,
	[SOUND_MIXER_CD] 	= AC97_REC_CD,
	[SOUND_MIXER_VIDEO] 	= AC97_REC_VIDEO,
	[SOUND_MIXER_LINE1] 	= AC97_REC_AUX,
	[SOUND_MIXER_LINE] 	= AC97_REC_LINE,
	[SOUND_MIXER_IGAIN]	= AC97_REC_STEREO,
	[SOUND_MIXER_PHONEIN] 	= AC97_REC_PHONE
};

/* reads the given OSS mixer from the ac97 the caller must have insured that the ac97 knows
   about that given mixer, and should be holding a spinlock for the card */
static int ac97_read_mixer(struct ac97_codec *codec, int oss_channel) 
{
	u16 val;
	int ret = 0;
	int scale;
	struct ac97_mixer_hw *mh = &ac97_hw[oss_channel];

	val = codec->codec_read(codec , mh->offset);

	if (val & AC97_MUTE) {
		ret = 0;
	} else if (AC97_STEREO_MASK & (1 << oss_channel)) {
		/* nice stereo mixers .. */
		int left,right;

		left = (val >> 8)  & 0x7f;
		right = val  & 0x7f;

		if (oss_channel == SOUND_MIXER_IGAIN) {
			right = (right * 100) / mh->scale;
			left = (left * 100) / mh->scale;
		} else {
			/* these may have 5 or 6 bit resolution */
			if(oss_channel == SOUND_MIXER_VOLUME || oss_channel == SOUND_MIXER_ALTPCM)
				scale = (1 << codec->bit_resolution);
			else
				scale = mh->scale;

			right = 100 - ((right * 100) / scale);
			left = 100 - ((left * 100) / scale);
		}
		ret = left | (right << 8);
	} else if (oss_channel == SOUND_MIXER_SPEAKER) {
		ret = 100 - ((((val & 0x1e)>>1) * 100) / mh->scale);
	} else if (oss_channel == SOUND_MIXER_PHONEIN) {
		ret = 100 - (((val & 0x1f) * 100) / mh->scale);
	} else if (oss_channel == SOUND_MIXER_PHONEOUT) {
		scale = (1 << codec->bit_resolution);
		ret = 100 - (((val & 0x1f) * 100) / scale);
	} else if (oss_channel == SOUND_MIXER_MIC) {
		ret = 100 - (((val & 0x1f) * 100) / mh->scale);
		/*  the low bit is optional in the tone sliders and masking
		    it lets us avoid the 0xf 'bypass'.. */
	} else if (oss_channel == SOUND_MIXER_BASS) {
		ret = 100 - ((((val >> 8) & 0xe) * 100) / mh->scale);
	} else if (oss_channel == SOUND_MIXER_TREBLE) {
		ret = 100 - (((val & 0xe) * 100) / mh->scale);
	}

#ifdef DEBUG
	printk("ac97_codec: read OSS mixer %2d (%s ac97 register 0x%02x), "
	       "0x%04x -> 0x%04x\n",
	       oss_channel, codec->id ? "Secondary" : "Primary",
	       mh->offset, val, ret);
#endif

	return ret;
}

/* write the OSS encoded volume to the given OSS encoded mixer, again caller's job to
   make sure all is well in arg land, call with spinlock held */
static void ac97_write_mixer(struct ac97_codec *codec, int oss_channel,
		      unsigned int left, unsigned int right)
{
	u16 val = 0;
	int scale;
	struct ac97_mixer_hw *mh = &ac97_hw[oss_channel];

#ifdef DEBUG
	printk("ac97_codec: wrote OSS mixer %2d (%s ac97 register 0x%02x), "
	       "left vol:%2d, right vol:%2d:",
	       oss_channel, codec->id ? "Secondary" : "Primary",
	       mh->offset, left, right);
#endif

	if (AC97_STEREO_MASK & (1 << oss_channel)) {
		/* stereo mixers */
		if (left == 0 && right == 0) {
			val = AC97_MUTE;
		} else {
			if (oss_channel == SOUND_MIXER_IGAIN) {
				right = (right * mh->scale) / 100;
				left = (left * mh->scale) / 100;
				if (right >= mh->scale)
					right = mh->scale-1;
				if (left >= mh->scale)
					left = mh->scale-1;
			} else {
				/* these may have 5 or 6 bit resolution */
				if (oss_channel == SOUND_MIXER_VOLUME ||
				    oss_channel == SOUND_MIXER_ALTPCM)
					scale = (1 << codec->bit_resolution);
				else
					scale = mh->scale;

				right = ((100 - right) * scale) / 100;
				left = ((100 - left) * scale) / 100;
				if (right >= scale)
					right = scale-1;
				if (left >= scale)
					left = scale-1;
			}
			val = (left << 8) | right;
		}
	} else if (oss_channel == SOUND_MIXER_BASS) {
		val = codec->codec_read(codec , mh->offset) & ~0x0f00;
		left = ((100 - left) * mh->scale) / 100;
		if (left >= mh->scale)
			left = mh->scale-1;
		val |= (left << 8) & 0x0e00;
	} else if (oss_channel == SOUND_MIXER_TREBLE) {
		val = codec->codec_read(codec , mh->offset) & ~0x000f;
		left = ((100 - left) * mh->scale) / 100;
		if (left >= mh->scale)
			left = mh->scale-1;
		val |= left & 0x000e;
	} else if(left == 0) {
		val = AC97_MUTE;
	} else if (oss_channel == SOUND_MIXER_SPEAKER) {
		left = ((100 - left) * mh->scale) / 100;
		if (left >= mh->scale)
			left = mh->scale-1;
		val = left << 1;
	} else if (oss_channel == SOUND_MIXER_PHONEIN) {
		left = ((100 - left) * mh->scale) / 100;
		if (left >= mh->scale)
			left = mh->scale-1;
		val = left;
	} else if (oss_channel == SOUND_MIXER_PHONEOUT) {
		scale = (1 << codec->bit_resolution);
		left = ((100 - left) * scale) / 100;
		if (left >= mh->scale)
			left = mh->scale-1;
		val = left;
	} else if (oss_channel == SOUND_MIXER_MIC) {
		val = codec->codec_read(codec , mh->offset) & ~0x801f;
		left = ((100 - left) * mh->scale) / 100;
		if (left >= mh->scale)
			left = mh->scale-1;
		val |= left;
		/*  the low bit is optional in the tone sliders and masking
		    it lets us avoid the 0xf 'bypass'.. */
	}
#ifdef DEBUG
	printk(" 0x%04x", val);
#endif

	codec->codec_write(codec, mh->offset, val);

#ifdef DEBUG
	val = codec->codec_read(codec, mh->offset);
	printk(" -> 0x%04x\n", val);
#endif
}

/* a thin wrapper for write_mixer */
static void ac97_set_mixer(struct ac97_codec *codec, unsigned int oss_mixer, unsigned int val ) 
{
	unsigned int left,right;

	/* cleanse input a little */
	right = ((val >> 8)  & 0xff) ;
	left = (val  & 0xff) ;

	if (right > 100) right = 100;
	if (left > 100) left = 100;

	codec->mixer_state[oss_mixer] = (right << 8) | left;
	codec->write_mixer(codec, oss_mixer, left, right);
}

/* read or write the recmask, the ac97 can really have left and right recording
   inputs independantly set, but OSS doesn't seem to want us to express that to
   the user. the caller guarantees that we have a supported bit set, and they
   must be holding the card's spinlock */
static int ac97_recmask_io(struct ac97_codec *codec, int rw, int mask) 
{
	unsigned int val;

	if (rw) {
		/* read it from the card */
		val = codec->codec_read(codec, AC97_RECORD_SELECT);
#ifdef DEBUG
		printk("ac97_codec: ac97 recmask to set to 0x%04x\n", val);
#endif
		return (1 << ac97_rm2oss[val & 0x07]);
	}

	/* else, write the first set in the mask as the
	   output */	
	/* clear out current set value first (AC97 supports only 1 input!) */
	val = (1 << ac97_rm2oss[codec->codec_read(codec, AC97_RECORD_SELECT) & 0x07]);
	if (mask != val)
	    mask &= ~val;
       
	val = ffs(mask); 
	val = ac97_oss_rm[val-1];
	val |= val << 8;  /* set both channels */

#ifdef DEBUG
	printk("ac97_codec: setting ac97 recmask to 0x%04x\n", val);
#endif

	codec->codec_write(codec, AC97_RECORD_SELECT, val);

	return 0;
};

static int ac97_mixer_ioctl(struct ac97_codec *codec, unsigned int cmd, unsigned long arg)
{
	int i, val = 0;

	if (cmd == SOUND_MIXER_INFO) {
		mixer_info info;
		strncpy(info.id, codec->name, sizeof(info.id));
		strncpy(info.name, codec->name, sizeof(info.name));
		info.modify_counter = codec->modcnt;
		if (copy_to_user((void *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}
	if (cmd == SOUND_OLD_MIXER_INFO) {
		_old_mixer_info info;
		strncpy(info.id, codec->name, sizeof(info.id));
		strncpy(info.name, codec->name, sizeof(info.name));
		if (copy_to_user((void *)arg, &info, sizeof(info)))
			return -EFAULT;
		return 0;
	}

	if (_IOC_TYPE(cmd) != 'M' || _IOC_SIZE(cmd) != sizeof(int))
		return -EINVAL;

	if (cmd == OSS_GETVERSION)
		return put_user(SOUND_VERSION, (int *)arg);

	if (_IOC_DIR(cmd) == _IOC_READ) {
		switch (_IOC_NR(cmd)) {
		case SOUND_MIXER_RECSRC: /* give them the current record source */
			if (!codec->recmask_io) {
				val = 0;
			} else {
				val = codec->recmask_io(codec, 1, 0);
			}
			break;

		case SOUND_MIXER_DEVMASK: /* give them the supported mixers */
			val = codec->supported_mixers;
			break;

		case SOUND_MIXER_RECMASK: /* Arg contains a bit for each supported recording source */
			val = codec->record_sources;
			break;

		case SOUND_MIXER_STEREODEVS: /* Mixer channels supporting stereo */
			val = codec->stereo_mixers;
			break;

		case SOUND_MIXER_CAPS:
			val = SOUND_CAP_EXCL_INPUT;
			break;

		default: /* read a specific mixer */
			i = _IOC_NR(cmd);

			if (!supported_mixer(codec, i)) 
				return -EINVAL;

			/* do we ever want to touch the hardware? */
		        /* val = codec->read_mixer(codec, i); */
			val = codec->mixer_state[i];
 			break;
		}
		return put_user(val, (int *)arg);
	}

	if (_IOC_DIR(cmd) == (_IOC_WRITE|_IOC_READ)) {
		codec->modcnt++;
		if (get_user(val, (int *)arg))
			return -EFAULT;

		switch (_IOC_NR(cmd)) {
		case SOUND_MIXER_RECSRC: /* Arg contains a bit for each recording source */
			if (!codec->recmask_io) return -EINVAL;
			if (!val) return 0;
			if (!(val &= codec->record_sources)) return -EINVAL;

			codec->recmask_io(codec, 0, val);

			return 0;
		default: /* write a specific mixer */
			i = _IOC_NR(cmd);

			if (!supported_mixer(codec, i)) 
				return -EINVAL;

			ac97_set_mixer(codec, i, val);

			return 0;
		}
	}
	return -EINVAL;
}

/* entry point for /proc/driver/controller_vendor/ac97/%d */
int ac97_read_proc (char *page, char **start, off_t off,
		    int count, int *eof, void *data)
{
	int len = 0, cap, extid, val, id1, id2;
	struct ac97_codec *codec;
	int is_ac97_20 = 0;

	if ((codec = data) == NULL)
		return -ENODEV;

	id1 = codec->codec_read(codec, AC97_VENDOR_ID1);
	id2 = codec->codec_read(codec, AC97_VENDOR_ID2);
	len += sprintf (page+len, "Vendor name      : %s\n", codec->name);
	len += sprintf (page+len, "Vendor id        : %04X %04X\n", id1, id2);

	extid = codec->codec_read(codec, AC97_EXTENDED_ID);
	extid &= ~((1<<2)|(1<<4)|(1<<5)|(1<<10)|(1<<11)|(1<<12)|(1<<13));
	len += sprintf (page+len, "AC97 Version     : %s\n",
			extid ? "2.0 or later" : "1.0");
	if (extid) is_ac97_20 = 1;

	cap = codec->codec_read(codec, AC97_RESET);
	len += sprintf (page+len, "Capabilities     :%s%s%s%s%s%s\n",
			cap & 0x0001 ? " -dedicated MIC PCM IN channel-" : "",
			cap & 0x0002 ? " -reserved1-" : "",
			cap & 0x0004 ? " -bass & treble-" : "",
			cap & 0x0008 ? " -simulated stereo-" : "",
			cap & 0x0010 ? " -headphone out-" : "",
			cap & 0x0020 ? " -loudness-" : "");
	val = cap & 0x00c0;
	len += sprintf (page+len, "DAC resolutions  :%s%s%s\n",
			" -16-bit-",
			val & 0x0040 ? " -18-bit-" : "",
			val & 0x0080 ? " -20-bit-" : "");
	val = cap & 0x0300;
	len += sprintf (page+len, "ADC resolutions  :%s%s%s\n",
			" -16-bit-",
			val & 0x0100 ? " -18-bit-" : "",
			val & 0x0200 ? " -20-bit-" : "");
	len += sprintf (page+len, "3D enhancement   : %s\n",
			ac97_stereo_enhancements[(cap >> 10) & 0x1f]);

	val = codec->codec_read(codec, AC97_GENERAL_PURPOSE);
	len += sprintf (page+len, "POP path         : %s 3D\n"
			"Sim. stereo      : %s\n"
			"3D enhancement   : %s\n"
			"Loudness         : %s\n"
			"Mono output      : %s\n"
			"MIC select       : %s\n"
			"ADC/DAC loopback : %s\n",
			val & 0x8000 ? "post" : "pre",
			val & 0x4000 ? "on" : "off",
			val & 0x2000 ? "on" : "off",
			val & 0x1000 ? "on" : "off",
			val & 0x0200 ? "MIC" : "MIX",
			val & 0x0100 ? "MIC2" : "MIC1",
			val & 0x0080 ? "on" : "off");

	extid = codec->codec_read(codec, AC97_EXTENDED_ID);
	cap = extid;
	len += sprintf (page+len, "Ext Capabilities :%s%s%s%s%s%s%s\n",
			cap & 0x0001 ? " -var rate PCM audio-" : "",
			cap & 0x0002 ? " -2x PCM audio out-" : "",
			cap & 0x0008 ? " -var rate MIC in-" : "",
			cap & 0x0040 ? " -PCM center DAC-" : "",
			cap & 0x0080 ? " -PCM surround DAC-" : "",
			cap & 0x0100 ? " -PCM LFE DAC-" : "",
			cap & 0x0200 ? " -slot/DAC mappings-" : "");
	if (is_ac97_20) {
		len += sprintf (page+len, "Front DAC rate   : %d\n",
				codec->codec_read(codec, AC97_PCM_FRONT_DAC_RATE));
	}

	return len;
}

/**
 *	ac97_probe_codec - Initialize and setup AC97-compatible codec
 *	@codec: (in/out) Kernel info for a single AC97 codec
 *
 *	Reset the AC97 codec, then initialize the mixer and
 *	the rest of the @codec structure.
 *
 *	The codec_read and codec_write fields of @codec are
 *	required to be setup and working when this function
 *	is called.  All other fields are set by this function.
 *
 *	codec_wait field of @codec can optionally be provided
 *	when calling this function.  If codec_wait is not %NULL,
 *	this function will call codec_wait any time it is
 *	necessary to wait for the audio chip to reach the
 *	codec-ready state.  If codec_wait is %NULL, then
 *	the default behavior is to call schedule_timeout.
 *	Currently codec_wait is used to wait for AC97 codec
 *	reset to complete. 
 *
 *	Returns 1 (true) on success, or 0 (false) on failure.
 */
 
int ac97_probe_codec(struct ac97_codec *codec)
{
	u16 id1, id2;
	u16 audio, modem;
	int i;

	/* probing AC97 codec, AC97 2.0 says that bit 15 of register 0x00 (reset) should 
	 * be read zero.
	 *
	 * FIXME: is the following comment outdated?  -jgarzik 
	 * Probing of AC97 in this way is not reliable, it is not even SAFE !!
	 */
	codec->codec_write(codec, AC97_RESET, 0L);

	/* also according to spec, we wait for codec-ready state */	
	if (codec->codec_wait)
		codec->codec_wait(codec);
	else
		udelay(10);

	if ((audio = codec->codec_read(codec, AC97_RESET)) & 0x8000) {
		printk(KERN_ERR "ac97_codec: %s ac97 codec not present\n",
		       codec->id ? "Secondary" : "Primary");
		return 0;
	}

	/* probe for Modem Codec */
	codec->codec_write(codec, AC97_EXTENDED_MODEM_ID, 0L);
	modem = codec->codec_read(codec, AC97_EXTENDED_MODEM_ID);

	codec->name = NULL;
	codec->codec_init = NULL;

	id1 = codec->codec_read(codec, AC97_VENDOR_ID1);
	id2 = codec->codec_read(codec, AC97_VENDOR_ID2);
	for (i = 0; i < arraysize(ac97_codec_ids); i++) {
		if (ac97_codec_ids[i].id == ((id1 << 16) | id2)) {
			codec->type = ac97_codec_ids[i].id;
			codec->name = ac97_codec_ids[i].name;
			codec->codec_init = ac97_codec_ids[i].init;
			break;
		}
	}
	if (codec->name == NULL)
		codec->name = "Unknown";
	printk(KERN_INFO "ac97_codec: AC97 %s codec, id: 0x%04x:"
	       "0x%04x (%s)\n", audio ? "Audio" : (modem ? "Modem" : ""),
	       id1, id2, codec->name);

	return ac97_init_mixer(codec);
}

static int ac97_init_mixer(struct ac97_codec *codec)
{
	u16 cap;
	int i;

	cap = codec->codec_read(codec, AC97_RESET);

	/* mixer masks */
	codec->supported_mixers = AC97_SUPPORTED_MASK;
	codec->stereo_mixers = AC97_STEREO_MASK;
	codec->record_sources = AC97_RECORD_MASK;
	if (!(cap & 0x04))
		codec->supported_mixers &= ~(SOUND_MASK_BASS|SOUND_MASK_TREBLE);
	if (!(cap & 0x10))
		codec->supported_mixers &= ~SOUND_MASK_ALTPCM;

	/* detect bit resolution */
	codec->codec_write(codec, AC97_MASTER_VOL_STEREO, 0x2020);
	if(codec->codec_read(codec, AC97_MASTER_VOL_STEREO) == 0x1f1f)
		codec->bit_resolution = 5;
	else
		codec->bit_resolution = 6;

	/* generic OSS to AC97 wrapper */
	codec->read_mixer = ac97_read_mixer;
	codec->write_mixer = ac97_write_mixer;
	codec->recmask_io = ac97_recmask_io;
	codec->mixer_ioctl = ac97_mixer_ioctl;

	/* codec specific initialization for 4-6 channel output or secondary codec stuff */
	if (codec->codec_init != NULL) {
		codec->codec_init(codec);
	}

	/* initilize mixer channel volumes */
	for (i = 0; i < SOUND_MIXER_NRDEVICES; i++) {
		struct mixer_defaults *md = &mixer_defaults[i];
		if (md->mixer == -1) 
			break;
		if (!supported_mixer(codec, md->mixer)) 
			continue;
		ac97_set_mixer(codec, md->mixer, md->value);
	}

	return 1;
}

static int sigmatel_init(struct ac97_codec * codec)
{
	/* Only set up secondary codec */
	if (codec->id == 0)
		return 1;

	codec->codec_write(codec, AC97_SURROUND_MASTER, 0L);

	/* initialize SigmaTel STAC9721/23 as secondary codec, decoding AC link
	   sloc 3,4 = 0x01, slot 7,8 = 0x00, */
	codec->codec_write(codec, 0x74, 0x00);

	/* we don't have the crystal when we are on an AMR card, so use
	   BIT_CLK as our clock source. Write the magic word ABBA and read
	   back to enable register 0x78 */
	codec->codec_write(codec, 0x76, 0xabba);
	codec->codec_read(codec, 0x76);

	/* sync all the clocks*/
	codec->codec_write(codec, 0x78, 0x3802);

	return 1;
}

/*
 *	Bring up an AD1885
 */
 
static int enable_eapd(struct ac97_codec * codec)
{
	codec->codec_write(codec, AC97_POWER_CONTROL,
		codec->codec_read(codec, AC97_POWER_CONTROL)|0x8000);
	return 0;
}
	

EXPORT_SYMBOL(ac97_read_proc);
EXPORT_SYMBOL(ac97_probe_codec);
