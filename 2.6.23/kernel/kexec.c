/*
 * kexec.c - kexec system call
 * Copyright (C) 2002-2004 Eric Biederman  <ebiederm@xmission.com>
 *
 * This source code is licensed under the GNU General Public License,
 * Version 2.  See the file COPYING for more details.
 */

#include <linux/capability.h>
#include <linux/mm.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/kexec.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/highmem.h>
#include <linux/syscalls.h>
#include <linux/reboot.h>
#include <linux/syscalls.h>
#include <linux/ioport.h>
#include <linux/hardirq.h>
#include <linux/elf.h>
#include <linux/elfcore.h>

#include <asm/page.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/system.h>
#include <asm/semaphore.h>

/* Per cpu memory for storing cpu states in case of system crash. */
note_buf_t* crash_notes;

/* Location of the reserved area for the crash kernel */
struct resource crashk_res = {
	.name  = "Crash kernel",
	.start = 0,
	.end   = 0,
	.flags = IORESOURCE_BUSY | IORESOURCE_MEM
};

int kexec_should_crash(struct task_struct *p)
{
	if (in_interrupt() || !p->pid || is_init(p) || panic_on_oops)
		return 1;
	return 0;
}

/*
 * When kexec transitions to the new kernel there is a one-to-one
 * mapping between physical and virtual addresses.  On processors
 * where you can disable the MMU this is trivial, and easy.  For
 * others it is still a simple predictable page table to setup.
 *
 * In that environment kexec copies the new kernel to its final
 * resting place.  This means I can only support memory whose
 * physical address can fit in an unsigned long.  In particular
 * addresses where (pfn << PAGE_SHIFT) > ULONG_MAX cannot be handled.
 * If the assembly stub has more restrictive requirements
 * KEXEC_SOURCE_MEMORY_LIMIT and KEXEC_DEST_MEMORY_LIMIT can be
 * defined more restrictively in <asm/kexec.h>.
 *
 * The code for the transition from the current kernel to the
 * the new kernel is placed in the control_code_buffer, whose size
 * is given by KEXEC_CONTROL_CODE_SIZE.  In the best case only a single
 * page of memory is necessary, but some architectures require more.
 * Because this memory must be identity mapped in the transition from
 * virtual to physical addresses it must live in the range
 * 0 - TASK_SIZE, as only the user space mappings are arbitrarily
 * modifiable.
 *
 * The assembly stub in the control code buffer is passed a linked list
 * of descriptor pages detailing the source pages of the new kernel,
 * and the destination addresses of those source pages.  As this data
 * structure is not used in the context of the current OS, it must
 * be self-contained.
 *
 * The code has been made to work with highmem pages and will use a
 * destination page in its final resting place (if it happens
 * to allocate it).  The end product of this is that most of the
 * physical address space, and most of RAM can be used.
 *
 * Future directions include:
 *  - allocating a page table with the control code buffer identity
 *    mapped, to simplify machine_kexec and make kexec_on_panic more
 *    reliable.
 */

/*
 * KIMAGE_NO_DEST is an impossible destination address..., for
 * allocating pages whose destination address we do not care about.
 */
#define KIMAGE_NO_DEST (-1UL)

static int kimage_is_destination_range(struct kimage *image,
				       unsigned long start, unsigned long end);
static struct page *kimage_alloc_page(struct kimage *image,
				       gfp_t gfp_mask,
				       unsigned long dest);

static int do_kimage_alloc(struct kimage **rimage, unsigned long entry,
	                    unsigned long nr_segments,
                            struct kexec_segment __user *segments)
{
	size_t segment_bytes;
	struct kimage *image;
	unsigned long i;
	int result;

	/* Allocate a controlling structure */
	result = -ENOMEM;
	image = kzalloc(sizeof(*image), GFP_KERNEL);
	if (!image)
		goto out;

	image->head = 0;
	image->entry = &image->head;
	image->last_entry = &image->head;
	image->control_page = ~0; /* By default this does not apply */
	image->start = entry;
	image->type = KEXEC_TYPE_DEFAULT;

	/* Initialize the list of control pages */
	INIT_LIST_HEAD(&image->control_pages);

