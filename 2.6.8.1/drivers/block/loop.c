/*
 *  linux/drivers/block/loop.c
 *
 *  Written by Theodore Ts'o, 3/29/93
 *
 * Copyright 1993 by Theodore Ts'o.  Redistribution of this file is
 * permitted under the GNU General Public License.
 *
 * DES encryption plus some minor changes by Werner Almesberger, 30-MAY-1993
 * more DES encryption plus IDEA encryption by Nicholas J. Leon, June 20, 1996
 *
 * Modularized and updated for 1.1.16 kernel - Mitch Dsouza 28th May 1994
 * Adapted for 1.3.59 kernel - Andries Brouwer, 1 Feb 1996
 *
 * Fixed do_loop_request() re-entrancy - Vincent.Renardias@waw.com Mar 20, 1997
 *
 * Added devfs support - Richard Gooch <rgooch@atnf.csiro.au> 16-Jan-1998
 *
 * Handle sparse backing files correctly - Kenn Humborg, Jun 28, 1998
 *
 * Loadable modules and other fixes by AK, 1998
 *
 * Make real block number available to downstream transfer functions, enables
 * CBC (and relatives) mode encryption requiring unique IVs per data block.
 * Reed H. Petty, rhp@draper.net
 *
 * Maximum number of loop devices now dynamic via max_loop module parameter.
 * Russell Kroll <rkroll@exploits.org> 19990701
 *
 * Maximum number of loop devices when compiled-in now selectable by passing
 * max_loop=<1-255> to the kernel on boot.
 * Erik I. Bols�, <eriki@himolde.no>, Oct 31, 1999
 *
 * Completely rewrite request handling to be make_request_fn style and
 * non blocking, pushing work to a helper thread. Lots of fixes from
 * Al Viro too.
 * Jens Axboe <axboe@suse.de>, Nov 2000
 *
 * Support up to 256 loop devices
 * Heinz Mauelshagen <mge@sistina.com>, Feb 2002
 *
 * Still To Fix:
 * - Advisory locking is ignored here.
 * - Should use an own CAP_* category instead of CAP_SYS_ADMIN
 *
 */

#include <linux/config.h>
#include <linux/module.h>

#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/major.h>
#include <linux/wait.h>
#include <linux/blkdev.h>
#include <linux/blkpg.h>
#include <linux/init.h>
#include <linux/devfs_fs_kernel.h>
#include <linux/smp_lock.h>
#include <linux/swap.h>
#include <linux/slab.h>
#include <linux/loop.h>
#include <linux/suspend.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>		/* for invalidate_bdev() */
#include <linux/completion.h>

#include <asm/uaccess.h>

static int max_loop = 8;
static struct loop_device *loop_dev;
static struct gendisk **disks;

/*
 * Transfer functions
 */
static int transfer_none(struct loop_device *lo, int cmd,
			 struct page *raw_page, unsigned raw_off,
			 struct page *loop_page, unsigned loop_off,
			 int size, sector_t real_block)
{
	char *raw_buf = kmap_atomic(raw_page, KM_USER0) + raw_off;
	char *loop_buf = kmap_atomic(loop_page, KM_USER1) + loop_off;

	if (cmd == READ)
		memcpy(loop_buf, raw_buf, size);
	else
		memcpy(raw_buf, loop_buf, size);

	kunmap_atomic(raw_buf, KM_USER0);
	kunmap_atomic(loop_buf, KM_USER1);
	cond_resched();
	return 0;
}

static int transfer_xor(struct loop_device *lo, int cmd,
			struct page *raw_page, unsigned raw_off,
			struct page *loop_page, unsigned loop_off,
			int size, sector_t real_block)
{
	char *raw_buf = kmap_atomic(raw_page, KM_USER0) + raw_off;
	char *loop_buf = kmap_atomic(loop_page, KM_USER1) + loop_off;
	char *in, *out, *key;
	int i, keysize;

	if (cmd == READ) {
		in = raw_buf;
		out = loop_buf;
	} else {
		in = loop_buf;
		out = raw_buf;
	}

	key = lo->lo_encrypt_key;
	keysize = lo->lo_encrypt_key_size;
	for (i = 0; i < size; i++)
		*out++ = *in++ ^ key[(i & 511) % keysize];

	kunmap_atomic(raw_buf, KM_USER0);
	kunmap_atomic(loop_buf, KM_USER1);
	cond_resched();
	return 0;
}

static int xor_init(struct loop_device *lo, const struct loop_info64 *info)
{
	if (info->lo_encrypt_key_size <= 0)
		return -EINVAL;
	return 0;
}

static struct loop_func_table none_funcs = {
	.number = LO_CRYPT_NONE,
	.transfer = transfer_none,
}; 	

