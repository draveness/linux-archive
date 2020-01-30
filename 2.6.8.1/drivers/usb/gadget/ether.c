/*
 * ether.c -- Ethernet gadget driver, with CDC and non-CDC options
 *
 * Copyright (C) 2003-2004 David Brownell
 * Copyright (C) 2003-2004 Robert Schwebel, Benedikt Spranger
 *
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
 */


// #define DEBUG 1
// #define VERBOSE

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/list.h>
#include <linux/interrupt.h>
#include <linux/uts.h>
#include <linux/version.h>
#include <linux/device.h>
#include <linux/moduleparam.h>
#include <linux/ctype.h>

#include <asm/byteorder.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/unaligned.h>

#include <linux/usb_ch9.h>
#include <linux/usb_gadget.h>

#include <linux/random.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>

#include "gadget_chips.h"

/*-------------------------------------------------------------------------*/

/*
 * Ethernet gadget driver -- with CDC and non-CDC options
 *
 * CDC Ethernet is the standard USB solution for sending Ethernet frames
 * using USB.  Real hardware tends to use the same framing protocol but look
 * different for control features.  This driver strongly prefers to use
 * this USB-IF standard as its open-systems interoperability solution;
 * most host side USB stacks (except from Microsoft) support it.
 *
 * There's some hardware that can't talk CDC.  We make that hardware
 * implement a "minimalist" vendor-agnostic CDC core:  same framing, but
 * link-level setup only requires activating the configuration.
 * Linux supports it, but other host operating systems may not.
 * (This is a subset of CDC Ethernet.)
 *
 * A third option is also in use.  Rather than CDC Ethernet, or something
 * simpler, Microsoft pushes their own approach: RNDIS.  The published
 * RNDIS specs are ambiguous and appear to be incomplete, and are also
 * needlessly complex.
 */

#define DRIVER_DESC		"Ethernet Gadget"
#define DRIVER_VERSION		"St Patrick's Day 2004"

static const char shortname [] = "ether";
static const char driver_desc [] = DRIVER_DESC;

#define RX_EXTRA	20		/* guard against rx overflows */

#ifdef	CONFIG_USB_ETH_RNDIS
#include "rndis.h"
#else
#define rndis_init() 0
#define rndis_exit() do{}while(0)
#endif

/*-------------------------------------------------------------------------*/

struct eth_dev {
	spinlock_t		lock;
	struct usb_gadget	*gadget;
	struct usb_request	*req;		/* for control responses */

	u8			config;
	struct usb_ep		*in_ep, *out_ep, *status_ep;
	const struct usb_endpoint_descriptor
				*in, *out, *status;
	struct list_head	tx_reqs, rx_reqs;

	struct net_device	*net;
	struct net_device_stats	stats;
	atomic_t		tx_qlen;

	struct work_struct	work;
	unsigned		zlp:1;
	unsigned		cdc:1;
	unsigned		rndis:1;
	unsigned		suspended:1;
	u16			cdc_filter;
	unsigned long		todo;
#define	WORK_RX_MEMORY		0
	int			rndis_config;
	u8			host_mac [ETH_ALEN];
};

/* This version autoconfigures as much as possible at run-time.
 *
 * It also ASSUMES a self-powered device, without remote wakeup,
 * although remote wakeup support would make sense.
 */
static const char *EP_IN_NAME;
static const char *EP_OUT_NAME;
static const char *EP_STATUS_NAME;

/*-------------------------------------------------------------------------*/

/* DO NOT REUSE THESE IDs with a protocol-incompatible driver!!  Ever!!
 * Instead:  allocate your own, using normal USB-IF procedures.
 */

/* Thanks to NetChip Technologies for donating this product ID.
 * It's for devices with only CDC Ethernet configurations.
 */
#define CDC_VENDOR_NUM	0x0525		/* NetChip */
#define CDC_PRODUCT_NUM	0xa4a1		/* Linux-USB Ethernet Gadget */

/* For hardware that can't talk CDC, we use the same vendor ID that
 * ARM Linux has used for ethernet-over-usb, both with sa1100 and
 * with pxa250.  We're protocol-compatible, if the host-side drivers
 * use the endpoint descriptors.  bcdDevice (version) is nonzero, so
 * drivers that need to hard-wire endpoint numbers have a hook.
 *
 * The protocol is a minimal subset of CDC Ether, which works on any bulk
 * hardware that's not deeply broken ... even on hardware that can't talk
 * RNDIS (like SA-1100, with no interrupt endpoint, or anything that
 * doesn't handle control-OUT).
 */
#define	SIMPLE_VENDOR_NUM	0x049f
#define	SIMPLE_PRODUCT_NUM	0x505a

/* For hardware that can talk RNDIS and either of the above protocols,
 * use this ID ... the windows INF files will know it.  Unless it's
 * used with CDC Ethernet, Linux 2.4 hosts will need updates to choose
 * the non-RNDIS configuration.
 */
#define RNDIS_VENDOR_NUM	0x0525	/* NetChip */
#define RNDIS_PRODUCT_NUM	0xa4a2	/* Ethernet/RNDIS Gadget */


/* Some systems will want different product identifers published in the
 * device descriptor, either numbers or strings or both.  These string
 * parameters are in UTF-8 (superset of ASCII's 7 bit characters).
 */

static ushort __initdata idVendor;
module_param(idVendor, ushort, S_IRUGO);
MODULE_PARM_DESC(idVendor, "USB Vendor ID");

static ushort __initdata idProduct;
module_param(idProduct, ushort, S_IRUGO);
MODULE_PARM_DESC(idProduct, "USB Product ID");

static ushort __initdata bcdDevice;
module_param(bcdDevice, ushort, S_IRUGO);
MODULE_PARM_DESC(bcdDevice, "USB Device version (BCD)");

static char *__initdata iManufacturer;
module_param(iManufacturer, charp, S_IRUGO);
MODULE_PARM_DESC(iManufacturer, "USB Manufacturer string");

static char *__initdata iProduct;
module_param(iProduct, charp, S_IRUGO);
MODULE_PARM_DESC(iProduct, "USB Product string");

/* initial value, changed by "ifconfig usb0 hw ether xx:xx:xx:xx:xx:xx" */
static char *__initdata dev_addr;
module_param(dev_addr, charp, S_IRUGO);
MODULE_PARM_DESC(iProduct, "Device Ethernet Address");

/* this address is invisible to ifconfig */
static char *__initdata host_addr;
module_param(host_addr, charp, S_IRUGO);
MODULE_PARM_DESC(host_addr, "Host Ethernet Address");


/*-------------------------------------------------------------------------*/

/* Include CDC support if we could run on CDC-capable hardware. */

#ifdef CONFIG_USB_GADGET_NET2280
#define	DEV_CONFIG_CDC
#endif

#ifdef CONFIG_USB_GADGET_DUMMY_HCD
#define	DEV_CONFIG_CDC
#endif

#ifdef CONFIG_USB_GADGET_GOKU
#define	DEV_CONFIG_CDC
#endif

#ifdef CONFIG_USB_GADGET_MQ11XX
#define	DEV_CONFIG_CDC
#endif

#ifdef CONFIG_USB_GADGET_OMAP
#define	DEV_CONFIG_CDC
#endif


/* For CDC-incapable hardware, choose the simple cdc subset.
 * Anything that talks bulk (without notable bugs) can do this.
 */
#ifdef CONFIG_USB_GADGET_PXA2XX
#define	DEV_CONFIG_SUBSET
#endif

#ifdef CONFIG_USB_GADGET_SH
#define	DEV_CONFIG_SUBSET
#endif

#ifdef CONFIG_USB_GADGET_SA1100
/* use non-CDC for backwards compatibility */
#define	DEV_CONFIG_SUBSET
#endif


/*-------------------------------------------------------------------------*/

#define DEFAULT_QLEN	2	/* double buffering by default */

#ifdef CONFIG_USB_GADGET_DUALSPEED

static unsigned qmult = 5;
module_param (qmult, uint, S_IRUGO|S_IWUSR);


/* for dual-speed hardware, use deeper queues at highspeed */
#define qlen(gadget) \
	(DEFAULT_QLEN*((gadget->speed == USB_SPEED_HIGH) ? qmult : 1))

/* also defer IRQs on highspeed TX */
#define TX_DELAY	qmult

#define	BITRATE(g) ((g->speed == USB_SPEED_HIGH) ? 4800000 : 120000)

#else	/* full speed (low speed doesn't do bulk) */
#define qlen(gadget) DEFAULT_QLEN

#define	BITRATE(g)	(12000)
#endif


/*-------------------------------------------------------------------------*/

