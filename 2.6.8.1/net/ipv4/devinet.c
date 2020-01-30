/*
 *	NET3	IP device support routines.
 *
 *	Version: $Id: devinet.c,v 1.44 2001/10/31 21:55:54 davem Exp $
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *	Derived from the IP parts of dev.c 1.0.19
 * 		Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *				Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *				Mark Evans, <evansmp@uhura.aston.ac.uk>
 *
 *	Additional Authors:
 *		Alan Cox, <gw4pts@gw4pts.ampr.org>
 *		Alexey Kuznetsov, <kuznet@ms2.inr.ac.ru>
 *
 *	Changes:
 *		Alexey Kuznetsov:	pa_* fields are replaced with ifaddr
 *					lists.
 *		Cyrus Durgin:		updated for kmod
 *		Matthias Andree:	in devinet_ioctl, compare label and
 *					address (4.4BSD alias style support),
 *					fall back to comparing just the label
 *					if no match found.
 */

#include <linux/config.h>

#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/bitops.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/socket.h>
#include <linux/sockios.h>
#include <linux/in.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/if_ether.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/rtnetlink.h>
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/inetdevice.h>
#include <linux/igmp.h>
#ifdef CONFIG_SYSCTL
#include <linux/sysctl.h>
#endif
#include <linux/kmod.h>

#include <net/ip.h>
#include <net/route.h>
#include <net/ip_fib.h>

struct ipv4_devconf ipv4_devconf = {
	.accept_redirects = 1,
	.send_redirects =  1,
	.secure_redirects = 1,
	.shared_media =	  1,
};

static struct ipv4_devconf ipv4_devconf_dflt = {
	.accept_redirects =  1,
	.send_redirects =    1,
	.secure_redirects =  1,
	.shared_media =	     1,
	.accept_source_route = 1,
};

static void rtmsg_ifa(int event, struct in_ifaddr *);

static struct notifier_block *inetaddr_chain;
static void inet_del_ifa(struct in_device *in_dev, struct in_ifaddr **ifap,
			 int destroy);
#ifdef CONFIG_SYSCTL
static void devinet_sysctl_register(struct in_device *in_dev,
				    struct ipv4_devconf *p);
static void devinet_sysctl_unregister(struct ipv4_devconf *p);
#endif

int inet_ifa_count;
int inet_dev_count;

/* Locks all the inet devices. */

rwlock_t inetdev_lock = RW_LOCK_UNLOCKED;

static struct in_ifaddr *inet_alloc_ifa(void)
{
	struct in_ifaddr *ifa = kmalloc(sizeof(*ifa), GFP_KERNEL);

	if (ifa) {
		memset(ifa, 0, sizeof(*ifa));
		inet_ifa_count++;
	}

	return ifa;
}

static __inline__ void inet_free_ifa(struct in_ifaddr *ifa)
{
	if (ifa->ifa_dev)
		__in_dev_put(ifa->ifa_dev);
	kfree(ifa);
	inet_ifa_count--;
}

void in_dev_finish_destroy(struct in_device *idev)
{
	struct net_device *dev = idev->dev;

	BUG_TRAP(!idev->ifa_list);
	BUG_TRAP(!idev->mc_list);
#ifdef NET_REFCNT_DEBUG
	printk(KERN_DEBUG "in_dev_finish_destroy: %p=%s\n",
	       idev, dev ? dev->name : "NIL");
#endif
	dev_put(dev);
	if (!idev->dead)
		printk("Freeing alive in_device %p\n", idev);
	else {
		inet_dev_count--;
		kfree(idev);
	}
}

struct in_device *inetdev_init(struct net_device *dev)
{
	struct in_device *in_dev;

	ASSERT_RTNL();

	in_dev = kmalloc(sizeof(*in_dev), GFP_KERNEL);
	if (!in_dev)
		goto out;
	memset(in_dev, 0, sizeof(*in_dev));
	in_dev->lock = RW_LOCK_UNLOCKED;
	memcpy(&in_dev->cnf, &ipv4_devconf_dflt, sizeof(in_dev->cnf));
	in_dev->cnf.sysctl = NULL;
	in_dev->dev = dev;
	if ((in_dev->arp_parms = neigh_parms_alloc(dev, &arp_tbl)) == NULL)
		goto out_kfree;
	inet_dev_count++;
	/* Reference in_dev->dev */
	dev_hold(dev);
#ifdef CONFIG_SYSCTL
	neigh_sysctl_register(dev, in_dev->arp_parms, NET_IPV4,
			      NET_IPV4_NEIGH, "ipv4", NULL);
#endif
	write_lock_bh(&inetdev_lock);
	dev->ip_ptr = in_dev;
	/* Account for reference dev->ip_ptr */
	in_dev_hold(in_dev);
	write_unlock_bh(&inetdev_lock);
#ifdef CONFIG_SYSCTL
	devinet_sysctl_register(in_dev, &in_dev->cnf);
#endif
	ip_mc_init_dev(in_dev);
	if (dev->flags & IFF_UP)
		ip_mc_up(in_dev);
out:
	return in_dev;
out_kfree:
	kfree(in_dev);
	in_dev = NULL;
	goto out;
}

static void inetdev_destroy(struct in_device *in_dev)
{
	struct in_ifaddr *ifa;

	ASSERT_RTNL();

	in_dev->dead = 1;

	ip_mc_destroy_dev(in_dev);

	while ((ifa = in_dev->ifa_list) != NULL) {
		inet_del_ifa(in_dev, &in_dev->ifa_list, 0);
		inet_free_ifa(ifa);
	}

#ifdef CONFIG_SYSCTL
	devinet_sysctl_unregister(&in_dev->cnf);
#endif
	write_lock_bh(&inetdev_lock);
	in_dev->dev->ip_ptr = NULL;
	/* in_dev_put following below will kill the in_device */
	write_unlock_bh(&inetdev_lock);

#ifdef CONFIG_SYSCTL
	neigh_sysctl_unregister(in_dev->arp_parms);
#endif
	neigh_parms_release(&arp_tbl, in_dev->arp_parms);
	in_dev_put(in_dev);
}

