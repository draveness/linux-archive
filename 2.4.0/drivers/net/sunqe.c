/* $Id: sunqe.c,v 1.47 2000/10/22 16:08:38 davem Exp $
 * sunqe.c: Sparc QuadEthernet 10baseT SBUS card driver.
 *          Once again I am out to prove that every ethernet
 *          controller out there can be most efficiently programmed
 *          if you make it look like a LANCE.
 *
 * Copyright (C) 1996, 1999 David S. Miller (davem@redhat.com)
 */

static char *version =
        "sunqe.c:v2.9 9/11/99 David S. Miller (davem@redhat.com)\n";

#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/ioport.h>
#include <linux/in.h>
#include <linux/malloc.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/init.h>

#include <asm/system.h>
#include <asm/bitops.h>
#include <asm/io.h>
#include <asm/dma.h>
#include <linux/errno.h>
#include <asm/byteorder.h>

#include <asm/idprom.h>
#include <asm/sbus.h>
#include <asm/openprom.h>
#include <asm/oplib.h>
#include <asm/auxio.h>
#include <asm/pgtable.h>
#include <asm/irq.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>

#include "sunqe.h"

static struct sunqec *root_qec_dev = NULL;

static void qe_set_multicast(struct net_device *dev);

#define QEC_RESET_TRIES 200

static inline int qec_global_reset(unsigned long gregs)
{
	int tries = QEC_RESET_TRIES;

	sbus_writel(GLOB_CTRL_RESET, gregs + GLOB_CTRL);
	while (--tries) {
		u32 tmp = sbus_readl(gregs + GLOB_CTRL);
		if (tmp & GLOB_CTRL_RESET) {
			udelay(20);
			continue;
		}
		break;
	}
	if (tries)
		return 0;
	printk(KERN_ERR "QuadEther: AIEEE cannot reset the QEC!\n");
	return -1;
}

#define MACE_RESET_RETRIES 200
#define QE_RESET_RETRIES   200

static inline int qe_stop(struct sunqe *qep)
{
	unsigned long cregs = qep->qcregs;
	unsigned long mregs = qep->mregs;
	int tries;

	/* Reset the MACE, then the QEC channel. */
	sbus_writeb(MREGS_BCONFIG_RESET, mregs + MREGS_BCONFIG);
	tries = MACE_RESET_RETRIES;
	while (--tries) {
		u8 tmp = sbus_readb(mregs + MREGS_BCONFIG);
		if (tmp & MREGS_BCONFIG_RESET) {
			udelay(20);
			continue;
		}
		break;
	}
	if (!tries) {
		printk(KERN_ERR "QuadEther: AIEEE cannot reset the MACE!\n");
		return -1;
	}

	sbus_writel(CREG_CTRL_RESET, cregs + CREG_CTRL);
	tries = QE_RESET_RETRIES;
	while (--tries) {
		u32 tmp = sbus_readl(cregs + CREG_CTRL);
		if (tmp & CREG_CTRL_RESET) {
			udelay(20);
			continue;
		}
		break;
	}
	if (!tries) {
		printk(KERN_ERR "QuadEther: Cannot reset QE channel!\n");
		return -1;
	}
	return 0;
}

static void qe_init_rings(struct sunqe *qep)
{
	struct qe_init_block *qb = qep->qe_block;
	struct sunqe_buffers *qbufs = qep->buffers;
	__u32 qbufs_dvma = qep->buffers_dvma;
	int i;

	qep->rx_new = qep->rx_old = qep->tx_new = qep->tx_old = 0;
	memset(qb, 0, sizeof(struct qe_init_block));
	memset(qbufs, 0, sizeof(struct sunqe_buffers));
	for (i = 0; i < RX_RING_SIZE; i++) {
		qb->qe_rxd[i].rx_addr = qbufs_dvma + qebuf_offset(rx_buf, i);
		qb->qe_rxd[i].rx_flags =
			(RXD_OWN | ((RXD_PKT_SZ) & RXD_LENGTH));
	}
}

