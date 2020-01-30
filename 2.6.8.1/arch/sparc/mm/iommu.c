/*
 * iommu.c:  IOMMU specific routines for memory management.
 *
 * Copyright (C) 1995 David S. Miller  (davem@caip.rutgers.edu)
 * Copyright (C) 1995,2002 Pete Zaitcev     (zaitcev@yahoo.com)
 * Copyright (C) 1996 Eddie C. Dost    (ecd@skynet.be)
 * Copyright (C) 1997,1998 Jakub Jelinek    (jj@sunsite.mff.cuni.cz)
 */
 
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/highmem.h>	/* pte_offset_map => kmap_atomic */

#include <asm/scatterlist.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/sbus.h>
#include <asm/io.h>
#include <asm/mxcc.h>
#include <asm/mbus.h>
#include <asm/cacheflush.h>
#include <asm/tlbflush.h>
#include <asm/bitext.h>
#include <asm/iommu.h>
#include <asm/dma.h>

/*
 * This can be sized dynamically, but we will do this
 * only when we have a guidance about actual I/O pressures.
 */
#define IOMMU_RNGE	IOMMU_RNGE_256MB
#define IOMMU_START	0xF0000000
#define IOMMU_WINSIZE	(256*1024*1024U)
#define IOMMU_NPTES	(IOMMU_WINSIZE/PAGE_SIZE)	/* 64K PTEs, 265KB */
#define IOMMU_ORDER	6				/* 4096 * (1<<6) */

/* srmmu.c */
extern int viking_mxcc_present;
BTFIXUPDEF_CALL(void, flush_page_for_dma, unsigned long)
#define flush_page_for_dma(page) BTFIXUP_CALL(flush_page_for_dma)(page)
extern int flush_page_for_dma_global;
static int viking_flush;
/* viking.S */
extern void viking_flush_page(unsigned long page);
extern void viking_mxcc_flush_page(unsigned long page);

/*
 * Values precomputed according to CPU type.
 */
static unsigned int ioperm_noc;		/* Consistent mapping iopte flags */
static pgprot_t dvma_prot;		/* Consistent mapping pte flags */

#define IOPERM        (IOPTE_CACHE | IOPTE_WRITE | IOPTE_VALID)
#define MKIOPTE(pfn, perm) (((((pfn)<<8) & IOPTE_PAGE) | (perm)) & ~IOPTE_WAZ)

void __init
iommu_init(int iommund, struct sbus_bus *sbus)
{
	unsigned int impl, vers;
	unsigned long tmp;
	struct iommu_struct *iommu;
	struct linux_prom_registers iommu_promregs[PROMREG_MAX];
	struct resource r;
	unsigned long *bitmap;

	iommu = kmalloc(sizeof(struct iommu_struct), GFP_ATOMIC);
	if (!iommu) {
		prom_printf("Unable to allocate iommu structure\n");
		prom_halt();
	}
	prom_getproperty(iommund, "reg", (void *) iommu_promregs,
			 sizeof(iommu_promregs));
	memset(&r, 0, sizeof(r));
	r.flags = iommu_promregs[0].which_io;
	r.start = iommu_promregs[0].phys_addr;
	iommu->regs = (struct iommu_regs *)
		sbus_ioremap(&r, 0, PAGE_SIZE * 3, "iommu_regs");
	if(!iommu->regs) {
		prom_printf("Cannot map IOMMU registers\n");
		prom_halt();
	}
	impl = (iommu->regs->control & IOMMU_CTRL_IMPL) >> 28;
	vers = (iommu->regs->control & IOMMU_CTRL_VERS) >> 24;
	tmp = iommu->regs->control;
	tmp &= ~(IOMMU_CTRL_RNGE);
	tmp |= (IOMMU_RNGE_256MB | IOMMU_CTRL_ENAB);
	iommu->regs->control = tmp;
	iommu_invalidate(iommu->regs);
	iommu->start = IOMMU_START;
	iommu->end = 0xffffffff;

	/* Allocate IOMMU page table */
	/* Stupid alignment constraints give me a headache. 
	   We need 256K or 512K or 1M or 2M area aligned to
           its size and current gfp will fortunately give
           it to us. */
        tmp = __get_free_pages(GFP_KERNEL, IOMMU_ORDER);
	if (!tmp) {
		prom_printf("Unable to allocate iommu table [0x%08x]\n",
			    IOMMU_NPTES*sizeof(iopte_t));
		prom_halt();
	}
	iommu->page_table = (iopte_t *)tmp;

	/* Initialize new table. */
	memset(iommu->page_table, 0, IOMMU_NPTES*sizeof(iopte_t));
	flush_cache_all();
	flush_tlb_all();
	iommu->regs->base = __pa((unsigned long) iommu->page_table) >> 4;
	iommu_invalidate(iommu->regs);

	bitmap = kmalloc(IOMMU_NPTES>>3, GFP_KERNEL);
	if (!bitmap) {
		prom_printf("Unable to allocate iommu bitmap [%d]\n",
			    (int)(IOMMU_NPTES>>3));
		prom_halt();
	}
	bit_map_init(&iommu->usemap, bitmap, IOMMU_NPTES);

	printk("IOMMU: impl %d vers %d table 0x%p[%d B] map [%d b]\n",
	    impl, vers, iommu->page_table,
	    (int)(IOMMU_NPTES*sizeof(iopte_t)), (int)IOMMU_NPTES);

	sbus->iommu = iommu;
}

