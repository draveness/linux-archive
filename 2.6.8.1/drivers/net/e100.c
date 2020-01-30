/*******************************************************************************

  
  Copyright(c) 1999 - 2004 Intel Corporation. All rights reserved.
  
  This program is free software; you can redistribute it and/or modify it 
  under the terms of the GNU General Public License as published by the Free 
  Software Foundation; either version 2 of the License, or (at your option) 
  any later version.
  
  This program is distributed in the hope that it will be useful, but WITHOUT 
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for 
  more details.
  
  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc., 59 
  Temple Place - Suite 330, Boston, MA  02111-1307, USA.
  
  The full GNU General Public License is included in this distribution in the
  file called LICENSE.
  
  Contact Information:
  Linux NICS <linux.nics@intel.com>
  Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497

*******************************************************************************/

/*
 *	e100.c: Intel(R) PRO/100 ethernet driver
 *
 *	(Re)written 2003 by scott.feldman@intel.com.  Based loosely on
 *	original e100 driver, but better described as a munging of
 *	e100, e1000, eepro100, tg3, 8139cp, and other drivers.
 *
 *	References:
 *		Intel 8255x 10/100 Mbps Ethernet Controller Family,
 *		Open Source Software Developers Manual,
 *		http://sourceforge.net/projects/e1000
 *
 *
 *	                      Theory of Operation
 *
 *	I.   General
 *
 *	The driver supports Intel(R) 10/100 Mbps PCI Fast Ethernet
 *	controller family, which includes the 82557, 82558, 82559, 82550,
 *	82551, and 82562 devices.  82558 and greater controllers
 *	integrate the Intel 82555 PHY.  The controllers are used in
 *	server and client network interface cards, as well as in
 *	LAN-On-Motherboard (LOM), CardBus, MiniPCI, and ICHx
 *	configurations.  8255x supports a 32-bit linear addressing
 *	mode and operates at 33Mhz PCI clock rate.
 *
 *	II.  Driver Operation
 *
 *	Memory-mapped mode is used exclusively to access the device's
 *	shared-memory structure, the Control/Status Registers (CSR). All
 *	setup, configuration, and control of the device, including queuing
 *	of Tx, Rx, and configuration commands is through the CSR.
 *	cmd_lock serializes accesses to the CSR command register.  cb_lock
 *	protects the shared Command Block List (CBL).
 *
 *	8255x is highly MII-compliant and all access to the PHY go
 *	through the Management Data Interface (MDI).  Consequently, the
 *	driver leverages the mii.c library shared with other MII-compliant
 *	devices.
 *
 *	Big- and Little-Endian byte order as well as 32- and 64-bit
 *	archs are supported.  Weak-ordered memory and non-cache-coherent
 *	archs are supported.
 *
 *	III. Transmit
 *
 *	A Tx skb is mapped and hangs off of a TCB.  TCBs are linked
 *	together in a fixed-size ring (CBL) thus forming the flexible mode
 *	memory structure.  A TCB marked with the suspend-bit indicates
 *	the end of the ring.  The last TCB processed suspends the
 *	controller, and the controller can be restarted by issue a CU
 *	resume command to continue from the suspend point, or a CU start
 *	command to start at a given position in the ring.
 *
 *	Non-Tx commands (config, multicast setup, etc) are linked
 *	into the CBL ring along with Tx commands.  The common structure
 *	used for both Tx and non-Tx commands is the Command Block (CB).
 *
 *	cb_to_use is the next CB to use for queuing a command; cb_to_clean
 *	is the next CB to check for completion; cb_to_send is the first
 *	CB to start on in case of a previous failure to resume.  CB clean
 *	up happens in interrupt context in response to a CU interrupt, or
 *	in dev->poll in the case where NAPI is enabled.  cbs_avail keeps
 *	track of number of free CB resources available.
 *
 * 	Hardware padding of short packets to minimum packet size is
 * 	enabled.  82557 pads with 7Eh, while the later controllers pad
 * 	with 00h.
 *
 *	IV.  Recieve
 *
 *	The Receive Frame Area (RFA) comprises a ring of Receive Frame
 *	Descriptors (RFD) + data buffer, thus forming the simplified mode
 *	memory structure.  Rx skbs are allocated to contain both the RFD
 *	and the data buffer, but the RFD is pulled off before the skb is
 *	indicated.  The data buffer is aligned such that encapsulated
 *	protocol headers are u32-aligned.  Since the RFD is part of the
 *	mapped shared memory, and completion status is contained within
 *	the RFD, the RFD must be dma_sync'ed to maintain a consistent
 *	view from software and hardware.
 *
 *	Under typical operation, the  receive unit (RU) is start once,
 *	and the controller happily fills RFDs as frames arrive.  If
 *	replacement RFDs cannot be allocated, or the RU goes non-active,
 *	the RU must be restarted.  Frame arrival generates an interrupt,
 *	and Rx indication and re-allocation happen in the same context,
 *	therefore no locking is required.  If NAPI is enabled, this work
 *	happens in dev->poll.  A software-generated interrupt is gen-
 *	erated from the watchdog to recover from a failed allocation
 *	senario where all Rx resources have been indicated and none re-
 *	placed.
 *
 *	V.   Miscellaneous
 *
 * 	VLAN offloading of tagging, stripping and filtering is not
 * 	supported, but driver will accommodate the extra 4-byte VLAN tag
 * 	for processing by upper layers.  Tx/Rx Checksum offloading is not
 * 	supported.  Tx Scatter/Gather is not supported.  Jumbo Frames is
 * 	not supported (hardware limitation).
 *
 * 	NAPI support is enabled with CONFIG_E100_NAPI.
 *
 * 	MagicPacket(tm) WoL support is enabled/disabled via ethtool.
 *
 * 	Thanks to JC (jchapman@katalix.com) for helping with
 * 	testing/troubleshooting the development driver.
 *
 * 	TODO:
 * 	o several entry points race with dev->close
 * 	o check for tx-no-resources/stop Q races with tx clean/wake Q
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/mii.h>
#include <linux/if_vlan.h>
#include <linux/skbuff.h>
#include <linux/ethtool.h>
#include <linux/string.h>
#include <asm/unaligned.h>


#define DRV_NAME		"e100"
#define DRV_VERSION		"3.0.18"
#define DRV_DESCRIPTION		"Intel(R) PRO/100 Network Driver"
#define DRV_COPYRIGHT		"Copyright(c) 1999-2004 Intel Corporation"
#define PFX			DRV_NAME ": "

#define E100_WATCHDOG_PERIOD	(2 * HZ)
#define E100_NAPI_WEIGHT	16

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_COPYRIGHT);
MODULE_LICENSE("GPL");

static int debug = 3;
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level (0=none,...,16=all)");
#define DPRINTK(nlevel, klevel, fmt, args...) \
	(void)((NETIF_MSG_##nlevel & nic->msg_enable) && \
	printk(KERN_##klevel PFX "%s: %s: " fmt, nic->netdev->name, \
		__FUNCTION__ , ## args))

#define INTEL_8255X_ETHERNET_DEVICE(device_id, ich) {\
	PCI_VENDOR_ID_INTEL, device_id, PCI_ANY_ID, PCI_ANY_ID, \
	PCI_CLASS_NETWORK_ETHERNET << 8, 0xFFFF00, ich }
static struct pci_device_id e100_id_table[] = {
	INTEL_8255X_ETHERNET_DEVICE(0x1029, 0),
	INTEL_8255X_ETHERNET_DEVICE(0x1030, 0),
	INTEL_8255X_ETHERNET_DEVICE(0x1031, 3),
	INTEL_8255X_ETHERNET_DEVICE(0x1032, 3),
	INTEL_8255X_ETHERNET_DEVICE(0x1033, 3),
	INTEL_8255X_ETHERNET_DEVICE(0x1034, 3),
	INTEL_8255X_ETHERNET_DEVICE(0x1038, 3),
	INTEL_8255X_ETHERNET_DEVICE(0x1039, 4),
	INTEL_8255X_ETHERNET_DEVICE(0x103A, 4),
	INTEL_8255X_ETHERNET_DEVICE(0x103B, 4),
	INTEL_8255X_ETHERNET_DEVICE(0x103C, 4),
	INTEL_8255X_ETHERNET_DEVICE(0x103D, 4),
	INTEL_8255X_ETHERNET_DEVICE(0x103E, 4),
	INTEL_8255X_ETHERNET_DEVICE(0x1050, 5),
	INTEL_8255X_ETHERNET_DEVICE(0x1051, 5),
	INTEL_8255X_ETHERNET_DEVICE(0x1052, 5),
	INTEL_8255X_ETHERNET_DEVICE(0x1053, 5),
	INTEL_8255X_ETHERNET_DEVICE(0x1054, 5),
	INTEL_8255X_ETHERNET_DEVICE(0x1055, 5),
	INTEL_8255X_ETHERNET_DEVICE(0x1064, 6),
	INTEL_8255X_ETHERNET_DEVICE(0x1065, 6),
	INTEL_8255X_ETHERNET_DEVICE(0x1066, 6),
	INTEL_8255X_ETHERNET_DEVICE(0x1067, 6),
	INTEL_8255X_ETHERNET_DEVICE(0x1068, 6),
	INTEL_8255X_ETHERNET_DEVICE(0x1069, 6),
	INTEL_8255X_ETHERNET_DEVICE(0x106A, 6),
	INTEL_8255X_ETHERNET_DEVICE(0x106B, 6),
	INTEL_8255X_ETHERNET_DEVICE(0x1059, 0),
	INTEL_8255X_ETHERNET_DEVICE(0x1209, 0),
	INTEL_8255X_ETHERNET_DEVICE(0x1229, 0),
	INTEL_8255X_ETHERNET_DEVICE(0x2449, 2),
	INTEL_8255X_ETHERNET_DEVICE(0x2459, 2),
	INTEL_8255X_ETHERNET_DEVICE(0x245D, 2),
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, e100_id_table);

enum mac {
	mac_82557_D100_A  = 0,
	mac_82557_D100_B  = 1,
	mac_82557_D100_C  = 2,
	mac_82558_D101_A4 = 4,
	mac_82558_D101_B0 = 5,
	mac_82559_D101M   = 8,
	mac_82559_D101S   = 9,
	mac_82550_D102    = 12,
	mac_82550_D102_C  = 13,
	mac_82551_E       = 14,
	mac_82551_F       = 15,
	mac_82551_10      = 16,
	mac_unknown       = 0xFF,
};

enum phy {
	phy_100a     = 0x000003E0,
	phy_100c     = 0x035002A8,
	phy_82555_tx = 0x015002A8,
	phy_nsc_tx   = 0x5C002000,
	phy_82562_et = 0x033002A8,
	phy_82562_em = 0x032002A8,
	phy_82562_eh = 0x017002A8,
	phy_unknown  = 0xFFFFFFFF,
};

/* CSR (Control/Status Registers) */
struct csr {
	struct {
		u8 status;
		u8 stat_ack;
		u8 cmd_lo;
		u8 cmd_hi;
		u32 gen_ptr;
	} scb;
	u32 port;
	u16 flash_ctrl;
	u8 eeprom_ctrl_lo;
	u8 eeprom_ctrl_hi;
	u32 mdi_ctrl;
	u32 rx_dma_count;
};

