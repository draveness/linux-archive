/*
 * net/sched/sch_prio.c	Simple 3-band priority "scheduler".
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Authors:	Alexey Kuznetsov, <kuznet@ms2.inr.ac.ru>
 * Fixes:       19990609: J Hadi Salim <hadi@nortelnetworks.com>: 
 *              Init --  EINVAL when opt undefined
 */

#include <linux/config.h>
#include <linux/module.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/bitops.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/if_ether.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/notifier.h>
#include <net/ip.h>
#include <net/route.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/pkt_sched.h>


struct prio_sched_data
{
	int bands;
	struct tcf_proto *filter_list;
	u8  prio2band[TC_PRIO_MAX+1];
	struct Qdisc *queues[TCQ_PRIO_BANDS];
};


static __inline__ unsigned prio_classify(struct sk_buff *skb, struct Qdisc *sch)
{
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;
	struct tcf_result res;
	u32 band;

	band = skb->priority;
	if (TC_H_MAJ(skb->priority) != sch->handle) {
		if (!q->filter_list || tc_classify(skb, q->filter_list, &res)) {
			if (TC_H_MAJ(band))
				band = 0;
			return q->prio2band[band&TC_PRIO_MAX];
		}
		band = res.classid;
	}
	band = TC_H_MIN(band) - 1;
	return band < q->bands ? band : q->prio2band[0];
}

static int
prio_enqueue(struct sk_buff *skb, struct Qdisc* sch)
{
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;
	struct Qdisc *qdisc;
	int ret;

	qdisc = q->queues[prio_classify(skb, sch)];

	if ((ret = qdisc->enqueue(skb, qdisc)) == 0) {
		sch->stats.bytes += skb->len;
		sch->stats.packets++;
		sch->q.qlen++;
		return 0;
	}
	sch->stats.drops++;
	return ret;
}


static int
prio_requeue(struct sk_buff *skb, struct Qdisc* sch)
{
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;
	struct Qdisc *qdisc;
	int ret;

	qdisc = q->queues[prio_classify(skb, sch)];

	if ((ret = qdisc->ops->requeue(skb, qdisc)) == 0) {
		sch->q.qlen++;
		return 0;
	}
	sch->stats.drops++;
	return ret;
}


static struct sk_buff *
prio_dequeue(struct Qdisc* sch)
{
	struct sk_buff *skb;
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;
	int prio;
	struct Qdisc *qdisc;

	for (prio = 0; prio < q->bands; prio++) {
		qdisc = q->queues[prio];
		skb = qdisc->dequeue(qdisc);
		if (skb) {
			sch->q.qlen--;
			return skb;
		}
	}
	return NULL;

}

static int
prio_drop(struct Qdisc* sch)
{
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;
	int prio;
	struct Qdisc *qdisc;

	for (prio = q->bands-1; prio >= 0; prio--) {
		qdisc = q->queues[prio];
		if (qdisc->ops->drop(qdisc)) {
			sch->q.qlen--;
			return 1;
		}
	}
	return 0;
}


static void
prio_reset(struct Qdisc* sch)
{
	int prio;
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;

	for (prio=0; prio<q->bands; prio++)
		qdisc_reset(q->queues[prio]);
	sch->q.qlen = 0;
}

static void
prio_destroy(struct Qdisc* sch)
{
	int prio;
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;

	for (prio=0; prio<q->bands; prio++) {
		qdisc_destroy(q->queues[prio]);
		q->queues[prio] = &noop_qdisc;
	}
	MOD_DEC_USE_COUNT;
}

static int prio_tune(struct Qdisc *sch, struct rtattr *opt)
{
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;
	struct tc_prio_qopt *qopt = RTA_DATA(opt);
	int i;

	if (opt->rta_len < RTA_LENGTH(sizeof(*qopt)))
		return -EINVAL;
	if (qopt->bands > TCQ_PRIO_BANDS || qopt->bands < 2)
		return -EINVAL;

	for (i=0; i<=TC_PRIO_MAX; i++) {
		if (qopt->priomap[i] >= qopt->bands)
			return -EINVAL;
	}

	sch_tree_lock(sch);
	q->bands = qopt->bands;
	memcpy(q->prio2band, qopt->priomap, TC_PRIO_MAX+1);

	for (i=q->bands; i<TCQ_PRIO_BANDS; i++) {
		struct Qdisc *child = xchg(&q->queues[i], &noop_qdisc);
		if (child != &noop_qdisc)
			qdisc_destroy(child);
	}
	sch_tree_unlock(sch);

	for (i=0; i<=TC_PRIO_MAX; i++) {
		int band = q->prio2band[i];
		if (q->queues[band] == &noop_qdisc) {
			struct Qdisc *child;
			child = qdisc_create_dflt(sch->dev, &pfifo_qdisc_ops);
			if (child) {
				sch_tree_lock(sch);
				child = xchg(&q->queues[band], child);

				if (child != &noop_qdisc)
					qdisc_destroy(child);
				sch_tree_unlock(sch);
			}
		}
	}
	return 0;
}

