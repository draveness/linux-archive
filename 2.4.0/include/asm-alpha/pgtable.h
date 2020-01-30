#ifndef _ALPHA_PGTABLE_H
#define _ALPHA_PGTABLE_H

/*
 * This file contains the functions and defines necessary to modify and use
 * the Alpha page table tree.
 *
 * This hopefully works with any standard Alpha page-size, as defined
 * in <asm/page.h> (currently 8192).
 */
#include <linux/config.h>

#include <asm/page.h>
#include <asm/processor.h>	/* For TASK_SIZE */
#include <asm/machvec.h>

/* Certain architectures need to do special things when PTEs
 * within a page table are directly modified.  Thus, the following
 * hook is made available.
 */
#define set_pte(pteptr, pteval) ((*(pteptr)) = (pteval))

/* PMD_SHIFT determines the size of the area a second-level page table can map */
#define PMD_SHIFT	(PAGE_SHIFT + (PAGE_SHIFT-3))
#define PMD_SIZE	(1UL << PMD_SHIFT)
#define PMD_MASK	(~(PMD_SIZE-1))

/* PGDIR_SHIFT determines what a third-level page table entry can map */
#define PGDIR_SHIFT	(PAGE_SHIFT + 2*(PAGE_SHIFT-3))
#define PGDIR_SIZE	(1UL << PGDIR_SHIFT)
#define PGDIR_MASK	(~(PGDIR_SIZE-1))

/*
 * Entries per page directory level:  the Alpha is three-level, with
 * all levels having a one-page page table.
 */
#define PTRS_PER_PTE	(1UL << (PAGE_SHIFT-3))
#define PTRS_PER_PMD	(1UL << (PAGE_SHIFT-3))
#define PTRS_PER_PGD	(1UL << (PAGE_SHIFT-3))
#define USER_PTRS_PER_PGD	(TASK_SIZE / PGDIR_SIZE)
#define FIRST_USER_PGD_NR	0

/* Number of pointers that fit on a page:  this will go away. */
#define PTRS_PER_PAGE	(1UL << (PAGE_SHIFT-3))

#ifdef CONFIG_ALPHA_LARGE_VMALLOC
#define VMALLOC_START		0xfffffe0000000000
#else
#define VMALLOC_START		(-2*PGDIR_SIZE)
#endif
#define VMALLOC_VMADDR(x)	((unsigned long)(x))
#define VMALLOC_END		(-PGDIR_SIZE)

/*
 * OSF/1 PAL-code-imposed page table bits
 */
#define _PAGE_VALID	0x0001
#define _PAGE_FOR	0x0002	/* used for page protection (fault on read) */
#define _PAGE_FOW	0x0004	/* used for page protection (fault on write) */
#define _PAGE_FOE	0x0008	/* used for page protection (fault on exec) */
#define _PAGE_ASM	0x0010
#if defined(CONFIG_ALPHA_EV6) && !defined(CONFIG_SMP)
#define _PAGE_MBE	0x0080	/* MB disable bit for EV6.  */
#else
#define _PAGE_MBE	0x0000
#endif
#define _PAGE_KRE	0x0100	/* xxx - see below on the "accessed" bit */
#define _PAGE_URE	0x0200	/* xxx */
#define _PAGE_KWE	0x1000	/* used to do the dirty bit in software */
#define _PAGE_UWE	0x2000	/* used to do the dirty bit in software */

/* .. and these are ours ... */
#define _PAGE_DIRTY	0x20000
#define _PAGE_ACCESSED	0x40000

/*
 * NOTE! The "accessed" bit isn't necessarily exact:  it can be kept exactly
 * by software (use the KRE/URE/KWE/UWE bits appropriately), but I'll fake it.
 * Under Linux/AXP, the "accessed" bit just means "read", and I'll just use
 * the KRE/URE bits to watch for it. That way we don't need to overload the
 * KWE/UWE bits with both handling dirty and accessed.
 *
 * Note that the kernel uses the accessed bit just to check whether to page
 * out a page or not, so it doesn't have to be exact anyway.
 */

#define __DIRTY_BITS	(_PAGE_DIRTY | _PAGE_KWE | _PAGE_UWE)
#define __ACCESS_BITS	(_PAGE_ACCESSED | _PAGE_KRE | _PAGE_URE)

#define _PFN_MASK	0xFFFFFFFF00000000

