/*
 *	IPv6 Address [auto]configuration
 *	Linux INET6 implementation
 *
 *	Authors:
 *	Pedro Roque		<roque@di.fc.ul.pt>	
 *	Alexey Kuznetsov	<kuznet@ms2.inr.ac.ru>
 *
 *	$Id: addrconf.c,v 1.59 2000/11/28 11:39:43 davem Exp $
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

/*
 *	Changes:
 *
 *	Janos Farkas			:	delete timer on ifdown
 *	<chexum@bankinf.banki.hu>
 *	Andi Kleen			:	kill doube kfree on module
 *						unload.
 *	Maciej W. Rozycki		:	FDDI support
 */

#include <linux/config.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/sched.h>
#include <linux/net.h>
#include <linux/in6.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/route.h>
#include <linux/inetdevice.h>
#include <linux/init.h>
#ifdef CONFIG_SYSCTL
#include <linux/sysctl.h>
#endif
#include <linux/delay.h>

#include <linux/proc_fs.h>
#include <net/sock.h>
#include <net/snmp.h>

#include <net/ipv6.h>
#include <net/protocol.h>
#include <net/ndisc.h>
#include <net/ip6_route.h>
#include <net/addrconf.h>
#include <net/ip.h>
#include <linux/if_tunnel.h>
#include <linux/rtnetlink.h>

#include <asm/uaccess.h>

/* Set to 3 to get tracing... */
#define ACONF_DEBUG 2

#if ACONF_DEBUG >= 3
#define ADBG(x) printk x
#else
#define ADBG(x)
#endif

#ifdef CONFIG_SYSCTL
static void addrconf_sysctl_register(struct inet6_dev *idev, struct ipv6_devconf *p);
static void addrconf_sysctl_unregister(struct ipv6_devconf *p);
#endif

int inet6_dev_count;
int inet6_ifa_count;

/*
 *	Configured unicast address hash table
 */
static struct inet6_ifaddr		*inet6_addr_lst[IN6_ADDR_HSIZE];
static rwlock_t	addrconf_hash_lock = RW_LOCK_UNLOCKED;

/* Protects inet6 devices */
rwlock_t addrconf_lock = RW_LOCK_UNLOCKED;

void addrconf_verify(unsigned long);

static struct timer_list addr_chk_timer = { function: addrconf_verify };

static int addrconf_ifdown(struct net_device *dev, int how);

static void addrconf_dad_start(struct inet6_ifaddr *ifp);
static void addrconf_dad_timer(unsigned long data);
static void addrconf_dad_completed(struct inet6_ifaddr *ifp);
static void addrconf_rs_timer(unsigned long data);
static void ipv6_ifa_notify(int event, struct inet6_ifaddr *ifa);

struct ipv6_devconf ipv6_devconf =
{
	0,				/* forwarding		*/
	IPV6_DEFAULT_HOPLIMIT,		/* hop limit		*/
	IPV6_MIN_MTU,			/* mtu			*/
	1,				/* accept RAs		*/
	1,				/* accept redirects	*/
	1,				/* autoconfiguration	*/
	1,				/* dad transmits	*/
	MAX_RTR_SOLICITATIONS,		/* router solicits	*/
	RTR_SOLICITATION_INTERVAL,	/* rtr solicit interval	*/
	MAX_RTR_SOLICITATION_DELAY,	/* rtr solicit delay	*/
};

static struct ipv6_devconf ipv6_devconf_dflt =
{
	0,				/* forwarding		*/
	IPV6_DEFAULT_HOPLIMIT,		/* hop limit		*/
	IPV6_MIN_MTU,			/* mtu			*/
	1,				/* accept RAs		*/
	1,				/* accept redirects	*/
	1,				/* autoconfiguration	*/
	1,				/* dad transmits	*/
	MAX_RTR_SOLICITATIONS,		/* router solicits	*/
	RTR_SOLICITATION_INTERVAL,	/* rtr solicit interval	*/
	MAX_RTR_SOLICITATION_DELAY,	/* rtr solicit delay	*/
};

int ipv6_addr_type(struct in6_addr *addr)
{
	u32 st;

	st = addr->s6_addr32[0];

	/* Consider all addresses with the first three bits different of
	   000 and 111 as unicasts.
	 */
	if ((st & __constant_htonl(0xE0000000)) != __constant_htonl(0x00000000) &&
	    (st & __constant_htonl(0xE0000000)) != __constant_htonl(0xE0000000))
		return IPV6_ADDR_UNICAST;

	if ((st & __constant_htonl(0xFF000000)) == __constant_htonl(0xFF000000)) {
		int type = IPV6_ADDR_MULTICAST;

		switch((st & __constant_htonl(0x00FF0000))) {
			case __constant_htonl(0x00010000):
				type |= IPV6_ADDR_LOOPBACK;
				break;

			case __constant_htonl(0x00020000):
				type |= IPV6_ADDR_LINKLOCAL;
				break;

			case __constant_htonl(0x00050000):
				type |= IPV6_ADDR_SITELOCAL;
				break;
		};
		return type;
	}
	
	if ((st & __constant_htonl(0xFFC00000)) == __constant_htonl(0xFE800000))
		return (IPV6_ADDR_LINKLOCAL | IPV6_ADDR_UNICAST);

	if ((st & __constant_htonl(0xFFC00000)) == __constant_htonl(0xFEC00000))
		return (IPV6_ADDR_SITELOCAL | IPV6_ADDR_UNICAST);

	if ((addr->s6_addr32[0] | addr->s6_addr32[1]) == 0) {
		if (addr->s6_addr32[2] == 0) {
			if (addr->in6_u.u6_addr32[3] == 0)
				return IPV6_ADDR_ANY;

			if (addr->s6_addr32[3] == __constant_htonl(0x00000001))
				return (IPV6_ADDR_LOOPBACK | IPV6_ADDR_UNICAST);

			return (IPV6_ADDR_COMPATv4 | IPV6_ADDR_UNICAST);
		}

		if (addr->s6_addr32[2] == __constant_htonl(0x0000ffff))
			return IPV6_ADDR_MAPPED;
	}

	return IPV6_ADDR_RESERVED;
}

static void addrconf_del_timer(struct inet6_ifaddr *ifp)
{
	if (del_timer(&ifp->timer))
		__in6_ifa_put(ifp);
}

enum addrconf_timer_t
{
	AC_NONE,
	AC_DAD,
	AC_RS,
};

static void addrconf_mod_timer(struct inet6_ifaddr *ifp,
			       enum addrconf_timer_t what,
			       unsigned long when)
{
	if (!del_timer(&ifp->timer))
		in6_ifa_hold(ifp);

	switch (what) {
	case AC_DAD:
		ifp->timer.function = addrconf_dad_timer;
		break;
	case AC_RS:
		ifp->timer.function = addrconf_rs_timer;
		break;
	default:;
	}
	ifp->timer.expires = jiffies + when;
	add_timer(&ifp->timer);
}


/* Nobody refers to this device, we may destroy it. */

void in6_dev_finish_destroy(struct inet6_dev *idev)
{
	struct net_device *dev = idev->dev;
	BUG_TRAP(idev->addr_list==NULL);
	BUG_TRAP(idev->mc_list==NULL);
#ifdef NET_REFCNT_DEBUG
	printk(KERN_DEBUG "in6_dev_finish_destroy: %s\n", dev ? dev->name : "NIL");
#endif
	dev_put(dev);
	if (!idev->dead) {
		printk("Freeing alive inet6 device %p\n", idev);
		return;
	}
	inet6_dev_count--;
	kfree(idev);
}

static struct inet6_dev * ipv6_add_dev(struct net_device *dev)
{
	struct inet6_dev *ndev;

	ASSERT_RTNL();

	if (dev->mtu < IPV6_MIN_MTU)
		return NULL;

	ndev = kmalloc(sizeof(struct inet6_dev), GFP_KERNEL);

	if (ndev) {
		memset(ndev, 0, sizeof(struct inet6_dev));

		ndev->lock = RW_LOCK_UNLOCKED;
		ndev->dev = dev;
		memcpy(&ndev->cnf, &ipv6_devconf_dflt, sizeof(ndev->cnf));
		ndev->cnf.mtu6 = dev->mtu;
		ndev->cnf.sysctl = NULL;
		ndev->nd_parms = neigh_parms_alloc(dev, &nd_tbl);
		if (ndev->nd_parms == NULL) {
			kfree(ndev);
			return NULL;
		}
		inet6_dev_count++;
		/* We refer to the device */
		dev_hold(dev);

		write_lock_bh(&addrconf_lock);
		dev->ip6_ptr = ndev;
		/* One reference from device */
		in6_dev_hold(ndev);
		write_unlock_bh(&addrconf_lock);

#ifdef CONFIG_SYSCTL
		neigh_sysctl_register(dev, ndev->nd_parms, NET_IPV6, NET_IPV6_NEIGH, "ipv6");
		addrconf_sysctl_register(ndev, &ndev->cnf);
#endif
	}
	return ndev;
}

