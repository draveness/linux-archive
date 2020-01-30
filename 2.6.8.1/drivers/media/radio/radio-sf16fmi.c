/* SF16FMI radio driver for Linux radio support
 * heavily based on rtrack driver...
 * (c) 1997 M. Kirkwood
 * (c) 1998 Petr Vandrovec, vandrove@vc.cvut.cz
 *
 * Fitted to new interface by Alan Cox <alan.cox@linux.org>
 * Made working and cleaned up functions <mikael.hedin@irf.se>
 * Support for ISAPnP by Ladislav Michl <ladis@psi.cz>
 *
 * Notes on the hardware
 *
 *  Frequency control is done digitally -- ie out(port,encodefreq(95.8));
 *  No volume control - only mute/unmute - you have to use line volume
 *  control on SB-part of SF16FMI
 *  
 */

#include <linux/kernel.h>	/* __setup			*/
#include <linux/module.h>	/* Modules 			*/
#include <linux/init.h>		/* Initdata			*/
#include <linux/ioport.h>	/* check_region, request_region	*/
#include <linux/delay.h>	/* udelay			*/
#include <linux/videodev.h>	/* kernel radio structs		*/
#include <linux/isapnp.h>
#include <asm/io.h>		/* outb, outb_p			*/
#include <asm/uaccess.h>	/* copy to/from user		*/
#include <asm/semaphore.h>

struct fmi_device
{
	int port;
        int curvol; /* 1 or 0 */
        unsigned long curfreq; /* freq in kHz */
        __u32 flags;
};

static int io = -1; 
static int radio_nr = -1;
static struct pnp_dev *dev = NULL;
static struct semaphore lock;

/* freq is in 1/16 kHz to internal number, hw precision is 50 kHz */
/* It is only useful to give freq in intervall of 800 (=0.05Mhz),
 * other bits will be truncated, e.g 92.7400016 -> 92.7, but 
 * 92.7400017 -> 92.75
 */
#define RSF16_ENCODE(x)	((x)/800+214)
#define RSF16_MINFREQ 87*16000
#define RSF16_MAXFREQ 108*16000

static void outbits(int bits, unsigned int data, int port)
{
	while(bits--) {
 		if(data & 1) {
			outb(5, port);
			udelay(6);
			outb(7, port);
			udelay(6);
		} else {
			outb(1, port);
			udelay(6);
			outb(3, port);
			udelay(6);
		}
		data>>=1;
	}
}

static inline void fmi_mute(int port)
{
	down(&lock);
	outb(0x00, port);
	up(&lock);
}

static inline void fmi_unmute(int port)
{
	down(&lock);
	outb(0x08, port);
	up(&lock);
}

static inline int fmi_setfreq(struct fmi_device *dev)
{
	int myport = dev->port;
	unsigned long freq = dev->curfreq;

	down(&lock);

	outbits(16, RSF16_ENCODE(freq), myport);
	outbits(8, 0xC0, myport);
	current->state = TASK_UNINTERRUPTIBLE;
	schedule_timeout(HZ/7);
	up(&lock);
	if (dev->curvol) fmi_unmute(myport);
	return 0;
}

static inline int fmi_getsigstr(struct fmi_device *dev)
{
	int val;
	int res;
	int myport = dev->port;

	
	down(&lock);
	val = dev->curvol ? 0x08 : 0x00;	/* unmute/mute */
	outb(val, myport);
	outb(val | 0x10, myport);
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(HZ/7);
	res = (int)inb(myport+1);
	outb(val, myport);
	
	up(&lock);
	return (res & 2) ? 0 : 0xFFFF;
}

static int fmi_do_ioctl(struct inode *inode, struct file *file,
			unsigned int cmd, void *arg)
{
	struct video_device *dev = video_devdata(file);
	struct fmi_device *fmi=dev->priv;
	
	switch(cmd)
	{
		case VIDIOCGCAP:
		{
			struct video_capability *v = arg;
			memset(v,0,sizeof(*v));
			strcpy(v->name, "SF16-FMx radio");
			v->type=VID_TYPE_TUNER;
			v->channels=1;
			v->audios=1;
			return 0;
		}
		case VIDIOCGTUNER:
		{
			struct video_tuner *v = arg;
			int mult;

			if(v->tuner)	/* Only 1 tuner */
				return -EINVAL;
			strcpy(v->name, "FM");
			mult = (fmi->flags & VIDEO_TUNER_LOW) ? 1 : 1000;
			v->rangelow = RSF16_MINFREQ/mult;
			v->rangehigh = RSF16_MAXFREQ/mult;
			v->flags=fmi->flags;
			v->mode=VIDEO_MODE_AUTO;
			v->signal = fmi_getsigstr(fmi);
			return 0;
		}
		case VIDIOCSTUNER:
		{
			struct video_tuner *v = arg;
			if(v->tuner!=0)
				return -EINVAL;
			fmi->flags = v->flags & VIDEO_TUNER_LOW;
			/* Only 1 tuner so no setting needed ! */
			return 0;
		}
		case VIDIOCGFREQ:
		{
			unsigned long *freq = arg;
			*freq = fmi->curfreq;
			if (!(fmi->flags & VIDEO_TUNER_LOW))
			    *freq /= 1000;
			return 0;
		}
		case VIDIOCSFREQ:
		{
			unsigned long *freq = arg;
			if (!(fmi->flags & VIDEO_TUNER_LOW))
				*freq *= 1000;
			if (*freq < RSF16_MINFREQ || *freq > RSF16_MAXFREQ )
				return -EINVAL;
			/*rounding in steps of 800 to match th freq
			  that will be used */
			fmi->curfreq = (*freq/800)*800; 
			fmi_setfreq(fmi);
			return 0;
		}
		case VIDIOCGAUDIO:
		{	
			struct video_audio *v = arg;
			memset(v,0,sizeof(*v));
			v->flags=( (!fmi->curvol)*VIDEO_AUDIO_MUTE | VIDEO_AUDIO_MUTABLE);
			strcpy(v->name, "Radio");
			v->mode=VIDEO_SOUND_STEREO;
			return 0;			
		}
		case VIDIOCSAUDIO:
		{
			struct video_audio *v = arg;
			if(v->audio)
				return -EINVAL;
			fmi->curvol= v->flags&VIDEO_AUDIO_MUTE ? 0 : 1;
			fmi->curvol ? 
				fmi_unmute(fmi->port) : fmi_mute(fmi->port);
			return 0;
		}
	        case VIDIOCGUNIT:
		{
               		struct video_unit *v = arg;
			v->video=VIDEO_NO_UNIT;
			v->vbi=VIDEO_NO_UNIT;
			v->radio=dev->minor;
			v->audio=0; /* How do we find out this??? */
			v->teletext=VIDEO_NO_UNIT;
			return 0;			
		}
		default:
			return -ENOIOCTLCMD;
	}
}

