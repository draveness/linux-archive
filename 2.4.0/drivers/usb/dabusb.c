/*****************************************************************************/

/*
 *      dabusb.c  --  dab usb driver.
 *
 *      Copyright (C) 1999  Deti Fliegl (deti@fliegl.de)
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License
 *      along with this program; if not, write to the Free Software
 *      Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 *
 *  $Id: dabusb.c,v 1.54 2000/07/24 21:39:39 deti Exp $
 *
 */

/*****************************************************************************/

#include <linux/module.h>
#include <linux/socket.h>
#include <linux/miscdevice.h>
#include <linux/list.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <asm/uaccess.h>
#include <asm/atomic.h>
#include <linux/delay.h>
#include <linux/usb.h>
#include <linux/smp_lock.h>

#include "dabusb.h"
#include "dabfirmware.h"

/* --------------------------------------------------------------------- */

#define NRDABUSB 4

/*-------------------------------------------------------------------*/

static dabusb_t dabusb[NRDABUSB];
static int buffers = 256;

/*-------------------------------------------------------------------*/

static int dabusb_add_buf_tail (pdabusb_t s, struct list_head *dst, struct list_head *src)
{
	unsigned long flags;
	struct list_head *tmp;
	int ret = 0;

	spin_lock_irqsave (&s->lock, flags);

	if (list_empty (src)) {
		// no elements in source buffer
		ret = -1;
		goto err;
	}
	tmp = src->next;
	list_del (tmp);
	list_add_tail (tmp, dst);

  err:	spin_unlock_irqrestore (&s->lock, flags);
	return ret;
}
/*-------------------------------------------------------------------*/
#ifdef DEBUG 
static void dump_urb (purb_t purb)
{
	dbg("urb                   :%p", purb);
	dbg("next                  :%p", purb->next);
	dbg("dev                   :%p", purb->dev);
	dbg("pipe                  :%08X", purb->pipe);
	dbg("status                :%d", purb->status);
	dbg("transfer_flags        :%08X", purb->transfer_flags);
	dbg("transfer_buffer       :%p", purb->transfer_buffer);
	dbg("transfer_buffer_length:%d", purb->transfer_buffer_length);
	dbg("actual_length         :%d", purb->actual_length);
	dbg("setup_packet          :%p", purb->setup_packet);
	dbg("start_frame           :%d", purb->start_frame);
	dbg("number_of_packets     :%d", purb->number_of_packets);
	dbg("interval              :%d", purb->interval);
	dbg("error_count           :%d", purb->error_count);
	dbg("context               :%p", purb->context);
	dbg("complete              :%p", purb->complete);
}
#endif
/*-------------------------------------------------------------------*/
static int dabusb_cancel_queue (pdabusb_t s, struct list_head *q)
{
	unsigned long flags;
	struct list_head *p;
	pbuff_t b;

	dbg("dabusb_cancel_queue");

	spin_lock_irqsave (&s->lock, flags);

	for (p = q->next; p != q; p = p->next) {
		b = list_entry (p, buff_t, buff_list);

#ifdef DEBUG
		dump_urb(b->purb);
#endif
		usb_unlink_urb (b->purb);
	}
	spin_unlock_irqrestore (&s->lock, flags);
	return 0;
}
/*-------------------------------------------------------------------*/
static int dabusb_free_queue (struct list_head *q)
{
	struct list_head *tmp;
	struct list_head *p;
	pbuff_t b;

	dbg("dabusb_free_queue");
	for (p = q->next; p != q;) {
		b = list_entry (p, buff_t, buff_list);

#ifdef DEBUG 
		dump_urb(b->purb);
#endif
		if (b->purb->transfer_buffer)
			kfree (b->purb->transfer_buffer);
		usb_free_urb(b->purb);
		tmp = p->next;
		list_del (p);
		kfree (b);
		p = tmp;
	}

	return 0;
}
/*-------------------------------------------------------------------*/
static int dabusb_free_buffers (pdabusb_t s)
{
	unsigned long flags;
	dbg("dabusb_free_buffers");

	spin_lock_irqsave(&s->lock, flags);

	dabusb_free_queue (&s->free_buff_list);
	dabusb_free_queue (&s->rec_buff_list);

	spin_unlock_irqrestore(&s->lock, flags);

	s->got_mem = 0;
	return 0;
}
/*-------------------------------------------------------------------*/
static void dabusb_iso_complete (purb_t purb)
{
	pbuff_t b = purb->context;
	pdabusb_t s = b->s;
	int i;
	int len;
	int dst = 0;
	void *buf = purb->transfer_buffer;

	dbg("dabusb_iso_complete");

	// process if URB was not killed
	if (purb->status != -ENOENT) {
		unsigned int pipe = usb_rcvisocpipe (purb->dev, _DABUSB_ISOPIPE);
		int pipesize = usb_maxpacket (purb->dev, pipe, usb_pipeout (pipe));
		for (i = 0; i < purb->number_of_packets; i++)
			if (!purb->iso_frame_desc[i].status) {
				len = purb->iso_frame_desc[i].actual_length;
				if (len <= pipesize) {
					memcpy (buf + dst, buf + purb->iso_frame_desc[i].offset, len);
					dst += len;
				}
				else
					err("dabusb_iso_complete: invalid len %d", len);
			}
			else
				warn("dabusb_iso_complete: corrupted packet status: %d", purb->iso_frame_desc[i].status);
		if (dst != purb->actual_length)
			err("dst!=purb->actual_length:%d!=%d", dst, purb->actual_length);
	}

	if (atomic_dec_and_test (&s->pending_io) && !s->remove_pending && s->state != _stopped) {
		s->overruns++;
		err("overrun (%d)", s->overruns);
	}
	wake_up (&s->wait);
}
/*-------------------------------------------------------------------*/
static int dabusb_alloc_buffers (pdabusb_t s)
{
	int buffers = 0;
	pbuff_t b;
	unsigned int pipe = usb_rcvisocpipe (s->usbdev, _DABUSB_ISOPIPE);
	int pipesize = usb_maxpacket (s->usbdev, pipe, usb_pipeout (pipe));
	int packets = _ISOPIPESIZE / pipesize;
	int transfer_buffer_length = packets * pipesize;
	int i;

	dbg("dabusb_alloc_buffers pipesize:%d packets:%d transfer_buffer_len:%d",
		 pipesize, packets, transfer_buffer_length);

	while (buffers < (s->total_buffer_size << 10)) {
		b = (pbuff_t) kmalloc (sizeof (buff_t), GFP_KERNEL);
		if (!b) {
			err("kmalloc(sizeof(buff_t))==NULL");
			goto err;
		}
		memset (b, sizeof (buff_t), 0);
		b->s = s;
		b->purb = usb_alloc_urb(packets);
		if (!b->purb) {
			err("usb_alloc_urb == NULL");
			kfree (b);
			goto err;
		}

		b->purb->transfer_buffer = kmalloc (transfer_buffer_length, GFP_KERNEL);
		if (!b->purb->transfer_buffer) {
			kfree (b->purb);
			kfree (b);
			err("kmalloc(%d)==NULL", transfer_buffer_length);
			goto err;
		}

		b->purb->transfer_buffer_length = transfer_buffer_length;
		b->purb->number_of_packets = packets;
		b->purb->complete = dabusb_iso_complete;
		b->purb->context = b;
		b->purb->dev = s->usbdev;
		b->purb->pipe = pipe;
		b->purb->transfer_flags = USB_ISO_ASAP;

		for (i = 0; i < packets; i++) {
			b->purb->iso_frame_desc[i].offset = i * pipesize;
			b->purb->iso_frame_desc[i].length = pipesize;
		}

		buffers += transfer_buffer_length;
		list_add_tail (&b->buff_list, &s->free_buff_list);
	}
	s->got_mem = buffers;

	return 0;

	err:
	dabusb_free_buffers (s);
	return -ENOMEM;
}
/*-------------------------------------------------------------------*/
static int dabusb_bulk (pdabusb_t s, pbulk_transfer_t pb)
{
	int ret;
	unsigned int pipe;
	int actual_length;

	dbg("dabusb_bulk");

	if (!pb->pipe)
		pipe = usb_rcvbulkpipe (s->usbdev, 2);
	else
		pipe = usb_sndbulkpipe (s->usbdev, 2);

	ret=usb_bulk_msg(s->usbdev, pipe, pb->data, pb->size, &actual_length, 100);
	if(ret<0) {
		err("dabusb: usb_bulk_msg failed(%d)",ret);

		if (usb_set_interface (s->usbdev, _DABUSB_IF, 1) < 0) {
			err("set_interface failed");
			return -EINVAL;
		}

	}
	
	if( ret == -EPIPE ) {
		warn("CLEAR_FEATURE request to remove STALL condition.");
		if(usb_clear_halt(s->usbdev, usb_pipeendpoint(pipe)))
			err("request failed");
	}

	pb->size = actual_length;
	return ret;
}
/* --------------------------------------------------------------------- */
static int dabusb_writemem (pdabusb_t s, int pos, unsigned char *data, int len)
{
	int ret;
	unsigned char *transfer_buffer =  kmalloc (len, GFP_KERNEL);

	if (!transfer_buffer) {
		err("dabusb_writemem: kmalloc(%d) failed.", len);
		return -ENOMEM;
	}

	memcpy (transfer_buffer, data, len);

	ret=usb_control_msg(s->usbdev, usb_sndctrlpipe( s->usbdev, 0 ), 0xa0, 0x40, pos, 0, transfer_buffer, len, 300);

	kfree (transfer_buffer);
	return ret;
}
/* --------------------------------------------------------------------- */
static int dabusb_8051_reset (pdabusb_t s, unsigned char reset_bit)
{
	dbg("dabusb_8051_reset: %d",reset_bit);
	return dabusb_writemem (s, CPUCS_REG, &reset_bit, 1);
}
/* --------------------------------------------------------------------- */
static int dabusb_loadmem (pdabusb_t s, const char *fname)
{
	int ret;
	PINTEL_HEX_RECORD ptr = firmware;

	dbg("Enter dabusb_loadmem (internal)");
	
	ret = dabusb_8051_reset (s, 1);
	while (ptr->Type == 0) {

		dbg("dabusb_writemem: %04X %p %d)", ptr->Address, ptr->Data, ptr->Length);

		ret = dabusb_writemem (s, ptr->Address, ptr->Data, ptr->Length);
		if (ret < 0) {
			err("dabusb_writemem failed (%d %04X %p %d)", ret, ptr->Address, ptr->Data, ptr->Length);
			break;
		}
		ptr++;
	}
	ret = dabusb_8051_reset (s, 0);

	dbg("dabusb_loadmem: exit");

	return ret;
}
/* --------------------------------------------------------------------- */
static int dabusb_fpga_clear (pdabusb_t s, pbulk_transfer_t b)
{
	b->size = 4;
	b->data[0] = 0x2a;
	b->data[1] = 0;
	b->data[2] = 0;
	b->data[3] = 0;

	dbg("dabusb_fpga_clear");

	return dabusb_bulk (s, b);
}
/* --------------------------------------------------------------------- */
static int dabusb_fpga_init (pdabusb_t s, pbulk_transfer_t b)
{
	b->size = 4;
	b->data[0] = 0x2c;
	b->data[1] = 0;
	b->data[2] = 0;
	b->data[3] = 0;

	dbg("dabusb_fpga_init");

	return dabusb_bulk (s, b);
}
/* --------------------------------------------------------------------- */
static int dabusb_fpga_download (pdabusb_t s, const char *fname)
{
	pbulk_transfer_t b = kmalloc (sizeof (bulk_transfer_t), GFP_KERNEL);
	unsigned int blen, n;
	int ret;
	unsigned char *buf = bitstream;

	dbg("Enter dabusb_fpga_download (internal)");

	if (!b) {
		err("kmalloc(sizeof(bulk_transfer_t))==NULL");
		return -ENOMEM;
	}

	b->pipe = 1;
	ret = dabusb_fpga_clear (s, b);
	mdelay (10);
	blen = buf[73] + (buf[72] << 8);

	dbg("Bitstream len: %i", blen);

	b->data[0] = 0x2b;
	b->data[1] = 0;
	b->data[2] = 0;
	b->data[3] = 60;

	for (n = 0; n <= blen + 60; n += 60) {
		// some cclks for startup
		b->size = 64;
		memcpy (b->data + 4, buf + 74 + n, 60);
		ret = dabusb_bulk (s, b);
		if (ret < 0) {
			err("dabusb_bulk failed.");
			break;
		}
		mdelay (1);
	}

	ret = dabusb_fpga_init (s, b);
	kfree (b);

	dbg("exit dabusb_fpga_download");

	return ret;
}

