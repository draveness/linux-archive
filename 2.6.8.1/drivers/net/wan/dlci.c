/*
 * DLCI		Implementation of Frame Relay protocol for Linux, according to
 *		RFC 1490.  This generic device provides en/decapsulation for an
 *		underlying hardware driver.  Routes & IPs are assigned to these
 *		interfaces.  Requires 'dlcicfg' program to create usable 
 *		interfaces, the initial one, 'dlci' is for IOCTL use only.
 *
 * Version:	@(#)dlci.c	0.35	4 Jan 1997
 *
 * Author:	Mike McLagan <mike.mclagan@linux.org>
 *
 * Changes:
 *
 *		0.15	Mike Mclagan	Packet freeing, bug in kmalloc call
 *					DLCI_RET handling
 *		0.20	Mike McLagan	More conservative on which packets
 *					are returned for retry and which are
 *					are dropped.  If DLCI_RET_DROP is
 *					returned from the FRAD, the packet is
 *				 	sent back to Linux for re-transmission
 *		0.25	Mike McLagan	Converted to use SIOC IOCTL calls
 *		0.30	Jim Freeman	Fixed to allow IPX traffic
 *		0.35	Michael Elizabeth	Fixed incorrect memcpy_fromfs
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

#include <linux/config.h> /* for CONFIG_DLCI_COUNT */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/in.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/if_frad.h>

#include <net/sock.h>

#include <asm/system.h>
#include <asm/bitops.h>
#include <asm/io.h>
#include <asm/dma.h>
#include <asm/uaccess.h>

static const char version[] = "DLCI driver v0.35, 4 Jan 1997, mike.mclagan@linux.org";

static LIST_HEAD(dlci_devs);

static void dlci_setup(struct net_device *);

/* 
 * these encapsulate the RFC 1490 requirements as well as 
 * deal with packet transmission and reception, working with
 * the upper network layers 
 */

static int dlci_header(struct sk_buff *skb, struct net_device *dev, 
                           unsigned short type, void *daddr, void *saddr, 
                           unsigned len)
{
	struct frhdr		hdr;
	struct dlci_local	*dlp;
	unsigned int		hlen;
	char			*dest;

	dlp = dev->priv;

	hdr.control = FRAD_I_UI;
	switch(type)
	{
		case ETH_P_IP:
			hdr.IP_NLPID = FRAD_P_IP;
			hlen = sizeof(hdr.control) + sizeof(hdr.IP_NLPID);
			break;

		/* feel free to add other types, if necessary */

		default:
			hdr.pad = FRAD_P_PADDING;
			hdr.NLPID = FRAD_P_SNAP;
			memset(hdr.OUI, 0, sizeof(hdr.OUI));
			hdr.PID = htons(type);
			hlen = sizeof(hdr);
			break;
	}

	dest = skb_push(skb, hlen);
	if (!dest)
		return(0);

	memcpy(dest, &hdr, hlen);

	return(hlen);
}

static void dlci_receive(struct sk_buff *skb, struct net_device *dev)
{
	struct dlci_local *dlp;
	struct frhdr		*hdr;
	int					process, header;

	dlp = dev->priv;
	if (!pskb_may_pull(skb, sizeof(*hdr))) {
		printk(KERN_NOTICE "%s: invalid data no header\n",
		       dev->name);
		dlp->stats.rx_errors++;
		kfree_skb(skb);
		return;
	}

	hdr = (struct frhdr *) skb->data;
	process = 0;
	header = 0;
	skb->dev = dev;

	if (hdr->control != FRAD_I_UI)
	{
		printk(KERN_NOTICE "%s: Invalid header flag 0x%02X.\n", dev->name, hdr->control);
		dlp->stats.rx_errors++;
	}
	else
		switch(hdr->IP_NLPID)
		{
			case FRAD_P_PADDING:
				if (hdr->NLPID != FRAD_P_SNAP)
				{
					printk(KERN_NOTICE "%s: Unsupported NLPID 0x%02X.\n", dev->name, hdr->NLPID);
					dlp->stats.rx_errors++;
					break;
				}
	 
				if (hdr->OUI[0] + hdr->OUI[1] + hdr->OUI[2] != 0)
				{
					printk(KERN_NOTICE "%s: Unsupported organizationally unique identifier 0x%02X-%02X-%02X.\n", dev->name, hdr->OUI[0], hdr->OUI[1], hdr->OUI[2]);
					dlp->stats.rx_errors++;
					break;
				}

				/* at this point, it's an EtherType frame */
				header = sizeof(struct frhdr);
				/* Already in network order ! */
				skb->protocol = hdr->PID;
				process = 1;
				break;

			case FRAD_P_IP:
				header = sizeof(hdr->control) + sizeof(hdr->IP_NLPID);
				skb->protocol = htons(ETH_P_IP);
				process = 1;
				break;

			case FRAD_P_SNAP:
			case FRAD_P_Q933:
			case FRAD_P_CLNP:
				printk(KERN_NOTICE "%s: Unsupported NLPID 0x%02X.\n", dev->name, hdr->pad);
				dlp->stats.rx_errors++;
				break;

			default:
				printk(KERN_NOTICE "%s: Invalid pad byte 0x%02X.\n", dev->name, hdr->pad);
				dlp->stats.rx_errors++;
				break;				
		}

	if (process)
	{
		/* we've set up the protocol, so discard the header */
		skb->mac.raw = skb->data; 
		skb_pull(skb, header);
		dlp->stats.rx_bytes += skb->len;
		netif_rx(skb);
		dlp->stats.rx_packets++;
		dev->last_rx = jiffies;
	}
	else
		dev_kfree_skb(skb);
}

