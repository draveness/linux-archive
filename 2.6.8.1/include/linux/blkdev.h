#ifndef _LINUX_BLKDEV_H
#define _LINUX_BLKDEV_H

#include <linux/config.h>
#include <linux/major.h>
#include <linux/genhd.h>
#include <linux/list.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/pagemap.h>
#include <linux/backing-dev.h>
#include <linux/wait.h>
#include <linux/mempool.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/stringify.h>

#include <asm/scatterlist.h>

struct request_queue;
typedef struct request_queue request_queue_t;
struct elevator_s;
typedef struct elevator_s elevator_t;
struct request_pm_state;

#define BLKDEV_MIN_RQ	4
#define BLKDEV_MAX_RQ	128	/* Default maximum */

/*
 * This is the per-process anticipatory I/O scheduler state.
 */
struct as_io_context {
	spinlock_t lock;

	void (*dtor)(struct as_io_context *aic); /* destructor */
	void (*exit)(struct as_io_context *aic); /* called on task exit */

	unsigned long state;
	atomic_t nr_queued; /* queued reads & sync writes */
	atomic_t nr_dispatched; /* number of requests gone to the drivers */

	/* IO History tracking */
	/* Thinktime */
	unsigned long last_end_request;
	unsigned long ttime_total;
	unsigned long ttime_samples;
	unsigned long ttime_mean;
	/* Layout pattern */
	unsigned int seek_samples;
	sector_t last_request_pos;
	u64 seek_total;
	sector_t seek_mean;
};

/*
 * This is the per-process I/O subsystem state.  It is refcounted and
 * kmalloc'ed. Currently all fields are modified in process io context
 * (apart from the atomic refcount), so require no locking.
 */
struct io_context {
	atomic_t refcount;
	pid_t pid;

	/*
	 * For request batching
	 */
	unsigned long last_waited; /* Time last woken after wait for request */
	int nr_batch_requests;     /* Number of requests left in the batch */

	struct as_io_context *aic;
};

void put_io_context(struct io_context *ioc);
void exit_io_context(void);
struct io_context *get_io_context(int gfp_flags);
void copy_io_context(struct io_context **pdst, struct io_context **psrc);
void swap_io_context(struct io_context **ioc1, struct io_context **ioc2);

struct request_list {
	int count[2];
	mempool_t *rq_pool;
	wait_queue_head_t wait[2];
};

#define BLK_MAX_CDB	16

/*
 * try to put the fields that are referenced together in the same cacheline
 */
struct request {
	struct list_head queuelist; /* looking for ->queue? you must _not_
				     * access it directly, use
				     * blkdev_dequeue_request! */
	unsigned long flags;		/* see REQ_ bits below */

	/* Maintain bio traversal state for part by part I/O submission.
	 * hard_* are block layer internals, no driver should touch them!
	 */

	sector_t sector;		/* next sector to submit */
	unsigned long nr_sectors;	/* no. of sectors left to submit */
	/* no. of sectors left to submit in the current segment */
	unsigned int current_nr_sectors;

	sector_t hard_sector;		/* next sector to complete */
	unsigned long hard_nr_sectors;	/* no. of sectors left to complete */
	/* no. of sectors left to complete in the current segment */
	unsigned int hard_cur_sectors;

	/* no. of segments left to submit in the current bio */
	unsigned short nr_cbio_segments;
	/* no. of sectors left to submit in the current bio */
	unsigned long nr_cbio_sectors;

	struct bio *cbio;		/* next bio to submit */
	struct bio *bio;		/* next unfinished bio to complete */
	struct bio *biotail;

	void *elevator_private;

	int rq_status;	/* should split this into a few status bits */
	struct gendisk *rq_disk;
	int errors;
	unsigned long start_time;

	/* Number of scatter-gather DMA addr+len pairs after
	 * physical address coalescing is performed.
	 */
	unsigned short nr_phys_segments;

	/* Number of scatter-gather addr+len pairs after
	 * physical and DMA remapping hardware coalescing is performed.
	 * This is the number of scatter-gather entries the driver
	 * will actually have to deal with after DMA mapping is done.
	 */
	unsigned short nr_hw_segments;

	int tag;
	char *buffer;

	int ref_count;
	request_queue_t *q;
	struct request_list *rl;

	struct completion *waiting;
	void *special;

