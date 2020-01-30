/* $Id$
 * vmelance.c: Ethernet driver for VME Lance cards on Baget/MIPS
 *      This code stealed and adopted from linux/drivers/net/atarilance.c
 *      See that for author info
 *
 * Copyright (C) 1998 Gleb Raiko & Vladimir Roganov
 */

/* 
 * Driver code for Baget/Lance taken from atarilance.c, which also
 * works well in case of Besta. Most significant changes made here
 * related with 16BIT-only access to A24 space.
 */

static char *version = "bagetlance.c: v1.1 11/10/98\n";

#include <linux/module.h>

#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/ptrace.h>
#include <linux/errno.h>
#include <linux/malloc.h>
#include <linux/interrupt.h>
#include <linux/init.h>

#include <asm/irq.h>
#include <asm/bitops.h>
#include <asm/io.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>

#include <asm/baget/baget.h>

#define BAGET_LANCE_IRQ  BAGET_IRQ_MASK(0xdf)

/*
 *  Define following if you don't need 16BIT-only access to Lance memory
 *  (Normally BAGET needs it)
 */
#undef NORMAL_MEM_ACCESS 

/* Debug level:
 *  0 = silent, print only serious errors
 *  1 = normal, print error messages
 *  2 = debug, print debug infos
 *  3 = debug, print even more debug infos (packet data)
 */

#define	LANCE_DEBUG	1  

#ifdef LANCE_DEBUG
static int lance_debug = LANCE_DEBUG;
#else
static int lance_debug = 1;
#endif
MODULE_PARM(lance_debug, "i");

/* Print debug messages on probing? */
#undef LANCE_DEBUG_PROBE

#define	DPRINTK(n,a)							\
	do {										\
		if (lance_debug >= n)					\
			printk a;							\
	} while( 0 )

#ifdef LANCE_DEBUG_PROBE
# define PROBE_PRINT(a)	printk a
#else
# define PROBE_PRINT(a)
#endif

/* These define the number of Rx and Tx buffers as log2. (Only powers
 * of two are valid)
 * Much more rx buffers (32) are reserved than tx buffers (8), since receiving
 * is more time critical then sending and packets may have to remain in the
 * board's memory when main memory is low.
 */

/* Baget Lance has 64K on-board memory, so it looks we can't increase
   buffer quantity (40*1.5K is about 64K) */

#define TX_LOG_RING_SIZE			3
#define RX_LOG_RING_SIZE			5

/* These are the derived values */

#define TX_RING_SIZE			(1 << TX_LOG_RING_SIZE)
#define TX_RING_LEN_BITS		(TX_LOG_RING_SIZE << 5)
#define	TX_RING_MOD_MASK		(TX_RING_SIZE - 1)

#define RX_RING_SIZE			(1 << RX_LOG_RING_SIZE)
#define RX_RING_LEN_BITS		(RX_LOG_RING_SIZE << 5)
#define	RX_RING_MOD_MASK		(RX_RING_SIZE - 1)

/* The LANCE Rx and Tx ring descriptors. */
struct lance_rx_head {
	volatile unsigned short	base;		/* Low word of base addr */
#ifdef NORMAL_MEM_ACCESS
       /* Following two fields are joined into one short to guarantee
		  16BIT access to Baget lance registers */
	volatile unsigned char	flag;
	unsigned char			base_hi;	/* High word of base addr (unused) */
#else
/* Following macros are used as replecements to 8BIT fields */
#define GET_FLAG(h)    (((h)->flag_base_hi >> 8) & 0xff)
#define SET_FLAG(h,f)  (h)->flag_base_hi = ((h)->flag_base_hi & 0xff) | \
		                                                (((unsigned)(f)) << 8)
	volatile unsigned short flag_base_hi; 
#endif
	volatile short			buf_length;	/* This length is 2s complement! */
	volatile short			msg_length;	/* This length is "normal". */
};


struct lance_tx_head {
	volatile unsigned short	base;		/* Low word of base addr */
#ifdef NORMAL_MEM_ACCESS 
/* See comments above about 8BIT-access Baget A24-space problems */
	volatile unsigned char	flag;
	unsigned char			base_hi;	/* High word of base addr (unused) */
#else
	volatile unsigned short  flag_base_hi;
#endif
	volatile short			length;		/* Length is 2s complement! */
	volatile short			misc;
};

struct ringdesc {
	volatile unsigned short	adr_lo;		/* Low 16 bits of address */
#ifdef NORMAL_MEM_ACCESS 
/* See comments above about 8BIT-access Bage A24-space problems */
	unsigned char	len;		/* Length bits */
	unsigned char	adr_hi;		/* High 8 bits of address (unused) */
#else
	volatile unsigned short  len_adr_hi;
#endif
};

/* The LANCE initialization block, described in databook. */
struct lance_init_block {
	unsigned short	mode;		/* Pre-set mode */
	unsigned char	hwaddr[6];	/* Physical ethernet address */
	unsigned		filter[2];	/* Multicast filter (unused). */
	/* Receive and transmit ring base, along with length bits. */
	struct ringdesc	rx_ring;
	struct ringdesc	tx_ring;
};

/* The whole layout of the Lance shared memory */
struct lance_memory {
	struct lance_init_block	init;
	struct lance_tx_head	tx_head[TX_RING_SIZE];
	struct lance_rx_head	rx_head[RX_RING_SIZE];
	char					packet_area[0];	/* packet data follow after the
											 * init block and the ring
											 * descriptors and are located
											 * at runtime */
};

/* RieblCard specifics:
 * The original TOS driver for these cards reserves the area from offset
 * 0xee70 to 0xeebb for storing configuration data. Of interest to us is the
 * Ethernet address there, and the magic for verifying the data's validity.
 * The reserved area isn't touch by packet buffers. Furthermore, offset 0xfffe
 * is reserved for the interrupt vector number.
 */
#define	RIEBL_RSVD_START	0xee70
#define	RIEBL_RSVD_END		0xeec0
#define RIEBL_MAGIC			0x09051990
#define RIEBL_MAGIC_ADDR	((unsigned long *)(((char *)MEM) + 0xee8a))
#define RIEBL_HWADDR_ADDR	((unsigned char *)(((char *)MEM) + 0xee8e))
#define RIEBL_IVEC_ADDR		((unsigned short *)(((char *)MEM) + 0xfffe))

