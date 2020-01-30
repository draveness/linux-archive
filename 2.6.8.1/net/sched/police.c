/*
 * net/sched/police.c	Input police filter.
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * Authors:	Alexey Kuznetsov, <kuznet@ms2.inr.ac.ru>
 * 		J Hadi Salim (action changes)
 */

#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/bitops.h>
#include <linux/config.h>
#include <linux/module.h>
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
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/module.h>
#include <linux/rtnetlink.h>
#include <linux/init.h>
#include <net/sock.h>
#include <net/pkt_sched.h>

#define L2T(p,L)   ((p)->R_tab->data[(L)>>(p)->R_tab->rate.cell_log])
#define L2T_P(p,L) ((p)->P_tab->data[(L)>>(p)->P_tab->rate.cell_log])
#define PRIV(a) ((struct tcf_police *) (a)->priv)

/* use generic hash table */
#define MY_TAB_SIZE     16
#define MY_TAB_MASK     15
static u32 idx_gen;
static struct tcf_police *tcf_police_ht[MY_TAB_SIZE];
/* Policer hash table lock */
static rwlock_t police_lock = RW_LOCK_UNLOCKED;

/* Each policer is serialized by its individual spinlock */

static __inline__ unsigned tcf_police_hash(u32 index)
{
	return index&0xF;
}

static __inline__ struct tcf_police * tcf_police_lookup(u32 index)
{
	struct tcf_police *p;

	read_lock(&police_lock);
	for (p = tcf_police_ht[tcf_police_hash(index)]; p; p = p->next) {
		if (p->index == index)
			break;
	}
	read_unlock(&police_lock);
	return p;
}

#ifdef CONFIG_NET_CLS_ACT
static __inline__ int tcf_generic_walker(struct sk_buff *skb, struct netlink_callback *cb, int type, struct tc_action *a)
{
	struct tcf_police *p;
	int err =0, index =  -1,i= 0, s_i = 0, n_i = 0;
	struct rtattr *r ;

	read_lock(&police_lock);

	s_i = cb->args[0];

	for (i = 0; i < MY_TAB_SIZE; i++) {
		p = tcf_police_ht[tcf_police_hash(i)];

		for (; p; p = p->next) {
			index++;
			if (index < s_i)
				continue;
			a->priv = p;
			a->order = index;
			r = (struct rtattr*) skb->tail;
			RTA_PUT(skb, a->order, 0, NULL);
			if (type == RTM_DELACTION)
				err = tcf_action_dump_1(skb, a, 0, 1);
			else
				err = tcf_action_dump_1(skb, a, 0, 0);
			if (0 > err) {
				index--;
				skb_trim(skb, (u8*)r - skb->data);
				goto done;
			}
			r->rta_len = skb->tail - (u8*)r;
			n_i++;
		}
	}
done:
	read_unlock(&police_lock);
	if (n_i)
		cb->args[0] += n_i;
	return n_i;

rtattr_failure:
	skb_trim(skb, (u8*)r - skb->data);
	goto done;
}

static inline int
tcf_hash_search(struct tc_action *a, u32 index)
{
	struct tcf_police *p = tcf_police_lookup(index);

	if (p != NULL) {
		a->priv = p;
		return 1;
	} else {
		return 0;
	}
}


#endif

static __inline__ u32 tcf_police_new_index(void)
{
	do {
		if (++idx_gen == 0)
			idx_gen = 1;
	} while (tcf_police_lookup(idx_gen));

	return idx_gen;
}


void tcf_police_destroy(struct tcf_police *p)
{
	unsigned h = tcf_police_hash(p->index);
	struct tcf_police **p1p;
	
	for (p1p = &tcf_police_ht[h]; *p1p; p1p = &(*p1p)->next) {
		if (*p1p == p) {
			write_lock_bh(&police_lock);
			*p1p = p->next;
			write_unlock_bh(&police_lock);
#ifdef CONFIG_NET_ESTIMATOR
			qdisc_kill_estimator(&p->stats);
#endif
			if (p->R_tab)
				qdisc_put_rtab(p->R_tab);
			if (p->P_tab)
				qdisc_put_rtab(p->P_tab);
			kfree(p);
			return;
		}
	}
	BUG_TRAP(0);
}

