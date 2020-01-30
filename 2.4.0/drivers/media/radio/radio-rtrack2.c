/* RadioTrack II driver for Linux radio support (C) 1998 Ben Pfaff
 * 
 * Based on RadioTrack I/RadioReveal (C) 1997 M. Kirkwood
 * Coverted to new API by Alan Cox <Alan.Cox@linux.org>
 * Various bugfixes and enhancements by Russell Kroll <rkroll@exploits.org>
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
#include <linux/config.h>	/* CONFIG_RADIO_RTRACK2_PORT 	*/
#include <linux/spinlock.h>

#ifndef CONFIG_RADIO_RTRACK2_PORT
#define CONFIG_RADIO_RTRACK2_PORT -1
#endif

static int io = CONFIG_RADIO_RTRACK2_PORT; 
static int users = 0;
static spinlock_t lock;

struct rt_device
{
	int port;
	unsigned long curfreq;
	int muted;
};


/* local things */

static void rt_mute(struct rt_device *dev)
{
        if(dev->muted)
		return;
	spin_lock(&lock);
	outb(1, io);
	spin_unlock(&lock);
	dev->muted = 1;
}

static void rt_unmute(struct rt_device *dev)
{
	if(dev->muted == 0)
		return;
	spin_lock(&lock);
	outb(0, io);
	spin_unlock(&lock);
	dev->muted = 0;
}

static void zero(void)
{
        outb_p(1, io);
	outb_p(3, io);
	outb_p(1, io);
}

static void one(void)
{
        outb_p(5, io);
	outb_p(7, io);
	outb_p(5, io);
}

static int rt_setfreq(struct rt_device *dev, unsigned long freq)
{
	int i;

	freq = freq / 200 + 856;
	
	spin_lock(&lock);

	outb_p(0xc8, io);
	outb_p(0xc9, io);
	outb_p(0xc9, io);

	for (i = 0; i < 10; i++)
		zero ();

	for (i = 14; i >= 0; i--)
		if (freq & (1 << i))
			one ();
		else
			zero ();

	outb_p(0xc8, io);
	if (!dev->muted)
		outb_p(0, io);
		
	spin_unlock(&lock);
	return 0;
}

static int rt_getsigstr(struct rt_device *dev)
{
	if (inb(io) & 2)	/* bit set = no signal present	*/
		return 0;
	return 1;		/* signal present		*/
}

static int rt_ioctl(struct video_device *dev, unsigned int cmd, void *arg)
{
	struct rt_device *rt=dev->priv;

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
			strcpy(v.name, "RadioTrack II");
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
			v.rangelow=88*16000;
			v.rangehigh=108*16000;
			v.flags=VIDEO_TUNER_LOW;
			v.mode=VIDEO_MODE_AUTO;
			v.signal=0xFFFF*rt_getsigstr(rt);
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
			rt_setfreq(rt, rt->curfreq);
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
				rt_mute(rt);
			else
			        rt_unmute(rt);

			return 0;
		}
		default:
			return -ENOIOCTLCMD;
	}
}

static int rt_open(struct video_device *dev, int flags)
{
	if(users)
		return -EBUSY;
	users++;
	MOD_INC_USE_COUNT;
	return 0;
}

static void rt_close(struct video_device *dev)
{
	users--;
	MOD_DEC_USE_COUNT;
}

static struct rt_device rtrack2_unit;

static struct video_device rtrack2_radio=
{
	name:		"RadioTrack II radio",
	type:		VID_TYPE_TUNER,
	hardware:	VID_HARDWARE_RTRACK2,
	open:		rt_open,
	close:		rt_close,
	ioctl:		rt_ioctl,
};

static int __init rtrack2_init(void)
{
	if(io==-1)
	{
		printk(KERN_ERR "You must set an I/O address with io=0x20c or io=0x30c\n");
		return -EINVAL;
	}
	if (!request_region(io, 4, "rtrack2")) 
	{
		printk(KERN_ERR "rtrack2: port 0x%x already in use\n", io);
		return -EBUSY;
	}

	rtrack2_radio.priv=&rtrack2_unit;

	spin_lock_init(&lock);	
	if(video_register_device(&rtrack2_radio, VFL_TYPE_RADIO)==-1)
	{
		release_region(io, 4);
		return -EINVAL;
	}
		
	printk(KERN_INFO "AIMSlab Radiotrack II card driver.\n");

 	/* mute card - prevents noisy bootups */
	outb(1, io);
	rtrack2_unit.muted = 1;

	return 0;
}

MODULE_AUTHOR("Ben Pfaff");
MODULE_DESCRIPTION("A driver for the RadioTrack II radio card.");
MODULE_PARM(io, "i");
MODULE_PARM_DESC(io, "I/O address of the RadioTrack card (0x20c or 0x30c)");

EXPORT_NO_SYMBOLS;

static void __exit rtrack2_cleanup_module(void)
{
	video_unregister_device(&rtrack2_radio);
	release_region(io,4);
}

module_init(rtrack2_init);
module_exit(rtrack2_cleanup_module);

/*
  Local variables:
  compile-command: "gcc -c -DMODVERSIONS -D__KERNEL__ -DMODULE -O6 -Wall -Wstrict-prototypes -I /home/blp/tmp/linux-2.1.111-rtrack/include radio-rtrack2.c"
  End:
*/
