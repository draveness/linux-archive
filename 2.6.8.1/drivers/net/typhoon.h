/* typhoon.h:	chip info for the 3Com 3CR990 family of controllers */
/*
	Written 2002-2003 by David Dillow <dave@thedillows.org>

	This software may be used and distributed according to the terms of
	the GNU General Public License (GPL), incorporated herein by reference.
	Drivers based on or derived from this code fall under the GPL and must
	retain the authorship, copyright and license notice.  This file is not
	a complete program and may only be used when the entire operating
	system is licensed under the GPL.

	This software is available on a public web site. It may enable
	cryptographic capabilities of the 3Com hardware, and may be
	exported from the United States under License Exception "TSU"
	pursuant to 15 C.F.R. Section 740.13(e).

	This work was funded by the National Library of Medicine under
	the Department of Energy project number 0274DD06D1 and NLM project
	number Y1-LM-2015-01.
*/

/* All Typhoon ring positions are specificed in bytes, and point to the
 * first "clean" entry in the ring -- ie the next entry we use for whatever
 * purpose.
 */

/* The Typhoon basic ring
 * ringBase:  where this ring lives (our virtual address)
 * lastWrite: the next entry we'll use
 */
struct basic_ring {
	u8 *ringBase;
	u32 lastWrite;
};

/* The Typoon transmit ring -- same as a basic ring, plus:
 * lastRead:      where we're at in regard to cleaning up the ring
 * writeRegister: register to use for writing (different for Hi & Lo rings)
 */
struct transmit_ring {
	u8 *ringBase;
	u32 lastWrite;
	u32 lastRead;
	int writeRegister;
};

/* The host<->Typhoon ring index structure
 * This indicates the current positions in the rings
 * 
 * All values must be in little endian format for the 3XP
 *
 * rxHiCleared:   entry we've cleared to in the Hi receive ring
 * rxLoCleared:   entry we've cleared to in the Lo receive ring
 * rxBuffReady:   next entry we'll put a free buffer in
 * respCleared:   entry we've cleared to in the response ring
 *
 * txLoCleared:   entry the NIC has cleared to in the Lo transmit ring
 * txHiCleared:   entry the NIC has cleared to in the Hi transmit ring
 * rxLoReady:     entry the NIC has filled to in the Lo receive ring
 * rxBuffCleared: entry the NIC has cleared in the free buffer ring
 * cmdCleared:    entry the NIC has cleared in the command ring
 * respReady:     entry the NIC has filled to in the response ring
 * rxHiReady:     entry the NIC has filled to in the Hi receive ring
 */
struct typhoon_indexes {
	/* The first four are written by the host, and read by the NIC */
	volatile u32 rxHiCleared;
	volatile u32 rxLoCleared;
	volatile u32 rxBuffReady;
	volatile u32 respCleared;

	/* The remaining are written by the NIC, and read by the host */
	volatile u32 txLoCleared;
	volatile u32 txHiCleared;
	volatile u32 rxLoReady;
	volatile u32 rxBuffCleared;
	volatile u32 cmdCleared;
	volatile u32 respReady;
	volatile u32 rxHiReady;
} __attribute__ ((packed));

/* The host<->Typhoon interface
 * Our means of communicating where things are
 *
 * All values must be in little endian format for the 3XP
 *
 * ringIndex:   64 bit bus address of the index structure
 * txLoAddr:    64 bit bus address of the Lo transmit ring
 * txLoSize:    size (in bytes) of the Lo transmit ring
 * txHi*:       as above for the Hi priority transmit ring
 * rxLo*:       as above for the Lo priority receive ring
 * rxBuff*:     as above for the free buffer ring
 * cmd*:        as above for the command ring
 * resp*:       as above for the response ring
 * zeroAddr:    64 bit bus address of a zero word (for DMA)
 * rxHi*:       as above for the Hi Priority receive ring
 *
 * While there is room for 64 bit addresses, current versions of the 3XP
 * only do 32 bit addresses, so the *Hi for each of the above will always
 * be zero.
 */
