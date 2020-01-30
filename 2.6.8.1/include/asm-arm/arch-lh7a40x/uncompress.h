/* include/asm-arm/arch-lh7a40x/uncompress.h
 *
 *  Copyright (C) 2004 Coastal Environmental Systems
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  version 2 as published by the Free Software Foundation.
 *
 */

#include <asm/arch/registers.h>

#ifndef UART_R_DATA
# define UART_R_DATA	(0x00)
#endif
#ifndef UART_R_STATUS
# define UART_R_STATUS	(0x10)
#endif
#define nTxRdy  	(0x20)	/* Not TxReady (literally Tx FIFO full) */

	/* Access UART with physical addresses before MMU is setup */
#define UART_STATUS (*(volatile unsigned long*) (UART2_PHYS + UART_R_STATUS))
#define UART_DATA   (*(volatile unsigned long*) (UART2_PHYS + UART_R_DATA))

static __inline__ void putc (char ch)
{
	while (UART_STATUS & nTxRdy)
		;
	UART_DATA = ch;
}

static void puts (const char* sz)
{
	for (; *sz; ++sz) {
		putc (*sz);
		if (*sz == '\n')
			putc ('\r');
	}
}

	/* NULL functions; we don't presently need them */
#define arch_decomp_setup()
#define arch_decomp_wdog()
