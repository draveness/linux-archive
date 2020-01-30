/*
**	DINO manager
**
**	(c) Copyright 1999 Red Hat Software
**	(c) Copyright 1999 SuSE GmbH
**	(c) Copyright 1999,2000 Hewlett-Packard Company
**	(c) Copyright 2000 Grant Grundler
**
**	This program is free software; you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**      the Free Software Foundation; either version 2 of the License, or
**      (at your option) any later version.
**
**	This module provides access to Dino PCI bus (config/IOport spaces)
**	and helps manage Dino IRQ lines.
**
**	Dino interrupt handling is a bit complicated.
**	Dino always writes to the broadcast EIR via irr0 for now.
**	(BIG WARNING: using broadcast EIR is a really bad thing for SMP!)
**	Only one processor interrupt is used for the 11 IRQ line 
**	inputs to dino.
**
**	The different between Built-in Dino and Card-Mode
**	dino is in chip initialization and pci device initialization.
**
**	Linux drivers can only use Card-Mode Dino if pci devices I/O port
**	BARs are configured and used by the driver. Programming MMIO address 
**	requires substantial knowledge of available Host I/O address ranges
**	is currently not supported.  Port/Config accessor functions are the
**	same. "BIOS" differences are handled within the existing routines.
*/

/*	Changes :
**	2001-06-14 : Clement Moyroud (moyroudc@esiee.fr)
**		- added support for the integrated RS232. 	
*/

/*
** TODO: create a virtual address for each Dino HPA.
**       GSC code might be able to do this since IODC data tells us
**       how many pages are used. PCI subsystem could (must?) do this
**       for PCI drivers devices which implement/use MMIO registers.
*/

#include <linux/config.h>
#include <linux/delay.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/interrupt.h>	/* for struct irqaction */
#include <linux/spinlock.h>	/* for spinlock_t and prototypes */

#include <asm/pdc.h>
#include <asm/page.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/hardware.h>

#include "gsc.h"

#undef DINO_DEBUG

#ifdef DINO_DEBUG
#define DBG(x...) printk(x)
#else
#define DBG(x...)
#endif

/*
** Config accessor functions only pass in the 8-bit bus number
** and not the 8-bit "PCI Segment" number. Each Dino will be
** assigned a PCI bus number based on "when" it's discovered.
**
** The "secondary" bus number is set to this before calling
** pci_scan_bus(). If any PPB's are present, the scan will
** discover them and update the "secondary" and "subordinate"
** fields in Dino's pci_bus structure.
**
** Changes in the configuration *will* result in a different
** bus number for each dino.
*/

#define is_card_dino(id) ((id)->hw_type == HPHW_A_DMA)

#define DINO_IAR0		0x004
#define DINO_IODC_ADDR		0x008
#define DINO_IODC_DATA_0	0x008
#define DINO_IODC_DATA_1	0x008
#define DINO_IRR0		0x00C
#define DINO_IAR1		0x010
#define DINO_IRR1		0x014
#define DINO_IMR		0x018
#define DINO_IPR		0x01C
#define DINO_TOC_ADDR		0x020
#define DINO_ICR		0x024
#define DINO_ILR		0x028
#define DINO_IO_COMMAND		0x030
#define DINO_IO_STATUS		0x034
#define DINO_IO_CONTROL		0x038
#define DINO_IO_GSC_ERR_RESP	0x040
#define DINO_IO_ERR_INFO	0x044
#define DINO_IO_PCI_ERR_RESP	0x048
#define DINO_IO_FBB_EN		0x05c
#define DINO_IO_ADDR_EN		0x060
#define DINO_PCI_ADDR		0x064
#define DINO_CONFIG_DATA	0x068
#define DINO_IO_DATA		0x06c
#define DINO_MEM_DATA		0x070	/* Dino 3.x only */
#define DINO_GSC2X_CONFIG	0x7b4
#define DINO_GMASK		0x800
#define DINO_PAMR		0x804
#define DINO_PAPR		0x808
#define DINO_DAMODE		0x80c
#define DINO_PCICMD		0x810
#define DINO_PCISTS		0x814
#define DINO_MLTIM		0x81c
#define DINO_BRDG_FEAT		0x820
#define DINO_PCIROR		0x824
#define DINO_PCIWOR		0x828
#define DINO_TLTIM		0x830

#define DINO_IRQS 11		/* bits 0-10 are architected */
#define DINO_IRR_MASK	0x5ff	/* only 10 bits are implemented */

#define DINO_MASK_IRQ(x)	(1<<(x))

#define PCIINTA   0x001
#define PCIINTB   0x002
#define PCIINTC   0x004
#define PCIINTD   0x008
#define PCIINTE   0x010
#define PCIINTF   0x020
#define GSCEXTINT 0x040
/* #define xxx       0x080 - bit 7 is "default" */
/* #define xxx    0x100 - bit 8 not used */
/* #define xxx    0x200 - bit 9 not used */
#define RS232INT  0x400

