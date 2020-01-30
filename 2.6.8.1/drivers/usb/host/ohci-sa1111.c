/*
 * OHCI HCD (Host Controller Driver) for USB.
 *
 * (C) Copyright 1999 Roman Weissgaerber <weissg@vienna.at>
 * (C) Copyright 2000-2002 David Brownell <dbrownell@users.sourceforge.net>
 * (C) Copyright 2002 Hewlett-Packard Company
 * 
 * SA1111 Bus Glue
 *
 * Written by Christopher Hoover <ch@hpl.hp.com>
 * Based on fragments of previous driver by Rusell King et al.
 *
 * This file is licenced under the GPL.
 */
 
#include <asm/hardware.h>
#include <asm/mach-types.h>
#include <asm/arch/assabet.h>
#include <asm/arch/badge4.h>
#include <asm/hardware/sa1111.h>

#ifndef CONFIG_SA1111
#error "This file is SA-1111 bus glue.  CONFIG_SA1111 must be defined."
#endif

extern int usb_disabled(void);

/*-------------------------------------------------------------------------*/

static void sa1111_start_hc(struct sa1111_dev *dev)
{
	unsigned int usb_rst = 0;

	printk(KERN_DEBUG __FILE__ 
	       ": starting SA-1111 OHCI USB Controller\n");

#ifdef CONFIG_SA1100_BADGE4
	if (machine_is_badge4()) {
		badge4_set_5V(BADGE4_5V_USB, 1);
	}
#endif

	if (machine_is_xp860() ||
	    machine_has_neponset() ||
	    machine_is_pfs168() ||
	    machine_is_badge4())
		usb_rst = USB_RESET_PWRSENSELOW | USB_RESET_PWRCTRLLOW;

	/*
	 * Configure the power sense and control lines.  Place the USB
	 * host controller in reset.
	 */
	sa1111_writel(usb_rst | USB_RESET_FORCEIFRESET | USB_RESET_FORCEHCRESET,
		      dev->mapbase + SA1111_USB_RESET);

	/*
	 * Now, carefully enable the USB clock, and take
	 * the USB host controller out of reset.
	 */
	sa1111_enable_device(dev);
	udelay(11);
	sa1111_writel(usb_rst, dev->mapbase + SA1111_USB_RESET);
}

static void sa1111_stop_hc(struct sa1111_dev *dev)
{
	unsigned int usb_rst;
	printk(KERN_DEBUG __FILE__ 
	       ": stopping SA-1111 OHCI USB Controller\n");

	/*
	 * Put the USB host controller into reset.
	 */
	usb_rst = sa1111_readl(dev->mapbase + SA1111_USB_RESET);
	sa1111_writel(usb_rst | USB_RESET_FORCEIFRESET | USB_RESET_FORCEHCRESET,
		      dev->mapbase + SA1111_USB_RESET);

	/*
	 * Stop the USB clock.
	 */
	sa1111_disable_device(dev);

#ifdef CONFIG_SA1100_BADGE4
	if (machine_is_badge4()) {
		/* Disable power to the USB bus */
		badge4_set_5V(BADGE4_5V_USB, 0);
	}
#endif
}


/*-------------------------------------------------------------------------*/

#if 0
static void dump_hci_status(struct usb_hcd *hcd, const char *label)
{
	unsigned long status = sa1111_readl(hcd->regs + SA1111_USB_STATUS);

	dbg ("%s USB_STATUS = { %s%s%s%s%s}", label,
	     ((status & USB_STATUS_IRQHCIRMTWKUP) ? "IRQHCIRMTWKUP " : ""),
	     ((status & USB_STATUS_IRQHCIBUFFACC) ? "IRQHCIBUFFACC " : ""),
	     ((status & USB_STATUS_NIRQHCIM) ? "" : "IRQHCIM "),
	     ((status & USB_STATUS_NHCIMFCLR) ? "" : "HCIMFCLR "),
	     ((status & USB_STATUS_USBPWRSENSE) ? "USBPWRSENSE " : ""));
}
#endif

