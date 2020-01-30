/* $Id: semaphore.h,v 1.3 2001/05/08 13:54:09 bjornw Exp $ */

/* On the i386 these are coded in asm, perhaps we should as well. Later.. */

#ifndef _CRIS_SEMAPHORE_H
#define _CRIS_SEMAPHORE_H

#define RW_LOCK_BIAS             0x01000000

#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/rwsem.h>

#include <asm/system.h>
#include <asm/atomic.h>

/*
 * CRIS semaphores, implemented in C-only so far. 
 */

int printk(const char *fmt, ...);

struct semaphore {
	atomic_t count;
	atomic_t waking;
	wait_queue_head_t wait;
#if WAITQUEUE_DEBUG
	long __magic;
#endif
};

#if WAITQUEUE_DEBUG
# define __SEM_DEBUG_INIT(name)         , (long)&(name).__magic
#else
# define __SEM_DEBUG_INIT(name)
#endif

#define __SEMAPHORE_INITIALIZER(name,count)             \
        { ATOMIC_INIT(count), ATOMIC_INIT(0),           \
          __WAIT_QUEUE_HEAD_INITIALIZER((name).wait)    \
          __SEM_DEBUG_INIT(name) }

#define __MUTEX_INITIALIZER(name) \
        __SEMAPHORE_INITIALIZER(name,1)

#define __DECLARE_SEMAPHORE_GENERIC(name,count) \
        struct semaphore name = __SEMAPHORE_INITIALIZER(name,count)

#define DECLARE_MUTEX(name) __DECLARE_SEMAPHORE_GENERIC(name,1)
#define DECLARE_MUTEX_LOCKED(name) __DECLARE_SEMAPHORE_GENERIC(name,0)

extern inline void sema_init(struct semaphore *sem, int val)
{
	*sem = (struct semaphore)__SEMAPHORE_INITIALIZER((*sem),val);
}

extern inline void init_MUTEX (struct semaphore *sem)
{
        sema_init(sem, 1);
}

extern inline void init_MUTEX_LOCKED (struct semaphore *sem)
{
        sema_init(sem, 0);
}

extern void __down(struct semaphore * sem);
extern int __down_interruptible(struct semaphore * sem);
extern int __down_trylock(struct semaphore * sem);
extern void __up(struct semaphore * sem);

/* notice - we probably can do cli/sti here instead of saving */

extern inline void down(struct semaphore * sem)
{
	unsigned long flags;
	int failed;

#if WAITQUEUE_DEBUG
	CHECK_MAGIC(sem->__magic);
#endif
	might_sleep();

	/* atomically decrement the semaphores count, and if its negative, we wait */
	local_save_flags(flags);
	local_irq_disable();
	failed = --(sem->count.counter) < 0;
	local_irq_restore(flags);
	if(failed) {
		__down(sem);
	}
}

/*
 * This version waits in interruptible state so that the waiting
 * process can be killed.  The down_interruptible routine
 * returns negative for signalled and zero for semaphore acquired.
 */

extern inline int down_interruptible(struct semaphore * sem)
{
	unsigned long flags;
	int failed;

#if WAITQUEUE_DEBUG
	CHECK_MAGIC(sem->__magic);
#endif
	might_sleep();

	/* atomically decrement the semaphores count, and if its negative, we wait */
	local_save_flags(flags);
	local_irq_disable();
	failed = --(sem->count.counter) < 0;
	local_irq_restore(flags);
	if(failed)
		failed = __down_interruptible(sem);
	return(failed);
}

extern inline int down_trylock(struct semaphore * sem)
{
	unsigned long flags;
	int failed;

#if WAITQUEUE_DEBUG
	CHECK_MAGIC(sem->__magic);
#endif

	local_save_flags(flags);
	local_irq_disable();
	failed = --(sem->count.counter) < 0;
	local_irq_restore(flags);
	if(failed)
		failed = __down_trylock(sem);
	return(failed);
}

/*
 * Note! This is subtle. We jump to wake people up only if
 * the semaphore was negative (== somebody was waiting on it).
 * The default case (no contention) will result in NO
 * jumps for both down() and up().
 */
extern inline void up(struct semaphore * sem)
{  
	unsigned long flags;
	int wakeup;

#if WAITQUEUE_DEBUG
	CHECK_MAGIC(sem->__magic);
#endif

	/* atomically increment the semaphores count, and if it was negative, we wake people */
	local_save_flags(flags);
	local_irq_disable();
	wakeup = ++(sem->count.counter) <= 0;
	local_irq_restore(flags);
	if(wakeup) {
		__up(sem);
	}
}

#endif
