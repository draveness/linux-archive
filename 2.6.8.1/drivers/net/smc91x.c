/*
 * smc91x.c
 * This is a driver for SMSC's 91C9x/91C1xx single-chip Ethernet devices.
 *
 * Copyright (C) 1996 by Erik Stahlman
 * Copyright (C) 2001 Standard Microsystems Corporation
 *	Developed by Simple Network Magic Corporation
 * Copyright (C) 2003 Monta Vista Software, Inc.
 *	Unified SMC91x driver by Nicolas Pitre
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Arguments:
 * 	io	= for the base address
 *	irq	= for the IRQ
 *	nowait	= 0 for normal wait states, 1 eliminates additional wait states
 *
 * original author:
 * 	Erik Stahlman <erik@vt.edu>
 *
 * hardware multicast code:
 *    Peter Cammaert <pc@denkart.be>
 *
 * contributors:
 * 	Daris A Nevil <dnevil@snmc.com>
 *      Nicolas Pitre <nico@cam.org>
 *	Russell King <rmk@arm.linux.org.uk>
 *
 * History:
 *   08/20/00  Arnaldo Melo       fix kfree(skb) in smc_hardware_send_packet
 *   12/15/00  Christian Jullien  fix "Warning: kfree_skb on hard IRQ"
 *   03/16/01  Daris A Nevil      modified smc9194.c for use with LAN91C111
 *   08/22/01  Scott Anderson     merge changes from smc9194 to smc91111
 *   08/21/01  Pramod B Bhardwaj  added support for RevB of LAN91C111
 *   12/20/01  Jeff Sutherland    initial port to Xscale PXA with DMA support
 *   04/07/03  Nicolas Pitre      unified SMC91x driver, killed irq races,
 *                                more bus abstraction, big cleanup, etc.
 *   29/09/03  Russell King       - add driver model support
 *                                - ethtool support
 *                                - convert to use generic MII interface
 *                                - add link up/down notification
 *                                - don't try to handle full negotiation in
 *                                  smc_phy_configure
 *                                - clean up (and fix stack overrun) in PHY
 *                                  MII read/write functions
 */
static const char version[] =
	"smc91x.c: v1.0, mar 07 2003 by Nicolas Pitre <nico@cam.org>\n";

/* Debugging level */
#ifndef SMC_DEBUG
#define SMC_DEBUG		0
#endif


#include <linux/config.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/crc32.h>
#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/ethtool.h>
#include <linux/mii.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>

#include <asm/io.h>
#include <asm/irq.h>

#include "smc91x.h"

#ifdef CONFIG_ISA
/*
 * the LAN91C111 can be at any of the following port addresses.  To change,
 * for a slightly different card, you can add it to the array.  Keep in
 * mind that the array must end in zero.
 */
static unsigned int smc_portlist[] __initdata = {
	0x200, 0x220, 0x240, 0x260, 0x280, 0x2A0, 0x2C0, 0x2E0,
	0x300, 0x320, 0x340, 0x360, 0x380, 0x3A0, 0x3C0, 0x3E0, 0
};

#ifndef SMC_IOADDR
# define SMC_IOADDR		-1
#endif
static unsigned long io = SMC_IOADDR;
module_param(io, ulong, 0400);
MODULE_PARM_DESC(io, "I/O base address");

#ifndef SMC_IRQ
# define SMC_IRQ		-1
#endif
static int irq = SMC_IRQ;
module_param(irq, int, 0400);
MODULE_PARM_DESC(irq, "IRQ number");

#endif  /* CONFIG_ISA */

#ifndef SMC_NOWAIT
# define SMC_NOWAIT		0
#endif
static int nowait = SMC_NOWAIT;
module_param(nowait, int, 0400);
MODULE_PARM_DESC(nowait, "set to 1 for no wait state");

/*
 * Transmit timeout, default 5 seconds.
 */
static int watchdog = 5000;
module_param(watchdog, int, 0400);
MODULE_PARM_DESC(watchdog, "transmit timeout in milliseconds");

MODULE_LICENSE("GPL");

/*
 * The internal workings of the driver.  If you are changing anything
 * here with the SMC stuff, you should have the datasheet and know
 * what you are doing.
 */
#define CARDNAME "smc91x"

/*
 * Use power-down feature of the chip
 */
#define POWER_DOWN		1

/*
 * Wait time for memory to be free.  This probably shouldn't be
 * tuned that much, as waiting for this means nothing else happens
 * in the system
 */
#define MEMORY_WAIT_TIME	16

/*
 * This selects whether TX packets are sent one by one to the SMC91x internal
 * memory and throttled until transmission completes.  This may prevent
 * RX overruns a litle by keeping much of the memory free for RX packets
 * but to the expense of reduced TX throughput and increased IRQ overhead.
 * Note this is not a cure for a too slow data bus or too high IRQ latency.
 */
#define THROTTLE_TX_PKTS	0

/*
 * The MII clock high/low times.  2x this number gives the MII clock period
 * in microseconds. (was 50, but this gives 6.4ms for each MII transaction!)
 */
#define MII_DELAY		1

/* store this information for the driver.. */
struct smc_local {
	/*
	 * If I have to wait until memory is available to send a
	 * packet, I will store the skbuff here, until I get the
	 * desired memory.  Then, I'll send it out and free it.
	 */
	struct sk_buff *saved_skb;

 	/*
	 * these are things that the kernel wants me to keep, so users
	 * can find out semi-useless statistics of how well the card is
	 * performing
	 */
	struct net_device_stats stats;

	/* version/revision of the SMC91x chip */
	int	version;

	/* Contains the current active transmission mode */
	int	tcr_cur_mode;

	/* Contains the current active receive mode */
	int	rcr_cur_mode;

	/* Contains the current active receive/phy mode */
	int	rpc_cur_mode;
	int	ctl_rfduplx;
	int	ctl_rspeed;

	u32	msg_enable;
	u32	phy_type;
	struct mii_if_info mii;
	spinlock_t lock;

#ifdef SMC_USE_PXA_DMA
	/* DMA needs the physical address of the chip */
	u_long physaddr;
#endif
};

#if SMC_DEBUG > 0
#define DBG(n, args...)					\
	do {						\
		if (SMC_DEBUG >= (n))			\
			printk(KERN_DEBUG args);	\
	} while (0)

#define PRINTK(args...)   printk(args)
#else
#define DBG(n, args...)   do { } while(0)
#define PRINTK(args...)   printk(KERN_DEBUG args)
#endif

#if SMC_DEBUG > 3
static void PRINT_PKT(u_char *buf, int length)
{
	int i;
	int remainder;
	int lines;

	lines = length / 16;
	remainder = length % 16;

	for (i = 0; i < lines ; i ++) {
		int cur;
		for (cur = 0; cur < 8; cur++) {
			u_char a, b;
			a = *buf++;
			b = *buf++;
			printk("%02x%02x ", a, b);
		}
		printk("\n");
	}
	for (i = 0; i < remainder/2 ; i++) {
		u_char a, b;
		a = *buf++;
		b = *buf++;
		printk("%02x%02x ", a, b);
	}
	printk("\n");
}
#else
#define PRINT_PKT(x...)  do { } while(0)
#endif


/* this enables an interrupt in the interrupt mask register */
#define SMC_ENABLE_INT(x) do {						\
	unsigned long flags;						\
	unsigned char mask;						\
	spin_lock_irqsave(&lp->lock, flags);				\
	mask = SMC_GET_INT_MASK();					\
	mask |= (x);							\
	SMC_SET_INT_MASK(mask);						\
	spin_unlock_irqrestore(&lp->lock, flags);			\
} while (0)

/* this disables an interrupt from the interrupt mask register */
#define SMC_DISABLE_INT(x) do {						\
	unsigned long flags;						\
	unsigned char mask;						\
	spin_lock_irqsave(&lp->lock, flags);				\
	mask = SMC_GET_INT_MASK();					\
	mask &= ~(x);							\
	SMC_SET_INT_MASK(mask);						\
	spin_unlock_irqrestore(&lp->lock, flags);			\
} while (0)

/*
 * Wait while MMU is busy.  This is usually in the order of a few nanosecs
 * if at all, but let's avoid deadlocking the system if the hardware
 * decides to go south.
 */
#define SMC_WAIT_MMU_BUSY() do {					\
	if (unlikely(SMC_GET_MMU_CMD() & MC_BUSY)) {			\
		unsigned long timeout = jiffies + 2;			\
		while (SMC_GET_MMU_CMD() & MC_BUSY) {			\
			if (time_after(jiffies, timeout)) {		\
				printk("%s: timeout %s line %d\n",	\
					dev->name, __FILE__, __LINE__);	\
				break;					\
			}						\
			cpu_relax();					\
		}							\
	}								\
} while (0)


/*
 * this does a soft reset on the device
 */