static struct inet6_dev * ipv6_find_idev(struct net_device *dev)
{
	struct inet6_dev *idev;

	ASSERT_RTNL();

	if ((idev = __in6_dev_get(dev)) == NULL) {
		if ((idev = ipv6_add_dev(dev)) == NULL)
			return NULL;
		if (dev->flags&IFF_UP)
			ipv6_mc_up(idev);
	}
	return idev;
}

static void addrconf_forward_change(struct inet6_dev *idev)
{
	struct net_device *dev;

	if (idev)
		return;

	read_lock(&dev_base_lock);
	for (dev=dev_base; dev; dev=dev->next) {
		read_lock(&addrconf_lock);
		idev = __in6_dev_get(dev);
		if (idev)
			idev->cnf.forwarding = ipv6_devconf.forwarding;
		read_unlock(&addrconf_lock);
	}
	read_unlock(&dev_base_lock);
}

/* Nobody refers to this ifaddr, destroy it */

void inet6_ifa_finish_destroy(struct inet6_ifaddr *ifp)
{
	BUG_TRAP(ifp->if_next==NULL);
	BUG_TRAP(ifp->lst_next==NULL);
	printk(KERN_DEBUG "inet6_ifa_finish_destroy\n");

	in6_dev_put(ifp->idev);

	if (del_timer(&ifp->timer))
		printk("Timer is still running, when freeing ifa=%p\n", ifp);

	if (!ifp->dead) {
		printk("Freeing alive inet6 address %p\n", ifp);
		return;
	}
	inet6_ifa_count--;
	kfree(ifp);
}

/* On success it returns ifp with increased reference count */

static struct inet6_ifaddr *
ipv6_add_addr(struct inet6_dev *idev, struct in6_addr *addr, int pfxlen,
	      int scope, unsigned flags)
{
	struct inet6_ifaddr *ifa;
	int hash;

	ifa = kmalloc(sizeof(struct inet6_ifaddr), GFP_ATOMIC);

	if (ifa == NULL) {
		ADBG(("ipv6_add_addr: malloc failed\n"));
		return NULL;
	}

	memset(ifa, 0, sizeof(struct inet6_ifaddr));
	ipv6_addr_copy(&ifa->addr, addr);

	spin_lock_init(&ifa->lock);
	init_timer(&ifa->timer);
	ifa->timer.data = (unsigned long) ifa;
	ifa->scope = scope;
	ifa->prefix_len = pfxlen;
	ifa->flags = flags | IFA_F_TENTATIVE;

	read_lock(&addrconf_lock);
	if (idev->dead) {
		read_unlock(&addrconf_lock);
		kfree(ifa);
		return NULL;
	}

	inet6_ifa_count++;
	ifa->idev = idev;
	in6_dev_hold(idev);
	/* For caller */
	in6_ifa_hold(ifa);

	/* Add to big hash table */
	hash = ipv6_addr_hash(addr);

	write_lock_bh(&addrconf_hash_lock);
	ifa->lst_next = inet6_addr_lst[hash];
	inet6_addr_lst[hash] = ifa;
	in6_ifa_hold(ifa);
	write_unlock_bh(&addrconf_hash_lock);

	write_lock_bh(&idev->lock);
	/* Add to inet6_dev unicast addr list. */
	ifa->if_next = idev->addr_list;
	idev->addr_list = ifa;
	in6_ifa_hold(ifa);
	write_unlock_bh(&idev->lock);
	read_unlock(&addrconf_lock);

	return ifa;
}

/* This function wants to get referenced ifp and releases it before return */

static void ipv6_del_addr(struct inet6_ifaddr *ifp)
{
	struct inet6_ifaddr *ifa, **ifap;
	struct inet6_dev *idev = ifp->idev;
	int hash;

	hash = ipv6_addr_hash(&ifp->addr);

	ifp->dead = 1;

	write_lock_bh(&addrconf_hash_lock);
	for (ifap = &inet6_addr_lst[hash]; (ifa=*ifap) != NULL;
	     ifap = &ifa->lst_next) {
		if (ifa == ifp) {
			*ifap = ifa->lst_next;
			__in6_ifa_put(ifp);
			ifa->lst_next = NULL;
			break;
		}
	}
	write_unlock_bh(&addrconf_hash_lock);

	write_lock_bh(&idev->lock);
	for (ifap = &idev->addr_list; (ifa=*ifap) != NULL;
	     ifap = &ifa->if_next) {
		if (ifa == ifp) {
			*ifap = ifa->if_next;
			__in6_ifa_put(ifp);
			ifa->if_next = NULL;
			break;
		}
	}
	write_unlock_bh(&idev->lock);

	ipv6_ifa_notify(RTM_DELADDR, ifp);


	addrconf_del_timer(ifp);

	in6_ifa_put(ifp);
}

/*
 *	Choose an apropriate source address
 *	should do:
 *	i)	get an address with an apropriate scope
 *	ii)	see if there is a specific route for the destination and use
 *		an address of the attached interface 
 *	iii)	don't use deprecated addresses
 */
int ipv6_get_saddr(struct dst_entry *dst,
		   struct in6_addr *daddr, struct in6_addr *saddr)
{
	int scope;
	struct inet6_ifaddr *ifp = NULL;
	struct inet6_ifaddr *match = NULL;
	struct net_device *dev = NULL;
	struct inet6_dev *idev;
	struct rt6_info *rt;
	int err;

	rt = (struct rt6_info *) dst;
	if (rt)
		dev = rt->rt6i_dev;

	scope = ipv6_addr_scope(daddr);
	if (rt && (rt->rt6i_flags & RTF_ALLONLINK)) {
		/*
		 *	route for the "all destinations on link" rule
		 *	when no routers are present
		 */
		scope = IFA_LINK;
	}

	/*
	 *	known dev
	 *	search dev and walk through dev addresses
	 */

	if (dev) {
		if (dev->flags & IFF_LOOPBACK)
			scope = IFA_HOST;

		read_lock(&addrconf_lock);
		idev = __in6_dev_get(dev);
		if (idev) {
			read_lock_bh(&idev->lock);
			for (ifp=idev->addr_list; ifp; ifp=ifp->if_next) {
				if (ifp->scope == scope) {
					if (!(ifp->flags & (IFA_F_DEPRECATED|IFA_F_TENTATIVE))) {
						in6_ifa_hold(ifp);
						read_unlock_bh(&idev->lock);
						read_unlock(&addrconf_lock);
						goto out;
					}

					if (!match && !(ifp->flags & IFA_F_TENTATIVE)) {
						match = ifp;
						in6_ifa_hold(ifp);
					}
				}
			}
			read_unlock_bh(&idev->lock);
		}
		read_unlock(&addrconf_lock);
	}

	if (scope == IFA_LINK)
		goto out;

	/*
	 *	dev == NULL or search failed for specified dev
	 */

	read_lock(&dev_base_lock);
	read_lock(&addrconf_lock);
	for (dev = dev_base; dev; dev=dev->next) {
		idev = __in6_dev_get(dev);
		if (idev) {
			read_lock_bh(&idev->lock);
			for (ifp=idev->addr_list; ifp; ifp=ifp->if_next) {
				if (ifp->scope == scope) {
					if (!(ifp->flags&(IFA_F_DEPRECATED|IFA_F_TENTATIVE))) {
						in6_ifa_hold(ifp);
						read_unlock_bh(&idev->lock);
						goto out_unlock_base;
					}

					if (!match && !(ifp->flags&IFA_F_TENTATIVE)) {
						match = ifp;
						in6_ifa_hold(ifp);
					}
				}
			}
			read_unlock_bh(&idev->lock);
		}
	}

out_unlock_base:
	read_unlock(&addrconf_lock);
	read_unlock(&dev_base_lock);

out:
	if (ifp == NULL) {
		ifp = match;
		match = NULL;
	}

	err = -EADDRNOTAVAIL;
	if (ifp) {
		ipv6_addr_copy(saddr, &ifp->addr);
		err = 0;
		in6_ifa_put(ifp);
	}
	if (match)
		in6_ifa_put(match);

	return err;
}

