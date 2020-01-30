/*
 *  linux/arch/arm/mach-clps711x/arch-edb7211.c
 *
 *  Copyright (C) 2000, 2001 Blue Mug, Inc.  All Rights Reserved.
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

extern void clps711x_init_irq(void);
extern void edb7211_map_io(void);
extern void clps711x_init_time(void);

static void __init
fixup_edb7211(struct machine_desc *desc, struct tag *tags,
	      char **cmdline, struct meminfo *mi)
{
	/*
	 * Bank start addresses are not present in the information
	 * passed in from the boot loader.  We could potentially
	 * detect them, but instead we hard-code them.
	 *
	 * Banks sizes _are_ present in the param block, but we're
	 * not using that information yet.
	 */
	mi->bank[0].start = 0xc0000000;
	mi->bank[0].size = 8*1024*1024;
	mi->bank[0].node = 0;
	mi->bank[1].start = 0xc1000000;
	mi->bank[1].size = 8*1024*1024;
	mi->bank[1].node = 1;
	mi->nr_banks = 2;
}

MACHINE_START(EDB7211, "CL-EDB7211 (EP7211 eval board)")
	MAINTAINER("Jon McClintock")
	BOOT_MEM(0xc0000000, 0x80000000, 0xff000000)
	BOOT_PARAMS(0xc0020100)	/* 0xc0000000 - 0xc001ffff can be video RAM */
	FIXUP(fixup_edb7211)
	MAPIO(edb7211_map_io)
	INITIRQ(clps711x_init_irq)
	INITTIME(clps711x_init_time)
MACHINE_END