static struct loop_func_table xor_funcs = {
	.number = LO_CRYPT_XOR,
	.transfer = transfer_xor,
	.init = xor_init
}; 	

/* xfer_funcs[0] is special - its release function is never called */
static struct loop_func_table *xfer_funcs[MAX_LO_CRYPT] = {
	&none_funcs,
	&xor_funcs
};

static loff_t get_loop_size(struct loop_device *lo, struct file *file)
{
	loff_t size, offset, loopsize;

	/* Compute loopsize in bytes */
	size = i_size_read(file->f_mapping->host);
	offset = lo->lo_offset;
	loopsize = size - offset;
	if (lo->lo_sizelimit > 0 && lo->lo_sizelimit < loopsize)
		loopsize = lo->lo_sizelimit;

	/*
	 * Unfortunately, if we want to do I/O on the device,
	 * the number of 512-byte sectors has to fit into a sector_t.
	 */
	return loopsize >> 9;
}

static int
figure_loop_size(struct loop_device *lo)
{
	loff_t size = get_loop_size(lo, lo->lo_backing_file);
	sector_t x = (sector_t)size;

	if ((loff_t)x != size)
		return -EFBIG;

	set_capacity(disks[lo->lo_number], x);
	return 0;					
}

static inline int
lo_do_transfer(struct loop_device *lo, int cmd,
	       struct page *rpage, unsigned roffs,
	       struct page *lpage, unsigned loffs,
	       int size, sector_t rblock)
{
	if (!lo->transfer)
		return 0;

	return lo->transfer(lo, cmd, rpage, roffs, lpage, loffs, size, rblock);
}

static int
do_lo_send(struct loop_device *lo, struct bio_vec *bvec, int bsize, loff_t pos)
{
	struct file *file = lo->lo_backing_file; /* kudos to NFsckingS */
	struct address_space *mapping = file->f_mapping;
	struct address_space_operations *aops = mapping->a_ops;
	struct page *page;
	pgoff_t index;
	unsigned size, offset, bv_offs;
	int len;
	int ret = 0;

	down(&mapping->host->i_sem);
	index = pos >> PAGE_CACHE_SHIFT;
	offset = pos & ((pgoff_t)PAGE_CACHE_SIZE - 1);
	bv_offs = bvec->bv_offset;
	len = bvec->bv_len;
	while (len > 0) {
		sector_t IV;
		int transfer_result;

		IV = ((sector_t)index << (PAGE_CACHE_SHIFT - 9))+(offset >> 9);

		size = PAGE_CACHE_SIZE - offset;
		if (size > len)
			size = len;

		page = grab_cache_page(mapping, index);
		if (!page)
			goto fail;
		if (aops->prepare_write(file, page, offset, offset+size))
			goto unlock;
		transfer_result = lo_do_transfer(lo, WRITE, page, offset,
						 bvec->bv_page, bv_offs,
						 size, IV);
		if (transfer_result) {
			char *kaddr;

			/*
			 * The transfer failed, but we still write the data to
			 * keep prepare/commit calls balanced.
			 */
			printk(KERN_ERR "loop: transfer error block %llu\n",
			       (unsigned long long)index);
			kaddr = kmap_atomic(page, KM_USER0);
			memset(kaddr + offset, 0, size);
			kunmap_atomic(kaddr, KM_USER0);
		}
		flush_dcache_page(page);
		if (aops->commit_write(file, page, offset, offset+size))
			goto unlock;
		if (transfer_result)
			goto unlock;
		bv_offs += size;
		len -= size;
		offset = 0;
		index++;
		pos += size;
		unlock_page(page);
		page_cache_release(page);
	}
	up(&mapping->host->i_sem);
out:
	return ret;

unlock:
	unlock_page(page);
	page_cache_release(page);
fail:
	up(&mapping->host->i_sem);
	ret = -1;
	goto out;
}

static int
lo_send(struct loop_device *lo, struct bio *bio, int bsize, loff_t pos)
{
	struct bio_vec *bvec;
	int i, ret = 0;

	bio_for_each_segment(bvec, bio, i) {
		ret = do_lo_send(lo, bvec, bsize, pos);
		if (ret < 0)
			break;
		pos += bvec->bv_len;
	}
	return ret;
}

struct lo_read_data {
	struct loop_device *lo;
	struct page *page;
	unsigned offset;
	int bsize;
};

static int
lo_read_actor(read_descriptor_t *desc, struct page *page,
	      unsigned long offset, unsigned long size)
{
	unsigned long count = desc->count;
	struct lo_read_data *p = desc->arg.data;
	struct loop_device *lo = p->lo;
	sector_t IV;

	IV = ((sector_t) page->index << (PAGE_CACHE_SHIFT - 9))+(offset >> 9);

	if (size > count)
		size = count;

	if (lo_do_transfer(lo, READ, page, offset, p->page, p->offset, size, IV)) {
		size = 0;
		printk(KERN_ERR "loop: transfer error block %ld\n",
		       page->index);
		desc->error = -EINVAL;
	}

	flush_dcache_page(p->page);

	desc->count = count - size;
	desc->written += size;
	p->offset += size;
	return size;
}