#define xprintk(d,level,fmt,args...) \
	printk(level "%s: " fmt , (d)->net->name , ## args)

#ifdef DEBUG
#undef DEBUG
#define DEBUG(dev,fmt,args...) \
	xprintk(dev , KERN_DEBUG , fmt , ## args)
#else
#define DEBUG(dev,fmt,args...) \
	do { } while (0)
#endif /* DEBUG */

#ifdef VERBOSE
#define VDEBUG	DEBUG
#else
#define VDEBUG(dev,fmt,args...) \
	do { } while (0)
#endif /* DEBUG */

#define ERROR(dev,fmt,args...) \
	xprintk(dev , KERN_ERR , fmt , ## args)
#define WARN(dev,fmt,args...) \
	xprintk(dev , KERN_WARNING , fmt , ## args)
#define INFO(dev,fmt,args...) \
	xprintk(dev , KERN_INFO , fmt , ## args)

/*-------------------------------------------------------------------------*/

/* USB DRIVER HOOKUP (to the hardware driver, below us), mostly
 * ep0 implementation:  descriptors, config management, setup().
 * also optional class-specific notification interrupt transfer.
 */

/*
 * DESCRIPTORS ... most are static, but strings and (full) configuration
 * descriptors are built on demand.  For now we do either full CDC, or
 * our simple subset, with RNDIS as an optional second configuration.
 *
 * RNDIS includes some CDC ACM descriptors ... like CDC Ethernet.  But
 * the class descriptors match a modem (they're ignored; it's really just
 * Ethernet functionality), they don't need the NOP altsetting, and the
 * status transfer endpoint isn't optional.
 */

#define STRING_MANUFACTURER		1
#define STRING_PRODUCT			2
#define STRING_ETHADDR			3
#define STRING_DATA			4
#define STRING_CONTROL			5
#define STRING_RNDIS_CONTROL		6
#define STRING_CDC			7
#define STRING_SUBSET			8
#define STRING_RNDIS			9

#define USB_BUFSIZ	256		/* holds our biggest descriptor */

/*
 * This device advertises one configuration, eth_config, unless RNDIS
 * is enabled (rndis_config) on hardware supporting at least two configs.
 *
 * NOTE:  Controllers like superh_udc should probably be able to use
 * an RNDIS-only configuration.
 *
 * FIXME define some higher-powered configurations to make it easier
 * to recharge batteries ...
 */

#define DEV_CONFIG_VALUE	1	/* cdc or subset */
#define DEV_RNDIS_CONFIG_VALUE	2	/* rndis; optional */

static struct usb_device_descriptor
device_desc = {
	.bLength =		sizeof device_desc,
	.bDescriptorType =	USB_DT_DEVICE,

	.bcdUSB =		__constant_cpu_to_le16 (0x0200),

	.bDeviceClass =		USB_CLASS_COMM,
	.bDeviceSubClass =	0,
	.bDeviceProtocol =	0,

	.idVendor =		__constant_cpu_to_le16 (CDC_VENDOR_NUM),
	.idProduct =		__constant_cpu_to_le16 (CDC_PRODUCT_NUM),
	.iManufacturer =	STRING_MANUFACTURER,
	.iProduct =		STRING_PRODUCT,
	.bNumConfigurations =	1,
};

static struct usb_otg_descriptor
otg_descriptor = {
	.bLength =		sizeof otg_descriptor,
	.bDescriptorType =	USB_DT_OTG,

	.bmAttributes =		USB_OTG_SRP,
};

static struct usb_config_descriptor
eth_config = {
	.bLength =		sizeof eth_config,
	.bDescriptorType =	USB_DT_CONFIG,

	/* compute wTotalLength on the fly */
	.bNumInterfaces =	2,
	.bConfigurationValue =	DEV_CONFIG_VALUE,
	.iConfiguration =	STRING_CDC,
	.bmAttributes =		USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
	.bMaxPower =		1,
};

#ifdef	CONFIG_USB_ETH_RNDIS
static struct usb_config_descriptor 
rndis_config = {
	.bLength =              sizeof rndis_config,
	.bDescriptorType =      USB_DT_CONFIG,

	/* compute wTotalLength on the fly */
	.bNumInterfaces =       2,
	.bConfigurationValue =  DEV_RNDIS_CONFIG_VALUE,
	.iConfiguration =       STRING_RNDIS,
	.bmAttributes =		USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
	.bMaxPower =            1,
};
#endif

/*
 * Compared to the simple CDC subset, the full CDC Ethernet model adds
 * three class descriptors, two interface descriptors, optional status
 * endpoint.  Both have a "data" interface and two bulk endpoints.
 * There are also differences in how control requests are handled.
 *
 * RNDIS shares a lot with CDC-Ethernet, since it's a variant of
 * the CDC-ACM (modem) spec.
 */

#ifdef	DEV_CONFIG_CDC
static struct usb_interface_descriptor
control_intf = {
	.bLength =		sizeof control_intf,
	.bDescriptorType =	USB_DT_INTERFACE,

	.bInterfaceNumber =	0,
	/* status endpoint is optional; this may be patched later */
	.bNumEndpoints =	1,
	.bInterfaceClass =	USB_CLASS_COMM,
	.bInterfaceSubClass =	6,	/* ethernet control model */
	.bInterfaceProtocol =	0,
	.iInterface =		STRING_CONTROL,
};
#endif

#ifdef	CONFIG_USB_ETH_RNDIS
static const struct usb_interface_descriptor
rndis_control_intf = {
	.bLength =              sizeof rndis_control_intf,
	.bDescriptorType =      USB_DT_INTERFACE,
	  
	.bInterfaceNumber =     0,
	.bNumEndpoints =        1,
	.bInterfaceClass =      USB_CLASS_COMM,
	.bInterfaceSubClass =   2,	/* abstract control model */
	.bInterfaceProtocol =   0xff,	/* vendor specific */
	.iInterface =           STRING_RNDIS_CONTROL,
};
#endif

#if defined(DEV_CONFIG_CDC) || defined(CONFIG_USB_ETH_RNDIS)

/* "Header Functional Descriptor" from CDC spec  5.2.3.1 */
struct header_desc {
	u8	bLength;
	u8	bDescriptorType;
	u8	bDescriptorSubType;

	u16	bcdCDC;
} __attribute__ ((packed));

static const struct header_desc header_desc = {
	.bLength =		sizeof header_desc,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	0,

	.bcdCDC =		__constant_cpu_to_le16 (0x0110),
};

/* "Union Functional Descriptor" from CDC spec 5.2.3.8 */
struct union_desc {
	u8	bLength;
	u8	bDescriptorType;
	u8	bDescriptorSubType;

	u8	bMasterInterface0;
	u8	bSlaveInterface0;
	/* ... and there could be other slave interfaces */
} __attribute__ ((packed));

static const struct union_desc union_desc = {
	.bLength =		sizeof union_desc,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	6,

	.bMasterInterface0 =	0,	/* index of control interface */
	.bSlaveInterface0 =	1,	/* index of DATA interface */
};

#endif	/* CDC || RNDIS */

#ifdef	CONFIG_USB_ETH_RNDIS

/* "Call Management Descriptor" from CDC spec  5.2.3.3 */
struct call_mgmt_descriptor {
	u8  bLength;
	u8  bDescriptorType;
	u8  bDescriptorSubType;

	u8  bmCapabilities;
	u8  bDataInterface;
} __attribute__ ((packed));

static const struct call_mgmt_descriptor call_mgmt_descriptor = {
	.bLength =  		sizeof call_mgmt_descriptor,
	.bDescriptorType = 	USB_DT_CS_INTERFACE,
	.bDescriptorSubType = 	0x01,

	.bmCapabilities = 	0x00,
	.bDataInterface = 	0x01,
};


/* "Abstract Control Management Descriptor" from CDC spec  5.2.3.4 */
struct acm_descriptor {
	u8  bLength;
	u8  bDescriptorType;
	u8  bDescriptorSubType;

	u8  bmCapabilities;
} __attribute__ ((packed));

static struct acm_descriptor acm_descriptor = {
	.bLength =  		sizeof acm_descriptor,
	.bDescriptorType = 	USB_DT_CS_INTERFACE,
	.bDescriptorSubType = 	0x02,

	.bmCapabilities = 	0X00,
};

#endif

#ifdef	DEV_CONFIG_CDC

/* "Ethernet Networking Functional Descriptor" from CDC spec 5.2.3.16 */
struct ether_desc {
	u8	bLength;
	u8	bDescriptorType;
	u8	bDescriptorSubType;

	u8	iMACAddress;
	u32	bmEthernetStatistics;
	u16	wMaxSegmentSize;
	u16	wNumberMCFilters;
	u8	bNumberPowerFilters;
} __attribute__ ((packed));

static const struct ether_desc ether_desc = {
	.bLength =		sizeof ether_desc,
	.bDescriptorType =	USB_DT_CS_INTERFACE,
	.bDescriptorSubType =	0x0f,

	/* this descriptor actually adds value, surprise! */
	.iMACAddress =		STRING_ETHADDR,
	.bmEthernetStatistics =	__constant_cpu_to_le32 (0), /* no statistics */
	.wMaxSegmentSize =	__constant_cpu_to_le16 (ETH_FRAME_LEN),
	.wNumberMCFilters =	__constant_cpu_to_le16 (0),
	.bNumberPowerFilters =	0,
};

#endif

#if defined(DEV_CONFIG_CDC) || defined(CONFIG_USB_ETH_RNDIS)

/* include the status endpoint if we can, even where it's optional.
 * use small wMaxPacketSize, since many "interrupt" endpoints have
 * very small fifos and it's no big deal if CDC_NOTIFY_SPEED_CHANGE
 * takes two packets.  also default to a big transfer interval, to
 * waste less bandwidth.
 *
 * some drivers (like Linux 2.4 cdc-ether!) "need" it to exist even
 * if they ignore the connect/disconnect notifications that real aether
 * can provide.  more advanced cdc configurations might want to support
 * encapsulated commands (vendor-specific, using control-OUT).
 *
 * RNDIS requires the status endpoint, since it uses that encapsulation
 * mechanism for its funky RPC scheme.
 */
 
#define LOG2_STATUS_INTERVAL_MSEC	5	/* 1 << 5 == 32 msec */
#define STATUS_BYTECOUNT		8	/* 8 byte header + data */

static struct usb_endpoint_descriptor
fs_status_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize =	__constant_cpu_to_le16 (STATUS_BYTECOUNT),
	.bInterval =		1 << LOG2_STATUS_INTERVAL_MSEC,
};
#endif

#ifdef	DEV_CONFIG_CDC

/* the default data interface has no endpoints ... */

static const struct usb_interface_descriptor
data_nop_intf = {
	.bLength =		sizeof data_nop_intf,
	.bDescriptorType =	USB_DT_INTERFACE,

	.bInterfaceNumber =	1,
	.bAlternateSetting =	0,
	.bNumEndpoints =	0,
	.bInterfaceClass =	USB_CLASS_CDC_DATA,
	.bInterfaceSubClass =	0,
	.bInterfaceProtocol =	0,
};

/* ... but the "real" data interface has two bulk endpoints */

static const struct usb_interface_descriptor
data_intf = {
	.bLength =		sizeof data_intf,
	.bDescriptorType =	USB_DT_INTERFACE,

	.bInterfaceNumber =	1,
	.bAlternateSetting =	1,
	.bNumEndpoints =	2,
	.bInterfaceClass =	USB_CLASS_CDC_DATA,
	.bInterfaceSubClass =	0,
	.bInterfaceProtocol =	0,
	.iInterface =		STRING_DATA,
};

#endif

#ifdef	CONFIG_USB_ETH_RNDIS

/* RNDIS doesn't activate by changing to the "real" altsetting */

static const struct usb_interface_descriptor
rndis_data_intf = {
	.bLength =		sizeof rndis_data_intf,
	.bDescriptorType =	USB_DT_INTERFACE,

	.bInterfaceNumber =	1,
	.bAlternateSetting =	0,
	.bNumEndpoints =	2,
	.bInterfaceClass =	USB_CLASS_CDC_DATA,
	.bInterfaceSubClass =	0,
	.bInterfaceProtocol =	0,
	.iInterface =		STRING_DATA,
};

#endif

#ifdef DEV_CONFIG_SUBSET

/*
 * "Simple" CDC-subset option is a simple vendor-neutral model that most
 * full speed controllers can handle:  one interface, two bulk endpoints.
 */

static const struct usb_interface_descriptor
subset_data_intf = {
	.bLength =		sizeof subset_data_intf,
	.bDescriptorType =	USB_DT_INTERFACE,

	.bInterfaceNumber =	0,
	.bAlternateSetting =	0,
	.bNumEndpoints =	2,
	.bInterfaceClass =	USB_CLASS_VENDOR_SPEC,
	.bInterfaceSubClass =	0,
	.bInterfaceProtocol =	0,
	.iInterface =		STRING_DATA,
};

#endif	/* SUBSET */


static struct usb_endpoint_descriptor
fs_source_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_IN,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
};

static struct usb_endpoint_descriptor
fs_sink_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bEndpointAddress =	USB_DIR_OUT,
	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
};

static const struct usb_descriptor_header *fs_eth_function [11] = {
	(struct usb_descriptor_header *) &otg_descriptor,
#ifdef DEV_CONFIG_CDC
	/* "cdc" mode descriptors */
	(struct usb_descriptor_header *) &control_intf,
	(struct usb_descriptor_header *) &header_desc,
	(struct usb_descriptor_header *) &union_desc,
	(struct usb_descriptor_header *) &ether_desc,
	/* NOTE: status endpoint may need to be removed */
	(struct usb_descriptor_header *) &fs_status_desc,
	/* data interface, with altsetting */
	(struct usb_descriptor_header *) &data_nop_intf,
	(struct usb_descriptor_header *) &data_intf,
	(struct usb_descriptor_header *) &fs_source_desc,
	(struct usb_descriptor_header *) &fs_sink_desc,
	NULL,
#endif /* DEV_CONFIG_CDC */
};

static inline void __init fs_subset_descriptors(void)
{
#ifdef DEV_CONFIG_SUBSET
	fs_eth_function[1] = (struct usb_descriptor_header *) &subset_data_intf;
	fs_eth_function[2] = (struct usb_descriptor_header *) &fs_source_desc;
	fs_eth_function[3] = (struct usb_descriptor_header *) &fs_sink_desc;
	fs_eth_function[4] = NULL;
#else
	fs_eth_function[1] = NULL;
#endif
}

#ifdef	CONFIG_USB_ETH_RNDIS
static const struct usb_descriptor_header *fs_rndis_function [] = {
	(struct usb_descriptor_header *) &otg_descriptor,
	/* control interface matches ACM, not Ethernet */
	(struct usb_descriptor_header *) &rndis_control_intf,
	(struct usb_descriptor_header *) &header_desc,
	(struct usb_descriptor_header *) &call_mgmt_descriptor,
	(struct usb_descriptor_header *) &acm_descriptor,
	(struct usb_descriptor_header *) &union_desc,
	(struct usb_descriptor_header *) &fs_status_desc,
	/* data interface has no altsetting */
	(struct usb_descriptor_header *) &rndis_data_intf,
	(struct usb_descriptor_header *) &fs_source_desc,
	(struct usb_descriptor_header *) &fs_sink_desc,
	NULL,
};
#endif

#ifdef	CONFIG_USB_GADGET_DUALSPEED

/*
 * usb 2.0 devices need to expose both high speed and full speed
 * descriptors, unless they only run at full speed.
 */

#if defined(DEV_CONFIG_CDC) || defined(CONFIG_USB_ETH_RNDIS)
static struct usb_endpoint_descriptor
hs_status_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bmAttributes =		USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize =	__constant_cpu_to_le16 (STATUS_BYTECOUNT),
	.bInterval =		LOG2_STATUS_INTERVAL_MSEC + 4,
};
#endif /* DEV_CONFIG_CDC */

static struct usb_endpoint_descriptor
hs_source_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	__constant_cpu_to_le16 (512),
};

