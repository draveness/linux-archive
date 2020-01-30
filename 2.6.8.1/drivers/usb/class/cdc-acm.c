/*
 * cdc-acm.c
 *
 * Copyright (c) 1999 Armin Fuerst	<fuerst@in.tum.de>
 * Copyright (c) 1999 Pavel Machek	<pavel@suse.cz>
 * Copyright (c) 1999 Johannes Erdfelt	<johannes@erdfelt.com>
 * Copyright (c) 2000 Vojtech Pavlik	<vojtech@suse.cz>
 * Copyright (c) 2004 Oliver Neukum	<oliver@neukum.name>
 *
 * USB Abstract Control Model driver for USB modems and ISDN adapters
 *
 * Sponsored by SuSE
 *
 * ChangeLog:
 *	v0.9  - thorough cleaning, URBification, almost a rewrite
 *	v0.10 - some more cleanups
 *	v0.11 - fixed flow control, read error doesn't stop reads
 *	v0.12 - added TIOCM ioctls, added break handling, made struct acm kmalloced
 *	v0.13 - added termios, added hangup
 *	v0.14 - sized down struct acm
 *	v0.15 - fixed flow control again - characters could be lost
 *	v0.16 - added code for modems with swapped data and control interfaces
 *	v0.17 - added new style probing
 *	v0.18 - fixed new style probing for devices with more configurations
 *	v0.19 - fixed CLOCAL handling (thanks to Richard Shih-Ping Chan)
 *	v0.20 - switched to probing on interface (rather than device) class
 *	v0.21 - revert to probing on device for devices with multiple configs
 *	v0.22 - probe only the control interface. if usbcore doesn't choose the
 *		config we want, sysadmin changes bConfigurationValue in sysfs.
 *	v0.23 - use softirq for rx processing, as needed by tty layer
 *	v0.24 - change probe method to evaluate CDC union descriptor
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

#undef DEBUG

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/module.h>
#include <linux/smp_lock.h>
#include <asm/uaccess.h>
#include <linux/usb.h>
#include <asm/byteorder.h>
#include <asm/unaligned.h>

#include "cdc-acm.h"

/*
 * Version Information
 */
#define DRIVER_VERSION "v0.23"
#define DRIVER_AUTHOR "Armin Fuerst, Pavel Machek, Johannes Erdfelt, Vojtech Pavlik"
#define DRIVER_DESC "USB Abstract Control Model driver for USB modems and ISDN adapters"

static struct usb_driver acm_driver;
static struct tty_driver *acm_tty_driver;
static struct acm *acm_table[ACM_TTY_MINORS];

static DECLARE_MUTEX(open_sem);

#define ACM_READY(acm)	(acm && acm->dev && acm->used)

/*
 * Functions for ACM control messages.
 */

static int acm_ctrl_msg(struct acm *acm, int request, int value, void *buf, int len)
{
	int retval = usb_control_msg(acm->dev, usb_sndctrlpipe(acm->dev, 0),
		request, USB_RT_ACM, value,
		acm->control->altsetting[0].desc.bInterfaceNumber,
		buf, len, HZ * 5);
	dbg("acm_control_msg: rq: 0x%02x val: %#x len: %#x result: %d", request, value, len, retval);
	return retval < 0 ? retval : 0;
}

/* devices aren't required to support these requests.
 * the cdc acm descriptor tells whether they do...
 */
#define acm_set_control(acm, control)	acm_ctrl_msg(acm, ACM_REQ_SET_CONTROL, control, NULL, 0)
#define acm_set_line(acm, line)		acm_ctrl_msg(acm, ACM_REQ_SET_LINE, 0, line, sizeof(struct acm_line))
#define acm_send_break(acm, ms)		acm_ctrl_msg(acm, ACM_REQ_SEND_BREAK, ms, NULL, 0)

/*
 * Interrupt handlers for various ACM device responses
 */

