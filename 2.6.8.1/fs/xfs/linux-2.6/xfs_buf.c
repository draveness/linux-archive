/*
 * Copyright (c) 2000-2004 Silicon Graphics, Inc.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Further, this software is distributed without any warranty that it is
 * free of the rightful claim of any third person regarding infringement
 * or the like.  Any license provided herein, whether implied or
 * otherwise, applies only to this software file.  Patent licenses, if
 * any, provided herein do not apply to combinations of this program with
 * other software, or any other product whatsoever.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston MA 02111-1307, USA.
 *
 * Contact information: Silicon Graphics, Inc., 1600 Amphitheatre Pkwy,
 * Mountain View, CA  94043, or:
 *
 * http://www.sgi.com
 *
 * For further information regarding this notice, see:
 *
 * http://oss.sgi.com/projects/GenInfo/SGIGPLNoticeExplan/
 */

/*
 *	The xfs_buf.c code provides an abstract buffer cache model on top
 *	of the Linux page cache.  Cached metadata blocks for a file system
 *	are hashed to the inode for the block device.  xfs_buf.c assembles
 *	buffers (xfs_buf_t) on demand to aggregate such cached pages for I/O.
 *
 *      Written by Steve Lord, Jim Mostek, Russell Cattelan
 *		    and Rajagopal Ananthanarayanan ("ananth") at SGI.
 *
 */

#include <linux/stddef.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/bio.h>
#include <linux/sysctl.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/suspend.h>
#include <linux/percpu.h>

#include "xfs_linux.h"

#ifndef GFP_READAHEAD
#define GFP_READAHEAD	(__GFP_NOWARN|__GFP_NORETRY)
#endif

/*
 * File wide globals
 */

STATIC kmem_cache_t *pagebuf_cache;
STATIC kmem_shaker_t pagebuf_shake;
STATIC int pagebuf_daemon_wakeup(int, unsigned int);
STATIC void pagebuf_delwri_queue(xfs_buf_t *, int);
STATIC struct workqueue_struct *pagebuf_logio_workqueue;
STATIC struct workqueue_struct *pagebuf_dataio_workqueue;

/*
 * Pagebuf debugging
 */

#ifdef PAGEBUF_TRACE
void
pagebuf_trace(
	xfs_buf_t	*pb,
	char		*id,
	void		*data,
	void		*ra)
{
	ktrace_enter(pagebuf_trace_buf,
		pb, id,
		(void *)(unsigned long)pb->pb_flags,
		(void *)(unsigned long)pb->pb_hold.counter,
		(void *)(unsigned long)pb->pb_sema.count.counter,
		(void *)current,
		data, ra,
		(void *)(unsigned long)((pb->pb_file_offset>>32) & 0xffffffff),
		(void *)(unsigned long)(pb->pb_file_offset & 0xffffffff),
		(void *)(unsigned long)pb->pb_buffer_length,
		NULL, NULL, NULL, NULL, NULL);
}
ktrace_t *pagebuf_trace_buf;
#define PAGEBUF_TRACE_SIZE	4096
#define PB_TRACE(pb, id, data)	\
	pagebuf_trace(pb, id, (void *)data, (void *)__builtin_return_address(0))
#else
#define PB_TRACE(pb, id, data)	do { } while (0)
#endif

#ifdef PAGEBUF_LOCK_TRACKING
# define PB_SET_OWNER(pb)	((pb)->pb_last_holder = current->pid)
# define PB_CLEAR_OWNER(pb)	((pb)->pb_last_holder = -1)
# define PB_GET_OWNER(pb)	((pb)->pb_last_holder)
#else
# define PB_SET_OWNER(pb)	do { } while (0)
# define PB_CLEAR_OWNER(pb)	do { } while (0)
# define PB_GET_OWNER(pb)	do { } while (0)
#endif

/*
 * Pagebuf allocation / freeing.
 */

#define pb_to_gfp(flags) \
	(((flags) & PBF_READ_AHEAD) ? GFP_READAHEAD : \
	 ((flags) & PBF_DONT_BLOCK) ? GFP_NOFS : GFP_KERNEL)

#define pb_to_km(flags) \
	 (((flags) & PBF_DONT_BLOCK) ? KM_NOFS : KM_SLEEP)


#define pagebuf_allocate(flags) \
	kmem_zone_alloc(pagebuf_cache, pb_to_km(flags))
#define pagebuf_deallocate(pb) \
	kmem_zone_free(pagebuf_cache, (pb));

/*
 * Pagebuf hashing
 */

#define NBITS	8
#define NHASH	(1<<NBITS)

typedef struct {
	struct list_head	pb_hash;
	spinlock_t		pb_hash_lock;
} pb_hash_t;

STATIC pb_hash_t	pbhash[NHASH];
#define pb_hash(pb)	&pbhash[pb->pb_hash_index]

STATIC int
_bhash(
	struct block_device *bdev,
	loff_t		base)
{
	int		bit, hval;

	base >>= 9;
	base ^= (unsigned long)bdev / L1_CACHE_BYTES;
	for (bit = hval = 0; base && bit < sizeof(base) * 8; bit += NBITS) {
		hval ^= (int)base & (NHASH-1);
		base >>= NBITS;
	}
	return hval;
}

/*
 * Mapping of multi-page buffers into contiguous virtual space
 */

typedef struct a_list {
	void		*vm_addr;
	struct a_list	*next;
} a_list_t;

STATIC a_list_t		*as_free_head;
STATIC int		as_list_len;
STATIC spinlock_t	as_lock = SPIN_LOCK_UNLOCKED;

/*
 * Try to batch vunmaps because they are costly.
 */
STATIC void
free_address(
	void		*addr)
{
	a_list_t	*aentry;

	aentry = kmalloc(sizeof(a_list_t), GFP_ATOMIC);
	if (aentry) {
		spin_lock(&as_lock);
		aentry->next = as_free_head;
		aentry->vm_addr = addr;
		as_free_head = aentry;
		as_list_len++;
		spin_unlock(&as_lock);
	} else {
		vunmap(addr);
	}
}

STATIC void
purge_addresses(void)
{
	a_list_t	*aentry, *old;

	if (as_free_head == NULL)
		return;

	spin_lock(&as_lock);
	aentry = as_free_head;
	as_free_head = NULL;
	as_list_len = 0;
	spin_unlock(&as_lock);

	while ((old = aentry) != NULL) {
		vunmap(aentry->vm_addr);
		aentry = aentry->next;
		kfree(old);
	}
}

/*
 *	Internal pagebuf object manipulation
 */