/* This begs to be btfixup-ed by srmmu. */
static void iommu_viking_flush_iotlb(iopte_t *iopte, unsigned int niopte)
{
	unsigned long start;
	unsigned long end;

	start = (unsigned long)iopte & PAGE_MASK;
	end = PAGE_ALIGN(start + niopte*sizeof(iopte_t));
	if (viking_mxcc_present) {
		while(start < end) {
			viking_mxcc_flush_page(start);
			start += PAGE_SIZE;
		}
	} else if (viking_flush) {
		while(start < end) {
			viking_flush_page(start);
			start += PAGE_SIZE;
		}
	}
}

static u32 iommu_get_one(struct page *page, int npages, struct sbus_bus *sbus)
{
	struct iommu_struct *iommu = sbus->iommu;
	int ioptex;
	iopte_t *iopte, *iopte0;
	unsigned int busa, busa0;
	int i;

	ioptex = bit_map_string_get(&iommu->usemap, npages, 1);
	if (ioptex < 0)
		panic("iommu out");
	busa0 = iommu->start + (ioptex << PAGE_SHIFT);
	iopte0 = &iommu->page_table[ioptex];

	busa = busa0;
	iopte = iopte0;
	for (i = 0; i < npages; i++) {
		iopte_val(*iopte) = MKIOPTE(page_to_pfn(page), IOPERM);
		iommu_invalidate_page(iommu->regs, busa);
		busa += PAGE_SIZE;
		iopte++;
		page++;
	}

	iommu_viking_flush_iotlb(iopte0, npages);

	return busa0;
}

static u32 iommu_get_scsi_one(char *vaddr, unsigned int len,
    struct sbus_bus *sbus)
{
	unsigned long off;
	int npages;
	struct page *page;
	u32 busa;

	off = (unsigned long)vaddr & ~PAGE_MASK;
	npages = (off + len + PAGE_SIZE-1) >> PAGE_SHIFT;
	page = virt_to_page((unsigned long)vaddr & PAGE_MASK);
	busa = iommu_get_one(page, npages, sbus);
	return busa + off;
}

static __u32 iommu_get_scsi_one_noflush(char *vaddr, unsigned long len, struct sbus_bus *sbus)
{
	return iommu_get_scsi_one(vaddr, len, sbus);
}

static __u32 iommu_get_scsi_one_gflush(char *vaddr, unsigned long len, struct sbus_bus *sbus)
{
	flush_page_for_dma(0);
	return iommu_get_scsi_one(vaddr, len, sbus);
}

static __u32 iommu_get_scsi_one_pflush(char *vaddr, unsigned long len, struct sbus_bus *sbus)
{
	unsigned long page = ((unsigned long) vaddr) & PAGE_MASK;

	while(page < ((unsigned long)(vaddr + len))) {
		flush_page_for_dma(page);
		page += PAGE_SIZE;
	}
	return iommu_get_scsi_one(vaddr, len, sbus);
}

static void iommu_get_scsi_sgl_noflush(struct scatterlist *sg, int sz, struct sbus_bus *sbus)
{
	int n;

	while (sz != 0) {
		--sz;
		n = (sg->length + sg->offset + PAGE_SIZE-1) >> PAGE_SHIFT;
		sg->dvma_address = iommu_get_one(sg->page, n, sbus) + sg->offset;
		sg->dvma_length = (__u32) sg->length;
		sg++;
	}
}

static void iommu_get_scsi_sgl_gflush(struct scatterlist *sg, int sz, struct sbus_bus *sbus)
{
	int n;

	flush_page_for_dma(0);
	while (sz != 0) {
		--sz;
		n = (sg->length + sg->offset + PAGE_SIZE-1) >> PAGE_SHIFT;
		sg->dvma_address = iommu_get_one(sg->page, n, sbus) + sg->offset;
		sg->dvma_length = (__u32) sg->length;
		sg++;
	}
}

