/* (C) 1999-2001 Paul `Rusty' Russell
 * (C) 2002-2004 Netfilter Core Team <coreteam@netfilter.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/types.h>
#include <linux/timer.h>
#include <linux/module.h>
#include <linux/udp.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <linux/ipv6.h>
#include <net/ip6_checksum.h>
#include <net/checksum.h>

#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter_ipv6.h>
#include <net/netfilter/nf_conntrack_l4proto.h>
#include <net/netfilter/nf_conntrack_ecache.h>

static unsigned int nf_ct_udp_timeout __read_mostly = 30*HZ;
static unsigned int nf_ct_udp_timeout_stream __read_mostly = 180*HZ;

static int udp_pkt_to_tuple(const struct sk_buff *skb,
			     unsigned int dataoff,
			     struct nf_conntrack_tuple *tuple)
{
	struct udphdr _hdr, *hp;

	/* Actually only need first 8 bytes. */
	hp = skb_header_pointer(skb, dataoff, sizeof(_hdr), &_hdr);
	if (hp == NULL)
		return 0;

	tuple->src.u.udp.port = hp->source;
	tuple->dst.u.udp.port = hp->dest;

	return 1;
}

static int udp_invert_tuple(struct nf_conntrack_tuple *tuple,
			    const struct nf_conntrack_tuple *orig)
{
	tuple->src.u.udp.port = orig->dst.u.udp.port;
	tuple->dst.u.udp.port = orig->src.u.udp.port;
	return 1;
}

/* Print out the per-protocol part of the tuple. */
static int udp_print_tuple(struct seq_file *s,
			   const struct nf_conntrack_tuple *tuple)
{
	return seq_printf(s, "sport=%hu dport=%hu ",
			  ntohs(tuple->src.u.udp.port),
			  ntohs(tuple->dst.u.udp.port));
}

/* Print out the private part of the conntrack. */
static int udp_print_conntrack(struct seq_file *s,
			       const struct nf_conn *conntrack)
{
	return 0;
}

/* Returns verdict for packet, and may modify conntracktype */
static int udp_packet(struct nf_conn *conntrack,
		      const struct sk_buff *skb,
		      unsigned int dataoff,
		      enum ip_conntrack_info ctinfo,
		      int pf,
		      unsigned int hooknum)
{
	/* If we've seen traffic both ways, this is some kind of UDP
	   stream.  Extend timeout. */
	if (test_bit(IPS_SEEN_REPLY_BIT, &conntrack->status)) {
		nf_ct_refresh_acct(conntrack, ctinfo, skb,
				   nf_ct_udp_timeout_stream);
		/* Also, more likely to be important, and not a probe */
		if (!test_and_set_bit(IPS_ASSURED_BIT, &conntrack->status))
			nf_conntrack_event_cache(IPCT_STATUS, skb);
	} else
		nf_ct_refresh_acct(conntrack, ctinfo, skb, nf_ct_udp_timeout);

	return NF_ACCEPT;
}

/* Called when a new connection for this protocol found. */
static int udp_new(struct nf_conn *conntrack, const struct sk_buff *skb,
		   unsigned int dataoff)
{
	return 1;
}

static int udp_error(struct sk_buff *skb, unsigned int dataoff,
		     enum ip_conntrack_info *ctinfo,
		     int pf,
		     unsigned int hooknum)
{
	unsigned int udplen = skb->len - dataoff;
	struct udphdr _hdr, *hdr;

	/* Header is too small? */
	hdr = skb_header_pointer(skb, dataoff, sizeof(_hdr), &_hdr);
	if (hdr == NULL) {
		if (LOG_INVALID(IPPROTO_UDP))
			nf_log_packet(pf, 0, skb, NULL, NULL, NULL,
				      "nf_ct_udp: short packet ");
		return -NF_ACCEPT;
	}

	/* Truncated/malformed packets */
	if (ntohs(hdr->len) > udplen || ntohs(hdr->len) < sizeof(*hdr)) {
		if (LOG_INVALID(IPPROTO_UDP))
			nf_log_packet(pf, 0, skb, NULL, NULL, NULL,
				"nf_ct_udp: truncated/malformed packet ");
		return -NF_ACCEPT;
	}

	/* Packet with no checksum */
	if (!hdr->check)
		return NF_ACCEPT;

	/* Checksum invalid? Ignore.
	 * We skip checking packets on the outgoing path
	 * because the checksum is assumed to be correct.
	 * FIXME: Source route IP option packets --RR */
	if (nf_conntrack_checksum &&
	    ((pf == PF_INET && hooknum == NF_IP_PRE_ROUTING) ||
	     (pf == PF_INET6 && hooknum == NF_IP6_PRE_ROUTING)) &&
	    nf_checksum(skb, hooknum, dataoff, IPPROTO_UDP, pf)) {
		if (LOG_INVALID(IPPROTO_UDP))
			nf_log_packet(pf, 0, skb, NULL, NULL, NULL,
				"nf_ct_udp: bad UDP checksum ");
		return -NF_ACCEPT;
	}

	return NF_ACCEPT;
}

