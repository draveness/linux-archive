#ifndef _LINUX_ELEVATOR_H
#define _LINUX_ELEVATOR_H

typedef int (elevator_merge_fn) (request_queue_t *, struct request **,
				 struct bio *);

typedef void (elevator_merge_req_fn) (request_queue_t *, struct request *, struct request *);

typedef void (elevator_merged_fn) (request_queue_t *, struct request *);

typedef struct request *(elevator_next_req_fn) (request_queue_t *);

typedef void (elevator_add_req_fn) (request_queue_t *, struct request *, int);
typedef int (elevator_queue_empty_fn) (request_queue_t *);
typedef void (elevator_remove_req_fn) (request_queue_t *, struct request *);
typedef void (elevator_requeue_req_fn) (request_queue_t *, struct request *);
typedef struct request *(elevator_request_list_fn) (request_queue_t *, struct request *);
typedef void (elevator_completed_req_fn) (request_queue_t *, struct request *);
typedef int (elevator_may_queue_fn) (request_queue_t *, int);

typedef int (elevator_set_req_fn) (request_queue_t *, struct request *, int);
typedef void (elevator_put_req_fn) (request_queue_t *, struct request *);

typedef int (elevator_init_fn) (request_queue_t *, elevator_t *);
typedef void (elevator_exit_fn) (request_queue_t *, elevator_t *);

struct elevator_s
{
	elevator_merge_fn *elevator_merge_fn;
	elevator_merged_fn *elevator_merged_fn;
	elevator_merge_req_fn *elevator_merge_req_fn;

	elevator_next_req_fn *elevator_next_req_fn;
	elevator_add_req_fn *elevator_add_req_fn;
	elevator_remove_req_fn *elevator_remove_req_fn;
	elevator_requeue_req_fn *elevator_requeue_req_fn;

	elevator_queue_empty_fn *elevator_queue_empty_fn;
	elevator_completed_req_fn *elevator_completed_req_fn;

	elevator_request_list_fn *elevator_former_req_fn;
	elevator_request_list_fn *elevator_latter_req_fn;

	elevator_set_req_fn *elevator_set_req_fn;
	elevator_put_req_fn *elevator_put_req_fn;

	elevator_may_queue_fn *elevator_may_queue_fn;

	elevator_init_fn *elevator_init_fn;
	elevator_exit_fn *elevator_exit_fn;

	void *elevator_data;

	struct kobject kobj;
	struct kobj_type *elevator_ktype;
	const char *elevator_name;
};

/*
 * block elevator interface
 */
extern void elv_add_request(request_queue_t *, struct request *, int, int);
extern void __elv_add_request(request_queue_t *, struct request *, int, int);
extern int elv_merge(request_queue_t *, struct request **, struct bio *);
extern void elv_merge_requests(request_queue_t *, struct request *,
			       struct request *);
extern void elv_merged_request(request_queue_t *, struct request *);
extern void elv_remove_request(request_queue_t *, struct request *);
extern void elv_requeue_request(request_queue_t *, struct request *);
extern int elv_queue_empty(request_queue_t *);
extern struct request *elv_next_request(struct request_queue *q);
extern struct request *elv_former_request(request_queue_t *, struct request *);
extern struct request *elv_latter_request(request_queue_t *, struct request *);
extern int elv_register_queue(request_queue_t *q);
extern void elv_unregister_queue(request_queue_t *q);
extern int elv_may_queue(request_queue_t *, int);
extern void elv_completed_request(request_queue_t *, struct request *);
extern int elv_set_request(request_queue_t *, struct request *, int);
extern void elv_put_request(request_queue_t *, struct request *);

/*
 * noop I/O scheduler. always merges, always inserts new request at tail
 */
extern elevator_t elevator_noop;

/*
 * deadline i/o scheduler. uses request time outs to prevent indefinite
 * starvation
 */
extern elevator_t iosched_deadline;

/*
 * anticipatory I/O scheduler
 */
extern elevator_t iosched_as;

/*
 * completely fair queueing I/O scheduler
 */
extern elevator_t iosched_cfq;

extern int elevator_init(request_queue_t *, elevator_t *);
extern void elevator_exit(request_queue_t *);
extern int elv_rq_merge_ok(struct request *, struct bio *);
extern int elv_try_merge(struct request *, struct bio *);
extern int elv_try_last_merge(request_queue_t *, struct bio *);

/*
 * Return values from elevator merger
 */
#define ELEVATOR_NO_MERGE	0
#define ELEVATOR_FRONT_MERGE	1
#define ELEVATOR_BACK_MERGE	2

/*
 * Insertion selection
 */
#define ELEVATOR_INSERT_FRONT	1
#define ELEVATOR_INSERT_BACK	2
#define ELEVATOR_INSERT_SORT	3

#endif
