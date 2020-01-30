/* Redirect.  Simple mapping which alters dst to a local IP address. */
/* (C) 1999-2001 Paul `Rusty' Russell
 * (C) 2002-2004 Netfilter Core Team <coreteam@netfilter.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/types.h>
#include <linux/ip.h>
#include <linux/timer.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <linux/netdevice.h>
#include <linux/if.h>
#include <linux/inetdevice.h>
#include <net/protocol.h>
#include <net/checksum.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv4/ip_nat_rule.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Netfilter Core Team <coreteam@netfilter.org>");
MODULE_DESCRIPTION("iptables REDIRECT target module");

#if 0
#define DEBUGP printk
#else
#define DEBUGP(format, args...)
#endif

/* FIXME: Take multiple ranges --RR */
static int
redirect_check(const char *tablename,
	       const struct ipt_entry *e,
	       void *targinfo,
	       unsigned int targinfosize,
	       unsigned int hook_mask)
{
	const struct ip_nat_multi_range *mr = targinfo;

	if (strcmp(tablename, "nat") != 0) {
		DEBUGP("redirect_check: bad table `%s'.\n", table);
		return 0;
	}
	if (targinfosize != IPT_ALIGN(sizeof(*mr))) {
		DEBUGP("redirect_check: size %u.\n", targinfosize);
		return 0;
	}
	if (hook_mask & ~((1 << NF_IP_PRE_ROUTING) | (1 << NF_IP_LOCAL_OUT))) {
		DEBUGP("redirect_check: bad hooks %x.\n", hook_mask);
		return 0;
	}
	if (mr->range[0].flags & IP_NAT_RANGE_MAP_IPS) {
		DEBUGP("redirect_check: bad MAP_IPS.\n");
		return 0;
	}
	if (mr->rangesize != 1) {
		DEBUGP("redirect_check: bad rangesize %u.\n", mr->rangesize);
		return 0;
	}
	return 1;
}

static unsigned int
redirect_target(struct sk_buff **pskb,
		const struct net_device *in,
		const struct net_device *out,
		unsigned int hooknum,
		const void *targinfo,
		void *userinfo)
{
	struct ip_conntrack *ct;
	enum ip_conntrack_info ctinfo;
	u_int32_t newdst;
	const struct ip_nat_multi_range *mr = targinfo;
	struct ip_nat_multi_range newrange;

	IP_NF_ASSERT(hooknum == NF_IP_PRE_ROUTING
		     || hooknum == NF_IP_LOCAL_OUT);

	ct = ip_conntrack_get(*pskb, &ctinfo);
	IP_NF_ASSERT(ct && (ctinfo == IP_CT_NEW || ctinfo == IP_CT_RELATED));

	/* Local packets: make them go to loopback */
	if (hooknum == NF_IP_LOCAL_OUT)
		newdst = htonl(0x7F000001);
	else {
		struct in_device *indev;

		/* Device might not have an associated in_device. */
		indev = (struct in_device *)(*pskb)->dev->ip_ptr;
		if (indev == NULL || indev->ifa_list == NULL)
			return NF_DROP;

		/* Grab first address on interface. */
		newdst = indev->ifa_list->ifa_local;
	}

	/* Transfer from original range. */
	newrange = ((struct ip_nat_multi_range)
		{ 1, { { mr->range[0].flags | IP_NAT_RANGE_MAP_IPS,
			 newdst, newdst,
			 mr->range[0].min, mr->range[0].max } } });

	/* Hand modified range to generic setup. */
	return ip_nat_setup_info(ct, &newrange, hooknum);
}

static struct ipt_target redirect_reg = {
	.name		= "REDIRECT",
	.target		= redirect_target,
	.checkentry	= redirect_check,
	.me		= THIS_MODULE,
};

static int __init init(void)
{
	return ipt_register_target(&redirect_reg);
}

static void __exit fini(void)
{
	ipt_unregister_target(&redirect_reg);
}

module_init(init);
module_exit(fini);