static irqreturn_t usb_hcd_sa1111_hcim_irq (int irq, void *__hcd, struct pt_regs * r)
{
	struct usb_hcd *hcd = __hcd;
//	unsigned long status = sa1111_readl(hcd->regs + SA1111_USB_STATUS);

	//dump_hci_status(hcd, "irq");

#if 0
	/* may work better this way -- need to investigate further */
	if (status & USB_STATUS_NIRQHCIM) {
		//dbg ("not normal HC interrupt; ignoring");
		return;
	}
#endif

	usb_hcd_irq(irq, hcd, r);

	/*
	 * SA1111 seems to re-assert its interrupt immediately
	 * after processing an interrupt.  Always return IRQ_HANDLED.
	 */
	return IRQ_HANDLED;
}

/*-------------------------------------------------------------------------*/

void usb_hcd_sa1111_remove (struct usb_hcd *, struct sa1111_dev *);

/* configure so an HC device and id are always provided */
/* always called with process context; sleeping is OK */


/**
 * usb_hcd_sa1111_probe - initialize SA-1111-based HCDs
 * Context: !in_interrupt()
 *
 * Allocates basic resources for this USB host controller, and
 * then invokes the start() method for the HCD associated with it
 * through the hotplug entry's driver_data.
 *
 * Store this function in the HCD's struct pci_driver as probe().
 */
int usb_hcd_sa1111_probe (const struct hc_driver *driver,
			  struct usb_hcd **hcd_out,
			  struct sa1111_dev *dev)
{
	int retval;
	struct usb_hcd *hcd = 0;

	if (!request_mem_region(dev->res.start, 
				dev->res.end - dev->res.start + 1, hcd_name)) {
		dbg("request_mem_region failed");
		return -EBUSY;
	}

	sa1111_start_hc(dev);

	hcd = driver->hcd_alloc ();
	if (hcd == NULL){
		dbg ("hcd_alloc failed");
		retval = -ENOMEM;
		goto err1;
	}

	hcd->driver = (struct hc_driver *) driver;
	hcd->description = driver->description;
	hcd->irq = dev->irq[1];
	hcd->regs = dev->mapbase;
	hcd->self.controller = &dev->dev;

	retval = hcd_buffer_create (hcd);
	if (retval != 0) {
		dbg ("pool alloc fail");
		goto err1;
	}

	retval = request_irq (hcd->irq, usb_hcd_sa1111_hcim_irq, SA_INTERRUPT,
			      hcd->description, hcd);
	if (retval != 0) {
		dbg("request_irq failed");
		retval = -EBUSY;
		goto err2;
	}

	info ("%s (SA-1111) at 0x%p, irq %d\n",
	      hcd->description, hcd->regs, hcd->irq);

	usb_bus_init (&hcd->self);
	hcd->self.op = &usb_hcd_operations;
	hcd->self.hcpriv = (void *) hcd;
	hcd->self.bus_name = "sa1111";
	hcd->product_desc = "SA-1111 OHCI";

	INIT_LIST_HEAD (&hcd->dev_list);

	usb_register_bus (&hcd->self);

	if ((retval = driver->start (hcd)) < 0) 
	{
		usb_hcd_sa1111_remove(hcd, dev);
		return retval;
	}

	*hcd_out = hcd;
	return 0;

 err2:
	hcd_buffer_destroy (hcd);
	if (hcd)
		driver->hcd_free(hcd);
 err1:
	sa1111_stop_hc(dev);
	release_mem_region(dev->res.start, dev->res.end - dev->res.start + 1);
	return retval;
}


/* may be called without controller electrically present */
/* may be called with controller, bus, and devices active */

/**
 * usb_hcd_sa1111_remove - shutdown processing for SA-1111-based HCDs
 * @dev: USB Host Controller being removed
 * Context: !in_interrupt()
 *
 * Reverses the effect of usb_hcd_sa1111_probe(), first invoking
 * the HCD's stop() method.  It is always called from a thread
 * context, normally "rmmod", "apmd", or something similar.
 *
 */
