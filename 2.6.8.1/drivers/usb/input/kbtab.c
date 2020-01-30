#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <asm/unaligned.h>
#include <asm/byteorder.h>

/*
 * Version Information
 * v0.0.1 - Original, extremely basic version, 2.4.xx only
 * v0.0.2 - Updated, works with 2.5.62 and 2.4.20;
 *           - added pressure-threshold modules param code from
 *              Alex Perry <alex.perry@ieee.org>
 */

#define DRIVER_VERSION "v0.0.2"
#define DRIVER_AUTHOR "Josh Myer <josh@joshisanerd.com>"
#define DRIVER_DESC "USB KB Gear JamStudio Tablet driver"
#define DRIVER_LICENSE "GPL"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE(DRIVER_LICENSE);

#define USB_VENDOR_ID_KBGEAR	0x084e

static int       kb_pressure_click = 0x10;
MODULE_PARM     (kb_pressure_click,"i");
MODULE_PARM_DESC(kb_pressure_click,
		 "pressure threshold for clicks");

struct kbtab {
	signed char *data;
	dma_addr_t data_dma;
	struct input_dev dev;
	struct usb_device *usbdev;
	struct urb *irq;
	int open;
	int x, y;
	int button;
	int pressure;
	__u32 serial[2];
	char phys[32];
};

static void kbtab_irq(struct urb *urb, struct pt_regs *regs)
{
	struct kbtab *kbtab = urb->context;
	unsigned char *data = kbtab->data;
	struct input_dev *dev = &kbtab->dev;
	int retval;

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

	kbtab->x = le16_to_cpu(get_unaligned((u16 *) &data[1]));
	kbtab->y = le16_to_cpu(get_unaligned((u16 *) &data[3]));

	kbtab->pressure = (data[5]);

	input_report_key(dev, BTN_TOOL_PEN, 1);

	input_report_abs(dev, ABS_X, kbtab->x);
	input_report_abs(dev, ABS_Y, kbtab->y);

	/*input_report_key(dev, BTN_TOUCH , data[0] & 0x01);*/
	input_report_key(dev, BTN_RIGHT, data[0] & 0x02);

	if( -1 == kb_pressure_click){ 
		input_report_abs(dev, ABS_PRESSURE, kbtab->pressure);
	} else {
		input_report_key(dev, BTN_LEFT, (kbtab->pressure > kb_pressure_click) ? 1 : 0);
	};
	
	input_sync(dev);

 exit:
	retval = usb_submit_urb (urb, GFP_ATOMIC);
	if (retval)
		err ("%s - usb_submit_urb failed with result %d",
		     __FUNCTION__, retval);
}

static struct usb_device_id kbtab_ids[] = {
	{ USB_DEVICE(USB_VENDOR_ID_KBGEAR, 0x1001), .driver_info = 0 },
	{ }
};

MODULE_DEVICE_TABLE(usb, kbtab_ids);

static int kbtab_open(struct input_dev *dev)
{
	struct kbtab *kbtab = dev->private;

	if (kbtab->open++)
		return 0;

	kbtab->irq->dev = kbtab->usbdev;
	if (usb_submit_urb(kbtab->irq, GFP_KERNEL)) {
		kbtab->open--;
		return -EIO;
	}

	return 0;
}

static void kbtab_close(struct input_dev *dev)
{
	struct kbtab *kbtab = dev->private;

	if (!--kbtab->open)
		usb_unlink_urb(kbtab->irq);
}

