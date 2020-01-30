/*
 * i386 semaphore implementation.
 *
 * (C) Copyright 1999 Linus Torvalds
 *
 * Portions Copyright 1999 Red Hat, Inc.
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 *
 * rw semaphores implemented November 1999 by Benjamin LaHaise <bcrl@redhat.com>
 */
#include <linux/config.h>
#include <linux/sched.h>

#include <asm/semaphore.h>

/*
 * Semaphores are implemented using a two-way counter:
 * The "count" variable is decremented for each process
 * that tries to acquire the semaphore, while the "sleeping"
 * variable is a count of such acquires.
 *
 * Notably, the inline "up()" and "down()" functions can
 * efficiently test if they need to do any extra work (up
 * needs to do something only if count was negative before
 * the increment operation.
 *
 * "sleeping" and the contention routine ordering is
 * protected by the semaphore spinlock.
 *
 * Note that these functions are only called when there is
 * contention on the lock, and as such all this is the
 * "non-critical" part of the whole semaphore business. The
 * critical part is the inline stuff in <asm/semaphore.h>
 * where we want to avoid any extra jumps and calls.
 */

/*
 * Logic:
 *  - only on a boundary condition do we need to care. When we go
 *    from a negative count to a non-negative, we wake people up.
 *  - when we go from a non-negative count to a negative do we
 *    (a) synchronize with the "sleeper" count and (b) make sure
 *    that we're on the wakeup list before we synchronize so that
 *    we cannot lose wakeup events.
 */

void __up(struct semaphore *sem)
{
	wake_up(&sem->wait);
}

static spinlock_t semaphore_lock = SPIN_LOCK_UNLOCKED;

void __down(struct semaphore * sem)
{
	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);
	tsk->state = TASK_UNINTERRUPTIBLE;
	add_wait_queue_exclusive(&sem->wait, &wait);

	spin_lock_irq(&semaphore_lock);
	sem->sleepers++;
	for (;;) {
		int sleepers = sem->sleepers;

		/*
		 * Add "everybody else" into it. They aren't
		 * playing, because we own the spinlock.
		 */
		if (!atomic_add_negative(sleepers - 1, &sem->count)) {
			sem->sleepers = 0;
			break;
		}
		sem->sleepers = 1;	/* us - see -1 above */
		spin_unlock_irq(&semaphore_lock);

		schedule();
		tsk->state = TASK_UNINTERRUPTIBLE;
		spin_lock_irq(&semaphore_lock);
	}
	spin_unlock_irq(&semaphore_lock);
	remove_wait_queue(&sem->wait, &wait);
	tsk->state = TASK_RUNNING;
	wake_up(&sem->wait);
}

int __down_interruptible(struct semaphore * sem)
{
	int retval = 0;
	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);
	tsk->state = TASK_INTERRUPTIBLE;
	add_wait_queue_exclusive(&sem->wait, &wait);

	spin_lock_irq(&semaphore_lock);
	sem->sleepers ++;
	for (;;) {
		int sleepers = sem->sleepers;

		/*
		 * With signals pending, this turns into
		 * the trylock failure case - we won't be
		 * sleeping, and we* can't get the lock as
		 * it has contention. Just correct the count
		 * and exit.
		 */
		if (signal_pending(current)) {
			retval = -EINTR;
			sem->sleepers = 0;
			atomic_add(sleepers, &sem->count);
			break;
		}

		/*
		 * Add "everybody else" into it. They aren't
		 * playing, because we own the spinlock. The
		 * "-1" is because we're still hoping to get
		 * the lock.
		 */
		if (!atomic_add_negative(sleepers - 1, &sem->count)) {
			sem->sleepers = 0;
			break;
		}
		sem->sleepers = 1;	/* us - see -1 above */
		spin_unlock_irq(&semaphore_lock);

		schedule();
		tsk->state = TASK_INTERRUPTIBLE;
		spin_lock_irq(&semaphore_lock);
	}
	spin_unlock_irq(&semaphore_lock);
	tsk->state = TASK_RUNNING;
	remove_wait_queue(&sem->wait, &wait);
	wake_up(&sem->wait);
	return retval;
}

