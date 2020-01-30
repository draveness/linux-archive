/*     
 *    Lance ethernet driver for the MIPS processor based
 *      DECstation family
 *
 *
 *      adopted from sunlance.c by Richard van den Berg
 *
 *      additional sources:
 *      - PMAD-AA TURBOchannel Ethernet Module Functional Specification,
 *        Revision 1.2
 *
 *      History:
 *
 *      v0.001: The kernel accepts the code and it shows the hardware address.
 *
 *      v0.002: Removed most sparc stuff, left only some module and dma stuff.
 *
 *      v0.003: Enhanced base address calculation from proposals by
 *      Harald Koerfgen and Thomas Riemer.
 *
 *      v0.004: lance-regs is pointing at the right addresses, added prom
 *      check. First start of address mapping and DMA.
 *
 *      v0.005: started to play around with LANCE-DMA. This driver will not work
 *      for non IOASIC lances. HK
 *
 *      v0.006: added pointer arrays to lance_private and setup routine for them
 *      in dec_lance_init. HK
 *
 *      v0.007: Big shit. The LANCE seems to use a different DMA mechanism to access
 *      the init block. This looks like one (short) word at a time, but the smallest
 *      amount the IOASIC can transfer is a (long) word. So we have a 2-2 padding here.
 *      Changed lance_init_block accordingly. The 16-16 padding for the buffers
 *      seems to be correct. HK
 *
 *     v0.008 - mods to make PMAX_LANCE work. 01/09/1999 triemer
 */

#undef DEBUG_DRIVER

static char *version =
"declance.c: v0.008 by Linux Mips DECstation task force\n";

static char *lancestr = "LANCE";

/*
 * card types
 */
#define ASIC_LANCE 1
#define PMAD_LANCE 2
#define PMAX_LANCE 3

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>

#include <asm/dec/interrupts.h>
#include <asm/dec/ioasic_ints.h>
#include <asm/dec/ioasic_addrs.h>
#include <asm/dec/machtype.h>
#include <asm/dec/tc.h>
#include <asm/dec/kn01.h>
#include <asm/wbflush.h>
#include <asm/addrspace.h>

#include <linux/config.h>
#include <linux/errno.h>
#include <linux/hdreg.h>
#include <linux/ioport.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/unistd.h>
#include <linux/ptrace.h>
#include <linux/malloc.h>
#include <linux/user.h>
#include <linux/utsname.h>
#include <linux/a.out.h>
#include <linux/tty.h>
#include <linux/delay.h>
#include <asm/io.h>
#include <linux/etherdevice.h>

#ifndef CONFIG_TC
unsigned long system_base = 0;
unsigned long dmaptr;
#endif
static int type;

#define CRC_POLYNOMIAL_BE 0x04c11db7UL	/* Ethernet CRC, big endian */
#define CRC_POLYNOMIAL_LE 0xedb88320UL	/* Ethernet CRC, little endian */

#define LE_CSR0 0
#define LE_CSR1 1
#define LE_CSR2 2
#define LE_CSR3 3

#define LE_MO_PROM      0x8000	/* Enable promiscuous mode */

#define	LE_C0_ERR	0x8000	/* Error: set if BAB, SQE, MISS or ME is set */
#define	LE_C0_BABL	0x4000	/* BAB:  Babble: tx timeout. */
#define	LE_C0_CERR	0x2000	/* SQE:  Signal quality error */
#define	LE_C0_MISS	0x1000	/* MISS: Missed a packet */
#define	LE_C0_MERR	0x0800	/* ME:   Memory error */
#define	LE_C0_RINT	0x0400	/* Received interrupt */
#define	LE_C0_TINT	0x0200	/* Transmitter Interrupt */
#define	LE_C0_IDON	0x0100	/* IFIN: Init finished. */
#define	LE_C0_INTR	0x0080	/* Interrupt or error */
#define	LE_C0_INEA	0x0040	/* Interrupt enable */
#define	LE_C0_RXON	0x0020	/* Receiver on */
#define	LE_C0_TXON	0x0010	/* Transmitter on */
#define	LE_C0_TDMD	0x0008	/* Transmitter demand */
#define	LE_C0_STOP	0x0004	/* Stop the card */
#define	LE_C0_STRT	0x0002	/* Start the card */
#define	LE_C0_INIT	0x0001	/* Init the card */

#define	LE_C3_BSWP	0x4	/* SWAP */
#define	LE_C3_ACON	0x2	/* ALE Control */
#define	LE_C3_BCON	0x1	/* Byte control */

/* Receive message descriptor 1 */
#define LE_R1_OWN       0x80	/* Who owns the entry */
#define LE_R1_ERR       0x40	/* Error: if FRA, OFL, CRC or BUF is set */
#define LE_R1_FRA       0x20	/* FRA: Frame error */
#define LE_R1_OFL       0x10	/* OFL: Frame overflow */
#define LE_R1_CRC       0x08	/* CRC error */
#define LE_R1_BUF       0x04	/* BUF: Buffer error */
#define LE_R1_SOP       0x02	/* Start of packet */
#define LE_R1_EOP       0x01	/* End of packet */
#define LE_R1_POK       0x03	/* Packet is complete: SOP + EOP */

