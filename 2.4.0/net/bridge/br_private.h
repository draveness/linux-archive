/*
 *	Linux ethernet bridge
 *
 *	Authors:
 *	Lennert Buytenhek		<buytenh@gnu.org>
 *
 *	$Id: br_private.h,v 1.3 2000/05/05 02:17:17 davem Exp $
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#ifndef _BR_PRIVATE_H
#define _BR_PRIVATE_H

#include <linux/netdevice.h>
#include <linux/miscdevice.h>
#include <linux/if_bridge.h>
#include "br_private_timer.h"

#define BR_HASH_BITS 8
#define BR_HASH_SIZE (1 << BR_HASH_BITS)

#define BR_HOLD_TIME (1*HZ)

typedef struct bridge_id bridge_id;
typedef struct mac_addr mac_addr;
typedef __u16 port_id;

struct bridge_id
{
	unsigned char	prio[2];
	unsigned char	addr[6];
};

struct mac_addr
{
	unsigned char	addr[6];
	unsigned char	pad[2];
};

struct net_bridge_fdb_entry
{
	struct net_bridge_fdb_entry	*next_hash;
	struct net_bridge_fdb_entry	**pprev_hash;
	atomic_t			use_count;
	mac_addr			addr;
	struct net_bridge_port		*dst;
	unsigned long			ageing_timer;
	unsigned			is_local:1;
	unsigned			is_static:1;
};

struct net_bridge_port
{
	struct net_bridge_port		*next;
	struct net_bridge		*br;
	struct net_device		*dev;
	int				port_no;

	/* STP */
	port_id				port_id;
	int				state;
	int				path_cost;
	bridge_id			designated_root;
	int				designated_cost;
	bridge_id			designated_bridge;
	port_id				designated_port;
	unsigned			topology_change_ack:1;
	unsigned			config_pending:1;
	int				priority;

	struct br_timer			forward_delay_timer;
	struct br_timer			hold_timer;
	struct br_timer			message_age_timer;
};

struct net_bridge
{
	struct net_bridge		*next;
	rwlock_t			lock;
	struct net_bridge_port		*port_list;
	struct net_device		dev;
	struct net_device_stats		statistics;
	rwlock_t			hash_lock;
	struct net_bridge_fdb_entry	*hash[BR_HASH_SIZE];
	struct timer_list		tick;

	/* STP */
	bridge_id			designated_root;
	int				root_path_cost;
	int				root_port;
	int				max_age;
	int				hello_time;
	int				forward_delay;
	bridge_id			bridge_id;
	int				bridge_max_age;
	int				bridge_hello_time;
	int				bridge_forward_delay;
	unsigned			stp_enabled:1;
	unsigned			topology_change:1;
	unsigned			topology_change_detected:1;

	struct br_timer			hello_timer;
	struct br_timer			tcn_timer;
	struct br_timer			topology_change_timer;
	struct br_timer			gc_timer;

	int				ageing_time;
	int				gc_interval;
};

struct notifier_block br_device_notifier;
unsigned char bridge_ula[6];

/* br.c */
void br_dec_use_count(void);
void br_inc_use_count(void);

/* br_device.c */
void br_dev_setup(struct net_device *dev);

/* br_fdb.c */
void br_fdb_changeaddr(struct net_bridge_port *p,
		       unsigned char *newaddr);
void br_fdb_cleanup(struct net_bridge *br);
void br_fdb_delete_by_port(struct net_bridge *br,
			   struct net_bridge_port *p);
struct net_bridge_fdb_entry *br_fdb_get(struct net_bridge *br,
					unsigned char *addr);
void br_fdb_put(struct net_bridge_fdb_entry *ent);
int  br_fdb_get_entries(struct net_bridge *br,
			unsigned char *_buf,
			int maxnum,
			int offset);
void br_fdb_insert(struct net_bridge *br,
		   struct net_bridge_port *source,
		   unsigned char *addr,
		   int is_local);

/* br_forward.c */
void br_forward(struct net_bridge_port *to,
		struct sk_buff *skb);
void br_flood(struct net_bridge *br,
	      struct sk_buff *skb,
	      int clone);

/* br_if.c */
int br_add_bridge(char *name);
int br_del_bridge(char *name);
int br_add_if(struct net_bridge *br,
	      struct net_device *dev);
int br_del_if(struct net_bridge *br,
	      struct net_device *dev);
int br_get_bridge_ifindices(int *indices,
			    int num);
void br_get_port_ifindices(struct net_bridge *br,
			   int *ifindices);

/* br_input.c */
void br_handle_frame(struct sk_buff *skb);

/* br_ioctl.c */
void br_call_ioctl_atomic(void (*fn)(void));
int br_ioctl(struct net_bridge *br,
	     unsigned int cmd,
	     unsigned long arg0,
	     unsigned long arg1,
	     unsigned long arg2);
int br_ioctl_deviceless_stub(unsigned long arg);

/* br_stp.c */
int br_is_root_bridge(struct net_bridge *br);
struct net_bridge_port *br_get_port(struct net_bridge *br,
				    int port_no);
void br_init_port(struct net_bridge_port *p);
port_id br_make_port_id(struct net_bridge_port *p);
void br_become_designated_port(struct net_bridge_port *p);

/* br_stp_if.c */
void br_stp_enable_bridge(struct net_bridge *br);
void br_stp_disable_bridge(struct net_bridge *br);
void br_stp_enable_port(struct net_bridge_port *p);
void br_stp_disable_port(struct net_bridge_port *p);
void br_stp_recalculate_bridge_id(struct net_bridge *br);
void br_stp_set_bridge_priority(struct net_bridge *br,
				int newprio);
void br_stp_set_port_priority(struct net_bridge_port *p,
			      int newprio);
void br_stp_set_path_cost(struct net_bridge_port *p,
			  int path_cost);

/* br_stp_bpdu.c */
void br_stp_handle_bpdu(struct sk_buff *skb);

#endif
