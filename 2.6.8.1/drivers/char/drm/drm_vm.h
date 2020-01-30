/**
 * \file drm_vm.h
 * Memory mapping for DRM
 * 
 * \author Rickard E. (Rik) Faith <faith@valinux.com>
 * \author Gareth Hughes <gareth@valinux.com>
 */

/*
 * Created: Mon Jan  4 08:58:31 1999 by faith@valinux.com
 *
 * Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
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
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "drmP.h"


/**
 * \c nopage method for AGP virtual memory.
 *
 * \param vma virtual memory area.
 * \param address access address.
 * \return pointer to the page structure.
 * 
 * Find the right map and if it's AGP memory find the real physical page to
 * map, get the page, increment the use count and return it.
 */
static __inline__ struct page *DRM(do_vm_nopage)(struct vm_area_struct *vma,
						 unsigned long address)
{
#if __REALLY_HAVE_AGP
	drm_file_t *priv  = vma->vm_file->private_data;
	drm_device_t *dev = priv->dev;
	drm_map_t *map    = NULL;
	drm_map_list_t  *r_list;
	struct list_head *list;

	/*
         * Find the right map
         */

	if(!dev->agp || !dev->agp->cant_use_aperture) goto vm_nopage_error;

	list_for_each(list, &dev->maplist->head) {
		r_list = list_entry(list, drm_map_list_t, head);
		map = r_list->map;
		if (!map) continue;
		if (map->offset == VM_OFFSET(vma)) break;
	}

	if (map && map->type == _DRM_AGP) {
		unsigned long offset = address - vma->vm_start;
		unsigned long baddr = VM_OFFSET(vma) + offset;
		struct drm_agp_mem *agpmem;
		struct page *page;

#ifdef __alpha__
		/*
                 * Adjust to a bus-relative address
                 */
		baddr -= dev->hose->mem_space->start;
#endif

		/*
                 * It's AGP memory - find the real physical page to map
                 */
		for(agpmem = dev->agp->memory; agpmem; agpmem = agpmem->next) {
			if (agpmem->bound <= baddr &&
			    agpmem->bound + agpmem->pages * PAGE_SIZE > baddr) 
				break;
		}

		if (!agpmem) goto vm_nopage_error;

		/*
                 * Get the page, inc the use count, and return it
                 */
		offset = (baddr - agpmem->bound) >> PAGE_SHIFT;
		page = virt_to_page(__va(agpmem->memory->memory[offset]));
		get_page(page);

		DRM_DEBUG("baddr = 0x%lx page = 0x%p, offset = 0x%lx, count=%d\n",
			  baddr, __va(agpmem->memory->memory[offset]), offset,
			  page_count(page));

		return page;
        }
vm_nopage_error:
#endif /* __REALLY_HAVE_AGP */

	return NOPAGE_SIGBUS;		/* Disallow mremap */
}

/**
 * \c nopage method for shared virtual memory.
 *
 * \param vma virtual memory area.
 * \param address access address.
 * \return pointer to the page structure.
 * 
 * Get the the mapping, find the real physical page to map, get the page, and
 * return it.
 */
static __inline__ struct page *DRM(do_vm_shm_nopage)(struct vm_area_struct *vma,
						     unsigned long address)
{
	drm_map_t	 *map	 = (drm_map_t *)vma->vm_private_data;
	unsigned long	 offset;
	unsigned long	 i;
	struct page	 *page;

	if (address > vma->vm_end) return NOPAGE_SIGBUS; /* Disallow mremap */
	if (!map)    		   return NOPAGE_OOM;  /* Nothing allocated */

	offset	 = address - vma->vm_start;
	i = (unsigned long)map->handle + offset;
	page = vmalloc_to_page((void *)i);
	if (!page)
		return NOPAGE_OOM;
	get_page(page);

	DRM_DEBUG("shm_nopage 0x%lx\n", address);
	return page;
}


/**
 * \c close method for shared virtual memory.
 * 
 * \param vma virtual memory area.
 * 
 * Deletes map information if we are the last
 * person to close a mapping and it's not in the global maplist.
 */
