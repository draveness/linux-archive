/*
 * Common framework for low-level network console, dump, and debugger code
 *
 * Sep 8 2003  Matt Mackall <mpm@selenic.com>
 *
 * based on the netconsole code from:
 *
 * Copyright (C) 2001  Ingo Molnar <mingo@redhat.com>
 * Copyright (C) 2002  Red Hat, Inc.
 */

#include <linux/smp_lock.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/string.h>
#include <linux/inetdevice.h>
#include <linux/inet.h>
#include <linux/interrupt.h>
#include <linux/netpoll.h>
#include <linux/sched.h>
#include <net/tcp.h>
#include <net/udp.h>

/*
 * We maintain a small pool of fully-sized skbs, to make sure the
 * message gets out even in extreme OOM situations.
 */

#define MAX_SKBS 32
#define MAX_UDP_CHUNK 1460

static spinlock_t skb_list_lock = SPIN_LOCK_UNLOCKED;
static int nr_skbs;
static struct sk_buff *skbs;

static spinlock_t rx_list_lock = SPIN_LOCK_UNLOCKED;
static LIST_HEAD(rx_list);

static int trapped;

#define MAX_SKB_SIZE \
		(MAX_UDP_CHUNK + sizeof(struct udphdr) + \
				sizeof(struct iphdr) + sizeof(struct ethhdr))

static void zap_completion_queue(void);

static int checksum_udp(struct sk_buff *skb, struct udphdr *uh,
			     unsigned short ulen, u32 saddr, u32 daddr)
{
	if (uh->check == 0)
		return 0;

	if (skb->ip_summed == CHECKSUM_HW)
		return csum_tcpudp_magic(
			saddr, daddr, ulen, IPPROTO_UDP, skb->csum);

	skb->csum = csum_tcpudp_nofold(saddr, daddr, ulen, IPPROTO_UDP, 0);

	return csum_fold(skb_checksum(skb, 0, skb->len, skb->csum));
}

void netpoll_poll(struct netpoll *np)
{
	int budget = 1;

	if(!np->dev || !netif_running(np->dev) || !np->dev->poll_controller)
		return;

	/* Process pending work on NIC */
	np->dev->poll_controller(np->dev);

	/* If scheduling is stopped, tickle NAPI bits */
	if(trapped && np->dev->poll &&
	   test_bit(__LINK_STATE_RX_SCHED, &np->dev->state))
		np->dev->poll(np->dev, &budget);
	zap_completion_queue();
}

static void refill_skbs(void)
{
	struct sk_buff *skb;
	unsigned long flags;

	spin_lock_irqsave(&skb_list_lock, flags);
	while (nr_skbs < MAX_SKBS) {
		skb = alloc_skb(MAX_SKB_SIZE, GFP_ATOMIC);
		if (!skb)
			break;

		skb->next = skbs;
		skbs = skb;
		nr_skbs++;
	}
	spin_unlock_irqrestore(&skb_list_lock, flags);
}

static void zap_completion_queue(void)
{
	unsigned long flags;
	struct softnet_data *sd = &get_cpu_var(softnet_data);

	if (sd->completion_queue) {
		struct sk_buff *clist;

		local_irq_save(flags);
		clist = sd->completion_queue;
		sd->completion_queue = NULL;
		local_irq_restore(flags);

		while (clist != NULL) {
			struct sk_buff *skb = clist;
			clist = clist->next;
			__kfree_skb(skb);
		}
	}

	put_cpu_var(softnet_data);
}

static struct sk_buff * find_skb(struct netpoll *np, int len, int reserve)
{
	int once = 1, count = 0;
	unsigned long flags;
	struct sk_buff *skb = NULL;

	zap_completion_queue();
repeat:
	if (nr_skbs < MAX_SKBS)
		refill_skbs();

	skb = alloc_skb(len, GFP_ATOMIC);

	if (!skb) {
		spin_lock_irqsave(&skb_list_lock, flags);
		skb = skbs;
		if (skb)
			skbs = skb->next;
		skb->next = NULL;
		nr_skbs--;
		spin_unlock_irqrestore(&skb_list_lock, flags);
	}

