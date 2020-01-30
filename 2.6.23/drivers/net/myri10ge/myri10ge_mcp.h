#ifndef __MYRI10GE_MCP_H__
#define __MYRI10GE_MCP_H__

#define MXGEFW_VERSION_MAJOR	1
#define MXGEFW_VERSION_MINOR	4

/* 8 Bytes */
struct mcp_dma_addr {
	__be32 high;
	__be32 low;
};

/* 4 Bytes */
struct mcp_slot {
	__sum16 checksum;
	__be16 length;
};

/* 64 Bytes */
struct mcp_cmd {
	__be32 cmd;
	__be32 data0;		/* will be low portion if data > 32 bits */
	/* 8 */
	__be32 data1;		/* will be high portion if data > 32 bits */
	__be32 data2;		/* currently unused.. */
	/* 16 */
	struct mcp_dma_addr response_addr;
	/* 24 */
	u8 pad[40];
};

/* 8 Bytes */
struct mcp_cmd_response {
	__be32 data;
	__be32 result;
};

/*
 * flags used in mcp_kreq_ether_send_t:
 *
 * The SMALL flag is only needed in the first segment. It is raised
 * for packets that are total less or equal 512 bytes.
 *
 * The CKSUM flag must be set in all segments.
 *
 * The PADDED flags is set if the packet needs to be padded, and it
 * must be set for all segments.
 *
 * The  MXGEFW_FLAGS_ALIGN_ODD must be set if the cumulative
 * length of all previous segments was odd.
 */

#define MXGEFW_FLAGS_SMALL      0x1
#define MXGEFW_FLAGS_TSO_HDR    0x1
#define MXGEFW_FLAGS_FIRST      0x2
#define MXGEFW_FLAGS_ALIGN_ODD  0x4
#define MXGEFW_FLAGS_CKSUM      0x8
#define MXGEFW_FLAGS_TSO_LAST   0x8
#define MXGEFW_FLAGS_NO_TSO     0x10
#define MXGEFW_FLAGS_TSO_CHOP   0x10
#define MXGEFW_FLAGS_TSO_PLD    0x20

#define MXGEFW_SEND_SMALL_SIZE  1520
#define MXGEFW_MAX_MTU          9400

union mcp_pso_or_cumlen {
	u16 pseudo_hdr_offset;
	u16 cum_len;
};

#define	MXGEFW_MAX_SEND_DESC 12
#define MXGEFW_PAD	    2

/* 16 Bytes */
struct mcp_kreq_ether_send {
	__be32 addr_high;
	__be32 addr_low;
	__be16 pseudo_hdr_offset;
	__be16 length;
	u8 pad;
	u8 rdma_count;
	u8 cksum_offset;	/* where to start computing cksum */
	u8 flags;		/* as defined above */
};

/* 8 Bytes */
struct mcp_kreq_ether_recv {
	__be32 addr_high;
	__be32 addr_low;
};

/* Commands */

#define	MXGEFW_BOOT_HANDOFF	0xfc0000
#define	MXGEFW_BOOT_DUMMY_RDMA	0xfc01c0

#define	MXGEFW_ETH_CMD		0xf80000
#define	MXGEFW_ETH_SEND_4	0x200000
#define	MXGEFW_ETH_SEND_1	0x240000
#define	MXGEFW_ETH_SEND_2	0x280000
#define	MXGEFW_ETH_SEND_3	0x2c0000
#define	MXGEFW_ETH_RECV_SMALL	0x300000
#define	MXGEFW_ETH_RECV_BIG	0x340000

#define	MXGEFW_ETH_SEND(n)		(0x200000 + (((n) & 0x03) * 0x40000))
#define	MXGEFW_ETH_SEND_OFFSET(n)	(MXGEFW_ETH_SEND(n) - MXGEFW_ETH_SEND_4)

enum myri10ge_mcp_cmd_type {
	MXGEFW_CMD_NONE = 0,
	/* Reset the mcp, it is left in a safe state, waiting
	 * for the driver to set all its parameters */
	MXGEFW_CMD_RESET,

	/* get the version number of the current firmware..
	 * (may be available in the eeprom strings..? */
	MXGEFW_GET_MCP_VERSION,

	/* Parameters which must be set by the driver before it can
	 * issue MXGEFW_CMD_ETHERNET_UP. They persist until the next
	 * MXGEFW_CMD_RESET is issued */

	MXGEFW_CMD_SET_INTRQ_DMA,
	MXGEFW_CMD_SET_BIG_BUFFER_SIZE,	/* in bytes, power of 2 */
	MXGEFW_CMD_SET_SMALL_BUFFER_SIZE,	/* in bytes */

	/* Parameters which refer to lanai SRAM addresses where the
	 * driver must issue PIO writes for various things */

	MXGEFW_CMD_GET_SEND_OFFSET,
	MXGEFW_CMD_GET_SMALL_RX_OFFSET,
	MXGEFW_CMD_GET_BIG_RX_OFFSET,
	MXGEFW_CMD_GET_IRQ_ACK_OFFSET,
	MXGEFW_CMD_GET_IRQ_DEASSERT_OFFSET,

	/* Parameters which refer to rings stored on the MCP,
	 * and whose size is controlled by the mcp */

