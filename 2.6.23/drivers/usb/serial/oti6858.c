/*
 * Ours Technology Inc. OTi-6858 USB to serial adapter driver.
 *
 * Copyleft  (C) 2007 Kees Lemmens (adapted for kernel 2.6.20)
 * Copyright (C) 2006 Tomasz Michal Lukaszewski (FIXME: add e-mail)
 * Copyright (C) 2001-2004 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2003 IBM Corp.
 *
 * Many thanks to the authors of pl2303 driver: all functions in this file
 * are heavily based on pl2303 code, buffering code is a 1-to-1 copy.
 *
 * Warning! You use this driver on your own risk! The only official
 * description of this device I have is datasheet from manufacturer,
 * and it doesn't contain almost any information needed to write a driver.
 * Almost all knowlegde used while writing this driver was gathered by:
 *  - analyzing traffic between device and the M$ Windows 2000 driver,
 *  - trying different bit combinations and checking pin states
 *    with a voltmeter,
 *  - receiving malformed frames and producing buffer overflows
 *    to learn how errors are reported,
 * So, THIS CODE CAN DESTROY OTi-6858 AND ANY OTHER DEVICES, THAT ARE
 * CONNECTED TO IT!
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License.
 *
 * See Documentation/usb/usb-serial.txt for more information on using this driver
 *
 * TODO:
 *  - implement correct flushing for ioctls and oti6858_close()
 *  - check how errors (rx overflow, parity error, framing error) are reported
 *  - implement oti6858_break_ctl()
 *  - implement more ioctls
 *  - test/implement flow control
 *  - allow setting custom baud rates
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/serial.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/usb/serial.h>
#include <asm/uaccess.h>
#include "oti6858.h"

#define OTI6858_DESCRIPTION \
	"Ours Technology Inc. OTi-6858 USB to serial adapter driver"
#define OTI6858_AUTHOR "Tomasz Michal Lukaszewski <FIXME@FIXME>"
#define OTI6858_VERSION "0.1"

static struct usb_device_id id_table [] = {
	{ USB_DEVICE(OTI6858_VENDOR_ID, OTI6858_PRODUCT_ID) },
	{ }
};

MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver oti6858_driver = {
	.name =		"oti6858",
	.probe =	usb_serial_probe,
	.disconnect =	usb_serial_disconnect,
	.id_table =	id_table,
	.no_dynamic_id = 	1,
};

static int debug;


/* buffering code, copied from pl2303 driver */
#define PL2303_BUF_SIZE		1024
#define PL2303_TMP_BUF_SIZE	1024

struct pl2303_buf {
	unsigned int	buf_size;
	char		*buf_buf;
	char		*buf_get;
	char		*buf_put;
};

/* requests */
#define	OTI6858_REQ_GET_STATUS		(USB_DIR_IN | USB_TYPE_VENDOR | 0x00)
#define	OTI6858_REQ_T_GET_STATUS	0x01

#define	OTI6858_REQ_SET_LINE		(USB_DIR_OUT | USB_TYPE_VENDOR | 0x00)
#define	OTI6858_REQ_T_SET_LINE		0x00

#define	OTI6858_REQ_CHECK_TXBUFF	(USB_DIR_IN | USB_TYPE_VENDOR | 0x01)
#define	OTI6858_REQ_T_CHECK_TXBUFF	0x00

/* format of the control packet */
struct oti6858_control_pkt {
	u16	divisor;	/* baud rate = 96000000 / (16 * divisor), LE */
#define OTI6858_MAX_BAUD_RATE	3000000
	u8	frame_fmt;
#define FMT_STOP_BITS_MASK	0xc0
#define FMT_STOP_BITS_1		0x00
#define FMT_STOP_BITS_2		0x40	/* 1.5 stop bits if FMT_DATA_BITS_5 */
#define FMT_PARITY_MASK		0x38
#define FMT_PARITY_NONE		0x00
#define FMT_PARITY_ODD		0x08
#define FMT_PARITY_EVEN		0x18
#define FMT_PARITY_MARK		0x28
#define FMT_PARITY_SPACE	0x38
#define FMT_DATA_BITS_MASK	0x03
#define FMT_DATA_BITS_5		0x00
#define FMT_DATA_BITS_6		0x01
#define FMT_DATA_BITS_7		0x02
#define FMT_DATA_BITS_8		0x03
	u8	something;	/* always equals 0x43 */
	u8	control;	/* settings of flow control lines */
#define CONTROL_MASK		0x0c
#define CONTROL_DTR_HIGH	0x08
#define CONTROL_RTS_HIGH	0x04
	u8	tx_status;
#define	TX_BUFFER_EMPTIED	0x09
	u8	pin_state;
#define PIN_MASK		0x3f
#define PIN_RTS			0x20	/* output pin */
#define PIN_CTS			0x10	/* input pin, active low */
#define PIN_DSR			0x08	/* input pin, active low */
#define PIN_DTR			0x04	/* output pin */
#define PIN_RI			0x02	/* input pin, active low */
#define PIN_DCD			0x01	/* input pin, active low */
	u8	rx_bytes_avail;		/* number of bytes in rx buffer */;
};

#define OTI6858_CTRL_PKT_SIZE	sizeof(struct oti6858_control_pkt)
#define OTI6858_CTRL_EQUALS_PENDING(a, priv) \
	(    ((a)->divisor == (priv)->pending_setup.divisor) \
	  && ((a)->control == (priv)->pending_setup.control) \
	  && ((a)->frame_fmt == (priv)->pending_setup.frame_fmt) )

/* function prototypes */
static int oti6858_open(struct usb_serial_port *port, struct file *filp);
static void oti6858_close(struct usb_serial_port *port, struct file *filp);
static void oti6858_set_termios(struct usb_serial_port *port,
				struct ktermios *old);
