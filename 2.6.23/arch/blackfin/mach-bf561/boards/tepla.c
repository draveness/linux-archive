/*
 *  File: arch/blackfin/mach-bf561/tepla.c
 *
 *  Copyright 2004-2007 Analog Devices Inc.
 *  Only SMSC91C1111 was registered, may do more later.
 *
 *  Copyright 2005 National ICT Australia (NICTA), Aidan Williams <aidan@nicta.com.au>
 *  Thanks to Jamey Hicks.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */

#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/irq.h>

char *bfin_board_name = "Tepla-BF561";

/*
 *  Driver needs to know address, irq and flag pin.
 */
static struct resource smc91x_resources[] = {
	{
		.start	= 0x2C000300,
		.end	= 0x2C000320,
		.flags	= IORESOURCE_MEM,
	}, {
		.start	= IRQ_PROG_INTB,
		.end	= IRQ_PROG_INTB,
		.flags	= IORESOURCE_IRQ|IORESOURCE_IRQ_HIGHLEVEL,
	}, {
		/*
		 *  denotes the flag pin and is used directly if
		 *  CONFIG_IRQCHIP_DEMUX_GPIO is defined.
		 */
		.start	= IRQ_PF7,
		.end	= IRQ_PF7,
		.flags	= IORESOURCE_IRQ|IORESOURCE_IRQ_HIGHLEVEL,
	},
};

static struct platform_device smc91x_device = {
	.name          = "smc91x",
	.id            = 0,
	.num_resources = ARRAY_SIZE(smc91x_resources),
	.resource      = smc91x_resources,
};

static struct platform_device *tepla_devices[] __initdata = {
	&smc91x_device,
};

static int __init tepla_init(void)
{
	printk(KERN_INFO "%s(): registering device resources\n", __FUNCTION__);
	return platform_add_devices(tepla_devices, ARRAY_SIZE(tepla_devices));
}

arch_initcall(tepla_init);
