/*
 * forcedeth: Ethernet driver for NVIDIA nForce media access controllers.
 *
 * Note: This driver is a cleanroom reimplementation based on reverse
 *      engineered documentation written by Carl-Daniel Hailfinger
 *      and Andrew de Quincey.
 *
 * NVIDIA, nForce and other NVIDIA marks are trademarks or registered
 * trademarks of NVIDIA Corporation in the United States and other
 * countries.
 *
 * Copyright (C) 2003,4,5 Manfred Spraul
 * Copyright (C) 2004 Andrew de Quincey (wol support)
 * Copyright (C) 2004 Carl-Daniel Hailfinger (invalid MAC handling, insane
 *		IRQ rate fixes, bigendian fixes, cleanups, verification)
 * Copyright (c) 2004,5,6 NVIDIA Corporation
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
 * Changelog:
 * 	0.01: 05 Oct 2003: First release that compiles without warnings.
 * 	0.02: 05 Oct 2003: Fix bug for nv_drain_tx: do not try to free NULL skbs.
 * 			   Check all PCI BARs for the register window.
 * 			   udelay added to mii_rw.
 * 	0.03: 06 Oct 2003: Initialize dev->irq.
 * 	0.04: 07 Oct 2003: Initialize np->lock, reduce handled irqs, add printks.
 * 	0.05: 09 Oct 2003: printk removed again, irq status print tx_timeout.
 * 	0.06: 10 Oct 2003: MAC Address read updated, pff flag generation updated,
 * 			   irq mask updated
 * 	0.07: 14 Oct 2003: Further irq mask updates.
 * 	0.08: 20 Oct 2003: rx_desc.Length initialization added, nv_alloc_rx refill
 * 			   added into irq handler, NULL check for drain_ring.
 * 	0.09: 20 Oct 2003: Basic link speed irq implementation. Only handle the
 * 			   requested interrupt sources.
 * 	0.10: 20 Oct 2003: First cleanup for release.
 * 	0.11: 21 Oct 2003: hexdump for tx added, rx buffer sizes increased.
 * 			   MAC Address init fix, set_multicast cleanup.
 * 	0.12: 23 Oct 2003: Cleanups for release.
 * 	0.13: 25 Oct 2003: Limit for concurrent tx packets increased to 10.
 * 			   Set link speed correctly. start rx before starting
 * 			   tx (nv_start_rx sets the link speed).
 * 	0.14: 25 Oct 2003: Nic dependant irq mask.
 * 	0.15: 08 Nov 2003: fix smp deadlock with set_multicast_list during
 * 			   open.
 * 	0.16: 15 Nov 2003: include file cleanup for ppc64, rx buffer size
 * 			   increased to 1628 bytes.
 * 	0.17: 16 Nov 2003: undo rx buffer size increase. Substract 1 from
 * 			   the tx length.
 * 	0.18: 17 Nov 2003: fix oops due to late initialization of dev_stats
 * 	0.19: 29 Nov 2003: Handle RxNoBuf, detect & handle invalid mac
 * 			   addresses, really stop rx if already running
 * 			   in nv_start_rx, clean up a bit.
 * 	0.20: 07 Dec 2003: alloc fixes
 * 	0.21: 12 Jan 2004: additional alloc fix, nic polling fix.
 *	0.22: 19 Jan 2004: reprogram timer to a sane rate, avoid lockup
 *			   on close.
 *	0.23: 26 Jan 2004: various small cleanups
 *	0.24: 27 Feb 2004: make driver even less anonymous in backtraces
 *	0.25: 09 Mar 2004: wol support
 *	0.26: 03 Jun 2004: netdriver specific annotation, sparse-related fixes
 *	0.27: 19 Jun 2004: Gigabit support, new descriptor rings,
 *			   added CK804/MCP04 device IDs, code fixes
 *			   for registers, link status and other minor fixes.
 *	0.28: 21 Jun 2004: Big cleanup, making driver mostly endian safe
 *	0.29: 31 Aug 2004: Add backup timer for link change notification.
 *	0.30: 25 Sep 2004: rx checksum support for nf 250 Gb. Add rx reset
 *			   into nv_close, otherwise reenabling for wol can
 *			   cause DMA to kfree'd memory.
 *	0.31: 14 Nov 2004: ethtool support for getting/setting link
 *			   capabilities.
 *	0.32: 16 Apr 2005: RX_ERROR4 handling added.
 *	0.33: 16 May 2005: Support for MCP51 added.
 *	0.34: 18 Jun 2005: Add DEV_NEED_LINKTIMER to all nForce nics.
 *	0.35: 26 Jun 2005: Support for MCP55 added.
 *	0.36: 28 Jun 2005: Add jumbo frame support.
 *	0.37: 10 Jul 2005: Additional ethtool support, cleanup of pci id list
 *	0.38: 16 Jul 2005: tx irq rewrite: Use global flags instead of
 *			   per-packet flags.
 *	0.39: 18 Jul 2005: Add 64bit descriptor support.
 *	0.40: 19 Jul 2005: Add support for mac address change.
 *	0.41: 30 Jul 2005: Write back original MAC in nv_close instead
 *			   of nv_remove
 *	0.42: 06 Aug 2005: Fix lack of link speed initialization
 *			   in the second (and later) nv_open call
 *	0.43: 10 Aug 2005: Add support for tx checksum.
 *	0.44: 20 Aug 2005: Add support for scatter gather and segmentation.
 *	0.45: 18 Sep 2005: Remove nv_stop/start_rx from every link check
 *	0.46: 20 Oct 2005: Add irq optimization modes.
 *	0.47: 26 Oct 2005: Add phyaddr 0 in phy scan.
 *	0.48: 24 Dec 2005: Disable TSO, bugfix for pci_map_single
 *	0.49: 10 Dec 2005: Fix tso for large buffers.
 *	0.50: 20 Jan 2006: Add 8021pq tagging support.
 *	0.51: 20 Jan 2006: Add 64bit consistent memory allocation for rings.
 *	0.52: 20 Jan 2006: Add MSI/MSIX support.
 *	0.53: 19 Mar 2006: Fix init from low power mode and add hw reset.
 *	0.54: 21 Mar 2006: Fix spin locks for multi irqs and cleanup.
 *	0.55: 22 Mar 2006: Add flow control (pause frame).
 *	0.56: 22 Mar 2006: Additional ethtool config and moduleparam support.
 *	0.57: 14 May 2006: Mac address set in probe/remove and order corrections.
 *	0.58: 30 Oct 2006: Added support for sideband management unit.
 *	0.59: 30 Oct 2006: Added support for recoverable error.
 *	0.60: 20 Jan 2007: Code optimizations for rings, rx & tx data paths, and stats.
 *
 * Known bugs:
 * We suspect that on some hardware no TX done interrupts are generated.
 * This means recovery from netif_stop_queue only happens if the hw timer
 * interrupt fires (100 times/second, configurable with NVREG_POLL_DEFAULT)
 * and the timer is active in the IRQMask, or if a rx packet arrives by chance.
 * If your hardware reliably generates tx done interrupts, then you can remove
 * DEV_NEED_TIMERIRQ from the driver_data flags.
 * DEV_NEED_TIMERIRQ will not harm you on sane hardware, only generating a few
 * superfluous timer interrupts from the nic.
 */
#ifdef CONFIG_FORCEDETH_NAPI
#define DRIVERNAPI "-NAPI"
#else
#define DRIVERNAPI
#endif
#define FORCEDETH_VERSION		"0.60"
#define DRV_NAME			"forcedeth"

#include <linux/module.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/ethtool.h>
#include <linux/timer.h>
#include <linux/skbuff.h>
#include <linux/mii.h>
#include <linux/random.h>
#include <linux/init.h>
#include <linux/if_vlan.h>
#include <linux/dma-mapping.h>

#include <asm/irq.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/system.h>

#if 0
#define dprintk			printk
#else
#define dprintk(x...)		do { } while (0)
#endif


/*
 * Hardware access:
 */

#define DEV_NEED_TIMERIRQ	0x0001  /* set the timer irq flag in the irq mask */
#define DEV_NEED_LINKTIMER	0x0002	/* poll link settings. Relies on the timer irq */
#define DEV_HAS_LARGEDESC	0x0004	/* device supports jumbo frames and needs packet format 2 */
#define DEV_HAS_HIGH_DMA        0x0008  /* device supports 64bit dma */
#define DEV_HAS_CHECKSUM        0x0010  /* device supports tx and rx checksum offloads */
#define DEV_HAS_VLAN            0x0020  /* device supports vlan tagging and striping */
#define DEV_HAS_MSI             0x0040  /* device supports MSI */
#define DEV_HAS_MSI_X           0x0080  /* device supports MSI-X */
#define DEV_HAS_POWER_CNTRL     0x0100  /* device supports power savings */
#define DEV_HAS_PAUSEFRAME_TX   0x0200  /* device supports tx pause frames */
#define DEV_HAS_STATISTICS_V1   0x0400  /* device supports hw statistics version 1 */
#define DEV_HAS_STATISTICS_V2   0x0800  /* device supports hw statistics version 2 */
#define DEV_HAS_TEST_EXTENDED   0x1000  /* device supports extended diagnostic test */
#define DEV_HAS_MGMT_UNIT       0x2000  /* device supports management unit */
#define DEV_HAS_CORRECT_MACADDR 0x4000  /* device supports correct mac address order */

enum {
	NvRegIrqStatus = 0x000,
#define NVREG_IRQSTAT_MIIEVENT	0x040
#define NVREG_IRQSTAT_MASK		0x81ff
	NvRegIrqMask = 0x004,
#define NVREG_IRQ_RX_ERROR		0x0001
#define NVREG_IRQ_RX			0x0002
#define NVREG_IRQ_RX_NOBUF		0x0004
#define NVREG_IRQ_TX_ERR		0x0008
#define NVREG_IRQ_TX_OK			0x0010
#define NVREG_IRQ_TIMER			0x0020
#define NVREG_IRQ_LINK			0x0040
#define NVREG_IRQ_RX_FORCED		0x0080
#define NVREG_IRQ_TX_FORCED		0x0100
#define NVREG_IRQ_RECOVER_ERROR		0x8000
#define NVREG_IRQMASK_THROUGHPUT	0x00df
#define NVREG_IRQMASK_CPU		0x0060
#define NVREG_IRQ_TX_ALL		(NVREG_IRQ_TX_ERR|NVREG_IRQ_TX_OK|NVREG_IRQ_TX_FORCED)
#define NVREG_IRQ_RX_ALL		(NVREG_IRQ_RX_ERROR|NVREG_IRQ_RX|NVREG_IRQ_RX_NOBUF|NVREG_IRQ_RX_FORCED)
#define NVREG_IRQ_OTHER			(NVREG_IRQ_TIMER|NVREG_IRQ_LINK|NVREG_IRQ_RECOVER_ERROR)

#define NVREG_IRQ_UNKNOWN	(~(NVREG_IRQ_RX_ERROR|NVREG_IRQ_RX|NVREG_IRQ_RX_NOBUF|NVREG_IRQ_TX_ERR| \
					NVREG_IRQ_TX_OK|NVREG_IRQ_TIMER|NVREG_IRQ_LINK|NVREG_IRQ_RX_FORCED| \
					NVREG_IRQ_TX_FORCED|NVREG_IRQ_RECOVER_ERROR))

	NvRegUnknownSetupReg6 = 0x008,
#define NVREG_UNKSETUP6_VAL		3

/*
 * NVREG_POLL_DEFAULT is the interval length of the timer source on the nic
 * NVREG_POLL_DEFAULT=97 would result in an interval length of 1 ms
 */
	NvRegPollingInterval = 0x00c,
#define NVREG_POLL_DEFAULT_THROUGHPUT	970 /* backup tx cleanup if loop max reached */
#define NVREG_POLL_DEFAULT_CPU	13
	NvRegMSIMap0 = 0x020,
	NvRegMSIMap1 = 0x024,
	NvRegMSIIrqMask = 0x030,
#define NVREG_MSI_VECTOR_0_ENABLED 0x01
	NvRegMisc1 = 0x080,
#define NVREG_MISC1_PAUSE_TX	0x01
#define NVREG_MISC1_HD		0x02
#define NVREG_MISC1_FORCE	0x3b0f3c

	NvRegMacReset = 0x3c,
#define NVREG_MAC_RESET_ASSERT	0x0F3
	NvRegTransmitterControl = 0x084,
#define NVREG_XMITCTL_START	0x01
#define NVREG_XMITCTL_MGMT_ST	0x40000000
#define NVREG_XMITCTL_SYNC_MASK		0x000f0000
#define NVREG_XMITCTL_SYNC_NOT_READY	0x0
#define NVREG_XMITCTL_SYNC_PHY_INIT	0x00040000
#define NVREG_XMITCTL_MGMT_SEMA_MASK	0x00000f00
#define NVREG_XMITCTL_MGMT_SEMA_FREE	0x0
#define NVREG_XMITCTL_HOST_SEMA_MASK	0x0000f000
#define NVREG_XMITCTL_HOST_SEMA_ACQ	0x0000f000
#define NVREG_XMITCTL_HOST_LOADED	0x00004000
#define NVREG_XMITCTL_TX_PATH_EN	0x01000000
	NvRegTransmitterStatus = 0x088,
#define NVREG_XMITSTAT_BUSY	0x01

	NvRegPacketFilterFlags = 0x8c,
#define NVREG_PFF_PAUSE_RX	0x08
#define NVREG_PFF_ALWAYS	0x7F0000
#define NVREG_PFF_PROMISC	0x80
#define NVREG_PFF_MYADDR	0x20
#define NVREG_PFF_LOOPBACK	0x10

	NvRegOffloadConfig = 0x90,
#define NVREG_OFFLOAD_HOMEPHY	0x601
#define NVREG_OFFLOAD_NORMAL	RX_NIC_BUFSIZE
	NvRegReceiverControl = 0x094,
#define NVREG_RCVCTL_START	0x01
#define NVREG_RCVCTL_RX_PATH_EN	0x01000000
	NvRegReceiverStatus = 0x98,
#define NVREG_RCVSTAT_BUSY	0x01

	NvRegRandomSeed = 0x9c,
#define NVREG_RNDSEED_MASK	0x00ff
#define NVREG_RNDSEED_FORCE	0x7f00
#define NVREG_RNDSEED_FORCE2	0x2d00
#define NVREG_RNDSEED_FORCE3	0x7400

	NvRegTxDeferral = 0xA0,
#define NVREG_TX_DEFERRAL_DEFAULT	0x15050f
#define NVREG_TX_DEFERRAL_RGMII_10_100	0x16070f
#define NVREG_TX_DEFERRAL_RGMII_1000	0x14050f
	NvRegRxDeferral = 0xA4,
#define NVREG_RX_DEFERRAL_DEFAULT	0x16
	NvRegMacAddrA = 0xA8,
	NvRegMacAddrB = 0xAC,
	NvRegMulticastAddrA = 0xB0,
#define NVREG_MCASTADDRA_FORCE	0x01
	NvRegMulticastAddrB = 0xB4,
	NvRegMulticastMaskA = 0xB8,
	NvRegMulticastMaskB = 0xBC,

	NvRegPhyInterface = 0xC0,
#define PHY_RGMII		0x10000000

	NvRegTxRingPhysAddr = 0x100,
	NvRegRxRingPhysAddr = 0x104,
	NvRegRingSizes = 0x108,
#define NVREG_RINGSZ_TXSHIFT 0
#define NVREG_RINGSZ_RXSHIFT 16
	NvRegTransmitPoll = 0x10c,
#define NVREG_TRANSMITPOLL_MAC_ADDR_REV	0x00008000
	NvRegLinkSpeed = 0x110,
#define NVREG_LINKSPEED_FORCE 0x10000
#define NVREG_LINKSPEED_10	1000
#define NVREG_LINKSPEED_100	100
#define NVREG_LINKSPEED_1000	50
#define NVREG_LINKSPEED_MASK	(0xFFF)
	NvRegUnknownSetupReg5 = 0x130,
#define NVREG_UNKSETUP5_BIT31	(1<<31)
	NvRegTxWatermark = 0x13c,
#define NVREG_TX_WM_DESC1_DEFAULT	0x0200010
#define NVREG_TX_WM_DESC2_3_DEFAULT	0x1e08000
#define NVREG_TX_WM_DESC2_3_1000	0xfe08000
	NvRegTxRxControl = 0x144,
#define NVREG_TXRXCTL_KICK	0x0001
#define NVREG_TXRXCTL_BIT1	0x0002
#define NVREG_TXRXCTL_BIT2	0x0004
#define NVREG_TXRXCTL_IDLE	0x0008
#define NVREG_TXRXCTL_RESET	0x0010
#define NVREG_TXRXCTL_RXCHECK	0x0400
#define NVREG_TXRXCTL_DESC_1	0
#define NVREG_TXRXCTL_DESC_2	0x002100
#define NVREG_TXRXCTL_DESC_3	0xc02200
#define NVREG_TXRXCTL_VLANSTRIP 0x00040
#define NVREG_TXRXCTL_VLANINS	0x00080
	NvRegTxRingPhysAddrHigh = 0x148,
	NvRegRxRingPhysAddrHigh = 0x14C,
	NvRegTxPauseFrame = 0x170,
#define NVREG_TX_PAUSEFRAME_DISABLE	0x1ff0080
#define NVREG_TX_PAUSEFRAME_ENABLE	0x0c00030
	NvRegMIIStatus = 0x180,
#define NVREG_MIISTAT_ERROR		0x0001
#define NVREG_MIISTAT_LINKCHANGE	0x0008
#define NVREG_MIISTAT_MASK		0x000f
#define NVREG_MIISTAT_MASK2		0x000f
	NvRegMIIMask = 0x184,
#define NVREG_MII_LINKCHANGE		0x0008

	NvRegAdapterControl = 0x188,
#define NVREG_ADAPTCTL_START	0x02
#define NVREG_ADAPTCTL_LINKUP	0x04
#define NVREG_ADAPTCTL_PHYVALID	0x40000
#define NVREG_ADAPTCTL_RUNNING	0x100000
#define NVREG_ADAPTCTL_PHYSHIFT	24
	NvRegMIISpeed = 0x18c,
#define NVREG_MIISPEED_BIT8	(1<<8)
#define NVREG_MIIDELAY	5
	NvRegMIIControl = 0x190,
#define NVREG_MIICTL_INUSE	0x08000
#define NVREG_MIICTL_WRITE	0x00400
#define NVREG_MIICTL_ADDRSHIFT	5
	NvRegMIIData = 0x194,
	NvRegWakeUpFlags = 0x200,
#define NVREG_WAKEUPFLAGS_VAL		0x7770
#define NVREG_WAKEUPFLAGS_BUSYSHIFT	24
#define NVREG_WAKEUPFLAGS_ENABLESHIFT	16
#define NVREG_WAKEUPFLAGS_D3SHIFT	12
#define NVREG_WAKEUPFLAGS_D2SHIFT	8
#define NVREG_WAKEUPFLAGS_D1SHIFT	4
#define NVREG_WAKEUPFLAGS_D0SHIFT	0
#define NVREG_WAKEUPFLAGS_ACCEPT_MAGPAT		0x01
#define NVREG_WAKEUPFLAGS_ACCEPT_WAKEUPPAT	0x02
#define NVREG_WAKEUPFLAGS_ACCEPT_LINKCHANGE	0x04
#define NVREG_WAKEUPFLAGS_ENABLE	0x1111

	NvRegPatternCRC = 0x204,
	NvRegPatternMask = 0x208,
	NvRegPowerCap = 0x268,
#define NVREG_POWERCAP_D3SUPP	(1<<30)
#define NVREG_POWERCAP_D2SUPP	(1<<26)
#define NVREG_POWERCAP_D1SUPP	(1<<25)
	NvRegPowerState = 0x26c,
#define NVREG_POWERSTATE_POWEREDUP	0x8000
#define NVREG_POWERSTATE_VALID		0x0100
#define NVREG_POWERSTATE_MASK		0x0003
#define NVREG_POWERSTATE_D0		0x0000
#define NVREG_POWERSTATE_D1		0x0001
#define NVREG_POWERSTATE_D2		0x0002
#define NVREG_POWERSTATE_D3		0x0003
	NvRegTxCnt = 0x280,
	NvRegTxZeroReXmt = 0x284,
	NvRegTxOneReXmt = 0x288,
	NvRegTxManyReXmt = 0x28c,
	NvRegTxLateCol = 0x290,
	NvRegTxUnderflow = 0x294,
	NvRegTxLossCarrier = 0x298,
	NvRegTxExcessDef = 0x29c,
	NvRegTxRetryErr = 0x2a0,
	NvRegRxFrameErr = 0x2a4,
	NvRegRxExtraByte = 0x2a8,
	NvRegRxLateCol = 0x2ac,
	NvRegRxRunt = 0x2b0,
	NvRegRxFrameTooLong = 0x2b4,
	NvRegRxOverflow = 0x2b8,
	NvRegRxFCSErr = 0x2bc,
	NvRegRxFrameAlignErr = 0x2c0,
	NvRegRxLenErr = 0x2c4,
	NvRegRxUnicast = 0x2c8,
	NvRegRxMulticast = 0x2cc,
	NvRegRxBroadcast = 0x2d0,
	NvRegTxDef = 0x2d4,
	NvRegTxFrame = 0x2d8,
	NvRegRxCnt = 0x2dc,
	NvRegTxPause = 0x2e0,
	NvRegRxPause = 0x2e4,
	NvRegRxDropFrame = 0x2e8,
	NvRegVlanControl = 0x300,
#define NVREG_VLANCONTROL_ENABLE	0x2000
	NvRegMSIXMap0 = 0x3e0,
	NvRegMSIXMap1 = 0x3e4,
	NvRegMSIXIrqStatus = 0x3f0,

	NvRegPowerState2 = 0x600,
#define NVREG_POWERSTATE2_POWERUP_MASK		0x0F11
#define NVREG_POWERSTATE2_POWERUP_REV_A3	0x0001
};

/* Big endian: should work, but is untested */
struct ring_desc {
	__le32 buf;
	__le32 flaglen;
};

struct ring_desc_ex {
	__le32 bufhigh;
	__le32 buflow;
	__le32 txvlan;
	__le32 flaglen;
};

union ring_type {
	struct ring_desc* orig;
	struct ring_desc_ex* ex;
};

#define FLAG_MASK_V1 0xffff0000
#define FLAG_MASK_V2 0xffffc000
#define LEN_MASK_V1 (0xffffffff ^ FLAG_MASK_V1)
#define LEN_MASK_V2 (0xffffffff ^ FLAG_MASK_V2)

#define NV_TX_LASTPACKET	(1<<16)
#define NV_TX_RETRYERROR	(1<<19)
#define NV_TX_FORCED_INTERRUPT	(1<<24)
#define NV_TX_DEFERRED		(1<<26)
#define NV_TX_CARRIERLOST	(1<<27)
#define NV_TX_LATECOLLISION	(1<<28)
#define NV_TX_UNDERFLOW		(1<<29)
#define NV_TX_ERROR		(1<<30)
#define NV_TX_VALID		(1<<31)

#define NV_TX2_LASTPACKET	(1<<29)
#define NV_TX2_RETRYERROR	(1<<18)
#define NV_TX2_FORCED_INTERRUPT	(1<<30)
#define NV_TX2_DEFERRED		(1<<25)
#define NV_TX2_CARRIERLOST	(1<<26)
#define NV_TX2_LATECOLLISION	(1<<27)
#define NV_TX2_UNDERFLOW	(1<<28)
/* error and valid are the same for both */
#define NV_TX2_ERROR		(1<<30)
#define NV_TX2_VALID		(1<<31)
#define NV_TX2_TSO		(1<<28)
#define NV_TX2_TSO_SHIFT	14
#define NV_TX2_TSO_MAX_SHIFT	14
#define NV_TX2_TSO_MAX_SIZE	(1<<NV_TX2_TSO_MAX_SHIFT)
#define NV_TX2_CHECKSUM_L3	(1<<27)
#define NV_TX2_CHECKSUM_L4	(1<<26)

#define NV_TX3_VLAN_TAG_PRESENT (1<<18)

#define NV_RX_DESCRIPTORVALID	(1<<16)
#define NV_RX_MISSEDFRAME	(1<<17)
#define NV_RX_SUBSTRACT1	(1<<18)
#define NV_RX_ERROR1		(1<<23)
#define NV_RX_ERROR2		(1<<24)
#define NV_RX_ERROR3		(1<<25)
#define NV_RX_ERROR4		(1<<26)
#define NV_RX_CRCERR		(1<<27)
#define NV_RX_OVERFLOW		(1<<28)
#define NV_RX_FRAMINGERR	(1<<29)
#define NV_RX_ERROR		(1<<30)
#define NV_RX_AVAIL		(1<<31)

#define NV_RX2_CHECKSUMMASK	(0x1C000000)
#define NV_RX2_CHECKSUMOK1	(0x10000000)
#define NV_RX2_CHECKSUMOK2	(0x14000000)
#define NV_RX2_CHECKSUMOK3	(0x18000000)
#define NV_RX2_DESCRIPTORVALID	(1<<29)
#define NV_RX2_SUBSTRACT1	(1<<25)
#define NV_RX2_ERROR1		(1<<18)
#define NV_RX2_ERROR2		(1<<19)
#define NV_RX2_ERROR3		(1<<20)
#define NV_RX2_ERROR4		(1<<21)
#define NV_RX2_CRCERR		(1<<22)
#define NV_RX2_OVERFLOW		(1<<23)
#define NV_RX2_FRAMINGERR	(1<<24)
/* error and avail are the same for both */
#define NV_RX2_ERROR		(1<<30)
#define NV_RX2_AVAIL		(1<<31)

#define NV_RX3_VLAN_TAG_PRESENT (1<<16)
#define NV_RX3_VLAN_TAG_MASK	(0x0000FFFF)

/* Miscelaneous hardware related defines: */
#define NV_PCI_REGSZ_VER1      	0x270
#define NV_PCI_REGSZ_VER2      	0x2d4
#define NV_PCI_REGSZ_VER3      	0x604

/* various timeout delays: all in usec */
#define NV_TXRX_RESET_DELAY	4
#define NV_TXSTOP_DELAY1	10
#define NV_TXSTOP_DELAY1MAX	500000
#define NV_TXSTOP_DELAY2	100
#define NV_RXSTOP_DELAY1	10
#define NV_RXSTOP_DELAY1MAX	500000
#define NV_RXSTOP_DELAY2	100
#define NV_SETUP5_DELAY		5
#define NV_SETUP5_DELAYMAX	50000
#define NV_POWERUP_DELAY	5
#define NV_POWERUP_DELAYMAX	5000
#define NV_MIIBUSY_DELAY	50
#define NV_MIIPHY_DELAY	10
#define NV_MIIPHY_DELAYMAX	10000
#define NV_MAC_RESET_DELAY	64

#define NV_WAKEUPPATTERNS	5
#define NV_WAKEUPMASKENTRIES	4

/* General driver defaults */
#define NV_WATCHDOG_TIMEO	(5*HZ)

#define RX_RING_DEFAULT		128
#define TX_RING_DEFAULT		256
#define RX_RING_MIN		128
#define TX_RING_MIN		64
#define RING_MAX_DESC_VER_1	1024
#define RING_MAX_DESC_VER_2_3	16384

/* rx/tx mac addr + type + vlan + align + slack*/
#define NV_RX_HEADERS		(64)
/* even more slack. */
#define NV_RX_ALLOC_PAD		(64)

/* maximum mtu size */
#define NV_PKTLIMIT_1	ETH_DATA_LEN	/* hard limit not known */
#define NV_PKTLIMIT_2	9100	/* Actual limit according to NVidia: 9202 */

#define OOM_REFILL	(1+HZ/20)
#define POLL_WAIT	(1+HZ/100)
#define LINK_TIMEOUT	(3*HZ)
#define STATS_INTERVAL	(10*HZ)

/*
 * desc_ver values:
 * The nic supports three different descriptor types:
 * - DESC_VER_1: Original
 * - DESC_VER_2: support for jumbo frames.
 * - DESC_VER_3: 64-bit format.
 */
