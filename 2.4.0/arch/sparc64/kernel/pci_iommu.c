/* $Id: pci_iommu.c,v 1.11 2000/03/10 02:42:15 davem Exp $
 * pci_iommu.c: UltraSparc PCI controller IOM/STC support.
 *
 * Copyright (C) 1999 David S. Miller (davem@redhat.com)
 * Copyright (C) 1999, 2000 Jakub Jelinek (jakub@redhat.com)
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>

#include <asm/pbm.h>

#include "iommu_common.h"

#define PCI_STC_CTXMATCH_ADDR(STC, CTX)	\
	((STC)->strbuf_ctxmatch_base + ((CTX) << 3))

/* Accessing IOMMU and Streaming Buffer registers.
 * REG parameter is a physical address.  All registers
 * are 64-bits in size.
 */
#define pci_iommu_read(__reg) \
({	u64 __ret; \
	__asm__ __volatile__("ldxa [%1] %2, %0" \
			     : "=r" (__ret) \
			     : "r" (__reg), "i" (ASI_PHYS_BYPASS_EC_E) \
			     : "memory"); \
	__ret; \
})
#define pci_iommu_write(__reg, __val) \
	__asm__ __volatile__("stxa %0, [%1] %2" \
			     : /* no outputs */ \
			     : "r" (__val), "r" (__reg), \
			       "i" (ASI_PHYS_BYPASS_EC_E))

/* Must be invoked under the IOMMU lock. */
static void __iommu_flushall(struct pci_iommu *iommu)
{
	unsigned long tag;
	int entry;

	tag = iommu->iommu_flush + (0xa580UL - 0x0210UL);
	for (entry = 0; entry < 16; entry++) {
		pci_iommu_write(tag, 0);
		tag += 8;
	}

	/* Ensure completion of previous PIO writes. */
	(void) pci_iommu_read(iommu->write_complete_reg);

	/* Now update everyone's flush point. */
	for (entry = 0; entry < PBM_NCLUSTERS; entry++) {
		iommu->alloc_info[entry].flush =
			iommu->alloc_info[entry].next;
	}
}

static iopte_t *alloc_streaming_cluster(struct pci_iommu *iommu, unsigned long npages)
{
	iopte_t *iopte, *limit;
	unsigned long cnum, ent, flush_point;

	cnum = 0;
	while ((1UL << cnum) < npages)
		cnum++;
	iopte  = (iommu->page_table +
		  (cnum << (iommu->page_table_sz_bits - PBM_LOGCLUSTERS)));

	if (cnum == 0)
		limit = (iommu->page_table +
			 iommu->lowest_consistent_map);
	else
		limit = (iopte +
			 (1 << (iommu->page_table_sz_bits - PBM_LOGCLUSTERS)));

	iopte += ((ent = iommu->alloc_info[cnum].next) << cnum);
	flush_point = iommu->alloc_info[cnum].flush;
	
	for (;;) {
		if (iopte_val(*iopte) == 0UL) {
			if ((iopte + (1 << cnum)) >= limit)
				ent = 0;
			else
				ent = ent + 1;
			iommu->alloc_info[cnum].next = ent;
			if (ent == flush_point)
				__iommu_flushall(iommu);
			break;
		}
		iopte += (1 << cnum);
		ent++;
		if (iopte >= limit) {
			iopte = (iommu->page_table +
				 (cnum <<
				  (iommu->page_table_sz_bits - PBM_LOGCLUSTERS)));
			ent = 0;
		}
		if (ent == flush_point)
			__iommu_flushall(iommu);
	}

	/* I've got your streaming cluster right here buddy boy... */
	return iopte;
}