struct typhoon_interface {
	u32 ringIndex;
	u32 ringIndexHi;
	u32 txLoAddr;
	u32 txLoAddrHi;
	u32 txLoSize;
	u32 txHiAddr;
	u32 txHiAddrHi;
	u32 txHiSize;
	u32 rxLoAddr;
	u32 rxLoAddrHi;
	u32 rxLoSize;
	u32 rxBuffAddr;
	u32 rxBuffAddrHi;
	u32 rxBuffSize;
	u32 cmdAddr;
	u32 cmdAddrHi;
	u32 cmdSize;
	u32 respAddr;
	u32 respAddrHi;
	u32 respSize;
	u32 zeroAddr;
	u32 zeroAddrHi;
	u32 rxHiAddr;
	u32 rxHiAddrHi;
	u32 rxHiSize;
} __attribute__ ((packed));

/* The Typhoon transmit/fragment descriptor
 *
 * A packet is described by a packet descriptor, followed by option descriptors,
 * if any, then one or more fragment descriptors.
 * 
 * Packet descriptor:
 * flags:	Descriptor type
 * len:i	zero, or length of this packet
 * addr*:	8 bytes of opaque data to the firmware -- for skb pointer
 * processFlags: Determine offload tasks to perform on this packet.
 *
 * Fragment descriptor:
 * flags:	Descriptor type
 * len:i	length of this fragment
 * addr:	low bytes of DMA address for this part of the packet
 * addrHi:	hi bytes of DMA address for this part of the packet
 * processFlags: must be zero
 *
 * TYPHOON_DESC_VALID is not mentioned in their docs, but their Linux
 * driver uses it.
 */
struct tx_desc {
	u8  flags;
#define TYPHOON_TYPE_MASK	0x07
#define 	TYPHOON_FRAG_DESC	0x00
#define 	TYPHOON_TX_DESC		0x01
#define 	TYPHOON_CMD_DESC	0x02
#define 	TYPHOON_OPT_DESC	0x03
#define 	TYPHOON_RX_DESC		0x04
#define 	TYPHOON_RESP_DESC	0x05
#define TYPHOON_OPT_TYPE_MASK	0xf0
#define 	TYPHOON_OPT_IPSEC	0x00
#define 	TYPHOON_OPT_TCP_SEG	0x10
#define TYPHOON_CMD_RESPOND	0x40
#define TYPHOON_RESP_ERROR	0x40
#define TYPHOON_RX_ERROR	0x40
#define TYPHOON_DESC_VALID	0x80
	u8  numDesc;
	u16 len;
	u32 addr;
	u32 addrHi;
	u32 processFlags;
#define TYPHOON_TX_PF_NO_CRC		__constant_cpu_to_le32(0x00000001)
#define TYPHOON_TX_PF_IP_CHKSUM		__constant_cpu_to_le32(0x00000002)
#define TYPHOON_TX_PF_TCP_CHKSUM	__constant_cpu_to_le32(0x00000004)
#define TYPHOON_TX_PF_TCP_SEGMENT	__constant_cpu_to_le32(0x00000008)
#define TYPHOON_TX_PF_INSERT_VLAN	__constant_cpu_to_le32(0x00000010)
#define TYPHOON_TX_PF_IPSEC		__constant_cpu_to_le32(0x00000020)
#define TYPHOON_TX_PF_VLAN_PRIORITY	__constant_cpu_to_le32(0x00000040)
#define TYPHOON_TX_PF_UDP_CHKSUM	__constant_cpu_to_le32(0x00000080)
#define TYPHOON_TX_PF_PAD_FRAME		__constant_cpu_to_le32(0x00000100)
#define TYPHOON_TX_PF_RESERVED		__constant_cpu_to_le32(0x00000e00)
#define TYPHOON_TX_PF_VLAN_MASK		__constant_cpu_to_le32(0x0ffff000)
#define TYPHOON_TX_PF_INTERNAL		__constant_cpu_to_le32(0xf0000000)
#define TYPHOON_TX_PF_VLAN_TAG_SHIFT	12
} __attribute__ ((packed));

/* The TCP Segmentation offload option descriptor
 *
 * flags:	descriptor type
 * numDesc:	must be 1
 * mss_flags:	bits 0-11 (little endian) are MSS, 12 is first TSO descriptor
 *			13 is list TSO descriptor, set both if only one TSO
 * respAddrLo:	low bytes of address of the bytesTx field of this descriptor
 * bytesTx:	total number of bytes in this TSO request
 * status:	0 on completion
 */