#define DESC_VER_1	1
#define DESC_VER_2	2
#define DESC_VER_3	3

/* PHY defines */
#define PHY_OUI_MARVELL	0x5043
#define PHY_OUI_CICADA	0x03f1
#define PHY_OUI_VITESSE	0x01c1
#define PHY_OUI_REALTEK	0x0732
#define PHYID1_OUI_MASK	0x03ff
#define PHYID1_OUI_SHFT	6
#define PHYID2_OUI_MASK	0xfc00
#define PHYID2_OUI_SHFT	10
#define PHYID2_MODEL_MASK		0x03f0
#define PHY_MODEL_MARVELL_E3016		0x220
#define PHY_MARVELL_E3016_INITMASK	0x0300
#define PHY_CICADA_INIT1	0x0f000
#define PHY_CICADA_INIT2	0x0e00
#define PHY_CICADA_INIT3	0x01000
#define PHY_CICADA_INIT4	0x0200
#define PHY_CICADA_INIT5	0x0004
#define PHY_CICADA_INIT6	0x02000
#define PHY_VITESSE_INIT_REG1	0x1f
#define PHY_VITESSE_INIT_REG2	0x10
#define PHY_VITESSE_INIT_REG3	0x11
#define PHY_VITESSE_INIT_REG4	0x12
#define PHY_VITESSE_INIT_MSK1	0xc
#define PHY_VITESSE_INIT_MSK2	0x0180
#define PHY_VITESSE_INIT1	0x52b5
#define PHY_VITESSE_INIT2	0xaf8a
#define PHY_VITESSE_INIT3	0x8
#define PHY_VITESSE_INIT4	0x8f8a
#define PHY_VITESSE_INIT5	0xaf86
#define PHY_VITESSE_INIT6	0x8f86
#define PHY_VITESSE_INIT7	0xaf82
#define PHY_VITESSE_INIT8	0x0100
#define PHY_VITESSE_INIT9	0x8f82
#define PHY_VITESSE_INIT10	0x0
#define PHY_REALTEK_INIT_REG1	0x1f
#define PHY_REALTEK_INIT_REG2	0x19
#define PHY_REALTEK_INIT_REG3	0x13
#define PHY_REALTEK_INIT1	0x0000
#define PHY_REALTEK_INIT2	0x8e00
#define PHY_REALTEK_INIT3	0x0001
#define PHY_REALTEK_INIT4	0xad17

#define PHY_GIGABIT	0x0100

#define PHY_TIMEOUT	0x1
#define PHY_ERROR	0x2

#define PHY_100	0x1
#define PHY_1000	0x2
#define PHY_HALF	0x100

#define NV_PAUSEFRAME_RX_CAPABLE 0x0001
#define NV_PAUSEFRAME_TX_CAPABLE 0x0002
#define NV_PAUSEFRAME_RX_ENABLE  0x0004
#define NV_PAUSEFRAME_TX_ENABLE  0x0008
#define NV_PAUSEFRAME_RX_REQ     0x0010
#define NV_PAUSEFRAME_TX_REQ     0x0020
#define NV_PAUSEFRAME_AUTONEG    0x0040

/* MSI/MSI-X defines */
#define NV_MSI_X_MAX_VECTORS  8
#define NV_MSI_X_VECTORS_MASK 0x000f
#define NV_MSI_CAPABLE        0x0010
#define NV_MSI_X_CAPABLE      0x0020
#define NV_MSI_ENABLED        0x0040
#define NV_MSI_X_ENABLED      0x0080

#define NV_MSI_X_VECTOR_ALL   0x0
#define NV_MSI_X_VECTOR_RX    0x0
#define NV_MSI_X_VECTOR_TX    0x1
#define NV_MSI_X_VECTOR_OTHER 0x2

/* statistics */
struct nv_ethtool_str {
	char name[ETH_GSTRING_LEN];
};

static const struct nv_ethtool_str nv_estats_str[] = {
	{ "tx_bytes" },
	{ "tx_zero_rexmt" },
	{ "tx_one_rexmt" },
	{ "tx_many_rexmt" },
	{ "tx_late_collision" },
	{ "tx_fifo_errors" },
	{ "tx_carrier_errors" },
	{ "tx_excess_deferral" },
	{ "tx_retry_error" },
	{ "rx_frame_error" },
	{ "rx_extra_byte" },
	{ "rx_late_collision" },
	{ "rx_runt" },
	{ "rx_frame_too_long" },
	{ "rx_over_errors" },
	{ "rx_crc_errors" },
	{ "rx_frame_align_error" },
	{ "rx_length_error" },
	{ "rx_unicast" },
	{ "rx_multicast" },
	{ "rx_broadcast" },
	{ "rx_packets" },
	{ "rx_errors_total" },
	{ "tx_errors_total" },

	/* version 2 stats */
	{ "tx_deferral" },
	{ "tx_packets" },
	{ "rx_bytes" },
	{ "tx_pause" },
	{ "rx_pause" },
	{ "rx_drop_frame" }
};

struct nv_ethtool_stats {
	u64 tx_bytes;
	u64 tx_zero_rexmt;
	u64 tx_one_rexmt;
	u64 tx_many_rexmt;
	u64 tx_late_collision;
	u64 tx_fifo_errors;
	u64 tx_carrier_errors;
	u64 tx_excess_deferral;
	u64 tx_retry_error;
	u64 rx_frame_error;
	u64 rx_extra_byte;
	u64 rx_late_collision;
	u64 rx_runt;
	u64 rx_frame_too_long;
	u64 rx_over_errors;
	u64 rx_crc_errors;
	u64 rx_frame_align_error;
	u64 rx_length_error;
	u64 rx_unicast;
	u64 rx_multicast;
	u64 rx_broadcast;
	u64 rx_packets;
	u64 rx_errors_total;
	u64 tx_errors_total;

	/* version 2 stats */
	u64 tx_deferral;
	u64 tx_packets;
	u64 rx_bytes;
	u64 tx_pause;
	u64 rx_pause;
	u64 rx_drop_frame;
};

#define NV_DEV_STATISTICS_V2_COUNT (sizeof(struct nv_ethtool_stats)/sizeof(u64))
#define NV_DEV_STATISTICS_V1_COUNT (NV_DEV_STATISTICS_V2_COUNT - 6)

/* diagnostics */
#define NV_TEST_COUNT_BASE 3
#define NV_TEST_COUNT_EXTENDED 4

static const struct nv_ethtool_str nv_etests_str[] = {
	{ "link      (online/offline)" },
	{ "register  (offline)       " },
	{ "interrupt (offline)       " },
	{ "loopback  (offline)       " }
};

struct register_test {
	__le32 reg;
	__le32 mask;
};

static const struct register_test nv_registers_test[] = {
	{ NvRegUnknownSetupReg6, 0x01 },
	{ NvRegMisc1, 0x03c },
	{ NvRegOffloadConfig, 0x03ff },
	{ NvRegMulticastAddrA, 0xffffffff },
	{ NvRegTxWatermark, 0x0ff },
	{ NvRegWakeUpFlags, 0x07777 },
	{ 0,0 }
};

struct nv_skb_map {
	struct sk_buff *skb;
	dma_addr_t dma;
	unsigned int dma_len;
};

/*
 * SMP locking:
 * All hardware access under dev->priv->lock, except the performance
 * critical parts:
 * - rx is (pseudo-) lockless: it relies on the single-threading provided
 *	by the arch code for interrupts.
 * - tx setup is lockless: it relies on netif_tx_lock. Actual submission
 *	needs dev->priv->lock :-(
 * - set_multicast_list: preparation lockless, relies on netif_tx_lock.
 */

/* in dev: base, irq */
struct fe_priv {
	spinlock_t lock;

	/* General data:
	 * Locking: spin_lock(&np->lock); */
	struct net_device_stats stats;
	struct nv_ethtool_stats estats;
	int in_shutdown;
	u32 linkspeed;
	int duplex;
	int autoneg;
	int fixed_mode;
	int phyaddr;
	int wolenabled;
	unsigned int phy_oui;
	unsigned int phy_model;
	u16 gigabit;
	int intr_test;
	int recover_error;

	/* General data: RO fields */
	dma_addr_t ring_addr;
	struct pci_dev *pci_dev;
	u32 orig_mac[2];
	u32 irqmask;
	u32 desc_ver;
	u32 txrxctl_bits;
	u32 vlanctl_bits;
	u32 driver_data;
	u32 register_size;
	int rx_csum;
	u32 mac_in_use;

	void __iomem *base;

	/* rx specific fields.
	 * Locking: Within irq hander or disable_irq+spin_lock(&np->lock);
	 */
	union ring_type get_rx, put_rx, first_rx, last_rx;
	struct nv_skb_map *get_rx_ctx, *put_rx_ctx;
	struct nv_skb_map *first_rx_ctx, *last_rx_ctx;
	struct nv_skb_map *rx_skb;

	union ring_type rx_ring;
	unsigned int rx_buf_sz;
	unsigned int pkt_limit;
	struct timer_list oom_kick;
	struct timer_list nic_poll;
	struct timer_list stats_poll;
	u32 nic_poll_irq;
	int rx_ring_size;

	/* media detection workaround.
	 * Locking: Within irq hander or disable_irq+spin_lock(&np->lock);
	 */
	int need_linktimer;
	unsigned long link_timeout;
	/*
	 * tx specific fields.
	 */
	union ring_type get_tx, put_tx, first_tx, last_tx;
	struct nv_skb_map *get_tx_ctx, *put_tx_ctx;
	struct nv_skb_map *first_tx_ctx, *last_tx_ctx;
	struct nv_skb_map *tx_skb;

	union ring_type tx_ring;
	u32 tx_flags;
	int tx_ring_size;
	int tx_stop;

	/* vlan fields */
	struct vlan_group *vlangrp;

	/* msi/msi-x fields */
	u32 msi_flags;
	struct msix_entry msi_x_entry[NV_MSI_X_MAX_VECTORS];

	/* flow control */
	u32 pause_flags;
};

/*
 * Maximum number of loops until we assume that a bit in the irq mask
 * is stuck. Overridable with module param.
 */
static int max_interrupt_work = 5;

/*
 * Optimization can be either throuput mode or cpu mode
 *
 * Throughput Mode: Every tx and rx packet will generate an interrupt.
 * CPU Mode: Interrupts are controlled by a timer.
 */
enum {
	NV_OPTIMIZATION_MODE_THROUGHPUT,
	NV_OPTIMIZATION_MODE_CPU
};
static int optimization_mode = NV_OPTIMIZATION_MODE_THROUGHPUT;

/*
 * Poll interval for timer irq
 *
 * This interval determines how frequent an interrupt is generated.
 * The is value is determined by [(time_in_micro_secs * 100) / (2^10)]
 * Min = 0, and Max = 65535
 */
static int poll_interval = -1;

/*
 * MSI interrupts
 */
enum {
	NV_MSI_INT_DISABLED,
	NV_MSI_INT_ENABLED
};
static int msi = NV_MSI_INT_ENABLED;

/*
 * MSIX interrupts
 */
enum {
	NV_MSIX_INT_DISABLED,
	NV_MSIX_INT_ENABLED
};
static int msix = NV_MSIX_INT_DISABLED;

/*
 * DMA 64bit
 */
enum {
	NV_DMA_64BIT_DISABLED,
	NV_DMA_64BIT_ENABLED
};
static int dma_64bit = NV_DMA_64BIT_ENABLED;

static inline struct fe_priv *get_nvpriv(struct net_device *dev)
{
	return netdev_priv(dev);
}

static inline u8 __iomem *get_hwbase(struct net_device *dev)
{
	return ((struct fe_priv *)netdev_priv(dev))->base;
}

static inline void pci_push(u8 __iomem *base)
{
	/* force out pending posted writes */
	readl(base);
}

static inline u32 nv_descr_getlength(struct ring_desc *prd, u32 v)
{
	return le32_to_cpu(prd->flaglen)
		& ((v == DESC_VER_1) ? LEN_MASK_V1 : LEN_MASK_V2);
}

static inline u32 nv_descr_getlength_ex(struct ring_desc_ex *prd, u32 v)
{
	return le32_to_cpu(prd->flaglen) & LEN_MASK_V2;
}

static int reg_delay(struct net_device *dev, int offset, u32 mask, u32 target,
				int delay, int delaymax, const char *msg)
{
	u8 __iomem *base = get_hwbase(dev);

	pci_push(base);
	do {
		udelay(delay);
		delaymax -= delay;
		if (delaymax < 0) {
			if (msg)
				printk(msg);
			return 1;
		}
	} while ((readl(base + offset) & mask) != target);
	return 0;
}

#define NV_SETUP_RX_RING 0x01
#define NV_SETUP_TX_RING 0x02

static void setup_hw_rings(struct net_device *dev, int rxtx_flags)
{
	struct fe_priv *np = get_nvpriv(dev);
	u8 __iomem *base = get_hwbase(dev);

	if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2) {
		if (rxtx_flags & NV_SETUP_RX_RING) {
			writel((u32) cpu_to_le64(np->ring_addr), base + NvRegRxRingPhysAddr);
		}
		if (rxtx_flags & NV_SETUP_TX_RING) {
			writel((u32) cpu_to_le64(np->ring_addr + np->rx_ring_size*sizeof(struct ring_desc)), base + NvRegTxRingPhysAddr);
		}
	} else {
		if (rxtx_flags & NV_SETUP_RX_RING) {
			writel((u32) cpu_to_le64(np->ring_addr), base + NvRegRxRingPhysAddr);
			writel((u32) (cpu_to_le64(np->ring_addr) >> 32), base + NvRegRxRingPhysAddrHigh);
		}
		if (rxtx_flags & NV_SETUP_TX_RING) {
			writel((u32) cpu_to_le64(np->ring_addr + np->rx_ring_size*sizeof(struct ring_desc_ex)), base + NvRegTxRingPhysAddr);
			writel((u32) (cpu_to_le64(np->ring_addr + np->rx_ring_size*sizeof(struct ring_desc_ex)) >> 32), base + NvRegTxRingPhysAddrHigh);
		}
	}
}

static void free_rings(struct net_device *dev)
{
	struct fe_priv *np = get_nvpriv(dev);

	if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2) {
		if (np->rx_ring.orig)
			pci_free_consistent(np->pci_dev, sizeof(struct ring_desc) * (np->rx_ring_size + np->tx_ring_size),
					    np->rx_ring.orig, np->ring_addr);
	} else {
		if (np->rx_ring.ex)
			pci_free_consistent(np->pci_dev, sizeof(struct ring_desc_ex) * (np->rx_ring_size + np->tx_ring_size),
					    np->rx_ring.ex, np->ring_addr);
	}
	if (np->rx_skb)
		kfree(np->rx_skb);
	if (np->tx_skb)
		kfree(np->tx_skb);
}

static int using_multi_irqs(struct net_device *dev)
{
	struct fe_priv *np = get_nvpriv(dev);

	if (!(np->msi_flags & NV_MSI_X_ENABLED) ||
	    ((np->msi_flags & NV_MSI_X_ENABLED) &&
	     ((np->msi_flags & NV_MSI_X_VECTORS_MASK) == 0x1)))
		return 0;
	else
		return 1;
}

static void nv_enable_irq(struct net_device *dev)
{
	struct fe_priv *np = get_nvpriv(dev);

	if (!using_multi_irqs(dev)) {
		if (np->msi_flags & NV_MSI_X_ENABLED)
			enable_irq(np->msi_x_entry[NV_MSI_X_VECTOR_ALL].vector);
		else
			enable_irq(dev->irq);
	} else {
		enable_irq(np->msi_x_entry[NV_MSI_X_VECTOR_RX].vector);
		enable_irq(np->msi_x_entry[NV_MSI_X_VECTOR_TX].vector);
		enable_irq(np->msi_x_entry[NV_MSI_X_VECTOR_OTHER].vector);
	}
}

static void nv_disable_irq(struct net_device *dev)
{
	struct fe_priv *np = get_nvpriv(dev);

	if (!using_multi_irqs(dev)) {
		if (np->msi_flags & NV_MSI_X_ENABLED)
			disable_irq(np->msi_x_entry[NV_MSI_X_VECTOR_ALL].vector);
		else
			disable_irq(dev->irq);
	} else {
		disable_irq(np->msi_x_entry[NV_MSI_X_VECTOR_RX].vector);
		disable_irq(np->msi_x_entry[NV_MSI_X_VECTOR_TX].vector);
		disable_irq(np->msi_x_entry[NV_MSI_X_VECTOR_OTHER].vector);
	}
}

/* In MSIX mode, a write to irqmask behaves as XOR */
static void nv_enable_hw_interrupts(struct net_device *dev, u32 mask)
{
	u8 __iomem *base = get_hwbase(dev);

	writel(mask, base + NvRegIrqMask);
}

static void nv_disable_hw_interrupts(struct net_device *dev, u32 mask)
{
	struct fe_priv *np = get_nvpriv(dev);
	u8 __iomem *base = get_hwbase(dev);

	if (np->msi_flags & NV_MSI_X_ENABLED) {
		writel(mask, base + NvRegIrqMask);
	} else {
		if (np->msi_flags & NV_MSI_ENABLED)
			writel(0, base + NvRegMSIIrqMask);
		writel(0, base + NvRegIrqMask);
	}
}

#define MII_READ	(-1)
/* mii_rw: read/write a register on the PHY.
 *
 * Caller must guarantee serialization
 */
static int mii_rw(struct net_device *dev, int addr, int miireg, int value)
{
	u8 __iomem *base = get_hwbase(dev);
	u32 reg;
	int retval;

	writel(NVREG_MIISTAT_MASK, base + NvRegMIIStatus);

	reg = readl(base + NvRegMIIControl);
	if (reg & NVREG_MIICTL_INUSE) {
		writel(NVREG_MIICTL_INUSE, base + NvRegMIIControl);
		udelay(NV_MIIBUSY_DELAY);
	}

	reg = (addr << NVREG_MIICTL_ADDRSHIFT) | miireg;
	if (value != MII_READ) {
		writel(value, base + NvRegMIIData);
		reg |= NVREG_MIICTL_WRITE;
	}
	writel(reg, base + NvRegMIIControl);

	if (reg_delay(dev, NvRegMIIControl, NVREG_MIICTL_INUSE, 0,
			NV_MIIPHY_DELAY, NV_MIIPHY_DELAYMAX, NULL)) {
		dprintk(KERN_DEBUG "%s: mii_rw of reg %d at PHY %d timed out.\n",
				dev->name, miireg, addr);
		retval = -1;
	} else if (value != MII_READ) {
		/* it was a write operation - fewer failures are detectable */
		dprintk(KERN_DEBUG "%s: mii_rw wrote 0x%x to reg %d at PHY %d\n",
				dev->name, value, miireg, addr);
		retval = 0;
	} else if (readl(base + NvRegMIIStatus) & NVREG_MIISTAT_ERROR) {
		dprintk(KERN_DEBUG "%s: mii_rw of reg %d at PHY %d failed.\n",
				dev->name, miireg, addr);
		retval = -1;
	} else {
		retval = readl(base + NvRegMIIData);
		dprintk(KERN_DEBUG "%s: mii_rw read from reg %d at PHY %d: 0x%x.\n",
				dev->name, miireg, addr, retval);
	}

	return retval;
}

static int phy_reset(struct net_device *dev, u32 bmcr_setup)
{
	struct fe_priv *np = netdev_priv(dev);
	u32 miicontrol;
	unsigned int tries = 0;

	miicontrol = BMCR_RESET | bmcr_setup;
	if (mii_rw(dev, np->phyaddr, MII_BMCR, miicontrol)) {
		return -1;
	}

	/* wait for 500ms */
	msleep(500);

	/* must wait till reset is deasserted */
	while (miicontrol & BMCR_RESET) {
		msleep(10);
		miicontrol = mii_rw(dev, np->phyaddr, MII_BMCR, MII_READ);
		/* FIXME: 100 tries seem excessive */
		if (tries++ > 100)
			return -1;
	}
	return 0;
}

