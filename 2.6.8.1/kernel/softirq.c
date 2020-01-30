/*
 *	linux/kernel/softirq.c
 *
 *	Copyright (C) 1992 Linus Torvalds
 *
 * Rewritten. Old one was good in 2.2, but in 2.3 it was immoral. --ANK (990903)
 */

#include <linux/module.h>
#include <linux/kernel_stat.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/notifier.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/kthread.h>

#include <asm/irq.h>
/*
   - No shared variables, all the data are CPU local.
   - If a softirq needs serialization, let it serialize itself
     by its own spinlocks.
   - Even if softirq is serialized, only local cpu is marked for
     execution. Hence, we get something sort of weak cpu binding.
     Though it is still not clear, will it result in better locality
     or will not.

   Examples:
   - NET RX softirq. It is multithreaded and does not require
     any global serialization.
   - NET TX softirq. It kicks software netdevice queues, hence
     it is logically serialized per device, but this serialization
     is invisible to common code.
   - Tasklets: serialized wrt itself.
 */

#ifndef __ARCH_IRQ_STAT
irq_cpustat_t irq_stat[NR_CPUS] ____cacheline_aligned;
EXPORT_SYMBOL(irq_stat);
#endif

static struct softirq_action softirq_vec[32] __cacheline_aligned_in_smp;

static DEFINE_PER_CPU(struct task_struct *, ksoftirqd);

/*
 * we cannot loop indefinitely here to avoid userspace starvation,
 * but we also don't want to introduce a worst case 1/HZ latency
 * to the pending events, so lets the scheduler to balance
 * the softirq load for us.
 */
static inline void wakeup_softirqd(void)
{
	/* Interrupts are disabled: no need to stop preemption */
	struct task_struct *tsk = __get_cpu_var(ksoftirqd);

	if (tsk && tsk->state != TASK_RUNNING)
		wake_up_process(tsk);
}

/*
 * We restart softirq processing MAX_SOFTIRQ_RESTART times,
 * and we fall back to softirqd after that.
 *
 * This number has been established via experimentation.
 * The two things to balance is latency against fairness -
 * we want to handle softirqs as soon as possible, but they
 * should not be able to lock up the box.
 */
#define MAX_SOFTIRQ_RESTART 10

asmlinkage void __do_softirq(void)
{
	struct softirq_action *h;
	__u32 pending;
	int max_restart = MAX_SOFTIRQ_RESTART;

	pending = local_softirq_pending();

	local_bh_disable();
restart:
	/* Reset the pending bitmask before enabling irqs */
	local_softirq_pending() = 0;

	local_irq_enable();

	h = softirq_vec;

	do {
		if (pending & 1)
			h->action(h);
		h++;
		pending >>= 1;
	} while (pending);

	local_irq_disable();

	pending = local_softirq_pending();
	if (pending && --max_restart)
		goto restart;

	if (pending)
		wakeup_softirqd();

	__local_bh_enable();
}

#ifndef __ARCH_HAS_DO_SOFTIRQ

asmlinkage void do_softirq(void)
{
	__u32 pending;
	unsigned long flags;

	if (in_interrupt())
		return;

	local_irq_save(flags);

	pending = local_softirq_pending();

	if (pending)
		__do_softirq();

	local_irq_restore(flags);
}

EXPORT_SYMBOL(do_softirq);

#endif

void local_bh_enable(void)
{
	__local_bh_enable();
	WARN_ON(irqs_disabled());
	if (unlikely(!in_interrupt() &&
		     local_softirq_pending()))
		invoke_softirq();
	preempt_check_resched();
}
EXPORT_SYMBOL(local_bh_enable);

/*
 * This function must run with irqs disabled!
 */
inline fastcall void raise_softirq_irqoff(unsigned int nr)
{
	__raise_softirq_irqoff(nr);

	/*
	 * If we're in an interrupt or softirq, we're done
	 * (this also catches softirq-disabled code). We will
	 * actually run the softirq once we return from
	 * the irq or softirq.
	 *
	 * Otherwise we wake up ksoftirqd to make sure we
	 * schedule the softirq soon.
	 */
	if (!in_interrupt())
		wakeup_softirqd();
}

EXPORT_SYMBOL(raise_softirq_irqoff);

void fastcall raise_softirq(unsigned int nr)
{
	unsigned long flags;

	local_irq_save(flags);
	raise_softirq_irqoff(nr);
	local_irq_restore(flags);
}

EXPORT_SYMBOL(raise_softirq);

