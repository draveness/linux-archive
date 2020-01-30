/* $Id: cache-sh4.c,v 1.26 2004/02/19 12:47:24 lethal Exp $
 *
 *  linux/arch/sh/mm/cache-sh4.c
 *
 * Copyright (C) 1999, 2000, 2002  Niibe Yutaka
 * Copyright (C) 2001, 2002, 2003, 2004  Paul Mundt
 * Copyright (C) 2003  Richard Curnow
 */

#include <linux/config.h>
#include <linux/init.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/threads.h>
#include <asm/addrspace.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/processor.h>
#include <asm/cache.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/pgalloc.h>
#include <asm/mmu_context.h>
#include <asm/cacheflush.h>

extern void __flush_cache_4096_all(unsigned long start);
static void __flush_cache_4096_all_ex(unsigned long start);
extern void __flush_dcache_all(void);
static void __flush_dcache_all_ex(void);

int __init detect_cpu_and_cache_system(void)
{
	unsigned long pvr, prr, cvr;
	unsigned long size;

	static unsigned long sizes[16] = {
		[1] = (1 << 12),
		[2] = (1 << 13),
		[4] = (1 << 14),
		[8] = (1 << 15),
		[9] = (1 << 16)
	};

	pvr = (ctrl_inl(CCN_PVR) >> 8) & 0xffff;
	prr = (ctrl_inl(CCN_PRR) >> 4) & 0xff;
	cvr = (ctrl_inl(CCN_CVR));

	/*
	 * Setup some sane SH-4 defaults for the icache
	 */
	cpu_data->icache.way_incr	= (1 << 13);
	cpu_data->icache.entry_shift	= 5;
	cpu_data->icache.entry_mask	= 0x1fe0;
	cpu_data->icache.sets		= 256;
	cpu_data->icache.ways		= 1;
	cpu_data->icache.linesz		= L1_CACHE_BYTES;

	/*
	 * And again for the dcache ..
	 */
	cpu_data->dcache.way_incr	= (1 << 14);
	cpu_data->dcache.entry_shift	= 5;
	cpu_data->dcache.entry_mask	= 0x3fe0;
	cpu_data->dcache.sets		= 512;
	cpu_data->dcache.ways		= 1;
	cpu_data->dcache.linesz		= L1_CACHE_BYTES;

	/* Set the FPU flag, virtually all SH-4's have one */
	cpu_data->flags |= CPU_HAS_FPU;

	/*
	 * Probe the underlying processor version/revision and
	 * adjust cpu_data setup accordingly.
	 */
	switch (pvr) {
	case 0x205:
		cpu_data->type = CPU_SH7750;
		cpu_data->flags |= CPU_HAS_P2_FLUSH_BUG;
		break;
	case 0x206:
		cpu_data->type = CPU_SH7750S;

		/* 
		 * FIXME: This is needed for 7750, but do we need it for the
		 * 7750S too? For now, assume we do.. -- PFM
		 */
		cpu_data->flags |= CPU_HAS_P2_FLUSH_BUG;

		break;
	case 0x1100:
		cpu_data->type = CPU_SH7751;
		break;
	case 0x8000:
		cpu_data->type = CPU_ST40RA;
		break;
	case 0x8100:
		cpu_data->type = CPU_ST40GX1;
		break;
	case 0x700:
		cpu_data->type = CPU_SH4_501;
		cpu_data->icache.ways = 2;
		cpu_data->dcache.ways = 2;

		/* No FPU on the SH4-500 series.. */
		cpu_data->flags &= ~CPU_HAS_FPU;
		break;
	case 0x600:
		cpu_data->type = CPU_SH4_202;
		cpu_data->icache.ways = 2;
		cpu_data->dcache.ways = 2;
		break;
	case 0x500 ... 0x501:
		switch (prr) {
		    case 0x10: cpu_data->type = CPU_SH7750R; break;
		    case 0x11: cpu_data->type = CPU_SH7751R; break;
		    case 0x50: cpu_data->type = CPU_SH7760;  break;
		}

		cpu_data->icache.ways = 2;
		cpu_data->dcache.ways = 2;

		break;
	default:
		cpu_data->type = CPU_SH_NONE;
		break;
	}

	/*
	 * On anything that's not a direct-mapped cache, look to the CVR
	 * for I/D-cache specifics.
	 */
	if (cpu_data->icache.ways > 1) {
		size = sizes[(cvr >> 20) & 0xf];
		cpu_data->icache.way_incr	= size / cpu_data->icache.ways;
		cpu_data->icache.sets		= (size >> 6);
		cpu_data->icache.entry_mask	=
			((size / cpu_data->icache.ways) - (1 << 5));
	}

	if (cpu_data->dcache.ways > 1) {
		size = sizes[(cvr >> 16) & 0xf];
		cpu_data->dcache.way_incr	= size / cpu_data->dcache.ways;
		cpu_data->dcache.sets		= (size >> 6);
		cpu_data->dcache.entry_mask	=
			((size / cpu_data->dcache.ways) - (1 << 5));
	}

	return 0;
}

