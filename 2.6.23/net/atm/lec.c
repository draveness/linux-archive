/*
 * lec.c: Lan Emulation driver
 *
 * Marko Kiiskila <mkiiskila@yahoo.com>
 */

#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/capability.h>

/* We are ethernet device */
#include <linux/if_ether.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <net/sock.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <asm/byteorder.h>
#include <asm/uaccess.h>
#include <net/arp.h>
#include <net/dst.h>
#include <linux/proc_fs.h>
#include <linux/spinlock.h>
#include <linux/seq_file.h>

/* TokenRing if needed */
#ifdef CONFIG_TR
#include <linux/trdevice.h>
#endif

/* And atm device */
#include <linux/atmdev.h>
#include <linux/atmlec.h>

/* Proxy LEC knows about bridging */
#if defined(CONFIG_BRIDGE) || defined(CONFIG_BRIDGE_MODULE)
#include <linux/if_bridge.h>
#include "../bridge/br_private.h"

static unsigned char bridge_ula_lec[] = { 0x01, 0x80, 0xc2, 0x00, 0x00 };
#endif

/* Modular too */
#include <linux/module.h>
#include <linux/init.h>

#include "lec.h"
#include "lec_arpc.h"
#include "resources.h"

#if 0
#define DPRINTK printk
#else
#define DPRINTK(format,args...)
#endif

#define DUMP_PACKETS 0		/*
				 * 0 = None,
				 * 1 = 30 first bytes
				 * 2 = Whole packet
				 */

#define LEC_UNRES_QUE_LEN 8	/*
				 * number of tx packets to queue for a
				 * single destination while waiting for SVC
				 */

static int lec_open(struct net_device *dev);
static int lec_start_xmit(struct sk_buff *skb, struct net_device *dev);
static int lec_close(struct net_device *dev);
static struct net_device_stats *lec_get_stats(struct net_device *dev);
static void lec_init(struct net_device *dev);
static struct lec_arp_table *lec_arp_find(struct lec_priv *priv,
					  unsigned char *mac_addr);
static int lec_arp_remove(struct lec_priv *priv,
			  struct lec_arp_table *to_remove);
/* LANE2 functions */
static void lane2_associate_ind(struct net_device *dev, u8 *mac_address,
				u8 *tlvs, u32 sizeoftlvs);
static int lane2_resolve(struct net_device *dev, u8 *dst_mac, int force,
			 u8 **tlvs, u32 *sizeoftlvs);
static int lane2_associate_req(struct net_device *dev, u8 *lan_dst,
			       u8 *tlvs, u32 sizeoftlvs);

static int lec_addr_delete(struct lec_priv *priv, unsigned char *atm_addr,
			   unsigned long permanent);
static void lec_arp_check_empties(struct lec_priv *priv,
				  struct atm_vcc *vcc, struct sk_buff *skb);
static void lec_arp_destroy(struct lec_priv *priv);
static void lec_arp_init(struct lec_priv *priv);
static struct atm_vcc *lec_arp_resolve(struct lec_priv *priv,
				       unsigned char *mac_to_find,
				       int is_rdesc,
				       struct lec_arp_table **ret_entry);
static void lec_arp_update(struct lec_priv *priv, unsigned char *mac_addr,
			   unsigned char *atm_addr, unsigned long remoteflag,
			   unsigned int targetless_le_arp);
static void lec_flush_complete(struct lec_priv *priv, unsigned long tran_id);
static int lec_mcast_make(struct lec_priv *priv, struct atm_vcc *vcc);
static void lec_set_flush_tran_id(struct lec_priv *priv,
				  unsigned char *atm_addr,
				  unsigned long tran_id);
static void lec_vcc_added(struct lec_priv *priv, struct atmlec_ioc *ioc_data,
			  struct atm_vcc *vcc,
			  void (*old_push) (struct atm_vcc *vcc,
					    struct sk_buff *skb));
static void lec_vcc_close(struct lec_priv *priv, struct atm_vcc *vcc);

/* must be done under lec_arp_lock */
static inline void lec_arp_hold(struct lec_arp_table *entry)
{
	atomic_inc(&entry->usage);
}

static inline void lec_arp_put(struct lec_arp_table *entry)
{
	if (atomic_dec_and_test(&entry->usage))
		kfree(entry);
}


static struct lane2_ops lane2_ops = {
	lane2_resolve,		/* resolve,             spec 3.1.3 */
	lane2_associate_req,	/* associate_req,       spec 3.1.4 */
	NULL			/* associate indicator, spec 3.1.5 */
};

static unsigned char bus_mac[ETH_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

/* Device structures */
static struct net_device *dev_lec[MAX_LEC_ITF];

#if defined(CONFIG_BRIDGE) || defined(CONFIG_BRIDGE_MODULE)
static void lec_handle_bridge(struct sk_buff *skb, struct net_device *dev)
{
	struct ethhdr *eth;
	char *buff;
	struct lec_priv *priv;

	/*
	 * Check if this is a BPDU. If so, ask zeppelin to send
	 * LE_TOPOLOGY_REQUEST with the same value of Topology Change bit
	 * as the Config BPDU has
	 */
	eth = (struct ethhdr *)skb->data;
	buff = skb->data + skb->dev->hard_header_len;
	if (*buff++ == 0x42 && *buff++ == 0x42 && *buff++ == 0x03) {
		struct sock *sk;
		struct sk_buff *skb2;
		struct atmlec_msg *mesg;

		skb2 = alloc_skb(sizeof(struct atmlec_msg), GFP_ATOMIC);
		if (skb2 == NULL)
			return;
		skb2->len = sizeof(struct atmlec_msg);
		mesg = (struct atmlec_msg *)skb2->data;
		mesg->type = l_topology_change;
		buff += 4;
		mesg->content.normal.flag = *buff & 0x01;	/* 0x01 is topology change */

		priv = (struct lec_priv *)dev->priv;
		atm_force_charge(priv->lecd, skb2->truesize);
		sk = sk_atm(priv->lecd);
		skb_queue_tail(&sk->sk_receive_queue, skb2);
		sk->sk_data_ready(sk, skb2->len);
	}

	return;
}
#endif /* defined(CONFIG_BRIDGE) || defined(CONFIG_BRIDGE_MODULE) */

/*
 * Modelled after tr_type_trans
 * All multicast and ARE or STE frames go to BUS.
 * Non source routed frames go by destination address.
 * Last hop source routed frames go by destination address.
 * Not last hop source routed frames go by _next_ route descriptor.
 * Returns pointer to destination MAC address or fills in rdesc
 * and returns NULL.
 */
#ifdef CONFIG_TR
static unsigned char *get_tr_dst(unsigned char *packet, unsigned char *rdesc)
{
	struct trh_hdr *trh;
	int riflen, num_rdsc;

	trh = (struct trh_hdr *)packet;
	if (trh->daddr[0] & (uint8_t) 0x80)
		return bus_mac;	/* multicast */

	if (trh->saddr[0] & TR_RII) {
		riflen = (ntohs(trh->rcf) & TR_RCF_LEN_MASK) >> 8;
		if ((ntohs(trh->rcf) >> 13) != 0)
			return bus_mac;	/* ARE or STE */
	} else
		return trh->daddr;	/* not source routed */

	if (riflen < 6)
		return trh->daddr;	/* last hop, source routed */

	/* riflen is 6 or more, packet has more than one route descriptor */
	num_rdsc = (riflen / 2) - 1;
	memset(rdesc, 0, ETH_ALEN);
	/* offset 4 comes from LAN destination field in LE control frames */
	if (trh->rcf & htons((uint16_t) TR_RCF_DIR_BIT))
		memcpy(&rdesc[4], &trh->rseg[num_rdsc - 2], sizeof(__be16));
	else {
		memcpy(&rdesc[4], &trh->rseg[1], sizeof(__be16));
		rdesc[5] = ((ntohs(trh->rseg[0]) & 0x000f) | (rdesc[5] & 0xf0));
	}

	return NULL;
}
#endif /* CONFIG_TR */

/*
 * Open/initialize the netdevice. This is called (in the current kernel)
 * sometime after booting when the 'ifconfig' program is run.
 *
 * This routine should set everything up anew at each open, even
 * registers that "should" only need to be set once at boot, so that
 * there is non-reboot way to recover if something goes wrong.
 */

static int lec_open(struct net_device *dev)
{
	struct lec_priv *priv = (struct lec_priv *)dev->priv;

	netif_start_queue(dev);
	memset(&priv->stats, 0, sizeof(struct net_device_stats));

	return 0;
}

static __inline__ void
lec_send(struct atm_vcc *vcc, struct sk_buff *skb, struct lec_priv *priv)
{
	ATM_SKB(skb)->vcc = vcc;
	ATM_SKB(skb)->atm_options = vcc->atm_options;

	atomic_add(skb->truesize, &sk_atm(vcc)->sk_wmem_alloc);
	if (vcc->send(vcc, skb) < 0) {
		priv->stats.tx_dropped++;
		return;
	}

	priv->stats.tx_packets++;
	priv->stats.tx_bytes += skb->len;
}

static void lec_tx_timeout(struct net_device *dev)
{
	printk(KERN_INFO "%s: tx timeout\n", dev->name);
	dev->trans_start = jiffies;
	netif_wake_queue(dev);
}

static int lec_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct sk_buff *skb2;
	struct lec_priv *priv = (struct lec_priv *)dev->priv;
	struct lecdatahdr_8023 *lec_h;
	struct atm_vcc *vcc;
	struct lec_arp_table *entry;
	unsigned char *dst;
	int min_frame_size;
#ifdef CONFIG_TR
	unsigned char rdesc[ETH_ALEN];	/* Token Ring route descriptor */
#endif
	int is_rdesc;
#if DUMP_PACKETS > 0
	char buf[300];
	int i = 0;
#endif /* DUMP_PACKETS >0 */

	DPRINTK("lec_start_xmit called\n");
	if (!priv->lecd) {
		printk("%s:No lecd attached\n", dev->name);
		priv->stats.tx_errors++;
		netif_stop_queue(dev);
		return -EUNATCH;
	}

	DPRINTK("skbuff head:%lx data:%lx tail:%lx end:%lx\n",
		(long)skb->head, (long)skb->data, (long)skb_tail_pointer(skb),
		(long)skb_end_pointer(skb));
#if defined(CONFIG_BRIDGE) || defined(CONFIG_BRIDGE_MODULE)
	if (memcmp(skb->data, bridge_ula_lec, sizeof(bridge_ula_lec)) == 0)
		lec_handle_bridge(skb, dev);
#endif

	/* Make sure we have room for lec_id */
	if (skb_headroom(skb) < 2) {

		DPRINTK("lec_start_xmit: reallocating skb\n");
		skb2 = skb_realloc_headroom(skb, LEC_HEADER_LEN);
		kfree_skb(skb);
		if (skb2 == NULL)
			return 0;
		skb = skb2;
	}
	skb_push(skb, 2);

	/* Put le header to place, works for TokenRing too */
	lec_h = (struct lecdatahdr_8023 *)skb->data;
	lec_h->le_header = htons(priv->lecid);

#ifdef CONFIG_TR
	/*
	 * Ugly. Use this to realign Token Ring packets for
	 * e.g. PCA-200E driver.
	 */
	if (priv->is_trdev) {
		skb2 = skb_realloc_headroom(skb, LEC_HEADER_LEN);
		kfree_skb(skb);
		if (skb2 == NULL)
			return 0;
		skb = skb2;
	}
#endif

#if DUMP_PACKETS > 0
	printk("%s: send datalen:%ld lecid:%4.4x\n", dev->name,
	       skb->len, priv->lecid);
#if DUMP_PACKETS >= 2
	for (i = 0; i < skb->len && i < 99; i++) {
		sprintf(buf + i * 3, "%2.2x ", 0xff & skb->data[i]);
	}
#elif DUMP_PACKETS >= 1
	for (i = 0; i < skb->len && i < 30; i++) {
		sprintf(buf + i * 3, "%2.2x ", 0xff & skb->data[i]);
	}
#endif /* DUMP_PACKETS >= 1 */
	if (i == skb->len)
		printk("%s\n", buf);
	else
		printk("%s...\n", buf);
#endif /* DUMP_PACKETS > 0 */

	/* Minimum ethernet-frame size */
#ifdef CONFIG_TR
	if (priv->is_trdev)
		min_frame_size = LEC_MINIMUM_8025_SIZE;
	else
#endif
		min_frame_size = LEC_MINIMUM_8023_SIZE;
	if (skb->len < min_frame_size) {
		if ((skb->len + skb_tailroom(skb)) < min_frame_size) {
			skb2 = skb_copy_expand(skb, 0,
					       min_frame_size - skb->truesize,
					       GFP_ATOMIC);
			dev_kfree_skb(skb);
			if (skb2 == NULL) {
				priv->stats.tx_dropped++;
				return 0;
			}
			skb = skb2;
		}
		skb_put(skb, min_frame_size - skb->len);
	}

	/* Send to right vcc */
	is_rdesc = 0;
	dst = lec_h->h_dest;
#ifdef CONFIG_TR
	if (priv->is_trdev) {
		dst = get_tr_dst(skb->data + 2, rdesc);
		if (dst == NULL) {
			dst = rdesc;
			is_rdesc = 1;
		}
	}
#endif
	entry = NULL;
	vcc = lec_arp_resolve(priv, dst, is_rdesc, &entry);
	DPRINTK("%s:vcc:%p vcc_flags:%x, entry:%p\n", dev->name,
		vcc, vcc ? vcc->flags : 0, entry);
	if (!vcc || !test_bit(ATM_VF_READY, &vcc->flags)) {
		if (entry && (entry->tx_wait.qlen < LEC_UNRES_QUE_LEN)) {
			DPRINTK("%s:lec_start_xmit: queuing packet, ",
				dev->name);
			DPRINTK("MAC address 0x%02x:%02x:%02x:%02x:%02x:%02x\n",
				lec_h->h_dest[0], lec_h->h_dest[1],
				lec_h->h_dest[2], lec_h->h_dest[3],
				lec_h->h_dest[4], lec_h->h_dest[5]);
			skb_queue_tail(&entry->tx_wait, skb);
		} else {
			DPRINTK
			    ("%s:lec_start_xmit: tx queue full or no arp entry, dropping, ",
			     dev->name);
			DPRINTK("MAC address 0x%02x:%02x:%02x:%02x:%02x:%02x\n",
				lec_h->h_dest[0], lec_h->h_dest[1],
				lec_h->h_dest[2], lec_h->h_dest[3],
				lec_h->h_dest[4], lec_h->h_dest[5]);
			priv->stats.tx_dropped++;
			dev_kfree_skb(skb);
		}
		goto out;
	}
#if DUMP_PACKETS > 0
	printk("%s:sending to vpi:%d vci:%d\n", dev->name, vcc->vpi, vcc->vci);
#endif /* DUMP_PACKETS > 0 */

	while (entry && (skb2 = skb_dequeue(&entry->tx_wait))) {
		DPRINTK("lec.c: emptying tx queue, ");
		DPRINTK("MAC address 0x%02x:%02x:%02x:%02x:%02x:%02x\n",
			lec_h->h_dest[0], lec_h->h_dest[1], lec_h->h_dest[2],
			lec_h->h_dest[3], lec_h->h_dest[4], lec_h->h_dest[5]);
		lec_send(vcc, skb2, priv);
	}

	lec_send(vcc, skb, priv);

	if (!atm_may_send(vcc, 0)) {
		struct lec_vcc_priv *vpriv = LEC_VCC_PRIV(vcc);

		vpriv->xoff = 1;
		netif_stop_queue(dev);

		/*
		 * vcc->pop() might have occurred in between, making
		 * the vcc usuable again.  Since xmit is serialized,
		 * this is the only situation we have to re-test.
		 */

		if (atm_may_send(vcc, 0))
			netif_wake_queue(dev);
	}

out:
	if (entry)
		lec_arp_put(entry);
	dev->trans_start = jiffies;
	return 0;
}

