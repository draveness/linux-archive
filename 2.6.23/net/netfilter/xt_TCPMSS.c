/*
 * This is a module which is used for setting the MSS option in TCP packets.
 *
 * Copyright (C) 2000 Marc Boucher <marc@mbsi.ca>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <net/ipv6.h>
#include <net/tcp.h>

#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/netfilter_ipv6/ip6_tables.h>
#include <linux/netfilter/x_tables.h>
#include <linux/netfilter/xt_tcpudp.h>
#include <linux/netfilter/xt_TCPMSS.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Marc Boucher <marc@mbsi.ca>");
MODULE_DESCRIPTION("x_tables TCP MSS modification module");
MODULE_ALIAS("ipt_TCPMSS");
MODULE_ALIAS("ip6t_TCPMSS");

static inline unsigned int
optlen(const u_int8_t *opt, unsigned int offset)
{
	/* Beware zero-length options: make finite progress */
	if (opt[offset] <= TCPOPT_NOP || opt[offset+1] == 0)
		return 1;
	else
		return opt[offset+1];
}

static int
tcpmss_mangle_packet(struct sk_buff **pskb,
		     const struct xt_tcpmss_info *info,
		     unsigned int tcphoff,
		     unsigned int minlen)
{
	struct tcphdr *tcph;
	unsigned int tcplen, i;
	__be16 oldval;
	u16 newmss;
	u8 *opt;

	if (!skb_make_writable(pskb, (*pskb)->len))
		return -1;

	tcplen = (*pskb)->len - tcphoff;
	tcph = (struct tcphdr *)(skb_network_header(*pskb) + tcphoff);

	/* Since it passed flags test in tcp match, we know it is is
	   not a fragment, and has data >= tcp header length.  SYN
	   packets should not contain data: if they did, then we risk
	   running over MTU, sending Frag Needed and breaking things
	   badly. --RR */
	if (tcplen != tcph->doff*4) {
		if (net_ratelimit())
			printk(KERN_ERR "xt_TCPMSS: bad length (%u bytes)\n",
			       (*pskb)->len);
		return -1;
	}

	if (info->mss == XT_TCPMSS_CLAMP_PMTU) {
		if (dst_mtu((*pskb)->dst) <= minlen) {
			if (net_ratelimit())
				printk(KERN_ERR "xt_TCPMSS: "
				       "unknown or invalid path-MTU (%u)\n",
				       dst_mtu((*pskb)->dst));
			return -1;
		}
		newmss = dst_mtu((*pskb)->dst) - minlen;
	} else
		newmss = info->mss;

	opt = (u_int8_t *)tcph;
	for (i = sizeof(struct tcphdr); i < tcph->doff*4; i += optlen(opt, i)) {
		if (opt[i] == TCPOPT_MSS && tcph->doff*4 - i >= TCPOLEN_MSS &&
		    opt[i+1] == TCPOLEN_MSS) {
			u_int16_t oldmss;

			oldmss = (opt[i+2] << 8) | opt[i+3];

			if (info->mss == XT_TCPMSS_CLAMP_PMTU &&
			    oldmss <= newmss)
				return 0;

			opt[i+2] = (newmss & 0xff00) >> 8;
			opt[i+3] = newmss & 0x00ff;

			nf_proto_csum_replace2(&tcph->check, *pskb,
					       htons(oldmss), htons(newmss), 0);
			return 0;
		}
	}

	/*
	 * MSS Option not found ?! add it..
	 */
	if (skb_tailroom((*pskb)) < TCPOLEN_MSS) {
		struct sk_buff *newskb;

		newskb = skb_copy_expand(*pskb, skb_headroom(*pskb),
					 TCPOLEN_MSS, GFP_ATOMIC);
		if (!newskb)
			return -1;
		kfree_skb(*pskb);
		*pskb = newskb;
		tcph = (struct tcphdr *)(skb_network_header(*pskb) + tcphoff);
	}

	skb_put((*pskb), TCPOLEN_MSS);

	opt = (u_int8_t *)tcph + sizeof(struct tcphdr);
	memmove(opt + TCPOLEN_MSS, opt, tcplen - sizeof(struct tcphdr));

	nf_proto_csum_replace2(&tcph->check, *pskb,
			       htons(tcplen), htons(tcplen + TCPOLEN_MSS), 1);
	opt[0] = TCPOPT_MSS;
	opt[1] = TCPOLEN_MSS;
	opt[2] = (newmss & 0xff00) >> 8;
	opt[3] = newmss & 0x00ff;

	nf_proto_csum_replace4(&tcph->check, *pskb, 0, *((__be32 *)opt), 0);

	oldval = ((__be16 *)tcph)[6];
	tcph->doff += TCPOLEN_MSS/4;
	nf_proto_csum_replace2(&tcph->check, *pskb,
				oldval, ((__be16 *)tcph)[6], 0);
	return TCPOLEN_MSS;
}

static unsigned int
xt_tcpmss_target4(struct sk_buff **pskb,
		  const struct net_device *in,
		  const struct net_device *out,
		  unsigned int hooknum,
		  const struct xt_target *target,
		  const void *targinfo)
{
	struct iphdr *iph = ip_hdr(*pskb);
	__be16 newlen;
	int ret;

	ret = tcpmss_mangle_packet(pskb, targinfo, iph->ihl * 4,
				   sizeof(*iph) + sizeof(struct tcphdr));
	if (ret < 0)
		return NF_DROP;
	if (ret > 0) {
		iph = ip_hdr(*pskb);
		newlen = htons(ntohs(iph->tot_len) + ret);
		nf_csum_replace2(&iph->check, iph->tot_len, newlen);
		iph->tot_len = newlen;
	}
	return XT_CONTINUE;
}