static int oti6858_ioctl(struct usb_serial_port *port, struct file *file,
			unsigned int cmd, unsigned long arg);
static void oti6858_read_int_callback(struct urb *urb);
static void oti6858_read_bulk_callback(struct urb *urb);
static void oti6858_write_bulk_callback(struct urb *urb);
static int oti6858_write(struct usb_serial_port *port,
			const unsigned char *buf, int count);
static int oti6858_write_room(struct usb_serial_port *port);
static void oti6858_break_ctl(struct usb_serial_port *port, int break_state);
static int oti6858_chars_in_buffer(struct usb_serial_port *port);
static int oti6858_tiocmget(struct usb_serial_port *port, struct file *file);
static int oti6858_tiocmset(struct usb_serial_port *port, struct file *file,
				unsigned int set, unsigned int clear);
static int oti6858_startup(struct usb_serial *serial);
static void oti6858_shutdown(struct usb_serial *serial);

/* functions operating on buffers */
static struct pl2303_buf *pl2303_buf_alloc(unsigned int size);
static void pl2303_buf_free(struct pl2303_buf *pb);
static void pl2303_buf_clear(struct pl2303_buf *pb);
static unsigned int pl2303_buf_data_avail(struct pl2303_buf *pb);
static unsigned int pl2303_buf_space_avail(struct pl2303_buf *pb);
static unsigned int pl2303_buf_put(struct pl2303_buf *pb, const char *buf,
					unsigned int count);
static unsigned int pl2303_buf_get(struct pl2303_buf *pb, char *buf,
					unsigned int count);


/* device info */
static struct usb_serial_driver oti6858_device = {
	.driver = {
		.owner =	THIS_MODULE,
		.name =		"oti6858",
	},
	.id_table =		id_table,
	.num_interrupt_in =	1,
	.num_bulk_in =		1,
	.num_bulk_out =		1,
	.num_ports =		1,
	.open =			oti6858_open,
	.close =		oti6858_close,
	.write =		oti6858_write,
	.ioctl =		oti6858_ioctl,
	.break_ctl =		oti6858_break_ctl,
	.set_termios =		oti6858_set_termios,
	.tiocmget =		oti6858_tiocmget,
	.tiocmset =		oti6858_tiocmset,
	.read_bulk_callback =	oti6858_read_bulk_callback,
	.read_int_callback =	oti6858_read_int_callback,
	.write_bulk_callback =	oti6858_write_bulk_callback,
	.write_room =		oti6858_write_room,
	.chars_in_buffer =	oti6858_chars_in_buffer,
	.attach =		oti6858_startup,
	.shutdown =		oti6858_shutdown,
};

struct oti6858_private {
	spinlock_t lock;

	struct pl2303_buf *buf;
	struct oti6858_control_pkt status;

	struct {
		u8 read_urb_in_use;
		u8 write_urb_in_use;
		u8 termios_initialized;
	} flags;
	struct delayed_work delayed_write_work;

	struct {
		u16 divisor;
		u8 frame_fmt;
		u8 control;
	} pending_setup;
	u8 transient;
	u8 setup_done;
	struct delayed_work delayed_setup_work;

	wait_queue_head_t intr_wait;
        struct usb_serial_port *port;   /* USB port with which associated */
};

#undef dbg
/* #define dbg(format, arg...) printk(KERN_INFO "%s: " format "\n", __FILE__, ## arg) */
#define dbg(format, arg...) printk(KERN_INFO "" format "\n", ## arg)

static void setup_line(struct work_struct *work)
{
	struct oti6858_private *priv = container_of(work, struct oti6858_private, delayed_setup_work.work);
	struct usb_serial_port *port = priv->port;
	struct oti6858_control_pkt *new_setup;
	unsigned long flags;
	int result;

	dbg("%s(port = %d)", __FUNCTION__, port->number);

	if ((new_setup = kmalloc(OTI6858_CTRL_PKT_SIZE, GFP_KERNEL)) == NULL) {
		dev_err(&port->dev, "%s(): out of memory!\n", __FUNCTION__);
		/* we will try again */
		schedule_delayed_work(&priv->delayed_setup_work, msecs_to_jiffies(2));
		return;
	}

	result = usb_control_msg(port->serial->dev,
				usb_rcvctrlpipe(port->serial->dev, 0),
				OTI6858_REQ_T_GET_STATUS,
				OTI6858_REQ_GET_STATUS,
				0, 0,
				new_setup, OTI6858_CTRL_PKT_SIZE,
				100);

	if (result != OTI6858_CTRL_PKT_SIZE) {
		dev_err(&port->dev, "%s(): error reading status", __FUNCTION__);
		kfree(new_setup);
		/* we will try again */
		schedule_delayed_work(&priv->delayed_setup_work, msecs_to_jiffies(2));
		return;
	}

	spin_lock_irqsave(&priv->lock, flags);
	if (!OTI6858_CTRL_EQUALS_PENDING(new_setup, priv)) {
		new_setup->divisor = priv->pending_setup.divisor;
		new_setup->control = priv->pending_setup.control;
		new_setup->frame_fmt = priv->pending_setup.frame_fmt;

		spin_unlock_irqrestore(&priv->lock, flags);
		result = usb_control_msg(port->serial->dev,
					usb_sndctrlpipe(port->serial->dev, 0),
					OTI6858_REQ_T_SET_LINE,
					OTI6858_REQ_SET_LINE,
					0, 0,
					new_setup, OTI6858_CTRL_PKT_SIZE,
					100);
	} else {
		spin_unlock_irqrestore(&priv->lock, flags);
		result = 0;
	}
	kfree(new_setup);

	spin_lock_irqsave(&priv->lock, flags);
	if (result != OTI6858_CTRL_PKT_SIZE)
		priv->transient = 0;
	priv->setup_done = 1;
	spin_unlock_irqrestore(&priv->lock, flags);

	dbg("%s(): submitting interrupt urb", __FUNCTION__);
	port->interrupt_in_urb->dev = port->serial->dev;
	result = usb_submit_urb(port->interrupt_in_urb, GFP_ATOMIC);
	if (result != 0) {
		dev_err(&port->dev, "%s(): usb_submit_urb() failed"
				" with error %d\n", __FUNCTION__, result);
	}
}

