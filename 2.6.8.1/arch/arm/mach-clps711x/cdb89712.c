/*
 *  linux/arch/arm/mach-clps711x/cdb89712.c
 *
 *  Copyright (C) 2000-2001 Deep Blue Solutions Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/mm.h>

#include <asm/hardware.h>
#include <asm/io.h>
#include <asm/pgtable.h>
#include <asm/page.h>
#include <asm/setup.h>
#include <asm/mach-types.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>

extern void clps711x_init_irq(void);
extern void clps711x_map_io(void);
extern void clps711x_init_time(void);

/*
 * Map the CS89712 Ethernet port.  That should be moved to the
 * ethernet driver, perhaps.
 */
static struct map_desc cdb89712_io_desc[] __initdata = {
	{ ETHER_BASE, ETHER_START, ETHER_SIZE, MT_DEVICE }
};

static void __init cdb89712_map_io(void)
{
	clps711x_map_io();
	iotable_init(cdb89712_io_desc, ARRAY_SIZE(cdb89712_io_desc));
}

MACHINE_START(CDB89712, "Cirrus-CDB89712")
	MAINTAINER("Ray Lehtiniemi")
	BOOT_MEM(0xc0000000, 0x80000000, 0xff000000)
	BOOT_PARAMS(0xc0000100)
	MAPIO(cdb89712_map_io)
	INITIRQ(clps711x_init_irq)
	INITTIME(clps711x_init_time)
MACHINE_END

static int cdb89712_hw_init(void)
{
	return 0;
}

__initcall(cdb89712_hw_init);