static struct usb_endpoint_descriptor
hs_sink_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	__constant_cpu_to_le16 (512),
};

static struct usb_qualifier_descriptor
dev_qualifier = {
	.bLength =		sizeof dev_qualifier,
	.bDescriptorType =	USB_DT_DEVICE_QUALIFIER,

	.bcdUSB =		__constant_cpu_to_le16 (0x0200),
	.bDeviceClass =		USB_CLASS_COMM,

	.bNumConfigurations =	1,
};

static const struct usb_descriptor_header *hs_eth_function [11] = {
	(struct usb_descriptor_header *) &otg_descriptor,
#ifdef DEV_CONFIG_CDC
	/* "cdc" mode descriptors */
	(struct usb_descriptor_header *) &control_intf,
	(struct usb_descriptor_header *) &header_desc,
	(struct usb_descriptor_header *) &union_desc,
	(struct usb_descriptor_header *) &ether_desc,
	/* NOTE: status endpoint may need to be removed */
	(struct usb_descriptor_header *) &hs_status_desc,
	/* data interface, with altsetting */
	(struct usb_descriptor_header *) &data_nop_intf,
	(struct usb_descriptor_header *) &data_intf,
	(struct usb_descriptor_header *) &hs_source_desc,
	(struct usb_descriptor_header *) &hs_sink_desc,
	NULL,
#endif /* DEV_CONFIG_CDC */
};

static inline void __init hs_subset_descriptors(void)
{
#ifdef DEV_CONFIG_SUBSET
	hs_eth_function[1] = (struct usb_descriptor_header *) &subset_data_intf;
	hs_eth_function[2] = (struct usb_descriptor_header *) &fs_source_desc;
	hs_eth_function[3] = (struct usb_descriptor_header *) &fs_sink_desc;
	hs_eth_function[4] = NULL;
#else
	hs_eth_function[1] = NULL;
#endif
}

#ifdef	CONFIG_USB_ETH_RNDIS
static const struct usb_descriptor_header *hs_rndis_function [] = {
	(struct usb_descriptor_header *) &otg_descriptor,
	/* control interface matches ACM, not Ethernet */
	(struct usb_descriptor_header *) &rndis_control_intf,
	(struct usb_descriptor_header *) &header_desc,
	(struct usb_descriptor_header *) &call_mgmt_descriptor,
	(struct usb_descriptor_header *) &acm_descriptor,
	(struct usb_descriptor_header *) &union_desc,
	(struct usb_descriptor_header *) &hs_status_desc,
	/* data interface has no altsetting */
	(struct usb_descriptor_header *) &rndis_data_intf,
	(struct usb_descriptor_header *) &hs_source_desc,
	(struct usb_descriptor_header *) &hs_sink_desc,
	NULL,
};
#endif


/* maxpacket and other transfer characteristics vary by speed. */
#define ep_desc(g,hs,fs) (((g)->speed==USB_SPEED_HIGH)?(hs):(fs))

#else

/* if there's no high speed support, maxpacket doesn't change. */
#define ep_desc(g,hs,fs) fs

static inline void __init hs_subset_descriptors(void)
{
}

#endif	/* !CONFIG_USB_GADGET_DUALSPEED */

/*-------------------------------------------------------------------------*/

/* descriptors that are built on-demand */

static char				manufacturer [40];
static char				product_desc [40] = DRIVER_DESC;

#ifdef	DEV_CONFIG_CDC
/* address that the host will use ... usually assigned at random */
static char				ethaddr [2 * ETH_ALEN + 1];
#endif

/* static strings, in iso 8859/1 */
static struct usb_string		strings [] = {
	{ STRING_MANUFACTURER,	manufacturer, },
	{ STRING_PRODUCT,	product_desc, },
	{ STRING_DATA,		"Ethernet Data", },
#ifdef	DEV_CONFIG_CDC
	{ STRING_CDC,		"CDC Ethernet", },
	{ STRING_ETHADDR,	ethaddr, },
	{ STRING_CONTROL,	"CDC Communications Control", },
#endif
#ifdef	DEV_CONFIG_SUBSET
	{ STRING_SUBSET,	"CDC Ethernet Subset", },
#endif
#ifdef	CONFIG_USB_ETH_RNDIS
	{ STRING_RNDIS,		"RNDIS", },
	{ STRING_RNDIS_CONTROL,	"RNDIS Communications Control", },
#endif
	{  }		/* end of list */
};

static struct usb_gadget_strings	stringtab = {
	.language	= 0x0409,	/* en-us */
	.strings	= strings,
};

/*
 * one config, two interfaces:  control, data.
 * complications: class descriptors, and an altsetting.
 */
static int
config_buf (enum usb_device_speed speed,
	u8 *buf, u8 type,
	unsigned index, int is_otg)
{
	int					len;
	const struct usb_config_descriptor	*config;
	const struct usb_descriptor_header	**function;
#ifdef CONFIG_USB_GADGET_DUALSPEED
	int				hs = (speed == USB_SPEED_HIGH);

	if (type == USB_DT_OTHER_SPEED_CONFIG)
		hs = !hs;
#define which_fn(t)	(hs ? & hs_ ## t ## _function : & fs_ ## t ## _function)
#else
#define	which_fn(t)	(& fs_ ## t ## _function)
#endif

	if (index >= device_desc.bNumConfigurations)
		return -EINVAL;

#ifdef	CONFIG_USB_ETH_RNDIS
	/* list the RNDIS config first, to make Microsoft's drivers
	 * happy. DOCSIS 1.0 needs this too.
	 */
	if (device_desc.bNumConfigurations == 2 && index == 0) {
		config = &rndis_config;
		function = (const struct usb_descriptor_header **)
				which_fn (rndis);
	} else
#endif
	{
		config = &eth_config;
		function = (const struct usb_descriptor_header **)
				which_fn (eth);
	}

	/* for now, don't advertise srp-only devices */
	if (!is_otg)
		function++;

	len = usb_gadget_config_buf (config, buf, USB_BUFSIZ, function);
	if (len < 0)
		return len;
	((struct usb_config_descriptor *) buf)->bDescriptorType = type;
	return len;
}

/*-------------------------------------------------------------------------*/

static void eth_start (struct eth_dev *dev, int gfp_flags);
static int alloc_requests (struct eth_dev *dev, unsigned n, int gfp_flags);

#ifdef	DEV_CONFIG_CDC
static inline int ether_alt_ep_setup (struct eth_dev *dev, struct usb_ep *ep)
{
	const struct usb_endpoint_descriptor	*d;

	/* With CDC,  the host isn't allowed to use these two data
	 * endpoints in the default altsetting for the interface.
	 * so we don't activate them yet.  Reset from SET_INTERFACE.
	 *
	 * Strictly speaking RNDIS should work the same: activation is
	 * a side effect of setting a packet filter.  Deactivation is
	 * from REMOTE_NDIS_HALT_MSG, reset from REMOTE_NDIS_RESET_MSG.
	 */

	/* one endpoint writes data back IN to the host */
	if (strcmp (ep->name, EP_IN_NAME) == 0) {
		d = ep_desc (dev->gadget, &hs_source_desc, &fs_source_desc);
		ep->driver_data = dev;
		dev->in_ep = ep;
		dev->in = d;

	/* one endpoint just reads OUT packets */
	} else if (strcmp (ep->name, EP_OUT_NAME) == 0) {
		d = ep_desc (dev->gadget, &hs_sink_desc, &fs_sink_desc);
		ep->driver_data = dev;
		dev->out_ep = ep;
		dev->out = d;

	/* optional status/notification endpoint */
	} else if (EP_STATUS_NAME &&
			strcmp (ep->name, EP_STATUS_NAME) == 0) {
		int			result;

		d = ep_desc (dev->gadget, &hs_status_desc, &fs_status_desc);
		result = usb_ep_enable (ep, d);
		if (result < 0)
			return result;

		ep->driver_data = dev;
		dev->status_ep = ep;
		dev->status = d;
	}
	return 0;
}
#endif

#if	defined(DEV_CONFIG_SUBSET) || defined(CONFIG_USB_ETH_RNDIS)
static inline int ether_ep_setup (struct eth_dev *dev, struct usb_ep *ep)
{
	int					result;
	const struct usb_endpoint_descriptor	*d;

	/* CDC subset is simpler:  if the device is there,
	 * it's live with rx and tx endpoints.
	 *
	 * Do this as a shortcut for RNDIS too.
	 */

	/* one endpoint writes data back IN to the host */
	if (strcmp (ep->name, EP_IN_NAME) == 0) {
		d = ep_desc (dev->gadget, &hs_source_desc, &fs_source_desc);
		result = usb_ep_enable (ep, d);
		if (result < 0)
			return result;

		ep->driver_data = dev;
		dev->in_ep = ep;
		dev->in = d;

	/* one endpoint just reads OUT packets */
	} else if (strcmp (ep->name, EP_OUT_NAME) == 0) {
		d = ep_desc (dev->gadget, &hs_sink_desc, &fs_sink_desc);
		result = usb_ep_enable (ep, d);
		if (result < 0)
			return result;

		ep->driver_data = dev;
		dev->out_ep = ep;
		dev->out = d;
	}

	return 0;
}
#endif

