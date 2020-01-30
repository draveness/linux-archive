/* rwsem-spinlock.c: R/W semaphores: contention handling functions for
 * generic spinlock implementation
 *
 * Copyright (c) 2001   David Howells (dhowells@redhat.com).
 * - Derived partially from idea by Andrea Arcangeli <andrea@suse.de>
 * - Derived also from comments by Linus
 */
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/module.h>

struct rwsem_waiter {
	struct list_head list;
	struct task_struct *task;
	unsigned int flags;
#define RWSEM_WAITING_FOR_READ	0x00000001
#define RWSEM_WAITING_FOR_WRITE	0x00000002
};

#if RWSEM_DEBUG
void rwsemtrace(struct rw_semaphore *sem, const char *str)
{
	if (sem->debug)
		printk("[%d] %s({%d,%d})\n",
		       current->pid, str, sem->activity,
		       list_empty(&sem->wait_list) ? 0 : 1);
}
#endif

/*
 * initialise the semaphore
 */
void fastcall init_rwsem(struct rw_semaphore *sem)
{
	sem->activity = 0;
	spin_lock_init(&sem->wait_lock);
	INIT_LIST_HEAD(&sem->wait_list);
#if RWSEM_DEBUG
	sem->debug = 0;
#endif
}

/*
 * handle the lock release when processes blocked on it that can now run
 * - if we come here, then:
 *   - the 'active count' _reached_ zero
 *   - the 'waiting count' is non-zero
 * - the spinlock must be held by the caller
 * - woken process blocks are discarded from the list after having task zeroed
 * - writers are only woken if wakewrite is non-zero
 */
static inline struct rw_semaphore *
__rwsem_do_wake(struct rw_semaphore *sem, int wakewrite)
{
	struct rwsem_waiter *waiter;
	struct task_struct *tsk;
	int woken;

	rwsemtrace(sem, "Entering __rwsem_do_wake");

	waiter = list_entry(sem->wait_list.next, struct rwsem_waiter, list);

	if (!wakewrite) {
		if (waiter->flags & RWSEM_WAITING_FOR_WRITE)
			goto out;
		goto dont_wake_writers;
	}

	/* if we are allowed to wake writers try to grant a single write lock
	 * if there's a writer at the front of the queue
	 * - we leave the 'waiting count' incremented to signify potential
	 *   contention
	 */
	if (waiter->flags & RWSEM_WAITING_FOR_WRITE) {
		sem->activity = -1;
		list_del(&waiter->list);
		tsk = waiter->task;
		/* Don't touch waiter after ->task has been NULLed */
		mb();
		waiter->task = NULL;
		wake_up_process(tsk);
		put_task_struct(tsk);
		goto out;
	}

	/* grant an infinite number of read locks to the front of the queue */
 dont_wake_writers:
	woken = 0;
	while (waiter->flags & RWSEM_WAITING_FOR_READ) {
		struct list_head *next = waiter->list.next;

		list_del(&waiter->list);
		tsk = waiter->task;
		mb();
		waiter->task = NULL;
		wake_up_process(tsk);
		put_task_struct(tsk);
		woken++;
		if (list_empty(&sem->wait_list))
			break;
		waiter = list_entry(next, struct rwsem_waiter, list);
	}

	sem->activity += woken;

 out:
	rwsemtrace(sem, "Leaving __rwsem_do_wake");
	return sem;
}

/*
 * wake a single writer
 */
static inline struct rw_semaphore *
__rwsem_wake_one_writer(struct rw_semaphore *sem)
{
	struct rwsem_waiter *waiter;
	struct task_struct *tsk;

	sem->activity = -1;

	waiter = list_entry(sem->wait_list.next, struct rwsem_waiter, list);
	list_del(&waiter->list);

	tsk = waiter->task;
	mb();
	waiter->task = NULL;
	wake_up_process(tsk);
	put_task_struct(tsk);
	return sem;
}

/*
 * get a read lock on the semaphore
 */
void fastcall __sched __down_read(struct rw_semaphore *sem)
{
	struct rwsem_waiter waiter;
	struct task_struct *tsk;

	rwsemtrace(sem, "Entering __down_read");

	spin_lock(&sem->wait_lock);

	if (sem->activity >= 0 && list_empty(&sem->wait_list)) {
		/* granted */
		sem->activity++;
		spin_unlock(&sem->wait_lock);
		goto out;
	}

	tsk = current;
	set_task_state(tsk, TASK_UNINTERRUPTIBLE);

	/* set up my own style of waitqueue */
	waiter.task = tsk;
	waiter.flags = RWSEM_WAITING_FOR_READ;
	get_task_struct(tsk);

	list_add_tail(&waiter.list, &sem->wait_list);

	/* we don't need to touch the semaphore struct anymore */
	spin_unlock(&sem->wait_lock);

	/* wait to be given the lock */
	for (;;) {
		if (!waiter.task)
			break;
		schedule();
		set_task_state(tsk, TASK_UNINTERRUPTIBLE);
	}

	tsk->state = TASK_RUNNING;

 out:
	rwsemtrace(sem, "Leaving __down_read");
}