	/* Initialize the list of destination pages */
	INIT_LIST_HEAD(&image->dest_pages);

	/* Initialize the list of unuseable pages */
	INIT_LIST_HEAD(&image->unuseable_pages);

	/* Read in the segments */
	image->nr_segments = nr_segments;
	segment_bytes = nr_segments * sizeof(*segments);
	result = copy_from_user(image->segment, segments, segment_bytes);
	if (result)
		goto out;

	/*
	 * Verify we have good destination addresses.  The caller is
	 * responsible for making certain we don't attempt to load
	 * the new image into invalid or reserved areas of RAM.  This
	 * just verifies it is an address we can use.
	 *
	 * Since the kernel does everything in page size chunks ensure
	 * the destination addreses are page aligned.  Too many
	 * special cases crop of when we don't do this.  The most
	 * insidious is getting overlapping destination addresses
	 * simply because addresses are changed to page size
	 * granularity.
	 */
	result = -EADDRNOTAVAIL;
	for (i = 0; i < nr_segments; i++) {
		unsigned long mstart, mend;

		mstart = image->segment[i].mem;
		mend   = mstart + image->segment[i].memsz;
		if ((mstart & ~PAGE_MASK) || (mend & ~PAGE_MASK))
			goto out;
		if (mend >= KEXEC_DESTINATION_MEMORY_LIMIT)
			goto out;
	}

	/* Verify our destination addresses do not overlap.
	 * If we alloed overlapping destination addresses
	 * through very weird things can happen with no
	 * easy explanation as one segment stops on another.
	 */
	result = -EINVAL;
	for (i = 0; i < nr_segments; i++) {
		unsigned long mstart, mend;
		unsigned long j;

		mstart = image->segment[i].mem;
		mend   = mstart + image->segment[i].memsz;
		for (j = 0; j < i; j++) {
			unsigned long pstart, pend;
			pstart = image->segment[j].mem;
			pend   = pstart + image->segment[j].memsz;
			/* Do the segments overlap ? */
			if ((mend > pstart) && (mstart < pend))
				goto out;
		}
	}

	/* Ensure our buffer sizes are strictly less than
	 * our memory sizes.  This should always be the case,
	 * and it is easier to check up front than to be surprised
	 * later on.
	 */
	result = -EINVAL;
	for (i = 0; i < nr_segments; i++) {
		if (image->segment[i].bufsz > image->segment[i].memsz)
			goto out;
	}

	result = 0;
out:
	if (result == 0)
		*rimage = image;
	else
		kfree(image);

	return result;

}

static int kimage_normal_alloc(struct kimage **rimage, unsigned long entry,
				unsigned long nr_segments,
				struct kexec_segment __user *segments)
{
	int result;
	struct kimage *image;

	/* Allocate and initialize a controlling structure */
	image = NULL;
	result = do_kimage_alloc(&image, entry, nr_segments, segments);
	if (result)
		goto out;

	*rimage = image;

	/*
	 * Find a location for the control code buffer, and add it
	 * the vector of segments so that it's pages will also be
	 * counted as destination pages.
	 */
	result = -ENOMEM;
	image->control_code_page = kimage_alloc_control_pages(image,
					   get_order(KEXEC_CONTROL_CODE_SIZE));
	if (!image->control_code_page) {
		printk(KERN_ERR "Could not allocate control_code_buffer\n");
		goto out;
	}

	result = 0;
 out:
	if (result == 0)
		*rimage = image;
	else
		kfree(image);

	return result;
}

static int kimage_crash_alloc(struct kimage **rimage, unsigned long entry,
				unsigned long nr_segments,
				struct kexec_segment __user *segments)
{
	int result;
	struct kimage *image;
	unsigned long i;

	image = NULL;
	/* Verify we have a valid entry point */
	if ((entry < crashk_res.start) || (entry > crashk_res.end)) {
		result = -EADDRNOTAVAIL;
		goto out;
	}

	/* Allocate and initialize a controlling structure */
	result = do_kimage_alloc(&image, entry, nr_segments, segments);
	if (result)
		goto out;

	/* Enable the special crash kernel control page
	 * allocation policy.
	 */
	image->control_page = crashk_res.start;
	image->type = KEXEC_TYPE_CRASH;

