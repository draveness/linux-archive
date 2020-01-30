#ifndef __ASM_SPINLOCK_H
#define __ASM_SPINLOCK_H

/*
 * Simple spin lock operations.  
 *
 * Copyright (C) 2001-2004 Paul Mackerras <paulus@au.ibm.com>, IBM
 * Copyright (C) 2001 Anton Blanchard <anton@au.ibm.com>, IBM
 *
 * Type of int is used as a full 64b word is not necessary.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#include <linux/config.h>
#include <asm/paca.h>

typedef struct {
	volatile unsigned int lock;
} spinlock_t;

#ifdef __KERNEL__
#define SPIN_LOCK_UNLOCKED	(spinlock_t) { 0 }

#define spin_is_locked(x)	((x)->lock != 0)
#define spin_lock_init(x)	do { *(x) = SPIN_LOCK_UNLOCKED; } while(0)

static __inline__ void _raw_spin_unlock(spinlock_t *lock)
{
	__asm__ __volatile__("lwsync	# spin_unlock": : :"memory");
	lock->lock = 0;
}

/*
 * Normally we use the spinlock functions in arch/ppc64/lib/locks.c.
 * For special applications such as profiling, we can have the
 * spinlock functions inline by defining CONFIG_SPINLINE.
 * This is not recommended on partitioned systems with shared
 * processors, since the inline spinlock functions don't include
 * the code for yielding the CPU to the lock holder.
 */

#ifndef CONFIG_SPINLINE
extern int _raw_spin_trylock(spinlock_t *lock);
extern void _raw_spin_lock(spinlock_t *lock);
extern void _raw_spin_lock_flags(spinlock_t *lock, unsigned long flags);
extern void spin_unlock_wait(spinlock_t *lock);

#else

static __inline__ int _raw_spin_trylock(spinlock_t *lock)
{
	unsigned int tmp, tmp2;

	__asm__ __volatile__(
"1:	lwarx		%0,0,%2		# spin_trylock\n\
	cmpwi		0,%0,0\n\
	bne-		2f\n\
	lwz		%1,%3(13)\n\
	stwcx.		%1,0,%2\n\
	bne-		1b\n\
	isync\n\
2:"	: "=&r"(tmp), "=&r"(tmp2)
	: "r"(&lock->lock), "i"(offsetof(struct paca_struct, lock_token))
	: "cr0", "memory");

	return tmp == 0;
}

static __inline__ void _raw_spin_lock(spinlock_t *lock)
{
	unsigned int tmp;

	__asm__ __volatile__(
	"b		2f		# spin_lock\n\
1:"
	HMT_LOW
"	lwzx		%0,0,%1\n\
	cmpwi		0,%0,0\n\
	bne+		1b\n"
	HMT_MEDIUM
"2:	lwarx		%0,0,%1\n\
	cmpwi		0,%0,0\n\
	bne-		1b\n\
	lwz		%0,%2(13)\n\
	stwcx.		%0,0,%1\n\
	bne-		2b\n\
	isync"
	: "=&r"(tmp)
	: "r"(&lock->lock), "i"(offsetof(struct paca_struct, lock_token))
	: "cr0", "memory");
}

/*
 * Note: if we ever want to inline the spinlocks on iSeries,
 * we will have to change the irq enable/disable stuff in here.
 */
static __inline__ void _raw_spin_lock_flags(spinlock_t *lock,
					    unsigned long flags)
{
	unsigned int tmp;
	unsigned long tmp2;

	__asm__ __volatile__(
	"b		3f		# spin_lock\n\
1:	mfmsr		%1\n\
	mtmsrd		%3,1\n\
2:"	HMT_LOW
"	lwzx		%0,0,%2\n\
	cmpwi		0,%0,0\n\
	bne+		2b\n"
	HMT_MEDIUM
"	mtmsrd		%1,1\n\
3:	lwarx		%0,0,%2\n\
	cmpwi		0,%0,0\n\
	bne-		1b\n\
	lwz		%1,%4(13)\n\
	stwcx.		%1,0,%2\n\
	bne-		3b\n\
	isync"
	: "=&r"(tmp), "=&r"(tmp2)
	: "r"(&lock->lock), "r"(flags),
	  "i" (offsetof(struct paca_struct, lock_token))
	: "cr0", "memory");
}

#define spin_unlock_wait(x)	do { cpu_relax(); } while (spin_is_locked(x))

#endif /* CONFIG_SPINLINE */