int inet_addr_onlink(struct in_device *in_dev, u32 a, u32 b)
{
	read_lock(&in_dev->lock);
	for_primary_ifa(in_dev) {
		if (inet_ifa_match(a, ifa)) {
			if (!b || inet_ifa_match(b, ifa)) {
				read_unlock(&in_dev->lock);
				return 1;
			}
		}
	} endfor_ifa(in_dev);
	read_unlock(&in_dev->lock);
	return 0;
}

static void inet_del_ifa(struct in_device *in_dev, struct in_ifaddr **ifap,
			 int destroy)
{
	struct in_ifaddr *ifa1 = *ifap;

	ASSERT_RTNL();

	/* 1. Deleting primary ifaddr forces deletion all secondaries */

	if (!(ifa1->ifa_flags & IFA_F_SECONDARY)) {
		struct in_ifaddr *ifa;
		struct in_ifaddr **ifap1 = &ifa1->ifa_next;

		while ((ifa = *ifap1) != NULL) {
			if (!(ifa->ifa_flags & IFA_F_SECONDARY) ||
			    ifa1->ifa_mask != ifa->ifa_mask ||
			    !inet_ifa_match(ifa1->ifa_address, ifa)) {
				ifap1 = &ifa->ifa_next;
				continue;
			}
			write_lock_bh(&in_dev->lock);
			*ifap1 = ifa->ifa_next;
			write_unlock_bh(&in_dev->lock);

			rtmsg_ifa(RTM_DELADDR, ifa);
			notifier_call_chain(&inetaddr_chain, NETDEV_DOWN, ifa);
			inet_free_ifa(ifa);
		}
	}

	/* 2. Unlink it */

	write_lock_bh(&in_dev->lock);
	*ifap = ifa1->ifa_next;
	write_unlock_bh(&in_dev->lock);

	/* 3. Announce address deletion */

	/* Send message first, then call notifier.
	   At first sight, FIB update triggered by notifier
	   will refer to already deleted ifaddr, that could confuse
	   netlink listeners. It is not true: look, gated sees
	   that route deleted and if it still thinks that ifaddr
	   is valid, it will try to restore deleted routes... Grr.
	   So that, this order is correct.
	 */
	rtmsg_ifa(RTM_DELADDR, ifa1);
	notifier_call_chain(&inetaddr_chain, NETDEV_DOWN, ifa1);
	if (destroy) {
		inet_free_ifa(ifa1);

		if (!in_dev->ifa_list)
			inetdev_destroy(in_dev);
	}
}

static int inet_insert_ifa(struct in_ifaddr *ifa)
{
	struct in_device *in_dev = ifa->ifa_dev;
	struct in_ifaddr *ifa1, **ifap, **last_primary;

	ASSERT_RTNL();

	if (!ifa->ifa_local) {
		inet_free_ifa(ifa);
		return 0;
	}

	ifa->ifa_flags &= ~IFA_F_SECONDARY;
	last_primary = &in_dev->ifa_list;

	for (ifap = &in_dev->ifa_list; (ifa1 = *ifap) != NULL;
	     ifap = &ifa1->ifa_next) {
		if (!(ifa1->ifa_flags & IFA_F_SECONDARY) &&
		    ifa->ifa_scope <= ifa1->ifa_scope)
			last_primary = &ifa1->ifa_next;
		if (ifa1->ifa_mask == ifa->ifa_mask &&
		    inet_ifa_match(ifa1->ifa_address, ifa)) {
			if (ifa1->ifa_local == ifa->ifa_local) {
				inet_free_ifa(ifa);
				return -EEXIST;
			}
			if (ifa1->ifa_scope != ifa->ifa_scope) {
				inet_free_ifa(ifa);
				return -EINVAL;
			}
			ifa->ifa_flags |= IFA_F_SECONDARY;
		}
	}

	if (!(ifa->ifa_flags & IFA_F_SECONDARY)) {
		net_srandom(ifa->ifa_local);
		ifap = last_primary;
	}

	ifa->ifa_next = *ifap;
	write_lock_bh(&in_dev->lock);
	*ifap = ifa;
	write_unlock_bh(&in_dev->lock);

	/* Send message first, then call notifier.
	   Notifier will trigger FIB update, so that
	   listeners of netlink will know about new ifaddr */
	rtmsg_ifa(RTM_NEWADDR, ifa);
	notifier_call_chain(&inetaddr_chain, NETDEV_UP, ifa);

	return 0;
}

static int inet_set_ifa(struct net_device *dev, struct in_ifaddr *ifa)
{
	struct in_device *in_dev = __in_dev_get(dev);

	ASSERT_RTNL();

	if (!in_dev) {
		in_dev = inetdev_init(dev);
		if (!in_dev) {
			inet_free_ifa(ifa);
			return -ENOBUFS;
		}
	}
	if (ifa->ifa_dev != in_dev) {
		BUG_TRAP(!ifa->ifa_dev);
		in_dev_hold(in_dev);
		ifa->ifa_dev = in_dev;
	}
	if (LOOPBACK(ifa->ifa_local))
		ifa->ifa_scope = RT_SCOPE_HOST;
	return inet_insert_ifa(ifa);
}

struct in_device *inetdev_by_index(int ifindex)
{
	struct net_device *dev;
	struct in_device *in_dev = NULL;
	read_lock(&dev_base_lock);
	dev = __dev_get_by_index(ifindex);
	if (dev)
		in_dev = in_dev_get(dev);
	read_unlock(&dev_base_lock);
	return in_dev;
}

/* Called only from RTNL semaphored context. No locks. */

struct in_ifaddr *inet_ifa_byprefix(struct in_device *in_dev, u32 prefix,
				    u32 mask)
{
	ASSERT_RTNL();

	for_primary_ifa(in_dev) {
		if (ifa->ifa_mask == mask && inet_ifa_match(prefix, ifa))
			return ifa;
	} endfor_ifa(in_dev);
	return NULL;
}

