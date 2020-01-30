/* 8139cp.c: A Linux PCI Ethernet driver for the RealTek 8139C+ chips. */
/*
	Copyright 2001-2004 Jeff Garzik <jgarzik@pobox.com>

	Copyright (C) 2001, 2002 David S. Miller (davem@redhat.com) [tg3.c]
	Copyright (C) 2000, 2001 David S. Miller (davem@redhat.com) [sungem.c]
	Copyright 2001 Manfred Spraul				    [natsemi.c]
	Copyright 1999-2001 by Donald Becker.			    [natsemi.c]
       	Written 1997-2001 by Donald Becker.			    [8139too.c]
	Copyright 1998-2001 by Jes Sorensen, <jes@trained-monkey.org>. [acenic.c]

	This software may be used and distributed according to the terms of
	the GNU General Public License (GPL), incorporated herein by reference.
	Drivers based on or derived from this code fall under the GPL and must
	retain the authorship, copyright and license notice.  This file is not
	a complete program and may only be used when the entire operating
	system is licensed under the GPL.

	See the file COPYING in this distribution for more information.

	Contributors:
	
		Wake-on-LAN support - Felipe Damasio <felipewd@terra.com.br>
		PCI suspend/resume  - Felipe Damasio <felipewd@terra.com.br>
		LinkChg interrupt   - Felipe Damasio <felipewd@terra.com.br>
			
	TODO:
	* Test Tx checksumming thoroughly
	* Implement dev->tx_timeout

	Low priority TODO:
	* Complete reset on PciErr
	* Consider Rx interrupt mitigation using TimerIntr
	* Investigate using skb->priority with h/w VLAN priority
	* Investigate using High Priority Tx Queue with skb->priority
	* Adjust Rx FIFO threshold and Max Rx DMA burst on Rx FIFO error
	* Adjust Tx FIFO threshold and Max Tx DMA burst on Tx FIFO error
	* Implement Tx software interrupt mitigation via
	  Tx descriptor bit
	* The real minimum of CP_MIN_MTU is 4 bytes.  However,
	  for this to be supported, one must(?) turn on packet padding.
	* Support external MII transceivers (patch available)

	NOTES:
	* TX checksumming is considered experimental.  It is off by
	  default, use ethtool to turn it on.

 */

#define DRV_NAME		"8139cp"
#define DRV_VERSION		"1.2"
#define DRV_RELDATE		"Mar 22, 2004"


#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/compiler.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/ethtool.h>
#include <linux/mii.h>
#include <linux/if_vlan.h>
#include <linux/crc32.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/cache.h>
#include <asm/io.h>
#include <asm/uaccess.h>

/* VLAN tagging feature enable/disable */
#if defined(CONFIG_VLAN_8021Q) || defined(CONFIG_VLAN_8021Q_MODULE)
#define CP_VLAN_TAG_USED 1
#define CP_VLAN_TX_TAG(tx_desc,vlan_tag_value) \
	do { (tx_desc)->opts2 = (vlan_tag_value); } while (0)
#else
#define CP_VLAN_TAG_USED 0
#define CP_VLAN_TX_TAG(tx_desc,vlan_tag_value) \
	do { (tx_desc)->opts2 = 0; } while (0)
#endif

/* These identify the driver base version and may not be removed. */
static char version[] =
KERN_INFO DRV_NAME ": 10/100 PCI Ethernet driver v" DRV_VERSION " (" DRV_RELDATE ")\n";

MODULE_AUTHOR("Jeff Garzik <jgarzik@pobox.com>");
MODULE_DESCRIPTION("RealTek RTL-8139C+ series 10/100 PCI Ethernet driver");
MODULE_LICENSE("GPL");

static int debug = -1;
MODULE_PARM (debug, "i");
MODULE_PARM_DESC (debug, "8139cp: bitmapped message enable number");

/* Maximum number of multicast addresses to filter (vs. Rx-all-multicast).
   The RTL chips use a 64 element hash table based on the Ethernet CRC.  */
static int multicast_filter_limit = 32;
MODULE_PARM (multicast_filter_limit, "i");
MODULE_PARM_DESC (multicast_filter_limit, "8139cp: maximum number of filtered multicast addresses");

#define PFX			DRV_NAME ": "

#ifndef TRUE
#define FALSE 0
#define TRUE (!FALSE)
#endif

#define CP_DEF_MSG_ENABLE	(NETIF_MSG_DRV		| \
				 NETIF_MSG_PROBE 	| \
				 NETIF_MSG_LINK)
#define CP_NUM_STATS		14	/* struct cp_dma_stats, plus one */
#define CP_STATS_SIZE		64	/* size in bytes of DMA stats block */
#define CP_REGS_SIZE		(0xff + 1)
#define CP_REGS_VER		1		/* version 1 */
#define CP_RX_RING_SIZE		64
#define CP_TX_RING_SIZE		64
#define CP_RING_BYTES		\
		((sizeof(struct cp_desc) * CP_RX_RING_SIZE) +	\
		 (sizeof(struct cp_desc) * CP_TX_RING_SIZE) +	\
		 CP_STATS_SIZE)
#define NEXT_TX(N)		(((N) + 1) & (CP_TX_RING_SIZE - 1))
#define NEXT_RX(N)		(((N) + 1) & (CP_RX_RING_SIZE - 1))
#define TX_BUFFS_AVAIL(CP)					\
	(((CP)->tx_tail <= (CP)->tx_head) ?			\
	  (CP)->tx_tail + (CP_TX_RING_SIZE - 1) - (CP)->tx_head :	\
	  (CP)->tx_tail - (CP)->tx_head - 1)

#define PKT_BUF_SZ		1536	/* Size of each temporary Rx buffer.*/
#define RX_OFFSET		2
#define CP_INTERNAL_PHY		32

/* The following settings are log_2(bytes)-4:  0 == 16 bytes .. 6==1024, 7==end of packet. */
#define RX_FIFO_THRESH		5	/* Rx buffer level before first PCI xfer.  */
#define RX_DMA_BURST		4	/* Maximum PCI burst, '4' is 256 */
#define TX_DMA_BURST		6	/* Maximum PCI burst, '6' is 1024 */
#define TX_EARLY_THRESH		256	/* Early Tx threshold, in bytes */

/* Time in jiffies before concluding the transmitter is hung. */
#define TX_TIMEOUT		(6*HZ)

/* hardware minimum and maximum for a single frame's data payload */
#define CP_MIN_MTU		60	/* TODO: allow lower, but pad */
#define CP_MAX_MTU		4096

enum {
	/* NIC register offsets */
	MAC0		= 0x00,	/* Ethernet hardware address. */
	MAR0		= 0x08,	/* Multicast filter. */
	StatsAddr	= 0x10,	/* 64-bit start addr of 64-byte DMA stats blk */
	TxRingAddr	= 0x20, /* 64-bit start addr of Tx ring */
	HiTxRingAddr	= 0x28, /* 64-bit start addr of high priority Tx ring */
	Cmd		= 0x37, /* Command register */
	IntrMask	= 0x3C, /* Interrupt mask */
	IntrStatus	= 0x3E, /* Interrupt status */
	TxConfig	= 0x40, /* Tx configuration */
	ChipVersion	= 0x43, /* 8-bit chip version, inside TxConfig */
	RxConfig	= 0x44, /* Rx configuration */
	RxMissed	= 0x4C,	/* 24 bits valid, write clears */
	Cfg9346		= 0x50, /* EEPROM select/control; Cfg reg [un]lock */
	Config1		= 0x52, /* Config1 */
	Config3		= 0x59, /* Config3 */
	Config4		= 0x5A, /* Config4 */
	MultiIntr	= 0x5C, /* Multiple interrupt select */
	BasicModeCtrl	= 0x62,	/* MII BMCR */
	BasicModeStatus	= 0x64, /* MII BMSR */
	NWayAdvert	= 0x66, /* MII ADVERTISE */
	NWayLPAR	= 0x68, /* MII LPA */
	NWayExpansion	= 0x6A, /* MII Expansion */
	Config5		= 0xD8,	/* Config5 */
	TxPoll		= 0xD9,	/* Tell chip to check Tx descriptors for work */
	RxMaxSize	= 0xDA, /* Max size of an Rx packet (8169 only) */
	CpCmd		= 0xE0, /* C+ Command register (C+ mode only) */
	IntrMitigate	= 0xE2,	/* rx/tx interrupt mitigation control */
	RxRingAddr	= 0xE4, /* 64-bit start addr of Rx ring */
	TxThresh	= 0xEC, /* Early Tx threshold */
	OldRxBufAddr	= 0x30, /* DMA address of Rx ring buffer (C mode) */
	OldTSD0		= 0x10, /* DMA address of first Tx desc (C mode) */