/* This is a default address for the old RieblCards without a battery
 * that have no ethernet address at boot time. 00:00:36:04 is the
 * prefix for Riebl cards, the 00:00 at the end is arbitrary.
 */

static unsigned char OldRieblDefHwaddr[6] = {
	0x00, 0x00, 0x36, 0x04, 0x00, 0x00
};

/* I/O registers of the Lance chip */

struct lance_ioreg {
/* base+0x0 */	volatile unsigned short	data;
/* base+0x2 */	volatile unsigned short	addr;
				unsigned char			_dummy1[3];
/* base+0x7 */	volatile unsigned char	ivec;
				unsigned char			_dummy2[5];
/* base+0xd */	volatile unsigned char	eeprom;
				unsigned char			_dummy3;
/* base+0xf */	volatile unsigned char	mem;
};

/* Types of boards this driver supports */

enum lance_type {
	OLD_RIEBL,		/* old Riebl card without battery */
	NEW_RIEBL,		/* new Riebl card with battery */
	PAM_CARD		/* PAM card with EEPROM */
};

static char *lance_names[] = {
	"Riebl-Card (without battery)",
	"Riebl-Card (with battery)",
	"PAM intern card"
};

/* The driver's private device structure */

struct lance_private {
	enum lance_type		cardtype;
	struct lance_ioreg	*iobase;
	struct lance_memory	*mem;
	int					cur_rx, cur_tx;	/* The next free ring entry */
	int					dirty_tx;		/* Ring entries to be freed. */
						/* copy function */
	void				*(*memcpy_f)( void *, const void *, size_t );
	struct net_device_stats stats;
/* These two must be longs for set_bit() */
	long				tx_full;
	long				lock;
};

/* I/O register access macros */

#define	MEM		lp->mem
#define	DREG	IO->data
#define	AREG	IO->addr
#define	REGA(a)	( AREG = (a), DREG )

/* Definitions for packet buffer access: */
#define PKT_BUF_SZ		1544
/* Get the address of a packet buffer corresponding to a given buffer head */
#define	PKTBUF_ADDR(head)	(((unsigned char *)(MEM)) + (head)->base)

/* Possible memory/IO addresses for probing */

struct lance_addr {
	unsigned long	memaddr;
	unsigned long	ioaddr;
	int				slow_flag;
} lance_addr_list[] = {
	{ BAGET_LANCE_MEM_BASE, BAGET_LANCE_IO_BASE, 1 }	/* Baget Lance */
};

#define	N_LANCE_ADDR	(sizeof(lance_addr_list)/sizeof(*lance_addr_list))


#define LANCE_HI_BASE (0xff & (BAGET_LANCE_MEM_BASE >> 16))

/* Definitions for the Lance */

/* tx_head flags */
#define TMD1_ENP		0x01	/* end of packet */
#define TMD1_STP		0x02	/* start of packet */
#define TMD1_DEF		0x04	/* deferred */
#define TMD1_ONE		0x08	/* one retry needed */
#define TMD1_MORE		0x10	/* more than one retry needed */
#define TMD1_ERR		0x40	/* error summary */
#define TMD1_OWN 		0x80	/* ownership (set: chip owns) */

#define TMD1_OWN_CHIP	TMD1_OWN
#define TMD1_OWN_HOST	0

/* tx_head misc field */
#define TMD3_TDR		0x03FF	/* Time Domain Reflectometry counter */
#define TMD3_RTRY		0x0400	/* failed after 16 retries */
#define TMD3_LCAR		0x0800	/* carrier lost */
#define TMD3_LCOL		0x1000	/* late collision */
#define TMD3_UFLO		0x4000	/* underflow (late memory) */
#define TMD3_BUFF		0x8000	/* buffering error (no ENP) */

/* rx_head flags */
#define RMD1_ENP		0x01	/* end of packet */
#define RMD1_STP		0x02	/* start of packet */
#define RMD1_BUFF		0x04	/* buffer error */
#define RMD1_CRC		0x08	/* CRC error */
#define RMD1_OFLO		0x10	/* overflow */
#define RMD1_FRAM		0x20	/* framing error */
#define RMD1_ERR		0x40	/* error summary */
#define RMD1_OWN 		0x80	/* ownership (set: ship owns) */

#define RMD1_OWN_CHIP	RMD1_OWN
#define RMD1_OWN_HOST	0

/* register names */
#define CSR0	0		/* mode/status */
#define CSR1	1		/* init block addr (low) */
#define CSR2	2		/* init block addr (high) */
#define CSR3	3		/* misc */
#define CSR8	8	  	/* address filter */
#define CSR15	15		/* promiscuous mode */

/* CSR0 */
/* (R=readable, W=writeable, S=set on write, C=clear on write) */
#define CSR0_INIT	0x0001		/* initialize (RS) */
#define CSR0_STRT	0x0002		/* start (RS) */
#define CSR0_STOP	0x0004		/* stop (RS) */
#define CSR0_TDMD	0x0008		/* transmit demand (RS) */
#define CSR0_TXON	0x0010		/* transmitter on (R) */
#define CSR0_RXON	0x0020		/* receiver on (R) */
#define CSR0_INEA	0x0040		/* interrupt enable (RW) */
#define CSR0_INTR	0x0080		/* interrupt active (R) */
#define CSR0_IDON	0x0100		/* initialization done (RC) */
#define CSR0_TINT	0x0200		/* transmitter interrupt (RC) */
#define CSR0_RINT	0x0400		/* receiver interrupt (RC) */
#define CSR0_MERR	0x0800		/* memory error (RC) */
#define CSR0_MISS	0x1000		/* missed frame (RC) */
#define CSR0_CERR	0x2000		/* carrier error (no heartbeat :-) (RC) */
#define CSR0_BABL	0x4000		/* babble: tx-ed too many bits (RC) */
#define CSR0_ERR	0x8000		/* error (RC) */

/* CSR3 */
#define CSR3_BCON	0x0001		/* byte control */
#define CSR3_ACON	0 // fixme: 0x0002		/* ALE control */
#define CSR3_BSWP	0x0004		/* byte swap (1=big endian) */



/***************************** Prototypes *****************************/

static int addr_accessible( volatile void *regp, int wordflag, int
                            writeflag );
