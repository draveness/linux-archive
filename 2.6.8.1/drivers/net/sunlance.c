/* $Id: sunlance.c,v 1.112 2002/01/15 06:48:55 davem Exp $
 * lance.c: Linux/Sparc/Lance driver
 *
 *	Written 1995, 1996 by Miguel de Icaza
 * Sources:
 *	The Linux  depca driver
 *	The Linux  lance driver.
 *	The Linux  skeleton driver.
 *	The NetBSD Sparc/Lance driver.
 *	Theo de Raadt (deraadt@openbsd.org)
 *	NCR92C990 Lan Controller manual
 *
 * 1.4:
 *	Added support to run with a ledma on the Sun4m
 *
 * 1.5:
 *	Added multiple card detection.
 *
 *	 4/17/96: Burst sizes and tpe selection on sun4m by Eddie C. Dost
 *		  (ecd@skynet.be)
 *
 *	 5/15/96: auto carrier detection on sun4m by Eddie C. Dost
 *		  (ecd@skynet.be)
 *
 *	 5/17/96: lebuffer on scsi/ether cards now work David S. Miller
 *		  (davem@caip.rutgers.edu)
 *
 *	 5/29/96: override option 'tpe-link-test?', if it is 'false', as
 *		  this disables auto carrier detection on sun4m. Eddie C. Dost
 *		  (ecd@skynet.be)
 *
 * 1.7:
 *	 6/26/96: Bug fix for multiple ledmas, miguel.
 *
 * 1.8:
 *		  Stole multicast code from depca.c, fixed lance_tx.
 *
 * 1.9:
 *	 8/21/96: Fixed the multicast code (Pedro Roque)
 *
 *	 8/28/96: Send fake packet in lance_open() if auto_select is true,
 *		  so we can detect the carrier loss condition in time.
 *		  Eddie C. Dost (ecd@skynet.be)
 *
 *	 9/15/96: Align rx_buf so that eth_copy_and_sum() won't cause an
 *		  MNA trap during chksum_partial_copy(). (ecd@skynet.be)
 *
 *	11/17/96: Handle LE_C0_MERR in lance_interrupt(). (ecd@skynet.be)
 *
 *	12/22/96: Don't loop forever in lance_rx() on incomplete packets.
 *		  This was the sun4c killer. Shit, stupid bug.
 *		  (ecd@skynet.be)
 *
 * 1.10:
 *	 1/26/97: Modularize driver. (ecd@skynet.be)
 *
 * 1.11:
 *	12/27/97: Added sun4d support. (jj@sunsite.mff.cuni.cz)
 *
 * 1.12:
 * 	 11/3/99: Fixed SMP race in lance_start_xmit found by davem.
 * 	          Anton Blanchard (anton@progsoc.uts.edu.au)
 * 2.00: 11/9/99: Massive overhaul and port to new SBUS driver interfaces.
 *		  David S. Miller (davem@redhat.com)
 * 2.01:
 *      11/08/01: Use library crc32 functions (Matt_Domsch@dell.com)
 *		  
 */

#undef DEBUG_DRIVER

static char version[] =
	"sunlance.c:v2.02 24/Aug/03 Miguel de Icaza (miguel@nuclecu.unam.mx)\n";

static char lancestr[] = "LANCE";

#include <linux/config.h>
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
#include <linux/socket.h> /* Used for the temporal inet entries and routing */
#include <linux/route.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/ethtool.h>

#include <asm/system.h>
#include <asm/bitops.h>
#include <asm/io.h>
#include <asm/dma.h>
#include <asm/pgtable.h>
#include <asm/byteorder.h>	/* Used by the checksum routines */
#include <asm/idprom.h>
#include <asm/sbus.h>
#include <asm/openprom.h>
#include <asm/oplib.h>
#include <asm/auxio.h>		/* For tpe-link-test? setting */
#include <asm/irq.h>

/* Define: 2^4 Tx buffers and 2^4 Rx buffers */
#ifndef LANCE_LOG_TX_BUFFERS
#define LANCE_LOG_TX_BUFFERS 4
#define LANCE_LOG_RX_BUFFERS 4
#endif

#define LE_CSR0 0
#define LE_CSR1 1
#define LE_CSR2 2
#define LE_CSR3 3

#define LE_MO_PROM      0x8000  /* Enable promiscuous mode */

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

#define	LE_C3_BSWP	0x4     /* SWAP */
#define	LE_C3_ACON	0x2	/* ALE Control */
#define	LE_C3_BCON	0x1	/* Byte control */

/* Receive message descriptor 1 */
#define LE_R1_OWN       0x80    /* Who owns the entry */
#define LE_R1_ERR       0x40    /* Error: if FRA, OFL, CRC or BUF is set */
#define LE_R1_FRA       0x20    /* FRA: Frame error */
#define LE_R1_OFL       0x10    /* OFL: Frame overflow */
#define LE_R1_CRC       0x08    /* CRC error */
#define LE_R1_BUF       0x04    /* BUF: Buffer error */
#define LE_R1_SOP       0x02    /* Start of packet */
#define LE_R1_EOP       0x01    /* End of packet */
#define LE_R1_POK       0x03    /* Packet is complete: SOP + EOP */

#define LE_T1_OWN       0x80    /* Lance owns the packet */
#define LE_T1_ERR       0x40    /* Error summary */
#define LE_T1_EMORE     0x10    /* Error: more than one retry needed */
#define LE_T1_EONE      0x08    /* Error: one retry needed */
#define LE_T1_EDEF      0x04    /* Error: deferred */
#define LE_T1_SOP       0x02    /* Start of packet */
#define LE_T1_EOP       0x01    /* End of packet */
#define LE_T1_POK	0x03	/* Packet is complete: SOP + EOP */

#define LE_T3_BUF       0x8000  /* Buffer error */
#define LE_T3_UFL       0x4000  /* Error underflow */
#define LE_T3_LCOL      0x1000  /* Error late collision */
#define LE_T3_CLOS      0x0800  /* Error carrier loss */
#define LE_T3_RTY       0x0400  /* Error retry */
#define LE_T3_TDR       0x03ff  /* Time Domain Reflectometry counter */

#define TX_RING_SIZE			(1 << (LANCE_LOG_TX_BUFFERS))
#define TX_RING_MOD_MASK		(TX_RING_SIZE - 1)
#define TX_RING_LEN_BITS		((LANCE_LOG_TX_BUFFERS) << 29)
#define TX_NEXT(__x)			(((__x)+1) & TX_RING_MOD_MASK)

#define RX_RING_SIZE			(1 << (LANCE_LOG_RX_BUFFERS))
#define RX_RING_MOD_MASK		(RX_RING_SIZE - 1)
#define RX_RING_LEN_BITS		((LANCE_LOG_RX_BUFFERS) << 29)
#define RX_NEXT(__x)			(((__x)+1) & RX_RING_MOD_MASK)