/*
 * SH-4 has virtually indexed and physically tagged cache.
 */

struct semaphore p3map_sem[4];

void __init p3_cache_init(void)
{
	if (remap_area_pages(P3SEG, 0, PAGE_SIZE*4, _PAGE_CACHABLE))
		panic("%s failed.", __FUNCTION__);

	sema_init (&p3map_sem[0], 1);
	sema_init (&p3map_sem[1], 1);
	sema_init (&p3map_sem[2], 1);
	sema_init (&p3map_sem[3], 1);
}

/*
 * Write back the dirty D-caches, but not invalidate them.
 *
 * START: Virtual Address (U0, P1, or P3)
 * SIZE: Size of the region.
 */
void __flush_wback_region(void *start, int size)
{
	unsigned long v;
	unsigned long begin, end;

	begin = (unsigned long)start & ~(L1_CACHE_BYTES-1);
	end = ((unsigned long)start + size + L1_CACHE_BYTES-1)
		& ~(L1_CACHE_BYTES-1);
	for (v = begin; v < end; v+=L1_CACHE_BYTES) {
		asm volatile("ocbwb	%0"
			     : /* no output */
			     : "m" (__m(v)));
	}
}

/*
 * Write back the dirty D-caches and invalidate them.
 *
 * START: Virtual Address (U0, P1, or P3)
 * SIZE: Size of the region.
 */
void __flush_purge_region(void *start, int size)
{
	unsigned long v;
	unsigned long begin, end;

	begin = (unsigned long)start & ~(L1_CACHE_BYTES-1);
	end = ((unsigned long)start + size + L1_CACHE_BYTES-1)
		& ~(L1_CACHE_BYTES-1);
	for (v = begin; v < end; v+=L1_CACHE_BYTES) {
		asm volatile("ocbp	%0"
			     : /* no output */
			     : "m" (__m(v)));
	}
}


/*
 * No write back please
 */
void __flush_invalidate_region(void *start, int size)
{
	unsigned long v;
	unsigned long begin, end;

	begin = (unsigned long)start & ~(L1_CACHE_BYTES-1);
	end = ((unsigned long)start + size + L1_CACHE_BYTES-1)
		& ~(L1_CACHE_BYTES-1);
	for (v = begin; v < end; v+=L1_CACHE_BYTES) {
		asm volatile("ocbi	%0"
			     : /* no output */
			     : "m" (__m(v)));
	}
}

static void __flush_dcache_all_ex(void)
{
	unsigned long addr, end_addr, entry_offset;

	end_addr = CACHE_OC_ADDRESS_ARRAY + (cpu_data->dcache.sets << cpu_data->dcache.entry_shift) * cpu_data->dcache.ways;
	entry_offset = 1 << cpu_data->dcache.entry_shift;
	for (addr = CACHE_OC_ADDRESS_ARRAY; addr < end_addr; addr += entry_offset) {
		ctrl_outl(0, addr);
	}
}

static void __flush_cache_4096_all_ex(unsigned long start)
{
	unsigned long addr, entry_offset;
	int i;

	entry_offset = 1 << cpu_data->dcache.entry_shift;
	for (i = 0; i < cpu_data->dcache.ways; i++, start += cpu_data->dcache.way_incr) {
		for (addr = CACHE_OC_ADDRESS_ARRAY + start;
		     addr < CACHE_OC_ADDRESS_ARRAY + 4096 + start;
		     addr += entry_offset) {
			ctrl_outl(0, addr);
		}
	}
}

