#ifndef __NET_PKT_SCHED_H
#define __NET_PKT_SCHED_H

#include <linux/config.h>
#include <linux/netdevice.h>
#include <linux/types.h>
#include <linux/pkt_sched.h>
#include <linux/rcupdate.h>
#include <net/pkt_cls.h>
#include <linux/module.h>
#include <linux/rtnetlink.h>

struct rtattr;
struct Qdisc;

struct qdisc_walker
{
	int	stop;
	int	skip;
	int	count;
	int	(*fn)(struct Qdisc *, unsigned long cl, struct qdisc_walker *);
};

struct Qdisc_class_ops
{
	/* Child qdisc manipulation */
	int			(*graft)(struct Qdisc *, unsigned long cl, struct Qdisc *, struct Qdisc **);
	struct Qdisc *		(*leaf)(struct Qdisc *, unsigned long cl);

	/* Class manipulation routines */
	unsigned long		(*get)(struct Qdisc *, u32 classid);
	void			(*put)(struct Qdisc *, unsigned long);
	int			(*change)(struct Qdisc *, u32, u32, struct rtattr **, unsigned long *);
	int			(*delete)(struct Qdisc *, unsigned long);
	void			(*walk)(struct Qdisc *, struct qdisc_walker * arg);

	/* Filter manipulation */
	struct tcf_proto **	(*tcf_chain)(struct Qdisc *, unsigned long);
	unsigned long		(*bind_tcf)(struct Qdisc *, unsigned long, u32 classid);
	void			(*unbind_tcf)(struct Qdisc *, unsigned long);

	/* rtnetlink specific */
	int			(*dump)(struct Qdisc *, unsigned long, struct sk_buff *skb, struct tcmsg*);
};

struct module;

struct Qdisc_ops
{
	struct Qdisc_ops	*next;
	struct Qdisc_class_ops	*cl_ops;
	char			id[IFNAMSIZ];
	int			priv_size;

	int 			(*enqueue)(struct sk_buff *, struct Qdisc *);
	struct sk_buff *	(*dequeue)(struct Qdisc *);
	int 			(*requeue)(struct sk_buff *, struct Qdisc *);
	unsigned int		(*drop)(struct Qdisc *);

	int			(*init)(struct Qdisc *, struct rtattr *arg);
	void			(*reset)(struct Qdisc *);
	void			(*destroy)(struct Qdisc *);
	int			(*change)(struct Qdisc *, struct rtattr *arg);

	int			(*dump)(struct Qdisc *, struct sk_buff *);

	struct module		*owner;
};

extern rwlock_t qdisc_tree_lock;

struct Qdisc
{
	int 			(*enqueue)(struct sk_buff *skb, struct Qdisc *dev);
	struct sk_buff *	(*dequeue)(struct Qdisc *dev);
	unsigned		flags;
#define TCQ_F_BUILTIN	1
#define TCQ_F_THROTTLED	2
#define TCQ_F_INGRES	4
	int			padded;
	struct Qdisc_ops	*ops;
	u32			handle;
	atomic_t		refcnt;
	struct sk_buff_head	q;
	struct net_device	*dev;
	struct list_head	list;

	struct tc_stats		stats;
	spinlock_t		*stats_lock;
	struct rcu_head 	q_rcu;
	int			(*reshape_fail)(struct sk_buff *skb, struct Qdisc *q);

	/* This field is deprecated, but it is still used by CBQ
	 * and it will live until better solution will be invented.
	 */
	struct Qdisc		*__parent;
};

#define	QDISC_ALIGN		32
#define	QDISC_ALIGN_CONST	(QDISC_ALIGN - 1)

static inline void *qdisc_priv(struct Qdisc *q)
{
	return (char *)q + ((sizeof(struct Qdisc) + QDISC_ALIGN_CONST)
			      & ~QDISC_ALIGN_CONST);
}

struct qdisc_rate_table
{
	struct tc_ratespec rate;
	u32		data[256];
	struct qdisc_rate_table *next;
	int		refcnt;
};

extern void qdisc_lock_tree(struct net_device *dev);
extern void qdisc_unlock_tree(struct net_device *dev);

#define sch_tree_lock(q)	qdisc_lock_tree((q)->dev)
#define sch_tree_unlock(q)	qdisc_unlock_tree((q)->dev)
#define tcf_tree_lock(tp)	qdisc_lock_tree((tp)->q->dev)
#define tcf_tree_unlock(tp)	qdisc_unlock_tree((tp)->q->dev)

