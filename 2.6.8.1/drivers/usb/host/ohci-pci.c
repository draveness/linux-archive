/*
 * OHCI HCD (Host Controller Driver) for USB.
 *
 * (C) Copyright 1999 Roman Weissgaerber <weissg@vienna.at>
 * (C) Copyright 2000-2002 David Brownell <dbrownell@users.sourceforge.net>
 * 
 * [ Initialisation is based on Linus'  ]
 * [ uhci code and gregs ohci fragments ]
 * [ (C) Copyright 1999 Linus Torvalds  ]
 * [ (C) Copyright 1999 Gregory P. Smith]
 * 
 * PCI Bus Glue
 *
 * This file is licenced under the GPL.
 */
 
#ifdef CONFIG_PMAC_PBOOK
#include <asm/machdep.h>
#include <asm/pmac_feature.h>
#include <asm/pci-bridge.h>
#include <asm/prom.h>
#ifndef CONFIG_PM
#	define CONFIG_PM
#endif
#endif

#ifndef CONFIG_PCI
#error "This file is PCI bus glue.  CONFIG_PCI must be defined."
#endif

/*-------------------------------------------------------------------------*/

static int
ohci_pci_reset (struct usb_hcd *hcd)
{
	struct ohci_hcd	*ohci = hcd_to_ohci (hcd);

	ohci->regs = hcd->regs;
	ohci->next_statechange = jiffies;
	return hc_reset (ohci);
}

static int __devinit
ohci_pci_start (struct usb_hcd *hcd)
{
	struct ohci_hcd	*ohci = hcd_to_ohci (hcd);
	int		ret;

	ohci->hcca = dma_alloc_coherent (hcd->self.controller,
			sizeof *ohci->hcca, &ohci->hcca_dma, 0);
	if (!ohci->hcca)
		return -ENOMEM;

	if(hcd->self.controller && hcd->self.controller->bus == &pci_bus_type) {
		struct pci_dev *pdev = to_pci_dev(hcd->self.controller);

		/* AMD 756, for most chips (early revs), corrupts register
		 * values on read ... so enable the vendor workaround.
		 */
		if (pdev->vendor == PCI_VENDOR_ID_AMD
				&& pdev->device == 0x740c) {
			ohci->flags = OHCI_QUIRK_AMD756;
			ohci_info (ohci, "AMD756 erratum 4 workaround\n");
		}

		/* FIXME for some of the early AMD 760 southbridges, OHCI
		 * won't work at all.  blacklist them.
		 */

		/* Apple's OHCI driver has a lot of bizarre workarounds
		 * for this chip.  Evidently control and bulk lists
		 * can get confused.  (B&W G3 models, and ...)
		 */
		else if (pdev->vendor == PCI_VENDOR_ID_OPTI
				&& pdev->device == 0xc861) {
			ohci_info (ohci,
				"WARNING: OPTi workarounds unavailable\n");
		}

		/* Check for NSC87560. We have to look at the bridge (fn1) to
		 * identify the USB (fn2). This quirk might apply to more or
		 * even all NSC stuff.
		 */
		else if (pdev->vendor == PCI_VENDOR_ID_NS) {
			struct pci_dev	*b;

			b  = pci_find_slot (pdev->bus->number,
					PCI_DEVFN (PCI_SLOT (pdev->devfn), 1));
			if (b && b->device == PCI_DEVICE_ID_NS_87560_LIO
					&& b->vendor == PCI_VENDOR_ID_NS) {
				ohci->flags |= OHCI_QUIRK_SUPERIO;
				ohci_info (ohci, "Using NSC SuperIO setup\n");
			}
		}
	
	}

        memset (ohci->hcca, 0, sizeof (struct ohci_hcca));
	if ((ret = ohci_mem_init (ohci)) < 0) {
		ohci_stop (hcd);
		return ret;
	}

	if (hc_start (ohci) < 0) {
		ohci_err (ohci, "can't start\n");
		ohci_stop (hcd);
		return -EBUSY;
	}
	create_debug_files (ohci);

#ifdef	DEBUG
	ohci_dump (ohci, 1);
#endif
	return 0;
}

#ifdef	CONFIG_PM

