/*
 * linux/arch/arm/common/locomo.c
 *
 * Sharp LoCoMo support
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This file contains all generic LoCoMo support.
 *
 * All initialization functions provided here are intended to be called
 * from machine specific code with proper arguments when required.
 *
 * Based on sa1111.c
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <asm/hardware.h>
#include <asm/mach-types.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/mach/irq.h>

#include <asm/hardware/locomo.h>

/* the following is the overall data for the locomo chip */
struct locomo {
	struct device *dev;
	unsigned long phys;
	unsigned int irq;
	void *base;
};

struct locomo_dev_info {
	unsigned long	offset;
	unsigned long	length;
	unsigned int	devid;
	unsigned int	irq[1];
	const char *	name;
};

static struct locomo_dev_info locomo_devices[] = {
};


/** LoCoMo interrupt handling stuff.
 * NOTE: LoCoMo has a 1 to many mapping on all of its IRQs.
 * that is, there is only one real hardware interrupt
 * we determine which interrupt it is by reading some IO memory.
 * We have two levels of expansion, first in the handler for the
 * hardware interrupt we generate an interrupt
 * IRQ_LOCOMO_*_BASE and those handlers generate more interrupts
 *
 * hardware irq reads LOCOMO_ICR & 0x0f00
 *   IRQ_LOCOMO_KEY_BASE
 *   IRQ_LOCOMO_GPIO_BASE
 *   IRQ_LOCOMO_LT_BASE
 *   IRQ_LOCOMO_SPI_BASE
 * IRQ_LOCOMO_KEY_BASE reads LOCOMO_KIC & 0x0001
 *   IRQ_LOCOMO_KEY
 * IRQ_LOCOMO_GPIO_BASE reads LOCOMO_GIR & LOCOMO_GPD & 0xffff
 *   IRQ_LOCOMO_GPIO[0-15]
 * IRQ_LOCOMO_LT_BASE reads LOCOMO_LTINT & 0x0001
 *   IRQ_LOCOMO_LT
 * IRQ_LOCOMO_SPI_BASE reads LOCOMO_SPIIR & 0x000F
 *   IRQ_LOCOMO_SPI_RFR
 *   IRQ_LOCOMO_SPI_RFW
 *   IRQ_LOCOMO_SPI_OVRN
 *   IRQ_LOCOMO_SPI_TEND
 */

#define LOCOMO_IRQ_START	(IRQ_LOCOMO_KEY_BASE)
#define LOCOMO_IRQ_KEY_START	(IRQ_LOCOMO_KEY)
#define	LOCOMO_IRQ_GPIO_START	(IRQ_LOCOMO_GPIO0)
#define	LOCOMO_IRQ_LT_START	(IRQ_LOCOMO_LT)
#define	LOCOMO_IRQ_SPI_START	(IRQ_LOCOMO_SPI_RFR)

static void locomo_handler(unsigned int irq, struct irqdesc *desc,
			struct pt_regs *regs)
{
	int req, i;
	struct irqdesc *d;
	void *mapbase = get_irq_chipdata(irq);

	/* Acknowledge the parent IRQ */
	desc->chip->ack(irq);

	/* check why this interrupt was generated */
	req = locomo_readl(mapbase + LOCOMO_ICR) & 0x0f00;

	if (req) {
		/* generate the next interrupt(s) */
		irq = LOCOMO_IRQ_START;
		d = irq_desc + irq;
		for (i = 0; i <= 3; i++, d++, irq++) {
			if (req & (0x0100 << i)) {
				d->handle(irq, d, regs);
			}

		}
	}
}

static void locomo_ack_irq(unsigned int irq)
{
}

static void locomo_mask_irq(unsigned int irq)
{
	void *mapbase = get_irq_chipdata(irq);
	unsigned int r;
	r = locomo_readl(mapbase + LOCOMO_ICR);
	r &= ~(0x0010 << (irq - LOCOMO_IRQ_START));
	locomo_writel(r, mapbase + LOCOMO_ICR);
}