static int kbtab_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct usb_endpoint_descriptor *endpoint;
	struct kbtab *kbtab;
	char path[64];

	if (!(kbtab = kmalloc(sizeof(struct kbtab), GFP_KERNEL)))
		return -ENOMEM;
	memset(kbtab, 0, sizeof(struct kbtab));

	kbtab->data = usb_buffer_alloc(dev, 8, GFP_KERNEL, &kbtab->data_dma);
	if (!kbtab->data) {
		kfree(kbtab);
		return -ENOMEM;
	}

	kbtab->irq = usb_alloc_urb(0, GFP_KERNEL);
	if (!kbtab->irq) {
		usb_buffer_free(dev, 10, kbtab->data, kbtab->data_dma);
		kfree(kbtab);
		return -ENOMEM;
	}

	kbtab->dev.evbit[0] |= BIT(EV_KEY) | BIT(EV_ABS) | BIT(EV_MSC);
	kbtab->dev.absbit[0] |= BIT(ABS_X) | BIT(ABS_Y) | BIT(ABS_PRESSURE);

	kbtab->dev.keybit[LONG(BTN_LEFT)] |= BIT(BTN_LEFT) | BIT(BTN_RIGHT) | BIT(BTN_MIDDLE);

	kbtab->dev.keybit[LONG(BTN_DIGI)] |= BIT(BTN_TOOL_PEN) | BIT(BTN_TOUCH);

	kbtab->dev.mscbit[0] |= BIT(MSC_SERIAL);

	kbtab->dev.absmax[ABS_X] = 0x2000;
	kbtab->dev.absmax[ABS_Y] = 0x1750;
	kbtab->dev.absmax[ABS_PRESSURE] = 0xff;
	
	kbtab->dev.absfuzz[ABS_X] = 4;
	kbtab->dev.absfuzz[ABS_Y] = 4;

	kbtab->dev.private = kbtab;
	kbtab->dev.open = kbtab_open;
	kbtab->dev.close = kbtab_close;

	usb_make_path(dev, path, 64);
	sprintf(kbtab->phys, "%s/input0", path);

	kbtab->dev.name = "KB Gear Tablet";
	kbtab->dev.phys = kbtab->phys;
	kbtab->dev.id.bustype = BUS_USB;
	kbtab->dev.id.vendor = dev->descriptor.idVendor;
	kbtab->dev.id.product = dev->descriptor.idProduct;
	kbtab->dev.id.version = dev->descriptor.bcdDevice;
	kbtab->dev.dev = &intf->dev;
	kbtab->usbdev = dev;

	endpoint = &intf->cur_altsetting->endpoint[0].desc;

	usb_fill_int_urb(kbtab->irq, dev,
			 usb_rcvintpipe(dev, endpoint->bEndpointAddress),
			 kbtab->data, 8,
			 kbtab_irq, kbtab, endpoint->bInterval);
	kbtab->irq->transfer_dma = kbtab->data_dma;
	kbtab->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	input_register_device(&kbtab->dev);

	printk(KERN_INFO "input: KB Gear Tablet on %s\n",  path);

	usb_set_intfdata(intf, kbtab);

	return 0;
}

static void kbtab_disconnect(struct usb_interface *intf)
{
	struct kbtab *kbtab = usb_get_intfdata (intf);

	usb_set_intfdata(intf, NULL);
	if (kbtab) {
		usb_unlink_urb(kbtab->irq);
		input_unregister_device(&kbtab->dev);
		usb_free_urb(kbtab->irq);
		usb_buffer_free(interface_to_usbdev(intf), 10, kbtab->data, kbtab->data_dma);
		kfree(kbtab);
	}
}

static struct usb_driver kbtab_driver = {
	.owner =	THIS_MODULE,
	.name =		"kbtab",
	.probe =	kbtab_probe,
	.disconnect =	kbtab_disconnect,
	.id_table =	kbtab_ids,
};

static int __init kbtab_init(void)
{
	int retval;
	retval = usb_register(&kbtab_driver);
	if (retval)
		goto out;
	info(DRIVER_VERSION ":" DRIVER_DESC);
out:
	return retval;
}

static void __exit kbtab_exit(void)
{
	usb_deregister(&kbtab_driver);
}

module_init(kbtab_init);
module_exit(kbtab_exit);
