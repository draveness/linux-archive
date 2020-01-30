/*
 *  Timers abstract layer
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <sound/driver.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/moduleparam.h>
#include <sound/core.h>
#include <sound/timer.h>
#include <sound/control.h>
#include <sound/info.h>
#include <sound/minors.h>
#include <sound/initval.h>
#include <linux/kmod.h>
#ifdef CONFIG_KERNELD
#include <linux/kerneld.h>
#endif

#if !defined(CONFIG_SND_RTCTIMER) && !defined(CONFIG_SND_RTCTIMER_MODULE)
#define DEFAULT_TIMER_LIMIT 1
#else
#define DEFAULT_TIMER_LIMIT 2
#endif

static int timer_limit = DEFAULT_TIMER_LIMIT;
MODULE_AUTHOR("Jaroslav Kysela <perex@suse.cz>, Takashi Iwai <tiwai@suse.de>");
MODULE_DESCRIPTION("ALSA timer interface");
MODULE_LICENSE("GPL");
MODULE_CLASSES("{sound}");
module_param(timer_limit, int, 0444);
MODULE_PARM_DESC(timer_limit, "Maximum global timers in system.");

typedef struct {
	snd_timer_instance_t *timeri;
	int tread;			/* enhanced read with timestamps and events */
	unsigned long ticks;
	unsigned long overrun;
	int qhead;
	int qtail;
	int qused;
	int queue_size;
	snd_timer_read_t *queue;
	snd_timer_tread_t *tqueue;
	spinlock_t qlock;
	unsigned long last_resolution;
	unsigned int filter;
	struct timespec tstamp;		/* trigger tstamp */
	wait_queue_head_t qchange_sleep;
	struct fasync_struct *fasync;
} snd_timer_user_t;

/* list of timers */
static LIST_HEAD(snd_timer_list);

/* list of slave instances */
static LIST_HEAD(snd_timer_slave_list);

/* lock for slave active lists */
static spinlock_t slave_active_lock = SPIN_LOCK_UNLOCKED;

static DECLARE_MUTEX(register_mutex);

static int snd_timer_free(snd_timer_t *timer);
static int snd_timer_dev_free(snd_device_t *device);
static int snd_timer_dev_register(snd_device_t *device);
static int snd_timer_dev_unregister(snd_device_t *device);

static void snd_timer_reschedule(snd_timer_t * timer, unsigned long ticks_left);

/*
 * create a timer instance with the given owner string.
 * when timer is not NULL, increments the module counter
 */
static snd_timer_instance_t *snd_timer_instance_new(char *owner, snd_timer_t *timer)
{
	snd_timer_instance_t *timeri;
	timeri = snd_kcalloc(sizeof(snd_timer_instance_t), GFP_KERNEL);
	if (timeri == NULL)
		return NULL;
	timeri->owner = snd_kmalloc_strdup(owner, GFP_KERNEL);
	if (! timeri->owner) {
		kfree(timeri);
		return NULL;
	}
	INIT_LIST_HEAD(&timeri->open_list);
	INIT_LIST_HEAD(&timeri->active_list);
	INIT_LIST_HEAD(&timeri->ack_list);
	INIT_LIST_HEAD(&timeri->slave_list_head);
	INIT_LIST_HEAD(&timeri->slave_active_head);

	timeri->timer = timer;
	if (timer && timer->card && !try_module_get(timer->card->module)) {
		kfree(timeri->owner);
		kfree(timeri);
		return NULL;
	}

	return timeri;
}

/*
 * find a timer instance from the given timer id
 */
static snd_timer_t *snd_timer_find(snd_timer_id_t *tid)
{
	snd_timer_t *timer = NULL;
	struct list_head *p;

	list_for_each(p, &snd_timer_list) {
		timer = (snd_timer_t *)list_entry(p, snd_timer_t, device_list);

		if (timer->tmr_class != tid->dev_class)
			continue;
		if ((timer->tmr_class == SNDRV_TIMER_CLASS_CARD ||
		     timer->tmr_class == SNDRV_TIMER_CLASS_PCM) &&
		    (timer->card == NULL ||
		     timer->card->number != tid->card))
			continue;
		if (timer->tmr_device != tid->device)
			continue;
		if (timer->tmr_subdevice != tid->subdevice)
			continue;
		return timer;
	}
	return NULL;
}

#ifdef CONFIG_KMOD

static void snd_timer_request(snd_timer_id_t *tid)
{
	if (! current->fs->root)
		return;
	switch (tid->dev_class) {
	case SNDRV_TIMER_CLASS_GLOBAL:
		if (tid->device < timer_limit)
			request_module("snd-timer-%i", tid->device);
		break;
	case SNDRV_TIMER_CLASS_CARD:
	case SNDRV_TIMER_CLASS_PCM:
		if (tid->card < snd_ecards_limit)
			request_module("snd-card-%i", tid->card);
		break;
	default:
		break;
	}
}

#endif

/*
 * look for a master instance matching with the slave id of the given slave.
 * when found, relink the open_link of the slave.
 *
 * call this with register_mutex down.
 */
static void snd_timer_check_slave(snd_timer_instance_t *slave)
{
	snd_timer_t *timer;
	snd_timer_instance_t *master;
	struct list_head *p, *q;

	/* FIXME: it's really dumb to look up all entries.. */
	list_for_each(p, &snd_timer_list) {
		timer = (snd_timer_t *)list_entry(p, snd_timer_t, device_list);
		list_for_each(q, &timer->open_list_head) {
			master = (snd_timer_instance_t *)list_entry(q, snd_timer_instance_t, open_list);
			if (slave->slave_class == master->slave_class &&
			    slave->slave_id == master->slave_id) {
				list_del(&slave->open_list);
				list_add_tail(&slave->open_list, &master->slave_list_head);
				spin_lock_irq(&slave_active_lock);
				slave->master = master;
				slave->timer = master->timer;
				spin_unlock_irq(&slave_active_lock);
				return;
			}
		}
	}
}

/*
 * look for slave instances matching with the slave id of the given master.
 * when found, relink the open_link of slaves.
 *
 * call this with register_mutex down.
 */
static void snd_timer_check_master(snd_timer_instance_t *master)
{
	snd_timer_instance_t *slave;
	struct list_head *p, *n;

	/* check all pending slaves */
	list_for_each_safe(p, n, &snd_timer_slave_list) {
		slave = (snd_timer_instance_t *)list_entry(p, snd_timer_instance_t, open_list);
		if (slave->slave_class == master->slave_class &&
		    slave->slave_id == master->slave_id) {
			list_del(p);
			list_add_tail(p, &master->slave_list_head);
			spin_lock_irq(&slave_active_lock);
			slave->master = master;
			slave->timer = master->timer;
			if (slave->flags & SNDRV_TIMER_IFLG_RUNNING)
				list_add_tail(&slave->active_list, &master->slave_active_head);
			spin_unlock_irq(&slave_active_lock);
		}
	}
}

/*
 * open a timer instance
 * when opening a master, the slave id must be here given.
 */
int snd_timer_open(snd_timer_instance_t **ti,
		   char *owner, snd_timer_id_t *tid,
		   unsigned int slave_id)
{
	snd_timer_t *timer;
	snd_timer_instance_t *timeri = NULL;
	
	if (tid->dev_class == SNDRV_TIMER_CLASS_SLAVE) {
		/* open a slave instance */
		if (tid->dev_sclass <= SNDRV_TIMER_SCLASS_NONE ||
		    tid->dev_sclass > SNDRV_TIMER_SCLASS_OSS_SEQUENCER) {
			snd_printd("invalid slave class %i\n", tid->dev_sclass);
			return -EINVAL;
		}
		down(&register_mutex);
		timeri = snd_timer_instance_new(owner, NULL);
		timeri->slave_class = tid->dev_sclass;
		timeri->slave_id = tid->device;
		timeri->flags |= SNDRV_TIMER_IFLG_SLAVE;
		list_add_tail(&timeri->open_list, &snd_timer_slave_list);
		snd_timer_check_slave(timeri);
		up(&register_mutex);
		*ti = timeri;
		return 0;
	}

	/* open a master instance */
	down(&register_mutex);
	timer = snd_timer_find(tid);
#ifdef CONFIG_KMOD
	if (timer == NULL) {
		up(&register_mutex);
		snd_timer_request(tid);
		down(&register_mutex);
		timer = snd_timer_find(tid);
	}
#endif
	if (timer) {
		if (!list_empty(&timer->open_list_head)) {
			timeri = (snd_timer_instance_t *)list_entry(timer->open_list_head.next, snd_timer_instance_t, open_list);
			if (timeri->flags & SNDRV_TIMER_IFLG_EXCLUSIVE) {
				up(&register_mutex);
				return -EBUSY;
			}
		}
		timeri = snd_timer_instance_new(owner, timer);
		if (timeri) {
			timeri->slave_class = tid->dev_sclass;
			timeri->slave_id = slave_id;
			if (list_empty(&timer->open_list_head) && timer->hw.open)
				timer->hw.open(timer);
			list_add_tail(&timeri->open_list, &timer->open_list_head);
			snd_timer_check_master(timeri);
		}
	} else {
		up(&register_mutex);
		return -ENODEV;
	}
	up(&register_mutex);
	*ti = timeri;
	return 0;
}