void open_softirq(int nr, void (*action)(struct softirq_action*), void *data)
{
	softirq_vec[nr].data = data;
	softirq_vec[nr].action = action;
}

EXPORT_SYMBOL(open_softirq);

/* Tasklets */
struct tasklet_head
{
	struct tasklet_struct *list;
};

/* Some compilers disobey section attribute on statics when not
   initialized -- RR */
static DEFINE_PER_CPU(struct tasklet_head, tasklet_vec) = { NULL };
static DEFINE_PER_CPU(struct tasklet_head, tasklet_hi_vec) = { NULL };

void fastcall __tasklet_schedule(struct tasklet_struct *t)
{
	unsigned long flags;

	local_irq_save(flags);
	t->next = __get_cpu_var(tasklet_vec).list;
	__get_cpu_var(tasklet_vec).list = t;
	raise_softirq_irqoff(TASKLET_SOFTIRQ);
	local_irq_restore(flags);
}

EXPORT_SYMBOL(__tasklet_schedule);

void fastcall __tasklet_hi_schedule(struct tasklet_struct *t)
{
	unsigned long flags;

	local_irq_save(flags);
	t->next = __get_cpu_var(tasklet_hi_vec).list;
	__get_cpu_var(tasklet_hi_vec).list = t;
	raise_softirq_irqoff(HI_SOFTIRQ);
	local_irq_restore(flags);
}

EXPORT_SYMBOL(__tasklet_hi_schedule);

static void tasklet_action(struct softirq_action *a)
{
	struct tasklet_struct *list;

	local_irq_disable();
	list = __get_cpu_var(tasklet_vec).list;
	__get_cpu_var(tasklet_vec).list = NULL;
	local_irq_enable();

	while (list) {
		struct tasklet_struct *t = list;

		list = list->next;

		if (tasklet_trylock(t)) {
			if (!atomic_read(&t->count)) {
				if (!test_and_clear_bit(TASKLET_STATE_SCHED, &t->state))
					BUG();
				t->func(t->data);
				tasklet_unlock(t);
				continue;
			}
			tasklet_unlock(t);
		}

		local_irq_disable();
		t->next = __get_cpu_var(tasklet_vec).list;
		__get_cpu_var(tasklet_vec).list = t;
		__raise_softirq_irqoff(TASKLET_SOFTIRQ);
		local_irq_enable();
	}
}

static void tasklet_hi_action(struct softirq_action *a)
{
	struct tasklet_struct *list;

	local_irq_disable();
	list = __get_cpu_var(tasklet_hi_vec).list;
	__get_cpu_var(tasklet_hi_vec).list = NULL;
	local_irq_enable();

	while (list) {
		struct tasklet_struct *t = list;

		list = list->next;

		if (tasklet_trylock(t)) {
			if (!atomic_read(&t->count)) {
				if (!test_and_clear_bit(TASKLET_STATE_SCHED, &t->state))
					BUG();
				t->func(t->data);
				tasklet_unlock(t);
				continue;
			}
			tasklet_unlock(t);
		}

		local_irq_disable();
		t->next = __get_cpu_var(tasklet_hi_vec).list;
		__get_cpu_var(tasklet_hi_vec).list = t;
		__raise_softirq_irqoff(HI_SOFTIRQ);
		local_irq_enable();
	}
}


void tasklet_init(struct tasklet_struct *t,
		  void (*func)(unsigned long), unsigned long data)
{
	t->next = NULL;
	t->state = 0;
	atomic_set(&t->count, 0);
	t->func = func;
	t->data = data;
}

EXPORT_SYMBOL(tasklet_init);

void tasklet_kill(struct tasklet_struct *t)
{
	if (in_interrupt())
		printk("Attempt to kill tasklet from interrupt\n");

	while (test_and_set_bit(TASKLET_STATE_SCHED, &t->state)) {
		do
			yield();
		while (test_bit(TASKLET_STATE_SCHED, &t->state));
	}
	tasklet_unlock_wait(t);
	clear_bit(TASKLET_STATE_SCHED, &t->state);
}

EXPORT_SYMBOL(tasklet_kill);

void __init softirq_init(void)
{
	open_softirq(TASKLET_SOFTIRQ, tasklet_action, NULL);
	open_softirq(HI_SOFTIRQ, tasklet_hi_action, NULL);
}