static loff_t dabusb_llseek (struct file *file, loff_t offset, int origin)
{
	return -ESPIPE;
}

static int dabusb_stop (pdabusb_t s)
{
	dbg("dabusb_stop");

	s->state = _stopped;
	dabusb_cancel_queue (s, &s->rec_buff_list);

	dbg("pending_io: %d", s->pending_io.counter);

	s->pending_io.counter = 0;
	return 0;
}

static int dabusb_startrek (pdabusb_t s)
{
	if (!s->got_mem && s->state != _started) {

		dbg("dabusb_startrek");

		if (dabusb_alloc_buffers (s) < 0)
			return -ENOMEM;
		dabusb_stop (s);
		s->state = _started;
		s->readptr = 0;
	}

	if (!list_empty (&s->free_buff_list)) {
		pbuff_t end;
		int ret;
		
	while (!dabusb_add_buf_tail (s, &s->rec_buff_list, &s->free_buff_list)) {

			dbg("submitting: end:%p s->rec_buff_list:%p", s->rec_buff_list.prev, &s->rec_buff_list);

			end = list_entry (s->rec_buff_list.prev, buff_t, buff_list);

			ret = usb_submit_urb (end->purb);
			if (ret) {
				err("usb_submit_urb returned:%d", ret);
				if (dabusb_add_buf_tail (s, &s->free_buff_list, &s->rec_buff_list))
					err("startrek: dabusb_add_buf_tail failed");
				break;
			}
			else
				atomic_inc (&s->pending_io);
		}
		dbg("pending_io: %d",s->pending_io.counter);
	}

	return 0;
}

