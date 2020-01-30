/*
 *  linux/arch/arm/mach-clps711x/arch-ceiva.c
 *
 *  Copyright (C) 2002, Rob Scott <rscott@mtrob.fdns.net>
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
#include <linux/init.h>
#include <linux/types.h>
#include <linux/string.h>

#include <asm/setup.h>
#include <asm/mach-types.h>
#include <asm/mach/arch.h>

#include <linux/kernel.h>

#include <asm/hardware.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/sizes.h>

#include <asm/mach/map.h>

extern void clps711x_init_irq(void);
extern void clps711x_init_time(void);

static struct map_desc ceiva_io_desc[] __initdata = {
 /* virtual, physical, length, type */

 /* SED1355 controlled video RAM & registers */
 { CEIVA_VIRT_SED1355, CEIVA_PHYS_SED1355, SZ_2M, MT_DEVICE }

};


static void __init ceiva_map_io(void)
{
        clps711x_map_io();
        iotable_init(ceiva_io_desc, ARRAY_SIZE(ceiva_io_desc));
}


MACHINE_START(CEIVA, "CEIVA/Polaroid Photo MAX Digital Picture Frame")
	MAINTAINER("Rob Scott")
	BOOT_MEM(0xc0000000, 0x80000000, 0xff000000)
	BOOT_PARAMS(0xc0000100)
	MAPIO(ceiva_map_io)
	INITIRQ(clps711x_init_irq)
	INITTIME(clps711x_init_time)
MACHINE_END