static int dlci_transmit(struct sk_buff *skb, struct net_device *dev)
{
	struct dlci_local *dlp;
	int					ret;

	ret = 0;

	if (!skb || !dev)
		return(0);

	dlp = dev->priv;

	netif_stop_queue(dev);
	
	ret = dlp->slave->hard_start_xmit(skb, dlp->slave);
	switch (ret)
	{
		case DLCI_RET_OK:
			dlp->stats.tx_packets++;
			ret = 0;
			break;
			case DLCI_RET_ERR:
			dlp->stats.tx_errors++;
			ret = 0;
			break;
			case DLCI_RET_DROP:
			dlp->stats.tx_dropped++;
			ret = 1;
			break;
	}
	/* Alan Cox recommends always returning 0, and always freeing the packet */
	/* experience suggest a slightly more conservative approach */

	if (!ret)
	{
		dev_kfree_skb(skb);
		netif_wake_queue(dev);
	}
	return(ret);
}

static int dlci_config(struct net_device *dev, struct dlci_conf __user *conf, int get)
{
	struct dlci_conf	config;
	struct dlci_local	*dlp;
	struct frad_local	*flp;
	int			err;

	dlp = dev->priv;

	flp = dlp->slave->priv;

	if (!get)
	{
		if(copy_from_user(&config, conf, sizeof(struct dlci_conf)))
			return -EFAULT;
		if (config.flags & ~DLCI_VALID_FLAGS)
			return(-EINVAL);
		memcpy(&dlp->config, &config, sizeof(struct dlci_conf));
		dlp->configured = 1;
	}

	err = (*flp->dlci_conf)(dlp->slave, dev, get);
	if (err)
		return(err);

	if (get)
	{
		if(copy_to_user(conf, &dlp->config, sizeof(struct dlci_conf)))
			return -EFAULT;
	}

	return(0);
}

static int dlci_dev_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	struct dlci_local *dlp;

	if (!capable(CAP_NET_ADMIN))
		return(-EPERM);

	dlp = dev->priv;

	switch(cmd)
	{
		case DLCI_GET_SLAVE:
			if (!*(short *)(dev->dev_addr))
				return(-EINVAL);

			strncpy(ifr->ifr_slave, dlp->slave->name, sizeof(ifr->ifr_slave));
			break;

		case DLCI_GET_CONF:
		case DLCI_SET_CONF:
			if (!*(short *)(dev->dev_addr))
				return(-EINVAL);

			return(dlci_config(dev, ifr->ifr_data, cmd == DLCI_GET_CONF));
			break;

		default: 
			return(-EOPNOTSUPP);
	}
	return(0);
}

static int dlci_change_mtu(struct net_device *dev, int new_mtu)
{
	struct dlci_local *dlp;

	dlp = dev->priv;

	return((*dlp->slave->change_mtu)(dlp->slave, new_mtu));
}

static int dlci_open(struct net_device *dev)
{
	struct dlci_local	*dlp;
	struct frad_local	*flp;
	int			err;

	dlp = dev->priv;

	if (!*(short *)(dev->dev_addr))
		return(-EINVAL);

	if (!netif_running(dlp->slave))
		return(-ENOTCONN);

	flp = dlp->slave->priv;
	err = (*flp->activate)(dlp->slave, dev);
	if (err)
		return(err);

	netif_start_queue(dev);

	return 0;
}

static int dlci_close(struct net_device *dev)
{
	struct dlci_local	*dlp;
	struct frad_local	*flp;
	int			err;

	netif_stop_queue(dev);

	dlp = dev->priv;

	flp = dlp->slave->priv;
	err = (*flp->deactivate)(dlp->slave, dev);

	return 0;
}

static struct net_device_stats *dlci_get_stats(struct net_device *dev)
{
	struct dlci_local *dlp;

	dlp = dev->priv;

	return(&dlp->stats);
}

