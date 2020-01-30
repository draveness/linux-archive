/* pcnet32.c: An AMD PCnet32 ethernet driver for linux. */
/*
 *	Copyright 1996-1999 Thomas Bogendoerfer
 * 
 *	Derived from the lance driver written 1993,1994,1995 by Donald Becker.
 * 
 *	Copyright 1993 United States Government as represented by the
 *	Director, National Security Agency.
 * 
 *	This software may be used and distributed according to the terms
 *	of the GNU Public License, incorporated herein by reference.
 *
 *	This driver is for PCnet32 and PCnetPCI based ethercards
 */

static const char *version = "pcnet32.c:v1.25kf 26.9.1999 tsbogend@alpha.franken.de\n";

#include <linux/module.h>

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/ptrace.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/malloc.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <asm/bitops.h>
#include <asm/io.h>
#include <asm/dma.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>

static unsigned int pcnet32_portlist[] __initdata = {0x300, 0x320, 0x340, 0x360, 0};

static int pcnet32_debug = 1;
static int tx_start = 1; /* Mapping -- 0:20, 1:64, 2:128, 3:~220 (depends on chip vers) */

static struct net_device *pcnet32_dev;

static const int max_interrupt_work = 80;
static const int rx_copybreak = 200;

#define PORT_AUI      0x00
#define PORT_10BT     0x01
#define PORT_GPSI     0x02
#define PORT_MII      0x03

#define PORT_PORTSEL  0x03
#define PORT_ASEL     0x04
#define PORT_100      0x40
#define PORT_FD	      0x80

#define PCNET32_DMA_MASK 0xffffffff

/*
 * table to translate option values from tulip
 * to internal options
 */
static unsigned char options_mapping[] = {
    PORT_ASEL,			   /*  0 Auto-select	  */
    PORT_AUI,			   /*  1 BNC/AUI	  */
    PORT_AUI,			   /*  2 AUI/BNC	  */ 
    PORT_ASEL,			   /*  3 not supported	  */
    PORT_10BT | PORT_FD,	   /*  4 10baseT-FD	  */
    PORT_ASEL,			   /*  5 not supported	  */
    PORT_ASEL,			   /*  6 not supported	  */
    PORT_ASEL,			   /*  7 not supported	  */
    PORT_ASEL,			   /*  8 not supported	  */
    PORT_MII,			   /*  9 MII 10baseT	  */
    PORT_MII | PORT_FD,		   /* 10 MII 10baseT-FD	  */
    PORT_MII,			   /* 11 MII (autosel)	  */
    PORT_10BT,			   /* 12 10BaseT	  */
    PORT_MII | PORT_100,	   /* 13 MII 100BaseTx	  */
    PORT_MII | PORT_100 | PORT_FD, /* 14 MII 100BaseTx-FD */
    PORT_ASEL			   /* 15 not supported	  */
};

#define MAX_UNITS 8
static int options[MAX_UNITS];
static int full_duplex[MAX_UNITS];

/*
 *				Theory of Operation
 * 
 * This driver uses the same software structure as the normal lance
 * driver. So look for a verbose description in lance.c. The differences
 * to the normal lance driver is the use of the 32bit mode of PCnet32
 * and PCnetPCI chips. Because these chips are 32bit chips, there is no
 * 16MB limitation and we don't need bounce buffers.
 */
 
/*
 * History:
 * v0.01:  Initial version
 *	   only tested on Alpha Noname Board
 * v0.02:  changed IRQ handling for new interrupt scheme (dev_id)
 *	   tested on a ASUS SP3G
 * v0.10:  fixed an odd problem with the 79C974 in a Compaq Deskpro XL
 *	   looks like the 974 doesn't like stopping and restarting in a
 *	   short period of time; now we do a reinit of the lance; the
 *	   bug was triggered by doing ifconfig eth0 <ip> broadcast <addr>
 *	   and hangs the machine (thanks to Klaus Liedl for debugging)
 * v0.12:  by suggestion from Donald Becker: Renamed driver to pcnet32,
 *	   made it standalone (no need for lance.c)
 * v0.13:  added additional PCI detecting for special PCI devices (Compaq)
 * v0.14:  stripped down additional PCI probe (thanks to David C Niemi
 *	   and sveneric@xs4all.nl for testing this on their Compaq boxes)
 * v0.15:  added 79C965 (VLB) probe
 *	   added interrupt sharing for PCI chips
 * v0.16:  fixed set_multicast_list on Alpha machines
 * v0.17:  removed hack from dev.c; now pcnet32 uses ethif_probe in Space.c
 * v0.19:  changed setting of autoselect bit
 * v0.20:  removed additional Compaq PCI probe; there is now a working one
 *	   in arch/i386/bios32.c
 * v0.21:  added endian conversion for ppc, from work by cort@cs.nmt.edu
 * v0.22:  added printing of status to ring dump
 * v0.23:  changed enet_statistics to net_devive_stats
 * v0.90:  added multicast filter
 *	   added module support
 *	   changed irq probe to new style
 *	   added PCnetFast chip id
 *	   added fix for receive stalls with Intel saturn chipsets
 *	   added in-place rx skbs like in the tulip driver
 *	   minor cleanups
 * v0.91:  added PCnetFast+ chip id
 *	   back port to 2.0.x
 * v1.00:  added some stuff from Donald Becker's 2.0.34 version
 *	   added support for byte counters in net_dev_stats
 * v1.01:  do ring dumps, only when debugging the driver
 *	   increased the transmit timeout
 * v1.02:  fixed memory leak in pcnet32_init_ring()
 * v1.10:  workaround for stopped transmitter
 *	   added port selection for modules
 *	   detect special T1/E1 WAN card and setup port selection
 * v1.11:  fixed wrong checking of Tx errors
 * v1.20:  added check of return value kmalloc (cpeterso@cs.washington.edu)
 *	   added save original kmalloc addr for freeing (mcr@solidum.com)
 *	   added support for PCnetHome chip (joe@MIT.EDU)
 *	   rewritten PCI card detection
 *	   added dwio mode to get driver working on some PPC machines
 * v1.21:  added mii selection and mii ioctl
 * v1.22:  changed pci scanning code to make PPC people happy
 *	   fixed switching to 32bit mode in pcnet32_open() (thanks
 *	   to Michael Richard <mcr@solidum.com> for noticing this one)
 *	   added sub vendor/device id matching (thanks again to 
 *	   Michael Richard <mcr@solidum.com>)
 *	   added chip id for 79c973/975 (thanks to Zach Brown <zab@zabbo.net>)
 * v1.23   fixed small bug, when manual selecting MII speed/duplex
 * v1.24   Applied Thomas' patch to use TxStartPoint and thus decrease TxFIFO
 *	   underflows.	Added tx_start_pt module parameter. Increased
 *	   TX_RING_SIZE from 16 to 32.	Added #ifdef'd code to use DXSUFLO
 *	   for FAST[+] chipsets. <kaf@fc.hp.com>
 * v1.24ac Added SMP spinlocking - Alan Cox <alan@redhat.com>
 * v1.25kf Added No Interrupt on successful Tx for some Tx's <kaf@fc.hp.com>
 * v1.26   Converted to pci_alloc_consistent, Jamey Hicks / George France
 *                                           <jamey@crl.dec.com>
 */


/*
 * Set the number of Tx and Rx buffers, using Log_2(# buffers).
 * Reasonable default values are 4 Tx buffers, and 16 Rx buffers.
 * That translates to 2 (4 == 2^^2) and 4 (16 == 2^^4).
 */
#ifndef PCNET32_LOG_TX_BUFFERS
#define PCNET32_LOG_TX_BUFFERS 4
#define PCNET32_LOG_RX_BUFFERS 5
#endif

#define TX_RING_SIZE			(1 << (PCNET32_LOG_TX_BUFFERS))
#define TX_RING_MOD_MASK		(TX_RING_SIZE - 1)
#define TX_RING_LEN_BITS		((PCNET32_LOG_TX_BUFFERS) << 12)

