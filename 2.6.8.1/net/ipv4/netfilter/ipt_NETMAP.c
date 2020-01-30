/* NETMAP - static NAT mapping of IP network addresses (1:1).
 * The mapping can be applied to source (POSTROUTING),
 * destination (PREROUTING), or both (with separate rules).
 */

/* (C) 2000-2001 Svenning Soerensen <svenning@post5.tele.dk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/config.h>
#include <linux/ip.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv4/ip_nat_rule.h>

#define MODULENAME "NETMAP"
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Svenning Soerensen <svenning@post5.tele.dk>");
MODULE_DESCRIPTION("iptables 1:1 NAT mapping of IP networks target");

#if 0
#define DEBUGP printk
#else
#define DEBUGP(format, args...)
#endif

static int
check(const char *tablename,
      const struct ipt_entry *e,
      void *targinfo,
      unsigned int targinfosize,
      unsigned int hook_mask)
{
	const struct ip_nat_multi_range *mr = targinfo;

	if (strcmp(tablename, "nat") != 0) {
		DEBUGP(MODULENAME":check: bad table `%s'.\n", tablename);
		return 0;
	}
	if (targinfosize != IPT_ALIGN(sizeof(*mr))) {
		DEBUGP(MODULENAME":check: size %u.\n", targinfosize);
		return 0;
	}
	if (hook_mask & ~((1 << NF_IP_PRE_ROUTING) | (1 << NF_IP_POST_ROUTING))) {
		DEBUGP(MODULENAME":check: bad hooks %x.\n", hook_mask);
		return 0;
	}
	if (!(mr->range[0].flags & IP_NAT_RANGE_MAP_IPS)) {
		DEBUGP(MODULENAME":check: bad MAP_IPS.\n");
		return 0;
	}
	if (mr->rangesize != 1) {
		DEBUGP(MODULENAME":check: bad rangesize %u.\n", mr->rangesize);
		return 0;
	}
	return 1;
}

static unsigned int
target(struct sk_buff **pskb,
       const struct net_device *in,
       const struct net_device *out,
       unsigned int hooknum,
       const void *targinfo,
       void *userinfo)
{
	struct ip_conntrack *ct;
	enum ip_conntrack_info ctinfo;
	u_int32_t new_ip, netmask;
	const struct ip_nat_multi_range *mr = targinfo;
	struct ip_nat_multi_range newrange;

	IP_NF_ASSERT(hooknum == NF_IP_PRE_ROUTING
		     || hooknum == NF_IP_POST_ROUTING);
	ct = ip_conntrack_get(*pskb, &ctinfo);

	netmask = ~(mr->range[0].min_ip ^ mr->range[0].max_ip);

	if (hooknum == NF_IP_PRE_ROUTING)
		new_ip = (*pskb)->nh.iph->daddr & ~netmask;
	else
		new_ip = (*pskb)->nh.iph->saddr & ~netmask;
	new_ip |= mr->range[0].min_ip & netmask;

	newrange = ((struct ip_nat_multi_range)
	{ 1, { { mr->range[0].flags | IP_NAT_RANGE_MAP_IPS,
		 new_ip, new_ip,
		 mr->range[0].min, mr->range[0].max } } });

	/* Hand modified range to generic setup. */
	return ip_nat_setup_info(ct, &newrange, hooknum);
}

static struct ipt_target target_module = { 
	.name 		= MODULENAME,
	.target 	= target, 
	.checkentry 	= check,
    	.me 		= THIS_MODULE 
};

static int __init init(void)
{
	return ipt_register_target(&target_module);
}

static void __exit fini(void)
{
	ipt_unregister_target(&target_module);
}

module_init(init);
module_exit(fini);
