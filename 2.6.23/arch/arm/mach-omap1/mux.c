/*
 * linux/arch/arm/mach-omap1/mux.c
 *
 * OMAP1 pin multiplexing configurations
 *
 * Copyright (C) 2003 - 2005 Nokia Corporation
 *
 * Written by Tony Lindgren <tony.lindgren@nokia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */
#include <linux/module.h>
#include <linux/init.h>
#include <asm/system.h>
#include <asm/io.h>
#include <linux/spinlock.h>

#include <asm/arch/mux.h>

#ifdef CONFIG_OMAP_MUX

#ifdef CONFIG_ARCH_OMAP730
struct pin_config __initdata_or_module omap730_pins[] = {
MUX_CFG_730("E2_730_KBR0",        12,   21,    0,   20,   1, 0)
MUX_CFG_730("J7_730_KBR1",        12,   25,    0,   24,   1, 0)
MUX_CFG_730("E1_730_KBR2",        12,   29,    0,   28,   1, 0)
MUX_CFG_730("F3_730_KBR3",        13,    1,    0,    0,   1, 0)
MUX_CFG_730("D2_730_KBR4",        13,    5,    0,    4,   1, 0)
MUX_CFG_730("C2_730_KBC0",        13,    9,    0,    8,   1, 0)
MUX_CFG_730("D3_730_KBC1",        13,   13,    0,   12,   1, 0)
MUX_CFG_730("E4_730_KBC2",        13,   17,    0,   16,   1, 0)
MUX_CFG_730("F4_730_KBC3",        13,   21,    0,   20,   1, 0)
MUX_CFG_730("E3_730_KBC4",        13,   25,    0,   24,   1, 0)

MUX_CFG_730("AA17_730_USB_DM",     2,   21,    0,   20,   0, 0)
MUX_CFG_730("W16_730_USB_PU_EN",   2,   25,    0,   24,   0, 0)
MUX_CFG_730("W17_730_USB_VBUSI",   2,   29,    0,   28,   0, 0)
};
#endif