#define PKT_BUF_SZ		1544
#define RX_BUFF_SIZE            PKT_BUF_SZ
#define TX_BUFF_SIZE            PKT_BUF_SZ

struct lance_rx_desc {
	u16	rmd0;		/* low address of packet */
	u8	rmd1_bits;	/* descriptor bits */
	u8	rmd1_hadr;	/* high address of packet */
	s16	length;		/* This length is 2s complement (negative)!
				 * Buffer length
				 */
	u16	mblength;	/* This is the actual number of bytes received */
};

struct lance_tx_desc {
	u16	tmd0;		/* low address of packet */
	u8 	tmd1_bits;	/* descriptor bits */
	u8 	tmd1_hadr;	/* high address of packet */
	s16 	length;		/* Length is 2s complement (negative)! */
	u16 	misc;
};
		
/* The LANCE initialization block, described in databook. */
/* On the Sparc, this block should be on a DMA region     */
struct lance_init_block {
	u16	mode;		/* Pre-set mode (reg. 15) */
	u8	phys_addr[6];	/* Physical ethernet address */
	u32	filter[2];	/* Multicast filter. */

	/* Receive and transmit ring base, along with extra bits. */
	u16	rx_ptr;		/* receive descriptor addr */
	u16	rx_len;		/* receive len and high addr */
	u16	tx_ptr;		/* transmit descriptor addr */
	u16	tx_len;		/* transmit len and high addr */
    
	/* The Tx and Rx ring entries must aligned on 8-byte boundaries. */
	struct lance_rx_desc brx_ring[RX_RING_SIZE];
	struct lance_tx_desc btx_ring[TX_RING_SIZE];
    
	u8	tx_buf [TX_RING_SIZE][TX_BUFF_SIZE];
	u8	pad[2];		/* align rx_buf for copy_and_sum(). */
	u8	rx_buf [RX_RING_SIZE][RX_BUFF_SIZE];
};

#define libdesc_offset(rt, elem) \
((__u32)(((unsigned long)(&(((struct lance_init_block *)0)->rt[elem])))))

#define libbuff_offset(rt, elem) \
((__u32)(((unsigned long)(&(((struct lance_init_block *)0)->rt[elem][0])))))

struct lance_private {
	unsigned long	lregs;		/* Lance RAP/RDP regs.		*/
	unsigned long	dregs;		/* DMA controller regs.		*/
	volatile struct lance_init_block *init_block;
    
	spinlock_t	lock;

	int		rx_new, tx_new;
	int		rx_old, tx_old;
    
	struct net_device_stats	stats;
	struct sbus_dma *ledma;	/* If set this points to ledma	*/
	char		tpe;		/* cable-selection is TPE	*/
	char		auto_select;	/* cable-selection by carrier	*/
	char		burst_sizes;	/* ledma SBus burst sizes	*/
	char		pio_buffer;	/* init block in PIO space?	*/

	unsigned short	busmaster_regval;

	void (*init_ring)(struct net_device *);
	void (*rx)(struct net_device *);
	void (*tx)(struct net_device *);

	char	       	       *name;
	dma_addr_t		init_block_dvma;
	struct net_device      *dev;		  /* Backpointer	*/
	struct lance_private   *next_module;
	struct sbus_dev	       *sdev;
	struct timer_list       multicast_timer;
};

#define TX_BUFFS_AVAIL ((lp->tx_old<=lp->tx_new)?\
			lp->tx_old+TX_RING_MOD_MASK-lp->tx_new:\
			lp->tx_old - lp->tx_new-1)

/* Lance registers. */
#define RDP		0x00UL		/* register data port		*/
#define RAP		0x02UL		/* register address port	*/
#define LANCE_REG_SIZE	0x04UL

#define STOP_LANCE(__lp) \
do {	unsigned long __base = (__lp)->lregs; \
	sbus_writew(LE_CSR0,	__base + RAP); \
	sbus_writew(LE_C0_STOP,	__base + RDP); \
} while (0)

int sparc_lance_debug = 2;

/* The Lance uses 24 bit addresses */
/* On the Sun4c the DVMA will provide the remaining bytes for us */
/* On the Sun4m we have to instruct the ledma to provide them    */
/* Even worse, on scsi/ether SBUS cards, the init block and the
 * transmit/receive buffers are addresses as offsets from absolute
 * zero on the lebuffer PIO area. -DaveM
 */

#define LANCE_ADDR(x) ((long)(x) & ~0xff000000)

static struct lance_private *root_lance_dev;

/* Load the CSR registers */
static void load_csrs(struct lance_private *lp)
{
	u32 leptr;

	if (lp->pio_buffer)
		leptr = 0;
	else
		leptr = LANCE_ADDR(lp->init_block_dvma);

	sbus_writew(LE_CSR1,		  lp->lregs + RAP);
	sbus_writew(leptr & 0xffff,	  lp->lregs + RDP);
	sbus_writew(LE_CSR2,		  lp->lregs + RAP);
	sbus_writew(leptr >> 16,	  lp->lregs + RDP);
	sbus_writew(LE_CSR3,		  lp->lregs + RAP);
	sbus_writew(lp->busmaster_regval, lp->lregs + RDP);

	/* Point back to csr0 */
	sbus_writew(LE_CSR0, lp->lregs + RAP);
}

/* Setup the Lance Rx and Tx rings */
static void lance_init_ring_dvma(struct net_device *dev)
{
	struct lance_private *lp = netdev_priv(dev);
	volatile struct lance_init_block *ib = lp->init_block;
	dma_addr_t aib = lp->init_block_dvma;
	__u32 leptr;
	int i;
    
	/* Lock out other processes while setting up hardware */
	netif_stop_queue(dev);
	lp->rx_new = lp->tx_new = 0;
	lp->rx_old = lp->tx_old = 0;

	/* Copy the ethernet address to the lance init block
	 * Note that on the sparc you need to swap the ethernet address.
	 */
	ib->phys_addr [0] = dev->dev_addr [1];
	ib->phys_addr [1] = dev->dev_addr [0];
	ib->phys_addr [2] = dev->dev_addr [3];
	ib->phys_addr [3] = dev->dev_addr [2];
	ib->phys_addr [4] = dev->dev_addr [5];
	ib->phys_addr [5] = dev->dev_addr [4];

	/* Setup the Tx ring entries */
	for (i = 0; i <= TX_RING_SIZE; i++) {
		leptr = LANCE_ADDR(aib + libbuff_offset(tx_buf, i));
		ib->btx_ring [i].tmd0      = leptr;
		ib->btx_ring [i].tmd1_hadr = leptr >> 16;
		ib->btx_ring [i].tmd1_bits = 0;
		ib->btx_ring [i].length    = 0xf000; /* The ones required by tmd2 */
		ib->btx_ring [i].misc      = 0;
	}

	/* Setup the Rx ring entries */
	for (i = 0; i < RX_RING_SIZE; i++) {
		leptr = LANCE_ADDR(aib + libbuff_offset(rx_buf, i));

		ib->brx_ring [i].rmd0      = leptr;
		ib->brx_ring [i].rmd1_hadr = leptr >> 16;
		ib->brx_ring [i].rmd1_bits = LE_R1_OWN;
		ib->brx_ring [i].length    = -RX_BUFF_SIZE | 0xf000;
		ib->brx_ring [i].mblength  = 0;
	}

	/* Setup the initialization block */
    
	/* Setup rx descriptor pointer */
	leptr = LANCE_ADDR(aib + libdesc_offset(brx_ring, 0));
	ib->rx_len = (LANCE_LOG_RX_BUFFERS << 13) | (leptr >> 16);
	ib->rx_ptr = leptr;
    
	/* Setup tx descriptor pointer */
	leptr = LANCE_ADDR(aib + libdesc_offset(btx_ring, 0));
	ib->tx_len = (LANCE_LOG_TX_BUFFERS << 13) | (leptr >> 16);
	ib->tx_ptr = leptr;
}