static void smc_reset(struct net_device *dev)
{
	unsigned long ioaddr = dev->base_addr;
	unsigned int ctl, cfg;

	DBG(2, "%s: %s\n", dev->name, __FUNCTION__);

	/*
	 * This resets the registers mostly to defaults, but doesn't
	 * affect EEPROM.  That seems unnecessary
	 */
	SMC_SELECT_BANK(0);
	SMC_SET_RCR(RCR_SOFTRST);

	/*
	 * Setup the Configuration Register
	 * This is necessary because the CONFIG_REG is not affected
	 * by a soft reset
	 */
	SMC_SELECT_BANK(1);

	cfg = CONFIG_DEFAULT;

	/*
	 * Setup for fast accesses if requested.  If the card/system
	 * can't handle it then there will be no recovery except for
	 * a hard reset or power cycle
	 */
	if (nowait)
		cfg |= CONFIG_NO_WAIT;

	/*
	 * Release from possible power-down state
	 * Configuration register is not affected by Soft Reset
	 */
	cfg |= CONFIG_EPH_POWER_EN;

	SMC_SET_CONFIG(cfg);

	/* this should pause enough for the chip to be happy */
	/*
	 * elaborate?  What does the chip _need_? --jgarzik
	 *
	 * This seems to be undocumented, but something the original
	 * driver(s) have always done.  Suspect undocumented timing
	 * info/determined empirically. --rmk
	 */
	udelay(1);

	/* Disable transmit and receive functionality */
	SMC_SELECT_BANK(0);
	SMC_SET_RCR(RCR_CLEAR);
	SMC_SET_TCR(TCR_CLEAR);

	SMC_SELECT_BANK(1);
	ctl = SMC_GET_CTL() | CTL_LE_ENABLE;

	/*
	 * Set the control register to automatically release successfully
	 * transmitted packets, to make the best use out of our limited
	 * memory
	 */
#if ! THROTTLE_TX_PKTS
	ctl |= CTL_AUTO_RELEASE;
#else
	ctl &= ~CTL_AUTO_RELEASE;
#endif
	SMC_SET_CTL(ctl);

	/* Disable all interrupts */
	SMC_SELECT_BANK(2);
	SMC_SET_INT_MASK(0);

	/* Reset the MMU */
	SMC_SET_MMU_CMD(MC_RESET);
	SMC_WAIT_MMU_BUSY();
}

/*
 * Enable Interrupts, Receive, and Transmit
 */
static void smc_enable(struct net_device *dev)
{
	unsigned long ioaddr = dev->base_addr;
	struct smc_local *lp = netdev_priv(dev);
	int mask;

	DBG(2, "%s: %s\n", dev->name, __FUNCTION__);

	/* see the header file for options in TCR/RCR DEFAULT */
	SMC_SELECT_BANK(0);
	SMC_SET_TCR(lp->tcr_cur_mode);
	SMC_SET_RCR(lp->rcr_cur_mode);

	/* now, enable interrupts */
	mask = IM_EPH_INT|IM_RX_OVRN_INT|IM_RCV_INT;
	if (lp->version >= (CHIP_91100 << 4))
		mask |= IM_MDINT;
	SMC_SELECT_BANK(2);
	SMC_SET_INT_MASK(mask);
}

/*
 * this puts the device in an inactive state
 */
static void smc_shutdown(unsigned long ioaddr)
{
	DBG(2, "%s: %s\n", CARDNAME, __FUNCTION__);

	/* no more interrupts for me */
	SMC_SELECT_BANK(2);
	SMC_SET_INT_MASK(0);

	/* and tell the card to stay away from that nasty outside world */
	SMC_SELECT_BANK(0);
	SMC_SET_RCR(RCR_CLEAR);
	SMC_SET_TCR(TCR_CLEAR);

#ifdef POWER_DOWN
	/* finally, shut the chip down */
	SMC_SELECT_BANK(1);
	SMC_SET_CONFIG(SMC_GET_CONFIG() & ~CONFIG_EPH_POWER_EN);
#endif
}

/*
 * This is the procedure to handle the receipt of a packet.
 */
static inline void  smc_rcv(struct net_device *dev)
{
	struct smc_local *lp = netdev_priv(dev);
	unsigned long ioaddr = dev->base_addr;
	unsigned int packet_number, status, packet_len;

	DBG(3, "%s: %s\n", dev->name, __FUNCTION__);

	packet_number = SMC_GET_RXFIFO();
	if (unlikely(packet_number & RXFIFO_REMPTY)) {
		PRINTK("%s: smc_rcv with nothing on FIFO.\n", dev->name);
		return;
	}

	/* read from start of packet */
	SMC_SET_PTR(PTR_READ | PTR_RCV | PTR_AUTOINC);

	/* First two words are status and packet length */
	SMC_GET_PKT_HDR(status, packet_len);
	packet_len &= 0x07ff;  /* mask off top bits */
	DBG(2, "%s: RX PNR 0x%x STATUS 0x%04x LENGTH 0x%04x (%d)\n",
		dev->name, packet_number, status,
		packet_len, packet_len);

	if (unlikely(status & RS_ERRORS)) {
		lp->stats.rx_errors++;
		if (status & RS_ALGNERR)
			lp->stats.rx_frame_errors++;
		if (status & (RS_TOOSHORT | RS_TOOLONG))
			lp->stats.rx_length_errors++;
		if (status & RS_BADCRC)
			lp->stats.rx_crc_errors++;
	} else {
		struct sk_buff *skb;
		unsigned char *data;
		unsigned int data_len;

		/* set multicast stats */
		if (status & RS_MULTICAST)
			lp->stats.multicast++;

		/*
		 * Actual payload is packet_len - 4 (or 3 if odd byte).
		 * We want skb_reserve(2) and the final ctrl word
		 * (2 bytes, possibly containing the payload odd byte).
		 * Ence packet_len - 4 + 2 + 2.
		 */
		skb = dev_alloc_skb(packet_len);
		if (unlikely(skb == NULL)) {
			printk(KERN_NOTICE "%s: Low memory, packet dropped.\n",
				dev->name);
			lp->stats.rx_dropped++;
			goto done;
		}

		/* Align IP header to 32 bits */
		skb_reserve(skb, 2);

		/* BUG: the LAN91C111 rev A never sets this bit. Force it. */
		if (lp->version == 0x90)
			status |= RS_ODDFRAME;

		/*
		 * If odd length: packet_len - 3,
		 * otherwise packet_len - 4.
		 */
		data_len = packet_len - ((status & RS_ODDFRAME) ? 3 : 4);
		data = skb_put(skb, data_len);
		SMC_PULL_DATA(data, packet_len - 2);

		PRINT_PKT(data, packet_len - 2);

		dev->last_rx = jiffies;
		skb->dev = dev;
		skb->protocol = eth_type_trans(skb, dev);
		netif_rx(skb);
		lp->stats.rx_packets++;
		lp->stats.rx_bytes += data_len;
	}

done:
	SMC_WAIT_MMU_BUSY();
	SMC_SET_MMU_CMD(MC_RELEASE);
}

/*
 * This is called to actually send a packet to the chip.
 * Returns non-zero when successful.
 */
static void smc_hardware_send_packet(struct net_device *dev)
{
	struct smc_local *lp = netdev_priv(dev);
	unsigned long ioaddr = dev->base_addr;
	struct sk_buff *skb = lp->saved_skb;
	unsigned int packet_no, len;
	unsigned char *buf;

	DBG(3, "%s: %s\n", dev->name, __FUNCTION__);

	packet_no = SMC_GET_AR();
	if (unlikely(packet_no & AR_FAILED)) {
		printk("%s: Memory allocation failed.\n", dev->name);
		lp->saved_skb = NULL;
		lp->stats.tx_errors++;
		lp->stats.tx_fifo_errors++;
		dev_kfree_skb_any(skb);
		return;
	}

	/* point to the beginning of the packet */
	SMC_SET_PN(packet_no);
	SMC_SET_PTR(PTR_AUTOINC);

	buf = skb->data;
	len = skb->len;
	DBG(2, "%s: TX PNR 0x%x LENGTH 0x%04x (%d) BUF 0x%p\n",
		dev->name, packet_no, len, len, buf);
	PRINT_PKT(buf, len);

	/*
	 * Send the packet length (+6 for status words, length, and ctl.
	 * The card will pad to 64 bytes with zeroes if packet is too small.
	 */
	SMC_PUT_PKT_HDR(0, len + 6);

	/* send the actual data */
	SMC_PUSH_DATA(buf, len & ~1);

	/* Send final ctl word with the last byte if there is one */
	SMC_outw(((len & 1) ? (0x2000 | buf[len-1]) : 0), ioaddr, DATA_REG);

	/* and let the chipset deal with it */
	SMC_SET_MMU_CMD(MC_ENQUEUE);
	SMC_ACK_INT(IM_TX_EMPTY_INT);

	dev->trans_start = jiffies;
	dev_kfree_skb_any(skb);
	lp->saved_skb = NULL;
	lp->stats.tx_packets++;
	lp->stats.tx_bytes += len;
}

