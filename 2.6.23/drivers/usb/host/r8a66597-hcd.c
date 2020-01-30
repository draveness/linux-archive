/*
 * R8A66597 HCD (Host Controller Driver)
 *
 * Copyright (C) 2006-2007 Renesas Solutions Corp.
 * Portions Copyright (C) 2004 Psion Teklogix (for NetBook PRO)
 * Portions Copyright (C) 2004-2005 David Brownell
 * Portions Copyright (C) 1999 Roman Weissgaerber
 *
 * Author : Yoshihiro Shimoda <shimoda.yoshihiro@renesas.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/smp_lock.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/interrupt.h>
#include <linux/usb.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/irq.h>

#include "../core/hcd.h"
#include "r8a66597.h"

MODULE_DESCRIPTION("R8A66597 USB Host Controller Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Yoshihiro Shimoda");

#define DRIVER_VERSION	"29 May 2007"

static const char hcd_name[] = "r8a66597_hcd";

/* module parameters */
static unsigned short clock = XTAL12;
module_param(clock, ushort, 0644);
MODULE_PARM_DESC(clock, "input clock: 48MHz=32768, 24MHz=16384, 12MHz=0 "
		"(default=0)");

static unsigned short vif = LDRV;
module_param(vif, ushort, 0644);
MODULE_PARM_DESC(vif, "input VIF: 3.3V=32768, 1.5V=0(default=32768)");

static unsigned short endian;
module_param(endian, ushort, 0644);
MODULE_PARM_DESC(endian, "data endian: big=256, little=0 (default=0)");

static unsigned short irq_sense = INTL;
module_param(irq_sense, ushort, 0644);
MODULE_PARM_DESC(irq_sense, "IRQ sense: low level=32, falling edge=0 "
		"(default=32)");

static void packet_write(struct r8a66597 *r8a66597, u16 pipenum);
static int r8a66597_get_frame(struct usb_hcd *hcd);

/* this function must be called with interrupt disabled */
static void enable_pipe_irq(struct r8a66597 *r8a66597, u16 pipenum,
			    unsigned long reg)
{
	u16 tmp;

	tmp = r8a66597_read(r8a66597, INTENB0);
	r8a66597_bclr(r8a66597, BEMPE | NRDYE | BRDYE, INTENB0);
	r8a66597_bset(r8a66597, 1 << pipenum, reg);
	r8a66597_write(r8a66597, tmp, INTENB0);
}

/* this function must be called with interrupt disabled */
static void disable_pipe_irq(struct r8a66597 *r8a66597, u16 pipenum,
			     unsigned long reg)
{
	u16 tmp;

	tmp = r8a66597_read(r8a66597, INTENB0);
	r8a66597_bclr(r8a66597, BEMPE | NRDYE | BRDYE, INTENB0);
	r8a66597_bclr(r8a66597, 1 << pipenum, reg);
	r8a66597_write(r8a66597, tmp, INTENB0);
}

static void set_devadd_reg(struct r8a66597 *r8a66597, u8 r8a66597_address,
			   u16 usbspd, u8 upphub, u8 hubport, int port)
{
	u16 val;
	unsigned long devadd_reg = get_devadd_addr(r8a66597_address);

	val = (upphub << 11) | (hubport << 8) | (usbspd << 6) | (port & 0x0001);
	r8a66597_write(r8a66597, val, devadd_reg);
}

static int enable_controller(struct r8a66597 *r8a66597)
{
	u16 tmp;
	int i = 0;

	do {
		r8a66597_write(r8a66597, USBE, SYSCFG0);
		tmp = r8a66597_read(r8a66597, SYSCFG0);
		if (i++ > 1000) {
			err("register access fail.");
			return -ENXIO;
		}
	} while ((tmp & USBE) != USBE);
	r8a66597_bclr(r8a66597, USBE, SYSCFG0);
	r8a66597_mdfy(r8a66597, clock, XTAL, SYSCFG0);

	i = 0;
	r8a66597_bset(r8a66597, XCKE, SYSCFG0);
	do {
		msleep(1);
		tmp = r8a66597_read(r8a66597, SYSCFG0);
		if (i++ > 500) {
			err("register access fail.");
			return -ENXIO;
		}
	} while ((tmp & SCKE) != SCKE);

	r8a66597_bset(r8a66597, DCFM | DRPD, SYSCFG0);
	r8a66597_bset(r8a66597, DRPD, SYSCFG1);

	r8a66597_bset(r8a66597, vif & LDRV, PINCFG);
	r8a66597_bset(r8a66597, HSE, SYSCFG0);
	r8a66597_bset(r8a66597, HSE, SYSCFG1);
	r8a66597_bset(r8a66597, USBE, SYSCFG0);

	r8a66597_bset(r8a66597, BEMPE | NRDYE | BRDYE, INTENB0);
	r8a66597_bset(r8a66597, irq_sense & INTL, SOFCFG);
	r8a66597_bset(r8a66597, BRDY0, BRDYENB);
	r8a66597_bset(r8a66597, BEMP0, BEMPENB);

	r8a66597_write(r8a66597, BURST | CPU_ADR_RD_WR, DMA0CFG);
	r8a66597_write(r8a66597, BURST | CPU_ADR_RD_WR, DMA1CFG);

	r8a66597_bset(r8a66597, endian & BIGEND, CFIFOSEL);
	r8a66597_bset(r8a66597, endian & BIGEND, D0FIFOSEL);
	r8a66597_bset(r8a66597, endian & BIGEND, D1FIFOSEL);

	r8a66597_bset(r8a66597, TRNENSEL, SOFCFG);

	r8a66597_bset(r8a66597, SIGNE | SACKE, INTENB1);
	r8a66597_bclr(r8a66597, DTCHE, INTENB1);
	r8a66597_bset(r8a66597, ATTCHE, INTENB1);
	r8a66597_bclr(r8a66597, DTCHE, INTENB2);
	r8a66597_bset(r8a66597, ATTCHE, INTENB2);

	return 0;
}

static void disable_controller(struct r8a66597 *r8a66597)
{
	u16 tmp;

	r8a66597_write(r8a66597, 0, INTENB0);
	r8a66597_write(r8a66597, 0, INTENB1);
	r8a66597_write(r8a66597, 0, INTENB2);
	r8a66597_write(r8a66597, 0, INTSTS0);
	r8a66597_write(r8a66597, 0, INTSTS1);
	r8a66597_write(r8a66597, 0, INTSTS2);

	r8a66597_port_power(r8a66597, 0, 0);
	r8a66597_port_power(r8a66597, 1, 0);

	do {
		tmp = r8a66597_read(r8a66597, SOFCFG) & EDGESTS;
		udelay(640);
	} while (tmp == EDGESTS);

	r8a66597_bclr(r8a66597, DCFM | DRPD, SYSCFG0);
	r8a66597_bclr(r8a66597, DRPD, SYSCFG1);
	r8a66597_bclr(r8a66597, HSE, SYSCFG0);
	r8a66597_bclr(r8a66597, HSE, SYSCFG1);

	r8a66597_bclr(r8a66597, SCKE, SYSCFG0);
	udelay(1);
	r8a66597_bclr(r8a66597, PLLC, SYSCFG0);
	r8a66597_bclr(r8a66597, XCKE, SYSCFG0);
	r8a66597_bclr(r8a66597, USBE, SYSCFG0);
}

static int get_parent_r8a66597_address(struct r8a66597 *r8a66597,
				       struct usb_device *udev)
{
	struct r8a66597_device *dev;

	if (udev->parent && udev->parent->devnum != 1)
		udev = udev->parent;

	dev = dev_get_drvdata(&udev->dev);
	if (dev)
		return dev->address;
	else
		return 0;
}

static int is_child_device(char *devpath)
{
	return (devpath[2] ? 1 : 0);
}

static int is_hub_limit(char *devpath)
{
	return ((strlen(devpath) >= 4) ? 1 : 0);
}

static void get_port_number(char *devpath, u16 *root_port, u16 *hub_port)
{
	if (root_port) {
		*root_port = (devpath[0] & 0x0F) - 1;
		if (*root_port >= R8A66597_MAX_ROOT_HUB)
			err("illegal root port number");
	}
	if (hub_port)
		*hub_port = devpath[2] & 0x0F;
}

static u16 get_r8a66597_usb_speed(enum usb_device_speed speed)
{
	u16 usbspd = 0;

	switch (speed) {
	case USB_SPEED_LOW:
		usbspd = LSMODE;
		break;
	case USB_SPEED_FULL:
		usbspd = FSMODE;
		break;
	case USB_SPEED_HIGH:
		usbspd = HSMODE;
		break;
	default:
		err("unknown speed");
		break;
	}

	return usbspd;
}

static void set_child_connect_map(struct r8a66597 *r8a66597, int address)
{
	int idx;

	idx = address / 32;
	r8a66597->child_connect_map[idx] |= 1 << (address % 32);
}

static void put_child_connect_map(struct r8a66597 *r8a66597, int address)
{
	int idx;

	idx = address / 32;
	r8a66597->child_connect_map[idx] &= ~(1 << (address % 32));
}

