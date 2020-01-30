/* [xirc2ps_cs.c wk 03.11.99] (1.40 1999/11/18 00:06:03)
 * Xircom CreditCard Ethernet Adapter IIps driver
 * Xircom Realport 10/100 (RE-100) driver 
 *
 * This driver supports various Xircom CreditCard Ethernet adapters
 * including the CE2, CE IIps, RE-10, CEM28, CEM33, CE33, CEM56,
 * CE3-100, CE3B, RE-100, REM10BT, and REM56G-100.
 * 
 * Written originally by Werner Koch based on David Hinds' skeleton of the
 * PCMCIA driver.
 *
 * Copyright (c) 1997,1998 Werner Koch (dd9jn)
 *
 * This driver is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * It is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 *
 *
 * ALTERNATIVELY, this driver may be distributed under the terms of
 * the following license, in which case the provisions of this license
 * are required INSTEAD OF the GNU General Public License.  (This clause
 * is necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/ptrace.h>
#include <linux/malloc.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/in.h>
#include <linux/delay.h>
#include <asm/io.h>
#include <asm/system.h>
#include <asm/bitops.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/ioport.h>

#include <pcmcia/version.h>
#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/cisreg.h>
#include <pcmcia/ciscode.h>

#ifndef MANFID_COMPAQ
  #define MANFID_COMPAQ 	   0x0138
  #define MANFID_COMPAQ2	   0x0183  /* is this correct? */
#endif

#include <pcmcia/ds.h>

/* Time in jiffies before concluding Tx hung */
#define TX_TIMEOUT	((400*HZ)/1000)

/****************
 * Some constants used to access the hardware
 */

/* Register offsets and value constans */
#define XIRCREG_CR  0	/* Command register (wr) */
enum xirc_cr {
    TransmitPacket = 0x01,
    SoftReset = 0x02,
    EnableIntr = 0x04,
    ForceIntr  = 0x08,
    ClearTxFIFO = 0x10,
    ClearRxOvrun = 0x20,
    RestartTx	 = 0x40
};
#define XIRCREG_ESR 0	/* Ethernet status register (rd) */
enum xirc_esr {
    FullPktRcvd = 0x01, /* full packet in receive buffer */
    PktRejected = 0x04, /* a packet has been rejected */
    TxPktPend = 0x08,	/* TX Packet Pending */
    IncorPolarity = 0x10,
    MediaSelect = 0x20	/* set if TP, clear if AUI */
};
#define XIRCREG_PR  1	/* Page Register select */
#define XIRCREG_EDP 4	/* Ethernet Data Port Register */
#define XIRCREG_ISR 6	/* Ethernet Interrupt Status Register */
enum xirc_isr {
    TxBufOvr = 0x01,	/* TX Buffer Overflow */
    PktTxed  = 0x02,	/* Packet Transmitted */
    MACIntr  = 0x04,	/* MAC Interrupt occured */
    TxResGrant = 0x08,	/* Tx Reservation Granted */
    RxFullPkt = 0x20,	/* Rx Full Packet */
    RxPktRej  = 0x40,	/* Rx Packet Rejected */
    ForcedIntr= 0x80	/* Forced Interrupt */
};
#define XIRCREG1_IMR0 12 /* Ethernet Interrupt Mask Register (on page 1)*/
#define XIRCREG1_IMR1 13
#define XIRCREG0_TSO  8  /* Transmit Space Open Register (on page 0)*/
#define XIRCREG0_TRS  10 /* Transmit reservation Size Register (page 0)*/
#define XIRCREG0_DO   12 /* Data Offset Register (page 0) (wr) */
#define XIRCREG0_RSR  12 /* Receive Status Register (page 0) (rd) */
enum xirc_rsr {
    PhyPkt = 0x01,	/* set:physical packet, clear: multicast packet */
    BrdcstPkt = 0x02,	/* set if it is a broadcast packet */
    PktTooLong = 0x04,	/* set if packet length > 1518 */
    AlignErr = 0x10,	/* incorrect CRC and last octet not complete */
    CRCErr = 0x20,	/* incorrect CRC and last octet is complete */
    PktRxOk = 0x80	/* received ok */
};
#define XIRCREG0_PTR 13 /* packets transmitted register (rd) */
#define XIRCREG0_RBC 14 /* receive byte count regsister (rd) */
#define XIRCREG1_ECR 14 /* ethernet configurationn register */
enum xirc_ecr {
    FullDuplex = 0x04,	/* enable full duplex mode */
    LongTPMode = 0x08,	/* adjust for longer lengths of TP cable */
    DisablePolCor = 0x10,/* disable auto polarity correction */
    DisableLinkPulse = 0x20, /* disable link pulse generation */
    DisableAutoTx = 0x40, /* disable auto-transmit */
};
#define XIRCREG2_RBS 8	/* receive buffer start register */
#define XIRCREG2_LED 10 /* LED Configuration register */
/* values for the leds:    Bits 2-0 for led 1
 *  0 disabled		   Bits 5-3 for led 2
 *  1 collision
 *  2 noncollision
 *  3 link_detected
 *  4 incor_polarity
 *  5 jabber
 *  6 auto_assertion
 *  7 rx_tx_activity
 */
#define XIRCREG2_MSR 12 /* Mohawk specific register */

#define XIRCREG4_GPR0 8 /* General Purpose Register 0 */
#define XIRCREG4_GPR1 9 /* General Purpose Register 1 */
#define XIRCREG2_GPR2 13 /* General Purpose Register 2 (page2!)*/
#define XIRCREG4_BOV 10 /* Bonding Version Register */
#define XIRCREG4_LMA 12 /* Local Memory Address Register */
#define XIRCREG4_LMD 14 /* Local Memory Data Port */
/* MAC register can only by accessed with 8 bit operations */
#define XIRCREG40_CMD0 8    /* Command Register (wr) */
enum xirc_cmd { 	    /* Commands */
    Transmit = 0x01,
    EnableRecv = 0x04,
    DisableRecv = 0x08,
    Abort = 0x10,
    Online = 0x20,
    IntrAck = 0x40,
    Offline = 0x80
};
#define XIRCREG5_RHSA0	10  /* Rx Host Start Address */
#define XIRCREG40_RXST0 9   /* Receive Status Register */
#define XIRCREG40_TXST0 11  /* Transmit Status Register 0 */
#define XIRCREG40_TXST1 12  /* Transmit Status Register 10 */
#define XIRCREG40_RMASK0 13  /* Receive Mask Register */
#define XIRCREG40_TMASK0 14  /* Transmit Mask Register 0 */
#define XIRCREG40_TMASK1 15  /* Transmit Mask Register 0 */
#define XIRCREG42_SWC0	8   /* Software Configuration 0 */
#define XIRCREG42_SWC1	9   /* Software Configuration 1 */
#define XIRCREG42_BOC	10  /* Back-Off Configuration */
#define XIRCREG44_TDR0	8   /* Time Domain Reflectometry 0 */
#define XIRCREG44_TDR1	9   /* Time Domain Reflectometry 1 */
#define XIRCREG44_RXBC_LO 10 /* Rx Byte Count 0 (rd) */
#define XIRCREG44_RXBC_HI 11 /* Rx Byte Count 1 (rd) */
#define XIRCREG45_REV	 15 /* Revision Register (rd) */
#define XIRCREG50_IA	8   /* Individual Address (8-13) */

static char *if_names[] = { "Auto", "10BaseT", "10Base2", "AUI", "100BaseT" };

/****************
 * All the PCMCIA modules use PCMCIA_DEBUG to control debugging.  If
 * you do not define PCMCIA_DEBUG at all, all the debug code will be
 * left out.  If you compile with PCMCIA_DEBUG=0, the debug code will
 * be present but disabled -- but it can then be enabled for specific
 * modules at load time with a 'pc_debug=#' option to insmod.
 */
#ifdef PCMCIA_DEBUG
static int pc_debug = PCMCIA_DEBUG;
MODULE_PARM(pc_debug, "i");
#define DEBUG(n, args...) if (pc_debug>(n)) printk(KDBG_XIRC args)
#else
#define DEBUG(n, args...)
#endif
static char *version =
"xirc2ps_cs.c 1.31 1998/12/09 19:32:55 (dd9jn+kvh)";
	    /* !--- CVS revision */
#define KDBG_XIRC KERN_DEBUG   "xirc2ps_cs: "
#define KERR_XIRC KERN_ERR     "xirc2ps_cs: "
#define KWRN_XIRC KERN_WARNING "xirc2ps_cs: "
#define KNOT_XIRC KERN_NOTICE  "xirc2ps_cs: "
#define KINF_XIRC KERN_INFO    "xirc2ps_cs: "

/* card types */
#define XIR_UNKNOWN  0	/* unknown: not supported */
#define XIR_CE	     1	/* (prodid 1) different hardware: not supported */
#define XIR_CE2      2	/* (prodid 2) */
#define XIR_CE3      3	/* (prodid 3) */
#define XIR_CEM      4	/* (prodid 1) different hardware: not supported */
#define XIR_CEM2     5	/* (prodid 2) */
#define XIR_CEM3     6	/* (prodid 3) */
#define XIR_CEM33    7	/* (prodid 4) */
#define XIR_CEM56M   8	/* (prodid 5) */
#define XIR_CEM56    9	/* (prodid 6) */
#define XIR_CM28    10	/* (prodid 3) modem only: not supported here */
#define XIR_CM33    11	/* (prodid 4) modem only: not supported here */
#define XIR_CM56    12	/* (prodid 5) modem only: not supported here */
#define XIR_CG	    13	/* (prodid 1) GSM modem only: not supported */
#define XIR_CBE     14	/* (prodid 1) cardbus ethernet: not supported */
/*====================================================================*/