#define cls_set_class(tp, clp, cl) tcf_set_class(tp, clp, cl)
static inline unsigned long
__cls_set_class(unsigned long *clp, unsigned long cl)
{
	unsigned long old_cl;

	old_cl = *clp;
	*clp = cl;
	return old_cl;
}


/* 
   Timer resolution MUST BE < 10% of min_schedulable_packet_size/bandwidth
   
   Normal IP packet size ~ 512byte, hence:

   0.5Kbyte/1Mbyte/sec = 0.5msec, so that we need 50usec timer for
   10Mbit ethernet.

   10msec resolution -> <50Kbit/sec.
   
   The result: [34]86 is not good choice for QoS router :-(

   The things are not so bad, because we may use artifical
   clock evaluated by integration of network data flow
   in the most critical places.

   Note: we do not use fastgettimeofday.
   The reason is that, when it is not the same thing as
   gettimeofday, it returns invalid timestamp, which is
   not updated, when net_bh is active.
 */

/* General note about internal clock.

   Any clock source returns time intervals, measured in units
   close to 1usec. With source CONFIG_NET_SCH_CLK_GETTIMEOFDAY it is precisely
   microseconds, otherwise something close but different chosen to minimize
   arithmetic cost. Ratio usec/internal untis in form nominator/denominator
   may be read from /proc/net/psched.
 */


#ifdef CONFIG_NET_SCH_CLK_GETTIMEOFDAY

typedef struct timeval	psched_time_t;
typedef long		psched_tdiff_t;

#define PSCHED_GET_TIME(stamp) do_gettimeofday(&(stamp))
#define PSCHED_US2JIFFIE(usecs) (((usecs)+(1000000/HZ-1))/(1000000/HZ))
#define PSCHED_JIFFIE2US(delay) ((delay)*(1000000/HZ))

#else /* !CONFIG_NET_SCH_CLK_GETTIMEOFDAY */

typedef u64	psched_time_t;
typedef long	psched_tdiff_t;

#ifdef CONFIG_NET_SCH_CLK_JIFFIES

#if HZ < 96
#define PSCHED_JSCALE 14
#elif HZ >= 96 && HZ < 192
#define PSCHED_JSCALE 13
#elif HZ >= 192 && HZ < 384
#define PSCHED_JSCALE 12
#elif HZ >= 384 && HZ < 768
#define PSCHED_JSCALE 11
#elif HZ >= 768
#define PSCHED_JSCALE 10
#endif

#define PSCHED_GET_TIME(stamp) ((stamp) = (get_jiffies_64()<<PSCHED_JSCALE))
#define PSCHED_US2JIFFIE(delay) (((delay)+(1<<PSCHED_JSCALE)-1)>>PSCHED_JSCALE)
#define PSCHED_JIFFIE2US(delay) ((delay)<<PSCHED_JSCALE)

#endif /* CONFIG_NET_SCH_CLK_JIFFIES */
#ifdef CONFIG_NET_SCH_CLK_CPU
#include <asm/timex.h>

extern psched_tdiff_t psched_clock_per_hz;
extern int psched_clock_scale;
extern psched_time_t psched_time_base;
extern cycles_t psched_time_mark;

#define PSCHED_GET_TIME(stamp)						\
do {									\
	cycles_t cur = get_cycles();					\
	if (sizeof(cycles_t) == sizeof(u32)) {				\
		if (cur <= psched_time_mark)				\
			psched_time_base += 0x100000000ULL;		\
		psched_time_mark = cur;					\
		(stamp) = (psched_time_base + cur)>>psched_clock_scale;	\
	} else {							\
		(stamp) = cur>>psched_clock_scale;			\
	}								\
} while (0)
#define PSCHED_US2JIFFIE(delay) (((delay)+psched_clock_per_hz-1)/psched_clock_per_hz)
#define PSCHED_JIFFIE2US(delay) ((delay)*psched_clock_per_hz)

#endif /* CONFIG_NET_SCH_CLK_CPU */

#endif /* !CONFIG_NET_SCH_CLK_GETTIMEOFDAY */

#ifdef CONFIG_NET_SCH_CLK_GETTIMEOFDAY
#define PSCHED_TDIFF(tv1, tv2) \
({ \
	   int __delta_sec = (tv1).tv_sec - (tv2).tv_sec; \
	   int __delta = (tv1).tv_usec - (tv2).tv_usec; \
	   if (__delta_sec) { \
	           switch (__delta_sec) { \
		   default: \
			   __delta = 0; \
		   case 2: \
			   __delta += 1000000; \
		   case 1: \
			   __delta += 1000000; \
	           } \
	   } \
	   __delta; \
})

