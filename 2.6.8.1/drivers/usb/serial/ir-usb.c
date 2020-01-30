/*
 * USB IR Dongle driver
 *
 *	Copyright (C) 2001-2002	Greg Kroah-Hartman (greg@kroah.com)
 *	Copyright (C) 2002	Gary Brubaker (xavyer@ix.netcom.com)
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version.
 *
 * This driver allows a USB IrDA device to be used as a "dumb" serial device.
 * This can be useful if you do not have access to a full IrDA stack on the
 * other side of the connection.  If you do have an IrDA stack on both devices,
 * please use the usb-irda driver, as it contains the proper error checking and
 * other goodness of a full IrDA stack.
 *
 * Portions of this driver were taken from drivers/net/irda/irda-usb.c, which
 * was written by Roman Weissgaerber <weissg@vienna.at>, Dag Brattli
 * <dag@brattli.net>, and Jean Tourrilhes <jt@hpl.hp.com>
 *
 * See Documentation/usb/usb-serial.txt for more information on using this driver
 *
 * 2002_Mar_07	greg kh
 *	moved some needed structures and #define values from the
 *	net/irda/irda-usb.h file into our file, as we don't want to depend on
 *	that codebase compiling correctly :)
 *
 * 2002_Jan_14  gb
 *	Added module parameter to force specific number of XBOFs.
 *	Added ir_xbof_change().
 *	Reorganized read_bulk_callback error handling.
 *	Switched from FILL_BULK_URB() to usb_fill_bulk_urb().
 *
 * 2001_Nov_08  greg kh
 *	Changed the irda_usb_find_class_desc() function based on comments and
 *	code from Martin Diehl.
 *
 * 2001_Nov_01	greg kh
 *	Added support for more IrDA USB devices.
 *	Added support for zero packet.  Added buffer override paramater, so
 *	users can transfer larger packets at once if they wish.  Both patches
 *	came from Dag Brattli <dag@obexcode.com>.
 *
 * 2001_Oct_07	greg kh
 *	initial version released.
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

/*
 * Version Information
 */
#define DRIVER_VERSION "v0.4"
#define DRIVER_AUTHOR "Greg Kroah-Hartman <greg@kroah.com>"
#define DRIVER_DESC "USB IR Dongle driver"

/* USB IrDA class spec information */
#define USB_CLASS_IRDA		0x02
#define USB_DT_IRDA		0x21
#define IU_REQ_GET_CLASS_DESC	0x06
#define SPEED_2400		0x01
#define SPEED_9600		0x02
#define SPEED_19200		0x03
#define SPEED_38400		0x04
#define SPEED_57600		0x05
#define SPEED_115200		0x06
#define SPEED_576000		0x07
#define SPEED_1152000		0x08
#define SPEED_4000000		0x09

struct irda_class_desc {
	u8	bLength;
	u8	bDescriptorType;
	u16	bcdSpecRevision;
	u8	bmDataSize;
	u8	bmWindowSize;
	u8	bmMinTurnaroundTime;
	u16	wBaudRate;
	u8	bmAdditionalBOFs;
	u8	bIrdaRateSniff;
	u8	bMaxUnicastList;
} __attribute__ ((packed));

static int debug;

/* if overridden by the user, then use their value for the size of the read and
 * write urbs */
static int buffer_size;
/* if overridden by the user, then use the specified number of XBOFs */
static int xbof = -1;

static int  ir_startup (struct usb_serial *serial);
static int  ir_open (struct usb_serial_port *port, struct file *filep);
static void ir_close (struct usb_serial_port *port, struct file *filep);
static int  ir_write (struct usb_serial_port *port, int from_user, const unsigned char *buf, int count);
static void ir_write_bulk_callback (struct urb *urb, struct pt_regs *regs);
static void ir_read_bulk_callback (struct urb *urb, struct pt_regs *regs);
static void ir_set_termios (struct usb_serial_port *port, struct termios *old_termios);

static u8 ir_baud = 0;
static u8 ir_xbof = 0;
static u8 ir_add_bof = 0;