	/* Tx and Rx status descriptors */
	DescOwn		= (1 << 31), /* Descriptor is owned by NIC */
	RingEnd		= (1 << 30), /* End of descriptor ring */
	FirstFrag	= (1 << 29), /* First segment of a packet */
	LastFrag	= (1 << 28), /* Final segment of a packet */
	TxError		= (1 << 23), /* Tx error summary */
	RxError		= (1 << 20), /* Rx error summary */
	IPCS		= (1 << 18), /* Calculate IP checksum */
	UDPCS		= (1 << 17), /* Calculate UDP/IP checksum */
	TCPCS		= (1 << 16), /* Calculate TCP/IP checksum */
	TxVlanTag	= (1 << 17), /* Add VLAN tag */
	RxVlanTagged	= (1 << 16), /* Rx VLAN tag available */
	IPFail		= (1 << 15), /* IP checksum failed */
	UDPFail		= (1 << 14), /* UDP/IP checksum failed */
	TCPFail		= (1 << 13), /* TCP/IP checksum failed */
	NormalTxPoll	= (1 << 6),  /* One or more normal Tx packets to send */
	PID1		= (1 << 17), /* 2 protocol id bits:  0==non-IP, */
	PID0		= (1 << 16), /* 1==UDP/IP, 2==TCP/IP, 3==IP */
	RxProtoTCP	= 1,
	RxProtoUDP	= 2,
	RxProtoIP	= 3,
	TxFIFOUnder	= (1 << 25), /* Tx FIFO underrun */
	TxOWC		= (1 << 22), /* Tx Out-of-window collision */
	TxLinkFail	= (1 << 21), /* Link failed during Tx of packet */
	TxMaxCol	= (1 << 20), /* Tx aborted due to excessive collisions */
	TxColCntShift	= 16,	     /* Shift, to get 4-bit Tx collision cnt */
	TxColCntMask	= 0x01 | 0x02 | 0x04 | 0x08, /* 4-bit collision count */
	RxErrFrame	= (1 << 27), /* Rx frame alignment error */
	RxMcast		= (1 << 26), /* Rx multicast packet rcv'd */
	RxErrCRC	= (1 << 18), /* Rx CRC error */
	RxErrRunt	= (1 << 19), /* Rx error, packet < 64 bytes */
	RxErrLong	= (1 << 21), /* Rx error, packet > 4096 bytes */
	RxErrFIFO	= (1 << 22), /* Rx error, FIFO overflowed, pkt bad */

	/* StatsAddr register */
	DumpStats	= (1 << 3),  /* Begin stats dump */

	/* RxConfig register */
	RxCfgFIFOShift	= 13,	     /* Shift, to get Rx FIFO thresh value */
	RxCfgDMAShift	= 8,	     /* Shift, to get Rx Max DMA value */
	AcceptErr	= 0x20,	     /* Accept packets with CRC errors */
	AcceptRunt	= 0x10,	     /* Accept runt (<64 bytes) packets */
	AcceptBroadcast	= 0x08,	     /* Accept broadcast packets */
	AcceptMulticast	= 0x04,	     /* Accept multicast packets */
	AcceptMyPhys	= 0x02,	     /* Accept pkts with our MAC as dest */
	AcceptAllPhys	= 0x01,	     /* Accept all pkts w/ physical dest */

	/* IntrMask / IntrStatus registers */
	PciErr		= (1 << 15), /* System error on the PCI bus */
	TimerIntr	= (1 << 14), /* Asserted when TCTR reaches TimerInt value */
	LenChg		= (1 << 13), /* Cable length change */
	SWInt		= (1 << 8),  /* Software-requested interrupt */
	TxEmpty		= (1 << 7),  /* No Tx descriptors available */
	RxFIFOOvr	= (1 << 6),  /* Rx FIFO Overflow */
	LinkChg		= (1 << 5),  /* Packet underrun, or link change */
	RxEmpty		= (1 << 4),  /* No Rx descriptors available */
	TxErr		= (1 << 3),  /* Tx error */
	TxOK		= (1 << 2),  /* Tx packet sent */
	RxErr		= (1 << 1),  /* Rx error */
	RxOK		= (1 << 0),  /* Rx packet received */
	IntrResvd	= (1 << 10), /* reserved, according to RealTek engineers,
					but hardware likes to raise it */

	IntrAll		= PciErr | TimerIntr | LenChg | SWInt | TxEmpty |
			  RxFIFOOvr | LinkChg | RxEmpty | TxErr | TxOK |
			  RxErr | RxOK | IntrResvd,

	/* C mode command register */
	CmdReset	= (1 << 4),  /* Enable to reset; self-clearing */
	RxOn		= (1 << 3),  /* Rx mode enable */
	TxOn		= (1 << 2),  /* Tx mode enable */

	/* C+ mode command register */
	RxVlanOn	= (1 << 6),  /* Rx VLAN de-tagging enable */
	RxChkSum	= (1 << 5),  /* Rx checksum offload enable */
	PCIDAC		= (1 << 4),  /* PCI Dual Address Cycle (64-bit PCI) */
	PCIMulRW	= (1 << 3),  /* Enable PCI read/write multiple */
	CpRxOn		= (1 << 1),  /* Rx mode enable */
	CpTxOn		= (1 << 0),  /* Tx mode enable */

	/* Cfg9436 EEPROM control register */
	Cfg9346_Lock	= 0x00,	     /* Lock ConfigX/MII register access */
	Cfg9346_Unlock	= 0xC0,	     /* Unlock ConfigX/MII register access */

	/* TxConfig register */
	IFG		= (1 << 25) | (1 << 24), /* standard IEEE interframe gap */
	TxDMAShift	= 8,	     /* DMA burst value (0-7) is shift this many bits */

	/* Early Tx Threshold register */
	TxThreshMask	= 0x3f,	     /* Mask bits 5-0 */
	TxThreshMax	= 2048,	     /* Max early Tx threshold */

	/* Config1 register */
	DriverLoaded	= (1 << 5),  /* Software marker, driver is loaded */
	LWACT           = (1 << 4),  /* LWAKE active mode */
	PMEnable	= (1 << 0),  /* Enable various PM features of chip */

	/* Config3 register */
	PARMEnable	= (1 << 6),  /* Enable auto-loading of PHY parms */
	MagicPacket     = (1 << 5),  /* Wake up when receives a Magic Packet */
	LinkUp          = (1 << 4),  /* Wake up when the cable connection is re-established */

	/* Config4 register */
	LWPTN           = (1 << 1),  /* LWAKE Pattern */
	LWPME           = (1 << 4),  /* LANWAKE vs PMEB */

	/* Config5 register */
	BWF             = (1 << 6),  /* Accept Broadcast wakeup frame */
	MWF             = (1 << 5),  /* Accept Multicast wakeup frame */
	UWF             = (1 << 4),  /* Accept Unicast wakeup frame */
	LANWake         = (1 << 1),  /* Enable LANWake signal */
	PMEStatus	= (1 << 0),  /* PME status can be reset by PCI RST# */

	cp_norx_intr_mask = PciErr | LinkChg | TxOK | TxErr | TxEmpty,
	cp_rx_intr_mask = RxOK | RxErr | RxEmpty | RxFIFOOvr,
	cp_intr_mask = cp_rx_intr_mask | cp_norx_intr_mask,
};

static const unsigned int cp_rx_config =
	  (RX_FIFO_THRESH << RxCfgFIFOShift) |
	  (RX_DMA_BURST << RxCfgDMAShift);

struct cp_desc {
	u32		opts1;
	u32		opts2;
	u64		addr;
};

struct ring_info {
	struct sk_buff		*skb;
	dma_addr_t		mapping;
	unsigned		frag;
};

struct cp_dma_stats {
	u64			tx_ok;
	u64			rx_ok;
	u64			tx_err;
	u32			rx_err;
	u16			rx_fifo;
	u16			frame_align;
	u32			tx_ok_1col;
	u32			tx_ok_mcol;
	u64			rx_ok_phys;
	u64			rx_ok_bcast;
	u32			rx_ok_mcast;
	u16			tx_abort;
	u16			tx_underrun;
} __attribute__((packed));

struct cp_extra_stats {
	unsigned long		rx_frags;
};

struct cp_private {
	void			*regs;
	struct net_device	*dev;
	spinlock_t		lock;
	u32			msg_enable;

	struct pci_dev		*pdev;
	u32			rx_config;
	u16			cpcmd;

	struct net_device_stats net_stats;
	struct cp_extra_stats	cp_stats;
	struct cp_dma_stats	*nic_stats;
	dma_addr_t		nic_stats_dma;

	unsigned		rx_tail		____cacheline_aligned;
	struct cp_desc		*rx_ring;
	struct ring_info	rx_skb[CP_RX_RING_SIZE];
	unsigned		rx_buf_sz;

	unsigned		tx_head		____cacheline_aligned;
	unsigned		tx_tail;

	struct cp_desc		*tx_ring;
	struct ring_info	tx_skb[CP_TX_RING_SIZE];
	dma_addr_t		ring_dma;

#if CP_VLAN_TAG_USED
	struct vlan_group	*vlgrp;
#endif

