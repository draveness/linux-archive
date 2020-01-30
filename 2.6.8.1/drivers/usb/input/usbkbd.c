/*
 * $Id: usbkbd.c,v 1.27 2001/12/27 10:37:41 vojtech Exp $
 *
 *  Copyright (c) 1999-2001 Vojtech Pavlik
 *
 *  USB HIDBP Keyboard support
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
 * 
 * Should you need to contact me, the author, you can do so either by
 * e-mail - mail your message to <vojtech@ucw.cz>, or by paper mail:
 * Vojtech Pavlik, Simunkova 1594, Prague 8, 182 00 Czech Republic
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/init.h>
#include <linux/usb.h>

/*
 * Version Information
 */
#define DRIVER_VERSION ""
#define DRIVER_AUTHOR "Vojtech Pavlik <vojtech@ucw.cz>"
#define DRIVER_DESC "USB HID Boot Protocol keyboard driver"
#define DRIVER_LICENSE "GPL"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE(DRIVER_LICENSE);

static unsigned char usb_kbd_keycode[256] = {
	  0,  0,  0,  0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
	 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,  2,  3,
	  4,  5,  6,  7,  8,  9, 10, 11, 28,  1, 14, 15, 57, 12, 13, 26,
	 27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
	 65, 66, 67, 68, 87, 88, 99, 70,119,110,102,104,111,107,109,106,
	105,108,103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
	 72, 73, 82, 83, 86,127,116,117,183,184,185,186,187,188,189,190,
	191,192,193,194,134,138,130,132,128,129,131,137,133,135,136,113,
	115,114,  0,  0,  0,121,  0, 89, 93,124, 92, 94, 95,  0,  0,  0,
	122,123, 90, 91, 85,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	 29, 42, 56,125, 97, 54,100,126,164,166,165,163,161,115,114,113,
	150,158,159,128,136,177,178,176,142,152,173,140
};

struct usb_kbd {
	struct input_dev dev;
	struct usb_device *usbdev;
	unsigned char old[8];
	struct urb *irq, *led;
	unsigned char newleds;
	char name[128];
	char phys[64];
	int open;

	unsigned char *new;
	struct usb_ctrlrequest *cr;
	unsigned char *leds;
	dma_addr_t cr_dma;
	dma_addr_t new_dma;
	dma_addr_t leds_dma;
};

static void usb_kbd_irq(struct urb *urb, struct pt_regs *regs)
{
	struct usb_kbd *kbd = urb->context;
	int i;

	switch (urb->status) {
	case 0:			/* success */
		break;
	case -ECONNRESET:	/* unlink */
	case -ENOENT:
	case -ESHUTDOWN:
		return;
	/* -EPIPE:  should clear the halt */
	default:		/* error */
		goto resubmit;
	}

	input_regs(&kbd->dev, regs);

	for (i = 0; i < 8; i++)
		input_report_key(&kbd->dev, usb_kbd_keycode[i + 224], (kbd->new[0] >> i) & 1);

	for (i = 2; i < 8; i++) {

		if (kbd->old[i] > 3 && memscan(kbd->new + 2, kbd->old[i], 6) == kbd->new + 8) {
			if (usb_kbd_keycode[kbd->old[i]])
				input_report_key(&kbd->dev, usb_kbd_keycode[kbd->old[i]], 0);
			else
				info("Unknown key (scancode %#x) released.", kbd->old[i]);
		}

		if (kbd->new[i] > 3 && memscan(kbd->old + 2, kbd->new[i], 6) == kbd->old + 8) {
			if (usb_kbd_keycode[kbd->new[i]])
				input_report_key(&kbd->dev, usb_kbd_keycode[kbd->new[i]], 1);
			else
				info("Unknown key (scancode %#x) pressed.", kbd->new[i]);
		}
	}

	input_sync(&kbd->dev);

	memcpy(kbd->old, kbd->new, 8);

resubmit:
	i = usb_submit_urb (urb, SLAB_ATOMIC);
	if (i)
		err ("can't resubmit intr, %s-%s/input0, status %d",
				kbd->usbdev->bus->bus_name,
				kbd->usbdev->devpath, i);
}