static void lance_init_ring_pio(struct net_device *dev)
{
	struct lance_private *lp = netdev_priv(dev);
	volatile struct lance_init_block *ib = lp->init_block;
	u32 leptr;
	int i;
    
	/* Lock out other processes while setting up hardware */
	netif_stop_queue(dev);
	lp->rx_new = lp->tx_new = 0;
	lp->rx_old = lp->tx_old = 0;

	/* Copy the ethernet address to the lance init block
	 * Note that on the sparc you need to swap the ethernet address.
	 */
	sbus_writeb(dev->dev_addr[1], &ib->phys_addr[0]);
	sbus_writeb(dev->dev_addr[0], &ib->phys_addr[1]);
	sbus_writeb(dev->dev_addr[3], &ib->phys_addr[2]);
	sbus_writeb(dev->dev_addr[2], &ib->phys_addr[3]);
	sbus_writeb(dev->dev_addr[5], &ib->phys_addr[4]);
	sbus_writeb(dev->dev_addr[4], &ib->phys_addr[5]);

	/* Setup the Tx ring entries */
	for (i = 0; i <= TX_RING_SIZE; i++) {
		leptr = libbuff_offset(tx_buf, i);
		sbus_writew(leptr,	&ib->btx_ring [i].tmd0);
		sbus_writeb(leptr >> 16,&ib->btx_ring [i].tmd1_hadr);
		sbus_writeb(0,		&ib->btx_ring [i].tmd1_bits);

		/* The ones required by tmd2 */
		sbus_writew(0xf000,	&ib->btx_ring [i].length);
		sbus_writew(0,		&ib->btx_ring [i].misc);
	}

	/* Setup the Rx ring entries */
	for (i = 0; i < RX_RING_SIZE; i++) {
		leptr = libbuff_offset(rx_buf, i);

		sbus_writew(leptr,	&ib->brx_ring [i].rmd0);
		sbus_writeb(leptr >> 16,&ib->brx_ring [i].rmd1_hadr);
		sbus_writeb(LE_R1_OWN,	&ib->brx_ring [i].rmd1_bits);
		sbus_writew(-RX_BUFF_SIZE|0xf000,
			    &ib->brx_ring [i].length);
		sbus_writew(0,		&ib->brx_ring [i].mblength);
	}

	/* Setup the initialization block */
    
	/* Setup rx descriptor pointer */
	leptr = libdesc_offset(brx_ring, 0);
	sbus_writew((LANCE_LOG_RX_BUFFERS << 13) | (leptr >> 16),
		    &ib->rx_len);
	sbus_writew(leptr, &ib->rx_ptr);
    
	/* Setup tx descriptor pointer */
	leptr = libdesc_offset(btx_ring, 0);
	sbus_writew((LANCE_LOG_TX_BUFFERS << 13) | (leptr >> 16),
		    &ib->tx_len);
	sbus_writew(leptr, &ib->tx_ptr);
}

static void init_restart_ledma(struct lance_private *lp)
{
	u32 csr = sbus_readl(lp->dregs + DMA_CSR);

	if (!(csr & DMA_HNDL_ERROR)) {
		/* E-Cache draining */
		while (sbus_readl(lp->dregs + DMA_CSR) & DMA_FIFO_ISDRAIN)
			barrier();
	}

	csr = sbus_readl(lp->dregs + DMA_CSR);
	csr &= ~DMA_E_BURSTS;
	if (lp->burst_sizes & DMA_BURST32)
		csr |= DMA_E_BURST32;
	else
		csr |= DMA_E_BURST16;

	csr |= (DMA_DSBL_RD_DRN | DMA_DSBL_WR_INV | DMA_FIFO_INV);

	if (lp->tpe)
		csr |= DMA_EN_ENETAUI;
	else
		csr &= ~DMA_EN_ENETAUI;
	udelay(20);
	sbus_writel(csr, lp->dregs + DMA_CSR);
	udelay(200);
}

static int init_restart_lance(struct lance_private *lp)
{
	u16 regval = 0;
	int i;

	if (lp->dregs)
		init_restart_ledma(lp);

	sbus_writew(LE_CSR0,	lp->lregs + RAP);
	sbus_writew(LE_C0_INIT,	lp->lregs + RDP);

	/* Wait for the lance to complete initialization */
	for (i = 0; i < 100; i++) {
		regval = sbus_readw(lp->lregs + RDP);

		if (regval & (LE_C0_ERR | LE_C0_IDON))
			break;
		barrier();
	}
	if (i == 100 || (regval & LE_C0_ERR)) {
		printk(KERN_ERR "LANCE unopened after %d ticks, csr0=%4.4x.\n",
		       i, regval);
		if (lp->dregs)
			printk("dcsr=%8.8x\n", sbus_readl(lp->dregs + DMA_CSR));
		return -1;
	}

	/* Clear IDON by writing a "1", enable interrupts and start lance */
	sbus_writew(LE_C0_IDON,			lp->lregs + RDP);
	sbus_writew(LE_C0_INEA | LE_C0_STRT,	lp->lregs + RDP);

	if (lp->dregs) {
		u32 csr = sbus_readl(lp->dregs + DMA_CSR);

		csr |= DMA_INT_ENAB;
		sbus_writel(csr, lp->dregs + DMA_CSR);
	}

	return 0;
}

