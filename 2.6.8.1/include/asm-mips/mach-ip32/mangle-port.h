/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2003 Ladislav Michl
 */
#ifndef __ASM_MACH_IP32_MANGLE_PORT_H
#define __ASM_MACH_IP32_MANGLE_PORT_H

#define __swizzle_addr_b(port)	((port) ^ 3)
#define __swizzle_addr_w(port)	((port) ^ 2)
#define __swizzle_addr_l(port)	(port)

#endif /* __ASM_MACH_IP32_MANGLE_PORT_H */
