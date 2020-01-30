/*
 *  arch/s390/kernel/vtime.c
 *    Virtual cpu timer based timer functions.
 *
 *  S390 version
 *    Copyright (C) 2004 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Jan Glauber <jan.glauber@de.ibm.com>
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/types.h>
#include <linux/timex.h>
#include <linux/notifier.h>

#include <asm/s390_ext.h>
#include <asm/timer.h>

#define VTIMER_MAGIC (0x4b87ad6e + 1)
static ext_int_info_t ext_int_info_timer;
DEFINE_PER_CPU(struct vtimer_queue, virt_cpu_timer);

void start_cpu_timer(void)
{
	struct vtimer_queue *vt_list;

	vt_list = &per_cpu(virt_cpu_timer, smp_processor_id());
	set_vtimer(vt_list->idle);
}

void stop_cpu_timer(void)
{
	__u64 done;
	struct vtimer_queue *vt_list;

	vt_list = &per_cpu(virt_cpu_timer, smp_processor_id());

	/* nothing to do */
	if (list_empty(&vt_list->list)) {
		vt_list->idle = VTIMER_MAX_SLICE;
		goto fire;
	}

	/* store progress */
	asm volatile ("STPT %0" : "=m" (done));

	/*
	 * If done is negative we do not stop the CPU timer
	 * because we will get instantly an interrupt that
	 * will start the CPU timer again.
	 */
	if (done & 1LL<<63)
		return;
	else
		vt_list->offset += vt_list->to_expire - done;

	/* save the actual expire value */
	vt_list->idle = done;

	/*
	 * We cannot halt the CPU timer, we just write a value that
	 * nearly never expires (only after 71 years) and re-write
	 * the stored expire value if we continue the timer
	 */
 fire:
	set_vtimer(VTIMER_MAX_SLICE);
}

void set_vtimer(__u64 expires)
{
	asm volatile ("SPT %0" : : "m" (expires));

	/* store expire time for this CPU timer */
	per_cpu(virt_cpu_timer, smp_processor_id()).to_expire = expires;
}

/*
 * Sorted add to a list. List is linear searched until first bigger
 * element is found.
 */
void list_add_sorted(struct vtimer_list *timer, struct list_head *head)
{
	struct vtimer_list *event;

	list_for_each_entry(event, head, entry) {
		if (event->expires > timer->expires) {
			list_add_tail(&timer->entry, &event->entry);
			return;
		}
	}
	list_add_tail(&timer->entry, head);
}

/*
 * Do the callback functions of expired vtimer events.
 * Called from within the interrupt handler.
 */
static void do_callbacks(struct list_head *cb_list, struct pt_regs *regs)
{
	struct vtimer_queue *vt_list;
	struct vtimer_list *event, *tmp;
	void (*fn)(unsigned long, struct pt_regs*);
	unsigned long data;

	if (list_empty(cb_list))
		return;

	vt_list = &per_cpu(virt_cpu_timer, smp_processor_id());

	list_for_each_entry_safe(event, tmp, cb_list, entry) {
		fn = event->function;
		data = event->data;
		fn(data, regs);

		if (!event->interval)
			/* delete one shot timer */
			list_del_init(&event->entry);
		else {
			/* move interval timer back to list */
			spin_lock(&vt_list->lock);
			list_del_init(&event->entry);
			list_add_sorted(event, &vt_list->list);
			spin_unlock(&vt_list->lock);
		}
	}
}

/*
 * Handler for the virtual CPU timer.
 */
static void do_cpu_timer_interrupt(struct pt_regs *regs, __u16 error_code)
{
	int cpu;
	__u64 next, delta;
	struct vtimer_queue *vt_list;
	struct vtimer_list *event, *tmp;
	struct list_head *ptr;
	/* the callback queue */
	struct list_head cb_list;

	INIT_LIST_HEAD(&cb_list);
	cpu = smp_processor_id();
	vt_list = &per_cpu(virt_cpu_timer, cpu);

	/* walk timer list, fire all expired events */
	spin_lock(&vt_list->lock);

	if (vt_list->to_expire < VTIMER_MAX_SLICE)
		vt_list->offset += vt_list->to_expire;

	list_for_each_entry_safe(event, tmp, &vt_list->list, entry) {
		if (event->expires > vt_list->offset)
			/* found first unexpired event, leave */
			break;

		/* re-charge interval timer, we have to add the offset */
		if (event->interval)
			event->expires = event->interval + vt_list->offset;

		/* move expired timer to the callback queue */
		list_move_tail(&event->entry, &cb_list);
	}
	spin_unlock(&vt_list->lock);
	do_callbacks(&cb_list, regs);

	/* next event is first in list */
	spin_lock(&vt_list->lock);
	if (!list_empty(&vt_list->list)) {
		ptr = vt_list->list.next;
		event = list_entry(ptr, struct vtimer_list, entry);
		next = event->expires - vt_list->offset;

		/* add the expired time from this interrupt handler
		 * and the callback functions
		 */
		asm volatile ("STPT %0" : "=m" (delta));
		delta = 0xffffffffffffffffLL - delta + 1;
		vt_list->offset += delta;
		next -= delta;
	} else {
		vt_list->offset = 0;
		next = VTIMER_MAX_SLICE;
	}
	spin_unlock(&vt_list->lock);
	set_vtimer(next);
}