static void lance_rx_dvma(struct net_device *dev)
{
	struct lance_private *lp = netdev_priv(dev);
	volatile struct lance_init_block *ib = lp->init_block;
	volatile struct lance_rx_desc *rd;
	u8 bits;
	int len, entry = lp->rx_new;
	struct sk_buff *skb;

	for (rd = &ib->brx_ring [entry];
	     !((bits = rd->rmd1_bits) & LE_R1_OWN);
	     rd = &ib->brx_ring [entry]) {

		/* We got an incomplete frame? */
		if ((bits & LE_R1_POK) != LE_R1_POK) {
			lp->stats.rx_over_errors++;
			lp->stats.rx_errors++;
		} else if (bits & LE_R1_ERR) {
			/* Count only the end frame as a rx error,
			 * not the beginning
			 */
			if (bits & LE_R1_BUF) lp->stats.rx_fifo_errors++;
			if (bits & LE_R1_CRC) lp->stats.rx_crc_errors++;
			if (bits & LE_R1_OFL) lp->stats.rx_over_errors++;
			if (bits & LE_R1_FRA) lp->stats.rx_frame_errors++;
			if (bits & LE_R1_EOP) lp->stats.rx_errors++;
		} else {
			len = (rd->mblength & 0xfff) - 4;
			skb = dev_alloc_skb(len + 2);

			if (skb == NULL) {
				printk(KERN_INFO "%s: Memory squeeze, deferring packet.\n",
				       dev->name);
				lp->stats.rx_dropped++;
				rd->mblength = 0;
				rd->rmd1_bits = LE_R1_OWN;
				lp->rx_new = RX_NEXT(entry);
				return;
			}
	    
			lp->stats.rx_bytes += len;

			skb->dev = dev;
			skb_reserve(skb, 2);		/* 16 byte align */
			skb_put(skb, len);		/* make room */
			eth_copy_and_sum(skb,
					 (unsigned char *)&(ib->rx_buf [entry][0]),
					 len, 0);
			skb->protocol = eth_type_trans(skb, dev);
			netif_rx(skb);
			dev->last_rx = jiffies;
			lp->stats.rx_packets++;
		}

		/* Return the packet to the pool */
		rd->mblength = 0;
		rd->rmd1_bits = LE_R1_OWN;
		entry = RX_NEXT(entry);
	}

	lp->rx_new = entry;
}

static void lance_tx_dvma(struct net_device *dev)
{
	struct lance_private *lp = netdev_priv(dev);
	volatile struct lance_init_block *ib = lp->init_block;
	int i, j;

	spin_lock(&lp->lock);

	j = lp->tx_old;
	for (i = j; i != lp->tx_new; i = j) {
		volatile struct lance_tx_desc *td = &ib->btx_ring [i];
		u8 bits = td->tmd1_bits;

		/* If we hit a packet not owned by us, stop */
		if (bits & LE_T1_OWN)
			break;
		
		if (bits & LE_T1_ERR) {
			u16 status = td->misc;
	    
			lp->stats.tx_errors++;
			if (status & LE_T3_RTY)  lp->stats.tx_aborted_errors++;
			if (status & LE_T3_LCOL) lp->stats.tx_window_errors++;

			if (status & LE_T3_CLOS) {
				lp->stats.tx_carrier_errors++;
				if (lp->auto_select) {
					lp->tpe = 1 - lp->tpe;
					printk(KERN_NOTICE "%s: Carrier Lost, trying %s\n",
					       dev->name, lp->tpe?"TPE":"AUI");
					STOP_LANCE(lp);
					lp->init_ring(dev);
					load_csrs(lp);
					init_restart_lance(lp);
					goto out;
				}
			}

			/* Buffer errors and underflows turn off the
			 * transmitter, restart the adapter.
			 */
			if (status & (LE_T3_BUF|LE_T3_UFL)) {
				lp->stats.tx_fifo_errors++;

				printk(KERN_ERR "%s: Tx: ERR_BUF|ERR_UFL, restarting\n",
				       dev->name);
				STOP_LANCE(lp);
				lp->init_ring(dev);
				load_csrs(lp);
				init_restart_lance(lp);
				goto out;
			}
		} else if ((bits & LE_T1_POK) == LE_T1_POK) {
			/*
			 * So we don't count the packet more than once.
			 */
			td->tmd1_bits = bits & ~(LE_T1_POK);

			/* One collision before packet was sent. */
			if (bits & LE_T1_EONE)
				lp->stats.collisions++;

			/* More than one collision, be optimistic. */
			if (bits & LE_T1_EMORE)
				lp->stats.collisions += 2;

			lp->stats.tx_packets++;
		}
	
		j = TX_NEXT(j);
	}
	lp->tx_old = j;
out:
	if (netif_queue_stopped(dev) &&
	    TX_BUFFS_AVAIL > 0)
		netif_wake_queue(dev);

	spin_unlock(&lp->lock);
}

static void lance_piocopy_to_skb(struct sk_buff *skb, volatile void *piobuf, int len)
{
	u16 *p16 = (u16 *) skb->data;
	u32 *p32;
	u8 *p8;
	unsigned long pbuf = (unsigned long) piobuf;

	/* We know here that both src and dest are on a 16bit boundary. */
	*p16++ = sbus_readw(pbuf);
	p32 = (u32 *) p16;
	pbuf += 2;
	len -= 2;

	while (len >= 4) {
		*p32++ = sbus_readl(pbuf);
		pbuf += 4;
		len -= 4;
	}
	p8 = (u8 *) p32;
	if (len >= 2) {
		p16 = (u16 *) p32;
		*p16++ = sbus_readw(pbuf);
		pbuf += 2;
		len -= 2;
		p8 = (u8 *) p16;
	}
	if (len >= 1)
		*p8 = sbus_readb(pbuf);
}

static void lance_rx_pio(struct net_device *dev)
{
	struct lance_private *lp = netdev_priv(dev);
	volatile struct lance_init_block *ib = lp->init_block;
	volatile struct lance_rx_desc *rd;
	unsigned char bits;
	int len, entry;
	struct sk_buff *skb;

	entry = lp->rx_new;
	for (rd = &ib->brx_ring [entry];
	     !((bits = sbus_readb(&rd->rmd1_bits)) & LE_R1_OWN);
	     rd = &ib->brx_ring [entry]) {

		/* We got an incomplete frame? */
		if ((bits & LE_R1_POK) != LE_R1_POK) {
			lp->stats.rx_over_errors++;
			lp->stats.rx_errors++;
		} else if (bits & LE_R1_ERR) {
			/* Count only the end frame as a rx error,
			 * not the beginning
			 */
			if (bits & LE_R1_BUF) lp->stats.rx_fifo_errors++;
			if (bits & LE_R1_CRC) lp->stats.rx_crc_errors++;
			if (bits & LE_R1_OFL) lp->stats.rx_over_errors++;
			if (bits & LE_R1_FRA) lp->stats.rx_frame_errors++;
			if (bits & LE_R1_EOP) lp->stats.rx_errors++;
		} else {
			len = (sbus_readw(&rd->mblength) & 0xfff) - 4;
			skb = dev_alloc_skb(len + 2);

			if (skb == NULL) {
				printk(KERN_INFO "%s: Memory squeeze, deferring packet.\n",
				       dev->name);
				lp->stats.rx_dropped++;
				sbus_writew(0, &rd->mblength);
				sbus_writeb(LE_R1_OWN, &rd->rmd1_bits);
				lp->rx_new = RX_NEXT(entry);
				return;
			}
	    
			lp->stats.rx_bytes += len;

			skb->dev = dev;
			skb_reserve (skb, 2);		/* 16 byte align */
			skb_put(skb, len);		/* make room */
			lance_piocopy_to_skb(skb, &(ib->rx_buf[entry][0]), len);
			skb->protocol = eth_type_trans(skb, dev);
			netif_rx(skb);
			dev->last_rx = jiffies;
			lp->stats.rx_packets++;
		}

		/* Return the packet to the pool */
		sbus_writew(0, &rd->mblength);
		sbus_writeb(LE_R1_OWN, &rd->rmd1_bits);
		entry = RX_NEXT(entry);
	}

	lp->rx_new = entry;
}

