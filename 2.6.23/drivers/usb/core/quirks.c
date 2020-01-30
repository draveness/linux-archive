/*
 * USB device quirk handling logic and table
 *
 * Copyright (c) 2007 Oliver Neukum
 * Copyright (c) 2007 Greg Kroah-Hartman <gregkh@suse.de>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, version 2.
 *
 *
 */

#include <linux/usb.h>
#include <linux/usb/quirks.h>
#include "usb.h"

/* List of quirky USB devices.  Please keep this list ordered by:
 * 	1) Vendor ID
 * 	2) Product ID
 * 	3) Class ID
 *
 * as we want specific devices to be overridden first, and only after that, any
 * class specific quirks.
 *
 * Right now the logic aborts if it finds a valid device in the table, we might
 * want to change that in the future if it turns out that a whole class of
 * devices is broken...
 */
static const struct usb_device_id usb_quirk_list[] = {
	/* CBM - Flash disk */
	{ USB_DEVICE(0x0204, 0x6025), .driver_info = USB_QUIRK_RESET_RESUME },
	/* HP 5300/5370C scanner */
	{ USB_DEVICE(0x03f0, 0x0701), .driver_info = USB_QUIRK_STRING_FETCH_255 },
	/* Hewlett-Packard PhotoSmart 720 / PhotoSmart 935 (storage) */
	{ USB_DEVICE(0x03f0, 0x4002), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },

	/* SGS Thomson Microelectronics 4in1 card reader */
	{ USB_DEVICE(0x0483, 0x0321), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },

	/* Acer Peripherals Inc. (now BenQ Corp.) Prisa 640BU */
	{ USB_DEVICE(0x04a5, 0x207e), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* Benq S2W 3300U */
	{ USB_DEVICE(0x04a5, 0x20b0), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* Canon, Inc. CanoScan N1240U/LiDE30 */
	{ USB_DEVICE(0x04a9, 0x220e), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* Canon, Inc. CanoScan N650U/N656U */
	{ USB_DEVICE(0x04a9, 0x2206), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* Canon, Inc. CanoScan 1220U */
	{ USB_DEVICE(0x04a9, 0x2207), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* Canon, Inc. CanoScan N670U/N676U/LiDE 20 */
	{ USB_DEVICE(0x04a9, 0x220d), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* old Cannon scanner */
	{ USB_DEVICE(0x04a9, 0x2220), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* Seiko Epson Corp. Perfection 1200 */
	{ USB_DEVICE(0x04b8, 0x0104), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* Seiko Epson Corp. Perfection 660 */
	{ USB_DEVICE(0x04b8, 0x0114), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* Epson Perfection 1260 Photo */
	{ USB_DEVICE(0x04b8, 0x011d), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* Seiko Epson Corp - Perfection 1670 */
	{ USB_DEVICE(0x04b8, 0x011f), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* EPSON Perfection 2480 */
	{ USB_DEVICE(0x04b8, 0x0121), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* Seiko Epson Corp.*/
	{ USB_DEVICE(0x04b8, 0x0122), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* Samsung ML-2010 printer */
	{ USB_DEVICE(0x04e8, 0x326c), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* Samsung ML-2510 Series printer */
	{ USB_DEVICE(0x04e8, 0x327e), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* Elsa MicroLink 56k (V.250) */
	{ USB_DEVICE(0x05cc, 0x2267), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* Ultima Electronics Corp.*/
	{ USB_DEVICE(0x05d8, 0x4005), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },

	/* Genesys USB-to-IDE */
	{ USB_DEVICE(0x0503, 0x0702), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },

	/* USB Graphical LCD - EEH Datalink GmbH */
	{ USB_DEVICE(0x060c, 0x04eb), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },

	/* INTEL VALUE SSD */
	{ USB_DEVICE(0x8086, 0xf1a5), .driver_info = USB_QUIRK_RESET_RESUME },

	/* M-Systems Flash Disk Pioneers */
	{ USB_DEVICE(0x08ec, 0x1000), .driver_info = USB_QUIRK_RESET_RESUME },

	/* Agfa Snapscan1212u */
	{ USB_DEVICE(0x06bd, 0x2061), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* Seagate RSS LLC */
	{ USB_DEVICE(0x0bc2, 0x3000), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	/* Umax [hex] Astra 3400U */
	{ USB_DEVICE(0x1606, 0x0060), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },

	/* Philips PSC805 audio device */
	{ USB_DEVICE(0x0471, 0x0155), .driver_info = USB_QUIRK_RESET_RESUME },

	/* Alcor multi-card reader */
	{ USB_DEVICE(0x058f, 0x6366), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },

	/* Canon EOS 5D in PC Connection mode */
	{ USB_DEVICE(0x04a9, 0x3101), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },

	/* RIM Blackberry */
	{ USB_DEVICE(0x0fca, 0x0001), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	{ USB_DEVICE(0x0fca, 0x0004), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },
	{ USB_DEVICE(0x0fca, 0x0006), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },

	/* Apple iPhone */
	{ USB_DEVICE(0x05ac, 0x1290), .driver_info = USB_QUIRK_NO_AUTOSUSPEND },

	/* SKYMEDI USB_DRIVE */
	{ USB_DEVICE(0x1516, 0x8628), .driver_info = USB_QUIRK_RESET_RESUME },

	{ }  /* terminating entry must be last */
};

static void usb_autosuspend_quirk(struct usb_device *udev)
{
#ifdef	CONFIG_USB_SUSPEND
	/* disable autosuspend, but allow the user to re-enable it via sysfs */
	udev->autosuspend_disabled = 1;
#endif
}

static const struct usb_device_id *find_id(struct usb_device *udev)
{
	const struct usb_device_id *id = usb_quirk_list;

	for (; id->idVendor || id->bDeviceClass || id->bInterfaceClass ||
			id->driver_info; id++) {
		if (usb_match_device(udev, id))
			return id;
	}
	return NULL;
}

/*
 * Detect any quirks the device has, and do any housekeeping for it if needed.
 */
void usb_detect_quirks(struct usb_device *udev)
{
	const struct usb_device_id *id = usb_quirk_list;

	id = find_id(udev);
	if (id)
		udev->quirks = (u32)(id->driver_info);
	if (udev->quirks)
		dev_dbg(&udev->dev, "USB quirks for this device: %x\n",
				udev->quirks);

	/* do any special quirk handling here if needed */
	if (udev->quirks & USB_QUIRK_NO_AUTOSUSPEND)
		usb_autosuspend_quirk(udev);

	/* By default, disable autosuspend for all non-hubs */
#ifdef	CONFIG_USB_SUSPEND
	if (udev->descriptor.bDeviceClass != USB_CLASS_HUB)
		udev->autosuspend_delay = -1;
#endif
}
