#ifndef __LINUX_SPINLOCK_H
#define __LINUX_SPINLOCK_H

#include <linux/config.h>

/*
 * These are the generic versions of the spinlocks and read-write
 * locks..
 */
#define spin_lock_irqsave(lock, flags)		do { local_irq_save(flags);       spin_lock(lock); } while (0)
#define spin_lock_irq(lock)			do { local_irq_disable();         spin_lock(lock); } while (0)
#define spin_lock_bh(lock)			do { local_bh_disable();          spin_lock(lock); } while (0)

#define read_lock_irqsave(lock, flags)		do { local_irq_save(flags);       read_lock(lock); } while (0)
#define read_lock_irq(lock)			do { local_irq_disable();         read_lock(lock); } while (0)
#define read_lock_bh(lock)			do { local_bh_disable();          read_lock(lock); } while (0)

#define write_lock_irqsave(lock, flags)		do { local_irq_save(flags);      write_lock(lock); } while (0)
#define write_lock_irq(lock)			do { local_irq_disable();        write_lock(lock); } while (0)
#define write_lock_bh(lock)			do { local_bh_disable();         write_lock(lock); } while (0)

#define spin_unlock_irqrestore(lock, flags)	do { spin_unlock(lock);  local_irq_restore(flags); } while (0)
#define spin_unlock_irq(lock)			do { spin_unlock(lock);  local_irq_enable();       } while (0)
#define spin_unlock_bh(lock)			do { spin_unlock(lock);  local_bh_enable();        } while (0)

#define read_unlock_irqrestore(lock, flags)	do { read_unlock(lock);  local_irq_restore(flags); } while (0)
#define read_unlock_irq(lock)			do { read_unlock(lock);  local_irq_enable();       } while (0)
#define read_unlock_bh(lock)			do { read_unlock(lock);  local_bh_enable();        } while (0)

#define write_unlock_irqrestore(lock, flags)	do { write_unlock(lock); local_irq_restore(flags); } while (0)
#define write_unlock_irq(lock)			do { write_unlock(lock); local_irq_enable();       } while (0)
#define write_unlock_bh(lock)			do { write_unlock(lock); local_bh_enable();        } while (0)

#ifdef CONFIG_SMP
#include <asm/spinlock.h>

#else /* !SMP */

#define DEBUG_SPINLOCKS	0	/* 0 == no debugging, 1 == maintain lock state, 2 == full debug */

#if (DEBUG_SPINLOCKS < 1)

#define atomic_dec_and_lock(atomic,lock) atomic_dec_and_test(atomic)

/*
 * Your basic spinlocks, allowing only a single CPU anywhere
 *
 * Most gcc versions have a nasty bug with empty initializers.
 */
#if (__GNUC__ > 2)
  typedef struct { } spinlock_t;
  #define SPIN_LOCK_UNLOCKED (spinlock_t) { }
#else
  typedef struct { int gcc_is_buggy; } spinlock_t;
  #define SPIN_LOCK_UNLOCKED (spinlock_t) { 0 }
#endif

#define spin_lock_init(lock)	do { } while(0)
#define spin_lock(lock)		(void)(lock) /* Not "unused variable". */
#define spin_is_locked(lock)	(0)
#define spin_trylock(lock)	({1; })
#define spin_unlock_wait(lock)	do { } while(0)
#define spin_unlock(lock)	do { } while(0)

#elif (DEBUG_SPINLOCKS < 2)

typedef struct {
	volatile unsigned long lock;
} spinlock_t;
#define SPIN_LOCK_UNLOCKED (spinlock_t) { 0 }

#define spin_lock_init(x)	do { (x)->lock = 0; } while (0)
#define spin_is_locked(lock)	(test_bit(0,(lock)))
#define spin_trylock(lock)	(!test_and_set_bit(0,(lock)))

#define spin_lock(x)		do { (x)->lock = 1; } while (0)
#define spin_unlock_wait(x)	do { } while (0)
#define spin_unlock(x)		do { (x)->lock = 0; } while (0)

#else /* (DEBUG_SPINLOCKS >= 2) */

typedef struct {
	volatile unsigned long lock;
	volatile unsigned int babble;
	const char *module;
} spinlock_t;
#define SPIN_LOCK_UNLOCKED (spinlock_t) { 0, 25, __BASE_FILE__ }

#include <linux/kernel.h>

#define spin_lock_init(x)	do { (x)->lock = 0; } while (0)
#define spin_is_locked(lock)	(test_bit(0,(lock)))
#define spin_trylock(lock)	(!test_and_set_bit(0,(lock)))

#define spin_lock(x)		do {unsigned long __spinflags; save_flags(__spinflags); cli(); if ((x)->lock&&(x)->babble) {printk("%s:%d: spin_lock(%s:%p) already locked\n", __BASE_FILE__,__LINE__, (x)->module, (x));(x)->babble--;} (x)->lock = 1; restore_flags(__spinflags);} while (0)
#define spin_unlock_wait(x)	do {unsigned long __spinflags; save_flags(__spinflags); cli(); if ((x)->lock&&(x)->babble) {printk("%s:%d: spin_unlock_wait(%s:%p) deadlock\n", __BASE_FILE__,__LINE__, (x)->module, (x));(x)->babble--;} restore_flags(__spinflags);} while (0)
#define spin_unlock(x)		do {unsigned long __spinflags; save_flags(__spinflags); cli(); if (!(x)->lock&&(x)->babble) {printk("%s:%d: spin_unlock(%s:%p) not locked\n", __BASE_FILE__,__LINE__, (x)->module, (x));(x)->babble--;} (x)->lock = 0; restore_flags(__spinflags);} while (0)

#endif	/* DEBUG_SPINLOCKS */

/*
 * Read-write spinlocks, allowing multiple readers
 * but only one writer.
 *
 * NOTE! it is quite common to have readers in interrupts
 * but no interrupt writers. For those circumstances we
 * can "mix" irq-safe locks - any writer needs to get a
 * irq-safe write-lock, but readers can get non-irqsafe
 * read-locks.
 *
 * Most gcc versions have a nasty bug with empty initializers.
 */
#if (__GNUC__ > 2)
  typedef struct { } rwlock_t;
  #define RW_LOCK_UNLOCKED (rwlock_t) { }
#else
  typedef struct { int gcc_is_buggy; } rwlock_t;
  #define RW_LOCK_UNLOCKED (rwlock_t) { 0 }
#endif

#define rwlock_init(lock)	do { } while(0)
#define read_lock(lock)		(void)(lock) /* Not "unused variable". */
#define read_unlock(lock)	do { } while(0)
#define write_lock(lock)	(void)(lock) /* Not "unused variable". */
#define write_unlock(lock)	do { } while(0)

#endif /* !SMP */

/* "lock on reference count zero" */
#ifndef atomic_dec_and_lock
#include <asm/atomic.h>
extern int atomic_dec_and_lock(atomic_t *atomic, spinlock_t *lock);
#endif

#endif /* __LINUX_SPINLOCK_H */
