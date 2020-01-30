/*
 * Copyright (C) 2001, 2002 Sistina Software (UK) Limited.
 *
 * This file is released under the GPL.
 */

#include "dm.h"
#include "dm-bio-list.h"

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/blkpg.h>
#include <linux/bio.h>
#include <linux/buffer_head.h>
#include <linux/mempool.h>
#include <linux/slab.h>

static const char *_name = DM_NAME;

static unsigned int major = 0;
static unsigned int _major = 0;

static int realloc_minor_bits(unsigned long requested_minor);
static void free_minor_bits(void);

/*
 * One of these is allocated per bio.
 */
struct dm_io {
	struct mapped_device *md;
	int error;
	struct bio *bio;
	atomic_t io_count;
};

/*
 * One of these is allocated per target within a bio.  Hopefully
 * this will be simplified out one day.
 */
struct target_io {
	struct dm_io *io;
	struct dm_target *ti;
	union map_info info;
};

/*
 * Bits for the md->flags field.
 */
#define DMF_BLOCK_IO 0
#define DMF_SUSPENDED 1
#define DMF_FS_LOCKED 2

struct mapped_device {
	struct rw_semaphore lock;
	rwlock_t map_lock;
	atomic_t holders;

	unsigned long flags;

	request_queue_t *queue;
	struct gendisk *disk;

	/*
	 * A list of ios that arrived while we were suspended.
	 */
	atomic_t pending;
	wait_queue_head_t wait;
 	struct bio_list deferred;

	/*
	 * The current mapping.
	 */
	struct dm_table *map;

	/*
	 * io objects are allocated from here.
	 */
	mempool_t *io_pool;
	mempool_t *tio_pool;

	/*
	 * Event handling.
	 */
	atomic_t event_nr;
	wait_queue_head_t eventq;

	/*
	 * freeze/thaw support require holding onto a super block
	 */
	struct super_block *frozen_sb;
};

#define MIN_IOS 256
static kmem_cache_t *_io_cache;
static kmem_cache_t *_tio_cache;

static int __init local_init(void)
{
	int r;

	/* allocate a slab for the dm_ios */
	_io_cache = kmem_cache_create("dm_io",
				      sizeof(struct dm_io), 0, 0, NULL, NULL);
	if (!_io_cache)
		return -ENOMEM;

	/* allocate a slab for the target ios */
	_tio_cache = kmem_cache_create("dm_tio", sizeof(struct target_io),
				       0, 0, NULL, NULL);
	if (!_tio_cache) {
		kmem_cache_destroy(_io_cache);
		return -ENOMEM;
	}

	r = realloc_minor_bits(1024);
	if (r < 0) {
		kmem_cache_destroy(_tio_cache);
		kmem_cache_destroy(_io_cache);
		return r;
	}

	_major = major;
	r = register_blkdev(_major, _name);
	if (r < 0) {
		kmem_cache_destroy(_tio_cache);
		kmem_cache_destroy(_io_cache);
		free_minor_bits();
		return r;
	}

	if (!_major)
		_major = r;

	return 0;
}

static void local_exit(void)
{
	kmem_cache_destroy(_tio_cache);
	kmem_cache_destroy(_io_cache);
	free_minor_bits();

	if (unregister_blkdev(_major, _name) < 0)
		DMERR("devfs_unregister_blkdev failed");

	_major = 0;

	DMINFO("cleaned up");
}

/*
 * We have a lot of init/exit functions, so it seems easier to
 * store them in an array.  The disposable macro 'xx'
 * expands a prefix into a pair of function names.
 */
static struct {
	int (*init) (void);
	void (*exit) (void);

} _inits[] = {
#define xx(n) {n ## _init, n ## _exit},
	xx(local)
	xx(dm_target)
	xx(dm_linear)
	xx(dm_stripe)
	xx(dm_interface)
#undef xx
};

static int __init dm_init(void)
{
	const int count = ARRAY_SIZE(_inits);

	int r, i;

	for (i = 0; i < count; i++) {
		r = _inits[i].init();
		if (r)
			goto bad;
	}

	return 0;

      bad:
	while (i--)
		_inits[i].exit();

	return r;
}

static void __exit dm_exit(void)
{
	int i = ARRAY_SIZE(_inits);

	while (i--)
		_inits[i].exit();
}

