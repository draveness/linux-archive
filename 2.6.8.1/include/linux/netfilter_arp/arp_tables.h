/*
 * 	Format of an ARP firewall descriptor
 *
 * 	src, tgt, src_mask, tgt_mask, arpop, arpop_mask are always stored in
 *	network byte order.
 * 	flags are stored in host byte order (of course).
 */

#ifndef _ARPTABLES_H
#define _ARPTABLES_H

#ifdef __KERNEL__
#include <linux/if.h>
#include <linux/types.h>
#include <linux/in.h>
#include <linux/if_arp.h>
#include <linux/skbuff.h>
#endif
#include <linux/compiler.h>
#include <linux/netfilter_arp.h>

#define ARPT_FUNCTION_MAXNAMELEN 30
#define ARPT_TABLE_MAXNAMELEN 32

#define ARPT_DEV_ADDR_LEN_MAX 16

struct arpt_devaddr_info {
	char addr[ARPT_DEV_ADDR_LEN_MAX];
	char mask[ARPT_DEV_ADDR_LEN_MAX];
};

/* Yes, Virginia, you have to zero the padding. */
struct arpt_arp {
	/* Source and target IP addr */
	struct in_addr src, tgt;
	/* Mask for src and target IP addr */
	struct in_addr smsk, tmsk;

	/* Device hw address length, src+target device addresses */
	u_int8_t arhln, arhln_mask;
	struct arpt_devaddr_info src_devaddr;
	struct arpt_devaddr_info tgt_devaddr;

	/* ARP operation code. */
	u_int16_t arpop, arpop_mask;

	/* ARP hardware address and protocol address format. */
	u_int16_t arhrd, arhrd_mask;
	u_int16_t arpro, arpro_mask;

	/* The protocol address length is only accepted if it is 4
	 * so there is no use in offering a way to do filtering on it.
	 */

	char iniface[IFNAMSIZ], outiface[IFNAMSIZ];
	unsigned char iniface_mask[IFNAMSIZ], outiface_mask[IFNAMSIZ];

	/* Flags word */
	u_int8_t flags;
	/* Inverse flags */
	u_int16_t invflags;
};

struct arpt_entry_target
{
	union {
		struct {
			u_int16_t target_size;

			/* Used by userspace */
			char name[ARPT_FUNCTION_MAXNAMELEN];
		} user;
		struct {
			u_int16_t target_size;

			/* Used inside the kernel */
			struct arpt_target *target;
		} kernel;

		/* Total length */
		u_int16_t target_size;
	} u;

	unsigned char data[0];
};

struct arpt_standard_target
{
	struct arpt_entry_target target;
	int verdict;
};

struct arpt_counters
{
	u_int64_t pcnt, bcnt;			/* Packet and byte counters */
};

/* Values for "flag" field in struct arpt_ip (general arp structure).
 * No flags defined yet.
 */
#define ARPT_F_MASK		0x00	/* All possible flag bits mask. */

/* Values for "inv" field in struct arpt_arp. */
#define ARPT_INV_VIA_IN		0x0001	/* Invert the sense of IN IFACE. */
#define ARPT_INV_VIA_OUT	0x0002	/* Invert the sense of OUT IFACE */
#define ARPT_INV_SRCIP		0x0004	/* Invert the sense of SRC IP. */
#define ARPT_INV_TGTIP		0x0008	/* Invert the sense of TGT IP. */
#define ARPT_INV_SRCDEVADDR	0x0010	/* Invert the sense of SRC DEV ADDR. */
#define ARPT_INV_TGTDEVADDR	0x0020	/* Invert the sense of TGT DEV ADDR. */
#define ARPT_INV_ARPOP		0x0040	/* Invert the sense of ARP OP. */
#define ARPT_INV_ARPHRD		0x0080	/* Invert the sense of ARP HRD. */
#define ARPT_INV_ARPPRO		0x0100	/* Invert the sense of ARP PRO. */
#define ARPT_INV_ARPHLN		0x0200	/* Invert the sense of ARP HLN. */
#define ARPT_INV_MASK		0x03FF	/* All possible flag bits mask. */

/* This structure defines each of the firewall rules.  Consists of 3
   parts which are 1) general ARP header stuff 2) match specific
   stuff 3) the target to perform if the rule matches */
struct arpt_entry
{
	struct arpt_arp arp;

	/* Size of arpt_entry + matches */
	u_int16_t target_offset;
	/* Size of arpt_entry + matches + target */
	u_int16_t next_offset;

	/* Back pointer */
	unsigned int comefrom;

	/* Packet and byte counters. */
	struct arpt_counters counters;

	/* The matches (if any), then the target. */
	unsigned char elems[0];
};

/*
 * New IP firewall options for [gs]etsockopt at the RAW IP level.
 * Unlike BSD Linux inherits IP options so you don't have to use a raw
 * socket for this. Instead we check rights in the calls.
 */
#define ARPT_BASE_CTL		96	/* base for firewall socket options */

#define ARPT_SO_SET_REPLACE		(ARPT_BASE_CTL)
#define ARPT_SO_SET_ADD_COUNTERS	(ARPT_BASE_CTL + 1)
#define ARPT_SO_SET_MAX			ARPT_SO_SET_ADD_COUNTERS

#define ARPT_SO_GET_INFO		(ARPT_BASE_CTL)
#define ARPT_SO_GET_ENTRIES		(ARPT_BASE_CTL + 1)
#define ARPT_SO_GET_MAX			ARPT_SO_GET_ENTRIES

