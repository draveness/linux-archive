/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2000,2002-2003 Silicon Graphics, Inc. All rights reserved.
 *
 * Routines for PCI DMA mapping.  See Documentation/DMA-mapping.txt for
 * a description of how these routines should be used.
 */

#include <linux/module.h>
#include <asm/sn/pci/pci_bus_cvlink.h>

/*
 * For ATE allocations
 */
pciio_dmamap_t get_free_pciio_dmamap(vertex_hdl_t);
void free_pciio_dmamap(pcibr_dmamap_t);
static struct pcibr_dmamap_s *find_sn_dma_map(dma_addr_t, unsigned char);
void sn_pci_unmap_sg(struct pci_dev *hwdev, struct scatterlist *sg, int nents, int direction);

/*
 * Toplogy stuff
 */
extern vertex_hdl_t busnum_to_pcibr_vhdl[];
extern nasid_t busnum_to_nid[];
extern void * busnum_to_atedmamaps[];

/**
 * get_free_pciio_dmamap - find and allocate an ATE
 * @pci_bus: PCI bus to get an entry for
 *
 * Finds and allocates an ATE on the PCI bus specified
 * by @pci_bus.
 */
pciio_dmamap_t
get_free_pciio_dmamap(vertex_hdl_t pci_bus)
{
	int i;
	struct pcibr_dmamap_s *sn_dma_map = NULL;

	/*
	 * Darn, we need to get the maps allocated for this bus.
	 */
	for (i = 0; i < MAX_PCI_XWIDGET; i++) {
		if (busnum_to_pcibr_vhdl[i] == pci_bus) {
			sn_dma_map = busnum_to_atedmamaps[i];
		}
	}

	/*
	 * Now get a free dmamap entry from this list.
	 */
	for (i = 0; i < MAX_ATE_MAPS; i++, sn_dma_map++) {
		if (!sn_dma_map->bd_dma_addr) {
			sn_dma_map->bd_dma_addr = -1;
			return( (pciio_dmamap_t) sn_dma_map );
		}
	}

	return NULL;
}

/**
 * free_pciio_dmamap - free an ATE
 * @dma_map: ATE to free
 *
 * Frees the ATE specified by @dma_map.
 */
void
free_pciio_dmamap(pcibr_dmamap_t dma_map)
{
	dma_map->bd_dma_addr = 0;
}

/**
 * find_sn_dma_map - find an ATE associated with @dma_addr and @busnum
 * @dma_addr: DMA address to look for
 * @busnum: PCI bus to look on
 *
 * Finds the ATE associated with @dma_addr and @busnum.
 */
static struct pcibr_dmamap_s *
find_sn_dma_map(dma_addr_t dma_addr, unsigned char busnum)
{

	struct pcibr_dmamap_s *sn_dma_map = NULL;
	int i;

	sn_dma_map = busnum_to_atedmamaps[busnum];

	for (i = 0; i < MAX_ATE_MAPS; i++, sn_dma_map++) {
		if (sn_dma_map->bd_dma_addr == dma_addr) {
			return sn_dma_map;
		}
	}

	return NULL;
}

/**
 * sn_pci_alloc_consistent - allocate memory for coherent DMA
 * @hwdev: device to allocate for
 * @size: size of the region
 * @dma_handle: DMA (bus) address
 *
 * pci_alloc_consistent() returns a pointer to a memory region suitable for
 * coherent DMA traffic to/from a PCI device.  On SN platforms, this means
 * that @dma_handle will have the %PCIIO_DMA_CMD flag set.
 *
 * This interface is usually used for "command" streams (e.g. the command
 * queue for a SCSI controller).  See Documentation/DMA-mapping.txt for
 * more information.
 *
 * Also known as platform_pci_alloc_consistent() by the IA64 machvec code.
 */