struct tcpopt_desc {
	u8  flags;
	u8  numDesc;
	u16 mss_flags;
#define TYPHOON_TSO_FIRST		__constant_cpu_to_le16(0x1000)
#define TYPHOON_TSO_LAST		__constant_cpu_to_le16(0x2000)
	u32 respAddrLo;
	u32 bytesTx;
	u32 status;
} __attribute__ ((packed));

/* The IPSEC Offload descriptor
 *
 * flags:	descriptor type
 * numDesc:	must be 1
 * ipsecFlags:	bit 0: 0 -- generate IV, 1 -- use supplied IV
 * sa1, sa2:	Security Association IDs for this packet
 * reserved:	set to 0
 */
struct ipsec_desc {
	u8  flags;
	u8  numDesc;
	u16 ipsecFlags;
#define TYPHOON_IPSEC_GEN_IV	__constant_cpu_to_le16(0x0000)
#define TYPHOON_IPSEC_USE_IV	__constant_cpu_to_le16(0x0001)
	u32 sa1;
	u32 sa2;
	u32 reserved;
} __attribute__ ((packed));

/* The Typhoon receive descriptor (Updated by NIC)
 *
 * flags:         Descriptor type, error indication
 * numDesc:       Always zero
 * frameLen:      the size of the packet received
 * addr:          low 32 bytes of the virtual addr passed in for this buffer
 * addrHi:        high 32 bytes of the virtual addr passed in for this buffer
 * rxStatus:      Error if set in flags, otherwise result of offload processing
 * filterResults: results of filtering on packet, not used
 * ipsecResults:  Results of IPSEC processing
 * vlanTag:       the 801.2q TCI from the packet
 */
struct rx_desc {
	u8  flags;
	u8  numDesc;
	u16 frameLen;
	u32 addr;
	u32 addrHi;
	u32 rxStatus;
#define TYPHOON_RX_ERR_INTERNAL		__constant_cpu_to_le32(0x00000000)
#define TYPHOON_RX_ERR_FIFO_UNDERRUN	__constant_cpu_to_le32(0x00000001)
#define TYPHOON_RX_ERR_BAD_SSD		__constant_cpu_to_le32(0x00000002)
#define TYPHOON_RX_ERR_RUNT		__constant_cpu_to_le32(0x00000003)
#define TYPHOON_RX_ERR_CRC		__constant_cpu_to_le32(0x00000004)
#define TYPHOON_RX_ERR_OVERSIZE		__constant_cpu_to_le32(0x00000005)
#define TYPHOON_RX_ERR_ALIGN		__constant_cpu_to_le32(0x00000006)
#define TYPHOON_RX_ERR_DRIBBLE		__constant_cpu_to_le32(0x00000007)
#define TYPHOON_RX_PROTO_MASK		__constant_cpu_to_le32(0x00000003)
#define TYPHOON_RX_PROTO_UNKNOWN	__constant_cpu_to_le32(0x00000000)
#define TYPHOON_RX_PROTO_IP		__constant_cpu_to_le32(0x00000001)
#define TYPHOON_RX_PROTO_IPX		__constant_cpu_to_le32(0x00000002)
#define TYPHOON_RX_VLAN			__constant_cpu_to_le32(0x00000004)
#define TYPHOON_RX_IP_FRAG		__constant_cpu_to_le32(0x00000008)
#define TYPHOON_RX_IPSEC		__constant_cpu_to_le32(0x00000010)
#define TYPHOON_RX_IP_CHK_FAIL		__constant_cpu_to_le32(0x00000020)
#define TYPHOON_RX_TCP_CHK_FAIL		__constant_cpu_to_le32(0x00000040)
#define TYPHOON_RX_UDP_CHK_FAIL		__constant_cpu_to_le32(0x00000080)
#define TYPHOON_RX_IP_CHK_GOOD		__constant_cpu_to_le32(0x00000100)
#define TYPHOON_RX_TCP_CHK_GOOD		__constant_cpu_to_le32(0x00000200)
#define TYPHOON_RX_UDP_CHK_GOOD		__constant_cpu_to_le32(0x00000400)
	u16 filterResults;
#define TYPHOON_RX_FILTER_MASK		__constant_cpu_to_le16(0x7fff)
#define TYPHOON_RX_FILTERED		__constant_cpu_to_le16(0x8000)
	u16 ipsecResults;
#define TYPHOON_RX_OUTER_AH_GOOD	__constant_cpu_to_le16(0x0001)
#define TYPHOON_RX_OUTER_ESP_GOOD	__constant_cpu_to_le16(0x0002)
#define TYPHOON_RX_INNER_AH_GOOD	__constant_cpu_to_le16(0x0004)
#define TYPHOON_RX_INNER_ESP_GOOD	__constant_cpu_to_le16(0x0008)
#define TYPHOON_RX_OUTER_AH_FAIL	__constant_cpu_to_le16(0x0010)
#define TYPHOON_RX_OUTER_ESP_FAIL	__constant_cpu_to_le16(0x0020)
#define TYPHOON_RX_INNER_AH_FAIL	__constant_cpu_to_le16(0x0040)
#define TYPHOON_RX_INNER_ESP_FAIL	__constant_cpu_to_le16(0x0080)
#define TYPHOON_RX_UNKNOWN_SA		__constant_cpu_to_le16(0x0100)
#define TYPHOON_RX_ESP_FORMAT_ERR	__constant_cpu_to_le16(0x0200)
	u32 vlanTag;
} __attribute__ ((packed));