static int
do_lo_receive(struct loop_device *lo,
	      struct bio_vec *bvec, int bsize, loff_t pos)
{
	struct lo_read_data cookie;
	struct file *file;
	int retval;

	cookie.lo = lo;
	cookie.page = bvec->bv_page;
	cookie.offset = bvec->bv_offset;
	cookie.bsize = bsize;
	file = lo->lo_backing_file;
	retval = file->f_op->sendfile(file, &pos, bvec->bv_len,
			lo_read_actor, &cookie);
	return (retval < 0)? retval: 0;
}

static int
lo_receive(struct loop_device *lo, struct bio *bio, int bsize, loff_t pos)
{
	struct bio_vec *bvec;
	int i, ret = 0;

	bio_for_each_segment(bvec, bio, i) {
		ret = do_lo_receive(lo, bvec, bsize, pos);
		if (ret < 0)
			break;
		pos += bvec->bv_len;
	}
	return ret;
}

static int do_bio_filebacked(struct loop_device *lo, struct bio *bio)
{
	loff_t pos;
	int ret;

	pos = ((loff_t) bio->bi_sector << 9) + lo->lo_offset;
	if (bio_rw(bio) == WRITE)
		ret = lo_send(lo, bio, lo->lo_blocksize, pos);
	else
		ret = lo_receive(lo, bio, lo->lo_blocksize, pos);
	return ret;
}

/*
 * Add bio to back of pending list
 */
static void loop_add_bio(struct loop_device *lo, struct bio *bio)
{
	unsigned long flags;

	spin_lock_irqsave(&lo->lo_lock, flags);
	if (lo->lo_biotail) {
		lo->lo_biotail->bi_next = bio;
		lo->lo_biotail = bio;
	} else
		lo->lo_bio = lo->lo_biotail = bio;
	spin_unlock_irqrestore(&lo->lo_lock, flags);

	up(&lo->lo_bh_mutex);
}

/*
 * Grab first pending buffer
 */
static struct bio *loop_get_bio(struct loop_device *lo)
{
	struct bio *bio;

	spin_lock_irq(&lo->lo_lock);
	if ((bio = lo->lo_bio)) {
		if (bio == lo->lo_biotail)
			lo->lo_biotail = NULL;
		lo->lo_bio = bio->bi_next;
		bio->bi_next = NULL;
	}
	spin_unlock_irq(&lo->lo_lock);

	return bio;
}

static int loop_make_request(request_queue_t *q, struct bio *old_bio)
{
	struct loop_device *lo = q->queuedata;
	int rw = bio_rw(old_bio);

	if (!lo)
		goto out;

	spin_lock_irq(&lo->lo_lock);
	if (lo->lo_state != Lo_bound)
		goto inactive;
	atomic_inc(&lo->lo_pending);
	spin_unlock_irq(&lo->lo_lock);

	if (rw == WRITE) {
		if (lo->lo_flags & LO_FLAGS_READ_ONLY)
			goto err;
	} else if (rw == READA) {
		rw = READ;
	} else if (rw != READ) {
		printk(KERN_ERR "loop: unknown command (%x)\n", rw);
		goto err;
	}
	loop_add_bio(lo, old_bio);
	return 0;
err:
	if (atomic_dec_and_test(&lo->lo_pending))
		up(&lo->lo_bh_mutex);
out:
	bio_io_error(old_bio, old_bio->bi_size);
	return 0;
inactive:
	spin_unlock_irq(&lo->lo_lock);
	goto out;
}

/*
 * kick off io on the underlying address space
 */
static void loop_unplug(request_queue_t *q)
{
	struct loop_device *lo = q->queuedata;

	clear_bit(QUEUE_FLAG_PLUGGED, &q->queue_flags);
	blk_run_address_space(lo->lo_backing_file->f_mapping);
}

struct switch_request {
	struct file *file;
	struct completion wait;
};

static void do_loop_switch(struct loop_device *, struct switch_request *);

static inline void loop_handle_bio(struct loop_device *lo, struct bio *bio)
{
	int ret;

	if (unlikely(!bio->bi_bdev)) {
		do_loop_switch(lo, bio->bi_private);
		bio_put(bio);
	} else {
		ret = do_bio_filebacked(lo, bio);
		bio_endio(bio, bio->bi_size, ret);
	}
}

/*
 * worker thread that handles reads/writes to file backed loop devices,
 * to avoid blocking in our make_request_fn. it also does loop decrypting
 * on reads for block backed loop, as that is too heavy to do from
 * b_end_io context where irqs may be disabled.
 */