/* The inverse routine to net_open(). */
static int lec_close(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

/*
 * Get the current statistics.
 * This may be called with the card open or closed.
 */
static struct net_device_stats *lec_get_stats(struct net_device *dev)
{
	return &((struct lec_priv *)dev->priv)->stats;
}

static int lec_atm_send(struct atm_vcc *vcc, struct sk_buff *skb)
{
	unsigned long flags;
	struct net_device *dev = (struct net_device *)vcc->proto_data;
	struct lec_priv *priv = (struct lec_priv *)dev->priv;
	struct atmlec_msg *mesg;
	struct lec_arp_table *entry;
	int i;
	char *tmp;		/* FIXME */

	atomic_sub(skb->truesize, &sk_atm(vcc)->sk_wmem_alloc);
	mesg = (struct atmlec_msg *)skb->data;
	tmp = skb->data;
	tmp += sizeof(struct atmlec_msg);
	DPRINTK("%s: msg from zeppelin:%d\n", dev->name, mesg->type);
	switch (mesg->type) {
	case l_set_mac_addr:
		for (i = 0; i < 6; i++) {
			dev->dev_addr[i] = mesg->content.normal.mac_addr[i];
		}
		break;
	case l_del_mac_addr:
		for (i = 0; i < 6; i++) {
			dev->dev_addr[i] = 0;
		}
		break;
	case l_addr_delete:
		lec_addr_delete(priv, mesg->content.normal.atm_addr,
				mesg->content.normal.flag);
		break;
	case l_topology_change:
		priv->topology_change = mesg->content.normal.flag;
		break;
	case l_flush_complete:
		lec_flush_complete(priv, mesg->content.normal.flag);
		break;
	case l_narp_req:	/* LANE2: see 7.1.35 in the lane2 spec */
		spin_lock_irqsave(&priv->lec_arp_lock, flags);
		entry = lec_arp_find(priv, mesg->content.normal.mac_addr);
		lec_arp_remove(priv, entry);
		spin_unlock_irqrestore(&priv->lec_arp_lock, flags);

		if (mesg->content.normal.no_source_le_narp)
			break;
		/* FALL THROUGH */
	case l_arp_update:
		lec_arp_update(priv, mesg->content.normal.mac_addr,
			       mesg->content.normal.atm_addr,
			       mesg->content.normal.flag,
			       mesg->content.normal.targetless_le_arp);
		DPRINTK("lec: in l_arp_update\n");
		if (mesg->sizeoftlvs != 0) {	/* LANE2 3.1.5 */
			DPRINTK("lec: LANE2 3.1.5, got tlvs, size %d\n",
				mesg->sizeoftlvs);
			lane2_associate_ind(dev, mesg->content.normal.mac_addr,
					    tmp, mesg->sizeoftlvs);
		}
		break;
	case l_config:
		priv->maximum_unknown_frame_count =
		    mesg->content.config.maximum_unknown_frame_count;
		priv->max_unknown_frame_time =
		    (mesg->content.config.max_unknown_frame_time * HZ);
		priv->max_retry_count = mesg->content.config.max_retry_count;
		priv->aging_time = (mesg->content.config.aging_time * HZ);
		priv->forward_delay_time =
		    (mesg->content.config.forward_delay_time * HZ);
		priv->arp_response_time =
		    (mesg->content.config.arp_response_time * HZ);
		priv->flush_timeout = (mesg->content.config.flush_timeout * HZ);
		priv->path_switching_delay =
		    (mesg->content.config.path_switching_delay * HZ);
		priv->lane_version = mesg->content.config.lane_version;	/* LANE2 */
		priv->lane2_ops = NULL;
		if (priv->lane_version > 1)
			priv->lane2_ops = &lane2_ops;
		if (dev->change_mtu(dev, mesg->content.config.mtu))
			printk("%s: change_mtu to %d failed\n", dev->name,
			       mesg->content.config.mtu);
		priv->is_proxy = mesg->content.config.is_proxy;
		break;
	case l_flush_tran_id:
		lec_set_flush_tran_id(priv, mesg->content.normal.atm_addr,
				      mesg->content.normal.flag);
		break;
	case l_set_lecid:
		priv->lecid =
		    (unsigned short)(0xffff & mesg->content.normal.flag);
		break;
	case l_should_bridge:
#if defined(CONFIG_BRIDGE) || defined(CONFIG_BRIDGE_MODULE)
		{
			struct net_bridge_fdb_entry *f;

			DPRINTK
			    ("%s: bridge zeppelin asks about 0x%02x:%02x:%02x:%02x:%02x:%02x\n",
			     dev->name, mesg->content.proxy.mac_addr[0],
			     mesg->content.proxy.mac_addr[1],
			     mesg->content.proxy.mac_addr[2],
			     mesg->content.proxy.mac_addr[3],
			     mesg->content.proxy.mac_addr[4],
			     mesg->content.proxy.mac_addr[5]);

			if (br_fdb_get_hook == NULL || dev->br_port == NULL)
				break;

			f = br_fdb_get_hook(dev->br_port->br,
					    mesg->content.proxy.mac_addr);
			if (f != NULL && f->dst->dev != dev
			    && f->dst->state == BR_STATE_FORWARDING) {
				/* hit from bridge table, send LE_ARP_RESPONSE */
				struct sk_buff *skb2;
				struct sock *sk;

				DPRINTK
				    ("%s: entry found, responding to zeppelin\n",
				     dev->name);
				skb2 =
				    alloc_skb(sizeof(struct atmlec_msg),
					      GFP_ATOMIC);
				if (skb2 == NULL) {
					br_fdb_put_hook(f);
					break;
				}
				skb2->len = sizeof(struct atmlec_msg);
				skb_copy_to_linear_data(skb2, mesg,
							sizeof(*mesg));
				atm_force_charge(priv->lecd, skb2->truesize);
				sk = sk_atm(priv->lecd);
				skb_queue_tail(&sk->sk_receive_queue, skb2);
				sk->sk_data_ready(sk, skb2->len);
			}
			if (f != NULL)
				br_fdb_put_hook(f);
		}
#endif /* defined(CONFIG_BRIDGE) || defined(CONFIG_BRIDGE_MODULE) */
		break;
	default:
		printk("%s: Unknown message type %d\n", dev->name, mesg->type);
		dev_kfree_skb(skb);
		return -EINVAL;
	}
	dev_kfree_skb(skb);
	return 0;
}

static void lec_atm_close(struct atm_vcc *vcc)
{
	struct sk_buff *skb;
	struct net_device *dev = (struct net_device *)vcc->proto_data;
	struct lec_priv *priv = (struct lec_priv *)dev->priv;

	priv->lecd = NULL;
	/* Do something needful? */

	netif_stop_queue(dev);
	lec_arp_destroy(priv);

	if (skb_peek(&sk_atm(vcc)->sk_receive_queue))
		printk("%s lec_atm_close: closing with messages pending\n",
		       dev->name);
	while ((skb = skb_dequeue(&sk_atm(vcc)->sk_receive_queue)) != NULL) {
		atm_return(vcc, skb->truesize);
		dev_kfree_skb(skb);
	}

	printk("%s: Shut down!\n", dev->name);
	module_put(THIS_MODULE);
}

static struct atmdev_ops lecdev_ops = {
	.close = lec_atm_close,
	.send = lec_atm_send
};

static struct atm_dev lecatm_dev = {
	.ops = &lecdev_ops,
	.type = "lec",
	.number = 999,		/* dummy device number */
	.lock = __SPIN_LOCK_UNLOCKED(lecatm_dev.lock)
};

/*
 * LANE2: new argument struct sk_buff *data contains
 * the LE_ARP based TLVs introduced in the LANE2 spec
 */
static int
send_to_lecd(struct lec_priv *priv, atmlec_msg_type type,
	     unsigned char *mac_addr, unsigned char *atm_addr,
	     struct sk_buff *data)
{
	struct sock *sk;
	struct sk_buff *skb;
	struct atmlec_msg *mesg;

	if (!priv || !priv->lecd) {
		return -1;
	}
	skb = alloc_skb(sizeof(struct atmlec_msg), GFP_ATOMIC);
	if (!skb)
		return -1;
	skb->len = sizeof(struct atmlec_msg);
	mesg = (struct atmlec_msg *)skb->data;
	memset(mesg, 0, sizeof(struct atmlec_msg));
	mesg->type = type;
	if (data != NULL)
		mesg->sizeoftlvs = data->len;
	if (mac_addr)
		memcpy(&mesg->content.normal.mac_addr, mac_addr, ETH_ALEN);
	else
		mesg->content.normal.targetless_le_arp = 1;
	if (atm_addr)
		memcpy(&mesg->content.normal.atm_addr, atm_addr, ATM_ESA_LEN);

	atm_force_charge(priv->lecd, skb->truesize);
	sk = sk_atm(priv->lecd);
	skb_queue_tail(&sk->sk_receive_queue, skb);
	sk->sk_data_ready(sk, skb->len);

	if (data != NULL) {
		DPRINTK("lec: about to send %d bytes of data\n", data->len);
		atm_force_charge(priv->lecd, data->truesize);
		skb_queue_tail(&sk->sk_receive_queue, data);
		sk->sk_data_ready(sk, skb->len);
	}

	return 0;
}

/* shamelessly stolen from drivers/net/net_init.c */
static int lec_change_mtu(struct net_device *dev, int new_mtu)
{
	if ((new_mtu < 68) || (new_mtu > 18190))
		return -EINVAL;
	dev->mtu = new_mtu;
	return 0;
}

static void lec_set_multicast_list(struct net_device *dev)
{
	/*
	 * by default, all multicast frames arrive over the bus.
	 * eventually support selective multicast service
	 */
	return;
}

static void lec_init(struct net_device *dev)
{
	dev->change_mtu = lec_change_mtu;
	dev->open = lec_open;
	dev->stop = lec_close;
	dev->hard_start_xmit = lec_start_xmit;
	dev->tx_timeout = lec_tx_timeout;

	dev->get_stats = lec_get_stats;
	dev->set_multicast_list = lec_set_multicast_list;
	dev->do_ioctl = NULL;
	printk("%s: Initialized!\n", dev->name);
	return;
}

static unsigned char lec_ctrl_magic[] = {
	0xff,
	0x00,
	0x01,
	0x01
};

#define LEC_DATA_DIRECT_8023  2
#define LEC_DATA_DIRECT_8025  3

static int lec_is_data_direct(struct atm_vcc *vcc)
{
	return ((vcc->sap.blli[0].l3.tr9577.snap[4] == LEC_DATA_DIRECT_8023) ||
		(vcc->sap.blli[0].l3.tr9577.snap[4] == LEC_DATA_DIRECT_8025));
}

static void lec_push(struct atm_vcc *vcc, struct sk_buff *skb)
{
	unsigned long flags;
	struct net_device *dev = (struct net_device *)vcc->proto_data;
	struct lec_priv *priv = (struct lec_priv *)dev->priv;

#if DUMP_PACKETS >0
	int i = 0;
	char buf[300];

	printk("%s: lec_push vcc vpi:%d vci:%d\n", dev->name,
	       vcc->vpi, vcc->vci);
#endif
	if (!skb) {
		DPRINTK("%s: null skb\n", dev->name);
		lec_vcc_close(priv, vcc);
		return;
	}
#if DUMP_PACKETS > 0
	printk("%s: rcv datalen:%ld lecid:%4.4x\n", dev->name,
	       skb->len, priv->lecid);
#if DUMP_PACKETS >= 2
	for (i = 0; i < skb->len && i < 99; i++) {
		sprintf(buf + i * 3, "%2.2x ", 0xff & skb->data[i]);
	}
#elif DUMP_PACKETS >= 1
	for (i = 0; i < skb->len && i < 30; i++) {
		sprintf(buf + i * 3, "%2.2x ", 0xff & skb->data[i]);
	}
#endif /* DUMP_PACKETS >= 1 */
	if (i == skb->len)
		printk("%s\n", buf);
	else
		printk("%s...\n", buf);
#endif /* DUMP_PACKETS > 0 */
	if (memcmp(skb->data, lec_ctrl_magic, 4) == 0) {	/* Control frame, to daemon */
		struct sock *sk = sk_atm(vcc);

		DPRINTK("%s: To daemon\n", dev->name);
		skb_queue_tail(&sk->sk_receive_queue, skb);
		sk->sk_data_ready(sk, skb->len);
	} else {		/* Data frame, queue to protocol handlers */
		struct lec_arp_table *entry;
		unsigned char *src, *dst;

		atm_return(vcc, skb->truesize);
		if (*(__be16 *) skb->data == htons(priv->lecid) ||
		    !priv->lecd || !(dev->flags & IFF_UP)) {
			/*
			 * Probably looping back, or if lecd is missing,
			 * lecd has gone down
			 */
			DPRINTK("Ignoring frame...\n");
			dev_kfree_skb(skb);
			return;
		}
#ifdef CONFIG_TR
		if (priv->is_trdev)
			dst = ((struct lecdatahdr_8025 *)skb->data)->h_dest;
		else
#endif
			dst = ((struct lecdatahdr_8023 *)skb->data)->h_dest;

		/*
		 * If this is a Data Direct VCC, and the VCC does not match
		 * the LE_ARP cache entry, delete the LE_ARP cache entry.
		 */
		spin_lock_irqsave(&priv->lec_arp_lock, flags);
		if (lec_is_data_direct(vcc)) {
#ifdef CONFIG_TR
			if (priv->is_trdev)
				src =
				    ((struct lecdatahdr_8025 *)skb->data)->
				    h_source;
			else
#endif
				src =
				    ((struct lecdatahdr_8023 *)skb->data)->
				    h_source;
			entry = lec_arp_find(priv, src);
			if (entry && entry->vcc != vcc) {
				lec_arp_remove(priv, entry);
				lec_arp_put(entry);
			}
		}
		spin_unlock_irqrestore(&priv->lec_arp_lock, flags);

		if (!(dst[0] & 0x01) &&	/* Never filter Multi/Broadcast */
		    !priv->is_proxy &&	/* Proxy wants all the packets */
		    memcmp(dst, dev->dev_addr, dev->addr_len)) {
			dev_kfree_skb(skb);
			return;
		}
		if (!hlist_empty(&priv->lec_arp_empty_ones)) {
			lec_arp_check_empties(priv, vcc, skb);
		}
		skb_pull(skb, 2);	/* skip lec_id */
#ifdef CONFIG_TR
		if (priv->is_trdev)
			skb->protocol = tr_type_trans(skb, dev);
		else
#endif
			skb->protocol = eth_type_trans(skb, dev);
		priv->stats.rx_packets++;
		priv->stats.rx_bytes += skb->len;
		memset(ATM_SKB(skb), 0, sizeof(struct atm_skb_data));
		netif_rx(skb);
	}
}

static void lec_pop(struct atm_vcc *vcc, struct sk_buff *skb)
{
	struct lec_vcc_priv *vpriv = LEC_VCC_PRIV(vcc);
	struct net_device *dev = skb->dev;

	if (vpriv == NULL) {
		printk("lec_pop(): vpriv = NULL!?!?!?\n");
		return;
	}

	vpriv->old_pop(vcc, skb);

	if (vpriv->xoff && atm_may_send(vcc, 0)) {
		vpriv->xoff = 0;
		if (netif_running(dev) && netif_queue_stopped(dev))
			netif_wake_queue(dev);
	}
}

static int lec_vcc_attach(struct atm_vcc *vcc, void __user *arg)
{
	struct lec_vcc_priv *vpriv;
	int bytes_left;
	struct atmlec_ioc ioc_data;

	/* Lecd must be up in this case */
	bytes_left = copy_from_user(&ioc_data, arg, sizeof(struct atmlec_ioc));
	if (bytes_left != 0) {
		printk
		    ("lec: lec_vcc_attach, copy from user failed for %d bytes\n",
		     bytes_left);
	}
	if (ioc_data.dev_num < 0 || ioc_data.dev_num >= MAX_LEC_ITF ||
	    !dev_lec[ioc_data.dev_num])
		return -EINVAL;
	if (!(vpriv = kmalloc(sizeof(struct lec_vcc_priv), GFP_KERNEL)))
		return -ENOMEM;
	vpriv->xoff = 0;
	vpriv->old_pop = vcc->pop;
	vcc->user_back = vpriv;
	vcc->pop = lec_pop;
	lec_vcc_added(dev_lec[ioc_data.dev_num]->priv,
		      &ioc_data, vcc, vcc->push);
	vcc->proto_data = dev_lec[ioc_data.dev_num];
	vcc->push = lec_push;
	return 0;
}

static int lec_mcast_attach(struct atm_vcc *vcc, int arg)
{
	if (arg < 0 || arg >= MAX_LEC_ITF || !dev_lec[arg])
		return -EINVAL;
	vcc->proto_data = dev_lec[arg];
	return (lec_mcast_make((struct lec_priv *)dev_lec[arg]->priv, vcc));
}

/* Initialize device. */
static int lecd_attach(struct atm_vcc *vcc, int arg)
{
	int i;
	struct lec_priv *priv;

	if (arg < 0)
		i = 0;
	else
		i = arg;
#ifdef CONFIG_TR
	if (arg >= MAX_LEC_ITF)
		return -EINVAL;
#else				/* Reserve the top NUM_TR_DEVS for TR */
	if (arg >= (MAX_LEC_ITF - NUM_TR_DEVS))
		return -EINVAL;
#endif
	if (!dev_lec[i]) {
		int is_trdev, size;

		is_trdev = 0;
		if (i >= (MAX_LEC_ITF - NUM_TR_DEVS))
			is_trdev = 1;

		size = sizeof(struct lec_priv);
#ifdef CONFIG_TR
		if (is_trdev)
			dev_lec[i] = alloc_trdev(size);
		else
#endif
			dev_lec[i] = alloc_etherdev(size);
		if (!dev_lec[i])
			return -ENOMEM;
		snprintf(dev_lec[i]->name, IFNAMSIZ, "lec%d", i);
		if (register_netdev(dev_lec[i])) {
			free_netdev(dev_lec[i]);
			return -EINVAL;
		}

		priv = dev_lec[i]->priv;
		priv->is_trdev = is_trdev;
		lec_init(dev_lec[i]);
	} else {
		priv = dev_lec[i]->priv;
		if (priv->lecd)
			return -EADDRINUSE;
	}
	lec_arp_init(priv);
	priv->itfnum = i;	/* LANE2 addition */
	priv->lecd = vcc;
	vcc->dev = &lecatm_dev;
	vcc_insert_socket(sk_atm(vcc));

	vcc->proto_data = dev_lec[i];
	set_bit(ATM_VF_META, &vcc->flags);
	set_bit(ATM_VF_READY, &vcc->flags);

	/* Set default values to these variables */
	priv->maximum_unknown_frame_count = 1;
	priv->max_unknown_frame_time = (1 * HZ);
	priv->vcc_timeout_period = (1200 * HZ);
	priv->max_retry_count = 1;
	priv->aging_time = (300 * HZ);
	priv->forward_delay_time = (15 * HZ);
	priv->topology_change = 0;
	priv->arp_response_time = (1 * HZ);
	priv->flush_timeout = (4 * HZ);
	priv->path_switching_delay = (6 * HZ);

	if (dev_lec[i]->flags & IFF_UP) {
		netif_start_queue(dev_lec[i]);
	}
	__module_get(THIS_MODULE);
	return i;
}

#ifdef CONFIG_PROC_FS
static char *lec_arp_get_status_string(unsigned char status)
{
	static char *lec_arp_status_string[] = {
		"ESI_UNKNOWN       ",
		"ESI_ARP_PENDING   ",
		"ESI_VC_PENDING    ",
		"<Undefined>       ",
		"ESI_FLUSH_PENDING ",
		"ESI_FORWARD_DIRECT"
	};

	if (status > ESI_FORWARD_DIRECT)
		status = 3;	/* ESI_UNDEFINED */
	return lec_arp_status_string[status];
}

static void lec_info(struct seq_file *seq, struct lec_arp_table *entry)
{
	int i;

	for (i = 0; i < ETH_ALEN; i++)
		seq_printf(seq, "%2.2x", entry->mac_addr[i] & 0xff);
	seq_printf(seq, " ");
	for (i = 0; i < ATM_ESA_LEN; i++)
		seq_printf(seq, "%2.2x", entry->atm_addr[i] & 0xff);
	seq_printf(seq, " %s %4.4x", lec_arp_get_status_string(entry->status),
		   entry->flags & 0xffff);
	if (entry->vcc)
		seq_printf(seq, "%3d %3d ", entry->vcc->vpi, entry->vcc->vci);
	else
		seq_printf(seq, "        ");
	if (entry->recv_vcc) {
		seq_printf(seq, "     %3d %3d", entry->recv_vcc->vpi,
			   entry->recv_vcc->vci);
	}
	seq_putc(seq, '\n');
}

struct lec_state {
	unsigned long flags;
	struct lec_priv *locked;
	struct hlist_node *node;
	struct net_device *dev;
	int itf;
	int arp_table;
	int misc_table;
};

static void *lec_tbl_walk(struct lec_state *state, struct hlist_head *tbl,
			  loff_t *l)
{
	struct hlist_node *e = state->node;
	struct lec_arp_table *tmp;

	if (!e)
		e = tbl->first;
	if (e == (void *)1) {
		e = tbl->first;
		--*l;
	}

	hlist_for_each_entry_from(tmp, e, next) {
		if (--*l < 0)
			break;
	}
	state->node = e;

	return (*l < 0) ? state : NULL;
}

static void *lec_arp_walk(struct lec_state *state, loff_t *l,
			  struct lec_priv *priv)
{
	void *v = NULL;
	int p;

	for (p = state->arp_table; p < LEC_ARP_TABLE_SIZE; p++) {
		v = lec_tbl_walk(state, &priv->lec_arp_tables[p], l);
		if (v)
			break;
	}
	state->arp_table = p;
	return v;
}

static void *lec_misc_walk(struct lec_state *state, loff_t *l,
			   struct lec_priv *priv)
{
	struct hlist_head *lec_misc_tables[] = {
		&priv->lec_arp_empty_ones,
		&priv->lec_no_forward,
		&priv->mcast_fwds
	};
	void *v = NULL;
	int q;

	for (q = state->misc_table; q < ARRAY_SIZE(lec_misc_tables); q++) {
		v = lec_tbl_walk(state, lec_misc_tables[q], l);
		if (v)
			break;
	}
	state->misc_table = q;
	return v;
}

static void *lec_priv_walk(struct lec_state *state, loff_t *l,
			   struct lec_priv *priv)
{
	if (!state->locked) {
		state->locked = priv;
		spin_lock_irqsave(&priv->lec_arp_lock, state->flags);
	}
	if (!lec_arp_walk(state, l, priv) && !lec_misc_walk(state, l, priv)) {
		spin_unlock_irqrestore(&priv->lec_arp_lock, state->flags);
		state->locked = NULL;
		/* Partial state reset for the next time we get called */
		state->arp_table = state->misc_table = 0;
	}
	return state->locked;
}

static void *lec_itf_walk(struct lec_state *state, loff_t *l)
{
	struct net_device *dev;
	void *v;

	dev = state->dev ? state->dev : dev_lec[state->itf];
	v = (dev && dev->priv) ? lec_priv_walk(state, l, dev->priv) : NULL;
	if (!v && dev) {
		dev_put(dev);
		/* Partial state reset for the next time we get called */
		dev = NULL;
	}
	state->dev = dev;
	return v;
}

static void *lec_get_idx(struct lec_state *state, loff_t l)
{
	void *v = NULL;

	for (; state->itf < MAX_LEC_ITF; state->itf++) {
		v = lec_itf_walk(state, &l);
		if (v)
			break;
	}
	return v;
}

static void *lec_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct lec_state *state = seq->private;

	state->itf = 0;
	state->dev = NULL;
	state->locked = NULL;
	state->arp_table = 0;
	state->misc_table = 0;
	state->node = (void *)1;

	return *pos ? lec_get_idx(state, *pos) : (void *)1;
}

