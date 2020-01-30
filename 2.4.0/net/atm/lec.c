/*
 * lec.c: Lan Emulation driver 
 * Marko Kiiskila carnil@cs.tut.fi
 *
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/bitops.h>

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
static unsigned char bridge_ula_lec[] = {0x01, 0x80, 0xc2, 0x00, 0x00};
#endif

/* Modular too */
#include <linux/module.h>
#include <linux/init.h>

#include "lec.h"
#include "lec_arpc.h"
#include "resources.h"  /* for bind_vcc() */

#if 0
#define DPRINTK printk
#else
#define DPRINTK(format,args...)
#endif

#define DUMP_PACKETS 0 /* 0 = None,
                        * 1 = 30 first bytes
                        * 2 = Whole packet
                        */

#define LEC_UNRES_QUE_LEN 8 /* number of tx packets to queue for a
                               single destination while waiting for SVC */

static int lec_open(struct net_device *dev);
static int lec_send_packet(struct sk_buff *skb, struct net_device *dev);
static int lec_close(struct net_device *dev);
static struct net_device_stats *lec_get_stats(struct net_device *dev);
static void lec_init(struct net_device *dev);
static __inline__ struct lec_arp_table* lec_arp_find(struct lec_priv *priv,
                                                     unsigned char *mac_addr);
static __inline__ int lec_arp_remove(struct lec_arp_table **lec_arp_tables,
				     struct lec_arp_table *to_remove);
/* LANE2 functions */
static void lane2_associate_ind (struct net_device *dev, u8 *mac_address,
                          u8 *tlvs, u32 sizeoftlvs);
static int lane2_resolve(struct net_device *dev, u8 *dst_mac, int force,
                  u8 **tlvs, u32 *sizeoftlvs);
static int lane2_associate_req (struct net_device *dev, u8 *lan_dst,
                         u8 *tlvs, u32 sizeoftlvs);

static struct lane2_ops lane2_ops = {
	lane2_resolve,         /* resolve,             spec 3.1.3 */
	lane2_associate_req,   /* associate_req,       spec 3.1.4 */
	NULL                  /* associate indicator, spec 3.1.5 */
};

static unsigned char bus_mac[ETH_ALEN] = {0xff,0xff,0xff,0xff,0xff,0xff};

/* Device structures */
static struct net_device *dev_lec[MAX_LEC_ITF];

/* This will be called from proc.c via function pointer */
struct net_device **get_dev_lec (void) {
        return &dev_lec[0];
}