static int loop_thread(void *data)
{
	struct loop_device *lo = data;
	struct bio *bio;

	daemonize("loop%d", lo->lo_number);

	/*
	 * loop can be used in an encrypted device,
	 * hence, it mustn't be stopped at all
	 * because it could be indirectly used during suspension
	 */
	current->flags |= PF_NOFREEZE;

	set_user_nice(current, -20);

	lo->lo_state = Lo_bound;
	atomic_inc(&lo->lo_pending);

	/*
	 * up sem, we are running
	 */
	up(&lo->lo_sem);

	for (;;) {
		down_interruptible(&lo->lo_bh_mutex);
		/*
		 * could be upped because of tear-down, not because of
		 * pending work
		 */
		if (!atomic_read(&lo->lo_pending))
			break;

		bio = loop_get_bio(lo);
		if (!bio) {
			printk("loop: missing bio\n");
			continue;
		}
		loop_handle_bio(lo, bio);

		/*
		 * upped both for pending work and tear-down, lo_pending
		 * will hit zero then
		 */
		if (atomic_dec_and_test(&lo->lo_pending))
			break;
	}

	up(&lo->lo_sem);
	return 0;
}

/*
 * loop_switch performs the hard work of switching a backing store.
 * First it needs to flush existing IO, it does this by sending a magic
 * BIO down the pipe. The completion of this BIO does the actual switch.
 */
static int loop_switch(struct loop_device *lo, struct file *file)
{
	struct switch_request w;
	struct bio *bio = bio_alloc(GFP_KERNEL, 1);
	if (!bio)
		return -ENOMEM;
	init_completion(&w.wait);
	w.file = file;
	bio->bi_private = &w;
	bio->bi_bdev = NULL;
	loop_make_request(lo->lo_queue, bio);
	wait_for_completion(&w.wait);
	return 0;
}

/*
 * Do the actual switch; called from the BIO completion routine
 */
static void do_loop_switch(struct loop_device *lo, struct switch_request *p)
{
	struct file *file = p->file;
	struct file *old_file = lo->lo_backing_file;
	struct address_space *mapping = file->f_mapping;

	mapping_set_gfp_mask(old_file->f_mapping, lo->old_gfp_mask);
	lo->lo_backing_file = file;
	lo->lo_blocksize = mapping->host->i_blksize;
	lo->old_gfp_mask = mapping_gfp_mask(mapping);
	mapping_set_gfp_mask(mapping, lo->old_gfp_mask & ~(__GFP_IO|__GFP_FS));
	complete(&p->wait);
}


/*
 * loop_change_fd switched the backing store of a loopback device to
 * a new file. This is useful for operating system installers to free up
 * the original file and in High Availability environments to switch to
 * an alternative location for the content in case of server meltdown.
 * This can only work if the loop device is used read-only, and if the
 * new backing store is the same size and type as the old backing store.
 */
static int loop_change_fd(struct loop_device *lo, struct file *lo_file,
		       struct block_device *bdev, unsigned int arg)
{
	struct file	*file, *old_file;
	struct inode	*inode;
	int		error;

	error = -ENXIO;
	if (lo->lo_state != Lo_bound)
		goto out;

	/* the loop device has to be read-only */
	error = -EINVAL;
	if (lo->lo_flags != LO_FLAGS_READ_ONLY)
		goto out;

	error = -EBADF;
	file = fget(arg);
	if (!file)
		goto out;

	inode = file->f_mapping->host;
	old_file = lo->lo_backing_file;

	error = -EINVAL;

	if (!S_ISREG(inode->i_mode) && !S_ISBLK(inode->i_mode))
		goto out_putf;

	/* new backing store needs to support loop (eg sendfile) */
	if (!inode->i_fop->sendfile)
		goto out_putf;

	/* size of the new backing store needs to be the same */
	if (get_loop_size(lo, file) != get_loop_size(lo, old_file))
		goto out_putf;

	/* and ... switch */
	error = loop_switch(lo, file);
	if (error)
		goto out_putf;

	fput(old_file);
	return 0;

 out_putf:
	fput(file);
 out:
	return error;
}

static int loop_set_fd(struct loop_device *lo, struct file *lo_file,
		       struct block_device *bdev, unsigned int arg)
{
	struct file	*file;
	struct inode	*inode;
	struct address_space *mapping;
	unsigned lo_blocksize;
	int		lo_flags = 0;
	int		error;
	loff_t		size;

	/* This is safe, since we have a reference from open(). */
	__module_get(THIS_MODULE);

	error = -EBUSY;
	if (lo->lo_state != Lo_unbound)
		goto out;

	error = -EBADF;
	file = fget(arg);
	if (!file)
		goto out;

	mapping = file->f_mapping;
	inode = mapping->host;