static void locomo_unmask_irq(unsigned int irq)
{
	void *mapbase = get_irq_chipdata(irq);
	unsigned int r;
	r = locomo_readl(mapbase + LOCOMO_ICR);
	r |= (0x0010 << (irq - LOCOMO_IRQ_START));
	locomo_writel(r, mapbase + LOCOMO_ICR);
}

static struct irqchip locomo_chip = {
	.ack	= locomo_ack_irq,
	.mask	= locomo_mask_irq,
	.unmask	= locomo_unmask_irq,
};

static void locomo_key_handler(unsigned int irq, struct irqdesc *desc,
			    struct pt_regs *regs)
{
	struct irqdesc *d;
	void *mapbase = get_irq_chipdata(irq);

	if (locomo_readl(mapbase + LOCOMO_KIC) & 0x0001) {
		d = irq_desc + LOCOMO_IRQ_KEY_START;
		d->handle(LOCOMO_IRQ_KEY_START, d, regs);
	}
}

static void locomo_key_ack_irq(unsigned int irq)
{
	void *mapbase = get_irq_chipdata(irq);
	unsigned int r;
	r = locomo_readl(mapbase + LOCOMO_KIC);
	r &= ~(0x0100 << (irq - LOCOMO_IRQ_KEY_START));
	locomo_writel(r, mapbase + LOCOMO_KIC);
}

static void locomo_key_mask_irq(unsigned int irq)
{
	void *mapbase = get_irq_chipdata(irq);
	unsigned int r;
	r = locomo_readl(mapbase + LOCOMO_KIC);
	r &= ~(0x0010 << (irq - LOCOMO_IRQ_KEY_START));
	locomo_writel(r, mapbase + LOCOMO_KIC);
}

static void locomo_key_unmask_irq(unsigned int irq)
{
	void *mapbase = get_irq_chipdata(irq);
	unsigned int r;
	r = locomo_readl(mapbase + LOCOMO_KIC);
	r |= (0x0010 << (irq - LOCOMO_IRQ_KEY_START));
	locomo_writel(r, mapbase + LOCOMO_KIC);
}

static struct irqchip locomo_key_chip = {
	.ack	= locomo_key_ack_irq,
	.mask	= locomo_key_mask_irq,
	.unmask	= locomo_key_unmask_irq,
};

static void locomo_gpio_handler(unsigned int irq, struct irqdesc *desc,
			     struct pt_regs *regs)
{
	int req, i;
	struct irqdesc *d;
	void *mapbase = get_irq_chipdata(irq);

	req = 	locomo_readl(mapbase + LOCOMO_GIR) &
		locomo_readl(mapbase + LOCOMO_GPD) &
		0xffff;

	if (req) {
		irq = LOCOMO_IRQ_GPIO_START;
		d = irq_desc + LOCOMO_IRQ_GPIO_START;
		for (i = 0; i <= 15; i++, irq++, d++) {
			if (req & (0x0001 << i)) {
				d->handle(irq, d, regs);
			}
		}
	}
}

static void locomo_gpio_ack_irq(unsigned int irq)
{
	void *mapbase = get_irq_chipdata(irq);
	unsigned int r;
	r = locomo_readl(mapbase + LOCOMO_GWE);
	r |= (0x0001 << (irq - LOCOMO_IRQ_GPIO_START));
	locomo_writel(r, mapbase + LOCOMO_GWE);

	r = locomo_readl(mapbase + LOCOMO_GIS);
	r &= ~(0x0001 << (irq - LOCOMO_IRQ_GPIO_START));
	locomo_writel(r, mapbase + LOCOMO_GIS);

	r = locomo_readl(mapbase + LOCOMO_GWE);
	r &= ~(0x0001 << (irq - LOCOMO_IRQ_GPIO_START));
	locomo_writel(r, mapbase + LOCOMO_GWE);
}