static void lec_seq_stop(struct seq_file *seq, void *v)
{
	struct lec_state *state = seq->private;

	if (state->dev) {
		spin_unlock_irqrestore(&state->locked->lec_arp_lock,
				       state->flags);
		dev_put(state->dev);
	}
}

static void *lec_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct lec_state *state = seq->private;

	v = lec_get_idx(state, 1);
	*pos += !!PTR_ERR(v);
	return v;
}

static int lec_seq_show(struct seq_file *seq, void *v)
{
	static char lec_banner[] = "Itf  MAC          ATM destination"
	    "                          Status            Flags "
	    "VPI/VCI Recv VPI/VCI\n";

	if (v == (void *)1)
		seq_puts(seq, lec_banner);
	else {
		struct lec_state *state = seq->private;
		struct net_device *dev = state->dev;
		struct lec_arp_table *entry = hlist_entry(state->node, struct lec_arp_table, next);

		seq_printf(seq, "%s ", dev->name);
		lec_info(seq, entry);
	}
	return 0;
}

static const struct seq_operations lec_seq_ops = {
	.start = lec_seq_start,
	.next = lec_seq_next,
	.stop = lec_seq_stop,
	.show = lec_seq_show,
};

static int lec_seq_open(struct inode *inode, struct file *file)
{
	struct lec_state *state;
	struct seq_file *seq;
	int rc = -EAGAIN;

	state = kmalloc(sizeof(*state), GFP_KERNEL);
	if (!state) {
		rc = -ENOMEM;
		goto out;
	}

	rc = seq_open(file, &lec_seq_ops);
	if (rc)
		goto out_kfree;
	seq = file->private_data;
	seq->private = state;
out:
	return rc;

out_kfree:
	kfree(state);
	goto out;
}