void send_data(struct work_struct *work)
{
	struct oti6858_private *priv = container_of(work, struct oti6858_private, delayed_write_work.work);
	struct usb_serial_port *port = priv->port;
	int count = 0, result;
	unsigned long flags;
	unsigned char allow;

	dbg("%s(port = %d)", __FUNCTION__, port->number);

	spin_lock_irqsave(&priv->lock, flags);
	if (priv->flags.write_urb_in_use) {
		spin_unlock_irqrestore(&priv->lock, flags);
		schedule_delayed_work(&priv->delayed_write_work, msecs_to_jiffies(2));
		return;
	}
	priv->flags.write_urb_in_use = 1;

	count = pl2303_buf_data_avail(priv->buf);
	spin_unlock_irqrestore(&priv->lock, flags);
	if (count > port->bulk_out_size)
		count = port->bulk_out_size;

	if (count != 0) {
		result = usb_control_msg(port->serial->dev,
				usb_rcvctrlpipe(port->serial->dev, 0),
				OTI6858_REQ_T_CHECK_TXBUFF,
				OTI6858_REQ_CHECK_TXBUFF,
				count, 0, &allow, 1, 100);
		if (result != 1 || allow != 0)
			count = 0;
	}

	if (count == 0) {
		priv->flags.write_urb_in_use = 0;

		dbg("%s(): submitting interrupt urb", __FUNCTION__);
		port->interrupt_in_urb->dev = port->serial->dev;
		result = usb_submit_urb(port->interrupt_in_urb, GFP_ATOMIC);
		if (result != 0) {
			dev_err(&port->dev, "%s(): usb_submit_urb() failed"
				" with error %d\n", __FUNCTION__, result);
		}
		return;
	}

	spin_lock_irqsave(&priv->lock, flags);
	pl2303_buf_get(priv->buf, port->write_urb->transfer_buffer, count);
	spin_unlock_irqrestore(&priv->lock, flags);

	port->write_urb->transfer_buffer_length = count;
	port->write_urb->dev = port->serial->dev;
	result = usb_submit_urb(port->write_urb, GFP_ATOMIC);
	if (result != 0) {
		dev_err(&port->dev, "%s(): usb_submit_urb() failed"
			       " with error %d\n", __FUNCTION__, result);
		priv->flags.write_urb_in_use = 0;
	}

	usb_serial_port_softint(port);
}

static int oti6858_startup(struct usb_serial *serial)
{
        struct usb_serial_port *port = serial->port[0];
        struct oti6858_private *priv;
	int i;

	for (i = 0; i < serial->num_ports; ++i) {
		priv = kzalloc(sizeof(struct oti6858_private), GFP_KERNEL);
		if (!priv)
			break;
		priv->buf = pl2303_buf_alloc(PL2303_BUF_SIZE);
		if (priv->buf == NULL) {
			kfree(priv);
			break;
		}

		spin_lock_init(&priv->lock);
		init_waitqueue_head(&priv->intr_wait);
//		INIT_WORK(&priv->setup_work, setup_line, serial->port[i]);
//		INIT_WORK(&priv->write_work, send_data, serial->port[i]);
		priv->port = port;
		INIT_DELAYED_WORK(&priv->delayed_setup_work, setup_line);
		INIT_DELAYED_WORK(&priv->delayed_write_work, send_data);

		usb_set_serial_port_data(serial->port[i], priv);
	}
	if (i == serial->num_ports)
		return 0;

	for (--i; i >= 0; --i) {
		priv = usb_get_serial_port_data(serial->port[i]);
		pl2303_buf_free(priv->buf);
		kfree(priv);
		usb_set_serial_port_data(serial->port[i], NULL);
	}
	return -ENOMEM;
}

static int oti6858_write(struct usb_serial_port *port,
			const unsigned char *buf, int count)
{
	struct oti6858_private *priv = usb_get_serial_port_data(port);
	unsigned long flags;

	dbg("%s(port = %d, count = %d)", __FUNCTION__, port->number, count);

	if (!count)
		return count;

	spin_lock_irqsave(&priv->lock, flags);
	count = pl2303_buf_put(priv->buf, buf, count);
	spin_unlock_irqrestore(&priv->lock, flags);

	return count;
}

static int oti6858_write_room(struct usb_serial_port *port)
{
	struct oti6858_private *priv = usb_get_serial_port_data(port);
	int room = 0;
	unsigned long flags;

	dbg("%s(port = %d)", __FUNCTION__, port->number);

	spin_lock_irqsave(&priv->lock, flags);
	room = pl2303_buf_space_avail(priv->buf);
	spin_unlock_irqrestore(&priv->lock, flags);

	return room;
}

static int oti6858_chars_in_buffer(struct usb_serial_port *port)
{
	struct oti6858_private *priv = usb_get_serial_port_data(port);
	int chars = 0;
	unsigned long flags;

	dbg("%s(port = %d)", __FUNCTION__, port->number);

	spin_lock_irqsave(&priv->lock, flags);
	chars = pl2303_buf_data_avail(priv->buf);
	spin_unlock_irqrestore(&priv->lock, flags);

	return chars;
}