static int ksoftirqd(void * __bind_cpu)
{
	set_user_nice(current, 19);
	current->flags |= PF_NOFREEZE;

	set_current_state(TASK_INTERRUPTIBLE);

	while (!kthread_should_stop()) {
		if (!local_softirq_pending())
			schedule();

		__set_current_state(TASK_RUNNING);

		while (local_softirq_pending()) {
			/* Preempt disable stops cpu going offline.
			   If already offline, we'll be on wrong CPU:
			   don't process */
			preempt_disable();
			if (cpu_is_offline((long)__bind_cpu))
				goto wait_to_die;
			do_softirq();
			preempt_enable();
			cond_resched();
		}

		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	return 0;

wait_to_die:
	preempt_enable();
	/* Wait for kthread_stop */
	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * tasklet_kill_immediate is called to remove a tasklet which can already be
 * scheduled for execution on @cpu.
 *
 * Unlike tasklet_kill, this function removes the tasklet
 * _immediately_, even if the tasklet is in TASKLET_STATE_SCHED state.
 *
 * When this function is called, @cpu must be in the CPU_DEAD state.
 */
void tasklet_kill_immediate(struct tasklet_struct *t, unsigned int cpu)
{
	struct tasklet_struct **i;

	BUG_ON(cpu_online(cpu));
	BUG_ON(test_bit(TASKLET_STATE_RUN, &t->state));

	if (!test_bit(TASKLET_STATE_SCHED, &t->state))
		return;

	/* CPU is dead, so no lock needed. */
	for (i = &per_cpu(tasklet_vec, cpu).list; *i; i = &(*i)->next) {
		if (*i == t) {
			*i = t->next;
			return;
		}
	}
	BUG();
}

static void takeover_tasklets(unsigned int cpu)
{
	struct tasklet_struct **i;

	/* CPU is dead, so no lock needed. */
	local_irq_disable();

	/* Find end, append list for that CPU. */
	for (i = &__get_cpu_var(tasklet_vec).list; *i; i = &(*i)->next);
	*i = per_cpu(tasklet_vec, cpu).list;
	per_cpu(tasklet_vec, cpu).list = NULL;
	raise_softirq_irqoff(TASKLET_SOFTIRQ);

	for (i = &__get_cpu_var(tasklet_hi_vec).list; *i; i = &(*i)->next);
	*i = per_cpu(tasklet_hi_vec, cpu).list;
	per_cpu(tasklet_hi_vec, cpu).list = NULL;
	raise_softirq_irqoff(HI_SOFTIRQ);

	local_irq_enable();
}
#endif /* CONFIG_HOTPLUG_CPU */

static int __devinit cpu_callback(struct notifier_block *nfb,
				  unsigned long action,
				  void *hcpu)
{
	int hotcpu = (unsigned long)hcpu;
	struct task_struct *p;

	switch (action) {
	case CPU_UP_PREPARE:
		BUG_ON(per_cpu(tasklet_vec, hotcpu).list);
		BUG_ON(per_cpu(tasklet_hi_vec, hotcpu).list);
		p = kthread_create(ksoftirqd, hcpu, "ksoftirqd/%d", hotcpu);
		if (IS_ERR(p)) {
			printk("ksoftirqd for %i failed\n", hotcpu);
			return NOTIFY_BAD;
		}
		kthread_bind(p, hotcpu);
  		per_cpu(ksoftirqd, hotcpu) = p;
 		break;
	case CPU_ONLINE:
		wake_up_process(per_cpu(ksoftirqd, hotcpu));
		break;
#ifdef CONFIG_HOTPLUG_CPU
	case CPU_UP_CANCELED:
		/* Unbind so it can run.  Fall thru. */
		kthread_bind(per_cpu(ksoftirqd, hotcpu), smp_processor_id());
	case CPU_DEAD:
		p = per_cpu(ksoftirqd, hotcpu);
		per_cpu(ksoftirqd, hotcpu) = NULL;
		kthread_stop(p);
		takeover_tasklets(hotcpu);
		break;
#endif /* CONFIG_HOTPLUG_CPU */
 	}
	return NOTIFY_OK;
}

static struct notifier_block __devinitdata cpu_nfb = {
	.notifier_call = cpu_callback
};

__init int spawn_ksoftirqd(void)
{
	void *cpu = (void *)(long)smp_processor_id();
	cpu_callback(&cpu_nfb, CPU_UP_PREPARE, cpu);
	cpu_callback(&cpu_nfb, CPU_ONLINE, cpu);
	register_cpu_notifier(&cpu_nfb);
	return 0;
}