STATIC void
_pagebuf_initialize(
	xfs_buf_t		*pb,
	xfs_buftarg_t		*target,
	loff_t			range_base,
	size_t			range_length,
	page_buf_flags_t	flags)
{
	/*
	 * We don't want certain flags to appear in pb->pb_flags.
	 */
	flags &= ~(PBF_LOCK|PBF_MAPPED|PBF_DONT_BLOCK|PBF_READ_AHEAD);

	memset(pb, 0, sizeof(xfs_buf_t));
	atomic_set(&pb->pb_hold, 1);
	init_MUTEX_LOCKED(&pb->pb_iodonesema);
	INIT_LIST_HEAD(&pb->pb_list);
	INIT_LIST_HEAD(&pb->pb_hash_list);
	init_MUTEX_LOCKED(&pb->pb_sema); /* held, no waiters */
	PB_SET_OWNER(pb);
	pb->pb_target = target;
	pb->pb_file_offset = range_base;
	/*
	 * Set buffer_length and count_desired to the same value initially.
	 * I/O routines should use count_desired, which will be the same in
	 * most cases but may be reset (e.g. XFS recovery).
	 */
	pb->pb_buffer_length = pb->pb_count_desired = range_length;
	pb->pb_flags = flags | PBF_NONE;
	pb->pb_bn = XFS_BUF_DADDR_NULL;
	atomic_set(&pb->pb_pin_count, 0);
	init_waitqueue_head(&pb->pb_waiters);

	XFS_STATS_INC(pb_create);
	PB_TRACE(pb, "initialize", target);
}

/*
 * Allocate a page array capable of holding a specified number
 * of pages, and point the page buf at it.
 */
STATIC int
_pagebuf_get_pages(
	xfs_buf_t		*pb,
	int			page_count,
	page_buf_flags_t	flags)
{
	/* Make sure that we have a page list */
	if (pb->pb_pages == NULL) {
		pb->pb_offset = page_buf_poff(pb->pb_file_offset);
		pb->pb_page_count = page_count;
		if (page_count <= PB_PAGES) {
			pb->pb_pages = pb->pb_page_array;
		} else {
			pb->pb_pages = kmem_alloc(sizeof(struct page *) *
					page_count, pb_to_km(flags));
			if (pb->pb_pages == NULL)
				return -ENOMEM;
		}
		memset(pb->pb_pages, 0, sizeof(struct page *) * page_count);
	}
	return 0;
}

/*
 *	Frees pb_pages if it was malloced.
 */
STATIC void
_pagebuf_free_pages(
	xfs_buf_t	*bp)
{
	if (bp->pb_pages != bp->pb_page_array) {
		kmem_free(bp->pb_pages,
			  bp->pb_page_count * sizeof(struct page *));
	}
}

/*
 *	Releases the specified buffer.
 *
 * 	The modification state of any associated pages is left unchanged.
 * 	The buffer most not be on any hash - use pagebuf_rele instead for
 * 	hashed and refcounted buffers
 */
void
pagebuf_free(
	xfs_buf_t		*bp)
{
	PB_TRACE(bp, "free", 0);

	ASSERT(list_empty(&bp->pb_hash_list));

	if (bp->pb_flags & _PBF_PAGE_CACHE) {
		uint		i;

		if ((bp->pb_flags & PBF_MAPPED) && (bp->pb_page_count > 1))
			free_address(bp->pb_addr - bp->pb_offset);

		for (i = 0; i < bp->pb_page_count; i++)
			page_cache_release(bp->pb_pages[i]);
		_pagebuf_free_pages(bp);
	} else if (bp->pb_flags & _PBF_KMEM_ALLOC) {
		 /*
		  * XXX(hch): bp->pb_count_desired might be incorrect (see
		  * pagebuf_associate_memory for details), but fortunately
		  * the Linux version of kmem_free ignores the len argument..
		  */
		kmem_free(bp->pb_addr, bp->pb_count_desired);
		_pagebuf_free_pages(bp);
	}

	pagebuf_deallocate(bp);
}

/*
 *	Finds all pages for buffer in question and builds it's page list.
 */
STATIC int
_pagebuf_lookup_pages(
	xfs_buf_t		*bp,
	uint			flags)
{
	struct address_space	*mapping = bp->pb_target->pbr_mapping;
	unsigned int		sectorshift = bp->pb_target->pbr_sshift;
	size_t			blocksize = bp->pb_target->pbr_bsize;
	size_t			size = bp->pb_count_desired;
	size_t			nbytes, offset;
	int			gfp_mask = pb_to_gfp(flags);
	unsigned short		page_count, i;
	pgoff_t			first;
	loff_t			end;
	int			error;

	end = bp->pb_file_offset + bp->pb_buffer_length;
	page_count = page_buf_btoc(end) - page_buf_btoct(bp->pb_file_offset);

	error = _pagebuf_get_pages(bp, page_count, flags);
	if (unlikely(error))
		return error;
	bp->pb_flags |= _PBF_PAGE_CACHE;

	offset = bp->pb_offset;
	first = bp->pb_file_offset >> PAGE_CACHE_SHIFT;

	for (i = 0; i < bp->pb_page_count; i++) {
		struct page	*page;
		uint		retries = 0;

	      retry:
		page = find_or_create_page(mapping, first + i, gfp_mask);
		if (unlikely(page == NULL)) {
			if (flags & PBF_READ_AHEAD) {
				bp->pb_page_count = i;
				for (i = 0; i < bp->pb_page_count; i++)
					unlock_page(bp->pb_pages[i]);
				return -ENOMEM;
			}

			/*
			 * This could deadlock.
			 *
			 * But until all the XFS lowlevel code is revamped to
			 * handle buffer allocation failures we can't do much.
			 */
			if (!(++retries % 100))
				printk(KERN_ERR
					"possible deadlock in %s (mode:0x%x)\n",
					__FUNCTION__, gfp_mask);

			XFS_STATS_INC(pb_page_retries);
			pagebuf_daemon_wakeup(0, gfp_mask);
			set_current_state(TASK_UNINTERRUPTIBLE);
			schedule_timeout(10);
			goto retry;
		}

		XFS_STATS_INC(pb_page_found);

		nbytes = min_t(size_t, size, PAGE_CACHE_SIZE - offset);
		size -= nbytes;

		if (!PageUptodate(page)) {
			page_count--;
			if (blocksize == PAGE_CACHE_SIZE) {
				if (flags & PBF_READ)
					bp->pb_locked = 1;
			} else if (!PagePrivate(page)) {
				unsigned long	j, range;

				/*
				 * In this case page->private holds a bitmap
				 * of uptodate sectors within the page
				 */
				ASSERT(blocksize < PAGE_CACHE_SIZE);
				range = (offset + nbytes) >> sectorshift;
				for (j = offset >> sectorshift; j < range; j++)
					if (!test_bit(j, &page->private))
						break;
				if (j == range)
					page_count++;
			}
		}

		bp->pb_pages[i] = page;
		offset = 0;
	}

	if (!bp->pb_locked) {
		for (i = 0; i < bp->pb_page_count; i++)
			unlock_page(bp->pb_pages[i]);
	}

	if (page_count) {
		/* if we have any uptodate pages, mark that in the buffer */
		bp->pb_flags &= ~PBF_NONE;

		/* if some pages aren't uptodate, mark that in the buffer */
		if (page_count != bp->pb_page_count)
			bp->pb_flags |= PBF_PARTIAL;
	}

	PB_TRACE(bp, "lookup_pages", (long)page_count);
	return error;
}

/*
 *	Map buffer into kernel address-space if nessecary.
 */