int inet_rtm_deladdr(struct sk_buff *skb, struct nlmsghdr *nlh, void *arg)
{
	struct rtattr **rta = arg;
	struct in_device *in_dev;
	struct ifaddrmsg *ifm = NLMSG_DATA(nlh);
	struct in_ifaddr *ifa, **ifap;

	ASSERT_RTNL();

	if ((in_dev = inetdev_by_index(ifm->ifa_index)) == NULL)
		goto out;
	__in_dev_put(in_dev);

	for (ifap = &in_dev->ifa_list; (ifa = *ifap) != NULL;
	     ifap = &ifa->ifa_next) {
		if ((rta[IFA_LOCAL - 1] &&
		     memcmp(RTA_DATA(rta[IFA_LOCAL - 1]),
			    &ifa->ifa_local, 4)) ||
		    (rta[IFA_LABEL - 1] &&
		     strcmp(RTA_DATA(rta[IFA_LABEL - 1]), ifa->ifa_label)) ||
		    (rta[IFA_ADDRESS - 1] &&
		     (ifm->ifa_prefixlen != ifa->ifa_prefixlen ||
		      !inet_ifa_match(*(u32*)RTA_DATA(rta[IFA_ADDRESS - 1]),
			      	      ifa))))
			continue;
		inet_del_ifa(in_dev, ifap, 1);
		return 0;
	}
out:
	return -EADDRNOTAVAIL;
}

int inet_rtm_newaddr(struct sk_buff *skb, struct nlmsghdr *nlh, void *arg)
{
	struct rtattr **rta = arg;
	struct net_device *dev;
	struct in_device *in_dev;
	struct ifaddrmsg *ifm = NLMSG_DATA(nlh);
	struct in_ifaddr *ifa;
	int rc = -EINVAL;

	ASSERT_RTNL();

	if (ifm->ifa_prefixlen > 32 || !rta[IFA_LOCAL - 1])
		goto out;

	rc = -ENODEV;
	if ((dev = __dev_get_by_index(ifm->ifa_index)) == NULL)
		goto out;

	rc = -ENOBUFS;
	if ((in_dev = __in_dev_get(dev)) == NULL) {
		in_dev = inetdev_init(dev);
		if (!in_dev)
			goto out;
	}

	if ((ifa = inet_alloc_ifa()) == NULL)
		goto out;

	if (!rta[IFA_ADDRESS - 1])
		rta[IFA_ADDRESS - 1] = rta[IFA_LOCAL - 1];
	memcpy(&ifa->ifa_local, RTA_DATA(rta[IFA_LOCAL - 1]), 4);
	memcpy(&ifa->ifa_address, RTA_DATA(rta[IFA_ADDRESS - 1]), 4);
	ifa->ifa_prefixlen = ifm->ifa_prefixlen;
	ifa->ifa_mask = inet_make_mask(ifm->ifa_prefixlen);
	if (rta[IFA_BROADCAST - 1])
		memcpy(&ifa->ifa_broadcast,
		       RTA_DATA(rta[IFA_BROADCAST - 1]), 4);
	if (rta[IFA_ANYCAST - 1])
		memcpy(&ifa->ifa_anycast, RTA_DATA(rta[IFA_ANYCAST - 1]), 4);
	ifa->ifa_flags = ifm->ifa_flags;
	ifa->ifa_scope = ifm->ifa_scope;
	in_dev_hold(in_dev);
	ifa->ifa_dev   = in_dev;
	if (rta[IFA_LABEL - 1])
		memcpy(ifa->ifa_label, RTA_DATA(rta[IFA_LABEL - 1]), IFNAMSIZ);
	else
		memcpy(ifa->ifa_label, dev->name, IFNAMSIZ);

	rc = inet_insert_ifa(ifa);
out:
	return rc;
}

/*
 *	Determine a default network mask, based on the IP address.
 */

static __inline__ int inet_abc_len(u32 addr)
{
	int rc = -1;	/* Something else, probably a multicast. */

  	if (ZERONET(addr))
  		rc = 0;
	else {
		addr = ntohl(addr);

		if (IN_CLASSA(addr))
			rc = 8;
		else if (IN_CLASSB(addr))
			rc = 16;
		else if (IN_CLASSC(addr))
			rc = 24;
	}

  	return rc;
}