extern int psched_tod_diff(int delta_sec, int bound);

#define PSCHED_TDIFF_SAFE(tv1, tv2, bound) \
({ \
	   int __delta_sec = (tv1).tv_sec - (tv2).tv_sec; \
	   int __delta = (tv1).tv_usec - (tv2).tv_usec; \
	   switch (__delta_sec) { \
	   default: \
		   __delta = psched_tod_diff(__delta_sec, bound);  break; \
	   case 2: \
		   __delta += 1000000; \
	   case 1: \
		   __delta += 1000000; \
	   case 0: ; \
	   } \
	   __delta; \
})

#define PSCHED_TLESS(tv1, tv2) (((tv1).tv_usec < (tv2).tv_usec && \
				(tv1).tv_sec <= (tv2).tv_sec) || \
				 (tv1).tv_sec < (tv2).tv_sec)

#define PSCHED_TADD2(tv, delta, tv_res) \
({ \
	   int __delta = (tv).tv_usec + (delta); \
	   (tv_res).tv_sec = (tv).tv_sec; \
	   if (__delta > 1000000) { (tv_res).tv_sec++; __delta -= 1000000; } \
	   (tv_res).tv_usec = __delta; \
})

#define PSCHED_TADD(tv, delta) \
({ \
	   (tv).tv_usec += (delta); \
	   if ((tv).tv_usec > 1000000) { (tv).tv_sec++; \
		 (tv).tv_usec -= 1000000; } \
})

/* Set/check that time is in the "past perfect";
   it depends on concrete representation of system time
 */

#define PSCHED_SET_PASTPERFECT(t)	((t).tv_sec = 0)
#define PSCHED_IS_PASTPERFECT(t)	((t).tv_sec == 0)

#define	PSCHED_AUDIT_TDIFF(t) ({ if ((t) > 2000000) (t) = 2000000; })

#else /* !CONFIG_NET_SCH_CLK_GETTIMEOFDAY */

#define PSCHED_TDIFF(tv1, tv2) (long)((tv1) - (tv2))
#define PSCHED_TDIFF_SAFE(tv1, tv2, bound) \
	min_t(long long, (tv1) - (tv2), bound)


#define PSCHED_TLESS(tv1, tv2) ((tv1) < (tv2))
#define PSCHED_TADD2(tv, delta, tv_res) ((tv_res) = (tv) + (delta))
#define PSCHED_TADD(tv, delta) ((tv) += (delta))
#define PSCHED_SET_PASTPERFECT(t)	((t) = 0)
#define PSCHED_IS_PASTPERFECT(t)	((t) == 0)
#define	PSCHED_AUDIT_TDIFF(t)

#endif /* !CONFIG_NET_SCH_CLK_GETTIMEOFDAY */

struct tcf_police
{
	struct tcf_police *next;
	int		refcnt;
#ifdef CONFIG_NET_CLS_ACT
	int		bindcnt;
#endif
	u32		index;
	int		action;
	int		result;
	u32		ewma_rate;
	u32		burst;
	u32		mtu;
	u32		toks;
	u32		ptoks;
	psched_time_t	t_c;
	spinlock_t	lock;
	struct qdisc_rate_table *R_tab;
	struct qdisc_rate_table *P_tab;

	struct tc_stats	stats;
	spinlock_t	*stats_lock;
};

#ifdef CONFIG_NET_CLS_ACT

#define ACT_P_CREATED 1
#define ACT_P_DELETED 1
#define tca_gen(name) \
struct tcf_##name *next; \
	u32 index; \
	int refcnt; \
	int bindcnt; \
	u32 capab; \
	int action; \
	struct tcf_t tm; \
	struct tc_stats stats; \
	spinlock_t *stats_lock; \
	spinlock_t lock


struct tc_action
{
	void *priv;
	struct tc_action_ops *ops;
	__u32   type;   /* for backward compat(TCA_OLD_COMPAT) */
	__u32   order; 
	struct tc_action *next;
};

