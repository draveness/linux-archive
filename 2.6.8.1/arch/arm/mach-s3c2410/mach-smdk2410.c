/***********************************************************************
 *
 * linux/arch/arm/mach-s3c2410/mach-smdk2410.c
 *
 * Copyright (C) 2004 by FS Forth-Systeme GmbH
 * All rights reserved.
 *
 * $Id: mach-smdk2410.c,v 1.1 2004/05/11 14:15:38 mpietrek Exp $
 * @Author: Jonas Dietsche
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 *
 * @History:
 * derived from linux/arch/arm/mach-s3c2410/mach-bast.c, written by
 * Ben Dooks <ben@simtec.co.uk>
 ***********************************************************************/

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/timer.h>
#include <linux/init.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/mach/irq.h>

#include <asm/hardware.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/mach-types.h>

#include <asm/arch/regs-serial.h>

#include "s3c2410.h"


static struct map_desc smdk2410_iodesc[] __initdata = {
  /* nothing here yet */
};

#define UCON S3C2410_UCON_DEFAULT
#define ULCON S3C2410_LCON_CS8 | S3C2410_LCON_PNONE | S3C2410_LCON_STOPB
#define UFCON S3C2410_UFCON_RXTRIG8 | S3C2410_UFCON_FIFOMODE

/* base baud rate for all our UARTs */
static unsigned long smdk2410_serial_clock = 24*1000*1000;

static struct s3c2410_uartcfg smdk2410_uartcfgs[] = {
	[0] = {
		.hwport	     = 0,
		.flags	     = 0,
		.clock	     = &smdk2410_serial_clock,
		.ucon	     = UCON,
		.ulcon	     = ULCON,
		.ufcon	     = UFCON,
	},
	[1] = {
		.hwport	     = 1,
		.flags	     = 0,
		.clock	     = &smdk2410_serial_clock,
		.ucon	     = UCON,
		.ulcon	     = ULCON,
		.ufcon	     = UFCON,
	},
	[2] = {
		.hwport	     = 2,
		.flags	     = 0,
		.clock	     = &smdk2410_serial_clock,
		.ucon	     = UCON,
		.ulcon	     = ULCON,
		.ufcon	     = UFCON,
	}
};


void __init smdk2410_map_io(void)
{
	s3c2410_map_io(smdk2410_iodesc, ARRAY_SIZE(smdk2410_iodesc));
	s3c2410_uartcfgs = smdk2410_uartcfgs;
}

void __init smdk2410_init_irq(void)
{
	s3c2410_init_irq();
}

void __init smdk2410_init_time(void)
{
	s3c2410_init_time();
}

MACHINE_START(SMDK2410, "SMDK2410") /* @TODO: request a new identifier and switch
				    * to SMDK2410 */
     MAINTAINER("Jonas Dietsche")
     BOOT_MEM(S3C2410_SDRAM_PA, S3C2410_PA_UART, S3C2410_VA_UART)
     BOOT_PARAMS(S3C2410_SDRAM_PA + 0x100)
     MAPIO(smdk2410_map_io)
     INITIRQ(smdk2410_init_irq)
     INITTIME(smdk2410_init_time)
MACHINE_END
