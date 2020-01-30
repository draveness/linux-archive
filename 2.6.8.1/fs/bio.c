/*
 * Copyright (C) 2001 Jens Axboe <axboe@suse.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public Licens
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-
 *
 */
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mempool.h>
#include <linux/workqueue.h>

#define BIO_POOL_SIZE 256

static mempool_t *bio_pool;
static kmem_cache_t *bio_slab;

#define BIOVEC_NR_POOLS 6

/*
 * a small number of entries is fine, not going to be performance critical.
 * basically we just need to survive
 */
#define BIO_SPLIT_ENTRIES 8	
mempool_t *bio_split_pool;

struct biovec_pool {
	int nr_vecs;
	char *name; 
	kmem_cache_t *slab;
	mempool_t *pool;
};

/*
 * if you change this list, also change bvec_alloc or things will
 * break badly! cannot be bigger than what you can fit into an
 * unsigned short
 */

#define BV(x) { .nr_vecs = x, .name = "biovec-"__stringify(x) }
static struct biovec_pool bvec_array[BIOVEC_NR_POOLS] = {
	BV(1), BV(4), BV(16), BV(64), BV(128), BV(BIO_MAX_PAGES),
};
#undef BV

static inline struct bio_vec *bvec_alloc(int gfp_mask, int nr, unsigned long *idx)
{
	struct biovec_pool *bp;
	struct bio_vec *bvl;

	/*
	 * see comment near bvec_array define!
	 */
	switch (nr) {
		case   1        : *idx = 0; break;
		case   2 ...   4: *idx = 1; break;
		case   5 ...  16: *idx = 2; break;
		case  17 ...  64: *idx = 3; break;
		case  65 ... 128: *idx = 4; break;
		case 129 ... BIO_MAX_PAGES: *idx = 5; break;
		default:
			return NULL;
	}
	/*
	 * idx now points to the pool we want to allocate from
	 */
	bp = bvec_array + *idx;

	bvl = mempool_alloc(bp->pool, gfp_mask);
	if (bvl)
		memset(bvl, 0, bp->nr_vecs * sizeof(struct bio_vec));
	return bvl;
}

/*
 * default destructor for a bio allocated with bio_alloc()
 */
void bio_destructor(struct bio *bio)
{
	const int pool_idx = BIO_POOL_IDX(bio);
	struct biovec_pool *bp = bvec_array + pool_idx;

	BIO_BUG_ON(pool_idx >= BIOVEC_NR_POOLS);

	/*
	 * cloned bio doesn't own the veclist
	 */
	if (!bio_flagged(bio, BIO_CLONED))
		mempool_free(bio->bi_io_vec, bp->pool);

	mempool_free(bio, bio_pool);
}

inline void bio_init(struct bio *bio)
{
	bio->bi_next = NULL;
	bio->bi_flags = 1 << BIO_UPTODATE;
	bio->bi_rw = 0;
	bio->bi_vcnt = 0;
	bio->bi_idx = 0;
	bio->bi_phys_segments = 0;
	bio->bi_hw_segments = 0;
	bio->bi_hw_front_size = 0;
	bio->bi_hw_back_size = 0;
	bio->bi_size = 0;
	bio->bi_max_vecs = 0;
	bio->bi_end_io = NULL;
	atomic_set(&bio->bi_cnt, 1);
	bio->bi_private = NULL;
}

/**
 * bio_alloc - allocate a bio for I/O
 * @gfp_mask:   the GFP_ mask given to the slab allocator
 * @nr_iovecs:	number of iovecs to pre-allocate
 *
 * Description:
 *   bio_alloc will first try it's on mempool to satisfy the allocation.
 *   If %__GFP_WAIT is set then we will block on the internal pool waiting
 *   for a &struct bio to become free.
 **/
struct bio *bio_alloc(int gfp_mask, int nr_iovecs)
{
	struct bio_vec *bvl = NULL;
	unsigned long idx;
	struct bio *bio;

	bio = mempool_alloc(bio_pool, gfp_mask);
	if (unlikely(!bio))
		goto out;

	bio_init(bio);

	if (unlikely(!nr_iovecs))
		goto noiovec;

