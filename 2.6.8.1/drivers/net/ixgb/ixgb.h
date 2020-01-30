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

#ifndef _IXGB_H_
#define _IXGB_H_

#include <linux/stddef.h>
#include <linux/config.h>
#include <linux/module.h>
#include <linux/types.h>
#include <asm/byteorder.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/delay.h>
#include <linux/timer.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/string.h>
#include <linux/pagemap.h>
#include <linux/dma-mapping.h>
#include <asm/bitops.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <linux/capability.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <net/pkt_sched.h>
#include <linux/list.h>
#include <linux/reboot.h>
#ifdef NETIF_F_TSO
#include <net/checksum.h>
#endif

#include <linux/ethtool.h>
#include <linux/if_vlan.h>

#define BAR_0		0
#define BAR_1		1
#define BAR_5		5

struct ixgb_adapter;
#include "ixgb_hw.h"
#include "ixgb_ee.h"
#include "ixgb_ids.h"

#ifdef _DEBUG_DRIVER_
#define IXGB_DBG(args...) printk(KERN_DEBUG "ixgb: " args)
#else
#define IXGB_DBG(args...)
#endif

#define IXGB_ERR(args...) printk(KERN_ERR "ixgb: " args)

/* Supported Rx Buffer Sizes */
#define IXGB_RXBUFFER_2048  2048
#define IXGB_RXBUFFER_4096  4096
#define IXGB_RXBUFFER_8192  8192
#define IXGB_RXBUFFER_16384 16384

/* How many Tx Descriptors do we need to call netif_wake_queue? */
#define IXGB_TX_QUEUE_WAKE 16

/* How many Rx Buffers do we bundle into one write to the hardware ? */
#define IXGB_RX_BUFFER_WRITE	16	/* Must be power of 2 */

/* only works for sizes that are powers of 2 */
#define IXGB_ROUNDUP(i, size) ((i) = (((i) + (size) - 1) & ~((size) - 1)))

/* wrapper around a pointer to a socket buffer,
 * so a DMA handle can be stored along with the buffer */
struct ixgb_buffer {
	struct sk_buff *skb;
	uint64_t dma;
	unsigned long length;
	unsigned long time_stamp;
	unsigned int next_to_watch;
};

struct ixgb_desc_ring {
	/* pointer to the descriptor ring memory */
	void *desc;
	/* physical address of the descriptor ring */
	dma_addr_t dma;
	/* length of descriptor ring in bytes */
	unsigned int size;
	/* number of descriptors in the ring */
	unsigned int count;
	/* next descriptor to associate a buffer with */
	unsigned int next_to_use;
	/* next descriptor to check for DD status bit */
	unsigned int next_to_clean;
	/* array of buffer information structs */
	struct ixgb_buffer *buffer_info;
};

#define IXGB_DESC_UNUSED(R) \
	((((R)->next_to_clean > (R)->next_to_use) ? 0 : (R)->count) + \
	(R)->next_to_clean - (R)->next_to_use - 1)

#define IXGB_GET_DESC(R, i, type)	(&(((struct type *)((R).desc))[i]))
#define IXGB_RX_DESC(R, i)		IXGB_GET_DESC(R, i, ixgb_rx_desc)
#define IXGB_TX_DESC(R, i)		IXGB_GET_DESC(R, i, ixgb_tx_desc)
#define IXGB_CONTEXT_DESC(R, i)	IXGB_GET_DESC(R, i, ixgb_context_desc)

/* board specific private data structure */

struct ixgb_adapter {
	struct timer_list watchdog_timer;
	struct vlan_group *vlgrp;
	uint32_t bd_number;
	uint32_t rx_buffer_len;
	uint32_t part_num;
	uint16_t link_speed;
	uint16_t link_duplex;
	spinlock_t tx_lock;
	atomic_t irq_sem;
	struct work_struct tx_timeout_task;

	struct timer_list blink_timer;
	unsigned long led_status;

	/* TX */
	struct ixgb_desc_ring tx_ring;
	unsigned long timeo_start;
	uint32_t tx_cmd_type;
	uint64_t hw_csum_tx_good;
	uint64_t hw_csum_tx_error;
	uint32_t tx_int_delay;
	boolean_t tx_int_delay_enable;

	/* RX */
	struct ixgb_desc_ring rx_ring;
	uint64_t hw_csum_rx_error;
	uint64_t hw_csum_rx_good;
	uint32_t rx_int_delay;
	boolean_t raidc;
	boolean_t rx_csum;

	/* OS defined structs */
	struct net_device *netdev;
	struct pci_dev *pdev;
	struct net_device_stats net_stats;

	/* structs defined in ixgb_hw.h */
	struct ixgb_hw hw;
	struct ixgb_hw_stats stats;
	uint32_t pci_state[16];
};
#endif				/* _IXGB_H_ */
