/*
 *	$Id: pci.c,v 1.91 1999/01/21 13:34:01 davem Exp $
 *
 *	PCI Bus Services, see include/linux/pci.h for further explanation.
 *
 *	Copyright 1993 -- 1997 Drew Eckhardt, Frederic Potter,
 *	David Mosberger-Tang
 *
 *	Copyright 1997 -- 2000 Martin Mares <mj@ucw.cz>
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <asm/dma.h>	/* isa_dma_bridge_buggy */

#undef DEBUG

#ifdef DEBUG
#define DBG(x...) printk(x)
#else
#define DBG(x...)
#endif

/**
 * pci_bus_max_busnr - returns maximum PCI bus number of given bus' children
 * @bus: pointer to PCI bus structure to search
 *
 * Given a PCI bus, returns the highest PCI bus number present in the set
 * including the given PCI bus and its list of child PCI buses.
 */
unsigned char __devinit
pci_bus_max_busnr(struct pci_bus* bus)
{
	struct list_head *tmp;
	unsigned char max, n;

	max = bus->number;
	list_for_each(tmp, &bus->children) {
		n = pci_bus_max_busnr(pci_bus_b(tmp));
		if(n > max)
			max = n;
	}
	return max;
}

/**
 * pci_max_busnr - returns maximum PCI bus number
 *
 * Returns the highest PCI bus number present in the system global list of
 * PCI buses.
 */
unsigned char __devinit
pci_max_busnr(void)
{
	struct pci_bus *bus = NULL;
	unsigned char max, n;

	max = 0;
	while ((bus = pci_find_next_bus(bus)) != NULL) {
		n = pci_bus_max_busnr(bus);
		if(n > max)
			max = n;
	}
	return max;
}

static int __pci_bus_find_cap(struct pci_bus *bus, unsigned int devfn, u8 hdr_type, int cap)
{
	u16 status;
	u8 pos, id;
	int ttl = 48;

	pci_bus_read_config_word(bus, devfn, PCI_STATUS, &status);
	if (!(status & PCI_STATUS_CAP_LIST))
		return 0;

	switch (hdr_type) {
	case PCI_HEADER_TYPE_NORMAL:
	case PCI_HEADER_TYPE_BRIDGE:
		pci_bus_read_config_byte(bus, devfn, PCI_CAPABILITY_LIST, &pos);
		break;
	case PCI_HEADER_TYPE_CARDBUS:
		pci_bus_read_config_byte(bus, devfn, PCI_CB_CAPABILITY_LIST, &pos);
		break;
	default:
		return 0;
	}
	while (ttl-- && pos >= 0x40) {
		pos &= ~3;
		pci_bus_read_config_byte(bus, devfn, pos + PCI_CAP_LIST_ID, &id);
		if (id == 0xff)
			break;
		if (id == cap)
			return pos;
		pci_bus_read_config_byte(bus, devfn, pos + PCI_CAP_LIST_NEXT, &pos);
	}
	return 0;
}

/**
 * pci_find_capability - query for devices' capabilities 
 * @dev: PCI device to query
 * @cap: capability code
 *
 * Tell if a device supports a given PCI capability.
 * Returns the address of the requested capability structure within the
 * device's PCI configuration space or 0 in case the device does not
 * support it.  Possible values for @cap:
 *
 *  %PCI_CAP_ID_PM           Power Management 
 *  %PCI_CAP_ID_AGP          Accelerated Graphics Port 
 *  %PCI_CAP_ID_VPD          Vital Product Data 
 *  %PCI_CAP_ID_SLOTID       Slot Identification 
 *  %PCI_CAP_ID_MSI          Message Signalled Interrupts
 *  %PCI_CAP_ID_CHSWP        CompactPCI HotSwap 
 *  %PCI_CAP_ID_PCIX         PCI-X
 *  %PCI_CAP_ID_EXP          PCI Express
 */
int pci_find_capability(struct pci_dev *dev, int cap)
{
	return __pci_bus_find_cap(dev->bus, dev->devfn, dev->hdr_type, cap);
}

/**
 * pci_bus_find_capability - query for devices' capabilities 
 * @bus:   the PCI bus to query
 * @devfn: PCI device to query
 * @cap:   capability code
 *
 * Like pci_find_capability() but works for pci devices that do not have a
 * pci_dev structure set up yet. 
 *
 * Returns the address of the requested capability structure within the
 * device's PCI configuration space or 0 in case the device does not
 * support it.
 */