void DRM(vm_shm_close)(struct vm_area_struct *vma)
{
	drm_file_t	*priv	= vma->vm_file->private_data;
	drm_device_t	*dev	= priv->dev;
	drm_vma_entry_t *pt, *prev, *next;
	drm_map_t *map;
	drm_map_list_t *r_list;
	struct list_head *list;
	int found_maps = 0;

	DRM_DEBUG("0x%08lx,0x%08lx\n",
		  vma->vm_start, vma->vm_end - vma->vm_start);
	atomic_dec(&dev->vma_count);

	map = vma->vm_private_data;

	down(&dev->struct_sem);
	for (pt = dev->vmalist, prev = NULL; pt; pt = next) {
		next = pt->next;
		if (pt->vma->vm_private_data == map) found_maps++;
		if (pt->vma == vma) {
			if (prev) {
				prev->next = pt->next;
			} else {
				dev->vmalist = pt->next;
			}
			DRM(free)(pt, sizeof(*pt), DRM_MEM_VMAS);
		} else {
			prev = pt;
		}
	}
	/* We were the only map that was found */
	if(found_maps == 1 &&
	   map->flags & _DRM_REMOVABLE) {
		/* Check to see if we are in the maplist, if we are not, then
		 * we delete this mappings information.
		 */
		found_maps = 0;
		list = &dev->maplist->head;
		list_for_each(list, &dev->maplist->head) {
			r_list = list_entry(list, drm_map_list_t, head);
			if (r_list->map == map) found_maps++;
		}

		if(!found_maps) {
			switch (map->type) {
			case _DRM_REGISTERS:
			case _DRM_FRAME_BUFFER:
#if __REALLY_HAVE_MTRR
				if (map->mtrr >= 0) {
					int retcode;
					retcode = mtrr_del(map->mtrr,
							   map->offset,
							   map->size);
					DRM_DEBUG("mtrr_del = %d\n", retcode);
				}
#endif
				DRM(ioremapfree)(map->handle, map->size, dev);
				break;
			case _DRM_SHM:
				vfree(map->handle);
				break;
			case _DRM_AGP:
			case _DRM_SCATTER_GATHER:
				break;
			}
			DRM(free)(map, sizeof(*map), DRM_MEM_MAPS);
		}
	}
	up(&dev->struct_sem);
}

/**
 * \c nopage method for DMA virtual memory.
 *
 * \param vma virtual memory area.
 * \param address access address.
 * \return pointer to the page structure.
 * 
 * Determine the page number from the page offset and get it from drm_device_dma::pagelist.
 */
static __inline__ struct page *DRM(do_vm_dma_nopage)(struct vm_area_struct *vma,
						     unsigned long address)
{
	drm_file_t	 *priv	 = vma->vm_file->private_data;
	drm_device_t	 *dev	 = priv->dev;
	drm_device_dma_t *dma	 = dev->dma;
	unsigned long	 offset;
	unsigned long	 page_nr;
	struct page	 *page;

	if (!dma)		   return NOPAGE_SIGBUS; /* Error */
	if (address > vma->vm_end) return NOPAGE_SIGBUS; /* Disallow mremap */
	if (!dma->pagelist)	   return NOPAGE_OOM ; /* Nothing allocated */

	offset	 = address - vma->vm_start; /* vm_[pg]off[set] should be 0 */
	page_nr  = offset >> PAGE_SHIFT;
	page = virt_to_page((dma->pagelist[page_nr] + 
			     (offset & (~PAGE_MASK))));

	get_page(page);

	DRM_DEBUG("dma_nopage 0x%lx (page %lu)\n", address, page_nr);
	return page;
}

/**
 * \c nopage method for scatter-gather virtual memory.
 *
 * \param vma virtual memory area.
 * \param address access address.
 * \return pointer to the page structure.
 * 
 * Determine the map offset from the page offset and get it from drm_sg_mem::pagelist.
 */
static __inline__ struct page *DRM(do_vm_sg_nopage)(struct vm_area_struct *vma,
						    unsigned long address)
{
	drm_map_t        *map    = (drm_map_t *)vma->vm_private_data;
	drm_file_t *priv = vma->vm_file->private_data;
	drm_device_t *dev = priv->dev;
	drm_sg_mem_t *entry = dev->sg;
	unsigned long offset;
	unsigned long map_offset;
	unsigned long page_offset;
	struct page *page;

	if (!entry)                return NOPAGE_SIGBUS; /* Error */
	if (address > vma->vm_end) return NOPAGE_SIGBUS; /* Disallow mremap */
	if (!entry->pagelist)      return NOPAGE_OOM ;  /* Nothing allocated */


	offset = address - vma->vm_start;
	map_offset = map->offset - dev->sg->handle;
	page_offset = (offset >> PAGE_SHIFT) + (map_offset >> PAGE_SHIFT);
	page = entry->pagelist[page_offset];
	get_page(page);

	return page;
}


