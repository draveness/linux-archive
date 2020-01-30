/*
 * w1_netlink.h
 *
 * Copyright (c) 2003 Evgeniy Polyakov <johnpol@2ka.mipt.ru>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#ifndef __W1_NETLINK_H
#define __W1_NETLINK_H

#include <asm/types.h>

#include "w1.h"

struct w1_netlink_msg 
{
	union
	{
		struct w1_reg_num 	id;
		__u64			w1_id;
	} id;
	__u64				val;
};

#ifdef __KERNEL__

void w1_netlink_send(struct w1_master *, struct w1_netlink_msg *);

#endif /* __KERNEL__ */
#endif /* __W1_NETLINK_H */
