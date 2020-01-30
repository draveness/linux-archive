/*
 * arch/ppc64/kernel/iommu.c
 * Copyright (C) 2001 Mike Corrigan & Dave Engebretsen, IBM Corporation
 * 
 * Rewrite, cleanup, new allocation schemes, virtual merging: 
 * Copyright (C) 2004 Olof Johansson, IBM Corporation
 *               and  Ben. Herrenschmidt, IBM Corporation
 *
 * Dynamic DMA mapping support, bus-independent parts.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */


#include <linux/config.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <asm/io.h>
#include <asm/prom.h>
#include <asm/iommu.h>
#include <asm/pci-bridge.h>
#include <asm/machdep.h>
#include <asm/bitops.h>

#define DBG(...)

#ifdef CONFIG_IOMMU_VMERGE
static int novmerge = 0;
#else
static int novmerge = 1;
#endif

static int __init setup_iommu(char *str)
{
	if (!strcmp(str, "novmerge"))
		novmerge = 1;
	else if (!strcmp(str, "vmerge"))
		novmerge = 0;
	return 1;
}

__setup("iommu=", setup_iommu);

static unsigned long iommu_range_alloc(struct iommu_table *tbl, unsigned long npages,
				       unsigned long *handle)
{ 
	unsigned long n, end, i, start;
	unsigned long limit;
	int largealloc = npages > 15;
	int pass = 0;

	/* This allocator was derived from x86_64's bit string search */

	/* Sanity check */
	if (unlikely(npages) == 0) {
		if (printk_ratelimit())
			WARN_ON(1);
		return DMA_ERROR_CODE;
	}

	if (handle && *handle)
		start = *handle;
	else
		start = largealloc ? tbl->it_largehint : tbl->it_hint;

	/* Use only half of the table for small allocs (15 pages or less) */
	limit = largealloc ? tbl->it_mapsize : tbl->it_halfpoint;

	if (largealloc && start < tbl->it_halfpoint)
		start = tbl->it_halfpoint;

	/* The case below can happen if we have a small segment appended
	 * to a large, or when the previous alloc was at the very end of
	 * the available space. If so, go back to the initial start.
	 */
	if (start >= limit)
		start = largealloc ? tbl->it_largehint : tbl->it_hint;
	
 again:

	n = find_next_zero_bit(tbl->it_map, limit, start);
	end = n + npages;

	if (unlikely(end >= limit)) {
		if (likely(pass < 2)) {
			/* First failure, just rescan the half of the table.
			 * Second failure, rescan the other half of the table.
			 */
			start = (largealloc ^ pass) ? tbl->it_halfpoint : 0;
			limit = pass ? tbl->it_mapsize : limit;
			pass++;
			goto again;
		} else {
			/* Third failure, give up */
			return DMA_ERROR_CODE;
		}
	}

	for (i = n; i < end; i++)
		if (test_bit(i, tbl->it_map)) {
			start = i+1;
			goto again;
		}

	for (i = n; i < end; i++)
		__set_bit(i, tbl->it_map);

	/* Bump the hint to a new block for small allocs. */
	if (largealloc) {
		/* Don't bump to new block to avoid fragmentation */
		tbl->it_largehint = end;
	} else {
		/* Overflow will be taken care of at the next allocation */
		tbl->it_hint = (end + tbl->it_blocksize - 1) &
		                ~(tbl->it_blocksize - 1);
	}

	/* Update handle for SG allocations */
	if (handle)
		*handle = end;

	return n;
}

static dma_addr_t iommu_alloc(struct iommu_table *tbl, void *page,
		       unsigned int npages, enum dma_data_direction direction)
{
	unsigned long entry, flags;
	dma_addr_t ret = DMA_ERROR_CODE;
	
	spin_lock_irqsave(&(tbl->it_lock), flags);

	entry = iommu_range_alloc(tbl, npages, NULL);

	if (unlikely(entry == DMA_ERROR_CODE)) {
		spin_unlock_irqrestore(&(tbl->it_lock), flags);
		return DMA_ERROR_CODE;
	}

	entry += tbl->it_offset;	/* Offset into real TCE table */
	ret = entry << PAGE_SHIFT;	/* Set the return dma address */

	/* Put the TCEs in the HW table */
	ppc_md.tce_build(tbl, entry, npages, (unsigned long)page & PAGE_MASK,
			 direction);


	/* Flush/invalidate TLB caches if necessary */
	if (ppc_md.tce_flush)
		ppc_md.tce_flush(tbl);

	spin_unlock_irqrestore(&(tbl->it_lock), flags);

	/* Make sure updates are seen by hardware */
	mb();

	return ret;
}