#define TCA_CAP_NONE 0
struct tc_action_ops
{
	struct tc_action_ops *next;
	char    kind[IFNAMSIZ];
	__u32   type; /* TBD to match kind */
	__u32 	capab;  /* capabilities includes 4 bit version */
	struct module		*owner;
	int     (*act)(struct sk_buff **, struct tc_action *);
	int     (*get_stats)(struct sk_buff *, struct tc_action *);
	int     (*dump)(struct sk_buff *, struct tc_action *,int , int);
	int     (*cleanup)(struct tc_action *, int bind);
	int     (*lookup)(struct tc_action *, u32 );
	int     (*init)(struct rtattr *,struct rtattr *,struct tc_action *, int , int );
	int     (*walk)(struct sk_buff *, struct netlink_callback *, int , struct tc_action *);
};

extern int tcf_register_action(struct tc_action_ops *a);
extern int tcf_unregister_action(struct tc_action_ops *a);
extern void tcf_action_destroy(struct tc_action *a, int bind);
extern int tcf_action_exec(struct sk_buff *skb, struct tc_action *a);
extern int tcf_action_init(struct rtattr *rta, struct rtattr *est, struct tc_action *a,char *n, int ovr, int bind);
extern int tcf_action_init_1(struct rtattr *rta, struct rtattr *est, struct tc_action *a,char *n, int ovr, int bind);
extern int tcf_action_dump(struct sk_buff *skb, struct tc_action *a, int, int);
extern int tcf_action_dump_old(struct sk_buff *skb, struct tc_action *a, int, int);
extern int tcf_action_dump_1(struct sk_buff *skb, struct tc_action *a, int, int);
extern int tcf_action_copy_stats (struct sk_buff *,struct tc_action *);
extern int tcf_act_police_locate(struct rtattr *rta, struct rtattr *est,struct tc_action *,int , int );
extern int tcf_act_police_dump(struct sk_buff *, struct tc_action *, int, int);
extern int tcf_act_police(struct sk_buff **skb, struct tc_action *a);
#endif

extern unsigned long tcf_set_class(struct tcf_proto *tp, unsigned long *clp, 
				   unsigned long cl);
extern int tcf_police(struct sk_buff *skb, struct tcf_police *p);
extern int qdisc_copy_stats(struct sk_buff *skb, struct tc_stats *st, spinlock_t *lock);
extern void tcf_police_destroy(struct tcf_police *p);
extern struct tcf_police * tcf_police_locate(struct rtattr *rta, struct rtattr *est);
extern int tcf_police_dump(struct sk_buff *skb, struct tcf_police *p);

static inline int tcf_police_release(struct tcf_police *p, int bind)
{
	int ret = 0;
#ifdef CONFIG_NET_CLS_ACT
	if (p) {
		if (bind) {
			 p->bindcnt--;
		}
		p->refcnt--;
		if (p->refcnt <= 0 && !p->bindcnt) {
			tcf_police_destroy(p);
			ret = 1;
		}
	}
#else
	if (p && --p->refcnt == 0)
		tcf_police_destroy(p);

#endif
	return ret;
}

extern struct Qdisc noop_qdisc;
extern struct Qdisc_ops noop_qdisc_ops;
extern struct Qdisc_ops pfifo_qdisc_ops;
extern struct Qdisc_ops bfifo_qdisc_ops;

int register_qdisc(struct Qdisc_ops *qops);
int unregister_qdisc(struct Qdisc_ops *qops);
struct Qdisc *qdisc_lookup(struct net_device *dev, u32 handle);
struct Qdisc *qdisc_lookup_class(struct net_device *dev, u32 handle);
void dev_init_scheduler(struct net_device *dev);
void dev_shutdown(struct net_device *dev);
void dev_activate(struct net_device *dev);
void dev_deactivate(struct net_device *dev);
void qdisc_reset(struct Qdisc *qdisc);
void qdisc_destroy(struct Qdisc *qdisc);
struct Qdisc * qdisc_create_dflt(struct net_device *dev, struct Qdisc_ops *ops);
int qdisc_new_estimator(struct tc_stats *stats, spinlock_t *stats_lock, struct rtattr *opt);
void qdisc_kill_estimator(struct tc_stats *stats);
struct qdisc_rate_table *qdisc_get_rtab(struct tc_ratespec *r, struct rtattr *tab);
void qdisc_put_rtab(struct qdisc_rate_table *tab);

extern int qdisc_restart(struct net_device *dev);

/* Calculate maximal size of packet seen by hard_start_xmit
   routine of this device.
 */
static inline unsigned psched_mtu(struct net_device *dev)
{
	unsigned mtu = dev->mtu;
	return dev->hard_header ? mtu + dev->hard_header_len : mtu;
}

#endif
