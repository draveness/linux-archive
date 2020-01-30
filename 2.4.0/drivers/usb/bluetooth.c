/*
 * bluetooth.c   Version 0.7
 *
 * Copyright (c) 2000 Greg Kroah-Hartman	<greg@kroah.com>
 * Copyright (c) 2000 Mark Douglas Corner	<mcorner@umich.edu>
 *
 * USB Bluetooth driver, based on the Bluetooth Spec version 1.0B
 *
 * (11/29/2000) Version 0.7 gkh
 *	Fixed problem with overrunning the tty flip buffer.
 *	Removed unneeded NULL pointer initialization.
 *
 * (10/05/2000) Version 0.6 gkh
 *	Fixed bug with urb->dev not being set properly, now that the usb
 *	core needs it.
 *	Got a real major id number and name.
 *
 * (08/06/2000) Version 0.5 gkh
 *	Fixed problem of not resubmitting the bulk read urb if there is
 *	an error in the callback.  Ericsson devices seem to need this.
 *
 * (07/11/2000) Version 0.4 gkh
 *	Fixed bug in disconnect for when we call tty_hangup
 *	Fixed bug in bluetooth_ctrl_msg where the bluetooth struct was not
 *	getting attached to the control urb properly.
 *	Fixed bug in bluetooth_write where we pay attention to the result
 *	of bluetooth_ctrl_msg.
 *
 * (08/03/2000) Version 0.3 gkh mdc
 *	Merged in Mark's changes to make the driver play nice with the Axis
 *	stack.
 *	Made the write bulk use an urb pool to enable larger transfers with
 *	fewer calls to the driver.
 *	Fixed off by one bug in acl pkt receive
 *	Made packet counters specific to each bluetooth device 
 *	Added checks for zero length callbacks
 *	Added buffers for int and bulk packets.  Had to do this otherwise 
 *	packet types could intermingle.
 *	Made a control urb pool for the control messages.
 *
 * (07/11/2000) Version 0.2 gkh
 *	Fixed a small bug found by Nils Faerber in the usb_bluetooth_probe 
 *	function.
 *
 * (07/09/2000) Version 0.1 gkh
 *	Initial release. Has support for sending ACL data (which is really just
 *	a HCI frame.) Raw HCI commands and HCI events are not supported.
 *	A ioctl will probably be needed for the HCI commands and events in the
 *	future. All isoch endpoints are ignored at this time also.
 *	This driver should work for all currently shipping USB Bluetooth 
 *	devices at this time :)
 * 
 */

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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */


#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/malloc.h>
#include <linux/fcntl.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/tty.h>
#include <linux/module.h>

#define DEBUG
#include <linux/usb.h>

/* Module information */
MODULE_AUTHOR("Greg Kroah-Hartman, Mark Douglas Corner");
MODULE_DESCRIPTION("USB Bluetooth driver");

/* define this if you have hardware that is not good */
/*#define	BTBUGGYHARDWARE */

/* Class, SubClass, and Protocol codes that describe a Bluetooth device */
#define WIRELESS_CLASS_CODE			0xe0
#define RF_SUBCLASS_CODE			0x01
#define BLUETOOTH_PROGRAMMING_PROTOCOL_CODE	0x01


#define BLUETOOTH_TTY_MAJOR	216	/* real device node major id */
#define BLUETOOTH_TTY_MINORS	256	/* whole lotta bluetooth devices */

#define USB_BLUETOOTH_MAGIC	0x6d02	/* magic number for bluetooth struct */

#define BLUETOOTH_CONTROL_REQUEST_TYPE	0x20

/* Bluetooth packet types */
#define CMD_PKT			0x01
#define ACL_PKT			0x02
#define SCO_PKT			0x03
#define EVENT_PKT		0x04
#define ERROR_PKT		0x05
#define NEG_PKT			0x06

/* Message sizes */
#define MAX_EVENT_SIZE		0xFF
#define EVENT_HDR_SIZE		3	/* 2 for the header + 1 for the type indicator */
#define EVENT_BUFFER_SIZE	(MAX_EVENT_SIZE + EVENT_HDR_SIZE)

#define MAX_ACL_SIZE		0xFFFF
#define ACL_HDR_SIZE		5	/* 4 for the header + 1 for the type indicator */
#define ACL_BUFFER_SIZE		(MAX_ACL_SIZE + ACL_HDR_SIZE)

/* parity check flag */
#define RELEVANT_IFLAG(iflag)	(iflag & (IGNBRK|BRKINT|IGNPAR|PARMRK|INPCK))

#define CHAR2INT16(c1,c0)	(((u32)((c1) & 0xff) << 8) + (u32)((c0) & 0xff))
#define MIN(a,b)		(((a)<(b))?(a):(b))

#define NUM_BULK_URBS		24
#define NUM_CONTROL_URBS	16

struct usb_bluetooth {
	int			magic;
	struct usb_device *	dev;
	struct tty_driver *	tty_driver;	/* the tty_driver for this device */
	struct tty_struct *	tty;		/* the coresponding tty for this port */

	unsigned char		minor;		/* the starting minor number for this device */
	char			active;		/* someone has this device open */
	int			throttle;	/* throttled by tty layer */
	
	__u8			control_out_bInterfaceNum;
	struct urb *		control_urb_pool[NUM_CONTROL_URBS];
	devrequest		dr[NUM_CONTROL_URBS];

	unsigned char *		interrupt_in_buffer;
	struct urb *		interrupt_in_urb;
	__u8			interrupt_in_endpointAddress;
	__u8			interrupt_in_interval;
	int			interrupt_in_buffer_size;