static void set_pipe_reg_addr(struct r8a66597_pipe *pipe, u8 dma_ch)
{
	u16 pipenum = pipe->info.pipenum;
	unsigned long fifoaddr[] = {D0FIFO, D1FIFO, CFIFO};
	unsigned long fifosel[] = {D0FIFOSEL, D1FIFOSEL, CFIFOSEL};
	unsigned long fifoctr[] = {D0FIFOCTR, D1FIFOCTR, CFIFOCTR};

	if (dma_ch > R8A66597_PIPE_NO_DMA)	/* dma fifo not use? */
		dma_ch = R8A66597_PIPE_NO_DMA;

	pipe->fifoaddr = fifoaddr[dma_ch];
	pipe->fifosel = fifosel[dma_ch];
	pipe->fifoctr = fifoctr[dma_ch];

	if (pipenum == 0)
		pipe->pipectr = DCPCTR;
	else
		pipe->pipectr = get_pipectr_addr(pipenum);

	if (check_bulk_or_isoc(pipenum)) {
		pipe->pipetre = get_pipetre_addr(pipenum);
		pipe->pipetrn = get_pipetrn_addr(pipenum);
	} else {
		pipe->pipetre = 0;
		pipe->pipetrn = 0;
	}
}

static struct r8a66597_device *
get_urb_to_r8a66597_dev(struct r8a66597 *r8a66597, struct urb *urb)
{
	if (usb_pipedevice(urb->pipe) == 0)
		return &r8a66597->device0;

	return dev_get_drvdata(&urb->dev->dev);
}

static int make_r8a66597_device(struct r8a66597 *r8a66597,
				struct urb *urb, u8 addr)
{
	struct r8a66597_device *dev;
	int usb_address = urb->setup_packet[2];	/* urb->pipe is address 0 */

	dev = kzalloc(sizeof(struct r8a66597_device), GFP_ATOMIC);
	if (dev == NULL)
		return -ENOMEM;

	dev_set_drvdata(&urb->dev->dev, dev);
	dev->udev = urb->dev;
	dev->address = addr;
	dev->usb_address = usb_address;
	dev->state = USB_STATE_ADDRESS;
	dev->ep_in_toggle = 0;
	dev->ep_out_toggle = 0;
	INIT_LIST_HEAD(&dev->device_list);
	list_add_tail(&dev->device_list, &r8a66597->child_device);

	get_port_number(urb->dev->devpath, &dev->root_port, &dev->hub_port);
	if (!is_child_device(urb->dev->devpath))
		r8a66597->root_hub[dev->root_port].dev = dev;

	set_devadd_reg(r8a66597, dev->address,
		       get_r8a66597_usb_speed(urb->dev->speed),
		       get_parent_r8a66597_address(r8a66597, urb->dev),
		       dev->hub_port, dev->root_port);

	return 0;
}

/* this function must be called with interrupt disabled */
static u8 alloc_usb_address(struct r8a66597 *r8a66597, struct urb *urb)
{
	u8 addr;	/* R8A66597's address */
	struct r8a66597_device *dev;

	if (is_hub_limit(urb->dev->devpath)) {
		err("Externel hub limit reached.");
		return 0;
	}

	dev = get_urb_to_r8a66597_dev(r8a66597, urb);
	if (dev && dev->state >= USB_STATE_ADDRESS)
		return dev->address;

	for (addr = 1; addr <= R8A66597_MAX_DEVICE; addr++) {
		if (r8a66597->address_map & (1 << addr))
			continue;

		dbg("alloc_address: r8a66597_addr=%d", addr);
		r8a66597->address_map |= 1 << addr;

		if (make_r8a66597_device(r8a66597, urb, addr) < 0)
			return 0;

		return addr;
	}

	err("cannot communicate with a USB device more than 10.(%x)",
	    r8a66597->address_map);

	return 0;
}

/* this function must be called with interrupt disabled */
static void free_usb_address(struct r8a66597 *r8a66597,
			     struct r8a66597_device *dev)
{
	int port;

	if (!dev)
		return;

	dbg("free_addr: addr=%d", dev->address);

	dev->state = USB_STATE_DEFAULT;
	r8a66597->address_map &= ~(1 << dev->address);
	dev->address = 0;
	dev_set_drvdata(&dev->udev->dev, NULL);
	list_del(&dev->device_list);
	kfree(dev);

	for (port = 0; port < R8A66597_MAX_ROOT_HUB; port++) {
		if (r8a66597->root_hub[port].dev == dev) {
			r8a66597->root_hub[port].dev = NULL;
			break;
		}
	}
}

static void r8a66597_reg_wait(struct r8a66597 *r8a66597, unsigned long reg,
			      u16 mask, u16 loop)
{
	u16 tmp;
	int i = 0;

	do {
		tmp = r8a66597_read(r8a66597, reg);
		if (i++ > 1000000) {
			err("register%lx, loop %x is timeout", reg, loop);
			break;
		}
		ndelay(1);
	} while ((tmp & mask) != loop);
}

/* this function must be called with interrupt disabled */
static void pipe_start(struct r8a66597 *r8a66597, struct r8a66597_pipe *pipe)
{
	u16 tmp;

	tmp = r8a66597_read(r8a66597, pipe->pipectr) & PID;
	if ((pipe->info.pipenum != 0) & ((tmp & PID_STALL) != 0)) /* stall? */
		r8a66597_mdfy(r8a66597, PID_NAK, PID, pipe->pipectr);
	r8a66597_mdfy(r8a66597, PID_BUF, PID, pipe->pipectr);
}

/* this function must be called with interrupt disabled */
static void pipe_stop(struct r8a66597 *r8a66597, struct r8a66597_pipe *pipe)
{
	u16 tmp;

	tmp = r8a66597_read(r8a66597, pipe->pipectr) & PID;
	if ((tmp & PID_STALL11) != PID_STALL11)	/* force stall? */
		r8a66597_mdfy(r8a66597, PID_STALL, PID, pipe->pipectr);
	r8a66597_mdfy(r8a66597, PID_NAK, PID, pipe->pipectr);
	r8a66597_reg_wait(r8a66597, pipe->pipectr, PBUSY, 0);
}

/* this function must be called with interrupt disabled */
static void clear_all_buffer(struct r8a66597 *r8a66597,
			     struct r8a66597_pipe *pipe)
{
	u16 tmp;

	if (!pipe || pipe->info.pipenum == 0)
		return;

	pipe_stop(r8a66597, pipe);
	r8a66597_bset(r8a66597, ACLRM, pipe->pipectr);
	tmp = r8a66597_read(r8a66597, pipe->pipectr);
	tmp = r8a66597_read(r8a66597, pipe->pipectr);
	tmp = r8a66597_read(r8a66597, pipe->pipectr);
	r8a66597_bclr(r8a66597, ACLRM, pipe->pipectr);
}

/* this function must be called with interrupt disabled */
static void r8a66597_pipe_toggle(struct r8a66597 *r8a66597,
				 struct r8a66597_pipe *pipe, int toggle)
{
	if (toggle)
		r8a66597_bset(r8a66597, SQSET, pipe->pipectr);
	else
		r8a66597_bset(r8a66597, SQCLR, pipe->pipectr);
}

/* this function must be called with interrupt disabled */
static inline void cfifo_change(struct r8a66597 *r8a66597, u16 pipenum)
{
	r8a66597_mdfy(r8a66597, MBW | pipenum, MBW | CURPIPE, CFIFOSEL);
	r8a66597_reg_wait(r8a66597, CFIFOSEL, CURPIPE, pipenum);
}

/* this function must be called with interrupt disabled */
static inline void fifo_change_from_pipe(struct r8a66597 *r8a66597,
					 struct r8a66597_pipe *pipe)
{
	cfifo_change(r8a66597, 0);
	r8a66597_mdfy(r8a66597, MBW | 0, MBW | CURPIPE, D0FIFOSEL);
	r8a66597_mdfy(r8a66597, MBW | 0, MBW | CURPIPE, D1FIFOSEL);

	r8a66597_mdfy(r8a66597, MBW | pipe->info.pipenum, MBW | CURPIPE,
		      pipe->fifosel);
	r8a66597_reg_wait(r8a66597, pipe->fifosel, CURPIPE, pipe->info.pipenum);
}

static u16 r8a66597_get_pipenum(struct urb *urb, struct usb_host_endpoint *hep)
{
	struct r8a66597_pipe *pipe = hep->hcpriv;

	if (usb_pipeendpoint(urb->pipe) == 0)
		return 0;
	else
		return pipe->info.pipenum;
}

static u16 get_urb_to_r8a66597_addr(struct r8a66597 *r8a66597, struct urb *urb)
{
	struct r8a66597_device *dev = get_urb_to_r8a66597_dev(r8a66597, urb);

	return (usb_pipedevice(urb->pipe) == 0) ? 0 : dev->address;
}

static unsigned short *get_toggle_pointer(struct r8a66597_device *dev,
					  int urb_pipe)
{
	if (!dev)
		return NULL;

	return usb_pipein(urb_pipe) ? &dev->ep_in_toggle : &dev->ep_out_toggle;
}

/* this function must be called with interrupt disabled */
static void pipe_toggle_set(struct r8a66597 *r8a66597,
			    struct r8a66597_pipe *pipe,
			    struct urb *urb, int set)
{
	struct r8a66597_device *dev = get_urb_to_r8a66597_dev(r8a66597, urb);
	unsigned char endpoint = usb_pipeendpoint(urb->pipe);
	unsigned short *toggle = get_toggle_pointer(dev, urb->pipe);

	if (!toggle)
		return;

	if (set)
		*toggle |= 1 << endpoint;
	else
		*toggle &= ~(1 << endpoint);
}

/* this function must be called with interrupt disabled */
static void pipe_toggle_save(struct r8a66597 *r8a66597,
			     struct r8a66597_pipe *pipe,
			     struct urb *urb)
{
	if (r8a66597_read(r8a66597, pipe->pipectr) & SQMON)
		pipe_toggle_set(r8a66597, pipe, urb, 1);
	else
		pipe_toggle_set(r8a66597, pipe, urb, 0);
}

