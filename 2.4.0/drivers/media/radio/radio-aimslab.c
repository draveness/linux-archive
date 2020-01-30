/* radiotrack (radioreveal) driver for Linux radio support
 * (c) 1997 M. Kirkwood
 * Coverted to new API by Alan Cox <Alan.Cox@linux.org>
 * Various bugfixes and enhancements by Russell Kroll <rkroll@exploits.org>
 *
 * History:
 * 1999-02-24	Russell Kroll <rkroll@exploits.org>
 * 		Fine tuning/VIDEO_TUNER_LOW
 *		Frequency range expanded to start at 87 MHz
 *
 * TODO: Allow for more than one of these foolish entities :-)
 *
 * Notes on the hardware (reverse engineered from other peoples'
 * reverse engineering of AIMS' code :-)
 *
 *  Frequency control is done digitally -- ie out(port,encodefreq(95.8));
 *
 *  The signal strength query is unsurprisingly inaccurate.  And it seems
 *  to indicate that (on my card, at least) the frequency setting isn't
 *  too great.  (I have to tune up .025MHz from what the freq should be
 *  to get a report that the thing is tuned.)
 *
 *  Volume control is (ugh) analogue:
 *   out(port, start_increasing_volume);
 *   wait(a_wee_while);
 *   out(port, stop_changing_the_volume);
 *  
 */

#include <linux/module.h>	/* Modules 			*/
#include <linux/init.h>		/* Initdata			*/
#include <linux/ioport.h>	/* check_region, request_region	*/
#include <linux/delay.h>	/* udelay			*/
#include <asm/io.h>		/* outb, outb_p			*/
#include <asm/uaccess.h>	/* copy to/from user		*/
#include <linux/videodev.h>	/* kernel radio structs		*/
#include <linux/config.h>	/* CONFIG_RADIO_RTRACK_PORT 	*/
#include <asm/semaphore.h>	/* Lock for the I/O 		*/

#ifndef CONFIG_RADIO_RTRACK_PORT
#define CONFIG_RADIO_RTRACK_PORT -1
#endif

static int io = CONFIG_RADIO_RTRACK_PORT; 
static int users = 0;
static struct semaphore lock;

struct rt_device
{
	int port;
	int curvol;
	unsigned long curfreq;
	int muted;
};


/* local things */

static void sleep_delay(long n)
{
	/* Sleep nicely for 'n' uS */
	int d=n/(1000000/HZ);
	if(!d)
		udelay(n);
	else
	{
		/* Yield CPU time */
		unsigned long x=jiffies;
		while((jiffies-x)<=d)
			schedule();
	}
}

static void rt_decvol(void)
{
	outb(0x58, io);		/* volume down + sigstr + on	*/
	sleep_delay(100000);
	outb(0xd8, io);		/* volume steady + sigstr + on	*/
}

static void rt_incvol(void)
{
	outb(0x98, io);		/* volume up + sigstr + on	*/
	sleep_delay(100000);
	outb(0xd8, io);		/* volume steady + sigstr + on	*/
}

static void rt_mute(struct rt_device *dev)
{
	dev->muted = 1;
	down(&lock);
	outb(0xd0, io);			/* volume steady, off		*/
	up(&lock);
}

static int rt_setvol(struct rt_device *dev, int vol)
{
	int i;

	down(&lock);
	
	if(vol == dev->curvol) {	/* requested volume = current */
		if (dev->muted) {	/* user is unmuting the card  */
			dev->muted = 0;
			outb (0xd8, io);	/* enable card */
		}	
		up(&lock);
		return 0;
	}

	if(vol == 0) {			/* volume = 0 means mute the card */
		outb(0x48, io);		/* volume down but still "on"	*/
		sleep_delay(2000000);	/* make sure it's totally down	*/
		outb(0xd0, io);		/* volume steady, off		*/
		dev->curvol = 0;	/* track the volume state!	*/
		up(&lock);
		return 0;
	}

	dev->muted = 0;
	if(vol > dev->curvol)
		for(i = dev->curvol; i < vol; i++) 
			rt_incvol();
	else
		for(i = dev->curvol; i > vol; i--) 
			rt_decvol();

	dev->curvol = vol;
	up(&lock);
	return 0;
}

/* the 128+64 on these outb's is to keep the volume stable while tuning 
 * without them, the volume _will_ creep up with each frequency change
 * and bit 4 (+16) is to keep the signal strength meter enabled
 */

void send_0_byte(int port, struct rt_device *dev)
{
	if ((dev->curvol == 0) || (dev->muted)) {
		outb_p(128+64+16+  1, port);   /* wr-enable + data low */
		outb_p(128+64+16+2+1, port);   /* clock */
	}
	else {
		outb_p(128+64+16+8+  1, port);  /* on + wr-enable + data low */
		outb_p(128+64+16+8+2+1, port);  /* clock */
	}
	sleep_delay(1000); 
}