/* control interface reports status changes with "interrupt" transfers */
static void acm_ctrl_irq(struct urb *urb, struct pt_regs *regs)
{
	struct acm *acm = urb->context;
	struct usb_ctrlrequest *dr = urb->transfer_buffer;
	unsigned char *data;
	int newctrl;
	int status;

	switch (urb->status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dbg("%s - urb shutting down with status: %d", __FUNCTION__, urb->status);
		return;
	default:
		dbg("%s - nonzero urb status received: %d", __FUNCTION__, urb->status);
		goto exit;
	}

	if (!ACM_READY(acm))
		goto exit;

	data = (unsigned char *)(dr + 1);
	switch (dr->bRequest) {

		case ACM_IRQ_NETWORK:

			dbg("%s network", dr->wValue ? "connected to" : "disconnected from");
			break;

		case ACM_IRQ_LINE_STATE:

			newctrl = le16_to_cpu(get_unaligned((__u16 *) data));

			if (acm->tty && !acm->clocal && (acm->ctrlin & ~newctrl & ACM_CTRL_DCD)) {
				dbg("calling hangup");
				tty_hangup(acm->tty);
			}

			acm->ctrlin = newctrl;

			dbg("input control lines: dcd%c dsr%c break%c ring%c framing%c parity%c overrun%c",
				acm->ctrlin & ACM_CTRL_DCD ? '+' : '-',	acm->ctrlin & ACM_CTRL_DSR ? '+' : '-',
				acm->ctrlin & ACM_CTRL_BRK ? '+' : '-',	acm->ctrlin & ACM_CTRL_RI  ? '+' : '-',
				acm->ctrlin & ACM_CTRL_FRAMING ? '+' : '-',	acm->ctrlin & ACM_CTRL_PARITY ? '+' : '-',
				acm->ctrlin & ACM_CTRL_OVERRUN ? '+' : '-');

			break;

		default:
			dbg("unknown control event received: request %d index %d len %d data0 %d data1 %d",
				dr->bRequest, dr->wIndex, dr->wLength, data[0], data[1]);
			break;
	}
exit:
	status = usb_submit_urb (urb, GFP_ATOMIC);
	if (status)
		err ("%s - usb_submit_urb failed with result %d",
		     __FUNCTION__, status);
}

/* data interface returns incoming bytes, or we got unthrottled */
static void acm_read_bulk(struct urb *urb, struct pt_regs *regs)
{
	struct acm *acm = urb->context;
	dbg("Entering acm_read_bulk with status %d\n", urb->status);

	if (!ACM_READY(acm))
		return;

	if (urb->status)
		dev_dbg(&acm->data->dev, "bulk rx status %d\n", urb->status);

	/* calling tty_flip_buffer_push() in_irq() isn't allowed */
	tasklet_schedule(&acm->bh);
}

static void acm_rx_tasklet(unsigned long _acm)
{
	struct acm *acm = (void *)_acm;
	struct urb *urb = acm->readurb;
	struct tty_struct *tty = acm->tty;
	unsigned char *data = urb->transfer_buffer;
	int i = 0;
	dbg("Entering acm_rx_tasklet");

	if (urb->actual_length > 0 && !acm->throttle)  {
		for (i = 0; i < urb->actual_length && !acm->throttle; i++) {
			/* if we insert more than TTY_FLIPBUF_SIZE characters,
			 * we drop them. */
			if (tty->flip.count >= TTY_FLIPBUF_SIZE) {
				tty_flip_buffer_push(tty);
			}
			tty_insert_flip_char(tty, data[i], 0);
		}
		dbg("Handed %d bytes to tty layer", i+1);
		tty_flip_buffer_push(tty);
	}

	spin_lock(&acm->throttle_lock);
	if (acm->throttle) {
		dbg("Throtteling noticed");
		memmove(data, data + i, urb->actual_length - i);
		urb->actual_length -= i;
		acm->resubmit_to_unthrottle = 1;
		spin_unlock(&acm->throttle_lock);
		return;
	}
	spin_unlock(&acm->throttle_lock);

	urb->actual_length = 0;
	urb->dev = acm->dev;

	i = usb_submit_urb(urb, GFP_ATOMIC);
	if (i)
		dev_dbg(&acm->data->dev, "bulk rx resubmit %d\n", i);
}

