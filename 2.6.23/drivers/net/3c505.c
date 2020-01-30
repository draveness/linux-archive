/*
 * Linux Ethernet device driver for the 3Com Etherlink Plus (3C505)
 *      By Craig Southeren, Juha Laiho and Philip Blundell
 *
 * 3c505.c      This module implements an interface to the 3Com
 *              Etherlink Plus (3c505) Ethernet card. Linux device
 *              driver interface reverse engineered from the Linux 3C509
 *              device drivers. Some 3C505 information gleaned from
 *              the Crynwr packet driver. Still this driver would not
 *              be here without 3C505 technical reference provided by
 *              3Com.
 *
 * $Id: 3c505.c,v 1.10 1996/04/16 13:06:27 phil Exp $
 *
 * Authors:     Linux 3c505 device driver by
 *                      Craig Southeren, <craigs@ineluki.apana.org.au>
 *              Final debugging by
 *                      Andrew Tridgell, <tridge@nimbus.anu.edu.au>
 *              Auto irq/address, tuning, cleanup and v1.1.4+ kernel mods by
 *                      Juha Laiho, <jlaiho@ichaos.nullnet.fi>
 *              Linux 3C509 driver by
 *                      Donald Becker, <becker@super.org>
 *			(Now at <becker@scyld.com>)
 *              Crynwr packet driver by
 *                      Krishnan Gopalan and Gregg Stefancik,
 *                      Clemson University Engineering Computer Operations.
 *                      Portions of the code have been adapted from the 3c505
 *                         driver for NCSA Telnet by Bruce Orchard and later
 *                         modified by Warren Van Houten and krus@diku.dk.
 *              3C505 technical information provided by
 *                      Terry Murphy, of 3Com Network Adapter Division
 *              Linux 1.3.0 changes by
 *                      Alan Cox <Alan.Cox@linux.org>
 *              More debugging, DMA support, currently maintained by
 *                      Philip Blundell <philb@gnu.org>
 *              Multicard/soft configurable dma channel/rev 2 hardware support
 *                      by Christopher Collins <ccollins@pcug.org.au>
 *		Ethtool support (jgarzik), 11/17/2001
 */

#define DRV_NAME	"3c505"
#define DRV_VERSION	"1.10a"


/* Theory of operation:
 *
 * The 3c505 is quite an intelligent board.  All communication with it is done
 * by means of Primary Command Blocks (PCBs); these are transferred using PIO
 * through the command register.  The card has 256k of on-board RAM, which is
 * used to buffer received packets.  It might seem at first that more buffers
 * are better, but in fact this isn't true.  From my tests, it seems that
 * more than about 10 buffers are unnecessary, and there is a noticeable
 * performance hit in having more active on the card.  So the majority of the
 * card's memory isn't, in fact, used.  Sadly, the card only has one transmit
 * buffer and, short of loading our own firmware into it (which is what some
 * drivers resort to) there's nothing we can do about this.
 *
 * We keep up to 4 "receive packet" commands active on the board at a time.
 * When a packet comes in, so long as there is a receive command active, the
 * board will send us a "packet received" PCB and then add the data for that
 * packet to the DMA queue.  If a DMA transfer is not already in progress, we
 * set one up to start uploading the data.  We have to maintain a list of
 * backlogged receive packets, because the card may decide to tell us about
 * a newly-arrived packet at any time, and we may not be able to start a DMA
 * transfer immediately (ie one may already be going on).  We can't NAK the
 * PCB, because then it would throw the packet away.
 *
 * Trying to send a PCB to the card at the wrong moment seems to have bad
 * effects.  If we send it a transmit PCB while a receive DMA is happening,
 * it will just NAK the PCB and so we will have wasted our time.  Worse, it
 * sometimes seems to interrupt the transfer.  The majority of the low-level
 * code is protected by one huge semaphore -- "busy" -- which is set whenever
 * it probably isn't safe to do anything to the card.  The receive routine
 * must gain a lock on "busy" before it can start a DMA transfer, and the
 * transmit routine must gain a lock before it sends the first PCB to the card.
 * The send_pcb() routine also has an internal semaphore to protect it against
 * being re-entered (which would be disastrous) -- this is needed because
 * several things can happen asynchronously (re-priming the receiver and
 * asking the card for statistics, for example).  send_pcb() will also refuse
 * to talk to the card at all if a DMA upload is happening.  The higher-level
 * networking code will reschedule a later retry if some part of the driver
 * is blocked.  In practice, this doesn't seem to happen very often.
 */

/* This driver may now work with revision 2.x hardware, since all the read
 * operations on the HCR have been removed (we now keep our own softcopy).
 * But I don't have an old card to test it on.
 *
 * This has had the bad effect that the autoprobe routine is now a bit
 * less friendly to other devices.  However, it was never very good.
 * before, so I doubt it will hurt anybody.
 */

/* The driver is a mess.  I took Craig's and Juha's code, and hacked it firstly
 * to make it more reliable, and secondly to add DMA mode.  Many things could
 * probably be done better; the concurrency protection is particularly awful.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/in.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/spinlock.h>
#include <linux/ethtool.h>
#include <linux/delay.h>
#include <linux/bitops.h>

#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/dma.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/init.h>

#include "3c505.h"

/*********************************************************
 *
 *  define debug messages here as common strings to reduce space
 *
 *********************************************************/

static const char filename[] = __FILE__;

static const char timeout_msg[] = "*** timeout at %s:%s (line %d) ***\n";
#define TIMEOUT_MSG(lineno) \
	printk(timeout_msg, filename,__FUNCTION__,(lineno))

static const char invalid_pcb_msg[] =
"*** invalid pcb length %d at %s:%s (line %d) ***\n";
#define INVALID_PCB_MSG(len) \
	printk(invalid_pcb_msg, (len),filename,__FUNCTION__,__LINE__)

static char search_msg[] __initdata = KERN_INFO "%s: Looking for 3c505 adapter at address %#x...";

static char stilllooking_msg[] __initdata = "still looking...";

static char found_msg[] __initdata = "found.\n";

static char notfound_msg[] __initdata = "not found (reason = %d)\n";

static char couldnot_msg[] __initdata = KERN_INFO "%s: 3c505 not found\n";

/*********************************************************
 *
 *  various other debug stuff
 *
 *********************************************************/

#ifdef ELP_DEBUG
static int elp_debug = ELP_DEBUG;
#else
static int elp_debug;
#endif
#define debug elp_debug

/*
 *  0 = no messages (well, some)
 *  1 = messages when high level commands performed
 *  2 = messages when low level commands performed
 *  3 = messages when interrupts received
 */

/*****************************************************************
 *
 * useful macros
 *
 *****************************************************************/

#ifndef	TRUE
#define	TRUE	1
#endif

#ifndef	FALSE
#define	FALSE	0
#endif


/*****************************************************************
 *
 * List of I/O-addresses we try to auto-sense
 * Last element MUST BE 0!
 *****************************************************************/

static int addr_list[] __initdata = {0x300, 0x280, 0x310, 0};

/* Dma Memory related stuff */

static unsigned long dma_mem_alloc(int size)
{
	int order = get_order(size);
	return __get_dma_pages(GFP_KERNEL, order);
}


/*****************************************************************
 *
 * Functions for I/O (note the inline !)
 *
 *****************************************************************/

