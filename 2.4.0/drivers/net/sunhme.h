/* $Id: sunhme.h,v 1.31 2000/11/12 10:23:30 davem Exp $
 * sunhme.h: Definitions for Sparc HME/BigMac 10/100baseT ethernet driver.
 *           Also known as the "Happy Meal".
 *
 * Copyright (C) 1996, 1999 David S. Miller (davem@redhat.com)
 */

#ifndef _SUNHME_H
#define _SUNHME_H

#include <linux/config.h>
#include <linux/pci.h>

/* Happy Meal global registers. */
#define GREG_SWRESET	0x000UL	/* Software Reset  */
#define GREG_CFG	0x004UL	/* Config Register */
#define GREG_STAT	0x108UL	/* Status          */
#define GREG_IMASK	0x10cUL	/* Interrupt Mask  */
#define GREG_REG_SIZE	0x110UL

/* Global reset register. */
#define GREG_RESET_ETX         0x01
#define GREG_RESET_ERX         0x02
#define GREG_RESET_ALL         0x03

/* Global config register. */
#define GREG_CFG_BURSTMSK      0x03
#define GREG_CFG_BURST16       0x00
#define GREG_CFG_BURST32       0x01
#define GREG_CFG_BURST64       0x02
#define GREG_CFG_64BIT         0x04
#define GREG_CFG_PARITY        0x08
#define GREG_CFG_RESV          0x10

/* Global status register. */
#define GREG_STAT_GOTFRAME     0x00000001 /* Received a frame                         */
#define GREG_STAT_RCNTEXP      0x00000002 /* Receive frame counter expired            */
#define GREG_STAT_ACNTEXP      0x00000004 /* Align-error counter expired              */
#define GREG_STAT_CCNTEXP      0x00000008 /* CRC-error counter expired                */
#define GREG_STAT_LCNTEXP      0x00000010 /* Length-error counter expired             */
#define GREG_STAT_RFIFOVF      0x00000020 /* Receive FIFO overflow                    */
#define GREG_STAT_CVCNTEXP     0x00000040 /* Code-violation counter expired           */
#define GREG_STAT_STSTERR      0x00000080 /* Test error in XIF for SQE                */
#define GREG_STAT_SENTFRAME    0x00000100 /* Transmitted a frame                      */
#define GREG_STAT_TFIFO_UND    0x00000200 /* Transmit FIFO underrun                   */
#define GREG_STAT_MAXPKTERR    0x00000400 /* Max-packet size error                    */
#define GREG_STAT_NCNTEXP      0x00000800 /* Normal-collision counter expired         */
#define GREG_STAT_ECNTEXP      0x00001000 /* Excess-collision counter expired         */
#define GREG_STAT_LCCNTEXP     0x00002000 /* Late-collision counter expired           */
#define GREG_STAT_FCNTEXP      0x00004000 /* First-collision counter expired          */
#define GREG_STAT_DTIMEXP      0x00008000 /* Defer-timer expired                      */
#define GREG_STAT_RXTOHOST     0x00010000 /* Moved from receive-FIFO to host memory   */
#define GREG_STAT_NORXD        0x00020000 /* No more receive descriptors              */
#define GREG_STAT_RXERR        0x00040000 /* Error during receive dma                 */
#define GREG_STAT_RXLATERR     0x00080000 /* Late error during receive dma            */
#define GREG_STAT_RXPERR       0x00100000 /* Parity error during receive dma          */
#define GREG_STAT_RXTERR       0x00200000 /* Tag error during receive dma             */
#define GREG_STAT_EOPERR       0x00400000 /* Transmit descriptor did not have EOP set */
#define GREG_STAT_MIFIRQ       0x00800000 /* MIF is signaling an interrupt condition  */
#define GREG_STAT_HOSTTOTX     0x01000000 /* Moved from host memory to transmit-FIFO  */
#define GREG_STAT_TXALL        0x02000000 /* Transmitted all packets in the tx-fifo   */
#define GREG_STAT_TXEACK       0x04000000 /* Error during transmit dma                */
#define GREG_STAT_TXLERR       0x08000000 /* Late error during transmit dma           */
#define GREG_STAT_TXPERR       0x10000000 /* Parity error during transmit dma         */
#define GREG_STAT_TXTERR       0x20000000 /* Tag error during transmit dma            */
#define GREG_STAT_SLVERR       0x40000000 /* PIO access got an error                  */
#define GREG_STAT_SLVPERR      0x80000000 /* PIO access got a parity error            */

/* All interesting error conditions. */
#define GREG_STAT_ERRORS       0xfc7efefc

