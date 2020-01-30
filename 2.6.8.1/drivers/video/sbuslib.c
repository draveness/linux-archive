/* sbuslib.c: Helper library for SBUS framebuffer drivers.
 *
 * Copyright (C) 2003 David S. Miller (davem@redhat.com)
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/fb.h>
#include <linux/mm.h>

#include <asm/oplib.h>
#include <asm/fbio.h>

#include "sbuslib.h"

void sbusfb_fill_var(struct fb_var_screeninfo *var, int prom_node, int bpp)
{
	memset(var, 0, sizeof(*var));

	var->xres = prom_getintdefault(prom_node, "width", 1152);
	var->yres = prom_getintdefault(prom_node, "height", 900);
	var->xres_virtual = var->xres;
	var->yres_virtual = var->yres;
	var->bits_per_pixel = bpp;
}

EXPORT_SYMBOL(sbusfb_fill_var);

static unsigned long sbusfb_mmapsize(long size, unsigned long fbsize)
{
	if (size == SBUS_MMAP_EMPTY) return 0;
	if (size >= 0) return size;
	return fbsize * (-size);
}

int sbusfb_mmap_helper(struct sbus_mmap_map *map,
		       unsigned long physbase,
		       unsigned long fbsize,
		       unsigned long iospace,
		       struct vm_area_struct *vma)
{
	unsigned int size, page, r, map_size;
	unsigned long map_offset = 0;
	unsigned long off;
	int i;
                                        
	size = vma->vm_end - vma->vm_start;
	if (vma->vm_pgoff > (~0UL >> PAGE_SHIFT))
		return -EINVAL;

	off = vma->vm_pgoff << PAGE_SHIFT;

	/* To stop the swapper from even considering these pages */
	vma->vm_flags |= (VM_SHM | VM_IO | VM_LOCKED);
	
	/* Each page, see which map applies */
	for (page = 0; page < size; ){
		map_size = 0;
		for (i = 0; map[i].size; i++)
			if (map[i].voff == off+page) {
				map_size = sbusfb_mmapsize(map[i].size, fbsize);
#ifdef __sparc_v9__
#define POFF_MASK	(PAGE_MASK|0x1UL)
#else
#define POFF_MASK	(PAGE_MASK)
#endif				
				map_offset = (physbase + map[i].poff) & POFF_MASK;
				break;
			}
		if (!map_size){
			page += PAGE_SIZE;
			continue;
		}
		if (page + map_size > size)
			map_size = size - page;
		r = io_remap_page_range(vma,
					vma->vm_start + page,
					map_offset, map_size,
					vma->vm_page_prot, iospace);
		if (r)
			return -EAGAIN;
		page += map_size;
	}

	return 0;
}
EXPORT_SYMBOL(sbusfb_mmap_helper);

int sbusfb_ioctl_helper(unsigned long cmd, unsigned long arg,
			struct fb_info *info,
			int type, int fb_depth, unsigned long fb_size)
{
	switch(cmd) {
	case FBIOGTYPE: {
		struct fbtype __user *f = (struct fbtype __user *) arg;

		if (put_user(type, &f->fb_type) ||
		    __put_user(info->var.yres, &f->fb_height) ||
		    __put_user(info->var.xres, &f->fb_width) ||
		    __put_user(fb_depth, &f->fb_depth) ||
		    __put_user(0, &f->fb_cmsize) ||
		    __put_user(fb_size, &f->fb_cmsize))
			return -EFAULT;
		return 0;
	}
	case FBIOPUTCMAP_SPARC: {
		struct fbcmap __user *c = (struct fbcmap __user *) arg;
		struct fb_cmap cmap;
		u16 red, green, blue;
		unsigned char __user *ured;
		unsigned char __user *ugreen;
		unsigned char __user *ublue;
		int index, count, i;

		if (get_user(index, &c->index) ||
		    __get_user(count, &c->count) ||
		    __get_user(ured, &c->red) ||
		    __get_user(ugreen, &c->green) ||
		    __get_user(ublue, &c->blue))
			return -EFAULT;

		cmap.len = 1;
		cmap.red = &red;
		cmap.green = &green;
		cmap.blue = &blue;
		cmap.transp = NULL;
		for (i = 0; i < count; i++) {
			int err;

			if (get_user(red, &ured[i]) ||
			    get_user(green, &ugreen[i]) ||
			    get_user(blue, &ublue[i]))
				return -EFAULT;

			cmap.start = index + i;
			err = fb_set_cmap(&cmap, info);
			if (err)
				return err;
		}
		return 0;
	}
	case FBIOGETCMAP_SPARC: {
		struct fbcmap __user *c = (struct fbcmap __user *) arg;
		unsigned char __user *ured;
		unsigned char __user *ugreen;
		unsigned char __user *ublue;
		struct fb_cmap *cmap = &info->cmap;
		int index, count, i;

		if (get_user(index, &c->index) ||
		    __get_user(count, &c->count) ||
		    __get_user(ured, &c->red) ||
		    __get_user(ugreen, &c->green) ||
		    __get_user(ublue, &c->blue))
			return -EFAULT;

		if (index + count > cmap->len)
			return -EINVAL;

		for (i = 0; i < count; i++) {
			if (put_user(cmap->red[index + i], &ured[i]) ||
			    put_user(cmap->green[index + i], &ugreen[i]) ||
			    put_user(cmap->blue[index + i], &ublue[i]))
				return -EFAULT;
		}
		return 0;
	}
	default:
		return -EINVAL;
	};
}
EXPORT_SYMBOL(sbusfb_ioctl_helper);