int ipv6_get_lladdr(struct net_device *dev, struct in6_addr *addr)
{
	struct inet6_dev *idev;
	int err = -EADDRNOTAVAIL;

	read_lock(&addrconf_lock);
	if ((idev = __in6_dev_get(dev)) != NULL) {
		struct inet6_ifaddr *ifp;

		read_lock_bh(&idev->lock);
		for (ifp=idev->addr_list; ifp; ifp=ifp->if_next) {
			if (ifp->scope == IFA_LINK && !(ifp->flags&IFA_F_TENTATIVE)) {
				ipv6_addr_copy(addr, &ifp->addr);
				err = 0;
				break;
			}
		}
		read_unlock_bh(&idev->lock);
	}
	read_unlock(&addrconf_lock);
	return err;
}

int ipv6_chk_addr(struct in6_addr *addr, struct net_device *dev)
{
	struct inet6_ifaddr * ifp;
	u8 hash = ipv6_addr_hash(addr);

	read_lock_bh(&addrconf_hash_lock);
	for(ifp = inet6_addr_lst[hash]; ifp; ifp=ifp->lst_next) {
		if (ipv6_addr_cmp(&ifp->addr, addr) == 0 &&
		    !(ifp->flags&IFA_F_TENTATIVE)) {
			if (dev == NULL || ifp->idev->dev == dev ||
			    !(ifp->scope&(IFA_LINK|IFA_HOST)))
				break;
		}
	}
	read_unlock_bh(&addrconf_hash_lock);
	return ifp != NULL;
}

struct inet6_ifaddr * ipv6_get_ifaddr(struct in6_addr *addr, struct net_device *dev)
{
	struct inet6_ifaddr * ifp;
	u8 hash = ipv6_addr_hash(addr);

	read_lock_bh(&addrconf_hash_lock);
	for(ifp = inet6_addr_lst[hash]; ifp; ifp=ifp->lst_next) {
		if (ipv6_addr_cmp(&ifp->addr, addr) == 0) {
			if (dev == NULL || ifp->idev->dev == dev ||
			    !(ifp->scope&(IFA_LINK|IFA_HOST))) {
				in6_ifa_hold(ifp);
				break;
			}
		}
	}
	read_unlock_bh(&addrconf_hash_lock);

	return ifp;
}

/* Gets referenced address, destroys ifaddr */

void addrconf_dad_failure(struct inet6_ifaddr *ifp)
{
	printk(KERN_INFO "%s: duplicate address detected!\n", ifp->idev->dev->name);
	if (ifp->flags&IFA_F_PERMANENT) {
		spin_lock_bh(&ifp->lock);
		addrconf_del_timer(ifp);
		ifp->flags |= IFA_F_TENTATIVE;
		spin_unlock_bh(&ifp->lock);
		in6_ifa_put(ifp);
	} else
		ipv6_del_addr(ifp);
}


/* Join to solicited addr multicast group. */

static void addrconf_join_solict(struct net_device *dev, struct in6_addr *addr)
{
	struct in6_addr maddr;

	if (dev->flags&(IFF_LOOPBACK|IFF_NOARP))
		return;

#ifndef CONFIG_IPV6_NO_PB
	addrconf_addr_solict_mult_old(addr, &maddr);
	ipv6_dev_mc_inc(dev, &maddr);
#endif
#ifdef CONFIG_IPV6_EUI64
	addrconf_addr_solict_mult_new(addr, &maddr);
	ipv6_dev_mc_inc(dev, &maddr);
#endif
}

static void addrconf_leave_solict(struct net_device *dev, struct in6_addr *addr)
{
	struct in6_addr maddr;

	if (dev->flags&(IFF_LOOPBACK|IFF_NOARP))
		return;

#ifndef CONFIG_IPV6_NO_PB
	addrconf_addr_solict_mult_old(addr, &maddr);
	ipv6_dev_mc_dec(dev, &maddr);
#endif
#ifdef CONFIG_IPV6_EUI64
	addrconf_addr_solict_mult_new(addr, &maddr);
	ipv6_dev_mc_dec(dev, &maddr);
#endif
}


#ifdef CONFIG_IPV6_EUI64
static int ipv6_generate_eui64(u8 *eui, struct net_device *dev)
{
	switch (dev->type) {
	case ARPHRD_ETHER:
	case ARPHRD_FDDI:
	case ARPHRD_IEEE802_TR:
		if (dev->addr_len != ETH_ALEN)
			return -1;
		memcpy(eui, dev->dev_addr, 3);
		memcpy(eui + 5, dev->dev_addr+3, 3);
		eui[3] = 0xFF;
		eui[4] = 0xFE;
		eui[0] ^= 2;
		return 0;
	}
	return -1;
}

static int ipv6_inherit_eui64(u8 *eui, struct inet6_dev *idev)
{
	int err = -1;
	struct inet6_ifaddr *ifp;

	read_lock_bh(&idev->lock);
	for (ifp=idev->addr_list; ifp; ifp=ifp->if_next) {
		if (ifp->scope == IFA_LINK && !(ifp->flags&IFA_F_TENTATIVE)) {
			memcpy(eui, ifp->addr.s6_addr+8, 8);
			err = 0;
			break;
		}
	}
	read_unlock_bh(&idev->lock);
	return err;
}
#endif

/*
 *	Add prefix route.
 */

static void
addrconf_prefix_route(struct in6_addr *pfx, int plen, struct net_device *dev,
		      unsigned long expires, unsigned flags)
{
	struct in6_rtmsg rtmsg;

	memset(&rtmsg, 0, sizeof(rtmsg));
	memcpy(&rtmsg.rtmsg_dst, pfx, sizeof(struct in6_addr));
	rtmsg.rtmsg_dst_len = plen;
	rtmsg.rtmsg_metric = IP6_RT_PRIO_ADDRCONF;
	rtmsg.rtmsg_ifindex = dev->ifindex;
	rtmsg.rtmsg_info = expires;
	rtmsg.rtmsg_flags = RTF_UP|flags;
	rtmsg.rtmsg_type = RTMSG_NEWROUTE;

	/* Prevent useless cloning on PtP SIT.
	   This thing is done here expecting that the whole
	   class of non-broadcast devices need not cloning.
	 */
	if (dev->type == ARPHRD_SIT && (dev->flags&IFF_POINTOPOINT))
		rtmsg.rtmsg_flags |= RTF_NONEXTHOP;

	ip6_route_add(&rtmsg);
}

/* Create "default" multicast route to the interface */

static void addrconf_add_mroute(struct net_device *dev)
{
	struct in6_rtmsg rtmsg;

	memset(&rtmsg, 0, sizeof(rtmsg));
	ipv6_addr_set(&rtmsg.rtmsg_dst,
		      __constant_htonl(0xFF000000), 0, 0, 0);
	rtmsg.rtmsg_dst_len = 8;
	rtmsg.rtmsg_metric = IP6_RT_PRIO_ADDRCONF;
	rtmsg.rtmsg_ifindex = dev->ifindex;
	rtmsg.rtmsg_flags = RTF_UP|RTF_ADDRCONF;
	rtmsg.rtmsg_type = RTMSG_NEWROUTE;
	ip6_route_add(&rtmsg);
}

static void sit_route_add(struct net_device *dev)
{
	struct in6_rtmsg rtmsg;

	memset(&rtmsg, 0, sizeof(rtmsg));

	rtmsg.rtmsg_type	= RTMSG_NEWROUTE;
	rtmsg.rtmsg_metric	= IP6_RT_PRIO_ADDRCONF;

	/* prefix length - 96 bytes "::d.d.d.d" */
	rtmsg.rtmsg_dst_len	= 96;
	rtmsg.rtmsg_flags	= RTF_UP|RTF_NONEXTHOP;
	rtmsg.rtmsg_ifindex	= dev->ifindex;

	ip6_route_add(&rtmsg);
}

static void addrconf_add_lroute(struct net_device *dev)
{
	struct in6_addr addr;

	ipv6_addr_set(&addr,  __constant_htonl(0xFE800000), 0, 0, 0);
	addrconf_prefix_route(&addr, 10, dev, 0, RTF_ADDRCONF);
}

static struct inet6_dev *addrconf_add_dev(struct net_device *dev)
{
	struct inet6_dev *idev;

	ASSERT_RTNL();

	if ((idev = ipv6_find_idev(dev)) == NULL)
		return NULL;

	/* Add default multicast route */
	addrconf_add_mroute(dev);

	/* Add link local route */
	addrconf_add_lroute(dev);
	return idev;
}