#if defined(CONFIG_IP6_NF_IPTABLES) || defined(CONFIG_IP6_NF_IPTABLES_MODULE)
static unsigned int
xt_tcpmss_target6(struct sk_buff **pskb,
		  const struct net_device *in,
		  const struct net_device *out,
		  unsigned int hooknum,
		  const struct xt_target *target,
		  const void *targinfo)
{
	struct ipv6hdr *ipv6h = ipv6_hdr(*pskb);
	u8 nexthdr;
	int tcphoff;
	int ret;

	nexthdr = ipv6h->nexthdr;
	tcphoff = ipv6_skip_exthdr(*pskb, sizeof(*ipv6h), &nexthdr);
	if (tcphoff < 0) {
		WARN_ON(1);
		return NF_DROP;
	}
	ret = tcpmss_mangle_packet(pskb, targinfo, tcphoff,
				   sizeof(*ipv6h) + sizeof(struct tcphdr));
	if (ret < 0)
		return NF_DROP;
	if (ret > 0) {
		ipv6h = ipv6_hdr(*pskb);
		ipv6h->payload_len = htons(ntohs(ipv6h->payload_len) + ret);
	}
	return XT_CONTINUE;
}
#endif

#define TH_SYN 0x02

/* Must specify -p tcp --syn */
static inline bool find_syn_match(const struct xt_entry_match *m)
{
	const struct xt_tcp *tcpinfo = (const struct xt_tcp *)m->data;

	if (strcmp(m->u.kernel.match->name, "tcp") == 0 &&
	    tcpinfo->flg_cmp & TH_SYN &&
	    !(tcpinfo->invflags & XT_TCP_INV_FLAGS))
		return true;

	return false;
}

static bool
xt_tcpmss_checkentry4(const char *tablename,
		      const void *entry,
		      const struct xt_target *target,
		      void *targinfo,
		      unsigned int hook_mask)
{
	const struct xt_tcpmss_info *info = targinfo;
	const struct ipt_entry *e = entry;

	if (info->mss == XT_TCPMSS_CLAMP_PMTU &&
	    (hook_mask & ~((1 << NF_IP_FORWARD) |
			   (1 << NF_IP_LOCAL_OUT) |
			   (1 << NF_IP_POST_ROUTING))) != 0) {
		printk("xt_TCPMSS: path-MTU clamping only supported in "
		       "FORWARD, OUTPUT and POSTROUTING hooks\n");
		return false;
	}
	if (IPT_MATCH_ITERATE(e, find_syn_match))
		return true;
	printk("xt_TCPMSS: Only works on TCP SYN packets\n");
	return false;
}

#if defined(CONFIG_IP6_NF_IPTABLES) || defined(CONFIG_IP6_NF_IPTABLES_MODULE)
static bool
xt_tcpmss_checkentry6(const char *tablename,
		      const void *entry,
		      const struct xt_target *target,
		      void *targinfo,
		      unsigned int hook_mask)
{
	const struct xt_tcpmss_info *info = targinfo;
	const struct ip6t_entry *e = entry;

	if (info->mss == XT_TCPMSS_CLAMP_PMTU &&
	    (hook_mask & ~((1 << NF_IP6_FORWARD) |
			   (1 << NF_IP6_LOCAL_OUT) |
			   (1 << NF_IP6_POST_ROUTING))) != 0) {
		printk("xt_TCPMSS: path-MTU clamping only supported in "
		       "FORWARD, OUTPUT and POSTROUTING hooks\n");
		return false;
	}
	if (IP6T_MATCH_ITERATE(e, find_syn_match))
		return true;
	printk("xt_TCPMSS: Only works on TCP SYN packets\n");
	return false;
}
#endif

static struct xt_target xt_tcpmss_reg[] __read_mostly = {
	{
		.family		= AF_INET,
		.name		= "TCPMSS",
		.checkentry	= xt_tcpmss_checkentry4,
		.target		= xt_tcpmss_target4,
		.targetsize	= sizeof(struct xt_tcpmss_info),
		.proto		= IPPROTO_TCP,
		.me		= THIS_MODULE,
	},
#if defined(CONFIG_IP6_NF_IPTABLES) || defined(CONFIG_IP6_NF_IPTABLES_MODULE)
	{
		.family		= AF_INET6,
		.name		= "TCPMSS",
		.checkentry	= xt_tcpmss_checkentry6,
		.target		= xt_tcpmss_target6,
		.targetsize	= sizeof(struct xt_tcpmss_info),
		.proto		= IPPROTO_TCP,
		.me		= THIS_MODULE,
	},
#endif
};

static int __init xt_tcpmss_init(void)
{
	return xt_register_targets(xt_tcpmss_reg, ARRAY_SIZE(xt_tcpmss_reg));
}

static void __exit xt_tcpmss_fini(void)
{
	xt_unregister_targets(xt_tcpmss_reg, ARRAY_SIZE(xt_tcpmss_reg));
}

module_init(xt_tcpmss_init);
module_exit(xt_tcpmss_fini);
