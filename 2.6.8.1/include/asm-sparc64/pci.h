#ifndef __SPARC64_PCI_H
#define __SPARC64_PCI_H

#ifdef __KERNEL__

#include <linux/fs.h>
#include <linux/mm.h>

/* Can be used to override the logic in pci_scan_bus for skipping
 * already-configured bus numbers - to be used for buggy BIOSes
 * or architectures with incomplete PCI setup by the loader.
 */
#define pcibios_assign_all_busses()	0
#define pcibios_scan_all_fns(a, b)	0

#define PCIBIOS_MIN_IO		0UL
#define PCIBIOS_MIN_MEM		0UL

#define PCI_IRQ_NONE		0xffffffff

static inline void pcibios_set_master(struct pci_dev *dev)
{
	/* No special bus mastering setup handling */
}

static inline void pcibios_penalize_isa_irq(int irq)
{
	/* We don't do dynamic PCI IRQ allocation */
}

/* Dynamic DMA mapping stuff.
 */

/* The PCI address space does not equal the physical memory
 * address space.  The networking and block device layers use
 * this boolean for bounce buffer decisions.
 */
#define PCI_DMA_BUS_IS_PHYS	(0)

#include <asm/scatterlist.h>

struct pci_dev;

/* Allocate and map kernel buffer using consistent mode DMA for a device.
 * hwdev should be valid struct pci_dev pointer for PCI devices.
 */
extern void *pci_alloc_consistent(struct pci_dev *hwdev, size_t size, dma_addr_t *dma_handle);

/* Free and unmap a consistent DMA buffer.
 * cpu_addr is what was returned from pci_alloc_consistent,
 * size must be the same as what as passed into pci_alloc_consistent,
 * and likewise dma_addr must be the same as what *dma_addrp was set to.
 *
 * References to the memory and mappings associated with cpu_addr/dma_addr
 * past this call are illegal.
 */
extern void pci_free_consistent(struct pci_dev *hwdev, size_t size, void *vaddr, dma_addr_t dma_handle);

/* Map a single buffer of the indicated size for DMA in streaming mode.
 * The 32-bit bus address to use is returned.
 *
 * Once the device is given the dma address, the device owns this memory
 * until either pci_unmap_single or pci_dma_sync_single_for_cpu is performed.
 */
extern dma_addr_t pci_map_single(struct pci_dev *hwdev, void *ptr, size_t size, int direction);

/* Unmap a single streaming mode DMA translation.  The dma_addr and size
 * must match what was provided for in a previous pci_map_single call.  All
 * other usages are undefined.
 *
 * After this call, reads by the cpu to the buffer are guaranteed to see
 * whatever the device wrote there.
 */
extern void pci_unmap_single(struct pci_dev *hwdev, dma_addr_t dma_addr, size_t size, int direction);

/* No highmem on sparc64, plus we have an IOMMU, so mapping pages is easy. */
#define pci_map_page(dev, page, off, size, dir) \
	pci_map_single(dev, (page_address(page) + (off)), size, dir)
#define pci_unmap_page(dev,addr,sz,dir) pci_unmap_single(dev,addr,sz,dir)

/* pci_unmap_{single,page} is not a nop, thus... */
#define DECLARE_PCI_UNMAP_ADDR(ADDR_NAME)	\
	dma_addr_t ADDR_NAME;
#define DECLARE_PCI_UNMAP_LEN(LEN_NAME)		\
	__u32 LEN_NAME;
#define pci_unmap_addr(PTR, ADDR_NAME)			\
	((PTR)->ADDR_NAME)
#define pci_unmap_addr_set(PTR, ADDR_NAME, VAL)		\
	(((PTR)->ADDR_NAME) = (VAL))
#define pci_unmap_len(PTR, LEN_NAME)			\
	((PTR)->LEN_NAME)
#define pci_unmap_len_set(PTR, LEN_NAME, VAL)		\
	(((PTR)->LEN_NAME) = (VAL))

/* Map a set of buffers described by scatterlist in streaming
 * mode for DMA.  This is the scatter-gather version of the
 * above pci_map_single interface.  Here the scatter gather list
 * elements are each tagged with the appropriate dma address
 * and length.  They are obtained via sg_dma_{address,length}(SG).
 *
 * NOTE: An implementation may be able to use a smaller number of
 *       DMA address/length pairs than there are SG table elements.
 *       (for example via virtual mapping capabilities)
 *       The routine returns the number of addr/length pairs actually
 *       used, at most nents.
 *
 * Device ownership issues as mentioned above for pci_map_single are
 * the same here.
 */
extern int pci_map_sg(struct pci_dev *hwdev, struct scatterlist *sg,
		      int nents, int direction);

/* Unmap a set of streaming mode DMA translations.
 * Again, cpu read rules concerning calls here are the same as for
 * pci_unmap_single() above.
 */
extern void pci_unmap_sg(struct pci_dev *hwdev, struct scatterlist *sg,
			 int nhwents, int direction);

