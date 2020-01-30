/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#ifndef __ARCH_PPC64_CACHE_H
#define __ARCH_PPC64_CACHE_H

/* bytes per L1 cache line */
#define L1_CACHE_SHIFT	7
#define L1_CACHE_BYTES	(1 << L1_CACHE_SHIFT)

#define SMP_CACHE_BYTES L1_CACHE_BYTES
#define L1_CACHE_SHIFT_MAX 7	/* largest L1 which this arch supports */

#endif
