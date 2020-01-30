/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		INET protocol dispatch tables.
 *
 * Version:	$Id: protocol.c,v 1.12 2000/10/03 07:29:00 anton Exp $
 *
 * Authors:	Ross Biro, <bir7@leland.Stanford.Edu>
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 * Fixes:
 *		Alan Cox	: Ahah! udp icmp errors don't work because
 *				  udp_err is never called!
 *		Alan Cox	: Added new fields for init and ready for
 *				  proper fragmentation (_NO_ 4K limits!)
 *		Richard Colella	: Hang on hash collision
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */

#include <asm/uaccess.h>
#include <asm/system.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/config.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/netdevice.h>
#include <linux/timer.h>
#include <linux/brlock.h>
#include <net/ip.h>
#include <net/protocol.h>
#include <net/tcp.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/icmp.h>
#include <net/udp.h>
#include <net/ipip.h>
#include <linux/igmp.h>

#define IPPROTO_PREVIOUS NULL

#ifdef CONFIG_IP_MULTICAST

static struct inet_protocol igmp_protocol = 
{
	igmp_rcv,		/* IGMP handler		*/
	NULL,			/* IGMP error control	*/
	IPPROTO_PREVIOUS,	/* next			*/
	IPPROTO_IGMP,		/* protocol ID		*/
	0,			/* copy			*/
	NULL,			/* data			*/
	"IGMP"			/* name			*/
};

#undef  IPPROTO_PREVIOUS
#define IPPROTO_PREVIOUS &igmp_protocol

#endif

static struct inet_protocol tcp_protocol = 
{
	tcp_v4_rcv,		/* TCP handler		*/
	tcp_v4_err,		/* TCP error control	*/  
	IPPROTO_PREVIOUS,
	IPPROTO_TCP,		/* protocol ID		*/
	0,			/* copy			*/
	NULL,			/* data			*/
	"TCP"			/* name			*/
};

#undef  IPPROTO_PREVIOUS
#define IPPROTO_PREVIOUS &tcp_protocol

static struct inet_protocol udp_protocol = 
{
	udp_rcv,		/* UDP handler		*/
	udp_err,		/* UDP error control	*/
	IPPROTO_PREVIOUS,	/* next			*/
	IPPROTO_UDP,		/* protocol ID		*/
	0,			/* copy			*/
	NULL,			/* data			*/
	"UDP"			/* name			*/
};

#undef  IPPROTO_PREVIOUS
#define IPPROTO_PREVIOUS &udp_protocol


static struct inet_protocol icmp_protocol = 
{
	icmp_rcv,		/* ICMP handler		*/
	NULL,			/* ICMP error control	*/
	IPPROTO_PREVIOUS,	/* next			*/
	IPPROTO_ICMP,		/* protocol ID		*/
	0,			/* copy			*/
	NULL,			/* data			*/
	"ICMP"			/* name			*/
};

#undef  IPPROTO_PREVIOUS
#define IPPROTO_PREVIOUS &icmp_protocol


struct inet_protocol *inet_protocol_base = IPPROTO_PREVIOUS;

struct inet_protocol *inet_protos[MAX_INET_PROTOS];

/*
 *	Add a protocol handler to the hash tables
 */

void inet_add_protocol(struct inet_protocol *prot)
{
	unsigned char hash;
	struct inet_protocol *p2;

	hash = prot->protocol & (MAX_INET_PROTOS - 1);
	br_write_lock_bh(BR_NETPROTO_LOCK);
	prot ->next = inet_protos[hash];
	inet_protos[hash] = prot;
	prot->copy = 0;

	/*
	 *	Set the copy bit if we need to. 
	 */
	 
	p2 = (struct inet_protocol *) prot->next;
	while(p2 != NULL) 
	{
		if (p2->protocol == prot->protocol) 
		{
			prot->copy = 1;
			break;
		}
		p2 = (struct inet_protocol *) p2->next;
	}
	br_write_unlock_bh(BR_NETPROTO_LOCK);
}

/*
 *	Remove a protocol from the hash tables.
 */
 
int inet_del_protocol(struct inet_protocol *prot)
{
	struct inet_protocol *p;
	struct inet_protocol *lp = NULL;
	unsigned char hash;

	hash = prot->protocol & (MAX_INET_PROTOS - 1);
	br_write_lock_bh(BR_NETPROTO_LOCK);
	if (prot == inet_protos[hash]) 
	{
		inet_protos[hash] = (struct inet_protocol *) inet_protos[hash]->next;
		br_write_unlock_bh(BR_NETPROTO_LOCK);
		return(0);
	}

	p = (struct inet_protocol *) inet_protos[hash];
	while(p != NULL) 
	{
		/*
		 * We have to worry if the protocol being deleted is
		 * the last one on the list, then we may need to reset
		 * someone's copied bit.
		 */
		if (p->next != NULL && p->next == prot) 
		{
			/*
			 * if we are the last one with this protocol and
			 * there is a previous one, reset its copy bit.
			 */
			if (p->copy == 0 && lp != NULL) 
				lp->copy = 0;
			p->next = prot->next;
			br_write_unlock_bh(BR_NETPROTO_LOCK);
			return(0);
		}
		if (p->next != NULL && p->next->protocol == prot->protocol) 
			lp = p;

		p = (struct inet_protocol *) p->next;
	}
	br_write_unlock_bh(BR_NETPROTO_LOCK);
	return(-1);
}
