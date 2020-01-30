/*
 * linux/include/asm-arm/arch-omap/hardware.h
 *
 * Hardware definitions for TI OMAP processors and boards
 *
 * NOTE: Please put device driver specific defines into a separate header
 *	 file for each driver.
 *
 * Copyright (C) 2001 RidgeRun, Inc.
 * Author: RidgeRun, Inc. Greg Lonnon <glonnon@ridgerun.com>
 *
 * Reorganized for Linux-2.6 by Tony Lindgren <tony@atomide.com>
 *                          and Dirk Behme <dirk.behme@de.bosch.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN
 * NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef __ASM_ARCH_OMAP_HARDWARE_H
#define __ASM_ARCH_OMAP_HARDWARE_H

#include <asm/sizes.h>
#include <linux/config.h>
#ifndef __ASSEMBLER__
#include <asm/types.h>
#endif
#include <asm/arch/io.h>

/*
 * ---------------------------------------------------------------------------
 * Common definitions for all OMAP processors
 * NOTE: Put all processor or board specific parts to the special header
 *	 files.
 * ---------------------------------------------------------------------------
 */

/*
 * ----------------------------------------------------------------------------
 * Clocks
 * ----------------------------------------------------------------------------
 */
#define CLKGEN_REG_BASE		(0xfffece00)
#define ARM_CKCTL		(CLKGEN_REG_BASE + 0x0)
#define ARM_IDLECT1		(CLKGEN_REG_BASE + 0x4)
#define ARM_IDLECT2		(CLKGEN_REG_BASE + 0x8)
#define ARM_EWUPCT		(CLKGEN_REG_BASE + 0xC)
#define ARM_RSTCT1		(CLKGEN_REG_BASE + 0x10)
#define ARM_RSTCT2		(CLKGEN_REG_BASE + 0x14)
#define ARM_SYSST		(CLKGEN_REG_BASE + 0x18)

#define CK_RATEF		1
#define CK_IDLEF		2
#define CK_ENABLEF		4
#define CK_SELECTF		8
#define SETARM_IDLE_SHIFT

/* DPLL control registers */
#define DPLL_CTL		(0xfffecf00)

/* DSP clock control */
#define DSP_CONFIG_REG_BASE     (0xe1008000)
#define DSP_IDLECT1		(DSP_CONFIG_REG_BASE + 0x4)
#define DSP_IDLECT2		(DSP_CONFIG_REG_BASE + 0x8)

/*
 * ---------------------------------------------------------------------------
 * UPLD
 * ---------------------------------------------------------------------------
 */
#define ULPD_REG_BASE		(0xfffe0800)
#define ULPD_IT_STATUS		(ULPD_REG_BASE + 0x14)
#define ULPD_CLOCK_CTRL		(ULPD_REG_BASE + 0x30)
#define ULPD_SOFT_REQ		(ULPD_REG_BASE + 0x34)
#define ULPD_DPLL_CTRL		(ULPD_REG_BASE + 0x3c)
#define ULPD_STATUS_REQ		(ULPD_REG_BASE + 0x40)
#define ULPD_APLL_CTRL		(ULPD_REG_BASE + 0x4c)
#define ULPD_POWER_CTRL		(ULPD_REG_BASE + 0x50)
#define ULPD_CAM_CLK_CTRL	(ULPD_REG_BASE + 0x7c)

/*
 * ---------------------------------------------------------------------------
 * Timers
 * ---------------------------------------------------------------------------
 */
#define OMAP_32kHz_TIMER_BASE 0xfffb9000

/* 32k Timer Registers */
#define TIMER32k_CR		0x08
#define TIMER32k_TVR		0x00
#define TIMER32k_TCR		0x04

/* 32k Timer Control Register definition */
#define TIMER32k_TSS		(1<<0)
#define TIMER32k_TRB		(1<<1)
#define TIMER32k_INT		(1<<2)
#define TIMER32k_ARL		(1<<3)

