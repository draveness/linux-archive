/*
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
 *
 * Clean ups from Moschip version and a few ioctl implementations by:
 *	Paul B Schroeder <pschroeder "at" uplogix "dot" com>
 *
 * Originally based on drivers/usb/serial/io_edgeport.c which is:
 *      Copyright (C) 2000 Inside Out Networks, All rights reserved.
 *      Copyright (C) 2001-2002 Greg Kroah-Hartman <greg@kroah.com>
 *
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/module.h>
#include <linux/serial.h>
#include <linux/usb.h>
#include <linux/usb/serial.h>
#include <asm/uaccess.h>

/*
 * Version Information
 */
#define DRIVER_VERSION "1.3.1"
#define DRIVER_DESC "Moschip 7840/7820 USB Serial Driver"

/*
 * 16C50 UART register defines
 */

#define LCR_BITS_5             0x00	/* 5 bits/char */
#define LCR_BITS_6             0x01	/* 6 bits/char */
#define LCR_BITS_7             0x02	/* 7 bits/char */
#define LCR_BITS_8             0x03	/* 8 bits/char */
#define LCR_BITS_MASK          0x03	/* Mask for bits/char field */

#define LCR_STOP_1             0x00	/* 1 stop bit */
#define LCR_STOP_1_5           0x04	/* 1.5 stop bits (if 5   bits/char) */
#define LCR_STOP_2             0x04	/* 2 stop bits   (if 6-8 bits/char) */
#define LCR_STOP_MASK          0x04	/* Mask for stop bits field */

#define LCR_PAR_NONE           0x00	/* No parity */
#define LCR_PAR_ODD            0x08	/* Odd parity */
#define LCR_PAR_EVEN           0x18	/* Even parity */
#define LCR_PAR_MARK           0x28	/* Force parity bit to 1 */
#define LCR_PAR_SPACE          0x38	/* Force parity bit to 0 */
#define LCR_PAR_MASK           0x38	/* Mask for parity field */

#define LCR_SET_BREAK          0x40	/* Set Break condition */
#define LCR_DL_ENABLE          0x80	/* Enable access to divisor latch */

#define MCR_DTR                0x01	/* Assert DTR */
#define MCR_RTS                0x02	/* Assert RTS */
#define MCR_OUT1               0x04	/* Loopback only: Sets state of RI */
#define MCR_MASTER_IE          0x08	/* Enable interrupt outputs */
#define MCR_LOOPBACK           0x10	/* Set internal (digital) loopback mode */
#define MCR_XON_ANY            0x20	/* Enable any char to exit XOFF mode */

#define MOS7840_MSR_CTS        0x10	/* Current state of CTS */
#define MOS7840_MSR_DSR        0x20	/* Current state of DSR */
#define MOS7840_MSR_RI         0x40	/* Current state of RI */
#define MOS7840_MSR_CD         0x80	/* Current state of CD */

/*
 * Defines used for sending commands to port
 */

#define WAIT_FOR_EVER   (HZ * 0 )	/* timeout urb is wait for ever */
#define MOS_WDR_TIMEOUT (HZ * 5 )	/* default urb timeout */

#define MOS_PORT1       0x0200
#define MOS_PORT2       0x0300
#define MOS_VENREG      0x0000
#define MOS_MAX_PORT	0x02
#define MOS_WRITE       0x0E
#define MOS_READ        0x0D

/* Requests */
#define MCS_RD_RTYPE    0xC0
#define MCS_WR_RTYPE    0x40
#define MCS_RDREQ       0x0D
#define MCS_WRREQ       0x0E
#define MCS_CTRL_TIMEOUT        500
#define VENDOR_READ_LENGTH      (0x01)

#define MAX_NAME_LEN    64

#define ZLP_REG1  0x3A		//Zero_Flag_Reg1    58
#define ZLP_REG5  0x3E		//Zero_Flag_Reg5    62

/* For higher baud Rates use TIOCEXBAUD */
#define TIOCEXBAUD     0x5462

/* vendor id and device id defines */

#define USB_VENDOR_ID_MOSCHIP           0x9710
#define MOSCHIP_DEVICE_ID_7840          0x7840
#define MOSCHIP_DEVICE_ID_7820          0x7820

/* Interrupt Rotinue Defines    */

#define SERIAL_IIR_RLS      0x06
#define SERIAL_IIR_MS       0x00

/*
 *  Emulation of the bit mask on the LINE STATUS REGISTER.
 */
#define SERIAL_LSR_DR       0x0001
#define SERIAL_LSR_OE       0x0002
#define SERIAL_LSR_PE       0x0004
#define SERIAL_LSR_FE       0x0008
#define SERIAL_LSR_BI       0x0010

#define MOS_MSR_DELTA_CTS   0x10
#define MOS_MSR_DELTA_DSR   0x20
#define MOS_MSR_DELTA_RI    0x40
#define MOS_MSR_DELTA_CD    0x80

// Serial Port register Address
#define INTERRUPT_ENABLE_REGISTER  ((__u16)(0x01))
#define FIFO_CONTROL_REGISTER      ((__u16)(0x02))
#define LINE_CONTROL_REGISTER      ((__u16)(0x03))
#define MODEM_CONTROL_REGISTER     ((__u16)(0x04))
#define LINE_STATUS_REGISTER       ((__u16)(0x05))
#define MODEM_STATUS_REGISTER      ((__u16)(0x06))
#define SCRATCH_PAD_REGISTER       ((__u16)(0x07))
#define DIVISOR_LATCH_LSB          ((__u16)(0x00))
#define DIVISOR_LATCH_MSB          ((__u16)(0x01))

#define CLK_MULTI_REGISTER         ((__u16)(0x02))
#define CLK_START_VALUE_REGISTER   ((__u16)(0x03))

#define SERIAL_LCR_DLAB            ((__u16)(0x0080))

/*
 * URB POOL related defines
 */
#define NUM_URBS                        16	/* URB Count */
#define URB_TRANSFER_BUFFER_SIZE        32	/* URB Size  */


static struct usb_device_id moschip_port_id_table[] = {
	{USB_DEVICE(USB_VENDOR_ID_MOSCHIP, MOSCHIP_DEVICE_ID_7840)},
	{USB_DEVICE(USB_VENDOR_ID_MOSCHIP, MOSCHIP_DEVICE_ID_7820)},
	{}			/* terminating entry */
};

static __devinitdata struct usb_device_id moschip_id_table_combined[] = {
	{USB_DEVICE(USB_VENDOR_ID_MOSCHIP, MOSCHIP_DEVICE_ID_7840)},
	{USB_DEVICE(USB_VENDOR_ID_MOSCHIP, MOSCHIP_DEVICE_ID_7820)},
	{}			/* terminating entry */
};

MODULE_DEVICE_TABLE(usb, moschip_id_table_combined);

/* This structure holds all of the local port information */

struct moschip_port {
	int port_num;		/*Actual port number in the device(1,2,etc) */
	struct urb *write_urb;	/* write URB for this port */
	struct urb *read_urb;	/* read URB for this port */
	struct urb *int_urb;
	__u8 shadowLCR;		/* last LCR value received */
	__u8 shadowMCR;		/* last MCR value received */
	char open;
	char open_ports;
	char zombie;
	wait_queue_head_t wait_chase;	/* for handling sleeping while waiting for chase to finish */
	wait_queue_head_t delta_msr_wait;	/* for handling sleeping while waiting for msr change to happen */
	int delta_msr_cond;
	struct async_icount icount;
	struct usb_serial_port *port;	/* loop back to the owner of this object */

	/*Offsets */
	__u8 SpRegOffset;
	__u8 ControlRegOffset;
	__u8 DcrRegOffset;
	//for processing control URBS in interrupt context
	struct urb *control_urb;
	struct usb_ctrlrequest *dr;
	char *ctrl_buf;
	int MsrLsr;

	spinlock_t pool_lock;
	struct urb *write_urb_pool[NUM_URBS];
	char busy[NUM_URBS];
};


static int debug;

/*
 * mos7840_set_reg_sync
 * 	To set the Control register by calling usb_fill_control_urb function
 *	by passing usb_sndctrlpipe function as parameter.
 */

static int mos7840_set_reg_sync(struct usb_serial_port *port, __u16 reg,
				__u16 val)
{
	struct usb_device *dev = port->serial->dev;
	val = val & 0x00ff;
	dbg("mos7840_set_reg_sync offset is %x, value %x\n", reg, val);

	return usb_control_msg(dev, usb_sndctrlpipe(dev, 0), MCS_WRREQ,
			       MCS_WR_RTYPE, val, reg, NULL, 0,
			       MOS_WDR_TIMEOUT);
}

/*
 * mos7840_get_reg_sync
 * 	To set the Uart register by calling usb_fill_control_urb function by
 *	passing usb_rcvctrlpipe function as parameter.
 */

static int mos7840_get_reg_sync(struct usb_serial_port *port, __u16 reg,
				__u16 * val)
{
	struct usb_device *dev = port->serial->dev;
	int ret = 0;

	ret = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), MCS_RDREQ,
			      MCS_RD_RTYPE, 0, reg, val, VENDOR_READ_LENGTH,
			      MOS_WDR_TIMEOUT);
	dbg("mos7840_get_reg_sync offset is %x, return val %x\n", reg, *val);
	*val = (*val) & 0x00ff;
	return ret;
}

/*
 * mos7840_set_uart_reg
 *	To set the Uart register by calling usb_fill_control_urb function by
 *	passing usb_sndctrlpipe function as parameter.
 */

static int mos7840_set_uart_reg(struct usb_serial_port *port, __u16 reg,
				__u16 val)
{

	struct usb_device *dev = port->serial->dev;
	val = val & 0x00ff;
	// For the UART control registers, the application number need to be Or'ed
	if (port->serial->num_ports == 4) {
		val |=
		    (((__u16) port->number - (__u16) (port->serial->minor)) +
		     1) << 8;
		dbg("mos7840_set_uart_reg application number is %x\n", val);
	} else {
		if (((__u16) port->number - (__u16) (port->serial->minor)) == 0) {
			val |=
			    (((__u16) port->number -
			      (__u16) (port->serial->minor)) + 1) << 8;
			dbg("mos7840_set_uart_reg application number is %x\n",
			    val);
		} else {
			val |=
			    (((__u16) port->number -
			      (__u16) (port->serial->minor)) + 2) << 8;
			dbg("mos7840_set_uart_reg application number is %x\n",
			    val);
		}
	}
	return usb_control_msg(dev, usb_sndctrlpipe(dev, 0), MCS_WRREQ,
			       MCS_WR_RTYPE, val, reg, NULL, 0,
			       MOS_WDR_TIMEOUT);

}

/*
 * mos7840_get_uart_reg
 *	To set the Control register by calling usb_fill_control_urb function
 *	by passing usb_rcvctrlpipe function as parameter.
 */
static int mos7840_get_uart_reg(struct usb_serial_port *port, __u16 reg,
				__u16 * val)
{
	struct usb_device *dev = port->serial->dev;
	int ret = 0;
	__u16 Wval;

	//dbg("application number is %4x \n",(((__u16)port->number - (__u16)(port->serial->minor))+1)<<8);
	/*Wval  is same as application number */
	if (port->serial->num_ports == 4) {
		Wval =
		    (((__u16) port->number - (__u16) (port->serial->minor)) +
		     1) << 8;
		dbg("mos7840_get_uart_reg application number is %x\n", Wval);
	} else {
		if (((__u16) port->number - (__u16) (port->serial->minor)) == 0) {
			Wval =
			    (((__u16) port->number -
			      (__u16) (port->serial->minor)) + 1) << 8;
			dbg("mos7840_get_uart_reg application number is %x\n",
			    Wval);
		} else {
			Wval =
			    (((__u16) port->number -
			      (__u16) (port->serial->minor)) + 2) << 8;
			dbg("mos7840_get_uart_reg application number is %x\n",
			    Wval);
		}
	}
	ret = usb_control_msg(dev, usb_rcvctrlpipe(dev, 0), MCS_RDREQ,
			      MCS_RD_RTYPE, Wval, reg, val, VENDOR_READ_LENGTH,
			      MOS_WDR_TIMEOUT);
	*val = (*val) & 0x00ff;
	return ret;
}