struct dino_device
{
	struct pci_hba_data	hba;	/* 'C' inheritance - must be first */
	spinlock_t		dinosaur_pen;
	unsigned long		txn_addr; /* EIR addr to generate interrupt */ 
	u32			txn_data; /* EIR data assign to each dino */ 
	int			irq;      /* Virtual IRQ dino uses */
	struct irq_region	*dino_region;  /* region for this Dino */

	u32 			imr; /* IRQ's which are enabled */ 
#ifdef DINO_DEBUG
	unsigned int		dino_irr0; /* save most recent IRQ line stat */ 
#endif
};

/* Looks nice and keeps the compiler happy */
#define DINO_DEV(d) ((struct dino_device *) d)


/*
 * Dino Configuration Space Accessor Functions
 */

#define DINO_CFG_TOK(bus,dfn,pos) ((u32) ((bus)<<16 | (dfn)<<8 | (pos)))

/*
 * keep the current highest bus count to assist in allocating busses.  This
 * tries to keep a global bus count total so that when we discover an 
 * entirely new bus, it can be given a unique bus number.
 */
static int dino_current_bus = 0;

static int dino_cfg_read(struct pci_bus *bus, unsigned int devfn, int where,
		int size, u32 *val)
{
	struct dino_device *d = DINO_DEV(parisc_walk_tree(bus->bridge));
	u32 local_bus = (bus->parent == NULL) ? 0 : bus->secondary;
	u32 v = DINO_CFG_TOK(local_bus, devfn, where & ~3);
	unsigned long base_addr = d->hba.base_addr;
	unsigned long flags;

	spin_lock_irqsave(&d->dinosaur_pen, flags);

	/* tell HW which CFG address */
	gsc_writel(v, base_addr + DINO_PCI_ADDR);

	/* generate cfg read cycle */
	if (size == 1) {
		*val = gsc_readb(base_addr + DINO_CONFIG_DATA + (where & 3));
	} else if (size == 2) {
		*val = le16_to_cpu(gsc_readw(base_addr +
					DINO_CONFIG_DATA + (where & 2)));
	} else if (size == 4) {
		*val = le32_to_cpu(gsc_readl(base_addr + DINO_CONFIG_DATA));
	}

	spin_unlock_irqrestore(&d->dinosaur_pen, flags);
	return 0;
}

/*
 * Dino address stepping "feature":
 * When address stepping, Dino attempts to drive the bus one cycle too soon
 * even though the type of cycle (config vs. MMIO) might be different. 
 * The read of Ven/Prod ID is harmless and avoids Dino's address stepping.
 */
static int dino_cfg_write(struct pci_bus *bus, unsigned int devfn, int where,
	int size, u32 val)
{
	struct dino_device *d = DINO_DEV(parisc_walk_tree(bus->bridge));
	u32 local_bus = (bus->parent == NULL) ? 0 : bus->secondary;
	u32 v = DINO_CFG_TOK(local_bus, devfn, where & ~3);
	unsigned long base_addr = d->hba.base_addr;
	unsigned long flags;

	spin_lock_irqsave(&d->dinosaur_pen, flags);

	/* avoid address stepping feature */
	gsc_writel(v & 0xffffff00, base_addr + DINO_PCI_ADDR);
	gsc_readl(base_addr + DINO_CONFIG_DATA);

	/* tell HW which CFG address */
	gsc_writel(v, base_addr + DINO_PCI_ADDR);
	/* generate cfg read cycle */
	if (size == 1) {
		gsc_writeb(val, base_addr + DINO_CONFIG_DATA + (where & 3));
	} else if (size == 2) {
		gsc_writew(cpu_to_le16(val),
				base_addr + DINO_CONFIG_DATA + (where & 2));
	} else if (size == 4) {
		gsc_writel(cpu_to_le32(val), base_addr + DINO_CONFIG_DATA);
	}

	spin_unlock_irqrestore(&d->dinosaur_pen, flags);
	return 0;
}

static struct pci_ops dino_cfg_ops = {
	.read =		dino_cfg_read,
	.write =	dino_cfg_write,
};


/*
 * Dino "I/O Port" Space Accessor Functions
 *
 * Many PCI devices don't require use of I/O port space (eg Tulip,
 * NCR720) since they export the same registers to both MMIO and
 * I/O port space.  Performance is going to stink if drivers use
 * I/O port instead of MMIO.
 */

#define cpu_to_le8(x) (x)
#define le8_to_cpu(x) (x)

#define DINO_PORT_IN(type, size, mask) \
static u##size dino_in##size (struct pci_hba_data *d, u16 addr) \
{ \
	u##size v; \
	unsigned long flags; \
	spin_lock_irqsave(&(DINO_DEV(d)->dinosaur_pen), flags); \
	/* tell HW which IO Port address */ \
	gsc_writel((u32) addr, d->base_addr + DINO_PCI_ADDR); \
	/* generate I/O PORT read cycle */ \
	v = gsc_read##type(d->base_addr+DINO_IO_DATA+(addr&mask)); \
	spin_unlock_irqrestore(&(DINO_DEV(d)->dinosaur_pen), flags); \
	return le##size##_to_cpu(v); \
}

