/* Kernel thread helper functions.
 *   Copyright (C) 2004 IBM Corporation, Rusty Russell.
 *
 * Creation is done via keventd, so that we get a clean environment
 * even if we're invoked from userspace (think modprobe, hotplug cpu,
 * etc.).
 */
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/unistd.h>
#include <linux/file.h>
#include <linux/module.h>
#include <asm/semaphore.h>

struct kthread_create_info
{
	/* Information passed to kthread() from keventd. */
	int (*threadfn)(void *data);
	void *data;
	struct completion started;

	/* Result passed back to kthread_create() from keventd. */
	struct task_struct *result;
	struct completion done;
};

struct kthread_stop_info
{
	struct task_struct *k;
	int err;
	struct completion done;
};

/* Thread stopping is done by setthing this var: lock serializes
 * multiple kthread_stop calls. */
static DECLARE_MUTEX(kthread_stop_lock);
static struct kthread_stop_info kthread_stop_info;

int kthread_should_stop(void)
{
	return (kthread_stop_info.k == current);
}
EXPORT_SYMBOL(kthread_should_stop);

static void kthread_exit_files(void)
{
	struct fs_struct *fs;
	struct task_struct *tsk = current;

	exit_fs(tsk);		/* current->fs->count--; */
	fs = init_task.fs;
	tsk->fs = fs;
	atomic_inc(&fs->count);
 	exit_files(tsk);
	current->files = init_task.files;
	atomic_inc(&tsk->files->count);
}

static int kthread(void *_create)
{
	struct kthread_create_info *create = _create;
	int (*threadfn)(void *data);
	void *data;
	sigset_t blocked;
	int ret = -EINTR;

	kthread_exit_files();

	/* Copy data: it's on keventd's stack */
	threadfn = create->threadfn;
	data = create->data;

	/* Block and flush all signals (in case we're not from keventd). */
	sigfillset(&blocked);
	sigprocmask(SIG_BLOCK, &blocked, NULL);
	flush_signals(current);

	/* By default we can run anywhere, unlike keventd. */
	set_cpus_allowed(current, CPU_MASK_ALL);

	/* OK, tell user we're spawned, wait for stop or wakeup */
	__set_current_state(TASK_INTERRUPTIBLE);
	complete(&create->started);
	schedule();

	if (!kthread_should_stop())
		ret = threadfn(data);

	/* It might have exited on its own, w/o kthread_stop.  Check. */
	if (kthread_should_stop()) {
		kthread_stop_info.err = ret;
		complete(&kthread_stop_info.done);
	}
	return 0;
}

/* We are keventd: create a thread. */
static void keventd_create_kthread(void *_create)
{
	struct kthread_create_info *create = _create;
	int pid;

	/* We want our own signal handler (we take no signals by default). */
	pid = kernel_thread(kthread, create, CLONE_FS | CLONE_FILES | SIGCHLD);
	if (pid < 0) {
		create->result = ERR_PTR(pid);
	} else {
		wait_for_completion(&create->started);
		create->result = find_task_by_pid(pid);
	}
	complete(&create->done);
}

struct task_struct *kthread_create(int (*threadfn)(void *data),
				   void *data,
				   const char namefmt[],
				   ...)
{
	struct kthread_create_info create;
	DECLARE_WORK(work, keventd_create_kthread, &create);

	create.threadfn = threadfn;
	create.data = data;
	init_completion(&create.started);
	init_completion(&create.done);

	/* If we're being called to start the first workqueue, we
	 * can't use keventd. */
	if (!keventd_up())
		work.func(work.data);
	else {
		schedule_work(&work);
		wait_for_completion(&create.done);
	}
	if (!IS_ERR(create.result)) {
		va_list args;
		va_start(args, namefmt);
		vsnprintf(create.result->comm, sizeof(create.result->comm),
			  namefmt, args);
		va_end(args);
	}

	return create.result;
}
EXPORT_SYMBOL(kthread_create);

void kthread_bind(struct task_struct *k, unsigned int cpu)
{
	BUG_ON(k->state != TASK_INTERRUPTIBLE);
	/* Must have done schedule() in kthread() before we set_task_cpu */
	wait_task_inactive(k);
	set_task_cpu(k, cpu);
	k->cpus_allowed = cpumask_of_cpu(cpu);
}
EXPORT_SYMBOL(kthread_bind);

int kthread_stop(struct task_struct *k)
{
	int ret;

	down(&kthread_stop_lock);

	/* It could exit after stop_info.k set, but before wake_up_process. */
	get_task_struct(k);

	/* Must init completion *before* thread sees kthread_stop_info.k */
	init_completion(&kthread_stop_info.done);
	wmb();

	/* Now set kthread_should_stop() to true, and wake it up. */
	kthread_stop_info.k = k;
	wake_up_process(k);
	put_task_struct(k);

	/* Once it dies, reset stop ptr, gather result and we're done. */
	wait_for_completion(&kthread_stop_info.done);
	kthread_stop_info.k = NULL;
	ret = kthread_stop_info.err;
	up(&kthread_stop_lock);

	return ret;
}
EXPORT_SYMBOL(kthread_stop);