	/*
	 * when request is used as a packet command carrier
	 */
	unsigned int cmd_len;
	unsigned char cmd[BLK_MAX_CDB];

	unsigned int data_len;
	void *data;

	unsigned int sense_len;
	void *sense;

	unsigned int timeout;

	/*
	 * For Power Management requests
	 */
	struct request_pm_state *pm;
};

/*
 * first three bits match BIO_RW* bits, important
 */
enum rq_flag_bits {
	__REQ_RW,		/* not set, read. set, write */
	__REQ_FAILFAST,		/* no low level driver retries */
	__REQ_SOFTBARRIER,	/* may not be passed by ioscheduler */
	__REQ_HARDBARRIER,	/* may not be passed by drive either */
	__REQ_CMD,		/* is a regular fs rw request */
	__REQ_NOMERGE,		/* don't touch this for merging */
	__REQ_STARTED,		/* drive already may have started this one */
	__REQ_DONTPREP,		/* don't call prep for this one */
	__REQ_QUEUED,		/* uses queueing */
	/*
	 * for ATA/ATAPI devices
	 */
	__REQ_PC,		/* packet command (special) */
	__REQ_BLOCK_PC,		/* queued down pc from block layer */
	__REQ_SENSE,		/* sense retrival */

	__REQ_FAILED,		/* set if the request failed */
	__REQ_QUIET,		/* don't worry about errors */
	__REQ_SPECIAL,		/* driver suplied command */
	__REQ_DRIVE_CMD,
	__REQ_DRIVE_TASK,
	__REQ_DRIVE_TASKFILE,
	__REQ_PREEMPT,		/* set for "ide_preempt" requests */
	__REQ_PM_SUSPEND,	/* suspend request */
	__REQ_PM_RESUME,	/* resume request */
	__REQ_PM_SHUTDOWN,	/* shutdown request */
	__REQ_NR_BITS,		/* stops here */
};

#define REQ_RW		(1 << __REQ_RW)
#define REQ_FAILFAST	(1 << __REQ_FAILFAST)
#define REQ_SOFTBARRIER	(1 << __REQ_SOFTBARRIER)
#define REQ_HARDBARRIER	(1 << __REQ_HARDBARRIER)
#define REQ_CMD		(1 << __REQ_CMD)
#define REQ_NOMERGE	(1 << __REQ_NOMERGE)
#define REQ_STARTED	(1 << __REQ_STARTED)
#define REQ_DONTPREP	(1 << __REQ_DONTPREP)
#define REQ_QUEUED	(1 << __REQ_QUEUED)
#define REQ_PC		(1 << __REQ_PC)
#define REQ_BLOCK_PC	(1 << __REQ_BLOCK_PC)
#define REQ_SENSE	(1 << __REQ_SENSE)
#define REQ_FAILED	(1 << __REQ_FAILED)
#define REQ_QUIET	(1 << __REQ_QUIET)
#define REQ_SPECIAL	(1 << __REQ_SPECIAL)
#define REQ_DRIVE_CMD	(1 << __REQ_DRIVE_CMD)
#define REQ_DRIVE_TASK	(1 << __REQ_DRIVE_TASK)
#define REQ_DRIVE_TASKFILE	(1 << __REQ_DRIVE_TASKFILE)
#define REQ_PREEMPT	(1 << __REQ_PREEMPT)
#define REQ_PM_SUSPEND	(1 << __REQ_PM_SUSPEND)
#define REQ_PM_RESUME	(1 << __REQ_PM_RESUME)
#define REQ_PM_SHUTDOWN	(1 << __REQ_PM_SHUTDOWN)

/*
 * State information carried for REQ_PM_SUSPEND and REQ_PM_RESUME
 * requests. Some step values could eventually be made generic.
 */
struct request_pm_state
{
	/* PM state machine step value, currently driver specific */
	int	pm_step;
	/* requested PM state value (S1, S2, S3, S4, ...) */
	u32	pm_state;
	void*	data;		/* for driver use */
};

#include <linux/elevator.h>

typedef int (merge_request_fn) (request_queue_t *, struct request *,
				struct bio *);
typedef int (merge_requests_fn) (request_queue_t *, struct request *,
				 struct request *);
typedef void (request_fn_proc) (request_queue_t *q);
typedef int (make_request_fn) (request_queue_t *q, struct bio *bio);
typedef int (prep_rq_fn) (request_queue_t *, struct request *);
typedef void (unplug_fn) (request_queue_t *);