/* MPU Timer base addresses */
#define OMAP_TIMER1_BASE	(0xfffec500)
#define OMAP_TIMER2_BASE	(0xfffec600)
#define OMAP_TIMER3_BASE	(0xfffec700)
#define OMAP_MPUTIMER_BASE	OMAP_TIMER1_BASE
#define OMAP_MPUTIMER_OFFSET	0x100

/* MPU Timer Registers */
#define OMAP_TIMER1_CNTL	(OMAP_TIMER_BASE1 + 0x0)
#define OMAP_TIMER1_LOAD_TIM	(OMAP_TIMER_BASE1 + 0x4)
#define OMAP_TIMER1_READ_TIM	(OMAP_TIMER_BASE1 + 0x8)

#define OMAP_TIMER2_CNTL	(OMAP_TIMER_BASE2 + 0x0)
#define OMAP_TIMER2_LOAD_TIM	(OMAP_TIMER_BASE2 + 0x4)
#define OMAP_TIMER2_READ_TIM	(OMAP_TIMER_BASE2 + 0x8)

#define OMAP_TIMER3_CNTL	(OMAP_TIMER_BASE3 + 0x0)
#define OMAP_TIMER3_LOAD_TIM	(OMAP_TIMER_BASE3 + 0x4)
#define OMAP_TIMER3_READ_TIM	(OMAP_TIMER_BASE3 + 0x8)

/* CNTL_TIMER register bits */
#define MPUTIM_FREE		(1<<6)
#define MPUTIM_CLOCK_ENABLE	(1<<5)
#define MPUTIM_PTV_MASK		(0x7<<PTV_BIT)
#define MPUTIM_PTV_BIT		2
#define MPUTIM_AR		(1<<1)
#define MPUTIM_ST		(1<<0)

/* Watchdog */
#define OMAP_WATCHDOG_BASE	(0xfffec800)
#define OMAP_WDT_TIMER		(OMAP_WATCHDOG_BASE + 0x0)
#define OMAP_WDT_LOAD_TIM	(OMAP_WATCHDOG_BASE + 0x4)
#define OMAP_WDT_READ_TIM	(OMAP_WATCHDOG_BASE + 0x4)
#define OMAP_WDT_TIMER_MODE	(OMAP_WATCHDOG_BASE + 0x8)

/*
 * ---------------------------------------------------------------------------
 * Interrupts
 * ---------------------------------------------------------------------------
 */
#define OMAP_IH1_BASE		0xfffecb00
#define OMAP_IH2_BASE		0xfffe0000

#define OMAP_IH1_ITR		(OMAP_IH1_BASE + 0x00)
#define OMAP_IH1_MIR		(OMAP_IH1_BASE + 0x04)
#define OMAP_IH1_SIR_IRQ	(OMAP_IH1_BASE + 0x10)
#define OMAP_IH1_SIR_FIQ	(OMAP_IH1_BASE + 0x14)
#define OMAP_IH1_CONTROL	(OMAP_IH1_BASE + 0x18)
#define OMAP_IH1_ILR0		(OMAP_IH1_BASE + 0x1c)
#define OMAP_IH1_ISR		(OMAP_IH1_BASE + 0x9c)

#define OMAP_IH2_ITR		(OMAP_IH2_BASE + 0x00)
#define OMAP_IH2_MIR		(OMAP_IH2_BASE + 0x04)
#define OMAP_IH2_SIR_IRQ	(OMAP_IH2_BASE + 0x10)
#define OMAP_IH2_SIR_FIQ	(OMAP_IH2_BASE + 0x14)
#define OMAP_IH2_CONTROL	(OMAP_IH2_BASE + 0x18)
#define OMAP_IH2_ILR0		(OMAP_IH2_BASE + 0x1c)
#define OMAP_IH2_ISR		(OMAP_IH2_BASE + 0x9c)

#define IRQ_ITR_REG_OFFSET	0x00
#define IRQ_MIR_REG_OFFSET	0x04
#define IRQ_SIR_IRQ_REG_OFFSET	0x10
#define IRQ_SIR_FIQ_REG_OFFSET	0x14
#define IRQ_CONTROL_REG_OFFSET	0x18
#define IRQ_ISR_REG_OFFSET	0x9c
#define IRQ_ILR0_REG_OFFSET	0x1c

