/*
 * Dynamic DMA mapping support.
 *
 * This implementation is for IA-64 platforms that do not support
 * I/O TLBs (aka DMA address translation hardware).
 * Copyright (C) 2000 Asit Mallick <Asit.K.Mallick@intel.com>
 * Copyright (C) 2000 Goutham Rao <goutham.rao@intel.com>
 * Copyright (C) 2000, 2003 Hewlett-Packard Co
 *	David Mosberger-Tang <davidm@hpl.hp.com>
 *
 * 03/05/07 davidm	Switch from PCI-DMA to generic device DMA API.
 * 00/12/13 davidm	Rename to swiotlb.c and add mark_clean() to avoid
 *			unnecessary i-cache flushing.
 */

#include <linux/cache.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/io.h>
#include <asm/pci.h>
#include <asm/dma.h>

#include <linux/init.h>
#include <linux/bootmem.h>

#define OFFSET(val,align) ((unsigned long)	\
	                   ( (val) & ( (align) - 1)))

#define SG_ENT_VIRT_ADDRESS(sg)	(page_address((sg)->page) + (sg)->offset)
#define SG_ENT_PHYS_ADDRESS(SG)	virt_to_phys(SG_ENT_VIRT_ADDRESS(SG))

/*
 * Maximum allowable number of contiguous slabs to map,
 * must be a power of 2.  What is the appropriate value ?
 * The complexity of {map,unmap}_single is linearly dependent on this value.
 */
#define IO_TLB_SEGSIZE	128

/*
 * log of the size of each IO TLB slab.  The number of slabs is command line controllable.
 */
#define IO_TLB_SHIFT 11

/*
 * Used to do a quick range check in swiotlb_unmap_single and swiotlb_sync_single_*, to see
 * if the memory was in fact allocated by this API.
 */
static char *io_tlb_start, *io_tlb_end;

/*
 * The number of IO TLB blocks (in groups of 64) betweeen io_tlb_start and io_tlb_end.
 * This is command line adjustable via setup_io_tlb_npages.
 */
static unsigned long io_tlb_nslabs = 1024;

/*
 * This is a free list describing the number of free entries available from each index
 */
static unsigned int *io_tlb_list;
static unsigned int io_tlb_index;

/*
 * We need to save away the original address corresponding to a mapped entry for the sync
 * operations.
 */
static unsigned char **io_tlb_orig_addr;

/*
 * Protect the above data structures in the map and unmap calls
 */
static spinlock_t io_tlb_lock = SPIN_LOCK_UNLOCKED;

static int __init
setup_io_tlb_npages (char *str)
{
	io_tlb_nslabs = simple_strtoul(str, NULL, 0) << (PAGE_SHIFT - IO_TLB_SHIFT);

	/* avoid tail segment of size < IO_TLB_SEGSIZE */
	io_tlb_nslabs = ALIGN(io_tlb_nslabs, IO_TLB_SEGSIZE);

	return 1;
}
__setup("swiotlb=", setup_io_tlb_npages);


/*
 * Statically reserve bounce buffer space and initialize bounce buffer data structures for
 * the software IO TLB used to implement the PCI DMA API.
 */
void
swiotlb_init (void)
{
	unsigned long i;

	/*
	 * Get IO TLB memory from the low pages
	 */
	io_tlb_start = alloc_bootmem_low_pages(io_tlb_nslabs * (1 << IO_TLB_SHIFT));
	if (!io_tlb_start)
		BUG();
	io_tlb_end = io_tlb_start + io_tlb_nslabs * (1 << IO_TLB_SHIFT);

	/*
	 * Allocate and initialize the free list array.  This array is used
	 * to find contiguous free memory regions of size up to IO_TLB_SEGSIZE
	 * between io_tlb_start and io_tlb_end.
	 */
	io_tlb_list = alloc_bootmem(io_tlb_nslabs * sizeof(int));
	for (i = 0; i < io_tlb_nslabs; i++)
 		io_tlb_list[i] = IO_TLB_SEGSIZE - OFFSET(i, IO_TLB_SEGSIZE);
	io_tlb_index = 0;
	io_tlb_orig_addr = alloc_bootmem(io_tlb_nslabs * sizeof(char *));

	printk(KERN_INFO "Placing software IO TLB between 0x%p - 0x%p\n",
	       (void *) io_tlb_start, (void *) io_tlb_end);
}

/*
 * Allocates bounce buffer and returns its kernel virtual address.
 */
