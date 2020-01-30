/*
 * Copyright (c) 2007 Patrick McHardy <kaber@trash.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * The code this is based on carried the following copyright notice:
 * ---
 * (C) Copyright 2001-2006
 * Alex Zeffertt, Cambridge Broadband Ltd, ajz@cambridgebroadband.com
 * Re-worked by Ben Greear <greearb@candelatech.com>
 * ---
 */
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/notifier.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_arp.h>
#include <linux/if_link.h>
#include <linux/if_macvlan.h>
#include <net/rtnetlink.h>

#define MACVLAN_HASH_SIZE	(1 << BITS_PER_BYTE)

struct macvlan_port {
	struct net_device	*dev;
	struct hlist_head	vlan_hash[MACVLAN_HASH_SIZE];
	struct list_head	vlans;
};

struct macvlan_dev {
	struct net_device	*dev;
	struct list_head	list;
	struct hlist_node	hlist;
	struct macvlan_port	*port;
	struct net_device	*lowerdev;
};


static struct macvlan_dev *macvlan_hash_lookup(const struct macvlan_port *port,
					       const unsigned char *addr)
{
	struct macvlan_dev *vlan;
	struct hlist_node *n;

	hlist_for_each_entry_rcu(vlan, n, &port->vlan_hash[addr[5]], hlist) {
		if (!compare_ether_addr(vlan->dev->dev_addr, addr))
			return vlan;
	}
	return NULL;
}

static void macvlan_broadcast(struct sk_buff *skb,
			      const struct macvlan_port *port)
{
	const struct ethhdr *eth = eth_hdr(skb);
	const struct macvlan_dev *vlan;
	struct hlist_node *n;
	struct net_device *dev;
	struct sk_buff *nskb;
	unsigned int i;

	for (i = 0; i < MACVLAN_HASH_SIZE; i++) {
		hlist_for_each_entry_rcu(vlan, n, &port->vlan_hash[i], hlist) {
			dev = vlan->dev;
			if (unlikely(!(dev->flags & IFF_UP)))
				continue;

			nskb = skb_clone(skb, GFP_ATOMIC);
			if (nskb == NULL) {
				dev->stats.rx_errors++;
				dev->stats.rx_dropped++;
				continue;
			}

			dev->stats.rx_bytes += skb->len + ETH_HLEN;
			dev->stats.rx_packets++;
			dev->stats.multicast++;
			dev->last_rx = jiffies;

			nskb->dev = dev;
			if (!compare_ether_addr(eth->h_dest, dev->broadcast))
				nskb->pkt_type = PACKET_BROADCAST;
			else
				nskb->pkt_type = PACKET_MULTICAST;

			netif_rx(nskb);
		}
	}
}

/* called under rcu_read_lock() from netif_receive_skb */
static struct sk_buff *macvlan_handle_frame(struct sk_buff *skb)
{
	const struct ethhdr *eth = eth_hdr(skb);
	const struct macvlan_port *port;
	const struct macvlan_dev *vlan;
	struct net_device *dev;

	port = rcu_dereference(skb->dev->macvlan_port);
	if (port == NULL)
		return skb;

	if (is_multicast_ether_addr(eth->h_dest)) {
		macvlan_broadcast(skb, port);
		return skb;
	}

	vlan = macvlan_hash_lookup(port, eth->h_dest);
	if (vlan == NULL)
		return skb;

	dev = vlan->dev;
	if (unlikely(!(dev->flags & IFF_UP))) {
		kfree_skb(skb);
		return NULL;
	}

	skb = skb_share_check(skb, GFP_ATOMIC);
	if (skb == NULL) {
		dev->stats.rx_errors++;
		dev->stats.rx_dropped++;
		return NULL;
	}

	dev->stats.rx_bytes += skb->len + ETH_HLEN;
	dev->stats.rx_packets++;
	dev->last_rx = jiffies;

	skb->dev = dev;
	skb->pkt_type = PACKET_HOST;