static int phy_init(struct net_device *dev)
{
	struct fe_priv *np = get_nvpriv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u32 phyinterface, phy_reserved, mii_status, mii_control, mii_control_1000,reg;

	/* phy errata for E3016 phy */
	if (np->phy_model == PHY_MODEL_MARVELL_E3016) {
		reg = mii_rw(dev, np->phyaddr, MII_NCONFIG, MII_READ);
		reg &= ~PHY_MARVELL_E3016_INITMASK;
		if (mii_rw(dev, np->phyaddr, MII_NCONFIG, reg)) {
			printk(KERN_INFO "%s: phy write to errata reg failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
	}
	if (np->phy_oui == PHY_OUI_REALTEK) {
		if (mii_rw(dev, np->phyaddr, PHY_REALTEK_INIT_REG1, PHY_REALTEK_INIT1)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		if (mii_rw(dev, np->phyaddr, PHY_REALTEK_INIT_REG2, PHY_REALTEK_INIT2)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		if (mii_rw(dev, np->phyaddr, PHY_REALTEK_INIT_REG1, PHY_REALTEK_INIT3)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		if (mii_rw(dev, np->phyaddr, PHY_REALTEK_INIT_REG3, PHY_REALTEK_INIT4)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		if (mii_rw(dev, np->phyaddr, PHY_REALTEK_INIT_REG1, PHY_REALTEK_INIT1)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
	}

	/* set advertise register */
	reg = mii_rw(dev, np->phyaddr, MII_ADVERTISE, MII_READ);
	reg |= (ADVERTISE_10HALF|ADVERTISE_10FULL|ADVERTISE_100HALF|ADVERTISE_100FULL|ADVERTISE_PAUSE_ASYM|ADVERTISE_PAUSE_CAP);
	if (mii_rw(dev, np->phyaddr, MII_ADVERTISE, reg)) {
		printk(KERN_INFO "%s: phy write to advertise failed.\n", pci_name(np->pci_dev));
		return PHY_ERROR;
	}

	/* get phy interface type */
	phyinterface = readl(base + NvRegPhyInterface);

	/* see if gigabit phy */
	mii_status = mii_rw(dev, np->phyaddr, MII_BMSR, MII_READ);
	if (mii_status & PHY_GIGABIT) {
		np->gigabit = PHY_GIGABIT;
		mii_control_1000 = mii_rw(dev, np->phyaddr, MII_CTRL1000, MII_READ);
		mii_control_1000 &= ~ADVERTISE_1000HALF;
		if (phyinterface & PHY_RGMII)
			mii_control_1000 |= ADVERTISE_1000FULL;
		else
			mii_control_1000 &= ~ADVERTISE_1000FULL;

		if (mii_rw(dev, np->phyaddr, MII_CTRL1000, mii_control_1000)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
	}
	else
		np->gigabit = 0;

	mii_control = mii_rw(dev, np->phyaddr, MII_BMCR, MII_READ);
	mii_control |= BMCR_ANENABLE;

	/* reset the phy
	 * (certain phys need bmcr to be setup with reset)
	 */
	if (phy_reset(dev, mii_control)) {
		printk(KERN_INFO "%s: phy reset failed\n", pci_name(np->pci_dev));
		return PHY_ERROR;
	}

	/* phy vendor specific configuration */
	if ((np->phy_oui == PHY_OUI_CICADA) && (phyinterface & PHY_RGMII) ) {
		phy_reserved = mii_rw(dev, np->phyaddr, MII_RESV1, MII_READ);
		phy_reserved &= ~(PHY_CICADA_INIT1 | PHY_CICADA_INIT2);
		phy_reserved |= (PHY_CICADA_INIT3 | PHY_CICADA_INIT4);
		if (mii_rw(dev, np->phyaddr, MII_RESV1, phy_reserved)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		phy_reserved = mii_rw(dev, np->phyaddr, MII_NCONFIG, MII_READ);
		phy_reserved |= PHY_CICADA_INIT5;
		if (mii_rw(dev, np->phyaddr, MII_NCONFIG, phy_reserved)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
	}
	if (np->phy_oui == PHY_OUI_CICADA) {
		phy_reserved = mii_rw(dev, np->phyaddr, MII_SREVISION, MII_READ);
		phy_reserved |= PHY_CICADA_INIT6;
		if (mii_rw(dev, np->phyaddr, MII_SREVISION, phy_reserved)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
	}
	if (np->phy_oui == PHY_OUI_VITESSE) {
		if (mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG1, PHY_VITESSE_INIT1)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		if (mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG2, PHY_VITESSE_INIT2)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		phy_reserved = mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG4, MII_READ);
		if (mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG4, phy_reserved)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		phy_reserved = mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG3, MII_READ);
		phy_reserved &= ~PHY_VITESSE_INIT_MSK1;
		phy_reserved |= PHY_VITESSE_INIT3;
		if (mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG3, phy_reserved)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		if (mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG2, PHY_VITESSE_INIT4)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		if (mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG2, PHY_VITESSE_INIT5)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		phy_reserved = mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG4, MII_READ);
		phy_reserved &= ~PHY_VITESSE_INIT_MSK1;
		phy_reserved |= PHY_VITESSE_INIT3;
		if (mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG4, phy_reserved)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		phy_reserved = mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG3, MII_READ);
		if (mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG3, phy_reserved)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		if (mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG2, PHY_VITESSE_INIT6)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		if (mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG2, PHY_VITESSE_INIT7)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		phy_reserved = mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG4, MII_READ);
		if (mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG4, phy_reserved)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		phy_reserved = mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG3, MII_READ);
		phy_reserved &= ~PHY_VITESSE_INIT_MSK2;
		phy_reserved |= PHY_VITESSE_INIT8;
		if (mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG3, phy_reserved)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		if (mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG2, PHY_VITESSE_INIT9)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		if (mii_rw(dev, np->phyaddr, PHY_VITESSE_INIT_REG1, PHY_VITESSE_INIT10)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
	}
	if (np->phy_oui == PHY_OUI_REALTEK) {
		/* reset could have cleared these out, set them back */
		if (mii_rw(dev, np->phyaddr, PHY_REALTEK_INIT_REG1, PHY_REALTEK_INIT1)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		if (mii_rw(dev, np->phyaddr, PHY_REALTEK_INIT_REG2, PHY_REALTEK_INIT2)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		if (mii_rw(dev, np->phyaddr, PHY_REALTEK_INIT_REG1, PHY_REALTEK_INIT3)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		if (mii_rw(dev, np->phyaddr, PHY_REALTEK_INIT_REG3, PHY_REALTEK_INIT4)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
		if (mii_rw(dev, np->phyaddr, PHY_REALTEK_INIT_REG1, PHY_REALTEK_INIT1)) {
			printk(KERN_INFO "%s: phy init failed.\n", pci_name(np->pci_dev));
			return PHY_ERROR;
		}
	}

	/* some phys clear out pause advertisment on reset, set it back */
	mii_rw(dev, np->phyaddr, MII_ADVERTISE, reg);

	/* restart auto negotiation */
	mii_control = mii_rw(dev, np->phyaddr, MII_BMCR, MII_READ);
	mii_control |= (BMCR_ANRESTART | BMCR_ANENABLE);
	if (mii_rw(dev, np->phyaddr, MII_BMCR, mii_control)) {
		return PHY_ERROR;
	}

	return 0;
}

static void nv_start_rx(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u32 rx_ctrl = readl(base + NvRegReceiverControl);

	dprintk(KERN_DEBUG "%s: nv_start_rx\n", dev->name);
	/* Already running? Stop it. */
	if ((readl(base + NvRegReceiverControl) & NVREG_RCVCTL_START) && !np->mac_in_use) {
		rx_ctrl &= ~NVREG_RCVCTL_START;
		writel(rx_ctrl, base + NvRegReceiverControl);
		pci_push(base);
	}
	writel(np->linkspeed, base + NvRegLinkSpeed);
	pci_push(base);
        rx_ctrl |= NVREG_RCVCTL_START;
        if (np->mac_in_use)
		rx_ctrl &= ~NVREG_RCVCTL_RX_PATH_EN;
	writel(rx_ctrl, base + NvRegReceiverControl);
	dprintk(KERN_DEBUG "%s: nv_start_rx to duplex %d, speed 0x%08x.\n",
				dev->name, np->duplex, np->linkspeed);
	pci_push(base);
}

static void nv_stop_rx(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u32 rx_ctrl = readl(base + NvRegReceiverControl);

	dprintk(KERN_DEBUG "%s: nv_stop_rx\n", dev->name);
	if (!np->mac_in_use)
		rx_ctrl &= ~NVREG_RCVCTL_START;
	else
		rx_ctrl |= NVREG_RCVCTL_RX_PATH_EN;
	writel(rx_ctrl, base + NvRegReceiverControl);
	reg_delay(dev, NvRegReceiverStatus, NVREG_RCVSTAT_BUSY, 0,
			NV_RXSTOP_DELAY1, NV_RXSTOP_DELAY1MAX,
			KERN_INFO "nv_stop_rx: ReceiverStatus remained busy");

	udelay(NV_RXSTOP_DELAY2);
	if (!np->mac_in_use)
		writel(0, base + NvRegLinkSpeed);
}

static void nv_start_tx(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u32 tx_ctrl = readl(base + NvRegTransmitterControl);

	dprintk(KERN_DEBUG "%s: nv_start_tx\n", dev->name);
	tx_ctrl |= NVREG_XMITCTL_START;
	if (np->mac_in_use)
		tx_ctrl &= ~NVREG_XMITCTL_TX_PATH_EN;
	writel(tx_ctrl, base + NvRegTransmitterControl);
	pci_push(base);
}

static void nv_stop_tx(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u32 tx_ctrl = readl(base + NvRegTransmitterControl);

	dprintk(KERN_DEBUG "%s: nv_stop_tx\n", dev->name);
	if (!np->mac_in_use)
		tx_ctrl &= ~NVREG_XMITCTL_START;
	else
		tx_ctrl |= NVREG_XMITCTL_TX_PATH_EN;
	writel(tx_ctrl, base + NvRegTransmitterControl);
	reg_delay(dev, NvRegTransmitterStatus, NVREG_XMITSTAT_BUSY, 0,
			NV_TXSTOP_DELAY1, NV_TXSTOP_DELAY1MAX,
			KERN_INFO "nv_stop_tx: TransmitterStatus remained busy");

	udelay(NV_TXSTOP_DELAY2);
	if (!np->mac_in_use)
		writel(readl(base + NvRegTransmitPoll) & NVREG_TRANSMITPOLL_MAC_ADDR_REV,
		       base + NvRegTransmitPoll);
}

static void nv_txrx_reset(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);

	dprintk(KERN_DEBUG "%s: nv_txrx_reset\n", dev->name);
	writel(NVREG_TXRXCTL_BIT2 | NVREG_TXRXCTL_RESET | np->txrxctl_bits, base + NvRegTxRxControl);
	pci_push(base);
	udelay(NV_TXRX_RESET_DELAY);
	writel(NVREG_TXRXCTL_BIT2 | np->txrxctl_bits, base + NvRegTxRxControl);
	pci_push(base);
}

static void nv_mac_reset(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);

	dprintk(KERN_DEBUG "%s: nv_mac_reset\n", dev->name);
	writel(NVREG_TXRXCTL_BIT2 | NVREG_TXRXCTL_RESET | np->txrxctl_bits, base + NvRegTxRxControl);
	pci_push(base);
	writel(NVREG_MAC_RESET_ASSERT, base + NvRegMacReset);
	pci_push(base);
	udelay(NV_MAC_RESET_DELAY);
	writel(0, base + NvRegMacReset);
	pci_push(base);
	udelay(NV_MAC_RESET_DELAY);
	writel(NVREG_TXRXCTL_BIT2 | np->txrxctl_bits, base + NvRegTxRxControl);
	pci_push(base);
}

static void nv_get_hw_stats(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);

	np->estats.tx_bytes += readl(base + NvRegTxCnt);
	np->estats.tx_zero_rexmt += readl(base + NvRegTxZeroReXmt);
	np->estats.tx_one_rexmt += readl(base + NvRegTxOneReXmt);
	np->estats.tx_many_rexmt += readl(base + NvRegTxManyReXmt);
	np->estats.tx_late_collision += readl(base + NvRegTxLateCol);
	np->estats.tx_fifo_errors += readl(base + NvRegTxUnderflow);
	np->estats.tx_carrier_errors += readl(base + NvRegTxLossCarrier);
	np->estats.tx_excess_deferral += readl(base + NvRegTxExcessDef);
	np->estats.tx_retry_error += readl(base + NvRegTxRetryErr);
	np->estats.rx_frame_error += readl(base + NvRegRxFrameErr);
	np->estats.rx_extra_byte += readl(base + NvRegRxExtraByte);
	np->estats.rx_late_collision += readl(base + NvRegRxLateCol);
	np->estats.rx_runt += readl(base + NvRegRxRunt);
	np->estats.rx_frame_too_long += readl(base + NvRegRxFrameTooLong);
	np->estats.rx_over_errors += readl(base + NvRegRxOverflow);
	np->estats.rx_crc_errors += readl(base + NvRegRxFCSErr);
	np->estats.rx_frame_align_error += readl(base + NvRegRxFrameAlignErr);
	np->estats.rx_length_error += readl(base + NvRegRxLenErr);
	np->estats.rx_unicast += readl(base + NvRegRxUnicast);
	np->estats.rx_multicast += readl(base + NvRegRxMulticast);
	np->estats.rx_broadcast += readl(base + NvRegRxBroadcast);
	np->estats.rx_packets =
		np->estats.rx_unicast +
		np->estats.rx_multicast +
		np->estats.rx_broadcast;
	np->estats.rx_errors_total =
		np->estats.rx_crc_errors +
		np->estats.rx_over_errors +
		np->estats.rx_frame_error +
		(np->estats.rx_frame_align_error - np->estats.rx_extra_byte) +
		np->estats.rx_late_collision +
		np->estats.rx_runt +
		np->estats.rx_frame_too_long;
	np->estats.tx_errors_total =
		np->estats.tx_late_collision +
		np->estats.tx_fifo_errors +
		np->estats.tx_carrier_errors +
		np->estats.tx_excess_deferral +
		np->estats.tx_retry_error;

	if (np->driver_data & DEV_HAS_STATISTICS_V2) {
		np->estats.tx_deferral += readl(base + NvRegTxDef);
		np->estats.tx_packets += readl(base + NvRegTxFrame);
		np->estats.rx_bytes += readl(base + NvRegRxCnt);
		np->estats.tx_pause += readl(base + NvRegTxPause);
		np->estats.rx_pause += readl(base + NvRegRxPause);
		np->estats.rx_drop_frame += readl(base + NvRegRxDropFrame);
	}
}

/*
 * nv_get_stats: dev->get_stats function
 * Get latest stats value from the nic.
 * Called with read_lock(&dev_base_lock) held for read -
 * only synchronized against unregister_netdevice.
 */
static struct net_device_stats *nv_get_stats(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);

	/* If the nic supports hw counters then retrieve latest values */
	if (np->driver_data & (DEV_HAS_STATISTICS_V1|DEV_HAS_STATISTICS_V2)) {
		nv_get_hw_stats(dev);

		/* copy to net_device stats */
		np->stats.tx_bytes = np->estats.tx_bytes;
		np->stats.tx_fifo_errors = np->estats.tx_fifo_errors;
		np->stats.tx_carrier_errors = np->estats.tx_carrier_errors;
		np->stats.rx_crc_errors = np->estats.rx_crc_errors;
		np->stats.rx_over_errors = np->estats.rx_over_errors;
		np->stats.rx_errors = np->estats.rx_errors_total;
		np->stats.tx_errors = np->estats.tx_errors_total;
	}
	return &np->stats;
}

/*
 * nv_alloc_rx: fill rx ring entries.
 * Return 1 if the allocations for the skbs failed and the
 * rx engine is without Available descriptors
 */
static int nv_alloc_rx(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	struct ring_desc* less_rx;

	less_rx = np->get_rx.orig;
	if (less_rx-- == np->first_rx.orig)
		less_rx = np->last_rx.orig;

	while (np->put_rx.orig != less_rx) {
		struct sk_buff *skb = dev_alloc_skb(np->rx_buf_sz + NV_RX_ALLOC_PAD);
		if (skb) {
			np->put_rx_ctx->skb = skb;
			np->put_rx_ctx->dma = pci_map_single(np->pci_dev,
							     skb->data,
							     skb_tailroom(skb),
							     PCI_DMA_FROMDEVICE);
			np->put_rx_ctx->dma_len = skb_tailroom(skb);
			np->put_rx.orig->buf = cpu_to_le32(np->put_rx_ctx->dma);
			wmb();
			np->put_rx.orig->flaglen = cpu_to_le32(np->rx_buf_sz | NV_RX_AVAIL);
			if (unlikely(np->put_rx.orig++ == np->last_rx.orig))
				np->put_rx.orig = np->first_rx.orig;
			if (unlikely(np->put_rx_ctx++ == np->last_rx_ctx))
				np->put_rx_ctx = np->first_rx_ctx;
		} else {
			return 1;
		}
	}
	return 0;
}

static int nv_alloc_rx_optimized(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	struct ring_desc_ex* less_rx;

	less_rx = np->get_rx.ex;
	if (less_rx-- == np->first_rx.ex)
		less_rx = np->last_rx.ex;

	while (np->put_rx.ex != less_rx) {
		struct sk_buff *skb = dev_alloc_skb(np->rx_buf_sz + NV_RX_ALLOC_PAD);
		if (skb) {
			np->put_rx_ctx->skb = skb;
			np->put_rx_ctx->dma = pci_map_single(np->pci_dev,
							     skb->data,
							     skb_tailroom(skb),
							     PCI_DMA_FROMDEVICE);
			np->put_rx_ctx->dma_len = skb_tailroom(skb);
			np->put_rx.ex->bufhigh = cpu_to_le64(np->put_rx_ctx->dma) >> 32;
			np->put_rx.ex->buflow = cpu_to_le64(np->put_rx_ctx->dma) & 0x0FFFFFFFF;
			wmb();
			np->put_rx.ex->flaglen = cpu_to_le32(np->rx_buf_sz | NV_RX2_AVAIL);
			if (unlikely(np->put_rx.ex++ == np->last_rx.ex))
				np->put_rx.ex = np->first_rx.ex;
			if (unlikely(np->put_rx_ctx++ == np->last_rx_ctx))
				np->put_rx_ctx = np->first_rx_ctx;
		} else {
			return 1;
		}
	}
	return 0;
}

/* If rx bufs are exhausted called after 50ms to attempt to refresh */
#ifdef CONFIG_FORCEDETH_NAPI
static void nv_do_rx_refill(unsigned long data)
{
	struct net_device *dev = (struct net_device *) data;

	/* Just reschedule NAPI rx processing */
	netif_rx_schedule(dev);
}
#else
static void nv_do_rx_refill(unsigned long data)
{
	struct net_device *dev = (struct net_device *) data;
	struct fe_priv *np = netdev_priv(dev);
	int retcode;

	if (!using_multi_irqs(dev)) {
		if (np->msi_flags & NV_MSI_X_ENABLED)
			disable_irq(np->msi_x_entry[NV_MSI_X_VECTOR_ALL].vector);
		else
			disable_irq(dev->irq);
	} else {
		disable_irq(np->msi_x_entry[NV_MSI_X_VECTOR_RX].vector);
	}
	if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2)
		retcode = nv_alloc_rx(dev);
	else
		retcode = nv_alloc_rx_optimized(dev);
	if (retcode) {
		spin_lock_irq(&np->lock);
		if (!np->in_shutdown)
			mod_timer(&np->oom_kick, jiffies + OOM_REFILL);
		spin_unlock_irq(&np->lock);
	}
	if (!using_multi_irqs(dev)) {
		if (np->msi_flags & NV_MSI_X_ENABLED)
			enable_irq(np->msi_x_entry[NV_MSI_X_VECTOR_ALL].vector);
		else
			enable_irq(dev->irq);
	} else {
		enable_irq(np->msi_x_entry[NV_MSI_X_VECTOR_RX].vector);
	}
}
#endif

static void nv_init_rx(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	int i;
	np->get_rx = np->put_rx = np->first_rx = np->rx_ring;
	if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2)
		np->last_rx.orig = &np->rx_ring.orig[np->rx_ring_size-1];
	else
		np->last_rx.ex = &np->rx_ring.ex[np->rx_ring_size-1];
	np->get_rx_ctx = np->put_rx_ctx = np->first_rx_ctx = np->rx_skb;
	np->last_rx_ctx = &np->rx_skb[np->rx_ring_size-1];

	for (i = 0; i < np->rx_ring_size; i++) {
		if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2) {
			np->rx_ring.orig[i].flaglen = 0;
			np->rx_ring.orig[i].buf = 0;
		} else {
			np->rx_ring.ex[i].flaglen = 0;
			np->rx_ring.ex[i].txvlan = 0;
			np->rx_ring.ex[i].bufhigh = 0;
			np->rx_ring.ex[i].buflow = 0;
		}
		np->rx_skb[i].skb = NULL;
		np->rx_skb[i].dma = 0;
	}
}

static void nv_init_tx(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	int i;
	np->get_tx = np->put_tx = np->first_tx = np->tx_ring;
	if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2)
		np->last_tx.orig = &np->tx_ring.orig[np->tx_ring_size-1];
	else
		np->last_tx.ex = &np->tx_ring.ex[np->tx_ring_size-1];
	np->get_tx_ctx = np->put_tx_ctx = np->first_tx_ctx = np->tx_skb;
	np->last_tx_ctx = &np->tx_skb[np->tx_ring_size-1];

	for (i = 0; i < np->tx_ring_size; i++) {
		if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2) {
			np->tx_ring.orig[i].flaglen = 0;
			np->tx_ring.orig[i].buf = 0;
		} else {
			np->tx_ring.ex[i].flaglen = 0;
			np->tx_ring.ex[i].txvlan = 0;
			np->tx_ring.ex[i].bufhigh = 0;
			np->tx_ring.ex[i].buflow = 0;
		}
		np->tx_skb[i].skb = NULL;
		np->tx_skb[i].dma = 0;
	}
}

static int nv_init_ring(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);

	nv_init_tx(dev);
	nv_init_rx(dev);
	if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2)
		return nv_alloc_rx(dev);
	else
		return nv_alloc_rx_optimized(dev);
}

static int nv_release_txskb(struct net_device *dev, struct nv_skb_map* tx_skb)
{
	struct fe_priv *np = netdev_priv(dev);

	if (tx_skb->dma) {
		pci_unmap_page(np->pci_dev, tx_skb->dma,
			       tx_skb->dma_len,
			       PCI_DMA_TODEVICE);
		tx_skb->dma = 0;
	}
	if (tx_skb->skb) {
		dev_kfree_skb_any(tx_skb->skb);
		tx_skb->skb = NULL;
		return 1;
	} else {
		return 0;
	}
}

static void nv_drain_tx(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	unsigned int i;

	for (i = 0; i < np->tx_ring_size; i++) {
		if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2) {
			np->tx_ring.orig[i].flaglen = 0;
			np->tx_ring.orig[i].buf = 0;
		} else {
			np->tx_ring.ex[i].flaglen = 0;
			np->tx_ring.ex[i].txvlan = 0;
			np->tx_ring.ex[i].bufhigh = 0;
			np->tx_ring.ex[i].buflow = 0;
		}
		if (nv_release_txskb(dev, &np->tx_skb[i]))
			np->stats.tx_dropped++;
	}
}

static void nv_drain_rx(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	int i;

	for (i = 0; i < np->rx_ring_size; i++) {
		if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2) {
			np->rx_ring.orig[i].flaglen = 0;
			np->rx_ring.orig[i].buf = 0;
		} else {
			np->rx_ring.ex[i].flaglen = 0;
			np->rx_ring.ex[i].txvlan = 0;
			np->rx_ring.ex[i].bufhigh = 0;
			np->rx_ring.ex[i].buflow = 0;
		}
		wmb();
		if (np->rx_skb[i].skb) {
			pci_unmap_single(np->pci_dev, np->rx_skb[i].dma,
					 (skb_end_pointer(np->rx_skb[i].skb) -
					  np->rx_skb[i].skb->data),
					 PCI_DMA_FROMDEVICE);
			dev_kfree_skb(np->rx_skb[i].skb);
			np->rx_skb[i].skb = NULL;
		}
	}
}

static void drain_ring(struct net_device *dev)
{
	nv_drain_tx(dev);
	nv_drain_rx(dev);
}

static inline u32 nv_get_empty_tx_slots(struct fe_priv *np)
{
	return (u32)(np->tx_ring_size - ((np->tx_ring_size + (np->put_tx_ctx - np->get_tx_ctx)) % np->tx_ring_size));
}

/*
 * nv_start_xmit: dev->hard_start_xmit function
 * Called with netif_tx_lock held.
 */
static int nv_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u32 tx_flags = 0;
	u32 tx_flags_extra = (np->desc_ver == DESC_VER_1 ? NV_TX_LASTPACKET : NV_TX2_LASTPACKET);
	unsigned int fragments = skb_shinfo(skb)->nr_frags;
	unsigned int i;
	u32 offset = 0;
	u32 bcnt;
	u32 size = skb->len-skb->data_len;
	u32 entries = (size >> NV_TX2_TSO_MAX_SHIFT) + ((size & (NV_TX2_TSO_MAX_SIZE-1)) ? 1 : 0);
	u32 empty_slots;
	struct ring_desc* put_tx;
	struct ring_desc* start_tx;
	struct ring_desc* prev_tx;
	struct nv_skb_map* prev_tx_ctx;

	/* add fragments to entries count */
	for (i = 0; i < fragments; i++) {
		entries += (skb_shinfo(skb)->frags[i].size >> NV_TX2_TSO_MAX_SHIFT) +
			   ((skb_shinfo(skb)->frags[i].size & (NV_TX2_TSO_MAX_SIZE-1)) ? 1 : 0);
	}

	empty_slots = nv_get_empty_tx_slots(np);
	if (unlikely(empty_slots <= entries)) {
		spin_lock_irq(&np->lock);
		netif_stop_queue(dev);
		np->tx_stop = 1;
		spin_unlock_irq(&np->lock);
		return NETDEV_TX_BUSY;
	}

	start_tx = put_tx = np->put_tx.orig;

	/* setup the header buffer */
	do {
		prev_tx = put_tx;
		prev_tx_ctx = np->put_tx_ctx;
		bcnt = (size > NV_TX2_TSO_MAX_SIZE) ? NV_TX2_TSO_MAX_SIZE : size;
		np->put_tx_ctx->dma = pci_map_single(np->pci_dev, skb->data + offset, bcnt,
						PCI_DMA_TODEVICE);
		np->put_tx_ctx->dma_len = bcnt;
		put_tx->buf = cpu_to_le32(np->put_tx_ctx->dma);
		put_tx->flaglen = cpu_to_le32((bcnt-1) | tx_flags);

		tx_flags = np->tx_flags;
		offset += bcnt;
		size -= bcnt;
		if (unlikely(put_tx++ == np->last_tx.orig))
			put_tx = np->first_tx.orig;
		if (unlikely(np->put_tx_ctx++ == np->last_tx_ctx))
			np->put_tx_ctx = np->first_tx_ctx;
	} while (size);

	/* setup the fragments */
	for (i = 0; i < fragments; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		u32 size = frag->size;
		offset = 0;

		do {
			prev_tx = put_tx;
			prev_tx_ctx = np->put_tx_ctx;
			bcnt = (size > NV_TX2_TSO_MAX_SIZE) ? NV_TX2_TSO_MAX_SIZE : size;
			np->put_tx_ctx->dma = pci_map_page(np->pci_dev, frag->page, frag->page_offset+offset, bcnt,
							   PCI_DMA_TODEVICE);
			np->put_tx_ctx->dma_len = bcnt;
			put_tx->buf = cpu_to_le32(np->put_tx_ctx->dma);
			put_tx->flaglen = cpu_to_le32((bcnt-1) | tx_flags);

			offset += bcnt;
			size -= bcnt;
			if (unlikely(put_tx++ == np->last_tx.orig))
				put_tx = np->first_tx.orig;
			if (unlikely(np->put_tx_ctx++ == np->last_tx_ctx))
				np->put_tx_ctx = np->first_tx_ctx;
		} while (size);
	}

	/* set last fragment flag  */
	prev_tx->flaglen |= cpu_to_le32(tx_flags_extra);

	/* save skb in this slot's context area */
	prev_tx_ctx->skb = skb;

	if (skb_is_gso(skb))
		tx_flags_extra = NV_TX2_TSO | (skb_shinfo(skb)->gso_size << NV_TX2_TSO_SHIFT);
	else
		tx_flags_extra = skb->ip_summed == CHECKSUM_PARTIAL ?
			 NV_TX2_CHECKSUM_L3 | NV_TX2_CHECKSUM_L4 : 0;

	spin_lock_irq(&np->lock);

	/* set tx flags */
	start_tx->flaglen |= cpu_to_le32(tx_flags | tx_flags_extra);
	np->put_tx.orig = put_tx;

	spin_unlock_irq(&np->lock);

	dprintk(KERN_DEBUG "%s: nv_start_xmit: entries %d queued for transmission. tx_flags_extra: %x\n",
		dev->name, entries, tx_flags_extra);
	{
		int j;
		for (j=0; j<64; j++) {
			if ((j%16) == 0)
				dprintk("\n%03x:", j);
			dprintk(" %02x", ((unsigned char*)skb->data)[j]);
		}
		dprintk("\n");
	}

	dev->trans_start = jiffies;
	writel(NVREG_TXRXCTL_KICK|np->txrxctl_bits, get_hwbase(dev) + NvRegTxRxControl);
	return NETDEV_TX_OK;
}

static int nv_start_xmit_optimized(struct sk_buff *skb, struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u32 tx_flags = 0;
	u32 tx_flags_extra;
	unsigned int fragments = skb_shinfo(skb)->nr_frags;
	unsigned int i;
	u32 offset = 0;
	u32 bcnt;
	u32 size = skb->len-skb->data_len;
	u32 entries = (size >> NV_TX2_TSO_MAX_SHIFT) + ((size & (NV_TX2_TSO_MAX_SIZE-1)) ? 1 : 0);
	u32 empty_slots;
	struct ring_desc_ex* put_tx;
	struct ring_desc_ex* start_tx;
	struct ring_desc_ex* prev_tx;
	struct nv_skb_map* prev_tx_ctx;

	/* add fragments to entries count */
	for (i = 0; i < fragments; i++) {
		entries += (skb_shinfo(skb)->frags[i].size >> NV_TX2_TSO_MAX_SHIFT) +
			   ((skb_shinfo(skb)->frags[i].size & (NV_TX2_TSO_MAX_SIZE-1)) ? 1 : 0);
	}

	empty_slots = nv_get_empty_tx_slots(np);
	if (unlikely(empty_slots <= entries)) {
		spin_lock_irq(&np->lock);
		netif_stop_queue(dev);
		np->tx_stop = 1;
		spin_unlock_irq(&np->lock);
		return NETDEV_TX_BUSY;
	}

	start_tx = put_tx = np->put_tx.ex;

	/* setup the header buffer */
	do {
		prev_tx = put_tx;
		prev_tx_ctx = np->put_tx_ctx;
		bcnt = (size > NV_TX2_TSO_MAX_SIZE) ? NV_TX2_TSO_MAX_SIZE : size;
		np->put_tx_ctx->dma = pci_map_single(np->pci_dev, skb->data + offset, bcnt,
						PCI_DMA_TODEVICE);
		np->put_tx_ctx->dma_len = bcnt;
		put_tx->bufhigh = cpu_to_le64(np->put_tx_ctx->dma) >> 32;
		put_tx->buflow = cpu_to_le64(np->put_tx_ctx->dma) & 0x0FFFFFFFF;
		put_tx->flaglen = cpu_to_le32((bcnt-1) | tx_flags);

		tx_flags = NV_TX2_VALID;
		offset += bcnt;
		size -= bcnt;
		if (unlikely(put_tx++ == np->last_tx.ex))
			put_tx = np->first_tx.ex;
		if (unlikely(np->put_tx_ctx++ == np->last_tx_ctx))
			np->put_tx_ctx = np->first_tx_ctx;
	} while (size);

	/* setup the fragments */
	for (i = 0; i < fragments; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		u32 size = frag->size;
		offset = 0;

		do {
			prev_tx = put_tx;
			prev_tx_ctx = np->put_tx_ctx;
			bcnt = (size > NV_TX2_TSO_MAX_SIZE) ? NV_TX2_TSO_MAX_SIZE : size;
			np->put_tx_ctx->dma = pci_map_page(np->pci_dev, frag->page, frag->page_offset+offset, bcnt,
							   PCI_DMA_TODEVICE);
			np->put_tx_ctx->dma_len = bcnt;
			put_tx->bufhigh = cpu_to_le64(np->put_tx_ctx->dma) >> 32;
			put_tx->buflow = cpu_to_le64(np->put_tx_ctx->dma) & 0x0FFFFFFFF;
			put_tx->flaglen = cpu_to_le32((bcnt-1) | tx_flags);

			offset += bcnt;
			size -= bcnt;
			if (unlikely(put_tx++ == np->last_tx.ex))
				put_tx = np->first_tx.ex;
			if (unlikely(np->put_tx_ctx++ == np->last_tx_ctx))
				np->put_tx_ctx = np->first_tx_ctx;
		} while (size);
	}

	/* set last fragment flag  */
	prev_tx->flaglen |= cpu_to_le32(NV_TX2_LASTPACKET);

	/* save skb in this slot's context area */
	prev_tx_ctx->skb = skb;

	if (skb_is_gso(skb))
		tx_flags_extra = NV_TX2_TSO | (skb_shinfo(skb)->gso_size << NV_TX2_TSO_SHIFT);
	else
		tx_flags_extra = skb->ip_summed == CHECKSUM_PARTIAL ?
			 NV_TX2_CHECKSUM_L3 | NV_TX2_CHECKSUM_L4 : 0;

	/* vlan tag */
	if (likely(!np->vlangrp)) {
		start_tx->txvlan = 0;
	} else {
		if (vlan_tx_tag_present(skb))
			start_tx->txvlan = cpu_to_le32(NV_TX3_VLAN_TAG_PRESENT | vlan_tx_tag_get(skb));
		else
			start_tx->txvlan = 0;
	}

	spin_lock_irq(&np->lock);

	/* set tx flags */
	start_tx->flaglen |= cpu_to_le32(tx_flags | tx_flags_extra);
	np->put_tx.ex = put_tx;

	spin_unlock_irq(&np->lock);

	dprintk(KERN_DEBUG "%s: nv_start_xmit_optimized: entries %d queued for transmission. tx_flags_extra: %x\n",
		dev->name, entries, tx_flags_extra);
	{
		int j;
		for (j=0; j<64; j++) {
			if ((j%16) == 0)
				dprintk("\n%03x:", j);
			dprintk(" %02x", ((unsigned char*)skb->data)[j]);
		}
		dprintk("\n");
	}

	dev->trans_start = jiffies;
	writel(NVREG_TXRXCTL_KICK|np->txrxctl_bits, get_hwbase(dev) + NvRegTxRxControl);
	return NETDEV_TX_OK;
}

/*
 * nv_tx_done: check for completed packets, release the skbs.
 *
 * Caller must own np->lock.
 */
