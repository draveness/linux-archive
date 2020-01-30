/*
 * arch/arm/mach-ks8695/gpio.c
 *
 * Copyright (C) 2006 Andrew Victor
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/module.h>

#include <asm/io.h>
#include <asm/hardware.h>
#include <asm/mach/irq.h>

#include <asm/arch/regs-gpio.h>
#include <asm/arch/gpio.h>

/*
 * Configure a GPIO line for either GPIO function, or its internal
 * function (Interrupt, Timer, etc).
 */
static void __init_or_module ks8695_gpio_mode(unsigned int pin, short gpio)
{
	unsigned int enable[] = { IOPC_IOEINT0EN, IOPC_IOEINT1EN, IOPC_IOEINT2EN, IOPC_IOEINT3EN, IOPC_IOTIM0EN, IOPC_IOTIM1EN };
	unsigned long x, flags;

	if (pin > KS8695_GPIO_5)	/* only GPIO 0..5 have internal functions */
		return;

	local_irq_save(flags);

	x = __raw_readl(KS8695_GPIO_VA + KS8695_IOPC);
	if (gpio)			/* GPIO: set bit to 0 */
		x &= ~enable[pin];
	else				/* Internal function: set bit to 1 */
		x |= enable[pin];
	__raw_writel(x, KS8695_GPIO_VA + KS8695_IOPC);

	local_irq_restore(flags);
}


static unsigned short gpio_irq[] = { KS8695_IRQ_EXTERN0, KS8695_IRQ_EXTERN1, KS8695_IRQ_EXTERN2, KS8695_IRQ_EXTERN3 };

/*
 * Configure GPIO pin as external interrupt source.
 */
int __init_or_module ks8695_gpio_interrupt(unsigned int pin, unsigned int type)
{
	unsigned long x, flags;

	if (pin > KS8695_GPIO_3)	/* only GPIO 0..3 can generate IRQ */
		return -EINVAL;

	local_irq_save(flags);

	/* set pin as input */
	x = __raw_readl(KS8695_GPIO_VA + KS8695_IOPM);
	x &= ~IOPM_(pin);
	__raw_writel(x, KS8695_GPIO_VA + KS8695_IOPM);

	local_irq_restore(flags);

	/* Set IRQ triggering type */
	set_irq_type(gpio_irq[pin], type);

	/* enable interrupt mode */
	ks8695_gpio_mode(pin, 0);

	return 0;
}
EXPORT_SYMBOL(ks8695_gpio_interrupt);



/* .... Generic GPIO interface .............................................. */

/*
 * Configure the GPIO line as an input.
 */
int __init_or_module gpio_direction_input(unsigned int pin)
{
	unsigned long x, flags;

	if (pin > KS8695_GPIO_15)
		return -EINVAL;

	/* set pin to GPIO mode */
	ks8695_gpio_mode(pin, 1);

	local_irq_save(flags);

	/* set pin as input */
	x = __raw_readl(KS8695_GPIO_VA + KS8695_IOPM);
	x &= ~IOPM_(pin);
	__raw_writel(x, KS8695_GPIO_VA + KS8695_IOPM);

	local_irq_restore(flags);

	return 0;
}
EXPORT_SYMBOL(gpio_direction_input);


/*
 * Configure the GPIO line as an output, with default state.
 */
int __init_or_module gpio_direction_output(unsigned int pin, unsigned int state)
{
	unsigned long x, flags;

	if (pin > KS8695_GPIO_15)
		return -EINVAL;

	/* set pin to GPIO mode */
	ks8695_gpio_mode(pin, 1);

	local_irq_save(flags);

	/* set line state */
	x = __raw_readl(KS8695_GPIO_VA + KS8695_IOPD);
	if (state)
		x |= (1 << pin);
	else
		x &= ~(1 << pin);
	__raw_writel(x, KS8695_GPIO_VA + KS8695_IOPD);

	/* set pin as output */
	x = __raw_readl(KS8695_GPIO_VA + KS8695_IOPM);
	x |= IOPM_(pin);
	__raw_writel(x, KS8695_GPIO_VA + KS8695_IOPM);

	local_irq_restore(flags);

	return 0;
}
EXPORT_SYMBOL(gpio_direction_output);


/*
 * Set the state of an output GPIO line.
 */
void gpio_set_value(unsigned int pin, unsigned int state)
{
	unsigned long x, flags;

	if (pin > KS8695_GPIO_15)
		return;

	local_irq_save(flags);

	/* set output line state */
	x = __raw_readl(KS8695_GPIO_VA + KS8695_IOPD);
	if (state)
		x |= (1 << pin);
	else
		x &= ~(1 << pin);
	__raw_writel(x, KS8695_GPIO_VA + KS8695_IOPD);

	local_irq_restore(flags);
}
EXPORT_SYMBOL(gpio_set_value);


/*
 * Read the state of a GPIO line.
 */
int gpio_get_value(unsigned int pin)
{
	unsigned long x;

	if (pin > KS8695_GPIO_15)
		return -EINVAL;

	x = __raw_readl(KS8695_GPIO_VA + KS8695_IOPD);
	return (x & (1 << pin)) != 0;
}
EXPORT_SYMBOL(gpio_get_value);


/*
 * Map GPIO line to IRQ number.
 */
int gpio_to_irq(unsigned int pin)
{
	if (pin > KS8695_GPIO_3)	/* only GPIO 0..3 can generate IRQ */
		return -EINVAL;

	return gpio_irq[pin];
}
EXPORT_SYMBOL(gpio_to_irq);


/*
 * Map IRQ number to GPIO line.
 */
int irq_to_gpio(unsigned int irq)
{
	if ((irq < KS8695_IRQ_EXTERN0) || (irq > KS8695_IRQ_EXTERN3))
		return -EINVAL;

	return (irq - KS8695_IRQ_EXTERN0);
}
EXPORT_SYMBOL(irq_to_gpio);
