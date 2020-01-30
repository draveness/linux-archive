/*
 * OHCI HCD (Host Controller Driver) for USB.
 *
 * (C) Copyright 1999 Roman Weissgaerber <weissg@vienna.at>
 * (C) Copyright 2000-2002 David Brownell <dbrownell@users.sourceforge.net>
 * (C) Copyright 2002 Hewlett-Packard Company
 *
 * Bus Glue for AMD Alchemy Au1xxx
 *
 * Written by Christopher Hoover <ch@hpl.hp.com>
 * Based on fragments of previous driver by Rusell King et al.
 *
 * Modified for LH7A404 from ohci-sa1111.c
 *  by Durgesh Pattamatta <pattamattad@sharpsec.com>
 * Modified for AMD Alchemy Au1xxx
 *  by Matt Porter <mporter@kernel.crashing.org>
 *
 * This file is licenced under the GPL.
 */

#include <linux/platform_device.h>
#include <linux/signal.h>

#include <asm/mach-au1x00/au1000.h>

#ifndef	CONFIG_SOC_AU1200

#define USBH_ENABLE_BE (1<<0)
#define USBH_ENABLE_C  (1<<1)
#define USBH_ENABLE_E  (1<<2)
#define USBH_ENABLE_CE (1<<3)
#define USBH_ENABLE_RD (1<<4)

#ifdef __LITTLE_ENDIAN
#define USBH_ENABLE_INIT (USBH_ENABLE_CE | USBH_ENABLE_E | USBH_ENABLE_C)
#elif __BIG_ENDIAN
#define USBH_ENABLE_INIT (USBH_ENABLE_CE | USBH_ENABLE_E | USBH_ENABLE_C | USBH_ENABLE_BE)
#else
#error not byte order defined
#endif

#else   /* Au1200 */

#define USB_HOST_CONFIG    (USB_MSR_BASE + USB_MSR_MCFG)
#define USB_MCFG_PFEN     (1<<31)
#define USB_MCFG_RDCOMB   (1<<30)
#define USB_MCFG_SSDEN    (1<<23)
#define USB_MCFG_OHCCLKEN (1<<16)
#define USB_MCFG_UCAM     (1<<7)
#define USB_MCFG_OBMEN    (1<<1)
#define USB_MCFG_OMEMEN   (1<<0)

#define USBH_ENABLE_CE    USB_MCFG_OHCCLKEN
#ifdef CONFIG_DMA_COHERENT
#define USBH_ENABLE_INIT  (USB_MCFG_OHCCLKEN \
                         | USB_MCFG_PFEN | USB_MCFG_RDCOMB \
                         | USB_MCFG_SSDEN | USB_MCFG_UCAM \
                         | USB_MCFG_OBMEN | USB_MCFG_OMEMEN)
#else
#define USBH_ENABLE_INIT  (USB_MCFG_OHCCLKEN \
                         | USB_MCFG_PFEN | USB_MCFG_RDCOMB \
                         | USB_MCFG_SSDEN \
                         | USB_MCFG_OBMEN | USB_MCFG_OMEMEN)
#endif
#define USBH_DISABLE      (USB_MCFG_OBMEN | USB_MCFG_OMEMEN)

#endif  /* Au1200 */

extern int usb_disabled(void);

/*-------------------------------------------------------------------------*/

static void au1xxx_start_ohc(struct platform_device *dev)
{
	printk(KERN_DEBUG __FILE__
		": starting Au1xxx OHCI USB Controller\n");

	/* enable host controller */

#ifndef CONFIG_SOC_AU1200

	au_writel(USBH_ENABLE_CE, USB_HOST_CONFIG);
	udelay(1000);
	au_writel(USBH_ENABLE_INIT, USB_HOST_CONFIG);
	udelay(1000);

#else   /* Au1200 */

	/* write HW defaults again in case Yamon cleared them */
	if (au_readl(USB_HOST_CONFIG) == 0) {
		au_writel(0x00d02000, USB_HOST_CONFIG);
		au_readl(USB_HOST_CONFIG);
		udelay(1000);
	}
	au_writel(USBH_ENABLE_CE | au_readl(USB_HOST_CONFIG), USB_HOST_CONFIG);
	au_readl(USB_HOST_CONFIG);
	udelay(1000);
	au_writel(USBH_ENABLE_INIT | au_readl(USB_HOST_CONFIG), USB_HOST_CONFIG);
	au_readl(USB_HOST_CONFIG);
	udelay(1000);

#endif  /* Au1200 */

#ifndef CONFIG_SOC_AU1200
	/* wait for reset complete (read register twice; see au1500 errata) */
	while (au_readl(USB_HOST_CONFIG),
		!(au_readl(USB_HOST_CONFIG) & USBH_ENABLE_RD))
#endif
		udelay(1000);

	printk(KERN_DEBUG __FILE__
	": Clock to USB host has been enabled \n");
}