#define LE_T1_OWN       0x80	/* Lance owns the packet */
#define LE_T1_ERR       0x40	/* Error summary */
#define LE_T1_EMORE     0x10	/* Error: more than one retry needed */
#define LE_T1_EONE      0x08	/* Error: one retry needed */
#define LE_T1_EDEF      0x04	/* Error: deferred */
#define LE_T1_SOP       0x02	/* Start of packet */
#define LE_T1_EOP       0x01	/* End of packet */
#define LE_T1_POK	0x03	/* Packet is complete: SOP + EOP */

#define LE_T3_BUF       0x8000	/* Buffer error */
#define LE_T3_UFL       0x4000	/* Error underflow */
#define LE_T3_LCOL      0x1000	/* Error late collision */
#define LE_T3_CLOS      0x0800	/* Error carrier loss */
#define LE_T3_RTY       0x0400	/* Error retry */
#define LE_T3_TDR       0x03ff	/* Time Domain Reflectometry counter */

/* Define: 2^4 Tx buffers and 2^4 Rx buffers */

#ifndef LANCE_LOG_TX_BUFFERS
#define LANCE_LOG_TX_BUFFERS 4
#define LANCE_LOG_RX_BUFFERS 4
#endif

#define TX_RING_SIZE			(1 << (LANCE_LOG_TX_BUFFERS))
#define TX_RING_MOD_MASK		(TX_RING_SIZE - 1)

#define RX_RING_SIZE			(1 << (LANCE_LOG_RX_BUFFERS))
#define RX_RING_MOD_MASK		(RX_RING_SIZE - 1)

#define PKT_BUF_SZ		1536
#define RX_BUFF_SIZE            PKT_BUF_SZ
#define TX_BUFF_SIZE            PKT_BUF_SZ

#undef TEST_HITS
#define DEBUG_DRIVER 1

#define ZERO 0

/* The DS2000/3000 have a linear 64 KB buffer.

 * The PMAD-AA has 128 kb buffer on-board. 
 *
 * The IOASIC LANCE devices use a shared memory region. This region as seen 
 * from the CPU is (max) 128 KB long and has to be on an 128 KB boundary.
 * The LANCE sees this as a 64 KB long continuous memory region.
 *
 * The LANCE's DMA address is used as an index in this buffer and DMA takes
 * place in bursts of eight 16-Bit words which are packed into four 32-Bit words
 * by the IOASIC. This leads to a strange padding: 16 bytes of valid data followed
 * by a 16 byte gap :-(.
 */

struct lance_rx_desc {
	unsigned short rmd0;	/* low address of packet */
	short gap0;
	unsigned char rmd1_hadr;	/* high address of packet */
	unsigned char rmd1_bits;	/* descriptor bits */
	short gap1;
	short length;		/* This length is 2s complement (negative)!
				   * Buffer length
				 */
	short gap2;
	unsigned short mblength;	/* This is the actual number of bytes received */
	short gap3;
};

struct lance_tx_desc {
	unsigned short tmd0;	/* low address of packet */
	short gap0;
	unsigned char tmd1_hadr;	/* high address of packet */
	unsigned char tmd1_bits;	/* descriptor bits */
	short gap1;
	short length;		/* Length is 2s complement (negative)! */
	short gap2;
	unsigned short misc;
	short gap3;
};


/* First part of the LANCE initialization block, described in databook. */
struct lance_init_block {
	unsigned short mode;	/* Pre-set mode (reg. 15) */
	short gap0;

	unsigned char phys_addr[12];	/* Physical ethernet address
					   * only 0, 1, 4, 5, 8, 9 are valid
					   * 2, 3, 6, 7, 10, 11 are gaps
					 */
	unsigned short filter[8];	/* Multicast filter.
					   * only 0, 2, 4, 6 are valid
					   * 1, 3, 5, 7 are gaps
					 */

	/* Receive and transmit ring base, along with extra bits. */
	unsigned short rx_ptr;	/* receive descriptor addr */
	short gap1;
	unsigned short rx_len;	/* receive len and high addr */
	short gap2;
	unsigned short tx_ptr;	/* transmit descriptor addr */
	short gap3;
	unsigned short tx_len;	/* transmit len and high addr */
	short gap4;
	char gap5[16];

	/* The buffer descriptors */
	struct lance_rx_desc brx_ring[RX_RING_SIZE];
	struct lance_tx_desc btx_ring[TX_RING_SIZE];
};

#define BUF_OFFSET_CPU sizeof(struct lance_init_block)
#define BUF_OFFSET_LNC (sizeof(struct lance_init_block)>>1)

#define libdesc_offset(rt, elem) \
((__u32)(((unsigned long)(&(((struct lance_init_block *)0)->rt[elem])))))

/*
 * This works *only* for the ring descriptors
 */
#define LANCE_ADDR(x) (PHYSADDR(x) >> 1)

struct lance_private {
	char *name;
	volatile struct lance_regs *ll;
	volatile struct lance_init_block *init_block;
	volatile unsigned long *dma_ptr_reg;

	spinlock_t	lock;

	int rx_new, tx_new;
	int rx_old, tx_old;

	struct net_device_stats stats;

	unsigned short busmaster_regval;

	struct net_device *dev;	/* Backpointer        */
	struct lance_private *next_module;
	struct timer_list       multicast_timer;

	/* Pointers to the ring buffers as seen from the CPU */
	char *rx_buf_ptr_cpu[RX_RING_SIZE];
	char *tx_buf_ptr_cpu[TX_RING_SIZE];