/*
 * Block device functions
 */
static int dm_blk_open(struct inode *inode, struct file *file)
{
	struct mapped_device *md;

	md = inode->i_bdev->bd_disk->private_data;
	dm_get(md);
	return 0;
}

static int dm_blk_close(struct inode *inode, struct file *file)
{
	struct mapped_device *md;

	md = inode->i_bdev->bd_disk->private_data;
	dm_put(md);
	return 0;
}

static inline struct dm_io *alloc_io(struct mapped_device *md)
{
	return mempool_alloc(md->io_pool, GFP_NOIO);
}

static inline void free_io(struct mapped_device *md, struct dm_io *io)
{
	mempool_free(io, md->io_pool);
}

static inline struct target_io *alloc_tio(struct mapped_device *md)
{
	return mempool_alloc(md->tio_pool, GFP_NOIO);
}

static inline void free_tio(struct mapped_device *md, struct target_io *tio)
{
	mempool_free(tio, md->tio_pool);
}

/*
 * Add the bio to the list of deferred io.
 */
static int queue_io(struct mapped_device *md, struct bio *bio)
{
	down_write(&md->lock);

	if (!test_bit(DMF_BLOCK_IO, &md->flags)) {
		up_write(&md->lock);
		return 1;
	}

	bio_list_add(&md->deferred, bio);

	up_write(&md->lock);
	return 0;		/* deferred successfully */
}

/*
 * Everyone (including functions in this file), should use this
 * function to access the md->map field, and make sure they call
 * dm_table_put() when finished.
 */
struct dm_table *dm_get_table(struct mapped_device *md)
{
	struct dm_table *t;

	read_lock(&md->map_lock);
	t = md->map;
	if (t)
		dm_table_get(t);
	read_unlock(&md->map_lock);

	return t;
}

/*-----------------------------------------------------------------
 * CRUD START:
 *   A more elegant soln is in the works that uses the queue
 *   merge fn, unfortunately there are a couple of changes to
 *   the block layer that I want to make for this.  So in the
 *   interests of getting something for people to use I give
 *   you this clearly demarcated crap.
 *---------------------------------------------------------------*/

/*
 * Decrements the number of outstanding ios that a bio has been
 * cloned into, completing the original io if necc.
 */
static inline void dec_pending(struct dm_io *io, int error)
{
	if (error)
		io->error = error;

	if (atomic_dec_and_test(&io->io_count)) {
		if (atomic_dec_and_test(&io->md->pending))
			/* nudge anyone waiting on suspend queue */
			wake_up(&io->md->wait);

		bio_endio(io->bio, io->bio->bi_size, io->error);
		free_io(io->md, io);
	}
}

static int clone_endio(struct bio *bio, unsigned int done, int error)
{
	int r = 0;
	struct target_io *tio = bio->bi_private;
	struct dm_io *io = tio->io;
	dm_endio_fn endio = tio->ti->type->end_io;

	if (bio->bi_size)
		return 1;

	if (!bio_flagged(bio, BIO_UPTODATE) && !error)
		error = -EIO;

	if (endio) {
		r = endio(tio->ti, bio, error, &tio->info);
		if (r < 0)
			error = r;

		else if (r > 0)
			/* the target wants another shot at the io */
			return 1;
	}

	free_tio(io->md, tio);
	dec_pending(io, error);
	bio_put(bio);
	return r;
}

static sector_t max_io_len(struct mapped_device *md,
			   sector_t sector, struct dm_target *ti)
{
	sector_t offset = sector - ti->begin;
	sector_t len = ti->len - offset;

	/*
	 * Does the target need to split even further ?
	 */
	if (ti->split_io) {
		sector_t boundary;
		boundary = dm_round_up(offset + 1, ti->split_io) - offset;

		if (len > boundary)
			len = boundary;
	}

	return len;
}

static void __map_bio(struct dm_target *ti, struct bio *clone,
		      struct target_io *tio)
{
	int r;

	/*
	 * Sanity checks.
	 */
	BUG_ON(!clone->bi_size);

	clone->bi_end_io = clone_endio;
	clone->bi_private = tio;

