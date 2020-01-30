/*
 * kernel/lockdep.c
 *
 * Runtime locking correctness validator
 *
 * Started by Ingo Molnar:
 *
 *  Copyright (C) 2006,2007 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *  Copyright (C) 2007 Red Hat, Inc., Peter Zijlstra <pzijlstr@redhat.com>
 *
 * this code maps all the lock dependencies as they occur in a live kernel
 * and will warn about the following classes of locking bugs:
 *
 * - lock inversion scenarios
 * - circular lock dependencies
 * - hardirq/softirq safe/unsafe locking bugs
 *
 * Bugs are reported even if the current locking scenario does not cause
 * any deadlock at this point.
 *
 * I.e. if anytime in the past two locks were taken in a different order,
 * even if it happened for another task, even if those were different
 * locks (but of the same class as this lock), this code will detect it.
 *
 * Thanks to Arjan van de Ven for coming up with the initial idea of
 * mapping lock dependencies runtime.
 */
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/kallsyms.h>
#include <linux/interrupt.h>
#include <linux/stacktrace.h>
#include <linux/debug_locks.h>
#include <linux/irqflags.h>
#include <linux/utsname.h>
#include <linux/hash.h>

#include <asm/sections.h>

#include "lockdep_internals.h"

#ifdef CONFIG_PROVE_LOCKING
int prove_locking = 1;
module_param(prove_locking, int, 0644);
#else
#define prove_locking 0
#endif

#ifdef CONFIG_LOCK_STAT
int lock_stat = 1;
module_param(lock_stat, int, 0644);
#else
#define lock_stat 0
#endif

/*
 * lockdep_lock: protects the lockdep graph, the hashes and the
 *               class/list/hash allocators.
 *
 * This is one of the rare exceptions where it's justified
 * to use a raw spinlock - we really dont want the spinlock
 * code to recurse back into the lockdep code...
 */
static raw_spinlock_t lockdep_lock = (raw_spinlock_t)__RAW_SPIN_LOCK_UNLOCKED;

static int graph_lock(void)
{
	__raw_spin_lock(&lockdep_lock);
	/*
	 * Make sure that if another CPU detected a bug while
	 * walking the graph we dont change it (while the other
	 * CPU is busy printing out stuff with the graph lock
	 * dropped already)
	 */
	if (!debug_locks) {
		__raw_spin_unlock(&lockdep_lock);
		return 0;
	}
	return 1;
}

static inline int graph_unlock(void)
{
	if (debug_locks && !__raw_spin_is_locked(&lockdep_lock))
		return DEBUG_LOCKS_WARN_ON(1);

	__raw_spin_unlock(&lockdep_lock);
	return 0;
}

/*
 * Turn lock debugging off and return with 0 if it was off already,
 * and also release the graph lock:
 */
static inline int debug_locks_off_graph_unlock(void)
{
	int ret = debug_locks_off();

	__raw_spin_unlock(&lockdep_lock);

	return ret;
}

static int lockdep_initialized;

unsigned long nr_list_entries;
static struct lock_list list_entries[MAX_LOCKDEP_ENTRIES];

/*
 * All data structures here are protected by the global debug_lock.
 *
 * Mutex key structs only get allocated, once during bootup, and never
 * get freed - this significantly simplifies the debugging code.
 */
unsigned long nr_lock_classes;
static struct lock_class lock_classes[MAX_LOCKDEP_KEYS];

#ifdef CONFIG_LOCK_STAT
static DEFINE_PER_CPU(struct lock_class_stats[MAX_LOCKDEP_KEYS], lock_stats);

static int lock_contention_point(struct lock_class *class, unsigned long ip)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(class->contention_point); i++) {
		if (class->contention_point[i] == 0) {
			class->contention_point[i] = ip;
			break;
		}
		if (class->contention_point[i] == ip)
			break;
	}

	return i;
}

static void lock_time_inc(struct lock_time *lt, s64 time)
{
	if (time > lt->max)
		lt->max = time;

	if (time < lt->min || !lt->min)
		lt->min = time;

	lt->total += time;
	lt->nr++;
}

static inline void lock_time_add(struct lock_time *src, struct lock_time *dst)
{
	dst->min += src->min;
	dst->max += src->max;
	dst->total += src->total;
	dst->nr += src->nr;
}

struct lock_class_stats lock_stats(struct lock_class *class)
{
	struct lock_class_stats stats;
	int cpu, i;

	memset(&stats, 0, sizeof(struct lock_class_stats));
	for_each_possible_cpu(cpu) {
		struct lock_class_stats *pcs =
			&per_cpu(lock_stats, cpu)[class - lock_classes];

		for (i = 0; i < ARRAY_SIZE(stats.contention_point); i++)
			stats.contention_point[i] += pcs->contention_point[i];

		lock_time_add(&pcs->read_waittime, &stats.read_waittime);
		lock_time_add(&pcs->write_waittime, &stats.write_waittime);

		lock_time_add(&pcs->read_holdtime, &stats.read_holdtime);
		lock_time_add(&pcs->write_holdtime, &stats.write_holdtime);

		for (i = 0; i < ARRAY_SIZE(stats.bounces); i++)
			stats.bounces[i] += pcs->bounces[i];
	}

	return stats;
}

void clear_lock_stats(struct lock_class *class)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct lock_class_stats *cpu_stats =
			&per_cpu(lock_stats, cpu)[class - lock_classes];

		memset(cpu_stats, 0, sizeof(struct lock_class_stats));
	}
	memset(class->contention_point, 0, sizeof(class->contention_point));
}

static struct lock_class_stats *get_lock_stats(struct lock_class *class)
{
	return &get_cpu_var(lock_stats)[class - lock_classes];
}

static void put_lock_stats(struct lock_class_stats *stats)
{
	put_cpu_var(lock_stats);
}

static void lock_release_holdtime(struct held_lock *hlock)
{
	struct lock_class_stats *stats;
	s64 holdtime;

	if (!lock_stat)
		return;

	holdtime = sched_clock() - hlock->holdtime_stamp;

	stats = get_lock_stats(hlock->class);
	if (hlock->read)
		lock_time_inc(&stats->read_holdtime, holdtime);
	else
		lock_time_inc(&stats->write_holdtime, holdtime);
	put_lock_stats(stats);
}
#else
static inline void lock_release_holdtime(struct held_lock *hlock)
{
}
#endif

/*
 * We keep a global list of all lock classes. The list only grows,
 * never shrinks. The list is only accessed with the lockdep
 * spinlock lock held.
 */
LIST_HEAD(all_lock_classes);

/*
 * The lockdep classes are in a hash-table as well, for fast lookup:
 */
#define CLASSHASH_BITS		(MAX_LOCKDEP_KEYS_BITS - 1)
#define CLASSHASH_SIZE		(1UL << CLASSHASH_BITS)
#define __classhashfn(key)	hash_long((unsigned long)key, CLASSHASH_BITS)
#define classhashentry(key)	(classhash_table + __classhashfn((key)))

static struct list_head classhash_table[CLASSHASH_SIZE];

/*
 * We put the lock dependency chains into a hash-table as well, to cache
 * their existence:
 */
#define CHAINHASH_BITS		(MAX_LOCKDEP_CHAINS_BITS-1)
#define CHAINHASH_SIZE		(1UL << CHAINHASH_BITS)
#define __chainhashfn(chain)	hash_long(chain, CHAINHASH_BITS)
#define chainhashentry(chain)	(chainhash_table + __chainhashfn((chain)))

static struct list_head chainhash_table[CHAINHASH_SIZE];

/*
 * The hash key of the lock dependency chains is a hash itself too:
 * it's a hash of all locks taken up to that lock, including that lock.
 * It's a 64-bit hash, because it's important for the keys to be
 * unique.
 */
#define iterate_chain_key(key1, key2) \
	(((key1) << MAX_LOCKDEP_KEYS_BITS) ^ \
	((key1) >> (64-MAX_LOCKDEP_KEYS_BITS)) ^ \
	(key2))

void lockdep_off(void)
{
	current->lockdep_recursion++;
}

EXPORT_SYMBOL(lockdep_off);

void lockdep_on(void)
{
	current->lockdep_recursion--;
}

EXPORT_SYMBOL(lockdep_on);

/*
 * Debugging switches:
 */

#define VERBOSE			0
#define VERY_VERBOSE		0

#if VERBOSE
# define HARDIRQ_VERBOSE	1
# define SOFTIRQ_VERBOSE	1
#else
# define HARDIRQ_VERBOSE	0
# define SOFTIRQ_VERBOSE	0
#endif

#if VERBOSE || HARDIRQ_VERBOSE || SOFTIRQ_VERBOSE
/*
 * Quick filtering for interesting events:
 */
static int class_filter(struct lock_class *class)
{
#if 0
	/* Example */
	if (class->name_version == 1 &&
			!strcmp(class->name, "lockname"))
		return 1;
	if (class->name_version == 1 &&
			!strcmp(class->name, "&struct->lockfield"))
		return 1;
#endif
	/* Filter everything else. 1 would be to allow everything else */
	return 0;
}
#endif

static int verbose(struct lock_class *class)
{
#if VERBOSE
	return class_filter(class);
#endif
	return 0;
}

/*
 * Stack-trace: tightly packed array of stack backtrace
 * addresses. Protected by the graph_lock.
 */
unsigned long nr_stack_trace_entries;
static unsigned long stack_trace[MAX_STACK_TRACE_ENTRIES];

static int save_trace(struct stack_trace *trace)
{
	trace->nr_entries = 0;
	trace->max_entries = MAX_STACK_TRACE_ENTRIES - nr_stack_trace_entries;
	trace->entries = stack_trace + nr_stack_trace_entries;

	trace->skip = 3;

	save_stack_trace(trace);

	trace->max_entries = trace->nr_entries;

	nr_stack_trace_entries += trace->nr_entries;

	if (nr_stack_trace_entries == MAX_STACK_TRACE_ENTRIES) {
		if (!debug_locks_off_graph_unlock())
			return 0;

		printk("BUG: MAX_STACK_TRACE_ENTRIES too low!\n");
		printk("turning off the locking correctness validator.\n");
		dump_stack();

		return 0;
	}

	return 1;
}

unsigned int nr_hardirq_chains;
unsigned int nr_softirq_chains;
unsigned int nr_process_chains;
unsigned int max_lockdep_depth;
unsigned int max_recursion_depth;

#ifdef CONFIG_DEBUG_LOCKDEP
/*
 * We cannot printk in early bootup code. Not even early_printk()
 * might work. So we mark any initialization errors and printk
 * about it later on, in lockdep_info().
 */
static int lockdep_init_error;
static unsigned long lockdep_init_trace_data[20];
static struct stack_trace lockdep_init_trace = {
	.max_entries = ARRAY_SIZE(lockdep_init_trace_data),
	.entries = lockdep_init_trace_data,
};

/*
 * Various lockdep statistics:
 */
atomic_t chain_lookup_hits;
atomic_t chain_lookup_misses;
atomic_t hardirqs_on_events;
atomic_t hardirqs_off_events;
atomic_t redundant_hardirqs_on;
atomic_t redundant_hardirqs_off;
atomic_t softirqs_on_events;
atomic_t softirqs_off_events;
atomic_t redundant_softirqs_on;
atomic_t redundant_softirqs_off;
atomic_t nr_unused_locks;
atomic_t nr_cyclic_checks;
atomic_t nr_cyclic_check_recursions;
atomic_t nr_find_usage_forwards_checks;
atomic_t nr_find_usage_forwards_recursions;
atomic_t nr_find_usage_backwards_checks;
atomic_t nr_find_usage_backwards_recursions;
# define debug_atomic_inc(ptr)		atomic_inc(ptr)
# define debug_atomic_dec(ptr)		atomic_dec(ptr)
# define debug_atomic_read(ptr)		atomic_read(ptr)
#else
# define debug_atomic_inc(ptr)		do { } while (0)
# define debug_atomic_dec(ptr)		do { } while (0)
# define debug_atomic_read(ptr)		0
#endif