void init_virt_timer(struct vtimer_list *timer)
{
	timer->magic = VTIMER_MAGIC;
	timer->function = NULL;
	INIT_LIST_HEAD(&timer->entry);
	spin_lock_init(&timer->lock);
}
EXPORT_SYMBOL(init_virt_timer);

static inline int check_vtimer(struct vtimer_list *timer)
{
	if (timer->magic != VTIMER_MAGIC)
		return -EINVAL;
	return 0;
}

static inline int vtimer_pending(struct vtimer_list *timer)
{
	return (!list_empty(&timer->entry));
}

/*
 * this function should only run on the specified CPU
 */
static void internal_add_vtimer(struct vtimer_list *timer)
{
	unsigned long flags;
	__u64 done;
	struct vtimer_list *event;
	struct vtimer_queue *vt_list;

	vt_list = &per_cpu(virt_cpu_timer, timer->cpu);
	spin_lock_irqsave(&vt_list->lock, flags);

	if (timer->cpu != smp_processor_id())
		printk("internal_add_vtimer: BUG, running on wrong CPU");

	/* if list is empty we only have to set the timer */
	if (list_empty(&vt_list->list)) {
		/* reset the offset, this may happen if the last timer was
		 * just deleted by mod_virt_timer and the interrupt
		 * didn't happen until here
		 */
		vt_list->offset = 0;
		goto fire;
	}

	/* save progress */
	asm volatile ("STPT %0" : "=m" (done));

	/* calculate completed work */
	done = vt_list->to_expire - done + vt_list->offset;
	vt_list->offset = 0;

	list_for_each_entry(event, &vt_list->list, entry)
		event->expires -= done;

 fire:
	list_add_sorted(timer, &vt_list->list);

	/* get first element, which is the next vtimer slice */
	event = list_entry(vt_list->list.next, struct vtimer_list, entry);

	set_vtimer(event->expires);
	spin_unlock_irqrestore(&vt_list->lock, flags);
	/* release CPU aquired in prepare_vtimer or mod_virt_timer() */
	put_cpu();
}

static inline int prepare_vtimer(struct vtimer_list *timer)
{
	if (check_vtimer(timer) || !timer->function) {
		printk("add_virt_timer: uninitialized timer\n");
		return -EINVAL;
	}

	if (!timer->expires || timer->expires > VTIMER_MAX_SLICE) {
		printk("add_virt_timer: invalid timer expire value!\n");
		return -EINVAL;
	}

	if (vtimer_pending(timer)) {
		printk("add_virt_timer: timer pending\n");
		return -EBUSY;
	}

	timer->cpu = get_cpu();
	return 0;
}

/*
 * add_virt_timer - add an oneshot virtual CPU timer
 */
void add_virt_timer(void *new)
{
	struct vtimer_list *timer;

	timer = (struct vtimer_list *)new;

	if (prepare_vtimer(timer) < 0)
		return;

	timer->interval = 0;
	internal_add_vtimer(timer);
}
EXPORT_SYMBOL(add_virt_timer);

/*
 * add_virt_timer_int - add an interval virtual CPU timer
 */
void add_virt_timer_periodic(void *new)
{
	struct vtimer_list *timer;

	timer = (struct vtimer_list *)new;

	if (prepare_vtimer(timer) < 0)
		return;

	timer->interval = timer->expires;
	internal_add_vtimer(timer);
}
EXPORT_SYMBOL(add_virt_timer_periodic);

/*
 * If we change a pending timer the function must be called on the CPU
 * where the timer is running on, e.g. by smp_call_function_on()
 *
 * The original mod_timer adds the timer if it is not pending. For compatibility
 * we do the same. The timer will be added on the current CPU as a oneshot timer.
 *
 * returns whether it has modified a pending timer (1) or not (0)
 */
