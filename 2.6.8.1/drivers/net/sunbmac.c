/* $Id: sunbmac.c,v 1.30 2002/01/15 06:48:55 davem Exp $
 * sunbmac.c: Driver for Sparc BigMAC 100baseT ethernet adapters.
 *
 * Copyright (C) 1997, 1998, 1999, 2003 David S. Miller (davem@redhat.com)
 */

#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/in.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/ethtool.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>

#include <asm/auxio.h>
#include <asm/bitops.h>
#include <asm/byteorder.h>
#include <asm/dma.h>
#include <asm/idprom.h>
#include <asm/io.h>
#include <asm/openprom.h>
#include <asm/oplib.h>
#include <asm/pgtable.h>
#include <asm/sbus.h>
#include <asm/system.h>

#include "sunbmac.h"

static char version[] __initdata =
        "sunbmac.c:v2.0 24/Nov/03 David S. Miller (davem@redhat.com)\n";

#undef DEBUG_PROBE
#undef DEBUG_TX
#undef DEBUG_IRQ

#ifdef DEBUG_PROBE
#define DP(x)  printk x
#else
#define DP(x)
#endif

#ifdef DEBUG_TX
#define DTX(x)  printk x
#else
#define DTX(x)
#endif

#ifdef DEBUG_IRQ
#define DIRQ(x)  printk x
#else
#define DIRQ(x)
#endif

static struct bigmac *root_bigmac_dev;

#define DEFAULT_JAMSIZE    4 /* Toe jam */

#define QEC_RESET_TRIES 200

static int qec_global_reset(unsigned long gregs)
{
	int tries = QEC_RESET_TRIES;

	sbus_writel(GLOB_CTRL_RESET, gregs + GLOB_CTRL);
	while (--tries) {
		if (sbus_readl(gregs + GLOB_CTRL) & GLOB_CTRL_RESET) {
			udelay(20);
			continue;
		}
		break;
	}
	if (tries)
		return 0;
	printk(KERN_ERR "BigMAC: Cannot reset the QEC.\n");
	return -1;
}

static void qec_init(struct bigmac *bp)
{
	unsigned long gregs = bp->gregs;
	struct sbus_dev *qec_sdev = bp->qec_sdev;
	u8 bsizes = bp->bigmac_bursts;
	u32 regval;

	/* 64byte bursts do not work at the moment, do
	 * not even try to enable them.  -DaveM
	 */
	if (bsizes & DMA_BURST32)
		regval = GLOB_CTRL_B32;
	else
		regval = GLOB_CTRL_B16;
	sbus_writel(regval | GLOB_CTRL_BMODE, gregs + GLOB_CTRL);
	sbus_writel(GLOB_PSIZE_2048, gregs + GLOB_PSIZE);

	/* All of memsize is given to bigmac. */
	sbus_writel(qec_sdev->reg_addrs[1].reg_size,
		    gregs + GLOB_MSIZE);

	/* Half to the transmitter, half to the receiver. */
	sbus_writel(qec_sdev->reg_addrs[1].reg_size >> 1,
		    gregs + GLOB_TSIZE);
	sbus_writel(qec_sdev->reg_addrs[1].reg_size >> 1,
		    gregs + GLOB_RSIZE);
}

#define TX_RESET_TRIES     32
#define RX_RESET_TRIES     32

static void bigmac_tx_reset(unsigned long bregs)
{
	int tries = TX_RESET_TRIES;

	sbus_writel(0, bregs + BMAC_TXCFG);

	/* The fifo threshold bit is read-only and does
	 * not clear.  -DaveM
	 */
	while ((sbus_readl(bregs + BMAC_TXCFG) & ~(BIGMAC_TXCFG_FIFO)) != 0 &&
	       --tries != 0)
		udelay(20);

	if (!tries) {
		printk(KERN_ERR "BIGMAC: Transmitter will not reset.\n");
		printk(KERN_ERR "BIGMAC: tx_cfg is %08x\n",
		       sbus_readl(bregs + BMAC_TXCFG));
	}
}

static void bigmac_rx_reset(unsigned long bregs)
{
	int tries = RX_RESET_TRIES;

	sbus_writel(0, bregs + BMAC_RXCFG);
	while (sbus_readl(bregs + BMAC_RXCFG) && --tries)
		udelay(20);

	if (!tries) {
		printk(KERN_ERR "BIGMAC: Receiver will not reset.\n");
		printk(KERN_ERR "BIGMAC: rx_cfg is %08x\n",
		       sbus_readl(bregs + BMAC_RXCFG));
	}
}

/* Reset the transmitter and receiver. */
static void bigmac_stop(struct bigmac *bp)
{
	bigmac_tx_reset(bp->bregs);
	bigmac_rx_reset(bp->bregs);
}

static void bigmac_get_counters(struct bigmac *bp, unsigned long bregs)
{
	struct net_device_stats *stats = &bp->enet_stats;

	stats->rx_crc_errors += sbus_readl(bregs + BMAC_RCRCECTR);
	sbus_writel(0, bregs + BMAC_RCRCECTR);

	stats->rx_frame_errors += sbus_readl(bregs + BMAC_UNALECTR);
	sbus_writel(0, bregs + BMAC_UNALECTR);

	stats->rx_length_errors += sbus_readl(bregs + BMAC_GLECTR);
	sbus_writel(0, bregs + BMAC_GLECTR);

	stats->tx_aborted_errors += sbus_readl(bregs + BMAC_EXCTR);

	stats->collisions +=
		(sbus_readl(bregs + BMAC_EXCTR) +
		 sbus_readl(bregs + BMAC_LTCTR));
	sbus_writel(0, bregs + BMAC_EXCTR);
	sbus_writel(0, bregs + BMAC_LTCTR);
}

static void bigmac_clean_rings(struct bigmac *bp)
{
	int i;

	for (i = 0; i < RX_RING_SIZE; i++) {
		if (bp->rx_skbs[i] != NULL) {
			dev_kfree_skb_any(bp->rx_skbs[i]);
			bp->rx_skbs[i] = NULL;
		}
	}

	for (i = 0; i < TX_RING_SIZE; i++) {
		if (bp->tx_skbs[i] != NULL) {
			dev_kfree_skb_any(bp->tx_skbs[i]);
			bp->tx_skbs[i] = NULL;
		}
	}
}