#define RX_RING_SIZE			(1 << (PCNET32_LOG_RX_BUFFERS))
#define RX_RING_MOD_MASK		(RX_RING_SIZE - 1)
#define RX_RING_LEN_BITS		((PCNET32_LOG_RX_BUFFERS) << 4)

#define PKT_BUF_SZ		1544

/* Offsets from base I/O address. */
#define PCNET32_WIO_RDP		0x10
#define PCNET32_WIO_RAP		0x12
#define PCNET32_WIO_RESET	0x14
#define PCNET32_WIO_BDP		0x16

#define PCNET32_DWIO_RDP	0x10
#define PCNET32_DWIO_RAP	0x14
#define PCNET32_DWIO_RESET	0x18
#define PCNET32_DWIO_BDP	0x1C

#define PCNET32_TOTAL_SIZE 0x20

/* some PCI ids */
#ifndef PCI_DEVICE_ID_AMD_LANCE
#define PCI_VENDOR_ID_AMD	      0x1022
#define PCI_DEVICE_ID_AMD_LANCE	      0x2000
#endif
#ifndef PCI_DEVICE_ID_AMD_PCNETHOME
#define PCI_DEVICE_ID_AMD_PCNETHOME   0x2001
#endif


#define CRC_POLYNOMIAL_LE 0xedb88320UL	/* Ethernet CRC, little endian */

/* The PCNET32 Rx and Tx ring descriptors. */
struct pcnet32_rx_head {
    u32 base;
    s16 buf_length;
    s16 status;	   
    u32 msg_length;
    u32 reserved;
};
	
struct pcnet32_tx_head {
    u32 base;
    s16 length;
    s16 status;
    u32 misc;
    u32 reserved;
};

/* The PCNET32 32-Bit initialization block, described in databook. */
struct pcnet32_init_block {
    u16 mode;
    u16 tlen_rlen;
    u8	phys_addr[6];
    u16 reserved;
    u32 filter[2];
    /* Receive and transmit ring base, along with extra bits. */    
    u32 rx_ring;
    u32 tx_ring;
};

/* PCnet32 access functions */
struct pcnet32_access {
    u16 (*read_csr)(unsigned long, int);
    void (*write_csr)(unsigned long, int, u16);
    u16 (*read_bcr)(unsigned long, int);
    void (*write_bcr)(unsigned long, int, u16);
    u16 (*read_rap)(unsigned long);
    void (*write_rap)(unsigned long, u16);
    void (*reset)(unsigned long);
};

/*
 * The first three fields of pcnet32_private are read by the ethernet device 
 * so we allocate the structure should be allocated by pci_alloc_consistent().
 */
struct pcnet32_private {
    /* The Tx and Rx ring entries must be aligned on 16-byte boundaries in 32bit mode. */
    struct pcnet32_rx_head   rx_ring[RX_RING_SIZE];
    struct pcnet32_tx_head   tx_ring[TX_RING_SIZE];
    struct pcnet32_init_block	init_block;
    dma_addr_t dma_addr;		/* DMA address of beginning of this object, returned by pci_alloc_consistent */
    struct pci_dev *pci_dev;		/* Pointer to the associated pci device structure */
    const char *name;
    /* The saved address of a sent-in-place packet/buffer, for skfree(). */
    struct sk_buff *tx_skbuff[TX_RING_SIZE];
    struct sk_buff *rx_skbuff[RX_RING_SIZE];
    dma_addr_t tx_dma_addr[TX_RING_SIZE];
    dma_addr_t rx_dma_addr[RX_RING_SIZE];
    struct pcnet32_access a;
    spinlock_t lock;					/* Guard lock */
    unsigned int cur_rx, cur_tx;		/* The next free ring entry */
    unsigned int dirty_rx, dirty_tx;	/* The ring entries to be free()ed. */
    struct net_device_stats stats;
    char tx_full;
    int	 options;
    int	 shared_irq:1,			/* shared irq possible */
	ltint:1,
#ifdef DO_DXSUFLO
	      dxsuflo:1,						    /* disable transmit stop on uflo */
#endif
	full_duplex:1,				/* full duplex possible */
	mii:1;					/* mii port available */
    struct net_device *next;
};

static int  pcnet32_probe_vlbus(int cards_found);
static int  pcnet32_probe_pci(struct pci_dev *, const struct pci_device_id *);
static int  pcnet32_probe1(unsigned long, unsigned char, int, int, struct pci_dev *);
static int  pcnet32_open(struct net_device *);
static int  pcnet32_init_ring(struct net_device *);
static int  pcnet32_start_xmit(struct sk_buff *, struct net_device *);
static int  pcnet32_rx(struct net_device *);
static void pcnet32_tx_timeout (struct net_device *dev);
static void pcnet32_interrupt(int, void *, struct pt_regs *);
static int  pcnet32_close(struct net_device *);
static struct net_device_stats *pcnet32_get_stats(struct net_device *);
static void pcnet32_set_multicast_list(struct net_device *);
#ifdef HAVE_PRIVATE_IOCTL
static int  pcnet32_mii_ioctl(struct net_device *, struct ifreq *, int);
#endif

enum pci_flags_bit {
    PCI_USES_IO=1, PCI_USES_MEM=2, PCI_USES_MASTER=4,
    PCI_ADDR0=0x10<<0, PCI_ADDR1=0x10<<1, PCI_ADDR2=0x10<<2, PCI_ADDR3=0x10<<3,
};

struct pcnet32_pci_id_info {
    const char *name;
    u16 vendor_id, device_id, svid, sdid, flags;
    int io_size;
    int (*probe1) (unsigned long, unsigned char, int, int, struct pci_dev *);
};


/*
 * PCI device identifiers for "new style" Linux PCI Device Drivers
 */
