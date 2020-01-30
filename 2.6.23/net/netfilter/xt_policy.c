/* IP tables module for matching IPsec policy
 *
 * Copyright (c) 2004,2005 Patrick McHardy, <kaber@trash.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/init.h>
#include <net/xfrm.h>

#include <linux/netfilter/xt_policy.h>
#include <linux/netfilter/x_tables.h>

MODULE_AUTHOR("Patrick McHardy <kaber@trash.net>");
MODULE_DESCRIPTION("Xtables IPsec policy matching module");
MODULE_LICENSE("GPL");

static inline bool
xt_addr_cmp(const union xt_policy_addr *a1, const union xt_policy_addr *m,
	    const union xt_policy_addr *a2, unsigned short family)
{
	switch (family) {
	case AF_INET:
		return !((a1->a4.s_addr ^ a2->a4.s_addr) & m->a4.s_addr);
	case AF_INET6:
		return !ipv6_masked_addr_cmp(&a1->a6, &m->a6, &a2->a6);
	}
	return false;
}

static inline bool
match_xfrm_state(const struct xfrm_state *x, const struct xt_policy_elem *e,
		 unsigned short family)
{
#define MATCH_ADDR(x,y,z)	(!e->match.x ||			       \
				 (xt_addr_cmp(&e->x, &e->y, z, family) \
				  ^ e->invert.x))
#define MATCH(x,y)		(!e->match.x || ((e->x == (y)) ^ e->invert.x))

	return MATCH_ADDR(saddr, smask, (union xt_policy_addr *)&x->props.saddr) &&
	       MATCH_ADDR(daddr, dmask, (union xt_policy_addr *)&x->id.daddr) &&
	       MATCH(proto, x->id.proto) &&
	       MATCH(mode, x->props.mode) &&
	       MATCH(spi, x->id.spi) &&
	       MATCH(reqid, x->props.reqid);
}

static int
match_policy_in(const struct sk_buff *skb, const struct xt_policy_info *info,
		unsigned short family)
{
	const struct xt_policy_elem *e;
	const struct sec_path *sp = skb->sp;
	int strict = info->flags & XT_POLICY_MATCH_STRICT;
	int i, pos;

	if (sp == NULL)
		return -1;
	if (strict && info->len != sp->len)
		return 0;

	for (i = sp->len - 1; i >= 0; i--) {
		pos = strict ? i - sp->len + 1 : 0;
		if (pos >= info->len)
			return 0;
		e = &info->pol[pos];

		if (match_xfrm_state(sp->xvec[i], e, family)) {
			if (!strict)
				return 1;
		} else if (strict)
			return 0;
	}

	return strict ? 1 : 0;
}

static int
match_policy_out(const struct sk_buff *skb, const struct xt_policy_info *info,
		 unsigned short family)
{
	const struct xt_policy_elem *e;
	const struct dst_entry *dst = skb->dst;
	int strict = info->flags & XT_POLICY_MATCH_STRICT;
	int i, pos;

	if (dst->xfrm == NULL)
		return -1;

	for (i = 0; dst && dst->xfrm; dst = dst->child, i++) {
		pos = strict ? i : 0;
		if (pos >= info->len)
			return 0;
		e = &info->pol[pos];

		if (match_xfrm_state(dst->xfrm, e, family)) {
			if (!strict)
				return 1;
		} else if (strict)
			return 0;
	}

	return strict ? i == info->len : 0;
}

static bool match(const struct sk_buff *skb,
		  const struct net_device *in,
		  const struct net_device *out,
		  const struct xt_match *match,
		  const void *matchinfo,
		  int offset,
		  unsigned int protoff,
		  bool *hotdrop)
{
	const struct xt_policy_info *info = matchinfo;
	int ret;

	if (info->flags & XT_POLICY_MATCH_IN)
		ret = match_policy_in(skb, info, match->family);
	else
		ret = match_policy_out(skb, info, match->family);

	if (ret < 0)
		ret = info->flags & XT_POLICY_MATCH_NONE ? true : false;
	else if (info->flags & XT_POLICY_MATCH_NONE)
		ret = false;

	return ret;
}

static bool checkentry(const char *tablename, const void *ip_void,
		       const struct xt_match *match,
		       void *matchinfo, unsigned int hook_mask)
{
	struct xt_policy_info *info = matchinfo;

	if (!(info->flags & (XT_POLICY_MATCH_IN|XT_POLICY_MATCH_OUT))) {
		printk(KERN_ERR "xt_policy: neither incoming nor "
				"outgoing policy selected\n");
		return false;
	}
	/* hook values are equal for IPv4 and IPv6 */
	if (hook_mask & (1 << NF_IP_PRE_ROUTING | 1 << NF_IP_LOCAL_IN)
	    && info->flags & XT_POLICY_MATCH_OUT) {
		printk(KERN_ERR "xt_policy: output policy not valid in "
				"PRE_ROUTING and INPUT\n");
		return false;
	}
	if (hook_mask & (1 << NF_IP_POST_ROUTING | 1 << NF_IP_LOCAL_OUT)
	    && info->flags & XT_POLICY_MATCH_IN) {
		printk(KERN_ERR "xt_policy: input policy not valid in "
				"POST_ROUTING and OUTPUT\n");
		return false;
	}
	if (info->len > XT_POLICY_MAX_ELEM) {
		printk(KERN_ERR "xt_policy: too many policy elements\n");
		return false;
	}
	return true;
}

static struct xt_match xt_policy_match[] __read_mostly = {
	{
		.name		= "policy",
		.family		= AF_INET,
		.checkentry 	= checkentry,
		.match		= match,
		.matchsize	= sizeof(struct xt_policy_info),
		.me		= THIS_MODULE,
	},
	{
		.name		= "policy",
		.family		= AF_INET6,
		.checkentry	= checkentry,
		.match		= match,
		.matchsize	= sizeof(struct xt_policy_info),
		.me		= THIS_MODULE,
	},
};

static int __init init(void)
{
	return xt_register_matches(xt_policy_match,
				   ARRAY_SIZE(xt_policy_match));
}

static void __exit fini(void)
{
	xt_unregister_matches(xt_policy_match, ARRAY_SIZE(xt_policy_match));
}

module_init(init);
module_exit(fini);
MODULE_ALIAS("ipt_policy");
MODULE_ALIAS("ip6t_policy");
