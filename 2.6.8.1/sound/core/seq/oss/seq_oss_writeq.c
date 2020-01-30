/*
 * OSS compatible sequencer driver
 *
 * seq_oss_writeq.c - write queue and sync
 *
 * Copyright (C) 1998,99 Takashi Iwai <tiwai@suse.de>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include "seq_oss_writeq.h"
#include "seq_oss_event.h"
#include "seq_oss_timer.h"
#include <sound/seq_oss_legacy.h>
#include "../seq_lock.h"
#include "../seq_clientmgr.h"


/*
 * create a write queue record
 */
seq_oss_writeq_t *
snd_seq_oss_writeq_new(seq_oss_devinfo_t *dp, int maxlen)
{
	seq_oss_writeq_t *q;
	snd_seq_client_pool_t pool;

	if ((q = snd_kcalloc(sizeof(*q), GFP_KERNEL)) == NULL)
		return NULL;
	q->dp = dp;
	q->maxlen = maxlen;
	spin_lock_init(&q->sync_lock);
	q->sync_event_put = 0;
	q->sync_time = 0;
	init_waitqueue_head(&q->sync_sleep);

	memset(&pool, 0, sizeof(pool));
	pool.client = dp->cseq;
	pool.output_pool = maxlen;
	pool.output_room = maxlen / 2;

	snd_seq_oss_control(dp, SNDRV_SEQ_IOCTL_SET_CLIENT_POOL, &pool);

	return q;
}

/*
 * delete the write queue
 */
void
snd_seq_oss_writeq_delete(seq_oss_writeq_t *q)
{
	snd_seq_oss_writeq_clear(q);	/* to be sure */
	kfree(q);
}


/*
 * reset the write queue
 */
void
snd_seq_oss_writeq_clear(seq_oss_writeq_t *q)
{
	snd_seq_remove_events_t reset;

	memset(&reset, 0, sizeof(reset));
	reset.remove_mode = SNDRV_SEQ_REMOVE_OUTPUT; /* remove all */
	snd_seq_oss_control(q->dp, SNDRV_SEQ_IOCTL_REMOVE_EVENTS, &reset);

	/* wake up sleepers if any */
	snd_seq_oss_writeq_wakeup(q, 0);
}

/*
 * wait until the write buffer has enough room
 */
int
snd_seq_oss_writeq_sync(seq_oss_writeq_t *q)
{
	seq_oss_devinfo_t *dp = q->dp;
	abstime_t time;
	unsigned long flags;

	time = snd_seq_oss_timer_cur_tick(dp->timer);
	if (q->sync_time >= time)
		return 0; /* already finished */

	if (! q->sync_event_put) {
		snd_seq_event_t ev;
		evrec_t *rec;

		/* put echoback event */
		memset(&ev, 0, sizeof(ev));
		ev.flags = 0;
		ev.type = SNDRV_SEQ_EVENT_ECHO;
		ev.time.tick = time;
		/* echo back to itself */
		snd_seq_oss_fill_addr(dp, &ev, dp->addr.client, dp->addr.port);
		rec = (evrec_t*)&ev.data;
		rec->t.code = SEQ_SYNCTIMER;
		rec->t.time = time;
		q->sync_event_put = 1;
		snd_seq_kernel_client_enqueue_blocking(dp->cseq, &ev, NULL, 0, 0);
	}

	spin_lock_irqsave(&q->sync_lock, flags);
	if (! q->sync_event_put) { /* echoback event has been received */
		spin_unlock_irqrestore(&q->sync_lock, flags);
		return 0;
	}
		
	/* wait for echo event */
	spin_unlock(&q->sync_lock);
	interruptible_sleep_on_timeout(&q->sync_sleep, HZ);
	spin_lock(&q->sync_lock);
	if (signal_pending(current)) {
		/* interrupted - return 0 to finish sync */
		q->sync_event_put = 0;
		spin_unlock_irqrestore(&q->sync_lock, flags);
		return 0;
	}
	spin_unlock_irqrestore(&q->sync_lock, flags);
	if (q->sync_time >= time)
		return 0;
	else
		return 1;
}

/*
 * wake up sync - echo event was catched
 */
void
snd_seq_oss_writeq_wakeup(seq_oss_writeq_t *q, abstime_t time)
{
	unsigned long flags;

	spin_lock_irqsave(&q->sync_lock, flags);
	q->sync_time = time;
	q->sync_event_put = 0;
	if (waitqueue_active(&q->sync_sleep)) {
		wake_up(&q->sync_sleep);
	}
	spin_unlock_irqrestore(&q->sync_lock, flags);
}


/*
 * return the unused pool size
 */
int
snd_seq_oss_writeq_get_free_size(seq_oss_writeq_t *q)
{
	snd_seq_client_pool_t pool;
	pool.client = q->dp->cseq;
	snd_seq_oss_control(q->dp, SNDRV_SEQ_IOCTL_GET_CLIENT_POOL, &pool);
	return pool.output_free;
}


/*
 * set output threshold size from ioctl
 */
void
snd_seq_oss_writeq_set_output(seq_oss_writeq_t *q, int val)
{
	snd_seq_client_pool_t pool;
	pool.client = q->dp->cseq;
	snd_seq_oss_control(q->dp, SNDRV_SEQ_IOCTL_GET_CLIENT_POOL, &pool);
	pool.output_room = val;
	snd_seq_oss_control(q->dp, SNDRV_SEQ_IOCTL_SET_CLIENT_POOL, &pool);
}