static void mos7840_dump_serial_port(struct moschip_port *mos7840_port)
{

	dbg("***************************************\n");
	dbg("SpRegOffset is %2x\n", mos7840_port->SpRegOffset);
	dbg("ControlRegOffset is %2x \n", mos7840_port->ControlRegOffset);
	dbg("DCRRegOffset is %2x \n", mos7840_port->DcrRegOffset);
	dbg("***************************************\n");

}

/************************************************************************/
/************************************************************************/
/*             I N T E R F A C E   F U N C T I O N S			*/
/*             I N T E R F A C E   F U N C T I O N S			*/
/************************************************************************/
/************************************************************************/

static inline void mos7840_set_port_private(struct usb_serial_port *port,
					    struct moschip_port *data)
{
	usb_set_serial_port_data(port, (void *)data);
}

static inline struct moschip_port *mos7840_get_port_private(struct
							    usb_serial_port
							    *port)
{
	return (struct moschip_port *)usb_get_serial_port_data(port);
}

static void mos7840_handle_new_msr(struct moschip_port *port, __u8 new_msr)
{
	struct moschip_port *mos7840_port;
	struct async_icount *icount;
	mos7840_port = port;
	icount = &mos7840_port->icount;
	if (new_msr &
	    (MOS_MSR_DELTA_CTS | MOS_MSR_DELTA_DSR | MOS_MSR_DELTA_RI |
	     MOS_MSR_DELTA_CD)) {
		icount = &mos7840_port->icount;

		/* update input line counters */
		if (new_msr & MOS_MSR_DELTA_CTS) {
			icount->cts++;
			smp_wmb();
		}
		if (new_msr & MOS_MSR_DELTA_DSR) {
			icount->dsr++;
			smp_wmb();
		}
		if (new_msr & MOS_MSR_DELTA_CD) {
			icount->dcd++;
			smp_wmb();
		}
		if (new_msr & MOS_MSR_DELTA_RI) {
			icount->rng++;
			smp_wmb();
		}
	}
}

static void mos7840_handle_new_lsr(struct moschip_port *port, __u8 new_lsr)
{
	struct async_icount *icount;

	dbg("%s - %02x", __FUNCTION__, new_lsr);

	if (new_lsr & SERIAL_LSR_BI) {
		//
		// Parity and Framing errors only count if they
		// occur exclusive of a break being
		// received.
		//
		new_lsr &= (__u8) (SERIAL_LSR_OE | SERIAL_LSR_BI);
	}

	/* update input line counters */
	icount = &port->icount;
	if (new_lsr & SERIAL_LSR_BI) {
		icount->brk++;
		smp_wmb();
	}
	if (new_lsr & SERIAL_LSR_OE) {
		icount->overrun++;
		smp_wmb();
	}
	if (new_lsr & SERIAL_LSR_PE) {
		icount->parity++;
		smp_wmb();
	}
	if (new_lsr & SERIAL_LSR_FE) {
		icount->frame++;
		smp_wmb();
	}
}

/************************************************************************/
/************************************************************************/
/*            U S B  C A L L B A C K   F U N C T I O N S                */
/*            U S B  C A L L B A C K   F U N C T I O N S                */
/************************************************************************/
/************************************************************************/

static void mos7840_control_callback(struct urb *urb)
{
	unsigned char *data;
	struct moschip_port *mos7840_port;
	__u8 regval = 0x0;
	int result = 0;
	int status = urb->status;

	mos7840_port = (struct moschip_port *)urb->context;

	switch (status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dbg("%s - urb shutting down with status: %d", __FUNCTION__,
		    status);
		return;
	default:
		dbg("%s - nonzero urb status received: %d", __FUNCTION__,
		    status);
		goto exit;
	}

	dbg("%s urb buffer size is %d\n", __FUNCTION__, urb->actual_length);
	dbg("%s mos7840_port->MsrLsr is %d port %d\n", __FUNCTION__,
	    mos7840_port->MsrLsr, mos7840_port->port_num);
	data = urb->transfer_buffer;
	regval = (__u8) data[0];
	dbg("%s data is %x\n", __FUNCTION__, regval);
	if (mos7840_port->MsrLsr == 0)
		mos7840_handle_new_msr(mos7840_port, regval);
	else if (mos7840_port->MsrLsr == 1)
		mos7840_handle_new_lsr(mos7840_port, regval);

exit:
	spin_lock(&mos7840_port->pool_lock);
	if (!mos7840_port->zombie)
		result = usb_submit_urb(mos7840_port->int_urb, GFP_ATOMIC);
	spin_unlock(&mos7840_port->pool_lock);
	if (result) {
		dev_err(&urb->dev->dev,
			"%s - Error %d submitting interrupt urb\n",
			__FUNCTION__, result);
	}
}

static int mos7840_get_reg(struct moschip_port *mcs, __u16 Wval, __u16 reg,
			   __u16 * val)
{
	struct usb_device *dev = mcs->port->serial->dev;
	struct usb_ctrlrequest *dr = mcs->dr;
	unsigned char *buffer = mcs->ctrl_buf;
	int ret;

	dr->bRequestType = MCS_RD_RTYPE;
	dr->bRequest = MCS_RDREQ;
	dr->wValue = cpu_to_le16(Wval);	//0;
	dr->wIndex = cpu_to_le16(reg);
	dr->wLength = cpu_to_le16(2);

	usb_fill_control_urb(mcs->control_urb, dev, usb_rcvctrlpipe(dev, 0),
			     (unsigned char *)dr, buffer, 2,
			     mos7840_control_callback, mcs);
	mcs->control_urb->transfer_buffer_length = 2;
	ret = usb_submit_urb(mcs->control_urb, GFP_ATOMIC);
	return ret;
}

/*****************************************************************************
 * mos7840_interrupt_callback
 *	this is the callback function for when we have received data on the
 *	interrupt endpoint.
 *****************************************************************************/

static void mos7840_interrupt_callback(struct urb *urb)
{
	int result;
	int length;
	struct moschip_port *mos7840_port;
	struct usb_serial *serial;
	__u16 Data;
	unsigned char *data;
	__u8 sp[5], st;
	int i, rv = 0;
	__u16 wval, wreg = 0;
	int status = urb->status;

	dbg("%s", " : Entering\n");

	switch (status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dbg("%s - urb shutting down with status: %d", __FUNCTION__,
		    status);
		return;
	default:
		dbg("%s - nonzero urb status received: %d", __FUNCTION__,
		    status);
		goto exit;
	}

	length = urb->actual_length;
	data = urb->transfer_buffer;

	serial = (struct usb_serial *)urb->context;

	/* Moschip get 5 bytes
	 * Byte 1 IIR Port 1 (port.number is 0)
	 * Byte 2 IIR Port 2 (port.number is 1)
	 * Byte 3 IIR Port 3 (port.number is 2)
	 * Byte 4 IIR Port 4 (port.number is 3)
	 * Byte 5 FIFO status for both */

	if (length && length > 5) {
		dbg("%s \n", "Wrong data !!!");
		return;
	}

	sp[0] = (__u8) data[0];
	sp[1] = (__u8) data[1];
	sp[2] = (__u8) data[2];
	sp[3] = (__u8) data[3];
	st = (__u8) data[4];

	for (i = 0; i < serial->num_ports; i++) {
		mos7840_port = mos7840_get_port_private(serial->port[i]);
		wval =
		    (((__u16) serial->port[i]->number -
		      (__u16) (serial->minor)) + 1) << 8;
		if (mos7840_port->open) {
			if (sp[i] & 0x01) {
				dbg("SP%d No Interrupt !!!\n", i);
			} else {
				switch (sp[i] & 0x0f) {
				case SERIAL_IIR_RLS:
					dbg("Serial Port %d: Receiver status error or ", i);
					dbg("address bit detected in 9-bit mode\n");
					mos7840_port->MsrLsr = 1;
					wreg = LINE_STATUS_REGISTER;
					break;
				case SERIAL_IIR_MS:
					dbg("Serial Port %d: Modem status change\n", i);
					mos7840_port->MsrLsr = 0;
					wreg = MODEM_STATUS_REGISTER;
					break;
				}
				spin_lock(&mos7840_port->pool_lock);
				if (!mos7840_port->zombie) {
					rv = mos7840_get_reg(mos7840_port, wval, wreg, &Data);
				} else {
					spin_unlock(&mos7840_port->pool_lock);
					return;
				}
				spin_unlock(&mos7840_port->pool_lock);
			}
		}
	}
	if (!(rv < 0)) /* the completion handler for the control urb will resubmit */
		return;
exit:
	result = usb_submit_urb(urb, GFP_ATOMIC);
	if (result) {
		dev_err(&urb->dev->dev,
			"%s - Error %d submitting interrupt urb\n",
			__FUNCTION__, result);
	}
}

static int mos7840_port_paranoia_check(struct usb_serial_port *port,
				       const char *function)
{
	if (!port) {
		dbg("%s - port == NULL", function);
		return -1;
	}
	if (!port->serial) {
		dbg("%s - port->serial == NULL", function);
		return -1;
	}

	return 0;
}

/* Inline functions to check the sanity of a pointer that is passed to us */
static int mos7840_serial_paranoia_check(struct usb_serial *serial,
					 const char *function)
{
	if (!serial) {
		dbg("%s - serial == NULL", function);
		return -1;
	}
	if (!serial->type) {
		dbg("%s - serial->type == NULL!", function);
		return -1;
	}

	return 0;
}

static struct usb_serial *mos7840_get_usb_serial(struct usb_serial_port *port,
						 const char *function)
{
	/* if no port was specified, or it fails a paranoia check */
	if (!port ||
	    mos7840_port_paranoia_check(port, function) ||
	    mos7840_serial_paranoia_check(port->serial, function)) {
		/* then say that we don't have a valid usb_serial thing, which will
		 * end up genrating -ENODEV return values */
		return NULL;
	}

	return port->serial;
}

/*****************************************************************************
 * mos7840_bulk_in_callback
 *	this is the callback function for when we have received data on the
 *	bulk in endpoint.
 *****************************************************************************/

static void mos7840_bulk_in_callback(struct urb *urb)
{
	int retval;
	unsigned char *data;
	struct usb_serial *serial;
	struct usb_serial_port *port;
	struct moschip_port *mos7840_port;
	struct tty_struct *tty;
	int status = urb->status;

	if (status) {
		dbg("nonzero read bulk status received: %d", status);
		return;
	}

	mos7840_port = (struct moschip_port *)urb->context;
	if (!mos7840_port) {
		dbg("%s", "NULL mos7840_port pointer \n");
		return;
	}

	port = (struct usb_serial_port *)mos7840_port->port;
	if (mos7840_port_paranoia_check(port, __FUNCTION__)) {
		dbg("%s", "Port Paranoia failed \n");
		return;
	}

	serial = mos7840_get_usb_serial(port, __FUNCTION__);
	if (!serial) {
		dbg("%s\n", "Bad serial pointer ");
		return;
	}

	dbg("%s\n", "Entering... \n");

	data = urb->transfer_buffer;

	dbg("%s", "Entering ........... \n");

	if (urb->actual_length) {
		tty = mos7840_port->port->tty;
		if (tty) {
			tty_buffer_request_room(tty, urb->actual_length);
			tty_insert_flip_string(tty, data, urb->actual_length);
			dbg(" %s \n", data);
			tty_flip_buffer_push(tty);
		}
		mos7840_port->icount.rx += urb->actual_length;
		smp_wmb();
		dbg("mos7840_port->icount.rx is %d:\n",
		    mos7840_port->icount.rx);
	}

	if (!mos7840_port->read_urb) {
		dbg("%s", "URB KILLED !!!\n");
		return;
	}


	mos7840_port->read_urb->dev = serial->dev;

	retval = usb_submit_urb(mos7840_port->read_urb, GFP_ATOMIC);

	if (retval) {
		dbg(" usb_submit_urb(read bulk) failed, retval = %d",
		 retval);
	}
}

