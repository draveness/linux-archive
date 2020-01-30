/*
 *	inet6 interface/address list definitions
 *	Linux INET6 implementation 
 *
 *	Authors:
 *	Pedro Roque		<roque@di.fc.ul.pt>	
 *
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#ifndef _NET_IF_INET6_H
#define _NET_IF_INET6_H

#define IF_RA_RCVD	0x20
#define IF_RS_SENT	0x10

#ifdef __KERNEL__

struct inet6_ifaddr 
{
	struct in6_addr		addr;
	__u32			prefix_len;
	
	__u32			valid_lft;
	__u32			prefered_lft;
	unsigned long		tstamp;
	atomic_t		refcnt;
	spinlock_t		lock;

	__u8			probes;
	__u8			flags;

	__u16			scope;

	struct timer_list	timer;

	struct inet6_dev	*idev;

	struct inet6_ifaddr	*lst_next;      /* next addr in addr_lst */
	struct inet6_ifaddr	*if_next;       /* next addr in inet6_dev */

	int			dead;
};

struct ipv6_mc_socklist
{
	struct in6_addr		addr;
	int			ifindex;
	struct ipv6_mc_socklist *next;
};

#define MAF_TIMER_RUNNING	0x01
#define MAF_LAST_REPORTER	0x02
#define MAF_LOADED		0x04

struct ifmcaddr6
{
	struct in6_addr		mca_addr;
	struct inet6_dev	*idev;
	struct ifmcaddr6	*next;
	struct timer_list	mca_timer;
	unsigned		mca_flags;
	int			mca_users;
	atomic_t		mca_refcnt;
	spinlock_t		mca_lock;
};

#define	IFA_HOST	IPV6_ADDR_LOOPBACK
#define	IFA_LINK	IPV6_ADDR_LINKLOCAL
#define	IFA_SITE	IPV6_ADDR_SITELOCAL
#define	IFA_GLOBAL	0x0000U

struct ipv6_devconf
{
	int		forwarding;
	int		hop_limit;
	int		mtu6;
	int		accept_ra;
	int		accept_redirects;
	int		autoconf;
	int		dad_transmits;
	int		rtr_solicits;
	int		rtr_solicit_interval;
	int		rtr_solicit_delay;

	void		*sysctl;
};

struct inet6_dev 
{
	struct net_device		*dev;

	struct inet6_ifaddr	*addr_list;
	struct ifmcaddr6	*mc_list;
	rwlock_t		lock;
	atomic_t		refcnt;
	__u32			if_flags;
	int			dead;

	struct neigh_parms	*nd_parms;
	struct inet6_dev	*next;
	struct ipv6_devconf	cnf;
};

extern struct ipv6_devconf ipv6_devconf;

static inline void ipv6_eth_mc_map(struct in6_addr *addr, char *buf)
{
	/*
	 *	+-------+-------+-------+-------+-------+-------+
	 *      |   33  |   33  | DST13 | DST14 | DST15 | DST16 |
	 *      +-------+-------+-------+-------+-------+-------+
	 */

	buf[0]= 0x33;
	buf[1]= 0x33;

	memcpy(buf + 2, &addr->s6_addr32[3], sizeof(__u32));
}

static inline void ipv6_tr_mc_map(struct in6_addr *addr, char *buf)
{
	/* All nodes FF01::1, FF02::1, FF02::1:FFxx:xxxx */

	if (((addr->s6_addr[0] == 0xFF) &&
	    ((addr->s6_addr[1] == 0x01) || (addr->s6_addr[1] == 0x02)) &&
	     (addr->s6_addr16[1] == 0) &&
	     (addr->s6_addr32[1] == 0) &&
	     (addr->s6_addr32[2] == 0) &&
	     (addr->s6_addr16[6] == 0) &&
	     (addr->s6_addr[15] == 1)) ||
	    ((addr->s6_addr[0] == 0xFF) &&
	     (addr->s6_addr[1] == 0x02) &&
	     (addr->s6_addr16[1] == 0) &&
	     (addr->s6_addr32[1] == 0) &&
	     (addr->s6_addr16[4] == 0) &&
	     (addr->s6_addr[10] == 0) &&
	     (addr->s6_addr[11] == 1) &&
	     (addr->s6_addr[12] == 0xff)))
	{
		buf[0]=0xC0;
		buf[1]=0x00;
		buf[2]=0x01;
		buf[3]=0x00;
		buf[4]=0x00;
		buf[5]=0x00;
	/* All routers FF0x::2 */
	} else if ((addr->s6_addr[0] ==0xff) &&
		((addr->s6_addr[1] & 0xF0) == 0) &&
		(addr->s6_addr16[1] == 0) &&
		(addr->s6_addr32[1] == 0) &&
		(addr->s6_addr32[2] == 0) &&
		(addr->s6_addr16[6] == 0) &&
		(addr->s6_addr[15] == 2))
	{
		buf[0]=0xC0;
		buf[1]=0x00;
		buf[2]=0x02;
		buf[3]=0x00;
		buf[4]=0x00;
		buf[5]=0x00;
	} else {
		unsigned char i ; 
		
		i = addr->s6_addr[15] & 7 ; 
		buf[0]=0xC0;
		buf[1]=0x00;
		buf[2]=0x00;
		buf[3]=0x01 << i ; 
		buf[4]=0x00;
		buf[5]=0x00;
	}
}
#endif
#endif
