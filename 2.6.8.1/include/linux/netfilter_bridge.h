#ifndef __LINUX_BRIDGE_NETFILTER_H
#define __LINUX_BRIDGE_NETFILTER_H

/* bridge-specific defines for netfilter. 
 */

#include <linux/config.h>
#include <linux/netfilter.h>
#if defined(__KERNEL__) && defined(CONFIG_BRIDGE_NETFILTER)
#include <asm/atomic.h>
#include <linux/if_ether.h>
#endif

/* Bridge Hooks */
/* After promisc drops, checksum checks. */
#define NF_BR_PRE_ROUTING	0
/* If the packet is destined for this box. */
#define NF_BR_LOCAL_IN		1
/* If the packet is destined for another interface. */
#define NF_BR_FORWARD		2
/* Packets coming from a local process. */
#define NF_BR_LOCAL_OUT		3
/* Packets about to hit the wire. */
#define NF_BR_POST_ROUTING	4
/* Not really a hook, but used for the ebtables broute table */
#define NF_BR_BROUTING		5
#define NF_BR_NUMHOOKS		6

#ifdef __KERNEL__

enum nf_br_hook_priorities {
	NF_BR_PRI_FIRST = INT_MIN,
	NF_BR_PRI_NAT_DST_BRIDGED = -300,
	NF_BR_PRI_FILTER_BRIDGED = -200,
	NF_BR_PRI_BRNF = 0,
	NF_BR_PRI_NAT_DST_OTHER = 100,
	NF_BR_PRI_FILTER_OTHER = 200,
	NF_BR_PRI_NAT_SRC = 300,
	NF_BR_PRI_LAST = INT_MAX,
};

#ifdef CONFIG_BRIDGE_NETFILTER

#define BRNF_PKT_TYPE			0x01
#define BRNF_BRIDGED_DNAT		0x02
#define BRNF_DONT_TAKE_PARENT		0x04
#define BRNF_BRIDGED			0x08
#define BRNF_NF_BRIDGE_PREROUTING	0x10

static inline
struct nf_bridge_info *nf_bridge_alloc(struct sk_buff *skb)
{
	struct nf_bridge_info **nf_bridge = &(skb->nf_bridge);

	if ((*nf_bridge = kmalloc(sizeof(**nf_bridge), GFP_ATOMIC)) != NULL) {
		atomic_set(&(*nf_bridge)->use, 1);
		(*nf_bridge)->mask = 0;
		(*nf_bridge)->physindev = (*nf_bridge)->physoutdev = NULL;
#if defined(CONFIG_VLAN_8021Q) || defined(CONFIG_VLAN_8021Q_MODULE)
		(*nf_bridge)->netoutdev = NULL;
#endif
	}

	return *nf_bridge;
}

/* Only used in br_forward.c */
static inline
void nf_bridge_maybe_copy_header(struct sk_buff *skb)
{
	if (skb->nf_bridge) {
		if (skb->protocol == __constant_htons(ETH_P_8021Q)) {
			memcpy(skb->data - 18, skb->nf_bridge->data, 18);
			skb_push(skb, 4);
		} else
			memcpy(skb->data - 16, skb->nf_bridge->data, 16);
	}
}

static inline
void nf_bridge_save_header(struct sk_buff *skb)
{
        int header_size = 16;

	if (skb->protocol == __constant_htons(ETH_P_8021Q))
		header_size = 18;

	memcpy(skb->nf_bridge->data, skb->data - header_size, header_size);
}

/* This is called by the IP fragmenting code and it ensures there is
 * enough room for the encapsulating header (if there is one). */
static inline
int nf_bridge_pad(struct sk_buff *skb)
{
	if (skb->protocol == __constant_htons(ETH_P_IP))
		return 0;
	if (skb->nf_bridge) {
		if (skb->protocol == __constant_htons(ETH_P_8021Q))
			return 4;
	}
	return 0;
}

struct bridge_skb_cb {
	union {
		__u32 ipv4;
	} daddr;
};
#endif /* CONFIG_BRIDGE_NETFILTER */

#endif /* __KERNEL__ */
#endif
