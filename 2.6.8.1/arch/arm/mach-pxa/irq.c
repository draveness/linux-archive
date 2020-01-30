/*
 *  linux/arch/arm/mach-pxa/irq.c
 *
 *  Generic PXA IRQ handling, GPIO IRQ demultiplexing, etc.
 *
 *  Author:	Nicolas Pitre
 *  Created:	Jun 15, 2001
 *  Copyright:	MontaVista Software Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>

#include <asm/hardware.h>
#include <asm/irq.h>
#include <asm/mach/irq.h>

#include "generic.h"


/*
 * This is for IRQs known as PXA_IRQ([8...31]).
 */

static void pxa_mask_irq(unsigned int irq)
{
	ICMR &= ~(1 << (irq + PXA_IRQ_SKIP));
}

static void pxa_unmask_irq(unsigned int irq)
{
	ICMR |= (1 << (irq + PXA_IRQ_SKIP));
}

static struct irqchip pxa_internal_chip = {
	.ack		= pxa_mask_irq,
	.mask		= pxa_mask_irq,
	.unmask		= pxa_unmask_irq,
};

/*
 * PXA GPIO edge detection for IRQs:
 * IRQs are generated on Falling-Edge, Rising-Edge, or both.
 * Use this instead of directly setting GRER/GFER.
 */

static long GPIO_IRQ_rising_edge[3];
static long GPIO_IRQ_falling_edge[3];
static long GPIO_IRQ_mask[3];

static int pxa_gpio_irq_type(unsigned int irq, unsigned int type)
{
	int gpio, idx;

	gpio = IRQ_TO_GPIO(irq);
	idx = gpio >> 5;

	if (type == IRQT_PROBE) {
	    /* Don't mess with enabled GPIOs using preconfigured edges or
	       GPIOs set to alternate function during probe */
		if ((GPIO_IRQ_rising_edge[idx] | GPIO_IRQ_falling_edge[idx]) &
		    GPIO_bit(gpio))
			return 0;
		if (GAFR(gpio) & (0x3 << (((gpio) & 0xf)*2)))
			return 0;
		type = __IRQT_RISEDGE | __IRQT_FALEDGE;
	}

	printk(KERN_DEBUG "IRQ%d (GPIO%d): ", irq, gpio);

	pxa_gpio_mode(gpio | GPIO_IN);

	if (type & __IRQT_RISEDGE) {
		printk("rising ");
		__set_bit (gpio, GPIO_IRQ_rising_edge);
	} else
		__clear_bit (gpio, GPIO_IRQ_rising_edge);

	if (type & __IRQT_FALEDGE) {
		printk("falling ");
		__set_bit (gpio, GPIO_IRQ_falling_edge);
	} else
		__clear_bit (gpio, GPIO_IRQ_falling_edge);

	printk("edges\n");

	GRER(gpio) = GPIO_IRQ_rising_edge[idx] & GPIO_IRQ_mask[idx];
	GFER(gpio) = GPIO_IRQ_falling_edge[idx] & GPIO_IRQ_mask[idx];
	return 0;
}

/*
 * GPIO IRQs must be acknowledged.  This is for GPIO 0 and 1.
 */

static void pxa_ack_low_gpio(unsigned int irq)
{
	GEDR0 = (1 << (irq - IRQ_GPIO0));
}

static struct irqchip pxa_low_gpio_chip = {
	.ack		= pxa_ack_low_gpio,
	.mask		= pxa_mask_irq,
	.unmask		= pxa_unmask_irq,
	.type		= pxa_gpio_irq_type,
};

/*
 * Demux handler for GPIO 2-80 edge detect interrupts
 */

static void pxa_gpio_demux_handler(unsigned int irq, struct irqdesc *desc,
				   struct pt_regs *regs)
{
	unsigned int mask;
	int loop;