DINO_PORT_IN(b,  8, 3)
DINO_PORT_IN(w, 16, 2)
DINO_PORT_IN(l, 32, 0)

#define DINO_PORT_OUT(type, size, mask) \
static void dino_out##size (struct pci_hba_data *d, u16 addr, u##size val) \
{ \
	unsigned long flags; \
	spin_lock_irqsave(&(DINO_DEV(d)->dinosaur_pen), flags); \
	/* tell HW which IO port address */ \
	gsc_writel((u32) addr, d->base_addr + DINO_PCI_ADDR); \
	/* generate cfg write cycle */ \
	gsc_write##type(cpu_to_le##size(val), d->base_addr+DINO_IO_DATA+(addr&mask)); \
	spin_unlock_irqrestore(&(DINO_DEV(d)->dinosaur_pen), flags); \
}

DINO_PORT_OUT(b,  8, 3)
DINO_PORT_OUT(w, 16, 2)
DINO_PORT_OUT(l, 32, 0)

struct pci_port_ops dino_port_ops = {
	.inb	= dino_in8,
	.inw	= dino_in16,
	.inl	= dino_in32,
	.outb	= dino_out8,
	.outw	= dino_out16,
	.outl	= dino_out32
};

static void
dino_mask_irq(void *irq_dev, int irq)
{
	struct dino_device *dino_dev = DINO_DEV(irq_dev);

	DBG(KERN_WARNING "%s(0x%p, %d)\n", __FUNCTION__, irq_dev, irq);

	if (NULL == irq_dev || irq > DINO_IRQS || irq < 0) {
		printk(KERN_WARNING "%s(0x%lx, %d) - not a dino irq?\n",
			__FUNCTION__, (long) irq_dev, irq);
		BUG();
	} else {
		/*
		** Clear the matching bit in the IMR register
		*/
		dino_dev->imr &= ~(DINO_MASK_IRQ(irq));
		gsc_writel(dino_dev->imr, dino_dev->hba.base_addr+DINO_IMR);
	}
}


static void
dino_unmask_irq(void *irq_dev, int irq)
{
	struct dino_device *dino_dev = DINO_DEV(irq_dev);
	u32 tmp;

	DBG(KERN_WARNING "%s(0x%p, %d)\n", __FUNCTION__, irq_dev, irq);

	if (NULL == irq_dev || irq > DINO_IRQS) {
		printk(KERN_WARNING "%s(): %d not a dino irq?\n",
				__FUNCTION__, irq);
		BUG();
		return;
	}

	/* set the matching bit in the IMR register */
	dino_dev->imr |= DINO_MASK_IRQ(irq);          /* used in dino_isr() */
	gsc_writel( dino_dev->imr, dino_dev->hba.base_addr+DINO_IMR);

	/* Emulate "Level Triggered" Interrupt
	** Basically, a driver is blowing it if the IRQ line is asserted
	** while the IRQ is disabled.  But tulip.c seems to do that....
	** Give 'em a kluge award and a nice round of applause!
	**
	** The gsc_write will generate an interrupt which invokes dino_isr().
	** dino_isr() will read IPR and find nothing. But then catch this
	** when it also checks ILR.
	*/
	tmp = gsc_readl(dino_dev->hba.base_addr+DINO_ILR);
	if (tmp & DINO_MASK_IRQ(irq)) {
		DBG(KERN_WARNING "%s(): IRQ asserted! (ILR 0x%x)\n",
				__FUNCTION__, tmp);
		gsc_writel(dino_dev->txn_data, dino_dev->txn_addr);
	}
}



static void
dino_enable_irq(void *irq_dev, int irq)
{
	struct dino_device *dino_dev = DINO_DEV(irq_dev);

	/*
	** clear pending IRQ bits
	**
	** This does NOT change ILR state!
	** See comments in dino_unmask_irq() for ILR usage.
	*/
	gsc_readl(dino_dev->hba.base_addr+DINO_IPR);

	dino_unmask_irq(irq_dev, irq);
}


static struct irq_region_ops dino_irq_ops = {
	.disable_irq	= dino_mask_irq,	/* ??? */
	.enable_irq	= dino_enable_irq, 
	.mask_irq	= dino_mask_irq,
	.unmask_irq	= dino_unmask_irq
};


/*
 * Handle a Processor interrupt generated by Dino.
 *
 * ilr_loop counter is a kluge to prevent a "stuck" IRQ line from
 * wedging the CPU. Could be removed or made optional at some point.
 */