	netif_rx(skb);
	return NULL;
}

static int macvlan_hard_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	const struct macvlan_dev *vlan = netdev_priv(dev);
	unsigned int len = skb->len;
	int ret;

	skb->dev = vlan->lowerdev;
	ret = dev_queue_xmit(skb);

	if (likely(ret == NET_XMIT_SUCCESS)) {
		dev->stats.tx_packets++;
		dev->stats.tx_bytes += len;
	} else {
		dev->stats.tx_errors++;
		dev->stats.tx_aborted_errors++;
	}
	return NETDEV_TX_OK;
}

static int macvlan_hard_header(struct sk_buff *skb, struct net_device *dev,
			       unsigned short type, void *daddr, void *saddr,
			       unsigned len)
{
	const struct macvlan_dev *vlan = netdev_priv(dev);
	struct net_device *lowerdev = vlan->lowerdev;

	return lowerdev->hard_header(skb, lowerdev, type, daddr,
				     saddr ? : dev->dev_addr, len);
}

static int macvlan_open(struct net_device *dev)
{
	struct macvlan_dev *vlan = netdev_priv(dev);
	struct macvlan_port *port = vlan->port;
	struct net_device *lowerdev = vlan->lowerdev;
	int err;

	err = dev_unicast_add(lowerdev, dev->dev_addr, ETH_ALEN);
	if (err < 0)
		return err;
	if (dev->flags & IFF_ALLMULTI)
		dev_set_allmulti(lowerdev, 1);

	hlist_add_head_rcu(&vlan->hlist, &port->vlan_hash[dev->dev_addr[5]]);
	return 0;
}

static int macvlan_stop(struct net_device *dev)
{
	struct macvlan_dev *vlan = netdev_priv(dev);
	struct net_device *lowerdev = vlan->lowerdev;

	dev_mc_unsync(lowerdev, dev);
	if (dev->flags & IFF_ALLMULTI)
		dev_set_allmulti(lowerdev, -1);

	dev_unicast_delete(lowerdev, dev->dev_addr, ETH_ALEN);

	hlist_del_rcu(&vlan->hlist);
	synchronize_rcu();
	return 0;
}

static void macvlan_change_rx_flags(struct net_device *dev, int change)
{
	struct macvlan_dev *vlan = netdev_priv(dev);
	struct net_device *lowerdev = vlan->lowerdev;

	if (change & IFF_ALLMULTI)
		dev_set_allmulti(lowerdev, dev->flags & IFF_ALLMULTI ? 1 : -1);
}

static void macvlan_set_multicast_list(struct net_device *dev)
{
	struct macvlan_dev *vlan = netdev_priv(dev);

	dev_mc_sync(vlan->lowerdev, dev);
}

static int macvlan_change_mtu(struct net_device *dev, int new_mtu)
{
	struct macvlan_dev *vlan = netdev_priv(dev);

	if (new_mtu < 68 || vlan->lowerdev->mtu < new_mtu)
		return -EINVAL;
	dev->mtu = new_mtu;
	return 0;
}

/*
 * macvlan network devices have devices nesting below it and are a special
 * "super class" of normal network devices; split their locks off into a
 * separate class since they always nest.
 */
static struct lock_class_key macvlan_netdev_xmit_lock_key;

#define MACVLAN_FEATURES \
	(NETIF_F_SG | NETIF_F_ALL_CSUM | NETIF_F_HIGHDMA | NETIF_F_FRAGLIST | \
	 NETIF_F_GSO | NETIF_F_TSO | NETIF_F_UFO | NETIF_F_GSO_ROBUST | \
	 NETIF_F_TSO_ECN | NETIF_F_TSO6)

#define MACVLAN_STATE_MASK \
	((1<<__LINK_STATE_NOCARRIER) | (1<<__LINK_STATE_DORMANT))

