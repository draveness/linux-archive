/*
 *  ebtables
 *
 *  Author:
 *  Bart De Schuymer		<bdschuym@pandora.be>
 *
 *  ebtables.c,v 2.0, July, 2002
 *
 *  This code is stongly inspired on the iptables code which is
 *  Copyright (C) 1999 Paul `Rusty' Russell & Michael J. Neuling
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 */

/* used for print_string */
#include <linux/sched.h>
#include <linux/tty.h>

#include <linux/kmod.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/netfilter_bridge/ebtables.h>
#include <linux/spinlock.h>
#include <asm/uaccess.h>
#include <linux/smp.h>
#include <net/sock.h>
/* needed for logical [in,out]-dev filtering */
#include "../br_private.h"

/* list_named_find */
#define ASSERT_READ_LOCK(x)
#define ASSERT_WRITE_LOCK(x)
#include <linux/netfilter_ipv4/listhelp.h>

#if 0
/* use this for remote debugging
 * Copyright (C) 1998 by Ori Pomerantz
 * Print the string to the appropriate tty, the one
 * the current task uses
 */
static void print_string(char *str)
{
	struct tty_struct *my_tty;

	/* The tty for the current task */
	my_tty = current->signal->tty;
	if (my_tty != NULL) {
		my_tty->driver->write(my_tty, 0, str, strlen(str));
		my_tty->driver->write(my_tty, 0, "\015\012", 2);
	}
}

#define BUGPRINT(args) print_string(args);
#else
#define BUGPRINT(format, args...) printk("kernel msg: ebtables bug: please "\
                                         "report to author: "format, ## args)
/* #define BUGPRINT(format, args...) */
#endif
#define MEMPRINT(format, args...) printk("kernel msg: ebtables "\
                                         ": out of memory: "format, ## args)
/* #define MEMPRINT(format, args...) */



/*
 * Each cpu has its own set of counters, so there is no need for write_lock in
 * the softirq
 * For reading or updating the counters, the user context needs to
 * get a write_lock
 */

/* The size of each set of counters is altered to get cache alignment */
#define SMP_ALIGN(x) (((x) + SMP_CACHE_BYTES-1) & ~(SMP_CACHE_BYTES-1))
#define COUNTER_OFFSET(n) (SMP_ALIGN(n * sizeof(struct ebt_counter)))
#define COUNTER_BASE(c, n, cpu) ((struct ebt_counter *)(((char *)c) + \
   COUNTER_OFFSET(n) * cpu))



static DECLARE_MUTEX(ebt_mutex);
static LIST_HEAD(ebt_tables);
static LIST_HEAD(ebt_targets);
static LIST_HEAD(ebt_matches);
static LIST_HEAD(ebt_watchers);

static struct ebt_target ebt_standard_target =
{ {NULL, NULL}, EBT_STANDARD_TARGET, NULL, NULL, NULL, NULL};

static inline int ebt_do_watcher (struct ebt_entry_watcher *w,
   const struct sk_buff *skb, const struct net_device *in,
   const struct net_device *out)
{
	w->u.watcher->watcher(skb, in, out, w->data,
	   w->watcher_size);
	/* watchers don't give a verdict */
	return 0;
}

static inline int ebt_do_match (struct ebt_entry_match *m,
   const struct sk_buff *skb, const struct net_device *in,
   const struct net_device *out)
{
	return m->u.match->match(skb, in, out, m->data,
	   m->match_size);
}

static inline int ebt_dev_check(char *entry, const struct net_device *device)
{
	if (*entry == '\0')
		return 0;
	if (!device)
		return 1;
	return !!strcmp(entry, device->name);
}

#define FWINV2(bool,invflg) ((bool) ^ !!(e->invflags & invflg))
/* process standard matches */
static inline int ebt_basic_match(struct ebt_entry *e, struct ethhdr *h,
   const struct net_device *in, const struct net_device *out)
{
	int verdict, i;

	if (e->bitmask & EBT_802_3) {
		if (FWINV2(ntohs(h->h_proto) >= 1536, EBT_IPROTO))
			return 1;
	} else if (!(e->bitmask & EBT_NOPROTO) &&
	   FWINV2(e->ethproto != h->h_proto, EBT_IPROTO))
		return 1;

	if (FWINV2(ebt_dev_check(e->in, in), EBT_IIN))
		return 1;
	if (FWINV2(ebt_dev_check(e->out, out), EBT_IOUT))
		return 1;
	if ((!in || !in->br_port) ? 0 : FWINV2(ebt_dev_check(
	   e->logical_in, in->br_port->br->dev), EBT_ILOGICALIN))
		return 1;
	if ((!out || !out->br_port) ? 0 : FWINV2(ebt_dev_check(
	   e->logical_out, out->br_port->br->dev), EBT_ILOGICALOUT))
		return 1;

	if (e->bitmask & EBT_SOURCEMAC) {
		verdict = 0;
		for (i = 0; i < 6; i++)
			verdict |= (h->h_source[i] ^ e->sourcemac[i]) &
			   e->sourcemsk[i];
		if (FWINV2(verdict != 0, EBT_ISOURCE) )
			return 1;
	}
	if (e->bitmask & EBT_DESTMAC) {
		verdict = 0;
		for (i = 0; i < 6; i++)
			verdict |= (h->h_dest[i] ^ e->destmac[i]) &
			   e->destmsk[i];
		if (FWINV2(verdict != 0, EBT_IDEST) )
			return 1;
	}
	return 0;
}

/* Do some firewalling */
unsigned int ebt_do_table (unsigned int hook, struct sk_buff **pskb,
   const struct net_device *in, const struct net_device *out,
   struct ebt_table *table)
{
	int i, nentries;
	struct ebt_entry *point;
	struct ebt_counter *counter_base, *cb_base;
	struct ebt_entry_target *t;
	int verdict, sp = 0;
	struct ebt_chainstack *cs;
	struct ebt_entries *chaininfo;
	char *base;
	struct ebt_table_info *private = table->private;