static irqreturn_t
dino_isr(int irq, void *intr_dev, struct pt_regs *regs)
{
	struct dino_device *dino_dev = DINO_DEV(intr_dev);
	u32 mask;
	int ilr_loop = 100;
	extern void do_irq(struct irqaction *a, int i, struct pt_regs *p);


	/* read and acknowledge pending interrupts */
#ifdef DINO_DEBUG
	dino_dev->dino_irr0 =
#endif
	mask = gsc_readl(dino_dev->hba.base_addr+DINO_IRR0) & DINO_IRR_MASK;

ilr_again:
	while (mask)
	{
		int irq;

		irq = __ffs(mask);

		mask &= ~(1<<irq);

		DBG(KERN_WARNING "%s(%x, %p) mask %0x\n",
			__FUNCTION__, irq, intr_dev, mask);
		do_irq(&dino_dev->dino_region->action[irq],
			dino_dev->dino_region->data.irqbase + irq,
			regs);

	}

	/* Support for level triggered IRQ lines.
	** 
	** Dropping this support would make this routine *much* faster.
	** But since PCI requires level triggered IRQ line to share lines...
	** device drivers may assume lines are level triggered (and not
	** edge triggered like EISA/ISA can be).
	*/
	mask = gsc_readl(dino_dev->hba.base_addr+DINO_ILR) & dino_dev->imr;
	if (mask) {
		if (--ilr_loop > 0)
			goto ilr_again;
		printk(KERN_ERR "Dino %lx: stuck interrupt %d\n", dino_dev->hba.base_addr, mask);
		return IRQ_NONE;
	}
	return IRQ_HANDLED;
}

static int dino_choose_irq(struct parisc_device *dev)
{
	int irq = -1;

	switch (dev->id.sversion) {
		case 0x00084:	irq =  8; break; /* PS/2 */
		case 0x0008c:	irq = 10; break; /* RS232 */
		case 0x00096:	irq =  8; break; /* PS/2 */
	}

	return irq;
}

static void __init
dino_bios_init(void)
{
	DBG("dino_bios_init\n");
}

/*
 * dino_card_setup - Set up the memory space for a Dino in card mode.
 * @bus: the bus under this dino
 *
 * Claim an 8MB chunk of unused IO space and call the generic PCI routines
 * to set up the addresses of the devices on this bus.
 */
#define _8MB 0x00800000UL
static void __init
dino_card_setup(struct pci_bus *bus, unsigned long base_addr)
{
	int i;
	struct dino_device *dino_dev = DINO_DEV(parisc_walk_tree(bus->bridge));
	struct resource *res;
	char name[128];
	int size;

	res = &dino_dev->hba.lmmio_space;
	res->flags = IORESOURCE_MEM;
	size = scnprintf(name, sizeof(name), "Dino LMMIO (%s)", bus->bridge->bus_id);
	res->name = kmalloc(size+1, GFP_KERNEL);
	if(res->name)
		strcpy((char *)res->name, name);
	else
		res->name = dino_dev->hba.lmmio_space.name;
	

	if (ccio_allocate_resource(dino_dev->hba.dev, res, _8MB,
				F_EXTEND(0xf0000000UL) | _8MB,
				F_EXTEND(0xffffffffUL) &~ _8MB, _8MB) < 0) {
		struct list_head *ln, *tmp_ln;

		printk(KERN_ERR "Dino: cannot attach bus %s\n",
		       bus->bridge->bus_id);
		/* kill the bus, we can't do anything with it */
		list_for_each_safe(ln, tmp_ln, &bus->devices) {
			struct pci_dev *dev = pci_dev_b(ln);

			list_del(&dev->global_list);
			list_del(&dev->bus_list);
		}
			
		return;
	}
	bus->resource[1] = res;
	bus->resource[0] = &(dino_dev->hba.io_space);

	/* Now tell dino what range it has */
	for (i = 1; i < 31; i++) {
		if (res->start == F_EXTEND(0xf0000000UL | (i * _8MB)))
			break;
	}
	DBG("DINO GSC WRITE i=%d, start=%lx, dino addr = %lx\n",
	    i, res->start, base_addr + DINO_IO_ADDR_EN);
	gsc_writel(1 << i, base_addr + DINO_IO_ADDR_EN);
}

static void __init
dino_card_fixup(struct pci_dev *dev)
{
	u32 irq_pin;

	/*
	** REVISIT: card-mode PCI-PCI expansion chassis do exist.
	**         Not sure they were ever productized.
	**         Die here since we'll die later in dino_inb() anyway.
	*/
	if ((dev->class >> 8) == PCI_CLASS_BRIDGE_PCI) {
		panic("Card-Mode Dino: PCI-PCI Bridge not supported\n");
	}

	/*
	** Set Latency Timer to 0xff (not a shared bus)
	** Set CACHELINE_SIZE.
	*/
	dino_cfg_write(dev->bus, dev->devfn, PCI_CACHE_LINE_SIZE, 2, 0xff00 | L1_CACHE_BYTES/4); 

	/*
	** Program INT_LINE for card-mode devices.
	** The cards are hardwired according to this algorithm.
	** And it doesn't matter if PPB's are present or not since
	** the IRQ lines bypass the PPB.
	**
	** "-1" converts INTA-D (1-4) to PCIINTA-D (0-3) range.
	** The additional "-1" adjusts for skewing the IRQ<->slot.
	*/
	dino_cfg_read(dev->bus, dev->devfn, PCI_INTERRUPT_PIN, 1, &irq_pin); 
	dev->irq = (irq_pin + PCI_SLOT(dev->devfn) - 1) % 4 ;

	/* Shouldn't really need to do this but it's in case someone tries
	** to bypass PCI services and look at the card themselves.
	*/
	dino_cfg_write(dev->bus, dev->devfn, PCI_INTERRUPT_LINE, 1, dev->irq); 
}