/*
 * Read-write spinlocks, allowing multiple readers
 * but only one writer.
 *
 * NOTE! it is quite common to have readers in interrupts
 * but no interrupt writers. For those circumstances we
 * can "mix" irq-safe locks - any writer needs to get a
 * irq-safe write-lock, but readers can get non-irqsafe
 * read-locks.
 */
typedef struct {
	volatile signed int lock;
} rwlock_t;

#define RW_LOCK_UNLOCKED (rwlock_t) { 0 }

#define rwlock_init(x)		do { *(x) = RW_LOCK_UNLOCKED; } while(0)
#define rwlock_is_locked(x)	((x)->lock)

static __inline__ int is_read_locked(rwlock_t *rw)
{
	return rw->lock > 0;
}

static __inline__ int is_write_locked(rwlock_t *rw)
{
	return rw->lock < 0;
}

static __inline__ void _raw_write_unlock(rwlock_t *rw)
{
	__asm__ __volatile__("lwsync		# write_unlock": : :"memory");
	rw->lock = 0;
}

#ifndef CONFIG_SPINLINE
extern int _raw_read_trylock(rwlock_t *rw);
extern void _raw_read_lock(rwlock_t *rw);
extern void _raw_read_unlock(rwlock_t *rw);
extern int _raw_write_trylock(rwlock_t *rw);
extern void _raw_write_lock(rwlock_t *rw);
extern void _raw_write_unlock(rwlock_t *rw);

#else
static __inline__ int _raw_read_trylock(rwlock_t *rw)
{
	unsigned int tmp;
	unsigned int ret;

	__asm__ __volatile__(
"1:	lwarx		%0,0,%2		# read_trylock\n\
	li		%1,0\n\
	extsw		%0,%0\n\
	addic.		%0,%0,1\n\
	ble-		2f\n\
	stwcx.		%0,0,%2\n\
	bne-		1b\n\
	li		%1,1\n\
	isync\n\
2:"	: "=&r"(tmp), "=&r"(ret)
	: "r"(&rw->lock)
	: "cr0", "memory");

	return ret;
}

static __inline__ void _raw_read_lock(rwlock_t *rw)
{
	unsigned int tmp;

	__asm__ __volatile__(
	"b		2f		# read_lock\n\
1:"
	HMT_LOW
"	lwax		%0,0,%1\n\
	cmpwi		0,%0,0\n\
	blt+		1b\n"
	HMT_MEDIUM
"2:	lwarx		%0,0,%1\n\
	extsw		%0,%0\n\
	addic.		%0,%0,1\n\
	ble-		1b\n\
	stwcx.		%0,0,%1\n\
	bne-		2b\n\
	isync"
	: "=&r"(tmp)
	: "r"(&rw->lock)
	: "cr0", "memory");
}

static __inline__ void _raw_read_unlock(rwlock_t *rw)
{
	unsigned int tmp;

	__asm__ __volatile__(
	"lwsync				# read_unlock\n\
1:	lwarx		%0,0,%1\n\
	addic		%0,%0,-1\n\
	stwcx.		%0,0,%1\n\
	bne-		1b"
	: "=&r"(tmp)
	: "r"(&rw->lock)
	: "cr0", "memory");
}

static __inline__ int _raw_write_trylock(rwlock_t *rw)
{
	unsigned int tmp;
	unsigned int ret;

	__asm__ __volatile__(
"1:	lwarx		%0,0,%2		# write_trylock\n\
	cmpwi		0,%0,0\n\
	li		%1,0\n\
	bne-		2f\n\
	stwcx.		%3,0,%2\n\
	bne-		1b\n\
	li		%1,1\n\
	isync\n\
2:"	: "=&r"(tmp), "=&r"(ret)
	: "r"(&rw->lock), "r"(-1)
	: "cr0", "memory");

	return ret;
}

static __inline__ void _raw_write_lock(rwlock_t *rw)
{
	unsigned int tmp;

	__asm__ __volatile__(
	"b		2f		# write_lock\n\
1:"
	HMT_LOW
	"lwax		%0,0,%1\n\
	cmpwi		0,%0,0\n\
	bne+		1b\n"
	HMT_MEDIUM
"2:	lwarx		%0,0,%1\n\
	cmpwi		0,%0,0\n\
	bne-		1b\n\
	stwcx.		%2,0,%1\n\
	bne-		2b\n\
	isync"
	: "=&r"(tmp)
	: "r"(&rw->lock), "r"(-1)
	: "cr0", "memory");
}
#endif /* CONFIG_SPINLINE */

#endif /* __KERNEL__ */
#endif /* __ASM_SPINLOCK_H */