static int _snd_timer_stop(snd_timer_instance_t * timeri, int keep_flag, enum sndrv_timer_event event);

/*
 * close a timer instance
 */
int snd_timer_close(snd_timer_instance_t * timeri)
{
	snd_timer_t *timer = NULL;
	struct list_head *p, *n;
	snd_timer_instance_t *slave;

	snd_assert(timeri != NULL, return -ENXIO);

	/* force to stop the timer */
	snd_timer_stop(timeri);

	if (timeri->flags & SNDRV_TIMER_IFLG_SLAVE) {
		/* wait, until the active callback is finished */
		spin_lock_irq(&slave_active_lock);
		while (timeri->flags & SNDRV_TIMER_IFLG_CALLBACK) {
			spin_unlock_irq(&slave_active_lock);
			udelay(10);
			spin_lock_irq(&slave_active_lock);
		}
		spin_unlock_irq(&slave_active_lock);
		down(&register_mutex);
		list_del(&timeri->open_list);
		up(&register_mutex);
	} else {
		timer = timeri->timer;
		/* wait, until the active callback is finished */
		spin_lock_irq(&timer->lock);
		while (timeri->flags & SNDRV_TIMER_IFLG_CALLBACK) {
			spin_unlock_irq(&timer->lock);
			udelay(10);
			spin_lock_irq(&timer->lock);
		}
		spin_unlock_irq(&timer->lock);
		down(&register_mutex);
		list_del(&timeri->open_list);
		if (timer && list_empty(&timer->open_list_head) && timer->hw.close)
			timer->hw.close(timer);
		/* remove slave links */
		list_for_each_safe(p, n, &timeri->slave_list_head) {
			slave = (snd_timer_instance_t *)list_entry(p, snd_timer_instance_t, open_list);
			spin_lock_irq(&slave_active_lock);
			_snd_timer_stop(slave, 1, SNDRV_TIMER_EVENT_RESOLUTION);
			list_del(p);
			list_add_tail(p, &snd_timer_slave_list);
			slave->master = NULL;
			slave->timer = NULL;
			spin_unlock_irq(&slave_active_lock);
		}
		up(&register_mutex);
	}
	if (timeri->private_free)
		timeri->private_free(timeri);
	if (timeri->owner)
		kfree(timeri->owner);
	kfree(timeri);
	if (timer && timer->card)
		module_put(timer->card->module);
	return 0;
}

unsigned long snd_timer_resolution(snd_timer_instance_t * timeri)
{
	snd_timer_t * timer;

	if (timeri == NULL)
		return 0;
	if ((timer = timeri->timer) != NULL) {
		if (timer->hw.c_resolution)
			return timer->hw.c_resolution(timer);
		return timer->hw.resolution;
	}
	return 0;
}

static void snd_timer_notify1(snd_timer_instance_t *ti, enum sndrv_timer_event event)
{
	snd_timer_t *timer;
	unsigned long flags;
	unsigned long resolution = 0;
	snd_timer_instance_t *ts;
	struct list_head *n;
	struct timespec tstamp;

	snd_timestamp_now(&tstamp, 1);
	snd_assert(event >= SNDRV_TIMER_EVENT_START && event <= SNDRV_TIMER_EVENT_PAUSE, return);
	if (event == SNDRV_TIMER_EVENT_START || event == SNDRV_TIMER_EVENT_CONTINUE)
		resolution = snd_timer_resolution(ti);
	if (ti->ccallback)
		ti->ccallback(ti, SNDRV_TIMER_EVENT_START, &tstamp, resolution);
	if (ti->flags & SNDRV_TIMER_IFLG_SLAVE)
		return;
	timer = ti->timer;
	if (timer == NULL)
		return;
	if (timer->hw.flags & SNDRV_TIMER_HW_SLAVE)
		return;
	spin_lock_irqsave(&timer->lock, flags);
	list_for_each(n, &ti->slave_active_head) {
		ts = (snd_timer_instance_t *)list_entry(n, snd_timer_instance_t, active_list);
		if (ts->ccallback)
			ts->ccallback(ti, event + 100, &tstamp, resolution);
	}
	spin_unlock_irqrestore(&timer->lock, flags);
}

static int snd_timer_start1(snd_timer_t *timer, snd_timer_instance_t *timeri, unsigned long sticks)
{
	list_del(&timeri->active_list);
	list_add_tail(&timeri->active_list, &timer->active_list_head);
	if (timer->running) {
		if (timer->hw.flags & SNDRV_TIMER_HW_SLAVE)
			goto __start_now;
		timer->flags |= SNDRV_TIMER_FLG_RESCHED;
		timeri->flags |= SNDRV_TIMER_IFLG_START;
		return 1;	/* delayed start */
	} else {
		timer->sticks = sticks;
		timer->hw.start(timer);
	      __start_now:
		timer->running++;
		timeri->flags |= SNDRV_TIMER_IFLG_RUNNING;
		return 0;
	}
}

static int snd_timer_start_slave(snd_timer_instance_t *timeri)
{
	unsigned long flags;

	spin_lock_irqsave(&slave_active_lock, flags);
	timeri->flags |= SNDRV_TIMER_IFLG_RUNNING;
	if (timeri->master)
		list_add_tail(&timeri->active_list, &timeri->master->slave_active_head);
	spin_unlock_irqrestore(&slave_active_lock, flags);
	return 1; /* delayed start */
}

/*
 *  start the timer instance
 */ 
int snd_timer_start(snd_timer_instance_t * timeri, unsigned int ticks)
{
	snd_timer_t *timer;
	int result = -EINVAL;
	unsigned long flags;

	if (timeri == NULL || ticks < 1)
		return -EINVAL;
	if (timeri->flags & SNDRV_TIMER_IFLG_SLAVE) {
		result = snd_timer_start_slave(timeri);
		snd_timer_notify1(timeri, SNDRV_TIMER_EVENT_START);
		return result;
	}
	timer = timeri->timer;
	if (timer == NULL)
		return -EINVAL;
	spin_lock_irqsave(&timer->lock, flags);
	timeri->ticks = timeri->cticks = ticks;
	timeri->pticks = 0;
	result = snd_timer_start1(timer, timeri, ticks);
	spin_unlock_irqrestore(&timer->lock, flags);
	snd_timer_notify1(timeri, SNDRV_TIMER_EVENT_START);
	return result;
}

static int _snd_timer_stop(snd_timer_instance_t * timeri, int keep_flag, enum sndrv_timer_event event)
{
	snd_timer_t *timer;
	unsigned long flags;

	snd_assert(timeri != NULL, return -ENXIO);

	if (timeri->flags & SNDRV_TIMER_IFLG_SLAVE) {
		if (!keep_flag) {
			spin_lock_irqsave(&slave_active_lock, flags);
			timeri->flags &= ~SNDRV_TIMER_IFLG_RUNNING;
			spin_unlock_irqrestore(&slave_active_lock, flags);
		}
		goto __end;
	}
	timer = timeri->timer;
	if (!timer)
		return -EINVAL;
	spin_lock_irqsave(&timer->lock, flags);
	list_del_init(&timeri->ack_list);
	list_del_init(&timeri->active_list);
	if ((timeri->flags & SNDRV_TIMER_IFLG_RUNNING) &&
	    !(--timer->running)) {
		timer->hw.stop(timer);
		if (timer->flags & SNDRV_TIMER_FLG_RESCHED) {
			timer->flags &= ~SNDRV_TIMER_FLG_RESCHED;
			snd_timer_reschedule(timer, 0);
			if (timer->flags & SNDRV_TIMER_FLG_CHANGE) {
				timer->flags &= ~SNDRV_TIMER_FLG_CHANGE;
				timer->hw.start(timer);
			}
		}
	}
	if (!keep_flag)
		timeri->flags &= ~(SNDRV_TIMER_IFLG_RUNNING|SNDRV_TIMER_IFLG_START);
	spin_unlock_irqrestore(&timer->lock, flags);
      __end:
	if (event != SNDRV_TIMER_EVENT_RESOLUTION)
		snd_timer_notify1(timeri, event);
	return 0;
}