/* this function must be called with interrupt disabled */
static void pipe_toggle_restore(struct r8a66597 *r8a66597,
				struct r8a66597_pipe *pipe,
				struct urb *urb)
{
	struct r8a66597_device *dev = get_urb_to_r8a66597_dev(r8a66597, urb);
	unsigned char endpoint = usb_pipeendpoint(urb->pipe);
	unsigned short *toggle = get_toggle_pointer(dev, urb->pipe);

	if (!toggle)
		return;

	r8a66597_pipe_toggle(r8a66597, pipe, *toggle & (1 << endpoint));
}

/* this function must be called with interrupt disabled */
static void pipe_buffer_setting(struct r8a66597 *r8a66597,
				struct r8a66597_pipe_info *info)
{
	u16 val = 0;

	if (info->pipenum == 0)
		return;

	r8a66597_bset(r8a66597, ACLRM, get_pipectr_addr(info->pipenum));
	r8a66597_bclr(r8a66597, ACLRM, get_pipectr_addr(info->pipenum));
	r8a66597_write(r8a66597, info->pipenum, PIPESEL);
	if (!info->dir_in)
		val |= R8A66597_DIR;
	if (info->type == R8A66597_BULK && info->dir_in)
		val |= R8A66597_DBLB | R8A66597_SHTNAK;
	val |= info->type | info->epnum;
	r8a66597_write(r8a66597, val, PIPECFG);

	r8a66597_write(r8a66597, (info->buf_bsize << 10) | (info->bufnum),
		       PIPEBUF);
	r8a66597_write(r8a66597, make_devsel(info->address) | info->maxpacket,
		       PIPEMAXP);
	if (info->interval)
		info->interval--;
	r8a66597_write(r8a66597, info->interval, PIPEPERI);
}



/* this function must be called with interrupt disabled */
static void pipe_setting(struct r8a66597 *r8a66597, struct r8a66597_td *td)
{
	struct r8a66597_pipe_info *info;
	struct urb *urb = td->urb;

	if (td->pipenum > 0) {
		info = &td->pipe->info;
		cfifo_change(r8a66597, 0);
		pipe_buffer_setting(r8a66597, info);

		if (!usb_gettoggle(urb->dev, usb_pipeendpoint(urb->pipe),
				   usb_pipeout(urb->pipe)) &&
		    !usb_pipecontrol(urb->pipe)) {
			r8a66597_pipe_toggle(r8a66597, td->pipe, 0);
			pipe_toggle_set(r8a66597, td->pipe, urb, 0);
			clear_all_buffer(r8a66597, td->pipe);
			usb_settoggle(urb->dev, usb_pipeendpoint(urb->pipe),
				      usb_pipeout(urb->pipe), 1);
		}
		pipe_toggle_restore(r8a66597, td->pipe, urb);
	}
}

/* this function must be called with interrupt disabled */
static u16 get_empty_pipenum(struct r8a66597 *r8a66597,
			     struct usb_endpoint_descriptor *ep)
{
	u16 array[R8A66597_MAX_NUM_PIPE], i = 0, min;

	memset(array, 0, sizeof(array));
	switch (ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) {
	case USB_ENDPOINT_XFER_BULK:
		if (ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK)
			array[i++] = 4;
		else {
			array[i++] = 3;
			array[i++] = 5;
		}
		break;
	case USB_ENDPOINT_XFER_INT:
		if (ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK) {
			array[i++] = 6;
			array[i++] = 7;
			array[i++] = 8;
		} else
			array[i++] = 9;
		break;
	case USB_ENDPOINT_XFER_ISOC:
		if (ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK)
			array[i++] = 2;
		else
			array[i++] = 1;
		break;
	default:
		err("Illegal type");
		return 0;
	}

	i = 1;
	min = array[0];
	while (array[i] != 0) {
		if (r8a66597->pipe_cnt[min] > r8a66597->pipe_cnt[array[i]])
			min = array[i];
		i++;
	}

	return min;
}

static u16 get_r8a66597_type(__u8 type)
{
	u16 r8a66597_type;

	switch (type) {
	case USB_ENDPOINT_XFER_BULK:
		r8a66597_type = R8A66597_BULK;
		break;
	case USB_ENDPOINT_XFER_INT:
		r8a66597_type = R8A66597_INT;
		break;
	case USB_ENDPOINT_XFER_ISOC:
		r8a66597_type = R8A66597_ISO;
		break;
	default:
		err("Illegal type");
		r8a66597_type = 0x0000;
		break;
	}

	return r8a66597_type;
}

static u16 get_bufnum(u16 pipenum)
{
	u16 bufnum = 0;

	if (pipenum == 0)
		bufnum = 0;
	else if (check_bulk_or_isoc(pipenum))
		bufnum = 8 + (pipenum - 1) * R8A66597_BUF_BSIZE*2;
	else if (check_interrupt(pipenum))
		bufnum = 4 + (pipenum - 6);
	else
		err("Illegal pipenum (%d)", pipenum);

	return bufnum;
}

static u16 get_buf_bsize(u16 pipenum)
{
	u16 buf_bsize = 0;

	if (pipenum == 0)
		buf_bsize = 3;
	else if (check_bulk_or_isoc(pipenum))
		buf_bsize = R8A66597_BUF_BSIZE - 1;
	else if (check_interrupt(pipenum))
		buf_bsize = 0;
	else
		err("Illegal pipenum (%d)", pipenum);

	return buf_bsize;
}

/* this function must be called with interrupt disabled */
static void enable_r8a66597_pipe_dma(struct r8a66597 *r8a66597,
				     struct r8a66597_device *dev,
				     struct r8a66597_pipe *pipe,
				     struct urb *urb)
{
	int i;
	struct r8a66597_pipe_info *info = &pipe->info;

	if ((pipe->info.pipenum != 0) && (info->type != R8A66597_INT)) {
		for (i = 0; i < R8A66597_MAX_DMA_CHANNEL; i++) {
			if ((r8a66597->dma_map & (1 << i)) != 0)
				continue;

			info("address %d, EndpointAddress 0x%02x use DMA FIFO",
			     usb_pipedevice(urb->pipe),
			     info->dir_in ? USB_ENDPOINT_DIR_MASK + info->epnum
					    : info->epnum);

			r8a66597->dma_map |= 1 << i;
			dev->dma_map |= 1 << i;
			set_pipe_reg_addr(pipe, i);

			cfifo_change(r8a66597, 0);
			r8a66597_mdfy(r8a66597, MBW | pipe->info.pipenum,
				      MBW | CURPIPE, pipe->fifosel);

			r8a66597_reg_wait(r8a66597, pipe->fifosel, CURPIPE,
					  pipe->info.pipenum);
			r8a66597_bset(r8a66597, BCLR, pipe->fifoctr);
			break;
		}
	}
}

/* this function must be called with interrupt disabled */
static void enable_r8a66597_pipe(struct r8a66597 *r8a66597, struct urb *urb,
				 struct usb_host_endpoint *hep,
				 struct r8a66597_pipe_info *info)
{
	struct r8a66597_device *dev = get_urb_to_r8a66597_dev(r8a66597, urb);
	struct r8a66597_pipe *pipe = hep->hcpriv;

	dbg("enable_pipe:");

	pipe->info = *info;
	set_pipe_reg_addr(pipe, R8A66597_PIPE_NO_DMA);
	r8a66597->pipe_cnt[pipe->info.pipenum]++;
	dev->pipe_cnt[pipe->info.pipenum]++;

	enable_r8a66597_pipe_dma(r8a66597, dev, pipe, urb);
}

/* this function must be called with interrupt disabled */
static void force_dequeue(struct r8a66597 *r8a66597, u16 pipenum, u16 address)
{
	struct r8a66597_td *td, *next;
	struct urb *urb;
	struct list_head *list = &r8a66597->pipe_queue[pipenum];

	if (list_empty(list))
		return;

	list_for_each_entry_safe(td, next, list, queue) {
		if (!td)
			continue;
		if (td->address != address)
			continue;

		urb = td->urb;
		list_del(&td->queue);
		kfree(td);

		if (urb) {
			urb->status = -ENODEV;
			urb->hcpriv = NULL;
			spin_unlock(&r8a66597->lock);
			usb_hcd_giveback_urb(r8a66597_to_hcd(r8a66597), urb);
			spin_lock(&r8a66597->lock);
		}
		break;
	}
}

/* this function must be called with interrupt disabled */
static void disable_r8a66597_pipe_all(struct r8a66597 *r8a66597,
				      struct r8a66597_device *dev)
{
	int check_ep0 = 0;
	u16 pipenum;

	if (!dev)
		return;

	for (pipenum = 1; pipenum < R8A66597_MAX_NUM_PIPE; pipenum++) {
		if (!dev->pipe_cnt[pipenum])
			continue;

		if (!check_ep0) {
			check_ep0 = 1;
			force_dequeue(r8a66597, 0, dev->address);
		}

		r8a66597->pipe_cnt[pipenum] -= dev->pipe_cnt[pipenum];
		dev->pipe_cnt[pipenum] = 0;
		force_dequeue(r8a66597, pipenum, dev->address);
	}

	dbg("disable_pipe");

	r8a66597->dma_map &= ~(dev->dma_map);
	dev->dma_map = 0;
}

/* this function must be called with interrupt disabled */
static void init_pipe_info(struct r8a66597 *r8a66597, struct urb *urb,
			   struct usb_host_endpoint *hep,
			   struct usb_endpoint_descriptor *ep)
{
	struct r8a66597_pipe_info info;