static int qe_init(struct sunqe *qep, int from_irq)
{
	struct sunqec *qecp = qep->parent;
	unsigned long cregs = qep->qcregs;
	unsigned long mregs = qep->mregs;
	unsigned long gregs = qecp->gregs;
	unsigned char *e = &qep->dev->dev_addr[0];
	u32 tmp;
	int i;

	/* Shut it up. */
	if (qe_stop(qep))
		return -EAGAIN;

	/* Setup initial rx/tx init block pointers. */
	sbus_writel(qep->qblock_dvma + qib_offset(qe_rxd, 0), cregs + CREG_RXDS);
	sbus_writel(qep->qblock_dvma + qib_offset(qe_txd, 0), cregs + CREG_TXDS);

	/* Enable/mask the various irq's. */
	sbus_writel(0, cregs + CREG_RIMASK);
	sbus_writel(1, cregs + CREG_TIMASK);

	sbus_writel(0, cregs + CREG_QMASK);
	sbus_writel(CREG_MMASK_RXCOLL, cregs + CREG_MMASK);

	/* Setup the FIFO pointers into QEC local memory. */
	tmp = qep->channel * sbus_readl(gregs + GLOB_MSIZE);
	sbus_writel(tmp, cregs + CREG_RXRBUFPTR);
	sbus_writel(tmp, cregs + CREG_RXWBUFPTR);

	tmp = sbus_readl(cregs + CREG_RXRBUFPTR) +
		sbus_readl(gregs + GLOB_RSIZE);
	sbus_writel(tmp, cregs + CREG_TXRBUFPTR);
	sbus_writel(tmp, cregs + CREG_TXWBUFPTR);

	/* Clear the channel collision counter. */
	sbus_writel(0, cregs + CREG_CCNT);

	/* For 10baseT, inter frame space nor throttle seems to be necessary. */
	sbus_writel(0, cregs + CREG_PIPG);

	/* Now dork with the AMD MACE. */
	sbus_writeb(MREGS_PHYCONFIG_AUTO, mregs + MREGS_PHYCONFIG);
	sbus_writeb(MREGS_TXFCNTL_AUTOPAD, mregs + MREGS_TXFCNTL);
	sbus_writeb(0, mregs + MREGS_RXFCNTL);

	/* The QEC dma's the rx'd packets from local memory out to main memory,
	 * and therefore it interrupts when the packet reception is "complete".
	 * So don't listen for the MACE talking about it.
	 */
	sbus_writeb(MREGS_IMASK_COLL | MREGS_IMASK_RXIRQ, mregs + MREGS_IMASK);
	sbus_writeb(MREGS_BCONFIG_BSWAP | MREGS_BCONFIG_64TS, mregs + MREGS_BCONFIG);
	sbus_writeb((MREGS_FCONFIG_TXF16 | MREGS_FCONFIG_RXF32 |
		     MREGS_FCONFIG_RFWU | MREGS_FCONFIG_TFWU),
		    mregs + MREGS_FCONFIG);

	/* Only usable interface on QuadEther is twisted pair. */
	sbus_writeb(MREGS_PLSCONFIG_TP, mregs + MREGS_PLSCONFIG);

	/* Tell MACE we are changing the ether address. */
	sbus_writeb(MREGS_IACONFIG_ACHNGE | MREGS_IACONFIG_PARESET,
		    mregs + MREGS_IACONFIG);
	while ((sbus_readb(mregs + MREGS_IACONFIG) & MREGS_IACONFIG_ACHNGE) != 0)
		barrier();
	sbus_writeb(e[0], mregs + MREGS_ETHADDR);
	sbus_writeb(e[1], mregs + MREGS_ETHADDR);
	sbus_writeb(e[2], mregs + MREGS_ETHADDR);
	sbus_writeb(e[3], mregs + MREGS_ETHADDR);
	sbus_writeb(e[4], mregs + MREGS_ETHADDR);
	sbus_writeb(e[5], mregs + MREGS_ETHADDR);

	/* Clear out the address filter. */
	sbus_writeb(MREGS_IACONFIG_ACHNGE | MREGS_IACONFIG_LARESET,
		    mregs + MREGS_IACONFIG);
	while ((sbus_readb(mregs + MREGS_IACONFIG) & MREGS_IACONFIG_ACHNGE) != 0)
		barrier();
	for (i = 0; i < 8; i++)
		sbus_writeb(0, mregs + MREGS_FILTER);

	/* Address changes are now complete. */
	sbus_writeb(0, mregs + MREGS_IACONFIG);

	qe_init_rings(qep);

	/* Wait a little bit for the link to come up... */
	mdelay(5);
	if (!(sbus_readb(mregs + MREGS_PHYCONFIG) & MREGS_PHYCONFIG_LTESTDIS)) {
		int tries = 50;

		while (tries--) {
			u8 tmp;

			mdelay(5);
			barrier();
			tmp = sbus_readb(mregs + MREGS_PHYCONFIG);
			if ((tmp & MREGS_PHYCONFIG_LSTAT) != 0)
				break;
		}
		if (tries == 0)
			printk(KERN_NOTICE "%s: Warning, link state is down.\n", qep->dev->name);
	}

	/* Missed packet counter is cleared on a read. */
	sbus_readb(mregs + MREGS_MPCNT);

	/* Reload multicast information, this will enable the receiver
	 * and transmitter.
	 */
	qe_set_multicast(qep->dev);

	/* QEC should now start to show interrupts. */
	return 0;
}

/* Grrr, certain error conditions completely lock up the AMD MACE,
 * so when we get these we _must_ reset the chip.
 */
