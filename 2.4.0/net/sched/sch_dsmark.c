/* net/sched/sch_dsmark.c - Differentiated Services field marker */

/* Written 1998-2000 by Werner Almesberger, EPFL ICA */


#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h> /* for pkt_sched */
#include <linux/rtnetlink.h>
#include <net/pkt_sched.h>
#include <net/dsfield.h>
#include <asm/byteorder.h>


#if 1 /* control */
#define DPRINTK(format,args...) printk(KERN_DEBUG format,##args)
#else
#define DPRINTK(format,args...)
#endif

#if 0 /* data */
#define D2PRINTK(format,args...) printk(KERN_DEBUG format,##args)
#else
#define D2PRINTK(format,args...)
#endif


#define PRIV(sch) ((struct dsmark_qdisc_data *) (sch)->data)


/*
 * classid	class		marking
 * -------	-----		-------
 *   n/a	  0		n/a
 *   x:0	  1		use entry [0]
 *   ...	 ...		...
 *   x:y y>0	 y+1		use entry [y]
 *   ...	 ...		...
 * x:indices-1	indices		use entry [indices-1]
 */


struct dsmark_qdisc_data {
	struct Qdisc		*q;
	struct tcf_proto	*filter_list;
	__u8			*mask;	/* "owns" the array */
	__u8			*value;
	__u16			indices;
	__u16			default_index;
	int			set_tc_index;
};


/* ------------------------- Class/flow operations ------------------------- */


static int dsmark_graft(struct Qdisc *sch,unsigned long arg,
    struct Qdisc *new,struct Qdisc **old)
{
	struct dsmark_qdisc_data *p = PRIV(sch);

	DPRINTK("dsmark_graft(sch %p,[qdisc %p],new %p,old %p)\n",sch,p,new,
	    old);
	if (!new)
		new = &noop_qdisc;
	sch_tree_lock(sch);
	*old = xchg(&p->q,new);
	if (*old)
		qdisc_reset(*old);
	sch_tree_unlock(sch); /* @@@ move up ? */
        return 0;
}


static struct Qdisc *dsmark_leaf(struct Qdisc *sch, unsigned long arg)
{
	return NULL;
}


static unsigned long dsmark_get(struct Qdisc *sch,u32 classid)
{
	struct dsmark_qdisc_data *p __attribute__((unused)) = PRIV(sch);

	DPRINTK("dsmark_get(sch %p,[qdisc %p],classid %x)\n",sch,p,classid);
	return TC_H_MIN(classid)+1;
}


static unsigned long dsmark_bind_filter(struct Qdisc *sch,
    unsigned long parent, u32 classid)
{
	return dsmark_get(sch,classid);
}


static void dsmark_put(struct Qdisc *sch, unsigned long cl)
{
}


static int dsmark_change(struct Qdisc *sch, u32 classid, u32 parent,
    struct rtattr **tca, unsigned long *arg)
{
	struct dsmark_qdisc_data *p = PRIV(sch);
	struct rtattr *opt = tca[TCA_OPTIONS-1];
	struct rtattr *tb[TCA_DSMARK_MAX];

	DPRINTK("dsmark_change(sch %p,[qdisc %p],classid %x,parent %x),"
	    "arg 0x%lx\n",sch,p,classid,parent,*arg);
	if (*arg > p->indices)
		return -ENOENT;
	if (!opt || rtattr_parse(tb, TCA_DSMARK_MAX, RTA_DATA(opt),
				 RTA_PAYLOAD(opt)))
		return -EINVAL;
	if (tb[TCA_DSMARK_MASK-1]) {
		if (!RTA_PAYLOAD(tb[TCA_DSMARK_MASK-1]))
			return -EINVAL;
		p->mask[*arg-1] = *(__u8 *) RTA_DATA(tb[TCA_DSMARK_MASK-1]);
	}
	if (tb[TCA_DSMARK_VALUE-1]) {
		if (!RTA_PAYLOAD(tb[TCA_DSMARK_VALUE-1]))
			return -EINVAL;
		p->value[*arg-1] = *(__u8 *) RTA_DATA(tb[TCA_DSMARK_VALUE-1]);
	}
	return 0;
}