#if defined(CONFIG_BRIDGE) || defined(CONFIG_BRIDGE_MODULE)
static void lec_handle_bridge(struct sk_buff *skb, struct net_device *dev)
{
        struct ethhdr *eth;
        char *buff;
        struct lec_priv *priv;

        /* Check if this is a BPDU. If so, ask zeppelin to send
         * LE_TOPOLOGY_REQUEST with the same value of Topology Change bit
         * as the Config BPDU has */
        eth = (struct ethhdr *)skb->data;
        buff = skb->data + skb->dev->hard_header_len;
        if (*buff++ == 0x42 && *buff++ == 0x42 && *buff++ == 0x03) {
                struct sk_buff *skb2;
                struct atmlec_msg *mesg;

                skb2 = alloc_skb(sizeof(struct atmlec_msg), GFP_ATOMIC);
                if (skb2 == NULL) return;
                skb2->len = sizeof(struct atmlec_msg);
                mesg = (struct atmlec_msg *)skb2->data;
                mesg->type = l_topology_change;
                buff += 4;
                mesg->content.normal.flag = *buff & 0x01; /* 0x01 is topology change */

                priv = (struct lec_priv *)dev->priv;
                atm_force_charge(priv->lecd, skb2->truesize);
                skb_queue_tail(&priv->lecd->recvq, skb2);
                wake_up(&priv->lecd->sleep);
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
unsigned char *get_tr_dst(unsigned char *packet, unsigned char *rdesc)
{
        struct trh_hdr *trh;
        int riflen, num_rdsc;
        
        trh = (struct trh_hdr *)packet;
        if (trh->daddr[0] & (uint8_t)0x80)
                return bus_mac; /* multicast */

        if (trh->saddr[0] & TR_RII) {
                riflen = (ntohs(trh->rcf) & TR_RCF_LEN_MASK) >> 8;
                if ((ntohs(trh->rcf) >> 13) != 0)
                        return bus_mac; /* ARE or STE */
        }
        else
                return trh->daddr; /* not source routed */

        if (riflen < 6)
                return trh->daddr; /* last hop, source routed */
                
        /* riflen is 6 or more, packet has more than one route descriptor */
        num_rdsc = (riflen/2) - 1;
        memset(rdesc, 0, ETH_ALEN);
        /* offset 4 comes from LAN destination field in LE control frames */
        if (trh->rcf & htons((uint16_t)TR_RCF_DIR_BIT))
                memcpy(&rdesc[4], &trh->rseg[num_rdsc-2], sizeof(uint16_t));
        else {
                memcpy(&rdesc[4], &trh->rseg[1], sizeof(uint16_t));
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

static int 
lec_open(struct net_device *dev)
{
        struct lec_priv *priv = (struct lec_priv *)dev->priv;
        
	netif_start_queue(dev);
        memset(&priv->stats,0,sizeof(struct net_device_stats));
        
        return 0;
}

static int 
lec_send_packet(struct sk_buff *skb, struct net_device *dev)
{
        struct sk_buff *skb2;
        struct lec_priv *priv = (struct lec_priv *)dev->priv;
        struct lecdatahdr_8023 *lec_h;
        struct atm_vcc *send_vcc;
	struct lec_arp_table *entry;
        unsigned char *nb, *dst;
#ifdef CONFIG_TR
        unsigned char rdesc[ETH_ALEN]; /* Token Ring route descriptor */
#endif
        int is_rdesc;
#if DUMP_PACKETS > 0
        char buf[300];
        int i=0;
#endif /* DUMP_PACKETS >0 */
        
        DPRINTK("Lec_send_packet called\n");  
        if (!priv->lecd) {
                printk("%s:No lecd attached\n",dev->name);
                priv->stats.tx_errors++;
                netif_stop_queue(dev);
                return -EUNATCH;
        } 

        DPRINTK("skbuff head:%lx data:%lx tail:%lx end:%lx\n",
                (long)skb->head, (long)skb->data, (long)skb->tail,
                (long)skb->end);
#if defined(CONFIG_BRIDGE) || defined(CONFIG_BRIDGE_MODULE)
        if (memcmp(skb->data, bridge_ula_lec, sizeof(bridge_ula_lec)) == 0)
                lec_handle_bridge(skb, dev);
#endif

        /* Make sure we have room for lec_id */
        if (skb_headroom(skb) < 2) {

                DPRINTK("lec_send_packet: reallocating skb\n");
                skb2 = skb_realloc_headroom(skb, LEC_HEADER_LEN);
                kfree_skb(skb);
                if (skb2 == NULL) return 0;
                skb = skb2;
        }
        skb_push(skb, 2);

        /* Put le header to place, works for TokenRing too */
        lec_h = (struct lecdatahdr_8023*)skb->data;
        lec_h->le_header = htons(priv->lecid); 

#ifdef CONFIG_TR
        /* Ugly. Use this to realign Token Ring packets for
         * e.g. PCA-200E driver. */
        if (priv->is_trdev) {
                skb2 = skb_realloc_headroom(skb, LEC_HEADER_LEN);
                kfree_skb(skb);
                if (skb2 == NULL) return 0;
                skb = skb2;
        }
#endif

#if DUMP_PACKETS > 0
        printk("%s: send datalen:%ld lecid:%4.4x\n", dev->name,
               skb->len, priv->lecid);
#if DUMP_PACKETS >= 2
        for(i=0;i<skb->len && i <99;i++) {
                sprintf(buf+i*3,"%2.2x ",0xff&skb->data[i]);
        }
#elif DUMP_PACKETS >= 1
        for(i=0;i<skb->len && i < 30;i++) {
                sprintf(buf+i*3,"%2.2x ", 0xff&skb->data[i]);
        }
#endif /* DUMP_PACKETS >= 1 */
        if (i==skb->len)
                printk("%s\n",buf);
        else
                printk("%s...\n",buf);
#endif /* DUMP_PACKETS > 0 */

        /* Minimum ethernet-frame size */
        if (skb->len <62) {
                if (skb->truesize < 62) {
                        printk("%s:data packet %d / %d\n",
                               dev->name,
                               skb->len,skb->truesize);
                        nb=(unsigned char*)kmalloc(64, GFP_ATOMIC);
                        memcpy(nb,skb->data,skb->len);
                        kfree(skb->head);
                        skb->head = skb->data = nb;
                        skb->tail = nb+62;
                        skb->end = nb+64;
                        skb->len=62;
                        skb->truesize = 64;
                } else {
                        skb->len = 62;
                }
        }
        
        /* Send to right vcc */
        is_rdesc = 0;
        dst = lec_h->h_dest;
#ifdef CONFIG_TR
        if (priv->is_trdev) {
                dst = get_tr_dst(skb->data+2, rdesc);
                if (dst == NULL) {
                        dst = rdesc;
                        is_rdesc = 1;
                }
        }
#endif
        entry = NULL;
        send_vcc = lec_arp_resolve(priv, dst, is_rdesc, &entry);
        DPRINTK("%s:send_vcc:%p vcc_flags:%x, entry:%p\n", dev->name,
                send_vcc, send_vcc?send_vcc->flags:0, entry);
        if (!send_vcc || !test_bit(ATM_VF_READY,&send_vcc->flags)) {    
                if (entry && (entry->tx_wait.qlen < LEC_UNRES_QUE_LEN)) {
                        DPRINTK("%s:lec_send_packet: queuing packet, ", dev->name);
                        DPRINTK("MAC address 0x%02x:%02x:%02x:%02x:%02x:%02x\n",
                                lec_h->h_dest[0], lec_h->h_dest[1], lec_h->h_dest[2],
                                lec_h->h_dest[3], lec_h->h_dest[4], lec_h->h_dest[5]);
                        skb_queue_tail(&entry->tx_wait, skb);
                } else {
                        DPRINTK("%s:lec_send_packet: tx queue full or no arp entry, dropping, ", dev->name);
                        DPRINTK("MAC address 0x%02x:%02x:%02x:%02x:%02x:%02x\n",
                                lec_h->h_dest[0], lec_h->h_dest[1], lec_h->h_dest[2],
                                lec_h->h_dest[3], lec_h->h_dest[4], lec_h->h_dest[5]);
                        priv->stats.tx_dropped++;
                        dev_kfree_skb(skb);
                }
                return 0;
        }
                
#if DUMP_PACKETS > 0                    
        printk("%s:sending to vpi:%d vci:%d\n", dev->name,
               send_vcc->vpi, send_vcc->vci);       
#endif /* DUMP_PACKETS > 0 */
                
        while (entry && (skb2 = skb_dequeue(&entry->tx_wait))) {
                DPRINTK("lec.c: emptying tx queue, ");
                DPRINTK("MAC address 0x%02x:%02x:%02x:%02x:%02x:%02x\n",
                        lec_h->h_dest[0], lec_h->h_dest[1], lec_h->h_dest[2],
                        lec_h->h_dest[3], lec_h->h_dest[4], lec_h->h_dest[5]);
                ATM_SKB(skb2)->vcc = send_vcc;
                ATM_SKB(skb2)->iovcnt = 0;
                ATM_SKB(skb2)->atm_options = send_vcc->atm_options;
                DPRINTK("%s:sending to vpi:%d vci:%d\n", dev->name,
                        send_vcc->vpi, send_vcc->vci);       
                if (atm_may_send(send_vcc, skb2->len)) {
                        atomic_add(skb2->truesize, &send_vcc->tx_inuse);
                        priv->stats.tx_packets++;
                        priv->stats.tx_bytes += skb2->len;
                        send_vcc->send(send_vcc, skb2);
                } else {
                        priv->stats.tx_dropped++;
                        dev_kfree_skb(skb2);
		}
        }

        ATM_SKB(skb)->vcc = send_vcc;
        ATM_SKB(skb)->iovcnt = 0;
        ATM_SKB(skb)->atm_options = send_vcc->atm_options;
        if (atm_may_send(send_vcc, skb->len)) {
                atomic_add(skb->truesize, &send_vcc->tx_inuse);
                priv->stats.tx_packets++;
                priv->stats.tx_bytes += skb->len;
                send_vcc->send(send_vcc, skb);
        } else {
                priv->stats.tx_dropped++;
                dev_kfree_skb(skb);
	}

#if 0
        /* Should we wait for card's device driver to notify us? */
        dev->tbusy=0;
#endif        
        return 0;
}

/* The inverse routine to net_open(). */
static int 
lec_close(struct net_device *dev) 
{
        netif_stop_queue(dev);
        return 0;
}

/*
 * Get the current statistics.
 * This may be called with the card open or closed.
 */
static struct net_device_stats *
lec_get_stats(struct net_device *dev)
{
        return &((struct lec_priv *)dev->priv)->stats;
}

static int 
lec_atm_send(struct atm_vcc *vcc, struct sk_buff *skb)
{
        struct net_device *dev = (struct net_device*)vcc->proto_data;
        struct lec_priv *priv = (struct lec_priv*)dev->priv;
        struct atmlec_msg *mesg;
        struct lec_arp_table *entry;
        int i;
        char *tmp; /* FIXME */

	atomic_sub(skb->truesize+ATM_PDU_OVHD, &vcc->tx_inuse);
        mesg = (struct atmlec_msg *)skb->data;
        tmp = skb->data;
        tmp += sizeof(struct atmlec_msg);
        DPRINTK("%s: msg from zeppelin:%d\n", dev->name, mesg->type);
        switch(mesg->type) {
        case l_set_mac_addr:
                for (i=0;i<6;i++) {
                        dev->dev_addr[i] = mesg->content.normal.mac_addr[i];
                }    
                break;
        case l_del_mac_addr:
                for(i=0;i<6;i++) {
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
        case l_narp_req: /* LANE2: see 7.1.35 in the lane2 spec */
                entry = lec_arp_find(priv, mesg->content.normal.mac_addr);
                lec_arp_remove(priv->lec_arp_tables, entry);

                if (mesg->content.normal.no_source_le_narp)
                        break;
                /* FALL THROUGH */
        case l_arp_update:
                lec_arp_update(priv, mesg->content.normal.mac_addr,
                               mesg->content.normal.atm_addr,
                               mesg->content.normal.flag,
                               mesg->content.normal.targetless_le_arp);
                DPRINTK("lec: in l_arp_update\n");
                if (mesg->sizeoftlvs != 0) { /* LANE2 3.1.5 */
                        DPRINTK("lec: LANE2 3.1.5, got tlvs, size %d\n", mesg->sizeoftlvs);
                        lane2_associate_ind(dev,
                                            mesg->content.normal.mac_addr,
                                            tmp, mesg->sizeoftlvs);
                }
                break;
        case l_config:
                priv->maximum_unknown_frame_count = 
                        mesg->content.config.maximum_unknown_frame_count;
                priv->max_unknown_frame_time = 
                        (mesg->content.config.max_unknown_frame_time*HZ);
                priv->max_retry_count = 
                        mesg->content.config.max_retry_count;
                priv->aging_time = (mesg->content.config.aging_time*HZ);
                priv->forward_delay_time = 
                        (mesg->content.config.forward_delay_time*HZ);
                priv->arp_response_time = 
                        (mesg->content.config.arp_response_time*HZ);
                priv->flush_timeout = (mesg->content.config.flush_timeout*HZ);
                priv->path_switching_delay = 
                        (mesg->content.config.path_switching_delay*HZ);
                priv->lane_version = mesg->content.config.lane_version; /* LANE2 */
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
                priv->lecid=(unsigned short)(0xffff&mesg->content.normal.flag);
                break;
        case l_should_bridge: {
#if defined(CONFIG_BRIDGE) || defined(CONFIG_BRIDGE_MODULE)
                struct net_bridge_fdb_entry *f;

                DPRINTK("%s: bridge zeppelin asks about 0x%02x:%02x:%02x:%02x:%02x:%02x\n",
                        dev->name,
                        mesg->content.proxy.mac_addr[0], mesg->content.proxy.mac_addr[1],
                        mesg->content.proxy.mac_addr[2], mesg->content.proxy.mac_addr[3],
                        mesg->content.proxy.mac_addr[4], mesg->content.proxy.mac_addr[5]);

                if (br_fdb_get_hook == NULL || dev->br_port == NULL)
                        break;

                f = br_fdb_get_hook(dev->br_port->br, mesg->content.proxy.mac_addr);
                if (f != NULL &&
                    f->dst->dev != dev &&
                    f->dst->state == BR_STATE_FORWARDING) {
                                /* hit from bridge table, send LE_ARP_RESPONSE */
                        struct sk_buff *skb2;

                        DPRINTK("%s: entry found, responding to zeppelin\n", dev->name);
                        skb2 = alloc_skb(sizeof(struct atmlec_msg), GFP_ATOMIC);
                        if (skb2 == NULL) {
                                br_fdb_put_hook(f);
                                break;
                        }
                        skb2->len = sizeof(struct atmlec_msg);
                        memcpy(skb2->data, mesg, sizeof(struct atmlec_msg));
                        atm_force_charge(priv->lecd, skb2->truesize);
                        skb_queue_tail(&priv->lecd->recvq, skb2);
                        wake_up(&priv->lecd->sleep);
                }
                if (f != NULL) br_fdb_put_hook(f);
#endif /* defined(CONFIG_BRIDGE) || defined(CONFIG_BRIDGE_MODULE) */
                }
                break;
        default:
                printk("%s: Unknown message type %d\n", dev->name, mesg->type);
                dev_kfree_skb(skb);
                return -EINVAL;
        }
        dev_kfree_skb(skb);
        return 0;
}

static void 
lec_atm_close(struct atm_vcc *vcc)
{
        struct sk_buff *skb;
        struct net_device *dev = (struct net_device *)vcc->proto_data;
        struct lec_priv *priv = (struct lec_priv *)dev->priv;

        priv->lecd = NULL;
        /* Do something needful? */

        netif_stop_queue(dev);
        lec_arp_destroy(priv);

        if (skb_peek(&vcc->recvq))
		printk("%s lec_atm_close: closing with messages pending\n",
                       dev->name);
        while ((skb = skb_dequeue(&vcc->recvq))) {
                atm_return(vcc, skb->truesize);
		dev_kfree_skb(skb);
        }
  
	printk("%s: Shut down!\n", dev->name);
        MOD_DEC_USE_COUNT;
}

static struct atmdev_ops lecdev_ops = {
        close:	lec_atm_close,
        send:	lec_atm_send
};

static struct atm_dev lecatm_dev = {
        &lecdev_ops,
        NULL,	    /*PHY*/
        "lec",	    /*type*/
        999,	    /*dummy device number*/
        NULL,NULL,  /*no VCCs*/
        NULL,NULL,  /*no data*/
        { 0 },	    /*no flags*/
        NULL,	    /* no local address*/
        { 0 }	    /*no ESI or rest of the atm_dev struct things*/
};

/*
 * LANE2: new argument struct sk_buff *data contains
 * the LE_ARP based TLVs introduced in the LANE2 spec
 */
int 
send_to_lecd(struct lec_priv *priv, atmlec_msg_type type, 
             unsigned char *mac_addr, unsigned char *atm_addr,
             struct sk_buff *data)
{
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
	skb_queue_tail(&priv->lecd->recvq, skb);
        wake_up(&priv->lecd->sleep);

        if (data != NULL) {
                DPRINTK("lec: about to send %d bytes of data\n", data->len);
                atm_force_charge(priv->lecd, data->truesize);
                skb_queue_tail(&priv->lecd->recvq, data);
                wake_up(&priv->lecd->sleep);
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

static void 
lec_init(struct net_device *dev)
{
        dev->change_mtu = lec_change_mtu;
        dev->open = lec_open;
        dev->stop = lec_close;
        dev->hard_start_xmit = lec_send_packet;

        dev->get_stats = lec_get_stats;
        dev->set_multicast_list = NULL;
        dev->do_ioctl  = NULL;
        printk("%s: Initialized!\n",dev->name);
        return;
}

static unsigned char lec_ctrl_magic[] = {
        0xff,
        0x00,
        0x01,
        0x01 };

void 
lec_push(struct atm_vcc *vcc, struct sk_buff *skb)
{
        struct net_device *dev = (struct net_device *)vcc->proto_data;
        struct lec_priv *priv = (struct lec_priv *)dev->priv; 

#if DUMP_PACKETS >0
        int i=0;
        char buf[300];

        printk("%s: lec_push vcc vpi:%d vci:%d\n", dev->name,
               vcc->vpi, vcc->vci);
#endif
        if (!skb) {
                DPRINTK("%s: null skb\n",dev->name);
                lec_vcc_close(priv, vcc);
                return;
        }
#if DUMP_PACKETS > 0
        printk("%s: rcv datalen:%ld lecid:%4.4x\n", dev->name,
               skb->len, priv->lecid);
#if DUMP_PACKETS >= 2
        for(i=0;i<skb->len && i <99;i++) {
                sprintf(buf+i*3,"%2.2x ",0xff&skb->data[i]);
        }
#elif DUMP_PACKETS >= 1
        for(i=0;i<skb->len && i < 30;i++) {
                sprintf(buf+i*3,"%2.2x ", 0xff&skb->data[i]);
        }
#endif /* DUMP_PACKETS >= 1 */
        if (i==skb->len)
                printk("%s\n",buf);
        else
                printk("%s...\n",buf);
#endif /* DUMP_PACKETS > 0 */
        if (memcmp(skb->data, lec_ctrl_magic, 4) ==0) { /* Control frame, to daemon*/
                DPRINTK("%s: To daemon\n",dev->name);
                skb_queue_tail(&vcc->recvq, skb);
                wake_up(&vcc->sleep);
        } else { /* Data frame, queue to protocol handlers */
                unsigned char *dst;

                atm_return(vcc,skb->truesize);
                if (*(uint16_t *)skb->data == htons(priv->lecid) ||
                    !priv->lecd) { 
                        /* Probably looping back, or if lecd is missing,
                           lecd has gone down */
                        DPRINTK("Ignoring loopback frame...\n");
                        dev_kfree_skb(skb);
                        return;
                }
#ifdef CONFIG_TR
                if (priv->is_trdev) dst = ((struct lecdatahdr_8025 *)skb->data)->h_dest;
                else
#endif
                dst = ((struct lecdatahdr_8023 *)skb->data)->h_dest;

                if (!(dst[0]&0x01) &&   /* Never filter Multi/Broadcast */
                    !priv->is_proxy &&  /* Proxy wants all the packets */
		    memcmp(dst, dev->dev_addr, dev->addr_len)) {
                        dev_kfree_skb(skb);
                        return;
                }
                if (priv->lec_arp_empty_ones) {
                        lec_arp_check_empties(priv, vcc, skb);
                }
                skb->dev = dev;
                skb->data += 2; /* skip lec_id */
#ifdef CONFIG_TR
                if (priv->is_trdev) skb->protocol = tr_type_trans(skb, dev);
                else
#endif
                skb->protocol = eth_type_trans(skb, dev);
                priv->stats.rx_packets++;
                priv->stats.rx_bytes += skb->len;
                netif_rx(skb);
        }
}

int 
lec_vcc_attach(struct atm_vcc *vcc, void *arg)
{
        int bytes_left;
        struct atmlec_ioc ioc_data;

        /* Lecd must be up in this case */
        bytes_left = copy_from_user(&ioc_data, arg, sizeof(struct atmlec_ioc));
        if (bytes_left != 0) {
                printk("lec: lec_vcc_attach, copy from user failed for %d bytes\n",
                       bytes_left);
        }
        if (ioc_data.dev_num < 0 || ioc_data.dev_num >= MAX_LEC_ITF || 
            !dev_lec[ioc_data.dev_num])
                return -EINVAL;
        lec_vcc_added(dev_lec[ioc_data.dev_num]->priv, 
                      &ioc_data, vcc, vcc->push);
        vcc->push = lec_push;
        vcc->proto_data = dev_lec[ioc_data.dev_num];
        return 0;
}

int 
lec_mcast_attach(struct atm_vcc *vcc, int arg)
{
        if (arg <0 || arg >= MAX_LEC_ITF || !dev_lec[arg])
                return -EINVAL;
        vcc->proto_data = dev_lec[arg];
        return (lec_mcast_make((struct lec_priv*)dev_lec[arg]->priv, vcc));
}

/* Initialize device. */
int 
lecd_attach(struct atm_vcc *vcc, int arg)
{  
        int i;
        struct lec_priv *priv;

        if (arg<0)
                i = 0;
        else
                i = arg;
#ifdef CONFIG_TR
        if (arg >= MAX_LEC_ITF)
                return -EINVAL;
#else /* Reserve the top NUM_TR_DEVS for TR */
        if (arg >= (MAX_LEC_ITF-NUM_TR_DEVS))
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
                        dev_lec[i] = init_trdev(NULL, size);
                else
#endif
                dev_lec[i] = init_etherdev(NULL, size);
                if (!dev_lec[i])
                        return -ENOMEM;

                priv = dev_lec[i]->priv;
                priv->is_trdev = is_trdev;
                sprintf(dev_lec[i]->name, "lec%d", i);
                lec_init(dev_lec[i]);
        } else {
                priv = dev_lec[i]->priv;
                if (priv->lecd)
                        return -EADDRINUSE;
        }
        lec_arp_init(priv);
	priv->itfnum = i;  /* LANE2 addition */
        priv->lecd = vcc;
        bind_vcc(vcc, &lecatm_dev);
        
        vcc->proto_data = dev_lec[i];
	set_bit(ATM_VF_META,&vcc->flags);
	set_bit(ATM_VF_READY,&vcc->flags);

        /* Set default values to these variables */
        priv->maximum_unknown_frame_count = 1;
        priv->max_unknown_frame_time = (1*HZ);
        priv->vcc_timeout_period = (1200*HZ);
        priv->max_retry_count = 1;
        priv->aging_time = (300*HZ);
        priv->forward_delay_time = (15*HZ);
        priv->topology_change = 0;
        priv->arp_response_time = (1*HZ);
        priv->flush_timeout = (4*HZ);
        priv->path_switching_delay = (6*HZ);

        if (dev_lec[i]->flags & IFF_UP) {
                netif_start_queue(dev_lec[i]);
        }
        MOD_INC_USE_COUNT;
        return i;
}

void atm_lane_init_ops(struct atm_lane_ops *ops)
{
        ops->lecd_attach = lecd_attach;
        ops->mcast_attach = lec_mcast_attach;
        ops->vcc_attach = lec_vcc_attach;
        ops->get_lecs = get_dev_lec;

        printk("lec.c: " __DATE__ " " __TIME__ " initialized\n");

	return;
}

static int __init lane_module_init(void)
{
        extern struct atm_lane_ops atm_lane_ops;

        atm_lane_init_ops(&atm_lane_ops);

        return 0;
}

static void __exit lane_module_cleanup(void)
{
        int i;
        extern struct atm_lane_ops atm_lane_ops;
        struct lec_priv *priv;

        atm_lane_ops.lecd_attach = NULL;
        atm_lane_ops.mcast_attach = NULL;
        atm_lane_ops.vcc_attach = NULL;
        atm_lane_ops.get_lecs = NULL;

        for (i = 0; i < MAX_LEC_ITF; i++) {
                if (dev_lec[i] != NULL) {
                        priv = (struct lec_priv *)dev_lec[i]->priv;
#if defined(CONFIG_TR)
                        unregister_trdev(dev_lec[i]);
#endif
                        kfree(dev_lec[i]);
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
        struct lec_priv *priv = (struct lec_priv *)dev->priv;
        struct lec_arp_table *table;
        struct sk_buff *skb;
        int retval;

        if (force == 0) {
                table = lec_arp_find(priv, dst_mac);
                if(table == NULL)
                        return -1;
                
                *tlvs = kmalloc(table->sizeoftlvs, GFP_KERNEL);
                if (*tlvs == NULL)
                        return -1;
                
                memcpy(*tlvs, table->tlvs, table->sizeoftlvs);
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
		memcpy(skb->data, *tlvs, *sizeoftlvs);
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
static int lane2_associate_req (struct net_device *dev, u8 *lan_dst,
                         u8 *tlvs, u32 sizeoftlvs)
{
        int retval;
        struct sk_buff *skb;
        struct lec_priv *priv = (struct lec_priv*)dev->priv;

        if ( memcmp(lan_dst, dev->dev_addr, ETH_ALEN) != 0 )
                return (0);       /* not our mac address */

        kfree(priv->tlvs); /* NULL if there was no previous association */

        priv->tlvs = kmalloc(sizeoftlvs, GFP_KERNEL);
        if (priv->tlvs == NULL)
                return (0);
        priv->sizeoftlvs = sizeoftlvs;
        memcpy(priv->tlvs, tlvs, sizeoftlvs);

        skb = alloc_skb(sizeoftlvs, GFP_ATOMIC);
        if (skb == NULL)
                return 0;
        skb->len = sizeoftlvs;
        memcpy(skb->data, tlvs, sizeoftlvs);
        retval = send_to_lecd(priv, l_associate_req, NULL, NULL, skb);
        if (retval != 0)
                printk("lec.c: lane2_associate_req() failed\n");
        /* If the previous association has changed we must
         * somehow notify other LANE entities about the change
         */
        return (1);
}

/*
 * LANE2: 3.1.5, LE_ASSOCIATE.indication
 *
 */
static void lane2_associate_ind (struct net_device *dev, u8 *mac_addr,
    u8 *tlvs, u32 sizeoftlvs)
{
#if 0
        int i = 0;
#endif
	struct lec_priv *priv = (struct lec_priv *)dev->priv;
#if 0 /* Why have the TLVs in LE_ARP entries since we do not use them? When you
         uncomment this code, make sure the TLVs get freed when entry is killed */
        struct lec_arp_table *entry = lec_arp_find(priv, mac_addr);

        if (entry == NULL)
                return;     /* should not happen */

        kfree(entry->tlvs);

        entry->tlvs = kmalloc(sizeoftlvs, GFP_KERNEL);
        if (entry->tlvs == NULL)
                return;

        entry->sizeoftlvs = sizeoftlvs;
        memcpy(entry->tlvs, tlvs, sizeoftlvs);
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
 *
 */

#include <linux/types.h>
#include <linux/sched.h>
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

static void lec_arp_check_expire(unsigned long data);
static __inline__ void lec_arp_expire_arp(unsigned long data);
void dump_arp_table(struct lec_priv *priv);

/* 
 * Arp table funcs
 */

#define HASH(ch) (ch & (LEC_ARP_TABLE_SIZE -1))

static __inline__ void 
lec_arp_lock(struct lec_priv *priv)
{
        atomic_inc(&priv->lec_arp_lock_var);
}

static __inline__ void 
lec_arp_unlock(struct lec_priv *priv)
{
        atomic_dec(&priv->lec_arp_lock_var);
}

/*
 * Initialization of arp-cache
 */
void 
lec_arp_init(struct lec_priv *priv)
{
        unsigned short i;

        for (i=0;i<LEC_ARP_TABLE_SIZE;i++) {
                priv->lec_arp_tables[i] = NULL;
        }        
        init_timer(&priv->lec_arp_timer);
        priv->lec_arp_timer.expires = jiffies+LEC_ARP_REFRESH_INTERVAL;
        priv->lec_arp_timer.data = (unsigned long)priv;
        priv->lec_arp_timer.function = lec_arp_check_expire;
        add_timer(&priv->lec_arp_timer);
}

void
lec_arp_clear_vccs(struct lec_arp_table *entry)
{
        if (entry->vcc) {
                entry->vcc->push = entry->old_push;
#if 0 /* August 6, 1998 */
                set_bit(ATM_VF_RELEASED,&entry->vcc->flags);
		clear_bit(ATM_VF_READY,&entry->vcc->flags);
                entry->vcc->push(entry->vcc, NULL);
#endif
		atm_async_release_vcc(entry->vcc, -EPIPE);
                entry->vcc = NULL;
        }
        if (entry->recv_vcc) {
                entry->recv_vcc->push = entry->old_recv_push;
#if 0
                set_bit(ATM_VF_RELEASED,&entry->recv_vcc->flags);
		clear_bit(ATM_VF_READY,&entry->recv_vcc->flags);
                entry->recv_vcc->push(entry->recv_vcc, NULL);
#endif
		atm_async_release_vcc(entry->recv_vcc, -EPIPE);
                entry->recv_vcc = NULL;
        }        
}

/*
 * Insert entry to lec_arp_table
 * LANE2: Add to the end of the list to satisfy 8.1.13
 */
static __inline__ void 
lec_arp_put(struct lec_arp_table **lec_arp_tables, 
            struct lec_arp_table *to_put)
{
        unsigned short place;
        unsigned long flags;
        struct lec_arp_table *tmp;

        save_flags(flags);
        cli();

        place = HASH(to_put->mac_addr[ETH_ALEN-1]);
        tmp = lec_arp_tables[place];
        to_put->next = NULL;
        if (tmp == NULL)
                lec_arp_tables[place] = to_put;
  
        else {  /* add to the end */
                while (tmp->next)
                        tmp = tmp->next;
                tmp->next = to_put;
        }

        restore_flags(flags);
        DPRINTK("LEC_ARP: Added entry:%2.2x %2.2x %2.2x %2.2x %2.2x %2.2x\n",
                0xff&to_put->mac_addr[0], 0xff&to_put->mac_addr[1],
                0xff&to_put->mac_addr[2], 0xff&to_put->mac_addr[3],
                0xff&to_put->mac_addr[4], 0xff&to_put->mac_addr[5]);
}

/*
 * Remove entry from lec_arp_table
 */
static __inline__ int 
lec_arp_remove(struct lec_arp_table **lec_arp_tables,
               struct lec_arp_table *to_remove)
{
        unsigned short place;
        struct lec_arp_table *tmp;
        unsigned long flags;
        int remove_vcc=1;

        save_flags(flags);
        cli();

        if (!to_remove) {
                restore_flags(flags);
                return -1;
        }
        place = HASH(to_remove->mac_addr[ETH_ALEN-1]);
        tmp = lec_arp_tables[place];
        if (tmp == to_remove) {
                lec_arp_tables[place] = tmp->next;
        } else {
                while(tmp && tmp->next != to_remove) {
                        tmp = tmp->next;
                }
                if (!tmp) {/* Entry was not found */
                        restore_flags(flags);
                        return -1;
                }
        }
        tmp->next = to_remove->next;
        del_timer(&to_remove->timer);
  
        /* If this is the only MAC connected to this VCC, also tear down
           the VCC */
        if (to_remove->status >= ESI_FLUSH_PENDING) {
                /*
                 * ESI_FLUSH_PENDING, ESI_FORWARD_DIRECT
                 */
                for(place=0;place<LEC_ARP_TABLE_SIZE;place++) {
                        for(tmp=lec_arp_tables[place];tmp!=NULL;tmp=tmp->next){
                                if (memcmp(tmp->atm_addr, to_remove->atm_addr,
                                           ATM_ESA_LEN)==0) {
                                        remove_vcc=0;
                                        break;
                                }
                        }
                }
                if (remove_vcc)
                        lec_arp_clear_vccs(to_remove);
        }
        skb_queue_purge(&to_remove->tx_wait); /* FIXME: good place for this? */
        restore_flags(flags);
        DPRINTK("LEC_ARP: Removed entry:%2.2x %2.2x %2.2x %2.2x %2.2x %2.2x\n",
                0xff&to_remove->mac_addr[0], 0xff&to_remove->mac_addr[1],
                0xff&to_remove->mac_addr[2], 0xff&to_remove->mac_addr[3],
                0xff&to_remove->mac_addr[4], 0xff&to_remove->mac_addr[5]);
        return 0;
}

#if DEBUG_ARP_TABLE
static char*
get_status_string(unsigned char st)
{
        switch(st) {
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
#endif

void
dump_arp_table(struct lec_priv *priv)
{
#if DEBUG_ARP_TABLE
        int i,j, offset;
        struct lec_arp_table *rulla;
        char buf[1024];
        struct lec_arp_table **lec_arp_tables =
                (struct lec_arp_table **)priv->lec_arp_tables;
        struct lec_arp_table *lec_arp_empty_ones =
                (struct lec_arp_table *)priv->lec_arp_empty_ones;
        struct lec_arp_table *lec_no_forward =
                (struct lec_arp_table *)priv->lec_no_forward;
        struct lec_arp_table *mcast_fwds = priv->mcast_fwds;


        printk("Dump %p:\n",priv);
        for (i=0;i<LEC_ARP_TABLE_SIZE;i++) {
                rulla = lec_arp_tables[i];
                offset = 0;
                offset += sprintf(buf,"%d: %p\n",i, rulla);
                while (rulla) {
                        offset += sprintf(buf+offset,"Mac:");
                        for(j=0;j<ETH_ALEN;j++) {
                                offset+=sprintf(buf+offset,
                                                "%2.2x ",
                                                rulla->mac_addr[j]&0xff);
                        }
                        offset +=sprintf(buf+offset,"Atm:");
                        for(j=0;j<ATM_ESA_LEN;j++) {
                                offset+=sprintf(buf+offset,
                                                "%2.2x ",
                                                rulla->atm_addr[j]&0xff);
                        }      
                        offset+=sprintf(buf+offset,
                                        "Vcc vpi:%d vci:%d, Recv_vcc vpi:%d vci:%d Last_used:%lx, Timestamp:%lx, No_tries:%d ",
                                        rulla->vcc?rulla->vcc->vpi:0, 
                                        rulla->vcc?rulla->vcc->vci:0,
                                        rulla->recv_vcc?rulla->recv_vcc->vpi:0,
                                        rulla->recv_vcc?rulla->recv_vcc->vci:0,
                                        rulla->last_used,
                                        rulla->timestamp, rulla->no_tries);
                        offset+=sprintf(buf+offset,
                                        "Flags:%x, Packets_flooded:%x, Status: %s ",
                                        rulla->flags, rulla->packets_flooded, 
                                        get_status_string(rulla->status));
                        offset+=sprintf(buf+offset,"->%p\n",rulla->next);
                        rulla = rulla->next;
                }
                printk("%s",buf);
        }
        rulla = lec_no_forward;
        if (rulla)
                printk("No forward\n");  
        while(rulla) {
                offset=0;
                offset += sprintf(buf+offset,"Mac:");
                for(j=0;j<ETH_ALEN;j++) {
                        offset+=sprintf(buf+offset,"%2.2x ",
                                        rulla->mac_addr[j]&0xff);
                }
                offset +=sprintf(buf+offset,"Atm:");
                for(j=0;j<ATM_ESA_LEN;j++) {
                        offset+=sprintf(buf+offset,"%2.2x ",
                                        rulla->atm_addr[j]&0xff);
                }      
                offset+=sprintf(buf+offset,
                                "Vcc vpi:%d vci:%d, Recv_vcc vpi:%d vci:%d Last_used:%lx, Timestamp:%lx, No_tries:%d ",
                                rulla->vcc?rulla->vcc->vpi:0, 
                                rulla->vcc?rulla->vcc->vci:0, 
                                rulla->recv_vcc?rulla->recv_vcc->vpi:0,
                                rulla->recv_vcc?rulla->recv_vcc->vci:0,
                                rulla->last_used, 
                                rulla->timestamp, rulla->no_tries);
                offset+=sprintf(buf+offset,
                                "Flags:%x, Packets_flooded:%x, Status: %s ",
                                rulla->flags, rulla->packets_flooded, 
                                get_status_string(rulla->status));
                offset+=sprintf(buf+offset,"->%lx\n",(long)rulla->next);
                rulla = rulla->next;
                printk("%s",buf);
        }
        rulla = lec_arp_empty_ones;
        if (rulla)
                printk("Empty ones\n");  
        while(rulla) {
                offset=0;
                offset += sprintf(buf+offset,"Mac:");
                for(j=0;j<ETH_ALEN;j++) {
                        offset+=sprintf(buf+offset,"%2.2x ",
                                        rulla->mac_addr[j]&0xff);
                }
                offset +=sprintf(buf+offset,"Atm:");
                for(j=0;j<ATM_ESA_LEN;j++) {
                        offset+=sprintf(buf+offset,"%2.2x ",
                                        rulla->atm_addr[j]&0xff);
                }      
                offset+=sprintf(buf+offset,
                                "Vcc vpi:%d vci:%d, Recv_vcc vpi:%d vci:%d Last_used:%lx, Timestamp:%lx, No_tries:%d ",
                                rulla->vcc?rulla->vcc->vpi:0, 
                                rulla->vcc?rulla->vcc->vci:0, 
                                rulla->recv_vcc?rulla->recv_vcc->vpi:0,
                                rulla->recv_vcc?rulla->recv_vcc->vci:0,
                                rulla->last_used, 
                                rulla->timestamp, rulla->no_tries);
                offset+=sprintf(buf+offset,
                                "Flags:%x, Packets_flooded:%x, Status: %s ",
                                rulla->flags, rulla->packets_flooded, 
                                get_status_string(rulla->status));
                offset+=sprintf(buf+offset,"->%lx\n",(long)rulla->next);
                rulla = rulla->next;
                printk("%s",buf);
        }

        rulla = mcast_fwds;
        if (rulla)
                printk("Multicast Forward VCCs\n");  
        while(rulla) {
                offset=0;
                offset += sprintf(buf+offset,"Mac:");
                for(j=0;j<ETH_ALEN;j++) {
                        offset+=sprintf(buf+offset,"%2.2x ",
                                        rulla->mac_addr[j]&0xff);
                }
                offset +=sprintf(buf+offset,"Atm:");
                for(j=0;j<ATM_ESA_LEN;j++) {
                        offset+=sprintf(buf+offset,"%2.2x ",
                                        rulla->atm_addr[j]&0xff);
                }      
                offset+=sprintf(buf+offset,
                                "Vcc vpi:%d vci:%d, Recv_vcc vpi:%d vci:%d Last_used:%lx, Timestamp:%lx, No_tries:%d ",
                                rulla->vcc?rulla->vcc->vpi:0, 
                                rulla->vcc?rulla->vcc->vci:0, 
                                rulla->recv_vcc?rulla->recv_vcc->vpi:0,
                                rulla->recv_vcc?rulla->recv_vcc->vci:0,
                                rulla->last_used, 
                                rulla->timestamp, rulla->no_tries);
                offset+=sprintf(buf+offset,
                                "Flags:%x, Packets_flooded:%x, Status: %s ",
                                rulla->flags, rulla->packets_flooded, 
                                get_status_string(rulla->status));
                offset+=sprintf(buf+offset,"->%lx\n",(long)rulla->next);
                rulla = rulla->next;
                printk("%s",buf);
        }

#endif
}

/*
 * Destruction of arp-cache
 */
void
lec_arp_destroy(struct lec_priv *priv)
{
        struct lec_arp_table *entry, *next;
        unsigned long flags;
        int i;

        save_flags(flags);
        cli();

        del_timer(&priv->lec_arp_timer);
        
        /*
         * Remove all entries
         */
        for (i=0;i<LEC_ARP_TABLE_SIZE;i++) {
                for(entry =priv->lec_arp_tables[i];entry != NULL; entry=next) {
                        next = entry->next;
                        lec_arp_remove(priv->lec_arp_tables, entry);
                        kfree(entry);
                }
        }
        entry = priv->lec_arp_empty_ones;
        while(entry) {
                next = entry->next;
                del_timer(&entry->timer);
                lec_arp_clear_vccs(entry);
                kfree(entry);
                entry = next;
        }
        priv->lec_arp_empty_ones = NULL;
        entry = priv->lec_no_forward;
        while(entry) {
                next = entry->next;
                del_timer(&entry->timer);
                lec_arp_clear_vccs(entry);
                kfree(entry);
                entry = next;
        }
        priv->lec_no_forward = NULL;
        entry = priv->mcast_fwds;
        while(entry) {
                next = entry->next;
                del_timer(&entry->timer);
                lec_arp_clear_vccs(entry);
                kfree(entry);
                entry = next;
        }
        priv->mcast_fwds = NULL;
        priv->mcast_vcc = NULL;
        memset(priv->lec_arp_tables, 0, 
               sizeof(struct lec_arp_table*)*LEC_ARP_TABLE_SIZE);
        restore_flags(flags);
}


/* 
 * Find entry by mac_address
 */
static __inline__ struct lec_arp_table*
lec_arp_find(struct lec_priv *priv,
             unsigned char *mac_addr)
{
        unsigned short place;
        struct lec_arp_table *to_return;

        DPRINTK("LEC_ARP: lec_arp_find :%2.2x %2.2x %2.2x %2.2x %2.2x %2.2x\n",
                mac_addr[0]&0xff, mac_addr[1]&0xff, mac_addr[2]&0xff, 
                mac_addr[3]&0xff, mac_addr[4]&0xff, mac_addr[5]&0xff);
        lec_arp_lock(priv);
        place = HASH(mac_addr[ETH_ALEN-1]);
  
        to_return = priv->lec_arp_tables[place];
        while(to_return) {
                if (memcmp(mac_addr, to_return->mac_addr, ETH_ALEN) == 0) {
                        lec_arp_unlock(priv);
                        return to_return;
                }
                to_return = to_return->next;
        }
        lec_arp_unlock(priv);
        return NULL;
}

static struct lec_arp_table*
make_entry(struct lec_priv *priv, unsigned char *mac_addr)
{
        struct lec_arp_table *to_return;

        to_return=(struct lec_arp_table *)kmalloc(sizeof(struct lec_arp_table),
                                                  GFP_ATOMIC);
        if (!to_return) {
                printk("LEC: Arp entry kmalloc failed\n");
                return NULL;
        }
        memset(to_return,0,sizeof(struct lec_arp_table));
        memcpy(to_return->mac_addr, mac_addr, ETH_ALEN);
        init_timer(&to_return->timer);
        to_return->timer.function = lec_arp_expire_arp;
        to_return->timer.data = (unsigned long)to_return;
        to_return->last_used = jiffies;
        to_return->priv = priv;
        skb_queue_head_init(&to_return->tx_wait);
        return to_return;
}

/*
 *
 * Arp sent timer expired
 *
 */
static void
lec_arp_expire_arp(unsigned long data)
{
        struct lec_arp_table *entry;

        entry = (struct lec_arp_table *)data;

        del_timer(&entry->timer);

        DPRINTK("lec_arp_expire_arp\n");
        if (entry->status == ESI_ARP_PENDING) {
                if (entry->no_tries <= entry->priv->max_retry_count) {
                        if (entry->is_rdesc)
                                send_to_lecd(entry->priv, l_rdesc_arp_xmt, entry->mac_addr, NULL, NULL);
                        else
                                send_to_lecd(entry->priv, l_arp_xmt, entry->mac_addr, NULL, NULL);
                        entry->no_tries++;
                }
                entry->timer.expires = jiffies + (1*HZ);
                add_timer(&entry->timer);
        }
}

/*
 *
 * Unknown/unused vcc expire, remove associated entry
 *
 */
static void
lec_arp_expire_vcc(unsigned long data)
{
        struct lec_arp_table *to_remove = (struct lec_arp_table*)data;
        struct lec_priv *priv = (struct lec_priv *)to_remove->priv;
        struct lec_arp_table *entry = NULL;

        del_timer(&to_remove->timer);

        DPRINTK("LEC_ARP %p %p: lec_arp_expire_vcc vpi:%d vci:%d\n",
                to_remove, priv, 
                to_remove->vcc?to_remove->recv_vcc->vpi:0,
                to_remove->vcc?to_remove->recv_vcc->vci:0);
        DPRINTK("eo:%p nf:%p\n",priv->lec_arp_empty_ones,priv->lec_no_forward);
        if (to_remove == priv->lec_arp_empty_ones)
                priv->lec_arp_empty_ones = to_remove->next;
        else {
                entry = priv->lec_arp_empty_ones;
                while (entry && entry->next != to_remove)
                        entry = entry->next;
                if (entry)
                        entry->next = to_remove->next;
        }
        if (!entry) {
                if (to_remove == priv->lec_no_forward) {
                        priv->lec_no_forward = to_remove->next;
                } else {
                        entry = priv->lec_no_forward;
                        while (entry && entry->next != to_remove)
                                entry = entry->next;
                        if (entry)
                                entry->next = to_remove->next;
                }
	}
        lec_arp_clear_vccs(to_remove);
        kfree(to_remove);
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
static void
lec_arp_check_expire(unsigned long data)
{
        struct lec_priv *priv = (struct lec_priv *)data;
        struct lec_arp_table **lec_arp_tables =
                (struct lec_arp_table **)priv->lec_arp_tables;
        struct lec_arp_table *entry, *next;
        unsigned long now;
        unsigned long time_to_check;
        int i;

        del_timer(&priv->lec_arp_timer);

        DPRINTK("lec_arp_check_expire %p,%d\n",priv,
                priv->lec_arp_lock_var.counter);
        DPRINTK("expire: eo:%p nf:%p\n",priv->lec_arp_empty_ones,
                priv->lec_no_forward);
        if (!priv->lec_arp_lock_var.counter) {
                lec_arp_lock(priv);
                now = jiffies;
                for(i=0;i<LEC_ARP_TABLE_SIZE;i++) {
                        for(entry = lec_arp_tables[i];entry != NULL;) {
                                if ((entry->flags) & LEC_REMOTE_FLAG && 
                                    priv->topology_change)
                                        time_to_check=priv->forward_delay_time;
                                else
                                        time_to_check = priv->aging_time;

                                DPRINTK("About to expire: %lx - %lx > %lx\n",
                                        now,entry->last_used, time_to_check);
                                if( time_after(now, entry->last_used+
                                   time_to_check) && 
                                    !(entry->flags & LEC_PERMANENT_FLAG) &&
                                    !(entry->mac_addr[0] & 0x01) ) { /* LANE2: 7.1.20 */
                                        /* Remove entry */
                                        DPRINTK("LEC:Entry timed out\n");
                                        next = entry->next;      
                                        lec_arp_remove(lec_arp_tables, entry);
                                        kfree(entry);
                                        entry = next;
                                } else {
                                        /* Something else */
                                        if ((entry->status == ESI_VC_PENDING ||
                                             entry->status == ESI_ARP_PENDING) 
                                            && time_after_eq(now,
                                            entry->timestamp +
                                            priv->max_unknown_frame_time)) {
                                                entry->timestamp = jiffies;
                                                entry->packets_flooded = 0;
                                                if (entry->status == ESI_VC_PENDING)
                                                        send_to_lecd(priv, l_svc_setup, entry->mac_addr, entry->atm_addr, NULL);
                                        }
                                        if (entry->status == ESI_FLUSH_PENDING 
                                           &&
                                           time_after_eq(now, entry->timestamp+
                                           priv->path_switching_delay)) {
                                                entry->last_used = jiffies;
                                                entry->status = 
                                                        ESI_FORWARD_DIRECT;
                                        }
                                        entry = entry->next;
                                }
                        }
                }
                lec_arp_unlock(priv);
        }
        priv->lec_arp_timer.expires = jiffies + LEC_ARP_REFRESH_INTERVAL;
        add_timer(&priv->lec_arp_timer);
}
/*
 * Try to find vcc where mac_address is attached.
 * 
 */
struct atm_vcc*
lec_arp_resolve(struct lec_priv *priv, unsigned char *mac_to_find, int is_rdesc,
                struct lec_arp_table **ret_entry)
{
        struct lec_arp_table *entry;

        if (mac_to_find[0]&0x01) {
                switch (priv->lane_version) {
                case 1:
                        return priv->mcast_vcc;
                        break;
                case 2:  /* LANE2 wants arp for multicast addresses */
                        if ( memcmp(mac_to_find, bus_mac, ETH_ALEN) == 0)
                                return priv->mcast_vcc;
                        break;
                default:
                        break;
                }
        }

        entry = lec_arp_find(priv, mac_to_find);
  
        if (entry) {
                if (entry->status == ESI_FORWARD_DIRECT) {
                        /* Connection Ok */
                        entry->last_used = jiffies;
                        *ret_entry = entry;
                        return entry->vcc;
                }
                /* Data direct VC not yet set up, check to see if the unknown
                   frame count is greater than the limit. If the limit has
                   not been reached, allow the caller to send packet to
                   BUS. */
                if (entry->status != ESI_FLUSH_PENDING &&
                    entry->packets_flooded<priv->maximum_unknown_frame_count) {
                        entry->packets_flooded++;
                        DPRINTK("LEC_ARP: Flooding..\n");
                        return priv->mcast_vcc;
                }
		/* We got here because entry->status == ESI_FLUSH_PENDING
		 * or BUS flood limit was reached for an entry which is
		 * in ESI_ARP_PENDING or ESI_VC_PENDING state.
		 */
                *ret_entry = entry;
                DPRINTK("lec: entry->status %d entry->vcc %p\n", entry->status, entry->vcc);
                return NULL;
        } else {
                /* No matching entry was found */
                entry = make_entry(priv, mac_to_find);
                DPRINTK("LEC_ARP: Making entry\n");
                if (!entry) {
                        return priv->mcast_vcc;
                }
                lec_arp_put(priv->lec_arp_tables, entry);
                /* We want arp-request(s) to be sent */
                entry->packets_flooded =1;
                entry->status = ESI_ARP_PENDING;
                entry->no_tries = 1;
                entry->last_used = entry->timestamp = jiffies;
                entry->is_rdesc = is_rdesc;
                if (entry->is_rdesc)
                        send_to_lecd(priv, l_rdesc_arp_xmt, mac_to_find, NULL, NULL);
                else
                        send_to_lecd(priv, l_arp_xmt, mac_to_find, NULL, NULL);
                entry->timer.expires = jiffies + (1*HZ);
                entry->timer.function = lec_arp_expire_arp;
                add_timer(&entry->timer);
                return priv->mcast_vcc;
        }
}

int
lec_addr_delete(struct lec_priv *priv, unsigned char *atm_addr, 
                unsigned long permanent)
{
        struct lec_arp_table *entry, *next;
        int i;

        lec_arp_lock(priv);
        DPRINTK("lec_addr_delete\n");
        for(i=0;i<LEC_ARP_TABLE_SIZE;i++) {
                for(entry=priv->lec_arp_tables[i];entry != NULL; entry=next) {
                        next = entry->next;
                        if (!memcmp(atm_addr, entry->atm_addr, ATM_ESA_LEN)
                            && (permanent || 
                                !(entry->flags & LEC_PERMANENT_FLAG))) {
                                lec_arp_remove(priv->lec_arp_tables, entry);
                                kfree(entry);
                        }
                        lec_arp_unlock(priv);
                        return 0;
                }
        }
        lec_arp_unlock(priv);
        return -1;
}

/*
 * Notifies:  Response to arp_request (atm_addr != NULL) 
 */
void
lec_arp_update(struct lec_priv *priv, unsigned char *mac_addr,
               unsigned char *atm_addr, unsigned long remoteflag,
               unsigned int targetless_le_arp)
{
        struct lec_arp_table *entry, *tmp;
        int i;

        DPRINTK("lec:%s", (targetless_le_arp) ? "targetless ": " ");
        DPRINTK("lec_arp_update mac:%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x\n",
                mac_addr[0],mac_addr[1],mac_addr[2],mac_addr[3],
                mac_addr[4],mac_addr[5]);

        entry = lec_arp_find(priv, mac_addr);
        if (entry == NULL && targetless_le_arp)
                return;   /* LANE2: ignore targetless LE_ARPs for which
                           * we have no entry in the cache. 7.1.30
                           */
        lec_arp_lock(priv);
        if (priv->lec_arp_empty_ones) {
                entry = priv->lec_arp_empty_ones;
                if (!memcmp(entry->atm_addr, atm_addr, ATM_ESA_LEN)) {
                        priv->lec_arp_empty_ones = entry->next;
                } else {
                        while(entry->next && memcmp(entry->next->atm_addr, 
                                                    atm_addr, ATM_ESA_LEN))
                                entry = entry->next;
                        if (entry->next) {
                                tmp = entry;
                                entry = entry->next;
                                tmp->next = entry->next;
                        } else
                                entry = NULL;
                        
                }
                if (entry) {
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
                                kfree(entry);
                                entry=tmp;
                        } else {
                                entry->status = ESI_FORWARD_DIRECT;
                                memcpy(entry->mac_addr, mac_addr, ETH_ALEN);
                                entry->last_used = jiffies;
                                lec_arp_put(priv->lec_arp_tables, entry);
                        }
                        if (remoteflag)
                                entry->flags|=LEC_REMOTE_FLAG;
                        else
                                entry->flags&=~LEC_REMOTE_FLAG;
                        lec_arp_unlock(priv);
                        DPRINTK("After update\n");
                        dump_arp_table(priv);
                        return;
                }
        }
        entry = lec_arp_find(priv, mac_addr);
        if (!entry) {
                entry = make_entry(priv, mac_addr);
                entry->status = ESI_UNKNOWN;
                lec_arp_put(priv->lec_arp_tables, entry);
                /* Temporary, changes before end of function */
        }
        memcpy(entry->atm_addr, atm_addr, ATM_ESA_LEN);
        del_timer(&entry->timer);
        for(i=0;i<LEC_ARP_TABLE_SIZE;i++) {
                for(tmp=priv->lec_arp_tables[i];tmp;tmp=tmp->next) {
                        if (entry != tmp &&
                            !memcmp(tmp->atm_addr, atm_addr,
                                    ATM_ESA_LEN)) { 
                                /* Vcc to this host exists */
                                if (tmp->status > ESI_VC_PENDING) {
                                        /*
                                         * ESI_FLUSH_PENDING,
                                         * ESI_FORWARD_DIRECT
                                         */
                                        entry->vcc = tmp->vcc;
                                        entry->old_push=tmp->old_push;
                                }
                                entry->status=tmp->status;
                                break;
                        }
                }
        }
        if (remoteflag)
                entry->flags|=LEC_REMOTE_FLAG;
        else
                entry->flags&=~LEC_REMOTE_FLAG;
        if (entry->status == ESI_ARP_PENDING ||
            entry->status == ESI_UNKNOWN) {
                entry->status = ESI_VC_PENDING;
                send_to_lecd(priv, l_svc_setup, entry->mac_addr, atm_addr, NULL);
        }
        DPRINTK("After update2\n");
        dump_arp_table(priv);
        lec_arp_unlock(priv);
}

/*
 * Notifies: Vcc setup ready 
 */
void
lec_vcc_added(struct lec_priv *priv, struct atmlec_ioc *ioc_data,
              struct atm_vcc *vcc,
              void (*old_push)(struct atm_vcc *vcc, struct sk_buff *skb))
{
        struct lec_arp_table *entry;
        int i, found_entry=0;

        lec_arp_lock(priv);
        if (ioc_data->receive == 2) {
                /* Vcc for Multicast Forward. No timer, LANEv2 7.1.20 and 2.3.5.3 */

                DPRINTK("LEC_ARP: Attaching mcast forward\n");
#if 0
                entry = lec_arp_find(priv, bus_mac);
                if (!entry) {
                        printk("LEC_ARP: Multicast entry not found!\n");
                        lec_arp_unlock(priv);
                        return;
                }
                memcpy(entry->atm_addr, ioc_data->atm_addr, ATM_ESA_LEN);
                entry->recv_vcc = vcc;
                entry->old_recv_push = old_push;
#endif
                entry = make_entry(priv, bus_mac);
                if (entry == NULL) {
                        lec_arp_unlock(priv);
                        return;
                }
                del_timer(&entry->timer);
                memcpy(entry->atm_addr, ioc_data->atm_addr, ATM_ESA_LEN);
                entry->recv_vcc = vcc;
                entry->old_recv_push = old_push;
                entry->next = priv->mcast_fwds;
                priv->mcast_fwds = entry;
                lec_arp_unlock(priv);
                return;
        } else if (ioc_data->receive == 1) {
                /* Vcc which we don't want to make default vcc, attach it
                   anyway. */
                DPRINTK("LEC_ARP:Attaching data direct, not default :%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x\n",
                        ioc_data->atm_addr[0],ioc_data->atm_addr[1],
                        ioc_data->atm_addr[2],ioc_data->atm_addr[3],
                        ioc_data->atm_addr[4],ioc_data->atm_addr[5],
                        ioc_data->atm_addr[6],ioc_data->atm_addr[7],
                        ioc_data->atm_addr[8],ioc_data->atm_addr[9],
                        ioc_data->atm_addr[10],ioc_data->atm_addr[11],
                        ioc_data->atm_addr[12],ioc_data->atm_addr[13],
                        ioc_data->atm_addr[14],ioc_data->atm_addr[15],
                        ioc_data->atm_addr[16],ioc_data->atm_addr[17],
                        ioc_data->atm_addr[18],ioc_data->atm_addr[19]);
                entry = make_entry(priv, bus_mac);
                memcpy(entry->atm_addr, ioc_data->atm_addr, ATM_ESA_LEN);
                memset(entry->mac_addr, 0, ETH_ALEN);
                entry->recv_vcc = vcc;
                entry->old_recv_push = old_push;
                entry->status = ESI_UNKNOWN;
                entry->timer.expires = jiffies + priv->vcc_timeout_period;
                entry->timer.function = lec_arp_expire_vcc;
                add_timer(&entry->timer);
                entry->next = priv->lec_no_forward;
                priv->lec_no_forward = entry;
                lec_arp_unlock(priv);
		dump_arp_table(priv);
                return;
        }
        DPRINTK("LEC_ARP:Attaching data direct, default:%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x\n",
                ioc_data->atm_addr[0],ioc_data->atm_addr[1],
                ioc_data->atm_addr[2],ioc_data->atm_addr[3],
                ioc_data->atm_addr[4],ioc_data->atm_addr[5],
                ioc_data->atm_addr[6],ioc_data->atm_addr[7],
                ioc_data->atm_addr[8],ioc_data->atm_addr[9],
                ioc_data->atm_addr[10],ioc_data->atm_addr[11],
                ioc_data->atm_addr[12],ioc_data->atm_addr[13],
                ioc_data->atm_addr[14],ioc_data->atm_addr[15],
                ioc_data->atm_addr[16],ioc_data->atm_addr[17],
                ioc_data->atm_addr[18],ioc_data->atm_addr[19]);
        for (i=0;i<LEC_ARP_TABLE_SIZE;i++) {
                for (entry = priv->lec_arp_tables[i];entry;entry=entry->next) {
                        if (memcmp(ioc_data->atm_addr, entry->atm_addr, 
                                   ATM_ESA_LEN)==0) {
                                DPRINTK("LEC_ARP: Attaching data direct\n");
                                DPRINTK("Currently -> Vcc: %d, Rvcc:%d\n",
                                        entry->vcc?entry->vcc->vci:0,
                                        entry->recv_vcc?entry->recv_vcc->vci:0);
                                found_entry=1;
                                del_timer(&entry->timer);
                                entry->vcc = vcc;
                                entry->old_push = old_push;
                                if (entry->status == ESI_VC_PENDING) {
                                        if(priv->maximum_unknown_frame_count
                                           ==0)
                                                entry->status = 
                                                        ESI_FORWARD_DIRECT;
                                        else {
                                                entry->timestamp = jiffies;
                                                entry->status = 
                                                        ESI_FLUSH_PENDING;
#if 0
                                                send_to_lecd(priv,l_flush_xmt,
                                                             NULL,
                                                             entry->atm_addr,
                                                             NULL);
#endif
                                        }
                                } else {
                                        /* They were forming a connection
                                           to us, and we to them. Our
                                           ATM address is numerically lower
                                           than theirs, so we make connection
                                           we formed into default VCC (8.1.11).
                                           Connection they made gets torn
                                           down. This might confuse some
                                           clients. Can be changed if
                                           someone reports trouble... */
                                        ;
                                }
                        }
                }
        }
        if (found_entry) {
                lec_arp_unlock(priv);
                DPRINTK("After vcc was added\n");
                dump_arp_table(priv);
                return;
        }
        /* Not found, snatch address from first data packet that arrives from
           this vcc */
        entry = make_entry(priv, bus_mac);
        entry->vcc = vcc;
        entry->old_push = old_push;
        memcpy(entry->atm_addr, ioc_data->atm_addr, ATM_ESA_LEN);
        memset(entry->mac_addr, 0, ETH_ALEN);
        entry->status = ESI_UNKNOWN;
        entry->next = priv->lec_arp_empty_ones;
        priv->lec_arp_empty_ones = entry;
        entry->timer.expires = jiffies + priv->vcc_timeout_period;
        entry->timer.function = lec_arp_expire_vcc;
        add_timer(&entry->timer);
        lec_arp_unlock(priv);
        DPRINTK("After vcc was added\n");
	dump_arp_table(priv);
}

void
lec_flush_complete(struct lec_priv *priv, unsigned long tran_id)
{
        struct lec_arp_table *entry;
        int i;
  
        DPRINTK("LEC:lec_flush_complete %lx\n",tran_id);
        for (i=0;i<LEC_ARP_TABLE_SIZE;i++) {
                for (entry=priv->lec_arp_tables[i];entry;entry=entry->next) {
                        if (entry->flush_tran_id == tran_id &&
                            entry->status == ESI_FLUSH_PENDING) {
                                entry->status = ESI_FORWARD_DIRECT;
                                DPRINTK("LEC_ARP: Flushed\n");
                        }
                }
        }
        dump_arp_table(priv);
}

void
lec_set_flush_tran_id(struct lec_priv *priv,
                      unsigned char *atm_addr, unsigned long tran_id)
{
        struct lec_arp_table *entry;
        int i;

        for (i=0;i<LEC_ARP_TABLE_SIZE;i++)
                for(entry=priv->lec_arp_tables[i];entry;entry=entry->next)
                        if (!memcmp(atm_addr, entry->atm_addr, ATM_ESA_LEN)) {
                                entry->flush_tran_id = tran_id;
                                DPRINTK("Set flush transaction id to %lx for %p\n",tran_id,entry);
                        }
}

int 
lec_mcast_make(struct lec_priv *priv, struct atm_vcc *vcc)
{
        unsigned char mac_addr[] = {
                0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        struct lec_arp_table *to_add;
  
        lec_arp_lock(priv);
        to_add = make_entry(priv, mac_addr);
        if (!to_add) {
                lec_arp_unlock(priv);
                return -ENOMEM;
        }
        memcpy(to_add->atm_addr, vcc->remote.sas_addr.prv, ATM_ESA_LEN);
        to_add->status = ESI_FORWARD_DIRECT;
        to_add->flags |= LEC_PERMANENT_FLAG;
        to_add->vcc = vcc;
        to_add->old_push = vcc->push;
        vcc->push = lec_push;
        priv->mcast_vcc = vcc;
        lec_arp_put(priv->lec_arp_tables, to_add);
        lec_arp_unlock(priv);
        return 0;
}

void
lec_vcc_close(struct lec_priv *priv, struct atm_vcc *vcc)
{
        struct lec_arp_table *entry, *next;
        int i;

        DPRINTK("LEC_ARP: lec_vcc_close vpi:%d vci:%d\n",vcc->vpi,vcc->vci);
        dump_arp_table(priv);
        lec_arp_lock(priv);
        for(i=0;i<LEC_ARP_TABLE_SIZE;i++) {
                for(entry = priv->lec_arp_tables[i];entry; entry=next) {
                        next = entry->next;
                        if (vcc == entry->vcc) {
                                lec_arp_remove(priv->lec_arp_tables,entry);
                                kfree(entry);
                                if (priv->mcast_vcc == vcc) {
                                        priv->mcast_vcc = NULL;
                                }
                        }
                }
        }

        entry = priv->lec_arp_empty_ones;
        priv->lec_arp_empty_ones = NULL;
        while (entry != NULL) {
                next = entry->next;
                if (entry->vcc == vcc) { /* leave it out from the list */
                        lec_arp_clear_vccs(entry);
                        del_timer(&entry->timer);
                        kfree(entry);
                }
                else {              /* put it back to the list */
                        entry->next = priv->lec_arp_empty_ones;
                        priv->lec_arp_empty_ones = entry;
                }
                entry = next;
        }
        
        entry = priv->lec_no_forward;
        priv->lec_no_forward = NULL;
        while (entry != NULL) {
                next = entry->next;
                if (entry->recv_vcc == vcc) {
                        lec_arp_clear_vccs(entry);
                        del_timer(&entry->timer);
                        kfree(entry);
                }
                else {
                        entry->next = priv->lec_no_forward;
                        priv->lec_no_forward = entry;
                }
                entry = next;
        }

        entry = priv->mcast_fwds;
        priv->mcast_fwds = NULL;
        while (entry != NULL) {
                next = entry->next;
                if (entry->recv_vcc == vcc) {
                        lec_arp_clear_vccs(entry);
                        /* No timer, LANEv2 7.1.20 and 2.3.5.3 */
                        kfree(entry);
                }
                else {
                        entry->next = priv->mcast_fwds;
                        priv->mcast_fwds = entry;
                }
                entry = next;
        }

        lec_arp_unlock(priv);
	dump_arp_table(priv);
}

void
lec_arp_check_empties(struct lec_priv *priv,
                      struct atm_vcc *vcc, struct sk_buff *skb)
{
        struct lec_arp_table *entry, *prev;
        struct lecdatahdr_8023 *hdr = (struct lecdatahdr_8023 *)skb->data;
        unsigned long flags;
        unsigned char *src;
#ifdef CONFIG_TR
        struct lecdatahdr_8025 *tr_hdr = (struct lecdatahdr_8025 *)skb->data;

        if (priv->is_trdev) src = tr_hdr->h_source;
        else
#endif
        src = hdr->h_source;

        lec_arp_lock(priv);
        entry = priv->lec_arp_empty_ones;
        if (vcc == entry->vcc) {
                save_flags(flags);
                cli();
                del_timer(&entry->timer);
                memcpy(entry->mac_addr, src, ETH_ALEN);
                entry->status = ESI_FORWARD_DIRECT;
                entry->last_used = jiffies;
                priv->lec_arp_empty_ones = entry->next;
                restore_flags(flags);
                /* We might have got an entry */
                if ((prev=lec_arp_find(priv,src))) {
                        lec_arp_remove(priv->lec_arp_tables, prev);
                        kfree(prev);
                }
                lec_arp_put(priv->lec_arp_tables, entry);
                lec_arp_unlock(priv);
                return;
        }
        prev = entry;
        entry = entry->next;
        while (entry && entry->vcc != vcc) {
                prev= entry;
                entry = entry->next;
        }
        if (!entry) {
                DPRINTK("LEC_ARP: Arp_check_empties: entry not found!\n");
                lec_arp_unlock(priv);
                return;
        }
        save_flags(flags);
        cli();
        del_timer(&entry->timer);
        memcpy(entry->mac_addr, src, ETH_ALEN);
        entry->status = ESI_FORWARD_DIRECT;
        entry->last_used = jiffies;
        prev->next = entry->next;
        restore_flags(flags);
        if ((prev = lec_arp_find(priv, src))) {
                lec_arp_remove(priv->lec_arp_tables,prev);
                kfree(prev);
        }
        lec_arp_put(priv->lec_arp_tables,entry);
        lec_arp_unlock(priv);  
}