	read_lock_bh(&table->lock);
	cb_base = COUNTER_BASE(private->counters, private->nentries,
	   smp_processor_id());
	if (private->chainstack)
		cs = private->chainstack[smp_processor_id()];
	else
		cs = NULL;
	chaininfo = private->hook_entry[hook];
	nentries = private->hook_entry[hook]->nentries;
	point = (struct ebt_entry *)(private->hook_entry[hook]->data);
	counter_base = cb_base + private->hook_entry[hook]->counter_offset;
	/* base for chain jumps */
	base = private->entries;
	i = 0;
	while (i < nentries) {
		if (ebt_basic_match(point, (**pskb).mac.ethernet, in, out))
			goto letscontinue;

		if (EBT_MATCH_ITERATE(point, ebt_do_match, *pskb, in, out) != 0)
			goto letscontinue;

		/* increase counter */
		(*(counter_base + i)).pcnt++;
		(*(counter_base + i)).bcnt+=(**pskb).len;

		/* these should only watch: not modify, nor tell us
		   what to do with the packet */
		EBT_WATCHER_ITERATE(point, ebt_do_watcher, *pskb, in,
		   out);

		t = (struct ebt_entry_target *)
		   (((char *)point) + point->target_offset);
		/* standard target */
		if (!t->u.target->target)
			verdict = ((struct ebt_standard_target *)t)->verdict;
		else
			verdict = t->u.target->target(pskb, hook,
			   in, out, t->data, t->target_size);
		if (verdict == EBT_ACCEPT) {
			read_unlock_bh(&table->lock);
			return NF_ACCEPT;
		}
		if (verdict == EBT_DROP) {
			read_unlock_bh(&table->lock);
			return NF_DROP;
		}
		if (verdict == EBT_RETURN) {
letsreturn:
#ifdef CONFIG_NETFILTER_DEBUG
			if (sp == 0) {
				BUGPRINT("RETURN on base chain");
				/* act like this is EBT_CONTINUE */
				goto letscontinue;
			}
#endif
			sp--;
			/* put all the local variables right */
			i = cs[sp].n;
			chaininfo = cs[sp].chaininfo;
			nentries = chaininfo->nentries;
			point = cs[sp].e;
			counter_base = cb_base +
			   chaininfo->counter_offset;
			continue;
		}
		if (verdict == EBT_CONTINUE)
			goto letscontinue;
#ifdef CONFIG_NETFILTER_DEBUG
		if (verdict < 0) {
			BUGPRINT("bogus standard verdict\n");
			read_unlock_bh(&table->lock);
			return NF_DROP;
		}
#endif
		/* jump to a udc */
		cs[sp].n = i + 1;
		cs[sp].chaininfo = chaininfo;
		cs[sp].e = (struct ebt_entry *)
		   (((char *)point) + point->next_offset);
		i = 0;
		chaininfo = (struct ebt_entries *) (base + verdict);
#ifdef CONFIG_NETFILTER_DEBUG
		if (chaininfo->distinguisher) {
			BUGPRINT("jump to non-chain\n");
			read_unlock_bh(&table->lock);
			return NF_DROP;
		}
#endif
		nentries = chaininfo->nentries;
		point = (struct ebt_entry *)chaininfo->data;
		counter_base = cb_base + chaininfo->counter_offset;
		sp++;
		continue;
letscontinue:
		point = (struct ebt_entry *)
		   (((char *)point) + point->next_offset);
		i++;
	}

	/* I actually like this :) */
	if (chaininfo->policy == EBT_RETURN)
		goto letsreturn;
	if (chaininfo->policy == EBT_ACCEPT) {
		read_unlock_bh(&table->lock);
		return NF_ACCEPT;
	}
	read_unlock_bh(&table->lock);
	return NF_DROP;
}

/* If it succeeds, returns element and locks mutex */
static inline void *
find_inlist_lock_noload(struct list_head *head, const char *name, int *error,
   struct semaphore *mutex)
{
	void *ret;

	*error = down_interruptible(mutex);
	if (*error != 0)
		return NULL;

	ret = list_named_find(head, name);
	if (!ret) {
		*error = -ENOENT;
		up(mutex);
	}
	return ret;
}

#ifndef CONFIG_KMOD
#define find_inlist_lock(h,n,p,e,m) find_inlist_lock_noload((h),(n),(e),(m))
#else
static void *
find_inlist_lock(struct list_head *head, const char *name, const char *prefix,
   int *error, struct semaphore *mutex)
{
	void *ret;

	ret = find_inlist_lock_noload(head, name, error, mutex);
	if (!ret) {
		request_module("%s%s", prefix, name);
		ret = find_inlist_lock_noload(head, name, error, mutex);
	}
	return ret;
}
#endif

static inline struct ebt_table *
find_table_lock(const char *name, int *error, struct semaphore *mutex)
{
	return find_inlist_lock(&ebt_tables, name, "ebtable_", error, mutex);
}

static inline struct ebt_match *
find_match_lock(const char *name, int *error, struct semaphore *mutex)
{
	return find_inlist_lock(&ebt_matches, name, "ebt_", error, mutex);
}

static inline struct ebt_watcher *
find_watcher_lock(const char *name, int *error, struct semaphore *mutex)
{
	return find_inlist_lock(&ebt_watchers, name, "ebt_", error, mutex);
}

static inline struct ebt_target *
find_target_lock(const char *name, int *error, struct semaphore *mutex)
{
	return find_inlist_lock(&ebt_targets, name, "ebt_", error, mutex);
}

static inline int
ebt_check_match(struct ebt_entry_match *m, struct ebt_entry *e,
   const char *name, unsigned int hookmask, unsigned int *cnt)
{
	struct ebt_match *match;
	int ret;

	if (((char *)m) + m->match_size + sizeof(struct ebt_entry_match) >
	   ((char *)e) + e->watchers_offset)
		return -EINVAL;
	match = find_match_lock(m->u.name, &ret, &ebt_mutex);
	if (!match)
		return ret;
	m->u.match = match;
	if (!try_module_get(match->me)) {
		up(&ebt_mutex);
		return -ENOENT;
	}
	up(&ebt_mutex);
	if (match->check &&
	   match->check(name, hookmask, e, m->data, m->match_size) != 0) {
		BUGPRINT("match->check failed\n");
		module_put(match->me);
		return -EINVAL;
	}
	(*cnt)++;
	return 0;
}

