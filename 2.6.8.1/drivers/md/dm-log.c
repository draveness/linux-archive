/*
 * Copyright (C) 2003 Sistina Software
 *
 * This file is released under the LGPL.
 */

#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/vmalloc.h>

#include "dm-log.h"
#include "dm-io.h"

static LIST_HEAD(_log_types);
static spinlock_t _lock = SPIN_LOCK_UNLOCKED;

int dm_register_dirty_log_type(struct dirty_log_type *type)
{
	if (!try_module_get(type->module))
		return -EINVAL;

	spin_lock(&_lock);
	type->use_count = 0;
	list_add(&type->list, &_log_types);
	spin_unlock(&_lock);

	return 0;
}

int dm_unregister_dirty_log_type(struct dirty_log_type *type)
{
	spin_lock(&_lock);

	if (type->use_count)
		DMWARN("Attempt to unregister a log type that is still in use");
	else {
		list_del(&type->list);
		module_put(type->module);
	}

	spin_unlock(&_lock);

	return 0;
}

static struct dirty_log_type *get_type(const char *type_name)
{
	struct dirty_log_type *type;

	spin_lock(&_lock);
	list_for_each_entry (type, &_log_types, list)
		if (!strcmp(type_name, type->name)) {
			type->use_count++;
			spin_unlock(&_lock);
			return type;
		}

	spin_unlock(&_lock);
	return NULL;
}

static void put_type(struct dirty_log_type *type)
{
	spin_lock(&_lock);
	type->use_count--;
	spin_unlock(&_lock);
}

struct dirty_log *dm_create_dirty_log(const char *type_name, struct dm_target *ti,
				      unsigned int argc, char **argv)
{
	struct dirty_log_type *type;
	struct dirty_log *log;

	log = kmalloc(sizeof(*log), GFP_KERNEL);
	if (!log)
		return NULL;

	type = get_type(type_name);
	if (!type) {
		kfree(log);
		return NULL;
	}

	log->type = type;
	if (type->ctr(log, ti, argc, argv)) {
		kfree(log);
		put_type(type);
		return NULL;
	}

	return log;
}

void dm_destroy_dirty_log(struct dirty_log *log)
{
	log->type->dtr(log);
	put_type(log->type);
	kfree(log);
}

/*-----------------------------------------------------------------
 * Persistent and core logs share a lot of their implementation.
 * FIXME: need a reload method to be called from a resume
 *---------------------------------------------------------------*/
/*
 * Magic for persistent mirrors: "MiRr"
 */
#define MIRROR_MAGIC 0x4D695272

/*
 * The on-disk version of the metadata.
 */
#define MIRROR_DISK_VERSION 1
#define LOG_OFFSET 2

struct log_header {
	uint32_t magic;

	/*
	 * Simple, incrementing version. no backward
	 * compatibility.
	 */
	uint32_t version;
	sector_t nr_regions;
};

struct log_c {
	struct dm_target *ti;
	int touched;
	sector_t region_size;
	unsigned int region_count;
	region_t sync_count;

	unsigned bitset_uint32_count;
	uint32_t *clean_bits;
	uint32_t *sync_bits;
	uint32_t *recovering_bits;	/* FIXME: this seems excessive */

	int sync_search;

	/*
	 * Disk log fields
	 */
	struct dm_dev *log_dev;
	struct log_header header;

	struct io_region header_location;
	struct log_header *disk_header;

	struct io_region bits_location;
	uint32_t *disk_bits;
};

/*
 * The touched member needs to be updated every time we access
 * one of the bitsets.
 */
static  inline int log_test_bit(uint32_t *bs, unsigned bit)
{
	return test_bit(bit, (unsigned long *) bs) ? 1 : 0;
}

static inline void log_set_bit(struct log_c *l,
			       uint32_t *bs, unsigned bit)
{
	set_bit(bit, (unsigned long *) bs);
	l->touched = 1;
}

static inline void log_clear_bit(struct log_c *l,
				 uint32_t *bs, unsigned bit)
{
	clear_bit(bit, (unsigned long *) bs);
	l->touched = 1;
}

/*----------------------------------------------------------------
 * Header IO
 *--------------------------------------------------------------*/