	unsigned char *		bulk_in_buffer;
	struct urb *		read_urb;
	__u8			bulk_in_endpointAddress;
	int			bulk_in_buffer_size;

	int			bulk_out_buffer_size;
	struct urb *		write_urb_pool[NUM_BULK_URBS];
	__u8			bulk_out_endpointAddress;

	wait_queue_head_t	write_wait;

	struct tq_struct	tqueue;		/* task queue for line discipline waking up */
	
	unsigned int		int_packet_pos;
	unsigned char		int_buffer[EVENT_BUFFER_SIZE];
	unsigned int		bulk_packet_pos;
	unsigned char		bulk_buffer[ACL_BUFFER_SIZE];	/* 64k preallocated, fix? */
};


/* local function prototypes */
static int  bluetooth_open		(struct tty_struct *tty, struct file *filp);
static void bluetooth_close		(struct tty_struct *tty, struct file *filp);
static int  bluetooth_write		(struct tty_struct *tty, int from_user, const unsigned char *buf, int count);
static int  bluetooth_write_room	(struct tty_struct *tty);
static int  bluetooth_chars_in_buffer	(struct tty_struct *tty);
static void bluetooth_throttle		(struct tty_struct *tty);
static void bluetooth_unthrottle	(struct tty_struct *tty);
static int  bluetooth_ioctl		(struct tty_struct *tty, struct file *file, unsigned int cmd, unsigned long arg);
static void bluetooth_set_termios	(struct tty_struct *tty, struct termios *old);

static void bluetooth_int_callback		(struct urb *urb);
static void bluetooth_ctrl_callback		(struct urb *urb);
static void bluetooth_read_bulk_callback	(struct urb *urb);
static void bluetooth_write_bulk_callback	(struct urb *urb);

static void * usb_bluetooth_probe(struct usb_device *dev, unsigned int ifnum,
			 	  const struct usb_device_id *id);
static void usb_bluetooth_disconnect	(struct usb_device *dev, void *ptr);


static struct usb_device_id usb_bluetooth_ids [] = {
	{ USB_DEVICE_INFO(WIRELESS_CLASS_CODE, RF_SUBCLASS_CODE, BLUETOOTH_PROGRAMMING_PROTOCOL_CODE) },
	{ }						/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, usb_bluetooth_ids);

static struct usb_driver usb_bluetooth_driver = {
	name:		"bluetooth",
	probe:		usb_bluetooth_probe,
	disconnect:	usb_bluetooth_disconnect,
	id_table:	usb_bluetooth_ids,
};

static int			bluetooth_refcount;
static struct tty_driver	bluetooth_tty_driver;
static struct tty_struct *	bluetooth_tty[BLUETOOTH_TTY_MINORS];
static struct termios *		bluetooth_termios[BLUETOOTH_TTY_MINORS];
static struct termios *		bluetooth_termios_locked[BLUETOOTH_TTY_MINORS];
static struct usb_bluetooth	*bluetooth_table[BLUETOOTH_TTY_MINORS];


static inline int bluetooth_paranoia_check (struct usb_bluetooth *bluetooth, const char *function)
{
	if (!bluetooth) {
		dbg("%s - bluetooth == NULL", function);
		return -1;
	}
	if (bluetooth->magic != USB_BLUETOOTH_MAGIC) {
		dbg("%s - bad magic number for bluetooth", function);
		return -1;
	}

	return 0;
}


static inline struct usb_bluetooth* get_usb_bluetooth (struct usb_bluetooth *bluetooth, const char *function)
{
	if (!bluetooth || 
	    bluetooth_paranoia_check (bluetooth, function)) { 
		/* then say that we dont have a valid usb_bluetooth thing, which will
		 * end up generating -ENODEV return values */
		return NULL;
	}

	return bluetooth;
}


static inline struct usb_bluetooth *get_bluetooth_by_minor (int minor)
{
	return bluetooth_table[minor];
}


static int bluetooth_ctrl_msg (struct usb_bluetooth *bluetooth, int request, int value, void *buf, int len)
{
	struct urb *urb = NULL;
	devrequest *dr = NULL;
	int i;
	int status;

	dbg (__FUNCTION__);

	/* try to find a free urb in our list */
	for (i = 0; i < NUM_CONTROL_URBS; ++i) {
		if (bluetooth->control_urb_pool[i]->status != -EINPROGRESS) {
			urb = bluetooth->control_urb_pool[i];
			dr = &bluetooth->dr[i];
			break;
		}
	}
	if (urb == NULL) {
		dbg (__FUNCTION__ " - no free urbs");
		return -ENOMEM;
	}

	/* free up the last buffer that this urb used */
	if (urb->transfer_buffer != NULL) {
		kfree(urb->transfer_buffer);
		urb->transfer_buffer = NULL;
	}

	dr->requesttype = BLUETOOTH_CONTROL_REQUEST_TYPE;
	dr->request = request;
	dr->value = cpu_to_le16p(&value);
	dr->index = cpu_to_le16p(&bluetooth->control_out_bInterfaceNum);
	dr->length = cpu_to_le16p(&len);
	
	FILL_CONTROL_URB (urb, bluetooth->dev, usb_sndctrlpipe(bluetooth->dev, 0),
			  (unsigned char*)dr, buf, len, bluetooth_ctrl_callback, bluetooth);

	/* send it down the pipe */
	status = usb_submit_urb(urb);
	if (status)
		dbg(__FUNCTION__ " - usb_submit_urb(control) failed with status = %d", status);
	
	return 0;
}





/*****************************************************************************
 * Driver tty interface functions
 *****************************************************************************/