static struct pci_device_id pcnet32_pci_tbl[] __devinitdata = {
    { PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_PCNETHOME, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
    { PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_LANCE, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
    { PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_LANCE, 0x1014, 0x2000, 0, 0, 0 },
    { 0, }
};

MODULE_DEVICE_TABLE (pci, pcnet32_pci_tbl);

static u16 pcnet32_wio_read_csr (unsigned long addr, int index)
{
    outw (index, addr+PCNET32_WIO_RAP);
    return inw (addr+PCNET32_WIO_RDP);
}

static void pcnet32_wio_write_csr (unsigned long addr, int index, u16 val)
{
    outw (index, addr+PCNET32_WIO_RAP);
    outw (val, addr+PCNET32_WIO_RDP);
}

static u16 pcnet32_wio_read_bcr (unsigned long addr, int index)
{
    outw (index, addr+PCNET32_WIO_RAP);
    return inw (addr+PCNET32_WIO_BDP);
}

static void pcnet32_wio_write_bcr (unsigned long addr, int index, u16 val)
{
    outw (index, addr+PCNET32_WIO_RAP);
    outw (val, addr+PCNET32_WIO_BDP);
}

static u16 pcnet32_wio_read_rap (unsigned long addr)
{
    return inw (addr+PCNET32_WIO_RAP);
}

static void pcnet32_wio_write_rap (unsigned long addr, u16 val)
{
    outw (val, addr+PCNET32_WIO_RAP);
}

static void pcnet32_wio_reset (unsigned long addr)
{
    inw (addr+PCNET32_WIO_RESET);
}

static int pcnet32_wio_check (unsigned long addr)
{
    outw (88, addr+PCNET32_WIO_RAP);
    return (inw (addr+PCNET32_WIO_RAP) == 88);
}

static struct pcnet32_access pcnet32_wio = {
    pcnet32_wio_read_csr,
    pcnet32_wio_write_csr,
    pcnet32_wio_read_bcr,
    pcnet32_wio_write_bcr,
    pcnet32_wio_read_rap,
    pcnet32_wio_write_rap,
    pcnet32_wio_reset
};

static u16 pcnet32_dwio_read_csr (unsigned long addr, int index)
{
    outl (index, addr+PCNET32_DWIO_RAP);
    return (inl (addr+PCNET32_DWIO_RDP) & 0xffff);
}

static void pcnet32_dwio_write_csr (unsigned long addr, int index, u16 val)
{
    outl (index, addr+PCNET32_DWIO_RAP);
    outl (val, addr+PCNET32_DWIO_RDP);
}

static u16 pcnet32_dwio_read_bcr (unsigned long addr, int index)
{
    outl (index, addr+PCNET32_DWIO_RAP);
    return (inl (addr+PCNET32_DWIO_BDP) & 0xffff);
}

static void pcnet32_dwio_write_bcr (unsigned long addr, int index, u16 val)
{
    outl (index, addr+PCNET32_DWIO_RAP);
    outl (val, addr+PCNET32_DWIO_BDP);
}

static u16 pcnet32_dwio_read_rap (unsigned long addr)
{
    return (inl (addr+PCNET32_DWIO_RAP) & 0xffff);
}

static void pcnet32_dwio_write_rap (unsigned long addr, u16 val)
{
    outl (val, addr+PCNET32_DWIO_RAP);
}

static void pcnet32_dwio_reset (unsigned long addr)
{
    inl (addr+PCNET32_DWIO_RESET);
}

static int pcnet32_dwio_check (unsigned long addr)
{
    outl (88, addr+PCNET32_DWIO_RAP);
    return (inl (addr+PCNET32_DWIO_RAP) == 88);
}

static struct pcnet32_access pcnet32_dwio = {
    pcnet32_dwio_read_csr,
    pcnet32_dwio_write_csr,
    pcnet32_dwio_read_bcr,
    pcnet32_dwio_write_bcr,
    pcnet32_dwio_read_rap,
    pcnet32_dwio_write_rap,
    pcnet32_dwio_reset

};



/* only probes for non-PCI devices, the rest are handled by pci_register_driver via pcnet32_probe_pci*/
static int __init pcnet32_probe_vlbus(int cards_found)
{
    unsigned long ioaddr = 0; // FIXME dev ? dev->base_addr: 0;
    unsigned int  irq_line = 0; // FIXME dev ? dev->irq : 0;
    int *port;
    
    printk(KERN_INFO "pcnet32_probe_vlbus: cards_found=%d\n", cards_found);
#ifndef __powerpc__
    if (ioaddr > 0x1ff) {
	if (check_region(ioaddr, PCNET32_TOTAL_SIZE) == 0)
	    return pcnet32_probe1(ioaddr, irq_line, 0, 0, NULL);
	else
	    return -ENODEV;
    } else
#endif
	if (ioaddr != 0)
	    return -ENXIO;
    
    /* now look for PCnet32 VLB cards */
    for (port = pcnet32_portlist; *port; port++) {
	unsigned long ioaddr = *port;
	
	if ( check_region(ioaddr, PCNET32_TOTAL_SIZE) == 0) {
	    /* check if there is really a pcnet chip on that ioaddr */
	    if ((inb(ioaddr + 14) == 0x57) &&
		(inb(ioaddr + 15) == 0x57) &&
		(pcnet32_probe1(ioaddr, 0, 0, 0, NULL) == 0))
		cards_found++;
	}
    }
    return cards_found ? 0: -ENODEV;
}



static int __init
pcnet32_probe_pci(struct pci_dev *pdev, const struct pci_device_id *ent)
{
    static int card_idx;
    long ioaddr;
    int err = 0;

    printk(KERN_INFO "pcnet32_probe_pci: found device %#08x.%#08x\n", ent->vendor, ent->device);

    ioaddr = pci_resource_start (pdev, 0);
    printk(KERN_INFO "    ioaddr=%#08lx  resource_flags=%#08lx\n", ioaddr, pci_resource_flags (pdev, 0));
    if (!ioaddr) {
        printk (KERN_ERR "no PCI IO resources, aborting\n");
        return -ENODEV;
    }
	
    if (!pci_dma_supported(pdev, PCNET32_DMA_MASK)) {
	printk(KERN_ERR "pcnet32.c: architecture does not support 32bit PCI busmaster DMA\n");
	return -ENODEV;
    }

    if ((err = pci_enable_device(pdev)) < 0) {
	printk(KERN_ERR "pcnet32.c: failed to enable device -- err=%d\n", err);
	return err;
    }
    
    pci_set_master(pdev);
    
    return pcnet32_probe1(ioaddr, pdev->irq, 1, card_idx, pdev);
}


/* pcnet32_probe1 
 *  Called from both pcnet32_probe_vlbus and pcnet_probe_pci.  
 *  pdev will be NULL when called from pcnet32_probe_vlbus.
 */
static int __init
pcnet32_probe1(unsigned long ioaddr, unsigned char irq_line, int shared, int card_idx, struct pci_dev *pdev)
{
    struct pcnet32_private *lp;
    dma_addr_t lp_dma_addr;
    int i,media,fdx = 0, mii = 0, fset = 0;
#ifdef DO_DXSUFLO
    int dxsuflo = 0;
#endif
    int ltint = 0;
    int chip_version;
    char *chipname;
    struct net_device *dev;
    struct pcnet32_access *a = NULL;

    /* reset the chip */
    pcnet32_dwio_reset(ioaddr);
    pcnet32_wio_reset(ioaddr);

    if (pcnet32_wio_read_csr (ioaddr, 0) == 4 && pcnet32_wio_check (ioaddr)) {
	a = &pcnet32_wio;
    } else {
	if (pcnet32_dwio_read_csr (ioaddr, 0) == 4 && pcnet32_dwio_check(ioaddr)) {
	    a = &pcnet32_dwio;
	} else
	    return -ENODEV;
    }

    chip_version = a->read_csr (ioaddr, 88) | (a->read_csr (ioaddr,89) << 16);
    if (pcnet32_debug > 2)
	printk(KERN_INFO "  PCnet chip version is %#x.\n", chip_version);
    if ((chip_version & 0xfff) != 0x003)
	return -ENODEV;
    chip_version = (chip_version >> 12) & 0xffff;
    switch (chip_version) {
    case 0x2420:
	chipname = "PCnet/PCI 79C970"; /* PCI */
	break;
    case 0x2430:
	if (shared)
	    chipname = "PCnet/PCI 79C970"; /* 970 gives the wrong chip id back */
	else
	    chipname = "PCnet/32 79C965"; /* 486/VL bus */
	break;
    case 0x2621:
	chipname = "PCnet/PCI II 79C970A"; /* PCI */
	fdx = 1;
	break;
    case 0x2623:
	chipname = "PCnet/FAST 79C971"; /* PCI */
	fdx = 1; mii = 1; fset = 1;
	ltint = 1;
	break;
    case 0x2624:
	chipname = "PCnet/FAST+ 79C972"; /* PCI */
	fdx = 1; mii = 1; fset = 1;
	break;
    case 0x2625:
	chipname = "PCnet/FAST III 79C973"; /* PCI */
	fdx = 1; mii = 1;
	break;
    case 0x2626:
	chipname = "PCnet/Home 79C978"; /* PCI */
	fdx = 1;
	/* 
	 * This is based on specs published at www.amd.com.  This section
	 * assumes that a card with a 79C978 wants to go into 1Mb HomePNA
	 * mode.  The 79C978 can also go into standard ethernet, and there
	 * probably should be some sort of module option to select the
	 * mode by which the card should operate
	 */
	/* switch to home wiring mode */
	media = a->read_bcr (ioaddr, 49);
#if 0
	if (pcnet32_debug > 2)
	    printk(KERN_DEBUG "pcnet32: pcnet32 media value %#x.\n",  media);
	media &= ~3;
	media |= 1;
#endif
	if (pcnet32_debug > 2)
	    printk(KERN_DEBUG "pcnet32: pcnet32 media reset to %#x.\n",  media);
	a->write_bcr (ioaddr, 49, media);
	break;
    case 0x2627:
	chipname = "PCnet/FAST III 79C975"; /* PCI */
	fdx = 1; mii = 1;
	break;
    default:
	printk(KERN_INFO "pcnet32: PCnet version %#x, no PCnet32 chip.\n",chip_version);
	return -ENODEV;
    }

    /*
     *	On selected chips turn on the BCR18:NOUFLO bit. This stops transmit
     *	starting until the packet is loaded. Strike one for reliability, lose
     *	one for latency - although on PCI this isnt a big loss. Older chips 
     *	have FIFO's smaller than a packet, so you can't do this.
     */
	 
    if(fset)
    {
	a->write_bcr(ioaddr, 18, (a->read_bcr(ioaddr, 18) | 0x0800));
	a->write_csr(ioaddr, 80, (a->read_csr(ioaddr, 80) & 0x0C00) | 0x0c00);
#ifdef DO_DXSUFLO
	dxsuflo = 1;
#endif
	ltint = 1;
    }
    
    dev = init_etherdev(NULL, 0);
    if(dev==NULL)
	return -ENOMEM;

    printk(KERN_INFO "%s: %s at %#3lx,", dev->name, chipname, ioaddr);

    /* There is a 16 byte station address PROM at the base address.
       The first six bytes are the station address. */
    for (i = 0; i < 6; i++)
	printk(" %2.2x", dev->dev_addr[i] = inb(ioaddr + i));

    if (((chip_version + 1) & 0xfffe) == 0x2624) { /* Version 0x2623 or 0x2624 */
	i = a->read_csr(ioaddr, 80) & 0x0C00;  /* Check tx_start_pt */
	printk("\n" KERN_INFO "    tx_start_pt(0x%04x):",i);
	switch(i>>10) {
	    case 0: printk("  20 bytes,"); break;
	    case 1: printk("  64 bytes,"); break;
	    case 2: printk(" 128 bytes,"); break;
	    case 3: printk("~220 bytes,"); break;
	}
	i = a->read_bcr(ioaddr, 18);  /* Check Burst/Bus control */
	printk(" BCR18(%x):",i&0xffff);
	if (i & (1<<5)) printk("BurstWrEn ");
	if (i & (1<<6)) printk("BurstRdEn ");
	if (i & (1<<7)) printk("DWordIO ");
	if (i & (1<<11)) printk("NoUFlow ");
	i = a->read_bcr(ioaddr, 25);
	printk("\n" KERN_INFO "    SRAMSIZE=0x%04x,",i<<8);
	i = a->read_bcr(ioaddr, 26);
	printk(" SRAM_BND=0x%04x,",i<<8);
	i = a->read_bcr(ioaddr, 27);
	if (i & (1<<14)) printk("LowLatRx");
    }

    dev->base_addr = ioaddr;
    request_region(ioaddr, PCNET32_TOTAL_SIZE, chipname);
    
    /* pci_alloc_consistent returns page-aligned memory, so we do not have to check the alignment */
    if ((lp = (struct pcnet32_private *)pci_alloc_consistent(pdev, sizeof(*lp), &lp_dma_addr)) == NULL)
	return -ENOMEM;

    memset(lp, 0, sizeof(*lp));
    lp->dma_addr = lp_dma_addr;
    lp->pci_dev = pdev;
    printk("\n" KERN_INFO "pcnet32: pcnet32_private lp=%p lp_dma_addr=%#08x", lp, lp_dma_addr);

    spin_lock_init(&lp->lock);
    
    dev->priv = lp;
    lp->name = chipname;
    lp->shared_irq = shared;
    lp->full_duplex = fdx;
#ifdef DO_DXSUFLO
    lp->dxsuflo = dxsuflo;
#endif
    lp->ltint = ltint;
    lp->mii = mii;
    if (options[card_idx] > sizeof (options_mapping))
	lp->options = PORT_ASEL;
    else
	lp->options = options_mapping[options[card_idx]];
    
    if (fdx && !(lp->options & PORT_ASEL) && full_duplex[card_idx])
	lp->options |= PORT_FD;
    
    if (a == NULL) {
      printk(KERN_ERR "pcnet32: No access methods\n");
      return -ENODEV;
    }
    lp->a = *a;
    
    /* detect special T1/E1 WAN card by checking for MAC address */
    if (dev->dev_addr[0] == 0x00 && dev->dev_addr[1] == 0xe0 && dev->dev_addr[2] == 0x75)
	lp->options = PORT_FD | PORT_GPSI;

    lp->init_block.mode = le16_to_cpu(0x0003);	/* Disable Rx and Tx. */
    lp->init_block.tlen_rlen = le16_to_cpu(TX_RING_LEN_BITS | RX_RING_LEN_BITS); 
    for (i = 0; i < 6; i++)
	lp->init_block.phys_addr[i] = dev->dev_addr[i];
    lp->init_block.filter[0] = 0x00000000;
    lp->init_block.filter[1] = 0x00000000;
    lp->init_block.rx_ring = (u32)le32_to_cpu(lp->dma_addr + offsetof(struct pcnet32_private, rx_ring));
    lp->init_block.tx_ring = (u32)le32_to_cpu(lp->dma_addr + offsetof(struct pcnet32_private, tx_ring));
    
    /* switch pcnet32 to 32bit mode */
    a->write_bcr (ioaddr, 20, 2);

    a->write_csr (ioaddr, 1, (lp->dma_addr + offsetof(struct pcnet32_private, init_block)) & 0xffff);
    a->write_csr (ioaddr, 2, (lp->dma_addr + offsetof(struct pcnet32_private, init_block)) >> 16);
    
    if (irq_line) {
	dev->irq = irq_line;
    }
    
    if (dev->irq >= 2)
	printk(" assigned IRQ %d.\n", dev->irq);
    else {
	unsigned long irq_mask = probe_irq_on();
	
	/*
	 * To auto-IRQ we enable the initialization-done and DMA error
	 * interrupts. For ISA boards we get a DMA error, but VLB and PCI
	 * boards will work.
	 */
	/* Trigger an initialization just for the interrupt. */
	a->write_csr (ioaddr, 0, 0x41);
	mdelay (1);
	
	dev->irq = probe_irq_off (irq_mask);
	if (dev->irq)
	    printk(", probed IRQ %d.\n", dev->irq);
	else {
	    printk(", failed to detect IRQ line.\n");
	    return -ENODEV;
	}
    }

    if (pcnet32_debug > 0)
	printk(KERN_INFO "%s", version);
    
    /* The PCNET32-specific entries in the device structure. */
    dev->open = &pcnet32_open;
    dev->hard_start_xmit = &pcnet32_start_xmit;
    dev->stop = &pcnet32_close;
    dev->get_stats = &pcnet32_get_stats;
    dev->set_multicast_list = &pcnet32_set_multicast_list;
#ifdef HAVE_PRIVATE_IOCTL
    dev->do_ioctl = &pcnet32_mii_ioctl;
#endif
    dev->tx_timeout = pcnet32_tx_timeout;
    dev->watchdog_timeo = (HZ >> 1);

    lp->next = pcnet32_dev;
    pcnet32_dev = dev;

    /* Fill in the generic fields of the device structure. */
    ether_setup(dev);
    return 0;
}


static int
pcnet32_open(struct net_device *dev)
{
    struct pcnet32_private *lp = (struct pcnet32_private *)dev->priv;
    unsigned long ioaddr = dev->base_addr;
    u16 val;
    int i;

    if (dev->irq == 0 ||
	request_irq(dev->irq, &pcnet32_interrupt,
		    lp->shared_irq ? SA_SHIRQ : 0, lp->name, (void *)dev)) {
	return -EAGAIN;
    }

    /* Reset the PCNET32 */
    lp->a.reset (ioaddr);

    /* switch pcnet32 to 32bit mode */
    lp->a.write_bcr (ioaddr, 20, 2);

    if (pcnet32_debug > 1)
	printk(KERN_DEBUG "%s: pcnet32_open() irq %d tx/rx rings %#x/%#x init %#x.\n",
	       dev->name, dev->irq,
	       (u32) (lp->dma_addr + offsetof(struct pcnet32_private, tx_ring)),
	       (u32) (lp->dma_addr + offsetof(struct pcnet32_private, rx_ring)),
	       (u32) (lp->dma_addr + offsetof(struct pcnet32_private, init_block)));
    
    /* set/reset autoselect bit */
    val = lp->a.read_bcr (ioaddr, 2) & ~2;
    if (lp->options & PORT_ASEL)
	val |= 2;
    lp->a.write_bcr (ioaddr, 2, val);
    
    /* handle full duplex setting */
    if (lp->full_duplex) {
	val = lp->a.read_bcr (ioaddr, 9) & ~3;
	if (lp->options & PORT_FD) {
	    val |= 1;
	    if (lp->options == (PORT_FD | PORT_AUI))
		val |= 2;
	}
	lp->a.write_bcr (ioaddr, 9, val);
    }
    
    /* set/reset GPSI bit in test register */
    val = lp->a.read_csr (ioaddr, 124) & ~0x10;
    if ((lp->options & PORT_PORTSEL) == PORT_GPSI)
	val |= 0x10;
    lp->a.write_csr (ioaddr, 124, val);
    
    if (lp->mii & !(lp->options & PORT_ASEL)) {
	val = lp->a.read_bcr (ioaddr, 32) & ~0x38; /* disable Auto Negotiation, set 10Mpbs, HD */
	if (lp->options & PORT_FD)
	    val |= 0x10;
	if (lp->options & PORT_100)
	    val |= 0x08;
	lp->a.write_bcr (ioaddr, 32, val);
    }

#ifdef DO_DXSUFLO 
    if (lp->dxsuflo) { /* Disable transmit stop on underflow */
	val = lp->a.read_csr (ioaddr, 3);
	val |= 0x40;
	lp->a.write_csr (ioaddr, 3, val);
    }
#endif
    if (lp->ltint) { /* Enable TxDone-intr inhibitor */
	val = lp->a.read_csr (ioaddr, 5);
	val |= (1<<14);
	lp->a.write_csr (ioaddr, 5, val);
    }
   
    lp->init_block.mode = le16_to_cpu((lp->options & PORT_PORTSEL) << 7);
    lp->init_block.filter[0] = 0x00000000;
    lp->init_block.filter[1] = 0x00000000;
    if (pcnet32_init_ring(dev))
	return -ENOMEM;
    
    /* Re-initialize the PCNET32, and start it when done. */
    lp->a.write_csr (ioaddr, 1, (lp->dma_addr + offsetof(struct pcnet32_private, init_block)) &0xffff);
    lp->a.write_csr (ioaddr, 2, (lp->dma_addr + offsetof(struct pcnet32_private, init_block)) >> 16);

    lp->a.write_csr (ioaddr, 4, 0x0915);
    lp->a.write_csr (ioaddr, 0, 0x0001);

    netif_start_queue(dev);

    i = 0;
    while (i++ < 100)
	if (lp->a.read_csr (ioaddr, 0) & 0x0100)
	    break;
    /* 
     * We used to clear the InitDone bit, 0x0100, here but Mark Stockton
     * reports that doing so triggers a bug in the '974.
     */
    lp->a.write_csr (ioaddr, 0, 0x0042);

    if (pcnet32_debug > 2)
	printk(KERN_DEBUG "%s: pcnet32 open after %d ticks, init block %#x csr0 %4.4x.\n",
	       dev->name, i, (u32) (lp->dma_addr + offsetof(struct pcnet32_private, init_block)),
	       lp->a.read_csr (ioaddr, 0));


    MOD_INC_USE_COUNT;
    
    return 0;	/* Always succeed */
}

/*
 * The LANCE has been halted for one reason or another (busmaster memory
 * arbitration error, Tx FIFO underflow, driver stopped it to reconfigure,
 * etc.).  Modern LANCE variants always reload their ring-buffer
 * configuration when restarted, so we must reinitialize our ring
 * context before restarting.  As part of this reinitialization,
 * find all packets still on the Tx ring and pretend that they had been
 * sent (in effect, drop the packets on the floor) - the higher-level
 * protocols will time out and retransmit.  It'd be better to shuffle
 * these skbs to a temp list and then actually re-Tx them after
 * restarting the chip, but I'm too lazy to do so right now.  dplatt@3do.com
 */

static void 
pcnet32_purge_tx_ring(struct net_device *dev)
{
    struct pcnet32_private *lp = (struct pcnet32_private *)dev->priv;
    int i;

    for (i = 0; i < TX_RING_SIZE; i++) {
	if (lp->tx_skbuff[i]) {
            pci_unmap_single(lp->pci_dev, lp->tx_dma_addr[i], lp->tx_skbuff[i]->len, PCI_DMA_TODEVICE);
	    dev_kfree_skb(lp->tx_skbuff[i]); 
	    lp->tx_skbuff[i] = NULL;
            lp->tx_dma_addr[i] = 0;
	}
    }
}


/* Initialize the PCNET32 Rx and Tx rings. */
static int
pcnet32_init_ring(struct net_device *dev)
{
    struct pcnet32_private *lp = (struct pcnet32_private *)dev->priv;
    int i;

    lp->tx_full = 0;
    lp->cur_rx = lp->cur_tx = 0;
    lp->dirty_rx = lp->dirty_tx = 0;

    for (i = 0; i < RX_RING_SIZE; i++) {
        struct sk_buff *rx_skbuff = lp->rx_skbuff[i];
	if (rx_skbuff == NULL) {
	    if (!(rx_skbuff = lp->rx_skbuff[i] = dev_alloc_skb (PKT_BUF_SZ))) {
		/* there is not much, we can do at this point */
		printk(KERN_ERR "%s: pcnet32_init_ring dev_alloc_skb failed.\n",dev->name);
		return -1;
	    }
	    skb_reserve (rx_skbuff, 2);
	}
        lp->rx_dma_addr[i] = pci_map_single(lp->pci_dev, rx_skbuff->tail, rx_skbuff->len, PCI_DMA_FROMDEVICE);
	lp->rx_ring[i].base = (u32)le32_to_cpu(lp->rx_dma_addr[i]);
	lp->rx_ring[i].buf_length = le16_to_cpu(-PKT_BUF_SZ);
	lp->rx_ring[i].status = le16_to_cpu(0x8000);
    }
    /* The Tx buffer address is filled in as needed, but we do need to clear
       the upper ownership bit. */
    for (i = 0; i < TX_RING_SIZE; i++) {
	lp->tx_ring[i].base = 0;
	lp->tx_ring[i].status = 0;
        lp->tx_dma_addr[i] = 0;
    }

    lp->init_block.tlen_rlen = le16_to_cpu(TX_RING_LEN_BITS | RX_RING_LEN_BITS);
    for (i = 0; i < 6; i++)
	lp->init_block.phys_addr[i] = dev->dev_addr[i];
    lp->init_block.rx_ring = (u32)le32_to_cpu(lp->dma_addr + offsetof(struct pcnet32_private, rx_ring));
    lp->init_block.tx_ring = (u32)le32_to_cpu(lp->dma_addr + offsetof(struct pcnet32_private, tx_ring));
    return 0;
}

static void
pcnet32_restart(struct net_device *dev, unsigned int csr0_bits)
{
    struct pcnet32_private *lp = (struct pcnet32_private *)dev->priv;
    unsigned long ioaddr = dev->base_addr;
    int i;
    
    pcnet32_purge_tx_ring(dev);
    if (pcnet32_init_ring(dev))
	return;
    
    /* ReInit Ring */
    lp->a.write_csr (ioaddr, 0, 1);
    i = 0;
    while (i++ < 100)
	if (lp->a.read_csr (ioaddr, 0) & 0x0100)
	    break;

    lp->a.write_csr (ioaddr, 0, csr0_bits);
}


static void
pcnet32_tx_timeout (struct net_device *dev)
{
    struct pcnet32_private *lp = (struct pcnet32_private *)dev->priv;
    unsigned int ioaddr = dev->base_addr;

    /* Transmitter timeout, serious problems. */
	printk(KERN_ERR "%s: transmit timed out, status %4.4x, resetting.\n",
	       dev->name, lp->a.read_csr (ioaddr, 0));
	lp->a.write_csr (ioaddr, 0, 0x0004);
	lp->stats.tx_errors++;
	if (pcnet32_debug > 2) {
	    int i;
	    printk(KERN_DEBUG " Ring data dump: dirty_tx %d cur_tx %d%s cur_rx %d.",
	       lp->dirty_tx, lp->cur_tx, lp->tx_full ? " (full)" : "",
	       lp->cur_rx);
	    for (i = 0 ; i < RX_RING_SIZE; i++)
	    printk("%s %08x %04x %08x %04x", i & 1 ? "" : "\n ",
		   lp->rx_ring[i].base, -lp->rx_ring[i].buf_length,
		   lp->rx_ring[i].msg_length, (unsigned)lp->rx_ring[i].status);
	    for (i = 0 ; i < TX_RING_SIZE; i++)
	    printk("%s %08x %04x %08x %04x", i & 1 ? "" : "\n ",
		   lp->tx_ring[i].base, -lp->tx_ring[i].length,
		   lp->tx_ring[i].misc, (unsigned)lp->tx_ring[i].status);
	    printk("\n");
	}
	pcnet32_restart(dev, 0x0042);

	dev->trans_start = jiffies;
	netif_start_queue(dev);
}


static int
pcnet32_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
    struct pcnet32_private *lp = (struct pcnet32_private *)dev->priv;
    unsigned int ioaddr = dev->base_addr;
    u16 status;
    int entry;
    unsigned long flags;