	/*
	 * Map the clone.  If r == 0 we don't need to do
	 * anything, the target has assumed ownership of
	 * this io.
	 */
	atomic_inc(&tio->io->io_count);
	r = ti->type->map(ti, clone, &tio->info);
	if (r > 0)
		/* the bio has been remapped so dispatch it */
		generic_make_request(clone);

	else if (r < 0) {
		/* error the io and bail out */
		struct dm_io *io = tio->io;
		free_tio(tio->io->md, tio);
		dec_pending(io, -EIO);
		bio_put(clone);
	}
}

struct clone_info {
	struct mapped_device *md;
	struct dm_table *map;
	struct bio *bio;
	struct dm_io *io;
	sector_t sector;
	sector_t sector_count;
	unsigned short idx;
};

/*
 * Creates a little bio that is just does part of a bvec.
 */
static struct bio *split_bvec(struct bio *bio, sector_t sector,
			      unsigned short idx, unsigned int offset,
			      unsigned int len)
{
	struct bio *clone;
	struct bio_vec *bv = bio->bi_io_vec + idx;

	clone = bio_alloc(GFP_NOIO, 1);
	*clone->bi_io_vec = *bv;

	clone->bi_sector = sector;
	clone->bi_bdev = bio->bi_bdev;
	clone->bi_rw = bio->bi_rw;
	clone->bi_vcnt = 1;
	clone->bi_size = to_bytes(len);
	clone->bi_io_vec->bv_offset = offset;
	clone->bi_io_vec->bv_len = clone->bi_size;

	return clone;
}

/*
 * Creates a bio that consists of range of complete bvecs.
 */
static struct bio *clone_bio(struct bio *bio, sector_t sector,
			     unsigned short idx, unsigned short bv_count,
			     unsigned int len)
{
	struct bio *clone;

	clone = bio_clone(bio, GFP_NOIO);
	clone->bi_sector = sector;
	clone->bi_idx = idx;
	clone->bi_vcnt = idx + bv_count;
	clone->bi_size = to_bytes(len);
	clone->bi_flags &= ~(1 << BIO_SEG_VALID);

	return clone;
}

static void __clone_and_map(struct clone_info *ci)
{
	struct bio *clone, *bio = ci->bio;
	struct dm_target *ti = dm_table_find_target(ci->map, ci->sector);
	sector_t len = 0, max = max_io_len(ci->md, ci->sector, ti);
	struct target_io *tio;

	/*
	 * Allocate a target io object.
	 */
	tio = alloc_tio(ci->md);
	tio->io = ci->io;
	tio->ti = ti;
	memset(&tio->info, 0, sizeof(tio->info));

	if (ci->sector_count <= max) {
		/*
		 * Optimise for the simple case where we can do all of
		 * the remaining io with a single clone.
		 */
		clone = clone_bio(bio, ci->sector, ci->idx,
				  bio->bi_vcnt - ci->idx, ci->sector_count);
		__map_bio(ti, clone, tio);
		ci->sector_count = 0;

	} else if (to_sector(bio->bi_io_vec[ci->idx].bv_len) <= max) {
		/*
		 * There are some bvecs that don't span targets.
		 * Do as many of these as possible.
		 */
		int i;
		sector_t remaining = max;
		sector_t bv_len;

		for (i = ci->idx; remaining && (i < bio->bi_vcnt); i++) {
			bv_len = to_sector(bio->bi_io_vec[i].bv_len);

			if (bv_len > remaining)
				break;

			remaining -= bv_len;
			len += bv_len;
		}

		clone = clone_bio(bio, ci->sector, ci->idx, i - ci->idx, len);
		__map_bio(ti, clone, tio);

		ci->sector += len;
		ci->sector_count -= len;
		ci->idx = i;

	} else {
		/*
		 * Create two copy bios to deal with io that has
		 * been split across a target.
		 */
		struct bio_vec *bv = bio->bi_io_vec + ci->idx;

		clone = split_bvec(bio, ci->sector, ci->idx,
				   bv->bv_offset, max);
		__map_bio(ti, clone, tio);

		ci->sector += max;
		ci->sector_count -= max;
		ti = dm_table_find_target(ci->map, ci->sector);

		len = to_sector(bv->bv_len) - max;
		clone = split_bvec(bio, ci->sector, ci->idx,
				   bv->bv_offset + to_bytes(max), len);
		tio = alloc_tio(ci->md);
		tio->io = ci->io;
		tio->ti = ti;
		memset(&tio->info, 0, sizeof(tio->info));
		__map_bio(ti, clone, tio);

		ci->sector += len;
		ci->sector_count -= len;
		ci->idx++;
	}
}