	/*
	 * Verify we have good destination addresses.  Normally
	 * the caller is responsible for making certain we don't
	 * attempt to load the new image into invalid or reserved
	 * areas of RAM.  But crash kernels are preloaded into a
	 * reserved area of ram.  We must ensure the addresses
	 * are in the reserved area otherwise preloading the
	 * kernel could corrupt things.
	 */
	result = -EADDRNOTAVAIL;
	for (i = 0; i < nr_segments; i++) {
		unsigned long mstart, mend;

		mstart = image->segment[i].mem;
		mend = mstart + image->segment[i].memsz - 1;
		/* Ensure we are within the crash kernel limits */
		if ((mstart < crashk_res.start) || (mend > crashk_res.end))
			goto out;
	}

	/*
	 * Find a location for the control code buffer, and add
	 * the vector of segments so that it's pages will also be
	 * counted as destination pages.
	 */
	result = -ENOMEM;
	image->control_code_page = kimage_alloc_control_pages(image,
					   get_order(KEXEC_CONTROL_CODE_SIZE));
	if (!image->control_code_page) {
		printk(KERN_ERR "Could not allocate control_code_buffer\n");
		goto out;
	}

	result = 0;
out:
	if (result == 0)
		*rimage = image;
	else
		kfree(image);

	return result;
}

static int kimage_is_destination_range(struct kimage *image,
					unsigned long start,
					unsigned long end)
{
	unsigned long i;

	for (i = 0; i < image->nr_segments; i++) {
		unsigned long mstart, mend;

		mstart = image->segment[i].mem;
		mend = mstart + image->segment[i].memsz;
		if ((end > mstart) && (start < mend))
			return 1;
	}

	return 0;
}

static struct page *kimage_alloc_pages(gfp_t gfp_mask, unsigned int order)
{
	struct page *pages;

	pages = alloc_pages(gfp_mask, order);
	if (pages) {
		unsigned int count, i;
		pages->mapping = NULL;
		set_page_private(pages, order);
		count = 1 << order;
		for (i = 0; i < count; i++)
			SetPageReserved(pages + i);
	}

	return pages;
}

static void kimage_free_pages(struct page *page)
{
	unsigned int order, count, i;

	order = page_private(page);
	count = 1 << order;
	for (i = 0; i < count; i++)
		ClearPageReserved(page + i);
	__free_pages(page, order);
}

static void kimage_free_page_list(struct list_head *list)
{
	struct list_head *pos, *next;

	list_for_each_safe(pos, next, list) {
		struct page *page;

		page = list_entry(pos, struct page, lru);
		list_del(&page->lru);
		kimage_free_pages(page);
	}
}

static struct page *kimage_alloc_normal_control_pages(struct kimage *image,
							unsigned int order)
{
	/* Control pages are special, they are the intermediaries
	 * that are needed while we copy the rest of the pages
	 * to their final resting place.  As such they must
	 * not conflict with either the destination addresses
	 * or memory the kernel is already using.
	 *
	 * The only case where we really need more than one of
	 * these are for architectures where we cannot disable
	 * the MMU and must instead generate an identity mapped
	 * page table for all of the memory.
	 *
	 * At worst this runs in O(N) of the image size.
	 */
	struct list_head extra_pages;
	struct page *pages;
	unsigned int count;

	count = 1 << order;
	INIT_LIST_HEAD(&extra_pages);

	/* Loop while I can allocate a page and the page allocated
	 * is a destination page.
	 */
	do {
		unsigned long pfn, epfn, addr, eaddr;

		pages = kimage_alloc_pages(GFP_KERNEL, order);
		if (!pages)
			break;
		pfn   = page_to_pfn(pages);
		epfn  = pfn + count;
		addr  = pfn << PAGE_SHIFT;
		eaddr = epfn << PAGE_SHIFT;
		if ((epfn >= (KEXEC_CONTROL_MEMORY_LIMIT >> PAGE_SHIFT)) ||
			      kimage_is_destination_range(image, addr, eaddr)) {
			list_add(&pages->lru, &extra_pages);
			pages = NULL;
		}
	} while (!pages);

	if (pages) {
		/* Remember the allocated page... */
		list_add(&pages->lru, &image->control_pages);

		/* Because the page is already in it's destination
		 * location we will never allocate another page at
		 * that address.  Therefore kimage_alloc_pages
		 * will not return it (again) and we don't need
		 * to give it an entry in image->segment[].
		 */
	}
	/* Deal with the destination pages I have inadvertently allocated.
	 *
	 * Ideally I would convert multi-page allocations into single
	 * page allocations, and add everyting to image->dest_pages.
	 *
	 * For now it is simpler to just free the pages.
	 */
	kimage_free_page_list(&extra_pages);

	return pages;
}