/* The Typhoon free buffer descriptor, used to give a buffer to the NIC
 *
 * physAddr:    low 32 bits of the bus address of the buffer
 * physAddrHi:  high 32 bits of the bus address of the buffer, always zero
 * virtAddr:    low 32 bits of the skb address
 * virtAddrHi:  high 32 bits of the skb address, always zero
 *
 * the virt* address is basically two 32 bit cookies, just passed back
 * from the NIC
 */
struct rx_free {
	u32 physAddr;
	u32 physAddrHi;
	u32 virtAddr;
	u32 virtAddrHi;
} __attribute__ ((packed));

/* The Typhoon command descriptor, used for commands and responses
 *
 * flags:   descriptor type
 * numDesc: number of descriptors following in this command/response,
 *				ie, zero for a one descriptor command
 * cmd:     the command
 * seqNo:   sequence number (unused)
 * parm1:   use varies by command
 * parm2:   use varies by command
 * parm3:   use varies by command
 */
struct cmd_desc {
	u8  flags;
	u8  numDesc;
	u16 cmd;
#define TYPHOON_CMD_TX_ENABLE		__constant_cpu_to_le16(0x0001)
#define TYPHOON_CMD_TX_DISABLE		__constant_cpu_to_le16(0x0002)
#define TYPHOON_CMD_RX_ENABLE		__constant_cpu_to_le16(0x0003)
#define TYPHOON_CMD_RX_DISABLE		__constant_cpu_to_le16(0x0004)
#define TYPHOON_CMD_SET_RX_FILTER	__constant_cpu_to_le16(0x0005)
#define TYPHOON_CMD_READ_STATS		__constant_cpu_to_le16(0x0007)
#define TYPHOON_CMD_XCVR_SELECT		__constant_cpu_to_le16(0x0013)
#define TYPHOON_CMD_SET_MAX_PKT_SIZE	__constant_cpu_to_le16(0x001a)
#define TYPHOON_CMD_READ_MEDIA_STATUS	__constant_cpu_to_le16(0x001b)
#define TYPHOON_CMD_GOTO_SLEEP		__constant_cpu_to_le16(0x0023)
#define TYPHOON_CMD_SET_MULTICAST_HASH	__constant_cpu_to_le16(0x0025)
#define TYPHOON_CMD_SET_MAC_ADDRESS	__constant_cpu_to_le16(0x0026)
#define TYPHOON_CMD_READ_MAC_ADDRESS	__constant_cpu_to_le16(0x0027)
#define TYPHOON_CMD_VLAN_TYPE_WRITE	__constant_cpu_to_le16(0x002b)
#define TYPHOON_CMD_CREATE_SA		__constant_cpu_to_le16(0x0034)
#define TYPHOON_CMD_DELETE_SA		__constant_cpu_to_le16(0x0035)
#define TYPHOON_CMD_READ_VERSIONS	__constant_cpu_to_le16(0x0043)
#define TYPHOON_CMD_IRQ_COALESCE_CTRL	__constant_cpu_to_le16(0x0045)
#define TYPHOON_CMD_ENABLE_WAKE_EVENTS	__constant_cpu_to_le16(0x0049)
#define TYPHOON_CMD_SET_OFFLOAD_TASKS	__constant_cpu_to_le16(0x004f)
#define TYPHOON_CMD_HELLO_RESP		__constant_cpu_to_le16(0x0057)
#define TYPHOON_CMD_HALT		__constant_cpu_to_le16(0x005d)
#define TYPHOON_CMD_READ_IPSEC_INFO	__constant_cpu_to_le16(0x005e)
#define TYPHOON_CMD_GET_IPSEC_ENABLE	__constant_cpu_to_le16(0x0067)
#define TYPHOON_CMD_GET_CMD_LVL		__constant_cpu_to_le16(0x0069)
	u16 seqNo;
	u16 parm1;
	u32 parm2;
	u32 parm3;
} __attribute__ ((packed));

