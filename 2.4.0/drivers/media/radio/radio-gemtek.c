/* GemTek radio card driver for Linux (C) 1998 Jonas Munsin <jmunsin@iki.fi>
 *
 * GemTek hasn't released any specs on the card, so the protocol had to
 * be reverse engineered with dosemu.
 *
 * Besides the protocol changes, this is mostly a copy of:
 *
 *    RadioTrack II driver for Linux radio support (C) 1998 Ben Pfaff
 * 
 *    Based on RadioTrack I/RadioReveal (C) 1997 M. Kirkwood
 *    Coverted to new API by Alan Cox <Alan.Cox@linux.org>
 *    Various bugfixes and enhancements by Russell Kroll <rkroll@exploits.org>
 *
 * TODO: Allow for more than one of these foolish entities :-)
 *
 */

#include <linux/module.h>	/* Modules 			*/
#include <linux/init.h>		/* Initdata			*/
#include <linux/ioport.h>	/* check_region, request_region	*/
#include <linux/delay.h>	/* udelay			*/
#include <asm/io.h>		/* outb, outb_p			*/
#include <asm/uaccess.h>	/* copy to/from user		*/
#include <linux/videodev.h>	/* kernel radio structs		*/
#include <linux/config.h>	/* CONFIG_RADIO_GEMTEK_PORT 	*/
#include <linux/spinlock.h>

#ifndef CONFIG_RADIO_GEMTEK_PORT
#define CONFIG_RADIO_GEMTEK_PORT -1
#endif

static int io = CONFIG_RADIO_GEMTEK_PORT; 
static int users = 0;
static spinlock_t lock;

struct gemtek_device
{
	int port;
	unsigned long curfreq;
	int muted;
};


/* local things */

/* the correct way to mute the gemtek may be to write the last written
 * frequency || 0x10, but just writing 0x10 once seems to do it as well
 */
static void gemtek_mute(struct gemtek_device *dev)
{
        if(dev->muted)
		return;
	spin_lock(&lock);
	outb(0x10, io);
	spin_unlock(&lock);
	dev->muted = 1;
}

static void gemtek_unmute(struct gemtek_device *dev)
{
	if(dev->muted == 0)
		return;
	spin_lock(&lock);
	outb(0x20, io);
	spin_unlock(&lock);
	dev->muted = 0;
}

static void zero(void)
{
	outb_p(0x04, io);
	udelay(5);
	outb_p(0x05, io);
	udelay(5);
}

static void one(void)
{
	outb_p(0x06, io);
	udelay(5);
	outb_p(0x07, io);
	udelay(5);
}

static int gemtek_setfreq(struct gemtek_device *dev, unsigned long freq)
{
	int i;

/*        freq = 78.25*((float)freq/16000.0 + 10.52); */

	freq /= 16;
	freq += 10520;
	freq *= 7825;
	freq /= 100000;

	spin_lock(&lock);
	
	/* 2 start bits */
	outb_p(0x03, io);
	udelay(5);
	outb_p(0x07, io);
	udelay(5);

        /* 28 frequency bits (lsb first) */
	for (i = 0; i < 14; i++)
		if (freq & (1 << i))
			one();
		else
			zero();
        /* 36 unknown bits */
	for (i = 0; i < 11; i++)
		zero();
	one();
	for (i = 0; i < 4; i++)
		zero();
	one();
	zero();

	/* 2 end bits */
	outb_p(0x03, io);
	udelay(5);
	outb_p(0x07, io);
	udelay(5);

	spin_unlock(&lock);
	
	return 0;
}

int gemtek_getsigstr(struct gemtek_device *dev)
{
	spin_lock(&lock);
	inb(io);
	udelay(5);
	spin_unlock(&lock);
	if (inb(io) & 8)		/* bit set = no signal present */
		return 0;
	return 1;		/* signal present */
}