static void oti6858_set_termios(struct usb_serial_port *port,
				struct ktermios *old_termios)
{
	struct oti6858_private *priv = usb_get_serial_port_data(port);
	unsigned long flags;
	unsigned int cflag;
	u8 frame_fmt, control;
	u16 divisor;
	int br;

	dbg("%s(port = %d)", __FUNCTION__, port->number);

	if ((!port->tty) || (!port->tty->termios)) {
		dbg("%s(): no tty structures", __FUNCTION__);
		return;
	}

	spin_lock_irqsave(&priv->lock, flags);
	if (!priv->flags.termios_initialized) {
		*(port->tty->termios) = tty_std_termios;
		port->tty->termios->c_cflag = B38400 | CS8 | CREAD | HUPCL | CLOCAL;
		priv->flags.termios_initialized = 1;
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	cflag = port->tty->termios->c_cflag;

	spin_lock_irqsave(&priv->lock, flags);
	divisor = priv->pending_setup.divisor;
	frame_fmt = priv->pending_setup.frame_fmt;
	control = priv->pending_setup.control;
	spin_unlock_irqrestore(&priv->lock, flags);

	frame_fmt &= ~FMT_DATA_BITS_MASK;
	switch (cflag & CSIZE) {
		case CS5:
			frame_fmt |= FMT_DATA_BITS_5;
			break;
		case CS6:
			frame_fmt |= FMT_DATA_BITS_6;
			break;
		case CS7:
			frame_fmt |= FMT_DATA_BITS_7;
			break;
		default:
		case CS8:
			frame_fmt |= FMT_DATA_BITS_8;
			break;
	}

	/* manufacturer claims that this device can work with baud rates
	 * up to 3 Mbps; I've tested it only on 115200 bps, so I can't
	 * guarantee that any other baud rate will work (especially
	 * the higher ones)
	 */
	br = tty_get_baud_rate(port->tty);
	if (br == 0) {
		divisor = 0;
	} else if (br <= OTI6858_MAX_BAUD_RATE) {
		int real_br;

		divisor = (96000000 + 8 * br) / (16 * br);
		real_br = 96000000 / (16 * divisor);
		if ((((real_br - br) * 100 + br - 1) / br) > 2) {
			dbg("%s(): baud rate %d is invalid", __FUNCTION__, br);
			return;
		}
		divisor = cpu_to_le16(divisor);
	} else {
		dbg("%s(): baud rate %d is too high", __FUNCTION__, br);
		return;
	}

	frame_fmt &= ~FMT_STOP_BITS_MASK;
	if ((cflag & CSTOPB) != 0) {
		frame_fmt |= FMT_STOP_BITS_2;
	} else {
		frame_fmt |= FMT_STOP_BITS_1;
	}

	frame_fmt &= ~FMT_PARITY_MASK;
	if ((cflag & PARENB) != 0) {
		if ((cflag & PARODD) != 0) {
			frame_fmt |= FMT_PARITY_ODD;
		} else {
			frame_fmt |= FMT_PARITY_EVEN;
		}
	} else {
		frame_fmt |= FMT_PARITY_NONE;
	}

	control &= ~CONTROL_MASK;
	if ((cflag & CRTSCTS) != 0)
		control |= (CONTROL_DTR_HIGH | CONTROL_RTS_HIGH);

	/* change control lines if we are switching to or from B0 */
	/* FIXME:
	spin_lock_irqsave(&priv->lock, flags);
	control = priv->line_control;
	if ((cflag & CBAUD) == B0)
		priv->line_control &= ~(CONTROL_DTR | CONTROL_RTS);
	else
		priv->line_control |= (CONTROL_DTR | CONTROL_RTS);
	if (control != priv->line_control) {
		control = priv->line_control;
		spin_unlock_irqrestore(&priv->lock, flags);
		set_control_lines(serial->dev, control);
	} else {
		spin_unlock_irqrestore(&priv->lock, flags);
	}
	*/

	spin_lock_irqsave(&priv->lock, flags);
	if (divisor != priv->pending_setup.divisor
			|| control != priv->pending_setup.control
			|| frame_fmt != priv->pending_setup.frame_fmt) {
		priv->pending_setup.divisor = divisor;
		priv->pending_setup.control = control;
		priv->pending_setup.frame_fmt = frame_fmt;
	}
	spin_unlock_irqrestore(&priv->lock, flags);
}

static int oti6858_open(struct usb_serial_port *port, struct file *filp)
{
	struct oti6858_private *priv = usb_get_serial_port_data(port);
	struct ktermios tmp_termios;
	struct usb_serial *serial = port->serial;
	struct oti6858_control_pkt *buf;
	unsigned long flags;
	int result;

	dbg("%s(port = %d)", __FUNCTION__, port->number);

	usb_clear_halt(serial->dev, port->write_urb->pipe);
	usb_clear_halt(serial->dev, port->read_urb->pipe);

	if (port->open_count != 1)
		return 0;

	if ((buf = kmalloc(OTI6858_CTRL_PKT_SIZE, GFP_KERNEL)) == NULL) {
		dev_err(&port->dev, "%s(): out of memory!\n", __FUNCTION__);
		return -ENOMEM;
	}

	result = usb_control_msg(serial->dev, usb_rcvctrlpipe(serial->dev, 0),
				OTI6858_REQ_T_GET_STATUS,
				OTI6858_REQ_GET_STATUS,
				0, 0,
				buf, OTI6858_CTRL_PKT_SIZE,
				100);
	if (result != OTI6858_CTRL_PKT_SIZE) {
		/* assume default (after power-on reset) values */
		buf->divisor = cpu_to_le16(0x009c);	/* 38400 bps */
		buf->frame_fmt = 0x03;	/* 8N1 */
		buf->something = 0x43;
		buf->control = 0x4c;	/* DTR, RTS */
		buf->tx_status = 0x00;
		buf->pin_state = 0x5b;	/* RTS, CTS, DSR, DTR, RI, DCD */
		buf->rx_bytes_avail = 0x00;
	}

	spin_lock_irqsave(&priv->lock, flags);
	memcpy(&priv->status, buf, OTI6858_CTRL_PKT_SIZE);
	priv->pending_setup.divisor = buf->divisor;
	priv->pending_setup.frame_fmt = buf->frame_fmt;
	priv->pending_setup.control = buf->control;
	spin_unlock_irqrestore(&priv->lock, flags);
	kfree(buf);

	dbg("%s(): submitting interrupt urb", __FUNCTION__);
	port->interrupt_in_urb->dev = serial->dev;
	result = usb_submit_urb(port->interrupt_in_urb, GFP_KERNEL);
	if (result != 0) {
		dev_err(&port->dev, "%s(): usb_submit_urb() failed"
			       " with error %d\n", __FUNCTION__, result);
		oti6858_close(port, NULL);
		return -EPROTO;
	}

	/* setup termios */
	if (port->tty)
		oti6858_set_termios(port, &tmp_termios);

	return 0;
}

static void oti6858_close(struct usb_serial_port *port, struct file *filp)
{
	struct oti6858_private *priv = usb_get_serial_port_data(port);
	unsigned long flags;
	long timeout;
	wait_queue_t wait;

	dbg("%s(port = %d)", __FUNCTION__, port->number);

	/* wait for data to drain from the buffer */
	spin_lock_irqsave(&priv->lock, flags);
	timeout = 30 * HZ;	/* PL2303_CLOSING_WAIT */
	init_waitqueue_entry(&wait, current);
	add_wait_queue(&port->tty->write_wait, &wait);
	dbg("%s(): entering wait loop", __FUNCTION__);
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (pl2303_buf_data_avail(priv->buf) == 0
		|| timeout == 0 || signal_pending(current)
		|| !usb_get_intfdata(port->serial->interface))	/* disconnect */
			break;
		spin_unlock_irqrestore(&priv->lock, flags);
		timeout = schedule_timeout(timeout);
		spin_lock_irqsave(&priv->lock, flags);
	}
	set_current_state(TASK_RUNNING);
	remove_wait_queue(&port->tty->write_wait, &wait);
	dbg("%s(): after wait loop", __FUNCTION__);

