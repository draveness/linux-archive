/*
 *  linux/drivers/block/as-iosched.c
 *
 *  Anticipatory & deadline i/o scheduler.
 *
 *  Copyright (C) 2002 Jens Axboe <axboe@suse.de>
 *                     Nick Piggin <piggin@cyberone.com.au>
 *
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
#include <linux/interrupt.h>

#define REQ_SYNC	1
#define REQ_ASYNC	0

/*
 * See Documentation/as-iosched.txt
 */

/*
 * max time before a read is submitted.
 */
#define default_read_expire (HZ / 8)

/*
 * ditto for writes, these limits are not hard, even
 * if the disk is capable of satisfying them.
 */
#define default_write_expire (HZ / 4)

/*
 * read_batch_expire describes how long we will allow a stream of reads to
 * persist before looking to see whether it is time to switch over to writes.
 */
#define default_read_batch_expire (HZ / 2)

/*
 * write_batch_expire describes how long we want a stream of writes to run for.
 * This is not a hard limit, but a target we set for the auto-tuning thingy.
 * See, the problem is: we can send a lot of writes to disk cache / TCQ in
 * a short amount of time...
 */
#define default_write_batch_expire (HZ / 8)

/*
 * max time we may wait to anticipate a read (default around 6ms)
 */
#define default_antic_expire ((HZ / 150) ? HZ / 150 : 1)

/*
 * Keep track of up to 20ms thinktimes. We can go as big as we like here,
 * however huge values tend to interfere and not decay fast enough. A program
 * might be in a non-io phase of operation. Waiting on user input for example,
 * or doing a lengthy computation. A small penalty can be justified there, and
 * will still catch out those processes that constantly have large thinktimes.
 */
#define MAX_THINKTIME (HZ/50UL)

/* Bits in as_io_context.state */
enum as_io_states {
	AS_TASK_RUNNING=0,	/* Process has not exitted */
	AS_TASK_IOSTARTED,	/* Process has started some IO */
	AS_TASK_IORUNNING,	/* Process has completed some IO */
};

enum anticipation_status {
	ANTIC_OFF=0,		/* Not anticipating (normal operation)	*/
	ANTIC_WAIT_REQ,		/* The last read has not yet completed  */
	ANTIC_WAIT_NEXT,	/* Currently anticipating a request vs
				   last read (which has completed) */
	ANTIC_FINISHED,		/* Anticipating but have found a candidate
				 * or timed out */
};

struct as_data {
	/*
	 * run time data
	 */

	struct request_queue *q;	/* the "owner" queue */

	/*
	 * requests (as_rq s) are present on both sort_list and fifo_list
	 */
	struct rb_root sort_list[2];
	struct list_head fifo_list[2];

	struct as_rq *next_arq[2];	/* next in sort order */
	sector_t last_sector[2];	/* last REQ_SYNC & REQ_ASYNC sectors */
	struct list_head *dispatch;	/* driver dispatch queue */
	struct list_head *hash;		/* request hash */

	unsigned long exit_prob;	/* probability a task will exit while
					   being waited on */
	unsigned long new_ttime_total; 	/* mean thinktime on new proc */
	unsigned long new_ttime_mean;
	u64 new_seek_total;		/* mean seek on new proc */
	sector_t new_seek_mean;

	unsigned long current_batch_expires;
	unsigned long last_check_fifo[2];
	int changed_batch;		/* 1: waiting for old batch to end */
	int new_batch;			/* 1: waiting on first read complete */
	int batch_data_dir;		/* current batch REQ_SYNC / REQ_ASYNC */
	int write_batch_count;		/* max # of reqs in a write batch */
	int current_write_count;	/* how many requests left this batch */
	int write_batch_idled;		/* has the write batch gone idle? */
	mempool_t *arq_pool;

	enum anticipation_status antic_status;
	unsigned long antic_start;	/* jiffies: when it started */
	struct timer_list antic_timer;	/* anticipatory scheduling timer */
	struct work_struct antic_work;	/* Deferred unplugging */
	struct io_context *io_context;	/* Identify the expected process */
	int ioc_finished; /* IO associated with io_context is finished */
	int nr_dispatched;

	/*
	 * settings that change how the i/o scheduler behaves
	 */
	unsigned long fifo_expire[2];
	unsigned long batch_expire[2];
	unsigned long antic_expire;
};

#define list_entry_fifo(ptr)	list_entry((ptr), struct as_rq, fifo)

/*
 * per-request data.
 */
enum arq_state {
	AS_RQ_NEW=0,		/* New - not referenced and not on any lists */
	AS_RQ_QUEUED,		/* In the request queue. It belongs to the
				   scheduler */
	AS_RQ_DISPATCHED,	/* On the dispatch list. It belongs to the
				   driver now */
	AS_RQ_PRESCHED,		/* Debug poisoning for requests being used */
	AS_RQ_REMOVED,
	AS_RQ_MERGED,
	AS_RQ_POSTSCHED,	/* when they shouldn't be */
};

struct as_rq {
	/*
	 * rbtree index, key is the starting offset
	 */
	struct rb_node rb_node;
	sector_t rb_key;

	struct request *request;

	struct io_context *io_context;	/* The submitting task */

	/*
	 * request hash, key is the ending offset (for back merge lookup)
	 */
	struct list_head hash;
	unsigned int on_hash;

	/*
	 * expire fifo
	 */
	struct list_head fifo;
	unsigned long expires;

	unsigned int is_sync;
	enum arq_state state;
};

#define RQ_DATA(rq)	((struct as_rq *) (rq)->elevator_private)

static kmem_cache_t *arq_pool;

/*
 * IO Context helper functions
 */

/* Called to deallocate the as_io_context */
static void free_as_io_context(struct as_io_context *aic)
{
	kfree(aic);
}

/* Called when the task exits */
static void exit_as_io_context(struct as_io_context *aic)
{
	WARN_ON(!test_bit(AS_TASK_RUNNING, &aic->state));
	clear_bit(AS_TASK_RUNNING, &aic->state);
}

static struct as_io_context *alloc_as_io_context(void)
{
	struct as_io_context *ret;

	ret = kmalloc(sizeof(*ret), GFP_ATOMIC);
	if (ret) {
		ret->dtor = free_as_io_context;
		ret->exit = exit_as_io_context;
		ret->state = 1 << AS_TASK_RUNNING;
		atomic_set(&ret->nr_queued, 0);
		atomic_set(&ret->nr_dispatched, 0);
		spin_lock_init(&ret->lock);
		ret->ttime_total = 0;
		ret->ttime_samples = 0;
		ret->ttime_mean = 0;
		ret->seek_total = 0;
		ret->seek_samples = 0;
		ret->seek_mean = 0;
	}

	return ret;
}

/*
 * If the current task has no AS IO context then create one and initialise it.
 * Then take a ref on the task's io context and return it.
 */
static struct io_context *as_get_io_context(void)
{
	struct io_context *ioc = get_io_context(GFP_ATOMIC);
	if (ioc && !ioc->aic) {
		ioc->aic = alloc_as_io_context();
		if (!ioc->aic) {
			put_io_context(ioc);
			ioc = NULL;
		}
	}
	return ioc;
}

/*
 * the back merge hash support functions
 */
static const int as_hash_shift = 6;
#define AS_HASH_BLOCK(sec)	((sec) >> 3)
#define AS_HASH_FN(sec)		(hash_long(AS_HASH_BLOCK((sec)), as_hash_shift))
#define AS_HASH_ENTRIES		(1 << as_hash_shift)
#define rq_hash_key(rq)		((rq)->sector + (rq)->nr_sectors)
#define list_entry_hash(ptr)	list_entry((ptr), struct as_rq, hash)

static inline void __as_del_arq_hash(struct as_rq *arq)
{
	arq->on_hash = 0;
	list_del_init(&arq->hash);
}

static inline void as_del_arq_hash(struct as_rq *arq)
{
	if (arq->on_hash)
		__as_del_arq_hash(arq);
}