/* Global interrupt mask register. */
#define GREG_IMASK_GOTFRAME    0x00000001 /* Received a frame                         */
#define GREG_IMASK_RCNTEXP     0x00000002 /* Receive frame counter expired            */
#define GREG_IMASK_ACNTEXP     0x00000004 /* Align-error counter expired              */
#define GREG_IMASK_CCNTEXP     0x00000008 /* CRC-error counter expired                */
#define GREG_IMASK_LCNTEXP     0x00000010 /* Length-error counter expired             */
#define GREG_IMASK_RFIFOVF     0x00000020 /* Receive FIFO overflow                    */
#define GREG_IMASK_CVCNTEXP    0x00000040 /* Code-violation counter expired           */
#define GREG_IMASK_STSTERR     0x00000080 /* Test error in XIF for SQE                */
#define GREG_IMASK_SENTFRAME   0x00000100 /* Transmitted a frame                      */
#define GREG_IMASK_TFIFO_UND   0x00000200 /* Transmit FIFO underrun                   */
#define GREG_IMASK_MAXPKTERR   0x00000400 /* Max-packet size error                    */
#define GREG_IMASK_NCNTEXP     0x00000800 /* Normal-collision counter expired         */
#define GREG_IMASK_ECNTEXP     0x00001000 /* Excess-collision counter expired         */
#define GREG_IMASK_LCCNTEXP    0x00002000 /* Late-collision counter expired           */
#define GREG_IMASK_FCNTEXP     0x00004000 /* First-collision counter expired          */
#define GREG_IMASK_DTIMEXP     0x00008000 /* Defer-timer expired                      */
#define GREG_IMASK_RXTOHOST    0x00010000 /* Moved from receive-FIFO to host memory   */
#define GREG_IMASK_NORXD       0x00020000 /* No more receive descriptors              */
#define GREG_IMASK_RXERR       0x00040000 /* Error during receive dma                 */
#define GREG_IMASK_RXLATERR    0x00080000 /* Late error during receive dma            */
#define GREG_IMASK_RXPERR      0x00100000 /* Parity error during receive dma          */
#define GREG_IMASK_RXTERR      0x00200000 /* Tag error during receive dma             */
#define GREG_IMASK_EOPERR      0x00400000 /* Transmit descriptor did not have EOP set */
#define GREG_IMASK_MIFIRQ      0x00800000 /* MIF is signaling an interrupt condition  */
#define GREG_IMASK_HOSTTOTX    0x01000000 /* Moved from host memory to transmit-FIFO  */
#define GREG_IMASK_TXALL       0x02000000 /* Transmitted all packets in the tx-fifo   */
#define GREG_IMASK_TXEACK      0x04000000 /* Error during transmit dma                */
#define GREG_IMASK_TXLERR      0x08000000 /* Late error during transmit dma           */
#define GREG_IMASK_TXPERR      0x10000000 /* Parity error during transmit dma         */
#define GREG_IMASK_TXTERR      0x20000000 /* Tag error during transmit dma            */
#define GREG_IMASK_SLVERR      0x40000000 /* PIO access got an error                  */
#define GREG_IMASK_SLVPERR     0x80000000 /* PIO access got a parity error            */

/* Happy Meal external transmitter registers. */
#define ETX_PENDING	0x00UL	/* Transmit pending/wakeup register */
#define ETX_CFG		0x04UL	/* Transmit config register         */
#define ETX_RING	0x08UL	/* Transmit ring pointer            */
#define ETX_BBASE	0x0cUL	/* Transmit buffer base             */
#define ETX_BDISP	0x10UL	/* Transmit buffer displacement     */
#define ETX_FIFOWPTR	0x14UL	/* FIFO write ptr                   */
#define ETX_FIFOSWPTR	0x18UL	/* FIFO write ptr (shadow register) */
#define ETX_FIFORPTR	0x1cUL	/* FIFO read ptr                    */
#define ETX_FIFOSRPTR	0x20UL	/* FIFO read ptr (shadow register)  */
#define ETX_FIFOPCNT	0x24UL	/* FIFO packet counter              */
#define ETX_SMACHINE	0x28UL	/* Transmitter state machine        */
#define ETX_RSIZE	0x2cUL	/* Ring descriptor size             */
#define ETX_BPTR	0x30UL	/* Transmit data buffer ptr         */
#define ETX_REG_SIZE	0x34UL

/* ETX transmit pending register. */
#define ETX_TP_DMAWAKEUP         0x00000001 /* Restart transmit dma             */

/* ETX config register. */
#define ETX_CFG_DMAENABLE        0x00000001 /* Enable transmit dma              */
#define ETX_CFG_FIFOTHRESH       0x000003fe /* Transmit FIFO threshold          */
#define ETX_CFG_IRQDAFTER        0x00000400 /* Interrupt after TX-FIFO drained  */
#define ETX_CFG_IRQDBEFORE       0x00000000 /* Interrupt before TX-FIFO drained */

#define ETX_RSIZE_SHIFT          4