static int bluetooth_open (struct tty_struct *tty, struct file * filp)
{
	struct usb_bluetooth *bluetooth;
	int result;

	dbg(__FUNCTION__);

	/* initialize the pointer incase something fails */
	tty->driver_data = NULL;

	/* get the bluetooth object associated with this tty pointer */
	bluetooth = get_bluetooth_by_minor (MINOR(tty->device));

	if (bluetooth_paranoia_check (bluetooth, __FUNCTION__)) {
		return -ENODEV;
	}

	if (bluetooth->active) {
		dbg (__FUNCTION__ " - device already open");
		return -EINVAL;
	}

	/* set up our structure making the tty driver remember our object, and us it */
	tty->driver_data = bluetooth;
	bluetooth->tty = tty;

	/* force low_latency on so that our tty_push actually forces the data through, 
	 * otherwise it is scheduled, and with high data rates (like with OHCI) data
	 * can get lost. */
	bluetooth->tty->low_latency = 1;
	
	bluetooth->active = 1;

	/* Reset the packet position counters */
	bluetooth->int_packet_pos = 0;
	bluetooth->bulk_packet_pos = 0;

#ifndef BTBUGGYHARDWARE
	/* Start reading from the device */
	FILL_BULK_URB(bluetooth->read_urb, bluetooth->dev, 
		      usb_rcvbulkpipe(bluetooth->dev, bluetooth->bulk_in_endpointAddress),
		      bluetooth->bulk_in_buffer, bluetooth->bulk_in_buffer_size, 
		      bluetooth_read_bulk_callback, bluetooth);
	result = usb_submit_urb(bluetooth->read_urb);
	if (result)
		dbg(__FUNCTION__ " - usb_submit_urb(read bulk) failed with status %d", result);
#endif
	FILL_INT_URB(bluetooth->interrupt_in_urb, bluetooth->dev, 
		     usb_rcvintpipe(bluetooth->dev, bluetooth->interrupt_in_endpointAddress),
		     bluetooth->interrupt_in_buffer, bluetooth->interrupt_in_buffer_size, 
		     bluetooth_int_callback, bluetooth, bluetooth->interrupt_in_interval);
	result = usb_submit_urb(bluetooth->interrupt_in_urb);
	if (result)
		dbg(__FUNCTION__ " - usb_submit_urb(interrupt in) failed with status %d", result);

	return 0;
}


static void bluetooth_close (struct tty_struct *tty, struct file * filp)
{
	struct usb_bluetooth *bluetooth = get_usb_bluetooth ((struct usb_bluetooth *)tty->driver_data, __FUNCTION__);
	int i;

	if (!bluetooth) {
		return;
	}

	dbg(__FUNCTION__);

	if (!bluetooth->active) {
		dbg (__FUNCTION__ " - device not opened");
		return;
	}

	/* shutdown any bulk reads and writes that might be going on */
	for (i = 0; i < NUM_BULK_URBS; ++i)
		usb_unlink_urb (bluetooth->write_urb_pool[i]);
	usb_unlink_urb (bluetooth->read_urb);

	bluetooth->active = 0;
}


static int bluetooth_write (struct tty_struct * tty, int from_user, const unsigned char *buf, int count)
{
	struct usb_bluetooth *bluetooth = get_usb_bluetooth ((struct usb_bluetooth *)tty->driver_data, __FUNCTION__);
	struct urb *urb = NULL;
	unsigned char *new_buffer;
	const unsigned char *current_position;
	int status;
	int bytes_sent;
	int buffer_size;
	int i;

	if (!bluetooth) {
		return -ENODEV;
	}

	dbg(__FUNCTION__ " - %d byte(s)", count);

	if (!bluetooth->active) {
		dbg (__FUNCTION__ " - device not opened");
		return -EINVAL;
	}

	if (count == 0) {
		dbg(__FUNCTION__ " - write request of 0 bytes");
		return 0;
	}
	if (count == 1) {
		dbg(__FUNCTION__ " - write request only included type %d", buf[0]);
		return 1;
	}

#ifdef DEBUG
	printk (KERN_DEBUG __FILE__ ": " __FUNCTION__ " - length = %d, data = ", count);
	for (i = 0; i < count; ++i) {
		printk ("%.2x ", buf[i]);
	}
	printk ("\n");
#endif

	switch (*buf) {
		/* First byte indicates the type of packet */
		case CMD_PKT:
			/* dbg(__FUNCTION__ "- Send cmd_pkt len:%d", count);*/

			if (in_interrupt()){
				printk("cmd_pkt from interrupt!\n");
				return count;
			}

			new_buffer = kmalloc (count-1, GFP_KERNEL);

			if (!new_buffer) {
				err (__FUNCTION__ "- out of memory.");
				return -ENOMEM;
			}

			if (from_user)
				copy_from_user (new_buffer, buf+1, count-1);
			else
				memcpy (new_buffer, buf+1, count-1);

			if (bluetooth_ctrl_msg (bluetooth, 0x00, 0x00, new_buffer, count-1) != 0) {
				kfree (new_buffer);
				return 0;
			}

			/* need to free new_buffer somehow... FIXME */
			return count;

		case ACL_PKT:
			current_position = buf;
			++current_position;
			--count;
			bytes_sent = 0;

			while (count > 0) {
				urb = NULL;

				/* try to find a free urb in our list */
				for (i = 0; i < NUM_BULK_URBS; ++i) {
					if (bluetooth->write_urb_pool[i]->status != -EINPROGRESS) {
						urb = bluetooth->write_urb_pool[i];
						break;
					}
				}
				if (urb == NULL) {
					dbg (__FUNCTION__ " - no free urbs");
					return bytes_sent;
				}
				
				/* free up the last buffer that this urb used */
				if (urb->transfer_buffer != NULL) {
					kfree(urb->transfer_buffer);
					urb->transfer_buffer = NULL;
				}

				buffer_size = MIN (count, bluetooth->bulk_out_buffer_size);
				
				new_buffer = kmalloc (buffer_size, GFP_KERNEL);
				if (new_buffer == NULL) {
					err(__FUNCTION__" no more kernel memory...");
					return bytes_sent;
				}

				if (from_user)
					copy_from_user(new_buffer, current_position, buffer_size);
				else
					memcpy (new_buffer, current_position, buffer_size);

				/* build up our urb */
				FILL_BULK_URB (urb, bluetooth->dev, usb_sndbulkpipe(bluetooth->dev, bluetooth->bulk_out_endpointAddress),
						new_buffer, buffer_size, bluetooth_write_bulk_callback, bluetooth);
				urb->transfer_flags |= USB_QUEUE_BULK;

				/* send it down the pipe */
				status = usb_submit_urb(urb);
				if (status)
					dbg(__FUNCTION__ " - usb_submit_urb(write bulk) failed with status = %d", status);
#ifdef BTBUGGYHARDWARE
				/* A workaround for the stalled data bug */
				/* May or may not be needed...*/
				if (count != 0) {
					udelay(500);
				}
#endif
				current_position += buffer_size;
				bytes_sent += buffer_size;
				count -= buffer_size;
			}

			return bytes_sent + 1;
		
		default :
			dbg(__FUNCTION__" - unsupported (at this time) write type");
	}

	return 0;
} 