/*
 * Trylock failed - make sure we correct for
 * having decremented the count.
 *
 * We could have done the trylock with a
 * single "cmpxchg" without failure cases,
 * but then it wouldn't work on a 386.
 */
int __down_trylock(struct semaphore * sem)
{
	int sleepers;
	unsigned long flags;

	spin_lock_irqsave(&semaphore_lock, flags);
	sleepers = sem->sleepers + 1;
	sem->sleepers = 0;

	/*
	 * Add "everybody else" and us into it. They aren't
	 * playing, because we own the spinlock.
	 */
	if (!atomic_add_negative(sleepers, &sem->count))
		wake_up(&sem->wait);

	spin_unlock_irqrestore(&semaphore_lock, flags);
	return 1;
}


/*
 * The semaphore operations have a special calling sequence that
 * allow us to do a simpler in-line version of them. These routines
 * need to convert that sequence back into the C sequence when
 * there is contention on the semaphore.
 *
 * %ecx contains the semaphore pointer on entry. Save the C-clobbered
 * registers (%eax, %edx and %ecx) except %eax when used as a return
 * value..
 */
asm(
".align 4\n"
".globl __down_failed\n"
"__down_failed:\n\t"
	"pushl %eax\n\t"
	"pushl %edx\n\t"
	"pushl %ecx\n\t"
	"call __down\n\t"
	"popl %ecx\n\t"
	"popl %edx\n\t"
	"popl %eax\n\t"
	"ret"
);

asm(
".align 4\n"
".globl __down_failed_interruptible\n"
"__down_failed_interruptible:\n\t"
	"pushl %edx\n\t"
	"pushl %ecx\n\t"
	"call __down_interruptible\n\t"
	"popl %ecx\n\t"
	"popl %edx\n\t"
	"ret"
);

asm(
".align 4\n"
".globl __down_failed_trylock\n"
"__down_failed_trylock:\n\t"
	"pushl %edx\n\t"
	"pushl %ecx\n\t"
	"call __down_trylock\n\t"
	"popl %ecx\n\t"
	"popl %edx\n\t"
	"ret"
);

asm(
".align 4\n"
".globl __up_wakeup\n"
"__up_wakeup:\n\t"
	"pushl %eax\n\t"
	"pushl %edx\n\t"
	"pushl %ecx\n\t"
	"call __up\n\t"
	"popl %ecx\n\t"
	"popl %edx\n\t"
	"popl %eax\n\t"
	"ret"
);

asm(
"
.align 4
.globl __down_read_failed
__down_read_failed:
	pushl	%edx
	pushl	%ecx
	jnc	2f

3:	call	down_read_failed_biased

1:	popl	%ecx
	popl	%edx
	ret

2:	call	down_read_failed
	" LOCK "subl	$1,(%eax)
	jns	1b
	jnc	2b
	jmp	3b
"
);

asm(
"
.align 4
.globl __down_write_failed
__down_write_failed:
	pushl	%edx
	pushl	%ecx
	jnc	2f

3:	call	down_write_failed_biased

1:	popl	%ecx
	popl	%edx
	ret

2:	call	down_write_failed
	" LOCK "subl	$" RW_LOCK_BIAS_STR ",(%eax)
	jz	1b
	jnc	2b
	jmp	3b
"
);

struct rw_semaphore *FASTCALL(rwsem_wake_readers(struct rw_semaphore *sem));
struct rw_semaphore *FASTCALL(rwsem_wake_writer(struct rw_semaphore *sem));

struct rw_semaphore *FASTCALL(down_read_failed_biased(struct rw_semaphore *sem));
struct rw_semaphore *FASTCALL(down_write_failed_biased(struct rw_semaphore *sem));
struct rw_semaphore *FASTCALL(down_read_failed(struct rw_semaphore *sem));
struct rw_semaphore *FASTCALL(down_write_failed(struct rw_semaphore *sem));

struct rw_semaphore *down_read_failed_biased(struct rw_semaphore *sem)
{
	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);

	add_wait_queue(&sem->wait, &wait);	/* put ourselves at the head of the list */

	for (;;) {
		if (sem->read_bias_granted && xchg(&sem->read_bias_granted, 0))
			break;
		set_task_state(tsk, TASK_UNINTERRUPTIBLE);
		if (!sem->read_bias_granted)
			schedule();
	}

	remove_wait_queue(&sem->wait, &wait);
	tsk->state = TASK_RUNNING;

	return sem;
}