/* Make physical memory consistent for a single
 * streaming mode DMA translation after a transfer.
 *
 * If you perform a pci_map_single() but wish to interrogate the
 * buffer using the cpu, yet do not wish to teardown the PCI dma
 * mapping, you must call this function before doing so.  At the
 * next point you give the PCI dma address back to the card, you
 * must first perform a pci_dma_sync_for_device, and then the
 * device again owns the buffer.
 */
extern void pci_dma_sync_single_for_cpu(struct pci_dev *hwdev, dma_addr_t dma_handle,
					size_t size, int direction);

static inline void
pci_dma_sync_single_for_device(struct pci_dev *hwdev, dma_addr_t dma_handle,
			       size_t size, int direction)
{
	/* No flushing needed to sync cpu writes to the device.  */
	BUG_ON(direction == PCI_DMA_NONE);
}

/* Make physical memory consistent for a set of streaming
 * mode DMA translations after a transfer.
 *
 * The same as pci_dma_sync_single_* but for a scatter-gather list,
 * same rules and usage.
 */
extern void pci_dma_sync_sg_for_cpu(struct pci_dev *hwdev, struct scatterlist *sg, int nelems, int direction);

static inline void
pci_dma_sync_sg_for_device(struct pci_dev *hwdev, struct scatterlist *sg,
			int nelems, int direction)
{
	/* No flushing needed to sync cpu writes to the device.  */
	BUG_ON(direction == PCI_DMA_NONE);
}

/* Return whether the given PCI device DMA address mask can
 * be supported properly.  For example, if your device can
 * only drive the low 24-bits during PCI bus mastering, then
 * you would pass 0x00ffffff as the mask to this function.
 */
extern int pci_dma_supported(struct pci_dev *hwdev, u64 mask);

/* PCI IOMMU mapping bypass support. */

/* PCI 64-bit addressing works for all slots on all controller
 * types on sparc64.  However, it requires that the device
 * can drive enough of the 64 bits.
 */
#define PCI64_REQUIRED_MASK	(~(dma64_addr_t)0)
#define PCI64_ADDR_BASE		0xfffc000000000000UL

/* Usage of the pci_dac_foo interfaces is only valid if this
 * test passes.
 */
#define pci_dac_dma_supported(pci_dev, mask) \
	((((mask) & PCI64_REQUIRED_MASK) == PCI64_REQUIRED_MASK) ? 1 : 0)

static inline dma64_addr_t
pci_dac_page_to_dma(struct pci_dev *pdev, struct page *page, unsigned long offset, int direction)
{
	return (PCI64_ADDR_BASE +
		__pa(page_address(page)) + offset);
}

static inline struct page *
pci_dac_dma_to_page(struct pci_dev *pdev, dma64_addr_t dma_addr)
{
	unsigned long paddr = (dma_addr & PAGE_MASK) - PCI64_ADDR_BASE;

	return virt_to_page(__va(paddr));
}

static inline unsigned long
pci_dac_dma_to_offset(struct pci_dev *pdev, dma64_addr_t dma_addr)
{
	return (dma_addr & ~PAGE_MASK);
}

static inline void
pci_dac_dma_sync_single_for_cpu(struct pci_dev *pdev, dma64_addr_t dma_addr, size_t len, int direction)
{
	/* DAC cycle addressing does not make use of the
	 * PCI controller's streaming cache, so nothing to do.
	 */
}

static inline void
pci_dac_dma_sync_single_for_device(struct pci_dev *pdev, dma64_addr_t dma_addr, size_t len, int direction)
{
	/* DAC cycle addressing does not make use of the
	 * PCI controller's streaming cache, so nothing to do.
	 */
}

#define PCI_DMA_ERROR_CODE	(~(dma_addr_t)0x0)

static inline int pci_dma_mapping_error(dma_addr_t dma_addr)
{
	return (dma_addr == PCI_DMA_ERROR_CODE);
}

/* Return the index of the PCI controller for device PDEV. */

extern int pci_domain_nr(struct pci_bus *bus);
extern int pci_name_bus(char *name, struct pci_bus *bus);

/* Platform support for /proc/bus/pci/X/Y mmap()s. */

#define HAVE_PCI_MMAP
#define HAVE_ARCH_PCI_GET_UNMAPPED_AREA
#define get_pci_unmapped_area get_fb_unmapped_area

extern int pci_mmap_page_range(struct pci_dev *dev, struct vm_area_struct *vma,
			       enum pci_mmap_state mmap_state,
			       int write_combine);

/* Platform specific MWI support. */
#define HAVE_ARCH_PCI_MWI
extern int pcibios_prep_mwi(struct pci_dev *dev);

extern void
pcibios_resource_to_bus(struct pci_dev *dev, struct pci_bus_region *region,
			struct resource *res);

extern void
pcibios_bus_to_resource(struct pci_dev *dev, struct resource *res,
			struct pci_bus_region *region);

static inline void pcibios_add_platform_entries(struct pci_dev *dev)
{
}

#endif /* __KERNEL__ */

#endif /* __SPARC64_PCI_H */
