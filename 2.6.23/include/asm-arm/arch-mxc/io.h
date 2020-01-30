/*
 *  Copyright 2004-2007 Freescale Semiconductor, Inc. All Rights Reserved.
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*!
 * @file io.h
 * @brief This file contains some memory mapping macros.
 * @note There is no real ISA or PCI buses. But have to define these macros
 * for some drivers to compile.
 *
 * @ingroup System
 */

#ifndef __ASM_ARCH_MXC_IO_H__
#define __ASM_ARCH_MXC_IO_H__

/*! Allow IO space to be anywhere in the memory */
#define IO_SPACE_LIMIT 0xffffffff

/*!
 * io address mapping macro
 */
#define __io(a)			((void __iomem *)(a))

#define __mem_pci(a)		(a)

#endif