static int lance_probe1( struct net_device *dev, struct lance_addr *init_rec );
static int lance_open( struct net_device *dev );
static void lance_init_ring( struct net_device *dev );
static int lance_start_xmit( struct sk_buff *skb, struct net_device *dev );
static void lance_interrupt( int irq, void *dev_id, struct pt_regs *fp );
static int lance_rx( struct net_device *dev );
static int lance_close( struct net_device *dev );
static struct net_device_stats *lance_get_stats( struct net_device *dev );
static void set_multicast_list( struct net_device *dev );
static int lance_set_mac_address( struct net_device *dev, void *addr );

/************************* End of Prototypes **************************/

/* Network traffic statistic (bytes) */

int lance_stat = 0;

static void update_lance_stat (int len) {
		lance_stat += len;
}

/* 
   This function is used to access Baget/Lance memory to avoid 
   8/32BIT access to VAC A24 space 
   ALL memcpy calls was chenged to this function to avoid dbe problems
   Don't confuse with function name -- it stays from original code
*/

void *slow_memcpy( void *dst, const void *src, size_t len )

{	
	unsigned long to     = (unsigned long)dst;
	unsigned long from   = (unsigned long)src;
	unsigned long to_end = to +len;
	
	/* Unaligned flags */

	int odd_from   = from   & 1;
	int odd_to     = to     & 1;
	int odd_to_end = to_end & 1;

	/* Align for 16BIT-access first */

	register unsigned short *from_a   = (unsigned short*) (from   & ~1);
	register unsigned short *to_a     = (unsigned short*) (to     & ~1); 
	register unsigned short *to_end_a = (unsigned short*) (to_end & ~1);

	/* Caching values -- not in loop invariant */

	register unsigned short from_v; 
	register unsigned short to_v;

	/* Invariant is: from_a and to_a are pointers before or exactly to
	   currently copying byte */

	if (odd_to) { 
			/* First byte unaligned case */
			from_v = *from_a;
			to_v   = *to_a;

			to_v &= ~0xff;
			to_v |=  0xff & (from_v >> (odd_from ? 0 : 8));
			*to_a++ = to_v;

			if (odd_from) from_a++;
	}
    if (odd_from == odd_to) {
			/* Same parity */
			while (to_a + 7 < to_end_a) {
					unsigned long dummy1, dummy2;
					unsigned long reg1, reg2, reg3, reg4;

					__asm__ __volatile__(
					".set\tnoreorder\n\t"
					".set\tnoat\n\t"
					"lh\t%2,0(%1)\n\t"
					"nop\n\t"
					 "lh\t%3,2(%1)\n\t"
					"sh\t%2,0(%0)\n\t"
					   "lh\t%4,4(%1)\n\t"
					 "sh\t%3,2(%0)\n\t"
					    "lh\t%5,6(%1)\n\t"
					   "sh\t%4,4(%0)\n\t"
					"lh\t%2,8(%1)\n\t"
					    "sh\t%5,6(%0)\n\t"
					 "lh\t%3,10(%1)\n\t"
					"sh\t%2,8(%0)\n\t"
					  "lh\t%4,12(%1)\n\t"
					 "sh\t%3,10(%0)\n\t"
					    "lh\t%5,14(%1)\n\t"
					  "sh\t%4,12(%0)\n\t"
					 "nop\n\t"
					    "sh\t%5,14(%0)\n\t"
					".set\tat\n\t"
					".set\treorder"
					:"=r" (dummy1), "=r" (dummy2),
					"=&r" (reg1), "=&r" (reg2), "=&r" (reg3), "=&r" (reg4)
					:"0" (to_a), "1" (from_a)
					:"memory");

					to_a   += 8;
					from_a += 8;

			}
			while (to_a < to_end_a) {
					*to_a++ = *from_a++;
			}
	} else {
			/* Different parity */
			from_v = *from_a;
			while (to_a < to_end_a) {
					unsigned short from_v_next;
					from_v_next = *++from_a;
					*to_a++ = ((from_v & 0xff)<<8) | ((from_v_next>>8) & 0xff);
					from_v = from_v_next; 
			}

	}
	if (odd_to_end) {
			/* Last byte unaligned case */
			to_v = *to_a;
			from_v = *from_a;

			to_v &= ~0xff00;
			if (odd_from == odd_to) {
					to_v |= from_v & 0xff00;
			} else {
					to_v |= (from_v<<8) & 0xff00;
			}

			*to_a = to_v;
	}

	update_lance_stat( len );

	return( dst );
}


int __init bagetlance_probe( struct net_device *dev )

{	int i;
	static int found = 0;

	SET_MODULE_OWNER(dev);

	if (found)
		/* Assume there's only one board possible... That seems true, since
		 * the Riebl/PAM board's address cannot be changed. */
		return( -ENODEV );

	for( i = 0; i < N_LANCE_ADDR; ++i ) {
		if (lance_probe1( dev, &lance_addr_list[i] )) {
			found = 1;
			return( 0 );
		}
	}

	return( -ENODEV );
}



/* Derived from hwreg_present() in vme/config.c: */

static int __init addr_accessible( volatile void *regp, 
				   int wordflag, 
				   int writeflag )
{	
		/* We have a fine function to do it */
		extern int try_read(unsigned long, int);
		return try_read((unsigned long)regp, sizeof(short)) != -1;   
}



/* Original atari driver uses it */
#define IRQ_TYPE_PRIO SA_INTERRUPT
#define IRQ_SOURCE_TO_VECTOR(x) (x)

static int __init lance_probe1( struct net_device *dev,
				struct lance_addr *init_rec )

{	volatile unsigned short *memaddr =
		(volatile unsigned short *)init_rec->memaddr;
	volatile unsigned short *ioaddr =
		(volatile unsigned short *)init_rec->ioaddr;
	struct lance_private	*lp;
	struct lance_ioreg		*IO;
	int 					i;
	static int 				did_version = 0;
	unsigned short			save1, save2;

	PROBE_PRINT(( "Probing for Lance card at mem %#lx io %#lx\n",
				  (long)memaddr, (long)ioaddr ));

	/* Test whether memory readable and writable */
	PROBE_PRINT(( "lance_probe1: testing memory to be accessible\n" ));
	if (!addr_accessible( memaddr, 1, 1 )) goto probe_fail;

	if ((unsigned long)memaddr >= KSEG2) {
			extern int kseg2_alloc_io (unsigned long addr, unsigned long size);
			if (kseg2_alloc_io((unsigned long)memaddr, BAGET_LANCE_MEM_SIZE)) {
					printk("bagetlance: unable map lance memory\n");
					goto probe_fail;
			}
	}