static void *
map_single (struct device *hwdev, char *buffer, size_t size, int dir)
{
	unsigned long flags;
	char *dma_addr;
	unsigned int nslots, stride, index, wrap;
	int i;

	/*
	 * For mappings greater than a page size, we limit the stride (and hence alignment)
	 * to a page size.
	 */
	nslots = ALIGN(size, 1 << IO_TLB_SHIFT) >> IO_TLB_SHIFT;
	if (size > (1 << PAGE_SHIFT))
		stride = (1 << (PAGE_SHIFT - IO_TLB_SHIFT));
	else
		stride = 1;

	if (!nslots)
		BUG();

	/*
	 * Find suitable number of IO TLB entries size that will fit this request and
	 * allocate a buffer from that IO TLB pool.
	 */
	spin_lock_irqsave(&io_tlb_lock, flags);
	{
		wrap = index = ALIGN(io_tlb_index, stride);

		if (index >= io_tlb_nslabs)
			wrap = index = 0;

		do {
			/*
			 * If we find a slot that indicates we have 'nslots' number of
			 * contiguous buffers, we allocate the buffers from that slot and
			 * mark the entries as '0' indicating unavailable.
			 */
			if (io_tlb_list[index] >= nslots) {
				int count = 0;

				for (i = index; i < (int) (index + nslots); i++)
					io_tlb_list[i] = 0;
				for (i = index - 1; (OFFSET(i, IO_TLB_SEGSIZE) != IO_TLB_SEGSIZE -1)
				       && io_tlb_list[i]; i--)
					io_tlb_list[i] = ++count;
				dma_addr = io_tlb_start + (index << IO_TLB_SHIFT);

				/*
				 * Update the indices to avoid searching in the next round.
				 */
				io_tlb_index = ((index + nslots) < io_tlb_nslabs
						? (index + nslots) : 0);

				goto found;
			}
			index += stride;
			if (index >= io_tlb_nslabs)
				index = 0;
		} while (index != wrap);

		/*
		 * XXX What is a suitable recovery mechanism here?  We cannot
		 * sleep because we are called from with in interrupts!
		 */
		panic("map_single: could not allocate software IO TLB (%ld bytes)", size);
	}
  found:
	spin_unlock_irqrestore(&io_tlb_lock, flags);

	/*
	 * Save away the mapping from the original address to the DMA address.  This is
	 * needed when we sync the memory.  Then we sync the buffer if needed.
	 */
	io_tlb_orig_addr[index] = buffer;
	if (dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL)
		memcpy(dma_addr, buffer, size);

	return dma_addr;
}

/*
 * dma_addr is the kernel virtual address of the bounce buffer to unmap.
 */
static void
unmap_single (struct device *hwdev, char *dma_addr, size_t size, int dir)
{
	unsigned long flags;
	int i, nslots = ALIGN(size, 1 << IO_TLB_SHIFT) >> IO_TLB_SHIFT;
	int index = (dma_addr - io_tlb_start) >> IO_TLB_SHIFT;
	char *buffer = io_tlb_orig_addr[index];

	/*
	 * First, sync the memory before unmapping the entry
	 */
	if ((dir == DMA_FROM_DEVICE) || (dir == DMA_BIDIRECTIONAL))
		/*
		 * bounce... copy the data back into the original buffer * and delete the
		 * bounce buffer.
		 */
		memcpy(buffer, dma_addr, size);

	/*
	 * Return the buffer to the free list by setting the corresponding entries to
	 * indicate the number of contigous entries available.  While returning the
	 * entries to the free list, we merge the entries with slots below and above the
	 * pool being returned.
	 */
	spin_lock_irqsave(&io_tlb_lock, flags);
	{
		int count = ((index + nslots) < ALIGN(index + 1, IO_TLB_SEGSIZE) ?
			     io_tlb_list[index + nslots] : 0);
		/*
		 * Step 1: return the slots to the free list, merging the slots with
		 * superceeding slots
		 */
		for (i = index + nslots - 1; i >= index; i--)
			io_tlb_list[i] = ++count;
		/*
		 * Step 2: merge the returned slots with the preceding slots, if
		 * available (non zero)
		 */
		for (i = index - 1;  (OFFSET(i, IO_TLB_SEGSIZE) != IO_TLB_SEGSIZE -1) &&
		       io_tlb_list[i]; i--)
			io_tlb_list[i] = ++count;
	}
	spin_unlock_irqrestore(&io_tlb_lock, flags);
}

static void
sync_single (struct device *hwdev, char *dma_addr, size_t size, int dir)
{
	int index = (dma_addr - io_tlb_start) >> IO_TLB_SHIFT;
	char *buffer = io_tlb_orig_addr[index];

	/*
	 * bounce... copy the data back into/from the original buffer
	 * XXX How do you handle DMA_BIDIRECTIONAL here ?
	 */
	if (dir == DMA_FROM_DEVICE)
		memcpy(buffer, dma_addr, size);
	else if (dir == DMA_TO_DEVICE)
		memcpy(dma_addr, buffer, size);
	else
		BUG();
}