int pci_bus_find_capability(struct pci_bus *bus, unsigned int devfn, int cap)
{
	u8 hdr_type;

	pci_bus_read_config_byte(bus, devfn, PCI_HEADER_TYPE, &hdr_type);

	return __pci_bus_find_cap(bus, devfn, hdr_type & 0x7f, cap);
}

/**
 * pci_find_ext_capability - Find an extended capability
 * @dev: PCI device to query
 * @cap: capability code
 *
 * Returns the address of the requested extended capability structure
 * within the device's PCI configuration space or 0 if the device does
 * not support it.  Possible values for @cap:
 *
 *  %PCI_EXT_CAP_ID_ERR		Advanced Error Reporting
 *  %PCI_EXT_CAP_ID_VC		Virtual Channel
 *  %PCI_EXT_CAP_ID_DSN		Device Serial Number
 *  %PCI_EXT_CAP_ID_PWR		Power Budgeting
 */
int pci_find_ext_capability(struct pci_dev *dev, int cap)
{
	u32 header;
	int ttl = 480; /* 3840 bytes, minimum 8 bytes per capability */
	int pos = 0x100;

	if (dev->cfg_size <= 256)
		return 0;

	if (pci_read_config_dword(dev, pos, &header) != PCIBIOS_SUCCESSFUL)
		return 0;

	/*
	 * If we have no capabilities, this is indicated by cap ID,
	 * cap version and next pointer all being 0.
	 */
	if (header == 0)
		return 0;

	while (ttl-- > 0) {
		if (PCI_EXT_CAP_ID(header) == cap)
			return pos;

		pos = PCI_EXT_CAP_NEXT(header);
		if (pos < 0x100)
			break;

		if (pci_read_config_dword(dev, pos, &header) != PCIBIOS_SUCCESSFUL)
			break;
	}

	return 0;
}

/**
 * pci_find_parent_resource - return resource region of parent bus of given region
 * @dev: PCI device structure contains resources to be searched
 * @res: child resource record for which parent is sought
 *
 *  For given resource region of given device, return the resource
 *  region of parent bus the given region is contained in or where
 *  it should be allocated from.
 */
struct resource *
pci_find_parent_resource(const struct pci_dev *dev, struct resource *res)
{
	const struct pci_bus *bus = dev->bus;
	int i;
	struct resource *best = NULL;

	for(i = 0; i < PCI_BUS_NUM_RESOURCES; i++) {
		struct resource *r = bus->resource[i];
		if (!r)
			continue;
		if (res->start && !(res->start >= r->start && res->end <= r->end))
			continue;	/* Not contained */
		if ((res->flags ^ r->flags) & (IORESOURCE_IO | IORESOURCE_MEM))
			continue;	/* Wrong type */
		if (!((res->flags ^ r->flags) & IORESOURCE_PREFETCH))
			return r;	/* Exact match */
		if ((res->flags & IORESOURCE_PREFETCH) && !(r->flags & IORESOURCE_PREFETCH))
			best = r;	/* Approximating prefetchable by non-prefetchable */
	}
	return best;
}

/**
 * pci_set_power_state - Set the power state of a PCI device
 * @dev: PCI device to be suspended
 * @state: Power state we're entering
 *
 * Transition a device to a new power state, using the Power Management 
 * Capabilities in the device's config space.
 *
 * RETURN VALUE: 
 * -EINVAL if trying to enter a lower state than we're already in.
 * 0 if we're already in the requested state.
 * -EIO if device does not support PCI PM.
 * 0 if we can successfully change the power state.
 */

