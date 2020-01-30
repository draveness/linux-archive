/*
 * linux/arch/arm/mach-pxa/lpd270.c
 *
 * Support for the LogicPD PXA270 Card Engine.
 * Derived from the mainstone code, which carries these notices:
 *
 * Author:	Nicolas Pitre
 * Created:	Nov 05, 2002
 * Copyright:	MontaVista Software Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/sysdev.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/bitops.h>
#include <linux/fb.h>
#include <linux/ioport.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>

#include <asm/types.h>
#include <asm/setup.h>
#include <asm/memory.h>
#include <asm/mach-types.h>
#include <asm/hardware.h>
#include <asm/irq.h>
#include <asm/sizes.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/irq.h>
#include <asm/mach/flash.h>

#include <asm/arch/pxa-regs.h>
#include <asm/arch/lpd270.h>
#include <asm/arch/audio.h>
#include <asm/arch/pxafb.h>
#include <asm/arch/mmc.h>
#include <asm/arch/irda.h>
#include <asm/arch/ohci.h>

#include "generic.h"
#include "devices.h"


static unsigned int lpd270_irq_enabled;

static void lpd270_mask_irq(unsigned int irq)
{
	int lpd270_irq = irq - LPD270_IRQ(0);

	__raw_writew(~(1 << lpd270_irq), LPD270_INT_STATUS);

	lpd270_irq_enabled &= ~(1 << lpd270_irq);
	__raw_writew(lpd270_irq_enabled, LPD270_INT_MASK);
}

static void lpd270_unmask_irq(unsigned int irq)
{
	int lpd270_irq = irq - LPD270_IRQ(0);

	lpd270_irq_enabled |= 1 << lpd270_irq;
	__raw_writew(lpd270_irq_enabled, LPD270_INT_MASK);
}

static struct irq_chip lpd270_irq_chip = {
	.name		= "CPLD",
	.ack		= lpd270_mask_irq,
	.mask		= lpd270_mask_irq,
	.unmask		= lpd270_unmask_irq,
};

static void lpd270_irq_handler(unsigned int irq, struct irq_desc *desc)
{
	unsigned long pending;

	pending = __raw_readw(LPD270_INT_STATUS) & lpd270_irq_enabled;
	do {
		GEDR(0) = GPIO_bit(0);  /* clear useless edge notification */
		if (likely(pending)) {
			irq = LPD270_IRQ(0) + __ffs(pending);
			desc = irq_desc + irq;
			desc_handle_irq(irq, desc);

			pending = __raw_readw(LPD270_INT_STATUS) &
						lpd270_irq_enabled;
		}
	} while (pending);
}

static void __init lpd270_init_irq(void)
{
	int irq;

	pxa27x_init_irq();

	__raw_writew(0, LPD270_INT_MASK);
	__raw_writew(0, LPD270_INT_STATUS);

	/* setup extra LogicPD PXA270 irqs */
	for (irq = LPD270_IRQ(2); irq <= LPD270_IRQ(4); irq++) {
		set_irq_chip(irq, &lpd270_irq_chip);
		set_irq_handler(irq, handle_level_irq);
		set_irq_flags(irq, IRQF_VALID | IRQF_PROBE);
	}
	set_irq_chained_handler(IRQ_GPIO(0), lpd270_irq_handler);
	set_irq_type(IRQ_GPIO(0), IRQT_FALLING);
}


#ifdef CONFIG_PM
static int lpd270_irq_resume(struct sys_device *dev)
{
	__raw_writew(lpd270_irq_enabled, LPD270_INT_MASK);
	return 0;
}

static struct sysdev_class lpd270_irq_sysclass = {
	set_kset_name("cpld_irq"),
	.resume = lpd270_irq_resume,
};

static struct sys_device lpd270_irq_device = {
	.cls = &lpd270_irq_sysclass,
};

static int __init lpd270_irq_device_init(void)
{
	int ret = sysdev_class_register(&lpd270_irq_sysclass);
	if (ret == 0)
		ret = sysdev_register(&lpd270_irq_device);
	return ret;
}

device_initcall(lpd270_irq_device_init);
#endif