static int macvlan_init(struct net_device *dev)
{
	struct macvlan_dev *vlan = netdev_priv(dev);
	const struct net_device *lowerdev = vlan->lowerdev;

	dev->state		= (dev->state & ~MACVLAN_STATE_MASK) |
				  (lowerdev->state & MACVLAN_STATE_MASK);
	dev->features 		= lowerdev->features & MACVLAN_FEATURES;
	dev->iflink		= lowerdev->ifindex;

	lockdep_set_class(&dev->_xmit_lock, &macvlan_netdev_xmit_lock_key);
	return 0;
}

static void macvlan_ethtool_get_drvinfo(struct net_device *dev,
					struct ethtool_drvinfo *drvinfo)
{
	snprintf(drvinfo->driver, 32, "macvlan");
	snprintf(drvinfo->version, 32, "0.1");
}

static u32 macvlan_ethtool_get_rx_csum(struct net_device *dev)
{
	const struct macvlan_dev *vlan = netdev_priv(dev);
	struct net_device *lowerdev = vlan->lowerdev;

	if (lowerdev->ethtool_ops->get_rx_csum == NULL)
		return 0;
	return lowerdev->ethtool_ops->get_rx_csum(lowerdev);
}

static const struct ethtool_ops macvlan_ethtool_ops = {
	.get_link		= ethtool_op_get_link,
	.get_rx_csum		= macvlan_ethtool_get_rx_csum,
	.get_tx_csum		= ethtool_op_get_tx_csum,
	.get_tso		= ethtool_op_get_tso,
	.get_ufo		= ethtool_op_get_ufo,
	.get_sg			= ethtool_op_get_sg,
	.get_drvinfo		= macvlan_ethtool_get_drvinfo,
};

static void macvlan_setup(struct net_device *dev)
{
	ether_setup(dev);

	dev->init		= macvlan_init;
	dev->open		= macvlan_open;
	dev->stop		= macvlan_stop;
	dev->change_mtu		= macvlan_change_mtu;
	dev->change_rx_flags	= macvlan_change_rx_flags;
	dev->set_multicast_list	= macvlan_set_multicast_list;
	dev->hard_header	= macvlan_hard_header;
	dev->hard_start_xmit	= macvlan_hard_start_xmit;
	dev->destructor		= free_netdev;
	dev->ethtool_ops	= &macvlan_ethtool_ops;
	dev->tx_queue_len	= 0;
}

static int macvlan_port_create(struct net_device *dev)
{
	struct macvlan_port *port;
	unsigned int i;

	if (dev->type != ARPHRD_ETHER || dev->flags & IFF_LOOPBACK)
		return -EINVAL;

	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (port == NULL)
		return -ENOMEM;

	port->dev = dev;
	INIT_LIST_HEAD(&port->vlans);
	for (i = 0; i < MACVLAN_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&port->vlan_hash[i]);
	rcu_assign_pointer(dev->macvlan_port, port);
	return 0;
}

static void macvlan_port_destroy(struct net_device *dev)
{
	struct macvlan_port *port = dev->macvlan_port;

	rcu_assign_pointer(dev->macvlan_port, NULL);
	synchronize_rcu();
	kfree(port);
}

static void macvlan_transfer_operstate(struct net_device *dev)
{
	struct macvlan_dev *vlan = netdev_priv(dev);
	const struct net_device *lowerdev = vlan->lowerdev;

	if (lowerdev->operstate == IF_OPER_DORMANT)
		netif_dormant_on(dev);
	else
		netif_dormant_off(dev);

	if (netif_carrier_ok(lowerdev)) {
		if (!netif_carrier_ok(dev))
			netif_carrier_on(dev);
	} else {
		if (netif_carrier_ok(lowerdev))
			netif_carrier_off(dev);
	}
}

static int macvlan_validate(struct nlattr *tb[], struct nlattr *data[])
{
	if (tb[IFLA_ADDRESS]) {
		if (nla_len(tb[IFLA_ADDRESS]) != ETH_ALEN)
			return -EINVAL;
		if (!is_valid_ether_addr(nla_data(tb[IFLA_ADDRESS])))
			return -EADDRNOTAVAIL;
	}
	return 0;
}

