/* linux/include/asm-arm/arch-s3c2410/uncompress.h
 *
 * (c) 2003 Simtec Electronics
 *    Ben Dooks <ben@simtec.co.uk>
 *
 * S3C2410 - uncompress code
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Changelog:
 *  22-May-2003 BJD  Created
 *  08-Sep-2003 BJD  Moved to linux v2.6
 *  12-Mar-2004 BJD  Updated header protection
*/

#ifndef __ASM_ARCH_UNCOMPRESS_H
#define __ASM_ARCH_UNCOMPRESS_H

/* defines for UART registers */
#include "asm/arch/regs-serial.h"

#include <asm/arch/map.h>

/* how many bytes we allow into the FIFO at a time in FIFO mode */
#define FIFO_MAX	 (14)

#if 1
#define uart_base S3C2410_PA_UART
#else
static unsigned int uart_base = S3C2410_PA_UART;
#endif

static __inline__ void
uart_wr(unsigned int reg, unsigned int val)
{
	volatile unsigned int *ptr;

	ptr = (volatile unsigned int *)(reg + uart_base);
	*ptr = val;
}

static __inline__ unsigned int
uart_rd(unsigned int reg)
{
	volatile unsigned int *ptr;

	ptr = (volatile unsigned int *)(reg + uart_base);
	return *ptr;
}


/* currently we do not need the watchdog... */
#define arch_decomp_wdog()


static void error(char *err);

static void
arch_decomp_setup(void)
{
	/* we may need to setup the uart(s) here if we are not running
	 * on an BAST... the BAST will have left the uarts configured
	 * after calling linux.
	 */
}

/* we can deal with the case the UARTs are being run
 * in FIFO mode, so that we don't hold up our execution
 * waiting for tx to happen...
*/

static void
putc(char ch)
{
	if (ch == '\n')
		putc('\r');    /* expand newline to \r\n */

	if (uart_rd(S3C2410_UFCON) & S3C2410_UFCON_FIFOMODE) {
		int level;

		while (1) {
			level = uart_rd(S3C2410_UFSTAT);
			level &= S3C2410_UFSTAT_TXMASK;
			level >>= S3C2410_UFSTAT_TXSHIFT;

			if (level < FIFO_MAX)
				break;
		}

	} else {
		/* not using fifos */

		while ((uart_rd(S3C2410_UTRSTAT) & S3C2410_UTRSTAT_TXFE) != S3C2410_UTRSTAT_TXFE);
	}

	/* write byte to transmission register */
	uart_wr(S3C2410_UTXH, ch);
}

static void
puts(const char *ptr)
{
	for (; *ptr != '\0'; ptr++) {
		putc(*ptr);
	}
}

#endif /* __ASM_ARCH_UNCOMPRESS_H */