static int bluetooth_write_room (struct tty_struct *tty) 
{
	struct usb_bluetooth *bluetooth = get_usb_bluetooth ((struct usb_bluetooth *)tty->driver_data, __FUNCTION__);
	int room = 0;
	int i;

	if (!bluetooth) {
		return -ENODEV;
	}

	dbg(__FUNCTION__);

	if (!bluetooth->active) {
		dbg (__FUNCTION__ " - device not open");
		return -EINVAL;
	}

	for (i = 0; i < NUM_BULK_URBS; ++i) {
		if (bluetooth->write_urb_pool[i]->status != -EINPROGRESS) {
			room += bluetooth->bulk_out_buffer_size;
		}
	}

	dbg(__FUNCTION__ " - returns %d", room);
	return room;
}


static int bluetooth_chars_in_buffer (struct tty_struct *tty) 
{
	struct usb_bluetooth *bluetooth = get_usb_bluetooth ((struct usb_bluetooth *)tty->driver_data, __FUNCTION__);
	int chars = 0;
	int i;

	if (!bluetooth) {
		return -ENODEV;
	}

	if (!bluetooth->active) {
		dbg (__FUNCTION__ " - device not open");
		return -EINVAL;
	}

	for (i = 0; i < NUM_BULK_URBS; ++i) {
		if (bluetooth->write_urb_pool[i]->status == -EINPROGRESS) {
			chars += bluetooth->write_urb_pool[i]->transfer_buffer_length;
		}
	}

	dbg (__FUNCTION__ " - returns %d", chars);
	return chars;
}


static void bluetooth_throttle (struct tty_struct * tty)
{
	struct usb_bluetooth *bluetooth = get_usb_bluetooth ((struct usb_bluetooth *)tty->driver_data, __FUNCTION__);

	if (!bluetooth) {
		return;
	}

	dbg(__FUNCTION__);

	if (!bluetooth->active) {
		dbg (__FUNCTION__ " - device not open");
		return;
	}
	
	dbg(__FUNCTION__ " unsupported (at this time)");

	return;
}


static void bluetooth_unthrottle (struct tty_struct * tty)
{
	struct usb_bluetooth *bluetooth = get_usb_bluetooth ((struct usb_bluetooth *)tty->driver_data, __FUNCTION__);

	if (!bluetooth) {
		return;
	}

	dbg(__FUNCTION__);

	if (!bluetooth->active) {
		dbg (__FUNCTION__ " - device not open");
		return;
	}

	dbg(__FUNCTION__ " unsupported (at this time)");
}


static int bluetooth_ioctl (struct tty_struct *tty, struct file * file, unsigned int cmd, unsigned long arg)
{
	struct usb_bluetooth *bluetooth = get_usb_bluetooth ((struct usb_bluetooth *)tty->driver_data, __FUNCTION__);

	if (!bluetooth) {
		return -ENODEV;
	}

	dbg(__FUNCTION__ " - cmd 0x%.4x", cmd);

	if (!bluetooth->active) {
		dbg (__FUNCTION__ " - device not open");
		return -ENODEV;
	}

	/* FIXME!!! */
	return -ENOIOCTLCMD;
}


static void bluetooth_set_termios (struct tty_struct *tty, struct termios * old)
{
	struct usb_bluetooth *bluetooth = get_usb_bluetooth ((struct usb_bluetooth *)tty->driver_data, __FUNCTION__);

	if (!bluetooth) {
		return;
	}

	dbg(__FUNCTION__);

	if (!bluetooth->active) {
		dbg (__FUNCTION__ " - device not open");
		return;
	}

	/* FIXME!!! */

	return;
}


#ifdef BTBUGGYHARDWARE
void btusb_enable_bulk_read(struct tty_struct *tty){
	struct usb_bluetooth *bluetooth = get_usb_bluetooth ((struct usb_bluetooth *)tty->driver_data, __FUNCTION__);
	int result;

	if (!bluetooth) {
		return;
	}

	dbg(__FUNCTION__);

	if (!bluetooth->active) {
		dbg (__FUNCTION__ " - device not open");
		return;
	}

	if (bluetooth->read_urb) {
		FILL_BULK_URB(bluetooth->read_urb, bluetooth->dev, 
			      usb_rcvbulkpipe(bluetooth->dev, bluetooth->bulk_in_endpointAddress),
			      bluetooth->bulk_in_buffer, bluetooth->bulk_in_buffer_size, 
			      bluetooth_read_bulk_callback, bluetooth);
		result = usb_submit_urb(bluetooth->read_urb);
		if (result)
			err (__FUNCTION__ " - failed submitting read urb, error %d", result);
	}
}