static void header_to_disk(struct log_header *core, struct log_header *disk)
{
	disk->magic = cpu_to_le32(core->magic);
	disk->version = cpu_to_le32(core->version);
	disk->nr_regions = cpu_to_le64(core->nr_regions);
}

static void header_from_disk(struct log_header *core, struct log_header *disk)
{
	core->magic = le32_to_cpu(disk->magic);
	core->version = le32_to_cpu(disk->version);
	core->nr_regions = le64_to_cpu(disk->nr_regions);
}

static int read_header(struct log_c *log)
{
	int r;
	unsigned long ebits;

	r = dm_io_sync_vm(1, &log->header_location, READ,
			  log->disk_header, &ebits);
	if (r)
		return r;

	header_from_disk(&log->header, log->disk_header);

	if (log->header.magic != MIRROR_MAGIC) {
		log->header.magic = MIRROR_MAGIC;
		log->header.version = MIRROR_DISK_VERSION;
		log->header.nr_regions = 0;
	}

	if (log->header.version != MIRROR_DISK_VERSION) {
		DMWARN("incompatible disk log version");
		return -EINVAL;
	}

	return 0;
}

static inline int write_header(struct log_c *log)
{
	unsigned long ebits;

	header_to_disk(&log->header, log->disk_header);
	return dm_io_sync_vm(1, &log->header_location, WRITE,
			     log->disk_header, &ebits);
}

/*----------------------------------------------------------------
 * Bits IO
 *--------------------------------------------------------------*/
static inline void bits_to_core(uint32_t *core, uint32_t *disk, unsigned count)
{
	unsigned i;

	for (i = 0; i < count; i++)
		core[i] = le32_to_cpu(disk[i]);
}

static inline void bits_to_disk(uint32_t *core, uint32_t *disk, unsigned count)
{
	unsigned i;

	/* copy across the clean/dirty bitset */
	for (i = 0; i < count; i++)
		disk[i] = cpu_to_le32(core[i]);
}

static int read_bits(struct log_c *log)
{
	int r;
	unsigned long ebits;

	r = dm_io_sync_vm(1, &log->bits_location, READ,
			  log->disk_bits, &ebits);
	if (r)
		return r;

	bits_to_core(log->clean_bits, log->disk_bits,
		     log->bitset_uint32_count);
	return 0;
}

static int write_bits(struct log_c *log)
{
	unsigned long ebits;
	bits_to_disk(log->clean_bits, log->disk_bits,
		     log->bitset_uint32_count);
	return dm_io_sync_vm(1, &log->bits_location, WRITE,
			     log->disk_bits, &ebits);
}

/*----------------------------------------------------------------
 * constructor/destructor
 *--------------------------------------------------------------*/
#define BYTE_SHIFT 3
static int core_ctr(struct dirty_log *log, struct dm_target *ti,
		    unsigned int argc, char **argv)
{
	struct log_c *lc;
	sector_t region_size;
	unsigned int region_count;
	size_t bitset_size;

	if (argc != 1) {
		DMWARN("wrong number of arguments to log_c");
		return -EINVAL;
	}

	if (sscanf(argv[0], SECTOR_FORMAT, &region_size) != 1) {
		DMWARN("invalid region size string");
		return -EINVAL;
	}

	region_count = dm_div_up(ti->len, region_size);

	lc = kmalloc(sizeof(*lc), GFP_KERNEL);
	if (!lc) {
		DMWARN("couldn't allocate core log");
		return -ENOMEM;
	}

	lc->ti = ti;
	lc->touched = 0;
	lc->region_size = region_size;
	lc->region_count = region_count;

	/*
	 * Work out how many words we need to hold the bitset.
	 */
	bitset_size = dm_round_up(region_count,
				  sizeof(*lc->clean_bits) << BYTE_SHIFT);
	bitset_size >>= BYTE_SHIFT;

	lc->bitset_uint32_count = bitset_size / 4;
	lc->clean_bits = vmalloc(bitset_size);
	if (!lc->clean_bits) {
		DMWARN("couldn't allocate clean bitset");
		kfree(lc);
		return -ENOMEM;
	}
	memset(lc->clean_bits, -1, bitset_size);

	lc->sync_bits = vmalloc(bitset_size);
	if (!lc->sync_bits) {
		DMWARN("couldn't allocate sync bitset");
		vfree(lc->clean_bits);
		kfree(lc);
		return -ENOMEM;
	}
	memset(lc->sync_bits, 0, bitset_size);
        lc->sync_count = 0;

	lc->recovering_bits = vmalloc(bitset_size);
	if (!lc->recovering_bits) {
		DMWARN("couldn't allocate sync bitset");
		vfree(lc->sync_bits);
		vfree(lc->clean_bits);
		kfree(lc);
		return -ENOMEM;
	}
	memset(lc->recovering_bits, 0, bitset_size);
	lc->sync_search = 0;
	log->context = lc;
	return 0;
}