#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,0)

static struct page *DRM(vm_nopage)(struct vm_area_struct *vma,
				   unsigned long address,
				   int *type) {
	if (type) *type = VM_FAULT_MINOR;
	return DRM(do_vm_nopage)(vma, address);
}

static struct page *DRM(vm_shm_nopage)(struct vm_area_struct *vma,
				       unsigned long address,
				       int *type) {
	if (type) *type = VM_FAULT_MINOR;
	return DRM(do_vm_shm_nopage)(vma, address);
}

static struct page *DRM(vm_dma_nopage)(struct vm_area_struct *vma,
				       unsigned long address,
				       int *type) {
	if (type) *type = VM_FAULT_MINOR;
	return DRM(do_vm_dma_nopage)(vma, address);
}

static struct page *DRM(vm_sg_nopage)(struct vm_area_struct *vma,
				      unsigned long address,
				      int *type) {
	if (type) *type = VM_FAULT_MINOR;
	return DRM(do_vm_sg_nopage)(vma, address);
}

#else	/* LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,0) */

static struct page *DRM(vm_nopage)(struct vm_area_struct *vma,
				   unsigned long address,
				   int unused) {
	return DRM(do_vm_nopage)(vma, address);
}

static struct page *DRM(vm_shm_nopage)(struct vm_area_struct *vma,
				       unsigned long address,
				       int unused) {
	return DRM(do_vm_shm_nopage)(vma, address);
}

static struct page *DRM(vm_dma_nopage)(struct vm_area_struct *vma,
				       unsigned long address,
				       int unused) {
	return DRM(do_vm_dma_nopage)(vma, address);
}

static struct page *DRM(vm_sg_nopage)(struct vm_area_struct *vma,
				      unsigned long address,
				      int unused) {
	return DRM(do_vm_sg_nopage)(vma, address);
}

#endif


/** AGP virtual memory operations */
static struct vm_operations_struct   DRM(vm_ops) = {
	.nopage = DRM(vm_nopage),
	.open	= DRM(vm_open),
	.close	= DRM(vm_close),
};

/** Shared virtual memory operations */
static struct vm_operations_struct   DRM(vm_shm_ops) = {
	.nopage = DRM(vm_shm_nopage),
	.open	= DRM(vm_open),
	.close	= DRM(vm_shm_close),
};

/** DMA virtual memory operations */
static struct vm_operations_struct   DRM(vm_dma_ops) = {
	.nopage = DRM(vm_dma_nopage),
	.open	= DRM(vm_open),
	.close	= DRM(vm_close),
};

/** Scatter-gather virtual memory operations */
static struct vm_operations_struct   DRM(vm_sg_ops) = {
	.nopage = DRM(vm_sg_nopage),
	.open   = DRM(vm_open),
	.close  = DRM(vm_close),
};


/**
 * \c open method for shared virtual memory.
 * 
 * \param vma virtual memory area.
 * 
 * Create a new drm_vma_entry structure as the \p vma private data entry and
 * add it to drm_device::vmalist.
 */
void DRM(vm_open)(struct vm_area_struct *vma)
{
	drm_file_t	*priv	= vma->vm_file->private_data;
	drm_device_t	*dev	= priv->dev;
	drm_vma_entry_t *vma_entry;

	DRM_DEBUG("0x%08lx,0x%08lx\n",
		  vma->vm_start, vma->vm_end - vma->vm_start);
	atomic_inc(&dev->vma_count);

	vma_entry = DRM(alloc)(sizeof(*vma_entry), DRM_MEM_VMAS);
	if (vma_entry) {
		down(&dev->struct_sem);
		vma_entry->vma	= vma;
		vma_entry->next = dev->vmalist;
		vma_entry->pid	= current->pid;
		dev->vmalist	= vma_entry;
		up(&dev->struct_sem);
	}
}

/**
 * \c close method for all virtual memory types.
 * 
 * \param vma virtual memory area.
 * 
 * Search the \p vma private data entry in drm_device::vmalist, unlink it, and
 * free it.
 */
void DRM(vm_close)(struct vm_area_struct *vma)
{
	drm_file_t	*priv	= vma->vm_file->private_data;
	drm_device_t	*dev	= priv->dev;
	drm_vma_entry_t *pt, *prev;

	DRM_DEBUG("0x%08lx,0x%08lx\n",
		  vma->vm_start, vma->vm_end - vma->vm_start);
	atomic_dec(&dev->vma_count);

	down(&dev->struct_sem);
	for (pt = dev->vmalist, prev = NULL; pt; prev = pt, pt = pt->next) {
		if (pt->vma == vma) {
			if (prev) {
				prev->next = pt->next;
			} else {
				dev->vmalist = pt->next;
			}
			DRM(free)(pt, sizeof(*pt), DRM_MEM_VMAS);
			break;
		}
	}
	up(&dev->struct_sem);
}