	if(!skb) {
		count++;
		if (once && (count == 1000000)) {
			printk("out of netpoll skbs!\n");
			once = 0;
		}
		netpoll_poll(np);
		goto repeat;
	}

	atomic_set(&skb->users, 1);
	skb_reserve(skb, reserve);
	return skb;
}

void netpoll_send_skb(struct netpoll *np, struct sk_buff *skb)
{
	int status;

repeat:
	if(!np || !np->dev || !netif_running(np->dev)) {
		__kfree_skb(skb);
		return;
	}

	spin_lock(&np->dev->xmit_lock);
	np->dev->xmit_lock_owner = smp_processor_id();

	status = np->dev->hard_start_xmit(skb, np->dev);
	np->dev->xmit_lock_owner = -1;
	spin_unlock(&np->dev->xmit_lock);

	/* transmit busy */
	if(status) {
		netpoll_poll(np);
		goto repeat;
	}
}

void netpoll_send_udp(struct netpoll *np, const char *msg, int len)
{
	int total_len, eth_len, ip_len, udp_len;
	struct sk_buff *skb;
	struct udphdr *udph;
	struct iphdr *iph;
	struct ethhdr *eth;

	udp_len = len + sizeof(*udph);
	ip_len = eth_len = udp_len + sizeof(*iph);
	total_len = eth_len + ETH_HLEN;

	skb = find_skb(np, total_len, total_len - len);
	if (!skb)
		return;

	memcpy(skb->data, msg, len);
	skb->len += len;

	udph = (struct udphdr *) skb_push(skb, sizeof(*udph));
	udph->source = htons(np->local_port);
	udph->dest = htons(np->remote_port);
	udph->len = htons(udp_len);
	udph->check = 0;

	iph = (struct iphdr *)skb_push(skb, sizeof(*iph));

	iph->version  = 4;
	iph->ihl      = 5;
	iph->tos      = 0;
	iph->tot_len  = htons(ip_len);
	iph->id       = 0;
	iph->frag_off = 0;
	iph->ttl      = 64;
	iph->protocol = IPPROTO_UDP;
	iph->check    = 0;
	iph->saddr    = htonl(np->local_ip);
	iph->daddr    = htonl(np->remote_ip);
	iph->check    = ip_fast_csum((unsigned char *)iph, iph->ihl);

	eth = (struct ethhdr *) skb_push(skb, ETH_HLEN);

	eth->h_proto = htons(ETH_P_IP);
	memcpy(eth->h_source, np->local_mac, 6);
	memcpy(eth->h_dest, np->remote_mac, 6);

	netpoll_send_skb(np, skb);
}