static struct usb_device_id id_table [] = {
	{ USB_DEVICE(0x050f, 0x0180) },		/* KC Technology, KC-180 */
	{ USB_DEVICE(0x08e9, 0x0100) },		/* XTNDAccess */
	{ USB_DEVICE(0x09c4, 0x0011) },		/* ACTiSys ACT-IR2000U */
	{ USB_INTERFACE_INFO (USB_CLASS_APP_SPEC, USB_CLASS_IRDA, 0) },
	{ }					/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, id_table);

static struct usb_driver ir_driver = {
	.owner =	THIS_MODULE,
	.name =		"ir-usb",
	.probe =	usb_serial_probe,
	.disconnect =	usb_serial_disconnect,
	.id_table =	id_table,
};


static struct usb_serial_device_type ir_device = {
	.owner =		THIS_MODULE,
	.name =			"IR Dongle",
	.id_table =		id_table,
	.num_interrupt_in =	1,
	.num_bulk_in =		1,
	.num_bulk_out =		1,
	.num_ports =		1,
	.set_termios =		ir_set_termios,
	.attach =		ir_startup,
	.open =			ir_open,
	.close =		ir_close,
	.write =		ir_write,
	.write_bulk_callback =	ir_write_bulk_callback,
	.read_bulk_callback =	ir_read_bulk_callback,
};

static inline void irda_usb_dump_class_desc(struct irda_class_desc *desc)
{
	dbg("bLength=%x", desc->bLength);
	dbg("bDescriptorType=%x", desc->bDescriptorType);
	dbg("bcdSpecRevision=%x", desc->bcdSpecRevision); 
	dbg("bmDataSize=%x", desc->bmDataSize);
	dbg("bmWindowSize=%x", desc->bmWindowSize);
	dbg("bmMinTurnaroundTime=%d", desc->bmMinTurnaroundTime);
	dbg("wBaudRate=%x", desc->wBaudRate);
	dbg("bmAdditionalBOFs=%x", desc->bmAdditionalBOFs);
	dbg("bIrdaRateSniff=%x", desc->bIrdaRateSniff);
	dbg("bMaxUnicastList=%x", desc->bMaxUnicastList);
}

/*------------------------------------------------------------------*/
/*
 * Function irda_usb_find_class_desc(dev, ifnum)
 *
 *    Returns instance of IrDA class descriptor, or NULL if not found
 *
 * The class descriptor is some extra info that IrDA USB devices will
 * offer to us, describing their IrDA characteristics. We will use that in
 * irda_usb_init_qos()
 *
 * Based on the same function in drivers/net/irda/irda-usb.c
 */
static struct irda_class_desc *irda_usb_find_class_desc(struct usb_device *dev, unsigned int ifnum)
{
	struct irda_class_desc *desc;
	int ret;
		
	desc = kmalloc(sizeof (struct irda_class_desc), GFP_KERNEL);
	if (desc == NULL) 
		return NULL;
	memset(desc, 0, sizeof(struct irda_class_desc));
	
	ret = usb_control_msg(dev, usb_rcvctrlpipe(dev,0),
			IU_REQ_GET_CLASS_DESC,
			USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE,
			0, ifnum, desc, sizeof(*desc), HZ);
	
	dbg("%s -  ret=%d", __FUNCTION__, ret);
	if (ret < sizeof(*desc)) {
		dbg("%s - class descriptor read %s (%d)",
				__FUNCTION__, 
				(ret<0) ? "failed" : "too short",
				ret);
		goto error;
	}
	if (desc->bDescriptorType != USB_DT_IRDA) {
		dbg("%s - bad class descriptor type", __FUNCTION__);
		goto error;
	}
	
	irda_usb_dump_class_desc(desc);
	return desc;
error:
	kfree(desc);
	return NULL;
}


static u8 ir_xbof_change(u8 xbof)
{
	u8 result;
	/* reference irda-usb.c */
	switch(xbof) {
		case 48: result = 0x10; break;
		case 28:
		case 24: result = 0x20; break;
		default:
		case 12: result = 0x30; break;
		case  5:
		case  6: result = 0x40; break;
		case  3: result = 0x50; break;
		case  2: result = 0x60; break;
		case  1: result = 0x70; break;
		case  0: result = 0x80; break;
	}
	return(result);
}