/*****************************************************************************
 * mos7840_bulk_out_data_callback
 *	this is the callback function for when we have finished sending serial data
 *	on the bulk out endpoint.
 *****************************************************************************/

static void mos7840_bulk_out_data_callback(struct urb *urb)
{
	struct moschip_port *mos7840_port;
	struct tty_struct *tty;
	int status = urb->status;
	int i;

	mos7840_port = (struct moschip_port *)urb->context;
	spin_lock(&mos7840_port->pool_lock);
	for (i = 0; i < NUM_URBS; i++) {
		if (urb == mos7840_port->write_urb_pool[i]) {
			mos7840_port->busy[i] = 0;
			break;
		}
	}
	spin_unlock(&mos7840_port->pool_lock);

	if (status) {
		dbg("nonzero write bulk status received:%d\n", status);
		return;
	}

	if (mos7840_port_paranoia_check(mos7840_port->port, __FUNCTION__)) {
		dbg("%s", "Port Paranoia failed \n");
		return;
	}

	dbg("%s \n", "Entering .........");

	tty = mos7840_port->port->tty;

	if (tty && mos7840_port->open)
		tty_wakeup(tty);

}

/************************************************************************/
/*       D R I V E R  T T Y  I N T E R F A C E  F U N C T I O N S       */
/************************************************************************/
#ifdef MCSSerialProbe
static int mos7840_serial_probe(struct usb_serial *serial,
				const struct usb_device_id *id)
{

	/*need to implement the mode_reg reading and updating\
	   structures usb_serial_ device_type\
	   (i.e num_ports, num_bulkin,bulkout etc) */
	/* Also we can update the changes  attach */
	return 1;
}
#endif

/*****************************************************************************
 * mos7840_open
 *	this function is called by the tty driver when a port is opened
 *	If successful, we return 0
 *	Otherwise we return a negative error number.
 *****************************************************************************/

static int mos7840_open(struct usb_serial_port *port, struct file *filp)
{
	int response;
	int j;
	struct usb_serial *serial;
	struct urb *urb;
	__u16 Data;
	int status;
	struct moschip_port *mos7840_port;
	struct moschip_port *port0;

	if (mos7840_port_paranoia_check(port, __FUNCTION__)) {
		dbg("%s", "Port Paranoia failed \n");
		return -ENODEV;
	}

	serial = port->serial;

	if (mos7840_serial_paranoia_check(serial, __FUNCTION__)) {
		dbg("%s", "Serial Paranoia failed \n");
		return -ENODEV;
	}

	mos7840_port = mos7840_get_port_private(port);
	port0 = mos7840_get_port_private(serial->port[0]);

	if (mos7840_port == NULL || port0 == NULL)
		return -ENODEV;

	usb_clear_halt(serial->dev, port->write_urb->pipe);
	usb_clear_halt(serial->dev, port->read_urb->pipe);
	port0->open_ports++;

	/* Initialising the write urb pool */
	for (j = 0; j < NUM_URBS; ++j) {
		urb = usb_alloc_urb(0, GFP_KERNEL);
		mos7840_port->write_urb_pool[j] = urb;

		if (urb == NULL) {
			err("No more urbs???");
			continue;
		}

		urb->transfer_buffer = kmalloc(URB_TRANSFER_BUFFER_SIZE, GFP_KERNEL);
		if (!urb->transfer_buffer) {
			usb_free_urb(urb);
			mos7840_port->write_urb_pool[j] = NULL;
			err("%s-out of memory for urb buffers.", __FUNCTION__);
			continue;
		}
	}

/*****************************************************************************
 * Initialize MCS7840 -- Write Init values to corresponding Registers
 *
 * Register Index
 * 1 : IER
 * 2 : FCR
 * 3 : LCR
 * 4 : MCR
 *
 * 0x08 : SP1/2 Control Reg
 *****************************************************************************/

//NEED to check the following Block

	status = 0;
	Data = 0x0;
	status = mos7840_get_reg_sync(port, mos7840_port->SpRegOffset, &Data);
	if (status < 0) {
		dbg("Reading Spreg failed\n");
		return -1;
	}
	Data |= 0x80;
	status = mos7840_set_reg_sync(port, mos7840_port->SpRegOffset, Data);
	if (status < 0) {
		dbg("writing Spreg failed\n");
		return -1;
	}

	Data &= ~0x80;
	status = mos7840_set_reg_sync(port, mos7840_port->SpRegOffset, Data);
	if (status < 0) {
		dbg("writing Spreg failed\n");
		return -1;
	}
//End of block to be checked

	status = 0;
	Data = 0x0;
	status =
	    mos7840_get_reg_sync(port, mos7840_port->ControlRegOffset, &Data);
	if (status < 0) {
		dbg("Reading Controlreg failed\n");
		return -1;
	}
	Data |= 0x08;		//Driver done bit
	Data |= 0x20;		//rx_disable
	status = mos7840_set_reg_sync(port, mos7840_port->ControlRegOffset, Data);
	if (status < 0) {
		dbg("writing Controlreg failed\n");
		return -1;
	}
	//do register settings here
	// Set all regs to the device default values.
	////////////////////////////////////
	// First Disable all interrupts.
	////////////////////////////////////

	Data = 0x00;
	status = mos7840_set_uart_reg(port, INTERRUPT_ENABLE_REGISTER, Data);
	if (status < 0) {
		dbg("disableing interrupts failed\n");
		return -1;
	}
	// Set FIFO_CONTROL_REGISTER to the default value
	Data = 0x00;
	status = mos7840_set_uart_reg(port, FIFO_CONTROL_REGISTER, Data);
	if (status < 0) {
		dbg("Writing FIFO_CONTROL_REGISTER  failed\n");
		return -1;
	}

	Data = 0xcf;
	status = mos7840_set_uart_reg(port, FIFO_CONTROL_REGISTER, Data);
	if (status < 0) {
		dbg("Writing FIFO_CONTROL_REGISTER  failed\n");
		return -1;
	}

	Data = 0x03;
	status = mos7840_set_uart_reg(port, LINE_CONTROL_REGISTER, Data);
	mos7840_port->shadowLCR = Data;

	Data = 0x0b;
	status = mos7840_set_uart_reg(port, MODEM_CONTROL_REGISTER, Data);
	mos7840_port->shadowMCR = Data;

	Data = 0x00;
	status = mos7840_get_uart_reg(port, LINE_CONTROL_REGISTER, &Data);
	mos7840_port->shadowLCR = Data;

	Data |= SERIAL_LCR_DLAB;	//data latch enable in LCR 0x80
	status = mos7840_set_uart_reg(port, LINE_CONTROL_REGISTER, Data);

	Data = 0x0c;
	status = 0;
	status = mos7840_set_uart_reg(port, DIVISOR_LATCH_LSB, Data);

	Data = 0x0;
	status = 0;
	status = mos7840_set_uart_reg(port, DIVISOR_LATCH_MSB, Data);

	Data = 0x00;
	status = 0;
	status = mos7840_get_uart_reg(port, LINE_CONTROL_REGISTER, &Data);

	Data = Data & ~SERIAL_LCR_DLAB;
	status = 0;
	status = mos7840_set_uart_reg(port, LINE_CONTROL_REGISTER, Data);
	mos7840_port->shadowLCR = Data;

	//clearing Bulkin and Bulkout Fifo
	Data = 0x0;
	status = 0;
	status = mos7840_get_reg_sync(port, mos7840_port->SpRegOffset, &Data);

	Data = Data | 0x0c;
	status = 0;
	status = mos7840_set_reg_sync(port, mos7840_port->SpRegOffset, Data);

	Data = Data & ~0x0c;
	status = 0;
	status = mos7840_set_reg_sync(port, mos7840_port->SpRegOffset, Data);
	//Finally enable all interrupts
	Data = 0x0;
	Data = 0x0c;
	status = 0;
	status = mos7840_set_uart_reg(port, INTERRUPT_ENABLE_REGISTER, Data);

	//clearing rx_disable
	Data = 0x0;
	status = 0;
	status =
	    mos7840_get_reg_sync(port, mos7840_port->ControlRegOffset, &Data);
	Data = Data & ~0x20;
	status = 0;
	status =
	    mos7840_set_reg_sync(port, mos7840_port->ControlRegOffset, Data);

	// rx_negate
	Data = 0x0;
	status = 0;
	status =
	    mos7840_get_reg_sync(port, mos7840_port->ControlRegOffset, &Data);
	Data = Data | 0x10;
	status = 0;
	status =
	    mos7840_set_reg_sync(port, mos7840_port->ControlRegOffset, Data);

	/* force low_latency on so that our tty_push actually forces *
	 * the data through,otherwise it is scheduled, and with      *
	 * high data rates (like with OHCI) data can get lost.       */

	if (port->tty)
		port->tty->low_latency = 1;
/* Check to see if we've set up our endpoint info yet    *
     * (can't set it up in mos7840_startup as the structures *
     * were not set up at that time.)                        */
	if (port0->open_ports == 1) {
		if (serial->port[0]->interrupt_in_buffer == NULL) {

			/* set up interrupt urb */

			usb_fill_int_urb(serial->port[0]->interrupt_in_urb,
					 serial->dev,
					 usb_rcvintpipe(serial->dev,
							serial->port[0]->
							interrupt_in_endpointAddress),
					 serial->port[0]->interrupt_in_buffer,
					 serial->port[0]->interrupt_in_urb->
					 transfer_buffer_length,
					 mos7840_interrupt_callback,
					 serial,
					 serial->port[0]->interrupt_in_urb->
					 interval);

			/* start interrupt read for mos7840               *
			 * will continue as long as mos7840 is connected  */

			response =
			    usb_submit_urb(serial->port[0]->interrupt_in_urb,
					   GFP_KERNEL);
			if (response) {
				err("%s - Error %d submitting interrupt urb",
				    __FUNCTION__, response);
			}

		}

	}

	/* see if we've set up our endpoint info yet   *
	 * (can't set it up in mos7840_startup as the  *
	 * structures were not set up at that time.)   */

	dbg("port number is %d \n", port->number);
	dbg("serial number is %d \n", port->serial->minor);
	dbg("Bulkin endpoint is %d \n", port->bulk_in_endpointAddress);
	dbg("BulkOut endpoint is %d \n", port->bulk_out_endpointAddress);
	dbg("Interrupt endpoint is %d \n", port->interrupt_in_endpointAddress);
	dbg("port's number in the device is %d\n", mos7840_port->port_num);
	mos7840_port->read_urb = port->read_urb;

	/* set up our bulk in urb */

	usb_fill_bulk_urb(mos7840_port->read_urb,
			  serial->dev,
			  usb_rcvbulkpipe(serial->dev,
					  port->bulk_in_endpointAddress),
			  port->bulk_in_buffer,
			  mos7840_port->read_urb->transfer_buffer_length,
			  mos7840_bulk_in_callback, mos7840_port);

	dbg("mos7840_open: bulkin endpoint is %d\n",
	    port->bulk_in_endpointAddress);
	response = usb_submit_urb(mos7840_port->read_urb, GFP_KERNEL);
	if (response) {
		err("%s - Error %d submitting control urb", __FUNCTION__,
		    response);
	}

	/* initialize our wait queues */
	init_waitqueue_head(&mos7840_port->wait_chase);
	init_waitqueue_head(&mos7840_port->delta_msr_wait);

	/* initialize our icount structure */
	memset(&(mos7840_port->icount), 0x00, sizeof(mos7840_port->icount));

	/* initialize our port settings */
	mos7840_port->shadowMCR = MCR_MASTER_IE;	/* Must set to enable ints! */
	/* send a open port command */
	mos7840_port->open = 1;
	//mos7840_change_port_settings(mos7840_port,old_termios);
	mos7840_port->icount.tx = 0;
	mos7840_port->icount.rx = 0;

	dbg("\n\nusb_serial serial:%p       mos7840_port:%p\n      usb_serial_port port:%p\n\n", serial, mos7840_port, port);

	return 0;

}

