#ifndef _MOTOROLA_PGTABLE_H
#define _MOTOROLA_PGTABLE_H

#include <linux/config.h>

/*
 * Definitions for MMU descriptors
 */
#define _PAGE_PRESENT	0x001
#define _PAGE_SHORT	0x002
#define _PAGE_RONLY	0x004
#define _PAGE_ACCESSED	0x008
#define _PAGE_DIRTY	0x010
#define _PAGE_SUPER	0x080	/* 68040 supervisor only */
#define _PAGE_FAKE_SUPER 0x200	/* fake supervisor only on 680[23]0 */
#define _PAGE_GLOBAL040	0x400	/* 68040 global bit, used for kva descs */
#define _PAGE_COW	0x800	/* implemented in software */
#define _PAGE_NOCACHE030 0x040	/* 68030 no-cache mode */
#define _PAGE_NOCACHE	0x060	/* 68040 cache mode, non-serialized */
#define _PAGE_NOCACHE_S	0x040	/* 68040 no-cache mode, serialized */
#define _PAGE_CACHE040	0x020	/* 68040 cache mode, cachable, copyback */
#define _PAGE_CACHE040W	0x000	/* 68040 cache mode, cachable, write-through */

#define _DESCTYPE_MASK	0x003

#define _CACHEMASK040	(~0x060)
#define _TABLE_MASK	(0xfffffe00)

#define _PAGE_TABLE	(_PAGE_SHORT)
#define _PAGE_CHG_MASK  (PAGE_MASK | _PAGE_ACCESSED | _PAGE_DIRTY | _PAGE_NOCACHE)

#ifndef __ASSEMBLY__

/* This is the cache mode to be used for pages containing page descriptors for
 * processors >= '040. It is in pte_mknocache(), and the variable is defined
 * and initialized in head.S */
extern int m68k_pgtable_cachemode;

/* This is the cache mode for normal pages, for supervisor access on
 * processors >= '040. It is used in pte_mkcache(), and the variable is
 * defined and initialized in head.S */

#if defined(CONFIG_060_WRITETHROUGH)
extern int m68k_supervisor_cachemode;
#else
#define m68k_supervisor_cachemode _PAGE_CACHE040
#endif

#if defined(CPU_M68040_OR_M68060_ONLY)
#define mm_cachebits _PAGE_CACHE040
#elif defined(CPU_M68020_OR_M68030_ONLY)
#define mm_cachebits 0
#else
extern unsigned long mm_cachebits;
#endif

#define PAGE_NONE	__pgprot(_PAGE_PRESENT | _PAGE_RONLY | _PAGE_ACCESSED | mm_cachebits)
#define PAGE_SHARED	__pgprot(_PAGE_PRESENT | _PAGE_ACCESSED | mm_cachebits)
#define PAGE_COPY	__pgprot(_PAGE_PRESENT | _PAGE_RONLY | _PAGE_ACCESSED | mm_cachebits)
#define PAGE_READONLY	__pgprot(_PAGE_PRESENT | _PAGE_RONLY | _PAGE_ACCESSED | mm_cachebits)
#define PAGE_KERNEL	__pgprot(_PAGE_PRESENT | _PAGE_DIRTY | _PAGE_ACCESSED | mm_cachebits)

/* Alternate definitions that are compile time constants, for
   initializing protection_map.  The cachebits are fixed later.  */
#define PAGE_NONE_C	__pgprot(_PAGE_PRESENT | _PAGE_RONLY | _PAGE_ACCESSED)
#define PAGE_SHARED_C	__pgprot(_PAGE_PRESENT | _PAGE_ACCESSED)
#define PAGE_COPY_C	__pgprot(_PAGE_PRESENT | _PAGE_RONLY | _PAGE_ACCESSED)
#define PAGE_READONLY_C	__pgprot(_PAGE_PRESENT | _PAGE_RONLY | _PAGE_ACCESSED)

/*
 * The m68k can't do page protection for execute, and considers that the same are read.
 * Also, write permissions imply read permissions. This is the closest we can get..
 */
#define __P000	PAGE_NONE_C
#define __P001	PAGE_READONLY_C
#define __P010	PAGE_COPY_C
#define __P011	PAGE_COPY_C
#define __P100	PAGE_READONLY_C
#define __P101	PAGE_READONLY_C
#define __P110	PAGE_COPY_C
#define __P111	PAGE_COPY_C