static struct page *kimage_alloc_crash_control_pages(struct kimage *image,
						      unsigned int order)
{
	/* Control pages are special, they are the intermediaries
	 * that are needed while we copy the rest of the pages
	 * to their final resting place.  As such they must
	 * not conflict with either the destination addresses
	 * or memory the kernel is already using.
	 *
	 * Control pages are also the only pags we must allocate
	 * when loading a crash kernel.  All of the other pages
	 * are specified by the segments and we just memcpy
	 * into them directly.
	 *
	 * The only case where we really need more than one of
	 * these are for architectures where we cannot disable
	 * the MMU and must instead generate an identity mapped
	 * page table for all of the memory.
	 *
	 * Given the low demand this implements a very simple
	 * allocator that finds the first hole of the appropriate
	 * size in the reserved memory region, and allocates all
	 * of the memory up to and including the hole.
	 */
	unsigned long hole_start, hole_end, size;
	struct page *pages;

	pages = NULL;
	size = (1 << order) << PAGE_SHIFT;
	hole_start = (image->control_page + (size - 1)) & ~(size - 1);
	hole_end   = hole_start + size - 1;
	while (hole_end <= crashk_res.end) {
		unsigned long i;

		if (hole_end > KEXEC_CONTROL_MEMORY_LIMIT)
			break;
		if (hole_end > crashk_res.end)
			break;
		/* See if I overlap any of the segments */
		for (i = 0; i < image->nr_segments; i++) {
			unsigned long mstart, mend;

			mstart = image->segment[i].mem;
			mend   = mstart + image->segment[i].memsz - 1;
			if ((hole_end >= mstart) && (hole_start <= mend)) {
				/* Advance the hole to the end of the segment */
				hole_start = (mend + (size - 1)) & ~(size - 1);
				hole_end   = hole_start + size - 1;
				break;
			}
		}
		/* If I don't overlap any segments I have found my hole! */
		if (i == image->nr_segments) {
			pages = pfn_to_page(hole_start >> PAGE_SHIFT);
			break;
		}
	}
	if (pages)
		image->control_page = hole_end;

	return pages;
}


struct page *kimage_alloc_control_pages(struct kimage *image,
					 unsigned int order)
{
	struct page *pages = NULL;

	switch (image->type) {
	case KEXEC_TYPE_DEFAULT:
		pages = kimage_alloc_normal_control_pages(image, order);
		break;
	case KEXEC_TYPE_CRASH:
		pages = kimage_alloc_crash_control_pages(image, order);
		break;
	}

	return pages;
}

static int kimage_add_entry(struct kimage *image, kimage_entry_t entry)
{
	if (*image->entry != 0)
		image->entry++;

	if (image->entry == image->last_entry) {
		kimage_entry_t *ind_page;
		struct page *page;

		page = kimage_alloc_page(image, GFP_KERNEL, KIMAGE_NO_DEST);
		if (!page)
			return -ENOMEM;

		ind_page = page_address(page);
		*image->entry = virt_to_phys(ind_page) | IND_INDIRECTION;
		image->entry = ind_page;
		image->last_entry = ind_page +
				      ((PAGE_SIZE/sizeof(kimage_entry_t)) - 1);
	}
	*image->entry = entry;
	image->entry++;
	*image->entry = 0;

	return 0;
}

static int kimage_set_destination(struct kimage *image,
				   unsigned long destination)
{
	int result;

	destination &= PAGE_MASK;
	result = kimage_add_entry(image, destination | IND_DESTINATION);
	if (result == 0)
		image->destination = destination;

	return result;
}


