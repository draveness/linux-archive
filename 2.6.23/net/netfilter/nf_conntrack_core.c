/* Connection state tracking for netfilter.  This is separated from,
   but required by, the NAT layer; it can also be used by an iptables
   extension. */

/* (C) 1999-2001 Paul `Rusty' Russell
 * (C) 2002-2006 Netfilter Core Team <coreteam@netfilter.org>
 * (C) 2003,2004 USAGI/WIDE Project <http://www.linux-ipv6.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/types.h>
#include <linux/netfilter.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <linux/stddef.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/jhash.h>
#include <linux/err.h>
#include <linux/percpu.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/socket.h>
#include <linux/mm.h>

#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_l3proto.h>
#include <net/netfilter/nf_conntrack_l4proto.h>
#include <net/netfilter/nf_conntrack_expect.h>
#include <net/netfilter/nf_conntrack_helper.h>
#include <net/netfilter/nf_conntrack_core.h>
#include <net/netfilter/nf_conntrack_extend.h>

#define NF_CONNTRACK_VERSION	"0.5.0"

DEFINE_RWLOCK(nf_conntrack_lock);
EXPORT_SYMBOL_GPL(nf_conntrack_lock);

/* nf_conntrack_standalone needs this */
atomic_t nf_conntrack_count = ATOMIC_INIT(0);
EXPORT_SYMBOL_GPL(nf_conntrack_count);

unsigned int nf_conntrack_htable_size __read_mostly;
EXPORT_SYMBOL_GPL(nf_conntrack_htable_size);

int nf_conntrack_max __read_mostly;
EXPORT_SYMBOL_GPL(nf_conntrack_max);

struct hlist_head *nf_conntrack_hash __read_mostly;
EXPORT_SYMBOL_GPL(nf_conntrack_hash);

struct nf_conn nf_conntrack_untracked __read_mostly;
EXPORT_SYMBOL_GPL(nf_conntrack_untracked);

unsigned int nf_ct_log_invalid __read_mostly;
HLIST_HEAD(unconfirmed);
static int nf_conntrack_vmalloc __read_mostly;
static struct kmem_cache *nf_conntrack_cachep __read_mostly;
static unsigned int nf_conntrack_next_id;

DEFINE_PER_CPU(struct ip_conntrack_stat, nf_conntrack_stat);
EXPORT_PER_CPU_SYMBOL(nf_conntrack_stat);

static int nf_conntrack_hash_rnd_initted;
static unsigned int nf_conntrack_hash_rnd;

static u_int32_t __hash_conntrack(const struct nf_conntrack_tuple *tuple,
				  unsigned int size, unsigned int rnd)
{
	unsigned int a, b;

	a = jhash2(tuple->src.u3.all, ARRAY_SIZE(tuple->src.u3.all),
		   (tuple->src.l3num << 16) | tuple->dst.protonum);
	b = jhash2(tuple->dst.u3.all, ARRAY_SIZE(tuple->dst.u3.all),
		   ((__force __u16)tuple->src.u.all << 16) |
		    (__force __u16)tuple->dst.u.all);

	return jhash_2words(a, b, rnd) % size;
}

static inline u_int32_t hash_conntrack(const struct nf_conntrack_tuple *tuple)
{
	return __hash_conntrack(tuple, nf_conntrack_htable_size,
				nf_conntrack_hash_rnd);
}

int
nf_ct_get_tuple(const struct sk_buff *skb,
		unsigned int nhoff,
		unsigned int dataoff,
		u_int16_t l3num,
		u_int8_t protonum,
		struct nf_conntrack_tuple *tuple,
		const struct nf_conntrack_l3proto *l3proto,
		const struct nf_conntrack_l4proto *l4proto)
{
	NF_CT_TUPLE_U_BLANK(tuple);

	tuple->src.l3num = l3num;
	if (l3proto->pkt_to_tuple(skb, nhoff, tuple) == 0)
		return 0;

	tuple->dst.protonum = protonum;
	tuple->dst.dir = IP_CT_DIR_ORIGINAL;

	return l4proto->pkt_to_tuple(skb, dataoff, tuple);
}
EXPORT_SYMBOL_GPL(nf_ct_get_tuple);

int nf_ct_get_tuplepr(const struct sk_buff *skb,
		      unsigned int nhoff,
		      u_int16_t l3num,
		      struct nf_conntrack_tuple *tuple)
{
	struct nf_conntrack_l3proto *l3proto;
	struct nf_conntrack_l4proto *l4proto;
	unsigned int protoff;
	u_int8_t protonum;
	int ret;

	rcu_read_lock();

	l3proto = __nf_ct_l3proto_find(l3num);
	ret = l3proto->get_l4proto(skb, nhoff, &protoff, &protonum);
	if (ret != NF_ACCEPT) {
		rcu_read_unlock();
		return 0;
	}

	l4proto = __nf_ct_l4proto_find(l3num, protonum);