enum scb_status {
	rus_ready        = 0x10,
	rus_mask         = 0x3C,
};

enum scb_stat_ack {
	stat_ack_not_ours    = 0x00,
	stat_ack_sw_gen      = 0x04,
	stat_ack_rnr         = 0x10,
	stat_ack_cu_idle     = 0x20,
	stat_ack_frame_rx    = 0x40,
	stat_ack_cu_cmd_done = 0x80,
	stat_ack_not_present = 0xFF,
	stat_ack_rx = (stat_ack_sw_gen | stat_ack_rnr | stat_ack_frame_rx),
	stat_ack_tx = (stat_ack_cu_idle | stat_ack_cu_cmd_done),
};

enum scb_cmd_hi {
	irq_mask_none = 0x00,
	irq_mask_all  = 0x01,
	irq_sw_gen    = 0x02,
};

enum scb_cmd_lo {
	cuc_nop        = 0x00,
	ruc_start      = 0x01,
	ruc_load_base  = 0x06,
	cuc_start      = 0x10,
	cuc_resume     = 0x20,
	cuc_dump_addr  = 0x40,
	cuc_dump_stats = 0x50,
	cuc_load_base  = 0x60,
	cuc_dump_reset = 0x70,
};

enum cuc_dump {
	cuc_dump_complete       = 0x0000A005,
	cuc_dump_reset_complete = 0x0000A007,
};
		
enum port {
	software_reset  = 0x0000,
	selftest        = 0x0001,
	selective_reset = 0x0002,
};

enum eeprom_ctrl_lo {
	eesk = 0x01,
	eecs = 0x02,
	eedi = 0x04,
	eedo = 0x08,
};

enum mdi_ctrl {
	mdi_write = 0x04000000,
	mdi_read  = 0x08000000,
	mdi_ready = 0x10000000,
};

enum eeprom_op {
	op_write = 0x05,
	op_read  = 0x06,
	op_ewds  = 0x10,
	op_ewen  = 0x13,
};

enum eeprom_offsets {
	eeprom_id         = 0x0A,
	eeprom_config_asf = 0x0D,
	eeprom_smbus_addr = 0x90,
};

enum eeprom_id {
	eeprom_id_wol = 0x0020,
};

enum eeprom_config_asf {
	eeprom_asf = 0x8000,
	eeprom_gcl = 0x4000,
};

enum cb_status {
	cb_complete = 0x8000,
	cb_ok       = 0x2000,
};

enum cb_command {
	cb_iaaddr = 0x0001,
	cb_config = 0x0002,
	cb_multi  = 0x0003,
	cb_tx     = 0x0004,
	cb_dump   = 0x0006,
	cb_tx_sf  = 0x0008,
	cb_cid    = 0x1f00,
	cb_i      = 0x2000,
	cb_s      = 0x4000,
	cb_el     = 0x8000,
};

struct rfd {
	u16 status;
	u16 command;
	u32 link;
	u32 rbd;
	u16 actual_size;
	u16 size;
};

struct rx {
	struct rx *next, *prev;
	struct sk_buff *skb;
	dma_addr_t dma_addr;
};

#if defined(__BIG_ENDIAN_BITFIELD)
#define X(a,b)	b,a
#else
#define X(a,b)	a,b
#endif
struct config {
/*0*/	u8 X(byte_count:6, pad0:2);
/*1*/	u8 X(X(rx_fifo_limit:4, tx_fifo_limit:3), pad1:1);
/*2*/	u8 adaptive_ifs;
/*3*/	u8 X(X(X(X(mwi_enable:1, type_enable:1), read_align_enable:1),
	   term_write_cache_line:1), pad3:4);
/*4*/	u8 X(rx_dma_max_count:7, pad4:1);
/*5*/	u8 X(tx_dma_max_count:7, dma_max_count_enable:1);
/*6*/	u8 X(X(X(X(X(X(X(late_scb_update:1, direct_rx_dma:1),
	   tno_intr:1), cna_intr:1), standard_tcb:1), standard_stat_counter:1),
	   rx_discard_overruns:1), rx_save_bad_frames:1);
/*7*/	u8 X(X(X(X(X(rx_discard_short_frames:1, tx_underrun_retry:2),
	   pad7:2), rx_extended_rfd:1), tx_two_frames_in_fifo:1),
	   tx_dynamic_tbd:1);
/*8*/	u8 X(X(mii_mode:1, pad8:6), csma_disabled:1);
/*9*/	u8 X(X(X(X(X(rx_tcpudp_checksum:1, pad9:3), vlan_arp_tco:1),
	   link_status_wake:1), arp_wake:1), mcmatch_wake:1);
/*10*/	u8 X(X(X(pad10:3, no_source_addr_insertion:1), preamble_length:2),
	   loopback:2);
/*11*/	u8 X(linear_priority:3, pad11:5);
/*12*/	u8 X(X(linear_priority_mode:1, pad12:3), ifs:4);
/*13*/	u8 ip_addr_lo;
/*14*/	u8 ip_addr_hi;
/*15*/	u8 X(X(X(X(X(X(X(promiscuous_mode:1, broadcast_disabled:1),
	   wait_after_win:1), pad15_1:1), ignore_ul_bit:1), crc_16_bit:1),
	   pad15_2:1), crs_or_cdt:1);
/*16*/	u8 fc_delay_lo;
/*17*/	u8 fc_delay_hi;
/*18*/	u8 X(X(X(X(X(rx_stripping:1, tx_padding:1), rx_crc_transfer:1),
	   rx_long_ok:1), fc_priority_threshold:3), pad18:1);
/*19*/	u8 X(X(X(X(X(X(X(addr_wake:1, magic_packet_disable:1),
	   fc_disable:1), fc_restop:1), fc_restart:1), fc_reject:1),
	   full_duplex_force:1), full_duplex_pin:1);
/*20*/	u8 X(X(X(pad20_1:5, fc_priority_location:1), multi_ia:1), pad20_2:1);
/*21*/	u8 X(X(pad21_1:3, multicast_all:1), pad21_2:4);
/*22*/	u8 X(X(rx_d102_mode:1, rx_vlan_drop:1), pad22:6);
	u8 pad_d102[9];
};

#define E100_MAX_MULTICAST_ADDRS	64
struct multi {
	u16 count;
	u8 addr[E100_MAX_MULTICAST_ADDRS * ETH_ALEN + 2/*pad*/];
};

/* Important: keep total struct u32-aligned */
struct cb {
	u16 status;
	u16 command;
	u32 link;
	union {
		u8 iaaddr[ETH_ALEN];
		struct config config;
		struct multi multi;
		struct {
			u32 tbd_array;
			u16 tcb_byte_count;
			u8 threshold;
			u8 tbd_count;
			struct {
				u32 buf_addr;
				u16 size;
				u16 eol;
			} tbd;
		} tcb;
		u32 dump_buffer_addr;
	} u;
	struct cb *next, *prev;
	dma_addr_t dma_addr;
	struct sk_buff *skb;
};

enum loopback {
	lb_none = 0, lb_mac = 1, lb_phy = 3,
};

struct stats {
	u32 tx_good_frames, tx_max_collisions, tx_late_collisions,
		tx_underruns, tx_lost_crs, tx_deferred, tx_single_collisions,
		tx_multiple_collisions, tx_total_collisions;
	u32 rx_good_frames, rx_crc_errors, rx_alignment_errors,
		rx_resource_errors, rx_overrun_errors, rx_cdt_errors,
		rx_short_frame_errors;
	u32 fc_xmt_pause, fc_rcv_pause, fc_rcv_unsupported;
	u16 xmt_tco_frames, rcv_tco_frames;
	u32 complete;
};

struct mem {
	struct {
		u32 signature;
		u32 result;
	} selftest;
	struct stats stats;
	u8 dump_buf[596];
};

struct param_range {
	u32 min;
	u32 max;
	u32 count;
};

struct params {
	struct param_range rfds;
	struct param_range cbs;
};

struct nic {
	/* Begin: frequently used values: keep adjacent for cache effect */
	u32 msg_enable				____cacheline_aligned;
	struct net_device *netdev;
	struct pci_dev *pdev;

	struct rx *rxs				____cacheline_aligned;
	struct rx *rx_to_use;
	struct rx *rx_to_clean;
	struct rfd blank_rfd;
	int ru_running;

	spinlock_t cb_lock			____cacheline_aligned;
	spinlock_t cmd_lock;
	struct csr *csr;
	enum scb_cmd_lo cuc_cmd;
	unsigned int cbs_avail;
	struct cb *cbs;
	struct cb *cb_to_use;
	struct cb *cb_to_send;
	struct cb *cb_to_clean;
	u16 tx_command;
	/* End: frequently used values: keep adjacent for cache effect */

	enum {
		ich                = (1 << 0),
		promiscuous        = (1 << 1),
		multicast_all      = (1 << 2),
		wol_magic          = (1 << 3),
		ich_10h_workaround = (1 << 4),
	} flags					____cacheline_aligned;

	enum mac mac;
	enum phy phy;
	struct params params;
	struct net_device_stats net_stats;
	struct timer_list watchdog;
	struct timer_list blink_timer;
	struct mii_if_info mii;
	enum loopback loopback;

	struct mem *mem;
	dma_addr_t dma_addr;

	dma_addr_t cbs_dma_addr;
	u8 adaptive_ifs;
	u8 tx_threshold;
	u32 tx_frames;
	u32 tx_collisions;
	u32 tx_deferred;
	u32 tx_single_collisions;
	u32 tx_multiple_collisions;
	u32 tx_fc_pause;
	u32 tx_tco_frames;

	u32 rx_fc_pause;
	u32 rx_fc_unsupported;
	u32 rx_tco_frames;

	u8 rev_id;
	u16 leds;
	u16 eeprom_wc;
	u16 eeprom[256];
	u32 pm_state[16];
};

static inline void e100_write_flush(struct nic *nic)
{
	/* Flush previous PCI writes through intermediate bridges
	 * by doing a benign read */
	(void)readb(&nic->csr->scb.status);
}

static inline void e100_enable_irq(struct nic *nic)
{
	writeb(irq_mask_none, &nic->csr->scb.cmd_hi);
	e100_write_flush(nic);
}

static inline void e100_disable_irq(struct nic *nic)
{
	writeb(irq_mask_all, &nic->csr->scb.cmd_hi);
	e100_write_flush(nic);
}