static void arp_reply(struct sk_buff *skb)
{
	struct arphdr *arp;
	unsigned char *arp_ptr;
	int size, type = ARPOP_REPLY, ptype = ETH_P_ARP;
	u32 sip, tip;
	struct sk_buff *send_skb;
	unsigned long flags;
	struct list_head *p;
	struct netpoll *np = NULL;

	spin_lock_irqsave(&rx_list_lock, flags);
	list_for_each(p, &rx_list) {
		np = list_entry(p, struct netpoll, rx_list);
		if ( np->dev == skb->dev )
			break;
		np = NULL;
	}
	spin_unlock_irqrestore(&rx_list_lock, flags);

	if (!np) return;

	/* No arp on this interface */
	if (skb->dev->flags & IFF_NOARP)
		return;

	if (!pskb_may_pull(skb, (sizeof(struct arphdr) +
				 (2 * skb->dev->addr_len) +
				 (2 * sizeof(u32)))))
		return;

	skb->h.raw = skb->nh.raw = skb->data;
	arp = skb->nh.arph;

	if ((arp->ar_hrd != htons(ARPHRD_ETHER) &&
	     arp->ar_hrd != htons(ARPHRD_IEEE802)) ||
	    arp->ar_pro != htons(ETH_P_IP) ||
	    arp->ar_op != htons(ARPOP_REQUEST))
		return;

	arp_ptr = (unsigned char *)(arp+1) + skb->dev->addr_len;
	memcpy(&sip, arp_ptr, 4);
	arp_ptr += 4 + skb->dev->addr_len;
	memcpy(&tip, arp_ptr, 4);

	/* Should we ignore arp? */
	if (tip != htonl(np->local_ip) || LOOPBACK(tip) || MULTICAST(tip))
		return;

	size = sizeof(struct arphdr) + 2 * (skb->dev->addr_len + 4);
	send_skb = find_skb(np, size + LL_RESERVED_SPACE(np->dev),
			    LL_RESERVED_SPACE(np->dev));

	if (!send_skb)
		return;

	send_skb->nh.raw = send_skb->data;
	arp = (struct arphdr *) skb_put(send_skb, size);
	send_skb->dev = skb->dev;
	send_skb->protocol = htons(ETH_P_ARP);

	/* Fill the device header for the ARP frame */

	if (np->dev->hard_header &&
	    np->dev->hard_header(send_skb, skb->dev, ptype,
				       np->remote_mac, np->local_mac,
				       send_skb->len) < 0) {
		kfree_skb(send_skb);
		return;
	}

	/*
	 * Fill out the arp protocol part.
	 *
	 * we only support ethernet device type,
	 * which (according to RFC 1390) should always equal 1 (Ethernet).
	 */

	arp->ar_hrd = htons(np->dev->type);
	arp->ar_pro = htons(ETH_P_IP);
	arp->ar_hln = np->dev->addr_len;
	arp->ar_pln = 4;
	arp->ar_op = htons(type);

	arp_ptr=(unsigned char *)(arp + 1);
	memcpy(arp_ptr, np->dev->dev_addr, np->dev->addr_len);
	arp_ptr += np->dev->addr_len;
	memcpy(arp_ptr, &tip, 4);
	arp_ptr += 4;
	memcpy(arp_ptr, np->remote_mac, np->dev->addr_len);
	arp_ptr += np->dev->addr_len;
	memcpy(arp_ptr, &sip, 4);

	netpoll_send_skb(np, send_skb);
}

int netpoll_rx(struct sk_buff *skb)
{
	int proto, len, ulen;
	struct iphdr *iph;
	struct udphdr *uh;
	struct netpoll *np;
	struct list_head *p;
	unsigned long flags;

	if (skb->dev->type != ARPHRD_ETHER)
		goto out;

	/* check if netpoll clients need ARP */
	if (skb->protocol == __constant_htons(ETH_P_ARP) && trapped) {
		arp_reply(skb);
		return 1;
	}

	proto = ntohs(skb->mac.ethernet->h_proto);
	if (proto != ETH_P_IP)
		goto out;
	if (skb->pkt_type == PACKET_OTHERHOST)
		goto out;
	if (skb_shared(skb))
		goto out;

	iph = (struct iphdr *)skb->data;
	if (!pskb_may_pull(skb, sizeof(struct iphdr)))
		goto out;
	if (iph->ihl < 5 || iph->version != 4)
		goto out;
	if (!pskb_may_pull(skb, iph->ihl*4))
		goto out;
	if (ip_fast_csum((u8 *)iph, iph->ihl) != 0)
		goto out;

	len = ntohs(iph->tot_len);
	if (skb->len < len || len < iph->ihl*4)
		goto out;

	if (iph->protocol != IPPROTO_UDP)
		goto out;

	len -= iph->ihl*4;
	uh = (struct udphdr *)(((char *)iph) + iph->ihl*4);
	ulen = ntohs(uh->len);

	if (ulen != len)
		goto out;
	if (checksum_udp(skb, uh, ulen, iph->saddr, iph->daddr) < 0)
		goto out;

	spin_lock_irqsave(&rx_list_lock, flags);
	list_for_each(p, &rx_list) {
		np = list_entry(p, struct netpoll, rx_list);
		if (np->dev && np->dev != skb->dev)
			continue;
		if (np->local_ip && np->local_ip != ntohl(iph->daddr))
			continue;
		if (np->remote_ip && np->remote_ip != ntohl(iph->saddr))
			continue;
		if (np->local_port && np->local_port != ntohs(uh->dest))
			continue;

		spin_unlock_irqrestore(&rx_list_lock, flags);

		if (np->rx_hook)
			np->rx_hook(np, ntohs(uh->source),
				    (char *)(uh+1),
				    ulen - sizeof(struct udphdr));

		return 1;
	}
	spin_unlock_irqrestore(&rx_list_lock, flags);

out:
	return trapped;
}