/* The alignment contraints for PCI bridges under dino */
#define DINO_BRIDGE_ALIGN 0x100000


static void __init
dino_fixup_bus(struct pci_bus *bus)
{
	struct list_head *ln;
        struct pci_dev *dev;
        struct dino_device *dino_dev = DINO_DEV(parisc_walk_tree(bus->bridge));
	int port_base = HBA_PORT_BASE(dino_dev->hba.hba_num);

	DBG(KERN_WARNING "%s(0x%p) bus %d sysdata 0x%p\n",
			__FUNCTION__, bus, bus->secondary, bus->bridge->platform_data);

	/* Firmware doesn't set up card-mode dino, so we have to */
	if (is_card_dino(&dino_dev->hba.dev->id)) {
		dino_card_setup(bus, dino_dev->hba.base_addr);
	} else if(bus->parent == NULL) {
		/* must have a dino above it, reparent the resources
		 * into the dino window */
		int i;
		struct resource *res = &dino_dev->hba.lmmio_space;

		bus->resource[0] = &(dino_dev->hba.io_space);
		for(i = 0; i < DINO_MAX_LMMIO_RESOURCES; i++) {
			if(res[i].flags == 0)
				break;
			bus->resource[i+1] = &res[i];
		}

	} else if(bus->self) {
		int i;

		pci_read_bridge_bases(bus);


		for(i = PCI_BRIDGE_RESOURCES; i < PCI_NUM_RESOURCES; i++) {
			if((bus->self->resource[i].flags & (IORESOURCE_IO | IORESOURCE_MEM)) == 0)
				continue;
			
			if(bus->self->resource[i].flags & IORESOURCE_MEM) {
				/* There's a quirk to alignment of
				 * bridge memory resources: the start
				 * is the alignment and start-end is
				 * the size.  However, firmware will
				 * have assigned start and end, so we
				 * need to take this into account */
				bus->self->resource[i].end = bus->self->resource[i].end - bus->self->resource[i].start + DINO_BRIDGE_ALIGN;
				bus->self->resource[i].start = DINO_BRIDGE_ALIGN;
				
			}
					
			DBG("DEBUG %s assigning %d [0x%lx,0x%lx]\n",
			    bus->self->dev.bus_id, i,
			    bus->self->resource[i].start,
			    bus->self->resource[i].end);
			pci_assign_resource(bus->self, i);
			DBG("DEBUG %s after assign %d [0x%lx,0x%lx]\n",
			    bus->self->dev.bus_id, i,
			    bus->self->resource[i].start,
			    bus->self->resource[i].end);
		}
	}


	list_for_each(ln, &bus->devices) {
		int i;

		dev = pci_dev_b(ln);
		if (is_card_dino(&dino_dev->hba.dev->id))
			dino_card_fixup(dev);

		/*
		** P2PB's only have 2 BARs, no IRQs.
		** I'd like to just ignore them for now.
		*/
		if ((dev->class >> 8) == PCI_CLASS_BRIDGE_PCI)
			continue;

		/* Adjust the I/O Port space addresses */
		for (i = 0; i < PCI_NUM_RESOURCES; i++) {
			struct resource *res = &dev->resource[i];
			if (res->flags & IORESOURCE_IO) {
				res->start |= port_base;
				res->end |= port_base;
			}
#ifdef __LP64__
			/* Sign Extend MMIO addresses */
			else if (res->flags & IORESOURCE_MEM) {
				res->start |= F_EXTEND(0UL);
				res->end   |= F_EXTEND(0UL);
			}
#endif
		}
		/* null out the ROM resource if there is one (we don't
		 * care about an expansion rom on parisc, since it
		 * usually contains (x86) bios code) */
		dev->resource[PCI_ROM_RESOURCE].flags = 0;
				
		if(dev->irq == 255) {

#define DINO_FIX_UNASSIGNED_INTERRUPTS
#ifdef DINO_FIX_UNASSIGNED_INTERRUPTS

			/* This code tries to assign an unassigned
			 * interrupt.  Leave it disabled unless you
			 * *really* know what you're doing since the
			 * pin<->interrupt line mapping varies by bus
			 * and machine */

			u32 irq_pin;
			
			dino_cfg_read(dev->bus, dev->devfn, PCI_INTERRUPT_PIN, 1, &irq_pin);
			dev->irq = (irq_pin + PCI_SLOT(dev->devfn) - 1) % 4 ;
			dino_cfg_write(dev->bus, dev->devfn, PCI_INTERRUPT_LINE, 1, dev->irq);
			dev->irq += dino_dev->dino_region->data.irqbase;
			printk(KERN_WARNING "Device %s has undefined IRQ, setting to %d\n", dev->slot_name, irq_pin);
#else
			dev->irq = 65535;
			printk(KERN_WARNING "Device %s has unassigned IRQ\n", dev->slot_name);	
#endif
		} else {

			/* Adjust INT_LINE for that busses region */
			dev->irq += dino_dev->dino_region->data.irqbase;
		}
	}
}