static inline int
ebt_check_watcher(struct ebt_entry_watcher *w, struct ebt_entry *e,
   const char *name, unsigned int hookmask, unsigned int *cnt)
{
	struct ebt_watcher *watcher;
	int ret;

	if (((char *)w) + w->watcher_size + sizeof(struct ebt_entry_watcher) >
	   ((char *)e) + e->target_offset)
		return -EINVAL;
	watcher = find_watcher_lock(w->u.name, &ret, &ebt_mutex);
	if (!watcher)
		return ret;
	w->u.watcher = watcher;
	if (!try_module_get(watcher->me)) {
		up(&ebt_mutex);
		return -ENOENT;
	}
	up(&ebt_mutex);
	if (watcher->check &&
	   watcher->check(name, hookmask, e, w->data, w->watcher_size) != 0) {
		BUGPRINT("watcher->check failed\n");
		module_put(watcher->me);
		return -EINVAL;
	}
	(*cnt)++;
	return 0;
}

/*
 * this one is very careful, as it is the first function
 * to parse the userspace data
 */
static inline int
ebt_check_entry_size_and_hooks(struct ebt_entry *e,
   struct ebt_table_info *newinfo, char *base, char *limit,
   struct ebt_entries **hook_entries, unsigned int *n, unsigned int *cnt,
   unsigned int *totalcnt, unsigned int *udc_cnt, unsigned int valid_hooks)
{
	int i;

	for (i = 0; i < NF_BR_NUMHOOKS; i++) {
		if ((valid_hooks & (1 << i)) == 0)
			continue;
		if ( (char *)hook_entries[i] - base ==
		   (char *)e - newinfo->entries)
			break;
	}
	/* beginning of a new chain
	   if i == NF_BR_NUMHOOKS it must be a user defined chain */
	if (i != NF_BR_NUMHOOKS || !(e->bitmask & EBT_ENTRY_OR_ENTRIES)) {
		if ((e->bitmask & EBT_ENTRY_OR_ENTRIES) != 0) {
			/* we make userspace set this right,
			   so there is no misunderstanding */
			BUGPRINT("EBT_ENTRY_OR_ENTRIES shouldn't be set "
			         "in distinguisher\n");
			return -EINVAL;
		}
		/* this checks if the previous chain has as many entries
		   as it said it has */
		if (*n != *cnt) {
			BUGPRINT("nentries does not equal the nr of entries "
		                 "in the chain\n");
			return -EINVAL;
		}
		/* before we look at the struct, be sure it is not too big */
		if ((char *)hook_entries[i] + sizeof(struct ebt_entries)
		   > limit) {
			BUGPRINT("entries_size too small\n");
			return -EINVAL;
		}
		if (((struct ebt_entries *)e)->policy != EBT_DROP &&
		   ((struct ebt_entries *)e)->policy != EBT_ACCEPT) {
			/* only RETURN from udc */
			if (i != NF_BR_NUMHOOKS ||
			   ((struct ebt_entries *)e)->policy != EBT_RETURN) {
				BUGPRINT("bad policy\n");
				return -EINVAL;
			}
		}
		if (i == NF_BR_NUMHOOKS) /* it's a user defined chain */
			(*udc_cnt)++;
		else
			newinfo->hook_entry[i] = (struct ebt_entries *)e;
		if (((struct ebt_entries *)e)->counter_offset != *totalcnt) {
			BUGPRINT("counter_offset != totalcnt");
			return -EINVAL;
		}
		*n = ((struct ebt_entries *)e)->nentries;
		*cnt = 0;
		return 0;
	}
	/* a plain old entry, heh */
	if (sizeof(struct ebt_entry) > e->watchers_offset ||
	   e->watchers_offset > e->target_offset ||
	   e->target_offset >= e->next_offset) {
		BUGPRINT("entry offsets not in right order\n");
		return -EINVAL;
	}
	/* this is not checked anywhere else */
	if (e->next_offset - e->target_offset < sizeof(struct ebt_entry_target)) {
		BUGPRINT("target size too small\n");
		return -EINVAL;
	}

	(*cnt)++;
	(*totalcnt)++;
	return 0;
}

struct ebt_cl_stack
{
	struct ebt_chainstack cs;
	int from;
	unsigned int hookmask;
};

/*
 * we need these positions to check that the jumps to a different part of the
 * entries is a jump to the beginning of a new chain.
 */
static inline int
ebt_get_udc_positions(struct ebt_entry *e, struct ebt_table_info *newinfo,
   struct ebt_entries **hook_entries, unsigned int *n, unsigned int valid_hooks,
   struct ebt_cl_stack *udc)
{
	int i;

	/* we're only interested in chain starts */
	if (e->bitmask & EBT_ENTRY_OR_ENTRIES)
		return 0;
	for (i = 0; i < NF_BR_NUMHOOKS; i++) {
		if ((valid_hooks & (1 << i)) == 0)
			continue;
		if (newinfo->hook_entry[i] == (struct ebt_entries *)e)
			break;
	}
	/* only care about udc */
	if (i != NF_BR_NUMHOOKS)
		return 0;

	udc[*n].cs.chaininfo = (struct ebt_entries *)e;
	/* these initialisations are depended on later in check_chainloops() */
	udc[*n].cs.n = 0;
	udc[*n].hookmask = 0;

	(*n)++;
	return 0;
}

static inline int
ebt_cleanup_match(struct ebt_entry_match *m, unsigned int *i)
{
	if (i && (*i)-- == 0)
		return 1;
	if (m->u.match->destroy)
		m->u.match->destroy(m->data, m->match_size);
	module_put(m->u.match->me);

	return 0;
}

static inline int
ebt_cleanup_watcher(struct ebt_entry_watcher *w, unsigned int *i)
{
	if (i && (*i)-- == 0)
		return 1;
	if (w->u.watcher->destroy)
		w->u.watcher->destroy(w->data, w->watcher_size);
	module_put(w->u.watcher->me);

	return 0;
}

static inline int
ebt_cleanup_entry(struct ebt_entry *e, unsigned int *cnt)
{
	struct ebt_entry_target *t;

	if ((e->bitmask & EBT_ENTRY_OR_ENTRIES) == 0)
		return 0;
	/* we're done */
	if (cnt && (*cnt)-- == 0)
		return 1;
	EBT_WATCHER_ITERATE(e, ebt_cleanup_watcher, NULL);
	EBT_MATCH_ITERATE(e, ebt_cleanup_match, NULL);
	t = (struct ebt_entry_target *)(((char *)e) + e->target_offset);
	if (t->u.target->destroy)
		t->u.target->destroy(t->data, t->target_size);
	module_put(t->u.target->me);

	return 0;
}