void *
sn_pci_alloc_consistent(struct pci_dev *hwdev, size_t size, dma_addr_t *dma_handle)
{
        void *cpuaddr;
	vertex_hdl_t vhdl;
	struct sn_device_sysdata *device_sysdata;
	unsigned long phys_addr;
	pcibr_dmamap_t dma_map = 0;

	/*
	 * Get hwgraph vertex for the device
	 */
	device_sysdata = SN_DEVICE_SYSDATA(hwdev);
	vhdl = device_sysdata->vhdl;

	/*
	 * Allocate the memory.
	 * FIXME: We should be doing alloc_pages_node for the node closest
	 *        to the PCI device.
	 */
	if (!(cpuaddr = (void *)__get_free_pages(GFP_ATOMIC, get_order(size))))
		return NULL;

	memset(cpuaddr, 0x0, size);

	/* physical addr. of the memory we just got */
	phys_addr = __pa(cpuaddr);

	/*
	 * 64 bit address translations should never fail.
	 * 32 bit translations can fail if there are insufficient mapping
	 *   resources and the direct map is already wired to a different
	 *   2GB range.
	 * 32 bit translations can also return a > 32 bit address, because
	 *   pcibr_dmatrans_addr ignores a missing PCIIO_DMA_A64 flag on
	 *   PCI-X buses.
	 */
	if (hwdev->dev.coherent_dma_mask == ~0UL)
		*dma_handle = pcibr_dmatrans_addr(vhdl, NULL, phys_addr, size,
					  PCIIO_DMA_CMD | PCIIO_DMA_A64);
	else {
		dma_map = pcibr_dmamap_alloc(vhdl, NULL, size, PCIIO_DMA_CMD | 
					     MINIMAL_ATE_FLAG(phys_addr, size));
		if (dma_map) {
			*dma_handle = (dma_addr_t)
				pcibr_dmamap_addr(dma_map, phys_addr, size);
			dma_map->bd_dma_addr = *dma_handle;
		}
		else {
			*dma_handle = pcibr_dmatrans_addr(vhdl, NULL, phys_addr, size,
						  PCIIO_DMA_CMD);
		}
	}

	if (!*dma_handle || *dma_handle > hwdev->dev.coherent_dma_mask) {
		if (dma_map) {
			pcibr_dmamap_done(dma_map);
			pcibr_dmamap_free(dma_map);
		}
		free_pages((unsigned long) cpuaddr, get_order(size));
		return NULL;
	}

        return cpuaddr;
}

/**
 * sn_pci_free_consistent - free memory associated with coherent DMAable region
 * @hwdev: device to free for
 * @size: size to free
 * @vaddr: kernel virtual address to free
 * @dma_handle: DMA address associated with this region
 *
 * Frees the memory allocated by pci_alloc_consistent().  Also known
 * as platform_pci_free_consistent() by the IA64 machvec code.
 */
void
sn_pci_free_consistent(struct pci_dev *hwdev, size_t size, void *vaddr, dma_addr_t dma_handle)
{
	struct pcibr_dmamap_s *dma_map = NULL;

	/*
	 * Get the sn_dma_map entry.
	 */
	if (IS_PCI32_MAPPED(dma_handle))
		dma_map = find_sn_dma_map(dma_handle, hwdev->bus->number);

	/*
	 * and free it if necessary...
	 */
	if (dma_map) {
		pcibr_dmamap_done(dma_map);
		pcibr_dmamap_free(dma_map);
	}
	free_pages((unsigned long) vaddr, get_order(size));
}

/**
 * sn_pci_map_sg - map a scatter-gather list for DMA
 * @hwdev: device to map for
 * @sg: scatterlist to map
 * @nents: number of entries
 * @direction: direction of the DMA transaction
 *
 * Maps each entry of @sg for DMA.  Also known as platform_pci_map_sg by the
 * IA64 machvec code.
 */
int
sn_pci_map_sg(struct pci_dev *hwdev, struct scatterlist *sg, int nents, int direction)
{
	int i;
	vertex_hdl_t vhdl;
	unsigned long phys_addr;
	struct sn_device_sysdata *device_sysdata;
	pcibr_dmamap_t dma_map;
	struct scatterlist *saved_sg = sg;
	unsigned dma_flag;

	/* can't go anywhere w/o a direction in life */
	if (direction == PCI_DMA_NONE)
		BUG();

	/*
	 * Get the hwgraph vertex for the device
	 */
	device_sysdata = SN_DEVICE_SYSDATA(hwdev);
	vhdl = device_sysdata->vhdl;

	/*
	 * 64 bit DMA mask can use direct translations
	 * PCI only
	 *   32 bit DMA mask might be able to use direct, otherwise use dma map
	 * PCI-X
	 *   only 64 bit DMA mask supported; both direct and dma map will fail
	 */
	if (hwdev->dma_mask == ~0UL)
		dma_flag = PCIIO_DMA_DATA | PCIIO_DMA_A64;
	else
		dma_flag = PCIIO_DMA_DATA;

	/*
	 * Setup a DMA address for each entry in the
	 * scatterlist.
	 */
	for (i = 0; i < nents; i++, sg++) {
		phys_addr = __pa((unsigned long)page_address(sg->page) + sg->offset);
		sg->dma_address = pcibr_dmatrans_addr(vhdl, NULL, phys_addr,
					       sg->length, dma_flag);
		if (sg->dma_address) {
			sg->dma_length = sg->length;
			continue;
		}

		dma_map = pcibr_dmamap_alloc(vhdl, NULL, sg->length,
			PCIIO_DMA_DATA|MINIMAL_ATE_FLAG(phys_addr, sg->length));
		if (!dma_map) {
			printk(KERN_ERR "sn_pci_map_sg: Unable to allocate "
			       "anymore 32 bit page map entries.\n");
			/*
			 * We will need to free all previously allocated entries.
			 */
			if (i > 0) {
				sn_pci_unmap_sg(hwdev, saved_sg, i, direction);
			}
			return (0);
		}

		sg->dma_address = pcibr_dmamap_addr(dma_map, phys_addr, sg->length);
		sg->dma_length = sg->length;
		dma_map->bd_dma_addr = sg->dma_address;
	}

	return nents;

}