int devinet_ioctl(unsigned int cmd, void __user *arg)
{
	struct ifreq ifr;
	struct sockaddr_in sin_orig;
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
	struct in_device *in_dev;
	struct in_ifaddr **ifap = NULL;
	struct in_ifaddr *ifa = NULL;
	struct net_device *dev;
	char *colon;
	int ret = -EFAULT;
	int tryaddrmatch = 0;

	/*
	 *	Fetch the caller's info block into kernel space
	 */

	if (copy_from_user(&ifr, arg, sizeof(struct ifreq)))
		goto out;
	ifr.ifr_name[IFNAMSIZ - 1] = 0;

	/* save original address for comparison */
	memcpy(&sin_orig, sin, sizeof(*sin));

	colon = strchr(ifr.ifr_name, ':');
	if (colon)
		*colon = 0;

#ifdef CONFIG_KMOD
	dev_load(ifr.ifr_name);
#endif

	switch(cmd) {
	case SIOCGIFADDR:	/* Get interface address */
	case SIOCGIFBRDADDR:	/* Get the broadcast address */
	case SIOCGIFDSTADDR:	/* Get the destination address */
	case SIOCGIFNETMASK:	/* Get the netmask for the interface */
		/* Note that these ioctls will not sleep,
		   so that we do not impose a lock.
		   One day we will be forced to put shlock here (I mean SMP)
		 */
		tryaddrmatch = (sin_orig.sin_family == AF_INET);
		memset(sin, 0, sizeof(*sin));
		sin->sin_family = AF_INET;
		break;

	case SIOCSIFFLAGS:
		ret = -EACCES;
		if (!capable(CAP_NET_ADMIN))
			goto out;
		break;
	case SIOCSIFADDR:	/* Set interface address (and family) */
	case SIOCSIFBRDADDR:	/* Set the broadcast address */
	case SIOCSIFDSTADDR:	/* Set the destination address */
	case SIOCSIFNETMASK: 	/* Set the netmask for the interface */
		ret = -EACCES;
		if (!capable(CAP_NET_ADMIN))
			goto out;
		ret = -EINVAL;
		if (sin->sin_family != AF_INET)
			goto out;
		break;
	default:
		ret = -EINVAL;
		goto out;
	}

	rtnl_lock();

	ret = -ENODEV;
	if ((dev = __dev_get_by_name(ifr.ifr_name)) == NULL)
		goto done;

	if (colon)
		*colon = ':';

	if ((in_dev = __in_dev_get(dev)) != NULL) {
		if (tryaddrmatch) {
			/* Matthias Andree */
			/* compare label and address (4.4BSD style) */
			/* note: we only do this for a limited set of ioctls
			   and only if the original address family was AF_INET.
			   This is checked above. */
			for (ifap = &in_dev->ifa_list; (ifa = *ifap) != NULL;
			     ifap = &ifa->ifa_next) {
				if (!strcmp(ifr.ifr_name, ifa->ifa_label) &&
				    sin_orig.sin_addr.s_addr ==
							ifa->ifa_address) {
					break; /* found */
				}
			}
		}
		/* we didn't get a match, maybe the application is
		   4.3BSD-style and passed in junk so we fall back to
		   comparing just the label */
		if (!ifa) {
			for (ifap = &in_dev->ifa_list; (ifa = *ifap) != NULL;
			     ifap = &ifa->ifa_next)
				if (!strcmp(ifr.ifr_name, ifa->ifa_label))
					break;
		}
	}

	ret = -EADDRNOTAVAIL;
	if (!ifa && cmd != SIOCSIFADDR && cmd != SIOCSIFFLAGS)
		goto done;

	switch(cmd) {
	case SIOCGIFADDR:	/* Get interface address */
		sin->sin_addr.s_addr = ifa->ifa_local;
		goto rarok;

	case SIOCGIFBRDADDR:	/* Get the broadcast address */
		sin->sin_addr.s_addr = ifa->ifa_broadcast;
		goto rarok;

	case SIOCGIFDSTADDR:	/* Get the destination address */
		sin->sin_addr.s_addr = ifa->ifa_address;
		goto rarok;

	case SIOCGIFNETMASK:	/* Get the netmask for the interface */
		sin->sin_addr.s_addr = ifa->ifa_mask;
		goto rarok;

	case SIOCSIFFLAGS:
		if (colon) {
			ret = -EADDRNOTAVAIL;
			if (!ifa)
				break;
			ret = 0;
			if (!(ifr.ifr_flags & IFF_UP))
				inet_del_ifa(in_dev, ifap, 1);
			break;
		}
		ret = dev_change_flags(dev, ifr.ifr_flags);
		break;

	case SIOCSIFADDR:	/* Set interface address (and family) */
		ret = -EINVAL;
		if (inet_abc_len(sin->sin_addr.s_addr) < 0)
			break;

		if (!ifa) {
			ret = -ENOBUFS;
			if ((ifa = inet_alloc_ifa()) == NULL)
				break;
			if (colon)
				memcpy(ifa->ifa_label, ifr.ifr_name, IFNAMSIZ);
			else
				memcpy(ifa->ifa_label, dev->name, IFNAMSIZ);
		} else {
			ret = 0;
			if (ifa->ifa_local == sin->sin_addr.s_addr)
				break;
			inet_del_ifa(in_dev, ifap, 0);
			ifa->ifa_broadcast = 0;
			ifa->ifa_anycast = 0;
		}

		ifa->ifa_address = ifa->ifa_local = sin->sin_addr.s_addr;

		if (!(dev->flags & IFF_POINTOPOINT)) {
			ifa->ifa_prefixlen = inet_abc_len(ifa->ifa_address);
			ifa->ifa_mask = inet_make_mask(ifa->ifa_prefixlen);
			if ((dev->flags & IFF_BROADCAST) &&
			    ifa->ifa_prefixlen < 31)
				ifa->ifa_broadcast = ifa->ifa_address |
						     ~ifa->ifa_mask;
		} else {
			ifa->ifa_prefixlen = 32;
			ifa->ifa_mask = inet_make_mask(32);
		}
		ret = inet_set_ifa(dev, ifa);
		break;

	case SIOCSIFBRDADDR:	/* Set the broadcast address */
		ret = 0;
		if (ifa->ifa_broadcast != sin->sin_addr.s_addr) {
			inet_del_ifa(in_dev, ifap, 0);
			ifa->ifa_broadcast = sin->sin_addr.s_addr;
			inet_insert_ifa(ifa);
		}
		break;

	case SIOCSIFDSTADDR:	/* Set the destination address */
		ret = 0;
		if (ifa->ifa_address == sin->sin_addr.s_addr)
			break;
		ret = -EINVAL;
		if (inet_abc_len(sin->sin_addr.s_addr) < 0)
			break;
		ret = 0;
		inet_del_ifa(in_dev, ifap, 0);
		ifa->ifa_address = sin->sin_addr.s_addr;
		inet_insert_ifa(ifa);
		break;

	case SIOCSIFNETMASK: 	/* Set the netmask for the interface */

		/*
		 *	The mask we set must be legal.
		 */
		ret = -EINVAL;
		if (bad_mask(sin->sin_addr.s_addr, 0))
			break;
		ret = 0;
		if (ifa->ifa_mask != sin->sin_addr.s_addr) {
			inet_del_ifa(in_dev, ifap, 0);
			ifa->ifa_mask = sin->sin_addr.s_addr;
			ifa->ifa_prefixlen = inet_mask_len(ifa->ifa_mask);

			/* See if current broadcast address matches
			 * with current netmask, then recalculate
			 * the broadcast address. Otherwise it's a
			 * funny address, so don't touch it since
			 * the user seems to know what (s)he's doing...
			 */
			if ((dev->flags & IFF_BROADCAST) &&
			    (ifa->ifa_prefixlen < 31) &&
			    (ifa->ifa_broadcast ==
			     (ifa->ifa_local|~ifa->ifa_mask))) {
				ifa->ifa_broadcast = (ifa->ifa_local |
						      ~sin->sin_addr.s_addr);
			}
			inet_insert_ifa(ifa);
		}
		break;
	}
done:
	rtnl_unlock();
out:
	return ret;
rarok:
	rtnl_unlock();
	ret = copy_to_user(arg, &ifr, sizeof(struct ifreq)) ? -EFAULT : 0;
	goto out;
}