/*
 * Since I am not sure if I will have enough room in the chip's ram
 * to store the packet, I call this routine which either sends it
 * now, or set the card to generates an interrupt when ready
 * for the packet.
 */
static int smc_hard_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct smc_local *lp = netdev_priv(dev);
	unsigned long ioaddr = dev->base_addr;
	unsigned int numPages, poll_count, status, saved_bank;

	DBG(3, "%s: %s\n", dev->name, __FUNCTION__);

	BUG_ON(lp->saved_skb != NULL);
	lp->saved_skb = skb;

	/*
	 * The MMU wants the number of pages to be the number of 256 bytes
	 * 'pages', minus 1 (since a packet can't ever have 0 pages :))
	 *
	 * The 91C111 ignores the size bits, but earlier models don't.
	 *
	 * Pkt size for allocating is data length +6 (for additional status
	 * words, length and ctl)
	 *
	 * If odd size then last byte is included in ctl word.
	 */
	numPages = ((skb->len & ~1) + (6 - 1)) >> 8;
	if (unlikely(numPages > 7)) {
		printk("%s: Far too big packet error.\n", dev->name);
		lp->saved_skb = NULL;
		lp->stats.tx_errors++;
		lp->stats.tx_dropped++;
		dev_kfree_skb(skb);
		return 0;
	}

	/* now, try to allocate the memory */
	saved_bank = SMC_CURRENT_BANK();
	SMC_SELECT_BANK(2);
	SMC_SET_MMU_CMD(MC_ALLOC | numPages);

	/*
	 * Poll the chip for a short amount of time in case the
	 * allocation succeeds quickly.
	 */
	poll_count = MEMORY_WAIT_TIME;
	do {
		status = SMC_GET_INT();
		if (status & IM_ALLOC_INT) {
			SMC_ACK_INT(IM_ALLOC_INT);
  			break;
		}
   	} while (--poll_count);

   	if (!poll_count) {
		/* oh well, wait until the chip finds memory later */
		netif_stop_queue(dev);
		DBG(2, "%s: TX memory allocation deferred.\n", dev->name);
		SMC_ENABLE_INT(IM_ALLOC_INT);
   	} else {
		/*
		 * Allocation succeeded: push packet to the chip's own memory
		 * immediately.
		 *
		 * If THROTTLE_TX_PKTS is selected that means we don't want
		 * more than a single TX packet taking up space in the chip's
		 * internal memory at all time, in which case we stop the
		 * queue right here until we're notified of TX completion.
		 *
		 * Otherwise we're quite happy to feed more TX packets right
		 * away for better TX throughput, in which case the queue is
		 * left active.
		 */  
#if THROTTLE_TX_PKTS
		netif_stop_queue(dev);
#endif
		smc_hardware_send_packet(dev);
		SMC_ENABLE_INT(IM_TX_INT | IM_TX_EMPTY_INT);
	}

	SMC_SELECT_BANK(saved_bank);
	return 0;
}

/*
 * This handles a TX interrupt, which is only called when:
 * - a TX error occurred, or
 * - CTL_AUTO_RELEASE is not set and TX of a packet completed.
 */
static void smc_tx(struct net_device *dev)
{
	unsigned long ioaddr = dev->base_addr;
	struct smc_local *lp = netdev_priv(dev);
	unsigned int saved_packet, packet_no, tx_status, pkt_len;

	DBG(3, "%s: %s\n", dev->name, __FUNCTION__);

	/* If the TX FIFO is empty then nothing to do */
	packet_no = SMC_GET_TXFIFO();
	if (unlikely(packet_no & TXFIFO_TEMPTY)) {
		PRINTK("%s: smc_tx with nothing on FIFO.\n", dev->name);
		return;
	}

	/* select packet to read from */
	saved_packet = SMC_GET_PN();
	SMC_SET_PN(packet_no);

	/* read the first word (status word) from this packet */
	SMC_SET_PTR(PTR_AUTOINC | PTR_READ);
	SMC_GET_PKT_HDR(tx_status, pkt_len);
	DBG(2, "%s: TX STATUS 0x%04x PNR 0x%02x\n",
		dev->name, tx_status, packet_no);

	if (!(tx_status & TS_SUCCESS))
		lp->stats.tx_errors++;
	if (tx_status & TS_LOSTCAR)
		lp->stats.tx_carrier_errors++;

	if (tx_status & TS_LATCOL) {
		PRINTK("%s: late collision occurred on last xmit\n", dev->name);
		lp->stats.tx_window_errors++;
		if (!(lp->stats.tx_window_errors & 63) && net_ratelimit()) {
			printk(KERN_INFO "%s: unexpectedly large numbers of "
			       "late collisions. Please check duplex "
			       "setting.\n", dev->name);
		}
	}

	/* kill the packet */
	SMC_WAIT_MMU_BUSY();
	SMC_SET_MMU_CMD(MC_FREEPKT);

	/* Don't restore Packet Number Reg until busy bit is cleared */
	SMC_WAIT_MMU_BUSY();
	SMC_SET_PN(saved_packet);

	/* re-enable transmit */
	SMC_SELECT_BANK(0);
	SMC_SET_TCR(lp->tcr_cur_mode);
	SMC_SELECT_BANK(2);
}


/*---PHY CONTROL AND CONFIGURATION-----------------------------------------*/

static void smc_mii_out(struct net_device *dev, unsigned int val, int bits)
{
	unsigned long ioaddr = dev->base_addr;
	unsigned int mii_reg, mask;

	mii_reg = SMC_GET_MII() & ~(MII_MCLK | MII_MDOE | MII_MDO);
	mii_reg |= MII_MDOE;

	for (mask = 1 << (bits - 1); mask; mask >>= 1) {
		if (val & mask)
			mii_reg |= MII_MDO;
		else
			mii_reg &= ~MII_MDO;

		SMC_SET_MII(mii_reg);
		udelay(MII_DELAY);
		SMC_SET_MII(mii_reg | MII_MCLK);
		udelay(MII_DELAY);
	}
}

static unsigned int smc_mii_in(struct net_device *dev, int bits)
{
	unsigned long ioaddr = dev->base_addr;
	unsigned int mii_reg, mask, val;

	mii_reg = SMC_GET_MII() & ~(MII_MCLK | MII_MDOE | MII_MDO);
	SMC_SET_MII(mii_reg);

	for (mask = 1 << (bits - 1), val = 0; mask; mask >>= 1) {
		if (SMC_GET_MII() & MII_MDI)
			val |= mask;

		SMC_SET_MII(mii_reg);
		udelay(MII_DELAY);
		SMC_SET_MII(mii_reg | MII_MCLK);
		udelay(MII_DELAY);
	}

	return val;
}

/*
 * Reads a register from the MII Management serial interface
 */
static int smc_phy_read(struct net_device *dev, int phyaddr, int phyreg)
{
	unsigned long ioaddr = dev->base_addr;
	unsigned int phydata, old_bank;

	/* Save the current bank, and select bank 3 */
	old_bank = SMC_CURRENT_BANK();
	SMC_SELECT_BANK(3);

	/* Idle - 32 ones */
	smc_mii_out(dev, 0xffffffff, 32);

	/* Start code (01) + read (10) + phyaddr + phyreg */
	smc_mii_out(dev, 6 << 10 | phyaddr << 5 | phyreg, 14);

	/* Turnaround (2bits) + phydata */
	phydata = smc_mii_in(dev, 18);

	/* Return to idle state */
	SMC_SET_MII(SMC_GET_MII() & ~(MII_MCLK|MII_MDOE|MII_MDO));

	/* And select original bank */
	SMC_SELECT_BANK(old_bank);

	DBG(3, "%s: phyaddr=0x%x, phyreg=0x%x, phydata=0x%x\n",
		__FUNCTION__, phyaddr, phyreg, phydata);

	return phydata;
}

/*
 * Writes a register to the MII Management serial interface
 */
static void smc_phy_write(struct net_device *dev, int phyaddr, int phyreg,
			  int phydata)
{
	unsigned long ioaddr = dev->base_addr;
	unsigned int old_bank;

	/* Save the current bank, and select bank 3 */
	old_bank = SMC_CURRENT_BANK();
	SMC_SELECT_BANK(3);

	/* Idle - 32 ones */
	smc_mii_out(dev, 0xffffffff, 32);

	/* Start code (01) + write (01) + phyaddr + phyreg + turnaround + phydata */
	smc_mii_out(dev, 5 << 28 | phyaddr << 23 | phyreg << 18 | 2 << 16 | phydata, 32);

	/* Return to idle state */
	SMC_SET_MII(SMC_GET_MII() & ~(MII_MCLK|MII_MDOE|MII_MDO));

	/* And select original bank */
	SMC_SELECT_BANK(old_bank);

	DBG(3, "%s: phyaddr=0x%x, phyreg=0x%x, phydata=0x%x\n",
		__FUNCTION__, phyaddr, phyreg, phydata);
}