/* CONTINUE verdict for targets */
#define ARPT_CONTINUE 0xFFFFFFFF

/* For standard target */
#define ARPT_RETURN (-NF_MAX_VERDICT - 1)

/* The argument to ARPT_SO_GET_INFO */
struct arpt_getinfo
{
	/* Which table: caller fills this in. */
	char name[ARPT_TABLE_MAXNAMELEN];

	/* Kernel fills these in. */
	/* Which hook entry points are valid: bitmask */
	unsigned int valid_hooks;

	/* Hook entry points: one per netfilter hook. */
	unsigned int hook_entry[NF_ARP_NUMHOOKS];

	/* Underflow points. */
	unsigned int underflow[NF_ARP_NUMHOOKS];

	/* Number of entries */
	unsigned int num_entries;

	/* Size of entries. */
	unsigned int size;
};

/* The argument to ARPT_SO_SET_REPLACE. */
struct arpt_replace
{
	/* Which table. */
	char name[ARPT_TABLE_MAXNAMELEN];

	/* Which hook entry points are valid: bitmask.  You can't
           change this. */
	unsigned int valid_hooks;

	/* Number of entries */
	unsigned int num_entries;

	/* Total size of new entries */
	unsigned int size;

	/* Hook entry points. */
	unsigned int hook_entry[NF_ARP_NUMHOOKS];

	/* Underflow points. */
	unsigned int underflow[NF_ARP_NUMHOOKS];

	/* Information about old entries: */
	/* Number of counters (must be equal to current number of entries). */
	unsigned int num_counters;
	/* The old entries' counters. */
	struct arpt_counters __user *counters;

	/* The entries (hang off end: not really an array). */
	struct arpt_entry entries[0];
};

/* The argument to ARPT_SO_ADD_COUNTERS. */
struct arpt_counters_info
{
	/* Which table. */
	char name[ARPT_TABLE_MAXNAMELEN];

	unsigned int num_counters;

	/* The counters (actually `number' of these). */
	struct arpt_counters counters[0];
};

/* The argument to ARPT_SO_GET_ENTRIES. */
struct arpt_get_entries
{
	/* Which table: user fills this in. */
	char name[ARPT_TABLE_MAXNAMELEN];

	/* User fills this in: total entry size. */
	unsigned int size;

	/* The entries. */
	struct arpt_entry entrytable[0];
};

/* Standard return verdict, or do jump. */
#define ARPT_STANDARD_TARGET ""
/* Error verdict. */
#define ARPT_ERROR_TARGET "ERROR"

/* Helper functions */
static __inline__ struct arpt_entry_target *arpt_get_target(struct arpt_entry *e)
{
	return (void *)e + e->target_offset;
}

/* fn returns 0 to continue iteration */
#define ARPT_ENTRY_ITERATE(entries, size, fn, args...)		\
({								\
	unsigned int __i;					\
	int __ret = 0;						\
	struct arpt_entry *__entry;				\
								\
	for (__i = 0; __i < (size); __i += __entry->next_offset) { \
		__entry = (void *)(entries) + __i;		\
								\
		__ret = fn(__entry , ## args);			\
		if (__ret != 0)					\
			break;					\
	}							\
	__ret;							\
})

/*
 *	Main firewall chains definitions and global var's definitions.
 */
#ifdef __KERNEL__

/* Registration hooks for targets. */
struct arpt_target
{
	struct list_head list;

	const char name[ARPT_FUNCTION_MAXNAMELEN];

	/* Returns verdict. */
	unsigned int (*target)(struct sk_buff **pskb,
			       unsigned int hooknum,
			       const struct net_device *in,
			       const struct net_device *out,
			       const void *targinfo,
			       void *userdata);

	/* Called when user tries to insert an entry of this type:
           hook_mask is a bitmask of hooks from which it can be
           called. */
	/* Should return true or false. */
	int (*checkentry)(const char *tablename,
			  const struct arpt_entry *e,
			  void *targinfo,
			  unsigned int targinfosize,
			  unsigned int hook_mask);

	/* Called when entry of this type deleted. */
	void (*destroy)(void *targinfo, unsigned int targinfosize);

	/* Set this to THIS_MODULE if you are a module, otherwise NULL */
	struct module *me;
};

extern int arpt_register_target(struct arpt_target *target);
extern void arpt_unregister_target(struct arpt_target *target);

/* Furniture shopping... */
struct arpt_table
{
	struct list_head list;

	/* A unique name... */
	char name[ARPT_TABLE_MAXNAMELEN];

	/* Seed table: copied in register_table */
	struct arpt_replace *table;

	/* What hooks you will enter on */
	unsigned int valid_hooks;

	/* Lock for the curtain */
	rwlock_t lock;

	/* Man behind the curtain... */
	struct arpt_table_info *private;

	/* Set this to THIS_MODULE if you are a module, otherwise NULL */
	struct module *me;
};

extern int arpt_register_table(struct arpt_table *table);
extern void arpt_unregister_table(struct arpt_table *table);
extern unsigned int arpt_do_table(struct sk_buff **pskb,
				  unsigned int hook,
				  const struct net_device *in,
				  const struct net_device *out,
				  struct arpt_table *table,
				  void *userdata);

#define ARPT_ALIGN(s) (((s) + (__alignof__(struct arpt_entry)-1)) & ~(__alignof__(struct arpt_entry)-1))
#endif /*__KERNEL__*/
#endif /* _ARPTABLES_H */