	if (!(file->f_mode & FMODE_WRITE))
		lo_flags |= LO_FLAGS_READ_ONLY;

	error = -EINVAL;
	if (S_ISREG(inode->i_mode) || S_ISBLK(inode->i_mode)) {
		struct address_space_operations *aops = mapping->a_ops;
		/*
		 * If we can't read - sorry. If we only can't write - well,
		 * it's going to be read-only.
		 */
		if (!file->f_op->sendfile)
			goto out_putf;

		if (!aops->prepare_write || !aops->commit_write)
			lo_flags |= LO_FLAGS_READ_ONLY;

		lo_blocksize = inode->i_blksize;
		error = 0;
	} else {
		goto out_putf;
	}

	size = get_loop_size(lo, file);

	if ((loff_t)(sector_t)size != size) {
		error = -EFBIG;
		goto out_putf;
	}

	if (!(lo_file->f_mode & FMODE_WRITE))
		lo_flags |= LO_FLAGS_READ_ONLY;

	set_device_ro(bdev, (lo_flags & LO_FLAGS_READ_ONLY) != 0);

	lo->lo_blocksize = lo_blocksize;
	lo->lo_device = bdev;
	lo->lo_flags = lo_flags;
	lo->lo_backing_file = file;
	lo->transfer = NULL;
	lo->ioctl = NULL;
	lo->lo_sizelimit = 0;
	lo->old_gfp_mask = mapping_gfp_mask(mapping);
	mapping_set_gfp_mask(mapping, lo->old_gfp_mask & ~(__GFP_IO|__GFP_FS));

	lo->lo_bio = lo->lo_biotail = NULL;

	/*
	 * set queue make_request_fn, and add limits based on lower level
	 * device
	 */
	blk_queue_make_request(lo->lo_queue, loop_make_request);
	lo->lo_queue->queuedata = lo;
	lo->lo_queue->unplug_fn = loop_unplug;

	set_capacity(disks[lo->lo_number], size);
	bd_set_size(bdev, size << 9);

	set_blocksize(bdev, lo_blocksize);

	kernel_thread(loop_thread, lo, CLONE_KERNEL);
	down(&lo->lo_sem);
	return 0;

 out_putf:
	fput(file);
 out:
	/* This is safe: open() is still holding a reference. */
	module_put(THIS_MODULE);
	return error;
}

static int
loop_release_xfer(struct loop_device *lo)
{
	int err = 0;
	struct loop_func_table *xfer = lo->lo_encryption;

	if (xfer) {
		if (xfer->release)
			err = xfer->release(lo);
		lo->transfer = NULL;
		lo->lo_encryption = NULL;
		module_put(xfer->owner);
	}
	return err;
}

static int
loop_init_xfer(struct loop_device *lo, struct loop_func_table *xfer,
	       const struct loop_info64 *i)
{
	int err = 0;

	if (xfer) {
		struct module *owner = xfer->owner;

		if (!try_module_get(owner))
			return -EINVAL;
		if (xfer->init)
			err = xfer->init(lo, i);
		if (err)
			module_put(owner);
		else
			lo->lo_encryption = xfer;
	}
	return err;
}

static int loop_clr_fd(struct loop_device *lo, struct block_device *bdev)
{
	struct file *filp = lo->lo_backing_file;
	int gfp = lo->old_gfp_mask;

	if (lo->lo_state != Lo_bound)
		return -ENXIO;

	if (lo->lo_refcnt > 1)	/* we needed one fd for the ioctl */
		return -EBUSY;

	if (filp == NULL)
		return -EINVAL;

	spin_lock_irq(&lo->lo_lock);
	lo->lo_state = Lo_rundown;
	if (atomic_dec_and_test(&lo->lo_pending))
		up(&lo->lo_bh_mutex);
	spin_unlock_irq(&lo->lo_lock);

	down(&lo->lo_sem);

	lo->lo_backing_file = NULL;

	loop_release_xfer(lo);
	lo->transfer = NULL;
	lo->ioctl = NULL;
	lo->lo_device = NULL;
	lo->lo_encryption = NULL;
	lo->lo_offset = 0;
	lo->lo_sizelimit = 0;
	lo->lo_encrypt_key_size = 0;
	lo->lo_flags = 0;
	memset(lo->lo_encrypt_key, 0, LO_KEY_SIZE);
	memset(lo->lo_crypt_name, 0, LO_NAME_SIZE);
	memset(lo->lo_file_name, 0, LO_NAME_SIZE);
	invalidate_bdev(bdev, 0);
	set_capacity(disks[lo->lo_number], 0);
	bd_set_size(bdev, 0);
	mapping_set_gfp_mask(filp->f_mapping, gfp);
	lo->lo_state = Lo_unbound;
	fput(filp);
	/* This is safe: open() is still holding a reference. */
	module_put(THIS_MODULE);
	return 0;
}