	/* Written values should come back... */
	PROBE_PRINT(( "lance_probe1: testing memory to be writable (1)\n" ));
	save1 = *memaddr;
	*memaddr = 0x0001;
	if (*memaddr != 0x0001) goto probe_fail;
	PROBE_PRINT(( "lance_probe1: testing memory to be writable (2)\n" ));
	*memaddr = 0x0000;
	if (*memaddr != 0x0000) goto probe_fail;
	*memaddr = save1;

	/* First port should be readable and writable */
	PROBE_PRINT(( "lance_probe1: testing ioport to be accessible\n" ));
	if (!addr_accessible( ioaddr, 1, 1 )) goto probe_fail;

	/* and written values should be readable */
	PROBE_PRINT(( "lance_probe1: testing ioport to be writeable\n" ));
	save2 = ioaddr[1];
	ioaddr[1] = 0x0001;
	if (ioaddr[1] != 0x0001) goto probe_fail;

	/* The CSR0_INIT bit should not be readable */
	PROBE_PRINT(( "lance_probe1: testing CSR0 register function (1)\n" ));
	save1 = ioaddr[0];
	ioaddr[1] = CSR0;
	ioaddr[0] = CSR0_INIT | CSR0_STOP;
	if (ioaddr[0] != CSR0_STOP) {
		ioaddr[0] = save1;
		ioaddr[1] = save2;
		goto probe_fail;
	}
	PROBE_PRINT(( "lance_probe1: testing CSR0 register function (2)\n" ));
	ioaddr[0] = CSR0_STOP;
	if (ioaddr[0] != CSR0_STOP) {
		ioaddr[0] = save1;
		ioaddr[1] = save2;
		goto probe_fail;
	}

	/* Now ok... */
	PROBE_PRINT(( "lance_probe1: Lance card detected\n" ));
	goto probe_ok;

  probe_fail:
	return( 0 );

  probe_ok:
	init_etherdev( dev, sizeof(struct lance_private) );
	if (!dev->priv)
		dev->priv = kmalloc( sizeof(struct lance_private), GFP_KERNEL );
	lp = (struct lance_private *)dev->priv;
	MEM = (struct lance_memory *)memaddr;
	IO = lp->iobase = (struct lance_ioreg *)ioaddr;
	dev->base_addr = (unsigned long)ioaddr; /* informational only */
	lp->memcpy_f = init_rec->slow_flag ? slow_memcpy : memcpy;

	REGA( CSR0 ) = CSR0_STOP;