void btusb_disable_bulk_read(struct tty_struct *tty){
	struct usb_bluetooth *bluetooth = get_usb_bluetooth ((struct usb_bluetooth *)tty->driver_data, __FUNCTION__);

	if (!bluetooth) {
		return;
	}

	dbg(__FUNCTION__);

	if (!bluetooth->active) {
		dbg (__FUNCTION__ " - device not open");
		return;
	}

	if ((bluetooth->read_urb) && (bluetooth->read_urb->actual_length))
		usb_unlink_urb(bluetooth->read_urb);
}
#endif


/*****************************************************************************
 * urb callback functions
 *****************************************************************************/


static void bluetooth_int_callback (struct urb *urb)
{
	struct usb_bluetooth *bluetooth = get_usb_bluetooth ((struct usb_bluetooth *)urb->context, __FUNCTION__);
	unsigned char *data = urb->transfer_buffer;
	unsigned int i;
	unsigned int count = urb->actual_length;
	unsigned int packet_size;

	dbg(__FUNCTION__);

	if (!bluetooth) {
		dbg(__FUNCTION__ " - bad bluetooth pointer, exiting");
		return;
	}

	if (urb->status) {
		dbg(__FUNCTION__ " - nonzero int status received: %d", urb->status);
		return;
	}

	if (!count) {
		dbg(__FUNCTION__ " - zero length int");
		return;
	}


#ifdef DEBUG
	if (count) {
		printk (KERN_DEBUG __FILE__ ": " __FUNCTION__ "- length = %d, data = ", count);
		for (i = 0; i < count; ++i) {
			printk ("%.2x ", data[i]);
		}
		printk ("\n");
	}
#endif

#ifdef BTBUGGYHARDWARE
	if ((count >= 2) && (data[0] == 0xFF) && (data[1] == 0x00)) {
		data += 2;
		count -= 2;
	}
	if (count == 0) {
		urb->actual_length = 0;
		return;
	}
#endif
	/* We add  a packet type identifier to the beginning of each
	   HCI frame.  This makes the data in the tty look like a
	   serial USB devices.  Each HCI frame can be broken across
	   multiple URBs so we buffer them until we have a full hci
	   packet */

	if (!bluetooth->int_packet_pos) {
		bluetooth->int_buffer[0] = EVENT_PKT;
		bluetooth->int_packet_pos++;
	}
	
	if (bluetooth->int_packet_pos + count > EVENT_BUFFER_SIZE) {
		err(__FUNCTION__ " - exceeded EVENT_BUFFER_SIZE");
		bluetooth->int_packet_pos = 0;
		return;
	}

	memcpy (&bluetooth->int_buffer[bluetooth->int_packet_pos],
		urb->transfer_buffer, count);
	bluetooth->int_packet_pos += count;
	urb->actual_length = 0;

	if (bluetooth->int_packet_pos >= EVENT_HDR_SIZE)
		packet_size = bluetooth->int_buffer[2];
	else
		return;

	if (packet_size + EVENT_HDR_SIZE < bluetooth->int_packet_pos) {
		err(__FUNCTION__ " - packet was too long");
		bluetooth->int_packet_pos = 0;
		return;
	}

	if (packet_size + EVENT_HDR_SIZE == bluetooth->int_packet_pos) {
		for (i = 0; i < bluetooth->int_packet_pos; ++i) {
			/* if we insert more than TTY_FLIPBUF_SIZE characters, we drop them */
			if (bluetooth->tty->flip.count >= TTY_FLIPBUF_SIZE) {
				tty_flip_buffer_push(bluetooth->tty);
			}
			tty_insert_flip_char(bluetooth->tty, bluetooth->int_buffer[i], 0);
		}
		tty_flip_buffer_push(bluetooth->tty);

		bluetooth->int_packet_pos = 0;
	}
}


static void bluetooth_ctrl_callback (struct urb *urb)
{
	struct usb_bluetooth *bluetooth = get_usb_bluetooth ((struct usb_bluetooth *)urb->context, __FUNCTION__);

	dbg(__FUNCTION__);

	if (!bluetooth) {
		dbg(__FUNCTION__ " - bad bluetooth pointer, exiting");
		return;
	}

	if (urb->status) {
		dbg(__FUNCTION__ " - nonzero read bulk status received: %d", urb->status);
		return;
	}
}


