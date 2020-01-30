#ifndef __ASM_SH_DMA_MAPPING_H
#define __ASM_SH_DMA_MAPPING_H

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/device.h>
#include <asm/scatterlist.h>
#include <asm/io.h>

/* arch/sh/mm/consistent.c */
extern void *consistent_alloc(int gfp, size_t size, dma_addr_t *handle);
extern void consistent_free(void *vaddr, size_t size);
extern void consistent_sync(void *vaddr, size_t size, int direction);

#ifdef CONFIG_SH_DREAMCAST
struct pci_dev;
extern struct bus_type pci_bus_type;
extern void *__pci_alloc_consistent(struct pci_dev *hwdev, size_t size,
				    dma_addr_t *dma_handle);
extern void __pci_free_consistent(struct pci_dev *hwdev, size_t size,
				  void *vaddr, dma_addr_t dma_handle);
#endif

#define dma_supported(dev, mask)	(1)

static inline int dma_set_mask(struct device *dev, u64 mask)
{
	if (!dev->dma_mask || !dma_supported(dev, mask))
		return -EIO;

	*dev->dma_mask = mask;

	return 0;
}

static inline void *dma_alloc_coherent(struct device *dev, size_t size,
			 dma_addr_t *dma_handle, int flag)
{
	/*
	 * Some platforms have special pci_alloc_consistent() implementations,
	 * in these instances we can't use the generic consistent_alloc().
	 */
#ifdef CONFIG_SH_DREAMCAST
	if (dev && dev->bus == &pci_bus_type)
		return __pci_alloc_consistent(NULL, size, dma_handle);
#endif
	if (sh_mv.mv_consistent_alloc)
		return sh_mv.mv_consistent_alloc(dev, size, dma_handle, flag);

	return consistent_alloc(flag, size, dma_handle);
}

static inline void dma_free_coherent(struct device *dev, size_t size,
		       void *vaddr, dma_addr_t dma_handle)
{
	/*
	 * Same note as above applies to pci_free_consistent()..
	 */
#ifdef CONFIG_SH_DREAMCAST
	if (dev && dev->bus == &pci_bus_type) {
		__pci_free_consistent(NULL, size, vaddr, dma_handle);
		return;
	}
#endif

	if (sh_mv.mv_consistent_free) {
		sh_mv.mv_consistent_free(dev, size, vaddr, dma_handle);
		return;
	}

	consistent_free(vaddr, size);
}

static inline void dma_cache_sync(void *vaddr, size_t size,
				  enum dma_data_direction dir)
{
	consistent_sync(vaddr, size, (int)dir);
}

static inline dma_addr_t dma_map_single(struct device *dev,
					void *ptr, size_t size,
					enum dma_data_direction dir)
{
#if defined(CONFIG_PCI) && !defined(CONFIG_SH_PCIDMA_NONCOHERENT)
	if (dev->bus == &pci_bus_type)
		return virt_to_bus(ptr);
#endif
	dma_cache_sync(ptr, size, dir);

	return virt_to_bus(ptr);
}

#define dma_unmap_single(dev, addr, size, dir)	do { } while (0)

static inline int dma_map_sg(struct device *dev, struct scatterlist *sg,
			     int nents, enum dma_data_direction dir)
{
	int i;

	for (i = 0; i < nents; i++) {
#if !defined(CONFIG_PCI) || defined(CONFIG_SH_PCIDMA_NONCOHERENT)
		dma_cache_sync(page_address(sg[i].page) + sg[i].offset,
			       sg[i].length, dir);
#endif
		sg[i].dma_address = page_to_phys(sg[i].page) + sg[i].offset;
	}

	return nents;
}

#define dma_unmap_sg(dev, sg, nents, dir)	do { } while (0)

static inline dma_addr_t dma_map_page(struct device *dev, struct page *page,
				      unsigned long offset, size_t size,
				      enum dma_data_direction dir)
{
	return dma_map_single(dev, page_address(page) + offset, size, dir);
}

static inline void dma_unmap_page(struct device *dev, dma_addr_t dma_address,
				  size_t size, enum dma_data_direction dir)
{
	dma_unmap_single(dev, dma_address, size, dir);
}

static inline void dma_sync_single(struct device *dev, dma_addr_t dma_handle,
				   size_t size, enum dma_data_direction dir)
{
#if defined(CONFIG_PCI) && !defined(CONFIG_SH_PCIDMA_NONCOHERENT)
	if (dev->bus == &pci_bus_type)
		return;
#endif
	dma_cache_sync(bus_to_virt(dma_handle), size, dir);
}

static inline void dma_sync_single_range(struct device *dev,
					 dma_addr_t dma_handle,
					 unsigned long offset, size_t size,
					 enum dma_data_direction dir)
{
#if defined(CONFIG_PCI) && !defined(CONFIG_SH_PCIDMA_NONCOHERENT)
	if (dev->bus == &pci_bus_type)
		return;
#endif
	dma_cache_sync(bus_to_virt(dma_handle) + offset, size, dir);
}

static inline void dma_sync_sg(struct device *dev, struct scatterlist *sg,
			       int nelems, enum dma_data_direction dir)
{
	int i;

	for (i = 0; i < nelems; i++) {
#if !defined(CONFIG_PCI) || defined(CONFIG_SH_PCIDMA_NONCOHERENT)
		dma_cache_sync(page_address(sg[i].page) + sg[i].offset,
			       sg[i].length, dir);
#endif
		sg[i].dma_address = page_to_phys(sg[i].page) + sg[i].offset;
	}
}

static inline void dma_sync_single_for_cpu(struct device *dev,
					   dma_addr_t dma_handle, size_t size,
					   enum dma_data_direction dir)
	__attribute__ ((alias("dma_sync_single")));

static inline void dma_sync_single_for_device(struct device *dev,
					   dma_addr_t dma_handle, size_t size,
					   enum dma_data_direction dir)
	__attribute__ ((alias("dma_sync_single")));

static inline void dma_sync_sg_for_cpu(struct device *dev,
				       struct scatterlist *sg, int nelems,
				       enum dma_data_direction dir)
	__attribute__ ((alias("dma_sync_sg")));

static inline void dma_sync_sg_for_device(struct device *dev,
				       struct scatterlist *sg, int nelems,
				       enum dma_data_direction dir)
	__attribute__ ((alias("dma_sync_sg")));

static inline int dma_get_cache_alignment(void)
{
	/*
	 * Each processor family will define its own L1_CACHE_SHIFT,
	 * L1_CACHE_BYTES wraps to this, so this is always safe.
	 */
	return L1_CACHE_BYTES;
}

static inline int dma_mapping_error(dma_addr_t dma_addr)
{
	return dma_addr == 0;
}

#endif /* __ASM_SH_DMA_MAPPING_H */

