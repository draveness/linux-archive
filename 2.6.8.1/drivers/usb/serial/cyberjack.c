/*
 *  REINER SCT cyberJack pinpad/e-com USB Chipcard Reader Driver
 *
 *  Copyright (C) 2001  REINER SCT
 *  Author: Matthias Bruestle
 *
 *  Contact: linux-usb@sii.li (see MAINTAINERS)
 *
 *  This program is largely derived from work by the linux-usb group
 *  and associated source files.  Please see the usb/serial files for
 *  individual credits and copyrights.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Thanks to Greg Kroah-Hartman (greg@kroah.com) for his help and
 *  patience.
 *
 *  In case of problems, please write to the contact e-mail address
 *  mentioned above.
 */


#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <asm/uaccess.h>
#include <linux/usb.h>
#include "usb-serial.h"

#define CYBERJACK_LOCAL_BUF_SIZE 32

static int debug;

/*
 * Version Information
 */
#define DRIVER_VERSION "v1.0"
#define DRIVER_AUTHOR "Matthias Bruestle"
#define DRIVER_DESC "REINER SCT cyberJack pinpad/e-com USB Chipcard Reader Driver"


#define CYBERJACK_VENDOR_ID	0x0C4B
#define CYBERJACK_PRODUCT_ID	0x0100

/* Function prototypes */
static int cyberjack_startup (struct usb_serial *serial);
static void cyberjack_shutdown (struct usb_serial *serial);
static int  cyberjack_open (struct usb_serial_port *port, struct file *filp);
static void cyberjack_close (struct usb_serial_port *port, struct file *filp);
static int cyberjack_write (struct usb_serial_port *port, int from_user,
	const unsigned char *buf, int count);
static int cyberjack_write_room( struct usb_serial_port *port );
static void cyberjack_read_int_callback (struct urb *urb, struct pt_regs *regs);
static void cyberjack_read_bulk_callback (struct urb *urb, struct pt_regs *regs);
static void cyberjack_write_bulk_callback (struct urb *urb, struct pt_regs *regs);

static struct usb_device_id id_table [] = {
	{ USB_DEVICE(CYBERJACK_VENDOR_ID, CYBERJACK_PRODUCT_ID) },
	{ }			/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, id_table);

static struct usb_driver cyberjack_driver = {
	.owner =	THIS_MODULE,
	.name =		"cyberjack",
	.probe =	usb_serial_probe,
	.disconnect =	usb_serial_disconnect,
	.id_table =	id_table,
};

static struct usb_serial_device_type cyberjack_device = {
	.owner =		THIS_MODULE,
	.name =			"Reiner SCT Cyberjack USB card reader",
	.short_name =		"cyberjack",
	.id_table =		id_table,
	.num_interrupt_in =	1,
	.num_bulk_in =		1,
	.num_bulk_out =		1,
	.num_ports =		1,
	.attach =		cyberjack_startup,
	.shutdown =		cyberjack_shutdown,
	.open =			cyberjack_open,
	.close =		cyberjack_close,
	.write =		cyberjack_write,
	.write_room =	cyberjack_write_room,
	.read_int_callback =	cyberjack_read_int_callback,
	.read_bulk_callback =	cyberjack_read_bulk_callback,
	.write_bulk_callback =	cyberjack_write_bulk_callback,
};

struct cyberjack_private {
	spinlock_t	lock;		/* Lock for SMP */
	short		rdtodo;		/* Bytes still to read */
	unsigned char	wrbuf[5*64];	/* Buffer for collecting data to write */
	short		wrfilled;	/* Overall data size we already got */
	short		wrsent;		/* Data already sent */
};