/*
 * Locking printouts:
 */

static const char *usage_str[] =
{
	[LOCK_USED] =			"initial-use ",
	[LOCK_USED_IN_HARDIRQ] =	"in-hardirq-W",
	[LOCK_USED_IN_SOFTIRQ] =	"in-softirq-W",
	[LOCK_ENABLED_SOFTIRQS] =	"softirq-on-W",
	[LOCK_ENABLED_HARDIRQS] =	"hardirq-on-W",
	[LOCK_USED_IN_HARDIRQ_READ] =	"in-hardirq-R",
	[LOCK_USED_IN_SOFTIRQ_READ] =	"in-softirq-R",
	[LOCK_ENABLED_SOFTIRQS_READ] =	"softirq-on-R",
	[LOCK_ENABLED_HARDIRQS_READ] =	"hardirq-on-R",
};

const char * __get_key_name(struct lockdep_subclass_key *key, char *str)
{
	return kallsyms_lookup((unsigned long)key, NULL, NULL, NULL, str);
}

void
get_usage_chars(struct lock_class *class, char *c1, char *c2, char *c3, char *c4)
{
	*c1 = '.', *c2 = '.', *c3 = '.', *c4 = '.';

	if (class->usage_mask & LOCKF_USED_IN_HARDIRQ)
		*c1 = '+';
	else
		if (class->usage_mask & LOCKF_ENABLED_HARDIRQS)
			*c1 = '-';

	if (class->usage_mask & LOCKF_USED_IN_SOFTIRQ)
		*c2 = '+';
	else
		if (class->usage_mask & LOCKF_ENABLED_SOFTIRQS)
			*c2 = '-';

	if (class->usage_mask & LOCKF_ENABLED_HARDIRQS_READ)
		*c3 = '-';
	if (class->usage_mask & LOCKF_USED_IN_HARDIRQ_READ) {
		*c3 = '+';
		if (class->usage_mask & LOCKF_ENABLED_HARDIRQS_READ)
			*c3 = '?';
	}

	if (class->usage_mask & LOCKF_ENABLED_SOFTIRQS_READ)
		*c4 = '-';
	if (class->usage_mask & LOCKF_USED_IN_SOFTIRQ_READ) {
		*c4 = '+';
		if (class->usage_mask & LOCKF_ENABLED_SOFTIRQS_READ)
			*c4 = '?';
	}
}

static void print_lock_name(struct lock_class *class)
{
	char str[KSYM_NAME_LEN], c1, c2, c3, c4;
	const char *name;

	get_usage_chars(class, &c1, &c2, &c3, &c4);

	name = class->name;
	if (!name) {
		name = __get_key_name(class->key, str);
		printk(" (%s", name);
	} else {
		printk(" (%s", name);
		if (class->name_version > 1)
			printk("#%d", class->name_version);
		if (class->subclass)
			printk("/%d", class->subclass);
	}
	printk("){%c%c%c%c}", c1, c2, c3, c4);
}

static void print_lockdep_cache(struct lockdep_map *lock)
{
	const char *name;
	char str[KSYM_NAME_LEN];

	name = lock->name;
	if (!name)
		name = __get_key_name(lock->key->subkeys, str);

	printk("%s", name);
}

static void print_lock(struct held_lock *hlock)
{
	print_lock_name(hlock->class);
	printk(", at: ");
	print_ip_sym(hlock->acquire_ip);
}

static void lockdep_print_held_locks(struct task_struct *curr)
{
	int i, depth = curr->lockdep_depth;

	if (!depth) {
		printk("no locks held by %s/%d.\n", curr->comm, curr->pid);
		return;
	}
	printk("%d lock%s held by %s/%d:\n",
		depth, depth > 1 ? "s" : "", curr->comm, curr->pid);

	for (i = 0; i < depth; i++) {
		printk(" #%d: ", i);
		print_lock(curr->held_locks + i);
	}
}

static void print_lock_class_header(struct lock_class *class, int depth)
{
	int bit;

	printk("%*s->", depth, "");
	print_lock_name(class);
	printk(" ops: %lu", class->ops);
	printk(" {\n");

	for (bit = 0; bit < LOCK_USAGE_STATES; bit++) {
		if (class->usage_mask & (1 << bit)) {
			int len = depth;

			len += printk("%*s   %s", depth, "", usage_str[bit]);
			len += printk(" at:\n");
			print_stack_trace(class->usage_traces + bit, len);
		}
	}
	printk("%*s }\n", depth, "");

	printk("%*s ... key      at: ",depth,"");
	print_ip_sym((unsigned long)class->key);
}

/*
 * printk all lock dependencies starting at <entry>:
 */
static void print_lock_dependencies(struct lock_class *class, int depth)
{
	struct lock_list *entry;

	if (DEBUG_LOCKS_WARN_ON(depth >= 20))
		return;

	print_lock_class_header(class, depth);

	list_for_each_entry(entry, &class->locks_after, entry) {
		if (DEBUG_LOCKS_WARN_ON(!entry->class))
			return;

		print_lock_dependencies(entry->class, depth + 1);

		printk("%*s ... acquired at:\n",depth,"");
		print_stack_trace(&entry->trace, 2);
		printk("\n");
	}
}

static void print_kernel_version(void)
{
	printk("%s %.*s\n", init_utsname()->release,
		(int)strcspn(init_utsname()->version, " "),
		init_utsname()->version);
}

static int very_verbose(struct lock_class *class)
{
#if VERY_VERBOSE
	return class_filter(class);
#endif
	return 0;
}

/*
 * Is this the address of a static object:
 */
static int static_obj(void *obj)
{
	unsigned long start = (unsigned long) &_stext,
		      end   = (unsigned long) &_end,
		      addr  = (unsigned long) obj;
#ifdef CONFIG_SMP
	int i;
#endif

	/*
	 * static variable?
	 */
	if ((addr >= start) && (addr < end))
		return 1;

#ifdef CONFIG_SMP
	/*
	 * percpu var?
	 */
	for_each_possible_cpu(i) {
		start = (unsigned long) &__per_cpu_start + per_cpu_offset(i);
		end   = (unsigned long) &__per_cpu_start + PERCPU_ENOUGH_ROOM
					+ per_cpu_offset(i);

		if ((addr >= start) && (addr < end))
			return 1;
	}
#endif

	/*
	 * module var?
	 */
	return is_module_address(addr);
}

/*
 * To make lock name printouts unique, we calculate a unique
 * class->name_version generation counter:
 */
static int count_matching_names(struct lock_class *new_class)
{
	struct lock_class *class;
	int count = 0;

	if (!new_class->name)
		return 0;

	list_for_each_entry(class, &all_lock_classes, lock_entry) {
		if (new_class->key - new_class->subclass == class->key)
			return class->name_version;
		if (class->name && !strcmp(class->name, new_class->name))
			count = max(count, class->name_version);
	}

	return count + 1;
}

/*
 * Register a lock's class in the hash-table, if the class is not present
 * yet. Otherwise we look it up. We cache the result in the lock object
 * itself, so actual lookup of the hash should be once per lock object.
 */
static inline struct lock_class *
look_up_lock_class(struct lockdep_map *lock, unsigned int subclass)
{
	struct lockdep_subclass_key *key;
	struct list_head *hash_head;
	struct lock_class *class;

#ifdef CONFIG_DEBUG_LOCKDEP
	/*
	 * If the architecture calls into lockdep before initializing
	 * the hashes then we'll warn about it later. (we cannot printk
	 * right now)
	 */
	if (unlikely(!lockdep_initialized)) {
		lockdep_init();
		lockdep_init_error = 1;
		save_stack_trace(&lockdep_init_trace);
	}
#endif

	/*
	 * Static locks do not have their class-keys yet - for them the key
	 * is the lock object itself:
	 */
	if (unlikely(!lock->key))
		lock->key = (void *)lock;

	/*
	 * NOTE: the class-key must be unique. For dynamic locks, a static
	 * lock_class_key variable is passed in through the mutex_init()
	 * (or spin_lock_init()) call - which acts as the key. For static
	 * locks we use the lock object itself as the key.
	 */
	BUILD_BUG_ON(sizeof(struct lock_class_key) >
			sizeof(struct lockdep_map));

	key = lock->key->subkeys + subclass;

	hash_head = classhashentry(key);

	/*
	 * We can walk the hash lockfree, because the hash only
	 * grows, and we are careful when adding entries to the end:
	 */
	list_for_each_entry(class, hash_head, hash_entry) {
		if (class->key == key) {
			WARN_ON_ONCE(class->name != lock->name);
			return class;
		}
	}

	return NULL;
}

/*
 * Register a lock's class in the hash-table, if the class is not present
 * yet. Otherwise we look it up. We cache the result in the lock object
 * itself, so actual lookup of the hash should be once per lock object.
 */
static inline struct lock_class *
register_lock_class(struct lockdep_map *lock, unsigned int subclass, int force)
{
	struct lockdep_subclass_key *key;
	struct list_head *hash_head;
	struct lock_class *class;
	unsigned long flags;

	class = look_up_lock_class(lock, subclass);
	if (likely(class))
		return class;

	/*
	 * Debug-check: all keys must be persistent!
 	 */
	if (!static_obj(lock->key)) {
		debug_locks_off();
		printk("INFO: trying to register non-static key.\n");
		printk("the code is fine but needs lockdep annotation.\n");
		printk("turning off the locking correctness validator.\n");
		dump_stack();

		return NULL;
	}

	key = lock->key->subkeys + subclass;
	hash_head = classhashentry(key);

	raw_local_irq_save(flags);
	if (!graph_lock()) {
		raw_local_irq_restore(flags);
		return NULL;
	}
	/*
	 * We have to do the hash-walk again, to avoid races
	 * with another CPU:
	 */
	list_for_each_entry(class, hash_head, hash_entry)
		if (class->key == key)
			goto out_unlock_set;
	/*
	 * Allocate a new key from the static array, and add it to
	 * the hash:
	 */
	if (nr_lock_classes >= MAX_LOCKDEP_KEYS) {
		if (!debug_locks_off_graph_unlock()) {
			raw_local_irq_restore(flags);
			return NULL;
		}
		raw_local_irq_restore(flags);

		printk("BUG: MAX_LOCKDEP_KEYS too low!\n");
		printk("turning off the locking correctness validator.\n");
		return NULL;
	}
	class = lock_classes + nr_lock_classes++;
	debug_atomic_inc(&nr_unused_locks);
	class->key = key;
	class->name = lock->name;
	class->subclass = subclass;
	INIT_LIST_HEAD(&class->lock_entry);
	INIT_LIST_HEAD(&class->locks_before);
	INIT_LIST_HEAD(&class->locks_after);
	class->name_version = count_matching_names(class);
	/*
	 * We use RCU's safe list-add method to make
	 * parallel walking of the hash-list safe:
	 */
	list_add_tail_rcu(&class->hash_entry, hash_head);

	if (verbose(class)) {
		graph_unlock();
		raw_local_irq_restore(flags);

		printk("\nnew class %p: %s", class->key, class->name);
		if (class->name_version > 1)
			printk("#%d", class->name_version);
		printk("\n");
		dump_stack();

		raw_local_irq_save(flags);
		if (!graph_lock()) {
			raw_local_irq_restore(flags);
			return NULL;
		}
	}
out_unlock_set:
	graph_unlock();
	raw_local_irq_restore(flags);

	if (!subclass || force)
		lock->class_cache = class;

	if (DEBUG_LOCKS_WARN_ON(class->subclass != subclass))
		return NULL;

	return class;
}

#ifdef CONFIG_PROVE_LOCKING
/*
 * Allocate a lockdep entry. (assumes the graph_lock held, returns
 * with NULL on failure)
 */