static void locomo_gpio_mask_irq(unsigned int irq)
{
	void *mapbase = get_irq_chipdata(irq);
	unsigned int r;
	r = locomo_readl(mapbase + LOCOMO_GIE);
	r &= ~(0x0001 << (irq - LOCOMO_IRQ_GPIO_START));
	locomo_writel(r, mapbase + LOCOMO_GIE);
}

static void locomo_gpio_unmask_irq(unsigned int irq)
{
	void *mapbase = get_irq_chipdata(irq);
	unsigned int r;
	r = locomo_readl(mapbase + LOCOMO_GIE);
	r |= (0x0001 << (irq - LOCOMO_IRQ_GPIO_START));
	locomo_writel(r, mapbase + LOCOMO_GIE);
}

static struct irqchip locomo_gpio_chip = {
	.ack	= locomo_gpio_ack_irq,
	.mask	= locomo_gpio_mask_irq,
	.unmask	= locomo_gpio_unmask_irq,
};

static void locomo_lt_handler(unsigned int irq, struct irqdesc *desc,
			   struct pt_regs *regs)
{
	struct irqdesc *d;
	void *mapbase = get_irq_chipdata(irq);

	if (locomo_readl(mapbase + LOCOMO_LTINT) & 0x0001) {
		d = irq_desc + LOCOMO_IRQ_LT_START;
		d->handle(LOCOMO_IRQ_LT_START, d, regs);
	}
}

static void locomo_lt_ack_irq(unsigned int irq)
{
	void *mapbase = get_irq_chipdata(irq);
	unsigned int r;
	r = locomo_readl(mapbase + LOCOMO_LTINT);
	r &= ~(0x0100 << (irq - LOCOMO_IRQ_LT_START));
	locomo_writel(r, mapbase + LOCOMO_LTINT);
}

static void locomo_lt_mask_irq(unsigned int irq)
{
	void *mapbase = get_irq_chipdata(irq);
	unsigned int r;
	r = locomo_readl(mapbase + LOCOMO_LTINT);
	r &= ~(0x0010 << (irq - LOCOMO_IRQ_LT_START));
	locomo_writel(r, mapbase + LOCOMO_LTINT);
}

static void locomo_lt_unmask_irq(unsigned int irq)
{
	void *mapbase = get_irq_chipdata(irq);
	unsigned int r;
	r = locomo_readl(mapbase + LOCOMO_LTINT);
	r |= (0x0010 << (irq - LOCOMO_IRQ_LT_START));
	locomo_writel(r, mapbase + LOCOMO_LTINT);
}

static struct irqchip locomo_lt_chip = {
	.ack	= locomo_lt_ack_irq,
	.mask	= locomo_lt_mask_irq,
	.unmask	= locomo_lt_unmask_irq,
};

static void locomo_spi_handler(unsigned int irq, struct irqdesc *desc,
			    struct pt_regs *regs)
{
	int req, i;
	struct irqdesc *d;
	void *mapbase = get_irq_chipdata(irq);

	req = locomo_readl(mapbase + LOCOMO_SPIIR) & 0x000F;
	if (req) {
		irq = LOCOMO_IRQ_SPI_START;
		d = irq_desc + irq;

		for (i = 0; i <= 3; i++, irq++, d++) {
			if (req & (0x0001 << i)) {
				d->handle(irq, d, regs);
			}
		}
	}
}

static void locomo_spi_ack_irq(unsigned int irq)
{
	void *mapbase = get_irq_chipdata(irq);
	unsigned int r;
	r = locomo_readl(mapbase + LOCOMO_SPIWE);
	r |= (0x0001 << (irq - LOCOMO_IRQ_SPI_START));
	locomo_writel(r, mapbase + LOCOMO_SPIWE);

	r = locomo_readl(mapbase + LOCOMO_SPIIS);
	r &= ~(0x0001 << (irq - LOCOMO_IRQ_SPI_START));
	locomo_writel(r, mapbase + LOCOMO_SPIIS);

	r = locomo_readl(mapbase + LOCOMO_SPIWE);
	r &= ~(0x0001 << (irq - LOCOMO_IRQ_SPI_START));
	locomo_writel(r, mapbase + LOCOMO_SPIWE);
}

