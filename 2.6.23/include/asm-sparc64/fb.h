#ifndef _ASM_FB_H_
#define _ASM_FB_H_
#include <linux/fb.h>
#include <linux/fs.h>
#include <asm/page.h>
#include <asm/prom.h>

static inline void fb_pgprotect(struct file *file, struct vm_area_struct *vma,
				unsigned long off)
{
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
}

static inline int fb_is_primary_device(struct fb_info *info)
{
	struct device *dev = info->device;
	struct device_node *node;

	node = dev->archdata.prom_node;
	if (node &&
	    node == of_console_device)
		return 1;

	return 0;
}

#endif /* _ASM_FB_H_ */