static void __iommu_free(struct iommu_table *tbl, dma_addr_t dma_addr, 
			 unsigned int npages)
{
	unsigned long entry, free_entry;
	unsigned long i;

	entry = dma_addr >> PAGE_SHIFT;
	free_entry = entry - tbl->it_offset;

	if (((free_entry + npages) > tbl->it_mapsize) ||
	    (entry < tbl->it_offset)) {
		if (printk_ratelimit()) {
			printk(KERN_INFO "iommu_free: invalid entry\n");
			printk(KERN_INFO "\tentry     = 0x%lx\n", entry); 
			printk(KERN_INFO "\tdma_addr  = 0x%lx\n", (u64)dma_addr);
			printk(KERN_INFO "\tTable     = 0x%lx\n", (u64)tbl);
			printk(KERN_INFO "\tbus#      = 0x%lx\n", (u64)tbl->it_busno);
			printk(KERN_INFO "\tmapsize   = 0x%lx\n", (u64)tbl->it_mapsize);
			printk(KERN_INFO "\tstartOff  = 0x%lx\n", (u64)tbl->it_offset);
			printk(KERN_INFO "\tindex     = 0x%lx\n", (u64)tbl->it_index);
			WARN_ON(1);
		}
		return;
	}

	ppc_md.tce_free(tbl, entry, npages);
	
	for (i = 0; i < npages; i++)
		__clear_bit(free_entry+i, tbl->it_map);
}

static void iommu_free(struct iommu_table *tbl, dma_addr_t dma_addr,
		unsigned int npages)
{
	unsigned long flags;

	spin_lock_irqsave(&(tbl->it_lock), flags);

	__iommu_free(tbl, dma_addr, npages);

	/* Make sure TLB cache is flushed if the HW needs it. We do
	 * not do an mb() here on purpose, it is not needed on any of
	 * the current platforms.
	 */
	if (ppc_md.tce_flush)
		ppc_md.tce_flush(tbl);

	spin_unlock_irqrestore(&(tbl->it_lock), flags);
}

int iommu_map_sg(struct device *dev, struct iommu_table *tbl,
		struct scatterlist *sglist, int nelems,
		enum dma_data_direction direction)
{
	dma_addr_t dma_next, dma_addr;
	unsigned long flags;
	struct scatterlist *s, *outs, *segstart;
	int outcount;
	unsigned long handle;

	BUG_ON(direction == DMA_NONE);

	if ((nelems == 0) || !tbl)
		return 0;

	outs = s = segstart = &sglist[0];
	outcount = 1;
	handle = 0;

	/* Init first segment length for backout at failure */
	outs->dma_length = 0;

	DBG("mapping %d elements:\n", nelems);

	spin_lock_irqsave(&(tbl->it_lock), flags);