	unsigned int		wol_enabled : 1; /* Is Wake-on-LAN enabled? */
	u32			power_state[16];

	struct mii_if_info	mii_if;
};

#define cpr8(reg)	readb(cp->regs + (reg))
#define cpr16(reg)	readw(cp->regs + (reg))
#define cpr32(reg)	readl(cp->regs + (reg))
#define cpw8(reg,val)	writeb((val), cp->regs + (reg))
#define cpw16(reg,val)	writew((val), cp->regs + (reg))
#define cpw32(reg,val)	writel((val), cp->regs + (reg))
#define cpw8_f(reg,val) do {			\
	writeb((val), cp->regs + (reg));	\
	readb(cp->regs + (reg));		\
	} while (0)
#define cpw16_f(reg,val) do {			\
	writew((val), cp->regs + (reg));	\
	readw(cp->regs + (reg));		\
	} while (0)
#define cpw32_f(reg,val) do {			\
	writel((val), cp->regs + (reg));	\
	readl(cp->regs + (reg));		\
	} while (0)


static void __cp_set_rx_mode (struct net_device *dev);
static void cp_tx (struct cp_private *cp);
static void cp_clean_rings (struct cp_private *cp);

static struct pci_device_id cp_pci_tbl[] = {
	{ PCI_VENDOR_ID_REALTEK, PCI_DEVICE_ID_REALTEK_8139,
	  PCI_ANY_ID, PCI_ANY_ID, 0, 0, },
	{ },
};
MODULE_DEVICE_TABLE(pci, cp_pci_tbl);

static struct {
	const char str[ETH_GSTRING_LEN];
} ethtool_stats_keys[] = {
	{ "tx_ok" },
	{ "rx_ok" },
	{ "tx_err" },
	{ "rx_err" },
	{ "rx_fifo" },
	{ "frame_align" },
	{ "tx_ok_1col" },
	{ "tx_ok_mcol" },
	{ "rx_ok_phys" },
	{ "rx_ok_bcast" },
	{ "rx_ok_mcast" },
	{ "tx_abort" },
	{ "tx_underrun" },
	{ "rx_frags" },
};


#if CP_VLAN_TAG_USED
static void cp_vlan_rx_register(struct net_device *dev, struct vlan_group *grp)
{
	struct cp_private *cp = netdev_priv(dev);
	unsigned long flags;

	spin_lock_irqsave(&cp->lock, flags);
	cp->vlgrp = grp;
	cp->cpcmd |= RxVlanOn;
	cpw16(CpCmd, cp->cpcmd);
	spin_unlock_irqrestore(&cp->lock, flags);
}

static void cp_vlan_rx_kill_vid(struct net_device *dev, unsigned short vid)
{
	struct cp_private *cp = netdev_priv(dev);
	unsigned long flags;

	spin_lock_irqsave(&cp->lock, flags);
	cp->cpcmd &= ~RxVlanOn;
	cpw16(CpCmd, cp->cpcmd);
	if (cp->vlgrp)
		cp->vlgrp->vlan_devices[vid] = NULL;
	spin_unlock_irqrestore(&cp->lock, flags);
}
#endif /* CP_VLAN_TAG_USED */

static inline void cp_set_rxbufsize (struct cp_private *cp)
{
	unsigned int mtu = cp->dev->mtu;
	
	if (mtu > ETH_DATA_LEN)
		/* MTU + ethernet header + FCS + optional VLAN tag */
		cp->rx_buf_sz = mtu + ETH_HLEN + 8;
	else
		cp->rx_buf_sz = PKT_BUF_SZ;
}

static inline void cp_rx_skb (struct cp_private *cp, struct sk_buff *skb,
			      struct cp_desc *desc)
{
	skb->protocol = eth_type_trans (skb, cp->dev);

	cp->net_stats.rx_packets++;
	cp->net_stats.rx_bytes += skb->len;
	cp->dev->last_rx = jiffies;

#if CP_VLAN_TAG_USED
	if (cp->vlgrp && (desc->opts2 & RxVlanTagged)) {
		vlan_hwaccel_receive_skb(skb, cp->vlgrp,
					 be16_to_cpu(desc->opts2 & 0xffff));
	} else
#endif
		netif_receive_skb(skb);
}

static void cp_rx_err_acct (struct cp_private *cp, unsigned rx_tail,
			    u32 status, u32 len)
{
	if (netif_msg_rx_err (cp))
		printk (KERN_DEBUG
			"%s: rx err, slot %d status 0x%x len %d\n",
			cp->dev->name, rx_tail, status, len);
	cp->net_stats.rx_errors++;
	if (status & RxErrFrame)
		cp->net_stats.rx_frame_errors++;
	if (status & RxErrCRC)
		cp->net_stats.rx_crc_errors++;
	if ((status & RxErrRunt) || (status & RxErrLong))
		cp->net_stats.rx_length_errors++;
	if ((status & (FirstFrag | LastFrag)) != (FirstFrag | LastFrag))
		cp->net_stats.rx_length_errors++;
	if (status & RxErrFIFO)
		cp->net_stats.rx_fifo_errors++;
}

static inline unsigned int cp_rx_csum_ok (u32 status)
{
	unsigned int protocol = (status >> 16) & 0x3;
	
	if (likely((protocol == RxProtoTCP) && (!(status & TCPFail))))
		return 1;
	else if ((protocol == RxProtoUDP) && (!(status & UDPFail)))
		return 1;
	else if ((protocol == RxProtoIP) && (!(status & IPFail)))
		return 1;
	return 0;
}

static int cp_rx_poll (struct net_device *dev, int *budget)
{
	struct cp_private *cp = netdev_priv(dev);
	unsigned rx_tail = cp->rx_tail;
	unsigned rx_work = dev->quota;
	unsigned rx;

rx_status_loop:
	rx = 0;
	cpw16(IntrStatus, cp_rx_intr_mask);

	while (1) {
		u32 status, len;
		dma_addr_t mapping;
		struct sk_buff *skb, *new_skb;
		struct cp_desc *desc;
		unsigned buflen;

		skb = cp->rx_skb[rx_tail].skb;
		if (!skb)
			BUG();

		desc = &cp->rx_ring[rx_tail];
		status = le32_to_cpu(desc->opts1);
		if (status & DescOwn)
			break;

		len = (status & 0x1fff) - 4;
		mapping = cp->rx_skb[rx_tail].mapping;

		if ((status & (FirstFrag | LastFrag)) != (FirstFrag | LastFrag)) {
			/* we don't support incoming fragmented frames.
			 * instead, we attempt to ensure that the
			 * pre-allocated RX skbs are properly sized such
			 * that RX fragments are never encountered
			 */
			cp_rx_err_acct(cp, rx_tail, status, len);
			cp->net_stats.rx_dropped++;
			cp->cp_stats.rx_frags++;
			goto rx_next;
		}

		if (status & (RxError | RxErrFIFO)) {
			cp_rx_err_acct(cp, rx_tail, status, len);
			goto rx_next;
		}

		if (netif_msg_rx_status(cp))
			printk(KERN_DEBUG "%s: rx slot %d status 0x%x len %d\n",
			       cp->dev->name, rx_tail, status, len);

		buflen = cp->rx_buf_sz + RX_OFFSET;
		new_skb = dev_alloc_skb (buflen);
		if (!new_skb) {
			cp->net_stats.rx_dropped++;
			goto rx_next;
		}

		skb_reserve(new_skb, RX_OFFSET);
		new_skb->dev = cp->dev;

		pci_unmap_single(cp->pdev, mapping,
				 buflen, PCI_DMA_FROMDEVICE);

		/* Handle checksum offloading for incoming packets. */
		if (cp_rx_csum_ok(status))
			skb->ip_summed = CHECKSUM_UNNECESSARY;
		else
			skb->ip_summed = CHECKSUM_NONE;

		skb_put(skb, len);

		mapping =
		cp->rx_skb[rx_tail].mapping =
			pci_map_single(cp->pdev, new_skb->tail,
				       buflen, PCI_DMA_FROMDEVICE);
		cp->rx_skb[rx_tail].skb = new_skb;

		cp_rx_skb(cp, skb, desc);
		rx++;

rx_next:
		cp->rx_ring[rx_tail].opts2 = 0;
		cp->rx_ring[rx_tail].addr = cpu_to_le64(mapping);
		if (rx_tail == (CP_RX_RING_SIZE - 1))
			desc->opts1 = cpu_to_le32(DescOwn | RingEnd |
						  cp->rx_buf_sz);
		else
			desc->opts1 = cpu_to_le32(DescOwn | cp->rx_buf_sz);
		rx_tail = NEXT_RX(rx_tail);

		if (!rx_work--)
			break;
	}

	cp->rx_tail = rx_tail;

	dev->quota -= rx;
	*budget -= rx;

	/* if we did not reach work limit, then we're done with
	 * this round of polling
	 */
	if (rx_work) {
		if (cpr16(IntrStatus) & cp_rx_intr_mask)
			goto rx_status_loop;

		local_irq_disable();
		cpw16_f(IntrMask, cp_intr_mask);
		__netif_rx_complete(dev);
		local_irq_enable();

		return 0;	/* done */
	}

	return 1;		/* not done */
}