	/* clear out any remaining data in the buffer */
	pl2303_buf_clear(priv->buf);
	spin_unlock_irqrestore(&priv->lock, flags);

	/* wait for characters to drain from the device */
	/* (this is long enough for the entire 256 byte */
	/* pl2303 hardware buffer to drain with no flow */
	/* control for data rates of 1200 bps or more, */
	/* for lower rates we should really know how much */
	/* data is in the buffer to compute a delay */
	/* that is not unnecessarily long) */
	/* FIXME
	bps = tty_get_baud_rate(port->tty);
	if (bps > 1200)
		timeout = max((HZ*2560)/bps,HZ/10);
	else
	*/
		timeout = 2*HZ;
	schedule_timeout_interruptible(timeout);
	dbg("%s(): after schedule_timeout_interruptible()", __FUNCTION__);

	/* cancel scheduled setup */
	cancel_delayed_work(&priv->delayed_setup_work);
	cancel_delayed_work(&priv->delayed_write_work);
	flush_scheduled_work();

	/* shutdown our urbs */
	dbg("%s(): shutting down urbs", __FUNCTION__);
	usb_kill_urb(port->write_urb);
	usb_kill_urb(port->read_urb);
	usb_kill_urb(port->interrupt_in_urb);

	/*
	if (port->tty && (port->tty->termios->c_cflag) & HUPCL) {
		// drop DTR and RTS
		spin_lock_irqsave(&priv->lock, flags);
		priv->pending_setup.control &= ~CONTROL_MASK;
		spin_unlock_irqrestore(&priv->lock, flags);
	}
	*/
}

