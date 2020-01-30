/*
 * Copyright (c) 2000-2002 Silicon Graphics, Inc.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Further, this software is distributed without any warranty that it is
 * free of the rightful claim of any third person regarding infringement
 * or the like.  Any license provided herein, whether implied or
 * otherwise, applies only to this software file.  Patent licenses, if
 * any, provided herein do not apply to combinations of this program with
 * other software, or any other product whatsoever.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston MA 02111-1307, USA.
 *
 * Contact information: Silicon Graphics, Inc., 1600 Amphitheatre Pkwy,
 * Mountain View, CA  94043, or:
 *
 * http://www.sgi.com
 *
 * For further information regarding this notice, see:
 *
 * http://oss.sgi.com/projects/GenInfo/SGIGPLNoticeExplan/
 */
#ifndef __XFS_SUPPORT_SV_H__
#define __XFS_SUPPORT_SV_H__

#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/spinlock.h>

/*
 * Synchronisation variables.
 *
 * (Parameters "pri", "svf" and "rts" are not implemented)
 */

typedef struct sv_s {
	wait_queue_head_t waiters;
} sv_t;

#define SV_FIFO		0x0		/* sv_t is FIFO type */
#define SV_LIFO		0x2		/* sv_t is LIFO type */
#define SV_PRIO		0x4		/* sv_t is PRIO type */
#define SV_KEYED	0x6		/* sv_t is KEYED type */
#define SV_DEFAULT      SV_FIFO


static inline void _sv_wait(sv_t *sv, spinlock_t *lock, int state,
			     unsigned long timeout)
{
	DECLARE_WAITQUEUE(wait, current);

	add_wait_queue_exclusive(&sv->waiters, &wait);
	__set_current_state(state);
	spin_unlock(lock);

	schedule_timeout(timeout);

	remove_wait_queue(&sv->waiters, &wait);
}

#define init_sv(sv,type,name,flag) \
	init_waitqueue_head(&(sv)->waiters)
#define sv_init(sv,flag,name) \
	init_waitqueue_head(&(sv)->waiters)
#define sv_destroy(sv) \
	/*NOTHING*/
#define sv_wait(sv, pri, lock, s) \
	_sv_wait(sv, lock, TASK_UNINTERRUPTIBLE, MAX_SCHEDULE_TIMEOUT)
#define sv_wait_sig(sv, pri, lock, s)   \
	_sv_wait(sv, lock, TASK_INTERRUPTIBLE, MAX_SCHEDULE_TIMEOUT)
#define sv_timedwait(sv, pri, lock, s, svf, ts, rts) \
	_sv_wait(sv, lock, TASK_UNINTERRUPTIBLE, timespec_to_jiffies(ts))
#define sv_timedwait_sig(sv, pri, lock, s, svf, ts, rts) \
	_sv_wait(sv, lock, TASK_INTERRUPTIBLE, timespec_to_jiffies(ts))
#define sv_signal(sv) \
	wake_up(&(sv)->waiters)
#define sv_broadcast(sv) \
	wake_up_all(&(sv)->waiters)

#endif /* __XFS_SUPPORT_SV_H__ */