int
pci_set_power_state(struct pci_dev *dev, int state)
{
	int pm;
	u16 pmcsr;

	/* bound the state we're entering */
	if (state > 3) state = 3;

	/* Validate current state:
	 * Can enter D0 from any state, but if we can only go deeper 
	 * to sleep if we're already in a low power state
	 */
	if (state > 0 && dev->current_state > state)
		return -EINVAL;
	else if (dev->current_state == state) 
		return 0;        /* we're already there */

	/* find PCI PM capability in list */
	pm = pci_find_capability(dev, PCI_CAP_ID_PM);
	
	/* abort if the device doesn't support PM capabilities */
	if (!pm) return -EIO; 

	/* check if this device supports the desired state */
	if (state == 1 || state == 2) {
		u16 pmc;
		pci_read_config_word(dev,pm + PCI_PM_PMC,&pmc);
		if (state == 1 && !(pmc & PCI_PM_CAP_D1)) return -EIO;
		else if (state == 2 && !(pmc & PCI_PM_CAP_D2)) return -EIO;
	}

	/* If we're in D3, force entire word to 0.
	 * This doesn't affect PME_Status, disables PME_En, and
	 * sets PowerState to 0.
	 */
	if (dev->current_state >= 3)
		pmcsr = 0;
	else {
		pci_read_config_word(dev, pm + PCI_PM_CTRL, &pmcsr);
		pmcsr &= ~PCI_PM_CTRL_STATE_MASK;
		pmcsr |= state;
	}

	/* enter specified state */
	pci_write_config_word(dev, pm + PCI_PM_CTRL, pmcsr);

	/* Mandatory power management transition delays */
	/* see PCI PM 1.1 5.6.1 table 18 */
	if(state == 3 || dev->current_state == 3)
	{
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ/100);
	}
	else if(state == 2 || dev->current_state == 2)
		udelay(200);
	dev->current_state = state;

	return 0;
}

/**
 * pci_save_state - save the PCI configuration space of a device before suspending
 * @dev: - PCI device that we're dealing with
 * @buffer: - buffer to hold config space context
 *
 * @buffer must be large enough to hold the entire PCI 2.2 config space 
 * (>= 64 bytes).
 */
int
pci_save_state(struct pci_dev *dev, u32 *buffer)
{
	int i;
	if (buffer) {
		/* XXX: 100% dword access ok here? */
		for (i = 0; i < 16; i++)
			pci_read_config_dword(dev, i * 4,&buffer[i]);
	}
	return 0;
}

/** 
 * pci_restore_state - Restore the saved state of a PCI device
 * @dev: - PCI device that we're dealing with
 * @buffer: - saved PCI config space
 *
 */
int 
pci_restore_state(struct pci_dev *dev, u32 *buffer)
{
	int i;

	if (buffer) {
		for (i = 0; i < 16; i++)
			pci_write_config_dword(dev,i * 4, buffer[i]);
	}
	/*
	 * otherwise, write the context information we know from bootup.
	 * This works around a problem where warm-booting from Windows
	 * combined with a D3(hot)->D0 transition causes PCI config
	 * header data to be forgotten.
	 */	
	else {
		for (i = 0; i < 6; i ++)
			pci_write_config_dword(dev,
					       PCI_BASE_ADDRESS_0 + (i * 4),
					       dev->resource[i].start);
		pci_write_config_byte(dev, PCI_INTERRUPT_LINE, dev->irq);
	}
	return 0;
}

/**
 * pci_enable_device_bars - Initialize some of a device for use
 * @dev: PCI device to be initialized
 * @bars: bitmask of BAR's that must be configured
 *
 *  Initialize device before it's used by a driver. Ask low-level code
 *  to enable selected I/O and memory resources. Wake up the device if it 
 *  was suspended. Beware, this function can fail.
 */
 
int
pci_enable_device_bars(struct pci_dev *dev, int bars)
{
	int err;

	pci_set_power_state(dev, 0);
	if ((err = pcibios_enable_device(dev, bars)) < 0)
		return err;
	return 0;
}

/**
 * pci_enable_device - Initialize device before it's used by a driver.
 * @dev: PCI device to be initialized
 *
 *  Initialize device before it's used by a driver. Ask low-level code
 *  to enable I/O and memory. Wake up the device if it was suspended.
 *  Beware, this function can fail.
 */
int
pci_enable_device(struct pci_dev *dev)
{
	dev->is_enabled = 1;
	return pci_enable_device_bars(dev, (1 << PCI_NUM_RESOURCES) - 1);
}

/**
 * pci_disable_device - Disable PCI device after use
 * @dev: PCI device to be disabled
 *
 * Signal to the system that the PCI device is not in use by the system
 * anymore.  This only involves disabling PCI bus-mastering, if active.
 */
void
pci_disable_device(struct pci_dev *dev)
{
	u16 pci_command;
	
	dev->is_enabled = 0;
	dev->is_busmaster = 0;

	pci_read_config_word(dev, PCI_COMMAND, &pci_command);
	if (pci_command & PCI_COMMAND_MASTER) {
		pci_command &= ~PCI_COMMAND_MASTER;
		pci_write_config_word(dev, PCI_COMMAND, pci_command);
	}
}

