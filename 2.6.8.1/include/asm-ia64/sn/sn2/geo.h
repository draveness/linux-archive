/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1992 - 1997, 2000-2003 Silicon Graphics, Inc. All rights reserved.
 */

#ifndef _ASM_IA64_SN_SN2_GEO_H
#define _ASM_IA64_SN_SN2_GEO_H

/* Headers required by declarations in this file */

#include <asm/sn/slotnum.h>


/* The geoid_t implementation below is based loosely on the pcfg_t
   implementation in sys/SN/promcfg.h. */

/* Type declaractions */

/* Size of a geoid_t structure (must be before decl. of geoid_u) */
#define GEOID_SIZE	8	/* Would 16 be better?  The size can
				   be different on different platforms. */

#define MAX_SLABS	0xe	/* slabs per module */

typedef unsigned char	geo_type_t;

/* Fields common to all substructures */
typedef struct geo_any_s {
    moduleid_t	module;		/* The module (box) this h/w lives in */
    geo_type_t	type;		/* What type of h/w is named by this geoid_t */
    slabid_t	slab;		/* The logical assembly within the module */
} geo_any_t;

/* Additional fields for particular types of hardware */
typedef struct geo_node_s {
    geo_any_t	any;		/* No additional fields needed */
} geo_node_t;

typedef struct geo_rtr_s {
    geo_any_t	any;		/* No additional fields needed */
} geo_rtr_t;

typedef struct geo_iocntl_s {
    geo_any_t	any;		/* No additional fields needed */
} geo_iocntl_t;

typedef struct geo_pcicard_s {
    geo_iocntl_t	any;
    char		bus;	/* Bus/widget number */
    slotid_t		slot;	/* PCI slot number */
} geo_pcicard_t;

/* Subcomponents of a node */
typedef struct geo_cpu_s {
    geo_node_t	node;
    char	slice;		/* Which CPU on the node */
} geo_cpu_t;

typedef struct geo_mem_s {
    geo_node_t	node;
    char	membus;		/* The memory bus on the node */
    char	memslot;	/* The memory slot on the bus */
} geo_mem_t;


typedef union geoid_u {
    geo_any_t	any;
    geo_node_t	node;
    geo_iocntl_t	iocntl;
    geo_pcicard_t	pcicard;
    geo_rtr_t	rtr;
    geo_cpu_t	cpu;
    geo_mem_t	mem;
    char	padsize[GEOID_SIZE];
} geoid_t;


/* Preprocessor macros */

#define GEO_MAX_LEN	48	/* max. formatted length, plus some pad:
				   module/001c07/slab/5/node/memory/2/slot/4 */

/* Values for geo_type_t */
#define GEO_TYPE_INVALID	0
#define GEO_TYPE_MODULE		1
#define GEO_TYPE_NODE		2
#define GEO_TYPE_RTR		3
#define GEO_TYPE_IOCNTL		4
#define GEO_TYPE_IOCARD		5
#define GEO_TYPE_CPU		6
#define GEO_TYPE_MEM		7
#define GEO_TYPE_MAX		(GEO_TYPE_MEM+1)

/* Parameter for hwcfg_format_geoid_compt() */
#define GEO_COMPT_MODULE	1
#define GEO_COMPT_SLAB		2
#define GEO_COMPT_IOBUS		3
#define GEO_COMPT_IOSLOT	4
#define GEO_COMPT_CPU		5
#define GEO_COMPT_MEMBUS	6
#define GEO_COMPT_MEMSLOT	7

#define GEO_INVALID_STR		"<invalid>"

#endif /* _ASM_IA64_SN_SN2_GEO_H */
