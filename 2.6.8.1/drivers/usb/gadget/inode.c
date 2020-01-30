/*
 * inode.c -- user mode filesystem api for usb gadget controllers
 *
 * Copyright (C) 2003 David Brownell
 * Copyright (C) 2003 Agilent Technologies
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


// #define	DEBUG 			/* data to help fault diagnosis */
// #define	VERBOSE		/* extra debug messages (success too) */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/uts.h>
#include <linux/wait.h>
#include <linux/compiler.h>
#include <asm/uaccess.h>
#include <linux/slab.h>

#include <linux/device.h>
#include <linux/moduleparam.h>

#include <linux/usb_gadgetfs.h>
#include <linux/usb_gadget.h>


/*
 * The gadgetfs API maps each endpoint to a file descriptor so that you
 * can use standard synchronous read/write calls for I/O.  There's some
 * O_NONBLOCK and O_ASYNC/FASYNC style i/o support.  Example usermode
 * drivers show how this works in practice.  You can also use AIO to
 * eliminate I/O gaps between requests, to help when streaming data.
 *
 * Key parts that must be USB-specific are protocols defining how the
 * read/write operations relate to the hardware state machines.  There
 * are two types of files.  One type is for the device, implementing ep0.
 * The other type is for each IN or OUT endpoint.  In both cases, the
 * user mode driver must configure the hardware before using it.
 *
 * - First, dev_config() is called when /dev/gadget/$CHIP is configured
 *   (by writing configuration and device descriptors).  Afterwards it
 *   may serve as a source of device events, used to handle all control
 *   requests other than basic enumeration.
 *
 * - Then either immediately, or after a SET_CONFIGURATION control request,
 *   ep_config() is called when each /dev/gadget/ep* file is configured
 *   (by writing endpoint descriptors).  Afterwards these files are used
 *   to write() IN data or to read() OUT data.  To halt the endpoint, a
 *   "wrong direction" request is issued (like reading an IN endpoint).
 *
 * Unlike "usbfs" the only ioctl()s are for things that are rare, and maybe
 * not possible on all hardware.  For example, precise fault handling with
 * respect to data left in endpoint fifos after aborted operations; or
 * selective clearing of endpoint halts, to implement SET_INTERFACE.
 */

#define	DRIVER_DESC	"USB Gadget filesystem"
#define	DRIVER_VERSION	"18 Nov 2003"

static const char driver_desc [] = DRIVER_DESC;
static const char shortname [] = "gadgetfs";

MODULE_DESCRIPTION (DRIVER_DESC);
MODULE_AUTHOR ("David Brownell");
MODULE_LICENSE ("GPL");


/*----------------------------------------------------------------------*/

#define GADGETFS_MAGIC		0xaee71ee7
#define DMA_ADDR_INVALID	(~(dma_addr_t)0)

/* /dev/gadget/$CHIP represents ep0 and the whole device */
enum ep0_state {
	/* DISBLED is the initial state.
	 */
	STATE_DEV_DISABLED = 0,

	/* Only one open() of /dev/gadget/$CHIP; only one file tracks
	 * ep0/device i/o modes and binding to the controller.  Driver
	 * must always write descriptors to initialize the device, then
	 * the device becomes UNCONNECTED until enumeration.
	 */
	STATE_OPENED,

	/* From then on, ep0 fd is in either of two basic modes:
	 * - (UN)CONNECTED: read usb_gadgetfs_event(s) from it
	 * - SETUP: read/write will transfer control data and succeed;
	 *   or if "wrong direction", performs protocol stall
	 */
	STATE_UNCONNECTED,
	STATE_CONNECTED,
	STATE_SETUP,

	/* UNBOUND means the driver closed ep0, so the device won't be
	 * accessible again (DEV_DISABLED) until all fds are closed.
	 */
	STATE_DEV_UNBOUND,
};

/* enough for the whole queue: most events invalidate others */
#define	N_EVENT			5

struct dev_data {
	spinlock_t			lock;
	atomic_t			count;
	enum ep0_state			state;
	struct usb_gadgetfs_event	event [N_EVENT];
	unsigned			ev_next;
	struct fasync_struct		*fasync;
	u8				current_config;

	/* drivers reading ep0 MUST handle control requests (SETUP)
	 * reported that way; else the host will time out.
	 */
	unsigned			usermode_setup : 1,
					setup_in : 1,
					setup_can_stall : 1,
					setup_out_ready : 1,
					setup_out_error : 1,
					setup_abort : 1;

	/* the rest is basically write-once */
	struct usb_config_descriptor	*config, *hs_config;
	struct usb_device_descriptor	*dev;
	struct usb_request		*req;
	struct usb_gadget		*gadget;
	struct list_head		epfiles;
	void				*buf;
	wait_queue_head_t		wait;
	struct super_block		*sb;
	struct dentry			*dentry;

	/* except this scratch i/o buffer for ep0 */
	u8				rbuf [256];
};

static inline void get_dev (struct dev_data *data)
{
	atomic_inc (&data->count);
}

static void put_dev (struct dev_data *data)
{
	if (likely (!atomic_dec_and_test (&data->count)))
		return;
	/* needs no more cleanup */
	BUG_ON (waitqueue_active (&data->wait));
	kfree (data);
}

static struct dev_data *dev_new (void)
{
	struct dev_data		*dev;

	dev = kmalloc (sizeof *dev, GFP_KERNEL);
	if (!dev)
		return NULL;
	memset (dev, 0, sizeof *dev);
	dev->state = STATE_DEV_DISABLED;
	atomic_set (&dev->count, 1);
	spin_lock_init (&dev->lock);
	INIT_LIST_HEAD (&dev->epfiles);
	init_waitqueue_head (&dev->wait);
	return dev;
}

/*----------------------------------------------------------------------*/

/* other /dev/gadget/$ENDPOINT files represent endpoints */
enum ep_state {
	STATE_EP_DISABLED = 0,
	STATE_EP_READY,
	STATE_EP_DEFER_ENABLE,
	STATE_EP_ENABLED,
	STATE_EP_UNBOUND,
};

struct ep_data {
	struct semaphore		lock;
	enum ep_state			state;
	atomic_t			count;
	struct dev_data			*dev;
	/* must hold dev->lock before accessing ep or req */
	struct usb_ep			*ep;
	struct usb_request		*req;
	ssize_t				status;
	char				name [16];
	struct usb_endpoint_descriptor	desc, hs_desc;
	struct list_head		epfiles;
	wait_queue_head_t		wait;
	struct dentry			*dentry;
	struct inode			*inode;
};

static inline void get_ep (struct ep_data *data)
{
	atomic_inc (&data->count);
}

static void put_ep (struct ep_data *data)
{
	if (likely (!atomic_dec_and_test (&data->count)))
		return;
	put_dev (data->dev);
	/* needs no more cleanup */
	BUG_ON (!list_empty (&data->epfiles));
	BUG_ON (waitqueue_active (&data->wait));
	BUG_ON (down_trylock (&data->lock) != 0);
	kfree (data);
}

/*----------------------------------------------------------------------*/

/* most "how to use the hardware" policy choices are in userspace:
 * mapping endpoint roles the driver needs to the capabilities that
 * the usb controller exposes.
 */

#ifdef	CONFIG_USB_GADGET_DUMMY_HCD
/* act (mostly) like a net2280 */
#define CONFIG_USB_GADGET_NET2280
#endif

#ifdef	CONFIG_USB_GADGET_NET2280
#define CHIP			"net2280"
#define HIGHSPEED
#endif

#ifdef	CONFIG_USB_GADGET_PXA2XX
#define CHIP			"pxa2xx_udc"
/* earlier hardware doesn't have UDCCFR, races set_{config,interface} */
#warning works best with pxa255 or newer
#endif

#ifdef	CONFIG_USB_GADGET_GOKU
#define CHIP			"goku_udc"
#endif

#ifdef	CONFIG_USB_GADGET_OMAP
#define CHIP			"omap_udc"
#endif