static void nv_tx_done(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u32 flags;
	struct ring_desc* orig_get_tx = np->get_tx.orig;

	while ((np->get_tx.orig != np->put_tx.orig) &&
	       !((flags = le32_to_cpu(np->get_tx.orig->flaglen)) & NV_TX_VALID)) {

		dprintk(KERN_DEBUG "%s: nv_tx_done: flags 0x%x.\n",
					dev->name, flags);

		pci_unmap_page(np->pci_dev, np->get_tx_ctx->dma,
			       np->get_tx_ctx->dma_len,
			       PCI_DMA_TODEVICE);
		np->get_tx_ctx->dma = 0;

		if (np->desc_ver == DESC_VER_1) {
			if (flags & NV_TX_LASTPACKET) {
				if (flags & NV_TX_ERROR) {
					if (flags & NV_TX_UNDERFLOW)
						np->stats.tx_fifo_errors++;
					if (flags & NV_TX_CARRIERLOST)
						np->stats.tx_carrier_errors++;
					np->stats.tx_errors++;
				} else {
					np->stats.tx_packets++;
					np->stats.tx_bytes += np->get_tx_ctx->skb->len;
				}
				dev_kfree_skb_any(np->get_tx_ctx->skb);
				np->get_tx_ctx->skb = NULL;
			}
		} else {
			if (flags & NV_TX2_LASTPACKET) {
				if (flags & NV_TX2_ERROR) {
					if (flags & NV_TX2_UNDERFLOW)
						np->stats.tx_fifo_errors++;
					if (flags & NV_TX2_CARRIERLOST)
						np->stats.tx_carrier_errors++;
					np->stats.tx_errors++;
				} else {
					np->stats.tx_packets++;
					np->stats.tx_bytes += np->get_tx_ctx->skb->len;
				}
				dev_kfree_skb_any(np->get_tx_ctx->skb);
				np->get_tx_ctx->skb = NULL;
			}
		}
		if (unlikely(np->get_tx.orig++ == np->last_tx.orig))
			np->get_tx.orig = np->first_tx.orig;
		if (unlikely(np->get_tx_ctx++ == np->last_tx_ctx))
			np->get_tx_ctx = np->first_tx_ctx;
	}
	if (unlikely((np->tx_stop == 1) && (np->get_tx.orig != orig_get_tx))) {
		np->tx_stop = 0;
		netif_wake_queue(dev);
	}
}

static void nv_tx_done_optimized(struct net_device *dev, int limit)
{
	struct fe_priv *np = netdev_priv(dev);
	u32 flags;
	struct ring_desc_ex* orig_get_tx = np->get_tx.ex;

	while ((np->get_tx.ex != np->put_tx.ex) &&
	       !((flags = le32_to_cpu(np->get_tx.ex->flaglen)) & NV_TX_VALID) &&
	       (limit-- > 0)) {

		dprintk(KERN_DEBUG "%s: nv_tx_done_optimized: flags 0x%x.\n",
					dev->name, flags);

		pci_unmap_page(np->pci_dev, np->get_tx_ctx->dma,
			       np->get_tx_ctx->dma_len,
			       PCI_DMA_TODEVICE);
		np->get_tx_ctx->dma = 0;

		if (flags & NV_TX2_LASTPACKET) {
			if (!(flags & NV_TX2_ERROR))
				np->stats.tx_packets++;
			dev_kfree_skb_any(np->get_tx_ctx->skb);
			np->get_tx_ctx->skb = NULL;
		}
		if (unlikely(np->get_tx.ex++ == np->last_tx.ex))
			np->get_tx.ex = np->first_tx.ex;
		if (unlikely(np->get_tx_ctx++ == np->last_tx_ctx))
			np->get_tx_ctx = np->first_tx_ctx;
	}
	if (unlikely((np->tx_stop == 1) && (np->get_tx.ex != orig_get_tx))) {
		np->tx_stop = 0;
		netif_wake_queue(dev);
	}
}

/*
 * nv_tx_timeout: dev->tx_timeout function
 * Called with netif_tx_lock held.
 */
static void nv_tx_timeout(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u32 status;

	if (np->msi_flags & NV_MSI_X_ENABLED)
		status = readl(base + NvRegMSIXIrqStatus) & NVREG_IRQSTAT_MASK;
	else
		status = readl(base + NvRegIrqStatus) & NVREG_IRQSTAT_MASK;

	printk(KERN_INFO "%s: Got tx_timeout. irq: %08x\n", dev->name, status);

	{
		int i;

		printk(KERN_INFO "%s: Ring at %lx\n",
		       dev->name, (unsigned long)np->ring_addr);
		printk(KERN_INFO "%s: Dumping tx registers\n", dev->name);
		for (i=0;i<=np->register_size;i+= 32) {
			printk(KERN_INFO "%3x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
					i,
					readl(base + i + 0), readl(base + i + 4),
					readl(base + i + 8), readl(base + i + 12),
					readl(base + i + 16), readl(base + i + 20),
					readl(base + i + 24), readl(base + i + 28));
		}
		printk(KERN_INFO "%s: Dumping tx ring\n", dev->name);
		for (i=0;i<np->tx_ring_size;i+= 4) {
			if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2) {
				printk(KERN_INFO "%03x: %08x %08x // %08x %08x // %08x %08x // %08x %08x\n",
				       i,
				       le32_to_cpu(np->tx_ring.orig[i].buf),
				       le32_to_cpu(np->tx_ring.orig[i].flaglen),
				       le32_to_cpu(np->tx_ring.orig[i+1].buf),
				       le32_to_cpu(np->tx_ring.orig[i+1].flaglen),
				       le32_to_cpu(np->tx_ring.orig[i+2].buf),
				       le32_to_cpu(np->tx_ring.orig[i+2].flaglen),
				       le32_to_cpu(np->tx_ring.orig[i+3].buf),
				       le32_to_cpu(np->tx_ring.orig[i+3].flaglen));
			} else {
				printk(KERN_INFO "%03x: %08x %08x %08x // %08x %08x %08x // %08x %08x %08x // %08x %08x %08x\n",
				       i,
				       le32_to_cpu(np->tx_ring.ex[i].bufhigh),
				       le32_to_cpu(np->tx_ring.ex[i].buflow),
				       le32_to_cpu(np->tx_ring.ex[i].flaglen),
				       le32_to_cpu(np->tx_ring.ex[i+1].bufhigh),
				       le32_to_cpu(np->tx_ring.ex[i+1].buflow),
				       le32_to_cpu(np->tx_ring.ex[i+1].flaglen),
				       le32_to_cpu(np->tx_ring.ex[i+2].bufhigh),
				       le32_to_cpu(np->tx_ring.ex[i+2].buflow),
				       le32_to_cpu(np->tx_ring.ex[i+2].flaglen),
				       le32_to_cpu(np->tx_ring.ex[i+3].bufhigh),
				       le32_to_cpu(np->tx_ring.ex[i+3].buflow),
				       le32_to_cpu(np->tx_ring.ex[i+3].flaglen));
			}
		}
	}

	spin_lock_irq(&np->lock);

	/* 1) stop tx engine */
	nv_stop_tx(dev);

	/* 2) check that the packets were not sent already: */
	if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2)
		nv_tx_done(dev);
	else
		nv_tx_done_optimized(dev, np->tx_ring_size);

	/* 3) if there are dead entries: clear everything */
	if (np->get_tx_ctx != np->put_tx_ctx) {
		printk(KERN_DEBUG "%s: tx_timeout: dead entries!\n", dev->name);
		nv_drain_tx(dev);
		nv_init_tx(dev);
		setup_hw_rings(dev, NV_SETUP_TX_RING);
	}

	netif_wake_queue(dev);

	/* 4) restart tx engine */
	nv_start_tx(dev);
	spin_unlock_irq(&np->lock);
}

/*
 * Called when the nic notices a mismatch between the actual data len on the
 * wire and the len indicated in the 802 header
 */
static int nv_getlen(struct net_device *dev, void *packet, int datalen)
{
	int hdrlen;	/* length of the 802 header */
	int protolen;	/* length as stored in the proto field */

	/* 1) calculate len according to header */
	if ( ((struct vlan_ethhdr *)packet)->h_vlan_proto == htons(ETH_P_8021Q)) {
		protolen = ntohs( ((struct vlan_ethhdr *)packet)->h_vlan_encapsulated_proto );
		hdrlen = VLAN_HLEN;
	} else {
		protolen = ntohs( ((struct ethhdr *)packet)->h_proto);
		hdrlen = ETH_HLEN;
	}
	dprintk(KERN_DEBUG "%s: nv_getlen: datalen %d, protolen %d, hdrlen %d\n",
				dev->name, datalen, protolen, hdrlen);
	if (protolen > ETH_DATA_LEN)
		return datalen; /* Value in proto field not a len, no checks possible */

	protolen += hdrlen;
	/* consistency checks: */
	if (datalen > ETH_ZLEN) {
		if (datalen >= protolen) {
			/* more data on wire than in 802 header, trim of
			 * additional data.
			 */
			dprintk(KERN_DEBUG "%s: nv_getlen: accepting %d bytes.\n",
					dev->name, protolen);
			return protolen;
		} else {
			/* less data on wire than mentioned in header.
			 * Discard the packet.
			 */
			dprintk(KERN_DEBUG "%s: nv_getlen: discarding long packet.\n",
					dev->name);
			return -1;
		}
	} else {
		/* short packet. Accept only if 802 values are also short */
		if (protolen > ETH_ZLEN) {
			dprintk(KERN_DEBUG "%s: nv_getlen: discarding short packet.\n",
					dev->name);
			return -1;
		}
		dprintk(KERN_DEBUG "%s: nv_getlen: accepting %d bytes.\n",
				dev->name, datalen);
		return datalen;
	}
}

static int nv_rx_process(struct net_device *dev, int limit)
{
	struct fe_priv *np = netdev_priv(dev);
	u32 flags;
	u32 rx_processed_cnt = 0;
	struct sk_buff *skb;
	int len;

	while((np->get_rx.orig != np->put_rx.orig) &&
	      !((flags = le32_to_cpu(np->get_rx.orig->flaglen)) & NV_RX_AVAIL) &&
		(rx_processed_cnt++ < limit)) {

		dprintk(KERN_DEBUG "%s: nv_rx_process: flags 0x%x.\n",
					dev->name, flags);

		/*
		 * the packet is for us - immediately tear down the pci mapping.
		 * TODO: check if a prefetch of the first cacheline improves
		 * the performance.
		 */
		pci_unmap_single(np->pci_dev, np->get_rx_ctx->dma,
				np->get_rx_ctx->dma_len,
				PCI_DMA_FROMDEVICE);
		skb = np->get_rx_ctx->skb;
		np->get_rx_ctx->skb = NULL;

		{
			int j;
			dprintk(KERN_DEBUG "Dumping packet (flags 0x%x).",flags);
			for (j=0; j<64; j++) {
				if ((j%16) == 0)
					dprintk("\n%03x:", j);
				dprintk(" %02x", ((unsigned char*)skb->data)[j]);
			}
			dprintk("\n");
		}
		/* look at what we actually got: */
		if (np->desc_ver == DESC_VER_1) {
			if (likely(flags & NV_RX_DESCRIPTORVALID)) {
				len = flags & LEN_MASK_V1;
				if (unlikely(flags & NV_RX_ERROR)) {
					if (flags & NV_RX_ERROR4) {
						len = nv_getlen(dev, skb->data, len);
						if (len < 0) {
							np->stats.rx_errors++;
							dev_kfree_skb(skb);
							goto next_pkt;
						}
					}
					/* framing errors are soft errors */
					else if (flags & NV_RX_FRAMINGERR) {
						if (flags & NV_RX_SUBSTRACT1) {
							len--;
						}
					}
					/* the rest are hard errors */
					else {
						if (flags & NV_RX_MISSEDFRAME)
							np->stats.rx_missed_errors++;
						if (flags & NV_RX_CRCERR)
							np->stats.rx_crc_errors++;
						if (flags & NV_RX_OVERFLOW)
							np->stats.rx_over_errors++;
						np->stats.rx_errors++;
						dev_kfree_skb(skb);
						goto next_pkt;
					}
				}
			} else {
				dev_kfree_skb(skb);
				goto next_pkt;
			}
		} else {
			if (likely(flags & NV_RX2_DESCRIPTORVALID)) {
				len = flags & LEN_MASK_V2;
				if (unlikely(flags & NV_RX2_ERROR)) {
					if (flags & NV_RX2_ERROR4) {
						len = nv_getlen(dev, skb->data, len);
						if (len < 0) {
							np->stats.rx_errors++;
							dev_kfree_skb(skb);
							goto next_pkt;
						}
					}
					/* framing errors are soft errors */
					else if (flags & NV_RX2_FRAMINGERR) {
						if (flags & NV_RX2_SUBSTRACT1) {
							len--;
						}
					}
					/* the rest are hard errors */
					else {
						if (flags & NV_RX2_CRCERR)
							np->stats.rx_crc_errors++;
						if (flags & NV_RX2_OVERFLOW)
							np->stats.rx_over_errors++;
						np->stats.rx_errors++;
						dev_kfree_skb(skb);
						goto next_pkt;
					}
				}
				if ((flags & NV_RX2_CHECKSUMMASK) == NV_RX2_CHECKSUMOK2)/*ip and tcp */ {
					skb->ip_summed = CHECKSUM_UNNECESSARY;
				} else {
					if ((flags & NV_RX2_CHECKSUMMASK) == NV_RX2_CHECKSUMOK1 ||
					    (flags & NV_RX2_CHECKSUMMASK) == NV_RX2_CHECKSUMOK3) {
						skb->ip_summed = CHECKSUM_UNNECESSARY;
					}
				}
			} else {
				dev_kfree_skb(skb);
				goto next_pkt;
			}
		}
		/* got a valid packet - forward it to the network core */
		skb_put(skb, len);
		skb->protocol = eth_type_trans(skb, dev);
		dprintk(KERN_DEBUG "%s: nv_rx_process: %d bytes, proto %d accepted.\n",
					dev->name, len, skb->protocol);
#ifdef CONFIG_FORCEDETH_NAPI
		netif_receive_skb(skb);
#else
		netif_rx(skb);
#endif
		dev->last_rx = jiffies;
		np->stats.rx_packets++;
		np->stats.rx_bytes += len;
next_pkt:
		if (unlikely(np->get_rx.orig++ == np->last_rx.orig))
			np->get_rx.orig = np->first_rx.orig;
		if (unlikely(np->get_rx_ctx++ == np->last_rx_ctx))
			np->get_rx_ctx = np->first_rx_ctx;
	}

	return rx_processed_cnt;
}

static int nv_rx_process_optimized(struct net_device *dev, int limit)
{
	struct fe_priv *np = netdev_priv(dev);
	u32 flags;
	u32 vlanflags = 0;
	u32 rx_processed_cnt = 0;
	struct sk_buff *skb;
	int len;

	while((np->get_rx.ex != np->put_rx.ex) &&
	      !((flags = le32_to_cpu(np->get_rx.ex->flaglen)) & NV_RX2_AVAIL) &&
	      (rx_processed_cnt++ < limit)) {

		dprintk(KERN_DEBUG "%s: nv_rx_process_optimized: flags 0x%x.\n",
					dev->name, flags);

		/*
		 * the packet is for us - immediately tear down the pci mapping.
		 * TODO: check if a prefetch of the first cacheline improves
		 * the performance.
		 */
		pci_unmap_single(np->pci_dev, np->get_rx_ctx->dma,
				np->get_rx_ctx->dma_len,
				PCI_DMA_FROMDEVICE);
		skb = np->get_rx_ctx->skb;
		np->get_rx_ctx->skb = NULL;

		{
			int j;
			dprintk(KERN_DEBUG "Dumping packet (flags 0x%x).",flags);
			for (j=0; j<64; j++) {
				if ((j%16) == 0)
					dprintk("\n%03x:", j);
				dprintk(" %02x", ((unsigned char*)skb->data)[j]);
			}
			dprintk("\n");
		}
		/* look at what we actually got: */
		if (likely(flags & NV_RX2_DESCRIPTORVALID)) {
			len = flags & LEN_MASK_V2;
			if (unlikely(flags & NV_RX2_ERROR)) {
				if (flags & NV_RX2_ERROR4) {
					len = nv_getlen(dev, skb->data, len);
					if (len < 0) {
						dev_kfree_skb(skb);
						goto next_pkt;
					}
				}
				/* framing errors are soft errors */
				else if (flags & NV_RX2_FRAMINGERR) {
					if (flags & NV_RX2_SUBSTRACT1) {
						len--;
					}
				}
				/* the rest are hard errors */
				else {
					dev_kfree_skb(skb);
					goto next_pkt;
				}
			}

			if ((flags & NV_RX2_CHECKSUMMASK) == NV_RX2_CHECKSUMOK2)/*ip and tcp */ {
				skb->ip_summed = CHECKSUM_UNNECESSARY;
			} else {
				if ((flags & NV_RX2_CHECKSUMMASK) == NV_RX2_CHECKSUMOK1 ||
				    (flags & NV_RX2_CHECKSUMMASK) == NV_RX2_CHECKSUMOK3) {
					skb->ip_summed = CHECKSUM_UNNECESSARY;
				}
			}

			/* got a valid packet - forward it to the network core */
			skb_put(skb, len);
			skb->protocol = eth_type_trans(skb, dev);
			prefetch(skb->data);

			dprintk(KERN_DEBUG "%s: nv_rx_process_optimized: %d bytes, proto %d accepted.\n",
				dev->name, len, skb->protocol);

			if (likely(!np->vlangrp)) {
#ifdef CONFIG_FORCEDETH_NAPI
				netif_receive_skb(skb);
#else
				netif_rx(skb);
#endif
			} else {
				vlanflags = le32_to_cpu(np->get_rx.ex->buflow);
				if (vlanflags & NV_RX3_VLAN_TAG_PRESENT) {
#ifdef CONFIG_FORCEDETH_NAPI
					vlan_hwaccel_receive_skb(skb, np->vlangrp,
								 vlanflags & NV_RX3_VLAN_TAG_MASK);
#else
					vlan_hwaccel_rx(skb, np->vlangrp,
							vlanflags & NV_RX3_VLAN_TAG_MASK);
#endif
				} else {
#ifdef CONFIG_FORCEDETH_NAPI
					netif_receive_skb(skb);
#else
					netif_rx(skb);
#endif
				}
			}

			dev->last_rx = jiffies;
			np->stats.rx_packets++;
			np->stats.rx_bytes += len;
		} else {
			dev_kfree_skb(skb);
		}
next_pkt:
		if (unlikely(np->get_rx.ex++ == np->last_rx.ex))
			np->get_rx.ex = np->first_rx.ex;
		if (unlikely(np->get_rx_ctx++ == np->last_rx_ctx))
			np->get_rx_ctx = np->first_rx_ctx;
	}

	return rx_processed_cnt;
}

static void set_bufsize(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);

	if (dev->mtu <= ETH_DATA_LEN)
		np->rx_buf_sz = ETH_DATA_LEN + NV_RX_HEADERS;
	else
		np->rx_buf_sz = dev->mtu + NV_RX_HEADERS;
}

/*
 * nv_change_mtu: dev->change_mtu function
 * Called with dev_base_lock held for read.
 */
static int nv_change_mtu(struct net_device *dev, int new_mtu)
{
	struct fe_priv *np = netdev_priv(dev);
	int old_mtu;

	if (new_mtu < 64 || new_mtu > np->pkt_limit)
		return -EINVAL;

	old_mtu = dev->mtu;
	dev->mtu = new_mtu;

	/* return early if the buffer sizes will not change */
	if (old_mtu <= ETH_DATA_LEN && new_mtu <= ETH_DATA_LEN)
		return 0;
	if (old_mtu == new_mtu)
		return 0;

	/* synchronized against open : rtnl_lock() held by caller */
	if (netif_running(dev)) {
		u8 __iomem *base = get_hwbase(dev);
		/*
		 * It seems that the nic preloads valid ring entries into an
		 * internal buffer. The procedure for flushing everything is
		 * guessed, there is probably a simpler approach.
		 * Changing the MTU is a rare event, it shouldn't matter.
		 */
		nv_disable_irq(dev);
		netif_tx_lock_bh(dev);
		spin_lock(&np->lock);
		/* stop engines */
		nv_stop_rx(dev);
		nv_stop_tx(dev);
		nv_txrx_reset(dev);
		/* drain rx queue */
		nv_drain_rx(dev);
		nv_drain_tx(dev);
		/* reinit driver view of the rx queue */
		set_bufsize(dev);
		if (nv_init_ring(dev)) {
			if (!np->in_shutdown)
				mod_timer(&np->oom_kick, jiffies + OOM_REFILL);
		}
		/* reinit nic view of the rx queue */
		writel(np->rx_buf_sz, base + NvRegOffloadConfig);
		setup_hw_rings(dev, NV_SETUP_RX_RING | NV_SETUP_TX_RING);
		writel( ((np->rx_ring_size-1) << NVREG_RINGSZ_RXSHIFT) + ((np->tx_ring_size-1) << NVREG_RINGSZ_TXSHIFT),
			base + NvRegRingSizes);
		pci_push(base);
		writel(NVREG_TXRXCTL_KICK|np->txrxctl_bits, get_hwbase(dev) + NvRegTxRxControl);
		pci_push(base);

		/* restart rx engine */
		nv_start_rx(dev);
		nv_start_tx(dev);
		spin_unlock(&np->lock);
		netif_tx_unlock_bh(dev);
		nv_enable_irq(dev);
	}
	return 0;
}

static void nv_copy_mac_to_hw(struct net_device *dev)
{
	u8 __iomem *base = get_hwbase(dev);
	u32 mac[2];

	mac[0] = (dev->dev_addr[0] << 0) + (dev->dev_addr[1] << 8) +
			(dev->dev_addr[2] << 16) + (dev->dev_addr[3] << 24);
	mac[1] = (dev->dev_addr[4] << 0) + (dev->dev_addr[5] << 8);

	writel(mac[0], base + NvRegMacAddrA);
	writel(mac[1], base + NvRegMacAddrB);
}

/*
 * nv_set_mac_address: dev->set_mac_address function
 * Called with rtnl_lock() held.
 */
static int nv_set_mac_address(struct net_device *dev, void *addr)
{
	struct fe_priv *np = netdev_priv(dev);
	struct sockaddr *macaddr = (struct sockaddr*)addr;

	if (!is_valid_ether_addr(macaddr->sa_data))
		return -EADDRNOTAVAIL;

	/* synchronized against open : rtnl_lock() held by caller */
	memcpy(dev->dev_addr, macaddr->sa_data, ETH_ALEN);

	if (netif_running(dev)) {
		netif_tx_lock_bh(dev);
		spin_lock_irq(&np->lock);

		/* stop rx engine */
		nv_stop_rx(dev);

		/* set mac address */
		nv_copy_mac_to_hw(dev);

		/* restart rx engine */
		nv_start_rx(dev);
		spin_unlock_irq(&np->lock);
		netif_tx_unlock_bh(dev);
	} else {
		nv_copy_mac_to_hw(dev);
	}
	return 0;
}

/*
 * nv_set_multicast: dev->set_multicast function
 * Called with netif_tx_lock held.
 */
static void nv_set_multicast(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u32 addr[2];
	u32 mask[2];
	u32 pff = readl(base + NvRegPacketFilterFlags) & NVREG_PFF_PAUSE_RX;

	memset(addr, 0, sizeof(addr));
	memset(mask, 0, sizeof(mask));

	if (dev->flags & IFF_PROMISC) {
		pff |= NVREG_PFF_PROMISC;
	} else {
		pff |= NVREG_PFF_MYADDR;

		if (dev->flags & IFF_ALLMULTI || dev->mc_list) {
			u32 alwaysOff[2];
			u32 alwaysOn[2];

			alwaysOn[0] = alwaysOn[1] = alwaysOff[0] = alwaysOff[1] = 0xffffffff;
			if (dev->flags & IFF_ALLMULTI) {
				alwaysOn[0] = alwaysOn[1] = alwaysOff[0] = alwaysOff[1] = 0;
			} else {
				struct dev_mc_list *walk;

				walk = dev->mc_list;
				while (walk != NULL) {
					u32 a, b;
					a = le32_to_cpu(*(u32 *) walk->dmi_addr);
					b = le16_to_cpu(*(u16 *) (&walk->dmi_addr[4]));
					alwaysOn[0] &= a;
					alwaysOff[0] &= ~a;
					alwaysOn[1] &= b;
					alwaysOff[1] &= ~b;
					walk = walk->next;
				}
			}
			addr[0] = alwaysOn[0];
			addr[1] = alwaysOn[1];
			mask[0] = alwaysOn[0] | alwaysOff[0];
			mask[1] = alwaysOn[1] | alwaysOff[1];
		}
	}
	addr[0] |= NVREG_MCASTADDRA_FORCE;
	pff |= NVREG_PFF_ALWAYS;
	spin_lock_irq(&np->lock);
	nv_stop_rx(dev);
	writel(addr[0], base + NvRegMulticastAddrA);
	writel(addr[1], base + NvRegMulticastAddrB);
	writel(mask[0], base + NvRegMulticastMaskA);
	writel(mask[1], base + NvRegMulticastMaskB);
	writel(pff, base + NvRegPacketFilterFlags);
	dprintk(KERN_INFO "%s: reconfiguration for multicast lists.\n",
		dev->name);
	nv_start_rx(dev);
	spin_unlock_irq(&np->lock);
}

static void nv_update_pause(struct net_device *dev, u32 pause_flags)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);

	np->pause_flags &= ~(NV_PAUSEFRAME_TX_ENABLE | NV_PAUSEFRAME_RX_ENABLE);

	if (np->pause_flags & NV_PAUSEFRAME_RX_CAPABLE) {
		u32 pff = readl(base + NvRegPacketFilterFlags) & ~NVREG_PFF_PAUSE_RX;
		if (pause_flags & NV_PAUSEFRAME_RX_ENABLE) {
			writel(pff|NVREG_PFF_PAUSE_RX, base + NvRegPacketFilterFlags);
			np->pause_flags |= NV_PAUSEFRAME_RX_ENABLE;
		} else {
			writel(pff, base + NvRegPacketFilterFlags);
		}
	}
	if (np->pause_flags & NV_PAUSEFRAME_TX_CAPABLE) {
		u32 regmisc = readl(base + NvRegMisc1) & ~NVREG_MISC1_PAUSE_TX;
		if (pause_flags & NV_PAUSEFRAME_TX_ENABLE) {
			writel(NVREG_TX_PAUSEFRAME_ENABLE,  base + NvRegTxPauseFrame);
			writel(regmisc|NVREG_MISC1_PAUSE_TX, base + NvRegMisc1);
			np->pause_flags |= NV_PAUSEFRAME_TX_ENABLE;
		} else {
			writel(NVREG_TX_PAUSEFRAME_DISABLE,  base + NvRegTxPauseFrame);
			writel(regmisc, base + NvRegMisc1);
		}
	}
}

/**
 * nv_update_linkspeed: Setup the MAC according to the link partner
 * @dev: Network device to be configured
 *
 * The function queries the PHY and checks if there is a link partner.
 * If yes, then it sets up the MAC accordingly. Otherwise, the MAC is
 * set to 10 MBit HD.
 *
 * The function returns 0 if there is no link partner and 1 if there is
 * a good link partner.
 */
