/*
 *  linux/drivers/block/deadline-iosched.c
 *
 *  Deadline i/o scheduler.
 *
 *  Copyright (C) 2002 Jens Axboe <axboe@suse.de>
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/config.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/hash.h>
#include <linux/rbtree.h>

/*
 * See Documentation/deadline-iosched.txt
 */
static int read_expire = HZ / 2;  /* max time before a read is submitted. */
static int write_expire = 5 * HZ; /* ditto for writes, these limits are SOFT! */
static int writes_starved = 2;    /* max times reads can starve a write */
static int fifo_batch = 16;       /* # of sequential requests treated as one
				     by the above parameters. For throughput. */

static const int deadline_hash_shift = 5;
#define DL_HASH_BLOCK(sec)	((sec) >> 3)
#define DL_HASH_FN(sec)		(hash_long(DL_HASH_BLOCK((sec)), deadline_hash_shift))
#define DL_HASH_ENTRIES		(1 << deadline_hash_shift)
#define rq_hash_key(rq)		((rq)->sector + (rq)->nr_sectors)
#define list_entry_hash(ptr)	list_entry((ptr), struct deadline_rq, hash)
#define ON_HASH(drq)		(drq)->on_hash

struct deadline_data {
	/*
	 * run time data
	 */

	/*
	 * requests (deadline_rq s) are present on both sort_list and fifo_list
	 */
	struct rb_root sort_list[2];	
	struct list_head fifo_list[2];
	
	/*
	 * next in sort order. read, write or both are NULL
	 */
	struct deadline_rq *next_drq[2];
	struct list_head *dispatch;	/* driver dispatch queue */
	struct list_head *hash;		/* request hash */
	unsigned int batching;		/* number of sequential requests made */
	sector_t last_sector;		/* head position */
	unsigned int starved;		/* times reads have starved writes */

	/*
	 * settings that change how the i/o scheduler behaves
	 */
	int fifo_expire[2];
	int fifo_batch;
	int writes_starved;
	int front_merges;

	mempool_t *drq_pool;
};

/*
 * pre-request data.
 */
struct deadline_rq {
	/*
	 * rbtree index, key is the starting offset
	 */
	struct rb_node rb_node;
	sector_t rb_key;

	struct request *request;

	/*
	 * request hash, key is the ending offset (for back merge lookup)
	 */
	struct list_head hash;
	char on_hash;

	/*
	 * expire fifo
	 */
	struct list_head fifo;
	unsigned long expires;
};

static void deadline_move_request(struct deadline_data *dd, struct deadline_rq *drq);

static kmem_cache_t *drq_pool;

#define RQ_DATA(rq)	((struct deadline_rq *) (rq)->elevator_private)

/*
 * the back merge hash support functions
 */
static inline void __deadline_del_drq_hash(struct deadline_rq *drq)
{
	drq->on_hash = 0;
	list_del_init(&drq->hash);
}

static inline void deadline_del_drq_hash(struct deadline_rq *drq)
{
	if (ON_HASH(drq))
		__deadline_del_drq_hash(drq);
}

static void
deadline_remove_merge_hints(request_queue_t *q, struct deadline_rq *drq)
{
	deadline_del_drq_hash(drq);

	if (q->last_merge == drq->request)
		q->last_merge = NULL;
}

static inline void
deadline_add_drq_hash(struct deadline_data *dd, struct deadline_rq *drq)
{
	struct request *rq = drq->request;

	BUG_ON(ON_HASH(drq));

	drq->on_hash = 1;
	list_add(&drq->hash, &dd->hash[DL_HASH_FN(rq_hash_key(rq))]);
}

/*
 * move hot entry to front of chain
 */
static inline void
deadline_hot_drq_hash(struct deadline_data *dd, struct deadline_rq *drq)
{
	struct request *rq = drq->request;
	struct list_head *head = &dd->hash[DL_HASH_FN(rq_hash_key(rq))];

	if (ON_HASH(drq) && drq->hash.prev != head) {
		list_del(&drq->hash);
		list_add(&drq->hash, head);
	}
}