static irqreturn_t
cp_interrupt (int irq, void *dev_instance, struct pt_regs *regs)
{
	struct net_device *dev = dev_instance;
	struct cp_private *cp;
	u16 status;

	if (unlikely(dev == NULL))
		return IRQ_NONE;
	cp = netdev_priv(dev);

	status = cpr16(IntrStatus);
	if (!status || (status == 0xFFFF))
		return IRQ_NONE;

	if (netif_msg_intr(cp))
		printk(KERN_DEBUG "%s: intr, status %04x cmd %02x cpcmd %04x\n",
		        dev->name, status, cpr8(Cmd), cpr16(CpCmd));

	cpw16(IntrStatus, status & ~cp_rx_intr_mask);

	spin_lock(&cp->lock);

	/* close possible race's with dev_close */
	if (unlikely(!netif_running(dev))) {
		cpw16(IntrMask, 0);
		spin_unlock(&cp->lock);
		return IRQ_HANDLED;
	}

	if (status & (RxOK | RxErr | RxEmpty | RxFIFOOvr))
		if (netif_rx_schedule_prep(dev)) {
			cpw16_f(IntrMask, cp_norx_intr_mask);
			__netif_rx_schedule(dev);
		}

	if (status & (TxOK | TxErr | TxEmpty | SWInt))
		cp_tx(cp);
	if (status & LinkChg)
		mii_check_media(&cp->mii_if, netif_msg_link(cp), FALSE);

	spin_unlock(&cp->lock);

	if (status & PciErr) {
		u16 pci_status;

		pci_read_config_word(cp->pdev, PCI_STATUS, &pci_status);
		pci_write_config_word(cp->pdev, PCI_STATUS, pci_status);
		printk(KERN_ERR "%s: PCI bus error, status=%04x, PCI status=%04x\n",
		       dev->name, status, pci_status);

		/* TODO: reset hardware */
	}

	return IRQ_HANDLED;
}

static void cp_tx (struct cp_private *cp)
{
	unsigned tx_head = cp->tx_head;
	unsigned tx_tail = cp->tx_tail;

	while (tx_tail != tx_head) {
		struct sk_buff *skb;
		u32 status;

		rmb();
		status = le32_to_cpu(cp->tx_ring[tx_tail].opts1);
		if (status & DescOwn)
			break;

		skb = cp->tx_skb[tx_tail].skb;
		if (!skb)
			BUG();

		pci_unmap_single(cp->pdev, cp->tx_skb[tx_tail].mapping,
					skb->len, PCI_DMA_TODEVICE);

		if (status & LastFrag) {
			if (status & (TxError | TxFIFOUnder)) {
				if (netif_msg_tx_err(cp))
					printk(KERN_DEBUG "%s: tx err, status 0x%x\n",
					       cp->dev->name, status);
				cp->net_stats.tx_errors++;
				if (status & TxOWC)
					cp->net_stats.tx_window_errors++;
				if (status & TxMaxCol)
					cp->net_stats.tx_aborted_errors++;
				if (status & TxLinkFail)
					cp->net_stats.tx_carrier_errors++;
				if (status & TxFIFOUnder)
					cp->net_stats.tx_fifo_errors++;
			} else {
				cp->net_stats.collisions +=
					((status >> TxColCntShift) & TxColCntMask);
				cp->net_stats.tx_packets++;
				cp->net_stats.tx_bytes += skb->len;
				if (netif_msg_tx_done(cp))
					printk(KERN_DEBUG "%s: tx done, slot %d\n", cp->dev->name, tx_tail);
			}
			dev_kfree_skb_irq(skb);
		}

		cp->tx_skb[tx_tail].skb = NULL;

		tx_tail = NEXT_TX(tx_tail);
	}

	cp->tx_tail = tx_tail;

	if (TX_BUFFS_AVAIL(cp) > (MAX_SKB_FRAGS + 1))
		netif_wake_queue(cp->dev);
}

static int cp_start_xmit (struct sk_buff *skb, struct net_device *dev)
{
	struct cp_private *cp = netdev_priv(dev);
	unsigned entry;
	u32 eor;
#if CP_VLAN_TAG_USED
	u32 vlan_tag = 0;
#endif

	spin_lock_irq(&cp->lock);

	/* This is a hard error, log it. */
	if (TX_BUFFS_AVAIL(cp) <= (skb_shinfo(skb)->nr_frags + 1)) {
		netif_stop_queue(dev);
		spin_unlock_irq(&cp->lock);
		printk(KERN_ERR PFX "%s: BUG! Tx Ring full when queue awake!\n",
		       dev->name);
		return 1;
	}

#if CP_VLAN_TAG_USED
	if (cp->vlgrp && vlan_tx_tag_present(skb))
		vlan_tag = TxVlanTag | cpu_to_be16(vlan_tx_tag_get(skb));
#endif

	entry = cp->tx_head;
	eor = (entry == (CP_TX_RING_SIZE - 1)) ? RingEnd : 0;
	if (skb_shinfo(skb)->nr_frags == 0) {
		struct cp_desc *txd = &cp->tx_ring[entry];
		u32 len;
		dma_addr_t mapping;

		len = skb->len;
		mapping = pci_map_single(cp->pdev, skb->data, len, PCI_DMA_TODEVICE);
		CP_VLAN_TX_TAG(txd, vlan_tag);
		txd->addr = cpu_to_le64(mapping);
		wmb();

		if (skb->ip_summed == CHECKSUM_HW) {
			const struct iphdr *ip = skb->nh.iph;
			if (ip->protocol == IPPROTO_TCP)
				txd->opts1 = cpu_to_le32(eor | len | DescOwn |
							 FirstFrag | LastFrag |
							 IPCS | TCPCS);
			else if (ip->protocol == IPPROTO_UDP)
				txd->opts1 = cpu_to_le32(eor | len | DescOwn |
							 FirstFrag | LastFrag |
							 IPCS | UDPCS);
			else
				BUG();
		} else
			txd->opts1 = cpu_to_le32(eor | len | DescOwn |
						 FirstFrag | LastFrag);
		wmb();

		cp->tx_skb[entry].skb = skb;
		cp->tx_skb[entry].mapping = mapping;
		cp->tx_skb[entry].frag = 0;
		entry = NEXT_TX(entry);
	} else {
		struct cp_desc *txd;
		u32 first_len, first_eor;
		dma_addr_t first_mapping;
		int frag, first_entry = entry;
		const struct iphdr *ip = skb->nh.iph;

		/* We must give this initial chunk to the device last.
		 * Otherwise we could race with the device.
		 */
		first_eor = eor;
		first_len = skb_headlen(skb);
		first_mapping = pci_map_single(cp->pdev, skb->data,
					       first_len, PCI_DMA_TODEVICE);
		cp->tx_skb[entry].skb = skb;
		cp->tx_skb[entry].mapping = first_mapping;
		cp->tx_skb[entry].frag = 1;
		entry = NEXT_TX(entry);

		for (frag = 0; frag < skb_shinfo(skb)->nr_frags; frag++) {
			skb_frag_t *this_frag = &skb_shinfo(skb)->frags[frag];
			u32 len;
			u32 ctrl;
			dma_addr_t mapping;

			len = this_frag->size;
			mapping = pci_map_single(cp->pdev,
						 ((void *) page_address(this_frag->page) +
						  this_frag->page_offset),
						 len, PCI_DMA_TODEVICE);
			eor = (entry == (CP_TX_RING_SIZE - 1)) ? RingEnd : 0;

			if (skb->ip_summed == CHECKSUM_HW) {
				ctrl = eor | len | DescOwn | IPCS;
				if (ip->protocol == IPPROTO_TCP)
					ctrl |= TCPCS;
				else if (ip->protocol == IPPROTO_UDP)
					ctrl |= UDPCS;
				else
					BUG();
			} else
				ctrl = eor | len | DescOwn;

			if (frag == skb_shinfo(skb)->nr_frags - 1)
				ctrl |= LastFrag;

			txd = &cp->tx_ring[entry];
			CP_VLAN_TX_TAG(txd, vlan_tag);
			txd->addr = cpu_to_le64(mapping);
			wmb();

			txd->opts1 = cpu_to_le32(ctrl);
			wmb();

			cp->tx_skb[entry].skb = skb;
			cp->tx_skb[entry].mapping = mapping;
			cp->tx_skb[entry].frag = frag + 2;
			entry = NEXT_TX(entry);
		}

		txd = &cp->tx_ring[first_entry];
		CP_VLAN_TX_TAG(txd, vlan_tag);
		txd->addr = cpu_to_le64(first_mapping);
		wmb();

		if (skb->ip_summed == CHECKSUM_HW) {
			if (ip->protocol == IPPROTO_TCP)
				txd->opts1 = cpu_to_le32(first_eor | first_len |
							 FirstFrag | DescOwn |
							 IPCS | TCPCS);
			else if (ip->protocol == IPPROTO_UDP)
				txd->opts1 = cpu_to_le32(first_eor | first_len |
							 FirstFrag | DescOwn |
							 IPCS | UDPCS);
			else
				BUG();
		} else
			txd->opts1 = cpu_to_le32(first_eor | first_len |
						 FirstFrag | DescOwn);
		wmb();
	}
	cp->tx_head = entry;
	if (netif_msg_tx_queued(cp))
		printk(KERN_DEBUG "%s: tx queued, slot %d, skblen %d\n",
		       dev->name, entry, skb->len);
	if (TX_BUFFS_AVAIL(cp) <= (MAX_SKB_FRAGS + 1))
		netif_stop_queue(dev);

	spin_unlock_irq(&cp->lock);

	cpw8(TxPoll, NormalTxPoll);
	dev->trans_start = jiffies;

	return 0;
}