static int gemtek_ioctl(struct video_device *dev, unsigned int cmd, void *arg)
{
	struct gemtek_device *rt=dev->priv;

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
			strcpy(v.name, "GemTek");
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
			v.rangelow=87*16000;
			v.rangehigh=108*16000;
			v.flags=VIDEO_TUNER_LOW;
			v.mode=VIDEO_MODE_AUTO;
			v.signal=0xFFFF*gemtek_getsigstr(rt);
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
			if(v.tuner!=0)
				return -EINVAL;
			/* Only 1 tuner so no setting needed ! */
			return 0;
		}
		case VIDIOCGFREQ:
			if(copy_to_user(arg, &rt->curfreq, sizeof(rt->curfreq)))
				return -EFAULT;
			return 0;
		case VIDIOCSFREQ:
			if(copy_from_user(&rt->curfreq, arg,sizeof(rt->curfreq)))
				return -EFAULT;
		/* needs to be called twice in order for getsigstr to work */
			gemtek_setfreq(rt, rt->curfreq);
			gemtek_setfreq(rt, rt->curfreq);
			return 0;
		case VIDIOCGAUDIO:
		{	
			struct video_audio v;
			memset(&v,0, sizeof(v));
			v.flags|=VIDEO_AUDIO_MUTABLE;
			v.volume=1;
			v.step=65535;
			strcpy(v.name, "Radio");
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

			if(v.flags&VIDEO_AUDIO_MUTE) 
				gemtek_mute(rt);
			else
			        gemtek_unmute(rt);

			return 0;
		}
		default:
			return -ENOIOCTLCMD;
	}
}

static int gemtek_open(struct video_device *dev, int flags)
{
	if(users)
		return -EBUSY;
	users++;
	MOD_INC_USE_COUNT;
	return 0;
}

static void gemtek_close(struct video_device *dev)
{
	users--;
	MOD_DEC_USE_COUNT;
}

static struct gemtek_device gemtek_unit;

static struct video_device gemtek_radio=
{
	name:		"GemTek radio",
	type:		VID_TYPE_TUNER,
	hardware:	VID_HARDWARE_GEMTEK,
	open:		gemtek_open,
	close:		gemtek_close,
	ioctl:		gemtek_ioctl,
};

static int __init gemtek_init(void)
{
	if(io==-1)
	{
		printk(KERN_ERR "You must set an I/O address with io=0x20c, io=0x30c, io=0x24c or io=0x34c (io=0x020c or io=0x248 for the combined sound/radiocard)\n");
		return -EINVAL;
	}

	if (!request_region(io, 4, "gemtek")) 
	{
		printk(KERN_ERR "gemtek: port 0x%x already in use\n", io);
		return -EBUSY;
	}

	gemtek_radio.priv=&gemtek_unit;
	
	if(video_register_device(&gemtek_radio, VFL_TYPE_RADIO)==-1)
	{
		release_region(io, 4);
		return -EINVAL;
	}
	printk(KERN_INFO "GemTek Radio Card driver.\n");

	spin_lock_init(&lock);
 	/* mute card - prevents noisy bootups */
	outb(0x10, io);
	udelay(5);
	gemtek_unit.muted = 1;

	/* this is _maybe_ unnecessary */
	outb(0x01, io);

	return 0;
}

MODULE_AUTHOR("Jonas Munsin");
MODULE_DESCRIPTION("A driver for the GemTek Radio Card");
MODULE_PARM(io, "i");
MODULE_PARM_DESC(io, "I/O address of the GemTek card (0x20c, 0x30c, 0x24c or 0x34c (0x20c or 0x248 have been reported to work for the combined sound/radiocard)).");

EXPORT_NO_SYMBOLS;

static void __exit gemtek_cleanup(void)
{
	video_unregister_device(&gemtek_radio);
	release_region(io,4);
}

module_init(gemtek_init);
module_exit(gemtek_cleanup);

/*
  Local variables:
  compile-command: "gcc -c -DMODVERSIONS -D__KERNEL__ -DMODULE -O6 -Wall -Wstrict-prototypes -I /home/blp/tmp/linux-2.1.111-rtrack/include radio-rtrack2.c"
  End:
*/