static inline unsigned char inb_status(unsigned int base_addr)
{
	return inb(base_addr + PORT_STATUS);
}

static inline int inb_command(unsigned int base_addr)
{
	return inb(base_addr + PORT_COMMAND);
}

static inline void outb_control(unsigned char val, struct net_device *dev)
{
	outb(val, dev->base_addr + PORT_CONTROL);
	((elp_device *)(dev->priv))->hcr_val = val;
}

#define HCR_VAL(x)   (((elp_device *)((x)->priv))->hcr_val)

static inline void outb_command(unsigned char val, unsigned int base_addr)
{
	outb(val, base_addr + PORT_COMMAND);
}

static inline unsigned int backlog_next(unsigned int n)
{
	return (n + 1) % BACKLOG_SIZE;
}

/*****************************************************************
 *
 *  useful functions for accessing the adapter
 *
 *****************************************************************/

/*
 * use this routine when accessing the ASF bits as they are
 * changed asynchronously by the adapter
 */

/* get adapter PCB status */
#define	GET_ASF(addr) \
	(get_status(addr)&ASF_PCB_MASK)

static inline int get_status(unsigned int base_addr)
{
	unsigned long timeout = jiffies + 10*HZ/100;
	register int stat1;
	do {
		stat1 = inb_status(base_addr);
	} while (stat1 != inb_status(base_addr) && time_before(jiffies, timeout));
	if (time_after_eq(jiffies, timeout))
		TIMEOUT_MSG(__LINE__);
	return stat1;
}

static inline void set_hsf(struct net_device *dev, int hsf)
{
	elp_device *adapter = dev->priv;
	unsigned long flags;

	spin_lock_irqsave(&adapter->lock, flags);
	outb_control((HCR_VAL(dev) & ~HSF_PCB_MASK) | hsf, dev);
	spin_unlock_irqrestore(&adapter->lock, flags);
}

static int start_receive(struct net_device *, pcb_struct *);

static inline void adapter_reset(struct net_device *dev)
{
	unsigned long timeout;
	elp_device *adapter = dev->priv;
	unsigned char orig_hcr = adapter->hcr_val;

	outb_control(0, dev);

	if (inb_status(dev->base_addr) & ACRF) {
		do {
			inb_command(dev->base_addr);
			timeout = jiffies + 2*HZ/100;
			while (time_before_eq(jiffies, timeout) && !(inb_status(dev->base_addr) & ACRF));
		} while (inb_status(dev->base_addr) & ACRF);
		set_hsf(dev, HSF_PCB_NAK);
	}
	outb_control(adapter->hcr_val | ATTN | DIR, dev);
	mdelay(10);
	outb_control(adapter->hcr_val & ~ATTN, dev);
	mdelay(10);
	outb_control(adapter->hcr_val | FLSH, dev);
	mdelay(10);
	outb_control(adapter->hcr_val & ~FLSH, dev);
	mdelay(10);

	outb_control(orig_hcr, dev);
	if (!start_receive(dev, &adapter->tx_pcb))
		printk(KERN_ERR "%s: start receive command failed \n", dev->name);
}

/* Check to make sure that a DMA transfer hasn't timed out.  This should
 * never happen in theory, but seems to occur occasionally if the card gets
 * prodded at the wrong time.
 */
static inline void check_3c505_dma(struct net_device *dev)
{
	elp_device *adapter = dev->priv;
	if (adapter->dmaing && time_after(jiffies, adapter->current_dma.start_time + 10)) {
		unsigned long flags, f;
		printk(KERN_ERR "%s: DMA %s timed out, %d bytes left\n", dev->name, adapter->current_dma.direction ? "download" : "upload", get_dma_residue(dev->dma));
		spin_lock_irqsave(&adapter->lock, flags);
		adapter->dmaing = 0;
		adapter->busy = 0;

		f=claim_dma_lock();
		disable_dma(dev->dma);
		release_dma_lock(f);

		if (adapter->rx_active)
			adapter->rx_active--;
		outb_control(adapter->hcr_val & ~(DMAE | TCEN | DIR), dev);
		spin_unlock_irqrestore(&adapter->lock, flags);
	}
}

/* Primitive functions used by send_pcb() */
static inline unsigned int send_pcb_slow(unsigned int base_addr, unsigned char byte)
{
	unsigned long timeout;
	outb_command(byte, base_addr);
	for (timeout = jiffies + 5*HZ/100; time_before(jiffies, timeout);) {
		if (inb_status(base_addr) & HCRE)
			return FALSE;
	}
	printk(KERN_WARNING "3c505: send_pcb_slow timed out\n");
	return TRUE;
}

static inline unsigned int send_pcb_fast(unsigned int base_addr, unsigned char byte)
{
	unsigned int timeout;
	outb_command(byte, base_addr);
	for (timeout = 0; timeout < 40000; timeout++) {
		if (inb_status(base_addr) & HCRE)
			return FALSE;
	}
	printk(KERN_WARNING "3c505: send_pcb_fast timed out\n");
	return TRUE;
}

/* Check to see if the receiver needs restarting, and kick it if so */
static inline void prime_rx(struct net_device *dev)
{
	elp_device *adapter = dev->priv;
	while (adapter->rx_active < ELP_RX_PCBS && netif_running(dev)) {
		if (!start_receive(dev, &adapter->itx_pcb))
			break;
	}
}

/*****************************************************************
 *
 * send_pcb
 *   Send a PCB to the adapter.
 *
 *	output byte to command reg  --<--+
 *	wait until HCRE is non zero      |
 *	loop until all bytes sent   -->--+
 *	set HSF1 and HSF2 to 1
 *	output pcb length
 *	wait until ASF give ACK or NAK
 *	set HSF1 and HSF2 to 0
 *
 *****************************************************************/

/* This can be quite slow -- the adapter is allowed to take up to 40ms
 * to respond to the initial interrupt.
 *
 * We run initially with interrupts turned on, but with a semaphore set
 * so that nobody tries to re-enter this code.  Once the first byte has
 * gone through, we turn interrupts off and then send the others (the
 * timeout is reduced to 500us).
 */

static int send_pcb(struct net_device *dev, pcb_struct * pcb)
{
	int i;
	unsigned long timeout;
	elp_device *adapter = dev->priv;
	unsigned long flags;

	check_3c505_dma(dev);

	if (adapter->dmaing && adapter->current_dma.direction == 0)
		return FALSE;

	/* Avoid contention */
	if (test_and_set_bit(1, &adapter->send_pcb_semaphore)) {
		if (elp_debug >= 3) {
			printk(KERN_DEBUG "%s: send_pcb entered while threaded\n", dev->name);
		}
		return FALSE;
	}
	/*
	 * load each byte into the command register and
	 * wait for the HCRE bit to indicate the adapter
	 * had read the byte
	 */
	set_hsf(dev, 0);

	if (send_pcb_slow(dev->base_addr, pcb->command))
		goto abort;

	spin_lock_irqsave(&adapter->lock, flags);

	if (send_pcb_fast(dev->base_addr, pcb->length))
		goto sti_abort;

	for (i = 0; i < pcb->length; i++) {
		if (send_pcb_fast(dev->base_addr, pcb->data.raw[i]))
			goto sti_abort;
	}

	outb_control(adapter->hcr_val | 3, dev);	/* signal end of PCB */
	outb_command(2 + pcb->length, dev->base_addr);

	/* now wait for the acknowledgement */
	spin_unlock_irqrestore(&adapter->lock, flags);

	for (timeout = jiffies + 5*HZ/100; time_before(jiffies, timeout);) {
		switch (GET_ASF(dev->base_addr)) {
		case ASF_PCB_ACK:
			adapter->send_pcb_semaphore = 0;
			return TRUE;

		case ASF_PCB_NAK:
#ifdef ELP_DEBUG
			printk(KERN_DEBUG "%s: send_pcb got NAK\n", dev->name);
#endif
			goto abort;
		}
	}

	if (elp_debug >= 1)
		printk(KERN_DEBUG "%s: timeout waiting for PCB acknowledge (status %02x)\n", dev->name, inb_status(dev->base_addr));
	goto abort;

      sti_abort:
	spin_unlock_irqrestore(&adapter->lock, flags);
      abort:
	adapter->send_pcb_semaphore = 0;
	return FALSE;
}