static inline int
ebt_check_entry(struct ebt_entry *e, struct ebt_table_info *newinfo,
   const char *name, unsigned int *cnt, unsigned int valid_hooks,
   struct ebt_cl_stack *cl_s, unsigned int udc_cnt)
{
	struct ebt_entry_target *t;
	struct ebt_target *target;
	unsigned int i, j, hook = 0, hookmask = 0;
	int ret;

	/* don't mess with the struct ebt_entries */
	if ((e->bitmask & EBT_ENTRY_OR_ENTRIES) == 0)
		return 0;

	if (e->bitmask & ~EBT_F_MASK) {
		BUGPRINT("Unknown flag for bitmask\n");
		return -EINVAL;
	}
	if (e->invflags & ~EBT_INV_MASK) {
		BUGPRINT("Unknown flag for inv bitmask\n");
		return -EINVAL;
	}
	if ( (e->bitmask & EBT_NOPROTO) && (e->bitmask & EBT_802_3) ) {
		BUGPRINT("NOPROTO & 802_3 not allowed\n");
		return -EINVAL;
	}
	/* what hook do we belong to? */
	for (i = 0; i < NF_BR_NUMHOOKS; i++) {
		if ((valid_hooks & (1 << i)) == 0)
			continue;
		if ((char *)newinfo->hook_entry[i] < (char *)e)
			hook = i;
		else
			break;
	}
	/* (1 << NF_BR_NUMHOOKS) tells the check functions the rule is on
	   a base chain */
	if (i < NF_BR_NUMHOOKS)
		hookmask = (1 << hook) | (1 << NF_BR_NUMHOOKS);
	else {
		for (i = 0; i < udc_cnt; i++)
			if ((char *)(cl_s[i].cs.chaininfo) > (char *)e)
				break;
		if (i == 0)
			hookmask = (1 << hook) | (1 << NF_BR_NUMHOOKS);
		else
			hookmask = cl_s[i - 1].hookmask;
	}
	i = 0;
	ret = EBT_MATCH_ITERATE(e, ebt_check_match, e, name, hookmask, &i);
	if (ret != 0)
		goto cleanup_matches;
	j = 0;
	ret = EBT_WATCHER_ITERATE(e, ebt_check_watcher, e, name, hookmask, &j);
	if (ret != 0)
		goto cleanup_watchers;
	t = (struct ebt_entry_target *)(((char *)e) + e->target_offset);
	target = find_target_lock(t->u.name, &ret, &ebt_mutex);
	if (!target)
		goto cleanup_watchers;
	if (!try_module_get(target->me)) {
		up(&ebt_mutex);
		ret = -ENOENT;
		goto cleanup_watchers;
	}
	up(&ebt_mutex);

	t->u.target = target;
	if (t->u.target == &ebt_standard_target) {
		if (e->target_offset + sizeof(struct ebt_standard_target) >
		   e->next_offset) {
			BUGPRINT("Standard target size too big\n");
			ret = -EFAULT;
			goto cleanup_watchers;
		}
		if (((struct ebt_standard_target *)t)->verdict <
		   -NUM_STANDARD_TARGETS) {
			BUGPRINT("Invalid standard target\n");
			ret = -EFAULT;
			goto cleanup_watchers;
		}
	} else if ((e->target_offset + t->target_size +
	   sizeof(struct ebt_entry_target) > e->next_offset) ||
	   (t->u.target->check &&
	   t->u.target->check(name, hookmask, e, t->data, t->target_size) != 0)){
		module_put(t->u.target->me);
		ret = -EFAULT;
		goto cleanup_watchers;
	}
	(*cnt)++;
	return 0;
cleanup_watchers:
	EBT_WATCHER_ITERATE(e, ebt_cleanup_watcher, &j);
cleanup_matches:
	EBT_MATCH_ITERATE(e, ebt_cleanup_match, &i);
	return ret;
}

/*
 * checks for loops and sets the hook mask for udc
 * the hook mask for udc tells us from which base chains the udc can be
 * accessed. This mask is a parameter to the check() functions of the extensions
 */
static int check_chainloops(struct ebt_entries *chain, struct ebt_cl_stack *cl_s,
   unsigned int udc_cnt, unsigned int hooknr, char *base)
{
	int i, chain_nr = -1, pos = 0, nentries = chain->nentries, verdict;
	struct ebt_entry *e = (struct ebt_entry *)chain->data;
	struct ebt_entry_target *t;

	while (pos < nentries || chain_nr != -1) {
		/* end of udc, go back one 'recursion' step */
		if (pos == nentries) {
			/* put back values of the time when this chain was called */
			e = cl_s[chain_nr].cs.e;
			if (cl_s[chain_nr].from != -1)
				nentries =
				cl_s[cl_s[chain_nr].from].cs.chaininfo->nentries;
			else
				nentries = chain->nentries;
			pos = cl_s[chain_nr].cs.n;
			/* make sure we won't see a loop that isn't one */
			cl_s[chain_nr].cs.n = 0;
			chain_nr = cl_s[chain_nr].from;
			if (pos == nentries)
				continue;
		}
		t = (struct ebt_entry_target *)
		   (((char *)e) + e->target_offset);
		if (strcmp(t->u.name, EBT_STANDARD_TARGET))
			goto letscontinue;
		if (e->target_offset + sizeof(struct ebt_standard_target) >
		   e->next_offset) {
			BUGPRINT("Standard target size too big\n");
			return -1;
		}
		verdict = ((struct ebt_standard_target *)t)->verdict;
		if (verdict >= 0) { /* jump to another chain */
			struct ebt_entries *hlp2 =
			   (struct ebt_entries *)(base + verdict);
			for (i = 0; i < udc_cnt; i++)
				if (hlp2 == cl_s[i].cs.chaininfo)
					break;
			/* bad destination or loop */
			if (i == udc_cnt) {
				BUGPRINT("bad destination\n");
				return -1;
			}
			if (cl_s[i].cs.n) {
				BUGPRINT("loop\n");
				return -1;
			}
			/* this can't be 0, so the above test is correct */
			cl_s[i].cs.n = pos + 1;
			pos = 0;
			cl_s[i].cs.e = ((void *)e + e->next_offset);
			e = (struct ebt_entry *)(hlp2->data);
			nentries = hlp2->nentries;
			cl_s[i].from = chain_nr;
			chain_nr = i;
			/* this udc is accessible from the base chain for hooknr */
			cl_s[i].hookmask |= (1 << hooknr);
			continue;
		}
letscontinue:
		e = (void *)e + e->next_offset;
		pos++;
	}
	return 0;
}