#ifdef CONFIG_SYSCTL
static unsigned int udp_sysctl_table_users;
static struct ctl_table_header *udp_sysctl_header;
static struct ctl_table udp_sysctl_table[] = {
	{
		.ctl_name	= NET_NF_CONNTRACK_UDP_TIMEOUT,
		.procname	= "nf_conntrack_udp_timeout",
		.data		= &nf_ct_udp_timeout,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= &proc_dointvec_jiffies,
	},
	{
		.ctl_name	= NET_NF_CONNTRACK_UDP_TIMEOUT_STREAM,
		.procname	= "nf_conntrack_udp_timeout_stream",
		.data		= &nf_ct_udp_timeout_stream,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= &proc_dointvec_jiffies,
	},
	{
		.ctl_name	= 0
	}
};
#ifdef CONFIG_NF_CONNTRACK_PROC_COMPAT
static struct ctl_table udp_compat_sysctl_table[] = {
	{
		.ctl_name	= NET_IPV4_NF_CONNTRACK_UDP_TIMEOUT,
		.procname	= "ip_conntrack_udp_timeout",
		.data		= &nf_ct_udp_timeout,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= &proc_dointvec_jiffies,
	},
	{
		.ctl_name	= NET_IPV4_NF_CONNTRACK_UDP_TIMEOUT_STREAM,
		.procname	= "ip_conntrack_udp_timeout_stream",
		.data		= &nf_ct_udp_timeout_stream,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= &proc_dointvec_jiffies,
	},
	{
		.ctl_name	= 0
	}
};
#endif /* CONFIG_NF_CONNTRACK_PROC_COMPAT */
#endif /* CONFIG_SYSCTL */

struct nf_conntrack_l4proto nf_conntrack_l4proto_udp4 __read_mostly =
{
	.l3proto		= PF_INET,
	.l4proto		= IPPROTO_UDP,
	.name			= "udp",
	.pkt_to_tuple		= udp_pkt_to_tuple,
	.invert_tuple		= udp_invert_tuple,
	.print_tuple		= udp_print_tuple,
	.print_conntrack	= udp_print_conntrack,
	.packet			= udp_packet,
	.new			= udp_new,
	.error			= udp_error,
#if defined(CONFIG_NF_CT_NETLINK) || defined(CONFIG_NF_CT_NETLINK_MODULE)
	.tuple_to_nfattr	= nf_ct_port_tuple_to_nfattr,
	.nfattr_to_tuple	= nf_ct_port_nfattr_to_tuple,
#endif
#ifdef CONFIG_SYSCTL
	.ctl_table_users	= &udp_sysctl_table_users,
	.ctl_table_header	= &udp_sysctl_header,
	.ctl_table		= udp_sysctl_table,
#ifdef CONFIG_NF_CONNTRACK_PROC_COMPAT
	.ctl_compat_table	= udp_compat_sysctl_table,
#endif
#endif
};
EXPORT_SYMBOL_GPL(nf_conntrack_l4proto_udp4);

struct nf_conntrack_l4proto nf_conntrack_l4proto_udp6 __read_mostly =
{
	.l3proto		= PF_INET6,
	.l4proto		= IPPROTO_UDP,
	.name			= "udp",
	.pkt_to_tuple		= udp_pkt_to_tuple,
	.invert_tuple		= udp_invert_tuple,
	.print_tuple		= udp_print_tuple,
	.print_conntrack	= udp_print_conntrack,
	.packet			= udp_packet,
	.new			= udp_new,
	.error			= udp_error,
#if defined(CONFIG_NF_CT_NETLINK) || defined(CONFIG_NF_CT_NETLINK_MODULE)
	.tuple_to_nfattr	= nf_ct_port_tuple_to_nfattr,
	.nfattr_to_tuple	= nf_ct_port_nfattr_to_tuple,
#endif
#ifdef CONFIG_SYSCTL
	.ctl_table_users	= &udp_sysctl_table_users,
	.ctl_table_header	= &udp_sysctl_header,
	.ctl_table		= udp_sysctl_table,
#endif
};
EXPORT_SYMBOL_GPL(nf_conntrack_l4proto_udp6);