static void free_streaming_cluster(struct pci_iommu *iommu, dma_addr_t base,
				   unsigned long npages, unsigned long ctx)
{
	unsigned long cnum, ent;

	cnum = 0;
	while ((1UL << cnum) < npages)
		cnum++;

	ent = (base << (32 - PAGE_SHIFT + PBM_LOGCLUSTERS - iommu->page_table_sz_bits))
		>> (32 + PBM_LOGCLUSTERS + cnum - iommu->page_table_sz_bits);

	/* If the global flush might not have caught this entry,
	 * adjust the flush point such that we will flush before
	 * ever trying to reuse it.
	 */
#define between(X,Y,Z)	(((Z) - (Y)) >= ((X) - (Y)))
	if (between(ent, iommu->alloc_info[cnum].next, iommu->alloc_info[cnum].flush))
		iommu->alloc_info[cnum].flush = ent;
#undef between
}

/* We allocate consistent mappings from the end of cluster zero. */
static iopte_t *alloc_consistent_cluster(struct pci_iommu *iommu, unsigned long npages)
{
	iopte_t *iopte;

	iopte = iommu->page_table + (1 << (iommu->page_table_sz_bits - PBM_LOGCLUSTERS));
	while (iopte > iommu->page_table) {
		iopte--;
		if (!(iopte_val(*iopte) & IOPTE_VALID)) {
			unsigned long tmp = npages;

			while (--tmp) {
				iopte--;
				if (iopte_val(*iopte) & IOPTE_VALID)
					break;
			}
			if (tmp == 0) {
				u32 entry = (iopte - iommu->page_table);

				if (entry < iommu->lowest_consistent_map)
					iommu->lowest_consistent_map = entry;
				return iopte;
			}
		}
	}
	return NULL;
}

#define IOPTE_CONSISTENT(CTX) \
	(IOPTE_VALID | IOPTE_CACHE | \
	 (((CTX) << 47) & IOPTE_CONTEXT))

#define IOPTE_STREAMING(CTX) \
	(IOPTE_CONSISTENT(CTX) | IOPTE_STBUF)

#define IOPTE_INVALID	0UL

/* Allocate and map kernel buffer of size SIZE using consistent mode
 * DMA for PCI device PDEV.  Return non-NULL cpu-side address if
 * successful and set *DMA_ADDRP to the PCI side dma address.
 */
void *pci_alloc_consistent(struct pci_dev *pdev, size_t size, dma_addr_t *dma_addrp)
{
	struct pcidev_cookie *pcp;
	struct pci_iommu *iommu;
	iopte_t *iopte;
	unsigned long flags, order, first_page, ctx;
	void *ret;
	int npages;

	size = PAGE_ALIGN(size);
	order = get_order(size);
	if (order >= 10)
		return NULL;

	first_page = __get_free_pages(GFP_ATOMIC, order);
	if (first_page == 0UL)
		return NULL;
	memset((char *)first_page, 0, PAGE_SIZE << order);

	pcp = pdev->sysdata;
	iommu = &pcp->pbm->parent->iommu;

	spin_lock_irqsave(&iommu->lock, flags);
	iopte = alloc_consistent_cluster(iommu, size >> PAGE_SHIFT);
	if (iopte == NULL) {
		spin_unlock_irqrestore(&iommu->lock, flags);
		free_pages(first_page, order);
		return NULL;
	}

	*dma_addrp = (iommu->page_table_map_base +
		      ((iopte - iommu->page_table) << PAGE_SHIFT));
	ret = (void *) first_page;
	npages = size >> PAGE_SHIFT;
	ctx = 0;
	if (iommu->iommu_ctxflush)
		ctx = iommu->iommu_cur_ctx++;
	first_page = __pa(first_page);
	while (npages--) {
		iopte_val(*iopte) = (IOPTE_CONSISTENT(ctx) |
				     IOPTE_WRITE |
				     (first_page & IOPTE_PAGE));
		iopte++;
		first_page += PAGE_SIZE;
	}

	if (iommu->iommu_ctxflush) {
		pci_iommu_write(iommu->iommu_ctxflush, ctx);
	} else {
		int i;
		u32 daddr = *dma_addrp;

		npages = size >> PAGE_SHIFT;
		for (i = 0; i < npages; i++) {
			pci_iommu_write(iommu->iommu_flush, daddr);
			daddr += PAGE_SIZE;
		}
	}

	spin_unlock_irqrestore(&iommu->lock, flags);

	return ret;
}