static ssize_t dabusb_read (struct file *file, char *buf, size_t count, loff_t * ppos)
{
	pdabusb_t s = (pdabusb_t) file->private_data;
	unsigned long flags;
	unsigned ret = 0;
	int rem;
	int cnt;
	pbuff_t b;
	purb_t purb = NULL;

	dbg("dabusb_read");

	if (*ppos)
		return -ESPIPE;

	if (s->remove_pending)
		return -EIO;


	if (!s->usbdev)
		return -EIO;

	while (count > 0) {
		dabusb_startrek (s);

		spin_lock_irqsave (&s->lock, flags);

		if (list_empty (&s->rec_buff_list)) {

			spin_unlock_irqrestore(&s->lock, flags);

			err("error: rec_buf_list is empty");
			goto err;
		}
		
		b = list_entry (s->rec_buff_list.next, buff_t, buff_list);
		purb = b->purb;

		spin_unlock_irqrestore(&s->lock, flags);

		if (purb->status == -EINPROGRESS) {
			if (file->f_flags & O_NONBLOCK)		// return nonblocking
			 {
				if (!ret)
					ret = -EAGAIN;
				goto err;
			}

			interruptible_sleep_on (&s->wait);

			if (signal_pending (current)) {
				if (!ret)
					ret = -ERESTARTSYS;
				goto err;
			}

			spin_lock_irqsave (&s->lock, flags);

			if (list_empty (&s->rec_buff_list)) {
				spin_unlock_irqrestore(&s->lock, flags);
				err("error: still no buffer available.");
				goto err;
			}
			spin_unlock_irqrestore(&s->lock, flags);
			s->readptr = 0;
		}
		if (s->remove_pending) {
			ret = -EIO;
			goto err;
		}

		rem = purb->actual_length - s->readptr;		// set remaining bytes to copy

		if (count >= rem)
			cnt = rem;
		else
			cnt = count;

		dbg("copy_to_user:%p %p %d",buf, purb->transfer_buffer + s->readptr, cnt);

		if (copy_to_user (buf, purb->transfer_buffer + s->readptr, cnt)) {
			err("read: copy_to_user failed");
			if (!ret)
				ret = -EFAULT;
			goto err;
		}

		s->readptr += cnt;
		count -= cnt;
		buf += cnt;
		ret += cnt;

		if (s->readptr == purb->actual_length) {
			// finished, take next buffer
			if (dabusb_add_buf_tail (s, &s->free_buff_list, &s->rec_buff_list))
				err("read: dabusb_add_buf_tail failed");
			s->readptr = 0;
		}
	}
      err:			//up(&s->mutex);
	return ret;
}

