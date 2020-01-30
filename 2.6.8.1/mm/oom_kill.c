/*
 *  linux/mm/oom_kill.c
 * 
 *  Copyright (C)  1998,2000  Rik van Riel
 *	Thanks go out to Claus Fischer for some serious inspiration and
 *	for goading me into coding this file...
 *
 *  The routines in this file are used to kill a process when
 *  we're seriously out of memory. This gets called from kswapd()
 *  in linux/mm/vmscan.c when we really run out of memory.
 *
 *  Since we won't call these routines often (on a well-configured
 *  machine) this file will double as a 'coding guide' and a signpost
 *  for newbie kernel hackers. It features several pointers to major
 *  kernel subsystems and hints as to where to find out what things do.
 */

#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/swap.h>
#include <linux/timex.h>
#include <linux/jiffies.h>

/* #define DEBUG */

/**
 * oom_badness - calculate a numeric value for how bad this task has been
 * @p: task struct of which task we should calculate
 *
 * The formula used is relatively simple and documented inline in the
 * function. The main rationale is that we want to select a good task
 * to kill when we run out of memory.
 *
 * Good in this context means that:
 * 1) we lose the minimum amount of work done
 * 2) we recover a large amount of memory
 * 3) we don't kill anything innocent of eating tons of memory
 * 4) we want to kill the minimum amount of processes (one)
 * 5) we try to kill the process the user expects us to kill, this
 *    algorithm has been meticulously tuned to meet the principle
 *    of least surprise ... (be careful when you change it)
 */

static int badness(struct task_struct *p)
{
	int points, cpu_time, run_time, s;

	if (!p->mm)
		return 0;

	if (p->flags & PF_MEMDIE)
		return 0;
	/*
	 * The memory size of the process is the basis for the badness.
	 */
	points = p->mm->total_vm;

	/*
	 * CPU time is in seconds and run time is in minutes. There is no
	 * particular reason for this other than that it turned out to work
	 * very well in practice.
	 */
	cpu_time = (p->utime + p->stime) >> (SHIFT_HZ + 3);
	run_time = (get_jiffies_64() - p->start_time) >> (SHIFT_HZ + 10);

	s = int_sqrt(cpu_time);
	if (s)
		points /= s;
	s = int_sqrt(int_sqrt(run_time));
	if (s)
		points /= s;

	/*
	 * Niced processes are most likely less important, so double
	 * their badness points.
	 */
	if (task_nice(p) > 0)
		points *= 2;

	/*
	 * Superuser processes are usually more important, so we make it
	 * less likely that we kill those.
	 */
	if (cap_t(p->cap_effective) & CAP_TO_MASK(CAP_SYS_ADMIN) ||
				p->uid == 0 || p->euid == 0)
		points /= 4;

	/*
	 * We don't want to kill a process with direct hardware access.
	 * Not only could that mess up the hardware, but usually users
	 * tend to only have this flag set on applications they think
	 * of as important.
	 */
	if (cap_t(p->cap_effective) & CAP_TO_MASK(CAP_SYS_RAWIO))
		points /= 4;
#ifdef DEBUG
	printk(KERN_DEBUG "OOMkill: task %d (%s) got %d points\n",
	p->pid, p->comm, points);
#endif
	return points;
}

/*
 * Simple selection loop. We chose the process with the highest
 * number of 'points'. We expect the caller will lock the tasklist.
 *
 * (not docbooked, we don't want this one cluttering up the manual)
 */
static struct task_struct * select_bad_process(void)
{
	int maxpoints = 0;
	struct task_struct *g, *p;
	struct task_struct *chosen = NULL;

	do_each_thread(g, p)
		if (p->pid) {
			int points = badness(p);
			if (points > maxpoints) {
				chosen = p;
				maxpoints = points;
			}
			if (p->flags & PF_SWAPOFF)
				return p;
		}
	while_each_thread(g, p);
	return chosen;
}

/**
 * We must be careful though to never send SIGKILL a process with
 * CAP_SYS_RAW_IO set, send SIGTERM instead (but it's unlikely that
 * we select a process with CAP_SYS_RAW_IO set).
 */