/*****************************************************************************
 * mos7840_chars_in_buffer
 *	this function is called by the tty driver when it wants to know how many
 *	bytes of data we currently have outstanding in the port (data that has
 *	been written, but hasn't made it out the port yet)
 *	If successful, we return the number of bytes left to be written in the
 *	system,
 *	Otherwise we return a negative error number.
 *****************************************************************************/

static int mos7840_chars_in_buffer(struct usb_serial_port *port)
{
	int i;
	int chars = 0;
	unsigned long flags;
	struct moschip_port *mos7840_port;

	dbg("%s \n", " mos7840_chars_in_buffer:entering ...........");

	if (mos7840_port_paranoia_check(port, __FUNCTION__)) {
		dbg("%s", "Invalid port \n");
		return -1;
	}

	mos7840_port = mos7840_get_port_private(port);
	if (mos7840_port == NULL) {
		dbg("%s \n", "mos7840_break:leaving ...........");
		return -1;
	}

	spin_lock_irqsave(&mos7840_port->pool_lock,flags);
	for (i = 0; i < NUM_URBS; ++i) {
		if (mos7840_port->busy[i]) {
			chars += URB_TRANSFER_BUFFER_SIZE;
		}
	}
	spin_unlock_irqrestore(&mos7840_port->pool_lock,flags);
	dbg("%s - returns %d", __FUNCTION__, chars);
	return chars;

}

/************************************************************************
 *
 * mos7840_block_until_tx_empty
 *
 *	This function will block the close until one of the following:
 *		1. TX count are 0
 *		2. The mos7840 has stopped
 *		3. A timout of 3 seconds without activity has expired
 *
 ************************************************************************/
static void mos7840_block_until_tx_empty(struct moschip_port *mos7840_port)
{
	int timeout = HZ / 10;
	int wait = 30;
	int count;

	while (1) {

		count = mos7840_chars_in_buffer(mos7840_port->port);

		/* Check for Buffer status */
		if (count <= 0) {
			return;
		}

		/* Block the thread for a while */
		interruptible_sleep_on_timeout(&mos7840_port->wait_chase,
					       timeout);

		/* No activity.. count down section */
		wait--;
		if (wait == 0) {
			dbg("%s - TIMEOUT", __FUNCTION__);
			return;
		} else {
			/* Reset timout value back to seconds */
			wait = 30;
		}
	}
}

/*****************************************************************************
 * mos7840_close
 *	this function is called by the tty driver when a port is closed
 *****************************************************************************/

static void mos7840_close(struct usb_serial_port *port, struct file *filp)
{
	struct usb_serial *serial;
	struct moschip_port *mos7840_port;
	struct moschip_port *port0;
	int j;
	__u16 Data;

	dbg("%s\n", "mos7840_close:entering...");

	if (mos7840_port_paranoia_check(port, __FUNCTION__)) {
		dbg("%s", "Port Paranoia failed \n");
		return;
	}

	serial = mos7840_get_usb_serial(port, __FUNCTION__);
	if (!serial) {
		dbg("%s", "Serial Paranoia failed \n");
		return;
	}

	mos7840_port = mos7840_get_port_private(port);
	port0 = mos7840_get_port_private(serial->port[0]);

	if (mos7840_port == NULL || port0 == NULL)
		return;

	for (j = 0; j < NUM_URBS; ++j)
		usb_kill_urb(mos7840_port->write_urb_pool[j]);

	/* Freeing Write URBs */
	for (j = 0; j < NUM_URBS; ++j) {
		if (mos7840_port->write_urb_pool[j]) {
			if (mos7840_port->write_urb_pool[j]->transfer_buffer)
				kfree(mos7840_port->write_urb_pool[j]->
				      transfer_buffer);

			usb_free_urb(mos7840_port->write_urb_pool[j]);
		}
	}

	if (serial->dev) {
		/* flush and block until tx is empty */
		mos7840_block_until_tx_empty(mos7840_port);
	}

	/* While closing port, shutdown all bulk read, write  *
	 * and interrupt read if they exists                  */
	if (serial->dev) {

		if (mos7840_port->write_urb) {
			dbg("%s", "Shutdown bulk write\n");
			usb_kill_urb(mos7840_port->write_urb);
		}

		if (mos7840_port->read_urb) {
			dbg("%s", "Shutdown bulk read\n");
			usb_kill_urb(mos7840_port->read_urb);
		}
		if ((&mos7840_port->control_urb)) {
			dbg("%s", "Shutdown control read\n");
			//      usb_kill_urb (mos7840_port->control_urb);

		}
	}
//              if(mos7840_port->ctrl_buf != NULL)
//                      kfree(mos7840_port->ctrl_buf);
	port0->open_ports--;
	dbg("mos7840_num_open_ports in close%d:in port%d\n",
	    port0->open_ports, port->number);
	if (port0->open_ports == 0) {
		if (serial->port[0]->interrupt_in_urb) {
			dbg("%s", "Shutdown interrupt_in_urb\n");
			usb_kill_urb(serial->port[0]->interrupt_in_urb);
		}
	}

	if (mos7840_port->write_urb) {
		/* if this urb had a transfer buffer already (old tx) free it */

		if (mos7840_port->write_urb->transfer_buffer != NULL) {
			kfree(mos7840_port->write_urb->transfer_buffer);
		}
		usb_free_urb(mos7840_port->write_urb);
	}

	Data = 0x0;
	mos7840_set_uart_reg(port, MODEM_CONTROL_REGISTER, Data);

	Data = 0x00;
	mos7840_set_uart_reg(port, INTERRUPT_ENABLE_REGISTER, Data);

	mos7840_port->open = 0;

	dbg("%s \n", "Leaving ............");
}

/************************************************************************
 *
 * mos7840_block_until_chase_response
 *
 *	This function will block the close until one of the following:
 *		1. Response to our Chase comes from mos7840
 *		2. A timout of 10 seconds without activity has expired
 *		   (1K of mos7840 data @ 2400 baud ==> 4 sec to empty)
 *
 ************************************************************************/

static void mos7840_block_until_chase_response(struct moschip_port
					       *mos7840_port)
{
	int timeout = 1 * HZ;
	int wait = 10;
	int count;

	while (1) {
		count = mos7840_chars_in_buffer(mos7840_port->port);

		/* Check for Buffer status */
		if (count <= 0) {
			return;
		}

		/* Block the thread for a while */
		interruptible_sleep_on_timeout(&mos7840_port->wait_chase,
					       timeout);
		/* No activity.. count down section */
		wait--;
		if (wait == 0) {
			dbg("%s - TIMEOUT", __FUNCTION__);
			return;
		} else {
			/* Reset timout value back to seconds */
			wait = 10;
		}
	}

}

/*****************************************************************************
 * mos7840_break
 *	this function sends a break to the port
 *****************************************************************************/
static void mos7840_break(struct usb_serial_port *port, int break_state)
{
	unsigned char data;
	struct usb_serial *serial;
	struct moschip_port *mos7840_port;

	dbg("%s \n", "Entering ...........");
	dbg("mos7840_break: Start\n");

	if (mos7840_port_paranoia_check(port, __FUNCTION__)) {
		dbg("%s", "Port Paranoia failed \n");
		return;
	}

	serial = mos7840_get_usb_serial(port, __FUNCTION__);
	if (!serial) {
		dbg("%s", "Serial Paranoia failed \n");
		return;
	}

	mos7840_port = mos7840_get_port_private(port);

	if (mos7840_port == NULL) {
		return;
	}

	if (serial->dev) {

		/* flush and block until tx is empty */
		mos7840_block_until_chase_response(mos7840_port);
	}

	if (break_state == -1) {
		data = mos7840_port->shadowLCR | LCR_SET_BREAK;
	} else {
		data = mos7840_port->shadowLCR & ~LCR_SET_BREAK;
	}

	mos7840_port->shadowLCR = data;
	dbg("mcs7840_break mos7840_port->shadowLCR is %x\n",
	    mos7840_port->shadowLCR);
	mos7840_set_uart_reg(port, LINE_CONTROL_REGISTER,
			     mos7840_port->shadowLCR);

	return;
}

/*****************************************************************************
 * mos7840_write_room
 *	this function is called by the tty driver when it wants to know how many
 *	bytes of data we can accept for a specific port.
 *	If successful, we return the amount of room that we have for this port
 *	Otherwise we return a negative error number.
 *****************************************************************************/

static int mos7840_write_room(struct usb_serial_port *port)
{
	int i;
	int room = 0;
	unsigned long flags;
	struct moschip_port *mos7840_port;

	dbg("%s \n", " mos7840_write_room:entering ...........");

	if (mos7840_port_paranoia_check(port, __FUNCTION__)) {
		dbg("%s", "Invalid port \n");
		dbg("%s \n", " mos7840_write_room:leaving ...........");
		return -1;
	}

	mos7840_port = mos7840_get_port_private(port);
	if (mos7840_port == NULL) {
		dbg("%s \n", "mos7840_break:leaving ...........");
		return -1;
	}

	spin_lock_irqsave(&mos7840_port->pool_lock, flags);
	for (i = 0; i < NUM_URBS; ++i) {
		if (!mos7840_port->busy[i]) {
			room += URB_TRANSFER_BUFFER_SIZE;
		}
	}
	spin_unlock_irqrestore(&mos7840_port->pool_lock, flags);

	room = (room == 0) ? 0 : room - URB_TRANSFER_BUFFER_SIZE + 1;
	dbg("%s - returns %d", __FUNCTION__, room);
	return room;

}

/*****************************************************************************
 * mos7840_write
 *	this function is called by the tty driver when data should be written to
 *	the port.
 *	If successful, we return the number of bytes written, otherwise we
 *      return a negative error number.
 *****************************************************************************/