static int
set_ether_config (struct eth_dev *dev, int gfp_flags)
{
	int			result = 0;
	struct usb_ep		*ep;
	struct usb_gadget	*gadget = dev->gadget;

	gadget_for_each_ep (ep, gadget) {
#ifdef	DEV_CONFIG_CDC
		if (!dev->rndis && dev->cdc) {
			result = ether_alt_ep_setup (dev, ep);
			if (result == 0)
				continue;
		}
#endif

#ifdef	CONFIG_USB_ETH_RNDIS
		if (dev->rndis && strcmp (ep->name, EP_STATUS_NAME) == 0) {
			const struct usb_endpoint_descriptor	*d;
			d = ep_desc (gadget, &hs_status_desc, &fs_status_desc);
			result = usb_ep_enable (ep, d);
			if (result == 0) {
				ep->driver_data = dev;
				dev->status_ep = ep;
				dev->status = d;
				continue;
			}
		} else
#endif

		{
#if	defined(DEV_CONFIG_SUBSET) || defined(CONFIG_USB_ETH_RNDIS)
			result = ether_ep_setup (dev, ep);
			if (result == 0)
				continue;
#endif
		}

		/* stop on error */
		ERROR (dev, "can't enable %s, result %d\n", ep->name, result);
		break;
	}
	if (!result && (!dev->in_ep || !dev->out_ep))
		result = -ENODEV;

	if (result == 0)
		result = alloc_requests (dev, qlen (gadget), gfp_flags);

	/* on error, disable any endpoints  */
	if (result < 0) {
#if defined(DEV_CONFIG_CDC) || defined(CONFIG_USB_ETH_RNDIS)
		if (dev->status_ep)
			(void) usb_ep_disable (dev->status_ep);
#endif
		dev->status_ep = NULL;
		dev->status = NULL;
#if defined(DEV_CONFIG_SUBSET) || defined(CONFIG_USB_ETH_RNDIS)
		if (dev->rndis || !dev->cdc) {
			if (dev->in_ep)
				(void) usb_ep_disable (dev->in_ep);
			if (dev->out_ep)
				(void) usb_ep_disable (dev->out_ep);
		}
#endif
		dev->in_ep = NULL;
		dev->in = NULL;
		dev->out_ep = NULL;
		dev->out = NULL;
	} else

	/* activate non-CDC configs right away
	 * this isn't strictly according to the RNDIS spec
	 */
#if defined(DEV_CONFIG_SUBSET) || defined(CONFIG_USB_ETH_RNDIS)
	if (dev->rndis || !dev->cdc) {
		netif_carrier_on (dev->net);
		if (netif_running (dev->net)) {
			spin_unlock (&dev->lock);
			eth_start (dev, GFP_ATOMIC);
			spin_lock (&dev->lock);
		}
	}
#endif

	if (result == 0)
		DEBUG (dev, "qlen %d\n", qlen (gadget));

	/* caller is responsible for cleanup on error */
	return result;
}

static void eth_reset_config (struct eth_dev *dev)
{
	struct usb_request	*req;

	if (dev->config == 0)
		return;

	DEBUG (dev, "%s\n", __FUNCTION__);

	netif_stop_queue (dev->net);
	netif_carrier_off (dev->net);

	/* disable endpoints, forcing (synchronous) completion of
	 * pending i/o.  then free the requests.
	 */
	if (dev->in_ep) {
		usb_ep_disable (dev->in_ep);
		while (likely (!list_empty (&dev->tx_reqs))) {
			req = container_of (dev->tx_reqs.next,
						struct usb_request, list);
			list_del (&req->list);
			usb_ep_free_request (dev->in_ep, req);
		}
		dev->in_ep = NULL;
	}
	if (dev->out_ep) {
		usb_ep_disable (dev->out_ep);
		while (likely (!list_empty (&dev->rx_reqs))) {
			req = container_of (dev->rx_reqs.next,
						struct usb_request, list);
			list_del (&req->list);
			usb_ep_free_request (dev->out_ep, req);
		}
		dev->out_ep = NULL;
	}

	if (dev->status_ep) {
		usb_ep_disable (dev->status_ep);
		dev->status_ep = NULL;
	}
	dev->config = 0;
}

/* change our operational config.  must agree with the code
 * that returns config descriptors, and altsetting code.
 */
static int
eth_set_config (struct eth_dev *dev, unsigned number, int gfp_flags)
{
	int			result = 0;
	struct usb_gadget	*gadget = dev->gadget;

	if (number == dev->config)
		return 0;

	if (gadget_is_sa1100 (gadget)
			&& dev->config
			&& atomic_read (&dev->tx_qlen) != 0) {
		/* tx fifo is full, but we can't clear it...*/
		INFO (dev, "can't change configurations\n");
		return -ESPIPE;
	}
	eth_reset_config (dev);

	/* default:  pass all packets, no multicast filtering */
	dev->cdc_filter = 0x000f;

	switch (number) {
	case DEV_CONFIG_VALUE:
		dev->rndis = 0;
		result = set_ether_config (dev, gfp_flags);
		break;
#ifdef	CONFIG_USB_ETH_RNDIS
	case DEV_RNDIS_CONFIG_VALUE:
		dev->rndis = 1;
		result = set_ether_config (dev, gfp_flags);
		break;
#endif
	default:
		result = -EINVAL;
		/* FALL THROUGH */
	case 0:
		return result;
	}

	if (result)
		eth_reset_config (dev);
	else {
		char *speed;

		switch (gadget->speed) {
		case USB_SPEED_FULL:	speed = "full"; break;
#ifdef CONFIG_USB_GADGET_DUALSPEED
		case USB_SPEED_HIGH:	speed = "high"; break;
#endif
		default: 		speed = "?"; break;
		}

		dev->config = number;
		INFO (dev, "%s speed config #%d: %s, using %s\n",
				speed, number, driver_desc,
				dev->rndis
					? "RNDIS"
					: (dev->cdc
						? "CDC Ethernet"
						: "CDC Ethernet Subset"));
	}
	return result;
}

/*-------------------------------------------------------------------------*/

/* section 3.8.2 table 11 of the CDC spec lists Ethernet notifications
 * section 3.6.2.1 table 5 specifies ACM notifications, accepted by RNDIS
 * and RNDIS also defines its own bit-incompatible notifications
 */
#define CDC_NOTIFY_NETWORK_CONNECTION	0x00	/* required; 6.3.1 */
#define CDC_NOTIFY_RESPONSE_AVAILABLE	0x01	/* optional; 6.3.2 */
#define CDC_NOTIFY_SPEED_CHANGE		0x2a	/* required; 6.3.8 */

#ifdef	DEV_CONFIG_CDC

struct cdc_notification {
	u8	bmRequestType;
	u8	bNotificationType;
	u16	wValue;
	u16	wIndex;
	u16	wLength;

	/* SPEED_CHANGE data looks like this */
	u32	data [2];
};

static void eth_status_complete (struct usb_ep *ep, struct usb_request *req)
{
	struct cdc_notification	*event = req->buf;
	int			value = req->status;
	struct eth_dev		*dev = ep->driver_data;

	/* issue the second notification if host reads the first */
	if (event->bNotificationType == CDC_NOTIFY_NETWORK_CONNECTION
			&& value == 0) {
		event->bmRequestType = 0xA1;
		event->bNotificationType = CDC_NOTIFY_SPEED_CHANGE;
		event->wValue = __constant_cpu_to_le16 (0);
		event->wIndex = __constant_cpu_to_le16 (1);
		event->wLength = __constant_cpu_to_le16 (8);

		/* SPEED_CHANGE data is up/down speeds in bits/sec */
		event->data [0] = event->data [1] =
			(dev->gadget->speed == USB_SPEED_HIGH)
				? (13 * 512 * 8 * 1000 * 8)
				: (19 *  64 * 1 * 1000 * 8);

		req->length = 16;
		value = usb_ep_queue (ep, req, GFP_ATOMIC);
		DEBUG (dev, "send SPEED_CHANGE --> %d\n", value);
		if (value == 0)
			return;
	} else
		DEBUG (dev, "event %02x --> %d\n",
			event->bNotificationType, value);

	/* free when done */
	usb_ep_free_buffer (ep, req->buf, req->dma, 16);
	usb_ep_free_request (ep, req);
}

static void issue_start_status (struct eth_dev *dev)
{
	struct usb_request	*req;
	struct cdc_notification	*event;
	int			value;
 
	DEBUG (dev, "%s, flush old status first\n", __FUNCTION__);

	/* flush old status
	 *
	 * FIXME ugly idiom, maybe we'd be better with just
	 * a "cancel the whole queue" primitive since any
	 * unlink-one primitive has way too many error modes.
	 * here, we "know" toggle is already clear...
	 */
	usb_ep_disable (dev->status_ep);
	usb_ep_enable (dev->status_ep, dev->status);

	/* FIXME make these allocations static like dev->req */
	req = usb_ep_alloc_request (dev->status_ep, GFP_ATOMIC);
	if (req == 0) {
		DEBUG (dev, "status ENOMEM\n");
		return;
	}
	req->buf = usb_ep_alloc_buffer (dev->status_ep, 16,
				&dev->req->dma, GFP_ATOMIC);
	if (req->buf == 0) {
		DEBUG (dev, "status buf ENOMEM\n");
free_req:
		usb_ep_free_request (dev->status_ep, req);
		return;
	}

	/* 3.8.1 says to issue first NETWORK_CONNECTION, then
	 * a SPEED_CHANGE.  could be useful in some configs.
	 */
	event = req->buf;
	event->bmRequestType = 0xA1;
	event->bNotificationType = CDC_NOTIFY_NETWORK_CONNECTION;
	event->wValue = __constant_cpu_to_le16 (1);	/* connected */
	event->wIndex = __constant_cpu_to_le16 (1);
	event->wLength = 0;

	req->length = 8;
	req->complete = eth_status_complete;
	value = usb_ep_queue (dev->status_ep, req, GFP_ATOMIC);
	if (value < 0) {
		DEBUG (dev, "status buf queue --> %d\n", value);
		usb_ep_free_buffer (dev->status_ep,
				req->buf, dev->req->dma, 16);
		goto free_req;
	}
}

#endif

/*-------------------------------------------------------------------------*/

static void eth_setup_complete (struct usb_ep *ep, struct usb_request *req)
{
	if (req->status || req->actual != req->length)
		DEBUG ((struct eth_dev *) ep->driver_data,
				"setup complete --> %d, %d/%d\n",
				req->status, req->actual, req->length);
}

/* see section 3.8.2 table 10 of the CDC spec for more ethernet
 * requests, mostly for filters (multicast, pm) and statistics
 * section 3.6.2.1 table 4 has ACM requests; RNDIS requires the
 * encapsulated command mechanism.
 */