/**
 * sn_pci_unmap_sg - unmap a scatter-gather list
 * @hwdev: device to unmap
 * @sg: scatterlist to unmap
 * @nents: number of scatterlist entries
 * @direction: DMA direction
 *
 * Unmap a set of streaming mode DMA translations.  Again, cpu read rules
 * concerning calls here are the same as for pci_unmap_single() below.  Also
 * known as sn_pci_unmap_sg() by the IA64 machvec code.
 */
void
sn_pci_unmap_sg(struct pci_dev *hwdev, struct scatterlist *sg, int nents, int direction)
{
	int i;
	struct pcibr_dmamap_s *dma_map;

	/* can't go anywhere w/o a direction in life */
	if (direction == PCI_DMA_NONE)
		BUG();

	for (i = 0; i < nents; i++, sg++){

		if (IS_PCI32_MAPPED(sg->dma_address)) {
                	dma_map = find_sn_dma_map(sg->dma_address, hwdev->bus->number);
        		if (dma_map) {
                		pcibr_dmamap_done(dma_map);
                		pcibr_dmamap_free(dma_map);
			}
        	}

		sg->dma_address = (dma_addr_t)NULL;
		sg->dma_length = 0;
	}
}

/**
 * sn_pci_map_single - map a single region for DMA
 * @hwdev: device to map for
 * @ptr: kernel virtual address of the region to map
 * @size: size of the region
 * @direction: DMA direction
 *
 * Map the region pointed to by @ptr for DMA and return the
 * DMA address.   Also known as platform_pci_map_single() by
 * the IA64 machvec code.
 *
 * We map this to the one step pcibr_dmamap_trans interface rather than
 * the two step pcibr_dmamap_alloc/pcibr_dmamap_addr because we have
 * no way of saving the dmamap handle from the alloc to later free
 * (which is pretty much unacceptable).
 *
 * TODO: simplify our interface;
 *       get rid of dev_desc and vhdl (seems redundant given a pci_dev);
 *       figure out how to save dmamap handle so can use two step.
 */
dma_addr_t
sn_pci_map_single(struct pci_dev *hwdev, void *ptr, size_t size, int direction)
{
	vertex_hdl_t vhdl;
	dma_addr_t dma_addr;
	unsigned long phys_addr;
	struct sn_device_sysdata *device_sysdata;
	pcibr_dmamap_t dma_map = NULL;
	unsigned dma_flag;

	if (direction == PCI_DMA_NONE)
		BUG();

	/*
	 * find vertex for the device
	 */
	device_sysdata = SN_DEVICE_SYSDATA(hwdev);
	vhdl = device_sysdata->vhdl;

	phys_addr = __pa(ptr);
	/*
	 * 64 bit DMA mask can use direct translations
	 * PCI only
	 *   32 bit DMA mask might be able to use direct, otherwise use dma map
	 * PCI-X
	 *   only 64 bit DMA mask supported; both direct and dma map will fail
	 */
	if (hwdev->dma_mask == ~0UL)
		dma_flag = PCIIO_DMA_DATA | PCIIO_DMA_A64;
	else
		dma_flag = PCIIO_DMA_DATA;

	dma_addr = pcibr_dmatrans_addr(vhdl, NULL, phys_addr, size, dma_flag);
	if (dma_addr)
		return dma_addr;

	/*
	 * It's a 32 bit card and we cannot do direct mapping so
	 * let's use the PMU instead.
	 */
	dma_map = NULL;
	dma_map = pcibr_dmamap_alloc(vhdl, NULL, size, PCIIO_DMA_DATA | 
				     MINIMAL_ATE_FLAG(phys_addr, size));

	/* PMU out of entries */
	if (!dma_map)
		return 0;

	dma_addr = (dma_addr_t) pcibr_dmamap_addr(dma_map, phys_addr, size);
	dma_map->bd_dma_addr = dma_addr;

	return ((dma_addr_t)dma_addr);
}

