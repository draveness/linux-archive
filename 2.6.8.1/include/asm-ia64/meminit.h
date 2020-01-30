#ifndef meminit_h
#define meminit_h

/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 */

#include <linux/config.h>

/*
 * Entries defined so far:
 * 	- boot param structure itself
 * 	- memory map
 * 	- initrd (optional)
 * 	- command line string
 * 	- kernel code & data
 *
 * More could be added if necessary
 */
#define IA64_MAX_RSVD_REGIONS 5

struct rsvd_region {
	unsigned long start;	/* virtual address of beginning of element */
	unsigned long end;	/* virtual address of end of element + 1 */
};

extern struct rsvd_region rsvd_region[IA64_MAX_RSVD_REGIONS + 1];
extern int num_rsvd_regions;

extern void find_memory (void);
extern void reserve_memory (void);
extern void find_initrd (void);
extern int filter_rsvd_memory (unsigned long start, unsigned long end, void *arg);

/*
 * For rounding an address to the next IA64_GRANULE_SIZE or order
 */
#define GRANULEROUNDDOWN(n)	((n) & ~(IA64_GRANULE_SIZE-1))
#define GRANULEROUNDUP(n)	(((n)+IA64_GRANULE_SIZE-1) & ~(IA64_GRANULE_SIZE-1))
#define ORDERROUNDDOWN(n)	((n) & ~((PAGE_SIZE<<MAX_ORDER)-1))

#ifdef CONFIG_DISCONTIGMEM
  extern void call_pernode_memory (unsigned long start, unsigned long len, void *func);
#else
# define call_pernode_memory(start, len, func)	(*func)(start, len, 0)
#endif

#define IGNORE_PFN0	1	/* XXX fix me: ignore pfn 0 until TLB miss handler is updated... */

#ifdef CONFIG_VIRTUAL_MEM_MAP
# define LARGE_GAP	0x40000000 /* Use virtual mem map if hole is > than this */
  extern unsigned long vmalloc_end;
  extern struct page *vmem_map;
  extern int find_largest_hole (u64 start, u64 end, void *arg);
  extern int create_mem_map_page_table (u64 start, u64 end, void *arg);
#endif

#endif /* meminit_h */