static int nv_update_linkspeed(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	int adv = 0;
	int lpa = 0;
	int adv_lpa, adv_pause, lpa_pause;
	int newls = np->linkspeed;
	int newdup = np->duplex;
	int mii_status;
	int retval = 0;
	u32 control_1000, status_1000, phyreg, pause_flags, txreg;

	/* BMSR_LSTATUS is latched, read it twice:
	 * we want the current value.
	 */
	mii_rw(dev, np->phyaddr, MII_BMSR, MII_READ);
	mii_status = mii_rw(dev, np->phyaddr, MII_BMSR, MII_READ);

	if (!(mii_status & BMSR_LSTATUS)) {
		dprintk(KERN_DEBUG "%s: no link detected by phy - falling back to 10HD.\n",
				dev->name);
		newls = NVREG_LINKSPEED_FORCE|NVREG_LINKSPEED_10;
		newdup = 0;
		retval = 0;
		goto set_speed;
	}

	if (np->autoneg == 0) {
		dprintk(KERN_DEBUG "%s: nv_update_linkspeed: autoneg off, PHY set to 0x%04x.\n",
				dev->name, np->fixed_mode);
		if (np->fixed_mode & LPA_100FULL) {
			newls = NVREG_LINKSPEED_FORCE|NVREG_LINKSPEED_100;
			newdup = 1;
		} else if (np->fixed_mode & LPA_100HALF) {
			newls = NVREG_LINKSPEED_FORCE|NVREG_LINKSPEED_100;
			newdup = 0;
		} else if (np->fixed_mode & LPA_10FULL) {
			newls = NVREG_LINKSPEED_FORCE|NVREG_LINKSPEED_10;
			newdup = 1;
		} else {
			newls = NVREG_LINKSPEED_FORCE|NVREG_LINKSPEED_10;
			newdup = 0;
		}
		retval = 1;
		goto set_speed;
	}
	/* check auto negotiation is complete */
	if (!(mii_status & BMSR_ANEGCOMPLETE)) {
		/* still in autonegotiation - configure nic for 10 MBit HD and wait. */
		newls = NVREG_LINKSPEED_FORCE|NVREG_LINKSPEED_10;
		newdup = 0;
		retval = 0;
		dprintk(KERN_DEBUG "%s: autoneg not completed - falling back to 10HD.\n", dev->name);
		goto set_speed;
	}

	adv = mii_rw(dev, np->phyaddr, MII_ADVERTISE, MII_READ);
	lpa = mii_rw(dev, np->phyaddr, MII_LPA, MII_READ);
	dprintk(KERN_DEBUG "%s: nv_update_linkspeed: PHY advertises 0x%04x, lpa 0x%04x.\n",
				dev->name, adv, lpa);

	retval = 1;
	if (np->gigabit == PHY_GIGABIT) {
		control_1000 = mii_rw(dev, np->phyaddr, MII_CTRL1000, MII_READ);
		status_1000 = mii_rw(dev, np->phyaddr, MII_STAT1000, MII_READ);

		if ((control_1000 & ADVERTISE_1000FULL) &&
			(status_1000 & LPA_1000FULL)) {
			dprintk(KERN_DEBUG "%s: nv_update_linkspeed: GBit ethernet detected.\n",
				dev->name);
			newls = NVREG_LINKSPEED_FORCE|NVREG_LINKSPEED_1000;
			newdup = 1;
			goto set_speed;
		}
	}

	/* FIXME: handle parallel detection properly */
	adv_lpa = lpa & adv;
	if (adv_lpa & LPA_100FULL) {
		newls = NVREG_LINKSPEED_FORCE|NVREG_LINKSPEED_100;
		newdup = 1;
	} else if (adv_lpa & LPA_100HALF) {
		newls = NVREG_LINKSPEED_FORCE|NVREG_LINKSPEED_100;
		newdup = 0;
	} else if (adv_lpa & LPA_10FULL) {
		newls = NVREG_LINKSPEED_FORCE|NVREG_LINKSPEED_10;
		newdup = 1;
	} else if (adv_lpa & LPA_10HALF) {
		newls = NVREG_LINKSPEED_FORCE|NVREG_LINKSPEED_10;
		newdup = 0;
	} else {
		dprintk(KERN_DEBUG "%s: bad ability %04x - falling back to 10HD.\n", dev->name, adv_lpa);
		newls = NVREG_LINKSPEED_FORCE|NVREG_LINKSPEED_10;
		newdup = 0;
	}

set_speed:
	if (np->duplex == newdup && np->linkspeed == newls)
		return retval;

	dprintk(KERN_INFO "%s: changing link setting from %d/%d to %d/%d.\n",
			dev->name, np->linkspeed, np->duplex, newls, newdup);

	np->duplex = newdup;
	np->linkspeed = newls;

	if (np->gigabit == PHY_GIGABIT) {
		phyreg = readl(base + NvRegRandomSeed);
		phyreg &= ~(0x3FF00);
		if ((np->linkspeed & 0xFFF) == NVREG_LINKSPEED_10)
			phyreg |= NVREG_RNDSEED_FORCE3;
		else if ((np->linkspeed & 0xFFF) == NVREG_LINKSPEED_100)
			phyreg |= NVREG_RNDSEED_FORCE2;
		else if ((np->linkspeed & 0xFFF) == NVREG_LINKSPEED_1000)
			phyreg |= NVREG_RNDSEED_FORCE;
		writel(phyreg, base + NvRegRandomSeed);
	}

	phyreg = readl(base + NvRegPhyInterface);
	phyreg &= ~(PHY_HALF|PHY_100|PHY_1000);
	if (np->duplex == 0)
		phyreg |= PHY_HALF;
	if ((np->linkspeed & NVREG_LINKSPEED_MASK) == NVREG_LINKSPEED_100)
		phyreg |= PHY_100;
	else if ((np->linkspeed & NVREG_LINKSPEED_MASK) == NVREG_LINKSPEED_1000)
		phyreg |= PHY_1000;
	writel(phyreg, base + NvRegPhyInterface);

	if (phyreg & PHY_RGMII) {
		if ((np->linkspeed & NVREG_LINKSPEED_MASK) == NVREG_LINKSPEED_1000)
			txreg = NVREG_TX_DEFERRAL_RGMII_1000;
		else
			txreg = NVREG_TX_DEFERRAL_RGMII_10_100;
	} else {
		txreg = NVREG_TX_DEFERRAL_DEFAULT;
	}
	writel(txreg, base + NvRegTxDeferral);

	if (np->desc_ver == DESC_VER_1) {
		txreg = NVREG_TX_WM_DESC1_DEFAULT;
	} else {
		if ((np->linkspeed & NVREG_LINKSPEED_MASK) == NVREG_LINKSPEED_1000)
			txreg = NVREG_TX_WM_DESC2_3_1000;
		else
			txreg = NVREG_TX_WM_DESC2_3_DEFAULT;
	}
	writel(txreg, base + NvRegTxWatermark);

	writel(NVREG_MISC1_FORCE | ( np->duplex ? 0 : NVREG_MISC1_HD),
		base + NvRegMisc1);
	pci_push(base);
	writel(np->linkspeed, base + NvRegLinkSpeed);
	pci_push(base);

	pause_flags = 0;
	/* setup pause frame */
	if (np->duplex != 0) {
		if (np->autoneg && np->pause_flags & NV_PAUSEFRAME_AUTONEG) {
			adv_pause = adv & (ADVERTISE_PAUSE_CAP| ADVERTISE_PAUSE_ASYM);
			lpa_pause = lpa & (LPA_PAUSE_CAP| LPA_PAUSE_ASYM);

			switch (adv_pause) {
			case ADVERTISE_PAUSE_CAP:
				if (lpa_pause & LPA_PAUSE_CAP) {
					pause_flags |= NV_PAUSEFRAME_RX_ENABLE;
					if (np->pause_flags & NV_PAUSEFRAME_TX_REQ)
						pause_flags |= NV_PAUSEFRAME_TX_ENABLE;
				}
				break;
			case ADVERTISE_PAUSE_ASYM:
				if (lpa_pause == (LPA_PAUSE_CAP| LPA_PAUSE_ASYM))
				{
					pause_flags |= NV_PAUSEFRAME_TX_ENABLE;
				}
				break;
			case ADVERTISE_PAUSE_CAP| ADVERTISE_PAUSE_ASYM:
				if (lpa_pause & LPA_PAUSE_CAP)
				{
					pause_flags |=  NV_PAUSEFRAME_RX_ENABLE;
					if (np->pause_flags & NV_PAUSEFRAME_TX_REQ)
						pause_flags |= NV_PAUSEFRAME_TX_ENABLE;
				}
				if (lpa_pause == LPA_PAUSE_ASYM)
				{
					pause_flags |= NV_PAUSEFRAME_RX_ENABLE;
				}
				break;
			}
		} else {
			pause_flags = np->pause_flags;
		}
	}
	nv_update_pause(dev, pause_flags);

	return retval;
}

static void nv_linkchange(struct net_device *dev)
{
	if (nv_update_linkspeed(dev)) {
		if (!netif_carrier_ok(dev)) {
			netif_carrier_on(dev);
			printk(KERN_INFO "%s: link up.\n", dev->name);
			nv_start_rx(dev);
		}
	} else {
		if (netif_carrier_ok(dev)) {
			netif_carrier_off(dev);
			printk(KERN_INFO "%s: link down.\n", dev->name);
			nv_stop_rx(dev);
		}
	}
}

static void nv_link_irq(struct net_device *dev)
{
	u8 __iomem *base = get_hwbase(dev);
	u32 miistat;

	miistat = readl(base + NvRegMIIStatus);
	writel(NVREG_MIISTAT_MASK, base + NvRegMIIStatus);
	dprintk(KERN_INFO "%s: link change irq, status 0x%x.\n", dev->name, miistat);

	if (miistat & (NVREG_MIISTAT_LINKCHANGE))
		nv_linkchange(dev);
	dprintk(KERN_DEBUG "%s: link change notification done.\n", dev->name);
}

static irqreturn_t nv_nic_irq(int foo, void *data)
{
	struct net_device *dev = (struct net_device *) data;
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u32 events;
	int i;

	dprintk(KERN_DEBUG "%s: nv_nic_irq\n", dev->name);

	for (i=0; ; i++) {
		if (!(np->msi_flags & NV_MSI_X_ENABLED)) {
			events = readl(base + NvRegIrqStatus) & NVREG_IRQSTAT_MASK;
			writel(NVREG_IRQSTAT_MASK, base + NvRegIrqStatus);
		} else {
			events = readl(base + NvRegMSIXIrqStatus) & NVREG_IRQSTAT_MASK;
			writel(NVREG_IRQSTAT_MASK, base + NvRegMSIXIrqStatus);
		}
		dprintk(KERN_DEBUG "%s: irq: %08x\n", dev->name, events);
		if (!(events & np->irqmask))
			break;

		spin_lock(&np->lock);
		nv_tx_done(dev);
		spin_unlock(&np->lock);

#ifdef CONFIG_FORCEDETH_NAPI
		if (events & NVREG_IRQ_RX_ALL) {
			netif_rx_schedule(dev);

			/* Disable furthur receive irq's */
			spin_lock(&np->lock);
			np->irqmask &= ~NVREG_IRQ_RX_ALL;

			if (np->msi_flags & NV_MSI_X_ENABLED)
				writel(NVREG_IRQ_RX_ALL, base + NvRegIrqMask);
			else
				writel(np->irqmask, base + NvRegIrqMask);
			spin_unlock(&np->lock);
		}
#else
		if (nv_rx_process(dev, dev->weight)) {
			if (unlikely(nv_alloc_rx(dev))) {
				spin_lock(&np->lock);
				if (!np->in_shutdown)
					mod_timer(&np->oom_kick, jiffies + OOM_REFILL);
				spin_unlock(&np->lock);
			}
		}
#endif
		if (unlikely(events & NVREG_IRQ_LINK)) {
			spin_lock(&np->lock);
			nv_link_irq(dev);
			spin_unlock(&np->lock);
		}
		if (unlikely(np->need_linktimer && time_after(jiffies, np->link_timeout))) {
			spin_lock(&np->lock);
			nv_linkchange(dev);
			spin_unlock(&np->lock);
			np->link_timeout = jiffies + LINK_TIMEOUT;
		}
		if (unlikely(events & (NVREG_IRQ_TX_ERR))) {
			dprintk(KERN_DEBUG "%s: received irq with events 0x%x. Probably TX fail.\n",
						dev->name, events);
		}
		if (unlikely(events & (NVREG_IRQ_UNKNOWN))) {
			printk(KERN_DEBUG "%s: received irq with unknown events 0x%x. Please report\n",
						dev->name, events);
		}
		if (unlikely(events & NVREG_IRQ_RECOVER_ERROR)) {
			spin_lock(&np->lock);
			/* disable interrupts on the nic */
			if (!(np->msi_flags & NV_MSI_X_ENABLED))
				writel(0, base + NvRegIrqMask);
			else
				writel(np->irqmask, base + NvRegIrqMask);
			pci_push(base);

			if (!np->in_shutdown) {
				np->nic_poll_irq = np->irqmask;
				np->recover_error = 1;
				mod_timer(&np->nic_poll, jiffies + POLL_WAIT);
			}
			spin_unlock(&np->lock);
			break;
		}
		if (unlikely(i > max_interrupt_work)) {
			spin_lock(&np->lock);
			/* disable interrupts on the nic */
			if (!(np->msi_flags & NV_MSI_X_ENABLED))
				writel(0, base + NvRegIrqMask);
			else
				writel(np->irqmask, base + NvRegIrqMask);
			pci_push(base);

			if (!np->in_shutdown) {
				np->nic_poll_irq = np->irqmask;
				mod_timer(&np->nic_poll, jiffies + POLL_WAIT);
			}
			spin_unlock(&np->lock);
			printk(KERN_DEBUG "%s: too many iterations (%d) in nv_nic_irq.\n", dev->name, i);
			break;
		}

	}
	dprintk(KERN_DEBUG "%s: nv_nic_irq completed\n", dev->name);

	return IRQ_RETVAL(i);
}

#define TX_WORK_PER_LOOP  64
#define RX_WORK_PER_LOOP  64
/**
 * All _optimized functions are used to help increase performance
 * (reduce CPU and increase throughput). They use descripter version 3,
 * compiler directives, and reduce memory accesses.
 */
static irqreturn_t nv_nic_irq_optimized(int foo, void *data)
{
	struct net_device *dev = (struct net_device *) data;
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u32 events;
	int i;

	dprintk(KERN_DEBUG "%s: nv_nic_irq_optimized\n", dev->name);

	for (i=0; ; i++) {
		if (!(np->msi_flags & NV_MSI_X_ENABLED)) {
			events = readl(base + NvRegIrqStatus) & NVREG_IRQSTAT_MASK;
			writel(NVREG_IRQSTAT_MASK, base + NvRegIrqStatus);
		} else {
			events = readl(base + NvRegMSIXIrqStatus) & NVREG_IRQSTAT_MASK;
			writel(NVREG_IRQSTAT_MASK, base + NvRegMSIXIrqStatus);
		}
		dprintk(KERN_DEBUG "%s: irq: %08x\n", dev->name, events);
		if (!(events & np->irqmask))
			break;

		spin_lock(&np->lock);
		nv_tx_done_optimized(dev, TX_WORK_PER_LOOP);
		spin_unlock(&np->lock);

#ifdef CONFIG_FORCEDETH_NAPI
		if (events & NVREG_IRQ_RX_ALL) {
			netif_rx_schedule(dev);

			/* Disable furthur receive irq's */
			spin_lock(&np->lock);
			np->irqmask &= ~NVREG_IRQ_RX_ALL;

			if (np->msi_flags & NV_MSI_X_ENABLED)
				writel(NVREG_IRQ_RX_ALL, base + NvRegIrqMask);
			else
				writel(np->irqmask, base + NvRegIrqMask);
			spin_unlock(&np->lock);
		}
#else
		if (nv_rx_process_optimized(dev, dev->weight)) {
			if (unlikely(nv_alloc_rx_optimized(dev))) {
				spin_lock(&np->lock);
				if (!np->in_shutdown)
					mod_timer(&np->oom_kick, jiffies + OOM_REFILL);
				spin_unlock(&np->lock);
			}
		}
#endif
		if (unlikely(events & NVREG_IRQ_LINK)) {
			spin_lock(&np->lock);
			nv_link_irq(dev);
			spin_unlock(&np->lock);
		}
		if (unlikely(np->need_linktimer && time_after(jiffies, np->link_timeout))) {
			spin_lock(&np->lock);
			nv_linkchange(dev);
			spin_unlock(&np->lock);
			np->link_timeout = jiffies + LINK_TIMEOUT;
		}
		if (unlikely(events & (NVREG_IRQ_TX_ERR))) {
			dprintk(KERN_DEBUG "%s: received irq with events 0x%x. Probably TX fail.\n",
						dev->name, events);
		}
		if (unlikely(events & (NVREG_IRQ_UNKNOWN))) {
			printk(KERN_DEBUG "%s: received irq with unknown events 0x%x. Please report\n",
						dev->name, events);
		}
		if (unlikely(events & NVREG_IRQ_RECOVER_ERROR)) {
			spin_lock(&np->lock);
			/* disable interrupts on the nic */
			if (!(np->msi_flags & NV_MSI_X_ENABLED))
				writel(0, base + NvRegIrqMask);
			else
				writel(np->irqmask, base + NvRegIrqMask);
			pci_push(base);

			if (!np->in_shutdown) {
				np->nic_poll_irq = np->irqmask;
				np->recover_error = 1;
				mod_timer(&np->nic_poll, jiffies + POLL_WAIT);
			}
			spin_unlock(&np->lock);
			break;
		}

		if (unlikely(i > max_interrupt_work)) {
			spin_lock(&np->lock);
			/* disable interrupts on the nic */
			if (!(np->msi_flags & NV_MSI_X_ENABLED))
				writel(0, base + NvRegIrqMask);
			else
				writel(np->irqmask, base + NvRegIrqMask);
			pci_push(base);

			if (!np->in_shutdown) {
				np->nic_poll_irq = np->irqmask;
				mod_timer(&np->nic_poll, jiffies + POLL_WAIT);
			}
			spin_unlock(&np->lock);
			printk(KERN_DEBUG "%s: too many iterations (%d) in nv_nic_irq.\n", dev->name, i);
			break;
		}

	}
	dprintk(KERN_DEBUG "%s: nv_nic_irq_optimized completed\n", dev->name);

	return IRQ_RETVAL(i);
}

static irqreturn_t nv_nic_irq_tx(int foo, void *data)
{
	struct net_device *dev = (struct net_device *) data;
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u32 events;
	int i;
	unsigned long flags;

	dprintk(KERN_DEBUG "%s: nv_nic_irq_tx\n", dev->name);

	for (i=0; ; i++) {
		events = readl(base + NvRegMSIXIrqStatus) & NVREG_IRQ_TX_ALL;
		writel(NVREG_IRQ_TX_ALL, base + NvRegMSIXIrqStatus);
		dprintk(KERN_DEBUG "%s: tx irq: %08x\n", dev->name, events);
		if (!(events & np->irqmask))
			break;

		spin_lock_irqsave(&np->lock, flags);
		nv_tx_done_optimized(dev, TX_WORK_PER_LOOP);
		spin_unlock_irqrestore(&np->lock, flags);

		if (unlikely(events & (NVREG_IRQ_TX_ERR))) {
			dprintk(KERN_DEBUG "%s: received irq with events 0x%x. Probably TX fail.\n",
						dev->name, events);
		}
		if (unlikely(i > max_interrupt_work)) {
			spin_lock_irqsave(&np->lock, flags);
			/* disable interrupts on the nic */
			writel(NVREG_IRQ_TX_ALL, base + NvRegIrqMask);
			pci_push(base);

			if (!np->in_shutdown) {
				np->nic_poll_irq |= NVREG_IRQ_TX_ALL;
				mod_timer(&np->nic_poll, jiffies + POLL_WAIT);
			}
			spin_unlock_irqrestore(&np->lock, flags);
			printk(KERN_DEBUG "%s: too many iterations (%d) in nv_nic_irq_tx.\n", dev->name, i);
			break;
		}

	}
	dprintk(KERN_DEBUG "%s: nv_nic_irq_tx completed\n", dev->name);

	return IRQ_RETVAL(i);
}

#ifdef CONFIG_FORCEDETH_NAPI
static int nv_napi_poll(struct net_device *dev, int *budget)
{
	int pkts, limit = min(*budget, dev->quota);
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	unsigned long flags;
	int retcode;

	if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2) {
		pkts = nv_rx_process(dev, limit);
		retcode = nv_alloc_rx(dev);
	} else {
		pkts = nv_rx_process_optimized(dev, limit);
		retcode = nv_alloc_rx_optimized(dev);
	}

	if (retcode) {
		spin_lock_irqsave(&np->lock, flags);
		if (!np->in_shutdown)
			mod_timer(&np->oom_kick, jiffies + OOM_REFILL);
		spin_unlock_irqrestore(&np->lock, flags);
	}

	if (pkts < limit) {
		/* all done, no more packets present */
		netif_rx_complete(dev);

		/* re-enable receive interrupts */
		spin_lock_irqsave(&np->lock, flags);

		np->irqmask |= NVREG_IRQ_RX_ALL;
		if (np->msi_flags & NV_MSI_X_ENABLED)
			writel(NVREG_IRQ_RX_ALL, base + NvRegIrqMask);
		else
			writel(np->irqmask, base + NvRegIrqMask);

		spin_unlock_irqrestore(&np->lock, flags);
		return 0;
	} else {
		/* used up our quantum, so reschedule */
		dev->quota -= pkts;
		*budget -= pkts;
		return 1;
	}
}
#endif

#ifdef CONFIG_FORCEDETH_NAPI
static irqreturn_t nv_nic_irq_rx(int foo, void *data)
{
	struct net_device *dev = (struct net_device *) data;
	u8 __iomem *base = get_hwbase(dev);
	u32 events;

	events = readl(base + NvRegMSIXIrqStatus) & NVREG_IRQ_RX_ALL;
	writel(NVREG_IRQ_RX_ALL, base + NvRegMSIXIrqStatus);

	if (events) {
		netif_rx_schedule(dev);
		/* disable receive interrupts on the nic */
		writel(NVREG_IRQ_RX_ALL, base + NvRegIrqMask);
		pci_push(base);
	}
	return IRQ_HANDLED;
}
#else
static irqreturn_t nv_nic_irq_rx(int foo, void *data)
{
	struct net_device *dev = (struct net_device *) data;
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u32 events;
	int i;
	unsigned long flags;

	dprintk(KERN_DEBUG "%s: nv_nic_irq_rx\n", dev->name);

	for (i=0; ; i++) {
		events = readl(base + NvRegMSIXIrqStatus) & NVREG_IRQ_RX_ALL;
		writel(NVREG_IRQ_RX_ALL, base + NvRegMSIXIrqStatus);
		dprintk(KERN_DEBUG "%s: rx irq: %08x\n", dev->name, events);
		if (!(events & np->irqmask))
			break;

		if (nv_rx_process_optimized(dev, dev->weight)) {
			if (unlikely(nv_alloc_rx_optimized(dev))) {
				spin_lock_irqsave(&np->lock, flags);
				if (!np->in_shutdown)
					mod_timer(&np->oom_kick, jiffies + OOM_REFILL);
				spin_unlock_irqrestore(&np->lock, flags);
			}
		}

		if (unlikely(i > max_interrupt_work)) {
			spin_lock_irqsave(&np->lock, flags);
			/* disable interrupts on the nic */
			writel(NVREG_IRQ_RX_ALL, base + NvRegIrqMask);
			pci_push(base);

			if (!np->in_shutdown) {
				np->nic_poll_irq |= NVREG_IRQ_RX_ALL;
				mod_timer(&np->nic_poll, jiffies + POLL_WAIT);
			}
			spin_unlock_irqrestore(&np->lock, flags);
			printk(KERN_DEBUG "%s: too many iterations (%d) in nv_nic_irq_rx.\n", dev->name, i);
			break;
		}
	}
	dprintk(KERN_DEBUG "%s: nv_nic_irq_rx completed\n", dev->name);

	return IRQ_RETVAL(i);
}
#endif

static irqreturn_t nv_nic_irq_other(int foo, void *data)
{
	struct net_device *dev = (struct net_device *) data;
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u32 events;
	int i;
	unsigned long flags;

	dprintk(KERN_DEBUG "%s: nv_nic_irq_other\n", dev->name);

	for (i=0; ; i++) {
		events = readl(base + NvRegMSIXIrqStatus) & NVREG_IRQ_OTHER;
		writel(NVREG_IRQ_OTHER, base + NvRegMSIXIrqStatus);
		dprintk(KERN_DEBUG "%s: irq: %08x\n", dev->name, events);
		if (!(events & np->irqmask))
			break;

		/* check tx in case we reached max loop limit in tx isr */
		spin_lock_irqsave(&np->lock, flags);
		nv_tx_done_optimized(dev, TX_WORK_PER_LOOP);
		spin_unlock_irqrestore(&np->lock, flags);

		if (events & NVREG_IRQ_LINK) {
			spin_lock_irqsave(&np->lock, flags);
			nv_link_irq(dev);
			spin_unlock_irqrestore(&np->lock, flags);
		}
		if (np->need_linktimer && time_after(jiffies, np->link_timeout)) {
			spin_lock_irqsave(&np->lock, flags);
			nv_linkchange(dev);
			spin_unlock_irqrestore(&np->lock, flags);
			np->link_timeout = jiffies + LINK_TIMEOUT;
		}
		if (events & NVREG_IRQ_RECOVER_ERROR) {
			spin_lock_irq(&np->lock);
			/* disable interrupts on the nic */
			writel(NVREG_IRQ_OTHER, base + NvRegIrqMask);
			pci_push(base);

			if (!np->in_shutdown) {
				np->nic_poll_irq |= NVREG_IRQ_OTHER;
				np->recover_error = 1;
				mod_timer(&np->nic_poll, jiffies + POLL_WAIT);
			}
			spin_unlock_irq(&np->lock);
			break;
		}
		if (events & (NVREG_IRQ_UNKNOWN)) {
			printk(KERN_DEBUG "%s: received irq with unknown events 0x%x. Please report\n",
						dev->name, events);
		}
		if (unlikely(i > max_interrupt_work)) {
			spin_lock_irqsave(&np->lock, flags);
			/* disable interrupts on the nic */
			writel(NVREG_IRQ_OTHER, base + NvRegIrqMask);
			pci_push(base);

			if (!np->in_shutdown) {
				np->nic_poll_irq |= NVREG_IRQ_OTHER;
				mod_timer(&np->nic_poll, jiffies + POLL_WAIT);
			}
			spin_unlock_irqrestore(&np->lock, flags);
			printk(KERN_DEBUG "%s: too many iterations (%d) in nv_nic_irq_other.\n", dev->name, i);
			break;
		}

	}
	dprintk(KERN_DEBUG "%s: nv_nic_irq_other completed\n", dev->name);

	return IRQ_RETVAL(i);
}

static irqreturn_t nv_nic_irq_test(int foo, void *data)
{
	struct net_device *dev = (struct net_device *) data;
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u32 events;

	dprintk(KERN_DEBUG "%s: nv_nic_irq_test\n", dev->name);

	if (!(np->msi_flags & NV_MSI_X_ENABLED)) {
		events = readl(base + NvRegIrqStatus) & NVREG_IRQSTAT_MASK;
		writel(NVREG_IRQ_TIMER, base + NvRegIrqStatus);
	} else {
		events = readl(base + NvRegMSIXIrqStatus) & NVREG_IRQSTAT_MASK;
		writel(NVREG_IRQ_TIMER, base + NvRegMSIXIrqStatus);
	}
	pci_push(base);
	dprintk(KERN_DEBUG "%s: irq: %08x\n", dev->name, events);
	if (!(events & NVREG_IRQ_TIMER))
		return IRQ_RETVAL(0);

	spin_lock(&np->lock);
	np->intr_test = 1;
	spin_unlock(&np->lock);

	dprintk(KERN_DEBUG "%s: nv_nic_irq_test completed\n", dev->name);

	return IRQ_RETVAL(1);
}

static void set_msix_vector_map(struct net_device *dev, u32 vector, u32 irqmask)
{
	u8 __iomem *base = get_hwbase(dev);
	int i;
	u32 msixmap = 0;

	/* Each interrupt bit can be mapped to a MSIX vector (4 bits).
	 * MSIXMap0 represents the first 8 interrupts and MSIXMap1 represents
	 * the remaining 8 interrupts.
	 */
	for (i = 0; i < 8; i++) {
		if ((irqmask >> i) & 0x1) {
			msixmap |= vector << (i << 2);
		}
	}
	writel(readl(base + NvRegMSIXMap0) | msixmap, base + NvRegMSIXMap0);

	msixmap = 0;
	for (i = 0; i < 8; i++) {
		if ((irqmask >> (i + 8)) & 0x1) {
			msixmap |= vector << (i << 2);
		}
	}
	writel(readl(base + NvRegMSIXMap1) | msixmap, base + NvRegMSIXMap1);
}

