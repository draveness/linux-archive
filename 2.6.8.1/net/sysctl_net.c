/* -*- linux-c -*-
 * sysctl_net.c: sysctl interface to net subsystem.
 *
 * Begun April 1, 1996, Mike Shaver.
 * Added /proc/sys/net directories for each protocol family. [MS]
 *
 * $Log: sysctl_net.c,v $
 * Revision 1.2  1996/05/08  20:24:40  shaver
 * Added bits for NET_BRIDGE and the NET_IPV4_ARP stuff and
 * NET_IPV4_IP_FORWARD.
 *
 *
 */

#include <linux/config.h>
#include <linux/mm.h>
#include <linux/sysctl.h>

#ifdef CONFIG_INET
extern struct ctl_table ipv4_table[];
#endif

extern struct ctl_table core_table[];

#ifdef CONFIG_NET
extern struct ctl_table ether_table[];
#endif

#ifdef CONFIG_TR
extern struct ctl_table tr_table[];
#endif

struct ctl_table net_table[] = {
	{
		.ctl_name	= NET_CORE,
		.procname	= "core",
		.mode		= 0555,
		.child		= core_table,
	},
#ifdef CONFIG_NET
	{
		.ctl_name	= NET_ETHER,
		.procname	= "ethernet",
		.mode		= 0555,
		.child		= ether_table,
	},
#endif
#ifdef CONFIG_INET
	{
		.ctl_name	= NET_IPV4,
		.procname	= "ipv4",
		.mode		= 0555,
		.child		= ipv4_table
	},
#endif
#ifdef CONFIG_TR
	{
		.ctl_name	= NET_TR,
		.procname	= "token-ring",
		.mode		= 0555,
		.child		= tr_table,
	},
#endif
	{ 0 },
};