int netpoll_parse_options(struct netpoll *np, char *opt)
{
	char *cur=opt, *delim;

	if(*cur != '@') {
		if ((delim = strchr(cur, '@')) == NULL)
			goto parse_failed;
		*delim=0;
		np->local_port=simple_strtol(cur, NULL, 10);
		cur=delim;
	}
	cur++;
	printk(KERN_INFO "%s: local port %d\n", np->name, np->local_port);

	if(*cur != '/') {
		if ((delim = strchr(cur, '/')) == NULL)
			goto parse_failed;
		*delim=0;
		np->local_ip=ntohl(in_aton(cur));
		cur=delim;

		printk(KERN_INFO "%s: local IP %d.%d.%d.%d\n",
		       np->name, HIPQUAD(np->local_ip));
	}
	cur++;

	if ( *cur != ',') {
		/* parse out dev name */
		if ((delim = strchr(cur, ',')) == NULL)
			goto parse_failed;
		*delim=0;
		strlcpy(np->dev_name, cur, sizeof(np->dev_name));
		cur=delim;
	}
	cur++;

	printk(KERN_INFO "%s: interface %s\n", np->name, np->dev_name);

	if ( *cur != '@' ) {
		/* dst port */
		if ((delim = strchr(cur, '@')) == NULL)
			goto parse_failed;
		*delim=0;
		np->remote_port=simple_strtol(cur, NULL, 10);
		cur=delim;
	}
	cur++;
	printk(KERN_INFO "%s: remote port %d\n", np->name, np->remote_port);

	/* dst ip */
	if ((delim = strchr(cur, '/')) == NULL)
		goto parse_failed;
	*delim=0;
	np->remote_ip=ntohl(in_aton(cur));
	cur=delim+1;

	printk(KERN_INFO "%s: remote IP %d.%d.%d.%d\n",
		       np->name, HIPQUAD(np->remote_ip));

	if( *cur != 0 )
	{
		/* MAC address */
		if ((delim = strchr(cur, ':')) == NULL)
			goto parse_failed;
		*delim=0;
		np->remote_mac[0]=simple_strtol(cur, NULL, 16);
		cur=delim+1;
		if ((delim = strchr(cur, ':')) == NULL)
			goto parse_failed;
		*delim=0;
		np->remote_mac[1]=simple_strtol(cur, NULL, 16);
		cur=delim+1;
		if ((delim = strchr(cur, ':')) == NULL)
			goto parse_failed;
		*delim=0;
		np->remote_mac[2]=simple_strtol(cur, NULL, 16);
		cur=delim+1;
		if ((delim = strchr(cur, ':')) == NULL)
			goto parse_failed;
		*delim=0;
		np->remote_mac[3]=simple_strtol(cur, NULL, 16);
		cur=delim+1;
		if ((delim = strchr(cur, ':')) == NULL)
			goto parse_failed;
		*delim=0;
		np->remote_mac[4]=simple_strtol(cur, NULL, 16);
		cur=delim+1;
		np->remote_mac[5]=simple_strtol(cur, NULL, 16);
	}

	printk(KERN_INFO "%s: remote ethernet address "
	       "%02x:%02x:%02x:%02x:%02x:%02x\n",
	       np->name,
	       np->remote_mac[0],
	       np->remote_mac[1],
	       np->remote_mac[2],
	       np->remote_mac[3],
	       np->remote_mac[4],
	       np->remote_mac[5]);

	return 0;

 parse_failed:
	printk(KERN_INFO "%s: couldn't parse config at %s!\n",
	       np->name, cur);
	return -1;
}