STATIC int
_pagebuf_map_pages(
	xfs_buf_t		*bp,
	uint			flags)
{
	/* A single page buffer is always mappable */
	if (bp->pb_page_count == 1) {
		bp->pb_addr = page_address(bp->pb_pages[0]) + bp->pb_offset;
		bp->pb_flags |= PBF_MAPPED;
	} else if (flags & PBF_MAPPED) {
		if (as_list_len > 64)
			purge_addresses();
		bp->pb_addr = vmap(bp->pb_pages, bp->pb_page_count,
				VM_MAP, PAGE_KERNEL);
		if (unlikely(bp->pb_addr == NULL))
			return -ENOMEM;
		bp->pb_addr += bp->pb_offset;
		bp->pb_flags |= PBF_MAPPED;
	}

	return 0;
}

/*
 *	Finding and Reading Buffers
 */

/*
 *	_pagebuf_find
 *
 *	Looks up, and creates if absent, a lockable buffer for
 *	a given range of an inode.  The buffer is returned
 *	locked.	 If other overlapping buffers exist, they are
 *	released before the new buffer is created and locked,
 *	which may imply that this call will block until those buffers
 *	are unlocked.  No I/O is implied by this call.
 */
STATIC xfs_buf_t *
_pagebuf_find(				/* find buffer for block	*/
	xfs_buftarg_t		*target,/* target for block		*/
	loff_t			ioff,	/* starting offset of range	*/
	size_t			isize,	/* length of range		*/
	page_buf_flags_t	flags,	/* PBF_TRYLOCK			*/
	xfs_buf_t		*new_pb)/* newly allocated buffer	*/
{
	loff_t			range_base;
	size_t			range_length;
	int			hval;
	pb_hash_t		*h;
	xfs_buf_t		*pb, *n;
	int			not_locked;

	range_base = (ioff << BBSHIFT);
	range_length = (isize << BBSHIFT);

	/* Ensure we never do IOs smaller than the sector size */
	BUG_ON(range_length < (1 << target->pbr_sshift));

	/* Ensure we never do IOs that are not sector aligned */
	BUG_ON(range_base & (loff_t)target->pbr_smask);

	hval = _bhash(target->pbr_bdev, range_base);
	h = &pbhash[hval];

	spin_lock(&h->pb_hash_lock);
	list_for_each_entry_safe(pb, n, &h->pb_hash, pb_hash_list) {
		if (pb->pb_target == target &&
		    pb->pb_file_offset == range_base &&
		    pb->pb_buffer_length == range_length) {
			/* If we look at something bring it to the
			 * front of the list for next time
			 */
			atomic_inc(&pb->pb_hold);
			list_move(&pb->pb_hash_list, &h->pb_hash);
			goto found;
		}
	}

	/* No match found */
	if (new_pb) {
		_pagebuf_initialize(new_pb, target, range_base,
				range_length, flags);
		new_pb->pb_hash_index = hval;
		list_add(&new_pb->pb_hash_list, &h->pb_hash);
	} else {
		XFS_STATS_INC(pb_miss_locked);
	}

	spin_unlock(&h->pb_hash_lock);
	return (new_pb);

found:
	spin_unlock(&h->pb_hash_lock);

	/* Attempt to get the semaphore without sleeping,
	 * if this does not work then we need to drop the
	 * spinlock and do a hard attempt on the semaphore.
	 */
	not_locked = down_trylock(&pb->pb_sema);
	if (not_locked) {
		if (!(flags & PBF_TRYLOCK)) {
			/* wait for buffer ownership */
			PB_TRACE(pb, "get_lock", 0);
			pagebuf_lock(pb);
			XFS_STATS_INC(pb_get_locked_waited);
		} else {
			/* We asked for a trylock and failed, no need
			 * to look at file offset and length here, we
			 * know that this pagebuf at least overlaps our
			 * pagebuf and is locked, therefore our buffer
			 * either does not exist, or is this buffer
			 */

			pagebuf_rele(pb);
			XFS_STATS_INC(pb_busy_locked);
			return (NULL);
		}
	} else {
		/* trylock worked */
		PB_SET_OWNER(pb);
	}

	if (pb->pb_flags & PBF_STALE)
		pb->pb_flags &= PBF_MAPPED;
	PB_TRACE(pb, "got_lock", 0);
	XFS_STATS_INC(pb_get_locked);
	return (pb);
}


/*
 *	pagebuf_find
 *
 *	pagebuf_find returns a buffer matching the specified range of
 *	data for the specified target, if any of the relevant blocks
 *	are in memory.  The buffer may have unallocated holes, if
 *	some, but not all, of the blocks are in memory.  Even where
 *	pages are present in the buffer, not all of every page may be
 *	valid.
 */
xfs_buf_t *
pagebuf_find(				/* find buffer for block	*/
					/* if the block is in memory	*/
	xfs_buftarg_t		*target,/* target for block		*/
	loff_t			ioff,	/* starting offset of range	*/
	size_t			isize,	/* length of range		*/
	page_buf_flags_t	flags)	/* PBF_TRYLOCK			*/
{
	return _pagebuf_find(target, ioff, isize, flags, NULL);
}

/*
 *	pagebuf_get
 *
 *	pagebuf_get assembles a buffer covering the specified range.
 *	Some or all of the blocks in the range may be valid.  Storage
 *	in memory for all portions of the buffer will be allocated,
 *	although backing storage may not be.  If PBF_READ is set in
 *	flags, pagebuf_iostart is called also.
 */
xfs_buf_t *
pagebuf_get(				/* allocate a buffer		*/
	xfs_buftarg_t		*target,/* target for buffer		*/
	loff_t			ioff,	/* starting offset of range	*/
	size_t			isize,	/* length of range		*/
	page_buf_flags_t	flags)	/* PBF_TRYLOCK			*/
{
	xfs_buf_t		*pb, *new_pb;
	int			error = 0, i;

	new_pb = pagebuf_allocate(flags);
	if (unlikely(!new_pb))
		return NULL;

	pb = _pagebuf_find(target, ioff, isize, flags, new_pb);
	if (pb == new_pb) {
		error = _pagebuf_lookup_pages(pb, flags);
		if (unlikely(error)) {
			printk(KERN_WARNING
			       "pagebuf_get: failed to lookup pages\n");
			goto no_buffer;
		}
	} else {
		pagebuf_deallocate(new_pb);
		if (unlikely(pb == NULL))
			return NULL;
	}

	for (i = 0; i < pb->pb_page_count; i++)
		mark_page_accessed(pb->pb_pages[i]);

	if (!(pb->pb_flags & PBF_MAPPED)) {
		error = _pagebuf_map_pages(pb, flags);
		if (unlikely(error)) {
			printk(KERN_WARNING
			       "pagebuf_get: failed to map pages\n");
			goto no_buffer;
		}
	}

	XFS_STATS_INC(pb_get);

	/*
	 * Always fill in the block number now, the mapped cases can do
	 * their own overlay of this later.
	 */
	pb->pb_bn = ioff;
	pb->pb_count_desired = pb->pb_buffer_length;

	if (flags & PBF_READ) {
		if (PBF_NOT_DONE(pb)) {
			PB_TRACE(pb, "get_read", (unsigned long)flags);
			XFS_STATS_INC(pb_get_read);
			pagebuf_iostart(pb, flags);
		} else if (flags & PBF_ASYNC) {
			PB_TRACE(pb, "get_read_async", (unsigned long)flags);
			/*
			 * Read ahead call which is already satisfied,
			 * drop the buffer
			 */
			goto no_buffer;
		} else {
			PB_TRACE(pb, "get_read_done", (unsigned long)flags);
			/* We do not want read in the flags */
			pb->pb_flags &= ~PBF_READ;
		}
	} else {
		PB_TRACE(pb, "get_write", (unsigned long)flags);
	}

	return pb;

no_buffer:
	if (flags & (PBF_LOCK | PBF_TRYLOCK))
		pagebuf_unlock(pb);
	pagebuf_rele(pb);
	return NULL;
}

