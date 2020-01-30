/* radio-trust.c - Trust FM Radio card driver for Linux 2.2 
 * by Eric Lammerts <eric@scintilla.utwente.nl>
 *
 * Based on radio-aztech.c. Original notes:
 *
 * Adapted to support the Video for Linux API by 
 * Russell Kroll <rkroll@exploits.org>.  Based on original tuner code by:
 *
 * Quay Ly
 * Donald Song
 * Jason Lewis      (jlewis@twilight.vtc.vsc.edu) 
 * Scott McGrath    (smcgrath@twilight.vtc.vsc.edu)
 * William McGrath  (wmcgrath@twilight.vtc.vsc.edu)
 *
 * The basis for this code may be found at http://bigbang.vtc.vsc.edu/fmradio/
 */

#include <stdarg.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/videodev.h>
#include <linux/config.h>	/* CONFIG_RADIO_TRUST_PORT 	*/

/* acceptable ports: 0x350 (JP3 shorted), 0x358 (JP3 open) */

#ifndef CONFIG_RADIO_TRUST_PORT
#define CONFIG_RADIO_TRUST_PORT -1
#endif

static int io = CONFIG_RADIO_TRUST_PORT; 
static int ioval = 0xf;
static int users = 0;
static __u16 curvol;
static __u16 curbass;
static __u16 curtreble;
static unsigned long curfreq;
static int curstereo;
static int curmute;

/* i2c addresses */
#define TDA7318_ADDR 0x88
#define TSA6060T_ADDR 0xc4

#define TR_DELAY do { inb(io); inb(io); inb(io); } while(0)
#define TR_SET_SCL outb(ioval |= 2, io)
#define TR_CLR_SCL outb(ioval &= 0xfd, io)
#define TR_SET_SDA outb(ioval |= 1, io)
#define TR_CLR_SDA outb(ioval &= 0xfe, io)

static void write_i2c(int n, ...)
{
	unsigned char val, mask;
	va_list args;

	va_start(args, n);

	/* start condition */
	TR_SET_SDA;
	TR_SET_SCL;
	TR_DELAY;
	TR_CLR_SDA;
	TR_CLR_SCL;
	TR_DELAY;

	for(; n; n--) {
		val = va_arg(args, unsigned);
		for(mask = 0x80; mask; mask >>= 1) {
			if(val & mask)
				TR_SET_SDA;
			else
				TR_CLR_SDA;
			TR_SET_SCL;
			TR_DELAY;
			TR_CLR_SCL;
			TR_DELAY;
		}
		/* acknowledge bit */
		TR_SET_SDA;
		TR_SET_SCL;
		TR_DELAY;
		TR_CLR_SCL;
		TR_DELAY;
	}

	/* stop condition */
	TR_CLR_SDA;
	TR_DELAY;
	TR_SET_SCL;
	TR_DELAY;
	TR_SET_SDA;
	TR_DELAY;

	va_end(args);
}

static void tr_setvol(__u16 vol)
{
	curvol = vol / 2048;
	write_i2c(2, TDA7318_ADDR, curvol ^ 0x1f);
}

static int basstreble2chip[15] = {
	0, 1, 2, 3, 4, 5, 6, 7, 14, 13, 12, 11, 10, 9, 8
};

static void tr_setbass(__u16 bass)
{
	curbass = bass / 4370;
	write_i2c(2, TDA7318_ADDR, 0x60 | basstreble2chip[curbass]);
}

static void tr_settreble(__u16 treble)
{
	curtreble = treble / 4370;
	write_i2c(2, TDA7318_ADDR, 0x70 | basstreble2chip[curtreble]);
}

static void tr_setstereo(int stereo)
{
	curstereo = !!stereo;
	ioval = (ioval & 0xfb) | (!curstereo << 2);
	outb(ioval, io);
}

static void tr_setmute(int mute)
{
	curmute = !!mute;
	ioval = (ioval & 0xf7) | (curmute << 3);
	outb(ioval, io);
}

static int tr_getsigstr(void)
{
	int i, v;
	
	for(i = 0, v = 0; i < 100; i++) v |= inb(io);
	return (v & 1)? 0 : 0xffff;
}

static int tr_getstereo(void)
{
	/* don't know how to determine it, just return the setting */
	return curstereo;
}

static void tr_setfreq(unsigned long f)
{
	f /= 160;	/* Convert to 10 kHz units	*/
	f += 1070;	/* Add 10.7 MHz IF			*/

	write_i2c(5, TSA6060T_ADDR, (f << 1) | 1, f >> 7, 0x60 | ((f >> 15) & 1), 0);
}

