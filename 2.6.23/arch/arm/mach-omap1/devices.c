/*
 * linux/arch/arm/mach-omap1/devices.c
 *
 * OMAP1 platform device setup/initialization
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>

#include <asm/hardware.h>
#include <asm/io.h>
#include <asm/mach-types.h>
#include <asm/mach/map.h>

#include <asm/arch/tc.h>
#include <asm/arch/board.h>
#include <asm/arch/mux.h>
#include <asm/arch/gpio.h>

/*-------------------------------------------------------------------------*/

#if defined(CONFIG_RTC_DRV_OMAP) || defined(CONFIG_RTC_DRV_OMAP_MODULE)

#define	OMAP_RTC_BASE		0xfffb4800

static struct resource rtc_resources[] = {
	{
		.start		= OMAP_RTC_BASE,
		.end		= OMAP_RTC_BASE + 0x5f,
		.flags		= IORESOURCE_MEM,
	},
	{
		.start		= INT_RTC_TIMER,
		.flags		= IORESOURCE_IRQ,
	},
	{
		.start		= INT_RTC_ALARM,
		.flags		= IORESOURCE_IRQ,
	},
};

static struct platform_device omap_rtc_device = {
	.name           = "omap_rtc",
	.id             = -1,
	.num_resources	= ARRAY_SIZE(rtc_resources),
	.resource	= rtc_resources,
};

static void omap_init_rtc(void)
{
	(void) platform_device_register(&omap_rtc_device);
}
#else
static inline void omap_init_rtc(void) {}
#endif

#if defined(CONFIG_OMAP_DSP) || defined(CONFIG_OMAP_DSP_MODULE)

#if defined(CONFIG_ARCH_OMAP15XX)
#  define OMAP1_MBOX_SIZE	0x23
#  define INT_DSP_MAILBOX1	INT_1510_DSP_MAILBOX1
#elif defined(CONFIG_ARCH_OMAP16XX)
#  define OMAP1_MBOX_SIZE	0x2f
#  define INT_DSP_MAILBOX1	INT_1610_DSP_MAILBOX1
#endif

#define OMAP1_MBOX_BASE		IO_ADDRESS(OMAP16XX_MAILBOX_BASE)

static struct resource mbox_resources[] = {
	{
		.start		= OMAP1_MBOX_BASE,
		.end		= OMAP1_MBOX_BASE + OMAP1_MBOX_SIZE,
		.flags		= IORESOURCE_MEM,
	},
	{
		.start		= INT_DSP_MAILBOX1,
		.flags		= IORESOURCE_IRQ,
	},
};

static struct platform_device mbox_device = {
	.name		= "mailbox",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(mbox_resources),
	.resource	= mbox_resources,
};

static inline void omap_init_mbox(void)
{
	platform_device_register(&mbox_device);
}
#else
static inline void omap_init_mbox(void) { }
#endif

#if defined(CONFIG_OMAP_STI)

#define OMAP1_STI_BASE		IO_ADDRESS(0xfffea000)
#define OMAP1_STI_CHANNEL_BASE	(OMAP1_STI_BASE + 0x400)

static struct resource sti_resources[] = {
	{
		.start		= OMAP1_STI_BASE,
		.end		= OMAP1_STI_BASE + SZ_1K - 1,
		.flags		= IORESOURCE_MEM,
	},
	{
		.start		= OMAP1_STI_CHANNEL_BASE,
		.end		= OMAP1_STI_CHANNEL_BASE + SZ_1K - 1,
		.flags		= IORESOURCE_MEM,
	},
	{
		.start		= INT_1610_STI,
		.flags		= IORESOURCE_IRQ,
	}
};

static struct platform_device sti_device = {
	.name		= "sti",
	.id		= -1,
	.num_resources	= ARRAY_SIZE(sti_resources),
	.resource	= sti_resources,
};

static inline void omap_init_sti(void)
{
	platform_device_register(&sti_device);
}
#else
static inline void omap_init_sti(void) {}
#endif

/*-------------------------------------------------------------------------*/

/*
 * This gets called after board-specific INIT_MACHINE, and initializes most
 * on-chip peripherals accessible on this board (except for few like USB):
 *
 *  (a) Does any "standard config" pin muxing needed.  Board-specific
 *	code will have muxed GPIO pins and done "nonstandard" setup;
 *	that code could live in the boot loader.
 *  (b) Populating board-specific platform_data with the data drivers
 *	rely on to handle wiring variations.
 *  (c) Creating platform devices as meaningful on this board and
 *	with this kernel configuration.
 *
 * Claiming GPIOs, and setting their direction and initial values, is the
 * responsibility of the device drivers.  So is responding to probe().
 *
 * Board-specific knowlege like creating devices or pin setup is to be
 * kept out of drivers as much as possible.  In particular, pin setup
 * may be handled by the boot loader, and drivers should expect it will
 * normally have been done by the time they're probed.
 */
static int __init omap1_init_devices(void)
{
	/* please keep these calls, and their implementations above,
	 * in alphabetical order so they're easier to sort through.
	 */

	omap_init_mbox();
	omap_init_rtc();
	omap_init_sti();

	return 0;
}
arch_initcall(omap1_init_devices);