	info.pipenum = get_empty_pipenum(r8a66597, ep);
	info.address = get_urb_to_r8a66597_addr(r8a66597, urb);
	info.epnum = ep->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
	info.maxpacket = ep->wMaxPacketSize;
	info.type = get_r8a66597_type(ep->bmAttributes
				      & USB_ENDPOINT_XFERTYPE_MASK);
	info.bufnum = get_bufnum(info.pipenum);
	info.buf_bsize = get_buf_bsize(info.pipenum);
	info.interval = ep->bInterval;
	if (ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK)
		info.dir_in = 1;
	else
		info.dir_in = 0;

	enable_r8a66597_pipe(r8a66597, urb, hep, &info);
}

static void init_pipe_config(struct r8a66597 *r8a66597, struct urb *urb)
{
	struct r8a66597_device *dev;

	dev = get_urb_to_r8a66597_dev(r8a66597, urb);
	dev->state = USB_STATE_CONFIGURED;
}

static void pipe_irq_enable(struct r8a66597 *r8a66597, struct urb *urb,
			    u16 pipenum)
{
	if (pipenum == 0 && usb_pipeout(urb->pipe))
		enable_irq_empty(r8a66597, pipenum);
	else
		enable_irq_ready(r8a66597, pipenum);

	if (!usb_pipeisoc(urb->pipe))
		enable_irq_nrdy(r8a66597, pipenum);
}

static void pipe_irq_disable(struct r8a66597 *r8a66597, u16 pipenum)
{
	disable_irq_ready(r8a66597, pipenum);
	disable_irq_nrdy(r8a66597, pipenum);
}

/* this function must be called with interrupt disabled */
static void r8a66597_usb_preconnect(struct r8a66597 *r8a66597, int port)
{
	r8a66597->root_hub[port].port |= (1 << USB_PORT_FEAT_CONNECTION)
					 | (1 << USB_PORT_FEAT_C_CONNECTION);
	r8a66597_write(r8a66597, ~DTCH, get_intsts_reg(port));
	r8a66597_bset(r8a66597, DTCHE, get_intenb_reg(port));
}

/* this function must be called with interrupt disabled */
static void r8a66597_usb_connect(struct r8a66597 *r8a66597, int port)
{
	u16 speed = get_rh_usb_speed(r8a66597, port);
	struct r8a66597_root_hub *rh = &r8a66597->root_hub[port];

	if (speed == HSMODE)
		rh->port |= (1 << USB_PORT_FEAT_HIGHSPEED);
	else if (speed == LSMODE)
		rh->port |= (1 << USB_PORT_FEAT_LOWSPEED);

	rh->port &= ~(1 << USB_PORT_FEAT_RESET);
	rh->port |= 1 << USB_PORT_FEAT_ENABLE;
}

/* this function must be called with interrupt disabled */
static void r8a66597_usb_disconnect(struct r8a66597 *r8a66597, int port)
{
	struct r8a66597_device *dev = r8a66597->root_hub[port].dev;

	r8a66597->root_hub[port].port &= ~(1 << USB_PORT_FEAT_CONNECTION);
	r8a66597->root_hub[port].port |= (1 << USB_PORT_FEAT_C_CONNECTION);

	disable_r8a66597_pipe_all(r8a66597, dev);
	free_usb_address(r8a66597, dev);

	r8a66597_bset(r8a66597, ATTCHE, get_intenb_reg(port));
}

/* this function must be called with interrupt disabled */
static void prepare_setup_packet(struct r8a66597 *r8a66597,
				 struct r8a66597_td *td)
{
	int i;
	u16 *p = (u16 *)td->urb->setup_packet;
	unsigned long setup_addr = USBREQ;

	r8a66597_write(r8a66597, make_devsel(td->address) | td->maxpacket,
		       DCPMAXP);
	r8a66597_write(r8a66597, ~(SIGN | SACK), INTSTS1);

	for (i = 0; i < 4; i++) {
		r8a66597_write(r8a66597, p[i], setup_addr);
		setup_addr += 2;
	}
	r8a66597_write(r8a66597, SUREQ, DCPCTR);
}

/* this function must be called with interrupt disabled */
static void prepare_packet_read(struct r8a66597 *r8a66597,
				struct r8a66597_td *td)
{
	struct urb *urb = td->urb;

	if (usb_pipecontrol(urb->pipe)) {
		r8a66597_bclr(r8a66597, R8A66597_DIR, DCPCFG);
		r8a66597_mdfy(r8a66597, 0, ISEL | CURPIPE, CFIFOSEL);
		r8a66597_reg_wait(r8a66597, CFIFOSEL, CURPIPE, 0);
		if (urb->actual_length == 0) {
			r8a66597_pipe_toggle(r8a66597, td->pipe, 1);
			r8a66597_write(r8a66597, BCLR, CFIFOCTR);
		}
		pipe_irq_disable(r8a66597, td->pipenum);
		pipe_start(r8a66597, td->pipe);
		pipe_irq_enable(r8a66597, urb, td->pipenum);
	} else {
		if (urb->actual_length == 0) {
			pipe_irq_disable(r8a66597, td->pipenum);
			pipe_setting(r8a66597, td);
			pipe_stop(r8a66597, td->pipe);
			r8a66597_write(r8a66597, ~(1 << td->pipenum), BRDYSTS);

			if (td->pipe->pipetre) {
				r8a66597_write(r8a66597, TRCLR,
						td->pipe->pipetre);
				r8a66597_write(r8a66597,
						(urb->transfer_buffer_length
						+ td->maxpacket - 1)
						/ td->maxpacket,
						td->pipe->pipetrn);
				r8a66597_bset(r8a66597, TRENB,
						td->pipe->pipetre);
			}

			pipe_start(r8a66597, td->pipe);
			pipe_irq_enable(r8a66597, urb, td->pipenum);
		}
	}
}

/* this function must be called with interrupt disabled */
static void prepare_packet_write(struct r8a66597 *r8a66597,
				 struct r8a66597_td *td)
{
	u16 tmp;
	struct urb *urb = td->urb;

	if (usb_pipecontrol(urb->pipe)) {
		pipe_stop(r8a66597, td->pipe);
		r8a66597_bset(r8a66597, R8A66597_DIR, DCPCFG);
		r8a66597_mdfy(r8a66597, ISEL, ISEL | CURPIPE, CFIFOSEL);
		r8a66597_reg_wait(r8a66597, CFIFOSEL, CURPIPE, 0);
		if (urb->actual_length == 0) {
			r8a66597_pipe_toggle(r8a66597, td->pipe, 1);
			r8a66597_write(r8a66597, BCLR, CFIFOCTR);
		}
	} else {
		if (urb->actual_length == 0)
			pipe_setting(r8a66597, td);
		if (td->pipe->pipetre)
			r8a66597_bclr(r8a66597, TRENB, td->pipe->pipetre);
	}
	r8a66597_write(r8a66597, ~(1 << td->pipenum), BRDYSTS);

	fifo_change_from_pipe(r8a66597, td->pipe);
	tmp = r8a66597_read(r8a66597, td->pipe->fifoctr);
	if (unlikely((tmp & FRDY) == 0))
		pipe_irq_enable(r8a66597, urb, td->pipenum);
	else
		packet_write(r8a66597, td->pipenum);
	pipe_start(r8a66597, td->pipe);
}

/* this function must be called with interrupt disabled */
static void prepare_status_packet(struct r8a66597 *r8a66597,
				  struct r8a66597_td *td)
{
	struct urb *urb = td->urb;

	r8a66597_pipe_toggle(r8a66597, td->pipe, 1);
	pipe_stop(r8a66597, td->pipe);

	if (urb->setup_packet[0] & USB_ENDPOINT_DIR_MASK) {
		r8a66597_bset(r8a66597, R8A66597_DIR, DCPCFG);
		r8a66597_mdfy(r8a66597, ISEL, ISEL | CURPIPE, CFIFOSEL);
		r8a66597_reg_wait(r8a66597, CFIFOSEL, CURPIPE, 0);
		r8a66597_write(r8a66597, ~BEMP0, BEMPSTS);
		r8a66597_write(r8a66597, BCLR, CFIFOCTR);
		r8a66597_write(r8a66597, BVAL, CFIFOCTR);
		enable_irq_empty(r8a66597, 0);
	} else {
		r8a66597_bclr(r8a66597, R8A66597_DIR, DCPCFG);
		r8a66597_mdfy(r8a66597, 0, ISEL | CURPIPE, CFIFOSEL);
		r8a66597_reg_wait(r8a66597, CFIFOSEL, CURPIPE, 0);
		r8a66597_write(r8a66597, BCLR, CFIFOCTR);
		enable_irq_ready(r8a66597, 0);
	}
	enable_irq_nrdy(r8a66597, 0);
	pipe_start(r8a66597, td->pipe);
}

/* this function must be called with interrupt disabled */
static int start_transfer(struct r8a66597 *r8a66597, struct r8a66597_td *td)
{
	BUG_ON(!td);

	switch (td->type) {
	case USB_PID_SETUP:
		if (td->urb->setup_packet[1] == USB_REQ_SET_ADDRESS) {
			td->set_address = 1;
			td->urb->setup_packet[2] = alloc_usb_address(r8a66597,
								     td->urb);
			if (td->urb->setup_packet[2] == 0)
				return -EPIPE;
		}
		prepare_setup_packet(r8a66597, td);
		break;
	case USB_PID_IN:
		prepare_packet_read(r8a66597, td);
		break;
	case USB_PID_OUT:
		prepare_packet_write(r8a66597, td);
		break;
	case USB_PID_ACK:
		prepare_status_packet(r8a66597, td);
		break;
	default:
		err("invalid type.");
		break;
	}

	return 0;
}