/* data interface wrote those outgoing bytes */
static void acm_write_bulk(struct urb *urb, struct pt_regs *regs)
{
	struct acm *acm = (struct acm *)urb->context;
	dbg("Entering acm_write_bulk with status %d\n", urb->status);

	if (!ACM_READY(acm))
		goto out;

	if (urb->status)
		dbg("nonzero write bulk status received: %d", urb->status);

	schedule_work(&acm->work);
out:
	acm->ready_for_write = 1;
}

static void acm_softint(void *private)
{
	struct acm *acm = private;
	struct tty_struct *tty = acm->tty;
	dbg("Entering acm_softint.\n");
	
	if (!ACM_READY(acm))
		return;

	if ((tty->flags & (1 << TTY_DO_WRITE_WAKEUP)) && tty->ldisc.write_wakeup)
		(tty->ldisc.write_wakeup)(tty);

	wake_up_interruptible(&tty->write_wait);
}

/*
 * TTY handlers
 */

static int acm_tty_open(struct tty_struct *tty, struct file *filp)
{
	struct acm *acm = acm_table[tty->index];
	dbg("Entering acm_tty_open.\n");

	if (!acm || !acm->dev)
		return -EINVAL;

	tty->driver_data = acm;
	acm->tty = tty;

        down(&open_sem);

	if (acm->used) {
		goto done;
        }

	acm->ctrlurb->dev = acm->dev;
	if (usb_submit_urb(acm->ctrlurb, GFP_KERNEL)) {
		dbg("usb_submit_urb(ctrl irq) failed");
		goto bail_out;
	}

	acm->readurb->dev = acm->dev;
	if (usb_submit_urb(acm->readurb, GFP_KERNEL)) {
		dbg("usb_submit_urb(read bulk) failed");
		goto bail_out_and_unlink;
	}

	if (0 > acm_set_control(acm, acm->ctrlout = ACM_CTRL_DTR | ACM_CTRL_RTS))
		goto full_bailout;

	/* force low_latency on so that our tty_push actually forces the data through, 
	   otherwise it is scheduled, and with high data rates data can get lost. */
	tty->low_latency = 1;

done:
	acm->used++;
	up(&open_sem);
	return 0;

full_bailout:
	usb_unlink_urb(acm->readurb);
bail_out_and_unlink:
	usb_unlink_urb(acm->ctrlurb);
bail_out:
	up(&open_sem);
	return -EIO;
}

static void acm_tty_close(struct tty_struct *tty, struct file *filp)
{
	struct acm *acm = tty->driver_data;

	if (!acm || !acm->used)
		return;

	down(&open_sem);
	if (!--acm->used) {
		if (acm->dev) {
			acm_set_control(acm, acm->ctrlout = 0);
			usb_unlink_urb(acm->ctrlurb);
			usb_unlink_urb(acm->writeurb);
			usb_unlink_urb(acm->readurb);
		} else {
			tty_unregister_device(acm_tty_driver, acm->minor);
			acm_table[acm->minor] = NULL;
			usb_free_urb(acm->ctrlurb);
			usb_free_urb(acm->readurb);
			usb_free_urb(acm->writeurb);
			kfree(acm);
		}
	}
	up(&open_sem);
}

static int acm_tty_write(struct tty_struct *tty, int from_user, const unsigned char *buf, int count)
{
	struct acm *acm = tty->driver_data;
	int stat;
	dbg("Entering acm_tty_write to write %d bytes from %s space,\n", count, from_user ? "user" : "kernel");

	if (!ACM_READY(acm))
		return -EINVAL;
	if (!acm->ready_for_write)
		return 0;
	if (!count)
		return 0;

	count = (count > acm->writesize) ? acm->writesize : count;

	dbg("Get %d bytes from %s space...", count, from_user ? "user" : "kernel");
	if (from_user) {
		if (copy_from_user(acm->write_buffer, (void __user *)buf, count))
			return -EFAULT;
	} else
		memcpy(acm->write_buffer, buf, count);
	dbg("  Successfully copied.\n");

	acm->writeurb->transfer_buffer_length = count;
	acm->writeurb->dev = acm->dev;

	acm->ready_for_write = 0;
	stat = usb_submit_urb(acm->writeurb, GFP_NOIO);
	if (stat < 0) {
		dbg("usb_submit_urb(write bulk) failed");
		acm->ready_for_write = 1;
		return stat;
	}

	return count;
}