void *
swiotlb_alloc_coherent (struct device *hwdev, size_t size, dma_addr_t *dma_handle, int flags)
{
	unsigned long dev_addr;
	void *ret;

	/* XXX fix me: the DMA API should pass us an explicit DMA mask instead: */
	flags |= GFP_DMA;

	ret = (void *)__get_free_pages(flags, get_order(size));
	if (!ret)
		return NULL;

	memset(ret, 0, size);
	dev_addr = virt_to_phys(ret);
	if (hwdev && hwdev->dma_mask && (dev_addr & ~*hwdev->dma_mask) != 0)
		panic("swiotlb_alloc_consistent: allocated memory is out of range for device");
	*dma_handle = dev_addr;
	return ret;
}

void
swiotlb_free_coherent (struct device *hwdev, size_t size, void *vaddr, dma_addr_t dma_handle)
{
	free_pages((unsigned long) vaddr, get_order(size));
}

/*
 * Map a single buffer of the indicated size for DMA in streaming mode.  The PCI address
 * to use is returned.
 *
 * Once the device is given the dma address, the device owns this memory until either
 * swiotlb_unmap_single or swiotlb_dma_sync_single is performed.
 */
dma_addr_t
swiotlb_map_single (struct device *hwdev, void *ptr, size_t size, int dir)
{
	unsigned long dev_addr = virt_to_phys(ptr);

	if (dir == DMA_NONE)
		BUG();
	/*
	 * Check if the PCI device can DMA to ptr... if so, just return ptr
	 */
	if (hwdev && hwdev->dma_mask && (dev_addr & ~*hwdev->dma_mask) == 0)
		/*
		 * Device is bit capable of DMA'ing to the buffer... just return the PCI
		 * address of ptr
		 */
		return dev_addr;

	/*
	 * get a bounce buffer:
	 */
	dev_addr = virt_to_phys(map_single(hwdev, ptr, size, dir));

	/*
	 * Ensure that the address returned is DMA'ble:
	 */
	if (hwdev && hwdev->dma_mask && (dev_addr & ~*hwdev->dma_mask) != 0)
		panic("map_single: bounce buffer is not DMA'ble");

	return dev_addr;
}

/*
 * Since DMA is i-cache coherent, any (complete) pages that were written via
 * DMA can be marked as "clean" so that update_mmu_cache() doesn't have to
 * flush them when they get mapped into an executable vm-area.
 */
static void
mark_clean (void *addr, size_t size)
{
	unsigned long pg_addr, end;

	pg_addr = PAGE_ALIGN((unsigned long) addr);
	end = (unsigned long) addr + size;
	while (pg_addr + PAGE_SIZE <= end) {
		struct page *page = virt_to_page(pg_addr);
		set_bit(PG_arch_1, &page->flags);
		pg_addr += PAGE_SIZE;
	}
}

/*
 * Unmap a single streaming mode DMA translation.  The dma_addr and size must match what
 * was provided for in a previous swiotlb_map_single call.  All other usages are
 * undefined.
 *
 * After this call, reads by the cpu to the buffer are guaranteed to see whatever the
 * device wrote there.
 */
void
swiotlb_unmap_single (struct device *hwdev, dma_addr_t dev_addr, size_t size, int dir)
{
	char *dma_addr = phys_to_virt(dev_addr);

	if (dir == DMA_NONE)
		BUG();
	if (dma_addr >= io_tlb_start && dma_addr < io_tlb_end)
		unmap_single(hwdev, dma_addr, size, dir);
	else if (dir == DMA_FROM_DEVICE)
		mark_clean(dma_addr, size);
}

/*
 * Make physical memory consistent for a single streaming mode DMA translation after a
 * transfer.
 *
 * If you perform a swiotlb_map_single() but wish to interrogate the buffer using the cpu,
 * yet do not wish to teardown the PCI dma mapping, you must call this function before
 * doing so.  At the next point you give the PCI dma address back to the card, you must
 * first perform a swiotlb_dma_sync_for_device, and then the device again owns the buffer
 */
void
swiotlb_sync_single_for_cpu (struct device *hwdev, dma_addr_t dev_addr, size_t size, int dir)
{
	char *dma_addr = phys_to_virt(dev_addr);

	if (dir == DMA_NONE)
		BUG();
	if (dma_addr >= io_tlb_start && dma_addr < io_tlb_end)
		sync_single(hwdev, dma_addr, size, dir);
	else if (dir == DMA_FROM_DEVICE)
		mark_clean(dma_addr, size);
}