static int inet_gifconf(struct net_device *dev, char __user *buf, int len)
{
	struct in_device *in_dev = __in_dev_get(dev);
	struct in_ifaddr *ifa;
	struct ifreq ifr;
	int done = 0;

	if (!in_dev || (ifa = in_dev->ifa_list) == NULL)
		goto out;

	for (; ifa; ifa = ifa->ifa_next) {
		if (!buf) {
			done += sizeof(ifr);
			continue;
		}
		if (len < (int) sizeof(ifr))
			break;
		memset(&ifr, 0, sizeof(struct ifreq));
		if (ifa->ifa_label)
			strcpy(ifr.ifr_name, ifa->ifa_label);
		else
			strcpy(ifr.ifr_name, dev->name);

		(*(struct sockaddr_in *)&ifr.ifr_addr).sin_family = AF_INET;
		(*(struct sockaddr_in *)&ifr.ifr_addr).sin_addr.s_addr =
								ifa->ifa_local;

		if (copy_to_user(buf, &ifr, sizeof(struct ifreq))) {
			done = -EFAULT;
			break;
		}
		buf  += sizeof(struct ifreq);
		len  -= sizeof(struct ifreq);
		done += sizeof(struct ifreq);
	}
out:
	return done;
}

u32 inet_select_addr(const struct net_device *dev, u32 dst, int scope)
{
	u32 addr = 0;
	struct in_device *in_dev;

	read_lock(&inetdev_lock);
	in_dev = __in_dev_get(dev);
	if (!in_dev)
		goto out_unlock_inetdev;

	read_lock(&in_dev->lock);
	for_primary_ifa(in_dev) {
		if (ifa->ifa_scope > scope)
			continue;
		if (!dst || inet_ifa_match(dst, ifa)) {
			addr = ifa->ifa_local;
			break;
		}
		if (!addr)
			addr = ifa->ifa_local;
	} endfor_ifa(in_dev);
	read_unlock(&in_dev->lock);
	read_unlock(&inetdev_lock);

	if (addr)
		goto out;

	/* Not loopback addresses on loopback should be preferred
	   in this case. It is importnat that lo is the first interface
	   in dev_base list.
	 */
	read_lock(&dev_base_lock);
	read_lock(&inetdev_lock);
	for (dev = dev_base; dev; dev = dev->next) {
		if ((in_dev = __in_dev_get(dev)) == NULL)
			continue;

		read_lock(&in_dev->lock);
		for_primary_ifa(in_dev) {
			if (ifa->ifa_scope != RT_SCOPE_LINK &&
			    ifa->ifa_scope <= scope) {
				read_unlock(&in_dev->lock);
				addr = ifa->ifa_local;
				goto out_unlock_both;
			}
		} endfor_ifa(in_dev);
		read_unlock(&in_dev->lock);
	}
out_unlock_both:
	read_unlock(&inetdev_lock);
	read_unlock(&dev_base_lock);
out:
	return addr;
out_unlock_inetdev:
	read_unlock(&inetdev_lock);
	goto out;
}

static u32 confirm_addr_indev(struct in_device *in_dev, u32 dst,
			      u32 local, int scope)
{
	int same = 0;
	u32 addr = 0;

	for_ifa(in_dev) {
		if (!addr &&
		    (local == ifa->ifa_local || !local) &&
		    ifa->ifa_scope <= scope) {
			addr = ifa->ifa_local;
			if (same)
				break;
		}
		if (!same) {
			same = (!local || inet_ifa_match(local, ifa)) &&
				(!dst || inet_ifa_match(dst, ifa));
			if (same && addr) {
				if (local || !dst)
					break;
				/* Is the selected addr into dst subnet? */
				if (inet_ifa_match(addr, ifa))
					break;
				/* No, then can we use new local src? */
				if (ifa->ifa_scope <= scope) {
					addr = ifa->ifa_local;
					break;
				}
				/* search for large dst subnet for addr */
				same = 0;
			}
		}
	} endfor_ifa(in_dev);

	return same? addr : 0;
}

/*
 * Confirm that local IP address exists using wildcards:
 * - dev: only on this interface, 0=any interface
 * - dst: only in the same subnet as dst, 0=any dst
 * - local: address, 0=autoselect the local address
 * - scope: maximum allowed scope value for the local address
 */
u32 inet_confirm_addr(const struct net_device *dev, u32 dst, u32 local, int scope)
{
	u32 addr = 0;
	struct in_device *in_dev;

	if (dev) {
		read_lock(&inetdev_lock);
		if ((in_dev = __in_dev_get(dev))) {
			read_lock(&in_dev->lock);
			addr = confirm_addr_indev(in_dev, dst, local, scope);
			read_unlock(&in_dev->lock);
		}
		read_unlock(&inetdev_lock);

		return addr;
	}

	read_lock(&dev_base_lock);
	read_lock(&inetdev_lock);
	for (dev = dev_base; dev; dev = dev->next) {
		if ((in_dev = __in_dev_get(dev))) {
			read_lock(&in_dev->lock);
			addr = confirm_addr_indev(in_dev, dst, local, scope);
			read_unlock(&in_dev->lock);
			if (addr)
				break;
		}
	}
	read_unlock(&inetdev_lock);
	read_unlock(&dev_base_lock);

	return addr;
}

/*
 *	Device notifier
 */

int register_inetaddr_notifier(struct notifier_block *nb)
{
	return notifier_chain_register(&inetaddr_chain, nb);
}

int unregister_inetaddr_notifier(struct notifier_block *nb)
{
	return notifier_chain_unregister(&inetaddr_chain, nb);
}

/* Rename ifa_labels for a device name change. Make some effort to preserve existing
 * alias numbering and to create unique labels if possible.
*/
static void inetdev_changename(struct net_device *dev, struct in_device *in_dev)
{ 
	struct in_ifaddr *ifa;
	int named = 0;

	for (ifa = in_dev->ifa_list; ifa; ifa = ifa->ifa_next) { 
		char old[IFNAMSIZ], *dot; 

		memcpy(old, ifa->ifa_label, IFNAMSIZ);
		memcpy(ifa->ifa_label, dev->name, IFNAMSIZ); 
		if (named++ == 0)
			continue;
		dot = strchr(ifa->ifa_label, ':');
		if (dot == NULL) { 
			sprintf(old, ":%d", named); 
			dot = old;
		}
		if (strlen(dot) + strlen(dev->name) < IFNAMSIZ) { 
			strcat(ifa->ifa_label, dot); 
		} else { 
			strcpy(ifa->ifa_label + (IFNAMSIZ - strlen(dot) - 1), dot); 
		} 
	}	
} 

/* Called only under RTNL semaphore */