	do {
		loop = 0;

		mask = GEDR0 & ~3;
		if (mask) {
			GEDR0 = mask;
			irq = IRQ_GPIO(2);
			desc = irq_desc + irq;
			mask >>= 2;
			do {
				if (mask & 1)
					desc->handle(irq, desc, regs);
				irq++;
				desc++;
				mask >>= 1;
			} while (mask);
			loop = 1;
		}

		mask = GEDR1;
		if (mask) {
			GEDR1 = mask;
			irq = IRQ_GPIO(32);
			desc = irq_desc + irq;
			do {
				if (mask & 1)
					desc->handle(irq, desc, regs);
				irq++;
				desc++;
				mask >>= 1;
			} while (mask);
			loop = 1;
		}

		mask = GEDR2;
		if (mask) {
			GEDR2 = mask;
			irq = IRQ_GPIO(64);
			desc = irq_desc + irq;
			do {
				if (mask & 1)
					desc->handle(irq, desc, regs);
				irq++;
				desc++;
				mask >>= 1;
			} while (mask);
			loop = 1;
		}
	} while (loop);
}

static void pxa_ack_muxed_gpio(unsigned int irq)
{
	int gpio = irq - IRQ_GPIO(2) + 2;
	GEDR(gpio) = GPIO_bit(gpio);
}

static void pxa_mask_muxed_gpio(unsigned int irq)
{
	int gpio = irq - IRQ_GPIO(2) + 2;
	__clear_bit(gpio, GPIO_IRQ_mask);
	GRER(gpio) &= ~GPIO_bit(gpio);
	GFER(gpio) &= ~GPIO_bit(gpio);
}

static void pxa_unmask_muxed_gpio(unsigned int irq)
{
	int gpio = irq - IRQ_GPIO(2) + 2;
	int idx = gpio >> 5;
	__set_bit(gpio, GPIO_IRQ_mask);
	GRER(gpio) = GPIO_IRQ_rising_edge[idx] & GPIO_IRQ_mask[idx];
	GFER(gpio) = GPIO_IRQ_falling_edge[idx] & GPIO_IRQ_mask[idx];
}

static struct irqchip pxa_muxed_gpio_chip = {
	.ack		= pxa_ack_muxed_gpio,
	.mask		= pxa_mask_muxed_gpio,
	.unmask		= pxa_unmask_muxed_gpio,
	.type		= pxa_gpio_irq_type,
};


void __init pxa_init_irq(void)
{
	int irq;

	/* disable all IRQs */
	ICMR = 0;

	/* all IRQs are IRQ, not FIQ */
	ICLR = 0;

	/* clear all GPIO edge detects */
	GFER0 = GFER1 = GFER2 = 0;
	GRER0 = GRER1 = GRER2 = 0;
	GEDR0 = GEDR0;
	GEDR1 = GEDR1;
	GEDR2 = GEDR2;

	/* only unmasked interrupts kick us out of idle */
	ICCR = 1;

	/* GPIO 0 and 1 must have their mask bit always set */
	GPIO_IRQ_mask[0] = 3;

	for (irq = PXA_IRQ(PXA_IRQ_SKIP); irq <= PXA_IRQ(31); irq++) {
		set_irq_chip(irq, &pxa_internal_chip);
		set_irq_handler(irq, do_level_IRQ);
		set_irq_flags(irq, IRQF_VALID);
	}

	for (irq = IRQ_GPIO0; irq <= IRQ_GPIO1; irq++) {
		set_irq_chip(irq, &pxa_low_gpio_chip);
		set_irq_handler(irq, do_edge_IRQ);
		set_irq_flags(irq, IRQF_VALID | IRQF_PROBE);
	}

	for (irq = IRQ_GPIO(2); irq <= IRQ_GPIO(80); irq++) {
		set_irq_chip(irq, &pxa_muxed_gpio_chip);
		set_irq_handler(irq, do_edge_IRQ);
		set_irq_flags(irq, IRQF_VALID | IRQF_PROBE);
	}

	/* Install handler for GPIO 2-80 edge detect interrupts */
	set_irq_chip(IRQ_GPIO_2_80, &pxa_internal_chip);
	set_irq_chained_handler(IRQ_GPIO_2_80, pxa_gpio_demux_handler);
}