void usb_hcd_sa1111_remove (struct usb_hcd *hcd, struct sa1111_dev *dev)
{
	void *base;

	info ("remove: %s, state %x", hcd->self.bus_name, hcd->state);

	if (in_interrupt ())
		BUG ();

	hcd->state = USB_STATE_QUIESCING;

	dbg ("%s: roothub graceful disconnect", hcd->self.bus_name);
	usb_disconnect (&hcd->self.root_hub);

	hcd->driver->stop (hcd);
	hcd->state = USB_STATE_HALT;

	free_irq (hcd->irq, hcd);
	hcd_buffer_destroy (hcd);

	usb_deregister_bus (&hcd->self);

	base = hcd->regs;
	hcd->driver->hcd_free (hcd);

	sa1111_stop_hc(dev);
	release_mem_region(dev->res.start, dev->res.end - dev->res.start + 1);
}

/*-------------------------------------------------------------------------*/

static int __devinit
ohci_sa1111_start (struct usb_hcd *hcd)
{
	struct ohci_hcd	*ohci = hcd_to_ohci (hcd);
	int		ret;

	ohci->hcca = dma_alloc_coherent (hcd->self.controller,
			sizeof *ohci->hcca, &ohci->hcca_dma, 0);
	if (!ohci->hcca)
		return -ENOMEM;
        
	memset (ohci->hcca, 0, sizeof (struct ohci_hcca));
	if ((ret = ohci_mem_init (ohci)) < 0) {
		ohci_stop (hcd);
		return ret;
	}
	ohci->regs = hcd->regs;

	if (hc_reset (ohci) < 0) {
		ohci_stop (hcd);
		return -ENODEV;
	}

	if (hc_start (ohci) < 0) {
		err ("can't start %s", ohci->hcd.self.bus_name);
		ohci_stop (hcd);
		return -EBUSY;
	}
	create_debug_files (ohci);

#ifdef	DEBUG
	ohci_dump (ohci, 1);
#endif
	return 0;
}

/*-------------------------------------------------------------------------*/

static const struct hc_driver ohci_sa1111_hc_driver = {
	.description =		hcd_name,

	/*
	 * generic hardware linkage
	 */
	.irq =			ohci_irq,
	.flags =		HCD_USB11,

	/*
	 * basic lifecycle operations
	 */
	.start =		ohci_sa1111_start,
#ifdef	CONFIG_PM
	/* suspend:		ohci_sa1111_suspend,  -- tbd */
	/* resume:		ohci_sa1111_resume,   -- tbd */
#endif
	.stop =			ohci_stop,

	/*
	 * memory lifecycle (except per-request)
	 */
	.hcd_alloc =		ohci_hcd_alloc,
	.hcd_free =		ohci_hcd_free,

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
#ifdef	CONFIG_USB_SUSPEND
	.hub_suspend =		ohci_hub_suspend,
	.hub_resume =		ohci_hub_resume,
#endif
};

/*-------------------------------------------------------------------------*/

static int ohci_hcd_sa1111_drv_probe(struct sa1111_dev *dev)
{
	struct usb_hcd *hcd = NULL;
	int ret;

	if (usb_disabled())
		return -ENODEV;

	ret = usb_hcd_sa1111_probe(&ohci_sa1111_hc_driver, &hcd, dev);

	if (ret == 0)
		sa1111_set_drvdata(dev, hcd);

	return ret;
}

static int ohci_hcd_sa1111_drv_remove(struct sa1111_dev *dev)
{
	struct usb_hcd *hcd = sa1111_get_drvdata(dev);

	usb_hcd_sa1111_remove(hcd, dev);

	sa1111_set_drvdata(dev, NULL);

	return 0;
}

static struct sa1111_driver ohci_hcd_sa1111_driver = {
	.drv = {
		.name	= "sa1111-ohci",
	},
	.devid		= SA1111_DEVID_USB,
	.probe		= ohci_hcd_sa1111_drv_probe,
	.remove		= ohci_hcd_sa1111_drv_remove,
};

static int __init ohci_hcd_sa1111_init (void)
{
	dbg (DRIVER_INFO " (SA-1111)");
	dbg ("block sizes: ed %d td %d",
		sizeof (struct ed), sizeof (struct td));

	return sa1111_driver_register(&ohci_hcd_sa1111_driver);
}

static void __exit ohci_hcd_sa1111_cleanup (void)
{
	sa1111_driver_unregister(&ohci_hcd_sa1111_driver);
}

module_init (ohci_hcd_sa1111_init);
module_exit (ohci_hcd_sa1111_cleanup);