/* Parameters that can be set with 'insmod' */

#define INT_MODULE_PARM(n, v) static int n = v; MODULE_PARM(n, "i")

static int irq_list[4] = { -1 };
MODULE_PARM(irq_list, "1-4i");
INT_MODULE_PARM(irq_mask,	0xdeb8);
INT_MODULE_PARM(if_port,	0);
INT_MODULE_PARM(full_duplex,	0);
INT_MODULE_PARM(do_sound, 	1);
INT_MODULE_PARM(lockup_hack,	0);  /* anti lockup hack */

/*====================================================================*/

/* We do not process more than these number of bytes during one
 * interrupt. (Of course we receive complete packets, so this is not
 * an exact value).
 * Something between 2000..22000; first value gives best interrupt latency,
 * the second enables the usage of the complete on-chip buffer. We use the
 * high value as the initial value.
 */
static unsigned maxrx_bytes = 22000;

/* MII management prototypes */
static void mii_idle(ioaddr_t ioaddr);
static void mii_putbit(ioaddr_t ioaddr, unsigned data);
static int  mii_getbit(ioaddr_t ioaddr);
static void mii_wbits(ioaddr_t ioaddr, unsigned data, int len);
static unsigned mii_rd(ioaddr_t ioaddr,	u_char phyaddr, u_char phyreg);
static void mii_wr(ioaddr_t ioaddr, u_char phyaddr, u_char phyreg,
		   unsigned data, int len);

/*
 * The event() function is this driver's Card Services event handler.
 * It will be called by Card Services when an appropriate card status
 * event is received.  The config() and release() entry points are
 * used to configure or release a socket, in response to card insertion
 * and ejection events.  They are invoked from the event handler.
 */

static int has_ce2_string(dev_link_t * link);
static void xirc2ps_config(dev_link_t * link);
static void xirc2ps_release(u_long arg);
static int xirc2ps_event(event_t event, int priority,
			 event_callback_args_t * args);

/****************
 * The attach() and detach() entry points are used to create and destroy
 * "instances" of the driver, where each instance represents everything
 * needed to manage one actual PCMCIA card.
 */

static dev_link_t *xirc2ps_attach(void);
static void xirc2ps_detach(dev_link_t *);

/****************
 * You'll also need to prototype all the functions that will actually
 * be used to talk to your device.  See 'pcmem_cs' for a good example
 * of a fully self-sufficient driver; the other drivers rely more or
 * less on other parts of the kernel.
 */

static void xirc2ps_interrupt(int irq, void *dev_id, struct pt_regs *regs);

/*
 * The dev_info variable is the "key" that is used to match up this
 * device driver with appropriate cards, through the card configuration
 * database.
 */

static dev_info_t dev_info = "xirc2ps_cs";

/****************
 * A linked list of "instances" of the device.  Each actual
 * PCMCIA card corresponds to one device instance, and is described
 * by one dev_link_t structure (defined in ds.h).
 *
 * You may not want to use a linked list for this -- for example, the
 * memory card driver uses an array of dev_link_t pointers, where minor
 * device numbers are used to derive the corresponding array index.
 */

static dev_link_t *dev_list = NULL;

/****************
 * A dev_link_t structure has fields for most things that are needed
 * to keep track of a socket, but there will usually be some device
 * specific information that also needs to be kept track of.  The
 * 'priv' pointer in a dev_link_t structure can be used to point to
 * a device-specific private data structure, like this.
 *
 * A driver needs to provide a dev_node_t structure for each device
 * on a card.  In some cases, there is only one device per card (for
 * example, ethernet cards, modems).  In other cases, there may be
 * many actual or logical devices (SCSI adapters, memory cards with
 * multiple partitions).  The dev_node_t structures need to be kept
 * in a linked list starting at the 'dev' field of a dev_link_t
 * structure.  We allocate them in the card's private data structure,
 * because they generally can't be allocated dynamically.
 */

typedef struct local_info_t {
    dev_link_t link;
    struct net_device dev;
    dev_node_t node;
    struct net_device_stats stats;
    int card_type;
    int probe_port;
    int silicon; /* silicon revision. 0=old CE2, 1=Scipper, 4=Mohawk */
    int mohawk;  /* a CE3 type card */
    int dingo;	 /* a CEM56 type card */
    int new_mii; /* has full 10baseT/100baseT MII */
    int modem;	 /* is a multi function card (i.e with a modem) */
    caddr_t dingo_ccr; /* only used for CEM56 cards */
    unsigned last_ptr_value; /* last packets transmitted value */
    const char *manf_str;
} local_info_t;

/****************
 * Some more prototypes
 */
static int do_start_xmit(struct sk_buff *skb, struct net_device *dev);
static void do_tx_timeout(struct net_device *dev);
static struct net_device_stats *do_get_stats(struct net_device *dev);
static void set_addresses(struct net_device *dev);
static void set_multicast_list(struct net_device *dev);
static int set_card_type(dev_link_t *link, const void *s);
static int do_config(struct net_device *dev, struct ifmap *map);
static int do_open(struct net_device *dev);
static int do_ioctl(struct net_device *dev, struct ifreq *rq, int cmd);
static void hardreset(struct net_device *dev);
static void do_reset(struct net_device *dev, int full);
static int init_mii(struct net_device *dev);
static void do_powerdown(struct net_device *dev);
static int do_stop(struct net_device *dev);

/*=============== Helper functions =========================*/
static void
flush_stale_links(void)
{
    dev_link_t *link, *next;
    for (link = dev_list; link; link = next) {
	next = link->next;
	if (link->state & DEV_STALE_LINK)
	    xirc2ps_detach(link);
    }
}

static void
cs_error(client_handle_t handle, int func, int ret)
{
    error_info_t err = { func, ret };
    CardServices(ReportError, handle, &err);
}

static int
get_tuple_data(int fn, client_handle_t handle, tuple_t *tuple)
{
    int err;

    if ((err=CardServices(fn, handle, tuple)))
	return err;
    return CardServices(GetTupleData, handle, tuple);
}

static int
get_tuple(int fn, client_handle_t handle, tuple_t *tuple, cisparse_t *parse)
{
    int err;

    if ((err=get_tuple_data(fn, handle, tuple)))
	return err;
    return CardServices(ParseTuple, handle, tuple, parse);
}

#define first_tuple(a, b, c) get_tuple(GetFirstTuple, a, b, c)
#define next_tuple(a, b, c)  get_tuple(GetNextTuple, a, b, c)

#define SelectPage(pgnr)   outb((pgnr), ioaddr + XIRCREG_PR)
#define GetByte(reg)	   ((unsigned)inb(ioaddr + (reg)))
#define GetWord(reg)	   ((unsigned)inw(ioaddr + (reg)))
#define PutByte(reg,value) outb((value), ioaddr+(reg))
#define PutWord(reg,value) outw((value), ioaddr+(reg))

static void
busy_loop(u_long len)
{
    if (in_interrupt()) {
	u_long timeout = jiffies + len;
	u_long flags;
	save_flags(flags);
	sti();
	while (timeout >= jiffies)
	    ;
	restore_flags(flags);
    } else {
	__set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(len);
    }
}

/*====== Functions used for debugging =================================*/
#if defined(PCMCIA_DEBUG) && 0 /* reading regs may change system status */
static void
PrintRegisters(struct net_device *dev)
{
    ioaddr_t ioaddr = dev->base_addr;

    if (pc_debug > 1) {
	int i, page;

	printk(KDBG_XIRC "Register  common: ");
	for (i = 0; i < 8; i++)
	    printk(" %2.2x", GetByte(i));
	printk("\n");
	for (page = 0; page <= 8; page++) {
	    printk(KDBG_XIRC "Register page %2x: ", page);
	    SelectPage(page);
	    for (i = 8; i < 16; i++)
		printk(" %2.2x", GetByte(i));
	    printk("\n");
	}
	for (page=0x40 ; page <= 0x5f; page++) {
	    if (page == 0x43 || (page >= 0x46 && page <= 0x4f)
		|| (page >= 0x51 && page <=0x5e))
		continue;
	    printk(KDBG_XIRC "Register page %2x: ", page);
	    SelectPage(page);
	    for (i = 8; i < 16; i++)
		printk(" %2.2x", GetByte(i));
	    printk("\n");
	}
    }
}
#endif /* PCMCIA_DEBUG */

/*============== MII Management functions ===============*/

/****************
 * Turn around for read
 */
static void
mii_idle(ioaddr_t ioaddr)
{
    PutByte(XIRCREG2_GPR2, 0x04|0); /* drive MDCK low */
    udelay(1);
    PutByte(XIRCREG2_GPR2, 0x04|1); /* and drive MDCK high */
    udelay(1);
}

/****************
 * Write a bit to MDI/O
 */