struct pci_bios_ops dino_bios_ops = {
	.init		= dino_bios_init,
	.fixup_bus	= dino_fixup_bus
};


/*
 *	Initialise a DINO controller chip
 */
static void __init
dino_card_init(struct dino_device *dino_dev)
{
	u32 brdg_feat = 0x00784e05;

	gsc_writel(0x00000000, dino_dev->hba.base_addr+DINO_GMASK);
	gsc_writel(0x00000001, dino_dev->hba.base_addr+DINO_IO_FBB_EN);
	gsc_writel(0x00000000, dino_dev->hba.base_addr+DINO_ICR);

#if 1
/* REVISIT - should be a runtime check (eg if (CPU_IS_PCX_L) ...) */
	/*
	** PCX-L processors don't support XQL like Dino wants it.
	** PCX-L2 ignore XQL signal and it doesn't matter.
	*/
	brdg_feat &= ~0x4;	/* UXQL */
#endif
	gsc_writel( brdg_feat, dino_dev->hba.base_addr+DINO_BRDG_FEAT);

	/*
	** Don't enable address decoding until we know which I/O range
	** currently is available from the host. Only affects MMIO
	** and not I/O port space.
	*/
	gsc_writel(0x00000000, dino_dev->hba.base_addr+DINO_IO_ADDR_EN);

	gsc_writel(0x00000000, dino_dev->hba.base_addr+DINO_DAMODE);
	gsc_writel(0x00222222, dino_dev->hba.base_addr+DINO_PCIROR);
	gsc_writel(0x00222222, dino_dev->hba.base_addr+DINO_PCIWOR);

	gsc_writel(0x00000040, dino_dev->hba.base_addr+DINO_MLTIM);
	gsc_writel(0x00000080, dino_dev->hba.base_addr+DINO_IO_CONTROL);
	gsc_writel(0x0000008c, dino_dev->hba.base_addr+DINO_TLTIM);

	/* Disable PAMR before writing PAPR */
	gsc_writel(0x0000007e, dino_dev->hba.base_addr+DINO_PAMR);
	gsc_writel(0x0000007f, dino_dev->hba.base_addr+DINO_PAPR);
	gsc_writel(0x00000000, dino_dev->hba.base_addr+DINO_PAMR);

	/*
	** Dino ERS encourages enabling FBB (0x6f).
	** We can't until we know *all* devices below us can support it.
	** (Something in device configuration header tells us).
	*/
	gsc_writel(0x0000004f, dino_dev->hba.base_addr+DINO_PCICMD);

	/* Somewhere, the PCI spec says give devices 1 second
	** to recover from the #RESET being de-asserted.
	** Experience shows most devices only need 10ms.
	** This short-cut speeds up booting significantly.
	*/
	mdelay(pci_post_reset_delay);
}

static int __init
dino_bridge_init(struct dino_device *dino_dev, const char *name)
{
	unsigned long io_addr;
	int result, i, count=0;
	struct resource *res, *prevres = NULL;
	/*
	 * Decoding IO_ADDR_EN only works for Built-in Dino
	 * since PDC has already initialized this.
	 */

	io_addr = gsc_readl(dino_dev->hba.base_addr + DINO_IO_ADDR_EN);
	if (io_addr == 0) {
		printk(KERN_WARNING "%s: No PCI devices enabled.\n", name);
		return -ENODEV;
	}

	res = &dino_dev->hba.lmmio_space;
	for (i = 0; i < 32; i++) {
		unsigned long start, end;

		if((io_addr & (1 << i)) == 0)
			continue;

		start = (unsigned long)(signed int)(0xf0000000 | (i << 23));
		end = start + 8 * 1024 * 1024 - 1;

		DBG("DINO RANGE %d is at 0x%lx-0x%lx\n", count,
		    start, end);

		if(prevres && prevres->end + 1 == start) {
			prevres->end = end;
		} else {
			if(count >= DINO_MAX_LMMIO_RESOURCES) {
				printk(KERN_ERR "%s is out of resource windows for range %d (0x%lx-0x%lx)\n", name, count, start, end);
				break;
			}
			prevres = res;
			res->start = start;
			res->end = end;
			res->flags = IORESOURCE_MEM;
			res->name = kmalloc(64, GFP_KERNEL);
			if(res->name)
				snprintf((char *)res->name, 64, "%s LMMIO %d",
					 name, count);
			res++;
			count++;
		}
	}

	res = &dino_dev->hba.lmmio_space;

	for(i = 0; i < DINO_MAX_LMMIO_RESOURCES; i++) {
		if(res[i].flags == 0)
			break;

		result = ccio_request_resource(dino_dev->hba.dev, &res[i]);
		if (result < 0) {
			printk(KERN_ERR "%s: failed to claim PCI Bus address space %d (0x%lx-0x%lx)!\n", name, i, res[i].start, res[i].end);
			return result;
		}
	}
	return 0;
}