#ifdef CONFIG_NET_CLS_ACT
int tcf_act_police_locate(struct rtattr *rta, struct rtattr *est,struct tc_action *a, int ovr, int bind)
{
	unsigned h;
	int ret = 0;
	struct rtattr *tb[TCA_POLICE_MAX];
	struct tc_police *parm;
	struct tcf_police *p;

	if (NULL == a) {
		if (net_ratelimit())
			printk("BUG: tcf_police_locate called with NULL params\n");
		return -1;  
	}

	if (rtattr_parse(tb, TCA_POLICE_MAX, RTA_DATA(rta), RTA_PAYLOAD(rta)) < 0)
		return -1;

	if (tb[TCA_POLICE_TBF-1] == NULL)
		return -1;

	parm = RTA_DATA(tb[TCA_POLICE_TBF-1]);

	if (parm->index && (p = tcf_police_lookup(parm->index)) != NULL) {
		a->priv = (void *)p;
		spin_lock(&p->lock);
		if (bind) {
			p->bindcnt += 1;
			p->refcnt += 1;
		}
		if (ovr) {
			goto override;
		}
		spin_unlock(&p->lock);
		return ret; 
	}

	p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (p == NULL)
		return -1;

	memset(p, 0, sizeof(*p));
	ret = 1;
	p->refcnt = 1;
	spin_lock_init(&p->lock);
	p->stats_lock = &p->lock;
	if (bind)
		p->bindcnt = 1;
override:
	if (parm->rate.rate) {
		if ((p->R_tab = qdisc_get_rtab(&parm->rate, tb[TCA_POLICE_RATE-1])) == NULL) {
			goto failure;
		}
		if (parm->peakrate.rate &&
		    (p->P_tab = qdisc_get_rtab(&parm->peakrate, tb[TCA_POLICE_PEAKRATE-1])) == NULL) {
			goto failure;
		}
	}
	if (tb[TCA_POLICE_RESULT-1])
		p->result = *(int*)RTA_DATA(tb[TCA_POLICE_RESULT-1]);
#ifdef CONFIG_NET_ESTIMATOR
	if (tb[TCA_POLICE_AVRATE-1])
		p->ewma_rate = *(u32*)RTA_DATA(tb[TCA_POLICE_AVRATE-1]);
#endif
	p->toks = p->burst = parm->burst;
	p->mtu = parm->mtu;
	if (p->mtu == 0) {
		p->mtu = ~0;
		if (p->R_tab)
			p->mtu = 255<<p->R_tab->rate.cell_log;
	}
	if (p->P_tab)
		p->ptoks = L2T_P(p, p->mtu);
	p->action = parm->action;

	if (ovr) {
		spin_unlock(&p->lock);
		return ret;
	}
	PSCHED_GET_TIME(p->t_c);
	p->index = parm->index ? : tcf_police_new_index();
#ifdef CONFIG_NET_ESTIMATOR
	if (est)
		qdisc_new_estimator(&p->stats, p->stats_lock, est);
#endif
	h = tcf_police_hash(p->index);
	write_lock_bh(&police_lock);
	p->next = tcf_police_ht[h];
	tcf_police_ht[h] = p;
	write_unlock_bh(&police_lock);

	a->priv = (void *)p;
	return ret;  

failure:
	if (p->R_tab)
		qdisc_put_rtab(p->R_tab);
	if (ovr)
		spin_unlock(&p->lock);
	kfree(p);
	return -1;
}

int tcf_act_police_cleanup(struct tc_action *a, int bind)
{
	struct tcf_police *p;
	p = PRIV(a);
	if (NULL != p) 
		return tcf_police_release(p, bind);

	return 0;
}

int tcf_act_police_stats(struct sk_buff *skb, struct tc_action *a)
{
	struct tcf_police *p;
	p = PRIV(a);
	if (NULL != p) 
		return qdisc_copy_stats(skb, &p->stats, p->stats_lock);

	return 1;
}

int tcf_act_police(struct sk_buff **pskb, struct tc_action *a)
{
	psched_time_t now;
	struct sk_buff *skb = *pskb;
	struct tcf_police *p;
	long toks;
	long ptoks = 0;

	p = PRIV(a);

	if (NULL == p) {
		printk("BUG: tcf_police called with NULL params\n");
		return -1;  
	}

	spin_lock(&p->lock);

	p->stats.bytes += skb->len;
	p->stats.packets++;

#ifdef CONFIG_NET_ESTIMATOR
	if (p->ewma_rate && p->stats.bps >= p->ewma_rate) {
		p->stats.overlimits++;
		spin_unlock(&p->lock);
		return p->action;
	}
#endif

	if (skb->len <= p->mtu) {
		if (p->R_tab == NULL) {
			spin_unlock(&p->lock);
			return p->result;
		}

		PSCHED_GET_TIME(now);

		toks = PSCHED_TDIFF_SAFE(now, p->t_c, p->burst);

		if (p->P_tab) {
			ptoks = toks + p->ptoks;
			if (ptoks > (long)L2T_P(p, p->mtu))
				ptoks = (long)L2T_P(p, p->mtu);
			ptoks -= L2T_P(p, skb->len);
		}
		toks += p->toks;
		if (toks > (long)p->burst)
			toks = p->burst;
		toks -= L2T(p, skb->len);

		if ((toks|ptoks) >= 0) {
			p->t_c = now;
			p->toks = toks;
			p->ptoks = ptoks;
			spin_unlock(&p->lock);
			return p->result;
		}
	}

	p->stats.overlimits++;
	spin_unlock(&p->lock);
	return p->action;
}