static struct request *
deadline_find_drq_hash(struct deadline_data *dd, sector_t offset)
{
	struct list_head *hash_list = &dd->hash[DL_HASH_FN(offset)];
	struct list_head *entry, *next = hash_list->next;

	while ((entry = next) != hash_list) {
		struct deadline_rq *drq = list_entry_hash(entry);
		struct request *__rq = drq->request;

		next = entry->next;
		
		BUG_ON(!ON_HASH(drq));

		if (!rq_mergeable(__rq)) {
			__deadline_del_drq_hash(drq);
			continue;
		}

		if (rq_hash_key(__rq) == offset)
			return __rq;
	}

	return NULL;
}

/*
 * rb tree support functions
 */
#define RB_NONE		(2)
#define RB_EMPTY(root)	((root)->rb_node == NULL)
#define ON_RB(node)	((node)->rb_color != RB_NONE)
#define RB_CLEAR(node)	((node)->rb_color = RB_NONE)
#define rb_entry_drq(node)	rb_entry((node), struct deadline_rq, rb_node)
#define DRQ_RB_ROOT(dd, drq)	(&(dd)->sort_list[rq_data_dir((drq)->request)])
#define rq_rb_key(rq)		(rq)->sector

static struct deadline_rq *
__deadline_add_drq_rb(struct deadline_data *dd, struct deadline_rq *drq)
{
	struct rb_node **p = &DRQ_RB_ROOT(dd, drq)->rb_node;
	struct rb_node *parent = NULL;
	struct deadline_rq *__drq;

	while (*p) {
		parent = *p;
		__drq = rb_entry_drq(parent);

		if (drq->rb_key < __drq->rb_key)
			p = &(*p)->rb_left;
		else if (drq->rb_key > __drq->rb_key)
			p = &(*p)->rb_right;
		else
			return __drq;
	}

	rb_link_node(&drq->rb_node, parent, p);
	return NULL;
}

static void
deadline_add_drq_rb(struct deadline_data *dd, struct deadline_rq *drq)
{
	struct deadline_rq *__alias;

	drq->rb_key = rq_rb_key(drq->request);

retry:
	__alias = __deadline_add_drq_rb(dd, drq);
	if (!__alias) {
		rb_insert_color(&drq->rb_node, DRQ_RB_ROOT(dd, drq));
		return;
	}

	deadline_move_request(dd, __alias);
	goto retry;
}

static inline void
deadline_del_drq_rb(struct deadline_data *dd, struct deadline_rq *drq)
{
	const int data_dir = rq_data_dir(drq->request);

	if (dd->next_drq[data_dir] == drq) {
		struct rb_node *rbnext = rb_next(&drq->rb_node);

		dd->next_drq[data_dir] = NULL;
		if (rbnext)
			dd->next_drq[data_dir] = rb_entry_drq(rbnext);
	}

	if (ON_RB(&drq->rb_node)) {
		rb_erase(&drq->rb_node, DRQ_RB_ROOT(dd, drq));
		RB_CLEAR(&drq->rb_node);
	}
}

static struct request *
deadline_find_drq_rb(struct deadline_data *dd, sector_t sector, int data_dir)
{
	struct rb_node *n = dd->sort_list[data_dir].rb_node;
	struct deadline_rq *drq;

	while (n) {
		drq = rb_entry_drq(n);

		if (sector < drq->rb_key)
			n = n->rb_left;
		else if (sector > drq->rb_key)
			n = n->rb_right;
		else
			return drq->request;
	}

	return NULL;
}

/*
 * deadline_find_first_drq finds the first (lowest sector numbered) request
 * for the specified data_dir. Used to sweep back to the start of the disk
 * (1-way elevator) after we process the last (highest sector) request.
 */
static struct deadline_rq *
deadline_find_first_drq(struct deadline_data *dd, int data_dir)
{
	struct rb_node *n = dd->sort_list[data_dir].rb_node;

	for (;;) {
		if (n->rb_left == NULL)
			return rb_entry_drq(n);
		
		n = n->rb_left;
	}
}