	/* Pointers to the ring buffers as seen from the LANCE */
	char *rx_buf_ptr_lnc[RX_RING_SIZE];
	char *tx_buf_ptr_lnc[TX_RING_SIZE];
};

#define TX_BUFFS_AVAIL ((lp->tx_old<=lp->tx_new)?\
			lp->tx_old+TX_RING_MOD_MASK-lp->tx_new:\
			lp->tx_old - lp->tx_new-1)

/* The lance control ports are at an absolute address, machine and tc-slot
 * dependant.
 * DECstations do only 32-bit access and the LANCE uses 16 bit addresses,
 * so we have to give the structure an extra member making rap pointing
 * at the right address
 */
struct lance_regs {
	volatile unsigned short rdp;	/* register data port */
	unsigned short pad;
	volatile unsigned short rap;	/* register address port */
};

int dec_lance_debug = 2;

/*
   #ifdef MODULE
   static struct lance_private *root_lance_dev = NULL;
   #endif
 */

static inline void writereg(volatile unsigned short *regptr, short value)
{
	*regptr = value;
	wbflush();
}

/* Load the CSR registers */
static void load_csrs(struct lance_private *lp)
{
	volatile struct lance_regs *ll = lp->ll;
	int leptr;

	/* The address space as seen from the LANCE
	 * begins at address 0. HK
	 */
	leptr = 0;

	writereg(&ll->rap, LE_CSR1);
	writereg(&ll->rdp, (leptr & 0xFFFF));
	writereg(&ll->rap, LE_CSR2);
	writereg(&ll->rdp, leptr >> 16);
	writereg(&ll->rap, LE_CSR3);
	writereg(&ll->rdp, lp->busmaster_regval);

	/* Point back to csr0 */
	writereg(&ll->rap, LE_CSR0);
}

/*
 * Our specialized copy routines
 *
 */
void cp_to_buf(void *to, const void *from, __kernel_size_t len)
{
	unsigned short *tp, *fp, clen;
	unsigned char *rtp, *rfp;

	if (type == PMAX_LANCE) {
		clen = len >> 1;
		tp = (unsigned short *) to;
		fp = (unsigned short *) from;

		while (clen--) {
			*tp++ = *fp++;
			tp++;
		}

		clen = len & 1;
		rtp = (unsigned char *) tp;
		rfp = (unsigned char *) fp;
		while (clen--) {
			*rtp++ = *rfp++;
		}
	} else {
		/*
		 * copy 16 Byte chunks
		 */
		clen = len >> 4;
		tp = (unsigned short *) to;
		fp = (unsigned short *) from;
		while (clen--) {
			*tp++ = *fp++;
			*tp++ = *fp++;
			*tp++ = *fp++;
			*tp++ = *fp++;
			*tp++ = *fp++;
			*tp++ = *fp++;
			*tp++ = *fp++;
			*tp++ = *fp++;
			tp += 8;
		}

		/*
		 * do the rest, if any.
		 */
		clen = len & 15;
		rtp = (unsigned char *) tp;
		rfp = (unsigned char *) fp;
		while (clen--) {
			*rtp++ = *rfp++;
		}
	}

	wbflush();
}

void cp_from_buf(void *to, unsigned char *from, int len)
{
	unsigned short *tp, *fp, clen;
	unsigned char *rtp, *rfp;

	if (type == PMAX_LANCE) {
		clen = len >> 1;
		tp = (unsigned short *) to;
		fp = (unsigned short *) from;
		while (clen--) {
			*tp++ = *fp++;
			fp++;
		}

		clen = len & 1;

		rtp = (unsigned char *) tp;
		rfp = (unsigned char *) fp;

		while (clen--) {
			*rtp++ = *rfp++;
		}
	} else {

		/*
		 * copy 16 Byte chunks
		 */
		clen = len >> 4;
		tp = (unsigned short *) to;
		fp = (unsigned short *) from;
		while (clen--) {
			*tp++ = *fp++;
			*tp++ = *fp++;
			*tp++ = *fp++;
			*tp++ = *fp++;
			*tp++ = *fp++;
			*tp++ = *fp++;
			*tp++ = *fp++;
			*tp++ = *fp++;
			fp += 8;
		}

		/*
		 * do the rest, if any.
		 */
		clen = len & 15;
		rtp = (unsigned char *) tp;
		rfp = (unsigned char *) fp;
		while (clen--) {
			*rtp++ = *rfp++;
		}


	}

}