static int acm_tty_write_room(struct tty_struct *tty)
{
	struct acm *acm = tty->driver_data;
	if (!ACM_READY(acm))
		return -EINVAL;
	return !acm->ready_for_write ? 0 : acm->writesize;
}

static int acm_tty_chars_in_buffer(struct tty_struct *tty)
{
	struct acm *acm = tty->driver_data;
	if (!ACM_READY(acm))
		return -EINVAL;
	return !acm->ready_for_write ? acm->writeurb->transfer_buffer_length : 0;
}

static void acm_tty_throttle(struct tty_struct *tty)
{
	struct acm *acm = tty->driver_data;
	if (!ACM_READY(acm))
		return;
	spin_lock_bh(&acm->throttle_lock);
	acm->throttle = 1;
	spin_unlock_bh(&acm->throttle_lock);
}

static void acm_tty_unthrottle(struct tty_struct *tty)
{
	struct acm *acm = tty->driver_data;
	if (!ACM_READY(acm))
		return;
	spin_lock_bh(&acm->throttle_lock);
	acm->throttle = 0;
	spin_unlock_bh(&acm->throttle_lock);
	if (acm->resubmit_to_unthrottle) {
		acm->resubmit_to_unthrottle = 0;
		acm_read_bulk(acm->readurb, NULL);
	}
}

static void acm_tty_break_ctl(struct tty_struct *tty, int state)
{
	struct acm *acm = tty->driver_data;
	if (!ACM_READY(acm))
		return;
	if (acm_send_break(acm, state ? 0xffff : 0))
		dbg("send break failed");
}

static int acm_tty_tiocmget(struct tty_struct *tty, struct file *file)
{
	struct acm *acm = tty->driver_data;

	if (!ACM_READY(acm))
		return -EINVAL;

	return (acm->ctrlout & ACM_CTRL_DTR ? TIOCM_DTR : 0) |
	       (acm->ctrlout & ACM_CTRL_RTS ? TIOCM_RTS : 0) |
	       (acm->ctrlin  & ACM_CTRL_DSR ? TIOCM_DSR : 0) |
	       (acm->ctrlin  & ACM_CTRL_RI  ? TIOCM_RI  : 0) |
	       (acm->ctrlin  & ACM_CTRL_DCD ? TIOCM_CD  : 0) |
	       TIOCM_CTS;
}

static int acm_tty_tiocmset(struct tty_struct *tty, struct file *file,
			    unsigned int set, unsigned int clear)
{
	struct acm *acm = tty->driver_data;
	unsigned int newctrl;

	if (!ACM_READY(acm))
		return -EINVAL;

	newctrl = acm->ctrlout;
	set = (set & TIOCM_DTR ? ACM_CTRL_DTR : 0) | (set & TIOCM_RTS ? ACM_CTRL_RTS : 0);
	clear = (clear & TIOCM_DTR ? ACM_CTRL_DTR : 0) | (clear & TIOCM_RTS ? ACM_CTRL_RTS : 0);

	newctrl = (newctrl & ~clear) | set;

	if (acm->ctrlout == newctrl)
		return 0;
	return acm_set_control(acm, acm->ctrlout = newctrl);
}

static int acm_tty_ioctl(struct tty_struct *tty, struct file *file, unsigned int cmd, unsigned long arg)
{
	struct acm *acm = tty->driver_data;

	if (!ACM_READY(acm))
		return -EINVAL;

	return -ENOIOCTLCMD;
}