static void e100_hw_reset(struct nic *nic)
{
	/* Put CU and RU into idle with a selective reset to get
	 * device off of PCI bus */
	writel(selective_reset, &nic->csr->port);
	e100_write_flush(nic); udelay(20);

	/* Now fully reset device */
	writel(software_reset, &nic->csr->port);
	e100_write_flush(nic); udelay(20);

	/* TCO workaround - 82559 and greater */
	if(nic->mac >= mac_82559_D101M) {
		/* Issue a redundant CU load base without setting
		 * general pointer, and without waiting for scb to
		 * clear.  This gets us into post-driver.  Finally,
		 * wait 20 msec for reset to take effect. */
		writeb(cuc_load_base, &nic->csr->scb.cmd_lo);
		mdelay(20);
	}

	/* Mask off our interrupt line - it's unmasked after reset */
	e100_disable_irq(nic);
}

static int e100_self_test(struct nic *nic)
{
	u32 dma_addr = nic->dma_addr + offsetof(struct mem, selftest);

	/* Passing the self-test is a pretty good indication
	 * that the device can DMA to/from host memory */

	nic->mem->selftest.signature = 0;
	nic->mem->selftest.result = 0xFFFFFFFF;

	writel(selftest | dma_addr, &nic->csr->port);
	e100_write_flush(nic);
	/* Wait 10 msec for self-test to complete */
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(HZ / 100 + 1);

	/* Interrupts are enabled after self-test */
	e100_disable_irq(nic);

	/* Check results of self-test */
	if(nic->mem->selftest.result != 0) {
		DPRINTK(HW, ERR, "Self-test failed: result=0x%08X\n",
			nic->mem->selftest.result);
		return -ETIMEDOUT;
	}
	if(nic->mem->selftest.signature == 0) {
		DPRINTK(HW, ERR, "Self-test failed: timed out\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static void e100_eeprom_write(struct nic *nic, u16 addr_len, u16 addr, u16 data)
{
	u32 cmd_addr_data[3];
	u8 ctrl;
	int i, j;

	/* Three cmds: write/erase enable, write data, write/erase disable */
	cmd_addr_data[0] = op_ewen << (addr_len - 2);
	cmd_addr_data[1] = (((op_write << addr_len) | addr) << 16) |
		cpu_to_le16(data);
	cmd_addr_data[2] = op_ewds << (addr_len - 2);

	/* Bit-bang cmds to write word to eeprom */
	for(j = 0; j < 3; j++) {

		/* Chip select */
		writeb(eecs | eesk, &nic->csr->eeprom_ctrl_lo);
		e100_write_flush(nic); udelay(4);

		for(i = 31; i >= 0; i--) {
			ctrl = (cmd_addr_data[j] & (1 << i)) ?
				eecs | eedi : eecs;
			writeb(ctrl, &nic->csr->eeprom_ctrl_lo);
			e100_write_flush(nic); udelay(4);

			writeb(ctrl | eesk, &nic->csr->eeprom_ctrl_lo);
			e100_write_flush(nic); udelay(4);
		}
		/* Wait 10 msec for cmd to complete */
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ / 100 + 1);

		/* Chip deselect */
		writeb(0, &nic->csr->eeprom_ctrl_lo);
		e100_write_flush(nic); udelay(4);
	}
};

/* General technique stolen from the eepro100 driver - very clever */
static u16 e100_eeprom_read(struct nic *nic, u16 *addr_len, u16 addr)
{
	u32 cmd_addr_data;
	u16 data = 0;
	u8 ctrl;
	int i;

	cmd_addr_data = ((op_read << *addr_len) | addr) << 16;

	/* Chip select */
	writeb(eecs | eesk, &nic->csr->eeprom_ctrl_lo);
	e100_write_flush(nic); udelay(4);

	/* Bit-bang to read word from eeprom */
	for(i = 31; i >= 0; i--) {
		ctrl = (cmd_addr_data & (1 << i)) ? eecs | eedi : eecs;
		writeb(ctrl, &nic->csr->eeprom_ctrl_lo);
		e100_write_flush(nic); udelay(4);
		
		writeb(ctrl | eesk, &nic->csr->eeprom_ctrl_lo);
		e100_write_flush(nic); udelay(4);
		
		/* Eeprom drives a dummy zero to EEDO after receiving
		 * complete address.  Use this to adjust addr_len. */
		ctrl = readb(&nic->csr->eeprom_ctrl_lo);
		if(!(ctrl & eedo) && i > 16) {
			*addr_len -= (i - 16);
			i = 17;
		}
		
		data = (data << 1) | (ctrl & eedo ? 1 : 0);
	}

	/* Chip deselect */
	writeb(0, &nic->csr->eeprom_ctrl_lo);
	e100_write_flush(nic); udelay(4);

	return le16_to_cpu(data);
};

/* Load entire EEPROM image into driver cache and validate checksum */
static int e100_eeprom_load(struct nic *nic)
{
	u16 addr, addr_len = 8, checksum = 0;

	/* Try reading with an 8-bit addr len to discover actual addr len */
	e100_eeprom_read(nic, &addr_len, 0);
	nic->eeprom_wc = 1 << addr_len;

	for(addr = 0; addr < nic->eeprom_wc; addr++) {
		nic->eeprom[addr] = e100_eeprom_read(nic, &addr_len, addr);
		if(addr < nic->eeprom_wc - 1)
			checksum += cpu_to_le16(nic->eeprom[addr]);
	}

	/* The checksum, stored in the last word, is calculated such that
	 * the sum of words should be 0xBABA */
	checksum = le16_to_cpu(0xBABA - checksum);
	if(checksum != nic->eeprom[nic->eeprom_wc - 1]) {
		DPRINTK(PROBE, ERR, "EEPROM corrupted\n");
		return -EAGAIN;
	}

	return 0;
}

/* Save (portion of) driver EEPROM cache to device and update checksum */
static int e100_eeprom_save(struct nic *nic, u16 start, u16 count)
{
	u16 addr, addr_len = 8, checksum = 0;

	/* Try reading with an 8-bit addr len to discover actual addr len */
	e100_eeprom_read(nic, &addr_len, 0);
	nic->eeprom_wc = 1 << addr_len;

	if(start + count >= nic->eeprom_wc)
		return -EINVAL;

	for(addr = start; addr < start + count; addr++)
		e100_eeprom_write(nic, addr_len, addr, nic->eeprom[addr]);

	/* The checksum, stored in the last word, is calculated such that
	 * the sum of words should be 0xBABA */
	for(addr = 0; addr < nic->eeprom_wc - 1; addr++)
		checksum += cpu_to_le16(nic->eeprom[addr]);
	nic->eeprom[nic->eeprom_wc - 1] = le16_to_cpu(0xBABA - checksum);
	e100_eeprom_write(nic, addr_len, nic->eeprom_wc - 1,
		nic->eeprom[nic->eeprom_wc - 1]);

	return 0;
}

#define E100_WAIT_SCB_TIMEOUT 40
static inline int e100_exec_cmd(struct nic *nic, u8 cmd, dma_addr_t dma_addr)
{
	unsigned long flags;
	unsigned int i;
	int err = 0;

	spin_lock_irqsave(&nic->cmd_lock, flags);

	/* Previous command is accepted when SCB clears */
	for(i = 0; i < E100_WAIT_SCB_TIMEOUT; i++) {
		if(likely(!readb(&nic->csr->scb.cmd_lo)))
			break;
		cpu_relax();
		if(unlikely(i > (E100_WAIT_SCB_TIMEOUT >> 1)))
			udelay(5);
	}
	if(unlikely(i == E100_WAIT_SCB_TIMEOUT)) {
		err = -EAGAIN;
		goto err_unlock;
	}

	if(unlikely(cmd != cuc_resume))
		writel(dma_addr, &nic->csr->scb.gen_ptr);
	writeb(cmd, &nic->csr->scb.cmd_lo);

err_unlock:
	spin_unlock_irqrestore(&nic->cmd_lock, flags);

	return err;
}

static inline int e100_exec_cb(struct nic *nic, struct sk_buff *skb,
	void (*cb_prepare)(struct nic *, struct cb *, struct sk_buff *))
{
	struct cb *cb;
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&nic->cb_lock, flags);

	if(unlikely(!nic->cbs_avail)) {
		err = -ENOMEM;
		goto err_unlock;
	}

	cb = nic->cb_to_use;
	nic->cb_to_use = cb->next;
	nic->cbs_avail--;
	cb->skb = skb;

	if(unlikely(!nic->cbs_avail))
		err = -ENOSPC;

	cb_prepare(nic, cb, skb);

	/* Order is important otherwise we'll be in a race with h/w:
	 * set S-bit in current first, then clear S-bit in previous. */
	cb->command |= cpu_to_le16(cb_s);
	wmb();
	cb->prev->command &= cpu_to_le16(~cb_s);

	while(nic->cb_to_send != nic->cb_to_use) {
		if(unlikely(e100_exec_cmd(nic, nic->cuc_cmd,
			nic->cb_to_send->dma_addr))) {
			/* Ok, here's where things get sticky.  It's
			 * possible that we can't schedule the command
			 * because the controller is too busy, so
			 * let's just queue the command and try again
			 * when another command is scheduled. */
			break;
		} else {
			nic->cuc_cmd = cuc_resume;
			nic->cb_to_send = nic->cb_to_send->next;
		}
	}

err_unlock:
	spin_unlock_irqrestore(&nic->cb_lock, flags);

	return err;
}

static u16 mdio_ctrl(struct nic *nic, u32 addr, u32 dir, u32 reg, u16 data)
{
	u32 data_out = 0;
	unsigned int i;

	writel((reg << 16) | (addr << 21) | dir | data, &nic->csr->mdi_ctrl);

	for(i = 0; i < 100; i++) {
		udelay(20);
		if((data_out = readl(&nic->csr->mdi_ctrl)) & mdi_ready)
			break;
	}

	DPRINTK(HW, DEBUG,
		"%s:addr=%d, reg=%d, data_in=0x%04X, data_out=0x%04X\n",
		dir == mdi_read ? "READ" : "WRITE", addr, reg, data, data_out);
	return (u16)data_out;
}

static int mdio_read(struct net_device *netdev, int addr, int reg)
{
	return mdio_ctrl(netdev_priv(netdev), addr, mdi_read, reg, 0);
}

static void mdio_write(struct net_device *netdev, int addr, int reg, int data)
{
	mdio_ctrl(netdev_priv(netdev), addr, mdi_write, reg, data);
}

static void e100_get_defaults(struct nic *nic)
{
	struct param_range rfds = { .min = 64, .max = 256, .count = 64 };
	struct param_range cbs  = { .min = 64, .max = 256, .count = 64 };

	pci_read_config_byte(nic->pdev, PCI_REVISION_ID, &nic->rev_id);
	/* MAC type is encoded as rev ID; exception: ICH is treated as 82559 */
	nic->mac = (nic->flags & ich) ? mac_82559_D101M : nic->rev_id;
	if(nic->mac == mac_unknown)
		nic->mac = mac_82557_D100_A;

	nic->params.rfds = rfds;
	nic->params.cbs = cbs;

	/* Quadwords to DMA into FIFO before starting frame transmit */
	nic->tx_threshold = 0xE0;

	nic->tx_command = cpu_to_le16(cb_tx | cb_i | cb_tx_sf |
		((nic->mac >= mac_82558_D101_A4) ? cb_cid : 0));

	/* Template for a freshly allocated RFD */
	nic->blank_rfd.command = cpu_to_le16(cb_el);
	nic->blank_rfd.rbd = 0xFFFFFFFF;
	nic->blank_rfd.size = cpu_to_le16(VLAN_ETH_FRAME_LEN);

	/* MII setup */
	nic->mii.phy_id_mask = 0x1F;
	nic->mii.reg_num_mask = 0x1F;
	nic->mii.dev = nic->netdev;
	nic->mii.mdio_read = mdio_read;
	nic->mii.mdio_write = mdio_write;
}

static void e100_configure(struct nic *nic, struct cb *cb, struct sk_buff *skb)
{
	struct config *config = &cb->u.config;
	u8 *c = (u8 *)config;

	cb->command = cpu_to_le16(cb_config);

	memset(config, 0, sizeof(struct config));

	config->byte_count = 0x16;		/* bytes in this struct */
	config->rx_fifo_limit = 0x8;		/* bytes in FIFO before DMA */
	config->direct_rx_dma = 0x1;		/* reserved */
	config->standard_tcb = 0x1;		/* 1=standard, 0=extended */
	config->standard_stat_counter = 0x1;	/* 1=standard, 0=extended */
	config->rx_discard_short_frames = 0x1;	/* 1=discard, 0=pass */
	config->tx_underrun_retry = 0x3;	/* # of underrun retries */
	config->mii_mode = 0x1;			/* 1=MII mode, 0=503 mode */
	config->pad10 = 0x6;
	config->no_source_addr_insertion = 0x1;	/* 1=no, 0=yes */
	config->preamble_length = 0x2;		/* 0=1, 1=3, 2=7, 3=15 bytes */
	config->ifs = 0x6;			/* x16 = inter frame spacing */
	config->ip_addr_hi = 0xF2;		/* ARP IP filter - not used */
	config->pad15_1 = 0x1;
	config->pad15_2 = 0x1;
	config->crs_or_cdt = 0x0;		/* 0=CRS only, 1=CRS or CDT */
	config->fc_delay_hi = 0x40;		/* time delay for fc frame */
	config->tx_padding = 0x1;		/* 1=pad short frames */
	config->fc_priority_threshold = 0x7;	/* 7=priority fc disabled */
	config->pad18 = 0x1;
	config->full_duplex_pin = 0x1;		/* 1=examine FDX# pin */
	config->pad20_1 = 0x1F;
	config->fc_priority_location = 0x1;	/* 1=byte#31, 0=byte#19 */
	config->pad21_1 = 0x5;

	config->adaptive_ifs = nic->adaptive_ifs;
	config->loopback = nic->loopback;

	if(nic->mii.force_media && nic->mii.full_duplex)
		config->full_duplex_force = 0x1;	/* 1=force, 0=auto */

	if(nic->flags & promiscuous || nic->loopback) {
		config->rx_save_bad_frames = 0x1;	/* 1=save, 0=discard */
		config->rx_discard_short_frames = 0x0;	/* 1=discard, 0=save */
		config->promiscuous_mode = 0x1;		/* 1=on, 0=off */
	}

	if(nic->flags & multicast_all)
		config->multicast_all = 0x1;		/* 1=accept, 0=no */

	if(!(nic->flags & wol_magic))
		config->magic_packet_disable = 0x1;	/* 1=off, 0=on */

	if(nic->mac >= mac_82558_D101_A4) {
		config->fc_disable = 0x1;	/* 1=Tx fc off, 0=Tx fc on */
		config->mwi_enable = 0x1;	/* 1=enable, 0=disable */
		config->standard_tcb = 0x0;	/* 1=standard, 0=extended */
		config->rx_long_ok = 0x1;	/* 1=VLANs ok, 0=standard */
		if(nic->mac >= mac_82559_D101M)
			config->tno_intr = 0x1;		/* TCO stats enable */
		else
			config->standard_stat_counter = 0x0;
	}

	DPRINTK(HW, DEBUG, "[00-07]=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\n",
		c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
	DPRINTK(HW, DEBUG, "[08-15]=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\n",
		c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
	DPRINTK(HW, DEBUG, "[16-23]=%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\n",
		c[16], c[17], c[18], c[19], c[20], c[21], c[22], c[23]);
}

static void e100_setup_iaaddr(struct nic *nic, struct cb *cb,
	struct sk_buff *skb)
{
	cb->command = cpu_to_le16(cb_iaaddr);
	memcpy(cb->u.iaaddr, nic->netdev->dev_addr, ETH_ALEN);
}

static void e100_dump(struct nic *nic, struct cb *cb, struct sk_buff *skb)
{
	cb->command = cpu_to_le16(cb_dump);
	cb->u.dump_buffer_addr = cpu_to_le32(nic->dma_addr +
		offsetof(struct mem, dump_buf));
}

#define NCONFIG_AUTO_SWITCH	0x0080
#define MII_NSC_CONG		MII_RESV1
#define NSC_CONG_ENABLE		0x0100
#define NSC_CONG_TXREADY	0x0400
#define ADVERTISE_FC_SUPPORTED	0x0400
static int e100_phy_init(struct nic *nic)
{
	struct net_device *netdev = nic->netdev;
	u32 addr;
	u16 bmcr, stat, id_lo, id_hi, cong;

	/* Discover phy addr by searching addrs in order {1,0,2,..., 31} */
	for(addr = 0; addr < 32; addr++) {
		nic->mii.phy_id = (addr == 0) ? 1 : (addr == 1) ? 0 : addr;
		bmcr = mdio_read(netdev, nic->mii.phy_id, MII_BMCR);
		stat = mdio_read(netdev, nic->mii.phy_id, MII_BMSR);
		stat = mdio_read(netdev, nic->mii.phy_id, MII_BMSR);
		if(!((bmcr == 0xFFFF) || ((stat == 0) && (bmcr == 0))))
			break;
	}
	DPRINTK(HW, DEBUG, "phy_addr = %d\n", nic->mii.phy_id);
	if(addr == 32)
		return -EAGAIN;

	/* Selected the phy and isolate the rest */
	for(addr = 0; addr < 32; addr++) {
		if(addr != nic->mii.phy_id) {
			mdio_write(netdev, addr, MII_BMCR, BMCR_ISOLATE);
		} else {
			bmcr = mdio_read(netdev, addr, MII_BMCR);
			mdio_write(netdev, addr, MII_BMCR,
				bmcr & ~BMCR_ISOLATE);
		}
	}

	/* Get phy ID */
	id_lo = mdio_read(netdev, nic->mii.phy_id, MII_PHYSID1);
	id_hi = mdio_read(netdev, nic->mii.phy_id, MII_PHYSID2);
	nic->phy = (u32)id_hi << 16 | (u32)id_lo;
	DPRINTK(HW, DEBUG, "phy ID = 0x%08X\n", nic->phy);

	/* Handle National tx phys */
#define NCS_PHY_MODEL_MASK	0xFFF0FFFF
	if((nic->phy & NCS_PHY_MODEL_MASK) == phy_nsc_tx) {
		/* Disable congestion control */
		cong = mdio_read(netdev, nic->mii.phy_id, MII_NSC_CONG);
		cong |= NSC_CONG_TXREADY;
		cong &= ~NSC_CONG_ENABLE;
		mdio_write(netdev, nic->mii.phy_id, MII_NSC_CONG, cong);
	}

	if(nic->mac >= mac_82550_D102)
		/* enable/disable MDI/MDI-X auto-switching */
		mdio_write(netdev, nic->mii.phy_id, MII_NCONFIG,
			nic->mii.force_media ? 0 : NCONFIG_AUTO_SWITCH);

	return 0;
}

static int e100_hw_init(struct nic *nic)
{
	int err;

	e100_hw_reset(nic);

	DPRINTK(HW, ERR, "e100_hw_init\n");
	if(!in_interrupt() && (err = e100_self_test(nic)))
		return err;

	if((err = e100_phy_init(nic)))
		return err;
	if((err = e100_exec_cmd(nic, cuc_load_base, 0)))
		return err;
	if((err = e100_exec_cmd(nic, ruc_load_base, 0)))
		return err;
	if((err = e100_exec_cb(nic, NULL, e100_configure)))
		return err;
	if((err = e100_exec_cb(nic, NULL, e100_setup_iaaddr)))
		return err;
	if((err = e100_exec_cmd(nic, cuc_dump_addr,
		nic->dma_addr + offsetof(struct mem, stats))))
		return err;
	if((err = e100_exec_cmd(nic, cuc_dump_reset, 0)))
		return err;

	e100_disable_irq(nic);

	return 0;
}

static void e100_multi(struct nic *nic, struct cb *cb, struct sk_buff *skb)
{
	struct net_device *netdev = nic->netdev;
	struct dev_mc_list *list = netdev->mc_list;
	u16 i, count = min(netdev->mc_count, E100_MAX_MULTICAST_ADDRS);

	cb->command = cpu_to_le16(cb_multi);
	cb->u.multi.count = cpu_to_le16(count * ETH_ALEN);
	for(i = 0; list && i < count; i++, list = list->next)
		memcpy(&cb->u.multi.addr[i*ETH_ALEN], &list->dmi_addr,
			ETH_ALEN);
}

static void e100_set_multicast_list(struct net_device *netdev)
{
	struct nic *nic = netdev_priv(netdev);

	DPRINTK(HW, DEBUG, "mc_count=%d, flags=0x%04X\n",
		netdev->mc_count, netdev->flags);

	if(netdev->flags & IFF_PROMISC)
		nic->flags |= promiscuous;
	else
		nic->flags &= ~promiscuous;

	if(netdev->flags & IFF_ALLMULTI ||
		netdev->mc_count > E100_MAX_MULTICAST_ADDRS)
		nic->flags |= multicast_all;
	else
		nic->flags &= ~multicast_all;

	e100_exec_cb(nic, NULL, e100_configure);
	e100_exec_cb(nic, NULL, e100_multi);
}

static void e100_update_stats(struct nic *nic)
{
	struct net_device_stats *ns = &nic->net_stats;
	struct stats *s = &nic->mem->stats;
	u32 *complete = (nic->mac < mac_82558_D101_A4) ? &s->fc_xmt_pause :
		(nic->mac < mac_82559_D101M) ? (u32 *)&s->xmt_tco_frames :
		&s->complete;

	/* Device's stats reporting may take several microseconds to
	 * complete, so where always waiting for results of the
	 * previous command. */

	if(*complete == le32_to_cpu(cuc_dump_reset_complete)) {
		*complete = 0;
		nic->tx_frames = le32_to_cpu(s->tx_good_frames);
		nic->tx_collisions = le32_to_cpu(s->tx_total_collisions);
		ns->tx_aborted_errors += le32_to_cpu(s->tx_max_collisions);
		ns->tx_window_errors += le32_to_cpu(s->tx_late_collisions);
		ns->tx_carrier_errors += le32_to_cpu(s->tx_lost_crs);
		ns->tx_fifo_errors += le32_to_cpu(s->tx_underruns);
		ns->collisions += nic->tx_collisions;
		ns->tx_errors += le32_to_cpu(s->tx_max_collisions) +
			le32_to_cpu(s->tx_lost_crs);
		ns->rx_dropped += le32_to_cpu(s->rx_resource_errors);
		ns->rx_length_errors += le32_to_cpu(s->rx_short_frame_errors);
		ns->rx_crc_errors += le32_to_cpu(s->rx_crc_errors);
		ns->rx_frame_errors += le32_to_cpu(s->rx_alignment_errors);
		ns->rx_fifo_errors += le32_to_cpu(s->rx_overrun_errors);
		ns->rx_errors += le32_to_cpu(s->rx_crc_errors) +
			le32_to_cpu(s->rx_alignment_errors) +
			le32_to_cpu(s->rx_short_frame_errors) +
			le32_to_cpu(s->rx_cdt_errors);
		nic->tx_deferred += le32_to_cpu(s->tx_deferred);
		nic->tx_single_collisions +=
			le32_to_cpu(s->tx_single_collisions);
		nic->tx_multiple_collisions +=
			le32_to_cpu(s->tx_multiple_collisions);
		if(nic->mac >= mac_82558_D101_A4) {
			nic->tx_fc_pause += le32_to_cpu(s->fc_xmt_pause);
			nic->rx_fc_pause += le32_to_cpu(s->fc_rcv_pause);
			nic->rx_fc_unsupported +=
				le32_to_cpu(s->fc_rcv_unsupported);
			if(nic->mac >= mac_82559_D101M) {
				nic->tx_tco_frames +=
					le16_to_cpu(s->xmt_tco_frames);
				nic->rx_tco_frames +=
					le16_to_cpu(s->rcv_tco_frames);
			}
		}
	}

	e100_exec_cmd(nic, cuc_dump_reset, 0);
}

static void e100_adjust_adaptive_ifs(struct nic *nic, int speed, int duplex)
{
	/* Adjust inter-frame-spacing (IFS) between two transmits if
	 * we're getting collisions on a half-duplex connection. */

	if(duplex == DUPLEX_HALF) {
		u32 prev = nic->adaptive_ifs;
		u32 min_frames = (speed == SPEED_100) ? 1000 : 100;

		if((nic->tx_frames / 32 < nic->tx_collisions) &&
		   (nic->tx_frames > min_frames)) {
			if(nic->adaptive_ifs < 60)
				nic->adaptive_ifs += 5;
		} else if (nic->tx_frames < min_frames) {
			if(nic->adaptive_ifs >= 5)
				nic->adaptive_ifs -= 5;
		}
		if(nic->adaptive_ifs != prev)
			e100_exec_cb(nic, NULL, e100_configure);
	}
}

static void e100_watchdog(unsigned long data)
{
	struct nic *nic = (struct nic *)data;
	struct ethtool_cmd cmd;

	DPRINTK(TIMER, DEBUG, "right now = %ld\n", jiffies);

	/* mii library handles link maintenance tasks */

	mii_ethtool_gset(&nic->mii, &cmd);

	if(mii_link_ok(&nic->mii) && !netif_carrier_ok(nic->netdev)) {
		DPRINTK(LINK, INFO, "link up, %sMbps, %s-duplex\n",
			cmd.speed == SPEED_100 ? "100" : "10",
			cmd.duplex == DUPLEX_FULL ? "full" : "half");
	} else if(!mii_link_ok(&nic->mii) && netif_carrier_ok(nic->netdev)) {
		DPRINTK(LINK, INFO, "link down\n");
	}

	mii_check_link(&nic->mii);

	/* Software generated interrupt to recover from (rare) Rx
	 * allocation failure */
	writeb(irq_sw_gen, &nic->csr->scb.cmd_hi);
	e100_write_flush(nic);

	e100_update_stats(nic);
	e100_adjust_adaptive_ifs(nic, cmd.speed, cmd.duplex);

	if(nic->mac <= mac_82557_D100_C)
		/* Issue a multicast command to workaround a 557 lock up */
		e100_set_multicast_list(nic->netdev);

	if(nic->flags & ich && cmd.speed==SPEED_10 && cmd.duplex==DUPLEX_HALF)
		/* Need SW workaround for ICH[x] 10Mbps/half duplex Tx hang. */
		nic->flags |= ich_10h_workaround;
	else
		nic->flags &= ~ich_10h_workaround;

	mod_timer(&nic->watchdog, jiffies + E100_WATCHDOG_PERIOD);
}

static inline void e100_xmit_prepare(struct nic *nic, struct cb *cb,
	struct sk_buff *skb)
{
	cb->command = nic->tx_command;
	cb->u.tcb.tbd_array = cb->dma_addr + offsetof(struct cb, u.tcb.tbd);
	cb->u.tcb.tcb_byte_count = 0;
	cb->u.tcb.threshold = nic->tx_threshold;
	cb->u.tcb.tbd_count = 1;
	cb->u.tcb.tbd.buf_addr = cpu_to_le32(pci_map_single(nic->pdev,
		skb->data, skb->len, PCI_DMA_TODEVICE));
	cb->u.tcb.tbd.size = cpu_to_le16(skb->len);
}

static int e100_xmit_frame(struct sk_buff *skb, struct net_device *netdev)
{
	struct nic *nic = netdev_priv(netdev);
	int err;

	if(nic->flags & ich_10h_workaround) {
		/* SW workaround for ICH[x] 10Mbps/half duplex Tx hang.
		   Issue a NOP command followed by a 1us delay before
		   issuing the Tx command. */
		e100_exec_cmd(nic, cuc_nop, 0);
		udelay(1);
	}

	err = e100_exec_cb(nic, skb, e100_xmit_prepare);

	switch(err) {
	case -ENOSPC:
		/* We queued the skb, but now we're out of space. */
		netif_stop_queue(netdev);
		break;
	case -ENOMEM:
		/* This is a hard error - log it. */
		DPRINTK(TX_ERR, DEBUG, "Out of Tx resources, returning skb\n");
		netif_stop_queue(netdev);
		return 1;
	}

	netdev->trans_start = jiffies;
	return 0;
}

static inline int e100_tx_clean(struct nic *nic)
{
	struct cb *cb;
	int tx_cleaned = 0;

	spin_lock(&nic->cb_lock);

	DPRINTK(TX_DONE, DEBUG, "cb->status = 0x%04X\n",
		nic->cb_to_clean->status);

	/* Clean CBs marked complete */
	for(cb = nic->cb_to_clean;
	    cb->status & cpu_to_le16(cb_complete);
	    cb = nic->cb_to_clean = cb->next) {
		if(likely(cb->skb != NULL)) {
			nic->net_stats.tx_packets++;
			nic->net_stats.tx_bytes += cb->skb->len;

			pci_unmap_single(nic->pdev,
				le32_to_cpu(cb->u.tcb.tbd.buf_addr),
				le16_to_cpu(cb->u.tcb.tbd.size),
				PCI_DMA_TODEVICE);
			dev_kfree_skb_any(cb->skb);
			cb->skb = NULL;
			tx_cleaned = 1;
		}
		cb->status = 0;
		nic->cbs_avail++;
	}

	spin_unlock(&nic->cb_lock);

	/* Recover from running out of Tx resources in xmit_frame */
	if(unlikely(tx_cleaned && netif_queue_stopped(nic->netdev)))
		netif_wake_queue(nic->netdev);

	return tx_cleaned;
}

static void e100_clean_cbs(struct nic *nic)
{
	if(nic->cbs) {
		while(nic->cbs_avail != nic->params.cbs.count) {
			struct cb *cb = nic->cb_to_clean;
			if(cb->skb) {
				pci_unmap_single(nic->pdev,
					le32_to_cpu(cb->u.tcb.tbd.buf_addr),
					le16_to_cpu(cb->u.tcb.tbd.size),
					PCI_DMA_TODEVICE);
				dev_kfree_skb(cb->skb);
			}
			nic->cb_to_clean = nic->cb_to_clean->next;
			nic->cbs_avail++;
		}
		pci_free_consistent(nic->pdev,
			sizeof(struct cb) * nic->params.cbs.count,
			nic->cbs, nic->cbs_dma_addr);
		nic->cbs = NULL;
		nic->cbs_avail = 0;
	}
	nic->cuc_cmd = cuc_start;
	nic->cb_to_use = nic->cb_to_send = nic->cb_to_clean =
		nic->cbs;
}

static int e100_alloc_cbs(struct nic *nic)
{
	struct cb *cb;
	unsigned int i, count = nic->params.cbs.count;

	nic->cuc_cmd = cuc_start;
	nic->cb_to_use = nic->cb_to_send = nic->cb_to_clean = NULL;
	nic->cbs_avail = 0;

	nic->cbs = pci_alloc_consistent(nic->pdev,
		sizeof(struct cb) * count, &nic->cbs_dma_addr);
	if(!nic->cbs)
		return -ENOMEM;

	for(cb = nic->cbs, i = 0; i < count; cb++, i++) {
		cb->next = (i + 1 < count) ? cb + 1 : nic->cbs;
		cb->prev = (i == 0) ? nic->cbs + count - 1 : cb - 1;

		cb->dma_addr = nic->cbs_dma_addr + i * sizeof(struct cb);
		cb->link = cpu_to_le32(nic->cbs_dma_addr +
			((i+1) % count) * sizeof(struct cb));
		cb->skb = NULL;
	}

	nic->cb_to_use = nic->cb_to_send = nic->cb_to_clean = nic->cbs;
	nic->cbs_avail = count;

	return 0;
}

static inline void e100_start_receiver(struct nic *nic)
{
	/* (Re)start RU if suspended or idle and RFA is non-NULL */
	if(!nic->ru_running && nic->rx_to_clean->skb) {
		e100_exec_cmd(nic, ruc_start, nic->rx_to_clean->dma_addr);
		nic->ru_running = 1;
	}
}

#define RFD_BUF_LEN (sizeof(struct rfd) + VLAN_ETH_FRAME_LEN)
static inline int e100_rx_alloc_skb(struct nic *nic, struct rx *rx)
{
	unsigned int rx_offset = 2; /* u32 align protocol headers */

	if(!(rx->skb = dev_alloc_skb(RFD_BUF_LEN + rx_offset)))
		return -ENOMEM;

	/* Align, init, and map the RFD. */
	rx->skb->dev = nic->netdev;
	skb_reserve(rx->skb, rx_offset);
	memcpy(rx->skb->data, &nic->blank_rfd, sizeof(struct rfd));
	rx->dma_addr = pci_map_single(nic->pdev, rx->skb->data,
		RFD_BUF_LEN, PCI_DMA_BIDIRECTIONAL);

	/* Link the RFD to end of RFA by linking previous RFD to
	 * this one, and clearing EL bit of previous.  */
	if(rx->prev->skb) {
		struct rfd *prev_rfd = (struct rfd *)rx->prev->skb->data;
		put_unaligned(cpu_to_le32(rx->dma_addr),
			(u32 *)&prev_rfd->link);
		wmb();
		prev_rfd->command &= ~cpu_to_le16(cb_el);
		pci_dma_sync_single_for_device(nic->pdev, rx->prev->dma_addr,
			sizeof(struct rfd), PCI_DMA_TODEVICE);
	}

	return 0;
}

static inline int e100_rx_indicate(struct nic *nic, struct rx *rx,
	unsigned int *work_done, unsigned int work_to_do)
{
	struct sk_buff *skb = rx->skb;
	struct rfd *rfd = (struct rfd *)skb->data;
	u16 rfd_status, actual_size;

	if(unlikely(work_done && *work_done >= work_to_do))
		return -EAGAIN;

	/* Need to sync before taking a peek at cb_complete bit */
	pci_dma_sync_single_for_cpu(nic->pdev, rx->dma_addr,
		sizeof(struct rfd), PCI_DMA_FROMDEVICE);
	rfd_status = le16_to_cpu(rfd->status);

	DPRINTK(RX_STATUS, DEBUG, "status=0x%04X\n", rfd_status);

	/* If data isn't ready, nothing to indicate */
	if(unlikely(!(rfd_status & cb_complete)))
       		return -EAGAIN;

	/* Get actual data size */
	actual_size = le16_to_cpu(rfd->actual_size) & 0x3FFF;
	if(unlikely(actual_size > RFD_BUF_LEN - sizeof(struct rfd)))
		actual_size = RFD_BUF_LEN - sizeof(struct rfd);

	/* Get data */
	pci_unmap_single(nic->pdev, rx->dma_addr,
		RFD_BUF_LEN, PCI_DMA_FROMDEVICE);

	/* Pull off the RFD and put the actual data (minus eth hdr) */
	skb_reserve(skb, sizeof(struct rfd));
	skb_put(skb, actual_size);
	skb->protocol = eth_type_trans(skb, nic->netdev);

	if(unlikely(!(rfd_status & cb_ok))) {
		/* Don't indicate if hardware indicates errors */
		nic->net_stats.rx_dropped++;
		dev_kfree_skb_any(skb);
	} else if(actual_size > nic->netdev->mtu + VLAN_ETH_HLEN) {
		/* Don't indicate oversized frames */
		nic->net_stats.rx_over_errors++;
		nic->net_stats.rx_dropped++;
		dev_kfree_skb_any(skb);
	} else {
		nic->net_stats.rx_packets++;
		nic->net_stats.rx_bytes += actual_size;
		nic->netdev->last_rx = jiffies;
#ifdef CONFIG_E100_NAPI
		netif_receive_skb(skb);
#else
		netif_rx(skb);
#endif
		if(work_done)
			(*work_done)++;
	}

	rx->skb = NULL;

	return 0;
}

static inline void e100_rx_clean(struct nic *nic, unsigned int *work_done,
	unsigned int work_to_do)
{
	struct rx *rx;

	/* Indicate newly arrived packets */
	for(rx = nic->rx_to_clean; rx->skb; rx = nic->rx_to_clean = rx->next) {
		if(e100_rx_indicate(nic, rx, work_done, work_to_do))
			break; /* No more to clean */
	}

	/* Alloc new skbs to refill list */
	for(rx = nic->rx_to_use; !rx->skb; rx = nic->rx_to_use = rx->next) {
		if(unlikely(e100_rx_alloc_skb(nic, rx)))
			break; /* Better luck next time (see watchdog) */
	}

	e100_start_receiver(nic);
}

static void e100_rx_clean_list(struct nic *nic)
{
	struct rx *rx;
	unsigned int i, count = nic->params.rfds.count;

	if(nic->rxs) {
		for(rx = nic->rxs, i = 0; i < count; rx++, i++) {
			if(rx->skb) {
				pci_unmap_single(nic->pdev, rx->dma_addr,
					RFD_BUF_LEN, PCI_DMA_FROMDEVICE);
				dev_kfree_skb(rx->skb);
			}
		}
		kfree(nic->rxs);
		nic->rxs = NULL;
	}

	nic->rx_to_use = nic->rx_to_clean = NULL;
	nic->ru_running = 0;
}

static int e100_rx_alloc_list(struct nic *nic)
{
	struct rx *rx;
	unsigned int i, count = nic->params.rfds.count;

	nic->rx_to_use = nic->rx_to_clean = NULL;

	if(!(nic->rxs = kmalloc(sizeof(struct rx) * count, GFP_ATOMIC)))
		return -ENOMEM;
	memset(nic->rxs, 0, sizeof(struct rx) * count);

	for(rx = nic->rxs, i = 0; i < count; rx++, i++) {
		rx->next = (i + 1 < count) ? rx + 1 : nic->rxs;
		rx->prev = (i == 0) ? nic->rxs + count - 1 : rx - 1;
		if(e100_rx_alloc_skb(nic, rx)) {
			e100_rx_clean_list(nic);
			return -ENOMEM;
		}
	}

	nic->rx_to_use = nic->rx_to_clean = nic->rxs;

	return 0;
}

static irqreturn_t e100_intr(int irq, void *dev_id, struct pt_regs *regs)
{
	struct net_device *netdev = dev_id;
	struct nic *nic = netdev_priv(netdev);
	u8 stat_ack = readb(&nic->csr->scb.stat_ack);

	DPRINTK(INTR, DEBUG, "stat_ack = 0x%02X\n", stat_ack);

	if(stat_ack == stat_ack_not_ours ||	/* Not our interrupt */
	   stat_ack == stat_ack_not_present)	/* Hardware is ejected */
		return IRQ_NONE;

	/* Ack interrupt(s) */
	writeb(stat_ack, &nic->csr->scb.stat_ack);

	/* We hit Receive No Resource (RNR); restart RU after cleaning */
	if(stat_ack & stat_ack_rnr)
		nic->ru_running = 0;

#ifdef CONFIG_E100_NAPI
	e100_disable_irq(nic);
	netif_rx_schedule(netdev);
#else
	if(stat_ack & stat_ack_rx)
		e100_rx_clean(nic, NULL, 0);
	if(stat_ack & stat_ack_tx)
		e100_tx_clean(nic);
#endif

	return IRQ_HANDLED;
}

#ifdef CONFIG_E100_NAPI
static int e100_poll(struct net_device *netdev, int *budget)
{
	struct nic *nic = netdev_priv(netdev);
	unsigned int work_to_do = min(netdev->quota, *budget);
	unsigned int work_done = 0;
	int tx_cleaned;

	e100_rx_clean(nic, &work_done, work_to_do);
	tx_cleaned = e100_tx_clean(nic);

	/* If no Rx and Tx cleanup work was done, exit polling mode. */
	if((!tx_cleaned && (work_done == 0)) || !netif_running(netdev)) {
		netif_rx_complete(netdev);
		e100_enable_irq(nic);
		return 0;
	}

	*budget -= work_done;
	netdev->quota -= work_done;

	return 1;
}
#endif

#ifdef CONFIG_NET_POLL_CONTROLLER
static void e100_netpoll(struct net_device *netdev)
{
	struct nic *nic = netdev_priv(netdev);
	e100_disable_irq(nic);
	e100_intr(nic->pdev->irq, netdev, NULL);
	e100_enable_irq(nic);
}
#endif

static struct net_device_stats *e100_get_stats(struct net_device *netdev)
{
	struct nic *nic = netdev_priv(netdev);
	return &nic->net_stats;
}

static int e100_set_mac_address(struct net_device *netdev, void *p)
{
	struct nic *nic = netdev_priv(netdev);
	struct sockaddr *addr = p;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	memcpy(netdev->dev_addr, addr->sa_data, netdev->addr_len);
	e100_exec_cb(nic, NULL, e100_setup_iaaddr);

	return 0;
}

static int e100_change_mtu(struct net_device *netdev, int new_mtu)
{
	if(new_mtu < ETH_ZLEN || new_mtu > ETH_DATA_LEN)
		return -EINVAL;
	netdev->mtu = new_mtu;
	return 0;
}

static int e100_asf(struct nic *nic)
{
	/* ASF can be enabled from eeprom */
	return((nic->pdev->device >= 0x1050) && (nic->pdev->device <= 0x1055) &&
	   (nic->eeprom[eeprom_config_asf] & eeprom_asf) &&
	   !(nic->eeprom[eeprom_config_asf] & eeprom_gcl) &&
	   ((nic->eeprom[eeprom_smbus_addr] & 0xFF) != 0xFE));
}

static int e100_up(struct nic *nic)
{
	int err;

	if((err = e100_rx_alloc_list(nic)))
		return err;
	if((err = e100_alloc_cbs(nic)))
		goto err_rx_clean_list;
	if((err = e100_hw_init(nic)))
		goto err_clean_cbs;
	e100_set_multicast_list(nic->netdev);
	e100_start_receiver(nic);
	mod_timer(&nic->watchdog, jiffies);
	if((err = request_irq(nic->pdev->irq, e100_intr, SA_SHIRQ,
		nic->netdev->name, nic->netdev)))
		goto err_no_irq;
	e100_enable_irq(nic);
	netif_wake_queue(nic->netdev);
	return 0;

err_no_irq:
	del_timer_sync(&nic->watchdog);
err_clean_cbs:
	e100_clean_cbs(nic);
err_rx_clean_list:
	e100_rx_clean_list(nic);
	return err;
}

static void e100_down(struct nic *nic)
{
	e100_hw_reset(nic);
	free_irq(nic->pdev->irq, nic->netdev);
	del_timer_sync(&nic->watchdog);
	netif_carrier_off(nic->netdev);
	netif_stop_queue(nic->netdev);
	e100_clean_cbs(nic);
	e100_rx_clean_list(nic);
}

static void e100_tx_timeout(struct net_device *netdev)
{
	struct nic *nic = netdev_priv(netdev);

	DPRINTK(TX_ERR, DEBUG, "scb.status=0x%02X\n",
		readb(&nic->csr->scb.status));
	e100_down(netdev_priv(netdev));
	e100_up(netdev_priv(netdev));
}

static int e100_loopback_test(struct nic *nic, enum loopback loopback_mode)
{
	int err;
	struct sk_buff *skb;

	/* Use driver resources to perform internal MAC or PHY
	 * loopback test.  A single packet is prepared and transmitted
	 * in loopback mode, and the test passes if the received
	 * packet compares byte-for-byte to the transmitted packet. */

	if((err = e100_rx_alloc_list(nic)))
		return err;
	if((err = e100_alloc_cbs(nic)))
		goto err_clean_rx;

	/* ICH PHY loopback is broken so do MAC loopback instead */
	if(nic->flags & ich && loopback_mode == lb_phy)
		loopback_mode = lb_mac;

	nic->loopback = loopback_mode;
	if((err = e100_hw_init(nic)))
		goto err_loopback_none;

	if(loopback_mode == lb_phy)
		mdio_write(nic->netdev, nic->mii.phy_id, MII_BMCR,
			BMCR_LOOPBACK);

	e100_start_receiver(nic);

	if(!(skb = dev_alloc_skb(ETH_DATA_LEN))) {
		err = -ENOMEM;
		goto err_loopback_none;
	}
	skb_put(skb, ETH_DATA_LEN);
	memset(skb->data, 0xFF, ETH_DATA_LEN);
	e100_xmit_frame(skb, nic->netdev);

	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(HZ / 100 + 1);

	if(memcmp(nic->rx_to_clean->skb->data + sizeof(struct rfd),
	   skb->data, ETH_DATA_LEN))
       		err = -EAGAIN;

err_loopback_none:
	mdio_write(nic->netdev, nic->mii.phy_id, MII_BMCR, 0);
	nic->loopback = lb_none;
	e100_hw_init(nic);
	e100_clean_cbs(nic);
err_clean_rx:
	e100_rx_clean_list(nic);
	return err;
}

#define MII_LED_CONTROL	0x1B
static void e100_blink_led(unsigned long data)
{
	struct nic *nic = (struct nic *)data;
	enum led_state {
		led_on     = 0x01,
		led_off    = 0x04,
		led_on_559 = 0x05,
		led_on_557 = 0x07,
	};

	nic->leds = (nic->leds & led_on) ? led_off :
		(nic->mac < mac_82559_D101M) ? led_on_557 : led_on_559;
	mdio_write(nic->netdev, nic->mii.phy_id, MII_LED_CONTROL, nic->leds);
	mod_timer(&nic->blink_timer, jiffies + HZ / 4);
}

static int e100_get_settings(struct net_device *netdev, struct ethtool_cmd *cmd)
{
	struct nic *nic = netdev_priv(netdev);
	return mii_ethtool_gset(&nic->mii, cmd);
}

static int e100_set_settings(struct net_device *netdev, struct ethtool_cmd *cmd)
{
	struct nic *nic = netdev_priv(netdev);
	int err;

	mdio_write(netdev, nic->mii.phy_id, MII_BMCR, BMCR_RESET);
	err = mii_ethtool_sset(&nic->mii, cmd);
	e100_exec_cb(nic, NULL, e100_configure);

	return err;
}

static void e100_get_drvinfo(struct net_device *netdev,
	struct ethtool_drvinfo *info)
{
	struct nic *nic = netdev_priv(netdev);
	strcpy(info->driver, DRV_NAME);
	strcpy(info->version, DRV_VERSION);
	strcpy(info->fw_version, "N/A");
	strcpy(info->bus_info, pci_name(nic->pdev));
}

static int e100_get_regs_len(struct net_device *netdev)
{
	struct nic *nic = netdev_priv(netdev);
#define E100_PHY_REGS		0x1C
#define E100_REGS_LEN		1 + E100_PHY_REGS + \
	sizeof(nic->mem->dump_buf) / sizeof(u32)
	return E100_REGS_LEN * sizeof(u32);
}

static void e100_get_regs(struct net_device *netdev,
	struct ethtool_regs *regs, void *p)
{
	struct nic *nic = netdev_priv(netdev);
	u32 *buff = p;
	int i;

	regs->version = (1 << 24) | nic->rev_id;
	buff[0] = readb(&nic->csr->scb.cmd_hi) << 24 |
		readb(&nic->csr->scb.cmd_lo) << 16 |
		readw(&nic->csr->scb.status);
	for(i = E100_PHY_REGS; i >= 0; i--)
		buff[1 + E100_PHY_REGS - i] =
			mdio_read(netdev, nic->mii.phy_id, i);
	memset(nic->mem->dump_buf, 0, sizeof(nic->mem->dump_buf));
	e100_exec_cb(nic, NULL, e100_dump);
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(HZ / 100 + 1);
	memcpy(&buff[2 + E100_PHY_REGS], nic->mem->dump_buf,
		sizeof(nic->mem->dump_buf));
}

static void e100_get_wol(struct net_device *netdev, struct ethtool_wolinfo *wol)
{
	struct nic *nic = netdev_priv(netdev);
	wol->supported = (nic->mac >= mac_82558_D101_A4) ?  WAKE_MAGIC : 0;
	wol->wolopts = (nic->flags & wol_magic) ? WAKE_MAGIC : 0;
}

static int e100_set_wol(struct net_device *netdev, struct ethtool_wolinfo *wol)
{
	struct nic *nic = netdev_priv(netdev);

	if(wol->wolopts != WAKE_MAGIC && wol->wolopts != 0)
		return -EOPNOTSUPP;

	if(wol->wolopts)
		nic->flags |= wol_magic;
	else
		nic->flags &= ~wol_magic;

	pci_enable_wake(nic->pdev, 0, nic->flags & (wol_magic | e100_asf(nic)));
	e100_exec_cb(nic, NULL, e100_configure);

	return 0;
}

static u32 e100_get_msglevel(struct net_device *netdev)
{
	struct nic *nic = netdev_priv(netdev);
	return nic->msg_enable;
}

static void e100_set_msglevel(struct net_device *netdev, u32 value)
{
	struct nic *nic = netdev_priv(netdev);
	nic->msg_enable = value;
}

static int e100_nway_reset(struct net_device *netdev)
{
	struct nic *nic = netdev_priv(netdev);
	return mii_nway_restart(&nic->mii);
}

static u32 e100_get_link(struct net_device *netdev)
{
	struct nic *nic = netdev_priv(netdev);
	return mii_link_ok(&nic->mii);
}

static int e100_get_eeprom_len(struct net_device *netdev)
{
	struct nic *nic = netdev_priv(netdev);
	return nic->eeprom_wc << 1;
}

#define E100_EEPROM_MAGIC	0x1234
static int e100_get_eeprom(struct net_device *netdev,
	struct ethtool_eeprom *eeprom, u8 *bytes)
{
	struct nic *nic = netdev_priv(netdev);

	eeprom->magic = E100_EEPROM_MAGIC;
	memcpy(bytes, &((u8 *)nic->eeprom)[eeprom->offset], eeprom->len);

	return 0;
}

static int e100_set_eeprom(struct net_device *netdev,
	struct ethtool_eeprom *eeprom, u8 *bytes)
{
	struct nic *nic = netdev_priv(netdev);

	if(eeprom->magic != E100_EEPROM_MAGIC)
		return -EINVAL;

	memcpy(&((u8 *)nic->eeprom)[eeprom->offset], bytes, eeprom->len);

	return e100_eeprom_save(nic, eeprom->offset >> 1,
		(eeprom->len >> 1) + 1);
}

static void e100_get_ringparam(struct net_device *netdev,
	struct ethtool_ringparam *ring)
{
	struct nic *nic = netdev_priv(netdev);
	struct param_range *rfds = &nic->params.rfds;
	struct param_range *cbs = &nic->params.cbs;

	ring->rx_max_pending = rfds->max;
	ring->tx_max_pending = cbs->max;
	ring->rx_mini_max_pending = 0;
	ring->rx_jumbo_max_pending = 0;
	ring->rx_pending = rfds->count;
	ring->tx_pending = cbs->count;
	ring->rx_mini_pending = 0;
	ring->rx_jumbo_pending = 0;
}

static int e100_set_ringparam(struct net_device *netdev,
	struct ethtool_ringparam *ring)
{
	struct nic *nic = netdev_priv(netdev);
	struct param_range *rfds = &nic->params.rfds;
	struct param_range *cbs = &nic->params.cbs;

	if(netif_running(netdev))
		e100_down(nic);
	rfds->count = max(ring->rx_pending, rfds->min);
	rfds->count = min(rfds->count, rfds->max);
	cbs->count = max(ring->tx_pending, cbs->min);
	cbs->count = min(cbs->count, cbs->max);
	if(netif_running(netdev))
		e100_up(nic);

	return 0;
}

static const char e100_gstrings_test[][ETH_GSTRING_LEN] = {
	"Link test     (on/offline)",
	"Eeprom test   (on/offline)",
	"Self test        (offline)",
	"Mac loopback     (offline)",
	"Phy loopback     (offline)",
};
#define E100_TEST_LEN	sizeof(e100_gstrings_test) / ETH_GSTRING_LEN

static int e100_diag_test_count(struct net_device *netdev)
{
	return E100_TEST_LEN;
}

static void e100_diag_test(struct net_device *netdev,
	struct ethtool_test *test, u64 *data)
{
	struct nic *nic = netdev_priv(netdev);
	int i;

	memset(data, 0, E100_TEST_LEN * sizeof(u64));
	data[0] = !mii_link_ok(&nic->mii);
	data[1] = e100_eeprom_load(nic);
	if(test->flags & ETH_TEST_FL_OFFLINE) {
		if(netif_running(netdev))
			e100_down(nic);
		data[2] = e100_self_test(nic);
		data[3] = e100_loopback_test(nic, lb_mac);
		data[4] = e100_loopback_test(nic, lb_phy);
		if(netif_running(netdev))
			e100_up(nic);
	}
	for(i = 0; i < E100_TEST_LEN; i++)
		test->flags |= data[i] ? ETH_TEST_FL_FAILED : 0;
}

static int e100_phys_id(struct net_device *netdev, u32 data)
{
	struct nic *nic = netdev_priv(netdev);

	if(!data || data > (u32)(MAX_SCHEDULE_TIMEOUT / HZ))
		data = (u32)(MAX_SCHEDULE_TIMEOUT / HZ);
	mod_timer(&nic->blink_timer, jiffies);
	set_current_state(TASK_INTERRUPTIBLE);
	schedule_timeout(data * HZ);
	del_timer_sync(&nic->blink_timer);
	mdio_write(netdev, nic->mii.phy_id, MII_LED_CONTROL, 0);

	return 0;
}

static const char e100_gstrings_stats[][ETH_GSTRING_LEN] = {
	"rx_packets", "tx_packets", "rx_bytes", "tx_bytes", "rx_errors",
	"tx_errors", "rx_dropped", "tx_dropped", "multicast", "collisions",
	"rx_length_errors", "rx_over_errors", "rx_crc_errors",
	"rx_frame_errors", "rx_fifo_errors", "rx_missed_errors",
	"tx_aborted_errors", "tx_carrier_errors", "tx_fifo_errors",
	"tx_heartbeat_errors", "tx_window_errors",
	/* device-specific stats */
	"tx_deferred", "tx_single_collisions", "tx_multi_collisions",
	"tx_flow_control_pause", "rx_flow_control_pause",
	"rx_flow_control_unsupported", "tx_tco_packets", "rx_tco_packets",
};
#define E100_NET_STATS_LEN	21
#define E100_STATS_LEN	sizeof(e100_gstrings_stats) / ETH_GSTRING_LEN

static int e100_get_stats_count(struct net_device *netdev)
{
	return E100_STATS_LEN;
}

static void e100_get_ethtool_stats(struct net_device *netdev,
	struct ethtool_stats *stats, u64 *data)
{
	struct nic *nic = netdev_priv(netdev);
	int i;

	for(i = 0; i < E100_NET_STATS_LEN; i++)
		data[i] = ((unsigned long *)&nic->net_stats)[i];

	data[i++] = nic->tx_deferred;
	data[i++] = nic->tx_single_collisions;
	data[i++] = nic->tx_multiple_collisions;
	data[i++] = nic->tx_fc_pause;
	data[i++] = nic->rx_fc_pause;
	data[i++] = nic->rx_fc_unsupported;
	data[i++] = nic->tx_tco_frames;
	data[i++] = nic->rx_tco_frames;
}

static void e100_get_strings(struct net_device *netdev, u32 stringset, u8 *data)
{
	switch(stringset) {
	case ETH_SS_TEST:
		memcpy(data, *e100_gstrings_test, sizeof(e100_gstrings_test));
		break;
	case ETH_SS_STATS:
		memcpy(data, *e100_gstrings_stats, sizeof(e100_gstrings_stats));
		break;
	}
}

static struct ethtool_ops e100_ethtool_ops = {
	.get_settings		= e100_get_settings,
	.set_settings		= e100_set_settings,
	.get_drvinfo		= e100_get_drvinfo,
	.get_regs_len		= e100_get_regs_len,
	.get_regs		= e100_get_regs,
	.get_wol		= e100_get_wol,
	.set_wol		= e100_set_wol,
	.get_msglevel		= e100_get_msglevel,
	.set_msglevel		= e100_set_msglevel,
	.nway_reset		= e100_nway_reset,
	.get_link		= e100_get_link,
	.get_eeprom_len		= e100_get_eeprom_len,
	.get_eeprom		= e100_get_eeprom,
	.set_eeprom		= e100_set_eeprom,
	.get_ringparam		= e100_get_ringparam,
	.set_ringparam		= e100_set_ringparam,
	.self_test_count	= e100_diag_test_count,
	.self_test		= e100_diag_test,
	.get_strings		= e100_get_strings,
	.phys_id		= e100_phys_id,
	.get_stats_count	= e100_get_stats_count,
	.get_ethtool_stats	= e100_get_ethtool_stats,
};

static int e100_do_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{
	struct nic *nic = netdev_priv(netdev);

	return generic_mii_ioctl(&nic->mii, if_mii(ifr), cmd, NULL);
}

static int e100_alloc(struct nic *nic)
{
	nic->mem = pci_alloc_consistent(nic->pdev, sizeof(struct mem),
		&nic->dma_addr);
	return nic->mem ? 0 : -ENOMEM;
}

static void e100_free(struct nic *nic)
{
	if(nic->mem) {
		pci_free_consistent(nic->pdev, sizeof(struct mem),
			nic->mem, nic->dma_addr);
		nic->mem = NULL;
	}
}

static int e100_open(struct net_device *netdev)
{
	struct nic *nic = netdev_priv(netdev);
	int err = 0;

	netif_carrier_off(netdev);
	if((err = e100_up(nic)))
		DPRINTK(IFUP, ERR, "Cannot open interface, aborting.\n");
	return err;
}

static int e100_close(struct net_device *netdev)
{
	e100_down(netdev_priv(netdev));
	return 0;
}

static int __devinit e100_probe(struct pci_dev *pdev,
	const struct pci_device_id *ent)
{
	struct net_device *netdev;
	struct nic *nic;
	int err;

	if(!(netdev = alloc_etherdev(sizeof(struct nic)))) {
		if(((1 << debug) - 1) & NETIF_MSG_PROBE)
			printk(KERN_ERR PFX "Etherdev alloc failed, abort.\n");
		return -ENOMEM;
	}

	netdev->open = e100_open;
	netdev->stop = e100_close;
	netdev->hard_start_xmit = e100_xmit_frame;
	netdev->get_stats = e100_get_stats;
	netdev->set_multicast_list = e100_set_multicast_list;
	netdev->set_mac_address = e100_set_mac_address;
	netdev->change_mtu = e100_change_mtu;
	netdev->do_ioctl = e100_do_ioctl;
	SET_ETHTOOL_OPS(netdev, &e100_ethtool_ops);
	netdev->tx_timeout = e100_tx_timeout;
	netdev->watchdog_timeo = E100_WATCHDOG_PERIOD;
#ifdef CONFIG_E100_NAPI
	netdev->poll = e100_poll;
	netdev->weight = E100_NAPI_WEIGHT;
#endif
#ifdef CONFIG_NET_POLL_CONTROLLER
	netdev->poll_controller = e100_netpoll;
#endif

	nic = netdev_priv(netdev);
	nic->netdev = netdev;
	nic->pdev = pdev;
	nic->msg_enable = (1 << debug) - 1;
	pci_set_drvdata(pdev, netdev);

	if((err = pci_enable_device(pdev))) {
		DPRINTK(PROBE, ERR, "Cannot enable PCI device, aborting.\n");
		goto err_out_free_dev;
	}

	if(!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM)) {
		DPRINTK(PROBE, ERR, "Cannot find proper PCI device "
			"base address, aborting.\n");
		err = -ENODEV;
		goto err_out_disable_pdev;
	}

	if((err = pci_request_regions(pdev, DRV_NAME))) {
		DPRINTK(PROBE, ERR, "Cannot obtain PCI resources, aborting.\n");
		goto err_out_disable_pdev;
	}

	pci_set_master(pdev);

	if((err = pci_set_dma_mask(pdev, 0xFFFFFFFFULL))) {
		DPRINTK(PROBE, ERR, "No usable DMA configuration, aborting.\n");
		goto err_out_free_res;
	}

	SET_MODULE_OWNER(netdev);
	SET_NETDEV_DEV(netdev, &pdev->dev);

	nic->csr = ioremap(pci_resource_start(pdev, 0), sizeof(struct csr));
	if(!nic->csr) {
		DPRINTK(PROBE, ERR, "Cannot map device registers, aborting.\n");
		err = -ENOMEM;
		goto err_out_free_res;
	}

	if(ent->driver_data)
		nic->flags |= ich;
	else
		nic->flags &= ~ich;

	spin_lock_init(&nic->cb_lock);
	spin_lock_init(&nic->cmd_lock);

	init_timer(&nic->watchdog);
	nic->watchdog.function = e100_watchdog;
	nic->watchdog.data = (unsigned long)nic;
	init_timer(&nic->blink_timer);
	nic->blink_timer.function = e100_blink_led;
	nic->blink_timer.data = (unsigned long)nic;

	if((err = e100_alloc(nic))) {
		DPRINTK(PROBE, ERR, "Cannot alloc driver memory, aborting.\n");
		goto err_out_iounmap;
	}

	e100_get_defaults(nic);
	e100_hw_reset(nic);
	e100_phy_init(nic);

	if((err = e100_eeprom_load(nic)))
		goto err_out_free;

	memcpy(netdev->dev_addr, nic->eeprom, ETH_ALEN);
	if(!is_valid_ether_addr(netdev->dev_addr)) {
		DPRINTK(PROBE, ERR, "Invalid MAC address from "
			"EEPROM, aborting.\n");
		err = -EAGAIN;
		goto err_out_free;
	}

	/* Wol magic packet can be enabled from eeprom */
	if((nic->mac >= mac_82558_D101_A4) &&
	   (nic->eeprom[eeprom_id] & eeprom_id_wol))
		nic->flags |= wol_magic;

	pci_enable_wake(pdev, 0, nic->flags & (wol_magic | e100_asf(nic)));

	if((err = register_netdev(netdev))) {
		DPRINTK(PROBE, ERR, "Cannot register net device, aborting.\n");
		goto err_out_free;
	}

	DPRINTK(PROBE, INFO, "addr 0x%lx, irq %d, "
		"MAC addr %02X:%02X:%02X:%02X:%02X:%02X\n",
		pci_resource_start(pdev, 0), pdev->irq,
		netdev->dev_addr[0], netdev->dev_addr[1], netdev->dev_addr[2],
		netdev->dev_addr[3], netdev->dev_addr[4], netdev->dev_addr[5]);

	return 0;

err_out_free:
	e100_free(nic);
err_out_iounmap:
	iounmap(nic->csr);
err_out_free_res:
	pci_release_regions(pdev);
err_out_disable_pdev:
	pci_disable_device(pdev);
err_out_free_dev:
	pci_set_drvdata(pdev, NULL);
	free_netdev(netdev);
	return err;
}

static void __devexit e100_remove(struct pci_dev *pdev)
{
	struct net_device *netdev = pci_get_drvdata(pdev);

	if(netdev) {
		struct nic *nic = netdev_priv(netdev);
		unregister_netdev(netdev);
		e100_free(nic);
		iounmap(nic->csr);
		free_netdev(netdev);
		pci_release_regions(pdev);
		pci_disable_device(pdev);
		pci_set_drvdata(pdev, NULL);
	}
}

#ifdef CONFIG_PM
static int e100_suspend(struct pci_dev *pdev, u32 state)
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct nic *nic = netdev_priv(netdev);

	if(netif_running(netdev))
		e100_down(nic);
	e100_hw_reset(nic);
	netif_device_detach(netdev);

	pci_save_state(pdev, nic->pm_state);
	pci_enable_wake(pdev, state, nic->flags & (wol_magic | e100_asf(nic)));
	pci_disable_device(pdev);
	pci_set_power_state(pdev, state);

	return 0;
}

static int e100_resume(struct pci_dev *pdev)
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct nic *nic = netdev_priv(netdev);

	pci_set_power_state(pdev, 0);
	pci_restore_state(pdev, nic->pm_state);
	e100_hw_init(nic);

	netif_device_attach(netdev);
	if(netif_running(netdev))
		e100_up(nic);

	return 0;
}
#endif

static struct pci_driver e100_driver = {
	.name =         DRV_NAME,
	.id_table =     e100_id_table,
	.probe =        e100_probe,
	.remove =       __devexit_p(e100_remove),
#ifdef CONFIG_PM
	.suspend =      e100_suspend,
	.resume =       e100_resume,
#endif
};

static int __init e100_init_module(void)
{
	if(((1 << debug) - 1) & NETIF_MSG_DRV) {
		printk(KERN_INFO PFX "%s, %s\n", DRV_DESCRIPTION, DRV_VERSION);
		printk(KERN_INFO PFX "%s\n", DRV_COPYRIGHT);
	}
        return pci_module_init(&e100_driver);
}

static void __exit e100_cleanup_module(void)
{
	pci_unregister_driver(&e100_driver);
}

module_init(e100_init_module);
module_exit(e100_cleanup_module);