/* Setup the Lance Rx and Tx rings */
static void lance_init_ring(struct net_device *dev)
{
	struct lance_private *lp = (struct lance_private *) dev->priv;
	volatile struct lance_init_block *ib;
	int leptr;
	int i;

	ib = (struct lance_init_block *) (dev->mem_start);

	/* Lock out other processes while setting up hardware */
	netif_stop_queue(dev);
	lp->rx_new = lp->tx_new = 0;
	lp->rx_old = lp->tx_old = 0;

	/* Copy the ethernet address to the lance init block.
	 * XXX bit 0 of the physical address registers has to be zero
	 */
	ib->phys_addr[0] = dev->dev_addr[0];
	ib->phys_addr[1] = dev->dev_addr[1];
	ib->phys_addr[4] = dev->dev_addr[2];
	ib->phys_addr[5] = dev->dev_addr[3];
	ib->phys_addr[8] = dev->dev_addr[4];
	ib->phys_addr[9] = dev->dev_addr[5];
	/* Setup the initialization block */

	/* Setup rx descriptor pointer */
	leptr = LANCE_ADDR(libdesc_offset(brx_ring, 0));
	ib->rx_len = (LANCE_LOG_RX_BUFFERS << 13) | (leptr >> 16);
	ib->rx_ptr = leptr;
	if (ZERO)
		printk("RX ptr: %8.8x(%8.8x)\n", leptr, libdesc_offset(brx_ring, 0));

	/* Setup tx descriptor pointer */
	leptr = LANCE_ADDR(libdesc_offset(btx_ring, 0));
	ib->tx_len = (LANCE_LOG_TX_BUFFERS << 13) | (leptr >> 16);
	ib->tx_ptr = leptr;
	if (ZERO)
		printk("TX ptr: %8.8x(%8.8x)\n", leptr, libdesc_offset(btx_ring, 0));

	if (ZERO)
		printk("TX rings:\n");

	/* Setup the Tx ring entries */
	for (i = 0; i < TX_RING_SIZE; i++) {
		leptr = (int) lp->tx_buf_ptr_lnc[i];
		ib->btx_ring[i].tmd0 = leptr;
		ib->btx_ring[i].tmd1_hadr = leptr >> 16;
		ib->btx_ring[i].tmd1_bits = 0;
		ib->btx_ring[i].length = 0xf000;	/* The ones required by tmd2 */
		ib->btx_ring[i].misc = 0;
		if (i < 3 && ZERO)
			printk("%d: 0x%8.8x(0x%8.8x)\n", i, leptr, (int) lp->tx_buf_ptr_cpu[i]);
	}

	/* Setup the Rx ring entries */
	if (ZERO)
		printk("RX rings:\n");
	for (i = 0; i < RX_RING_SIZE; i++) {
		leptr = (int) lp->rx_buf_ptr_lnc[i];
		ib->brx_ring[i].rmd0 = leptr;
		ib->brx_ring[i].rmd1_hadr = leptr >> 16;
		ib->brx_ring[i].rmd1_bits = LE_R1_OWN;
		ib->brx_ring[i].length = -RX_BUFF_SIZE | 0xf000;
		ib->brx_ring[i].mblength = 0;
		if (i < 3 && ZERO)
			printk("%d: 0x%8.8x(0x%8.8x)\n", i, leptr, (int) lp->rx_buf_ptr_cpu[i]);
	}
	wbflush();
}

static int init_restart_lance(struct lance_private *lp)
{
	volatile struct lance_regs *ll = lp->ll;
	int i;

	writereg(&ll->rap, LE_CSR0);
	writereg(&ll->rdp, LE_C0_INIT);

	/* Wait for the lance to complete initialization */
	for (i = 0; (i < 100) && !(ll->rdp & LE_C0_IDON); i++) {
		udelay(10);
	}
	if ((i == 100) || (ll->rdp & LE_C0_ERR)) {
		printk("LANCE unopened after %d ticks, csr0=%4.4x.\n", i, ll->rdp);
		return -1;
	}
	if ((ll->rdp & LE_C0_ERR)) {
		printk("LANCE unopened after %d ticks, csr0=%4.4x.\n", i, ll->rdp);
		return -1;
	}
	writereg(&ll->rdp, LE_C0_IDON);
	writereg(&ll->rdp, LE_C0_STRT);
	writereg(&ll->rdp, LE_C0_INEA);

	return 0;
}

static int lance_rx(struct net_device *dev)
{
	struct lance_private *lp = (struct lance_private *) dev->priv;
	volatile struct lance_init_block *ib;
	volatile struct lance_rx_desc *rd = 0;
	unsigned char bits;
	int len = 0;
	struct sk_buff *skb = 0;
	ib = (struct lance_init_block *) (dev->mem_start);

#ifdef TEST_HITS
	int i;

	printk("[");
	for (i = 0; i < RX_RING_SIZE; i++) {
		if (i == lp->rx_new)
			printk("%s",
			       ib->brx_ring[i].rmd1_bits & LE_R1_OWN ? "_" : "X");
		else
			printk("%s",
			       ib->brx_ring[i].rmd1_bits & LE_R1_OWN ? "." : "1");
	}
	printk("]");
#endif

	for (rd = &ib->brx_ring[lp->rx_new];
	     !((bits = rd->rmd1_bits) & LE_R1_OWN);
	     rd = &ib->brx_ring[lp->rx_new]) {

		/* We got an incomplete frame? */
		if ((bits & LE_R1_POK) != LE_R1_POK) {
			lp->stats.rx_over_errors++;
			lp->stats.rx_errors++;
		} else if (bits & LE_R1_ERR) {
			/* Count only the end frame as a rx error,
			 * not the beginning
			 */
			if (bits & LE_R1_BUF)
				lp->stats.rx_fifo_errors++;
			if (bits & LE_R1_CRC)
				lp->stats.rx_crc_errors++;
			if (bits & LE_R1_OFL)
				lp->stats.rx_over_errors++;
			if (bits & LE_R1_FRA)
				lp->stats.rx_frame_errors++;
			if (bits & LE_R1_EOP)
				lp->stats.rx_errors++;
		} else {
			len = (rd->mblength & 0xfff) - 4;
			skb = dev_alloc_skb(len + 2);

			if (skb == 0) {
				printk("%s: Memory squeeze, deferring packet.\n",
				       dev->name);
				lp->stats.rx_dropped++;
				rd->mblength = 0;
				rd->rmd1_bits = LE_R1_OWN;
				lp->rx_new = (lp->rx_new + 1) & RX_RING_MOD_MASK;
				return 0;
			}
			lp->stats.rx_bytes += len;

			skb->dev = dev;
			skb_reserve(skb, 2);	/* 16 byte align */
			skb_put(skb, len);	/* make room */
			cp_from_buf(skb->data,
				 (char *) lp->rx_buf_ptr_cpu[lp->rx_new],
				    len);
			skb->protocol = eth_type_trans(skb, dev);
			netif_rx(skb);
			lp->stats.rx_packets++;
		}

		/* Return the packet to the pool */
		rd->mblength = 0;
		rd->length = -RX_BUFF_SIZE | 0xf000;
		rd->rmd1_bits = LE_R1_OWN;
		lp->rx_new = (lp->rx_new + 1) & RX_RING_MOD_MASK;
	}
	return 0;
}