/*
 * stop the timer instance.
 *
 * do not call this from the timer callback!
 */
int snd_timer_stop(snd_timer_instance_t * timeri)
{
	snd_timer_t *timer;
	unsigned long flags;
	int err;

	err = _snd_timer_stop(timeri, 0, SNDRV_TIMER_EVENT_STOP);
	if (err < 0)
		return err;
	timer = timeri->timer;
	spin_lock_irqsave(&timer->lock, flags);
	timeri->cticks = timeri->ticks;
	timeri->pticks = 0;
	spin_unlock_irqrestore(&timer->lock, flags);
	return 0;
}

/*
 * start again..  the tick is kept.
 */
int snd_timer_continue(snd_timer_instance_t * timeri)
{
	snd_timer_t *timer;
	int result = -EINVAL;
	unsigned long flags;

	if (timeri == NULL)
		return result;
	if (timeri->flags & SNDRV_TIMER_IFLG_SLAVE)
		return snd_timer_start_slave(timeri);
	timer = timeri->timer;
	if (! timer)
		return -EINVAL;
	spin_lock_irqsave(&timer->lock, flags);
	if (!timeri->cticks)
		timeri->cticks = 1;
	timeri->pticks = 0;
	result = snd_timer_start1(timer, timeri, timer->sticks);
	spin_unlock_irqrestore(&timer->lock, flags);
	snd_timer_notify1(timeri, SNDRV_TIMER_EVENT_CONTINUE);
	return result;
}

/*
 * pause.. remember the ticks left
 */
int snd_timer_pause(snd_timer_instance_t * timeri)
{
	return _snd_timer_stop(timeri, 0, SNDRV_TIMER_EVENT_PAUSE);
}

/*
 * reschedule the timer
 *
 * start pending instances and check the scheduling ticks.
 * when the scheduling ticks is changed set CHANGE flag to reprogram the timer.
 */
static void snd_timer_reschedule(snd_timer_t * timer, unsigned long ticks_left)
{
	snd_timer_instance_t *ti;
	unsigned long ticks = ~0UL;
	struct list_head *p;

	list_for_each(p, &timer->active_list_head) {
		ti = (snd_timer_instance_t *)list_entry(p, snd_timer_instance_t, active_list);
		if (ti->flags & SNDRV_TIMER_IFLG_START) {
			ti->flags &= ~SNDRV_TIMER_IFLG_START;
			ti->flags |= SNDRV_TIMER_IFLG_RUNNING;
			timer->running++;
		}
		if (ti->flags & SNDRV_TIMER_IFLG_RUNNING) {
			if (ticks > ti->cticks)
				ticks = ti->cticks;
		}
	}
	if (ticks == ~0UL) {
		timer->flags &= ~SNDRV_TIMER_FLG_RESCHED;
		return;
	}
	if (ticks > timer->hw.ticks)
		ticks = timer->hw.ticks;
	if (ticks_left != ticks)
		timer->flags |= SNDRV_TIMER_FLG_CHANGE;
	timer->sticks = ticks;
}

/*
 * timer tasklet
 *
 */
static void snd_timer_tasklet(unsigned long arg)
{
	snd_timer_t *timer = (snd_timer_t *) arg;
	snd_timer_instance_t *ti;
	struct list_head *p;
	unsigned long resolution, ticks;

	spin_lock(&timer->lock);
	/* now process all callbacks */
	while (!list_empty(&timer->sack_list_head)) {
		p = timer->sack_list_head.next;		/* get first item */
		ti = (snd_timer_instance_t *)list_entry(p, snd_timer_instance_t, ack_list);

		/* remove from ack_list and make empty */
		list_del_init(p);
		
		ticks = ti->pticks;
		ti->pticks = 0;
		resolution = ti->resolution;

		ti->flags |= SNDRV_TIMER_IFLG_CALLBACK;
		spin_unlock(&timer->lock);
		if (ti->callback)
			ti->callback(ti, resolution, ticks);
		spin_lock(&timer->lock);
		ti->flags &= ~SNDRV_TIMER_IFLG_CALLBACK;
	}
	spin_unlock(&timer->lock);
}

/*
 * timer interrupt
 *
 * ticks_left is usually equal to timer->sticks.
 *
 */
void snd_timer_interrupt(snd_timer_t * timer, unsigned long ticks_left)
{
	snd_timer_instance_t *ti, *ts;
	unsigned long resolution, ticks;
	struct list_head *p, *q, *n;
	int use_tasklet = 0;

	if (timer == NULL)
		return;

	spin_lock(&timer->lock);

	/* remember the current resolution */
	if (timer->hw.c_resolution)
		resolution = timer->hw.c_resolution(timer);
	else
		resolution = timer->hw.resolution;

	/* loop for all active instances
	 * here we cannot use list_for_each because the active_list of a processed
	 * instance is relinked to done_list_head before callback is called.
	 */
	list_for_each_safe(p, n, &timer->active_list_head) {
		ti = (snd_timer_instance_t *)list_entry(p, snd_timer_instance_t, active_list);
		if (!(ti->flags & SNDRV_TIMER_IFLG_RUNNING))
			continue;
		ti->pticks += ticks_left;
		ti->resolution = resolution;
		if (ti->cticks < ticks_left)
			ti->cticks = 0;
		else
			ti->cticks -= ticks_left;
		if (ti->cticks) /* not expired */
			continue;
		if (ti->flags & SNDRV_TIMER_IFLG_AUTO) {
			ti->cticks = ti->ticks;
		} else {
			ti->flags &= ~SNDRV_TIMER_IFLG_RUNNING;
			if (--timer->running)
				list_del(p);
		}
		if (list_empty(&ti->ack_list)) {
			if ((timer->hw.flags & SNDRV_TIMER_HW_TASKLET) ||
			    (ti->flags & SNDRV_TIMER_IFLG_FAST)) {
				list_add_tail(&ti->ack_list, &timer->ack_list_head);
			} else {
				list_add_tail(&ti->ack_list, &timer->sack_list_head);
			}
		}
		list_for_each(q, &ti->slave_active_head) {
			ts = (snd_timer_instance_t *)list_entry(q, snd_timer_instance_t, active_list);
			ts->pticks = ti->pticks;
			ts->resolution = resolution;
			if (list_empty(&ts->ack_list)) {
				if ((timer->hw.flags & SNDRV_TIMER_HW_TASKLET) ||
				    (ti->flags & SNDRV_TIMER_IFLG_FAST)) {
					list_add_tail(&ts->ack_list, &timer->ack_list_head);
				} else {
					list_add_tail(&ts->ack_list, &timer->sack_list_head);
				}
			}
		}
	}
	if (timer->flags & SNDRV_TIMER_FLG_RESCHED)
		snd_timer_reschedule(timer, ticks_left);
	if (timer->running) {
		if (timer->hw.flags & SNDRV_TIMER_HW_STOP) {
			timer->hw.stop(timer);
			timer->flags |= SNDRV_TIMER_FLG_CHANGE;
		}
		if (!(timer->hw.flags & SNDRV_TIMER_HW_AUTO) ||
		    (timer->flags & SNDRV_TIMER_FLG_CHANGE)) {
			/* restart timer */
			timer->flags &= ~SNDRV_TIMER_FLG_CHANGE;
			timer->hw.start(timer);
		}
	} else {
		timer->hw.stop(timer);
	}

	/* now process all fast callbacks */
	while (!list_empty(&timer->ack_list_head)) {
		p = timer->ack_list_head.next;		/* get first item */
		ti = (snd_timer_instance_t *)list_entry(p, snd_timer_instance_t, ack_list);
		
		/* remove from ack_list and make empty */
		list_del_init(p);
		
		ticks = ti->pticks;
		ti->pticks = 0;

		ti->flags |= SNDRV_TIMER_IFLG_CALLBACK;
		spin_unlock(&timer->lock);
		if (ti->callback)
			ti->callback(ti, resolution, ticks);
		spin_lock(&timer->lock);
		ti->flags &= ~SNDRV_TIMER_IFLG_CALLBACK;
	}

	/* do we have any slow callbacks? */
	use_tasklet = !list_empty(&timer->sack_list_head);
	spin_unlock(&timer->lock);

	if (use_tasklet)
		tasklet_hi_schedule(&timer->task_queue);
}