#define _PAGE_TABLE	(_PAGE_VALID | __DIRTY_BITS | __ACCESS_BITS)
#define _PAGE_CHG_MASK	(_PFN_MASK | __DIRTY_BITS | __ACCESS_BITS | _PAGE_MBE)

/*
 * All the normal masks have the "page accessed" bits on, as any time they
 * are used, the page is accessed.  They are cleared only by the page-out
 * routines. 
 */
#define PAGE_NONE	__pgprot(_PAGE_VALID | __ACCESS_BITS | _PAGE_FOR | _PAGE_FOW | _PAGE_FOE)
#define PAGE_SHARED	__pgprot(_PAGE_VALID | __ACCESS_BITS)
#define PAGE_COPY	__pgprot(_PAGE_VALID | __ACCESS_BITS | _PAGE_FOW)
#define PAGE_READONLY	__pgprot(_PAGE_VALID | __ACCESS_BITS | _PAGE_FOW)
#define PAGE_KERNEL	__pgprot(_PAGE_VALID | _PAGE_ASM | _PAGE_KRE | _PAGE_KWE | _PAGE_MBE)

#define _PAGE_NORMAL(x) __pgprot(_PAGE_VALID | __ACCESS_BITS | _PAGE_MBE | (x))

#define _PAGE_P(x) _PAGE_NORMAL((x) | (((x) & _PAGE_FOW)?0:_PAGE_FOW))
#define _PAGE_S(x) _PAGE_NORMAL(x)

/*
 * The hardware can handle write-only mappings, but as the Alpha
 * architecture does byte-wide writes with a read-modify-write
 * sequence, it's not practical to have write-without-read privs.
 * Thus the "-w- -> rw-" and "-wx -> rwx" mapping here (and in
 * arch/alpha/mm/fault.c)
 */
	/* xwr */
#define __P000	_PAGE_P(_PAGE_FOE | _PAGE_FOW | _PAGE_FOR)
#define __P001	_PAGE_P(_PAGE_FOE | _PAGE_FOW)
#define __P010	_PAGE_P(_PAGE_FOE)
#define __P011	_PAGE_P(_PAGE_FOE)
#define __P100	_PAGE_P(_PAGE_FOW | _PAGE_FOR)
#define __P101	_PAGE_P(_PAGE_FOW)
#define __P110	_PAGE_P(0)
#define __P111	_PAGE_P(0)

#define __S000	_PAGE_S(_PAGE_FOE | _PAGE_FOW | _PAGE_FOR)
#define __S001	_PAGE_S(_PAGE_FOE | _PAGE_FOW)
#define __S010	_PAGE_S(_PAGE_FOE)
#define __S011	_PAGE_S(_PAGE_FOE)
#define __S100	_PAGE_S(_PAGE_FOW | _PAGE_FOR)
#define __S101	_PAGE_S(_PAGE_FOW)
#define __S110	_PAGE_S(0)
#define __S111	_PAGE_S(0)

/*
 * BAD_PAGETABLE is used when we need a bogus page-table, while
 * BAD_PAGE is used for a bogus page.
 *
 * ZERO_PAGE is a global shared page that is always zero:  used
 * for zero-mapped memory areas etc..
 */
extern pte_t __bad_page(void);
extern pmd_t * __bad_pagetable(void);

extern unsigned long __zero_page(void);

#define BAD_PAGETABLE	__bad_pagetable()
#define BAD_PAGE	__bad_page()
#define ZERO_PAGE(vaddr)	(virt_to_page(ZERO_PGE))

/* number of bits that fit into a memory pointer */
#define BITS_PER_PTR			(8*sizeof(unsigned long))

/* to align the pointer to a pointer address */
#define PTR_MASK			(~(sizeof(void*)-1))

/* sizeof(void*)==1<<SIZEOF_PTR_LOG2 */
#define SIZEOF_PTR_LOG2			3

/* to find an entry in a page-table */
#define PAGE_PTR(address)		\
  ((unsigned long)(address)>>(PAGE_SHIFT-SIZEOF_PTR_LOG2)&PTR_MASK&~PAGE_MASK)

/*
 * On certain platforms whose physical address space can overlap KSEG,
 * namely EV6 and above, we must re-twiddle the physaddr to restore the
 * correct high-order bits.
 *
 * This is extremely confusing until you realize that this is actually
 * just working around a userspace bug.  The X server was intending to
 * provide the physical address but instead provided the KSEG address.
 * Or tried to, except it's not representable.
 * 
 * On Tsunami there's nothing meaningful at 0x40000000000, so this is
 * a safe thing to do.  Come the first core logic that does put something
 * in this area -- memory or whathaveyou -- then this hack will have
 * to go away.  So be prepared!
 */