static void core_dtr(struct dirty_log *log)
{
	struct log_c *lc = (struct log_c *) log->context;
	vfree(lc->clean_bits);
	vfree(lc->sync_bits);
	vfree(lc->recovering_bits);
	kfree(lc);
}

static int disk_ctr(struct dirty_log *log, struct dm_target *ti,
		    unsigned int argc, char **argv)
{
	int r;
	size_t size;
	struct log_c *lc;
	struct dm_dev *dev;

	if (argc != 2) {
		DMWARN("wrong number of arguments to log_d");
		return -EINVAL;
	}

	r = dm_get_device(ti, argv[0], 0, 0 /* FIXME */,
			  FMODE_READ | FMODE_WRITE, &dev);
	if (r)
		return r;

	r = core_ctr(log, ti, argc - 1, argv + 1);
	if (r) {
		dm_put_device(ti, dev);
		return r;
	}

	lc = (struct log_c *) log->context;
	lc->log_dev = dev;

	/* setup the disk header fields */
	lc->header_location.bdev = lc->log_dev->bdev;
	lc->header_location.sector = 0;
	lc->header_location.count = 1;

	/*
	 * We can't read less than this amount, even though we'll
	 * not be using most of this space.
	 */
	lc->disk_header = vmalloc(1 << SECTOR_SHIFT);
	if (!lc->disk_header)
		goto bad;

	/* setup the disk bitset fields */
	lc->bits_location.bdev = lc->log_dev->bdev;
	lc->bits_location.sector = LOG_OFFSET;

	size = dm_round_up(lc->bitset_uint32_count * sizeof(uint32_t),
			   1 << SECTOR_SHIFT);
	lc->bits_location.count = size >> SECTOR_SHIFT;
	lc->disk_bits = vmalloc(size);
	if (!lc->disk_bits) {
		vfree(lc->disk_header);
		goto bad;
	}
	return 0;

 bad:
	dm_put_device(ti, lc->log_dev);
	core_dtr(log);
	return -ENOMEM;
}

static void disk_dtr(struct dirty_log *log)
{
	struct log_c *lc = (struct log_c *) log->context;
	dm_put_device(lc->ti, lc->log_dev);
	vfree(lc->disk_header);
	vfree(lc->disk_bits);
	core_dtr(log);
}

static int count_bits32(uint32_t *addr, unsigned size)
{
	int count = 0, i;

	for (i = 0; i < size; i++) {
		count += hweight32(*(addr+i));
	}
	return count;
}

static int disk_resume(struct dirty_log *log)
{
	int r;
	unsigned i;
	struct log_c *lc = (struct log_c *) log->context;
	size_t size = lc->bitset_uint32_count * sizeof(uint32_t);

	/* read the disk header */
	r = read_header(lc);
	if (r)
		return r;

	/* read the bits */
	r = read_bits(lc);
	if (r)
		return r;

	/* zero any new bits if the mirror has grown */
	for (i = lc->header.nr_regions; i < lc->region_count; i++)
		/* FIXME: amazingly inefficient */
		log_clear_bit(lc, lc->clean_bits, i);

	/* copy clean across to sync */
	memcpy(lc->sync_bits, lc->clean_bits, size);
	lc->sync_count = count_bits32(lc->clean_bits, lc->bitset_uint32_count);

	/* write the bits */
	r = write_bits(lc);
	if (r)
		return r;

	/* set the correct number of regions in the header */
	lc->header.nr_regions = lc->region_count;

	/* write the new header */
	return write_header(lc);
}

static sector_t core_get_region_size(struct dirty_log *log)
{
	struct log_c *lc = (struct log_c *) log->context;
	return lc->region_size;
}