struct bio_vec;
typedef int (merge_bvec_fn) (request_queue_t *, struct bio *, struct bio_vec *);
typedef void (activity_fn) (void *data, int rw);

enum blk_queue_state {
	Queue_down,
	Queue_up,
};

#define BLK_TAGS_PER_LONG	(sizeof(unsigned long) * 8)
#define BLK_TAGS_MASK		(BLK_TAGS_PER_LONG - 1)

struct blk_queue_tag {
	struct request **tag_index;	/* map of busy tags */
	unsigned long *tag_map;		/* bit map of free/busy tags */
	struct list_head busy_list;	/* fifo list of busy tags */
	int busy;			/* current depth */
	int max_depth;			/* what we will send to device */
	int real_max_depth;		/* what the array can hold */
	atomic_t refcnt;		/* map can be shared */
};

struct request_queue
{
	/*
	 * Together with queue_head for cacheline sharing
	 */
	struct list_head	queue_head;
	struct request		*last_merge;
	elevator_t		elevator;

	/*
	 * the queue request freelist, one for reads and one for writes
	 */
	struct request_list	rq;

	request_fn_proc		*request_fn;
	merge_request_fn	*back_merge_fn;
	merge_request_fn	*front_merge_fn;
	merge_requests_fn	*merge_requests_fn;
	make_request_fn		*make_request_fn;
	prep_rq_fn		*prep_rq_fn;
	unplug_fn		*unplug_fn;
	merge_bvec_fn		*merge_bvec_fn;
	activity_fn		*activity_fn;

	/*
	 * Auto-unplugging state
	 */
	struct timer_list	unplug_timer;
	int			unplug_thresh;	/* After this many requests */
	unsigned long		unplug_delay;	/* After this many jiffies */
	struct work_struct	unplug_work;

	struct backing_dev_info	backing_dev_info;

	/*
	 * The queue owner gets to use this for whatever they like.
	 * ll_rw_blk doesn't touch it.
	 */
	void			*queuedata;

	void			*activity_data;

	/*
	 * queue needs bounce pages for pages above this limit
	 */
	unsigned long		bounce_pfn;
	int			bounce_gfp;

	/*
	 * various queue flags, see QUEUE_* below
	 */
	unsigned long		queue_flags;

	/*
	 * protects queue structures from reentrancy
	 */
	spinlock_t		*queue_lock;

	/*
	 * queue kobject
	 */
	struct kobject kobj;

	/*
	 * queue settings
	 */
	unsigned long		nr_requests;	/* Max # of requests */
	unsigned int		nr_congestion_on;
	unsigned int		nr_congestion_off;

	unsigned short		max_sectors;
	unsigned short		max_phys_segments;
	unsigned short		max_hw_segments;
	unsigned short		hardsect_size;
	unsigned int		max_segment_size;

	unsigned long		seg_boundary_mask;
	unsigned int		dma_alignment;

	struct blk_queue_tag	*queue_tags;

	atomic_t		refcnt;

	unsigned int		in_flight;

	/*
	 * sg stuff
	 */
	unsigned int		sg_timeout;
	unsigned int		sg_reserved_size;
};

#define RQ_INACTIVE		(-1)
#define RQ_ACTIVE		1
#define RQ_SCSI_BUSY		0xffff
#define RQ_SCSI_DONE		0xfffe
#define RQ_SCSI_DISCONNECTING	0xffe0

#define QUEUE_FLAG_CLUSTER	0	/* cluster several segments into 1 */
#define QUEUE_FLAG_QUEUED	1	/* uses generic tag queueing */
#define QUEUE_FLAG_STOPPED	2	/* queue is stopped */
#define	QUEUE_FLAG_READFULL	3	/* write queue has been filled */
#define QUEUE_FLAG_WRITEFULL	4	/* read queue has been filled */
#define QUEUE_FLAG_DEAD		5	/* queue being torn down */
#define QUEUE_FLAG_REENTER	6	/* Re-entrancy avoidance */
#define QUEUE_FLAG_PLUGGED	7	/* queue is plugged */

#define blk_queue_plugged(q)	test_bit(QUEUE_FLAG_PLUGGED, &(q)->queue_flags)
#define blk_queue_tagged(q)	test_bit(QUEUE_FLAG_QUEUED, &(q)->queue_flags)
#define blk_queue_stopped(q)	test_bit(QUEUE_FLAG_STOPPED, &(q)->queue_flags)