/*****************************************************************
 *
 * receive_pcb
 *   Read a PCB from the adapter
 *
 *	wait for ACRF to be non-zero        ---<---+
 *	input a byte                               |
 *	if ASF1 and ASF2 were not both one         |
 *		before byte was read, loop      --->---+
 *	set HSF1 and HSF2 for ack
 *
 *****************************************************************/

static int receive_pcb(struct net_device *dev, pcb_struct * pcb)
{
	int i, j;
	int total_length;
	int stat;
	unsigned long timeout;
	unsigned long flags;

	elp_device *adapter = dev->priv;

	set_hsf(dev, 0);

	/* get the command code */
	timeout = jiffies + 2*HZ/100;
	while (((stat = get_status(dev->base_addr)) & ACRF) == 0 && time_before(jiffies, timeout));
	if (time_after_eq(jiffies, timeout)) {
		TIMEOUT_MSG(__LINE__);
		return FALSE;
	}
	pcb->command = inb_command(dev->base_addr);

	/* read the data length */
	timeout = jiffies + 3*HZ/100;
	while (((stat = get_status(dev->base_addr)) & ACRF) == 0 && time_before(jiffies, timeout));
	if (time_after_eq(jiffies, timeout)) {
		TIMEOUT_MSG(__LINE__);
		printk(KERN_INFO "%s: status %02x\n", dev->name, stat);
		return FALSE;
	}
	pcb->length = inb_command(dev->base_addr);

	if (pcb->length > MAX_PCB_DATA) {
		INVALID_PCB_MSG(pcb->length);
		adapter_reset(dev);
		return FALSE;
	}
	/* read the data */
	spin_lock_irqsave(&adapter->lock, flags);
	i = 0;
	do {
		j = 0;
		while (((stat = get_status(dev->base_addr)) & ACRF) == 0 && j++ < 20000);
		pcb->data.raw[i++] = inb_command(dev->base_addr);
		if (i > MAX_PCB_DATA)
			INVALID_PCB_MSG(i);
	} while ((stat & ASF_PCB_MASK) != ASF_PCB_END && j < 20000);
	spin_unlock_irqrestore(&adapter->lock, flags);
	if (j >= 20000) {
		TIMEOUT_MSG(__LINE__);
		return FALSE;
	}
	/* woops, the last "data" byte was really the length! */
	total_length = pcb->data.raw[--i];

	/* safety check total length vs data length */
	if (total_length != (pcb->length + 2)) {
		if (elp_debug >= 2)
			printk(KERN_WARNING "%s: mangled PCB received\n", dev->name);
		set_hsf(dev, HSF_PCB_NAK);
		return FALSE;
	}

	if (pcb->command == CMD_RECEIVE_PACKET_COMPLETE) {
		if (test_and_set_bit(0, (void *) &adapter->busy)) {
			if (backlog_next(adapter->rx_backlog.in) == adapter->rx_backlog.out) {
				set_hsf(dev, HSF_PCB_NAK);
				printk(KERN_WARNING "%s: PCB rejected, transfer in progress and backlog full\n", dev->name);
				pcb->command = 0;
				return TRUE;
			} else {
				pcb->command = 0xff;
			}
		}
	}
	set_hsf(dev, HSF_PCB_ACK);
	return TRUE;
}

/******************************************************
 *
 *  queue a receive command on the adapter so we will get an
 *  interrupt when a packet is received.
 *
 ******************************************************/

static int start_receive(struct net_device *dev, pcb_struct * tx_pcb)
{
	int status;
	elp_device *adapter = dev->priv;

	if (elp_debug >= 3)
		printk(KERN_DEBUG "%s: restarting receiver\n", dev->name);
	tx_pcb->command = CMD_RECEIVE_PACKET;
	tx_pcb->length = sizeof(struct Rcv_pkt);
	tx_pcb->data.rcv_pkt.buf_seg
	    = tx_pcb->data.rcv_pkt.buf_ofs = 0;		/* Unused */
	tx_pcb->data.rcv_pkt.buf_len = 1600;
	tx_pcb->data.rcv_pkt.timeout = 0;	/* set timeout to zero */
	status = send_pcb(dev, tx_pcb);
	if (status)
		adapter->rx_active++;
	return status;
}

/******************************************************
 *
 * extract a packet from the adapter
 * this routine is only called from within the interrupt
 * service routine, so no cli/sti calls are needed
 * note that the length is always assumed to be even
 *
 ******************************************************/

static void receive_packet(struct net_device *dev, int len)
{
	int rlen;
	elp_device *adapter = dev->priv;
	void *target;
	struct sk_buff *skb;
	unsigned long flags;

	rlen = (len + 1) & ~1;
	skb = dev_alloc_skb(rlen + 2);

	if (!skb) {
		printk(KERN_WARNING "%s: memory squeeze, dropping packet\n", dev->name);
		target = adapter->dma_buffer;
		adapter->current_dma.target = NULL;
		/* FIXME: stats */
		return;
	}

	skb_reserve(skb, 2);
	target = skb_put(skb, rlen);
	if ((unsigned long)(target + rlen) >= MAX_DMA_ADDRESS) {
		adapter->current_dma.target = target;
		target = adapter->dma_buffer;
	} else {
		adapter->current_dma.target = NULL;
	}

	/* if this happens, we die */
	if (test_and_set_bit(0, (void *) &adapter->dmaing))
		printk(KERN_ERR "%s: rx blocked, DMA in progress, dir %d\n", dev->name, adapter->current_dma.direction);

	adapter->current_dma.direction = 0;
	adapter->current_dma.length = rlen;
	adapter->current_dma.skb = skb;
	adapter->current_dma.start_time = jiffies;

	outb_control(adapter->hcr_val | DIR | TCEN | DMAE, dev);

	flags=claim_dma_lock();
	disable_dma(dev->dma);
	clear_dma_ff(dev->dma);
	set_dma_mode(dev->dma, 0x04);	/* dma read */
	set_dma_addr(dev->dma, isa_virt_to_bus(target));
	set_dma_count(dev->dma, rlen);
	enable_dma(dev->dma);
	release_dma_lock(flags);

	if (elp_debug >= 3) {
		printk(KERN_DEBUG "%s: rx DMA transfer started\n", dev->name);
	}

	if (adapter->rx_active)
		adapter->rx_active--;

	if (!adapter->busy)
		printk(KERN_WARNING "%s: receive_packet called, busy not set.\n", dev->name);
}

/******************************************************
 *
 * interrupt handler
 *
 ******************************************************/

