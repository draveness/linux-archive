/*
 * linux/arch/arm/mach-omap1/board-h3.c
 *
 * This file contains OMAP1710 H3 specific code.
 *
 * Copyright (C) 2004 Texas Instruments, Inc.
 * Copyright (C) 2002 MontaVista Software, Inc.
 * Copyright (C) 2001 RidgeRun, Inc.
 * Author: RidgeRun, Inc.
 *         Greg Lonnon (glonnon@ridgerun.com) or info@ridgerun.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/types.h>
#include <linux/init.h>
#include <linux/major.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/errno.h>
#include <linux/workqueue.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/partitions.h>
#include <linux/input.h>

#include <asm/setup.h>
#include <asm/page.h>
#include <asm/hardware.h>
#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/flash.h>
#include <asm/mach/map.h>

#include <asm/arch/gpio.h>
#include <asm/arch/gpioexpander.h>
#include <asm/arch/irqs.h>
#include <asm/arch/mux.h>
#include <asm/arch/tc.h>
#include <asm/arch/irda.h>
#include <asm/arch/usb.h>
#include <asm/arch/keypad.h>
#include <asm/arch/dma.h>
#include <asm/arch/common.h>

extern int omap_gpio_init(void);

static int h3_keymap[] = {
	KEY(0, 0, KEY_LEFT),
	KEY(0, 1, KEY_RIGHT),
	KEY(0, 2, KEY_3),
	KEY(0, 3, KEY_F10),
	KEY(0, 4, KEY_F5),
	KEY(0, 5, KEY_9),
	KEY(1, 0, KEY_DOWN),
	KEY(1, 1, KEY_UP),
	KEY(1, 2, KEY_2),
	KEY(1, 3, KEY_F9),
	KEY(1, 4, KEY_F7),
	KEY(1, 5, KEY_0),
	KEY(2, 0, KEY_ENTER),
	KEY(2, 1, KEY_6),
	KEY(2, 2, KEY_1),
	KEY(2, 3, KEY_F2),
	KEY(2, 4, KEY_F6),
	KEY(2, 5, KEY_HOME),
	KEY(3, 0, KEY_8),
	KEY(3, 1, KEY_5),
	KEY(3, 2, KEY_F12),
	KEY(3, 3, KEY_F3),
	KEY(3, 4, KEY_F8),
	KEY(3, 5, KEY_END),
	KEY(4, 0, KEY_7),
	KEY(4, 1, KEY_4),
	KEY(4, 2, KEY_F11),
	KEY(4, 3, KEY_F1),
	KEY(4, 4, KEY_F4),
	KEY(4, 5, KEY_ESC),
	KEY(5, 0, KEY_F13),
	KEY(5, 1, KEY_F14),
	KEY(5, 2, KEY_F15),
	KEY(5, 3, KEY_F16),
	KEY(5, 4, KEY_SLEEP),
	0
};


static struct mtd_partition nor_partitions[] = {
	/* bootloader (U-Boot, etc) in first sector */
	{
	      .name		= "bootloader",
	      .offset		= 0,
	      .size		= SZ_128K,
	      .mask_flags	= MTD_WRITEABLE, /* force read-only */
	},
	/* bootloader params in the next sector */
	{
	      .name		= "params",
	      .offset		= MTDPART_OFS_APPEND,
	      .size		= SZ_128K,
	      .mask_flags	= 0,
	},
	/* kernel */
	{
	      .name		= "kernel",
	      .offset		= MTDPART_OFS_APPEND,
	      .size		= SZ_2M,
	      .mask_flags	= 0
	},
	/* file system */
	{
	      .name		= "filesystem",
	      .offset		= MTDPART_OFS_APPEND,
	      .size		= MTDPART_SIZ_FULL,
	      .mask_flags	= 0
	}
};