static void bigmac_init_rings(struct bigmac *bp, int from_irq)
{
	struct bmac_init_block *bb = bp->bmac_block;
	struct net_device *dev = bp->dev;
	int i, gfp_flags = GFP_KERNEL;

	if (from_irq || in_interrupt())
		gfp_flags = GFP_ATOMIC;

	bp->rx_new = bp->rx_old = bp->tx_new = bp->tx_old = 0;

	/* Free any skippy bufs left around in the rings. */
	bigmac_clean_rings(bp);

	/* Now get new skbufs for the receive ring. */
	for (i = 0; i < RX_RING_SIZE; i++) {
		struct sk_buff *skb;

		skb = big_mac_alloc_skb(RX_BUF_ALLOC_SIZE, gfp_flags);
		if (!skb)
			continue;

		bp->rx_skbs[i] = skb;
		skb->dev = dev;

		/* Because we reserve afterwards. */
		skb_put(skb, ETH_FRAME_LEN);
		skb_reserve(skb, 34);

		bb->be_rxd[i].rx_addr =
			sbus_map_single(bp->bigmac_sdev, skb->data,
					RX_BUF_ALLOC_SIZE - 34,
					SBUS_DMA_FROMDEVICE);
		bb->be_rxd[i].rx_flags =
			(RXD_OWN | ((RX_BUF_ALLOC_SIZE - 34) & RXD_LENGTH));
	}

	for (i = 0; i < TX_RING_SIZE; i++)
		bb->be_txd[i].tx_flags = bb->be_txd[i].tx_addr = 0;
}

#define MGMT_CLKON  (MGMT_PAL_INT_MDIO|MGMT_PAL_EXT_MDIO|MGMT_PAL_OENAB|MGMT_PAL_DCLOCK)
#define MGMT_CLKOFF (MGMT_PAL_INT_MDIO|MGMT_PAL_EXT_MDIO|MGMT_PAL_OENAB)

static void idle_transceiver(unsigned long tregs)
{
	int i = 20;

	while (i--) {
		sbus_writel(MGMT_CLKOFF, tregs + TCVR_MPAL);
		sbus_readl(tregs + TCVR_MPAL);
		sbus_writel(MGMT_CLKON, tregs + TCVR_MPAL);
		sbus_readl(tregs + TCVR_MPAL);
	}
}

static void write_tcvr_bit(struct bigmac *bp, unsigned long tregs, int bit)
{
	if (bp->tcvr_type == internal) {
		bit = (bit & 1) << 3;
		sbus_writel(bit | (MGMT_PAL_OENAB | MGMT_PAL_EXT_MDIO),
			    tregs + TCVR_MPAL);
		sbus_readl(tregs + TCVR_MPAL);
		sbus_writel(bit | MGMT_PAL_OENAB | MGMT_PAL_EXT_MDIO | MGMT_PAL_DCLOCK,
			    tregs + TCVR_MPAL);
		sbus_readl(tregs + TCVR_MPAL);
	} else if (bp->tcvr_type == external) {
		bit = (bit & 1) << 2;
		sbus_writel(bit | MGMT_PAL_INT_MDIO | MGMT_PAL_OENAB,
			    tregs + TCVR_MPAL);
		sbus_readl(tregs + TCVR_MPAL);
		sbus_writel(bit | MGMT_PAL_INT_MDIO | MGMT_PAL_OENAB | MGMT_PAL_DCLOCK,
			    tregs + TCVR_MPAL);
		sbus_readl(tregs + TCVR_MPAL);
	} else {
		printk(KERN_ERR "write_tcvr_bit: No transceiver type known!\n");
	}
}

static int read_tcvr_bit(struct bigmac *bp, unsigned long tregs)
{
	int retval = 0;

	if (bp->tcvr_type == internal) {
		sbus_writel(MGMT_PAL_EXT_MDIO, tregs + TCVR_MPAL);
		sbus_readl(tregs + TCVR_MPAL);
		sbus_writel(MGMT_PAL_EXT_MDIO | MGMT_PAL_DCLOCK,
			    tregs + TCVR_MPAL);
		sbus_readl(tregs + TCVR_MPAL);
		retval = (sbus_readl(tregs + TCVR_MPAL) & MGMT_PAL_INT_MDIO) >> 3;
	} else if (bp->tcvr_type == external) {
		sbus_writel(MGMT_PAL_INT_MDIO, tregs + TCVR_MPAL);
		sbus_readl(tregs + TCVR_MPAL);
		sbus_writel(MGMT_PAL_INT_MDIO | MGMT_PAL_DCLOCK, tregs + TCVR_MPAL);
		sbus_readl(tregs + TCVR_MPAL);
		retval = (sbus_readl(tregs + TCVR_MPAL) & MGMT_PAL_EXT_MDIO) >> 2;
	} else {
		printk(KERN_ERR "read_tcvr_bit: No transceiver type known!\n");
	}
	return retval;
}

static int read_tcvr_bit2(struct bigmac *bp, unsigned long tregs)
{
	int retval = 0;

	if (bp->tcvr_type == internal) {
		sbus_writel(MGMT_PAL_EXT_MDIO, tregs + TCVR_MPAL);
		sbus_readl(tregs + TCVR_MPAL);
		retval = (sbus_readl(tregs + TCVR_MPAL) & MGMT_PAL_INT_MDIO) >> 3;
		sbus_writel(MGMT_PAL_EXT_MDIO | MGMT_PAL_DCLOCK, tregs + TCVR_MPAL);
		sbus_readl(tregs + TCVR_MPAL);
	} else if (bp->tcvr_type == external) {
		sbus_writel(MGMT_PAL_INT_MDIO, tregs + TCVR_MPAL);
		sbus_readl(tregs + TCVR_MPAL);
		retval = (sbus_readl(tregs + TCVR_MPAL) & MGMT_PAL_EXT_MDIO) >> 2;
		sbus_writel(MGMT_PAL_INT_MDIO | MGMT_PAL_DCLOCK, tregs + TCVR_MPAL);
		sbus_readl(tregs + TCVR_MPAL);
	} else {
		printk(KERN_ERR "read_tcvr_bit2: No transceiver type known!\n");
	}
	return retval;
}