static int macvlan_newlink(struct net_device *dev,
			   struct nlattr *tb[], struct nlattr *data[])
{
	struct macvlan_dev *vlan = netdev_priv(dev);
	struct macvlan_port *port;
	struct net_device *lowerdev;
	int err;

	if (!tb[IFLA_LINK])
		return -EINVAL;

	lowerdev = __dev_get_by_index(nla_get_u32(tb[IFLA_LINK]));
	if (lowerdev == NULL)
		return -ENODEV;

	if (!tb[IFLA_MTU])
		dev->mtu = lowerdev->mtu;
	else if (dev->mtu > lowerdev->mtu)
		return -EINVAL;

	if (!tb[IFLA_ADDRESS])
		random_ether_addr(dev->dev_addr);

	if (lowerdev->macvlan_port == NULL) {
		err = macvlan_port_create(lowerdev);
		if (err < 0)
			return err;
	}
	port = lowerdev->macvlan_port;

	vlan->lowerdev = lowerdev;
	vlan->dev      = dev;
	vlan->port     = port;

	err = register_netdevice(dev);
	if (err < 0)
		return err;

	list_add_tail(&vlan->list, &port->vlans);
	macvlan_transfer_operstate(dev);
	return 0;
}

static void macvlan_dellink(struct net_device *dev)
{
	struct macvlan_dev *vlan = netdev_priv(dev);
	struct macvlan_port *port = vlan->port;

	list_del(&vlan->list);
	unregister_netdevice(dev);

	if (list_empty(&port->vlans))
		macvlan_port_destroy(dev);
}

static struct rtnl_link_ops macvlan_link_ops __read_mostly = {
	.kind		= "macvlan",
	.priv_size	= sizeof(struct macvlan_dev),
	.setup		= macvlan_setup,
	.validate	= macvlan_validate,
	.newlink	= macvlan_newlink,
	.dellink	= macvlan_dellink,
};

static int macvlan_device_event(struct notifier_block *unused,
				unsigned long event, void *ptr)
{
	struct net_device *dev = ptr;
	struct macvlan_dev *vlan, *next;
	struct macvlan_port *port;

	port = dev->macvlan_port;
	if (port == NULL)
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_CHANGE:
		list_for_each_entry(vlan, &port->vlans, list)
			macvlan_transfer_operstate(vlan->dev);
		break;
	case NETDEV_FEAT_CHANGE:
		list_for_each_entry(vlan, &port->vlans, list) {
			vlan->dev->features = dev->features & MACVLAN_FEATURES;
			netdev_features_change(vlan->dev);
		}
		break;
	case NETDEV_UNREGISTER:
		list_for_each_entry_safe(vlan, next, &port->vlans, list)
			macvlan_dellink(vlan->dev);
		break;
	}
	return NOTIFY_DONE;
}

static struct notifier_block macvlan_notifier_block __read_mostly = {
	.notifier_call	= macvlan_device_event,
};

static int __init macvlan_init_module(void)
{
	int err;

	register_netdevice_notifier(&macvlan_notifier_block);
	macvlan_handle_frame_hook = macvlan_handle_frame;

	err = rtnl_link_register(&macvlan_link_ops);
	if (err < 0)
		goto err1;
	return 0;
err1:
	macvlan_handle_frame_hook = macvlan_handle_frame;
	unregister_netdevice_notifier(&macvlan_notifier_block);
	return err;
}

static void __exit macvlan_cleanup_module(void)
{
	rtnl_link_unregister(&macvlan_link_ops);
	macvlan_handle_frame_hook = NULL;
	unregister_netdevice_notifier(&macvlan_notifier_block);
}

module_init(macvlan_init_module);
module_exit(macvlan_cleanup_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Patrick McHardy <kaber@trash.net>");
MODULE_DESCRIPTION("Driver for MAC address based VLANs");
MODULE_ALIAS_RTNL_LINK("macvlan");
