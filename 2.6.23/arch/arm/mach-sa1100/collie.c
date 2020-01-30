/*
 * linux/arch/arm/mach-sa1100/collie.c
 *
 * May be copied or modified under the terms of the GNU General Public
 * License.  See linux/COPYING for more information.
 *
 * This file contains all Collie-specific tweaks.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * ChangeLog:
 *  2006 Pavel Machek <pavel@suse.cz>
 *  03-06-2004 John Lenz <lenz@cs.wisc.edu>
 *  06-04-2002 Chris Larson <kergoth@digitalnemesis.net>
 *  04-16-2001 Lineo Japan,Inc. ...
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/tty.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/timer.h>

#include <asm/hardware.h>
#include <asm/mach-types.h>
#include <asm/irq.h>
#include <asm/setup.h>
#include <asm/arch/collie.h>

#include <asm/mach/arch.h>
#include <asm/mach/flash.h>
#include <asm/mach/map.h>
#include <asm/mach/serial_sa1100.h>

#include <asm/hardware/scoop.h>
#include <asm/mach/sharpsl_param.h>
#include <asm/hardware/locomo.h>
#include <asm/arch/mcp.h>

#include "generic.h"

static struct resource collie_scoop_resources[] = {
	[0] = {
		.start		= 0x40800000,
		.end		= 0x40800fff,
		.flags		= IORESOURCE_MEM,
	},
};

static struct scoop_config collie_scoop_setup = {
	.io_dir 	= COLLIE_SCOOP_IO_DIR,
	.io_out		= COLLIE_SCOOP_IO_OUT,
};

struct platform_device colliescoop_device = {
	.name		= "sharp-scoop",
	.id		= -1,
	.dev		= {
 		.platform_data	= &collie_scoop_setup,
	},
	.num_resources	= ARRAY_SIZE(collie_scoop_resources),
	.resource	= collie_scoop_resources,
};

static struct scoop_pcmcia_dev collie_pcmcia_scoop[] = {
{
       .dev        = &colliescoop_device.dev,
       .irq        = COLLIE_IRQ_GPIO_CF_IRQ,
       .cd_irq     = COLLIE_IRQ_GPIO_CF_CD,
       .cd_irq_str = "PCMCIA0 CD",
},
};

static struct scoop_pcmcia_config collie_pcmcia_config = {
	.devs         = &collie_pcmcia_scoop[0],
	.num_devs     = 1,
};


static struct mcp_plat_data collie_mcp_data = {
	.mccr0          = MCCR0_ADM | MCCR0_ExtClk,
	.sclk_rate      = 9216000,
};

#ifdef CONFIG_SHARP_LOCOMO
/*
 * low-level UART features.
 */
struct platform_device collie_locomo_device;

static void collie_uart_set_mctrl(struct uart_port *port, u_int mctrl)
{
 	if (mctrl & TIOCM_RTS)
		locomo_gpio_write(&collie_locomo_device.dev, LOCOMO_GPIO_RTS, 0);
 	else
		locomo_gpio_write(&collie_locomo_device.dev, LOCOMO_GPIO_RTS, 1);

 	if (mctrl & TIOCM_DTR)
		locomo_gpio_write(&collie_locomo_device.dev, LOCOMO_GPIO_DTR, 0);
 	else
		locomo_gpio_write(&collie_locomo_device.dev, LOCOMO_GPIO_DTR, 1);
}

static u_int collie_uart_get_mctrl(struct uart_port *port)
{
	int ret = TIOCM_CD;
	unsigned int r;

	r = locomo_gpio_read_output(&collie_locomo_device.dev, LOCOMO_GPIO_CTS & LOCOMO_GPIO_DSR);
	if (r == -ENODEV)
		return ret;
	if (r & LOCOMO_GPIO_CTS)
		ret |= TIOCM_CTS;
	if (r & LOCOMO_GPIO_DSR)
		ret |= TIOCM_DSR;

	return ret;
}

static struct sa1100_port_fns collie_port_fns __initdata = {
	.set_mctrl	= collie_uart_set_mctrl,
	.get_mctrl	= collie_uart_get_mctrl,
};

static int collie_uart_probe(struct locomo_dev *dev)
{
	return 0;
}

static int collie_uart_remove(struct locomo_dev *dev)
{
	return 0;
}

static struct locomo_driver collie_uart_driver = {
	.drv = {
		.name = "collie_uart",
	},
	.devid	= LOCOMO_DEVID_UART,
	.probe	= collie_uart_probe,
	.remove	= collie_uart_remove,
};

static int __init collie_uart_init(void) {
	return locomo_driver_register(&collie_uart_driver);
}
device_initcall(collie_uart_init);

#endif


static struct resource locomo_resources[] = {
	[0] = {
		.start		= 0x40000000,
		.end		= 0x40001fff,
		.flags		= IORESOURCE_MEM,
	},
	[1] = {
		.start		= IRQ_GPIO25,
		.end		= IRQ_GPIO25,
		.flags		= IORESOURCE_IRQ,
	},
};

struct platform_device collie_locomo_device = {
	.name		= "locomo",
	.id		= 0,
	.num_resources	= ARRAY_SIZE(locomo_resources),
	.resource	= locomo_resources,
};