static void lance_tx(struct net_device *dev)
{
	struct lance_private *lp = (struct lance_private *) dev->priv;
	volatile struct lance_init_block *ib;
	volatile struct lance_regs *ll = lp->ll;
	volatile struct lance_tx_desc *td;
	int i, j;
	int status;
	ib = (struct lance_init_block *) (dev->mem_start);
	j = lp->tx_old;

	spin_lock(&lp->lock);

	for (i = j; i != lp->tx_new; i = j) {
		td = &ib->btx_ring[i];
		/* If we hit a packet not owned by us, stop */
		if (td->tmd1_bits & LE_T1_OWN)
			break;

		if (td->tmd1_bits & LE_T1_ERR) {
			status = td->misc;

			lp->stats.tx_errors++;
			if (status & LE_T3_RTY)
				lp->stats.tx_aborted_errors++;
			if (status & LE_T3_LCOL)
				lp->stats.tx_window_errors++;

			if (status & LE_T3_CLOS) {
				lp->stats.tx_carrier_errors++;
				printk("%s: Carrier Lost", dev->name);
				/* Stop the lance */
				writereg(&ll->rap, LE_CSR0);
				writereg(&ll->rdp, LE_C0_STOP);
				lance_init_ring(dev);
				load_csrs(lp);
				init_restart_lance(lp);
				goto out;
			}
			/* Buffer errors and underflows turn off the
			 * transmitter, restart the adapter.
			 */
			if (status & (LE_T3_BUF | LE_T3_UFL)) {
				lp->stats.tx_fifo_errors++;

				printk("%s: Tx: ERR_BUF|ERR_UFL, restarting\n",
				       dev->name);
				/* Stop the lance */
				writereg(&ll->rap, LE_CSR0);
				writereg(&ll->rdp, LE_C0_STOP);
				lance_init_ring(dev);
				load_csrs(lp);
				init_restart_lance(lp);
				goto out;
			}
		} else if ((td->tmd1_bits & LE_T1_POK) == LE_T1_POK) {
			/*
			 * So we don't count the packet more than once.
			 */
			td->tmd1_bits &= ~(LE_T1_POK);

			/* One collision before packet was sent. */
			if (td->tmd1_bits & LE_T1_EONE)
				lp->stats.collisions++;

			/* More than one collision, be optimistic. */
			if (td->tmd1_bits & LE_T1_EMORE)
				lp->stats.collisions += 2;

			lp->stats.tx_packets++;
		}
		j = (j + 1) & TX_RING_MOD_MASK;
	}
	lp->tx_old = j;
out:
	if (netif_queue_stopped(dev) &&
	    TX_BUFFS_AVAIL > 0)
		netif_wake_queue(dev);

	spin_unlock(&lp->lock);
}

static void lance_interrupt(const int irq, void *dev_id, struct pt_regs *regs)
{
	struct net_device *dev = (struct net_device *) dev_id;
	struct lance_private *lp = (struct lance_private *) dev->priv;
	volatile struct lance_regs *ll = lp->ll;
	int csr0;

	writereg(&ll->rap, LE_CSR0);
	csr0 = ll->rdp;

	/* Acknowledge all the interrupt sources ASAP */
	writereg(&ll->rdp, csr0 & (LE_C0_INTR | LE_C0_TINT | LE_C0_RINT));

	if ((csr0 & LE_C0_ERR)) {
		/* Clear the error condition */
		writereg(&ll->rdp, LE_C0_BABL | LE_C0_ERR | LE_C0_MISS |
			 LE_C0_CERR | LE_C0_MERR);
	}
	if (csr0 & LE_C0_RINT)
		lance_rx(dev);

	if (csr0 & LE_C0_TINT)
		lance_tx(dev);

	if (csr0 & LE_C0_BABL)
		lp->stats.tx_errors++;

	if (csr0 & LE_C0_MISS)
		lp->stats.rx_errors++;

	if (csr0 & LE_C0_MERR) {
		volatile unsigned long int_stat = *(unsigned long *) (system_base + IOCTL + SIR);

		printk("%s: Memory error, status %04x", dev->name, csr0);

		if (int_stat & LANCE_DMA_MEMRDERR) {
			printk("%s: DMA error\n", dev->name);
			int_stat |= LANCE_DMA_MEMRDERR;
			/*
			 * re-enable LANCE DMA
			 */
			*(unsigned long *) (system_base + IOCTL + SSR) |= (1 << 16);
			wbflush();
		}
		writereg(&ll->rdp, LE_C0_STOP);

		lance_init_ring(dev);
		load_csrs(lp);
		init_restart_lance(lp);
		netif_wake_queue(dev);
	}
	writereg(&ll->rdp, LE_C0_INEA);
	writereg(&ll->rdp, LE_C0_INEA);
}