static void bluetooth_read_bulk_callback (struct urb *urb)
{
	struct usb_bluetooth *bluetooth = get_usb_bluetooth ((struct usb_bluetooth *)urb->context, __FUNCTION__);
	unsigned char *data = urb->transfer_buffer;
	unsigned int count = urb->actual_length;
	unsigned int i;
	unsigned int packet_size;
	int result;

#ifdef BTBUGGYHARDWARE
	if ((count == 4) && (data[0] == 0x00) && (data[1] == 0x00)
	    && (data[2] == 0x00) && (data[3] == 0x00)) {
		urb->actual_length = 0;
		FILL_BULK_URB(bluetooth->read_urb, bluetooth->dev, 
			      usb_rcvbulkpipe(bluetooth->dev, bluetooth->bulk_in_endpointAddress),
			      bluetooth->bulk_in_buffer, bluetooth->bulk_in_buffer_size, 
			      bluetooth_read_bulk_callback, bluetooth);
		result = usb_submit_urb(bluetooth->read_urb);
		if (result)
			err (__FUNCTION__ " - failed resubmitting read urb, error %d", result);

		return;
	}
#endif

	dbg(__FUNCTION__);

	if (!bluetooth) {
		dbg(__FUNCTION__ " - bad bluetooth pointer, exiting");
		goto exit;
	}

	if (urb->status) {
		dbg(__FUNCTION__ " - nonzero read bulk status received: %d", urb->status);
		goto exit;
	}

	if (!count) {
		dbg(__FUNCTION__ " - zero length read bulk");
		goto exit;
	}

#ifdef DEBUG
	if (count) {
		printk (KERN_DEBUG __FILE__ ": " __FUNCTION__ "- length = %d, data = ", count);
		for (i = 0; i < count; ++i) {
			printk ("%.2x ", data[i]);
		}
		printk ("\n");
	}
#endif
	/* We add  a packet type identifier to the beginning of each
	   HCI frame.  This makes the data in the tty look like a
	   serial USB devices.  Each HCI frame can be broken across
	   multiple URBs so we buffer them until we have a full hci
	   packet */
	
	if (!bluetooth->bulk_packet_pos) {
		bluetooth->bulk_buffer[0] = ACL_PKT;
		bluetooth->bulk_packet_pos++;
	}

	if (bluetooth->bulk_packet_pos + count > ACL_BUFFER_SIZE) {
		err(__FUNCTION__ " - exceeded ACL_BUFFER_SIZE");
		bluetooth->bulk_packet_pos = 0;
		goto exit;
	}

	memcpy (&bluetooth->bulk_buffer[bluetooth->bulk_packet_pos],
		urb->transfer_buffer, count);
	bluetooth->bulk_packet_pos += count;
	urb->actual_length = 0;

	if (bluetooth->bulk_packet_pos >= ACL_HDR_SIZE) {
		packet_size = CHAR2INT16(bluetooth->bulk_buffer[4],bluetooth->bulk_buffer[3]);
	} else {
		goto exit;
	}

	if (packet_size + ACL_HDR_SIZE < bluetooth->bulk_packet_pos) {
		err(__FUNCTION__ " - packet was too long");
		bluetooth->bulk_packet_pos = 0;
		goto exit;
	}

	if (packet_size + ACL_HDR_SIZE == bluetooth->bulk_packet_pos) {
		for (i = 0; i < bluetooth->bulk_packet_pos; ++i) {
			/* if we insert more than TTY_FLIPBUF_SIZE characters, we drop them. */
			if (bluetooth->tty->flip.count >= TTY_FLIPBUF_SIZE) {
				tty_flip_buffer_push(bluetooth->tty);
			}
			tty_insert_flip_char(bluetooth->tty, bluetooth->bulk_buffer[i], 0);
		}
		tty_flip_buffer_push(bluetooth->tty);
		bluetooth->bulk_packet_pos = 0;
	}	

exit:
	FILL_BULK_URB(bluetooth->read_urb, bluetooth->dev, 
		      usb_rcvbulkpipe(bluetooth->dev, bluetooth->bulk_in_endpointAddress),
		      bluetooth->bulk_in_buffer, bluetooth->bulk_in_buffer_size, 
		      bluetooth_read_bulk_callback, bluetooth);
	result = usb_submit_urb(bluetooth->read_urb);
	if (result)
		err (__FUNCTION__ " - failed resubmitting read urb, error %d", result);

	return;
}


static void bluetooth_write_bulk_callback (struct urb *urb)
{
	struct usb_bluetooth *bluetooth = get_usb_bluetooth ((struct usb_bluetooth *)urb->context, __FUNCTION__);

	dbg(__FUNCTION__);

	if (!bluetooth) {
		dbg(__FUNCTION__ " - bad bluetooth pointer, exiting");
		return;
	}

	if (urb->status) {
		dbg(__FUNCTION__ " - nonzero write bulk status received: %d", urb->status);
		return;
	}

	/* wake up our little function to let the tty layer know that something happened */
	queue_task(&bluetooth->tqueue, &tq_immediate);
	mark_bh(IMMEDIATE_BH);
	return;
}


static void bluetooth_softint(void *private)
{
	struct usb_bluetooth *bluetooth = get_usb_bluetooth ((struct usb_bluetooth *)private, __FUNCTION__);
	struct tty_struct *tty;

	dbg(__FUNCTION__);

	if (!bluetooth) {
		return;
	}

	tty = bluetooth->tty;
	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) && tty->ldisc.write_wakeup) {
		dbg(__FUNCTION__ " - write wakeup call.");
		(tty->ldisc.write_wakeup)(tty);
	}

	wake_up_interruptible(&tty->write_wait);
}