/*
 * Split the bio into several clones.
 */
static void __split_bio(struct mapped_device *md, struct bio *bio)
{
	struct clone_info ci;

	ci.map = dm_get_table(md);
	if (!ci.map) {
		bio_io_error(bio, bio->bi_size);
		return;
	}

	ci.md = md;
	ci.bio = bio;
	ci.io = alloc_io(md);
	ci.io->error = 0;
	atomic_set(&ci.io->io_count, 1);
	ci.io->bio = bio;
	ci.io->md = md;
	ci.sector = bio->bi_sector;
	ci.sector_count = bio_sectors(bio);
	ci.idx = bio->bi_idx;

	atomic_inc(&md->pending);
	while (ci.sector_count)
		__clone_and_map(&ci);

	/* drop the extra reference count */
	dec_pending(ci.io, 0);
	dm_table_put(ci.map);
}
/*-----------------------------------------------------------------
 * CRUD END
 *---------------------------------------------------------------*/

/*
 * The request function that just remaps the bio built up by
 * dm_merge_bvec.
 */
static int dm_request(request_queue_t *q, struct bio *bio)
{
	int r;
	struct mapped_device *md = q->queuedata;

	down_read(&md->lock);

	/*
	 * If we're suspended we have to queue
	 * this io for later.
	 */
	while (test_bit(DMF_BLOCK_IO, &md->flags)) {
		up_read(&md->lock);

		if (bio_rw(bio) == READA) {
			bio_io_error(bio, bio->bi_size);
			return 0;
		}

		r = queue_io(md, bio);
		if (r < 0) {
			bio_io_error(bio, bio->bi_size);
			return 0;

		} else if (r == 0)
			return 0;	/* deferred successfully */

		/*
		 * We're in a while loop, because someone could suspend
		 * before we get to the following read lock.
		 */
		down_read(&md->lock);
	}

	__split_bio(md, bio);
	up_read(&md->lock);
	return 0;
}

static void dm_unplug_all(request_queue_t *q)
{
	struct mapped_device *md = q->queuedata;
	struct dm_table *map = dm_get_table(md);

	if (map) {
		dm_table_unplug_all(map);
		dm_table_put(map);
	}
}

static int dm_any_congested(void *congested_data, int bdi_bits)
{
	int r;
	struct mapped_device *md = (struct mapped_device *) congested_data;
	struct dm_table *map = dm_get_table(md);

	if (!map || test_bit(DMF_BLOCK_IO, &md->flags))
		r = bdi_bits;
	else
		r = dm_table_any_congested(map, bdi_bits);

	dm_table_put(map);
	return r;
}

/*-----------------------------------------------------------------
 * A bitset is used to keep track of allocated minor numbers.
 *---------------------------------------------------------------*/
static DECLARE_MUTEX(_minor_lock);
static unsigned long *_minor_bits = NULL;
static unsigned long _max_minors = 0;

#define MINORS_SIZE(minors) ((minors / BITS_PER_LONG) * sizeof(unsigned long))

static int realloc_minor_bits(unsigned long requested_minor)
{
	unsigned long max_minors;
	unsigned long *minor_bits, *tmp;

	if (requested_minor < _max_minors)
		return -EINVAL;

	/* Round up the requested minor to the next power-of-2. */
	max_minors = 1 << fls(requested_minor - 1);
	if (max_minors > (1 << MINORBITS))
		return -EINVAL;

	minor_bits = kmalloc(MINORS_SIZE(max_minors), GFP_KERNEL);
	if (!minor_bits)
		return -ENOMEM;
	memset(minor_bits, 0, MINORS_SIZE(max_minors));

	/* Copy the existing bit-set to the new one. */
	if (_minor_bits)
		memcpy(minor_bits, _minor_bits, MINORS_SIZE(_max_minors));

	tmp = _minor_bits;
	_minor_bits = minor_bits;
	_max_minors = max_minors;
	if (tmp)
		kfree(tmp);

	return 0;
}