static void
mii_putbit(ioaddr_t ioaddr, unsigned data)
{
  #if 1
    if (data) {
	PutByte(XIRCREG2_GPR2, 0x0c|2|0); /* set MDIO */
	udelay(1);
	PutByte(XIRCREG2_GPR2, 0x0c|2|1); /* and drive MDCK high */
	udelay(1);
    } else {
	PutByte(XIRCREG2_GPR2, 0x0c|0|0); /* clear MDIO */
	udelay(1);
	PutByte(XIRCREG2_GPR2, 0x0c|0|1); /* and drive MDCK high */
	udelay(1);
    }
  #else
    if (data) {
	PutWord(XIRCREG2_GPR2-1, 0x0e0e);
	udelay(1);
	PutWord(XIRCREG2_GPR2-1, 0x0f0f);
	udelay(1);
    } else {
	PutWord(XIRCREG2_GPR2-1, 0x0c0c);
	udelay(1);
	PutWord(XIRCREG2_GPR2-1, 0x0d0d);
	udelay(1);
    }
  #endif
}

/****************
 * Get a bit from MDI/O
 */
static int
mii_getbit(ioaddr_t ioaddr)
{
    unsigned d;

    PutByte(XIRCREG2_GPR2, 4|0); /* drive MDCK low */
    udelay(1);
    d = GetByte(XIRCREG2_GPR2); /* read MDIO */
    PutByte(XIRCREG2_GPR2, 4|1); /* drive MDCK high again */
    udelay(1);
    return d & 0x20; /* read MDIO */
}

static void
mii_wbits(ioaddr_t ioaddr, unsigned data, int len)
{
    unsigned m = 1 << (len-1);
    for (; m; m >>= 1)
	mii_putbit(ioaddr, data & m);
}

static unsigned
mii_rd(ioaddr_t ioaddr,	u_char phyaddr, u_char phyreg)
{
    int i;
    unsigned data=0, m;

    SelectPage(2);
    for (i=0; i < 32; i++)		/* 32 bit preamble */
	mii_putbit(ioaddr, 1);
    mii_wbits(ioaddr, 0x06, 4); 	/* Start and opcode for read */
    mii_wbits(ioaddr, phyaddr, 5);	/* PHY address to be accessed */
    mii_wbits(ioaddr, phyreg, 5);	/* PHY register to read */
    mii_idle(ioaddr);			/* turn around */
    mii_getbit(ioaddr);

    for (m = 1<<15; m; m >>= 1)
	if (mii_getbit(ioaddr))
	    data |= m;
    mii_idle(ioaddr);
    return data;
}

static void
mii_wr(ioaddr_t ioaddr, u_char phyaddr, u_char phyreg, unsigned data, int len)
{
    int i;

    SelectPage(2);
    for (i=0; i < 32; i++)		/* 32 bit preamble */
	mii_putbit(ioaddr, 1);
    mii_wbits(ioaddr, 0x05, 4); 	/* Start and opcode for write */
    mii_wbits(ioaddr, phyaddr, 5);	/* PHY address to be accessed */
    mii_wbits(ioaddr, phyreg, 5);	/* PHY Register to write */
    mii_putbit(ioaddr, 1);		/* turn around */
    mii_putbit(ioaddr, 0);
    mii_wbits(ioaddr, data, len);	/* And write the data */
    mii_idle(ioaddr);
}

/*============= Main bulk of functions	=========================*/

/****************
 * xirc2ps_attach() creates an "instance" of the driver, allocating
 * local data structures for one device.  The device is registered
 * with Card Services.
 *
 * The dev_link structure is initialized, but we don't actually
 * configure the card at this point -- we wait until we receive a
 * card insertion event.
 */

static dev_link_t *
xirc2ps_attach(void)
{
    client_reg_t client_reg;
    dev_link_t *link;
    struct net_device *dev;
    local_info_t *local;
    int err;

    DEBUG(0, "attach()\n");
    flush_stale_links();

    /* Allocate the device structure */
    local = kmalloc(sizeof(*local), GFP_KERNEL);
    if (!local) return NULL;
    memset(local, 0, sizeof(*local));
    link = &local->link; dev = &local->dev;
    link->priv = dev->priv = local;

    link->release.function = &xirc2ps_release;
    link->release.data = (u_long) link;

    /* General socket configuration */
    link->conf.Attributes = CONF_ENABLE_IRQ;
    link->conf.Vcc = 50;
    link->conf.IntType = INT_MEMORY_AND_IO;
    link->conf.ConfigIndex = 1;
    link->conf.Present = PRESENT_OPTION;
    link->irq.Handler = xirc2ps_interrupt;
    link->irq.Instance = dev;

    /* Fill in card specific entries */
    dev->hard_start_xmit = &do_start_xmit;
    dev->set_config = &do_config;
    dev->get_stats = &do_get_stats;
    dev->do_ioctl = &do_ioctl;
    dev->set_multicast_list = &set_multicast_list;
    ether_setup(dev);
    dev->open = &do_open;
    dev->stop = &do_stop;
    dev->tx_timeout = do_tx_timeout;
    dev->watchdog_timeo = TX_TIMEOUT;

    /* Register with Card Services */
    link->next = dev_list;
    dev_list = link;
    client_reg.dev_info = &dev_info;
    client_reg.Attributes = INFO_IO_CLIENT | INFO_CARD_SHARE;
    client_reg.EventMask =
	CS_EVENT_CARD_INSERTION | CS_EVENT_CARD_REMOVAL |
	CS_EVENT_RESET_PHYSICAL | CS_EVENT_CARD_RESET |
	CS_EVENT_PM_SUSPEND | CS_EVENT_PM_RESUME;
    client_reg.event_handler = &xirc2ps_event;
    client_reg.Version = 0x0210;
    client_reg.event_callback_args.client_data = link;
    if ((err = CardServices(RegisterClient, &link->handle, &client_reg))) {
	cs_error(link->handle, RegisterClient, err);
	xirc2ps_detach(link);
	return NULL;
    }

    return link;
} /* xirc2ps_attach */

/****************
 *  This deletes a driver "instance".  The device is de-registered
 *  with Card Services.  If it has been released, all local data
 *  structures are freed.  Otherwise, the structures will be freed
 *  when the device is released.
 */

static void
xirc2ps_detach(dev_link_t * link)
{
    local_info_t *local = link->priv;
    dev_link_t **linkp;

    DEBUG(0, "detach(0x%p)\n", link);

    /* Locate device structure */
    for (linkp = &dev_list; *linkp; linkp = &(*linkp)->next)
	if (*linkp == link)
	    break;
    if (!*linkp) {
	DEBUG(0, "detach(0x%p): dev_link lost\n", link);
	return;
    }

    /*
     * If the device is currently configured and active, we won't
     * actually delete it yet.	Instead, it is marked so that when
     * the release() function is called, that will trigger a proper
     * detach().
     */
    del_timer(&link->release);
    if (link->state & DEV_CONFIG) {
	DEBUG(0, "detach postponed, '%s' still locked\n",
	      link->dev->dev_name);
	link->state |= DEV_STALE_LINK;
	return;
    }

    /* Break the link with Card Services */
    if (link->handle)
	CardServices(DeregisterClient, link->handle);

    /* Unlink device structure, free it */
    *linkp = link->next;
    if (link->dev)
	unregister_netdev(&local->dev);
    kfree(local);

} /* xirc2ps_detach */

/****************
 * Detect the type of the card. s is the buffer with the data of tuple 0x20
 * Returns: 0 := not supported
 *		       mediaid=11 and prodid=47
 * Media-Id bits:
 *  Ethernet	    0x01
 *  Tokenring	    0x02
 *  Arcnet	    0x04
 *  Wireless	    0x08
 *  Modem	    0x10
 *  GSM only	    0x20
 * Prod-Id bits:
 *  Pocket	    0x10
 *  External	    0x20
 *  Creditcard	    0x40
 *  Cardbus	    0x80
 *
 */
static int
set_card_type(dev_link_t *link, const void *s)
{
    local_info_t *local = link->priv;
  #ifdef PCMCIA_DEBUG
    unsigned cisrev = ((const unsigned char *)s)[2];
  #endif
    unsigned mediaid= ((const unsigned char *)s)[3];
    unsigned prodid = ((const unsigned char *)s)[4];

    DEBUG(0, "cisrev=%02x mediaid=%02x prodid=%02x\n",
	  cisrev, mediaid, prodid);

    local->mohawk = 0;
    local->dingo = 0;
    local->modem = 0;
    local->card_type = XIR_UNKNOWN;
    if (!(prodid & 0x40)) {
	printk(KNOT_XIRC "Ooops: Not a creditcard\n");
	return 0;
    }
    if (!(mediaid & 0x01)) {
	printk(KNOT_XIRC "Not an Ethernet card\n");
	return 0;
    }
    if (mediaid & 0x10) {
	local->modem = 1;
	switch(prodid & 15) {
	  case 1: local->card_type = XIR_CEM   ; break;
	  case 2: local->card_type = XIR_CEM2  ; break;
	  case 3: local->card_type = XIR_CEM3  ; break;
	  case 4: local->card_type = XIR_CEM33 ; break;
	  case 5: local->card_type = XIR_CEM56M;
		  local->mohawk = 1;
		  break;
	  case 6:
	  case 7: /* 7 is the RealPort 10/56 */
		  local->card_type = XIR_CEM56 ;
		  local->mohawk = 1;
		  local->dingo = 1;
		  break;
	}
    } else {
	switch(prodid & 15) {
	  case 1: local->card_type = has_ce2_string(link)? XIR_CE2 : XIR_CE ;
		  break;
	  case 2: local->card_type = XIR_CE2; break;
	  case 3: local->card_type = XIR_CE3;
		  local->mohawk = 1;
		  break;
	}
    }
    if (local->card_type == XIR_CE || local->card_type == XIR_CEM) {
	printk(KNOT_XIRC "Sorry, this is an old CE card\n");
	return 0;
    }
    if (local->card_type == XIR_UNKNOWN)
	printk(KNOT_XIRC "unknown card (mediaid=%02x prodid=%02x)\n",
	       mediaid, prodid);

    return 1;
}