/* Free and unmap a consistent DMA translation. */
void pci_free_consistent(struct pci_dev *pdev, size_t size, void *cpu, dma_addr_t dvma)
{
	struct pcidev_cookie *pcp;
	struct pci_iommu *iommu;
	iopte_t *iopte;
	unsigned long flags, order, npages, i, ctx;

	npages = PAGE_ALIGN(size) >> PAGE_SHIFT;
	pcp = pdev->sysdata;
	iommu = &pcp->pbm->parent->iommu;
	iopte = iommu->page_table +
		((dvma - iommu->page_table_map_base) >> PAGE_SHIFT);

	spin_lock_irqsave(&iommu->lock, flags);

	if ((iopte - iommu->page_table) ==
	    iommu->lowest_consistent_map) {
		iopte_t *walk = iopte + npages;
		iopte_t *limit;

		limit = (iommu->page_table +
			 (1 << (iommu->page_table_sz_bits - PBM_LOGCLUSTERS)));
		while (walk < limit) {
			if (iopte_val(*walk) != IOPTE_INVALID)
				break;
			walk++;
		}
		iommu->lowest_consistent_map =
			(walk - iommu->page_table);
	}

	/* Data for consistent mappings cannot enter the streaming
	 * buffers, so we only need to update the TSB.  We flush
	 * the IOMMU here as well to prevent conflicts with the
	 * streaming mapping deferred tlb flush scheme.
	 */

	ctx = 0;
	if (iommu->iommu_ctxflush)
		ctx = (iopte_val(*iopte) & IOPTE_CONTEXT) >> 47UL;

	for (i = 0; i < npages; i++, iopte++)
		iopte_val(*iopte) = IOPTE_INVALID;

	if (iommu->iommu_ctxflush) {
		pci_iommu_write(iommu->iommu_ctxflush, ctx);
	} else {
		for (i = 0; i < npages; i++) {
			u32 daddr = dvma + (i << PAGE_SHIFT);

			pci_iommu_write(iommu->iommu_flush, daddr);
		}
	}

	spin_unlock_irqrestore(&iommu->lock, flags);

	order = get_order(size);
	if (order < 10)
		free_pages((unsigned long)cpu, order);
}

/* Map a single buffer at PTR of SZ bytes for PCI DMA
 * in streaming mode.
 */
dma_addr_t pci_map_single(struct pci_dev *pdev, void *ptr, size_t sz, int direction)
{
	struct pcidev_cookie *pcp;
	struct pci_iommu *iommu;
	struct pci_strbuf *strbuf;
	iopte_t *base;
	unsigned long flags, npages, oaddr;
	unsigned long i, base_paddr, ctx;
	u32 bus_addr, ret;
	unsigned long iopte_protection;

	pcp = pdev->sysdata;
	iommu = &pcp->pbm->parent->iommu;
	strbuf = &pcp->pbm->stc;

	if (direction == PCI_DMA_NONE)
		BUG();

	oaddr = (unsigned long)ptr;
	npages = PAGE_ALIGN(oaddr + sz) - (oaddr & PAGE_MASK);
	npages >>= PAGE_SHIFT;

	spin_lock_irqsave(&iommu->lock, flags);

	base = alloc_streaming_cluster(iommu, npages);
	bus_addr = (iommu->page_table_map_base +
		    ((base - iommu->page_table) << PAGE_SHIFT));
	ret = bus_addr | (oaddr & ~PAGE_MASK);
	base_paddr = __pa(oaddr & PAGE_MASK);
	ctx = 0;
	if (iommu->iommu_ctxflush)
		ctx = iommu->iommu_cur_ctx++;
	if (strbuf->strbuf_enabled)
		iopte_protection = IOPTE_STREAMING(ctx);
	else
		iopte_protection = IOPTE_CONSISTENT(ctx);
	if (direction != PCI_DMA_TODEVICE)
		iopte_protection |= IOPTE_WRITE;

	for (i = 0; i < npages; i++, base++, base_paddr += PAGE_SIZE)
		iopte_val(*base) = iopte_protection | base_paddr;

	spin_unlock_irqrestore(&iommu->lock, flags);

	return ret;
}