/*

 */

int snd_timer_new(snd_card_t *card, char *id, snd_timer_id_t *tid, snd_timer_t ** rtimer)
{
	snd_timer_t *timer;
	int err;
	static snd_device_ops_t ops = {
		.dev_free = snd_timer_dev_free,
		.dev_register = snd_timer_dev_register,
		.dev_unregister = snd_timer_dev_unregister
	};

	snd_assert(tid != NULL, return -EINVAL);
	snd_assert(rtimer != NULL, return -EINVAL);
	*rtimer = NULL;
	timer = snd_magic_kcalloc(snd_timer_t, 0, GFP_KERNEL);
	if (timer == NULL)
		return -ENOMEM;
	timer->tmr_class = tid->dev_class;
	timer->card = card;
	timer->tmr_device = tid->device;
	timer->tmr_subdevice = tid->subdevice;
	if (id)
		strlcpy(timer->id, id, sizeof(timer->id));
	INIT_LIST_HEAD(&timer->device_list);
	INIT_LIST_HEAD(&timer->open_list_head);
	INIT_LIST_HEAD(&timer->active_list_head);
	INIT_LIST_HEAD(&timer->ack_list_head);
	INIT_LIST_HEAD(&timer->sack_list_head);
	spin_lock_init(&timer->lock);
	tasklet_init(&timer->task_queue, snd_timer_tasklet, (unsigned long)timer);
	if (card != NULL) {
		if ((err = snd_device_new(card, SNDRV_DEV_TIMER, timer, &ops)) < 0) {
			snd_timer_free(timer);
			return err;
		}
	}
	*rtimer = timer;
	return 0;
}

static int snd_timer_free(snd_timer_t *timer)
{
	snd_assert(timer != NULL, return -ENXIO);
	if (timer->private_free)
		timer->private_free(timer);
	snd_magic_kfree(timer);
	return 0;
}

int snd_timer_dev_free(snd_device_t *device)
{
	snd_timer_t *timer = snd_magic_cast(snd_timer_t, device->device_data, return -ENXIO);
	return snd_timer_free(timer);
}

int snd_timer_dev_register(snd_device_t *dev)
{
	snd_timer_t *timer = snd_magic_cast(snd_timer_t, dev->device_data, return -ENXIO);
	snd_timer_t *timer1;
	struct list_head *p;

	snd_assert(timer != NULL && timer->hw.start != NULL && timer->hw.stop != NULL, return -ENXIO);
	if (!(timer->hw.flags & SNDRV_TIMER_HW_SLAVE) &&
	    !timer->hw.resolution && timer->hw.c_resolution == NULL)
	    	return -EINVAL;

	down(&register_mutex);
	list_for_each(p, &snd_timer_list) {
		timer1 = (snd_timer_t *)list_entry(p, snd_timer_t, device_list);
		if (timer1->tmr_class > timer->tmr_class)
			break;
		if (timer1->tmr_class < timer->tmr_class)
			continue;
		if (timer1->card && timer->card) {
			if (timer1->card->number > timer->card->number)
				break;
			if (timer1->card->number < timer->card->number)
				continue;
		}
		if (timer1->tmr_device > timer->tmr_device)
			break;
		if (timer1->tmr_device < timer->tmr_device)
			continue;
		if (timer1->tmr_subdevice > timer->tmr_subdevice)
			break;
		if (timer1->tmr_subdevice < timer->tmr_subdevice)
			continue;
		/* conflicts.. */
		up(&register_mutex);
		return -EBUSY;
	}
	list_add_tail(&timer->device_list, p);
	up(&register_mutex);
	return 0;
}

int snd_timer_unregister(snd_timer_t *timer)
{
	struct list_head *p, *n;
	snd_timer_instance_t *ti;

	snd_assert(timer != NULL, return -ENXIO);
	down(&register_mutex);
	if (! list_empty(&timer->open_list_head)) {
		snd_printk(KERN_WARNING "timer 0x%lx is busy?\n", (long)timer);
		list_for_each_safe(p, n, &timer->open_list_head) {
			list_del_init(p);
			ti = (snd_timer_instance_t *)list_entry(p, snd_timer_instance_t, open_list);
			ti->timer = NULL;
		}
	}
	list_del(&timer->device_list);
	up(&register_mutex);
	return snd_timer_free(timer);
}

static int snd_timer_dev_unregister(snd_device_t *device)
{
	snd_timer_t *timer = snd_magic_cast(snd_timer_t, device->device_data, return -ENXIO);
	return snd_timer_unregister(timer);
}

void snd_timer_notify(snd_timer_t *timer, enum sndrv_timer_event event, struct timespec *tstamp)
{
	unsigned long flags;
	unsigned long resolution = 0;
	snd_timer_instance_t *ti, *ts;
	struct list_head *p, *n;

	snd_runtime_check(timer->hw.flags & SNDRV_TIMER_HW_SLAVE, return);	
	snd_assert(event >= SNDRV_TIMER_EVENT_MSTART && event <= SNDRV_TIMER_EVENT_MPAUSE, return);
	spin_lock_irqsave(&timer->lock, flags);
	if (event == SNDRV_TIMER_EVENT_MSTART || event == SNDRV_TIMER_EVENT_MCONTINUE) {
		if (timer->hw.c_resolution)
			resolution = timer->hw.c_resolution(timer);
		else
			resolution = timer->hw.resolution;
	}
	list_for_each(p, &timer->active_list_head) {
		ti = (snd_timer_instance_t *)list_entry(p, snd_timer_instance_t, active_list);
		if (ti->ccallback)
			ti->ccallback(ti, event, tstamp, resolution);
		list_for_each(n, &ti->slave_active_head) {
			ts = (snd_timer_instance_t *)list_entry(n, snd_timer_instance_t, active_list);
			if (ts->ccallback)
				ts->ccallback(ts, event, tstamp, resolution);
		}
	}
	spin_unlock_irqrestore(&timer->lock, flags);
}

/*
 * exported functions for global timers
 */
int snd_timer_global_new(char *id, int device, snd_timer_t **rtimer)
{
	snd_timer_id_t tid;
	
	tid.dev_class = SNDRV_TIMER_CLASS_GLOBAL;
	tid.dev_sclass = SNDRV_TIMER_SCLASS_NONE;
	tid.card = -1;
	tid.device = device;
	tid.subdevice = 0;
	return snd_timer_new(NULL, id, &tid, rtimer);
}

int snd_timer_global_free(snd_timer_t *timer)
{
	return snd_timer_free(timer);
}

int snd_timer_global_register(snd_timer_t *timer)
{
	snd_device_t dev;

	memset(&dev, 0, sizeof(dev));
	dev.device_data = timer;
	return snd_timer_dev_register(&dev);
}

int snd_timer_global_unregister(snd_timer_t *timer)
{
	return snd_timer_unregister(timer);
}

/* 
 *  System timer
 */

struct snd_timer_system_private {
	struct timer_list tlist;
	struct timer * timer;
	unsigned long last_expires;
	unsigned long last_jiffies;
	unsigned long correction;
};

unsigned int snd_timer_system_resolution(void)
{
	return 1000000000L / HZ;
}

static void snd_timer_s_function(unsigned long data)
{
	snd_timer_t *timer = (snd_timer_t *)data;
	struct snd_timer_system_private *priv = timer->private_data;
	unsigned long jiff = jiffies;
	if (time_after(jiff, priv->last_expires))
		priv->correction = (long)jiff - (long)priv->last_expires;
	snd_timer_interrupt(timer, (long)jiff - (long)priv->last_jiffies);
}

static int snd_timer_s_start(snd_timer_t * timer)
{
	struct snd_timer_system_private *priv;
	unsigned long njiff;

	priv = (struct snd_timer_system_private *) timer->private_data;
	njiff = (priv->last_jiffies = jiffies);
	if (priv->correction > timer->sticks - 1) {
		priv->correction -= timer->sticks - 1;
		njiff++;
	} else {
		njiff += timer->sticks - priv->correction;
		priv->correction -= timer->sticks;
	}
	priv->last_expires = priv->tlist.expires = njiff;
	add_timer(&priv->tlist);
	return 0;
}