static void put_tcvr_byte(struct bigmac *bp,
			  unsigned long tregs,
			  unsigned int byte)
{
	int shift = 4;

	do {
		write_tcvr_bit(bp, tregs, ((byte >> shift) & 1));
		shift -= 1;
	} while (shift >= 0);
}

static void bigmac_tcvr_write(struct bigmac *bp, unsigned long tregs,
			      int reg, unsigned short val)
{
	int shift;

	reg &= 0xff;
	val &= 0xffff;
	switch(bp->tcvr_type) {
	case internal:
	case external:
		break;

	default:
		printk(KERN_ERR "bigmac_tcvr_read: Whoops, no known transceiver type.\n");
		return;
	};

	idle_transceiver(tregs);
	write_tcvr_bit(bp, tregs, 0);
	write_tcvr_bit(bp, tregs, 1);
	write_tcvr_bit(bp, tregs, 0);
	write_tcvr_bit(bp, tregs, 1);

	put_tcvr_byte(bp, tregs,
		      ((bp->tcvr_type == internal) ?
		       BIGMAC_PHY_INTERNAL : BIGMAC_PHY_EXTERNAL));

	put_tcvr_byte(bp, tregs, reg);

	write_tcvr_bit(bp, tregs, 1);
	write_tcvr_bit(bp, tregs, 0);

	shift = 15;
	do {
		write_tcvr_bit(bp, tregs, (val >> shift) & 1);
		shift -= 1;
	} while (shift >= 0);
}

static unsigned short bigmac_tcvr_read(struct bigmac *bp,
				       unsigned long tregs,
				       int reg)
{
	unsigned short retval = 0;

	reg &= 0xff;
	switch(bp->tcvr_type) {
	case internal:
	case external:
		break;

	default:
		printk(KERN_ERR "bigmac_tcvr_read: Whoops, no known transceiver type.\n");
		return 0xffff;
	};

	idle_transceiver(tregs);
	write_tcvr_bit(bp, tregs, 0);
	write_tcvr_bit(bp, tregs, 1);
	write_tcvr_bit(bp, tregs, 1);
	write_tcvr_bit(bp, tregs, 0);

	put_tcvr_byte(bp, tregs,
		      ((bp->tcvr_type == internal) ?
		       BIGMAC_PHY_INTERNAL : BIGMAC_PHY_EXTERNAL));

	put_tcvr_byte(bp, tregs, reg);

	if (bp->tcvr_type == external) {
		int shift = 15;

		(void) read_tcvr_bit2(bp, tregs);
		(void) read_tcvr_bit2(bp, tregs);

		do {
			int tmp;

			tmp = read_tcvr_bit2(bp, tregs);
			retval |= ((tmp & 1) << shift);
			shift -= 1;
		} while (shift >= 0);

		(void) read_tcvr_bit2(bp, tregs);
		(void) read_tcvr_bit2(bp, tregs);
		(void) read_tcvr_bit2(bp, tregs);
	} else {
		int shift = 15;

		(void) read_tcvr_bit(bp, tregs);
		(void) read_tcvr_bit(bp, tregs);

		do {
			int tmp;

			tmp = read_tcvr_bit(bp, tregs);
			retval |= ((tmp & 1) << shift);
			shift -= 1;
		} while (shift >= 0);

		(void) read_tcvr_bit(bp, tregs);
		(void) read_tcvr_bit(bp, tregs);
		(void) read_tcvr_bit(bp, tregs);
	}
	return retval;
}

static void bigmac_tcvr_init(struct bigmac *bp)
{
	unsigned long tregs = bp->tregs;
	u32 mpal;

	idle_transceiver(tregs);
	sbus_writel(MGMT_PAL_INT_MDIO | MGMT_PAL_EXT_MDIO | MGMT_PAL_DCLOCK,
		    tregs + TCVR_MPAL);
	sbus_readl(tregs + TCVR_MPAL);

	/* Only the bit for the present transceiver (internal or
	 * external) will stick, set them both and see what stays.
	 */
	sbus_writel(MGMT_PAL_INT_MDIO | MGMT_PAL_EXT_MDIO, tregs + TCVR_MPAL);
	sbus_readl(tregs + TCVR_MPAL);
	udelay(20);

	mpal = sbus_readl(tregs + TCVR_MPAL);
	if (mpal & MGMT_PAL_EXT_MDIO) {
		bp->tcvr_type = external;
		sbus_writel(~(TCVR_PAL_EXTLBACK | TCVR_PAL_MSENSE | TCVR_PAL_LTENABLE),
			    tregs + TCVR_TPAL);
		sbus_readl(tregs + TCVR_TPAL);
	} else if (mpal & MGMT_PAL_INT_MDIO) {
		bp->tcvr_type = internal;
		sbus_writel(~(TCVR_PAL_SERIAL | TCVR_PAL_EXTLBACK |
			      TCVR_PAL_MSENSE | TCVR_PAL_LTENABLE),
			    tregs + TCVR_TPAL);
		sbus_readl(tregs + TCVR_TPAL);
	} else {
		printk(KERN_ERR "BIGMAC: AIEEE, neither internal nor "
		       "external MDIO available!\n");
		printk(KERN_ERR "BIGMAC: mgmt_pal[%08x] tcvr_pal[%08x]\n",
		       sbus_readl(tregs + TCVR_MPAL),
		       sbus_readl(tregs + TCVR_TPAL));
	}
}

static int bigmac_init(struct bigmac *, int);

static int try_next_permutation(struct bigmac *bp, unsigned long tregs)
{
	if (bp->sw_bmcr & BMCR_SPEED100) {
		int timeout;

		/* Reset the PHY. */
		bp->sw_bmcr	= (BMCR_ISOLATE | BMCR_PDOWN | BMCR_LOOPBACK);
		bigmac_tcvr_write(bp, tregs, BIGMAC_BMCR, bp->sw_bmcr);
		bp->sw_bmcr	= (BMCR_RESET);
		bigmac_tcvr_write(bp, tregs, BIGMAC_BMCR, bp->sw_bmcr);

		timeout = 64;
		while (--timeout) {
			bp->sw_bmcr = bigmac_tcvr_read(bp, tregs, BIGMAC_BMCR);
			if ((bp->sw_bmcr & BMCR_RESET) == 0)
				break;
			udelay(20);
		}
		if (timeout == 0)
			printk(KERN_ERR "%s: PHY reset failed.\n", bp->dev->name);

		bp->sw_bmcr = bigmac_tcvr_read(bp, tregs, BIGMAC_BMCR);

		/* Now we try 10baseT. */
		bp->sw_bmcr &= ~(BMCR_SPEED100);
		bigmac_tcvr_write(bp, tregs, BIGMAC_BMCR, bp->sw_bmcr);
		return 0;
	}

	/* We've tried them all. */
	return -1;
}