struct net_device *last_dev = 0;

static int lance_open(struct net_device *dev)
{
	volatile struct lance_init_block *ib = (struct lance_init_block *) (dev->mem_start);
	struct lance_private *lp = (struct lance_private *) dev->priv;
	volatile struct lance_regs *ll = lp->ll;
	int status = 0;

	last_dev = dev;

	/* Associate IRQ with lance_interrupt */
	if (request_irq(dev->irq, &lance_interrupt, 0, lp->name, dev)) {
		printk("Lance: Can't get irq %d\n", dev->irq);
		return -EAGAIN;
	}
	/* Stop the Lance */
	writereg(&ll->rap, LE_CSR0);
	writereg(&ll->rdp, LE_C0_STOP);

	/* Set mode and clear multicast filter only at device open,
	 * so that lance_init_ring() called at any error will not
	 * forget multicast filters.
	 *
	 * BTW it is common bug in all lance drivers! --ANK
	 */
	ib->mode = 0;
	ib->filter [0] = 0;
	ib->filter [2] = 0;

	lance_init_ring(dev);
	load_csrs(lp);

	netif_start_queue(dev);

	status = init_restart_lance(lp);

	/*
	 * if (!status)
	 *      MOD_INC_USE_COUNT;
	 */

	return status;
}

static int lance_close(struct net_device *dev)
{
	struct lance_private *lp = (struct lance_private *) dev->priv;
	volatile struct lance_regs *ll = lp->ll;

	netif_stop_queue(dev);
	del_timer_sync(&lp->multicast_timer);

	/* Stop the card */
	writereg(&ll->rap, LE_CSR0);
	writereg(&ll->rdp, LE_C0_STOP);

	free_irq(dev->irq, (void *) dev);
	/*
	   MOD_DEC_USE_COUNT;
	 */
	return 0;
}

static inline int lance_reset(struct net_device *dev)
{
	struct lance_private *lp = (struct lance_private *) dev->priv;
	volatile struct lance_regs *ll = lp->ll;
	int status;

	/* Stop the lance */
	writereg(&ll->rap, LE_CSR0);
	writereg(&ll->rdp, LE_C0_STOP);

	lance_init_ring(dev);
	load_csrs(lp);
	dev->trans_start = jiffies;
	status = init_restart_lance(lp);
	return status;
}

static void lance_tx_timeout(struct net_device *dev)
{
	struct lance_private *lp = (struct lance_private *) dev->priv;
	volatile struct lance_regs *ll = lp->ll;

	printk(KERN_ERR "%s: transmit timed out, status %04x, reset\n",
			       dev->name, ll->rdp);
			lance_reset(dev);
	netif_wake_queue(dev);
}

static int lance_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct lance_private *lp = (struct lance_private *) dev->priv;
	volatile struct lance_regs *ll = lp->ll;
	volatile struct lance_init_block *ib = (struct lance_init_block *) (dev->mem_start);
	int entry, skblen, len;

	skblen = skb->len;

	len = (skblen <= ETH_ZLEN) ? ETH_ZLEN : skblen;

	lp->stats.tx_bytes += len;

	entry = lp->tx_new & TX_RING_MOD_MASK;
	ib->btx_ring[entry].length = (-len);
	ib->btx_ring[entry].misc = 0;

	cp_to_buf((char *) lp->tx_buf_ptr_cpu[entry], skb->data, skblen);

	/* Clear the slack of the packet, do I need this? */
	/* For a firewall its a good idea - AC */
/*
   if (len != skblen)
   memset ((char *) &ib->tx_buf [entry][skblen], 0, (len - skblen) << 1);
 */

	/* Now, give the packet to the lance */
	ib->btx_ring[entry].tmd1_bits = (LE_T1_POK | LE_T1_OWN);
	lp->tx_new = (lp->tx_new + 1) & TX_RING_MOD_MASK;

	if (TX_BUFFS_AVAIL <= 0)
		netif_stop_queue(dev);

	/* Kick the lance: transmit now */
	writereg(&ll->rdp, LE_C0_INEA | LE_C0_TDMD);

	spin_unlock_irq(&lp->lock);

	dev->trans_start = jiffies;
	dev_kfree_skb(skb);

 	return 0;
}

static struct net_device_stats *lance_get_stats(struct net_device *dev)
{
	struct lance_private *lp = (struct lance_private *) dev->priv;

	return &lp->stats;
}