static __u32 acm_tty_speed[] = {
	0, 50, 75, 110, 134, 150, 200, 300, 600,
	1200, 1800, 2400, 4800, 9600, 19200, 38400,
	57600, 115200, 230400, 460800, 500000, 576000,
	921600, 1000000, 1152000, 1500000, 2000000,
	2500000, 3000000, 3500000, 4000000
};

static __u8 acm_tty_size[] = {
	5, 6, 7, 8
};

static void acm_tty_set_termios(struct tty_struct *tty, struct termios *termios_old)
{
	struct acm *acm = tty->driver_data;
	struct termios *termios = tty->termios;
	struct acm_line newline;
	int newctrl = acm->ctrlout;

	if (!ACM_READY(acm))
		return;

	newline.speed = cpu_to_le32p(acm_tty_speed +
		(termios->c_cflag & CBAUD & ~CBAUDEX) + (termios->c_cflag & CBAUDEX ? 15 : 0));
	newline.stopbits = termios->c_cflag & CSTOPB ? 2 : 0;
	newline.parity = termios->c_cflag & PARENB ?
		(termios->c_cflag & PARODD ? 1 : 2) + (termios->c_cflag & CMSPAR ? 2 : 0) : 0;
	newline.databits = acm_tty_size[(termios->c_cflag & CSIZE) >> 4];

	acm->clocal = ((termios->c_cflag & CLOCAL) != 0);

	if (!newline.speed) {
		newline.speed = acm->line.speed;
		newctrl &= ~ACM_CTRL_DTR;
	} else  newctrl |=  ACM_CTRL_DTR;

	if (newctrl != acm->ctrlout)
		acm_set_control(acm, acm->ctrlout = newctrl);

	if (memcmp(&acm->line, &newline, sizeof(struct acm_line))) {
		memcpy(&acm->line, &newline, sizeof(struct acm_line));
		dbg("set line: %d %d %d %d", newline.speed, newline.stopbits, newline.parity, newline.databits);
		acm_set_line(acm, &acm->line);
	}
}

/*
 * USB probe and disconnect routines.
 */