static int fmi_ioctl(struct inode *inode, struct file *file,
		     unsigned int cmd, unsigned long arg)
{
	return video_usercopy(inode, file, cmd, arg, fmi_do_ioctl);
}

static struct fmi_device fmi_unit;

static struct file_operations fmi_fops = {
	.owner		= THIS_MODULE,
	.open           = video_exclusive_open,
	.release        = video_exclusive_release,
	.ioctl		= fmi_ioctl,
	.llseek         = no_llseek,
};

static struct video_device fmi_radio=
{
	.owner		= THIS_MODULE,
	.name		= "SF16FMx radio",
	.type		= VID_TYPE_TUNER,
	.hardware	= VID_HARDWARE_SF16MI,
	.fops           = &fmi_fops,
};

/* ladis: this is my card. does any other types exist? */
static struct isapnp_device_id id_table[] __devinitdata = {
	{	ISAPNP_ANY_ID, ISAPNP_ANY_ID,
		ISAPNP_VENDOR('M','F','R'), ISAPNP_FUNCTION(0xad10), 0},
	{	ISAPNP_CARD_END, },
};

MODULE_DEVICE_TABLE(isapnp, id_table);

static int isapnp_fmi_probe(void)
{
	int i = 0;

	while (id_table[i].card_vendor != 0 && dev == NULL) {
		dev = pnp_find_dev(NULL, id_table[i].vendor,
				   id_table[i].function, NULL);
		i++;
	}

	if (!dev)
		return -ENODEV;
	if (pnp_device_attach(dev) < 0)
		return -EAGAIN;
	if (pnp_activate_dev(dev) < 0) {
		printk ("radio-sf16fmi: PnP configure failed (out of resources?)\n");
		pnp_device_detach(dev);
		return -ENOMEM;
	}
	if (!pnp_port_valid(dev, 0)) {
		pnp_device_detach(dev);
		return -ENODEV;
	}

	i = pnp_port_start(dev, 0);
	printk ("radio-sf16fmi: PnP reports card at %#x\n", i);

	return i;
}

static int __init fmi_init(void)
{
	if (io < 0)
		io = isapnp_fmi_probe();
	if (io < 0) {
		printk(KERN_ERR "radio-sf16fmi: No PnP card found.\n");
		return io;
	}
	if (!request_region(io, 2, "radio-sf16fmi")) {
		printk(KERN_ERR "radio-sf16fmi: port 0x%x already in use\n", io);
		return -EBUSY;
	}

	fmi_unit.port = io;
	fmi_unit.curvol = 0;
	fmi_unit.curfreq = 0;
	fmi_unit.flags = VIDEO_TUNER_LOW;
	fmi_radio.priv = &fmi_unit;
	
	init_MUTEX(&lock);
	
	if (video_register_device(&fmi_radio, VFL_TYPE_RADIO, radio_nr) == -1) {
		release_region(io, 2);
		return -EINVAL;
	}
		
	printk(KERN_INFO "SF16FMx radio card driver at 0x%x\n", io);
	/* mute card - prevents noisy bootups */
	fmi_mute(io);
	return 0;
}

MODULE_AUTHOR("Petr Vandrovec, vandrove@vc.cvut.cz and M. Kirkwood");
MODULE_DESCRIPTION("A driver for the SF16MI radio.");
MODULE_LICENSE("GPL");

MODULE_PARM(io, "i");
MODULE_PARM_DESC(io, "I/O address of the SF16MI card (0x284 or 0x384)");
MODULE_PARM(radio_nr, "i");

static void __exit fmi_cleanup_module(void)
{
	video_unregister_device(&fmi_radio);
	release_region(io, 2);
	if (dev)
		pnp_device_detach(dev);
}

module_init(fmi_init);
module_exit(fmi_cleanup_module);

#ifndef MODULE
static int __init fmi_setup_io(char *str)
{
	get_option(&str, &io);
	return 1;
}

__setup("sf16fm=", fmi_setup_io);
#endif
