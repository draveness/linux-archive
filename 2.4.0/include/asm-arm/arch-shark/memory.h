/*
 * linux/include/asm-arm/arch-shark/memory.h
 *
 * by Alexander.Schulz@stud.uni-karlsruhe.de
 *
 * derived from:
 * linux/include/asm-arm/arch-ebsa110/memory.h
 * Copyright (c) 1996-1999 Russell King.
 */
#ifndef __ASM_ARCH_MEMORY_H
#define __ASM_ARCH_MEMORY_H

/*
 * Task size: 3GB
 */
#define TASK_SIZE	(0xc0000000UL)
#define TASK_SIZE_26	(0x04000000UL)

/*
 * This decides where the kernel will search for a free chunk of vm
 * space during mmap's.
 */
#define TASK_UNMAPPED_BASE (TASK_SIZE / 3)

/*
 * Page offset: = 3GB
 */
#define PAGE_OFFSET	(0xC0000000UL)
#define PHYS_OFFSET     (0x08000000UL)

#define __virt_to_phys__is_a_macro
#define __phys_to_virt__is_a_macro
#define __virt_to_phys(vpage) (vpage - PAGE_OFFSET + PHYS_OFFSET)
#define __phys_to_virt(ppage) (ppage - PHYS_OFFSET + PAGE_OFFSET)

#define __virt_to_bus__is_a_macro
#define __virt_to_bus(x)	__virt_to_phys(x)
#define __bus_to_virt__is_a_macro
#define __bus_to_virt(x)	__phys_to_virt(x)

#endif