static int qe_is_bolixed(struct sunqe *qep, u32 qe_status)
{
	struct net_device *dev = qep->dev;
	int mace_hwbug_workaround = 0;

	if (qe_status & CREG_STAT_EDEFER) {
		printk(KERN_ERR "%s: Excessive transmit defers.\n", dev->name);
		qep->net_stats.tx_errors++;
	}

	if (qe_status & CREG_STAT_CLOSS) {
		printk(KERN_ERR "%s: Carrier lost, link down?\n", dev->name);
		qep->net_stats.tx_errors++;
		qep->net_stats.tx_carrier_errors++;
	}

	if (qe_status & CREG_STAT_ERETRIES) {
		printk(KERN_ERR "%s: Excessive transmit retries (more than 16).\n", dev->name);
		qep->net_stats.tx_errors++;
		mace_hwbug_workaround = 1;
	}

	if (qe_status & CREG_STAT_LCOLL) {
		printk(KERN_ERR "%s: Late transmit collision.\n", dev->name);
		qep->net_stats.tx_errors++;
		qep->net_stats.collisions++;
		mace_hwbug_workaround = 1;
	}

	if (qe_status & CREG_STAT_FUFLOW) {
		printk(KERN_ERR "%s: Transmit fifo underflow, driver bug.\n", dev->name);
		qep->net_stats.tx_errors++;
		mace_hwbug_workaround = 1;
	}

	if (qe_status & CREG_STAT_JERROR) {
		printk(KERN_ERR "%s: Jabber error.\n", dev->name);
	}

	if (qe_status & CREG_STAT_BERROR) {
		printk(KERN_ERR "%s: Babble error.\n", dev->name);
	}

	if (qe_status & CREG_STAT_CCOFLOW) {
		qep->net_stats.tx_errors += 256;
		qep->net_stats.collisions += 256;
	}

	if (qe_status & CREG_STAT_TXDERROR) {
		printk(KERN_ERR "%s: Transmit descriptor is bogus, driver bug.\n", dev->name);
		qep->net_stats.tx_errors++;
		qep->net_stats.tx_aborted_errors++;
		mace_hwbug_workaround = 1;
	}

	if (qe_status & CREG_STAT_TXLERR) {
		printk(KERN_ERR "%s: Transmit late error.\n", dev->name);
		qep->net_stats.tx_errors++;
		mace_hwbug_workaround = 1;
	}

	if (qe_status & CREG_STAT_TXPERR) {
		printk(KERN_ERR "%s: Transmit DMA parity error.\n", dev->name);
		qep->net_stats.tx_errors++;
		qep->net_stats.tx_aborted_errors++;
		mace_hwbug_workaround = 1;
	}

	if (qe_status & CREG_STAT_TXSERR) {
		printk(KERN_ERR "%s: Transmit DMA sbus error ack.\n", dev->name);
		qep->net_stats.tx_errors++;
		qep->net_stats.tx_aborted_errors++;
		mace_hwbug_workaround = 1;
	}

	if (qe_status & CREG_STAT_RCCOFLOW) {
		qep->net_stats.rx_errors += 256;
		qep->net_stats.collisions += 256;
	}

	if (qe_status & CREG_STAT_RUOFLOW) {
		qep->net_stats.rx_errors += 256;
		qep->net_stats.rx_over_errors += 256;
	}

	if (qe_status & CREG_STAT_MCOFLOW) {
		qep->net_stats.rx_errors += 256;
		qep->net_stats.rx_missed_errors += 256;
	}

	if (qe_status & CREG_STAT_RXFOFLOW) {
		printk(KERN_ERR "%s: Receive fifo overflow.\n", dev->name);
		qep->net_stats.rx_errors++;
		qep->net_stats.rx_over_errors++;
	}

	if (qe_status & CREG_STAT_RLCOLL) {
		printk(KERN_ERR "%s: Late receive collision.\n", dev->name);
		qep->net_stats.rx_errors++;
		qep->net_stats.collisions++;
	}

	if (qe_status & CREG_STAT_FCOFLOW) {
		qep->net_stats.rx_errors += 256;
		qep->net_stats.rx_frame_errors += 256;
	}

	if (qe_status & CREG_STAT_CECOFLOW) {
		qep->net_stats.rx_errors += 256;
		qep->net_stats.rx_crc_errors += 256;
	}

	if (qe_status & CREG_STAT_RXDROP) {
		printk(KERN_ERR "%s: Receive packet dropped.\n", dev->name);
		qep->net_stats.rx_errors++;
		qep->net_stats.rx_dropped++;
		qep->net_stats.rx_missed_errors++;
	}

	if (qe_status & CREG_STAT_RXSMALL) {
		printk(KERN_ERR "%s: Receive buffer too small, driver bug.\n", dev->name);
		qep->net_stats.rx_errors++;
		qep->net_stats.rx_length_errors++;
	}

	if (qe_status & CREG_STAT_RXLERR) {
		printk(KERN_ERR "%s: Receive late error.\n", dev->name);
		qep->net_stats.rx_errors++;
		mace_hwbug_workaround = 1;
	}

	if (qe_status & CREG_STAT_RXPERR) {
		printk(KERN_ERR "%s: Receive DMA parity error.\n", dev->name);
		qep->net_stats.rx_errors++;
		qep->net_stats.rx_missed_errors++;
		mace_hwbug_workaround = 1;
	}

	if (qe_status & CREG_STAT_RXSERR) {
		printk(KERN_ERR "%s: Receive DMA sbus error ack.\n", dev->name);
		qep->net_stats.rx_errors++;
		qep->net_stats.rx_missed_errors++;
		mace_hwbug_workaround = 1;
	}

	if (mace_hwbug_workaround)
		qe_init(qep, 1);
	return mace_hwbug_workaround;
}