/*
 * add drq to rbtree and fifo
 */
static inline void
deadline_add_request(struct request_queue *q, struct request *rq)
{
	struct deadline_data *dd = q->elevator.elevator_data;
	struct deadline_rq *drq = RQ_DATA(rq);

	const int data_dir = rq_data_dir(drq->request);

	deadline_add_drq_rb(dd, drq);
	/*
	 * set expire time (only used for reads) and add to fifo list
	 */
	drq->expires = jiffies + dd->fifo_expire[data_dir];
	list_add_tail(&drq->fifo, &dd->fifo_list[data_dir]);

	if (rq_mergeable(rq)) {
		deadline_add_drq_hash(dd, drq);

		if (!q->last_merge)
			q->last_merge = rq;
	}
}

/*
 * remove rq from rbtree, fifo, and hash
 */
static void deadline_remove_request(request_queue_t *q, struct request *rq)
{
	struct deadline_rq *drq = RQ_DATA(rq);

	if (drq) {
		struct deadline_data *dd = q->elevator.elevator_data;

		list_del_init(&drq->fifo);
		deadline_remove_merge_hints(q, drq);
		deadline_del_drq_rb(dd, drq);
	}
}

static int
deadline_merge(request_queue_t *q, struct request **req, struct bio *bio)
{
	struct deadline_data *dd = q->elevator.elevator_data;
	struct request *__rq;
	int ret;

	/*
	 * try last_merge to avoid going to hash
	 */
	ret = elv_try_last_merge(q, bio);
	if (ret != ELEVATOR_NO_MERGE) {
		__rq = q->last_merge;
		goto out_insert;
	}

	/*
	 * see if the merge hash can satisfy a back merge
	 */
	__rq = deadline_find_drq_hash(dd, bio->bi_sector);
	if (__rq) {
		BUG_ON(__rq->sector + __rq->nr_sectors != bio->bi_sector);

		if (elv_rq_merge_ok(__rq, bio)) {
			ret = ELEVATOR_BACK_MERGE;
			goto out;
		}
	}

	/*
	 * check for front merge
	 */
	if (dd->front_merges) {
		sector_t rb_key = bio->bi_sector + bio_sectors(bio);

		__rq = deadline_find_drq_rb(dd, rb_key, bio_data_dir(bio));
		if (__rq) {
			BUG_ON(rb_key != rq_rb_key(__rq));

			if (elv_rq_merge_ok(__rq, bio)) {
				ret = ELEVATOR_FRONT_MERGE;
				goto out;
			}
		}
	}

	return ELEVATOR_NO_MERGE;
out:
	q->last_merge = __rq;
out_insert:
	if (ret)
		deadline_hot_drq_hash(dd, RQ_DATA(__rq));
	*req = __rq;
	return ret;
}

static void deadline_merged_request(request_queue_t *q, struct request *req)
{
	struct deadline_data *dd = q->elevator.elevator_data;
	struct deadline_rq *drq = RQ_DATA(req);

	/*
	 * hash always needs to be repositioned, key is end sector
	 */
	deadline_del_drq_hash(drq);
	deadline_add_drq_hash(dd, drq);

	/*
	 * if the merge was a front merge, we need to reposition request
	 */
	if (rq_rb_key(req) != drq->rb_key) {
		deadline_del_drq_rb(dd, drq);
		deadline_add_drq_rb(dd, drq);
	}

	q->last_merge = req;
}

static void
deadline_merged_requests(request_queue_t *q, struct request *req,
			 struct request *next)
{
	struct deadline_data *dd = q->elevator.elevator_data;
	struct deadline_rq *drq = RQ_DATA(req);
	struct deadline_rq *dnext = RQ_DATA(next);

	BUG_ON(!drq);
	BUG_ON(!dnext);

	/*
	 * reposition drq (this is the merged request) in hash, and in rbtree
	 * in case of a front merge
	 */
	deadline_del_drq_hash(drq);
	deadline_add_drq_hash(dd, drq);

	if (rq_rb_key(req) != drq->rb_key) {
		deadline_del_drq_rb(dd, drq);
		deadline_add_drq_rb(dd, drq);
	}