/* do the parsing of the table/chains/entries/matches/watchers/targets, heh */
static int translate_table(struct ebt_replace *repl,
   struct ebt_table_info *newinfo)
{
	unsigned int i, j, k, udc_cnt;
	int ret;
	struct ebt_cl_stack *cl_s = NULL; /* used in the checking for chain loops */

	i = 0;
	while (i < NF_BR_NUMHOOKS && !(repl->valid_hooks & (1 << i)))
		i++;
	if (i == NF_BR_NUMHOOKS) {
		BUGPRINT("No valid hooks specified\n");
		return -EINVAL;
	}
	if (repl->hook_entry[i] != (struct ebt_entries *)repl->entries) {
		BUGPRINT("Chains don't start at beginning\n");
		return -EINVAL;
	}
	/* make sure chains are ordered after each other in same order
	   as their corresponding hooks */
	for (j = i + 1; j < NF_BR_NUMHOOKS; j++) {
		if (!(repl->valid_hooks & (1 << j)))
			continue;
		if ( repl->hook_entry[j] <= repl->hook_entry[i] ) {
			BUGPRINT("Hook order must be followed\n");
			return -EINVAL;
		}
		i = j;
	}

	for (i = 0; i < NF_BR_NUMHOOKS; i++)
		newinfo->hook_entry[i] = NULL;

	newinfo->entries_size = repl->entries_size;
	newinfo->nentries = repl->nentries;

	/* do some early checkings and initialize some things */
	i = 0; /* holds the expected nr. of entries for the chain */
	j = 0; /* holds the up to now counted entries for the chain */
	k = 0; /* holds the total nr. of entries, should equal
	          newinfo->nentries afterwards */
	udc_cnt = 0; /* will hold the nr. of user defined chains (udc) */
	ret = EBT_ENTRY_ITERATE(newinfo->entries, newinfo->entries_size,
	   ebt_check_entry_size_and_hooks, newinfo, repl->entries,
	   repl->entries + repl->entries_size, repl->hook_entry, &i, &j, &k,
	   &udc_cnt, repl->valid_hooks);

	if (ret != 0)
		return ret;

	if (i != j) {
		BUGPRINT("nentries does not equal the nr of entries in the "
		         "(last) chain\n");
		return -EINVAL;
	}
	if (k != newinfo->nentries) {
		BUGPRINT("Total nentries is wrong\n");
		return -EINVAL;
	}

	/* check if all valid hooks have a chain */
	for (i = 0; i < NF_BR_NUMHOOKS; i++) {
		if (newinfo->hook_entry[i] == NULL &&
		   (repl->valid_hooks & (1 << i))) {
			BUGPRINT("Valid hook without chain\n");
			return -EINVAL;
		}
	}

	/* get the location of the udc, put them in an array
	   while we're at it, allocate the chainstack */
	if (udc_cnt) {
		/* this will get free'd in do_replace()/ebt_register_table()
		   if an error occurs */
		newinfo->chainstack = (struct ebt_chainstack **)
		   vmalloc(NR_CPUS * sizeof(struct ebt_chainstack));
		if (!newinfo->chainstack)
			return -ENOMEM;
		for (i = 0; i < NR_CPUS; i++) {
			newinfo->chainstack[i] =
			   vmalloc(udc_cnt * sizeof(struct ebt_chainstack));
			if (!newinfo->chainstack[i]) {
				while (i)
					vfree(newinfo->chainstack[--i]);
				vfree(newinfo->chainstack);
				newinfo->chainstack = NULL;
				return -ENOMEM;
			}
		}

		cl_s = (struct ebt_cl_stack *)
		   vmalloc(udc_cnt * sizeof(struct ebt_cl_stack));
		if (!cl_s)
			return -ENOMEM;
		i = 0; /* the i'th udc */
		EBT_ENTRY_ITERATE(newinfo->entries, newinfo->entries_size,
		   ebt_get_udc_positions, newinfo, repl->hook_entry, &i,
		   repl->valid_hooks, cl_s);
		/* sanity check */
		if (i != udc_cnt) {
			BUGPRINT("i != udc_cnt\n");
			vfree(cl_s);
			return -EFAULT;
		}
	}

	/* Check for loops */
	for (i = 0; i < NF_BR_NUMHOOKS; i++)
		if (repl->valid_hooks & (1 << i))
			if (check_chainloops(newinfo->hook_entry[i],
			   cl_s, udc_cnt, i, newinfo->entries)) {
				if (cl_s)
					vfree(cl_s);
				return -EINVAL;
			}

	/* we now know the following (along with E=mc�):
	   - the nr of entries in each chain is right
	   - the size of the allocated space is right
	   - all valid hooks have a corresponding chain
	   - there are no loops
	   - wrong data can still be on the level of a single entry
	   - could be there are jumps to places that are not the
	     beginning of a chain. This can only occur in chains that
	     are not accessible from any base chains, so we don't care. */

	/* used to know what we need to clean up if something goes wrong */
	i = 0;
	ret = EBT_ENTRY_ITERATE(newinfo->entries, newinfo->entries_size,
	   ebt_check_entry, newinfo, repl->name, &i, repl->valid_hooks,
	   cl_s, udc_cnt);
	if (ret != 0) {
		EBT_ENTRY_ITERATE(newinfo->entries, newinfo->entries_size,
		   ebt_cleanup_entry, &i);
	}
	if (cl_s)
		vfree(cl_s);
	return ret;
}

/* called under write_lock */
static void get_counters(struct ebt_counter *oldcounters,
   struct ebt_counter *counters, unsigned int nentries)
{
	int i, cpu;
	struct ebt_counter *counter_base;