/*
 * Finds and reports the PHY address
 */
static void smc_detect_phy(struct net_device *dev)
{
	struct smc_local *lp = netdev_priv(dev);
	int phyaddr;

	DBG(2, "%s: %s\n", dev->name, __FUNCTION__);

	lp->phy_type = 0;

	/*
	 * Scan all 32 PHY addresses if necessary, starting at
	 * PHY#1 to PHY#31, and then PHY#0 last.
	 */
	for (phyaddr = 1; phyaddr < 33; ++phyaddr) {
		unsigned int id1, id2;

		/* Read the PHY identifiers */
		id1 = smc_phy_read(dev, phyaddr & 31, MII_PHYSID1);
		id2 = smc_phy_read(dev, phyaddr & 31, MII_PHYSID2);

		DBG(3, "%s: phy_id1=0x%x, phy_id2=0x%x\n",
			dev->name, id1, id2);

		/* Make sure it is a valid identifier */
		if (id1 != 0x0000 && id1 != 0xffff && id1 != 0x8000 &&
		    id2 != 0x0000 && id2 != 0xffff && id2 != 0x8000) {
			/* Save the PHY's address */
			lp->mii.phy_id = phyaddr & 31;
			lp->phy_type = id1 << 16 | id2;
			break;
		}
	}
}

/*
 * Sets the PHY to a configuration as determined by the user
 */
static int smc_phy_fixed(struct net_device *dev)
{
	struct smc_local *lp = netdev_priv(dev);
	unsigned long ioaddr = dev->base_addr;
	int phyaddr = lp->mii.phy_id;
	int bmcr, cfg1;

	DBG(3, "%s: %s\n", dev->name, __FUNCTION__);

	/* Enter Link Disable state */
	cfg1 = smc_phy_read(dev, phyaddr, PHY_CFG1_REG);
	cfg1 |= PHY_CFG1_LNKDIS;
	smc_phy_write(dev, phyaddr, PHY_CFG1_REG, cfg1);

	/*
	 * Set our fixed capabilities
	 * Disable auto-negotiation
	 */
	bmcr = 0;

	if (lp->ctl_rfduplx)
		bmcr |= BMCR_FULLDPLX;

	if (lp->ctl_rspeed == 100)
		bmcr |= BMCR_SPEED100;

	/* Write our capabilities to the phy control register */
	smc_phy_write(dev, phyaddr, MII_BMCR, bmcr);

	/* Re-Configure the Receive/Phy Control register */
	SMC_SET_RPC(lp->rpc_cur_mode);

	return 1;
}

/*
 * smc_phy_reset - reset the phy
 * @dev: net device
 * @phy: phy address
 *
 * Issue a software reset for the specified PHY and
 * wait up to 100ms for the reset to complete.  We should
 * not access the PHY for 50ms after issuing the reset.
 *
 * The time to wait appears to be dependent on the PHY.
 *
 * Must be called with lp->lock locked.
 */
static int smc_phy_reset(struct net_device *dev, int phy)
{
	struct smc_local *lp = netdev_priv(dev);
	unsigned int bmcr;
	int timeout;

	smc_phy_write(dev, phy, MII_BMCR, BMCR_RESET);

	for (timeout = 2; timeout; timeout--) {
		spin_unlock_irq(&lp->lock);
		msleep(50);
		spin_lock_irq(&lp->lock);

		bmcr = smc_phy_read(dev, phy, MII_BMCR);
		if (!(bmcr & BMCR_RESET))
			break;
	}

	return bmcr & BMCR_RESET;
}

/*
 * smc_phy_powerdown - powerdown phy
 * @dev: net device
 * @phy: phy address
 *
 * Power down the specified PHY
 */
static void smc_phy_powerdown(struct net_device *dev, int phy)
{
	struct smc_local *lp = netdev_priv(dev);
	unsigned int bmcr;

	spin_lock_irq(&lp->lock);
	bmcr = smc_phy_read(dev, phy, MII_BMCR);
	smc_phy_write(dev, phy, MII_BMCR, bmcr | BMCR_PDOWN);
	spin_unlock_irq(&lp->lock);
}

/*
 * smc_phy_check_media - check the media status and adjust TCR
 * @dev: net device
 * @init: set true for initialisation
 *
 * Select duplex mode depending on negotiation state.  This
 * also updates our carrier state.
 */
static void smc_phy_check_media(struct net_device *dev, int init)
{
	struct smc_local *lp = netdev_priv(dev);
	unsigned long ioaddr = dev->base_addr;

	if (mii_check_media(&lp->mii, netif_msg_link(lp), init)) {
		unsigned int old_bank;

		/* duplex state has changed */
		if (lp->mii.full_duplex) {
			lp->tcr_cur_mode |= TCR_SWFDUP;
		} else {
			lp->tcr_cur_mode &= ~TCR_SWFDUP;
		}

		old_bank = SMC_CURRENT_BANK();
		SMC_SELECT_BANK(0);
		SMC_SET_TCR(lp->tcr_cur_mode);
		SMC_SELECT_BANK(old_bank);
	}
}

/*
 * Configures the specified PHY through the MII management interface
 * using Autonegotiation.
 * Calls smc_phy_fixed() if the user has requested a certain config.
 * If RPC ANEG bit is set, the media selection is dependent purely on
 * the selection by the MII (either in the MII BMCR reg or the result
 * of autonegotiation.)  If the RPC ANEG bit is cleared, the selection
 * is controlled by the RPC SPEED and RPC DPLX bits.
 */
static void smc_phy_configure(struct net_device *dev)
{
	struct smc_local *lp = netdev_priv(dev);
	unsigned long ioaddr = dev->base_addr;
	int phyaddr = lp->mii.phy_id;
	int my_phy_caps; /* My PHY capabilities */
	int my_ad_caps; /* My Advertised capabilities */
	int status;

	DBG(3, "%s:smc_program_phy()\n", dev->name);

	spin_lock_irq(&lp->lock);

	/*
	 * We should not be called if phy_type is zero.
	 */
	if (lp->phy_type == 0)
		goto smc_phy_configure_exit;

	if (smc_phy_reset(dev, phyaddr)) {
		printk("%s: PHY reset timed out\n", dev->name);
		goto smc_phy_configure_exit;
	}

	/*
	 * Enable PHY Interrupts (for register 18)
	 * Interrupts listed here are disabled
	 */
	smc_phy_write(dev, phyaddr, PHY_MASK_REG,
		PHY_INT_LOSSSYNC | PHY_INT_CWRD | PHY_INT_SSD |
		PHY_INT_ESD | PHY_INT_RPOL | PHY_INT_JAB |
		PHY_INT_SPDDET | PHY_INT_DPLXDET);

	/* Configure the Receive/Phy Control register */
	SMC_SELECT_BANK(0);
	SMC_SET_RPC(lp->rpc_cur_mode);

	/* If the user requested no auto neg, then go set his request */
	if (lp->mii.force_media) {
		smc_phy_fixed(dev);
		goto smc_phy_configure_exit;
	}

	/* Copy our capabilities from MII_BMSR to MII_ADVERTISE */
	my_phy_caps = smc_phy_read(dev, phyaddr, MII_BMSR);

	if (!(my_phy_caps & BMSR_ANEGCAPABLE)) {
		printk(KERN_INFO "Auto negotiation NOT supported\n");
		smc_phy_fixed(dev);
		goto smc_phy_configure_exit;
	}

	my_ad_caps = ADVERTISE_CSMA; /* I am CSMA capable */

	if (my_phy_caps & BMSR_100BASE4)
		my_ad_caps |= ADVERTISE_100BASE4;
	if (my_phy_caps & BMSR_100FULL)
		my_ad_caps |= ADVERTISE_100FULL;
	if (my_phy_caps & BMSR_100HALF)
		my_ad_caps |= ADVERTISE_100HALF;
	if (my_phy_caps & BMSR_10FULL)
		my_ad_caps |= ADVERTISE_10FULL;
	if (my_phy_caps & BMSR_10HALF)
		my_ad_caps |= ADVERTISE_10HALF;

	/* Disable capabilities not selected by our user */
	if (lp->ctl_rspeed != 100)
		my_ad_caps &= ~(ADVERTISE_100BASE4|ADVERTISE_100FULL|ADVERTISE_100HALF);

	if (!lp->ctl_rfduplx)
		my_ad_caps &= ~(ADVERTISE_100FULL|ADVERTISE_10FULL);

	/* Update our Auto-Neg Advertisement Register */
	smc_phy_write(dev, phyaddr, MII_ADVERTISE, my_ad_caps);
	lp->mii.advertising = my_ad_caps;

	/*
	 * Read the register back.  Without this, it appears that when
	 * auto-negotiation is restarted, sometimes it isn't ready and
	 * the link does not come up.
	 */
	status = smc_phy_read(dev, phyaddr, MII_ADVERTISE);

	DBG(2, "%s: phy caps=%x\n", dev->name, my_phy_caps);
	DBG(2, "%s: phy advertised caps=%x\n", dev->name, my_ad_caps);

	/* Restart auto-negotiation process in order to advertise my caps */
	smc_phy_write(dev, phyaddr, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);

	smc_phy_check_media(dev, 1);

smc_phy_configure_exit:
	spin_unlock_irq(&lp->lock);
}