static int mos7840_write(struct usb_serial_port *port,
			 const unsigned char *data, int count)
{
	int status;
	int i;
	int bytes_sent = 0;
	int transfer_size;
	unsigned long flags;

	struct moschip_port *mos7840_port;
	struct usb_serial *serial;
	struct urb *urb;
	//__u16 Data;
	const unsigned char *current_position = data;
	unsigned char *data1;
	dbg("%s \n", "entering ...........");
	//dbg("mos7840_write: mos7840_port->shadowLCR is %x\n",mos7840_port->shadowLCR);

#ifdef NOTMOS7840
	Data = 0x00;
	status = 0;
	status = mos7840_get_uart_reg(port, LINE_CONTROL_REGISTER, &Data);
	mos7840_port->shadowLCR = Data;
	dbg("mos7840_write: LINE_CONTROL_REGISTER is %x\n", Data);
	dbg("mos7840_write: mos7840_port->shadowLCR is %x\n",
	    mos7840_port->shadowLCR);

	//Data = 0x03;
	//status = mos7840_set_uart_reg(port,LINE_CONTROL_REGISTER,Data);
	//mos7840_port->shadowLCR=Data;//Need to add later

	Data |= SERIAL_LCR_DLAB;	//data latch enable in LCR 0x80
	status = 0;
	status = mos7840_set_uart_reg(port, LINE_CONTROL_REGISTER, Data);

	//Data = 0x0c;
	//status = mos7840_set_uart_reg(port,DIVISOR_LATCH_LSB,Data);
	Data = 0x00;
	status = 0;
	status = mos7840_get_uart_reg(port, DIVISOR_LATCH_LSB, &Data);
	dbg("mos7840_write:DLL value is %x\n", Data);

	Data = 0x0;
	status = 0;
	status = mos7840_get_uart_reg(port, DIVISOR_LATCH_MSB, &Data);
	dbg("mos7840_write:DLM value is %x\n", Data);

	Data = Data & ~SERIAL_LCR_DLAB;
	dbg("mos7840_write: mos7840_port->shadowLCR is %x\n",
	    mos7840_port->shadowLCR);
	status = 0;
	status = mos7840_set_uart_reg(port, LINE_CONTROL_REGISTER, Data);
#endif

	if (mos7840_port_paranoia_check(port, __FUNCTION__)) {
		dbg("%s", "Port Paranoia failed \n");
		return -1;
	}

	serial = port->serial;
	if (mos7840_serial_paranoia_check(serial, __FUNCTION__)) {
		dbg("%s", "Serial Paranoia failed \n");
		return -1;
	}

	mos7840_port = mos7840_get_port_private(port);
	if (mos7840_port == NULL) {
		dbg("%s", "mos7840_port is NULL\n");
		return -1;
	}

	/* try to find a free urb in the list */
	urb = NULL;

	spin_lock_irqsave(&mos7840_port->pool_lock, flags);
	for (i = 0; i < NUM_URBS; ++i) {
		if (!mos7840_port->busy[i]) {
			mos7840_port->busy[i] = 1;
			urb = mos7840_port->write_urb_pool[i];
			dbg("\nURB:%d", i);
			break;
		}
	}
	spin_unlock_irqrestore(&mos7840_port->pool_lock, flags);

	if (urb == NULL) {
		dbg("%s - no more free urbs", __FUNCTION__);
		goto exit;
	}

	if (urb->transfer_buffer == NULL) {
		urb->transfer_buffer =
		    kmalloc(URB_TRANSFER_BUFFER_SIZE, GFP_KERNEL);

		if (urb->transfer_buffer == NULL) {
			err("%s no more kernel memory...", __FUNCTION__);
			goto exit;
		}
	}
	transfer_size = min(count, URB_TRANSFER_BUFFER_SIZE);

	memcpy(urb->transfer_buffer, current_position, transfer_size);

	/* fill urb with data and submit  */
	usb_fill_bulk_urb(urb,
			  serial->dev,
			  usb_sndbulkpipe(serial->dev,
					  port->bulk_out_endpointAddress),
			  urb->transfer_buffer,
			  transfer_size,
			  mos7840_bulk_out_data_callback, mos7840_port);

	data1 = urb->transfer_buffer;
	dbg("\nbulkout endpoint is %d", port->bulk_out_endpointAddress);

	/* send it down the pipe */
	status = usb_submit_urb(urb, GFP_ATOMIC);

	if (status) {
		mos7840_port->busy[i] = 0;
		err("%s - usb_submit_urb(write bulk) failed with status = %d",
		    __FUNCTION__, status);
		bytes_sent = status;
		goto exit;
	}
	bytes_sent = transfer_size;
	mos7840_port->icount.tx += transfer_size;
	smp_wmb();
	dbg("mos7840_port->icount.tx is %d:\n", mos7840_port->icount.tx);
      exit:

	return bytes_sent;

}

/*****************************************************************************
 * mos7840_throttle
 *	this function is called by the tty driver when it wants to stop the data
 *	being read from the port.
 *****************************************************************************/

static void mos7840_throttle(struct usb_serial_port *port)
{
	struct moschip_port *mos7840_port;
	struct tty_struct *tty;
	int status;

	if (mos7840_port_paranoia_check(port, __FUNCTION__)) {
		dbg("%s", "Invalid port \n");
		return;
	}

	dbg("- port %d\n", port->number);

	mos7840_port = mos7840_get_port_private(port);

	if (mos7840_port == NULL)
		return;

	if (!mos7840_port->open) {
		dbg("%s\n", "port not opened");
		return;
	}

	dbg("%s", "Entering .......... \n");

	tty = port->tty;
	if (!tty) {
		dbg("%s - no tty available", __FUNCTION__);
		return;
	}

	/* if we are implementing XON/XOFF, send the stop character */
	if (I_IXOFF(tty)) {
		unsigned char stop_char = STOP_CHAR(tty);
		status = mos7840_write(port, &stop_char, 1);
		if (status <= 0) {
			return;
		}
	}

	/* if we are implementing RTS/CTS, toggle that line */
	if (tty->termios->c_cflag & CRTSCTS) {
		mos7840_port->shadowMCR &= ~MCR_RTS;
		status = 0;
		status =
		    mos7840_set_uart_reg(port, MODEM_CONTROL_REGISTER,
					 mos7840_port->shadowMCR);

		if (status < 0) {
			return;
		}
	}

	return;
}

/*****************************************************************************
 * mos7840_unthrottle
 *	this function is called by the tty driver when it wants to resume the data
 *	being read from the port (called after SerialThrottle is called)
 *****************************************************************************/
static void mos7840_unthrottle(struct usb_serial_port *port)
{
	struct tty_struct *tty;
	int status;
	struct moschip_port *mos7840_port = mos7840_get_port_private(port);

	if (mos7840_port_paranoia_check(port, __FUNCTION__)) {
		dbg("%s", "Invalid port \n");
		return;
	}

	if (mos7840_port == NULL)
		return;

	if (!mos7840_port->open) {
		dbg("%s - port not opened", __FUNCTION__);
		return;
	}

	dbg("%s", "Entering .......... \n");

	tty = port->tty;
	if (!tty) {
		dbg("%s - no tty available", __FUNCTION__);
		return;
	}

	/* if we are implementing XON/XOFF, send the start character */
	if (I_IXOFF(tty)) {
		unsigned char start_char = START_CHAR(tty);
		status = mos7840_write(port, &start_char, 1);
		if (status <= 0) {
			return;
		}
	}

	/* if we are implementing RTS/CTS, toggle that line */
	if (tty->termios->c_cflag & CRTSCTS) {
		mos7840_port->shadowMCR |= MCR_RTS;
		status = 0;
		status =
		    mos7840_set_uart_reg(port, MODEM_CONTROL_REGISTER,
					 mos7840_port->shadowMCR);
		if (status < 0) {
			return;
		}
	}

	return;
}

static int mos7840_tiocmget(struct usb_serial_port *port, struct file *file)
{
	struct moschip_port *mos7840_port;
	unsigned int result;
	__u16 msr;
	__u16 mcr;
	int status = 0;
	mos7840_port = mos7840_get_port_private(port);

	dbg("%s - port %d", __FUNCTION__, port->number);

	if (mos7840_port == NULL)
		return -ENODEV;

	status = mos7840_get_uart_reg(port, MODEM_STATUS_REGISTER, &msr);
	status = mos7840_get_uart_reg(port, MODEM_CONTROL_REGISTER, &mcr);
	result = ((mcr & MCR_DTR) ? TIOCM_DTR : 0)
	    | ((mcr & MCR_RTS) ? TIOCM_RTS : 0)
	    | ((mcr & MCR_LOOPBACK) ? TIOCM_LOOP : 0)
	    | ((msr & MOS7840_MSR_CTS) ? TIOCM_CTS : 0)
	    | ((msr & MOS7840_MSR_CD) ? TIOCM_CAR : 0)
	    | ((msr & MOS7840_MSR_RI) ? TIOCM_RI : 0)
	    | ((msr & MOS7840_MSR_DSR) ? TIOCM_DSR : 0);

	dbg("%s - 0x%04X", __FUNCTION__, result);

	return result;
}

static int mos7840_tiocmset(struct usb_serial_port *port, struct file *file,
			    unsigned int set, unsigned int clear)
{
	struct moschip_port *mos7840_port;
	unsigned int mcr;
	unsigned int status;

	dbg("%s - port %d", __FUNCTION__, port->number);

	mos7840_port = mos7840_get_port_private(port);

	if (mos7840_port == NULL)
		return -ENODEV;

	mcr = mos7840_port->shadowMCR;
	if (clear & TIOCM_RTS)
		mcr &= ~MCR_RTS;
	if (clear & TIOCM_DTR)
		mcr &= ~MCR_DTR;
	if (clear & TIOCM_LOOP)
		mcr &= ~MCR_LOOPBACK;

	if (set & TIOCM_RTS)
		mcr |= MCR_RTS;
	if (set & TIOCM_DTR)
		mcr |= MCR_DTR;
	if (set & TIOCM_LOOP)
		mcr |= MCR_LOOPBACK;

	mos7840_port->shadowMCR = mcr;

	status = 0;
	status = mos7840_set_uart_reg(port, MODEM_CONTROL_REGISTER, mcr);
	if (status < 0) {
		dbg("setting MODEM_CONTROL_REGISTER Failed\n");
		return -1;
	}

	return 0;
}

/*****************************************************************************
 * mos7840_calc_baud_rate_divisor
 *	this function calculates the proper baud rate divisor for the specified
 *	baud rate.
 *****************************************************************************/
static int mos7840_calc_baud_rate_divisor(int baudRate, int *divisor,
					  __u16 * clk_sel_val)
{

	dbg("%s - %d", __FUNCTION__, baudRate);

	if (baudRate <= 115200) {
		*divisor = 115200 / baudRate;
		*clk_sel_val = 0x0;
	}
	if ((baudRate > 115200) && (baudRate <= 230400)) {
		*divisor = 230400 / baudRate;
		*clk_sel_val = 0x10;
	} else if ((baudRate > 230400) && (baudRate <= 403200)) {
		*divisor = 403200 / baudRate;
		*clk_sel_val = 0x20;
	} else if ((baudRate > 403200) && (baudRate <= 460800)) {
		*divisor = 460800 / baudRate;
		*clk_sel_val = 0x30;
	} else if ((baudRate > 460800) && (baudRate <= 806400)) {
		*divisor = 806400 / baudRate;
		*clk_sel_val = 0x40;
	} else if ((baudRate > 806400) && (baudRate <= 921600)) {
		*divisor = 921600 / baudRate;
		*clk_sel_val = 0x50;
	} else if ((baudRate > 921600) && (baudRate <= 1572864)) {
		*divisor = 1572864 / baudRate;
		*clk_sel_val = 0x60;
	} else if ((baudRate > 1572864) && (baudRate <= 3145728)) {
		*divisor = 3145728 / baudRate;
		*clk_sel_val = 0x70;
	}
	return 0;

#ifdef NOTMCS7840

	for (i = 0; i < ARRAY_SIZE(mos7840_divisor_table); i++) {
		if (mos7840_divisor_table[i].BaudRate == baudrate) {
			*divisor = mos7840_divisor_table[i].Divisor;
			return 0;
		}
	}

	/* After trying for all the standard baud rates    *
	 * Try calculating the divisor for this baud rate  */

	if (baudrate > 75 && baudrate < 230400) {
		/* get the divisor */
		custom = (__u16) (230400L / baudrate);

		/* Check for round off */
		round1 = (__u16) (2304000L / baudrate);
		round = (__u16) (round1 - (custom * 10));
		if (round > 4) {
			custom++;
		}
		*divisor = custom;

		dbg(" Baud %d = %d\n", baudrate, custom);
		return 0;
	}

	dbg("%s\n", " Baud calculation Failed...");
	return -1;
#endif
}

