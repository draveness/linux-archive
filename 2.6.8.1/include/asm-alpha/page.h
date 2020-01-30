#ifndef _ALPHA_PAGE_H
#define _ALPHA_PAGE_H

#include <asm/pal.h>

/* PAGE_SHIFT determines the page size */
#define PAGE_SHIFT	13
#define PAGE_SIZE	(1UL << PAGE_SHIFT)
#define PAGE_MASK	(~(PAGE_SIZE-1))

#ifdef __KERNEL__

#ifndef __ASSEMBLY__

#define STRICT_MM_TYPECHECKS

extern void clear_page(void *page);
#define clear_user_page(page, vaddr, pg)	clear_page(page)

extern void copy_page(void * _to, void * _from);
#define copy_user_page(to, from, vaddr, pg)	copy_page(to, from)

#ifdef STRICT_MM_TYPECHECKS
/*
 * These are used to make use of C type-checking..
 */
typedef struct { unsigned long pte; } pte_t;
typedef struct { unsigned long pmd; } pmd_t;
typedef struct { unsigned long pgd; } pgd_t;
typedef struct { unsigned long pgprot; } pgprot_t;

#define pte_val(x)	((x).pte)
#define pmd_val(x)	((x).pmd)
#define pgd_val(x)	((x).pgd)
#define pgprot_val(x)	((x).pgprot)

#define __pte(x)	((pte_t) { (x) } )
#define __pmd(x)	((pmd_t) { (x) } )
#define __pgd(x)	((pgd_t) { (x) } )
#define __pgprot(x)	((pgprot_t) { (x) } )

#else
/*
 * .. while these make it easier on the compiler
 */
typedef unsigned long pte_t;
typedef unsigned long pmd_t;
typedef unsigned long pgd_t;
typedef unsigned long pgprot_t;

#define pte_val(x)	(x)
#define pmd_val(x)	(x)
#define pgd_val(x)	(x)
#define pgprot_val(x)	(x)

#define __pte(x)	(x)
#define __pgd(x)	(x)
#define __pgprot(x)	(x)

#endif /* STRICT_MM_TYPECHECKS */

/* Pure 2^n version of get_order */
extern __inline__ int get_order(unsigned long size)
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

#ifdef USE_48_BIT_KSEG
#define PAGE_OFFSET		0xffff800000000000UL
#else
#define PAGE_OFFSET		0xfffffc0000000000UL
#endif

#else

#ifdef USE_48_BIT_KSEG
#define PAGE_OFFSET		0xffff800000000000
#else
#define PAGE_OFFSET		0xfffffc0000000000
#endif

#endif /* !__ASSEMBLY__ */

/* to align the pointer to the (next) page boundary */
#define PAGE_ALIGN(addr)	(((addr)+PAGE_SIZE-1)&PAGE_MASK)

#define __pa(x)			((unsigned long) (x) - PAGE_OFFSET)
#define __va(x)			((void *)((unsigned long) (x) + PAGE_OFFSET))
#ifndef CONFIG_DISCONTIGMEM
#define pfn_to_page(pfn)	(mem_map + (pfn))
#define page_to_pfn(page)	((unsigned long)((page) - mem_map))
#define virt_to_page(kaddr)	pfn_to_page(__pa(kaddr) >> PAGE_SHIFT)

#define pfn_valid(pfn)		((pfn) < max_mapnr)
#define virt_addr_valid(kaddr)	pfn_valid(__pa(kaddr) >> PAGE_SHIFT)
#endif /* CONFIG_DISCONTIGMEM */

#define VM_DATA_DEFAULT_FLAGS		(VM_READ | VM_WRITE | VM_EXEC | \
					 VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC)

#endif /* __KERNEL__ */

#endif /* _ALPHA_PAGE_H */
