/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1994, 95, 96, 97, 98, 99, 2000, 2003 Ralf Baechle
 * Copyright (C) 1999, 2000, 2001 Silicon Graphics, Inc.
 */
#ifndef _ASM_PGTABLE_64_H
#define _ASM_PGTABLE_64_H

#include <linux/config.h>
#include <linux/linkage.h>

#include <asm/addrspace.h>
#include <asm/page.h>
#include <asm/cachectl.h>

/*
 * Each address space has 2 4K pages as its page directory, giving 1024
 * (== PTRS_PER_PGD) 8 byte pointers to pmd tables. Each pmd table is a
 * pair of 4K pages, giving 1024 (== PTRS_PER_PMD) 8 byte pointers to
 * page tables. Each page table is a single 4K page, giving 512 (==
 * PTRS_PER_PTE) 8 byte ptes. Each pgde is initialized to point to
 * invalid_pmd_table, each pmde is initialized to point to
 * invalid_pte_table, each pte is initialized to 0. When memory is low,
 * and a pmd table or a page table allocation fails, empty_bad_pmd_table
 * and empty_bad_page_table is returned back to higher layer code, so
 * that the failure is recognized later on. Linux does not seem to
 * handle these failures very well though. The empty_bad_page_table has
 * invalid pte entries in it, to force page faults.
 * Vmalloc handling: vmalloc uses swapper_pg_dir[0] (returned by
 * pgd_offset_k), which is initalized to point to kpmdtbl. kpmdtbl is
 * the only single page pmd in the system. kpmdtbl entries point into
 * kptbl[] array. We reserve 1 << PGD_ORDER pages to hold the
 * vmalloc range translations, which the fault handler looks at.
 */

/* PMD_SHIFT determines the size of the area a second-level page table can map */
#define PMD_SHIFT	(PAGE_SHIFT + (PAGE_SHIFT - 3))
#define PMD_SIZE	(1UL << PMD_SHIFT)
#define PMD_MASK	(~(PMD_SIZE-1))

/* PGDIR_SHIFT determines what a third-level page table entry can map */
#define PGDIR_SHIFT	(PMD_SHIFT + (PAGE_SHIFT + 1 - 3))
#define PGDIR_SIZE	(1UL << PGDIR_SHIFT)
#define PGDIR_MASK	(~(PGDIR_SIZE-1))

/*
 * For 4kB page size we use a 3 level page tree and a 8kB pmd and pgds which
 * permits us mapping 40 bits of virtual address space.
 *
 * We used to implement 41 bits by having an order 1 pmd level but that seemed
 * rather pointless.
 *
 * For 8kB page size we use a 3 level page tree which permits a total of
 * 8TB of address space.  Alternatively a 33-bit / 8GB organization using
 * two levels would be easy to implement.
 *
 * For 16kB page size we use a 2 level page tree which permits a total of
 * 36 bits of virtual address space.  We could add a third leve. but it seems
 * like at the moment there's no need for this.
 *
 * For 64kB page size we use a 2 level page table tree for a total of 42 bits
 * of virtual address space.
 */
#ifdef CONFIG_PAGE_SIZE_4KB
#define PGD_ORDER		1
#define PMD_ORDER		1
#define PTE_ORDER		0
#endif
#ifdef CONFIG_PAGE_SIZE_8KB
#define PGD_ORDER		0
#define PMD_ORDER		0
#define PTE_ORDER		0
#endif
#ifdef CONFIG_PAGE_SIZE_16KB
#define PGD_ORDER		0
#define PMD_ORDER		0
#define PTE_ORDER		0
#endif
#ifdef CONFIG_PAGE_SIZE_64KB
#define PGD_ORDER		0
#define PMD_ORDER		0
#define PTE_ORDER		0
#endif

#define PTRS_PER_PGD	((PAGE_SIZE << PGD_ORDER) / sizeof(pgd_t))
#define PTRS_PER_PMD	((PAGE_SIZE << PMD_ORDER) / sizeof(pmd_t))
#define PTRS_PER_PTE	((PAGE_SIZE << PTE_ORDER) / sizeof(pte_t))

#define USER_PTRS_PER_PGD	(TASK_SIZE / PGDIR_SIZE)
#define FIRST_USER_PGD_NR	0

#define VMALLOC_START		XKSEG
#define VMALLOC_END	\
	(VMALLOC_START + ((1 << PGD_ORDER) * PTRS_PER_PTE * PAGE_SIZE))

#define pte_ERROR(e) \
	printk("%s:%d: bad pte %016lx.\n", __FILE__, __LINE__, pte_val(e))
#define pmd_ERROR(e) \
	printk("%s:%d: bad pmd %016lx.\n", __FILE__, __LINE__, pmd_val(e))
#define pgd_ERROR(e) \
	printk("%s:%d: bad pgd %016lx.\n", __FILE__, __LINE__, pgd_val(e))

extern pte_t invalid_pte_table[PAGE_SIZE/sizeof(pte_t)];
extern pte_t empty_bad_page_table[PAGE_SIZE/sizeof(pte_t)];
extern pmd_t invalid_pmd_table[2*PAGE_SIZE/sizeof(pmd_t)];
extern pmd_t empty_bad_pmd_table[2*PAGE_SIZE/sizeof(pmd_t)];