/*
 * trylock for reading -- returns 1 if successful, 0 if contention
 */
int fastcall __down_read_trylock(struct rw_semaphore *sem)
{
	int ret = 0;
	rwsemtrace(sem, "Entering __down_read_trylock");

	spin_lock(&sem->wait_lock);

	if (sem->activity >= 0 && list_empty(&sem->wait_list)) {
		/* granted */
		sem->activity++;
		ret = 1;
	}

	spin_unlock(&sem->wait_lock);

	rwsemtrace(sem, "Leaving __down_read_trylock");
	return ret;
}

/*
 * get a write lock on the semaphore
 * - we increment the waiting count anyway to indicate an exclusive lock
 */
void fastcall __sched __down_write(struct rw_semaphore *sem)
{
	struct rwsem_waiter waiter;
	struct task_struct *tsk;

	rwsemtrace(sem, "Entering __down_write");

	spin_lock(&sem->wait_lock);

	if (sem->activity == 0 && list_empty(&sem->wait_list)) {
		/* granted */
		sem->activity = -1;
		spin_unlock(&sem->wait_lock);
		goto out;
	}

	tsk = current;
	set_task_state(tsk, TASK_UNINTERRUPTIBLE);

	/* set up my own style of waitqueue */
	waiter.task = tsk;
	waiter.flags = RWSEM_WAITING_FOR_WRITE;
	get_task_struct(tsk);

	list_add_tail(&waiter.list, &sem->wait_list);

	/* we don't need to touch the semaphore struct anymore */
	spin_unlock(&sem->wait_lock);

	/* wait to be given the lock */
	for (;;) {
		if (!waiter.task)
			break;
		schedule();
		set_task_state(tsk, TASK_UNINTERRUPTIBLE);
	}

	tsk->state = TASK_RUNNING;

 out:
	rwsemtrace(sem, "Leaving __down_write");
}

/*
 * trylock for writing -- returns 1 if successful, 0 if contention
 */
int fastcall __down_write_trylock(struct rw_semaphore *sem)
{
	int ret = 0;
	rwsemtrace(sem, "Entering __down_write_trylock");

	spin_lock(&sem->wait_lock);

	if (sem->activity == 0 && list_empty(&sem->wait_list)) {
		/* granted */
		sem->activity = -1;
		ret = 1;
	}

	spin_unlock(&sem->wait_lock);

	rwsemtrace(sem, "Leaving __down_write_trylock");
	return ret;
}

/*
 * release a read lock on the semaphore
 */
void fastcall __up_read(struct rw_semaphore *sem)
{
	rwsemtrace(sem, "Entering __up_read");

	spin_lock(&sem->wait_lock);

	if (--sem->activity == 0 && !list_empty(&sem->wait_list))
		sem = __rwsem_wake_one_writer(sem);

	spin_unlock(&sem->wait_lock);

	rwsemtrace(sem, "Leaving __up_read");
}

/*
 * release a write lock on the semaphore
 */
void fastcall __up_write(struct rw_semaphore *sem)
{
	rwsemtrace(sem, "Entering __up_write");

	spin_lock(&sem->wait_lock);

	sem->activity = 0;
	if (!list_empty(&sem->wait_list))
		sem = __rwsem_do_wake(sem, 1);

	spin_unlock(&sem->wait_lock);

	rwsemtrace(sem, "Leaving __up_write");
}

/*
 * downgrade a write lock into a read lock
 * - just wake up any readers at the front of the queue
 */
void fastcall __downgrade_write(struct rw_semaphore *sem)
{
	rwsemtrace(sem, "Entering __downgrade_write");

	spin_lock(&sem->wait_lock);

	sem->activity = 1;
	if (!list_empty(&sem->wait_list))
		sem = __rwsem_do_wake(sem, 0);

	spin_unlock(&sem->wait_lock);

	rwsemtrace(sem, "Leaving __downgrade_write");
}

EXPORT_SYMBOL(init_rwsem);
EXPORT_SYMBOL(__down_read);
EXPORT_SYMBOL(__down_read_trylock);
EXPORT_SYMBOL(__down_write);
EXPORT_SYMBOL(__down_write_trylock);
EXPORT_SYMBOL(__up_read);
EXPORT_SYMBOL(__up_write);
EXPORT_SYMBOL(__downgrade_write);
#if RWSEM_DEBUG
EXPORT_SYMBOL(rwsemtrace);
#endif
