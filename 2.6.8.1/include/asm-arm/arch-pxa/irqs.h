/*
 *  linux/include/asm-arm/arch-pxa/irqs.h
 *
 *  Author:	Nicolas Pitre
 *  Created:	Jun 15, 2001
 *  Copyright:	MontaVista Software Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/config.h>

#define PXA_IRQ_SKIP	7	/* The first 7 IRQs are not yet used */
#define PXA_IRQ(x)	((x) - PXA_IRQ_SKIP)

#define IRQ_HWUART	PXA_IRQ(7)	/* HWUART Transmit/Receive/Error */
#define	IRQ_GPIO0	PXA_IRQ(8)	/* GPIO0 Edge Detect */
#define	IRQ_GPIO1	PXA_IRQ(9)	/* GPIO1 Edge Detect */
#define	IRQ_GPIO_2_80	PXA_IRQ(10)	/* GPIO[2-80] Edge Detect */
#define	IRQ_USB		PXA_IRQ(11)	/* USB Service */
#define	IRQ_PMU		PXA_IRQ(12)	/* Performance Monitoring Unit */
#define	IRQ_I2S		PXA_IRQ(13)	/* I2S Interrupt */
#define	IRQ_AC97	PXA_IRQ(14)	/* AC97 Interrupt */
#define IRQ_ASSP	PXA_IRQ(15)	/* Audio SSP Service Request */
#define IRQ_NSSP	PXA_IRQ(16)	/* Network SSP Service Request */
#define	IRQ_LCD		PXA_IRQ(17)	/* LCD Controller Service Request */
#define	IRQ_I2C		PXA_IRQ(18)	/* I2C Service Request */
#define	IRQ_ICP		PXA_IRQ(19)	/* ICP Transmit/Receive/Error */
#define	IRQ_STUART	PXA_IRQ(20)	/* STUART Transmit/Receive/Error */
#define	IRQ_BTUART	PXA_IRQ(21)	/* BTUART Transmit/Receive/Error */
#define	IRQ_FFUART	PXA_IRQ(22)	/* FFUART Transmit/Receive/Error*/
#define	IRQ_MMC		PXA_IRQ(23)	/* MMC Status/Error Detection */
#define	IRQ_SSP		PXA_IRQ(24)	/* SSP Service Request */
#define	IRQ_DMA 	PXA_IRQ(25)	/* DMA Channel Service Request */
#define	IRQ_OST0 	PXA_IRQ(26)	/* OS Timer match 0 */
#define	IRQ_OST1 	PXA_IRQ(27)	/* OS Timer match 1 */
#define	IRQ_OST2 	PXA_IRQ(28)	/* OS Timer match 2 */
#define	IRQ_OST3 	PXA_IRQ(29)	/* OS Timer match 3 */
#define	IRQ_RTC1Hz	PXA_IRQ(30)	/* RTC HZ Clock Tick */
#define	IRQ_RTCAlrm	PXA_IRQ(31)	/* RTC Alarm */

#define GPIO_2_80_TO_IRQ(x)	\
			PXA_IRQ((x) - 2 + 32)
#define IRQ_GPIO(x)	(((x) < 2) ? (IRQ_GPIO0 + (x)) : GPIO_2_80_TO_IRQ(x))

#define IRQ_TO_GPIO_2_80(i)	\
			((i) - PXA_IRQ(32) + 2)
#define IRQ_TO_GPIO(i)	((i) - (((i) > IRQ_GPIO1) ? IRQ_GPIO(2) - 2 : IRQ_GPIO(0)))

/*
 * The next 16 interrupts are for board specific purposes.  Since
 * the kernel can only run on one machine at a time, we can re-use
 * these.  If you need more, increase IRQ_BOARD_END, but keep it
 * within sensible limits.
 */
#define IRQ_BOARD_START		(IRQ_GPIO(80) + 1)
#define IRQ_BOARD_END		(IRQ_BOARD_START + 16)

