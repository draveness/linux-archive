/*
 *	Linux INET6 implementation
 *
 *	Authors:
 *	Pedro Roque		<roque@di.fc.ul.pt>
 *
 *	$Id: ipv6.h,v 1.22 2000/09/18 05:54:13 davem Exp $
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#ifndef _NET_IPV6_H
#define _NET_IPV6_H

#include <linux/ipv6.h>
#include <asm/hardirq.h>
#include <net/ndisc.h>
#include <net/flow.h>

#define SIN6_LEN_RFC2133	24

/*
 *	NextHeader field of IPv6 header
 */

#define NEXTHDR_HOP		0	/* Hop-by-hop option header. */
#define NEXTHDR_TCP		6	/* TCP segment. */
#define NEXTHDR_UDP		17	/* UDP message. */
#define NEXTHDR_IPV6		41	/* IPv6 in IPv6 */
#define NEXTHDR_ROUTING		43	/* Routing header. */
#define NEXTHDR_FRAGMENT	44	/* Fragmentation/reassembly header. */
#define NEXTHDR_ESP		50	/* Encapsulating security payload. */
#define NEXTHDR_AUTH		51	/* Authentication header. */
#define NEXTHDR_ICMP		58	/* ICMP for IPv6. */
#define NEXTHDR_NONE		59	/* No next header */
#define NEXTHDR_DEST		60	/* Destination options header. */

#define NEXTHDR_MAX		255



#define IPV6_DEFAULT_HOPLIMIT   64
#define IPV6_DEFAULT_MCASTHOPS	1

/*
 *	Addr type
 *	
 *	type	-	unicast | multicast | anycast
 *	scope	-	local	| site	    | global
 *	v4	-	compat
 *	v4mapped
 *	any
 *	loopback
 */

#define IPV6_ADDR_ANY		0x0000U

#define IPV6_ADDR_UNICAST      	0x0001U	
#define IPV6_ADDR_MULTICAST    	0x0002U	
#define IPV6_ADDR_ANYCAST	0x0004U

#define IPV6_ADDR_LOOPBACK	0x0010U
#define IPV6_ADDR_LINKLOCAL	0x0020U
#define IPV6_ADDR_SITELOCAL	0x0040U

#define IPV6_ADDR_COMPATv4	0x0080U

#define IPV6_ADDR_SCOPE_MASK	0x00f0U

#define IPV6_ADDR_MAPPED	0x1000U
#define IPV6_ADDR_RESERVED	0x2000U	/* reserved address space */

/*
 *	fragmentation header
 */

struct frag_hdr {
	unsigned char	nexthdr;
	unsigned char	reserved;	
	unsigned short	frag_off;
	__u32		identification;
};

#ifdef __KERNEL__

#include <net/sock.h>

extern struct ipv6_mib		ipv6_statistics[NR_CPUS*2];
#define IP6_INC_STATS(field)		SNMP_INC_STATS(ipv6_statistics, field)
#define IP6_INC_STATS_BH(field)		SNMP_INC_STATS_BH(ipv6_statistics, field)
#define IP6_INC_STATS_USER(field) 	SNMP_INC_STATS_USER(ipv6_statistics, field)
extern struct icmpv6_mib	icmpv6_statistics[NR_CPUS*2];
#define ICMP6_INC_STATS(field)		SNMP_INC_STATS(icmpv6_statistics, field)
#define ICMP6_INC_STATS_BH(field)	SNMP_INC_STATS_BH(icmpv6_statistics, field)
#define ICMP6_INC_STATS_USER(field) 	SNMP_INC_STATS_USER(icmpv6_statistics, field)
extern struct udp_mib		udp_stats_in6[NR_CPUS*2];
#define UDP6_INC_STATS(field)		SNMP_INC_STATS(udp_stats_in6, field)
#define UDP6_INC_STATS_BH(field)	SNMP_INC_STATS_BH(udp_stats_in6, field)
#define UDP6_INC_STATS_USER(field) 	SNMP_INC_STATS_USER(udp_stats_in6, field)
extern atomic_t			inet6_sock_nr;