/*****************************************************************************
 * mos7840_send_cmd_write_baud_rate
 *	this function sends the proper command to change the baud rate of the
 *	specified port.
 *****************************************************************************/

static int mos7840_send_cmd_write_baud_rate(struct moschip_port *mos7840_port,
					    int baudRate)
{
	int divisor = 0;
	int status;
	__u16 Data;
	unsigned char number;
	__u16 clk_sel_val;
	struct usb_serial_port *port;

	if (mos7840_port == NULL)
		return -1;

	port = (struct usb_serial_port *)mos7840_port->port;
	if (mos7840_port_paranoia_check(port, __FUNCTION__)) {
		dbg("%s", "Invalid port \n");
		return -1;
	}

	if (mos7840_serial_paranoia_check(port->serial, __FUNCTION__)) {
		dbg("%s", "Invalid Serial \n");
		return -1;
	}

	dbg("%s", "Entering .......... \n");

	number = mos7840_port->port->number - mos7840_port->port->serial->minor;

	dbg("%s - port = %d, baud = %d", __FUNCTION__,
	    mos7840_port->port->number, baudRate);
	//reset clk_uart_sel in spregOffset
	if (baudRate > 115200) {
#ifdef HW_flow_control
		//NOTE: need to see the pther register to modify
		//setting h/w flow control bit to 1;
		status = 0;
		Data = 0x2b;
		mos7840_port->shadowMCR = Data;
		status =
		    mos7840_set_uart_reg(port, MODEM_CONTROL_REGISTER, Data);
		if (status < 0) {
			dbg("Writing spreg failed in set_serial_baud\n");
			return -1;
		}
#endif

	} else {
#ifdef HW_flow_control
		//setting h/w flow control bit to 0;
		status = 0;
		Data = 0xb;
		mos7840_port->shadowMCR = Data;
		status =
		    mos7840_set_uart_reg(port, MODEM_CONTROL_REGISTER, Data);
		if (status < 0) {
			dbg("Writing spreg failed in set_serial_baud\n");
			return -1;
		}
#endif

	}

	if (1)			//baudRate <= 115200)
	{
		clk_sel_val = 0x0;
		Data = 0x0;
		status = 0;
		status =
		    mos7840_calc_baud_rate_divisor(baudRate, &divisor,
						   &clk_sel_val);
		status =
		    mos7840_get_reg_sync(port, mos7840_port->SpRegOffset,
					 &Data);
		if (status < 0) {
			dbg("reading spreg failed in set_serial_baud\n");
			return -1;
		}
		Data = (Data & 0x8f) | clk_sel_val;
		status = 0;
		status =
		    mos7840_set_reg_sync(port, mos7840_port->SpRegOffset, Data);
		if (status < 0) {
			dbg("Writing spreg failed in set_serial_baud\n");
			return -1;
		}
		/* Calculate the Divisor */

		if (status) {
			err("%s - bad baud rate", __FUNCTION__);
			dbg("%s\n", "bad baud rate");
			return status;
		}
		/* Enable access to divisor latch */
		Data = mos7840_port->shadowLCR | SERIAL_LCR_DLAB;
		mos7840_port->shadowLCR = Data;
		mos7840_set_uart_reg(port, LINE_CONTROL_REGISTER, Data);

		/* Write the divisor */
		Data = (unsigned char)(divisor & 0xff);
		dbg("set_serial_baud Value to write DLL is %x\n", Data);
		mos7840_set_uart_reg(port, DIVISOR_LATCH_LSB, Data);

		Data = (unsigned char)((divisor & 0xff00) >> 8);
		dbg("set_serial_baud Value to write DLM is %x\n", Data);
		mos7840_set_uart_reg(port, DIVISOR_LATCH_MSB, Data);

		/* Disable access to divisor latch */
		Data = mos7840_port->shadowLCR & ~SERIAL_LCR_DLAB;
		mos7840_port->shadowLCR = Data;
		mos7840_set_uart_reg(port, LINE_CONTROL_REGISTER, Data);

	}

	return status;
}

/*****************************************************************************
 * mos7840_change_port_settings
 *	This routine is called to set the UART on the device to match
 *      the specified new settings.
 *****************************************************************************/

static void mos7840_change_port_settings(struct moschip_port *mos7840_port,
					 struct ktermios *old_termios)
{
	struct tty_struct *tty;
	int baud;
	unsigned cflag;
	unsigned iflag;
	__u8 lData;
	__u8 lParity;
	__u8 lStop;
	int status;
	__u16 Data;
	struct usb_serial_port *port;
	struct usb_serial *serial;

	if (mos7840_port == NULL)
		return;

	port = (struct usb_serial_port *)mos7840_port->port;

	if (mos7840_port_paranoia_check(port, __FUNCTION__)) {
		dbg("%s", "Invalid port \n");
		return;
	}

	if (mos7840_serial_paranoia_check(port->serial, __FUNCTION__)) {
		dbg("%s", "Invalid Serial \n");
		return;
	}

	serial = port->serial;

	dbg("%s - port %d", __FUNCTION__, mos7840_port->port->number);

	if (!mos7840_port->open) {
		dbg("%s - port not opened", __FUNCTION__);
		return;
	}

	tty = mos7840_port->port->tty;

	if ((!tty) || (!tty->termios)) {
		dbg("%s - no tty structures", __FUNCTION__);
		return;
	}

	dbg("%s", "Entering .......... \n");

	lData = LCR_BITS_8;
	lStop = LCR_STOP_1;
	lParity = LCR_PAR_NONE;

	cflag = tty->termios->c_cflag;
	iflag = tty->termios->c_iflag;

	/* Change the number of bits */
	if (cflag & CSIZE) {
		switch (cflag & CSIZE) {
		case CS5:
			lData = LCR_BITS_5;
			break;

		case CS6:
			lData = LCR_BITS_6;
			break;

		case CS7:
			lData = LCR_BITS_7;
			break;
		default:
		case CS8:
			lData = LCR_BITS_8;
			break;
		}
	}
	/* Change the Parity bit */
	if (cflag & PARENB) {
		if (cflag & PARODD) {
			lParity = LCR_PAR_ODD;
			dbg("%s - parity = odd", __FUNCTION__);
		} else {
			lParity = LCR_PAR_EVEN;
			dbg("%s - parity = even", __FUNCTION__);
		}

	} else {
		dbg("%s - parity = none", __FUNCTION__);
	}

	if (cflag & CMSPAR) {
		lParity = lParity | 0x20;
	}

	/* Change the Stop bit */
	if (cflag & CSTOPB) {
		lStop = LCR_STOP_2;
		dbg("%s - stop bits = 2", __FUNCTION__);
	} else {
		lStop = LCR_STOP_1;
		dbg("%s - stop bits = 1", __FUNCTION__);
	}

	/* Update the LCR with the correct value */
	mos7840_port->shadowLCR &=
	    ~(LCR_BITS_MASK | LCR_STOP_MASK | LCR_PAR_MASK);
	mos7840_port->shadowLCR |= (lData | lParity | lStop);

	dbg("mos7840_change_port_settings mos7840_port->shadowLCR is %x\n",
	    mos7840_port->shadowLCR);
	/* Disable Interrupts */
	Data = 0x00;
	mos7840_set_uart_reg(port, INTERRUPT_ENABLE_REGISTER, Data);

	Data = 0x00;
	mos7840_set_uart_reg(port, FIFO_CONTROL_REGISTER, Data);

	Data = 0xcf;
	mos7840_set_uart_reg(port, FIFO_CONTROL_REGISTER, Data);

	/* Send the updated LCR value to the mos7840 */
	Data = mos7840_port->shadowLCR;

	mos7840_set_uart_reg(port, LINE_CONTROL_REGISTER, Data);

	Data = 0x00b;
	mos7840_port->shadowMCR = Data;
	mos7840_set_uart_reg(port, MODEM_CONTROL_REGISTER, Data);
	Data = 0x00b;
	mos7840_set_uart_reg(port, MODEM_CONTROL_REGISTER, Data);

	/* set up the MCR register and send it to the mos7840 */

	mos7840_port->shadowMCR = MCR_MASTER_IE;
	if (cflag & CBAUD) {
		mos7840_port->shadowMCR |= (MCR_DTR | MCR_RTS);
	}

	if (cflag & CRTSCTS) {
		mos7840_port->shadowMCR |= (MCR_XON_ANY);

	} else {
		mos7840_port->shadowMCR &= ~(MCR_XON_ANY);
	}

	Data = mos7840_port->shadowMCR;
	mos7840_set_uart_reg(port, MODEM_CONTROL_REGISTER, Data);

	/* Determine divisor based on baud rate */
	baud = tty_get_baud_rate(tty);

	if (!baud) {
		/* pick a default, any default... */
		dbg("%s\n", "Picked default baud...");
		baud = 9600;
	}

	dbg("%s - baud rate = %d", __FUNCTION__, baud);
	status = mos7840_send_cmd_write_baud_rate(mos7840_port, baud);

	/* Enable Interrupts */
	Data = 0x0c;
	mos7840_set_uart_reg(port, INTERRUPT_ENABLE_REGISTER, Data);

	if (mos7840_port->read_urb->status != -EINPROGRESS) {
		mos7840_port->read_urb->dev = serial->dev;

		status = usb_submit_urb(mos7840_port->read_urb, GFP_ATOMIC);

		if (status) {
			dbg(" usb_submit_urb(read bulk) failed, status = %d",
			    status);
		}
	}
	wake_up(&mos7840_port->delta_msr_wait);
	mos7840_port->delta_msr_cond = 1;
	dbg("mos7840_change_port_settings mos7840_port->shadowLCR is End %x\n",
	    mos7840_port->shadowLCR);

	return;
}

/*****************************************************************************
 * mos7840_set_termios
 *	this function is called by the tty driver when it wants to change
 *	the termios structure
 *****************************************************************************/

static void mos7840_set_termios(struct usb_serial_port *port,
				struct ktermios *old_termios)
{
	int status;
	unsigned int cflag;
	struct usb_serial *serial;
	struct moschip_port *mos7840_port;
	struct tty_struct *tty;
	dbg("mos7840_set_termios: START\n");
	if (mos7840_port_paranoia_check(port, __FUNCTION__)) {
		dbg("%s", "Invalid port \n");
		return;
	}

	serial = port->serial;

	if (mos7840_serial_paranoia_check(serial, __FUNCTION__)) {
		dbg("%s", "Invalid Serial \n");
		return;
	}

	mos7840_port = mos7840_get_port_private(port);

	if (mos7840_port == NULL)
		return;

	tty = port->tty;

	if (!port->tty || !port->tty->termios) {
		dbg("%s - no tty or termios", __FUNCTION__);
		return;
	}

	if (!mos7840_port->open) {
		dbg("%s - port not opened", __FUNCTION__);
		return;
	}

	dbg("%s\n", "setting termios - ");

	cflag = tty->termios->c_cflag;

	if (!cflag) {
		dbg("%s %s\n", __FUNCTION__, "cflag is NULL");
		return;
	}

	dbg("%s - clfag %08x iflag %08x", __FUNCTION__,
	    tty->termios->c_cflag, RELEVANT_IFLAG(tty->termios->c_iflag));

	if (old_termios) {
		dbg("%s - old clfag %08x old iflag %08x", __FUNCTION__,
		    old_termios->c_cflag, RELEVANT_IFLAG(old_termios->c_iflag));
	}

	dbg("%s - port %d", __FUNCTION__, port->number);

	/* change the port settings to the new ones specified */

	mos7840_change_port_settings(mos7840_port, old_termios);

	if (!mos7840_port->read_urb) {
		dbg("%s", "URB KILLED !!!!!\n");
		return;
	}