/*
 * ---------------------------------------------------------------------------
 * Traffic controller memory interface
 * ---------------------------------------------------------------------------
 */
#define TCMIF_BASE		0xfffecc00
#define IMIF_PRIO		(TCMIF_BASE + 0x00)
#define EMIFS_PRIO		(TCMIF_BASE + 0x04)
#define EMIFF_PRIO		(TCMIF_BASE + 0x08)
#define EMIFS_CONFIG		(TCMIF_BASE + 0x0c)
#define EMIFS_CS0_CONFIG	(TCMIF_BASE + 0x10)
#define EMIFS_CS1_CONFIG	(TCMIF_BASE + 0x14)
#define EMIFS_CS2_CONFIG	(TCMIF_BASE + 0x18)
#define EMIFS_CS3_CONFIG	(TCMIF_BASE + 0x1c)
#define EMIFF_SDRAM_CONFIG	(TCMIF_BASE + 0x20)
#define EMIFF_MRS		(TCMIF_BASE + 0x24)
#define TC_TIMEOUT1		(TCMIF_BASE + 0x28)
#define TC_TIMEOUT2		(TCMIF_BASE + 0x2c)
#define TC_TIMEOUT3		(TCMIF_BASE + 0x30)
#define TC_ENDIANISM		(TCMIF_BASE + 0x34)
#define EMIFF_SDRAM_CONFIG_2	(TCMIF_BASE + 0x3c)
#define EMIF_CFG_DYNAMIC_WS	(TCMIF_BASE + 0x40)
/*
 * ----------------------------------------------------------------------------
 * System control registers
 * ----------------------------------------------------------------------------
 */
#define MOD_CONF_CTRL_0		0xfffe1080
#define MOD_CONF_CTRL_1		0xfffe1110

/*
 * ----------------------------------------------------------------------------
 * Pin multiplexing registers
 * ----------------------------------------------------------------------------
 */
#define FUNC_MUX_CTRL_0		0xfffe1000
#define FUNC_MUX_CTRL_1		0xfffe1004
#define FUNC_MUX_CTRL_2		0xfffe1008
#define COMP_MODE_CTRL_0	0xfffe100c
#define FUNC_MUX_CTRL_3		0xfffe1010
#define FUNC_MUX_CTRL_4		0xfffe1014
#define FUNC_MUX_CTRL_5		0xfffe1018
#define FUNC_MUX_CTRL_6		0xfffe101C
#define FUNC_MUX_CTRL_7		0xfffe1020
#define FUNC_MUX_CTRL_8		0xfffe1024
#define FUNC_MUX_CTRL_9		0xfffe1028
#define FUNC_MUX_CTRL_A		0xfffe102C
#define FUNC_MUX_CTRL_B		0xfffe1030
#define FUNC_MUX_CTRL_C		0xfffe1034
#define FUNC_MUX_CTRL_D		0xfffe1038
#define PULL_DWN_CTRL_0		0xfffe1040
#define PULL_DWN_CTRL_1		0xfffe1044
#define PULL_DWN_CTRL_2		0xfffe1048
#define PULL_DWN_CTRL_3		0xfffe104c

/* OMAP-1610 specific multiplexing registers */
#define FUNC_MUX_CTRL_E		0xfffe1090
#define FUNC_MUX_CTRL_F		0xfffe1094
#define FUNC_MUX_CTRL_10	0xfffe1098
#define FUNC_MUX_CTRL_11	0xfffe109c
#define FUNC_MUX_CTRL_12	0xfffe10a0
#define PU_PD_SEL_0		0xfffe10b4
#define PU_PD_SEL_1		0xfffe10b8
#define PU_PD_SEL_2		0xfffe10bc
#define PU_PD_SEL_3		0xfffe10c0
#define PU_PD_SEL_4		0xfffe10c4

/*
 * ---------------------------------------------------------------------------
 * TIPB bus interface
 * ---------------------------------------------------------------------------
 */