void flush_cache_4096_all(unsigned long start)
{
	if (cpu_data->dcache.ways == 1)
		__flush_cache_4096_all(start);
	else
		__flush_cache_4096_all_ex(start);
}

/*
 * Write back the range of D-cache, and purge the I-cache.
 *
 * Called from kernel/module.c:sys_init_module and routine for a.out format.
 */
void flush_icache_range(unsigned long start, unsigned long end)
{
	flush_cache_all();
}

/*
 * Write back the D-cache and purge the I-cache for signal trampoline. 
 * .. which happens to be the same behavior as flush_icache_range().
 * So, we simply flush out a line.
 */
void flush_cache_sigtramp(unsigned long addr)
{
	unsigned long v, index;
	unsigned long flags; 
	int i;

	v = addr & ~(L1_CACHE_BYTES-1);
	asm volatile("ocbwb	%0"
		     : /* no output */
		     : "m" (__m(v)));

	index = CACHE_IC_ADDRESS_ARRAY | (v & cpu_data->icache.entry_mask);

	local_irq_save(flags);
	jump_to_P2();
	for(i = 0; i < cpu_data->icache.ways; i++, index += cpu_data->icache.way_incr)
		ctrl_outl(0, index);	/* Clear out Valid-bit */
	back_to_P1();
	local_irq_restore(flags);
}

static inline void flush_cache_4096(unsigned long start,
				    unsigned long phys)
{
	unsigned long flags; 
	extern void __flush_cache_4096(unsigned long addr, unsigned long phys, unsigned long exec_offset);

	/*
	 * SH7751, SH7751R, and ST40 have no restriction to handle cache.
	 * (While SH7750 must do that at P2 area.)
	 */
	if ((cpu_data->flags & CPU_HAS_P2_FLUSH_BUG)
	   || start < CACHE_OC_ADDRESS_ARRAY) {
		local_irq_save(flags);
		__flush_cache_4096(start | SH_CACHE_ASSOC, P1SEGADDR(phys), 0x20000000);
		local_irq_restore(flags);
	} else {
		__flush_cache_4096(start | SH_CACHE_ASSOC, P1SEGADDR(phys), 0);
	}
}

/*
 * Write back & invalidate the D-cache of the page.
 * (To avoid "alias" issues)
 */
void flush_dcache_page(struct page *page)
{
	if (test_bit(PG_mapped, &page->flags)) {
		unsigned long phys = PHYSADDR(page_address(page));

		/* Loop all the D-cache */
		flush_cache_4096(CACHE_OC_ADDRESS_ARRAY,          phys);
		flush_cache_4096(CACHE_OC_ADDRESS_ARRAY | 0x1000, phys);
		flush_cache_4096(CACHE_OC_ADDRESS_ARRAY | 0x2000, phys);
		flush_cache_4096(CACHE_OC_ADDRESS_ARRAY | 0x3000, phys);
	}
}

static inline void flush_icache_all(void)
{
	unsigned long flags, ccr;

	local_irq_save(flags);
	jump_to_P2();

	/* Flush I-cache */
	ccr = ctrl_inl(CCR);
	ccr |= CCR_CACHE_ICI;
	ctrl_outl(ccr, CCR);

	back_to_P1();
	local_irq_restore(flags);
}

void flush_cache_all(void)
{
	if (cpu_data->dcache.ways == 1)
		__flush_dcache_all();
	else
		__flush_dcache_all_ex();
	flush_icache_all();
}

void flush_cache_mm(struct mm_struct *mm)
{
	/* Is there any good way? */
	/* XXX: possibly call flush_cache_range for each vm area */
	/* 
	 * FIXME: Really, the optimal solution here would be able to flush out
	 * individual lines created by the specified context, but this isn't
	 * feasible for a number of architectures (such as MIPS, and some
	 * SPARC) .. is this possible for SuperH?
	 *
	 * In the meantime, we'll just flush all of the caches.. this
	 * seems to be the simplest way to avoid at least a few wasted
	 * cache flushes. -Lethal
	 */
	flush_cache_all();
}