/*
 * smc_phy_interrupt
 *
 * Purpose:  Handle interrupts relating to PHY register 18. This is
 *  called from the "hard" interrupt handler under our private spinlock.
 */
static void smc_phy_interrupt(struct net_device *dev)
{
	struct smc_local *lp = netdev_priv(dev);
	int phyaddr = lp->mii.phy_id;
	int phy18;

	DBG(2, "%s: %s\n", dev->name, __FUNCTION__);

	if (lp->phy_type == 0)
		return;

	for(;;) {
		smc_phy_check_media(dev, 0);

		/* Read PHY Register 18, Status Output */
		phy18 = smc_phy_read(dev, phyaddr, PHY_INT_REG);
		if ((phy18 & PHY_INT_INT) == 0)
			break;
	}
}

/*--- END PHY CONTROL AND CONFIGURATION-------------------------------------*/

static void smc_10bt_check_media(struct net_device *dev, int init)
{
	struct smc_local *lp = netdev_priv(dev);
	unsigned long ioaddr = dev->base_addr;
	unsigned int old_carrier, new_carrier, old_bank;

	old_bank = SMC_CURRENT_BANK();
	SMC_SELECT_BANK(0);
	old_carrier = netif_carrier_ok(dev) ? 1 : 0;
	new_carrier = SMC_inw(ioaddr, EPH_STATUS_REG) & ES_LINK_OK ? 1 : 0;

	if (init || (old_carrier != new_carrier)) {
		if (!new_carrier) {
			netif_carrier_off(dev);
		} else {
			netif_carrier_on(dev);
		}
		if (netif_msg_link(lp))
			printk(KERN_INFO "%s: link %s\n", dev->name,
			       new_carrier ? "up" : "down");
	}
	SMC_SELECT_BANK(old_bank);
}

static void smc_eph_interrupt(struct net_device *dev)
{
	unsigned long ioaddr = dev->base_addr;
	unsigned int old_bank, ctl;

	smc_10bt_check_media(dev, 0);

	old_bank = SMC_CURRENT_BANK();
	SMC_SELECT_BANK(1);

	ctl = SMC_GET_CTL();
	SMC_SET_CTL(ctl & ~CTL_LE_ENABLE);
	SMC_SET_CTL(ctl);

	SMC_SELECT_BANK(old_bank);
}

/*
 * This is the main routine of the driver, to handle the device when
 * it needs some attention.
 */
static irqreturn_t smc_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	struct net_device *dev = dev_id;
	unsigned long ioaddr = dev->base_addr;
	struct smc_local *lp = netdev_priv(dev);
	int status, mask, timeout, card_stats;
	int saved_bank, saved_pointer;

	DBG(3, "%s: %s\n", dev->name, __FUNCTION__);

	saved_bank = SMC_CURRENT_BANK();
	SMC_SELECT_BANK(2);
	saved_pointer = SMC_GET_PTR();
	mask = SMC_GET_INT_MASK();
	SMC_SET_INT_MASK(0);

	/* set a timeout value, so I don't stay here forever */
	timeout = 8;

	do {
		status = SMC_GET_INT();

		DBG(2, "%s: IRQ 0x%02x MASK 0x%02x MEM 0x%04x FIFO 0x%04x\n",
			dev->name, status, mask,
			({ int meminfo; SMC_SELECT_BANK(0);
			   meminfo = SMC_GET_MIR();
			   SMC_SELECT_BANK(2); meminfo; }),
			SMC_GET_FIFO());

		status &= mask;
		if (!status)
			break;

		spin_lock(&lp->lock);

		if (status & IM_RCV_INT) {
			DBG(3, "%s: RX irq\n", dev->name);
			smc_rcv(dev);
		} else if (status & IM_TX_INT) {
			DBG(3, "%s: TX int\n", dev->name);
			smc_tx(dev);
			SMC_ACK_INT(IM_TX_INT);
#if THROTTLE_TX_PKTS
			netif_wake_queue(dev);
#endif
		} else if (status & IM_ALLOC_INT) {
			DBG(3, "%s: Allocation irq\n", dev->name);
			smc_hardware_send_packet(dev);
			mask |= (IM_TX_INT | IM_TX_EMPTY_INT);
			mask &= ~IM_ALLOC_INT;
#if ! THROTTLE_TX_PKTS
			netif_wake_queue(dev);
#endif
		} else if (status & IM_TX_EMPTY_INT) {
			DBG(3, "%s: TX empty\n", dev->name);
			mask &= ~IM_TX_EMPTY_INT;

			/* update stats */
			SMC_SELECT_BANK(0);
			card_stats = SMC_GET_COUNTER();
			SMC_SELECT_BANK(2);

			/* single collisions */
			lp->stats.collisions += card_stats & 0xF;
			card_stats >>= 4;

			/* multiple collisions */
			lp->stats.collisions += card_stats & 0xF;
		} else if (status & IM_RX_OVRN_INT) {
			DBG(1, "%s: RX overrun\n", dev->name);
			SMC_ACK_INT(IM_RX_OVRN_INT);
			lp->stats.rx_errors++;
			lp->stats.rx_fifo_errors++;
		} else if (status & IM_EPH_INT) {
			smc_eph_interrupt(dev);
		} else if (status & IM_MDINT) {
			SMC_ACK_INT(IM_MDINT);
			smc_phy_interrupt(dev);
		} else if (status & IM_ERCV_INT) {
			SMC_ACK_INT(IM_ERCV_INT);
			PRINTK("%s: UNSUPPORTED: ERCV INTERRUPT \n", dev->name);
		}

		spin_unlock(&lp->lock);
	} while (--timeout);

	/* restore register states */
	SMC_SET_INT_MASK(mask);
	SMC_SET_PTR(saved_pointer);
	SMC_SELECT_BANK(saved_bank);

	DBG(3, "%s: Interrupt done (%d loops)\n", dev->name, 8-timeout);

	/*
	 * We return IRQ_HANDLED unconditionally here even if there was
	 * nothing to do.  There is a possibility that a packet might
	 * get enqueued into the chip right after TX_EMPTY_INT is raised
	 * but just before the CPU acknowledges the IRQ.
	 * Better take an unneeded IRQ in some occasions than complexifying
	 * the code for all cases.
	 */
	return IRQ_HANDLED;
}

/* Our watchdog timed out. Called by the networking layer */
static void smc_timeout(struct net_device *dev)
{
	struct smc_local *lp = netdev_priv(dev);

	DBG(2, "%s: %s\n", dev->name, __FUNCTION__);

	smc_reset(dev);
	smc_enable(dev);

#if 0
	/*
	 * Reconfiguring the PHY doesn't seem like a bad idea here, but
	 * it introduced a problem.  Now that this is a timeout routine,
	 * we are getting called from within an interrupt context.
	 * smc_phy_configure() calls msleep() which calls
	 * schedule_timeout() which calls schedule().  When schedule()
	 * is called from an interrupt context, it prints out
	 * "Scheduling in interrupt" and then calls BUG().  This is
	 * obviously not desirable.  This was worked around by removing
	 * the call to smc_phy_configure() here because it didn't seem
	 * absolutely necessary.  Ultimately, if msleep() is
	 * supposed to be usable from an interrupt context (which it
	 * looks like it thinks it should handle), it should be fixed.
	 */
	if (lp->phy_type != 0)
		smc_phy_configure(dev);
#endif

	/* clear anything saved */
	if (lp->saved_skb != NULL) {
		dev_kfree_skb (lp->saved_skb);
		lp->saved_skb = NULL;
		lp->stats.tx_errors++;
		lp->stats.tx_aborted_errors++;
	}
	/* We can accept TX packets again */
	dev->trans_start = jiffies;
	netif_wake_queue(dev);
}

/*
 *    This sets the internal hardware table to filter out unwanted multicast
 *    packets before they take up memory.
 *
 *    The SMC chip uses a hash table where the high 6 bits of the CRC of
 *    address are the offset into the table.  If that bit is 1, then the
 *    multicast packet is accepted.  Otherwise, it's dropped silently.
 *
 *    To use the 6 bits as an offset into the table, the high 3 bits are the
 *    number of the 8 bit register, while the low 3 bits are the bit within
 *    that register.
 *
 *    This routine is based very heavily on the one provided by Peter Cammaert.
 */