/**
 * mmap DMA memory.
 *
 * \param filp file pointer.
 * \param vma virtual memory area.
 * \return zero on success or a negative number on failure.
 * 
 * Sets the virtual memory area operations structure to vm_dma_ops, the file
 * pointer, and calls vm_open().
 */
int DRM(mmap_dma)(struct file *filp, struct vm_area_struct *vma)
{
	drm_file_t	 *priv	 = filp->private_data;
	drm_device_t	 *dev;
	drm_device_dma_t *dma;
	unsigned long	 length	 = vma->vm_end - vma->vm_start;

	lock_kernel();
	dev	 = priv->dev;
	dma	 = dev->dma;
	DRM_DEBUG("start = 0x%lx, end = 0x%lx, offset = 0x%lx\n",
		  vma->vm_start, vma->vm_end, VM_OFFSET(vma));

				/* Length must match exact page count */
	if (!dma || (length >> PAGE_SHIFT) != dma->page_count) {
		unlock_kernel();
		return -EINVAL;
	}
	unlock_kernel();

	vma->vm_ops   = &DRM(vm_dma_ops);

#if LINUX_VERSION_CODE <= 0x02040e /* KERNEL_VERSION(2,4,14) */
	vma->vm_flags |= VM_LOCKED | VM_SHM; /* Don't swap */
#else
	vma->vm_flags |= VM_RESERVED; /* Don't swap */
#endif

	vma->vm_file  =	 filp;	/* Needed for drm_vm_open() */
	DRM(vm_open)(vma);
	return 0;
}

#ifndef DRIVER_GET_MAP_OFS
#define DRIVER_GET_MAP_OFS()	(map->offset)
#endif

#ifndef DRIVER_GET_REG_OFS
#ifdef __alpha__
#define DRIVER_GET_REG_OFS()	(dev->hose->dense_mem_base -	\
				 dev->hose->mem_space->start)
#else
#define DRIVER_GET_REG_OFS()	0
#endif
#endif

/**
 * mmap DMA memory.
 *
 * \param filp file pointer.
 * \param vma virtual memory area.
 * \return zero on success or a negative number on failure.
 * 
 * If the virtual memory area has no offset associated with it then it's a DMA
 * area, so calls mmap_dma(). Otherwise searches the map in drm_device::maplist,
 * checks that the restricted flag is not set, sets the virtual memory operations
 * according to the mapping type and remaps the pages. Finally sets the file
 * pointer and calls vm_open().
 */