int tcf_act_police_dump(struct sk_buff *skb, struct tc_action *a, int bind, int ref)
{
	unsigned char	 *b = skb->tail;
	struct tc_police opt;
	struct tcf_police *p;

	p = PRIV(a);
	if (NULL == p) {
		printk("BUG: tcf_police_dump called with NULL params\n");
		goto rtattr_failure;
	}

	opt.index = p->index;
	opt.action = p->action;
	opt.mtu = p->mtu;
	opt.burst = p->burst;
	opt.refcnt = p->refcnt - ref;
	opt.bindcnt = p->bindcnt - bind;
	if (p->R_tab)
		opt.rate = p->R_tab->rate;
	else
		memset(&opt.rate, 0, sizeof(opt.rate));
	if (p->P_tab)
		opt.peakrate = p->P_tab->rate;
	else
		memset(&opt.peakrate, 0, sizeof(opt.peakrate));
	RTA_PUT(skb, TCA_POLICE_TBF, sizeof(opt), &opt);
	if (p->result)
		RTA_PUT(skb, TCA_POLICE_RESULT, sizeof(int), &p->result);
#ifdef CONFIG_NET_ESTIMATOR
	if (p->ewma_rate)
		RTA_PUT(skb, TCA_POLICE_AVRATE, 4, &p->ewma_rate);
#endif
	return skb->len;

rtattr_failure:
	skb_trim(skb, b - skb->data);
	return -1;
}

MODULE_AUTHOR("Alexey Kuznetsov");
MODULE_DESCRIPTION("Policing actions");
MODULE_LICENSE("GPL");


static struct tc_action_ops act_police_ops = {
	.next		=	NULL,
	.kind		=	"police",
	.type		=	TCA_ID_POLICE, 
	.capab		=	TCA_CAP_NONE, 
	.owner		=	THIS_MODULE,
	.act		=	tcf_act_police,
	.get_stats	=	tcf_act_police_stats,
	.dump		=	tcf_act_police_dump,
	.cleanup	=	tcf_act_police_cleanup,
	.lookup		=	tcf_hash_search,
	.init		=	tcf_act_police_locate,
	.walk		=	tcf_generic_walker
};

static int __init
police_init_module(void)
{
	return tcf_register_action(&act_police_ops);
}

static void __exit
police_cleanup_module(void)
{
	tcf_unregister_action(&act_police_ops);
}

module_init(police_init_module);
module_exit(police_cleanup_module);

#endif

struct tcf_police * tcf_police_locate(struct rtattr *rta, struct rtattr *est)
{
	unsigned h;
	struct tcf_police *p;
	struct rtattr *tb[TCA_POLICE_MAX];
	struct tc_police *parm;

	if (rtattr_parse(tb, TCA_POLICE_MAX, RTA_DATA(rta), RTA_PAYLOAD(rta)) < 0)
		return NULL;

	if (tb[TCA_POLICE_TBF-1] == NULL)
		return NULL;

	parm = RTA_DATA(tb[TCA_POLICE_TBF-1]);

	if (parm->index && (p = tcf_police_lookup(parm->index)) != NULL) {
		p->refcnt++;
		return p;
	}

	p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (p == NULL)
		return NULL;