#define CDC_SEND_ENCAPSULATED_COMMAND		0x00	/* optional */
#define CDC_GET_ENCAPSULATED_RESPONSE		0x01	/* optional */
#define CDC_SET_ETHERNET_MULTICAST_FILTERS	0x40	/* optional */
#define CDC_SET_ETHERNET_PM_PATTERN_FILTER	0x41	/* optional */
#define CDC_GET_ETHERNET_PM_PATTERN_FILTER	0x42	/* optional */
#define CDC_SET_ETHERNET_PACKET_FILTER		0x43	/* required */
#define CDC_GET_ETHERNET_STATISTIC		0x44	/* optional */

/* table 62; bits in cdc_filter */
#define	CDC_PACKET_TYPE_PROMISCUOUS		(1 << 0)
#define	CDC_PACKET_TYPE_ALL_MULTICAST		(1 << 1) /* no filter */
#define	CDC_PACKET_TYPE_DIRECTED		(1 << 2)
#define	CDC_PACKET_TYPE_BROADCAST		(1 << 3)
#define	CDC_PACKET_TYPE_MULTICAST		(1 << 4) /* filtered */

#ifdef CONFIG_USB_ETH_RNDIS

static void rndis_response_complete (struct usb_ep *ep, struct usb_request *req)
{
	if (req->status || req->actual != req->length)
		DEBUG (dev, "rndis response complete --> %d, %d/%d\n",
		       req->status, req->actual, req->length);

	/* done sending after CDC_GET_ENCAPSULATED_RESPONSE */
}

static void rndis_command_complete (struct usb_ep *ep, struct usb_request *req)
{
	struct eth_dev          *dev = ep->driver_data;
	int			status;
	
	/* received RNDIS command from CDC_SEND_ENCAPSULATED_COMMAND */
	spin_lock(&dev->lock);
	status = rndis_msg_parser (dev->rndis_config, (u8 *) req->buf);
	if (status < 0)
		ERROR(dev, "%s: rndis parse error %d\n", __FUNCTION__, status);
	spin_unlock(&dev->lock);
}

#endif	/* RNDIS */

/*
 * The setup() callback implements all the ep0 functionality that's not
 * handled lower down.  CDC has a number of less-common features:
 *
 *  - two interfaces:  control, and ethernet data
 *  - Ethernet data interface has two altsettings:  default, and active
 *  - class-specific descriptors for the control interface
 *  - class-specific control requests
 */
static int
eth_setup (struct usb_gadget *gadget, const struct usb_ctrlrequest *ctrl)
{
	struct eth_dev		*dev = get_gadget_data (gadget);
	struct usb_request	*req = dev->req;
	int			value = -EOPNOTSUPP;

	/* descriptors just go into the pre-allocated ep0 buffer,
	 * while config change events may enable network traffic.
	 */
	req->complete = eth_setup_complete;
	switch (ctrl->bRequest) {

	case USB_REQ_GET_DESCRIPTOR:
		if (ctrl->bRequestType != USB_DIR_IN)
			break;
		switch (ctrl->wValue >> 8) {

		case USB_DT_DEVICE:
			value = min (ctrl->wLength, (u16) sizeof device_desc);
			memcpy (req->buf, &device_desc, value);
			break;
#ifdef CONFIG_USB_GADGET_DUALSPEED
		case USB_DT_DEVICE_QUALIFIER:
			if (!gadget->is_dualspeed)
				break;
			value = min (ctrl->wLength, (u16) sizeof dev_qualifier);
			memcpy (req->buf, &dev_qualifier, value);
			break;

		case USB_DT_OTHER_SPEED_CONFIG:
			if (!gadget->is_dualspeed)
				break;
			// FALLTHROUGH
#endif /* CONFIG_USB_GADGET_DUALSPEED */
		case USB_DT_CONFIG:
			value = config_buf (gadget->speed, req->buf,
					ctrl->wValue >> 8,
					ctrl->wValue & 0xff,
					gadget->is_otg);
			if (value >= 0)
				value = min (ctrl->wLength, (u16) value);
			break;

		case USB_DT_STRING:
			value = usb_gadget_get_string (&stringtab,
					ctrl->wValue & 0xff, req->buf);
			if (value >= 0)
				value = min (ctrl->wLength, (u16) value);
			break;
		}
		break;

	case USB_REQ_SET_CONFIGURATION:
		if (ctrl->bRequestType != 0)
			break;
		if (gadget->a_hnp_support)
			DEBUG (dev, "HNP available\n");
		else if (gadget->a_alt_hnp_support)
			DEBUG (dev, "HNP needs a different root port\n");
		spin_lock (&dev->lock);
		value = eth_set_config (dev, ctrl->wValue, GFP_ATOMIC);
		spin_unlock (&dev->lock);
		break;
	case USB_REQ_GET_CONFIGURATION:
		if (ctrl->bRequestType != USB_DIR_IN)
			break;
		*(u8 *)req->buf = dev->config;
		value = min (ctrl->wLength, (u16) 1);
		break;

	case USB_REQ_SET_INTERFACE:
		if (ctrl->bRequestType != USB_RECIP_INTERFACE
				|| !dev->config
				|| ctrl->wIndex > 1)
			break;
		if (!dev->cdc && ctrl->wIndex != 0)
			break;
		spin_lock (&dev->lock);

		/* PXA hardware partially handles SET_INTERFACE;
		 * we need to kluge around that interference.
		 */
		if (gadget_is_pxa (gadget)) {
			value = eth_set_config (dev, DEV_CONFIG_VALUE,
						GFP_ATOMIC);
			goto done_set_intf;
		}

#ifdef DEV_CONFIG_CDC
		switch (ctrl->wIndex) {
		case 0:		/* control/master intf */
			if (ctrl->wValue != 0)
				break;
			if (dev->status_ep) {
				usb_ep_disable (dev->status_ep);
				usb_ep_enable (dev->status_ep, dev->status);
			}
			value = 0;
			break;
		case 1:		/* data intf */
			if (ctrl->wValue > 1)
				break;
			usb_ep_disable (dev->in_ep);
			usb_ep_disable (dev->out_ep);

			/* CDC requires the data transfers not be done from
			 * the default interface setting ... also, setting
			 * the non-default interface clears filters etc.
			 */
			if (ctrl->wValue == 1) {
				usb_ep_enable (dev->in_ep, dev->in);
				usb_ep_enable (dev->out_ep, dev->out);
				netif_carrier_on (dev->net);
				if (dev->status_ep)
					issue_start_status (dev);
				if (netif_running (dev->net)) {
					spin_unlock (&dev->lock);
					eth_start (dev, GFP_ATOMIC);
					spin_lock (&dev->lock);
				}
			} else {
				netif_stop_queue (dev->net);
				netif_carrier_off (dev->net);
			}
			value = 0;
			break;
		}
#else
		/* FIXME this is wrong, as is the assumption that
		 * all non-PXA hardware talks real CDC ...
		 */
		dev_warn (&gadget->dev, "set_interface ignored!\n");
#endif /* DEV_CONFIG_CDC */

done_set_intf:
		spin_unlock (&dev->lock);
		break;
	case USB_REQ_GET_INTERFACE:
		if (ctrl->bRequestType != (USB_DIR_IN|USB_RECIP_INTERFACE)
				|| !dev->config
				|| ctrl->wIndex > 1)
			break;
		if (!(dev->cdc || dev->rndis) && ctrl->wIndex != 0)
			break;

		/* for CDC, iff carrier is on, data interface is active. */
		if (dev->rndis || ctrl->wIndex != 1)
			*(u8 *)req->buf = 0;
		else
			*(u8 *)req->buf = netif_carrier_ok (dev->net) ? 1 : 0;
		value = min (ctrl->wLength, (u16) 1);
		break;

#ifdef DEV_CONFIG_CDC
	case CDC_SET_ETHERNET_PACKET_FILTER:
		/* see 6.2.30: no data, wIndex = interface,
		 * wValue = packet filter bitmap
		 */
		if (ctrl->bRequestType != (USB_TYPE_CLASS|USB_RECIP_INTERFACE)
				|| !dev->cdc
				|| dev->rndis
				|| ctrl->wLength != 0
				|| ctrl->wIndex > 1)
			break;
		DEBUG (dev, "NOP packet filter %04x\n", ctrl->wValue);
		/* NOTE: table 62 has 5 filter bits to reduce traffic,
		 * and we "must" support multicast and promiscuous.
		 * this NOP implements a bad filter (always promisc)
		 */
		dev->cdc_filter = ctrl->wValue;
		value = 0;
		break;
#endif /* DEV_CONFIG_CDC */

#ifdef CONFIG_USB_ETH_RNDIS		
	/* RNDIS uses the CDC command encapsulation mechanism to implement
	 * an RPC scheme, with much getting/setting of attributes by OID.
	 */
	case CDC_SEND_ENCAPSULATED_COMMAND:
		if (ctrl->bRequestType != (USB_TYPE_CLASS|USB_RECIP_INTERFACE)
				|| !dev->rndis
				|| ctrl->wLength > USB_BUFSIZ
				|| ctrl->wValue
				|| rndis_control_intf.bInterfaceNumber
					!= ctrl->wIndex)
			break;
		/* read the request, then process it */
		value = ctrl->wLength;
		req->complete = rndis_command_complete;
		/* later, rndis_control_ack () sends a notification */
		break;
		
	case CDC_GET_ENCAPSULATED_RESPONSE:
		if ((USB_DIR_IN|USB_TYPE_CLASS|USB_RECIP_INTERFACE)
					== ctrl->bRequestType
				&& dev->rndis
				// && ctrl->wLength >= 0x0400
				&& !ctrl->wValue
				&& rndis_control_intf.bInterfaceNumber
					== ctrl->wIndex) {
			u8 *buf;

			/* return the result */
			buf = rndis_get_next_response (dev->rndis_config,
						       &value);
			if (buf) {
				memcpy (req->buf, buf, value);
				req->complete = rndis_response_complete;
				rndis_free_response(dev->rndis_config, buf);
			}
			/* else stalls ... spec says to avoid that */
		}
		break;
#endif	/* RNDIS */

	default:
		VDEBUG (dev,
			"unknown control req%02x.%02x v%04x i%04x l%d\n",
			ctrl->bRequestType, ctrl->bRequest,
			ctrl->wValue, ctrl->wIndex, ctrl->wLength);
	}

	/* respond with data transfer before status phase? */
	if (value >= 0) {
		req->length = value;
		req->zero = value < ctrl->wLength
				&& (value % gadget->ep0->maxpacket) == 0;
		value = usb_ep_queue (gadget->ep0, req, GFP_ATOMIC);
		if (value < 0) {
			DEBUG (dev, "ep_queue --> %d\n", value);
			req->status = 0;
			eth_setup_complete (gadget->ep0, req);
		}
	}

	/* host either stalls (value < 0) or reports success */
	return value;
}

static void
eth_disconnect (struct usb_gadget *gadget)
{
	struct eth_dev		*dev = get_gadget_data (gadget);
	unsigned long		flags;

	spin_lock_irqsave (&dev->lock, flags);
	netif_stop_queue (dev->net);
	netif_carrier_off (dev->net);
	eth_reset_config (dev);
	spin_unlock_irqrestore (&dev->lock, flags);

	/* FIXME RNDIS should enter RNDIS_UNINITIALIZED */

	/* next we may get setup() calls to enumerate new connections;
	 * or an unbind() during shutdown (including removing module).
	 */
}