/* The Typhoon response descriptor, see command descriptor for details
 */
struct resp_desc {
	u8  flags;
	u8  numDesc;
	u16 cmd;
	u16 seqNo;
	u16 parm1;
	u32 parm2;
	u32 parm3;
} __attribute__ ((packed));

#define INIT_COMMAND_NO_RESPONSE(x, command)				\
	do { struct cmd_desc *_ptr = (x);				\
		memset(_ptr, 0, sizeof(struct cmd_desc));		\
		_ptr->flags = TYPHOON_CMD_DESC | TYPHOON_DESC_VALID;	\
		_ptr->cmd = command;					\
	} while(0)

/* We set seqNo to 1 if we're expecting a response from this command */
#define INIT_COMMAND_WITH_RESPONSE(x, command)				\
	do { struct cmd_desc *_ptr = (x);				\
		memset(_ptr, 0, sizeof(struct cmd_desc));		\
		_ptr->flags = TYPHOON_CMD_RESPOND | TYPHOON_CMD_DESC;	\
		_ptr->flags |= TYPHOON_DESC_VALID; 			\
		_ptr->cmd = command;					\
		_ptr->seqNo = 1;					\
	} while(0)

/* TYPHOON_CMD_SET_RX_FILTER filter bits (cmd.parm1)
 */
#define TYPHOON_RX_FILTER_DIRECTED	__constant_cpu_to_le16(0x0001)
#define TYPHOON_RX_FILTER_ALL_MCAST	__constant_cpu_to_le16(0x0002)
#define TYPHOON_RX_FILTER_BROADCAST	__constant_cpu_to_le16(0x0004)
#define TYPHOON_RX_FILTER_PROMISCOUS	__constant_cpu_to_le16(0x0008)
#define TYPHOON_RX_FILTER_MCAST_HASH	__constant_cpu_to_le16(0x0010)

/* TYPHOON_CMD_READ_STATS response format
 */
struct stats_resp {
	u8  flags;
	u8  numDesc;
	u16 cmd;
	u16 seqNo;
	u16 unused;
	u32 txPackets;
	u64 txBytes;
	u32 txDeferred;
	u32 txLateCollisions;
	u32 txCollisions;
	u32 txCarrierLost;
	u32 txMultipleCollisions;
	u32 txExcessiveCollisions;
	u32 txFifoUnderruns;
	u32 txMulticastTxOverflows;
	u32 txFiltered;
	u32 rxPacketsGood;
	u64 rxBytesGood;
	u32 rxFifoOverruns;
	u32 BadSSD;
	u32 rxCrcErrors;
	u32 rxOversized;
	u32 rxBroadcast;
	u32 rxMulticast;
	u32 rxOverflow;
	u32 rxFiltered;
	u32 linkStatus;
#define TYPHOON_LINK_STAT_MASK		__constant_cpu_to_le32(0x00000001)
#define TYPHOON_LINK_GOOD		__constant_cpu_to_le32(0x00000001)
#define TYPHOON_LINK_BAD		__constant_cpu_to_le32(0x00000000)
#define TYPHOON_LINK_SPEED_MASK		__constant_cpu_to_le32(0x00000002)
#define TYPHOON_LINK_100MBPS		__constant_cpu_to_le32(0x00000002)
#define TYPHOON_LINK_10MBPS		__constant_cpu_to_le32(0x00000000)
#define TYPHOON_LINK_DUPLEX_MASK	__constant_cpu_to_le32(0x00000004)
#define TYPHOON_LINK_FULL_DUPLEX	__constant_cpu_to_le32(0x00000004)
#define TYPHOON_LINK_HALF_DUPLEX	__constant_cpu_to_le32(0x00000000)
	u32 unused2;
	u32 unused3;
} __attribute__ ((packed));