static int snd_timer_s_stop(snd_timer_t * timer)
{
	struct snd_timer_system_private *priv;
	unsigned long jiff;

	priv = (struct snd_timer_system_private *) timer->private_data;
	del_timer(&priv->tlist);
	jiff = jiffies;
	if (time_before(jiff, priv->last_expires))
		timer->sticks = priv->last_expires - jiff;
	else
		timer->sticks = 1;
	return 0;
}

static struct _snd_timer_hardware snd_timer_system =
{
	.flags =	SNDRV_TIMER_HW_FIRST | SNDRV_TIMER_HW_TASKLET,
	.resolution =	1000000000L / HZ,
	.ticks =	10000000L,
	.start =	snd_timer_s_start,
	.stop =		snd_timer_s_stop
};

static void snd_timer_free_system(snd_timer_t *timer)
{
	if (timer->private_data)
		kfree(timer->private_data);
}

static int snd_timer_register_system(void)
{
	snd_timer_t *timer;
	struct snd_timer_system_private *priv;
	int err;

	if ((err = snd_timer_global_new("system", SNDRV_TIMER_GLOBAL_SYSTEM, &timer)) < 0)
		return err;
	strcpy(timer->name, "system timer");
	timer->hw = snd_timer_system;
	priv = (struct snd_timer_system_private *) snd_kcalloc(sizeof(struct snd_timer_system_private), GFP_KERNEL);
	if (priv == NULL) {
		snd_timer_free(timer);
		return -ENOMEM;
	}
	init_timer(&priv->tlist);
	priv->tlist.function = snd_timer_s_function;
	priv->tlist.data = (unsigned long) timer;
	timer->private_data = priv;
	timer->private_free = snd_timer_free_system;
	return snd_timer_global_register(timer);
}

/*
 *  Info interface
 */

static void snd_timer_proc_read(snd_info_entry_t *entry,
				snd_info_buffer_t * buffer)
{
	unsigned long flags;
	snd_timer_t *timer;
	snd_timer_instance_t *ti;
	struct list_head *p, *q;

	down(&register_mutex);
	list_for_each(p, &snd_timer_list) {
		timer = (snd_timer_t *)list_entry(p, snd_timer_t, device_list);
		switch (timer->tmr_class) {
		case SNDRV_TIMER_CLASS_GLOBAL:
			snd_iprintf(buffer, "G%i: ", timer->tmr_device);
			break;
		case SNDRV_TIMER_CLASS_CARD:
			snd_iprintf(buffer, "C%i-%i: ", timer->card->number, timer->tmr_device);
			break;
		case SNDRV_TIMER_CLASS_PCM:
			snd_iprintf(buffer, "P%i-%i-%i: ", timer->card->number, timer->tmr_device, timer->tmr_subdevice);
			break;
		default:
			snd_iprintf(buffer, "?%i-%i-%i-%i: ", timer->tmr_class, timer->card ? timer->card->number : -1, timer->tmr_device, timer->tmr_subdevice);
		}
		snd_iprintf(buffer, "%s :", timer->name);
		if (timer->hw.resolution)
			snd_iprintf(buffer, " %lu.%03luus (%lu ticks)", timer->hw.resolution / 1000, timer->hw.resolution % 1000, timer->hw.ticks);
		if (timer->hw.flags & SNDRV_TIMER_HW_SLAVE)
			snd_iprintf(buffer, " SLAVE");
		snd_iprintf(buffer, "\n");
		spin_lock_irqsave(&timer->lock, flags);
		list_for_each(q, &timer->open_list_head) {
			ti = (snd_timer_instance_t *)list_entry(q, snd_timer_instance_t, open_list);
			snd_iprintf(buffer, "  Client %s : %s : lost interrupts %li\n",
					ti->owner ? ti->owner : "unknown",
					ti->flags & (SNDRV_TIMER_IFLG_START|SNDRV_TIMER_IFLG_RUNNING) ? "running" : "stopped",
					ti->lost);
		}
		spin_unlock_irqrestore(&timer->lock, flags);
	}
	up(&register_mutex);
}

/*
 *  USER SPACE interface
 */

static void snd_timer_user_interrupt(snd_timer_instance_t *timeri,
				     unsigned long resolution,
				     unsigned long ticks)
{
	snd_timer_user_t *tu = snd_magic_cast(snd_timer_user_t, timeri->callback_data, return);
	snd_timer_read_t *r;
	int prev;
	
	spin_lock(&tu->qlock);
	if (tu->qused > 0) {
		prev = tu->qtail == 0 ? tu->queue_size - 1 : tu->qtail - 1;
		r = &tu->queue[prev];
		if (r->resolution == resolution) {
			r->ticks += ticks;
			goto __wake;
		}
	}
	if (tu->qused >= tu->queue_size) {
		tu->overrun++;
	} else {
		r = &tu->queue[tu->qtail++];
		tu->qtail %= tu->queue_size;
		r->resolution = resolution;
		r->ticks = ticks;
		tu->qused++;
	}
      __wake:
	spin_unlock(&tu->qlock);
	kill_fasync(&tu->fasync, SIGIO, POLL_IN);
	wake_up(&tu->qchange_sleep);
}

static void snd_timer_user_append_to_tqueue(snd_timer_user_t *tu, snd_timer_tread_t *tread)
{
	if (tu->qused >= tu->queue_size) {
		tu->overrun++;
	} else {
		memcpy(&tu->queue[tu->qtail++], tread, sizeof(*tread));
		tu->qused++;
	}
}

static void snd_timer_user_ccallback(snd_timer_instance_t *timeri,
				     enum sndrv_timer_event event,
				     struct timespec *tstamp,
				     unsigned long resolution)
{
	snd_timer_user_t *tu = snd_magic_cast(snd_timer_user_t, timeri->callback_data, return);
	snd_timer_tread_t r1;

	if (event >= SNDRV_TIMER_EVENT_START && event <= SNDRV_TIMER_EVENT_PAUSE)
		tu->tstamp = *tstamp;
	if ((tu->filter & (1 << event)) == 0 || !tu->tread)
		return;
	r1.event = event;
	r1.tstamp = *tstamp;
	r1.val = resolution;
	spin_lock(&tu->qlock);
	snd_timer_user_append_to_tqueue(tu, &r1);
	spin_unlock(&tu->qlock);
}

static void snd_timer_user_tinterrupt(snd_timer_instance_t *timeri,
				      unsigned long resolution,
				      unsigned long ticks)
{
	snd_timer_user_t *tu = snd_magic_cast(snd_timer_user_t, timeri->callback_data, return);
	snd_timer_tread_t *r, r1;
	struct timespec tstamp;
	int prev, append = 0;

	snd_timestamp_zero(&tstamp);
	spin_lock(&tu->qlock);
	if ((tu->filter & ((1 << SNDRV_TIMER_EVENT_RESOLUTION)|(1 << SNDRV_TIMER_EVENT_TICK))) == 0) {
		spin_unlock(&tu->qlock);
		return;
	}
	if (tu->last_resolution != resolution || ticks > 0)
		snd_timestamp_now(&tstamp, 1);
	if ((tu->filter & (1 << SNDRV_TIMER_EVENT_RESOLUTION)) && tu->last_resolution != resolution) {
		r1.event = SNDRV_TIMER_EVENT_RESOLUTION;
		r1.tstamp = tstamp;
		r1.val = resolution;
		snd_timer_user_append_to_tqueue(tu, &r1);
		tu->last_resolution = resolution;
		append++;
	}
	if ((tu->filter & (1 << SNDRV_TIMER_EVENT_TICK)) == 0)
		goto __wake;
	if (ticks == 0)
		goto __wake;
	if (tu->qused > 0) {
		prev = tu->qtail == 0 ? tu->queue_size - 1 : tu->qtail - 1;
		r = &tu->tqueue[prev];
		if (r->event == SNDRV_TIMER_EVENT_TICK) {
			r->tstamp = tstamp;
			r->val += ticks;
			append++;
			goto __wake;
		}
	}
	r1.event = SNDRV_TIMER_EVENT_TICK;
	r1.tstamp = tstamp;
	r1.val = ticks;
	snd_timer_user_append_to_tqueue(tu, &r1);
	append++;
      __wake:
	spin_unlock(&tu->qlock);
	if (append == 0)
		return;
	kill_fasync(&tu->fasync, SIGIO, POLL_IN);
	wake_up(&tu->qchange_sleep);
}