static void lance_tx_pio(struct net_device *dev)
{
	struct lance_private *lp = netdev_priv(dev);
	volatile struct lance_init_block *ib = lp->init_block;
	int i, j;

	spin_lock(&lp->lock);

	j = lp->tx_old;
	for (i = j; i != lp->tx_new; i = j) {
		volatile struct lance_tx_desc *td = &ib->btx_ring [i];
		u8 bits = sbus_readb(&td->tmd1_bits);

		/* If we hit a packet not owned by us, stop */
		if (bits & LE_T1_OWN)
			break;
		
		if (bits & LE_T1_ERR) {
			u16 status = sbus_readw(&td->misc);
	    
			lp->stats.tx_errors++;
			if (status & LE_T3_RTY)  lp->stats.tx_aborted_errors++;
			if (status & LE_T3_LCOL) lp->stats.tx_window_errors++;

			if (status & LE_T3_CLOS) {
				lp->stats.tx_carrier_errors++;
				if (lp->auto_select) {
					lp->tpe = 1 - lp->tpe;
					printk(KERN_NOTICE "%s: Carrier Lost, trying %s\n",
					       dev->name, lp->tpe?"TPE":"AUI");
					STOP_LANCE(lp);
					lp->init_ring(dev);
					load_csrs(lp);
					init_restart_lance(lp);
					goto out;
				}
			}

			/* Buffer errors and underflows turn off the
			 * transmitter, restart the adapter.
			 */
			if (status & (LE_T3_BUF|LE_T3_UFL)) {
				lp->stats.tx_fifo_errors++;

				printk(KERN_ERR "%s: Tx: ERR_BUF|ERR_UFL, restarting\n",
				       dev->name);
				STOP_LANCE(lp);
				lp->init_ring(dev);
				load_csrs(lp);
				init_restart_lance(lp);
				goto out;
			}
		} else if ((bits & LE_T1_POK) == LE_T1_POK) {
			/*
			 * So we don't count the packet more than once.
			 */
			sbus_writeb(bits & ~(LE_T1_POK), &td->tmd1_bits);

			/* One collision before packet was sent. */
			if (bits & LE_T1_EONE)
				lp->stats.collisions++;

			/* More than one collision, be optimistic. */
			if (bits & LE_T1_EMORE)
				lp->stats.collisions += 2;

			lp->stats.tx_packets++;
		}
	
		j = TX_NEXT(j);
	}
	lp->tx_old = j;

	if (netif_queue_stopped(dev) &&
	    TX_BUFFS_AVAIL > 0)
		netif_wake_queue(dev);
out:
	spin_unlock(&lp->lock);
}

static irqreturn_t lance_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	struct net_device *dev = (struct net_device *)dev_id;
	struct lance_private *lp = netdev_priv(dev);
	int csr0;
    
	sbus_writew(LE_CSR0, lp->lregs + RAP);
	csr0 = sbus_readw(lp->lregs + RDP);

	/* Acknowledge all the interrupt sources ASAP */
	sbus_writew(csr0 & (LE_C0_INTR | LE_C0_TINT | LE_C0_RINT),
		    lp->lregs + RDP);
    
	if ((csr0 & LE_C0_ERR) != 0) {
		/* Clear the error condition */
		sbus_writew((LE_C0_BABL | LE_C0_ERR | LE_C0_MISS |
			     LE_C0_CERR | LE_C0_MERR),
			    lp->lregs + RDP);
	}
    
	if (csr0 & LE_C0_RINT)
		lp->rx(dev);
    
	if (csr0 & LE_C0_TINT)
		lp->tx(dev);
    
	if (csr0 & LE_C0_BABL)
		lp->stats.tx_errors++;

	if (csr0 & LE_C0_MISS)
		lp->stats.rx_errors++;

	if (csr0 & LE_C0_MERR) {
		if (lp->dregs) {
			u32 addr = sbus_readl(lp->dregs + DMA_ADDR);

			printk(KERN_ERR "%s: Memory error, status %04x, addr %06x\n",
			       dev->name, csr0, addr & 0xffffff);
		} else {
			printk(KERN_ERR "%s: Memory error, status %04x\n",
			       dev->name, csr0);
		}

		sbus_writew(LE_C0_STOP, lp->lregs + RDP);

		if (lp->dregs) {
			u32 dma_csr = sbus_readl(lp->dregs + DMA_CSR);

			dma_csr |= DMA_FIFO_INV;
			sbus_writel(dma_csr, lp->dregs + DMA_CSR);
		}

		lp->init_ring(dev);
		load_csrs(lp);
		init_restart_lance(lp);
		netif_wake_queue(dev);
	}

	sbus_writew(LE_C0_INEA, lp->lregs + RDP);

	return IRQ_HANDLED;
}

/* Build a fake network packet and send it to ourselves. */
static void build_fake_packet(struct lance_private *lp)
{
	struct net_device *dev = lp->dev;
	volatile struct lance_init_block *ib = lp->init_block;
	u16 *packet;
	struct ethhdr *eth;
	int i, entry;

	entry = lp->tx_new & TX_RING_MOD_MASK;
	packet = (u16 *) &(ib->tx_buf[entry][0]);
	eth = (struct ethhdr *) packet;
	if (lp->pio_buffer) {
		for (i = 0; i < (ETH_ZLEN / sizeof(u16)); i++)
			sbus_writew(0, &packet[i]);
		for (i = 0; i < 6; i++) {
			sbus_writeb(dev->dev_addr[i], &eth->h_dest[i]);
			sbus_writeb(dev->dev_addr[i], &eth->h_source[i]);
		}
		sbus_writew((-ETH_ZLEN) | 0xf000, &ib->btx_ring[entry].length);
		sbus_writew(0, &ib->btx_ring[entry].misc);
		sbus_writeb(LE_T1_POK|LE_T1_OWN, &ib->btx_ring[entry].tmd1_bits);
	} else {
		memset(packet, 0, ETH_ZLEN);
		for (i = 0; i < 6; i++) {
			eth->h_dest[i] = dev->dev_addr[i];
			eth->h_source[i] = dev->dev_addr[i];
		}
		ib->btx_ring[entry].length = (-ETH_ZLEN) | 0xf000;
		ib->btx_ring[entry].misc = 0;
		ib->btx_ring[entry].tmd1_bits = (LE_T1_POK|LE_T1_OWN);
	}
	lp->tx_new = TX_NEXT(entry);
}

struct net_device *last_dev;

