/* linux/include/asm/arch-s3c2410/regs-lcd.h
 *
 * Copyright (c) 2003 Simtec Electronics <linux@simtec.co.uk>
 *		      http://www.simtec.co.uk/products/SWLINUX/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *
 *
 *  Changelog:
 *    12-06-2003     BJD     Created file
 *    26-06-2003     BJD     Updated LCDCON register definitions
 *    12-03-2004     BJD     Updated include protection
*/


#ifndef ___ASM_ARCH_REGS_LCD_H
#define ___ASM_ARCH_REGS_LCD_H "$Id: lcd.h,v 1.3 2003/06/26 13:25:06 ben Exp $"

#define S3C2410_LCDREG(x) ((x) + S3C2410_VA_LCD)

/* LCD control registers */
#define S3C2410_LCDCON1	    S3C2410_LCDREG(0x00)
#define S3C2410_LCDCON2	    S3C2410_LCDREG(0x04)
#define S3C2410_LCDCON3	    S3C2410_LCDREG(0x08)
#define S3C2410_LCDCON4	    S3C2410_LCDREG(0x0C)
#define S3C2410_LCDCON5	    S3C2410_LCDREG(0x10)

#define S3C2410_LCDCON1_CLKVAL(x)  ((x) << 8)
#define S3C2410_LCDCON1_MMODE	   (1<<6)
#define S3C2410_LCDCON1_DSCAN4	   (0<<5)
#define S3C2410_LCDCON1_STN4	   (1<<5)
#define S3C2410_LCDCON1_STN8	   (2<<5)
#define S3C2410_LCDCON1_TFT	   (3<<5)

#define S3C2410_LCDCON1_STN1BPP	   (0<<1)
#define S3C2410_LCDCON1_STN2GREY   (1<<1)
#define S3C2410_LCDCON1_STN4GREY   (2<<1)
#define S3C2410_LCDCON1_STN8BPP	   (3<<1)
#define S3C2410_LCDCON1_STN12BPP   (4<<1)

#define S3C2410_LCDCON1_TFT1BPP	   (8<<1)
#define S3C2410_LCDCON1_TFT2BPP	   (9<<1)
#define S3C2410_LCDCON1_TFT4BPP	   (10<<1)
#define S3C2410_LCDCON1_TFT8BPP	   (11<<1)
#define S3C2410_LCDCON1_TFT16BPP   (12<<1)
#define S3C2410_LCDCON1_TFT24BPP   (13<<1)

#define S3C2410_LCDCON1_ENVDI	   (1)

#define S3C2410_LCDCON2_VBPD(x)	    ((x) << 24)
#define S3C2410_LCDCON2_LINEVAL(x)  ((x) << 14)
#define S3C2410_LCDCON2_VFPD(x)	    ((x) << 6)
#define S3C2410_LCDCON2_VSPW(x)	    ((x) << 0)

#define S3C2410_LCDCON3_HBPD(x)	    ((x) << 25)
#define S3C2410_LCDCON3_WDLY(x)	    ((x) << 25)
#define S3C2410_LCDCON3_HOZVAL(x)   ((x) << 8)
#define S3C2410_LCDCON3_HFPD(x)	    ((x) << 0)
#define S3C2410_LCDCON3_LINEBLANK(x)((x) << 0)

#define S3C2410_LCDCON4_MVAL(x)	    ((x) << 8)
#define S3C2410_LCDCON4_HSPW(x)	    ((x) << 0)
#define S3C2410_LCDCON4_WLH(x)	    ((x) << 0)

#define S3C2410_LCDCON5_BPP24BL	    (1<<12)
#define S3C2410_LCDCON5_FRM565	    (1<<11)
#define S3C2410_LCDCON5_INVVCLK	    (1<<10)
#define S3C2410_LCDCON5_INVVLINE    (1<<9)
#define S3C2410_LCDCON5_INVVFRAME   (1<<8)
#define S3C2410_LCDCON5_INVVD	    (1<<7)
#define S3C2410_LCDCON5_INVVDEN	    (1<<6)
#define S3C2410_LCDCON5_INVPWREN    (1<<5)
#define S3C2410_LCDCON5_INVLEND	    (1<<4)
#define S3C2410_LCDCON5_PWREN	    (1<<3)
#define S3C2410_LCDCON5_ENLEND	    (1<<2)
#define S3C2410_LCDCON5_BSWP	    (1<<1)
#define S3C2410_LCDCON5_HWSWP	    (1<<0)

/* framebuffer start addressed */
#define S3C2410_LCDSADDR1   S3C2410_LCDREG(0x14)
#define S3C2410_LCDSADDR2   S3C2410_LCDREG(0x18)
#define S3C2410_LCDSADDR3   S3C2410_LCDREG(0x1C)

/* colour lookup and miscellaneous controls */

#define S3C2410_REDLUT	   S3C2410_LCDREG(0x20)
#define S3C2410_GREENLUT   S3C2410_LCDREG(0x24)
#define S3C2410_BLUELUT	   S3C2410_LCDREG(0x28)

#define S3C2410_DITHMODE   S3C2410_LCDREG(0x4C)
#define S3C2410_TPAL	   S3C2410_LCDREG(0x50)

/* interrupt info */
#define S3C2410_LCDINTPND  S3C2410_LCDREG(0x54)
#define S3C2410_LCDSRCPND  S3C2410_LCDREG(0x58)
#define S3C2410_LCDINTMSK  S3C2410_LCDREG(0x5C)
#define S3C2410_LPCSEL	   S3C2410_LCDREG(0x60)

#define S3C2410_TFTPAL(x)  S3C2410_LCDREG((0x400 + (x)*4))

#endif /* ___ASM_ARCH_REGS_LCD_H */