	/* counters of cpu 0 */
	memcpy(counters, oldcounters,
	   sizeof(struct ebt_counter) * nentries);
	/* add other counters to those of cpu 0 */
	for (cpu = 1; cpu < NR_CPUS; cpu++) {
		counter_base = COUNTER_BASE(oldcounters, nentries, cpu);
		for (i = 0; i < nentries; i++) {
			counters[i].pcnt += counter_base[i].pcnt;
			counters[i].bcnt += counter_base[i].bcnt;
		}
	}
}

/* replace the table */
static int do_replace(void __user *user, unsigned int len)
{
	int ret, i, countersize;
	struct ebt_table_info *newinfo;
	struct ebt_replace tmp;
	struct ebt_table *t;
	struct ebt_counter *counterstmp = NULL;
	/* used to be able to unlock earlier */
	struct ebt_table_info *table;

	if (copy_from_user(&tmp, user, sizeof(tmp)) != 0)
		return -EFAULT;

	if (len != sizeof(tmp) + tmp.entries_size) {
		BUGPRINT("Wrong len argument\n");
		return -EINVAL;
	}

	if (tmp.entries_size == 0) {
		BUGPRINT("Entries_size never zero\n");
		return -EINVAL;
	}
	countersize = COUNTER_OFFSET(tmp.nentries) * NR_CPUS;
	newinfo = (struct ebt_table_info *)
	   vmalloc(sizeof(struct ebt_table_info) + countersize);
	if (!newinfo)
		return -ENOMEM;

	if (countersize)
		memset(newinfo->counters, 0, countersize);

	newinfo->entries = (char *)vmalloc(tmp.entries_size);
	if (!newinfo->entries) {
		ret = -ENOMEM;
		goto free_newinfo;
	}
	if (copy_from_user(
	   newinfo->entries, tmp.entries, tmp.entries_size) != 0) {
		BUGPRINT("Couldn't copy entries from userspace\n");
		ret = -EFAULT;
		goto free_entries;
	}

	/* the user wants counters back
	   the check on the size is done later, when we have the lock */
	if (tmp.num_counters) {
		counterstmp = (struct ebt_counter *)
		   vmalloc(tmp.num_counters * sizeof(struct ebt_counter));
		if (!counterstmp) {
			ret = -ENOMEM;
			goto free_entries;
		}
	}
	else
		counterstmp = NULL;

	/* this can get initialized by translate_table() */
	newinfo->chainstack = NULL;
	ret = translate_table(&tmp, newinfo);

	if (ret != 0)
		goto free_counterstmp;

	t = find_table_lock(tmp.name, &ret, &ebt_mutex);
	if (!t) {
		ret = -ENOENT;
		goto free_iterate;
	}

	/* the table doesn't like it */
	if (t->check && (ret = t->check(newinfo, tmp.valid_hooks)))
		goto free_unlock;

	if (tmp.num_counters && tmp.num_counters != t->private->nentries) {
		BUGPRINT("Wrong nr. of counters requested\n");
		ret = -EINVAL;
		goto free_unlock;
	}

	/* we have the mutex lock, so no danger in reading this pointer */
	table = t->private;
	/* make sure the table can only be rmmod'ed if it contains no rules */
	if (!table->nentries && newinfo->nentries && !try_module_get(t->me)) {
		ret = -ENOENT;
		goto free_unlock;
	} else if (table->nentries && !newinfo->nentries)
		module_put(t->me);
	/* we need an atomic snapshot of the counters */
	write_lock_bh(&t->lock);
	if (tmp.num_counters)
		get_counters(t->private->counters, counterstmp,
		   t->private->nentries);

	t->private = newinfo;
	write_unlock_bh(&t->lock);
	up(&ebt_mutex);
	/* so, a user can change the chains while having messed up her counter
	   allocation. Only reason why this is done is because this way the lock
	   is held only once, while this doesn't bring the kernel into a
	   dangerous state. */
	if (tmp.num_counters &&
	   copy_to_user(tmp.counters, counterstmp,
	   tmp.num_counters * sizeof(struct ebt_counter))) {
		BUGPRINT("Couldn't copy counters to userspace\n");
		ret = -EFAULT;
	}
	else
		ret = 0;

	/* decrease module count and free resources */
	EBT_ENTRY_ITERATE(table->entries, table->entries_size,
	   ebt_cleanup_entry, NULL);

	vfree(table->entries);
	if (table->chainstack) {
		for (i = 0; i < NR_CPUS; i++)
			vfree(table->chainstack[i]);
		vfree(table->chainstack);
	}
	vfree(table);

	if (counterstmp)
		vfree(counterstmp);
	return ret;

free_unlock:
	up(&ebt_mutex);
free_iterate:
	EBT_ENTRY_ITERATE(newinfo->entries, newinfo->entries_size,
	   ebt_cleanup_entry, NULL);
free_counterstmp:
	if (counterstmp)
		vfree(counterstmp);
	/* can be initialized in translate_table() */
	if (newinfo->chainstack) {
		for (i = 0; i < NR_CPUS; i++)
			vfree(newinfo->chainstack[i]);
		vfree(newinfo->chainstack);
	}
free_entries:
	if (newinfo->entries)
		vfree(newinfo->entries);
free_newinfo:
	if (newinfo)
		vfree(newinfo);
	return ret;
}

int ebt_register_target(struct ebt_target *target)
{
	int ret;

	ret = down_interruptible(&ebt_mutex);
	if (ret != 0)
		return ret;
	if (!list_named_insert(&ebt_targets, target)) {
		up(&ebt_mutex);
		return -EEXIST;
	}
	up(&ebt_mutex);

	return 0;
}

void ebt_unregister_target(struct ebt_target *target)
{
	down(&ebt_mutex);
	LIST_DELETE(&ebt_targets, target);
	up(&ebt_mutex);
}

int ebt_register_match(struct ebt_match *match)
{
	int ret;

	ret = down_interruptible(&ebt_mutex);
	if (ret != 0)
		return ret;
	if (!list_named_insert(&ebt_matches, match)) {
		up(&ebt_mutex);
		return -EEXIST;
	}
	up(&ebt_mutex);

	return 0;
}

void ebt_unregister_match(struct ebt_match *match)
{
	down(&ebt_mutex);
	LIST_DELETE(&ebt_matches, match);
	up(&ebt_mutex);
}