/* do some startup allocations not currently performed by usb_serial_probe() */
static int cyberjack_startup (struct usb_serial *serial)
{
	struct cyberjack_private *priv;

	dbg("%s", __FUNCTION__);

	/* allocate the private data structure */
	priv = kmalloc(sizeof(struct cyberjack_private), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	/* set initial values */
	spin_lock_init(&priv->lock);
	priv->rdtodo = 0;
	priv->wrfilled = 0;
	priv->wrsent = 0;
	usb_set_serial_port_data(serial->port[0], priv);

	init_waitqueue_head(&serial->port[0]->write_wait);

	return( 0 );
}

static void cyberjack_shutdown (struct usb_serial *serial)
{
	int i;
	
	dbg("%s", __FUNCTION__);

	for (i=0; i < serial->num_ports; ++i) {
		/* My special items, the standard routines free my urbs */
		kfree(usb_get_serial_port_data(serial->port[i]));
		usb_set_serial_port_data(serial->port[i], NULL);
	}
}
	
static int  cyberjack_open (struct usb_serial_port *port, struct file *filp)
{
	struct cyberjack_private *priv;
	unsigned long flags;
	int result = 0;

	dbg("%s - port %d", __FUNCTION__, port->number);

	dbg("%s - usb_clear_halt", __FUNCTION__ );
	usb_clear_halt(port->serial->dev, port->write_urb->pipe);

	/* force low_latency on so that our tty_push actually forces
	 * the data through, otherwise it is scheduled, and with high
	 * data rates (like with OHCI) data can get lost.
	 */
	port->tty->low_latency = 1;

	priv = usb_get_serial_port_data(port);
	spin_lock_irqsave(&priv->lock, flags);
	priv->rdtodo = 0;
	priv->wrfilled = 0;
	priv->wrsent = 0;
	spin_unlock_irqrestore(&priv->lock, flags);

	/* shutdown any bulk reads that might be going on */
	usb_unlink_urb (port->write_urb);
	usb_unlink_urb (port->read_urb);
	usb_unlink_urb (port->interrupt_in_urb);

	port->interrupt_in_urb->dev = port->serial->dev;
	result = usb_submit_urb(port->interrupt_in_urb, GFP_KERNEL);
	if (result)
		err(" usb_submit_urb(read int) failed");
	dbg("%s - usb_submit_urb(int urb)", __FUNCTION__);

	return result;
}

static void cyberjack_close (struct usb_serial_port *port, struct file *filp)
{
	dbg("%s - port %d", __FUNCTION__, port->number);

	if (port->serial->dev) {
		/* shutdown any bulk reads that might be going on */
		usb_unlink_urb (port->write_urb);
		usb_unlink_urb (port->read_urb);
		usb_unlink_urb (port->interrupt_in_urb);
		dbg("%s - usb_clear_halt", __FUNCTION__ );
		usb_clear_halt(port->serial->dev, port->write_urb->pipe);
		usb_clear_halt(port->serial->dev, port->read_urb->pipe);
		usb_clear_halt(port->serial->dev, port->interrupt_in_urb->pipe);
	}
}

static int cyberjack_write (struct usb_serial_port *port, int from_user, const unsigned char *buf, int count)
{
	struct usb_serial *serial = port->serial;
	struct cyberjack_private *priv = usb_get_serial_port_data(port);
	unsigned long flags;
	int result;
	int wrexpected;

	dbg("%s - port %d", __FUNCTION__, port->number);
	dbg("%s - from_user %d", __FUNCTION__, from_user);

	if (count == 0) {
		dbg("%s - write request of 0 bytes", __FUNCTION__);
		return (0);
	}

	if (port->write_urb->status == -EINPROGRESS) {
		dbg("%s - already writing", __FUNCTION__);
		return (0);
	}

	spin_lock_irqsave(&priv->lock, flags);

	if( (count+priv->wrfilled)>sizeof(priv->wrbuf) ) {
		/* To much data for buffer. Reset buffer. */
		priv->wrfilled=0;
		spin_unlock_irqrestore(&priv->lock, flags);
		return (0);
	}

	/* Copy data */
	if (from_user) {
		if (copy_from_user(priv->wrbuf+priv->wrfilled, buf, count)) {
			spin_unlock_irqrestore(&priv->lock, flags);
			return -EFAULT;
		}
	} else {
		memcpy (priv->wrbuf+priv->wrfilled, buf, count);
	}  

	usb_serial_debug_data(debug, &port->dev, __FUNCTION__, count,
		priv->wrbuf+priv->wrfilled);
	priv->wrfilled += count;

	if( priv->wrfilled >= 3 ) {
		wrexpected = ((int)priv->wrbuf[2]<<8)+priv->wrbuf[1]+3;
		dbg("%s - expected data: %d", __FUNCTION__, wrexpected);
	} else {
		wrexpected = sizeof(priv->wrbuf);
	}

	if( priv->wrfilled >= wrexpected ) {
		/* We have enough data to begin transmission */
		int length;

		dbg("%s - transmitting data (frame 1)", __FUNCTION__);
		length = (wrexpected > port->bulk_out_size) ? port->bulk_out_size : wrexpected;

		memcpy (port->write_urb->transfer_buffer, priv->wrbuf, length );
		priv->wrsent=length;

		/* set up our urb */
		usb_fill_bulk_urb(port->write_urb, serial->dev, 
			      usb_sndbulkpipe(serial->dev, port->bulk_out_endpointAddress),
			      port->write_urb->transfer_buffer, length,
			      ((serial->type->write_bulk_callback) ? 
			       serial->type->write_bulk_callback : 
			       cyberjack_write_bulk_callback), 
			      port);

		/* send the data out the bulk port */
		result = usb_submit_urb(port->write_urb, GFP_ATOMIC);
		if (result) {
			err("%s - failed submitting write urb, error %d", __FUNCTION__, result);
			/* Throw away data. No better idea what to do with it. */
			priv->wrfilled=0;
			priv->wrsent=0;
			spin_unlock_irqrestore(&priv->lock, flags);
			return 0;
		}

		dbg("%s - priv->wrsent=%d", __FUNCTION__,priv->wrsent);
		dbg("%s - priv->wrfilled=%d", __FUNCTION__,priv->wrfilled);

		if( priv->wrsent>=priv->wrfilled ) {
			dbg("%s - buffer cleaned", __FUNCTION__);
			memset( priv->wrbuf, 0, sizeof(priv->wrbuf) );
			priv->wrfilled=0;
			priv->wrsent=0;
		}
	}

	spin_unlock_irqrestore(&priv->lock, flags);

	return (count);
} 

static int cyberjack_write_room( struct usb_serial_port *port )
{
	return CYBERJACK_LOCAL_BUF_SIZE;
}

static void cyberjack_read_int_callback( struct urb *urb, struct pt_regs *regs )
{
	struct usb_serial_port *port = (struct usb_serial_port *)urb->context;
	struct cyberjack_private *priv = usb_get_serial_port_data(port);
	unsigned char *data = urb->transfer_buffer;
	int result;

	dbg("%s - port %d", __FUNCTION__, port->number);

	/* the urb might have been killed. */
	if (urb->status)
		return;

	usb_serial_debug_data(debug, &port->dev, __FUNCTION__, urb->actual_length, data);

	/* React only to interrupts signaling a bulk_in transfer */
	if( (urb->actual_length==4) && (data[0]==0x01) ) {
		short old_rdtodo;
		int result;

		/* This is a announcement of coming bulk_ins. */
		unsigned short size = ((unsigned short)data[3]<<8)+data[2]+3;

		spin_lock(&priv->lock);

		old_rdtodo = priv->rdtodo;

		if( (old_rdtodo+size)<(old_rdtodo) ) {
			dbg( "To many bulk_in urbs to do." );
			spin_unlock(&priv->lock);
			goto resubmit;
		}

		/* "+=" is probably more fault tollerant than "=" */
		priv->rdtodo += size;

		dbg("%s - rdtodo: %d", __FUNCTION__, priv->rdtodo);

		spin_unlock(&priv->lock);

		if( !old_rdtodo ) {
			port->read_urb->dev = port->serial->dev;
			result = usb_submit_urb(port->read_urb, GFP_ATOMIC);
			if( result )
				err("%s - failed resubmitting read urb, error %d", __FUNCTION__, result);
			dbg("%s - usb_submit_urb(read urb)", __FUNCTION__);
		}
	}

resubmit:
	port->interrupt_in_urb->dev = port->serial->dev;
	result = usb_submit_urb(port->interrupt_in_urb, GFP_ATOMIC);
	if (result)
		err(" usb_submit_urb(read int) failed");
	dbg("%s - usb_submit_urb(int urb)", __FUNCTION__);
}

static void cyberjack_read_bulk_callback (struct urb *urb, struct pt_regs *regs)
{
	struct usb_serial_port *port = (struct usb_serial_port *)urb->context;
	struct cyberjack_private *priv = usb_get_serial_port_data(port);
	struct tty_struct *tty;
	unsigned char *data = urb->transfer_buffer;
	short todo;
	int i;
	int result;

	dbg("%s - port %d", __FUNCTION__, port->number);
	
	usb_serial_debug_data(debug, &port->dev, __FUNCTION__, urb->actual_length, data);
	if (urb->status) {
		dbg("%s - nonzero read bulk status received: %d", __FUNCTION__, urb->status);
		return;
	}

	tty = port->tty;
	if (urb->actual_length) {
		for (i = 0; i < urb->actual_length ; ++i) {
			/* if we insert more than TTY_FLIPBUF_SIZE characters, we drop them. */
			if(tty->flip.count >= TTY_FLIPBUF_SIZE) {
				tty_flip_buffer_push(tty);
			}
			/* this doesn't actually push the data through unless tty->low_latency is set */
			tty_insert_flip_char(tty, data[i], 0);
		}
	  	tty_flip_buffer_push(tty);
	}

	spin_lock(&priv->lock);

	/* Reduce urbs to do by one. */
	priv->rdtodo-=urb->actual_length;
	/* Just to be sure */
	if ( priv->rdtodo<0 ) priv->rdtodo = 0;
	todo = priv->rdtodo;

	spin_unlock(&priv->lock);

	dbg("%s - rdtodo: %d", __FUNCTION__, todo);

	/* Continue to read if we have still urbs to do. */
	if( todo /* || (urb->actual_length==port->bulk_in_endpointAddress)*/ ) {
		port->read_urb->dev = port->serial->dev;
		result = usb_submit_urb(port->read_urb, GFP_ATOMIC);
		if (result)
			err("%s - failed resubmitting read urb, error %d", __FUNCTION__, result);
		dbg("%s - usb_submit_urb(read urb)", __FUNCTION__);
	}
}

static void cyberjack_write_bulk_callback (struct urb *urb, struct pt_regs *regs)
{
	struct usb_serial_port *port = (struct usb_serial_port *)urb->context;
	struct cyberjack_private *priv = usb_get_serial_port_data(port);

	dbg("%s - port %d", __FUNCTION__, port->number);
	
	if (urb->status) {
		dbg("%s - nonzero write bulk status received: %d", __FUNCTION__, urb->status);
		return;
	}

	spin_lock(&priv->lock);

	/* only do something if we have more data to send */
	if( priv->wrfilled ) {
		int length, blksize, result;

		if (port->write_urb->status == -EINPROGRESS) {
			dbg("%s - already writing", __FUNCTION__);
			spin_unlock(&priv->lock);
			return;
		}

		dbg("%s - transmitting data (frame n)", __FUNCTION__);

		length = ((priv->wrfilled - priv->wrsent) > port->bulk_out_size) ?
			port->bulk_out_size : (priv->wrfilled - priv->wrsent);

		memcpy (port->write_urb->transfer_buffer, priv->wrbuf + priv->wrsent,
			length );
		priv->wrsent+=length;

		/* set up our urb */
		usb_fill_bulk_urb(port->write_urb, port->serial->dev, 
			      usb_sndbulkpipe(port->serial->dev, port->bulk_out_endpointAddress),
			      port->write_urb->transfer_buffer, length,
			      ((port->serial->type->write_bulk_callback) ? 
			       port->serial->type->write_bulk_callback : 
			       cyberjack_write_bulk_callback), 
			      port);

		/* send the data out the bulk port */
		result = usb_submit_urb(port->write_urb, GFP_ATOMIC);
		if (result) {
			err("%s - failed submitting write urb, error %d", __FUNCTION__, result);
			/* Throw away data. No better idea what to do with it. */
			priv->wrfilled=0;
			priv->wrsent=0;
			goto exit;
		}

		dbg("%s - priv->wrsent=%d", __FUNCTION__,priv->wrsent);
		dbg("%s - priv->wrfilled=%d", __FUNCTION__,priv->wrfilled);

		blksize = ((int)priv->wrbuf[2]<<8)+priv->wrbuf[1]+3;

		if( (priv->wrsent>=priv->wrfilled) || (priv->wrsent>=blksize) ) {
			dbg("%s - buffer cleaned", __FUNCTION__);
			memset( priv->wrbuf, 0, sizeof(priv->wrbuf) );
			priv->wrfilled=0;
			priv->wrsent=0;
		}
	}

exit:
	spin_unlock(&priv->lock);
	schedule_work(&port->work);
}

static int __init cyberjack_init (void)
{
	int retval;
	retval  = usb_serial_register(&cyberjack_device);
	if (retval)
		goto failed_usb_serial_register;
	retval = usb_register(&cyberjack_driver);
	if (retval) 
		goto failed_usb_register;

	info(DRIVER_VERSION " " DRIVER_AUTHOR);
	info(DRIVER_DESC);

	return 0;
failed_usb_register:
	usb_serial_deregister(&cyberjack_device);
failed_usb_serial_register:
	return retval;
}

static void __exit cyberjack_exit (void)
{
	usb_deregister (&cyberjack_driver);
	usb_serial_deregister (&cyberjack_device);
}

module_init(cyberjack_init);
module_exit(cyberjack_exit);

MODULE_AUTHOR( DRIVER_AUTHOR );
MODULE_DESCRIPTION( DRIVER_DESC );
MODULE_LICENSE("GPL");

module_param(debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Debug enabled or not");