static int kimage_add_page(struct kimage *image, unsigned long page)
{
	int result;

	page &= PAGE_MASK;
	result = kimage_add_entry(image, page | IND_SOURCE);
	if (result == 0)
		image->destination += PAGE_SIZE;

	return result;
}


static void kimage_free_extra_pages(struct kimage *image)
{
	/* Walk through and free any extra destination pages I may have */
	kimage_free_page_list(&image->dest_pages);

	/* Walk through and free any unuseable pages I have cached */
	kimage_free_page_list(&image->unuseable_pages);

}
static int kimage_terminate(struct kimage *image)
{
	if (*image->entry != 0)
		image->entry++;

	*image->entry = IND_DONE;

	return 0;
}

#define for_each_kimage_entry(image, ptr, entry) \
	for (ptr = &image->head; (entry = *ptr) && !(entry & IND_DONE); \
		ptr = (entry & IND_INDIRECTION)? \
			phys_to_virt((entry & PAGE_MASK)): ptr +1)

static void kimage_free_entry(kimage_entry_t entry)
{
	struct page *page;

	page = pfn_to_page(entry >> PAGE_SHIFT);
	kimage_free_pages(page);
}

static void kimage_free(struct kimage *image)
{
	kimage_entry_t *ptr, entry;
	kimage_entry_t ind = 0;

	if (!image)
		return;

	kimage_free_extra_pages(image);
	for_each_kimage_entry(image, ptr, entry) {
		if (entry & IND_INDIRECTION) {
			/* Free the previous indirection page */
			if (ind & IND_INDIRECTION)
				kimage_free_entry(ind);
			/* Save this indirection page until we are
			 * done with it.
			 */
			ind = entry;
		}
		else if (entry & IND_SOURCE)
			kimage_free_entry(entry);
	}
	/* Free the final indirection page */
	if (ind & IND_INDIRECTION)
		kimage_free_entry(ind);

	/* Handle any machine specific cleanup */
	machine_kexec_cleanup(image);

	/* Free the kexec control pages... */
	kimage_free_page_list(&image->control_pages);
	kfree(image);
}

static kimage_entry_t *kimage_dst_used(struct kimage *image,
					unsigned long page)
{
	kimage_entry_t *ptr, entry;
	unsigned long destination = 0;

	for_each_kimage_entry(image, ptr, entry) {
		if (entry & IND_DESTINATION)
			destination = entry & PAGE_MASK;
		else if (entry & IND_SOURCE) {
			if (page == destination)
				return ptr;
			destination += PAGE_SIZE;
		}
	}

	return NULL;
}

static struct page *kimage_alloc_page(struct kimage *image,
					gfp_t gfp_mask,
					unsigned long destination)
{
	/*
	 * Here we implement safeguards to ensure that a source page
	 * is not copied to its destination page before the data on
	 * the destination page is no longer useful.
	 *
	 * To do this we maintain the invariant that a source page is
	 * either its own destination page, or it is not a
	 * destination page at all.
	 *
	 * That is slightly stronger than required, but the proof
	 * that no problems will not occur is trivial, and the
	 * implementation is simply to verify.
	 *
	 * When allocating all pages normally this algorithm will run
	 * in O(N) time, but in the worst case it will run in O(N^2)
	 * time.   If the runtime is a problem the data structures can
	 * be fixed.
	 */
	struct page *page;
	unsigned long addr;

	/*
	 * Walk through the list of destination pages, and see if I
	 * have a match.
	 */
	list_for_each_entry(page, &image->dest_pages, lru) {
		addr = page_to_pfn(page) << PAGE_SHIFT;
		if (addr == destination) {
			list_del(&page->lru);
			return page;
		}
	}
	page = NULL;
	while (1) {
		kimage_entry_t *old;

		/* Allocate a page, if we run out of memory give up */
		page = kimage_alloc_pages(gfp_mask, 0);
		if (!page)
			return NULL;
		/* If the page cannot be used file it away */
		if (page_to_pfn(page) >
				(KEXEC_SOURCE_MEMORY_LIMIT >> PAGE_SHIFT)) {
			list_add(&page->lru, &image->unuseable_pages);
			continue;
		}
		addr = page_to_pfn(page) << PAGE_SHIFT;

		/* If it is the destination page we want use it */
		if (addr == destination)
			break;

		/* If the page is not a destination page use it */
		if (!kimage_is_destination_range(image, addr,
						  addr + PAGE_SIZE))
			break;

		/*
		 * I know that the page is someones destination page.
		 * See if there is already a source page for this
		 * destination page.  And if so swap the source pages.
		 */
		old = kimage_dst_used(image, addr);
		if (old) {
			/* If so move it */
			unsigned long old_addr;
			struct page *old_page;

			old_addr = *old & PAGE_MASK;
			old_page = pfn_to_page(old_addr >> PAGE_SHIFT);
			copy_highpage(page, old_page);
			*old = addr | (*old & ~PAGE_MASK);

			/* The old page I have found cannot be a
			 * destination page, so return it.
			 */
			addr = old_addr;
			page = old_page;
			break;
		}
		else {
			/* Place the page on the destination list I
			 * will use it later.
			 */
			list_add(&page->lru, &image->dest_pages);
		}
	}

	return page;
}