/*-------------------------------------------------------------------------*/

/* NETWORK DRIVER HOOKUP (to the layer above this driver) */

static int eth_change_mtu (struct net_device *net, int new_mtu)
{
	struct eth_dev	*dev = (struct eth_dev *) net->priv;

	// FIXME if rndis, don't change while link's live

	if (new_mtu <= ETH_HLEN || new_mtu > ETH_FRAME_LEN)
		return -ERANGE;
	/* no zero-length packet read wanted after mtu-sized packets */
	if (((new_mtu + sizeof (struct ethhdr)) % dev->in_ep->maxpacket) == 0)
		return -EDOM;
	net->mtu = new_mtu;
	return 0;
}

static struct net_device_stats *eth_get_stats (struct net_device *net)
{
	return &((struct eth_dev *) net->priv)->stats;
}

static int eth_ethtool_ioctl (struct net_device *net, void __user *useraddr)
{
	struct eth_dev	*dev = (struct eth_dev *) net->priv;
	u32		cmd;

	if (get_user (cmd, (u32 __user *)useraddr))
		return -EFAULT;
	switch (cmd) {

	case ETHTOOL_GDRVINFO: {	/* get driver info */
		struct ethtool_drvinfo		info;

		memset (&info, 0, sizeof info);
		info.cmd = ETHTOOL_GDRVINFO;
		strlcpy (info.driver, shortname, sizeof info.driver);
		strlcpy (info.version, DRIVER_VERSION, sizeof info.version);
		strlcpy (info.fw_version, dev->gadget->name,
			sizeof info.fw_version);
		strlcpy (info.bus_info, dev->gadget->dev.bus_id,
			sizeof info.bus_info);
		if (copy_to_user (useraddr, &info, sizeof (info)))
			return -EFAULT;
		return 0;
		}

	case ETHTOOL_GLINK: {		/* get link status */
		struct ethtool_value	edata = { ETHTOOL_GLINK };

		edata.data = (dev->gadget->speed != USB_SPEED_UNKNOWN);
		if (copy_to_user (useraddr, &edata, sizeof (edata)))
			return -EFAULT;
		return 0;
		}

	}
	/* Note that the ethtool user space code requires EOPNOTSUPP */
	return -EOPNOTSUPP;
}

static int eth_ioctl (struct net_device *net, struct ifreq *rq, int cmd)
{
	switch (cmd) {
	case SIOCETHTOOL:
		return eth_ethtool_ioctl(net, rq->ifr_data);
	default:
		return -EOPNOTSUPP;
	}
}

static void defer_kevent (struct eth_dev *dev, int flag)
{
	if (test_and_set_bit (flag, &dev->todo))
		return;
	if (!schedule_work (&dev->work))
		ERROR (dev, "kevent %d may have been dropped\n", flag);
	else
		DEBUG (dev, "kevent %d scheduled\n", flag);
}

static void rx_complete (struct usb_ep *ep, struct usb_request *req);

static int
rx_submit (struct eth_dev *dev, struct usb_request *req, int gfp_flags)
{
	struct sk_buff		*skb;
	int			retval = -ENOMEM;
	size_t			size;

	/* Padding up to RX_EXTRA handles minor disagreements with host.
	 * Normally we use the USB "terminate on short read" convention;
	 * so allow up to (N*maxpacket), since that memory is normally
	 * already allocated.  Some hardware doesn't deal well with short
	 * reads (e.g. DMA must be N*maxpacket), so for now don't trim a
	 * byte off the end (to force hardware errors on overflow).
	 *
	 * RNDIS uses internal framing, and explicitly allows senders to
	 * pad to end-of-packet.  That's potentially nice for speed,
	 * but means receivers can't recover synch on their own.
	 */
	size = (sizeof (struct ethhdr) + dev->net->mtu + RX_EXTRA);
	size += dev->out_ep->maxpacket - 1;
#ifdef CONFIG_USB_ETH_RNDIS
	if (dev->rndis)
		size += sizeof (struct rndis_packet_msg_type);
#endif	
	size -= size % dev->out_ep->maxpacket;

	if ((skb = alloc_skb (size, gfp_flags)) == 0) {
		DEBUG (dev, "no rx skb\n");
		goto enomem;
	}

	req->buf = skb->data;
	req->length = size;
	req->complete = rx_complete;
	req->context = skb;

	retval = usb_ep_queue (dev->out_ep, req, gfp_flags);
	if (retval == -ENOMEM)
enomem:
		defer_kevent (dev, WORK_RX_MEMORY);
	if (retval) {
		DEBUG (dev, "rx submit --> %d\n", retval);
		dev_kfree_skb_any (skb);
		spin_lock (&dev->lock);
		list_add (&req->list, &dev->rx_reqs);
		spin_unlock (&dev->lock);
	}
	return retval;
}

static void rx_complete (struct usb_ep *ep, struct usb_request *req)
{
	struct sk_buff	*skb = req->context;
	struct eth_dev	*dev = ep->driver_data;
	int		status = req->status;

	switch (status) {

	/* normal completion */
	case 0:
		skb_put (skb, req->actual);
#ifdef CONFIG_USB_ETH_RNDIS
		/* we know MaxPacketsPerTransfer == 1 here */
		if (dev->rndis)
			rndis_rm_hdr (req->buf, &(skb->len));
#endif
		if (ETH_HLEN > skb->len || skb->len > ETH_FRAME_LEN) {
			dev->stats.rx_errors++;
			dev->stats.rx_length_errors++;
			DEBUG (dev, "rx length %d\n", skb->len);
			break;
		}

		skb->dev = dev->net;
		skb->protocol = eth_type_trans (skb, dev->net);
		dev->stats.rx_packets++;
		dev->stats.rx_bytes += skb->len;

		/* no buffer copies needed, unless hardware can't
		 * use skb buffers.
		 */
		status = netif_rx (skb);
		skb = NULL;
		break;

	/* software-driven interface shutdown */
	case -ECONNRESET:		// unlink
	case -ESHUTDOWN:		// disconnect etc
		VDEBUG (dev, "rx shutdown, code %d\n", status);
		goto quiesce;

	/* for hardware automagic (such as pxa) */
	case -ECONNABORTED:		// endpoint reset
		DEBUG (dev, "rx %s reset\n", ep->name);
		defer_kevent (dev, WORK_RX_MEMORY);
quiesce:
		dev_kfree_skb_any (skb);
		goto clean;

	/* data overrun */
	case -EOVERFLOW:
		dev->stats.rx_over_errors++;
		// FALLTHROUGH
	    
	default:
		dev->stats.rx_errors++;
		DEBUG (dev, "rx status %d\n", status);
		break;
	}

	if (skb)
		dev_kfree_skb_any (skb);
	if (!netif_running (dev->net)) {
clean:
		/* nobody reading rx_reqs, so no dev->lock */
		list_add (&req->list, &dev->rx_reqs);
		req = NULL;
	}
	if (req)
		rx_submit (dev, req, GFP_ATOMIC);
}

static int prealloc (struct list_head *list, struct usb_ep *ep,
			unsigned n, int gfp_flags)
{
	unsigned		i;
	struct usb_request	*req;

	if (!n)
		return -ENOMEM;

	/* queue/recycle up to N requests */
	i = n;
	list_for_each_entry (req, list, list) {
		if (i-- == 0)
			goto extra;
	}
	while (i--) {
		req = usb_ep_alloc_request (ep, gfp_flags);
		if (!req)
			return list_empty (list) ? -ENOMEM : 0;
		list_add (&req->list, list);
	}
	return 0;

extra:
	/* free extras */
	for (;;) {
		struct list_head	*next;

		next = req->list.next;
		list_del (&req->list);
		usb_ep_free_request (ep, req);

		if (next == list)
			break;

		req = container_of (next, struct usb_request, list);
	}
	return 0;
}

static int alloc_requests (struct eth_dev *dev, unsigned n, int gfp_flags)
{
	int status;

	status = prealloc (&dev->tx_reqs, dev->in_ep, n, gfp_flags);
	if (status < 0)
		goto fail;
	status = prealloc (&dev->rx_reqs, dev->out_ep, n, gfp_flags);
	if (status < 0)
		goto fail;
	return 0;
fail:
	DEBUG (dev, "can't alloc requests\n");
	return status;
}

static void rx_fill (struct eth_dev *dev, int gfp_flags)
{
	struct usb_request	*req;
	unsigned long		flags;

	clear_bit (WORK_RX_MEMORY, &dev->todo);

	/* fill unused rxq slots with some skb */
	spin_lock_irqsave (&dev->lock, flags);
	while (!list_empty (&dev->rx_reqs)) {
		req = container_of (dev->rx_reqs.next,
				struct usb_request, list);
		list_del_init (&req->list);
		spin_unlock_irqrestore (&dev->lock, flags);

		if (rx_submit (dev, req, gfp_flags) < 0) {
			defer_kevent (dev, WORK_RX_MEMORY);
			return;
		}

		spin_lock_irqsave (&dev->lock, flags);
	}
	spin_unlock_irqrestore (&dev->lock, flags);
}

static void eth_work (void *_dev)
{
	struct eth_dev		*dev = _dev;

	if (test_bit (WORK_RX_MEMORY, &dev->todo)) {
		if (netif_running (dev->net))
			rx_fill (dev, GFP_KERNEL);
		else
			clear_bit (WORK_RX_MEMORY, &dev->todo);
	}

	if (dev->todo)
		DEBUG (dev, "work done, flags = 0x%lx\n", dev->todo);
}

static void tx_complete (struct usb_ep *ep, struct usb_request *req)
{
	struct sk_buff	*skb = req->context;
	struct eth_dev	*dev = ep->driver_data;

	switch (req->status) {
	default:
		dev->stats.tx_errors++;
		VDEBUG (dev, "tx err %d\n", req->status);
		/* FALLTHROUGH */
	case -ECONNRESET:		// unlink
	case -ESHUTDOWN:		// disconnect etc
		break;
	case 0:
		dev->stats.tx_bytes += skb->len;
	}
	dev->stats.tx_packets++;

	spin_lock (&dev->lock);
	list_add (&req->list, &dev->tx_reqs);
	spin_unlock (&dev->lock);
	dev_kfree_skb_any (skb);

	atomic_dec (&dev->tx_qlen);
	if (netif_carrier_ok (dev->net))
		netif_wake_queue (dev->net);
}