/* Per-QE receive interrupt service routine.  Just like on the happy meal
 * we receive directly into skb's with a small packet copy water mark.
 */
static void qe_rx(struct sunqe *qep)
{
	struct qe_rxd *rxbase = &qep->qe_block->qe_rxd[0];
	struct qe_rxd *this;
	struct sunqe_buffers *qbufs = qep->buffers;
	__u32 qbufs_dvma = qep->buffers_dvma;
	int elem = qep->rx_new, drops = 0;
	u32 flags;

	this = &rxbase[elem];
	while (!((flags = this->rx_flags) & RXD_OWN)) {
		struct sk_buff *skb;
		unsigned char *this_qbuf =
			&qbufs->rx_buf[elem & (RX_RING_SIZE - 1)][0];
		__u32 this_qbuf_dvma = qbufs_dvma +
			qebuf_offset(rx_buf, (elem & (RX_RING_SIZE - 1)));
		struct qe_rxd *end_rxd =
			&rxbase[(elem+RX_RING_SIZE)&(RX_RING_MAXSIZE-1)];
		int len = (flags & RXD_LENGTH) - 4;  /* QE adds ether FCS size to len */

		/* Check for errors. */
		if (len < ETH_ZLEN) {
			qep->net_stats.rx_errors++;
			qep->net_stats.rx_length_errors++;
			qep->net_stats.rx_dropped++;
		} else {
			skb = dev_alloc_skb(len + 2);
			if (skb == NULL) {
				drops++;
				qep->net_stats.rx_dropped++;
			} else {
				skb->dev = qep->dev;
				skb_reserve(skb, 2);
				skb_put(skb, len);
				eth_copy_and_sum(skb, (unsigned char *) this_qbuf,
						 len, 0);
				skb->protocol = eth_type_trans(skb, qep->dev);
				netif_rx(skb);
				qep->net_stats.rx_packets++;
				qep->net_stats.rx_bytes += len;
			}
		}
		end_rxd->rx_addr = this_qbuf_dvma;
		end_rxd->rx_flags = (RXD_OWN | ((RXD_PKT_SZ) & RXD_LENGTH));
		
		elem = NEXT_RX(elem);
		this = &rxbase[elem];
	}
	qep->rx_new = elem;
	if (drops)
		printk(KERN_NOTICE "%s: Memory squeeze, deferring packet.\n", qep->dev->name);
}

static void qe_tx_reclaim(struct sunqe *qep);

/* Interrupts for all QE's get filtered out via the QEC master controller,
 * so we just run through each qe and check to see who is signaling
 * and thus needs to be serviced.
 */
static void qec_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	struct sunqec *qecp = (struct sunqec *) dev_id;
	u32 qec_status;
	int channel = 0;

	/* Latch the status now. */
	qec_status = sbus_readl(qecp->gregs + GLOB_STAT);
	while (channel < 4) {
		if (qec_status & 0xf) {
			struct sunqe *qep = qecp->qes[channel];
			u32 qe_status;

			qe_status = sbus_readl(qep->qcregs + CREG_STAT);
			if (qe_status & CREG_STAT_ERRORS) {
				if (qe_is_bolixed(qep, qe_status))
					goto next;
			}
			if (qe_status & CREG_STAT_RXIRQ)
				qe_rx(qep);
			if (netif_queue_stopped(qep->dev) &&
			    (qe_status & CREG_STAT_TXIRQ)) {
				spin_lock(&qep->lock);
				qe_tx_reclaim(qep);
				if (TX_BUFFS_AVAIL(qep) > 0) {
					/* Wake net queue and return to
					 * lazy tx reclaim.
					 */
					netif_wake_queue(qep->dev);
					sbus_writel(1, qep->qcregs + CREG_TIMASK);
				}
				spin_unlock(&qep->lock);
			}
	next:
		}
		qec_status >>= 4;
		channel++;
	}
}

static int qe_open(struct net_device *dev)
{
	struct sunqe *qep = (struct sunqe *) dev->priv;
	int res;

	qep->mconfig = (MREGS_MCONFIG_TXENAB |
			MREGS_MCONFIG_RXENAB |
			MREGS_MCONFIG_MBAENAB);
	res = qe_init(qep, 0);
	if (!res)
		MOD_INC_USE_COUNT;

	return res;
}

static int qe_close(struct net_device *dev)
{
	struct sunqe *qep = (struct sunqe *) dev->priv;

	qe_stop(qep);
	MOD_DEC_USE_COUNT;
	return 0;
}

/* Reclaim TX'd frames from the ring.  This must always run under
 * the IRQ protected qep->lock.
 */