static void as_remove_merge_hints(request_queue_t *q, struct as_rq *arq)
{
	as_del_arq_hash(arq);

	if (q->last_merge == arq->request)
		q->last_merge = NULL;
}

static void as_add_arq_hash(struct as_data *ad, struct as_rq *arq)
{
	struct request *rq = arq->request;

	BUG_ON(arq->on_hash);

	arq->on_hash = 1;
	list_add(&arq->hash, &ad->hash[AS_HASH_FN(rq_hash_key(rq))]);
}

/*
 * move hot entry to front of chain
 */
static inline void as_hot_arq_hash(struct as_data *ad, struct as_rq *arq)
{
	struct request *rq = arq->request;
	struct list_head *head = &ad->hash[AS_HASH_FN(rq_hash_key(rq))];

	if (!arq->on_hash) {
		WARN_ON(1);
		return;
	}

	if (arq->hash.prev != head) {
		list_del(&arq->hash);
		list_add(&arq->hash, head);
	}
}

static struct request *as_find_arq_hash(struct as_data *ad, sector_t offset)
{
	struct list_head *hash_list = &ad->hash[AS_HASH_FN(offset)];
	struct list_head *entry, *next = hash_list->next;

	while ((entry = next) != hash_list) {
		struct as_rq *arq = list_entry_hash(entry);
		struct request *__rq = arq->request;

		next = entry->next;

		BUG_ON(!arq->on_hash);

		if (!rq_mergeable(__rq)) {
			as_remove_merge_hints(ad->q, arq);
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
#define rb_entry_arq(node)	rb_entry((node), struct as_rq, rb_node)
#define ARQ_RB_ROOT(ad, arq)	(&(ad)->sort_list[(arq)->is_sync])
#define rq_rb_key(rq)		(rq)->sector

/*
 * as_find_first_arq finds the first (lowest sector numbered) request
 * for the specified data_dir. Used to sweep back to the start of the disk
 * (1-way elevator) after we process the last (highest sector) request.
 */
static struct as_rq *as_find_first_arq(struct as_data *ad, int data_dir)
{
	struct rb_node *n = ad->sort_list[data_dir].rb_node;

	if (n == NULL)
		return NULL;

	for (;;) {
		if (n->rb_left == NULL)
			return rb_entry_arq(n);

		n = n->rb_left;
	}
}

/*
 * Add the request to the rb tree if it is unique.  If there is an alias (an
 * existing request against the same sector), which can happen when using
 * direct IO, then return the alias.
 */
static struct as_rq *as_add_arq_rb(struct as_data *ad, struct as_rq *arq)
{
	struct rb_node **p = &ARQ_RB_ROOT(ad, arq)->rb_node;
	struct rb_node *parent = NULL;
	struct as_rq *__arq;
	struct request *rq = arq->request;

	arq->rb_key = rq_rb_key(rq);

	while (*p) {
		parent = *p;
		__arq = rb_entry_arq(parent);

		if (arq->rb_key < __arq->rb_key)
			p = &(*p)->rb_left;
		else if (arq->rb_key > __arq->rb_key)
			p = &(*p)->rb_right;
		else
			return __arq;
	}

	rb_link_node(&arq->rb_node, parent, p);
	rb_insert_color(&arq->rb_node, ARQ_RB_ROOT(ad, arq));

	return NULL;
}

static inline void as_del_arq_rb(struct as_data *ad, struct as_rq *arq)
{
	if (!ON_RB(&arq->rb_node)) {
		WARN_ON(1);
		return;
	}

	rb_erase(&arq->rb_node, ARQ_RB_ROOT(ad, arq));
	RB_CLEAR(&arq->rb_node);
}

static struct request *
as_find_arq_rb(struct as_data *ad, sector_t sector, int data_dir)
{
	struct rb_node *n = ad->sort_list[data_dir].rb_node;
	struct as_rq *arq;

	while (n) {
		arq = rb_entry_arq(n);

		if (sector < arq->rb_key)
			n = n->rb_left;
		else if (sector > arq->rb_key)
			n = n->rb_right;
		else
			return arq->request;
	}

	return NULL;
}

/*
 * IO Scheduler proper
 */

#define MAXBACK (1024 * 1024)	/*
				 * Maximum distance the disk will go backward
				 * for a request.
				 */

#define BACK_PENALTY	2

/*
 * as_choose_req selects the preferred one of two requests of the same data_dir
 * ignoring time - eg. timeouts, which is the job of as_dispatch_request
 */
static struct as_rq *
as_choose_req(struct as_data *ad, struct as_rq *arq1, struct as_rq *arq2)
{
	int data_dir;
	sector_t last, s1, s2, d1, d2;
	int r1_wrap=0, r2_wrap=0;	/* requests are behind the disk head */
	const sector_t maxback = MAXBACK;

	if (arq1 == NULL || arq1 == arq2)
		return arq2;
	if (arq2 == NULL)
		return arq1;

	data_dir = arq1->is_sync;

	last = ad->last_sector[data_dir];
	s1 = arq1->request->sector;
	s2 = arq2->request->sector;

	BUG_ON(data_dir != arq2->is_sync);

	/*
	 * Strict one way elevator _except_ in the case where we allow
	 * short backward seeks which are biased as twice the cost of a
	 * similar forward seek.
	 */
	if (s1 >= last)
		d1 = s1 - last;
	else if (s1+maxback >= last)
		d1 = (last - s1)*BACK_PENALTY;
	else {
		r1_wrap = 1;
		d1 = 0; /* shut up, gcc */
	}

	if (s2 >= last)
		d2 = s2 - last;
	else if (s2+maxback >= last)
		d2 = (last - s2)*BACK_PENALTY;
	else {
		r2_wrap = 1;
		d2 = 0;
	}

	/* Found required data */
	if (!r1_wrap && r2_wrap)
		return arq1;
	else if (!r2_wrap && r1_wrap)
		return arq2;
	else if (r1_wrap && r2_wrap) {
		/* both behind the head */
		if (s1 <= s2)
			return arq1;
		else
			return arq2;
	}

	/* Both requests in front of the head */
	if (d1 < d2)
		return arq1;
	else if (d2 < d1)
		return arq2;
	else {
		if (s1 >= s2)
			return arq1;
		else
			return arq2;
	}
}

/*
 * as_find_next_arq finds the next request after @prev in elevator order.
 * this with as_choose_req form the basis for how the scheduler chooses
 * what request to process next. Anticipation works on top of this.
 */
static struct as_rq *as_find_next_arq(struct as_data *ad, struct as_rq *last)
{
	const int data_dir = last->is_sync;
	struct as_rq *ret;
	struct rb_node *rbnext = rb_next(&last->rb_node);
	struct rb_node *rbprev = rb_prev(&last->rb_node);
	struct as_rq *arq_next, *arq_prev;

	BUG_ON(!ON_RB(&last->rb_node));

	if (rbprev)
		arq_prev = rb_entry_arq(rbprev);
	else
		arq_prev = NULL;

	if (rbnext)
		arq_next = rb_entry_arq(rbnext);
	else {
		arq_next = as_find_first_arq(ad, data_dir);
		if (arq_next == last)
			arq_next = NULL;
	}

	ret = as_choose_req(ad,	arq_next, arq_prev);

	return ret;
}

/*
 * anticipatory scheduling functions follow
 */

/*
 * as_antic_expired tells us when we have anticipated too long.
 * The funny "absolute difference" math on the elapsed time is to handle
 * jiffy wraps, and disks which have been idle for 0x80000000 jiffies.
 */
static int as_antic_expired(struct as_data *ad)
{
	long delta_jif;

	delta_jif = jiffies - ad->antic_start;
	if (unlikely(delta_jif < 0))
		delta_jif = -delta_jif;
	if (delta_jif < ad->antic_expire)
		return 0;

	return 1;
}

/*
 * as_antic_waitnext starts anticipating that a nice request will soon be
 * submitted. See also as_antic_waitreq
 */
static void as_antic_waitnext(struct as_data *ad)
{
	unsigned long timeout;

	BUG_ON(ad->antic_status != ANTIC_OFF
			&& ad->antic_status != ANTIC_WAIT_REQ);

	timeout = ad->antic_start + ad->antic_expire;

	mod_timer(&ad->antic_timer, timeout);

	ad->antic_status = ANTIC_WAIT_NEXT;
}

/*
 * as_antic_waitreq starts anticipating. We don't start timing the anticipation
 * until the request that we're anticipating on has finished. This means we
 * are timing from when the candidate process wakes up hopefully.
 */
static void as_antic_waitreq(struct as_data *ad)
{
	BUG_ON(ad->antic_status == ANTIC_FINISHED);
	if (ad->antic_status == ANTIC_OFF) {
		if (!ad->io_context || ad->ioc_finished)
			as_antic_waitnext(ad);
		else
			ad->antic_status = ANTIC_WAIT_REQ;
	}
}

/*
 * This is called directly by the functions in this file to stop anticipation.
 * We kill the timer and schedule a call to the request_fn asap.
 */
static void as_antic_stop(struct as_data *ad)
{
	int status = ad->antic_status;

	if (status == ANTIC_WAIT_REQ || status == ANTIC_WAIT_NEXT) {
		if (status == ANTIC_WAIT_NEXT)
			del_timer(&ad->antic_timer);
		ad->antic_status = ANTIC_FINISHED;
		/* see as_work_handler */
		kblockd_schedule_work(&ad->antic_work);
	}
}

/*
 * as_antic_timeout is the timer function set by as_antic_waitnext.
 */
static void as_antic_timeout(unsigned long data)
{
	struct request_queue *q = (struct request_queue *)data;
	struct as_data *ad = q->elevator.elevator_data;
	unsigned long flags;

	spin_lock_irqsave(q->queue_lock, flags);
	if (ad->antic_status == ANTIC_WAIT_REQ
			|| ad->antic_status == ANTIC_WAIT_NEXT) {
		struct as_io_context *aic = ad->io_context->aic;

		ad->antic_status = ANTIC_FINISHED;
		kblockd_schedule_work(&ad->antic_work);

		if (aic->ttime_samples == 0) {
			/* process anticipated on has exitted or timed out*/
			ad->exit_prob = (7*ad->exit_prob + 256)/8;
		}
	}
	spin_unlock_irqrestore(q->queue_lock, flags);
}

/*
 * as_close_req decides if one request is considered "close" to the
 * previous one issued.
 */
static int as_close_req(struct as_data *ad, struct as_rq *arq)
{
	unsigned long delay;	/* milliseconds */
	sector_t last = ad->last_sector[ad->batch_data_dir];
	sector_t next = arq->request->sector;
	sector_t delta; /* acceptable close offset (in sectors) */

	if (ad->antic_status == ANTIC_OFF || !ad->ioc_finished)
		delay = 0;
	else
		delay = ((jiffies - ad->antic_start) * 1000) / HZ;

	if (delay <= 1)
		delta = 64;
	else if (delay <= 20 && delay <= ad->antic_expire)
		delta = 64 << (delay-1);
	else
		return 1;

	return (last - (delta>>1) <= next) && (next <= last + delta);
}

/*
 * as_can_break_anticipation returns true if we have been anticipating this
 * request.
 *
 * It also returns true if the process against which we are anticipating
 * submits a write - that's presumably an fsync, O_SYNC write, etc. We want to
 * dispatch it ASAP, because we know that application will not be submitting
 * any new reads.
 *
 * If the task which has submitted the request has exitted, break anticipation.
 *
 * If this task has queued some other IO, do not enter enticipation.
 */
static int as_can_break_anticipation(struct as_data *ad, struct as_rq *arq)
{
	struct io_context *ioc;
	struct as_io_context *aic;
	sector_t s;

	ioc = ad->io_context;
	BUG_ON(!ioc);

	if (arq && ioc == arq->io_context) {
		/* request from same process */
		return 1;
	}

	if (ad->ioc_finished && as_antic_expired(ad)) {
		/*
		 * In this situation status should really be FINISHED,
		 * however the timer hasn't had the chance to run yet.
		 */
		return 1;
	}

	aic = ioc->aic;
	if (!aic)
		return 0;

	if (!test_bit(AS_TASK_RUNNING, &aic->state)) {
		/* process anticipated on has exitted */
		if (aic->ttime_samples == 0)
			ad->exit_prob = (7*ad->exit_prob + 256)/8;
		return 1;
	}

	if (atomic_read(&aic->nr_queued) > 0) {
		/* process has more requests queued */
		return 1;
	}

	if (atomic_read(&aic->nr_dispatched) > 0) {
		/* process has more requests dispatched */
		return 1;
	}

	if (arq && arq->is_sync == REQ_SYNC && as_close_req(ad, arq)) {
		/*
		 * Found a close request that is not one of ours.
		 *
		 * This makes close requests from another process reset
		 * our thinktime delay. Is generally useful when there are
		 * two or more cooperating processes working in the same
		 * area.
		 */
		spin_lock(&aic->lock);
		aic->last_end_request = jiffies;
		spin_unlock(&aic->lock);
		return 1;
	}


	if (aic->ttime_samples == 0) {
		if (ad->new_ttime_mean > ad->antic_expire)
			return 1;
		if (ad->exit_prob > 128)
			return 1;
	} else if (aic->ttime_mean > ad->antic_expire) {
		/* the process thinks too much between requests */
		return 1;
	}

	if (!arq)
		return 0;

	if (ad->last_sector[REQ_SYNC] < arq->request->sector)
		s = arq->request->sector - ad->last_sector[REQ_SYNC];
	else
		s = ad->last_sector[REQ_SYNC] - arq->request->sector;

	if (aic->seek_samples == 0) {
		/*
		 * Process has just started IO. Use past statistics to
		 * guage success possibility
		 */
		if (ad->new_seek_mean > s) {
			/* this request is better than what we're expecting */
			return 1;
		}

	} else {
		if (aic->seek_mean > s) {
			/* this request is better than what we're expecting */
			return 1;
		}
	}

	return 0;
}

/*
 * as_can_anticipate indicates weather we should either run arq
 * or keep anticipating a better request.
 */
static int as_can_anticipate(struct as_data *ad, struct as_rq *arq)
{
	if (!ad->io_context)
		/*
		 * Last request submitted was a write
		 */
		return 0;

	if (ad->antic_status == ANTIC_FINISHED)
		/*
		 * Don't restart if we have just finished. Run the next request
		 */
		return 0;

	if (as_can_break_anticipation(ad, arq))
		/*
		 * This request is a good candidate. Don't keep anticipating,
		 * run it.
		 */
		return 0;

	/*
	 * OK from here, we haven't finished, and don't have a decent request!
	 * Status is either ANTIC_OFF so start waiting,
	 * ANTIC_WAIT_REQ so continue waiting for request to finish
	 * or ANTIC_WAIT_NEXT so continue waiting for an acceptable request.
	 *
	 */

	return 1;
}

static void as_update_thinktime(struct as_data *ad, struct as_io_context *aic, unsigned long ttime)
{
	/* fixed point: 1.0 == 1<<8 */
	if (aic->ttime_samples == 0) {
		ad->new_ttime_total = (7*ad->new_ttime_total + 256*ttime) / 8;
		ad->new_ttime_mean = ad->new_ttime_total / 256;

		ad->exit_prob = (7*ad->exit_prob)/8;
	}
	aic->ttime_samples = (7*aic->ttime_samples + 256) / 8;
	aic->ttime_total = (7*aic->ttime_total + 256*ttime) / 8;
	aic->ttime_mean = (aic->ttime_total + 128) / aic->ttime_samples;
}

static void as_update_seekdist(struct as_data *ad, struct as_io_context *aic, sector_t sdist)
{
	u64 total;

	if (aic->seek_samples == 0) {
		ad->new_seek_total = (7*ad->new_seek_total + 256*(u64)sdist)/8;
		ad->new_seek_mean = ad->new_seek_total / 256;
	}

	/*
	 * Don't allow the seek distance to get too large from the
	 * odd fragment, pagein, etc
	 */
	if (aic->seek_samples <= 60) /* second&third seek */
		sdist = min(sdist, (aic->seek_mean * 4) + 2*1024*1024);
	else
		sdist = min(sdist, (aic->seek_mean * 4)	+ 2*1024*64);

	aic->seek_samples = (7*aic->seek_samples + 256) / 8;
	aic->seek_total = (7*aic->seek_total + (u64)256*sdist) / 8;
	total = aic->seek_total + (aic->seek_samples/2);
	do_div(total, aic->seek_samples);
	aic->seek_mean = (sector_t)total;
}

/*
 * as_update_iohist keeps a decaying histogram of IO thinktimes, and
 * updates @aic->ttime_mean based on that. It is called when a new
 * request is queued.
 */
static void as_update_iohist(struct as_data *ad, struct as_io_context *aic, struct request *rq)
{
	struct as_rq *arq = RQ_DATA(rq);
	int data_dir = arq->is_sync;
	unsigned long thinktime;
	sector_t seek_dist;

	if (aic == NULL)
		return;

	if (data_dir == REQ_SYNC) {
		unsigned long in_flight = atomic_read(&aic->nr_queued)
					+ atomic_read(&aic->nr_dispatched);
		spin_lock(&aic->lock);
		if (test_bit(AS_TASK_IORUNNING, &aic->state) ||
			test_bit(AS_TASK_IOSTARTED, &aic->state)) {
			/* Calculate read -> read thinktime */
			if (test_bit(AS_TASK_IORUNNING, &aic->state)
							&& in_flight == 0) {
				thinktime = jiffies - aic->last_end_request;
				thinktime = min(thinktime, MAX_THINKTIME-1);
			} else
				thinktime = 0;
			as_update_thinktime(ad, aic, thinktime);

			/* Calculate read -> read seek distance */
			if (aic->last_request_pos < rq->sector)
				seek_dist = rq->sector - aic->last_request_pos;
			else
				seek_dist = aic->last_request_pos - rq->sector;
			as_update_seekdist(ad, aic, seek_dist);
		}
		aic->last_request_pos = rq->sector + rq->nr_sectors;
		set_bit(AS_TASK_IOSTARTED, &aic->state);
		spin_unlock(&aic->lock);
	}
}

/*
 * as_update_arq must be called whenever a request (arq) is added to
 * the sort_list. This function keeps caches up to date, and checks if the
 * request might be one we are "anticipating"
 */
static void as_update_arq(struct as_data *ad, struct as_rq *arq)
{
	const int data_dir = arq->is_sync;

	/* keep the next_arq cache up to date */
	ad->next_arq[data_dir] = as_choose_req(ad, arq, ad->next_arq[data_dir]);

	/*
	 * have we been anticipating this request?
	 * or does it come from the same process as the one we are anticipating
	 * for?
	 */
	if (ad->antic_status == ANTIC_WAIT_REQ
			|| ad->antic_status == ANTIC_WAIT_NEXT) {
		if (as_can_break_anticipation(ad, arq))
			as_antic_stop(ad);
	}
}

/*
 * Gathers timings and resizes the write batch automatically
 */
static void update_write_batch(struct as_data *ad)
{
	unsigned long batch = ad->batch_expire[REQ_ASYNC];
	long write_time;

	write_time = (jiffies - ad->current_batch_expires) + batch;
	if (write_time < 0)
		write_time = 0;

	if (write_time > batch && !ad->write_batch_idled) {
		if (write_time > batch * 3)
			ad->write_batch_count /= 2;
		else
			ad->write_batch_count--;
	} else if (write_time < batch && ad->current_write_count == 0) {
		if (batch > write_time * 3)
			ad->write_batch_count *= 2;
		else
			ad->write_batch_count++;
	}

	if (ad->write_batch_count < 1)
		ad->write_batch_count = 1;
}

/*
 * as_completed_request is to be called when a request has completed and
 * returned something to the requesting process, be it an error or data.
 */
static void as_completed_request(request_queue_t *q, struct request *rq)
{
	struct as_data *ad = q->elevator.elevator_data;
	struct as_rq *arq = RQ_DATA(rq);

	WARN_ON(!list_empty(&rq->queuelist));

	if (arq->state == AS_RQ_PRESCHED) {
		WARN_ON(arq->io_context);
		goto out;
	}

	if (arq->state == AS_RQ_MERGED)
		goto out_ioc;

	if (arq->state != AS_RQ_REMOVED) {
		printk("arq->state %d\n", arq->state);
		WARN_ON(1);
		goto out;
	}

	if (!blk_fs_request(rq))
		goto out;

	if (ad->changed_batch && ad->nr_dispatched == 1) {
		kblockd_schedule_work(&ad->antic_work);
		ad->changed_batch = 0;

		if (ad->batch_data_dir == REQ_SYNC)
			ad->new_batch = 1;
	}
	WARN_ON(ad->nr_dispatched == 0);
	ad->nr_dispatched--;

	/*
	 * Start counting the batch from when a request of that direction is
	 * actually serviced. This should help devices with big TCQ windows
	 * and writeback caches
	 */
	if (ad->new_batch && ad->batch_data_dir == arq->is_sync) {
		update_write_batch(ad);
		ad->current_batch_expires = jiffies +
				ad->batch_expire[REQ_SYNC];
		ad->new_batch = 0;
	}

	if (ad->io_context == arq->io_context && ad->io_context) {
		ad->antic_start = jiffies;
		ad->ioc_finished = 1;
		if (ad->antic_status == ANTIC_WAIT_REQ) {
			/*
			 * We were waiting on this request, now anticipate
			 * the next one
			 */
			as_antic_waitnext(ad);
		}
	}

out_ioc:
	if (!arq->io_context)
		goto out;

	if (arq->is_sync == REQ_SYNC) {
		struct as_io_context *aic = arq->io_context->aic;
		if (aic) {
			spin_lock(&aic->lock);
			set_bit(AS_TASK_IORUNNING, &aic->state);
			aic->last_end_request = jiffies;
			spin_unlock(&aic->lock);
		}
	}

	put_io_context(arq->io_context);
out:
	arq->state = AS_RQ_POSTSCHED;
}

/*
 * as_remove_queued_request removes a request from the pre dispatch queue
 * without updating refcounts. It is expected the caller will drop the
 * reference unless it replaces the request at somepart of the elevator
 * (ie. the dispatch queue)
 */
static void as_remove_queued_request(request_queue_t *q, struct request *rq)
{
	struct as_rq *arq = RQ_DATA(rq);
	const int data_dir = arq->is_sync;
	struct as_data *ad = q->elevator.elevator_data;

	WARN_ON(arq->state != AS_RQ_QUEUED);

	if (arq->io_context && arq->io_context->aic) {
		BUG_ON(!atomic_read(&arq->io_context->aic->nr_queued));
		atomic_dec(&arq->io_context->aic->nr_queued);
	}

	/*
	 * Update the "next_arq" cache if we are about to remove its
	 * entry
	 */
	if (ad->next_arq[data_dir] == arq)
		ad->next_arq[data_dir] = as_find_next_arq(ad, arq);

	list_del_init(&arq->fifo);
	as_remove_merge_hints(q, arq);
	as_del_arq_rb(ad, arq);
}

/*
 * as_remove_dispatched_request is called to remove a request which has gone
 * to the dispatch list.
 */
static void as_remove_dispatched_request(request_queue_t *q, struct request *rq)
{
	struct as_rq *arq = RQ_DATA(rq);
	struct as_io_context *aic;

	if (!arq) {
		WARN_ON(1);
		return;
	}

	WARN_ON(arq->state != AS_RQ_DISPATCHED);
	WARN_ON(ON_RB(&arq->rb_node));
	if (arq->io_context && arq->io_context->aic) {
		aic = arq->io_context->aic;
		if (aic) {
			WARN_ON(!atomic_read(&aic->nr_dispatched));
			atomic_dec(&aic->nr_dispatched);
		}
	}
}

/*
 * as_remove_request is called when a driver has finished with a request.
 * This should be only called for dispatched requests, but for some reason
 * a POWER4 box running hwscan it does not.
 */
static void as_remove_request(request_queue_t *q, struct request *rq)
{
	struct as_rq *arq = RQ_DATA(rq);

	if (unlikely(arq->state == AS_RQ_NEW))
		goto out;

	if (ON_RB(&arq->rb_node)) {
		if (arq->state != AS_RQ_QUEUED) {
			printk("arq->state %d\n", arq->state);
			WARN_ON(1);
			goto out;
		}
		/*
		 * We'll lose the aliased request(s) here. I don't think this
		 * will ever happen, but if it does, hopefully someone will
		 * report it.
		 */
		WARN_ON(!list_empty(&rq->queuelist));
		as_remove_queued_request(q, rq);
	} else {
		if (arq->state != AS_RQ_DISPATCHED) {
			printk("arq->state %d\n", arq->state);
			WARN_ON(1);
			goto out;
		}
		as_remove_dispatched_request(q, rq);
	}
out:
	arq->state = AS_RQ_REMOVED;
}

/*
 * as_fifo_expired returns 0 if there are no expired reads on the fifo,
 * 1 otherwise.  It is ratelimited so that we only perform the check once per
 * `fifo_expire' interval.  Otherwise a large number of expired requests
 * would create a hopeless seekstorm.
 *
 * See as_antic_expired comment.
 */
static int as_fifo_expired(struct as_data *ad, int adir)
{
	struct as_rq *arq;
	long delta_jif;

	delta_jif = jiffies - ad->last_check_fifo[adir];
	if (unlikely(delta_jif < 0))
		delta_jif = -delta_jif;
	if (delta_jif < ad->fifo_expire[adir])
		return 0;

	ad->last_check_fifo[adir] = jiffies;

	if (list_empty(&ad->fifo_list[adir]))
		return 0;

	arq = list_entry_fifo(ad->fifo_list[adir].next);

	return time_after(jiffies, arq->expires);
}

/*
 * as_batch_expired returns true if the current batch has expired. A batch
 * is a set of reads or a set of writes.
 */
static inline int as_batch_expired(struct as_data *ad)
{
	if (ad->changed_batch || ad->new_batch)
		return 0;

	if (ad->batch_data_dir == REQ_SYNC)
		/* TODO! add a check so a complete fifo gets written? */
		return time_after(jiffies, ad->current_batch_expires);

	return time_after(jiffies, ad->current_batch_expires)
		|| ad->current_write_count == 0;
}

/*
 * move an entry to dispatch queue
 */
static void as_move_to_dispatch(struct as_data *ad, struct as_rq *arq)
{
	struct request *rq = arq->request;
	struct list_head *insert;
	const int data_dir = arq->is_sync;

	BUG_ON(!ON_RB(&arq->rb_node));

	as_antic_stop(ad);
	ad->antic_status = ANTIC_OFF;

	/*
	 * This has to be set in order to be correctly updated by
	 * as_find_next_arq
	 */
	ad->last_sector[data_dir] = rq->sector + rq->nr_sectors;

	if (data_dir == REQ_SYNC) {
		/* In case we have to anticipate after this */
		copy_io_context(&ad->io_context, &arq->io_context);
	} else {
		if (ad->io_context) {
			put_io_context(ad->io_context);
			ad->io_context = NULL;
		}

		if (ad->current_write_count != 0)
			ad->current_write_count--;
	}
	ad->ioc_finished = 0;

	ad->next_arq[data_dir] = as_find_next_arq(ad, arq);

	/*
	 * take it off the sort and fifo list, add to dispatch queue
	 */
	insert = ad->dispatch->prev;

	while (!list_empty(&rq->queuelist)) {
		struct request *__rq = list_entry_rq(rq->queuelist.next);
		struct as_rq *__arq = RQ_DATA(__rq);

		list_move_tail(&__rq->queuelist, ad->dispatch);

		if (__arq->io_context && __arq->io_context->aic)
			atomic_inc(&__arq->io_context->aic->nr_dispatched);

		WARN_ON(__arq->state != AS_RQ_QUEUED);
		__arq->state = AS_RQ_DISPATCHED;

		ad->nr_dispatched++;
	}

	as_remove_queued_request(ad->q, rq);
	WARN_ON(arq->state != AS_RQ_QUEUED);

	list_add(&rq->queuelist, insert);
	arq->state = AS_RQ_DISPATCHED;
	if (arq->io_context && arq->io_context->aic)
		atomic_inc(&arq->io_context->aic->nr_dispatched);
	ad->nr_dispatched++;
}

/*
 * as_dispatch_request selects the best request according to
 * read/write expire, batch expire, etc, and moves it to the dispatch
 * queue. Returns 1 if a request was found, 0 otherwise.
 */
static int as_dispatch_request(struct as_data *ad)
{
	struct as_rq *arq;
	const int reads = !list_empty(&ad->fifo_list[REQ_SYNC]);
	const int writes = !list_empty(&ad->fifo_list[REQ_ASYNC]);

	/* Signal that the write batch was uncontended, so we can't time it */
	if (ad->batch_data_dir == REQ_ASYNC && !reads) {
		if (ad->current_write_count == 0 || !writes)
			ad->write_batch_idled = 1;
	}

	if (!(reads || writes)
		|| ad->antic_status == ANTIC_WAIT_REQ
		|| ad->antic_status == ANTIC_WAIT_NEXT
		|| ad->changed_batch)
		return 0;

	if (!(reads && writes && as_batch_expired(ad)) ) {
		/*
		 * batch is still running or no reads or no writes
		 */
		arq = ad->next_arq[ad->batch_data_dir];

		if (ad->batch_data_dir == REQ_SYNC && ad->antic_expire) {
			if (as_fifo_expired(ad, REQ_SYNC))
				goto fifo_expired;

			if (as_can_anticipate(ad, arq)) {
				as_antic_waitreq(ad);
				return 0;
			}
		}

		if (arq) {
			/* we have a "next request" */
			if (reads && !writes)
				ad->current_batch_expires =
					jiffies + ad->batch_expire[REQ_SYNC];
			goto dispatch_request;
		}
	}

	/*
	 * at this point we are not running a batch. select the appropriate
	 * data direction (read / write)
	 */

	if (reads) {
		BUG_ON(RB_EMPTY(&ad->sort_list[REQ_SYNC]));

		if (writes && ad->batch_data_dir == REQ_SYNC)
			/*
			 * Last batch was a read, switch to writes
			 */
			goto dispatch_writes;

		if (ad->batch_data_dir == REQ_ASYNC) {
			WARN_ON(ad->new_batch);
			ad->changed_batch = 1;
		}
		ad->batch_data_dir = REQ_SYNC;
		arq = list_entry_fifo(ad->fifo_list[ad->batch_data_dir].next);
		ad->last_check_fifo[ad->batch_data_dir] = jiffies;
		goto dispatch_request;
	}

	/*
	 * the last batch was a read
	 */

	if (writes) {
dispatch_writes:
		BUG_ON(RB_EMPTY(&ad->sort_list[REQ_ASYNC]));

		if (ad->batch_data_dir == REQ_SYNC) {
			ad->changed_batch = 1;

			/*
			 * new_batch might be 1 when the queue runs out of
			 * reads. A subsequent submission of a write might
			 * cause a change of batch before the read is finished.
			 */
			ad->new_batch = 0;
		}
		ad->batch_data_dir = REQ_ASYNC;
		ad->current_write_count = ad->write_batch_count;
		ad->write_batch_idled = 0;
		arq = ad->next_arq[ad->batch_data_dir];
		goto dispatch_request;
	}

	BUG();
	return 0;

dispatch_request:
	/*
	 * If a request has expired, service it.
	 */

	if (as_fifo_expired(ad, ad->batch_data_dir)) {
fifo_expired:
		arq = list_entry_fifo(ad->fifo_list[ad->batch_data_dir].next);
		BUG_ON(arq == NULL);
	}

	if (ad->changed_batch) {
		WARN_ON(ad->new_batch);

		if (ad->nr_dispatched)
			return 0;

		if (ad->batch_data_dir == REQ_ASYNC)
			ad->current_batch_expires = jiffies +
					ad->batch_expire[REQ_ASYNC];
		else
			ad->new_batch = 1;

		ad->changed_batch = 0;
	}

	/*
	 * arq is the selected appropriate request.
	 */
	as_move_to_dispatch(ad, arq);

	return 1;
}

static struct request *as_next_request(request_queue_t *q)
{
	struct as_data *ad = q->elevator.elevator_data;
	struct request *rq = NULL;

	/*
	 * if there are still requests on the dispatch queue, grab the first
	 */
	if (!list_empty(ad->dispatch) || as_dispatch_request(ad))
		rq = list_entry_rq(ad->dispatch->next);

	return rq;
}

/*
 * Add arq to a list behind alias
 */
static inline void
as_add_aliased_request(struct as_data *ad, struct as_rq *arq, struct as_rq *alias)
{
	struct request  *req = arq->request;
	struct list_head *insert = alias->request->queuelist.prev;

	/*
	 * Transfer list of aliases
	 */
	while (!list_empty(&req->queuelist)) {
		struct request *__rq = list_entry_rq(req->queuelist.next);
		struct as_rq *__arq = RQ_DATA(__rq);

		list_move_tail(&__rq->queuelist, &alias->request->queuelist);

		WARN_ON(__arq->state != AS_RQ_QUEUED);
	}

	/*
	 * Another request with the same start sector on the rbtree.
	 * Link this request to that sector. They are untangled in
	 * as_move_to_dispatch
	 */
	list_add(&arq->request->queuelist, insert);

	/*
	 * Don't want to have to handle merges.
	 */
	as_remove_merge_hints(ad->q, arq);
}

/*
 * add arq to rbtree and fifo
 */
static void as_add_request(struct as_data *ad, struct as_rq *arq)
{
	struct as_rq *alias;
	int data_dir;

	if (rq_data_dir(arq->request) == READ
			|| current->flags&PF_SYNCWRITE)
		arq->is_sync = 1;
	else
		arq->is_sync = 0;
	data_dir = arq->is_sync;

	arq->io_context = as_get_io_context();

	if (arq->io_context) {
		as_update_iohist(ad, arq->io_context->aic, arq->request);
		atomic_inc(&arq->io_context->aic->nr_queued);
	}

	alias = as_add_arq_rb(ad, arq);
	if (!alias) {
		/*
		 * set expire time (only used for reads) and add to fifo list
		 */
		arq->expires = jiffies + ad->fifo_expire[data_dir];
		list_add_tail(&arq->fifo, &ad->fifo_list[data_dir]);

		if (rq_mergeable(arq->request)) {
			as_add_arq_hash(ad, arq);

			if (!ad->q->last_merge)
				ad->q->last_merge = arq->request;
		}
		as_update_arq(ad, arq); /* keep state machine up to date */

	} else {
		as_add_aliased_request(ad, arq, alias);

		/*
		 * have we been anticipating this request?
		 * or does it come from the same process as the one we are
		 * anticipating for?
		 */
		if (ad->antic_status == ANTIC_WAIT_REQ
				|| ad->antic_status == ANTIC_WAIT_NEXT) {
			if (as_can_break_anticipation(ad, arq))
				as_antic_stop(ad);
		}
	}

	arq->state = AS_RQ_QUEUED;
}

/*
 * requeue the request. The request has not been completed, nor is it a
 * new request, so don't touch accounting.
 */
static void as_requeue_request(request_queue_t *q, struct request *rq)
{
	struct as_data *ad = q->elevator.elevator_data;
	struct as_rq *arq = RQ_DATA(rq);

	if (arq) {
		if (arq->state != AS_RQ_REMOVED) {
			printk("arq->state %d\n", arq->state);
			WARN_ON(1);
		}

		arq->state = AS_RQ_DISPATCHED;
		if (arq->io_context && arq->io_context->aic)
			atomic_inc(&arq->io_context->aic->nr_dispatched);
	} else
		WARN_ON(blk_fs_request(rq)
			&& (!(rq->flags & (REQ_HARDBARRIER|REQ_SOFTBARRIER))) );

	list_add(&rq->queuelist, ad->dispatch);

	/* Stop anticipating - let this request get through */
	as_antic_stop(ad);
}

/*
 * Account a request that is inserted directly onto the dispatch queue.
 * arq->io_context->aic->nr_dispatched should not need to be incremented
 * because only new requests should come through here: requeues go through
 * our explicit requeue handler.
 */
static void as_account_queued_request(struct as_data *ad, struct request *rq)
{
	if (blk_fs_request(rq)) {
		struct as_rq *arq = RQ_DATA(rq);
		arq->state = AS_RQ_DISPATCHED;
		ad->nr_dispatched++;
	}
}

static void
as_insert_request(request_queue_t *q, struct request *rq, int where)
{
	struct as_data *ad = q->elevator.elevator_data;
	struct as_rq *arq = RQ_DATA(rq);

	if (arq) {
		if (arq->state != AS_RQ_PRESCHED) {
			printk("arq->state: %d\n", arq->state);
			WARN_ON(1);
		}
		arq->state = AS_RQ_NEW;
	}

	/* barriers must flush the reorder queue */
	if (unlikely(rq->flags & (REQ_SOFTBARRIER | REQ_HARDBARRIER)
			&& where == ELEVATOR_INSERT_SORT)) {
		WARN_ON(1);
		where = ELEVATOR_INSERT_BACK;
	}

	switch (where) {
		case ELEVATOR_INSERT_BACK:
			while (ad->next_arq[REQ_SYNC])
				as_move_to_dispatch(ad, ad->next_arq[REQ_SYNC]);

			while (ad->next_arq[REQ_ASYNC])
				as_move_to_dispatch(ad, ad->next_arq[REQ_ASYNC]);

			list_add_tail(&rq->queuelist, ad->dispatch);
			as_account_queued_request(ad, rq);
			as_antic_stop(ad);
			break;
		case ELEVATOR_INSERT_FRONT:
			list_add(&rq->queuelist, ad->dispatch);
			as_account_queued_request(ad, rq);
			as_antic_stop(ad);
			break;
		case ELEVATOR_INSERT_SORT:
			BUG_ON(!blk_fs_request(rq));
			as_add_request(ad, arq);
			break;
		default:
			BUG();
			return;
	}
}

/*
 * as_queue_empty tells us if there are requests left in the device. It may
 * not be the case that a driver can get the next request even if the queue
 * is not empty - it is used in the block layer to check for plugging and
 * merging opportunities
 */
static int as_queue_empty(request_queue_t *q)
{
	struct as_data *ad = q->elevator.elevator_data;

	if (!list_empty(&ad->fifo_list[REQ_ASYNC])
		|| !list_empty(&ad->fifo_list[REQ_SYNC])
		|| !list_empty(ad->dispatch))
			return 0;

	return 1;
}

static struct request *
as_former_request(request_queue_t *q, struct request *rq)
{
	struct as_rq *arq = RQ_DATA(rq);
	struct rb_node *rbprev = rb_prev(&arq->rb_node);
	struct request *ret = NULL;

	if (rbprev)
		ret = rb_entry_arq(rbprev)->request;

	return ret;
}

static struct request *
as_latter_request(request_queue_t *q, struct request *rq)
{
	struct as_rq *arq = RQ_DATA(rq);
	struct rb_node *rbnext = rb_next(&arq->rb_node);
	struct request *ret = NULL;

	if (rbnext)
		ret = rb_entry_arq(rbnext)->request;

	return ret;
}

static int
as_merge(request_queue_t *q, struct request **req, struct bio *bio)
{
	struct as_data *ad = q->elevator.elevator_data;
	sector_t rb_key = bio->bi_sector + bio_sectors(bio);
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
	__rq = as_find_arq_hash(ad, bio->bi_sector);
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
	__rq = as_find_arq_rb(ad, rb_key, bio_data_dir(bio));
	if (__rq) {
		BUG_ON(rb_key != rq_rb_key(__rq));

		if (elv_rq_merge_ok(__rq, bio)) {
			ret = ELEVATOR_FRONT_MERGE;
			goto out;
		}
	}

	return ELEVATOR_NO_MERGE;
out:
	if (rq_mergeable(__rq))
		q->last_merge = __rq;
out_insert:
	if (ret) {
		if (rq_mergeable(__rq))
			as_hot_arq_hash(ad, RQ_DATA(__rq));
	}
	*req = __rq;
	return ret;
}

static void as_merged_request(request_queue_t *q, struct request *req)
{
	struct as_data *ad = q->elevator.elevator_data;
	struct as_rq *arq = RQ_DATA(req);

	/*
	 * hash always needs to be repositioned, key is end sector
	 */
	as_del_arq_hash(arq);
	as_add_arq_hash(ad, arq);

	/*
	 * if the merge was a front merge, we need to reposition request
	 */
	if (rq_rb_key(req) != arq->rb_key) {
		struct as_rq *alias, *next_arq = NULL;

		if (ad->next_arq[arq->is_sync] == arq)
			next_arq = as_find_next_arq(ad, arq);

		/*
		 * Note! We should really be moving any old aliased requests
		 * off this request and try to insert them into the rbtree. We
		 * currently don't bother. Ditto the next function.
		 */
		as_del_arq_rb(ad, arq);
		if ((alias = as_add_arq_rb(ad, arq)) ) {
			list_del_init(&arq->fifo);
			as_add_aliased_request(ad, arq, alias);
			if (next_arq)
				ad->next_arq[arq->is_sync] = next_arq;
		}
		/*
		 * Note! At this stage of this and the next function, our next
		 * request may not be optimal - eg the request may have "grown"
		 * behind the disk head. We currently don't bother adjusting.
		 */
	}

	if (arq->on_hash)
		q->last_merge = req;
}

static void
as_merged_requests(request_queue_t *q, struct request *req,
			 struct request *next)
{
	struct as_data *ad = q->elevator.elevator_data;
	struct as_rq *arq = RQ_DATA(req);
	struct as_rq *anext = RQ_DATA(next);

	BUG_ON(!arq);
	BUG_ON(!anext);

	/*
	 * reposition arq (this is the merged request) in hash, and in rbtree
	 * in case of a front merge
	 */
	as_del_arq_hash(arq);
	as_add_arq_hash(ad, arq);

	if (rq_rb_key(req) != arq->rb_key) {
		struct as_rq *alias, *next_arq = NULL;

		if (ad->next_arq[arq->is_sync] == arq)
			next_arq = as_find_next_arq(ad, arq);

		as_del_arq_rb(ad, arq);
		if ((alias = as_add_arq_rb(ad, arq)) ) {
			list_del_init(&arq->fifo);
			as_add_aliased_request(ad, arq, alias);
			if (next_arq)
				ad->next_arq[arq->is_sync] = next_arq;
		}
	}

	/*
	 * if anext expires before arq, assign its expire time to arq
	 * and move into anext position (anext will be deleted) in fifo
	 */
	if (!list_empty(&arq->fifo) && !list_empty(&anext->fifo)) {
		if (time_before(anext->expires, arq->expires)) {
			list_move(&arq->fifo, &anext->fifo);
			arq->expires = anext->expires;
			/*
			 * Don't copy here but swap, because when anext is
			 * removed below, it must contain the unused context
			 */
			swap_io_context(&arq->io_context, &anext->io_context);
		}
	}

	/*
	 * Transfer list of aliases
	 */
	while (!list_empty(&next->queuelist)) {
		struct request *__rq = list_entry_rq(next->queuelist.next);
		struct as_rq *__arq = RQ_DATA(__rq);

		list_move_tail(&__rq->queuelist, &req->queuelist);

		WARN_ON(__arq->state != AS_RQ_QUEUED);
	}

	/*
	 * kill knowledge of next, this one is a goner
	 */
	as_remove_queued_request(q, next);

	anext->state = AS_RQ_MERGED;
}

/*
 * This is executed in a "deferred" process context, by kblockd. It calls the
 * driver's request_fn so the driver can submit that request.
 *
 * IMPORTANT! This guy will reenter the elevator, so set up all queue global
 * state before calling, and don't rely on any state over calls.
 *
 * FIXME! dispatch queue is not a queue at all!
 */
static void as_work_handler(void *data)
{
	struct request_queue *q = data;
	unsigned long flags;

	spin_lock_irqsave(q->queue_lock, flags);
	if (as_next_request(q))
		q->request_fn(q);
	spin_unlock_irqrestore(q->queue_lock, flags);
}

static void as_put_request(request_queue_t *q, struct request *rq)
{
	struct as_data *ad = q->elevator.elevator_data;
	struct as_rq *arq = RQ_DATA(rq);

	if (!arq) {
		WARN_ON(1);
		return;
	}

	if (arq->state != AS_RQ_POSTSCHED && arq->state != AS_RQ_PRESCHED) {
		printk("arq->state %d\n", arq->state);
		WARN_ON(1);
	}

	mempool_free(arq, ad->arq_pool);
	rq->elevator_private = NULL;
}

static int as_set_request(request_queue_t *q, struct request *rq, int gfp_mask)
{
	struct as_data *ad = q->elevator.elevator_data;
	struct as_rq *arq = mempool_alloc(ad->arq_pool, gfp_mask);

	if (arq) {
		memset(arq, 0, sizeof(*arq));
		RB_CLEAR(&arq->rb_node);
		arq->request = rq;
		arq->state = AS_RQ_PRESCHED;
		arq->io_context = NULL;
		INIT_LIST_HEAD(&arq->hash);
		arq->on_hash = 0;
		INIT_LIST_HEAD(&arq->fifo);
		rq->elevator_private = arq;
		return 0;
	}

	return 1;
}

static int as_may_queue(request_queue_t *q, int rw)
{
	int ret = 0;
	struct as_data *ad = q->elevator.elevator_data;
	struct io_context *ioc;
	if (ad->antic_status == ANTIC_WAIT_REQ ||
			ad->antic_status == ANTIC_WAIT_NEXT) {
		ioc = as_get_io_context();
		if (ad->io_context == ioc)
			ret = 1;
		put_io_context(ioc);
	}

	return ret;
}

static void as_exit(request_queue_t *q, elevator_t *e)
{
	struct as_data *ad = e->elevator_data;

	del_timer_sync(&ad->antic_timer);
	kblockd_flush();

	BUG_ON(!list_empty(&ad->fifo_list[REQ_SYNC]));
	BUG_ON(!list_empty(&ad->fifo_list[REQ_ASYNC]));

	mempool_destroy(ad->arq_pool);
	put_io_context(ad->io_context);
	kfree(ad->hash);
	kfree(ad);
}

/*
 * initialize elevator private data (as_data), and alloc a arq for
 * each request on the free lists
 */
static int as_init(request_queue_t *q, elevator_t *e)
{
	struct as_data *ad;
	int i;

	if (!arq_pool)
		return -ENOMEM;

	ad = kmalloc(sizeof(*ad), GFP_KERNEL);
	if (!ad)
		return -ENOMEM;
	memset(ad, 0, sizeof(*ad));

	ad->q = q; /* Identify what queue the data belongs to */

	ad->hash = kmalloc(sizeof(struct list_head)*AS_HASH_ENTRIES,GFP_KERNEL);
	if (!ad->hash) {
		kfree(ad);
		return -ENOMEM;
	}

	ad->arq_pool = mempool_create(BLKDEV_MIN_RQ, mempool_alloc_slab, mempool_free_slab, arq_pool);
	if (!ad->arq_pool) {
		kfree(ad->hash);
		kfree(ad);
		return -ENOMEM;
	}

	/* anticipatory scheduling helpers */
	ad->antic_timer.function = as_antic_timeout;
	ad->antic_timer.data = (unsigned long)q;
	init_timer(&ad->antic_timer);
	INIT_WORK(&ad->antic_work, as_work_handler, q);

	for (i = 0; i < AS_HASH_ENTRIES; i++)
		INIT_LIST_HEAD(&ad->hash[i]);

	INIT_LIST_HEAD(&ad->fifo_list[REQ_SYNC]);
	INIT_LIST_HEAD(&ad->fifo_list[REQ_ASYNC]);
	ad->sort_list[REQ_SYNC] = RB_ROOT;
	ad->sort_list[REQ_ASYNC] = RB_ROOT;
	ad->dispatch = &q->queue_head;
	ad->fifo_expire[REQ_SYNC] = default_read_expire;
	ad->fifo_expire[REQ_ASYNC] = default_write_expire;
	ad->antic_expire = default_antic_expire;
	ad->batch_expire[REQ_SYNC] = default_read_batch_expire;
	ad->batch_expire[REQ_ASYNC] = default_write_batch_expire;
	e->elevator_data = ad;

	ad->current_batch_expires = jiffies + ad->batch_expire[REQ_SYNC];
	ad->write_batch_count = ad->batch_expire[REQ_ASYNC] / 10;
	if (ad->write_batch_count < 2)
		ad->write_batch_count = 2;

	return 0;
}

/*
 * sysfs parts below
 */
struct as_fs_entry {
	struct attribute attr;
	ssize_t (*show)(struct as_data *, char *);
	ssize_t (*store)(struct as_data *, const char *, size_t);
};

static ssize_t
as_var_show(unsigned int var, char *page)
{
	var = (var * 1000) / HZ;
	return sprintf(page, "%d\n", var);
}

static ssize_t
as_var_store(unsigned long *var, const char *page, size_t count)
{
	unsigned long tmp;
	char *p = (char *) page;

	tmp = simple_strtoul(p, &p, 10);
	if (tmp != 0) {
		tmp = (tmp * HZ) / 1000;
		if (tmp == 0)
			tmp = 1;
	}
	*var = tmp;
	return count;
}

static ssize_t as_est_show(struct as_data *ad, char *page)
{
	int pos = 0;

	pos += sprintf(page+pos, "%lu %% exit probability\n", 100*ad->exit_prob/256);
	pos += sprintf(page+pos, "%lu ms new thinktime\n", ad->new_ttime_mean);
	pos += sprintf(page+pos, "%llu sectors new seek distance\n", (unsigned long long)ad->new_seek_mean);

	return pos;
}

#define SHOW_FUNCTION(__FUNC, __VAR)					\
static ssize_t __FUNC(struct as_data *ad, char *page)		\
{									\
	return as_var_show(__VAR, (page));			\
}
SHOW_FUNCTION(as_readexpire_show, ad->fifo_expire[REQ_SYNC]);
SHOW_FUNCTION(as_writeexpire_show, ad->fifo_expire[REQ_ASYNC]);
SHOW_FUNCTION(as_anticexpire_show, ad->antic_expire);
SHOW_FUNCTION(as_read_batchexpire_show, ad->batch_expire[REQ_SYNC]);
SHOW_FUNCTION(as_write_batchexpire_show, ad->batch_expire[REQ_ASYNC]);
#undef SHOW_FUNCTION

#define STORE_FUNCTION(__FUNC, __PTR, MIN, MAX)				\
static ssize_t __FUNC(struct as_data *ad, const char *page, size_t count)	\
{									\
	int ret = as_var_store(__PTR, (page), count);		\
	if (*(__PTR) < (MIN))						\
		*(__PTR) = (MIN);					\
	else if (*(__PTR) > (MAX))					\
		*(__PTR) = (MAX);					\
	return ret;							\
}
STORE_FUNCTION(as_readexpire_store, &ad->fifo_expire[REQ_SYNC], 0, INT_MAX);
STORE_FUNCTION(as_writeexpire_store, &ad->fifo_expire[REQ_ASYNC], 0, INT_MAX);
STORE_FUNCTION(as_anticexpire_store, &ad->antic_expire, 0, INT_MAX);
STORE_FUNCTION(as_read_batchexpire_store,
			&ad->batch_expire[REQ_SYNC], 0, INT_MAX);
STORE_FUNCTION(as_write_batchexpire_store,
			&ad->batch_expire[REQ_ASYNC], 0, INT_MAX);
#undef STORE_FUNCTION

static struct as_fs_entry as_est_entry = {
	.attr = {.name = "est_time", .mode = S_IRUGO },
	.show = as_est_show,
};
static struct as_fs_entry as_readexpire_entry = {
	.attr = {.name = "read_expire", .mode = S_IRUGO | S_IWUSR },
	.show = as_readexpire_show,
	.store = as_readexpire_store,
};
static struct as_fs_entry as_writeexpire_entry = {
	.attr = {.name = "write_expire", .mode = S_IRUGO | S_IWUSR },
	.show = as_writeexpire_show,
	.store = as_writeexpire_store,
};
static struct as_fs_entry as_anticexpire_entry = {
	.attr = {.name = "antic_expire", .mode = S_IRUGO | S_IWUSR },
	.show = as_anticexpire_show,
	.store = as_anticexpire_store,
};
static struct as_fs_entry as_read_batchexpire_entry = {
	.attr = {.name = "read_batch_expire", .mode = S_IRUGO | S_IWUSR },
	.show = as_read_batchexpire_show,
	.store = as_read_batchexpire_store,
};
static struct as_fs_entry as_write_batchexpire_entry = {
	.attr = {.name = "write_batch_expire", .mode = S_IRUGO | S_IWUSR },
	.show = as_write_batchexpire_show,
	.store = as_write_batchexpire_store,
};

static struct attribute *default_attrs[] = {
	&as_est_entry.attr,
	&as_readexpire_entry.attr,
	&as_writeexpire_entry.attr,
	&as_anticexpire_entry.attr,
	&as_read_batchexpire_entry.attr,
	&as_write_batchexpire_entry.attr,
	NULL,
};

#define to_as(atr) container_of((atr), struct as_fs_entry, attr)

static ssize_t
as_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	elevator_t *e = container_of(kobj, elevator_t, kobj);
	struct as_fs_entry *entry = to_as(attr);

	if (!entry->show)
		return 0;

	return entry->show(e->elevator_data, page);
}

static ssize_t
as_attr_store(struct kobject *kobj, struct attribute *attr,
		    const char *page, size_t length)
{
	elevator_t *e = container_of(kobj, elevator_t, kobj);
	struct as_fs_entry *entry = to_as(attr);

	if (!entry->store)
		return -EINVAL;

	return entry->store(e->elevator_data, page, length);
}

static struct sysfs_ops as_sysfs_ops = {
	.show	= as_attr_show,
	.store	= as_attr_store,
};

static struct kobj_type as_ktype = {
	.sysfs_ops	= &as_sysfs_ops,
	.default_attrs	= default_attrs,
};

static int __init as_slab_setup(void)
{
	arq_pool = kmem_cache_create("as_arq", sizeof(struct as_rq),
				     0, 0, NULL, NULL);

	if (!arq_pool)
		panic("as: can't init slab pool\n");

	return 0;
}

subsys_initcall(as_slab_setup);

elevator_t iosched_as = {
	.elevator_merge_fn = 		as_merge,
	.elevator_merged_fn =		as_merged_request,
	.elevator_merge_req_fn =	as_merged_requests,
	.elevator_next_req_fn =		as_next_request,
	.elevator_add_req_fn =		as_insert_request,
	.elevator_remove_req_fn =	as_remove_request,
	.elevator_requeue_req_fn = 	as_requeue_request,
	.elevator_queue_empty_fn =	as_queue_empty,
	.elevator_completed_req_fn =	as_completed_request,
	.elevator_former_req_fn =	as_former_request,
	.elevator_latter_req_fn =	as_latter_request,
	.elevator_set_req_fn =		as_set_request,
	.elevator_put_req_fn =		as_put_request,
	.elevator_may_queue_fn =	as_may_queue,
	.elevator_init_fn =		as_init,
	.elevator_exit_fn =		as_exit,

	.elevator_ktype =		&as_ktype,
	.elevator_name =		"anticipatory",
};

EXPORT_SYMBOL(iosched_as);