static struct platform_device *devices[] __initdata = {
	&collie_locomo_device,
	&colliescoop_device,
};

static struct mtd_partition collie_partitions[] = {
	{
		.name		= "bootloader",
		.offset 	= 0,
		.size		= 0x000C0000,
		.mask_flags	= MTD_WRITEABLE
	}, {
		.name		= "kernel",
		.offset 	= MTDPART_OFS_APPEND,
		.size		= 0x00100000,
	}, {
		.name		= "rootfs",
		.offset 	= MTDPART_OFS_APPEND,
		.size		= 0x00e20000,
	}
};

static void collie_set_vpp(int vpp)
{
	write_scoop_reg(&colliescoop_device.dev, SCOOP_GPCR, read_scoop_reg(&colliescoop_device.dev, SCOOP_GPCR) | COLLIE_SCP_VPEN);
	if (vpp)
		write_scoop_reg(&colliescoop_device.dev, SCOOP_GPWR, read_scoop_reg(&colliescoop_device.dev, SCOOP_GPWR) | COLLIE_SCP_VPEN);
	else
		write_scoop_reg(&colliescoop_device.dev, SCOOP_GPWR, read_scoop_reg(&colliescoop_device.dev, SCOOP_GPWR) & ~COLLIE_SCP_VPEN);
}

static struct flash_platform_data collie_flash_data = {
	.map_name	= "cfi_probe",
	.set_vpp	= collie_set_vpp,
	.parts		= collie_partitions,
	.nr_parts	= ARRAY_SIZE(collie_partitions),
};

static struct resource collie_flash_resources[] = {
	{
		.start	= SA1100_CS0_PHYS,
		.end	= SA1100_CS0_PHYS + SZ_32M - 1,
		.flags	= IORESOURCE_MEM,
	}
};

static void __init collie_init(void)
{
	int ret = 0;

	/* cpu initialize */
	GAFR = ( GPIO_SSP_TXD | \
		 GPIO_SSP_SCLK | GPIO_SSP_SFRM | GPIO_SSP_CLK | GPIO_TIC_ACK | \
		 GPIO_32_768kHz );

	GPDR = ( GPIO_LDD8 | GPIO_LDD9 | GPIO_LDD10 | GPIO_LDD11 | GPIO_LDD12 | \
		 GPIO_LDD13 | GPIO_LDD14 | GPIO_LDD15 | GPIO_SSP_TXD | \
		 GPIO_SSP_SCLK | GPIO_SSP_SFRM | GPIO_SDLC_SCLK | \
		 GPIO_SDLC_AAF | GPIO_UART_SCLK1 | GPIO_32_768kHz );
	GPLR = GPIO_GPIO18;

	// PPC pin setting
	PPDR = ( PPC_LDD0 | PPC_LDD1 | PPC_LDD2 | PPC_LDD3 | PPC_LDD4 | PPC_LDD5 | \
		 PPC_LDD6 | PPC_LDD7 | PPC_L_PCLK | PPC_L_LCLK | PPC_L_FCLK | PPC_L_BIAS | \
	 	 PPC_TXD1 | PPC_TXD2 | PPC_RXD2 | PPC_TXD3 | PPC_TXD4 | PPC_SCLK | PPC_SFRM );

	PSDR = ( PPC_RXD1 | PPC_RXD2 | PPC_RXD3 | PPC_RXD4 );

	GAFR |= GPIO_32_768kHz;
	GPDR |= GPIO_32_768kHz;
	TUCR  = TUCR_32_768kHz;

	platform_scoop_config = &collie_pcmcia_config;

	ret = platform_add_devices(devices, ARRAY_SIZE(devices));
	if (ret) {
		printk(KERN_WARNING "collie: Unable to register LoCoMo device\n");
	}

	sa11x0_set_flash_data(&collie_flash_data, collie_flash_resources,
			      ARRAY_SIZE(collie_flash_resources));
	sa11x0_set_mcp_data(&collie_mcp_data);

	sharpsl_save_param();
}

static struct map_desc collie_io_desc[] __initdata = {
	{	/* 32M main flash (cs0) */
		.virtual	= 0xe8000000,
		.pfn		= __phys_to_pfn(0x00000000),
		.length		= 0x02000000,
		.type		= MT_DEVICE
	}, {	/* 32M boot flash (cs1) */
		.virtual	= 0xea000000,
		.pfn		= __phys_to_pfn(0x08000000),
		.length		= 0x02000000,
		.type		= MT_DEVICE
	}
};

static void __init collie_map_io(void)
{
	sa1100_map_io();
	iotable_init(collie_io_desc, ARRAY_SIZE(collie_io_desc));

#ifdef CONFIG_SHARP_LOCOMO
	sa1100_register_uart_fns(&collie_port_fns);
#endif
	sa1100_register_uart(0, 3);
	sa1100_register_uart(1, 1);
}

MACHINE_START(COLLIE, "Sharp-Collie")
	.phys_io	= 0x80000000,
	.io_pg_offst	= ((0xf8000000) >> 18) & 0xfffc,
	.map_io		= collie_map_io,
	.init_irq	= sa1100_init_irq,
	.timer		= &sa1100_timer,
	.init_machine	= collie_init,
MACHINE_END