static void __flush_cache_page(struct vm_area_struct *vma,
			       unsigned long address,
			       unsigned long phys)
{
	/* We only need to flush D-cache when we have alias */
	if ((address^phys) & CACHE_ALIAS) {
		/* Loop 4K of the D-cache */
		flush_cache_4096(
			CACHE_OC_ADDRESS_ARRAY | (address & CACHE_ALIAS),
			phys);
		/* Loop another 4K of the D-cache */
		flush_cache_4096(
			CACHE_OC_ADDRESS_ARRAY | (phys & CACHE_ALIAS),
			phys);
	}

	if (vma->vm_flags & VM_EXEC)
		/* Loop 4K (half) of the I-cache */
		flush_cache_4096(
			CACHE_IC_ADDRESS_ARRAY | (address & 0x1000),
			phys);
}

/*
 * Write back and invalidate D-caches.
 *
 * START, END: Virtual Address (U0 address)
 *
 * NOTE: We need to flush the _physical_ page entry.
 * Flushing the cache lines for U0 only isn't enough.
 * We need to flush for P1 too, which may contain aliases.
 */
void flush_cache_range(struct vm_area_struct *vma, unsigned long start,
		       unsigned long end)
{
	unsigned long p = start & PAGE_MASK;
	pgd_t *dir;
	pmd_t *pmd;
	pte_t *pte;
	pte_t entry;
	unsigned long phys;
	unsigned long d = 0;

	dir = pgd_offset(vma->vm_mm, p);
	pmd = pmd_offset(dir, p);

	do {
		if (pmd_none(*pmd) || pmd_bad(*pmd)) {
			p &= ~((1 << PMD_SHIFT) -1);
			p += (1 << PMD_SHIFT);
			pmd++;
			continue;
		}
		pte = pte_offset_kernel(pmd, p);
		do {
			entry = *pte;
			if ((pte_val(entry) & _PAGE_PRESENT)) {
				phys = pte_val(entry)&PTE_PHYS_MASK;
				if ((p^phys) & CACHE_ALIAS) {
					d |= 1 << ((p & CACHE_ALIAS)>>12); 
					d |= 1 << ((phys & CACHE_ALIAS)>>12);
					if (d == 0x0f)
						goto loop_exit;
				}
			}
			pte++;
			p += PAGE_SIZE;
		} while (p < end && ((unsigned long)pte & ~PAGE_MASK));
		pmd++;
	} while (p < end);
 loop_exit:
	if (d & 1)
		flush_cache_4096_all(0);
	if (d & 2)
		flush_cache_4096_all(0x1000);
	if (d & 4)
		flush_cache_4096_all(0x2000);
	if (d & 8)
		flush_cache_4096_all(0x3000);
	if (vma->vm_flags & VM_EXEC)
		flush_icache_all();
}

/*
 * Write back and invalidate I/D-caches for the page.
 *
 * ADDR: Virtual Address (U0 address)
 */
void flush_cache_page(struct vm_area_struct *vma, unsigned long address)
{
	pgd_t *dir;
	pmd_t *pmd;
	pte_t *pte;
	pte_t entry;
	unsigned long phys;

	dir = pgd_offset(vma->vm_mm, address);
	pmd = pmd_offset(dir, address);
	if (pmd_none(*pmd) || pmd_bad(*pmd))
		return;
	pte = pte_offset_kernel(pmd, address);
	entry = *pte;
	if (!(pte_val(entry) & _PAGE_PRESENT))
		return;

	phys = pte_val(entry)&PTE_PHYS_MASK;
	__flush_cache_page(vma, address, phys);
}

/*
 * flush_icache_user_range
 * @vma: VMA of the process
 * @page: page
 * @addr: U0 address
 * @len: length of the range (< page size)
 */
void flush_icache_user_range(struct vm_area_struct *vma,
			     struct page *page, unsigned long addr, int len)
{
	__flush_cache_page(vma, addr, PHYSADDR(page_address(page)));
}