static int dabusb_open (struct inode *inode, struct file *file)
{
	int devnum = MINOR (inode->i_rdev);
	pdabusb_t s;

	if (devnum < DABUSB_MINOR || devnum >= (DABUSB_MINOR + NRDABUSB))
		return -EIO;

	s = &dabusb[devnum - DABUSB_MINOR];

	dbg("dabusb_open");
	down (&s->mutex);

	while (!s->usbdev || s->opened) {
		up (&s->mutex);

		if (file->f_flags & O_NONBLOCK) {
			return -EBUSY;
		}
		schedule_timeout (HZ / 2);

		if (signal_pending (current)) {
			return -EAGAIN;
		}
		down (&s->mutex);
	}
	if (usb_set_interface (s->usbdev, _DABUSB_IF, 1) < 0) {
		err("set_interface failed");
		return -EINVAL;
	}
	s->opened = 1;
	up (&s->mutex);

	file->f_pos = 0;
	file->private_data = s;

	return 0;
}

static int dabusb_release (struct inode *inode, struct file *file)
{
	pdabusb_t s = (pdabusb_t) file->private_data;

	dbg("dabusb_release");

	lock_kernel();
	down (&s->mutex);
	dabusb_stop (s);
	dabusb_free_buffers (s);
	up (&s->mutex);

	if (!s->remove_pending) {
		if (usb_set_interface (s->usbdev, _DABUSB_IF, 0) < 0)
			err("set_interface failed");
	}
	else
		wake_up (&s->remove_ok);

	s->opened = 0;
	unlock_kernel();
	return 0;
}