static void qe_tx_reclaim(struct sunqe *qep)
{
	struct qe_txd *txbase = &qep->qe_block->qe_txd[0];
	int elem = qep->tx_old;

	while (elem != qep->tx_new) {
		u32 flags = txbase[elem].tx_flags;

		if (flags & TXD_OWN)
			break;
		elem = NEXT_TX(elem);
	}
	qep->tx_old = elem;
}

static void qe_tx_timeout(struct net_device *dev)
{
	struct sunqe *qep = (struct sunqe *) dev->priv;
	int tx_full;

	spin_lock_irq(&qep->lock);

	/* Try to reclaim, if that frees up some tx
	 * entries, we're fine.
	 */
	qe_tx_reclaim(qep);
	tx_full = TX_BUFFS_AVAIL(qep) <= 0;

	spin_unlock_irq(&qep->lock);

	if (! tx_full)
		goto out;

	printk(KERN_ERR "%s: transmit timed out, resetting\n", dev->name);
	qe_init(qep, 1);

out:
	netif_wake_queue(dev);
}

/* Get a packet queued to go onto the wire. */
static int qe_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct sunqe *qep = (struct sunqe *) dev->priv;
	struct sunqe_buffers *qbufs = qep->buffers;
	__u32 txbuf_dvma, qbufs_dvma = qep->buffers_dvma;
	unsigned char *txbuf;
	int len, entry;

	spin_lock_irq(&qep->lock);

	qe_tx_reclaim(qep);

	len = skb->len;
	entry = qep->tx_new;

	txbuf = &qbufs->tx_buf[entry & (TX_RING_SIZE - 1)][0];
	txbuf_dvma = qbufs_dvma +
		qebuf_offset(tx_buf, (entry & (TX_RING_SIZE - 1)));

	/* Avoid a race... */
	qep->qe_block->qe_txd[entry].tx_flags = TXD_UPDATE;

	memcpy(txbuf, skb->data, len);

	qep->qe_block->qe_txd[entry].tx_addr = txbuf_dvma;
	qep->qe_block->qe_txd[entry].tx_flags =
		(TXD_OWN | TXD_SOP | TXD_EOP | (len & TXD_LENGTH));
	qep->tx_new = NEXT_TX(entry);

	/* Get it going. */
	dev->trans_start = jiffies;
	sbus_writel(CREG_CTRL_TWAKEUP, qep->qcregs + CREG_CTRL);

	qep->net_stats.tx_packets++;
	qep->net_stats.tx_bytes += len;

	if (TX_BUFFS_AVAIL(qep) <= 0) {
		/* Halt the net queue and enable tx interrupts.
		 * When the tx queue empties the tx irq handler
		 * will wake up the queue and return us back to
		 * the lazy tx reclaim scheme.
		 */
		netif_stop_queue(dev);
		sbus_writel(0, qep->qcregs + CREG_TIMASK);
	}
	spin_unlock_irq(&qep->lock);

	dev_kfree_skb(skb);

	return 0;
}

static struct net_device_stats *qe_get_stats(struct net_device *dev)
{
	struct sunqe *qep = (struct sunqe *) dev->priv;

	return &qep->net_stats;
}

#define CRC_POLYNOMIAL_BE 0x04c11db7UL  /* Ethernet CRC, big endian */
#define CRC_POLYNOMIAL_LE 0xedb88320UL  /* Ethernet CRC, little endian */

static void qe_set_multicast(struct net_device *dev)
{
	struct sunqe *qep = (struct sunqe *) dev->priv;
	struct dev_mc_list *dmi = dev->mc_list;
	u8 new_mconfig = qep->mconfig;
	char *addrs;
	int i, j, bit, byte;
	u32 crc, poly = CRC_POLYNOMIAL_LE;

	/* Lock out others. */
	netif_stop_queue(dev);

	if ((dev->flags & IFF_ALLMULTI) || (dev->mc_count > 64)) {
		sbus_writeb(MREGS_IACONFIG_ACHNGE | MREGS_IACONFIG_LARESET,
			    qep->mregs + MREGS_IACONFIG);
		while ((sbus_readb(qep->mregs + MREGS_IACONFIG) & MREGS_IACONFIG_ACHNGE) != 0)
			barrier();
		for (i = 0; i < 8; i++)
			sbus_writeb(0xff, qep->mregs + MREGS_FILTER);
		sbus_writeb(0, qep->mregs + MREGS_IACONFIG);
	} else if (dev->flags & IFF_PROMISC) {
		new_mconfig |= MREGS_MCONFIG_PROMISC;
	} else {
		u16 hash_table[4];
		u8 *hbytes = (unsigned char *) &hash_table[0];

		for (i = 0; i < 4; i++)
			hash_table[i] = 0;

		for (i = 0; i < dev->mc_count; i++) {
			addrs = dmi->dmi_addr;
			dmi = dmi->next;

			if (!(*addrs & 1))
				continue;

			crc = 0xffffffffU;
			for (byte = 0; byte < 6; byte++) {
				for (bit = *addrs++, j = 0; j < 8; j++, bit >>= 1) {
					int test;

					test = ((bit ^ crc) & 0x01);
					crc >>= 1;
					if (test)
						crc = crc ^ poly;
				}
			}
			crc >>= 26;
			hash_table[crc >> 4] |= 1 << (crc & 0xf);
		}
		/* Program the qe with the new filter value. */
		sbus_writeb(MREGS_IACONFIG_ACHNGE | MREGS_IACONFIG_LARESET,
			    qep->mregs + MREGS_IACONFIG);
		while ((sbus_readb(qep->mregs + MREGS_IACONFIG) & MREGS_IACONFIG_ACHNGE) != 0)
			barrier();
		for (i = 0; i < 8; i++) {
			u8 tmp = *hbytes++;
			sbus_writeb(tmp, qep->mregs + MREGS_FILTER);
		}
		sbus_writeb(0, qep->mregs + MREGS_IACONFIG);
	}

	/* Any change of the logical address filter, the physical address,
	 * or enabling/disabling promiscuous mode causes the MACE to disable
	 * the receiver.  So we must re-enable them here or else the MACE
	 * refuses to listen to anything on the network.  Sheesh, took
	 * me a day or two to find this bug.
	 */
	qep->mconfig = new_mconfig;
	sbus_writeb(qep->mconfig, qep->mregs + MREGS_MCONFIG);

	/* Let us get going again. */
	netif_wake_queue(dev);
}

