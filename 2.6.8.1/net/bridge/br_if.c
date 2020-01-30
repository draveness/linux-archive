/*
 *	Userspace interface
 *	Linux ethernet bridge
 *
 *	Authors:
 *	Lennert Buytenhek		<buytenh@gnu.org>
 *
 *	$Id: br_if.c,v 1.7 2001/12/24 00:59:55 davem Exp $
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/if_arp.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/rtnetlink.h>
#include <net/sock.h>

#include "br_private.h"

/*
 * Determine initial path cost based on speed.
 * using recommendations from 802.1d standard
 *
 * Need to simulate user ioctl because not all device's that support
 * ethtool, use ethtool_ops.  Also, since driver might sleep need to
 * not be holding any locks.
 */
static int br_initial_port_cost(struct net_device *dev)
{

	struct ethtool_cmd ecmd = { ETHTOOL_GSET };
	struct ifreq ifr;
	mm_segment_t old_fs;
	int err;

	strncpy(ifr.ifr_name, dev->name, IFNAMSIZ);
	ifr.ifr_data = (void __user *) &ecmd;

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	err = dev_ethtool(&ifr);
	set_fs(old_fs);
	
	if (!err) {
		switch(ecmd.speed) {
		case SPEED_100:
			return 19;
		case SPEED_1000:
			return 4;
		case SPEED_10000:
			return 2;
		case SPEED_10:
			return 100;
		default:
			pr_info("bridge: can't decode speed from %s: %d\n",
				dev->name, ecmd.speed);
			return 100;
		}
	}

	/* Old silly heuristics based on name */
	if (!strncmp(dev->name, "lec", 3))
		return 7;

	if (!strncmp(dev->name, "plip", 4))
		return 2500;

	return 100;	/* assume old 10Mbps */
}

static void destroy_nbp(struct net_bridge_port *p)
{
	struct net_device *dev = p->dev;

	dev->br_port = NULL;
	p->br = NULL;
	p->dev = NULL;
	dev_put(dev);

	br_sysfs_freeif(p);
}

static void destroy_nbp_rcu(struct rcu_head *head)
{
	struct net_bridge_port *p =
			container_of(head, struct net_bridge_port, rcu);
	destroy_nbp(p);
}

/* called with RTNL */
static void del_nbp(struct net_bridge_port *p)
{
	struct net_bridge *br = p->br;
	struct net_device *dev = p->dev;

	dev_set_promiscuity(dev, -1);

	spin_lock_bh(&br->lock);
	br_stp_disable_port(p);
	spin_unlock_bh(&br->lock);

	br_fdb_delete_by_port(br, p);

	list_del_rcu(&p->list);

	del_timer_sync(&p->message_age_timer);
	del_timer_sync(&p->forward_delay_timer);
	del_timer_sync(&p->hold_timer);
	
	call_rcu(&p->rcu, destroy_nbp_rcu);
}

/* called with RTNL */
static void del_br(struct net_bridge *br)
{
	struct net_bridge_port *p, *n;

	list_for_each_entry_safe(p, n, &br->port_list, list) {
		br_sysfs_removeif(p);
		del_nbp(p);
	}

	del_timer_sync(&br->gc_timer);

	br_sysfs_delbr(br->dev);
 	unregister_netdevice(br->dev);
}

static struct net_device *new_bridge_dev(const char *name)
{
	struct net_bridge *br;
	struct net_device *dev;

	dev = alloc_netdev(sizeof(struct net_bridge), name,
			   br_dev_setup);
	
	if (!dev)
		return NULL;

	br = netdev_priv(dev);
	br->dev = dev;

	br->lock = SPIN_LOCK_UNLOCKED;
	INIT_LIST_HEAD(&br->port_list);
	br->hash_lock = SPIN_LOCK_UNLOCKED;

	br->bridge_id.prio[0] = 0x80;
	br->bridge_id.prio[1] = 0x00;
	memset(br->bridge_id.addr, 0, ETH_ALEN);

	br->stp_enabled = 0;
	br->designated_root = br->bridge_id;
	br->root_path_cost = 0;
	br->root_port = 0;
	br->bridge_max_age = br->max_age = 20 * HZ;
	br->bridge_hello_time = br->hello_time = 2 * HZ;
	br->bridge_forward_delay = br->forward_delay = 15 * HZ;
	br->topology_change = 0;
	br->topology_change_detected = 0;
	br->ageing_time = 300 * HZ;
	INIT_LIST_HEAD(&br->age_list);

	br_stp_timer_init(br);

	return dev;
}

/* find an available port number */
static int find_portno(struct net_bridge *br)
{
	int index;
	struct net_bridge_port *p;
	unsigned long *inuse;

	inuse = kmalloc(BITS_TO_LONGS(BR_MAX_PORTS)*sizeof(unsigned long),
			GFP_KERNEL);
	if (!inuse)
		return -ENOMEM;

	memset(inuse, 0, BITS_TO_LONGS(BR_MAX_PORTS)*sizeof(unsigned long));
	set_bit(0, inuse);	/* zero is reserved */
	list_for_each_entry(p, &br->port_list, list) {
		set_bit(p->port_no, inuse);
	}
	index = find_first_zero_bit(inuse, BR_MAX_PORTS);
	kfree(inuse);

	return (index >= BR_MAX_PORTS) ? -EXFULL : index;
}

