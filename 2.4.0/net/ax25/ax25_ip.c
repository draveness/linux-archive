/*
 *	AX.25 release 037
 *
 *	This code REQUIRES 2.1.15 or higher/ NET3.038
 *
 *	This module:
 *		This module is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *	History
 *	AX.25 036	Jonathan(G4KLX)	Split from af_ax25.c.
 */

#include <linux/config.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/sockios.h>
#include <linux/net.h>
#include <net/ax25.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <linux/fcntl.h>
#include <linux/termios.h>	/* For TIOCINQ/OUTQ */
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/notifier.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/netfilter.h>
#include <linux/sysctl.h>
#include <net/ip.h>
#include <net/arp.h>

/*
 *	IP over AX.25 encapsulation.
 */

/*
 *	Shove an AX.25 UI header on an IP packet and handle ARP
 */

#ifdef CONFIG_INET

int ax25_encapsulate(struct sk_buff *skb, struct net_device *dev, unsigned short type, void *daddr, void *saddr, unsigned len)
{
  	/* header is an AX.25 UI frame from us to them */
 	unsigned char *buff = skb_push(skb, AX25_HEADER_LEN);

  	*buff++ = 0x00;	/* KISS DATA */

	if (daddr != NULL)
		memcpy(buff, daddr, dev->addr_len);	/* Address specified */

  	buff[6] &= ~AX25_CBIT;
  	buff[6] &= ~AX25_EBIT;
  	buff[6] |= AX25_SSSID_SPARE;
  	buff    += AX25_ADDR_LEN;

  	if (saddr != NULL)
  		memcpy(buff, saddr, dev->addr_len);
  	else
  		memcpy(buff, dev->dev_addr, dev->addr_len);

  	buff[6] &= ~AX25_CBIT;
  	buff[6] |= AX25_EBIT;
  	buff[6] |= AX25_SSSID_SPARE;
  	buff    += AX25_ADDR_LEN;

  	*buff++  = AX25_UI;	/* UI */

  	/* Append a suitable AX.25 PID */
  	switch (type) {
  		case ETH_P_IP:
  			*buff++ = AX25_P_IP;
 			break;
  		case ETH_P_ARP:
  			*buff++ = AX25_P_ARP;
  			break;
  		default:
  			printk(KERN_ERR "AX.25: ax25_encapsulate - wrong protocol type 0x%x2.2\n", type);
  			*buff++ = 0;
  			break;
 	}

	if (daddr != NULL)
	  	return AX25_HEADER_LEN;

	return -AX25_HEADER_LEN;	/* Unfinished header */
}

int ax25_rebuild_header(struct sk_buff *skb)
{
	struct sk_buff *ourskb;
	unsigned char *bp  = skb->data;
	struct net_device *dev;
	ax25_address *src, *dst;
	ax25_route *route;
	ax25_dev *ax25_dev;

	dst = (ax25_address *)(bp + 1);
	src = (ax25_address *)(bp + 8);

  	if (arp_find(bp + 1, skb))
  		return 1;

	route    = ax25_rt_find_route(dst, NULL);
	dev      = route->dev;

	if (dev == NULL)
		dev = skb->dev;

        if ((ax25_dev = ax25_dev_ax25dev(dev)) == NULL)
                return 1;

	if (bp[16] == AX25_P_IP) {
		if (route->ip_mode == 'V' || (route->ip_mode == ' ' && ax25_dev->values[AX25_VALUES_IPDEFMODE])) {
			/*
			 *	We copy the buffer and release the original thereby
			 *	keeping it straight
			 *
			 *	Note: we report 1 back so the caller will
			 *	not feed the frame direct to the physical device
			 *	We don't want that to happen. (It won't be upset
			 *	as we have pulled the frame from the queue by
			 *	freeing it).
			 *
			 *	NB: TCP modifies buffers that are still
			 *	on a device queue, thus we use skb_copy()
			 *      instead of using skb_clone() unless this
			 *	gets fixed.
			 */

			ax25_address src_c;
			ax25_address dst_c;

			if ((ourskb = skb_copy(skb, GFP_ATOMIC)) == NULL) {
				kfree_skb(skb);
				return 1;
			}

			if (skb->sk != NULL)
				skb_set_owner_w(ourskb, skb->sk);

			kfree_skb(skb);

			src_c = *src;
			dst_c = *dst;

			skb_pull(ourskb, AX25_HEADER_LEN - 1);	/* Keep PID */
			skb->nh.raw = skb->data;

			ax25_send_frame(ourskb, ax25_dev->values[AX25_VALUES_PACLEN], &src_c, 
&dst_c, route->digipeat, dev);

			return 1;
		}
	}

  	bp[7]  &= ~AX25_CBIT;
  	bp[7]  &= ~AX25_EBIT;
  	bp[7]  |= AX25_SSSID_SPARE;

  	bp[14] &= ~AX25_CBIT;
  	bp[14] |= AX25_EBIT;
  	bp[14] |= AX25_SSSID_SPARE;

	skb_pull(skb, AX25_KISS_HEADER_LEN);

	if (route->digipeat != NULL) {
		if ((ourskb = ax25_rt_build_path(skb, src, dst, route->digipeat)) == NULL) {
			kfree_skb(skb);
			return 1;
		}

		skb = ourskb;
	}

	skb->dev      = dev;

	ax25_queue_xmit(skb);

  	return 1;
}

#else	/* INET */

int ax25_encapsulate(struct sk_buff *skb, struct net_device *dev, unsigned short type, void *daddr, void *saddr, unsigned len)
{
	return -AX25_HEADER_LEN;
}

int ax25_rebuild_header(struct sk_buff *skb)
{
	return 1;
}

#endif