static int check_transfer_finish(struct r8a66597_td *td, struct urb *urb)
{
	if (usb_pipeisoc(urb->pipe)) {
		if (urb->number_of_packets == td->iso_cnt)
			return 1;
	}

	/* control or bulk or interrupt */
	if ((urb->transfer_buffer_length <= urb->actual_length) ||
	    (td->short_packet) || (td->zero_packet))
		return 1;

	return 0;
}

/* this function must be called with interrupt disabled */
static void set_td_timer(struct r8a66597 *r8a66597, struct r8a66597_td *td)
{
	unsigned long time;

	BUG_ON(!td);

	if (!list_empty(&r8a66597->pipe_queue[td->pipenum]) &&
	    !usb_pipecontrol(td->urb->pipe) && usb_pipein(td->urb->pipe)) {
		r8a66597->timeout_map |= 1 << td->pipenum;
		switch (usb_pipetype(td->urb->pipe)) {
		case PIPE_INTERRUPT:
		case PIPE_ISOCHRONOUS:
			time = 30;
			break;
		default:
			time = 300;
			break;
		}

		mod_timer(&r8a66597->td_timer[td->pipenum],
			  jiffies + msecs_to_jiffies(time));
	}
}

/* this function must be called with interrupt disabled */
static void done(struct r8a66597 *r8a66597, struct r8a66597_td *td,
		 u16 pipenum, struct urb *urb)
{
	int restart = 0;
	struct usb_hcd *hcd = r8a66597_to_hcd(r8a66597);

	r8a66597->timeout_map &= ~(1 << pipenum);

	if (likely(td)) {
		if (td->set_address && urb->status != 0)
			r8a66597->address_map &= ~(1 << urb->setup_packet[2]);

		pipe_toggle_save(r8a66597, td->pipe, urb);
		list_del(&td->queue);
		kfree(td);
	}

	if (!list_empty(&r8a66597->pipe_queue[pipenum]))
		restart = 1;

	if (likely(urb)) {
		if (usb_pipeisoc(urb->pipe))
			urb->start_frame = r8a66597_get_frame(hcd);

		urb->hcpriv = NULL;
		spin_unlock(&r8a66597->lock);
		usb_hcd_giveback_urb(hcd, urb);
		spin_lock(&r8a66597->lock);
	}

	if (restart) {
		td = r8a66597_get_td(r8a66597, pipenum);
		if (unlikely(!td))
			return;

		start_transfer(r8a66597, td);
		set_td_timer(r8a66597, td);
	}
}

/* this function must be called with interrupt disabled */
static void finish_request(struct r8a66597 *r8a66597, struct r8a66597_td *td,
			   u16 pipenum, struct urb *urb)
__releases(r8a66597->lock) __acquires(r8a66597->lock)
{
	done(r8a66597, td, pipenum, urb);
}

static void packet_read(struct r8a66597 *r8a66597, u16 pipenum)
{
	u16 tmp;
	int rcv_len, bufsize, urb_len, size;
	u16 *buf;
	struct r8a66597_td *td = r8a66597_get_td(r8a66597, pipenum);
	struct urb *urb;
	int finish = 0;

	if (unlikely(!td))
		return;
	urb = td->urb;

	fifo_change_from_pipe(r8a66597, td->pipe);
	tmp = r8a66597_read(r8a66597, td->pipe->fifoctr);
	if (unlikely((tmp & FRDY) == 0)) {
		urb->status = -EPIPE;
		pipe_stop(r8a66597, td->pipe);
		pipe_irq_disable(r8a66597, pipenum);
		err("in fifo not ready (%d)", pipenum);
		finish_request(r8a66597, td, pipenum, td->urb);
		return;
	}

	/* prepare parameters */
	rcv_len = tmp & DTLN;
	bufsize = td->maxpacket;
	if (usb_pipeisoc(urb->pipe)) {
		buf = (u16 *)(urb->transfer_buffer +
				urb->iso_frame_desc[td->iso_cnt].offset);
		urb_len = urb->iso_frame_desc[td->iso_cnt].length;
	} else {
		buf = (void *)urb->transfer_buffer + urb->actual_length;
		urb_len = urb->transfer_buffer_length - urb->actual_length;
	}
	if (rcv_len < bufsize)
		size = min(rcv_len, urb_len);
	else
		size = min(bufsize, urb_len);

	/* update parameters */
	urb->actual_length += size;
	if (rcv_len == 0)
		td->zero_packet = 1;
	if ((size % td->maxpacket) > 0) {
		td->short_packet = 1;
		if (urb->transfer_buffer_length != urb->actual_length &&
		    urb->transfer_flags & URB_SHORT_NOT_OK)
			td->urb->status = -EREMOTEIO;
	}
	if (usb_pipeisoc(urb->pipe)) {
		urb->iso_frame_desc[td->iso_cnt].actual_length = size;
		urb->iso_frame_desc[td->iso_cnt].status = 0;
		td->iso_cnt++;
	}

	/* check transfer finish */
	if (check_transfer_finish(td, urb)) {
		pipe_stop(r8a66597, td->pipe);
		pipe_irq_disable(r8a66597, pipenum);
		finish = 1;
	}

	/* read fifo */
	if (urb->transfer_buffer) {
		if (size == 0)
			r8a66597_write(r8a66597, BCLR, td->pipe->fifoctr);
		else
			r8a66597_read_fifo(r8a66597, td->pipe->fifoaddr,
					   buf, size);
	}

	if (finish && pipenum != 0) {
		if (td->urb->status == -EINPROGRESS)
			td->urb->status = 0;
		finish_request(r8a66597, td, pipenum, urb);
	}
}

static void packet_write(struct r8a66597 *r8a66597, u16 pipenum)
{
	u16 tmp;
	int bufsize, size;
	u16 *buf;
	struct r8a66597_td *td = r8a66597_get_td(r8a66597, pipenum);
	struct urb *urb;

	if (unlikely(!td))
		return;
	urb = td->urb;

	fifo_change_from_pipe(r8a66597, td->pipe);
	tmp = r8a66597_read(r8a66597, td->pipe->fifoctr);
	if (unlikely((tmp & FRDY) == 0)) {
		urb->status = -EPIPE;
		pipe_stop(r8a66597, td->pipe);
		pipe_irq_disable(r8a66597, pipenum);
		err("out write fifo not ready. (%d)", pipenum);
		finish_request(r8a66597, td, pipenum, td->urb);
		return;
	}

	/* prepare parameters */
	bufsize = td->maxpacket;
	if (usb_pipeisoc(urb->pipe)) {
		buf = (u16 *)(urb->transfer_buffer +
				urb->iso_frame_desc[td->iso_cnt].offset);
		size = min(bufsize,
			   (int)urb->iso_frame_desc[td->iso_cnt].length);
	} else {
		buf = (u16 *)(urb->transfer_buffer + urb->actual_length);
		size = min((int)bufsize,
			   urb->transfer_buffer_length - urb->actual_length);
	}

	/* write fifo */
	if (pipenum > 0)
		r8a66597_write(r8a66597, ~(1 << pipenum), BEMPSTS);
	if (urb->transfer_buffer) {
		r8a66597_write_fifo(r8a66597, td->pipe->fifoaddr, buf, size);
		if (!usb_pipebulk(urb->pipe) || td->maxpacket != size)
			r8a66597_write(r8a66597, BVAL, td->pipe->fifoctr);
	}

	/* update parameters */
	urb->actual_length += size;
	if (usb_pipeisoc(urb->pipe)) {
		urb->iso_frame_desc[td->iso_cnt].actual_length = size;
		urb->iso_frame_desc[td->iso_cnt].status = 0;
		td->iso_cnt++;
	}

	/* check transfer finish */
	if (check_transfer_finish(td, urb)) {
		disable_irq_ready(r8a66597, pipenum);
		enable_irq_empty(r8a66597, pipenum);
		if (!usb_pipeisoc(urb->pipe))
			enable_irq_nrdy(r8a66597, pipenum);
	} else
		pipe_irq_enable(r8a66597, urb, pipenum);
}


static void check_next_phase(struct r8a66597 *r8a66597)
{
	struct r8a66597_td *td = r8a66597_get_td(r8a66597, 0);
	struct urb *urb;
	u8 finish = 0;

	if (unlikely(!td))
		return;
	urb = td->urb;

	switch (td->type) {
	case USB_PID_IN:
	case USB_PID_OUT:
		if (urb->status != -EINPROGRESS) {
			finish = 1;
			break;
		}
		if (check_transfer_finish(td, urb))
			td->type = USB_PID_ACK;
		break;
	case USB_PID_SETUP:
		if (urb->status != -EINPROGRESS)
			finish = 1;
		else if (urb->transfer_buffer_length == urb->actual_length) {
			td->type = USB_PID_ACK;
			urb->status = 0;
		} else if (usb_pipeout(urb->pipe))
			td->type = USB_PID_OUT;
		else
			td->type = USB_PID_IN;
		break;
	case USB_PID_ACK:
		finish = 1;
		if (urb->status == -EINPROGRESS)
			urb->status = 0;
		break;
	}

	if (finish)
		finish_request(r8a66597, td, 0, urb);
	else
		start_transfer(r8a66597, td);
}

static void set_urb_error(struct r8a66597 *r8a66597, u16 pipenum)
{
	struct r8a66597_td *td = r8a66597_get_td(r8a66597, pipenum);

	if (td && td->urb) {
		u16 pid = r8a66597_read(r8a66597, td->pipe->pipectr) & PID;

		if (pid == PID_NAK)
			td->urb->status = -ECONNRESET;
		else
			td->urb->status = -EPIPE;
	}
}