	if (mos7840_port->read_urb->status != -EINPROGRESS) {
		mos7840_port->read_urb->dev = serial->dev;
		status = usb_submit_urb(mos7840_port->read_urb, GFP_ATOMIC);
		if (status) {
			dbg(" usb_submit_urb(read bulk) failed, status = %d",
			    status);
		}
	}
	return;
}

/*****************************************************************************
 * mos7840_get_lsr_info - get line status register info
 *
 * Purpose: Let user call ioctl() to get info when the UART physically
 * 	    is emptied.  On bus types like RS485, the transmitter must
 * 	    release the bus after transmitting. This must be done when
 * 	    the transmit shift register is empty, not be done when the
 * 	    transmit holding register is empty.  This functionality
 * 	    allows an RS485 driver to be written in user space.
 *****************************************************************************/

static int mos7840_get_lsr_info(struct moschip_port *mos7840_port,
				unsigned int __user *value)
{
	int count;
	unsigned int result = 0;

	count = mos7840_chars_in_buffer(mos7840_port->port);
	if (count == 0) {
		dbg("%s -- Empty", __FUNCTION__);
		result = TIOCSER_TEMT;
	}

	if (copy_to_user(value, &result, sizeof(int)))
		return -EFAULT;
	return 0;
}

/*****************************************************************************
 * mos7840_set_modem_info
 *      function to set modem info
 *****************************************************************************/

static int mos7840_set_modem_info(struct moschip_port *mos7840_port,
				  unsigned int cmd, unsigned int __user *value)
{
	unsigned int mcr;
	unsigned int arg;
	__u16 Data;
	int status;
	struct usb_serial_port *port;

	if (mos7840_port == NULL)
		return -1;

	port = (struct usb_serial_port *)mos7840_port->port;
	if (mos7840_port_paranoia_check(port, __FUNCTION__)) {
		dbg("%s", "Invalid port \n");
		return -1;
	}

	mcr = mos7840_port->shadowMCR;

	if (copy_from_user(&arg, value, sizeof(int)))
		return -EFAULT;

	switch (cmd) {
	case TIOCMBIS:
		if (arg & TIOCM_RTS)
			mcr |= MCR_RTS;
		if (arg & TIOCM_DTR)
			mcr |= MCR_RTS;
		if (arg & TIOCM_LOOP)
			mcr |= MCR_LOOPBACK;
		break;

	case TIOCMBIC:
		if (arg & TIOCM_RTS)
			mcr &= ~MCR_RTS;
		if (arg & TIOCM_DTR)
			mcr &= ~MCR_RTS;
		if (arg & TIOCM_LOOP)
			mcr &= ~MCR_LOOPBACK;
		break;

	case TIOCMSET:
		/* turn off the RTS and DTR and LOOPBACK
		 * and then only turn on what was asked to */
		mcr &= ~(MCR_RTS | MCR_DTR | MCR_LOOPBACK);
		mcr |= ((arg & TIOCM_RTS) ? MCR_RTS : 0);
		mcr |= ((arg & TIOCM_DTR) ? MCR_DTR : 0);
		mcr |= ((arg & TIOCM_LOOP) ? MCR_LOOPBACK : 0);
		break;
	}

	mos7840_port->shadowMCR = mcr;

	Data = mos7840_port->shadowMCR;
	status = 0;
	status = mos7840_set_uart_reg(port, MODEM_CONTROL_REGISTER, Data);
	if (status < 0) {
		dbg("setting MODEM_CONTROL_REGISTER Failed\n");
		return -1;
	}

	return 0;
}

/*****************************************************************************
 * mos7840_get_modem_info
 *      function to get modem info
 *****************************************************************************/

static int mos7840_get_modem_info(struct moschip_port *mos7840_port,
				  unsigned int __user *value)
{
	unsigned int result = 0;
	__u16 msr;
	unsigned int mcr = mos7840_port->shadowMCR;
	int status = 0;
	status =
	    mos7840_get_uart_reg(mos7840_port->port, MODEM_STATUS_REGISTER,
				 &msr);
	result = ((mcr & MCR_DTR) ? TIOCM_DTR : 0)	/* 0x002 */
	    |((mcr & MCR_RTS) ? TIOCM_RTS : 0)	/* 0x004 */
	    |((msr & MOS7840_MSR_CTS) ? TIOCM_CTS : 0)	/* 0x020 */
	    |((msr & MOS7840_MSR_CD) ? TIOCM_CAR : 0)	/* 0x040 */
	    |((msr & MOS7840_MSR_RI) ? TIOCM_RI : 0)	/* 0x080 */
	    |((msr & MOS7840_MSR_DSR) ? TIOCM_DSR : 0);	/* 0x100 */

	dbg("%s -- %x", __FUNCTION__, result);

	if (copy_to_user(value, &result, sizeof(int)))
		return -EFAULT;
	return 0;
}

/*****************************************************************************
 * mos7840_get_serial_info
 *      function to get information about serial port
 *****************************************************************************/

static int mos7840_get_serial_info(struct moschip_port *mos7840_port,
				   struct serial_struct __user *retinfo)
{
	struct serial_struct tmp;

	if (mos7840_port == NULL)
		return -1;

	if (!retinfo)
		return -EFAULT;

	memset(&tmp, 0, sizeof(tmp));

	tmp.type = PORT_16550A;
	tmp.line = mos7840_port->port->serial->minor;
	tmp.port = mos7840_port->port->number;
	tmp.irq = 0;
	tmp.flags = ASYNC_SKIP_TEST | ASYNC_AUTO_IRQ;
	tmp.xmit_fifo_size = NUM_URBS * URB_TRANSFER_BUFFER_SIZE;
	tmp.baud_base = 9600;
	tmp.close_delay = 5 * HZ;
	tmp.closing_wait = 30 * HZ;

	if (copy_to_user(retinfo, &tmp, sizeof(*retinfo)))
		return -EFAULT;
	return 0;
}

/*****************************************************************************
 * SerialIoctl
 *	this function handles any ioctl calls to the driver
 *****************************************************************************/

static int mos7840_ioctl(struct usb_serial_port *port, struct file *file,
			 unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;
	struct moschip_port *mos7840_port;
	struct tty_struct *tty;

	struct async_icount cnow;
	struct async_icount cprev;
	struct serial_icounter_struct icount;
	int mosret = 0;

	if (mos7840_port_paranoia_check(port, __FUNCTION__)) {
		dbg("%s", "Invalid port \n");
		return -1;
	}

	mos7840_port = mos7840_get_port_private(port);

	if (mos7840_port == NULL)
		return -1;

	tty = mos7840_port->port->tty;

	dbg("%s - port %d, cmd = 0x%x", __FUNCTION__, port->number, cmd);

	switch (cmd) {
		/* return number of bytes available */

	case TIOCSERGETLSR:
		dbg("%s (%d) TIOCSERGETLSR", __FUNCTION__, port->number);
		return mos7840_get_lsr_info(mos7840_port, argp);
		return 0;

	case TIOCMBIS:
	case TIOCMBIC:
	case TIOCMSET:
		dbg("%s (%d) TIOCMSET/TIOCMBIC/TIOCMSET", __FUNCTION__,
		    port->number);
		mosret =
		    mos7840_set_modem_info(mos7840_port, cmd, argp);
		return mosret;

	case TIOCMGET:
		dbg("%s (%d) TIOCMGET", __FUNCTION__, port->number);
		return mos7840_get_modem_info(mos7840_port, argp);

	case TIOCGSERIAL:
		dbg("%s (%d) TIOCGSERIAL", __FUNCTION__, port->number);
		return mos7840_get_serial_info(mos7840_port, argp);

	case TIOCSSERIAL:
		dbg("%s (%d) TIOCSSERIAL", __FUNCTION__, port->number);
		break;

	case TIOCMIWAIT:
		dbg("%s (%d) TIOCMIWAIT", __FUNCTION__, port->number);
		cprev = mos7840_port->icount;
		while (1) {
			//interruptible_sleep_on(&mos7840_port->delta_msr_wait);
			mos7840_port->delta_msr_cond = 0;
			wait_event_interruptible(mos7840_port->delta_msr_wait,
						 (mos7840_port->
						  delta_msr_cond == 1));

			/* see if a signal did it */
			if (signal_pending(current))
				return -ERESTARTSYS;
			cnow = mos7840_port->icount;
			smp_rmb();
			if (cnow.rng == cprev.rng && cnow.dsr == cprev.dsr &&
			    cnow.dcd == cprev.dcd && cnow.cts == cprev.cts)
				return -EIO;	/* no change => error */
			if (((arg & TIOCM_RNG) && (cnow.rng != cprev.rng)) ||
			    ((arg & TIOCM_DSR) && (cnow.dsr != cprev.dsr)) ||
			    ((arg & TIOCM_CD) && (cnow.dcd != cprev.dcd)) ||
			    ((arg & TIOCM_CTS) && (cnow.cts != cprev.cts))) {
				return 0;
			}
			cprev = cnow;
		}
		/* NOTREACHED */
		break;

	case TIOCGICOUNT:
		cnow = mos7840_port->icount;
		smp_rmb();
		icount.cts = cnow.cts;
		icount.dsr = cnow.dsr;
		icount.rng = cnow.rng;
		icount.dcd = cnow.dcd;
		icount.rx = cnow.rx;
		icount.tx = cnow.tx;
		icount.frame = cnow.frame;
		icount.overrun = cnow.overrun;
		icount.parity = cnow.parity;
		icount.brk = cnow.brk;
		icount.buf_overrun = cnow.buf_overrun;

		dbg("%s (%d) TIOCGICOUNT RX=%d, TX=%d", __FUNCTION__,
		    port->number, icount.rx, icount.tx);
		if (copy_to_user(argp, &icount, sizeof(icount)))
			return -EFAULT;
		return 0;

	case TIOCEXBAUD:
		return 0;
	default:
		break;
	}

	return -ENOIOCTLCMD;
}

static int mos7840_calc_num_ports(struct usb_serial *serial)
{
	int mos7840_num_ports = 0;

	dbg("numberofendpoints: %d \n",
	    (int)serial->interface->cur_altsetting->desc.bNumEndpoints);
	dbg("numberofendpoints: %d \n",
	    (int)serial->interface->altsetting->desc.bNumEndpoints);
	if (serial->interface->cur_altsetting->desc.bNumEndpoints == 5) {
		mos7840_num_ports = serial->num_ports = 2;
	} else if (serial->interface->cur_altsetting->desc.bNumEndpoints == 9) {
		serial->num_bulk_in = 4;
		serial->num_bulk_out = 4;
		mos7840_num_ports = serial->num_ports = 4;
	}

	return mos7840_num_ports;
}

/****************************************************************************
 * mos7840_startup
 ****************************************************************************/