static int core_is_clean(struct dirty_log *log, region_t region)
{
	struct log_c *lc = (struct log_c *) log->context;
	return log_test_bit(lc->clean_bits, region);
}

static int core_in_sync(struct dirty_log *log, region_t region, int block)
{
	struct log_c *lc = (struct log_c *) log->context;
	return log_test_bit(lc->sync_bits, region);
}

static int core_flush(struct dirty_log *log)
{
	/* no op */
	return 0;
}

static int disk_flush(struct dirty_log *log)
{
	int r;
	struct log_c *lc = (struct log_c *) log->context;

	/* only write if the log has changed */
	if (!lc->touched)
		return 0;

	r = write_bits(lc);
	if (!r)
		lc->touched = 0;

	return r;
}

static void core_mark_region(struct dirty_log *log, region_t region)
{
	struct log_c *lc = (struct log_c *) log->context;
	log_clear_bit(lc, lc->clean_bits, region);
}

static void core_clear_region(struct dirty_log *log, region_t region)
{
	struct log_c *lc = (struct log_c *) log->context;
	log_set_bit(lc, lc->clean_bits, region);
}

static int core_get_resync_work(struct dirty_log *log, region_t *region)
{
	struct log_c *lc = (struct log_c *) log->context;

	if (lc->sync_search >= lc->region_count)
		return 0;

	do {
		*region = find_next_zero_bit((unsigned long *) lc->sync_bits,
					     lc->region_count,
					     lc->sync_search);
		lc->sync_search = *region + 1;

		if (*region == lc->region_count)
			return 0;

	} while (log_test_bit(lc->recovering_bits, *region));

	log_set_bit(lc, lc->recovering_bits, *region);
	return 1;
}

static void core_complete_resync_work(struct dirty_log *log, region_t region,
				      int success)
{
	struct log_c *lc = (struct log_c *) log->context;

	log_clear_bit(lc, lc->recovering_bits, region);
	if (success) {
		log_set_bit(lc, lc->sync_bits, region);
                lc->sync_count++;
        }
}

static region_t core_get_sync_count(struct dirty_log *log)
{
        struct log_c *lc = (struct log_c *) log->context;

        return lc->sync_count;
}

static struct dirty_log_type _core_type = {
	.name = "core",
	.module = THIS_MODULE,
	.ctr = core_ctr,
	.dtr = core_dtr,
	.get_region_size = core_get_region_size,
	.is_clean = core_is_clean,
	.in_sync = core_in_sync,
	.flush = core_flush,
	.mark_region = core_mark_region,
	.clear_region = core_clear_region,
	.get_resync_work = core_get_resync_work,
	.complete_resync_work = core_complete_resync_work,
        .get_sync_count = core_get_sync_count
};

static struct dirty_log_type _disk_type = {
	.name = "disk",
	.module = THIS_MODULE,
	.ctr = disk_ctr,
	.dtr = disk_dtr,
	.suspend = disk_flush,
	.resume = disk_resume,
	.get_region_size = core_get_region_size,
	.is_clean = core_is_clean,
	.in_sync = core_in_sync,
	.flush = disk_flush,
	.mark_region = core_mark_region,
	.clear_region = core_clear_region,
	.get_resync_work = core_get_resync_work,
	.complete_resync_work = core_complete_resync_work,
        .get_sync_count = core_get_sync_count
};

int __init dm_dirty_log_init(void)
{
	int r;

	r = dm_register_dirty_log_type(&_core_type);
	if (r)
		DMWARN("couldn't register core log");

	r = dm_register_dirty_log_type(&_disk_type);
	if (r) {
		DMWARN("couldn't register disk type");
		dm_unregister_dirty_log_type(&_core_type);
	}

	return r;
}

void dm_dirty_log_exit(void)
{
	dm_unregister_dirty_log_type(&_disk_type);
	dm_unregister_dirty_log_type(&_core_type);
}

EXPORT_SYMBOL(dm_register_dirty_log_type);
EXPORT_SYMBOL(dm_unregister_dirty_log_type);
EXPORT_SYMBOL(dm_create_dirty_log);
EXPORT_SYMBOL(dm_destroy_dirty_log);