int mod_virt_timer(struct vtimer_list *timer, __u64 expires)
{
	struct vtimer_queue *vt_list;
	unsigned long flags;
	int cpu;

	if (check_vtimer(timer) || !timer->function) {
		printk("mod_virt_timer: uninitialized timer\n");
		return	-EINVAL;
	}

	if (!expires || expires > VTIMER_MAX_SLICE) {
		printk("mod_virt_timer: invalid expire range\n");
		return -EINVAL;
	}

	/*
	 * This is a common optimization triggered by the
	 * networking code - if the timer is re-modified
	 * to be the same thing then just return:
	 */
	if (timer->expires == expires && vtimer_pending(timer))
		return 1;

	cpu = get_cpu();
	vt_list = &per_cpu(virt_cpu_timer, cpu);

	/* disable interrupts before test if timer is pending */
	spin_lock_irqsave(&vt_list->lock, flags);

	/* if timer isn't pending add it on the current CPU */
	if (!vtimer_pending(timer)) {
		spin_unlock_irqrestore(&vt_list->lock, flags);
		/* we do not activate an interval timer with mod_virt_timer */
		timer->interval = 0;
		timer->expires = expires;
		timer->cpu = cpu;
		internal_add_vtimer(timer);
		return 0;
	}

	/* check if we run on the right CPU */
	if (timer->cpu != cpu) {
		printk("mod_virt_timer: running on wrong CPU, check your code\n");
		spin_unlock_irqrestore(&vt_list->lock, flags);
		put_cpu();
		return -EINVAL;
	}

	list_del_init(&timer->entry);
	timer->expires = expires;

	/* also change the interval if we have an interval timer */
	if (timer->interval)
		timer->interval = expires;

	/* the timer can't expire anymore so we can release the lock */
	spin_unlock_irqrestore(&vt_list->lock, flags);
	internal_add_vtimer(timer);
	return 1;
}
EXPORT_SYMBOL(mod_virt_timer);

/*
 * delete a virtual timer
 *
 * returns whether the deleted timer was pending (1) or not (0)
 */
int del_virt_timer(struct vtimer_list *timer)
{
	unsigned long flags;
	struct vtimer_queue *vt_list;

	if (check_vtimer(timer)) {
		printk("del_virt_timer: timer not initialized\n");
		return -EINVAL;
	}

	/* check if timer is pending */
	if (!vtimer_pending(timer))
		return 0;

	vt_list = &per_cpu(virt_cpu_timer, timer->cpu);
	spin_lock_irqsave(&vt_list->lock, flags);

	/* we don't interrupt a running timer, just let it expire! */
	list_del_init(&timer->entry);

	/* last timer removed */
	if (list_empty(&vt_list->list)) {
		vt_list->to_expire = 0;
		vt_list->offset = 0;
	}

	spin_unlock_irqrestore(&vt_list->lock, flags);
	return 1;
}
EXPORT_SYMBOL(del_virt_timer);

/*
 * Start the virtual CPU timer on the current CPU.
 */
void init_cpu_vtimer(void)
{
	struct vtimer_queue *vt_list;
	unsigned long cr0;
	__u64 timer;

	/* kick the virtual timer */
	timer = VTIMER_MAX_SLICE;
	asm volatile ("SPT %0" : : "m" (timer));
	__ctl_store(cr0, 0, 0);
	cr0 |= 0x400;
	__ctl_load(cr0, 0, 0);

	vt_list = &per_cpu(virt_cpu_timer, smp_processor_id());
	INIT_LIST_HEAD(&vt_list->list);
	spin_lock_init(&vt_list->lock);
	vt_list->to_expire = 0;
	vt_list->offset = 0;
	vt_list->idle = 0;

}

static int vtimer_idle_notify(struct notifier_block *self,
			      unsigned long action, void *hcpu)
{
	switch (action) {
	case CPU_IDLE:
		stop_cpu_timer();
		break;
	case CPU_NOT_IDLE:
		start_cpu_timer();
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block vtimer_idle_nb = {
	.notifier_call = vtimer_idle_notify,
};

void __init vtime_init(void)
{
	/* request the cpu timer external interrupt */
	if (register_early_external_interrupt(0x1005, do_cpu_timer_interrupt,
					      &ext_int_info_timer) != 0)
		panic("Couldn't request external interrupt 0x1005");

	if (register_idle_notifier(&vtimer_idle_nb))
		panic("Couldn't register idle notifier");

	init_cpu_vtimer();
}