static int dabusb_ioctl (struct inode *inode, struct file *file, unsigned int cmd, unsigned long arg)
{
	pdabusb_t s = (pdabusb_t) file->private_data;
	pbulk_transfer_t pbulk;
	int ret = 0;
	int version = DABUSB_VERSION;

	dbg("dabusb_ioctl");

	if (s->remove_pending)
		return -EIO;

	down (&s->mutex);

	if (!s->usbdev) {
		up (&s->mutex);
		return -EIO;
	}

	switch (cmd) {

	case IOCTL_DAB_BULK:
		pbulk = (pbulk_transfer_t) kmalloc (sizeof (bulk_transfer_t), GFP_KERNEL);

		if (!pbulk) {
			ret = -ENOMEM;
			break;
		}

		if (copy_from_user (pbulk, (void *) arg, sizeof (bulk_transfer_t))) {
			ret = -EFAULT;
			kfree (pbulk);
			break;
		}

		ret=dabusb_bulk (s, pbulk);
		if(ret==0)
			ret = copy_to_user ((void *) arg, pbulk, sizeof (bulk_transfer_t));
		kfree (pbulk);
		break;

	case IOCTL_DAB_OVERRUNS:
		ret = put_user (s->overruns, (unsigned int *) arg);
		break;

	case IOCTL_DAB_VERSION:
		ret = put_user (version, (unsigned int *) arg);
		break;

	default:
		ret = -ENOIOCTLCMD;
		break;
	}
	up (&s->mutex);
	return ret;
}

static struct file_operations dabusb_fops =
{
	owner:		THIS_MODULE,
	llseek:		dabusb_llseek,
	read:		dabusb_read,
	ioctl:		dabusb_ioctl,
	open:		dabusb_open,
	release:	dabusb_release,
};