static int lec_seq_release(struct inode *inode, struct file *file)
{
	return seq_release_private(inode, file);
}

static const struct file_operations lec_seq_fops = {
	.owner = THIS_MODULE,
	.open = lec_seq_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = lec_seq_release,
};
#endif

static int lane_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct atm_vcc *vcc = ATM_SD(sock);
	int err = 0;

	switch (cmd) {
	case ATMLEC_CTRL:
	case ATMLEC_MCAST:
	case ATMLEC_DATA:
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;
		break;
	default:
		return -ENOIOCTLCMD;
	}

	switch (cmd) {
	case ATMLEC_CTRL:
		err = lecd_attach(vcc, (int)arg);
		if (err >= 0)
			sock->state = SS_CONNECTED;
		break;
	case ATMLEC_MCAST:
		err = lec_mcast_attach(vcc, (int)arg);
		break;
	case ATMLEC_DATA:
		err = lec_vcc_attach(vcc, (void __user *)arg);
		break;
	}

	return err;
}

static struct atm_ioctl lane_ioctl_ops = {
	.owner = THIS_MODULE,
	.ioctl = lane_ioctl,
};

static int __init lane_module_init(void)
{
#ifdef CONFIG_PROC_FS
	struct proc_dir_entry *p;

	p = create_proc_entry("lec", S_IRUGO, atm_proc_root);
	if (p)
		p->proc_fops = &lec_seq_fops;
#endif

	register_atm_ioctl(&lane_ioctl_ops);
	printk("lec.c: " __DATE__ " " __TIME__ " initialized\n");
	return 0;
}

static void __exit lane_module_cleanup(void)
{
	int i;
	struct lec_priv *priv;

	remove_proc_entry("lec", atm_proc_root);

	deregister_atm_ioctl(&lane_ioctl_ops);

	for (i = 0; i < MAX_LEC_ITF; i++) {
		if (dev_lec[i] != NULL) {
			priv = (struct lec_priv *)dev_lec[i]->priv;
			unregister_netdev(dev_lec[i]);
			free_netdev(dev_lec[i]);
			dev_lec[i] = NULL;
		}
	}

	return;
}

module_init(lane_module_init);
module_exit(lane_module_cleanup);

/*
 * LANE2: 3.1.3, LE_RESOLVE.request
 * Non force allocates memory and fills in *tlvs, fills in *sizeoftlvs.
 * If sizeoftlvs == NULL the default TLVs associated with with this
 * lec will be used.
 * If dst_mac == NULL, targetless LE_ARP will be sent
 */
static int lane2_resolve(struct net_device *dev, u8 *dst_mac, int force,
			 u8 **tlvs, u32 *sizeoftlvs)
{
	unsigned long flags;
	struct lec_priv *priv = (struct lec_priv *)dev->priv;
	struct lec_arp_table *table;
	struct sk_buff *skb;
	int retval;

	if (force == 0) {
		spin_lock_irqsave(&priv->lec_arp_lock, flags);
		table = lec_arp_find(priv, dst_mac);
		spin_unlock_irqrestore(&priv->lec_arp_lock, flags);
		if (table == NULL)
			return -1;

		*tlvs = kmemdup(table->tlvs, table->sizeoftlvs, GFP_ATOMIC);
		if (*tlvs == NULL)
			return -1;

		*sizeoftlvs = table->sizeoftlvs;

		return 0;
	}

	if (sizeoftlvs == NULL)
		retval = send_to_lecd(priv, l_arp_xmt, dst_mac, NULL, NULL);

	else {
		skb = alloc_skb(*sizeoftlvs, GFP_ATOMIC);
		if (skb == NULL)
			return -1;
		skb->len = *sizeoftlvs;
		skb_copy_to_linear_data(skb, *tlvs, *sizeoftlvs);
		retval = send_to_lecd(priv, l_arp_xmt, dst_mac, NULL, skb);
	}
	return retval;
}