static int dsmark_delete(struct Qdisc *sch,unsigned long arg)
{
	struct dsmark_qdisc_data *p = PRIV(sch);

	if (!arg || arg > p->indices)
		return -EINVAL;
	p->mask[arg-1] = 0xff;
	p->value[arg-1] = 0;
	return 0;
}


static void dsmark_walk(struct Qdisc *sch,struct qdisc_walker *walker)
{
	struct dsmark_qdisc_data *p = PRIV(sch);
	int i;

	DPRINTK("dsmark_walk(sch %p,[qdisc %p],walker %p)\n",sch,p,walker);
	if (walker->stop)
		return;
	for (i = 0; i < p->indices; i++) {
		if (p->mask[i] == 0xff && !p->value[i])
			continue;
		if (walker->count >= walker->skip) {
			if (walker->fn(sch, i+1, walker) < 0) {
				walker->stop = 1;
				break;
			}
		}
                walker->count++;
        }
}


static struct tcf_proto **dsmark_find_tcf(struct Qdisc *sch,unsigned long cl)
{
	struct dsmark_qdisc_data *p = PRIV(sch);

	return &p->filter_list;
}


/* --------------------------- Qdisc operations ---------------------------- */


static int dsmark_enqueue(struct sk_buff *skb,struct Qdisc *sch)
{
	struct dsmark_qdisc_data *p = PRIV(sch);
	struct tcf_result res;
	int result;
	int ret;

	D2PRINTK("dsmark_enqueue(skb %p,sch %p,[qdisc %p])\n",skb,sch,p);
	if (p->set_tc_index) {
		switch (skb->protocol) {
			case __constant_htons(ETH_P_IP):
				skb->tc_index = ipv4_get_dsfield(skb->nh.iph);
				break;
			case __constant_htons(ETH_P_IPV6):
				skb->tc_index = ipv6_get_dsfield(skb->nh.ipv6h);
				break;
			default:
				skb->tc_index = 0;
				break;
		};
	}
	result = TC_POLICE_OK; /* be nice to gcc */
	if (TC_H_MAJ(skb->priority) == sch->handle) {
		skb->tc_index = TC_H_MIN(skb->priority);
	} else {
		result = tc_classify(skb,p->filter_list,&res);
		D2PRINTK("result %d class 0x%04x\n",result,res.classid);
		switch (result) {
#ifdef CONFIG_NET_CLS_POLICE
			case TC_POLICE_SHOT:
				kfree_skb(skb);
				break;
#if 0
			case TC_POLICE_RECLASSIFY:
				/* FIXME: what to do here ??? */
#endif
#endif
			case TC_POLICE_OK:
				skb->tc_index = TC_H_MIN(res.classid);
				break;
			case TC_POLICE_UNSPEC:
				/* fall through */
			default:
				if (p->default_index)
					skb->tc_index = p->default_index;
				break;
		};
	}
	if (
#ifdef CONFIG_NET_CLS_POLICE
	    result == TC_POLICE_SHOT ||
#endif

	    ((ret = p->q->enqueue(skb,p->q)) != 0)) {
		sch->stats.drops++;
		return 0;
	}
	sch->stats.bytes += skb->len;
	sch->stats.packets++;
	sch->q.qlen++;
	return ret;
}


static struct sk_buff *dsmark_dequeue(struct Qdisc *sch)
{
	struct dsmark_qdisc_data *p = PRIV(sch);
	struct sk_buff *skb;
	int index;