static struct flash_platform_data nor_data = {
	.map_name	= "cfi_probe",
	.width		= 2,
	.parts		= nor_partitions,
	.nr_parts	= ARRAY_SIZE(nor_partitions),
};

static struct resource nor_resource = {
	/* This is on CS3, wherever it's mapped */
	.flags		= IORESOURCE_MEM,
};

static struct platform_device nor_device = {
	.name		= "omapflash",
	.id		= 0,
	.dev		= {
		.platform_data	= &nor_data,
	},
	.num_resources	= 1,
	.resource	= &nor_resource,
};

static struct mtd_partition nand_partitions[] = {
#if 0
	/* REVISIT: enable these partitions if you make NAND BOOT work */
	{
		.name		= "xloader",
		.offset		= 0,
		.size		= 64 * 1024,
		.mask_flags	= MTD_WRITEABLE,	/* force read-only */
	},
	{
		.name		= "bootloader",
		.offset		= MTDPART_OFS_APPEND,
		.size		= 256 * 1024,
		.mask_flags	= MTD_WRITEABLE,	/* force read-only */
	},
	{
		.name		= "params",
		.offset		= MTDPART_OFS_APPEND,
		.size		= 192 * 1024,
	},
	{
		.name		= "kernel",
		.offset		= MTDPART_OFS_APPEND,
		.size		= 2 * SZ_1M,
	},
#endif
	{
		.name		= "filesystem",
		.size		= MTDPART_SIZ_FULL,
		.offset		= MTDPART_OFS_APPEND,
	},
};

/* dip switches control NAND chip access:  8 bit, 16 bit, or neither */
static struct nand_platform_data nand_data = {
	.options	= NAND_SAMSUNG_LP_OPTIONS,
	.parts		= nand_partitions,
	.nr_parts	= ARRAY_SIZE(nand_partitions),
};

static struct resource nand_resource = {
	.flags		= IORESOURCE_MEM,
};

static struct platform_device nand_device = {
	.name		= "omapnand",
	.id		= 0,
	.dev		= {
		.platform_data	= &nand_data,
	},
	.num_resources	= 1,
	.resource	= &nand_resource,
};

static struct resource smc91x_resources[] = {
	[0] = {
		.start	= OMAP1710_ETHR_START,		/* Physical */
		.end	= OMAP1710_ETHR_START + 0xf,
		.flags	= IORESOURCE_MEM,
	},
	[1] = {
		.start	= OMAP_GPIO_IRQ(40),
		.end	= OMAP_GPIO_IRQ(40),
		.flags	= IORESOURCE_IRQ,
	},
};

static struct platform_device smc91x_device = {
	.name		= "smc91x",
	.id		= 0,
	.num_resources	= ARRAY_SIZE(smc91x_resources),
	.resource	= smc91x_resources,
};

#define GPTIMER_BASE		0xFFFB1400
#define GPTIMER_REGS(x)	(0xFFFB1400 + (x * 0x800))
#define GPTIMER_REGS_SIZE	0x46

static struct resource intlat_resources[] = {
	[0] = {
		.start  = GPTIMER_REGS(0),	      /* Physical */
		.end    = GPTIMER_REGS(0) + GPTIMER_REGS_SIZE,
		.flags  = IORESOURCE_MEM,
	},
	[1] = {
		.start  = INT_1610_GPTIMER1,
		.end    = INT_1610_GPTIMER1,
		.flags  = IORESOURCE_IRQ,
	},
};

static struct platform_device intlat_device = {
	.name	   = "omap_intlat",
	.id	     = 0,
	.num_resources  = ARRAY_SIZE(intlat_resources),
	.resource       = intlat_resources,
};