static void free_minor_bits(void)
{
	down(&_minor_lock);
	kfree(_minor_bits);
	_minor_bits = NULL;
	_max_minors = 0;
	up(&_minor_lock);
}

static void free_minor(unsigned int minor)
{
	down(&_minor_lock);
	if (minor < _max_minors)
		clear_bit(minor, _minor_bits);
	up(&_minor_lock);
}

/*
 * See if the device with a specific minor # is free.
 */
static int specific_minor(unsigned int minor)
{
	int r = 0;

	if (minor > (1 << MINORBITS))
		return -EINVAL;

	down(&_minor_lock);
	if (minor >= _max_minors) {
		r = realloc_minor_bits(minor);
		if (r) {
			up(&_minor_lock);
			return r;
		}
	}

	if (test_and_set_bit(minor, _minor_bits))
		r = -EBUSY;
	up(&_minor_lock);

	return r;
}

static int next_free_minor(unsigned int *minor)
{
	int r;
	unsigned int m;

	down(&_minor_lock);
	m = find_first_zero_bit(_minor_bits, _max_minors);
	if (m >= _max_minors) {
		r = realloc_minor_bits(_max_minors * 2);
		if (r) {
			up(&_minor_lock);
			return r;
		}
		m = find_first_zero_bit(_minor_bits, _max_minors);
	}

	set_bit(m, _minor_bits);
	*minor = m;
	up(&_minor_lock);

	return 0;
}

static struct block_device_operations dm_blk_dops;

/*
 * Allocate and initialise a blank device with a given minor.
 */
static struct mapped_device *alloc_dev(unsigned int minor, int persistent)
{
	int r;
	struct mapped_device *md = kmalloc(sizeof(*md), GFP_KERNEL);

	if (!md) {
		DMWARN("unable to allocate device, out of memory.");
		return NULL;
	}

	/* get a minor number for the dev */
	r = persistent ? specific_minor(minor) : next_free_minor(&minor);
	if (r < 0)
		goto bad1;

	memset(md, 0, sizeof(*md));
	init_rwsem(&md->lock);
	rwlock_init(&md->map_lock);
	atomic_set(&md->holders, 1);
	atomic_set(&md->event_nr, 0);

	md->queue = blk_alloc_queue(GFP_KERNEL);
	if (!md->queue)
		goto bad1;

	md->queue->queuedata = md;
	md->queue->backing_dev_info.congested_fn = dm_any_congested;
	md->queue->backing_dev_info.congested_data = md;
	blk_queue_make_request(md->queue, dm_request);
	md->queue->unplug_fn = dm_unplug_all;

	md->io_pool = mempool_create(MIN_IOS, mempool_alloc_slab,
				     mempool_free_slab, _io_cache);
 	if (!md->io_pool)
 		goto bad2;

	md->tio_pool = mempool_create(MIN_IOS, mempool_alloc_slab,
				      mempool_free_slab, _tio_cache);
	if (!md->tio_pool)
		goto bad3;

	md->disk = alloc_disk(1);
	if (!md->disk)
		goto bad4;

	md->disk->major = _major;
	md->disk->first_minor = minor;
	md->disk->fops = &dm_blk_dops;
	md->disk->queue = md->queue;
	md->disk->private_data = md;
	sprintf(md->disk->disk_name, "dm-%d", minor);
	add_disk(md->disk);

	atomic_set(&md->pending, 0);
	init_waitqueue_head(&md->wait);
	init_waitqueue_head(&md->eventq);

	return md;

 bad4:
	mempool_destroy(md->tio_pool);
 bad3:
	mempool_destroy(md->io_pool);
 bad2:
	blk_put_queue(md->queue);
	free_minor(minor);
 bad1:
	kfree(md);
	return NULL;
}

static void free_dev(struct mapped_device *md)
{
	free_minor(md->disk->first_minor);
	mempool_destroy(md->tio_pool);
	mempool_destroy(md->io_pool);
	del_gendisk(md->disk);
	put_disk(md->disk);
	blk_put_queue(md->queue);
	kfree(md);
}

/*
 * Bind a table to the device.
 */
