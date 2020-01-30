#ifndef _SUN3_PGTABLE_H
#define _SUN3_PGTABLE_H

#include <asm/sun3mmu.h>

#ifndef __ASSEMBLY__
#include <asm/virtconvert.h>
#include <linux/linkage.h>

/*
 * This file contains all the things which change drastically for the sun3
 * pagetable stuff, to avoid making too much of a mess of the generic m68k
 * `pgtable.h'; this should only be included from the generic file. --m
 */

/* For virtual address to physical address conversion */
#define VTOP(addr)	__pa(addr)
#define PTOV(addr)	__va(addr)


#endif	/* !__ASSEMBLY__ */

/* These need to be defined for compatibility although the sun3 doesn't use them */
#define _PAGE_NOCACHE030 0x040
#define _CACHEMASK040   (~0x060)
#define _PAGE_NOCACHE_S 0x040

/* Page protection values within PTE. */
#define SUN3_PAGE_VALID     (0x80000000)
#define SUN3_PAGE_WRITEABLE (0x40000000)
#define SUN3_PAGE_SYSTEM    (0x20000000)
#define SUN3_PAGE_NOCACHE   (0x10000000)
#define SUN3_PAGE_ACCESSED  (0x02000000)
#define SUN3_PAGE_MODIFIED  (0x01000000)


/* Externally used page protection values. */
#define _PAGE_PRESENT	(SUN3_PAGE_VALID)
#define _PAGE_ACCESSED	(SUN3_PAGE_ACCESSED)

/* Compound page protection values. */
//todo: work out which ones *should* have SUN3_PAGE_NOCACHE and fix...
// is it just PAGE_KERNEL and PAGE_SHARED?
#define PAGE_NONE	__pgprot(SUN3_PAGE_VALID \
				 | SUN3_PAGE_ACCESSED \
				 | SUN3_PAGE_NOCACHE)
#define PAGE_SHARED	__pgprot(SUN3_PAGE_VALID \
				 | SUN3_PAGE_WRITEABLE \
				 | SUN3_PAGE_ACCESSED \
				 | SUN3_PAGE_NOCACHE)
#define PAGE_COPY	__pgprot(SUN3_PAGE_VALID \
				 | SUN3_PAGE_ACCESSED \
				 | SUN3_PAGE_NOCACHE)
#define PAGE_READONLY	__pgprot(SUN3_PAGE_VALID \
				 | SUN3_PAGE_ACCESSED \
				 | SUN3_PAGE_NOCACHE)
#define PAGE_KERNEL	__pgprot(SUN3_PAGE_VALID \
				 | SUN3_PAGE_WRITEABLE \
				 | SUN3_PAGE_SYSTEM \
				 | SUN3_PAGE_NOCACHE \
				 | SUN3_PAGE_ACCESSED \
				 | SUN3_PAGE_MODIFIED)
#define PAGE_INIT	__pgprot(SUN3_PAGE_VALID \
				 | SUN3_PAGE_WRITEABLE \
				 | SUN3_PAGE_SYSTEM \
				 | SUN3_PAGE_NOCACHE)

/*
 * Page protections for initialising protection_map. The sun3 has only two
 * protection settings, valid (implying read and execute) and writeable. These
 * are as close as we can get...
 */
#define __P000	PAGE_NONE
#define __P001	PAGE_READONLY
#define __P010	PAGE_COPY
#define __P011	PAGE_COPY
#define __P100	PAGE_READONLY
#define __P101	PAGE_READONLY
#define __P110	PAGE_COPY
#define __P111	PAGE_COPY

#define __S000	PAGE_NONE
#define __S001	PAGE_READONLY
#define __S010	PAGE_SHARED
#define __S011	PAGE_SHARED
#define __S100	PAGE_READONLY
#define __S101	PAGE_READONLY
#define __S110	PAGE_SHARED
#define __S111	PAGE_SHARED

/* Use these fake page-protections on PMDs. */
#define SUN3_PMD_VALID	(0x00000001)
#define SUN3_PMD_MASK	(0x0000003F)
#define SUN3_PMD_MAGIC	(0x0000002B)

#ifndef __ASSEMBLY__

/*
 * Conversion functions: convert a page and protection to a page entry,
 * and a page entry and page directory to the page they refer to.
 */
#define __mk_pte(page, pgprot) \
({ pte_t __pte; pte_val(__pte) = (__pa(page) >> PAGE_SHIFT) | pgprot_val(pgprot); __pte; })
#define mk_pte(page, pgprot) __mk_pte(page_address(page), (pgprot))
#define mk_pte_phys(physpage, pgprot) \
({ pte_t __pte; pte_val(__pte) = ((physpage) >> PAGE_SHIFT) | pgprot_val(pgprot); __pte; })
extern inline pte_t pte_modify (pte_t pte, pgprot_t newprot)
{ pte_val(pte) = (pte_val(pte) & SUN3_PAGE_CHG_MASK) | pgprot_val(newprot); return pte; }

#define pmd_set(pmdp,ptep) do {} while (0)

extern inline void pgd_set(pgd_t * pgdp, pmd_t * pmdp)
{ pgd_val(*pgdp) = virt_to_phys(pmdp); }

#define __pte_page(pte) \
((unsigned long) __va ((pte_val (pte) & SUN3_PAGE_PGNUM_MASK) << PAGE_SHIFT))
#define __pmd_page(pmd) \
((unsigned long) __va (pmd_val (pmd) & PAGE_MASK))

extern inline int pte_none (pte_t pte) { return !pte_val (pte); }
extern inline int pte_present (pte_t pte) { return pte_val (pte) & SUN3_PAGE_VALID; }
extern inline void pte_clear (pte_t *ptep) { pte_val (*ptep) = 0; }

