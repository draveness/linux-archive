/*
 * linux/kernel/time/tick-oneshot.c
 *
 * This file contains functions which manage high resolution tick
 * related events.
 *
 * Copyright(C) 2005-2006, Thomas Gleixner <tglx@linutronix.de>
 * Copyright(C) 2005-2007, Red Hat, Inc., Ingo Molnar
 * Copyright(C) 2006-2007, Timesys Corp., Thomas Gleixner
 *
 * This code is licenced under the GPL version 2. For details see
 * kernel-base/COPYING.
 */
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/hrtimer.h>
#include <linux/irq.h>
#include <linux/percpu.h>
#include <linux/profile.h>
#include <linux/sched.h>
#include <linux/tick.h>

#include "tick-internal.h"

/**
 * tick_program_event
 */
int tick_program_event(ktime_t expires, int force)
{
	struct clock_event_device *dev = __get_cpu_var(tick_cpu_device).evtdev;
	ktime_t now = ktime_get();

	while (1) {
		int ret = clockevents_program_event(dev, expires, now);

		if (!ret || !force)
			return ret;
		now = ktime_get();
		expires = ktime_add(now, ktime_set(0, dev->min_delta_ns));
	}
}

/**
 * tick_resume_onshot - resume oneshot mode
 */
void tick_resume_oneshot(void)
{
	struct tick_device *td = &__get_cpu_var(tick_cpu_device);
	struct clock_event_device *dev = td->evtdev;

	clockevents_set_mode(dev, CLOCK_EVT_MODE_ONESHOT);
	tick_program_event(ktime_get(), 1);
}

/**
 * tick_setup_oneshot - setup the event device for oneshot mode (hres or nohz)
 */
void tick_setup_oneshot(struct clock_event_device *newdev,
			void (*handler)(struct clock_event_device *),
			ktime_t next_event)
{
	newdev->event_handler = handler;
	clockevents_set_mode(newdev, CLOCK_EVT_MODE_ONESHOT);
	clockevents_program_event(newdev, next_event, ktime_get());
}

/**
 * tick_switch_to_oneshot - switch to oneshot mode
 */
int tick_switch_to_oneshot(void (*handler)(struct clock_event_device *))
{
	struct tick_device *td = &__get_cpu_var(tick_cpu_device);
	struct clock_event_device *dev = td->evtdev;

	if (!dev || !(dev->features & CLOCK_EVT_FEAT_ONESHOT) ||
		    !tick_device_is_functional(dev)) {

		printk(KERN_INFO "Clockevents: "
		       "could not switch to one-shot mode:");
		if (!dev) {
			printk(" no tick device\n");
		} else {
			if (!tick_device_is_functional(dev))
				printk(" %s is not functional.\n", dev->name);
			else
				printk(" %s does not support one-shot mode.\n",
				       dev->name);
		}
		return -EINVAL;
	}

	td->mode = TICKDEV_MODE_ONESHOT;
	dev->event_handler = handler;
	clockevents_set_mode(dev, CLOCK_EVT_MODE_ONESHOT);
	tick_broadcast_switch_to_oneshot();
	return 0;
}

#ifdef CONFIG_HIGH_RES_TIMERS
/**
 * tick_init_highres - switch to high resolution mode
 *
 * Called with interrupts disabled.
 */
int tick_init_highres(void)
{
	return tick_switch_to_oneshot(hrtimer_interrupt);
}
#endif