	bvl = bvec_alloc(gfp_mask, nr_iovecs, &idx);
	if (bvl) {
		bio->bi_flags |= idx << BIO_POOL_OFFSET;
		bio->bi_max_vecs = bvec_array[idx].nr_vecs;
noiovec:
		bio->bi_io_vec = bvl;
		bio->bi_destructor = bio_destructor;
out:
		return bio;
	}

	mempool_free(bio, bio_pool);
	bio = NULL;
	goto out;
}

/**
 * bio_put - release a reference to a bio
 * @bio:   bio to release reference to
 *
 * Description:
 *   Put a reference to a &struct bio, either one you have gotten with
 *   bio_alloc or bio_get. The last put of a bio will free it.
 **/
void bio_put(struct bio *bio)
{
	BIO_BUG_ON(!atomic_read(&bio->bi_cnt));

	/*
	 * last put frees it
	 */
	if (atomic_dec_and_test(&bio->bi_cnt)) {
		bio->bi_next = NULL;
		bio->bi_destructor(bio);
	}
}

inline int bio_phys_segments(request_queue_t *q, struct bio *bio)
{
	if (unlikely(!bio_flagged(bio, BIO_SEG_VALID)))
		blk_recount_segments(q, bio);

	return bio->bi_phys_segments;
}

inline int bio_hw_segments(request_queue_t *q, struct bio *bio)
{
	if (unlikely(!bio_flagged(bio, BIO_SEG_VALID)))
		blk_recount_segments(q, bio);

	return bio->bi_hw_segments;
}

/**
 * 	__bio_clone	-	clone a bio
 * 	@bio: destination bio
 * 	@bio_src: bio to clone
 *
 *	Clone a &bio. Caller will own the returned bio, but not
 *	the actual data it points to. Reference count of returned
 * 	bio will be one.
 */
inline void __bio_clone(struct bio *bio, struct bio *bio_src)
{
	bio->bi_io_vec = bio_src->bi_io_vec;

	bio->bi_sector = bio_src->bi_sector;
	bio->bi_bdev = bio_src->bi_bdev;
	bio->bi_flags |= 1 << BIO_CLONED;
	bio->bi_rw = bio_src->bi_rw;

	/*
	 * notes -- maybe just leave bi_idx alone. assume identical mapping
	 * for the clone
	 */
	bio->bi_vcnt = bio_src->bi_vcnt;
	bio->bi_idx = bio_src->bi_idx;
	if (bio_flagged(bio, BIO_SEG_VALID)) {
		bio->bi_phys_segments = bio_src->bi_phys_segments;
		bio->bi_hw_segments = bio_src->bi_hw_segments;
		bio->bi_flags |= (1 << BIO_SEG_VALID);
	}
	bio->bi_size = bio_src->bi_size;

	/*
	 * cloned bio does not own the bio_vec, so users cannot fiddle with
	 * it. clear bi_max_vecs and clear the BIO_POOL_BITS to make this
	 * apparent
	 */
	bio->bi_max_vecs = 0;
	bio->bi_flags &= (BIO_POOL_MASK - 1);
}

/**
 *	bio_clone	-	clone a bio
 *	@bio: bio to clone
 *	@gfp_mask: allocation priority
 *
 * 	Like __bio_clone, only also allocates the returned bio
 */
struct bio *bio_clone(struct bio *bio, int gfp_mask)
{
	struct bio *b = bio_alloc(gfp_mask, 0);

	if (b)
		__bio_clone(b, bio);

	return b;
}

/**
 *	bio_get_nr_vecs		- return approx number of vecs
 *	@bdev:  I/O target
 *
 *	Return the approximate number of pages we can send to this target.
 *	There's no guarantee that you will be able to fit this number of pages
 *	into a bio, it does not account for dynamic restrictions that vary
 *	on offset.
 */