/* Unmap a single streaming mode DMA translation. */
void pci_unmap_single(struct pci_dev *pdev, dma_addr_t bus_addr, size_t sz, int direction)
{
	struct pcidev_cookie *pcp;
	struct pci_iommu *iommu;
	struct pci_strbuf *strbuf;
	iopte_t *base;
	unsigned long flags, npages, i, ctx;

	if (direction == PCI_DMA_NONE)
		BUG();

	pcp = pdev->sysdata;
	iommu = &pcp->pbm->parent->iommu;
	strbuf = &pcp->pbm->stc;

	npages = PAGE_ALIGN(bus_addr + sz) - (bus_addr & PAGE_MASK);
	npages >>= PAGE_SHIFT;
	base = iommu->page_table +
		((bus_addr - iommu->page_table_map_base) >> PAGE_SHIFT);
#ifdef DEBUG_PCI_IOMMU
	if (iopte_val(*base) == IOPTE_INVALID)
		printk("pci_unmap_single called on non-mapped region %08x,%08x from %016lx\n", bus_addr, sz, __builtin_return_address(0));
#endif
	bus_addr &= PAGE_MASK;

	spin_lock_irqsave(&iommu->lock, flags);

	/* Record the context, if any. */
	ctx = 0;
	if (iommu->iommu_ctxflush)
		ctx = (iopte_val(*base) & IOPTE_CONTEXT) >> 47UL;

	/* Step 1: Kick data out of streaming buffers if necessary. */
	if (strbuf->strbuf_enabled) {
		u32 vaddr = bus_addr;

		PCI_STC_FLUSHFLAG_INIT(strbuf);
		if (strbuf->strbuf_ctxflush &&
		    iommu->iommu_ctxflush) {
			unsigned long matchreg, flushreg;

			flushreg = strbuf->strbuf_ctxflush;
			matchreg = PCI_STC_CTXMATCH_ADDR(strbuf, ctx);
			do {
				pci_iommu_write(flushreg, ctx);
			} while(((long)pci_iommu_read(matchreg)) < 0L);
		} else {
			for (i = 0; i < npages; i++, vaddr += PAGE_SIZE)
				pci_iommu_write(strbuf->strbuf_pflush, vaddr);
		}

		pci_iommu_write(strbuf->strbuf_fsync, strbuf->strbuf_flushflag_pa);
		(void) pci_iommu_read(iommu->write_complete_reg);
		while (!PCI_STC_FLUSHFLAG_SET(strbuf))
			membar("#LoadLoad");
	}

	/* Step 2: Clear out first TSB entry. */
	iopte_val(*base) = IOPTE_INVALID;

	free_streaming_cluster(iommu, bus_addr - iommu->page_table_map_base,
			       npages, ctx);

	spin_unlock_irqrestore(&iommu->lock, flags);
}

static inline void fill_sg(iopte_t *iopte, struct scatterlist *sg, int nused, unsigned long iopte_protection)
{
	struct scatterlist *dma_sg = sg;
	int i;

	for (i = 0; i < nused; i++) {
		unsigned long pteval = ~0UL;
		u32 dma_npages;

		dma_npages = ((dma_sg->dvma_address & (PAGE_SIZE - 1UL)) +
			      dma_sg->dvma_length +
			      ((u32)(PAGE_SIZE - 1UL))) >> PAGE_SHIFT;
		do {
			unsigned long offset;
			signed int len;

			/* If we are here, we know we have at least one
			 * more page to map.  So walk forward until we
			 * hit a page crossing, and begin creating new
			 * mappings from that spot.
			 */
			for (;;) {
				unsigned long tmp;

				tmp = (unsigned long) __pa(sg->address);
				len = sg->length;
				if (((tmp ^ pteval) >> PAGE_SHIFT) != 0UL) {
					pteval = tmp & PAGE_MASK;
					offset = tmp & (PAGE_SIZE - 1UL);
					break;
				}
				if (((tmp ^ (tmp + len - 1UL)) >> PAGE_SHIFT) != 0UL) {
					pteval = (tmp + PAGE_SIZE) & PAGE_MASK;
					offset = 0UL;
					len -= (PAGE_SIZE - (tmp & (PAGE_SIZE - 1UL)));
					break;
				}
				sg++;
			}

			pteval = iopte_protection | (pteval & IOPTE_PAGE);
			while (len > 0) {
				*iopte++ = __iopte(pteval);
				pteval += PAGE_SIZE;
				len -= (PAGE_SIZE - offset);
				offset = 0;
				dma_npages--;
			}

			pteval = (pteval & IOPTE_PAGE) + len;
			sg++;

			/* Skip over any tail mappings we've fully mapped,
			 * adjusting pteval along the way.  Stop when we
			 * detect a page crossing event.
			 */
			while ((pteval << (64 - PAGE_SHIFT)) != 0UL &&
			       pteval == __pa(sg->address) &&
			       ((pteval ^
				 (__pa(sg->address) + sg->length - 1UL)) >> PAGE_SHIFT) == 0UL) {
				pteval += sg->length;
				sg++;
			}
			if ((pteval << (64 - PAGE_SHIFT)) == 0UL)
				pteval = ~0UL;
		} while (dma_npages != 0);
		dma_sg++;
	}
}