/*
 * Create a skeletal pagebuf (no pages associated with it).
 */
xfs_buf_t *
pagebuf_lookup(
	xfs_buftarg_t		*target,
	loff_t			ioff,
	size_t			isize,
	page_buf_flags_t	flags)
{
	xfs_buf_t		*pb;

	pb = pagebuf_allocate(flags);
	if (pb) {
		_pagebuf_initialize(pb, target, ioff, isize, flags);
	}
	return pb;
}

/*
 * If we are not low on memory then do the readahead in a deadlock
 * safe manner.
 */
void
pagebuf_readahead(
	xfs_buftarg_t		*target,
	loff_t			ioff,
	size_t			isize,
	page_buf_flags_t	flags)
{
	struct backing_dev_info *bdi;

	bdi = target->pbr_mapping->backing_dev_info;
	if (bdi_read_congested(bdi))
		return;
	if (bdi_write_congested(bdi))
		return;

	flags |= (PBF_TRYLOCK|PBF_READ|PBF_ASYNC|PBF_READ_AHEAD);
	pagebuf_get(target, ioff, isize, flags);
}

xfs_buf_t *
pagebuf_get_empty(
	size_t			len,
	xfs_buftarg_t		*target)
{
	xfs_buf_t		*pb;

	pb = pagebuf_allocate(0);
	if (pb)
		_pagebuf_initialize(pb, target, 0, len, 0);
	return pb;
}

static inline struct page *
mem_to_page(
	void			*addr)
{
	if (((unsigned long)addr < VMALLOC_START) ||
	    ((unsigned long)addr >= VMALLOC_END)) {
		return virt_to_page(addr);
	} else {
		return vmalloc_to_page(addr);
	}
}

int
pagebuf_associate_memory(
	xfs_buf_t		*pb,
	void			*mem,
	size_t			len)
{
	int			rval;
	int			i = 0;
	size_t			ptr;
	size_t			end, end_cur;
	off_t			offset;
	int			page_count;

	page_count = PAGE_CACHE_ALIGN(len) >> PAGE_CACHE_SHIFT;
	offset = (off_t) mem - ((off_t)mem & PAGE_CACHE_MASK);
	if (offset && (len > PAGE_CACHE_SIZE))
		page_count++;

	/* Free any previous set of page pointers */
	if (pb->pb_pages)
		_pagebuf_free_pages(pb);

	pb->pb_pages = NULL;
	pb->pb_addr = mem;

	rval = _pagebuf_get_pages(pb, page_count, 0);
	if (rval)
		return rval;

	pb->pb_offset = offset;
	ptr = (size_t) mem & PAGE_CACHE_MASK;
	end = PAGE_CACHE_ALIGN((size_t) mem + len);
	end_cur = end;
	/* set up first page */
	pb->pb_pages[0] = mem_to_page(mem);

	ptr += PAGE_CACHE_SIZE;
	pb->pb_page_count = ++i;
	while (ptr < end) {
		pb->pb_pages[i] = mem_to_page((void *)ptr);
		pb->pb_page_count = ++i;
		ptr += PAGE_CACHE_SIZE;
	}
	pb->pb_locked = 0;

	pb->pb_count_desired = pb->pb_buffer_length = len;
	pb->pb_flags |= PBF_MAPPED;

	return 0;
}

xfs_buf_t *
pagebuf_get_no_daddr(
	size_t			len,
	xfs_buftarg_t		*target)
{
	size_t			malloc_len = len;
	xfs_buf_t		*bp;
	void			*data;
	int			error;

	bp = pagebuf_allocate(0);
	if (unlikely(bp == NULL))
		goto fail;
	_pagebuf_initialize(bp, target, 0, len, PBF_FORCEIO);

 try_again:
	data = kmem_alloc(malloc_len, KM_SLEEP | KM_MAYFAIL);
	if (unlikely(data == NULL))
		goto fail_free_buf;

	/* check whether alignment matches.. */
	if ((__psunsigned_t)data !=
	    ((__psunsigned_t)data & ~target->pbr_smask)) {
		/* .. else double the size and try again */
		kmem_free(data, malloc_len);
		malloc_len <<= 1;
		goto try_again;
	}

	error = pagebuf_associate_memory(bp, data, len);
	if (error)
		goto fail_free_mem;
	bp->pb_flags |= _PBF_KMEM_ALLOC;

	pagebuf_unlock(bp);

	PB_TRACE(bp, "no_daddr", data);
	return bp;
 fail_free_mem:
	kmem_free(data, malloc_len);
 fail_free_buf:
	pagebuf_free(bp);
 fail:
	return NULL;
}

/*
 *	pagebuf_hold
 *
 *	Increment reference count on buffer, to hold the buffer concurrently
 *	with another thread which may release (free) the buffer asynchronously.
 *
 *	Must hold the buffer already to call this function.
 */
void
pagebuf_hold(
	xfs_buf_t		*pb)
{
	atomic_inc(&pb->pb_hold);
	PB_TRACE(pb, "hold", 0);
}

/*
 *	pagebuf_rele
 *
 *	pagebuf_rele releases a hold on the specified buffer.  If the
 *	the hold count is 1, pagebuf_rele calls pagebuf_free.
 */
void
pagebuf_rele(
	xfs_buf_t		*pb)
{
	pb_hash_t		*hash = pb_hash(pb);

	PB_TRACE(pb, "rele", pb->pb_relse);

	if (atomic_dec_and_lock(&pb->pb_hold, &hash->pb_hash_lock)) {
		int		do_free = 1;

		if (pb->pb_relse) {
			atomic_inc(&pb->pb_hold);
			spin_unlock(&hash->pb_hash_lock);
			(*(pb->pb_relse)) (pb);
			spin_lock(&hash->pb_hash_lock);
			do_free = 0;
		}

		if (pb->pb_flags & PBF_DELWRI) {
			pb->pb_flags |= PBF_ASYNC;
			atomic_inc(&pb->pb_hold);
			pagebuf_delwri_queue(pb, 0);
			do_free = 0;
		} else if (pb->pb_flags & PBF_FS_MANAGED) {
			do_free = 0;
		}

		if (do_free) {
			list_del_init(&pb->pb_hash_list);
			spin_unlock(&hash->pb_hash_lock);
			pagebuf_free(pb);
		} else {
			spin_unlock(&hash->pb_hash_lock);
		}
	}
}


/*
 *	Mutual exclusion on buffers.  Locking model:
 *
 *	Buffers associated with inodes for which buffer locking
 *	is not enabled are not protected by semaphores, and are
 *	assumed to be exclusively owned by the caller.  There is a
 *	spinlock in the buffer, used by the caller when concurrent
 *	access is possible.
 */