/* TYPHOON_CMD_XCVR_SELECT xcvr values (resp.parm1)
 */
#define TYPHOON_XCVR_10HALF	__constant_cpu_to_le16(0x0000)
#define TYPHOON_XCVR_10FULL	__constant_cpu_to_le16(0x0001)
#define TYPHOON_XCVR_100HALF	__constant_cpu_to_le16(0x0002)
#define TYPHOON_XCVR_100FULL	__constant_cpu_to_le16(0x0003)
#define TYPHOON_XCVR_AUTONEG	__constant_cpu_to_le16(0x0004)

/* TYPHOON_CMD_READ_MEDIA_STATUS (resp.parm1)
 */
#define TYPHOON_MEDIA_STAT_CRC_STRIP_DISABLE	__constant_cpu_to_le16(0x0004)
#define TYPHOON_MEDIA_STAT_COLLISION_DETECT	__constant_cpu_to_le16(0x0010)
#define TYPHOON_MEDIA_STAT_CARRIER_SENSE	__constant_cpu_to_le16(0x0020)
#define TYPHOON_MEDIA_STAT_POLARITY_REV		__constant_cpu_to_le16(0x0400)
#define TYPHOON_MEDIA_STAT_NO_LINK		__constant_cpu_to_le16(0x0800)

/* TYPHOON_CMD_SET_MULTICAST_HASH enable values (cmd.parm1)
 */
#define TYPHOON_MCAST_HASH_DISABLE	__constant_cpu_to_le16(0x0000)
#define TYPHOON_MCAST_HASH_ENABLE	__constant_cpu_to_le16(0x0001)
#define TYPHOON_MCAST_HASH_SET		__constant_cpu_to_le16(0x0002)

/* TYPHOON_CMD_CREATE_SA descriptor and settings
 */
struct sa_descriptor {
	u8  flags;
	u8  numDesc;
	u16 cmd;
	u16 seqNo;
	u16 mode;
#define TYPHOON_SA_MODE_NULL		__constant_cpu_to_le16(0x0000)
#define TYPHOON_SA_MODE_AH		__constant_cpu_to_le16(0x0001)
#define TYPHOON_SA_MODE_ESP		__constant_cpu_to_le16(0x0002)
	u8  hashFlags;
#define TYPHOON_SA_HASH_ENABLE		0x01
#define TYPHOON_SA_HASH_SHA1		0x02
#define TYPHOON_SA_HASH_MD5		0x04
	u8  direction;
#define TYPHOON_SA_DIR_RX		0x00
#define TYPHOON_SA_DIR_TX		0x01
	u8  encryptionFlags;
#define TYPHOON_SA_ENCRYPT_ENABLE	0x01
#define TYPHOON_SA_ENCRYPT_DES		0x02
#define TYPHOON_SA_ENCRYPT_3DES		0x00
#define TYPHOON_SA_ENCRYPT_3DES_2KEY	0x00
#define TYPHOON_SA_ENCRYPT_3DES_3KEY	0x04
#define TYPHOON_SA_ENCRYPT_CBC		0x08
#define TYPHOON_SA_ENCRYPT_ECB		0x00
	u8  specifyIndex;
#define TYPHOON_SA_SPECIFY_INDEX	0x01
#define TYPHOON_SA_GENERATE_INDEX	0x00
	u32 SPI;
	u32 destAddr;
	u32 destMask;
	u8  integKey[20];
	u8  confKey[24];
	u32 index;
	u32 unused;
	u32 unused2;
} __attribute__ ((packed));