/* Set or clear the multicast filter for this adaptor.
   This routine is not state sensitive and need not be SMP locked. */

static void __cp_set_rx_mode (struct net_device *dev)
{
	struct cp_private *cp = netdev_priv(dev);
	u32 mc_filter[2];	/* Multicast hash filter */
	int i, rx_mode;
	u32 tmp;

	/* Note: do not reorder, GCC is clever about common statements. */
	if (dev->flags & IFF_PROMISC) {
		/* Unconditionally log net taps. */
		printk (KERN_NOTICE "%s: Promiscuous mode enabled.\n",
			dev->name);
		rx_mode =
		    AcceptBroadcast | AcceptMulticast | AcceptMyPhys |
		    AcceptAllPhys;
		mc_filter[1] = mc_filter[0] = 0xffffffff;
	} else if ((dev->mc_count > multicast_filter_limit)
		   || (dev->flags & IFF_ALLMULTI)) {
		/* Too many to filter perfectly -- accept all multicasts. */
		rx_mode = AcceptBroadcast | AcceptMulticast | AcceptMyPhys;
		mc_filter[1] = mc_filter[0] = 0xffffffff;
	} else {
		struct dev_mc_list *mclist;
		rx_mode = AcceptBroadcast | AcceptMyPhys;
		mc_filter[1] = mc_filter[0] = 0;
		for (i = 0, mclist = dev->mc_list; mclist && i < dev->mc_count;
		     i++, mclist = mclist->next) {
			int bit_nr = ether_crc(ETH_ALEN, mclist->dmi_addr) >> 26;

			mc_filter[bit_nr >> 5] |= 1 << (bit_nr & 31);
			rx_mode |= AcceptMulticast;
		}
	}

	/* We can safely update without stopping the chip. */
	tmp = cp_rx_config | rx_mode;
	if (cp->rx_config != tmp) {
		cpw32_f (RxConfig, tmp);
		cp->rx_config = tmp;
	}
	cpw32_f (MAR0 + 0, mc_filter[0]);
	cpw32_f (MAR0 + 4, mc_filter[1]);
}

static void cp_set_rx_mode (struct net_device *dev)
{
	unsigned long flags;
	struct cp_private *cp = netdev_priv(dev);

	spin_lock_irqsave (&cp->lock, flags);
	__cp_set_rx_mode(dev);
	spin_unlock_irqrestore (&cp->lock, flags);
}

static void __cp_get_stats(struct cp_private *cp)
{
	/* only lower 24 bits valid; write any value to clear */
	cp->net_stats.rx_missed_errors += (cpr32 (RxMissed) & 0xffffff);
	cpw32 (RxMissed, 0);
}

static struct net_device_stats *cp_get_stats(struct net_device *dev)
{
	struct cp_private *cp = netdev_priv(dev);
	unsigned long flags;

	/* The chip only need report frame silently dropped. */
	spin_lock_irqsave(&cp->lock, flags);
 	if (netif_running(dev) && netif_device_present(dev))
 		__cp_get_stats(cp);
	spin_unlock_irqrestore(&cp->lock, flags);

	return &cp->net_stats;
}

static void cp_stop_hw (struct cp_private *cp)
{
	cpw16(IntrStatus, ~(cpr16(IntrStatus)));
	cpw16_f(IntrMask, 0);
	cpw8(Cmd, 0);
	cpw16_f(CpCmd, 0);
	cpw16_f(IntrStatus, ~(cpr16(IntrStatus)));

	cp->rx_tail = 0;
	cp->tx_head = cp->tx_tail = 0;
}

static void cp_reset_hw (struct cp_private *cp)
{
	unsigned work = 1000;

	cpw8(Cmd, CmdReset);

	while (work--) {
		if (!(cpr8(Cmd) & CmdReset))
			return;

		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(10);
	}

	printk(KERN_ERR "%s: hardware reset timeout\n", cp->dev->name);
}

static inline void cp_start_hw (struct cp_private *cp)
{
	cpw16(CpCmd, cp->cpcmd);
	cpw8(Cmd, RxOn | TxOn);
}

static void cp_init_hw (struct cp_private *cp)
{
	struct net_device *dev = cp->dev;
	dma_addr_t ring_dma;

	cp_reset_hw(cp);

	cpw8_f (Cfg9346, Cfg9346_Unlock);

	/* Restore our idea of the MAC address. */
	cpw32_f (MAC0 + 0, cpu_to_le32 (*(u32 *) (dev->dev_addr + 0)));
	cpw32_f (MAC0 + 4, cpu_to_le32 (*(u32 *) (dev->dev_addr + 4)));

	cp_start_hw(cp);
	cpw8(TxThresh, 0x06); /* XXX convert magic num to a constant */

	__cp_set_rx_mode(dev);
	cpw32_f (TxConfig, IFG | (TX_DMA_BURST << TxDMAShift));

	cpw8(Config1, cpr8(Config1) | DriverLoaded | PMEnable);
	/* Disable Wake-on-LAN. Can be turned on with ETHTOOL_SWOL */
	cpw8(Config3, PARMEnable);
	cp->wol_enabled = 0;

	cpw8(Config5, cpr8(Config5) & PMEStatus); 

	cpw32_f(HiTxRingAddr, 0);
	cpw32_f(HiTxRingAddr + 4, 0);

	ring_dma = cp->ring_dma;
	cpw32_f(RxRingAddr, ring_dma & 0xffffffff);
	cpw32_f(RxRingAddr + 4, (ring_dma >> 16) >> 16);

	ring_dma += sizeof(struct cp_desc) * CP_RX_RING_SIZE;
	cpw32_f(TxRingAddr, ring_dma & 0xffffffff);
	cpw32_f(TxRingAddr + 4, (ring_dma >> 16) >> 16);

	cpw16(MultiIntr, 0);

	cpw16_f(IntrMask, cp_intr_mask);

	cpw8_f(Cfg9346, Cfg9346_Lock);
}

static int cp_refill_rx (struct cp_private *cp)
{
	unsigned i;

	for (i = 0; i < CP_RX_RING_SIZE; i++) {
		struct sk_buff *skb;

		skb = dev_alloc_skb(cp->rx_buf_sz + RX_OFFSET);
		if (!skb)
			goto err_out;

		skb->dev = cp->dev;
		skb_reserve(skb, RX_OFFSET);

		cp->rx_skb[i].mapping = pci_map_single(cp->pdev,
			skb->tail, cp->rx_buf_sz, PCI_DMA_FROMDEVICE);
		cp->rx_skb[i].skb = skb;
		cp->rx_skb[i].frag = 0;

		cp->rx_ring[i].opts2 = 0;
		cp->rx_ring[i].addr = cpu_to_le64(cp->rx_skb[i].mapping);
		if (i == (CP_RX_RING_SIZE - 1))
			cp->rx_ring[i].opts1 =
				cpu_to_le32(DescOwn | RingEnd | cp->rx_buf_sz);
		else
			cp->rx_ring[i].opts1 =
				cpu_to_le32(DescOwn | cp->rx_buf_sz);
	}

	return 0;

err_out:
	cp_clean_rings(cp);
	return -ENOMEM;
}

static int cp_init_rings (struct cp_private *cp)
{
	memset(cp->tx_ring, 0, sizeof(struct cp_desc) * CP_TX_RING_SIZE);
	cp->tx_ring[CP_TX_RING_SIZE - 1].opts1 = cpu_to_le32(RingEnd);

	cp->rx_tail = 0;
	cp->tx_head = cp->tx_tail = 0;

	return cp_refill_rx (cp);
}