	/*
	 * if dnext expires before drq, assign its expire time to drq
	 * and move into dnext position (dnext will be deleted) in fifo
	 */
	if (!list_empty(&drq->fifo) && !list_empty(&dnext->fifo)) {
		if (time_before(dnext->expires, drq->expires)) {
			list_move(&drq->fifo, &dnext->fifo);
			drq->expires = dnext->expires;
		}
	}

	/*
	 * kill knowledge of next, this one is a goner
	 */
	deadline_remove_request(q, next);
}

/*
 * move request from sort list to dispatch queue.
 */
static inline void
deadline_move_to_dispatch(struct deadline_data *dd, struct deadline_rq *drq)
{
	request_queue_t *q = drq->request->q;

	deadline_remove_request(q, drq->request);
	list_add_tail(&drq->request->queuelist, dd->dispatch);
}

/*
 * move an entry to dispatch queue
 */
static void
deadline_move_request(struct deadline_data *dd, struct deadline_rq *drq)
{
	const int data_dir = rq_data_dir(drq->request);
	struct rb_node *rbnext = rb_next(&drq->rb_node);

	dd->next_drq[READ] = NULL;
	dd->next_drq[WRITE] = NULL;

	if (rbnext)
		dd->next_drq[data_dir] = rb_entry_drq(rbnext);
	
	dd->last_sector = drq->request->sector + drq->request->nr_sectors;

	/*
	 * take it off the sort and fifo list, move
	 * to dispatch queue
	 */
	deadline_move_to_dispatch(dd, drq);
}

#define list_entry_fifo(ptr)	list_entry((ptr), struct deadline_rq, fifo)

/*
 * deadline_check_fifo returns 0 if there are no expired reads on the fifo,
 * 1 otherwise. Requires !list_empty(&dd->fifo_list[data_dir])
 */
static inline int deadline_check_fifo(struct deadline_data *dd, int ddir)
{
	struct deadline_rq *drq = list_entry_fifo(dd->fifo_list[ddir].next);

	/*
	 * drq is expired!
	 */
	if (time_after(jiffies, drq->expires))
		return 1;

	return 0;
}

/*
 * deadline_dispatch_requests selects the best request according to
 * read/write expire, fifo_batch, etc
 */
static int deadline_dispatch_requests(struct deadline_data *dd)
{
	const int reads = !list_empty(&dd->fifo_list[READ]);
	const int writes = !list_empty(&dd->fifo_list[WRITE]);
	struct deadline_rq *drq;
	int data_dir, other_dir;

	/*
	 * batches are currently reads XOR writes
	 */
	drq = NULL;

	if (dd->next_drq[READ])
		drq = dd->next_drq[READ];

	if (dd->next_drq[WRITE])
		drq = dd->next_drq[WRITE];

	if (drq) {
		/* we have a "next request" */
		
		if (dd->last_sector != drq->request->sector)
			/* end the batch on a non sequential request */
			dd->batching += dd->fifo_batch;
		
		if (dd->batching < dd->fifo_batch)
			/* we are still entitled to batch */
			goto dispatch_request;
	}

	/*
	 * at this point we are not running a batch. select the appropriate
	 * data direction (read / write)
	 */

	if (reads) {
		BUG_ON(RB_EMPTY(&dd->sort_list[READ]));

		if (writes && (dd->starved++ >= dd->writes_starved))
			goto dispatch_writes;

		data_dir = READ;
		other_dir = WRITE;

		goto dispatch_find_request;
	}

	/*
	 * there are either no reads or writes have been starved
	 */

	if (writes) {
dispatch_writes:
		BUG_ON(RB_EMPTY(&dd->sort_list[WRITE]));

		dd->starved = 0;

		data_dir = WRITE;
		other_dir = READ;

		goto dispatch_find_request;
	}

	return 0;

dispatch_find_request:
	/*
	 * we are not running a batch, find best request for selected data_dir
	 */
	if (deadline_check_fifo(dd, data_dir)) {
		/* An expired request exists - satisfy it */
		dd->batching = 0;
		drq = list_entry_fifo(dd->fifo_list[data_dir].next);
		
	} else if (dd->next_drq[data_dir]) {
		/*
		 * The last req was the same dir and we have a next request in
		 * sort order. No expired requests so continue on from here.
		 */
		drq = dd->next_drq[data_dir];
	} else {
		/*
		 * The last req was the other direction or we have run out of
		 * higher-sectored requests. Go back to the lowest sectored
		 * request (1 way elevator) and start a new batch.
		 */
		dd->batching = 0;
		drq = deadline_find_first_drq(dd, data_dir);
	}

dispatch_request:
	/*
	 * drq is the selected appropriate request.
	 */
	dd->batching++;
	deadline_move_request(dd, drq);

	return 1;
}