/* TYPHOON_CMD_SET_OFFLOAD_TASKS bits (cmd.parm2 (Tx) & cmd.parm3 (Rx))
 * This is all for IPv4.
 */
#define TYPHOON_OFFLOAD_TCP_CHKSUM	__constant_cpu_to_le32(0x00000002)
#define TYPHOON_OFFLOAD_UDP_CHKSUM	__constant_cpu_to_le32(0x00000004)
#define TYPHOON_OFFLOAD_IP_CHKSUM	__constant_cpu_to_le32(0x00000008)
#define TYPHOON_OFFLOAD_IPSEC		__constant_cpu_to_le32(0x00000010)
#define TYPHOON_OFFLOAD_BCAST_THROTTLE	__constant_cpu_to_le32(0x00000020)
#define TYPHOON_OFFLOAD_DHCP_PREVENT	__constant_cpu_to_le32(0x00000040)
#define TYPHOON_OFFLOAD_VLAN		__constant_cpu_to_le32(0x00000080)
#define TYPHOON_OFFLOAD_FILTERING	__constant_cpu_to_le32(0x00000100)
#define TYPHOON_OFFLOAD_TCP_SEGMENT	__constant_cpu_to_le32(0x00000200)

/* TYPHOON_CMD_ENABLE_WAKE_EVENTS bits (cmd.parm1)
 */
#define TYPHOON_WAKE_MAGIC_PKT		__constant_cpu_to_le16(0x01)
#define TYPHOON_WAKE_LINK_EVENT		__constant_cpu_to_le16(0x02)
#define TYPHOON_WAKE_ICMP_ECHO		__constant_cpu_to_le16(0x04)
#define TYPHOON_WAKE_ARP		__constant_cpu_to_le16(0x08)

/* These are used to load the firmware image on the NIC
 */
struct typhoon_file_header {
	u8  tag[8];
	u32 version;
	u32 numSections;
	u32 startAddr;
	u32 hmacDigest[5];
} __attribute__ ((packed));

struct typhoon_section_header {
	u32 len;
	u16 checksum;
	u16 reserved;
	u32 startAddr;
} __attribute__ ((packed));

/* The Typhoon Register offsets
 */
#define TYPHOON_REG_SOFT_RESET			0x00
#define TYPHOON_REG_INTR_STATUS			0x04
#define TYPHOON_REG_INTR_ENABLE			0x08
#define TYPHOON_REG_INTR_MASK			0x0c
#define TYPHOON_REG_SELF_INTERRUPT		0x10
#define TYPHOON_REG_HOST2ARM7			0x14
#define TYPHOON_REG_HOST2ARM6			0x18
#define TYPHOON_REG_HOST2ARM5			0x1c
#define TYPHOON_REG_HOST2ARM4			0x20
#define TYPHOON_REG_HOST2ARM3			0x24
#define TYPHOON_REG_HOST2ARM2			0x28
#define TYPHOON_REG_HOST2ARM1			0x2c
#define TYPHOON_REG_HOST2ARM0			0x30
#define TYPHOON_REG_ARM2HOST3			0x34
#define TYPHOON_REG_ARM2HOST2			0x38
#define TYPHOON_REG_ARM2HOST1			0x3c
#define TYPHOON_REG_ARM2HOST0			0x40

#define TYPHOON_REG_BOOT_DATA_LO		TYPHOON_REG_HOST2ARM5
#define TYPHOON_REG_BOOT_DATA_HI		TYPHOON_REG_HOST2ARM4
#define TYPHOON_REG_BOOT_DEST_ADDR		TYPHOON_REG_HOST2ARM3
#define TYPHOON_REG_BOOT_CHECKSUM		TYPHOON_REG_HOST2ARM2
#define TYPHOON_REG_BOOT_LENGTH			TYPHOON_REG_HOST2ARM1

