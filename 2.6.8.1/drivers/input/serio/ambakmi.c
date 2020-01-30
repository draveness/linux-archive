/*
 *  linux/drivers/input/serio/ambakmi.c
 *
 *  Copyright (C) 2000-2003 Deep Blue Solutions Ltd.
 *  Copyright (C) 2002 Russell King.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/serio.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/err.h>

#include <asm/io.h>
#include <asm/irq.h>
#include <asm/hardware/amba.h>
#include <asm/hardware/amba_kmi.h>
#include <asm/hardware/clock.h>

#define KMI_BASE	(kmi->base)

struct amba_kmi_port {
	struct serio		io;
	struct clk		*clk;
	unsigned char		*base;
	unsigned int		irq;
	unsigned int		divisor;
	unsigned int		open;
};

static irqreturn_t amba_kmi_int(int irq, void *dev_id, struct pt_regs *regs)
{
	struct amba_kmi_port *kmi = dev_id;
	unsigned int status = readb(KMIIR);
	int handled = IRQ_NONE;

	while (status & KMIIR_RXINTR) {
		serio_interrupt(&kmi->io, readb(KMIDATA), 0, regs);
		status = readb(KMIIR);
		handled = IRQ_HANDLED;
	}

	return handled;
}

static int amba_kmi_write(struct serio *io, unsigned char val)
{
	struct amba_kmi_port *kmi = io->driver;
	unsigned int timeleft = 10000; /* timeout in 100ms */

	while ((readb(KMISTAT) & KMISTAT_TXEMPTY) == 0 && timeleft--)
		udelay(10);

	if (timeleft)
		writeb(val, KMIDATA);

	return timeleft ? 0 : SERIO_TIMEOUT;
}

static int amba_kmi_open(struct serio *io)
{
	struct amba_kmi_port *kmi = io->driver;
	unsigned int divisor;
	int ret;

	ret = clk_use(kmi->clk);
	if (ret)
		goto out;

	ret = clk_enable(kmi->clk);
	if (ret)
		goto clk_unuse;

	divisor = clk_get_rate(kmi->clk) / 8000000 - 1;
	writeb(divisor, KMICLKDIV);
	writeb(KMICR_EN, KMICR);

	ret = request_irq(kmi->irq, amba_kmi_int, 0, "kmi-pl050", kmi);
	if (ret) {
		printk(KERN_ERR "kmi: failed to claim IRQ%d\n", kmi->irq);
		writeb(0, KMICR);
		goto clk_disable;
	}

	writeb(KMICR_EN | KMICR_RXINTREN, KMICR);

	return 0;

 clk_disable:
	clk_disable(kmi->clk);
 clk_unuse:
	clk_unuse(kmi->clk);
 out:
	return ret;
}

static void amba_kmi_close(struct serio *io)
{
	struct amba_kmi_port *kmi = io->driver;

	writeb(0, KMICR);

	free_irq(kmi->irq, kmi);
	clk_disable(kmi->clk);
	clk_unuse(kmi->clk);
}

static int amba_kmi_probe(struct amba_device *dev, void *id)
{
	struct amba_kmi_port *kmi;
	int ret;

	ret = amba_request_regions(dev, NULL);
	if (ret)
		return ret;

	kmi = kmalloc(sizeof(struct amba_kmi_port), GFP_KERNEL);
	if (!kmi) {
		ret = -ENOMEM;
		goto out;
	}

	memset(kmi, 0, sizeof(struct amba_kmi_port));

	kmi->io.type	= SERIO_8042;
	kmi->io.write	= amba_kmi_write;
	kmi->io.open	= amba_kmi_open;
	kmi->io.close	= amba_kmi_close;
	kmi->io.name	= dev->dev.bus_id;
	kmi->io.phys	= dev->dev.bus_id;
	kmi->io.driver	= kmi;

	kmi->base	= ioremap(dev->res.start, KMI_SIZE);
	if (!kmi->base) {
		ret = -ENOMEM;
		goto out;
	}

	kmi->clk = clk_get(&dev->dev, "KMIREFCLK");
	if (IS_ERR(kmi->clk)) {
		ret = PTR_ERR(kmi->clk);
		goto unmap;
	}

	kmi->irq = dev->irq[0];
	amba_set_drvdata(dev, kmi);

	serio_register_port(&kmi->io);
	return 0;

 unmap:
	iounmap(kmi->base);
 out:
	kfree(kmi);
	amba_release_regions(dev);
	return ret;
}

static int amba_kmi_remove(struct amba_device *dev)
{
	struct amba_kmi_port *kmi = amba_get_drvdata(dev);

	amba_set_drvdata(dev, NULL);

	serio_unregister_port(&kmi->io);
	clk_put(kmi->clk);
	iounmap(kmi->base);
	kfree(kmi);
	amba_release_regions(dev);
	return 0;
}

static int amba_kmi_resume(struct amba_device *dev)
{
	struct amba_kmi_port *kmi = amba_get_drvdata(dev);

	/* kick the serio layer to rescan this port */
	serio_rescan(&kmi->io);

	return 0;
}

static struct amba_id amba_kmi_idtable[] = {
	{
		.id	= 0x00041050,
		.mask	= 0x000fffff,
	},
	{ 0, 0 }
};

static struct amba_driver ambakmi_driver = {
	.drv		= {
		.name	= "kmi-pl050",
	},
	.id_table	= amba_kmi_idtable,
	.probe		= amba_kmi_probe,
	.remove		= amba_kmi_remove,
	.resume		= amba_kmi_resume,
};

static int __init amba_kmi_init(void)
{
	return amba_driver_register(&ambakmi_driver);
}

static void __exit amba_kmi_exit(void)
{
	return amba_driver_unregister(&ambakmi_driver);
}

module_init(amba_kmi_init);
module_exit(amba_kmi_exit);

MODULE_AUTHOR("Russell King <rmk@arm.linux.org.uk>");
MODULE_DESCRIPTION("AMBA KMI controller driver");
MODULE_LICENSE("GPL");