static void locomo_spi_mask_irq(unsigned int irq)
{
	void *mapbase = get_irq_chipdata(irq);
	unsigned int r;
	r = locomo_readl(mapbase + LOCOMO_SPIIE);
	r &= ~(0x0001 << (irq - LOCOMO_IRQ_SPI_START));
	locomo_writel(r, mapbase + LOCOMO_SPIIE);
}

static void locomo_spi_unmask_irq(unsigned int irq)
{
	void *mapbase = get_irq_chipdata(irq);
	unsigned int r;
	r = locomo_readl(mapbase + LOCOMO_SPIIE);
	r |= (0x0001 << (irq - LOCOMO_IRQ_SPI_START));
	locomo_writel(r, mapbase + LOCOMO_SPIIE);
}

static struct irqchip locomo_spi_chip = {
	.ack	= locomo_spi_ack_irq,
	.mask	= locomo_spi_mask_irq,
	.unmask	= locomo_spi_unmask_irq,
};

static void locomo_setup_irq(struct locomo *lchip)
{
	int irq;
	void *irqbase = lchip->base;

	/*
	 * Install handler for IRQ_LOCOMO_HW.
	 */
	set_irq_type(lchip->irq, IRQT_FALLING);
	set_irq_chipdata(lchip->irq, irqbase);
	set_irq_chained_handler(lchip->irq, locomo_handler);

	/* Install handlers for IRQ_LOCOMO_*_BASE */
	set_irq_chip(IRQ_LOCOMO_KEY_BASE, &locomo_chip);
	set_irq_chipdata(IRQ_LOCOMO_KEY_BASE, irqbase);
	set_irq_chained_handler(IRQ_LOCOMO_KEY_BASE, locomo_key_handler);
	set_irq_flags(IRQ_LOCOMO_KEY_BASE, IRQF_VALID | IRQF_PROBE);

	set_irq_chip(IRQ_LOCOMO_GPIO_BASE, &locomo_chip);
	set_irq_chipdata(IRQ_LOCOMO_GPIO_BASE, irqbase);
	set_irq_chained_handler(IRQ_LOCOMO_GPIO_BASE, locomo_gpio_handler);
	set_irq_flags(IRQ_LOCOMO_GPIO_BASE, IRQF_VALID | IRQF_PROBE);

	set_irq_chip(IRQ_LOCOMO_LT_BASE, &locomo_chip);
	set_irq_chipdata(IRQ_LOCOMO_LT_BASE, irqbase);
	set_irq_chained_handler(IRQ_LOCOMO_LT_BASE, locomo_lt_handler);
	set_irq_flags(IRQ_LOCOMO_LT_BASE, IRQF_VALID | IRQF_PROBE);

	set_irq_chip(IRQ_LOCOMO_SPI_BASE, &locomo_chip);
	set_irq_chipdata(IRQ_LOCOMO_SPI_BASE, irqbase);
	set_irq_chained_handler(IRQ_LOCOMO_SPI_BASE, locomo_spi_handler);
	set_irq_flags(IRQ_LOCOMO_SPI_BASE, IRQF_VALID | IRQF_PROBE);

	/* install handlers for IRQ_LOCOMO_KEY_BASE generated interrupts */
	set_irq_chip(LOCOMO_IRQ_KEY_START, &locomo_key_chip);
	set_irq_chipdata(LOCOMO_IRQ_KEY_START, irqbase);
	set_irq_handler(LOCOMO_IRQ_KEY_START, do_edge_IRQ);
	set_irq_flags(LOCOMO_IRQ_KEY_START, IRQF_VALID | IRQF_PROBE);

	/* install handlers for IRQ_LOCOMO_GPIO_BASE generated interrupts */
	for (irq = LOCOMO_IRQ_GPIO_START; irq < LOCOMO_IRQ_GPIO_START + 16; irq++) {
		set_irq_chip(irq, &locomo_gpio_chip);
		set_irq_chipdata(irq, irqbase);
		set_irq_handler(irq, do_edge_IRQ);
		set_irq_flags(irq, IRQF_VALID | IRQF_PROBE);
	}

	/* install handlers for IRQ_LOCOMO_LT_BASE generated interrupts */
	set_irq_chip(LOCOMO_IRQ_LT_START, &locomo_lt_chip);
	set_irq_chipdata(LOCOMO_IRQ_LT_START, irqbase);
	set_irq_handler(LOCOMO_IRQ_LT_START, do_edge_IRQ);
	set_irq_flags(LOCOMO_IRQ_LT_START, IRQF_VALID | IRQF_PROBE);

	/* install handlers for IRQ_LOCOMO_SPI_BASE generated interrupts */
	for (irq = LOCOMO_IRQ_SPI_START; irq < LOCOMO_IRQ_SPI_START + 3; irq++) {
		set_irq_chip(irq, &locomo_spi_chip);
		set_irq_chipdata(irq, irqbase);
		set_irq_handler(irq, do_edge_IRQ);
		set_irq_flags(irq, IRQF_VALID | IRQF_PROBE);
	}
}


