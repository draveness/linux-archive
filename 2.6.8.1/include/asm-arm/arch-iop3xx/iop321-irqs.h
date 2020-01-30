/*
 * linux/include/asm-arm/arch-iop3xx/irqs.h
 *
 * Author:	Rory Bolt <rorybolt@pacbell.net>
 * Copyright:	(C) 2002 Rory Bolt
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/config.h>

/*
 * IOP80321 chipset interrupts
 */
#define IOP321_IRQ_OFS		0
#define IOP321_IRQ(x)		(IOP321_IRQ_OFS + (x))

/*
 * On IRQ or FIQ register
 */
#define IRQ_IOP321_DMA0_EOT	IOP321_IRQ(0)
#define IRQ_IOP321_DMA0_EOC	IOP321_IRQ(1)
#define IRQ_IOP321_DMA1_EOT	IOP321_IRQ(2)
#define IRQ_IOP321_DMA1_EOC	IOP321_IRQ(3)
#define IRQ_IOP321_RSVD_4	IOP321_IRQ(4)
#define IRQ_IOP321_RSVD_5	IOP321_IRQ(5)
#define IRQ_IOP321_AA_EOT	IOP321_IRQ(6)
#define IRQ_IOP321_AA_EOC	IOP321_IRQ(7)
#define IRQ_IOP321_CORE_PMON	IOP321_IRQ(8)
#define IRQ_IOP321_TIMER0	IOP321_IRQ(9)
#define IRQ_IOP321_TIMER1	IOP321_IRQ(10)
#define IRQ_IOP321_I2C_0	IOP321_IRQ(11)
#define IRQ_IOP321_I2C_1	IOP321_IRQ(12)
#define IRQ_IOP321_MESSAGING	IOP321_IRQ(13)
#define IRQ_IOP321_ATU_BIST	IOP321_IRQ(14)
#define IRQ_IOP321_PERFMON	IOP321_IRQ(15)
#define IRQ_IOP321_CORE_PMU	IOP321_IRQ(16)
#define IRQ_IOP321_BIU_ERR	IOP321_IRQ(17)
#define IRQ_IOP321_ATU_ERR	IOP321_IRQ(18)
#define IRQ_IOP321_MCU_ERR	IOP321_IRQ(19)
#define IRQ_IOP321_DMA0_ERR	IOP321_IRQ(20)
#define IRQ_IOP321_DMA1_ERR	IOP321_IRQ(21)
#define IRQ_IOP321_RSVD_22	IOP321_IRQ(22)
#define IRQ_IOP321_AA_ERR	IOP321_IRQ(23)
#define IRQ_IOP321_MSG_ERR	IOP321_IRQ(24)
#define IRQ_IOP321_SSP		IOP321_IRQ(25)
#define IRQ_IOP321_RSVD_26	IOP321_IRQ(26)
#define IRQ_IOP321_XINT0	IOP321_IRQ(27)
#define IRQ_IOP321_XINT1	IOP321_IRQ(28)
#define IRQ_IOP321_XINT2	IOP321_IRQ(29)
#define IRQ_IOP321_XINT3	IOP321_IRQ(30)
#define IRQ_IOP321_HPI		IOP321_IRQ(31)

#define NR_IOP321_IRQS		(IOP321_IRQ(31) + 1)

#define NR_IRQS			NR_IOP321_IRQS


/*
 * Interrupts available on the IQ80321 board
 */
#ifdef CONFIG_ARCH_IQ80321

/*
 * On board devices
 */
#define	IRQ_IQ80321_I82544	IRQ_IOP321_XINT0
#define IRQ_IQ80321_UART	IRQ_IOP321_XINT1

/*
 * PCI interrupts
 */
#define	IRQ_IQ80321_INTA	IRQ_IOP321_XINT0
#define	IRQ_IQ80321_INTB	IRQ_IOP321_XINT1
#define	IRQ_IQ80321_INTC	IRQ_IOP321_XINT2
#define	IRQ_IQ80321_INTD	IRQ_IOP321_XINT3

#endif // CONFIG_ARCH_IQ80321

#define XSCALE_PMU_IRQ	IRQ_IOP321_CORE_PMU