static int kimage_load_normal_segment(struct kimage *image,
					 struct kexec_segment *segment)
{
	unsigned long maddr;
	unsigned long ubytes, mbytes;
	int result;
	unsigned char __user *buf;

	result = 0;
	buf = segment->buf;
	ubytes = segment->bufsz;
	mbytes = segment->memsz;
	maddr = segment->mem;

	result = kimage_set_destination(image, maddr);
	if (result < 0)
		goto out;

	while (mbytes) {
		struct page *page;
		char *ptr;
		size_t uchunk, mchunk;

		page = kimage_alloc_page(image, GFP_HIGHUSER, maddr);
		if (page == 0) {
			result  = -ENOMEM;
			goto out;
		}
		result = kimage_add_page(image, page_to_pfn(page)
								<< PAGE_SHIFT);
		if (result < 0)
			goto out;

		ptr = kmap(page);
		/* Start with a clear page */
		memset(ptr, 0, PAGE_SIZE);
		ptr += maddr & ~PAGE_MASK;
		mchunk = PAGE_SIZE - (maddr & ~PAGE_MASK);
		if (mchunk > mbytes)
			mchunk = mbytes;

		uchunk = mchunk;
		if (uchunk > ubytes)
			uchunk = ubytes;

		result = copy_from_user(ptr, buf, uchunk);
		kunmap(page);
		if (result) {
			result = (result < 0) ? result : -EIO;
			goto out;
		}
		ubytes -= uchunk;
		maddr  += mchunk;
		buf    += mchunk;
		mbytes -= mchunk;
	}
out:
	return result;
}

static int kimage_load_crash_segment(struct kimage *image,
					struct kexec_segment *segment)
{
	/* For crash dumps kernels we simply copy the data from
	 * user space to it's destination.
	 * We do things a page at a time for the sake of kmap.
	 */
	unsigned long maddr;
	unsigned long ubytes, mbytes;
	int result;
	unsigned char __user *buf;

	result = 0;
	buf = segment->buf;
	ubytes = segment->bufsz;
	mbytes = segment->memsz;
	maddr = segment->mem;
	while (mbytes) {
		struct page *page;
		char *ptr;
		size_t uchunk, mchunk;

		page = pfn_to_page(maddr >> PAGE_SHIFT);
		if (page == 0) {
			result  = -ENOMEM;
			goto out;
		}
		ptr = kmap(page);
		ptr += maddr & ~PAGE_MASK;
		mchunk = PAGE_SIZE - (maddr & ~PAGE_MASK);
		if (mchunk > mbytes)
			mchunk = mbytes;

		uchunk = mchunk;
		if (uchunk > ubytes) {
			uchunk = ubytes;
			/* Zero the trailing part of the page */
			memset(ptr + uchunk, 0, mchunk - uchunk);
		}
		result = copy_from_user(ptr, buf, uchunk);
		kexec_flush_icache_page(page);
		kunmap(page);
		if (result) {
			result = (result < 0) ? result : -EIO;
			goto out;
		}
		ubytes -= uchunk;
		maddr  += mchunk;
		buf    += mchunk;
		mbytes -= mchunk;
	}
out:
	return result;
}