int ebt_register_watcher(struct ebt_watcher *watcher)
{
	int ret;

	ret = down_interruptible(&ebt_mutex);
	if (ret != 0)
		return ret;
	if (!list_named_insert(&ebt_watchers, watcher)) {
		up(&ebt_mutex);
		return -EEXIST;
	}
	up(&ebt_mutex);

	return 0;
}

void ebt_unregister_watcher(struct ebt_watcher *watcher)
{
	down(&ebt_mutex);
	LIST_DELETE(&ebt_watchers, watcher);
	up(&ebt_mutex);
}

int ebt_register_table(struct ebt_table *table)
{
	struct ebt_table_info *newinfo;
	int ret, i, countersize;

	if (!table || !table->table ||!table->table->entries ||
	    table->table->entries_size == 0 ||
	    table->table->counters || table->private) {
		BUGPRINT("Bad table data for ebt_register_table!!!\n");
		return -EINVAL;
	}

	countersize = COUNTER_OFFSET(table->table->nentries) * NR_CPUS;
	newinfo = (struct ebt_table_info *)
	   vmalloc(sizeof(struct ebt_table_info) + countersize);
	ret = -ENOMEM;
	if (!newinfo)
		return -ENOMEM;

	newinfo->entries = (char *)vmalloc(table->table->entries_size);
	if (!(newinfo->entries))
		goto free_newinfo;

	memcpy(newinfo->entries, table->table->entries,
	   table->table->entries_size);

	if (countersize)
		memset(newinfo->counters, 0, countersize);

	/* fill in newinfo and parse the entries */
	newinfo->chainstack = NULL;
	ret = translate_table(table->table, newinfo);
	if (ret != 0) {
		BUGPRINT("Translate_table failed\n");
		goto free_chainstack;
	}

	if (table->check && table->check(newinfo, table->valid_hooks)) {
		BUGPRINT("The table doesn't like its own initial data, lol\n");
		return -EINVAL;
	}

	table->private = newinfo;
	table->lock = RW_LOCK_UNLOCKED;
	ret = down_interruptible(&ebt_mutex);
	if (ret != 0)
		goto free_chainstack;

	if (list_named_find(&ebt_tables, table->name)) {
		ret = -EEXIST;
		BUGPRINT("Table name already exists\n");
		goto free_unlock;
	}

	/* Hold a reference count if the chains aren't empty */
	if (newinfo->nentries && !try_module_get(table->me)) {
		ret = -ENOENT;
		goto free_unlock;
	}
	list_prepend(&ebt_tables, table);
	up(&ebt_mutex);
	return 0;
free_unlock:
	up(&ebt_mutex);
free_chainstack:
	if (newinfo->chainstack) {
		for (i = 0; i < NR_CPUS; i++)
			vfree(newinfo->chainstack[i]);
		vfree(newinfo->chainstack);
	}
	vfree(newinfo->entries);
free_newinfo:
	vfree(newinfo);
	return ret;
}

void ebt_unregister_table(struct ebt_table *table)
{
	int i;

	if (!table) {
		BUGPRINT("Request to unregister NULL table!!!\n");
		return;
	}
	down(&ebt_mutex);
	LIST_DELETE(&ebt_tables, table);
	up(&ebt_mutex);
	if (table->private->entries)
		vfree(table->private->entries);
	if (table->private->chainstack) {
		for (i = 0; i < NR_CPUS; i++)
			vfree(table->private->chainstack[i]);
		vfree(table->private->chainstack);
	}
	vfree(table->private);
}

/* userspace just supplied us with counters */
static int update_counters(void __user *user, unsigned int len)
{
	int i, ret;
	struct ebt_counter *tmp;
	struct ebt_replace hlp;
	struct ebt_table *t;

	if (copy_from_user(&hlp, user, sizeof(hlp)))
		return -EFAULT;

	if (len != sizeof(hlp) + hlp.num_counters * sizeof(struct ebt_counter))
		return -EINVAL;
	if (hlp.num_counters == 0)
		return -EINVAL;

	if ( !(tmp = (struct ebt_counter *)
	   vmalloc(hlp.num_counters * sizeof(struct ebt_counter))) ){
		MEMPRINT("Update_counters && nomemory\n");
		return -ENOMEM;
	}

	t = find_table_lock(hlp.name, &ret, &ebt_mutex);
	if (!t)
		goto free_tmp;

	if (hlp.num_counters != t->private->nentries) {
		BUGPRINT("Wrong nr of counters\n");
		ret = -EINVAL;
		goto unlock_mutex;
	}

	if ( copy_from_user(tmp, hlp.counters,
	   hlp.num_counters * sizeof(struct ebt_counter)) ) {
		BUGPRINT("Updata_counters && !cfu\n");
		ret = -EFAULT;
		goto unlock_mutex;
	}

	/* we want an atomic add of the counters */
	write_lock_bh(&t->lock);

	/* we add to the counters of the first cpu */
	for (i = 0; i < hlp.num_counters; i++) {
		t->private->counters[i].pcnt += tmp[i].pcnt;
		t->private->counters[i].bcnt += tmp[i].bcnt;
	}

	write_unlock_bh(&t->lock);
	ret = 0;
unlock_mutex:
	up(&ebt_mutex);
free_tmp:
	vfree(tmp);
	return ret;
}

static inline int ebt_make_matchname(struct ebt_entry_match *m,
   char *base, char *ubase)
{
	char *hlp = ubase - base + (char *)m;
	if (copy_to_user(hlp, m->u.match->name, EBT_FUNCTION_MAXNAMELEN))
		return -EFAULT;
	return 0;
}

static inline int ebt_make_watchername(struct ebt_entry_watcher *w,
   char *base, char *ubase)
{
	char *hlp = ubase - base + (char *)w;
	if (copy_to_user(hlp , w->u.watcher->name, EBT_FUNCTION_MAXNAMELEN))
		return -EFAULT;
	return 0;
}

static inline int ebt_make_names(struct ebt_entry *e, char *base, char *ubase)
{
	int ret;
	char *hlp;
	struct ebt_entry_target *t;

	if ((e->bitmask & EBT_ENTRY_OR_ENTRIES) == 0)
		return 0;

	hlp = ubase - base + (char *)e + e->target_offset;
	t = (struct ebt_entry_target *)(((char *)e) + e->target_offset);
	
	ret = EBT_MATCH_ITERATE(e, ebt_make_matchname, base, ubase);
	if (ret != 0)
		return ret;
	ret = EBT_WATCHER_ITERATE(e, ebt_make_watchername, base, ubase);
	if (ret != 0)
		return ret;
	if (copy_to_user(hlp, t->u.target->name, EBT_FUNCTION_MAXNAMELEN))
		return -EFAULT;
	return 0;
}