static void irq_pipe_ready(struct r8a66597 *r8a66597)
{
	u16 check;
	u16 pipenum;
	u16 mask;
	struct r8a66597_td *td;

	mask = r8a66597_read(r8a66597, BRDYSTS)
	       & r8a66597_read(r8a66597, BRDYENB);
	r8a66597_write(r8a66597, ~mask, BRDYSTS);
	if (mask & BRDY0) {
		td = r8a66597_get_td(r8a66597, 0);
		if (td && td->type == USB_PID_IN)
			packet_read(r8a66597, 0);
		else
			pipe_irq_disable(r8a66597, 0);
		check_next_phase(r8a66597);
	}

	for (pipenum = 1; pipenum < R8A66597_MAX_NUM_PIPE; pipenum++) {
		check = 1 << pipenum;
		if (mask & check) {
			td = r8a66597_get_td(r8a66597, pipenum);
			if (unlikely(!td))
				continue;

			if (td->type == USB_PID_IN)
				packet_read(r8a66597, pipenum);
			else if (td->type == USB_PID_OUT)
				packet_write(r8a66597, pipenum);
		}
	}
}

static void irq_pipe_empty(struct r8a66597 *r8a66597)
{
	u16 tmp;
	u16 check;
	u16 pipenum;
	u16 mask;
	struct r8a66597_td *td;

	mask = r8a66597_read(r8a66597, BEMPSTS)
	       & r8a66597_read(r8a66597, BEMPENB);
	r8a66597_write(r8a66597, ~mask, BEMPSTS);
	if (mask & BEMP0) {
		cfifo_change(r8a66597, 0);
		td = r8a66597_get_td(r8a66597, 0);
		if (td && td->type != USB_PID_OUT)
			disable_irq_empty(r8a66597, 0);
		check_next_phase(r8a66597);
	}

	for (pipenum = 1; pipenum < R8A66597_MAX_NUM_PIPE; pipenum++) {
		check = 1 << pipenum;
		if (mask &  check) {
			struct r8a66597_td *td;
			td = r8a66597_get_td(r8a66597, pipenum);
			if (unlikely(!td))
				continue;

			tmp = r8a66597_read(r8a66597, td->pipe->pipectr);
			if ((tmp & INBUFM) == 0) {
				disable_irq_empty(r8a66597, pipenum);
				pipe_irq_disable(r8a66597, pipenum);
				if (td->urb->status == -EINPROGRESS)
					td->urb->status = 0;
				finish_request(r8a66597, td, pipenum, td->urb);
			}
		}
	}
}

static void irq_pipe_nrdy(struct r8a66597 *r8a66597)
{
	u16 check;
	u16 pipenum;
	u16 mask;

	mask = r8a66597_read(r8a66597, NRDYSTS)
	       & r8a66597_read(r8a66597, NRDYENB);
	r8a66597_write(r8a66597, ~mask, NRDYSTS);
	if (mask & NRDY0) {
		cfifo_change(r8a66597, 0);
		set_urb_error(r8a66597, 0);
		pipe_irq_disable(r8a66597, 0);
		check_next_phase(r8a66597);
	}

	for (pipenum = 1; pipenum < R8A66597_MAX_NUM_PIPE; pipenum++) {
		check = 1 << pipenum;
		if (mask & check) {
			struct r8a66597_td *td;
			td = r8a66597_get_td(r8a66597, pipenum);
			if (unlikely(!td))
				continue;

			set_urb_error(r8a66597, pipenum);
			pipe_irq_disable(r8a66597, pipenum);
			pipe_stop(r8a66597, td->pipe);
			finish_request(r8a66597, td, pipenum, td->urb);
		}
	}
}

static void start_root_hub_sampling(struct r8a66597 *r8a66597, int port)
{
	struct r8a66597_root_hub *rh = &r8a66597->root_hub[port];

	rh->old_syssts = r8a66597_read(r8a66597, get_syssts_reg(port)) & LNST;
	rh->scount = R8A66597_MAX_SAMPLING;
	mod_timer(&r8a66597->rh_timer, jiffies + msecs_to_jiffies(50));
}

static irqreturn_t r8a66597_irq(struct usb_hcd *hcd)
{
	struct r8a66597 *r8a66597 = hcd_to_r8a66597(hcd);
	u16 intsts0, intsts1, intsts2;
	u16 intenb0, intenb1, intenb2;
	u16 mask0, mask1, mask2;

	spin_lock(&r8a66597->lock);

	intsts0 = r8a66597_read(r8a66597, INTSTS0);
	intsts1 = r8a66597_read(r8a66597, INTSTS1);
	intsts2 = r8a66597_read(r8a66597, INTSTS2);
	intenb0 = r8a66597_read(r8a66597, INTENB0);
	intenb1 = r8a66597_read(r8a66597, INTENB1);
	intenb2 = r8a66597_read(r8a66597, INTENB2);

	mask2 = intsts2 & intenb2;
	mask1 = intsts1 & intenb1;
	mask0 = intsts0 & intenb0 & (BEMP | NRDY | BRDY);
	if (mask2) {
		if (mask2 & ATTCH) {
			r8a66597_write(r8a66597, ~ATTCH, INTSTS2);
			r8a66597_bclr(r8a66597, ATTCHE, INTENB2);

			/* start usb bus sampling */
			start_root_hub_sampling(r8a66597, 1);
		}
		if (mask2 & DTCH) {
			r8a66597_write(r8a66597, ~DTCH, INTSTS2);
			r8a66597_bclr(r8a66597, DTCHE, INTENB2);
			r8a66597_usb_disconnect(r8a66597, 1);
		}
	}

	if (mask1) {
		if (mask1 & ATTCH) {
			r8a66597_write(r8a66597, ~ATTCH, INTSTS1);
			r8a66597_bclr(r8a66597, ATTCHE, INTENB1);

			/* start usb bus sampling */
			start_root_hub_sampling(r8a66597, 0);
		}
		if (mask1 & DTCH) {
			r8a66597_write(r8a66597, ~DTCH, INTSTS1);
			r8a66597_bclr(r8a66597, DTCHE, INTENB1);
			r8a66597_usb_disconnect(r8a66597, 0);
		}
		if (mask1 & SIGN) {
			r8a66597_write(r8a66597, ~SIGN, INTSTS1);
			set_urb_error(r8a66597, 0);
			check_next_phase(r8a66597);
		}
		if (mask1 & SACK) {
			r8a66597_write(r8a66597, ~SACK, INTSTS1);
			check_next_phase(r8a66597);
		}
	}
	if (mask0) {
		if (mask0 & BRDY)
			irq_pipe_ready(r8a66597);
		if (mask0 & BEMP)
			irq_pipe_empty(r8a66597);
		if (mask0 & NRDY)
			irq_pipe_nrdy(r8a66597);
	}

	spin_unlock(&r8a66597->lock);
	return IRQ_HANDLED;
}

/* this function must be called with interrupt disabled */
static void r8a66597_root_hub_control(struct r8a66597 *r8a66597, int port)
{
	u16 tmp;
	struct r8a66597_root_hub *rh = &r8a66597->root_hub[port];

	if (rh->port & (1 << USB_PORT_FEAT_RESET)) {
		unsigned long dvstctr_reg = get_dvstctr_reg(port);

		tmp = r8a66597_read(r8a66597, dvstctr_reg);
		if ((tmp & USBRST) == USBRST) {
			r8a66597_mdfy(r8a66597, UACT, USBRST | UACT,
				      dvstctr_reg);
			mod_timer(&r8a66597->rh_timer,
				  jiffies + msecs_to_jiffies(50));
		} else
			r8a66597_usb_connect(r8a66597, port);
	}

	if (rh->scount > 0) {
		tmp = r8a66597_read(r8a66597, get_syssts_reg(port)) & LNST;
		if (tmp == rh->old_syssts) {
			rh->scount--;
			if (rh->scount == 0) {
				if (tmp == FS_JSTS) {
					r8a66597_bset(r8a66597, HSE,
						      get_syscfg_reg(port));
					r8a66597_usb_preconnect(r8a66597, port);
				} else if (tmp == LS_JSTS) {
					r8a66597_bclr(r8a66597, HSE,
						      get_syscfg_reg(port));
					r8a66597_usb_preconnect(r8a66597, port);
				} else if (tmp == SE0)
					r8a66597_bset(r8a66597, ATTCHE,
						      get_intenb_reg(port));
			} else {
				mod_timer(&r8a66597->rh_timer,
					  jiffies + msecs_to_jiffies(50));
			}
		} else {
			rh->scount = R8A66597_MAX_SAMPLING;
			rh->old_syssts = tmp;
			mod_timer(&r8a66597->rh_timer,
				  jiffies + msecs_to_jiffies(50));
		}
	}
}

static void r8a66597_td_timer(unsigned long _r8a66597)
{
	struct r8a66597 *r8a66597 = (struct r8a66597 *)_r8a66597;
	unsigned long flags;
	u16 pipenum;
	struct r8a66597_td *td, *new_td = NULL;
	struct r8a66597_pipe *pipe;

	spin_lock_irqsave(&r8a66597->lock, flags);
	for (pipenum = 0; pipenum < R8A66597_MAX_NUM_PIPE; pipenum++) {
		if (!(r8a66597->timeout_map & (1 << pipenum)))
			continue;
		if (timer_pending(&r8a66597->td_timer[pipenum]))
			continue;

		td = r8a66597_get_td(r8a66597, pipenum);
		if (!td) {
			r8a66597->timeout_map &= ~(1 << pipenum);
			continue;
		}

		if (td->urb->actual_length) {
			set_td_timer(r8a66597, td);
			break;
		}

		pipe = td->pipe;
		pipe_stop(r8a66597, pipe);

		new_td = td;
		do {
			list_move_tail(&new_td->queue,
				       &r8a66597->pipe_queue[pipenum]);
			new_td = r8a66597_get_td(r8a66597, pipenum);
			if (!new_td) {
				new_td = td;
				break;
			}
		} while (td != new_td && td->address == new_td->address);

		start_transfer(r8a66597, new_td);

		if (td == new_td)
			r8a66597->timeout_map &= ~(1 << pipenum);
		else
			set_td_timer(r8a66597, new_td);
		break;
	}
	spin_unlock_irqrestore(&r8a66597->lock, flags);
}