/* Happy Meal external receiver registers. */
#define ERX_CFG		0x00UL	/* Receiver config register         */
#define ERX_RING	0x04UL	/* Receiver ring ptr                */
#define ERX_BPTR	0x08UL	/* Receiver buffer ptr              */
#define ERX_FIFOWPTR	0x0cUL	/* FIFO write ptr                   */
#define ERX_FIFOSWPTR	0x10UL	/* FIFO write ptr (shadow register) */
#define ERX_FIFORPTR	0x14UL	/* FIFO read ptr                    */
#define ERX_FIFOSRPTR	0x18UL	/* FIFO read ptr (shadow register)  */
#define ERX_SMACHINE	0x1cUL	/* Receiver state machine           */
#define ERX_REG_SIZE	0x20UL

/* ERX config register. */
#define ERX_CFG_DMAENABLE    0x00000001 /* Enable receive DMA        */
#define ERX_CFG_RESV1        0x00000006 /* Unused...                 */
#define ERX_CFG_BYTEOFFSET   0x00000038 /* Receive first byte offset */
#define ERX_CFG_RESV2        0x000001c0 /* Unused...                 */
#define ERX_CFG_SIZE32       0x00000000 /* Receive ring size == 32   */
#define ERX_CFG_SIZE64       0x00000200 /* Receive ring size == 64   */
#define ERX_CFG_SIZE128      0x00000400 /* Receive ring size == 128  */
#define ERX_CFG_SIZE256      0x00000600 /* Receive ring size == 256  */
#define ERX_CFG_RESV3        0x0000f800 /* Unused...                 */
#define ERX_CFG_CSUMSTART    0x007f0000 /* Offset of checksum start  */

/* I'd like a Big Mac, small fries, small coke, and SparcLinux please. */
#define BMAC_XIFCFG	0x0000UL	/* XIF config register                */
	/* 0x4-->0x204, reserved */
#define BMAC_TXSWRESET	0x208UL	/* Transmitter software reset         */
#define BMAC_TXCFG	0x20cUL	/* Transmitter config register        */
#define BMAC_IGAP1	0x210UL	/* Inter-packet gap 1                 */
#define BMAC_IGAP2	0x214UL	/* Inter-packet gap 2                 */
#define BMAC_ALIMIT	0x218UL	/* Transmit attempt limit             */
#define BMAC_STIME	0x21cUL	/* Transmit slot time                 */
#define BMAC_PLEN	0x220UL	/* Size of transmit preamble          */
#define BMAC_PPAT	0x224UL	/* Pattern for transmit preamble      */
#define BMAC_TXSDELIM	0x228UL	/* Transmit delimiter                 */
#define BMAC_JSIZE	0x22cUL	/* Jam size                           */
#define BMAC_TXMAX	0x230UL	/* Transmit max pkt size              */
#define BMAC_TXMIN	0x234UL	/* Transmit min pkt size              */
#define BMAC_PATTEMPT	0x238UL	/* Count of transmit peak attempts    */
#define BMAC_DTCTR	0x23cUL	/* Transmit defer timer               */
#define BMAC_NCCTR	0x240UL	/* Transmit normal-collision counter  */
#define BMAC_FCCTR	0x244UL	/* Transmit first-collision counter   */
#define BMAC_EXCTR	0x248UL	/* Transmit excess-collision counter  */
#define BMAC_LTCTR	0x24cUL	/* Transmit late-collision counter    */
#define BMAC_RSEED	0x250UL	/* Transmit random number seed        */
#define BMAC_TXSMACHINE	0x254UL	/* Transmit state machine             */
	/* 0x258-->0x304, reserved */
#define BMAC_RXSWRESET	0x308UL	/* Receiver software reset            */
#define BMAC_RXCFG	0x30cUL	/* Receiver config register           */
#define BMAC_RXMAX	0x310UL	/* Receive max pkt size               */
#define BMAC_RXMIN	0x314UL	/* Receive min pkt size               */
#define BMAC_MACADDR2	0x318UL	/* Ether address register 2           */
#define BMAC_MACADDR1	0x31cUL	/* Ether address register 1           */
#define BMAC_MACADDR0	0x320UL	/* Ether address register 0           */
#define BMAC_FRCTR	0x324UL	/* Receive frame receive counter      */
#define BMAC_GLECTR	0x328UL	/* Receive giant-length error counter */
#define BMAC_UNALECTR	0x32cUL	/* Receive unaligned error counter    */
#define BMAC_RCRCECTR	0x330UL	/* Receive CRC error counter          */
#define BMAC_RXSMACHINE	0x334UL	/* Receiver state machine             */
#define BMAC_RXCVALID	0x338UL	/* Receiver code violation            */
	/* 0x33c, reserved */
#define BMAC_HTABLE3	0x340UL	/* Hash table 3                       */
#define BMAC_HTABLE2	0x344UL	/* Hash table 2                       */
#define BMAC_HTABLE1	0x348UL	/* Hash table 1                       */
#define BMAC_HTABLE0	0x34cUL	/* Hash table 0                       */
#define BMAC_AFILTER2	0x350UL	/* Address filter 2                   */
#define BMAC_AFILTER1	0x354UL	/* Address filter 1                   */
#define BMAC_AFILTER0	0x358UL	/* Address filter 0                   */
#define BMAC_AFMASK	0x35cUL	/* Address filter mask                */
#define BMAC_REG_SIZE	0x360UL