#define blk_fs_request(rq)	((rq)->flags & REQ_CMD)
#define blk_pc_request(rq)	((rq)->flags & REQ_BLOCK_PC)
#define blk_noretry_request(rq)	((rq)->flags & REQ_FAILFAST)
#define blk_rq_started(rq)	((rq)->flags & REQ_STARTED)

#define blk_account_rq(rq)	(blk_rq_started(rq) && blk_fs_request(rq))

#define blk_pm_suspend_request(rq)	((rq)->flags & REQ_PM_SUSPEND)
#define blk_pm_resume_request(rq)	((rq)->flags & REQ_PM_RESUME)
#define blk_pm_request(rq)	\
	((rq)->flags & (REQ_PM_SUSPEND | REQ_PM_RESUME))

#define list_entry_rq(ptr)	list_entry((ptr), struct request, queuelist)

#define rq_data_dir(rq)		((rq)->flags & 1)

static inline int blk_queue_full(struct request_queue *q, int rw)
{
	if (rw == READ)
		return test_bit(QUEUE_FLAG_READFULL, &q->queue_flags);
	return test_bit(QUEUE_FLAG_WRITEFULL, &q->queue_flags);
}

static inline void blk_set_queue_full(struct request_queue *q, int rw)
{
	if (rw == READ)
		set_bit(QUEUE_FLAG_READFULL, &q->queue_flags);
	else
		set_bit(QUEUE_FLAG_WRITEFULL, &q->queue_flags);
}

static inline void blk_clear_queue_full(struct request_queue *q, int rw)
{
	if (rw == READ)
		clear_bit(QUEUE_FLAG_READFULL, &q->queue_flags);
	else
		clear_bit(QUEUE_FLAG_WRITEFULL, &q->queue_flags);
}


/*
 * mergeable request must not have _NOMERGE or _BARRIER bit set, nor may
 * it already be started by driver.
 */
#define RQ_NOMERGE_FLAGS	\
	(REQ_NOMERGE | REQ_STARTED | REQ_HARDBARRIER | REQ_SOFTBARRIER)
#define rq_mergeable(rq)	\
	(!((rq)->flags & RQ_NOMERGE_FLAGS) && blk_fs_request((rq)))

/*
 * noop, requests are automagically marked as active/inactive by I/O
 * scheduler -- see elv_next_request
 */
#define blk_queue_headactive(q, head_active)

/* current index into bio being processed for submission */
#define blk_rq_idx(rq)	((rq)->cbio->bi_vcnt - (rq)->nr_cbio_segments)

/* current bio vector being processed */
#define blk_rq_vec(rq)	(bio_iovec_idx((rq)->cbio, blk_rq_idx(rq)))

/* current offset with respect to start of the segment being submitted */
#define blk_rq_offset(rq) \
	(((rq)->hard_cur_sectors - (rq)->current_nr_sectors) << 9)

/*
 * temporarily mapping a (possible) highmem bio (typically for PIO transfer)
 */

/* Assumes rq->cbio != NULL */
static inline char * rq_map_buffer(struct request *rq, unsigned long *flags)
{
	return (__bio_kmap_irq(rq->cbio, blk_rq_idx(rq), flags)
		+ blk_rq_offset(rq));
}

static inline void rq_unmap_buffer(char *buffer, unsigned long *flags)
{
	__bio_kunmap_irq(buffer, flags);
}

/*
 * q->prep_rq_fn return values
 */
#define BLKPREP_OK		0	/* serve it */
#define BLKPREP_KILL		1	/* fatal error, kill */
#define BLKPREP_DEFER		2	/* leave on queue */

extern unsigned long blk_max_low_pfn, blk_max_pfn;

/*
 * standard bounce addresses:
 *
 * BLK_BOUNCE_HIGH	: bounce all highmem pages
 * BLK_BOUNCE_ANY	: don't bounce anything
 * BLK_BOUNCE_ISA	: bounce pages above ISA DMA boundary
 */
#define BLK_BOUNCE_HIGH		((u64)blk_max_low_pfn << PAGE_SHIFT)
#define BLK_BOUNCE_ANY		((u64)blk_max_pfn << PAGE_SHIFT)
#define BLK_BOUNCE_ISA		(ISA_DMA_THRESHOLD)

