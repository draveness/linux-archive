/*
 * include/asm-arm/arch-ixp4xx/irqs.h 
 *
 * IRQ definitions for IXP4XX based systems
 *
 * Copyright (C) 2002 Intel Corporation.
 * Copyright (C) 2003 MontaVista Software, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#ifndef _ARCH_IXP4XX_IRQS_H_
#define _ARCH_IXP4XX_IRQS_H_

#define NR_IRQS			32

#define IRQ_IXP4XX_NPEA		0
#define IRQ_IXP4XX_NPEB		1
#define IRQ_IXP4XX_NPEC		2
#define IRQ_IXP4XX_QM1		3
#define IRQ_IXP4XX_QM2		4
#define IRQ_IXP4XX_TIMER1	5
#define IRQ_IXP4XX_GPIO0	6
#define IRQ_IXP4XX_GPIO1	7
#define IRQ_IXP4XX_PCI_INT	8
#define IRQ_IXP4XX_PCI_DMA1	9
#define IRQ_IXP4XX_PCI_DMA2	10
#define IRQ_IXP4XX_TIMER2	11
#define IRQ_IXP4XX_USB		12
#define IRQ_IXP4XX_UART2	13
#define IRQ_IXP4XX_TIMESTAMP	14
#define IRQ_IXP4XX_UART1	15
#define IRQ_IXP4XX_WDOG		16
#define IRQ_IXP4XX_AHB_PMU	17
#define IRQ_IXP4XX_XSCALE_PMU	18
#define IRQ_IXP4XX_GPIO2	19
#define IRQ_IXP4XX_GPIO3	20
#define IRQ_IXP4XX_GPIO4	21
#define IRQ_IXP4XX_GPIO5	22
#define IRQ_IXP4XX_GPIO6	23
#define IRQ_IXP4XX_GPIO7	24
#define IRQ_IXP4XX_GPIO8	25
#define IRQ_IXP4XX_GPIO9	26
#define IRQ_IXP4XX_GPIO10	27
#define IRQ_IXP4XX_GPIO11	28
#define IRQ_IXP4XX_GPIO12	29
#define IRQ_IXP4XX_SW_INT1	30
#define IRQ_IXP4XX_SW_INT2	31

#define	XSCALE_PMU_IRQ		(IRQ_IXP4XX_XSCALE_PMU)

/*
 * IXDP425 board IRQs
 */
#define	IRQ_IXDP425_PCI_INTA	IRQ_IXP4XX_GPIO11
#define	IRQ_IXDP425_PCI_INTB	IRQ_IXP4XX_GPIO10
#define	IRQ_IXDP425_PCI_INTC	IRQ_IXP4XX_GPIO9
#define	IRQ_IXDP425_PCI_INTD	IRQ_IXP4XX_GPIO8

/*
 * PrPMC1100 Board IRQs
 */
#define	IRQ_PRPMC1100_PCI_INTA	IRQ_IXP4XX_GPIO11
#define	IRQ_PRPMC1100_PCI_INTB	IRQ_IXP4XX_GPIO10
#define	IRQ_PRPMC1100_PCI_INTC	IRQ_IXP4XX_GPIO9
#define	IRQ_PRPMC1100_PCI_INTD	IRQ_IXP4XX_GPIO8

/*
 * ADI Coyote Board IRQs
 */
#define	IRQ_COYOTE_PCI_SLOT0	IRQ_IXP4XX_GPIO6
#define	IRQ_COYOTE_PCI_SLOT1	IRQ_IXP4XX_GPIO11
#define	IRQ_COYOTE_IDE		IRQ_IXP4XX_GPIO5

#endif