/*
 * LANE2: 3.1.4, LE_ASSOCIATE.request
 * Associate the *tlvs with the *lan_dst address.
 * Will overwrite any previous association
 * Returns 1 for success, 0 for failure (out of memory)
 *
 */
static int lane2_associate_req(struct net_device *dev, u8 *lan_dst,
			       u8 *tlvs, u32 sizeoftlvs)
{
	int retval;
	struct sk_buff *skb;
	struct lec_priv *priv = (struct lec_priv *)dev->priv;

	if (compare_ether_addr(lan_dst, dev->dev_addr))
		return (0);	/* not our mac address */

	kfree(priv->tlvs);	/* NULL if there was no previous association */

	priv->tlvs = kmemdup(tlvs, sizeoftlvs, GFP_KERNEL);
	if (priv->tlvs == NULL)
		return (0);
	priv->sizeoftlvs = sizeoftlvs;

	skb = alloc_skb(sizeoftlvs, GFP_ATOMIC);
	if (skb == NULL)
		return 0;
	skb->len = sizeoftlvs;
	skb_copy_to_linear_data(skb, tlvs, sizeoftlvs);
	retval = send_to_lecd(priv, l_associate_req, NULL, NULL, skb);
	if (retval != 0)
		printk("lec.c: lane2_associate_req() failed\n");
	/*
	 * If the previous association has changed we must
	 * somehow notify other LANE entities about the change
	 */
	return (1);
}

/*
 * LANE2: 3.1.5, LE_ASSOCIATE.indication
 *
 */
static void lane2_associate_ind(struct net_device *dev, u8 *mac_addr,
				u8 *tlvs, u32 sizeoftlvs)
{
#if 0
	int i = 0;
#endif
	struct lec_priv *priv = (struct lec_priv *)dev->priv;
#if 0				/*
				 * Why have the TLVs in LE_ARP entries
				 * since we do not use them? When you
				 * uncomment this code, make sure the
				 * TLVs get freed when entry is killed
				 */
	struct lec_arp_table *entry = lec_arp_find(priv, mac_addr);

	if (entry == NULL)
		return;		/* should not happen */

	kfree(entry->tlvs);

	entry->tlvs = kmemdup(tlvs, sizeoftlvs, GFP_KERNEL);
	if (entry->tlvs == NULL)
		return;
	entry->sizeoftlvs = sizeoftlvs;
#endif
#if 0
	printk("lec.c: lane2_associate_ind()\n");
	printk("dump of tlvs, sizeoftlvs=%d\n", sizeoftlvs);
	while (i < sizeoftlvs)
		printk("%02x ", tlvs[i++]);

	printk("\n");
#endif

	/* tell MPOA about the TLVs we saw */
	if (priv->lane2_ops && priv->lane2_ops->associate_indicator) {
		priv->lane2_ops->associate_indicator(dev, mac_addr,
						     tlvs, sizeoftlvs);
	}
	return;
}

/*
 * Here starts what used to lec_arpc.c
 *
 * lec_arpc.c was added here when making
 * lane client modular. October 1997
 */

#include <linux/types.h>
#include <linux/timer.h>
#include <asm/param.h>
#include <asm/atomic.h>
#include <linux/inetdevice.h>
#include <net/route.h>

#if 0
#define DPRINTK(format,args...)
/*
#define DPRINTK printk
*/
#endif
#define DEBUG_ARP_TABLE 0

#define LEC_ARP_REFRESH_INTERVAL (3*HZ)

static void lec_arp_check_expire(struct work_struct *work);
static void lec_arp_expire_arp(unsigned long data);

/*
 * Arp table funcs
 */

#define HASH(ch) (ch & (LEC_ARP_TABLE_SIZE -1))

/*
 * Initialization of arp-cache
 */
static void lec_arp_init(struct lec_priv *priv)
{
	unsigned short i;

	for (i = 0; i < LEC_ARP_TABLE_SIZE; i++) {
		INIT_HLIST_HEAD(&priv->lec_arp_tables[i]);
	}
	INIT_HLIST_HEAD(&priv->lec_arp_empty_ones);
	INIT_HLIST_HEAD(&priv->lec_no_forward);
	INIT_HLIST_HEAD(&priv->mcast_fwds);
	spin_lock_init(&priv->lec_arp_lock);
	INIT_DELAYED_WORK(&priv->lec_arp_work, lec_arp_check_expire);
	schedule_delayed_work(&priv->lec_arp_work, LEC_ARP_REFRESH_INTERVAL);
}

static void lec_arp_clear_vccs(struct lec_arp_table *entry)
{
	if (entry->vcc) {
		struct atm_vcc *vcc = entry->vcc;
		struct lec_vcc_priv *vpriv = LEC_VCC_PRIV(vcc);
		struct net_device *dev = (struct net_device *)vcc->proto_data;

		vcc->pop = vpriv->old_pop;
		if (vpriv->xoff)
			netif_wake_queue(dev);
		kfree(vpriv);
		vcc->user_back = NULL;
		vcc->push = entry->old_push;
		vcc_release_async(vcc, -EPIPE);
		entry->vcc = NULL;
	}
	if (entry->recv_vcc) {
		entry->recv_vcc->push = entry->old_recv_push;
		vcc_release_async(entry->recv_vcc, -EPIPE);
		entry->recv_vcc = NULL;
	}
}

/*
 * Insert entry to lec_arp_table
 * LANE2: Add to the end of the list to satisfy 8.1.13
 */
static inline void
lec_arp_add(struct lec_priv *priv, struct lec_arp_table *entry)
{
	struct hlist_head *tmp;

	tmp = &priv->lec_arp_tables[HASH(entry->mac_addr[ETH_ALEN - 1])];
	hlist_add_head(&entry->next, tmp);

	DPRINTK("LEC_ARP: Added entry:%2.2x %2.2x %2.2x %2.2x %2.2x %2.2x\n",
		0xff & entry->mac_addr[0], 0xff & entry->mac_addr[1],
		0xff & entry->mac_addr[2], 0xff & entry->mac_addr[3],
		0xff & entry->mac_addr[4], 0xff & entry->mac_addr[5]);
}

/*
 * Remove entry from lec_arp_table
 */
static int
lec_arp_remove(struct lec_priv *priv, struct lec_arp_table *to_remove)
{
	struct hlist_node *node;
	struct lec_arp_table *entry;
	int i, remove_vcc = 1;

	if (!to_remove) {
		return -1;
	}

	hlist_del(&to_remove->next);
	del_timer(&to_remove->timer);

	/* If this is the only MAC connected to this VCC, also tear down the VCC */
	if (to_remove->status >= ESI_FLUSH_PENDING) {
		/*
		 * ESI_FLUSH_PENDING, ESI_FORWARD_DIRECT
		 */
		for (i = 0; i < LEC_ARP_TABLE_SIZE; i++) {
			hlist_for_each_entry(entry, node, &priv->lec_arp_tables[i], next) {
				if (memcmp(to_remove->atm_addr,
					   entry->atm_addr, ATM_ESA_LEN) == 0) {
					remove_vcc = 0;
					break;
				}
			}
		}
		if (remove_vcc)
			lec_arp_clear_vccs(to_remove);
	}
	skb_queue_purge(&to_remove->tx_wait);	/* FIXME: good place for this? */

	DPRINTK("LEC_ARP: Removed entry:%2.2x %2.2x %2.2x %2.2x %2.2x %2.2x\n",
		0xff & to_remove->mac_addr[0], 0xff & to_remove->mac_addr[1],
		0xff & to_remove->mac_addr[2], 0xff & to_remove->mac_addr[3],
		0xff & to_remove->mac_addr[4], 0xff & to_remove->mac_addr[5]);
	return 0;
}

#if DEBUG_ARP_TABLE
static char *get_status_string(unsigned char st)
{
	switch (st) {
	case ESI_UNKNOWN:
		return "ESI_UNKNOWN";
	case ESI_ARP_PENDING:
		return "ESI_ARP_PENDING";
	case ESI_VC_PENDING:
		return "ESI_VC_PENDING";
	case ESI_FLUSH_PENDING:
		return "ESI_FLUSH_PENDING";
	case ESI_FORWARD_DIRECT:
		return "ESI_FORWARD_DIRECT";
	default:
		return "<UNKNOWN>";
	}
}