static void bigmac_timer(unsigned long data)
{
	struct bigmac *bp = (struct bigmac *) data;
	unsigned long tregs = bp->tregs;
	int restart_timer = 0;

	bp->timer_ticks++;
	if (bp->timer_state == ltrywait) {
		bp->sw_bmsr = bigmac_tcvr_read(bp, tregs, BIGMAC_BMSR);
		bp->sw_bmcr = bigmac_tcvr_read(bp, tregs, BIGMAC_BMCR);
		if (bp->sw_bmsr & BMSR_LSTATUS) {
			printk(KERN_INFO "%s: Link is now up at %s.\n",
			       bp->dev->name,
			       (bp->sw_bmcr & BMCR_SPEED100) ?
			       "100baseT" : "10baseT");
			bp->timer_state = asleep;
			restart_timer = 0;
		} else {
			if (bp->timer_ticks >= 4) {
				int ret;

				ret = try_next_permutation(bp, tregs);
				if (ret == -1) {
					printk(KERN_ERR "%s: Link down, cable problem?\n",
					       bp->dev->name);
					ret = bigmac_init(bp, 0);
					if (ret) {
						printk(KERN_ERR "%s: Error, cannot re-init the "
						       "BigMAC.\n", bp->dev->name);
					}
					return;
				}
				bp->timer_ticks = 0;
				restart_timer = 1;
			} else {
				restart_timer = 1;
			}
		}
	} else {
		/* Can't happens.... */
		printk(KERN_ERR "%s: Aieee, link timer is asleep but we got one anyways!\n",
		       bp->dev->name);
		restart_timer = 0;
		bp->timer_ticks = 0;
		bp->timer_state = asleep; /* foo on you */
	}

	if (restart_timer != 0) {
		bp->bigmac_timer.expires = jiffies + ((12 * HZ)/10); /* 1.2 sec. */
		add_timer(&bp->bigmac_timer);
	}
}

/* Well, really we just force the chip into 100baseT then
 * 10baseT, each time checking for a link status.
 */
static void bigmac_begin_auto_negotiation(struct bigmac *bp)
{
	unsigned long tregs = bp->tregs;
	int timeout;

	/* Grab new software copies of PHY registers. */
	bp->sw_bmsr	= bigmac_tcvr_read(bp, tregs, BIGMAC_BMSR);
	bp->sw_bmcr	= bigmac_tcvr_read(bp, tregs, BIGMAC_BMCR);

	/* Reset the PHY. */
	bp->sw_bmcr	= (BMCR_ISOLATE | BMCR_PDOWN | BMCR_LOOPBACK);
	bigmac_tcvr_write(bp, tregs, BIGMAC_BMCR, bp->sw_bmcr);
	bp->sw_bmcr	= (BMCR_RESET);
	bigmac_tcvr_write(bp, tregs, BIGMAC_BMCR, bp->sw_bmcr);

	timeout = 64;
	while (--timeout) {
		bp->sw_bmcr = bigmac_tcvr_read(bp, tregs, BIGMAC_BMCR);
		if ((bp->sw_bmcr & BMCR_RESET) == 0)
			break;
		udelay(20);
	}
	if (timeout == 0)
		printk(KERN_ERR "%s: PHY reset failed.\n", bp->dev->name);

	bp->sw_bmcr = bigmac_tcvr_read(bp, tregs, BIGMAC_BMCR);

	/* First we try 100baseT. */
	bp->sw_bmcr |= BMCR_SPEED100;
	bigmac_tcvr_write(bp, tregs, BIGMAC_BMCR, bp->sw_bmcr);

	bp->timer_state = ltrywait;
	bp->timer_ticks = 0;
	bp->bigmac_timer.expires = jiffies + (12 * HZ) / 10;
	bp->bigmac_timer.data = (unsigned long) bp;
	bp->bigmac_timer.function = &bigmac_timer;
	add_timer(&bp->bigmac_timer);
}