static int ir_startup (struct usb_serial *serial)
{
	struct irda_class_desc *irda_desc;

	irda_desc = irda_usb_find_class_desc (serial->dev, 0);
	if (irda_desc == NULL) {
		dev_err (&serial->dev->dev, "IRDA class descriptor not found, device not bound\n");
		return -ENODEV;
	}

	dbg ("%s - Baud rates supported:%s%s%s%s%s%s%s%s%s",
		__FUNCTION__,
		(irda_desc->wBaudRate & 0x0001) ? " 2400"    : "",
		(irda_desc->wBaudRate & 0x0002) ? " 9600"    : "",
		(irda_desc->wBaudRate & 0x0004) ? " 19200"   : "",
		(irda_desc->wBaudRate & 0x0008) ? " 38400"   : "",
		(irda_desc->wBaudRate & 0x0010) ? " 57600"   : "",
		(irda_desc->wBaudRate & 0x0020) ? " 115200"  : "",
		(irda_desc->wBaudRate & 0x0040) ? " 576000"  : "",
		(irda_desc->wBaudRate & 0x0080) ? " 1152000" : "",
		(irda_desc->wBaudRate & 0x0100) ? " 4000000" : "");

	switch( irda_desc->bmAdditionalBOFs ) {
		case 0x01: ir_add_bof = 48; break;
		case 0x02: ir_add_bof = 24; break;
		case 0x04: ir_add_bof = 12; break;
		case 0x08: ir_add_bof =  6; break;
		case 0x10: ir_add_bof =  3; break;
		case 0x20: ir_add_bof =  2; break;
		case 0x40: ir_add_bof =  1; break;
		case 0x80: ir_add_bof =  0; break;
		default:;
	}

	kfree (irda_desc);

	return 0;		
}

static int ir_open (struct usb_serial_port *port, struct file *filp)
{
	char *buffer;
	int result = 0;

	dbg("%s - port %d", __FUNCTION__, port->number);

	if (buffer_size) {
		/* override the default buffer sizes */
		buffer = kmalloc (buffer_size, GFP_KERNEL);
		if (!buffer) {
			dev_err (&port->dev, "%s - out of memory.\n", __FUNCTION__);
			return -ENOMEM;
		}
		kfree (port->read_urb->transfer_buffer);
		port->read_urb->transfer_buffer = buffer;
		port->read_urb->transfer_buffer_length = buffer_size;

		buffer = kmalloc (buffer_size, GFP_KERNEL);
		if (!buffer) {
			dev_err (&port->dev, "%s - out of memory.\n", __FUNCTION__);
			return -ENOMEM;
		}
		kfree (port->write_urb->transfer_buffer);
		port->write_urb->transfer_buffer = buffer;
		port->write_urb->transfer_buffer_length = buffer_size;
		port->bulk_out_size = buffer_size;
	}

	/* Start reading from the device */
	usb_fill_bulk_urb (
		port->read_urb,
		port->serial->dev, 
		usb_rcvbulkpipe(port->serial->dev, port->bulk_in_endpointAddress),
		port->read_urb->transfer_buffer,
		port->read_urb->transfer_buffer_length,
		ir_read_bulk_callback,
		port);
	result = usb_submit_urb(port->read_urb, GFP_KERNEL);
	if (result)
		dev_err(&port->dev, "%s - failed submitting read urb, error %d\n", __FUNCTION__, result);

	return result;
}

static void ir_close (struct usb_serial_port *port, struct file * filp)
{
	dbg("%s - port %d", __FUNCTION__, port->number);
			 
	/* shutdown our bulk read */
	usb_unlink_urb (port->read_urb);
}