    if (pcnet32_debug > 3) {
	printk(KERN_DEBUG "%s: pcnet32_start_xmit() called, csr0 %4.4x.\n",
	       dev->name, lp->a.read_csr (ioaddr, 0));
    }

    spin_lock_irqsave(&lp->lock, flags);

    /* Default status -- will not enable Successful-TxDone
     * interrupt when that option is available to us.
     */
    status = 0x8300;
    if ((lp->ltint) &&
	((lp->cur_tx - lp->dirty_tx == TX_RING_SIZE/2) ||
	 (lp->cur_tx - lp->dirty_tx >= TX_RING_SIZE-2)))
    {
	/* Enable Successful-TxDone interrupt if we have
	 * 1/2 of, or nearly all of, our ring buffer Tx'd
	 * but not yet cleaned up.  Thus, most of the time,
	 * we will not enable Successful-TxDone interrupts.
	 */
	status = 0x9300;
    }
  
    /* Fill in a Tx ring entry */
  
    /* Mask to ring buffer boundary. */
    entry = lp->cur_tx & TX_RING_MOD_MASK;
  
    /* Caution: the write order is important here, set the base address
       with the "ownership" bits last. */

    lp->tx_ring[entry].length = le16_to_cpu(-skb->len);

    lp->tx_ring[entry].misc = 0x00000000;