/*
 * Empty pmd entries point to the invalid_pte_table.
 */
static inline int pmd_none(pmd_t pmd)
{
	return pmd_val(pmd) == (unsigned long) invalid_pte_table;
}

#define pmd_bad(pmd)		(pmd_val(pmd) & ~PAGE_MASK)

static inline int pmd_present(pmd_t pmd)
{
	return pmd_val(pmd) != (unsigned long) invalid_pte_table;
}

static inline void pmd_clear(pmd_t *pmdp)
{
	pmd_val(*pmdp) = ((unsigned long) invalid_pte_table);
}

/*
 * Empty pgd entries point to the invalid_pmd_table.
 */
static inline int pgd_none(pgd_t pgd)
{
	return pgd_val(pgd) == (unsigned long) invalid_pmd_table;
}

#define pgd_bad(pgd)		(pgd_val(pgd) &~ PAGE_MASK)

static inline int pgd_present(pgd_t pgd)
{
	return pgd_val(pgd) != (unsigned long) invalid_pmd_table;
}

static inline void pgd_clear(pgd_t *pgdp)
{
	pgd_val(*pgdp) = ((unsigned long) invalid_pmd_table);
}

#define pte_page(x)		pfn_to_page((unsigned long)((pte_val(x) >> PAGE_SHIFT)))
#ifdef CONFIG_CPU_VR41XX
#define pte_pfn(x)		((unsigned long)((x).pte >> (PAGE_SHIFT + 2)))
#define pfn_pte(pfn, prot)	__pte(((pfn) << (PAGE_SHIFT + 2)) | pgprot_val(prot))
#else
#define pte_pfn(x)		((unsigned long)((x).pte >> PAGE_SHIFT))
#define pfn_pte(pfn, prot)	__pte(((pfn) << PAGE_SHIFT) | pgprot_val(prot))
#endif

#define __pgd_offset(address)	pgd_index(address)
#define page_pte(page) page_pte_prot(page, __pgprot(0))

/* to find an entry in a kernel page-table-directory */
#define pgd_offset_k(address) pgd_offset(&init_mm, 0)

#define pgd_index(address)		((address) >> PGDIR_SHIFT)

/* to find an entry in a page-table-directory */
#define pgd_offset(mm,addr)	((mm)->pgd + pgd_index(addr))

static inline unsigned long pgd_page(pgd_t pgd)
{
	return pgd_val(pgd);
}

/* Find an entry in the second-level page table.. */
static inline pmd_t *pmd_offset(pgd_t * dir, unsigned long address)
{
	return (pmd_t *) pgd_page(*dir) +
	       ((address >> PMD_SHIFT) & (PTRS_PER_PMD - 1));
}

/* Find an entry in the third-level page table.. */
#define __pte_offset(address)						\
	(((address) >> PAGE_SHIFT) & (PTRS_PER_PTE - 1))
#define pte_offset(dir, address)					\
	((pte_t *) (pmd_page_kernel(*dir)) + __pte_offset(address))
#define pte_offset_kernel(dir, address)					\
	((pte_t *) pmd_page_kernel(*(dir)) +  __pte_offset(address))
#define pte_offset_map(dir, address)					\
	((pte_t *)page_address(pmd_page(*(dir))) + __pte_offset(address))
#define pte_offset_map_nested(dir, address)				\
	((pte_t *)page_address(pmd_page(*(dir))) + __pte_offset(address))
#define pte_unmap(pte) ((void)(pte))
#define pte_unmap_nested(pte) ((void)(pte))

/*
 * Initialize a new pgd / pmd table with invalid pointers.
 */
extern void pgd_init(unsigned long page);
extern void pmd_init(unsigned long page, unsigned long pagetable);

/*
 * Non-present pages:  high 24 bits are offset, next 8 bits type,
 * low 32 bits zero.
 */
static inline pte_t mk_swap_pte(unsigned long type, unsigned long offset)
{ pte_t pte; pte_val(pte) = (type << 32) | (offset << 40); return pte; }

#define __swp_type(x)		(((x).val >> 32) & 0xff)
#define __swp_offset(x)		((x).val >> 40)
#define __swp_entry(type,offset) ((swp_entry_t) { pte_val(mk_swap_pte((type),(offset))) })
#define __pte_to_swp_entry(pte)	((swp_entry_t) { pte_val(pte) })
#define __swp_entry_to_pte(x)	((pte_t) { (x).val })

/*
 * Bits 0, 1, 2, 7 and 8 are taken, split up the 32 bits of offset
 * into this range:
 */
#define PTE_FILE_MAX_BITS	32

#define pte_to_pgoff(_pte) \
	((((_pte).pte >> 3) & 0x1f ) + (((_pte).pte >> 9) << 6 ))

#define pgoff_to_pte(off) \
	((pte_t) { (((off) & 0x1f) << 3) + (((off) >> 6) << 9) + _PAGE_FILE })

/*
 * Used for the b0rked handling of kernel pagetables on the 64-bit kernel.
 */
extern pte_t kptbl[(PAGE_SIZE << PGD_ORDER)/sizeof(pte_t)];
extern pmd_t kpmdtbl[PTRS_PER_PMD];

#endif /* _ASM_PGTABLE_64_H */