static int ir_write (struct usb_serial_port *port, int from_user, const unsigned char *buf, int count)
{
	unsigned char *transfer_buffer;
	int result;
	int transfer_size;

	dbg("%s - port = %d, count = %d", __FUNCTION__, port->number, count);

	if (!port->tty) {
		dev_err (&port->dev, "%s - no tty???\n", __FUNCTION__);
		return 0;
	}

	if (count == 0)
		return 0;

	if (port->write_urb->status == -EINPROGRESS) {
		dbg ("%s - already writing", __FUNCTION__);
		return 0;
	}

	transfer_buffer = port->write_urb->transfer_buffer;
	transfer_size = min(count, port->bulk_out_size - 1);

	/*
	 * The first byte of the packet we send to the device contains an
	 * inband header which indicates an additional number of BOFs and
	 * a baud rate change.
	 *
	 * See section 5.4.2.2 of the USB IrDA spec.
	 */
	*transfer_buffer = ir_xbof | ir_baud;
	++transfer_buffer;

	if (from_user) {
		if (copy_from_user (transfer_buffer, buf, transfer_size))
			return -EFAULT;
	} else {
		memcpy (transfer_buffer, buf, transfer_size);
	}

	usb_fill_bulk_urb (
		port->write_urb,
		port->serial->dev,
		usb_sndbulkpipe(port->serial->dev,
			port->bulk_out_endpointAddress),
		port->write_urb->transfer_buffer,
		transfer_size + 1,
		ir_write_bulk_callback,
		port);

	port->write_urb->transfer_flags = URB_ZERO_PACKET;

	result = usb_submit_urb (port->write_urb, GFP_ATOMIC);
	if (result)
		dev_err(&port->dev, "%s - failed submitting write urb, error %d\n", __FUNCTION__, result);
	else
		result = transfer_size;

	return result;
}

static void ir_write_bulk_callback (struct urb *urb, struct pt_regs *regs)
{
	struct usb_serial_port *port = (struct usb_serial_port *)urb->context;

	dbg("%s - port %d", __FUNCTION__, port->number);
	
	if (urb->status) {
		dbg("%s - nonzero write bulk status received: %d", __FUNCTION__, urb->status);
		return;
	}

	usb_serial_debug_data (
		debug,
		&port->dev,
		__FUNCTION__,
		urb->actual_length,
		urb->transfer_buffer);

	schedule_work(&port->work);
}

static void ir_read_bulk_callback (struct urb *urb, struct pt_regs *regs)
{
	struct usb_serial_port *port = (struct usb_serial_port *)urb->context;
	struct tty_struct *tty;
	unsigned char *data = urb->transfer_buffer;
	int result;

	dbg("%s - port %d", __FUNCTION__, port->number);

	if (!port->open_count) {
		dbg("%s - port closed.", __FUNCTION__);
		return;
	}

	switch (urb->status) {

		case 0: /* Successful */

			/*
			 * The first byte of the packet we get from the device
			 * contains a busy indicator and baud rate change.
			 * See section 5.4.1.2 of the USB IrDA spec.
			 */
			if ((*data & 0x0f) > 0)
				ir_baud = *data & 0x0f;

			usb_serial_debug_data (
				debug,
				&port->dev,
				__FUNCTION__,
				urb->actual_length,
				data);

			/*
			 * Bypass flip-buffers, and feed the ldisc directly
			 * due to our potentially large buffer size.  Since we
			 * used to set low_latency, this is exactly what the
			 * tty layer did anyway :)
			 */
			tty = port->tty;

			tty->ldisc.receive_buf(
				tty,
				data+1,
				NULL,
				urb->actual_length-1);

			/*
			 * No break here.
			 * We want to resubmit the urb so we can read
			 * again.
			 */

		case -EPROTO: /* taking inspiration from pl2303.c */

			/* Continue trying to always read */
			usb_fill_bulk_urb (
				port->read_urb,
				port->serial->dev, 
				usb_rcvbulkpipe(port->serial->dev,
					port->bulk_in_endpointAddress),
				port->read_urb->transfer_buffer,
				port->read_urb->transfer_buffer_length,
				ir_read_bulk_callback,
				port);

			result = usb_submit_urb(port->read_urb, GFP_ATOMIC);
			if (result)
				dev_err(&port->dev, "%s - failed resubmitting read urb, error %d\n",
					__FUNCTION__, result);

			break ;

		default:
			dbg("%s - nonzero read bulk status received: %d",
				__FUNCTION__, 
				urb->status);
			break ;

	}

	return;
}