static void r8a66597_timer(unsigned long _r8a66597)
{
	struct r8a66597 *r8a66597 = (struct r8a66597 *)_r8a66597;
	unsigned long flags;

	spin_lock_irqsave(&r8a66597->lock, flags);

	r8a66597_root_hub_control(r8a66597, 0);
	r8a66597_root_hub_control(r8a66597, 1);

	spin_unlock_irqrestore(&r8a66597->lock, flags);
}

static int check_pipe_config(struct r8a66597 *r8a66597, struct urb *urb)
{
	struct r8a66597_device *dev = get_urb_to_r8a66597_dev(r8a66597, urb);

	if (dev && dev->address && dev->state != USB_STATE_CONFIGURED &&
	    (urb->dev->state == USB_STATE_CONFIGURED))
		return 1;
	else
		return 0;
}

static int r8a66597_start(struct usb_hcd *hcd)
{
	struct r8a66597 *r8a66597 = hcd_to_r8a66597(hcd);

	hcd->state = HC_STATE_RUNNING;
	return enable_controller(r8a66597);
}

static void r8a66597_stop(struct usb_hcd *hcd)
{
	struct r8a66597 *r8a66597 = hcd_to_r8a66597(hcd);

	disable_controller(r8a66597);
}

static void set_address_zero(struct r8a66597 *r8a66597, struct urb *urb)
{
	unsigned int usb_address = usb_pipedevice(urb->pipe);
	u16 root_port, hub_port;

	if (usb_address == 0) {
		get_port_number(urb->dev->devpath,
				&root_port, &hub_port);
		set_devadd_reg(r8a66597, 0,
			       get_r8a66597_usb_speed(urb->dev->speed),
			       get_parent_r8a66597_address(r8a66597, urb->dev),
			       hub_port, root_port);
	}
}

static struct r8a66597_td *r8a66597_make_td(struct r8a66597 *r8a66597,
					    struct urb *urb,
					    struct usb_host_endpoint *hep)
{
	struct r8a66597_td *td;
	u16 pipenum;

	td = kzalloc(sizeof(struct r8a66597_td), GFP_ATOMIC);
	if (td == NULL)
		return NULL;

	pipenum = r8a66597_get_pipenum(urb, hep);
	td->pipenum = pipenum;
	td->pipe = hep->hcpriv;
	td->urb = urb;
	td->address = get_urb_to_r8a66597_addr(r8a66597, urb);
	td->maxpacket = usb_maxpacket(urb->dev, urb->pipe,
				      !usb_pipein(urb->pipe));
	if (usb_pipecontrol(urb->pipe))
		td->type = USB_PID_SETUP;
	else if (usb_pipein(urb->pipe))
		td->type = USB_PID_IN;
	else
		td->type = USB_PID_OUT;
	INIT_LIST_HEAD(&td->queue);

	return td;
}

static int r8a66597_urb_enqueue(struct usb_hcd *hcd,
				struct usb_host_endpoint *hep,
				struct urb *urb,
				gfp_t mem_flags)
{
	struct r8a66597 *r8a66597 = hcd_to_r8a66597(hcd);
	struct r8a66597_td *td = NULL;
	int ret = 0, request = 0;
	unsigned long flags;

	spin_lock_irqsave(&r8a66597->lock, flags);
	if (!get_urb_to_r8a66597_dev(r8a66597, urb)) {
		ret = -ENODEV;
		goto error;
	}

	if (!hep->hcpriv) {
		hep->hcpriv = kzalloc(sizeof(struct r8a66597_pipe),
				GFP_ATOMIC);
		if (!hep->hcpriv) {
			ret = -ENOMEM;
			goto error;
		}
		set_pipe_reg_addr(hep->hcpriv, R8A66597_PIPE_NO_DMA);
		if (usb_pipeendpoint(urb->pipe))
			init_pipe_info(r8a66597, urb, hep, &hep->desc);
	}

	if (unlikely(check_pipe_config(r8a66597, urb)))
		init_pipe_config(r8a66597, urb);

	set_address_zero(r8a66597, urb);
	td = r8a66597_make_td(r8a66597, urb, hep);
	if (td == NULL) {
		ret = -ENOMEM;
		goto error;
	}
	if (list_empty(&r8a66597->pipe_queue[td->pipenum]))
		request = 1;
	list_add_tail(&td->queue, &r8a66597->pipe_queue[td->pipenum]);

	spin_lock(&urb->lock);
	if (urb->status != -EINPROGRESS) {
		spin_unlock(&urb->lock);
		ret = -EPIPE;
		goto error;
	}
	urb->hcpriv = td;
	spin_unlock(&urb->lock);

	if (request) {
		ret = start_transfer(r8a66597, td);
		if (ret < 0) {
			list_del(&td->queue);
			kfree(td);
		}
	} else
		set_td_timer(r8a66597, td);

error:
	spin_unlock_irqrestore(&r8a66597->lock, flags);
	return ret;
}

static int r8a66597_urb_dequeue(struct usb_hcd *hcd, struct urb *urb)
{
	struct r8a66597 *r8a66597 = hcd_to_r8a66597(hcd);
	struct r8a66597_td *td;
	unsigned long flags;

	spin_lock_irqsave(&r8a66597->lock, flags);
	if (urb->hcpriv) {
		td = urb->hcpriv;
		pipe_stop(r8a66597, td->pipe);
		pipe_irq_disable(r8a66597, td->pipenum);
		disable_irq_empty(r8a66597, td->pipenum);
		done(r8a66597, td, td->pipenum, urb);
	}
	spin_unlock_irqrestore(&r8a66597->lock, flags);
	return 0;
}

static void r8a66597_endpoint_disable(struct usb_hcd *hcd,
				      struct usb_host_endpoint *hep)
{
	struct r8a66597 *r8a66597 = hcd_to_r8a66597(hcd);
	struct r8a66597_pipe *pipe = (struct r8a66597_pipe *)hep->hcpriv;
	struct r8a66597_td *td;
	struct urb *urb = NULL;
	u16 pipenum;
	unsigned long flags;

	if (pipe == NULL)
		return;
	pipenum = pipe->info.pipenum;

	if (pipenum == 0) {
		kfree(hep->hcpriv);
		hep->hcpriv = NULL;
		return;
	}

	spin_lock_irqsave(&r8a66597->lock, flags);
	pipe_stop(r8a66597, pipe);
	pipe_irq_disable(r8a66597, pipenum);
	disable_irq_empty(r8a66597, pipenum);
	td = r8a66597_get_td(r8a66597, pipenum);
	if (td)
		urb = td->urb;
	done(r8a66597, td, pipenum, urb);
	kfree(hep->hcpriv);
	hep->hcpriv = NULL;
	spin_unlock_irqrestore(&r8a66597->lock, flags);
}

static int r8a66597_get_frame(struct usb_hcd *hcd)
{
	struct r8a66597 *r8a66597 = hcd_to_r8a66597(hcd);
	return r8a66597_read(r8a66597, FRMNUM) & 0x03FF;
}

static void collect_usb_address_map(struct usb_device *udev, unsigned long *map)
{
	int chix;

	if (udev->state == USB_STATE_CONFIGURED &&
	    udev->parent && udev->parent->devnum > 1 &&
	    udev->parent->descriptor.bDeviceClass == USB_CLASS_HUB)
		map[udev->devnum/32] |= (1 << (udev->devnum % 32));

	for (chix = 0; chix < udev->maxchild; chix++) {
		struct usb_device *childdev = udev->children[chix];

		if (childdev)
			collect_usb_address_map(childdev, map);
	}
}

/* this function must be called with interrupt disabled */
static struct r8a66597_device *get_r8a66597_device(struct r8a66597 *r8a66597,
						   int addr)
{
	struct r8a66597_device *dev;
	struct list_head *list = &r8a66597->child_device;

	list_for_each_entry(dev, list, device_list) {
		if (!dev)
			continue;
		if (dev->usb_address != addr)
			continue;

		return dev;
	}

	err("get_r8a66597_device fail.(%d)\n", addr);
	return NULL;
}

static void update_usb_address_map(struct r8a66597 *r8a66597,
				   struct usb_device *root_hub,
				   unsigned long *map)
{
	int i, j, addr;
	unsigned long diff;
	unsigned long flags;

	for (i = 0; i < 4; i++) {
		diff = r8a66597->child_connect_map[i] ^ map[i];
		if (!diff)
			continue;

		for (j = 0; j < 32; j++) {
			if (!(diff & (1 << j)))
				continue;

			addr = i * 32 + j;
			if (map[i] & (1 << j))
				set_child_connect_map(r8a66597, addr);
			else {
				struct r8a66597_device *dev;

				spin_lock_irqsave(&r8a66597->lock, flags);
				dev = get_r8a66597_device(r8a66597, addr);
				disable_r8a66597_pipe_all(r8a66597, dev);
				free_usb_address(r8a66597, dev);
				put_child_connect_map(r8a66597, addr);
				spin_unlock_irqrestore(&r8a66597->lock, flags);
			}
		}
	}
}

static void r8a66597_check_detect_child(struct r8a66597 *r8a66597,
					struct usb_hcd *hcd)
{
	struct usb_bus *bus;
	unsigned long now_map[4];