static void locomo_dev_release(struct device *_dev)
{
	struct locomo_dev *dev = LOCOMO_DEV(_dev);

	release_resource(&dev->res);
	kfree(dev);
}

static int
locomo_init_one_child(struct locomo *lchip, struct resource *parent,
		      struct locomo_dev_info *info)
{
	struct locomo_dev *dev;
	int ret;

	dev = kmalloc(sizeof(struct locomo_dev), GFP_KERNEL);
	if (!dev) {
		ret = -ENOMEM;
		goto out;
	}
	memset(dev, 0, sizeof(struct locomo_dev));

	strncpy(dev->dev.bus_id,info->name,sizeof(dev->dev.bus_id));
	/*
	 * If the parent device has a DMA mask associated with it,
	 * propagate it down to the children.
	 */
	if (lchip->dev->dma_mask) {
		dev->dma_mask = *lchip->dev->dma_mask;
		dev->dev.dma_mask = &dev->dma_mask;
	}

	dev->devid	 = info->devid;
	dev->dev.parent  = lchip->dev;
	dev->dev.bus     = &locomo_bus_type;
	dev->dev.release = locomo_dev_release;
	dev->dev.coherent_dma_mask = lchip->dev->coherent_dma_mask;
	dev->res.start   = lchip->phys + info->offset;
	dev->res.end     = dev->res.start + info->length;
	dev->res.name    = dev->dev.bus_id;
	dev->res.flags   = IORESOURCE_MEM;
	dev->mapbase     = lchip->base + info->offset;
	memmove(dev->irq, info->irq, sizeof(dev->irq));

	if (info->length) {
		ret = request_resource(parent, &dev->res);
		if (ret) {
			printk("LoCoMo: failed to allocate resource for %s\n",
				dev->res.name);
			goto out;
		}
	}

	ret = device_register(&dev->dev);
	if (ret) {
		release_resource(&dev->res);
 out:
		kfree(dev);
	}
	return ret;
}

/**
 *	locomo_probe - probe for a single LoCoMo chip.
 *	@phys_addr: physical address of device.
 *
 *	Probe for a LoCoMo chip.  This must be called
 *	before any other locomo-specific code.
 *
 *	Returns:
 *	%-ENODEV	device not found.
 *	%-EBUSY		physical address already marked in-use.
 *	%0		successful.
 */
