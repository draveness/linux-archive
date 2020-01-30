/* $Id: page.h,v 1.39 2002/02/09 19:49:31 davem Exp $ */

#ifndef _SPARC64_PAGE_H
#define _SPARC64_PAGE_H

#include <linux/config.h>
#include <asm/const.h>

#define PAGE_SHIFT   13
#define PAGE_SIZE    (_AC(1,UL) << PAGE_SHIFT)
#define PAGE_MASK    (~(PAGE_SIZE-1))

#ifdef __KERNEL__

#ifndef __ASSEMBLY__

extern void _clear_page(void *page);
#define clear_page(X)	_clear_page((void *)(X))
struct page;
extern void clear_user_page(void *addr, unsigned long vaddr, struct page *page);
#define copy_page(X,Y)	memcpy((void *)(X), (void *)(Y), PAGE_SIZE)
extern void copy_user_page(void *to, void *from, unsigned long vaddr, struct page *topage);

/* GROSS, defining this makes gcc pass these types as aggregates,
 * and thus on the stack, turn this crap off... -DaveM
 */

/* #define STRICT_MM_TYPECHECKS */

#ifdef STRICT_MM_TYPECHECKS
/* These are used to make use of C type-checking.. */
typedef struct { unsigned long pte; } pte_t;
typedef struct { unsigned long iopte; } iopte_t;
typedef struct { unsigned int pmd; } pmd_t;
typedef struct { unsigned int pgd; } pgd_t;
typedef struct { unsigned long ctxd; } ctxd_t;
typedef struct { unsigned long pgprot; } pgprot_t;
typedef struct { unsigned long iopgprot; } iopgprot_t;

#define pte_val(x)	((x).pte)
#define iopte_val(x)	((x).iopte)
#define pmd_val(x)      ((x).pmd)
#define pgd_val(x)	((x).pgd)
#define ctxd_val(x)	((x).ctxd)
#define pgprot_val(x)	((x).pgprot)
#define iopgprot_val(x)	((x).iopgprot)

#define __pte(x)	((pte_t) { (x) } )
#define __iopte(x)	((iopte_t) { (x) } )
#define __pmd(x)        ((pmd_t) { (x) } )
#define __pgd(x)	((pgd_t) { (x) } )
#define __ctxd(x)	((ctxd_t) { (x) } )
#define __pgprot(x)	((pgprot_t) { (x) } )
#define __iopgprot(x)	((iopgprot_t) { (x) } )

#else
/* .. while these make it easier on the compiler */
typedef unsigned long pte_t;
typedef unsigned long iopte_t;
typedef unsigned int pmd_t;
typedef unsigned int pgd_t;
typedef unsigned long ctxd_t;
typedef unsigned long pgprot_t;
typedef unsigned long iopgprot_t;

#define pte_val(x)	(x)
#define iopte_val(x)	(x)
#define pmd_val(x)      (x)
#define pgd_val(x)	(x)
#define ctxd_val(x)	(x)
#define pgprot_val(x)	(x)
#define iopgprot_val(x)	(x)

#define __pte(x)	(x)
#define __iopte(x)	(x)
#define __pmd(x)        (x)
#define __pgd(x)	(x)
#define __ctxd(x)	(x)
#define __pgprot(x)	(x)
#define __iopgprot(x)	(x)

#endif /* (STRICT_MM_TYPECHECKS) */

#if defined(CONFIG_HUGETLB_PAGE_SIZE_4MB)
#define HPAGE_SHIFT		22
#elif defined(CONFIG_HUGETLB_PAGE_SIZE_512K)
#define HPAGE_SHIFT		19
#elif defined(CONFIG_HUGETLB_PAGE_SIZE_64K)
#define HPAGE_SHIFT		16
#endif

#ifdef CONFIG_HUGETLB_PAGE
#define HPAGE_SIZE		(_AC(1,UL) << HPAGE_SHIFT)
#define HPAGE_MASK		(~(HPAGE_SIZE - 1UL))
#define HUGETLB_PAGE_ORDER	(HPAGE_SHIFT - PAGE_SHIFT)
#endif

#define TASK_UNMAPPED_BASE	(test_thread_flag(TIF_32BIT) ? \
				 (_AC(0x0000000070000000,UL)) : (PAGE_OFFSET))

#endif /* !(__ASSEMBLY__) */

/* to align the pointer to the (next) page boundary */
#define PAGE_ALIGN(addr)	(((addr)+PAGE_SIZE-1)&PAGE_MASK)

/* We used to stick this into a hard-coded global register (%g4)
 * but that does not make sense anymore.
 */
#define PAGE_OFFSET		_AC(0xFFFFF80000000000,UL)

#ifndef __ASSEMBLY__

#define __pa(x)			((unsigned long)(x) - PAGE_OFFSET)
#define __va(x)			((void *)((unsigned long) (x) + PAGE_OFFSET))

/* PFNs are real physical page numbers.  However, mem_map only begins to record
 * per-page information starting at pfn_base.  This is to handle systems where
 * the first physical page in the machine is at some huge physical address,
 * such as 4GB.   This is common on a partitioned E10000, for example.
 */
extern struct page *pfn_to_page(unsigned long pfn);
extern unsigned long page_to_pfn(struct page *);

#define virt_to_page(kaddr)	pfn_to_page(__pa(kaddr)>>PAGE_SHIFT)

#define pfn_valid(pfn)		(((pfn)-(pfn_base)) < max_mapnr)
#define virt_addr_valid(kaddr)	pfn_valid(__pa(kaddr) >> PAGE_SHIFT)

#define virt_to_phys __pa
#define phys_to_virt __va

/* The following structure is used to hold the physical
 * memory configuration of the machine.  This is filled in
 * probe_memory() and is later used by mem_init() to set up
 * mem_map[].  We statically allocate SPARC_PHYS_BANKS of
 * these structs, this is arbitrary.  The entry after the
 * last valid one has num_bytes==0.
 */

struct sparc_phys_banks {
	unsigned long base_addr;
	unsigned long num_bytes;
};

#define SPARC_PHYS_BANKS 32

extern struct sparc_phys_banks sp_banks[SPARC_PHYS_BANKS];

/* Pure 2^n version of get_order */
static __inline__ int get_order(unsigned long size)
{
	int order;

	size = (size-1) >> (PAGE_SHIFT-1);
	order = -1;
	do {
		size >>= 1;
		order++;
	} while (size);
	return order;
}

#endif /* !(__ASSEMBLY__) */

#define VM_DATA_DEFAULT_FLAGS	(VM_READ | VM_WRITE | VM_EXEC | \
				 VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC)

#endif /* !(__KERNEL__) */

#endif /* !(_SPARC64_PAGE_H) */