static void iommu_get_scsi_sgl_pflush(struct scatterlist *sg, int sz, struct sbus_bus *sbus)
{
	unsigned long page, oldpage = 0;
	int n, i;

	while(sz != 0) {
		--sz;

		n = (sg->length + sg->offset + PAGE_SIZE-1) >> PAGE_SHIFT;

		/*
		 * We expect unmapped highmem pages to be not in the cache.
		 * XXX Is this a good assumption?
		 * XXX What if someone else unmaps it here and races us?
		 */
		if ((page = (unsigned long) page_address(sg->page)) != 0) {
			for (i = 0; i < n; i++) {
				if (page != oldpage) {	/* Already flushed? */
					flush_page_for_dma(page);
					oldpage = page;
				}
				page += PAGE_SIZE;
			}
		}

		sg->dvma_address = iommu_get_one(sg->page, n, sbus) + sg->offset;
		sg->dvma_length = (__u32) sg->length;
		sg++;
	}
}

static void iommu_release_one(u32 busa, int npages, struct sbus_bus *sbus)
{
	struct iommu_struct *iommu = sbus->iommu;
	int ioptex;
	int i;

	if (busa < iommu->start)
		BUG();
	ioptex = (busa - iommu->start) >> PAGE_SHIFT;
	for (i = 0; i < npages; i++) {
		iopte_val(iommu->page_table[ioptex + i]) = 0;
		iommu_invalidate_page(iommu->regs, busa);
		busa += PAGE_SIZE;
	}
	bit_map_clear(&iommu->usemap, ioptex, npages);
}

static void iommu_release_scsi_one(__u32 vaddr, unsigned long len, struct sbus_bus *sbus)
{
	unsigned long off;
	int npages;

	off = vaddr & ~PAGE_MASK;
	npages = (off + len + PAGE_SIZE-1) >> PAGE_SHIFT;
	iommu_release_one(vaddr & PAGE_MASK, npages, sbus);
}

static void iommu_release_scsi_sgl(struct scatterlist *sg, int sz, struct sbus_bus *sbus)
{
	int n;

	while(sz != 0) {
		--sz;

		n = (sg->length + sg->offset + PAGE_SIZE-1) >> PAGE_SHIFT;
		iommu_release_one(sg->dvma_address & PAGE_MASK, n, sbus);
		sg->dvma_address = 0x21212121;
		sg++;
	}
}

#ifdef CONFIG_SBUS
static int iommu_map_dma_area(dma_addr_t *pba, unsigned long va,
    unsigned long addr, int len)
{
	unsigned long page, end;
	struct iommu_struct *iommu = sbus_root->iommu;
	iopte_t *iopte = iommu->page_table;
	iopte_t *first;
	int ioptex;

	if ((va & ~PAGE_MASK) != 0) BUG();
	if ((addr & ~PAGE_MASK) != 0) BUG();
	if ((len & ~PAGE_MASK) != 0) BUG();

	ioptex = bit_map_string_get(&iommu->usemap, len >> PAGE_SHIFT, 1);
	if (ioptex < 0)
		panic("iommu out");

	iopte += ioptex;
	first = iopte;
	end = addr + len;
	while(addr < end) {
		page = va;
		{
			pgd_t *pgdp;
			pmd_t *pmdp;
			pte_t *ptep;

			if (viking_mxcc_present)
				viking_mxcc_flush_page(page);
			else if (viking_flush)
				viking_flush_page(page);
			else
				__flush_page_to_ram(page);

			pgdp = pgd_offset(&init_mm, addr);
			pmdp = pmd_offset(pgdp, addr);
			ptep = pte_offset_map(pmdp, addr);

			set_pte(ptep, mk_pte(virt_to_page(page), dvma_prot));
		}
		iopte_val(*iopte++) =
		    MKIOPTE(page_to_pfn(virt_to_page(page)), ioperm_noc);
		addr += PAGE_SIZE;
		va += PAGE_SIZE;
	}
	/* P3: why do we need this?
	 *
	 * DAVEM: Because there are several aspects, none of which
	 *        are handled by a single interface.  Some cpus are
	 *        completely not I/O DMA coherent, and some have
	 *        virtually indexed caches.  The driver DMA flushing
	 *        methods handle the former case, but here during
	 *        IOMMU page table modifications, and usage of non-cacheable
	 *        cpu mappings of pages potentially in the cpu caches, we have
	 *        to handle the latter case as well.
	 */
	flush_cache_all();
	iommu_viking_flush_iotlb(first, len >> PAGE_SHIFT);
	flush_tlb_all();
	iommu_invalidate(iommu->regs);

	*pba = iommu->start + (ioptex << PAGE_SHIFT);
	return 0;
}