static void
smc_setmulticast(unsigned long ioaddr, int count, struct dev_mc_list *addrs)
{
	int i;
	unsigned char multicast_table[8];
	struct dev_mc_list *cur_addr;

	/* table for flipping the order of 3 bits */
	static unsigned char invert3[] = { 0, 4, 2, 6, 1, 5, 3, 7 };

	/* start with a table of all zeros: reject all */
	memset(multicast_table, 0, sizeof(multicast_table));

	cur_addr = addrs;
	for (i = 0; i < count; i++, cur_addr = cur_addr->next) {
		int position;

		/* do we have a pointer here? */
		if (!cur_addr)
			break;
		/* make sure this is a multicast address - shouldn't this
		   be a given if we have it here ? */
		if (!(*cur_addr->dmi_addr & 1))
			continue;

		/* only use the low order bits */
		position = crc32_le(~0, cur_addr->dmi_addr, 6) & 0x3f;

		/* do some messy swapping to put the bit in the right spot */
		multicast_table[invert3[position&7]] |=
					(1<<invert3[(position>>3)&7]);

	}
	/* now, the table can be loaded into the chipset */
	SMC_SELECT_BANK(3);
	SMC_SET_MCAST(multicast_table);
}

/*
 * This routine will, depending on the values passed to it,
 * either make it accept multicast packets, go into
 * promiscuous mode (for TCPDUMP and cousins) or accept
 * a select set of multicast packets
 */
static void smc_set_multicast_list(struct net_device *dev)
{
	struct smc_local *lp = netdev_priv(dev);
	unsigned long ioaddr = dev->base_addr;

	DBG(2, "%s: %s\n", dev->name, __FUNCTION__);

	SMC_SELECT_BANK(0);
	if (dev->flags & IFF_PROMISC) {
		DBG(2, "%s: RCR_PRMS\n", dev->name);
		lp->rcr_cur_mode |= RCR_PRMS;
		SMC_SET_RCR(lp->rcr_cur_mode);
	}

/* BUG?  I never disable promiscuous mode if multicasting was turned on.
   Now, I turn off promiscuous mode, but I don't do anything to multicasting
   when promiscuous mode is turned on.
*/

	/*
	 * Here, I am setting this to accept all multicast packets.
	 * I don't need to zero the multicast table, because the flag is
	 * checked before the table is
	 */
	else if (dev->flags & IFF_ALLMULTI || dev->mc_count > 16) {
		lp->rcr_cur_mode |= RCR_ALMUL;
		SMC_SET_RCR(lp->rcr_cur_mode);
		DBG(2, "%s: RCR_ALMUL\n", dev->name);
	}

	/*
	 * We just get all multicast packets even if we only want them
	 * from one source.  This will be changed at some future point.
	 */
	else if (dev->mc_count)  {
		/* support hardware multicasting */

		/* be sure I get rid of flags I might have set */
		lp->rcr_cur_mode &= ~(RCR_PRMS | RCR_ALMUL);
		SMC_SET_RCR(lp->rcr_cur_mode);
		/*
		 * NOTE: this has to set the bank, so make sure it is the
		 * last thing called.  The bank is set to zero at the top
		 */
		smc_setmulticast(ioaddr, dev->mc_count, dev->mc_list);
	} else  {
		DBG(2, "%s: ~(RCR_PRMS|RCR_ALMUL)\n", dev->name);
		lp->rcr_cur_mode &= ~(RCR_PRMS | RCR_ALMUL);
		SMC_SET_RCR(lp->rcr_cur_mode);

		/*
		 * since I'm disabling all multicast entirely, I need to
		 * clear the multicast list
		 */
		SMC_SELECT_BANK(3);
		SMC_CLEAR_MCAST();
	}
}


/*
 * Open and Initialize the board
 *
 * Set up everything, reset the card, etc..
 */
static int
smc_open(struct net_device *dev)
{
	struct smc_local *lp = netdev_priv(dev);
	unsigned long ioaddr = dev->base_addr;

	DBG(2, "%s: %s\n", dev->name, __FUNCTION__);

	/*
	 * Check that the address is valid.  If its not, refuse
	 * to bring the device up.  The user must specify an
	 * address using ifconfig eth0 hw ether xx:xx:xx:xx:xx:xx
	 */
	if (!is_valid_ether_addr(dev->dev_addr)) {
		DBG(2, (KERN_DEBUG "smc_open: no valid ethernet hw addr\n"));
		return -EINVAL;
	}

	/* clear out all the junk that was put here before... */
	lp->saved_skb = NULL;

	/* Setup the default Register Modes */
	lp->tcr_cur_mode = TCR_DEFAULT;
	lp->rcr_cur_mode = RCR_DEFAULT;
	lp->rpc_cur_mode = RPC_DEFAULT;

	/*
	 * If we are not using a MII interface, we need to
	 * monitor our own carrier signal to detect faults.
	 */
	if (lp->phy_type == 0)
		lp->tcr_cur_mode |= TCR_MON_CSN;

	/* reset the hardware */
	smc_reset(dev);
	smc_enable(dev);

	SMC_SELECT_BANK(1);
	SMC_SET_MAC_ADDR(dev->dev_addr);

	/* Configure the PHY */
	if (lp->phy_type != 0)
		smc_phy_configure(dev);
	else {
		spin_lock_irq(&lp->lock);
		smc_10bt_check_media(dev, 1);
		spin_unlock_irq(&lp->lock);
	}

	/*
	 * make sure to initialize the link state with netif_carrier_off()
	 * somewhere, too --jgarzik
	 *
	 * smc_phy_configure() and smc_10bt_check_media() does that. --rmk
	 */
	netif_start_queue(dev);
	return 0;
}

/*
 * smc_close
 *
 * this makes the board clean up everything that it can
 * and not talk to the outside world.   Caused by
 * an 'ifconfig ethX down'
 */
static int smc_close(struct net_device *dev)
{
	struct smc_local *lp = netdev_priv(dev);

	DBG(2, "%s: %s\n", dev->name, __FUNCTION__);

	netif_stop_queue(dev);
	netif_carrier_off(dev);

	/* clear everything */
	smc_shutdown(dev->base_addr);

	if (lp->phy_type != 0)
		smc_phy_powerdown(dev, lp->mii.phy_id);

	return 0;
}

/*
 * Get the current statistics.
 * This may be called with the card open or closed.
 */
static struct net_device_stats *smc_query_statistics(struct net_device *dev)
{
	struct smc_local *lp = netdev_priv(dev);

	DBG(2, "%s: %s\n", dev->name, __FUNCTION__);

	return &lp->stats;
}

/*
 * Ethtool support
 */
static int
smc_ethtool_getsettings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct smc_local *lp = netdev_priv(dev);
	int ret;

	cmd->maxtxpkt = 1;
	cmd->maxrxpkt = 1;

	if (lp->phy_type != 0) {
		spin_lock_irq(&lp->lock);
		ret = mii_ethtool_gset(&lp->mii, cmd);
		spin_unlock_irq(&lp->lock);
	} else {
		cmd->supported = SUPPORTED_10baseT_Half |
				 SUPPORTED_10baseT_Full |
				 SUPPORTED_TP | SUPPORTED_AUI;

		if (lp->ctl_rspeed == 10)
			cmd->speed = SPEED_10;
		else if (lp->ctl_rspeed == 100)
			cmd->speed = SPEED_100;

		cmd->autoneg = AUTONEG_DISABLE;
		cmd->transceiver = XCVR_INTERNAL;
		cmd->port = 0;
		cmd->duplex = lp->tcr_cur_mode & TCR_SWFDUP ? DUPLEX_FULL : DUPLEX_HALF;

		ret = 0;
	}

	return ret;
}

static int
smc_ethtool_setsettings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct smc_local *lp = netdev_priv(dev);
	int ret;

	if (lp->phy_type != 0) {
		spin_lock_irq(&lp->lock);
		ret = mii_ethtool_sset(&lp->mii, cmd);
		spin_unlock_irq(&lp->lock);
	} else {
		if (cmd->autoneg != AUTONEG_DISABLE ||
		    cmd->speed != SPEED_10 ||
		    (cmd->duplex != DUPLEX_HALF && cmd->duplex != DUPLEX_FULL) ||
		    (cmd->port != PORT_TP && cmd->port != PORT_AUI))
			return -EINVAL;

//		lp->port = cmd->port;
		lp->ctl_rfduplx = cmd->duplex == DUPLEX_FULL;

//		if (netif_running(dev))
//			smc_set_port(dev);

		ret = 0;
	}

	return ret;
}

static void
smc_ethtool_getdrvinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
	strncpy(info->driver, CARDNAME, sizeof(info->driver));
	strncpy(info->version, version, sizeof(info->version));
	strncpy(info->bus_info, dev->class_dev.dev->bus_id, sizeof(info->bus_info));
}