static void lance_load_multicast(struct net_device *dev)
{
	volatile struct lance_init_block *ib = (struct lance_init_block *) (dev->mem_start);
	volatile u16 *mcast_table = (u16 *) & ib->filter;
	struct dev_mc_list *dmi = dev->mc_list;
	char *addrs;
	int i, j, bit, byte;
	u32 crc, poly = CRC_POLYNOMIAL_BE;

	/* set all multicast bits */
	if (dev->flags & IFF_ALLMULTI) {
		ib->filter[0] = 0xffff;
		ib->filter[2] = 0xffff;
		ib->filter[4] = 0xffff;
		ib->filter[6] = 0xffff;
		return;
	}
	/* clear the multicast filter */
	ib->filter[0] = 0;
	ib->filter[2] = 0;
	ib->filter[4] = 0;
	ib->filter[6] = 0;

	/* Add addresses */
	for (i = 0; i < dev->mc_count; i++) {
		addrs = dmi->dmi_addr;
		dmi = dmi->next;

		/* multicast address? */
		if (!(*addrs & 1))
			continue;

		crc = 0xffffffff;
		for (byte = 0; byte < 6; byte++)
			for (bit = *addrs++, j = 0; j < 8; j++, bit >>= 1) {
				int test;

				test = ((bit ^ crc) & 0x01);
				crc >>= 1;

				if (test) {
					crc = crc ^ poly;
				}
			}

		crc = crc >> 26;
		mcast_table[crc >> 3] |= 1 << (crc & 0xf);
	}
	return;
}

static void lance_set_multicast(struct net_device *dev)
{
	struct lance_private *lp = (struct lance_private *) dev->priv;
	volatile struct lance_init_block *ib;
	volatile struct lance_regs *ll = lp->ll;

	ib = (struct lance_init_block *) (dev->mem_start);

	if (!netif_running(dev))
		return;

	if (lp->tx_old != lp->tx_new) {
		mod_timer(&lp->multicast_timer, jiffies + 4);
		netif_wake_queue(dev);
		return;
	}

	netif_stop_queue(dev);

	writereg(&ll->rap, LE_CSR0);
	writereg(&ll->rdp, LE_C0_STOP);

	lance_init_ring(dev);

	if (dev->flags & IFF_PROMISC) {
		ib->mode |= LE_MO_PROM;
	} else {
		ib->mode &= ~LE_MO_PROM;
		lance_load_multicast(dev);
	}
	load_csrs(lp);
	init_restart_lance(lp);
	netif_wake_queue(dev);
}

static void lance_set_multicast_retry(unsigned long _opaque)
{
	struct net_device *dev = (struct net_device *) _opaque;

	lance_set_multicast(dev);
}