static irqreturn_t elp_interrupt(int irq, void *dev_id)
{
	int len;
	int dlen;
	int icount = 0;
	struct net_device *dev;
	elp_device *adapter;
	unsigned long timeout;

	dev = dev_id;
	adapter = (elp_device *) dev->priv;

	spin_lock(&adapter->lock);

	do {
		/*
		 * has a DMA transfer finished?
		 */
		if (inb_status(dev->base_addr) & DONE) {
			if (!adapter->dmaing) {
				printk(KERN_WARNING "%s: phantom DMA completed\n", dev->name);
			}
			if (elp_debug >= 3) {
				printk(KERN_DEBUG "%s: %s DMA complete, status %02x\n", dev->name, adapter->current_dma.direction ? "tx" : "rx", inb_status(dev->base_addr));
			}

			outb_control(adapter->hcr_val & ~(DMAE | TCEN | DIR), dev);
			if (adapter->current_dma.direction) {
				dev_kfree_skb_irq(adapter->current_dma.skb);
			} else {
				struct sk_buff *skb = adapter->current_dma.skb;
				if (skb) {
					if (adapter->current_dma.target) {
				  	/* have already done the skb_put() */
				  	memcpy(adapter->current_dma.target, adapter->dma_buffer, adapter->current_dma.length);
					}
					skb->protocol = eth_type_trans(skb,dev);
					adapter->stats.rx_bytes += skb->len;
					netif_rx(skb);
					dev->last_rx = jiffies;
				}
			}
			adapter->dmaing = 0;
			if (adapter->rx_backlog.in != adapter->rx_backlog.out) {
				int t = adapter->rx_backlog.length[adapter->rx_backlog.out];
				adapter->rx_backlog.out = backlog_next(adapter->rx_backlog.out);
				if (elp_debug >= 2)
					printk(KERN_DEBUG "%s: receiving backlogged packet (%d)\n", dev->name, t);
				receive_packet(dev, t);
			} else {
				adapter->busy = 0;
			}
		} else {
			/* has one timed out? */
			check_3c505_dma(dev);
		}

		/*
		 * receive a PCB from the adapter
		 */
		timeout = jiffies + 3*HZ/100;
		while ((inb_status(dev->base_addr) & ACRF) != 0 && time_before(jiffies, timeout)) {
			if (receive_pcb(dev, &adapter->irx_pcb)) {
				switch (adapter->irx_pcb.command)
				{
				case 0:
					break;
					/*
					 * received a packet - this must be handled fast
					 */
				case 0xff:
				case CMD_RECEIVE_PACKET_COMPLETE:
					/* if the device isn't open, don't pass packets up the stack */
					if (!netif_running(dev))
						break;
					len = adapter->irx_pcb.data.rcv_resp.pkt_len;
					dlen = adapter->irx_pcb.data.rcv_resp.buf_len;
					if (adapter->irx_pcb.data.rcv_resp.timeout != 0) {
						printk(KERN_ERR "%s: interrupt - packet not received correctly\n", dev->name);
					} else {
						if (elp_debug >= 3) {
							printk(KERN_DEBUG "%s: interrupt - packet received of length %i (%i)\n", dev->name, len, dlen);
						}
						if (adapter->irx_pcb.command == 0xff) {
							if (elp_debug >= 2)
								printk(KERN_DEBUG "%s: adding packet to backlog (len = %d)\n", dev->name, dlen);
							adapter->rx_backlog.length[adapter->rx_backlog.in] = dlen;
							adapter->rx_backlog.in = backlog_next(adapter->rx_backlog.in);
						} else {
							receive_packet(dev, dlen);
						}
						if (elp_debug >= 3)
							printk(KERN_DEBUG "%s: packet received\n", dev->name);
					}
					break;

					/*
					 * 82586 configured correctly
					 */
				case CMD_CONFIGURE_82586_RESPONSE:
					adapter->got[CMD_CONFIGURE_82586] = 1;
					if (elp_debug >= 3)
						printk(KERN_DEBUG "%s: interrupt - configure response received\n", dev->name);
					break;

					/*
					 * Adapter memory configuration
					 */
				case CMD_CONFIGURE_ADAPTER_RESPONSE:
					adapter->got[CMD_CONFIGURE_ADAPTER_MEMORY] = 1;
					if (elp_debug >= 3)
						printk(KERN_DEBUG "%s: Adapter memory configuration %s.\n", dev->name,
						       adapter->irx_pcb.data.failed ? "failed" : "succeeded");
					break;

					/*
					 * Multicast list loading
					 */
				case CMD_LOAD_MULTICAST_RESPONSE:
					adapter->got[CMD_LOAD_MULTICAST_LIST] = 1;
					if (elp_debug >= 3)
						printk(KERN_DEBUG "%s: Multicast address list loading %s.\n", dev->name,
						       adapter->irx_pcb.data.failed ? "failed" : "succeeded");
					break;

					/*
					 * Station address setting
					 */
				case CMD_SET_ADDRESS_RESPONSE:
					adapter->got[CMD_SET_STATION_ADDRESS] = 1;
					if (elp_debug >= 3)
						printk(KERN_DEBUG "%s: Ethernet address setting %s.\n", dev->name,
						       adapter->irx_pcb.data.failed ? "failed" : "succeeded");
					break;


					/*
					 * received board statistics
					 */
				case CMD_NETWORK_STATISTICS_RESPONSE:
					adapter->stats.rx_packets += adapter->irx_pcb.data.netstat.tot_recv;
					adapter->stats.tx_packets += adapter->irx_pcb.data.netstat.tot_xmit;
					adapter->stats.rx_crc_errors += adapter->irx_pcb.data.netstat.err_CRC;
					adapter->stats.rx_frame_errors += adapter->irx_pcb.data.netstat.err_align;
					adapter->stats.rx_fifo_errors += adapter->irx_pcb.data.netstat.err_ovrrun;
					adapter->stats.rx_over_errors += adapter->irx_pcb.data.netstat.err_res;
					adapter->got[CMD_NETWORK_STATISTICS] = 1;
					if (elp_debug >= 3)
						printk(KERN_DEBUG "%s: interrupt - statistics response received\n", dev->name);
					break;

					/*
					 * sent a packet
					 */
				case CMD_TRANSMIT_PACKET_COMPLETE:
					if (elp_debug >= 3)
						printk(KERN_DEBUG "%s: interrupt - packet sent\n", dev->name);
					if (!netif_running(dev))
						break;
					switch (adapter->irx_pcb.data.xmit_resp.c_stat) {
					case 0xffff:
						adapter->stats.tx_aborted_errors++;
						printk(KERN_INFO "%s: transmit timed out, network cable problem?\n", dev->name);
						break;
					case 0xfffe:
						adapter->stats.tx_fifo_errors++;
						printk(KERN_INFO "%s: transmit timed out, FIFO underrun\n", dev->name);
						break;
					}
					netif_wake_queue(dev);
					break;

					/*
					 * some unknown PCB
					 */
				default:
					printk(KERN_DEBUG "%s: unknown PCB received - %2.2x\n", dev->name, adapter->irx_pcb.command);
					break;
				}
			} else {
				printk(KERN_WARNING "%s: failed to read PCB on interrupt\n", dev->name);
				adapter_reset(dev);
			}
		}

	} while (icount++ < 5 && (inb_status(dev->base_addr) & (ACRF | DONE)));

	prime_rx(dev);

	/*
	 * indicate no longer in interrupt routine
	 */
	spin_unlock(&adapter->lock);
	return IRQ_HANDLED;
}