/****************
 * There are some CE2 cards out which claim to be a CE card.
 * This function looks for a "CE2" in the 3rd version field.
 * Returns: true if this is a CE2
 */
static int
has_ce2_string(dev_link_t * link)
{
    client_handle_t handle = link->handle;
    tuple_t tuple;
    cisparse_t parse;
    u_char buf[256];

    tuple.Attributes = 0;
    tuple.TupleData = buf;
    tuple.TupleDataMax = 254;
    tuple.TupleOffset = 0;
    tuple.DesiredTuple = CISTPL_VERS_1;
    if (!first_tuple(handle, &tuple, &parse) && parse.version_1.ns > 2) {
	if (strstr(parse.version_1.str + parse.version_1.ofs[2], "CE2"))
	    return 1;
    }
    return 0;
}

/****************
 * xirc2ps_config() is scheduled to run after a CARD_INSERTION event
 * is received, to configure the PCMCIA socket, and to make the
 * ethernet device available to the system.
 */
static void
xirc2ps_config(dev_link_t * link)
{
    client_handle_t handle = link->handle;
    local_info_t *local = link->priv;
    struct net_device *dev = &local->dev;
    tuple_t tuple;
    cisparse_t parse;
    ioaddr_t ioaddr;
    int err, i;
    u_char buf[64];
    cistpl_lan_node_id_t *node_id = (cistpl_lan_node_id_t*)parse.funce.data;
    cistpl_cftable_entry_t *cf = &parse.cftable_entry;

    local->dingo_ccr = 0;

    DEBUG(0, "config(0x%p)\n", link);

    /*
     * This reads the card's CONFIG tuple to find its configuration
     * registers.
     */
    tuple.Attributes = 0;
    tuple.TupleData = buf;
    tuple.TupleDataMax = 64;
    tuple.TupleOffset = 0;

    /* Is this a valid	card */
    tuple.DesiredTuple = CISTPL_MANFID;
    if ((err=first_tuple(handle, &tuple, &parse))) {
	printk(KNOT_XIRC "manfid not found in CIS\n");
	goto failure;
    }

    switch(parse.manfid.manf) {
      case MANFID_XIRCOM:
	local->manf_str = "Xircom";
	break;
      case MANFID_ACCTON:
	local->manf_str = "Accton";
	break;
      case MANFID_COMPAQ:
      case MANFID_COMPAQ2:
	local->manf_str = "Compaq";
	break;
      case MANFID_INTEL:
	local->manf_str = "Intel";
	break;
      case MANFID_TOSHIBA:
	local->manf_str = "Toshiba";
	break;
      default:
	printk(KNOT_XIRC "Unknown Card Manufacturer ID: 0x%04x\n",
	       (unsigned)parse.manfid.manf);
	goto failure;
    }
    DEBUG(0, "found %s card\n", local->manf_str);

    if (!set_card_type(link, buf)) {
	printk(KNOT_XIRC "this card is not supported\n");
	goto failure;
    }

    /* get configuration stuff */
    tuple.DesiredTuple = CISTPL_CONFIG;
    if ((err=first_tuple(handle, &tuple, &parse)))
	goto cis_error;
    link->conf.ConfigBase = parse.config.base;
    link->conf.Present =    parse.config.rmask[0];

    /* get the ethernet address from the CIS */
    tuple.DesiredTuple = CISTPL_FUNCE;
    for (err = first_tuple(handle, &tuple, &parse); !err;
			     err = next_tuple(handle, &tuple, &parse)) {
	/* Once I saw two CISTPL_FUNCE_LAN_NODE_ID entries:
	 * the first one with a length of zero the second correct -
	 * so I skip all entries with length 0 */
	if (parse.funce.type == CISTPL_FUNCE_LAN_NODE_ID
	    && ((cistpl_lan_node_id_t *)parse.funce.data)->nb)
	    break;
    }
    if (err) { /* not found: try to get the node-id from tuple 0x89 */
	tuple.DesiredTuple = 0x89;  /* data layout looks like tuple 0x22 */
	if (!(err = get_tuple_data(GetFirstTuple, handle, &tuple))) {
	    if (tuple.TupleDataLen == 8 && *buf == CISTPL_FUNCE_LAN_NODE_ID)
		memcpy(&parse, buf, 8);
	    else
		err = -1;
	}
    }
    if (err) { /* another try	(James Lehmer's CE2 version 4.1)*/
	tuple.DesiredTuple = CISTPL_FUNCE;
	for (err = first_tuple(handle, &tuple, &parse); !err;
				 err = next_tuple(handle, &tuple, &parse)) {
	    if (parse.funce.type == 0x02 && parse.funce.data[0] == 1
		&& parse.funce.data[1] == 6 && tuple.TupleDataLen == 13) {
		buf[1] = 4;
		memcpy(&parse, buf+1, 8);
		break;
	    }
	}
    }
    if (err) {
	printk(KNOT_XIRC "node-id not found in CIS\n");
	goto failure;
    }
    node_id = (cistpl_lan_node_id_t *)parse.funce.data;
    if (node_id->nb != 6) {
	printk(KNOT_XIRC "malformed node-id in CIS\n");
	goto failure;
    }
    for (i=0; i < 6; i++)
	dev->dev_addr[i] = node_id->id[i];

    /* Configure card */
    link->state |= DEV_CONFIG;

    link->io.IOAddrLines =10;
    link->io.Attributes1 = IO_DATA_PATH_WIDTH_16;
    link->irq.Attributes = IRQ_HANDLE_PRESENT;
    link->irq.IRQInfo1 = IRQ_INFO2_VALID | IRQ_LEVEL_ID;
    if (irq_list[0] == -1)
	link->irq.IRQInfo2 = irq_mask;
    else {
	for (i = 0; i < 4; i++)
	    link->irq.IRQInfo2 |= 1 << irq_list[i];
    }
    if (local->modem) {
	int pass;

	if (do_sound) {
	    link->conf.Attributes |= CONF_ENABLE_SPKR;
	    link->conf.Status |= CCSR_AUDIO_ENA;
	}
	link->irq.Attributes |= IRQ_TYPE_DYNAMIC_SHARING|IRQ_FIRST_SHARED ;
	link->io.NumPorts2 = 8;
	link->io.Attributes2 = IO_DATA_PATH_WIDTH_8;
	if (local->dingo) {
	    /* Take the Modem IO port from the CIS and scan for a free
	     * Ethernet port */
	    link->io.NumPorts1 = 16; /* no Mako stuff anymore */
	    tuple.DesiredTuple = CISTPL_CFTABLE_ENTRY;
	    for (err = first_tuple(handle, &tuple, &parse); !err;
				 err = next_tuple(handle, &tuple, &parse)) {
		if (cf->io.nwin > 0  &&  (cf->io.win[0].base & 0xf) == 8) {
		    for (ioaddr = 0x300; ioaddr < 0x400; ioaddr += 0x10) {
			link->conf.ConfigIndex = cf->index ;
			link->io.BasePort2 = cf->io.win[0].base;
			link->io.BasePort1 = ioaddr;
			if (!(err=CardServices(RequestIO, link->handle,
								&link->io)))
			    goto port_found;
		    }
		}
	    }
	} else {
	    link->io.NumPorts1 = 18;
	    /* We do 2 passes here: The first one uses the regular mapping and
	     * the second tries again, thereby considering that the 32 ports are
	     * mirrored every 32 bytes. Actually we use a mirrored port for
	     * the Mako if (on the first pass) the COR bit 5 is set.
	     */
	    for (pass=0; pass < 2; pass++) {
		tuple.DesiredTuple = CISTPL_CFTABLE_ENTRY;
		for (err = first_tuple(handle, &tuple, &parse); !err;
				     err = next_tuple(handle, &tuple, &parse)){
		    if (cf->io.nwin > 0  &&  (cf->io.win[0].base & 0xf) == 8){
			link->conf.ConfigIndex = cf->index ;
			link->io.BasePort2 = cf->io.win[0].base;
			link->io.BasePort1 = link->io.BasePort2
				    + (pass ? (cf->index & 0x20 ? -24:8)
					    : (cf->index & 0x20 ?   8:-24));
			if (!(err=CardServices(RequestIO, link->handle,
								&link->io)))
			    goto port_found;
		    }
		}
	    }
	    /* if special option:
	     * try to configure as Ethernet only.
	     * .... */
	}
	printk(KNOT_XIRC "no ports available\n");
    } else {
	link->irq.Attributes |= IRQ_TYPE_EXCLUSIVE;
	link->io.NumPorts1 = 16;
	for (ioaddr = 0x300; ioaddr < 0x400; ioaddr += 0x10) {
	    link->io.BasePort1 = ioaddr;
	    if (!(err=CardServices(RequestIO, link->handle, &link->io)))
		goto port_found;
	}
	link->io.BasePort1 = 0; /* let CS decide */
	if ((err=CardServices(RequestIO, link->handle, &link->io))) {
	    cs_error(link->handle, RequestIO, err);
	    goto config_error;
	}
    }
  port_found:
    if (err)
	 goto config_error;

    /****************
     * Now allocate an interrupt line.	Note that this does not
     * actually assign a handler to the interrupt.
     */
    if ((err=CardServices(RequestIRQ, link->handle, &link->irq))) {
	cs_error(link->handle, RequestIRQ, err);
	goto config_error;
    }

    /****************
     * This actually configures the PCMCIA socket -- setting up
     * the I/O windows and the interrupt mapping.
     */
    if ((err=CardServices(RequestConfiguration,
			  link->handle, &link->conf))) {
	cs_error(link->handle, RequestConfiguration, err);
	goto config_error;
    }

    if (local->dingo) {
	conf_reg_t reg;
	win_req_t req;
	memreq_t mem;

	/* Reset the modem's BAR to the correct value
	 * This is necessary because in the RequestConfiguration call,
	 * the base address of the ethernet port (BasePort1) is written
	 * to the BAR registers of the modem.
	 */
	reg.Action = CS_WRITE;
	reg.Offset = CISREG_IOBASE_0;
	reg.Value = link->io.BasePort2 & 0xff;
	if ((err = CardServices(AccessConfigurationRegister, link->handle,
				&reg))) {
	    cs_error(link->handle, AccessConfigurationRegister, err);
	    goto config_error;
	}
	reg.Action = CS_WRITE;
	reg.Offset = CISREG_IOBASE_1;
	reg.Value = (link->io.BasePort2 >> 8) & 0xff;
	if ((err = CardServices(AccessConfigurationRegister, link->handle,
				&reg))) {
	    cs_error(link->handle, AccessConfigurationRegister, err);
	    goto config_error;
	}

	/* There is no config entry for the Ethernet part which
	 * is at 0x0800. So we allocate a window into the attribute
	 * memory and write direct to the CIS registers
	 */
	req.Attributes = WIN_DATA_WIDTH_8|WIN_MEMORY_TYPE_AM|WIN_ENABLE;
	req.Base = req.Size = 0;
	req.AccessSpeed = 0;
	link->win = (window_handle_t)link->handle;
	if ((err = CardServices(RequestWindow, &link->win, &req))) {
	    cs_error(link->handle, RequestWindow, err);
	    goto config_error;
	}
	local->dingo_ccr = ioremap(req.Base,0x1000) + 0x0800;
	mem.CardOffset = 0x0;
	mem.Page = 0;
	if ((err = CardServices(MapMemPage, link->win, &mem))) {
	    cs_error(link->handle, MapMemPage, err);
	    goto config_error;
	}

	/* Setup the CCRs; there are no infos in the CIS about the Ethernet
	 * part.
	 */
	writeb(0x47, local->dingo_ccr + CISREG_COR);
	ioaddr = link->io.BasePort1;
	writeb(ioaddr & 0xff	  , local->dingo_ccr + CISREG_IOBASE_0);
	writeb((ioaddr >> 8)&0xff , local->dingo_ccr + CISREG_IOBASE_1);

      #if 0
	{
	    u_char tmp;
	    printk(KERN_INFO "ECOR:");
	    for (i=0; i < 7; i++) {
		tmp = readb(local->dingo_ccr + i*2);
		printk(" %02x", tmp);
	    }
	    printk("\n");
	    printk(KERN_INFO "DCOR:");
	    for (i=0; i < 4; i++) {
		tmp = readb(local->dingo_ccr + 0x20 + i*2);
		printk(" %02x", tmp);
	    }
	    printk("\n");
	    printk(KERN_INFO "SCOR:");
	    for (i=0; i < 10; i++) {
		tmp = readb(local->dingo_ccr + 0x40 + i*2);
		printk(" %02x", tmp);
	    }
	    printk("\n");
	}
      #endif

	writeb(0x01, local->dingo_ccr + 0x20);
	writeb(0x0c, local->dingo_ccr + 0x22);
	writeb(0x00, local->dingo_ccr + 0x24);
	writeb(0x00, local->dingo_ccr + 0x26);
	writeb(0x00, local->dingo_ccr + 0x28);
    }

    /* The if_port symbol can be set when the module is loaded */
    local->probe_port=0;
    if (!if_port) {
	local->probe_port = dev->if_port = 1;
    } else if ((if_port >= 1 && if_port <= 2) ||
	       (local->mohawk && if_port==4))
	dev->if_port = if_port;
    else
	printk(KNOT_XIRC "invalid if_port requested\n");

    /* we can now register the device with the net subsystem */
    dev->irq = link->irq.AssignedIRQ;
    dev->base_addr = link->io.BasePort1;
    if ((err=register_netdev(dev))) {
	printk(KNOT_XIRC "register_netdev() failed\n");
	goto config_error;
    }

    strcpy(local->node.dev_name, dev->name);
    link->dev = &local->node;
    link->state &= ~DEV_CONFIG_PENDING;

    if (local->dingo)
	do_reset(dev, 1); /* a kludge to make the cem56 work */

    /* give some infos about the hardware */
    printk(KERN_INFO "%s: %s: port %#3lx, irq %d, hwaddr",
	 dev->name, local->manf_str,(u_long)dev->base_addr, (int)dev->irq);
    for (i = 0; i < 6; i++)
	printk("%c%02X", i?':':' ', dev->dev_addr[i]);
    printk("\n");

    return;

  config_error:
    link->state &= ~DEV_CONFIG_PENDING;
    xirc2ps_release((u_long)link);
    return;

  cis_error:
    printk(KNOT_XIRC "unable to parse CIS\n");
  failure:
    link->state &= ~DEV_CONFIG_PENDING;
} /* xirc2ps_config */

