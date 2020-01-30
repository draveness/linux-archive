/*
 *  linux/arch/arm/mach-clps711x/clep7312.c
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
extern void clps711x_map_io(void); 
extern void clps711x_init_time(void);

static void __init
fixup_clep7312(struct machine_desc *desc, struct tag *tags,
	    char **cmdline, struct meminfo *mi)
{
	mi->nr_banks=1;
	mi->bank[0].start = 0xc0000000;
	mi->bank[0].size = 0x01000000;
	mi->bank[0].node = 0;
}


MACHINE_START(CLEP7212, "Cirrus Logic 7212/7312")
	MAINTAINER("Nobody")
        BOOT_MEM(0xc0000000, 0x80000000, 0xff000000)
	BOOT_PARAMS(0xc0000100)
	FIXUP(fixup_clep7312)
	MAPIO(clps711x_map_io)
	INITIRQ(clps711x_init_irq)
	INITTIME(clps711x_init_time)
MACHINE_END