/* called with ebt_mutex down */
static int copy_everything_to_user(struct ebt_table *t, void __user *user,
   int *len, int cmd)
{
	struct ebt_replace tmp;
	struct ebt_counter *counterstmp, *oldcounters;
	unsigned int entries_size, nentries;
	char *entries;

	if (cmd == EBT_SO_GET_ENTRIES) {
		entries_size = t->private->entries_size;
		nentries = t->private->nentries;
		entries = t->private->entries;
		oldcounters = t->private->counters;
	} else {
		entries_size = t->table->entries_size;
		nentries = t->table->nentries;
		entries = t->table->entries;
		oldcounters = t->table->counters;
	}

	if (copy_from_user(&tmp, user, sizeof(tmp))) {
		BUGPRINT("Cfu didn't work\n");
		return -EFAULT;
	}

	if (*len != sizeof(struct ebt_replace) + entries_size +
	   (tmp.num_counters? nentries * sizeof(struct ebt_counter): 0)) {
		BUGPRINT("Wrong size\n");
		return -EINVAL;
	}

	if (tmp.nentries != nentries) {
		BUGPRINT("Nentries wrong\n");
		return -EINVAL;
	}

	if (tmp.entries_size != entries_size) {
		BUGPRINT("Wrong size\n");
		return -EINVAL;
	}

	/* userspace might not need the counters */
	if (tmp.num_counters) {
		if (tmp.num_counters != nentries) {
			BUGPRINT("Num_counters wrong\n");
			return -EINVAL;
		}
		counterstmp = (struct ebt_counter *)
		   vmalloc(nentries * sizeof(struct ebt_counter));
		if (!counterstmp) {
			MEMPRINT("Couldn't copy counters, out of memory\n");
			return -ENOMEM;
		}
		write_lock_bh(&t->lock);
		get_counters(oldcounters, counterstmp, nentries);
		write_unlock_bh(&t->lock);

		if (copy_to_user(tmp.counters, counterstmp,
		   nentries * sizeof(struct ebt_counter))) {
			BUGPRINT("Couldn't copy counters to userspace\n");
			vfree(counterstmp);
			return -EFAULT;
		}
		vfree(counterstmp);
	}

	if (copy_to_user(tmp.entries, entries, entries_size)) {
		BUGPRINT("Couldn't copy entries to userspace\n");
		return -EFAULT;
	}
	/* set the match/watcher/target names right */
	return EBT_ENTRY_ITERATE(entries, entries_size,
	   ebt_make_names, entries, tmp.entries);
}

static int do_ebt_set_ctl(struct sock *sk,
	int cmd, void __user *user, unsigned int len)
{
	int ret;

	switch(cmd) {
	case EBT_SO_SET_ENTRIES:
		ret = do_replace(user, len);
		break;
	case EBT_SO_SET_COUNTERS:
		ret = update_counters(user, len);
		break;
	default:
		ret = -EINVAL;
  }
	return ret;
}

static int do_ebt_get_ctl(struct sock *sk, int cmd, void __user *user, int *len)
{
	int ret;
	struct ebt_replace tmp;
	struct ebt_table *t;

	if (copy_from_user(&tmp, user, sizeof(tmp)))
		return -EFAULT;

	t = find_table_lock(tmp.name, &ret, &ebt_mutex);
	if (!t)
		return ret;

	switch(cmd) {
	case EBT_SO_GET_INFO:
	case EBT_SO_GET_INIT_INFO:
		if (*len != sizeof(struct ebt_replace)){
			ret = -EINVAL;
			up(&ebt_mutex);
			break;
		}
		if (cmd == EBT_SO_GET_INFO) {
			tmp.nentries = t->private->nentries;
			tmp.entries_size = t->private->entries_size;
			tmp.valid_hooks = t->valid_hooks;
		} else {
			tmp.nentries = t->table->nentries;
			tmp.entries_size = t->table->entries_size;
			tmp.valid_hooks = t->table->valid_hooks;
		}
		up(&ebt_mutex);
		if (copy_to_user(user, &tmp, *len) != 0){
			BUGPRINT("c2u Didn't work\n");
			ret = -EFAULT;
			break;
		}
		ret = 0;
		break;

	case EBT_SO_GET_ENTRIES:
	case EBT_SO_GET_INIT_ENTRIES:
		ret = copy_everything_to_user(t, user, len, cmd);
		up(&ebt_mutex);
		break;

	default:
		up(&ebt_mutex);
		ret = -EINVAL;
	}

	return ret;
}

static struct nf_sockopt_ops ebt_sockopts =
{ { NULL, NULL }, PF_INET, EBT_BASE_CTL, EBT_SO_SET_MAX + 1, do_ebt_set_ctl,
    EBT_BASE_CTL, EBT_SO_GET_MAX + 1, do_ebt_get_ctl, 0, NULL
};

static int __init init(void)
{
	int ret;

	down(&ebt_mutex);
	list_named_insert(&ebt_targets, &ebt_standard_target);
	up(&ebt_mutex);
	if ((ret = nf_register_sockopt(&ebt_sockopts)) < 0)
		return ret;

	printk(KERN_NOTICE "Ebtables v2.0 registered\n");
	return 0;
}

static void __exit fini(void)
{
	nf_unregister_sockopt(&ebt_sockopts);
	printk(KERN_NOTICE "Ebtables v2.0 unregistered\n");
}

EXPORT_SYMBOL(ebt_register_table);
EXPORT_SYMBOL(ebt_unregister_table);
EXPORT_SYMBOL(ebt_register_match);
EXPORT_SYMBOL(ebt_unregister_match);
EXPORT_SYMBOL(ebt_register_watcher);
EXPORT_SYMBOL(ebt_unregister_watcher);
EXPORT_SYMBOL(ebt_register_target);
EXPORT_SYMBOL(ebt_unregister_target);
EXPORT_SYMBOL(ebt_do_table);
module_init(init);
module_exit(fini);
MODULE_LICENSE("GPL");