void addrconf_prefix_rcv(struct net_device *dev, u8 *opt, int len)
{
	struct prefix_info *pinfo;
	struct rt6_info *rt;
	__u32 valid_lft;
	__u32 prefered_lft;
	int addr_type;
	unsigned long rt_expires;
	struct inet6_dev *in6_dev;

	pinfo = (struct prefix_info *) opt;
	
	if (len < sizeof(struct prefix_info)) {
		ADBG(("addrconf: prefix option too short\n"));
		return;
	}
	
	/*
	 *	Validation checks ([ADDRCONF], page 19)
	 */

	addr_type = ipv6_addr_type(&pinfo->prefix);

	if (addr_type & (IPV6_ADDR_MULTICAST|IPV6_ADDR_LINKLOCAL))
		return;

	valid_lft = ntohl(pinfo->valid);
	prefered_lft = ntohl(pinfo->prefered);

	if (prefered_lft > valid_lft) {
		printk(KERN_WARNING "addrconf: prefix option has invalid lifetime\n");
		return;
	}

	in6_dev = in6_dev_get(dev);

	if (in6_dev == NULL) {
		printk(KERN_DEBUG "addrconf: device %s not configured\n", dev->name);
		return;
	}

	/*
	 *	Two things going on here:
	 *	1) Add routes for on-link prefixes
	 *	2) Configure prefixes with the auto flag set
	 */

	/* Avoid arithemtic overflow. Really, we could
	   save rt_expires in seconds, likely valid_lft,
	   but it would require division in fib gc, that it
	   not good.
	 */
	if (valid_lft >= 0x7FFFFFFF/HZ)
		rt_expires = 0;
	else
		rt_expires = jiffies + valid_lft * HZ;

	rt = rt6_lookup(&pinfo->prefix, NULL, dev->ifindex, 1);

	if (rt && ((rt->rt6i_flags & (RTF_GATEWAY | RTF_DEFAULT)) == 0)) {
		if (rt->rt6i_flags&RTF_EXPIRES) {
			if (pinfo->onlink == 0 || valid_lft == 0) {
				ip6_del_rt(rt);
				rt = NULL;
			} else {
				rt->rt6i_expires = rt_expires;
			}
		}
	} else if (pinfo->onlink && valid_lft) {
		addrconf_prefix_route(&pinfo->prefix, pinfo->prefix_len,
				      dev, rt_expires, RTF_ADDRCONF|RTF_EXPIRES);
	}
	if (rt)
		dst_release(&rt->u.dst);

	/* Try to figure out our local address for this prefix */

	if (pinfo->autoconf && in6_dev->cnf.autoconf) {
		struct inet6_ifaddr * ifp;
		struct in6_addr addr;
		int plen;

		plen = pinfo->prefix_len >> 3;

#ifdef CONFIG_IPV6_EUI64
		if (pinfo->prefix_len == 64) {
			memcpy(&addr, &pinfo->prefix, 8);
			if (ipv6_generate_eui64(addr.s6_addr + 8, dev) &&
			    ipv6_inherit_eui64(addr.s6_addr + 8, in6_dev)) {
				in6_dev_put(in6_dev);
				return;
			}
			goto ok;
		}
#endif
#ifndef CONFIG_IPV6_NO_PB
		if (pinfo->prefix_len == ((sizeof(struct in6_addr) - dev->addr_len)<<3)) {
			memcpy(&addr, &pinfo->prefix, plen);
			memcpy(addr.s6_addr + plen, dev->dev_addr,
			       dev->addr_len);
			goto ok;
		}
#endif
		printk(KERN_DEBUG "IPv6 addrconf: prefix with wrong length %d\n", pinfo->prefix_len);
		in6_dev_put(in6_dev);
		return;

ok:

		ifp = ipv6_get_ifaddr(&addr, dev);

		if (ifp == NULL && valid_lft) {
			ifp = ipv6_add_addr(in6_dev, &addr, pinfo->prefix_len,
					    addr_type&IPV6_ADDR_SCOPE_MASK, 0);

			if (ifp == NULL) {
				in6_dev_put(in6_dev);
				return;
			}

			addrconf_dad_start(ifp);
		}

		if (ifp && valid_lft == 0) {
			ipv6_del_addr(ifp);
			ifp = NULL;
		}

		if (ifp) {
			int flags;

			spin_lock(&ifp->lock);
			ifp->valid_lft = valid_lft;
			ifp->prefered_lft = prefered_lft;
			ifp->tstamp = jiffies;
			flags = ifp->flags;
			ifp->flags &= ~IFA_F_DEPRECATED;
			spin_unlock(&ifp->lock);

			if (!(flags&IFA_F_TENTATIVE))
				ipv6_ifa_notify((flags&IFA_F_DEPRECATED) ?
						0 : RTM_NEWADDR, ifp);
			in6_ifa_put(ifp);
		}
	}
	in6_dev_put(in6_dev);
}

/*
 *	Set destination address.
 *	Special case for SIT interfaces where we create a new "virtual"
 *	device.
 */
int addrconf_set_dstaddr(void *arg)
{
	struct in6_ifreq ireq;
	struct net_device *dev;
	int err = -EINVAL;

	rtnl_lock();

	err = -EFAULT;
	if (copy_from_user(&ireq, arg, sizeof(struct in6_ifreq)))
		goto err_exit;

	dev = __dev_get_by_index(ireq.ifr6_ifindex);

	err = -ENODEV;
	if (dev == NULL)
		goto err_exit;

	if (dev->type == ARPHRD_SIT) {
		struct ifreq ifr;
		mm_segment_t	oldfs;
		struct ip_tunnel_parm p;

		err = -EADDRNOTAVAIL;
		if (!(ipv6_addr_type(&ireq.ifr6_addr) & IPV6_ADDR_COMPATv4))
			goto err_exit;

		memset(&p, 0, sizeof(p));
		p.iph.daddr = ireq.ifr6_addr.s6_addr32[3];
		p.iph.saddr = 0;
		p.iph.version = 4;
		p.iph.ihl = 5;
		p.iph.protocol = IPPROTO_IPV6;
		p.iph.ttl = 64;
		ifr.ifr_ifru.ifru_data = (void*)&p;

		oldfs = get_fs(); set_fs(KERNEL_DS);
		err = dev->do_ioctl(dev, &ifr, SIOCADDTUNNEL);
		set_fs(oldfs);

		if (err == 0) {
			err = -ENOBUFS;
			if ((dev = __dev_get_by_name(p.name)) == NULL)
				goto err_exit;
			err = dev_open(dev);
		}
	}

err_exit:
	rtnl_unlock();
	return err;
}

/*
 *	Manual configuration of address on an interface
 */
static int inet6_addr_add(int ifindex, struct in6_addr *pfx, int plen)
{
	struct inet6_ifaddr *ifp;
	struct inet6_dev *idev;
	struct net_device *dev;
	int scope;

	ASSERT_RTNL();
	
	if ((dev = __dev_get_by_index(ifindex)) == NULL)
		return -ENODEV;
	
	if (!(dev->flags&IFF_UP))
		return -ENETDOWN;

	if ((idev = addrconf_add_dev(dev)) == NULL)
		return -ENOBUFS;

	scope = ipv6_addr_scope(pfx);

	if ((ifp = ipv6_add_addr(idev, pfx, plen, scope, IFA_F_PERMANENT)) != NULL) {
		addrconf_dad_start(ifp);
		in6_ifa_put(ifp);
		return 0;
	}

	return -ENOBUFS;
}

static int inet6_addr_del(int ifindex, struct in6_addr *pfx, int plen)
{
	struct inet6_ifaddr *ifp;
	struct inet6_dev *idev;
	struct net_device *dev;
	
	if ((dev = __dev_get_by_index(ifindex)) == NULL)
		return -ENODEV;

	if ((idev = __in6_dev_get(dev)) == NULL)
		return -ENXIO;

	read_lock_bh(&idev->lock);
	for (ifp = idev->addr_list; ifp; ifp=ifp->if_next) {
		if (ifp->prefix_len == plen &&
		    (!memcmp(pfx, &ifp->addr, sizeof(struct in6_addr)))) {
			in6_ifa_hold(ifp);
			read_unlock_bh(&idev->lock);
			
			ipv6_del_addr(ifp);

			/* If the last address is deleted administratively,
			   disable IPv6 on this interface.
			 */
			if (idev->addr_list == NULL)
				addrconf_ifdown(idev->dev, 1);
			return 0;
		}
	}
	read_unlock_bh(&idev->lock);
	return -EADDRNOTAVAIL;
}