/******************************************************
 *
 * open the board
 *
 ******************************************************/

static int elp_open(struct net_device *dev)
{
	elp_device *adapter;
	int retval;

	adapter = dev->priv;

	if (elp_debug >= 3)
		printk(KERN_DEBUG "%s: request to open device\n", dev->name);

	/*
	 * make sure we actually found the device
	 */
	if (adapter == NULL) {
		printk(KERN_ERR "%s: Opening a non-existent physical device\n", dev->name);
		return -EAGAIN;
	}
	/*
	 * disable interrupts on the board
	 */
	outb_control(0, dev);

	/*
	 * clear any pending interrupts
	 */
	inb_command(dev->base_addr);
	adapter_reset(dev);

	/*
	 * no receive PCBs active
	 */
	adapter->rx_active = 0;

	adapter->busy = 0;
	adapter->send_pcb_semaphore = 0;
	adapter->rx_backlog.in = 0;
	adapter->rx_backlog.out = 0;

	spin_lock_init(&adapter->lock);

	/*
	 * install our interrupt service routine
	 */
	if ((retval = request_irq(dev->irq, &elp_interrupt, 0, dev->name, dev))) {
		printk(KERN_ERR "%s: could not allocate IRQ%d\n", dev->name, dev->irq);
		return retval;
	}
	if ((retval = request_dma(dev->dma, dev->name))) {
		free_irq(dev->irq, dev);
		printk(KERN_ERR "%s: could not allocate DMA%d channel\n", dev->name, dev->dma);
		return retval;
	}
	adapter->dma_buffer = (void *) dma_mem_alloc(DMA_BUFFER_SIZE);
	if (!adapter->dma_buffer) {
		printk(KERN_ERR "%s: could not allocate DMA buffer\n", dev->name);
		free_dma(dev->dma);
		free_irq(dev->irq, dev);
		return -ENOMEM;
	}
	adapter->dmaing = 0;

	/*
	 * enable interrupts on the board
	 */
	outb_control(CMDE, dev);

	/*
	 * configure adapter memory: we need 10 multicast addresses, default==0
	 */
	if (elp_debug >= 3)
		printk(KERN_DEBUG "%s: sending 3c505 memory configuration command\n", dev->name);
	adapter->tx_pcb.command = CMD_CONFIGURE_ADAPTER_MEMORY;
	adapter->tx_pcb.data.memconf.cmd_q = 10;
	adapter->tx_pcb.data.memconf.rcv_q = 20;
	adapter->tx_pcb.data.memconf.mcast = 10;
	adapter->tx_pcb.data.memconf.frame = 20;
	adapter->tx_pcb.data.memconf.rcv_b = 20;
	adapter->tx_pcb.data.memconf.progs = 0;
	adapter->tx_pcb.length = sizeof(struct Memconf);
	adapter->got[CMD_CONFIGURE_ADAPTER_MEMORY] = 0;
	if (!send_pcb(dev, &adapter->tx_pcb))
		printk(KERN_ERR "%s: couldn't send memory configuration command\n", dev->name);
	else {
		unsigned long timeout = jiffies + TIMEOUT;
		while (adapter->got[CMD_CONFIGURE_ADAPTER_MEMORY] == 0 && time_before(jiffies, timeout));
		if (time_after_eq(jiffies, timeout))
			TIMEOUT_MSG(__LINE__);
	}


	/*
	 * configure adapter to receive broadcast messages and wait for response
	 */
	if (elp_debug >= 3)
		printk(KERN_DEBUG "%s: sending 82586 configure command\n", dev->name);
	adapter->tx_pcb.command = CMD_CONFIGURE_82586;
	adapter->tx_pcb.data.configure = NO_LOOPBACK | RECV_BROAD;
	adapter->tx_pcb.length = 2;
	adapter->got[CMD_CONFIGURE_82586] = 0;
	if (!send_pcb(dev, &adapter->tx_pcb))
		printk(KERN_ERR "%s: couldn't send 82586 configure command\n", dev->name);
	else {
		unsigned long timeout = jiffies + TIMEOUT;
		while (adapter->got[CMD_CONFIGURE_82586] == 0 && time_before(jiffies, timeout));
		if (time_after_eq(jiffies, timeout))
			TIMEOUT_MSG(__LINE__);
	}

	/* enable burst-mode DMA */
	/* outb(0x1, dev->base_addr + PORT_AUXDMA); */

	/*
	 * queue receive commands to provide buffering
	 */
	prime_rx(dev);
	if (elp_debug >= 3)
		printk(KERN_DEBUG "%s: %d receive PCBs active\n", dev->name, adapter->rx_active);

	/*
	 * device is now officially open!
	 */

	netif_start_queue(dev);
	return 0;
}


/******************************************************
 *
 * send a packet to the adapter
 *
 ******************************************************/

static int send_packet(struct net_device *dev, struct sk_buff *skb)
{
	elp_device *adapter = dev->priv;
	unsigned long target;
	unsigned long flags;

	/*
	 * make sure the length is even and no shorter than 60 bytes
	 */
	unsigned int nlen = (((skb->len < 60) ? 60 : skb->len) + 1) & (~1);

	if (test_and_set_bit(0, (void *) &adapter->busy)) {
		if (elp_debug >= 2)
			printk(KERN_DEBUG "%s: transmit blocked\n", dev->name);
		return FALSE;
	}

	adapter->stats.tx_bytes += nlen;

	/*
	 * send the adapter a transmit packet command. Ignore segment and offset
	 * and make sure the length is even
	 */
	adapter->tx_pcb.command = CMD_TRANSMIT_PACKET;
	adapter->tx_pcb.length = sizeof(struct Xmit_pkt);
	adapter->tx_pcb.data.xmit_pkt.buf_ofs
	    = adapter->tx_pcb.data.xmit_pkt.buf_seg = 0;	/* Unused */
	adapter->tx_pcb.data.xmit_pkt.pkt_len = nlen;

	if (!send_pcb(dev, &adapter->tx_pcb)) {
		adapter->busy = 0;
		return FALSE;
	}
	/* if this happens, we die */
	if (test_and_set_bit(0, (void *) &adapter->dmaing))
		printk(KERN_DEBUG "%s: tx: DMA %d in progress\n", dev->name, adapter->current_dma.direction);

	adapter->current_dma.direction = 1;
	adapter->current_dma.start_time = jiffies;

	if ((unsigned long)(skb->data + nlen) >= MAX_DMA_ADDRESS || nlen != skb->len) {
		skb_copy_from_linear_data(skb, adapter->dma_buffer, nlen);
		memset(adapter->dma_buffer+skb->len, 0, nlen-skb->len);
		target = isa_virt_to_bus(adapter->dma_buffer);
	}
	else {
		target = isa_virt_to_bus(skb->data);
	}
	adapter->current_dma.skb = skb;

	flags=claim_dma_lock();
	disable_dma(dev->dma);
	clear_dma_ff(dev->dma);
	set_dma_mode(dev->dma, 0x48);	/* dma memory -> io */
	set_dma_addr(dev->dma, target);
	set_dma_count(dev->dma, nlen);
	outb_control(adapter->hcr_val | DMAE | TCEN, dev);
	enable_dma(dev->dma);
	release_dma_lock(flags);

	if (elp_debug >= 3)
		printk(KERN_DEBUG "%s: DMA transfer started\n", dev->name);

	return TRUE;
}