static void au1xxx_stop_ohc(struct platform_device *dev)
{
	printk(KERN_DEBUG __FILE__
	       ": stopping Au1xxx OHCI USB Controller\n");

#ifndef CONFIG_SOC_AU1200

	/* Disable clock */
	au_writel(au_readl(USB_HOST_CONFIG) & ~USBH_ENABLE_CE, USB_HOST_CONFIG);

#else   /* Au1200 */

	/* Disable mem */
	au_writel(~USBH_DISABLE & au_readl(USB_HOST_CONFIG), USB_HOST_CONFIG);
	udelay(1000);
	/* Disable clock */
	au_writel(~USBH_ENABLE_CE & au_readl(USB_HOST_CONFIG), USB_HOST_CONFIG);
	au_readl(USB_HOST_CONFIG);
#endif  /* Au1200 */
}


/*-------------------------------------------------------------------------*/

/* configure so an HC device and id are always provided */
/* always called with process context; sleeping is OK */


/**
 * usb_ohci_au1xxx_probe - initialize Au1xxx-based HCDs
 * Context: !in_interrupt()
 *
 * Allocates basic resources for this USB host controller, and
 * then invokes the start() method for the HCD associated with it
 * through the hotplug entry's driver_data.
 *
 */
static int usb_ohci_au1xxx_probe(const struct hc_driver *driver,
			  struct platform_device *dev)
{
	int retval;
	struct usb_hcd *hcd;

#if defined(CONFIG_SOC_AU1200) && defined(CONFIG_DMA_COHERENT)
	/* Au1200 AB USB does not support coherent memory */
	if (!(read_c0_prid() & 0xff)) {
		pr_info("%s: this is chip revision AB !!\n",
			dev->name);
		pr_info("%s: update your board or re-configure the kernel\n",
			dev->name);
		return -ENODEV;
	}
#endif

	if (dev->resource[1].flags != IORESOURCE_IRQ) {
		pr_debug("resource[1] is not IORESOURCE_IRQ\n");
		return -ENOMEM;
	}

	hcd = usb_create_hcd(driver, &dev->dev, "au1xxx");
	if (!hcd)
		return -ENOMEM;
	hcd->rsrc_start = dev->resource[0].start;
	hcd->rsrc_len = dev->resource[0].end - dev->resource[0].start + 1;

	if (!request_mem_region(hcd->rsrc_start, hcd->rsrc_len, hcd_name)) {
		pr_debug("request_mem_region failed\n");
		retval = -EBUSY;
		goto err1;
	}

	hcd->regs = ioremap(hcd->rsrc_start, hcd->rsrc_len);
	if (!hcd->regs) {
		pr_debug("ioremap failed\n");
		retval = -ENOMEM;
		goto err2;
	}

	au1xxx_start_ohc(dev);
	ohci_hcd_init(hcd_to_ohci(hcd));

	retval = usb_add_hcd(hcd, dev->resource[1].start, IRQF_DISABLED | IRQF_SHARED);
	if (retval == 0)
		return retval;

	au1xxx_stop_ohc(dev);
	iounmap(hcd->regs);
 err2:
	release_mem_region(hcd->rsrc_start, hcd->rsrc_len);
 err1:
	usb_put_hcd(hcd);
	return retval;
}


/* may be called without controller electrically present */
/* may be called with controller, bus, and devices active */