static int smc_ethtool_nwayreset(struct net_device *dev)
{
	struct smc_local *lp = netdev_priv(dev);
	int ret = -EINVAL;

	if (lp->phy_type != 0) {
		spin_lock_irq(&lp->lock);
		ret = mii_nway_restart(&lp->mii);
		spin_unlock_irq(&lp->lock);
	}

	return ret;
}

static u32 smc_ethtool_getmsglevel(struct net_device *dev)
{
	struct smc_local *lp = netdev_priv(dev);
	return lp->msg_enable;
}

static void smc_ethtool_setmsglevel(struct net_device *dev, u32 level)
{
	struct smc_local *lp = netdev_priv(dev);
	lp->msg_enable = level;
}

static struct ethtool_ops smc_ethtool_ops = {
	.get_settings	= smc_ethtool_getsettings,
	.set_settings	= smc_ethtool_setsettings,
	.get_drvinfo	= smc_ethtool_getdrvinfo,

	.get_msglevel	= smc_ethtool_getmsglevel,
	.set_msglevel	= smc_ethtool_setmsglevel,
	.nway_reset	= smc_ethtool_nwayreset,
	.get_link	= ethtool_op_get_link,
//	.get_eeprom	= smc_ethtool_geteeprom,
//	.set_eeprom	= smc_ethtool_seteeprom,
};

/*
 * smc_findirq
 *
 * This routine has a simple purpose -- make the SMC chip generate an
 * interrupt, so an auto-detect routine can detect it, and find the IRQ,
 */
/*
 * does this still work?
 *
 * I just deleted auto_irq.c, since it was never built...
 *   --jgarzik
 */
static int __init smc_findirq(unsigned long ioaddr)
{
	int timeout = 20;
	unsigned long cookie;

	DBG(2, "%s: %s\n", CARDNAME, __FUNCTION__);

	cookie = probe_irq_on();

	/*
	 * What I try to do here is trigger an ALLOC_INT. This is done
	 * by allocating a small chunk of memory, which will give an interrupt
	 * when done.
	 */
	/* enable ALLOCation interrupts ONLY */
	SMC_SELECT_BANK(2);
	SMC_SET_INT_MASK(IM_ALLOC_INT);

	/*
 	 * Allocate 512 bytes of memory.  Note that the chip was just
	 * reset so all the memory is available
	 */
	SMC_SET_MMU_CMD(MC_ALLOC | 1);

	/*
	 * Wait until positive that the interrupt has been generated
	 */
	do {
		int int_status;
		udelay(10);
		int_status = SMC_GET_INT();
		if (int_status & IM_ALLOC_INT)
			break;		/* got the interrupt */
	} while (--timeout);

	/*
	 * there is really nothing that I can do here if timeout fails,
	 * as autoirq_report will return a 0 anyway, which is what I
	 * want in this case.   Plus, the clean up is needed in both
	 * cases.
	 */

	/* and disable all interrupts again */
	SMC_SET_INT_MASK(0);

	/* and return what I found */
	return probe_irq_off(cookie);
}

/*
 * Function: smc_probe(unsigned long ioaddr)
 *
 * Purpose:
 *	Tests to see if a given ioaddr points to an SMC91x chip.
 *	Returns a 0 on success
 *
 * Algorithm:
 *	(1) see if the high byte of BANK_SELECT is 0x33
 * 	(2) compare the ioaddr with the base register's address
 *	(3) see if I recognize the chip ID in the appropriate register
 *
 * Here I do typical initialization tasks.
 *
 * o  Initialize the structure if needed
 * o  print out my vanity message if not done so already
 * o  print out what type of hardware is detected
 * o  print out the ethernet address
 * o  find the IRQ
 * o  set up my private data
 * o  configure the dev structure with my subroutines
 * o  actually GRAB the irq.
 * o  GRAB the region
 */
static int __init smc_probe(struct net_device *dev, unsigned long ioaddr)
{
	struct smc_local *lp = netdev_priv(dev);
	static int version_printed = 0;
	int i, retval;
	unsigned int val, revision_register;
	const char *version_string;

	DBG(2, "%s: %s\n", CARDNAME, __FUNCTION__);

	/* First, see if the high byte is 0x33 */
	val = SMC_CURRENT_BANK();
	DBG(2, "%s: bank signature probe returned 0x%04x\n", CARDNAME, val);
	if ((val & 0xFF00) != 0x3300) {
		if ((val & 0xFF) == 0x33) {
			printk(KERN_WARNING
				"%s: Detected possible byte-swapped interface"
				" at IOADDR 0x%lx\n", CARDNAME, ioaddr);
		}
		retval = -ENODEV;
		goto err_out;
	}

	/*
	 * The above MIGHT indicate a device, but I need to write to
	 * further test this.
	 */
	SMC_SELECT_BANK(0);
	val = SMC_CURRENT_BANK();
	if ((val & 0xFF00) != 0x3300) {
		retval = -ENODEV;
		goto err_out;
	}

	/*
	 * well, we've already written once, so hopefully another
	 * time won't hurt.  This time, I need to switch the bank
	 * register to bank 1, so I can access the base address
	 * register
	 */
	SMC_SELECT_BANK(1);
	val = SMC_GET_BASE();
	val = ((val & 0x1F00) >> 3) << SMC_IO_SHIFT;
	if ((ioaddr & ((PAGE_SIZE-1)<<SMC_IO_SHIFT)) != val) {
		printk("%s: IOADDR %lx doesn't match configuration (%x).\n",
			CARDNAME, ioaddr, val);
	}

	/*
	 * check if the revision register is something that I
	 * recognize.  These might need to be added to later,
	 * as future revisions could be added.
	 */
	SMC_SELECT_BANK(3);
	revision_register = SMC_GET_REV();
	DBG(2, "%s: revision = 0x%04x\n", CARDNAME, revision_register);
	version_string = chip_ids[ (revision_register >> 4) & 0xF];
	if (!version_string || (revision_register & 0xff00) != 0x3300) {
		/* I don't recognize this chip, so... */
		printk("%s: IO 0x%lx: Unrecognized revision register 0x%04x"
			", Contact author.\n", CARDNAME,
			ioaddr, revision_register);

		retval = -ENODEV;
		goto err_out;
	}

	/* At this point I'll assume that the chip is an SMC91x. */
	if (version_printed++ == 0)
		printk("%s", version);

	/* fill in some of the fields */
	dev->base_addr = ioaddr;
	lp->version = revision_register & 0xff;

	/* Get the MAC address */
	SMC_SELECT_BANK(1);
	SMC_GET_MAC_ADDR(dev->dev_addr);

	/* now, reset the chip, and put it into a known state */
	smc_reset(dev);

	/*
	 * If dev->irq is 0, then the device has to be banged on to see
	 * what the IRQ is.
 	 *
	 * This banging doesn't always detect the IRQ, for unknown reasons.
	 * a workaround is to reset the chip and try again.
	 *
	 * Interestingly, the DOS packet driver *SETS* the IRQ on the card to
	 * be what is requested on the command line.   I don't do that, mostly
	 * because the card that I have uses a non-standard method of accessing
	 * the IRQs, and because this _should_ work in most configurations.
	 *
	 * Specifying an IRQ is done with the assumption that the user knows
	 * what (s)he is doing.  No checking is done!!!!
	 */
	if (dev->irq < 1) {
		int trials;

		trials = 3;
		while (trials--) {
			dev->irq = smc_findirq(ioaddr);
			if (dev->irq)
				break;
			/* kick the card and try again */
			smc_reset(dev);
		}
	}
	if (dev->irq == 0) {
		printk("%s: Couldn't autodetect your IRQ. Use irq=xx.\n",
			dev->name);
		retval = -ENODEV;
		goto err_out;
	}
	dev->irq = irq_canonicalize(dev->irq);

	/* Fill in the fields of the device structure with ethernet values. */
	ether_setup(dev);

	dev->open = smc_open;
	dev->stop = smc_close;
	dev->hard_start_xmit = smc_hard_start_xmit;
	dev->tx_timeout = smc_timeout;
	dev->watchdog_timeo = msecs_to_jiffies(watchdog);
	dev->get_stats = smc_query_statistics;
	dev->set_multicast_list = smc_set_multicast_list;
	dev->ethtool_ops = &smc_ethtool_ops;

	spin_lock_init(&lp->lock);
	lp->mii.phy_id_mask = 0x1f;
	lp->mii.reg_num_mask = 0x1f;
	lp->mii.force_media = 0;
	lp->mii.full_duplex = 0;
	lp->mii.dev = dev;
	lp->mii.mdio_read = smc_phy_read;
	lp->mii.mdio_write = smc_phy_write;

	/*
	 * Locate the phy, if any.
	 */
	if (lp->version >= (CHIP_91100 << 4))
		smc_detect_phy(dev);

	/* Set default parameters */
	lp->msg_enable = NETIF_MSG_LINK;
	lp->ctl_rfduplx = 0;
	lp->ctl_rspeed = 10;

	if (lp->version >= (CHIP_91100 << 4)) {
		lp->ctl_rfduplx = 1;
		lp->ctl_rspeed = 100;
	}

	/* Grab the IRQ */
      	retval = request_irq(dev->irq, &smc_interrupt, 0, dev->name, dev);
      	if (retval)
      		goto err_out;

	set_irq_type(dev->irq, IRQT_RISING);
#ifdef SMC_USE_PXA_DMA
	{
		int dma = pxa_request_dma(dev->name, DMA_PRIO_LOW,
					  smc_pxa_dma_irq, NULL);
		if (dma >= 0)
			dev->dma = dma;
	}
#endif

	retval = register_netdev(dev);
	if (retval == 0) {
		/* now, print out the card info, in a short format.. */
		printk("%s: %s (rev %d) at %#lx IRQ %d",
			dev->name, version_string, revision_register & 0x0f,
			dev->base_addr, dev->irq);

		if (dev->dma != (unsigned char)-1)
			printk(" DMA %d", dev->dma);

		printk("%s%s\n", nowait ? " [nowait]" : "",
			THROTTLE_TX_PKTS ? " [throttle_tx]" : "");

		if (!is_valid_ether_addr(dev->dev_addr)) {
			printk("%s: Invalid ethernet MAC address.  Please "
			       "set using ifconfig\n", dev->name);
		} else {
			/* Print the Ethernet address */
			printk("%s: Ethernet addr: ", dev->name);
			for (i = 0; i < 5; i++)
				printk("%2.2x:", dev->dev_addr[i]);
			printk("%2.2x\n", dev->dev_addr[5]);
		}

		if (lp->phy_type == 0) {
			PRINTK("%s: No PHY found\n", dev->name);
		} else if ((lp->phy_type & 0xfffffff0) == 0x0016f840) {
			PRINTK("%s: PHY LAN83C183 (LAN91C111 Internal)\n", dev->name);
		} else if ((lp->phy_type & 0xfffffff0) == 0x02821c50) {
			PRINTK("%s: PHY LAN83C180\n", dev->name);
		}
	}

err_out:
#ifdef SMC_USE_PXA_DMA
	if (retval && dev->dma != (unsigned char)-1)
		pxa_free_dma(dev->dma);
#endif
	return retval;
}