static int inetdev_event(struct notifier_block *this, unsigned long event,
			 void *ptr)
{
	struct net_device *dev = ptr;
	struct in_device *in_dev = __in_dev_get(dev);

	ASSERT_RTNL();

	if (!in_dev)
		goto out;

	switch (event) {
	case NETDEV_REGISTER:
		printk(KERN_DEBUG "inetdev_event: bug\n");
		dev->ip_ptr = NULL;
		break;
	case NETDEV_UP:
		if (dev->mtu < 68)
			break;
		if (dev == &loopback_dev) {
			struct in_ifaddr *ifa;
			if ((ifa = inet_alloc_ifa()) != NULL) {
				ifa->ifa_local =
				  ifa->ifa_address = htonl(INADDR_LOOPBACK);
				ifa->ifa_prefixlen = 8;
				ifa->ifa_mask = inet_make_mask(8);
				in_dev_hold(in_dev);
				ifa->ifa_dev = in_dev;
				ifa->ifa_scope = RT_SCOPE_HOST;
				memcpy(ifa->ifa_label, dev->name, IFNAMSIZ);
				inet_insert_ifa(ifa);
			}
			in_dev->cnf.no_xfrm = 1;
			in_dev->cnf.no_policy = 1;
		}
		ip_mc_up(in_dev);
		break;
	case NETDEV_DOWN:
		ip_mc_down(in_dev);
		break;
	case NETDEV_CHANGEMTU:
		if (dev->mtu >= 68)
			break;
		/* MTU falled under 68, disable IP */
	case NETDEV_UNREGISTER:
		inetdev_destroy(in_dev);
		break;
	case NETDEV_CHANGENAME:
		/* Do not notify about label change, this event is
		 * not interesting to applications using netlink.
		 */
		inetdev_changename(dev, in_dev);

#ifdef CONFIG_SYSCTL
		devinet_sysctl_unregister(&in_dev->cnf);
		neigh_sysctl_unregister(in_dev->arp_parms);
		neigh_sysctl_register(dev, in_dev->arp_parms, NET_IPV4,
				      NET_IPV4_NEIGH, "ipv4", NULL);
		devinet_sysctl_register(in_dev, &in_dev->cnf);
#endif
		break;
	}
out:
	return NOTIFY_DONE;
}

static struct notifier_block ip_netdev_notifier = {
	.notifier_call =inetdev_event,
};

static int inet_fill_ifaddr(struct sk_buff *skb, struct in_ifaddr *ifa,
			    u32 pid, u32 seq, int event)
{
	struct ifaddrmsg *ifm;
	struct nlmsghdr  *nlh;
	unsigned char	 *b = skb->tail;

	nlh = NLMSG_PUT(skb, pid, seq, event, sizeof(*ifm));
	if (pid) nlh->nlmsg_flags |= NLM_F_MULTI;
	ifm = NLMSG_DATA(nlh);
	ifm->ifa_family = AF_INET;
	ifm->ifa_prefixlen = ifa->ifa_prefixlen;
	ifm->ifa_flags = ifa->ifa_flags|IFA_F_PERMANENT;
	ifm->ifa_scope = ifa->ifa_scope;
	ifm->ifa_index = ifa->ifa_dev->dev->ifindex;
	if (ifa->ifa_address)
		RTA_PUT(skb, IFA_ADDRESS, 4, &ifa->ifa_address);
	if (ifa->ifa_local)
		RTA_PUT(skb, IFA_LOCAL, 4, &ifa->ifa_local);
	if (ifa->ifa_broadcast)
		RTA_PUT(skb, IFA_BROADCAST, 4, &ifa->ifa_broadcast);
	if (ifa->ifa_anycast)
		RTA_PUT(skb, IFA_ANYCAST, 4, &ifa->ifa_anycast);
	if (ifa->ifa_label[0])
		RTA_PUT(skb, IFA_LABEL, IFNAMSIZ, &ifa->ifa_label);
	nlh->nlmsg_len = skb->tail - b;
	return skb->len;

nlmsg_failure:
rtattr_failure:
	skb_trim(skb, b - skb->data);
	return -1;
}

static int inet_dump_ifaddr(struct sk_buff *skb, struct netlink_callback *cb)
{
	int idx, ip_idx;
	struct net_device *dev;
	struct in_device *in_dev;
	struct in_ifaddr *ifa;
	int s_ip_idx, s_idx = cb->args[0];

	s_ip_idx = ip_idx = cb->args[1];
	read_lock(&dev_base_lock);
	for (dev = dev_base, idx = 0; dev; dev = dev->next, idx++) {
		if (idx < s_idx)
			continue;
		if (idx > s_idx)
			s_ip_idx = 0;
		read_lock(&inetdev_lock);
		if ((in_dev = __in_dev_get(dev)) == NULL) {
			read_unlock(&inetdev_lock);
			continue;
		}
		read_lock(&in_dev->lock);
		for (ifa = in_dev->ifa_list, ip_idx = 0; ifa;
		     ifa = ifa->ifa_next, ip_idx++) {
			if (ip_idx < s_ip_idx)
				continue;
			if (inet_fill_ifaddr(skb, ifa, NETLINK_CB(cb->skb).pid,
					     cb->nlh->nlmsg_seq,
					     RTM_NEWADDR) <= 0) {
				read_unlock(&in_dev->lock);
				read_unlock(&inetdev_lock);
				goto done;
			}
		}
		read_unlock(&in_dev->lock);
		read_unlock(&inetdev_lock);
	}

done:
	read_unlock(&dev_base_lock);
	cb->args[0] = idx;
	cb->args[1] = ip_idx;

	return skb->len;
}

static void rtmsg_ifa(int event, struct in_ifaddr* ifa)
{
	int size = NLMSG_SPACE(sizeof(struct ifaddrmsg) + 128);
	struct sk_buff *skb = alloc_skb(size, GFP_KERNEL);

	if (!skb)
		netlink_set_err(rtnl, 0, RTMGRP_IPV4_IFADDR, ENOBUFS);
	else if (inet_fill_ifaddr(skb, ifa, 0, 0, event) < 0) {
		kfree_skb(skb);
		netlink_set_err(rtnl, 0, RTMGRP_IPV4_IFADDR, EINVAL);
	} else {
		NETLINK_CB(skb).dst_groups = RTMGRP_IPV4_IFADDR;
		netlink_broadcast(rtnl, skb, 0, RTMGRP_IPV4_IFADDR, GFP_KERNEL);
	}
}