int addrconf_add_ifaddr(void *arg)
{
	struct in6_ifreq ireq;
	int err;
	
	if (!capable(CAP_NET_ADMIN))
		return -EPERM;
	
	if (copy_from_user(&ireq, arg, sizeof(struct in6_ifreq)))
		return -EFAULT;

	rtnl_lock();
	err = inet6_addr_add(ireq.ifr6_ifindex, &ireq.ifr6_addr, ireq.ifr6_prefixlen);
	rtnl_unlock();
	return err;
}

int addrconf_del_ifaddr(void *arg)
{
	struct in6_ifreq ireq;
	int err;
	
	if (!capable(CAP_NET_ADMIN))
		return -EPERM;

	if (copy_from_user(&ireq, arg, sizeof(struct in6_ifreq)))
		return -EFAULT;

	rtnl_lock();
	err = inet6_addr_del(ireq.ifr6_ifindex, &ireq.ifr6_addr, ireq.ifr6_prefixlen);
	rtnl_unlock();
	return err;
}

static void sit_add_v4_addrs(struct inet6_dev *idev)
{
	struct inet6_ifaddr * ifp;
	struct in6_addr addr;
	struct net_device *dev;
	int scope;

	ASSERT_RTNL();

	memset(&addr, 0, sizeof(struct in6_addr));
	memcpy(&addr.s6_addr32[3], idev->dev->dev_addr, 4);

	if (idev->dev->flags&IFF_POINTOPOINT) {
		addr.s6_addr32[0] = __constant_htonl(0xfe800000);
		scope = IFA_LINK;
	} else {
		scope = IPV6_ADDR_COMPATv4;
	}

	if (addr.s6_addr32[3]) {
		ifp = ipv6_add_addr(idev, &addr, 128, scope, IFA_F_PERMANENT);
		if (ifp) {
			spin_lock_bh(&ifp->lock);
			ifp->flags &= ~IFA_F_TENTATIVE;
			spin_unlock_bh(&ifp->lock);
			ipv6_ifa_notify(RTM_NEWADDR, ifp);
			in6_ifa_put(ifp);
		}
		return;
	}

        for (dev = dev_base; dev != NULL; dev = dev->next) {
		struct in_device * in_dev = __in_dev_get(dev);
		if (in_dev && (dev->flags & IFF_UP)) {
			struct in_ifaddr * ifa;

			int flag = scope;

			for (ifa = in_dev->ifa_list; ifa; ifa = ifa->ifa_next) {
				int plen;

				addr.s6_addr32[3] = ifa->ifa_local;

				if (ifa->ifa_scope == RT_SCOPE_LINK)
					continue;
				if (ifa->ifa_scope >= RT_SCOPE_HOST) {
					if (idev->dev->flags&IFF_POINTOPOINT)
						continue;
					flag |= IFA_HOST;
				}
				if (idev->dev->flags&IFF_POINTOPOINT)
					plen = 10;
				else
					plen = 96;

				ifp = ipv6_add_addr(idev, &addr, plen, flag,
						    IFA_F_PERMANENT);
				if (ifp) {
					spin_lock_bh(&ifp->lock);
					ifp->flags &= ~IFA_F_TENTATIVE;
					spin_unlock_bh(&ifp->lock);
					ipv6_ifa_notify(RTM_NEWADDR, ifp);
					in6_ifa_put(ifp);
				}
			}
		}
        }
}

static void init_loopback(struct net_device *dev)
{
	struct in6_addr addr;
	struct inet6_dev  *idev;
	struct inet6_ifaddr * ifp;

	/* ::1 */

	ASSERT_RTNL();

	memset(&addr, 0, sizeof(struct in6_addr));
	addr.s6_addr[15] = 1;

	if ((idev = ipv6_find_idev(dev)) == NULL) {
		printk(KERN_DEBUG "init loopback: add_dev failed\n");
		return;
	}

	ifp = ipv6_add_addr(idev, &addr, 128, IFA_HOST, IFA_F_PERMANENT);
	if (ifp) {
		spin_lock_bh(&ifp->lock);
		ifp->flags &= ~IFA_F_TENTATIVE;
		spin_unlock_bh(&ifp->lock);
		ipv6_ifa_notify(RTM_NEWADDR, ifp);
		in6_ifa_put(ifp);
	}
}

static void addrconf_add_linklocal(struct inet6_dev *idev, struct in6_addr *addr)
{
	struct inet6_ifaddr * ifp;

	ifp = ipv6_add_addr(idev, addr, 10, IFA_LINK, IFA_F_PERMANENT);
	if (ifp) {
		addrconf_dad_start(ifp);
		in6_ifa_put(ifp);
	}
}

static void addrconf_dev_config(struct net_device *dev)
{
	struct in6_addr addr;
	struct inet6_dev    * idev;

	ASSERT_RTNL();

	if ((dev->type != ARPHRD_ETHER) && 
	    (dev->type != ARPHRD_FDDI) &&
	    (dev->type != ARPHRD_IEEE802_TR)) {
		/* Alas, we support only Ethernet autoconfiguration. */
		return;
	}

	idev = addrconf_add_dev(dev);
	if (idev == NULL)
		return;

#ifdef CONFIG_IPV6_EUI64
	memset(&addr, 0, sizeof(struct in6_addr));

	addr.s6_addr[0] = 0xFE;
	addr.s6_addr[1] = 0x80;

	if (ipv6_generate_eui64(addr.s6_addr + 8, dev) == 0)
		addrconf_add_linklocal(idev, &addr);
#endif

#ifndef CONFIG_IPV6_NO_PB
	memset(&addr, 0, sizeof(struct in6_addr));

	addr.s6_addr[0] = 0xFE;
	addr.s6_addr[1] = 0x80;

	memcpy(addr.s6_addr + (sizeof(struct in6_addr) - dev->addr_len), 
	       dev->dev_addr, dev->addr_len);
	addrconf_add_linklocal(idev, &addr);
#endif
}

static void addrconf_sit_config(struct net_device *dev)
{
	struct inet6_dev *idev;

	ASSERT_RTNL();

	/* 
	 * Configure the tunnel with one of our IPv4 
	 * addresses... we should configure all of 
	 * our v4 addrs in the tunnel
	 */

	if ((idev = ipv6_find_idev(dev)) == NULL) {
		printk(KERN_DEBUG "init sit: add_dev failed\n");
		return;
	}

	sit_add_v4_addrs(idev);

	if (dev->flags&IFF_POINTOPOINT) {
		addrconf_add_mroute(dev);
		addrconf_add_lroute(dev);
	} else
		sit_route_add(dev);
}


int addrconf_notify(struct notifier_block *this, unsigned long event, 
		    void * data)
{
	struct net_device *dev;

	dev = (struct net_device *) data;

	switch(event) {
	case NETDEV_UP:
		switch(dev->type) {
		case ARPHRD_SIT:
			addrconf_sit_config(dev);
			break;

		case ARPHRD_LOOPBACK:
			init_loopback(dev);
			break;

		default:
			addrconf_dev_config(dev);
			break;
		};
		break;

	case NETDEV_CHANGEMTU:
		if (dev->mtu >= IPV6_MIN_MTU) {
			struct inet6_dev *idev;

			if ((idev = __in6_dev_get(dev)) == NULL)
				break;
			idev->cnf.mtu6 = dev->mtu;
			rt6_mtu_change(dev, dev->mtu);
			break;
		}

		/* MTU falled under IPV6_MIN_MTU. Stop IPv6 on this interface. */

	case NETDEV_DOWN:
	case NETDEV_UNREGISTER:
		/*
		 *	Remove all addresses from this interface.
		 */
		addrconf_ifdown(dev, event != NETDEV_DOWN);
		break;
	case NETDEV_CHANGE:
		break;
	};

	return NOTIFY_OK;
}