static int eth_start_xmit (struct sk_buff *skb, struct net_device *net)
{
	struct eth_dev		*dev = (struct eth_dev *) net->priv;
	int			length = skb->len;
	int			retval;
	struct usb_request	*req = NULL;
	unsigned long		flags;

	/* FIXME check dev->cdc_filter to decide whether to send this,
	 * instead of acting as if CDC_PACKET_TYPE_PROMISCUOUS were
	 * always set.  RNDIS has the same kind of outgoing filter.
	 */

	spin_lock_irqsave (&dev->lock, flags);
	req = container_of (dev->tx_reqs.next, struct usb_request, list);
	list_del (&req->list);
	if (list_empty (&dev->tx_reqs))
		netif_stop_queue (net);
	spin_unlock_irqrestore (&dev->lock, flags);

	/* no buffer copies needed, unless the network stack did it
	 * or the hardware can't use skb buffers.
	 * or there's not enough space for any RNDIS headers we need
	 */
#ifdef CONFIG_USB_ETH_RNDIS
	if (dev->rndis) {
		struct sk_buff	*skb_rndis;

		skb_rndis = skb_realloc_headroom (skb,
				sizeof (struct rndis_packet_msg_type));
		if (!skb_rndis)
			goto drop;
	
		dev_kfree_skb_any (skb);
		skb = skb_rndis;
		rndis_add_hdr (skb);
		length = skb->len;
	}
#endif
	req->buf = skb->data;
	req->context = skb;
	req->complete = tx_complete;

	/* use zlp framing on tx for strict CDC-Ether conformance,
	 * though any robust network rx path ignores extra padding.
	 * and some hardware doesn't like to write zlps.
	 */
	req->zero = 1;
	if (!dev->zlp && (length % dev->in_ep->maxpacket) == 0)
		length++;

	req->length = length;

#ifdef	CONFIG_USB_GADGET_DUALSPEED
	/* throttle highspeed IRQ rate back slightly */
	req->no_interrupt = (dev->gadget->speed == USB_SPEED_HIGH)
		? ((atomic_read (&dev->tx_qlen) % TX_DELAY) != 0)
		: 0;
#endif

	retval = usb_ep_queue (dev->in_ep, req, GFP_ATOMIC);
	switch (retval) {
	default:
		DEBUG (dev, "tx queue err %d\n", retval);
		break;
	case 0:
		net->trans_start = jiffies;
		atomic_inc (&dev->tx_qlen);
	}

	if (retval) {
#ifdef CONFIG_USB_ETH_RNDIS
drop:
#endif
		dev->stats.tx_dropped++;
		dev_kfree_skb_any (skb);
		spin_lock_irqsave (&dev->lock, flags);
		if (list_empty (&dev->tx_reqs))
			netif_start_queue (net);
		list_add (&req->list, &dev->tx_reqs);
		spin_unlock_irqrestore (&dev->lock, flags);
	}
	return 0;
}

/*-------------------------------------------------------------------------*/

#ifdef CONFIG_USB_ETH_RNDIS

static void rndis_send_media_state (struct eth_dev *dev, int connect)
{
	if (!dev)
		return;
	
	if (connect) {
		if (rndis_signal_connect (dev->rndis_config))
			return;
	} else {
		if (rndis_signal_disconnect (dev->rndis_config))
			return;
	}
}

static void rndis_control_ack_complete (struct usb_ep *ep, struct usb_request *req)
{
	if (req->status || req->actual != req->length)
		DEBUG (dev, "rndis control ack complete --> %d, %d/%d\n",
		       req->status, req->actual, req->length);

	usb_ep_free_buffer(ep, req->buf, req->dma, 8);
	usb_ep_free_request(ep, req);
}

static int rndis_control_ack (struct net_device *net)
{
	struct eth_dev          *dev = (struct eth_dev *) net->priv;
	u32                     length;
	struct usb_request      *resp;
	
	/* in case RNDIS calls this after disconnect */
	if (!dev->status_ep) {
		DEBUG (dev, "status ENODEV\n");
		return -ENODEV;
	}

	/* Allocate memory for notification ie. ACK */
	resp = usb_ep_alloc_request (dev->status_ep, GFP_ATOMIC);
	if (!resp) {
		DEBUG (dev, "status ENOMEM\n");
		return -ENOMEM;
	}
	
	resp->buf = usb_ep_alloc_buffer (dev->status_ep, 8,
					 &resp->dma, GFP_ATOMIC);
	if (!resp->buf) {
		DEBUG (dev, "status buf ENOMEM\n");
		usb_ep_free_request (dev->status_ep, resp);
		return -ENOMEM;
	}
	
	/* Send RNDIS RESPONSE_AVAILABLE notification;
	 * CDC_NOTIFY_RESPONSE_AVAILABLE should work too
	 */
	resp->length = 8;
	resp->complete = rndis_control_ack_complete;
	
	*((u32 *) resp->buf) = __constant_cpu_to_le32 (1);
	*((u32 *) resp->buf + 1) = __constant_cpu_to_le32 (0);
	
	length = usb_ep_queue (dev->status_ep, resp, GFP_ATOMIC);
	if (length < 0) {
		resp->status = 0;
		rndis_control_ack_complete (dev->status_ep, resp);
	}
	
	return 0;
}

#endif	/* RNDIS */

static void eth_start (struct eth_dev *dev, int gfp_flags)
{
	DEBUG (dev, "%s\n", __FUNCTION__);

	/* fill the rx queue */
	rx_fill (dev, gfp_flags);

	/* and open the tx floodgates */ 
	atomic_set (&dev->tx_qlen, 0);
	netif_wake_queue (dev->net);
#ifdef CONFIG_USB_ETH_RNDIS
	if (dev->rndis) {
		rndis_set_param_medium (dev->rndis_config,
					NDIS_MEDIUM_802_3,
					BITRATE(dev->gadget));
		rndis_send_media_state (dev, 1);
	}
#endif	
}

static int eth_open (struct net_device *net)
{
	struct eth_dev		*dev = (struct eth_dev *) net->priv;

	DEBUG (dev, "%s\n", __FUNCTION__);
	if (netif_carrier_ok (dev->net))
		eth_start (dev, GFP_KERNEL);
	return 0;
}

static int eth_stop (struct net_device *net)
{
	struct eth_dev		*dev = (struct eth_dev *) net->priv;

	VDEBUG (dev, "%s\n", __FUNCTION__);
	netif_stop_queue (net);

	DEBUG (dev, "stop stats: rx/tx %ld/%ld, errs %ld/%ld\n",
		dev->stats.rx_packets, dev->stats.tx_packets, 
		dev->stats.rx_errors, dev->stats.tx_errors
		);

	/* ensure there are no more active requests */
	if (dev->gadget->speed != USB_SPEED_UNKNOWN) {
		usb_ep_disable (dev->in_ep);
		usb_ep_disable (dev->out_ep);
		if (netif_carrier_ok (dev->net)) {
			DEBUG (dev, "host still using in/out endpoints\n");
			// FIXME idiom may leave toggle wrong here
			usb_ep_enable (dev->in_ep, dev->in);
			usb_ep_enable (dev->out_ep, dev->out);
		}
		if (dev->status_ep) {
			usb_ep_disable (dev->status_ep);
			usb_ep_enable (dev->status_ep, dev->status);
		}
	}
	
#ifdef	CONFIG_USB_ETH_RNDIS
	if (dev->rndis) {
		rndis_set_param_medium (dev->rndis_config,
					NDIS_MEDIUM_802_3, 0);
		rndis_send_media_state (dev, 0);
	}
#endif

	return 0;
}

/*-------------------------------------------------------------------------*/

static void
eth_unbind (struct usb_gadget *gadget)
{
	struct eth_dev		*dev = get_gadget_data (gadget);

	DEBUG (dev, "unbind\n");
#ifdef CONFIG_USB_ETH_RNDIS
	rndis_deregister (dev->rndis_config);
	rndis_exit ();
#endif

	/* we've already been disconnected ... no i/o is active */
	if (dev->req) {
		usb_ep_free_buffer (gadget->ep0,
				dev->req->buf, dev->req->dma,
				USB_BUFSIZ);
		usb_ep_free_request (gadget->ep0, dev->req);
		dev->req = NULL;
	}

	unregister_netdev (dev->net);
	free_netdev(dev->net);

	/* assuming we used keventd, it must quiesce too */
	flush_scheduled_work ();
	set_gadget_data (gadget, NULL);
}

static u8 __init nibble (unsigned char c)
{
	if (likely (isdigit (c)))
		return c - '0';
	c = toupper (c);
	if (likely (isxdigit (c)))
		return 10 + c - 'A';
	return 0;
}

static void __init get_ether_addr (const char *str, u8 *dev_addr)
{
	if (str) {
		unsigned	i;

		for (i = 0; i < 6; i++) {
			unsigned char num;

			if((*str == '.') || (*str == ':'))
				str++;
			num = nibble(*str++) << 4;
			num |= (nibble(*str++));
			dev_addr [i] = num;
		}
		if (is_valid_ether_addr (dev_addr))
			return;
	}
	random_ether_addr(dev_addr);
}