/**
 * pci_enable_wake - enable device to generate PME# when suspended
 * @dev: - PCI device to operate on
 * @state: - Current state of device.
 * @enable: - Flag to enable or disable generation
 * 
 * Set the bits in the device's PM Capabilities to generate PME# when
 * the system is suspended. 
 *
 * -EIO is returned if device doesn't have PM Capabilities. 
 * -EINVAL is returned if device supports it, but can't generate wake events.
 * 0 if operation is successful.
 * 
 */
int pci_enable_wake(struct pci_dev *dev, u32 state, int enable)
{
	int pm;
	u16 value;

	/* find PCI PM capability in list */
	pm = pci_find_capability(dev, PCI_CAP_ID_PM);

	/* If device doesn't support PM Capabilities, but request is to disable
	 * wake events, it's a nop; otherwise fail */
	if (!pm) 
		return enable ? -EIO : 0; 

	/* Check device's ability to generate PME# */
	pci_read_config_word(dev,pm+PCI_PM_PMC,&value);

	value &= PCI_PM_CAP_PME_MASK;
	value >>= ffs(PCI_PM_CAP_PME_MASK) - 1;   /* First bit of mask */

	/* Check if it can generate PME# from requested state. */
	if (!value || !(value & (1 << state))) 
		return enable ? -EINVAL : 0;

	pci_read_config_word(dev, pm + PCI_PM_CTRL, &value);

	/* Clear PME_Status by writing 1 to it and enable PME# */
	value |= PCI_PM_CTRL_PME_STATUS | PCI_PM_CTRL_PME_ENABLE;

	if (!enable)
		value &= ~PCI_PM_CTRL_PME_ENABLE;

	pci_write_config_word(dev, pm + PCI_PM_CTRL, value);
	
	return 0;
}

int
pci_get_interrupt_pin(struct pci_dev *dev, struct pci_dev **bridge)
{
	u8 pin;

	pci_read_config_byte(dev, PCI_INTERRUPT_PIN, &pin);
	if (!pin)
		return -1;
	pin--;
	while (dev->bus->self) {
		pin = (pin + PCI_SLOT(dev->devfn)) % 4;
		dev = dev->bus->self;
	}
	*bridge = dev;
	return pin;
}

/**
 *	pci_release_region - Release a PCI bar
 *	@pdev: PCI device whose resources were previously reserved by pci_request_region
 *	@bar: BAR to release
 *
 *	Releases the PCI I/O and memory resources previously reserved by a
 *	successful call to pci_request_region.  Call this function only
 *	after all use of the PCI regions has ceased.
 */
void pci_release_region(struct pci_dev *pdev, int bar)
{
	if (pci_resource_len(pdev, bar) == 0)
		return;
	if (pci_resource_flags(pdev, bar) & IORESOURCE_IO)
		release_region(pci_resource_start(pdev, bar),
				pci_resource_len(pdev, bar));
	else if (pci_resource_flags(pdev, bar) & IORESOURCE_MEM)
		release_mem_region(pci_resource_start(pdev, bar),
				pci_resource_len(pdev, bar));
}

/**
 *	pci_request_region - Reserved PCI I/O and memory resource
 *	@pdev: PCI device whose resources are to be reserved
 *	@bar: BAR to be reserved
 *	@res_name: Name to be associated with resource.
 *
 *	Mark the PCI region associated with PCI device @pdev BR @bar as
 *	being reserved by owner @res_name.  Do not access any
 *	address inside the PCI regions unless this call returns
 *	successfully.
 *
 *	Returns 0 on success, or %EBUSY on error.  A warning
 *	message is also printed on failure.
 */
int pci_request_region(struct pci_dev *pdev, int bar, char *res_name)
{
	if (pci_resource_len(pdev, bar) == 0)
		return 0;
		
	if (pci_resource_flags(pdev, bar) & IORESOURCE_IO) {
		if (!request_region(pci_resource_start(pdev, bar),
			    pci_resource_len(pdev, bar), res_name))
			goto err_out;
	}
	else if (pci_resource_flags(pdev, bar) & IORESOURCE_MEM) {
		if (!request_mem_region(pci_resource_start(pdev, bar),
				        pci_resource_len(pdev, bar), res_name))
			goto err_out;
	}
	
	return 0;

err_out:
	printk (KERN_WARNING "PCI: Unable to reserve %s region #%d:%lx@%lx for device %s\n",
		pci_resource_flags(pdev, bar) & IORESOURCE_IO ? "I/O" : "mem",
		bar + 1, /* PCI BAR # */
		pci_resource_len(pdev, bar), pci_resource_start(pdev, bar),
		pci_name(pdev));
	return -EBUSY;
}