static struct lock_list *alloc_list_entry(void)
{
	if (nr_list_entries >= MAX_LOCKDEP_ENTRIES) {
		if (!debug_locks_off_graph_unlock())
			return NULL;

		printk("BUG: MAX_LOCKDEP_ENTRIES too low!\n");
		printk("turning off the locking correctness validator.\n");
		return NULL;
	}
	return list_entries + nr_list_entries++;
}

/*
 * Add a new dependency to the head of the list:
 */
static int add_lock_to_list(struct lock_class *class, struct lock_class *this,
			    struct list_head *head, unsigned long ip, int distance)
{
	struct lock_list *entry;
	/*
	 * Lock not present yet - get a new dependency struct and
	 * add it to the list:
	 */
	entry = alloc_list_entry();
	if (!entry)
		return 0;

	entry->class = this;
	entry->distance = distance;
	if (!save_trace(&entry->trace))
		return 0;

	/*
	 * Since we never remove from the dependency list, the list can
	 * be walked lockless by other CPUs, it's only allocation
	 * that must be protected by the spinlock. But this also means
	 * we must make new entries visible only once writes to the
	 * entry become visible - hence the RCU op:
	 */
	list_add_tail_rcu(&entry->entry, head);

	return 1;
}

/*
 * Recursive, forwards-direction lock-dependency checking, used for
 * both noncyclic checking and for hardirq-unsafe/softirq-unsafe
 * checking.
 *
 * (to keep the stackframe of the recursive functions small we
 *  use these global variables, and we also mark various helper
 *  functions as noinline.)
 */
static struct held_lock *check_source, *check_target;

/*
 * Print a dependency chain entry (this is only done when a deadlock
 * has been detected):
 */
static noinline int
print_circular_bug_entry(struct lock_list *target, unsigned int depth)
{
	if (debug_locks_silent)
		return 0;
	printk("\n-> #%u", depth);
	print_lock_name(target->class);
	printk(":\n");
	print_stack_trace(&target->trace, 6);

	return 0;
}

/*
 * When a circular dependency is detected, print the
 * header first:
 */
static noinline int
print_circular_bug_header(struct lock_list *entry, unsigned int depth)
{
	struct task_struct *curr = current;

	if (!debug_locks_off_graph_unlock() || debug_locks_silent)
		return 0;

	printk("\n=======================================================\n");
	printk(  "[ INFO: possible circular locking dependency detected ]\n");
	print_kernel_version();
	printk(  "-------------------------------------------------------\n");
	printk("%s/%d is trying to acquire lock:\n",
		curr->comm, curr->pid);
	print_lock(check_source);
	printk("\nbut task is already holding lock:\n");
	print_lock(check_target);
	printk("\nwhich lock already depends on the new lock.\n\n");
	printk("\nthe existing dependency chain (in reverse order) is:\n");

	print_circular_bug_entry(entry, depth);

	return 0;
}

static noinline int print_circular_bug_tail(void)
{
	struct task_struct *curr = current;
	struct lock_list this;

	if (debug_locks_silent)
		return 0;

	this.class = check_source->class;
	if (!save_trace(&this.trace))
		return 0;

	print_circular_bug_entry(&this, 0);

	printk("\nother info that might help us debug this:\n\n");
	lockdep_print_held_locks(curr);

	printk("\nstack backtrace:\n");
	dump_stack();

	return 0;
}

#define RECURSION_LIMIT 40

static int noinline print_infinite_recursion_bug(void)
{
	if (!debug_locks_off_graph_unlock())
		return 0;

	WARN_ON(1);

	return 0;
}

/*
 * Prove that the dependency graph starting at <entry> can not
 * lead to <target>. Print an error and return 0 if it does.
 */
static noinline int
check_noncircular(struct lock_class *source, unsigned int depth)
{
	struct lock_list *entry;

	debug_atomic_inc(&nr_cyclic_check_recursions);
	if (depth > max_recursion_depth)
		max_recursion_depth = depth;
	if (depth >= RECURSION_LIMIT)
		return print_infinite_recursion_bug();
	/*
	 * Check this lock's dependency list:
	 */
	list_for_each_entry(entry, &source->locks_after, entry) {
		if (entry->class == check_target->class)
			return print_circular_bug_header(entry, depth+1);
		debug_atomic_inc(&nr_cyclic_checks);
		if (!check_noncircular(entry->class, depth+1))
			return print_circular_bug_entry(entry, depth+1);
	}
	return 1;
}

#ifdef CONFIG_TRACE_IRQFLAGS
/*
 * Forwards and backwards subgraph searching, for the purposes of
 * proving that two subgraphs can be connected by a new dependency
 * without creating any illegal irq-safe -> irq-unsafe lock dependency.
 */
static enum lock_usage_bit find_usage_bit;
static struct lock_class *forwards_match, *backwards_match;

/*
 * Find a node in the forwards-direction dependency sub-graph starting
 * at <source> that matches <find_usage_bit>.
 *
 * Return 2 if such a node exists in the subgraph, and put that node
 * into <forwards_match>.
 *
 * Return 1 otherwise and keep <forwards_match> unchanged.
 * Return 0 on error.
 */
static noinline int
find_usage_forwards(struct lock_class *source, unsigned int depth)
{
	struct lock_list *entry;
	int ret;

	if (depth > max_recursion_depth)
		max_recursion_depth = depth;
	if (depth >= RECURSION_LIMIT)
		return print_infinite_recursion_bug();

	debug_atomic_inc(&nr_find_usage_forwards_checks);
	if (source->usage_mask & (1 << find_usage_bit)) {
		forwards_match = source;
		return 2;
	}

	/*
	 * Check this lock's dependency list:
	 */
	list_for_each_entry(entry, &source->locks_after, entry) {
		debug_atomic_inc(&nr_find_usage_forwards_recursions);
		ret = find_usage_forwards(entry->class, depth+1);
		if (ret == 2 || ret == 0)
			return ret;
	}
	return 1;
}

/*
 * Find a node in the backwards-direction dependency sub-graph starting
 * at <source> that matches <find_usage_bit>.
 *
 * Return 2 if such a node exists in the subgraph, and put that node
 * into <backwards_match>.
 *
 * Return 1 otherwise and keep <backwards_match> unchanged.
 * Return 0 on error.
 */
static noinline int
find_usage_backwards(struct lock_class *source, unsigned int depth)
{
	struct lock_list *entry;
	int ret;

	if (!__raw_spin_is_locked(&lockdep_lock))
		return DEBUG_LOCKS_WARN_ON(1);

	if (depth > max_recursion_depth)
		max_recursion_depth = depth;
	if (depth >= RECURSION_LIMIT)
		return print_infinite_recursion_bug();

	debug_atomic_inc(&nr_find_usage_backwards_checks);
	if (source->usage_mask & (1 << find_usage_bit)) {
		backwards_match = source;
		return 2;
	}

	/*
	 * Check this lock's dependency list:
	 */
	list_for_each_entry(entry, &source->locks_before, entry) {
		debug_atomic_inc(&nr_find_usage_backwards_recursions);
		ret = find_usage_backwards(entry->class, depth+1);
		if (ret == 2 || ret == 0)
			return ret;
	}
	return 1;
}

static int
print_bad_irq_dependency(struct task_struct *curr,
			 struct held_lock *prev,
			 struct held_lock *next,
			 enum lock_usage_bit bit1,
			 enum lock_usage_bit bit2,
			 const char *irqclass)
{
	if (!debug_locks_off_graph_unlock() || debug_locks_silent)
		return 0;

	printk("\n======================================================\n");
	printk(  "[ INFO: %s-safe -> %s-unsafe lock order detected ]\n",
		irqclass, irqclass);
	print_kernel_version();
	printk(  "------------------------------------------------------\n");
	printk("%s/%d [HC%u[%lu]:SC%u[%lu]:HE%u:SE%u] is trying to acquire:\n",
		curr->comm, curr->pid,
		curr->hardirq_context, hardirq_count() >> HARDIRQ_SHIFT,
		curr->softirq_context, softirq_count() >> SOFTIRQ_SHIFT,
		curr->hardirqs_enabled,
		curr->softirqs_enabled);
	print_lock(next);

	printk("\nand this task is already holding:\n");
	print_lock(prev);
	printk("which would create a new lock dependency:\n");
	print_lock_name(prev->class);
	printk(" ->");
	print_lock_name(next->class);
	printk("\n");

	printk("\nbut this new dependency connects a %s-irq-safe lock:\n",
		irqclass);
	print_lock_name(backwards_match);
	printk("\n... which became %s-irq-safe at:\n", irqclass);

	print_stack_trace(backwards_match->usage_traces + bit1, 1);

	printk("\nto a %s-irq-unsafe lock:\n", irqclass);
	print_lock_name(forwards_match);
	printk("\n... which became %s-irq-unsafe at:\n", irqclass);
	printk("...");

	print_stack_trace(forwards_match->usage_traces + bit2, 1);

	printk("\nother info that might help us debug this:\n\n");
	lockdep_print_held_locks(curr);

	printk("\nthe %s-irq-safe lock's dependencies:\n", irqclass);
	print_lock_dependencies(backwards_match, 0);

	printk("\nthe %s-irq-unsafe lock's dependencies:\n", irqclass);
	print_lock_dependencies(forwards_match, 0);

	printk("\nstack backtrace:\n");
	dump_stack();

	return 0;
}

static int
check_usage(struct task_struct *curr, struct held_lock *prev,
	    struct held_lock *next, enum lock_usage_bit bit_backwards,
	    enum lock_usage_bit bit_forwards, const char *irqclass)
{
	int ret;

	find_usage_bit = bit_backwards;
	/* fills in <backwards_match> */
	ret = find_usage_backwards(prev->class, 0);
	if (!ret || ret == 1)
		return ret;

	find_usage_bit = bit_forwards;
	ret = find_usage_forwards(next->class, 0);
	if (!ret || ret == 1)
		return ret;
	/* ret == 2 */
	return print_bad_irq_dependency(curr, prev, next,
			bit_backwards, bit_forwards, irqclass);
}

static int
check_prev_add_irq(struct task_struct *curr, struct held_lock *prev,
		struct held_lock *next)
{
	/*
	 * Prove that the new dependency does not connect a hardirq-safe
	 * lock with a hardirq-unsafe lock - to achieve this we search
	 * the backwards-subgraph starting at <prev>, and the
	 * forwards-subgraph starting at <next>:
	 */
	if (!check_usage(curr, prev, next, LOCK_USED_IN_HARDIRQ,
					LOCK_ENABLED_HARDIRQS, "hard"))
		return 0;

	/*
	 * Prove that the new dependency does not connect a hardirq-safe-read
	 * lock with a hardirq-unsafe lock - to achieve this we search
	 * the backwards-subgraph starting at <prev>, and the
	 * forwards-subgraph starting at <next>:
	 */
	if (!check_usage(curr, prev, next, LOCK_USED_IN_HARDIRQ_READ,
					LOCK_ENABLED_HARDIRQS, "hard-read"))
		return 0;

	/*
	 * Prove that the new dependency does not connect a softirq-safe
	 * lock with a softirq-unsafe lock - to achieve this we search
	 * the backwards-subgraph starting at <prev>, and the
	 * forwards-subgraph starting at <next>:
	 */
	if (!check_usage(curr, prev, next, LOCK_USED_IN_SOFTIRQ,
					LOCK_ENABLED_SOFTIRQS, "soft"))
		return 0;
	/*
	 * Prove that the new dependency does not connect a softirq-safe-read
	 * lock with a softirq-unsafe lock - to achieve this we search
	 * the backwards-subgraph starting at <prev>, and the
	 * forwards-subgraph starting at <next>:
	 */
	if (!check_usage(curr, prev, next, LOCK_USED_IN_SOFTIRQ_READ,
					LOCK_ENABLED_SOFTIRQS, "soft"))
		return 0;

	return 1;
}