static int dabusb_find_struct (void)
{
	int u;

	for (u = 0; u < NRDABUSB; u++) {
		pdabusb_t s = &dabusb[u];
		if (!s->usbdev)
			return u;
	}
	return -1;
}

/* --------------------------------------------------------------------- */
static void *dabusb_probe (struct usb_device *usbdev, unsigned int ifnum,
			   const struct usb_device_id *id)
{
	int devnum;
	pdabusb_t s;

	dbg("dabusb: probe: vendor id 0x%x, device id 0x%x ifnum:%d",
	  usbdev->descriptor.idVendor, usbdev->descriptor.idProduct, ifnum);

	/* We don't handle multiple configurations */
	if (usbdev->descriptor.bNumConfigurations != 1)
		return NULL;

	if (ifnum != _DABUSB_IF && usbdev->descriptor.idProduct == 0x9999)
		return NULL;

	devnum = dabusb_find_struct ();
	if (devnum == -1)
		return NULL;

	s = &dabusb[devnum];

	down (&s->mutex);
	s->remove_pending = 0;
	s->usbdev = usbdev;

	if (usb_set_configuration (usbdev, usbdev->config[0].bConfigurationValue) < 0) {
		err("set_configuration failed");
		goto reject;
	}
	if (usbdev->descriptor.idProduct == 0x2131) {
		dabusb_loadmem (s, NULL);
		goto reject;
	}
	else {
		dabusb_fpga_download (s, NULL);

		if (usb_set_interface (s->usbdev, _DABUSB_IF, 0) < 0) {
			err("set_interface failed");
			goto reject;
		}
	}
	dbg("bound to interface: %d", ifnum);
	up (&s->mutex);
	MOD_INC_USE_COUNT;
	return s;

      reject:
	up (&s->mutex);
	s->usbdev = NULL;
	return NULL;
}

static void dabusb_disconnect (struct usb_device *usbdev, void *ptr)
{
	pdabusb_t s = (pdabusb_t) ptr;

	dbg("dabusb_disconnect");

	s->remove_pending = 1;
	wake_up (&s->wait);
	if (s->state == _started)
		sleep_on (&s->remove_ok);
	s->usbdev = NULL;
	s->overruns = 0;
	MOD_DEC_USE_COUNT;
}

static struct usb_device_id dabusb_ids [] = {
	{ USB_DEVICE(0x0547, 0x2131) },
	{ USB_DEVICE(0x0547, 0x9999) },
	{ }						/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, dabusb_ids);

static struct usb_driver dabusb_driver =
{
	name:		"dabusb",
	probe:		dabusb_probe,
	disconnect:	dabusb_disconnect,
	fops:		&dabusb_fops,
	minor:		DABUSB_MINOR,
	id_table:	dabusb_ids,
};

/* --------------------------------------------------------------------- */

static int __init dabusb_init (void)
{
	unsigned u;

	/* initialize struct */
	for (u = 0; u < NRDABUSB; u++) {
		pdabusb_t s = &dabusb[u];
		memset (s, 0, sizeof (dabusb_t));
		init_MUTEX (&s->mutex);
		s->usbdev = NULL;
		s->total_buffer_size = buffers;
		init_waitqueue_head (&s->wait);
		init_waitqueue_head (&s->remove_ok);
		spin_lock_init (&s->lock);
		INIT_LIST_HEAD (&s->free_buff_list);
		INIT_LIST_HEAD (&s->rec_buff_list);
	}

	/* register misc device */
	if (usb_register(&dabusb_driver))
		return -1;

	dbg("dabusb_init: driver registered");
	return 0;
}

static void __exit dabusb_cleanup (void)
{
	dbg("dabusb_cleanup");

	usb_deregister (&dabusb_driver);
}

/* --------------------------------------------------------------------- */

MODULE_AUTHOR ("Deti Fliegl, deti@fliegl.de");
MODULE_DESCRIPTION ("DAB-USB Interface Driver for Linux (c)1999");
MODULE_PARM (buffers, "i");
MODULE_PARM_DESC (buffers, "Number of buffers (default=256)");

module_init (dabusb_init);
module_exit (dabusb_cleanup);

/* --------------------------------------------------------------------- */