static struct resource smc91x_resources[] = {
	[0] = {
		.start	= LPD270_ETH_PHYS,
		.end	= (LPD270_ETH_PHYS + 0xfffff),
		.flags	= IORESOURCE_MEM,
	},
	[1] = {
		.start	= LPD270_ETHERNET_IRQ,
		.end	= LPD270_ETHERNET_IRQ,
		.flags	= IORESOURCE_IRQ,
	},
};

static struct platform_device smc91x_device = {
	.name		= "smc91x",
	.id		= 0,
	.num_resources	= ARRAY_SIZE(smc91x_resources),
	.resource	= smc91x_resources,
};

static struct platform_device lpd270_audio_device = {
	.name		= "pxa2xx-ac97",
	.id		= -1,
};

static struct resource lpd270_flash_resources[] = {
	[0] = {
		.start	= PXA_CS0_PHYS,
		.end	= PXA_CS0_PHYS + SZ_64M - 1,
		.flags	= IORESOURCE_MEM,
	},
	[1] = {
		.start	= PXA_CS1_PHYS,
		.end	= PXA_CS1_PHYS + SZ_64M - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct mtd_partition lpd270_flash0_partitions[] = {
	{
		.name =		"Bootloader",
		.size =		0x00040000,
		.offset =	0,
		.mask_flags =	MTD_WRITEABLE  /* force read-only */
	}, {
		.name =		"Kernel",
		.size =		0x00400000,
		.offset =	0x00040000,
	}, {
		.name =		"Filesystem",
		.size =		MTDPART_SIZ_FULL,
		.offset =	0x00440000
	},
};

static struct flash_platform_data lpd270_flash_data[2] = {
	{
		.name		= "processor-flash",
		.map_name	= "cfi_probe",
		.parts		= lpd270_flash0_partitions,
		.nr_parts	= ARRAY_SIZE(lpd270_flash0_partitions),
	}, {
		.name		= "mainboard-flash",
		.map_name	= "cfi_probe",
		.parts		= NULL,
		.nr_parts	= 0,
	}
};

static struct platform_device lpd270_flash_device[2] = {
	{
		.name		= "pxa2xx-flash",
		.id		= 0,
		.dev = {
			.platform_data	= &lpd270_flash_data[0],
		},
		.resource	= &lpd270_flash_resources[0],
		.num_resources	= 1,
	}, {
		.name		= "pxa2xx-flash",
		.id		= 1,
		.dev = {
			.platform_data	= &lpd270_flash_data[1],
		},
		.resource	= &lpd270_flash_resources[1],
		.num_resources	= 1,
	},
};

static void lpd270_backlight_power(int on)
{
	if (on) {
		pxa_gpio_mode(GPIO16_PWM0_MD);
		pxa_set_cken(CKEN_PWM0, 1);
		PWM_CTRL0 = 0;
		PWM_PWDUTY0 = 0x3ff;
		PWM_PERVAL0 = 0x3ff;
	} else {
		PWM_CTRL0 = 0;
		PWM_PWDUTY0 = 0x0;
		PWM_PERVAL0 = 0x3FF;
		pxa_set_cken(CKEN_PWM0, 0);
	}
}

/* 5.7" TFT QVGA (LoLo display number 1) */
static struct pxafb_mode_info sharp_lq057q3dc02_mode = {
	.pixclock		= 150000,
	.xres			= 320,
	.yres			= 240,
	.bpp			= 16,
	.hsync_len		= 0x14,
	.left_margin		= 0x28,
	.right_margin		= 0x0a,
	.vsync_len		= 0x02,
	.upper_margin		= 0x08,
	.lower_margin		= 0x14,
	.sync			= FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
};

static struct pxafb_mach_info sharp_lq057q3dc02 = {
	.modes			= &sharp_lq057q3dc02_mode,
	.num_modes		= 1,
	.lccr0			= 0x07800080,
	.lccr3			= 0x00400000,
	.pxafb_backlight_power	= lpd270_backlight_power,
};

/* 12.1" TFT SVGA (LoLo display number 2) */
static struct pxafb_mode_info sharp_lq121s1dg31_mode = {
	.pixclock		= 50000,
	.xres			= 800,
	.yres			= 600,
	.bpp			= 16,
	.hsync_len		= 0x05,
	.left_margin		= 0x52,
	.right_margin		= 0x05,
	.vsync_len		= 0x04,
	.upper_margin		= 0x14,
	.lower_margin		= 0x0a,
	.sync			= FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
};

static struct pxafb_mach_info sharp_lq121s1dg31 = {
	.modes			= &sharp_lq121s1dg31_mode,
	.num_modes		= 1,
	.lccr0			= 0x07800080,
	.lccr3			= 0x00400000,
	.pxafb_backlight_power	= lpd270_backlight_power,
};

/* 3.6" TFT QVGA (LoLo display number 3) */
static struct pxafb_mode_info sharp_lq036q1da01_mode = {
	.pixclock		= 150000,
	.xres			= 320,
	.yres			= 240,
	.bpp			= 16,
	.hsync_len		= 0x0e,
	.left_margin		= 0x04,
	.right_margin		= 0x0a,
	.vsync_len		= 0x03,
	.upper_margin		= 0x03,
	.lower_margin		= 0x03,
	.sync			= FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
};

static struct pxafb_mach_info sharp_lq036q1da01 = {
	.modes			= &sharp_lq036q1da01_mode,
	.num_modes		= 1,
	.lccr0			= 0x07800080,
	.lccr3			= 0x00400000,
	.pxafb_backlight_power	= lpd270_backlight_power,
};

/* 6.4" TFT VGA (LoLo display number 5) */
static struct pxafb_mode_info sharp_lq64d343_mode = {
	.pixclock		= 25000,
	.xres			= 640,
	.yres			= 480,
	.bpp			= 16,
	.hsync_len		= 0x31,
	.left_margin		= 0x89,
	.right_margin		= 0x19,
	.vsync_len		= 0x12,
	.upper_margin		= 0x22,
	.lower_margin		= 0x00,
	.sync			= FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
};

static struct pxafb_mach_info sharp_lq64d343 = {
	.modes			= &sharp_lq64d343_mode,
	.num_modes		= 1,
	.lccr0			= 0x07800080,
	.lccr3			= 0x00400000,
	.pxafb_backlight_power	= lpd270_backlight_power,
};

/* 10.4" TFT VGA (LoLo display number 7) */
static struct pxafb_mode_info sharp_lq10d368_mode = {
	.pixclock		= 25000,
	.xres			= 640,
	.yres			= 480,
	.bpp			= 16,
	.hsync_len		= 0x31,
	.left_margin		= 0x89,
	.right_margin		= 0x19,
	.vsync_len		= 0x12,
	.upper_margin		= 0x22,
	.lower_margin		= 0x00,
	.sync			= FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
};

static struct pxafb_mach_info sharp_lq10d368 = {
	.modes			= &sharp_lq10d368_mode,
	.num_modes		= 1,
	.lccr0			= 0x07800080,
	.lccr3			= 0x00400000,
	.pxafb_backlight_power	= lpd270_backlight_power,
};

/* 3.5" TFT QVGA (LoLo display number 8) */
static struct pxafb_mode_info sharp_lq035q7db02_20_mode = {
	.pixclock		= 150000,
	.xres			= 240,
	.yres			= 320,
	.bpp			= 16,
	.hsync_len		= 0x0e,
	.left_margin		= 0x0a,
	.right_margin		= 0x0a,
	.vsync_len		= 0x03,
	.upper_margin		= 0x05,
	.lower_margin		= 0x14,
	.sync			= FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
};

static struct pxafb_mach_info sharp_lq035q7db02_20 = {
	.modes			= &sharp_lq035q7db02_20_mode,
	.num_modes		= 1,
	.lccr0			= 0x07800080,
	.lccr3			= 0x00400000,
	.pxafb_backlight_power	= lpd270_backlight_power,
};

static struct pxafb_mach_info *lpd270_lcd_to_use;

static int __init lpd270_set_lcd(char *str)
{
	if (!strnicmp(str, "lq057q3dc02", 11)) {
		lpd270_lcd_to_use = &sharp_lq057q3dc02;
	} else if (!strnicmp(str, "lq121s1dg31", 11)) {
		lpd270_lcd_to_use = &sharp_lq121s1dg31;
	} else if (!strnicmp(str, "lq036q1da01", 11)) {
		lpd270_lcd_to_use = &sharp_lq036q1da01;
	} else if (!strnicmp(str, "lq64d343", 8)) {
		lpd270_lcd_to_use = &sharp_lq64d343;
	} else if (!strnicmp(str, "lq10d368", 8)) {
		lpd270_lcd_to_use = &sharp_lq10d368;
	} else if (!strnicmp(str, "lq035q7db02-20", 14)) {
		lpd270_lcd_to_use = &sharp_lq035q7db02_20;
	} else {
		printk(KERN_INFO "lpd270: unknown lcd panel [%s]\n", str);
	}

	return 1;
}

__setup("lcd=", lpd270_set_lcd);

static struct platform_device *platform_devices[] __initdata = {
	&smc91x_device,
	&lpd270_audio_device,
	&lpd270_flash_device[0],
	&lpd270_flash_device[1],
};

static int lpd270_ohci_init(struct device *dev)
{
	/* setup Port1 GPIO pin. */
	pxa_gpio_mode(88 | GPIO_ALT_FN_1_IN);	/* USBHPWR1 */
	pxa_gpio_mode(89 | GPIO_ALT_FN_2_OUT);	/* USBHPEN1 */

	/* Set the Power Control Polarity Low and Power Sense
	   Polarity Low to active low. */
	UHCHR = (UHCHR | UHCHR_PCPL | UHCHR_PSPL) &
		~(UHCHR_SSEP1 | UHCHR_SSEP2 | UHCHR_SSEP3 | UHCHR_SSE);

	return 0;
}

static struct pxaohci_platform_data lpd270_ohci_platform_data = {
	.port_mode	= PMM_PERPORT_MODE,
	.init		= lpd270_ohci_init,
};

static void __init lpd270_init(void)
{
	lpd270_flash_data[0].width = (BOOT_DEF & 1) ? 2 : 4;
	lpd270_flash_data[1].width = 4;

	/*
	 * System bus arbiter setting:
	 * - Core_Park
	 * - LCD_wt:DMA_wt:CORE_Wt = 2:3:4
	 */
	ARB_CNTRL = ARB_CORE_PARK | 0x234;

	/*
	 * On LogicPD PXA270, we route AC97_SYSCLK via GPIO45.
	 */
	pxa_gpio_mode(GPIO45_SYSCLK_AC97_MD);

	platform_add_devices(platform_devices, ARRAY_SIZE(platform_devices));

	if (lpd270_lcd_to_use != NULL)
		set_pxa_fb_info(lpd270_lcd_to_use);

	pxa_set_ohci_info(&lpd270_ohci_platform_data);
}


static struct map_desc lpd270_io_desc[] __initdata = {
	{
		.virtual	= LPD270_CPLD_VIRT,
		.pfn		= __phys_to_pfn(LPD270_CPLD_PHYS),
		.length		= LPD270_CPLD_SIZE,
		.type		= MT_DEVICE,
	},
};

static void __init lpd270_map_io(void)
{
	pxa_map_io();
	iotable_init(lpd270_io_desc, ARRAY_SIZE(lpd270_io_desc));

	/* initialize sleep mode regs (wake-up sources, etc) */
	PGSR0 = 0x00008800;
	PGSR1 = 0x00000002;
	PGSR2 = 0x0001FC00;
	PGSR3 = 0x00001F81;
	PWER  = 0xC0000002;
	PRER  = 0x00000002;
	PFER  = 0x00000002;

	/* for use I SRAM as framebuffer.  */
	PSLR |= 0x00000F04;
	PCFR  = 0x00000066;
}

MACHINE_START(LOGICPD_PXA270, "LogicPD PXA270 Card Engine")
	/* Maintainer: Peter Barada */
	.phys_io	= 0x40000000,
	.io_pg_offst	= (io_p2v(0x40000000) >> 18) & 0xfffc,
	.boot_params	= 0xa0000100,
	.map_io		= lpd270_map_io,
	.init_irq	= lpd270_init_irq,
	.timer		= &pxa_timer,
	.init_machine	= lpd270_init,
MACHINE_END