/* This is only called once at boot time for each card probed. */
static inline void qec_init_once(struct sunqec *qecp, struct sbus_dev *qsdev)
{
	u8 bsizes = qecp->qec_bursts;

	if (sbus_can_burst64(qsdev) && (bsizes & DMA_BURST64)) {
		sbus_writel(GLOB_CTRL_B64, qecp->gregs + GLOB_CTRL);
	} else if (bsizes & DMA_BURST32) {
		sbus_writel(GLOB_CTRL_B32, qecp->gregs + GLOB_CTRL);
	} else {
		sbus_writel(GLOB_CTRL_B16, qecp->gregs + GLOB_CTRL);
	}

	/* Packetsize only used in 100baseT BigMAC configurations,
	 * set it to zero just to be on the safe side.
	 */
	sbus_writel(GLOB_PSIZE_2048, qecp->gregs + GLOB_PSIZE);

	/* Set the local memsize register, divided up to one piece per QE channel. */
	sbus_writel((qsdev->reg_addrs[1].reg_size >> 2),
		    qecp->gregs + GLOB_MSIZE);

	/* Divide up the local QEC memory amongst the 4 QE receiver and
	 * transmitter FIFOs.  Basically it is (total / 2 / num_channels).
	 */
	sbus_writel((qsdev->reg_addrs[1].reg_size >> 2) >> 1,
		    qecp->gregs + GLOB_TSIZE);
	sbus_writel((qsdev->reg_addrs[1].reg_size >> 2) >> 1,
		    qecp->gregs + GLOB_RSIZE);
}