#ifdef CONFIG_MMU
extern int init_emergency_isa_pool(void);
extern void blk_queue_bounce(request_queue_t *q, struct bio **bio);
#else
static inline int init_emergency_isa_pool(void)
{
	return 0;
}
static inline void blk_queue_bounce(request_queue_t *q, struct bio **bio)
{
}
#endif /* CONFIG_MMU */

#define rq_for_each_bio(_bio, rq)	\
	if ((rq->bio))			\
		for (_bio = (rq)->bio; _bio; _bio = _bio->bi_next)

struct sec_size {
	unsigned block_size;
	unsigned block_size_bits;
};

extern int blk_register_queue(struct gendisk *disk);
extern void blk_unregister_queue(struct gendisk *disk);
extern void register_disk(struct gendisk *dev);
extern void generic_make_request(struct bio *bio);
extern void blk_put_request(struct request *);
extern void blk_attempt_remerge(request_queue_t *, struct request *);
extern void __blk_attempt_remerge(request_queue_t *, struct request *);
extern struct request *blk_get_request(request_queue_t *, int, int);
extern void blk_put_request(struct request *);
extern void blk_insert_request(request_queue_t *, struct request *, int, void *, int);
extern void blk_requeue_request(request_queue_t *, struct request *);
extern void blk_plug_device(request_queue_t *);
extern int blk_remove_plug(request_queue_t *);
extern void blk_recount_segments(request_queue_t *, struct bio *);
extern int blk_phys_contig_segment(request_queue_t *q, struct bio *, struct bio *);
extern int blk_hw_contig_segment(request_queue_t *q, struct bio *, struct bio *);
extern int scsi_cmd_ioctl(struct file *, struct gendisk *, unsigned int, void __user *);
extern void blk_start_queue(request_queue_t *q);
extern void blk_stop_queue(request_queue_t *q);
extern void __blk_stop_queue(request_queue_t *q);
extern void blk_run_queue(request_queue_t *);
extern void blk_queue_activity_fn(request_queue_t *, activity_fn *, void *);
extern struct request *blk_rq_map_user(request_queue_t *, int, void __user *, unsigned int);
extern int blk_rq_unmap_user(struct request *, struct bio *, unsigned int);
extern int blk_execute_rq(request_queue_t *, struct gendisk *, struct request *);

static inline request_queue_t *bdev_get_queue(struct block_device *bdev)
{
	return bdev->bd_disk->queue;
}

static inline void blk_run_backing_dev(struct backing_dev_info *bdi,
				       struct page *page)
{
	if (bdi && bdi->unplug_io_fn)
		bdi->unplug_io_fn(bdi, page);
}

static inline void blk_run_address_space(struct address_space *mapping)
{
	if (mapping)
		blk_run_backing_dev(mapping->backing_dev_info, NULL);
}

/*
 * end_request() and friends. Must be called with the request queue spinlock
 * acquired. All functions called within end_request() _must_be_ atomic.
 *
 * Several drivers define their own end_request and call
 * end_that_request_first() and end_that_request_last()
 * for parts of the original function. This prevents
 * code duplication in drivers.
 */
extern int end_that_request_first(struct request *, int, int);
extern int end_that_request_chunk(struct request *, int, int);
extern void end_that_request_last(struct request *);
extern int process_that_request_first(struct request *, unsigned int);
extern void end_request(struct request *req, int uptodate);

static inline void blkdev_dequeue_request(struct request *req)
{
	BUG_ON(list_empty(&req->queuelist));

	list_del_init(&req->queuelist);

	if (req->rl)
		elv_remove_request(req->q, req);
}

/*
 * Access functions for manipulating queue properties
 */
extern request_queue_t *blk_init_queue(request_fn_proc *, spinlock_t *);
extern void blk_cleanup_queue(request_queue_t *);
extern void blk_queue_make_request(request_queue_t *, make_request_fn *);
extern void blk_queue_bounce_limit(request_queue_t *, u64);
extern void blk_queue_max_sectors(request_queue_t *, unsigned short);
extern void blk_queue_max_phys_segments(request_queue_t *, unsigned short);
extern void blk_queue_max_hw_segments(request_queue_t *, unsigned short);
extern void blk_queue_max_segment_size(request_queue_t *, unsigned int);
extern void blk_queue_hardsect_size(request_queue_t *, unsigned short);
extern void blk_queue_stack_limits(request_queue_t *t, request_queue_t *b);
extern void blk_queue_segment_boundary(request_queue_t *, unsigned long);
extern void blk_queue_prep_rq(request_queue_t *, prep_rq_fn *pfn);
extern void blk_queue_merge_bvec(request_queue_t *, merge_bvec_fn *);
extern void blk_queue_dma_alignment(request_queue_t *, int);
extern struct backing_dev_info *blk_get_backing_dev_info(struct block_device *bdev);