static int __init dec_lance_init(struct net_device *dev, const int type)
{
	static unsigned version_printed;
	struct net_device *dev;
	struct lance_private *lp;
	volatile struct lance_regs *ll;
	int i, ret;
	unsigned long esar_base;
	unsigned char *esar;

#ifndef CONFIG_TC
	system_base = KN01_LANCE_BASE;
#else
	int slot;
#endif

	if (dec_lance_debug && version_printed++ == 0)
		printk(version);

	dev = init_etherdev(0, sizeof(struct lance_private));
	if (!dev)
		return -ENOMEM;

	/* init_etherdev ensures the data structures used by the LANCE are aligned. */
	lp = (struct lance_private *) dev->priv;
	spin_lock_init(&lp->lock);

	switch (type) {
#ifdef CONFIG_TC
	case ASIC_LANCE:
		dev->base_addr = system_base + LANCE;

		/* buffer space for the on-board LANCE shared memory */
		/*
		 * FIXME: ugly hack!
		 */
		dev->mem_start = KSEG1ADDR(0x0020000);
		dev->mem_end = dev->mem_start + 0x00020000;
		dev->irq = ETHER;
		esar_base = system_base + ESAR;

		/*
		 * setup the pointer arrays, this sucks [tm] :-(
		 */
		for (i = 0; i < RX_RING_SIZE; i++) {
			lp->rx_buf_ptr_cpu[i] = (char *) (dev->mem_start + BUF_OFFSET_CPU
						 + 2 * i * RX_BUFF_SIZE);
			lp->rx_buf_ptr_lnc[i] = (char *) (BUF_OFFSET_LNC
						     + i * RX_BUFF_SIZE);
		}
		for (i = 0; i < TX_RING_SIZE; i++) {
			lp->tx_buf_ptr_cpu[i] = (char *) (dev->mem_start + BUF_OFFSET_CPU
					+ 2 * RX_RING_SIZE * RX_BUFF_SIZE
						 + 2 * i * TX_BUFF_SIZE);
			lp->tx_buf_ptr_lnc[i] = (char *) (BUF_OFFSET_LNC
					    + RX_RING_SIZE * RX_BUFF_SIZE
						     + i * TX_BUFF_SIZE);
		}

		/*
		 * setup and enable IOASIC LANCE DMA
		 */
		lp->dma_ptr_reg = (unsigned long *) (system_base + IOCTL + LANCE_DMA_P);
		*(lp->dma_ptr_reg) = PHYSADDR(dev->mem_start) << 3;
		*(unsigned long *) (system_base + IOCTL + SSR) |= (1 << 16);
		wbflush();

		break;
	case PMAD_LANCE:
		slot = search_tc_card("PMAD-AA");
		claim_tc_card(slot);

		dev->mem_start = get_tc_base_addr(slot);
		dev->base_addr = dev->mem_start + 0x100000;
		dev->irq = get_tc_irq_nr(slot);
		esar_base = dev->mem_start + 0x1c0002;
		break;
#endif
	case PMAX_LANCE:
		dev->irq = ETHER;
		dev->base_addr = KN01_LANCE_BASE;
		dev->mem_start = KN01_LANCE_BASE + 0x01000000;
		esar_base = KN01_RTC_BASE + 1;
		/*
		 * setup the pointer arrays, this sucks [tm] :-(
		 */
		for (i = 0; i < RX_RING_SIZE; i++) {
			lp->rx_buf_ptr_cpu[i] =
			    (char *) (dev->mem_start + BUF_OFFSET_CPU
				      + 2 * i * RX_BUFF_SIZE);

			lp->rx_buf_ptr_lnc[i] =
			    (char *) (BUF_OFFSET_LNC
				      + i * RX_BUFF_SIZE);

		}
		for (i = 0; i < TX_RING_SIZE; i++) {
			lp->tx_buf_ptr_cpu[i] =
			    (char *) (dev->mem_start + BUF_OFFSET_CPU
				      + 2 * RX_RING_SIZE * RX_BUFF_SIZE
				      + 2 * i * TX_BUFF_SIZE);
			lp->tx_buf_ptr_lnc[i] = (char *) (BUF_OFFSET_LNC
					    + RX_RING_SIZE * RX_BUFF_SIZE
						     + i * TX_BUFF_SIZE);

		}
		break;
	default:
		printk("declance_init called with unknown type\n");
		ret = -ENODEV;
		goto err_out;
	}

	ll = (struct lance_regs *) dev->base_addr;
	esar = (unsigned char *) esar_base;

	/* prom checks */
	/* First, check for test pattern */
	if (esar[0x60] != 0xff && esar[0x64] != 0x00 &&
	    esar[0x68] != 0x55 && esar[0x6c] != 0xaa) {
		printk("Ethernet station address prom not found!\n");
		ret = -ENODEV;
		goto err_out;
	}
	/* Check the prom contents */
	for (i = 0; i < 8; i++) {
		if (esar[i * 4] != esar[0x3c - i * 4] &&
		    esar[i * 4] != esar[0x40 + i * 4] &&
		    esar[0x3c - i * 4] != esar[0x40 + i * 4]) {
			printk("Something is wrong with the ethernet "
			       "station address prom!\n");
			ret = -ENODEV;
			goto err_out;
		}
	}

	/* Copy the ethernet address to the device structure, later to the
	 * lance initialization block so the lance gets it every time it's
	 * (re)initialized.
	 */
	switch (type) {
	case ASIC_LANCE:
		printk("%s: IOASIC onboard LANCE, addr = ", dev->name);
		break;
	case PMAD_LANCE:
		printk("%s: PMAD-AA, addr = ", dev->name);
		break;
	case PMAX_LANCE:
		printk("%s: PMAX onboard LANCE, addr = ", dev->name);
		break;
	}
	for (i = 0; i < 6; i++) {
		dev->dev_addr[i] = esar[i * 4];
		printk("%2.2x%c", dev->dev_addr[i], i == 5 ? ',' : ':');
	}

	printk(" irq = %d\n", dev->irq);

	lp->dev = dev;
	dev->open = &lance_open;
	dev->stop = &lance_close;
	dev->hard_start_xmit = &lance_start_xmit;
	dev->tx_timeout = &lance_tx_timeout;
	dev->watchdog_timeo = 5*HZ;
	dev->get_stats = &lance_get_stats;
	dev->set_multicast_list = &lance_set_multicast;

	/* lp->ll is the location of the registers for lance card */
	lp->ll = ll;

	lp->name = lancestr;

	/* busmaster_regval (CSR3) should be zero according to the PMAD-AA
	 * specification.
	 */
	lp->busmaster_regval = 0;

	dev->dma = 0;

	ether_setup(dev);

	/* We cannot sleep if the chip is busy during a
	 * multicast list update event, because such events
	 * can occur from interrupts (ex. IPv6).  So we
	 * use a timer to try again later when necessary. -DaveM
	 */
	init_timer(&lp->multicast_timer);
	lp->multicast_timer.data = (unsigned long) dev;
	lp->multicast_timer.function = &lance_set_multicast_retry;

#ifdef MODULE
	dev->ifindex = dev_new_index();
	lp->next_module = root_lance_dev;
	root_lance_dev = lp;
#endif
	return 0;

err_out:
	unregister_netdev(dev);
	kfree(dev);
	return ret;
}


/* Find all the lance cards on the system and initialize them */
static int __init dec_lance_probe(void)
{
	struct net_device *dev = NULL;
	static int called = 0;

#ifdef MODULE
	root_lance_dev = NULL;
#endif

#ifdef CONFIG_TC
	int slot = -1;

	if (TURBOCHANNEL) {
		if (IOASIC && !called) {
			called = 1;
			type = ASIC_LANCE;
		} else {
			if ((slot = search_tc_card("PMAD-AA")) >= 0) {
				type = PMAD_LANCE;
			} else {
				return -ENODEV;
			}
		}
	} else {
		if (!called) {
			called = 1;
			type = PMAX_LANCE;
		} else {
			return -ENODEV;
		}
	}
#else
	if (!called && !TURBOCHANNEL) {
		called = 1;
		type = PMAX_LANCE;
	} else {
		return -ENODEV;
	}
#endif

	return dec_lance_init(dev, type);
}

static void __exit dec_lance_cleanup(void)
{
#ifdef MODULE
   struct lance_private *lp;

   while (root_lance_dev) {
   lp = root_lance_dev->next_module;

   unregister_netdev(root_lance_dev->dev);
   kfree(root_lance_dev->dev);
   root_lance_dev = lp;
   }
#endif /* MODULE */
}

module_init(dec_lance_probe);
module_exit(dec_lance_cleanup);