/* called with RTNL */
static struct net_bridge_port *new_nbp(struct net_bridge *br, 
				       struct net_device *dev,
				       unsigned long cost)
{
	int index;
	struct net_bridge_port *p;
	
	index = find_portno(br);
	if (index < 0)
		return ERR_PTR(index);

	p = kmalloc(sizeof(*p), GFP_KERNEL);
	if (p == NULL)
		return ERR_PTR(-ENOMEM);

	memset(p, 0, sizeof(*p));
	p->br = br;
	dev_hold(dev);
	p->dev = dev;
	p->path_cost = cost;
 	p->priority = 0x8000 >> BR_PORT_BITS;
	dev->br_port = p;
	p->port_no = index;
	br_init_port(p);
	p->state = BR_STATE_DISABLED;
	kobject_init(&p->kobj);

	return p;
}

int br_add_bridge(const char *name)
{
	struct net_device *dev;
	int ret;

	dev = new_bridge_dev(name);
	if (!dev) 
		return -ENOMEM;

	rtnl_lock();
	if (strchr(dev->name, '%')) {
		ret = dev_alloc_name(dev, dev->name);
		if (ret < 0)
			goto err1;
	}

	ret = register_netdevice(dev);
	if (ret)
		goto err2;

	/* network device kobject is not setup until
	 * after rtnl_unlock does it's hotplug magic.
	 * so hold reference to avoid race.
	 */
	dev_hold(dev);
	rtnl_unlock();

	ret = br_sysfs_addbr(dev);
	dev_put(dev);

	if (ret) 
		unregister_netdev(dev);
 out:
	return ret;

 err2:
	free_netdev(dev);
 err1:
	rtnl_unlock();
	goto out;
}

int br_del_bridge(const char *name)
{
	struct net_device *dev;
	int ret = 0;

	rtnl_lock();
	dev = __dev_get_by_name(name);
	if (dev == NULL) 
		ret =  -ENXIO; 	/* Could not find device */

	else if (!(dev->priv_flags & IFF_EBRIDGE)) {
		/* Attempt to delete non bridge device! */
		ret = -EPERM;
	}

	else if (dev->flags & IFF_UP) {
		/* Not shutdown yet. */
		ret = -EBUSY;
	} 

	else 
		del_br(netdev_priv(dev));

	rtnl_unlock();
	return ret;
}

/* Mtu of the bridge pseudo-device 1500 or the minimum of the ports */
int br_min_mtu(const struct net_bridge *br)
{
	const struct net_bridge_port *p;
	int mtu = 0;

	ASSERT_RTNL();

	if (list_empty(&br->port_list))
		mtu = 1500;
	else {
		list_for_each_entry(p, &br->port_list, list) {
			if (!mtu  || p->dev->mtu < mtu)
				mtu = p->dev->mtu;
		}
	}
	return mtu;
}

/* called with RTNL */
int br_add_if(struct net_bridge *br, struct net_device *dev)
{
	struct net_bridge_port *p;
	int err = 0;

	if (dev->flags & IFF_LOOPBACK || dev->type != ARPHRD_ETHER)
		return -EINVAL;

	if (dev->hard_start_xmit == br_dev_xmit)
		return -ELOOP;

	if (dev->br_port != NULL)
		return -EBUSY;

	if (IS_ERR(p = new_nbp(br, dev, br_initial_port_cost(dev))))
		return PTR_ERR(p);

 	if ((err = br_fdb_insert(br, p, dev->dev_addr, 1)))
		destroy_nbp(p);
 
	else if ((err = br_sysfs_addif(p)))
		del_nbp(p);
	else {
		dev_set_promiscuity(dev, 1);

		list_add_rcu(&p->list, &br->port_list);

		spin_lock_bh(&br->lock);
		br_stp_recalculate_bridge_id(br);
		if ((br->dev->flags & IFF_UP) 
		    && (dev->flags & IFF_UP) && netif_carrier_ok(dev))
			br_stp_enable_port(p);
		spin_unlock_bh(&br->lock);

		dev_set_mtu(br->dev, br_min_mtu(br));
	}

	return err;
}

/* called with RTNL */
int br_del_if(struct net_bridge *br, struct net_device *dev)
{
	struct net_bridge_port *p = dev->br_port;
	
	if (!p || p->br != br) 
		return -EINVAL;

	br_sysfs_removeif(p);
	del_nbp(p);

	spin_lock_bh(&br->lock);
	br_stp_recalculate_bridge_id(br);
	spin_unlock_bh(&br->lock);

	return 0;
}

void __exit br_cleanup_bridges(void)
{
	struct net_device *dev, *nxt;

	rtnl_lock();
	for (dev = dev_base; dev; dev = nxt) {
		nxt = dev->next;
		if (dev->priv_flags & IFF_EBRIDGE)
			del_br(dev->priv);
	}
	rtnl_unlock();

}
