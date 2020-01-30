/* linux/include/asm/arch-s3c2410/regs0watchdog.h
 *
 * Copyright (c) 2003 Simtec Electronics <linux@simtec.co.uk>
 *		      http://www.simtec.co.uk/products/SWLINUX/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * S3C2410 Watchdog timer control
 *
 *  Changelog:
 *    21-06-2003     BJD     Created file
 *    12-03-2004     BJD     Updated include protection
*/


#ifndef __ASM_ARCH_REGS_WATCHDOG_H
#define __ASM_ARCH_REGS_WATCHDOG_H "$Id: watchdog.h,v 1.2 2003/04/29 13:31:09 ben Exp $"

#define S3C2410_WDOGREG(x) ((x) + S3C2410_VA_WATCHDOG)

#define S3C2410_WTCON	   S3C2410_WDOGREG(0x00)
#define S3C2410_WTDAT	   S3C2410_WDOGREG(0x04)
#define S3C2410_WTCNT	   S3C2410_WDOGREG(0x08)

/* the watchdog can either generate a reset pulse, or an
 * interrupt.
 */

#define S3C2410_WTCON_RSTEN   (0x01)
#define S3C2410_WTCON_INTEN   (1<<2)
#define S3C2410_WTCON_ENABLE  (1<<5)

#define S3C2410_WTCON_DIV16   (0<<3)
#define S3C2410_WTCON_DIV32   (1<<3)
#define S3C2410_WTCON_DIV64   (2<<3)
#define S3C2410_WTCON_DIV128  (3<<3)

#define S3C2410_WTCON_PRESCALE(x) ((x) << 8)

#endif /* __ASM_ARCH_REGS_WATCHDOG_H */