static int snd_timer_user_open(struct inode *inode, struct file *file)
{
	snd_timer_user_t *tu;
	
	tu = snd_magic_kcalloc(snd_timer_user_t, 0, GFP_KERNEL);
	if (tu == NULL)
		return -ENOMEM;
	spin_lock_init(&tu->qlock);
	init_waitqueue_head(&tu->qchange_sleep);
	tu->ticks = 1;
	tu->queue_size = 128;
	tu->queue = (snd_timer_read_t *)kmalloc(tu->queue_size * sizeof(snd_timer_read_t), GFP_KERNEL);
	if (tu->queue == NULL) {
		snd_magic_kfree(tu);
		return -ENOMEM;
	}
	file->private_data = tu;
	return 0;
}

static int snd_timer_user_release(struct inode *inode, struct file *file)
{
	snd_timer_user_t *tu;

	if (file->private_data) {
		tu = snd_magic_cast(snd_timer_user_t, file->private_data, return -ENXIO);
		file->private_data = NULL;
		fasync_helper(-1, file, 0, &tu->fasync);
		if (tu->timeri)
			snd_timer_close(tu->timeri);
		if (tu->queue)
			kfree(tu->queue);
		if (tu->tqueue)
			kfree(tu->tqueue);
		snd_magic_kfree(tu);
	}
	return 0;
}

static void snd_timer_user_zero_id(snd_timer_id_t *id)
{
	id->dev_class = SNDRV_TIMER_CLASS_NONE;
	id->dev_sclass = SNDRV_TIMER_SCLASS_NONE;
	id->card = -1;
	id->device = -1;
	id->subdevice = -1;
}

static void snd_timer_user_copy_id(snd_timer_id_t *id, snd_timer_t *timer)
{
	id->dev_class = timer->tmr_class;
	id->dev_sclass = SNDRV_TIMER_SCLASS_NONE;
	id->card = timer->card ? timer->card->number : -1;
	id->device = timer->tmr_device;
	id->subdevice = timer->tmr_subdevice;
}

static int snd_timer_user_next_device(snd_timer_id_t __user *_tid)
{
	snd_timer_id_t id;
	snd_timer_t *timer;
	struct list_head *p;
	
	if (copy_from_user(&id, _tid, sizeof(id)))
		return -EFAULT;
	down(&register_mutex);
	if (id.dev_class < 0) {		/* first item */
		if (list_empty(&snd_timer_list))
			snd_timer_user_zero_id(&id);
		else {
			timer = (snd_timer_t *)list_entry(snd_timer_list.next, snd_timer_t, device_list);
			snd_timer_user_copy_id(&id, timer);
		}
	} else {
		switch (id.dev_class) {
		case SNDRV_TIMER_CLASS_GLOBAL:
			id.device = id.device < 0 ? 0 : id.device + 1;
			list_for_each(p, &snd_timer_list) {
				timer = (snd_timer_t *)list_entry(p, snd_timer_t, device_list);
				if (timer->tmr_class > SNDRV_TIMER_CLASS_GLOBAL) {
					snd_timer_user_copy_id(&id, timer);
					break;
				}
				if (timer->tmr_device >= id.device) {
					snd_timer_user_copy_id(&id, timer);
					break;
				}
			}
			if (p == &snd_timer_list)
				snd_timer_user_zero_id(&id);
			break;
		case SNDRV_TIMER_CLASS_CARD:
		case SNDRV_TIMER_CLASS_PCM:
			if (id.card < 0) {
				id.card = 0;
			} else {
				if (id.card < 0) {
					id.card = 0;
				} else {
					if (id.device < 0) {
						id.device = 0;
					} else {
						id.subdevice = id.subdevice < 0 ? 0 : id.subdevice + 1;
					}
				}
			}
			list_for_each(p, &snd_timer_list) {
				timer = (snd_timer_t *)list_entry(p, snd_timer_t, device_list);
				if (timer->tmr_class > id.dev_class) {
					snd_timer_user_copy_id(&id, timer);
					break;
				}
				if (timer->tmr_class < id.dev_class)
					continue;
				if (timer->card->number > id.card) {
					snd_timer_user_copy_id(&id, timer);
					break;
				}
				if (timer->card->number < id.card)
					continue;
				if (timer->tmr_device > id.device) {
					snd_timer_user_copy_id(&id, timer);
					break;
				}
				if (timer->tmr_device < id.device)
					continue;
				if (timer->tmr_subdevice > id.subdevice) {
					snd_timer_user_copy_id(&id, timer);
					break;
				}
				if (timer->tmr_subdevice < id.subdevice)
					continue;
				snd_timer_user_copy_id(&id, timer);
				break;
			}
			if (p == &snd_timer_list)
				snd_timer_user_zero_id(&id);
			break;
		default:
			snd_timer_user_zero_id(&id);
		}
	}
	up(&register_mutex);
	if (copy_to_user(_tid, &id, sizeof(*_tid)))
		return -EFAULT;
	return 0;
} 

static int snd_timer_user_ginfo(struct file *file, snd_timer_ginfo_t __user *_ginfo)
{
	snd_timer_ginfo_t ginfo;
	snd_timer_id_t tid;
	snd_timer_t *t;
	struct list_head *p;
	int err = 0;

	if (copy_from_user(&ginfo, _ginfo, sizeof(ginfo)))
		return -EFAULT;
	tid = ginfo.tid;
	memset(&ginfo, 0, sizeof(ginfo));
	ginfo.tid = tid;
	down(&register_mutex);
	t = snd_timer_find(&tid);
	if (t != NULL) {
		ginfo.card = t->card ? t->card->number : -1;
		if (t->hw.flags & SNDRV_TIMER_HW_SLAVE)
			ginfo.flags |= SNDRV_TIMER_FLG_SLAVE;
		strlcpy(ginfo.id, t->id, sizeof(ginfo.id));
		strlcpy(ginfo.name, t->name, sizeof(ginfo.name));
		ginfo.resolution = t->hw.resolution;
		if (t->hw.resolution_min > 0) {
			ginfo.resolution_min = t->hw.resolution_min;
			ginfo.resolution_max = t->hw.resolution_max;
		}
		list_for_each(p, &t->open_list_head) {
			ginfo.clients++;
		}
	} else {
		err = -ENODEV;
	}
	up(&register_mutex);
	if (err >= 0 && copy_to_user(_ginfo, &ginfo, sizeof(ginfo)))
		err = -EFAULT;
	return err;
}

static int snd_timer_user_gparams(struct file *file, snd_timer_gparams_t __user *_gparams)
{
	snd_timer_gparams_t gparams;
	snd_timer_t *t;
	int err;

	if (copy_from_user(&gparams, _gparams, sizeof(gparams)))
		return -EFAULT;
	down(&register_mutex);
	t = snd_timer_find(&gparams.tid);
	if (t != NULL) {
		if (list_empty(&t->open_list_head)) {
			if (t->hw.set_period)
				err = t->hw.set_period(t, gparams.period_num, gparams.period_den);
			else
				err = -ENOSYS;
		} else {
			err = -EBUSY;
		}
	} else {
		err = -ENODEV;
	}
	up(&register_mutex);
	return err;
}

static int snd_timer_user_gstatus(struct file *file, snd_timer_gstatus_t __user *_gstatus)
{
	snd_timer_gstatus_t gstatus;
	snd_timer_id_t tid;
	snd_timer_t *t;
	int err = 0;

	if (copy_from_user(&gstatus, _gstatus, sizeof(gstatus)))
		return -EFAULT;
	tid = gstatus.tid;
	memset(&gstatus, 0, sizeof(gstatus));
	gstatus.tid = tid;
	down(&register_mutex);
	t = snd_timer_find(&tid);
	if (t != NULL) {
		if (t->hw.c_resolution)
			gstatus.resolution = t->hw.c_resolution(t);
		else
			gstatus.resolution = t->hw.resolution;
		if (t->hw.precise_resolution) {
			t->hw.precise_resolution(t, &gstatus.resolution_num, &gstatus.resolution_den);
		} else {
			gstatus.resolution_num = gstatus.resolution;
			gstatus.resolution_den = 1000000000uL;
		}
	} else {
		err = -ENODEV;
	}
	up(&register_mutex);
	if (err >= 0 && copy_to_user(_gstatus, &gstatus, sizeof(gstatus)))
		err = -EFAULT;
	return err;
}

