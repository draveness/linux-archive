#ifndef _PPC64_LMB_H
#define _PPC64_LMB_H

/*
 * Definitions for talking to the Open Firmware PROM on
 * Power Macintosh computers.
 *
 * Copyright (C) 2001 Peter Bergner, IBM Corp.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/init.h>
#include <asm/prom.h>

extern unsigned long reloc_offset(void);

#define MAX_LMB_REGIONS 128

union lmb_reg_property { 
	struct reg_property32	 addr32[MAX_LMB_REGIONS];
	struct reg_property64	 addr64[MAX_LMB_REGIONS];
	struct reg_property_pmac addrPM[MAX_LMB_REGIONS];
};

#define LMB_ALLOC_ANYWHERE	0

struct lmb_property {
	unsigned long base;
	unsigned long physbase;
	unsigned long size;
};

struct lmb_region {
	unsigned long cnt;
	unsigned long size;
	struct lmb_property region[MAX_LMB_REGIONS+1];
};

struct lmb {
	unsigned long debug;
	unsigned long rmo_size;
	struct lmb_region memory;
	struct lmb_region reserved;
};

extern struct lmb lmb __initdata;

extern void __init lmb_init(void);
extern void __init lmb_analyze(void);
extern long __init lmb_add(unsigned long, unsigned long);
extern long __init lmb_reserve(unsigned long, unsigned long);
extern unsigned long __init lmb_alloc(unsigned long, unsigned long);
extern unsigned long __init lmb_alloc_base(unsigned long, unsigned long,
					   unsigned long);
extern unsigned long __init lmb_phys_mem_size(void);
extern unsigned long __init lmb_end_of_DRAM(void);
extern unsigned long __init lmb_abs_to_phys(unsigned long);

extern unsigned long io_hole_start;

#endif /* _PPC64_LMB_H */