/* BigMac XIF config register. */
#define BIGMAC_XCFG_ODENABLE  0x00000001 /* Output driver enable         */
#define BIGMAC_XCFG_XLBACK    0x00000002 /* Loopback-mode XIF enable     */
#define BIGMAC_XCFG_MLBACK    0x00000004 /* Loopback-mode MII enable     */
#define BIGMAC_XCFG_MIIDISAB  0x00000008 /* MII receive buffer disable   */
#define BIGMAC_XCFG_SQENABLE  0x00000010 /* SQE test enable              */
#define BIGMAC_XCFG_SQETWIN   0x000003e0 /* SQE time window              */
#define BIGMAC_XCFG_LANCE     0x00000010 /* Lance mode enable            */
#define BIGMAC_XCFG_LIPG0     0x000003e0 /* Lance mode IPG0              */

/* BigMac transmit config register. */
#define BIGMAC_TXCFG_ENABLE   0x00000001 /* Enable the transmitter       */
#define BIGMAC_TXCFG_SMODE    0x00000020 /* Enable slow transmit mode    */
#define BIGMAC_TXCFG_CIGN     0x00000040 /* Ignore transmit collisions   */
#define BIGMAC_TXCFG_FCSOFF   0x00000080 /* Do not emit FCS              */
#define BIGMAC_TXCFG_DBACKOFF 0x00000100 /* Disable backoff              */
#define BIGMAC_TXCFG_FULLDPLX 0x00000200 /* Enable full-duplex           */
#define BIGMAC_TXCFG_DGIVEUP  0x00000400 /* Don't give up on transmits   */

/* BigMac receive config register. */
#define BIGMAC_RXCFG_ENABLE   0x00000001 /* Enable the receiver             */
#define BIGMAC_RXCFG_PSTRIP   0x00000020 /* Pad byte strip enable           */
#define BIGMAC_RXCFG_PMISC    0x00000040 /* Enable promiscous mode          */
#define BIGMAC_RXCFG_DERR     0x00000080 /* Disable error checking          */
#define BIGMAC_RXCFG_DCRCS    0x00000100 /* Disable CRC stripping           */
#define BIGMAC_RXCFG_ME       0x00000200 /* Receive packets addressed to me */
#define BIGMAC_RXCFG_PGRP     0x00000400 /* Enable promisc group mode       */
#define BIGMAC_RXCFG_HENABLE  0x00000800 /* Enable the hash filter          */
#define BIGMAC_RXCFG_AENABLE  0x00001000 /* Enable the address filter       */

/* These are the "Management Interface" (ie. MIF) registers of the transceiver. */
#define TCVR_BBCLOCK	0x00UL	/* Bit bang clock register          */
#define TCVR_BBDATA	0x04UL	/* Bit bang data register           */
#define TCVR_BBOENAB	0x08UL	/* Bit bang output enable           */
#define TCVR_FRAME	0x0cUL	/* Frame control/data register      */
#define TCVR_CFG	0x10UL	/* MIF config register              */
#define TCVR_IMASK	0x14UL	/* MIF interrupt mask               */
#define TCVR_STATUS	0x18UL	/* MIF status                       */
#define TCVR_SMACHINE	0x1cUL	/* MIF state machine                */
#define TCVR_REG_SIZE	0x20UL

/* Frame commands. */
#define FRAME_WRITE           0x50020000
#define FRAME_READ            0x60020000

/* Transceiver config register */
#define TCV_CFG_PSELECT       0x00000001 /* Select PHY                      */
#define TCV_CFG_PENABLE       0x00000002 /* Enable MIF polling              */
#define TCV_CFG_BENABLE       0x00000004 /* Enable the "bit banger" oh baby */
#define TCV_CFG_PREGADDR      0x000000f8 /* Address of poll register        */
#define TCV_CFG_MDIO0         0x00000100 /* MDIO zero, data/attached        */
#define TCV_CFG_MDIO1         0x00000200 /* MDIO one,  data/attached        */
#define TCV_CFG_PDADDR        0x00007c00 /* Device PHY address polling      */

/* Here are some PHY addresses. */
#define TCV_PADDR_ETX         0          /* Internal transceiver            */
#define TCV_PADDR_ITX         1          /* External transceiver            */

/* Transceiver status register */
#define TCV_STAT_BASIC        0xffff0000 /* The "basic" part                */
#define TCV_STAT_NORMAL       0x0000ffff /* The "non-basic" part            */

/* Inside the Happy Meal transceiver is the physical layer, they use an
 * implementations for National Semiconductor, part number DP83840VCE.
 * You can retrieve the data sheets and programming docs for this beast
 * from http://www.national.com/
 *
 * The DP83840 is capable of both 10 and 100Mbps ethernet, in both
 * half and full duplex mode.  It also supports auto negotiation.
 *
 * But.... THIS THING IS A PAIN IN THE ASS TO PROGRAM!
 * Debugging eeprom burnt code is more fun than programming this chip!
 */