/*
 *	The upper layer thinks we timed out
 */

static void elp_timeout(struct net_device *dev)
{
	elp_device *adapter = dev->priv;
	int stat;

	stat = inb_status(dev->base_addr);
	printk(KERN_WARNING "%s: transmit timed out, lost %s?\n", dev->name, (stat & ACRF) ? "interrupt" : "command");
	if (elp_debug >= 1)
		printk(KERN_DEBUG "%s: status %#02x\n", dev->name, stat);
	dev->trans_start = jiffies;
	adapter->stats.tx_dropped++;
	netif_wake_queue(dev);
}

/******************************************************
 *
 * start the transmitter
 *    return 0 if sent OK, else return 1
 *
 ******************************************************/

static int elp_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	unsigned long flags;
	elp_device *adapter = dev->priv;

	spin_lock_irqsave(&adapter->lock, flags);
	check_3c505_dma(dev);

	if (elp_debug >= 3)
		printk(KERN_DEBUG "%s: request to send packet of length %d\n", dev->name, (int) skb->len);

	netif_stop_queue(dev);

	/*
	 * send the packet at skb->data for skb->len
	 */
	if (!send_packet(dev, skb)) {
		if (elp_debug >= 2) {
			printk(KERN_DEBUG "%s: failed to transmit packet\n", dev->name);
		}
		spin_unlock_irqrestore(&adapter->lock, flags);
		return 1;
	}
	if (elp_debug >= 3)
		printk(KERN_DEBUG "%s: packet of length %d sent\n", dev->name, (int) skb->len);

	/*
	 * start the transmit timeout
	 */
	dev->trans_start = jiffies;

	prime_rx(dev);
	spin_unlock_irqrestore(&adapter->lock, flags);
	netif_start_queue(dev);
	return 0;
}

/******************************************************
 *
 * return statistics on the board
 *
 ******************************************************/

static struct net_device_stats *elp_get_stats(struct net_device *dev)
{
	elp_device *adapter = (elp_device *) dev->priv;

	if (elp_debug >= 3)
		printk(KERN_DEBUG "%s: request for stats\n", dev->name);

	/* If the device is closed, just return the latest stats we have,
	   - we cannot ask from the adapter without interrupts */
	if (!netif_running(dev))
		return &adapter->stats;

	/* send a get statistics command to the board */
	adapter->tx_pcb.command = CMD_NETWORK_STATISTICS;
	adapter->tx_pcb.length = 0;
	adapter->got[CMD_NETWORK_STATISTICS] = 0;
	if (!send_pcb(dev, &adapter->tx_pcb))
		printk(KERN_ERR "%s: couldn't send get statistics command\n", dev->name);
	else {
		unsigned long timeout = jiffies + TIMEOUT;
		while (adapter->got[CMD_NETWORK_STATISTICS] == 0 && time_before(jiffies, timeout));
		if (time_after_eq(jiffies, timeout)) {
			TIMEOUT_MSG(__LINE__);
			return &adapter->stats;
		}
	}

	/* statistics are now up to date */
	return &adapter->stats;
}


static void netdev_get_drvinfo(struct net_device *dev,
			       struct ethtool_drvinfo *info)
{
	strcpy(info->driver, DRV_NAME);
	strcpy(info->version, DRV_VERSION);
	sprintf(info->bus_info, "ISA 0x%lx", dev->base_addr);
}

static u32 netdev_get_msglevel(struct net_device *dev)
{
	return debug;
}

static void netdev_set_msglevel(struct net_device *dev, u32 level)
{
	debug = level;
}

static const struct ethtool_ops netdev_ethtool_ops = {
	.get_drvinfo		= netdev_get_drvinfo,
	.get_msglevel		= netdev_get_msglevel,
	.set_msglevel		= netdev_set_msglevel,
};

/******************************************************
 *
 * close the board
 *
 ******************************************************/

static int elp_close(struct net_device *dev)
{
	elp_device *adapter;

	adapter = dev->priv;

	if (elp_debug >= 3)
		printk(KERN_DEBUG "%s: request to close device\n", dev->name);

	netif_stop_queue(dev);

	/* Someone may request the device statistic information even when
	 * the interface is closed. The following will update the statistics
	 * structure in the driver, so we'll be able to give current statistics.
	 */
	(void) elp_get_stats(dev);

	/*
	 * disable interrupts on the board
	 */
	outb_control(0, dev);

	/*
	 * release the IRQ
	 */
	free_irq(dev->irq, dev);

	free_dma(dev->dma);
	free_pages((unsigned long) adapter->dma_buffer, get_order(DMA_BUFFER_SIZE));

	return 0;
}


/************************************************************
 *
 * Set multicast list
 * num_addrs==0: clear mc_list
 * num_addrs==-1: set promiscuous mode
 * num_addrs>0: set mc_list
 *
 ************************************************************/

static void elp_set_mc_list(struct net_device *dev)
{
	elp_device *adapter = (elp_device *) dev->priv;
	struct dev_mc_list *dmi = dev->mc_list;
	int i;
	unsigned long flags;

	if (elp_debug >= 3)
		printk(KERN_DEBUG "%s: request to set multicast list\n", dev->name);

	spin_lock_irqsave(&adapter->lock, flags);

	if (!(dev->flags & (IFF_PROMISC | IFF_ALLMULTI))) {
		/* send a "load multicast list" command to the board, max 10 addrs/cmd */
		/* if num_addrs==0 the list will be cleared */
		adapter->tx_pcb.command = CMD_LOAD_MULTICAST_LIST;
		adapter->tx_pcb.length = 6 * dev->mc_count;
		for (i = 0; i < dev->mc_count; i++) {
			memcpy(adapter->tx_pcb.data.multicast[i], dmi->dmi_addr, 6);
			dmi = dmi->next;
		}
		adapter->got[CMD_LOAD_MULTICAST_LIST] = 0;
		if (!send_pcb(dev, &adapter->tx_pcb))
			printk(KERN_ERR "%s: couldn't send set_multicast command\n", dev->name);
		else {
			unsigned long timeout = jiffies + TIMEOUT;
			while (adapter->got[CMD_LOAD_MULTICAST_LIST] == 0 && time_before(jiffies, timeout));
			if (time_after_eq(jiffies, timeout)) {
				TIMEOUT_MSG(__LINE__);
			}
		}
		if (dev->mc_count)
			adapter->tx_pcb.data.configure = NO_LOOPBACK | RECV_BROAD | RECV_MULTI;
		else		/* num_addrs == 0 */
			adapter->tx_pcb.data.configure = NO_LOOPBACK | RECV_BROAD;
	} else
		adapter->tx_pcb.data.configure = NO_LOOPBACK | RECV_PROMISC;
	/*
	 * configure adapter to receive messages (as specified above)
	 * and wait for response
	 */
	if (elp_debug >= 3)
		printk(KERN_DEBUG "%s: sending 82586 configure command\n", dev->name);
	adapter->tx_pcb.command = CMD_CONFIGURE_82586;
	adapter->tx_pcb.length = 2;
	adapter->got[CMD_CONFIGURE_82586] = 0;
	if (!send_pcb(dev, &adapter->tx_pcb))
	{
		spin_unlock_irqrestore(&adapter->lock, flags);
		printk(KERN_ERR "%s: couldn't send 82586 configure command\n", dev->name);
	}
	else {
		unsigned long timeout = jiffies + TIMEOUT;
		spin_unlock_irqrestore(&adapter->lock, flags);
		while (adapter->got[CMD_CONFIGURE_82586] == 0 && time_before(jiffies, timeout));
		if (time_after_eq(jiffies, timeout))
			TIMEOUT_MSG(__LINE__);
	}
}