#if defined(CONFIG_ALPHA_GENERIC) && defined(USE_48_BIT_KSEG)
#error "EV6-only feature in a generic kernel"
#endif
#if defined(CONFIG_ALPHA_GENERIC) || \
    (defined(CONFIG_ALPHA_EV6) && !defined(USE_48_BIT_KSEG))
#define PHYS_TWIDDLE(phys) \
  ((((phys) & 0xc0000000000UL) == 0x40000000000UL) \
  ? ((phys) ^= 0xc0000000000UL) : (phys))
#else
#define PHYS_TWIDDLE(phys) (phys)
#endif

/*
 * Conversion functions:  convert a page and protection to a page entry,
 * and a page entry and page directory to the page they refer to.
 */

#define mk_pte(page, pgprot)						\
({									\
	pte_t pte;							\
									\
	pte_val(pte) = ((unsigned long)(page - mem_map) << 32) |	\
		       pgprot_val(pgprot);				\
	pte;								\
})

extern inline pte_t mk_pte_phys(unsigned long physpage, pgprot_t pgprot)
{ pte_t pte; pte_val(pte) = (PHYS_TWIDDLE(physpage) << (32-PAGE_SHIFT)) | (pgprot_val(pgprot) & ~_PAGE_MBE); return pte; }

extern inline pte_t pte_modify(pte_t pte, pgprot_t newprot)
{ pte_val(pte) = (pte_val(pte) & _PAGE_CHG_MASK) | pgprot_val(newprot); return pte; }

extern inline void pmd_set(pmd_t * pmdp, pte_t * ptep)
{ pmd_val(*pmdp) = _PAGE_TABLE | ((((unsigned long) ptep) - PAGE_OFFSET) << (32-PAGE_SHIFT)); }

extern inline void pgd_set(pgd_t * pgdp, pmd_t * pmdp)
{ pgd_val(*pgdp) = _PAGE_TABLE | ((((unsigned long) pmdp) - PAGE_OFFSET) << (32-PAGE_SHIFT)); }

#define pte_page(x)	(mem_map+(unsigned long)((pte_val(x) >> 32)))

extern inline unsigned long pmd_page(pmd_t pmd)
{ return PAGE_OFFSET + ((pmd_val(pmd) & _PFN_MASK) >> (32-PAGE_SHIFT)); }

extern inline unsigned long pgd_page(pgd_t pgd)
{ return PAGE_OFFSET + ((pgd_val(pgd) & _PFN_MASK) >> (32-PAGE_SHIFT)); }

extern inline int pte_none(pte_t pte)		{ return !pte_val(pte); }
extern inline int pte_present(pte_t pte)	{ return pte_val(pte) & _PAGE_VALID; }
extern inline void pte_clear(pte_t *ptep)	{ pte_val(*ptep) = 0; }

extern inline int pmd_none(pmd_t pmd)		{ return !pmd_val(pmd); }
extern inline int pmd_bad(pmd_t pmd)		{ return (pmd_val(pmd) & ~_PFN_MASK) != _PAGE_TABLE; }
extern inline int pmd_present(pmd_t pmd)	{ return pmd_val(pmd) & _PAGE_VALID; }
extern inline void pmd_clear(pmd_t * pmdp)	{ pmd_val(*pmdp) = 0; }

extern inline int pgd_none(pgd_t pgd)		{ return !pgd_val(pgd); }
extern inline int pgd_bad(pgd_t pgd)		{ return (pgd_val(pgd) & ~_PFN_MASK) != _PAGE_TABLE; }
extern inline int pgd_present(pgd_t pgd)	{ return pgd_val(pgd) & _PAGE_VALID; }
extern inline void pgd_clear(pgd_t * pgdp)	{ pgd_val(*pgdp) = 0; }

#define page_address(page)	((page)->virtual)

/*
 * The following only work if pte_present() is true.
 * Undefined behaviour if not..
 */
extern inline int pte_read(pte_t pte)		{ return !(pte_val(pte) & _PAGE_FOR); }
extern inline int pte_write(pte_t pte)		{ return !(pte_val(pte) & _PAGE_FOW); }
extern inline int pte_exec(pte_t pte)		{ return !(pte_val(pte) & _PAGE_FOE); }
extern inline int pte_dirty(pte_t pte)		{ return pte_val(pte) & _PAGE_DIRTY; }
extern inline int pte_young(pte_t pte)		{ return pte_val(pte) & _PAGE_ACCESSED; }