static int acm_probe (struct usb_interface *intf,
		      const struct usb_device_id *id)
{
	struct union_desc *union_header = NULL;
	char *buffer = intf->altsetting->extra;
	int buflen = intf->altsetting->extralen;
	struct usb_interface *control_interface;
	struct usb_interface *data_interface;
	struct usb_endpoint_descriptor *epctrl;
	struct usb_endpoint_descriptor *epread;
	struct usb_endpoint_descriptor *epwrite;
	struct usb_device *usb_dev = interface_to_usbdev(intf);
	struct acm *acm;
	int minor;
	int ctrlsize,readsize;
	u8 *buf;
	u8 ac_management_function = 0;
	u8 call_management_function = 0;
	int call_interface_num = -1;
	int data_interface_num;

	if (!buffer) {
		err("Wierd descriptor references");
		return -EINVAL;
	}

	while (buflen > 0) {
		if (buffer [1] != USB_DT_CS_INTERFACE) {
			err("skipping garbage");
			goto next_desc;
		}

		switch (buffer [2]) {
			case CDC_UNION_TYPE: /* we've found it */
				if (union_header) {
					err("More than one union descriptor, skipping ...");
					goto next_desc;
				}
				union_header = (struct union_desc *)buffer;
				break;
			case CDC_COUNTRY_TYPE: /* maybe somehow export */
				break; /* for now we ignore it */
			case CDC_AC_MANAGEMENT_TYPE:
				ac_management_function = buffer[3];
				break;
			case CDC_CALL_MANAGEMENT_TYPE:
				call_management_function = buffer[3];
				call_interface_num = buffer[4];
				if ((call_management_function & 3) != 3)
					err("This device cannot do calls on its own. It is no modem.");
				break;
				
			default:
				err("Ignoring extra header");
				break;
			}
next_desc:
		buflen -= buffer[0];
		buffer += buffer[0];
	}

	if (!union_header) {
		dev_dbg(&intf->dev,"No union descriptor, giving up\n");
		return -ENODEV;
	}

	control_interface = usb_ifnum_to_if(usb_dev, union_header->bMasterInterface0);
	data_interface = usb_ifnum_to_if(usb_dev, (data_interface_num = union_header->bSlaveInterface0));
	if (!control_interface || !data_interface) {
		dev_dbg(&intf->dev,"no interfaces\n");
		return -ENODEV;
	}
	
	if (data_interface_num != call_interface_num)
		dev_dbg(&intf->dev,"Seperate call control interface. That is not fully supported.");

	if (usb_interface_claimed(data_interface)) { /* valid in this context */
		dev_dbg(&intf->dev,"The data interface isn't available\n");
		return -EBUSY;
	}

	/*workaround for switched interfaces */
	if (data_interface->cur_altsetting->desc.bInterfaceClass != CDC_DATA_INTERFACE_TYPE) {
		if (control_interface->cur_altsetting->desc.bInterfaceClass == CDC_DATA_INTERFACE_TYPE) {
			struct usb_interface *t;
			dev_dbg(&intf->dev,"Your device has switched interfaces.\n");

			t = control_interface;
			control_interface = data_interface;
			data_interface = t;
		} else {
			return -EINVAL;
		}
	}
	if (data_interface->cur_altsetting->desc.bNumEndpoints < 2)
		return -EINVAL;

	epctrl = &control_interface->cur_altsetting->endpoint[0].desc;
	epread = &data_interface->cur_altsetting->endpoint[0].desc;
	epwrite = &data_interface->cur_altsetting->endpoint[1].desc;


	/* workaround for switched endpoints */
	if ((epread->bEndpointAddress & USB_DIR_IN) != USB_DIR_IN) {
		/* descriptors are swapped */
		struct usb_endpoint_descriptor *t;
		dev_dbg(&intf->dev,"The data interface has switched endpoints\n");
		
		t = epread;
		epread = epwrite;
		epwrite = t;
	}
	dbg("interfaces are valid");
	for (minor = 0; minor < ACM_TTY_MINORS && acm_table[minor]; minor++);

	if (acm_table[minor]) {
		err("no more free acm devices");
		return -ENODEV;
	}

	if (!(acm = kmalloc(sizeof(struct acm), GFP_KERNEL))) {
		dev_dbg(&intf->dev, "out of memory (acm kmalloc)\n");
		goto alloc_fail;
	}
	memset(acm, 0, sizeof(struct acm));

	ctrlsize = epctrl->wMaxPacketSize;
	readsize = epread->wMaxPacketSize;
	acm->writesize = epwrite->wMaxPacketSize;
	acm->control = control_interface;
	acm->data = data_interface;
	acm->minor = minor;
	acm->dev = usb_dev;
	acm->ctrl_caps = ac_management_function;
	acm->ctrlsize = ctrlsize;
	acm->readsize = readsize;
	acm->bh.func = acm_rx_tasklet;
	acm->bh.data = (unsigned long) acm;
	INIT_WORK(&acm->work, acm_softint, acm);
	spin_lock_init(&acm->throttle_lock);
	acm->ready_for_write = 1;

	buf = usb_buffer_alloc(usb_dev, ctrlsize, GFP_KERNEL, &acm->ctrl_dma);
	if (!buf) {
		dev_dbg(&intf->dev, "out of memory (ctrl buffer alloc)\n");
		goto alloc_fail2;
	}
	acm->ctrl_buffer = buf;

	buf = usb_buffer_alloc(usb_dev, readsize, GFP_KERNEL, &acm->read_dma);
	if (!buf) {
		dev_dbg(&intf->dev, "out of memory (read buffer alloc)\n");
		goto alloc_fail3;
	}
	acm->read_buffer = buf;

	buf = usb_buffer_alloc(usb_dev, acm->writesize, GFP_KERNEL, &acm->write_dma);
	if (!buf) {
		dev_dbg(&intf->dev, "out of memory (write buffer alloc)\n");
		goto alloc_fail4;
	}
	acm->write_buffer = buf;	

	acm->ctrlurb = usb_alloc_urb(0, GFP_KERNEL);
	if (!acm->ctrlurb) {
		dev_dbg(&intf->dev, "out of memory (ctrlurb kmalloc)\n");
		goto alloc_fail5;
	}
	acm->readurb = usb_alloc_urb(0, GFP_KERNEL);
	if (!acm->readurb) {
		dev_dbg(&intf->dev, "out of memory (readurb kmalloc)\n");
		goto alloc_fail6;
	}
	acm->writeurb = usb_alloc_urb(0, GFP_KERNEL);
	if (!acm->writeurb) {
		dev_dbg(&intf->dev, "out of memory (writeurb kmalloc)\n");
		goto alloc_fail7;
	}

	usb_fill_int_urb(acm->ctrlurb, usb_dev, usb_rcvintpipe(usb_dev, epctrl->bEndpointAddress),
			 acm->ctrl_buffer, ctrlsize, acm_ctrl_irq, acm, epctrl->bInterval);
	acm->ctrlurb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	acm->ctrlurb->transfer_dma = acm->ctrl_dma;

	usb_fill_bulk_urb(acm->readurb, usb_dev, usb_rcvbulkpipe(usb_dev, epread->bEndpointAddress),
			  acm->read_buffer, readsize, acm_read_bulk, acm);
	acm->readurb->transfer_flags |= URB_NO_FSBR | URB_NO_TRANSFER_DMA_MAP;
	acm->readurb->transfer_dma = acm->read_dma;

	usb_fill_bulk_urb(acm->writeurb, usb_dev, usb_sndbulkpipe(usb_dev, epwrite->bEndpointAddress),
			  acm->write_buffer, acm->writesize, acm_write_bulk, acm);
	acm->writeurb->transfer_flags |= URB_NO_FSBR | URB_NO_TRANSFER_DMA_MAP;
	acm->writeurb->transfer_dma = acm->write_dma;

	dev_info(&intf->dev, "ttyACM%d: USB ACM device\n", minor);

	acm_set_control(acm, acm->ctrlout);

	acm->line.speed = cpu_to_le32(9600);
	acm->line.databits = 8;
	acm_set_line(acm, &acm->line);

	usb_driver_claim_interface(&acm_driver, data_interface, acm);

	tty_register_device(acm_tty_driver, minor, &intf->dev);

	acm_table[minor] = acm;
	usb_set_intfdata (intf, acm);
	return 0;

alloc_fail7:
	usb_free_urb(acm->readurb);
alloc_fail6:
	usb_free_urb(acm->ctrlurb);
alloc_fail5:
	usb_buffer_free(usb_dev, acm->writesize, acm->write_buffer, acm->write_dma);
alloc_fail4:
	usb_buffer_free(usb_dev, readsize, acm->read_buffer, acm->read_dma);
alloc_fail3:
	usb_buffer_free(usb_dev, ctrlsize, acm->ctrl_buffer, acm->ctrl_dma);
alloc_fail2:
	kfree(acm);
alloc_fail:
	return -ENOMEM;
}