static int cp_alloc_rings (struct cp_private *cp)
{
	void *mem;

	mem = pci_alloc_consistent(cp->pdev, CP_RING_BYTES, &cp->ring_dma);
	if (!mem)
		return -ENOMEM;

	cp->rx_ring = mem;
	cp->tx_ring = &cp->rx_ring[CP_RX_RING_SIZE];

	mem += (CP_RING_BYTES - CP_STATS_SIZE);
	cp->nic_stats = mem;
	cp->nic_stats_dma = cp->ring_dma + (CP_RING_BYTES - CP_STATS_SIZE);

	return cp_init_rings(cp);
}

static void cp_clean_rings (struct cp_private *cp)
{
	unsigned i;

	memset(cp->rx_ring, 0, sizeof(struct cp_desc) * CP_RX_RING_SIZE);
	memset(cp->tx_ring, 0, sizeof(struct cp_desc) * CP_TX_RING_SIZE);

	for (i = 0; i < CP_RX_RING_SIZE; i++) {
		if (cp->rx_skb[i].skb) {
			pci_unmap_single(cp->pdev, cp->rx_skb[i].mapping,
					 cp->rx_buf_sz, PCI_DMA_FROMDEVICE);
			dev_kfree_skb(cp->rx_skb[i].skb);
		}
	}

	for (i = 0; i < CP_TX_RING_SIZE; i++) {
		if (cp->tx_skb[i].skb) {
			struct sk_buff *skb = cp->tx_skb[i].skb;
			pci_unmap_single(cp->pdev, cp->tx_skb[i].mapping,
					 skb->len, PCI_DMA_TODEVICE);
			dev_kfree_skb(skb);
			cp->net_stats.tx_dropped++;
		}
	}

	memset(&cp->rx_skb, 0, sizeof(struct ring_info) * CP_RX_RING_SIZE);
	memset(&cp->tx_skb, 0, sizeof(struct ring_info) * CP_TX_RING_SIZE);
}

static void cp_free_rings (struct cp_private *cp)
{
	cp_clean_rings(cp);
	pci_free_consistent(cp->pdev, CP_RING_BYTES, cp->rx_ring, cp->ring_dma);
	cp->rx_ring = NULL;
	cp->tx_ring = NULL;
	cp->nic_stats = NULL;
}

static int cp_open (struct net_device *dev)
{
	struct cp_private *cp = netdev_priv(dev);
	int rc;

	if (netif_msg_ifup(cp))
		printk(KERN_DEBUG "%s: enabling interface\n", dev->name);

	rc = cp_alloc_rings(cp);
	if (rc)
		return rc;

	cp_init_hw(cp);

	rc = request_irq(dev->irq, cp_interrupt, SA_SHIRQ, dev->name, dev);
	if (rc)
		goto err_out_hw;

	netif_carrier_off(dev);
	mii_check_media(&cp->mii_if, netif_msg_link(cp), TRUE);
	netif_start_queue(dev);

	return 0;

err_out_hw:
	cp_stop_hw(cp);
	cp_free_rings(cp);
	return rc;
}

static int cp_close (struct net_device *dev)
{
	struct cp_private *cp = netdev_priv(dev);
	unsigned long flags;

	if (netif_msg_ifdown(cp))
		printk(KERN_DEBUG "%s: disabling interface\n", dev->name);

	spin_lock_irqsave(&cp->lock, flags);

	netif_stop_queue(dev);
	netif_carrier_off(dev);

	cp_stop_hw(cp);

	spin_unlock_irqrestore(&cp->lock, flags);

	synchronize_irq(dev->irq);
	free_irq(dev->irq, dev);

	cp_free_rings(cp);
	return 0;
}

#ifdef BROKEN
static int cp_change_mtu(struct net_device *dev, int new_mtu)
{
	struct cp_private *cp = netdev_priv(dev);
	int rc;
	unsigned long flags;

	/* check for invalid MTU, according to hardware limits */
	if (new_mtu < CP_MIN_MTU || new_mtu > CP_MAX_MTU)
		return -EINVAL;

	/* if network interface not up, no need for complexity */
	if (!netif_running(dev)) {
		dev->mtu = new_mtu;
		cp_set_rxbufsize(cp);	/* set new rx buf size */
		return 0;
	}

	spin_lock_irqsave(&cp->lock, flags);

	cp_stop_hw(cp);			/* stop h/w and free rings */
	cp_clean_rings(cp);

	dev->mtu = new_mtu;
	cp_set_rxbufsize(cp);		/* set new rx buf size */

	rc = cp_init_rings(cp);		/* realloc and restart h/w */
	cp_start_hw(cp);

	spin_unlock_irqrestore(&cp->lock, flags);

	return rc;
}
#endif /* BROKEN */

static char mii_2_8139_map[8] = {
	BasicModeCtrl,
	BasicModeStatus,
	0,
	0,
	NWayAdvert,
	NWayLPAR,
	NWayExpansion,
	0
};

static int mdio_read(struct net_device *dev, int phy_id, int location)
{
	struct cp_private *cp = netdev_priv(dev);

	return location < 8 && mii_2_8139_map[location] ?
	       readw(cp->regs + mii_2_8139_map[location]) : 0;
}


static void mdio_write(struct net_device *dev, int phy_id, int location,
		       int value)
{
	struct cp_private *cp = netdev_priv(dev);

	if (location == 0) {
		cpw8(Cfg9346, Cfg9346_Unlock);
		cpw16(BasicModeCtrl, value);
		cpw8(Cfg9346, Cfg9346_Lock);
	} else if (location < 8 && mii_2_8139_map[location])
		cpw16(mii_2_8139_map[location], value);
}

/* Set the ethtool Wake-on-LAN settings */
static int netdev_set_wol (struct cp_private *cp,
			   const struct ethtool_wolinfo *wol)
{
	u8 options;

	options = cpr8 (Config3) & ~(LinkUp | MagicPacket);
	/* If WOL is being disabled, no need for complexity */
	if (wol->wolopts) {
		if (wol->wolopts & WAKE_PHY)	options |= LinkUp;
		if (wol->wolopts & WAKE_MAGIC)	options |= MagicPacket;
	}

	cpw8 (Cfg9346, Cfg9346_Unlock);
	cpw8 (Config3, options);
	cpw8 (Cfg9346, Cfg9346_Lock);

	options = 0; /* Paranoia setting */
	options = cpr8 (Config5) & ~(UWF | MWF | BWF);
	/* If WOL is being disabled, no need for complexity */
	if (wol->wolopts) {
		if (wol->wolopts & WAKE_UCAST)  options |= UWF;
		if (wol->wolopts & WAKE_BCAST)	options |= BWF;
		if (wol->wolopts & WAKE_MCAST)	options |= MWF;
	}

	cpw8 (Config5, options);

	cp->wol_enabled = (wol->wolopts) ? 1 : 0;

	return 0;
}

/* Get the ethtool Wake-on-LAN settings */
static void netdev_get_wol (struct cp_private *cp,
	             struct ethtool_wolinfo *wol)
{
	u8 options;

	wol->wolopts   = 0; /* Start from scratch */
	wol->supported = WAKE_PHY   | WAKE_BCAST | WAKE_MAGIC |
		         WAKE_MCAST | WAKE_UCAST;
	/* We don't need to go on if WOL is disabled */
	if (!cp->wol_enabled) return;
	
	options        = cpr8 (Config3);
	if (options & LinkUp)        wol->wolopts |= WAKE_PHY;
	if (options & MagicPacket)   wol->wolopts |= WAKE_MAGIC;

	options        = 0; /* Paranoia setting */
	options        = cpr8 (Config5);
	if (options & UWF)           wol->wolopts |= WAKE_UCAST;
	if (options & BWF)           wol->wolopts |= WAKE_BCAST;
	if (options & MWF)           wol->wolopts |= WAKE_MCAST;
}

static void cp_get_drvinfo (struct net_device *dev, struct ethtool_drvinfo *info)
{
	struct cp_private *cp = netdev_priv(dev);

	strcpy (info->driver, DRV_NAME);
	strcpy (info->version, DRV_VERSION);
	strcpy (info->bus_info, pci_name(cp->pdev));
}

static int cp_get_regs_len(struct net_device *dev)
{
	return CP_REGS_SIZE;
}

static int cp_get_stats_count (struct net_device *dev)
{
	return CP_NUM_STATS;
}

static int cp_get_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct cp_private *cp = netdev_priv(dev);
	int rc;
	unsigned long flags;

	spin_lock_irqsave(&cp->lock, flags);
	rc = mii_ethtool_gset(&cp->mii_if, cmd);
	spin_unlock_irqrestore(&cp->lock, flags);

	return rc;
}

static int cp_set_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct cp_private *cp = netdev_priv(dev);
	int rc;
	unsigned long flags;

	spin_lock_irqsave(&cp->lock, flags);
	rc = mii_ethtool_sset(&cp->mii_if, cmd);
	spin_unlock_irqrestore(&cp->lock, flags);

	return rc;
}