static void event_callback(void *context)
{
	struct mapped_device *md = (struct mapped_device *) context;

	atomic_inc(&md->event_nr);;
	wake_up(&md->eventq);
}

static void __set_size(struct gendisk *disk, sector_t size)
{
	struct block_device *bdev;

	set_capacity(disk, size);
	bdev = bdget_disk(disk, 0);
	if (bdev) {
		down(&bdev->bd_inode->i_sem);
		i_size_write(bdev->bd_inode, (loff_t)size << SECTOR_SHIFT);
		up(&bdev->bd_inode->i_sem);
		bdput(bdev);
	}
}

static int __bind(struct mapped_device *md, struct dm_table *t)
{
	request_queue_t *q = md->queue;
	sector_t size;

	size = dm_table_get_size(t);
	__set_size(md->disk, size);
	if (size == 0)
		return 0;

	write_lock(&md->map_lock);
	md->map = t;
	write_unlock(&md->map_lock);

	dm_table_get(t);
	dm_table_event_callback(md->map, event_callback, md);
	dm_table_set_restrictions(t, q);
	return 0;
}

static void __unbind(struct mapped_device *md)
{
	struct dm_table *map = md->map;

	if (!map)
		return;

	dm_table_event_callback(map, NULL, NULL);
	write_lock(&md->map_lock);
	md->map = NULL;
	write_unlock(&md->map_lock);
	dm_table_put(map);
}

/*
 * Constructor for a new device.
 */
static int create_aux(unsigned int minor, int persistent,
		      struct mapped_device **result)
{
	struct mapped_device *md;

	md = alloc_dev(minor, persistent);
	if (!md)
		return -ENXIO;

	*result = md;
	return 0;
}

int dm_create(struct mapped_device **result)
{
	return create_aux(0, 0, result);
}

int dm_create_with_minor(unsigned int minor, struct mapped_device **result)
{
	return create_aux(minor, 1, result);
}

void dm_get(struct mapped_device *md)
{
	atomic_inc(&md->holders);
}

void dm_put(struct mapped_device *md)
{
	struct dm_table *map = dm_get_table(md);

	if (atomic_dec_and_test(&md->holders)) {
		if (!test_bit(DMF_SUSPENDED, &md->flags) && map)
			dm_table_suspend_targets(map);
		__unbind(md);
		free_dev(md);
	}

	dm_table_put(map);
}

/*
 * Process the deferred bios
 */
static void __flush_deferred_io(struct mapped_device *md, struct bio *c)
{
	struct bio *n;

	while (c) {
		n = c->bi_next;
		c->bi_next = NULL;
		__split_bio(md, c);
		c = n;
	}
}

/*
 * Swap in a new table (destroying old one).
 */
int dm_swap_table(struct mapped_device *md, struct dm_table *table)
{
	int r;

	down_write(&md->lock);

	/* device must be suspended */
	if (!test_bit(DMF_SUSPENDED, &md->flags)) {
		up_write(&md->lock);
		return -EPERM;
	}

	__unbind(md);
	r = __bind(md, table);
	if (r)
		return r;

	up_write(&md->lock);
	return 0;
}

/*
 * Functions to lock and unlock any filesystem running on the
 * device.
 */
static int __lock_fs(struct mapped_device *md)
{
	struct block_device *bdev;

	if (test_and_set_bit(DMF_FS_LOCKED, &md->flags))
		return 0;

	bdev = bdget_disk(md->disk, 0);
	if (!bdev) {
		DMWARN("bdget failed in __lock_fs");
		return -ENOMEM;
	}

	WARN_ON(md->frozen_sb);
	md->frozen_sb = freeze_bdev(bdev);
	/* don't bdput right now, we don't want the bdev
	 * to go away while it is locked.  We'll bdput
	 * in __unlock_fs
	 */
	return 0;
}

static int __unlock_fs(struct mapped_device *md)
{
	struct block_device *bdev;

	if (!test_and_clear_bit(DMF_FS_LOCKED, &md->flags))
		return 0;

	bdev = bdget_disk(md->disk, 0);
	if (!bdev) {
		DMWARN("bdget failed in __unlock_fs");
		return -ENOMEM;
	}

	thaw_bdev(bdev, md->frozen_sb);
	md->frozen_sb = NULL;
	bdput(bdev);
	bdput(bdev);
	return 0;
}