#ifdef	CONFIG_USB_GADGET_SA1100
#define CHIP			"sa1100"
#endif

/*----------------------------------------------------------------------*/

/* NOTE:  don't use dev_printk calls before binding to the gadget
 * at the end of ep0 configuration, or after unbind.
 */

/* too wordy: dev_printk(level , &(d)->gadget->dev , fmt , ## args) */
#define xprintk(d,level,fmt,args...) \
	printk(level "%s: " fmt , shortname , ## args)

#ifdef DEBUG
#define DBG(dev,fmt,args...) \
	xprintk(dev , KERN_DEBUG , fmt , ## args)
#else
#define DBG(dev,fmt,args...) \
	do { } while (0)
#endif /* DEBUG */

#ifdef VERBOSE
#define VDEBUG	DBG
#else
#define VDEBUG(dev,fmt,args...) \
	do { } while (0)
#endif /* DEBUG */

#define ERROR(dev,fmt,args...) \
	xprintk(dev , KERN_ERR , fmt , ## args)
#define WARN(dev,fmt,args...) \
	xprintk(dev , KERN_WARNING , fmt , ## args)
#define INFO(dev,fmt,args...) \
	xprintk(dev , KERN_INFO , fmt , ## args)


/*----------------------------------------------------------------------*/

/* SYNCHRONOUS ENDPOINT OPERATIONS (bulk/intr/iso)
 *
 * After opening, configure non-control endpoints.  Then use normal
 * stream read() and write() requests; and maybe ioctl() to get more
 * precise FIFO status when recovering from cancelation.
 */

static void epio_complete (struct usb_ep *ep, struct usb_request *req)
{
	struct ep_data	*epdata = ep->driver_data;

	if (!req->context)
		return;
	if (req->status)
		epdata->status = req->status;
	else
		epdata->status = req->actual;
	complete ((struct completion *)req->context);
}

/* tasklock endpoint, returning when it's connected.
 * still need dev->lock to use epdata->ep.
 */
static int
get_ready_ep (unsigned f_flags, struct ep_data *epdata)
{
	int	val;

	if (f_flags & O_NONBLOCK) {
		if (down_trylock (&epdata->lock) != 0)
			goto nonblock;
		if (epdata->state != STATE_EP_ENABLED) {
			up (&epdata->lock);
nonblock:
			val = -EAGAIN;
		} else
			val = 0;
		return val;
	}

	if ((val = down_interruptible (&epdata->lock)) < 0)
		return val;
newstate:
	switch (epdata->state) {
	case STATE_EP_ENABLED:
		break;
	case STATE_EP_DEFER_ENABLE:
		DBG (epdata->dev, "%s wait for host\n", epdata->name);
		if ((val = wait_event_interruptible (epdata->wait, 
				epdata->state != STATE_EP_DEFER_ENABLE
				|| epdata->dev->state == STATE_DEV_UNBOUND
				)) < 0)
			goto fail;
		goto newstate;
	// case STATE_EP_DISABLED:		/* "can't happen" */
	// case STATE_EP_READY:			/* "can't happen" */
	default:				/* error! */
		pr_debug ("%s: ep %p not available, state %d\n",
				shortname, epdata, epdata->state);
		// FALLTHROUGH
	case STATE_EP_UNBOUND:			/* clean disconnect */
		val = -ENODEV;
fail:
		up (&epdata->lock);
	}
	return val;
}

static ssize_t
ep_io (struct ep_data *epdata, void *buf, unsigned len)
{
	DECLARE_COMPLETION (done);
	int value;

	spin_lock_irq (&epdata->dev->lock);
	if (likely (epdata->ep != NULL)) {
		struct usb_request	*req = epdata->req;

		req->context = &done;
		req->complete = epio_complete;
		req->buf = buf;
		req->length = len;
		value = usb_ep_queue (epdata->ep, req, GFP_ATOMIC);
	} else
		value = -ENODEV;
	spin_unlock_irq (&epdata->dev->lock);

	if (likely (value == 0)) {
		value = wait_event_interruptible (done.wait, done.done);
		if (value != 0) {
			spin_lock_irq (&epdata->dev->lock);
			if (likely (epdata->ep != NULL)) {
				DBG (epdata->dev, "%s i/o interrupted\n",
						epdata->name);
				usb_ep_dequeue (epdata->ep, epdata->req);
				spin_unlock_irq (&epdata->dev->lock);

				wait_event (done.wait, done.done);
				if (epdata->status == -ECONNRESET)
					epdata->status = -EINTR;
			} else {
				spin_unlock_irq (&epdata->dev->lock);

				DBG (epdata->dev, "endpoint gone\n");
				epdata->status = -ENODEV;
			}
		}
		return epdata->status;
	}
	return value;
}


/* handle a synchronous OUT bulk/intr/iso transfer */
static ssize_t
ep_read (struct file *fd, char __user *buf, size_t len, loff_t *ptr)
{
	struct ep_data		*data = fd->private_data;
	void			*kbuf;
	ssize_t			value;

	if ((value = get_ready_ep (fd->f_flags, data)) < 0)
		return value;

	/* halt any endpoint by doing a "wrong direction" i/o call */
	if (data->desc.bEndpointAddress & USB_DIR_IN) {
		if ((data->desc.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
				== USB_ENDPOINT_XFER_ISOC)
			return -EINVAL;
		DBG (data->dev, "%s halt\n", data->name);
		spin_lock_irq (&data->dev->lock);
		if (likely (data->ep != NULL))
			usb_ep_set_halt (data->ep);
		spin_unlock_irq (&data->dev->lock);
		up (&data->lock);
		return -EBADMSG;
	}

	/* FIXME readahead for O_NONBLOCK and poll(); careful with ZLPs */

	value = -ENOMEM;
	kbuf = kmalloc (len, SLAB_KERNEL);
	if (unlikely (!kbuf))
		goto free1;

	value = ep_io (data, kbuf, len);
	VDEBUG (data->dev, "%s read %d OUT, status %d\n",
		data->name, len, value);
	if (value >= 0 && copy_to_user (buf, kbuf, value))
		value = -EFAULT;

free1:
	up (&data->lock);
	kfree (kbuf);
	return value;
}

/* handle a synchronous IN bulk/intr/iso transfer */
static ssize_t
ep_write (struct file *fd, const char __user *buf, size_t len, loff_t *ptr)
{
	struct ep_data		*data = fd->private_data;
	void			*kbuf;
	ssize_t			value;

	if ((value = get_ready_ep (fd->f_flags, data)) < 0)
		return value;

	/* halt any endpoint by doing a "wrong direction" i/o call */
	if (!(data->desc.bEndpointAddress & USB_DIR_IN)) {
		if ((data->desc.bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
				== USB_ENDPOINT_XFER_ISOC)
			return -EINVAL;
		DBG (data->dev, "%s halt\n", data->name);
		spin_lock_irq (&data->dev->lock);
		if (likely (data->ep != NULL))
			usb_ep_set_halt (data->ep);
		spin_unlock_irq (&data->dev->lock);
		up (&data->lock);
		return -EBADMSG;
	}

	/* FIXME writebehind for O_NONBLOCK and poll(), qlen = 1 */

	value = -ENOMEM;
	kbuf = kmalloc (len, SLAB_KERNEL);
	if (!kbuf)
		goto free1;
	if (copy_from_user (kbuf, buf, len)) {
		value = -EFAULT;
		goto free1;
	}

	value = ep_io (data, kbuf, len);
	VDEBUG (data->dev, "%s write %d IN, status %d\n",
		data->name, len, value);
free1:
	up (&data->lock);
	kfree (kbuf);
	return value;
}

static int
ep_release (struct inode *inode, struct file *fd)
{
	struct ep_data		*data = fd->private_data;

	/* clean up if this can be reopened */
	if (data->state != STATE_EP_UNBOUND) {
		data->state = STATE_EP_DISABLED;
		data->desc.bDescriptorType = 0;
		data->hs_desc.bDescriptorType = 0;
	}
	put_ep (data);
	return 0;
}

static int ep_ioctl (struct inode *inode, struct file *fd,
		unsigned code, unsigned long value)
{
	struct ep_data		*data = fd->private_data;
	int			status;

	if ((status = get_ready_ep (fd->f_flags, data)) < 0)
		return status;

	spin_lock_irq (&data->dev->lock);
	if (likely (data->ep != NULL)) {
		switch (code) {
		case GADGETFS_FIFO_STATUS:
			status = usb_ep_fifo_status (data->ep);
			break;
		case GADGETFS_FIFO_FLUSH:
			usb_ep_fifo_flush (data->ep);
			break;
		case GADGETFS_CLEAR_HALT:
			status = usb_ep_clear_halt (data->ep);
			break;
		default:
			status = -ENOTTY;
		}
	} else
		status = -ENODEV;
	spin_unlock_irq (&data->dev->lock);
	up (&data->lock);
	return status;
}

/*----------------------------------------------------------------------*/

/* ASYNCHRONOUS ENDPOINT I/O OPERATIONS (bulk/intr/iso) */

struct kiocb_priv {
	struct usb_request	*req;
	struct ep_data		*epdata;
	void			*buf;
	char __user		*ubuf;
	unsigned		actual;
};

static int ep_aio_cancel(struct kiocb *iocb, struct io_event *e)
{
	struct kiocb_priv	*priv = (void *) &iocb->private;
	struct ep_data		*epdata;
	int			value;

	local_irq_disable();
	epdata = priv->epdata;
	// spin_lock(&epdata->dev->lock);
	kiocbSetCancelled(iocb);
	if (likely(epdata && epdata->ep && priv->req))
		value = usb_ep_dequeue (epdata->ep, priv->req);
	else
		value = -EINVAL;
	// spin_unlock(&epdata->dev->lock);
	local_irq_enable();

	aio_put_req(iocb);
	return value;
}

static long ep_aio_read_retry(struct kiocb *iocb)
{
	struct kiocb_priv	*priv = (void *) &iocb->private;
	int			status = priv->actual;

	/* we "retry" to get the right mm context for this: */
	status = copy_to_user(priv->ubuf, priv->buf, priv->actual);
	if (unlikely(0 != status))
		status = -EFAULT;
	else
		status = priv->actual;
	kfree(priv->buf);
	aio_put_req(iocb);
	return status;
}

static void ep_aio_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct kiocb		*iocb = req->context;
	struct kiocb_priv	*priv = (void *) &iocb->private;
	struct ep_data		*epdata = priv->epdata;

	/* lock against disconnect (and ideally, cancel) */
	spin_lock(&epdata->dev->lock);
	priv->req = NULL;
	priv->epdata = NULL;
	if (NULL == iocb->ki_retry
			|| unlikely(0 == req->actual)
			|| unlikely(kiocbIsCancelled(iocb))) {
		kfree(req->buf);
		/* aio_complete() reports bytes-transferred _and_ faults */
		if (unlikely(kiocbIsCancelled(iocb)))
			aio_put_req(iocb);
		else
			aio_complete(iocb,
				req->actual ? req->actual : req->status,
				req->status);
	} else {
		/* retry() won't report both; so we hide some faults */
		if (unlikely(0 != req->status))
			DBG(epdata->dev, "%s fault %d len %d\n",
				ep->name, req->status, req->actual);

		priv->buf = req->buf;
		priv->actual = req->actual;
		kick_iocb(iocb);
	}
	spin_unlock(&epdata->dev->lock);

	usb_ep_free_request(ep, req);
	put_ep(epdata);
}

static ssize_t
ep_aio_rwtail(struct kiocb *iocb, char *buf, size_t len, struct ep_data *epdata)
{
	struct kiocb_priv	*priv = (void *) &iocb->private;
	struct usb_request	*req;
	ssize_t			value;

	value = get_ready_ep(iocb->ki_filp->f_flags, epdata);
	if (unlikely(value < 0)) {
		kfree(buf);
		return value;
	}

	iocb->ki_cancel = ep_aio_cancel;
	get_ep(epdata);
	priv->epdata = epdata;
	priv->actual = 0;

	/* each kiocb is coupled to one usb_request, but we can't
	 * allocate or submit those if the host disconnected.
	 */
	spin_lock_irq(&epdata->dev->lock);
	if (likely(epdata->ep)) {
		req = usb_ep_alloc_request(epdata->ep, GFP_ATOMIC);
		if (likely(req)) {
			priv->req = req;
			req->buf = buf;
			req->length = len;
			req->complete = ep_aio_complete;
			req->context = iocb;
			value = usb_ep_queue(epdata->ep, req, GFP_ATOMIC);
			if (unlikely(0 != value))
				usb_ep_free_request(epdata->ep, req);
		} else
			value = -EAGAIN;
	} else
		value = -ENODEV;
	spin_unlock_irq(&epdata->dev->lock);

	up(&epdata->lock);

	if (unlikely(value))
		put_ep(epdata);
	else
		value = -EIOCBQUEUED;
	return value;
}

static ssize_t
ep_aio_read(struct kiocb *iocb, char __user *ubuf, size_t len, loff_t o)
{
	struct kiocb_priv	*priv = (void *) &iocb->private;
	struct ep_data		*epdata = iocb->ki_filp->private_data;
	char			*buf;

	if (unlikely(epdata->desc.bEndpointAddress & USB_DIR_IN))
		return -EINVAL;
	buf = kmalloc(len, GFP_KERNEL);
	if (unlikely(!buf))
		return -ENOMEM;
	iocb->ki_retry = ep_aio_read_retry;
	priv->ubuf = ubuf;
	return ep_aio_rwtail(iocb, buf, len, epdata);
}

static ssize_t
ep_aio_write(struct kiocb *iocb, const char __user *ubuf, size_t len, loff_t o)
{
	struct ep_data		*epdata = iocb->ki_filp->private_data;
	char			*buf;

	if (unlikely(!(epdata->desc.bEndpointAddress & USB_DIR_IN)))
		return -EINVAL;
	buf = kmalloc(len, GFP_KERNEL);
	if (unlikely(!buf))
		return -ENOMEM;
	if (unlikely(copy_from_user(buf, ubuf, len) != 0)) {
		kfree(buf);
		return -EFAULT;
	}
	return ep_aio_rwtail(iocb, buf, len, epdata);
}

/*----------------------------------------------------------------------*/

/* used after endpoint configuration */
static struct file_operations ep_io_operations = {
	.owner =	THIS_MODULE,
	.read =		ep_read,
	.write =	ep_write,
	.ioctl =	ep_ioctl,
	.release =	ep_release,

	.aio_read =	ep_aio_read,
	.aio_write =	ep_aio_write,
};

/* ENDPOINT INITIALIZATION
 *
 *     fd = open ("/dev/gadget/$ENDPOINT", O_RDWR)
 *     status = write (fd, descriptors, sizeof descriptors)
 *
 * That write establishes the endpoint configuration, configuring
 * the controller to process bulk, interrupt, or isochronous transfers
 * at the right maxpacket size, and so on.
 *
 * The descriptors are message type 1, identified by a host order u32
 * at the beginning of what's written.  Descriptor order is: full/low
 * speed descriptor, then optional high speed descriptor.
 */
static ssize_t
ep_config (struct file *fd, const char __user *buf, size_t len, loff_t *ptr)
{
	struct ep_data		*data = fd->private_data;
	struct usb_ep		*ep;
	u32			tag;
	int			value;

	if ((value = down_interruptible (&data->lock)) < 0)
		return value;

	if (data->state != STATE_EP_READY) {
		value = -EL2HLT;
		goto fail;
	}

	value = len;
	if (len < USB_DT_ENDPOINT_SIZE + 4)
		goto fail0;

	/* we might need to change message format someday */
	if (copy_from_user (&tag, buf, 4)) {
		goto fail1;
	}
	if (tag != 1) {
		DBG(data->dev, "config %s, bad tag %d\n", data->name, tag);
		goto fail0;
	}
	buf += 4;
	len -= 4;

	/* NOTE:  audio endpoint extensions not accepted here;
	 * just don't include the extra bytes.
	 */

	/* full/low speed descriptor, then high speed */
	if (copy_from_user (&data->desc, buf, USB_DT_ENDPOINT_SIZE)) {
		goto fail1;
	}
	if (data->desc.bLength != USB_DT_ENDPOINT_SIZE
			|| data->desc.bDescriptorType != USB_DT_ENDPOINT)
		goto fail0;
	if (len != USB_DT_ENDPOINT_SIZE) {
		if (len != 2 * USB_DT_ENDPOINT_SIZE)
			goto fail0;
		if (copy_from_user (&data->hs_desc, buf + USB_DT_ENDPOINT_SIZE,
					USB_DT_ENDPOINT_SIZE)) {
			goto fail1;
		}
		if (data->hs_desc.bLength != USB_DT_ENDPOINT_SIZE
				|| data->hs_desc.bDescriptorType
					!= USB_DT_ENDPOINT) {
			DBG(data->dev, "config %s, bad hs length or type\n",
					data->name);
			goto fail0;
		}
	}
	value = len;

	spin_lock_irq (&data->dev->lock);
	if (data->dev->state == STATE_DEV_UNBOUND) {
		value = -ENOENT;
		goto gone;
	} else if ((ep = data->ep) == NULL) {
		value = -ENODEV;
		goto gone;
	}
	switch (data->dev->gadget->speed) {
	case USB_SPEED_LOW:
	case USB_SPEED_FULL:
		value = usb_ep_enable (ep, &data->desc);
		if (value == 0)
			data->state = STATE_EP_ENABLED;
		break;
#ifdef	HIGHSPEED
	case USB_SPEED_HIGH:
		/* fails if caller didn't provide that descriptor... */
		value = usb_ep_enable (ep, &data->hs_desc);
		if (value == 0)
			data->state = STATE_EP_ENABLED;
		break;
#endif
	default:
		DBG (data->dev, "unconnected, %s init deferred\n",
				data->name);
		data->state = STATE_EP_DEFER_ENABLE;
	}
	if (value == 0)
		fd->f_op = &ep_io_operations;
gone:
	spin_unlock_irq (&data->dev->lock);
	if (value < 0) {
fail:
		data->desc.bDescriptorType = 0;
		data->hs_desc.bDescriptorType = 0;
	}
	up (&data->lock);
	return value;
fail0:
	value = -EINVAL;
	goto fail;
fail1:
	value = -EFAULT;
	goto fail;
}

static int
ep_open (struct inode *inode, struct file *fd)
{
	struct ep_data		*data = inode->u.generic_ip;
	int			value = -EBUSY;

	if (down_interruptible (&data->lock) != 0)
		return -EINTR;
	spin_lock_irq (&data->dev->lock);
	if (data->dev->state == STATE_DEV_UNBOUND)
		value = -ENOENT;
	else if (data->state == STATE_EP_DISABLED) {
		value = 0;
		data->state = STATE_EP_READY;
		get_ep (data);
		fd->private_data = data;
		VDEBUG (data->dev, "%s ready\n", data->name);
	} else
		DBG (data->dev, "%s state %d\n",
			data->name, data->state);
	spin_unlock_irq (&data->dev->lock);
	up (&data->lock);
	return value;
}

/* used before endpoint configuration */
static struct file_operations ep_config_operations = {
	.owner =	THIS_MODULE,
	.open =		ep_open,
	.write =	ep_config,
	.release =	ep_release,
};

/*----------------------------------------------------------------------*/

/* EP0 IMPLEMENTATION can be partly in userspace.
 *
 * Drivers that use this facility receive various events, including
 * control requests the kernel doesn't handle.  Drivers that don't
 * use this facility may be too simple-minded for real applications.
 */

static inline void ep0_readable (struct dev_data *dev)
{
	wake_up (&dev->wait);
	kill_fasync (&dev->fasync, SIGIO, POLL_IN);
}

static void clean_req (struct usb_ep *ep, struct usb_request *req)
{
	struct dev_data		*dev = ep->driver_data;

	if (req->buf != dev->rbuf) {
		usb_ep_free_buffer (ep, req->buf, req->dma, req->length);
		req->buf = dev->rbuf;
		req->dma = DMA_ADDR_INVALID;
	}
	req->complete = epio_complete;
	dev->setup_out_ready = 0;
}

static void ep0_complete (struct usb_ep *ep, struct usb_request *req)
{
	struct dev_data		*dev = ep->driver_data;
	int			free = 1;

	/* for control OUT, data must still get to userspace */
	if (!dev->setup_in) {
		dev->setup_out_error = (req->status != 0);
		if (!dev->setup_out_error)
			free = 0;
		dev->setup_out_ready = 1;
		ep0_readable (dev);
	} else if (dev->state == STATE_SETUP)
		dev->state = STATE_CONNECTED;

	/* clean up as appropriate */
	if (free && req->buf != &dev->rbuf)
		clean_req (ep, req);
	req->complete = epio_complete;
}

static int setup_req (struct usb_ep *ep, struct usb_request *req, u16 len)
{
	struct dev_data	*dev = ep->driver_data;

	if (dev->setup_out_ready) {
		DBG (dev, "ep0 request busy!\n");
		return -EBUSY;
	}
	if (len > sizeof (dev->rbuf))
		req->buf = usb_ep_alloc_buffer (ep, len, &req->dma, GFP_ATOMIC);
	if (req->buf == 0) {
		req->buf = dev->rbuf;
		return -ENOMEM;
	}
	req->complete = ep0_complete;
	req->length = len;
	return 0;
}

static ssize_t
ep0_read (struct file *fd, char __user *buf, size_t len, loff_t *ptr)
{
	struct dev_data			*dev = fd->private_data;
	ssize_t				retval;
	enum ep0_state			state;

	spin_lock_irq (&dev->lock);

	/* report fd mode change before acting on it */
	if (dev->setup_abort) {
		dev->setup_abort = 0;
		retval = -EIDRM;
		goto done;
	}

	/* control DATA stage */
	if ((state = dev->state) == STATE_SETUP) {

		if (dev->setup_in) {		/* stall IN */
			VDEBUG(dev, "ep0in stall\n");
			(void) usb_ep_set_halt (dev->gadget->ep0);
			retval = -EL2HLT;
			dev->state = STATE_CONNECTED;

		} else if (len == 0) {		/* ack SET_CONFIGURATION etc */
			struct usb_ep		*ep = dev->gadget->ep0;
			struct usb_request	*req = dev->req;

			if ((retval = setup_req (ep, req, 0)) == 0)
				retval = usb_ep_queue (ep, req, GFP_ATOMIC);
			dev->state = STATE_CONNECTED;

		} else {			/* collect OUT data */
			if ((fd->f_flags & O_NONBLOCK) != 0
					&& !dev->setup_out_ready) {
				retval = -EAGAIN;
				goto done;
			}
			spin_unlock_irq (&dev->lock);
			retval = wait_event_interruptible (dev->wait,
					dev->setup_out_ready != 0);

			/* FIXME state could change from under us */
			spin_lock_irq (&dev->lock);
			if (retval)
				goto done;
			if (dev->setup_out_error)
				retval = -EIO;
			else {
				len = min (len, (size_t)dev->req->actual);
// FIXME don't call this with the spinlock held ...
				if (copy_to_user (buf, &dev->req->buf, len))
					retval = -EFAULT;
				clean_req (dev->gadget->ep0, dev->req);
				/* NOTE userspace can't yet choose to stall */
			}
		}
		goto done;
	}

	/* else normal: return event data */
	if (len < sizeof dev->event [0]) {
		retval = -EINVAL;
		goto done;
	}
	len -= len % sizeof (struct usb_gadgetfs_event);
	dev->usermode_setup = 1;

scan:
	/* return queued events right away */
	if (dev->ev_next != 0) {
		unsigned		i, n;
		int			tmp = dev->ev_next;

		len = min (len, tmp * sizeof (struct usb_gadgetfs_event));
		n = len / sizeof (struct usb_gadgetfs_event);

		/* ep0 can't deliver events when STATE_SETUP */
		for (i = 0; i < n; i++) {
			if (dev->event [i].type == GADGETFS_SETUP) {
				len = n = i + 1;
				len *= sizeof (struct usb_gadgetfs_event);
				n = 0;
				break;
			}
		}
		spin_unlock_irq (&dev->lock);
		if (copy_to_user (buf, &dev->event, len))
			retval = -EFAULT;
		else
			retval = len;
		if (len > 0) {
			len /= sizeof (struct usb_gadgetfs_event);

			/* NOTE this doesn't guard against broken drivers;
			 * concurrent ep0 readers may lose events.
			 */
			spin_lock_irq (&dev->lock);
			dev->ev_next -= len;
			if (dev->ev_next != 0)
				memmove (&dev->event, &dev->event [len],
					sizeof (struct usb_gadgetfs_event)
						* (tmp - len));
			if (n == 0)
				dev->state = STATE_SETUP;
			spin_unlock_irq (&dev->lock);
		}
		return retval;
	}
	if (fd->f_flags & O_NONBLOCK) {
		retval = -EAGAIN;
		goto done;
	}

	switch (state) {
	default:
		DBG (dev, "fail %s, state %d\n", __FUNCTION__, state);
		retval = -ESRCH;
		break;
	case STATE_UNCONNECTED:
	case STATE_CONNECTED:
		spin_unlock_irq (&dev->lock);
		DBG (dev, "%s wait\n", __FUNCTION__);

		/* wait for events */
		retval = wait_event_interruptible (dev->wait,
				dev->ev_next != 0);
		if (retval < 0)
			return retval;
		spin_lock_irq (&dev->lock);
		goto scan;
	}

done:
	spin_unlock_irq (&dev->lock);
	return retval;
}

static struct usb_gadgetfs_event *
next_event (struct dev_data *dev, enum usb_gadgetfs_event_type type)
{
	struct usb_gadgetfs_event	*event;
	unsigned			i;

	switch (type) {
	/* these events purge the queue */
	case GADGETFS_DISCONNECT:
		if (dev->state == STATE_SETUP)
			dev->setup_abort = 1;
		// FALL THROUGH
	case GADGETFS_CONNECT:
		dev->ev_next = 0;
		break;
	case GADGETFS_SETUP:		/* previous request timed out */
	case GADGETFS_SUSPEND:		/* same effect */
		/* these events can't be repeated */
		for (i = 0; i != dev->ev_next; i++) {
			if (dev->event [i].type != type)
				continue;
			DBG (dev, "discard old event %d\n", type);
			dev->ev_next--;
			if (i == dev->ev_next)
				break;
			/* indices start at zero, for simplicity */
			memmove (&dev->event [i], &dev->event [i + 1],
				sizeof (struct usb_gadgetfs_event)
					* (dev->ev_next - i));
		}
		break;
	default:
		BUG ();
	}
	event = &dev->event [dev->ev_next++];
	BUG_ON (dev->ev_next > N_EVENT);
	VDEBUG (dev, "ev %d, next %d\n", type, dev->ev_next);
	memset (event, 0, sizeof *event);
	event->type = type;
	return event;
}

static ssize_t
ep0_write (struct file *fd, const char __user *buf, size_t len, loff_t *ptr)
{
	struct dev_data		*dev = fd->private_data;
	ssize_t			retval = -ESRCH;

	spin_lock_irq (&dev->lock);

	/* report fd mode change before acting on it */
	if (dev->setup_abort) {
		dev->setup_abort = 0;
		retval = -EIDRM;

	/* data and/or status stage for control request */
	} else if (dev->state == STATE_SETUP) {

		/* IN DATA+STATUS caller makes len <= wLength */
		if (dev->setup_in) {
			retval = setup_req (dev->gadget->ep0, dev->req, len);
			if (retval == 0) {
				spin_unlock_irq (&dev->lock);
				if (copy_from_user (dev->req->buf, buf, len))
					retval = -EFAULT;
				else
					retval = usb_ep_queue (
						dev->gadget->ep0, dev->req,
						GFP_KERNEL);
				if (retval < 0) {
					spin_lock_irq (&dev->lock);
					clean_req (dev->gadget->ep0, dev->req);
					spin_unlock_irq (&dev->lock);
				} else
					retval = len;

				return retval;
			}

		/* can stall some OUT transfers */
		} else if (dev->setup_can_stall) {
			VDEBUG(dev, "ep0out stall\n");
			(void) usb_ep_set_halt (dev->gadget->ep0);
			retval = -EL2HLT;
			dev->state = STATE_CONNECTED;
		} else {
			DBG(dev, "bogus ep0out stall!\n");
		}
	} else
		DBG (dev, "fail %s, state %d\n", __FUNCTION__, dev->state);

	spin_unlock_irq (&dev->lock);
	return retval;
}

static int
ep0_fasync (int f, struct file *fd, int on)
{
	struct dev_data		*dev = fd->private_data;
	// caller must F_SETOWN before signal delivery happens
	VDEBUG (dev, "%s %s\n", __FUNCTION__, on ? "on" : "off");
	return fasync_helper (f, fd, on, &dev->fasync);
}

static struct usb_gadget_driver gadgetfs_driver;

static int
dev_release (struct inode *inode, struct file *fd)
{
	struct dev_data		*dev = fd->private_data;

	/* closing ep0 === shutdown all */

	usb_gadget_unregister_driver (&gadgetfs_driver);

	/* at this point "good" hardware has disconnected the
	 * device from USB; the host won't see it any more.
	 * alternatively, all host requests will time out.
	 */

	fasync_helper (-1, fd, 0, &dev->fasync);
	kfree (dev->buf);
	dev->buf = NULL;
	put_dev (dev);

	/* other endpoints were all decoupled from this device */
	dev->state = STATE_DEV_DISABLED;
	return 0;
}

static int dev_ioctl (struct inode *inode, struct file *fd,
		unsigned code, unsigned long value)
{
	struct dev_data		*dev = fd->private_data;
	struct usb_gadget	*gadget = dev->gadget;

	if (gadget->ops->ioctl)
		return gadget->ops->ioctl (gadget, code, value);
	return -ENOTTY;
}

/* used after device configuration */
static struct file_operations ep0_io_operations = {
	.owner =	THIS_MODULE,
	.read =		ep0_read,
	.write =	ep0_write,
	.fasync =	ep0_fasync,
	// .poll =	ep0_poll,
	.ioctl =	dev_ioctl,
	.release =	dev_release,
};

/*----------------------------------------------------------------------*/

/* The in-kernel gadget driver handles most ep0 issues, in particular
 * enumerating the single configuration (as provided from user space).
 *
 * Unrecognized ep0 requests may be handled in user space.
 */

#ifdef	HIGHSPEED
static void make_qualifier (struct dev_data *dev)
{
	struct usb_qualifier_descriptor		qual;
	struct usb_device_descriptor		*desc;

	qual.bLength = sizeof qual;
	qual.bDescriptorType = USB_DT_DEVICE_QUALIFIER;
	qual.bcdUSB = __constant_cpu_to_le16 (0x0200);

	desc = dev->dev;
	qual.bDeviceClass = desc->bDeviceClass;
	qual.bDeviceSubClass = desc->bDeviceSubClass;
	qual.bDeviceProtocol = desc->bDeviceProtocol;

	/* assumes ep0 uses the same value for both speeds ... */
	qual.bMaxPacketSize0 = desc->bMaxPacketSize0;

	qual.bNumConfigurations = 1;
	qual.bRESERVED = 0;

	memcpy (dev->rbuf, &qual, sizeof qual);
}
#endif

static int
config_buf (struct dev_data *dev, u8 type, unsigned index)
{
	int		len;
#ifdef HIGHSPEED
	int		hs;
#endif

	/* only one configuration */
	if (index > 0)
		return -EINVAL;

#ifdef HIGHSPEED
	hs = (dev->gadget->speed == USB_SPEED_HIGH);
	if (type == USB_DT_OTHER_SPEED_CONFIG)
		hs = !hs;
	if (hs) {
		dev->req->buf = dev->hs_config;
		len = le16_to_cpup (&dev->hs_config->wTotalLength);
	} else
#endif
	{
		dev->req->buf = dev->config;
		len = le16_to_cpup (&dev->config->wTotalLength);
	}
	((u8 *)dev->req->buf) [1] = type;
	return len;
}

static int
gadgetfs_setup (struct usb_gadget *gadget, const struct usb_ctrlrequest *ctrl)
{
	struct dev_data			*dev = get_gadget_data (gadget);
	struct usb_request		*req = dev->req;
	int				value = -EOPNOTSUPP;
	struct usb_gadgetfs_event	*event;

	spin_lock (&dev->lock);
	dev->setup_abort = 0;
	if (dev->state == STATE_UNCONNECTED) {
		struct usb_ep	*ep;
		struct ep_data	*data;

		dev->state = STATE_CONNECTED;
		dev->dev->bMaxPacketSize0 = gadget->ep0->maxpacket;

#ifdef	HIGHSPEED
		if (gadget->speed == USB_SPEED_HIGH && dev->hs_config == 0) {
			ERROR (dev, "no high speed config??\n");
			return -EINVAL;
		}
#endif	/* HIGHSPEED */

		INFO (dev, "connected\n");
		event = next_event (dev, GADGETFS_CONNECT);
		event->u.speed = gadget->speed;
		ep0_readable (dev);

		list_for_each_entry (ep, &gadget->ep_list, ep_list) {
			data = ep->driver_data;
			/* ... down_trylock (&data->lock) ... */
			if (data->state != STATE_EP_DEFER_ENABLE)
				continue;
#ifdef	HIGHSPEED
			if (gadget->speed == USB_SPEED_HIGH)
				value = usb_ep_enable (ep, &data->hs_desc);
			else
#endif	/* HIGHSPEED */
				value = usb_ep_enable (ep, &data->desc);
			if (value) {
				ERROR (dev, "deferred %s enable --> %d\n",
					data->name, value);
				continue;
			}
			data->state = STATE_EP_ENABLED;
			wake_up (&data->wait);
			DBG (dev, "woke up %s waiters\n", data->name);
		}

	/* host may have given up waiting for response.  we can miss control
	 * requests handled lower down (device/endpoint status and features);
	 * then ep0_{read,write} will report the wrong status. controller
	 * driver will have aborted pending i/o.
	 */
	} else if (dev->state == STATE_SETUP)
		dev->setup_abort = 1;

	req->buf = dev->rbuf;
	req->dma = DMA_ADDR_INVALID;
	req->context = NULL;
	value = -EOPNOTSUPP;
	switch (ctrl->bRequest) {

	case USB_REQ_GET_DESCRIPTOR:
		if (ctrl->bRequestType != USB_DIR_IN)
			goto unrecognized;
		switch (ctrl->wValue >> 8) {

		case USB_DT_DEVICE:
			value = min (ctrl->wLength, (u16) sizeof *dev->dev);
			req->buf = dev->dev;
			break;
#ifdef	HIGHSPEED
		case USB_DT_DEVICE_QUALIFIER:
			if (!dev->hs_config)
				break;
			value = min (ctrl->wLength, (u16)
				sizeof (struct usb_qualifier_descriptor));
			make_qualifier (dev);
			break;
		case USB_DT_OTHER_SPEED_CONFIG:
			// FALLTHROUGH
#endif
		case USB_DT_CONFIG:
			value = config_buf (dev,
					ctrl->wValue >> 8,
					ctrl->wValue & 0xff);
			if (value >= 0)
				value = min (ctrl->wLength, (u16) value);
			break;
		case USB_DT_STRING:
			goto unrecognized;

		default:		// all others are errors
			break;
		}
		break;

	/* currently one config, two speeds */
	case USB_REQ_SET_CONFIGURATION:
		if (ctrl->bRequestType != 0)
			break;
		if (0 == (u8) ctrl->wValue) {
			value = 0;
			dev->current_config = 0;
			// user mode expected to disable endpoints
		} else {
			u8	config;
#ifdef	HIGHSPEED
			if (gadget->speed == USB_SPEED_HIGH)
				config = dev->hs_config->bConfigurationValue;
			else
#endif
				config = dev->config->bConfigurationValue;

			if (config == (u8) ctrl->wValue) {
				value = 0;
				dev->current_config = config;
			}
		}

		/* report SET_CONFIGURATION like any other control request,
		 * except that usermode may not stall this.  the next
		 * request mustn't be allowed start until this finishes:
		 * endpoints and threads set up, etc.
		 *
		 * NOTE:  older PXA hardware (before PXA 255: without UDCCFR)
		 * has bad/racey automagic that prevents synchronizing here.
		 * even kernel mode drivers often miss them.
		 */
		if (value == 0) {
			INFO (dev, "configuration #%d\n", dev->current_config);
			if (dev->usermode_setup) {
				dev->setup_can_stall = 0;
				goto delegate;
			}
		}
		break;

#ifndef	CONFIG_USB_GADGETFS_PXA2XX
	/* PXA automagically handles this request too */
	case USB_REQ_GET_CONFIGURATION:
		if (ctrl->bRequestType != 0x80)
			break;
		*(u8 *)req->buf = dev->current_config;
		value = min (ctrl->wLength, (u16) 1);
		break;
#endif

	default:
unrecognized:
		VDEBUG (dev, "%s req%02x.%02x v%04x i%04x l%d\n",
			dev->usermode_setup ? "delegate" : "fail",
			ctrl->bRequestType, ctrl->bRequest,
			ctrl->wValue, ctrl->wIndex, ctrl->wLength);

		/* if there's an ep0 reader, don't stall */
		if (dev->usermode_setup) {
			dev->setup_can_stall = 1;
delegate:
			dev->setup_in = (ctrl->bRequestType & USB_DIR_IN)
						? 1 : 0;
			dev->setup_out_ready = 0;
			dev->setup_out_error = 0;
			value = 0;

			/* read DATA stage for OUT right away */
			if (unlikely (!dev->setup_in && ctrl->wLength)) {
				value = setup_req (gadget->ep0, dev->req,
							ctrl->wLength);
				if (value < 0)
					break;
				value = usb_ep_queue (gadget->ep0, dev->req,
							GFP_ATOMIC);
				if (value < 0) {
					clean_req (gadget->ep0, dev->req);
					break;
				}

				/* we can't currently stall these */
				dev->setup_can_stall = 0;
			}

			/* state changes when reader collects event */
			event = next_event (dev, GADGETFS_SETUP);
			event->u.setup = *ctrl;
			ep0_readable (dev);
			spin_unlock (&dev->lock);
			return 0;
		}
	}

	/* proceed with data transfer and status phases? */
	if (value >= 0 && dev->state != STATE_SETUP) {
		req->length = value;
		req->zero = value < ctrl->wLength
				&& (value % gadget->ep0->maxpacket) == 0;
		value = usb_ep_queue (gadget->ep0, req, GFP_ATOMIC);
		if (value < 0) {
			DBG (dev, "ep_queue --> %d\n", value);
			req->status = 0;
		}
	}

	/* device stalls when value < 0 */
	spin_unlock (&dev->lock);
	return value;
}

static void destroy_ep_files (struct dev_data *dev)
{
	struct list_head	*entry, *tmp;

	DBG (dev, "%s %d\n", __FUNCTION__, dev->state);

	/* dev->state must prevent interference */
restart:
	spin_lock_irq (&dev->lock);
	list_for_each_safe (entry, tmp, &dev->epfiles) {
		struct ep_data	*ep;
		struct inode	*parent;
		struct dentry	*dentry;

		/* break link to FS */
		ep = list_entry (entry, struct ep_data, epfiles);
		list_del_init (&ep->epfiles);
		dentry = ep->dentry;
		ep->dentry = NULL;
		parent = dentry->d_parent->d_inode;

		/* break link to controller */
		if (ep->state == STATE_EP_ENABLED)
			(void) usb_ep_disable (ep->ep);
		ep->state = STATE_EP_UNBOUND;
		usb_ep_free_request (ep->ep, ep->req);
		ep->ep = NULL;
		wake_up (&ep->wait);
		put_ep (ep);

		spin_unlock_irq (&dev->lock);

		/* break link to dcache */
		down (&parent->i_sem);
		d_delete (dentry);
		dput (dentry);
		up (&parent->i_sem);

		/* fds may still be open */
		goto restart;
	}
	spin_unlock_irq (&dev->lock);
}


static struct inode *
gadgetfs_create_file (struct super_block *sb, char const *name,
		void *data, struct file_operations *fops,
		struct dentry **dentry_p);

static int activate_ep_files (struct dev_data *dev)
{
	struct usb_ep	*ep;

	gadget_for_each_ep (ep, dev->gadget) {
		struct ep_data	*data;

		data = kmalloc (sizeof *data, GFP_KERNEL);
		if (!data)
			goto enomem;
		memset (data, 0, sizeof data);
		data->state = STATE_EP_DISABLED;
		init_MUTEX (&data->lock);
		init_waitqueue_head (&data->wait);

		strncpy (data->name, ep->name, sizeof (data->name) - 1);
		atomic_set (&data->count, 1);
		data->dev = dev;
		get_dev (dev);

		data->ep = ep;
		ep->driver_data = data;

		data->req = usb_ep_alloc_request (ep, GFP_KERNEL);
		if (!data->req)
			goto enomem;

		data->inode = gadgetfs_create_file (dev->sb, data->name,
				data, &ep_config_operations,
				&data->dentry);
		if (!data->inode) {
			kfree (data);
			goto enomem;
		}
		list_add_tail (&data->epfiles, &dev->epfiles);
	}
	return 0;

enomem:
	DBG (dev, "%s enomem\n", __FUNCTION__);
	destroy_ep_files (dev);
	return -ENOMEM;
}

static void
gadgetfs_unbind (struct usb_gadget *gadget)
{
	struct dev_data		*dev = get_gadget_data (gadget);

	DBG (dev, "%s\n", __FUNCTION__);

	spin_lock_irq (&dev->lock);
	dev->state = STATE_DEV_UNBOUND;
	spin_unlock_irq (&dev->lock);

	destroy_ep_files (dev);
	gadget->ep0->driver_data = NULL;
	set_gadget_data (gadget, NULL);

	/* we've already been disconnected ... no i/o is active */
	if (dev->req)
		usb_ep_free_request (gadget->ep0, dev->req);
	DBG (dev, "%s done\n", __FUNCTION__);
	put_dev (dev);
}

static struct dev_data		*the_device;

static int
gadgetfs_bind (struct usb_gadget *gadget)
{
	struct dev_data		*dev = the_device;

	if (!dev)
		return -ESRCH;
	if (0 != strcmp (CHIP, gadget->name)) {
		printk (KERN_ERR "%s expected " CHIP " controller not %s\n",
			shortname, gadget->name);
		return -ENODEV;
	}

	set_gadget_data (gadget, dev);
	dev->gadget = gadget;
	gadget->ep0->driver_data = dev;
	dev->dev->bMaxPacketSize0 = gadget->ep0->maxpacket;

	/* preallocate control response and buffer */
	dev->req = usb_ep_alloc_request (gadget->ep0, GFP_KERNEL);
	if (!dev->req)
		goto enomem;
	dev->req->context = NULL;
	dev->req->complete = epio_complete;

	if (activate_ep_files (dev) < 0)
		goto enomem;

	INFO (dev, "bound to %s driver\n", gadget->name);
	dev->state = STATE_UNCONNECTED;
	get_dev (dev);
	return 0;

enomem:
	gadgetfs_unbind (gadget);
	return -ENOMEM;
}

static void
gadgetfs_disconnect (struct usb_gadget *gadget)
{
	struct dev_data		*dev = get_gadget_data (gadget);

	if (dev->state == STATE_UNCONNECTED) {
		DBG (dev, "already unconnected\n");
		return;
	}
	dev->state = STATE_UNCONNECTED;

	INFO (dev, "disconnected\n");
	spin_lock (&dev->lock);
	next_event (dev, GADGETFS_DISCONNECT);
	ep0_readable (dev);
	spin_unlock (&dev->lock);
}

static void
gadgetfs_suspend (struct usb_gadget *gadget)
{
	struct dev_data		*dev = get_gadget_data (gadget);

	INFO (dev, "suspended from state %d\n", dev->state);
	spin_lock (&dev->lock);
	switch (dev->state) {
	case STATE_SETUP:		// VERY odd... host died??
	case STATE_CONNECTED:
	case STATE_UNCONNECTED:
		next_event (dev, GADGETFS_SUSPEND);
		ep0_readable (dev);
		/* FALLTHROUGH */
	default:
		break;
	}
	spin_unlock (&dev->lock);
}

static struct usb_gadget_driver gadgetfs_driver = {
#ifdef	HIGHSPEED
	.speed		= USB_SPEED_HIGH,
#else
	.speed		= USB_SPEED_FULL,
#endif
	.function	= (char *) driver_desc,
	.bind		= gadgetfs_bind,
	.unbind		= gadgetfs_unbind,
	.setup		= gadgetfs_setup,
	.disconnect	= gadgetfs_disconnect,
	.suspend	= gadgetfs_suspend,

	.driver 	= {
		.name		= (char *) shortname,
		// .shutdown = ...
		// .suspend = ...
		// .resume = ...
	},
};

/*----------------------------------------------------------------------*/

/* DEVICE INITIALIZATION
 *
 *     fd = open ("/dev/gadget/$CHIP", O_RDWR)
 *     status = write (fd, descriptors, sizeof descriptors)
 *
 * That write establishes the device configuration, so the kernel can
 * bind to the controller ... guaranteeing it can handle enumeration
 * at all necessary speeds.  Descriptor order is:
 *
 * . message tag (u32, host order) ... for now, must be zero; it
 *	would change to support features like multi-config devices
 * . full/low speed config ... all wTotalLength bytes (with interface,
 *	class, altsetting, endpoint, and other descriptors)
 * . high speed config ... all descriptors, for high speed operation;
 * 	this one's optional except for high-speed hardware
 * . device descriptor
 *
 * Endpoints are not yet enabled. Drivers may want to immediately
 * initialize them, using the /dev/gadget/ep* files that are available
 * as soon as the kernel sees the configuration, or they can wait
 * until device configuration and interface altsetting changes create
 * the need to configure (or unconfigure) them.
 *
 * After initialization, the device stays active for as long as that
 * $CHIP file is open.  Events may then be read from that descriptor,
 * such configuration notifications.  More complex drivers will handle
 * some control requests in user space.
 */

static int is_valid_config (struct usb_config_descriptor *config)
{
	return config->bDescriptorType == USB_DT_CONFIG
		&& config->bLength == USB_DT_CONFIG_SIZE
		&& config->bConfigurationValue != 0
		&& (config->bmAttributes & USB_CONFIG_ATT_ONE) != 0
		&& (config->bmAttributes & USB_CONFIG_ATT_WAKEUP) == 0;
	/* FIXME check lengths: walk to end */
}

static ssize_t
dev_config (struct file *fd, const char __user *buf, size_t len, loff_t *ptr)
{
	struct dev_data		*dev = fd->private_data;
	ssize_t			value = len, length = len;
	unsigned		total;
	u32			tag;
	char			*kbuf;

	if (dev->state != STATE_OPENED)
		return -EEXIST;

	if (len < (USB_DT_CONFIG_SIZE + USB_DT_DEVICE_SIZE + 4))
		return -EINVAL;

	/* we might need to change message format someday */
	if (copy_from_user (&tag, buf, 4))
		return -EFAULT;
	if (tag != 0)
		return -EINVAL;
	buf += 4;
	length -= 4;

	kbuf = kmalloc (length, SLAB_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	if (copy_from_user (kbuf, buf, length)) {
		kfree (kbuf);
		return -EFAULT;
	}

	spin_lock_irq (&dev->lock);
	value = -EINVAL;
	if (dev->buf)
		goto fail;
	dev->buf = kbuf;

	/* full or low speed config */
	dev->config = (void *) kbuf;
	total = le16_to_cpup (&dev->config->wTotalLength);
	if (!is_valid_config (dev->config) || total >= length)
		goto fail;
	kbuf += total;
	length -= total;

	/* optional high speed config */
	if (kbuf [1] == USB_DT_CONFIG) {
		dev->hs_config = (void *) kbuf;
		total = le16_to_cpup (&dev->hs_config->wTotalLength);
		if (!is_valid_config (dev->hs_config) || total >= length)
			goto fail;
		kbuf += total;
		length -= total;
	}

	/* could support multiple configs, using another encoding! */

	/* device descriptor (tweaked for paranoia) */
	if (length != USB_DT_DEVICE_SIZE)
		goto fail;
	dev->dev = (void *)kbuf;
	if (dev->dev->bLength != USB_DT_DEVICE_SIZE
			|| dev->dev->bDescriptorType != USB_DT_DEVICE
			|| dev->dev->bNumConfigurations != 1)
		goto fail;
	dev->dev->bNumConfigurations = 1;
	dev->dev->bcdUSB = __constant_cpu_to_le16 (0x0200);

	/* triggers gadgetfs_bind(); then we can enumerate. */
	spin_unlock_irq (&dev->lock);
	value = usb_gadget_register_driver (&gadgetfs_driver);
	if (value != 0) {
		kfree (dev->buf);
		dev->buf = NULL;
	} else {
		/* at this point "good" hardware has for the first time
		 * let the USB the host see us.  alternatively, if users
		 * unplug/replug that will clear all the error state.
		 *
		 * note:  everything running before here was guaranteed
		 * to choke driver model style diagnostics.  from here
		 * on, they can work ... except in cleanup paths that
		 * kick in after the ep0 descriptor is closed.
		 */
		fd->f_op = &ep0_io_operations;
		value = len;
	}
	return value;

fail:
	spin_unlock_irq (&dev->lock);
	pr_debug ("%s: %s fail %Zd, %p\n", shortname, __FUNCTION__, value, dev);
	kfree (dev->buf);
	dev->buf = NULL;
	return value;
}

static int
dev_open (struct inode *inode, struct file *fd)
{
	struct dev_data		*dev = inode->u.generic_ip;
	int			value = -EBUSY;

	if (dev->state == STATE_DEV_DISABLED) {
		dev->ev_next = 0;
		dev->state = STATE_OPENED;
		fd->private_data = dev;
		get_dev (dev);
		value = 0;
	}
	return value;
}

static struct file_operations dev_init_operations = {
	.owner =	THIS_MODULE,
	.open =		dev_open,
	.write =	dev_config,
	.fasync =	ep0_fasync,
	.ioctl =	dev_ioctl,
	.release =	dev_release,
};

/*----------------------------------------------------------------------*/

/* FILESYSTEM AND SUPERBLOCK OPERATIONS
 *
 * Mounting the filesystem creates a controller file, used first for
 * device configuration then later for event monitoring.
 */


/* FIXME PAM etc could set this security policy without mount options
 * if epfiles inherited ownership and permissons from ep0 ...
 */

static unsigned default_uid;
static unsigned default_gid;
static unsigned default_perm = S_IRUSR | S_IWUSR;

module_param (default_uid, uint, 0644);
module_param (default_gid, uint, 0644);
module_param (default_perm, uint, 0644);


static struct inode *
gadgetfs_make_inode (struct super_block *sb,
		void *data, struct file_operations *fops,
		int mode)
{
	struct inode *inode = new_inode (sb);

	if (inode) {
		inode->i_mode = mode;
		inode->i_uid = default_uid;
		inode->i_gid = default_gid;
		inode->i_blksize = PAGE_CACHE_SIZE;
		inode->i_blocks = 0;
		inode->i_atime = inode->i_mtime = inode->i_ctime
				= CURRENT_TIME;
		inode->u.generic_ip = data;
		inode->i_fop = fops;
	}
	return inode;
}

/* creates in fs root directory, so non-renamable and non-linkable.
 * so inode and dentry are paired, until device reconfig.
 */
static struct inode *
gadgetfs_create_file (struct super_block *sb, char const *name,
		void *data, struct file_operations *fops,
		struct dentry **dentry_p)
{
	struct dentry	*dentry;
	struct inode	*inode;
	struct qstr	qname;

	qname.name = name;
	qname.len = strlen (name);
	qname.hash = full_name_hash (qname.name, qname.len);
	dentry = d_alloc (sb->s_root, &qname);
	if (!dentry)
		return NULL;

	inode = gadgetfs_make_inode (sb, data, fops,
			S_IFREG | (default_perm & S_IRWXUGO));
	if (!inode) {
		dput(dentry);
		return NULL;
	}
	d_add (dentry, inode);
	*dentry_p = dentry;
	return inode;
}

static struct super_operations gadget_fs_operations = {
	.statfs =	simple_statfs,
	.drop_inode =	generic_delete_inode,
};

static int
gadgetfs_fill_super (struct super_block *sb, void *opts, int silent)
{
	struct inode	*inode;
	struct dentry	*d;
	struct dev_data	*dev;

	if (the_device)
		return -ESRCH;

	/* superblock */
	sb->s_blocksize = PAGE_CACHE_SIZE;
	sb->s_blocksize_bits = PAGE_CACHE_SHIFT;
	sb->s_magic = GADGETFS_MAGIC;
	sb->s_op = &gadget_fs_operations;

	/* root inode */
	inode = gadgetfs_make_inode (sb,
			NULL, &simple_dir_operations,
			S_IFDIR | S_IRUGO | S_IXUGO);
	if (!inode)
		return -ENOMEM;
	inode->i_op = &simple_dir_inode_operations;
	if (!(d = d_alloc_root (inode))) {
		iput (inode);
		return -ENOMEM;
	}
	sb->s_root = d;

	/* the ep0 file is named after the controller we expect;
	 * user mode code can use it for sanity checks, like we do.
	 */
	dev = dev_new ();
	if (!dev)
		return -ENOMEM;

	dev->sb = sb;
	if (!(inode = gadgetfs_create_file (sb, CHIP,
				dev, &dev_init_operations,
				&dev->dentry))) {
		put_dev(dev);
		return -ENOMEM;
	}

	/* other endpoint files are available after hardware setup,
	 * from binding to a controller.
	 */
	the_device = dev;
	return 0;
}

/* "mount -t gadgetfs path /dev/gadget" ends up here */
static struct super_block *
gadgetfs_get_sb (struct file_system_type *t, int flags,
		const char *path, void *opts)
{
	return get_sb_single (t, flags, opts, gadgetfs_fill_super);
}

static void
gadgetfs_kill_sb (struct super_block *sb)
{
	kill_litter_super (sb);
	if (the_device) {
		put_dev (the_device);
		the_device = NULL;
	}
}

/*----------------------------------------------------------------------*/

static struct file_system_type gadgetfs_type = {
	.owner		= THIS_MODULE,
	.name		= shortname,
	.get_sb		= gadgetfs_get_sb,
	.kill_sb	= gadgetfs_kill_sb,
};

/*----------------------------------------------------------------------*/

static int __init init (void)
{
	int status;

	status = register_filesystem (&gadgetfs_type);
	if (status == 0)
		pr_info ("%s: %s, version " DRIVER_VERSION "\n",
			shortname, driver_desc);
	return status;
}
module_init (init);

static void __exit cleanup (void)
{
	pr_debug ("unregister %s\n", shortname);
	unregister_filesystem (&gadgetfs_type);
}
module_exit (cleanup);