static int tr_ioctl(struct video_device *dev, unsigned int cmd, void *arg)
{
	switch(cmd)
	{
		case VIDIOCGCAP:
		{
			struct video_capability v;

			v.type=VID_TYPE_TUNER;
			v.channels=1;
			v.audios=1;

			/* No we don't do pictures */
			v.maxwidth=0;
			v.maxheight=0;
			v.minwidth=0;
			v.minheight=0;

			strcpy(v.name, "Trust FM Radio");

			if(copy_to_user(arg,&v,sizeof(v)))
				return -EFAULT;

			return 0;
		}
		case VIDIOCGTUNER:
		{
			struct video_tuner v;

			if(copy_from_user(&v, arg,sizeof(v))!=0) 
				return -EFAULT;

			if(v.tuner)	/* Only 1 tuner */ 
				return -EINVAL;

			v.rangelow = 87500 * 16;
			v.rangehigh = 108000 * 16;
			v.flags = VIDEO_TUNER_LOW;
			v.mode = VIDEO_MODE_AUTO;

			v.signal = tr_getsigstr();
			if(tr_getstereo())
				v.flags |= VIDEO_TUNER_STEREO_ON;

			strcpy(v.name, "FM");

			if(copy_to_user(arg,&v, sizeof(v)))
				return -EFAULT;

			return 0;
		}
		case VIDIOCSTUNER:
		{
			struct video_tuner v;

			if(copy_from_user(&v, arg, sizeof(v)))
				return -EFAULT;
			if(v.tuner != 0)
				return -EINVAL;

			return 0;
		}
		case VIDIOCGFREQ:
			if(copy_to_user(arg, &curfreq, sizeof(curfreq)))
				return -EFAULT;
			return 0;

		case VIDIOCSFREQ:
		{
			unsigned long f;

			if(copy_from_user(&f, arg, sizeof(curfreq)))
				return -EFAULT;
			tr_setfreq(f);
			return 0;
		}
		case VIDIOCGAUDIO:
		{	
			struct video_audio v;

			memset(&v,0, sizeof(v));
			v.flags = VIDEO_AUDIO_MUTABLE | VIDEO_AUDIO_VOLUME |
			          VIDEO_AUDIO_BASS | VIDEO_AUDIO_TREBLE;
			v.mode = curstereo? VIDEO_SOUND_STEREO : VIDEO_SOUND_MONO;
			v.volume = curvol * 2048;
			v.step = 2048;
			v.bass = curbass * 4370;
			v.treble = curtreble * 4370;
			
			strcpy(v.name, "Trust FM Radio");
			if(copy_to_user(arg,&v, sizeof(v)))
				return -EFAULT;
			return 0;			
		}
		case VIDIOCSAUDIO:
		{
			struct video_audio v;

			if(copy_from_user(&v, arg, sizeof(v))) 
				return -EFAULT;	
			if(v.audio) 
				return -EINVAL;

			tr_setvol(v.volume);					
			tr_setbass(v.bass);
			tr_settreble(v.treble);
			tr_setstereo(v.mode & VIDEO_SOUND_STEREO);
			tr_setmute(v.flags & VIDEO_AUDIO_MUTE);
			return 0;
		}
		default:
			return -ENOIOCTLCMD;
	}
}

static int tr_open(struct video_device *dev, int flags)
{
	if(users)
		return -EBUSY;
	users++;
	MOD_INC_USE_COUNT;
	return 0;
}

static void tr_close(struct video_device *dev)
{
	users--;
	MOD_DEC_USE_COUNT;
}

static struct video_device trust_radio=
{
	name:		"Trust FM Radio",
	type:		VID_TYPE_TUNER,
	hardware:	VID_HARDWARE_TRUST,
	open:		tr_open,
	close:		tr_close,
	ioctl:		tr_ioctl,
};

static int __init trust_init(void)
{
	if(io == -1) {
		printk(KERN_ERR "You must set an I/O address with io=0x???\n");
		return -EINVAL;
	}
	if(!request_region(io, 2, "Trust FM Radio")) {
		printk(KERN_ERR "trust: port 0x%x already in use\n", io);
		return -EBUSY;
	}
	if(video_register_device(&trust_radio, VFL_TYPE_RADIO)==-1)
	{
		release_region(io, 2);
		return -EINVAL;
	}

	printk(KERN_INFO "Trust FM Radio card driver v1.0.\n");

	write_i2c(2, TDA7318_ADDR, 0x80);	/* speaker att. LF = 0 dB */
	write_i2c(2, TDA7318_ADDR, 0xa0);	/* speaker att. RF = 0 dB */
	write_i2c(2, TDA7318_ADDR, 0xc0);	/* speaker att. LR = 0 dB */
	write_i2c(2, TDA7318_ADDR, 0xe0);	/* speaker att. RR = 0 dB */
	write_i2c(2, TDA7318_ADDR, 0x40);	/* stereo 1 input, gain = 18.75 dB */

	tr_setvol(0x8000);					
	tr_setbass(0x8000);
	tr_settreble(0x8000);
	tr_setstereo(1);

	/* mute card - prevents noisy bootups */
	tr_setmute(1);

	return 0;
}

MODULE_AUTHOR("Eric Lammerts, Russell Kroll, Quay Lu, Donald Song, Jason Lewis, Scott McGrath, William McGrath");
MODULE_DESCRIPTION("A driver for the Trust FM Radio card.");
MODULE_PARM(io, "i");
MODULE_PARM_DESC(io, "I/O address of the Trust FM Radio card (0x350 or 0x358)");

EXPORT_NO_SYMBOLS;

static void __exit cleanup_trust_module(void)
{
	video_unregister_device(&trust_radio);
	release_region(io, 2);
}

module_init(trust_init);
module_exit(cleanup_trust_module);