static void acm_disconnect(struct usb_interface *intf)
{
	struct acm *acm = usb_get_intfdata (intf);
	struct usb_device *usb_dev = interface_to_usbdev(intf);

	if (!acm || !acm->dev) {
		dbg("disconnect on nonexisting interface");
		return;
	}

	down(&open_sem);
	acm->dev = NULL;
	usb_set_intfdata (intf, NULL);

	usb_unlink_urb(acm->ctrlurb);
	usb_unlink_urb(acm->readurb);
	usb_unlink_urb(acm->writeurb);

	flush_scheduled_work(); /* wait for acm_softint */

	usb_buffer_free(usb_dev, acm->writesize, acm->write_buffer, acm->write_dma);
	usb_buffer_free(usb_dev, acm->readsize, acm->read_buffer, acm->read_dma);
	usb_buffer_free(usb_dev, acm->ctrlsize, acm->ctrl_buffer, acm->ctrl_dma);

	usb_driver_release_interface(&acm_driver, acm->data);

	if (!acm->used) {
		tty_unregister_device(acm_tty_driver, acm->minor);
		acm_table[acm->minor] = NULL;
		usb_free_urb(acm->ctrlurb);
		usb_free_urb(acm->readurb);
		usb_free_urb(acm->writeurb);
		kfree(acm);
		up(&open_sem);
		return;
	}

	up(&open_sem);

	if (acm->tty)
		tty_hangup(acm->tty);
}