void
swiotlb_sync_single_for_device (struct device *hwdev, dma_addr_t dev_addr, size_t size, int dir)
{
	char *dma_addr = phys_to_virt(dev_addr);

	if (dir == DMA_NONE)
		BUG();
	if (dma_addr >= io_tlb_start && dma_addr < io_tlb_end)
		sync_single(hwdev, dma_addr, size, dir);
	else if (dir == DMA_FROM_DEVICE)
		mark_clean(dma_addr, size);
}

/*
 * Map a set of buffers described by scatterlist in streaming mode for DMA.  This is the
 * scatter-gather version of the above swiotlb_map_single interface.  Here the scatter
 * gather list elements are each tagged with the appropriate dma address and length.  They
 * are obtained via sg_dma_{address,length}(SG).
 *
 * NOTE: An implementation may be able to use a smaller number of
 *       DMA address/length pairs than there are SG table elements.
 *       (for example via virtual mapping capabilities)
 *       The routine returns the number of addr/length pairs actually
 *       used, at most nents.
 *
 * Device ownership issues as mentioned above for swiotlb_map_single are the same here.
 */
int
swiotlb_map_sg (struct device *hwdev, struct scatterlist *sg, int nelems, int dir)
{
	void *addr;
	unsigned long dev_addr;
	int i;

	if (dir == DMA_NONE)
		BUG();

	for (i = 0; i < nelems; i++, sg++) {
		addr = SG_ENT_VIRT_ADDRESS(sg);
		dev_addr = virt_to_phys(addr);
		if (hwdev && hwdev->dma_mask && (dev_addr & ~*hwdev->dma_mask) != 0)
			sg->dma_address = (dma_addr_t) map_single(hwdev, addr, sg->length, dir);
		else
			sg->dma_address = dev_addr;
		sg->dma_length = sg->length;
	}
	return nelems;
}

/*
 * Unmap a set of streaming mode DMA translations.  Again, cpu read rules concerning calls
 * here are the same as for swiotlb_unmap_single() above.
 */
void
swiotlb_unmap_sg (struct device *hwdev, struct scatterlist *sg, int nelems, int dir)
{
	int i;

	if (dir == DMA_NONE)
		BUG();

	for (i = 0; i < nelems; i++, sg++)
		if (sg->dma_address != SG_ENT_PHYS_ADDRESS(sg))
			unmap_single(hwdev, (void *) sg->dma_address, sg->dma_length, dir);
		else if (dir == DMA_FROM_DEVICE)
			mark_clean(SG_ENT_VIRT_ADDRESS(sg), sg->dma_length);
}

/*
 * Make physical memory consistent for a set of streaming mode DMA translations after a
 * transfer.
 *
 * The same as swiotlb_sync_single_* but for a scatter-gather list, same rules and
 * usage.
 */
void
swiotlb_sync_sg_for_cpu (struct device *hwdev, struct scatterlist *sg, int nelems, int dir)
{
	int i;

	if (dir == DMA_NONE)
		BUG();

	for (i = 0; i < nelems; i++, sg++)
		if (sg->dma_address != SG_ENT_PHYS_ADDRESS(sg))
			sync_single(hwdev, (void *) sg->dma_address, sg->dma_length, dir);
}

void
swiotlb_sync_sg_for_device (struct device *hwdev, struct scatterlist *sg, int nelems, int dir)
{
	int i;

	if (dir == DMA_NONE)
		BUG();

	for (i = 0; i < nelems; i++, sg++)
		if (sg->dma_address != SG_ENT_PHYS_ADDRESS(sg))
			sync_single(hwdev, (void *) sg->dma_address, sg->dma_length, dir);
}

int
swiotlb_dma_mapping_error (dma_addr_t dma_addr)
{
	return 0;
}

/*
 * Return whether the given PCI device DMA address mask can be supported properly.  For
 * example, if your device can only drive the low 24-bits during PCI bus mastering, then
 * you would pass 0x00ffffff as the mask to this function.
 */
int
swiotlb_dma_supported (struct device *hwdev, u64 mask)
{
	return 1;
}

EXPORT_SYMBOL(swiotlb_init);
EXPORT_SYMBOL(swiotlb_map_single);
EXPORT_SYMBOL(swiotlb_unmap_single);
EXPORT_SYMBOL(swiotlb_map_sg);
EXPORT_SYMBOL(swiotlb_unmap_sg);
EXPORT_SYMBOL(swiotlb_sync_single_for_cpu);
EXPORT_SYMBOL(swiotlb_sync_single_for_device);
EXPORT_SYMBOL(swiotlb_sync_sg_for_cpu);
EXPORT_SYMBOL(swiotlb_sync_sg_for_device);
EXPORT_SYMBOL(swiotlb_dma_mapping_error);
EXPORT_SYMBOL(swiotlb_alloc_coherent);
EXPORT_SYMBOL(swiotlb_free_coherent);
EXPORT_SYMBOL(swiotlb_dma_supported);