static int bigmac_init(struct bigmac *bp, int from_irq)
{
	unsigned long gregs        = bp->gregs;
	unsigned long cregs        = bp->creg;
	unsigned long bregs        = bp->bregs;
	unsigned char *e = &bp->dev->dev_addr[0];

	/* Latch current counters into statistics. */
	bigmac_get_counters(bp, bregs);

	/* Reset QEC. */
	qec_global_reset(gregs);

	/* Init QEC. */
	qec_init(bp);

	/* Alloc and reset the tx/rx descriptor chains. */
	bigmac_init_rings(bp, from_irq);

	/* Initialize the PHY. */
	bigmac_tcvr_init(bp);

	/* Stop transmitter and receiver. */
	bigmac_stop(bp);

	/* Set hardware ethernet address. */
	sbus_writel(((e[4] << 8) | e[5]), bregs + BMAC_MACADDR2);
	sbus_writel(((e[2] << 8) | e[3]), bregs + BMAC_MACADDR1);
	sbus_writel(((e[0] << 8) | e[1]), bregs + BMAC_MACADDR0);

	/* Clear the hash table until mc upload occurs. */
	sbus_writel(0, bregs + BMAC_HTABLE3);
	sbus_writel(0, bregs + BMAC_HTABLE2);
	sbus_writel(0, bregs + BMAC_HTABLE1);
	sbus_writel(0, bregs + BMAC_HTABLE0);

	/* Enable Big Mac hash table filter. */
	sbus_writel(BIGMAC_RXCFG_HENABLE | BIGMAC_RXCFG_FIFO,
		    bregs + BMAC_RXCFG);
	udelay(20);

	/* Ok, configure the Big Mac transmitter. */
	sbus_writel(BIGMAC_TXCFG_FIFO, bregs + BMAC_TXCFG);

	/* The HME docs recommend to use the 10LSB of our MAC here. */
	sbus_writel(((e[5] | e[4] << 8) & 0x3ff),
		    bregs + BMAC_RSEED);

	/* Enable the output drivers no matter what. */
	sbus_writel(BIGMAC_XCFG_ODENABLE | BIGMAC_XCFG_RESV,
		    bregs + BMAC_XIFCFG);

	/* Tell the QEC where the ring descriptors are. */
	sbus_writel(bp->bblock_dvma + bib_offset(be_rxd, 0),
		    cregs + CREG_RXDS);
	sbus_writel(bp->bblock_dvma + bib_offset(be_txd, 0),
		    cregs + CREG_TXDS);

	/* Setup the FIFO pointers into QEC local memory. */
	sbus_writel(0, cregs + CREG_RXRBUFPTR);
	sbus_writel(0, cregs + CREG_RXWBUFPTR);
	sbus_writel(sbus_readl(gregs + GLOB_RSIZE),
		    cregs + CREG_TXRBUFPTR);
	sbus_writel(sbus_readl(gregs + GLOB_RSIZE),
		    cregs + CREG_TXWBUFPTR);

	/* Tell bigmac what interrupts we don't want to hear about. */
	sbus_writel(BIGMAC_IMASK_GOTFRAME | BIGMAC_IMASK_SENTFRAME,
		    bregs + BMAC_IMASK);

	/* Enable the various other irq's. */
	sbus_writel(0, cregs + CREG_RIMASK);
	sbus_writel(0, cregs + CREG_TIMASK);
	sbus_writel(0, cregs + CREG_QMASK);
	sbus_writel(0, cregs + CREG_BMASK);

	/* Set jam size to a reasonable default. */
	sbus_writel(DEFAULT_JAMSIZE, bregs + BMAC_JSIZE);

	/* Clear collision counter. */
	sbus_writel(0, cregs + CREG_CCNT);

	/* Enable transmitter and receiver. */
	sbus_writel(sbus_readl(bregs + BMAC_TXCFG) | BIGMAC_TXCFG_ENABLE,
		    bregs + BMAC_TXCFG);
	sbus_writel(sbus_readl(bregs + BMAC_RXCFG) | BIGMAC_RXCFG_ENABLE,
		    bregs + BMAC_RXCFG);

	/* Ok, start detecting link speed/duplex. */
	bigmac_begin_auto_negotiation(bp);

	/* Success. */
	return 0;
}

/* Error interrupts get sent here. */
static void bigmac_is_medium_rare(struct bigmac *bp, u32 qec_status, u32 bmac_status)
{
	printk(KERN_ERR "bigmac_is_medium_rare: ");
	if (qec_status & (GLOB_STAT_ER | GLOB_STAT_BM)) {
		if (qec_status & GLOB_STAT_ER)
			printk("QEC_ERROR, ");
		if (qec_status & GLOB_STAT_BM)
			printk("QEC_BMAC_ERROR, ");
	}
	if (bmac_status & CREG_STAT_ERRORS) {
		if (bmac_status & CREG_STAT_BERROR)
			printk("BMAC_ERROR, ");
		if (bmac_status & CREG_STAT_TXDERROR)
			printk("TXD_ERROR, ");
		if (bmac_status & CREG_STAT_TXLERR)
			printk("TX_LATE_ERROR, ");
		if (bmac_status & CREG_STAT_TXPERR)
			printk("TX_PARITY_ERROR, ");
		if (bmac_status & CREG_STAT_TXSERR)
			printk("TX_SBUS_ERROR, ");

		if (bmac_status & CREG_STAT_RXDROP)
			printk("RX_DROP_ERROR, ");

		if (bmac_status & CREG_STAT_RXSMALL)
			printk("RX_SMALL_ERROR, ");
		if (bmac_status & CREG_STAT_RXLERR)
			printk("RX_LATE_ERROR, ");
		if (bmac_status & CREG_STAT_RXPERR)
			printk("RX_PARITY_ERROR, ");
		if (bmac_status & CREG_STAT_RXSERR)
			printk("RX_SBUS_ERROR, ");
	}

	printk(" RESET\n");
	bigmac_init(bp, 1);
}

/* BigMAC transmit complete service routines. */
static void bigmac_tx(struct bigmac *bp)
{
	struct be_txd *txbase = &bp->bmac_block->be_txd[0];
	struct net_device *dev = bp->dev;
	int elem;

	spin_lock(&bp->lock);

	elem = bp->tx_old;
	DTX(("bigmac_tx: tx_old[%d] ", elem));
	while (elem != bp->tx_new) {
		struct sk_buff *skb;
		struct be_txd *this = &txbase[elem];

		DTX(("this(%p) [flags(%08x)addr(%08x)]",
		     this, this->tx_flags, this->tx_addr));

		if (this->tx_flags & TXD_OWN)
			break;
		skb = bp->tx_skbs[elem];
		bp->enet_stats.tx_packets++;
		bp->enet_stats.tx_bytes += skb->len;
		sbus_unmap_single(bp->bigmac_sdev,
				  this->tx_addr, skb->len,
				  SBUS_DMA_TODEVICE);

		DTX(("skb(%p) ", skb));
		bp->tx_skbs[elem] = NULL;
		dev_kfree_skb_irq(skb);

		elem = NEXT_TX(elem);
	}
	DTX((" DONE, tx_old=%d\n", elem));
	bp->tx_old = elem;

	if (netif_queue_stopped(dev) &&
	    TX_BUFFS_AVAIL(bp) > 0)
		netif_wake_queue(bp->dev);

	spin_unlock(&bp->lock);
}

