/*
 * asm-ia64/rwsem.h: R/W semaphores for ia64
 *
 * Copyright (C) 2003 Ken Chen <kenneth.w.chen@intel.com>
 * Copyright (C) 2003 Asit Mallick <asit.k.mallick@intel.com>
 *
 * Based on asm-i386/rwsem.h and other architecture implementation.
 *
 * The MSW of the count is the negated number of active writers and
 * waiting lockers, and the LSW is the total number of active locks.
 *
 * The lock count is initialized to 0 (no active and no waiting lockers).
 *
 * When a writer subtracts WRITE_BIAS, it'll get 0xffff0001 for the case
 * of an uncontended lock. Readers increment by 1 and see a positive value
 * when uncontended, negative if there are writers (and maybe) readers
 * waiting (in which case it goes to sleep).
 */

#ifndef _ASM_IA64_RWSEM_H
#define _ASM_IA64_RWSEM_H

#include <linux/list.h>
#include <linux/spinlock.h>

#include <asm/intrinsics.h>

/*
 * the semaphore definition
 */
struct rw_semaphore {
	signed int		count;
	spinlock_t		wait_lock;
	struct list_head	wait_list;
#if RWSEM_DEBUG
	int			debug;
#endif
};

#define RWSEM_UNLOCKED_VALUE		0x00000000
#define RWSEM_ACTIVE_BIAS		0x00000001
#define RWSEM_ACTIVE_MASK		0x0000ffff
#define RWSEM_WAITING_BIAS		(-0x00010000)
#define RWSEM_ACTIVE_READ_BIAS		RWSEM_ACTIVE_BIAS
#define RWSEM_ACTIVE_WRITE_BIAS		(RWSEM_WAITING_BIAS + RWSEM_ACTIVE_BIAS)

/*
 * initialization
 */
#if RWSEM_DEBUG
#define __RWSEM_DEBUG_INIT      , 0
#else
#define __RWSEM_DEBUG_INIT	/* */
#endif

#define __RWSEM_INITIALIZER(name) \
	{ RWSEM_UNLOCKED_VALUE, SPIN_LOCK_UNLOCKED, \
	  LIST_HEAD_INIT((name).wait_list) \
	  __RWSEM_DEBUG_INIT }

#define DECLARE_RWSEM(name) \
	struct rw_semaphore name = __RWSEM_INITIALIZER(name)

extern struct rw_semaphore *rwsem_down_read_failed(struct rw_semaphore *sem);
extern struct rw_semaphore *rwsem_down_write_failed(struct rw_semaphore *sem);
extern struct rw_semaphore *rwsem_wake(struct rw_semaphore *sem);
extern struct rw_semaphore *rwsem_downgrade_wake(struct rw_semaphore *sem);

static inline void
init_rwsem (struct rw_semaphore *sem)
{
	sem->count = RWSEM_UNLOCKED_VALUE;
	spin_lock_init(&sem->wait_lock);
	INIT_LIST_HEAD(&sem->wait_list);
#if RWSEM_DEBUG
	sem->debug = 0;
#endif
}

/*
 * lock for reading
 */
static inline void
__down_read (struct rw_semaphore *sem)
{
	int result = ia64_fetchadd4_acq((unsigned int *)&sem->count, 1);

	if (result < 0)
		rwsem_down_read_failed(sem);
}

/*
 * lock for writing
 */
static inline void
__down_write (struct rw_semaphore *sem)
{
	int old, new;

	do {
		old = sem->count;
		new = old + RWSEM_ACTIVE_WRITE_BIAS;
	} while (cmpxchg_acq(&sem->count, old, new) != old);

	if (old != 0)
		rwsem_down_write_failed(sem);
}

/*
 * unlock after reading
 */
static inline void
__up_read (struct rw_semaphore *sem)
{
	int result = ia64_fetchadd4_rel((unsigned int *)&sem->count, -1);

	if (result < 0 && (--result & RWSEM_ACTIVE_MASK) == 0)
		rwsem_wake(sem);
}

/*
 * unlock after writing
 */
static inline void
__up_write (struct rw_semaphore *sem)
{
	int old, new;

	do {
		old = sem->count;
		new = old - RWSEM_ACTIVE_WRITE_BIAS;
	} while (cmpxchg_rel(&sem->count, old, new) != old);

	if (new < 0 && (new & RWSEM_ACTIVE_MASK) == 0)
		rwsem_wake(sem);
}

/*
 * trylock for reading -- returns 1 if successful, 0 if contention
 */
static inline int
__down_read_trylock (struct rw_semaphore *sem)
{
	int tmp;
	while ((tmp = sem->count) >= 0) {
		if (tmp == cmpxchg_acq(&sem->count, tmp, tmp+1)) {
			return 1;
		}
	}
	return 0;
}

/*
 * trylock for writing -- returns 1 if successful, 0 if contention
 */
static inline int
__down_write_trylock (struct rw_semaphore *sem)
{
	int tmp = cmpxchg_acq(&sem->count, RWSEM_UNLOCKED_VALUE,
			      RWSEM_ACTIVE_WRITE_BIAS);
	return tmp == RWSEM_UNLOCKED_VALUE;
}

/*
 * downgrade write lock to read lock
 */
static inline void
__downgrade_write (struct rw_semaphore *sem)
{
	int old, new;

	do {
		old = sem->count;
		new = old - RWSEM_WAITING_BIAS;
	} while (cmpxchg_rel(&sem->count, old, new) != old);

	if (old < 0)
		rwsem_downgrade_wake(sem);
}

/*
 * Implement atomic add functionality.  These used to be "inline" functions, but GCC v3.1
 * doesn't quite optimize this stuff right and ends up with bad calls to fetchandadd.
 */
#define rwsem_atomic_add(delta, sem)	atomic_add(delta, (atomic_t *)(&(sem)->count))
#define rwsem_atomic_update(delta, sem)	atomic_add_return(delta, (atomic_t *)(&(sem)->count))

#endif /* _ASM_IA64_RWSEM_H */
