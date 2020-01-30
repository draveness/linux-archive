/*
 * AGPGART backend specific includes. Not for userspace consumption.
 *
 * Copyright (C) 2002-2003 Dave Jones
 * Copyright (C) 1999 Jeff Hartmann
 * Copyright (C) 1999 Precision Insight, Inc.
 * Copyright (C) 1999 Xi Graphics, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * JEFF HARTMANN, OR ANY OTHER CONTRIBUTORS BE LIABLE FOR ANY CLAIM, 
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR 
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE 
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef _AGP_BACKEND_H
#define _AGP_BACKEND_H 1

#ifdef __KERNEL__

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

enum chipset_type {
	NOT_SUPPORTED,
	SUPPORTED,
};

struct agp_version {
	u16 major;
	u16 minor;
};

struct agp_kern_info {
	struct agp_version version;
	struct pci_dev *device;
	enum chipset_type chipset;
	unsigned long mode;
	off_t aper_base;
	size_t aper_size;
	int max_memory;		/* In pages */
	int current_memory;
	int cant_use_aperture;
	unsigned long page_mask;
	struct vm_operations_struct *vm_ops;
};

/* 
 * The agp_memory structure has information about the block of agp memory
 * allocated.  A caller may manipulate the next and prev pointers to link
 * each allocated item into a list.  These pointers are ignored by the backend.
 * Everything else should never be written to, but the caller may read any of
 * the items to detrimine the status of this block of agp memory. 
 */

struct agp_memory {
	int key;
	struct agp_memory *next;
	struct agp_memory *prev;
	size_t page_count;
	int num_scratch_pages;
	unsigned long *memory;
	off_t pg_start;
	u32 type;
	u32 physical;
	u8 is_bound;
	u8 is_flushed;
};

#define AGP_NORMAL_MEMORY 0

extern void agp_free_memory(struct agp_memory *);
extern struct agp_memory *agp_allocate_memory(size_t, u32);
extern int agp_copy_info(struct agp_kern_info *);
extern int agp_bind_memory(struct agp_memory *, off_t);
extern int agp_unbind_memory(struct agp_memory *);
extern void agp_enable(u32);
extern int agp_backend_acquire(void);
extern void agp_backend_release(void);

/*
 * Interface between drm and agp code.  When agp initializes, it makes
 * the below structure available via inter_module_register(), drm might
 * use it.  Keith Owens <kaos@ocs.com.au> 28 Oct 2000.
 */
typedef struct {
	void			(*free_memory)(struct agp_memory *);
	struct agp_memory *	(*allocate_memory)(size_t, u32);
	int			(*bind_memory)(struct agp_memory *, off_t);
	int			(*unbind_memory)(struct agp_memory *);
	void			(*enable)(u32);
	int			(*acquire)(void);
	void			(*release)(void);
	int			(*copy_info)(struct agp_kern_info *);
} drm_agp_t;

extern const drm_agp_t *drm_agp_p;

#endif				/* __KERNEL__ */
#endif				/* _AGP_BACKEND_H */