static void iommu_unmap_dma_area(unsigned long busa, int len)
{
	struct iommu_struct *iommu = sbus_root->iommu;
	iopte_t *iopte = iommu->page_table;
	unsigned long end;
	int ioptex = (busa - iommu->start) >> PAGE_SHIFT;

	if ((busa & ~PAGE_MASK) != 0) BUG();
	if ((len & ~PAGE_MASK) != 0) BUG();

	iopte += ioptex;
	end = busa + len;
	while (busa < end) {
		iopte_val(*iopte++) = 0;
		busa += PAGE_SIZE;
	}
	flush_tlb_all();
	iommu_invalidate(iommu->regs);
	bit_map_clear(&iommu->usemap, ioptex, len >> PAGE_SHIFT);
}

static struct page *iommu_translate_dvma(unsigned long busa)
{
	struct iommu_struct *iommu = sbus_root->iommu;
	iopte_t *iopte = iommu->page_table;

	iopte += ((busa - iommu->start) >> PAGE_SHIFT);
	return pfn_to_page((iopte_val(*iopte) & IOPTE_PAGE) >> (PAGE_SHIFT-4));
}
#endif

static char *iommu_lockarea(char *vaddr, unsigned long len)
{
	return vaddr;
}

static void iommu_unlockarea(char *vaddr, unsigned long len)
{
}

void __init ld_mmu_iommu(void)
{
	viking_flush = (BTFIXUPVAL_CALL(flush_page_for_dma) == (unsigned long)viking_flush_page);
	BTFIXUPSET_CALL(mmu_lockarea, iommu_lockarea, BTFIXUPCALL_RETO0);
	BTFIXUPSET_CALL(mmu_unlockarea, iommu_unlockarea, BTFIXUPCALL_NOP);

	if (!BTFIXUPVAL_CALL(flush_page_for_dma)) {
		/* IO coherent chip */
		BTFIXUPSET_CALL(mmu_get_scsi_one, iommu_get_scsi_one_noflush, BTFIXUPCALL_RETO0);
		BTFIXUPSET_CALL(mmu_get_scsi_sgl, iommu_get_scsi_sgl_noflush, BTFIXUPCALL_NORM);
	} else if (flush_page_for_dma_global) {
		/* flush_page_for_dma flushes everything, no matter of what page is it */
		BTFIXUPSET_CALL(mmu_get_scsi_one, iommu_get_scsi_one_gflush, BTFIXUPCALL_NORM);
		BTFIXUPSET_CALL(mmu_get_scsi_sgl, iommu_get_scsi_sgl_gflush, BTFIXUPCALL_NORM);
	} else {
		BTFIXUPSET_CALL(mmu_get_scsi_one, iommu_get_scsi_one_pflush, BTFIXUPCALL_NORM);
		BTFIXUPSET_CALL(mmu_get_scsi_sgl, iommu_get_scsi_sgl_pflush, BTFIXUPCALL_NORM);
	}
	BTFIXUPSET_CALL(mmu_release_scsi_one, iommu_release_scsi_one, BTFIXUPCALL_NORM);
	BTFIXUPSET_CALL(mmu_release_scsi_sgl, iommu_release_scsi_sgl, BTFIXUPCALL_NORM);

#ifdef CONFIG_SBUS
	BTFIXUPSET_CALL(mmu_map_dma_area, iommu_map_dma_area, BTFIXUPCALL_NORM);
	BTFIXUPSET_CALL(mmu_unmap_dma_area, iommu_unmap_dma_area, BTFIXUPCALL_NORM);
	BTFIXUPSET_CALL(mmu_translate_dvma, iommu_translate_dvma, BTFIXUPCALL_NORM);
#endif

	if (viking_mxcc_present || srmmu_modtype == HyperSparc) {
		dvma_prot = __pgprot(SRMMU_CACHE | SRMMU_ET_PTE | SRMMU_PRIV);
		ioperm_noc = IOPTE_CACHE | IOPTE_WRITE | IOPTE_VALID;
	} else {
		dvma_prot = __pgprot(SRMMU_ET_PTE | SRMMU_PRIV);
		ioperm_noc = IOPTE_WRITE | IOPTE_VALID;
	}
}