/* Four QE's per QEC card. */
static int __init qec_ether_init(struct net_device *dev, struct sbus_dev *sdev)
{
	static unsigned version_printed = 0;
	struct net_device *qe_devs[4];
	struct sunqe *qeps[4];
	struct sbus_dev *qesdevs[4];
	struct sunqec *qecp = NULL;
	u8 bsizes, bsizes_more;
	int i, j, res = ENOMEM;

	dev = init_etherdev(0, sizeof(struct sunqe));
	qe_devs[0] = dev;
	qeps[0] = (struct sunqe *) dev->priv;
	qeps[0]->channel = 0;
	spin_lock_init(&qeps[0]->lock);
	for (j = 0; j < 6; j++)
		qe_devs[0]->dev_addr[j] = idprom->id_ethaddr[j];

	if (version_printed++ == 0)
		printk(KERN_INFO "%s", version);

	qe_devs[1] = qe_devs[2] = qe_devs[3] = NULL;
	for (i = 1; i < 4; i++) {
		qe_devs[i] = init_etherdev(0, sizeof(struct sunqe));
		if (qe_devs[i] == NULL || qe_devs[i]->priv == NULL)
			goto qec_free_devs;
		qeps[i] = (struct sunqe *) qe_devs[i]->priv;
		for (j = 0; j < 6; j++)
			qe_devs[i]->dev_addr[j] = idprom->id_ethaddr[j];
		qeps[i]->channel = i;
	}
	qecp = kmalloc(sizeof(struct sunqec), GFP_KERNEL);
	if (qecp == NULL)
		goto qec_free_devs;
	qecp->qec_sdev = sdev;

	for (i = 0; i < 4; i++) {
		qecp->qes[i] = qeps[i];
		qeps[i]->dev = qe_devs[i];
		qeps[i]->parent = qecp;
	}

	/* Link in channel 0. */
	i = prom_getintdefault(sdev->child->prom_node, "channel#", -1);
	if (i == -1) { res=ENODEV; goto qec_free_devs; }
	qesdevs[i] = sdev->child;

	/* Link in channel 1. */
	i = prom_getintdefault(sdev->child->next->prom_node, "channel#", -1);
	if (i == -1) { res=ENODEV; goto qec_free_devs; }
	qesdevs[i] = sdev->child->next;

	/* Link in channel 2. */
	i = prom_getintdefault(sdev->child->next->next->prom_node, "channel#", -1);
	if (i == -1) { res=ENODEV; goto qec_free_devs; }
	qesdevs[i] = sdev->child->next->next;

	/* Link in channel 3. */
	i = prom_getintdefault(sdev->child->next->next->next->prom_node, "channel#", -1);
	if (i == -1) { res=ENODEV; goto qec_free_devs; }
	qesdevs[i] = sdev->child->next->next->next;

	for (i = 0; i < 4; i++)
		qeps[i]->qe_sdev = qesdevs[i];

	/* Now map in the registers, QEC globals first. */
	qecp->gregs = sbus_ioremap(&sdev->resource[0], 0,
				   GLOB_REG_SIZE, "QEC Global Registers");
	if (!qecp->gregs) {
		printk(KERN_ERR "QuadEther: Cannot map QEC global registers.\n");
		res = ENODEV;
		goto qec_free_devs;
	}

	/* Make sure the QEC is in MACE mode. */
	if ((sbus_readl(qecp->gregs + GLOB_CTRL) & 0xf0000000) != GLOB_CTRL_MMODE) {
		printk(KERN_ERR "QuadEther: AIEEE, QEC is not in MACE mode!\n");
		res = ENODEV;
		goto qec_free_devs;
	}

	/* Reset the QEC. */
	if (qec_global_reset(qecp->gregs)) {
		res = ENODEV;
		goto qec_free_devs;
	}

	/* Find and set the burst sizes for the QEC, since it does
	 * the actual dma for all 4 channels.
	 */
	bsizes = prom_getintdefault(sdev->prom_node, "burst-sizes", 0xff);
	bsizes &= 0xff;
	bsizes_more = prom_getintdefault(sdev->bus->prom_node, "burst-sizes", 0xff);

	if (bsizes_more != 0xff)
		bsizes &= bsizes_more;
	if (bsizes == 0xff || (bsizes & DMA_BURST16) == 0 ||
	   (bsizes & DMA_BURST32)==0)
		bsizes = (DMA_BURST32 - 1);

	qecp->qec_bursts = bsizes;

	/* Perform one time QEC initialization, we never touch the QEC
	 * globals again after this.
	 */
	qec_init_once(qecp, sdev);

	for (i = 0; i < 4; i++) {
		/* Map in QEC per-channel control registers. */
		qeps[i]->qcregs = sbus_ioremap(&qesdevs[i]->resource[0], 0,
					       CREG_REG_SIZE, "QEC Channel Registers");
		if (!qeps[i]->qcregs) {
			printk(KERN_ERR "QuadEther: Cannot map QE %d's channel registers.\n", i);
			res = ENODEV;
			goto qec_free_devs;
		}

		/* Map in per-channel AMD MACE registers. */
		qeps[i]->mregs = sbus_ioremap(&qesdevs[i]->resource[1], 0,
					      MREGS_REG_SIZE, "QE MACE Registers");
		if (!qeps[i]->mregs) {
			printk(KERN_ERR "QuadEther: Cannot map QE %d's MACE registers.\n", i);
			res = ENODEV;
			goto qec_free_devs;
		}

		qeps[i]->qe_block = sbus_alloc_consistent(qesdevs[i],
							  PAGE_SIZE,
							  &qeps[i]->qblock_dvma);
		qeps[i]->buffers = sbus_alloc_consistent(qesdevs[i],
							 sizeof(struct sunqe_buffers),
							 &qeps[i]->buffers_dvma);
		if (qeps[i]->qe_block == NULL ||
		    qeps[i]->qblock_dvma == 0 ||
		    qeps[i]->buffers == NULL ||
		    qeps[i]->buffers_dvma == 0) {
			res = ENODEV;
			goto qec_free_devs;
		}

		/* Stop this QE. */
		qe_stop(qeps[i]);
	}

	for (i = 0; i < 4; i++) {
		qe_devs[i]->open = qe_open;
		qe_devs[i]->stop = qe_close;
		qe_devs[i]->hard_start_xmit = qe_start_xmit;
		qe_devs[i]->get_stats = qe_get_stats;
		qe_devs[i]->set_multicast_list = qe_set_multicast;
		qe_devs[i]->tx_timeout = qe_tx_timeout;
		qe_devs[i]->watchdog_timeo = 5*HZ;
		qe_devs[i]->irq = sdev->irqs[0];
		qe_devs[i]->dma = 0;
		ether_setup(qe_devs[i]);
	}

	/* QEC receives interrupts from each QE, then it sends the actual
	 * IRQ to the cpu itself.  Since QEC is the single point of
	 * interrupt for all QE channels we register the IRQ handler
	 * for it now.
	 */
	if (request_irq(sdev->irqs[0], &qec_interrupt,
			SA_SHIRQ, "QuadEther", (void *) qecp)) {
		printk(KERN_ERR "QuadEther: Can't register QEC master irq handler.\n");
		res = EAGAIN;
		goto qec_free_devs;
	}

	/* Report the QE channels. */
	for (i = 0; i < 4; i++) {
		printk(KERN_INFO "%s: QuadEthernet channel[%d] ", qe_devs[i]->name, i);
		for (j = 0; j < 6; j++)
			printk ("%2.2x%c",
				qe_devs[i]->dev_addr[j],
				j == 5 ? ' ': ':');
		printk("\n");
	}

	/* We are home free at this point, link the qe's into
	 * the master list for later driver exit.
	 */
	for (i = 0; i < 4; i++)
		qe_devs[i]->ifindex = dev_new_index();
	qecp->next_module = root_qec_dev;
	root_qec_dev = qecp;

	return 0;

qec_free_devs:
	for (i = 0; i < 4; i++) {
		if (qe_devs[i] != NULL) {
			if (qe_devs[i]->priv) {
				struct sunqe *qe = (struct sunqe *)qe_devs[i]->priv;

				if (qe->qcregs)
					sbus_iounmap(qe->qcregs, CREG_REG_SIZE);
				if (qe->mregs)
					sbus_iounmap(qe->mregs, MREGS_REG_SIZE);
				if (qe->qe_block != NULL)
					sbus_free_consistent(qe->qe_sdev,
							     PAGE_SIZE,
							     qe->qe_block,
							     qe->qblock_dvma);
				if (qe->buffers != NULL)
					sbus_free_consistent(qe->qe_sdev,
							     sizeof(struct sunqe_buffers),
							     qe->buffers,
							     qe->buffers_dvma);
				kfree(qe_devs[i]->priv);
			}
			kfree(qe_devs[i]);
		}
	}
	if (qecp != NULL) {
		if (qecp->gregs)
			sbus_iounmap(qecp->gregs, GLOB_REG_SIZE);
		kfree(qecp);
	}
	return res;
}

