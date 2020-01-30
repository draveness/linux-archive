/* $Id: socket.h,v 1.10 2001/06/13 16:25:03 davem Exp $ */
#ifndef _ASM_SOCKET_H
#define _ASM_SOCKET_H

#include <asm/sockios.h>

/* For setsockopt(2) */
#define SOL_SOCKET	0xffff

#define SO_DEBUG	0x0001
#define SO_PASSCRED	0x0002
#define SO_REUSEADDR	0x0004
#define SO_KEEPALIVE	0x0008
#define SO_DONTROUTE	0x0010
#define SO_BROADCAST	0x0020
#define SO_PEERCRED	0x0040
#define SO_LINGER	0x0080
#define SO_OOBINLINE	0x0100
/* To add :#define SO_REUSEPORT 0x0200 */
#define SO_BSDCOMPAT    0x0400
#define SO_RCVLOWAT     0x0800
#define SO_SNDLOWAT     0x1000
#define SO_RCVTIMEO     0x2000
#define SO_SNDTIMEO     0x4000
#define SO_ACCEPTCONN	0x8000

/* wha!??? */
#define SO_DONTLINGER   (~SO_LINGER)  /* Older SunOS compat. hack */

#define SO_SNDBUF	0x1001
#define SO_RCVBUF	0x1002
#define SO_ERROR	0x1007
#define SO_TYPE		0x1008

/* Linux specific, keep the same. */
#define SO_NO_CHECK	0x000b
#define SO_PRIORITY	0x000c

#define SO_BINDTODEVICE 0x000d

#define SO_ATTACH_FILTER	0x001a
#define SO_DETACH_FILTER        0x001b

#define SO_PEERNAME		0x001c
#define SO_TIMESTAMP		0x001d
#define SCM_TIMESTAMP		SO_TIMESTAMP

#define SO_PEERSEC		0x001e

/* Security levels - as per NRL IPv6 - don't actually do anything */
#define SO_SECURITY_AUTHENTICATION		0x5001
#define SO_SECURITY_ENCRYPTION_TRANSPORT	0x5002
#define SO_SECURITY_ENCRYPTION_NETWORK		0x5004

/* Nast libc5 fixup - bletch */
#if defined(__KERNEL__)
/* Socket types. */
#define SOCK_STREAM	1		/* stream (connection) socket	*/
#define SOCK_DGRAM	2		/* datagram (conn.less) socket	*/
#define SOCK_RAW	3		/* raw socket			*/
#define SOCK_RDM	4		/* reliably-delivered message	*/
#define SOCK_SEQPACKET	5		/* sequential packet socket	*/
#define SOCK_PACKET	10		/* linux specific way of	*/
					/* getting packets at the dev	*/
					/* level.  For writing rarp and	*/
					/* other similar things on the	*/
					/* user level.			*/
#define	SOCK_MAX	(SOCK_PACKET+1)
#endif

#endif /* _ASM_SOCKET_H */