/*
 *	pagebuf_cond_lock
 *
 *	pagebuf_cond_lock locks a buffer object, if it is not already locked.
 *	Note that this in no way
 *	locks the underlying pages, so it is only useful for synchronizing
 *	concurrent use of page buffer objects, not for synchronizing independent
 *	access to the underlying pages.
 */
int
pagebuf_cond_lock(			/* lock buffer, if not locked	*/
					/* returns -EBUSY if locked)	*/
	xfs_buf_t		*pb)
{
	int			locked;

	locked = down_trylock(&pb->pb_sema) == 0;
	if (locked) {
		PB_SET_OWNER(pb);
	}
	PB_TRACE(pb, "cond_lock", (long)locked);
	return(locked ? 0 : -EBUSY);
}

/*
 *	pagebuf_lock_value
 *
 *	Return lock value for a pagebuf
 */
int
pagebuf_lock_value(
	xfs_buf_t		*pb)
{
	return(atomic_read(&pb->pb_sema.count));
}

/*
 *	pagebuf_lock
 *
 *	pagebuf_lock locks a buffer object.  Note that this in no way
 *	locks the underlying pages, so it is only useful for synchronizing
 *	concurrent use of page buffer objects, not for synchronizing independent
 *	access to the underlying pages.
 */
int
pagebuf_lock(
	xfs_buf_t		*pb)
{
	PB_TRACE(pb, "lock", 0);
	if (atomic_read(&pb->pb_io_remaining))
		blk_run_address_space(pb->pb_target->pbr_mapping);
	down(&pb->pb_sema);
	PB_SET_OWNER(pb);
	PB_TRACE(pb, "locked", 0);
	return 0;
}

/*
 *	pagebuf_unlock
 *
 *	pagebuf_unlock releases the lock on the buffer object created by
 *	pagebuf_lock or pagebuf_cond_lock (not any
 *	pinning of underlying pages created by pagebuf_pin).
 */
void
pagebuf_unlock(				/* unlock buffer		*/
	xfs_buf_t		*pb)	/* buffer to unlock		*/
{
	PB_CLEAR_OWNER(pb);
	up(&pb->pb_sema);
	PB_TRACE(pb, "unlock", 0);
}


/*
 *	Pinning Buffer Storage in Memory
 */

/*
 *	pagebuf_pin
 *
 *	pagebuf_pin locks all of the memory represented by a buffer in
 *	memory.  Multiple calls to pagebuf_pin and pagebuf_unpin, for
 *	the same or different buffers affecting a given page, will
 *	properly count the number of outstanding "pin" requests.  The
 *	buffer may be released after the pagebuf_pin and a different
 *	buffer used when calling pagebuf_unpin, if desired.
 *	pagebuf_pin should be used by the file system when it wants be
 *	assured that no attempt will be made to force the affected
 *	memory to disk.	 It does not assure that a given logical page
 *	will not be moved to a different physical page.
 */
void
pagebuf_pin(
	xfs_buf_t		*pb)
{
	atomic_inc(&pb->pb_pin_count);
	PB_TRACE(pb, "pin", (long)pb->pb_pin_count.counter);
}

/*
 *	pagebuf_unpin
 *
 *	pagebuf_unpin reverses the locking of memory performed by
 *	pagebuf_pin.  Note that both functions affected the logical
 *	pages associated with the buffer, not the buffer itself.
 */
void
pagebuf_unpin(
	xfs_buf_t		*pb)
{
	if (atomic_dec_and_test(&pb->pb_pin_count)) {
		wake_up_all(&pb->pb_waiters);
	}
	PB_TRACE(pb, "unpin", (long)pb->pb_pin_count.counter);
}

int
pagebuf_ispin(
	xfs_buf_t		*pb)
{
	return atomic_read(&pb->pb_pin_count);
}

/*
 *	pagebuf_wait_unpin
 *
 *	pagebuf_wait_unpin waits until all of the memory associated
 *	with the buffer is not longer locked in memory.  It returns
 *	immediately if none of the affected pages are locked.
 */
static inline void
_pagebuf_wait_unpin(
	xfs_buf_t		*pb)
{
	DECLARE_WAITQUEUE	(wait, current);

	if (atomic_read(&pb->pb_pin_count) == 0)
		return;

	add_wait_queue(&pb->pb_waiters, &wait);
	for (;;) {
		set_current_state(TASK_UNINTERRUPTIBLE);
		if (atomic_read(&pb->pb_pin_count) == 0)
			break;
		if (atomic_read(&pb->pb_io_remaining))
			blk_run_address_space(pb->pb_target->pbr_mapping);
		schedule();
	}
	remove_wait_queue(&pb->pb_waiters, &wait);
	set_current_state(TASK_RUNNING);
}

/*
 *	Buffer Utility Routines
 */

/*
 *	pagebuf_iodone
 *
 *	pagebuf_iodone marks a buffer for which I/O is in progress
 *	done with respect to that I/O.	The pb_iodone routine, if
 *	present, will be called as a side-effect.
 */
void
pagebuf_iodone_work(
	void			*v)
{
	xfs_buf_t		*bp = (xfs_buf_t *)v;

	if (bp->pb_iodone)
		(*(bp->pb_iodone))(bp);
	else if (bp->pb_flags & PBF_ASYNC)
		xfs_buf_relse(bp);
}

void
pagebuf_iodone(
	xfs_buf_t		*pb,
	int			dataio,
	int			schedule)
{
	pb->pb_flags &= ~(PBF_READ | PBF_WRITE);
	if (pb->pb_error == 0) {
		pb->pb_flags &= ~(PBF_PARTIAL | PBF_NONE);
	}

	PB_TRACE(pb, "iodone", pb->pb_iodone);

	if ((pb->pb_iodone) || (pb->pb_flags & PBF_ASYNC)) {
		if (schedule) {
			INIT_WORK(&pb->pb_iodone_work, pagebuf_iodone_work, pb);
			queue_work(dataio ? pagebuf_dataio_workqueue :
				pagebuf_logio_workqueue, &pb->pb_iodone_work);
		} else {
			pagebuf_iodone_work(pb);
		}
	} else {
		up(&pb->pb_iodonesema);
	}
}

/*
 *	pagebuf_ioerror
 *
 *	pagebuf_ioerror sets the error code for a buffer.
 */
void
pagebuf_ioerror(			/* mark/clear buffer error flag */
	xfs_buf_t		*pb,	/* buffer to mark		*/
	int			error)	/* error to store (0 if none)	*/
{
	ASSERT(error >= 0 && error <= 0xffff);
	pb->pb_error = (unsigned short)error;
	PB_TRACE(pb, "ioerror", (unsigned long)error);
}

/*
 *	pagebuf_iostart
 *
 *	pagebuf_iostart initiates I/O on a buffer, based on the flags supplied.
 *	If necessary, it will arrange for any disk space allocation required,
 *	and it will break up the request if the block mappings require it.
 *	The pb_iodone routine in the buffer supplied will only be called
 *	when all of the subsidiary I/O requests, if any, have been completed.
 *	pagebuf_iostart calls the pagebuf_ioinitiate routine or
 *	pagebuf_iorequest, if the former routine is not defined, to start
 *	the I/O on a given low-level request.
 */