static int ohci_pci_suspend (struct usb_hcd *hcd, u32 state)
{
	struct ohci_hcd		*ohci = hcd_to_ohci (hcd);

	/* suspend root hub, hoping it keeps power during suspend */
	while (time_before (jiffies, ohci->next_statechange))
		msleep (100);

#ifdef	CONFIG_USB_SUSPEND
	(void) usb_suspend_device (hcd->self.root_hub, state);
#else
	down (&hcd->self.root_hub->serialize);
	(void) ohci_hub_suspend (hcd);
	up (&hcd->self.root_hub->serialize);
#endif

	/* let things settle down a bit */
	msleep (100);
	
#ifdef CONFIG_PMAC_PBOOK
	if (_machine == _MACH_Pmac)
		disable_irq ((to_pci_dev(hcd->self.controller))->irq);

	{
	   	struct device_node	*of_node;
 
		/* Disable USB PAD & cell clock */
		of_node = pci_device_to_OF_node (to_pci_dev(hcd->self.controller));
		if (of_node)
			pmac_call_feature(PMAC_FTR_USB_ENABLE, of_node, 0, 0);
	}
#endif
	return 0;
}


static int ohci_pci_resume (struct usb_hcd *hcd)
{
	struct ohci_hcd		*ohci = hcd_to_ohci (hcd);
	int			retval = 0;

#ifdef CONFIG_PMAC_PBOOK
	{
		struct device_node *of_node;

		/* Re-enable USB PAD & cell clock */
		of_node = pci_device_to_OF_node (to_pci_dev(hcd->self.controller));
		if (of_node)
			pmac_call_feature (PMAC_FTR_USB_ENABLE, of_node, 0, 1);
	}
#endif

	/* resume root hub */
	while (time_before (jiffies, ohci->next_statechange))
		msleep (100);
#ifdef	CONFIG_USB_SUSPEND
	/* get extra cleanup even if remote wakeup isn't in use */
	retval = usb_resume_device (hcd->self.root_hub);
#else
	down (&hcd->self.root_hub->serialize);
	retval = ohci_hub_resume (hcd);
	up (&hcd->self.root_hub->serialize);
#endif

	if (retval == 0) {
		hcd->self.controller->power.power_state = 0;
#ifdef CONFIG_PMAC_PBOOK
		if (_machine == _MACH_Pmac)
			enable_irq (to_pci_dev(hcd->self.controller)->irq);
#endif
	}
	return retval;
}

#endif	/* CONFIG_PM */


/*-------------------------------------------------------------------------*/

static const struct hc_driver ohci_pci_hc_driver = {
	.description =		hcd_name,

	/*
	 * generic hardware linkage
	 */
	.irq =			ohci_irq,
	.flags =		HCD_MEMORY | HCD_USB11,

	/*
	 * basic lifecycle operations
	 */
	.reset =		ohci_pci_reset,
	.start =		ohci_pci_start,
#ifdef	CONFIG_PM
	.suspend =		ohci_pci_suspend,
	.resume =		ohci_pci_resume,
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


static const struct pci_device_id pci_ids [] = { {
	/* handle any USB OHCI controller */
	PCI_DEVICE_CLASS((PCI_CLASS_SERIAL_USB << 8) | 0x10, ~0),
	.driver_data =	(unsigned long) &ohci_pci_hc_driver,
	}, { /* end: all zeroes */ }
};
MODULE_DEVICE_TABLE (pci, pci_ids);

/* pci driver glue; this is a "new style" PCI driver module */
static struct pci_driver ohci_pci_driver = {
	.name =		(char *) hcd_name,
	.id_table =	pci_ids,

	.probe =	usb_hcd_pci_probe,
	.remove =	usb_hcd_pci_remove,

#ifdef	CONFIG_PM
	.suspend =	usb_hcd_pci_suspend,
	.resume =	usb_hcd_pci_resume,
#endif
};

 
static int __init ohci_hcd_pci_init (void) 
{
	printk (KERN_DEBUG "%s: " DRIVER_INFO " (PCI)\n", hcd_name);
	if (usb_disabled())
		return -ENODEV;

	printk (KERN_DEBUG "%s: block sizes: ed %Zd td %Zd\n", hcd_name,
		sizeof (struct ed), sizeof (struct td));
	return pci_module_init (&ohci_pci_driver);
}
module_init (ohci_hcd_pci_init);

/*-------------------------------------------------------------------------*/

static void __exit ohci_hcd_pci_cleanup (void) 
{	
	pci_unregister_driver (&ohci_pci_driver);
}
module_exit (ohci_hcd_pci_cleanup);