static int lance_open(struct net_device *dev)
{
	struct lance_private *lp = netdev_priv(dev);
	volatile struct lance_init_block *ib = lp->init_block;
	int status = 0;

	last_dev = dev;

	STOP_LANCE(lp);

	if (request_irq(dev->irq, &lance_interrupt, SA_SHIRQ,
			lancestr, (void *) dev)) {
		printk(KERN_ERR "Lance: Can't get irq %s\n", __irq_itoa(dev->irq));
		return -EAGAIN;
	}

	/* On the 4m, setup the ledma to provide the upper bits for buffers */
	if (lp->dregs) {
		u32 regval = lp->init_block_dvma & 0xff000000;

		sbus_writel(regval, lp->dregs + DMA_TEST);
	}

	/* Set mode and clear multicast filter only at device open,
	 * so that lance_init_ring() called at any error will not
	 * forget multicast filters.
	 *
	 * BTW it is common bug in all lance drivers! --ANK
	 */
	if (lp->pio_buffer) {
		sbus_writew(0, &ib->mode);
		sbus_writel(0, &ib->filter[0]);
		sbus_writel(0, &ib->filter[1]);
	} else {
		ib->mode = 0;
		ib->filter [0] = 0;
		ib->filter [1] = 0;
	}

	lp->init_ring(dev);
	load_csrs(lp);

	netif_start_queue(dev);

	status = init_restart_lance(lp);
	if (!status && lp->auto_select) {
		build_fake_packet(lp);
		sbus_writew(LE_C0_INEA | LE_C0_TDMD, lp->lregs + RDP);
	}

	return status;
}

static int lance_close(struct net_device *dev)
{
	struct lance_private *lp = netdev_priv(dev);

	netif_stop_queue(dev);
	del_timer_sync(&lp->multicast_timer);

	STOP_LANCE(lp);

	free_irq(dev->irq, (void *) dev);
	return 0;
}

static int lance_reset(struct net_device *dev)
{
	struct lance_private *lp = netdev_priv(dev);
	int status;
    
	STOP_LANCE(lp);

	/* On the 4m, reset the dma too */
	if (lp->dregs) {
		u32 csr, addr;

		printk(KERN_ERR "resetting ledma\n");
		csr = sbus_readl(lp->dregs + DMA_CSR);
		sbus_writel(csr | DMA_RST_ENET, lp->dregs + DMA_CSR);
		udelay(200);
		sbus_writel(csr & ~DMA_RST_ENET, lp->dregs + DMA_CSR);

		addr = lp->init_block_dvma & 0xff000000;
		sbus_writel(addr, lp->dregs + DMA_TEST);
	}
	lp->init_ring(dev);
	load_csrs(lp);
	dev->trans_start = jiffies;
	status = init_restart_lance(lp);
	return status;
}

static void lance_piocopy_from_skb(volatile void *dest, unsigned char *src, int len)
{
	unsigned long piobuf = (unsigned long) dest;
	u32 *p32;
	u16 *p16;
	u8 *p8;

	switch ((unsigned long)src & 0x3) {
	case 0:
		p32 = (u32 *) src;
		while (len >= 4) {
			sbus_writel(*p32, piobuf);
			p32++;
			piobuf += 4;
			len -= 4;
		}
		src = (char *) p32;
		break;
	case 1:
	case 3:
		p8 = (u8 *) src;
		while (len >= 4) {
			u32 val;

			val  = p8[0] << 24;
			val |= p8[1] << 16;
			val |= p8[2] << 8;
			val |= p8[3];
			sbus_writel(val, piobuf);
			p8 += 4;
			piobuf += 4;
			len -= 4;
		}
		src = (char *) p8;
		break;
	case 2:
		p16 = (u16 *) src;
		while (len >= 4) {
			u32 val = p16[0]<<16 | p16[1];
			sbus_writel(val, piobuf);
			p16 += 2;
			piobuf += 4;
			len -= 4;
		}
		src = (char *) p16;
		break;
	};
	if (len >= 2) {
		u16 val = src[0] << 8 | src[1];
		sbus_writew(val, piobuf);
		src += 2;
		piobuf += 2;
		len -= 2;
	}
	if (len >= 1)
		sbus_writeb(src[0], piobuf);
}

static void lance_piozero(volatile void *dest, int len)
{
	unsigned long piobuf = (unsigned long) dest;

	if (piobuf & 1) {
		sbus_writeb(0, piobuf);
		piobuf += 1;
		len -= 1;
		if (len == 0)
			return;
	}
	if (len == 1) {
		sbus_writeb(0, piobuf);
		return;
	}
	if (piobuf & 2) {
		sbus_writew(0, piobuf);
		piobuf += 2;
		len -= 2;
		if (len == 0)
			return;
	}
	while (len >= 4) {
		sbus_writel(0, piobuf);
		piobuf += 4;
		len -= 4;
	}
	if (len >= 2) {
		sbus_writew(0, piobuf);
		piobuf += 2;
		len -= 2;
	}
	if (len >= 1)
		sbus_writeb(0, piobuf);
}

static void lance_tx_timeout(struct net_device *dev)
{
	struct lance_private *lp = netdev_priv(dev);

	printk(KERN_ERR "%s: transmit timed out, status %04x, reset\n",
	       dev->name, sbus_readw(lp->lregs + RDP));
	lance_reset(dev);
	netif_wake_queue(dev);
}

static int lance_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct lance_private *lp = netdev_priv(dev);
	volatile struct lance_init_block *ib = lp->init_block;
	int entry, skblen, len;

	skblen = skb->len;

	len = (skblen <= ETH_ZLEN) ? ETH_ZLEN : skblen;

	spin_lock_irq(&lp->lock);

	lp->stats.tx_bytes += len;

	entry = lp->tx_new & TX_RING_MOD_MASK;
	if (lp->pio_buffer) {
		sbus_writew((-len) | 0xf000, &ib->btx_ring[entry].length);
		sbus_writew(0, &ib->btx_ring[entry].misc);
		lance_piocopy_from_skb(&ib->tx_buf[entry][0], skb->data, skblen);
		if (len != skblen)
			lance_piozero(&ib->tx_buf[entry][skblen], len - skblen);
		sbus_writeb(LE_T1_POK | LE_T1_OWN, &ib->btx_ring[entry].tmd1_bits);
	} else {
		ib->btx_ring [entry].length = (-len) | 0xf000;
		ib->btx_ring [entry].misc = 0;
		memcpy((char *)&ib->tx_buf [entry][0], skb->data, skblen);
		if (len != skblen)
			memset((char *) &ib->tx_buf [entry][skblen], 0, len - skblen);
		ib->btx_ring [entry].tmd1_bits = (LE_T1_POK | LE_T1_OWN);
	}

	lp->tx_new = TX_NEXT(entry);

	if (TX_BUFFS_AVAIL <= 0)
		netif_stop_queue(dev);

	/* Kick the lance: transmit now */
	sbus_writew(LE_C0_INEA | LE_C0_TDMD, lp->lregs + RDP);

	/* Read back CSR to invalidate the E-Cache.
	 * This is needed, because DMA_DSBL_WR_INV is set.
	 */
	if (lp->dregs)
		sbus_readw(lp->lregs + RDP);

	spin_unlock_irq(&lp->lock);

	dev->trans_start = jiffies;
	dev_kfree_skb(skb);
    
	return 0;
}