static int kimage_load_segment(struct kimage *image,
				struct kexec_segment *segment)
{
	int result = -ENOMEM;

	switch (image->type) {
	case KEXEC_TYPE_DEFAULT:
		result = kimage_load_normal_segment(image, segment);
		break;
	case KEXEC_TYPE_CRASH:
		result = kimage_load_crash_segment(image, segment);
		break;
	}

	return result;
}

/*
 * Exec Kernel system call: for obvious reasons only root may call it.
 *
 * This call breaks up into three pieces.
 * - A generic part which loads the new kernel from the current
 *   address space, and very carefully places the data in the
 *   allocated pages.
 *
 * - A generic part that interacts with the kernel and tells all of
 *   the devices to shut down.  Preventing on-going dmas, and placing
 *   the devices in a consistent state so a later kernel can
 *   reinitialize them.
 *
 * - A machine specific part that includes the syscall number
 *   and the copies the image to it's final destination.  And
 *   jumps into the image at entry.
 *
 * kexec does not sync, or unmount filesystems so if you need
 * that to happen you need to do that yourself.
 */
struct kimage *kexec_image;
struct kimage *kexec_crash_image;
/*
 * A home grown binary mutex.
 * Nothing can wait so this mutex is safe to use
 * in interrupt context :)
 */
static int kexec_lock;

asmlinkage long sys_kexec_load(unsigned long entry, unsigned long nr_segments,
				struct kexec_segment __user *segments,
				unsigned long flags)
{
	struct kimage **dest_image, *image;
	int locked;
	int result;

	/* We only trust the superuser with rebooting the system. */
	if (!capable(CAP_SYS_BOOT))
		return -EPERM;

	/*
	 * Verify we have a legal set of flags
	 * This leaves us room for future extensions.
	 */
	if ((flags & KEXEC_FLAGS) != (flags & ~KEXEC_ARCH_MASK))
		return -EINVAL;

	/* Verify we are on the appropriate architecture */
	if (((flags & KEXEC_ARCH_MASK) != KEXEC_ARCH) &&
		((flags & KEXEC_ARCH_MASK) != KEXEC_ARCH_DEFAULT))
		return -EINVAL;

	/* Put an artificial cap on the number
	 * of segments passed to kexec_load.
	 */
	if (nr_segments > KEXEC_SEGMENT_MAX)
		return -EINVAL;

	image = NULL;
	result = 0;

	/* Because we write directly to the reserved memory
	 * region when loading crash kernels we need a mutex here to
	 * prevent multiple crash  kernels from attempting to load
	 * simultaneously, and to prevent a crash kernel from loading
	 * over the top of a in use crash kernel.
	 *
	 * KISS: always take the mutex.
	 */
	locked = xchg(&kexec_lock, 1);
	if (locked)
		return -EBUSY;

	dest_image = &kexec_image;
	if (flags & KEXEC_ON_CRASH)
		dest_image = &kexec_crash_image;
	if (nr_segments > 0) {
		unsigned long i;

		/* Loading another kernel to reboot into */
		if ((flags & KEXEC_ON_CRASH) == 0)
			result = kimage_normal_alloc(&image, entry,
							nr_segments, segments);
		/* Loading another kernel to switch to if this one crashes */
		else if (flags & KEXEC_ON_CRASH) {
			/* Free any current crash dump kernel before
			 * we corrupt it.
			 */
			kimage_free(xchg(&kexec_crash_image, NULL));
			result = kimage_crash_alloc(&image, entry,
						     nr_segments, segments);
		}
		if (result)
			goto out;

		result = machine_kexec_prepare(image);
		if (result)
			goto out;

		for (i = 0; i < nr_segments; i++) {
			result = kimage_load_segment(image, &image->segment[i]);
			if (result)
				goto out;
		}
		result = kimage_terminate(image);
		if (result)
			goto out;
	}
	/* Install the new kernel, and  Uninstall the old */
	image = xchg(dest_image, image);

out:
	locked = xchg(&kexec_lock, 0); /* Release the mutex */
	BUG_ON(!locked);
	kimage_free(image);

	return result;
}

