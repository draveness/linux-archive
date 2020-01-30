/* radiotrack (radioreveal) driver for Linux radio support
 * (c) 1997 M. Kirkwood
 * Converted to V4L2 API by Mauro Carvalho Chehab <mchehab@infradead.org>
 * Converted to new API by Alan Cox <Alan.Cox@linux.org>
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
#include <linux/ioport.h>	/* request_region		*/
#include <linux/delay.h>	/* udelay			*/
#include <asm/io.h>		/* outb, outb_p			*/
#include <asm/uaccess.h>	/* copy to/from user		*/
#include <linux/videodev2.h>	/* kernel radio structs		*/
#include <media/v4l2-common.h>
#include <asm/semaphore.h>	/* Lock for the I/O 		*/

#include <linux/version.h>	/* for KERNEL_VERSION MACRO	*/
#define RADIO_VERSION KERNEL_VERSION(0,0,2)

#ifndef CONFIG_RADIO_RTRACK_PORT
#define CONFIG_RADIO_RTRACK_PORT -1
#endif

static int io = CONFIG_RADIO_RTRACK_PORT;
static int radio_nr = -1;
static struct mutex lock;

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
	int d=n/msecs_to_jiffies(1000);
	if(!d)
		udelay(n);
	else
		msleep(jiffies_to_msecs(d));
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
	mutex_lock(&lock);
	outb(0xd0, io);			/* volume steady, off		*/
	mutex_unlock(&lock);
}

static int rt_setvol(struct rt_device *dev, int vol)
{
	int i;

	mutex_lock(&lock);

	if(vol == dev->curvol) {	/* requested volume = current */
		if (dev->muted) {	/* user is unmuting the card  */
			dev->muted = 0;
			outb (0xd8, io);	/* enable card */
		}
		mutex_unlock(&lock);
		return 0;
	}

	if(vol == 0) {			/* volume = 0 means mute the card */
		outb(0x48, io);		/* volume down but still "on"	*/
		sleep_delay(2000000);	/* make sure it's totally down	*/
		outb(0xd0, io);		/* volume steady, off		*/
		dev->curvol = 0;	/* track the volume state!	*/
		mutex_unlock(&lock);
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
	mutex_unlock(&lock);
	return 0;
}

/* the 128+64 on these outb's is to keep the volume stable while tuning
 * without them, the volume _will_ creep up with each frequency change
 * and bit 4 (+16) is to keep the signal strength meter enabled
 */

static void send_0_byte(int port, struct rt_device *dev)
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

static void send_1_byte(int port, struct rt_device *dev)
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

	mutex_lock(&lock);			/* Stop other ops interfering */

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

	mutex_unlock(&lock);

	return 0;
}

static int rt_getsigstr(struct rt_device *dev)
{
	if (inb(io) & 2)	/* bit set = no signal present	*/
		return 0;
	return 1;		/* signal present		*/
}

static struct v4l2_queryctrl radio_qctrl[] = {
	{
		.id            = V4L2_CID_AUDIO_MUTE,
		.name          = "Mute",
		.minimum       = 0,
		.maximum       = 1,
		.default_value = 1,
		.type          = V4L2_CTRL_TYPE_BOOLEAN,
	},{
		.id            = V4L2_CID_AUDIO_VOLUME,
		.name          = "Volume",
		.minimum       = 0,
		.maximum       = 0xff,
		.step          = 1,
		.default_value = 0xff,
		.type          = V4L2_CTRL_TYPE_INTEGER,
	}
};

static int vidioc_querycap(struct file *file, void  *priv,
					struct v4l2_capability *v)
{
	strlcpy(v->driver, "radio-aimslab", sizeof(v->driver));
	strlcpy(v->card, "RadioTrack", sizeof(v->card));
	sprintf(v->bus_info, "ISA");
	v->version = RADIO_VERSION;
	v->capabilities = V4L2_CAP_TUNER;
	return 0;
}

static int vidioc_g_tuner(struct file *file, void *priv,
					struct v4l2_tuner *v)
{
	struct video_device *dev = video_devdata(file);
	struct rt_device *rt = dev->priv;

	if (v->index > 0)
		return -EINVAL;

	strcpy(v->name, "FM");
	v->type = V4L2_TUNER_RADIO;
	v->rangelow = (87*16000);
	v->rangehigh = (108*16000);
	v->rxsubchans = V4L2_TUNER_SUB_MONO;
	v->capability = V4L2_TUNER_CAP_LOW;
	v->audmode = V4L2_TUNER_MODE_MONO;
	v->signal = 0xffff*rt_getsigstr(rt);
	return 0;
}

static int vidioc_s_tuner(struct file *file, void *priv,
					struct v4l2_tuner *v)
{
	if (v->index > 0)
		return -EINVAL;
	return 0;
}