static int
__locomo_probe(struct device *me, struct resource *mem, int irq)
{
	struct locomo *lchip;
	unsigned long r;
	int i, ret = -ENODEV;

	lchip = kmalloc(sizeof(struct locomo), GFP_KERNEL);
	if (!lchip)
		return -ENOMEM;

	memset(lchip, 0, sizeof(struct locomo));

	lchip->dev = me;
	dev_set_drvdata(lchip->dev, lchip);

	lchip->phys = mem->start;
	lchip->irq = irq;

	/*
	 * Map the whole region.  This also maps the
	 * registers for our children.
	 */
	lchip->base = ioremap(mem->start, PAGE_SIZE);
	if (!lchip->base) {
		ret = -ENOMEM;
		goto out;
	}

	/* locomo initialize */
	locomo_writel(0, lchip->base + LOCOMO_ICR);
	/* KEYBOARD */
	locomo_writel(0, lchip->base + LOCOMO_KIC);

	/* GPIO */
	locomo_writel(0, lchip->base + LOCOMO_GPO);
	locomo_writel( (LOCOMO_GPIO(2) | LOCOMO_GPIO(3) | LOCOMO_GPIO(13) | LOCOMO_GPIO(14))
			, lchip->base + LOCOMO_GPE);
	locomo_writel( (LOCOMO_GPIO(2) | LOCOMO_GPIO(3) | LOCOMO_GPIO(13) | LOCOMO_GPIO(14))
			, lchip->base + LOCOMO_GPD);
	locomo_writel(0, lchip->base + LOCOMO_GIE);

	/* FrontLight */
	locomo_writel(0, lchip->base + LOCOMO_ALS);
	locomo_writel(0, lchip->base + LOCOMO_ALD);
	/* Longtime timer */
	locomo_writel(0, lchip->base + LOCOMO_LTINT);
	/* SPI */
	locomo_writel(0, lchip->base + LOCOMO_SPIIE);

	locomo_writel(6 + 8 + 320 + 30 - 10, lchip->base + LOCOMO_ASD);
	r = locomo_readl(lchip->base + LOCOMO_ASD);
	r |= 0x8000;
	locomo_writel(r, lchip->base + LOCOMO_ASD);

	locomo_writel(6 + 8 + 320 + 30 - 10 - 128 + 4, lchip->base + LOCOMO_HSD);
	r = locomo_readl(lchip->base + LOCOMO_HSD);
	r |= 0x8000;
	locomo_writel(r, lchip->base + LOCOMO_HSD);

	locomo_writel(128 / 8, lchip->base + LOCOMO_HSC);

	/* XON */
	locomo_writel(0x80, lchip->base + LOCOMO_TADC);
	udelay(1000);
	/* CLK9MEN */
	r = locomo_readl(lchip->base + LOCOMO_TADC);
	r |= 0x10;
	locomo_writel(r, lchip->base + LOCOMO_TADC);
	udelay(100);

	/* init DAC */
	r = locomo_readl(lchip->base + LOCOMO_DAC);
	r |= LOCOMO_DAC_SCLOEB | LOCOMO_DAC_SDAOEB;
	locomo_writel(r, lchip->base + LOCOMO_DAC);

	r = locomo_readl(lchip->base + LOCOMO_VER);
	printk(KERN_INFO "LoCoMo Chip: %lu%lu\n", (r >> 8), (r & 0xff));

	/*
	 * The interrupt controller must be initialised before any
	 * other device to ensure that the interrupts are available.
	 */
	if (lchip->irq != NO_IRQ)
		locomo_setup_irq(lchip);

	for (i = 0; i < ARRAY_SIZE(locomo_devices); i++)
		locomo_init_one_child(lchip, mem, &locomo_devices[i]);

	return 0;

 out:
	kfree(lchip);
	return ret;
}

static void __locomo_remove(struct locomo *lchip)
{
	struct list_head *l, *n;

	list_for_each_safe(l, n, &lchip->dev->children) {
		struct device *d = list_to_dev(l);

		device_unregister(d);
	}

	if (lchip->irq != NO_IRQ) {
		set_irq_chained_handler(lchip->irq, NULL);
		set_irq_data(lchip->irq, NULL);
	}

	iounmap(lchip->base);
	kfree(lchip);
}

static int locomo_probe(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct resource *mem = NULL, *irq = NULL;
	int i;

	for (i = 0; i < pdev->num_resources; i++) {
		if (pdev->resource[i].flags & IORESOURCE_MEM)
			mem = &pdev->resource[i];
		if (pdev->resource[i].flags & IORESOURCE_IRQ)
			irq = &pdev->resource[i];
	}

	return __locomo_probe(dev, mem, irq ? irq->start : NO_IRQ);
}