#ifdef CONFIG_COMPAT
asmlinkage long compat_sys_kexec_load(unsigned long entry,
				unsigned long nr_segments,
				struct compat_kexec_segment __user *segments,
				unsigned long flags)
{
	struct compat_kexec_segment in;
	struct kexec_segment out, __user *ksegments;
	unsigned long i, result;

	/* Don't allow clients that don't understand the native
	 * architecture to do anything.
	 */
	if ((flags & KEXEC_ARCH_MASK) == KEXEC_ARCH_DEFAULT)
		return -EINVAL;

	if (nr_segments > KEXEC_SEGMENT_MAX)
		return -EINVAL;

	ksegments = compat_alloc_user_space(nr_segments * sizeof(out));
	for (i=0; i < nr_segments; i++) {
		result = copy_from_user(&in, &segments[i], sizeof(in));
		if (result)
			return -EFAULT;

		out.buf   = compat_ptr(in.buf);
		out.bufsz = in.bufsz;
		out.mem   = in.mem;
		out.memsz = in.memsz;

		result = copy_to_user(&ksegments[i], &out, sizeof(out));
		if (result)
			return -EFAULT;
	}

	return sys_kexec_load(entry, nr_segments, ksegments, flags);
}
#endif

void crash_kexec(struct pt_regs *regs)
{
	int locked;


	/* Take the kexec_lock here to prevent sys_kexec_load
	 * running on one cpu from replacing the crash kernel
	 * we are using after a panic on a different cpu.
	 *
	 * If the crash kernel was not located in a fixed area
	 * of memory the xchg(&kexec_crash_image) would be
	 * sufficient.  But since I reuse the memory...
	 */
	locked = xchg(&kexec_lock, 1);
	if (!locked) {
		if (kexec_crash_image) {
			struct pt_regs fixed_regs;
			crash_setup_regs(&fixed_regs, regs);
			machine_crash_shutdown(&fixed_regs);
			machine_kexec(kexec_crash_image);
		}
		locked = xchg(&kexec_lock, 0);
		BUG_ON(!locked);
	}
}

static u32 *append_elf_note(u32 *buf, char *name, unsigned type, void *data,
			    size_t data_len)
{
	struct elf_note note;

	note.n_namesz = strlen(name) + 1;
	note.n_descsz = data_len;
	note.n_type   = type;
	memcpy(buf, &note, sizeof(note));
	buf += (sizeof(note) + 3)/4;
	memcpy(buf, name, note.n_namesz);
	buf += (note.n_namesz + 3)/4;
	memcpy(buf, data, note.n_descsz);
	buf += (note.n_descsz + 3)/4;

	return buf;
}

static void final_note(u32 *buf)
{
	struct elf_note note;

	note.n_namesz = 0;
	note.n_descsz = 0;
	note.n_type   = 0;
	memcpy(buf, &note, sizeof(note));
}

void crash_save_cpu(struct pt_regs *regs, int cpu)
{
	struct elf_prstatus prstatus;
	u32 *buf;

	if ((cpu < 0) || (cpu >= NR_CPUS))
		return;

	/* Using ELF notes here is opportunistic.
	 * I need a well defined structure format
	 * for the data I pass, and I need tags
	 * on the data to indicate what information I have
	 * squirrelled away.  ELF notes happen to provide
	 * all of that, so there is no need to invent something new.
	 */
	buf = (u32*)per_cpu_ptr(crash_notes, cpu);
	if (!buf)
		return;
	memset(&prstatus, 0, sizeof(prstatus));
	prstatus.pr_pid = current->pid;
	elf_core_copy_regs(&prstatus.pr_reg, regs);
	buf = append_elf_note(buf, KEXEC_CORE_NOTE_NAME, NT_PRSTATUS,
		      	      &prstatus, sizeof(prstatus));
	final_note(buf);
}

static int __init crash_notes_memory_init(void)
{
	/* Allocate memory for saving cpu registers. */
	crash_notes = alloc_percpu(note_buf_t);
	if (!crash_notes) {
		printk("Kexec: Memory allocation for saving cpu register"
		" states failed\n");
		return -ENOMEM;
	}
	return 0;
}
module_init(crash_notes_memory_init)
