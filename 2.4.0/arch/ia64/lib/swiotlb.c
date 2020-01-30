/*
 * Dynamic DMA mapping support.
 *
 * This implementation is for IA-64 platforms that do not support
 * I/O TLBs (aka DMA address translation hardware).
 * Copyright (C) 2000 Asit Mallick <Asit.K.Mallick@intel.com>
 * Copyright (C) 2000 Goutham Rao <goutham.rao@intel.com>
 *
 * 00/12/13 davidm	Rename to swiotlb.c and add mark_clean() to avoid
 *			unnecessary i-cache flushing.
 */

#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/io.h>
#include <asm/pci.h>
#include <asm/dma.h>

#include <linux/init.h>
#include <linux/bootmem.h>

#define ALIGN(val, align) ((unsigned long)	\
	(((unsigned long) (val) + ((align) - 1)) & ~((align) - 1)))

/*
 * log of the size of each IO TLB slab.  The number of slabs is command line controllable.
 */
#define IO_TLB_SHIFT 11

/*
 * Used to do a quick range check in swiotlb_unmap_single and swiotlb_sync_single, to see
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
	int i;

	/*
	 * Get IO TLB memory from the low pages
	 */
	io_tlb_start = alloc_bootmem_low_pages(io_tlb_nslabs * (1 << IO_TLB_SHIFT));
	if (!io_tlb_start)
		BUG();
	io_tlb_end = io_tlb_start + io_tlb_nslabs * (1 << IO_TLB_SHIFT);

	/*
	 * Allocate and initialize the free list array.  This array is used
	 * to find contiguous free memory regions of size 2^IO_TLB_SHIFT between
	 * io_tlb_start and io_tlb_end.
	 */
	io_tlb_list = alloc_bootmem(io_tlb_nslabs * sizeof(int));
	for (i = 0; i < io_tlb_nslabs; i++)
		io_tlb_list[i] = io_tlb_nslabs - i;
	io_tlb_index = 0;
	io_tlb_orig_addr = alloc_bootmem(io_tlb_nslabs * sizeof(char *));

	printk("Placing software IO TLB between 0x%p - 0x%p\n",
	       (void *) io_tlb_start, (void *) io_tlb_end);
}

/*
 * Allocates bounce buffer and returns its kernel virtual address.
 */
static void *
map_single (struct pci_dev *hwdev, char *buffer, size_t size, int direction)
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
		stride = nslots;

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

				for (i = index; i < index + nslots; i++)
					io_tlb_list[i] = 0;
				for (i = index - 1; (i >= 0) && io_tlb_list[i]; i--)
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
found:
	}
	spin_unlock_irqrestore(&io_tlb_lock, flags);

	/*
	 * Save away the mapping from the original address to the DMA address.  This is
	 * needed when we sync the memory.  Then we sync the buffer if needed.
	 */
	io_tlb_orig_addr[index] = buffer;
	if (direction == PCI_DMA_TODEVICE || direction == PCI_DMA_BIDIRECTIONAL)
		memcpy(dma_addr, buffer, size);

	return dma_addr;
}

/*
 * dma_addr is the kernel virtual address of the bounce buffer to unmap.
 */
static void
unmap_single (struct pci_dev *hwdev, char *dma_addr, size_t size, int direction)
{
	unsigned long flags;
	int i, nslots = ALIGN(size, 1 << IO_TLB_SHIFT) >> IO_TLB_SHIFT;
	int index = (dma_addr - io_tlb_start) >> IO_TLB_SHIFT;
	char *buffer = io_tlb_orig_addr[index];

	/*
	 * First, sync the memory before unmapping the entry
	 */
	if ((direction == PCI_DMA_FROMDEVICE) || (direction == PCI_DMA_BIDIRECTIONAL))
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
		int count = ((index + nslots) < io_tlb_nslabs ? io_tlb_list[index + nslots] : 0);
		/*
		 * Step 1: return the slots to the free list, merging the slots with
		 * superceeding slots
		 */
		for (i = index + nslots - 1; i >= index; i--)
			io_tlb_list[i] = ++count;
		/*
		 * Step 2: merge the returned slots with the preceeding slots, if
		 * available (non zero)
		 */
		for (i = index - 1; (i >= 0) && io_tlb_list[i]; i--)
			io_tlb_list[i] = ++count;
	}
	spin_unlock_irqrestore(&io_tlb_lock, flags);
}

static void
sync_single (struct pci_dev *hwdev, char *dma_addr, size_t size, int direction)
{
	int index = (dma_addr - io_tlb_start) >> IO_TLB_SHIFT;
	char *buffer = io_tlb_orig_addr[index];

	/*
  	 * bounce... copy the data back into/from the original buffer
	 * XXX How do you handle PCI_DMA_BIDIRECTIONAL here ?
 	 */
	if (direction == PCI_DMA_FROMDEVICE)
		memcpy(buffer, dma_addr, size);
	else if (direction == PCI_DMA_TODEVICE)
		memcpy(dma_addr, buffer, size);
	else
		BUG();
}

void *
swiotlb_alloc_consistent (struct pci_dev *hwdev, size_t size, dma_addr_t *dma_handle)
{
	unsigned long pci_addr;
	int gfp = GFP_ATOMIC;
	void *ret;

	if (!hwdev || hwdev->dma_mask <= 0xffffffff)
		gfp |= GFP_DMA; /* XXX fix me: should change this to GFP_32BIT or ZONE_32BIT */
	ret = (void *)__get_free_pages(gfp, get_order(size));
	if (!ret)
		return NULL;

	memset(ret, 0, size);
	pci_addr = virt_to_phys(ret);
	if ((pci_addr & ~hwdev->dma_mask) != 0)
		panic("swiotlb_alloc_consistent: allocated memory is out of range for PCI device");
	*dma_handle = pci_addr;
	return ret;
}