/**
 *	pci_release_regions - Release reserved PCI I/O and memory resources
 *	@pdev: PCI device whose resources were previously reserved by pci_request_regions
 *
 *	Releases all PCI I/O and memory resources previously reserved by a
 *	successful call to pci_request_regions.  Call this function only
 *	after all use of the PCI regions has ceased.
 */

void pci_release_regions(struct pci_dev *pdev)
{
	int i;
	
	for (i = 0; i < 6; i++)
		pci_release_region(pdev, i);
}

/**
 *	pci_request_regions - Reserved PCI I/O and memory resources
 *	@pdev: PCI device whose resources are to be reserved
 *	@res_name: Name to be associated with resource.
 *
 *	Mark all PCI regions associated with PCI device @pdev as
 *	being reserved by owner @res_name.  Do not access any
 *	address inside the PCI regions unless this call returns
 *	successfully.
 *
 *	Returns 0 on success, or %EBUSY on error.  A warning
 *	message is also printed on failure.
 */
int pci_request_regions(struct pci_dev *pdev, char *res_name)
{
	int i;
	
	for (i = 0; i < 6; i++)
		if(pci_request_region(pdev, i, res_name))
			goto err_out;
	return 0;

err_out:
	while(--i >= 0)
		pci_release_region(pdev, i);
		
	return -EBUSY;
}

/**
 * pci_set_master - enables bus-mastering for device dev
 * @dev: the PCI device to enable
 *
 * Enables bus-mastering on the device and calls pcibios_set_master()
 * to do the needed arch specific settings.
 */
void
pci_set_master(struct pci_dev *dev)
{
	u16 cmd;

	pci_read_config_word(dev, PCI_COMMAND, &cmd);
	if (! (cmd & PCI_COMMAND_MASTER)) {
		DBG("PCI: Enabling bus mastering for device %s\n", pci_name(dev));
		cmd |= PCI_COMMAND_MASTER;
		pci_write_config_word(dev, PCI_COMMAND, cmd);
	}
	dev->is_busmaster = 1;
	pcibios_set_master(dev);
}

#ifndef HAVE_ARCH_PCI_MWI
/* This can be overridden by arch code. */
u8 pci_cache_line_size = L1_CACHE_BYTES >> 2;

/**
 * pci_generic_prep_mwi - helper function for pci_set_mwi
 * @dev: the PCI device for which MWI is enabled
 *
 * Helper function for generic implementation of pcibios_prep_mwi
 * function.  Originally copied from drivers/net/acenic.c.
 * Copyright 1998-2001 by Jes Sorensen, <jes@trained-monkey.org>.
 *
 * RETURNS: An appropriate -ERRNO error value on error, or zero for success.
 */
static int
pci_generic_prep_mwi(struct pci_dev *dev)
{
	u8 cacheline_size;

	if (!pci_cache_line_size)
		return -EINVAL;		/* The system doesn't support MWI. */

	/* Validate current setting: the PCI_CACHE_LINE_SIZE must be
	   equal to or multiple of the right value. */
	pci_read_config_byte(dev, PCI_CACHE_LINE_SIZE, &cacheline_size);
	if (cacheline_size >= pci_cache_line_size &&
	    (cacheline_size % pci_cache_line_size) == 0)
		return 0;

	/* Write the correct value. */
	pci_write_config_byte(dev, PCI_CACHE_LINE_SIZE, pci_cache_line_size);
	/* Read it back. */
	pci_read_config_byte(dev, PCI_CACHE_LINE_SIZE, &cacheline_size);
	if (cacheline_size == pci_cache_line_size)
		return 0;

	printk(KERN_DEBUG "PCI: cache line size of %d is not supported "
	       "by device %s\n", pci_cache_line_size << 2, pci_name(dev));

	return -EINVAL;
}
#endif /* !HAVE_ARCH_PCI_MWI */

/**
 * pci_set_mwi - enables memory-write-invalidate PCI transaction
 * @dev: the PCI device for which MWI is enabled
 *
 * Enables the Memory-Write-Invalidate transaction in %PCI_COMMAND,
 * and then calls @pcibios_set_mwi to do the needed arch specific
 * operations or a generic mwi-prep function.
 *
 * RETURNS: An appropriate -ERRNO error value on error, or zero for success.
 */