static int __init qec_match(struct sbus_dev *sdev)
{
	struct sbus_dev *sibling;
	int i;

	if (strcmp(sdev->prom_name, "qec") != 0)
		return 0;

	/* QEC can be parent of either QuadEthernet or BigMAC
	 * children.  Do not confuse this with qfe/SUNW,qfe
	 * which is a quad-happymeal card and handled by
	 * a different driver.
	 */
	sibling = sdev->child;
	for (i = 0; i < 4; i++) {
		if (sibling == NULL)
			return 0;
		if (strcmp(sibling->prom_name, "qe") != 0)
			return 0;
		sibling = sibling->next;
	}
	return 1;
}

static int __init qec_probe(void)
{
	struct net_device *dev = NULL;
	struct sbus_bus *bus;
	struct sbus_dev *sdev = 0;
	static int called = 0;
	int cards = 0, v;

	root_qec_dev = NULL;

	if (called)
		return -ENODEV;
	called++;

	for_each_sbus(bus) {
		for_each_sbusdev(sdev, bus) {
			if (cards)
				dev = NULL;

			if (qec_match(sdev)) {
				cards++;
				if ((v = qec_ether_init(dev, sdev)))
					return v;
			}
		}
	}
	if (!cards)
		return -ENODEV;
	return 0;
}

static void __exit qec_cleanup(void)
{
	struct sunqec *next_qec;
	int i;

	while (root_qec_dev) {
		next_qec = root_qec_dev->next_module;

		/* Release all four QE channels, then the QEC itself. */
		for (i = 0; i < 4; i++) {
			unregister_netdev(root_qec_dev->qes[i]->dev);
			sbus_iounmap(root_qec_dev->qes[i]->qcregs, CREG_REG_SIZE);
			sbus_iounmap(root_qec_dev->qes[i]->mregs, MREGS_REG_SIZE);
			sbus_free_consistent(root_qec_dev->qes[i]->qe_sdev,
					     PAGE_SIZE,
					     root_qec_dev->qes[i]->qe_block,
					     root_qec_dev->qes[i]->qblock_dvma);
			sbus_free_consistent(root_qec_dev->qes[i]->qe_sdev,
					     sizeof(struct sunqe_buffers),
					     root_qec_dev->qes[i]->buffers,
					     root_qec_dev->qes[i]->buffers_dvma);
			kfree(root_qec_dev->qes[i]->dev);
		}
		free_irq(root_qec_dev->qec_sdev->irqs[0], (void *)root_qec_dev);
		sbus_iounmap(root_qec_dev->gregs, GLOB_REG_SIZE);
		kfree(root_qec_dev);
		root_qec_dev = next_qec;
	}
}

module_init(qec_probe);
module_exit(qec_cleanup);