static int smc_enable_device(unsigned long attrib_phys)
{
	unsigned long flags;
	unsigned char ecor, ecsr;
	void *addr;

	/*
	 * Map the attribute space.  This is overkill, but clean.
	 */
	addr = ioremap(attrib_phys, ATTRIB_SIZE);
	if (!addr)
		return -ENOMEM;

	/*
	 * Reset the device.  We must disable IRQs around this
	 * since a reset causes the IRQ line become active.
	 */
	local_irq_save(flags);
	ecor = readb(addr + (ECOR << SMC_IO_SHIFT)) & ~ECOR_RESET;
	writeb(ecor | ECOR_RESET, addr + (ECOR << SMC_IO_SHIFT));
	readb(addr + (ECOR << SMC_IO_SHIFT));

	/*
	 * Wait 100us for the chip to reset.
	 */
	udelay(100);

	/*
	 * The device will ignore all writes to the enable bit while
	 * reset is asserted, even if the reset bit is cleared in the
	 * same write.  Must clear reset first, then enable the device.
	 */
	writeb(ecor, addr + (ECOR << SMC_IO_SHIFT));
	writeb(ecor | ECOR_ENABLE, addr + (ECOR << SMC_IO_SHIFT));

	/*
	 * Set the appropriate byte/word mode.
	 */
	ecsr = readb(addr + (ECSR << SMC_IO_SHIFT)) & ~ECSR_IOIS8;
#ifndef SMC_CAN_USE_16BIT
	ecsr |= ECSR_IOIS8;
#endif
	writeb(ecsr, addr + (ECSR << SMC_IO_SHIFT));
	local_irq_restore(flags);

	iounmap(addr);

	/*
	 * Wait for the chip to wake up.  We could poll the control
	 * register in the main register space, but that isn't mapped
	 * yet.  We know this is going to take 750us.
	 */
	msleep(1);

	return 0;
}

/*
 * smc_init(void)
 *   Input parameters:
 *	dev->base_addr == 0, try to find all possible locations
 *	dev->base_addr > 0x1ff, this is the address to check
 *	dev->base_addr == <anything else>, return failure code
 *
 *   Output:
 *	0 --> there is a device
 *	anything else, error
 */
static int smc_drv_probe(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct net_device *ndev;
	struct resource *res, *ext = NULL;
	unsigned int *addr;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -ENODEV;
		goto out;
	}

	/*
	 * Request the regions.
	 */
	if (!request_mem_region(res->start, SMC_IO_EXTENT, "smc91x")) {
		ret = -EBUSY;
		goto out;
	}

	ndev = alloc_etherdev(sizeof(struct smc_local));
	if (!ndev) {
		printk("%s: could not allocate device.\n", CARDNAME);
		ret = -ENOMEM;
		goto release_1;
	}
	SET_MODULE_OWNER(ndev);
	SET_NETDEV_DEV(ndev, dev);

	ndev->dma = (unsigned char)-1;
	ndev->irq = platform_get_irq(pdev, 0);

	ext = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (ext) {
		if (!request_mem_region(ext->start, ATTRIB_SIZE, ndev->name)) {
			ret = -EBUSY;
			goto release_1;
		}

#if defined(CONFIG_SA1100_ASSABET)
		NCR_0 |= NCR_ENET_OSC_EN;
#endif

		ret = smc_enable_device(ext->start);
		if (ret)
			goto release_both;
	}

	addr = ioremap(res->start, SMC_IO_EXTENT);
	if (!addr) {
		ret = -ENOMEM;
		goto release_both;
	}

	dev_set_drvdata(dev, ndev);
	ret = smc_probe(ndev, (unsigned long)addr);
	if (ret != 0) {
		dev_set_drvdata(dev, NULL);
		iounmap(addr);
 release_both:
		if (ext)
			release_mem_region(ext->start, ATTRIB_SIZE);
		free_netdev(ndev);
 release_1:
		release_mem_region(res->start, SMC_IO_EXTENT);
 out:
		printk("%s: not found (%d).\n", CARDNAME, ret);
	}
#ifdef SMC_USE_PXA_DMA
	else {
		struct smc_local *lp = netdev_priv(ndev);
		lp->physaddr = res->start;
	}
#endif

	return ret;
}

static int smc_drv_remove(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct net_device *ndev = dev_get_drvdata(dev);
	struct resource *res;

	dev_set_drvdata(dev, NULL);

	unregister_netdev(ndev);

	free_irq(ndev->irq, ndev);

#ifdef SMC_USE_PXA_DMA
	if (ndev->dma != (unsigned char)-1)
		pxa_free_dma(ndev->dma);
#endif
	iounmap((void *)ndev->base_addr);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (res)
		release_mem_region(res->start, ATTRIB_SIZE);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	release_mem_region(res->start, SMC_IO_EXTENT);

	free_netdev(ndev);

	return 0;
}

static int smc_drv_suspend(struct device *dev, u32 state, u32 level)
{
	struct net_device *ndev = dev_get_drvdata(dev);

	if (ndev && level == SUSPEND_DISABLE) {
		if (netif_running(ndev)) {
			netif_device_detach(ndev);
			smc_shutdown(ndev->base_addr);
		}
	}
	return 0;
}

static int smc_drv_resume(struct device *dev, u32 level)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct net_device *ndev = dev_get_drvdata(dev);

	if (ndev && level == RESUME_ENABLE) {
		struct smc_local *lp = netdev_priv(ndev);
		unsigned long ioaddr = ndev->base_addr;

		if (pdev->num_resources == 3)
			smc_enable_device(pdev->resource[2].start);
		if (netif_running(ndev)) {
			smc_reset(ndev);
			smc_enable(ndev);
			SMC_SELECT_BANK(1);
			SMC_SET_MAC_ADDR(ndev->dev_addr);
			if (lp->phy_type != 0)
				smc_phy_configure(ndev);
			netif_device_attach(ndev);
		}
	}
	return 0;
}

static struct device_driver smc_driver = {
	.name		= CARDNAME,
	.bus		= &platform_bus_type,
	.probe		= smc_drv_probe,
	.remove		= smc_drv_remove,
	.suspend	= smc_drv_suspend,
	.resume		= smc_drv_resume,
};

static int __init smc_init(void)
{
#ifdef MODULE
	if (io == -1)
		printk(KERN_WARNING 
			"%s: You shouldn't use auto-probing with insmod!\n",
			CARDNAME);
#endif

	return driver_register(&smc_driver);
}

static void __exit smc_cleanup(void)
{
	driver_unregister(&smc_driver);
}

module_init(smc_init);
module_exit(smc_cleanup);