static struct request *deadline_next_request(request_queue_t *q)
{
	struct deadline_data *dd = q->elevator.elevator_data;
	struct request *rq;

	/*
	 * if there are still requests on the dispatch queue, grab the first one
	 */
	if (!list_empty(dd->dispatch)) {
dispatch:
		rq = list_entry_rq(dd->dispatch->next);
		return rq;
	}

	if (deadline_dispatch_requests(dd))
		goto dispatch;

	return NULL;
}

static void
deadline_insert_request(request_queue_t *q, struct request *rq, int where)
{
	struct deadline_data *dd = q->elevator.elevator_data;

	/* barriers must flush the reorder queue */
	if (unlikely(rq->flags & (REQ_SOFTBARRIER | REQ_HARDBARRIER)
			&& where == ELEVATOR_INSERT_SORT))
		where = ELEVATOR_INSERT_BACK;

	switch (where) {
		case ELEVATOR_INSERT_BACK:
			while (deadline_dispatch_requests(dd))
				;
			list_add_tail(&rq->queuelist, dd->dispatch);
			break;
		case ELEVATOR_INSERT_FRONT:
			list_add(&rq->queuelist, dd->dispatch);
			break;
		case ELEVATOR_INSERT_SORT:
			BUG_ON(!blk_fs_request(rq));
			deadline_add_request(q, rq);
			break;
		default:
			printk("%s: bad insert point %d\n", __FUNCTION__,where);
			return;
	}
}

static int deadline_queue_empty(request_queue_t *q)
{
	struct deadline_data *dd = q->elevator.elevator_data;

	if (!list_empty(&dd->fifo_list[WRITE])
	    || !list_empty(&dd->fifo_list[READ])
	    || !list_empty(dd->dispatch))
		return 0;

	return 1;
}

static struct request *
deadline_former_request(request_queue_t *q, struct request *rq)
{
	struct deadline_rq *drq = RQ_DATA(rq);
	struct rb_node *rbprev = rb_prev(&drq->rb_node);

	if (rbprev)
		return rb_entry_drq(rbprev)->request;

	return NULL;
}

static struct request *
deadline_latter_request(request_queue_t *q, struct request *rq)
{
	struct deadline_rq *drq = RQ_DATA(rq);
	struct rb_node *rbnext = rb_next(&drq->rb_node);

	if (rbnext)
		return rb_entry_drq(rbnext)->request;

	return NULL;
}

static void deadline_exit(request_queue_t *q, elevator_t *e)
{
	struct deadline_data *dd = e->elevator_data;

	BUG_ON(!list_empty(&dd->fifo_list[READ]));
	BUG_ON(!list_empty(&dd->fifo_list[WRITE]));

	mempool_destroy(dd->drq_pool);
	kfree(dd->hash);
	kfree(dd);
}

/*
 * initialize elevator private data (deadline_data), and alloc a drq for
 * each request on the free lists
 */