void
swiotlb_free_consistent (struct pci_dev *hwdev, size_t size, void *vaddr, dma_addr_t dma_handle)
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
swiotlb_map_single (struct pci_dev *hwdev, void *ptr, size_t size, int direction)
{
	unsigned long pci_addr = virt_to_phys(ptr);

	if (direction == PCI_DMA_NONE)
		BUG();
	/*
	 * Check if the PCI device can DMA to ptr... if so, just return ptr
	 */
	if ((pci_addr & ~hwdev->dma_mask) == 0)
		/*
		 * Device is bit capable of DMA'ing to the buffer... just return the PCI
		 * address of ptr
		 */
		return pci_addr;

	/* 
	 * get a bounce buffer: 
	 */
	pci_addr = virt_to_phys(map_single(hwdev, ptr, size, direction));

	/*
	 * Ensure that the address returned is DMA'ble:
	 */
	if ((pci_addr & ~hwdev->dma_mask) != 0)
		panic("map_single: bounce buffer is not DMA'ble");

	return pci_addr;
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
#if 0
		set_bit(PG_arch_1, virt_to_page(pg_addr));
#else
		if (!VALID_PAGE(virt_to_page(pg_addr)))
			printk("Invalid addr %lx!!!\n", pg_addr);
#endif
		pg_addr += PAGE_SIZE;
	}
}

/*
 * Unmap a single streaming mode DMA translation.  The dma_addr and size must match what
 * was provided for in a previous swiotlb_map_single call.  All other usages are
 * undefined.
 *
 * After this call, reads by the cpu to the buffer are guarenteed to see whatever the
 * device wrote there.
 */
void
swiotlb_unmap_single (struct pci_dev *hwdev, dma_addr_t pci_addr, size_t size, int direction)
{
	char *dma_addr = phys_to_virt(pci_addr);

	if (direction == PCI_DMA_NONE)
		BUG();
	if (dma_addr >= io_tlb_start && dma_addr < io_tlb_end)
		unmap_single(hwdev, dma_addr, size, direction);
	else if (direction == PCI_DMA_FROMDEVICE)
		mark_clean(dma_addr, size);
}

/*
 * Make physical memory consistent for a single streaming mode DMA translation after a
 * transfer.
 *
 * If you perform a swiotlb_map_single() but wish to interrogate the buffer using the cpu,
 * yet do not wish to teardown the PCI dma mapping, you must call this function before
 * doing so.  At the next point you give the PCI dma address back to the card, the device
 * again owns the buffer.
 */
void
swiotlb_sync_single (struct pci_dev *hwdev, dma_addr_t pci_addr, size_t size, int direction)
{
	char *dma_addr = phys_to_virt(pci_addr);

	if (direction == PCI_DMA_NONE)
		BUG();
	if (dma_addr >= io_tlb_start && dma_addr < io_tlb_end)
		sync_single(hwdev, dma_addr, size, direction);
	else if (direction == PCI_DMA_FROMDEVICE)
		mark_clean(dma_addr, size);
}

/*
 * Map a set of buffers described by scatterlist in streaming mode for DMA.  This is the
 * scather-gather version of the above swiotlb_map_single interface.  Here the scatter
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
swiotlb_map_sg (struct pci_dev *hwdev, struct scatterlist *sg, int nelems, int direction)
{
	int i;

	if (direction == PCI_DMA_NONE)
		BUG();

	for (i = 0; i < nelems; i++, sg++) {
		sg->orig_address = sg->address;
		if ((virt_to_phys(sg->address) & ~hwdev->dma_mask) != 0) {
			sg->address = map_single(hwdev, sg->address, sg->length, direction);
		}
	}
	return nelems;
}

/*
 * Unmap a set of streaming mode DMA translations.  Again, cpu read rules concerning calls
 * here are the same as for swiotlb_unmap_single() above.
 */
void
swiotlb_unmap_sg (struct pci_dev *hwdev, struct scatterlist *sg, int nelems, int direction)
{
	int i;

	if (direction == PCI_DMA_NONE)
		BUG();

	for (i = 0; i < nelems; i++, sg++)
		if (sg->orig_address != sg->address) {
			unmap_single(hwdev, sg->address, sg->length, direction);
			sg->address = sg->orig_address;
		} else if (direction == PCI_DMA_FROMDEVICE)
			mark_clean(sg->address, sg->length);
}

/*
 * Make physical memory consistent for a set of streaming mode DMA translations after a
 * transfer.
 *
 * The same as swiotlb_dma_sync_single but for a scatter-gather list, same rules and
 * usage.
 */
void
swiotlb_sync_sg (struct pci_dev *hwdev, struct scatterlist *sg, int nelems, int direction)
{
	int i;

	if (direction == PCI_DMA_NONE)
		BUG();

	for (i = 0; i < nelems; i++, sg++)
		if (sg->orig_address != sg->address)
			sync_single(hwdev, sg->address, sg->length, direction);
}

unsigned long
swiotlb_dma_address (struct scatterlist *sg)
{
	return virt_to_phys(sg->address);
}