static struct rtnetlink_link inet_rtnetlink_table[RTM_MAX - RTM_BASE + 1] = {
	 [4] = { .doit	 = inet_rtm_newaddr,  },
	 [5] = { .doit	 = inet_rtm_deladdr,  },
	 [6] = { .dumpit = inet_dump_ifaddr,  },
	 [8] = { .doit	 = inet_rtm_newroute, },
	 [9] = { .doit	 = inet_rtm_delroute, },
	[10] = { .doit	 = inet_rtm_getroute, .dumpit = inet_dump_fib, },
#ifdef CONFIG_IP_MULTIPLE_TABLES
	[16] = { .doit	 = inet_rtm_newrule, },
	[17] = { .doit	 = inet_rtm_delrule, },
	[18] = { .dumpit = inet_dump_rules,  },
#endif
};

#ifdef CONFIG_SYSCTL

void inet_forward_change(void)
{
	struct net_device *dev;
	int on = ipv4_devconf.forwarding;

	ipv4_devconf.accept_redirects = !on;
	ipv4_devconf_dflt.forwarding = on;

	read_lock(&dev_base_lock);
	for (dev = dev_base; dev; dev = dev->next) {
		struct in_device *in_dev;
		read_lock(&inetdev_lock);
		in_dev = __in_dev_get(dev);
		if (in_dev)
			in_dev->cnf.forwarding = on;
		read_unlock(&inetdev_lock);
	}
	read_unlock(&dev_base_lock);

	rt_cache_flush(0);
}

static int devinet_sysctl_forward(ctl_table *ctl, int write,
				  struct file* filp, void __user *buffer,
				  size_t *lenp, loff_t *ppos)
{
	int *valp = ctl->data;
	int val = *valp;
	int ret = proc_dointvec(ctl, write, filp, buffer, lenp, ppos);

	if (write && *valp != val) {
		if (valp == &ipv4_devconf.forwarding)
			inet_forward_change();
		else if (valp != &ipv4_devconf_dflt.forwarding)
			rt_cache_flush(0);
	}

	return ret;
}

int ipv4_doint_and_flush(ctl_table *ctl, int write,
			 struct file* filp, void __user *buffer,
			 size_t *lenp, loff_t *ppos)
{
	int *valp = ctl->data;
	int val = *valp;
	int ret = proc_dointvec(ctl, write, filp, buffer, lenp, ppos);

	if (write && *valp != val)
		rt_cache_flush(0);

	return ret;
}

int ipv4_doint_and_flush_strategy(ctl_table *table, int __user *name, int nlen,
				  void __user *oldval, size_t __user *oldlenp,
				  void __user *newval, size_t newlen, 
				  void **context)
{
	int *valp = table->data;
	int new;

	if (!newval || !newlen)
		return 0;

	if (newlen != sizeof(int))
		return -EINVAL;

	if (get_user(new, (int __user *)newval))
		return -EFAULT;

	if (new == *valp)
		return 0;

	if (oldval && oldlenp) {
		size_t len;

		if (get_user(len, oldlenp))
			return -EFAULT;

		if (len) {
			if (len > table->maxlen)
				len = table->maxlen;
			if (copy_to_user(oldval, valp, len))
				return -EFAULT;
			if (put_user(len, oldlenp))
				return -EFAULT;
		}
	}

	*valp = new;
	rt_cache_flush(0);
	return 1;
}