	memset(now_map, 0, sizeof(now_map));

	list_for_each_entry(bus, &usb_bus_list, bus_list) {
		if (!bus->root_hub)
			continue;

		if (bus->busnum != hcd->self.busnum)
			continue;

		collect_usb_address_map(bus->root_hub, now_map);
		update_usb_address_map(r8a66597, bus->root_hub, now_map);
	}
}

static int r8a66597_hub_status_data(struct usb_hcd *hcd, char *buf)
{
	struct r8a66597 *r8a66597 = hcd_to_r8a66597(hcd);
	unsigned long flags;
	int i;

	r8a66597_check_detect_child(r8a66597, hcd);

	spin_lock_irqsave(&r8a66597->lock, flags);

	*buf = 0;	/* initialize (no change) */

	for (i = 0; i < R8A66597_MAX_ROOT_HUB; i++) {
		if (r8a66597->root_hub[i].port & 0xffff0000)
			*buf |= 1 << (i + 1);
	}

	spin_unlock_irqrestore(&r8a66597->lock, flags);

	return (*buf != 0);
}

static void r8a66597_hub_descriptor(struct r8a66597 *r8a66597,
				    struct usb_hub_descriptor *desc)
{
	desc->bDescriptorType = 0x29;
	desc->bHubContrCurrent = 0;
	desc->bNbrPorts = R8A66597_MAX_ROOT_HUB;
	desc->bDescLength = 9;
	desc->bPwrOn2PwrGood = 0;
	desc->wHubCharacteristics = cpu_to_le16(0x0011);
	desc->bitmap[0] = ((1 << R8A66597_MAX_ROOT_HUB) - 1) << 1;
	desc->bitmap[1] = ~0;
}

static int r8a66597_hub_control(struct usb_hcd *hcd, u16 typeReq, u16 wValue,
				u16 wIndex, char *buf, u16 wLength)
{
	struct r8a66597 *r8a66597 = hcd_to_r8a66597(hcd);
	int ret;
	int port = (wIndex & 0x00FF) - 1;
	struct r8a66597_root_hub *rh = &r8a66597->root_hub[port];
	unsigned long flags;

	ret = 0;

	spin_lock_irqsave(&r8a66597->lock, flags);
	switch (typeReq) {
	case ClearHubFeature:
	case SetHubFeature:
		switch (wValue) {
		case C_HUB_OVER_CURRENT:
		case C_HUB_LOCAL_POWER:
			break;
		default:
			goto error;
		}
		break;
	case ClearPortFeature:
		if (wIndex > R8A66597_MAX_ROOT_HUB)
			goto error;
		if (wLength != 0)
			goto error;

		switch (wValue) {
		case USB_PORT_FEAT_ENABLE:
			rh->port &= (1 << USB_PORT_FEAT_POWER);
			break;
		case USB_PORT_FEAT_SUSPEND:
			break;
		case USB_PORT_FEAT_POWER:
			r8a66597_port_power(r8a66597, port, 0);
			break;
		case USB_PORT_FEAT_C_ENABLE:
		case USB_PORT_FEAT_C_SUSPEND:
		case USB_PORT_FEAT_C_CONNECTION:
		case USB_PORT_FEAT_C_OVER_CURRENT:
		case USB_PORT_FEAT_C_RESET:
			break;
		default:
			goto error;
		}
		rh->port &= ~(1 << wValue);
		break;
	case GetHubDescriptor:
		r8a66597_hub_descriptor(r8a66597,
					(struct usb_hub_descriptor *)buf);
		break;
	case GetHubStatus:
		*buf = 0x00;
		break;
	case GetPortStatus:
		if (wIndex > R8A66597_MAX_ROOT_HUB)
			goto error;
		*(u32 *)buf = rh->port;
		break;
	case SetPortFeature:
		if (wIndex > R8A66597_MAX_ROOT_HUB)
			goto error;
		if (wLength != 0)
			goto error;

		switch (wValue) {
		case USB_PORT_FEAT_SUSPEND:
			break;
		case USB_PORT_FEAT_POWER:
			r8a66597_port_power(r8a66597, port, 1);
			rh->port |= (1 << USB_PORT_FEAT_POWER);
			break;
		case USB_PORT_FEAT_RESET: {
			struct r8a66597_device *dev = rh->dev;

			rh->port |= (1 << USB_PORT_FEAT_RESET);

			disable_r8a66597_pipe_all(r8a66597, dev);
			free_usb_address(r8a66597, dev);

			r8a66597_mdfy(r8a66597, USBRST, USBRST | UACT,
				      get_dvstctr_reg(port));
			mod_timer(&r8a66597->rh_timer,
				  jiffies + msecs_to_jiffies(50));
			}
			break;
		default:
			goto error;
		}
		rh->port |= 1 << wValue;
		break;
	default:
error:
		ret = -EPIPE;
		break;
	}

	spin_unlock_irqrestore(&r8a66597->lock, flags);
	return ret;
}

static struct hc_driver r8a66597_hc_driver = {
	.description =		hcd_name,
	.hcd_priv_size =	sizeof(struct r8a66597),
	.irq =			r8a66597_irq,

	/*
	 * generic hardware linkage
	 */
	.flags =		HCD_USB2,

	.start =		r8a66597_start,
	.stop =			r8a66597_stop,

	/*
	 * managing i/o requests and associated device resources
	 */
	.urb_enqueue =		r8a66597_urb_enqueue,
	.urb_dequeue =		r8a66597_urb_dequeue,
	.endpoint_disable =	r8a66597_endpoint_disable,

	/*
	 * periodic schedule support
	 */
	.get_frame_number =	r8a66597_get_frame,

	/*
	 * root hub support
	 */
	.hub_status_data =	r8a66597_hub_status_data,
	.hub_control =		r8a66597_hub_control,
};

#if defined(CONFIG_PM)
static int r8a66597_suspend(struct platform_device *pdev, pm_message_t state)
{
	pdev->dev.power.power_state = state;
	return 0;
}

static int r8a66597_resume(struct platform_device *pdev)
{
	pdev->dev.power.power_state = PMSG_ON;
	return 0;
}
#else	/* if defined(CONFIG_PM) */
#define r8a66597_suspend	NULL
#define r8a66597_resume		NULL
#endif

static int __init_or_module r8a66597_remove(struct platform_device *pdev)
{
	struct r8a66597		*r8a66597 = dev_get_drvdata(&pdev->dev);
	struct usb_hcd		*hcd = r8a66597_to_hcd(r8a66597);

	del_timer_sync(&r8a66597->rh_timer);
	iounmap((void *)r8a66597->reg);
	usb_remove_hcd(hcd);
	usb_put_hcd(hcd);
	return 0;
}

#define resource_len(r) (((r)->end - (r)->start) + 1)
static int __init r8a66597_probe(struct platform_device *pdev)
{
	struct resource *res = NULL;
	int irq = -1;
	void __iomem *reg = NULL;
	struct usb_hcd *hcd = NULL;
	struct r8a66597 *r8a66597;
	int ret = 0;
	int i;

	if (pdev->dev.dma_mask) {
		ret = -EINVAL;
		err("dma not support");
		goto clean_up;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
					   (char *)hcd_name);
	if (!res) {
		ret = -ENODEV;
		err("platform_get_resource_byname error.");
		goto clean_up;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = -ENODEV;
		err("platform_get_irq error.");
		goto clean_up;
	}

	reg = ioremap(res->start, resource_len(res));
	if (reg == NULL) {
		ret = -ENOMEM;
		err("ioremap error.");
		goto clean_up;
	}

	/* initialize hcd */
	hcd = usb_create_hcd(&r8a66597_hc_driver, &pdev->dev, (char *)hcd_name);
	if (!hcd) {
		ret = -ENOMEM;
		err("Failed to create hcd");
		goto clean_up;
	}
	r8a66597 = hcd_to_r8a66597(hcd);
	memset(r8a66597, 0, sizeof(struct r8a66597));
	dev_set_drvdata(&pdev->dev, r8a66597);

	spin_lock_init(&r8a66597->lock);
	init_timer(&r8a66597->rh_timer);
	r8a66597->rh_timer.function = r8a66597_timer;
	r8a66597->rh_timer.data = (unsigned long)r8a66597;
	r8a66597->reg = (unsigned long)reg;

	for (i = 0; i < R8A66597_MAX_NUM_PIPE; i++) {
		INIT_LIST_HEAD(&r8a66597->pipe_queue[i]);
		init_timer(&r8a66597->td_timer[i]);
		r8a66597->td_timer[i].function = r8a66597_td_timer;
		r8a66597->td_timer[i].data = (unsigned long)r8a66597;
	}
	INIT_LIST_HEAD(&r8a66597->child_device);

	hcd->rsrc_start = res->start;
	ret = usb_add_hcd(hcd, irq, 0);
	if (ret != 0) {
		err("Failed to add hcd");
		goto clean_up;
	}

	return 0;

clean_up:
	if (reg)
		iounmap(reg);

	return ret;
}

static struct platform_driver r8a66597_driver = {
	.probe =	r8a66597_probe,
	.remove =	r8a66597_remove,
	.suspend =	r8a66597_suspend,
	.resume =	r8a66597_resume,
	.driver		= {
		.name = (char *) hcd_name,
	},
};

static int __init r8a66597_init(void)
{
	if (usb_disabled())
		return -ENODEV;

	info("driver %s, %s", hcd_name, DRIVER_VERSION);
	return platform_driver_register(&r8a66597_driver);
}
module_init(r8a66597_init);

static void __exit r8a66597_cleanup(void)
{
	platform_driver_unregister(&r8a66597_driver);
}
module_exit(r8a66597_cleanup);