	ret = nf_ct_get_tuple(skb, nhoff, protoff, l3num, protonum, tuple,
			      l3proto, l4proto);

	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(nf_ct_get_tuplepr);

int
nf_ct_invert_tuple(struct nf_conntrack_tuple *inverse,
		   const struct nf_conntrack_tuple *orig,
		   const struct nf_conntrack_l3proto *l3proto,
		   const struct nf_conntrack_l4proto *l4proto)
{
	NF_CT_TUPLE_U_BLANK(inverse);

	inverse->src.l3num = orig->src.l3num;
	if (l3proto->invert_tuple(inverse, orig) == 0)
		return 0;

	inverse->dst.dir = !orig->dst.dir;

	inverse->dst.protonum = orig->dst.protonum;
	return l4proto->invert_tuple(inverse, orig);
}
EXPORT_SYMBOL_GPL(nf_ct_invert_tuple);

static void
clean_from_lists(struct nf_conn *ct)
{
	pr_debug("clean_from_lists(%p)\n", ct);
	hlist_del(&ct->tuplehash[IP_CT_DIR_ORIGINAL].hnode);
	hlist_del(&ct->tuplehash[IP_CT_DIR_REPLY].hnode);

	/* Destroy all pending expectations */
	nf_ct_remove_expectations(ct);
}

static void
destroy_conntrack(struct nf_conntrack *nfct)
{
	struct nf_conn *ct = (struct nf_conn *)nfct;
	struct nf_conntrack_l4proto *l4proto;

	pr_debug("destroy_conntrack(%p)\n", ct);
	NF_CT_ASSERT(atomic_read(&nfct->use) == 0);
	NF_CT_ASSERT(!timer_pending(&ct->timeout));

	nf_conntrack_event(IPCT_DESTROY, ct);
	set_bit(IPS_DYING_BIT, &ct->status);

	/* To make sure we don't get any weird locking issues here:
	 * destroy_conntrack() MUST NOT be called with a write lock
	 * to nf_conntrack_lock!!! -HW */
	rcu_read_lock();
	l4proto = __nf_ct_l4proto_find(ct->tuplehash[IP_CT_DIR_REPLY].tuple.src.l3num,
				       ct->tuplehash[IP_CT_DIR_REPLY].tuple.dst.protonum);
	if (l4proto && l4proto->destroy)
		l4proto->destroy(ct);

	nf_ct_ext_destroy(ct);

	rcu_read_unlock();

	write_lock_bh(&nf_conntrack_lock);
	/* Expectations will have been removed in clean_from_lists,
	 * except TFTP can create an expectation on the first packet,
	 * before connection is in the list, so we need to clean here,
	 * too. */
	nf_ct_remove_expectations(ct);

	/* We overload first tuple to link into unconfirmed list. */
	if (!nf_ct_is_confirmed(ct)) {
		BUG_ON(hlist_unhashed(&ct->tuplehash[IP_CT_DIR_ORIGINAL].hnode));
		hlist_del(&ct->tuplehash[IP_CT_DIR_ORIGINAL].hnode);
	}

	NF_CT_STAT_INC(delete);
	write_unlock_bh(&nf_conntrack_lock);

	if (ct->master)
		nf_ct_put(ct->master);

	pr_debug("destroy_conntrack: returning ct=%p to slab\n", ct);
	nf_conntrack_free(ct);
}

static void death_by_timeout(unsigned long ul_conntrack)
{
	struct nf_conn *ct = (void *)ul_conntrack;
	struct nf_conn_help *help = nfct_help(ct);
	struct nf_conntrack_helper *helper;

	if (help) {
		rcu_read_lock();
		helper = rcu_dereference(help->helper);
		if (helper && helper->destroy)
			helper->destroy(ct);
		rcu_read_unlock();
	}

	write_lock_bh(&nf_conntrack_lock);
	/* Inside lock so preempt is disabled on module removal path.
	 * Otherwise we can get spurious warnings. */
	NF_CT_STAT_INC(delete_list);
	clean_from_lists(ct);
	write_unlock_bh(&nf_conntrack_lock);
	nf_ct_put(ct);
}

struct nf_conntrack_tuple_hash *
__nf_conntrack_find(const struct nf_conntrack_tuple *tuple,
		    const struct nf_conn *ignored_conntrack)
{
	struct nf_conntrack_tuple_hash *h;
	struct hlist_node *n;
	unsigned int hash = hash_conntrack(tuple);

	hlist_for_each_entry(h, n, &nf_conntrack_hash[hash], hnode) {
		if (nf_ct_tuplehash_to_ctrack(h) != ignored_conntrack &&
		    nf_ct_tuple_equal(tuple, &h->tuple)) {
			NF_CT_STAT_INC(found);
			return h;
		}
		NF_CT_STAT_INC(searched);
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(__nf_conntrack_find);

/* Find a connection corresponding to a tuple. */
struct nf_conntrack_tuple_hash *
nf_conntrack_find_get(const struct nf_conntrack_tuple *tuple)
{
	struct nf_conntrack_tuple_hash *h;

	read_lock_bh(&nf_conntrack_lock);
	h = __nf_conntrack_find(tuple, NULL);
	if (h)
		atomic_inc(&nf_ct_tuplehash_to_ctrack(h)->ct_general.use);
	read_unlock_bh(&nf_conntrack_lock);

	return h;
}
EXPORT_SYMBOL_GPL(nf_conntrack_find_get);

static void __nf_conntrack_hash_insert(struct nf_conn *ct,
				       unsigned int hash,
				       unsigned int repl_hash)
{
	ct->id = ++nf_conntrack_next_id;
	hlist_add_head(&ct->tuplehash[IP_CT_DIR_ORIGINAL].hnode,
		       &nf_conntrack_hash[hash]);
	hlist_add_head(&ct->tuplehash[IP_CT_DIR_REPLY].hnode,
		       &nf_conntrack_hash[repl_hash]);
}

void nf_conntrack_hash_insert(struct nf_conn *ct)
{
	unsigned int hash, repl_hash;

	hash = hash_conntrack(&ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple);
	repl_hash = hash_conntrack(&ct->tuplehash[IP_CT_DIR_REPLY].tuple);

	write_lock_bh(&nf_conntrack_lock);
	__nf_conntrack_hash_insert(ct, hash, repl_hash);
	write_unlock_bh(&nf_conntrack_lock);
}
EXPORT_SYMBOL_GPL(nf_conntrack_hash_insert);

/* Confirm a connection given skb; places it in hash table */
int
__nf_conntrack_confirm(struct sk_buff **pskb)
{
	unsigned int hash, repl_hash;
	struct nf_conntrack_tuple_hash *h;
	struct nf_conn *ct;
	struct nf_conn_help *help;
	struct hlist_node *n;
	enum ip_conntrack_info ctinfo;

	ct = nf_ct_get(*pskb, &ctinfo);

	/* ipt_REJECT uses nf_conntrack_attach to attach related
	   ICMP/TCP RST packets in other direction.  Actual packet
	   which created connection will be IP_CT_NEW or for an
	   expected connection, IP_CT_RELATED. */
	if (CTINFO2DIR(ctinfo) != IP_CT_DIR_ORIGINAL)
		return NF_ACCEPT;

	hash = hash_conntrack(&ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple);
	repl_hash = hash_conntrack(&ct->tuplehash[IP_CT_DIR_REPLY].tuple);

	/* We're not in hash table, and we refuse to set up related
	   connections for unconfirmed conns.  But packet copies and
	   REJECT will give spurious warnings here. */
	/* NF_CT_ASSERT(atomic_read(&ct->ct_general.use) == 1); */

	/* No external references means noone else could have
	   confirmed us. */
	NF_CT_ASSERT(!nf_ct_is_confirmed(ct));
	pr_debug("Confirming conntrack %p\n", ct);

	write_lock_bh(&nf_conntrack_lock);

	/* See if there's one in the list already, including reverse:
	   NAT could have grabbed it without realizing, since we're
	   not in the hash.  If there is, we lost race. */
	hlist_for_each_entry(h, n, &nf_conntrack_hash[hash], hnode)
		if (nf_ct_tuple_equal(&ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple,
				      &h->tuple))
			goto out;
	hlist_for_each_entry(h, n, &nf_conntrack_hash[repl_hash], hnode)
		if (nf_ct_tuple_equal(&ct->tuplehash[IP_CT_DIR_REPLY].tuple,
				      &h->tuple))
			goto out;

	/* Remove from unconfirmed list */
	hlist_del(&ct->tuplehash[IP_CT_DIR_ORIGINAL].hnode);

	__nf_conntrack_hash_insert(ct, hash, repl_hash);
	/* Timer relative to confirmation time, not original
	   setting time, otherwise we'd get timer wrap in
	   weird delay cases. */
	ct->timeout.expires += jiffies;
	add_timer(&ct->timeout);
	atomic_inc(&ct->ct_general.use);
	set_bit(IPS_CONFIRMED_BIT, &ct->status);
	NF_CT_STAT_INC(insert);
	write_unlock_bh(&nf_conntrack_lock);
	help = nfct_help(ct);
	if (help && help->helper)
		nf_conntrack_event_cache(IPCT_HELPER, *pskb);
#ifdef CONFIG_NF_NAT_NEEDED
	if (test_bit(IPS_SRC_NAT_DONE_BIT, &ct->status) ||
	    test_bit(IPS_DST_NAT_DONE_BIT, &ct->status))
		nf_conntrack_event_cache(IPCT_NATINFO, *pskb);
#endif
	nf_conntrack_event_cache(master_ct(ct) ?
				 IPCT_RELATED : IPCT_NEW, *pskb);
	return NF_ACCEPT;

out:
	NF_CT_STAT_INC(insert_failed);
	write_unlock_bh(&nf_conntrack_lock);
	return NF_DROP;
}
EXPORT_SYMBOL_GPL(__nf_conntrack_confirm);

/* Returns true if a connection correspondings to the tuple (required
   for NAT). */
int
nf_conntrack_tuple_taken(const struct nf_conntrack_tuple *tuple,
			 const struct nf_conn *ignored_conntrack)
{
	struct nf_conntrack_tuple_hash *h;

	read_lock_bh(&nf_conntrack_lock);
	h = __nf_conntrack_find(tuple, ignored_conntrack);
	read_unlock_bh(&nf_conntrack_lock);

	return h != NULL;
}
EXPORT_SYMBOL_GPL(nf_conntrack_tuple_taken);

#define NF_CT_EVICTION_RANGE	8

/* There's a small race here where we may free a just-assured
   connection.  Too bad: we're in trouble anyway. */
static int early_drop(unsigned int hash)
{
	/* Use oldest entry, which is roughly LRU */
	struct nf_conntrack_tuple_hash *h;
	struct nf_conn *ct = NULL, *tmp;
	struct hlist_node *n;
	unsigned int i, cnt = 0;
	int dropped = 0;

	read_lock_bh(&nf_conntrack_lock);
	for (i = 0; i < nf_conntrack_htable_size; i++) {
		hlist_for_each_entry(h, n, &nf_conntrack_hash[hash], hnode) {
			tmp = nf_ct_tuplehash_to_ctrack(h);
			if (!test_bit(IPS_ASSURED_BIT, &tmp->status))
				ct = tmp;
			cnt++;
		}
		if (ct || cnt >= NF_CT_EVICTION_RANGE)
			break;
		hash = (hash + 1) % nf_conntrack_htable_size;
	}
	if (ct)
		atomic_inc(&ct->ct_general.use);
	read_unlock_bh(&nf_conntrack_lock);

	if (!ct)
		return dropped;

	if (del_timer(&ct->timeout)) {
		death_by_timeout((unsigned long)ct);
		dropped = 1;
		NF_CT_STAT_INC_ATOMIC(early_drop);
	}
	nf_ct_put(ct);
	return dropped;
}

struct nf_conn *nf_conntrack_alloc(const struct nf_conntrack_tuple *orig,
				   const struct nf_conntrack_tuple *repl)
{
	struct nf_conn *conntrack = NULL;

	if (unlikely(!nf_conntrack_hash_rnd_initted)) {
		get_random_bytes(&nf_conntrack_hash_rnd, 4);
		nf_conntrack_hash_rnd_initted = 1;
	}

	/* We don't want any race condition at early drop stage */
	atomic_inc(&nf_conntrack_count);

	if (nf_conntrack_max
	    && atomic_read(&nf_conntrack_count) > nf_conntrack_max) {
		unsigned int hash = hash_conntrack(orig);
		if (!early_drop(hash)) {
			atomic_dec(&nf_conntrack_count);
			if (net_ratelimit())
				printk(KERN_WARNING
				       "nf_conntrack: table full, dropping"
				       " packet.\n");
			return ERR_PTR(-ENOMEM);
		}
	}

	conntrack = kmem_cache_zalloc(nf_conntrack_cachep, GFP_ATOMIC);
	if (conntrack == NULL) {
		pr_debug("nf_conntrack_alloc: Can't alloc conntrack.\n");
		atomic_dec(&nf_conntrack_count);
		return ERR_PTR(-ENOMEM);
	}

	atomic_set(&conntrack->ct_general.use, 1);
	conntrack->tuplehash[IP_CT_DIR_ORIGINAL].tuple = *orig;
	conntrack->tuplehash[IP_CT_DIR_REPLY].tuple = *repl;
	/* Don't set timer yet: wait for confirmation */
	setup_timer(&conntrack->timeout, death_by_timeout,
		    (unsigned long)conntrack);

	return conntrack;
}
EXPORT_SYMBOL_GPL(nf_conntrack_alloc);

void nf_conntrack_free(struct nf_conn *conntrack)
{
	nf_ct_ext_free(conntrack);
	kmem_cache_free(nf_conntrack_cachep, conntrack);
	atomic_dec(&nf_conntrack_count);
}
EXPORT_SYMBOL_GPL(nf_conntrack_free);

/* Allocate a new conntrack: we return -ENOMEM if classification
   failed due to stress.  Otherwise it really is unclassifiable. */
static struct nf_conntrack_tuple_hash *
init_conntrack(const struct nf_conntrack_tuple *tuple,
	       struct nf_conntrack_l3proto *l3proto,
	       struct nf_conntrack_l4proto *l4proto,
	       struct sk_buff *skb,
	       unsigned int dataoff)
{
	struct nf_conn *conntrack;
	struct nf_conn_help *help;
	struct nf_conntrack_tuple repl_tuple;
	struct nf_conntrack_expect *exp;

	if (!nf_ct_invert_tuple(&repl_tuple, tuple, l3proto, l4proto)) {
		pr_debug("Can't invert tuple.\n");
		return NULL;
	}

	conntrack = nf_conntrack_alloc(tuple, &repl_tuple);
	if (conntrack == NULL || IS_ERR(conntrack)) {
		pr_debug("Can't allocate conntrack.\n");
		return (struct nf_conntrack_tuple_hash *)conntrack;
	}

	if (!l4proto->new(conntrack, skb, dataoff)) {
		nf_conntrack_free(conntrack);
		pr_debug("init conntrack: can't track with proto module\n");
		return NULL;
	}

	write_lock_bh(&nf_conntrack_lock);
	exp = nf_ct_find_expectation(tuple);
	if (exp) {
		pr_debug("conntrack: expectation arrives ct=%p exp=%p\n",
			 conntrack, exp);
		/* Welcome, Mr. Bond.  We've been expecting you... */
		__set_bit(IPS_EXPECTED_BIT, &conntrack->status);
		conntrack->master = exp->master;
		if (exp->helper) {
			help = nf_ct_helper_ext_add(conntrack, GFP_ATOMIC);
			if (help)
				rcu_assign_pointer(help->helper, exp->helper);
		}

#ifdef CONFIG_NF_CONNTRACK_MARK
		conntrack->mark = exp->master->mark;
#endif
#ifdef CONFIG_NF_CONNTRACK_SECMARK
		conntrack->secmark = exp->master->secmark;
#endif
		nf_conntrack_get(&conntrack->master->ct_general);
		NF_CT_STAT_INC(expect_new);
	} else {
		struct nf_conntrack_helper *helper;

		helper = __nf_ct_helper_find(&repl_tuple);
		if (helper) {
			help = nf_ct_helper_ext_add(conntrack, GFP_ATOMIC);
			if (help)
				rcu_assign_pointer(help->helper, helper);
		}
		NF_CT_STAT_INC(new);
	}

	/* Overload tuple linked list to put us in unconfirmed list. */
	hlist_add_head(&conntrack->tuplehash[IP_CT_DIR_ORIGINAL].hnode,
		       &unconfirmed);

	write_unlock_bh(&nf_conntrack_lock);

	if (exp) {
		if (exp->expectfn)
			exp->expectfn(conntrack, exp);
		nf_ct_expect_put(exp);
	}

	return &conntrack->tuplehash[IP_CT_DIR_ORIGINAL];
}

/* On success, returns conntrack ptr, sets skb->nfct and ctinfo */
static inline struct nf_conn *
resolve_normal_ct(struct sk_buff *skb,
		  unsigned int dataoff,
		  u_int16_t l3num,
		  u_int8_t protonum,
		  struct nf_conntrack_l3proto *l3proto,
		  struct nf_conntrack_l4proto *l4proto,
		  int *set_reply,
		  enum ip_conntrack_info *ctinfo)
{
	struct nf_conntrack_tuple tuple;
	struct nf_conntrack_tuple_hash *h;
	struct nf_conn *ct;

	if (!nf_ct_get_tuple(skb, skb_network_offset(skb),
			     dataoff, l3num, protonum, &tuple, l3proto,
			     l4proto)) {
		pr_debug("resolve_normal_ct: Can't get tuple\n");
		return NULL;
	}

	/* look for tuple match */
	h = nf_conntrack_find_get(&tuple);
	if (!h) {
		h = init_conntrack(&tuple, l3proto, l4proto, skb, dataoff);
		if (!h)
			return NULL;
		if (IS_ERR(h))
			return (void *)h;
	}
	ct = nf_ct_tuplehash_to_ctrack(h);

	/* It exists; we have (non-exclusive) reference. */
	if (NF_CT_DIRECTION(h) == IP_CT_DIR_REPLY) {
		*ctinfo = IP_CT_ESTABLISHED + IP_CT_IS_REPLY;
		/* Please set reply bit if this packet OK */
		*set_reply = 1;
	} else {
		/* Once we've had two way comms, always ESTABLISHED. */
		if (test_bit(IPS_SEEN_REPLY_BIT, &ct->status)) {
			pr_debug("nf_conntrack_in: normal packet for %p\n", ct);
			*ctinfo = IP_CT_ESTABLISHED;
		} else if (test_bit(IPS_EXPECTED_BIT, &ct->status)) {
			pr_debug("nf_conntrack_in: related packet for %p\n",
				 ct);
			*ctinfo = IP_CT_RELATED;
		} else {
			pr_debug("nf_conntrack_in: new packet for %p\n", ct);
			*ctinfo = IP_CT_NEW;
		}
		*set_reply = 0;
	}
	skb->nfct = &ct->ct_general;
	skb->nfctinfo = *ctinfo;
	return ct;
}

unsigned int
nf_conntrack_in(int pf, unsigned int hooknum, struct sk_buff **pskb)
{
	struct nf_conn *ct;
	enum ip_conntrack_info ctinfo;
	struct nf_conntrack_l3proto *l3proto;
	struct nf_conntrack_l4proto *l4proto;
	unsigned int dataoff;
	u_int8_t protonum;
	int set_reply = 0;
	int ret;

	/* Previously seen (loopback or untracked)?  Ignore. */
	if ((*pskb)->nfct) {
		NF_CT_STAT_INC_ATOMIC(ignore);
		return NF_ACCEPT;
	}

	/* rcu_read_lock()ed by nf_hook_slow */
	l3proto = __nf_ct_l3proto_find((u_int16_t)pf);
	ret = l3proto->get_l4proto(*pskb, skb_network_offset(*pskb),
				   &dataoff, &protonum);
	if (ret <= 0) {
		pr_debug("not prepared to track yet or error occured\n");
		NF_CT_STAT_INC_ATOMIC(error);
		NF_CT_STAT_INC_ATOMIC(invalid);
		return -ret;
	}

	l4proto = __nf_ct_l4proto_find((u_int16_t)pf, protonum);

	/* It may be an special packet, error, unclean...
	 * inverse of the return code tells to the netfilter
	 * core what to do with the packet. */
	if (l4proto->error != NULL &&
	    (ret = l4proto->error(*pskb, dataoff, &ctinfo, pf, hooknum)) <= 0) {
		NF_CT_STAT_INC_ATOMIC(error);
		NF_CT_STAT_INC_ATOMIC(invalid);
		return -ret;
	}

	ct = resolve_normal_ct(*pskb, dataoff, pf, protonum, l3proto, l4proto,
			       &set_reply, &ctinfo);
	if (!ct) {
		/* Not valid part of a connection */
		NF_CT_STAT_INC_ATOMIC(invalid);
		return NF_ACCEPT;
	}

	if (IS_ERR(ct)) {
		/* Too stressed to deal. */
		NF_CT_STAT_INC_ATOMIC(drop);
		return NF_DROP;
	}

	NF_CT_ASSERT((*pskb)->nfct);

	ret = l4proto->packet(ct, *pskb, dataoff, ctinfo, pf, hooknum);
	if (ret < 0) {
		/* Invalid: inverse of the return code tells
		 * the netfilter core what to do */
		pr_debug("nf_conntrack_in: Can't track with proto module\n");
		nf_conntrack_put((*pskb)->nfct);
		(*pskb)->nfct = NULL;
		NF_CT_STAT_INC_ATOMIC(invalid);
		return -ret;
	}

	if (set_reply && !test_and_set_bit(IPS_SEEN_REPLY_BIT, &ct->status))
		nf_conntrack_event_cache(IPCT_STATUS, *pskb);

	return ret;
}
EXPORT_SYMBOL_GPL(nf_conntrack_in);

int nf_ct_invert_tuplepr(struct nf_conntrack_tuple *inverse,
			 const struct nf_conntrack_tuple *orig)
{
	int ret;

	rcu_read_lock();
	ret = nf_ct_invert_tuple(inverse, orig,
				 __nf_ct_l3proto_find(orig->src.l3num),
				 __nf_ct_l4proto_find(orig->src.l3num,
						      orig->dst.protonum));
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(nf_ct_invert_tuplepr);

/* Alter reply tuple (maybe alter helper).  This is for NAT, and is
   implicitly racy: see __nf_conntrack_confirm */
void nf_conntrack_alter_reply(struct nf_conn *ct,
			      const struct nf_conntrack_tuple *newreply)
{
	struct nf_conn_help *help = nfct_help(ct);
	struct nf_conntrack_helper *helper;

	write_lock_bh(&nf_conntrack_lock);
	/* Should be unconfirmed, so not in hash table yet */
	NF_CT_ASSERT(!nf_ct_is_confirmed(ct));

	pr_debug("Altering reply tuple of %p to ", ct);
	NF_CT_DUMP_TUPLE(newreply);

	ct->tuplehash[IP_CT_DIR_REPLY].tuple = *newreply;
	if (ct->master || (help && help->expecting != 0))
		goto out;

	helper = __nf_ct_helper_find(newreply);
	if (helper == NULL) {
		if (help)
			rcu_assign_pointer(help->helper, NULL);
		goto out;
	}

	if (help == NULL) {
		help = nf_ct_helper_ext_add(ct, GFP_ATOMIC);
		if (help == NULL)
			goto out;
	} else {
		memset(&help->help, 0, sizeof(help->help));
	}

	rcu_assign_pointer(help->helper, helper);
out:
	write_unlock_bh(&nf_conntrack_lock);
}
EXPORT_SYMBOL_GPL(nf_conntrack_alter_reply);

/* Refresh conntrack for this many jiffies and do accounting if do_acct is 1 */
void __nf_ct_refresh_acct(struct nf_conn *ct,
			  enum ip_conntrack_info ctinfo,
			  const struct sk_buff *skb,
			  unsigned long extra_jiffies,
			  int do_acct)
{
	int event = 0;

	NF_CT_ASSERT(ct->timeout.data == (unsigned long)ct);
	NF_CT_ASSERT(skb);

	write_lock_bh(&nf_conntrack_lock);

	/* Only update if this is not a fixed timeout */
	if (test_bit(IPS_FIXED_TIMEOUT_BIT, &ct->status)) {
		write_unlock_bh(&nf_conntrack_lock);
		return;
	}

	/* If not in hash table, timer will not be active yet */
	if (!nf_ct_is_confirmed(ct)) {
		ct->timeout.expires = extra_jiffies;
		event = IPCT_REFRESH;
	} else {
		unsigned long newtime = jiffies + extra_jiffies;

		/* Only update the timeout if the new timeout is at least
		   HZ jiffies from the old timeout. Need del_timer for race
		   avoidance (may already be dying). */
		if (newtime - ct->timeout.expires >= HZ
		    && del_timer(&ct->timeout)) {
			ct->timeout.expires = newtime;
			add_timer(&ct->timeout);
			event = IPCT_REFRESH;
		}
	}

#ifdef CONFIG_NF_CT_ACCT
	if (do_acct) {
		ct->counters[CTINFO2DIR(ctinfo)].packets++;
		ct->counters[CTINFO2DIR(ctinfo)].bytes +=
			skb->len - skb_network_offset(skb);

		if ((ct->counters[CTINFO2DIR(ctinfo)].packets & 0x80000000)
		    || (ct->counters[CTINFO2DIR(ctinfo)].bytes & 0x80000000))
			event |= IPCT_COUNTER_FILLING;
	}
#endif

	write_unlock_bh(&nf_conntrack_lock);

	/* must be unlocked when calling event cache */
	if (event)
		nf_conntrack_event_cache(event, skb);
}
EXPORT_SYMBOL_GPL(__nf_ct_refresh_acct);

#if defined(CONFIG_NF_CT_NETLINK) || defined(CONFIG_NF_CT_NETLINK_MODULE)

#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_conntrack.h>
#include <linux/mutex.h>


/* Generic function for tcp/udp/sctp/dccp and alike. This needs to be
 * in ip_conntrack_core, since we don't want the protocols to autoload
 * or depend on ctnetlink */
int nf_ct_port_tuple_to_nfattr(struct sk_buff *skb,
			       const struct nf_conntrack_tuple *tuple)
{
	NFA_PUT(skb, CTA_PROTO_SRC_PORT, sizeof(u_int16_t),
		&tuple->src.u.tcp.port);
	NFA_PUT(skb, CTA_PROTO_DST_PORT, sizeof(u_int16_t),
		&tuple->dst.u.tcp.port);
	return 0;

nfattr_failure:
	return -1;
}
EXPORT_SYMBOL_GPL(nf_ct_port_tuple_to_nfattr);

static const size_t cta_min_proto[CTA_PROTO_MAX] = {
	[CTA_PROTO_SRC_PORT-1]  = sizeof(u_int16_t),
	[CTA_PROTO_DST_PORT-1]  = sizeof(u_int16_t)
};

int nf_ct_port_nfattr_to_tuple(struct nfattr *tb[],
			       struct nf_conntrack_tuple *t)
{
	if (!tb[CTA_PROTO_SRC_PORT-1] || !tb[CTA_PROTO_DST_PORT-1])
		return -EINVAL;

	if (nfattr_bad_size(tb, CTA_PROTO_MAX, cta_min_proto))
		return -EINVAL;

	t->src.u.tcp.port = *(__be16 *)NFA_DATA(tb[CTA_PROTO_SRC_PORT-1]);
	t->dst.u.tcp.port = *(__be16 *)NFA_DATA(tb[CTA_PROTO_DST_PORT-1]);

	return 0;
}
EXPORT_SYMBOL_GPL(nf_ct_port_nfattr_to_tuple);
#endif

/* Used by ipt_REJECT and ip6t_REJECT. */
void __nf_conntrack_attach(struct sk_buff *nskb, struct sk_buff *skb)
{
	struct nf_conn *ct;
	enum ip_conntrack_info ctinfo;

	/* This ICMP is in reverse direction to the packet which caused it */
	ct = nf_ct_get(skb, &ctinfo);
	if (CTINFO2DIR(ctinfo) == IP_CT_DIR_ORIGINAL)
		ctinfo = IP_CT_RELATED + IP_CT_IS_REPLY;
	else
		ctinfo = IP_CT_RELATED;

	/* Attach to new skbuff, and increment count */
	nskb->nfct = &ct->ct_general;
	nskb->nfctinfo = ctinfo;
	nf_conntrack_get(nskb->nfct);
}
EXPORT_SYMBOL_GPL(__nf_conntrack_attach);

static inline int
do_iter(const struct nf_conntrack_tuple_hash *i,
	int (*iter)(struct nf_conn *i, void *data),
	void *data)
{
	return iter(nf_ct_tuplehash_to_ctrack(i), data);
}

/* Bring out ya dead! */
static struct nf_conn *
get_next_corpse(int (*iter)(struct nf_conn *i, void *data),
		void *data, unsigned int *bucket)
{
	struct nf_conntrack_tuple_hash *h;
	struct nf_conn *ct;
	struct hlist_node *n;

	write_lock_bh(&nf_conntrack_lock);
	for (; *bucket < nf_conntrack_htable_size; (*bucket)++) {
		hlist_for_each_entry(h, n, &nf_conntrack_hash[*bucket], hnode) {
			ct = nf_ct_tuplehash_to_ctrack(h);
			if (iter(ct, data))
				goto found;
		}
	}
	hlist_for_each_entry(h, n, &unconfirmed, hnode) {
		ct = nf_ct_tuplehash_to_ctrack(h);
		if (iter(ct, data))
			set_bit(IPS_DYING_BIT, &ct->status);
	}
	write_unlock_bh(&nf_conntrack_lock);
	return NULL;
found:
	atomic_inc(&ct->ct_general.use);
	write_unlock_bh(&nf_conntrack_lock);
	return ct;
}

void
nf_ct_iterate_cleanup(int (*iter)(struct nf_conn *i, void *data), void *data)
{
	struct nf_conn *ct;
	unsigned int bucket = 0;

	while ((ct = get_next_corpse(iter, data, &bucket)) != NULL) {
		/* Time to push up daises... */
		if (del_timer(&ct->timeout))
			death_by_timeout((unsigned long)ct);
		/* ... else the timer will get him soon. */

		nf_ct_put(ct);
	}
}
EXPORT_SYMBOL_GPL(nf_ct_iterate_cleanup);

static int kill_all(struct nf_conn *i, void *data)
{
	return 1;
}

void nf_ct_free_hashtable(struct hlist_head *hash, int vmalloced, int size)
{
	if (vmalloced)
		vfree(hash);
	else
		free_pages((unsigned long)hash,
			   get_order(sizeof(struct hlist_head) * size));
}
EXPORT_SYMBOL_GPL(nf_ct_free_hashtable);

void nf_conntrack_flush(void)
{
	nf_ct_iterate_cleanup(kill_all, NULL);
}
EXPORT_SYMBOL_GPL(nf_conntrack_flush);

/* Mishearing the voices in his head, our hero wonders how he's
   supposed to kill the mall. */
void nf_conntrack_cleanup(void)
{
	rcu_assign_pointer(ip_ct_attach, NULL);

	/* This makes sure all current packets have passed through
	   netfilter framework.  Roll on, two-stage module
	   delete... */
	synchronize_net();

	nf_ct_event_cache_flush();
 i_see_dead_people:
	nf_conntrack_flush();
	if (atomic_read(&nf_conntrack_count) != 0) {
		schedule();
		goto i_see_dead_people;
	}
	/* wait until all references to nf_conntrack_untracked are dropped */
	while (atomic_read(&nf_conntrack_untracked.ct_general.use) > 1)
		schedule();

	rcu_assign_pointer(nf_ct_destroy, NULL);

	kmem_cache_destroy(nf_conntrack_cachep);
	nf_ct_free_hashtable(nf_conntrack_hash, nf_conntrack_vmalloc,
			     nf_conntrack_htable_size);

	nf_conntrack_proto_fini();
	nf_conntrack_helper_fini();
	nf_conntrack_expect_fini();
}

struct hlist_head *nf_ct_alloc_hashtable(int *sizep, int *vmalloced)
{
	struct hlist_head *hash;
	unsigned int size, i;

	*vmalloced = 0;

	size = *sizep = roundup(*sizep, PAGE_SIZE / sizeof(struct hlist_head));
	hash = (void*)__get_free_pages(GFP_KERNEL,
				       get_order(sizeof(struct hlist_head)
						 * size));
	if (!hash) {
		*vmalloced = 1;
		printk(KERN_WARNING "nf_conntrack: falling back to vmalloc.\n");
		hash = vmalloc(sizeof(struct hlist_head) * size);
	}

	if (hash)
		for (i = 0; i < size; i++)
			INIT_HLIST_HEAD(&hash[i]);

	return hash;
}
EXPORT_SYMBOL_GPL(nf_ct_alloc_hashtable);

int set_hashsize(const char *val, struct kernel_param *kp)
{
	int i, bucket, hashsize, vmalloced;
	int old_vmalloced, old_size;
	int rnd;
	struct hlist_head *hash, *old_hash;
	struct nf_conntrack_tuple_hash *h;

	/* On boot, we can set this without any fancy locking. */
	if (!nf_conntrack_htable_size)
		return param_set_uint(val, kp);

	hashsize = simple_strtol(val, NULL, 0);
	if (!hashsize)
		return -EINVAL;

	hash = nf_ct_alloc_hashtable(&hashsize, &vmalloced);
	if (!hash)
		return -ENOMEM;

	/* We have to rehahs for the new table anyway, so we also can
	 * use a newrandom seed */
	get_random_bytes(&rnd, 4);

	write_lock_bh(&nf_conntrack_lock);
	for (i = 0; i < nf_conntrack_htable_size; i++) {
		while (!hlist_empty(&nf_conntrack_hash[i])) {
			h = hlist_entry(nf_conntrack_hash[i].first,
					struct nf_conntrack_tuple_hash, hnode);
			hlist_del(&h->hnode);
			bucket = __hash_conntrack(&h->tuple, hashsize, rnd);
			hlist_add_head(&h->hnode, &hash[bucket]);
		}
	}
	old_size = nf_conntrack_htable_size;
	old_vmalloced = nf_conntrack_vmalloc;
	old_hash = nf_conntrack_hash;

	nf_conntrack_htable_size = hashsize;
	nf_conntrack_vmalloc = vmalloced;
	nf_conntrack_hash = hash;
	nf_conntrack_hash_rnd = rnd;
	write_unlock_bh(&nf_conntrack_lock);

	nf_ct_free_hashtable(old_hash, old_vmalloced, old_size);
	return 0;
}

module_param_call(hashsize, set_hashsize, param_get_uint,
		  &nf_conntrack_htable_size, 0600);

int __init nf_conntrack_init(void)
{
	int max_factor = 8;
	int ret;

	/* Idea from tcp.c: use 1/16384 of memory.  On i386: 32MB
	 * machine has 512 buckets. >= 1GB machines have 16384 buckets. */
	if (!nf_conntrack_htable_size) {
		nf_conntrack_htable_size
			= (((num_physpages << PAGE_SHIFT) / 16384)
			   / sizeof(struct hlist_head));
		if (num_physpages > (1024 * 1024 * 1024 / PAGE_SIZE))
			nf_conntrack_htable_size = 16384;
		if (nf_conntrack_htable_size < 32)
			nf_conntrack_htable_size = 32;

		/* Use a max. factor of four by default to get the same max as
		 * with the old struct list_heads. When a table size is given
		 * we use the old value of 8 to avoid reducing the max.
		 * entries. */
		max_factor = 4;
	}
	nf_conntrack_hash = nf_ct_alloc_hashtable(&nf_conntrack_htable_size,
						  &nf_conntrack_vmalloc);
	if (!nf_conntrack_hash) {
		printk(KERN_ERR "Unable to create nf_conntrack_hash\n");
		goto err_out;
	}

	nf_conntrack_max = max_factor * nf_conntrack_htable_size;

	printk("nf_conntrack version %s (%u buckets, %d max)\n",
	       NF_CONNTRACK_VERSION, nf_conntrack_htable_size,
	       nf_conntrack_max);

	nf_conntrack_cachep = kmem_cache_create("nf_conntrack",
						sizeof(struct nf_conn),
						0, 0, NULL);
	if (!nf_conntrack_cachep) {
		printk(KERN_ERR "Unable to create nf_conn slab cache\n");
		goto err_free_hash;
	}

	ret = nf_conntrack_proto_init();
	if (ret < 0)
		goto err_free_conntrack_slab;

	ret = nf_conntrack_expect_init();
	if (ret < 0)
		goto out_fini_proto;

	ret = nf_conntrack_helper_init();
	if (ret < 0)
		goto out_fini_expect;

	/* For use by REJECT target */
	rcu_assign_pointer(ip_ct_attach, __nf_conntrack_attach);
	rcu_assign_pointer(nf_ct_destroy, destroy_conntrack);

	/* Set up fake conntrack:
	    - to never be deleted, not in any hashes */
	atomic_set(&nf_conntrack_untracked.ct_general.use, 1);
	/*  - and look it like as a confirmed connection */
	set_bit(IPS_CONFIRMED_BIT, &nf_conntrack_untracked.status);

	return ret;

out_fini_expect:
	nf_conntrack_expect_fini();
out_fini_proto:
	nf_conntrack_proto_fini();
err_free_conntrack_slab:
	kmem_cache_destroy(nf_conntrack_cachep);
err_free_hash:
	nf_ct_free_hashtable(nf_conntrack_hash, nf_conntrack_vmalloc,
			     nf_conntrack_htable_size);
err_out:
	return -ENOMEM;
}