static void * usb_bluetooth_probe(struct usb_device *dev, unsigned int ifnum,
			 	  const struct usb_device_id *id)
{
	struct usb_bluetooth *bluetooth = NULL;
	struct usb_interface_descriptor *interface;
	struct usb_endpoint_descriptor *endpoint;
	struct usb_endpoint_descriptor *interrupt_in_endpoint[8];
	struct usb_endpoint_descriptor *bulk_in_endpoint[8];
	struct usb_endpoint_descriptor *bulk_out_endpoint[8];
	int control_out_endpoint;

	int minor;
	int buffer_size;
	int i;
	int num_interrupt_in = 0;
	int num_bulk_in = 0;
	int num_bulk_out = 0;

	interface = &dev->actconfig->interface[ifnum].altsetting[0];
	control_out_endpoint = interface->bInterfaceNumber;

	/* find the endpoints that we need */
	for (i = 0; i < interface->bNumEndpoints; ++i) {
		endpoint = &interface->endpoint[i];

		if ((endpoint->bEndpointAddress & 0x80) &&
		    ((endpoint->bmAttributes & 3) == 0x02)) {
			/* we found a bulk in endpoint */
			dbg("found bulk in");
			bulk_in_endpoint[num_bulk_in] = endpoint;
			++num_bulk_in;
		}

		if (((endpoint->bEndpointAddress & 0x80) == 0x00) &&
		    ((endpoint->bmAttributes & 3) == 0x02)) {
			/* we found a bulk out endpoint */
			dbg("found bulk out");
			bulk_out_endpoint[num_bulk_out] = endpoint;
			++num_bulk_out;
		}

		if ((endpoint->bEndpointAddress & 0x80) &&
		    ((endpoint->bmAttributes & 3) == 0x03)) {
			/* we found a interrupt in endpoint */
			dbg("found interrupt in");
			interrupt_in_endpoint[num_interrupt_in] = endpoint;
			++num_interrupt_in;
		}
	}

	/* according to the spec, we can only have 1 bulk_in, 1 bulk_out, and 1 interrupt_in endpoints */
	if ((num_bulk_in != 1) ||
	    (num_bulk_out != 1) ||
	    (num_interrupt_in != 1)) {
		dbg (__FUNCTION__ " - improper number of endpoints. Bluetooth driver not bound.");
		return NULL;
	}

	MOD_INC_USE_COUNT;
	info("USB Bluetooth converter detected");

	for (minor = 0; minor < BLUETOOTH_TTY_MINORS && bluetooth_table[minor]; ++minor)
		;
	if (bluetooth_table[minor]) {
		err("No more free Bluetooth devices");
		MOD_DEC_USE_COUNT;
		return NULL;
	}

	if (!(bluetooth = kmalloc(sizeof(struct usb_bluetooth), GFP_KERNEL))) {
		err("Out of memory");
		MOD_DEC_USE_COUNT;
		return NULL;
	}

	memset(bluetooth, 0, sizeof(struct usb_bluetooth));

	bluetooth->magic = USB_BLUETOOTH_MAGIC;
	bluetooth->dev = dev;
	bluetooth->minor = minor;
	bluetooth->tqueue.routine = bluetooth_softint;
	bluetooth->tqueue.data = bluetooth;

	/* record the interface number for the control out */
	bluetooth->control_out_bInterfaceNum = control_out_endpoint;
	
	/* create our control out urb pool */ 
	for (i = 0; i < NUM_CONTROL_URBS; ++i) {
		struct urb  *urb = usb_alloc_urb(0);
		if (urb == NULL) {
			err("No free urbs available");
			goto probe_error;
		}
		urb->transfer_buffer = NULL;
		bluetooth->control_urb_pool[i] = urb;
	}

	/* set up the endpoint information */
	endpoint = bulk_in_endpoint[0];
	bluetooth->read_urb = usb_alloc_urb (0);
	if (!bluetooth->read_urb) {
		err("No free urbs available");
		goto probe_error;
	}
	bluetooth->bulk_in_buffer_size = buffer_size = endpoint->wMaxPacketSize;
	bluetooth->bulk_in_endpointAddress = endpoint->bEndpointAddress;
	bluetooth->bulk_in_buffer = kmalloc (buffer_size, GFP_KERNEL);
	if (!bluetooth->bulk_in_buffer) {
		err("Couldn't allocate bulk_in_buffer");
		goto probe_error;
	}
	FILL_BULK_URB(bluetooth->read_urb, dev, usb_rcvbulkpipe(dev, endpoint->bEndpointAddress),
		      bluetooth->bulk_in_buffer, buffer_size, bluetooth_read_bulk_callback, bluetooth);

	endpoint = bulk_out_endpoint[0];
	bluetooth->bulk_out_endpointAddress = endpoint->bEndpointAddress;
	
	/* create our write urb pool */ 
	for (i = 0; i < NUM_BULK_URBS; ++i) {
		struct urb  *urb = usb_alloc_urb(0);
		if (urb == NULL) {
			err("No free urbs available");
			goto probe_error;
		}
		urb->transfer_buffer = NULL;
		bluetooth->write_urb_pool[i] = urb;
	}
	
	bluetooth->bulk_out_buffer_size = endpoint->wMaxPacketSize * 2;

	endpoint = interrupt_in_endpoint[0];
	bluetooth->interrupt_in_urb = usb_alloc_urb(0);
	if (!bluetooth->interrupt_in_urb) {
		err("No free urbs available");
		goto probe_error;
	}
	bluetooth->interrupt_in_buffer_size = buffer_size = endpoint->wMaxPacketSize;
	bluetooth->interrupt_in_endpointAddress = endpoint->bEndpointAddress;
	bluetooth->interrupt_in_interval = endpoint->bInterval;
	bluetooth->interrupt_in_buffer = kmalloc (buffer_size, GFP_KERNEL);
	if (!bluetooth->interrupt_in_buffer) {
		err("Couldn't allocate interrupt_in_buffer");
		goto probe_error;
	}
	FILL_INT_URB(bluetooth->interrupt_in_urb, dev, usb_rcvintpipe(dev, endpoint->bEndpointAddress),
		     bluetooth->interrupt_in_buffer, buffer_size, bluetooth_int_callback,
		     bluetooth, endpoint->bInterval);

	/* initialize the devfs nodes for this device and let the user know what bluetooths we are bound to */
	tty_register_devfs (&bluetooth_tty_driver, 0, minor);
	info("Bluetooth converter now attached to ttyUB%d (or usb/ttub/%d for devfs)", minor, minor);

	bluetooth_table[minor] = bluetooth;

	return bluetooth; /* success */

probe_error:
	if (bluetooth->read_urb)
		usb_free_urb (bluetooth->read_urb);
	if (bluetooth->bulk_in_buffer)
		kfree (bluetooth->bulk_in_buffer);
	if (bluetooth->interrupt_in_urb)
		usb_free_urb (bluetooth->interrupt_in_urb);
	if (bluetooth->interrupt_in_buffer)
		kfree (bluetooth->interrupt_in_buffer);
	for (i = 0; i < NUM_BULK_URBS; ++i)
		if (bluetooth->write_urb_pool[i])
			usb_free_urb (bluetooth->write_urb_pool[i]);
	for (i = 0; i < NUM_CONTROL_URBS; ++i) 
		if (bluetooth->control_urb_pool[i])
			usb_free_urb (bluetooth->control_urb_pool[i]);

	bluetooth_table[minor] = NULL;

	/* free up any memory that we allocated */
	kfree (bluetooth);
	MOD_DEC_USE_COUNT;
	return NULL;
}