static int vidioc_s_frequency(struct file *file, void *priv,
					struct v4l2_frequency *f)
{
	struct video_device *dev = video_devdata(file);
	struct rt_device *rt = dev->priv;

	rt->curfreq = f->frequency;
	rt_setfreq(rt, rt->curfreq);
	return 0;
}

static int vidioc_g_frequency(struct file *file, void *priv,
					struct v4l2_frequency *f)
{
	struct video_device *dev = video_devdata(file);
	struct rt_device *rt = dev->priv;

	f->type = V4L2_TUNER_RADIO;
	f->frequency = rt->curfreq;
	return 0;
}

static int vidioc_queryctrl(struct file *file, void *priv,
					struct v4l2_queryctrl *qc)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(radio_qctrl); i++) {
		if (qc->id && qc->id == radio_qctrl[i].id) {
			memcpy(qc, &(radio_qctrl[i]),
						sizeof(*qc));
			return 0;
		}
	}
	return -EINVAL;
}

static int vidioc_g_ctrl(struct file *file, void *priv,
					struct v4l2_control *ctrl)
{
	struct video_device *dev = video_devdata(file);
	struct rt_device *rt = dev->priv;

	switch (ctrl->id) {
	case V4L2_CID_AUDIO_MUTE:
		ctrl->value = rt->muted;
		return 0;
	case V4L2_CID_AUDIO_VOLUME:
		ctrl->value = rt->curvol * 6554;
		return 0;
	}
	return -EINVAL;
}

static int vidioc_s_ctrl(struct file *file, void *priv,
					struct v4l2_control *ctrl)
{
	struct video_device *dev = video_devdata(file);
	struct rt_device *rt = dev->priv;

	switch (ctrl->id) {
	case V4L2_CID_AUDIO_MUTE:
		if (ctrl->value)
			rt_mute(rt);
		else
			rt_setvol(rt,rt->curvol);
		return 0;
	case V4L2_CID_AUDIO_VOLUME:
		rt_setvol(rt,ctrl->value);
		return 0;
	}
	return -EINVAL;
}

static int vidioc_g_audio (struct file *file, void *priv,
					struct v4l2_audio *a)
{
	if (a->index > 1)
		return -EINVAL;

	strcpy(a->name, "Radio");
	a->capability = V4L2_AUDCAP_STEREO;
	return 0;
}

static int vidioc_g_input(struct file *filp, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int vidioc_s_input(struct file *filp, void *priv, unsigned int i)
{
	if (i != 0)
		return -EINVAL;
	return 0;
}

static int vidioc_s_audio(struct file *file, void *priv,
					struct v4l2_audio *a)
{
	if (a->index != 0)
		return -EINVAL;
	return 0;
}

static struct rt_device rtrack_unit;

static const struct file_operations rtrack_fops = {
	.owner		= THIS_MODULE,
	.open           = video_exclusive_open,
	.release        = video_exclusive_release,
	.ioctl		= video_ioctl2,
	.compat_ioctl	= v4l_compat_ioctl32,
	.llseek         = no_llseek,
};

static struct video_device rtrack_radio=
{
	.owner		= THIS_MODULE,
	.name		= "RadioTrack radio",
	.type		= VID_TYPE_TUNER,
	.fops           = &rtrack_fops,
	.vidioc_querycap    = vidioc_querycap,
	.vidioc_g_tuner     = vidioc_g_tuner,
	.vidioc_s_tuner     = vidioc_s_tuner,
	.vidioc_g_audio     = vidioc_g_audio,
	.vidioc_s_audio     = vidioc_s_audio,
	.vidioc_g_input     = vidioc_g_input,
	.vidioc_s_input     = vidioc_s_input,
	.vidioc_g_frequency = vidioc_g_frequency,
	.vidioc_s_frequency = vidioc_s_frequency,
	.vidioc_queryctrl   = vidioc_queryctrl,
	.vidioc_g_ctrl      = vidioc_g_ctrl,
	.vidioc_s_ctrl      = vidioc_s_ctrl,
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

	if(video_register_device(&rtrack_radio, VFL_TYPE_RADIO, radio_nr)==-1)
	{
		release_region(io, 2);
		return -EINVAL;
	}
	printk(KERN_INFO "AIMSlab RadioTrack/RadioReveal card driver.\n");

	/* Set up the I/O locking */

	mutex_init(&lock);

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
MODULE_LICENSE("GPL");

module_param(io, int, 0);
MODULE_PARM_DESC(io, "I/O address of the RadioTrack card (0x20f or 0x30f)");
module_param(radio_nr, int, 0);

static void __exit cleanup_rtrack_module(void)
{
	video_unregister_device(&rtrack_radio);
	release_region(io,2);
}

module_init(rtrack_init);
module_exit(cleanup_rtrack_module);

