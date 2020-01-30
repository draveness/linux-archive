
/* $Id: rtc-jazz.c,v 1.2 1998/06/25 20:19:14 ralf Exp $

 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * RTC routines for DECstation style attached Dallas chip.
 *
 * Copyright (C) 1998 by Ralf Baechle, Harald Koerfgen
 */
#include <linux/mc146818rtc.h>

extern char *dec_rtc_base;

static unsigned char dec_rtc_read_data(unsigned long addr)
{
    return (dec_rtc_base[addr * 4]);
}

static void dec_rtc_write_data(unsigned char data, unsigned long addr)
{
    dec_rtc_base[addr * 4] = data;
}

static int dec_rtc_bcd_mode(void)
{
    return 0;
}

struct rtc_ops dec_rtc_ops =
{
    &dec_rtc_read_data,
    &dec_rtc_write_data,
    &dec_rtc_bcd_mode
};