int DRM(mmap)(struct file *filp, struct vm_area_struct *vma)
{
	drm_file_t	*priv	= filp->private_data;
	drm_device_t	*dev	= priv->dev;
	drm_map_t	*map	= NULL;
	drm_map_list_t  *r_list;
	unsigned long   offset  = 0;
	struct list_head *list;

	DRM_DEBUG("start = 0x%lx, end = 0x%lx, offset = 0x%lx\n",
		  vma->vm_start, vma->vm_end, VM_OFFSET(vma));

	if ( !priv->authenticated ) return -EACCES;

	/* We check for "dma". On Apple's UniNorth, it's valid to have
	 * the AGP mapped at physical address 0
	 * --BenH.
	 */
	if (!VM_OFFSET(vma)
#if __REALLY_HAVE_AGP
	    && (!dev->agp || dev->agp->agp_info.device->vendor != PCI_VENDOR_ID_APPLE)
#endif
	    )
		return DRM(mmap_dma)(filp, vma);

				/* A sequential search of a linked list is
				   fine here because: 1) there will only be
				   about 5-10 entries in the list and, 2) a
				   DRI client only has to do this mapping
				   once, so it doesn't have to be optimized
				   for performance, even if the list was a
				   bit longer. */
	list_for_each(list, &dev->maplist->head) {
		unsigned long off;

		r_list = list_entry(list, drm_map_list_t, head);
		map = r_list->map;
		if (!map) continue;
		off = DRIVER_GET_MAP_OFS();
		if (off == VM_OFFSET(vma)) break;
	}

	if (!map || ((map->flags&_DRM_RESTRICTED) && !capable(CAP_SYS_ADMIN)))
		return -EPERM;

				/* Check for valid size. */
	if (map->size != vma->vm_end - vma->vm_start) return -EINVAL;

	if (!capable(CAP_SYS_ADMIN) && (map->flags & _DRM_READ_ONLY)) {
		vma->vm_flags &= ~(VM_WRITE | VM_MAYWRITE);
#if defined(__i386__) || defined(__x86_64__)
		pgprot_val(vma->vm_page_prot) &= ~_PAGE_RW;
#else
				/* Ye gads this is ugly.  With more thought
                                   we could move this up higher and use
                                   `protection_map' instead.  */
		vma->vm_page_prot = __pgprot(pte_val(pte_wrprotect(
			__pte(pgprot_val(vma->vm_page_prot)))));
#endif
	}

	switch (map->type) {
        case _DRM_AGP:
#if __REALLY_HAVE_AGP
	  if (dev->agp->cant_use_aperture) {
                /*
                 * On some platforms we can't talk to bus dma address from the CPU, so for
                 * memory of type DRM_AGP, we'll deal with sorting out the real physical
                 * pages and mappings in nopage()
                 */
#if defined(__powerpc__)
		pgprot_val(vma->vm_page_prot) |= _PAGE_NO_CACHE;
#endif
                vma->vm_ops = &DRM(vm_ops);
                break;
	  }
#endif
                /* fall through to _DRM_FRAME_BUFFER... */        
	case _DRM_FRAME_BUFFER:
	case _DRM_REGISTERS:
		if (VM_OFFSET(vma) >= __pa(high_memory)) {
#if defined(__i386__) || defined(__x86_64__)
			if (boot_cpu_data.x86 > 3 && map->type != _DRM_AGP) {
				pgprot_val(vma->vm_page_prot) |= _PAGE_PCD;
				pgprot_val(vma->vm_page_prot) &= ~_PAGE_PWT;
			}
#elif defined(__powerpc__)
			pgprot_val(vma->vm_page_prot) |= _PAGE_NO_CACHE | _PAGE_GUARDED;
#endif
			vma->vm_flags |= VM_IO;	/* not in core dump */
		}
#if defined(__ia64__)
		if (map->type != _DRM_AGP)
			vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
#endif
		offset = DRIVER_GET_REG_OFS();
#ifdef __sparc__
		if (io_remap_page_range(DRM_RPR_ARG(vma) vma->vm_start,
					VM_OFFSET(vma) + offset,
					vma->vm_end - vma->vm_start,
					vma->vm_page_prot, 0))
#else
		if (remap_page_range(DRM_RPR_ARG(vma) vma->vm_start,
				     VM_OFFSET(vma) + offset,
				     vma->vm_end - vma->vm_start,
				     vma->vm_page_prot))
#endif
				return -EAGAIN;
		DRM_DEBUG("   Type = %d; start = 0x%lx, end = 0x%lx,"
			  " offset = 0x%lx\n",
			  map->type,
			  vma->vm_start, vma->vm_end, VM_OFFSET(vma) + offset);
		vma->vm_ops = &DRM(vm_ops);
		break;
	case _DRM_SHM:
		vma->vm_ops = &DRM(vm_shm_ops);
		vma->vm_private_data = (void *)map;
				/* Don't let this area swap.  Change when
				   DRM_KERNEL advisory is supported. */
#if LINUX_VERSION_CODE <= 0x02040e /* KERNEL_VERSION(2,4,14) */
		vma->vm_flags |= VM_LOCKED;
#else
		vma->vm_flags |= VM_RESERVED;
#endif
		break;
	case _DRM_SCATTER_GATHER:
		vma->vm_ops = &DRM(vm_sg_ops);
		vma->vm_private_data = (void *)map;
#if LINUX_VERSION_CODE <= 0x02040e /* KERNEL_VERSION(2,4,14) */
		vma->vm_flags |= VM_LOCKED;
#else
		vma->vm_flags |= VM_RESERVED;
#endif
                break;
	default:
		return -EINVAL;	/* This should never happen. */
	}
#if LINUX_VERSION_CODE <= 0x02040e /* KERNEL_VERSION(2,4,14) */
	vma->vm_flags |= VM_LOCKED | VM_SHM; /* Don't swap */
#else
	vma->vm_flags |= VM_RESERVED; /* Don't swap */
#endif

	vma->vm_file  =	 filp;	/* Needed for drm_vm_open() */
	DRM(vm_open)(vma);
	return 0;
}