/****************
 * After a card is removed, xirc2ps_release() will unregister the net
 * device, and release the PCMCIA configuration.  If the device is
 * still open, this will be postponed until it is closed.
 */
static void
xirc2ps_release(u_long arg)
{
    dev_link_t *link = (dev_link_t *) arg;
    local_info_t *local = link->priv;
    struct net_device *dev = &local->dev;

    DEBUG(0, "release(0x%p)\n", link);

    /*
     * If the device is currently in use, we won't release until it
     * is actually closed.
     */
    if (link->open) {
	DEBUG(0, "release postponed, '%s' "
	      "still open\n", link->dev->dev_name);
	link->state |= DEV_STALE_CONFIG;
	return;
    }

    if (link->win) {
	local_info_t *local = dev->priv;
	if (local->dingo)
	    iounmap(local->dingo_ccr - 0x0800);
	CardServices(ReleaseWindow, link->win);
    }
    CardServices(ReleaseConfiguration, link->handle);
    CardServices(ReleaseIO, link->handle, &link->io);
    CardServices(ReleaseIRQ, link->handle, &link->irq);
    link->state &= ~DEV_CONFIG;

} /* xirc2ps_release */

/*====================================================================*/

/****************
 * The card status event handler.  Mostly, this schedules other
 * stuff to run after an event is received.  A CARD_REMOVAL event
 * also sets some flags to discourage the net drivers from trying
 * to talk to the card any more.
 *
 * When a CARD_REMOVAL event is received, we immediately set a flag
 * to block future accesses to this device.  All the functions that
 * actually access the device should check this flag to make sure
 * the card is still present.
 */

static int
xirc2ps_event(event_t event, int priority,
	      event_callback_args_t * args)
{
    dev_link_t *link = args->client_data;
    local_info_t *lp = link->priv;
    struct net_device *dev = &lp->dev;

    DEBUG(0, "event(%d)\n", (int)event);

    switch (event) {
    case CS_EVENT_REGISTRATION_COMPLETE:
	DEBUG(0, "registration complete\n");
	break;
    case CS_EVENT_CARD_REMOVAL:
	link->state &= ~DEV_PRESENT;
	if (link->state & DEV_CONFIG) {
	    netif_device_detach(dev);
	    mod_timer(&link->release, jiffies + HZ/20);
	}
	break;
    case CS_EVENT_CARD_INSERTION:
	link->state |= DEV_PRESENT | DEV_CONFIG_PENDING;
	xirc2ps_config(link);
	break;
    case CS_EVENT_PM_SUSPEND:
	link->state |= DEV_SUSPEND;
	/* Fall through... */
    case CS_EVENT_RESET_PHYSICAL:
	if (link->state & DEV_CONFIG) {
	    if (link->open) {
		netif_device_detach(dev);
		do_powerdown(dev);
	    }
	    CardServices(ReleaseConfiguration, link->handle);
	}
	break;
    case CS_EVENT_PM_RESUME:
	link->state &= ~DEV_SUSPEND;
	/* Fall through... */
    case CS_EVENT_CARD_RESET:
	if (link->state & DEV_CONFIG) {
	    CardServices(RequestConfiguration, link->handle, &link->conf);
	    if (link->open) {
		do_reset(dev,1);
		netif_device_attach(dev);
	    }
	}
	break;
    }
    return 0;
} /* xirc2ps_event */