	memset(p, 0, sizeof(*p));
	p->refcnt = 1;
	spin_lock_init(&p->lock);
	p->stats_lock = &p->lock;
	if (parm->rate.rate) {
		if ((p->R_tab = qdisc_get_rtab(&parm->rate, tb[TCA_POLICE_RATE-1])) == NULL)
			goto failure;
		if (parm->peakrate.rate &&
		    (p->P_tab = qdisc_get_rtab(&parm->peakrate, tb[TCA_POLICE_PEAKRATE-1])) == NULL)
			goto failure;
	}
	if (tb[TCA_POLICE_RESULT-1])
		p->result = *(int*)RTA_DATA(tb[TCA_POLICE_RESULT-1]);
#ifdef CONFIG_NET_ESTIMATOR
	if (tb[TCA_POLICE_AVRATE-1])
		p->ewma_rate = *(u32*)RTA_DATA(tb[TCA_POLICE_AVRATE-1]);
#endif
	p->toks = p->burst = parm->burst;
	p->mtu = parm->mtu;
	if (p->mtu == 0) {
		p->mtu = ~0;
		if (p->R_tab)
			p->mtu = 255<<p->R_tab->rate.cell_log;
	}
	if (p->P_tab)
		p->ptoks = L2T_P(p, p->mtu);
	PSCHED_GET_TIME(p->t_c);
	p->index = parm->index ? : tcf_police_new_index();
	p->action = parm->action;
#ifdef CONFIG_NET_ESTIMATOR
	if (est)
		qdisc_new_estimator(&p->stats, p->stats_lock, est);
#endif
	h = tcf_police_hash(p->index);
	write_lock_bh(&police_lock);
	p->next = tcf_police_ht[h];
	tcf_police_ht[h] = p;
	write_unlock_bh(&police_lock);
	return p;

failure:
	if (p->R_tab)
		qdisc_put_rtab(p->R_tab);
	kfree(p);
	return NULL;
}

int tcf_police(struct sk_buff *skb, struct tcf_police *p)
{
	psched_time_t now;
	long toks;
	long ptoks = 0;

	spin_lock(&p->lock);

	p->stats.bytes += skb->len;
	p->stats.packets++;

#ifdef CONFIG_NET_ESTIMATOR
	if (p->ewma_rate && p->stats.bps >= p->ewma_rate) {
		p->stats.overlimits++;
		spin_unlock(&p->lock);
		return p->action;
	}
#endif

	if (skb->len <= p->mtu) {
		if (p->R_tab == NULL) {
			spin_unlock(&p->lock);
			return p->result;
		}

		PSCHED_GET_TIME(now);

		toks = PSCHED_TDIFF_SAFE(now, p->t_c, p->burst);

		if (p->P_tab) {
			ptoks = toks + p->ptoks;
			if (ptoks > (long)L2T_P(p, p->mtu))
				ptoks = (long)L2T_P(p, p->mtu);
			ptoks -= L2T_P(p, skb->len);
		}
		toks += p->toks;
		if (toks > (long)p->burst)
			toks = p->burst;
		toks -= L2T(p, skb->len);

		if ((toks|ptoks) >= 0) {
			p->t_c = now;
			p->toks = toks;
			p->ptoks = ptoks;
			spin_unlock(&p->lock);
			return p->result;
		}
	}

	p->stats.overlimits++;
	spin_unlock(&p->lock);
	return p->action;
}

int tcf_police_dump(struct sk_buff *skb, struct tcf_police *p)
{
	unsigned char	 *b = skb->tail;
	struct tc_police opt;

	opt.index = p->index;
	opt.action = p->action;
	opt.mtu = p->mtu;
	opt.burst = p->burst;
	if (p->R_tab)
		opt.rate = p->R_tab->rate;
	else
		memset(&opt.rate, 0, sizeof(opt.rate));
	if (p->P_tab)
		opt.peakrate = p->P_tab->rate;
	else
		memset(&opt.peakrate, 0, sizeof(opt.peakrate));
	RTA_PUT(skb, TCA_POLICE_TBF, sizeof(opt), &opt);
	if (p->result)
		RTA_PUT(skb, TCA_POLICE_RESULT, sizeof(int), &p->result);
#ifdef CONFIG_NET_ESTIMATOR
	if (p->ewma_rate)
		RTA_PUT(skb, TCA_POLICE_AVRATE, 4, &p->ewma_rate);
#endif
	return skb->len;

rtattr_failure:
	skb_trim(skb, b - skb->data);
	return -1;
}

EXPORT_SYMBOL(tcf_police);
EXPORT_SYMBOL(tcf_police_destroy);
EXPORT_SYMBOL(tcf_police_dump);
EXPORT_SYMBOL(tcf_police_hash);
EXPORT_SYMBOL(tcf_police_ht);
EXPORT_SYMBOL(tcf_police_locate);
EXPORT_SYMBOL(tcf_police_lookup);
EXPORT_SYMBOL(tcf_police_new_index);