int usb_kbd_event(struct input_dev *dev, unsigned int type, unsigned int code, int value)
{
	struct usb_kbd *kbd = dev->private;

	if (type != EV_LED)
		return -1;


	kbd->newleds = (!!test_bit(LED_KANA,    dev->led) << 3) | (!!test_bit(LED_COMPOSE, dev->led) << 3) |
		       (!!test_bit(LED_SCROLLL, dev->led) << 2) | (!!test_bit(LED_CAPSL,   dev->led) << 1) |
		       (!!test_bit(LED_NUML,    dev->led));

	if (kbd->led->status == -EINPROGRESS)
		return 0;

	if (*(kbd->leds) == kbd->newleds)
		return 0;

	*(kbd->leds) = kbd->newleds;
	kbd->led->dev = kbd->usbdev;
	if (usb_submit_urb(kbd->led, GFP_ATOMIC))
		err("usb_submit_urb(leds) failed");

	return 0;
}

static void usb_kbd_led(struct urb *urb, struct pt_regs *regs)
{
	struct usb_kbd *kbd = urb->context;

	if (urb->status)
		warn("led urb status %d received", urb->status);
	
	if (*(kbd->leds) == kbd->newleds)
		return;

	*(kbd->leds) = kbd->newleds;
	kbd->led->dev = kbd->usbdev;
	if (usb_submit_urb(kbd->led, GFP_ATOMIC))
		err("usb_submit_urb(leds) failed");
}

static int usb_kbd_open(struct input_dev *dev)
{
	struct usb_kbd *kbd = dev->private;

	if (kbd->open++)
		return 0;

	kbd->irq->dev = kbd->usbdev;
	if (usb_submit_urb(kbd->irq, GFP_KERNEL)) {
		kbd->open--;
		return -EIO;
	}

	return 0;
}

static void usb_kbd_close(struct input_dev *dev)
{
	struct usb_kbd *kbd = dev->private;

	if (!--kbd->open)
		usb_unlink_urb(kbd->irq);
}

static int usb_kbd_alloc_mem(struct usb_device *dev, struct usb_kbd *kbd)
{
	if (!(kbd->irq = usb_alloc_urb(0, GFP_KERNEL)))
		return -1;
	if (!(kbd->led = usb_alloc_urb(0, GFP_KERNEL)))
		return -1;
	if (!(kbd->new = usb_buffer_alloc(dev, 8, SLAB_ATOMIC, &kbd->new_dma)))
		return -1;
	if (!(kbd->cr = usb_buffer_alloc(dev, sizeof(struct usb_ctrlrequest), SLAB_ATOMIC, &kbd->cr_dma)))
		return -1;
	if (!(kbd->leds = usb_buffer_alloc(dev, 1, SLAB_ATOMIC, &kbd->leds_dma)))
		return -1;

	return 0;
}

static void usb_kbd_free_mem(struct usb_device *dev, struct usb_kbd *kbd)
{
	if (kbd->irq)
		usb_free_urb(kbd->irq);
	if (kbd->led)
		usb_free_urb(kbd->led);
	if (kbd->new)
		usb_buffer_free(dev, 8, kbd->new, kbd->new_dma);
	if (kbd->cr)
		usb_buffer_free(dev, sizeof(struct usb_ctrlrequest), kbd->cr, kbd->cr_dma);
	if (kbd->leds)
		usb_buffer_free(dev, 1, kbd->leds, kbd->leds_dma);
}