static int addrconf_ifdown(struct net_device *dev, int how)
{
	struct inet6_dev *idev;
	struct inet6_ifaddr *ifa, **bifa;
	int i;

	ASSERT_RTNL();

	rt6_ifdown(dev);
	neigh_ifdown(&nd_tbl, dev);

	idev = __in6_dev_get(dev);
	if (idev == NULL)
		return -ENODEV;

	/* Step 1: remove reference to ipv6 device from parent device.
	           Do not dev_put!
	 */
	if (how == 1) {
		write_lock_bh(&addrconf_lock);
		dev->ip6_ptr = NULL;
		idev->dead = 1;
		write_unlock_bh(&addrconf_lock);
	}

	/* Step 2: clear hash table */
	for (i=0; i<IN6_ADDR_HSIZE; i++) {
		bifa = &inet6_addr_lst[i];

		write_lock_bh(&addrconf_hash_lock);
		while ((ifa = *bifa) != NULL) {
			if (ifa->idev == idev) {
				*bifa = ifa->lst_next;
				ifa->lst_next = NULL;
				addrconf_del_timer(ifa);
				in6_ifa_put(ifa);
				continue;
			}
			bifa = &ifa->lst_next;
		}
		write_unlock_bh(&addrconf_hash_lock);
	}

	/* Step 3: clear address list */

	write_lock_bh(&idev->lock);
	while ((ifa = idev->addr_list) != NULL) {
		idev->addr_list = ifa->if_next;
		ifa->if_next = NULL;
		ifa->dead = 1;
		addrconf_del_timer(ifa);
		write_unlock_bh(&idev->lock);

		ipv6_ifa_notify(RTM_DELADDR, ifa);
		in6_ifa_put(ifa);

		write_lock_bh(&idev->lock);
	}
	write_unlock_bh(&idev->lock);

	/* Step 4: Discard multicast list */

	if (how == 1)
		ipv6_mc_destroy_dev(idev);
	else
		ipv6_mc_down(idev);

	/* Shot the device (if unregistered) */

	if (how == 1) {
		neigh_parms_release(&nd_tbl, idev->nd_parms);
#ifdef CONFIG_SYSCTL
		addrconf_sysctl_unregister(&idev->cnf);
#endif
		in6_dev_put(idev);
	}
	return 0;
}

static void addrconf_rs_timer(unsigned long data)
{
	struct inet6_ifaddr *ifp = (struct inet6_ifaddr *) data;

	if (ifp->idev->cnf.forwarding)
		goto out;

	if (ifp->idev->if_flags & IF_RA_RCVD) {
		/*
		 *	Announcement received after solicitation
		 *	was sent
		 */
		goto out;
	}

	spin_lock(&ifp->lock);
	if (ifp->probes++ <= ifp->idev->cnf.rtr_solicits) {
		struct in6_addr all_routers;

		addrconf_mod_timer(ifp, AC_RS,
				   ifp->idev->cnf.rtr_solicit_interval);
		spin_unlock(&ifp->lock);

		ipv6_addr_all_routers(&all_routers);

		ndisc_send_rs(ifp->idev->dev, &ifp->addr, &all_routers);
	} else {
		struct in6_rtmsg rtmsg;

		spin_unlock(&ifp->lock);

		printk(KERN_DEBUG "%s: no IPv6 routers present\n",
		       ifp->idev->dev->name);

		memset(&rtmsg, 0, sizeof(struct in6_rtmsg));
		rtmsg.rtmsg_type = RTMSG_NEWROUTE;
		rtmsg.rtmsg_metric = IP6_RT_PRIO_ADDRCONF;
		rtmsg.rtmsg_flags = (RTF_ALLONLINK | RTF_ADDRCONF | 
				     RTF_DEFAULT | RTF_UP);

		rtmsg.rtmsg_ifindex = ifp->idev->dev->ifindex;

		ip6_route_add(&rtmsg);
	}

out:
	in6_ifa_put(ifp);
}

/*
 *	Duplicate Address Detection
 */
static void addrconf_dad_start(struct inet6_ifaddr *ifp)
{
	struct net_device *dev;
	unsigned long rand_num;

	dev = ifp->idev->dev;

	addrconf_join_solict(dev, &ifp->addr);

	if (ifp->prefix_len != 128 && (ifp->flags&IFA_F_PERMANENT))
		addrconf_prefix_route(&ifp->addr, ifp->prefix_len, dev, 0, RTF_ADDRCONF);

	net_srandom(ifp->addr.s6_addr32[3]);
	rand_num = net_random() % (ifp->idev->cnf.rtr_solicit_delay ? : 1);

	spin_lock_bh(&ifp->lock);

	if (dev->flags&(IFF_NOARP|IFF_LOOPBACK) ||
	    !(ifp->flags&IFA_F_TENTATIVE)) {
		ifp->flags &= ~IFA_F_TENTATIVE;
		spin_unlock_bh(&ifp->lock);

		addrconf_dad_completed(ifp);
		return;
	}

	ifp->probes = ifp->idev->cnf.dad_transmits;
	addrconf_mod_timer(ifp, AC_DAD, rand_num);

	spin_unlock_bh(&ifp->lock);
}

static void addrconf_dad_timer(unsigned long data)
{
	struct inet6_ifaddr *ifp = (struct inet6_ifaddr *) data;
	struct in6_addr unspec;
	struct in6_addr mcaddr;

	spin_lock_bh(&ifp->lock);
	if (ifp->probes == 0) {
		/*
		 * DAD was successful
		 */

		ifp->flags &= ~IFA_F_TENTATIVE;
		spin_unlock_bh(&ifp->lock);

		addrconf_dad_completed(ifp);

		in6_ifa_put(ifp);
		return;
	}

	ifp->probes--;
	addrconf_mod_timer(ifp, AC_DAD, ifp->idev->cnf.rtr_solicit_interval);
	spin_unlock_bh(&ifp->lock);

	/* send a neighbour solicitation for our addr */
	memset(&unspec, 0, sizeof(unspec));
#ifdef CONFIG_IPV6_EUI64
	addrconf_addr_solict_mult_new(&ifp->addr, &mcaddr);
	ndisc_send_ns(ifp->idev->dev, NULL, &ifp->addr, &mcaddr, &unspec);
#endif
#ifndef CONFIG_IPV6_NO_PB
	addrconf_addr_solict_mult_old(&ifp->addr, &mcaddr);
	ndisc_send_ns(ifp->idev->dev, NULL, &ifp->addr, &mcaddr, &unspec);
#endif

	in6_ifa_put(ifp);
}

static void addrconf_dad_completed(struct inet6_ifaddr *ifp)
{
	struct net_device *	dev = ifp->idev->dev;

	/*
	 *	Configure the address for reception. Now it is valid.
	 */

	ipv6_ifa_notify(RTM_NEWADDR, ifp);

	/* If added prefix is link local and forwarding is off,
	   start sending router solicitations.
	 */

	if (ifp->idev->cnf.forwarding == 0 &&
	    (dev->flags&IFF_LOOPBACK) == 0 &&
	    (ipv6_addr_type(&ifp->addr) & IPV6_ADDR_LINKLOCAL)) {
		struct in6_addr all_routers;

		ipv6_addr_all_routers(&all_routers);

		/*
		 *	If a host as already performed a random delay
		 *	[...] as part of DAD [...] there is no need
		 *	to delay again before sending the first RS
		 */
		ndisc_send_rs(ifp->idev->dev, &ifp->addr, &all_routers);

		spin_lock_bh(&ifp->lock);
		ifp->probes = 1;
		ifp->idev->if_flags |= IF_RS_SENT;
		addrconf_mod_timer(ifp, AC_RS, ifp->idev->cnf.rtr_solicit_interval);
		spin_unlock_bh(&ifp->lock);
	}
}

#ifdef CONFIG_PROC_FS
static int iface_proc_info(char *buffer, char **start, off_t offset,
			   int length)
{
	struct inet6_ifaddr *ifp;
	int i;
	int len = 0;
	off_t pos=0;
	off_t begin=0;

	for (i=0; i < IN6_ADDR_HSIZE; i++) {
		read_lock_bh(&addrconf_hash_lock);
		for (ifp=inet6_addr_lst[i]; ifp; ifp=ifp->lst_next) {
			int j;

			for (j=0; j<16; j++) {
				sprintf(buffer + len, "%02x",
					ifp->addr.s6_addr[j]);
				len += 2;
			}

			len += sprintf(buffer + len,
				       " %02x %02x %02x %02x %8s\n",
				       ifp->idev->dev->ifindex,
				       ifp->prefix_len,
				       ifp->scope,
				       ifp->flags,
				       ifp->idev->dev->name);
			pos=begin+len;
			if(pos<offset) {
				len=0;
				begin=pos;
			}
			if(pos>offset+length) {
				read_unlock_bh(&addrconf_hash_lock);
				goto done;
			}
		}
		read_unlock_bh(&addrconf_hash_lock);
	}

done:

	*start=buffer+(offset-begin);
	len-=(offset-begin);
	if(len>length)
		len=length;
	if(len<0)
		len=0;
	return len;
}

#endif	/* CONFIG_PROC_FS */

/*
 *	Periodic address status verification
 */