static int cp_nway_reset(struct net_device *dev)
{
	struct cp_private *cp = netdev_priv(dev);
	return mii_nway_restart(&cp->mii_if);
}

static u32 cp_get_msglevel(struct net_device *dev)
{
	struct cp_private *cp = netdev_priv(dev);
	return cp->msg_enable;
}

static void cp_set_msglevel(struct net_device *dev, u32 value)
{
	struct cp_private *cp = netdev_priv(dev);
	cp->msg_enable = value;
}

static u32 cp_get_rx_csum(struct net_device *dev)
{
	struct cp_private *cp = netdev_priv(dev);
	return (cpr16(CpCmd) & RxChkSum) ? 1 : 0;
}

static int cp_set_rx_csum(struct net_device *dev, u32 data)
{
	struct cp_private *cp = netdev_priv(dev);
	u16 cmd = cp->cpcmd, newcmd;

	newcmd = cmd;

	if (data)
		newcmd |= RxChkSum;
	else
		newcmd &= ~RxChkSum;

	if (newcmd != cmd) {
		unsigned long flags;

		spin_lock_irqsave(&cp->lock, flags);
		cp->cpcmd = newcmd;
		cpw16_f(CpCmd, newcmd);
		spin_unlock_irqrestore(&cp->lock, flags);
	}

	return 0;
}

static void cp_get_regs(struct net_device *dev, struct ethtool_regs *regs,
		        void *p)
{
	struct cp_private *cp = netdev_priv(dev);
	unsigned long flags;

	if (regs->len < CP_REGS_SIZE)
		return /* -EINVAL */;

	regs->version = CP_REGS_VER;

	spin_lock_irqsave(&cp->lock, flags);
	memcpy_fromio(p, cp->regs, CP_REGS_SIZE);
	spin_unlock_irqrestore(&cp->lock, flags);
}

static void cp_get_wol (struct net_device *dev, struct ethtool_wolinfo *wol)
{
	struct cp_private *cp = netdev_priv(dev);
	unsigned long flags;

	spin_lock_irqsave (&cp->lock, flags);
	netdev_get_wol (cp, wol);
	spin_unlock_irqrestore (&cp->lock, flags);
}

static int cp_set_wol (struct net_device *dev, struct ethtool_wolinfo *wol)
{
	struct cp_private *cp = netdev_priv(dev);
	unsigned long flags;
	int rc;

	spin_lock_irqsave (&cp->lock, flags);
	rc = netdev_set_wol (cp, wol);
	spin_unlock_irqrestore (&cp->lock, flags);

	return rc;
}

static void cp_get_strings (struct net_device *dev, u32 stringset, u8 *buf)
{
	switch (stringset) {
	case ETH_SS_STATS:
		memcpy(buf, &ethtool_stats_keys, sizeof(ethtool_stats_keys));
		break;
	default:
		BUG();
		break;
	}
}

static void cp_get_ethtool_stats (struct net_device *dev,
				  struct ethtool_stats *estats, u64 *tmp_stats)
{
	struct cp_private *cp = netdev_priv(dev);
	unsigned int work = 100;
	int i;

	/* begin NIC statistics dump */
	cpw32(StatsAddr + 4, (cp->nic_stats_dma >> 16) >> 16);
	cpw32(StatsAddr, (cp->nic_stats_dma & 0xffffffff) | DumpStats);
	cpr32(StatsAddr);

	while (work-- > 0) {
		if ((cpr32(StatsAddr) & DumpStats) == 0)
			break;
		cpu_relax();
	}

	if (cpr32(StatsAddr) & DumpStats)
		return /* -EIO */;

	i = 0;
	tmp_stats[i++] = le64_to_cpu(cp->nic_stats->tx_ok);
	tmp_stats[i++] = le64_to_cpu(cp->nic_stats->rx_ok);
	tmp_stats[i++] = le64_to_cpu(cp->nic_stats->tx_err);
	tmp_stats[i++] = le32_to_cpu(cp->nic_stats->rx_err);
	tmp_stats[i++] = le16_to_cpu(cp->nic_stats->rx_fifo);
	tmp_stats[i++] = le16_to_cpu(cp->nic_stats->frame_align);
	tmp_stats[i++] = le32_to_cpu(cp->nic_stats->tx_ok_1col);
	tmp_stats[i++] = le32_to_cpu(cp->nic_stats->tx_ok_mcol);
	tmp_stats[i++] = le64_to_cpu(cp->nic_stats->rx_ok_phys);
	tmp_stats[i++] = le64_to_cpu(cp->nic_stats->rx_ok_bcast);
	tmp_stats[i++] = le32_to_cpu(cp->nic_stats->rx_ok_mcast);
	tmp_stats[i++] = le16_to_cpu(cp->nic_stats->tx_abort);
	tmp_stats[i++] = le16_to_cpu(cp->nic_stats->tx_underrun);
	tmp_stats[i++] = cp->cp_stats.rx_frags;
	if (i != CP_NUM_STATS)
		BUG();
}

static struct ethtool_ops cp_ethtool_ops = {
	.get_drvinfo		= cp_get_drvinfo,
	.get_regs_len		= cp_get_regs_len,
	.get_stats_count	= cp_get_stats_count,
	.get_settings		= cp_get_settings,
	.set_settings		= cp_set_settings,
	.nway_reset		= cp_nway_reset,
	.get_link		= ethtool_op_get_link,
	.get_msglevel		= cp_get_msglevel,
	.set_msglevel		= cp_set_msglevel,
	.get_rx_csum		= cp_get_rx_csum,
	.set_rx_csum		= cp_set_rx_csum,
	.get_tx_csum		= ethtool_op_get_tx_csum,
	.set_tx_csum		= ethtool_op_set_tx_csum, /* local! */
	.get_sg			= ethtool_op_get_sg,
	.set_sg			= ethtool_op_set_sg,
	.get_regs		= cp_get_regs,
	.get_wol		= cp_get_wol,
	.set_wol		= cp_set_wol,
	.get_strings		= cp_get_strings,
	.get_ethtool_stats	= cp_get_ethtool_stats,
};

static int cp_ioctl (struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct cp_private *cp = netdev_priv(dev);
	int rc;
	unsigned long flags;

	if (!netif_running(dev))
		return -EINVAL;

	spin_lock_irqsave(&cp->lock, flags);
	rc = generic_mii_ioctl(&cp->mii_if, if_mii(rq), cmd, NULL);
	spin_unlock_irqrestore(&cp->lock, flags);
	return rc;
}

/* Serial EEPROM section. */

/*  EEPROM_Ctrl bits. */
#define EE_SHIFT_CLK	0x04	/* EEPROM shift clock. */
#define EE_CS			0x08	/* EEPROM chip select. */
#define EE_DATA_WRITE	0x02	/* EEPROM chip data in. */
#define EE_WRITE_0		0x00
#define EE_WRITE_1		0x02
#define EE_DATA_READ	0x01	/* EEPROM chip data out. */
#define EE_ENB			(0x80 | EE_CS)

/* Delay between EEPROM clock transitions.
   No extra delay is needed with 33Mhz PCI, but 66Mhz may change this.
 */

#define eeprom_delay()	readl(ee_addr)

/* The EEPROM commands include the alway-set leading bit. */
#define EE_WRITE_CMD	(5)
#define EE_READ_CMD		(6)
#define EE_ERASE_CMD	(7)

static int read_eeprom (void *ioaddr, int location, int addr_len)
{
	int i;
	unsigned retval = 0;
	void *ee_addr = ioaddr + Cfg9346;
	int read_cmd = location | (EE_READ_CMD << addr_len);

	writeb (EE_ENB & ~EE_CS, ee_addr);
	writeb (EE_ENB, ee_addr);
	eeprom_delay ();

	/* Shift the read command bits out. */
	for (i = 4 + addr_len; i >= 0; i--) {
		int dataval = (read_cmd & (1 << i)) ? EE_DATA_WRITE : 0;
		writeb (EE_ENB | dataval, ee_addr);
		eeprom_delay ();
		writeb (EE_ENB | dataval | EE_SHIFT_CLK, ee_addr);
		eeprom_delay ();
	}
	writeb (EE_ENB, ee_addr);
	eeprom_delay ();

	for (i = 16; i > 0; i--) {
		writeb (EE_ENB | EE_SHIFT_CLK, ee_addr);
		eeprom_delay ();
		retval =
		    (retval << 1) | ((readb (ee_addr) & EE_DATA_READ) ? 1 :
				     0);
		writeb (EE_ENB, ee_addr);
		eeprom_delay ();
	}

	/* Terminate the EEPROM access. */
	writeb (~EE_CS, ee_addr);
	eeprom_delay ();

	return retval;
}

/* Put the board into D3cold state and wait for WakeUp signal */
static void cp_set_d3_state (struct cp_private *cp)
{
	pci_enable_wake (cp->pdev, 0, 1); /* Enable PME# generation */
	pci_set_power_state (cp->pdev, 3);
}