int
pagebuf_iostart(			/* start I/O on a buffer	  */
	xfs_buf_t		*pb,	/* buffer to start		  */
	page_buf_flags_t	flags)	/* PBF_LOCK, PBF_ASYNC, PBF_READ, */
					/* PBF_WRITE, PBF_DELWRI,	  */
					/* PBF_DONT_BLOCK		  */
{
	int			status = 0;

	PB_TRACE(pb, "iostart", (unsigned long)flags);

	if (flags & PBF_DELWRI) {
		pb->pb_flags &= ~(PBF_READ | PBF_WRITE | PBF_ASYNC);
		pb->pb_flags |= flags & (PBF_DELWRI | PBF_ASYNC);
		pagebuf_delwri_queue(pb, 1);
		return status;
	}

	pb->pb_flags &= ~(PBF_READ | PBF_WRITE | PBF_ASYNC | PBF_DELWRI | \
			PBF_READ_AHEAD | _PBF_RUN_QUEUES);
	pb->pb_flags |= flags & (PBF_READ | PBF_WRITE | PBF_ASYNC | \
			PBF_READ_AHEAD | _PBF_RUN_QUEUES);

	BUG_ON(pb->pb_bn == XFS_BUF_DADDR_NULL);

	/* For writes allow an alternate strategy routine to precede
	 * the actual I/O request (which may not be issued at all in
	 * a shutdown situation, for example).
	 */
	status = (flags & PBF_WRITE) ?
		pagebuf_iostrategy(pb) : pagebuf_iorequest(pb);

	/* Wait for I/O if we are not an async request.
	 * Note: async I/O request completion will release the buffer,
	 * and that can already be done by this point.  So using the
	 * buffer pointer from here on, after async I/O, is invalid.
	 */
	if (!status && !(flags & PBF_ASYNC))
		status = pagebuf_iowait(pb);

	return status;
}

/*
 * Helper routine for pagebuf_iorequest
 */

STATIC __inline__ int
_pagebuf_iolocked(
	xfs_buf_t		*pb)
{
	ASSERT(pb->pb_flags & (PBF_READ|PBF_WRITE));
	if (pb->pb_flags & PBF_READ)
		return pb->pb_locked;
	return 0;
}

STATIC __inline__ void
_pagebuf_iodone(
	xfs_buf_t		*pb,
	int			schedule)
{
	if (atomic_dec_and_test(&pb->pb_io_remaining) == 1) {
		pb->pb_locked = 0;
		pagebuf_iodone(pb, (pb->pb_flags & PBF_FS_DATAIOD), schedule);
	}
}

STATIC int
bio_end_io_pagebuf(
	struct bio		*bio,
	unsigned int		bytes_done,
	int			error)
{
	xfs_buf_t		*pb = (xfs_buf_t *)bio->bi_private;
	unsigned int		i, blocksize = pb->pb_target->pbr_bsize;
	unsigned int		sectorshift = pb->pb_target->pbr_sshift;
	struct bio_vec		*bvec = bio->bi_io_vec;

	if (bio->bi_size)
		return 1;

	if (!test_bit(BIO_UPTODATE, &bio->bi_flags))
		pb->pb_error = EIO;

	for (i = 0; i < bio->bi_vcnt; i++, bvec++) {
		struct page	*page = bvec->bv_page;

		if (pb->pb_error) {
			SetPageError(page);
		} else if (blocksize == PAGE_CACHE_SIZE) {
			SetPageUptodate(page);
		} else if (!PagePrivate(page) &&
				(pb->pb_flags & _PBF_PAGE_CACHE)) {
			unsigned long	j, range;

			ASSERT(blocksize < PAGE_CACHE_SIZE);
			range = (bvec->bv_offset + bvec->bv_len) >> sectorshift;
			for (j = bvec->bv_offset >> sectorshift; j < range; j++)
				set_bit(j, &page->private);
			if (page->private == (unsigned long)(PAGE_CACHE_SIZE-1))
				SetPageUptodate(page);
		}

		if (_pagebuf_iolocked(pb)) {
			unlock_page(page);
		}
	}

	_pagebuf_iodone(pb, 1);
	bio_put(bio);
	return 0;
}

void
_pagebuf_ioapply(
	xfs_buf_t		*pb)
{
	int			i, map_i, total_nr_pages, nr_pages;
	struct bio		*bio;
	int			offset = pb->pb_offset;
	int			size = pb->pb_count_desired;
	sector_t		sector = pb->pb_bn;
	unsigned int		blocksize = pb->pb_target->pbr_bsize;
	int			locking = _pagebuf_iolocked(pb);

	total_nr_pages = pb->pb_page_count;
	map_i = 0;

	/* Special code path for reading a sub page size pagebuf in --
	 * we populate up the whole page, and hence the other metadata
	 * in the same page.  This optimization is only valid when the
	 * filesystem block size and the page size are equal.
	 */
	if ((pb->pb_buffer_length < PAGE_CACHE_SIZE) &&
	    (pb->pb_flags & PBF_READ) && locking &&
	    (blocksize == PAGE_CACHE_SIZE)) {
		bio = bio_alloc(GFP_NOIO, 1);

		bio->bi_bdev = pb->pb_target->pbr_bdev;
		bio->bi_sector = sector - (offset >> BBSHIFT);
		bio->bi_end_io = bio_end_io_pagebuf;
		bio->bi_private = pb;

		bio_add_page(bio, pb->pb_pages[0], PAGE_CACHE_SIZE, 0);
		size = 0;

		atomic_inc(&pb->pb_io_remaining);

		goto submit_io;
	}

	/* Lock down the pages which we need to for the request */
	if (locking && (pb->pb_flags & PBF_WRITE) && (pb->pb_locked == 0)) {
		for (i = 0; size; i++) {
			int		nbytes = PAGE_CACHE_SIZE - offset;
			struct page	*page = pb->pb_pages[i];

			if (nbytes > size)
				nbytes = size;

			lock_page(page);

			size -= nbytes;
			offset = 0;
		}
		offset = pb->pb_offset;
		size = pb->pb_count_desired;
	}

next_chunk:
	atomic_inc(&pb->pb_io_remaining);
	nr_pages = BIO_MAX_SECTORS >> (PAGE_SHIFT - BBSHIFT);
	if (nr_pages > total_nr_pages)
		nr_pages = total_nr_pages;

	bio = bio_alloc(GFP_NOIO, nr_pages);
	bio->bi_bdev = pb->pb_target->pbr_bdev;
	bio->bi_sector = sector;
	bio->bi_end_io = bio_end_io_pagebuf;
	bio->bi_private = pb;

	for (; size && nr_pages; nr_pages--, map_i++) {
		int	nbytes = PAGE_CACHE_SIZE - offset;

		if (nbytes > size)
			nbytes = size;

		if (bio_add_page(bio, pb->pb_pages[map_i],
					nbytes, offset) < nbytes)
			break;

		offset = 0;
		sector += nbytes >> BBSHIFT;
		size -= nbytes;
		total_nr_pages--;
	}

submit_io:
	if (likely(bio->bi_size)) {
		submit_bio((pb->pb_flags & PBF_READ) ? READ : WRITE, bio);
		if (size)
			goto next_chunk;
	} else {
		bio_put(bio);
		pagebuf_ioerror(pb, EIO);
	}

	if (pb->pb_flags & _PBF_RUN_QUEUES) {
		pb->pb_flags &= ~_PBF_RUN_QUEUES;
		if (atomic_read(&pb->pb_io_remaining) > 1)
			blk_run_address_space(pb->pb_target->pbr_mapping);
	}
}