#define TIPB_PUBLIC_CNTL_BASE		0xfffed300
#define MPU_PUBLIC_TIPB_CNTL		(TIPB_PUBLIC_CNTL_BASE + 0x8)
#define TIPB_PRIVATE_CNTL_BASE		0xfffeca00
#define MPU_PRIVATE_TIPB_CNTL		(TIPB_PRIVATE_CNTL_BASE + 0x8)

/*
 * ----------------------------------------------------------------------------
 * MPUI interface
 * ----------------------------------------------------------------------------
 */
#define MPUI_BASE			(0xfffec900)
#define MPUI_CTRL			(MPUI_BASE + 0x0)
#define MPUI_DEBUG_ADDR			(MPUI_BASE + 0x4)
#define MPUI_DEBUG_DATA			(MPUI_BASE + 0x8)
#define MPUI_DEBUG_FLAG			(MPUI_BASE + 0xc)
#define MPUI_STATUS_REG			(MPUI_BASE + 0x10)
#define MPUI_DSP_STATUS			(MPUI_BASE + 0x14)
#define MPUI_DSP_BOOT_CONFIG		(MPUI_BASE + 0x18)
#define MPUI_DSP_API_CONFIG		(MPUI_BASE + 0x1c)

#ifndef __ASSEMBLER__

/*
 * ---------------------------------------------------------------------------
 * Processor differentiation
 * ---------------------------------------------------------------------------
 */
#define OMAP_ID_BASE		(0xfffed400)
#define OMAP_ID_REG		__REG32(OMAP_ID_BASE +  0x04)

#define ID_SHIFT		12
#define ID_MASK			0x7fff

/* See also uncompress.h */
#define OMAP_ID_730		0x355F
#define OMAP_ID_1510		0x3470
#define OMAP_ID_1610		0x3576
#define OMAP_ID_1710		0x35F7
#define OMAP_ID_5912		0x358C

#ifdef CONFIG_ARCH_OMAP730
#include "omap730.h"
#define cpu_is_omap730()	(((OMAP_ID_REG >> ID_SHIFT) & ID_MASK) == OMAP_ID_730)
#else
#define cpu_is_omap730()	0
#endif

#ifdef CONFIG_ARCH_OMAP1510
#include "omap1510.h"
#define cpu_is_omap1510()	(((OMAP_ID_REG >> ID_SHIFT) & ID_MASK) == OMAP_ID_1510)
#else
#define cpu_is_omap1510()	0
#endif

#ifdef CONFIG_ARCH_OMAP1610
#include "omap1610.h"
#define cpu_is_omap1710()       (((OMAP_ID_REG >> ID_SHIFT) & ID_MASK) == OMAP_ID_1710)
/* Detect 1710 as 1610 for now */
#define cpu_is_omap1610()	(((OMAP_ID_REG >> ID_SHIFT) & ID_MASK) == OMAP_ID_1610 \
				|| cpu_is_omap1710())
#else
#define cpu_is_omap1610()	0
#define cpu_is_omap1710()	0
#endif

#ifdef CONFIG_ARCH_OMAP5912
#include "omap5912.h"
#define cpu_is_omap5912()	(((OMAP_ID_REG >> ID_SHIFT) & ID_MASK) == OMAP_ID_5912)
#else
#define cpu_is_omap5912()	0
#endif

/*
 * ---------------------------------------------------------------------------
 * Board differentiation
 * ---------------------------------------------------------------------------
 */

#ifdef CONFIG_MACH_OMAP_INNOVATOR
#include "board-innovator.h"
#endif

#ifdef CONFIG_MACH_OMAP_H2
#include "board-h2.h"
#endif

#ifdef CONFIG_MACH_OMAP_PERSEUS2
#include "board-perseus2.h"
#endif

#ifdef CONFIG_MACH_OMAP_H3
#include "board-h3.h"
#error "Support for H3 board not yet implemented."
#endif

#ifdef CONFIG_MACH_OMAP_H4
#include "board-h4.h"
#error "Support for H4 board not yet implemented."
#endif

#ifdef CONFIG_MACH_OMAP_OSK
#include "board-osk.h"
#endif

#endif /* !__ASSEMBLER__ */

#endif	/* __ASM_ARCH_OMAP_HARDWARE_H */