static int snd_timer_user_tselect(struct file *file, snd_timer_select_t __user *_tselect)
{
	snd_timer_user_t *tu;
	snd_timer_select_t tselect;
	char str[32];
	int err;
	
	tu = snd_magic_cast(snd_timer_user_t, file->private_data, return -ENXIO);
	if (tu->timeri)
		snd_timer_close(tu->timeri);
	if (copy_from_user(&tselect, _tselect, sizeof(tselect)))
		return -EFAULT;
	sprintf(str, "application %i", current->pid);
	if (tselect.id.dev_class != SNDRV_TIMER_CLASS_SLAVE)
		tselect.id.dev_sclass = SNDRV_TIMER_SCLASS_APPLICATION;
	if ((err = snd_timer_open(&tu->timeri, str, &tselect.id, current->pid)) < 0)
		return err;

	if (tu->queue) {
		kfree(tu->queue);
		tu->queue = NULL;
	}
	if (tu->tqueue) {
		kfree(tu->tqueue);
		tu->tqueue = NULL;
	}
	if (tu->tread) {
		tu->tqueue = (snd_timer_tread_t *)kmalloc(tu->queue_size * sizeof(snd_timer_tread_t), GFP_KERNEL);
		if (tu->tqueue == NULL) {
			snd_timer_close(tu->timeri);
			return -ENOMEM;
		}
	} else {
		tu->queue = (snd_timer_read_t *)kmalloc(tu->queue_size * sizeof(snd_timer_read_t), GFP_KERNEL);
		if (tu->queue == NULL) {
			snd_timer_close(tu->timeri);
			return -ENOMEM;
		}
	}
	
	tu->timeri->flags |= SNDRV_TIMER_IFLG_FAST;
	tu->timeri->callback = tu->tread ? snd_timer_user_tinterrupt : snd_timer_user_interrupt;
	tu->timeri->ccallback = snd_timer_user_ccallback;
	tu->timeri->callback_data = (void *)tu;
	return 0;
}

static int snd_timer_user_info(struct file *file, snd_timer_info_t __user *_info)
{
	snd_timer_user_t *tu;
	snd_timer_info_t info;
	snd_timer_t *t;

	tu = snd_magic_cast(snd_timer_user_t, file->private_data, return -ENXIO);
	snd_assert(tu->timeri != NULL, return -ENXIO);
	t = tu->timeri->timer;
	snd_assert(t != NULL, return -ENXIO);
	memset(&info, 0, sizeof(info));
	info.card = t->card ? t->card->number : -1;
	if (t->hw.flags & SNDRV_TIMER_HW_SLAVE)
		info.flags |= SNDRV_TIMER_FLG_SLAVE;
	strlcpy(info.id, t->id, sizeof(info.id));
	strlcpy(info.name, t->name, sizeof(info.name));
	info.resolution = t->hw.resolution;
	if (copy_to_user(_info, &info, sizeof(*_info)))
		return -EFAULT;
	return 0;
}

static int snd_timer_user_params(struct file *file, snd_timer_params_t __user *_params)
{
	snd_timer_user_t *tu;
	snd_timer_params_t params;
	snd_timer_t *t;
	snd_timer_read_t *tr;
	snd_timer_tread_t *ttr;
	int err;
	
	tu = snd_magic_cast(snd_timer_user_t, file->private_data, return -ENXIO);
	snd_assert(tu->timeri != NULL, return -ENXIO);
	t = tu->timeri->timer;
	snd_assert(t != NULL, return -ENXIO);
	if (copy_from_user(&params, _params, sizeof(params)))
		return -EFAULT;
	if (!(t->hw.flags & SNDRV_TIMER_HW_SLAVE) && params.ticks < 1) {
		err = -EINVAL;
		goto _end;
	}
	if (params.queue_size > 0 && (params.queue_size < 32 || params.queue_size > 1024)) {
		err = -EINVAL;
		goto _end;
	}
	if (params.filter & ~((1<<SNDRV_TIMER_EVENT_RESOLUTION)|
			      (1<<SNDRV_TIMER_EVENT_TICK)|
			      (1<<SNDRV_TIMER_EVENT_START)|
			      (1<<SNDRV_TIMER_EVENT_STOP)|
			      (1<<SNDRV_TIMER_EVENT_CONTINUE)|
			      (1<<SNDRV_TIMER_EVENT_PAUSE)|
			      (1<<SNDRV_TIMER_EVENT_MSTART)|
			      (1<<SNDRV_TIMER_EVENT_MSTOP)|
			      (1<<SNDRV_TIMER_EVENT_MCONTINUE)|
			      (1<<SNDRV_TIMER_EVENT_MPAUSE))) {
		err = -EINVAL;
		goto _end;
	}
	snd_timer_stop(tu->timeri);
	spin_lock_irq(&t->lock);
	tu->timeri->flags &= ~(SNDRV_TIMER_IFLG_AUTO|
			       SNDRV_TIMER_IFLG_EXCLUSIVE|
			       SNDRV_TIMER_IFLG_EARLY_EVENT);
	if (params.flags & SNDRV_TIMER_PSFLG_AUTO)
		tu->timeri->flags |= SNDRV_TIMER_IFLG_AUTO;
	if (params.flags & SNDRV_TIMER_PSFLG_EXCLUSIVE)
		tu->timeri->flags |= SNDRV_TIMER_IFLG_EXCLUSIVE;
	if (params.flags & SNDRV_TIMER_PSFLG_EARLY_EVENT)
		tu->timeri->flags |= SNDRV_TIMER_IFLG_EARLY_EVENT;
	spin_unlock_irq(&t->lock);
	if (params.queue_size > 0 && (unsigned int)tu->queue_size != params.queue_size) {
		if (tu->tread) {
			ttr = (snd_timer_tread_t *)kmalloc(params.queue_size * sizeof(snd_timer_tread_t), GFP_KERNEL);
			if (ttr) {
				kfree(tu->tqueue);
				tu->queue_size = params.queue_size;
				tu->tqueue = ttr;
			}
		} else {
			tr = (snd_timer_read_t *)kmalloc(params.queue_size * sizeof(snd_timer_read_t), GFP_KERNEL);
			if (tr) {
				kfree(tu->queue);
				tu->queue_size = params.queue_size;
				tu->queue = tr;
			}
		}
	}
	tu->qhead = tu->qtail = tu->qused = 0;
	if (tu->timeri->flags & SNDRV_TIMER_IFLG_EARLY_EVENT) {
		if (tu->tread) {
			snd_timer_tread_t tread;
			tread.event = SNDRV_TIMER_EVENT_EARLY;
			tread.tstamp.tv_sec = 0;
			tread.tstamp.tv_nsec = 0;
			tread.val = 0;
			snd_timer_user_append_to_tqueue(tu, &tread);
		} else {
			snd_timer_read_t *r = &tu->queue[0];
			r->resolution = 0;
			r->ticks = 0;
			tu->qused++;
			tu->qtail++;
		}
		
	}
	tu->filter = params.filter;
	tu->ticks = params.ticks;
	err = 0;
 _end:
	if (copy_to_user(_params, &params, sizeof(params)))
		return -EFAULT;
	return err;
}

static int snd_timer_user_status(struct file *file, snd_timer_status_t __user *_status)
{
	snd_timer_user_t *tu;
	snd_timer_status_t status;
	
	tu = snd_magic_cast(snd_timer_user_t, file->private_data, return -ENXIO);
	snd_assert(tu->timeri != NULL, return -ENXIO);
	memset(&status, 0, sizeof(status));
	status.tstamp = tu->tstamp;
	status.resolution = snd_timer_resolution(tu->timeri);
	status.lost = tu->timeri->lost;
	status.overrun = tu->overrun;
	spin_lock_irq(&tu->qlock);
	status.queue = tu->qused;
	spin_unlock_irq(&tu->qlock);
	if (copy_to_user(_status, &status, sizeof(status)))
		return -EFAULT;
	return 0;
}

static int snd_timer_user_start(struct file *file)
{
	int err;
	snd_timer_user_t *tu;
		
	tu = snd_magic_cast(snd_timer_user_t, file->private_data, return -ENXIO);
	snd_assert(tu->timeri != NULL, return -ENXIO);
	snd_timer_stop(tu->timeri);
	tu->timeri->lost = 0;
	tu->last_resolution = 0;
	return (err = snd_timer_start(tu->timeri, tu->ticks)) < 0 ? err : 0;
}

static int snd_timer_user_stop(struct file *file)
{
	int err;
	snd_timer_user_t *tu;
		
	tu = snd_magic_cast(snd_timer_user_t, file->private_data, return -ENXIO);
	snd_assert(tu->timeri != NULL, return -ENXIO);
	return (err = snd_timer_stop(tu->timeri)) < 0 ? err : 0;
}