/*====================================================================*/

/****************
 * This is the Interrupt service route.
 */
static void
xirc2ps_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
    struct net_device *dev = (struct net_device *)dev_id;
    local_info_t *lp = dev->priv;
    ioaddr_t ioaddr;
    u_char saved_page;
    unsigned bytes_rcvd;
    unsigned int_status, eth_status, rx_status, tx_status;
    unsigned rsr, pktlen;
    ulong start_ticks = jiffies; /* fixme: jiffies rollover every 497 days
				  * is this something to worry about?
				  * -- on a laptop?
				  */

    if (!netif_device_present(dev))
	return;

    ioaddr = dev->base_addr;
    if (lp->mohawk) { /* must disable the interrupt */
	PutByte(XIRCREG_CR, 0);
    }

    DEBUG(6, "%s: interrupt %d at %#x.\n", dev->name, irq, ioaddr);

    saved_page = GetByte(XIRCREG_PR);
    /* Read the ISR to see whats the cause for the interrupt.
     * This also clears the interrupt flags on CE2 cards
     */
    int_status = GetByte(XIRCREG_ISR);
    bytes_rcvd = 0;
  loop_entry:
    if (int_status == 0xff) { /* card may be ejected */
	DEBUG(3, "%s: interrupt %d for dead card\n", dev->name, irq);
	goto leave;
    }
    eth_status = GetByte(XIRCREG_ESR);

    SelectPage(0x40);
    rx_status  = GetByte(XIRCREG40_RXST0);
    PutByte(XIRCREG40_RXST0, (~rx_status & 0xff));
    tx_status = GetByte(XIRCREG40_TXST0);
    tx_status |= GetByte(XIRCREG40_TXST1) << 8;
    PutByte(XIRCREG40_TXST0, 0);
    PutByte(XIRCREG40_TXST1, 0);

    DEBUG(3, "%s: ISR=%#2.2x ESR=%#2.2x RSR=%#2.2x TSR=%#4.4x\n",
	  dev->name, int_status, eth_status, rx_status, tx_status);

    /***** receive section ******/
    SelectPage(0);
    while (eth_status & FullPktRcvd) {
	rsr = GetByte(XIRCREG0_RSR);
	if (bytes_rcvd > maxrx_bytes && (rsr & PktRxOk)) {
	    /* too many bytes received during this int, drop the rest of the
	     * packets */
	    lp->stats.rx_dropped++;
	    DEBUG(2, "%s: RX drop, too much done\n", dev->name);
	    PutWord(XIRCREG0_DO, 0x8000); /* issue cmd: skip_rx_packet */
	} else if (rsr & PktRxOk) {
	    struct sk_buff *skb;

	    pktlen = GetWord(XIRCREG0_RBC);
	    bytes_rcvd += pktlen;

	    DEBUG(5, "rsr=%#02x packet_length=%u\n", rsr, pktlen);

	    skb = dev_alloc_skb(pktlen+3); /* 1 extra so we can use insw */
	    if (!skb) {
		printk(KNOT_XIRC "low memory, packet dropped (size=%u)\n",
		       pktlen);
		lp->stats.rx_dropped++;
	    } else { /* okay get the packet */
		skb_reserve(skb, 2);
		if (lp->silicon == 0 ) { /* work around a hardware bug */
		    unsigned rhsa; /* receive start address */

		    SelectPage(5);
		    rhsa = GetWord(XIRCREG5_RHSA0);
		    SelectPage(0);
		    rhsa += 3; /* skip control infos */
		    if (rhsa >= 0x8000)
			rhsa = 0;
		    if (rhsa + pktlen > 0x8000) {
			unsigned i;
			u_char *buf = skb_put(skb, pktlen);
			for (i=0; i < pktlen ; i++, rhsa++) {
			    buf[i] = GetByte(XIRCREG_EDP);
			    if (rhsa == 0x8000) {
				rhsa = 0;
				i--;
			    }
			}
		    } else {
			insw(ioaddr+XIRCREG_EDP,
				skb_put(skb, pktlen), (pktlen+1)>>1);
		    }
		}
	      #if 0
		else if (lp->mohawk) {
		    /* To use this 32 bit access we should use
		     * a manual optimized loop
		     * Also the words are swapped, we can get more
		     * performance by using 32 bit access and swapping
		     * the words in a register. Will need this for cardbus
		     *
		     * Note: don't forget to change the ALLOC_SKB to .. +3
		     */
		    unsigned i;
		    u_long *p = skb_put(skb, pktlen);
		    register u_long a;
		    ioaddr_t edpreg = ioaddr+XIRCREG_EDP-2;
		    for (i=0; i < len ; i += 4, p++) {
			a = inl(edpreg);
			__asm__("rorl $16,%0\n\t"
				:"=q" (a)
				: "0" (a));
			*p = a;
		    }
		}
	      #endif
		else {
		    insw(ioaddr+XIRCREG_EDP, skb_put(skb, pktlen),
			    (pktlen+1)>>1);
		}
		skb->protocol = eth_type_trans(skb, dev);
		skb->dev = dev;
		netif_rx(skb);
		lp->stats.rx_packets++;
		lp->stats.rx_bytes += pktlen;
		if (!(rsr & PhyPkt))
		    lp->stats.multicast++;
	    }
	    PutWord(XIRCREG0_DO, 0x8000); /* issue cmd: skip_rx_packet */
	} else {
	    DEBUG(5, "rsr=%#02x\n", rsr);
	}
	if (rsr & PktTooLong) {
	    lp->stats.rx_frame_errors++;
	    DEBUG(3, "%s: Packet too long\n", dev->name);
	}
	if (rsr & CRCErr) {
	    lp->stats.rx_crc_errors++;
	    DEBUG(3, "%s: CRC error\n", dev->name);
	}
	if (rsr & AlignErr) {
	    lp->stats.rx_fifo_errors++; /* okay ? */
	    DEBUG(3, "%s: Alignment error\n", dev->name);
	}

	/* get the new ethernet status */
	eth_status = GetByte(XIRCREG_ESR);
    }
    if (rx_status & 0x10) { /* Receive overrun */
	lp->stats.rx_over_errors++;
	PutByte(XIRCREG_CR, ClearRxOvrun);
	DEBUG(3, "receive overrun cleared\n");
    }

    /***** transmit section ******/
    if (int_status & PktTxed) {
	unsigned n, nn;

	n = lp->last_ptr_value;
	nn = GetByte(XIRCREG0_PTR);
	lp->last_ptr_value = nn;
	if (nn < n) /* rollover */
	    lp->stats.tx_packets += 256 - n;
	else if (n == nn) { /* happens sometimes - don't know why */
	    DEBUG(0, "PTR not changed?\n");
	} else
	    lp->stats.tx_packets += lp->last_ptr_value - n;
	netif_wake_queue(dev);
    }
    if (tx_status & 0x0002) {	/* Execessive collissions */
	DEBUG(0, "tx restarted due to execssive collissions\n");
	PutByte(XIRCREG_CR, RestartTx);  /* restart transmitter process */
    }
    if (tx_status & 0x0040)
	lp->stats.tx_aborted_errors++;

    /* recalculate our work chunk so that we limit the duration of this
     * ISR to about 1/10 of a second.
     * Calculate only if we received a reasonable amount of bytes.
     */
    if (bytes_rcvd > 1000) {
	u_long duration = jiffies - start_ticks;

	if (duration >= HZ/10) { /* if more than about 1/10 second */
	    maxrx_bytes = (bytes_rcvd * (HZ/10)) / duration;
	    if (maxrx_bytes < 2000)
		maxrx_bytes = 2000;
	    else if (maxrx_bytes > 22000)
		maxrx_bytes = 22000;
	    DEBUG(1, "set maxrx=%u (rcvd=%u ticks=%lu)\n",
		  maxrx_bytes, bytes_rcvd, duration);
	} else if (!duration && maxrx_bytes < 22000) {
	    /* now much faster */
	    maxrx_bytes += 2000;
	    if (maxrx_bytes > 22000)
		maxrx_bytes = 22000;
	    DEBUG(1, "set maxrx=%u\n", maxrx_bytes);
	}
    }

  leave:
    if (lockup_hack) {
	if (int_status != 0xff && (int_status = GetByte(XIRCREG_ISR)) != 0)
	    goto loop_entry;
    }
    SelectPage(saved_page);
    PutByte(XIRCREG_CR, EnableIntr);  /* re-enable interrupts */
    /* Instead of dropping packets during a receive, we could
     * force an interrupt with this command:
     *	  PutByte(XIRCREG_CR, EnableIntr|ForceIntr);
     */
} /* xirc2ps_interrupt */

/*====================================================================*/

static void
do_tx_timeout(struct net_device *dev)
{
    local_info_t *lp = dev->priv;
    printk(KERN_NOTICE "%s: transmit timed out\n", dev->name);
    lp->stats.tx_errors++;
    /* reset the card */
    do_reset(dev,1);
    dev->trans_start = jiffies;
    netif_start_queue(dev);
}