static int nv_request_irq(struct net_device *dev, int intr_test)
{
	struct fe_priv *np = get_nvpriv(dev);
	u8 __iomem *base = get_hwbase(dev);
	int ret = 1;
	int i;
	irqreturn_t (*handler)(int foo, void *data);

	if (intr_test) {
		handler = nv_nic_irq_test;
	} else {
		if (np->desc_ver == DESC_VER_3)
			handler = nv_nic_irq_optimized;
		else
			handler = nv_nic_irq;
	}

	if (np->msi_flags & NV_MSI_X_CAPABLE) {
		for (i = 0; i < (np->msi_flags & NV_MSI_X_VECTORS_MASK); i++) {
			np->msi_x_entry[i].entry = i;
		}
		if ((ret = pci_enable_msix(np->pci_dev, np->msi_x_entry, (np->msi_flags & NV_MSI_X_VECTORS_MASK))) == 0) {
			np->msi_flags |= NV_MSI_X_ENABLED;
			if (optimization_mode == NV_OPTIMIZATION_MODE_THROUGHPUT && !intr_test) {
				/* Request irq for rx handling */
				if (request_irq(np->msi_x_entry[NV_MSI_X_VECTOR_RX].vector, &nv_nic_irq_rx, IRQF_SHARED, dev->name, dev) != 0) {
					printk(KERN_INFO "forcedeth: request_irq failed for rx %d\n", ret);
					pci_disable_msix(np->pci_dev);
					np->msi_flags &= ~NV_MSI_X_ENABLED;
					goto out_err;
				}
				/* Request irq for tx handling */
				if (request_irq(np->msi_x_entry[NV_MSI_X_VECTOR_TX].vector, &nv_nic_irq_tx, IRQF_SHARED, dev->name, dev) != 0) {
					printk(KERN_INFO "forcedeth: request_irq failed for tx %d\n", ret);
					pci_disable_msix(np->pci_dev);
					np->msi_flags &= ~NV_MSI_X_ENABLED;
					goto out_free_rx;
				}
				/* Request irq for link and timer handling */
				if (request_irq(np->msi_x_entry[NV_MSI_X_VECTOR_OTHER].vector, &nv_nic_irq_other, IRQF_SHARED, dev->name, dev) != 0) {
					printk(KERN_INFO "forcedeth: request_irq failed for link %d\n", ret);
					pci_disable_msix(np->pci_dev);
					np->msi_flags &= ~NV_MSI_X_ENABLED;
					goto out_free_tx;
				}
				/* map interrupts to their respective vector */
				writel(0, base + NvRegMSIXMap0);
				writel(0, base + NvRegMSIXMap1);
				set_msix_vector_map(dev, NV_MSI_X_VECTOR_RX, NVREG_IRQ_RX_ALL);
				set_msix_vector_map(dev, NV_MSI_X_VECTOR_TX, NVREG_IRQ_TX_ALL);
				set_msix_vector_map(dev, NV_MSI_X_VECTOR_OTHER, NVREG_IRQ_OTHER);
			} else {
				/* Request irq for all interrupts */
				if (request_irq(np->msi_x_entry[NV_MSI_X_VECTOR_ALL].vector, handler, IRQF_SHARED, dev->name, dev) != 0) {
					printk(KERN_INFO "forcedeth: request_irq failed %d\n", ret);
					pci_disable_msix(np->pci_dev);
					np->msi_flags &= ~NV_MSI_X_ENABLED;
					goto out_err;
				}

				/* map interrupts to vector 0 */
				writel(0, base + NvRegMSIXMap0);
				writel(0, base + NvRegMSIXMap1);
			}
		}
	}
	if (ret != 0 && np->msi_flags & NV_MSI_CAPABLE) {
		if ((ret = pci_enable_msi(np->pci_dev)) == 0) {
			np->msi_flags |= NV_MSI_ENABLED;
			if (request_irq(np->pci_dev->irq, handler, IRQF_SHARED, dev->name, dev) != 0) {
				printk(KERN_INFO "forcedeth: request_irq failed %d\n", ret);
				pci_disable_msi(np->pci_dev);
				np->msi_flags &= ~NV_MSI_ENABLED;
				goto out_err;
			}

			/* map interrupts to vector 0 */
			writel(0, base + NvRegMSIMap0);
			writel(0, base + NvRegMSIMap1);
			/* enable msi vector 0 */
			writel(NVREG_MSI_VECTOR_0_ENABLED, base + NvRegMSIIrqMask);
		}
	}
	if (ret != 0) {
		if (request_irq(np->pci_dev->irq, handler, IRQF_SHARED, dev->name, dev) != 0)
			goto out_err;

	}

	return 0;
out_free_tx:
	free_irq(np->msi_x_entry[NV_MSI_X_VECTOR_TX].vector, dev);
out_free_rx:
	free_irq(np->msi_x_entry[NV_MSI_X_VECTOR_RX].vector, dev);
out_err:
	return 1;
}

static void nv_free_irq(struct net_device *dev)
{
	struct fe_priv *np = get_nvpriv(dev);
	int i;

	if (np->msi_flags & NV_MSI_X_ENABLED) {
		for (i = 0; i < (np->msi_flags & NV_MSI_X_VECTORS_MASK); i++) {
			free_irq(np->msi_x_entry[i].vector, dev);
		}
		pci_disable_msix(np->pci_dev);
		np->msi_flags &= ~NV_MSI_X_ENABLED;
	} else {
		free_irq(np->pci_dev->irq, dev);
		if (np->msi_flags & NV_MSI_ENABLED) {
			pci_disable_msi(np->pci_dev);
			np->msi_flags &= ~NV_MSI_ENABLED;
		}
	}
}

static void nv_do_nic_poll(unsigned long data)
{
	struct net_device *dev = (struct net_device *) data;
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u32 mask = 0;

	/*
	 * First disable irq(s) and then
	 * reenable interrupts on the nic, we have to do this before calling
	 * nv_nic_irq because that may decide to do otherwise
	 */

	if (!using_multi_irqs(dev)) {
		if (np->msi_flags & NV_MSI_X_ENABLED)
			disable_irq_lockdep(np->msi_x_entry[NV_MSI_X_VECTOR_ALL].vector);
		else
			disable_irq_lockdep(dev->irq);
		mask = np->irqmask;
	} else {
		if (np->nic_poll_irq & NVREG_IRQ_RX_ALL) {
			disable_irq_lockdep(np->msi_x_entry[NV_MSI_X_VECTOR_RX].vector);
			mask |= NVREG_IRQ_RX_ALL;
		}
		if (np->nic_poll_irq & NVREG_IRQ_TX_ALL) {
			disable_irq_lockdep(np->msi_x_entry[NV_MSI_X_VECTOR_TX].vector);
			mask |= NVREG_IRQ_TX_ALL;
		}
		if (np->nic_poll_irq & NVREG_IRQ_OTHER) {
			disable_irq_lockdep(np->msi_x_entry[NV_MSI_X_VECTOR_OTHER].vector);
			mask |= NVREG_IRQ_OTHER;
		}
	}
	np->nic_poll_irq = 0;

	if (np->recover_error) {
		np->recover_error = 0;
		printk(KERN_INFO "forcedeth: MAC in recoverable error state\n");
		if (netif_running(dev)) {
			netif_tx_lock_bh(dev);
			spin_lock(&np->lock);
			/* stop engines */
			nv_stop_rx(dev);
			nv_stop_tx(dev);
			nv_txrx_reset(dev);
			/* drain rx queue */
			nv_drain_rx(dev);
			nv_drain_tx(dev);
			/* reinit driver view of the rx queue */
			set_bufsize(dev);
			if (nv_init_ring(dev)) {
				if (!np->in_shutdown)
					mod_timer(&np->oom_kick, jiffies + OOM_REFILL);
			}
			/* reinit nic view of the rx queue */
			writel(np->rx_buf_sz, base + NvRegOffloadConfig);
			setup_hw_rings(dev, NV_SETUP_RX_RING | NV_SETUP_TX_RING);
			writel( ((np->rx_ring_size-1) << NVREG_RINGSZ_RXSHIFT) + ((np->tx_ring_size-1) << NVREG_RINGSZ_TXSHIFT),
				base + NvRegRingSizes);
			pci_push(base);
			writel(NVREG_TXRXCTL_KICK|np->txrxctl_bits, get_hwbase(dev) + NvRegTxRxControl);
			pci_push(base);

			/* restart rx engine */
			nv_start_rx(dev);
			nv_start_tx(dev);
			spin_unlock(&np->lock);
			netif_tx_unlock_bh(dev);
		}
	}

	/* FIXME: Do we need synchronize_irq(dev->irq) here? */

	writel(mask, base + NvRegIrqMask);
	pci_push(base);

	if (!using_multi_irqs(dev)) {
		if (np->desc_ver == DESC_VER_3)
			nv_nic_irq_optimized(0, dev);
		else
			nv_nic_irq(0, dev);
		if (np->msi_flags & NV_MSI_X_ENABLED)
			enable_irq_lockdep(np->msi_x_entry[NV_MSI_X_VECTOR_ALL].vector);
		else
			enable_irq_lockdep(dev->irq);
	} else {
		if (np->nic_poll_irq & NVREG_IRQ_RX_ALL) {
			nv_nic_irq_rx(0, dev);
			enable_irq_lockdep(np->msi_x_entry[NV_MSI_X_VECTOR_RX].vector);
		}
		if (np->nic_poll_irq & NVREG_IRQ_TX_ALL) {
			nv_nic_irq_tx(0, dev);
			enable_irq_lockdep(np->msi_x_entry[NV_MSI_X_VECTOR_TX].vector);
		}
		if (np->nic_poll_irq & NVREG_IRQ_OTHER) {
			nv_nic_irq_other(0, dev);
			enable_irq_lockdep(np->msi_x_entry[NV_MSI_X_VECTOR_OTHER].vector);
		}
	}
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void nv_poll_controller(struct net_device *dev)
{
	nv_do_nic_poll((unsigned long) dev);
}
#endif

static void nv_do_stats_poll(unsigned long data)
{
	struct net_device *dev = (struct net_device *) data;
	struct fe_priv *np = netdev_priv(dev);

	nv_get_hw_stats(dev);

	if (!np->in_shutdown)
		mod_timer(&np->stats_poll, jiffies + STATS_INTERVAL);
}

static void nv_get_drvinfo(struct net_device *dev, struct ethtool_drvinfo *info)
{
	struct fe_priv *np = netdev_priv(dev);
	strcpy(info->driver, "forcedeth");
	strcpy(info->version, FORCEDETH_VERSION);
	strcpy(info->bus_info, pci_name(np->pci_dev));
}

static void nv_get_wol(struct net_device *dev, struct ethtool_wolinfo *wolinfo)
{
	struct fe_priv *np = netdev_priv(dev);
	wolinfo->supported = WAKE_MAGIC;

	spin_lock_irq(&np->lock);
	if (np->wolenabled)
		wolinfo->wolopts = WAKE_MAGIC;
	spin_unlock_irq(&np->lock);
}

static int nv_set_wol(struct net_device *dev, struct ethtool_wolinfo *wolinfo)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u32 flags = 0;

	if (wolinfo->wolopts == 0) {
		np->wolenabled = 0;
	} else if (wolinfo->wolopts & WAKE_MAGIC) {
		np->wolenabled = 1;
		flags = NVREG_WAKEUPFLAGS_ENABLE;
	}
	if (netif_running(dev)) {
		spin_lock_irq(&np->lock);
		writel(flags, base + NvRegWakeUpFlags);
		spin_unlock_irq(&np->lock);
	}
	return 0;
}

static int nv_get_settings(struct net_device *dev, struct ethtool_cmd *ecmd)
{
	struct fe_priv *np = netdev_priv(dev);
	int adv;

	spin_lock_irq(&np->lock);
	ecmd->port = PORT_MII;
	if (!netif_running(dev)) {
		/* We do not track link speed / duplex setting if the
		 * interface is disabled. Force a link check */
		if (nv_update_linkspeed(dev)) {
			if (!netif_carrier_ok(dev))
				netif_carrier_on(dev);
		} else {
			if (netif_carrier_ok(dev))
				netif_carrier_off(dev);
		}
	}

	if (netif_carrier_ok(dev)) {
		switch(np->linkspeed & (NVREG_LINKSPEED_MASK)) {
		case NVREG_LINKSPEED_10:
			ecmd->speed = SPEED_10;
			break;
		case NVREG_LINKSPEED_100:
			ecmd->speed = SPEED_100;
			break;
		case NVREG_LINKSPEED_1000:
			ecmd->speed = SPEED_1000;
			break;
		}
		ecmd->duplex = DUPLEX_HALF;
		if (np->duplex)
			ecmd->duplex = DUPLEX_FULL;
	} else {
		ecmd->speed = -1;
		ecmd->duplex = -1;
	}

	ecmd->autoneg = np->autoneg;

	ecmd->advertising = ADVERTISED_MII;
	if (np->autoneg) {
		ecmd->advertising |= ADVERTISED_Autoneg;
		adv = mii_rw(dev, np->phyaddr, MII_ADVERTISE, MII_READ);
		if (adv & ADVERTISE_10HALF)
			ecmd->advertising |= ADVERTISED_10baseT_Half;
		if (adv & ADVERTISE_10FULL)
			ecmd->advertising |= ADVERTISED_10baseT_Full;
		if (adv & ADVERTISE_100HALF)
			ecmd->advertising |= ADVERTISED_100baseT_Half;
		if (adv & ADVERTISE_100FULL)
			ecmd->advertising |= ADVERTISED_100baseT_Full;
		if (np->gigabit == PHY_GIGABIT) {
			adv = mii_rw(dev, np->phyaddr, MII_CTRL1000, MII_READ);
			if (adv & ADVERTISE_1000FULL)
				ecmd->advertising |= ADVERTISED_1000baseT_Full;
		}
	}
	ecmd->supported = (SUPPORTED_Autoneg |
		SUPPORTED_10baseT_Half | SUPPORTED_10baseT_Full |
		SUPPORTED_100baseT_Half | SUPPORTED_100baseT_Full |
		SUPPORTED_MII);
	if (np->gigabit == PHY_GIGABIT)
		ecmd->supported |= SUPPORTED_1000baseT_Full;

	ecmd->phy_address = np->phyaddr;
	ecmd->transceiver = XCVR_EXTERNAL;

	/* ignore maxtxpkt, maxrxpkt for now */
	spin_unlock_irq(&np->lock);
	return 0;
}

static int nv_set_settings(struct net_device *dev, struct ethtool_cmd *ecmd)
{
	struct fe_priv *np = netdev_priv(dev);

	if (ecmd->port != PORT_MII)
		return -EINVAL;
	if (ecmd->transceiver != XCVR_EXTERNAL)
		return -EINVAL;
	if (ecmd->phy_address != np->phyaddr) {
		/* TODO: support switching between multiple phys. Should be
		 * trivial, but not enabled due to lack of test hardware. */
		return -EINVAL;
	}
	if (ecmd->autoneg == AUTONEG_ENABLE) {
		u32 mask;

		mask = ADVERTISED_10baseT_Half | ADVERTISED_10baseT_Full |
			  ADVERTISED_100baseT_Half | ADVERTISED_100baseT_Full;
		if (np->gigabit == PHY_GIGABIT)
			mask |= ADVERTISED_1000baseT_Full;

		if ((ecmd->advertising & mask) == 0)
			return -EINVAL;

	} else if (ecmd->autoneg == AUTONEG_DISABLE) {
		/* Note: autonegotiation disable, speed 1000 intentionally
		 * forbidden - noone should need that. */

		if (ecmd->speed != SPEED_10 && ecmd->speed != SPEED_100)
			return -EINVAL;
		if (ecmd->duplex != DUPLEX_HALF && ecmd->duplex != DUPLEX_FULL)
			return -EINVAL;
	} else {
		return -EINVAL;
	}

	netif_carrier_off(dev);
	if (netif_running(dev)) {
		nv_disable_irq(dev);
		netif_tx_lock_bh(dev);
		spin_lock(&np->lock);
		/* stop engines */
		nv_stop_rx(dev);
		nv_stop_tx(dev);
		spin_unlock(&np->lock);
		netif_tx_unlock_bh(dev);
	}

	if (ecmd->autoneg == AUTONEG_ENABLE) {
		int adv, bmcr;

		np->autoneg = 1;

		/* advertise only what has been requested */
		adv = mii_rw(dev, np->phyaddr, MII_ADVERTISE, MII_READ);
		adv &= ~(ADVERTISE_ALL | ADVERTISE_100BASE4 | ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM);
		if (ecmd->advertising & ADVERTISED_10baseT_Half)
			adv |= ADVERTISE_10HALF;
		if (ecmd->advertising & ADVERTISED_10baseT_Full)
			adv |= ADVERTISE_10FULL;
		if (ecmd->advertising & ADVERTISED_100baseT_Half)
			adv |= ADVERTISE_100HALF;
		if (ecmd->advertising & ADVERTISED_100baseT_Full)
			adv |= ADVERTISE_100FULL;
		if (np->pause_flags & NV_PAUSEFRAME_RX_REQ)  /* for rx we set both advertisments but disable tx pause */
			adv |=  ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM;
		if (np->pause_flags & NV_PAUSEFRAME_TX_REQ)
			adv |=  ADVERTISE_PAUSE_ASYM;
		mii_rw(dev, np->phyaddr, MII_ADVERTISE, adv);

		if (np->gigabit == PHY_GIGABIT) {
			adv = mii_rw(dev, np->phyaddr, MII_CTRL1000, MII_READ);
			adv &= ~ADVERTISE_1000FULL;
			if (ecmd->advertising & ADVERTISED_1000baseT_Full)
				adv |= ADVERTISE_1000FULL;
			mii_rw(dev, np->phyaddr, MII_CTRL1000, adv);
		}

		if (netif_running(dev))
			printk(KERN_INFO "%s: link down.\n", dev->name);
		bmcr = mii_rw(dev, np->phyaddr, MII_BMCR, MII_READ);
		if (np->phy_model == PHY_MODEL_MARVELL_E3016) {
			bmcr |= BMCR_ANENABLE;
			/* reset the phy in order for settings to stick,
			 * and cause autoneg to start */
			if (phy_reset(dev, bmcr)) {
				printk(KERN_INFO "%s: phy reset failed\n", dev->name);
				return -EINVAL;
			}
		} else {
			bmcr |= (BMCR_ANENABLE | BMCR_ANRESTART);
			mii_rw(dev, np->phyaddr, MII_BMCR, bmcr);
		}
	} else {
		int adv, bmcr;

		np->autoneg = 0;

		adv = mii_rw(dev, np->phyaddr, MII_ADVERTISE, MII_READ);
		adv &= ~(ADVERTISE_ALL | ADVERTISE_100BASE4 | ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM);
		if (ecmd->speed == SPEED_10 && ecmd->duplex == DUPLEX_HALF)
			adv |= ADVERTISE_10HALF;
		if (ecmd->speed == SPEED_10 && ecmd->duplex == DUPLEX_FULL)
			adv |= ADVERTISE_10FULL;
		if (ecmd->speed == SPEED_100 && ecmd->duplex == DUPLEX_HALF)
			adv |= ADVERTISE_100HALF;
		if (ecmd->speed == SPEED_100 && ecmd->duplex == DUPLEX_FULL)
			adv |= ADVERTISE_100FULL;
		np->pause_flags &= ~(NV_PAUSEFRAME_AUTONEG|NV_PAUSEFRAME_RX_ENABLE|NV_PAUSEFRAME_TX_ENABLE);
		if (np->pause_flags & NV_PAUSEFRAME_RX_REQ) {/* for rx we set both advertisments but disable tx pause */
			adv |=  ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM;
			np->pause_flags |= NV_PAUSEFRAME_RX_ENABLE;
		}
		if (np->pause_flags & NV_PAUSEFRAME_TX_REQ) {
			adv |=  ADVERTISE_PAUSE_ASYM;
			np->pause_flags |= NV_PAUSEFRAME_TX_ENABLE;
		}
		mii_rw(dev, np->phyaddr, MII_ADVERTISE, adv);
		np->fixed_mode = adv;

		if (np->gigabit == PHY_GIGABIT) {
			adv = mii_rw(dev, np->phyaddr, MII_CTRL1000, MII_READ);
			adv &= ~ADVERTISE_1000FULL;
			mii_rw(dev, np->phyaddr, MII_CTRL1000, adv);
		}

		bmcr = mii_rw(dev, np->phyaddr, MII_BMCR, MII_READ);
		bmcr &= ~(BMCR_ANENABLE|BMCR_SPEED100|BMCR_SPEED1000|BMCR_FULLDPLX);
		if (np->fixed_mode & (ADVERTISE_10FULL|ADVERTISE_100FULL))
			bmcr |= BMCR_FULLDPLX;
		if (np->fixed_mode & (ADVERTISE_100HALF|ADVERTISE_100FULL))
			bmcr |= BMCR_SPEED100;
		if (np->phy_oui == PHY_OUI_MARVELL) {
			/* reset the phy in order for forced mode settings to stick */
			if (phy_reset(dev, bmcr)) {
				printk(KERN_INFO "%s: phy reset failed\n", dev->name);
				return -EINVAL;
			}
		} else {
			mii_rw(dev, np->phyaddr, MII_BMCR, bmcr);
			if (netif_running(dev)) {
				/* Wait a bit and then reconfigure the nic. */
				udelay(10);
				nv_linkchange(dev);
			}
		}
	}

	if (netif_running(dev)) {
		nv_start_rx(dev);
		nv_start_tx(dev);
		nv_enable_irq(dev);
	}

	return 0;
}

#define FORCEDETH_REGS_VER	1

static int nv_get_regs_len(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	return np->register_size;
}

static void nv_get_regs(struct net_device *dev, struct ethtool_regs *regs, void *buf)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u32 *rbuf = buf;
	int i;

	regs->version = FORCEDETH_REGS_VER;
	spin_lock_irq(&np->lock);
	for (i = 0;i <= np->register_size/sizeof(u32); i++)
		rbuf[i] = readl(base + i*sizeof(u32));
	spin_unlock_irq(&np->lock);
}

static int nv_nway_reset(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	int ret;

	if (np->autoneg) {
		int bmcr;

		netif_carrier_off(dev);
		if (netif_running(dev)) {
			nv_disable_irq(dev);
			netif_tx_lock_bh(dev);
			spin_lock(&np->lock);
			/* stop engines */
			nv_stop_rx(dev);
			nv_stop_tx(dev);
			spin_unlock(&np->lock);
			netif_tx_unlock_bh(dev);
			printk(KERN_INFO "%s: link down.\n", dev->name);
		}

		bmcr = mii_rw(dev, np->phyaddr, MII_BMCR, MII_READ);
		if (np->phy_model == PHY_MODEL_MARVELL_E3016) {
			bmcr |= BMCR_ANENABLE;
			/* reset the phy in order for settings to stick*/
			if (phy_reset(dev, bmcr)) {
				printk(KERN_INFO "%s: phy reset failed\n", dev->name);
				return -EINVAL;
			}
		} else {
			bmcr |= (BMCR_ANENABLE | BMCR_ANRESTART);
			mii_rw(dev, np->phyaddr, MII_BMCR, bmcr);
		}

		if (netif_running(dev)) {
			nv_start_rx(dev);
			nv_start_tx(dev);
			nv_enable_irq(dev);
		}
		ret = 0;
	} else {
		ret = -EINVAL;
	}

	return ret;
}

static int nv_set_tso(struct net_device *dev, u32 value)
{
	struct fe_priv *np = netdev_priv(dev);

	if ((np->driver_data & DEV_HAS_CHECKSUM))
		return ethtool_op_set_tso(dev, value);
	else
		return -EOPNOTSUPP;
}

static void nv_get_ringparam(struct net_device *dev, struct ethtool_ringparam* ring)
{
	struct fe_priv *np = netdev_priv(dev);

	ring->rx_max_pending = (np->desc_ver == DESC_VER_1) ? RING_MAX_DESC_VER_1 : RING_MAX_DESC_VER_2_3;
	ring->rx_mini_max_pending = 0;
	ring->rx_jumbo_max_pending = 0;
	ring->tx_max_pending = (np->desc_ver == DESC_VER_1) ? RING_MAX_DESC_VER_1 : RING_MAX_DESC_VER_2_3;

	ring->rx_pending = np->rx_ring_size;
	ring->rx_mini_pending = 0;
	ring->rx_jumbo_pending = 0;
	ring->tx_pending = np->tx_ring_size;
}

static int nv_set_ringparam(struct net_device *dev, struct ethtool_ringparam* ring)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	u8 *rxtx_ring, *rx_skbuff, *tx_skbuff;
	dma_addr_t ring_addr;

	if (ring->rx_pending < RX_RING_MIN ||
	    ring->tx_pending < TX_RING_MIN ||
	    ring->rx_mini_pending != 0 ||
	    ring->rx_jumbo_pending != 0 ||
	    (np->desc_ver == DESC_VER_1 &&
	     (ring->rx_pending > RING_MAX_DESC_VER_1 ||
	      ring->tx_pending > RING_MAX_DESC_VER_1)) ||
	    (np->desc_ver != DESC_VER_1 &&
	     (ring->rx_pending > RING_MAX_DESC_VER_2_3 ||
	      ring->tx_pending > RING_MAX_DESC_VER_2_3))) {
		return -EINVAL;
	}

	/* allocate new rings */
	if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2) {
		rxtx_ring = pci_alloc_consistent(np->pci_dev,
					    sizeof(struct ring_desc) * (ring->rx_pending + ring->tx_pending),
					    &ring_addr);
	} else {
		rxtx_ring = pci_alloc_consistent(np->pci_dev,
					    sizeof(struct ring_desc_ex) * (ring->rx_pending + ring->tx_pending),
					    &ring_addr);
	}
	rx_skbuff = kmalloc(sizeof(struct nv_skb_map) * ring->rx_pending, GFP_KERNEL);
	tx_skbuff = kmalloc(sizeof(struct nv_skb_map) * ring->tx_pending, GFP_KERNEL);
	if (!rxtx_ring || !rx_skbuff || !tx_skbuff) {
		/* fall back to old rings */
		if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2) {
			if (rxtx_ring)
				pci_free_consistent(np->pci_dev, sizeof(struct ring_desc) * (ring->rx_pending + ring->tx_pending),
						    rxtx_ring, ring_addr);
		} else {
			if (rxtx_ring)
				pci_free_consistent(np->pci_dev, sizeof(struct ring_desc_ex) * (ring->rx_pending + ring->tx_pending),
						    rxtx_ring, ring_addr);
		}
		if (rx_skbuff)
			kfree(rx_skbuff);
		if (tx_skbuff)
			kfree(tx_skbuff);
		goto exit;
	}

	if (netif_running(dev)) {
		nv_disable_irq(dev);
		netif_tx_lock_bh(dev);
		spin_lock(&np->lock);
		/* stop engines */
		nv_stop_rx(dev);
		nv_stop_tx(dev);
		nv_txrx_reset(dev);
		/* drain queues */
		nv_drain_rx(dev);
		nv_drain_tx(dev);
		/* delete queues */
		free_rings(dev);
	}

	/* set new values */
	np->rx_ring_size = ring->rx_pending;
	np->tx_ring_size = ring->tx_pending;
	if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2) {
		np->rx_ring.orig = (struct ring_desc*)rxtx_ring;
		np->tx_ring.orig = &np->rx_ring.orig[np->rx_ring_size];
	} else {
		np->rx_ring.ex = (struct ring_desc_ex*)rxtx_ring;
		np->tx_ring.ex = &np->rx_ring.ex[np->rx_ring_size];
	}
	np->rx_skb = (struct nv_skb_map*)rx_skbuff;
	np->tx_skb = (struct nv_skb_map*)tx_skbuff;
	np->ring_addr = ring_addr;

	memset(np->rx_skb, 0, sizeof(struct nv_skb_map) * np->rx_ring_size);
	memset(np->tx_skb, 0, sizeof(struct nv_skb_map) * np->tx_ring_size);

	if (netif_running(dev)) {
		/* reinit driver view of the queues */
		set_bufsize(dev);
		if (nv_init_ring(dev)) {
			if (!np->in_shutdown)
				mod_timer(&np->oom_kick, jiffies + OOM_REFILL);
		}

		/* reinit nic view of the queues */
		writel(np->rx_buf_sz, base + NvRegOffloadConfig);
		setup_hw_rings(dev, NV_SETUP_RX_RING | NV_SETUP_TX_RING);
		writel( ((np->rx_ring_size-1) << NVREG_RINGSZ_RXSHIFT) + ((np->tx_ring_size-1) << NVREG_RINGSZ_TXSHIFT),
			base + NvRegRingSizes);
		pci_push(base);
		writel(NVREG_TXRXCTL_KICK|np->txrxctl_bits, get_hwbase(dev) + NvRegTxRxControl);
		pci_push(base);

		/* restart engines */
		nv_start_rx(dev);
		nv_start_tx(dev);
		spin_unlock(&np->lock);
		netif_tx_unlock_bh(dev);
		nv_enable_irq(dev);
	}
	return 0;