static struct net_device_stats *lance_get_stats(struct net_device *dev)
{
	struct lance_private *lp = netdev_priv(dev);

	return &lp->stats;
}

/* taken from the depca driver */
static void lance_load_multicast(struct net_device *dev)
{
	struct lance_private *lp = netdev_priv(dev);
	volatile struct lance_init_block *ib = lp->init_block;
	volatile u16 *mcast_table = (u16 *) &ib->filter;
	struct dev_mc_list *dmi = dev->mc_list;
	char *addrs;
	int i;
	u32 crc;
	
	/* set all multicast bits */
	if (dev->flags & IFF_ALLMULTI) {
		if (lp->pio_buffer) {
			sbus_writel(0xffffffff, &ib->filter[0]);
			sbus_writel(0xffffffff, &ib->filter[1]);
		} else {
			ib->filter [0] = 0xffffffff;
			ib->filter [1] = 0xffffffff;
		}
		return;
	}
	/* clear the multicast filter */
	if (lp->pio_buffer) {
		sbus_writel(0, &ib->filter[0]);
		sbus_writel(0, &ib->filter[1]);
	} else {
		ib->filter [0] = 0;
		ib->filter [1] = 0;
	}

	/* Add addresses */
	for (i = 0; i < dev->mc_count; i++) {
		addrs = dmi->dmi_addr;
		dmi   = dmi->next;

		/* multicast address? */
		if (!(*addrs & 1))
			continue;
		crc = ether_crc_le(6, addrs);
		crc = crc >> 26;
		if (lp->pio_buffer) {
			u16 tmp = sbus_readw(&mcast_table[crc>>4]);
			tmp |= 1 << (crc & 0xf);
			sbus_writew(tmp, &mcast_table[crc>>4]);
		} else {
			mcast_table [crc >> 4] |= 1 << (crc & 0xf);
		}
	}
}