struct ip6_ra_chain
{
	struct ip6_ra_chain	*next;
	struct sock		*sk;
	int			sel;
	void			(*destructor)(struct sock *);
};

extern struct ip6_ra_chain	*ip6_ra_chain;
extern rwlock_t ip6_ra_lock;

/*
   This structure is prepared by protocol, when parsing
   ancillary data and passed to IPv6.
 */

struct ipv6_txoptions
{
	/* Length of this structure */
	int			tot_len;

	/* length of extension headers   */

	__u16			opt_flen;	/* after fragment hdr */
	__u16			opt_nflen;	/* before fragment hdr */

	struct ipv6_opt_hdr	*hopopt;
	struct ipv6_opt_hdr	*dst0opt;
	struct ipv6_rt_hdr	*srcrt;	/* Routing Header */
	struct ipv6_opt_hdr	*auth;
	struct ipv6_opt_hdr	*dst1opt;

	/* Option buffer, as read by IPV6_PKTOPTIONS, starts here. */
};

struct ip6_flowlabel
{
	struct ip6_flowlabel	*next;
	u32			label;
	struct in6_addr		dst;
	struct ipv6_txoptions	*opt;
	atomic_t		users;
	u32			linger;
	u8			share;
	u32			owner;
	unsigned long		lastuse;
	unsigned long		expires;
};

#define IPV6_FLOWINFO_MASK	__constant_htonl(0x0FFFFFFF)
#define IPV6_FLOWLABEL_MASK	__constant_htonl(0x000FFFFF)

struct ipv6_fl_socklist
{
	struct ipv6_fl_socklist	*next;
	struct ip6_flowlabel	*fl;
};

extern struct ip6_flowlabel	*fl6_sock_lookup(struct sock *sk, u32 label);
extern struct ipv6_txoptions	*fl6_merge_options(struct ipv6_txoptions * opt_space,
						   struct ip6_flowlabel * fl,
						   struct ipv6_txoptions * fopt);
extern void			fl6_free_socklist(struct sock *sk);
extern int			ipv6_flowlabel_opt(struct sock *sk, char *optval, int optlen);
extern void			ip6_flowlabel_init(void);
extern void			ip6_flowlabel_cleanup(void);

static inline void fl6_sock_release(struct ip6_flowlabel *fl)
{
	if (fl)
		atomic_dec(&fl->users);
}

extern int 			ip6_ra_control(struct sock *sk, int sel,
					       void (*destructor)(struct sock *));


extern int			ip6_call_ra_chain(struct sk_buff *skb, int sel);

extern u8 *			ipv6_reassembly(struct sk_buff **skb, u8 *nhptr);

extern u8 *			ipv6_parse_hopopts(struct sk_buff *skb, u8 *nhptr);

extern u8 *			ipv6_parse_exthdrs(struct sk_buff **skb, u8 *nhptr);

extern struct ipv6_txoptions *  ipv6_dup_options(struct sock *sk, struct ipv6_txoptions *opt);

extern int ip6_frag_nqueues;
extern atomic_t ip6_frag_mem;

#define IPV6_FRAG_TIMEOUT	(60*HZ)		/* 60 seconds */

/*
 *	Function prototype for build_xmit
 */

typedef int		(*inet_getfrag_t) (const void *data,
					   struct in6_addr *addr,
					   char *,
					   unsigned int, unsigned int);


extern int		ipv6_addr_type(struct in6_addr *addr);

static inline int ipv6_addr_scope(struct in6_addr *addr)
{
	return ipv6_addr_type(addr) & IPV6_ADDR_SCOPE_MASK;
}

static inline int ipv6_addr_cmp(struct in6_addr *a1, struct in6_addr *a2)
{
	return memcmp((void *) a1, (void *) a2, sizeof(struct in6_addr));
}

