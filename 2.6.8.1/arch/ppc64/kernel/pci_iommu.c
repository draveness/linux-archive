/*
 * arch/ppc64/kernel/pci_iommu.c
 * Copyright (C) 2001 Mike Corrigan & Dave Engebretsen, IBM Corporation
 * 
 * Rewrite, cleanup, new allocation schemes: 
 * Copyright (C) 2004 Olof Johansson, IBM Corporation
 *
 * Dynamic DMA mapping support, platform-independent parts.
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
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <asm/io.h>
#include <asm/prom.h>
#include <asm/iommu.h>
#include <asm/pci-bridge.h>
#include <asm/machdep.h>
#include "pci.h"

#ifdef CONFIG_PPC_ISERIES
#include <asm/iSeries/iSeries_pci.h>
#endif /* CONFIG_PPC_ISERIES */

static inline struct iommu_table *devnode_table(struct pci_dev *dev)
{
	if (!dev)
		dev = ppc64_isabridge_dev;
	if (!dev)
		return NULL;

#ifdef CONFIG_PPC_ISERIES
	return ISERIES_DEVNODE(dev)->iommu_table;
#endif /* CONFIG_PPC_ISERIES */

#ifdef CONFIG_PPC_PSERIES
	return PCI_GET_DN(dev)->iommu_table;
#endif /* CONFIG_PPC_PSERIES */
}


/* Allocates a contiguous real buffer and creates mappings over it.
 * Returns the virtual address of the buffer and sets dma_handle
 * to the dma address (mapping) of the first page.
 */
static void *pci_iommu_alloc_consistent(struct pci_dev *hwdev, size_t size,
			   dma_addr_t *dma_handle)
{
	return iommu_alloc_consistent(devnode_table(hwdev), size, dma_handle);
}

static void pci_iommu_free_consistent(struct pci_dev *hwdev, size_t size,
			 void *vaddr, dma_addr_t dma_handle)
{
	iommu_free_consistent(devnode_table(hwdev), size, vaddr, dma_handle);
}

/* Creates TCEs for a user provided buffer.  The user buffer must be 
 * contiguous real kernel storage (not vmalloc).  The address of the buffer
 * passed here is the kernel (virtual) address of the buffer.  The buffer
 * need not be page aligned, the dma_addr_t returned will point to the same
 * byte within the page as vaddr.
 */
static dma_addr_t pci_iommu_map_single(struct pci_dev *hwdev, void *vaddr,
		size_t size, enum dma_data_direction direction)
{
	return iommu_map_single(devnode_table(hwdev), vaddr, size, direction);
}


static void pci_iommu_unmap_single(struct pci_dev *hwdev, dma_addr_t dma_handle,
		size_t size, enum dma_data_direction direction)
{
	iommu_unmap_single(devnode_table(hwdev), dma_handle, size, direction);
}


static int pci_iommu_map_sg(struct pci_dev *pdev, struct scatterlist *sglist,
		int nelems, enum dma_data_direction direction)
{
	return iommu_map_sg(&pdev->dev, devnode_table(pdev), sglist,
			nelems, direction);
}

static void pci_iommu_unmap_sg(struct pci_dev *pdev, struct scatterlist *sglist,
		int nelems, enum dma_data_direction direction)
{
	iommu_unmap_sg(devnode_table(pdev), sglist, nelems, direction);
}

/* We support DMA to/from any memory page via the iommu */
static int pci_iommu_dma_supported(struct pci_dev *pdev, u64 mask)
{
	return 1;
}

void pci_iommu_init(void)
{
	pci_dma_ops.pci_alloc_consistent = pci_iommu_alloc_consistent;
	pci_dma_ops.pci_free_consistent = pci_iommu_free_consistent;
	pci_dma_ops.pci_map_single = pci_iommu_map_single;
	pci_dma_ops.pci_unmap_single = pci_iommu_unmap_single;
	pci_dma_ops.pci_map_sg = pci_iommu_map_sg;
	pci_dma_ops.pci_unmap_sg = pci_iommu_unmap_sg;
	pci_dma_ops.pci_dma_supported = pci_iommu_dma_supported;
}