static struct resource h3_kp_resources[] = {
	[0] = {
		.start	= INT_KEYBOARD,
		.end	= INT_KEYBOARD,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct omap_kp_platform_data h3_kp_data = {
	.rows		= 8,
	.cols		= 8,
	.keymap		= h3_keymap,
	.keymapsize	= ARRAY_SIZE(h3_keymap),
	.rep		= 1,
	.delay		= 9,
	.dbounce	= 1,
};

static struct platform_device h3_kp_device = {
	.name		= "omap-keypad",
	.id		= -1,
	.dev		= {
		.platform_data = &h3_kp_data,
	},
	.num_resources	= ARRAY_SIZE(h3_kp_resources),
	.resource	= h3_kp_resources,
};


/* Select between the IrDA and aGPS module
 */
static int h3_select_irda(struct device *dev, int state)
{
	unsigned char expa;
	int err = 0;

	if ((err = read_gpio_expa(&expa, 0x26))) {
		printk(KERN_ERR "Error reading from I/O EXPANDER \n");
		return err;
	}

	/* 'P6' enable/disable IRDA_TX and IRDA_RX */
	if (state & IR_SEL) { /* IrDA */
		if ((err = write_gpio_expa(expa | 0x40, 0x26))) {
			printk(KERN_ERR "Error writing to I/O EXPANDER \n");
			return err;
		}
	} else {
		if ((err = write_gpio_expa(expa & ~0x40, 0x26))) {
			printk(KERN_ERR "Error writing to I/O EXPANDER \n");
			return err;
		}
	}
	return err;
}

static void set_trans_mode(struct work_struct *work)
{
	struct omap_irda_config *irda_config =
		container_of(work, struct omap_irda_config, gpio_expa.work);
	int mode = irda_config->mode;
	unsigned char expa;
	int err = 0;

	if ((err = read_gpio_expa(&expa, 0x27)) != 0) {
		printk(KERN_ERR "Error reading from I/O expander\n");
	}

	expa &= ~0x03;

	if (mode & IR_SIRMODE) {
		expa |= 0x01;
	} else { /* MIR/FIR */
		expa |= 0x03;
	}

	if ((err = write_gpio_expa(expa, 0x27)) != 0) {
		printk(KERN_ERR "Error writing to I/O expander\n");
	}
}

static int h3_transceiver_mode(struct device *dev, int mode)
{
	struct omap_irda_config *irda_config = dev->platform_data;

	irda_config->mode = mode;
	cancel_delayed_work(&irda_config->gpio_expa);
	PREPARE_DELAYED_WORK(&irda_config->gpio_expa, set_trans_mode);
	schedule_delayed_work(&irda_config->gpio_expa, 0);

	return 0;
}

static struct omap_irda_config h3_irda_data = {
	.transceiver_cap	= IR_SIRMODE | IR_MIRMODE | IR_FIRMODE,
	.transceiver_mode	= h3_transceiver_mode,
	.select_irda	 	= h3_select_irda,
	.rx_channel		= OMAP_DMA_UART3_RX,
	.tx_channel		= OMAP_DMA_UART3_TX,
	.dest_start		= UART3_THR,
	.src_start		= UART3_RHR,
	.tx_trigger		= 0,
	.rx_trigger		= 0,
};

static struct resource h3_irda_resources[] = {
	[0] = {
		.start	= INT_UART3,
		.end	= INT_UART3,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct platform_device h3_irda_device = {
	.name		= "omapirda",
	.id		= 0,
	.dev		= {
		.platform_data	= &h3_irda_data,
	},
	.num_resources	= ARRAY_SIZE(h3_irda_resources),
	.resource	= h3_irda_resources,
};

static struct platform_device h3_lcd_device = {
	.name		= "lcd_h3",
	.id		= -1,
};

static struct platform_device *devices[] __initdata = {
	&nor_device,
	&nand_device,
        &smc91x_device,
	&intlat_device,
	&h3_irda_device,
	&h3_kp_device,
	&h3_lcd_device,
};

static struct omap_usb_config h3_usb_config __initdata = {
	/* usb1 has a Mini-AB port and external isp1301 transceiver */
	.otg	    = 2,

#ifdef CONFIG_USB_GADGET_OMAP
	.hmc_mode       = 19,   /* 0:host(off) 1:dev|otg 2:disabled */
#elif  defined(CONFIG_USB_OHCI_HCD) || defined(CONFIG_USB_OHCI_HCD_MODULE)
	/* NONSTANDARD CABLE NEEDED (B-to-Mini-B) */
	.hmc_mode       = 20,   /* 1:dev|otg(off) 1:host 2:disabled */
#endif

	.pins[1]	= 3,
};

static struct omap_mmc_config h3_mmc_config __initdata = {
	.mmc[0] = {
		.enabled 	= 1,
		.power_pin	= -1,   /* tps65010 GPIO4 */
		.switch_pin	= OMAP_MPUIO(1),
	},
};

static struct omap_uart_config h3_uart_config __initdata = {
	.enabled_uarts = ((1 << 0) | (1 << 1) | (1 << 2)),
};

static struct omap_lcd_config h3_lcd_config __initdata = {
	.ctrl_name	= "internal",
};

static struct omap_board_config_kernel h3_config[] = {
	{ OMAP_TAG_USB,		&h3_usb_config },
	{ OMAP_TAG_MMC,		&h3_mmc_config },
	{ OMAP_TAG_UART,	&h3_uart_config },
	{ OMAP_TAG_LCD,		&h3_lcd_config },
};

#define H3_NAND_RB_GPIO_PIN	10

static int nand_dev_ready(struct nand_platform_data *data)
{
	return omap_get_gpio_datain(H3_NAND_RB_GPIO_PIN);
}

static void __init h3_init(void)
{
	/* Here we assume the NOR boot config:  NOR on CS3 (possibly swapped
	 * to address 0 by a dip switch), NAND on CS2B.  The NAND driver will
	 * notice whether a NAND chip is enabled at probe time.
	 *
	 * H3 support NAND-boot, with a dip switch to put NOR on CS2B and NAND
	 * (which on H2 may be 16bit) on CS3.  Try detecting that in code here,
	 * to avoid probing every possible flash configuration...
	 */
	nor_resource.end = nor_resource.start = omap_cs3_phys();
	nor_resource.end += SZ_32M - 1;

	nand_resource.end = nand_resource.start = OMAP_CS2B_PHYS;
	nand_resource.end += SZ_4K - 1;
	if (!(omap_request_gpio(H3_NAND_RB_GPIO_PIN)))
		nand_data.dev_ready = nand_dev_ready;

	/* GPIO10 Func_MUX_CTRL reg bit 29:27, Configure V2 to mode1 as GPIO */
	/* GPIO10 pullup/down register, Enable pullup on GPIO10 */
	omap_cfg_reg(V2_1710_GPIO10);

	platform_add_devices(devices, ARRAY_SIZE(devices));
	omap_board_config = h3_config;
	omap_board_config_size = ARRAY_SIZE(h3_config);
	omap_serial_init();
}

static void __init h3_init_smc91x(void)
{
	omap_cfg_reg(W15_1710_GPIO40);
	if (omap_request_gpio(40) < 0) {
		printk("Error requesting gpio 40 for smc91x irq\n");
		return;
	}
}

static void __init h3_init_irq(void)
{
	omap1_init_common_hw();
	omap_init_irq();
	omap_gpio_init();
	h3_init_smc91x();
}

static void __init h3_map_io(void)
{
	omap1_map_common_io();
}

MACHINE_START(OMAP_H3, "TI OMAP1710 H3 board")
	/* Maintainer: Texas Instruments, Inc. */
	.phys_io	= 0xfff00000,
	.io_pg_offst	= ((0xfef00000) >> 18) & 0xfffc,
	.boot_params	= 0x10000100,
	.map_io		= h3_map_io,
	.init_irq	= h3_init_irq,
	.init_machine	= h3_init,
	.timer		= &omap_timer,
MACHINE_END