static int deadline_init(request_queue_t *q, elevator_t *e)
{
	struct deadline_data *dd;
	int i;

	if (!drq_pool)
		return -ENOMEM;

	dd = kmalloc(sizeof(*dd), GFP_KERNEL);
	if (!dd)
		return -ENOMEM;
	memset(dd, 0, sizeof(*dd));

	dd->hash = kmalloc(sizeof(struct list_head)*DL_HASH_ENTRIES,GFP_KERNEL);
	if (!dd->hash) {
		kfree(dd);
		return -ENOMEM;
	}

	dd->drq_pool = mempool_create(BLKDEV_MIN_RQ, mempool_alloc_slab, mempool_free_slab, drq_pool);
	if (!dd->drq_pool) {
		kfree(dd->hash);
		kfree(dd);
		return -ENOMEM;
	}

	for (i = 0; i < DL_HASH_ENTRIES; i++)
		INIT_LIST_HEAD(&dd->hash[i]);

	INIT_LIST_HEAD(&dd->fifo_list[READ]);
	INIT_LIST_HEAD(&dd->fifo_list[WRITE]);
	dd->sort_list[READ] = RB_ROOT;
	dd->sort_list[WRITE] = RB_ROOT;
	dd->dispatch = &q->queue_head;
	dd->fifo_expire[READ] = read_expire;
	dd->fifo_expire[WRITE] = write_expire;
	dd->writes_starved = writes_starved;
	dd->front_merges = 1;
	dd->fifo_batch = fifo_batch;
	e->elevator_data = dd;
	return 0;
}

static void deadline_put_request(request_queue_t *q, struct request *rq)
{
	struct deadline_data *dd = q->elevator.elevator_data;
	struct deadline_rq *drq = RQ_DATA(rq);

	if (drq) {
		mempool_free(drq, dd->drq_pool);
		rq->elevator_private = NULL;
	}
}

static int
deadline_set_request(request_queue_t *q, struct request *rq, int gfp_mask)
{
	struct deadline_data *dd = q->elevator.elevator_data;
	struct deadline_rq *drq;

	drq = mempool_alloc(dd->drq_pool, gfp_mask);
	if (drq) {
		memset(drq, 0, sizeof(*drq));
		RB_CLEAR(&drq->rb_node);
		drq->request = rq;

		INIT_LIST_HEAD(&drq->hash);
		drq->on_hash = 0;

		INIT_LIST_HEAD(&drq->fifo);

		rq->elevator_private = drq;
		return 0;
	}

	return 1;
}

/*
 * sysfs parts below
 */
struct deadline_fs_entry {
	struct attribute attr;
	ssize_t (*show)(struct deadline_data *, char *);
	ssize_t (*store)(struct deadline_data *, const char *, size_t);
};

static ssize_t
deadline_var_show(unsigned int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t
deadline_var_store(unsigned int *var, const char *page, size_t count)
{
	char *p = (char *) page;

	*var = simple_strtoul(p, &p, 10);
	return count;
}

#define SHOW_FUNCTION(__FUNC, __VAR)					\
static ssize_t __FUNC(struct deadline_data *dd, char *page)		\
{									\
	return deadline_var_show(__VAR, (page));			\
}
SHOW_FUNCTION(deadline_readexpire_show, dd->fifo_expire[READ]);
SHOW_FUNCTION(deadline_writeexpire_show, dd->fifo_expire[WRITE]);
SHOW_FUNCTION(deadline_writesstarved_show, dd->writes_starved);
SHOW_FUNCTION(deadline_frontmerges_show, dd->front_merges);
SHOW_FUNCTION(deadline_fifobatch_show, dd->fifo_batch);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX)				\
static ssize_t __FUNC(struct deadline_data *dd, const char *page, size_t count)	\
{									\
	int ret = deadline_var_store(__PTR, (page), count);		\
	if (*(__PTR) < (MIN))						\
		*(__PTR) = (MIN);					\
	else if (*(__PTR) > (MAX))					\
		*(__PTR) = (MAX);					\
	return ret;							\
}
STORE_FUNCTION(deadline_readexpire_store, &dd->fifo_expire[READ], 0, INT_MAX);
STORE_FUNCTION(deadline_writeexpire_store, &dd->fifo_expire[WRITE], 0, INT_MAX);
STORE_FUNCTION(deadline_writesstarved_store, &dd->writes_starved, INT_MIN, INT_MAX);
STORE_FUNCTION(deadline_frontmerges_store, &dd->front_merges, 0, 1);
STORE_FUNCTION(deadline_fifobatch_store, &dd->fifo_batch, 0, INT_MAX);
#undef STORE_FUNCTION