static void inc_chains(void)
{
	if (current->hardirq_context)
		nr_hardirq_chains++;
	else {
		if (current->softirq_context)
			nr_softirq_chains++;
		else
			nr_process_chains++;
	}
}

#else

static inline int
check_prev_add_irq(struct task_struct *curr, struct held_lock *prev,
		struct held_lock *next)
{
	return 1;
}

static inline void inc_chains(void)
{
	nr_process_chains++;
}

#endif

static int
print_deadlock_bug(struct task_struct *curr, struct held_lock *prev,
		   struct held_lock *next)
{
	if (!debug_locks_off_graph_unlock() || debug_locks_silent)
		return 0;

	printk("\n=============================================\n");
	printk(  "[ INFO: possible recursive locking detected ]\n");
	print_kernel_version();
	printk(  "---------------------------------------------\n");
	printk("%s/%d is trying to acquire lock:\n",
		curr->comm, curr->pid);
	print_lock(next);
	printk("\nbut task is already holding lock:\n");
	print_lock(prev);

	printk("\nother info that might help us debug this:\n");
	lockdep_print_held_locks(curr);

	printk("\nstack backtrace:\n");
	dump_stack();

	return 0;
}

/*
 * Check whether we are holding such a class already.
 *
 * (Note that this has to be done separately, because the graph cannot
 * detect such classes of deadlocks.)
 *
 * Returns: 0 on deadlock detected, 1 on OK, 2 on recursive read
 */
static int
check_deadlock(struct task_struct *curr, struct held_lock *next,
	       struct lockdep_map *next_instance, int read)
{
	struct held_lock *prev;
	int i;

	for (i = 0; i < curr->lockdep_depth; i++) {
		prev = curr->held_locks + i;
		if (prev->class != next->class)
			continue;
		/*
		 * Allow read-after-read recursion of the same
		 * lock class (i.e. read_lock(lock)+read_lock(lock)):
		 */
		if ((read == 2) && prev->read)
			return 2;
		return print_deadlock_bug(curr, prev, next);
	}
	return 1;
}

/*
 * There was a chain-cache miss, and we are about to add a new dependency
 * to a previous lock. We recursively validate the following rules:
 *
 *  - would the adding of the <prev> -> <next> dependency create a
 *    circular dependency in the graph? [== circular deadlock]
 *
 *  - does the new prev->next dependency connect any hardirq-safe lock
 *    (in the full backwards-subgraph starting at <prev>) with any
 *    hardirq-unsafe lock (in the full forwards-subgraph starting at
 *    <next>)? [== illegal lock inversion with hardirq contexts]
 *
 *  - does the new prev->next dependency connect any softirq-safe lock
 *    (in the full backwards-subgraph starting at <prev>) with any
 *    softirq-unsafe lock (in the full forwards-subgraph starting at
 *    <next>)? [== illegal lock inversion with softirq contexts]
 *
 * any of these scenarios could lead to a deadlock.
 *
 * Then if all the validations pass, we add the forwards and backwards
 * dependency.
 */
static int
check_prev_add(struct task_struct *curr, struct held_lock *prev,
	       struct held_lock *next, int distance)
{
	struct lock_list *entry;
	int ret;

	/*
	 * Prove that the new <prev> -> <next> dependency would not
	 * create a circular dependency in the graph. (We do this by
	 * forward-recursing into the graph starting at <next>, and
	 * checking whether we can reach <prev>.)
	 *
	 * We are using global variables to control the recursion, to
	 * keep the stackframe size of the recursive functions low:
	 */
	check_source = next;
	check_target = prev;
	if (!(check_noncircular(next->class, 0)))
		return print_circular_bug_tail();

	if (!check_prev_add_irq(curr, prev, next))
		return 0;

	/*
	 * For recursive read-locks we do all the dependency checks,
	 * but we dont store read-triggered dependencies (only
	 * write-triggered dependencies). This ensures that only the
	 * write-side dependencies matter, and that if for example a
	 * write-lock never takes any other locks, then the reads are
	 * equivalent to a NOP.
	 */
	if (next->read == 2 || prev->read == 2)
		return 1;
	/*
	 * Is the <prev> -> <next> dependency already present?
	 *
	 * (this may occur even though this is a new chain: consider
	 *  e.g. the L1 -> L2 -> L3 -> L4 and the L5 -> L1 -> L2 -> L3
	 *  chains - the second one will be new, but L1 already has
	 *  L2 added to its dependency list, due to the first chain.)
	 */
	list_for_each_entry(entry, &prev->class->locks_after, entry) {
		if (entry->class == next->class) {
			if (distance == 1)
				entry->distance = 1;
			return 2;
		}
	}

	/*
	 * Ok, all validations passed, add the new lock
	 * to the previous lock's dependency list:
	 */
	ret = add_lock_to_list(prev->class, next->class,
			       &prev->class->locks_after, next->acquire_ip, distance);

	if (!ret)
		return 0;

	ret = add_lock_to_list(next->class, prev->class,
			       &next->class->locks_before, next->acquire_ip, distance);
	if (!ret)
		return 0;

	/*
	 * Debugging printouts:
	 */
	if (verbose(prev->class) || verbose(next->class)) {
		graph_unlock();
		printk("\n new dependency: ");
		print_lock_name(prev->class);
		printk(" => ");
		print_lock_name(next->class);
		printk("\n");
		dump_stack();
		return graph_lock();
	}
	return 1;
}

/*
 * Add the dependency to all directly-previous locks that are 'relevant'.
 * The ones that are relevant are (in increasing distance from curr):
 * all consecutive trylock entries and the final non-trylock entry - or
 * the end of this context's lock-chain - whichever comes first.
 */
static int
check_prevs_add(struct task_struct *curr, struct held_lock *next)
{
	int depth = curr->lockdep_depth;
	struct held_lock *hlock;

	/*
	 * Debugging checks.
	 *
	 * Depth must not be zero for a non-head lock:
	 */
	if (!depth)
		goto out_bug;
	/*
	 * At least two relevant locks must exist for this
	 * to be a head:
	 */
	if (curr->held_locks[depth].irq_context !=
			curr->held_locks[depth-1].irq_context)
		goto out_bug;

	for (;;) {
		int distance = curr->lockdep_depth - depth + 1;
		hlock = curr->held_locks + depth-1;
		/*
		 * Only non-recursive-read entries get new dependencies
		 * added:
		 */
		if (hlock->read != 2) {
			if (!check_prev_add(curr, hlock, next, distance))
				return 0;
			/*
			 * Stop after the first non-trylock entry,
			 * as non-trylock entries have added their
			 * own direct dependencies already, so this
			 * lock is connected to them indirectly:
			 */
			if (!hlock->trylock)
				break;
		}
		depth--;
		/*
		 * End of lock-stack?
		 */
		if (!depth)
			break;
		/*
		 * Stop the search if we cross into another context:
		 */
		if (curr->held_locks[depth].irq_context !=
				curr->held_locks[depth-1].irq_context)
			break;
	}
	return 1;
out_bug:
	if (!debug_locks_off_graph_unlock())
		return 0;

	WARN_ON(1);

	return 0;
}

unsigned long nr_lock_chains;
static struct lock_chain lock_chains[MAX_LOCKDEP_CHAINS];

/*
 * Look up a dependency chain. If the key is not present yet then
 * add it and return 1 - in this case the new dependency chain is
 * validated. If the key is already hashed, return 0.
 * (On return with 1 graph_lock is held.)
 */
static inline int lookup_chain_cache(u64 chain_key, struct lock_class *class)
{
	struct list_head *hash_head = chainhashentry(chain_key);
	struct lock_chain *chain;

	if (DEBUG_LOCKS_WARN_ON(!irqs_disabled()))
		return 0;
	/*
	 * We can walk it lock-free, because entries only get added
	 * to the hash:
	 */
	list_for_each_entry(chain, hash_head, entry) {
		if (chain->chain_key == chain_key) {
cache_hit:
			debug_atomic_inc(&chain_lookup_hits);
			if (very_verbose(class))
				printk("\nhash chain already cached, key: "
					"%016Lx tail class: [%p] %s\n",
					(unsigned long long)chain_key,
					class->key, class->name);
			return 0;
		}
	}
	if (very_verbose(class))
		printk("\nnew hash chain, key: %016Lx tail class: [%p] %s\n",
			(unsigned long long)chain_key, class->key, class->name);
	/*
	 * Allocate a new chain entry from the static array, and add
	 * it to the hash:
	 */
	if (!graph_lock())
		return 0;
	/*
	 * We have to walk the chain again locked - to avoid duplicates:
	 */
	list_for_each_entry(chain, hash_head, entry) {
		if (chain->chain_key == chain_key) {
			graph_unlock();
			goto cache_hit;
		}
	}
	if (unlikely(nr_lock_chains >= MAX_LOCKDEP_CHAINS)) {
		if (!debug_locks_off_graph_unlock())
			return 0;

		printk("BUG: MAX_LOCKDEP_CHAINS too low!\n");
		printk("turning off the locking correctness validator.\n");
		return 0;
	}
	chain = lock_chains + nr_lock_chains++;
	chain->chain_key = chain_key;
	list_add_tail_rcu(&chain->entry, hash_head);
	debug_atomic_inc(&chain_lookup_misses);
	inc_chains();

	return 1;
}

static int validate_chain(struct task_struct *curr, struct lockdep_map *lock,
	       	struct held_lock *hlock, int chain_head)
{
	/*
	 * Trylock needs to maintain the stack of held locks, but it
	 * does not add new dependencies, because trylock can be done
	 * in any order.
	 *
	 * We look up the chain_key and do the O(N^2) check and update of
	 * the dependencies only if this is a new dependency chain.
	 * (If lookup_chain_cache() returns with 1 it acquires
	 * graph_lock for us)
	 */
	if (!hlock->trylock && (hlock->check == 2) &&
			lookup_chain_cache(curr->curr_chain_key, hlock->class)) {
		/*
		 * Check whether last held lock:
		 *
		 * - is irq-safe, if this lock is irq-unsafe
		 * - is softirq-safe, if this lock is hardirq-unsafe
		 *
		 * And check whether the new lock's dependency graph
		 * could lead back to the previous lock.
		 *
		 * any of these scenarios could lead to a deadlock. If
		 * All validations
		 */
		int ret = check_deadlock(curr, hlock, lock, hlock->read);

		if (!ret)
			return 0;
		/*
		 * Mark recursive read, as we jump over it when
		 * building dependencies (just like we jump over
		 * trylock entries):
		 */
		if (ret == 2)
			hlock->read = 2;
		/*
		 * Add dependency only if this lock is not the head
		 * of the chain, and if it's not a secondary read-lock:
		 */
		if (!chain_head && ret != 2)
			if (!check_prevs_add(curr, hlock))
				return 0;
		graph_unlock();
	} else
		/* after lookup_chain_cache(): */
		if (unlikely(!debug_locks))
			return 0;

	return 1;
}
#else
static inline int validate_chain(struct task_struct *curr,
	       	struct lockdep_map *lock, struct held_lock *hlock,
		int chain_head)
{
	return 1;
}
#endif

/*
 * We are building curr_chain_key incrementally, so double-check
 * it from scratch, to make sure that it's done correctly:
 */
static void check_chain_key(struct task_struct *curr)
{
#ifdef CONFIG_DEBUG_LOCKDEP
	struct held_lock *hlock, *prev_hlock = NULL;
	unsigned int i, id;
	u64 chain_key = 0;

	for (i = 0; i < curr->lockdep_depth; i++) {
		hlock = curr->held_locks + i;
		if (chain_key != hlock->prev_chain_key) {
			debug_locks_off();
			printk("hm#1, depth: %u [%u], %016Lx != %016Lx\n",
				curr->lockdep_depth, i,
				(unsigned long long)chain_key,
				(unsigned long long)hlock->prev_chain_key);
			WARN_ON(1);
			return;
		}
		id = hlock->class - lock_classes;
		if (DEBUG_LOCKS_WARN_ON(id >= MAX_LOCKDEP_KEYS))
			return;

		if (prev_hlock && (prev_hlock->irq_context !=
							hlock->irq_context))
			chain_key = 0;
		chain_key = iterate_chain_key(chain_key, id);
		prev_hlock = hlock;
	}
	if (chain_key != curr->curr_chain_key) {
		debug_locks_off();
		printk("hm#2, depth: %u [%u], %016Lx != %016Lx\n",
			curr->lockdep_depth, i,
			(unsigned long long)chain_key,
			(unsigned long long)curr->curr_chain_key);
		WARN_ON(1);
	}
#endif
}