static void dump_arp_table(struct lec_priv *priv)
{
	struct hlist_node *node;
	struct lec_arp_table *rulla;
	char buf[256];
	int i, j, offset;

	printk("Dump %p:\n", priv);
	for (i = 0; i < LEC_ARP_TABLE_SIZE; i++) {
		hlist_for_each_entry(rulla, node, &priv->lec_arp_tables[i], next) {
			offset = 0;
			offset += sprintf(buf, "%d: %p\n", i, rulla);
			offset += sprintf(buf + offset, "Mac:");
			for (j = 0; j < ETH_ALEN; j++) {
				offset += sprintf(buf + offset,
						  "%2.2x ",
						  rulla->mac_addr[j] & 0xff);
			}
			offset += sprintf(buf + offset, "Atm:");
			for (j = 0; j < ATM_ESA_LEN; j++) {
				offset += sprintf(buf + offset,
						  "%2.2x ",
						  rulla->atm_addr[j] & 0xff);
			}
			offset += sprintf(buf + offset,
					  "Vcc vpi:%d vci:%d, Recv_vcc vpi:%d vci:%d Last_used:%lx, Timestamp:%lx, No_tries:%d ",
					  rulla->vcc ? rulla->vcc->vpi : 0,
					  rulla->vcc ? rulla->vcc->vci : 0,
					  rulla->recv_vcc ? rulla->recv_vcc->
					  vpi : 0,
					  rulla->recv_vcc ? rulla->recv_vcc->
					  vci : 0, rulla->last_used,
					  rulla->timestamp, rulla->no_tries);
			offset +=
			    sprintf(buf + offset,
				    "Flags:%x, Packets_flooded:%x, Status: %s ",
				    rulla->flags, rulla->packets_flooded,
				    get_status_string(rulla->status));
			printk("%s\n", buf);
		}
	}

	if (!hlist_empty(&priv->lec_no_forward))
		printk("No forward\n");
	hlist_for_each_entry(rulla, node, &priv->lec_no_forward, next) {
		offset = 0;
		offset += sprintf(buf + offset, "Mac:");
		for (j = 0; j < ETH_ALEN; j++) {
			offset += sprintf(buf + offset, "%2.2x ",
					  rulla->mac_addr[j] & 0xff);
		}
		offset += sprintf(buf + offset, "Atm:");
		for (j = 0; j < ATM_ESA_LEN; j++) {
			offset += sprintf(buf + offset, "%2.2x ",
					  rulla->atm_addr[j] & 0xff);
		}
		offset += sprintf(buf + offset,
				  "Vcc vpi:%d vci:%d, Recv_vcc vpi:%d vci:%d Last_used:%lx, Timestamp:%lx, No_tries:%d ",
				  rulla->vcc ? rulla->vcc->vpi : 0,
				  rulla->vcc ? rulla->vcc->vci : 0,
				  rulla->recv_vcc ? rulla->recv_vcc->vpi : 0,
				  rulla->recv_vcc ? rulla->recv_vcc->vci : 0,
				  rulla->last_used,
				  rulla->timestamp, rulla->no_tries);
		offset += sprintf(buf + offset,
				  "Flags:%x, Packets_flooded:%x, Status: %s ",
				  rulla->flags, rulla->packets_flooded,
				  get_status_string(rulla->status));
		printk("%s\n", buf);
	}

	if (!hlist_empty(&priv->lec_arp_empty_ones))
		printk("Empty ones\n");
	hlist_for_each_entry(rulla, node, &priv->lec_arp_empty_ones, next) {
		offset = 0;
		offset += sprintf(buf + offset, "Mac:");
		for (j = 0; j < ETH_ALEN; j++) {
			offset += sprintf(buf + offset, "%2.2x ",
					  rulla->mac_addr[j] & 0xff);
		}
		offset += sprintf(buf + offset, "Atm:");
		for (j = 0; j < ATM_ESA_LEN; j++) {
			offset += sprintf(buf + offset, "%2.2x ",
					  rulla->atm_addr[j] & 0xff);
		}
		offset += sprintf(buf + offset,
				  "Vcc vpi:%d vci:%d, Recv_vcc vpi:%d vci:%d Last_used:%lx, Timestamp:%lx, No_tries:%d ",
				  rulla->vcc ? rulla->vcc->vpi : 0,
				  rulla->vcc ? rulla->vcc->vci : 0,
				  rulla->recv_vcc ? rulla->recv_vcc->vpi : 0,
				  rulla->recv_vcc ? rulla->recv_vcc->vci : 0,
				  rulla->last_used,
				  rulla->timestamp, rulla->no_tries);
		offset += sprintf(buf + offset,
				  "Flags:%x, Packets_flooded:%x, Status: %s ",
				  rulla->flags, rulla->packets_flooded,
				  get_status_string(rulla->status));
		printk("%s", buf);
	}

	if (!hlist_empty(&priv->mcast_fwds))
		printk("Multicast Forward VCCs\n");
	hlist_for_each_entry(rulla, node, &priv->mcast_fwds, next) {
		offset = 0;
		offset += sprintf(buf + offset, "Mac:");
		for (j = 0; j < ETH_ALEN; j++) {
			offset += sprintf(buf + offset, "%2.2x ",
					  rulla->mac_addr[j] & 0xff);
		}
		offset += sprintf(buf + offset, "Atm:");
		for (j = 0; j < ATM_ESA_LEN; j++) {
			offset += sprintf(buf + offset, "%2.2x ",
					  rulla->atm_addr[j] & 0xff);
		}
		offset += sprintf(buf + offset,
				  "Vcc vpi:%d vci:%d, Recv_vcc vpi:%d vci:%d Last_used:%lx, Timestamp:%lx, No_tries:%d ",
				  rulla->vcc ? rulla->vcc->vpi : 0,
				  rulla->vcc ? rulla->vcc->vci : 0,
				  rulla->recv_vcc ? rulla->recv_vcc->vpi : 0,
				  rulla->recv_vcc ? rulla->recv_vcc->vci : 0,
				  rulla->last_used,
				  rulla->timestamp, rulla->no_tries);
		offset += sprintf(buf + offset,
				  "Flags:%x, Packets_flooded:%x, Status: %s ",
				  rulla->flags, rulla->packets_flooded,
				  get_status_string(rulla->status));
		printk("%s\n", buf);
	}

}
#else
#define dump_arp_table(priv) do { } while (0)
#endif

/*
 * Destruction of arp-cache
 */
static void lec_arp_destroy(struct lec_priv *priv)
{
	unsigned long flags;
	struct hlist_node *node, *next;
	struct lec_arp_table *entry;
	int i;

	cancel_rearming_delayed_work(&priv->lec_arp_work);

	/*
	 * Remove all entries
	 */

	spin_lock_irqsave(&priv->lec_arp_lock, flags);
	for (i = 0; i < LEC_ARP_TABLE_SIZE; i++) {
		hlist_for_each_entry_safe(entry, node, next, &priv->lec_arp_tables[i], next) {
			lec_arp_remove(priv, entry);
			lec_arp_put(entry);
		}
		INIT_HLIST_HEAD(&priv->lec_arp_tables[i]);
	}

	hlist_for_each_entry_safe(entry, node, next, &priv->lec_arp_empty_ones, next) {
		del_timer_sync(&entry->timer);
		lec_arp_clear_vccs(entry);
		hlist_del(&entry->next);
		lec_arp_put(entry);
	}
	INIT_HLIST_HEAD(&priv->lec_arp_empty_ones);

	hlist_for_each_entry_safe(entry, node, next, &priv->lec_no_forward, next) {
		del_timer_sync(&entry->timer);
		lec_arp_clear_vccs(entry);
		hlist_del(&entry->next);
		lec_arp_put(entry);
	}
	INIT_HLIST_HEAD(&priv->lec_no_forward);

	hlist_for_each_entry_safe(entry, node, next, &priv->mcast_fwds, next) {
		/* No timer, LANEv2 7.1.20 and 2.3.5.3 */
		lec_arp_clear_vccs(entry);
		hlist_del(&entry->next);
		lec_arp_put(entry);
	}
	INIT_HLIST_HEAD(&priv->mcast_fwds);
	priv->mcast_vcc = NULL;
	spin_unlock_irqrestore(&priv->lec_arp_lock, flags);
}

/*
 * Find entry by mac_address
 */
static struct lec_arp_table *lec_arp_find(struct lec_priv *priv,
					  unsigned char *mac_addr)
{
	struct hlist_node *node;
	struct hlist_head *head;
	struct lec_arp_table *entry;

	DPRINTK("LEC_ARP: lec_arp_find :%2.2x %2.2x %2.2x %2.2x %2.2x %2.2x\n",
		mac_addr[0] & 0xff, mac_addr[1] & 0xff, mac_addr[2] & 0xff,
		mac_addr[3] & 0xff, mac_addr[4] & 0xff, mac_addr[5] & 0xff);

	head = &priv->lec_arp_tables[HASH(mac_addr[ETH_ALEN - 1])];
	hlist_for_each_entry(entry, node, head, next) {
		if (!compare_ether_addr(mac_addr, entry->mac_addr)) {
			return entry;
		}
	}
	return NULL;
}

static struct lec_arp_table *make_entry(struct lec_priv *priv,
					unsigned char *mac_addr)
{
	struct lec_arp_table *to_return;

	to_return = kzalloc(sizeof(struct lec_arp_table), GFP_ATOMIC);
	if (!to_return) {
		printk("LEC: Arp entry kmalloc failed\n");
		return NULL;
	}
	memcpy(to_return->mac_addr, mac_addr, ETH_ALEN);
	INIT_HLIST_NODE(&to_return->next);
	init_timer(&to_return->timer);
	to_return->timer.function = lec_arp_expire_arp;
	to_return->timer.data = (unsigned long)to_return;
	to_return->last_used = jiffies;
	to_return->priv = priv;
	skb_queue_head_init(&to_return->tx_wait);
	atomic_set(&to_return->usage, 1);
	return to_return;
}

/* Arp sent timer expired */
static void lec_arp_expire_arp(unsigned long data)
{
	struct lec_arp_table *entry;

	entry = (struct lec_arp_table *)data;

	DPRINTK("lec_arp_expire_arp\n");
	if (entry->status == ESI_ARP_PENDING) {
		if (entry->no_tries <= entry->priv->max_retry_count) {
			if (entry->is_rdesc)
				send_to_lecd(entry->priv, l_rdesc_arp_xmt,
					     entry->mac_addr, NULL, NULL);
			else
				send_to_lecd(entry->priv, l_arp_xmt,
					     entry->mac_addr, NULL, NULL);
			entry->no_tries++;
		}
		mod_timer(&entry->timer, jiffies + (1 * HZ));
	}
}

/* Unknown/unused vcc expire, remove associated entry */
static void lec_arp_expire_vcc(unsigned long data)
{
	unsigned long flags;
	struct lec_arp_table *to_remove = (struct lec_arp_table *)data;
	struct lec_priv *priv = (struct lec_priv *)to_remove->priv;

	del_timer(&to_remove->timer);

	DPRINTK("LEC_ARP %p %p: lec_arp_expire_vcc vpi:%d vci:%d\n",
		to_remove, priv,
		to_remove->vcc ? to_remove->recv_vcc->vpi : 0,
		to_remove->vcc ? to_remove->recv_vcc->vci : 0);

	spin_lock_irqsave(&priv->lec_arp_lock, flags);
	hlist_del(&to_remove->next);
	spin_unlock_irqrestore(&priv->lec_arp_lock, flags);

	lec_arp_clear_vccs(to_remove);
	lec_arp_put(to_remove);
}

/*
 * Expire entries.
 * 1. Re-set timer
 * 2. For each entry, delete entries that have aged past the age limit.
 * 3. For each entry, depending on the status of the entry, perform
 *    the following maintenance.
 *    a. If status is ESI_VC_PENDING or ESI_ARP_PENDING then if the
 *       tick_count is above the max_unknown_frame_time, clear
 *       the tick_count to zero and clear the packets_flooded counter
 *       to zero. This supports the packet rate limit per address
 *       while flooding unknowns.
 *    b. If the status is ESI_FLUSH_PENDING and the tick_count is greater
 *       than or equal to the path_switching_delay, change the status
 *       to ESI_FORWARD_DIRECT. This causes the flush period to end
 *       regardless of the progress of the flush protocol.
 */
static void lec_arp_check_expire(struct work_struct *work)
{
	unsigned long flags;
	struct lec_priv *priv =
		container_of(work, struct lec_priv, lec_arp_work.work);
	struct hlist_node *node, *next;
	struct lec_arp_table *entry;
	unsigned long now;
	unsigned long time_to_check;
	int i;

	DPRINTK("lec_arp_check_expire %p\n", priv);
	now = jiffies;
restart:
	spin_lock_irqsave(&priv->lec_arp_lock, flags);
	for (i = 0; i < LEC_ARP_TABLE_SIZE; i++) {
		hlist_for_each_entry_safe(entry, node, next, &priv->lec_arp_tables[i], next) {
			if ((entry->flags) & LEC_REMOTE_FLAG &&
			    priv->topology_change)
				time_to_check = priv->forward_delay_time;
			else
				time_to_check = priv->aging_time;

			DPRINTK("About to expire: %lx - %lx > %lx\n",
				now, entry->last_used, time_to_check);
			if (time_after(now, entry->last_used + time_to_check)
			    && !(entry->flags & LEC_PERMANENT_FLAG)
			    && !(entry->mac_addr[0] & 0x01)) {	/* LANE2: 7.1.20 */
				/* Remove entry */
				DPRINTK("LEC:Entry timed out\n");
				lec_arp_remove(priv, entry);
				lec_arp_put(entry);
			} else {
				/* Something else */
				if ((entry->status == ESI_VC_PENDING ||
				     entry->status == ESI_ARP_PENDING)
				    && time_after_eq(now,
						     entry->timestamp +
						     priv->
						     max_unknown_frame_time)) {
					entry->timestamp = jiffies;
					entry->packets_flooded = 0;
					if (entry->status == ESI_VC_PENDING)
						send_to_lecd(priv, l_svc_setup,
							     entry->mac_addr,
							     entry->atm_addr,
							     NULL);
				}
				if (entry->status == ESI_FLUSH_PENDING
				    &&
				    time_after_eq(now, entry->timestamp +
						  priv->path_switching_delay)) {
					struct sk_buff *skb;
					struct atm_vcc *vcc = entry->vcc;

					lec_arp_hold(entry);
					spin_unlock_irqrestore(&priv->lec_arp_lock, flags);
					while ((skb = skb_dequeue(&entry->tx_wait)) != NULL)
						lec_send(vcc, skb, entry->priv);
					entry->last_used = jiffies;
					entry->status = ESI_FORWARD_DIRECT;
					lec_arp_put(entry);
					goto restart;
				}
			}
		}
	}
	spin_unlock_irqrestore(&priv->lec_arp_lock, flags);

	schedule_delayed_work(&priv->lec_arp_work, LEC_ARP_REFRESH_INTERVAL);
}