static int __init dino_common_init(struct parisc_device *dev,
		struct dino_device *dino_dev, const char *name)
{
	int status;
	u32 eim;
	struct gsc_irq gsc_irq;
	struct resource *res;

	pcibios_register_hba(&dino_dev->hba);

	pci_bios = &dino_bios_ops;   /* used by pci_scan_bus() */
	pci_port = &dino_port_ops;

	/*
	** Note: SMP systems can make use of IRR1/IAR1 registers
	**   But it won't buy much performance except in very
	**   specific applications/configurations. Note Dino
	**   still only has 11 IRQ input lines - just map some of them
	**   to a different processor.
	*/
	dino_dev->irq = gsc_alloc_irq(&gsc_irq);
	dino_dev->txn_addr = gsc_irq.txn_addr;
	dino_dev->txn_data = gsc_irq.txn_data;
	eim = ((u32) gsc_irq.txn_addr) | gsc_irq.txn_data;

	/* 
	** Dino needs a PA "IRQ" to get a processor's attention.
	** arch/parisc/kernel/irq.c returns an EIRR bit.
	*/
	if (dino_dev->irq < 0) {
		printk(KERN_WARNING "%s: gsc_alloc_irq() failed\n", name);
		return 1;
	}

	status = request_irq(dino_dev->irq, dino_isr, 0, name, dino_dev);
	if (status) {
		printk(KERN_WARNING "%s: request_irq() failed with %d\n", 
			name, status);
		return 1;
	}

	/*
	** Tell generic interrupt support we have 11 bits which need
	** be checked in the interrupt handler.
	*/
	dino_dev->dino_region = alloc_irq_region(DINO_IRQS, &dino_irq_ops,
						name, dino_dev);

	if (NULL == dino_dev->dino_region) {
		printk(KERN_WARNING "%s: alloc_irq_region() failed\n", name);
		return 1;
	}

	/* Support the serial port which is sometimes attached on built-in
	 * Dino / Cujo chips.
	 */

	fixup_child_irqs(dev, dino_dev->dino_region->data.irqbase,
			dino_choose_irq);

	/*
	** This enables DINO to generate interrupts when it sees
	** any of its inputs *change*. Just asserting an IRQ
	** before it's enabled (ie unmasked) isn't good enough.
	*/
	gsc_writel(eim, dino_dev->hba.base_addr+DINO_IAR0);

	/*
	** Some platforms don't clear Dino's IRR0 register at boot time.
	** Reading will clear it now.
	*/
	gsc_readl(dino_dev->hba.base_addr+DINO_IRR0);

	/* allocate I/O Port resource region */
	res = &dino_dev->hba.io_space;
	if (dev->id.hversion == 0x680 || is_card_dino(&dev->id)) {
		res->name = "Dino I/O Port";
	} else {
		res->name = "Cujo I/O Port";
	}
	res->start = HBA_PORT_BASE(dino_dev->hba.hba_num);
	res->end = res->start + (HBA_PORT_SPACE_SIZE - 1);
	res->flags = IORESOURCE_IO; /* do not mark it busy ! */
	if (request_resource(&ioport_resource, res) < 0) {
		printk(KERN_ERR "%s: request I/O Port region failed 0x%lx/%lx (hpa 0x%lx)\n",
				name, res->start, res->end, dino_dev->hba.base_addr);
		return 1;
	}

	return 0;
}

#define CUJO_RAVEN_ADDR		F_EXTEND(0xf1000000UL)
#define CUJO_FIREHAWK_ADDR	F_EXTEND(0xf1604000UL)
#define CUJO_RAVEN_BADPAGE	0x01003000UL
#define CUJO_FIREHAWK_BADPAGE	0x01607000UL

static const char *dino_vers[] = {
	"2.0",
	"2.1",
	"3.0",
	"3.1"
};

static const char *cujo_vers[] = {
	"1.0",
	"2.0"
};

void ccio_cujo20_fixup(struct parisc_device *dev, u32 iovp);

