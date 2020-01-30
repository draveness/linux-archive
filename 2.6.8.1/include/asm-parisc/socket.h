#ifndef _ASM_SOCKET_H
#define _ASM_SOCKET_H

#include <asm/sockios.h>

/* For setsockopt(2) */
#define SOL_SOCKET	0xffff

#define SO_DEBUG	0x0001
#define SO_REUSEADDR	0x0004
#define SO_KEEPALIVE	0x0008
#define SO_DONTROUTE	0x0010
#define SO_BROADCAST	0x0020
#define SO_LINGER	0x0080
#define SO_OOBINLINE	0x0100
/* To add :#define SO_REUSEPORT 0x0200 */
#define SO_SNDBUF	0x1001
#define SO_RCVBUF	0x1002
#define SO_SNDLOWAT	0x1003
#define SO_RCVLOWAT	0x1004
#define SO_SNDTIMEO	0x1005
#define SO_RCVTIMEO	0x1006
#define SO_ERROR	0x1007
#define SO_TYPE		0x1008
#define SO_PEERNAME	0x2000

#define SO_NO_CHECK	0x400b
#define SO_PRIORITY	0x400c
#define SO_BSDCOMPAT	0x400e
#define SO_PASSCRED	0x4010
#define SO_PEERCRED	0x4011
#define SO_TIMESTAMP	0x4012
#define SCM_TIMESTAMP	SO_TIMESTAMP

/* Security levels - as per NRL IPv6 - don't actually do anything */
#define SO_SECURITY_AUTHENTICATION		0x4016
#define SO_SECURITY_ENCRYPTION_TRANSPORT	0x4017
#define SO_SECURITY_ENCRYPTION_NETWORK		0x4018

#define SO_BINDTODEVICE	0x4019

/* Socket filtering */
#define SO_ATTACH_FILTER        0x401a
#define SO_DETACH_FILTER        0x401b

#define SO_ACCEPTCONN		0x401c

#define SO_PEERSEC		0x401d

#if defined(__KERNEL__)
#define SOCK_STREAM	1	/* stream (connection) socket	*/
#define SOCK_DGRAM	2	/* datagram (conn.less) socket	*/
#define SOCK_RAW	3	/* raw socket			*/
#define SOCK_RDM	4	/* reliably-delivered message	*/
#define SOCK_SEQPACKET	5	/* sequential packet socket	*/
#define SOCK_PACKET	10	/* linux specific way of	*/
				/* getting packets at the dev	*/
				/* level.  For writing rarp and	*/
				/* other similar things on the	*/
				/* user level.			*/
#define	SOCK_MAX	(SOCK_PACKET+1)
#endif

#endif /* _ASM_SOCKET_H */