static inline void ipv6_addr_copy(struct in6_addr *a1, struct in6_addr *a2)
{
	memcpy((void *) a1, (void *) a2, sizeof(struct in6_addr));
}

#ifndef __HAVE_ARCH_ADDR_SET
static inline void ipv6_addr_set(struct in6_addr *addr, 
				     __u32 w1, __u32 w2,
				     __u32 w3, __u32 w4)
{
	addr->s6_addr32[0] = w1;
	addr->s6_addr32[1] = w2;
	addr->s6_addr32[2] = w3;
	addr->s6_addr32[3] = w4;
}
#endif

static inline int ipv6_addr_any(struct in6_addr *a)
{
	return ((a->s6_addr32[0] | a->s6_addr32[1] | 
		 a->s6_addr32[2] | a->s6_addr32[3] ) == 0); 
}

/*
 *	Prototypes exported by ipv6
 */

/*
 *	rcv function (called from netdevice level)
 */

extern int			ipv6_rcv(struct sk_buff *skb, 
					 struct net_device *dev, 
					 struct packet_type *pt);

/*
 *	upper-layer output functions
 */
extern int			ip6_xmit(struct sock *sk,
					 struct sk_buff *skb,
					 struct flowi *fl,
					 struct ipv6_txoptions *opt);

extern int			ip6_nd_hdr(struct sock *sk,
					   struct sk_buff *skb,
					   struct net_device *dev,
					   struct in6_addr *saddr,
					   struct in6_addr *daddr,
					   int proto, int len);

extern int			ip6_build_xmit(struct sock *sk,
					       inet_getfrag_t getfrag,
					       const void *data,
					       struct flowi *fl,
					       unsigned length,
					       struct ipv6_txoptions *opt,
					       int hlimit, int flags);

/*
 *	skb processing functions
 */

extern int			ip6_output(struct sk_buff *skb);
extern int			ip6_forward(struct sk_buff *skb);
extern int			ip6_input(struct sk_buff *skb);
extern int			ip6_mc_input(struct sk_buff *skb);

/*
 *	Extension header (options) processing
 */

extern u8 *			ipv6_build_nfrag_opts(struct sk_buff *skb,
						      u8 *prev_hdr,
						      struct ipv6_txoptions *opt,
						      struct in6_addr *daddr,
						      u32 jumbolen);
extern u8 *			ipv6_build_frag_opts(struct sk_buff *skb,
						     u8 *prev_hdr,
						     struct ipv6_txoptions *opt);
extern void 			ipv6_push_nfrag_opts(struct sk_buff *skb,
						     struct ipv6_txoptions *opt,
						     u8 *proto,
						     struct in6_addr **daddr_p);
extern void			ipv6_push_frag_opts(struct sk_buff *skb,
						    struct ipv6_txoptions *opt,
						    u8 *proto);

extern u8 *			ipv6_skip_exthdr(struct ipv6_opt_hdr *hdr, 
					         u8 *nexthdrp, int len);

extern struct ipv6_txoptions *	ipv6_invert_rthdr(struct sock *sk,
						  struct ipv6_rt_hdr *hdr);


/*
 *	socket options (ipv6_sockglue.c)
 */

extern int			ipv6_setsockopt(struct sock *sk, int level, 
						int optname, char *optval, 
						int optlen);
extern int			ipv6_getsockopt(struct sock *sk, int level, 
						int optname, char *optval, 
						int *optlen);

extern void			ipv6_packet_init(void);

extern void			ipv6_netdev_notif_init(void);

extern void			ipv6_packet_cleanup(void);

extern void			ipv6_netdev_notif_cleanup(void);

extern int 			ipv6_recv_error(struct sock *sk, struct msghdr *msg, int len);
extern void			ipv6_icmp_error(struct sock *sk, struct sk_buff *skb, int err, u16 port,
						u32 info, u8 *payload);
extern void			ipv6_local_error(struct sock *sk, int err, struct flowi *fl, u32 info);

#endif /* __KERNEL__ */
#endif /* _NET_IPV6_H */