exit:
	return -ENOMEM;
}

static void nv_get_pauseparam(struct net_device *dev, struct ethtool_pauseparam* pause)
{
	struct fe_priv *np = netdev_priv(dev);

	pause->autoneg = (np->pause_flags & NV_PAUSEFRAME_AUTONEG) != 0;
	pause->rx_pause = (np->pause_flags & NV_PAUSEFRAME_RX_ENABLE) != 0;
	pause->tx_pause = (np->pause_flags & NV_PAUSEFRAME_TX_ENABLE) != 0;
}

static int nv_set_pauseparam(struct net_device *dev, struct ethtool_pauseparam* pause)
{
	struct fe_priv *np = netdev_priv(dev);
	int adv, bmcr;

	if ((!np->autoneg && np->duplex == 0) ||
	    (np->autoneg && !pause->autoneg && np->duplex == 0)) {
		printk(KERN_INFO "%s: can not set pause settings when forced link is in half duplex.\n",
		       dev->name);
		return -EINVAL;
	}
	if (pause->tx_pause && !(np->pause_flags & NV_PAUSEFRAME_TX_CAPABLE)) {
		printk(KERN_INFO "%s: hardware does not support tx pause frames.\n", dev->name);
		return -EINVAL;
	}

	netif_carrier_off(dev);
	if (netif_running(dev)) {
		nv_disable_irq(dev);
		netif_tx_lock_bh(dev);
		spin_lock(&np->lock);
		/* stop engines */
		nv_stop_rx(dev);
		nv_stop_tx(dev);
		spin_unlock(&np->lock);
		netif_tx_unlock_bh(dev);
	}

	np->pause_flags &= ~(NV_PAUSEFRAME_RX_REQ|NV_PAUSEFRAME_TX_REQ);
	if (pause->rx_pause)
		np->pause_flags |= NV_PAUSEFRAME_RX_REQ;
	if (pause->tx_pause)
		np->pause_flags |= NV_PAUSEFRAME_TX_REQ;

	if (np->autoneg && pause->autoneg) {
		np->pause_flags |= NV_PAUSEFRAME_AUTONEG;

		adv = mii_rw(dev, np->phyaddr, MII_ADVERTISE, MII_READ);
		adv &= ~(ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM);
		if (np->pause_flags & NV_PAUSEFRAME_RX_REQ) /* for rx we set both advertisments but disable tx pause */
			adv |=  ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM;
		if (np->pause_flags & NV_PAUSEFRAME_TX_REQ)
			adv |=  ADVERTISE_PAUSE_ASYM;
		mii_rw(dev, np->phyaddr, MII_ADVERTISE, adv);

		if (netif_running(dev))
			printk(KERN_INFO "%s: link down.\n", dev->name);
		bmcr = mii_rw(dev, np->phyaddr, MII_BMCR, MII_READ);
		bmcr |= (BMCR_ANENABLE | BMCR_ANRESTART);
		mii_rw(dev, np->phyaddr, MII_BMCR, bmcr);
	} else {
		np->pause_flags &= ~(NV_PAUSEFRAME_AUTONEG|NV_PAUSEFRAME_RX_ENABLE|NV_PAUSEFRAME_TX_ENABLE);
		if (pause->rx_pause)
			np->pause_flags |= NV_PAUSEFRAME_RX_ENABLE;
		if (pause->tx_pause)
			np->pause_flags |= NV_PAUSEFRAME_TX_ENABLE;

		if (!netif_running(dev))
			nv_update_linkspeed(dev);
		else
			nv_update_pause(dev, np->pause_flags);
	}

	if (netif_running(dev)) {
		nv_start_rx(dev);
		nv_start_tx(dev);
		nv_enable_irq(dev);
	}
	return 0;
}

static u32 nv_get_rx_csum(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	return (np->rx_csum) != 0;
}

static int nv_set_rx_csum(struct net_device *dev, u32 data)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	int retcode = 0;

	if (np->driver_data & DEV_HAS_CHECKSUM) {
		if (data) {
			np->rx_csum = 1;
			np->txrxctl_bits |= NVREG_TXRXCTL_RXCHECK;
		} else {
			np->rx_csum = 0;
			/* vlan is dependent on rx checksum offload */
			if (!(np->vlanctl_bits & NVREG_VLANCONTROL_ENABLE))
				np->txrxctl_bits &= ~NVREG_TXRXCTL_RXCHECK;
		}
		if (netif_running(dev)) {
			spin_lock_irq(&np->lock);
			writel(np->txrxctl_bits, base + NvRegTxRxControl);
			spin_unlock_irq(&np->lock);
		}
	} else {
		return -EINVAL;
	}

	return retcode;
}

static int nv_set_tx_csum(struct net_device *dev, u32 data)
{
	struct fe_priv *np = netdev_priv(dev);

	if (np->driver_data & DEV_HAS_CHECKSUM)
		return ethtool_op_set_tx_hw_csum(dev, data);
	else
		return -EOPNOTSUPP;
}

static int nv_set_sg(struct net_device *dev, u32 data)
{
	struct fe_priv *np = netdev_priv(dev);

	if (np->driver_data & DEV_HAS_CHECKSUM)
		return ethtool_op_set_sg(dev, data);
	else
		return -EOPNOTSUPP;
}

static int nv_get_stats_count(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);

	if (np->driver_data & DEV_HAS_STATISTICS_V1)
		return NV_DEV_STATISTICS_V1_COUNT;
	else if (np->driver_data & DEV_HAS_STATISTICS_V2)
		return NV_DEV_STATISTICS_V2_COUNT;
	else
		return 0;
}

static void nv_get_ethtool_stats(struct net_device *dev, struct ethtool_stats *estats, u64 *buffer)
{
	struct fe_priv *np = netdev_priv(dev);

	/* update stats */
	nv_do_stats_poll((unsigned long)dev);

	memcpy(buffer, &np->estats, nv_get_stats_count(dev)*sizeof(u64));
}

static int nv_self_test_count(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);

	if (np->driver_data & DEV_HAS_TEST_EXTENDED)
		return NV_TEST_COUNT_EXTENDED;
	else
		return NV_TEST_COUNT_BASE;
}

static int nv_link_test(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	int mii_status;

	mii_rw(dev, np->phyaddr, MII_BMSR, MII_READ);
	mii_status = mii_rw(dev, np->phyaddr, MII_BMSR, MII_READ);

	/* check phy link status */
	if (!(mii_status & BMSR_LSTATUS))
		return 0;
	else
		return 1;
}

static int nv_register_test(struct net_device *dev)
{
	u8 __iomem *base = get_hwbase(dev);
	int i = 0;
	u32 orig_read, new_read;

	do {
		orig_read = readl(base + nv_registers_test[i].reg);

		/* xor with mask to toggle bits */
		orig_read ^= nv_registers_test[i].mask;

		writel(orig_read, base + nv_registers_test[i].reg);

		new_read = readl(base + nv_registers_test[i].reg);

		if ((new_read & nv_registers_test[i].mask) != (orig_read & nv_registers_test[i].mask))
			return 0;

		/* restore original value */
		orig_read ^= nv_registers_test[i].mask;
		writel(orig_read, base + nv_registers_test[i].reg);

	} while (nv_registers_test[++i].reg != 0);

	return 1;
}

static int nv_interrupt_test(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	int ret = 1;
	int testcnt;
	u32 save_msi_flags, save_poll_interval = 0;

	if (netif_running(dev)) {
		/* free current irq */
		nv_free_irq(dev);
		save_poll_interval = readl(base+NvRegPollingInterval);
	}

	/* flag to test interrupt handler */
	np->intr_test = 0;

	/* setup test irq */
	save_msi_flags = np->msi_flags;
	np->msi_flags &= ~NV_MSI_X_VECTORS_MASK;
	np->msi_flags |= 0x001; /* setup 1 vector */
	if (nv_request_irq(dev, 1))
		return 0;

	/* setup timer interrupt */
	writel(NVREG_POLL_DEFAULT_CPU, base + NvRegPollingInterval);
	writel(NVREG_UNKSETUP6_VAL, base + NvRegUnknownSetupReg6);

	nv_enable_hw_interrupts(dev, NVREG_IRQ_TIMER);

	/* wait for at least one interrupt */
	msleep(100);

	spin_lock_irq(&np->lock);

	/* flag should be set within ISR */
	testcnt = np->intr_test;
	if (!testcnt)
		ret = 2;

	nv_disable_hw_interrupts(dev, NVREG_IRQ_TIMER);
	if (!(np->msi_flags & NV_MSI_X_ENABLED))
		writel(NVREG_IRQSTAT_MASK, base + NvRegIrqStatus);
	else
		writel(NVREG_IRQSTAT_MASK, base + NvRegMSIXIrqStatus);

	spin_unlock_irq(&np->lock);

	nv_free_irq(dev);

	np->msi_flags = save_msi_flags;

	if (netif_running(dev)) {
		writel(save_poll_interval, base + NvRegPollingInterval);
		writel(NVREG_UNKSETUP6_VAL, base + NvRegUnknownSetupReg6);
		/* restore original irq */
		if (nv_request_irq(dev, 0))
			return 0;
	}

	return ret;
}

static int nv_loopback_test(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	struct sk_buff *tx_skb, *rx_skb;
	dma_addr_t test_dma_addr;
	u32 tx_flags_extra = (np->desc_ver == DESC_VER_1 ? NV_TX_LASTPACKET : NV_TX2_LASTPACKET);
	u32 flags;
	int len, i, pkt_len;
	u8 *pkt_data;
	u32 filter_flags = 0;
	u32 misc1_flags = 0;
	int ret = 1;

	if (netif_running(dev)) {
		nv_disable_irq(dev);
		filter_flags = readl(base + NvRegPacketFilterFlags);
		misc1_flags = readl(base + NvRegMisc1);
	} else {
		nv_txrx_reset(dev);
	}

	/* reinit driver view of the rx queue */
	set_bufsize(dev);
	nv_init_ring(dev);

	/* setup hardware for loopback */
	writel(NVREG_MISC1_FORCE, base + NvRegMisc1);
	writel(NVREG_PFF_ALWAYS | NVREG_PFF_LOOPBACK, base + NvRegPacketFilterFlags);

	/* reinit nic view of the rx queue */
	writel(np->rx_buf_sz, base + NvRegOffloadConfig);
	setup_hw_rings(dev, NV_SETUP_RX_RING | NV_SETUP_TX_RING);
	writel( ((np->rx_ring_size-1) << NVREG_RINGSZ_RXSHIFT) + ((np->tx_ring_size-1) << NVREG_RINGSZ_TXSHIFT),
		base + NvRegRingSizes);
	pci_push(base);

	/* restart rx engine */
	nv_start_rx(dev);
	nv_start_tx(dev);

	/* setup packet for tx */
	pkt_len = ETH_DATA_LEN;
	tx_skb = dev_alloc_skb(pkt_len);
	if (!tx_skb) {
		printk(KERN_ERR "dev_alloc_skb() failed during loopback test"
			 " of %s\n", dev->name);
		ret = 0;
		goto out;
	}
	test_dma_addr = pci_map_single(np->pci_dev, tx_skb->data,
				       skb_tailroom(tx_skb),
				       PCI_DMA_FROMDEVICE);
	pkt_data = skb_put(tx_skb, pkt_len);
	for (i = 0; i < pkt_len; i++)
		pkt_data[i] = (u8)(i & 0xff);

	if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2) {
		np->tx_ring.orig[0].buf = cpu_to_le32(test_dma_addr);
		np->tx_ring.orig[0].flaglen = cpu_to_le32((pkt_len-1) | np->tx_flags | tx_flags_extra);
	} else {
		np->tx_ring.ex[0].bufhigh = cpu_to_le64(test_dma_addr) >> 32;
		np->tx_ring.ex[0].buflow = cpu_to_le64(test_dma_addr) & 0x0FFFFFFFF;
		np->tx_ring.ex[0].flaglen = cpu_to_le32((pkt_len-1) | np->tx_flags | tx_flags_extra);
	}
	writel(NVREG_TXRXCTL_KICK|np->txrxctl_bits, get_hwbase(dev) + NvRegTxRxControl);
	pci_push(get_hwbase(dev));

	msleep(500);

	/* check for rx of the packet */
	if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2) {
		flags = le32_to_cpu(np->rx_ring.orig[0].flaglen);
		len = nv_descr_getlength(&np->rx_ring.orig[0], np->desc_ver);

	} else {
		flags = le32_to_cpu(np->rx_ring.ex[0].flaglen);
		len = nv_descr_getlength_ex(&np->rx_ring.ex[0], np->desc_ver);
	}

	if (flags & NV_RX_AVAIL) {
		ret = 0;
	} else if (np->desc_ver == DESC_VER_1) {
		if (flags & NV_RX_ERROR)
			ret = 0;
	} else {
		if (flags & NV_RX2_ERROR) {
			ret = 0;
		}
	}

	if (ret) {
		if (len != pkt_len) {
			ret = 0;
			dprintk(KERN_DEBUG "%s: loopback len mismatch %d vs %d\n",
				dev->name, len, pkt_len);
		} else {
			rx_skb = np->rx_skb[0].skb;
			for (i = 0; i < pkt_len; i++) {
				if (rx_skb->data[i] != (u8)(i & 0xff)) {
					ret = 0;
					dprintk(KERN_DEBUG "%s: loopback pattern check failed on byte %d\n",
						dev->name, i);
					break;
				}
			}
		}
	} else {
		dprintk(KERN_DEBUG "%s: loopback - did not receive test packet\n", dev->name);
	}

	pci_unmap_page(np->pci_dev, test_dma_addr,
		       (skb_end_pointer(tx_skb) - tx_skb->data),
		       PCI_DMA_TODEVICE);
	dev_kfree_skb_any(tx_skb);
 out:
	/* stop engines */
	nv_stop_rx(dev);
	nv_stop_tx(dev);
	nv_txrx_reset(dev);
	/* drain rx queue */
	nv_drain_rx(dev);
	nv_drain_tx(dev);

	if (netif_running(dev)) {
		writel(misc1_flags, base + NvRegMisc1);
		writel(filter_flags, base + NvRegPacketFilterFlags);
		nv_enable_irq(dev);
	}

	return ret;
}

static void nv_self_test(struct net_device *dev, struct ethtool_test *test, u64 *buffer)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	int result;
	memset(buffer, 0, nv_self_test_count(dev)*sizeof(u64));

	if (!nv_link_test(dev)) {
		test->flags |= ETH_TEST_FL_FAILED;
		buffer[0] = 1;
	}

	if (test->flags & ETH_TEST_FL_OFFLINE) {
		if (netif_running(dev)) {
			netif_stop_queue(dev);
			netif_poll_disable(dev);
			netif_tx_lock_bh(dev);
			spin_lock_irq(&np->lock);
			nv_disable_hw_interrupts(dev, np->irqmask);
			if (!(np->msi_flags & NV_MSI_X_ENABLED)) {
				writel(NVREG_IRQSTAT_MASK, base + NvRegIrqStatus);
			} else {
				writel(NVREG_IRQSTAT_MASK, base + NvRegMSIXIrqStatus);
			}
			/* stop engines */
			nv_stop_rx(dev);
			nv_stop_tx(dev);
			nv_txrx_reset(dev);
			/* drain rx queue */
			nv_drain_rx(dev);
			nv_drain_tx(dev);
			spin_unlock_irq(&np->lock);
			netif_tx_unlock_bh(dev);
		}

		if (!nv_register_test(dev)) {
			test->flags |= ETH_TEST_FL_FAILED;
			buffer[1] = 1;
		}

		result = nv_interrupt_test(dev);
		if (result != 1) {
			test->flags |= ETH_TEST_FL_FAILED;
			buffer[2] = 1;
		}
		if (result == 0) {
			/* bail out */
			return;
		}

		if (!nv_loopback_test(dev)) {
			test->flags |= ETH_TEST_FL_FAILED;
			buffer[3] = 1;
		}

		if (netif_running(dev)) {
			/* reinit driver view of the rx queue */
			set_bufsize(dev);
			if (nv_init_ring(dev)) {
				if (!np->in_shutdown)
					mod_timer(&np->oom_kick, jiffies + OOM_REFILL);
			}
			/* reinit nic view of the rx queue */
			writel(np->rx_buf_sz, base + NvRegOffloadConfig);
			setup_hw_rings(dev, NV_SETUP_RX_RING | NV_SETUP_TX_RING);
			writel( ((np->rx_ring_size-1) << NVREG_RINGSZ_RXSHIFT) + ((np->tx_ring_size-1) << NVREG_RINGSZ_TXSHIFT),
				base + NvRegRingSizes);
			pci_push(base);
			writel(NVREG_TXRXCTL_KICK|np->txrxctl_bits, get_hwbase(dev) + NvRegTxRxControl);
			pci_push(base);
			/* restart rx engine */
			nv_start_rx(dev);
			nv_start_tx(dev);
			netif_start_queue(dev);
			netif_poll_enable(dev);
			nv_enable_hw_interrupts(dev, np->irqmask);
		}
	}
}

static void nv_get_strings(struct net_device *dev, u32 stringset, u8 *buffer)
{
	switch (stringset) {
	case ETH_SS_STATS:
		memcpy(buffer, &nv_estats_str, nv_get_stats_count(dev)*sizeof(struct nv_ethtool_str));
		break;
	case ETH_SS_TEST:
		memcpy(buffer, &nv_etests_str, nv_self_test_count(dev)*sizeof(struct nv_ethtool_str));
		break;
	}
}

static const struct ethtool_ops ops = {
	.get_drvinfo = nv_get_drvinfo,
	.get_link = ethtool_op_get_link,
	.get_wol = nv_get_wol,
	.set_wol = nv_set_wol,
	.get_settings = nv_get_settings,
	.set_settings = nv_set_settings,
	.get_regs_len = nv_get_regs_len,
	.get_regs = nv_get_regs,
	.nway_reset = nv_nway_reset,
	.get_tso = ethtool_op_get_tso,
	.set_tso = nv_set_tso,
	.get_ringparam = nv_get_ringparam,
	.set_ringparam = nv_set_ringparam,
	.get_pauseparam = nv_get_pauseparam,
	.set_pauseparam = nv_set_pauseparam,
	.get_rx_csum = nv_get_rx_csum,
	.set_rx_csum = nv_set_rx_csum,
	.get_tx_csum = ethtool_op_get_tx_csum,
	.set_tx_csum = nv_set_tx_csum,
	.get_sg = ethtool_op_get_sg,
	.set_sg = nv_set_sg,
	.get_strings = nv_get_strings,
	.get_stats_count = nv_get_stats_count,
	.get_ethtool_stats = nv_get_ethtool_stats,
	.self_test_count = nv_self_test_count,
	.self_test = nv_self_test,
};

static void nv_vlan_rx_register(struct net_device *dev, struct vlan_group *grp)
{
	struct fe_priv *np = get_nvpriv(dev);

	spin_lock_irq(&np->lock);

	/* save vlan group */
	np->vlangrp = grp;

	if (grp) {
		/* enable vlan on MAC */
		np->txrxctl_bits |= NVREG_TXRXCTL_VLANSTRIP | NVREG_TXRXCTL_VLANINS;
	} else {
		/* disable vlan on MAC */
		np->txrxctl_bits &= ~NVREG_TXRXCTL_VLANSTRIP;
		np->txrxctl_bits &= ~NVREG_TXRXCTL_VLANINS;
	}

	writel(np->txrxctl_bits, get_hwbase(dev) + NvRegTxRxControl);

	spin_unlock_irq(&np->lock);
}

/* The mgmt unit and driver use a semaphore to access the phy during init */
static int nv_mgmt_acquire_sema(struct net_device *dev)
{
	u8 __iomem *base = get_hwbase(dev);
	int i;
	u32 tx_ctrl, mgmt_sema;

	for (i = 0; i < 10; i++) {
		mgmt_sema = readl(base + NvRegTransmitterControl) & NVREG_XMITCTL_MGMT_SEMA_MASK;
		if (mgmt_sema == NVREG_XMITCTL_MGMT_SEMA_FREE)
			break;
		msleep(500);
	}

	if (mgmt_sema != NVREG_XMITCTL_MGMT_SEMA_FREE)
		return 0;

	for (i = 0; i < 2; i++) {
		tx_ctrl = readl(base + NvRegTransmitterControl);
		tx_ctrl |= NVREG_XMITCTL_HOST_SEMA_ACQ;
		writel(tx_ctrl, base + NvRegTransmitterControl);

		/* verify that semaphore was acquired */
		tx_ctrl = readl(base + NvRegTransmitterControl);
		if (((tx_ctrl & NVREG_XMITCTL_HOST_SEMA_MASK) == NVREG_XMITCTL_HOST_SEMA_ACQ) &&
		    ((tx_ctrl & NVREG_XMITCTL_MGMT_SEMA_MASK) == NVREG_XMITCTL_MGMT_SEMA_FREE))
			return 1;
		else
			udelay(50);
	}

	return 0;
}

static int nv_open(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);
	int ret = 1;
	int oom, i;

	dprintk(KERN_DEBUG "nv_open: begin\n");

	/* erase previous misconfiguration */
	if (np->driver_data & DEV_HAS_POWER_CNTRL)
		nv_mac_reset(dev);
	writel(NVREG_MCASTADDRA_FORCE, base + NvRegMulticastAddrA);
	writel(0, base + NvRegMulticastAddrB);
	writel(0, base + NvRegMulticastMaskA);
	writel(0, base + NvRegMulticastMaskB);
	writel(0, base + NvRegPacketFilterFlags);

	writel(0, base + NvRegTransmitterControl);
	writel(0, base + NvRegReceiverControl);

	writel(0, base + NvRegAdapterControl);

	if (np->pause_flags & NV_PAUSEFRAME_TX_CAPABLE)
		writel(NVREG_TX_PAUSEFRAME_DISABLE,  base + NvRegTxPauseFrame);

	/* initialize descriptor rings */
	set_bufsize(dev);
	oom = nv_init_ring(dev);

	writel(0, base + NvRegLinkSpeed);
	writel(readl(base + NvRegTransmitPoll) & NVREG_TRANSMITPOLL_MAC_ADDR_REV, base + NvRegTransmitPoll);
	nv_txrx_reset(dev);
	writel(0, base + NvRegUnknownSetupReg6);

	np->in_shutdown = 0;

	/* give hw rings */
	setup_hw_rings(dev, NV_SETUP_RX_RING | NV_SETUP_TX_RING);
	writel( ((np->rx_ring_size-1) << NVREG_RINGSZ_RXSHIFT) + ((np->tx_ring_size-1) << NVREG_RINGSZ_TXSHIFT),
		base + NvRegRingSizes);

	writel(np->linkspeed, base + NvRegLinkSpeed);
	if (np->desc_ver == DESC_VER_1)
		writel(NVREG_TX_WM_DESC1_DEFAULT, base + NvRegTxWatermark);
	else
		writel(NVREG_TX_WM_DESC2_3_DEFAULT, base + NvRegTxWatermark);
	writel(np->txrxctl_bits, base + NvRegTxRxControl);
	writel(np->vlanctl_bits, base + NvRegVlanControl);
	pci_push(base);
	writel(NVREG_TXRXCTL_BIT1|np->txrxctl_bits, base + NvRegTxRxControl);
	reg_delay(dev, NvRegUnknownSetupReg5, NVREG_UNKSETUP5_BIT31, NVREG_UNKSETUP5_BIT31,
			NV_SETUP5_DELAY, NV_SETUP5_DELAYMAX,
			KERN_INFO "open: SetupReg5, Bit 31 remained off\n");

	writel(0, base + NvRegMIIMask);
	writel(NVREG_IRQSTAT_MASK, base + NvRegIrqStatus);
	writel(NVREG_MIISTAT_MASK2, base + NvRegMIIStatus);

	writel(NVREG_MISC1_FORCE | NVREG_MISC1_HD, base + NvRegMisc1);
	writel(readl(base + NvRegTransmitterStatus), base + NvRegTransmitterStatus);
	writel(NVREG_PFF_ALWAYS, base + NvRegPacketFilterFlags);
	writel(np->rx_buf_sz, base + NvRegOffloadConfig);

	writel(readl(base + NvRegReceiverStatus), base + NvRegReceiverStatus);
	get_random_bytes(&i, sizeof(i));
	writel(NVREG_RNDSEED_FORCE | (i&NVREG_RNDSEED_MASK), base + NvRegRandomSeed);
	writel(NVREG_TX_DEFERRAL_DEFAULT, base + NvRegTxDeferral);
	writel(NVREG_RX_DEFERRAL_DEFAULT, base + NvRegRxDeferral);
	if (poll_interval == -1) {
		if (optimization_mode == NV_OPTIMIZATION_MODE_THROUGHPUT)
			writel(NVREG_POLL_DEFAULT_THROUGHPUT, base + NvRegPollingInterval);
		else
			writel(NVREG_POLL_DEFAULT_CPU, base + NvRegPollingInterval);
	}
	else
		writel(poll_interval & 0xFFFF, base + NvRegPollingInterval);
	writel(NVREG_UNKSETUP6_VAL, base + NvRegUnknownSetupReg6);
	writel((np->phyaddr << NVREG_ADAPTCTL_PHYSHIFT)|NVREG_ADAPTCTL_PHYVALID|NVREG_ADAPTCTL_RUNNING,
			base + NvRegAdapterControl);
	writel(NVREG_MIISPEED_BIT8|NVREG_MIIDELAY, base + NvRegMIISpeed);
	writel(NVREG_MII_LINKCHANGE, base + NvRegMIIMask);
	if (np->wolenabled)
		writel(NVREG_WAKEUPFLAGS_ENABLE , base + NvRegWakeUpFlags);

	i = readl(base + NvRegPowerState);
	if ( (i & NVREG_POWERSTATE_POWEREDUP) == 0)
		writel(NVREG_POWERSTATE_POWEREDUP|i, base + NvRegPowerState);

	pci_push(base);
	udelay(10);
	writel(readl(base + NvRegPowerState) | NVREG_POWERSTATE_VALID, base + NvRegPowerState);

	nv_disable_hw_interrupts(dev, np->irqmask);
	pci_push(base);
	writel(NVREG_MIISTAT_MASK2, base + NvRegMIIStatus);
	writel(NVREG_IRQSTAT_MASK, base + NvRegIrqStatus);
	pci_push(base);

	if (nv_request_irq(dev, 0)) {
		goto out_drain;
	}

	/* ask for interrupts */
	nv_enable_hw_interrupts(dev, np->irqmask);

	spin_lock_irq(&np->lock);
	writel(NVREG_MCASTADDRA_FORCE, base + NvRegMulticastAddrA);
	writel(0, base + NvRegMulticastAddrB);
	writel(0, base + NvRegMulticastMaskA);
	writel(0, base + NvRegMulticastMaskB);
	writel(NVREG_PFF_ALWAYS|NVREG_PFF_MYADDR, base + NvRegPacketFilterFlags);
	/* One manual link speed update: Interrupts are enabled, future link
	 * speed changes cause interrupts and are handled by nv_link_irq().
	 */
	{
		u32 miistat;
		miistat = readl(base + NvRegMIIStatus);
		writel(NVREG_MIISTAT_MASK, base + NvRegMIIStatus);
		dprintk(KERN_INFO "startup: got 0x%08x.\n", miistat);
	}
	/* set linkspeed to invalid value, thus force nv_update_linkspeed
	 * to init hw */
	np->linkspeed = 0;
	ret = nv_update_linkspeed(dev);
	nv_start_rx(dev);
	nv_start_tx(dev);
	netif_start_queue(dev);
	netif_poll_enable(dev);

	if (ret) {
		netif_carrier_on(dev);
	} else {
		printk("%s: no link during initialization.\n", dev->name);
		netif_carrier_off(dev);
	}
	if (oom)
		mod_timer(&np->oom_kick, jiffies + OOM_REFILL);

	/* start statistics timer */
	if (np->driver_data & (DEV_HAS_STATISTICS_V1|DEV_HAS_STATISTICS_V2))
		mod_timer(&np->stats_poll, jiffies + STATS_INTERVAL);

	spin_unlock_irq(&np->lock);

	return 0;