#define TYPHOON_REG_DOWNLOAD_BOOT_ADDR		TYPHOON_REG_HOST2ARM1
#define TYPHOON_REG_DOWNLOAD_HMAC_0		TYPHOON_REG_HOST2ARM2
#define TYPHOON_REG_DOWNLOAD_HMAC_1		TYPHOON_REG_HOST2ARM3
#define TYPHOON_REG_DOWNLOAD_HMAC_2		TYPHOON_REG_HOST2ARM4
#define TYPHOON_REG_DOWNLOAD_HMAC_3		TYPHOON_REG_HOST2ARM5
#define TYPHOON_REG_DOWNLOAD_HMAC_4		TYPHOON_REG_HOST2ARM6

#define TYPHOON_REG_BOOT_RECORD_ADDR_HI		TYPHOON_REG_HOST2ARM2
#define TYPHOON_REG_BOOT_RECORD_ADDR_LO		TYPHOON_REG_HOST2ARM1

#define TYPHOON_REG_TX_LO_READY			TYPHOON_REG_HOST2ARM3
#define TYPHOON_REG_CMD_READY			TYPHOON_REG_HOST2ARM2
#define TYPHOON_REG_TX_HI_READY			TYPHOON_REG_HOST2ARM1

#define TYPHOON_REG_COMMAND			TYPHOON_REG_HOST2ARM0
#define TYPHOON_REG_HEARTBEAT			TYPHOON_REG_ARM2HOST3
#define TYPHOON_REG_STATUS			TYPHOON_REG_ARM2HOST0

/* 3XP Reset values (TYPHOON_REG_SOFT_RESET)
 */
#define TYPHOON_RESET_ALL	0x7f
#define TYPHOON_RESET_NONE	0x00

/* 3XP irq bits (TYPHOON_REG_INTR{STATUS,ENABLE,MASK})
 *
 * Some of these came from OpenBSD, as the 3Com docs have it wrong
 * (INTR_SELF) or don't list it at all (INTR_*_ABORT)
 *
 * Enabling irqs on the Heartbeat reg (ArmToHost3) gets you an irq
 * about every 8ms, so don't do it.
 */
#define TYPHOON_INTR_HOST_INT		0x00000001
#define TYPHOON_INTR_ARM2HOST0		0x00000002
#define TYPHOON_INTR_ARM2HOST1		0x00000004
#define TYPHOON_INTR_ARM2HOST2		0x00000008
#define TYPHOON_INTR_ARM2HOST3		0x00000010
#define TYPHOON_INTR_DMA0		0x00000020
#define TYPHOON_INTR_DMA1		0x00000040
#define TYPHOON_INTR_DMA2		0x00000080
#define TYPHOON_INTR_DMA3		0x00000100
#define TYPHOON_INTR_MASTER_ABORT	0x00000200
#define TYPHOON_INTR_TARGET_ABORT	0x00000400
#define TYPHOON_INTR_SELF		0x00000800
#define TYPHOON_INTR_RESERVED		0xfffff000

#define TYPHOON_INTR_BOOTCMD		TYPHOON_INTR_ARM2HOST0

#define TYPHOON_INTR_ENABLE_ALL		0xffffffef
#define TYPHOON_INTR_ALL		0xffffffff
#define TYPHOON_INTR_NONE		0x00000000

/* The commands for the 3XP chip (TYPHOON_REG_COMMAND)
 */
#define TYPHOON_BOOTCMD_BOOT			0x00
#define TYPHOON_BOOTCMD_WAKEUP			0xfa
#define TYPHOON_BOOTCMD_DNLD_COMPLETE		0xfb
#define TYPHOON_BOOTCMD_SEG_AVAILABLE		0xfc
#define TYPHOON_BOOTCMD_RUNTIME_IMAGE		0xfd
#define TYPHOON_BOOTCMD_REG_BOOT_RECORD		0xff

/* 3XP Status values (TYPHOON_REG_STATUS)
 */
#define TYPHOON_STATUS_WAITING_FOR_BOOT		0x07
#define TYPHOON_STATUS_SECOND_INIT		0x08
#define TYPHOON_STATUS_RUNNING			0x09
#define TYPHOON_STATUS_WAITING_FOR_HOST		0x0d
#define TYPHOON_STATUS_WAITING_FOR_SEGMENT	0x10
#define TYPHOON_STATUS_SLEEPING			0x11
#define TYPHOON_STATUS_HALTED			0x14