    lp->tx_skbuff[entry] = skb;
    lp->tx_dma_addr[entry] = pci_map_single(lp->pci_dev, skb->data, skb->len, PCI_DMA_TODEVICE);
    lp->tx_ring[entry].base = (u32)le32_to_cpu(lp->tx_dma_addr[entry]);
    lp->tx_ring[entry].status = le16_to_cpu(status);

    lp->cur_tx++;
    lp->stats.tx_bytes += skb->len;

    /* Trigger an immediate send poll. */
    lp->a.write_csr (ioaddr, 0, 0x0048);

    dev->trans_start = jiffies;

    if (lp->tx_ring[(entry+1) & TX_RING_MOD_MASK].base == 0)
	netif_start_queue(dev);
    else {
	lp->tx_full = 1;
	netif_stop_queue(dev);
    }
    spin_unlock_irqrestore(&lp->lock, flags);
    return 0;
}

/* The PCNET32 interrupt handler. */
static void
pcnet32_interrupt(int irq, void *dev_id, struct pt_regs * regs)
{
    struct net_device *dev = (struct net_device *)dev_id;
    struct pcnet32_private *lp;
    unsigned long ioaddr;
    u16 csr0,rap;
    int boguscnt =  max_interrupt_work;
    int must_restart;

    if (dev == NULL) {
	printk (KERN_DEBUG "pcnet32_interrupt(): irq %d for unknown device.\n", irq);
	return;
    }

    ioaddr = dev->base_addr;
    lp = (struct pcnet32_private *)dev->priv;
    
    spin_lock(&lp->lock);
    
    rap = lp->a.read_rap(ioaddr);
    while ((csr0 = lp->a.read_csr (ioaddr, 0)) & 0x8600 && --boguscnt >= 0) {
	/* Acknowledge all of the current interrupt sources ASAP. */
	lp->a.write_csr (ioaddr, 0, csr0 & ~0x004f);

	must_restart = 0;

	if (pcnet32_debug > 5)
	    printk(KERN_DEBUG "%s: interrupt  csr0=%#2.2x new csr=%#2.2x.\n",
		   dev->name, csr0, lp->a.read_csr (ioaddr, 0));

	if (csr0 & 0x0400)		/* Rx interrupt */
	    pcnet32_rx(dev);

	if (csr0 & 0x0200) {		/* Tx-done interrupt */
	    unsigned int dirty_tx = lp->dirty_tx;

	    while (dirty_tx < lp->cur_tx) {
		int entry = dirty_tx & TX_RING_MOD_MASK;
		int status = (short)le16_to_cpu(lp->tx_ring[entry].status);
			
		if (status < 0)
		    break;		/* It still hasn't been Txed */

		lp->tx_ring[entry].base = 0;

		if (status & 0x4000) {
		    /* There was an major error, log it. */
		    int err_status = le32_to_cpu(lp->tx_ring[entry].misc);
		    lp->stats.tx_errors++;
		    if (err_status & 0x04000000) lp->stats.tx_aborted_errors++;
		    if (err_status & 0x08000000) lp->stats.tx_carrier_errors++;
		    if (err_status & 0x10000000) lp->stats.tx_window_errors++;
#ifndef DO_DXSUFLO
		    if (err_status & 0x40000000) {
			lp->stats.tx_fifo_errors++;
			/* Ackk!  On FIFO errors the Tx unit is turned off! */
			/* Remove this verbosity later! */
			printk(KERN_ERR "%s: Tx FIFO error! CSR0=%4.4x\n",
			       dev->name, csr0);
			must_restart = 1;
		    }
#else
		    if (err_status & 0x40000000) {
			lp->stats.tx_fifo_errors++;
			if (! lp->dxsuflo) {  /* If controller doesn't recover ... */
			    /* Ackk!  On FIFO errors the Tx unit is turned off! */
			    /* Remove this verbosity later! */
			    printk(KERN_ERR "%s: Tx FIFO error! CSR0=%4.4x\n",
				   dev->name, csr0);
			    must_restart = 1;
			}
		    }
#endif
		} else {
		    if (status & 0x1800)
			lp->stats.collisions++;
		    lp->stats.tx_packets++;
		}

		/* We must free the original skb */
		if (lp->tx_skbuff[entry]) {
                    pci_unmap_single(lp->pci_dev, lp->tx_dma_addr[entry], lp->tx_skbuff[entry]->len, PCI_DMA_TODEVICE);
		    dev_kfree_skb_irq(lp->tx_skbuff[entry]);
		    lp->tx_skbuff[entry] = 0;
                    lp->tx_dma_addr[entry] = 0;
		}
		dirty_tx++;
	    }

#ifndef final_version
	    if (lp->cur_tx - dirty_tx >= TX_RING_SIZE) {
		printk(KERN_ERR "out-of-sync dirty pointer, %d vs. %d, full=%d.\n",
		       dirty_tx, lp->cur_tx, lp->tx_full);
		dirty_tx += TX_RING_SIZE;
	    }
#endif
	    if (lp->tx_full &&
		netif_queue_stopped(dev) &&
		dirty_tx > lp->cur_tx - TX_RING_SIZE + 2) {
		/* The ring is no longer full, clear tbusy. */
		lp->tx_full = 0;
		netif_wake_queue (dev);
	    }
	    lp->dirty_tx = dirty_tx;
	}

	/* Log misc errors. */
	if (csr0 & 0x4000) lp->stats.tx_errors++; /* Tx babble. */
	if (csr0 & 0x1000) {
	    /*
	     * this happens when our receive ring is full. This shouldn't
	     * be a problem as we will see normal rx interrupts for the frames
	     * in the receive ring. But there are some PCI chipsets (I can reproduce
	     * this on SP3G with Intel saturn chipset) which have sometimes problems
	     * and will fill up the receive ring with error descriptors. In this
	     * situation we don't get a rx interrupt, but a missed frame interrupt sooner
	     * or later. So we try to clean up our receive ring here.
	     */
	    pcnet32_rx(dev);
	    lp->stats.rx_errors++; /* Missed a Rx frame. */
	}
	if (csr0 & 0x0800) {
	    printk(KERN_ERR "%s: Bus master arbitration failure, status %4.4x.\n",
		   dev->name, csr0);
	    /* unlike for the lance, there is no restart needed */
	}

	if (must_restart) {
	    /* stop the chip to clear the error condition, then restart */
	    lp->a.write_csr (ioaddr, 0, 0x0004);
	    pcnet32_restart(dev, 0x0002);
	}
    }

    /* Clear any other interrupt, and set interrupt enable. */
    lp->a.write_csr (ioaddr, 0, 0x7940);
    lp->a.write_rap(ioaddr,rap);
    
    if (pcnet32_debug > 4)
	printk(KERN_DEBUG "%s: exiting interrupt, csr0=%#4.4x.\n",
	       dev->name, lp->a.read_csr (ioaddr, 0));

    spin_unlock(&lp->lock);
}

static int
pcnet32_rx(struct net_device *dev)
{
    struct pcnet32_private *lp = (struct pcnet32_private *)dev->priv;
    int entry = lp->cur_rx & RX_RING_MOD_MASK;

    /* If we own the next entry, it's a new packet. Send it up. */
    while ((short)le16_to_cpu(lp->rx_ring[entry].status) >= 0) {
	int status = (short)le16_to_cpu(lp->rx_ring[entry].status) >> 8;

	if (status != 0x03) {			/* There was an error. */
	    /* 
	     * There is a tricky error noted by John Murphy,
	     * <murf@perftech.com> to Russ Nelson: Even with full-sized
	     * buffers it's possible for a jabber packet to use two
	     * buffers, with only the last correctly noting the error.
	     */
	    if (status & 0x01)	/* Only count a general error at the */
		lp->stats.rx_errors++; /* end of a packet.*/
	    if (status & 0x20) lp->stats.rx_frame_errors++;
	    if (status & 0x10) lp->stats.rx_over_errors++;
	    if (status & 0x08) lp->stats.rx_crc_errors++;
	    if (status & 0x04) lp->stats.rx_fifo_errors++;
	    lp->rx_ring[entry].status &= le16_to_cpu(0x03ff);
	} else {
	    /* Malloc up new buffer, compatible with net-2e. */
	    short pkt_len = (le32_to_cpu(lp->rx_ring[entry].msg_length) & 0xfff)-4;
	    struct sk_buff *skb;
			
	    if(pkt_len < 60) {
		printk(KERN_ERR "%s: Runt packet!\n",dev->name);
		lp->stats.rx_errors++;
	    } else {
		int rx_in_place = 0;

		if (pkt_len > rx_copybreak) {
		    struct sk_buff *newskb;
				
		    if ((newskb = dev_alloc_skb (PKT_BUF_SZ))) {
			skb_reserve (newskb, 2);
			skb = lp->rx_skbuff[entry];
			skb_put (skb, pkt_len);
			lp->rx_skbuff[entry] = newskb;
			newskb->dev = dev;
                        lp->rx_dma_addr[entry] = pci_map_single(lp->pci_dev, newskb->tail, newskb->len, PCI_DMA_FROMDEVICE);
			lp->rx_ring[entry].base = le32_to_cpu(lp->rx_dma_addr[entry]);
			rx_in_place = 1;
		    } else
			skb = NULL;
		} else {
		    skb = dev_alloc_skb(pkt_len+2);
                }
			    
		if (skb == NULL) {
                    int i;
		    printk(KERN_ERR "%s: Memory squeeze, deferring packet.\n", dev->name);
		    for (i = 0; i < RX_RING_SIZE; i++)
			if ((short)le16_to_cpu(lp->rx_ring[(entry+i) & RX_RING_MOD_MASK].status) < 0)
			    break;

		    if (i > RX_RING_SIZE -2) {
			lp->stats.rx_dropped++;
			lp->rx_ring[entry].status |= le16_to_cpu(0x8000);
			lp->cur_rx++;
		    }
		    break;
		}
		skb->dev = dev;
		if (!rx_in_place) {
		    skb_reserve(skb,2); /* 16 byte align */
		    skb_put(skb,pkt_len);	/* Make room */
		    eth_copy_and_sum(skb,
				     (unsigned char *)(lp->rx_skbuff[entry]->tail),
				     pkt_len,0);
		}
		lp->stats.rx_bytes += skb->len;
		skb->protocol=eth_type_trans(skb,dev);
		netif_rx(skb);
		lp->stats.rx_packets++;
	    }
	}
	/*
	 * The docs say that the buffer length isn't touched, but Andrew Boyd
	 * of QNX reports that some revs of the 79C965 clear it.
	 */
	lp->rx_ring[entry].buf_length = le16_to_cpu(-PKT_BUF_SZ);
	lp->rx_ring[entry].status |= le16_to_cpu(0x8000);
	entry = (++lp->cur_rx) & RX_RING_MOD_MASK;
    }

    return 0;
}

static int
pcnet32_close(struct net_device *dev)
{
    unsigned long ioaddr = dev->base_addr;
    struct pcnet32_private *lp = (struct pcnet32_private *)dev->priv;
    int i;

    netif_stop_queue(dev);

    lp->stats.rx_missed_errors = lp->a.read_csr (ioaddr, 112);

    if (pcnet32_debug > 1)
	printk(KERN_DEBUG "%s: Shutting down ethercard, status was %2.2x.\n",
	       dev->name, lp->a.read_csr (ioaddr, 0));

    /* We stop the PCNET32 here -- it occasionally polls memory if we don't. */
    lp->a.write_csr (ioaddr, 0, 0x0004);

    /*
     * Switch back to 16bit mode to avoid problems with dumb 
     * DOS packet driver after a warm reboot
     */
    lp->a.write_bcr (ioaddr, 20, 4);

    free_irq(dev->irq, dev);
    
    /* free all allocated skbuffs */
    for (i = 0; i < RX_RING_SIZE; i++) {
	lp->rx_ring[i].status = 0;			    
	if (lp->rx_skbuff[i]) {
            pci_unmap_single(lp->pci_dev, lp->rx_dma_addr[i], lp->rx_skbuff[i]->len, PCI_DMA_FROMDEVICE);
	    dev_kfree_skb(lp->rx_skbuff[i]);
        }
	lp->rx_skbuff[i] = NULL;
        lp->rx_dma_addr[i] = 0;
    }
    
    for (i = 0; i < TX_RING_SIZE; i++) {
	if (lp->tx_skbuff[i]) {
            pci_unmap_single(lp->pci_dev, lp->tx_dma_addr[i], lp->tx_skbuff[i]->len, PCI_DMA_TODEVICE);
	    dev_kfree_skb(lp->tx_skbuff[i]);
        }
	lp->tx_skbuff[i] = NULL;
        lp->tx_dma_addr[i] = 0;
    }
    
    MOD_DEC_USE_COUNT;

    return 0;
}

static struct net_device_stats *
pcnet32_get_stats(struct net_device *dev)
{
    struct pcnet32_private *lp = (struct pcnet32_private *)dev->priv;
    unsigned long ioaddr = dev->base_addr;
    u16 saved_addr;
    unsigned long flags;

    spin_lock_irqsave(&lp->lock, flags);
    saved_addr = lp->a.read_rap(ioaddr);
    lp->stats.rx_missed_errors = lp->a.read_csr (ioaddr, 112);
    lp->a.write_rap(ioaddr, saved_addr);
    spin_unlock_irqrestore(&lp->lock, flags);

    return &lp->stats;
}

/* taken from the sunlance driver, which it took from the depca driver */
static void pcnet32_load_multicast (struct net_device *dev)
{
    struct pcnet32_private *lp = (struct pcnet32_private *) dev->priv;
    volatile struct pcnet32_init_block *ib = &lp->init_block;
    volatile u16 *mcast_table = (u16 *)&ib->filter;
    struct dev_mc_list *dmi=dev->mc_list;
    char *addrs;
    int i, j, bit, byte;
    u32 crc, poly = CRC_POLYNOMIAL_LE;
	
    /* set all multicast bits */
    if (dev->flags & IFF_ALLMULTI){ 
	ib->filter [0] = 0xffffffff;
	ib->filter [1] = 0xffffffff;
	return;
    }
    /* clear the multicast filter */
    ib->filter [0] = 0;
    ib->filter [1] = 0;

    /* Add addresses */
    for (i = 0; i < dev->mc_count; i++){
	addrs = dmi->dmi_addr;
	dmi   = dmi->next;
	
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
	mcast_table [crc >> 4] |= 1 << (crc & 0xf);
    }
    return;
}


/*
 * Set or clear the multicast filter for this adaptor.
 */
static void pcnet32_set_multicast_list(struct net_device *dev)
{
    unsigned long ioaddr = dev->base_addr;
    struct pcnet32_private *lp = (struct pcnet32_private *)dev->priv;	 

    if (dev->flags&IFF_PROMISC) {
	/* Log any net taps. */
	printk(KERN_INFO "%s: Promiscuous mode enabled.\n", dev->name);
	lp->init_block.mode = le16_to_cpu(0x8000 | (lp->options & PORT_PORTSEL) << 7);
    } else {
	lp->init_block.mode = le16_to_cpu((lp->options & PORT_PORTSEL) << 7);
	pcnet32_load_multicast (dev);
    }
    
    lp->a.write_csr (ioaddr, 0, 0x0004); /* Temporarily stop the lance. */

    pcnet32_restart(dev, 0x0042); /*  Resume normal operation */
}

#ifdef HAVE_PRIVATE_IOCTL
static int pcnet32_mii_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
    unsigned long ioaddr = dev->base_addr;
    struct pcnet32_private *lp = (struct pcnet32_private *)dev->priv;	 
    u16 *data = (u16 *)&rq->ifr_data;
    int phyaddr = lp->a.read_bcr (ioaddr, 33);

