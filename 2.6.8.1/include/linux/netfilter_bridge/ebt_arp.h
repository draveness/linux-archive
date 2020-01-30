#ifndef __LINUX_BRIDGE_EBT_ARP_H
#define __LINUX_BRIDGE_EBT_ARP_H

#define EBT_ARP_OPCODE 0x01
#define EBT_ARP_HTYPE 0x02
#define EBT_ARP_PTYPE 0x04
#define EBT_ARP_SRC_IP 0x08
#define EBT_ARP_DST_IP 0x10
#define EBT_ARP_SRC_MAC 0x20
#define EBT_ARP_DST_MAC 0x40
#define EBT_ARP_MASK (EBT_ARP_OPCODE | EBT_ARP_HTYPE | EBT_ARP_PTYPE | \
   EBT_ARP_SRC_IP | EBT_ARP_DST_IP | EBT_ARP_SRC_MAC | EBT_ARP_DST_MAC)
#define EBT_ARP_MATCH "arp"

struct ebt_arp_info
{
	uint16_t htype;
	uint16_t ptype;
	uint16_t opcode;
	uint32_t saddr;
	uint32_t smsk;
	uint32_t daddr;
	uint32_t dmsk;
	unsigned char smaddr[ETH_ALEN];
	unsigned char smmsk[ETH_ALEN];
	unsigned char dmaddr[ETH_ALEN];
	unsigned char dmmsk[ETH_ALEN];
	uint8_t  bitmask;
	uint8_t  invflags;
};

#endif