static int __init
eth_bind (struct usb_gadget *gadget)
{
	struct eth_dev		*dev;
	struct net_device	*net;
	u8			cdc = 1, zlp = 1, rndis = 1;
	struct usb_ep		*ep;
	int			status = -ENOMEM;

	/* these flags are only ever cleared; compiler take note */
#ifndef	DEV_CONFIG_CDC
	cdc = 0;
#endif
#ifndef	CONFIG_USB_ETH_RNDIS
	rndis = 0;
#endif

	/* Because most host side USB stacks handle CDC Ethernet, that
	 * standard protocol is _strongly_ preferred for interop purposes.
	 * (By everyone except Microsoft.)
	 */
	if (gadget_is_net2280 (gadget)) {
		device_desc.bcdDevice = __constant_cpu_to_le16 (0x0201);
	} else if (gadget_is_dummy (gadget)) {
		device_desc.bcdDevice = __constant_cpu_to_le16 (0x0202);
	} else if (gadget_is_pxa (gadget)) {
		device_desc.bcdDevice = __constant_cpu_to_le16 (0x0203);
		/* pxa doesn't support altsettings */
		cdc = 0;
	} else if (gadget_is_sh(gadget)) {
		device_desc.bcdDevice = __constant_cpu_to_le16 (0x0204);
		/* sh doesn't support multiple interfaces or configs */
		cdc = 0;
		rndis = 0;
	} else if (gadget_is_sa1100 (gadget)) {
		device_desc.bcdDevice = __constant_cpu_to_le16 (0x0205);
		/* hardware can't write zlps */
		zlp = 0;
		/* sa1100 CAN do CDC, without status endpoint ... we use
		 * non-CDC to be compatible with ARM Linux-2.4 "usb-eth".
		 */
		cdc = 0;
	} else if (gadget_is_goku (gadget)) {
		device_desc.bcdDevice = __constant_cpu_to_le16 (0x0206);
	} else if (gadget_is_mq11xx (gadget)) {
		device_desc.bcdDevice = __constant_cpu_to_le16 (0x0207);
	} else if (gadget_is_omap (gadget)) {
		device_desc.bcdDevice = __constant_cpu_to_le16 (0x0208);
	} else {
		/* can't assume CDC works.  don't want to default to
		 * anything less functional on CDC-capable hardware,
		 * so we fail in this case.
		 */
		dev_err (&gadget->dev,
			"controller '%s' not recognized\n",
			gadget->name);
		return -ENODEV;
	}
	snprintf (manufacturer, sizeof manufacturer,
		UTS_SYSNAME " " UTS_RELEASE "/%s",
		gadget->name);

	/* If there's an RNDIS configuration, that's what Windows wants to
	 * be using ... so use these product IDs here and in the "linux.inf"
	 * needed to install MSFT drivers.  Current Linux kernels will use
	 * the second configuration if it's CDC Ethernet, and need some help
	 * to choose the right configuration otherwise.
	 */
	if (rndis) {
		device_desc.idVendor =
			__constant_cpu_to_le16(RNDIS_VENDOR_NUM);
		device_desc.idProduct =
			__constant_cpu_to_le16(RNDIS_PRODUCT_NUM);
		snprintf (product_desc, sizeof product_desc,
			"RNDIS/%s", driver_desc);

	/* CDC subset ... recognized by Linux since 2.4.10, but Windows
	 * drivers aren't widely available.
	 */
	} else if (!cdc) {
		device_desc.bDeviceClass = USB_CLASS_VENDOR_SPEC;
		device_desc.idVendor =
			__constant_cpu_to_le16(SIMPLE_VENDOR_NUM);
		device_desc.idProduct =
			__constant_cpu_to_le16(SIMPLE_PRODUCT_NUM);
	}

	/* support optional vendor/distro customization */
	if (idVendor) {
		if (!idProduct) {
			dev_err (&gadget->dev, "idVendor needs idProduct!\n");
			return -ENODEV;
		}
		device_desc.idVendor = cpu_to_le16(idVendor);
		device_desc.idProduct = cpu_to_le16(idProduct);
		if (bcdDevice)
			device_desc.bcdDevice = cpu_to_le16(bcdDevice);
	}
	if (iManufacturer)
		strlcpy (manufacturer, iManufacturer, sizeof manufacturer);
	if (iProduct)
		strlcpy (product_desc, iProduct, sizeof product_desc);

	/* all we really need is bulk IN/OUT */
	usb_ep_autoconfig_reset (gadget);
	ep = usb_ep_autoconfig (gadget, &fs_source_desc);
	if (!ep) {
autoconf_fail:
		dev_err (&gadget->dev,
			"can't autoconfigure on %s\n",
			gadget->name);
		return -ENODEV;
	}
	EP_IN_NAME = ep->name;
	ep->driver_data = ep;	/* claim */
	
	ep = usb_ep_autoconfig (gadget, &fs_sink_desc);
	if (!ep)
		goto autoconf_fail;
	EP_OUT_NAME = ep->name;
	ep->driver_data = ep;	/* claim */

#if defined(DEV_CONFIG_CDC) || defined(CONFIG_USB_ETH_RNDIS)
	/* CDC Ethernet control interface doesn't require a status endpoint.
	 * Since some hosts expect one, try to allocate one anyway.
	 */
	if (cdc || rndis) {
		ep = usb_ep_autoconfig (gadget, &fs_status_desc);
		if (ep) {
			EP_STATUS_NAME = ep->name;
			ep->driver_data = ep;	/* claim */
		} else if (rndis) {
			dev_err (&gadget->dev,
				"can't run RNDIS on %s\n",
				gadget->name);
			return -ENODEV;
		} else if (cdc) {
			control_intf.bNumEndpoints = 0;
			/* FIXME remove endpoint from descriptor list */
		}
	}
#endif

	/* one config:  cdc, else minimal subset */
	if (!cdc) {
		eth_config.bNumInterfaces = 1;
		eth_config.iConfiguration = STRING_SUBSET;
		fs_subset_descriptors();
		hs_subset_descriptors();
	}

	/* For now RNDIS is always a second config */
	if (rndis)
		device_desc.bNumConfigurations = 2;

#ifdef	CONFIG_USB_GADGET_DUALSPEED
	if (rndis)
		dev_qualifier.bNumConfigurations = 2;
	else if (!cdc)
		dev_qualifier.bDeviceClass = USB_CLASS_VENDOR_SPEC;

	/* assumes ep0 uses the same value for both speeds ... */
	dev_qualifier.bMaxPacketSize0 = device_desc.bMaxPacketSize0;

	/* and that all endpoints are dual-speed */
	hs_source_desc.bEndpointAddress = fs_source_desc.bEndpointAddress;
	hs_sink_desc.bEndpointAddress = fs_sink_desc.bEndpointAddress;
#if defined(DEV_CONFIG_CDC) || defined(CONFIG_USB_ETH_RNDIS)
	if (EP_STATUS_NAME)
		hs_status_desc.bEndpointAddress =
				fs_status_desc.bEndpointAddress;
#endif
#endif	/* DUALSPEED */

	device_desc.bMaxPacketSize0 = gadget->ep0->maxpacket;
	usb_gadget_set_selfpowered (gadget);

	if (gadget->is_otg) {
		otg_descriptor.bmAttributes |= USB_OTG_HNP,
		eth_config.bmAttributes |= USB_CONFIG_ATT_WAKEUP;
#ifdef	CONFIG_USB_ETH_RNDIS
		rndis_config.bmAttributes |= USB_CONFIG_ATT_WAKEUP;
#endif
	}

 	net = alloc_etherdev (sizeof *dev);
 	if (!net)
		return status;
	dev = net->priv;
	spin_lock_init (&dev->lock);
	INIT_WORK (&dev->work, eth_work, dev);
	INIT_LIST_HEAD (&dev->tx_reqs);
	INIT_LIST_HEAD (&dev->rx_reqs);

	/* network device setup */
	dev->net = net;
	SET_MODULE_OWNER (net);
	strcpy (net->name, "usb%d");
	dev->cdc = cdc;
	dev->zlp = zlp;

	/* Module params for these addresses should come from ID proms.
	 * The host side address is used with CDC and RNDIS, and commonly
	 * ends up in a persistent config database.
	 */
	get_ether_addr(dev_addr, net->dev_addr);
	if (cdc || rndis) {
		get_ether_addr(host_addr, dev->host_mac);
#ifdef	DEV_CONFIG_CDC
		snprintf (ethaddr, sizeof ethaddr, "%02X%02X%02X%02X%02X%02X",
			dev->host_mac [0], dev->host_mac [1],
			dev->host_mac [2], dev->host_mac [3],
			dev->host_mac [4], dev->host_mac [5]);
#endif
	}

	if (rndis) {
		status = rndis_init();
		if (status < 0) {
			dev_err (&gadget->dev, "can't init RNDIS, %d\n",
				status);
			goto fail;
		}
	}

	net->change_mtu = eth_change_mtu;
	net->get_stats = eth_get_stats;
	net->hard_start_xmit = eth_start_xmit;
	net->open = eth_open;
	net->stop = eth_stop;
	// watchdog_timeo, tx_timeout ...
	// set_multicast_list
	net->do_ioctl = eth_ioctl;

	/* preallocate control response and buffer */
	dev->req = usb_ep_alloc_request (gadget->ep0, GFP_KERNEL);
	if (!dev->req)
		goto fail;
	dev->req->complete = eth_setup_complete;
	dev->req->buf = usb_ep_alloc_buffer (gadget->ep0, USB_BUFSIZ,
				&dev->req->dma, GFP_KERNEL);
	if (!dev->req->buf) {
		usb_ep_free_request (gadget->ep0, dev->req);
		goto fail;
	}

	/* finish hookup to lower layer ... */
	dev->gadget = gadget;
	set_gadget_data (gadget, dev);
	gadget->ep0->driver_data = dev;
	
	/* two kinds of host-initiated state changes:
	 *  - iff DATA transfer is active, carrier is "on"
	 *  - tx queueing enabled if open *and* carrier is "on"
	 */
	netif_stop_queue (dev->net);
	netif_carrier_off (dev->net);

 	// SET_NETDEV_DEV (dev->net, &gadget->dev);
 	status = register_netdev (dev->net);
	if (status < 0)
		goto fail1;

	INFO (dev, "%s, version: " DRIVER_VERSION "\n", driver_desc);
	INFO (dev, "using %s, OUT %s IN %s%s%s\n", gadget->name,
		EP_OUT_NAME, EP_IN_NAME,
		EP_STATUS_NAME ? " STATUS " : "",
		EP_STATUS_NAME ? EP_STATUS_NAME : ""
		);
	INFO (dev, "MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
		net->dev_addr [0], net->dev_addr [1],
		net->dev_addr [2], net->dev_addr [3],
		net->dev_addr [4], net->dev_addr [5]);

	if (cdc || rndis)
		INFO (dev, "HOST MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
			dev->host_mac [0], dev->host_mac [1],
			dev->host_mac [2], dev->host_mac [3],
			dev->host_mac [4], dev->host_mac [5]);

#ifdef	CONFIG_USB_ETH_RNDIS
	if (rndis) {
		u32	vendorID = 0;

		/* FIXME RNDIS vendor id == "vendor NIC code" == ? */
		
		dev->rndis_config = rndis_register (rndis_control_ack);
		if (dev->rndis_config < 0) {
fail0:
			unregister_netdev (dev->net);
			status = -ENODEV;
			goto fail;
		}
		
		/* these set up a lot of the OIDs that RNDIS needs */
		rndis_set_host_mac (dev->rndis_config, dev->host_mac);
		if (rndis_set_param_dev (dev->rndis_config, dev->net,
					 &dev->stats))
			goto fail0;
		if (rndis_set_param_vendor (dev->rndis_config, vendorID,
					    manufacturer))
			goto fail0;
		if (rndis_set_param_medium (dev->rndis_config,
					    NDIS_MEDIUM_802_3,
					    0))
			goto fail0;
		INFO (dev, "RNDIS ready\n");
	}
#endif	

	return status;

fail1:
	dev_dbg(&gadget->dev, "register_netdev failed, %d\n", status);
fail:
	eth_unbind (gadget);
	return status;
}

/*-------------------------------------------------------------------------*/

static void
eth_suspend (struct usb_gadget *gadget)
{
	struct eth_dev		*dev = get_gadget_data (gadget);

	DEBUG (dev, "suspend\n");
	dev->suspended = 1;
}

static void
eth_resume (struct usb_gadget *gadget)
{
	struct eth_dev		*dev = get_gadget_data (gadget);

	DEBUG (dev, "resume\n");
	dev->suspended = 0;
}

/*-------------------------------------------------------------------------*/

static struct usb_gadget_driver eth_driver = {
#ifdef CONFIG_USB_GADGET_DUALSPEED
	.speed		= USB_SPEED_HIGH,
#else
	.speed		= USB_SPEED_FULL,
#endif
	.function	= (char *) driver_desc,
	.bind		= eth_bind,
	.unbind		= eth_unbind,

	.setup		= eth_setup,
	.disconnect	= eth_disconnect,

	.suspend	= eth_suspend,
	.resume		= eth_resume,

	.driver 	= {
		.name		= (char *) shortname,
		// .shutdown = ...
		// .suspend = ...
		// .resume = ...
	},
};

MODULE_DESCRIPTION (DRIVER_DESC);
MODULE_AUTHOR ("David Brownell, Benedikt Spanger");
MODULE_LICENSE ("GPL");


static int __init init (void)
{
	return usb_gadget_register_driver (&eth_driver);
}
module_init (init);

static void __exit cleanup (void)
{
	usb_gadget_unregister_driver (&eth_driver);
}
module_exit (cleanup);