#define IRQ_SA1111_START	(IRQ_BOARD_END)
#define IRQ_GPAIN0		(IRQ_BOARD_END + 0)
#define IRQ_GPAIN1		(IRQ_BOARD_END + 1)
#define IRQ_GPAIN2		(IRQ_BOARD_END + 2)
#define IRQ_GPAIN3		(IRQ_BOARD_END + 3)
#define IRQ_GPBIN0		(IRQ_BOARD_END + 4)
#define IRQ_GPBIN1		(IRQ_BOARD_END + 5)
#define IRQ_GPBIN2		(IRQ_BOARD_END + 6)
#define IRQ_GPBIN3		(IRQ_BOARD_END + 7)
#define IRQ_GPBIN4		(IRQ_BOARD_END + 8)
#define IRQ_GPBIN5		(IRQ_BOARD_END + 9)
#define IRQ_GPCIN0		(IRQ_BOARD_END + 10)
#define IRQ_GPCIN1		(IRQ_BOARD_END + 11)
#define IRQ_GPCIN2		(IRQ_BOARD_END + 12)
#define IRQ_GPCIN3		(IRQ_BOARD_END + 13)
#define IRQ_GPCIN4		(IRQ_BOARD_END + 14)
#define IRQ_GPCIN5		(IRQ_BOARD_END + 15)
#define IRQ_GPCIN6		(IRQ_BOARD_END + 16)
#define IRQ_GPCIN7		(IRQ_BOARD_END + 17)
#define IRQ_MSTXINT		(IRQ_BOARD_END + 18)
#define IRQ_MSRXINT		(IRQ_BOARD_END + 19)
#define IRQ_MSSTOPERRINT	(IRQ_BOARD_END + 20)
#define IRQ_TPTXINT		(IRQ_BOARD_END + 21)
#define IRQ_TPRXINT		(IRQ_BOARD_END + 22)
#define IRQ_TPSTOPERRINT	(IRQ_BOARD_END + 23)
#define SSPXMTINT		(IRQ_BOARD_END + 24)
#define SSPRCVINT		(IRQ_BOARD_END + 25)
#define SSPROR			(IRQ_BOARD_END + 26)
#define AUDXMTDMADONEA		(IRQ_BOARD_END + 32)
#define AUDRCVDMADONEA		(IRQ_BOARD_END + 33)
#define AUDXMTDMADONEB		(IRQ_BOARD_END + 34)
#define AUDRCVDMADONEB		(IRQ_BOARD_END + 35)
#define AUDTFSR			(IRQ_BOARD_END + 36)
#define AUDRFSR			(IRQ_BOARD_END + 37)
#define AUDTUR			(IRQ_BOARD_END + 38)
#define AUDROR			(IRQ_BOARD_END + 39)
#define AUDDTS			(IRQ_BOARD_END + 40)
#define AUDRDD			(IRQ_BOARD_END + 41)
#define AUDSTO			(IRQ_BOARD_END + 42)
#define IRQ_USBPWR		(IRQ_BOARD_END + 43)
#define IRQ_HCIM		(IRQ_BOARD_END + 44)
#define IRQ_HCIBUFFACC		(IRQ_BOARD_END + 45)
#define IRQ_HCIRMTWKP		(IRQ_BOARD_END + 46)
#define IRQ_NHCIMFCIR		(IRQ_BOARD_END + 47)
#define IRQ_USB_PORT_RESUME	(IRQ_BOARD_END + 48)
#define IRQ_S0_READY_NINT	(IRQ_BOARD_END + 49)
#define IRQ_S1_READY_NINT	(IRQ_BOARD_END + 50)
#define IRQ_S0_CD_VALID		(IRQ_BOARD_END + 51)
#define IRQ_S1_CD_VALID		(IRQ_BOARD_END + 52)
#define IRQ_S0_BVD1_STSCHG	(IRQ_BOARD_END + 53)
#define IRQ_S1_BVD1_STSCHG	(IRQ_BOARD_END + 54)

/*
 * Figure out the MAX IRQ number.
 *
 * If we have an SA1111, the max IRQ is S1_BVD1_STSCHG+1.
 * Otherwise, we have the standard IRQs only.
 */
#ifdef CONFIG_SA1111
#define NR_IRQS			(IRQ_S1_BVD1_STSCHG + 1)
#elif defined(CONFIG_ARCH_LUBBOCK) || \
      defined(CONFIG_MACH_MAINSTONE)
#define NR_IRQS			(IRQ_BOARD_END)
#else
#define NR_IRQS			(IRQ_BOARD_START)
#endif

/*
 * Board specific IRQs.  Define them here.
 * Do not surround them with ifdefs.
 */
#define LUBBOCK_IRQ(x)		(IRQ_BOARD_START + (x))
#define LUBBOCK_SD_IRQ		LUBBOCK_IRQ(0)
#define LUBBOCK_SA1111_IRQ	LUBBOCK_IRQ(1)
#define LUBBOCK_USB_IRQ		LUBBOCK_IRQ(2)  /* usb connect */
#define LUBBOCK_ETH_IRQ		LUBBOCK_IRQ(3)
#define LUBBOCK_UCB1400_IRQ	LUBBOCK_IRQ(4)
#define LUBBOCK_BB_IRQ		LUBBOCK_IRQ(5)
#define LUBBOCK_USB_DISC_IRQ	LUBBOCK_IRQ(6)  /* usb disconnect */
#define LUBBOCK_LAST_IRQ	LUBBOCK_IRQ(6)

#define MAINSTONE_IRQ(x)	(IRQ_BOARD_START + (x))
#define MAINSTONE_MMC_IRQ	MAINSTONE_IRQ(0)
#define MAINSTONE_USIM_IRQ	MAINSTONE_IRQ(1)
#define MAINSTONE_USBC_IRQ	MAINSTONE_IRQ(2)
#define MAINSTONE_ETHERNET_IRQ	MAINSTONE_IRQ(3)
#define MAINSTONE_AC97_IRQ	MAINSTONE_IRQ(4)
#define MAINSTONE_PEN_IRQ	MAINSTONE_IRQ(5)
#define MAINSTONE_MSINS_IRQ	MAINSTONE_IRQ(6)
#define MAINSTONE_EXBRD_IRQ	MAINSTONE_IRQ(7)
#define MAINSTONE_S0_CD_IRQ	MAINSTONE_IRQ(9)
#define MAINSTONE_S0_STSCHG_IRQ	MAINSTONE_IRQ(10)
#define MAINSTONE_S0_IRQ	MAINSTONE_IRQ(11)
#define MAINSTONE_S1_CD_IRQ	MAINSTONE_IRQ(13)
#define MAINSTONE_S1_STSCHG_IRQ	MAINSTONE_IRQ(14)
#define MAINSTONE_S1_IRQ	MAINSTONE_IRQ(15)