/**
 * sn_pci_unmap_single - unmap a region used for DMA
 * @hwdev: device to unmap
 * @dma_addr: DMA address to unmap
 * @size: size of region
 * @direction: DMA direction
 *
 * Unmaps the region pointed to by @dma_addr.  Also known as
 * platform_pci_unmap_single() by the IA64 machvec code.
 */
void
sn_pci_unmap_single(struct pci_dev *hwdev, dma_addr_t dma_addr, size_t size, int direction)
{
	struct pcibr_dmamap_s *dma_map = NULL;

        if (direction == PCI_DMA_NONE)
		BUG();

	/*
	 * Get the sn_dma_map entry.
	 */
	if (IS_PCI32_MAPPED(dma_addr))
		dma_map = find_sn_dma_map(dma_addr, hwdev->bus->number);

	/*
	 * and free it if necessary...
	 */
	if (dma_map) {
		pcibr_dmamap_done(dma_map);
		pcibr_dmamap_free(dma_map);
	}
}

/**
 * sn_pci_dma_sync_single_* - make sure all DMAs or CPU accesses
 * have completed
 * @hwdev: device to sync
 * @dma_handle: DMA address to sync
 * @size: size of region
 * @direction: DMA direction
 *
 * This routine is supposed to sync the DMA region specified
 * by @dma_handle into the 'coherence domain'.  We do not need to do 
 * anything on our platform.
 */
void
sn_pci_dma_sync_single_for_cpu(struct pci_dev *hwdev, dma_addr_t dma_handle, size_t size, int direction)
{
	return;
}

void
sn_pci_dma_sync_single_for_device(struct pci_dev *hwdev, dma_addr_t dma_handle, size_t size, int direction)
{
	return;
}

/**
 * sn_pci_dma_sync_sg_* - make sure all DMAs or CPU accesses have completed
 * @hwdev: device to sync
 * @sg: scatterlist to sync
 * @nents: number of entries in the scatterlist
 * @direction: DMA direction
 *
 * This routine is supposed to sync the DMA regions specified
 * by @sg into the 'coherence domain'.  We do not need to do anything 
 * on our platform.
 */
void
sn_pci_dma_sync_sg_for_cpu(struct pci_dev *hwdev, struct scatterlist *sg, int nents, int direction)
{
	return;
}

void
sn_pci_dma_sync_sg_for_device(struct pci_dev *hwdev, struct scatterlist *sg, int nents, int direction)
{
	return;
}

/**
 * sn_dma_supported - test a DMA mask
 * @hwdev: device to test
 * @mask: DMA mask to test
 *
 * Return whether the given PCI device DMA address mask can be supported
 * properly.  For example, if your device can only drive the low 24-bits
 * during PCI bus mastering, then you would pass 0x00ffffff as the mask to
 * this function.  Of course, SN only supports devices that have 32 or more
 * address bits when using the PMU.  We could theoretically support <32 bit
 * cards using direct mapping, but we'll worry about that later--on the off
 * chance that someone actually wants to use such a card.
 */
int
sn_pci_dma_supported(struct pci_dev *hwdev, u64 mask)
{
	if (mask < 0xffffffff)
		return 0;
	return 1;
}

/*
 * New generic DMA routines just wrap sn2 PCI routines until we
 * support other bus types (if ever).
 */

int
sn_dma_supported(struct device *dev, u64 mask)
{
	BUG_ON(dev->bus != &pci_bus_type);

	return sn_pci_dma_supported(to_pci_dev(dev), mask);
}
EXPORT_SYMBOL(sn_dma_supported);

int
sn_dma_set_mask(struct device *dev, u64 dma_mask)
{
	BUG_ON(dev->bus != &pci_bus_type);

	if (!sn_dma_supported(dev, dma_mask))
		return 0;

	*dev->dma_mask = dma_mask;
	return 1;
}
EXPORT_SYMBOL(sn_dma_set_mask);

void *
sn_dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle,
		   int flag)
{
	BUG_ON(dev->bus != &pci_bus_type);

	return sn_pci_alloc_consistent(to_pci_dev(dev), size, dma_handle);
}
EXPORT_SYMBOL(sn_dma_alloc_coherent);

