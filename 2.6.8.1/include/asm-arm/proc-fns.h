/*
 *  linux/include/asm-arm/proc-fns.h
 *
 *  Copyright (C) 1997-1999 Russell King
 *  Copyright (C) 2000 Deep Blue Solutions Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __ASM_PROCFNS_H
#define __ASM_PROCFNS_H

#ifdef __KERNEL__

#include <linux/config.h>

/*
 * Work out if we need multiple CPU support
 */
#undef MULTI_CPU
#undef CPU_NAME

/*
 * CPU_NAME - the prefix for CPU related functions
 */

#ifdef CONFIG_CPU_32
# ifdef CONFIG_CPU_ARM610
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME cpu_arm6
#  endif
# endif
# ifdef CONFIG_CPU_ARM710
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME cpu_arm7
#  endif
# endif
# ifdef CONFIG_CPU_ARM720T
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME cpu_arm720
#  endif
# endif
# ifdef CONFIG_CPU_ARM920T
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME cpu_arm920
#  endif
# endif
# ifdef CONFIG_CPU_ARM922T
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME cpu_arm922
#  endif
# endif
# ifdef CONFIG_CPU_ARM925T
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME cpu_arm925
#  endif
# endif
# ifdef CONFIG_CPU_ARM926T
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME cpu_arm926
#  endif
# endif
# ifdef CONFIG_CPU_SA110
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME cpu_sa110
#  endif
# endif
# ifdef CONFIG_CPU_SA1100
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME cpu_sa1100
#  endif
# endif
# ifdef CONFIG_CPU_ARM1020
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME cpu_arm1020
#  endif
# endif
# ifdef CONFIG_CPU_ARM1020E
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME cpu_arm1020e
#  endif
# endif
# ifdef CONFIG_CPU_ARM1022
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME cpu_arm1022
#  endif
# endif
# ifdef CONFIG_CPU_ARM1026
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME cpu_arm1026
#  endif
# endif
# ifdef CONFIG_CPU_XSCALE
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME cpu_xscale
#  endif
# endif
# ifdef CONFIG_CPU_V6
#  ifdef CPU_NAME
#   undef  MULTI_CPU
#   define MULTI_CPU
#  else
#   define CPU_NAME cpu_v6
#  endif
# endif
#endif

#ifndef __ASSEMBLY__

#ifndef MULTI_CPU
#include "asm/cpu-single.h"
#else
#include "asm/cpu-multi32.h"
#endif

#include <asm/memory.h>

#define cpu_switch_mm(pgd,mm) cpu_do_switch_mm(virt_to_phys(pgd),mm)

#define cpu_get_pgd()	\
	({						\
		unsigned long pg;			\
		__asm__("mrc	p15, 0, %0, c2, c0, 0"	\
			 : "=r" (pg) : : "cc");		\
		pg &= ~0x3fff;				\
		(pgd_t *)phys_to_virt(pg);		\
	})

#endif /* __ASSEMBLY__ */
#endif /* __KERNEL__ */
#endif /* __ASM_PROCFNS_H */