/**
 * usb_hcd_au1xxx_remove - shutdown processing for Au1xxx-based HCDs
 * @dev: USB Host Controller being removed
 * Context: !in_interrupt()
 *
 * Reverses the effect of usb_hcd_au1xxx_probe(), first invoking
 * the HCD's stop() method.  It is always called from a thread
 * context, normally "rmmod", "apmd", or something similar.
 *
 */
static void usb_ohci_au1xxx_remove(struct usb_hcd *hcd, struct platform_device *dev)
{
	usb_remove_hcd(hcd);
	au1xxx_stop_ohc(dev);
	iounmap(hcd->regs);
	release_mem_region(hcd->rsrc_start, hcd->rsrc_len);
	usb_put_hcd(hcd);
}

/*-------------------------------------------------------------------------*/

static int __devinit
ohci_au1xxx_start (struct usb_hcd *hcd)
{
	struct ohci_hcd	*ohci = hcd_to_ohci (hcd);
	int		ret;

	ohci_dbg (ohci, "ohci_au1xxx_start, ohci:%p", ohci);

	if ((ret = ohci_init (ohci)) < 0)
		return ret;

	if ((ret = ohci_run (ohci)) < 0) {
		err ("can't start %s", hcd->self.bus_name);
		ohci_stop (hcd);
		return ret;
	}

	return 0;
}

/*-------------------------------------------------------------------------*/

static const struct hc_driver ohci_au1xxx_hc_driver = {
	.description =		hcd_name,
	.product_desc =		"Au1xxx OHCI",
	.hcd_priv_size =	sizeof(struct ohci_hcd),

	/*
	 * generic hardware linkage
	 */
	.irq =			ohci_irq,
	.flags =		HCD_USB11 | HCD_MEMORY,

	/*
	 * basic lifecycle operations
	 */
	.start =		ohci_au1xxx_start,
	.stop =			ohci_stop,
	.shutdown =		ohci_shutdown,

	/*
	 * managing i/o requests and associated device resources
	 */
	.urb_enqueue =		ohci_urb_enqueue,
	.urb_dequeue =		ohci_urb_dequeue,
	.endpoint_disable =	ohci_endpoint_disable,

	/*
	 * scheduling support
	 */
	.get_frame_number =	ohci_get_frame,

	/*
	 * root hub support
	 */
	.hub_status_data =	ohci_hub_status_data,
	.hub_control =		ohci_hub_control,
	.hub_irq_enable =	ohci_rhsc_enable,
#ifdef	CONFIG_PM
	.bus_suspend =		ohci_bus_suspend,
	.bus_resume =		ohci_bus_resume,
#endif
	.start_port_reset =	ohci_start_port_reset,
};

/*-------------------------------------------------------------------------*/

static int ohci_hcd_au1xxx_drv_probe(struct platform_device *pdev)
{
	int ret;

	pr_debug ("In ohci_hcd_au1xxx_drv_probe");

	if (usb_disabled())
		return -ENODEV;

	ret = usb_ohci_au1xxx_probe(&ohci_au1xxx_hc_driver, pdev);
	return ret;
}

static int ohci_hcd_au1xxx_drv_remove(struct platform_device *pdev)
{
	struct usb_hcd *hcd = platform_get_drvdata(pdev);

	usb_ohci_au1xxx_remove(hcd, pdev);
	return 0;
}
	/*TBD*/
/*static int ohci_hcd_au1xxx_drv_suspend(struct platform_device *dev)
{
	struct usb_hcd *hcd = platform_get_drvdata(dev);

	return 0;
}
static int ohci_hcd_au1xxx_drv_resume(struct platform_device *dev)
{
	struct usb_hcd *hcd = platform_get_drvdata(dev);

	return 0;
}
*/

static struct platform_driver ohci_hcd_au1xxx_driver = {
	.probe		= ohci_hcd_au1xxx_drv_probe,
	.remove		= ohci_hcd_au1xxx_drv_remove,
	.shutdown	= usb_hcd_platform_shutdown,
	/*.suspend	= ohci_hcd_au1xxx_drv_suspend, */
	/*.resume	= ohci_hcd_au1xxx_drv_resume, */
	.driver		= {
		.name	= "au1xxx-ohci",
		.owner	= THIS_MODULE,
	},
};