/* Map a set of buffers described by SGLIST with NELEMS array
 * elements in streaming mode for PCI DMA.
 * When making changes here, inspect the assembly output. I was having
 * hard time to kepp this routine out of using stack slots for holding variables.
 */
int pci_map_sg(struct pci_dev *pdev, struct scatterlist *sglist, int nelems, int direction)
{
	struct pcidev_cookie *pcp;
	struct pci_iommu *iommu;
	struct pci_strbuf *strbuf;
	unsigned long flags, ctx, npages, iopte_protection;
	iopte_t *base;
	u32 dma_base;
	struct scatterlist *sgtmp;
	int used;

	/* Fast path single entry scatterlists. */
	if (nelems == 1) {
		sglist->dvma_address = pci_map_single(pdev, sglist->address, sglist->length, direction);
		sglist->dvma_length = sglist->length;
		return 1;
	}

	pcp = pdev->sysdata;
	iommu = &pcp->pbm->parent->iommu;
	strbuf = &pcp->pbm->stc;
	
	if (direction == PCI_DMA_NONE)
		BUG();

	/* Step 1: Prepare scatter list. */

	npages = prepare_sg(sglist, nelems);

	/* Step 2: Allocate a cluster. */

	spin_lock_irqsave(&iommu->lock, flags);

	base = alloc_streaming_cluster(iommu, npages);
	dma_base = iommu->page_table_map_base + ((base - iommu->page_table) << PAGE_SHIFT);

	/* Step 3: Normalize DMA addresses. */
	used = nelems;

	sgtmp = sglist;
	while (used && sgtmp->dvma_length) {
		sgtmp->dvma_address += dma_base;
		sgtmp++;
		used--;
	}
	used = nelems - used;

	/* Step 4: Choose a context if necessary. */
	ctx = 0;
	if (iommu->iommu_ctxflush)
		ctx = iommu->iommu_cur_ctx++;

	/* Step 5: Create the mappings. */
	if (strbuf->strbuf_enabled)
		iopte_protection = IOPTE_STREAMING(ctx);
	else
		iopte_protection = IOPTE_CONSISTENT(ctx);
	if (direction != PCI_DMA_TODEVICE)
		iopte_protection |= IOPTE_WRITE;
	fill_sg (base, sglist, used, iopte_protection);
#ifdef VERIFY_SG
	verify_sglist(sglist, nelems, base, npages);
#endif

	spin_unlock_irqrestore(&iommu->lock, flags);

	return used;
}