    if (lp->mii) {
	switch(cmd) {
	case SIOCDEVPRIVATE:		/* Get the address of the PHY in use. */
	    data[0] = (phyaddr >> 5) & 0x1f;
	    /* Fall Through */
	case SIOCDEVPRIVATE+1:		/* Read the specified MII register. */
	    lp->a.write_bcr (ioaddr, 33, ((data[0] & 0x1f) << 5) | (data[1] & 0x1f));
	    data[3] = lp->a.read_bcr (ioaddr, 34);
	    lp->a.write_bcr (ioaddr, 33, phyaddr);
	    return 0;
	case SIOCDEVPRIVATE+2:		/* Write the specified MII register */
	    if (!capable(CAP_NET_ADMIN))
		return -EPERM;
	    lp->a.write_bcr (ioaddr, 33, ((data[0] & 0x1f) << 5) | (data[1] & 0x1f));
	    lp->a.write_bcr (ioaddr, 34, data[2]);
	    lp->a.write_bcr (ioaddr, 33, phyaddr);
	    return 0;
	default:
	    return -EOPNOTSUPP;
	}
    }
    return -EOPNOTSUPP;
}
#endif	/* HAVE_PRIVATE_IOCTL */
					    
static struct pci_driver pcnet32_driver = {
    name:  "pcnet32",
    probe: pcnet32_probe_pci,
    remove: NULL,
    id_table: pcnet32_pci_tbl,
};