	MXGEFW_CMD_GET_SEND_RING_SIZE,	/* in bytes */
	MXGEFW_CMD_GET_RX_RING_SIZE,	/* in bytes */

	/* Parameters which refer to rings stored in the host,
	 * and whose size is controlled by the host.  Note that
	 * all must be physically contiguous and must contain
	 * a power of 2 number of entries.  */

	MXGEFW_CMD_SET_INTRQ_SIZE,	/* in bytes */

	/* command to bring ethernet interface up.  Above parameters
	 * (plus mtu & mac address) must have been exchanged prior
	 * to issuing this command  */
	MXGEFW_CMD_ETHERNET_UP,

	/* command to bring ethernet interface down.  No further sends
	 * or receives may be processed until an MXGEFW_CMD_ETHERNET_UP
	 * is issued, and all interrupt queues must be flushed prior
	 * to ack'ing this command */

	MXGEFW_CMD_ETHERNET_DOWN,

	/* commands the driver may issue live, without resetting
	 * the nic.  Note that increasing the mtu "live" should
	 * only be done if the driver has already supplied buffers
	 * sufficiently large to handle the new mtu.  Decreasing
	 * the mtu live is safe */

	MXGEFW_CMD_SET_MTU,
	MXGEFW_CMD_GET_INTR_COAL_DELAY_OFFSET,	/* in microseconds */
	MXGEFW_CMD_SET_STATS_INTERVAL,	/* in microseconds */
	MXGEFW_CMD_SET_STATS_DMA_OBSOLETE,	/* replaced by SET_STATS_DMA_V2 */

	MXGEFW_ENABLE_PROMISC,
	MXGEFW_DISABLE_PROMISC,
	MXGEFW_SET_MAC_ADDRESS,

	MXGEFW_ENABLE_FLOW_CONTROL,
	MXGEFW_DISABLE_FLOW_CONTROL,

	/* do a DMA test
	 * data0,data1 = DMA address
	 * data2       = RDMA length (MSH), WDMA length (LSH)
	 * command return data = repetitions (MSH), 0.5-ms ticks (LSH)
	 */
	MXGEFW_DMA_TEST,

	MXGEFW_ENABLE_ALLMULTI,
	MXGEFW_DISABLE_ALLMULTI,

	/* returns MXGEFW_CMD_ERROR_MULTICAST
	 * if there is no room in the cache
	 * data0,MSH(data1) = multicast group address */
	MXGEFW_JOIN_MULTICAST_GROUP,
	/* returns MXGEFW_CMD_ERROR_MULTICAST
	 * if the address is not in the cache,
	 * or is equal to FF-FF-FF-FF-FF-FF
	 * data0,MSH(data1) = multicast group address */
	MXGEFW_LEAVE_MULTICAST_GROUP,
	MXGEFW_LEAVE_ALL_MULTICAST_GROUPS,

	MXGEFW_CMD_SET_STATS_DMA_V2,
	/* data0, data1 = bus addr,
	 * data2 = sizeof(struct mcp_irq_data) from driver point of view, allows
	 * adding new stuff to mcp_irq_data without changing the ABI */

	MXGEFW_CMD_UNALIGNED_TEST,
	/* same than DMA_TEST (same args) but abort with UNALIGNED on unaligned
	 * chipset */

	MXGEFW_CMD_UNALIGNED_STATUS
	    /* return data = boolean, true if the chipset is known to be unaligned */
};

enum myri10ge_mcp_cmd_status {
	MXGEFW_CMD_OK = 0,
	MXGEFW_CMD_UNKNOWN,
	MXGEFW_CMD_ERROR_RANGE,
	MXGEFW_CMD_ERROR_BUSY,
	MXGEFW_CMD_ERROR_EMPTY,
	MXGEFW_CMD_ERROR_CLOSED,
	MXGEFW_CMD_ERROR_HASH_ERROR,
	MXGEFW_CMD_ERROR_BAD_PORT,
	MXGEFW_CMD_ERROR_RESOURCES,
	MXGEFW_CMD_ERROR_MULTICAST,
	MXGEFW_CMD_ERROR_UNALIGNED
};

#define MXGEFW_OLD_IRQ_DATA_LEN 40

struct mcp_irq_data {
	/* add new counters at the beginning */
	__be32 future_use[1];
	__be32 dropped_pause;
	__be32 dropped_unicast_filtered;
	__be32 dropped_bad_crc32;
	__be32 dropped_bad_phy;
	__be32 dropped_multicast_filtered;
	/* 40 Bytes */
	__be32 send_done_count;

#define MXGEFW_LINK_DOWN 0
#define MXGEFW_LINK_UP 1
#define MXGEFW_LINK_MYRINET 2
#define MXGEFW_LINK_UNKNOWN 3
	__be32 link_up;
	__be32 dropped_link_overflow;
	__be32 dropped_link_error_or_filtered;
	__be32 dropped_runt;
	__be32 dropped_overrun;
	__be32 dropped_no_small_buffer;
	__be32 dropped_no_big_buffer;
	__be32 rdma_tags_available;

	u8 tx_stopped;
	u8 link_down;
	u8 stats_updated;
	u8 valid;
};

#endif				/* __MYRI10GE_MCP_H__ */