int bio_get_nr_vecs(struct block_device *bdev)
{
	request_queue_t *q = bdev_get_queue(bdev);
	int nr_pages;

	nr_pages = ((q->max_sectors << 9) + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (nr_pages > q->max_phys_segments)
		nr_pages = q->max_phys_segments;
	if (nr_pages > q->max_hw_segments)
		nr_pages = q->max_hw_segments;

	return nr_pages;
}

static int __bio_add_page(request_queue_t *q, struct bio *bio, struct page
			  *page, unsigned int len, unsigned int offset)
{
	int retried_segments = 0;
	struct bio_vec *bvec;

	/*
	 * cloned bio must not modify vec list
	 */
	if (unlikely(bio_flagged(bio, BIO_CLONED)))
		return 0;

	if (bio->bi_vcnt >= bio->bi_max_vecs)
		return 0;

	if (((bio->bi_size + len) >> 9) > q->max_sectors)
		return 0;

	/*
	 * we might lose a segment or two here, but rather that than
	 * make this too complex.
	 */

	while (bio->bi_phys_segments >= q->max_phys_segments
	       || bio->bi_hw_segments >= q->max_hw_segments
	       || BIOVEC_VIRT_OVERSIZE(bio->bi_size)) {

		if (retried_segments)
			return 0;

		retried_segments = 1;
		blk_recount_segments(q, bio);
	}

	/*
	 * setup the new entry, we might clear it again later if we
	 * cannot add the page
	 */
	bvec = &bio->bi_io_vec[bio->bi_vcnt];
	bvec->bv_page = page;
	bvec->bv_len = len;
	bvec->bv_offset = offset;

	/*
	 * if queue has other restrictions (eg varying max sector size
	 * depending on offset), it can specify a merge_bvec_fn in the
	 * queue to get further control
	 */
	if (q->merge_bvec_fn) {
		/*
		 * merge_bvec_fn() returns number of bytes it can accept
		 * at this offset
		 */
		if (q->merge_bvec_fn(q, bio, bvec) < len) {
			bvec->bv_page = NULL;
			bvec->bv_len = 0;
			bvec->bv_offset = 0;
			return 0;
		}
	}

	/* If we may be able to merge these biovecs, force a recount */
	if (bio->bi_vcnt && (BIOVEC_PHYS_MERGEABLE(bvec-1, bvec) ||
	    BIOVEC_VIRT_MERGEABLE(bvec-1, bvec)))
		bio->bi_flags &= ~(1 << BIO_SEG_VALID);

	bio->bi_vcnt++;
	bio->bi_phys_segments++;
	bio->bi_hw_segments++;
	bio->bi_size += len;
	return len;
}

/**
 *	bio_add_page	-	attempt to add page to bio
 *	@bio: destination bio
 *	@page: page to add
 *	@len: vec entry length
 *	@offset: vec entry offset
 *
 *	Attempt to add a page to the bio_vec maplist. This can fail for a
 *	number of reasons, such as the bio being full or target block
 *	device limitations. The target block device must allow bio's
 *      smaller than PAGE_SIZE, so it is always possible to add a single
 *      page to an empty bio.
 */
int bio_add_page(struct bio *bio, struct page *page, unsigned int len,
		 unsigned int offset)
{
	return __bio_add_page(bdev_get_queue(bio->bi_bdev), bio, page,
			      len, offset);
}

/**
 *	bio_uncopy_user	-	finish previously mapped bio
 *	@bio: bio being terminated
 *
 *	Free pages allocated from bio_copy_user() and write back data
 *	to user space in case of a read.
 */
int bio_uncopy_user(struct bio *bio)
{
	struct bio_vec *bvec;
	int i, ret = 0;

	if (bio_data_dir(bio) == READ) {
		char *uaddr = bio->bi_private;

		__bio_for_each_segment(bvec, bio, i, 0) {
			char *addr = page_address(bvec->bv_page);

			if (!ret && copy_to_user(uaddr, addr, bvec->bv_len))
				ret = -EFAULT;

			__free_page(bvec->bv_page);
			uaddr += bvec->bv_len;
		}
	}

	bio_put(bio);
	return ret;
}

/**
 *	bio_copy_user	-	copy user data to bio
 *	@q: destination block queue
 *	@uaddr: start of user address
 *	@len: length in bytes
 *	@write_to_vm: bool indicating writing to pages or not
 *
 *	Prepares and returns a bio for indirect user io, bouncing data
 *	to/from kernel pages as necessary. Must be paired with
 *	call bio_uncopy_user() on io completion.
 */
struct bio *bio_copy_user(request_queue_t *q, unsigned long uaddr,
			  unsigned int len, int write_to_vm)
{
	unsigned long end = (uaddr + len + PAGE_SIZE - 1) >> PAGE_SHIFT;
	unsigned long start = uaddr >> PAGE_SHIFT;
	struct bio_vec *bvec;
	struct page *page;
	struct bio *bio;
	int i, ret;

	bio = bio_alloc(GFP_KERNEL, end - start);
	if (!bio)
		return ERR_PTR(-ENOMEM);

	ret = 0;
	while (len) {
		unsigned int bytes = PAGE_SIZE;

		if (bytes > len)
			bytes = len;

		page = alloc_page(q->bounce_gfp | GFP_KERNEL);
		if (!page) {
			ret = -ENOMEM;
			break;
		}

		if (__bio_add_page(q, bio, page, bytes, 0) < bytes) {
			ret = -EINVAL;
			break;
		}

		len -= bytes;
	}

	/*
	 * success
	 */
	if (!ret) {
		if (!write_to_vm) {
			bio->bi_rw |= (1 << BIO_RW);
			/*
	 		 * for a write, copy in data to kernel pages
			 */
			ret = -EFAULT;
			bio_for_each_segment(bvec, bio, i) {
				char *addr = page_address(bvec->bv_page);

				if (copy_from_user(addr, (char *) uaddr, bvec->bv_len))
					goto cleanup;
			}
		}

		bio->bi_private = (void *) uaddr;
		return bio;
	}

	/*
	 * cleanup
	 */
cleanup:
	bio_for_each_segment(bvec, bio, i)
		__free_page(bvec->bv_page);

	bio_put(bio);
	return ERR_PTR(ret);
}

static struct bio *__bio_map_user(request_queue_t *q, struct block_device *bdev,
				  unsigned long uaddr, unsigned int len,
				  int write_to_vm)
{
	unsigned long end = (uaddr + len + PAGE_SIZE - 1) >> PAGE_SHIFT;
	unsigned long start = uaddr >> PAGE_SHIFT;
	const int nr_pages = end - start;
	int ret, offset, i;
	struct page **pages;
	struct bio *bio;

	/*
	 * transfer and buffer must be aligned to at least hardsector
	 * size for now, in the future we can relax this restriction
	 */
	if ((uaddr & queue_dma_alignment(q)) || (len & queue_dma_alignment(q)))
		return ERR_PTR(-EINVAL);

	bio = bio_alloc(GFP_KERNEL, nr_pages);
	if (!bio)
		return ERR_PTR(-ENOMEM);

	ret = -ENOMEM;
	pages = kmalloc(nr_pages * sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		goto out;

	down_read(&current->mm->mmap_sem);
	ret = get_user_pages(current, current->mm, uaddr, nr_pages,
						write_to_vm, 0, pages, NULL);
	up_read(&current->mm->mmap_sem);

	if (ret < nr_pages)
		goto out;

	bio->bi_bdev = bdev;

	offset = uaddr & ~PAGE_MASK;
	for (i = 0; i < nr_pages; i++) {
		unsigned int bytes = PAGE_SIZE - offset;

		if (len <= 0)
			break;

		if (bytes > len)
			bytes = len;

		/*
		 * sorry...
		 */
		if (__bio_add_page(q, bio, pages[i], bytes, offset) < bytes)
			break;

		len -= bytes;
		offset = 0;
	}

	/*
	 * release the pages we didn't map into the bio, if any
	 */
	while (i < nr_pages)
		page_cache_release(pages[i++]);

	kfree(pages);

	/*
	 * set data direction, and check if mapped pages need bouncing
	 */
	if (!write_to_vm)
		bio->bi_rw |= (1 << BIO_RW);

	bio->bi_flags |= (1 << BIO_USER_MAPPED);
	return bio;
out:
	kfree(pages);
	bio_put(bio);
	return ERR_PTR(ret);
}

/**
 *	bio_map_user	-	map user address into bio
 *	@bdev: destination block device
 *	@uaddr: start of user address
 *	@len: length in bytes
 *	@write_to_vm: bool indicating writing to pages or not
 *
 *	Map the user space address into a bio suitable for io to a block
 *	device. Returns an error pointer in case of error.
 */
struct bio *bio_map_user(request_queue_t *q, struct block_device *bdev,
			 unsigned long uaddr, unsigned int len, int write_to_vm)
{
	struct bio *bio;

	bio = __bio_map_user(q, bdev, uaddr, len, write_to_vm);

	if (IS_ERR(bio))
		return bio;

	/*
	 * subtle -- if __bio_map_user() ended up bouncing a bio,
	 * it would normally disappear when its bi_end_io is run.
	 * however, we need it for the unmap, so grab an extra
	 * reference to it
	 */
	bio_get(bio);

	if (bio->bi_size == len)
		return bio;

	/*
	 * don't support partial mappings
	 */
	bio_endio(bio, bio->bi_size, 0);
	bio_unmap_user(bio);
	return ERR_PTR(-EINVAL);
}

static void __bio_unmap_user(struct bio *bio)
{
	struct bio_vec *bvec;
	int i;

	/*
	 * find original bio if it was bounced
	 */
	if (bio->bi_private) {
		/*
		 * someone stole our bio, must not happen
		 */
		BUG_ON(!bio_flagged(bio, BIO_BOUNCED));
	
		bio = bio->bi_private;
	}

	/*
	 * make sure we dirty pages we wrote to
	 */
	__bio_for_each_segment(bvec, bio, i, 0) {
		if (bio_data_dir(bio) == READ)
			set_page_dirty_lock(bvec->bv_page);

		page_cache_release(bvec->bv_page);
	}

	bio_put(bio);
}

/**
 *	bio_unmap_user	-	unmap a bio
 *	@bio:		the bio being unmapped
 *
 *	Unmap a bio previously mapped by bio_map_user(). Must be called with
 *	a process context.
 *
 *	bio_unmap_user() may sleep.
 */
void bio_unmap_user(struct bio *bio)
{
	__bio_unmap_user(bio);
	bio_put(bio);
}

/*
 * bio_set_pages_dirty() and bio_check_pages_dirty() are support functions
 * for performing direct-IO in BIOs.
 *
 * The problem is that we cannot run set_page_dirty() from interrupt context
 * because the required locks are not interrupt-safe.  So what we can do is to
 * mark the pages dirty _before_ performing IO.  And in interrupt context,
 * check that the pages are still dirty.   If so, fine.  If not, redirty them
 * in process context.
 *
 * We special-case compound pages here: normally this means reads into hugetlb
 * pages.  The logic in here doesn't really work right for compound pages
 * because the VM does not uniformly chase down the head page in all cases.
 * But dirtiness of compound pages is pretty meaningless anyway: the VM doesn't
 * handle them at all.  So we skip compound pages here at an early stage.
 *
 * Note that this code is very hard to test under normal circumstances because
 * direct-io pins the pages with get_user_pages().  This makes
 * is_page_cache_freeable return false, and the VM will not clean the pages.
 * But other code (eg, pdflush) could clean the pages if they are mapped
 * pagecache.
 *
 * Simply disabling the call to bio_set_pages_dirty() is a good way to test the
 * deferred bio dirtying paths.
 */

/*
 * bio_set_pages_dirty() will mark all the bio's pages as dirty.
 */
void bio_set_pages_dirty(struct bio *bio)
{
	struct bio_vec *bvec = bio->bi_io_vec;
	int i;

	for (i = 0; i < bio->bi_vcnt; i++) {
		struct page *page = bvec[i].bv_page;

		if (page && !PageCompound(page))
			set_page_dirty_lock(page);
	}
}

static void bio_release_pages(struct bio *bio)
{
	struct bio_vec *bvec = bio->bi_io_vec;
	int i;

	for (i = 0; i < bio->bi_vcnt; i++) {
		struct page *page = bvec[i].bv_page;

		if (page)
			put_page(page);
	}
}

/*
 * bio_check_pages_dirty() will check that all the BIO's pages are still dirty.
 * If they are, then fine.  If, however, some pages are clean then they must
 * have been written out during the direct-IO read.  So we take another ref on
 * the BIO and the offending pages and re-dirty the pages in process context.
 *
 * It is expected that bio_check_pages_dirty() will wholly own the BIO from
 * here on.  It will run one page_cache_release() against each page and will
 * run one bio_put() against the BIO.
 */

static void bio_dirty_fn(void *data);

static DECLARE_WORK(bio_dirty_work, bio_dirty_fn, NULL);
static spinlock_t bio_dirty_lock = SPIN_LOCK_UNLOCKED;
static struct bio *bio_dirty_list;

/*
 * This runs in process context
 */
static void bio_dirty_fn(void *data)
{
	unsigned long flags;
	struct bio *bio;

	spin_lock_irqsave(&bio_dirty_lock, flags);
	bio = bio_dirty_list;
	bio_dirty_list = NULL;
	spin_unlock_irqrestore(&bio_dirty_lock, flags);

	while (bio) {
		struct bio *next = bio->bi_private;

		bio_set_pages_dirty(bio);
		bio_release_pages(bio);
		bio_put(bio);
		bio = next;
	}
}

void bio_check_pages_dirty(struct bio *bio)
{
	struct bio_vec *bvec = bio->bi_io_vec;
	int nr_clean_pages = 0;
	int i;

	for (i = 0; i < bio->bi_vcnt; i++) {
		struct page *page = bvec[i].bv_page;

		if (PageDirty(page) || PageCompound(page)) {
			page_cache_release(page);
			bvec[i].bv_page = NULL;
		} else {
			nr_clean_pages++;
		}
	}

	if (nr_clean_pages) {
		unsigned long flags;

		spin_lock_irqsave(&bio_dirty_lock, flags);
		bio->bi_private = bio_dirty_list;
		bio_dirty_list = bio;
		spin_unlock_irqrestore(&bio_dirty_lock, flags);
		schedule_work(&bio_dirty_work);
	} else {
		bio_put(bio);
	}
}

/**
 * bio_endio - end I/O on a bio
 * @bio:	bio
 * @bytes_done:	number of bytes completed
 * @error:	error, if any
 *
 * Description:
 *   bio_endio() will end I/O on @bytes_done number of bytes. This may be
 *   just a partial part of the bio, or it may be the whole bio. bio_endio()
 *   is the preferred way to end I/O on a bio, it takes care of decrementing
 *   bi_size and clearing BIO_UPTODATE on error. @error is 0 on success, and
 *   and one of the established -Exxxx (-EIO, for instance) error values in
 *   case something went wrong. Noone should call bi_end_io() directly on
 *   a bio unless they own it and thus know that it has an end_io function.
 **/
void bio_endio(struct bio *bio, unsigned int bytes_done, int error)
{
	if (error)
		clear_bit(BIO_UPTODATE, &bio->bi_flags);

	if (unlikely(bytes_done > bio->bi_size)) {
		printk("%s: want %u bytes done, only %u left\n", __FUNCTION__,
						bytes_done, bio->bi_size);
		bytes_done = bio->bi_size;
	}

	bio->bi_size -= bytes_done;
	bio->bi_sector += (bytes_done >> 9);

	if (bio->bi_end_io)
		bio->bi_end_io(bio, bytes_done, error);
}

void bio_pair_release(struct bio_pair *bp)
{
	if (atomic_dec_and_test(&bp->cnt)) {
		struct bio *master = bp->bio1.bi_private;

		bio_endio(master, master->bi_size, bp->error);
		mempool_free(bp, bp->bio2.bi_private);
	}
}

static int bio_pair_end_1(struct bio * bi, unsigned int done, int err)
{
	struct bio_pair *bp = container_of(bi, struct bio_pair, bio1);

	if (err)
		bp->error = err;

	if (bi->bi_size)
		return 1;

	bio_pair_release(bp);
	return 0;
}

static int bio_pair_end_2(struct bio * bi, unsigned int done, int err)
{
	struct bio_pair *bp = container_of(bi, struct bio_pair, bio2);

	if (err)
		bp->error = err;

	if (bi->bi_size)
		return 1;

	bio_pair_release(bp);
	return 0;
}

/*
 * split a bio - only worry about a bio with a single page
 * in it's iovec
 */
struct bio_pair *bio_split(struct bio *bi, mempool_t *pool, int first_sectors)
{
	struct bio_pair *bp = mempool_alloc(pool, GFP_NOIO);

	if (!bp)
		return bp;

	BUG_ON(bi->bi_vcnt != 1);
	BUG_ON(bi->bi_idx != 0);
	atomic_set(&bp->cnt, 3);
	bp->error = 0;
	bp->bio1 = *bi;
	bp->bio2 = *bi;
	bp->bio2.bi_sector += first_sectors;
	bp->bio2.bi_size -= first_sectors << 9;
	bp->bio1.bi_size = first_sectors << 9;

	bp->bv1 = bi->bi_io_vec[0];
	bp->bv2 = bi->bi_io_vec[0];
	bp->bv2.bv_offset += first_sectors << 9;
	bp->bv2.bv_len -= first_sectors << 9;
	bp->bv1.bv_len = first_sectors << 9;

	bp->bio1.bi_io_vec = &bp->bv1;
	bp->bio2.bi_io_vec = &bp->bv2;

	bp->bio1.bi_end_io = bio_pair_end_1;
	bp->bio2.bi_end_io = bio_pair_end_2;

	bp->bio1.bi_private = bi;
	bp->bio2.bi_private = pool;

	return bp;
}

static void *bio_pair_alloc(int gfp_flags, void *data)
{
	return kmalloc(sizeof(struct bio_pair), gfp_flags);
}

static void bio_pair_free(void *bp, void *data)
{
	kfree(bp);
}

static void __init biovec_init_pools(void)
{
	int i, size, megabytes, pool_entries = BIO_POOL_SIZE;
	int scale = BIOVEC_NR_POOLS;

	megabytes = nr_free_pages() >> (20 - PAGE_SHIFT);

	/*
	 * find out where to start scaling
	 */
	if (megabytes <= 16)
		scale = 0;
	else if (megabytes <= 32)
		scale = 1;
	else if (megabytes <= 64)
		scale = 2;
	else if (megabytes <= 96)
		scale = 3;
	else if (megabytes <= 128)
		scale = 4;

	/*
	 * scale number of entries
	 */
	pool_entries = megabytes * 2;
	if (pool_entries > 256)
		pool_entries = 256;

	for (i = 0; i < BIOVEC_NR_POOLS; i++) {
		struct biovec_pool *bp = bvec_array + i;

		size = bp->nr_vecs * sizeof(struct bio_vec);

		bp->slab = kmem_cache_create(bp->name, size, 0,
				SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL, NULL);

		if (i >= scale)
			pool_entries >>= 1;

		bp->pool = mempool_create(pool_entries, mempool_alloc_slab,
					mempool_free_slab, bp->slab);
		if (!bp->pool)
			panic("biovec: can't init mempool\n");
	}
}

static int __init init_bio(void)
{
	bio_slab = kmem_cache_create("bio", sizeof(struct bio), 0,
				SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL, NULL);
	bio_pool = mempool_create(BIO_POOL_SIZE, mempool_alloc_slab,
				mempool_free_slab, bio_slab);
	if (!bio_pool)
		panic("bio: can't create mempool\n");

	biovec_init_pools();

	bio_split_pool = mempool_create(BIO_SPLIT_ENTRIES,
				bio_pair_alloc, bio_pair_free, NULL);
	if (!bio_split_pool)
		panic("bio: can't create split pool\n");

	return 0;
}

subsys_initcall(init_bio);

EXPORT_SYMBOL(bio_alloc);
EXPORT_SYMBOL(bio_put);
EXPORT_SYMBOL(bio_endio);
EXPORT_SYMBOL(bio_init);
EXPORT_SYMBOL(__bio_clone);
EXPORT_SYMBOL(bio_clone);
EXPORT_SYMBOL(bio_phys_segments);
EXPORT_SYMBOL(bio_hw_segments);
EXPORT_SYMBOL(bio_add_page);
EXPORT_SYMBOL(bio_get_nr_vecs);
EXPORT_SYMBOL(bio_map_user);
EXPORT_SYMBOL(bio_unmap_user);
EXPORT_SYMBOL(bio_pair_release);
EXPORT_SYMBOL(bio_split);
EXPORT_SYMBOL(bio_split_pool);
EXPORT_SYMBOL(bio_copy_user);
EXPORT_SYMBOL(bio_uncopy_user);