/* First, the DP83840 register numbers. */
#define DP83840_BMCR            0x00        /* Basic mode control register */
#define DP83840_BMSR            0x01        /* Basic mode status register  */
#define DP83840_PHYSID1         0x02        /* PHYS ID 1                   */
#define DP83840_PHYSID2         0x03        /* PHYS ID 2                   */
#define DP83840_ADVERTISE       0x04        /* Advertisement control reg   */
#define DP83840_LPA             0x05        /* Link partner ability reg    */
#define DP83840_EXPANSION       0x06        /* Expansion register          */
#define DP83840_DCOUNTER        0x12        /* Disconnect counter          */
#define DP83840_FCSCOUNTER      0x13        /* False carrier counter       */
#define DP83840_NWAYTEST        0x14        /* N-way auto-neg test reg     */
#define DP83840_RERRCOUNTER     0x15        /* Receive error counter       */
#define DP83840_SREVISION       0x16        /* Silicon revision            */
#define DP83840_CSCONFIG        0x17        /* CS configuration            */
#define DP83840_LBRERROR        0x18        /* Lpback, rx, bypass error    */
#define DP83840_PHYADDR         0x19        /* PHY address                 */
#define DP83840_RESERVED        0x1a        /* Unused...                   */
#define DP83840_TPISTATUS       0x1b        /* TPI status for 10mbps       */
#define DP83840_NCONFIG         0x1c        /* Network interface config    */

/* Basic mode control register. */
#define BMCR_RESV               0x007f  /* Unused...                   */
#define BMCR_CTST               0x0080  /* Collision test              */
#define BMCR_FULLDPLX           0x0100  /* Full duplex                 */
#define BMCR_ANRESTART          0x0200  /* Auto negotiation restart    */
#define BMCR_ISOLATE            0x0400  /* Disconnect DP83840 from MII */
#define BMCR_PDOWN              0x0800  /* Powerdown the DP83840       */
#define BMCR_ANENABLE           0x1000  /* Enable auto negotiation     */
#define BMCR_SPEED100           0x2000  /* Select 100Mbps              */
#define BMCR_LOOPBACK           0x4000  /* TXD loopback bits           */
#define BMCR_RESET              0x8000  /* Reset the DP83840           */

/* Basic mode status register. */
#define BMSR_ERCAP              0x0001  /* Ext-reg capability          */
#define BMSR_JCD                0x0002  /* Jabber detected             */
#define BMSR_LSTATUS            0x0004  /* Link status                 */
#define BMSR_ANEGCAPABLE        0x0008  /* Able to do auto-negotiation */
#define BMSR_RFAULT             0x0010  /* Remote fault detected       */
#define BMSR_ANEGCOMPLETE       0x0020  /* Auto-negotiation complete   */
#define BMSR_RESV               0x07c0  /* Unused...                   */
#define BMSR_10HALF             0x0800  /* Can do 10mbps, half-duplex  */
#define BMSR_10FULL             0x1000  /* Can do 10mbps, full-duplex  */
#define BMSR_100HALF            0x2000  /* Can do 100mbps, half-duplex */
#define BMSR_100FULL            0x4000  /* Can do 100mbps, full-duplex */
#define BMSR_100BASE4           0x8000  /* Can do 100mbps, 4k packets  */

/* Advertisement control register. */
#define ADVERTISE_SLCT          0x001f  /* Selector bits               */
#define ADVERTISE_CSMA          0x0001  /* Only selector supported     */
#define ADVERTISE_10HALF        0x0020  /* Try for 10mbps half-duplex  */
#define ADVERTISE_10FULL        0x0040  /* Try for 10mbps full-duplex  */
#define ADVERTISE_100HALF       0x0080  /* Try for 100mbps half-duplex */
#define ADVERTISE_100FULL       0x0100  /* Try for 100mbps full-duplex */
#define ADVERTISE_100BASE4      0x0200  /* Try for 100mbps 4k packets  */
#define ADVERTISE_RESV          0x1c00  /* Unused...                   */
#define ADVERTISE_RFAULT        0x2000  /* Say we can detect faults    */
#define ADVERTISE_LPACK         0x4000  /* Ack link partners response  */
#define ADVERTISE_NPAGE         0x8000  /* Next page bit               */

#define ADVERTISE_ALL (ADVERTISE_10HALF | ADVERTISE_10FULL | \
                       ADVERTISE_100HALF | ADVERTISE_100FULL)