static struct deadline_fs_entry deadline_readexpire_entry = {
	.attr = {.name = "read_expire", .mode = S_IRUGO | S_IWUSR },
	.show = deadline_readexpire_show,
	.store = deadline_readexpire_store,
};
static struct deadline_fs_entry deadline_writeexpire_entry = {
	.attr = {.name = "write_expire", .mode = S_IRUGO | S_IWUSR },
	.show = deadline_writeexpire_show,
	.store = deadline_writeexpire_store,
};
static struct deadline_fs_entry deadline_writesstarved_entry = {
	.attr = {.name = "writes_starved", .mode = S_IRUGO | S_IWUSR },
	.show = deadline_writesstarved_show,
	.store = deadline_writesstarved_store,
};
static struct deadline_fs_entry deadline_frontmerges_entry = {
	.attr = {.name = "front_merges", .mode = S_IRUGO | S_IWUSR },
	.show = deadline_frontmerges_show,
	.store = deadline_frontmerges_store,
};
static struct deadline_fs_entry deadline_fifobatch_entry = {
	.attr = {.name = "fifo_batch", .mode = S_IRUGO | S_IWUSR },
	.show = deadline_fifobatch_show,
	.store = deadline_fifobatch_store,
};

static struct attribute *default_attrs[] = {
	&deadline_readexpire_entry.attr,
	&deadline_writeexpire_entry.attr,
	&deadline_writesstarved_entry.attr,
	&deadline_frontmerges_entry.attr,
	&deadline_fifobatch_entry.attr,
	NULL,
};

#define to_deadline(atr) container_of((atr), struct deadline_fs_entry, attr)

static ssize_t
deadline_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	elevator_t *e = container_of(kobj, elevator_t, kobj);
	struct deadline_fs_entry *entry = to_deadline(attr);

	if (!entry->show)
		return 0;

	return entry->show(e->elevator_data, page);
}

static ssize_t
deadline_attr_store(struct kobject *kobj, struct attribute *attr,
		    const char *page, size_t length)
{
	elevator_t *e = container_of(kobj, elevator_t, kobj);
	struct deadline_fs_entry *entry = to_deadline(attr);

	if (!entry->store)
		return -EINVAL;

	return entry->store(e->elevator_data, page, length);
}

static struct sysfs_ops deadline_sysfs_ops = {
	.show	= deadline_attr_show,
	.store	= deadline_attr_store,
};

struct kobj_type deadline_ktype = {
	.sysfs_ops	= &deadline_sysfs_ops,
	.default_attrs	= default_attrs,
};

static int __init deadline_slab_setup(void)
{
	drq_pool = kmem_cache_create("deadline_drq", sizeof(struct deadline_rq),
				     0, 0, NULL, NULL);

	if (!drq_pool)
		panic("deadline: can't init slab pool\n");

	return 0;
}

subsys_initcall(deadline_slab_setup);

elevator_t iosched_deadline = {
	.elevator_merge_fn = 		deadline_merge,
	.elevator_merged_fn =		deadline_merged_request,
	.elevator_merge_req_fn =	deadline_merged_requests,
	.elevator_next_req_fn =		deadline_next_request,
	.elevator_add_req_fn =		deadline_insert_request,
	.elevator_remove_req_fn =	deadline_remove_request,
	.elevator_queue_empty_fn =	deadline_queue_empty,
	.elevator_former_req_fn =	deadline_former_request,
	.elevator_latter_req_fn =	deadline_latter_request,
	.elevator_set_req_fn =		deadline_set_request,
	.elevator_put_req_fn = 		deadline_put_request,
	.elevator_init_fn =		deadline_init,
	.elevator_exit_fn =		deadline_exit,

	.elevator_ktype =		&deadline_ktype,
	.elevator_name =		"deadline",
};

EXPORT_SYMBOL(iosched_deadline);