/************************************************************
 *
 * A couple of tests to see if there's 3C505 or not
 * Called only by elp_autodetect
 ************************************************************/

static int __init elp_sense(struct net_device *dev)
{
	int addr = dev->base_addr;
	const char *name = dev->name;
	byte orig_HSR;

	if (!request_region(addr, ELP_IO_EXTENT, "3c505"))
		return -ENODEV;

	orig_HSR = inb_status(addr);

	if (elp_debug > 0)
		printk(search_msg, name, addr);

	if (orig_HSR == 0xff) {
		if (elp_debug > 0)
			printk(notfound_msg, 1);
		goto out;
	}

	/* Wait for a while; the adapter may still be booting up */
	if (elp_debug > 0)
		printk(stilllooking_msg);

	if (orig_HSR & DIR) {
		/* If HCR.DIR is up, we pull it down. HSR.DIR should follow. */
		outb(0, dev->base_addr + PORT_CONTROL);
		msleep(300);
		if (inb_status(addr) & DIR) {
			if (elp_debug > 0)
				printk(notfound_msg, 2);
			goto out;
		}
	} else {
		/* If HCR.DIR is down, we pull it up. HSR.DIR should follow. */
		outb(DIR, dev->base_addr + PORT_CONTROL);
		msleep(300);
		if (!(inb_status(addr) & DIR)) {
			if (elp_debug > 0)
				printk(notfound_msg, 3);
			goto out;
		}
	}
	/*
	 * It certainly looks like a 3c505.
	 */
	if (elp_debug > 0)
		printk(found_msg);

	return 0;
out:
	release_region(addr, ELP_IO_EXTENT);
	return -ENODEV;
}

/*************************************************************
 *
 * Search through addr_list[] and try to find a 3C505
 * Called only by eplus_probe
 *************************************************************/

static int __init elp_autodetect(struct net_device *dev)
{
	int idx = 0;

	/* if base address set, then only check that address
	   otherwise, run through the table */
	if (dev->base_addr != 0) {	/* dev->base_addr == 0 ==> plain autodetect */
		if (elp_sense(dev) == 0)
			return dev->base_addr;
	} else
		while ((dev->base_addr = addr_list[idx++])) {
			if (elp_sense(dev) == 0)
				return dev->base_addr;
		}

	/* could not find an adapter */
	if (elp_debug > 0)
		printk(couldnot_msg, dev->name);

	return 0;		/* Because of this, the layer above will return -ENODEV */
}


/******************************************************
 *
 * probe for an Etherlink Plus board at the specified address
 *
 ******************************************************/

/* There are three situations we need to be able to detect here:

 *  a) the card is idle
 *  b) the card is still booting up
 *  c) the card is stuck in a strange state (some DOS drivers do this)
 *
 * In case (a), all is well.  In case (b), we wait 10 seconds to see if the
 * card finishes booting, and carry on if so.  In case (c), we do a hard reset,
 * loop round, and hope for the best.
 *
 * This is all very unpleasant, but hopefully avoids the problems with the old
 * probe code (which had a 15-second delay if the card was idle, and didn't
 * work at all if it was in a weird state).
 */

