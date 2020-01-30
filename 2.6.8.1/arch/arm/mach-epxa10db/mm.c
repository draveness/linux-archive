/*
 *  linux/arch/arm/mach-epxa10db/mm.c
 *
 *  MM routines for Altera'a Epxa10db board
 *
 *  Copyright (C) 2001 Altera Corporation
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

#include <asm/hardware.h>
#include <asm/io.h>
#include <asm/sizes.h>
 
#include <asm/mach/map.h>

/* Page table mapping for I/O region */
 
static struct map_desc epxa10db_io_desc[] __initdata = {
 { IO_ADDRESS(EXC_REGISTERS_BASE),   EXC_REGISTERS_BASE,    SZ_16K, MT_DEVICE }, 
 { IO_ADDRESS(EXC_PLD_BLOCK0_BASE),  EXC_PLD_BLOCK0_BASE,   SZ_16K, MT_DEVICE }, 
 { IO_ADDRESS(EXC_PLD_BLOCK1_BASE),  EXC_PLD_BLOCK1_BASE,   SZ_16K, MT_DEVICE }, 
 { IO_ADDRESS(EXC_PLD_BLOCK2_BASE),  EXC_PLD_BLOCK2_BASE,   SZ_16K, MT_DEVICE }, 
 { IO_ADDRESS(EXC_PLD_BLOCK3_BASE),  EXC_PLD_BLOCK3_BASE,   SZ_16K, MT_DEVICE }, 
 { FLASH_VADDR(EXC_EBI_BLOCK0_BASE), EXC_EBI_BLOCK0_BASE,   SZ_16M, MT_DEVICE }
};

void __init epxa10db_map_io(void)
{
	iotable_init(epxa10db_io_desc, ARRAY_SIZE(epxa10db_io_desc));
}