void addrconf_verify(unsigned long foo)
{
	struct inet6_ifaddr *ifp;
	unsigned long now = jiffies;
	int i;

	for (i=0; i < IN6_ADDR_HSIZE; i++) {

restart:
		write_lock(&addrconf_hash_lock);
		for (ifp=inet6_addr_lst[i]; ifp; ifp=ifp->lst_next) {
			unsigned long age;

			if (ifp->flags & IFA_F_PERMANENT)
				continue;

			age = (now - ifp->tstamp) / HZ;

			if (age > ifp->valid_lft) {
				in6_ifa_hold(ifp);
				write_unlock(&addrconf_hash_lock);
				ipv6_del_addr(ifp);
				goto restart;
			} else if (age > ifp->prefered_lft) {
				int deprecate = 0;

				spin_lock(&ifp->lock);
				if (!(ifp->flags&IFA_F_DEPRECATED)) {
					deprecate = 1;
					ifp->flags |= IFA_F_DEPRECATED;
				}
				spin_unlock(&ifp->lock);

				if (deprecate) {
					in6_ifa_hold(ifp);
					write_unlock(&addrconf_hash_lock);

					ipv6_ifa_notify(0, ifp);
					in6_ifa_put(ifp);
					goto restart;
				}
			}
		}
		write_unlock(&addrconf_hash_lock);
	}

	mod_timer(&addr_chk_timer, jiffies + ADDR_CHECK_FREQUENCY);
}

#ifdef CONFIG_RTNETLINK

static int
inet6_rtm_deladdr(struct sk_buff *skb, struct nlmsghdr *nlh, void *arg)
{
	struct rtattr **rta = arg;
	struct ifaddrmsg *ifm = NLMSG_DATA(nlh);
	struct in6_addr *pfx;

	pfx = NULL;
	if (rta[IFA_ADDRESS-1]) {
		if (RTA_PAYLOAD(rta[IFA_ADDRESS-1]) < sizeof(*pfx))
			return -EINVAL;
		pfx = RTA_DATA(rta[IFA_ADDRESS-1]);
	}
	if (rta[IFA_LOCAL-1]) {
		if (pfx && memcmp(pfx, RTA_DATA(rta[IFA_LOCAL-1]), sizeof(*pfx)))
			return -EINVAL;
		pfx = RTA_DATA(rta[IFA_LOCAL-1]);
	}
	if (pfx == NULL)
		return -EINVAL;

	return inet6_addr_del(ifm->ifa_index, pfx, ifm->ifa_prefixlen);
}

static int
inet6_rtm_newaddr(struct sk_buff *skb, struct nlmsghdr *nlh, void *arg)
{
	struct rtattr  **rta = arg;
	struct ifaddrmsg *ifm = NLMSG_DATA(nlh);
	struct in6_addr *pfx;

	pfx = NULL;
	if (rta[IFA_ADDRESS-1]) {
		if (RTA_PAYLOAD(rta[IFA_ADDRESS-1]) < sizeof(*pfx))
			return -EINVAL;
		pfx = RTA_DATA(rta[IFA_ADDRESS-1]);
	}
	if (rta[IFA_LOCAL-1]) {
		if (pfx && memcmp(pfx, RTA_DATA(rta[IFA_LOCAL-1]), sizeof(*pfx)))
			return -EINVAL;
		pfx = RTA_DATA(rta[IFA_LOCAL-1]);
	}
	if (pfx == NULL)
		return -EINVAL;

	return inet6_addr_add(ifm->ifa_index, pfx, ifm->ifa_prefixlen);
}

static int inet6_fill_ifaddr(struct sk_buff *skb, struct inet6_ifaddr *ifa,
			     u32 pid, u32 seq, int event)
{
	struct ifaddrmsg *ifm;
	struct nlmsghdr  *nlh;
	struct ifa_cacheinfo ci;
	unsigned char	 *b = skb->tail;

	nlh = NLMSG_PUT(skb, pid, seq, event, sizeof(*ifm));
	ifm = NLMSG_DATA(nlh);
	ifm->ifa_family = AF_INET6;
	ifm->ifa_prefixlen = ifa->prefix_len;
	ifm->ifa_flags = ifa->flags;
	ifm->ifa_scope = RT_SCOPE_UNIVERSE;
	if (ifa->scope&IFA_HOST)
		ifm->ifa_scope = RT_SCOPE_HOST;
	else if (ifa->scope&IFA_LINK)
		ifm->ifa_scope = RT_SCOPE_LINK;
	else if (ifa->scope&IFA_SITE)
		ifm->ifa_scope = RT_SCOPE_SITE;
	ifm->ifa_index = ifa->idev->dev->ifindex;
	RTA_PUT(skb, IFA_ADDRESS, 16, &ifa->addr);
	if (!(ifa->flags&IFA_F_PERMANENT)) {
		ci.ifa_prefered = ifa->prefered_lft;
		ci.ifa_valid = ifa->valid_lft;
		if (ci.ifa_prefered != 0xFFFFFFFF) {
			long tval = (jiffies - ifa->tstamp)/HZ;
			ci.ifa_prefered -= tval;
			if (ci.ifa_valid != 0xFFFFFFFF)
				ci.ifa_valid -= tval;
		}
		RTA_PUT(skb, IFA_CACHEINFO, sizeof(ci), &ci);
	}
	nlh->nlmsg_len = skb->tail - b;
	return skb->len;

nlmsg_failure:
rtattr_failure:
	skb_trim(skb, b - skb->data);
	return -1;
}

static int inet6_dump_ifaddr(struct sk_buff *skb, struct netlink_callback *cb)
{
	int idx, ip_idx;
	int s_idx, s_ip_idx;
 	struct inet6_ifaddr *ifa;

	s_idx = cb->args[0];
	s_ip_idx = ip_idx = cb->args[1];

	for (idx=0; idx < IN6_ADDR_HSIZE; idx++) {
		if (idx < s_idx)
			continue;
		if (idx > s_idx)
			s_ip_idx = 0;
		read_lock_bh(&addrconf_hash_lock);
		for (ifa=inet6_addr_lst[idx], ip_idx = 0; ifa;
		     ifa = ifa->lst_next, ip_idx++) {
			if (ip_idx < s_ip_idx)
				continue;
			if (inet6_fill_ifaddr(skb, ifa, NETLINK_CB(cb->skb).pid,
					      cb->nlh->nlmsg_seq, RTM_NEWADDR) <= 0) {
				read_unlock_bh(&addrconf_hash_lock);
				goto done;
			}
		}
		read_unlock_bh(&addrconf_hash_lock);
	}
done:
	cb->args[0] = idx;
	cb->args[1] = ip_idx;

	return skb->len;
}

static void inet6_ifa_notify(int event, struct inet6_ifaddr *ifa)
{
	struct sk_buff *skb;
	int size = NLMSG_SPACE(sizeof(struct ifaddrmsg)+128);

	skb = alloc_skb(size, GFP_ATOMIC);
	if (!skb) {
		netlink_set_err(rtnl, 0, RTMGRP_IPV6_IFADDR, ENOBUFS);
		return;
	}
	if (inet6_fill_ifaddr(skb, ifa, 0, 0, event) < 0) {
		kfree_skb(skb);
		netlink_set_err(rtnl, 0, RTMGRP_IPV6_IFADDR, EINVAL);
		return;
	}
	NETLINK_CB(skb).dst_groups = RTMGRP_IPV6_IFADDR;
	netlink_broadcast(rtnl, skb, 0, RTMGRP_IPV6_IFADDR, GFP_ATOMIC);
}

static struct rtnetlink_link inet6_rtnetlink_table[RTM_MAX-RTM_BASE+1] =
{
	{ NULL,			NULL,			},
	{ NULL,			NULL,			},
	{ NULL,			NULL,			},
	{ NULL,			NULL,			},

	{ inet6_rtm_newaddr,	NULL,			},
	{ inet6_rtm_deladdr,	NULL,			},
	{ NULL,			inet6_dump_ifaddr,	},
	{ NULL,			NULL,			},

	{ inet6_rtm_newroute,	NULL,			},
	{ inet6_rtm_delroute,	NULL,			},
	{ inet6_rtm_getroute,	inet6_dump_fib,		},
	{ NULL,			NULL,			},
};
#endif

static void ipv6_ifa_notify(int event, struct inet6_ifaddr *ifp)
{
#ifdef CONFIG_RTNETLINK
	inet6_ifa_notify(event ? : RTM_NEWADDR, ifp);
#endif
	switch (event) {
	case RTM_NEWADDR:
		ip6_rt_addr_add(&ifp->addr, ifp->idev->dev);
		break;
	case RTM_DELADDR:
		addrconf_leave_solict(ifp->idev->dev, &ifp->addr);
		if (!ipv6_chk_addr(&ifp->addr, ifp->idev->dev))
			ip6_rt_addr_del(&ifp->addr, ifp->idev->dev);
		break;
	}
}

#ifdef CONFIG_SYSCTL