static void __oom_kill_task(task_t *p)
{
	task_lock(p);
	if (!p->mm || p->mm == &init_mm) {
		WARN_ON(1);
		printk(KERN_WARNING "tried to kill an mm-less task!\n");
		task_unlock(p);
		return;
	}
	task_unlock(p);
	printk(KERN_ERR "Out of Memory: Killed process %d (%s).\n", p->pid, p->comm);

	/*
	 * We give our sacrificial lamb high priority and access to
	 * all the memory it needs. That way it should be able to
	 * exit() and clear out its resources quickly...
	 */
	p->time_slice = HZ;
	p->flags |= PF_MEMALLOC | PF_MEMDIE;

	/* This process has hardware access, be more careful. */
	if (cap_t(p->cap_effective) & CAP_TO_MASK(CAP_SYS_RAWIO)) {
		force_sig(SIGTERM, p);
	} else {
		force_sig(SIGKILL, p);
	}
}

static struct mm_struct *oom_kill_task(task_t *p)
{
	struct mm_struct *mm = get_task_mm(p);
	if (!mm || mm == &init_mm)
		return NULL;
	__oom_kill_task(p);
	return mm;
}


/**
 * oom_kill - kill the "best" process when we run out of memory
 *
 * If we run out of memory, we have the choice between either
 * killing a random task (bad), letting the system crash (worse)
 * OR try to be smart about which process to kill. Note that we
 * don't have to be perfect here, we just have to be good.
 */
static void oom_kill(void)
{
	struct mm_struct *mm;
	struct task_struct *g, *p, *q;
	
	read_lock(&tasklist_lock);
retry:
	p = select_bad_process();

	/* Found nothing?!?! Either we hang forever, or we panic. */
	if (!p) {
		show_free_areas();
		panic("Out of memory and no killable processes...\n");
	}

	mm = oom_kill_task(p);
	if (!mm)
		goto retry;
	/*
	 * kill all processes that share the ->mm (i.e. all threads),
	 * but are in a different thread group
	 */
	do_each_thread(g, q)
		if (q->mm == mm && q->tgid != p->tgid)
			__oom_kill_task(q);
	while_each_thread(g, q);
	if (!p->mm)
		printk(KERN_INFO "Fixed up OOM kill of mm-less task\n");
	read_unlock(&tasklist_lock);
	mmput(mm);

	/*
	 * Make kswapd go out of the way, so "p" has a good chance of
	 * killing itself before someone else gets the chance to ask
	 * for more memory.
	 */
	yield();
	return;
}

/**
 * out_of_memory - is the system out of memory?
 */
void out_of_memory(int gfp_mask)
{
	/*
	 * oom_lock protects out_of_memory()'s static variables.
	 * It's a global lock; this is not performance-critical.
	 */
	static spinlock_t oom_lock = SPIN_LOCK_UNLOCKED;
	static unsigned long first, last, count, lastkill;
	unsigned long now, since;

	spin_lock(&oom_lock);
	now = jiffies;
	since = now - last;
	last = now;

	/*
	 * If it's been a long time since last failure,
	 * we're not oom.
	 */
	if (since > 5*HZ)
		goto reset;

	/*
	 * If we haven't tried for at least one second,
	 * we're not really oom.
	 */
	since = now - first;
	if (since < HZ)
		goto out_unlock;

	/*
	 * If we have gotten only a few failures,
	 * we're not really oom. 
	 */
	if (++count < 10)
		goto out_unlock;

	/*
	 * If we just killed a process, wait a while
	 * to give that task a chance to exit. This
	 * avoids killing multiple processes needlessly.
	 */
	since = now - lastkill;
	if (since < HZ*5)
		goto out_unlock;

	/*
	 * Ok, really out of memory. Kill something.
	 */
	lastkill = now;

	printk("oom-killer: gfp_mask=0x%x\n", gfp_mask);
	show_free_areas();

	/* oom_kill() sleeps */
	spin_unlock(&oom_lock);
	oom_kill();
	spin_lock(&oom_lock);

reset:
	/*
	 * We dropped the lock above, so check to be sure the variable
	 * first only ever increases to prevent false OOM's.
	 */
	if (time_after(now, first))
		first = now;
	count = 0;

out_unlock:
	spin_unlock(&oom_lock);
}