MODULE_PARM(debug, "i");
MODULE_PARM(max_interrupt_work, "i");
MODULE_PARM(rx_copybreak, "i");
MODULE_PARM(tx_start_pt, "i");
MODULE_PARM(options, "1-" __MODULE_STRING(MAX_UNITS) "i");
MODULE_PARM(full_duplex, "1-" __MODULE_STRING(MAX_UNITS) "i");
MODULE_AUTHOR("Thomas Bogendoerfer");
MODULE_DESCRIPTION("Driver for PCnet32 and PCnetPCI based ethercards");

/* An additional parameter that may be passed in... */
static int debug = -1;
static int tx_start_pt = -1;

static int __init pcnet32_init_module(void)
{
    int cards_found = 0;
    int err;

    if (debug > 0)
	pcnet32_debug = debug;
    if ((tx_start_pt >= 0) && (tx_start_pt <= 3))
	tx_start = tx_start_pt;
    
    pcnet32_dev = NULL;
    /* find the PCI devices */
#define USE_PCI_REGISTER_DRIVER
#ifdef USE_PCI_REGISTER_DRIVER
    if ((err = pci_module_init(&pcnet32_driver)) < 0 )
       return err;
#else
    {
        struct pci_device_id *devid = pcnet32_pci_tbl;
        for (devid = pcnet32_pci_tbl; devid != NULL && devid->vendor != 0; devid++) {
            struct pci_dev *pdev = pci_find_subsys(devid->vendor, devid->device, devid->subvendor, devid->subdevice, NULL);
            if (pdev != NULL) {
                if (pcnet32_probe_pci(pdev, devid) >= 0) {
                    cards_found++;
                }
            }
        }
    }
#endif
    return 0;
    /* find any remaining VLbus devices */
    return pcnet32_probe_vlbus(cards_found);
}

static void __exit pcnet32_cleanup_module(void)
{
    struct net_device *next_dev;

    /* No need to check MOD_IN_USE, as sys_delete_module() checks. */
    while (pcnet32_dev) {
        struct pcnet32_private *lp = (struct pcnet32_private *) pcnet32_dev->priv;
	next_dev = lp->next;
	unregister_netdev(pcnet32_dev);
	release_region(pcnet32_dev->base_addr, PCNET32_TOTAL_SIZE);
        pci_free_consistent(lp->pci_dev, sizeof(*lp), lp, lp->dma_addr);
	kfree(pcnet32_dev);
	pcnet32_dev = next_dev;
    }
}

module_init(pcnet32_init_module);
module_exit(pcnet32_cleanup_module);

/*
 * Local variables:
 *  compile-command: "gcc -D__KERNEL__ -I/usr/src/linux/net/inet -Wall -Wstrict-prototypes -O6 -m486 -c pcnet32.c"
 *  c-indent-level: 4
 *  tab-width: 8
 * End:
 */