static
int addrconf_sysctl_forward(ctl_table *ctl, int write, struct file * filp,
			   void *buffer, size_t *lenp)
{
	int *valp = ctl->data;
	int val = *valp;
	int ret;

	ret = proc_dointvec(ctl, write, filp, buffer, lenp);

	if (write && *valp != val && valp != &ipv6_devconf_dflt.forwarding) {
		struct inet6_dev *idev = NULL;

		if (valp != &ipv6_devconf.forwarding) {
			struct net_device *dev = dev_get_by_index(ctl->ctl_name);
			if (dev) {
				idev = in6_dev_get(dev);
				dev_put(dev);
			}
			if (idev == NULL)
				return ret;
		} else
			ipv6_devconf_dflt.forwarding = ipv6_devconf.forwarding;

		addrconf_forward_change(idev);

		if (*valp)
			rt6_purge_dflt_routers(0);
		if (idev)
			in6_dev_put(idev);
	}

        return ret;
}

static struct addrconf_sysctl_table
{
	struct ctl_table_header *sysctl_header;
	ctl_table addrconf_vars[11];
	ctl_table addrconf_dev[2];
	ctl_table addrconf_conf_dir[2];
	ctl_table addrconf_proto_dir[2];
	ctl_table addrconf_root_dir[2];
} addrconf_sysctl = {
	NULL,
        {{NET_IPV6_FORWARDING, "forwarding",
         &ipv6_devconf.forwarding, sizeof(int), 0644, NULL,
         &addrconf_sysctl_forward},

	{NET_IPV6_HOP_LIMIT, "hop_limit",
         &ipv6_devconf.hop_limit, sizeof(int), 0644, NULL,
         &proc_dointvec},

	{NET_IPV6_MTU, "mtu",
         &ipv6_devconf.mtu6, sizeof(int), 0644, NULL,
         &proc_dointvec},

	{NET_IPV6_ACCEPT_RA, "accept_ra",
         &ipv6_devconf.accept_ra, sizeof(int), 0644, NULL,
         &proc_dointvec},

	{NET_IPV6_ACCEPT_REDIRECTS, "accept_redirects",
         &ipv6_devconf.accept_redirects, sizeof(int), 0644, NULL,
         &proc_dointvec},

	{NET_IPV6_AUTOCONF, "autoconf",
         &ipv6_devconf.autoconf, sizeof(int), 0644, NULL,
         &proc_dointvec},

	{NET_IPV6_DAD_TRANSMITS, "dad_transmits",
         &ipv6_devconf.dad_transmits, sizeof(int), 0644, NULL,
         &proc_dointvec},

	{NET_IPV6_RTR_SOLICITS, "router_solicitations",
         &ipv6_devconf.rtr_solicits, sizeof(int), 0644, NULL,
         &proc_dointvec},

	{NET_IPV6_RTR_SOLICIT_INTERVAL, "router_solicitation_interval",
         &ipv6_devconf.rtr_solicit_interval, sizeof(int), 0644, NULL,
         &proc_dointvec_jiffies},

	{NET_IPV6_RTR_SOLICIT_DELAY, "router_solicitation_delay",
         &ipv6_devconf.rtr_solicit_delay, sizeof(int), 0644, NULL,
         &proc_dointvec_jiffies},

	{0}},

	{{NET_PROTO_CONF_ALL, "all", NULL, 0, 0555, addrconf_sysctl.addrconf_vars},{0}},
	{{NET_IPV6_CONF, "conf", NULL, 0, 0555, addrconf_sysctl.addrconf_dev},{0}},
	{{NET_IPV6, "ipv6", NULL, 0, 0555, addrconf_sysctl.addrconf_conf_dir},{0}},
	{{CTL_NET, "net", NULL, 0, 0555, addrconf_sysctl.addrconf_proto_dir},{0}}
};

static void addrconf_sysctl_register(struct inet6_dev *idev, struct ipv6_devconf *p)
{
	int i;
	struct net_device *dev = idev ? idev->dev : NULL;
	struct addrconf_sysctl_table *t;

	t = kmalloc(sizeof(*t), GFP_KERNEL);
	if (t == NULL)
		return;
	memcpy(t, &addrconf_sysctl, sizeof(*t));
	for (i=0; i<sizeof(t->addrconf_vars)/sizeof(t->addrconf_vars[0])-1; i++) {
		t->addrconf_vars[i].data += (char*)p - (char*)&ipv6_devconf;
		t->addrconf_vars[i].de = NULL;
	}
	if (dev) {
		t->addrconf_dev[0].procname = dev->name;
		t->addrconf_dev[0].ctl_name = dev->ifindex;
	} else {
		t->addrconf_dev[0].procname = "default";
		t->addrconf_dev[0].ctl_name = NET_PROTO_CONF_DEFAULT;
	}
	t->addrconf_dev[0].child = t->addrconf_vars;
	t->addrconf_dev[0].de = NULL;
	t->addrconf_conf_dir[0].child = t->addrconf_dev;
	t->addrconf_conf_dir[0].de = NULL;
	t->addrconf_proto_dir[0].child = t->addrconf_conf_dir;
	t->addrconf_proto_dir[0].de = NULL;
	t->addrconf_root_dir[0].child = t->addrconf_proto_dir;
	t->addrconf_root_dir[0].de = NULL;

	t->sysctl_header = register_sysctl_table(t->addrconf_root_dir, 0);
	if (t->sysctl_header == NULL)
		kfree(t);
	else
		p->sysctl = t;
}

static void addrconf_sysctl_unregister(struct ipv6_devconf *p)
{
	if (p->sysctl) {
		struct addrconf_sysctl_table *t = p->sysctl;
		p->sysctl = NULL;
		unregister_sysctl_table(t->sysctl_header);
		kfree(t);
	}
}


#endif

/*
 *	Init / cleanup code
 */

void __init addrconf_init(void)
{
#ifdef MODULE
	struct net_device *dev;

	/* This takes sense only during module load. */
	rtnl_lock();
	for (dev = dev_base; dev; dev = dev->next) {
		if (!(dev->flags&IFF_UP))
			continue;

		switch (dev->type) {
		case ARPHRD_LOOPBACK:	
			init_loopback(dev);
			break;
		case ARPHRD_ETHER:
		case ARPHRD_FDDI:
		case ARPHRD_IEEE802_TR:	
			addrconf_dev_config(dev);
			break;
		default:;
			/* Ignore all other */
		}
	}
	rtnl_unlock();
#endif

#ifdef CONFIG_PROC_FS
	proc_net_create("if_inet6", 0, iface_proc_info);
#endif
	
	addr_chk_timer.expires = jiffies + ADDR_CHECK_FREQUENCY;
	add_timer(&addr_chk_timer);
#ifdef CONFIG_RTNETLINK
	rtnetlink_links[PF_INET6] = inet6_rtnetlink_table;
#endif
#ifdef CONFIG_SYSCTL
	addrconf_sysctl.sysctl_header =
		register_sysctl_table(addrconf_sysctl.addrconf_root_dir, 0);
	addrconf_sysctl_register(NULL, &ipv6_devconf_dflt);
#endif
}

#ifdef MODULE
void addrconf_cleanup(void)
{
 	struct net_device *dev;
 	struct inet6_dev *idev;
 	struct inet6_ifaddr *ifa;
	int i;

#ifdef CONFIG_RTNETLINK
	rtnetlink_links[PF_INET6] = NULL;
#endif
#ifdef CONFIG_SYSCTL
	addrconf_sysctl_unregister(&ipv6_devconf_dflt);
	addrconf_sysctl_unregister(&ipv6_devconf);
#endif

	rtnl_lock();

	/*
	 *	clean dev list.
	 */

	for (dev=dev_base; dev; dev=dev->next) {
		if ((idev = __in6_dev_get(dev)) == NULL)
			continue;
		addrconf_ifdown(dev, 1);
	}

	/*
	 *	Check hash table.
	 */

	write_lock_bh(&addrconf_hash_lock);
	for (i=0; i < IN6_ADDR_HSIZE; i++) {
		for (ifa=inet6_addr_lst[i]; ifa; ) {
			struct inet6_ifaddr *bifa;

			bifa = ifa;
			ifa = ifa->lst_next;
			printk(KERN_DEBUG "bug: IPv6 address leakage detected: ifa=%p\n", bifa);
			/* Do not free it; something is wrong.
			   Now we can investigate it with debugger.
			 */
		}
	}
	write_unlock_bh(&addrconf_hash_lock);

	del_timer(&addr_chk_timer);

	rtnl_unlock();

#ifdef CONFIG_PROC_FS
	proc_net_remove("if_inet6");
#endif
}
#endif	/* MODULE */