static void ir_set_termios (struct usb_serial_port *port, struct termios *old_termios)
{
	unsigned char *transfer_buffer;
	unsigned int cflag;
	int result;

	dbg("%s - port %d", __FUNCTION__, port->number);

	if ((!port->tty) || (!port->tty->termios)) {
		dbg("%s - no tty structures", __FUNCTION__);
		return;
	}

	cflag = port->tty->termios->c_cflag;
	/* check that they really want us to change something */
	if (old_termios) {
		if ((cflag == old_termios->c_cflag) &&
		    (RELEVANT_IFLAG(port->tty->termios->c_iflag) == RELEVANT_IFLAG(old_termios->c_iflag))) {
			dbg("%s - nothing to change...", __FUNCTION__);
			return;
		}
	}

	/* All we can change is the baud rate */
	if (cflag & CBAUD) {

		dbg ("%s - asking for baud %d",
			__FUNCTION__,
			tty_get_baud_rate(port->tty));

		/* 
		 * FIXME, we should compare the baud request against the
		 * capability stated in the IR header that we got in the
		 * startup function.
		 */
		switch (cflag & CBAUD) {
			case B2400:    ir_baud = SPEED_2400;    break;
			default:
			case B9600:    ir_baud = SPEED_9600;    break;
			case B19200:   ir_baud = SPEED_19200;   break;
			case B38400:   ir_baud = SPEED_38400;   break;
			case B57600:   ir_baud = SPEED_57600;   break;
			case B115200:  ir_baud = SPEED_115200;  break;
			case B576000:  ir_baud = SPEED_576000;  break;
			case B1152000: ir_baud = SPEED_1152000; break;
#ifdef B4000000
			case B4000000: ir_baud = SPEED_4000000; break;
#endif
		}

		if (xbof == -1) {
			ir_xbof = ir_xbof_change(ir_add_bof);
		} else {
			ir_xbof = ir_xbof_change(xbof) ;
		}

		/* Notify the tty driver that the termios have changed. */
		port->tty->ldisc.set_termios(port->tty, NULL);

		/* FIXME need to check to see if our write urb is busy right
		 * now, or use a urb pool.
		 *
		 * send the baud change out on an "empty" data packet
		 */
		transfer_buffer = port->write_urb->transfer_buffer;
		*transfer_buffer = ir_xbof | ir_baud;

		usb_fill_bulk_urb (
			port->write_urb,
			port->serial->dev,
			usb_sndbulkpipe(port->serial->dev,
				port->bulk_out_endpointAddress),
			port->write_urb->transfer_buffer,
			1,
			ir_write_bulk_callback,
			port);

		port->write_urb->transfer_flags = URB_ZERO_PACKET;

		result = usb_submit_urb (port->write_urb, GFP_KERNEL);
		if (result)
			dev_err(&port->dev, "%s - failed submitting write urb, error %d\n", __FUNCTION__, result);
	}
	return;
}


static int __init ir_init (void)
{
	int retval;
	retval = usb_serial_register(&ir_device);
	if (retval)
		goto failed_usb_serial_register;
	retval = usb_register(&ir_driver);
	if (retval) 
		goto failed_usb_register;
	info(DRIVER_DESC " " DRIVER_VERSION);
	return 0;
failed_usb_register:
	usb_serial_deregister(&ir_device);
failed_usb_serial_register:
	return retval;
}


static void __exit ir_exit (void)
{
	usb_deregister (&ir_driver);
	usb_serial_deregister (&ir_device);
}


module_init(ir_init);
module_exit(ir_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

module_param(debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Debug enabled or not");
module_param(xbof, int, 0);
MODULE_PARM_DESC(xbof, "Force specific number of XBOFs");
module_param(buffer_size, int, 0);
MODULE_PARM_DESC(buffer_size, "Size of the transfer buffers");