static int mos7840_startup(struct usb_serial *serial)
{
	struct moschip_port *mos7840_port;
	struct usb_device *dev;
	int i, status;

	__u16 Data;
	dbg("%s \n", " mos7840_startup :entering..........");

	if (!serial) {
		dbg("%s\n", "Invalid Handler");
		return -1;
	}

	dev = serial->dev;

	dbg("%s\n", "Entering...");

	/* we set up the pointers to the endpoints in the mos7840_open *
	 * function, as the structures aren't created yet.             */

	/* set up port private structures */
	for (i = 0; i < serial->num_ports; ++i) {
		mos7840_port = kzalloc(sizeof(struct moschip_port), GFP_KERNEL);
		if (mos7840_port == NULL) {
			err("%s - Out of memory", __FUNCTION__);
			status = -ENOMEM;
			i--; /* don't follow NULL pointer cleaning up */
			goto error;
		}

		/* Initialize all port interrupt end point to port 0 int endpoint *
		 * Our device has only one interrupt end point comman to all port */

		mos7840_port->port = serial->port[i];
		mos7840_set_port_private(serial->port[i], mos7840_port);
		spin_lock_init(&mos7840_port->pool_lock);

		mos7840_port->port_num = ((serial->port[i]->number -
					   (serial->port[i]->serial->minor)) +
					  1);

		if (mos7840_port->port_num == 1) {
			mos7840_port->SpRegOffset = 0x0;
			mos7840_port->ControlRegOffset = 0x1;
			mos7840_port->DcrRegOffset = 0x4;
		} else if ((mos7840_port->port_num == 2)
			   && (serial->num_ports == 4)) {
			mos7840_port->SpRegOffset = 0x8;
			mos7840_port->ControlRegOffset = 0x9;
			mos7840_port->DcrRegOffset = 0x16;
		} else if ((mos7840_port->port_num == 2)
			   && (serial->num_ports == 2)) {
			mos7840_port->SpRegOffset = 0xa;
			mos7840_port->ControlRegOffset = 0xb;
			mos7840_port->DcrRegOffset = 0x19;
		} else if ((mos7840_port->port_num == 3)
			   && (serial->num_ports == 4)) {
			mos7840_port->SpRegOffset = 0xa;
			mos7840_port->ControlRegOffset = 0xb;
			mos7840_port->DcrRegOffset = 0x19;
		} else if ((mos7840_port->port_num == 4)
			   && (serial->num_ports == 4)) {
			mos7840_port->SpRegOffset = 0xc;
			mos7840_port->ControlRegOffset = 0xd;
			mos7840_port->DcrRegOffset = 0x1c;
		}
		mos7840_dump_serial_port(mos7840_port);

		mos7840_set_port_private(serial->port[i], mos7840_port);

		//enable rx_disable bit in control register

		status =
		    mos7840_get_reg_sync(serial->port[i],
					 mos7840_port->ControlRegOffset, &Data);
		if (status < 0) {
			dbg("Reading ControlReg failed status-0x%x\n", status);
			break;
		} else
			dbg("ControlReg Reading success val is %x, status%d\n",
			    Data, status);
		Data |= 0x08;	//setting driver done bit
		Data |= 0x04;	//sp1_bit to have cts change reflect in modem status reg

		//Data |= 0x20; //rx_disable bit
		status = 0;
		status =
		    mos7840_set_reg_sync(serial->port[i],
					 mos7840_port->ControlRegOffset, Data);
		if (status < 0) {
			dbg("Writing ControlReg failed(rx_disable) status-0x%x\n", status);
			break;
		} else
			dbg("ControlReg Writing success(rx_disable) status%d\n",
			    status);

		//Write default values in DCR (i.e 0x01 in DCR0, 0x05 in DCR2 and 0x24 in DCR3
		Data = 0x01;
		status = 0;
		status =
		    mos7840_set_reg_sync(serial->port[i],
					 (__u16) (mos7840_port->DcrRegOffset +
						  0), Data);
		if (status < 0) {
			dbg("Writing DCR0 failed status-0x%x\n", status);
			break;
		} else
			dbg("DCR0 Writing success status%d\n", status);

		Data = 0x05;
		status = 0;
		status =
		    mos7840_set_reg_sync(serial->port[i],
					 (__u16) (mos7840_port->DcrRegOffset +
						  1), Data);
		if (status < 0) {
			dbg("Writing DCR1 failed status-0x%x\n", status);
			break;
		} else
			dbg("DCR1 Writing success status%d\n", status);

		Data = 0x24;
		status = 0;
		status =
		    mos7840_set_reg_sync(serial->port[i],
					 (__u16) (mos7840_port->DcrRegOffset +
						  2), Data);
		if (status < 0) {
			dbg("Writing DCR2 failed status-0x%x\n", status);
			break;
		} else
			dbg("DCR2 Writing success status%d\n", status);

		// write values in clkstart0x0 and clkmulti 0x20
		Data = 0x0;
		status = 0;
		status =
		    mos7840_set_reg_sync(serial->port[i],
					 CLK_START_VALUE_REGISTER, Data);
		if (status < 0) {
			dbg("Writing CLK_START_VALUE_REGISTER failed status-0x%x\n", status);
			break;
		} else
			dbg("CLK_START_VALUE_REGISTER Writing success status%d\n", status);

		Data = 0x20;
		status =
		    mos7840_set_reg_sync(serial->port[i], CLK_MULTI_REGISTER,
					 Data);
		if (status < 0) {
			dbg("Writing CLK_MULTI_REGISTER failed status-0x%x\n",
			    status);
			goto error;
		} else
			dbg("CLK_MULTI_REGISTER Writing success status%d\n",
			    status);

		//write value 0x0 to scratchpad register
		Data = 0x00;
		status =
		    mos7840_set_uart_reg(serial->port[i], SCRATCH_PAD_REGISTER,
					 Data);
		if (status < 0) {
			dbg("Writing SCRATCH_PAD_REGISTER failed status-0x%x\n",
			    status);
			break;
		} else
			dbg("SCRATCH_PAD_REGISTER Writing success status%d\n",
			    status);

		//Zero Length flag register
		if ((mos7840_port->port_num != 1)
		    && (serial->num_ports == 2)) {

			Data = 0xff;
			status = 0;
			status = mos7840_set_reg_sync(serial->port[i],
						      (__u16) (ZLP_REG1 +
							       ((__u16)
								mos7840_port->
								port_num)),
						      Data);
			dbg("ZLIP offset%x\n",
			    (__u16) (ZLP_REG1 +
				     ((__u16) mos7840_port->port_num)));
			if (status < 0) {
				dbg("Writing ZLP_REG%d failed status-0x%x\n",
				    i + 2, status);
				break;
			} else
				dbg("ZLP_REG%d Writing success status%d\n",
				    i + 2, status);
		} else {
			Data = 0xff;
			status = 0;
			status = mos7840_set_reg_sync(serial->port[i],
						      (__u16) (ZLP_REG1 +
							       ((__u16)
								mos7840_port->
								port_num) -
							       0x1), Data);
			dbg("ZLIP offset%x\n",
			    (__u16) (ZLP_REG1 +
				     ((__u16) mos7840_port->port_num) - 0x1));
			if (status < 0) {
				dbg("Writing ZLP_REG%d failed status-0x%x\n",
				    i + 1, status);
				break;
			} else
				dbg("ZLP_REG%d Writing success status%d\n",
				    i + 1, status);

		}
		mos7840_port->control_urb = usb_alloc_urb(0, GFP_KERNEL);
		mos7840_port->ctrl_buf = kmalloc(16, GFP_KERNEL);
		mos7840_port->dr = kmalloc(sizeof(struct usb_ctrlrequest), GFP_KERNEL);
		if (!mos7840_port->control_urb || !mos7840_port->ctrl_buf || !mos7840_port->dr) {
			status = -ENOMEM;
			goto error;
		}
	}

	//Zero Length flag enable
	Data = 0x0f;
	status = mos7840_set_reg_sync(serial->port[0], ZLP_REG5, Data);
	if (status < 0) {
		dbg("Writing ZLP_REG5 failed status-0x%x\n", status);
		return -1;
	} else
		dbg("ZLP_REG5 Writing success status%d\n", status);

	/* setting configuration feature to one */
	usb_control_msg(serial->dev, usb_sndctrlpipe(serial->dev, 0),
			(__u8) 0x03, 0x00, 0x01, 0x00, NULL, 0x00, 5 * HZ);
	return 0;
error:
	for (/* nothing */; i >= 0; i--) {
		mos7840_port = mos7840_get_port_private(serial->port[i]);

		kfree(mos7840_port->dr);
		kfree(mos7840_port->ctrl_buf);
		usb_free_urb(mos7840_port->control_urb);
		kfree(mos7840_port);
		serial->port[i] = NULL;
	}
	return status;
}

/****************************************************************************
 * mos7840_shutdown
 *	This function is called whenever the device is removed from the usb bus.
 ****************************************************************************/

static void mos7840_shutdown(struct usb_serial *serial)
{
	int i;
	unsigned long flags;
	struct moschip_port *mos7840_port;
	dbg("%s \n", " shutdown :entering..........");

	if (!serial) {
		dbg("%s", "Invalid Handler \n");
		return;
	}

	/*      check for the ports to be closed,close the ports and disconnect         */

	/* free private structure allocated for serial port  *
	 * stop reads and writes on all ports                */

	for (i = 0; i < serial->num_ports; ++i) {
		mos7840_port = mos7840_get_port_private(serial->port[i]);
		spin_lock_irqsave(&mos7840_port->pool_lock, flags);
		mos7840_port->zombie = 1;
		spin_unlock_irqrestore(&mos7840_port->pool_lock, flags);
		usb_kill_urb(mos7840_port->control_urb);
		kfree(mos7840_port->ctrl_buf);
		kfree(mos7840_port->dr);
		kfree(mos7840_port);
		mos7840_set_port_private(serial->port[i], NULL);
	}

	dbg("%s\n", "Thank u :: ");

}

static struct usb_driver io_driver = {
	.name = "mos7840",
	.probe = usb_serial_probe,
	.disconnect = usb_serial_disconnect,
	.id_table = moschip_id_table_combined,
	.no_dynamic_id = 1,
};

static struct usb_serial_driver moschip7840_4port_device = {
	.driver = {
		   .owner = THIS_MODULE,
		   .name = "mos7840",
		   },
	.description = DRIVER_DESC,
	.usb_driver = &io_driver,
	.id_table = moschip_port_id_table,
	.num_interrupt_in = 1,	//NUM_DONT_CARE,//1,
#ifdef check
	.num_bulk_in = 4,
	.num_bulk_out = 4,
	.num_ports = 4,
#endif
	.open = mos7840_open,
	.close = mos7840_close,
	.write = mos7840_write,
	.write_room = mos7840_write_room,
	.chars_in_buffer = mos7840_chars_in_buffer,
	.throttle = mos7840_throttle,
	.unthrottle = mos7840_unthrottle,
	.calc_num_ports = mos7840_calc_num_ports,
#ifdef MCSSerialProbe
	.probe = mos7840_serial_probe,
#endif
	.ioctl = mos7840_ioctl,
	.set_termios = mos7840_set_termios,
	.break_ctl = mos7840_break,
	.tiocmget = mos7840_tiocmget,
	.tiocmset = mos7840_tiocmset,
	.attach = mos7840_startup,
	.shutdown = mos7840_shutdown,
	.read_bulk_callback = mos7840_bulk_in_callback,
	.read_int_callback = mos7840_interrupt_callback,
};

/****************************************************************************
 * moschip7840_init
 *	This is called by the module subsystem, or on startup to initialize us
 ****************************************************************************/
static int __init moschip7840_init(void)
{
	int retval;

	dbg("%s \n", " mos7840_init :entering..........");

	/* Register with the usb serial */
	retval = usb_serial_register(&moschip7840_4port_device);

	if (retval)
		goto failed_port_device_register;

	dbg("%s\n", "Entring...");
	info(DRIVER_DESC " " DRIVER_VERSION);

	/* Register with the usb */
	retval = usb_register(&io_driver);

	if (retval)
		goto failed_usb_register;

	if (retval == 0) {
		dbg("%s\n", "Leaving...");
		return 0;
	}

      failed_usb_register:
	usb_serial_deregister(&moschip7840_4port_device);

      failed_port_device_register:

	return retval;
}

/****************************************************************************
 * moschip7840_exit
 *	Called when the driver is about to be unloaded.
 ****************************************************************************/
static void __exit moschip7840_exit(void)
{

	dbg("%s \n", " mos7840_exit :entering..........");

	usb_deregister(&io_driver);

	usb_serial_deregister(&moschip7840_4port_device);

	dbg("%s\n", "Entring...");
}

module_init(moschip7840_init);
module_exit(moschip7840_exit);

/* Module information */
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

module_param(debug, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(debug, "Debug enabled or not");