extern int blk_rq_map_sg(request_queue_t *, struct request *, struct scatterlist *);
extern void blk_dump_rq_flags(struct request *, char *);
extern void generic_unplug_device(request_queue_t *);
extern void __generic_unplug_device(request_queue_t *);
extern long nr_blockdev_pages(void);

int blk_get_queue(request_queue_t *);
request_queue_t *blk_alloc_queue(int);
#define blk_put_queue(q) blk_cleanup_queue((q))

/*
 * tag stuff
 */
#define blk_queue_tag_depth(q)		((q)->queue_tags->busy)
#define blk_queue_tag_queue(q)		((q)->queue_tags->busy < (q)->queue_tags->max_depth)
#define blk_rq_tagged(rq)		((rq)->flags & REQ_QUEUED)
extern int blk_queue_start_tag(request_queue_t *, struct request *);
extern struct request *blk_queue_find_tag(request_queue_t *, int);
extern void blk_queue_end_tag(request_queue_t *, struct request *);
extern int blk_queue_init_tags(request_queue_t *, int, struct blk_queue_tag *);
extern void blk_queue_free_tags(request_queue_t *);
extern int blk_queue_resize_tags(request_queue_t *, int);
extern void blk_queue_invalidate_tags(request_queue_t *);
extern long blk_congestion_wait(int rw, long timeout);

extern void blk_rq_bio_prep(request_queue_t *, struct request *, struct bio *);
extern void blk_rq_prep_restart(struct request *);

#define MAX_PHYS_SEGMENTS 128
#define MAX_HW_SEGMENTS 128
#define MAX_SECTORS 255

#define MAX_SEGMENT_SIZE	65536

#define blkdev_entry_to_request(entry) list_entry((entry), struct request, queuelist)

extern void drive_stat_acct(struct request *, int, int);

static inline int queue_hardsect_size(request_queue_t *q)
{
	int retval = 512;

	if (q && q->hardsect_size)
		retval = q->hardsect_size;

	return retval;
}

static inline int bdev_hardsect_size(struct block_device *bdev)
{
	return queue_hardsect_size(bdev_get_queue(bdev));
}

static inline int queue_dma_alignment(request_queue_t *q)
{
	int retval = 511;

	if (q && q->dma_alignment)
		retval = q->dma_alignment;

	return retval;
}

static inline int bdev_dma_aligment(struct block_device *bdev)
{
	return queue_dma_alignment(bdev_get_queue(bdev));
}

#define blk_finished_io(nsects)	do { } while (0)
#define blk_started_io(nsects)	do { } while (0)

/* assumes size > 256 */
static inline unsigned int blksize_bits(unsigned int size)
{
	unsigned int bits = 8;
	do {
		bits++;
		size >>= 1;
	} while (size > 256);
	return bits;
}

extern inline unsigned int block_size(struct block_device *bdev)
{
	return bdev->bd_block_size;
}

typedef struct {struct page *v;} Sector;

unsigned char *read_dev_sector(struct block_device *, sector_t, Sector *);

static inline void put_dev_sector(Sector p)
{
	page_cache_release(p.v);
}

struct work_struct;
int kblockd_schedule_work(struct work_struct *work);
void kblockd_flush(void);

#ifdef CONFIG_LBD
# include <asm/div64.h>
# define sector_div(a, b) do_div(a, b)
#else
# define sector_div(n, b)( \
{ \
	int _res; \
	_res = (n) % (b); \
	(n) /= (b); \
	_res; \
} \
)
#endif 

#define MODULE_ALIAS_BLOCKDEV(major,minor) \
	MODULE_ALIAS("block-major-" __stringify(major) "-" __stringify(minor))
#define MODULE_ALIAS_BLOCKDEV_MAJOR(major) \
	MODULE_ALIAS("block-major-" __stringify(major) "-*")


#endif