int
pci_set_mwi(struct pci_dev *dev)
{
	int rc;
	u16 cmd;

#ifdef HAVE_ARCH_PCI_MWI
	rc = pcibios_prep_mwi(dev);
#else
	rc = pci_generic_prep_mwi(dev);
#endif

	if (rc)
		return rc;

	pci_read_config_word(dev, PCI_COMMAND, &cmd);
	if (! (cmd & PCI_COMMAND_INVALIDATE)) {
		DBG("PCI: Enabling Mem-Wr-Inval for device %s\n", pci_name(dev));
		cmd |= PCI_COMMAND_INVALIDATE;
		pci_write_config_word(dev, PCI_COMMAND, cmd);
	}
	
	return 0;
}

/**
 * pci_clear_mwi - disables Memory-Write-Invalidate for device dev
 * @dev: the PCI device to disable
 *
 * Disables PCI Memory-Write-Invalidate transaction on the device
 */
void
pci_clear_mwi(struct pci_dev *dev)
{
	u16 cmd;

	pci_read_config_word(dev, PCI_COMMAND, &cmd);
	if (cmd & PCI_COMMAND_INVALIDATE) {
		cmd &= ~PCI_COMMAND_INVALIDATE;
		pci_write_config_word(dev, PCI_COMMAND, cmd);
	}
}

#ifndef HAVE_ARCH_PCI_SET_DMA_MASK
/*
 * These can be overridden by arch-specific implementations
 */
int
pci_set_dma_mask(struct pci_dev *dev, u64 mask)
{
	if (!pci_dma_supported(dev, mask))
		return -EIO;

	dev->dma_mask = mask;

	return 0;
}
    
int
pci_dac_set_dma_mask(struct pci_dev *dev, u64 mask)
{
	if (!pci_dac_dma_supported(dev, mask))
		return -EIO;

	dev->dma_mask = mask;

	return 0;
}

int
pci_set_consistent_dma_mask(struct pci_dev *dev, u64 mask)
{
	if (!pci_dma_supported(dev, mask))
		return -EIO;

	dev->dev.coherent_dma_mask = mask;

	return 0;
}
#endif
     
static int __devinit pci_init(void)
{
	struct pci_dev *dev = NULL;

	while ((dev = pci_find_device(PCI_ANY_ID, PCI_ANY_ID, dev)) != NULL) {
		pci_fixup_device(PCI_FIXUP_FINAL, dev);
	}
	return 0;
}

static int __devinit pci_setup(char *str)
{
	while (str) {
		char *k = strchr(str, ',');
		if (k)
			*k++ = 0;
		if (*str && (str = pcibios_setup(str)) && *str) {
			/* PCI layer options should be handled here */
			printk(KERN_ERR "PCI: Unknown option `%s'\n", str);
		}
		str = k;
	}
	return 1;
}

device_initcall(pci_init);

__setup("pci=", pci_setup);

#if defined(CONFIG_ISA) || defined(CONFIG_EISA)
/* FIXME: Some boxes have multiple ISA bridges! */
struct pci_dev *isa_bridge;
EXPORT_SYMBOL(isa_bridge);
#endif

EXPORT_SYMBOL(pci_enable_device_bars);
EXPORT_SYMBOL(pci_enable_device);
EXPORT_SYMBOL(pci_disable_device);
EXPORT_SYMBOL(pci_max_busnr);
EXPORT_SYMBOL(pci_bus_max_busnr);
EXPORT_SYMBOL(pci_find_capability);
EXPORT_SYMBOL(pci_bus_find_capability);
EXPORT_SYMBOL(pci_release_regions);
EXPORT_SYMBOL(pci_request_regions);
EXPORT_SYMBOL(pci_release_region);
EXPORT_SYMBOL(pci_request_region);
EXPORT_SYMBOL(pci_set_master);
EXPORT_SYMBOL(pci_set_mwi);
EXPORT_SYMBOL(pci_clear_mwi);
EXPORT_SYMBOL(pci_set_dma_mask);
EXPORT_SYMBOL(pci_dac_set_dma_mask);
EXPORT_SYMBOL(pci_set_consistent_dma_mask);
EXPORT_SYMBOL(pci_assign_resource);
EXPORT_SYMBOL(pci_find_parent_resource);

EXPORT_SYMBOL(pci_set_power_state);
EXPORT_SYMBOL(pci_save_state);
EXPORT_SYMBOL(pci_restore_state);
EXPORT_SYMBOL(pci_enable_wake);

/* Quirk info */

EXPORT_SYMBOL(isa_dma_bridge_buggy);
EXPORT_SYMBOL(pci_pci_problems);