/* BigMAC receive complete service routines. */
static void bigmac_rx(struct bigmac *bp)
{
	struct be_rxd *rxbase = &bp->bmac_block->be_rxd[0];
	struct be_rxd *this;
	int elem = bp->rx_new, drops = 0;
	u32 flags;

	this = &rxbase[elem];
	while (!((flags = this->rx_flags) & RXD_OWN)) {
		struct sk_buff *skb;
		int len = (flags & RXD_LENGTH); /* FCS not included */

		/* Check for errors. */
		if (len < ETH_ZLEN) {
			bp->enet_stats.rx_errors++;
			bp->enet_stats.rx_length_errors++;

	drop_it:
			/* Return it to the BigMAC. */
			bp->enet_stats.rx_dropped++;
			this->rx_flags =
				(RXD_OWN | ((RX_BUF_ALLOC_SIZE - 34) & RXD_LENGTH));
			goto next;
		}
		skb = bp->rx_skbs[elem];
		if (len > RX_COPY_THRESHOLD) {
			struct sk_buff *new_skb;

			/* Now refill the entry, if we can. */
			new_skb = big_mac_alloc_skb(RX_BUF_ALLOC_SIZE, GFP_ATOMIC);
			if (new_skb == NULL) {
				drops++;
				goto drop_it;
			}
			sbus_unmap_single(bp->bigmac_sdev,
					  this->rx_addr,
					  RX_BUF_ALLOC_SIZE - 34,
					  SBUS_DMA_FROMDEVICE);
			bp->rx_skbs[elem] = new_skb;
			new_skb->dev = bp->dev;
			skb_put(new_skb, ETH_FRAME_LEN);
			skb_reserve(new_skb, 34);
			this->rx_addr = sbus_map_single(bp->bigmac_sdev,
							new_skb->data,
							RX_BUF_ALLOC_SIZE - 34,
							SBUS_DMA_FROMDEVICE);
			this->rx_flags =
				(RXD_OWN | ((RX_BUF_ALLOC_SIZE - 34) & RXD_LENGTH));

			/* Trim the original skb for the netif. */
			skb_trim(skb, len);
		} else {
			struct sk_buff *copy_skb = dev_alloc_skb(len + 2);

			if (copy_skb == NULL) {
				drops++;
				goto drop_it;
			}
			copy_skb->dev = bp->dev;
			skb_reserve(copy_skb, 2);
			skb_put(copy_skb, len);
			sbus_dma_sync_single_for_cpu(bp->bigmac_sdev,
						     this->rx_addr, len,
						     SBUS_DMA_FROMDEVICE);
			eth_copy_and_sum(copy_skb, (unsigned char *)skb->data, len, 0);
			sbus_dma_sync_single_for_device(bp->bigmac_sdev,
							this->rx_addr, len,
							SBUS_DMA_FROMDEVICE);

			/* Reuse original ring buffer. */
			this->rx_flags =
				(RXD_OWN | ((RX_BUF_ALLOC_SIZE - 34) & RXD_LENGTH));

			skb = copy_skb;
		}

		/* No checksums done by the BigMAC ;-( */
		skb->protocol = eth_type_trans(skb, bp->dev);
		netif_rx(skb);
		bp->dev->last_rx = jiffies;
		bp->enet_stats.rx_packets++;
		bp->enet_stats.rx_bytes += len;
	next:
		elem = NEXT_RX(elem);
		this = &rxbase[elem];
	}
	bp->rx_new = elem;
	if (drops)
		printk(KERN_NOTICE "%s: Memory squeeze, deferring packet.\n", bp->dev->name);
}

static irqreturn_t bigmac_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	struct bigmac *bp = (struct bigmac *) dev_id;
	u32 qec_status, bmac_status;

	DIRQ(("bigmac_interrupt: "));

	/* Latch status registers now. */
	bmac_status = sbus_readl(bp->creg + CREG_STAT);
	qec_status = sbus_readl(bp->gregs + GLOB_STAT);

	DIRQ(("qec_status=%08x bmac_status=%08x\n", qec_status, bmac_status));
	if ((qec_status & (GLOB_STAT_ER | GLOB_STAT_BM)) ||
	   (bmac_status & CREG_STAT_ERRORS))
		bigmac_is_medium_rare(bp, qec_status, bmac_status);

	if (bmac_status & CREG_STAT_TXIRQ)
		bigmac_tx(bp);

	if (bmac_status & CREG_STAT_RXIRQ)
		bigmac_rx(bp);

	return IRQ_HANDLED;
}

static int bigmac_open(struct net_device *dev)
{
	struct bigmac *bp = (struct bigmac *) dev->priv;
	int ret;

	ret = request_irq(dev->irq, &bigmac_interrupt, SA_SHIRQ, dev->name, bp);
	if (ret) {
		printk(KERN_ERR "BIGMAC: Can't order irq %d to go.\n", dev->irq);
		return ret;
	}
	init_timer(&bp->bigmac_timer);
	ret = bigmac_init(bp, 0);
	if (ret)
		free_irq(dev->irq, bp);
	return ret;
}

static int bigmac_close(struct net_device *dev)
{
	struct bigmac *bp = (struct bigmac *) dev->priv;

	del_timer(&bp->bigmac_timer);
	bp->timer_state = asleep;
	bp->timer_ticks = 0;

	bigmac_stop(bp);
	bigmac_clean_rings(bp);
	free_irq(dev->irq, bp);
	return 0;
}

static void bigmac_tx_timeout(struct net_device *dev)
{
	struct bigmac *bp = (struct bigmac *) dev->priv;

	bigmac_init(bp, 0);
	netif_wake_queue(dev);
}

/* Put a packet on the wire. */
static int bigmac_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct bigmac *bp = (struct bigmac *) dev->priv;
	int len, entry;
	u32 mapping;

	len = skb->len;
	mapping = sbus_map_single(bp->bigmac_sdev, skb->data, len, SBUS_DMA_TODEVICE);

	/* Avoid a race... */
	spin_lock_irq(&bp->lock);
	entry = bp->tx_new;
	DTX(("bigmac_start_xmit: len(%d) entry(%d)\n", len, entry));
	bp->bmac_block->be_txd[entry].tx_flags = TXD_UPDATE;
	bp->tx_skbs[entry] = skb;
	bp->bmac_block->be_txd[entry].tx_addr = mapping;
	bp->bmac_block->be_txd[entry].tx_flags =
		(TXD_OWN | TXD_SOP | TXD_EOP | (len & TXD_LENGTH));
	bp->tx_new = NEXT_TX(entry);
	if (TX_BUFFS_AVAIL(bp) <= 0)
		netif_stop_queue(dev);
	spin_unlock_irq(&bp->lock);

	/* Get it going. */
	sbus_writel(CREG_CTRL_TWAKEUP, bp->creg + CREG_CTRL);


	dev->trans_start = jiffies;

	return 0;
}

static struct net_device_stats *bigmac_get_stats(struct net_device *dev)
{
	struct bigmac *bp = (struct bigmac *) dev->priv;