/* Unmap a set of streaming mode DMA translations. */
void pci_unmap_sg(struct pci_dev *pdev, struct scatterlist *sglist, int nelems, int direction)
{
	struct pcidev_cookie *pcp;
	struct pci_iommu *iommu;
	struct pci_strbuf *strbuf;
	iopte_t *base;
	unsigned long flags, ctx, i, npages;
	u32 bus_addr;

	if (direction == PCI_DMA_NONE)
		BUG();

	pcp = pdev->sysdata;
	iommu = &pcp->pbm->parent->iommu;
	strbuf = &pcp->pbm->stc;
	
	bus_addr = sglist->dvma_address & PAGE_MASK;

	for (i = 1; i < nelems; i++)
		if (sglist[i].dvma_length == 0)
			break;
	i--;
	npages = (PAGE_ALIGN(sglist[i].dvma_address + sglist[i].dvma_length) - bus_addr) >> PAGE_SHIFT;

	base = iommu->page_table +
		((bus_addr - iommu->page_table_map_base) >> PAGE_SHIFT);

#ifdef DEBUG_PCI_IOMMU
	if (iopte_val(*base) == IOPTE_INVALID)
		printk("pci_unmap_sg called on non-mapped region %08x,%d from %016lx\n", sglist->dvma_address, nelems, __builtin_return_address(0));
#endif

	spin_lock_irqsave(&iommu->lock, flags);

	/* Record the context, if any. */
	ctx = 0;
	if (iommu->iommu_ctxflush)
		ctx = (iopte_val(*base) & IOPTE_CONTEXT) >> 47UL;

	/* Step 1: Kick data out of streaming buffers if necessary. */
	if (strbuf->strbuf_enabled) {
		u32 vaddr = bus_addr;

		PCI_STC_FLUSHFLAG_INIT(strbuf);
		if (strbuf->strbuf_ctxflush &&
		    iommu->iommu_ctxflush) {
			unsigned long matchreg, flushreg;

			flushreg = strbuf->strbuf_ctxflush;
			matchreg = PCI_STC_CTXMATCH_ADDR(strbuf, ctx);
			do {
				pci_iommu_write(flushreg, ctx);
			} while(((long)pci_iommu_read(matchreg)) < 0L);
		} else {
			for (i = 0; i < npages; i++, vaddr += PAGE_SIZE)
				pci_iommu_write(strbuf->strbuf_pflush, vaddr);
		}

		pci_iommu_write(strbuf->strbuf_fsync, strbuf->strbuf_flushflag_pa);
		(void) pci_iommu_read(iommu->write_complete_reg);
		while (!PCI_STC_FLUSHFLAG_SET(strbuf))
			membar("#LoadLoad");
	}

	/* Step 2: Clear out first TSB entry. */
	iopte_val(*base) = IOPTE_INVALID;

	free_streaming_cluster(iommu, bus_addr - iommu->page_table_map_base,
			       npages, ctx);

	spin_unlock_irqrestore(&iommu->lock, flags);
}

/* Make physical memory consistent for a single
 * streaming mode DMA translation after a transfer.
 */
void pci_dma_sync_single(struct pci_dev *pdev, dma_addr_t bus_addr, size_t sz, int direction)
{
	struct pcidev_cookie *pcp;
	struct pci_iommu *iommu;
	struct pci_strbuf *strbuf;
	unsigned long flags, ctx, npages;

	pcp = pdev->sysdata;
	iommu = &pcp->pbm->parent->iommu;
	strbuf = &pcp->pbm->stc;

	if (!strbuf->strbuf_enabled)
		return;

	spin_lock_irqsave(&iommu->lock, flags);

	npages = PAGE_ALIGN(bus_addr + sz) - (bus_addr & PAGE_MASK);
	npages >>= PAGE_SHIFT;
	bus_addr &= PAGE_MASK;

	/* Step 1: Record the context, if any. */
	ctx = 0;
	if (iommu->iommu_ctxflush &&
	    strbuf->strbuf_ctxflush) {
		iopte_t *iopte;

		iopte = iommu->page_table +
			((bus_addr - iommu->page_table_map_base)>>PAGE_SHIFT);
		ctx = (iopte_val(*iopte) & IOPTE_CONTEXT) >> 47UL;
	}

	/* Step 2: Kick data out of streaming buffers. */
	PCI_STC_FLUSHFLAG_INIT(strbuf);
	if (iommu->iommu_ctxflush &&
	    strbuf->strbuf_ctxflush) {
		unsigned long matchreg, flushreg;

		flushreg = strbuf->strbuf_ctxflush;
		matchreg = PCI_STC_CTXMATCH_ADDR(strbuf, ctx);
		do {
			pci_iommu_write(flushreg, ctx);
		} while(((long)pci_iommu_read(matchreg)) < 0L);
	} else {
		unsigned long i;

		for (i = 0; i < npages; i++, bus_addr += PAGE_SIZE)
			pci_iommu_write(strbuf->strbuf_pflush, bus_addr);
	}

	/* Step 3: Perform flush synchronization sequence. */
	pci_iommu_write(strbuf->strbuf_fsync, strbuf->strbuf_flushflag_pa);
	(void) pci_iommu_read(iommu->write_complete_reg);
	while (!PCI_STC_FLUSHFLAG_SET(strbuf))
		membar("#LoadLoad");

	spin_unlock_irqrestore(&iommu->lock, flags);
}