/*
 * Try to find vcc where mac_address is attached.
 *
 */
static struct atm_vcc *lec_arp_resolve(struct lec_priv *priv,
				       unsigned char *mac_to_find, int is_rdesc,
				       struct lec_arp_table **ret_entry)
{
	unsigned long flags;
	struct lec_arp_table *entry;
	struct atm_vcc *found;

	if (mac_to_find[0] & 0x01) {
		switch (priv->lane_version) {
		case 1:
			return priv->mcast_vcc;
			break;
		case 2:	/* LANE2 wants arp for multicast addresses */
			if (!compare_ether_addr(mac_to_find, bus_mac))
				return priv->mcast_vcc;
			break;
		default:
			break;
		}
	}

	spin_lock_irqsave(&priv->lec_arp_lock, flags);
	entry = lec_arp_find(priv, mac_to_find);

	if (entry) {
		if (entry->status == ESI_FORWARD_DIRECT) {
			/* Connection Ok */
			entry->last_used = jiffies;
			lec_arp_hold(entry);
			*ret_entry = entry;
			found = entry->vcc;
			goto out;
		}
		/*
		 * If the LE_ARP cache entry is still pending, reset count to 0
		 * so another LE_ARP request can be made for this frame.
		 */
		if (entry->status == ESI_ARP_PENDING) {
			entry->no_tries = 0;
		}
		/*
		 * Data direct VC not yet set up, check to see if the unknown
		 * frame count is greater than the limit. If the limit has
		 * not been reached, allow the caller to send packet to
		 * BUS.
		 */
		if (entry->status != ESI_FLUSH_PENDING &&
		    entry->packets_flooded <
		    priv->maximum_unknown_frame_count) {
			entry->packets_flooded++;
			DPRINTK("LEC_ARP: Flooding..\n");
			found = priv->mcast_vcc;
			goto out;
		}
		/*
		 * We got here because entry->status == ESI_FLUSH_PENDING
		 * or BUS flood limit was reached for an entry which is
		 * in ESI_ARP_PENDING or ESI_VC_PENDING state.
		 */
		lec_arp_hold(entry);
		*ret_entry = entry;
		DPRINTK("lec: entry->status %d entry->vcc %p\n", entry->status,
			entry->vcc);
		found = NULL;
	} else {
		/* No matching entry was found */
		entry = make_entry(priv, mac_to_find);
		DPRINTK("LEC_ARP: Making entry\n");
		if (!entry) {
			found = priv->mcast_vcc;
			goto out;
		}
		lec_arp_add(priv, entry);
		/* We want arp-request(s) to be sent */
		entry->packets_flooded = 1;
		entry->status = ESI_ARP_PENDING;
		entry->no_tries = 1;
		entry->last_used = entry->timestamp = jiffies;
		entry->is_rdesc = is_rdesc;
		if (entry->is_rdesc)
			send_to_lecd(priv, l_rdesc_arp_xmt, mac_to_find, NULL,
				     NULL);
		else
			send_to_lecd(priv, l_arp_xmt, mac_to_find, NULL, NULL);
		entry->timer.expires = jiffies + (1 * HZ);
		entry->timer.function = lec_arp_expire_arp;
		add_timer(&entry->timer);
		found = priv->mcast_vcc;
	}

out:
	spin_unlock_irqrestore(&priv->lec_arp_lock, flags);
	return found;
}

static int
lec_addr_delete(struct lec_priv *priv, unsigned char *atm_addr,
		unsigned long permanent)
{
	unsigned long flags;
	struct hlist_node *node, *next;
	struct lec_arp_table *entry;
	int i;

	DPRINTK("lec_addr_delete\n");
	spin_lock_irqsave(&priv->lec_arp_lock, flags);
	for (i = 0; i < LEC_ARP_TABLE_SIZE; i++) {
		hlist_for_each_entry_safe(entry, node, next, &priv->lec_arp_tables[i], next) {
			if (!memcmp(atm_addr, entry->atm_addr, ATM_ESA_LEN)
			    && (permanent ||
				!(entry->flags & LEC_PERMANENT_FLAG))) {
				lec_arp_remove(priv, entry);
				lec_arp_put(entry);
			}
			spin_unlock_irqrestore(&priv->lec_arp_lock, flags);
			return 0;
		}
	}
	spin_unlock_irqrestore(&priv->lec_arp_lock, flags);
	return -1;
}

/*
 * Notifies:  Response to arp_request (atm_addr != NULL)
 */
static void
lec_arp_update(struct lec_priv *priv, unsigned char *mac_addr,
	       unsigned char *atm_addr, unsigned long remoteflag,
	       unsigned int targetless_le_arp)
{
	unsigned long flags;
	struct hlist_node *node, *next;
	struct lec_arp_table *entry, *tmp;
	int i;

	DPRINTK("lec:%s", (targetless_le_arp) ? "targetless " : " ");
	DPRINTK("lec_arp_update mac:%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x\n",
		mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3],
		mac_addr[4], mac_addr[5]);

	spin_lock_irqsave(&priv->lec_arp_lock, flags);
	entry = lec_arp_find(priv, mac_addr);
	if (entry == NULL && targetless_le_arp)
		goto out;	/*
				 * LANE2: ignore targetless LE_ARPs for which
				 * we have no entry in the cache. 7.1.30
				 */
	if (!hlist_empty(&priv->lec_arp_empty_ones)) {
		hlist_for_each_entry_safe(entry, node, next, &priv->lec_arp_empty_ones, next) {
			if (memcmp(entry->atm_addr, atm_addr, ATM_ESA_LEN) == 0) {
				hlist_del(&entry->next);
				del_timer(&entry->timer);
				tmp = lec_arp_find(priv, mac_addr);
				if (tmp) {
					del_timer(&tmp->timer);
					tmp->status = ESI_FORWARD_DIRECT;
					memcpy(tmp->atm_addr, atm_addr, ATM_ESA_LEN);
					tmp->vcc = entry->vcc;
					tmp->old_push = entry->old_push;
					tmp->last_used = jiffies;
					del_timer(&entry->timer);
					lec_arp_put(entry);
					entry = tmp;
				} else {
					entry->status = ESI_FORWARD_DIRECT;
					memcpy(entry->mac_addr, mac_addr, ETH_ALEN);
					entry->last_used = jiffies;
					lec_arp_add(priv, entry);
				}
				if (remoteflag)
					entry->flags |= LEC_REMOTE_FLAG;
				else
					entry->flags &= ~LEC_REMOTE_FLAG;
				DPRINTK("After update\n");
				dump_arp_table(priv);
				goto out;
			}
		}
	}

	entry = lec_arp_find(priv, mac_addr);
	if (!entry) {
		entry = make_entry(priv, mac_addr);
		if (!entry)
			goto out;
		entry->status = ESI_UNKNOWN;
		lec_arp_add(priv, entry);
		/* Temporary, changes before end of function */
	}
	memcpy(entry->atm_addr, atm_addr, ATM_ESA_LEN);
	del_timer(&entry->timer);
	for (i = 0; i < LEC_ARP_TABLE_SIZE; i++) {
		hlist_for_each_entry(tmp, node, &priv->lec_arp_tables[i], next) {
			if (entry != tmp &&
			    !memcmp(tmp->atm_addr, atm_addr, ATM_ESA_LEN)) {
				/* Vcc to this host exists */
				if (tmp->status > ESI_VC_PENDING) {
					/*
					 * ESI_FLUSH_PENDING,
					 * ESI_FORWARD_DIRECT
					 */
					entry->vcc = tmp->vcc;
					entry->old_push = tmp->old_push;
				}
				entry->status = tmp->status;
				break;
			}
		}
	}
	if (remoteflag)
		entry->flags |= LEC_REMOTE_FLAG;
	else
		entry->flags &= ~LEC_REMOTE_FLAG;
	if (entry->status == ESI_ARP_PENDING || entry->status == ESI_UNKNOWN) {
		entry->status = ESI_VC_PENDING;
		send_to_lecd(priv, l_svc_setup, entry->mac_addr, atm_addr, NULL);
	}
	DPRINTK("After update2\n");
	dump_arp_table(priv);
out:
	spin_unlock_irqrestore(&priv->lec_arp_lock, flags);
}

/*
 * Notifies: Vcc setup ready
 */