	bigmac_get_counters(bp, bp->bregs);
	return &bp->enet_stats;
}

static void bigmac_set_multicast(struct net_device *dev)
{
	struct bigmac *bp = (struct bigmac *) dev->priv;
	unsigned long bregs = bp->bregs;
	struct dev_mc_list *dmi = dev->mc_list;
	char *addrs;
	int i;
	u32 tmp, crc;

	/* Disable the receiver.  The bit self-clears when
	 * the operation is complete.
	 */
	tmp = sbus_readl(bregs + BMAC_RXCFG);
	tmp &= ~(BIGMAC_RXCFG_ENABLE);
	sbus_writel(tmp, bregs + BMAC_RXCFG);
	while ((sbus_readl(bregs + BMAC_RXCFG) & BIGMAC_RXCFG_ENABLE) != 0)
		udelay(20);

	if ((dev->flags & IFF_ALLMULTI) || (dev->mc_count > 64)) {
		sbus_writel(0xffff, bregs + BMAC_HTABLE0);
		sbus_writel(0xffff, bregs + BMAC_HTABLE1);
		sbus_writel(0xffff, bregs + BMAC_HTABLE2);
		sbus_writel(0xffff, bregs + BMAC_HTABLE3);
	} else if (dev->flags & IFF_PROMISC) {
		tmp = sbus_readl(bregs + BMAC_RXCFG);
		tmp |= BIGMAC_RXCFG_PMISC;
		sbus_writel(tmp, bregs + BMAC_RXCFG);
	} else {
		u16 hash_table[4];

		for (i = 0; i < 4; i++)
			hash_table[i] = 0;

		for (i = 0; i < dev->mc_count; i++) {
			addrs = dmi->dmi_addr;
			dmi = dmi->next;

			if (!(*addrs & 1))
				continue;

			crc = ether_crc_le(6, addrs);
			crc >>= 26;
			hash_table[crc >> 4] |= 1 << (crc & 0xf);
		}
		sbus_writel(hash_table[0], bregs + BMAC_HTABLE0);
		sbus_writel(hash_table[1], bregs + BMAC_HTABLE1);
		sbus_writel(hash_table[2], bregs + BMAC_HTABLE2);
		sbus_writel(hash_table[3], bregs + BMAC_HTABLE3);
	}

	/* Re-enable the receiver. */
	tmp = sbus_readl(bregs + BMAC_RXCFG);
	tmp |= BIGMAC_RXCFG_ENABLE;
	sbus_writel(tmp, bregs + BMAC_RXCFG);
}

/* Ethtool support... */
static void bigmac_get_drvinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
	struct bigmac *bp = dev->priv;

	strcpy(info->driver, "sunbmac");
	strcpy(info->version, "2.0");
	sprintf(info->bus_info, "SBUS:%d",
		bp->qec_sdev->slot);
}

static u32 bigmac_get_link(struct net_device *dev)
{
	struct bigmac *bp = dev->priv;

	spin_lock_irq(&bp->lock);
	bp->sw_bmsr = bigmac_tcvr_read(bp, bp->tregs, BIGMAC_BMSR);
	spin_unlock_irq(&bp->lock);

	return (bp->sw_bmsr & BMSR_LSTATUS);
}

static struct ethtool_ops bigmac_ethtool_ops = {
	.get_drvinfo		= bigmac_get_drvinfo,
	.get_link		= bigmac_get_link,
};