out_drain:
	drain_ring(dev);
	return ret;
}

static int nv_close(struct net_device *dev)
{
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base;

	spin_lock_irq(&np->lock);
	np->in_shutdown = 1;
	spin_unlock_irq(&np->lock);
	netif_poll_disable(dev);
	synchronize_irq(dev->irq);

	del_timer_sync(&np->oom_kick);
	del_timer_sync(&np->nic_poll);
	del_timer_sync(&np->stats_poll);

	netif_stop_queue(dev);
	spin_lock_irq(&np->lock);
	nv_stop_tx(dev);
	nv_stop_rx(dev);
	nv_txrx_reset(dev);

	/* disable interrupts on the nic or we will lock up */
	base = get_hwbase(dev);
	nv_disable_hw_interrupts(dev, np->irqmask);
	pci_push(base);
	dprintk(KERN_INFO "%s: Irqmask is zero again\n", dev->name);

	spin_unlock_irq(&np->lock);

	nv_free_irq(dev);

	drain_ring(dev);

	if (np->wolenabled) {
		writel(NVREG_PFF_ALWAYS|NVREG_PFF_MYADDR, base + NvRegPacketFilterFlags);
		nv_start_rx(dev);
	}

	/* FIXME: power down nic */

	return 0;
}

static int __devinit nv_probe(struct pci_dev *pci_dev, const struct pci_device_id *id)
{
	struct net_device *dev;
	struct fe_priv *np;
	unsigned long addr;
	u8 __iomem *base;
	int err, i;
	u32 powerstate, txreg;
	u32 phystate_orig = 0, phystate;
	int phyinitialized = 0;

	dev = alloc_etherdev(sizeof(struct fe_priv));
	err = -ENOMEM;
	if (!dev)
		goto out;

	np = netdev_priv(dev);
	np->pci_dev = pci_dev;
	spin_lock_init(&np->lock);
	SET_MODULE_OWNER(dev);
	SET_NETDEV_DEV(dev, &pci_dev->dev);

	init_timer(&np->oom_kick);
	np->oom_kick.data = (unsigned long) dev;
	np->oom_kick.function = &nv_do_rx_refill;	/* timer handler */
	init_timer(&np->nic_poll);
	np->nic_poll.data = (unsigned long) dev;
	np->nic_poll.function = &nv_do_nic_poll;	/* timer handler */
	init_timer(&np->stats_poll);
	np->stats_poll.data = (unsigned long) dev;
	np->stats_poll.function = &nv_do_stats_poll;	/* timer handler */

	err = pci_enable_device(pci_dev);
	if (err) {
		printk(KERN_INFO "forcedeth: pci_enable_dev failed (%d) for device %s\n",
				err, pci_name(pci_dev));
		goto out_free;
	}

	pci_set_master(pci_dev);

	err = pci_request_regions(pci_dev, DRV_NAME);
	if (err < 0)
		goto out_disable;

	if (id->driver_data & (DEV_HAS_VLAN|DEV_HAS_MSI_X|DEV_HAS_POWER_CNTRL|DEV_HAS_STATISTICS_V2))
		np->register_size = NV_PCI_REGSZ_VER3;
	else if (id->driver_data & DEV_HAS_STATISTICS_V1)
		np->register_size = NV_PCI_REGSZ_VER2;
	else
		np->register_size = NV_PCI_REGSZ_VER1;

	err = -EINVAL;
	addr = 0;
	for (i = 0; i < DEVICE_COUNT_RESOURCE; i++) {
		dprintk(KERN_DEBUG "%s: resource %d start %p len %ld flags 0x%08lx.\n",
				pci_name(pci_dev), i, (void*)pci_resource_start(pci_dev, i),
				pci_resource_len(pci_dev, i),
				pci_resource_flags(pci_dev, i));
		if (pci_resource_flags(pci_dev, i) & IORESOURCE_MEM &&
				pci_resource_len(pci_dev, i) >= np->register_size) {
			addr = pci_resource_start(pci_dev, i);
			break;
		}
	}
	if (i == DEVICE_COUNT_RESOURCE) {
		printk(KERN_INFO "forcedeth: Couldn't find register window for device %s.\n",
					pci_name(pci_dev));
		goto out_relreg;
	}

	/* copy of driver data */
	np->driver_data = id->driver_data;

	/* handle different descriptor versions */
	if (id->driver_data & DEV_HAS_HIGH_DMA) {
		/* packet format 3: supports 40-bit addressing */
		np->desc_ver = DESC_VER_3;
		np->txrxctl_bits = NVREG_TXRXCTL_DESC_3;
		if (dma_64bit) {
			if (pci_set_dma_mask(pci_dev, DMA_39BIT_MASK)) {
				printk(KERN_INFO "forcedeth: 64-bit DMA failed, using 32-bit addressing for device %s.\n",
				       pci_name(pci_dev));
			} else {
				dev->features |= NETIF_F_HIGHDMA;
				printk(KERN_INFO "forcedeth: using HIGHDMA\n");
			}
			if (pci_set_consistent_dma_mask(pci_dev, DMA_39BIT_MASK)) {
				printk(KERN_INFO "forcedeth: 64-bit DMA (consistent) failed, using 32-bit ring buffers for device %s.\n",
				       pci_name(pci_dev));
			}
		}
	} else if (id->driver_data & DEV_HAS_LARGEDESC) {
		/* packet format 2: supports jumbo frames */
		np->desc_ver = DESC_VER_2;
		np->txrxctl_bits = NVREG_TXRXCTL_DESC_2;
	} else {
		/* original packet format */
		np->desc_ver = DESC_VER_1;
		np->txrxctl_bits = NVREG_TXRXCTL_DESC_1;
	}

	np->pkt_limit = NV_PKTLIMIT_1;
	if (id->driver_data & DEV_HAS_LARGEDESC)
		np->pkt_limit = NV_PKTLIMIT_2;

	if (id->driver_data & DEV_HAS_CHECKSUM) {
		np->rx_csum = 1;
		np->txrxctl_bits |= NVREG_TXRXCTL_RXCHECK;
		dev->features |= NETIF_F_HW_CSUM | NETIF_F_SG;
		dev->features |= NETIF_F_TSO;
	}

	np->vlanctl_bits = 0;
	if (id->driver_data & DEV_HAS_VLAN) {
		np->vlanctl_bits = NVREG_VLANCONTROL_ENABLE;
		dev->features |= NETIF_F_HW_VLAN_RX | NETIF_F_HW_VLAN_TX;
		dev->vlan_rx_register = nv_vlan_rx_register;
	}

	np->msi_flags = 0;
	if ((id->driver_data & DEV_HAS_MSI) && msi) {
		np->msi_flags |= NV_MSI_CAPABLE;
	}
	if ((id->driver_data & DEV_HAS_MSI_X) && msix) {
		np->msi_flags |= NV_MSI_X_CAPABLE;
	}

	np->pause_flags = NV_PAUSEFRAME_RX_CAPABLE | NV_PAUSEFRAME_RX_REQ | NV_PAUSEFRAME_AUTONEG;
	if (id->driver_data & DEV_HAS_PAUSEFRAME_TX) {
		np->pause_flags |= NV_PAUSEFRAME_TX_CAPABLE | NV_PAUSEFRAME_TX_REQ;
	}


	err = -ENOMEM;
	np->base = ioremap(addr, np->register_size);
	if (!np->base)
		goto out_relreg;
	dev->base_addr = (unsigned long)np->base;

	dev->irq = pci_dev->irq;

	np->rx_ring_size = RX_RING_DEFAULT;
	np->tx_ring_size = TX_RING_DEFAULT;

	if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2) {
		np->rx_ring.orig = pci_alloc_consistent(pci_dev,
					sizeof(struct ring_desc) * (np->rx_ring_size + np->tx_ring_size),
					&np->ring_addr);
		if (!np->rx_ring.orig)
			goto out_unmap;
		np->tx_ring.orig = &np->rx_ring.orig[np->rx_ring_size];
	} else {
		np->rx_ring.ex = pci_alloc_consistent(pci_dev,
					sizeof(struct ring_desc_ex) * (np->rx_ring_size + np->tx_ring_size),
					&np->ring_addr);
		if (!np->rx_ring.ex)
			goto out_unmap;
		np->tx_ring.ex = &np->rx_ring.ex[np->rx_ring_size];
	}
	np->rx_skb = kcalloc(np->rx_ring_size, sizeof(struct nv_skb_map), GFP_KERNEL);
	np->tx_skb = kcalloc(np->tx_ring_size, sizeof(struct nv_skb_map), GFP_KERNEL);
	if (!np->rx_skb || !np->tx_skb)
		goto out_freering;

	dev->open = nv_open;
	dev->stop = nv_close;
	if (np->desc_ver == DESC_VER_1 || np->desc_ver == DESC_VER_2)
		dev->hard_start_xmit = nv_start_xmit;
	else
		dev->hard_start_xmit = nv_start_xmit_optimized;
	dev->get_stats = nv_get_stats;
	dev->change_mtu = nv_change_mtu;
	dev->set_mac_address = nv_set_mac_address;
	dev->set_multicast_list = nv_set_multicast;
#ifdef CONFIG_NET_POLL_CONTROLLER
	dev->poll_controller = nv_poll_controller;
#endif
	dev->weight = RX_WORK_PER_LOOP;
#ifdef CONFIG_FORCEDETH_NAPI
	dev->poll = nv_napi_poll;
#endif
	SET_ETHTOOL_OPS(dev, &ops);
	dev->tx_timeout = nv_tx_timeout;
	dev->watchdog_timeo = NV_WATCHDOG_TIMEO;

	pci_set_drvdata(pci_dev, dev);

	/* read the mac address */
	base = get_hwbase(dev);
	np->orig_mac[0] = readl(base + NvRegMacAddrA);
	np->orig_mac[1] = readl(base + NvRegMacAddrB);

	/* check the workaround bit for correct mac address order */
	txreg = readl(base + NvRegTransmitPoll);
	if ((txreg & NVREG_TRANSMITPOLL_MAC_ADDR_REV) ||
	    (id->driver_data & DEV_HAS_CORRECT_MACADDR)) {
		/* mac address is already in correct order */
		dev->dev_addr[0] = (np->orig_mac[0] >>  0) & 0xff;
		dev->dev_addr[1] = (np->orig_mac[0] >>  8) & 0xff;
		dev->dev_addr[2] = (np->orig_mac[0] >> 16) & 0xff;
		dev->dev_addr[3] = (np->orig_mac[0] >> 24) & 0xff;
		dev->dev_addr[4] = (np->orig_mac[1] >>  0) & 0xff;
		dev->dev_addr[5] = (np->orig_mac[1] >>  8) & 0xff;
	} else {
		/* need to reverse mac address to correct order */
		dev->dev_addr[0] = (np->orig_mac[1] >>  8) & 0xff;
		dev->dev_addr[1] = (np->orig_mac[1] >>  0) & 0xff;
		dev->dev_addr[2] = (np->orig_mac[0] >> 24) & 0xff;
		dev->dev_addr[3] = (np->orig_mac[0] >> 16) & 0xff;
		dev->dev_addr[4] = (np->orig_mac[0] >>  8) & 0xff;
		dev->dev_addr[5] = (np->orig_mac[0] >>  0) & 0xff;
		/* set permanent address to be correct aswell */
		np->orig_mac[0] = (dev->dev_addr[0] << 0) + (dev->dev_addr[1] << 8) +
			(dev->dev_addr[2] << 16) + (dev->dev_addr[3] << 24);
		np->orig_mac[1] = (dev->dev_addr[4] << 0) + (dev->dev_addr[5] << 8);
		writel(txreg|NVREG_TRANSMITPOLL_MAC_ADDR_REV, base + NvRegTransmitPoll);
	}
	memcpy(dev->perm_addr, dev->dev_addr, dev->addr_len);

	if (!is_valid_ether_addr(dev->perm_addr)) {
		/*
		 * Bad mac address. At least one bios sets the mac address
		 * to 01:23:45:67:89:ab
		 */
		printk(KERN_ERR "%s: Invalid Mac address detected: %02x:%02x:%02x:%02x:%02x:%02x\n",
			pci_name(pci_dev),
			dev->dev_addr[0], dev->dev_addr[1], dev->dev_addr[2],
			dev->dev_addr[3], dev->dev_addr[4], dev->dev_addr[5]);
		printk(KERN_ERR "Please complain to your hardware vendor. Switching to a random MAC.\n");
		dev->dev_addr[0] = 0x00;
		dev->dev_addr[1] = 0x00;
		dev->dev_addr[2] = 0x6c;
		get_random_bytes(&dev->dev_addr[3], 3);
	}

	dprintk(KERN_DEBUG "%s: MAC Address %02x:%02x:%02x:%02x:%02x:%02x\n", pci_name(pci_dev),
			dev->dev_addr[0], dev->dev_addr[1], dev->dev_addr[2],
			dev->dev_addr[3], dev->dev_addr[4], dev->dev_addr[5]);

	/* set mac address */
	nv_copy_mac_to_hw(dev);

	/* disable WOL */
	writel(0, base + NvRegWakeUpFlags);
	np->wolenabled = 0;

	if (id->driver_data & DEV_HAS_POWER_CNTRL) {

		/* take phy and nic out of low power mode */
		powerstate = readl(base + NvRegPowerState2);
		powerstate &= ~NVREG_POWERSTATE2_POWERUP_MASK;
		if ((id->device == PCI_DEVICE_ID_NVIDIA_NVENET_12 ||
		     id->device == PCI_DEVICE_ID_NVIDIA_NVENET_13) &&
		    pci_dev->revision >= 0xA3)
			powerstate |= NVREG_POWERSTATE2_POWERUP_REV_A3;
		writel(powerstate, base + NvRegPowerState2);
	}

	if (np->desc_ver == DESC_VER_1) {
		np->tx_flags = NV_TX_VALID;
	} else {
		np->tx_flags = NV_TX2_VALID;
	}
	if (optimization_mode == NV_OPTIMIZATION_MODE_THROUGHPUT) {
		np->irqmask = NVREG_IRQMASK_THROUGHPUT;
		if (np->msi_flags & NV_MSI_X_CAPABLE) /* set number of vectors */
			np->msi_flags |= 0x0003;
	} else {
		np->irqmask = NVREG_IRQMASK_CPU;
		if (np->msi_flags & NV_MSI_X_CAPABLE) /* set number of vectors */
			np->msi_flags |= 0x0001;
	}

	if (id->driver_data & DEV_NEED_TIMERIRQ)
		np->irqmask |= NVREG_IRQ_TIMER;
	if (id->driver_data & DEV_NEED_LINKTIMER) {
		dprintk(KERN_INFO "%s: link timer on.\n", pci_name(pci_dev));
		np->need_linktimer = 1;
		np->link_timeout = jiffies + LINK_TIMEOUT;
	} else {
		dprintk(KERN_INFO "%s: link timer off.\n", pci_name(pci_dev));
		np->need_linktimer = 0;
	}

	/* clear phy state and temporarily halt phy interrupts */
	writel(0, base + NvRegMIIMask);
	phystate = readl(base + NvRegAdapterControl);
	if (phystate & NVREG_ADAPTCTL_RUNNING) {
		phystate_orig = 1;
		phystate &= ~NVREG_ADAPTCTL_RUNNING;
		writel(phystate, base + NvRegAdapterControl);
	}
	writel(NVREG_MIISTAT_MASK, base + NvRegMIIStatus);

	if (id->driver_data & DEV_HAS_MGMT_UNIT) {
		/* management unit running on the mac? */
		if (readl(base + NvRegTransmitterControl) & NVREG_XMITCTL_SYNC_PHY_INIT) {
			np->mac_in_use = readl(base + NvRegTransmitterControl) & NVREG_XMITCTL_MGMT_ST;
			dprintk(KERN_INFO "%s: mgmt unit is running. mac in use %x.\n", pci_name(pci_dev), np->mac_in_use);
			for (i = 0; i < 5000; i++) {
				msleep(1);
				if (nv_mgmt_acquire_sema(dev)) {
					/* management unit setup the phy already? */
					if ((readl(base + NvRegTransmitterControl) & NVREG_XMITCTL_SYNC_MASK) ==
					    NVREG_XMITCTL_SYNC_PHY_INIT) {
						/* phy is inited by mgmt unit */
						phyinitialized = 1;
						dprintk(KERN_INFO "%s: Phy already initialized by mgmt unit.\n", pci_name(pci_dev));
					} else {
						/* we need to init the phy */
					}
					break;
				}
			}
		}
	}

	/* find a suitable phy */
	for (i = 1; i <= 32; i++) {
		int id1, id2;
		int phyaddr = i & 0x1F;

		spin_lock_irq(&np->lock);
		id1 = mii_rw(dev, phyaddr, MII_PHYSID1, MII_READ);
		spin_unlock_irq(&np->lock);
		if (id1 < 0 || id1 == 0xffff)
			continue;
		spin_lock_irq(&np->lock);
		id2 = mii_rw(dev, phyaddr, MII_PHYSID2, MII_READ);
		spin_unlock_irq(&np->lock);
		if (id2 < 0 || id2 == 0xffff)
			continue;

		np->phy_model = id2 & PHYID2_MODEL_MASK;
		id1 = (id1 & PHYID1_OUI_MASK) << PHYID1_OUI_SHFT;
		id2 = (id2 & PHYID2_OUI_MASK) >> PHYID2_OUI_SHFT;
		dprintk(KERN_DEBUG "%s: open: Found PHY %04x:%04x at address %d.\n",
			pci_name(pci_dev), id1, id2, phyaddr);
		np->phyaddr = phyaddr;
		np->phy_oui = id1 | id2;
		break;
	}
	if (i == 33) {
		printk(KERN_INFO "%s: open: Could not find a valid PHY.\n",
		       pci_name(pci_dev));
		goto out_error;
	}

	if (!phyinitialized) {
		/* reset it */
		phy_init(dev);
	} else {
		/* see if it is a gigabit phy */
		u32 mii_status = mii_rw(dev, np->phyaddr, MII_BMSR, MII_READ);
		if (mii_status & PHY_GIGABIT) {
			np->gigabit = PHY_GIGABIT;
		}
	}

	/* set default link speed settings */
	np->linkspeed = NVREG_LINKSPEED_FORCE|NVREG_LINKSPEED_10;
	np->duplex = 0;
	np->autoneg = 1;

	err = register_netdev(dev);
	if (err) {
		printk(KERN_INFO "forcedeth: unable to register netdev: %d\n", err);
		goto out_error;
	}
	printk(KERN_INFO "%s: forcedeth.c: subsystem: %05x:%04x bound to %s\n",
			dev->name, pci_dev->subsystem_vendor, pci_dev->subsystem_device,
			pci_name(pci_dev));

	return 0;

out_error:
	if (phystate_orig)
		writel(phystate|NVREG_ADAPTCTL_RUNNING, base + NvRegAdapterControl);
	pci_set_drvdata(pci_dev, NULL);
out_freering:
	free_rings(dev);
out_unmap:
	iounmap(get_hwbase(dev));
out_relreg:
	pci_release_regions(pci_dev);
out_disable:
	pci_disable_device(pci_dev);
out_free:
	free_netdev(dev);
out:
	return err;
}

static void __devexit nv_remove(struct pci_dev *pci_dev)
{
	struct net_device *dev = pci_get_drvdata(pci_dev);
	struct fe_priv *np = netdev_priv(dev);
	u8 __iomem *base = get_hwbase(dev);

	unregister_netdev(dev);

	/* special op: write back the misordered MAC address - otherwise
	 * the next nv_probe would see a wrong address.
	 */
	writel(np->orig_mac[0], base + NvRegMacAddrA);
	writel(np->orig_mac[1], base + NvRegMacAddrB);

	/* free all structures */
	free_rings(dev);
	iounmap(get_hwbase(dev));
	pci_release_regions(pci_dev);
	pci_disable_device(pci_dev);
	free_netdev(dev);
	pci_set_drvdata(pci_dev, NULL);
}

#ifdef CONFIG_PM
static int nv_suspend(struct pci_dev *pdev, pm_message_t state)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	struct fe_priv *np = netdev_priv(dev);

	if (!netif_running(dev))
		goto out;

	netif_device_detach(dev);

	// Gross.
	nv_close(dev);

	pci_save_state(pdev);
	pci_enable_wake(pdev, pci_choose_state(pdev, state), np->wolenabled);
	pci_set_power_state(pdev, pci_choose_state(pdev, state));
out:
	return 0;
}

static int nv_resume(struct pci_dev *pdev)
{
	struct net_device *dev = pci_get_drvdata(pdev);
	int rc = 0;

	if (!netif_running(dev))
		goto out;

	netif_device_attach(dev);

	pci_set_power_state(pdev, PCI_D0);
	pci_restore_state(pdev);
	pci_enable_wake(pdev, PCI_D0, 0);

	rc = nv_open(dev);
out:
	return rc;
}
#else
#define nv_suspend NULL
#define nv_resume NULL
#endif /* CONFIG_PM */

static struct pci_device_id pci_tbl[] = {
	{	/* nForce Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_1),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER,
	},
	{	/* nForce2 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_2),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER,
	},
	{	/* nForce3 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_3),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER,
	},
	{	/* nForce3 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_4),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_LARGEDESC|DEV_HAS_CHECKSUM,
	},
	{	/* nForce3 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_5),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_LARGEDESC|DEV_HAS_CHECKSUM,
	},
	{	/* nForce3 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_6),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_LARGEDESC|DEV_HAS_CHECKSUM,
	},
	{	/* nForce3 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_7),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_LARGEDESC|DEV_HAS_CHECKSUM,
	},
	{	/* CK804 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_8),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_LARGEDESC|DEV_HAS_CHECKSUM|DEV_HAS_HIGH_DMA|DEV_HAS_STATISTICS_V1,
	},
	{	/* CK804 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_9),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_LARGEDESC|DEV_HAS_CHECKSUM|DEV_HAS_HIGH_DMA|DEV_HAS_STATISTICS_V1,
	},
	{	/* MCP04 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_10),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_LARGEDESC|DEV_HAS_CHECKSUM|DEV_HAS_HIGH_DMA|DEV_HAS_STATISTICS_V1,
	},
	{	/* MCP04 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_11),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_LARGEDESC|DEV_HAS_CHECKSUM|DEV_HAS_HIGH_DMA|DEV_HAS_STATISTICS_V1,
	},
	{	/* MCP51 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_12),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_STATISTICS_V1,
	},
	{	/* MCP51 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_13),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_STATISTICS_V1,
	},
	{	/* MCP55 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_14),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_LARGEDESC|DEV_HAS_CHECKSUM|DEV_HAS_HIGH_DMA|DEV_HAS_VLAN|DEV_HAS_MSI|DEV_HAS_MSI_X|DEV_HAS_POWER_CNTRL|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT,
	},
	{	/* MCP55 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_15),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_LARGEDESC|DEV_HAS_CHECKSUM|DEV_HAS_HIGH_DMA|DEV_HAS_VLAN|DEV_HAS_MSI|DEV_HAS_MSI_X|DEV_HAS_POWER_CNTRL|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT,
	},
	{	/* MCP61 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_16),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_MSI|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT|DEV_HAS_CORRECT_MACADDR,
	},
	{	/* MCP61 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_17),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_MSI|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT|DEV_HAS_CORRECT_MACADDR,
	},
	{	/* MCP61 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_18),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_MSI|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT|DEV_HAS_CORRECT_MACADDR,
	},
	{	/* MCP61 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_19),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_MSI|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT|DEV_HAS_CORRECT_MACADDR,
	},
	{	/* MCP65 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_20),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_LARGEDESC|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_MSI|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT|DEV_HAS_CORRECT_MACADDR,
	},
	{	/* MCP65 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_21),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_LARGEDESC|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_MSI|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT|DEV_HAS_CORRECT_MACADDR,
	},
	{	/* MCP65 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_22),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_LARGEDESC|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_MSI|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT|DEV_HAS_CORRECT_MACADDR,
	},
	{	/* MCP65 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_23),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_LARGEDESC|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_MSI|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT|DEV_HAS_CORRECT_MACADDR,
	},
	{	/* MCP67 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_24),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_MSI|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT|DEV_HAS_CORRECT_MACADDR,
	},
	{	/* MCP67 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_25),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_MSI|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT|DEV_HAS_CORRECT_MACADDR,
	},
	{	/* MCP67 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_26),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_MSI|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT|DEV_HAS_CORRECT_MACADDR,
	},
	{	/* MCP67 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_27),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_MSI|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT|DEV_HAS_CORRECT_MACADDR,
	},
	{	/* MCP73 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_28),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_MSI|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT|DEV_HAS_CORRECT_MACADDR,
	},
	{	/* MCP73 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_29),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_MSI|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT|DEV_HAS_CORRECT_MACADDR,
	},
	{	/* MCP73 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_30),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_MSI|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT|DEV_HAS_CORRECT_MACADDR,
	},
	{	/* MCP73 Ethernet Controller */
		PCI_DEVICE(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_NVENET_31),
		.driver_data = DEV_NEED_TIMERIRQ|DEV_NEED_LINKTIMER|DEV_HAS_HIGH_DMA|DEV_HAS_POWER_CNTRL|DEV_HAS_MSI|DEV_HAS_PAUSEFRAME_TX|DEV_HAS_STATISTICS_V2|DEV_HAS_TEST_EXTENDED|DEV_HAS_MGMT_UNIT|DEV_HAS_CORRECT_MACADDR,
	},
	{0,},
};

static struct pci_driver driver = {
	.name = "forcedeth",
	.id_table = pci_tbl,
	.probe = nv_probe,
	.remove = __devexit_p(nv_remove),
	.suspend = nv_suspend,
	.resume	= nv_resume,
};

static int __init init_nic(void)
{
	printk(KERN_INFO "forcedeth.c: Reverse Engineered nForce ethernet driver. Version %s.\n", FORCEDETH_VERSION);
	return pci_register_driver(&driver);
}

static void __exit exit_nic(void)
{
	pci_unregister_driver(&driver);
}

module_param(max_interrupt_work, int, 0);
MODULE_PARM_DESC(max_interrupt_work, "forcedeth maximum events handled per interrupt");
module_param(optimization_mode, int, 0);
MODULE_PARM_DESC(optimization_mode, "In throughput mode (0), every tx & rx packet will generate an interrupt. In CPU mode (1), interrupts are controlled by a timer.");
module_param(poll_interval, int, 0);
MODULE_PARM_DESC(poll_interval, "Interval determines how frequent timer interrupt is generated by [(time_in_micro_secs * 100) / (2^10)]. Min is 0 and Max is 65535.");
module_param(msi, int, 0);
MODULE_PARM_DESC(msi, "MSI interrupts are enabled by setting to 1 and disabled by setting to 0.");
module_param(msix, int, 0);
MODULE_PARM_DESC(msix, "MSIX interrupts are enabled by setting to 1 and disabled by setting to 0.");
module_param(dma_64bit, int, 0);
MODULE_PARM_DESC(dma_64bit, "High DMA is enabled by setting to 1 and disabled by setting to 0.");

MODULE_AUTHOR("Manfred Spraul <manfred@colorfullife.com>");
MODULE_DESCRIPTION("Reverse Engineered nForce ethernet driver");
MODULE_LICENSE("GPL");

MODULE_DEVICE_TABLE(pci, pci_tbl);

module_init(init_nic);
module_exit(exit_nic);