/*
 * USB driver structure.
 */

static struct usb_device_id acm_ids[] = {
	/* control interfaces with various AT-command sets */
	{ USB_INTERFACE_INFO(USB_CLASS_COMM, 2, 1) },
	{ USB_INTERFACE_INFO(USB_CLASS_COMM, 2, 2) },
	{ USB_INTERFACE_INFO(USB_CLASS_COMM, 2, 3) },
	{ USB_INTERFACE_INFO(USB_CLASS_COMM, 2, 4) },
	{ USB_INTERFACE_INFO(USB_CLASS_COMM, 2, 5) },
	{ USB_INTERFACE_INFO(USB_CLASS_COMM, 2, 6) },

	/* NOTE:  COMM/2/0xff is likely MSFT RNDIS ... NOT a modem!! */
	{ }
};

MODULE_DEVICE_TABLE (usb, acm_ids);

static struct usb_driver acm_driver = {
	.owner =	THIS_MODULE,
	.name =		"cdc_acm",
	.probe =	acm_probe,
	.disconnect =	acm_disconnect,
	.id_table =	acm_ids,
};

/*
 * TTY driver structures.
 */

static struct tty_operations acm_ops = {
	.open =			acm_tty_open,
	.close =		acm_tty_close,
	.write =		acm_tty_write,
	.write_room =		acm_tty_write_room,
	.ioctl =		acm_tty_ioctl,
	.throttle =		acm_tty_throttle,
	.unthrottle =		acm_tty_unthrottle,
	.chars_in_buffer =	acm_tty_chars_in_buffer,
	.break_ctl =		acm_tty_break_ctl,
	.set_termios =		acm_tty_set_termios,
	.tiocmget =		acm_tty_tiocmget,
	.tiocmset =		acm_tty_tiocmset,
};

/*
 * Init / exit.
 */

static int __init acm_init(void)
{
	int retval;
	acm_tty_driver = alloc_tty_driver(ACM_TTY_MINORS);
	if (!acm_tty_driver)
		return -ENOMEM;
	acm_tty_driver->owner = THIS_MODULE,
	acm_tty_driver->driver_name = "acm",
	acm_tty_driver->name = "ttyACM",
	acm_tty_driver->devfs_name = "usb/acm/",
	acm_tty_driver->major = ACM_TTY_MAJOR,
	acm_tty_driver->minor_start = 0,
	acm_tty_driver->type = TTY_DRIVER_TYPE_SERIAL,
	acm_tty_driver->subtype = SERIAL_TYPE_NORMAL,
	acm_tty_driver->flags = TTY_DRIVER_REAL_RAW | TTY_DRIVER_NO_DEVFS,
	acm_tty_driver->init_termios = tty_std_termios;
	acm_tty_driver->init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	tty_set_operations(acm_tty_driver, &acm_ops);

	retval = tty_register_driver(acm_tty_driver);
	if (retval) {
		put_tty_driver(acm_tty_driver);
		return retval;
	}

	retval = usb_register(&acm_driver);
	if (retval) {
		tty_unregister_driver(acm_tty_driver);
		put_tty_driver(acm_tty_driver);
		return retval;
	}

	info(DRIVER_VERSION ":" DRIVER_DESC);

	return 0;
}

static void __exit acm_exit(void)
{
	usb_deregister(&acm_driver);
	tty_unregister_driver(acm_tty_driver);
	put_tty_driver(acm_tty_driver);
}

module_init(acm_init);
module_exit(acm_exit);

MODULE_AUTHOR( DRIVER_AUTHOR );
MODULE_DESCRIPTION( DRIVER_DESC );
MODULE_LICENSE("GPL");