extern inline pte_t pte_wrprotect(pte_t pte)	{ pte_val(pte) |= _PAGE_FOW; return pte; }
extern inline pte_t pte_rdprotect(pte_t pte)	{ pte_val(pte) |= _PAGE_FOR; return pte; }
extern inline pte_t pte_exprotect(pte_t pte)	{ pte_val(pte) |= _PAGE_FOE; return pte; }
extern inline pte_t pte_mkclean(pte_t pte)	{ pte_val(pte) &= ~(__DIRTY_BITS); return pte; }
extern inline pte_t pte_mkold(pte_t pte)	{ pte_val(pte) &= ~(__ACCESS_BITS); return pte; }
extern inline pte_t pte_mkwrite(pte_t pte)	{ pte_val(pte) &= ~_PAGE_FOW; return pte; }
extern inline pte_t pte_mkread(pte_t pte)	{ pte_val(pte) &= ~_PAGE_FOR; return pte; }
extern inline pte_t pte_mkexec(pte_t pte)	{ pte_val(pte) &= ~_PAGE_FOE; return pte; }
extern inline pte_t pte_mkdirty(pte_t pte)	{ pte_val(pte) |= __DIRTY_BITS; return pte; }
extern inline pte_t pte_mkyoung(pte_t pte)	{ pte_val(pte) |= __ACCESS_BITS; return pte; }

#define PAGE_DIR_OFFSET(tsk,address) pgd_offset((tsk),(address))

/* to find an entry in a kernel page-table-directory */
#define pgd_offset_k(address) pgd_offset(&init_mm, address)

/* to find an entry in a page-table-directory. */
#define pgd_index(address)	((address >> PGDIR_SHIFT) & (PTRS_PER_PGD - 1))
#define __pgd_offset(address)	pgd_index(address)
#define pgd_offset(mm, address)	((mm)->pgd+pgd_index(address))

/* Find an entry in the second-level page table.. */
extern inline pmd_t * pmd_offset(pgd_t * dir, unsigned long address)
{
	return (pmd_t *) pgd_page(*dir) + ((address >> PMD_SHIFT) & (PTRS_PER_PAGE - 1));
}

/* Find an entry in the third-level page table.. */
extern inline pte_t * pte_offset(pmd_t * dir, unsigned long address)
{
	return (pte_t *) pmd_page(*dir) + ((address >> PAGE_SHIFT) & (PTRS_PER_PAGE - 1));
}

extern pgd_t swapper_pg_dir[1024];

/*
 * The Alpha doesn't have any external MMU info:  the kernel page
 * tables contain all the necessary information.
 */
extern inline void update_mmu_cache(struct vm_area_struct * vma,
	unsigned long address, pte_t pte)
{
}

/*
 * Non-present pages:  high 24 bits are offset, next 8 bits type,
 * low 32 bits zero.
 */
extern inline pte_t mk_swap_pte(unsigned long type, unsigned long offset)
{ pte_t pte; pte_val(pte) = (type << 32) | (offset << 40); return pte; }

#define SWP_TYPE(x)			(((x).val >> 32) & 0xff)
#define SWP_OFFSET(x)			((x).val >> 40)
#define SWP_ENTRY(type, offset)		((swp_entry_t) { pte_val(mk_swap_pte((type),(offset))) })
#define pte_to_swp_entry(pte)		((swp_entry_t) { pte_val(pte) })
#define swp_entry_to_pte(x)		((pte_t) { (x).val })

/* Needs to be defined here and not in linux/mm.h, as it is arch dependent */
#define PageSkip(page)		(0)
#define kern_addr_valid(addr)	(1)

#define io_remap_page_range(start, busaddr, size, prot) \
	remap_page_range(start, virt_to_phys(__ioremap(busaddr)), size, prot)

#define pte_ERROR(e) \
	printk("%s:%d: bad pte %016lx.\n", __FILE__, __LINE__, pte_val(e))
#define pmd_ERROR(e) \
	printk("%s:%d: bad pmd %016lx.\n", __FILE__, __LINE__, pmd_val(e))
#define pgd_ERROR(e) \
	printk("%s:%d: bad pgd %016lx.\n", __FILE__, __LINE__, pgd_val(e))

extern void paging_init(void);

#include <asm-generic/pgtable.h>

#endif /* _ALPHA_PGTABLE_H */