static int
do_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
    local_info_t *lp = dev->priv;
    ioaddr_t ioaddr = dev->base_addr;
    int okay;
    unsigned freespace;
    unsigned pktlen = skb? skb->len : 0;

    DEBUG(1, "do_start_xmit(skb=%p, dev=%p) len=%u\n",
	  skb, dev, pktlen);

    netif_stop_queue(dev);

    /* adjust the packet length to min. required
     * and hope that the buffer is large enough
     * to provide some random data.
     * fixme: For Mohawk we can change this by sending
     * a larger packetlen than we actually have; the chip will
     * pad this in his buffer with random bytes
     */
    if (pktlen < ETH_ZLEN)
	pktlen = ETH_ZLEN;

    SelectPage(0);
    PutWord(XIRCREG0_TRS, (u_short)pktlen+2);
    freespace = GetWord(XIRCREG0_TSO);
    okay = freespace & 0x8000;
    freespace &= 0x7fff;
    /* TRS doesn't work - (indeed it is eliminated with sil-rev 1) */
    okay = pktlen +2 < freespace;
    DEBUG(2 + (okay ? 2 : 0), "%s: avail. tx space=%u%s\n",
	  dev->name, freespace, okay ? " (okay)":" (not enough)");
    if (!okay) { /* not enough space */
	return 1;  /* upper layer may decide to requeue this packet */
    }
    /* send the packet */
    PutWord(XIRCREG_EDP, (u_short)pktlen);
    outsw(ioaddr+XIRCREG_EDP, skb->data, pktlen>>1);
    if (pktlen & 1)
	PutByte(XIRCREG_EDP, skb->data[pktlen-1]);

    if (lp->mohawk)
	PutByte(XIRCREG_CR, TransmitPacket|EnableIntr);

    dev_kfree_skb (skb);
    dev->trans_start = jiffies;
    lp->stats.tx_bytes += pktlen;
    netif_start_queue(dev);
    return 0;
}

static struct net_device_stats *
do_get_stats(struct net_device *dev)
{
    local_info_t *lp = dev->priv;

    /*	lp->stats.rx_missed_errors = GetByte(?) */
    return &lp->stats;
}

/****************
 * Set all addresses: This first one is the individual address,
 * the next 9 addresses are taken from the multicast list and
 * the rest is filled with the individual address.
 */
static void
set_addresses(struct net_device *dev)
{
    ioaddr_t ioaddr = dev->base_addr;
    local_info_t *lp = dev->priv;
    struct dev_mc_list *dmi = dev->mc_list;
    char *addr;
    int i,j,k,n;

    SelectPage(k=0x50);
    for (i=0,j=8,n=0; ; i++, j++) {
	if (i > 5) {
	    if (++n > 9)
		break;
	    i = 0;
	}
	if (j > 15) {
	    j = 8;
	    k++;
	    SelectPage(k);
	}

	if (n && n <= dev->mc_count && dmi) {
	    addr = dmi->dmi_addr;
	    dmi = dmi->next;
	} else
	    addr = dev->dev_addr;

	if (lp->mohawk)
	    PutByte(j, addr[5-i]);
	else
	    PutByte(j, addr[i]);
    }
    SelectPage(0);
}

/****************
 * Set or clear the multicast filter for this adaptor.
 * We can filter up to 9 addresses, if more are requested we set
 * multicast promiscuous mode.
 */

static void
set_multicast_list(struct net_device *dev)
{
    ioaddr_t ioaddr = dev->base_addr;

    SelectPage(0x42);
    if (dev->flags & IFF_PROMISC) { /* snoop */
	PutByte(XIRCREG42_SWC1, 0x06); /* set MPE and PME */
    } else if (dev->mc_count > 9 || (dev->flags & IFF_ALLMULTI)) {
	PutByte(XIRCREG42_SWC1, 0x06); /* set MPE */
    } else if (dev->mc_count) {
	/* the chip can filter 9 addresses perfectly */
	PutByte(XIRCREG42_SWC1, 0x00);
	SelectPage(0x40);
	PutByte(XIRCREG40_CMD0, Offline);
	set_addresses(dev);
	SelectPage(0x40);
	PutByte(XIRCREG40_CMD0, EnableRecv | Online);
    } else { /* standard usage */
	PutByte(XIRCREG42_SWC1, 0x00);
    }
    SelectPage(0);
}

static int
do_config(struct net_device *dev, struct ifmap *map)
{
    local_info_t *local = dev->priv;

    DEBUG(0, "do_config(%p)\n", dev);
    if (map->port != 255 && map->port != dev->if_port) {
	if (map->port > 4)
	    return -EINVAL;
	if (!map->port) {
	    local->probe_port = 1;
	    dev->if_port = 1;
	} else {
	    local->probe_port = 0;
	    dev->if_port = map->port;
	}
	printk(KERN_INFO "%s: switching to %s port\n",
	       dev->name, if_names[dev->if_port]);
	do_reset(dev,1);  /* not the fine way :-) */
    }
    return 0;
}

/****************
 * Open the driver
 */
static int
do_open(struct net_device *dev)
{
    local_info_t *lp = dev->priv;
    dev_link_t *link = &lp->link;

    DEBUG(0, "do_open(%p)\n", dev);

    /* Check that the PCMCIA card is still here. */
    /* Physical device present signature. */
    if (!DEV_OK(link))
	return -ENODEV;

    /* okay */
    link->open++;
    MOD_INC_USE_COUNT;

    netif_start_queue(dev);
    do_reset(dev,1);

    return 0;
}

static int
do_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
    local_info_t *local = dev->priv;
    ioaddr_t ioaddr = dev->base_addr;
    u16 *data = (u16 *)&rq->ifr_data;

    DEBUG(1, "%s: ioctl(%-.6s, %#04x) %04x %04x %04x %04x\n",
	  dev->name, rq->ifr_ifrn.ifrn_name, cmd,
	  data[0], data[1], data[2], data[3]);

    if (!local->mohawk)
	return -EOPNOTSUPP;

    switch(cmd) {
      case SIOCDEVPRIVATE:	/* Get the address of the PHY in use. */
	data[0] = 0;		/* we have only this address */
	/* fall trough */
      case SIOCDEVPRIVATE+1:	/* Read the specified MII register. */
	data[3] = mii_rd(ioaddr, data[0] & 0x1f, data[1] & 0x1f);
	break;
      case SIOCDEVPRIVATE+2:	/* Write the specified MII register */
	if (!capable(CAP_NET_ADMIN))
	    return -EPERM;
	mii_wr(ioaddr, data[0] & 0x1f, data[1] & 0x1f, data[2], 16);
	break;
      default:
	return -EOPNOTSUPP;
    }
    return 0;
}

static void
hardreset(struct net_device *dev)
{
    local_info_t *local = dev->priv;
    ioaddr_t ioaddr = dev->base_addr;

    SelectPage(4);
    udelay(1);
    PutByte(XIRCREG4_GPR1, 0);	     /* clear bit 0: power down */
    busy_loop(HZ/25);		     /* wait 40 msec */
    if (local->mohawk)
	PutByte(XIRCREG4_GPR1, 1);	 /* set bit 0: power up */
    else
	PutByte(XIRCREG4_GPR1, 1 | 4);	 /* set bit 0: power up, bit 2: AIC */
    busy_loop(HZ/50);		     /* wait 20 msec */
}