void send_1_byte(int port, struct rt_device *dev)
{
	if ((dev->curvol == 0) || (dev->muted)) {
		outb_p(128+64+16+4  +1, port);   /* wr-enable+data high */
		outb_p(128+64+16+4+2+1, port);   /* clock */
	} 
	else {
		outb_p(128+64+16+8+4  +1, port); /* on+wr-enable+data high */
		outb_p(128+64+16+8+4+2+1, port); /* clock */
	}

	sleep_delay(1000); 
}

static int rt_setfreq(struct rt_device *dev, unsigned long freq)
{
	int i;

	/* adapted from radio-aztech.c */

	/* now uses VIDEO_TUNER_LOW for fine tuning */

	freq += 171200;			/* Add 10.7 MHz IF 		*/
	freq /= 800;			/* Convert to 50 kHz units	*/
	
	down(&lock);			/* Stop other ops interfering */
	 
	send_0_byte (io, dev);		/*  0: LSB of frequency		*/

	for (i = 0; i < 13; i++)	/*   : frequency bits (1-13)	*/
		if (freq & (1 << i))
			send_1_byte (io, dev);
		else
			send_0_byte (io, dev);

	send_0_byte (io, dev);		/* 14: test bit - always 0    */
	send_0_byte (io, dev);		/* 15: test bit - always 0    */

	send_0_byte (io, dev);		/* 16: band data 0 - always 0 */
	send_0_byte (io, dev);		/* 17: band data 1 - always 0 */
	send_0_byte (io, dev);		/* 18: band data 2 - always 0 */
	send_0_byte (io, dev);		/* 19: time base - always 0   */

	send_0_byte (io, dev);		/* 20: spacing (0 = 25 kHz)   */
	send_1_byte (io, dev);		/* 21: spacing (1 = 25 kHz)   */
	send_0_byte (io, dev);		/* 22: spacing (0 = 25 kHz)   */
	send_1_byte (io, dev);		/* 23: AM/FM (FM = 1, always) */

	if ((dev->curvol == 0) || (dev->muted))
		outb (0xd0, io);	/* volume steady + sigstr */
	else
		outb (0xd8, io);	/* volume steady + sigstr + on */
		
	up(&lock);

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
			strcpy(v.name, "RadioTrack");
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
			v.rangelow=(87*16000);
			v.rangehigh=(108*16000);
			v.flags=VIDEO_TUNER_LOW;
			v.mode=VIDEO_MODE_AUTO;
			strcpy(v.name, "FM");
			v.signal=0xFFFF*rt_getsigstr(rt);
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
			v.flags|=VIDEO_AUDIO_MUTABLE|VIDEO_AUDIO_VOLUME;
			v.volume=rt->curvol * 6554;
			v.step=6554;
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
				rt_setvol(rt,v.volume/6554);	

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

static struct rt_device rtrack_unit;

static struct video_device rtrack_radio=
{
	name:		"RadioTrack radio",
	type:		VID_TYPE_TUNER,
	hardware:	VID_HARDWARE_RTRACK,
	open:		rt_open,
	close:		rt_close,
	ioctl:		rt_ioctl,
};

static int __init rtrack_init(void)
{
	if(io==-1)
	{
		printk(KERN_ERR "You must set an I/O address with io=0x???\n");
		return -EINVAL;
	}

	if (!request_region(io, 2, "rtrack")) 
	{
		printk(KERN_ERR "rtrack: port 0x%x already in use\n", io);
		return -EBUSY;
	}

	rtrack_radio.priv=&rtrack_unit;
	
	if(video_register_device(&rtrack_radio, VFL_TYPE_RADIO)==-1)
	{
		release_region(io, 2);
		return -EINVAL;
	}
	printk(KERN_INFO "AIMSlab RadioTrack/RadioReveal card driver.\n");

	/* Set up the I/O locking */
	
	init_MUTEX(&lock);
	
 	/* mute card - prevents noisy bootups */

	/* this ensures that the volume is all the way down  */
	outb(0x48, io);		/* volume down but still "on"	*/
	sleep_delay(2000000);	/* make sure it's totally down	*/
	outb(0xc0, io);		/* steady volume, mute card	*/
	rtrack_unit.curvol = 0;

	return 0;
}

MODULE_AUTHOR("M.Kirkwood");
MODULE_DESCRIPTION("A driver for the RadioTrack/RadioReveal radio card.");
MODULE_PARM(io, "i");
MODULE_PARM_DESC(io, "I/O address of the RadioTrack card (0x20f or 0x30f)");

EXPORT_NO_SYMBOLS;

static void __exit cleanup_rtrack_module(void)
{
	video_unregister_device(&rtrack_radio);
	release_region(io,2);
}

module_init(rtrack_init);
module_exit(cleanup_rtrack_module);