static void
lec_vcc_added(struct lec_priv *priv, struct atmlec_ioc *ioc_data,
	      struct atm_vcc *vcc,
	      void (*old_push) (struct atm_vcc *vcc, struct sk_buff *skb))
{
	unsigned long flags;
	struct hlist_node *node;
	struct lec_arp_table *entry;
	int i, found_entry = 0;

	spin_lock_irqsave(&priv->lec_arp_lock, flags);
	if (ioc_data->receive == 2) {
		/* Vcc for Multicast Forward. No timer, LANEv2 7.1.20 and 2.3.5.3 */

		DPRINTK("LEC_ARP: Attaching mcast forward\n");
#if 0
		entry = lec_arp_find(priv, bus_mac);
		if (!entry) {
			printk("LEC_ARP: Multicast entry not found!\n");
			goto out;
		}
		memcpy(entry->atm_addr, ioc_data->atm_addr, ATM_ESA_LEN);
		entry->recv_vcc = vcc;
		entry->old_recv_push = old_push;
#endif
		entry = make_entry(priv, bus_mac);
		if (entry == NULL)
			goto out;
		del_timer(&entry->timer);
		memcpy(entry->atm_addr, ioc_data->atm_addr, ATM_ESA_LEN);
		entry->recv_vcc = vcc;
		entry->old_recv_push = old_push;
		hlist_add_head(&entry->next, &priv->mcast_fwds);
		goto out;
	} else if (ioc_data->receive == 1) {
		/*
		 * Vcc which we don't want to make default vcc,
		 * attach it anyway.
		 */
		DPRINTK
		    ("LEC_ARP:Attaching data direct, not default: "
		     "%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x\n",
		     ioc_data->atm_addr[0], ioc_data->atm_addr[1],
		     ioc_data->atm_addr[2], ioc_data->atm_addr[3],
		     ioc_data->atm_addr[4], ioc_data->atm_addr[5],
		     ioc_data->atm_addr[6], ioc_data->atm_addr[7],
		     ioc_data->atm_addr[8], ioc_data->atm_addr[9],
		     ioc_data->atm_addr[10], ioc_data->atm_addr[11],
		     ioc_data->atm_addr[12], ioc_data->atm_addr[13],
		     ioc_data->atm_addr[14], ioc_data->atm_addr[15],
		     ioc_data->atm_addr[16], ioc_data->atm_addr[17],
		     ioc_data->atm_addr[18], ioc_data->atm_addr[19]);
		entry = make_entry(priv, bus_mac);
		if (entry == NULL)
			goto out;
		memcpy(entry->atm_addr, ioc_data->atm_addr, ATM_ESA_LEN);
		memset(entry->mac_addr, 0, ETH_ALEN);
		entry->recv_vcc = vcc;
		entry->old_recv_push = old_push;
		entry->status = ESI_UNKNOWN;
		entry->timer.expires = jiffies + priv->vcc_timeout_period;
		entry->timer.function = lec_arp_expire_vcc;
		hlist_add_head(&entry->next, &priv->lec_no_forward);
		add_timer(&entry->timer);
		dump_arp_table(priv);
		goto out;
	}
	DPRINTK
	    ("LEC_ARP:Attaching data direct, default: "
	     "%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x\n",
	     ioc_data->atm_addr[0], ioc_data->atm_addr[1],
	     ioc_data->atm_addr[2], ioc_data->atm_addr[3],
	     ioc_data->atm_addr[4], ioc_data->atm_addr[5],
	     ioc_data->atm_addr[6], ioc_data->atm_addr[7],
	     ioc_data->atm_addr[8], ioc_data->atm_addr[9],
	     ioc_data->atm_addr[10], ioc_data->atm_addr[11],
	     ioc_data->atm_addr[12], ioc_data->atm_addr[13],
	     ioc_data->atm_addr[14], ioc_data->atm_addr[15],
	     ioc_data->atm_addr[16], ioc_data->atm_addr[17],
	     ioc_data->atm_addr[18], ioc_data->atm_addr[19]);
	for (i = 0; i < LEC_ARP_TABLE_SIZE; i++) {
		hlist_for_each_entry(entry, node, &priv->lec_arp_tables[i], next) {
			if (memcmp
			    (ioc_data->atm_addr, entry->atm_addr,
			     ATM_ESA_LEN) == 0) {
				DPRINTK("LEC_ARP: Attaching data direct\n");
				DPRINTK("Currently -> Vcc: %d, Rvcc:%d\n",
					entry->vcc ? entry->vcc->vci : 0,
					entry->recv_vcc ? entry->recv_vcc->
					vci : 0);
				found_entry = 1;
				del_timer(&entry->timer);
				entry->vcc = vcc;
				entry->old_push = old_push;
				if (entry->status == ESI_VC_PENDING) {
					if (priv->maximum_unknown_frame_count
					    == 0)
						entry->status =
						    ESI_FORWARD_DIRECT;
					else {
						entry->timestamp = jiffies;
						entry->status =
						    ESI_FLUSH_PENDING;
#if 0
						send_to_lecd(priv, l_flush_xmt,
							     NULL,
							     entry->atm_addr,
							     NULL);
#endif
					}
				} else {
					/*
					 * They were forming a connection
					 * to us, and we to them. Our
					 * ATM address is numerically lower
					 * than theirs, so we make connection
					 * we formed into default VCC (8.1.11).
					 * Connection they made gets torn
					 * down. This might confuse some
					 * clients. Can be changed if
					 * someone reports trouble...
					 */
					;
				}
			}
		}
	}
	if (found_entry) {
		DPRINTK("After vcc was added\n");
		dump_arp_table(priv);
		goto out;
	}
	/*
	 * Not found, snatch address from first data packet that arrives
	 * from this vcc
	 */
	entry = make_entry(priv, bus_mac);
	if (!entry)
		goto out;
	entry->vcc = vcc;
	entry->old_push = old_push;
	memcpy(entry->atm_addr, ioc_data->atm_addr, ATM_ESA_LEN);
	memset(entry->mac_addr, 0, ETH_ALEN);
	entry->status = ESI_UNKNOWN;
	hlist_add_head(&entry->next, &priv->lec_arp_empty_ones);
	entry->timer.expires = jiffies + priv->vcc_timeout_period;
	entry->timer.function = lec_arp_expire_vcc;
	add_timer(&entry->timer);
	DPRINTK("After vcc was added\n");
	dump_arp_table(priv);
out:
	spin_unlock_irqrestore(&priv->lec_arp_lock, flags);
}

static void lec_flush_complete(struct lec_priv *priv, unsigned long tran_id)
{
	unsigned long flags;
	struct hlist_node *node;
	struct lec_arp_table *entry;
	int i;

	DPRINTK("LEC:lec_flush_complete %lx\n", tran_id);
restart:
	spin_lock_irqsave(&priv->lec_arp_lock, flags);
	for (i = 0; i < LEC_ARP_TABLE_SIZE; i++) {
		hlist_for_each_entry(entry, node, &priv->lec_arp_tables[i], next) {
			if (entry->flush_tran_id == tran_id
			    && entry->status == ESI_FLUSH_PENDING) {
				struct sk_buff *skb;
				struct atm_vcc *vcc = entry->vcc;

				lec_arp_hold(entry);
				spin_unlock_irqrestore(&priv->lec_arp_lock, flags);
				while ((skb = skb_dequeue(&entry->tx_wait)) != NULL)
					lec_send(vcc, skb, entry->priv);
				entry->last_used = jiffies;
				entry->status = ESI_FORWARD_DIRECT;
				lec_arp_put(entry);
				DPRINTK("LEC_ARP: Flushed\n");
				goto restart;
			}
		}
	}
	spin_unlock_irqrestore(&priv->lec_arp_lock, flags);
	dump_arp_table(priv);
}

static void
lec_set_flush_tran_id(struct lec_priv *priv,
		      unsigned char *atm_addr, unsigned long tran_id)
{
	unsigned long flags;
	struct hlist_node *node;
	struct lec_arp_table *entry;
	int i;

	spin_lock_irqsave(&priv->lec_arp_lock, flags);
	for (i = 0; i < LEC_ARP_TABLE_SIZE; i++)
		hlist_for_each_entry(entry, node, &priv->lec_arp_tables[i], next) {
			if (!memcmp(atm_addr, entry->atm_addr, ATM_ESA_LEN)) {
				entry->flush_tran_id = tran_id;
				DPRINTK("Set flush transaction id to %lx for %p\n",
					tran_id, entry);
			}
		}
	spin_unlock_irqrestore(&priv->lec_arp_lock, flags);
}

static int lec_mcast_make(struct lec_priv *priv, struct atm_vcc *vcc)
{
	unsigned long flags;
	unsigned char mac_addr[] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff
	};
	struct lec_arp_table *to_add;
	struct lec_vcc_priv *vpriv;
	int err = 0;

	if (!(vpriv = kmalloc(sizeof(struct lec_vcc_priv), GFP_KERNEL)))
		return -ENOMEM;
	vpriv->xoff = 0;
	vpriv->old_pop = vcc->pop;
	vcc->user_back = vpriv;
	vcc->pop = lec_pop;
	spin_lock_irqsave(&priv->lec_arp_lock, flags);
	to_add = make_entry(priv, mac_addr);
	if (!to_add) {
		vcc->pop = vpriv->old_pop;
		kfree(vpriv);
		err = -ENOMEM;
		goto out;
	}
	memcpy(to_add->atm_addr, vcc->remote.sas_addr.prv, ATM_ESA_LEN);
	to_add->status = ESI_FORWARD_DIRECT;
	to_add->flags |= LEC_PERMANENT_FLAG;
	to_add->vcc = vcc;
	to_add->old_push = vcc->push;
	vcc->push = lec_push;
	priv->mcast_vcc = vcc;
	lec_arp_add(priv, to_add);
out:
	spin_unlock_irqrestore(&priv->lec_arp_lock, flags);
	return err;
}

static void lec_vcc_close(struct lec_priv *priv, struct atm_vcc *vcc)
{
	unsigned long flags;
	struct hlist_node *node, *next;
	struct lec_arp_table *entry;
	int i;

	DPRINTK("LEC_ARP: lec_vcc_close vpi:%d vci:%d\n", vcc->vpi, vcc->vci);
	dump_arp_table(priv);

	spin_lock_irqsave(&priv->lec_arp_lock, flags);

	for (i = 0; i < LEC_ARP_TABLE_SIZE; i++) {
		hlist_for_each_entry_safe(entry, node, next, &priv->lec_arp_tables[i], next) {
			if (vcc == entry->vcc) {
				lec_arp_remove(priv, entry);
				lec_arp_put(entry);
				if (priv->mcast_vcc == vcc) {
					priv->mcast_vcc = NULL;
				}
			}
		}
	}

	hlist_for_each_entry_safe(entry, node, next, &priv->lec_arp_empty_ones, next) {
		if (entry->vcc == vcc) {
			lec_arp_clear_vccs(entry);
			del_timer(&entry->timer);
			hlist_del(&entry->next);
			lec_arp_put(entry);
		}
	}

	hlist_for_each_entry_safe(entry, node, next, &priv->lec_no_forward, next) {
		if (entry->recv_vcc == vcc) {
			lec_arp_clear_vccs(entry);
			del_timer(&entry->timer);
			hlist_del(&entry->next);
			lec_arp_put(entry);
		}
	}

	hlist_for_each_entry_safe(entry, node, next, &priv->mcast_fwds, next) {
		if (entry->recv_vcc == vcc) {
			lec_arp_clear_vccs(entry);
			/* No timer, LANEv2 7.1.20 and 2.3.5.3 */
			hlist_del(&entry->next);
			lec_arp_put(entry);
		}
	}

	spin_unlock_irqrestore(&priv->lec_arp_lock, flags);
	dump_arp_table(priv);
}

static void
lec_arp_check_empties(struct lec_priv *priv,
		      struct atm_vcc *vcc, struct sk_buff *skb)
{
	unsigned long flags;
	struct hlist_node *node, *next;
	struct lec_arp_table *entry, *tmp;
	struct lecdatahdr_8023 *hdr = (struct lecdatahdr_8023 *)skb->data;
	unsigned char *src;
#ifdef CONFIG_TR
	struct lecdatahdr_8025 *tr_hdr = (struct lecdatahdr_8025 *)skb->data;

	if (priv->is_trdev)
		src = tr_hdr->h_source;
	else
#endif
		src = hdr->h_source;

	spin_lock_irqsave(&priv->lec_arp_lock, flags);
	hlist_for_each_entry_safe(entry, node, next, &priv->lec_arp_empty_ones, next) {
		if (vcc == entry->vcc) {
			del_timer(&entry->timer);
			memcpy(entry->mac_addr, src, ETH_ALEN);
			entry->status = ESI_FORWARD_DIRECT;
			entry->last_used = jiffies;
			/* We might have got an entry */
			if ((tmp = lec_arp_find(priv, src))) {
				lec_arp_remove(priv, tmp);
				lec_arp_put(tmp);
			}
			hlist_del(&entry->next);
			lec_arp_add(priv, entry);
			goto out;
		}
	}
	DPRINTK("LEC_ARP: Arp_check_empties: entry not found!\n");
out:
	spin_unlock_irqrestore(&priv->lec_arp_lock, flags);
}

MODULE_LICENSE("GPL");