struct rw_semaphore *down_write_failed_biased(struct rw_semaphore *sem)
{
	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);

	add_wait_queue_exclusive(&sem->write_bias_wait, &wait);	/* put ourselves at the end of the list */

	for (;;) {
		if (sem->write_bias_granted && xchg(&sem->write_bias_granted, 0))
			break;
		set_task_state(tsk, TASK_UNINTERRUPTIBLE);
		if (!sem->write_bias_granted)
			schedule();
	}

	remove_wait_queue(&sem->write_bias_wait, &wait);
	tsk->state = TASK_RUNNING;

	/* if the lock is currently unbiased, awaken the sleepers
	 * FIXME: this wakes up the readers early in a bit of a
	 * stampede -> bad!
	 */
	if (atomic_read(&sem->count) >= 0)
		wake_up(&sem->wait);

	return sem;
}

/* Wait for the lock to become unbiased.  Readers
 * are non-exclusive. =)
 */
struct rw_semaphore *down_read_failed(struct rw_semaphore *sem)
{
	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);

	__up_read(sem);	/* this takes care of granting the lock */

	add_wait_queue(&sem->wait, &wait);

	while (atomic_read(&sem->count) < 0) {
		set_task_state(tsk, TASK_UNINTERRUPTIBLE);
		if (atomic_read(&sem->count) >= 0)
			break;
		schedule();
	}

	remove_wait_queue(&sem->wait, &wait);
	tsk->state = TASK_RUNNING;

	return sem;
}

/* Wait for the lock to become unbiased. Since we're
 * a writer, we'll make ourselves exclusive.
 */
struct rw_semaphore *down_write_failed(struct rw_semaphore *sem)
{
	struct task_struct *tsk = current;
	DECLARE_WAITQUEUE(wait, tsk);

	__up_write(sem);	/* this takes care of granting the lock */

	add_wait_queue_exclusive(&sem->wait, &wait);

	while (atomic_read(&sem->count) < 0) {
		set_task_state(tsk, TASK_UNINTERRUPTIBLE);
		if (atomic_read(&sem->count) >= 0)
			break;	/* we must attempt to acquire or bias the lock */
		schedule();
	}

	remove_wait_queue(&sem->wait, &wait);
	tsk->state = TASK_RUNNING;

	return sem;
}

asm(
"
.align 4
.globl __rwsem_wake
__rwsem_wake:
	pushl	%edx
	pushl	%ecx

	jz	1f
	call	rwsem_wake_readers
	jmp	2f

1:	call	rwsem_wake_writer

2:	popl	%ecx
	popl	%edx
	ret
"
);

/* Called when someone has done an up that transitioned from
 * negative to non-negative, meaning that the lock has been
 * granted to whomever owned the bias.
 */
struct rw_semaphore *rwsem_wake_readers(struct rw_semaphore *sem)
{
	if (xchg(&sem->read_bias_granted, 1))
		BUG();
	wake_up(&sem->wait);
	return sem;
}

struct rw_semaphore *rwsem_wake_writer(struct rw_semaphore *sem)
{
	if (xchg(&sem->write_bias_granted, 1))
		BUG();
	wake_up(&sem->write_bias_wait);
	return sem;
}

#if defined(CONFIG_SMP)
asm(
"
.align	4
.globl	__write_lock_failed
__write_lock_failed:
	" LOCK "addl	$" RW_LOCK_BIAS_STR ",(%eax)
1:	cmpl	$" RW_LOCK_BIAS_STR ",(%eax)
	jne	1b

	" LOCK "subl	$" RW_LOCK_BIAS_STR ",(%eax)
	jnz	__write_lock_failed
	ret


.align	4
.globl	__read_lock_failed
__read_lock_failed:
	lock ; incl	(%eax)
1:	cmpl	$1,(%eax)
	js	1b

	lock ; decl	(%eax)
	js	__read_lock_failed
	ret
"
);
#endif