static int prio_init(struct Qdisc *sch, struct rtattr *opt)
{
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;
	int i;

	for (i=0; i<TCQ_PRIO_BANDS; i++)
		q->queues[i] = &noop_qdisc;

	if (opt == NULL) {
		return -EINVAL;
	} else {
		int err;

		if ((err= prio_tune(sch, opt)) != 0)
			return err;
	}
	MOD_INC_USE_COUNT;
	return 0;
}

#ifdef CONFIG_RTNETLINK
static int prio_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;
	unsigned char	 *b = skb->tail;
	struct tc_prio_qopt opt;

	opt.bands = q->bands;
	memcpy(&opt.priomap, q->prio2band, TC_PRIO_MAX+1);
	RTA_PUT(skb, TCA_OPTIONS, sizeof(opt), &opt);
	return skb->len;

rtattr_failure:
	skb_trim(skb, b - skb->data);
	return -1;
}
#endif

static int prio_graft(struct Qdisc *sch, unsigned long arg, struct Qdisc *new,
		      struct Qdisc **old)
{
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;
	unsigned long band = arg - 1;

	if (band >= q->bands)
		return -EINVAL;

	if (new == NULL)
		new = &noop_qdisc;

	sch_tree_lock(sch);
	*old = q->queues[band];
	q->queues[band] = new;
	qdisc_reset(*old);
	sch_tree_unlock(sch);

	return 0;
}

static struct Qdisc *
prio_leaf(struct Qdisc *sch, unsigned long arg)
{
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;
	unsigned long band = arg - 1;

	if (band >= q->bands)
		return NULL;

	return q->queues[band];
}

static unsigned long prio_get(struct Qdisc *sch, u32 classid)
{
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;
	unsigned long band = TC_H_MIN(classid);

	if (band - 1 >= q->bands)
		return 0;
	return band;
}

static unsigned long prio_bind(struct Qdisc *sch, unsigned long parent, u32 classid)
{
	return prio_get(sch, classid);
}


static void prio_put(struct Qdisc *q, unsigned long cl)
{
	return;
}

static int prio_change(struct Qdisc *sch, u32 handle, u32 parent, struct rtattr **tca, unsigned long *arg)
{
	unsigned long cl = *arg;
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;

	if (cl - 1 > q->bands)
		return -ENOENT;
	return 0;
}

static int prio_delete(struct Qdisc *sch, unsigned long cl)
{
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;
	if (cl - 1 > q->bands)
		return -ENOENT;
	return 0;
}


#ifdef CONFIG_RTNETLINK
static int prio_dump_class(struct Qdisc *sch, unsigned long cl, struct sk_buff *skb,
			   struct tcmsg *tcm)
{
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;

	if (cl - 1 > q->bands)
		return -ENOENT;
	if (q->queues[cl-1])
		tcm->tcm_info = q->queues[cl-1]->handle;
	return 0;
}
#endif

static void prio_walk(struct Qdisc *sch, struct qdisc_walker *arg)
{
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;
	int prio;

	if (arg->stop)
		return;

	for (prio = 0; prio < q->bands; prio++) {
		if (arg->count < arg->skip) {
			arg->count++;
			continue;
		}
		if (arg->fn(sch, prio+1, arg) < 0) {
			arg->stop = 1;
			break;
		}
		arg->count++;
	}
}

static struct tcf_proto ** prio_find_tcf(struct Qdisc *sch, unsigned long cl)
{
	struct prio_sched_data *q = (struct prio_sched_data *)sch->data;

	if (cl)
		return NULL;
	return &q->filter_list;
}

static struct Qdisc_class_ops prio_class_ops =
{
	prio_graft,
	prio_leaf,

	prio_get,
	prio_put,
	prio_change,
	prio_delete,
	prio_walk,

	prio_find_tcf,
	prio_bind,
	prio_put,

#ifdef CONFIG_RTNETLINK
	prio_dump_class,
#endif
};

struct Qdisc_ops prio_qdisc_ops =
{
	NULL,
	&prio_class_ops,
	"prio",
	sizeof(struct prio_sched_data),

	prio_enqueue,
	prio_dequeue,
	prio_requeue,
	prio_drop,

	prio_init,
	prio_reset,
	prio_destroy,
	prio_tune,

#ifdef CONFIG_RTNETLINK
	prio_dump,
#endif
};

#ifdef MODULE

int init_module(void)
{
	return register_qdisc(&prio_qdisc_ops);
}

void cleanup_module(void) 
{
	unregister_qdisc(&prio_qdisc_ops);
}

#endif