/* Link partner ability register. */
#define LPA_SLCT                0x001f  /* Same as advertise selector  */
#define LPA_10HALF              0x0020  /* Can do 10mbps half-duplex   */
#define LPA_10FULL              0x0040  /* Can do 10mbps full-duplex   */
#define LPA_100HALF             0x0080  /* Can do 100mbps half-duplex  */
#define LPA_100FULL             0x0100  /* Can do 100mbps full-duplex  */
#define LPA_100BASE4            0x0200  /* Can do 100mbps 4k packets   */
#define LPA_RESV                0x1c00  /* Unused...                   */
#define LPA_RFAULT              0x2000  /* Link partner faulted        */
#define LPA_LPACK               0x4000  /* Link partner acked us       */
#define LPA_NPAGE               0x8000  /* Next page bit               */

/* Expansion register for auto-negotiation. */
#define EXPANSION_NWAY          0x0001  /* Can do N-way auto-nego      */
#define EXPANSION_LCWP          0x0002  /* Got new RX page code word   */
#define EXPANSION_ENABLENPAGE   0x0004  /* This enables npage words    */
#define EXPANSION_NPCAPABLE     0x0008  /* Link partner supports npage */
#define EXPANSION_MFAULTS       0x0010  /* Multiple faults detected    */
#define EXPANSION_RESV          0xffe0  /* Unused...                   */

/* N-way test register. */
#define NWAYTEST_RESV1          0x00ff  /* Unused...                   */
#define NWAYTEST_LOOPBACK       0x0100  /* Enable loopback for N-way   */
#define NWAYTEST_RESV2          0xfe00  /* Unused...                   */

/* The Carrier Sense config register. */
#define CSCONFIG_RESV1          0x0001  /* Unused...                   */
#define CSCONFIG_LED4           0x0002  /* Pin for full-dplx LED4      */
#define CSCONFIG_LED1           0x0004  /* Pin for conn-status LED1    */
#define CSCONFIG_RESV2          0x0008  /* Unused...                   */
#define CSCONFIG_TCVDISAB       0x0010  /* Turns off the transceiver   */
#define CSCONFIG_DFBYPASS       0x0020  /* Bypass disconnect function  */
#define CSCONFIG_GLFORCE        0x0040  /* Good link force for 100mbps */
#define CSCONFIG_CLKTRISTATE    0x0080  /* Tristate 25m clock          */
#define CSCONFIG_RESV3          0x0700  /* Unused...                   */
#define CSCONFIG_ENCODE         0x0800  /* 1=MLT-3, 0=binary           */
#define CSCONFIG_RENABLE        0x1000  /* Repeater mode enable        */
#define CSCONFIG_TCDISABLE      0x2000  /* Disable timeout counter     */
#define CSCONFIG_RESV4          0x4000  /* Unused...                   */
#define CSCONFIG_NDISABLE       0x8000  /* Disable NRZI                */

/* Loopback, receive, bypass error register. */
#define LBRERROR_EBUFFER        0x0001  /* Show elasticity buf errors  */
#define LBRERROR_PACKET         0x0002  /* Show packet errors          */
#define LBRERROR_LINK           0x0004  /* Show link errors            */
#define LBRERROR_END            0x0008  /* Show premature end errors   */
#define LBRERROR_CODE           0x0010  /* Show code errors            */
#define LBRERROR_RESV1          0x00e0  /* Unused...                   */
#define LBRERROR_LBACK          0x0300  /* Remote and twister loopback */
#define LBRERROR_10TX           0x0400  /* Transceiver loopback 10mbps */
#define LBRERROR_ENDEC          0x0800  /* ENDEC loopback 10mbps       */
#define LBRERROR_ALIGN          0x1000  /* Bypass symbol alignment     */
#define LBRERROR_SCRAMBLER      0x2000  /* Bypass (de)scrambler        */
#define LBRERROR_ENCODER        0x4000  /* Bypass 4B5B/5B4B encoders   */
#define LBRERROR_BEBUF          0x8000  /* Bypass elasticity buffers   */

/* Physical address register. */
#define PHYADDR_ADDRESS         0x001f  /* The address itself          */
#define PHYADDR_DISCONNECT      0x0020  /* Disconnect status           */
#define PHYADDR_10MBPS          0x0040  /* 1=10mbps, 0=100mbps         */
#define PHYADDR_RESV            0xff80  /* Unused...                   */

/* TPI status register for 10mbps. */
#define TPISTATUS_RESV1         0x01ff  /* Unused...                   */
#define TPISTATUS_SERIAL        0x0200  /* Enable 10mbps serial mode   */
#define TPISTATUS_RESV2         0xfc00  /* Unused...                   */

/* Network interface config register. */
#define NCONFIG_JENABLE         0x0001  /* Jabber enable               */
#define NCONFIG_RESV1           0x0002  /* Unused...                   */
#define NCONFIG_SQUELCH         0x0004  /* Use low squelch             */
#define NCONFIG_UTP             0x0008  /* 1=UTP, 0=STP                */
#define NCONFIG_HBEAT           0x0010  /* Heart-beat enable           */
#define NCONFIG_LDISABLE        0x0020  /* Disable the link            */
#define NCONFIG_RESV2           0xffc0  /* Unused...                   */

