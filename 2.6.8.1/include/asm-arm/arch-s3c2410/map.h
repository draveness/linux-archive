/* linux/include/asm-arm/arch-s3c2410/map.h
 *
 * (c) 2003 Simtec Electronics
 *  Ben Dooks <ben@simtec.co.uk>
 *
 * S3C2410 - Memory map definitions
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Changelog:
 *  12-May-2003 BJD  Created file
 *  06-Jan-2003 BJD  Linux 2.6.0 version, moved bast specifics out
*/

#ifndef __ASM_ARCH_MAP_H
#define __ASM_ARCH_MAP_H

/* we have a bit of a tight squeeze to fit all our registers from
 * 0xF00000000 upwards, since we use all of the nGCS space in some
 * capacity, and also need to fit the S3C2410 registers in as well...
 *
 * we try to ensure stuff like the IRQ registers are available for
 * an single MOVS instruction (ie, only 8 bits of set data)
 *
 * Note, we are trying to remove some of these from the implementation
 * as they are only useful to certain drivers...
 */

#define S3C2410_ADDR(x)	  (0xF0000000 + (x))

/* interrupt controller is the first thing we put in, to make
 * the assembly code for the irq detection easier
 */
#define S3C2410_VA_IRQ	   S3C2410_ADDR(0x00000000)
#define S3C2410_PA_IRQ	   (0x4A000000)
#define S3C2410_SZ_IRQ	   SZ_1M

/* memory controller registers */
#define S3C2410_VA_MEMCTRL S3C2410_ADDR(0x00100000)
#define S3C2410_PA_MEMCTRL (0x48000000)
#define S3C2410_SZ_MEMCTRL SZ_1M

/* USB host controller */
#define S3C2410_VA_USBHOST S3C2410_ADDR(0x00200000)
#define S3C2410_PA_USBHOST (0x49000000)
#define S3C2410_SZ_USBHOST SZ_1M

/* DMA controller */
#define S3C2410_VA_DMA	   S3C2410_ADDR(0x00300000)
#define S3C2410_PA_DMA	   (0x4B000000)
#define S3C2410_SZ_DMA	   SZ_1M

/* Clock and Power management */
#define S3C2410_VA_CLKPWR  S3C2410_ADDR(0x00400000)
#define S3C2410_PA_CLKPWR  (0x4C000000)
#define S3C2410_SZ_CLKPWR  SZ_1M

/* LCD controller */
#define S3C2410_VA_LCD	   S3C2410_ADDR(0x00600000)
#define S3C2410_PA_LCD	   (0x4D000000)
#define S3C2410_SZ_LCD	   SZ_1M

/* NAND flash controller */
#define S3C2410_VA_NAND	   S3C2410_ADDR(0x00700000)
#define S3C2410_PA_NAND	   (0x4E000000)
#define S3C2410_SZ_NAND	   SZ_1M

/* UARTs */
#define S3C2410_VA_UART	   S3C2410_ADDR(0x00800000)
#define S3C2410_PA_UART	   (0x50000000)
#define S3C2410_SZ_UART	   SZ_1M

/* Timers */
#define S3C2410_VA_TIMER   S3C2410_ADDR(0x00900000)
#define S3C2410_PA_TIMER   (0x51000000)
#define S3C2410_SZ_TIMER   SZ_1M

/* USB Device port */
#define S3C2410_VA_USBDEV  S3C2410_ADDR(0x00A00000)
#define S3C2410_PA_USBDEV  (0x52000000)
#define S3C2410_SZ_USBDEV  SZ_1M

/* Watchdog */
#define S3C2410_VA_WATCHDOG S3C2410_ADDR(0x00B00000)
#define S3C2410_PA_WATCHDOG (0x53000000)
#define S3C2410_SZ_WATCHDOG SZ_1M

/* IIC hardware controller */
#define S3C2410_VA_IIC	   S3C2410_ADDR(0x00C00000)
#define S3C2410_PA_IIC	   (0x54000000)
#define S3C2410_SZ_IIC	   SZ_1M

#define VA_IIC_BASE	   (S3C2410_VA_IIC)

/* IIS controller */
#define S3C2410_VA_IIS	   S3C2410_ADDR(0x00D00000)
#define S3C2410_PA_IIS	   (0x55000000)
#define S3C2410_SZ_IIS	   SZ_1M

/* GPIO ports */
#define S3C2410_VA_GPIO	   S3C2410_ADDR(0x00E00000)
#define S3C2410_PA_GPIO	   (0x56000000)
#define S3C2410_SZ_GPIO	   SZ_1M

/* RTC */
#define S3C2410_VA_RTC	   S3C2410_ADDR(0x00F00000)
#define S3C2410_PA_RTC	   (0x57000000)
#define S3C2410_SZ_RTC	   SZ_1M

/* ADC */
#define S3C2410_VA_ADC	   S3C2410_ADDR(0x01000000)
#define S3C2410_PA_ADC	   (0x58000000)
#define S3C2410_SZ_ADC	   SZ_1M

/* SPI */
#define S3C2410_VA_SPI	   S3C2410_ADDR(0x01100000)
#define S3C2410_PA_SPI	   (0x59000000)
#define S3C2410_SZ_SPI	   SZ_1M

/* SDI */
#define S3C2410_VA_SDI	   S3C2410_ADDR(0x01200000)
#define S3C2410_PA_SDI	   (0x5A000000)
#define S3C2410_SZ_SDI	   SZ_1M

/* ISA style IO, for each machine to sort out mappings for, if it
 * implements it. We reserve two 16M regions for ISA.
 */

#define S3C2410_VA_ISA_WORD  S3C2410_ADDR(0x02000000)
#define S3C2410_VA_ISA_BYTE  S3C2410_ADDR(0x03000000)

/* physical addresses of all the chip-select areas */

#define S3C2410_CS0 (0x00000000)
#define S3C2410_CS1 (0x08000000)
#define S3C2410_CS2 (0x10000000)
#define S3C2410_CS3 (0x18000000)
#define S3C2410_CS4 (0x20000000)
#define S3C2410_CS5 (0x28000000)
#define S3C2410_CS6 (0x30000000)
#define S3C2410_CS7 (0x38000000)

#define S3C2410_SDRAM_PA    (S3C2410_CS6)


#endif /* __ASM_ARCH_MAP_H */