/*
** Determine if dino should claim this chip (return 0) or not (return 1).
** If so, initialize the chip appropriately (card-mode vs bridge mode).
** Much of the initialization is common though.
*/
static int __init
dino_driver_callback(struct parisc_device *dev)
{
	struct dino_device *dino_dev;	// Dino specific control struct
	const char *version = "unknown";
	const int name_len = 32;
	char hw_path[64];
	char *name;
	int is_cujo = 0;
	struct pci_bus *bus;
	
	name = kmalloc(name_len, GFP_KERNEL);
	if(name) {
		print_pa_hwpath(dev, hw_path);
		snprintf(name, name_len, "Dino [%s]", hw_path);
	} 
	else
		name = "Dino";

	if (is_card_dino(&dev->id)) {
		version = "3.x (card mode)";
	} else {
		if(dev->id.hversion == 0x680) {
			if (dev->id.hversion_rev < 4) {
				version = dino_vers[dev->id.hversion_rev];
			}
		} else {
			name = "Cujo";
			is_cujo = 1;
			if (dev->id.hversion_rev < 2) {
				version = cujo_vers[dev->id.hversion_rev];
			}
		}
	}

	printk("%s version %s found at 0x%lx\n", name, version, dev->hpa);

	if (!request_mem_region(dev->hpa, PAGE_SIZE, name)) {
		printk(KERN_ERR "DINO: Hey! Someone took my MMIO space (0x%ld)!\n",
			dev->hpa);
		return 1;
	}

	/* Check for bugs */
	if (is_cujo && dev->id.hversion_rev == 1) {
#ifdef CONFIG_IOMMU_CCIO
		printk(KERN_WARNING "Enabling Cujo 2.0 bug workaround\n");
		if (dev->hpa == (unsigned long)CUJO_RAVEN_ADDR) {
			ccio_cujo20_fixup(dev->parent, CUJO_RAVEN_BADPAGE);
		} else if (dev->hpa == (unsigned long)CUJO_FIREHAWK_ADDR) {
			ccio_cujo20_fixup(dev->parent, CUJO_FIREHAWK_BADPAGE);
		} else {
			printk("Don't recognise Cujo at address 0x%lx, not enabling workaround\n", dev->hpa);
		}
#endif
	} else if (!is_cujo && !is_card_dino(&dev->id) &&
			dev->id.hversion_rev < 3) {
		printk(KERN_WARNING
"The GSCtoPCI (Dino hrev %d) bus converter found may exhibit\n"
"data corruption.  See Service Note Numbers: A4190A-01, A4191A-01.\n"
"Systems shipped after Aug 20, 1997 will not exhibit this problem.\n"
"Models affected: C180, C160, C160L, B160L, and B132L workstations.\n\n",
			dev->id.hversion_rev);
/* REVISIT: why are C200/C240 listed in the README table but not
**   "Models affected"? Could be an omission in the original literature.
*/
	}

	dino_dev = kmalloc(sizeof(struct dino_device), GFP_KERNEL);
	if (!dino_dev) {
		printk("dino_init_chip - couldn't alloc dino_device\n");
		return 1;
	}

	memset(dino_dev, 0, sizeof(struct dino_device));

	dino_dev->hba.dev = dev;
	dino_dev->hba.base_addr = dev->hpa;  /* faster access */
	dino_dev->hba.lmmio_space_offset = 0;	/* CPU addrs == bus addrs */
	dino_dev->dinosaur_pen = SPIN_LOCK_UNLOCKED;
	dino_dev->hba.iommu = ccio_get_iommu(dev);

	if (is_card_dino(&dev->id)) {
		dino_card_init(dino_dev);
	} else {
		dino_bridge_init(dino_dev, name);
	}

	if (dino_common_init(dev, dino_dev, name))
		return 1;

	dev->dev.platform_data = dino_dev;

	/*
	** It's not used to avoid chicken/egg problems
	** with configuration accessor functions.
	*/
	bus = pci_scan_bus_parented(&dev->dev, dino_current_bus,
				    &dino_cfg_ops, NULL);
	if(bus) {
		/* This code *depends* on scanning being single threaded
		 * if it isn't, this global bus number count will fail
		 */
		dino_current_bus = bus->subordinate + 1;
		pci_bus_assign_resources(bus);
	} else {
		printk(KERN_ERR "ERROR: failed to scan PCI bus on %s (probably duplicate bus number %d)\n", dev->dev.bus_id, dino_current_bus);
		/* increment the bus number in case of duplicates */
		dino_current_bus++;
	}
	dino_dev->hba.hba_bus = bus;
	return 0;
}

/*
 * Normally, we would just test sversion.  But the Elroy PCI adapter has
 * the same sversion as Dino, so we have to check hversion as well.
 * Unfortunately, the J2240 PDC reports the wrong hversion for the first
 * Dino, so we have to test for Dino, Cujo and Dino-in-a-J2240.
 * For card-mode Dino, most machines report an sversion of 9D.  But 715
 * and 725 firmware misreport it as 0x08080 for no adequately explained
 * reason.
 */
static struct parisc_device_id dino_tbl[] = {
	{ HPHW_A_DMA, HVERSION_REV_ANY_ID, 0x004, 0x0009D },/* Card-mode Dino */
	{ HPHW_A_DMA, HVERSION_REV_ANY_ID, HVERSION_ANY_ID, 0x08080 }, /* XXX */
	{ HPHW_BRIDGE, HVERSION_REV_ANY_ID, 0x680, 0xa }, /* Bridge-mode Dino */
	{ HPHW_BRIDGE, HVERSION_REV_ANY_ID, 0x682, 0xa }, /* Bridge-mode Cujo */
	{ HPHW_BRIDGE, HVERSION_REV_ANY_ID, 0x05d, 0xa }, /* Dino in a J2240 */
	{ 0, }
};

static struct parisc_driver dino_driver = {
	.name =		"Dino",
	.id_table =	dino_tbl,
	.probe =	dino_driver_callback,
};

/*
 * One time initialization to let the world know Dino is here.
 * This is the only routine which is NOT static.
 * Must be called exactly once before pci_init().
 */
int __init dino_init(void)
{
	register_parisc_driver(&dino_driver);
	return 0;
}