static void lance_set_multicast(struct net_device *dev)
{
	struct lance_private *lp = netdev_priv(dev);
	volatile struct lance_init_block *ib = lp->init_block;
	u16 mode;

	if (!netif_running(dev))
		return;

	if (lp->tx_old != lp->tx_new) {
		mod_timer(&lp->multicast_timer, jiffies + 4);
		netif_wake_queue(dev);
		return;
	}

	netif_stop_queue(dev);

	STOP_LANCE(lp);
	lp->init_ring(dev);

	if (lp->pio_buffer)
		mode = sbus_readw(&ib->mode);
	else
		mode = ib->mode;
	if (dev->flags & IFF_PROMISC) {
		mode |= LE_MO_PROM;
		if (lp->pio_buffer)
			sbus_writew(mode, &ib->mode);
		else
			ib->mode = mode;
	} else {
		mode &= ~LE_MO_PROM;
		if (lp->pio_buffer)
			sbus_writew(mode, &ib->mode);
		else
			ib->mode = mode;
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

static void lance_free_hwresources(struct lance_private *lp)
{
	if (lp->lregs)
		sbus_iounmap(lp->lregs, LANCE_REG_SIZE);
	if (lp->init_block != NULL) {
		if (lp->pio_buffer) {
			sbus_iounmap((unsigned long)lp->init_block,
				     sizeof(struct lance_init_block));
		} else {
			sbus_free_consistent(lp->sdev,
					     sizeof(struct lance_init_block),
					     (void *)lp->init_block,
					     lp->init_block_dvma);
		}
	}
}

/* Ethtool support... */
static void sparc_lance_get_drvinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
	struct lance_private *lp = netdev_priv(dev);

	strcpy(info->driver, "sunlance");
	strcpy(info->version, "2.02");
	sprintf(info->bus_info, "SBUS:%d",
		lp->sdev->slot);
}

static u32 sparc_lance_get_link(struct net_device *dev)
{
	/* We really do not keep track of this, but this
	 * is better than not reporting anything at all.
	 */
	return 1;
}

static struct ethtool_ops sparc_lance_ethtool_ops = {
	.get_drvinfo		= sparc_lance_get_drvinfo,
	.get_link		= sparc_lance_get_link,
};

static int __init sparc_lance_init(struct sbus_dev *sdev,
				   struct sbus_dma *ledma,
				   struct sbus_dev *lebuffer)
{
	static unsigned version_printed;
	struct net_device *dev;
	struct lance_private *lp;
	int    i;

	dev = alloc_etherdev(sizeof(struct lance_private) + 8);
	if (!dev)
		return -ENOMEM;

	lp = netdev_priv(dev);

	if (sparc_lance_debug && version_printed++ == 0)
		printk (KERN_INFO "%s", version);

	spin_lock_init(&lp->lock);

	/* Copy the IDPROM ethernet address to the device structure, later we
	 * will copy the address in the device structure to the lance
	 * initialization block.
	 */
	for (i = 0; i < 6; i++)
		dev->dev_addr[i] = idprom->id_ethaddr[i];

	/* Get the IO region */
	lp->lregs = sbus_ioremap(&sdev->resource[0], 0,
				 LANCE_REG_SIZE, lancestr);
	if (lp->lregs == 0UL) {
		printk(KERN_ERR "SunLance: Cannot map registers.\n");
		goto fail;
	}

	lp->sdev = sdev;
	if (lebuffer) {
		lp->init_block = (volatile struct lance_init_block *)
			sbus_ioremap(&lebuffer->resource[0], 0,
				     sizeof(struct lance_init_block), "lebuffer");
		if (lp->init_block == NULL) {
			printk(KERN_ERR "SunLance: Cannot map PIO buffer.\n");
			goto fail;
		}
		lp->init_block_dvma = 0;
		lp->pio_buffer = 1;
		lp->init_ring = lance_init_ring_pio;
		lp->rx = lance_rx_pio;
		lp->tx = lance_tx_pio;
	} else {
		lp->init_block = (volatile struct lance_init_block *)
			sbus_alloc_consistent(sdev, sizeof(struct lance_init_block),
					      &lp->init_block_dvma);
		if (lp->init_block == NULL ||
		    lp->init_block_dvma == 0) {
			printk(KERN_ERR "SunLance: Cannot allocate consistent DMA memory.\n");
			goto fail;
		}
		lp->pio_buffer = 0;
		lp->init_ring = lance_init_ring_dvma;
		lp->rx = lance_rx_dvma;
		lp->tx = lance_tx_dvma;
	}
	lp->busmaster_regval = prom_getintdefault(sdev->prom_node,
						  "busmaster-regval",
						  (LE_C3_BSWP | LE_C3_ACON |
						   LE_C3_BCON));

	lp->name = lancestr;
	lp->ledma = ledma;

	lp->burst_sizes = 0;
	if (lp->ledma) {
		char prop[6];
		unsigned int sbmask;
		u32 csr;

		/* Find burst-size property for ledma */
		lp->burst_sizes = prom_getintdefault(ledma->sdev->prom_node,
						     "burst-sizes", 0);

		/* ledma may be capable of fast bursts, but sbus may not. */
		sbmask = prom_getintdefault(ledma->sdev->bus->prom_node,
					    "burst-sizes", DMA_BURSTBITS);
		lp->burst_sizes &= sbmask;

		/* Get the cable-selection property */
		memset(prop, 0, sizeof(prop));
		prom_getstring(ledma->sdev->prom_node, "cable-selection",
			       prop, sizeof(prop));
		if (prop[0] == 0) {
			int topnd, nd;

			printk(KERN_INFO "SunLance: using auto-carrier-detection.\n");

			/* Is this found at /options .attributes in all
			 * Prom versions? XXX
			 */
			topnd = prom_getchild(prom_root_node);

			nd = prom_searchsiblings(topnd, "options");
			if (!nd)
				goto no_link_test;

			if (!prom_node_has_property(nd, "tpe-link-test?"))
				goto no_link_test;

			memset(prop, 0, sizeof(prop));
			prom_getstring(nd, "tpe-link-test?", prop,
				       sizeof(prop));

			if (strcmp(prop, "true")) {
				printk(KERN_NOTICE "SunLance: warning: overriding option "
				       "'tpe-link-test?'\n");
				printk(KERN_NOTICE "SunLance: warning: mail any problems "
				       "to ecd@skynet.be\n");
				auxio_set_lte(AUXIO_LTE_ON);
			}
no_link_test:
			lp->auto_select = 1;
			lp->tpe = 0;
		} else if (!strcmp(prop, "aui")) {
			lp->auto_select = 0;
			lp->tpe = 0;
		} else {
			lp->auto_select = 0;
			lp->tpe = 1;
		}

		lp->dregs = ledma->regs;

		/* Reset ledma */
		csr = sbus_readl(lp->dregs + DMA_CSR);
		sbus_writel(csr | DMA_RST_ENET, lp->dregs + DMA_CSR);
		udelay(200);
		sbus_writel(csr & ~DMA_RST_ENET, lp->dregs + DMA_CSR);
	} else
		lp->dregs = 0;

	/* This should never happen. */
	if ((unsigned long)(lp->init_block->brx_ring) & 0x07) {
		printk(KERN_ERR "SunLance: ERROR: Rx and Tx rings not on even boundary.\n");
		goto fail;
	}

	lp->dev = dev;
	SET_MODULE_OWNER(dev);
	dev->open = &lance_open;
	dev->stop = &lance_close;
	dev->hard_start_xmit = &lance_start_xmit;
	dev->tx_timeout = &lance_tx_timeout;
	dev->watchdog_timeo = 5*HZ;
	dev->get_stats = &lance_get_stats;
	dev->set_multicast_list = &lance_set_multicast;
	dev->ethtool_ops = &sparc_lance_ethtool_ops;

	dev->irq = sdev->irqs[0];

	dev->dma = 0;

	/* We cannot sleep if the chip is busy during a
	 * multicast list update event, because such events
	 * can occur from interrupts (ex. IPv6).  So we
	 * use a timer to try again later when necessary. -DaveM
	 */
	init_timer(&lp->multicast_timer);
	lp->multicast_timer.data = (unsigned long) dev;
	lp->multicast_timer.function = &lance_set_multicast_retry;

	if (register_netdev(dev)) {
		printk(KERN_ERR "SunLance: Cannot register device.\n");
		goto fail;
	}

	lp->next_module = root_lance_dev;
	root_lance_dev = lp;

	printk(KERN_INFO "%s: LANCE ", dev->name);

	for (i = 0; i < 6; i++)
		printk("%2.2x%c", dev->dev_addr[i],
		       i == 5 ? ' ': ':');
	printk("\n");

	return 0;

fail:
	if (lp != NULL)
		lance_free_hwresources(lp);
	free_netdev(dev);
	return -ENODEV;
}

/* On 4m, find the associated dma for the lance chip */
static inline struct sbus_dma *find_ledma(struct sbus_dev *sdev)
{
	struct sbus_dma *p;

	for_each_dvma(p) {
		if (p->sdev == sdev)
			return p;
	}
	return NULL;
}

#ifdef CONFIG_SUN4

#include <asm/sun4paddr.h>
#include <asm/machines.h>

/* Find all the lance cards on the system and initialize them */
static int __init sparc_lance_probe(void)
{
	static struct sbus_dev sdev;
	static int called;

	root_lance_dev = NULL;

	if (called)
		return -ENODEV;
	called++;

	if ((idprom->id_machtype == (SM_SUN4|SM_4_330)) ||
	    (idprom->id_machtype == (SM_SUN4|SM_4_470))) {
		memset(&sdev, 0, sizeof(sdev));
		sdev.reg_addrs[0].phys_addr = sun4_eth_physaddr;
		sdev.irqs[0] = 6;
		return sparc_lance_init(&sdev, 0, 0);
	}
	return -ENODEV;
}

#else /* !CONFIG_SUN4 */

/* Find all the lance cards on the system and initialize them */
static int __init sparc_lance_probe(void)
{
	struct sbus_bus *bus;
	struct sbus_dev *sdev = NULL;
	struct sbus_dma *ledma = NULL;
	static int called;
	int cards = 0, v;

	root_lance_dev = NULL;

	if (called)
		return -ENODEV;
	called++;

	for_each_sbus (bus) {
		for_each_sbusdev (sdev, bus) {
			if (strcmp(sdev->prom_name, "le") == 0) {
				cards++;
				if ((v = sparc_lance_init(sdev, NULL, NULL)))
					return v;
				continue;
			}
			if (strcmp(sdev->prom_name, "ledma") == 0) {
				cards++;
				ledma = find_ledma(sdev);
				if ((v = sparc_lance_init(sdev->child,
							  ledma, NULL)))
					return v;
				continue;
			}
			if (strcmp(sdev->prom_name, "lebuffer") == 0){
				cards++;
				if ((v = sparc_lance_init(sdev->child,
							  NULL, sdev)))
					return v;
				continue;
			}
		} /* for each sbusdev */
	} /* for each sbus */
	if (!cards)
		return -ENODEV;
	return 0;
}
#endif /* !CONFIG_SUN4 */

static void __exit sparc_lance_cleanup(void)
{
	struct lance_private *lp;

	while (root_lance_dev) {
		lp = root_lance_dev->next_module;

		unregister_netdev(root_lance_dev->dev);
		lance_free_hwresources(root_lance_dev);
		free_netdev(root_lance_dev->dev);
		root_lance_dev = lp;
	}
}

module_init(sparc_lance_probe);
module_exit(sparc_lance_cleanup);
MODULE_LICENSE("GPL");