static int usb_kbd_probe(struct usb_interface *iface, 
			 const struct usb_device_id *id)
{
	struct usb_device * dev = interface_to_usbdev(iface);
	struct usb_host_interface *interface;
	struct usb_endpoint_descriptor *endpoint;
	struct usb_kbd *kbd;
	int i, pipe, maxp;
	char path[64];
	char *buf;

	interface = iface->cur_altsetting;

	if (interface->desc.bNumEndpoints != 1)
		return -ENODEV;

	endpoint = &interface->endpoint[0].desc;
	if (!(endpoint->bEndpointAddress & 0x80))
		return -ENODEV;
	if ((endpoint->bmAttributes & 3) != 3)
		return -ENODEV;

	pipe = usb_rcvintpipe(dev, endpoint->bEndpointAddress);
	maxp = usb_maxpacket(dev, pipe, usb_pipeout(pipe));

	if (!(kbd = kmalloc(sizeof(struct usb_kbd), GFP_KERNEL)))
		return -ENOMEM;
	memset(kbd, 0, sizeof(struct usb_kbd));

	if (usb_kbd_alloc_mem(dev, kbd)) {
		usb_kbd_free_mem(dev, kbd);
		kfree(kbd);
		return -ENOMEM;
	}

	kbd->usbdev = dev;

	kbd->dev.evbit[0] = BIT(EV_KEY) | BIT(EV_LED) | BIT(EV_REP);
	kbd->dev.ledbit[0] = BIT(LED_NUML) | BIT(LED_CAPSL) | BIT(LED_SCROLLL) | BIT(LED_COMPOSE) | BIT(LED_KANA);

	for (i = 0; i < 255; i++)
		set_bit(usb_kbd_keycode[i], kbd->dev.keybit);
	clear_bit(0, kbd->dev.keybit);
	
	kbd->dev.private = kbd;
	kbd->dev.event = usb_kbd_event;
	kbd->dev.open = usb_kbd_open;
	kbd->dev.close = usb_kbd_close;

	usb_fill_int_urb(kbd->irq, dev, pipe,
			 kbd->new, (maxp > 8 ? 8 : maxp),
			 usb_kbd_irq, kbd, endpoint->bInterval);
	kbd->irq->transfer_dma = kbd->new_dma;
	kbd->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	kbd->cr->bRequestType = USB_TYPE_CLASS | USB_RECIP_INTERFACE;
	kbd->cr->bRequest = 0x09;
	kbd->cr->wValue = cpu_to_le16(0x200);
	kbd->cr->wIndex = cpu_to_le16(interface->desc.bInterfaceNumber);
	kbd->cr->wLength = cpu_to_le16(1);

	usb_make_path(dev, path, 64);
	sprintf(kbd->phys, "%s/input0", path);

	kbd->dev.name = kbd->name;
	kbd->dev.phys = kbd->phys;	
	kbd->dev.id.bustype = BUS_USB;
	kbd->dev.id.vendor = dev->descriptor.idVendor;
	kbd->dev.id.product = dev->descriptor.idProduct;
	kbd->dev.id.version = dev->descriptor.bcdDevice;
	kbd->dev.dev = &iface->dev;

	if (!(buf = kmalloc(63, GFP_KERNEL))) {
		usb_free_urb(kbd->irq);
		usb_kbd_free_mem(dev, kbd);
		kfree(kbd);
		return -ENOMEM;
	}

	if (dev->descriptor.iManufacturer &&
		usb_string(dev, dev->descriptor.iManufacturer, buf, 63) > 0)
			strcat(kbd->name, buf);
	if (dev->descriptor.iProduct &&
		usb_string(dev, dev->descriptor.iProduct, buf, 63) > 0)
			sprintf(kbd->name, "%s %s", kbd->name, buf);

	if (!strlen(kbd->name))
		sprintf(kbd->name, "USB HIDBP Keyboard %04x:%04x",
			kbd->dev.id.vendor, kbd->dev.id.product);

	kfree(buf);

	usb_fill_control_urb(kbd->led, dev, usb_sndctrlpipe(dev, 0),
			     (void *) kbd->cr, kbd->leds, 1,
			     usb_kbd_led, kbd);
	kbd->led->setup_dma = kbd->cr_dma;
	kbd->led->transfer_dma = kbd->leds_dma;
	kbd->led->transfer_flags |= (URB_NO_TRANSFER_DMA_MAP
				| URB_NO_SETUP_DMA_MAP);

	input_register_device(&kbd->dev);

	printk(KERN_INFO "input: %s on %s\n", kbd->name, path);

	usb_set_intfdata(iface, kbd);
	return 0;
}

static void usb_kbd_disconnect(struct usb_interface *intf)
{
	struct usb_kbd *kbd = usb_get_intfdata (intf);
	
	usb_set_intfdata(intf, NULL);
	if (kbd) {
		usb_unlink_urb(kbd->irq);
		input_unregister_device(&kbd->dev);
		usb_kbd_free_mem(interface_to_usbdev(intf), kbd);
		kfree(kbd);
	}
}

static struct usb_device_id usb_kbd_id_table [] = {
	{ USB_INTERFACE_INFO(3, 1, 1) },
	{ }						/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, usb_kbd_id_table);

static struct usb_driver usb_kbd_driver = {
	.owner =	THIS_MODULE,
	.name =		"usbkbd",
	.probe =	usb_kbd_probe,
	.disconnect =	usb_kbd_disconnect,
	.id_table =	usb_kbd_id_table,
};

static int __init usb_kbd_init(void)
{
	int result = usb_register(&usb_kbd_driver);
	if (result == 0)
		info(DRIVER_VERSION ":" DRIVER_DESC);
	return result;
}

static void __exit usb_kbd_exit(void)
{
	usb_deregister(&usb_kbd_driver);
}

module_init(usb_kbd_init);
module_exit(usb_kbd_exit);