/*
 * We need to be able to change a mapping table under a mounted
 * filesystem.  For example we might want to move some data in
 * the background.  Before the table can be swapped with
 * dm_bind_table, dm_suspend must be called to flush any in
 * flight bios and ensure that any further io gets deferred.
 */
int dm_suspend(struct mapped_device *md)
{
	struct dm_table *map;
	DECLARE_WAITQUEUE(wait, current);

	/* Flush I/O to the device. */
	down_read(&md->lock);
	if (test_bit(DMF_BLOCK_IO, &md->flags)) {
		up_read(&md->lock);
		return -EINVAL;
	}

	__lock_fs(md);
	up_read(&md->lock);

	/*
	 * First we set the BLOCK_IO flag so no more ios will be
	 * mapped.
	 */
	down_write(&md->lock);
	if (test_bit(DMF_BLOCK_IO, &md->flags)) {
		/*
		 * If we get here we know another thread is
		 * trying to suspend as well, so we leave the fs
		 * locked for this thread.
		 */
		up_write(&md->lock);
		return -EINVAL;
	}

	set_bit(DMF_BLOCK_IO, &md->flags);
	add_wait_queue(&md->wait, &wait);
	up_write(&md->lock);

	/* unplug */
	map = dm_get_table(md);
	if (map) {
		dm_table_unplug_all(map);
		dm_table_put(map);
	}

	/*
	 * Then we wait for the already mapped ios to
	 * complete.
	 */
	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);

		if (!atomic_read(&md->pending) || signal_pending(current))
			break;

		io_schedule();
	}
	set_current_state(TASK_RUNNING);

	down_write(&md->lock);
	remove_wait_queue(&md->wait, &wait);

	/* were we interrupted ? */
	if (atomic_read(&md->pending)) {
		__unlock_fs(md);
		clear_bit(DMF_BLOCK_IO, &md->flags);
		up_write(&md->lock);
		return -EINTR;
	}

	set_bit(DMF_SUSPENDED, &md->flags);

	map = dm_get_table(md);
	if (map)
		dm_table_suspend_targets(map);
	dm_table_put(map);
	up_write(&md->lock);

	return 0;
}

int dm_resume(struct mapped_device *md)
{
	struct bio *def;
	struct dm_table *map = dm_get_table(md);

	down_write(&md->lock);
	if (!map ||
	    !test_bit(DMF_SUSPENDED, &md->flags) ||
	    !dm_table_get_size(map)) {
		up_write(&md->lock);
		dm_table_put(map);
		return -EINVAL;
	}

	dm_table_resume_targets(map);
	clear_bit(DMF_SUSPENDED, &md->flags);
	clear_bit(DMF_BLOCK_IO, &md->flags);

	def = bio_list_get(&md->deferred);
	__flush_deferred_io(md, def);
	up_write(&md->lock);
	__unlock_fs(md);
	dm_table_unplug_all(map);
	dm_table_put(map);

	return 0;
}

/*-----------------------------------------------------------------
 * Event notification.
 *---------------------------------------------------------------*/
uint32_t dm_get_event_nr(struct mapped_device *md)
{
	return atomic_read(&md->event_nr);
}

int dm_wait_event(struct mapped_device *md, int event_nr)
{
	return wait_event_interruptible(md->eventq,
			(event_nr != atomic_read(&md->event_nr)));
}

/*
 * The gendisk is only valid as long as you have a reference
 * count on 'md'.
 */
struct gendisk *dm_disk(struct mapped_device *md)
{
	return md->disk;
}

int dm_suspended(struct mapped_device *md)
{
	return test_bit(DMF_SUSPENDED, &md->flags);
}

static struct block_device_operations dm_blk_dops = {
	.open = dm_blk_open,
	.release = dm_blk_close,
	.owner = THIS_MODULE
};

/*
 * module hooks
 */
module_init(dm_init);
module_exit(dm_exit);

module_param(major, uint, 0);
MODULE_PARM_DESC(major, "The major number of the device mapper");
MODULE_DESCRIPTION(DM_NAME " driver");
MODULE_AUTHOR("Joe Thornber <thornber@sistina.com>");
MODULE_LICENSE("GPL");