	D2PRINTK("dsmark_dequeue(sch %p,[qdisc %p])\n",sch,p);
	skb = p->q->ops->dequeue(p->q);
	if (!skb)
		return NULL;
	sch->q.qlen--;
	index = skb->tc_index & (p->indices-1);
	D2PRINTK("index %d->%d\n",skb->tc_index,index);
	switch (skb->protocol) {
		case __constant_htons(ETH_P_IP):
			ipv4_change_dsfield(skb->nh.iph,
			    p->mask[index],p->value[index]);
			break;
		case __constant_htons(ETH_P_IPV6):
			ipv6_change_dsfield(skb->nh.ipv6h,
			    p->mask[index],p->value[index]);
			break;
		default:
			/*
			 * Only complain if a change was actually attempted.
			 * This way, we can send non-IP traffic through dsmark
			 * and don't need yet another qdisc as a bypass.
			 */
			if (p->mask[index] != 0xff || p->value[index])
				printk(KERN_WARNING "dsmark_dequeue: "
				       "unsupported protocol %d\n",
				       htons(skb->protocol));
			break;
	};
	return skb;
}


static int dsmark_requeue(struct sk_buff *skb,struct Qdisc *sch)
{
	int ret;
	struct dsmark_qdisc_data *p = PRIV(sch);

	D2PRINTK("dsmark_requeue(skb %p,sch %p,[qdisc %p])\n",skb,sch,p);
        if ((ret = p->q->ops->requeue(skb, p->q)) == 0) {
		sch->q.qlen++;
		return 0;
	}
	sch->stats.drops++;
	return ret;
}


static int dsmark_drop(struct Qdisc *sch)
{
	struct dsmark_qdisc_data *p = PRIV(sch);

	DPRINTK("dsmark_reset(sch %p,[qdisc %p])\n",sch,p);
	if (!p->q->ops->drop)
		return 0;
	if (!p->q->ops->drop(p->q))
		return 0;
	sch->q.qlen--;
	return 1;
}


int dsmark_init(struct Qdisc *sch,struct rtattr *opt)
{
	struct dsmark_qdisc_data *p = PRIV(sch);
	struct rtattr *tb[TCA_DSMARK_MAX];
	__u16 tmp;

	DPRINTK("dsmark_init(sch %p,[qdisc %p],opt %p)\n",sch,p,opt);
	if (rtattr_parse(tb,TCA_DSMARK_MAX,RTA_DATA(opt),RTA_PAYLOAD(opt)) < 0 ||
	    !tb[TCA_DSMARK_INDICES-1] ||
	    RTA_PAYLOAD(tb[TCA_DSMARK_INDICES-1]) < sizeof(__u16))
                return -EINVAL;
	memset(p,0,sizeof(*p));
	p->filter_list = NULL;
	p->indices = *(__u16 *) RTA_DATA(tb[TCA_DSMARK_INDICES-1]);
	if (!p->indices)
		return -EINVAL;
	for (tmp = p->indices; tmp != 1; tmp >>= 1) {
		if (tmp & 1)
			return -EINVAL;
	}
	p->default_index = 0;
	if (tb[TCA_DSMARK_DEFAULT_INDEX-1]) {
		if (RTA_PAYLOAD(tb[TCA_DSMARK_DEFAULT_INDEX-1]) < sizeof(__u16))
			return -EINVAL;
		p->default_index =
		    *(__u16 *) RTA_DATA(tb[TCA_DSMARK_DEFAULT_INDEX-1]);
		if (!p->default_index || p->default_index >= p->indices)
			return -EINVAL;
	}
	p->set_tc_index = !!tb[TCA_DSMARK_SET_TC_INDEX-1];
	p->mask = kmalloc(p->indices*2,GFP_KERNEL);
	if (!p->mask)
		return -ENOMEM;
	p->value = p->mask+p->indices;
	memset(p->mask,0xff,p->indices);
	memset(p->value,0,p->indices);
	if (!(p->q = qdisc_create_dflt(sch->dev, &pfifo_qdisc_ops)))
		p->q = &noop_qdisc;
	DPRINTK("dsmark_init: qdisc %p\n",&p->q);
	MOD_INC_USE_COUNT;
	return 0;
}


static void dsmark_reset(struct Qdisc *sch)
{
	struct dsmark_qdisc_data *p = PRIV(sch);

	DPRINTK("dsmark_reset(sch %p,[qdisc %p])\n",sch,p);
	qdisc_reset(p->q);
	sch->q.qlen = 0;
}