static int
print_usage_bug(struct task_struct *curr, struct held_lock *this,
		enum lock_usage_bit prev_bit, enum lock_usage_bit new_bit)
{
	if (!debug_locks_off_graph_unlock() || debug_locks_silent)
		return 0;

	printk("\n=================================\n");
	printk(  "[ INFO: inconsistent lock state ]\n");
	print_kernel_version();
	printk(  "---------------------------------\n");

	printk("inconsistent {%s} -> {%s} usage.\n",
		usage_str[prev_bit], usage_str[new_bit]);

	printk("%s/%d [HC%u[%lu]:SC%u[%lu]:HE%u:SE%u] takes:\n",
		curr->comm, curr->pid,
		trace_hardirq_context(curr), hardirq_count() >> HARDIRQ_SHIFT,
		trace_softirq_context(curr), softirq_count() >> SOFTIRQ_SHIFT,
		trace_hardirqs_enabled(curr),
		trace_softirqs_enabled(curr));
	print_lock(this);

	printk("{%s} state was registered at:\n", usage_str[prev_bit]);
	print_stack_trace(this->class->usage_traces + prev_bit, 1);

	print_irqtrace_events(curr);
	printk("\nother info that might help us debug this:\n");
	lockdep_print_held_locks(curr);

	printk("\nstack backtrace:\n");
	dump_stack();

	return 0;
}

/*
 * Print out an error if an invalid bit is set:
 */
static inline int
valid_state(struct task_struct *curr, struct held_lock *this,
	    enum lock_usage_bit new_bit, enum lock_usage_bit bad_bit)
{
	if (unlikely(this->class->usage_mask & (1 << bad_bit)))
		return print_usage_bug(curr, this, bad_bit, new_bit);
	return 1;
}

static int mark_lock(struct task_struct *curr, struct held_lock *this,
		     enum lock_usage_bit new_bit);

#ifdef CONFIG_TRACE_IRQFLAGS

/*
 * print irq inversion bug:
 */
static int
print_irq_inversion_bug(struct task_struct *curr, struct lock_class *other,
			struct held_lock *this, int forwards,
			const char *irqclass)
{
	if (!debug_locks_off_graph_unlock() || debug_locks_silent)
		return 0;

	printk("\n=========================================================\n");
	printk(  "[ INFO: possible irq lock inversion dependency detected ]\n");
	print_kernel_version();
	printk(  "---------------------------------------------------------\n");
	printk("%s/%d just changed the state of lock:\n",
		curr->comm, curr->pid);
	print_lock(this);
	if (forwards)
		printk("but this lock took another, %s-irq-unsafe lock in the past:\n", irqclass);
	else
		printk("but this lock was taken by another, %s-irq-safe lock in the past:\n", irqclass);
	print_lock_name(other);
	printk("\n\nand interrupts could create inverse lock ordering between them.\n\n");

	printk("\nother info that might help us debug this:\n");
	lockdep_print_held_locks(curr);

	printk("\nthe first lock's dependencies:\n");
	print_lock_dependencies(this->class, 0);

	printk("\nthe second lock's dependencies:\n");
	print_lock_dependencies(other, 0);

	printk("\nstack backtrace:\n");
	dump_stack();

	return 0;
}

/*
 * Prove that in the forwards-direction subgraph starting at <this>
 * there is no lock matching <mask>:
 */
static int
check_usage_forwards(struct task_struct *curr, struct held_lock *this,
		     enum lock_usage_bit bit, const char *irqclass)
{
	int ret;

	find_usage_bit = bit;
	/* fills in <forwards_match> */
	ret = find_usage_forwards(this->class, 0);
	if (!ret || ret == 1)
		return ret;

	return print_irq_inversion_bug(curr, forwards_match, this, 1, irqclass);
}

/*
 * Prove that in the backwards-direction subgraph starting at <this>
 * there is no lock matching <mask>:
 */
static int
check_usage_backwards(struct task_struct *curr, struct held_lock *this,
		      enum lock_usage_bit bit, const char *irqclass)
{
	int ret;

	find_usage_bit = bit;
	/* fills in <backwards_match> */
	ret = find_usage_backwards(this->class, 0);
	if (!ret || ret == 1)
		return ret;

	return print_irq_inversion_bug(curr, backwards_match, this, 0, irqclass);
}

void print_irqtrace_events(struct task_struct *curr)
{
	printk("irq event stamp: %u\n", curr->irq_events);
	printk("hardirqs last  enabled at (%u): ", curr->hardirq_enable_event);
	print_ip_sym(curr->hardirq_enable_ip);
	printk("hardirqs last disabled at (%u): ", curr->hardirq_disable_event);
	print_ip_sym(curr->hardirq_disable_ip);
	printk("softirqs last  enabled at (%u): ", curr->softirq_enable_event);
	print_ip_sym(curr->softirq_enable_ip);
	printk("softirqs last disabled at (%u): ", curr->softirq_disable_event);
	print_ip_sym(curr->softirq_disable_ip);
}

static int hardirq_verbose(struct lock_class *class)
{
#if HARDIRQ_VERBOSE
	return class_filter(class);
#endif
	return 0;
}

static int softirq_verbose(struct lock_class *class)
{
#if SOFTIRQ_VERBOSE
	return class_filter(class);
#endif
	return 0;
}

#define STRICT_READ_CHECKS	1

static int mark_lock_irq(struct task_struct *curr, struct held_lock *this,
		enum lock_usage_bit new_bit)
{
	int ret = 1;

	switch(new_bit) {
	case LOCK_USED_IN_HARDIRQ:
		if (!valid_state(curr, this, new_bit, LOCK_ENABLED_HARDIRQS))
			return 0;
		if (!valid_state(curr, this, new_bit,
				 LOCK_ENABLED_HARDIRQS_READ))
			return 0;
		/*
		 * just marked it hardirq-safe, check that this lock
		 * took no hardirq-unsafe lock in the past:
		 */
		if (!check_usage_forwards(curr, this,
					  LOCK_ENABLED_HARDIRQS, "hard"))
			return 0;
#if STRICT_READ_CHECKS
		/*
		 * just marked it hardirq-safe, check that this lock
		 * took no hardirq-unsafe-read lock in the past:
		 */
		if (!check_usage_forwards(curr, this,
				LOCK_ENABLED_HARDIRQS_READ, "hard-read"))
			return 0;
#endif
		if (hardirq_verbose(this->class))
			ret = 2;
		break;
	case LOCK_USED_IN_SOFTIRQ:
		if (!valid_state(curr, this, new_bit, LOCK_ENABLED_SOFTIRQS))
			return 0;
		if (!valid_state(curr, this, new_bit,
				 LOCK_ENABLED_SOFTIRQS_READ))
			return 0;
		/*
		 * just marked it softirq-safe, check that this lock
		 * took no softirq-unsafe lock in the past:
		 */
		if (!check_usage_forwards(curr, this,
					  LOCK_ENABLED_SOFTIRQS, "soft"))
			return 0;
#if STRICT_READ_CHECKS
		/*
		 * just marked it softirq-safe, check that this lock
		 * took no softirq-unsafe-read lock in the past:
		 */
		if (!check_usage_forwards(curr, this,
				LOCK_ENABLED_SOFTIRQS_READ, "soft-read"))
			return 0;
#endif
		if (softirq_verbose(this->class))
			ret = 2;
		break;
	case LOCK_USED_IN_HARDIRQ_READ:
		if (!valid_state(curr, this, new_bit, LOCK_ENABLED_HARDIRQS))
			return 0;
		/*
		 * just marked it hardirq-read-safe, check that this lock
		 * took no hardirq-unsafe lock in the past:
		 */
		if (!check_usage_forwards(curr, this,
					  LOCK_ENABLED_HARDIRQS, "hard"))
			return 0;
		if (hardirq_verbose(this->class))
			ret = 2;
		break;
	case LOCK_USED_IN_SOFTIRQ_READ:
		if (!valid_state(curr, this, new_bit, LOCK_ENABLED_SOFTIRQS))
			return 0;
		/*
		 * just marked it softirq-read-safe, check that this lock
		 * took no softirq-unsafe lock in the past:
		 */
		if (!check_usage_forwards(curr, this,
					  LOCK_ENABLED_SOFTIRQS, "soft"))
			return 0;
		if (softirq_verbose(this->class))
			ret = 2;
		break;
	case LOCK_ENABLED_HARDIRQS:
		if (!valid_state(curr, this, new_bit, LOCK_USED_IN_HARDIRQ))
			return 0;
		if (!valid_state(curr, this, new_bit,
				 LOCK_USED_IN_HARDIRQ_READ))
			return 0;
		/*
		 * just marked it hardirq-unsafe, check that no hardirq-safe
		 * lock in the system ever took it in the past:
		 */
		if (!check_usage_backwards(curr, this,
					   LOCK_USED_IN_HARDIRQ, "hard"))
			return 0;
#if STRICT_READ_CHECKS
		/*
		 * just marked it hardirq-unsafe, check that no
		 * hardirq-safe-read lock in the system ever took
		 * it in the past:
		 */
		if (!check_usage_backwards(curr, this,
				   LOCK_USED_IN_HARDIRQ_READ, "hard-read"))
			return 0;
#endif
		if (hardirq_verbose(this->class))
			ret = 2;
		break;
	case LOCK_ENABLED_SOFTIRQS:
		if (!valid_state(curr, this, new_bit, LOCK_USED_IN_SOFTIRQ))
			return 0;
		if (!valid_state(curr, this, new_bit,
				 LOCK_USED_IN_SOFTIRQ_READ))
			return 0;
		/*
		 * just marked it softirq-unsafe, check that no softirq-safe
		 * lock in the system ever took it in the past:
		 */
		if (!check_usage_backwards(curr, this,
					   LOCK_USED_IN_SOFTIRQ, "soft"))
			return 0;
#if STRICT_READ_CHECKS
		/*
		 * just marked it softirq-unsafe, check that no
		 * softirq-safe-read lock in the system ever took
		 * it in the past:
		 */
		if (!check_usage_backwards(curr, this,
				   LOCK_USED_IN_SOFTIRQ_READ, "soft-read"))
			return 0;
#endif
		if (softirq_verbose(this->class))
			ret = 2;
		break;
	case LOCK_ENABLED_HARDIRQS_READ:
		if (!valid_state(curr, this, new_bit, LOCK_USED_IN_HARDIRQ))
			return 0;
#if STRICT_READ_CHECKS
		/*
		 * just marked it hardirq-read-unsafe, check that no
		 * hardirq-safe lock in the system ever took it in the past:
		 */
		if (!check_usage_backwards(curr, this,
					   LOCK_USED_IN_HARDIRQ, "hard"))
			return 0;
#endif
		if (hardirq_verbose(this->class))
			ret = 2;
		break;
	case LOCK_ENABLED_SOFTIRQS_READ:
		if (!valid_state(curr, this, new_bit, LOCK_USED_IN_SOFTIRQ))
			return 0;
#if STRICT_READ_CHECKS
		/*
		 * just marked it softirq-read-unsafe, check that no
		 * softirq-safe lock in the system ever took it in the past:
		 */
		if (!check_usage_backwards(curr, this,
					   LOCK_USED_IN_SOFTIRQ, "soft"))
			return 0;
#endif
		if (softirq_verbose(this->class))
			ret = 2;
		break;
	default:
		WARN_ON(1);
		break;
	}