static int dlci_add(struct dlci_add *dlci)
{
	struct net_device	*master, *slave;
	struct dlci_local	*dlp;
	struct frad_local	*flp;
	int			err = -EINVAL;


	/* validate slave device */
	slave = dev_get_by_name(dlci->devname);
	if (!slave)
		return -ENODEV;

	if (slave->type != ARPHRD_FRAD || slave->priv == NULL)
		goto err1;

	/* create device name */
	master = alloc_netdev( sizeof(struct dlci_local), "dlci%d",
			      dlci_setup);
	if (!master) {
		err = -ENOMEM;
		goto err1;
	}

	/* make sure same slave not already registered */
	rtnl_lock();
	list_for_each_entry(dlp, &dlci_devs, list) {
		if (dlp->slave == slave) {
			err = -EBUSY;
			goto err2;
		}
	}

	err = dev_alloc_name(master, master->name);
	if (err < 0)
		goto err2;

	*(short *)(master->dev_addr) = dlci->dlci;

	dlp = (struct dlci_local *) master->priv;
	dlp->slave = slave;
	dlp->master = master;

	flp = slave->priv;
	err = (*flp->assoc)(slave, master);
	if (err < 0)
		goto err2;

	err = register_netdevice(master);
	if (err < 0) 
		goto err2;

	strcpy(dlci->devname, master->name);

	list_add(&dlp->list, &dlci_devs);
	rtnl_unlock();

	return(0);

 err2:
	rtnl_unlock();
	free_netdev(master);
 err1:
	dev_put(slave);
	return(err);
}

static int dlci_del(struct dlci_add *dlci)
{
	struct dlci_local	*dlp;
	struct frad_local	*flp;
	struct net_device	*master, *slave;
	int			err;

	/* validate slave device */
	master = __dev_get_by_name(dlci->devname);
	if (!master)
		return(-ENODEV);

	if (netif_running(master)) {
		return(-EBUSY);
	}

	dlp = master->priv;
	slave = dlp->slave;
	flp = slave->priv;

	rtnl_lock();
	err = (*flp->deassoc)(slave, master);
	if (!err) {
		list_del(&dlp->list);

		unregister_netdevice(master);

		dev_put(slave);
	}
	rtnl_unlock();

	return(err);
}

static int dlci_ioctl(unsigned int cmd, void __user *arg)
{
	struct dlci_add add;
	int err;
	
	if (!capable(CAP_NET_ADMIN))
		return(-EPERM);

	if(copy_from_user(&add, arg, sizeof(struct dlci_add)))
		return -EFAULT;

	switch (cmd)
	{
		case SIOCADDDLCI:
			err = dlci_add(&add);

			if (!err)
				if(copy_to_user(arg, &add, sizeof(struct dlci_add)))
					return -EFAULT;
			break;

		case SIOCDELDLCI:
			err = dlci_del(&add);
			break;

		default:
			err = -EINVAL;
	}

	return(err);
}

static void dlci_setup(struct net_device *dev)
{
	struct dlci_local *dlp = dev->priv;

	dev->flags		= 0;
	dev->open		= dlci_open;
	dev->stop		= dlci_close;
	dev->do_ioctl		= dlci_dev_ioctl;
	dev->hard_start_xmit	= dlci_transmit;
	dev->hard_header	= dlci_header;
	dev->get_stats		= dlci_get_stats;
	dev->change_mtu		= dlci_change_mtu;
	dev->destructor		= free_netdev;

	dlp->receive		= dlci_receive;

	dev->type		= ARPHRD_DLCI;
	dev->hard_header_len	= sizeof(struct frhdr);
	dev->addr_len		= sizeof(short);

}

/* if slave is unregistering, then cleanup master */
static int dlci_dev_event(struct notifier_block *unused,
			  unsigned long event, void *ptr)
{
	struct net_device *dev = (struct net_device *) ptr;

	if (event == NETDEV_UNREGISTER) {
		struct dlci_local *dlp;

		list_for_each_entry(dlp, &dlci_devs, list) {
			if (dlp->slave == dev) {
				list_del(&dlp->list);
				unregister_netdevice(dlp->master);
				dev_put(dlp->slave);
				break;
			}
		}
	}
	return NOTIFY_DONE;
}

static struct notifier_block dlci_notifier = {
	.notifier_call = dlci_dev_event,
};

static int __init init_dlci(void)
{
	dlci_ioctl_set(dlci_ioctl);
	register_netdevice_notifier(&dlci_notifier);

	printk("%s.\n", version);

	return 0;
}

static void __exit dlci_exit(void)
{
	struct dlci_local	*dlp, *nxt;
	
	dlci_ioctl_set(NULL);
	unregister_netdevice_notifier(&dlci_notifier);

	rtnl_lock();
	list_for_each_entry_safe(dlp, nxt, &dlci_devs, list) {
		unregister_netdevice(dlp->master);
		dev_put(dlp->slave);
	}
	rtnl_unlock();
}

module_init(init_dlci);
module_exit(dlci_exit);

MODULE_AUTHOR("Mike McLagan");
MODULE_DESCRIPTION("Frame Relay DLCI layer");
MODULE_LICENSE("GPL");