static int snd_timer_user_continue(struct file *file)
{
	int err;
	snd_timer_user_t *tu;
		
	tu = snd_magic_cast(snd_timer_user_t, file->private_data, return -ENXIO);
	snd_assert(tu->timeri != NULL, return -ENXIO);
	tu->timeri->lost = 0;
	return (err = snd_timer_continue(tu->timeri)) < 0 ? err : 0;
}

static int snd_timer_user_ioctl(struct inode *inode, struct file *file,
				unsigned int cmd, unsigned long arg)
{
	snd_timer_user_t *tu;
	void __user *argp = (void __user *)arg;
	int __user *p = argp;
	
	tu = snd_magic_cast(snd_timer_user_t, file->private_data, return -ENXIO);
	switch (cmd) {
	case SNDRV_TIMER_IOCTL_PVERSION:
		return put_user(SNDRV_TIMER_VERSION, p) ? -EFAULT : 0;
	case SNDRV_TIMER_IOCTL_NEXT_DEVICE:
		return snd_timer_user_next_device(argp);
	case SNDRV_TIMER_IOCTL_TREAD:
	{
		int xarg;
		
		if (tu->timeri)		/* too late */
			return -EBUSY;
		if (get_user(xarg, p))
			return -EFAULT;
		tu->tread = xarg ? 1 : 0;
		return 0;
	}
	case SNDRV_TIMER_IOCTL_GINFO:
		return snd_timer_user_ginfo(file, argp);
	case SNDRV_TIMER_IOCTL_GPARAMS:
		return snd_timer_user_gparams(file, argp);
	case SNDRV_TIMER_IOCTL_GSTATUS:
		return snd_timer_user_gstatus(file, argp);
	case SNDRV_TIMER_IOCTL_SELECT:
		return snd_timer_user_tselect(file, argp);
	case SNDRV_TIMER_IOCTL_INFO:
		return snd_timer_user_info(file, argp);
	case SNDRV_TIMER_IOCTL_PARAMS:
		return snd_timer_user_params(file, argp);
	case SNDRV_TIMER_IOCTL_STATUS:
		return snd_timer_user_status(file, argp);
	case SNDRV_TIMER_IOCTL_START:
		return snd_timer_user_start(file);
	case SNDRV_TIMER_IOCTL_STOP:
		return snd_timer_user_stop(file);
	case SNDRV_TIMER_IOCTL_CONTINUE:
		return snd_timer_user_continue(file);
	}
	return -ENOTTY;
}

static int snd_timer_user_fasync(int fd, struct file * file, int on)
{
	snd_timer_user_t *tu;
	int err;
	
	tu = snd_magic_cast(snd_timer_user_t, file->private_data, return -ENXIO);
	err = fasync_helper(fd, file, on, &tu->fasync);
        if (err < 0)
		return err;
	return 0;
}

static ssize_t snd_timer_user_read(struct file *file, char __user *buffer, size_t count, loff_t *offset)
{
	snd_timer_user_t *tu;
	long result = 0, unit;
	int err = 0;
	
	tu = snd_magic_cast(snd_timer_user_t, file->private_data, return -ENXIO);
	unit = tu->tread ? sizeof(snd_timer_tread_t) : sizeof(snd_timer_read_t);
	spin_lock_irq(&tu->qlock);
	while ((long)count - result >= unit) {
		while (!tu->qused) {
			wait_queue_t wait;

			if ((file->f_flags & O_NONBLOCK) != 0 || result > 0) {
				err = -EAGAIN;
				break;
			}

			set_current_state(TASK_INTERRUPTIBLE);
			init_waitqueue_entry(&wait, current);
			add_wait_queue(&tu->qchange_sleep, &wait);

			spin_unlock_irq(&tu->qlock);
			schedule();
			spin_lock_irq(&tu->qlock);

			remove_wait_queue(&tu->qchange_sleep, &wait);

			if (signal_pending(current)) {
				err = -ERESTARTSYS;
				break;
			}
		}

		spin_unlock_irq(&tu->qlock);
		if (err < 0)
			goto _error;

		if (tu->tread) {
			if (copy_to_user(buffer, &tu->tqueue[tu->qhead++], sizeof(snd_timer_tread_t))) {
				err = -EFAULT;
				goto _error;
			}
		} else {
			if (copy_to_user(buffer, &tu->queue[tu->qhead++], sizeof(snd_timer_read_t))) {
				err = -EFAULT;
				goto _error;
			}
		}

		tu->qhead %= tu->queue_size;

		result += unit;
		buffer += unit;

		spin_lock_irq(&tu->qlock);
		tu->qused--;
	}
	spin_unlock_irq(&tu->qlock);
 _error:
	return result > 0 ? result : err;
}

static unsigned int snd_timer_user_poll(struct file *file, poll_table * wait)
{
        unsigned int mask;
        snd_timer_user_t *tu;

        tu = snd_magic_cast(snd_timer_user_t, file->private_data, return 0);

        poll_wait(file, &tu->qchange_sleep, wait);
	
	mask = 0;
	if (tu->qused)
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

static struct file_operations snd_timer_f_ops =
{
	.owner =	THIS_MODULE,
	.read =		snd_timer_user_read,
	.open =		snd_timer_user_open,
	.release =	snd_timer_user_release,
	.poll =		snd_timer_user_poll,
	.ioctl =	snd_timer_user_ioctl,
	.fasync = 	snd_timer_user_fasync,
};

static snd_minor_t snd_timer_reg =
{
	.comment =	"timer",
	.f_ops =	&snd_timer_f_ops,
};

/*
 *  ENTRY functions
 */

static snd_info_entry_t *snd_timer_proc_entry = NULL;

static int __init alsa_timer_init(void)
{
	int err;
	snd_info_entry_t *entry;

#ifdef SNDRV_OSS_INFO_DEV_TIMERS
	snd_oss_info_register(SNDRV_OSS_INFO_DEV_TIMERS, SNDRV_CARDS - 1, "system timer");
#endif
	if ((entry = snd_info_create_module_entry(THIS_MODULE, "timers", NULL)) != NULL) {
		entry->c.text.read_size = SNDRV_TIMER_DEVICES * 128;
		entry->c.text.read = snd_timer_proc_read;
		if (snd_info_register(entry) < 0) {
			snd_info_free_entry(entry);
			entry = NULL;
		}
	}
	snd_timer_proc_entry = entry;
	if ((err = snd_timer_register_system()) < 0)
		snd_printk(KERN_ERR "unable to register system timer (%i)\n", err);
	if ((err = snd_register_device(SNDRV_DEVICE_TYPE_TIMER,
					NULL, 0, &snd_timer_reg, "timer"))<0)
		snd_printk(KERN_ERR "unable to register timer device (%i)\n", err);
	return 0;
}

static void __exit alsa_timer_exit(void)
{
	struct list_head *p, *n;

	snd_unregister_device(SNDRV_DEVICE_TYPE_TIMER, NULL, 0);
	/* unregister the system timer */
	list_for_each_safe(p, n, &snd_timer_list) {
		snd_timer_t *timer = (snd_timer_t *)list_entry(p, snd_timer_t, device_list);
		snd_timer_unregister(timer);
	}
	if (snd_timer_proc_entry) {
		snd_info_unregister(snd_timer_proc_entry);
		snd_timer_proc_entry = NULL;
	}
#ifdef SNDRV_OSS_INFO_DEV_TIMERS
	snd_oss_info_unregister(SNDRV_OSS_INFO_DEV_TIMERS, SNDRV_CARDS - 1);
#endif
}

module_init(alsa_timer_init)
module_exit(alsa_timer_exit)

EXPORT_SYMBOL(snd_timer_open);
EXPORT_SYMBOL(snd_timer_close);
EXPORT_SYMBOL(snd_timer_resolution);
EXPORT_SYMBOL(snd_timer_start);
EXPORT_SYMBOL(snd_timer_stop);
EXPORT_SYMBOL(snd_timer_continue);
EXPORT_SYMBOL(snd_timer_pause);
EXPORT_SYMBOL(snd_timer_new);
EXPORT_SYMBOL(snd_timer_notify);
EXPORT_SYMBOL(snd_timer_global_new);
EXPORT_SYMBOL(snd_timer_global_free);
EXPORT_SYMBOL(snd_timer_global_register);
EXPORT_SYMBOL(snd_timer_global_unregister);
EXPORT_SYMBOL(snd_timer_interrupt);
EXPORT_SYMBOL(snd_timer_system_resolution);