#if defined(CONFIG_ARCH_OMAP15XX) || defined(CONFIG_ARCH_OMAP16XX)
struct pin_config __initdata_or_module omap1xxx_pins[] = {
/*
 *	 description		mux  mode   mux	 pull pull  pull  pu_pd	 pu  dbg
 *				reg  offset mode reg  bit   ena	  reg
 */
MUX_CFG("UART1_TX",		 9,   21,    1,	  2,   3,   0,	 NA,	 0,  0)
MUX_CFG("UART1_RTS",		 9,   12,    1,	  2,   0,   0,	 NA,	 0,  0)

/* UART2 (COM_UART_GATING), conflicts with USB2 */
MUX_CFG("UART2_TX",		 C,   27,    1,	  3,   3,   0,	 NA,	 0,  0)
MUX_CFG("UART2_RX",		 C,   18,    0,	  3,   1,   1,	 NA,	 0,  0)
MUX_CFG("UART2_CTS",		 C,   21,    0,	  3,   1,   1,	 NA,	 0,  0)
MUX_CFG("UART2_RTS",		 C,   24,    1,	  3,   2,   0,	 NA,	 0,  0)

/* UART3 (GIGA_UART_GATING) */
MUX_CFG("UART3_TX",		 6,    0,    1,	  0,  30,   0,	 NA,	 0,  0)
MUX_CFG("UART3_RX",		 6,    3,    0,	  0,  31,   1,	 NA,	 0,  0)
MUX_CFG("UART3_CTS",		 5,   12,    2,	  0,  24,   0,	 NA,	 0,  0)
MUX_CFG("UART3_RTS",		 5,   15,    2,	  0,  25,   0,	 NA,	 0,  0)
MUX_CFG("UART3_CLKREQ",		 9,   27,    0,	  2,   5,   0,	 NA,	 0,  0)
MUX_CFG("UART3_BCLK",		 A,    0,    0,	  2,   6,   0,	 NA,	 0,  0)
MUX_CFG("Y15_1610_UART3_RTS",	 A,    0,    1,	  2,   6,   0,	 NA,	 0,  0)

/* PWT & PWL, conflicts with UART3 */
MUX_CFG("PWT",			 6,    0,    2,	  0,  30,   0,	 NA,	 0,  0)
MUX_CFG("PWL",			 6,    3,    1,	  0,  31,   1,	 NA,	 0,  0)

/* USB internal master generic */
MUX_CFG("R18_USB_VBUS",		 7,    9,    2,	  1,  11,   0,	 NA,	 0,  1)
MUX_CFG("R18_1510_USB_GPIO0",	 7,    9,    0,	  1,  11,   1,	 NA,	 0,  1)
/* works around erratum:  W4_USB_PUEN and W4_USB_PUDIS are switched! */
MUX_CFG("W4_USB_PUEN",		 D,    3,    3,	  3,   5,   1,	 NA,	 0,  1)
MUX_CFG("W4_USB_CLKO",		 D,    3,    1,	  3,   5,   0,	 NA,	 0,  1)
MUX_CFG("W4_USB_HIGHZ",		 D,    3,    4,	  3,   5,   0,	  3,	 0,  1)
MUX_CFG("W4_GPIO58",		 D,    3,    7,	  3,   5,   0,	  3,	 0,  1)

/* USB1 master */
MUX_CFG("USB1_SUSP",		 8,   27,    2,	  1,  27,   0,	 NA,	 0,  1)
MUX_CFG("USB1_SE0",		 9,    0,    2,	  1,  28,   0,	 NA,	 0,  1)
MUX_CFG("W13_1610_USB1_SE0",	 9,    0,    4,	  1,  28,   0,	 NA,	 0,  1)
MUX_CFG("USB1_TXEN",		 9,    3,    2,	  1,  29,   0,	 NA,	 0,  1)
MUX_CFG("USB1_TXD",		 9,   24,    1,	  2,   4,   0,	 NA,	 0,  1)
MUX_CFG("USB1_VP",		 A,    3,    1,	  2,   7,   0,	 NA,	 0,  1)
MUX_CFG("USB1_VM",		 A,    6,    1,	  2,   8,   0,	 NA,	 0,  1)
MUX_CFG("USB1_RCV",		 A,    9,    1,	  2,   9,   0,	 NA,	 0,  1)
MUX_CFG("USB1_SPEED",		 A,   12,    2,	  2,  10,   0,	 NA,	 0,  1)
MUX_CFG("R13_1610_USB1_SPEED",	 A,   12,    5,	  2,  10,   0,	 NA,	 0,  1)
MUX_CFG("R13_1710_USB1_SEO",	 A,   12,    5,   2,  10,   0,   NA,     0,  1)

/* USB2 master */
MUX_CFG("USB2_SUSP",		 B,    3,    1,	  2,  17,   0,	 NA,	 0,  1)
MUX_CFG("USB2_VP",		 B,    6,    1,	  2,  18,   0,	 NA,	 0,  1)
MUX_CFG("USB2_TXEN",		 B,    9,    1,	  2,  19,   0,	 NA,	 0,  1)
MUX_CFG("USB2_VM",		 C,   18,    1,	  3,   0,   0,	 NA,	 0,  1)
MUX_CFG("USB2_RCV",		 C,   21,    1,	  3,   1,   0,	 NA,	 0,  1)
MUX_CFG("USB2_SE0",		 C,   24,    2,	  3,   2,   0,	 NA,	 0,  1)
MUX_CFG("USB2_TXD",		 C,   27,    2,	  3,   3,   0,	 NA,	 0,  1)

/* OMAP-1510 GPIO */
MUX_CFG("R18_1510_GPIO0",	 7,    9,    0,   1,  11,   1,    0,     0,  1)
MUX_CFG("R19_1510_GPIO1",	 7,    6,    0,   1,  10,   1,    0,     0,  1)
MUX_CFG("M14_1510_GPIO2",	 7,    3,    0,   1,   9,   1,    0,     0,  1)

/* OMAP1610 GPIO */
MUX_CFG("P18_1610_GPIO3",	 7,    0,    0,   1,   8,   0,   NA,     0,  1)
MUX_CFG("Y15_1610_GPIO17",	 A,    0,    7,   2,   6,   0,   NA,     0,  1)

/* OMAP-1710 GPIO */
MUX_CFG("R18_1710_GPIO0",        7,    9,    0,   1,  11,   1,    1,     1,  1)
MUX_CFG("V2_1710_GPIO10",        F,   27,    1,   4,   3,   1,    4,     1,  1)
MUX_CFG("N21_1710_GPIO14",       6,    9,    0,   1,   1,   1,    1,     1,  1)
MUX_CFG("W15_1710_GPIO40",       9,   27,    7,   2,   5,   1,    2,     1,  1)

/* MPUIO */
MUX_CFG("MPUIO2",		 7,   18,    0,	  1,  14,   1,	 NA,	 0,  1)
MUX_CFG("N15_1610_MPUIO2",	 7,   18,    0,	  1,  14,   1,	  1,	 0,  1)
MUX_CFG("MPUIO4",		 7,   15,    0,	  1,  13,   1,	 NA,	 0,  1)
MUX_CFG("MPUIO5",		 7,   12,    0,	  1,  12,   1,	 NA,	 0,  1)

MUX_CFG("T20_1610_MPUIO5",	 7,   12,    0,	  1,  12,   0,	  3,	 0,  1)
MUX_CFG("W11_1610_MPUIO6",	10,   15,    2,	  3,   8,   0,	  3,	 0,  1)
MUX_CFG("V10_1610_MPUIO7",	 A,   24,    2,	  2,  14,   0,	  2,	 0,  1)
MUX_CFG("W11_1610_MPUIO9",	10,   15,    1,	  3,   8,   0,	  3,	 0,  1)
MUX_CFG("V10_1610_MPUIO10",	 A,   24,    1,	  2,  14,   0,	  2,	 0,  1)
MUX_CFG("W10_1610_MPUIO11",	 A,   18,    2,	  2,  11,   0,	  2,	 0,  1)
MUX_CFG("E20_1610_MPUIO13",	 3,   21,    1,	  0,   7,   0,	  0,	 0,  1)
MUX_CFG("U20_1610_MPUIO14",	 9,    6,    6,	  0,  30,   0,	  0,	 0,  1)
MUX_CFG("E19_1610_MPUIO15",	 3,   18,    1,	  0,   6,   0,	  0,	 0,  1)

/* MCBSP2 */
MUX_CFG("MCBSP2_CLKR",		 C,    6,    0,	  2,  27,   1,	 NA,	 0,  1)
MUX_CFG("MCBSP2_CLKX",		 C,    9,    0,	  2,  29,   1,	 NA,	 0,  1)
MUX_CFG("MCBSP2_DR",		 C,    0,    0,	  2,  26,   1,	 NA,	 0,  1)
MUX_CFG("MCBSP2_DX",		 C,   15,    0,	  2,  31,   1,	 NA,	 0,  1)
MUX_CFG("MCBSP2_FSR",		 C,   12,    0,	  2,  30,   1,	 NA,	 0,  1)
MUX_CFG("MCBSP2_FSX",		 C,    3,    0,	  2,  27,   1,	 NA,	 0,  1)

/* MCBSP3 NOTE: Mode must 1 for clock */
MUX_CFG("MCBSP3_CLKX",		 9,    3,    1,	  1,  29,   0,	 NA,	 0,  1)

/* Misc ballouts */
MUX_CFG("BALLOUT_V8_ARMIO3",	 B,   18,    0,	  2,  25,   1,	 NA,	 0,  1)
MUX_CFG("N20_HDQ",		 6,   18,    1,   1,   4,   0,    1,     4,  0)

/* OMAP-1610 MMC2 */
MUX_CFG("W8_1610_MMC2_DAT0",	 B,   21,    6,	  2,  23,   1,	  2,	 1,  1)
MUX_CFG("V8_1610_MMC2_DAT1",	 B,   27,    6,	  2,  25,   1,	  2,	 1,  1)
MUX_CFG("W15_1610_MMC2_DAT2",	 9,   12,    6,	  2,   5,   1,	  2,	 1,  1)
MUX_CFG("R10_1610_MMC2_DAT3",	 B,   18,    6,	  2,  22,   1,	  2,	 1,  1)
MUX_CFG("Y10_1610_MMC2_CLK",	 B,    3,    6,	  2,  17,   0,	  2,	 0,  1)
MUX_CFG("Y8_1610_MMC2_CMD",	 B,   24,    6,	  2,  24,   1,	  2,	 1,  1)
MUX_CFG("V9_1610_MMC2_CMDDIR",	 B,   12,    6,	  2,  20,   0,	  2,	 1,  1)
MUX_CFG("V5_1610_MMC2_DATDIR0",	 B,   15,    6,	  2,  21,   0,	  2,	 1,  1)
MUX_CFG("W19_1610_MMC2_DATDIR1", 8,   15,    6,	  1,  23,   0,	  1,	 1,  1)
MUX_CFG("R18_1610_MMC2_CLKIN",	 7,    9,    6,	  1,  11,   0,	  1,	11,  1)

/* OMAP-1610 External Trace Interface */
MUX_CFG("M19_1610_ETM_PSTAT0",	 5,   27,    1,	  0,  29,   0,	  0,	 0,  1)
MUX_CFG("L15_1610_ETM_PSTAT1",	 5,   24,    1,	  0,  28,   0,	  0,	 0,  1)
MUX_CFG("L18_1610_ETM_PSTAT2",	 5,   21,    1,	  0,  27,   0,	  0,	 0,  1)
MUX_CFG("L19_1610_ETM_D0",	 5,   18,    1,	  0,  26,   0,	  0,	 0,  1)
MUX_CFG("J19_1610_ETM_D6",	 5,    0,    1,	  0,  20,   0,	  0,	 0,  1)
MUX_CFG("J18_1610_ETM_D7",	 5,   27,    1,	  0,  19,   0,	  0,	 0,  1)

/* OMAP16XX GPIO */
MUX_CFG("P20_1610_GPIO4",	 6,   27,    0,	  1,   7,   0,	  1,	 1,  1)
MUX_CFG("V9_1610_GPIO7",	 B,   12,    1,	  2,  20,   0,	  2,	 1,  1)
MUX_CFG("W8_1610_GPIO9",	 B,   21,    0,	  2,  23,   0,	  2,	 1,  1)
MUX_CFG("N20_1610_GPIO11",       6,   18,    0,   1,   4,   0,    1,     1,  1)
MUX_CFG("N19_1610_GPIO13",	 6,   12,    0,	  1,   2,   0,	  1,	 1,  1)
MUX_CFG("P10_1610_GPIO22",	 C,    0,    7,	  2,  26,   0,	  2,	 1,  1)
MUX_CFG("V5_1610_GPIO24",	 B,   15,    7,	  2,  21,   0,	  2,	 1,  1)
MUX_CFG("AA20_1610_GPIO_41",	 9,    9,    7,	  1,  31,   0,	  1,	 1,  1)
MUX_CFG("W19_1610_GPIO48",	 8,   15,    7,   1,  23,   1,    1,     0,  1)
MUX_CFG("M7_1610_GPIO62",	10,    0,    0,   4,  24,   0,    4,     0,  1)
MUX_CFG("V14_16XX_GPIO37",	 9,   18,    7,	  2,   2,   0,	  2,	 2,  0)
MUX_CFG("R9_16XX_GPIO18",	 C,   18,    7,   3,   0,   0,    3,     0,  0)
MUX_CFG("L14_16XX_GPIO49",	 6,    3,    7,   0,  31,   0,    0,    31,  0)

/* OMAP-1610 uWire */
MUX_CFG("V19_1610_UWIRE_SCLK",	 8,    6,    0,	  1,  20,   0,	  1,	 1,  1)
MUX_CFG("U18_1610_UWIRE_SDI",	 8,    0,    0,	  1,  18,   0,	  1,	 1,  1)
MUX_CFG("W21_1610_UWIRE_SDO",	 8,    3,    0,	  1,  19,   0,	  1,	 1,  1)
MUX_CFG("N14_1610_UWIRE_CS0",	 8,    9,    1,	  1,  21,   0,	  1,	 1,  1)
MUX_CFG("P15_1610_UWIRE_CS3",	 8,   12,    1,	  1,  22,   0,	  1,	 1,  1)
MUX_CFG("N15_1610_UWIRE_CS1",	 7,   18,    2,	  1,  14,   0,	 NA,	 0,  1)

/* OMAP-1610 SPI */
MUX_CFG("U19_1610_SPIF_SCK",	 7,    21,   6,	  1,  15,   0,	  1,	 1,  1)
MUX_CFG("U18_1610_SPIF_DIN",	 8,    0,    6,	  1,  18,   1,	  1,	 0,  1)
MUX_CFG("P20_1610_SPIF_DIN",	 6,    27,   4,   1,   7,   1,    1,     0,  1)
MUX_CFG("W21_1610_SPIF_DOUT",	 8,    3,    6,	  1,  19,   0,	  1,	 0,  1)
MUX_CFG("R18_1610_SPIF_DOUT",	 7,    9,    3,	  1,  11,   0,	  1,	 0,  1)
MUX_CFG("N14_1610_SPIF_CS0",	 8,    9,    6,	  1,  21,   0,	  1,	 1,  1)
MUX_CFG("N15_1610_SPIF_CS1",	 7,    18,   6,	  1,  14,   0,	  1,	 1,  1)
MUX_CFG("T19_1610_SPIF_CS2",	 7,    15,   4,	  1,  13,   0,	  1,	 1,  1)
MUX_CFG("P15_1610_SPIF_CS3",	 8,    12,   3,	  1,  22,   0,	  1,	 1,  1)

/* OMAP-1610 Flash */
MUX_CFG("L3_1610_FLASH_CS2B_OE",10,    6,    1,	 NA,   0,   0,	 NA,	 0,  1)
MUX_CFG("M8_1610_FLASH_CS2B_WE",10,    3,    1,	 NA,   0,   0,	 NA,	 0,  1)

/* First MMC interface, same on 1510, 1610 and 1710 */
MUX_CFG("MMC_CMD",		 A,   27,    0,	  2,  15,   1,	  2,	 1,  1)
MUX_CFG("MMC_DAT1",		 A,   24,    0,	  2,  14,   1,	  2,	 1,  1)
MUX_CFG("MMC_DAT2",		 A,   18,    0,	  2,  12,   1,	  2,	 1,  1)
MUX_CFG("MMC_DAT0",		 B,    0,    0,	  2,  16,   1,	  2,	 1,  1)
MUX_CFG("MMC_CLK",		 A,   21,    0,	 NA,   0,   0,	 NA,	 0,  1)
MUX_CFG("MMC_DAT3",		10,   15,    0,	  3,   8,   1,	  3,	 1,  1)
MUX_CFG("M15_1710_MMC_CLKI",	 6,   21,    2,   0,   0,   0,   NA,     0,  1)
MUX_CFG("P19_1710_MMC_CMDDIR",	 6,   24,    6,   0,   0,   0,   NA,     0,  1)
MUX_CFG("P20_1710_MMC_DATDIR0",	 6,   27,    5,   0,   0,   0,   NA,     0,  1)

/* OMAP-1610 USB0 alternate configuration */
MUX_CFG("W9_USB0_TXEN",		 B,   9,     5,	  2,  19,   0,	  2,	 0,  1)
MUX_CFG("AA9_USB0_VP",		 B,   6,     5,	  2,  18,   0,	  2,	 0,  1)
MUX_CFG("Y5_USB0_RCV",		 C,  21,     5,	  3,   1,   0,	  1,	 0,  1)
MUX_CFG("R9_USB0_VM",		 C,  18,     5,	  3,   0,   0,	  3,	 0,  1)
MUX_CFG("V6_USB0_TXD",		 C,  27,     5,	  3,   3,   0,	  3,	 0,  1)
MUX_CFG("W5_USB0_SE0",		 C,  24,     5,	  3,   2,   0,	  3,	 0,  1)
MUX_CFG("V9_USB0_SPEED",	 B,  12,     5,	  2,  20,   0,	  2,	 0,  1)
MUX_CFG("Y10_USB0_SUSP",	 B,   3,     5,	  2,  17,   0,	  2,	 0,  1)

/* USB2 interface */
MUX_CFG("W9_USB2_TXEN",		 B,   9,     1,	 NA,   0,   0,	 NA,	 0,  1)
MUX_CFG("AA9_USB2_VP",		 B,   6,     1,	 NA,   0,   0,	 NA,	 0,  1)
MUX_CFG("Y5_USB2_RCV",		 C,  21,     1,	 NA,   0,   0,	 NA,	 0,  1)
MUX_CFG("R9_USB2_VM",		 C,  18,     1,	 NA,   0,   0,	 NA,	 0,  1)
MUX_CFG("V6_USB2_TXD",		 C,  27,     2,	 NA,   0,   0,	 NA,	 0,  1)
MUX_CFG("W5_USB2_SE0",		 C,  24,     2,	 NA,   0,   0,	 NA,	 0,  1)

/* 16XX UART */
MUX_CFG("R13_1610_UART1_TX",	 A,  12,     6,	  2,  10,   0,	  2,	10,  1)
MUX_CFG("V14_16XX_UART1_RX",	 9,  18,     0,	  2,   2,   0,	  2,	 2,  1)
MUX_CFG("R14_1610_UART1_CTS",	 9,  15,     0,	  2,   1,   0,	  2,	 1,  1)
MUX_CFG("AA15_1610_UART1_RTS",	 9,  12,     1,	  2,   0,   0,	  2,	 0,  1)
MUX_CFG("R9_16XX_UART2_RX",	 C,  18,     0,   3,   0,   0,    3,     0,  1)
MUX_CFG("L14_16XX_UART3_RX",	 6,   3,     0,   0,  31,   0,    0,    31,  1)

/* I2C interface */
MUX_CFG("I2C_SCL",		 7,  24,     0,	 NA,   0,   0,	 NA,	 0,  0)
MUX_CFG("I2C_SDA",		 7,  27,     0,	 NA,   0,   0,	 NA,	 0,  0)

/* Keypad */
MUX_CFG("F18_1610_KBC0",	 3,  15,     0,	  0,   5,   1,	  0,	 0,  0)
MUX_CFG("D20_1610_KBC1",	 3,  12,     0,	  0,   4,   1,	  0,	 0,  0)
MUX_CFG("D19_1610_KBC2",	 3,   9,     0,	  0,   3,   1,	  0,	 0,  0)
MUX_CFG("E18_1610_KBC3",	 3,   6,     0,	  0,   2,   1,	  0,	 0,  0)
MUX_CFG("C21_1610_KBC4",	 3,   3,     0,	  0,   1,   1,	  0,	 0,  0)
MUX_CFG("G18_1610_KBR0",	 4,   0,     0,	  0,   10,  1,	  0,	 1,  0)
MUX_CFG("F19_1610_KBR1",	 3,   27,    0,	  0,   9,   1,	  0,	 1,  0)
MUX_CFG("H14_1610_KBR2",	 3,   24,    0,	  0,   8,   1,	  0,	 1,  0)
MUX_CFG("E20_1610_KBR3",	 3,   21,    0,	  0,   7,   1,	  0,	 1,  0)
MUX_CFG("E19_1610_KBR4",	 3,   18,    0,	  0,   6,   1,	  0,	 1,  0)
MUX_CFG("N19_1610_KBR5",	 6,  12,     1,	  1,   2,   1,	  1,	 1,  0)

/* Power management */
MUX_CFG("T20_1610_LOW_PWR",	 7,   12,    1,	  NA,   0,   0,   NA,	 0,  0)

/* MCLK Settings */
MUX_CFG("V5_1710_MCLK_ON",	 B,   15,    0,	  NA,   0,   0,   NA,	 0,  0)
MUX_CFG("V5_1710_MCLK_OFF",	 B,   15,    6,	  NA,   0,   0,   NA,	 0,  0)
MUX_CFG("R10_1610_MCLK_ON",	 B,   18,    0,	  NA,  22,   0,	  NA,	 1,  0)
MUX_CFG("R10_1610_MCLK_OFF",	 B,   18,    6,	  2,   22,   1,	  2,	 1,  1)

/* CompactFlash controller, conflicts with MMC1 */
MUX_CFG("P11_1610_CF_CD2",	 A,   27,    3,	  2,   15,   1,	  2,	 1,  1)
MUX_CFG("R11_1610_CF_IOIS16",	 B,    0,    3,	  2,   16,   1,	  2,	 1,  1)
MUX_CFG("V10_1610_CF_IREQ",	 A,   24,    3,	  2,   14,   0,	  2,	 0,  1)
MUX_CFG("W10_1610_CF_RESET",	 A,   18,    3,	  2,   12,   1,	  2,	 1,  1)
MUX_CFG("W11_1610_CF_CD1",	10,   15,    3,	  3,    8,   1,	  3,	 1,  1)
};
#endif	/* CONFIG_ARCH_OMAP15XX || CONFIG_ARCH_OMAP16XX */

int __init omap1_mux_init(void)
{

#ifdef CONFIG_ARCH_OMAP730
	omap_mux_register(omap730_pins, ARRAY_SIZE(omap730_pins));
#endif

#if defined(CONFIG_ARCH_OMAP15XX) || defined(CONFIG_ARCH_OMAP16XX)
	omap_mux_register(omap1xxx_pins, ARRAY_SIZE(omap1xxx_pins));
#endif

	return 0;
}

#endif