static int
loop_set_status(struct loop_device *lo, const struct loop_info64 *info)
{
	int err;
	struct loop_func_table *xfer;

	if (lo->lo_encrypt_key_size && lo->lo_key_owner != current->uid &&
	    !capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (lo->lo_state != Lo_bound)
		return -ENXIO;
	if ((unsigned int) info->lo_encrypt_key_size > LO_KEY_SIZE)
		return -EINVAL;

	err = loop_release_xfer(lo);
	if (err)
		return err;

	if (info->lo_encrypt_type) {
		unsigned int type = info->lo_encrypt_type;

		if (type >= MAX_LO_CRYPT)
			return -EINVAL;
		xfer = xfer_funcs[type];
		if (xfer == NULL)
			return -EINVAL;
	} else
		xfer = NULL;

	err = loop_init_xfer(lo, xfer, info);
	if (err)
		return err;

	if (lo->lo_offset != info->lo_offset ||
	    lo->lo_sizelimit != info->lo_sizelimit) {
		lo->lo_offset = info->lo_offset;
		lo->lo_sizelimit = info->lo_sizelimit;
		if (figure_loop_size(lo))
			return -EFBIG;
	}

	memcpy(lo->lo_file_name, info->lo_file_name, LO_NAME_SIZE);
	memcpy(lo->lo_crypt_name, info->lo_crypt_name, LO_NAME_SIZE);
	lo->lo_file_name[LO_NAME_SIZE-1] = 0;
	lo->lo_crypt_name[LO_NAME_SIZE-1] = 0;

	if (!xfer)
		xfer = &none_funcs;
	lo->transfer = xfer->transfer;
	lo->ioctl = xfer->ioctl;

	lo->lo_encrypt_key_size = info->lo_encrypt_key_size;
	lo->lo_init[0] = info->lo_init[0];
	lo->lo_init[1] = info->lo_init[1];
	if (info->lo_encrypt_key_size) {
		memcpy(lo->lo_encrypt_key, info->lo_encrypt_key,
		       info->lo_encrypt_key_size);
		lo->lo_key_owner = current->uid;
	}	

	return 0;
}

static int
loop_get_status(struct loop_device *lo, struct loop_info64 *info)
{
	struct file *file = lo->lo_backing_file;
	struct kstat stat;
	int error;

	if (lo->lo_state != Lo_bound)
		return -ENXIO;
	error = vfs_getattr(file->f_vfsmnt, file->f_dentry, &stat);
	if (error)
		return error;
	memset(info, 0, sizeof(*info));
	info->lo_number = lo->lo_number;
	info->lo_device = huge_encode_dev(stat.dev);
	info->lo_inode = stat.ino;
	info->lo_rdevice = huge_encode_dev(lo->lo_device ? stat.rdev : stat.dev);
	info->lo_offset = lo->lo_offset;
	info->lo_sizelimit = lo->lo_sizelimit;
	info->lo_flags = lo->lo_flags;
	memcpy(info->lo_file_name, lo->lo_file_name, LO_NAME_SIZE);
	memcpy(info->lo_crypt_name, lo->lo_crypt_name, LO_NAME_SIZE);
	info->lo_encrypt_type =
		lo->lo_encryption ? lo->lo_encryption->number : 0;
	if (lo->lo_encrypt_key_size && capable(CAP_SYS_ADMIN)) {
		info->lo_encrypt_key_size = lo->lo_encrypt_key_size;
		memcpy(info->lo_encrypt_key, lo->lo_encrypt_key,
		       lo->lo_encrypt_key_size);
	}
	return 0;
}

static void
loop_info64_from_old(const struct loop_info *info, struct loop_info64 *info64)
{
	memset(info64, 0, sizeof(*info64));
	info64->lo_number = info->lo_number;
	info64->lo_device = info->lo_device;
	info64->lo_inode = info->lo_inode;
	info64->lo_rdevice = info->lo_rdevice;
	info64->lo_offset = info->lo_offset;
	info64->lo_sizelimit = 0;
	info64->lo_encrypt_type = info->lo_encrypt_type;
	info64->lo_encrypt_key_size = info->lo_encrypt_key_size;
	info64->lo_flags = info->lo_flags;
	info64->lo_init[0] = info->lo_init[0];
	info64->lo_init[1] = info->lo_init[1];
	if (info->lo_encrypt_type == LO_CRYPT_CRYPTOAPI)
		memcpy(info64->lo_crypt_name, info->lo_name, LO_NAME_SIZE);
	else
		memcpy(info64->lo_file_name, info->lo_name, LO_NAME_SIZE);
	memcpy(info64->lo_encrypt_key, info->lo_encrypt_key, LO_KEY_SIZE);
}

static int
loop_info64_to_old(const struct loop_info64 *info64, struct loop_info *info)
{
	memset(info, 0, sizeof(*info));
	info->lo_number = info64->lo_number;
	info->lo_device = info64->lo_device;
	info->lo_inode = info64->lo_inode;
	info->lo_rdevice = info64->lo_rdevice;
	info->lo_offset = info64->lo_offset;
	info->lo_encrypt_type = info64->lo_encrypt_type;
	info->lo_encrypt_key_size = info64->lo_encrypt_key_size;
	info->lo_flags = info64->lo_flags;
	info->lo_init[0] = info64->lo_init[0];
	info->lo_init[1] = info64->lo_init[1];
	if (info->lo_encrypt_type == LO_CRYPT_CRYPTOAPI)
		memcpy(info->lo_name, info64->lo_crypt_name, LO_NAME_SIZE);
	else
		memcpy(info->lo_name, info64->lo_file_name, LO_NAME_SIZE);
	memcpy(info->lo_encrypt_key, info64->lo_encrypt_key, LO_KEY_SIZE);

	/* error in case values were truncated */
	if (info->lo_device != info64->lo_device ||
	    info->lo_rdevice != info64->lo_rdevice ||
	    info->lo_inode != info64->lo_inode ||
	    info->lo_offset != info64->lo_offset)
		return -EOVERFLOW;

	return 0;
}

static int
loop_set_status_old(struct loop_device *lo, const struct loop_info __user *arg)
{
	struct loop_info info;
	struct loop_info64 info64;

	if (copy_from_user(&info, arg, sizeof (struct loop_info)))
		return -EFAULT;
	loop_info64_from_old(&info, &info64);
	return loop_set_status(lo, &info64);
}

static int
loop_set_status64(struct loop_device *lo, const struct loop_info64 __user *arg)
{
	struct loop_info64 info64;

	if (copy_from_user(&info64, arg, sizeof (struct loop_info64)))
		return -EFAULT;
	return loop_set_status(lo, &info64);
}

static int
loop_get_status_old(struct loop_device *lo, struct loop_info __user *arg) {
	struct loop_info info;
	struct loop_info64 info64;
	int err = 0;

	if (!arg)
		err = -EINVAL;
	if (!err)
		err = loop_get_status(lo, &info64);
	if (!err)
		err = loop_info64_to_old(&info64, &info);
	if (!err && copy_to_user(arg, &info, sizeof(info)))
		err = -EFAULT;

	return err;
}

static int
loop_get_status64(struct loop_device *lo, struct loop_info64 __user *arg) {
	struct loop_info64 info64;
	int err = 0;

	if (!arg)
		err = -EINVAL;
	if (!err)
		err = loop_get_status(lo, &info64);
	if (!err && copy_to_user(arg, &info64, sizeof(info64)))
		err = -EFAULT;

	return err;
}

static int lo_ioctl(struct inode * inode, struct file * file,
	unsigned int cmd, unsigned long arg)
{
	struct loop_device *lo = inode->i_bdev->bd_disk->private_data;
	int err;

	down(&lo->lo_ctl_mutex);
	switch (cmd) {
	case LOOP_SET_FD:
		err = loop_set_fd(lo, file, inode->i_bdev, arg);
		break;
	case LOOP_CHANGE_FD:
		err = loop_change_fd(lo, file, inode->i_bdev, arg);
		break;
	case LOOP_CLR_FD:
		err = loop_clr_fd(lo, inode->i_bdev);
		break;
	case LOOP_SET_STATUS:
		err = loop_set_status_old(lo, (struct loop_info __user *) arg);
		break;
	case LOOP_GET_STATUS:
		err = loop_get_status_old(lo, (struct loop_info __user *) arg);
		break;
	case LOOP_SET_STATUS64:
		err = loop_set_status64(lo, (struct loop_info64 __user *) arg);
		break;
	case LOOP_GET_STATUS64:
		err = loop_get_status64(lo, (struct loop_info64 __user *) arg);
		break;
	default:
		err = lo->ioctl ? lo->ioctl(lo, cmd, arg) : -EINVAL;
	}
	up(&lo->lo_ctl_mutex);
	return err;
}

static int lo_open(struct inode *inode, struct file *file)
{
	struct loop_device *lo = inode->i_bdev->bd_disk->private_data;

	down(&lo->lo_ctl_mutex);
	lo->lo_refcnt++;
	up(&lo->lo_ctl_mutex);

	return 0;
}

static int lo_release(struct inode *inode, struct file *file)
{
	struct loop_device *lo = inode->i_bdev->bd_disk->private_data;

	down(&lo->lo_ctl_mutex);
	--lo->lo_refcnt;
	up(&lo->lo_ctl_mutex);

	return 0;
}

static struct block_device_operations lo_fops = {
	.owner =	THIS_MODULE,
	.open =		lo_open,
	.release =	lo_release,
	.ioctl =	lo_ioctl,
};

/*
 * And now the modules code and kernel interface.
 */
MODULE_PARM(max_loop, "i");
MODULE_PARM_DESC(max_loop, "Maximum number of loop devices (1-256)");
MODULE_LICENSE("GPL");
MODULE_ALIAS_BLOCKDEV_MAJOR(LOOP_MAJOR);

int loop_register_transfer(struct loop_func_table *funcs)
{
	unsigned int n = funcs->number;

	if (n >= MAX_LO_CRYPT || xfer_funcs[n])
		return -EINVAL;
	xfer_funcs[n] = funcs;
	return 0;
}

int loop_unregister_transfer(int number)
{
	unsigned int n = number;
	struct loop_device *lo;
	struct loop_func_table *xfer;

	if (n == 0 || n >= MAX_LO_CRYPT || (xfer = xfer_funcs[n]) == NULL)
		return -EINVAL;

	xfer_funcs[n] = NULL;

	for (lo = &loop_dev[0]; lo < &loop_dev[max_loop]; lo++) {
		down(&lo->lo_ctl_mutex);

		if (lo->lo_encryption == xfer)
			loop_release_xfer(lo);

		up(&lo->lo_ctl_mutex);
	}

	return 0;
}

EXPORT_SYMBOL(loop_register_transfer);
EXPORT_SYMBOL(loop_unregister_transfer);

int __init loop_init(void)
{
	int	i;

	if (max_loop < 1 || max_loop > 256) {
		printk(KERN_WARNING "loop: invalid max_loop (must be between"
				    " 1 and 256), using default (8)\n");
		max_loop = 8;
	}

	if (register_blkdev(LOOP_MAJOR, "loop"))
		return -EIO;

	loop_dev = kmalloc(max_loop * sizeof(struct loop_device), GFP_KERNEL);
	if (!loop_dev)
		goto out_mem1;
	memset(loop_dev, 0, max_loop * sizeof(struct loop_device));

	disks = kmalloc(max_loop * sizeof(struct gendisk *), GFP_KERNEL);
	if (!disks)
		goto out_mem2;

	for (i = 0; i < max_loop; i++) {
		disks[i] = alloc_disk(1);
		if (!disks[i])
			goto out_mem3;
	}

	devfs_mk_dir("loop");

	for (i = 0; i < max_loop; i++) {
		struct loop_device *lo = &loop_dev[i];
		struct gendisk *disk = disks[i];

		memset(lo, 0, sizeof(*lo));
		lo->lo_queue = blk_alloc_queue(GFP_KERNEL);
		if (!lo->lo_queue)
			goto out_mem4;
		init_MUTEX(&lo->lo_ctl_mutex);
		init_MUTEX_LOCKED(&lo->lo_sem);
		init_MUTEX_LOCKED(&lo->lo_bh_mutex);
		lo->lo_number = i;
		spin_lock_init(&lo->lo_lock);
		disk->major = LOOP_MAJOR;
		disk->first_minor = i;
		disk->fops = &lo_fops;
		sprintf(disk->disk_name, "loop%d", i);
		sprintf(disk->devfs_name, "loop/%d", i);
		disk->private_data = lo;
		disk->queue = lo->lo_queue;
	}

	/* We cannot fail after we call this, so another loop!*/
	for (i = 0; i < max_loop; i++)
		add_disk(disks[i]);
	printk(KERN_INFO "loop: loaded (max %d devices)\n", max_loop);
	return 0;

out_mem4:
	while (i--)
		blk_put_queue(loop_dev[i].lo_queue);
	devfs_remove("loop");
	i = max_loop;
out_mem3:
	while (i--)
		put_disk(disks[i]);
	kfree(disks);
out_mem2:
	kfree(loop_dev);
out_mem1:
	unregister_blkdev(LOOP_MAJOR, "loop");
	printk(KERN_ERR "loop: ran out of memory\n");
	return -ENOMEM;
}

void loop_exit(void)
{
	int i;

	for (i = 0; i < max_loop; i++) {
		del_gendisk(disks[i]);
		blk_put_queue(loop_dev[i].lo_queue);
		put_disk(disks[i]);
	}
	devfs_remove("loop");
	if (unregister_blkdev(LOOP_MAJOR, "loop"))
		printk(KERN_WARNING "loop: cannot unregister blkdev\n");

	kfree(disks);
	kfree(loop_dev);
}

module_init(loop_init);
module_exit(loop_exit);

#ifndef MODULE
static int __init max_loop_setup(char *str)
{
	max_loop = simple_strtol(str, NULL, 0);
	return 1;
}

__setup("max_loop=", max_loop_setup);
#endif