/* Make physical memory consistent for a set of streaming
 * mode DMA translations after a transfer.
 */
void pci_dma_sync_sg(struct pci_dev *pdev, struct scatterlist *sglist, int nelems, int direction)
{
	struct pcidev_cookie *pcp;
	struct pci_iommu *iommu;
	struct pci_strbuf *strbuf;
	unsigned long flags, ctx;

	pcp = pdev->sysdata;
	iommu = &pcp->pbm->parent->iommu;
	strbuf = &pcp->pbm->stc;

	if (!strbuf->strbuf_enabled)
		return;

	spin_lock_irqsave(&iommu->lock, flags);

	/* Step 1: Record the context, if any. */
	ctx = 0;
	if (iommu->iommu_ctxflush &&
	    strbuf->strbuf_ctxflush) {
		iopte_t *iopte;

		iopte = iommu->page_table +
			((sglist[0].dvma_address - iommu->page_table_map_base) >> PAGE_SHIFT);
		ctx = (iopte_val(*iopte) & IOPTE_CONTEXT) >> 47UL;
	}

	/* Step 2: Kick data out of streaming buffers. */
	PCI_STC_FLUSHFLAG_INIT(strbuf);
	if (iommu->iommu_ctxflush &&
	    strbuf->strbuf_ctxflush) {
		unsigned long matchreg, flushreg;

		flushreg = strbuf->strbuf_ctxflush;
		matchreg = PCI_STC_CTXMATCH_ADDR(strbuf, ctx);
		do {
			pci_iommu_write(flushreg, ctx);
		} while (((long)pci_iommu_read(matchreg)) < 0L);
	} else {
		unsigned long i, npages;
		u32 bus_addr;

		bus_addr = sglist[0].dvma_address & PAGE_MASK;

		for(i = 1; i < nelems; i++)
			if (!sglist[i].dvma_length)
				break;
		i--;
		npages = (PAGE_ALIGN(sglist[i].dvma_address + sglist[i].dvma_length) - bus_addr) >> PAGE_SHIFT;
		for (i = 0; i < npages; i++, bus_addr += PAGE_SIZE)
			pci_iommu_write(strbuf->strbuf_pflush, bus_addr);
	}

	/* Step 3: Perform flush synchronization sequence. */
	pci_iommu_write(strbuf->strbuf_fsync, strbuf->strbuf_flushflag_pa);
	(void) pci_iommu_read(iommu->write_complete_reg);
	while (!PCI_STC_FLUSHFLAG_SET(strbuf))
		membar("#LoadLoad");

	spin_unlock_irqrestore(&iommu->lock, flags);
}

int pci_dma_supported(struct pci_dev *pdev, dma_addr_t device_mask)
{
	struct pcidev_cookie *pcp = pdev->sysdata;
	u32 dma_addr_mask;

	if (pdev == NULL) {
		dma_addr_mask = 0xffffffff;
	} else {
		struct pci_iommu *iommu = &pcp->pbm->parent->iommu;

		dma_addr_mask = iommu->dma_addr_mask;
	}

	return (device_mask & dma_addr_mask) == dma_addr_mask;
}