/*
 *	pagebuf_iorequest -- the core I/O request routine.
 */
int
pagebuf_iorequest(			/* start real I/O		*/
	xfs_buf_t		*pb)	/* buffer to convey to device	*/
{
	PB_TRACE(pb, "iorequest", 0);

	if (pb->pb_flags & PBF_DELWRI) {
		pagebuf_delwri_queue(pb, 1);
		return 0;
	}

	if (pb->pb_flags & PBF_WRITE) {
		_pagebuf_wait_unpin(pb);
	}

	pagebuf_hold(pb);

	/* Set the count to 1 initially, this will stop an I/O
	 * completion callout which happens before we have started
	 * all the I/O from calling pagebuf_iodone too early.
	 */
	atomic_set(&pb->pb_io_remaining, 1);
	_pagebuf_ioapply(pb);
	_pagebuf_iodone(pb, 0);

	pagebuf_rele(pb);
	return 0;
}

/*
 *	pagebuf_iowait
 *
 *	pagebuf_iowait waits for I/O to complete on the buffer supplied.
 *	It returns immediately if no I/O is pending.  In any case, it returns
 *	the error code, if any, or 0 if there is no error.
 */
int
pagebuf_iowait(
	xfs_buf_t		*pb)
{
	PB_TRACE(pb, "iowait", 0);
	if (atomic_read(&pb->pb_io_remaining))
		blk_run_address_space(pb->pb_target->pbr_mapping);
	down(&pb->pb_iodonesema);
	PB_TRACE(pb, "iowaited", (long)pb->pb_error);
	return pb->pb_error;
}

caddr_t
pagebuf_offset(
	xfs_buf_t		*pb,
	size_t			offset)
{
	struct page		*page;

	offset += pb->pb_offset;

	page = pb->pb_pages[offset >> PAGE_CACHE_SHIFT];
	return (caddr_t) page_address(page) + (offset & (PAGE_CACHE_SIZE - 1));
}

/*
 *	pagebuf_iomove
 *
 *	Move data into or out of a buffer.
 */
void
pagebuf_iomove(
	xfs_buf_t		*pb,	/* buffer to process		*/
	size_t			boff,	/* starting buffer offset	*/
	size_t			bsize,	/* length to copy		*/
	caddr_t			data,	/* data address			*/
	page_buf_rw_t		mode)	/* read/write flag		*/
{
	size_t			bend, cpoff, csize;
	struct page		*page;

	bend = boff + bsize;
	while (boff < bend) {
		page = pb->pb_pages[page_buf_btoct(boff + pb->pb_offset)];
		cpoff = page_buf_poff(boff + pb->pb_offset);
		csize = min_t(size_t,
			      PAGE_CACHE_SIZE-cpoff, pb->pb_count_desired-boff);

		ASSERT(((csize + cpoff) <= PAGE_CACHE_SIZE));

		switch (mode) {
		case PBRW_ZERO:
			memset(page_address(page) + cpoff, 0, csize);
			break;
		case PBRW_READ:
			memcpy(data, page_address(page) + cpoff, csize);
			break;
		case PBRW_WRITE:
			memcpy(page_address(page) + cpoff, data, csize);
		}

		boff += csize;
		data += csize;
	}
}

/*
 *	Handling of buftargs.
 */

void
xfs_free_buftarg(
	xfs_buftarg_t		*btp,
	int			external)
{
	xfs_flush_buftarg(btp, 1);
	if (external)
		xfs_blkdev_put(btp->pbr_bdev);
	kmem_free(btp, sizeof(*btp));
}

void
xfs_incore_relse(
	xfs_buftarg_t		*btp,
	int			delwri_only,
	int			wait)
{
	invalidate_bdev(btp->pbr_bdev, 1);
	truncate_inode_pages(btp->pbr_mapping, 0LL);
}

void
xfs_setsize_buftarg(
	xfs_buftarg_t		*btp,
	unsigned int		blocksize,
	unsigned int		sectorsize)
{
	btp->pbr_bsize = blocksize;
	btp->pbr_sshift = ffs(sectorsize) - 1;
	btp->pbr_smask = sectorsize - 1;

	if (set_blocksize(btp->pbr_bdev, sectorsize)) {
		printk(KERN_WARNING
			"XFS: Cannot set_blocksize to %u on device %s\n",
			sectorsize, XFS_BUFTARG_NAME(btp));
	}
}

xfs_buftarg_t *
xfs_alloc_buftarg(
	struct block_device	*bdev)
{
	xfs_buftarg_t		*btp;

	btp = kmem_zalloc(sizeof(*btp), KM_SLEEP);

	btp->pbr_dev =  bdev->bd_dev;
	btp->pbr_bdev = bdev;
	btp->pbr_mapping = bdev->bd_inode->i_mapping;
	xfs_setsize_buftarg(btp, PAGE_CACHE_SIZE, bdev_hardsect_size(bdev));

	return btp;
}


/*
 * Pagebuf delayed write buffer handling
 */

STATIC LIST_HEAD(pbd_delwrite_queue);
STATIC spinlock_t pbd_delwrite_lock = SPIN_LOCK_UNLOCKED;

STATIC void
pagebuf_delwri_queue(
	xfs_buf_t		*pb,
	int			unlock)
{
	PB_TRACE(pb, "delwri_q", (long)unlock);
	ASSERT(pb->pb_flags & PBF_DELWRI);

	spin_lock(&pbd_delwrite_lock);
	/* If already in the queue, dequeue and place at tail */
	if (!list_empty(&pb->pb_list)) {
		if (unlock) {
			atomic_dec(&pb->pb_hold);
		}
		list_del(&pb->pb_list);
	}

	list_add_tail(&pb->pb_list, &pbd_delwrite_queue);
	pb->pb_queuetime = jiffies;
	spin_unlock(&pbd_delwrite_lock);

	if (unlock)
		pagebuf_unlock(pb);
}

void
pagebuf_delwri_dequeue(
	xfs_buf_t		*pb)
{
	int			dequeued = 0;

	spin_lock(&pbd_delwrite_lock);
	if ((pb->pb_flags & PBF_DELWRI) && !list_empty(&pb->pb_list)) {
		list_del_init(&pb->pb_list);
		dequeued = 1;
	}
	pb->pb_flags &= ~PBF_DELWRI;
	spin_unlock(&pbd_delwrite_lock);

	if (dequeued)
		pagebuf_rele(pb);

	PB_TRACE(pb, "delwri_dq", (long)dequeued);
}

STATIC void
pagebuf_runall_queues(
	struct workqueue_struct	*queue)
{
	flush_workqueue(queue);
}