static int locomo_remove(struct device *dev)
{
	struct locomo *lchip = dev_get_drvdata(dev);

	if (lchip) {
		__locomo_remove(lchip);
		dev_set_drvdata(dev, NULL);

		kfree(dev->saved_state);
		dev->saved_state = NULL;
	}

	return 0;
}

/*
 *	Not sure if this should be on the system bus or not yet.
 *	We really want some way to register a system device at
 *	the per-machine level, and then have this driver pick
 *	up the registered devices.
 */
static struct device_driver locomo_device_driver = {
	.name		= "locomo",
	.bus		= &platform_bus_type,
	.probe		= locomo_probe,
	.remove		= locomo_remove,
};

/*
 *	Get the parent device driver (us) structure
 *	from a child function device
 */
static inline struct locomo *locomo_chip_driver(struct locomo_dev *ldev)
{
	return (struct locomo *)dev_get_drvdata(ldev->dev.parent);
}

/*
 *	LoCoMo "Register Access Bus."
 *
 *	We model this as a regular bus type, and hang devices directly
 *	off this.
 */
static int locomo_match(struct device *_dev, struct device_driver *_drv)
{
	struct locomo_dev *dev = LOCOMO_DEV(_dev);
	struct locomo_driver *drv = LOCOMO_DRV(_drv);

	return dev->devid == drv->devid;
}

static int locomo_bus_suspend(struct device *dev, u32 state)
{
	struct locomo_dev *ldev = LOCOMO_DEV(dev);
	struct locomo_driver *drv = LOCOMO_DRV(dev->driver);
	int ret = 0;

	if (drv && drv->suspend)
		ret = drv->suspend(ldev, state);
	return ret;
}

static int locomo_bus_resume(struct device *dev)
{
	struct locomo_dev *ldev = LOCOMO_DEV(dev);
	struct locomo_driver *drv = LOCOMO_DRV(dev->driver);
	int ret = 0;

	if (drv && drv->resume)
		ret = drv->resume(ldev);
	return ret;
}

static int locomo_bus_probe(struct device *dev)
{
	struct locomo_dev *ldev = LOCOMO_DEV(dev);
	struct locomo_driver *drv = LOCOMO_DRV(dev->driver);
	int ret = -ENODEV;

	if (drv->probe)
		ret = drv->probe(ldev);
	return ret;
}

static int locomo_bus_remove(struct device *dev)
{
	struct locomo_dev *ldev = LOCOMO_DEV(dev);
	struct locomo_driver *drv = LOCOMO_DRV(dev->driver);
	int ret = 0;

	if (drv->remove)
		ret = drv->remove(ldev);
	return ret;
}

struct bus_type locomo_bus_type = {
	.name		= "locomo-bus",
	.match		= locomo_match,
	.suspend	= locomo_bus_suspend,
	.resume		= locomo_bus_resume,
};

int locomo_driver_register(struct locomo_driver *driver)
{
	driver->drv.probe = locomo_bus_probe;
	driver->drv.remove = locomo_bus_remove;
	driver->drv.bus = &locomo_bus_type;
	return driver_register(&driver->drv);
}

void locomo_driver_unregister(struct locomo_driver *driver)
{
	driver_unregister(&driver->drv);
}

static int __init locomo_init(void)
{
	int ret = bus_register(&locomo_bus_type);
	if (ret == 0)
		driver_register(&locomo_device_driver);
	return ret;
}

static void __exit locomo_exit(void)
{
	driver_unregister(&locomo_device_driver);
	bus_unregister(&locomo_bus_type);
}

module_init(locomo_init);
module_exit(locomo_exit);

MODULE_DESCRIPTION("Sharp LoCoMo core driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("John Lenz <jelenz@students.wisc.edu>");

EXPORT_SYMBOL(locomo_driver_register);
EXPORT_SYMBOL(locomo_driver_unregister);