	return ret;
}

/*
 * Mark all held locks with a usage bit:
 */
static int
mark_held_locks(struct task_struct *curr, int hardirq)
{
	enum lock_usage_bit usage_bit;
	struct held_lock *hlock;
	int i;

	for (i = 0; i < curr->lockdep_depth; i++) {
		hlock = curr->held_locks + i;

		if (hardirq) {
			if (hlock->read)
				usage_bit = LOCK_ENABLED_HARDIRQS_READ;
			else
				usage_bit = LOCK_ENABLED_HARDIRQS;
		} else {
			if (hlock->read)
				usage_bit = LOCK_ENABLED_SOFTIRQS_READ;
			else
				usage_bit = LOCK_ENABLED_SOFTIRQS;
		}
		if (!mark_lock(curr, hlock, usage_bit))
			return 0;
	}

	return 1;
}

/*
 * Debugging helper: via this flag we know that we are in
 * 'early bootup code', and will warn about any invalid irqs-on event:
 */
static int early_boot_irqs_enabled;

void early_boot_irqs_off(void)
{
	early_boot_irqs_enabled = 0;
}

void early_boot_irqs_on(void)
{
	early_boot_irqs_enabled = 1;
}

/*
 * Hardirqs will be enabled:
 */
void trace_hardirqs_on(void)
{
	struct task_struct *curr = current;
	unsigned long ip;

	if (unlikely(!debug_locks || current->lockdep_recursion))
		return;

	if (DEBUG_LOCKS_WARN_ON(unlikely(!early_boot_irqs_enabled)))
		return;

	if (unlikely(curr->hardirqs_enabled)) {
		debug_atomic_inc(&redundant_hardirqs_on);
		return;
	}
	/* we'll do an OFF -> ON transition: */
	curr->hardirqs_enabled = 1;
	ip = (unsigned long) __builtin_return_address(0);

	if (DEBUG_LOCKS_WARN_ON(!irqs_disabled()))
		return;
	if (DEBUG_LOCKS_WARN_ON(current->hardirq_context))
		return;
	/*
	 * We are going to turn hardirqs on, so set the
	 * usage bit for all held locks:
	 */
	if (!mark_held_locks(curr, 1))
		return;
	/*
	 * If we have softirqs enabled, then set the usage
	 * bit for all held locks. (disabled hardirqs prevented
	 * this bit from being set before)
	 */
	if (curr->softirqs_enabled)
		if (!mark_held_locks(curr, 0))
			return;

	curr->hardirq_enable_ip = ip;
	curr->hardirq_enable_event = ++curr->irq_events;
	debug_atomic_inc(&hardirqs_on_events);
}

EXPORT_SYMBOL(trace_hardirqs_on);

/*
 * Hardirqs were disabled:
 */
void trace_hardirqs_off(void)
{
	struct task_struct *curr = current;

	if (unlikely(!debug_locks || current->lockdep_recursion))
		return;

	if (DEBUG_LOCKS_WARN_ON(!irqs_disabled()))
		return;

	if (curr->hardirqs_enabled) {
		/*
		 * We have done an ON -> OFF transition:
		 */
		curr->hardirqs_enabled = 0;
		curr->hardirq_disable_ip = _RET_IP_;
		curr->hardirq_disable_event = ++curr->irq_events;
		debug_atomic_inc(&hardirqs_off_events);
	} else
		debug_atomic_inc(&redundant_hardirqs_off);
}

EXPORT_SYMBOL(trace_hardirqs_off);

/*
 * Softirqs will be enabled:
 */
void trace_softirqs_on(unsigned long ip)
{
	struct task_struct *curr = current;

	if (unlikely(!debug_locks))
		return;

	if (DEBUG_LOCKS_WARN_ON(!irqs_disabled()))
		return;

	if (curr->softirqs_enabled) {
		debug_atomic_inc(&redundant_softirqs_on);
		return;
	}

	/*
	 * We'll do an OFF -> ON transition:
	 */
	curr->softirqs_enabled = 1;
	curr->softirq_enable_ip = ip;
	curr->softirq_enable_event = ++curr->irq_events;
	debug_atomic_inc(&softirqs_on_events);
	/*
	 * We are going to turn softirqs on, so set the
	 * usage bit for all held locks, if hardirqs are
	 * enabled too:
	 */
	if (curr->hardirqs_enabled)
		mark_held_locks(curr, 0);
}

/*
 * Softirqs were disabled:
 */
void trace_softirqs_off(unsigned long ip)
{
	struct task_struct *curr = current;

	if (unlikely(!debug_locks))
		return;

	if (DEBUG_LOCKS_WARN_ON(!irqs_disabled()))
		return;

	if (curr->softirqs_enabled) {
		/*
		 * We have done an ON -> OFF transition:
		 */
		curr->softirqs_enabled = 0;
		curr->softirq_disable_ip = ip;
		curr->softirq_disable_event = ++curr->irq_events;
		debug_atomic_inc(&softirqs_off_events);
		DEBUG_LOCKS_WARN_ON(!softirq_count());
	} else
		debug_atomic_inc(&redundant_softirqs_off);
}

static int mark_irqflags(struct task_struct *curr, struct held_lock *hlock)
{
	/*
	 * If non-trylock use in a hardirq or softirq context, then
	 * mark the lock as used in these contexts:
	 */
	if (!hlock->trylock) {
		if (hlock->read) {
			if (curr->hardirq_context)
				if (!mark_lock(curr, hlock,
						LOCK_USED_IN_HARDIRQ_READ))
					return 0;
			if (curr->softirq_context)
				if (!mark_lock(curr, hlock,
						LOCK_USED_IN_SOFTIRQ_READ))
					return 0;
		} else {
			if (curr->hardirq_context)
				if (!mark_lock(curr, hlock, LOCK_USED_IN_HARDIRQ))
					return 0;
			if (curr->softirq_context)
				if (!mark_lock(curr, hlock, LOCK_USED_IN_SOFTIRQ))
					return 0;
		}
	}
	if (!hlock->hardirqs_off) {
		if (hlock->read) {
			if (!mark_lock(curr, hlock,
					LOCK_ENABLED_HARDIRQS_READ))
				return 0;
			if (curr->softirqs_enabled)
				if (!mark_lock(curr, hlock,
						LOCK_ENABLED_SOFTIRQS_READ))
					return 0;
		} else {
			if (!mark_lock(curr, hlock,
					LOCK_ENABLED_HARDIRQS))
				return 0;
			if (curr->softirqs_enabled)
				if (!mark_lock(curr, hlock,
						LOCK_ENABLED_SOFTIRQS))
					return 0;
		}
	}

	return 1;
}

static int separate_irq_context(struct task_struct *curr,
		struct held_lock *hlock)
{
	unsigned int depth = curr->lockdep_depth;

	/*
	 * Keep track of points where we cross into an interrupt context:
	 */
	hlock->irq_context = 2*(curr->hardirq_context ? 1 : 0) +
				curr->softirq_context;
	if (depth) {
		struct held_lock *prev_hlock;

		prev_hlock = curr->held_locks + depth-1;
		/*
		 * If we cross into another context, reset the
		 * hash key (this also prevents the checking and the
		 * adding of the dependency to 'prev'):
		 */
		if (prev_hlock->irq_context != hlock->irq_context)
			return 1;
	}
	return 0;
}

#else

static inline
int mark_lock_irq(struct task_struct *curr, struct held_lock *this,
		enum lock_usage_bit new_bit)
{
	WARN_ON(1);
	return 1;
}

static inline int mark_irqflags(struct task_struct *curr,
		struct held_lock *hlock)
{
	return 1;
}

static inline int separate_irq_context(struct task_struct *curr,
		struct held_lock *hlock)
{
	return 0;
}

#endif

/*
 * Mark a lock with a usage bit, and validate the state transition:
 */
static int mark_lock(struct task_struct *curr, struct held_lock *this,
		     enum lock_usage_bit new_bit)
{
	unsigned int new_mask = 1 << new_bit, ret = 1;

	/*
	 * If already set then do not dirty the cacheline,
	 * nor do any checks:
	 */
	if (likely(this->class->usage_mask & new_mask))
		return 1;

	if (!graph_lock())
		return 0;
	/*
	 * Make sure we didnt race:
	 */
	if (unlikely(this->class->usage_mask & new_mask)) {
		graph_unlock();
		return 1;
	}

	this->class->usage_mask |= new_mask;

	if (!save_trace(this->class->usage_traces + new_bit))
		return 0;

	switch (new_bit) {
	case LOCK_USED_IN_HARDIRQ:
	case LOCK_USED_IN_SOFTIRQ:
	case LOCK_USED_IN_HARDIRQ_READ:
	case LOCK_USED_IN_SOFTIRQ_READ:
	case LOCK_ENABLED_HARDIRQS:
	case LOCK_ENABLED_SOFTIRQS:
	case LOCK_ENABLED_HARDIRQS_READ:
	case LOCK_ENABLED_SOFTIRQS_READ:
		ret = mark_lock_irq(curr, this, new_bit);
		if (!ret)
			return 0;
		break;
	case LOCK_USED:
		/*
		 * Add it to the global list of classes:
		 */
		list_add_tail_rcu(&this->class->lock_entry, &all_lock_classes);
		debug_atomic_dec(&nr_unused_locks);
		break;
	default:
		if (!debug_locks_off_graph_unlock())
			return 0;
		WARN_ON(1);
		return 0;
	}

	graph_unlock();

	/*
	 * We must printk outside of the graph_lock:
	 */
	if (ret == 2) {
		printk("\nmarked lock as {%s}:\n", usage_str[new_bit]);
		print_lock(this);
		print_irqtrace_events(curr);
		dump_stack();
	}

	return ret;
}

/*
 * Initialize a lock instance's lock-class mapping info:
 */
void lockdep_init_map(struct lockdep_map *lock, const char *name,
		      struct lock_class_key *key, int subclass)
{
	if (unlikely(!debug_locks))
		return;

	if (DEBUG_LOCKS_WARN_ON(!key))
		return;
	if (DEBUG_LOCKS_WARN_ON(!name))
		return;
	/*
	 * Sanity check, the lock-class key must be persistent:
	 */
	if (!static_obj(key)) {
		printk("BUG: key %p not in .data!\n", key);
		DEBUG_LOCKS_WARN_ON(1);
		return;
	}
	lock->name = name;
	lock->key = key;
	lock->class_cache = NULL;
#ifdef CONFIG_LOCK_STAT
	lock->cpu = raw_smp_processor_id();
#endif
	if (subclass)
		register_lock_class(lock, subclass, 1);
}

EXPORT_SYMBOL_GPL(lockdep_init_map);

/*
 * This gets called for every mutex_lock*()/spin_lock*() operation.
 * We maintain the dependency maps and validate the locking attempt:
 */