/* Defines for pagebuf daemon */
STATIC DECLARE_COMPLETION(pagebuf_daemon_done);
STATIC struct task_struct *pagebuf_daemon_task;
STATIC int pagebuf_daemon_active;
STATIC int force_flush;


STATIC int
pagebuf_daemon_wakeup(
	int			priority,
	unsigned int		mask)
{
	force_flush = 1;
	barrier();
	wake_up_process(pagebuf_daemon_task);
	return 0;
}

STATIC int
pagebuf_daemon(
	void			*data)
{
	struct list_head	tmp;
	unsigned long		age;
	xfs_buftarg_t		*target;
	xfs_buf_t		*pb, *n;

	/*  Set up the thread  */
	daemonize("xfsbufd");
	current->flags |= PF_MEMALLOC;

	pagebuf_daemon_task = current;
	pagebuf_daemon_active = 1;
	barrier();

	INIT_LIST_HEAD(&tmp);
	do {
		/* swsusp */
		if (current->flags & PF_FREEZE)
			refrigerator(PF_FREEZE);

		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout((xfs_buf_timer_centisecs * HZ) / 100);

		age = (xfs_buf_age_centisecs * HZ) / 100;
		spin_lock(&pbd_delwrite_lock);
		list_for_each_entry_safe(pb, n, &pbd_delwrite_queue, pb_list) {
			PB_TRACE(pb, "walkq1", (long)pagebuf_ispin(pb));
			ASSERT(pb->pb_flags & PBF_DELWRI);

			if (!pagebuf_ispin(pb) && !pagebuf_cond_lock(pb)) {
				if (!force_flush &&
				    time_before(jiffies,
						pb->pb_queuetime + age)) {
					pagebuf_unlock(pb);
					break;
				}

				pb->pb_flags &= ~PBF_DELWRI;
				pb->pb_flags |= PBF_WRITE;
				list_move(&pb->pb_list, &tmp);
			}
		}
		spin_unlock(&pbd_delwrite_lock);

		while (!list_empty(&tmp)) {
			pb = list_entry(tmp.next, xfs_buf_t, pb_list);
			target = pb->pb_target;

			list_del_init(&pb->pb_list);
			pagebuf_iostrategy(pb);

			blk_run_address_space(target->pbr_mapping);
		}

		if (as_list_len > 0)
			purge_addresses();

		force_flush = 0;
	} while (pagebuf_daemon_active);

	complete_and_exit(&pagebuf_daemon_done, 0);
}

/*
 * Go through all incore buffers, and release buffers if they belong to
 * the given device. This is used in filesystem error handling to
 * preserve the consistency of its metadata.
 */
int
xfs_flush_buftarg(
	xfs_buftarg_t		*target,
	int			wait)
{
	struct list_head	tmp;
	xfs_buf_t		*pb, *n;
	int			pincount = 0;

	pagebuf_runall_queues(pagebuf_dataio_workqueue);
	pagebuf_runall_queues(pagebuf_logio_workqueue);

	INIT_LIST_HEAD(&tmp);
	spin_lock(&pbd_delwrite_lock);
	list_for_each_entry_safe(pb, n, &pbd_delwrite_queue, pb_list) {

		if (pb->pb_target != target)
			continue;

		ASSERT(pb->pb_flags & PBF_DELWRI);
		PB_TRACE(pb, "walkq2", (long)pagebuf_ispin(pb));
		if (pagebuf_ispin(pb)) {
			pincount++;
			continue;
		}

		pb->pb_flags &= ~PBF_DELWRI;
		pb->pb_flags |= PBF_WRITE;
		list_move(&pb->pb_list, &tmp);
	}
	spin_unlock(&pbd_delwrite_lock);

	/*
	 * Dropped the delayed write list lock, now walk the temporary list
	 */
	list_for_each_entry_safe(pb, n, &tmp, pb_list) {
		if (wait)
			pb->pb_flags &= ~PBF_ASYNC;
		else
			list_del_init(&pb->pb_list);

		pagebuf_lock(pb);
		pagebuf_iostrategy(pb);
	}

	/*
	 * Remaining list items must be flushed before returning
	 */
	while (!list_empty(&tmp)) {
		pb = list_entry(tmp.next, xfs_buf_t, pb_list);

		list_del_init(&pb->pb_list);
		xfs_iowait(pb);
		xfs_buf_relse(pb);
	}

	if (wait)
		blk_run_address_space(target->pbr_mapping);

	return pincount;
}

STATIC int
pagebuf_daemon_start(void)
{
	int		rval;

	pagebuf_logio_workqueue = create_workqueue("xfslogd");
	if (!pagebuf_logio_workqueue)
		return -ENOMEM;

	pagebuf_dataio_workqueue = create_workqueue("xfsdatad");
	if (!pagebuf_dataio_workqueue) {
		destroy_workqueue(pagebuf_logio_workqueue);
		return -ENOMEM;
	}

	rval = kernel_thread(pagebuf_daemon, NULL, CLONE_FS|CLONE_FILES);
	if (rval < 0) {
		destroy_workqueue(pagebuf_logio_workqueue);
		destroy_workqueue(pagebuf_dataio_workqueue);
	}

	return rval;
}

/*
 * pagebuf_daemon_stop
 *
 * Note: do not mark as __exit, it is called from pagebuf_terminate.
 */
STATIC void
pagebuf_daemon_stop(void)
{
	pagebuf_daemon_active = 0;
	barrier();
	wait_for_completion(&pagebuf_daemon_done);

	destroy_workqueue(pagebuf_logio_workqueue);
	destroy_workqueue(pagebuf_dataio_workqueue);
}

/*
 *	Initialization and Termination
 */

int __init
pagebuf_init(void)
{
	int			i;

	pagebuf_cache = kmem_cache_create("xfs_buf_t", sizeof(xfs_buf_t), 0,
			SLAB_HWCACHE_ALIGN, NULL, NULL);
	if (pagebuf_cache == NULL) {
		printk("XFS: couldn't init xfs_buf_t cache\n");
		pagebuf_terminate();
		return -ENOMEM;
	}

#ifdef PAGEBUF_TRACE
	pagebuf_trace_buf = ktrace_alloc(PAGEBUF_TRACE_SIZE, KM_SLEEP);
#endif

	pagebuf_daemon_start();

	pagebuf_shake = kmem_shake_register(pagebuf_daemon_wakeup);
	if (pagebuf_shake == NULL) {
		pagebuf_terminate();
		return -ENOMEM;
	}

	for (i = 0; i < NHASH; i++) {
		spin_lock_init(&pbhash[i].pb_hash_lock);
		INIT_LIST_HEAD(&pbhash[i].pb_hash);
	}

	return 0;
}


/*
 *	pagebuf_terminate.
 *
 *	Note: do not mark as __exit, this is also called from the __init code.
 */
void
pagebuf_terminate(void)
{
	pagebuf_daemon_stop();

#ifdef PAGEBUF_TRACE
	ktrace_free(pagebuf_trace_buf);
#endif

	kmem_zone_destroy(pagebuf_cache);
	kmem_shake_deregister(pagebuf_shake);
}