static struct devinet_sysctl_table {
	struct ctl_table_header *sysctl_header;
	ctl_table		devinet_vars[20];
	ctl_table		devinet_dev[2];
	ctl_table		devinet_conf_dir[2];
	ctl_table		devinet_proto_dir[2];
	ctl_table		devinet_root_dir[2];
} devinet_sysctl = {
	.devinet_vars = {
		{
			.ctl_name	= NET_IPV4_CONF_FORWARDING,
			.procname	= "forwarding",
			.data		= &ipv4_devconf.forwarding,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &devinet_sysctl_forward,
		},
		{
			.ctl_name	= NET_IPV4_CONF_MC_FORWARDING,
			.procname	= "mc_forwarding",
			.data		= &ipv4_devconf.mc_forwarding,
			.maxlen		= sizeof(int),
			.mode		= 0444,
			.proc_handler	= &proc_dointvec,
		},
		{
			.ctl_name	= NET_IPV4_CONF_ACCEPT_REDIRECTS,
			.procname	= "accept_redirects",
			.data		= &ipv4_devconf.accept_redirects,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &proc_dointvec,
		},
		{
			.ctl_name	= NET_IPV4_CONF_SECURE_REDIRECTS,
			.procname	= "secure_redirects",
			.data		= &ipv4_devconf.secure_redirects,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &proc_dointvec,
		},
		{
			.ctl_name	= NET_IPV4_CONF_SHARED_MEDIA,
			.procname	= "shared_media",
			.data		= &ipv4_devconf.shared_media,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &proc_dointvec,
		},
		{
			.ctl_name	= NET_IPV4_CONF_RP_FILTER,
			.procname	= "rp_filter",
			.data		= &ipv4_devconf.rp_filter,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &proc_dointvec,
		},
		{
			.ctl_name	= NET_IPV4_CONF_SEND_REDIRECTS,
			.procname	= "send_redirects",
			.data		= &ipv4_devconf.send_redirects,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &proc_dointvec,
		},
		{
			.ctl_name	= NET_IPV4_CONF_ACCEPT_SOURCE_ROUTE,
			.procname	= "accept_source_route",
			.data		= &ipv4_devconf.accept_source_route,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &proc_dointvec,
		},
		{
			.ctl_name	= NET_IPV4_CONF_PROXY_ARP,
			.procname	= "proxy_arp",
			.data		= &ipv4_devconf.proxy_arp,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &proc_dointvec,
		},
		{
			.ctl_name	= NET_IPV4_CONF_MEDIUM_ID,
			.procname	= "medium_id",
			.data		= &ipv4_devconf.medium_id,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &proc_dointvec,
		},
		{
			.ctl_name	= NET_IPV4_CONF_BOOTP_RELAY,
			.procname	= "bootp_relay",
			.data		= &ipv4_devconf.bootp_relay,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &proc_dointvec,
		},
		{
			.ctl_name	= NET_IPV4_CONF_LOG_MARTIANS,
			.procname	= "log_martians",
			.data		= &ipv4_devconf.log_martians,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &proc_dointvec,
		},
		{
			.ctl_name	= NET_IPV4_CONF_TAG,
			.procname	= "tag",
			.data		= &ipv4_devconf.tag,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &proc_dointvec,
		},
		{
			.ctl_name	= NET_IPV4_CONF_ARPFILTER,
			.procname	= "arp_filter",
			.data		= &ipv4_devconf.arp_filter,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &proc_dointvec,
		},
		{
			.ctl_name	= NET_IPV4_CONF_ARP_ANNOUNCE,
			.procname	= "arp_announce",
			.data		= &ipv4_devconf.arp_announce,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &proc_dointvec,
		},
		{
			.ctl_name	= NET_IPV4_CONF_ARP_IGNORE,
			.procname	= "arp_ignore",
			.data		= &ipv4_devconf.arp_ignore,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &proc_dointvec,
		},
		{
			.ctl_name	= NET_IPV4_CONF_NOXFRM,
			.procname	= "disable_xfrm",
			.data		= &ipv4_devconf.no_xfrm,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &ipv4_doint_and_flush,
			.strategy	= &ipv4_doint_and_flush_strategy,
		},
		{
			.ctl_name	= NET_IPV4_CONF_NOPOLICY,
			.procname	= "disable_policy",
			.data		= &ipv4_devconf.no_policy,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &ipv4_doint_and_flush,
			.strategy	= &ipv4_doint_and_flush_strategy,
		},
		{
			.ctl_name	= NET_IPV4_CONF_FORCE_IGMP_VERSION,
			.procname	= "force_igmp_version",
			.data		= &ipv4_devconf.force_igmp_version,
			.maxlen		= sizeof(int),
			.mode		= 0644,
			.proc_handler	= &ipv4_doint_and_flush,
			.strategy	= &ipv4_doint_and_flush_strategy,
		},
	},
	.devinet_dev = {
		{
			.ctl_name	= NET_PROTO_CONF_ALL,
			.procname	= "all",
			.mode		= 0555,
			.child		= devinet_sysctl.devinet_vars,
		},
	},
	.devinet_conf_dir = {
	        {
			.ctl_name	= NET_IPV4_CONF,
			.procname	= "conf",
			.mode		= 0555,
			.child		= devinet_sysctl.devinet_dev,
		},
	},
	.devinet_proto_dir = {
		{
			.ctl_name	= NET_IPV4,
			.procname	= "ipv4",
			.mode		= 0555,
			.child 		= devinet_sysctl.devinet_conf_dir,
		},
	},
	.devinet_root_dir = {
		{
			.ctl_name	= CTL_NET,
			.procname 	= "net",
			.mode		= 0555,
			.child		= devinet_sysctl.devinet_proto_dir,
		},
	},
};

static void devinet_sysctl_register(struct in_device *in_dev,
				    struct ipv4_devconf *p)
{
	int i;
	struct net_device *dev = in_dev ? in_dev->dev : NULL;
	struct devinet_sysctl_table *t = kmalloc(sizeof(*t), GFP_KERNEL);
	char *dev_name = NULL;

	if (!t)
		return;
	memcpy(t, &devinet_sysctl, sizeof(*t));
	for (i = 0; i < ARRAY_SIZE(t->devinet_vars) - 1; i++) {
		t->devinet_vars[i].data += (char *)p - (char *)&ipv4_devconf;
		t->devinet_vars[i].de = NULL;
	}

	if (dev) {
		dev_name = dev->name; 
		t->devinet_dev[0].ctl_name = dev->ifindex;
	} else {
		dev_name = "default";
		t->devinet_dev[0].ctl_name = NET_PROTO_CONF_DEFAULT;
	}

	/* 
	 * Make a copy of dev_name, because '.procname' is regarded as const 
	 * by sysctl and we wouldn't want anyone to change it under our feet
	 * (see SIOCSIFNAME).
	 */	
	dev_name = net_sysctl_strdup(dev_name);
	if (!dev_name)
	    goto free;

	t->devinet_dev[0].procname    = dev_name;
	t->devinet_dev[0].child	      = t->devinet_vars;
	t->devinet_dev[0].de	      = NULL;
	t->devinet_conf_dir[0].child  = t->devinet_dev;
	t->devinet_conf_dir[0].de     = NULL;
	t->devinet_proto_dir[0].child = t->devinet_conf_dir;
	t->devinet_proto_dir[0].de    = NULL;
	t->devinet_root_dir[0].child  = t->devinet_proto_dir;
	t->devinet_root_dir[0].de     = NULL;

	t->sysctl_header = register_sysctl_table(t->devinet_root_dir, 0);
	if (!t->sysctl_header)
	    goto free_procname;

	p->sysctl = t;
	return;

	/* error path */
 free_procname:
	kfree(dev_name);
 free:
	kfree(t);
	return;
}

static void devinet_sysctl_unregister(struct ipv4_devconf *p)
{
	if (p->sysctl) {
		struct devinet_sysctl_table *t = p->sysctl;
		p->sysctl = NULL;
		unregister_sysctl_table(t->sysctl_header);
		kfree(t->devinet_dev[0].procname);
		kfree(t);
	}
}
#endif

void __init devinet_init(void)
{
	register_gifconf(PF_INET, inet_gifconf);
	register_netdevice_notifier(&ip_netdev_notifier);
	rtnetlink_links[PF_INET] = inet_rtnetlink_table;
#ifdef CONFIG_SYSCTL
	devinet_sysctl.sysctl_header =
		register_sysctl_table(devinet_sysctl.devinet_root_dir, 0);
	devinet_sysctl_register(NULL, &ipv4_devconf_dflt);
#endif
}

EXPORT_SYMBOL(devinet_ioctl);
EXPORT_SYMBOL(in_dev_finish_destroy);
EXPORT_SYMBOL(inet_select_addr);
EXPORT_SYMBOL(inetdev_by_index);
EXPORT_SYMBOL(inetdev_lock);
EXPORT_SYMBOL(register_inetaddr_notifier);
EXPORT_SYMBOL(unregister_inetaddr_notifier);