	for (s = outs; nelems; nelems--, s++) {
		unsigned long vaddr, npages, entry, slen;

		slen = s->length;
		/* Sanity check */
		if (slen == 0) {
			dma_next = 0;
			continue;
		}
		/* Allocate iommu entries for that segment */
		vaddr = (unsigned long)page_address(s->page) + s->offset;
		npages = PAGE_ALIGN(vaddr + slen) - (vaddr & PAGE_MASK);
		npages >>= PAGE_SHIFT;
		entry = iommu_range_alloc(tbl, npages, &handle);

		DBG("  - vaddr: %lx, size: %lx\n", vaddr, slen);

		/* Handle failure */
		if (unlikely(entry == DMA_ERROR_CODE)) {
			if (printk_ratelimit())
				printk(KERN_INFO "iommu_alloc failed, tbl %p vaddr %lx"
				       " npages %lx\n", tbl, vaddr, npages);
			goto failure;
		}

		/* Convert entry to a dma_addr_t */
		entry += tbl->it_offset;
		dma_addr = entry << PAGE_SHIFT;
		dma_addr |= s->offset;

		DBG("  - %lx pages, entry: %lx, dma_addr: %lx\n",
			    npages, entry, dma_addr);

		/* Insert into HW table */
		ppc_md.tce_build(tbl, entry, npages, vaddr & PAGE_MASK, direction);

		/* If we are in an open segment, try merging */
		if (segstart != s) {
			DBG("  - trying merge...\n");
			/* We cannot merge if:
			 * - allocated dma_addr isn't contiguous to previous allocation
			 */
			if (novmerge || (dma_addr != dma_next)) {
				/* Can't merge: create a new segment */
				segstart = s;
				outcount++; outs++;
				DBG("    can't merge, new segment.\n");
			} else {
				outs->dma_length += s->length;
				DBG("    merged, new len: %lx\n", outs->dma_length);
			}
		}

		if (segstart == s) {
			/* This is a new segment, fill entries */
			DBG("  - filling new segment.\n");
			outs->dma_address = dma_addr;
			outs->dma_length = slen;
		}

		/* Calculate next page pointer for contiguous check */
		dma_next = dma_addr + slen;

		DBG("  - dma next is: %lx\n", dma_next);
	}

	/* Flush/invalidate TLB caches if necessary */
	if (ppc_md.tce_flush)
		ppc_md.tce_flush(tbl);

	spin_unlock_irqrestore(&(tbl->it_lock), flags);

	/* Make sure updates are seen by hardware */
	mb();

	DBG("mapped %d elements:\n", outcount);

	/* For the sake of iommu_free_sg, we clear out the length in the
	 * next entry of the sglist if we didn't fill the list completely
	 */
	if (outcount < nelems) {
		outs++;
		outs->dma_address = DMA_ERROR_CODE;
		outs->dma_length = 0;
	}
	return outcount;

 failure:
	for (s = &sglist[0]; s <= outs; s++) {
		if (s->dma_length != 0) {
			unsigned long vaddr, npages;

			vaddr = s->dma_address & PAGE_MASK;
			npages = (PAGE_ALIGN(s->dma_address + s->dma_length) - vaddr)
				>> PAGE_SHIFT;
			__iommu_free(tbl, vaddr, npages);
		}
	}
	spin_unlock_irqrestore(&(tbl->it_lock), flags);
	return 0;
}


void iommu_unmap_sg(struct iommu_table *tbl, struct scatterlist *sglist,
		int nelems, enum dma_data_direction direction)
{
	unsigned long flags;

	BUG_ON(direction == DMA_NONE);

	if (!tbl)
		return;

	spin_lock_irqsave(&(tbl->it_lock), flags);

	while (nelems--) {
		unsigned int npages;
		dma_addr_t dma_handle = sglist->dma_address;

		if (sglist->dma_length == 0)
			break;
		npages = (PAGE_ALIGN(dma_handle + sglist->dma_length)
			  - (dma_handle & PAGE_MASK)) >> PAGE_SHIFT;
		__iommu_free(tbl, dma_handle, npages);
		sglist++;
	}

	/* Flush/invalidate TLBs if necessary. As for iommu_free(), we
	 * do not do an mb() here, the affected platforms do not need it
	 * when freeing.
	 */
	if (ppc_md.tce_flush)
		ppc_md.tce_flush(tbl);

	spin_unlock_irqrestore(&(tbl->it_lock), flags);
}

/*
 * Build a iommu_table structure.  This contains a bit map which
 * is used to manage allocation of the tce space.
 */
struct iommu_table *iommu_init_table(struct iommu_table *tbl)
{
	unsigned long sz;
	static int welcomed = 0;

	/* it_size is in pages, it_mapsize in number of entries */
	tbl->it_mapsize = (tbl->it_size << PAGE_SHIFT) / tbl->it_entrysize;

	/* Set aside 1/4 of the table for large allocations. */
	tbl->it_halfpoint = tbl->it_mapsize * 3 / 4;

	/* number of bytes needed for the bitmap */
	sz = (tbl->it_mapsize + 7) >> 3;