int netpoll_setup(struct netpoll *np)
{
	struct net_device *ndev = NULL;
	struct in_device *in_dev;

	if (np->dev_name)
		ndev = dev_get_by_name(np->dev_name);
	if (!ndev) {
		printk(KERN_ERR "%s: %s doesn't exist, aborting.\n",
		       np->name, np->dev_name);
		return -1;
	}
	if (!ndev->poll_controller) {
		printk(KERN_ERR "%s: %s doesn't support polling, aborting.\n",
		       np->name, np->dev_name);
		goto release;
	}

	if (!(ndev->flags & IFF_UP)) {
		unsigned short oflags;
		unsigned long atmost, atleast;

		printk(KERN_INFO "%s: device %s not up yet, forcing it\n",
		       np->name, np->dev_name);

		oflags = ndev->flags;

		rtnl_shlock();
		if (dev_change_flags(ndev, oflags | IFF_UP) < 0) {
			printk(KERN_ERR "%s: failed to open %s\n",
			       np->name, np->dev_name);
			rtnl_shunlock();
			goto release;
		}
		rtnl_shunlock();

		atleast = jiffies + HZ/10;
 		atmost = jiffies + 10*HZ;
		while (!netif_carrier_ok(ndev)) {
			if (time_after(jiffies, atmost)) {
				printk(KERN_NOTICE
				       "%s: timeout waiting for carrier\n",
				       np->name);
				break;
			}
			cond_resched();
		}

		if (time_before(jiffies, atleast)) {
			printk(KERN_NOTICE "%s: carrier detect appears flaky,"
			       " waiting 10 seconds\n",
			       np->name);
			while (time_before(jiffies, atmost))
				cond_resched();
		}
	}

	if (!memcmp(np->local_mac, "\0\0\0\0\0\0", 6) && ndev->dev_addr)
		memcpy(np->local_mac, ndev->dev_addr, 6);

	if (!np->local_ip) {
		in_dev = in_dev_get(ndev);

		if (!in_dev) {
			printk(KERN_ERR "%s: no IP address for %s, aborting\n",
			       np->name, np->dev_name);
			goto release;
		}

		np->local_ip = ntohl(in_dev->ifa_list->ifa_local);
		in_dev_put(in_dev);
		printk(KERN_INFO "%s: local IP %d.%d.%d.%d\n",
		       np->name, HIPQUAD(np->local_ip));
	}

	np->dev = ndev;

	if(np->rx_hook) {
		unsigned long flags;

#ifdef CONFIG_NETPOLL_RX
		np->dev->netpoll_rx = 1;
#endif

		spin_lock_irqsave(&rx_list_lock, flags);
		list_add(&np->rx_list, &rx_list);
		spin_unlock_irqrestore(&rx_list_lock, flags);
	}

	return 0;
 release:
	dev_put(ndev);
	return -1;
}

void netpoll_cleanup(struct netpoll *np)
{
	if(np->rx_hook) {
		unsigned long flags;

		spin_lock_irqsave(&rx_list_lock, flags);
		list_del(&np->rx_list);
#ifdef CONFIG_NETPOLL_RX
		np->dev->netpoll_rx = 0;
#endif
		spin_unlock_irqrestore(&rx_list_lock, flags);
	}

	dev_put(np->dev);
	np->dev = NULL;
}

int netpoll_trap(void)
{
	return trapped;
}

void netpoll_set_trap(int trap)
{
	trapped = trap;
}

EXPORT_SYMBOL(netpoll_set_trap);
EXPORT_SYMBOL(netpoll_trap);
EXPORT_SYMBOL(netpoll_parse_options);
EXPORT_SYMBOL(netpoll_setup);
EXPORT_SYMBOL(netpoll_cleanup);
EXPORT_SYMBOL(netpoll_send_skb);
EXPORT_SYMBOL(netpoll_send_udp);
EXPORT_SYMBOL(netpoll_poll);