static void
do_reset(struct net_device *dev, int full)
{
    local_info_t *local = dev->priv;
    ioaddr_t ioaddr = dev->base_addr;
    unsigned value;

    DEBUG(0, "%s: do_reset(%p,%d)\n", dev? dev->name:"eth?", dev, full);

    hardreset(dev);
    PutByte(XIRCREG_CR, SoftReset); /* set */
    busy_loop(HZ/50);		     /* wait 20 msec */
    PutByte(XIRCREG_CR, 0);	     /* clear */
    busy_loop(HZ/25);		     /* wait 40 msec */
    if (local->mohawk) {
	SelectPage(4);
	/* set pin GP1 and GP2 to output  (0x0c)
	 * set GP1 to low to power up the ML6692 (0x00)
	 * set GP2 to high to power up the 10Mhz chip  (0x02)
	 */
	PutByte(XIRCREG4_GPR0, 0x0e);
    }

    /* give the circuits some time to power up */
    busy_loop(HZ/2);		/* about 500ms */

    local->last_ptr_value = 0;
    local->silicon = local->mohawk ? (GetByte(XIRCREG4_BOV) & 0x70) >> 4
				   : (GetByte(XIRCREG4_BOV) & 0x30) >> 4;

    if (local->probe_port) {
	if (!local->mohawk) {
	    SelectPage(4);
	    PutByte(XIRCREG4_GPR0, 4);
	    local->probe_port = 0;
	}
    } else if (dev->if_port == 2) { /* enable 10Base2 */
	SelectPage(0x42);
	PutByte(XIRCREG42_SWC1, 0xC0);
    } else { /* enable 10BaseT */
	SelectPage(0x42);
	PutByte(XIRCREG42_SWC1, 0x80);
    }
    busy_loop(HZ/25);		     /* wait 40 msec to let it complete */

  #ifdef PCMCIA_DEBUG
    if (pc_debug) {
	SelectPage(0);
	value = GetByte(XIRCREG_ESR);	 /* read the ESR */
	printk(KERN_DEBUG "%s: ESR is: %#02x\n", dev->name, value);
    }
  #endif

    /* setup the ECR */
    SelectPage(1);
    PutByte(XIRCREG1_IMR0, 0xff); /* allow all ints */
    PutByte(XIRCREG1_IMR1, 1	); /* and Set TxUnderrunDetect */
    value = GetByte(XIRCREG1_ECR);
  #if 0
    if (local->mohawk)
	value |= DisableLinkPulse;
    PutByte(XIRCREG1_ECR, value);
  #endif
    DEBUG(0, "%s: ECR is: %#02x\n", dev->name, value);

    SelectPage(0x42);
    PutByte(XIRCREG42_SWC0, 0x20); /* disable source insertion */

    if (local->silicon != 1) {
	/* set the local memory dividing line.
	 * The comments in the sample code say that this is only
	 * settable with the scipper version 2 which is revision 0.
	 * Always for CE3 cards
	 */
	SelectPage(2);
	PutWord(XIRCREG2_RBS, 0x2000);
    }

    if (full)
	set_addresses(dev);

    /* Hardware workaround:
     * The receive byte pointer after reset is off by 1 so we need
     * to move the offset pointer back to 0.
     */
    SelectPage(0);
    PutWord(XIRCREG0_DO, 0x2000); /* change offset command, off=0 */

    /* setup MAC IMRs and clear status registers */
    SelectPage(0x40);		     /* Bit 7 ... bit 0 */
    PutByte(XIRCREG40_RMASK0, 0xff); /* ROK, RAB, rsv, RO, CRC, AE, PTL, MP */
    PutByte(XIRCREG40_TMASK0, 0xff); /* TOK, TAB, SQE, LL, TU, JAB, EXC, CRS */
    PutByte(XIRCREG40_TMASK1, 0xb0); /* rsv, rsv, PTD, EXT, rsv,rsv,rsv, rsv*/
    PutByte(XIRCREG40_RXST0,  0x00); /* ROK, RAB, REN, RO, CRC, AE, PTL, MP */
    PutByte(XIRCREG40_TXST0,  0x00); /* TOK, TAB, SQE, LL, TU, JAB, EXC, CRS */
    PutByte(XIRCREG40_TXST1,  0x00); /* TEN, rsv, PTD, EXT, retry_counter:4  */

    if (full && local->mohawk && init_mii(dev)) {
	if (dev->if_port == 4 || local->dingo || local->new_mii) {
	    printk(KERN_INFO "%s: MII selected\n", dev->name);
	    SelectPage(2);
	    PutByte(XIRCREG2_MSR, GetByte(XIRCREG2_MSR) | 0x08);
	    busy_loop(HZ/50);
	} else {
	    printk(KERN_INFO "%s: MII detected; using 10mbs\n",
		   dev->name);
	    SelectPage(0x42);
	    if (dev->if_port == 2) /* enable 10Base2 */
		PutByte(XIRCREG42_SWC1, 0xC0);
	    else  /* enable 10BaseT */
		PutByte(XIRCREG42_SWC1, 0x80);
	    busy_loop(HZ/25);	/* wait 40 msec to let it complete */
	}
	if (full_duplex)
	    PutByte(XIRCREG1_ECR, GetByte(XIRCREG1_ECR | FullDuplex));
    } else {  /* No MII */
	SelectPage(0);
	value = GetByte(XIRCREG_ESR);	 /* read the ESR */
	dev->if_port = (value & MediaSelect) ? 1 : 2;
    }

    /* configure the LEDs */
    SelectPage(2);
    if (dev->if_port == 1 || dev->if_port == 4) /* TP: Link and Activity */
	PutByte(XIRCREG2_LED, 0x3b);
    else			      /* Coax: Not-Collision and Activity */
	PutByte(XIRCREG2_LED, 0x3a);

    if (local->dingo)
	PutByte(0x0b, 0x04); /* 100 Mbit LED */

    /* enable receiver and put the mac online */
    if (full) {
	SelectPage(0x40);
	PutByte(XIRCREG40_CMD0, EnableRecv | Online);
    }

    /* setup Ethernet IMR and enable interrupts */
    SelectPage(1);
    PutByte(XIRCREG1_IMR0, 0xff);
    udelay(1);
    SelectPage(0);
    PutByte(XIRCREG_CR, EnableIntr);
    if (local->modem && !local->dingo) { /* do some magic */
	if (!(GetByte(0x10) & 0x01))
	    PutByte(0x10, 0x11); /* unmask master-int bit */
    }

    if (full)
	printk(KERN_INFO "%s: media %s, silicon revision %d\n",
	       dev->name, if_names[dev->if_port], local->silicon);
    /* We should switch back to page 0 to avoid a bug in revision 0
     * where regs with offset below 8 can't be read after an access
     * to the MAC registers */
    SelectPage(0);
}

/****************
 * Initialize the Media-Independent-Interface
 * Returns: True if we have a good MII
 */
static int
init_mii(struct net_device *dev)
{
    local_info_t *local = dev->priv;
    ioaddr_t ioaddr = dev->base_addr;
    unsigned control, status, linkpartner;
    int i;

    status = mii_rd(ioaddr,  0, 1);
    if ((status & 0xff00) != 0x7800)
	return 0; /* No MII */

    local->new_mii = (mii_rd(ioaddr, 0, 2) != 0xffff);
    
    if (local->probe_port)
	control = 0x1000; /* auto neg */
    else if (dev->if_port == 4)
	control = 0x2000; /* no auto neg, 100mbs mode */
    else
	control = 0x0000; /* no auto neg, 10mbs mode */
    mii_wr(ioaddr,  0, 0, control, 16);
    udelay(100);
    control = mii_rd(ioaddr, 0, 0);

    if (control & 0x0400) {
	printk(KERN_NOTICE "%s can't take PHY out of isolation mode\n",
	       dev->name);
	local->probe_port = 0;
	return 0;
    }

    if (local->probe_port) {
	/* according to the DP83840A specs the auto negotiation process
	 * may take up to 3.5 sec, so we use this also for our ML6692
	 * Fixme: Better to use a timer here!
	 */
	for (i=0; i < 35; i++) {
	    busy_loop(HZ/10);	 /* wait 100 msec */
	    status = mii_rd(ioaddr,  0, 1);
	    if ((status & 0x0020) && (status & 0x0004))
		break;
	}

	if (!(status & 0x0020)) {
	    printk(KERN_INFO "%s: autonegotiation failed;"
		   " using 10mbs\n", dev->name);
	    if (!local->new_mii) {
		control = 0x0000;
		mii_wr(ioaddr,  0, 0, control, 16);
		udelay(100);
		SelectPage(0);
		dev->if_port = (GetByte(XIRCREG_ESR) & MediaSelect) ? 1 : 2;
	    }
	} else {
	    linkpartner = mii_rd(ioaddr, 0, 5);
	    printk(KERN_INFO "%s: MII link partner: %04x\n",
		   dev->name, linkpartner);
	    if (linkpartner & 0x0080) {
		dev->if_port = 4;
	    } else
		dev->if_port = 1;
	}
    }

    return 1;
}

static void
do_powerdown(struct net_device *dev)
{

    ioaddr_t ioaddr = dev->base_addr;

    DEBUG(0, "do_powerdown(%p)\n", dev);

    SelectPage(4);
    PutByte(XIRCREG4_GPR1, 0);	     /* clear bit 0: power down */
    SelectPage(0);
}

static int
do_stop(struct net_device *dev)
{
    ioaddr_t ioaddr = dev->base_addr;
    local_info_t *lp = dev->priv;
    dev_link_t *link = &lp->link;

    DEBUG(0, "do_stop(%p)\n", dev);

    if (!link)
	return -ENODEV;

    netif_stop_queue(dev);

    SelectPage(0);
    PutByte(XIRCREG_CR, 0);  /* disable interrupts */
    SelectPage(0x01);
    PutByte(XIRCREG1_IMR0, 0x00); /* forbid all ints */
    SelectPage(4);
    PutByte(XIRCREG4_GPR1, 0);	/* clear bit 0: power down */
    SelectPage(0);

    link->open--;
    if (link->state & DEV_STALE_CONFIG)
	mod_timer(&link->release, jiffies + HZ/20);

    MOD_DEC_USE_COUNT;

    return 0;
}

static int __init
init_xirc2ps_cs(void)
{
    servinfo_t serv;

    printk(KERN_INFO "%s\n", version);
    if (lockup_hack)
	printk(KINF_XIRC "lockup hack is enabled\n");
    CardServices(GetCardServicesInfo, &serv);
    if (serv.Revision != CS_RELEASE_CODE) {
	printk(KNOT_XIRC "Card Services release does not match!\n");
	return -1;
    }
    DEBUG(0, "pc_debug=%d\n", pc_debug);
    register_pccard_driver(&dev_info, &xirc2ps_attach, &xirc2ps_detach);
    return 0;
}

static void __exit
exit_xirc2ps_cs(void)
{
    DEBUG(0, "unloading\n");
    unregister_pccard_driver(&dev_info);
    while (dev_list) {
	if (dev_list->state & DEV_CONFIG)
	    xirc2ps_release((u_long)dev_list);
	if (dev_list)	/* xirc2ps_release() might already have detached... */
	    xirc2ps_detach(dev_list);
    }
}

module_init(init_xirc2ps_cs);
module_exit(exit_xirc2ps_cs);