#define __S000	PAGE_NONE_C
#define __S001	PAGE_READONLY_C
#define __S010	PAGE_SHARED_C
#define __S011	PAGE_SHARED_C
#define __S100	PAGE_READONLY_C
#define __S101	PAGE_READONLY_C
#define __S110	PAGE_SHARED_C
#define __S111	PAGE_SHARED_C

/*
 * Conversion functions: convert a page and protection to a page entry,
 * and a page entry and page directory to the page they refer to.
 */
#define __mk_pte(page, pgprot) \
({									\
	pte_t __pte;							\
									\
	pte_val(__pte) = __pa(page) + pgprot_val(pgprot);	        \
	__pte;								\
})
#define mk_pte(page, pgprot) __mk_pte(page_address(page), (pgprot))
#define mk_pte_phys(physpage, pgprot) \
({									\
	pte_t __pte;							\
									\
	pte_val(__pte) = (physpage) + pgprot_val(pgprot);		\
	__pte;								\
})

extern inline pte_t pte_modify(pte_t pte, pgprot_t newprot)
{ pte_val(pte) = (pte_val(pte) & _PAGE_CHG_MASK) | pgprot_val(newprot); return pte; }

extern inline void pmd_set(pmd_t * pmdp, pte_t * ptep)
{
	unsigned long ptbl = virt_to_phys(ptep) | _PAGE_TABLE | _PAGE_ACCESSED;
	unsigned long *ptr = pmdp->pmd;
	short i = 16;
	while (--i >= 0) {
		*ptr++ = ptbl;
		ptbl += (sizeof(pte_t)*PTRS_PER_PTE/16);
	}
}

extern inline void pgd_set(pgd_t * pgdp, pmd_t * pmdp)
{ pgd_val(*pgdp) = _PAGE_TABLE | _PAGE_ACCESSED | __pa(pmdp); }

#define __pte_page(pte) ((unsigned long)__va(pte_val(pte) & PAGE_MASK))
#define __pmd_page(pmd) ((unsigned long)__va(pmd_val(pmd) & _TABLE_MASK))
#define __pgd_page(pgd) ((unsigned long)__va(pgd_val(pgd) & _TABLE_MASK))


#define pte_none(pte)		(!pte_val(pte))
#define pte_present(pte)	(pte_val(pte) & (_PAGE_PRESENT | _PAGE_FAKE_SUPER))
#define pte_clear(ptep)		({ pte_val(*(ptep)) = 0; })
#define pte_pagenr(pte)		((__pte_page(pte) - PAGE_OFFSET) >> PAGE_SHIFT)

#define pmd_none(pmd)		(!pmd_val(pmd))
#define pmd_bad(pmd)		((pmd_val(pmd) & _DESCTYPE_MASK) != _PAGE_TABLE)
#define pmd_present(pmd)	(pmd_val(pmd) & _PAGE_TABLE)
#define pmd_clear(pmdp) ({			\
	unsigned long *__ptr = pmdp->pmd;	\
	short __i = 16;				\
	while (--__i >= 0)			\
		*__ptr++ = 0;			\
})


#define pgd_none(pgd)		(!pgd_val(pgd))
#define pgd_bad(pgd)		((pgd_val(pgd) & _DESCTYPE_MASK) != _PAGE_TABLE)
#define pgd_present(pgd)	(pgd_val(pgd) & _PAGE_TABLE)
#define pgd_clear(pgdp)		({ pgd_val(*pgdp) = 0; })
/* Permanent address of a page. */
#define page_address(page)	({ if (!(page)->virtual) BUG(); (page)->virtual; })
#define __page_address(page)	(PAGE_OFFSET + (((page) - mem_map) << PAGE_SHIFT))
#define pte_page(pte)		(mem_map+pte_pagenr(pte))

#define pte_ERROR(e) \
	printk("%s:%d: bad pte %08lx.\n", __FILE__, __LINE__, pte_val(e))
#define pmd_ERROR(e) \
	printk("%s:%d: bad pmd %08lx.\n", __FILE__, __LINE__, pmd_val(e))
#define pgd_ERROR(e) \
	printk("%s:%d: bad pgd %08lx.\n", __FILE__, __LINE__, pgd_val(e))


/*
 * The following only work if pte_present() is true.
 * Undefined behaviour if not..
 */
