/*
 * elevator noop
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

#include <asm/uaccess.h>

/*
 * See if we can find a request that this buffer can be coalesced with.
 */
int elevator_noop_merge(request_queue_t *q, struct request **req,
			struct bio *bio)
{
	struct list_head *entry = &q->queue_head;
	struct request *__rq;
	int ret;

	if ((ret = elv_try_last_merge(q, bio))) {
		*req = q->last_merge;
		return ret;
	}

	while ((entry = entry->prev) != &q->queue_head) {
		__rq = list_entry_rq(entry);

		if (__rq->flags & (REQ_SOFTBARRIER | REQ_HARDBARRIER))
			break;
		else if (__rq->flags & REQ_STARTED)
			break;

		if (!blk_fs_request(__rq))
			continue;

		if ((ret = elv_try_merge(__rq, bio))) {
			*req = __rq;
			q->last_merge = __rq;
			return ret;
		}
	}

	return ELEVATOR_NO_MERGE;
}

void elevator_noop_merge_requests(request_queue_t *q, struct request *req,
				  struct request *next)
{
	list_del_init(&next->queuelist);
}

void elevator_noop_add_request(request_queue_t *q, struct request *rq,
			       int where)
{
	struct list_head *insert = q->queue_head.prev;

	if (where == ELEVATOR_INSERT_FRONT)
		insert = &q->queue_head;

	list_add_tail(&rq->queuelist, &q->queue_head);

	/*
	 * new merges must not precede this barrier
	 */
	if (rq->flags & REQ_HARDBARRIER)
		q->last_merge = NULL;
	else if (!q->last_merge)
		q->last_merge = rq;
}

struct request *elevator_noop_next_request(request_queue_t *q)
{
	if (!list_empty(&q->queue_head))
		return list_entry_rq(q->queue_head.next);

	return NULL;
}

elevator_t elevator_noop = {
	.elevator_merge_fn		= elevator_noop_merge,
	.elevator_merge_req_fn		= elevator_noop_merge_requests,
	.elevator_next_req_fn		= elevator_noop_next_request,
	.elevator_add_req_fn		= elevator_noop_add_request,
	.elevator_name			= "noop",
};

EXPORT_SYMBOL(elevator_noop);