static int __lock_acquire(struct lockdep_map *lock, unsigned int subclass,
			  int trylock, int read, int check, int hardirqs_off,
			  unsigned long ip)
{
	struct task_struct *curr = current;
	struct lock_class *class = NULL;
	struct held_lock *hlock;
	unsigned int depth, id;
	int chain_head = 0;
	u64 chain_key;

	if (!prove_locking)
		check = 1;

	if (unlikely(!debug_locks))
		return 0;

	if (DEBUG_LOCKS_WARN_ON(!irqs_disabled()))
		return 0;

	if (unlikely(subclass >= MAX_LOCKDEP_SUBCLASSES)) {
		debug_locks_off();
		printk("BUG: MAX_LOCKDEP_SUBCLASSES too low!\n");
		printk("turning off the locking correctness validator.\n");
		return 0;
	}

	if (!subclass)
		class = lock->class_cache;
	/*
	 * Not cached yet or subclass?
	 */
	if (unlikely(!class)) {
		class = register_lock_class(lock, subclass, 0);
		if (!class)
			return 0;
	}
	debug_atomic_inc((atomic_t *)&class->ops);
	if (very_verbose(class)) {
		printk("\nacquire class [%p] %s", class->key, class->name);
		if (class->name_version > 1)
			printk("#%d", class->name_version);
		printk("\n");
		dump_stack();
	}

	/*
	 * Add the lock to the list of currently held locks.
	 * (we dont increase the depth just yet, up until the
	 * dependency checks are done)
	 */
	depth = curr->lockdep_depth;
	if (DEBUG_LOCKS_WARN_ON(depth >= MAX_LOCK_DEPTH))
		return 0;

	hlock = curr->held_locks + depth;

	hlock->class = class;
	hlock->acquire_ip = ip;
	hlock->instance = lock;
	hlock->trylock = trylock;
	hlock->read = read;
	hlock->check = check;
	hlock->hardirqs_off = hardirqs_off;
#ifdef CONFIG_LOCK_STAT
	hlock->waittime_stamp = 0;
	hlock->holdtime_stamp = sched_clock();
#endif

	if (check == 2 && !mark_irqflags(curr, hlock))
		return 0;

	/* mark it as used: */
	if (!mark_lock(curr, hlock, LOCK_USED))
		return 0;

	/*
	 * Calculate the chain hash: it's the combined has of all the
	 * lock keys along the dependency chain. We save the hash value
	 * at every step so that we can get the current hash easily
	 * after unlock. The chain hash is then used to cache dependency
	 * results.
	 *
	 * The 'key ID' is what is the most compact key value to drive
	 * the hash, not class->key.
	 */
	id = class - lock_classes;
	if (DEBUG_LOCKS_WARN_ON(id >= MAX_LOCKDEP_KEYS))
		return 0;

	chain_key = curr->curr_chain_key;
	if (!depth) {
		if (DEBUG_LOCKS_WARN_ON(chain_key != 0))
			return 0;
		chain_head = 1;
	}

	hlock->prev_chain_key = chain_key;
	if (separate_irq_context(curr, hlock)) {
		chain_key = 0;
		chain_head = 1;
	}
	chain_key = iterate_chain_key(chain_key, id);
	curr->curr_chain_key = chain_key;

	if (!validate_chain(curr, lock, hlock, chain_head))
		return 0;

	curr->lockdep_depth++;
	check_chain_key(curr);
#ifdef CONFIG_DEBUG_LOCKDEP
	if (unlikely(!debug_locks))
		return 0;
#endif
	if (unlikely(curr->lockdep_depth >= MAX_LOCK_DEPTH)) {
		debug_locks_off();
		printk("BUG: MAX_LOCK_DEPTH too low!\n");
		printk("turning off the locking correctness validator.\n");
		return 0;
	}

	if (unlikely(curr->lockdep_depth > max_lockdep_depth))
		max_lockdep_depth = curr->lockdep_depth;

	return 1;
}

static int
print_unlock_inbalance_bug(struct task_struct *curr, struct lockdep_map *lock,
			   unsigned long ip)
{
	if (!debug_locks_off())
		return 0;
	if (debug_locks_silent)
		return 0;

	printk("\n=====================================\n");
	printk(  "[ BUG: bad unlock balance detected! ]\n");
	printk(  "-------------------------------------\n");
	printk("%s/%d is trying to release lock (",
		curr->comm, curr->pid);
	print_lockdep_cache(lock);
	printk(") at:\n");
	print_ip_sym(ip);
	printk("but there are no more locks to release!\n");
	printk("\nother info that might help us debug this:\n");
	lockdep_print_held_locks(curr);

	printk("\nstack backtrace:\n");
	dump_stack();

	return 0;
}

/*
 * Common debugging checks for both nested and non-nested unlock:
 */
static int check_unlock(struct task_struct *curr, struct lockdep_map *lock,
			unsigned long ip)
{
	if (unlikely(!debug_locks))
		return 0;
	if (DEBUG_LOCKS_WARN_ON(!irqs_disabled()))
		return 0;

	if (curr->lockdep_depth <= 0)
		return print_unlock_inbalance_bug(curr, lock, ip);

	return 1;
}

/*
 * Remove the lock to the list of currently held locks in a
 * potentially non-nested (out of order) manner. This is a
 * relatively rare operation, as all the unlock APIs default
 * to nested mode (which uses lock_release()):
 */
static int
lock_release_non_nested(struct task_struct *curr,
			struct lockdep_map *lock, unsigned long ip)
{
	struct held_lock *hlock, *prev_hlock;
	unsigned int depth;
	int i;

	/*
	 * Check whether the lock exists in the current stack
	 * of held locks:
	 */
	depth = curr->lockdep_depth;
	if (DEBUG_LOCKS_WARN_ON(!depth))
		return 0;

	prev_hlock = NULL;
	for (i = depth-1; i >= 0; i--) {
		hlock = curr->held_locks + i;
		/*
		 * We must not cross into another context:
		 */
		if (prev_hlock && prev_hlock->irq_context != hlock->irq_context)
			break;
		if (hlock->instance == lock)
			goto found_it;
		prev_hlock = hlock;
	}
	return print_unlock_inbalance_bug(curr, lock, ip);

found_it:
	lock_release_holdtime(hlock);

	/*
	 * We have the right lock to unlock, 'hlock' points to it.
	 * Now we remove it from the stack, and add back the other
	 * entries (if any), recalculating the hash along the way:
	 */
	curr->lockdep_depth = i;
	curr->curr_chain_key = hlock->prev_chain_key;

	for (i++; i < depth; i++) {
		hlock = curr->held_locks + i;
		if (!__lock_acquire(hlock->instance,
			hlock->class->subclass, hlock->trylock,
				hlock->read, hlock->check, hlock->hardirqs_off,
				hlock->acquire_ip))
			return 0;
	}

	if (DEBUG_LOCKS_WARN_ON(curr->lockdep_depth != depth - 1))
		return 0;
	return 1;
}

/*
 * Remove the lock to the list of currently held locks - this gets
 * called on mutex_unlock()/spin_unlock*() (or on a failed
 * mutex_lock_interruptible()). This is done for unlocks that nest
 * perfectly. (i.e. the current top of the lock-stack is unlocked)
 */
static int lock_release_nested(struct task_struct *curr,
			       struct lockdep_map *lock, unsigned long ip)
{
	struct held_lock *hlock;
	unsigned int depth;

	/*
	 * Pop off the top of the lock stack:
	 */
	depth = curr->lockdep_depth - 1;
	hlock = curr->held_locks + depth;

	/*
	 * Is the unlock non-nested:
	 */
	if (hlock->instance != lock)
		return lock_release_non_nested(curr, lock, ip);
	curr->lockdep_depth--;

	if (DEBUG_LOCKS_WARN_ON(!depth && (hlock->prev_chain_key != 0)))
		return 0;

	curr->curr_chain_key = hlock->prev_chain_key;

	lock_release_holdtime(hlock);

#ifdef CONFIG_DEBUG_LOCKDEP
	hlock->prev_chain_key = 0;
	hlock->class = NULL;
	hlock->acquire_ip = 0;
	hlock->irq_context = 0;
#endif
	return 1;
}

/*
 * Remove the lock to the list of currently held locks - this gets
 * called on mutex_unlock()/spin_unlock*() (or on a failed
 * mutex_lock_interruptible()). This is done for unlocks that nest
 * perfectly. (i.e. the current top of the lock-stack is unlocked)
 */
static void
__lock_release(struct lockdep_map *lock, int nested, unsigned long ip)
{
	struct task_struct *curr = current;

	if (!check_unlock(curr, lock, ip))
		return;

	if (nested) {
		if (!lock_release_nested(curr, lock, ip))
			return;
	} else {
		if (!lock_release_non_nested(curr, lock, ip))
			return;
	}

	check_chain_key(curr);
}

/*
 * Check whether we follow the irq-flags state precisely:
 */
static void check_flags(unsigned long flags)
{
#if defined(CONFIG_DEBUG_LOCKDEP) && defined(CONFIG_TRACE_IRQFLAGS)
	if (!debug_locks)
		return;

	if (irqs_disabled_flags(flags))
		DEBUG_LOCKS_WARN_ON(current->hardirqs_enabled);
	else
		DEBUG_LOCKS_WARN_ON(!current->hardirqs_enabled);

	/*
	 * We dont accurately track softirq state in e.g.
	 * hardirq contexts (such as on 4KSTACKS), so only
	 * check if not in hardirq contexts:
	 */
	if (!hardirq_count()) {
		if (softirq_count())
			DEBUG_LOCKS_WARN_ON(current->softirqs_enabled);
		else
			DEBUG_LOCKS_WARN_ON(!current->softirqs_enabled);
	}

	if (!debug_locks)
		print_irqtrace_events(current);
#endif
}

/*
 * We are not always called with irqs disabled - do that here,
 * and also avoid lockdep recursion:
 */
void lock_acquire(struct lockdep_map *lock, unsigned int subclass,
		  int trylock, int read, int check, unsigned long ip)
{
	unsigned long flags;

	if (unlikely(!lock_stat && !prove_locking))
		return;

	if (unlikely(current->lockdep_recursion))
		return;

	raw_local_irq_save(flags);
	check_flags(flags);

	current->lockdep_recursion = 1;
	__lock_acquire(lock, subclass, trylock, read, check,
		       irqs_disabled_flags(flags), ip);
	current->lockdep_recursion = 0;
	raw_local_irq_restore(flags);
}

EXPORT_SYMBOL_GPL(lock_acquire);

void lock_release(struct lockdep_map *lock, int nested, unsigned long ip)
{
	unsigned long flags;

	if (unlikely(!lock_stat && !prove_locking))
		return;

	if (unlikely(current->lockdep_recursion))
		return;

	raw_local_irq_save(flags);
	check_flags(flags);
	current->lockdep_recursion = 1;
	__lock_release(lock, nested, ip);
	current->lockdep_recursion = 0;
	raw_local_irq_restore(flags);
}

EXPORT_SYMBOL_GPL(lock_release);

#ifdef CONFIG_LOCK_STAT
static int
print_lock_contention_bug(struct task_struct *curr, struct lockdep_map *lock,
			   unsigned long ip)
{
	if (!debug_locks_off())
		return 0;
	if (debug_locks_silent)
		return 0;

	printk("\n=================================\n");
	printk(  "[ BUG: bad contention detected! ]\n");
	printk(  "---------------------------------\n");
	printk("%s/%d is trying to contend lock (",
		curr->comm, curr->pid);
	print_lockdep_cache(lock);
	printk(") at:\n");
	print_ip_sym(ip);
	printk("but there are no locks held!\n");
	printk("\nother info that might help us debug this:\n");
	lockdep_print_held_locks(curr);

	printk("\nstack backtrace:\n");
	dump_stack();

	return 0;
}

static void
__lock_contended(struct lockdep_map *lock, unsigned long ip)
{
	struct task_struct *curr = current;
	struct held_lock *hlock, *prev_hlock;
	struct lock_class_stats *stats;
	unsigned int depth;
	int i, point;

	depth = curr->lockdep_depth;
	if (DEBUG_LOCKS_WARN_ON(!depth))
		return;

	prev_hlock = NULL;
	for (i = depth-1; i >= 0; i--) {
		hlock = curr->held_locks + i;
		/*
		 * We must not cross into another context:
		 */
		if (prev_hlock && prev_hlock->irq_context != hlock->irq_context)
			break;
		if (hlock->instance == lock)
			goto found_it;
		prev_hlock = hlock;
	}
	print_lock_contention_bug(curr, lock, ip);
	return;

found_it:
	hlock->waittime_stamp = sched_clock();

	point = lock_contention_point(hlock->class, ip);

	stats = get_lock_stats(hlock->class);
	if (point < ARRAY_SIZE(stats->contention_point))
		stats->contention_point[i]++;
	if (lock->cpu != smp_processor_id())
		stats->bounces[bounce_contended + !!hlock->read]++;
	put_lock_stats(stats);
}

