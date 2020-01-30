/*
 *  linux/arch/arm/mach-pxa/mainstone.c
 *
 *  Support for the Intel HCDDBBVA0 Development Platform.
 *  (go figure how they came up with such name...)
 *
 *  Author:	Nicolas Pitre
 *  Created:	Nov 05, 2002
 *  Copyright:	MontaVista Software Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/bitops.h>
#include <linux/fb.h>

#include <asm/types.h>
#include <asm/setup.h>
#include <asm/memory.h>
#include <asm/mach-types.h>
#include <asm/hardware.h>
#include <asm/irq.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/irq.h>

#include <asm/arch/mainstone.h>
#include <asm/arch/pxafb.h>

#include "generic.h"


static unsigned long mainstone_irq_enabled;

static void mainstone_mask_irq(unsigned int irq)
{
	int mainstone_irq = (irq - MAINSTONE_IRQ(0));
	MST_INTMSKENA = (mainstone_irq_enabled &= ~(1 << mainstone_irq));
}

static void mainstone_unmask_irq(unsigned int irq)
{
	int mainstone_irq = (irq - MAINSTONE_IRQ(0));
	/* the irq can be acknowledged only if deasserted, so it's done here */
	MST_INTSETCLR &= ~(1 << mainstone_irq);
	MST_INTMSKENA = (mainstone_irq_enabled |= (1 << mainstone_irq));
}

static struct irqchip mainstone_irq_chip = {
	.ack		= mainstone_mask_irq,
	.mask		= mainstone_mask_irq,
	.unmask		= mainstone_unmask_irq,
};


static void mainstone_irq_handler(unsigned int irq, struct irqdesc *desc,
				  struct pt_regs *regs)
{
	unsigned long pending = MST_INTSETCLR & mainstone_irq_enabled;
	do {
		GEDR(0) = GPIO_bit(0);  /* clear useless edge notification */
		if (likely(pending)) {
			irq = MAINSTONE_IRQ(0) + __ffs(pending);
			desc = irq_desc + irq;
			desc->handle(irq, desc, regs);
		}
		pending = MST_INTSETCLR & mainstone_irq_enabled;
	} while (pending);
}

static void __init mainstone_init_irq(void)
{
	int irq;

	pxa_init_irq();

	/* setup extra Mainstone irqs */
	for(irq = MAINSTONE_IRQ(0); irq <= MAINSTONE_IRQ(15); irq++) {
		set_irq_chip(irq, &mainstone_irq_chip);
		set_irq_handler(irq, do_level_IRQ);
		set_irq_flags(irq, IRQF_VALID | IRQF_PROBE);
	}
	set_irq_flags(MAINSTONE_IRQ(8), 0);
	set_irq_flags(MAINSTONE_IRQ(12), 0);

	MST_INTMSKENA = 0;
	MST_INTSETCLR = 0;

	set_irq_chained_handler(IRQ_GPIO(0), mainstone_irq_handler);
	set_irq_type(IRQ_GPIO(0), IRQT_FALLING);
}


static struct resource smc91x_resources[] = {
	[0] = {
		.start	= (MST_ETH_PHYS + 0x300),
		.end	= (MST_ETH_PHYS + 0xfffff),
		.flags	= IORESOURCE_MEM,
	},
	[1] = {
		.start	= MAINSTONE_IRQ(3),
		.end	= MAINSTONE_IRQ(3),
		.flags	= IORESOURCE_IRQ,
	}
};

static struct platform_device smc91x_device = {
	.name		= "smc91x",
	.id		= 0,
	.num_resources	= ARRAY_SIZE(smc91x_resources),
	.resource	= smc91x_resources,
};


static void mainstone_backlight_power(int on)
{
	if (on) {
		pxa_gpio_mode(GPIO16_PWM0_MD);
		pxa_set_cken(CKEN0_PWM0, 1);
		PWM_CTRL0 = 0;
		PWM_PWDUTY0 = 0x3ff;
		PWM_PERVAL0 = 0x3ff;
	} else {
		PWM_CTRL0 = 0;
		PWM_PWDUTY0 = 0x0;
		PWM_PERVAL0 = 0x3FF;
		pxa_set_cken(CKEN0_PWM0, 0);
	}
}

static struct pxafb_mach_info toshiba_ltm04c380k __initdata = {
	.pixclock		= 50000,
	.xres			= 640,
	.yres			= 480,
	.bpp			= 16,
	.hsync_len		= 1,
	.left_margin		= 0x9f,
	.right_margin		= 1,
	.vsync_len		= 44,
	.upper_margin		= 0,
	.lower_margin		= 0,
	.sync			= FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT,
	.lccr0			= LCCR0_Act,
	.lccr3			= LCCR3_PCP,
	.pxafb_backlight_power	= mainstone_backlight_power,
};

static struct pxafb_mach_info toshiba_ltm035a776c __initdata = {
	.pixclock		= 110000,
	.xres			= 240,
	.yres			= 320,
	.bpp			= 16,
	.hsync_len		= 4,
	.left_margin		= 8,
	.right_margin		= 20,
	.vsync_len		= 3,
	.upper_margin		= 1,
	.lower_margin		= 10,
	.sync			= FB_SYNC_HOR_HIGH_ACT|FB_SYNC_VERT_HIGH_ACT,
	.lccr0			= LCCR0_Act,
	.lccr3			= LCCR3_PCP,
	.pxafb_backlight_power	= mainstone_backlight_power,
};

static void __init mainstone_init(void)
{
	platform_device_register(&smc91x_device);

	/* reading Mainstone's "Virtual Configuration Register"
	   might be handy to select LCD type here */
	if (0)
		set_pxa_fb_info(&toshiba_ltm04c380k);
	else
		set_pxa_fb_info(&toshiba_ltm035a776c);
}


static struct map_desc mainstone_io_desc[] __initdata = {
  { MST_FPGA_VIRT, MST_FPGA_PHYS, 0x00100000, MT_DEVICE }, /* CPLD */
};

static void __init mainstone_map_io(void)
{
	pxa_map_io();
	iotable_init(mainstone_io_desc, ARRAY_SIZE(mainstone_io_desc));
}

MACHINE_START(MAINSTONE, "Intel HCDDBBVA0 Development Platform (aka Mainstone)")
	MAINTAINER("MontaVista Software Inc.")
	BOOT_MEM(0xa0000000, 0x40000000, io_p2v(0x40000000))
	MAPIO(mainstone_map_io)
	INITIRQ(mainstone_init_irq)
	INITTIME(pxa_init_time)
	INIT_MACHINE(mainstone_init)
MACHINE_END