static int __init elplus_setup(struct net_device *dev)
{
	elp_device *adapter = dev->priv;
	int i, tries, tries1, okay;
	unsigned long timeout;
	unsigned long cookie = 0;
	int err = -ENODEV;

	SET_MODULE_OWNER(dev);

	/*
	 *  setup adapter structure
	 */

	dev->base_addr = elp_autodetect(dev);
	if (!dev->base_addr)
		return -ENODEV;

	adapter->send_pcb_semaphore = 0;

	for (tries1 = 0; tries1 < 3; tries1++) {
		outb_control((adapter->hcr_val | CMDE) & ~DIR, dev);
		/* First try to write just one byte, to see if the card is
		 * responding at all normally.
		 */
		timeout = jiffies + 5*HZ/100;
		okay = 0;
		while (time_before(jiffies, timeout) && !(inb_status(dev->base_addr) & HCRE));
		if ((inb_status(dev->base_addr) & HCRE)) {
			outb_command(0, dev->base_addr);	/* send a spurious byte */
			timeout = jiffies + 5*HZ/100;
			while (time_before(jiffies, timeout) && !(inb_status(dev->base_addr) & HCRE));
			if (inb_status(dev->base_addr) & HCRE)
				okay = 1;
		}
		if (!okay) {
			/* Nope, it's ignoring the command register.  This means that
			 * either it's still booting up, or it's died.
			 */
			printk(KERN_ERR "%s: command register wouldn't drain, ", dev->name);
			if ((inb_status(dev->base_addr) & 7) == 3) {
				/* If the adapter status is 3, it *could* still be booting.
				 * Give it the benefit of the doubt for 10 seconds.
				 */
				printk("assuming 3c505 still starting\n");
				timeout = jiffies + 10*HZ;
				while (time_before(jiffies, timeout) && (inb_status(dev->base_addr) & 7));
				if (inb_status(dev->base_addr) & 7) {
					printk(KERN_ERR "%s: 3c505 failed to start\n", dev->name);
				} else {
					okay = 1;  /* It started */
				}
			} else {
				/* Otherwise, it must just be in a strange
				 * state.  We probably need to kick it.
				 */
				printk("3c505 is sulking\n");
			}
		}
		for (tries = 0; tries < 5 && okay; tries++) {

			/*
			 * Try to set the Ethernet address, to make sure that the board
			 * is working.
			 */
			adapter->tx_pcb.command = CMD_STATION_ADDRESS;
			adapter->tx_pcb.length = 0;
			cookie = probe_irq_on();
			if (!send_pcb(dev, &adapter->tx_pcb)) {
				printk(KERN_ERR "%s: could not send first PCB\n", dev->name);
				probe_irq_off(cookie);
				continue;
			}
			if (!receive_pcb(dev, &adapter->rx_pcb)) {
				printk(KERN_ERR "%s: could not read first PCB\n", dev->name);
				probe_irq_off(cookie);
				continue;
			}
			if ((adapter->rx_pcb.command != CMD_ADDRESS_RESPONSE) ||
			    (adapter->rx_pcb.length != 6)) {
				printk(KERN_ERR "%s: first PCB wrong (%d, %d)\n", dev->name, adapter->rx_pcb.command, adapter->rx_pcb.length);
				probe_irq_off(cookie);
				continue;
			}
			goto okay;
		}
		/* It's broken.  Do a hard reset to re-initialise the board,
		 * and try again.
		 */
		printk(KERN_INFO "%s: resetting adapter\n", dev->name);
		outb_control(adapter->hcr_val | FLSH | ATTN, dev);
		outb_control(adapter->hcr_val & ~(FLSH | ATTN), dev);
	}
	printk(KERN_ERR "%s: failed to initialise 3c505\n", dev->name);
	goto out;

      okay:
	if (dev->irq) {		/* Is there a preset IRQ? */
		int rpt = probe_irq_off(cookie);
		if (dev->irq != rpt) {
			printk(KERN_WARNING "%s: warning, irq %d configured but %d detected\n", dev->name, dev->irq, rpt);
		}
		/* if dev->irq == probe_irq_off(cookie), all is well */
	} else		       /* No preset IRQ; just use what we can detect */
		dev->irq = probe_irq_off(cookie);
	switch (dev->irq) {    /* Legal, sane? */
	case 0:
		printk(KERN_ERR "%s: IRQ probe failed: check 3c505 jumpers.\n",
		       dev->name);
		goto out;
	case 1:
	case 6:
	case 8:
	case 13:
		printk(KERN_ERR "%s: Impossible IRQ %d reported by probe_irq_off().\n",
		       dev->name, dev->irq);
		       goto out;
	}
	/*
	 *  Now we have the IRQ number so we can disable the interrupts from
	 *  the board until the board is opened.
	 */
	outb_control(adapter->hcr_val & ~CMDE, dev);

	/*
	 * copy Ethernet address into structure
	 */
	for (i = 0; i < 6; i++)
		dev->dev_addr[i] = adapter->rx_pcb.data.eth_addr[i];

	/* find a DMA channel */
	if (!dev->dma) {
		if (dev->mem_start) {
			dev->dma = dev->mem_start & 7;
		}
		else {
			printk(KERN_WARNING "%s: warning, DMA channel not specified, using default\n", dev->name);
			dev->dma = ELP_DMA;
		}
	}

	/*
	 * print remainder of startup message
	 */
	printk(KERN_INFO "%s: 3c505 at %#lx, irq %d, dma %d, ",
	       dev->name, dev->base_addr, dev->irq, dev->dma);
	printk("addr %02x:%02x:%02x:%02x:%02x:%02x, ",
	       dev->dev_addr[0], dev->dev_addr[1], dev->dev_addr[2],
	       dev->dev_addr[3], dev->dev_addr[4], dev->dev_addr[5]);

	/*
	 * read more information from the adapter
	 */

	adapter->tx_pcb.command = CMD_ADAPTER_INFO;
	adapter->tx_pcb.length = 0;
	if (!send_pcb(dev, &adapter->tx_pcb) ||
	    !receive_pcb(dev, &adapter->rx_pcb) ||
	    (adapter->rx_pcb.command != CMD_ADAPTER_INFO_RESPONSE) ||
	    (adapter->rx_pcb.length != 10)) {
		printk("not responding to second PCB\n");
	}
	printk("rev %d.%d, %dk\n", adapter->rx_pcb.data.info.major_vers, adapter->rx_pcb.data.info.minor_vers, adapter->rx_pcb.data.info.RAM_sz);

	/*
	 * reconfigure the adapter memory to better suit our purposes
	 */
	adapter->tx_pcb.command = CMD_CONFIGURE_ADAPTER_MEMORY;
	adapter->tx_pcb.length = 12;
	adapter->tx_pcb.data.memconf.cmd_q = 8;
	adapter->tx_pcb.data.memconf.rcv_q = 8;
	adapter->tx_pcb.data.memconf.mcast = 10;
	adapter->tx_pcb.data.memconf.frame = 10;
	adapter->tx_pcb.data.memconf.rcv_b = 10;
	adapter->tx_pcb.data.memconf.progs = 0;
	if (!send_pcb(dev, &adapter->tx_pcb) ||
	    !receive_pcb(dev, &adapter->rx_pcb) ||
	    (adapter->rx_pcb.command != CMD_CONFIGURE_ADAPTER_RESPONSE) ||
	    (adapter->rx_pcb.length != 2)) {
		printk(KERN_ERR "%s: could not configure adapter memory\n", dev->name);
	}
	if (adapter->rx_pcb.data.configure) {
		printk(KERN_ERR "%s: adapter configuration failed\n", dev->name);
	}

	dev->open = elp_open;				/* local */
	dev->stop = elp_close;				/* local */
	dev->get_stats = elp_get_stats;			/* local */
	dev->hard_start_xmit = elp_start_xmit;		/* local */
	dev->tx_timeout = elp_timeout;			/* local */
	dev->watchdog_timeo = 10*HZ;
	dev->set_multicast_list = elp_set_mc_list;	/* local */
	dev->ethtool_ops = &netdev_ethtool_ops;		/* local */

	memset(&(adapter->stats), 0, sizeof(struct net_device_stats));
	dev->mem_start = dev->mem_end = 0;

	err = register_netdev(dev);
	if (err)
		goto out;

	return 0;
out:
	release_region(dev->base_addr, ELP_IO_EXTENT);
	return err;
}

#ifndef MODULE
struct net_device * __init elplus_probe(int unit)
{
	struct net_device *dev = alloc_etherdev(sizeof(elp_device));
	int err;
	if (!dev)
		return ERR_PTR(-ENOMEM);

	sprintf(dev->name, "eth%d", unit);
	netdev_boot_setup_check(dev);

	err = elplus_setup(dev);
	if (err) {
		free_netdev(dev);
		return ERR_PTR(err);
	}
	return dev;
}

#else
static struct net_device *dev_3c505[ELP_MAX_CARDS];
static int io[ELP_MAX_CARDS];
static int irq[ELP_MAX_CARDS];
static int dma[ELP_MAX_CARDS];
module_param_array(io, int, NULL, 0);
module_param_array(irq, int, NULL, 0);
module_param_array(dma, int, NULL, 0);
MODULE_PARM_DESC(io, "EtherLink Plus I/O base address(es)");
MODULE_PARM_DESC(irq, "EtherLink Plus IRQ number(s) (assigned)");
MODULE_PARM_DESC(dma, "EtherLink Plus DMA channel(s)");

int __init init_module(void)
{
	int this_dev, found = 0;

	for (this_dev = 0; this_dev < ELP_MAX_CARDS; this_dev++) {
		struct net_device *dev = alloc_etherdev(sizeof(elp_device));
		if (!dev)
			break;

		dev->irq = irq[this_dev];
		dev->base_addr = io[this_dev];
		if (dma[this_dev]) {
			dev->dma = dma[this_dev];
		} else {
			dev->dma = ELP_DMA;
			printk(KERN_WARNING "3c505.c: warning, using default DMA channel,\n");
		}
		if (io[this_dev] == 0) {
			if (this_dev) {
				free_netdev(dev);
				break;
			}
			printk(KERN_NOTICE "3c505.c: module autoprobe not recommended, give io=xx.\n");
		}
		if (elplus_setup(dev) != 0) {
			printk(KERN_WARNING "3c505.c: Failed to register card at 0x%x.\n", io[this_dev]);
			free_netdev(dev);
			break;
		}
		dev_3c505[this_dev] = dev;
		found++;
	}
	if (!found)
		return -ENODEV;
	return 0;
}

void __exit cleanup_module(void)
{
	int this_dev;

	for (this_dev = 0; this_dev < ELP_MAX_CARDS; this_dev++) {
		struct net_device *dev = dev_3c505[this_dev];
		if (dev) {
			unregister_netdev(dev);
			release_region(dev->base_addr, ELP_IO_EXTENT);
			free_netdev(dev);
		}
	}
}

#endif				/* MODULE */
MODULE_LICENSE("GPL");
