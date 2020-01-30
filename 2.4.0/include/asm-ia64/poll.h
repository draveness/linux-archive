#ifndef _ASM_IA64_POLL_H
#define _ASM_IA64_POLL_H

/*
 * poll(2) bit definitions.  Chosen to be compatible with Linux/x86.
 *
 * Copyright (C) 1998, 1999 Hewlett-Packard Co
 * Copyright (C) 1998, 1999 David Mosberger-Tang <davidm@hpl.hp.com>
 */

#define POLLIN		0x0001
#define POLLPRI		0x0002
#define POLLOUT		0x0004
#define POLLERR		0x0008
#define POLLHUP		0x0010
#define POLLNVAL	0x0020

#define POLLRDNORM	0x0040
#define POLLRDBAND	0x0080
#define POLLWRNORM	0x0100
#define POLLWRBAND	0x0200
#define POLLMSG		0x0400

struct pollfd {
	int fd;
	short events;
	short revents;
};

#endif /* _ASM_IA64_POLL_H */