void
sn_dma_free_coherent(struct device *dev, size_t size, void *cpu_addr,
		    dma_addr_t dma_handle)
{
	BUG_ON(dev->bus != &pci_bus_type);

	sn_pci_free_consistent(to_pci_dev(dev), size, cpu_addr, dma_handle);
}
EXPORT_SYMBOL(sn_dma_free_coherent);

dma_addr_t
sn_dma_map_single(struct device *dev, void *cpu_addr, size_t size,
	       int direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	return sn_pci_map_single(to_pci_dev(dev), cpu_addr, size, (int)direction);
}
EXPORT_SYMBOL(sn_dma_map_single);

void
sn_dma_unmap_single(struct device *dev, dma_addr_t dma_addr, size_t size,
		 int direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	sn_pci_unmap_single(to_pci_dev(dev), dma_addr, size, (int)direction);
}
EXPORT_SYMBOL(sn_dma_unmap_single);

dma_addr_t
sn_dma_map_page(struct device *dev, struct page *page,
	     unsigned long offset, size_t size,
	     int direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	return pci_map_page(to_pci_dev(dev), page, offset, size, (int)direction);
}
EXPORT_SYMBOL(sn_dma_map_page);

void
sn_dma_unmap_page(struct device *dev, dma_addr_t dma_address, size_t size,
	       int direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	pci_unmap_page(to_pci_dev(dev), dma_address, size, (int)direction);
}
EXPORT_SYMBOL(sn_dma_unmap_page);

int
sn_dma_map_sg(struct device *dev, struct scatterlist *sg, int nents,
	   int direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	return sn_pci_map_sg(to_pci_dev(dev), sg, nents, (int)direction);
}
EXPORT_SYMBOL(sn_dma_map_sg);

void
sn_dma_unmap_sg(struct device *dev, struct scatterlist *sg, int nhwentries,
	     int direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	sn_pci_unmap_sg(to_pci_dev(dev), sg, nhwentries, (int)direction);
}
EXPORT_SYMBOL(sn_dma_unmap_sg);

void
sn_dma_sync_single_for_cpu(struct device *dev, dma_addr_t dma_handle, size_t size,
			   int direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	sn_pci_dma_sync_single_for_cpu(to_pci_dev(dev), dma_handle, size, (int)direction);
}
EXPORT_SYMBOL(sn_dma_sync_single_for_cpu);

void
sn_dma_sync_single_for_device(struct device *dev, dma_addr_t dma_handle, size_t size,
		int direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	sn_pci_dma_sync_single_for_device(to_pci_dev(dev), dma_handle, size, (int)direction);
}
EXPORT_SYMBOL(sn_dma_sync_single_for_device);

void
sn_dma_sync_sg_for_cpu(struct device *dev, struct scatterlist *sg, int nelems,
	    int direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	sn_pci_dma_sync_sg_for_cpu(to_pci_dev(dev), sg, nelems, (int)direction);
}
EXPORT_SYMBOL(sn_dma_sync_sg_for_cpu);

void
sn_dma_sync_sg_for_device(struct device *dev, struct scatterlist *sg, int nelems,
	    int direction)
{
	BUG_ON(dev->bus != &pci_bus_type);

	sn_pci_dma_sync_sg_for_device(to_pci_dev(dev), sg, nelems, (int)direction);
}
EXPORT_SYMBOL(sn_dma_sync_sg_for_device);

int
sn_dma_mapping_error(dma_addr_t dma_addr)
{
	/*
	 * We can only run out of page mapping entries, so if there's
	 * an error, tell the caller to try again later.
	 */
	if (!dma_addr)
		return -EAGAIN;
	return 0;
}

EXPORT_SYMBOL(sn_dma_mapping_error);
EXPORT_SYMBOL(sn_pci_unmap_single);
EXPORT_SYMBOL(sn_pci_map_single);
EXPORT_SYMBOL(sn_pci_dma_sync_single_for_cpu);
EXPORT_SYMBOL(sn_pci_dma_sync_single_for_device);
EXPORT_SYMBOL(sn_pci_dma_sync_sg_for_cpu);
EXPORT_SYMBOL(sn_pci_dma_sync_sg_for_device);
EXPORT_SYMBOL(sn_pci_map_sg);
EXPORT_SYMBOL(sn_pci_unmap_sg);
EXPORT_SYMBOL(sn_pci_alloc_consistent);
EXPORT_SYMBOL(sn_pci_free_consistent);
EXPORT_SYMBOL(sn_pci_dma_supported);

