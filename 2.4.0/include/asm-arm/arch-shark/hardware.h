/*
 * linux/include/asm-arm/arch-shark/hardware.h
 *
 * by Alexander.Schulz@stud.uni-karlsruhe.de
 *
 * derived from:
 * linux/include/asm-arm/arch-ebsa110/hardware.h
 * Copyright (C) 1996-1999 Russell King.
 */
#ifndef __ASM_ARCH_HARDWARE_H
#define __ASM_ARCH_HARDWARE_H

#ifndef __ASSEMBLY__

/*
 * Mapping areas
 */
#define IO_BASE			0xe0000000

/*
 * RAM definitions
 */
#define FLUSH_BASE_PHYS		0x80000000

#else

#define IO_BASE			0

#endif

#define IO_SIZE			0x08000000
#define IO_START		0x40000000
#define ROMCARD_SIZE		0x08000000
#define ROMCARD_START		0x10000000

#define FLUSH_BASE		0xdf000000
#define PCIO_BASE		0xe0000000


/* defines for the Framebuffer */
#define FB_START                0x06000000

/* Registers for Framebuffer */
/*#define FBREG_START             0x06800000*/

#define UNCACHEABLE_ADDR        0xdf010000

#define SEQUOIA_LED_GREEN       (1<<6)
#define SEQUOIA_LED_AMBER       (1<<5)
#define SEQUOIA_LED_BACK        (1<<7)

#endif