	tbl->it_map = (unsigned long *)__get_free_pages(GFP_ATOMIC, get_order(sz));
	if (!tbl->it_map)
		panic("iommu_init_table: Can't allocate %ld bytes\n", sz);

	memset(tbl->it_map, 0, sz);

	tbl->it_hint = 0;
	tbl->it_largehint = tbl->it_halfpoint;
	spin_lock_init(&tbl->it_lock);

	if (!welcomed) {
		printk(KERN_INFO "IOMMU table initialized, virtual merging %s\n",
		       novmerge ? "disabled" : "enabled");
		welcomed = 1;
	}

	return tbl;
}

/* Creates TCEs for a user provided buffer.  The user buffer must be
 * contiguous real kernel storage (not vmalloc).  The address of the buffer
 * passed here is the kernel (virtual) address of the buffer.  The buffer
 * need not be page aligned, the dma_addr_t returned will point to the same
 * byte within the page as vaddr.
 */
dma_addr_t iommu_map_single(struct iommu_table *tbl, void *vaddr,
		size_t size, enum dma_data_direction direction)
{
	dma_addr_t dma_handle = DMA_ERROR_CODE;
	unsigned long uaddr;
	unsigned int npages;

	BUG_ON(direction == DMA_NONE);

	uaddr = (unsigned long)vaddr;
	npages = PAGE_ALIGN(uaddr + size) - (uaddr & PAGE_MASK);
	npages >>= PAGE_SHIFT;

	if (tbl) {
		dma_handle = iommu_alloc(tbl, vaddr, npages, direction);
		if (dma_handle == DMA_ERROR_CODE) {
			if (printk_ratelimit())  {
				printk(KERN_INFO "iommu_alloc failed, "
						"tbl %p vaddr %p npages %d\n",
						tbl, vaddr, npages);
			}
		} else
			dma_handle |= (uaddr & ~PAGE_MASK);
	}

	return dma_handle;
}

void iommu_unmap_single(struct iommu_table *tbl, dma_addr_t dma_handle,
		size_t size, enum dma_data_direction direction)
{
	BUG_ON(direction == DMA_NONE);

	if (tbl)
		iommu_free(tbl, dma_handle, (PAGE_ALIGN(dma_handle + size) -
					(dma_handle & PAGE_MASK)) >> PAGE_SHIFT);
}

/* Allocates a contiguous real buffer and creates mappings over it.
 * Returns the virtual address of the buffer and sets dma_handle
 * to the dma address (mapping) of the first page.
 */
void *iommu_alloc_consistent(struct iommu_table *tbl, size_t size,
		dma_addr_t *dma_handle)
{
	void *ret = NULL;
	dma_addr_t mapping;
	unsigned int npages, order;

	size = PAGE_ALIGN(size);
	npages = size >> PAGE_SHIFT;
	order = get_order(size);

 	/*
	 * Client asked for way too much space.  This is checked later
	 * anyway.  It is easier to debug here for the drivers than in
	 * the tce tables.
	 */
	if (order >= IOMAP_MAX_ORDER) {
		printk("iommu_alloc_consistent size too large: 0x%lx\n", size);
		return NULL;
	}

	if (!tbl)
		return NULL;

	/* Alloc enough pages (and possibly more) */
	ret = (void *)__get_free_pages(GFP_ATOMIC, order);
	if (!ret)
		return NULL;
	memset(ret, 0, size);

	/* Set up tces to cover the allocated range */
	mapping = iommu_alloc(tbl, ret, npages, DMA_BIDIRECTIONAL);
	if (mapping == DMA_ERROR_CODE) {
		free_pages((unsigned long)ret, order);
		ret = NULL;
	} else
		*dma_handle = mapping;
	return ret;
}

void iommu_free_consistent(struct iommu_table *tbl, size_t size,
			 void *vaddr, dma_addr_t dma_handle)
{
	unsigned int npages;

	if (tbl) {
		size = PAGE_ALIGN(size);
		npages = size >> PAGE_SHIFT;
		iommu_free(tbl, dma_handle, npages);
		free_pages((unsigned long)vaddr, get_order(size));
	}
}