extern inline int pte_read(pte_t pte)		{ return 1; }
extern inline int pte_write(pte_t pte)		{ return !(pte_val(pte) & _PAGE_RONLY); }
extern inline int pte_exec(pte_t pte)		{ return 1; }
extern inline int pte_dirty(pte_t pte)		{ return pte_val(pte) & _PAGE_DIRTY; }
extern inline int pte_young(pte_t pte)		{ return pte_val(pte) & _PAGE_ACCESSED; }

extern inline pte_t pte_wrprotect(pte_t pte)	{ pte_val(pte) |= _PAGE_RONLY; return pte; }
extern inline pte_t pte_rdprotect(pte_t pte)	{ return pte; }
extern inline pte_t pte_exprotect(pte_t pte)	{ return pte; }
extern inline pte_t pte_mkclean(pte_t pte)	{ pte_val(pte) &= ~_PAGE_DIRTY; return pte; }
extern inline pte_t pte_mkold(pte_t pte)	{ pte_val(pte) &= ~_PAGE_ACCESSED; return pte; }
extern inline pte_t pte_mkwrite(pte_t pte)	{ pte_val(pte) &= ~_PAGE_RONLY; return pte; }
extern inline pte_t pte_mkread(pte_t pte)	{ return pte; }
extern inline pte_t pte_mkexec(pte_t pte)	{ return pte; }
extern inline pte_t pte_mkdirty(pte_t pte)	{ pte_val(pte) |= _PAGE_DIRTY; return pte; }
extern inline pte_t pte_mkyoung(pte_t pte)	{ pte_val(pte) |= _PAGE_ACCESSED; return pte; }
extern inline pte_t pte_mknocache(pte_t pte)
{
	pte_val(pte) = (pte_val(pte) & _CACHEMASK040) | m68k_pgtable_cachemode;
	return pte;
}
extern inline pte_t pte_mkcache(pte_t pte)	{ pte_val(pte) = (pte_val(pte) & _CACHEMASK040) | m68k_supervisor_cachemode; return pte; }

#define PAGE_DIR_OFFSET(tsk,address) pgd_offset((tsk),(address))

#define pgd_index(address)     ((address) >> PGDIR_SHIFT)

/* to find an entry in a page-table-directory */
extern inline pgd_t * pgd_offset(struct mm_struct * mm, unsigned long address)
{
	return mm->pgd + pgd_index(address);
}

#define swapper_pg_dir kernel_pg_dir
extern pgd_t kernel_pg_dir[128];

extern inline pgd_t * pgd_offset_k(unsigned long address)
{
	return kernel_pg_dir + (address >> PGDIR_SHIFT);
}


/* Find an entry in the second-level page table.. */
extern inline pmd_t * pmd_offset(pgd_t * dir, unsigned long address)
{
	return (pmd_t *)__pgd_page(*dir) + ((address >> PMD_SHIFT) & (PTRS_PER_PMD-1));
}

/* Find an entry in the third-level page table.. */ 
extern inline pte_t * pte_offset(pmd_t * pmdp, unsigned long address)
{
	return (pte_t *)__pmd_page(*pmdp) + ((address >> PAGE_SHIFT) & (PTRS_PER_PTE - 1));
}


/*
 * Allocate and free page tables. The xxx_kernel() versions are
 * used to allocate a kernel page table - this turns on ASN bits
 * if any.
 */

/* Prior to calling these routines, the page should have been flushed
 * from both the cache and ATC, or the CPU might not notice that the
 * cache setting for the page has been changed. -jskov
 */
static inline void nocache_page (unsigned long vaddr)
{
	if (CPU_IS_040_OR_060) {
		pgd_t *dir;
		pmd_t *pmdp;
		pte_t *ptep;

		dir = pgd_offset_k(vaddr);
		pmdp = pmd_offset(dir,vaddr);
		ptep = pte_offset(pmdp,vaddr);
		*ptep = pte_mknocache(*ptep);
	}
}

static inline void cache_page (unsigned long vaddr)
{
	if (CPU_IS_040_OR_060) {
		pgd_t *dir;
		pmd_t *pmdp;
		pte_t *ptep;

		dir = pgd_offset_k(vaddr);
		pmdp = pmd_offset(dir,vaddr);
		ptep = pte_offset(pmdp,vaddr);
		*ptep = pte_mkcache(*ptep);
	}
}


#endif	/* !__ASSEMBLY__ */
#endif /* _MOTOROLA_PGTABLE_H */