static int oti6858_tiocmset(struct usb_serial_port *port, struct file *file,
				unsigned int set, unsigned int clear)
{
	struct oti6858_private *priv = usb_get_serial_port_data(port);
	unsigned long flags;
	u8 control;

	dbg("%s(port = %d, set = 0x%08x, clear = 0x%08x)",
				__FUNCTION__, port->number, set, clear);

	if (!usb_get_intfdata(port->serial->interface))
		return -ENODEV;

	/* FIXME: check if this is correct (active high/low) */
	spin_lock_irqsave(&priv->lock, flags);
	control = priv->pending_setup.control;
	if ((set & TIOCM_RTS) != 0)
		control |= CONTROL_RTS_HIGH;
	if ((set & TIOCM_DTR) != 0)
		control |= CONTROL_DTR_HIGH;
	if ((clear & TIOCM_RTS) != 0)
		control &= ~CONTROL_RTS_HIGH;
	if ((clear & TIOCM_DTR) != 0)
		control &= ~CONTROL_DTR_HIGH;

	if (control != priv->pending_setup.control) {
		priv->pending_setup.control = control;
	}
	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static int oti6858_tiocmget(struct usb_serial_port *port, struct file *file)
{
	struct oti6858_private *priv = usb_get_serial_port_data(port);
	unsigned long flags;
	unsigned pin_state;
	unsigned result = 0;

	dbg("%s(port = %d)", __FUNCTION__, port->number);

	if (!usb_get_intfdata(port->serial->interface))
		return -ENODEV;

	spin_lock_irqsave(&priv->lock, flags);
	pin_state = priv->status.pin_state & PIN_MASK;
	spin_unlock_irqrestore(&priv->lock, flags);

	/* FIXME: check if this is correct (active high/low) */
	if ((pin_state & PIN_RTS) != 0)
		result |= TIOCM_RTS;
	if ((pin_state & PIN_CTS) != 0)
		result |= TIOCM_CTS;
	if ((pin_state & PIN_DSR) != 0)
		result |= TIOCM_DSR;
	if ((pin_state & PIN_DTR) != 0)
		result |= TIOCM_DTR;
	if ((pin_state & PIN_RI) != 0)
		result |= TIOCM_RI;
	if ((pin_state & PIN_DCD) != 0)
		result |= TIOCM_CD;

	dbg("%s() = 0x%08x", __FUNCTION__, result);

	return result;
}

static int wait_modem_info(struct usb_serial_port *port, unsigned int arg)
{
	struct oti6858_private *priv = usb_get_serial_port_data(port);
	unsigned long flags;
	unsigned int prev, status;
	unsigned int changed;

	spin_lock_irqsave(&priv->lock, flags);
	prev = priv->status.pin_state;
	spin_unlock_irqrestore(&priv->lock, flags);

	while (1) {
		wait_event_interruptible(priv->intr_wait, priv->status.pin_state != prev);
		if (signal_pending(current))
			return -ERESTARTSYS;

		spin_lock_irqsave(&priv->lock, flags);
		status = priv->status.pin_state & PIN_MASK;
		spin_unlock_irqrestore(&priv->lock, flags);

		changed = prev ^ status;
		/* FIXME: check if this is correct (active high/low) */
		if (	((arg & TIOCM_RNG) && (changed & PIN_RI)) ||
			((arg & TIOCM_DSR) && (changed & PIN_DSR)) ||
			((arg & TIOCM_CD)  && (changed & PIN_DCD)) ||
			((arg & TIOCM_CTS) && (changed & PIN_CTS))) {
				return 0;
		}
		prev = status;
	}

	/* NOTREACHED */
	return 0;
}

static int oti6858_ioctl(struct usb_serial_port *port, struct file *file,
			unsigned int cmd, unsigned long arg)
{
	void __user *user_arg = (void __user *) arg;
	unsigned int x;

	dbg("%s(port = %d, cmd = 0x%04x, arg = 0x%08lx)",
				__FUNCTION__, port->number, cmd, arg);

	switch (cmd) {
		case TCFLSH:
			/* FIXME */
			return 0;

		case TIOCMBIS:
			if (copy_from_user(&x, user_arg, sizeof(x)))
				return -EFAULT;
			return oti6858_tiocmset(port, NULL, x, 0);

		case TIOCMBIC:
			if (copy_from_user(&x, user_arg, sizeof(x)))
				return -EFAULT;
			return oti6858_tiocmset(port, NULL, 0, x);

		case TIOCGSERIAL:
			if (copy_to_user(user_arg, port->tty->termios,
						sizeof(struct ktermios))) {
				return -EFAULT;
			}
                        return 0;

		case TIOCSSERIAL:
			if (copy_from_user(port->tty->termios, user_arg,
						sizeof(struct ktermios))) {
				return -EFAULT;
			}
			oti6858_set_termios(port, NULL);
			return 0;

		case TIOCMIWAIT:
			dbg("%s(): TIOCMIWAIT", __FUNCTION__);
			return wait_modem_info(port, arg);

		default:
			dbg("%s(): 0x%04x not supported", __FUNCTION__, cmd);
			break;
	}

	return -ENOIOCTLCMD;
}

static void oti6858_break_ctl(struct usb_serial_port *port, int break_state)
{
	int state;

	dbg("%s(port = %d)", __FUNCTION__, port->number);

	state = (break_state == 0) ? 0 : 1;
	dbg("%s(): turning break %s", __FUNCTION__, state ? "on" : "off");

	/* FIXME */
/*
	result = usb_control_msg (serial->dev, usb_sndctrlpipe (serial->dev, 0),
				  BREAK_REQUEST, BREAK_REQUEST_TYPE, state,
				  0, NULL, 0, 100);
	if (result != 0)
		dbg("%s(): error sending break", __FUNCTION__);
 */
}

static void oti6858_shutdown(struct usb_serial *serial)
{
	struct oti6858_private *priv;
	int i;

	dbg("%s()", __FUNCTION__);

	for (i = 0; i < serial->num_ports; ++i) {
		priv = usb_get_serial_port_data(serial->port[i]);
		if (priv) {
			pl2303_buf_free(priv->buf);
			kfree(priv);
			usb_set_serial_port_data(serial->port[i], NULL);
		}
	}
}

static void oti6858_read_int_callback(struct urb *urb)
{
	struct usb_serial_port *port = (struct usb_serial_port *) urb->context;
	struct oti6858_private *priv = usb_get_serial_port_data(port);
	int transient = 0, can_recv = 0, resubmit = 1;
	int status = urb->status;

	dbg("%s(port = %d, status = %d)",
				__FUNCTION__, port->number, status);

	switch (status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dbg("%s(): urb shutting down with status: %d",
					__FUNCTION__, status);
		return;
	default:
		dbg("%s(): nonzero urb status received: %d",
					__FUNCTION__, status);
		break;
	}

	if (status == 0 && urb->actual_length == OTI6858_CTRL_PKT_SIZE) {
		struct oti6858_control_pkt *xs = urb->transfer_buffer;
		unsigned long flags;

		spin_lock_irqsave(&priv->lock, flags);

		if (!priv->transient) {
			if (!OTI6858_CTRL_EQUALS_PENDING(xs, priv)) {
				if (xs->rx_bytes_avail == 0) {
					priv->transient = 4;
					priv->setup_done = 0;
					resubmit = 0;
					dbg("%s(): scheduling setup_line()",
					    __FUNCTION__);
					schedule_delayed_work(&priv->delayed_setup_work, 0);
				}
			}
		} else {
			if (OTI6858_CTRL_EQUALS_PENDING(xs, priv)) {
				priv->transient = 0;
			} else if (!priv->setup_done) {
				resubmit = 0;
			} else if (--priv->transient == 0) {
				if (xs->rx_bytes_avail == 0) {
					priv->transient = 4;
					priv->setup_done = 0;
					resubmit = 0;
					dbg("%s(): scheduling setup_line()",
					    __FUNCTION__);
					schedule_delayed_work(&priv->delayed_setup_work, 0);
				}
			}
		}

		if (!priv->transient) {
			if (xs->pin_state != priv->status.pin_state)
				wake_up_interruptible(&priv->intr_wait);
			memcpy(&priv->status, xs, OTI6858_CTRL_PKT_SIZE);
		}

		if (!priv->transient && xs->rx_bytes_avail != 0) {
			can_recv = xs->rx_bytes_avail;
			priv->flags.read_urb_in_use = 1;
		}

		transient = priv->transient;
		spin_unlock_irqrestore(&priv->lock, flags);
	}

	if (can_recv) {
		int result;

		port->read_urb->dev = port->serial->dev;
		result = usb_submit_urb(port->read_urb, GFP_ATOMIC);
		if (result != 0) {
			priv->flags.read_urb_in_use = 0;
			dev_err(&port->dev, "%s(): usb_submit_urb() failed,"
					" error %d\n", __FUNCTION__, result);
		} else {
			resubmit = 0;
		}
	} else if (!transient) {
		unsigned long flags;

		spin_lock_irqsave(&priv->lock, flags);
		if (priv->flags.write_urb_in_use == 0
				&& pl2303_buf_data_avail(priv->buf) != 0) {
			schedule_delayed_work(&priv->delayed_write_work,0);
			resubmit = 0;
		}
		spin_unlock_irqrestore(&priv->lock, flags);
	}

	if (resubmit) {
		int result;

//		dbg("%s(): submitting interrupt urb", __FUNCTION__);
		urb->dev = port->serial->dev;
		result = usb_submit_urb(urb, GFP_ATOMIC);
		if (result != 0) {
			dev_err(&urb->dev->dev,
					"%s(): usb_submit_urb() failed with"
					" error %d\n", __FUNCTION__, result);
		}
	}
}

static void oti6858_read_bulk_callback(struct urb *urb)
{
	struct usb_serial_port *port = (struct usb_serial_port *) urb->context;
	struct oti6858_private *priv = usb_get_serial_port_data(port);
	struct tty_struct *tty;
	unsigned char *data = urb->transfer_buffer;
	unsigned long flags;
	int i, result;
	int status = urb->status;
	char tty_flag;

	dbg("%s(port = %d, status = %d)",
				__FUNCTION__, port->number, status);

	spin_lock_irqsave(&priv->lock, flags);
	priv->flags.read_urb_in_use = 0;
	spin_unlock_irqrestore(&priv->lock, flags);

	if (status != 0) {
		if (!port->open_count) {
			dbg("%s(): port is closed, exiting", __FUNCTION__);
			return;
		}
		/*
		if (status == -EPROTO) {
			// PL2303 mysteriously fails with -EPROTO reschedule the read
			dbg("%s - caught -EPROTO, resubmitting the urb", __FUNCTION__);
			result = usb_submit_urb(urb, GFP_ATOMIC);
			if (result)
				dev_err(&urb->dev->dev, "%s - failed resubmitting read urb, error %d\n", __FUNCTION__, result);
			return;
		}
		*/
		dbg("%s(): unable to handle the error, exiting", __FUNCTION__);
		return;
	}

	// get tty_flag from status
	tty_flag = TTY_NORMAL;

/* FIXME: probably, errors will be signalled using interrupt pipe! */
/*
	// break takes precedence over parity,
	// which takes precedence over framing errors
	if (status & UART_BREAK_ERROR )
		tty_flag = TTY_BREAK;
	else if (status & UART_PARITY_ERROR)
		tty_flag = TTY_PARITY;
	else if (status & UART_FRAME_ERROR)
		tty_flag = TTY_FRAME;
	dbg("%s - tty_flag = %d", __FUNCTION__, tty_flag);
*/

	tty = port->tty;
	if (tty != NULL && urb->actual_length > 0) {
		tty_buffer_request_room(tty, urb->actual_length);
		for (i = 0; i < urb->actual_length; ++i)
			tty_insert_flip_char(tty, data[i], tty_flag);
		tty_flip_buffer_push(tty);
	}

	// schedule the interrupt urb if we are still open */
	if (port->open_count != 0) {
		port->interrupt_in_urb->dev = port->serial->dev;
		result = usb_submit_urb(port->interrupt_in_urb, GFP_ATOMIC);
		if (result != 0) {
			dev_err(&port->dev, "%s(): usb_submit_urb() failed,"
					" error %d\n", __FUNCTION__, result);
		}
	}
}

static void oti6858_write_bulk_callback(struct urb *urb)
{
	struct usb_serial_port *port = (struct usb_serial_port *) urb->context;
	struct oti6858_private *priv = usb_get_serial_port_data(port);
	int status = urb->status;
	int result;

	dbg("%s(port = %d, status = %d)",
				__FUNCTION__, port->number, status);

	switch (status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dbg("%s(): urb shutting down with status: %d",
					__FUNCTION__, status);
		priv->flags.write_urb_in_use = 0;
		return;
	default:
		/* error in the urb, so we have to resubmit it */
		dbg("%s(): nonzero write bulk status received: %d",
					__FUNCTION__, status);
		dbg("%s(): overflow in write", __FUNCTION__);

		port->write_urb->transfer_buffer_length = 1;
		port->write_urb->dev = port->serial->dev;
		result = usb_submit_urb(port->write_urb, GFP_ATOMIC);
		if (result) {
			dev_err(&port->dev, "%s(): usb_submit_urb() failed,"
					" error %d\n", __FUNCTION__, result);
		} else {
			return;
		}
	}

	priv->flags.write_urb_in_use = 0;

	// schedule the interrupt urb if we are still open */
	port->interrupt_in_urb->dev = port->serial->dev;
	dbg("%s(): submitting interrupt urb", __FUNCTION__);
	result = usb_submit_urb(port->interrupt_in_urb, GFP_ATOMIC);
	if (result != 0) {
		dev_err(&port->dev, "%s(): failed submitting int urb,"
					" error %d\n", __FUNCTION__, result);
	}
}


/*
 * pl2303_buf_alloc
 *
 * Allocate a circular buffer and all associated memory.
 */
static struct pl2303_buf *pl2303_buf_alloc(unsigned int size)
{
	struct pl2303_buf *pb;

	if (size == 0)
		return NULL;

	pb = (struct pl2303_buf *)kmalloc(sizeof(struct pl2303_buf), GFP_KERNEL);
	if (pb == NULL)
		return NULL;

	pb->buf_buf = kmalloc(size, GFP_KERNEL);
	if (pb->buf_buf == NULL) {
		kfree(pb);
		return NULL;
	}

	pb->buf_size = size;
	pb->buf_get = pb->buf_put = pb->buf_buf;

	return pb;
}

/*
 * pl2303_buf_free
 *
 * Free the buffer and all associated memory.
 */
static void pl2303_buf_free(struct pl2303_buf *pb)
{
	if (pb) {
		kfree(pb->buf_buf);
		kfree(pb);
	}
}

/*
 * pl2303_buf_clear
 *
 * Clear out all data in the circular buffer.
 */
static void pl2303_buf_clear(struct pl2303_buf *pb)
{
	if (pb != NULL) {
		/* equivalent to a get of all data available */
		pb->buf_get = pb->buf_put;
	}
}

/*
 * pl2303_buf_data_avail
 *
 * Return the number of bytes of data available in the circular
 * buffer.
 */
static unsigned int pl2303_buf_data_avail(struct pl2303_buf *pb)
{
	if (pb == NULL)
		return 0;
	return ((pb->buf_size + pb->buf_put - pb->buf_get) % pb->buf_size);
}

/*
 * pl2303_buf_space_avail
 *
 * Return the number of bytes of space available in the circular
 * buffer.
 */
static unsigned int pl2303_buf_space_avail(struct pl2303_buf *pb)
{
	if (pb == NULL)
		return 0;
	return ((pb->buf_size + pb->buf_get - pb->buf_put - 1) % pb->buf_size);
}

/*
 * pl2303_buf_put
 *
 * Copy data data from a user buffer and put it into the circular buffer.
 * Restrict to the amount of space available.
 *
 * Return the number of bytes copied.
 */
static unsigned int pl2303_buf_put(struct pl2303_buf *pb, const char *buf,
					unsigned int count)
{
	unsigned int len;

	if (pb == NULL)
		return 0;

	len  = pl2303_buf_space_avail(pb);
	if (count > len)
		count = len;

	if (count == 0)
		return 0;

	len = pb->buf_buf + pb->buf_size - pb->buf_put;
	if (count > len) {
		memcpy(pb->buf_put, buf, len);
		memcpy(pb->buf_buf, buf+len, count - len);
		pb->buf_put = pb->buf_buf + count - len;
	} else {
		memcpy(pb->buf_put, buf, count);
		if (count < len)
			pb->buf_put += count;
		else /* count == len */
			pb->buf_put = pb->buf_buf;
	}

	return count;
}

/*
 * pl2303_buf_get
 *
 * Get data from the circular buffer and copy to the given buffer.
 * Restrict to the amount of data available.
 *
 * Return the number of bytes copied.
 */
static unsigned int pl2303_buf_get(struct pl2303_buf *pb, char *buf,
					unsigned int count)
{
	unsigned int len;

	if (pb == NULL)
		return 0;

	len = pl2303_buf_data_avail(pb);
	if (count > len)
		count = len;

	if (count == 0)
		return 0;

	len = pb->buf_buf + pb->buf_size - pb->buf_get;
	if (count > len) {
		memcpy(buf, pb->buf_get, len);
		memcpy(buf+len, pb->buf_buf, count - len);
		pb->buf_get = pb->buf_buf + count - len;
	} else {
		memcpy(buf, pb->buf_get, count);
		if (count < len)
			pb->buf_get += count;
		else /* count == len */
			pb->buf_get = pb->buf_buf;
	}

	return count;
}

/* module description and (de)initialization */

static int __init oti6858_init(void)
{
	int retval;

	if ((retval = usb_serial_register(&oti6858_device)) == 0) {
		if ((retval = usb_register(&oti6858_driver)) != 0)
			usb_serial_deregister(&oti6858_device);
		else
			return 0;
	}

	return retval;
}

static void __exit oti6858_exit(void)
{
	usb_deregister(&oti6858_driver);
	usb_serial_deregister(&oti6858_device);
}

module_init(oti6858_init);
module_exit(oti6858_exit);

MODULE_DESCRIPTION(OTI6858_DESCRIPTION);
MODULE_AUTHOR(OTI6858_AUTHOR);
MODULE_VERSION(OTI6858_VERSION);
MODULE_LICENSE("GPL");

module_param(debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "enable debug output");

