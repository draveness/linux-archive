/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET  is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the IP router.
 *
 * Version:	@(#)route.h	1.0.4	05/27/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 * Fixes:
 *		Alan Cox	:	Reformatted. Added ip_rt_local()
 *		Alan Cox	:	Support for TCP parameters.
 *		Alexey Kuznetsov:	Major changes for new routing code.
 *		Mike McLagan    :	Routing by source
 *		Robert Olsson   :	Added rt_cache statistics
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _ROUTE_H
#define _ROUTE_H

#include <linux/config.h>
#include <net/dst.h>
#include <net/inetpeer.h>
#include <net/flow.h>
#include <linux/in_route.h>
#include <linux/rtnetlink.h>
#include <linux/route.h>
#include <linux/ip.h>
#include <linux/cache.h>

#ifndef __KERNEL__
#warning This file is not supposed to be used outside of kernel.
#endif

#define RTO_ONLINK	0x01

#define RTO_CONN	0
/* RTO_CONN is not used (being alias for 0), but preserved not to break
 * some modules referring to it. */

#define RT_CONN_FLAGS(sk)   (RT_TOS(inet_sk(sk)->tos) | sk->sk_localroute)

struct inet_peer;
struct rtable
{
	union
	{
		struct dst_entry	dst;
		struct rtable		*rt_next;
	} u;

	struct in_device	*idev;
	
	unsigned		rt_flags;
	unsigned		rt_type;

	__u32			rt_dst;	/* Path destination	*/
	__u32			rt_src;	/* Path source		*/
	int			rt_iif;

	/* Info on neighbour */
	__u32			rt_gateway;

	/* Cache lookup keys */
	struct flowi		fl;

	/* Miscellaneous cached information */
	__u32			rt_spec_dst; /* RFC1122 specific destination */
	struct inet_peer	*peer; /* long-living peer info */

#ifdef CONFIG_IP_ROUTE_NAT
	__u32			rt_src_map;
	__u32			rt_dst_map;
#endif
};

struct ip_rt_acct
{
	__u32 	o_bytes;
	__u32 	o_packets;
	__u32 	i_bytes;
	__u32 	i_packets;
};

struct rt_cache_stat 
{
        unsigned int in_hit;
        unsigned int in_slow_tot;
        unsigned int in_slow_mc;
        unsigned int in_no_route;
        unsigned int in_brd;
        unsigned int in_martian_dst;
        unsigned int in_martian_src;
        unsigned int out_hit;
        unsigned int out_slow_tot;
        unsigned int out_slow_mc;
        unsigned int gc_total;
        unsigned int gc_ignored;
        unsigned int gc_goal_miss;
        unsigned int gc_dst_overflow;
        unsigned int in_hlist_search;
        unsigned int out_hlist_search;
};

extern struct rt_cache_stat *rt_cache_stat;
#define RT_CACHE_STAT_INC(field)					  \
		(per_cpu_ptr(rt_cache_stat, smp_processor_id())->field++)

extern struct ip_rt_acct *ip_rt_acct;

struct in_device;
extern int		ip_rt_init(void);
extern void		ip_rt_redirect(u32 old_gw, u32 dst, u32 new_gw,
				       u32 src, u8 tos, struct net_device *dev);
extern void		ip_rt_advice(struct rtable **rp, int advice);
extern void		rt_cache_flush(int how);
extern int		__ip_route_output_key(struct rtable **, const struct flowi *flp);
extern int		ip_route_output_key(struct rtable **, struct flowi *flp);
extern int		ip_route_output_flow(struct rtable **rp, struct flowi *flp, struct sock *sk, int flags);
extern int		ip_route_input(struct sk_buff*, u32 dst, u32 src, u8 tos, struct net_device *devin);
extern unsigned short	ip_rt_frag_needed(struct iphdr *iph, unsigned short new_mtu);
extern void		ip_rt_send_redirect(struct sk_buff *skb);

extern unsigned		inet_addr_type(u32 addr);
extern void		ip_rt_multicast_event(struct in_device *);
extern int		ip_rt_ioctl(unsigned int cmd, void __user *arg);
extern void		ip_rt_get_source(u8 *src, struct rtable *rt);
extern int		ip_rt_dump(struct sk_buff *skb,  struct netlink_callback *cb);

static inline void ip_rt_put(struct rtable * rt)
{
	if (rt)
		dst_release(&rt->u.dst);
}

#define IPTOS_RT_MASK	(IPTOS_TOS_MASK & ~3)

extern __u8 ip_tos2prio[16];

static inline char rt_tos2priority(u8 tos)
{
	return ip_tos2prio[IPTOS_TOS(tos)>>1];
}

static inline int ip_route_connect(struct rtable **rp, u32 dst,
				   u32 src, u32 tos, int oif, u8 protocol,
				   u16 sport, u16 dport, struct sock *sk)
{
	struct flowi fl = { .oif = oif,
			    .nl_u = { .ip4_u = { .daddr = dst,
						 .saddr = src,
						 .tos   = tos } },
			    .proto = protocol,
			    .uli_u = { .ports =
				       { .sport = sport,
					 .dport = dport } } };

	int err;
	if (!dst || !src) {
		err = __ip_route_output_key(rp, &fl);
		if (err)
			return err;
		fl.fl4_dst = (*rp)->rt_dst;
		fl.fl4_src = (*rp)->rt_src;
		ip_rt_put(*rp);
		*rp = NULL;
	}
	return ip_route_output_flow(rp, &fl, sk, 0);
}

static inline int ip_route_newports(struct rtable **rp, u16 sport, u16 dport,
				    struct sock *sk)
{
	if (sport != (*rp)->fl.fl_ip_sport ||
	    dport != (*rp)->fl.fl_ip_dport) {
		struct flowi fl;

		memcpy(&fl, &(*rp)->fl, sizeof(fl));
		fl.fl_ip_sport = sport;
		fl.fl_ip_dport = dport;
		ip_rt_put(*rp);
		*rp = NULL;
		return ip_route_output_flow(rp, &fl, sk, 0);
	}
	return 0;
}

extern void rt_bind_peer(struct rtable *rt, int create);

static inline struct inet_peer *rt_get_peer(struct rtable *rt)
{
	if (rt->peer)
		return rt->peer;

	rt_bind_peer(rt, 0);
	return rt->peer;
}

#endif	/* _ROUTE_H */
