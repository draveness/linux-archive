/**
 * \file ati_pcigart.h 
 * ATI PCI GART support
 *
 * \author Gareth Hughes <gareth@valinux.com>
 */

/*
 * Created: Wed Dec 13 21:52:19 2000 by gareth@valinux.com
 *
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "drmP.h"

#if PAGE_SIZE == 65536
# define ATI_PCIGART_TABLE_ORDER	0
# define ATI_PCIGART_TABLE_PAGES	(1 << 0)
#elif PAGE_SIZE == 16384
# define ATI_PCIGART_TABLE_ORDER	1
# define ATI_PCIGART_TABLE_PAGES	(1 << 1)
#elif PAGE_SIZE == 8192
# define ATI_PCIGART_TABLE_ORDER 	2
# define ATI_PCIGART_TABLE_PAGES 	(1 << 2)
#elif PAGE_SIZE == 4096
# define ATI_PCIGART_TABLE_ORDER 	3
# define ATI_PCIGART_TABLE_PAGES 	(1 << 3)
#else
# error - PAGE_SIZE not 64K, 16K, 8K or 4K
#endif

# define ATI_MAX_PCIGART_PAGES		8192	/**< 32 MB aperture, 4K pages */
# define ATI_PCIGART_PAGE_SIZE		4096	/**< PCI GART page size */

static unsigned long DRM(ati_alloc_pcigart_table)( void )
{
	unsigned long address;
	struct page *page;
	int i;
	DRM_DEBUG( "%s\n", __FUNCTION__ );

	address = __get_free_pages( GFP_KERNEL, ATI_PCIGART_TABLE_ORDER );
	if ( address == 0UL ) {
		return 0;
	}

	page = virt_to_page( address );

	for ( i = 0 ; i < ATI_PCIGART_TABLE_PAGES ; i++, page++ ) {
		get_page(page);
		SetPageReserved( page );
	}

	DRM_DEBUG( "%s: returning 0x%08lx\n", __FUNCTION__, address );
	return address;
}

static void DRM(ati_free_pcigart_table)( unsigned long address )
{
	struct page *page;
	int i;
	DRM_DEBUG( "%s\n", __FUNCTION__ );

	page = virt_to_page( address );

	for ( i = 0 ; i < ATI_PCIGART_TABLE_PAGES ; i++, page++ ) {
		__put_page(page);
		ClearPageReserved( page );
	}

	free_pages( address, ATI_PCIGART_TABLE_ORDER );
}

int DRM(ati_pcigart_init)( drm_device_t *dev,
			   unsigned long *addr,
			   dma_addr_t *bus_addr)
{
	drm_sg_mem_t *entry = dev->sg;
	unsigned long address = 0;
	unsigned long pages;
	u32 *pci_gart, page_base, bus_address = 0;
	int i, j, ret = 0;

	if ( !entry ) {
		DRM_ERROR( "no scatter/gather memory!\n" );
		goto done;
	}

	address = DRM(ati_alloc_pcigart_table)();
	if ( !address ) {
		DRM_ERROR( "cannot allocate PCI GART page!\n" );
		goto done;
	}

	if ( !dev->pdev ) {
		DRM_ERROR( "PCI device unknown!\n" );
		goto done;
	}

	bus_address = pci_map_single(dev->pdev, (void *)address,
				  ATI_PCIGART_TABLE_PAGES * PAGE_SIZE,
				  PCI_DMA_TODEVICE);
	if (bus_address == 0) {
		DRM_ERROR( "unable to map PCIGART pages!\n" );
		DRM(ati_free_pcigart_table)( address );
		address = 0;
		goto done;
	}

	pci_gart = (u32 *)address;

	pages = ( entry->pages <= ATI_MAX_PCIGART_PAGES )
		? entry->pages : ATI_MAX_PCIGART_PAGES;

	memset( pci_gart, 0, ATI_MAX_PCIGART_PAGES * sizeof(u32) );

	for ( i = 0 ; i < pages ; i++ ) {
		/* we need to support large memory configurations */
		entry->busaddr[i] = pci_map_single(dev->pdev,
					   page_address( entry->pagelist[i] ),
					   PAGE_SIZE,
					   PCI_DMA_TODEVICE);
		if (entry->busaddr[i] == 0) {
			DRM_ERROR( "unable to map PCIGART pages!\n" );
			DRM(ati_pcigart_cleanup)( dev, address, bus_address );
			address = 0;
			bus_address = 0;
			goto done;
		}
		page_base = (u32) entry->busaddr[i];

		for (j = 0; j < (PAGE_SIZE / ATI_PCIGART_PAGE_SIZE); j++) {
			*pci_gart++ = cpu_to_le32( page_base );
			page_base += ATI_PCIGART_PAGE_SIZE;
		}
	}

	ret = 1;

#if defined(__i386__) || defined(__x86_64__)
	asm volatile ( "wbinvd" ::: "memory" );
#else
	mb();
#endif

done:
	*addr = address;
	*bus_addr = bus_address;
	return ret;
}

int DRM(ati_pcigart_cleanup)( drm_device_t *dev,
			      unsigned long addr,
			      dma_addr_t bus_addr)
{
	drm_sg_mem_t *entry = dev->sg;
	unsigned long pages;
	int i;

	/* we need to support large memory configurations */
	if ( !entry ) {
		DRM_ERROR( "no scatter/gather memory!\n" );
		return 0;
	}

	if ( bus_addr ) {
		pci_unmap_single(dev->pdev, bus_addr,
				 ATI_PCIGART_TABLE_PAGES * PAGE_SIZE,
				 PCI_DMA_TODEVICE);

		pages = ( entry->pages <= ATI_MAX_PCIGART_PAGES )
		        ? entry->pages : ATI_MAX_PCIGART_PAGES;

		for ( i = 0 ; i < pages ; i++ ) {
			if ( !entry->busaddr[i] ) break;
			pci_unmap_single(dev->pdev, entry->busaddr[i],
					 PAGE_SIZE, PCI_DMA_TODEVICE);
		}
	}

	if ( addr ) {
		DRM(ati_free_pcigart_table)( addr );
	}

	return 1;
}