static void dsmark_destroy(struct Qdisc *sch)
{
	struct dsmark_qdisc_data *p = PRIV(sch);
	struct tcf_proto *tp;

	DPRINTK("dsmark_destroy(sch %p,[qdisc %p])\n",sch,p);
	while (p->filter_list) {
		tp = p->filter_list;
		p->filter_list = tp->next;
		tp->ops->destroy(tp);
	}
	qdisc_destroy(p->q);
	p->q = &noop_qdisc;
	kfree(p->mask);
	MOD_DEC_USE_COUNT;
}


#ifdef CONFIG_RTNETLINK

static int dsmark_dump_class(struct Qdisc *sch, unsigned long cl,
    struct sk_buff *skb, struct tcmsg *tcm)
{
	struct dsmark_qdisc_data *p = PRIV(sch);
	unsigned char *b = skb->tail;
	struct rtattr *rta;

	DPRINTK("dsmark_dump_class(sch %p,[qdisc %p],class %ld\n",sch,p,cl);
	if (!cl || cl > p->indices)
		return -EINVAL;
	tcm->tcm_handle = TC_H_MAKE(TC_H_MAJ(sch->handle),cl-1);
	rta = (struct rtattr *) b;
	RTA_PUT(skb,TCA_OPTIONS,0,NULL);
	RTA_PUT(skb,TCA_DSMARK_MASK,1,&p->mask[cl-1]);
	RTA_PUT(skb,TCA_DSMARK_VALUE,1,&p->value[cl-1]);
	rta->rta_len = skb->tail-b;
	return skb->len;

rtattr_failure:
	skb_trim(skb,b-skb->data);
	return -1;
}

static int dsmark_dump(struct Qdisc *sch, struct sk_buff *skb)
{
	struct dsmark_qdisc_data *p = PRIV(sch);
	unsigned char *b = skb->tail;
	struct rtattr *rta;

	rta = (struct rtattr *) b;
	RTA_PUT(skb,TCA_OPTIONS,0,NULL);
	RTA_PUT(skb,TCA_DSMARK_INDICES,sizeof(__u16),&p->indices);
	if (p->default_index)
		RTA_PUT(skb,TCA_DSMARK_DEFAULT_INDEX, sizeof(__u16),
			&p->default_index);
	if (p->set_tc_index)
		RTA_PUT(skb, TCA_DSMARK_SET_TC_INDEX, 0, NULL);
	rta->rta_len = skb->tail-b;
	return skb->len;

rtattr_failure:
	skb_trim(skb,b-skb->data);
	return -1;
}

#endif


static struct Qdisc_class_ops dsmark_class_ops =
{
	dsmark_graft,			/* graft */
	dsmark_leaf,			/* leaf */
	dsmark_get,			/* get */
	dsmark_put,			/* put */
	dsmark_change,			/* change */
	dsmark_delete,			/* delete */
	dsmark_walk,			/* walk */

	dsmark_find_tcf,		/* tcf_chain */
	dsmark_bind_filter,		/* bind_tcf */
	dsmark_put,			/* unbind_tcf */

#ifdef CONFIG_RTNETLINK
	dsmark_dump_class,		/* dump */
#endif
};

struct Qdisc_ops dsmark_qdisc_ops =
{
	NULL,				/* next */
	&dsmark_class_ops,		/* cl_ops */
	"dsmark",
	sizeof(struct dsmark_qdisc_data),

	dsmark_enqueue,			/* enqueue */
	dsmark_dequeue,			/* dequeue */
	dsmark_requeue,			/* requeue */
	dsmark_drop,			/* drop */

	dsmark_init,			/* init */
	dsmark_reset,			/* reset */
	dsmark_destroy,			/* destroy */
	NULL,				/* change */

#ifdef CONFIG_RTNETLINK
	dsmark_dump			/* dump */
#endif
};

#ifdef MODULE
int init_module(void)
{
	return register_qdisc(&dsmark_qdisc_ops);
}


void cleanup_module(void) 
{
	unregister_qdisc(&dsmark_qdisc_ops);
}
#endif