static void
__lock_acquired(struct lockdep_map *lock)
{
	struct task_struct *curr = current;
	struct held_lock *hlock, *prev_hlock;
	struct lock_class_stats *stats;
	unsigned int depth;
	u64 now;
	s64 waittime = 0;
	int i, cpu;

	depth = curr->lockdep_depth;
	if (DEBUG_LOCKS_WARN_ON(!depth))
		return;

	prev_hlock = NULL;
	for (i = depth-1; i >= 0; i--) {
		hlock = curr->held_locks + i;
		/*
		 * We must not cross into another context:
		 */
		if (prev_hlock && prev_hlock->irq_context != hlock->irq_context)
			break;
		if (hlock->instance == lock)
			goto found_it;
		prev_hlock = hlock;
	}
	print_lock_contention_bug(curr, lock, _RET_IP_);
	return;

found_it:
	cpu = smp_processor_id();
	if (hlock->waittime_stamp) {
		now = sched_clock();
		waittime = now - hlock->waittime_stamp;
		hlock->holdtime_stamp = now;
	}

	stats = get_lock_stats(hlock->class);
	if (waittime) {
		if (hlock->read)
			lock_time_inc(&stats->read_waittime, waittime);
		else
			lock_time_inc(&stats->write_waittime, waittime);
	}
	if (lock->cpu != cpu)
		stats->bounces[bounce_acquired + !!hlock->read]++;
	put_lock_stats(stats);

	lock->cpu = cpu;
}

void lock_contended(struct lockdep_map *lock, unsigned long ip)
{
	unsigned long flags;

	if (unlikely(!lock_stat))
		return;

	if (unlikely(current->lockdep_recursion))
		return;

	raw_local_irq_save(flags);
	check_flags(flags);
	current->lockdep_recursion = 1;
	__lock_contended(lock, ip);
	current->lockdep_recursion = 0;
	raw_local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(lock_contended);

void lock_acquired(struct lockdep_map *lock)
{
	unsigned long flags;

	if (unlikely(!lock_stat))
		return;

	if (unlikely(current->lockdep_recursion))
		return;

	raw_local_irq_save(flags);
	check_flags(flags);
	current->lockdep_recursion = 1;
	__lock_acquired(lock);
	current->lockdep_recursion = 0;
	raw_local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(lock_acquired);
#endif

/*
 * Used by the testsuite, sanitize the validator state
 * after a simulated failure:
 */

void lockdep_reset(void)
{
	unsigned long flags;
	int i;

	raw_local_irq_save(flags);
	current->curr_chain_key = 0;
	current->lockdep_depth = 0;
	current->lockdep_recursion = 0;
	memset(current->held_locks, 0, MAX_LOCK_DEPTH*sizeof(struct held_lock));
	nr_hardirq_chains = 0;
	nr_softirq_chains = 0;
	nr_process_chains = 0;
	debug_locks = 1;
	for (i = 0; i < CHAINHASH_SIZE; i++)
		INIT_LIST_HEAD(chainhash_table + i);
	raw_local_irq_restore(flags);
}

static void zap_class(struct lock_class *class)
{
	int i;

	/*
	 * Remove all dependencies this lock is
	 * involved in:
	 */
	for (i = 0; i < nr_list_entries; i++) {
		if (list_entries[i].class == class)
			list_del_rcu(&list_entries[i].entry);
	}
	/*
	 * Unhash the class and remove it from the all_lock_classes list:
	 */
	list_del_rcu(&class->hash_entry);
	list_del_rcu(&class->lock_entry);

}

static inline int within(void *addr, void *start, unsigned long size)
{
	return addr >= start && addr < start + size;
}

void lockdep_free_key_range(void *start, unsigned long size)
{
	struct lock_class *class, *next;
	struct list_head *head;
	unsigned long flags;
	int i;

	raw_local_irq_save(flags);
	graph_lock();

	/*
	 * Unhash all classes that were created by this module:
	 */
	for (i = 0; i < CLASSHASH_SIZE; i++) {
		head = classhash_table + i;
		if (list_empty(head))
			continue;
		list_for_each_entry_safe(class, next, head, hash_entry)
			if (within(class->key, start, size))
				zap_class(class);
	}

	graph_unlock();
	raw_local_irq_restore(flags);
}

void lockdep_reset_lock(struct lockdep_map *lock)
{
	struct lock_class *class, *next;
	struct list_head *head;
	unsigned long flags;
	int i, j;

	raw_local_irq_save(flags);

	/*
	 * Remove all classes this lock might have:
	 */
	for (j = 0; j < MAX_LOCKDEP_SUBCLASSES; j++) {
		/*
		 * If the class exists we look it up and zap it:
		 */
		class = look_up_lock_class(lock, j);
		if (class)
			zap_class(class);
	}
	/*
	 * Debug check: in the end all mapped classes should
	 * be gone.
	 */
	graph_lock();
	for (i = 0; i < CLASSHASH_SIZE; i++) {
		head = classhash_table + i;
		if (list_empty(head))
			continue;
		list_for_each_entry_safe(class, next, head, hash_entry) {
			if (unlikely(class == lock->class_cache)) {
				if (debug_locks_off_graph_unlock())
					WARN_ON(1);
				goto out_restore;
			}
		}
	}
	graph_unlock();

out_restore:
	raw_local_irq_restore(flags);
}

void lockdep_init(void)
{
	int i;

	/*
	 * Some architectures have their own start_kernel()
	 * code which calls lockdep_init(), while we also
	 * call lockdep_init() from the start_kernel() itself,
	 * and we want to initialize the hashes only once:
	 */
	if (lockdep_initialized)
		return;

	for (i = 0; i < CLASSHASH_SIZE; i++)
		INIT_LIST_HEAD(classhash_table + i);

	for (i = 0; i < CHAINHASH_SIZE; i++)
		INIT_LIST_HEAD(chainhash_table + i);

	lockdep_initialized = 1;
}

void __init lockdep_info(void)
{
	printk("Lock dependency validator: Copyright (c) 2006 Red Hat, Inc., Ingo Molnar\n");

	printk("... MAX_LOCKDEP_SUBCLASSES:    %lu\n", MAX_LOCKDEP_SUBCLASSES);
	printk("... MAX_LOCK_DEPTH:          %lu\n", MAX_LOCK_DEPTH);
	printk("... MAX_LOCKDEP_KEYS:        %lu\n", MAX_LOCKDEP_KEYS);
	printk("... CLASSHASH_SIZE:           %lu\n", CLASSHASH_SIZE);
	printk("... MAX_LOCKDEP_ENTRIES:     %lu\n", MAX_LOCKDEP_ENTRIES);
	printk("... MAX_LOCKDEP_CHAINS:      %lu\n", MAX_LOCKDEP_CHAINS);
	printk("... CHAINHASH_SIZE:          %lu\n", CHAINHASH_SIZE);

	printk(" memory used by lock dependency info: %lu kB\n",
		(sizeof(struct lock_class) * MAX_LOCKDEP_KEYS +
		sizeof(struct list_head) * CLASSHASH_SIZE +
		sizeof(struct lock_list) * MAX_LOCKDEP_ENTRIES +
		sizeof(struct lock_chain) * MAX_LOCKDEP_CHAINS +
		sizeof(struct list_head) * CHAINHASH_SIZE) / 1024);

	printk(" per task-struct memory footprint: %lu bytes\n",
		sizeof(struct held_lock) * MAX_LOCK_DEPTH);

#ifdef CONFIG_DEBUG_LOCKDEP
	if (lockdep_init_error) {
		printk("WARNING: lockdep init error! Arch code didn't call lockdep_init() early enough?\n");
		printk("Call stack leading to lockdep invocation was:\n");
		print_stack_trace(&lockdep_init_trace, 0);
	}
#endif
}

static inline int in_range(const void *start, const void *addr, const void *end)
{
	return addr >= start && addr <= end;
}

static void
print_freed_lock_bug(struct task_struct *curr, const void *mem_from,
		     const void *mem_to, struct held_lock *hlock)
{
	if (!debug_locks_off())
		return;
	if (debug_locks_silent)
		return;

	printk("\n=========================\n");
	printk(  "[ BUG: held lock freed! ]\n");
	printk(  "-------------------------\n");
	printk("%s/%d is freeing memory %p-%p, with a lock still held there!\n",
		curr->comm, curr->pid, mem_from, mem_to-1);
	print_lock(hlock);
	lockdep_print_held_locks(curr);

	printk("\nstack backtrace:\n");
	dump_stack();
}

/*
 * Called when kernel memory is freed (or unmapped), or if a lock
 * is destroyed or reinitialized - this code checks whether there is
 * any held lock in the memory range of <from> to <to>:
 */
void debug_check_no_locks_freed(const void *mem_from, unsigned long mem_len)
{
	const void *mem_to = mem_from + mem_len, *lock_from, *lock_to;
	struct task_struct *curr = current;
	struct held_lock *hlock;
	unsigned long flags;
	int i;

	if (unlikely(!debug_locks))
		return;

	local_irq_save(flags);
	for (i = 0; i < curr->lockdep_depth; i++) {
		hlock = curr->held_locks + i;

		lock_from = (void *)hlock->instance;
		lock_to = (void *)(hlock->instance + 1);

		if (!in_range(mem_from, lock_from, mem_to) &&
					!in_range(mem_from, lock_to, mem_to))
			continue;

		print_freed_lock_bug(curr, mem_from, mem_to, hlock);
		break;
	}
	local_irq_restore(flags);
}
EXPORT_SYMBOL_GPL(debug_check_no_locks_freed);

static void print_held_locks_bug(struct task_struct *curr)
{
	if (!debug_locks_off())
		return;
	if (debug_locks_silent)
		return;

	printk("\n=====================================\n");
	printk(  "[ BUG: lock held at task exit time! ]\n");
	printk(  "-------------------------------------\n");
	printk("%s/%d is exiting with locks still held!\n",
		curr->comm, curr->pid);
	lockdep_print_held_locks(curr);

	printk("\nstack backtrace:\n");
	dump_stack();
}

void debug_check_no_locks_held(struct task_struct *task)
{
	if (unlikely(task->lockdep_depth > 0))
		print_held_locks_bug(task);
}

void debug_show_all_locks(void)
{
	struct task_struct *g, *p;
	int count = 10;
	int unlock = 1;

	if (unlikely(!debug_locks)) {
		printk("INFO: lockdep is turned off.\n");
		return;
	}
	printk("\nShowing all locks held in the system:\n");

	/*
	 * Here we try to get the tasklist_lock as hard as possible,
	 * if not successful after 2 seconds we ignore it (but keep
	 * trying). This is to enable a debug printout even if a
	 * tasklist_lock-holding task deadlocks or crashes.
	 */
retry:
	if (!read_trylock(&tasklist_lock)) {
		if (count == 10)
			printk("hm, tasklist_lock locked, retrying... ");
		if (count) {
			count--;
			printk(" #%d", 10-count);
			mdelay(200);
			goto retry;
		}
		printk(" ignoring it.\n");
		unlock = 0;
	}
	if (count != 10)
		printk(" locked it.\n");

	do_each_thread(g, p) {
		if (p->lockdep_depth)
			lockdep_print_held_locks(p);
		if (!unlock)
			if (read_trylock(&tasklist_lock))
				unlock = 1;
	} while_each_thread(g, p);

	printk("\n");
	printk("=============================================\n\n");

	if (unlock)
		read_unlock(&tasklist_lock);
}

EXPORT_SYMBOL_GPL(debug_show_all_locks);

void debug_show_held_locks(struct task_struct *task)
{
	if (unlikely(!debug_locks)) {
		printk("INFO: lockdep is turned off.\n");
		return;
	}
	lockdep_print_held_locks(task);
}

EXPORT_SYMBOL_GPL(debug_show_held_locks);