/* FIXME: this is only a guess */
#define pte_pagenr(pte)		((__pte_page(pte) - PAGE_OFFSET) >> PAGE_SHIFT)
/* Permanent address of a page. */
#define page_address(page)	({ if (!(page)->virtual) BUG(); (page)->virtual; })
#define __page_address(page)	(PAGE_OFFSET + (((page) - mem_map) << PAGE_SHIFT))
#define pte_page(pte)		(mem_map+pte_pagenr(pte))


extern inline int pmd_none2 (pmd_t *pmd) { return !pmd_val (*pmd); }
#define pmd_none(pmd) pmd_none2(&(pmd))
//extern inline int pmd_bad (pmd_t pmd) { return (pmd_val (pmd) & SUN3_PMD_MASK) != SUN3_PMD_MAGIC; }
extern inline int pmd_bad2 (pmd_t *pmd) { return 0; }
#define pmd_bad(pmd) pmd_bad2(&(pmd))
extern inline int pmd_present2 (pmd_t *pmd) { return pmd_val (*pmd) & SUN3_PMD_VALID; }
#define pmd_present(pmd) pmd_present2(&(pmd))
extern inline void pmd_clear (pmd_t *pmdp) { pmd_val (*pmdp) = 0; }

extern inline int pgd_none (pgd_t pgd) { return 0; }
extern inline int pgd_bad (pgd_t pgd) { return 0; }
extern inline int pgd_present (pgd_t pgd) { return 0; }
extern inline void pgd_clear (pgd_t *pgdp) {}


#define pte_ERROR(e) \
	printk("%s:%d: bad pte %08lx.\n", __FILE__, __LINE__, pte_val(e))
#define pmd_ERROR(e) \
	printk("%s:%d: bad pmd %08lx.\n", __FILE__, __LINE__, pmd_val(e))
#define pgd_ERROR(e) \
	printk("%s:%d: bad pgd %08lx.\n", __FILE__, __LINE__, pgd_val(e))


/*
 * The following only work if pte_present() is true.
 * Undefined behaviour if not...
 * [we have the full set here even if they don't change from m68k]
 */
extern inline int pte_read(pte_t pte)		{ return 1; }
extern inline int pte_write(pte_t pte)		{ return pte_val(pte) & SUN3_PAGE_WRITEABLE; }
extern inline int pte_exec(pte_t pte)		{ return 1; }
extern inline int pte_dirty(pte_t pte)		{ return pte_val(pte) & SUN3_PAGE_MODIFIED; }
extern inline int pte_young(pte_t pte)		{ return pte_val(pte) & SUN3_PAGE_ACCESSED; }

extern inline pte_t pte_wrprotect(pte_t pte)	{ pte_val(pte) &= ~SUN3_PAGE_WRITEABLE; return pte; }
extern inline pte_t pte_rdprotect(pte_t pte)	{ return pte; }
extern inline pte_t pte_exprotect(pte_t pte)	{ return pte; }
extern inline pte_t pte_mkclean(pte_t pte)	{ pte_val(pte) &= ~SUN3_PAGE_MODIFIED; return pte; }
extern inline pte_t pte_mkold(pte_t pte)	{ pte_val(pte) &= ~SUN3_PAGE_ACCESSED; return pte; }
extern inline pte_t pte_mkwrite(pte_t pte)	{ pte_val(pte) |= SUN3_PAGE_WRITEABLE; return pte; }
extern inline pte_t pte_mkread(pte_t pte)	{ return pte; }
extern inline pte_t pte_mkexec(pte_t pte)	{ return pte; }
extern inline pte_t pte_mkdirty(pte_t pte)	{ pte_val(pte) |= SUN3_PAGE_MODIFIED; return pte; }
extern inline pte_t pte_mkyoung(pte_t pte)	{ pte_val(pte) |= SUN3_PAGE_ACCESSED; return pte; }
extern inline pte_t pte_mknocache(pte_t pte)	{ pte_val(pte) |= SUN3_PAGE_NOCACHE; return pte; }
// use this version when caches work...
//extern inline pte_t pte_mkcache(pte_t pte)	{ pte_val(pte) &= SUN3_PAGE_NOCACHE; return pte; }
// until then, use:
extern inline pte_t pte_mkcache(pte_t pte)	{ return pte; }

extern pgd_t swapper_pg_dir[PTRS_PER_PGD];
extern pgd_t kernel_pg_dir[PTRS_PER_PGD];

/* Find an entry in a pagetable directory. */
#define pgd_index(address)     ((address) >> PGDIR_SHIFT)

#define pgd_offset(mm, address) \
((mm)->pgd + pgd_index(address))

/* Find an entry in a kernel pagetable directory. */
#define pgd_offset_k(address) pgd_offset(&init_mm, address)

/* Find an entry in the second-level pagetable. */
extern inline pmd_t *pmd_offset (pgd_t *pgd, unsigned long address)
{
	return (pmd_t *) pgd;
}

/* Find an entry in the third-level pagetable. */
#define pte_offset(pmd, address) \
((pte_t *) __pmd_page (*pmd) + ((address >> PAGE_SHIFT) & (PTRS_PER_PTE-1)))

/* Disable caching for page at given kernel virtual address. */
static inline void nocache_page (unsigned long vaddr)
{
	/* Don't think this is required on sun3. --m */
}

/* Enable caching for page at given kernel virtual address. */
static inline void cache_page (unsigned long vaddr)
{
	/* Don't think this is required on sun3. --m */
}



#endif	/* !__ASSEMBLY__ */
#endif	/* !_SUN3_PGTABLE_H */