	/* Now test for type: If the eeprom I/O port is readable, it is a
	 * PAM card */
	if (addr_accessible( &(IO->eeprom), 0, 0 )) {
		/* Switch back to Ram */
		i = IO->mem;
		lp->cardtype = PAM_CARD;
	}
#ifdef NORMAL_MEM_ACCESS
	else if (*RIEBL_MAGIC_ADDR == RIEBL_MAGIC) {
#else
	else if (({
			unsigned short *a = (unsigned short*)RIEBL_MAGIC_ADDR;
		    (((int)a[0]) << 16) + ((int)a[1]) == RIEBL_MAGIC;
	})) {
#endif
		lp->cardtype = NEW_RIEBL;
	}
	else
		lp->cardtype = OLD_RIEBL;

	if (lp->cardtype == PAM_CARD ||
		memaddr == (unsigned short *)0xffe00000) {
		/* PAMs card and Riebl on ST use level 5 autovector */
		request_irq(BAGET_LANCE_IRQ, lance_interrupt, IRQ_TYPE_PRIO,
		            "PAM/Riebl-ST Ethernet", dev);
		dev->irq = (unsigned short)BAGET_LANCE_IRQ;
	}
	else {
		/* For VME-RieblCards, request a free VME int;
		 * (This must be unsigned long, since dev->irq is short and the
		 * IRQ_MACHSPEC bit would be cut off...)
		 */
		unsigned long irq = BAGET_LANCE_IRQ; 
		if (!irq) {
			printk( "Lance: request for VME interrupt failed\n" );
			return( 0 );
		}
		request_irq(irq, lance_interrupt, IRQ_TYPE_PRIO,
		            "Riebl-VME Ethernet", dev);
		dev->irq = irq;
	}

	printk("%s: %s at io %#lx, mem %#lx, irq %d%s, hwaddr ",
		   dev->name, lance_names[lp->cardtype],
		   (unsigned long)ioaddr,
		   (unsigned long)memaddr,
		   dev->irq,
		   init_rec->slow_flag ? " (slow memcpy)" : "" );

	/* Get the ethernet address */
	switch( lp->cardtype ) {
	  case OLD_RIEBL:
		/* No ethernet address! (Set some default address) */
		slow_memcpy( dev->dev_addr, OldRieblDefHwaddr, 6 );
		break;
	  case NEW_RIEBL:
		lp->memcpy_f( dev->dev_addr, RIEBL_HWADDR_ADDR, 6 );
		break;
	  case PAM_CARD:
		i = IO->eeprom;
		for( i = 0; i < 6; ++i )
			dev->dev_addr[i] =
				((((unsigned short *)MEM)[i*2] & 0x0f) << 4) |
				((((unsigned short *)MEM)[i*2+1] & 0x0f));
		i = IO->mem;
		break;
	}
	for( i = 0; i < 6; ++i )
		printk( "%02x%s", dev->dev_addr[i], (i < 5) ? ":" : "\n" );
	if (lp->cardtype == OLD_RIEBL) {
		printk( "%s: Warning: This is a default ethernet address!\n",
				dev->name );
		printk( "      Use \"ifconfig hw ether ...\" to set the address.\n" );
	}

	MEM->init.mode = 0x0000;		/* Disable Rx and Tx. */

	{
			unsigned char hwaddr[6];
			for( i = 0; i < 6; i++ ) 
					hwaddr[i] = dev->dev_addr[i^1]; /* <- 16 bit swap! */
			slow_memcpy(MEM->init.hwaddr, hwaddr, sizeof(hwaddr));
	}

	MEM->init.filter[0] = 0x00000000;
	MEM->init.filter[1] = 0x00000000;
	MEM->init.rx_ring.adr_lo = offsetof( struct lance_memory, rx_head );

#ifdef NORMAL_MEM_ACCESS
	MEM->init.rx_ring.adr_hi = LANCE_HI_BASE; 
	MEM->init.rx_ring.len    = RX_RING_LEN_BITS;
#else
	MEM->init.rx_ring.len_adr_hi = 
			((unsigned)RX_RING_LEN_BITS << 8) | LANCE_HI_BASE;
#endif


	MEM->init.tx_ring.adr_lo = offsetof( struct lance_memory, tx_head );

#ifdef NORMAL_MEM_ACCESS
	MEM->init.tx_ring.adr_hi = LANCE_HI_BASE; 
	MEM->init.tx_ring.len    = TX_RING_LEN_BITS;
#else
	MEM->init.tx_ring.len_adr_hi = 
			((unsigned)TX_RING_LEN_BITS<<8) | LANCE_HI_BASE;
#endif

	if (lp->cardtype == PAM_CARD)
		IO->ivec = IRQ_SOURCE_TO_VECTOR(dev->irq);
	else
		*RIEBL_IVEC_ADDR = IRQ_SOURCE_TO_VECTOR(dev->irq);

	if (did_version++ == 0)
		DPRINTK( 1, ( version ));

	/* The LANCE-specific entries in the device structure. */
	dev->open = &lance_open;
	dev->hard_start_xmit = &lance_start_xmit;
	dev->stop = &lance_close;
	dev->get_stats = &lance_get_stats;
	dev->set_multicast_list = &set_multicast_list;
	dev->set_mac_address = &lance_set_mac_address;
	dev->start = 0;

	memset( &lp->stats, 0, sizeof(lp->stats) );

	return( 1 );
}


static int lance_open( struct net_device *dev )

{	struct lance_private *lp = (struct lance_private *)dev->priv;
	struct lance_ioreg	 *IO = lp->iobase;
	int i;

	DPRINTK( 2, ( "%s: lance_open()\n", dev->name ));

	lance_init_ring(dev);
	/* Re-initialize the LANCE, and start it when done. */

	REGA( CSR3 ) = CSR3_BSWP | (lp->cardtype == PAM_CARD ? CSR3_ACON : 0);
	REGA( CSR2 ) = 0;
	REGA( CSR1 ) = 0;
	REGA( CSR0 ) = CSR0_INIT;
	/* From now on, AREG is kept to point to CSR0 */

	i = 1000000;
	while (--i > 0)
		if (DREG & CSR0_IDON)
			break;
	if (i < 0 || (DREG & CSR0_ERR)) {
		DPRINTK( 2, ( "lance_open(): opening %s failed, i=%d, csr0=%04x\n",
					  dev->name, i, DREG ));
		DREG = CSR0_STOP;
		return( -EIO );
	}
	DREG = CSR0_IDON;
	DREG = CSR0_STRT;
	DREG = CSR0_INEA;

	dev->tbusy = 0;
	dev->interrupt = 0;
	dev->start = 1;

	DPRINTK( 2, ( "%s: LANCE is open, csr0 %04x\n", dev->name, DREG ));
	return( 0 );
}


/* Initialize the LANCE Rx and Tx rings. */

static void lance_init_ring( struct net_device *dev )

{	struct lance_private *lp = (struct lance_private *)dev->priv;
	int i;
	unsigned offset;

	lp->lock = 0;
	lp->tx_full = 0;
	lp->cur_rx = lp->cur_tx = 0;
	lp->dirty_tx = 0;

	offset = offsetof( struct lance_memory, packet_area );

/* If the packet buffer at offset 'o' would conflict with the reserved area
 * of RieblCards, advance it */
#define	CHECK_OFFSET(o)														 \
	do {																	 \
		if (lp->cardtype == OLD_RIEBL || lp->cardtype == NEW_RIEBL) {		 \
			if (((o) < RIEBL_RSVD_START) ? (o)+PKT_BUF_SZ > RIEBL_RSVD_START \
										 : (o) < RIEBL_RSVD_END)			 \
				(o) = RIEBL_RSVD_END;										 \
		}																	 \
	} while(0)

	for( i = 0; i < TX_RING_SIZE; i++ ) {
		CHECK_OFFSET(offset);
		MEM->tx_head[i].base = offset;
#ifdef NORMAL_MEM_ACCESS
		MEM->tx_head[i].flag = TMD1_OWN_HOST;
 		MEM->tx_head[i].base_hi = LANCE_HI_BASE;
#else
		MEM->tx_head[i].flag_base_hi = 
				(TMD1_OWN_HOST<<8) | LANCE_HI_BASE;
#endif
		MEM->tx_head[i].length = 0;
		MEM->tx_head[i].misc = 0;
		offset += PKT_BUF_SZ;
	}

	for( i = 0; i < RX_RING_SIZE; i++ ) {
		CHECK_OFFSET(offset);
		MEM->rx_head[i].base = offset;
#ifdef NORMAL_MEM_ACCESS
		MEM->rx_head[i].flag = TMD1_OWN_CHIP;
		MEM->rx_head[i].base_hi = LANCE_HI_BASE; 
#else
		MEM->rx_head[i].flag_base_hi = 
				(TMD1_OWN_CHIP<<8) | LANCE_HI_BASE;
#endif
		MEM->rx_head[i].buf_length = -PKT_BUF_SZ;
		MEM->rx_head[i].msg_length = 0;
		offset += PKT_BUF_SZ;
	}
}


static int lance_start_xmit( struct sk_buff *skb, struct net_device *dev )

{	struct lance_private *lp = (struct lance_private *)dev->priv;
	struct lance_ioreg	 *IO = lp->iobase;
	int entry, len;
	struct lance_tx_head *head;
	unsigned long flags;

	/* Transmitter timeout, serious problems. */
	if (dev->tbusy) {
		int tickssofar = jiffies - dev->trans_start;
		if (tickssofar < 20)
			return( 1 );
		AREG = CSR0;
		DPRINTK( 1, ( "%s: transmit timed out, status %04x, resetting.\n",
					  dev->name, DREG ));
		DREG = CSR0_STOP;
		/*
		 * Always set BSWP after a STOP as STOP puts it back into
		 * little endian mode.
		 */
		REGA( CSR3 ) = CSR3_BSWP | (lp->cardtype == PAM_CARD ? CSR3_ACON : 0);
		lp->stats.tx_errors++;
#ifndef final_version
		{	int i;
			DPRINTK( 2, ( "Ring data: dirty_tx %d cur_tx %d%s cur_rx %d\n",
						  lp->dirty_tx, lp->cur_tx,
						  lp->tx_full ? " (full)" : "",
						  lp->cur_rx ));
			for( i = 0 ; i < RX_RING_SIZE; i++ )
				DPRINTK( 2, ( "rx #%d: base=%04x blen=%04x mlen=%04x\n",
							  i, MEM->rx_head[i].base,
							  -MEM->rx_head[i].buf_length,
							  MEM->rx_head[i].msg_length ));
			for( i = 0 ; i < TX_RING_SIZE; i++ )
				DPRINTK( 2, ( "tx #%d: base=%04x len=%04x misc=%04x\n",
							  i, MEM->tx_head[i].base,
							  -MEM->tx_head[i].length,
							  MEM->tx_head[i].misc ));
		}
#endif
		lance_init_ring(dev);
		REGA( CSR0 ) = CSR0_INEA | CSR0_INIT | CSR0_STRT;

		dev->tbusy = 0;
		dev->trans_start = jiffies;

		return( 0 );
	}

	DPRINTK( 2, ( "%s: lance_start_xmit() called, csr0 %4.4x.\n",
				  dev->name, DREG ));

	/* Block a timer-based transmit from overlapping.  This could better be
	   done with atomic_swap(1, dev->tbusy), but set_bit() works as well. */
	if (test_and_set_bit( 0, (void*)&dev->tbusy ) != 0) {
		DPRINTK( 0, ( "%s: Transmitter access conflict.\n", dev->name ));
		return 1;
	}

	if (test_and_set_bit( 0, (void*)&lp->lock ) != 0) {
		DPRINTK( 0, ( "%s: tx queue lock!.\n", dev->name ));
		/* don't clear dev->tbusy flag. */
		return 1;
	}

	/* Fill in a Tx ring entry */
	if (lance_debug >= 3) {
		u_char *p;
		int i;
		printk( "%s: TX pkt type 0x%04x from ", dev->name,
				((u_short *)skb->data)[6]);
		for( p = &((u_char *)skb->data)[6], i = 0; i < 6; i++ )
			printk("%02x%s", *p++, i != 5 ? ":" : "" );
		printk(" to ");
		for( p = (u_char *)skb->data, i = 0; i < 6; i++ )
			printk("%02x%s", *p++, i != 5 ? ":" : "" );
		printk(" data at 0x%08x len %d\n", (int)skb->data,
			   (int)skb->len );
	}

	/* We're not prepared for the int until the last flags are set/reset. And
	 * the int may happen already after setting the OWN_CHIP... */
	save_flags(flags);
	cli();

	/* Mask to ring buffer boundary. */
	entry = lp->cur_tx & TX_RING_MOD_MASK;
	head  = &(MEM->tx_head[entry]);

	/* Caution: the write order is important here, set the "ownership" bits
	 * last.
	 */

	/* The old LANCE chips doesn't automatically pad buffers to min. size. */
	len = (ETH_ZLEN < skb->len) ? skb->len : ETH_ZLEN;
	/* PAM-Card has a bug: Can only send packets with even number of bytes! */
	if (lp->cardtype == PAM_CARD && (len & 1))
		++len;

	head->length = -len;
	head->misc = 0;
	lp->memcpy_f( PKTBUF_ADDR(head), (void *)skb->data, skb->len );
#ifdef NORMAL_MEM_ACCESS
	head->flag = TMD1_OWN_CHIP | TMD1_ENP | TMD1_STP;
#else
    SET_FLAG(head,(TMD1_OWN_CHIP | TMD1_ENP | TMD1_STP));
#endif
	dev_kfree_skb( skb );
	lp->cur_tx++;
	lp->stats.tx_bytes += skb->len;
	while( lp->cur_tx >= TX_RING_SIZE && lp->dirty_tx >= TX_RING_SIZE ) {
		lp->cur_tx -= TX_RING_SIZE;
		lp->dirty_tx -= TX_RING_SIZE;
	}

	/* Trigger an immediate send poll. */
	DREG = CSR0_INEA | CSR0_TDMD;
	dev->trans_start = jiffies;

	lp->lock = 0;
#ifdef NORMAL_MEM_ACCESS
	if ((MEM->tx_head[(entry+1) & TX_RING_MOD_MASK].flag & TMD1_OWN) ==
#else
	if ((GET_FLAG(&MEM->tx_head[(entry+1) & TX_RING_MOD_MASK]) & TMD1_OWN) ==
#endif
		TMD1_OWN_HOST)
		dev->tbusy = 0;
	else
		lp->tx_full = 1;
	restore_flags(flags);

	return 0;
}

/* The LANCE interrupt handler. */

static void lance_interrupt( int irq, void *dev_id, struct pt_regs *fp)
{
	struct net_device *dev = dev_id;
	struct lance_private *lp;
	struct lance_ioreg	 *IO;
	int csr0, boguscnt = 10;

	if (dev == NULL) {
		DPRINTK( 1, ( "lance_interrupt(): interrupt for unknown device.\n" ));
		return;
	}

	lp = (struct lance_private *)dev->priv;
	IO = lp->iobase;
	AREG = CSR0;

	if (dev->interrupt) {
			DPRINTK( 1, ( "Re-entering CAUSE=%08x STATUS=%08x\n",  
						  read_32bit_cp0_register(CP0_CAUSE),  
						  read_32bit_cp0_register(CP0_STATUS) ));
			panic("lance: interrupt handler reentered !");
	}

	dev->interrupt = 1;

	while( ((csr0 = DREG) & (CSR0_ERR | CSR0_TINT | CSR0_RINT)) &&
		   --boguscnt >= 0) {
		/* Acknowledge all of the current interrupt sources ASAP. */
		DREG = csr0 & ~(CSR0_INIT | CSR0_STRT | CSR0_STOP |
									CSR0_TDMD | CSR0_INEA);

		DPRINTK( 2, ( "%s: interrupt  csr0=%04x new csr=%04x.\n",
					  dev->name, csr0, DREG ));

		if (csr0 & CSR0_RINT)			/* Rx interrupt */
			lance_rx( dev );

		if (csr0 & CSR0_TINT) {			/* Tx-done interrupt */
			int dirty_tx = lp->dirty_tx;

			while( dirty_tx < lp->cur_tx) {
				int entry = dirty_tx & TX_RING_MOD_MASK;
#ifdef NORMAL_MEM_ACCESS
				int status = MEM->tx_head[entry].flag;
#else
				int status = GET_FLAG(&MEM->tx_head[entry]);
#endif
				if (status & TMD1_OWN_CHIP)
					break;			/* It still hasn't been Txed */

#ifdef NORMAL_MEM_ACCESS
				MEM->tx_head[entry].flag = 0;
#else
				SET_FLAG(&MEM->tx_head[entry],0);
#endif

				if (status & TMD1_ERR) {
					/* There was an major error, log it. */
					int err_status = MEM->tx_head[entry].misc;
					lp->stats.tx_errors++;
					if (err_status & TMD3_RTRY) lp->stats.tx_aborted_errors++;
					if (err_status & TMD3_LCAR) lp->stats.tx_carrier_errors++;
					if (err_status & TMD3_LCOL) lp->stats.tx_window_errors++;
					if (err_status & TMD3_UFLO) {
						/* Ackk!  On FIFO errors the Tx unit is turned off! */
						lp->stats.tx_fifo_errors++;
						/* Remove this verbosity later! */
						DPRINTK( 1, ( "%s: Tx FIFO error! Status %04x\n",
									  dev->name, csr0 ));
						/* Restart the chip. */
						DREG = CSR0_STRT;
					}
				} else {
					if (status & (TMD1_MORE | TMD1_ONE | TMD1_DEF))
						lp->stats.collisions++;
					lp->stats.tx_packets++;
				}
				dirty_tx++;
			}

#ifndef final_version
			if (lp->cur_tx - dirty_tx >= TX_RING_SIZE) {
				DPRINTK( 0, ( "out-of-sync dirty pointer,"
							  " %d vs. %d, full=%d.\n",
							  dirty_tx, lp->cur_tx, lp->tx_full ));
				dirty_tx += TX_RING_SIZE;
			}
#endif

			if (lp->tx_full && dev->tbusy
				&& dirty_tx > lp->cur_tx - TX_RING_SIZE + 2) {
				/* The ring is no longer full, clear tbusy. */
				lp->tx_full = 0;
				dev->tbusy = 0;
				mark_bh( NET_BH );
			}

			lp->dirty_tx = dirty_tx;
		}

		/* Log misc errors. */
		if (csr0 & CSR0_BABL) lp->stats.tx_errors++; /* Tx babble. */
		if (csr0 & CSR0_MISS) lp->stats.rx_errors++; /* Missed a Rx frame. */
		if (csr0 & CSR0_MERR) {
			DPRINTK( 1, ( "%s: Bus master arbitration failure (?!?), "
						  "status %04x.\n", dev->name, csr0 ));
			/* Restart the chip. */
			DREG = CSR0_STRT;
		}
	}

    /* Clear any other interrupt, and set interrupt enable. */
	DREG = CSR0_BABL | CSR0_CERR | CSR0_MISS | CSR0_MERR |
		   CSR0_IDON | CSR0_INEA;

	DPRINTK( 2, ( "%s: exiting interrupt, csr0=%#04x.\n",
				  dev->name, DREG ));
	dev->interrupt = 0;
	return;
}


static int lance_rx( struct net_device *dev )

{	struct lance_private *lp = (struct lance_private *)dev->priv;
	int entry = lp->cur_rx & RX_RING_MOD_MASK;
	int i;

#ifdef NORMAL_MEM_ACCESS
	DPRINTK( 2, ( "%s: rx int, flag=%04x\n", dev->name,
				  MEM->rx_head[entry].flag ));
#else
	DPRINTK( 2, ( "%s: rx int, flag=%04x\n", dev->name,
				  GET_FLAG(&MEM->rx_head[entry]) ));
#endif

	/* If we own the next entry, it's a new packet. Send it up. */
#ifdef NORMAL_MEM_ACCESS
	while( (MEM->rx_head[entry].flag & RMD1_OWN) == RMD1_OWN_HOST ) {
#else
	while( (GET_FLAG(&MEM->rx_head[entry]) & RMD1_OWN) == RMD1_OWN_HOST ) {
#endif
		struct lance_rx_head *head = &(MEM->rx_head[entry]);
#ifdef NORMAL_MEM_ACCESS
		int status = head->flag;
#else
		int status = GET_FLAG(head);
#endif

		if (status != (RMD1_ENP|RMD1_STP)) {		/* There was an error. */
			/* There is a tricky error noted by John Murphy,
			   <murf@perftech.com> to Russ Nelson: Even with full-sized
			   buffers it's possible for a jabber packet to use two
			   buffers, with only the last correctly noting the error. */
			if (status & RMD1_ENP)	/* Only count a general error at the */
				lp->stats.rx_errors++; /* end of a packet.*/
			if (status & RMD1_FRAM) lp->stats.rx_frame_errors++;
			if (status & RMD1_OFLO) lp->stats.rx_over_errors++;
			if (status & RMD1_CRC) lp->stats.rx_crc_errors++;
			if (status & RMD1_BUFF) lp->stats.rx_fifo_errors++;
#ifdef NORMAL_MEM_ACCESS
			head->flag &= (RMD1_ENP|RMD1_STP);
#else
			SET_FLAG(head,GET_FLAG(head) & (RMD1_ENP|RMD1_STP));
#endif
		} else {
			/* Malloc up new buffer, compatible with net-3. */
			short pkt_len = head->msg_length & 0xfff;
			struct sk_buff *skb;

			if (pkt_len < 60) {
				printk( "%s: Runt packet!\n", dev->name );
				lp->stats.rx_errors++;
			}
			else {
				skb = dev_alloc_skb( pkt_len+2 );
				if (skb == NULL) {
					DPRINTK( 1, ( "%s: Memory squeeze, deferring packet.\n",
								  dev->name ));
                          for( i = 0; i < RX_RING_SIZE; i++ )
#ifdef NORMAL_MEM_ACCESS
                        if (MEM->rx_head[(entry+i) & RX_RING_MOD_MASK].flag &
#else
						if (GET_FLAG(&MEM->rx_head[(entry+i) & \
												  RX_RING_MOD_MASK]) &
#endif
							RMD1_OWN_CHIP)
							break;

					if (i > RX_RING_SIZE - 2) {
						lp->stats.rx_dropped++;
#ifdef NORMAL_MEM_ACCESS
                        head->flag |= RMD1_OWN_CHIP;
#else
                        SET_FLAG(head,GET_FLAG(head) | RMD1_OWN_CHIP);
#endif
						lp->cur_rx++;
					}
					break;
				}

				if (lance_debug >= 3) {
					u_char *data = PKTBUF_ADDR(head), *p;
					printk( "%s: RX pkt type 0x%04x from ", dev->name,
							((u_short *)data)[6]);
					for( p = &data[6], i = 0; i < 6; i++ )
						printk("%02x%s", *p++, i != 5 ? ":" : "" );
					printk(" to ");
					for( p = data, i = 0; i < 6; i++ )
						printk("%02x%s", *p++, i != 5 ? ":" : "" );
					printk(" data %02x %02x %02x %02x %02x %02x %02x %02x "
						   "len %d\n",
						   data[15], data[16], data[17], data[18],
						   data[19], data[20], data[21], data[22],
						   pkt_len );
				}

				skb->dev = dev;
				skb_reserve( skb, 2 );	/* 16 byte align */
				skb_put( skb, pkt_len );	/* Make room */
				lp->memcpy_f( skb->data, PKTBUF_ADDR(head), pkt_len );
				skb->protocol = eth_type_trans( skb, dev );
				netif_rx( skb );
				lp->stats.rx_packets++;
				lp->stats.rx_bytes += skb->len;
			}
		}

#ifdef NORMAL_MEM_ACCESS
		head->flag |= RMD1_OWN_CHIP;
#else
		SET_FLAG(head,GET_FLAG(head) | RMD1_OWN_CHIP);
#endif
		entry = (++lp->cur_rx) & RX_RING_MOD_MASK;
	}
	lp->cur_rx &= RX_RING_MOD_MASK;

	/* From lance.c (Donald Becker): */
	/* We should check that at least two ring entries are free.	 If not,
	   we should free one and mark stats->rx_dropped++. */

	return 0;
}


static int lance_close( struct net_device *dev )

{	struct lance_private *lp = (struct lance_private *)dev->priv;
	struct lance_ioreg	 *IO = lp->iobase;

	dev->start = 0;
	dev->tbusy = 1;

	AREG = CSR0;

	DPRINTK( 2, ( "%s: Shutting down ethercard, status was %2.2x.\n",
				  dev->name, DREG ));

	/* We stop the LANCE here -- it occasionally polls
	   memory if we don't. */
	DREG = CSR0_STOP;

	return 0;
}


static struct net_device_stats *lance_get_stats( struct net_device *dev )

{	
	struct lance_private *lp = (struct lance_private *)dev->priv;
	return &lp->stats;
}


/* Set or clear the multicast filter for this adaptor.
   num_addrs == -1		Promiscuous mode, receive all packets
   num_addrs == 0		Normal mode, clear multicast list
   num_addrs > 0		Multicast mode, receive normal and MC packets, and do
						best-effort filtering.
 */

static void set_multicast_list( struct net_device *dev )

{	struct lance_private *lp = (struct lance_private *)dev->priv;
	struct lance_ioreg	 *IO = lp->iobase;

	if (!dev->start)
		/* Only possible if board is already started */
		return;

	/* We take the simple way out and always enable promiscuous mode. */
	DREG = CSR0_STOP; /* Temporarily stop the lance. */

	if (dev->flags & IFF_PROMISC) {
		/* Log any net taps. */
		DPRINTK( 1, ( "%s: Promiscuous mode enabled.\n", dev->name ));
		REGA( CSR15 ) = 0x8000; /* Set promiscuous mode */
	} else {
		short multicast_table[4];
		int num_addrs = dev->mc_count;
		int i;
		/* We don't use the multicast table, but rely on upper-layer
		 * filtering. */
		memset( multicast_table, (num_addrs == 0) ? 0 : -1,
				sizeof(multicast_table) );
		for( i = 0; i < 4; i++ )
			REGA( CSR8+i ) = multicast_table[i];
		REGA( CSR15 ) = 0; /* Unset promiscuous mode */
	}

	/*
	 * Always set BSWP after a STOP as STOP puts it back into
	 * little endian mode.
	 */
	REGA( CSR3 ) = CSR3_BSWP | (lp->cardtype == PAM_CARD ? CSR3_ACON : 0);

	/* Resume normal operation and reset AREG to CSR0 */
	REGA( CSR0 ) = CSR0_IDON | CSR0_INEA | CSR0_STRT;
}


/* This is needed for old RieblCards and possible for new RieblCards */

static int lance_set_mac_address( struct net_device *dev, void *addr )

{	struct lance_private *lp = (struct lance_private *)dev->priv;
	struct sockaddr *saddr = addr;
	int i;

	if (lp->cardtype != OLD_RIEBL && lp->cardtype != NEW_RIEBL)
		return( -EOPNOTSUPP );

	if (dev->start) {
		/* Only possible while card isn't started */
		DPRINTK( 1, ( "%s: hwaddr can be set only while card isn't open.\n",
					  dev->name ));
		return( -EIO );
	}

	slow_memcpy( dev->dev_addr, saddr->sa_data, dev->addr_len );

	{
			unsigned char hwaddr[6];
			for( i = 0; i < 6; i++ ) 
					hwaddr[i] = dev->dev_addr[i^1]; /* <- 16 bit swap! */
			slow_memcpy(MEM->init.hwaddr, hwaddr, sizeof(hwaddr));
	}

	lp->memcpy_f( RIEBL_HWADDR_ADDR, dev->dev_addr, 6 );
	/* set also the magic for future sessions */
#ifdef NORMAL_MEM_ACCESS
	*RIEBL_MAGIC_ADDR = RIEBL_MAGIC;
#else
	{
			unsigned long magic = RIEBL_MAGIC;
			slow_memcpy(RIEBL_MAGIC_ADDR, &magic, sizeof(*RIEBL_MAGIC_ADDR));
	}
#endif
	return( 0 );
}


#ifdef MODULE
static struct net_device bagetlance_dev;

int init_module(void)

{	int err;

	bagetlance_dev.init = bagetlance_probe;
	if ((err = register_netdev( &bagetlance_dev ))) {
		if (err == -EIO)  {
			printk( "No Vme Lance board found. Module not loaded.\n");
		}
		return( err );
	}
	return( 0 );
}

void cleanup_module(void)

{
	unregister_netdev( &bagetlance_dev );
}

#endif /* MODULE */

/*
 * Local variables:
 *  c-indent-level: 4
 *  tab-width: 4
 * End:
 */