/* Happy Meal descriptor rings and such.
 * All descriptor rings must be aligned on a 2K boundry.
 * All receive buffers must be 64 byte aligned.
 * Always write the address first before setting the ownership
 * bits to avoid races with the hardware scanning the ring.
 */
struct happy_meal_rxd {
	u32 rx_flags;
	u32 rx_addr;
};

#define RXFLAG_OWN         0x80000000 /* 1 = hardware, 0 = software */
#define RXFLAG_OVERFLOW    0x40000000 /* 1 = buffer overflow        */
#define RXFLAG_SIZE        0x3fff0000 /* Size of the buffer         */
#define RXFLAG_CSUM        0x0000ffff /* HW computed checksum       */

struct happy_meal_txd {
	u32 tx_flags;
	u32 tx_addr;
};

#define TXFLAG_OWN         0x80000000 /* 1 = hardware, 0 = software */
#define TXFLAG_SOP         0x40000000 /* 1 = start of packet        */
#define TXFLAG_EOP         0x20000000 /* 1 = end of packet          */
#define TXFLAG_CSENABLE    0x10000000 /* 1 = enable hw-checksums    */
#define TXFLAG_CSLOCATION  0x0ff00000 /* Where to stick the csum    */
#define TXFLAG_CSBUFBEGIN  0x000fc000 /* Where to begin checksum    */
#define TXFLAG_SIZE        0x00003fff /* Size of the packet         */

#define TX_RING_SIZE       32         /* Must be >16 and <255, multiple of 16  */
#define RX_RING_SIZE       32         /* see ERX_CFG_SIZE* for possible values */

#define TX_RING_MAXSIZE    256
#define RX_RING_MAXSIZE    256

/* 34 byte offset for checksum computation.  This works because ip_input() will clear out
 * the skb->csum and skb->ip_summed fields and recompute the csum if IP options are
 * present in the header.  34 == (ethernet header len) + sizeof(struct iphdr)
 */
#define ERX_CFG_DEFAULT(off) (ERX_CFG_DMAENABLE|((off)<<3)|ERX_CFG_SIZE32|(0x22<<16))

#define NEXT_RX(num)       (((num) + 1) & (RX_RING_SIZE - 1))
#define NEXT_TX(num)       (((num) + 1) & (TX_RING_SIZE - 1))
#define PREV_RX(num)       (((num) - 1) & (RX_RING_SIZE - 1))
#define PREV_TX(num)       (((num) - 1) & (TX_RING_SIZE - 1))

#define TX_BUFFS_AVAIL(hp)                                    \
        (((hp)->tx_old <= (hp)->tx_new) ?                     \
	  (hp)->tx_old + (TX_RING_SIZE - 1) - (hp)->tx_new :  \
			    (hp)->tx_old - (hp)->tx_new - 1)

#define RX_OFFSET          2
#define RX_BUF_ALLOC_SIZE  (1546 + RX_OFFSET + 64)

#define RX_COPY_THRESHOLD  256

struct hmeal_init_block {
	struct happy_meal_rxd happy_meal_rxd[RX_RING_MAXSIZE];
	struct happy_meal_txd happy_meal_txd[TX_RING_MAXSIZE];
};

#define hblock_offset(mem, elem) \
((__u32)((unsigned long)(&(((struct hmeal_init_block *)0)->mem[elem]))))

/* Now software state stuff. */
enum happy_transceiver {
	external = 0,
	internal = 1,
	none     = 2,
};

/* Timer state engine. */
enum happy_timer_state {
	arbwait  = 0,  /* Waiting for auto negotiation to complete.          */
	lupwait  = 1,  /* Auto-neg complete, awaiting link-up status.        */
	ltrywait = 2,  /* Forcing try of all modes, from fastest to slowest. */
	asleep   = 3,  /* Time inactive.                                     */
};

struct quattro;

/* Happy happy, joy joy! */
struct happy_meal {
	unsigned long	gregs;			/* Happy meal global registers       */
	struct hmeal_init_block  *happy_block;	/* RX and TX descriptors (CPU addr)  */

#if defined(CONFIG_SBUS) && defined(CONFIG_PCI)
	u32 (*read_desc32)(u32 *);
	void (*write_txd)(struct happy_meal_txd *, u32, u32);
	void (*write_rxd)(struct happy_meal_rxd *, u32, u32);
	u32 (*dma_map)(void *, void *, long, int);
	void (*dma_unmap)(void *, u32, long, int);
	void (*dma_sync)(void *, u32, long, int);
#endif

	/* This is either a sbus_dev or a pci_dev. */
	void			  *happy_dev;

	spinlock_t		  happy_lock;

	struct sk_buff           *rx_skbs[RX_RING_SIZE];
	struct sk_buff           *tx_skbs[TX_RING_SIZE];

	int rx_new, tx_new, rx_old, tx_old;

	struct net_device_stats	  net_stats;      /* Statistical counters              */

#if defined(CONFIG_SBUS) && defined(CONFIG_PCI)
	u32 (*read32)(unsigned long);
	void (*write32)(unsigned long, u32);
#endif