static void usb_bluetooth_disconnect(struct usb_device *dev, void *ptr)
{
	struct usb_bluetooth *bluetooth = (struct usb_bluetooth *) ptr;
	int i;

	if (bluetooth) {
		if ((bluetooth->active) && (bluetooth->tty))
			tty_hangup(bluetooth->tty);

		bluetooth->active = 0;

		if (bluetooth->read_urb) {
			usb_unlink_urb (bluetooth->read_urb);
			usb_free_urb (bluetooth->read_urb);
		}
		if (bluetooth->bulk_in_buffer)
			kfree (bluetooth->bulk_in_buffer);

		if (bluetooth->interrupt_in_urb) {
			usb_unlink_urb (bluetooth->interrupt_in_urb);
			usb_free_urb (bluetooth->interrupt_in_urb);
		}
		if (bluetooth->interrupt_in_buffer)
			kfree (bluetooth->interrupt_in_buffer);

		tty_unregister_devfs (&bluetooth_tty_driver, bluetooth->minor);

		for (i = 0; i < NUM_BULK_URBS; ++i) {
			if (bluetooth->write_urb_pool[i]) {
				usb_unlink_urb (bluetooth->write_urb_pool[i]);
				if (bluetooth->write_urb_pool[i]->transfer_buffer)
					kfree (bluetooth->write_urb_pool[i]->transfer_buffer);
				usb_free_urb (bluetooth->write_urb_pool[i]);
			}
		}
		for (i = 0; i < NUM_CONTROL_URBS; ++i) {
			if (bluetooth->control_urb_pool[i]) {
				usb_unlink_urb (bluetooth->control_urb_pool[i]);
				if (bluetooth->control_urb_pool[i]->transfer_buffer)
					kfree (bluetooth->control_urb_pool[i]->transfer_buffer);
				usb_free_urb (bluetooth->control_urb_pool[i]);
			}
		}
		
		info("Bluetooth converter now disconnected from ttyUB%d", bluetooth->minor);

		bluetooth_table[bluetooth->minor] = NULL;

		/* free up any memory that we allocated */
		kfree (bluetooth);

	} else {
		info("device disconnected");
	}

	MOD_DEC_USE_COUNT;
}


static struct tty_driver bluetooth_tty_driver = {
	magic:			TTY_DRIVER_MAGIC,
	driver_name:		"usb-bluetooth",
	name:			"usb/ttub/%d",
	major:			BLUETOOTH_TTY_MAJOR,
	minor_start:		0,
	num:			BLUETOOTH_TTY_MINORS,
	type:			TTY_DRIVER_TYPE_SERIAL,
	subtype:		SERIAL_TYPE_NORMAL,
	flags:			TTY_DRIVER_REAL_RAW | TTY_DRIVER_NO_DEVFS,

	refcount:		&bluetooth_refcount,
	table:			bluetooth_tty,
	termios:		bluetooth_termios,
	termios_locked:		bluetooth_termios_locked,

	open:			bluetooth_open,
	close:			bluetooth_close,
	write:			bluetooth_write,
	write_room:		bluetooth_write_room,
	ioctl:			bluetooth_ioctl,
	set_termios:		bluetooth_set_termios,
	throttle:		bluetooth_throttle,
	unthrottle:		bluetooth_unthrottle,
	chars_in_buffer:	bluetooth_chars_in_buffer,
};


int usb_bluetooth_init(void)
{
	int i;
	int result;

	/* Initalize our global data */
	for (i = 0; i < BLUETOOTH_TTY_MINORS; ++i) {
		bluetooth_table[i] = NULL;
	}

	info ("USB Bluetooth support registered");

	/* register the tty driver */
	bluetooth_tty_driver.init_termios          = tty_std_termios;
	bluetooth_tty_driver.init_termios.c_cflag  = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	if (tty_register_driver (&bluetooth_tty_driver)) {
		err(__FUNCTION__ " - failed to register tty driver");
		return -1;
	}

	/* register the USB driver */
	result = usb_register(&usb_bluetooth_driver);
	if (result < 0) {
		tty_unregister_driver(&bluetooth_tty_driver);
		err("usb_register failed for the USB bluetooth driver. Error number %d", result);
		return -1;
	}

	return 0;
}


void usb_bluetooth_exit(void)
{
	usb_deregister(&usb_bluetooth_driver);
	tty_unregister_driver(&bluetooth_tty_driver);
}


module_init(usb_bluetooth_init);
module_exit(usb_bluetooth_exit);