static int cp_init_one (struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct net_device *dev;
	struct cp_private *cp;
	int rc;
	void *regs;
	long pciaddr;
	unsigned int addr_len, i, pci_using_dac;
	u8 pci_rev;

#ifndef MODULE
	static int version_printed;
	if (version_printed++ == 0)
		printk("%s", version);
#endif

	pci_read_config_byte(pdev, PCI_REVISION_ID, &pci_rev);

	if (pdev->vendor == PCI_VENDOR_ID_REALTEK &&
	    pdev->device == PCI_DEVICE_ID_REALTEK_8139 && pci_rev < 0x20) {
		printk(KERN_ERR PFX "pci dev %s (id %04x:%04x rev %02x) is not an 8139C+ compatible chip\n",
		       pci_name(pdev), pdev->vendor, pdev->device, pci_rev);
		printk(KERN_ERR PFX "Try the \"8139too\" driver instead.\n");
		return -ENODEV;
	}

	dev = alloc_etherdev(sizeof(struct cp_private));
	if (!dev)
		return -ENOMEM;
	SET_MODULE_OWNER(dev);
	SET_NETDEV_DEV(dev, &pdev->dev);

	cp = netdev_priv(dev);
	cp->pdev = pdev;
	cp->dev = dev;
	cp->msg_enable = (debug < 0 ? CP_DEF_MSG_ENABLE : debug);
	spin_lock_init (&cp->lock);
	cp->mii_if.dev = dev;
	cp->mii_if.mdio_read = mdio_read;
	cp->mii_if.mdio_write = mdio_write;
	cp->mii_if.phy_id = CP_INTERNAL_PHY;
	cp->mii_if.phy_id_mask = 0x1f;
	cp->mii_if.reg_num_mask = 0x1f;
	cp_set_rxbufsize(cp);

	rc = pci_enable_device(pdev);
	if (rc)
		goto err_out_free;

	rc = pci_set_mwi(pdev);
	if (rc)
		goto err_out_disable;

	rc = pci_request_regions(pdev, DRV_NAME);
	if (rc)
		goto err_out_mwi;

	pciaddr = pci_resource_start(pdev, 1);
	if (!pciaddr) {
		rc = -EIO;
		printk(KERN_ERR PFX "no MMIO resource for pci dev %s\n",
		       pci_name(pdev));
		goto err_out_res;
	}
	if (pci_resource_len(pdev, 1) < CP_REGS_SIZE) {
		rc = -EIO;
		printk(KERN_ERR PFX "MMIO resource (%lx) too small on pci dev %s\n",
		       pci_resource_len(pdev, 1), pci_name(pdev));
		goto err_out_res;
	}

	/* Configure DMA attributes. */
	if ((sizeof(dma_addr_t) > 32) &&
	    !pci_set_consistent_dma_mask(pdev, 0xffffffffffffffffULL) &&
	    !pci_set_dma_mask(pdev, 0xffffffffffffffffULL)) {
		pci_using_dac = 1;
	} else {
		pci_using_dac = 0;

		rc = pci_set_dma_mask(pdev, 0xffffffffULL);
		if (rc) {
			printk(KERN_ERR PFX "No usable DMA configuration, "
			       "aborting.\n");
			goto err_out_res;
		}
		rc = pci_set_consistent_dma_mask(pdev, 0xffffffffULL);
		if (rc) {
			printk(KERN_ERR PFX "No usable consistent DMA configuration, "
			       "aborting.\n");
			goto err_out_res;
		}
	}

	cp->cpcmd = (pci_using_dac ? PCIDAC : 0) |
		    PCIMulRW | RxChkSum | CpRxOn | CpTxOn;

	regs = ioremap(pciaddr, CP_REGS_SIZE);
	if (!regs) {
		rc = -EIO;
		printk(KERN_ERR PFX "Cannot map PCI MMIO (%lx@%lx) on pci dev %s\n",
		       pci_resource_len(pdev, 1), pciaddr, pci_name(pdev));
		goto err_out_res;
	}
	dev->base_addr = (unsigned long) regs;
	cp->regs = regs;

	cp_stop_hw(cp);

	/* read MAC address from EEPROM */
	addr_len = read_eeprom (regs, 0, 8) == 0x8129 ? 8 : 6;
	for (i = 0; i < 3; i++)
		((u16 *) (dev->dev_addr))[i] =
		    le16_to_cpu (read_eeprom (regs, i + 7, addr_len));

	dev->open = cp_open;
	dev->stop = cp_close;
	dev->set_multicast_list = cp_set_rx_mode;
	dev->hard_start_xmit = cp_start_xmit;
	dev->get_stats = cp_get_stats;
	dev->do_ioctl = cp_ioctl;
	dev->poll = cp_rx_poll;
	dev->weight = 16;	/* arbitrary? from NAPI_HOWTO.txt. */
#ifdef BROKEN
	dev->change_mtu = cp_change_mtu;
#endif
	dev->ethtool_ops = &cp_ethtool_ops;
#if 0
	dev->tx_timeout = cp_tx_timeout;
	dev->watchdog_timeo = TX_TIMEOUT;
#endif

#if CP_VLAN_TAG_USED
	dev->features |= NETIF_F_HW_VLAN_TX | NETIF_F_HW_VLAN_RX;
	dev->vlan_rx_register = cp_vlan_rx_register;
	dev->vlan_rx_kill_vid = cp_vlan_rx_kill_vid;
#endif

	if (pci_using_dac)
		dev->features |= NETIF_F_HIGHDMA;

	dev->irq = pdev->irq;

	rc = register_netdev(dev);
	if (rc)
		goto err_out_iomap;

	printk (KERN_INFO "%s: RTL-8139C+ at 0x%lx, "
		"%02x:%02x:%02x:%02x:%02x:%02x, "
		"IRQ %d\n",
		dev->name,
		dev->base_addr,
		dev->dev_addr[0], dev->dev_addr[1],
		dev->dev_addr[2], dev->dev_addr[3],
		dev->dev_addr[4], dev->dev_addr[5],
		dev->irq);

	pci_set_drvdata(pdev, dev);

	/* enable busmastering and memory-write-invalidate */
	pci_set_master(pdev);

	if (cp->wol_enabled) cp_set_d3_state (cp);

	return 0;

err_out_iomap:
	iounmap(regs);
err_out_res:
	pci_release_regions(pdev);
err_out_mwi:
	pci_clear_mwi(pdev);
err_out_disable:
	pci_disable_device(pdev);
err_out_free:
	free_netdev(dev);
	return rc;
}

static void cp_remove_one (struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	struct cp_private *cp = netdev_priv(dev);

	if (!dev)
		BUG();
	unregister_netdev(dev);
	iounmap(cp->regs);
	if (cp->wol_enabled) pci_set_power_state (pdev, 0);
	pci_release_regions(pdev);
	pci_clear_mwi(pdev);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	free_netdev(dev);
}

#ifdef CONFIG_PM
static int cp_suspend (struct pci_dev *pdev, u32 state)
{
	struct net_device *dev;
	struct cp_private *cp;
	unsigned long flags;

	dev = pci_get_drvdata (pdev);
	cp  = netdev_priv(dev);

	if (!dev || !netif_running (dev)) return 0;

	netif_device_detach (dev);
	netif_stop_queue (dev);

	spin_lock_irqsave (&cp->lock, flags);

	/* Disable Rx and Tx */
	cpw16 (IntrMask, 0);
	cpw8  (Cmd, cpr8 (Cmd) & (~RxOn | ~TxOn));

	spin_unlock_irqrestore (&cp->lock, flags);

	if (cp->pdev && cp->wol_enabled) {
		pci_save_state (cp->pdev, cp->power_state);
		cp_set_d3_state (cp);
	}

	return 0;
}

static int cp_resume (struct pci_dev *pdev)
{
	struct net_device *dev;
	struct cp_private *cp;

	dev = pci_get_drvdata (pdev);
	cp  = netdev_priv(dev);

	netif_device_attach (dev);
	
	if (cp->pdev && cp->wol_enabled) {
		pci_set_power_state (cp->pdev, 0);
		pci_restore_state (cp->pdev, cp->power_state);
	}
	
	cp_init_hw (cp);
	netif_start_queue (dev);
	
	return 0;
}
#endif /* CONFIG_PM */

static struct pci_driver cp_driver = {
	.name         = DRV_NAME,
	.id_table     = cp_pci_tbl,
	.probe        =	cp_init_one,
	.remove       = cp_remove_one,
#ifdef CONFIG_PM
	.resume       = cp_resume,
	.suspend      = cp_suspend,
#endif
};

static int __init cp_init (void)
{
#ifdef MODULE
	printk("%s", version);
#endif
	return pci_module_init (&cp_driver);
}

static void __exit cp_exit (void)
{
	pci_unregister_driver (&cp_driver);
}

module_init(cp_init);
module_exit(cp_exit);