	unsigned long	etxregs;        /* External transmitter regs         */
	unsigned long	erxregs;        /* External receiver regs            */
	unsigned long	bigmacregs;     /* BIGMAC core regs		     */
	unsigned long	tcvregs;        /* MIF transceiver regs              */

	__u32                     hblock_dvma;    /* DVMA visible address happy block  */
	unsigned int              happy_flags;    /* Driver state flags                */
	enum happy_transceiver    tcvr_type;      /* Kind of transceiver in use        */
	unsigned int              happy_bursts;   /* Get your mind out of the gutter   */
	unsigned int              paddr;          /* PHY address for transceiver       */
	unsigned short            hm_revision;    /* Happy meal revision               */
	unsigned short            sw_bmcr;        /* SW copy of BMCR                   */
	unsigned short            sw_bmsr;        /* SW copy of BMSR                   */
	unsigned short            sw_physid1;     /* SW copy of PHYSID1                */
	unsigned short            sw_physid2;     /* SW copy of PHYSID2                */
	unsigned short            sw_advertise;   /* SW copy of ADVERTISE              */
	unsigned short            sw_lpa;         /* SW copy of LPA                    */
	unsigned short            sw_expansion;   /* SW copy of EXPANSION              */
	unsigned short            sw_csconfig;    /* SW copy of CSCONFIG               */
	unsigned int              auto_speed;     /* Auto-nego link speed              */
        unsigned int              forced_speed;   /* Force mode link speed             */
	unsigned int              poll_data;      /* MIF poll data                     */
	unsigned int              poll_flag;      /* MIF poll flag                     */
	unsigned int              linkcheck;      /* Have we checked the link yet?     */
	unsigned int              lnkup;          /* Is the link up as far as we know? */
	unsigned int              lnkdown;        /* Trying to force the link down?    */
	unsigned int              lnkcnt;         /* Counter for link-up attempts.     */
	struct timer_list         happy_timer;    /* To watch the link when coming up. */
	enum happy_timer_state    timer_state;    /* State of the auto-neg timer.      */
	unsigned int              timer_ticks;    /* Number of clicks at each state.   */

	struct net_device	 *dev;		/* Backpointer                       */
	struct quattro		 *qfe_parent;	/* For Quattro cards                 */
	int			  qfe_ent;	/* Which instance on quattro         */
	struct happy_meal         *next_module;
};

/* Here are the happy flags. */
#define HFLAG_POLL                0x00000001      /* We are doing MIF polling          */
#define HFLAG_FENABLE             0x00000002      /* The MII frame is enabled          */
#define HFLAG_LANCE               0x00000004      /* We are using lance-mode           */
#define HFLAG_RXENABLE            0x00000008      /* Receiver is enabled               */
#define HFLAG_AUTO                0x00000010      /* Using auto-negotiation, 0 = force */
#define HFLAG_FULL                0x00000020      /* Full duplex enable                */
#define HFLAG_MACFULL             0x00000040      /* Using full duplex in the MAC      */
#define HFLAG_POLLENABLE          0x00000080      /* Actually try MIF polling          */
#define HFLAG_RXCV                0x00000100      /* XXX RXCV ENABLE                   */
#define HFLAG_INIT                0x00000200      /* Init called at least once         */
#define HFLAG_LINKUP              0x00000400      /* 1 = Link is up                    */
#define HFLAG_PCI                 0x00000800      /* PCI based Happy Meal              */
#define HFLAG_QUATTRO		  0x00001000      /* On QFE/Quattro card	       */

#define HFLAG_20_21  (HFLAG_POLLENABLE | HFLAG_FENABLE)
#define HFLAG_NOT_A0 (HFLAG_POLLENABLE | HFLAG_FENABLE | HFLAG_LANCE | HFLAG_RXCV)

/* Support for QFE/Quattro cards. */
struct quattro {
	struct net_device	*happy_meals[4];

	/* This is either a sbus_dev or a pci_dev. */
	void			*quattro_dev;

	struct quattro		*next;

	/* PROM ranges, if any. */
#ifdef CONFIG_SBUS
	struct linux_prom_ranges  ranges[8];
#endif
	int			  nranges;
};

/* We use this to acquire receive skb's that we can DMA directly into. */
#define ALIGNED_RX_SKB_ADDR(addr) \
        ((((unsigned long)(addr) + (64UL - 1UL)) & ~(64UL - 1UL)) - (unsigned long)(addr))
#define happy_meal_alloc_skb(__length, __gfp_flags) \
({	struct sk_buff *__skb; \
	__skb = alloc_skb((__length) + 64, (__gfp_flags)); \
	if(__skb) { \
		int __offset = (int) ALIGNED_RX_SKB_ADDR(__skb->data); \
		if(__offset) \
			skb_reserve(__skb, __offset); \
	} \
	__skb; \
})

#endif /* !(_SUNHME_H) */
