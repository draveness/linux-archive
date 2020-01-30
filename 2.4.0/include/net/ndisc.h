#ifndef _NDISC_H
#define _NDISC_H

/*
 *	ICMP codes for neighbour discovery messages
 */

#define NDISC_ROUTER_SOLICITATION	133
#define NDISC_ROUTER_ADVERTISEMENT	134
#define NDISC_NEIGHBOUR_SOLICITATION	135
#define NDISC_NEIGHBOUR_ADVERTISEMENT	136
#define NDISC_REDIRECT			137

/*
 *	ndisc options
 */

#define ND_OPT_SOURCE_LL_ADDR		1
#define ND_OPT_TARGET_LL_ADDR		2
#define ND_OPT_PREFIX_INFO		3
#define ND_OPT_REDIRECT_HDR		4
#define ND_OPT_MTU			5

#define MAX_RTR_SOLICITATION_DELAY	HZ

#define ND_REACHABLE_TIME		(30*HZ)
#define ND_RETRANS_TIMER		HZ

#define ND_MIN_RANDOM_FACTOR		(1/2)
#define ND_MAX_RANDOM_FACTOR		(3/2)

#ifdef __KERNEL__

#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/icmpv6.h>
#include <net/neighbour.h>
#include <asm/atomic.h>

extern struct neigh_table nd_tbl;

struct nd_msg {
        struct icmp6hdr	icmph;
        struct in6_addr	target;
        struct {
                __u8	opt_type;
                __u8	opt_len;
                __u8	link_addr[MAX_ADDR_LEN];
        } opt;
};

struct ra_msg {
        struct icmp6hdr		icmph;
	__u32			reachable_time;
	__u32			retrans_timer;
};


extern int			ndisc_init(struct net_proto_family *ops);

extern void			ndisc_cleanup(void);

extern int			ndisc_rcv(struct sk_buff *skb, unsigned long len);

extern void			ndisc_send_ns(struct net_device *dev,
					      struct neighbour *neigh,
					      struct in6_addr *solicit,
					      struct in6_addr *daddr,
					      struct in6_addr *saddr);

extern void			ndisc_send_rs(struct net_device *dev,
					      struct in6_addr *saddr,
					      struct in6_addr *daddr);

extern void			ndisc_forwarding_on(void);
extern void			ndisc_forwarding_off(void);

extern void			ndisc_send_redirect(struct sk_buff *skb,
						    struct neighbour *neigh,
						    struct in6_addr *target);

extern int			ndisc_mc_map(struct in6_addr *addr, char *buf, struct net_device *dev, int dir);


struct rt6_info *		dflt_rt_lookup(void);

/*
 *	IGMP
 */
extern int			igmp6_init(struct net_proto_family *ops);

extern void			igmp6_cleanup(void);

extern int			igmp6_event_query(struct sk_buff *skb,
						  struct icmp6hdr *hdr,
						  int len);

extern int			igmp6_event_report(struct sk_buff *skb,
						   struct icmp6hdr *hdr,
						   int len);

extern void			igmp6_cleanup(void);

static inline struct neighbour * ndisc_get_neigh(struct net_device *dev, struct in6_addr *addr)
{

	if (dev)
		return __neigh_lookup(&nd_tbl, addr, dev, 1);

	return NULL;
}


#endif /* __KERNEL__ */


#endif
