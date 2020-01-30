/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the Interfaces handler.
 *
 * Version:	@(#)dev.h	1.0.10	08/12/93
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Corey Minyard <wf-rch!minyard@relay.EU.net>
 *		Donald J. Becker, <becker@cesdis.gsfc.nasa.gov>
 *		Alan Cox, <Alan.Cox@linux.org>
 *		Bjorn Ekwall. <bj0rn@blox.se>
 *              Pekka Riikonen <priikone@poseidon.pspt.fi>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 *		Moved to /usr/include/linux for NET3
 */
#ifndef _LINUX_NETDEVICE_H
#define _LINUX_NETDEVICE_H

#include <linux/if.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>

#ifdef __KERNEL__
#include <asm/atomic.h>
#include <asm/cache.h>
#include <asm/byteorder.h>

#include <linux/config.h>
#include <linux/device.h>
#include <linux/percpu.h>

struct divert_blk;
struct vlan_group;
struct ethtool_ops;

					/* source back-compat hooks */
#define SET_ETHTOOL_OPS(netdev,ops) \
	( (netdev)->ethtool_ops = (ops) )

#define HAVE_ALLOC_NETDEV		/* feature macro: alloc_xxxdev
					   functions are available. */
#define HAVE_FREE_NETDEV		/* free_netdev() */
#define HAVE_NETDEV_PRIV		/* netdev_priv() */

#define NET_XMIT_SUCCESS	0
#define NET_XMIT_DROP		1	/* skb dropped			*/
#define NET_XMIT_CN		2	/* congestion notification	*/
#define NET_XMIT_POLICED	3	/* skb is shot by police	*/
#define NET_XMIT_BYPASS		4	/* packet does not leave via dequeue;
					   (TC use only - dev_queue_xmit
					   returns this as NET_XMIT_SUCCESS) */

/* Backlog congestion levels */
#define NET_RX_SUCCESS		0   /* keep 'em coming, baby */
#define NET_RX_DROP		1  /* packet dropped */
#define NET_RX_CN_LOW		2   /* storm alert, just in case */
#define NET_RX_CN_MOD		3   /* Storm on its way! */
#define NET_RX_CN_HIGH		4   /* The storm is here */
#define NET_RX_BAD		5  /* packet dropped due to kernel error */

#define net_xmit_errno(e)	((e) != NET_XMIT_CN ? -ENOBUFS : 0)

#endif

#define MAX_ADDR_LEN	32		/* Largest hardware address length */

/*
 *	Compute the worst case header length according to the protocols
 *	used.
 */
 
#if !defined(CONFIG_AX25) && !defined(CONFIG_AX25_MODULE) && !defined(CONFIG_TR)
#define LL_MAX_HEADER	32
#else
#if defined(CONFIG_AX25) || defined(CONFIG_AX25_MODULE)
#define LL_MAX_HEADER	96
#else
#define LL_MAX_HEADER	48
#endif
#endif

#if !defined(CONFIG_NET_IPIP) && \
    !defined(CONFIG_IPV6) && !defined(CONFIG_IPV6_MODULE)
#define MAX_HEADER LL_MAX_HEADER
#else
#define MAX_HEADER (LL_MAX_HEADER + 48)
#endif

/*
 *	Network device statistics. Akin to the 2.0 ether stats but
 *	with byte counters.
 */
 
struct net_device_stats
{
	unsigned long	rx_packets;		/* total packets received	*/
	unsigned long	tx_packets;		/* total packets transmitted	*/
	unsigned long	rx_bytes;		/* total bytes received 	*/
	unsigned long	tx_bytes;		/* total bytes transmitted	*/
	unsigned long	rx_errors;		/* bad packets received		*/
	unsigned long	tx_errors;		/* packet transmit problems	*/
	unsigned long	rx_dropped;		/* no space in linux buffers	*/
	unsigned long	tx_dropped;		/* no space available in linux	*/
	unsigned long	multicast;		/* multicast packets received	*/
	unsigned long	collisions;

	/* detailed rx_errors: */
	unsigned long	rx_length_errors;
	unsigned long	rx_over_errors;		/* receiver ring buff overflow	*/
	unsigned long	rx_crc_errors;		/* recved pkt with crc error	*/
	unsigned long	rx_frame_errors;	/* recv'd frame alignment error */
	unsigned long	rx_fifo_errors;		/* recv'r fifo overrun		*/
	unsigned long	rx_missed_errors;	/* receiver missed packet	*/

	/* detailed tx_errors */
	unsigned long	tx_aborted_errors;
	unsigned long	tx_carrier_errors;
	unsigned long	tx_fifo_errors;
	unsigned long	tx_heartbeat_errors;
	unsigned long	tx_window_errors;
	
	/* for cslip etc */
	unsigned long	rx_compressed;
	unsigned long	tx_compressed;
};


/* Media selection options. */
enum {
        IF_PORT_UNKNOWN = 0,
        IF_PORT_10BASE2,
        IF_PORT_10BASET,
        IF_PORT_AUI,
        IF_PORT_100BASET,
        IF_PORT_100BASETX,
        IF_PORT_100BASEFX
};

#ifdef __KERNEL__

#include <linux/cache.h>
#include <linux/skbuff.h>

struct neighbour;
struct neigh_parms;
struct sk_buff;

struct netif_rx_stats
{
	unsigned total;
	unsigned dropped;
	unsigned time_squeeze;
	unsigned throttled;
	unsigned fastroute_hit;
	unsigned fastroute_success;
	unsigned fastroute_defer;
	unsigned fastroute_deferred_out;
	unsigned fastroute_latency_reduction;
	unsigned cpu_collision;
};

DECLARE_PER_CPU(struct netif_rx_stats, netdev_rx_stat);


/*
 *	We tag multicasts with these structures.
 */
 
struct dev_mc_list
{	
	struct dev_mc_list	*next;
	__u8			dmi_addr[MAX_ADDR_LEN];
	unsigned char		dmi_addrlen;
	int			dmi_users;
	int			dmi_gusers;
};

struct hh_cache
{
	struct hh_cache *hh_next;	/* Next entry			     */
	atomic_t	hh_refcnt;	/* number of users                   */
	unsigned short  hh_type;	/* protocol identifier, f.e ETH_P_IP
                                         *  NOTE:  For VLANs, this will be the
                                         *  encapuslated type. --BLG
                                         */
	int		hh_len;		/* length of header */
	int		(*hh_output)(struct sk_buff *skb);
	rwlock_t	hh_lock;

	/* cached hardware header; allow for machine alignment needs.        */
#define HH_DATA_MOD	16
#define HH_DATA_OFF(__len) \
	(HH_DATA_MOD - ((__len) & (HH_DATA_MOD - 1)))
#define HH_DATA_ALIGN(__len) \
	(((__len)+(HH_DATA_MOD-1))&~(HH_DATA_MOD - 1))
	unsigned long	hh_data[HH_DATA_ALIGN(LL_MAX_HEADER) / sizeof(long)];
};

/* Reserve HH_DATA_MOD byte aligned hard_header_len, but at least that much.
 * Alternative is:
 *   dev->hard_header_len ? (dev->hard_header_len +
 *                           (HH_DATA_MOD - 1)) & ~(HH_DATA_MOD - 1) : 0
 *
 * We could use other alignment values, but we must maintain the
 * relationship HH alignment <= LL alignment.
 */
#define LL_RESERVED_SPACE(dev) \
	(((dev)->hard_header_len&~(HH_DATA_MOD - 1)) + HH_DATA_MOD)
#define LL_RESERVED_SPACE_EXTRA(dev,extra) \
	((((dev)->hard_header_len+extra)&~(HH_DATA_MOD - 1)) + HH_DATA_MOD)

/* These flag bits are private to the generic network queueing
 * layer, they may not be explicitly referenced by any other
 * code.
 */

enum netdev_state_t
{
	__LINK_STATE_XOFF=0,
	__LINK_STATE_START,
	__LINK_STATE_PRESENT,
	__LINK_STATE_SCHED,
	__LINK_STATE_NOCARRIER,
	__LINK_STATE_RX_SCHED,
	__LINK_STATE_LINKWATCH_PENDING
};


/*
 * This structure holds at boot time configured netdevice settings. They
 * are then used in the device probing. 
 */
struct netdev_boot_setup {
	char name[IFNAMSIZ];
	struct ifmap map;
};
#define NETDEV_BOOT_SETUP_MAX 8


/*
 *	The DEVICE structure.
 *	Actually, this whole structure is a big mistake.  It mixes I/O
 *	data with strictly "high-level" data, and it has to know about
 *	almost every data structure used in the INET module.
 *
 *	FIXME: cleanup struct net_device such that network protocol info
 *	moves out.
 */

struct net_device
{

	/*
	 * This is the first field of the "visible" part of this structure
	 * (i.e. as seen by users in the "Space.c" file).  It is the name
	 * the interface.
	 */
	char			name[IFNAMSIZ];

	/*
	 *	I/O specific fields
	 *	FIXME: Merge these and struct ifmap into one
	 */
	unsigned long		mem_end;	/* shared mem end	*/
	unsigned long		mem_start;	/* shared mem start	*/
	unsigned long		base_addr;	/* device I/O address	*/
	unsigned int		irq;		/* device IRQ number	*/

	/*
	 *	Some hardware also needs these fields, but they are not
	 *	part of the usual set specified in Space.c.
	 */

	unsigned char		if_port;	/* Selectable AUI, TP,..*/
	unsigned char		dma;		/* DMA channel		*/

	unsigned long		state;

	struct net_device	*next;
	
	/* The device initialization function. Called only once. */
	int			(*init)(struct net_device *dev);

	/* ------- Fields preinitialized in Space.c finish here ------- */

	struct net_device	*next_sched;

	/* Interface index. Unique device identifier	*/
	int			ifindex;
	int			iflink;


	struct net_device_stats* (*get_stats)(struct net_device *dev);
	struct iw_statistics*	(*get_wireless_stats)(struct net_device *dev);

	/* List of functions to handle Wireless Extensions (instead of ioctl).
	 * See <net/iw_handler.h> for details. Jean II */
	struct iw_handler_def *	wireless_handlers;

	struct ethtool_ops *ethtool_ops;

	/*
	 * This marks the end of the "visible" part of the structure. All
	 * fields hereafter are internal to the system, and may change at
	 * will (read: may be cleaned up at will).
	 */

	/* These may be needed for future network-power-down code. */
	unsigned long		trans_start;	/* Time (in jiffies) of last Tx	*/
	unsigned long		last_rx;	/* Time of last Rx	*/

	unsigned short		flags;	/* interface flags (a la BSD)	*/
	unsigned short		gflags;
        unsigned short          priv_flags; /* Like 'flags' but invisible to userspace. */
        unsigned short          unused_alignment_fixer; /* Because we need priv_flags,
                                                         * and we want to be 32-bit aligned.
                                                         */

	unsigned		mtu;	/* interface MTU value		*/
	unsigned short		type;	/* interface hardware type	*/
	unsigned short		hard_header_len;	/* hardware hdr length	*/
	void			*priv;	/* pointer to private data	*/

	struct net_device	*master; /* Pointer to master device of a group,
					  * which this device is member of.
					  */

	/* Interface address info. */
	unsigned char		broadcast[MAX_ADDR_LEN];	/* hw bcast add	*/
	unsigned char		dev_addr[MAX_ADDR_LEN];	/* hw address	*/
	unsigned char		addr_len;	/* hardware address length	*/

	struct dev_mc_list	*mc_list;	/* Multicast mac addresses	*/
	int			mc_count;	/* Number of installed mcasts	*/
	int			promiscuity;
	int			allmulti;

	int			watchdog_timeo;
	struct timer_list	watchdog_timer;

	/* Protocol specific pointers */
	
	void 			*atalk_ptr;	/* AppleTalk link 	*/
	void			*ip_ptr;	/* IPv4 specific data	*/  
	void                    *dn_ptr;        /* DECnet specific data */
	void                    *ip6_ptr;       /* IPv6 specific data */
	void			*ec_ptr;	/* Econet specific data	*/
	void			*ax25_ptr;	/* AX.25 specific data */

	struct list_head	poll_list;	/* Link to poll list	*/
	int			quota;
	int			weight;

	struct Qdisc		*qdisc;
	struct Qdisc		*qdisc_sleeping;
	struct Qdisc		*qdisc_ingress;
	struct list_head	qdisc_list;
	unsigned long		tx_queue_len;	/* Max frames per queue allowed */

	/* ingress path synchronizer */
	spinlock_t		ingress_lock;
	/* hard_start_xmit synchronizer */
	spinlock_t		xmit_lock;
	/* cpu id of processor entered to hard_start_xmit or -1,
	   if nobody entered there.
	 */
	int			xmit_lock_owner;
	/* device queue lock */
	spinlock_t		queue_lock;
	/* Number of references to this device */
	atomic_t		refcnt;
	/* delayed register/unregister */
	struct list_head	todo_list;
	/* device name hash chain */
	struct hlist_node	name_hlist;
	/* device index hash chain */
	struct hlist_node	index_hlist;

	/* register/unregister state machine */
	enum { NETREG_UNINITIALIZED=0,
	       NETREG_REGISTERING,	/* called register_netdevice */
	       NETREG_REGISTERED,	/* completed register todo */
	       NETREG_UNREGISTERING,	/* called unregister_netdevice */
	       NETREG_UNREGISTERED,	/* completed unregister todo */
	       NETREG_RELEASED,		/* called free_netdev */
	} reg_state;

	/* Net device features */
	int			features;
#define NETIF_F_SG		1	/* Scatter/gather IO. */
#define NETIF_F_IP_CSUM		2	/* Can checksum only TCP/UDP over IPv4. */
#define NETIF_F_NO_CSUM		4	/* Does not require checksum. F.e. loopack. */
#define NETIF_F_HW_CSUM		8	/* Can checksum all the packets. */
#define NETIF_F_HIGHDMA		32	/* Can DMA to high memory. */
#define NETIF_F_FRAGLIST	64	/* Scatter/gather IO. */
#define NETIF_F_HW_VLAN_TX	128	/* Transmit VLAN hw acceleration */
#define NETIF_F_HW_VLAN_RX	256	/* Receive VLAN hw acceleration */
#define NETIF_F_HW_VLAN_FILTER	512	/* Receive filtering on VLAN */
#define NETIF_F_VLAN_CHALLENGED	1024	/* Device cannot handle VLAN packets */
#define NETIF_F_TSO		2048	/* Can offload TCP/IP segmentation */
#define NETIF_F_LLTX		4096	/* LockLess TX */

	/* Called after device is detached from network. */
	void			(*uninit)(struct net_device *dev);
	/* Called after last user reference disappears. */
	void			(*destructor)(struct net_device *dev);

	/* Pointers to interface service routines.	*/
	int			(*open)(struct net_device *dev);
	int			(*stop)(struct net_device *dev);
	int			(*hard_start_xmit) (struct sk_buff *skb,
						    struct net_device *dev);
#define HAVE_NETDEV_POLL
	int			(*poll) (struct net_device *dev, int *quota);
	int			(*hard_header) (struct sk_buff *skb,
						struct net_device *dev,
						unsigned short type,
						void *daddr,
						void *saddr,
						unsigned len);
	int			(*rebuild_header)(struct sk_buff *skb);
#define HAVE_MULTICAST			 
	void			(*set_multicast_list)(struct net_device *dev);
#define HAVE_SET_MAC_ADDR  		 
	int			(*set_mac_address)(struct net_device *dev,
						   void *addr);
#define HAVE_PRIVATE_IOCTL
	int			(*do_ioctl)(struct net_device *dev,
					    struct ifreq *ifr, int cmd);
#define HAVE_SET_CONFIG
	int			(*set_config)(struct net_device *dev,
					      struct ifmap *map);
#define HAVE_HEADER_CACHE
	int			(*hard_header_cache)(struct neighbour *neigh,
						     struct hh_cache *hh);
	void			(*header_cache_update)(struct hh_cache *hh,
						       struct net_device *dev,
						       unsigned char *  haddr);
#define HAVE_CHANGE_MTU
	int			(*change_mtu)(struct net_device *dev, int new_mtu);

#define HAVE_TX_TIMEOUT
	void			(*tx_timeout) (struct net_device *dev);

	void			(*vlan_rx_register)(struct net_device *dev,
						    struct vlan_group *grp);
	void			(*vlan_rx_add_vid)(struct net_device *dev,
						   unsigned short vid);
	void			(*vlan_rx_kill_vid)(struct net_device *dev,
						    unsigned short vid);

	int			(*hard_header_parse)(struct sk_buff *skb,
						     unsigned char *haddr);
	int			(*neigh_setup)(struct net_device *dev, struct neigh_parms *);
	int			(*accept_fastpath)(struct net_device *, struct dst_entry*);
#ifdef CONFIG_NETPOLL_RX
	int			netpoll_rx;
#endif
#ifdef CONFIG_NET_POLL_CONTROLLER
	void                    (*poll_controller)(struct net_device *dev);
#endif

	/* bridge stuff */
	struct net_bridge_port	*br_port;

#ifdef CONFIG_NET_DIVERT
	/* this will get initialized at each interface type init routine */
	struct divert_blk	*divert;
#endif /* CONFIG_NET_DIVERT */

	/* class/net/name entry */
	struct class_device	class_dev;
	struct net_device_stats* (*last_stats)(struct net_device *);
	/* how much padding had been added by alloc_netdev() */
	int padded;
};

#define	NETDEV_ALIGN		32
#define	NETDEV_ALIGN_CONST	(NETDEV_ALIGN - 1)

static inline void *netdev_priv(struct net_device *dev)
{
	return (char *)dev + ((sizeof(struct net_device)
					+ NETDEV_ALIGN_CONST)
				& ~NETDEV_ALIGN_CONST);
}

#define SET_MODULE_OWNER(dev) do { } while (0)
/* Set the sysfs physical device reference for the network logical device
 * if set prior to registration will cause a symlink during initialization.
 */
#define SET_NETDEV_DEV(net, pdev)	((net)->class_dev.dev = (pdev))

struct packet_type {
	unsigned short		type;	/* This is really htons(ether_type).	*/
	struct net_device		*dev;	/* NULL is wildcarded here		*/
	int			(*func) (struct sk_buff *, struct net_device *,
					 struct packet_type *);
	void			*af_packet_priv;
	struct list_head	list;
};

#include <linux/interrupt.h>
#include <linux/notifier.h>

extern struct net_device		loopback_dev;		/* The loopback */
extern struct net_device		*dev_base;		/* All devices */
extern rwlock_t				dev_base_lock;		/* Device list lock */

extern int			netdev_boot_setup_add(char *name, struct ifmap *map);
extern int 			netdev_boot_setup_check(struct net_device *dev);
extern unsigned long		netdev_boot_base(const char *prefix, int unit);
extern struct net_device    *dev_getbyhwaddr(unsigned short type, char *hwaddr);
extern struct net_device *__dev_getfirstbyhwtype(unsigned short type);
extern struct net_device *dev_getfirstbyhwtype(unsigned short type);
extern void		dev_add_pack(struct packet_type *pt);
extern void		dev_remove_pack(struct packet_type *pt);
extern void		__dev_remove_pack(struct packet_type *pt);
extern int		__dev_get(const char *name);
static inline int __deprecated dev_get(const char *name)
{
	return __dev_get(name);
}
extern struct net_device	*dev_get_by_flags(unsigned short flags,
						  unsigned short mask);
extern struct net_device	*__dev_get_by_flags(unsigned short flags,
						    unsigned short mask);
extern struct net_device	*dev_get_by_name(const char *name);
extern struct net_device	*__dev_get_by_name(const char *name);
extern int		dev_alloc_name(struct net_device *dev, const char *name);
extern int		dev_open(struct net_device *dev);
extern int		dev_close(struct net_device *dev);
extern int		dev_queue_xmit(struct sk_buff *skb);
extern int		register_netdevice(struct net_device *dev);
extern int		unregister_netdevice(struct net_device *dev);
extern void		free_netdev(struct net_device *dev);
extern void		synchronize_net(void);
extern int 		register_netdevice_notifier(struct notifier_block *nb);
extern int		unregister_netdevice_notifier(struct notifier_block *nb);
extern int		call_netdevice_notifiers(unsigned long val, void *v);
extern int		dev_new_index(void);
extern struct net_device	*dev_get_by_index(int ifindex);
extern struct net_device	*__dev_get_by_index(int ifindex);
extern int		dev_restart(struct net_device *dev);
#ifdef CONFIG_NETPOLL_TRAP
extern int		netpoll_trap(void);
#endif

typedef int gifconf_func_t(struct net_device * dev, char __user * bufptr, int len);
extern int		register_gifconf(unsigned int family, gifconf_func_t * gifconf);
static inline int unregister_gifconf(unsigned int family)
{
	return register_gifconf(family, NULL);
}

/*
 * Incoming packets are placed on per-cpu queues so that
 * no locking is needed.
 */

struct softnet_data
{
	int			throttle;
	int			cng_level;
	int			avg_blog;
	struct sk_buff_head	input_pkt_queue;
	struct list_head	poll_list;
	struct net_device	*output_queue;
	struct sk_buff		*completion_queue;

	struct net_device	backlog_dev;	/* Sorry. 8) */
};

DECLARE_PER_CPU(struct softnet_data,softnet_data);

#define HAVE_NETIF_QUEUE

static inline void __netif_schedule(struct net_device *dev)
{
	if (!test_and_set_bit(__LINK_STATE_SCHED, &dev->state)) {
		unsigned long flags;
		struct softnet_data *sd;

		local_irq_save(flags);
		sd = &__get_cpu_var(softnet_data);
		dev->next_sched = sd->output_queue;
		sd->output_queue = dev;
		raise_softirq_irqoff(NET_TX_SOFTIRQ);
		local_irq_restore(flags);
	}
}

static inline void netif_schedule(struct net_device *dev)
{
	if (!test_bit(__LINK_STATE_XOFF, &dev->state))
		__netif_schedule(dev);
}

static inline void netif_start_queue(struct net_device *dev)
{
	clear_bit(__LINK_STATE_XOFF, &dev->state);
}

static inline void netif_wake_queue(struct net_device *dev)
{
#ifdef CONFIG_NETPOLL_TRAP
	if (netpoll_trap())
		return;
#endif
	if (test_and_clear_bit(__LINK_STATE_XOFF, &dev->state))
		__netif_schedule(dev);
}

static inline void netif_stop_queue(struct net_device *dev)
{
#ifdef CONFIG_NETPOLL_TRAP
	if (netpoll_trap())
		return;
#endif
	set_bit(__LINK_STATE_XOFF, &dev->state);
}

static inline int netif_queue_stopped(const struct net_device *dev)
{
	return test_bit(__LINK_STATE_XOFF, &dev->state);
}

static inline int netif_running(const struct net_device *dev)
{
	return test_bit(__LINK_STATE_START, &dev->state);
}


/* Use this variant when it is known for sure that it
 * is executing from interrupt context.
 */
static inline void dev_kfree_skb_irq(struct sk_buff *skb)
{
	if (atomic_dec_and_test(&skb->users)) {
		struct softnet_data *sd;
		unsigned long flags;

		local_irq_save(flags);
		sd = &__get_cpu_var(softnet_data);
		skb->next = sd->completion_queue;
		sd->completion_queue = skb;
		raise_softirq_irqoff(NET_TX_SOFTIRQ);
		local_irq_restore(flags);
	}
}

/* Use this variant in places where it could be invoked
 * either from interrupt or non-interrupt context.
 */
static inline void dev_kfree_skb_any(struct sk_buff *skb)
{
	if (in_irq() || irqs_disabled())
		dev_kfree_skb_irq(skb);
	else
		dev_kfree_skb(skb);
}

#define HAVE_NETIF_RX 1
extern int		netif_rx(struct sk_buff *skb);
#define HAVE_NETIF_RECEIVE_SKB 1
extern int		netif_receive_skb(struct sk_buff *skb);
extern int		dev_ioctl(unsigned int cmd, void __user *);
extern int		dev_ethtool(struct ifreq *);
extern unsigned		dev_get_flags(const struct net_device *);
extern int		dev_change_flags(struct net_device *, unsigned);
extern int		dev_set_mtu(struct net_device *, int);
extern void		dev_queue_xmit_nit(struct sk_buff *skb, struct net_device *dev);

extern void		dev_init(void);

extern int		netdev_nit;

/* Post buffer to the network code from _non interrupt_ context.
 * see net/core/dev.c for netif_rx description.
 */
static inline int netif_rx_ni(struct sk_buff *skb)
{
       int err = netif_rx(skb);
       if (softirq_pending(smp_processor_id()))
               do_softirq();
       return err;
}

/* Called by rtnetlink.c:rtnl_unlock() */
extern void netdev_run_todo(void);

static inline void dev_put(struct net_device *dev)
{
	atomic_dec(&dev->refcnt);
}

#define __dev_put(dev) atomic_dec(&(dev)->refcnt)
#define dev_hold(dev) atomic_inc(&(dev)->refcnt)

/* Carrier loss detection, dial on demand. The functions netif_carrier_on
 * and _off may be called from IRQ context, but it is caller
 * who is responsible for serialization of these calls.
 */

extern void linkwatch_fire_event(struct net_device *dev);

static inline int netif_carrier_ok(const struct net_device *dev)
{
	return !test_bit(__LINK_STATE_NOCARRIER, &dev->state);
}

extern void __netdev_watchdog_up(struct net_device *dev);

static inline void netif_carrier_on(struct net_device *dev)
{
	if (test_and_clear_bit(__LINK_STATE_NOCARRIER, &dev->state))
		linkwatch_fire_event(dev);
	if (netif_running(dev))
		__netdev_watchdog_up(dev);
}

static inline void netif_carrier_off(struct net_device *dev)
{
	if (!test_and_set_bit(__LINK_STATE_NOCARRIER, &dev->state))
		linkwatch_fire_event(dev);
}

/* Hot-plugging. */
static inline int netif_device_present(struct net_device *dev)
{
	return test_bit(__LINK_STATE_PRESENT, &dev->state);
}

static inline void netif_device_detach(struct net_device *dev)
{
	if (test_and_clear_bit(__LINK_STATE_PRESENT, &dev->state) &&
	    netif_running(dev)) {
		netif_stop_queue(dev);
	}
}

static inline void netif_device_attach(struct net_device *dev)
{
	if (!test_and_set_bit(__LINK_STATE_PRESENT, &dev->state) &&
	    netif_running(dev)) {
		netif_wake_queue(dev);
 		__netdev_watchdog_up(dev);
	}
}

/*
 * Network interface message level settings
 */
#define HAVE_NETIF_MSG 1

enum {
	NETIF_MSG_DRV		= 0x0001,
	NETIF_MSG_PROBE		= 0x0002,
	NETIF_MSG_LINK		= 0x0004,
	NETIF_MSG_TIMER		= 0x0008,
	NETIF_MSG_IFDOWN	= 0x0010,
	NETIF_MSG_IFUP		= 0x0020,
	NETIF_MSG_RX_ERR	= 0x0040,
	NETIF_MSG_TX_ERR	= 0x0080,
	NETIF_MSG_TX_QUEUED	= 0x0100,
	NETIF_MSG_INTR		= 0x0200,
	NETIF_MSG_TX_DONE	= 0x0400,
	NETIF_MSG_RX_STATUS	= 0x0800,
	NETIF_MSG_PKTDATA	= 0x1000,
	NETIF_MSG_HW		= 0x2000,
	NETIF_MSG_WOL		= 0x4000,
};

#define netif_msg_drv(p)	((p)->msg_enable & NETIF_MSG_DRV)
#define netif_msg_probe(p)	((p)->msg_enable & NETIF_MSG_PROBE)
#define netif_msg_link(p)	((p)->msg_enable & NETIF_MSG_LINK)
#define netif_msg_timer(p)	((p)->msg_enable & NETIF_MSG_TIMER)
#define netif_msg_ifdown(p)	((p)->msg_enable & NETIF_MSG_IFDOWN)
#define netif_msg_ifup(p)	((p)->msg_enable & NETIF_MSG_IFUP)
#define netif_msg_rx_err(p)	((p)->msg_enable & NETIF_MSG_RX_ERR)
#define netif_msg_tx_err(p)	((p)->msg_enable & NETIF_MSG_TX_ERR)
#define netif_msg_tx_queued(p)	((p)->msg_enable & NETIF_MSG_TX_QUEUED)
#define netif_msg_intr(p)	((p)->msg_enable & NETIF_MSG_INTR)
#define netif_msg_tx_done(p)	((p)->msg_enable & NETIF_MSG_TX_DONE)
#define netif_msg_rx_status(p)	((p)->msg_enable & NETIF_MSG_RX_STATUS)
#define netif_msg_pktdata(p)	((p)->msg_enable & NETIF_MSG_PKTDATA)
#define netif_msg_hw(p)		((p)->msg_enable & NETIF_MSG_HW)
#define netif_msg_wol(p)	((p)->msg_enable & NETIF_MSG_WOL)

static inline u32 netif_msg_init(int debug_value, int default_msg_enable_bits)
{
	/* use default */
	if (debug_value < 0 || debug_value >= (sizeof(u32) * 8))
		return default_msg_enable_bits;
	if (debug_value == 0)	/* no output */
		return 0;
	/* set low N bits */
	return (1 << debug_value) - 1;
}

/* Schedule rx intr now? */

static inline int netif_rx_schedule_prep(struct net_device *dev)
{
	return netif_running(dev) &&
		!test_and_set_bit(__LINK_STATE_RX_SCHED, &dev->state);
}

/* Add interface to tail of rx poll list. This assumes that _prep has
 * already been called and returned 1.
 */

static inline void __netif_rx_schedule(struct net_device *dev)
{
	unsigned long flags;

	local_irq_save(flags);
	dev_hold(dev);
	list_add_tail(&dev->poll_list, &__get_cpu_var(softnet_data).poll_list);
	if (dev->quota < 0)
		dev->quota += dev->weight;
	else
		dev->quota = dev->weight;
	__raise_softirq_irqoff(NET_RX_SOFTIRQ);
	local_irq_restore(flags);
}

/* Try to reschedule poll. Called by irq handler. */

static inline void netif_rx_schedule(struct net_device *dev)
{
	if (netif_rx_schedule_prep(dev))
		__netif_rx_schedule(dev);
}

/* Try to reschedule poll. Called by dev->poll() after netif_rx_complete().
 * Do not inline this?
 */
static inline int netif_rx_reschedule(struct net_device *dev, int undo)
{
	if (netif_rx_schedule_prep(dev)) {
		unsigned long flags;

		dev->quota += undo;

		local_irq_save(flags);
		list_add_tail(&dev->poll_list, &__get_cpu_var(softnet_data).poll_list);
		__raise_softirq_irqoff(NET_RX_SOFTIRQ);
		local_irq_restore(flags);
		return 1;
	}
	return 0;
}

/* Remove interface from poll list: it must be in the poll list
 * on current cpu. This primitive is called by dev->poll(), when
 * it completes the work. The device cannot be out of poll list at this
 * moment, it is BUG().
 */
static inline void netif_rx_complete(struct net_device *dev)
{
	unsigned long flags;

	local_irq_save(flags);
	BUG_ON(!test_bit(__LINK_STATE_RX_SCHED, &dev->state));
	list_del(&dev->poll_list);
	smp_mb__before_clear_bit();
	clear_bit(__LINK_STATE_RX_SCHED, &dev->state);
	local_irq_restore(flags);
}

static inline void netif_poll_disable(struct net_device *dev)
{
	while (test_and_set_bit(__LINK_STATE_RX_SCHED, &dev->state)) {
		/* No hurry. */
		current->state = TASK_INTERRUPTIBLE;
		schedule_timeout(1);
	}
}

static inline void netif_poll_enable(struct net_device *dev)
{
	clear_bit(__LINK_STATE_RX_SCHED, &dev->state);
}

/* same as netif_rx_complete, except that local_irq_save(flags)
 * has already been issued
 */
static inline void __netif_rx_complete(struct net_device *dev)
{
	BUG_ON(!test_bit(__LINK_STATE_RX_SCHED, &dev->state));
	list_del(&dev->poll_list);
	smp_mb__before_clear_bit();
	clear_bit(__LINK_STATE_RX_SCHED, &dev->state);
}

static inline void netif_tx_disable(struct net_device *dev)
{
	spin_lock_bh(&dev->xmit_lock);
	netif_stop_queue(dev);
	spin_unlock_bh(&dev->xmit_lock);
}

/* These functions live elsewhere (drivers/net/net_init.c, but related) */

extern void		ether_setup(struct net_device *dev);
extern void		fddi_setup(struct net_device *dev);
extern void		tr_setup(struct net_device *dev);
extern void		fc_setup(struct net_device *dev);
extern void		fc_freedev(struct net_device *dev);
/* Support for loadable net-drivers */
extern struct net_device *alloc_netdev(int sizeof_priv, const char *name,
				       void (*setup)(struct net_device *));
extern int		register_netdev(struct net_device *dev);
extern void		unregister_netdev(struct net_device *dev);
/* Functions used for multicast support */
extern void		dev_mc_upload(struct net_device *dev);
extern int 		dev_mc_delete(struct net_device *dev, void *addr, int alen, int all);
extern int		dev_mc_add(struct net_device *dev, void *addr, int alen, int newonly);
extern void		dev_mc_discard(struct net_device *dev);
extern void		dev_set_promiscuity(struct net_device *dev, int inc);
extern void		dev_set_allmulti(struct net_device *dev, int inc);
extern void		netdev_state_change(struct net_device *dev);
/* Load a device via the kmod */
extern void		dev_load(const char *name);
extern void		dev_mcast_init(void);
extern int		netdev_register_fc(struct net_device *dev, void (*stimul)(struct net_device *dev));
extern void		netdev_unregister_fc(int bit);
extern int		netdev_max_backlog;
extern int		weight_p;
extern unsigned long	netdev_fc_xoff;
extern atomic_t netdev_dropping;
extern int		netdev_set_master(struct net_device *dev, struct net_device *master);
extern int skb_checksum_help(struct sk_buff **pskb, int inward);

#ifdef CONFIG_SYSCTL
extern char *net_sysctl_strdup(const char *s);
#endif

#endif /* __KERNEL__ */

#endif	/* _LINUX_DEV_H */