static int __init bigmac_ether_init(struct sbus_dev *qec_sdev)
{
	struct net_device *dev;
	static int version_printed;
	struct bigmac *bp;
	u8 bsizes, bsizes_more;
	int i;

	/* Get a new device struct for this interface. */
	dev = alloc_etherdev(sizeof(struct bigmac));
	if (!dev)
		return -ENOMEM;
	SET_MODULE_OWNER(dev);

	if (version_printed++ == 0)
		printk(KERN_INFO "%s", version);

	dev->base_addr = (long) qec_sdev;
	for (i = 0; i < 6; i++)
		dev->dev_addr[i] = idprom->id_ethaddr[i];

	/* Setup softc, with backpointers to QEC and BigMAC SBUS device structs. */
	bp = dev->priv;
	bp->qec_sdev = qec_sdev;
	bp->bigmac_sdev = qec_sdev->child;

	spin_lock_init(&bp->lock);

	/* Verify the registers we expect, are actually there. */
	if ((bp->bigmac_sdev->num_registers != 3) ||
	   (bp->qec_sdev->num_registers != 2)) {
		printk(KERN_ERR "BIGMAC: Device does not have 2 and 3 regs, it has %d and %d.\n",
		       bp->qec_sdev->num_registers,
		       bp->bigmac_sdev->num_registers);
		printk(KERN_ERR "BIGMAC: Would you like that for here or to go?\n");
		goto fail_and_cleanup;
	}

	/* Map in QEC global control registers. */
	bp->gregs = sbus_ioremap(&bp->qec_sdev->resource[0], 0,
				 GLOB_REG_SIZE, "BigMAC QEC GLobal Regs");
	if (!bp->gregs) {
		printk(KERN_ERR "BIGMAC: Cannot map QEC global registers.\n");
		goto fail_and_cleanup;
	}

	/* Make sure QEC is in BigMAC mode. */
	if ((sbus_readl(bp->gregs + GLOB_CTRL) & 0xf0000000) != GLOB_CTRL_BMODE) {
		printk(KERN_ERR "BigMAC: AIEEE, QEC is not in BigMAC mode!\n");
		goto fail_and_cleanup;
	}

	/* Reset the QEC. */
	if (qec_global_reset(bp->gregs))
		goto fail_and_cleanup;

	/* Get supported SBUS burst sizes. */
	bsizes = prom_getintdefault(bp->qec_sdev->prom_node,
				    "burst-sizes",
				    0xff);

	bsizes_more = prom_getintdefault(bp->qec_sdev->bus->prom_node,
					 "burst-sizes",
					 0xff);

	bsizes &= 0xff;
	if (bsizes_more != 0xff)
		bsizes &= bsizes_more;
	if (bsizes == 0xff || (bsizes & DMA_BURST16) == 0 ||
	    (bsizes & DMA_BURST32) == 0)
		bsizes = (DMA_BURST32 - 1);
	bp->bigmac_bursts = bsizes;

	/* Perform QEC initialization. */
	qec_init(bp);

	/* Map in the BigMAC channel registers. */
	bp->creg = sbus_ioremap(&bp->bigmac_sdev->resource[0], 0,
				CREG_REG_SIZE, "BigMAC QEC Channel Regs");
	if (!bp->creg) {
		printk(KERN_ERR "BIGMAC: Cannot map QEC channel registers.\n");
		goto fail_and_cleanup;
	}

	/* Map in the BigMAC control registers. */
	bp->bregs = sbus_ioremap(&bp->bigmac_sdev->resource[1], 0,
				 BMAC_REG_SIZE, "BigMAC Primary Regs");
	if (!bp->bregs) {
		printk(KERN_ERR "BIGMAC: Cannot map BigMAC primary registers.\n");
		goto fail_and_cleanup;
	}

	/* Map in the BigMAC transceiver registers, this is how you poke at
	 * the BigMAC's PHY.
	 */
	bp->tregs = sbus_ioremap(&bp->bigmac_sdev->resource[2], 0,
				 TCVR_REG_SIZE, "BigMAC Transceiver Regs");
	if (!bp->tregs) {
		printk(KERN_ERR "BIGMAC: Cannot map BigMAC transceiver registers.\n");
		goto fail_and_cleanup;
	}

	/* Stop the BigMAC. */
	bigmac_stop(bp);

	/* Allocate transmit/receive descriptor DVMA block. */
	bp->bmac_block = sbus_alloc_consistent(bp->bigmac_sdev,
					       PAGE_SIZE,
					       &bp->bblock_dvma);
	if (bp->bmac_block == NULL || bp->bblock_dvma == 0) {
		printk(KERN_ERR "BIGMAC: Cannot allocate consistent DMA.\n");
		goto fail_and_cleanup;
	}

	/* Get the board revision of this BigMAC. */
	bp->board_rev = prom_getintdefault(bp->bigmac_sdev->prom_node,
					   "board-version", 1);

	/* Init auto-negotiation timer state. */
	init_timer(&bp->bigmac_timer);
	bp->timer_state = asleep;
	bp->timer_ticks = 0;

	/* Backlink to generic net device struct. */
	bp->dev = dev;

	/* Set links to our BigMAC open and close routines. */
	dev->open = &bigmac_open;
	dev->stop = &bigmac_close;
	dev->hard_start_xmit = &bigmac_start_xmit;
	dev->ethtool_ops = &bigmac_ethtool_ops;

	/* Set links to BigMAC statistic and multi-cast loading code. */
	dev->get_stats = &bigmac_get_stats;
	dev->set_multicast_list = &bigmac_set_multicast;

	dev->tx_timeout = &bigmac_tx_timeout;
	dev->watchdog_timeo = 5*HZ;

	/* Finish net device registration. */
	dev->irq = bp->bigmac_sdev->irqs[0];
	dev->dma = 0;

	if (register_netdev(dev)) {
		printk(KERN_ERR "BIGMAC: Cannot register device.\n");
		goto fail_and_cleanup;
	}

	/* Put us into the list of instances attached for later driver
	 * exit.
	 */
	bp->next_module = root_bigmac_dev;
	root_bigmac_dev = bp;

	printk(KERN_INFO "%s: BigMAC 100baseT Ethernet ", dev->name);
	for (i = 0; i < 6; i++)
		printk("%2.2x%c", dev->dev_addr[i],
		       i == 5 ? ' ' : ':');
	printk("\n");

	return 0;

fail_and_cleanup:
	/* Something went wrong, undo whatever we did so far. */
	/* Free register mappings if any. */
	if (bp->gregs)
		sbus_iounmap(bp->gregs, GLOB_REG_SIZE);
	if (bp->creg)
		sbus_iounmap(bp->creg, CREG_REG_SIZE);
	if (bp->bregs)
		sbus_iounmap(bp->bregs, BMAC_REG_SIZE);
	if (bp->tregs)
		sbus_iounmap(bp->tregs, TCVR_REG_SIZE);

	if (bp->bmac_block)
		sbus_free_consistent(bp->bigmac_sdev,
				     PAGE_SIZE,
				     bp->bmac_block,
				     bp->bblock_dvma);

	/* This also frees the co-located 'dev->priv' */
	free_netdev(dev);
	return -ENODEV;
}

/* QEC can be the parent of either QuadEthernet or
 * a BigMAC.  We want the latter.
 */
static int __init bigmac_match(struct sbus_dev *sdev)
{
	struct sbus_dev *child = sdev->child;

	if (strcmp(sdev->prom_name, "qec") != 0)
		return 0;

	if (child == NULL)
		return 0;

	if (strcmp(child->prom_name, "be") != 0)
		return 0;

	return 1;
}

static int __init bigmac_probe(void)
{
	struct sbus_bus *sbus;
	struct sbus_dev *sdev = NULL;
	static int called;
	int cards = 0, v;

	root_bigmac_dev = NULL;

	if (called)
		return -ENODEV;
	called++;

	for_each_sbus(sbus) {
		for_each_sbusdev(sdev, sbus) {
			if (bigmac_match(sdev)) {
				cards++;
				if ((v = bigmac_ether_init(sdev)))
					return v;
			}
		}
	}
	if (!cards)
		return -ENODEV;
	return 0;
}

static void __exit bigmac_cleanup(void)
{
	while (root_bigmac_dev) {
		struct bigmac *bp = root_bigmac_dev;
		struct bigmac *bp_nxt = root_bigmac_dev->next_module;

		sbus_iounmap(bp->gregs, GLOB_REG_SIZE);
		sbus_iounmap(bp->creg, CREG_REG_SIZE);
		sbus_iounmap(bp->bregs, BMAC_REG_SIZE);
		sbus_iounmap(bp->tregs, TCVR_REG_SIZE);
		sbus_free_consistent(bp->bigmac_sdev,
				     PAGE_SIZE,
				     bp->bmac_block,
				     bp->bblock_dvma);

		unregister_netdev(bp->dev);
		free_netdev(bp->dev);
		root_bigmac_dev = bp_nxt;
	}
}

module_init(bigmac_probe);
module_exit(bigmac_cleanup);
MODULE_LICENSE("GPL");
