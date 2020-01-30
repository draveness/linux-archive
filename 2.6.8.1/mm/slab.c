/*
 * linux/mm/slab.c
 * Written by Mark Hemment, 1996/97.
 * (markhe@nextd.demon.co.uk)
 *
 * kmem_cache_destroy() + some cleanup - 1999 Andrea Arcangeli
 *
 * Major cleanup, different bufctl logic, per-cpu arrays
 *	(c) 2000 Manfred Spraul
 *
 * Cleanup, make the head arrays unconditional, preparation for NUMA
 * 	(c) 2002 Manfred Spraul
 *
 * An implementation of the Slab Allocator as described in outline in;
 *	UNIX Internals: The New Frontiers by Uresh Vahalia
 *	Pub: Prentice Hall	ISBN 0-13-101908-2
 * or with a little more detail in;
 *	The Slab Allocator: An Object-Caching Kernel Memory Allocator
 *	Jeff Bonwick (Sun Microsystems).
 *	Presented at: USENIX Summer 1994 Technical Conference
 *
 * The memory is organized in caches, one cache for each object type.
 * (e.g. inode_cache, dentry_cache, buffer_head, vm_area_struct)
 * Each cache consists out of many slabs (they are small (usually one
 * page long) and always contiguous), and each slab contains multiple
 * initialized objects.
 *
 * This means, that your constructor is used only for newly allocated
 * slabs and you must pass objects with the same intializations to
 * kmem_cache_free.
 *
 * Each cache can only support one memory type (GFP_DMA, GFP_HIGHMEM,
 * normal). If you need a special memory type, then must create a new
 * cache for that memory type.
 *
 * In order to reduce fragmentation, the slabs are sorted in 3 groups:
 *   full slabs with 0 free objects
 *   partial slabs
 *   empty slabs with no allocated objects
 *
 * If partial slabs exist, then new allocations come from these slabs,
 * otherwise from empty slabs or new slabs are allocated.
 *
 * kmem_cache_destroy() CAN CRASH if you try to allocate from the cache
 * during kmem_cache_destroy(). The caller must prevent concurrent allocs.
 *
 * Each cache has a short per-cpu head array, most allocs
 * and frees go into that array, and if that array overflows, then 1/2
 * of the entries in the array are given back into the global cache.
 * The head array is strictly LIFO and should improve the cache hit rates.
 * On SMP, it additionally reduces the spinlock operations.
 *
 * The c_cpuarray may not be read with enabled local interrupts - 
 * it's changed with a smp_call_function().
 *
 * SMP synchronization:
 *  constructors and destructors are called without any locking.
 *  Several members in kmem_cache_t and struct slab never change, they
 *	are accessed without any locking.
 *  The per-cpu arrays are never accessed from the wrong cpu, no locking,
 *  	and local interrupts are disabled so slab code is preempt-safe.
 *  The non-constant members are protected with a per-cache irq spinlock.
 *
 * Many thanks to Mark Hemment, who wrote another per-cpu slab patch
 * in 2000 - many ideas in the current implementation are derived from
 * his patch.
 *
 * Further notes from the original documentation:
 *
 * 11 April '97.  Started multi-threading - markhe
 *	The global cache-chain is protected by the semaphore 'cache_chain_sem'.
 *	The sem is only needed when accessing/extending the cache-chain, which
 *	can never happen inside an interrupt (kmem_cache_create(),
 *	kmem_cache_shrink() and kmem_cache_reap()).
 *
 *	At present, each engine can be growing a cache.  This should be blocked.
 *
 */

#include	<linux/config.h>
#include	<linux/slab.h>
#include	<linux/mm.h>
#include	<linux/swap.h>
#include	<linux/cache.h>
#include	<linux/interrupt.h>
#include	<linux/init.h>
#include	<linux/compiler.h>
#include	<linux/seq_file.h>
#include	<linux/notifier.h>
#include	<linux/kallsyms.h>
#include	<linux/cpu.h>
#include	<linux/sysctl.h>
#include	<linux/module.h>

#include	<asm/uaccess.h>
#include	<asm/cacheflush.h>
#include	<asm/tlbflush.h>

/*
 * DEBUG	- 1 for kmem_cache_create() to honour; SLAB_DEBUG_INITIAL,
 *		  SLAB_RED_ZONE & SLAB_POISON.
 *		  0 for faster, smaller code (especially in the critical paths).
 *
 * STATS	- 1 to collect stats for /proc/slabinfo.
 *		  0 for faster, smaller code (especially in the critical paths).
 *
 * FORCED_DEBUG	- 1 enables SLAB_RED_ZONE and SLAB_POISON (if possible)
 */

#ifdef CONFIG_DEBUG_SLAB
#define	DEBUG		1
#define	STATS		1
#define	FORCED_DEBUG	1
#else
#define	DEBUG		0
#define	STATS		0
#define	FORCED_DEBUG	0
#endif


/* Shouldn't this be in a header file somewhere? */
#define	BYTES_PER_WORD		sizeof(void *)

#ifndef cache_line_size
#define cache_line_size()	L1_CACHE_BYTES
#endif

#ifndef ARCH_KMALLOC_MINALIGN
#define ARCH_KMALLOC_MINALIGN 0
#endif

#ifndef ARCH_KMALLOC_FLAGS
#define ARCH_KMALLOC_FLAGS SLAB_HWCACHE_ALIGN
#endif

/* Legal flag mask for kmem_cache_create(). */
#if DEBUG
# define CREATE_MASK	(SLAB_DEBUG_INITIAL | SLAB_RED_ZONE | \
			 SLAB_POISON | SLAB_HWCACHE_ALIGN | \
			 SLAB_NO_REAP | SLAB_CACHE_DMA | \
			 SLAB_MUST_HWCACHE_ALIGN | SLAB_STORE_USER | \
			 SLAB_RECLAIM_ACCOUNT | SLAB_PANIC)
#else
# define CREATE_MASK	(SLAB_HWCACHE_ALIGN | SLAB_NO_REAP | \
			 SLAB_CACHE_DMA | SLAB_MUST_HWCACHE_ALIGN | \
			 SLAB_RECLAIM_ACCOUNT | SLAB_PANIC)
#endif

/*
 * kmem_bufctl_t:
 *
 * Bufctl's are used for linking objs within a slab
 * linked offsets.
 *
 * This implementation relies on "struct page" for locating the cache &
 * slab an object belongs to.
 * This allows the bufctl structure to be small (one int), but limits
 * the number of objects a slab (not a cache) can contain when off-slab
 * bufctls are used. The limit is the size of the largest general cache
 * that does not use off-slab slabs.
 * For 32bit archs with 4 kB pages, is this 56.
 * This is not serious, as it is only for large objects, when it is unwise
 * to have too many per slab.
 * Note: This limit can be raised by introducing a general cache whose size
 * is less than 512 (PAGE_SIZE<<3), but greater than 256.
 */

#define BUFCTL_END	(((kmem_bufctl_t)(~0U))-0)
#define BUFCTL_FREE	(((kmem_bufctl_t)(~0U))-1)
#define	SLAB_LIMIT	(((kmem_bufctl_t)(~0U))-2)

/* Max number of objs-per-slab for caches which use off-slab slabs.
 * Needed to avoid a possible looping condition in cache_grow().
 */
static unsigned long offslab_limit;

/*
 * struct slab
 *
 * Manages the objs in a slab. Placed either at the beginning of mem allocated
 * for a slab, or allocated from an general cache.
 * Slabs are chained into three list: fully used, partial, fully free slabs.
 */
struct slab {
	struct list_head	list;
	unsigned long		colouroff;
	void			*s_mem;		/* including colour offset */
	unsigned int		inuse;		/* num of objs active in slab */
	kmem_bufctl_t		free;
};

/*
 * struct array_cache
 *
 * Per cpu structures
 * Purpose:
 * - LIFO ordering, to hand out cache-warm objects from _alloc
 * - reduce the number of linked list operations
 * - reduce spinlock operations
 *
 * The limit is stored in the per-cpu structure to reduce the data cache
 * footprint.
 *
 */
struct array_cache {
	unsigned int avail;
	unsigned int limit;
	unsigned int batchcount;
	unsigned int touched;
};

/* bootstrap: The caches do not work without cpuarrays anymore,
 * but the cpuarrays are allocated from the generic caches...
 */
#define BOOT_CPUCACHE_ENTRIES	1
struct arraycache_init {
	struct array_cache cache;
	void * entries[BOOT_CPUCACHE_ENTRIES];
};

/*
 * The slab lists of all objects.
 * Hopefully reduce the internal fragmentation
 * NUMA: The spinlock could be moved from the kmem_cache_t
 * into this structure, too. Figure out what causes
 * fewer cross-node spinlock operations.
 */
struct kmem_list3 {
	struct list_head	slabs_partial;	/* partial list first, better asm code */
	struct list_head	slabs_full;
	struct list_head	slabs_free;
	unsigned long	free_objects;
	int		free_touched;
	unsigned long	next_reap;
	struct array_cache	*shared;
};

#define LIST3_INIT(parent) \
	{ \
		.slabs_full	= LIST_HEAD_INIT(parent.slabs_full), \
		.slabs_partial	= LIST_HEAD_INIT(parent.slabs_partial), \
		.slabs_free	= LIST_HEAD_INIT(parent.slabs_free) \
	}
#define list3_data(cachep) \
	(&(cachep)->lists)

/* NUMA: per-node */
#define list3_data_ptr(cachep, ptr) \
		list3_data(cachep)

/*
 * kmem_cache_t
 *
 * manages a cache.
 */
	
struct kmem_cache_s {
/* 1) per-cpu data, touched during every alloc/free */
	struct array_cache	*array[NR_CPUS];
	unsigned int		batchcount;
	unsigned int		limit;
/* 2) touched by every alloc & free from the backend */
	struct kmem_list3	lists;
	/* NUMA: kmem_3list_t	*nodelists[MAX_NUMNODES] */
	unsigned int		objsize;
	unsigned int	 	flags;	/* constant flags */
	unsigned int		num;	/* # of objs per slab */
	unsigned int		free_limit; /* upper limit of objects in the lists */
	spinlock_t		spinlock;

/* 3) cache_grow/shrink */
	/* order of pgs per slab (2^n) */
	unsigned int		gfporder;

	/* force GFP flags, e.g. GFP_DMA */
	unsigned int		gfpflags;

	size_t			colour;		/* cache colouring range */
	unsigned int		colour_off;	/* colour offset */
	unsigned int		colour_next;	/* cache colouring */
	kmem_cache_t		*slabp_cache;
	unsigned int		slab_size;
	unsigned int		dflags;		/* dynamic flags */

	/* constructor func */
	void (*ctor)(void *, kmem_cache_t *, unsigned long);

	/* de-constructor func */
	void (*dtor)(void *, kmem_cache_t *, unsigned long);

/* 4) cache creation/removal */
	const char		*name;
	struct list_head	next;

/* 5) statistics */
#if STATS
	unsigned long		num_active;
	unsigned long		num_allocations;
	unsigned long		high_mark;
	unsigned long		grown;
	unsigned long		reaped;
	unsigned long 		errors;
	unsigned long		max_freeable;
	atomic_t		allochit;
	atomic_t		allocmiss;
	atomic_t		freehit;
	atomic_t		freemiss;
#endif
#if DEBUG
	int			dbghead;
	int			reallen;
#endif
};

#define CFLGS_OFF_SLAB		(0x80000000UL)
#define	OFF_SLAB(x)	((x)->flags & CFLGS_OFF_SLAB)

#define BATCHREFILL_LIMIT	16
/* Optimization question: fewer reaps means less 
 * probability for unnessary cpucache drain/refill cycles.
 *
 * OTHO the cpuarrays can contain lots of objects,
 * which could lock up otherwise freeable slabs.
 */
#define REAPTIMEOUT_CPUC	(2*HZ)
#define REAPTIMEOUT_LIST3	(4*HZ)

#if STATS
#define	STATS_INC_ACTIVE(x)	((x)->num_active++)
#define	STATS_DEC_ACTIVE(x)	((x)->num_active--)
#define	STATS_INC_ALLOCED(x)	((x)->num_allocations++)
#define	STATS_INC_GROWN(x)	((x)->grown++)
#define	STATS_INC_REAPED(x)	((x)->reaped++)
#define	STATS_SET_HIGH(x)	do { if ((x)->num_active > (x)->high_mark) \
					(x)->high_mark = (x)->num_active; \
				} while (0)
#define	STATS_INC_ERR(x)	((x)->errors++)
#define	STATS_SET_FREEABLE(x, i) \
				do { if ((x)->max_freeable < i) \
					(x)->max_freeable = i; \
				} while (0)

#define STATS_INC_ALLOCHIT(x)	atomic_inc(&(x)->allochit)
#define STATS_INC_ALLOCMISS(x)	atomic_inc(&(x)->allocmiss)
#define STATS_INC_FREEHIT(x)	atomic_inc(&(x)->freehit)
#define STATS_INC_FREEMISS(x)	atomic_inc(&(x)->freemiss)
#else
#define	STATS_INC_ACTIVE(x)	do { } while (0)
#define	STATS_DEC_ACTIVE(x)	do { } while (0)
#define	STATS_INC_ALLOCED(x)	do { } while (0)
#define	STATS_INC_GROWN(x)	do { } while (0)
#define	STATS_INC_REAPED(x)	do { } while (0)
#define	STATS_SET_HIGH(x)	do { } while (0)
#define	STATS_INC_ERR(x)	do { } while (0)
#define	STATS_SET_FREEABLE(x, i) \
				do { } while (0)

#define STATS_INC_ALLOCHIT(x)	do { } while (0)
#define STATS_INC_ALLOCMISS(x)	do { } while (0)
#define STATS_INC_FREEHIT(x)	do { } while (0)
#define STATS_INC_FREEMISS(x)	do { } while (0)
#endif

#if DEBUG
/* Magic nums for obj red zoning.
 * Placed in the first word before and the first word after an obj.
 */
#define	RED_INACTIVE	0x5A2CF071UL	/* when obj is inactive */
#define	RED_ACTIVE	0x170FC2A5UL	/* when obj is active */

/* ...and for poisoning */
#define	POISON_INUSE	0x5a	/* for use-uninitialised poisoning */
#define POISON_FREE	0x6b	/* for use-after-free poisoning */
#define	POISON_END	0xa5	/* end-byte of poisoning */

/* memory layout of objects:
 * 0		: objp
 * 0 .. cachep->dbghead - BYTES_PER_WORD - 1: padding. This ensures that
 * 		the end of an object is aligned with the end of the real
 * 		allocation. Catches writes behind the end of the allocation.
 * cachep->dbghead - BYTES_PER_WORD .. cachep->dbghead - 1:
 * 		redzone word.
 * cachep->dbghead: The real object.
 * cachep->objsize - 2* BYTES_PER_WORD: redzone word [BYTES_PER_WORD long]
 * cachep->objsize - 1* BYTES_PER_WORD: last caller address [BYTES_PER_WORD long]
 */
static int obj_dbghead(kmem_cache_t *cachep)
{
	return cachep->dbghead;
}

static int obj_reallen(kmem_cache_t *cachep)
{
	return cachep->reallen;
}

static unsigned long *dbg_redzone1(kmem_cache_t *cachep, void *objp)
{
	BUG_ON(!(cachep->flags & SLAB_RED_ZONE));
	return (unsigned long*) (objp+obj_dbghead(cachep)-BYTES_PER_WORD);
}

static unsigned long *dbg_redzone2(kmem_cache_t *cachep, void *objp)
{
	BUG_ON(!(cachep->flags & SLAB_RED_ZONE));
	if (cachep->flags & SLAB_STORE_USER)
		return (unsigned long*) (objp+cachep->objsize-2*BYTES_PER_WORD);
	return (unsigned long*) (objp+cachep->objsize-BYTES_PER_WORD);
}

static void **dbg_userword(kmem_cache_t *cachep, void *objp)
{
	BUG_ON(!(cachep->flags & SLAB_STORE_USER));
	return (void**)(objp+cachep->objsize-BYTES_PER_WORD);
}

#else

#define obj_dbghead(x)			0
#define obj_reallen(cachep)		(cachep->objsize)
#define dbg_redzone1(cachep, objp)	({BUG(); (unsigned long *)NULL;})
#define dbg_redzone2(cachep, objp)	({BUG(); (unsigned long *)NULL;})
#define dbg_userword(cachep, objp)	({BUG(); (void **)NULL;})

#endif

/*
 * Maximum size of an obj (in 2^order pages)
 * and absolute limit for the gfp order.
 */
#if defined(CONFIG_LARGE_ALLOCS)
#define	MAX_OBJ_ORDER	13	/* up to 32Mb */
#define	MAX_GFP_ORDER	13	/* up to 32Mb */
#elif defined(CONFIG_MMU)
#define	MAX_OBJ_ORDER	5	/* 32 pages */
#define	MAX_GFP_ORDER	5	/* 32 pages */
#else
#define	MAX_OBJ_ORDER	8	/* up to 1Mb */
#define	MAX_GFP_ORDER	8	/* up to 1Mb */
#endif

/*
 * Do not go above this order unless 0 objects fit into the slab.
 */
#define	BREAK_GFP_ORDER_HI	1
#define	BREAK_GFP_ORDER_LO	0
static int slab_break_gfp_order = BREAK_GFP_ORDER_LO;

/* Macros for storing/retrieving the cachep and or slab from the
 * global 'mem_map'. These are used to find the slab an obj belongs to.
 * With kfree(), these are used to find the cache which an obj belongs to.
 */
#define	SET_PAGE_CACHE(pg,x)  ((pg)->lru.next = (struct list_head *)(x))
#define	GET_PAGE_CACHE(pg)    ((kmem_cache_t *)(pg)->lru.next)
#define	SET_PAGE_SLAB(pg,x)   ((pg)->lru.prev = (struct list_head *)(x))
#define	GET_PAGE_SLAB(pg)     ((struct slab *)(pg)->lru.prev)

/* These are the default caches for kmalloc. Custom caches can have other sizes. */
struct cache_sizes malloc_sizes[] = {
#define CACHE(x) { .cs_size = (x) },
#include <linux/kmalloc_sizes.h>
	{ 0, }
#undef CACHE
};

EXPORT_SYMBOL(malloc_sizes);

/* Must match cache_sizes above. Out of line to keep cache footprint low. */
struct cache_names {
	char *name;
	char *name_dma;
};

static struct cache_names __initdata cache_names[] = {
#define CACHE(x) { .name = "size-" #x, .name_dma = "size-" #x "(DMA)" },
#include <linux/kmalloc_sizes.h>
	{ NULL, }
#undef CACHE
};

struct arraycache_init initarray_cache __initdata = { { 0, BOOT_CPUCACHE_ENTRIES, 1, 0} };
struct arraycache_init initarray_generic __initdata = { { 0, BOOT_CPUCACHE_ENTRIES, 1, 0} };

/* internal cache of cache description objs */
static kmem_cache_t cache_cache = {
	.lists		= LIST3_INIT(cache_cache.lists),
	.batchcount	= 1,
	.limit		= BOOT_CPUCACHE_ENTRIES,
	.objsize	= sizeof(kmem_cache_t),
	.flags		= SLAB_NO_REAP,
	.spinlock	= SPIN_LOCK_UNLOCKED,
	.name		= "kmem_cache",
#if DEBUG
	.reallen	= sizeof(kmem_cache_t),
#endif
};

/* Guard access to the cache-chain. */
static struct semaphore	cache_chain_sem;

struct list_head cache_chain;

/*
 * vm_enough_memory() looks at this to determine how many
 * slab-allocated pages are possibly freeable under pressure
 *
 * SLAB_RECLAIM_ACCOUNT turns this on per-slab
 */
atomic_t slab_reclaim_pages;
EXPORT_SYMBOL(slab_reclaim_pages);

/*
 * chicken and egg problem: delay the per-cpu array allocation
 * until the general caches are up.
 */
enum {
	NONE,
	PARTIAL,
	FULL
} g_cpucache_up;

static DEFINE_PER_CPU(struct timer_list, reap_timers);

static void reap_timer_fnc(unsigned long data);
static void free_block(kmem_cache_t* cachep, void** objpp, int len);
static void enable_cpucache (kmem_cache_t *cachep);

static inline void ** ac_entry(struct array_cache *ac)
{
	return (void**)(ac+1);
}

static inline struct array_cache *ac_data(kmem_cache_t *cachep)
{
	return cachep->array[smp_processor_id()];
}

/* Cal the num objs, wastage, and bytes left over for a given slab size. */
static void cache_estimate (unsigned long gfporder, size_t size, size_t align,
		 int flags, size_t *left_over, unsigned int *num)
{
	int i;
	size_t wastage = PAGE_SIZE<<gfporder;
	size_t extra = 0;
	size_t base = 0;

	if (!(flags & CFLGS_OFF_SLAB)) {
		base = sizeof(struct slab);
		extra = sizeof(kmem_bufctl_t);
	}
	i = 0;
	while (i*size + ALIGN(base+i*extra, align) <= wastage)
		i++;
	if (i > 0)
		i--;

	if (i > SLAB_LIMIT)
		i = SLAB_LIMIT;

	*num = i;
	wastage -= i*size;
	wastage -= ALIGN(base+i*extra, align);
	*left_over = wastage;
}

#define slab_error(cachep, msg) __slab_error(__FUNCTION__, cachep, msg)

static void __slab_error(const char *function, kmem_cache_t *cachep, char *msg)
{
	printk(KERN_ERR "slab error in %s(): cache `%s': %s\n",
		function, cachep->name, msg);
	dump_stack();
}

/*
 * Start the reap timer running on the target CPU.  We run at around 1 to 2Hz.
 * Add the CPU number into the expiry time to minimize the possibility of the
 * CPUs getting into lockstep and contending for the global cache chain lock.
 */
static void __devinit start_cpu_timer(int cpu)
{
	struct timer_list *rt = &per_cpu(reap_timers, cpu);

	if (rt->function == NULL) {
		init_timer(rt);
		rt->expires = jiffies + HZ + 3*cpu;
		rt->data = cpu;
		rt->function = reap_timer_fnc;
		add_timer_on(rt, cpu);
	}
}

#ifdef CONFIG_HOTPLUG_CPU
static void stop_cpu_timer(int cpu)
{
	struct timer_list *rt = &per_cpu(reap_timers, cpu);

	if (rt->function) {
		del_timer_sync(rt);
		WARN_ON(timer_pending(rt));
		rt->function = NULL;
	}
}
#endif

static struct array_cache *alloc_arraycache(int cpu, int entries, int batchcount)
{
	int memsize = sizeof(void*)*entries+sizeof(struct array_cache);
	struct array_cache *nc = NULL;

	if (cpu != -1) {
		nc = kmem_cache_alloc_node(kmem_find_general_cachep(memsize,
					GFP_KERNEL), cpu_to_node(cpu));
	}
	if (!nc)
		nc = kmalloc(memsize, GFP_KERNEL);
	if (nc) {
		nc->avail = 0;
		nc->limit = entries;
		nc->batchcount = batchcount;
		nc->touched = 0;
	}
	return nc;
}

static int __devinit cpuup_callback(struct notifier_block *nfb,
				  unsigned long action,
				  void *hcpu)
{
	long cpu = (long)hcpu;
	kmem_cache_t* cachep;

	switch (action) {
	case CPU_UP_PREPARE:
		down(&cache_chain_sem);
		list_for_each_entry(cachep, &cache_chain, next) {
			struct array_cache *nc;

			nc = alloc_arraycache(cpu, cachep->limit, cachep->batchcount);
			if (!nc)
				goto bad;

			spin_lock_irq(&cachep->spinlock);
			cachep->array[cpu] = nc;
			cachep->free_limit = (1+num_online_cpus())*cachep->batchcount
						+ cachep->num;
			spin_unlock_irq(&cachep->spinlock);

		}
		up(&cache_chain_sem);
		break;
	case CPU_ONLINE:
		start_cpu_timer(cpu);
		break;
#ifdef CONFIG_HOTPLUG_CPU
	case CPU_DEAD:
		stop_cpu_timer(cpu);
		/* fall thru */
	case CPU_UP_CANCELED:
		down(&cache_chain_sem);

		list_for_each_entry(cachep, &cache_chain, next) {
			struct array_cache *nc;

			spin_lock_irq(&cachep->spinlock);
			/* cpu is dead; no one can alloc from it. */
			nc = cachep->array[cpu];
			cachep->array[cpu] = NULL;
			cachep->free_limit -= cachep->batchcount;
			free_block(cachep, ac_entry(nc), nc->avail);
			spin_unlock_irq(&cachep->spinlock);
			kfree(nc);
		}
		up(&cache_chain_sem);
		break;
#endif
	}
	return NOTIFY_OK;
bad:
	up(&cache_chain_sem);
	return NOTIFY_BAD;
}

static struct notifier_block cpucache_notifier = { &cpuup_callback, NULL, 0 };

/* Initialisation.
 * Called after the gfp() functions have been enabled, and before smp_init().
 */
void __init kmem_cache_init(void)
{
	size_t left_over;
	struct cache_sizes *sizes;
	struct cache_names *names;

	/*
	 * Fragmentation resistance on low memory - only use bigger
	 * page orders on machines with more than 32MB of memory.
	 */
	if (num_physpages > (32 << 20) >> PAGE_SHIFT)
		slab_break_gfp_order = BREAK_GFP_ORDER_HI;

	
	/* Bootstrap is tricky, because several objects are allocated
	 * from caches that do not exist yet:
	 * 1) initialize the cache_cache cache: it contains the kmem_cache_t
	 *    structures of all caches, except cache_cache itself: cache_cache
	 *    is statically allocated.
	 *    Initially an __init data area is used for the head array, it's
	 *    replaced with a kmalloc allocated array at the end of the bootstrap.
	 * 2) Create the first kmalloc cache.
	 *    The kmem_cache_t for the new cache is allocated normally. An __init
	 *    data area is used for the head array.
	 * 3) Create the remaining kmalloc caches, with minimally sized head arrays.
	 * 4) Replace the __init data head arrays for cache_cache and the first
	 *    kmalloc cache with kmalloc allocated arrays.
	 * 5) Resize the head arrays of the kmalloc caches to their final sizes.
	 */

	/* 1) create the cache_cache */
	init_MUTEX(&cache_chain_sem);
	INIT_LIST_HEAD(&cache_chain);
	list_add(&cache_cache.next, &cache_chain);
	cache_cache.colour_off = cache_line_size();
	cache_cache.array[smp_processor_id()] = &initarray_cache.cache;

	cache_cache.objsize = ALIGN(cache_cache.objsize, cache_line_size());

	cache_estimate(0, cache_cache.objsize, cache_line_size(), 0,
				&left_over, &cache_cache.num);
	if (!cache_cache.num)
		BUG();

	cache_cache.colour = left_over/cache_cache.colour_off;
	cache_cache.colour_next = 0;
	cache_cache.slab_size = ALIGN(cache_cache.num*sizeof(kmem_bufctl_t) +
				sizeof(struct slab), cache_line_size());

	/* 2+3) create the kmalloc caches */
	sizes = malloc_sizes;
	names = cache_names;

	while (sizes->cs_size) {
		/* For performance, all the general caches are L1 aligned.
		 * This should be particularly beneficial on SMP boxes, as it
		 * eliminates "false sharing".
		 * Note for systems short on memory removing the alignment will
		 * allow tighter packing of the smaller caches. */
		sizes->cs_cachep = kmem_cache_create(names->name,
			sizes->cs_size, ARCH_KMALLOC_MINALIGN,
			(ARCH_KMALLOC_FLAGS | SLAB_PANIC), NULL, NULL);

		/* Inc off-slab bufctl limit until the ceiling is hit. */
		if (!(OFF_SLAB(sizes->cs_cachep))) {
			offslab_limit = sizes->cs_size-sizeof(struct slab);
			offslab_limit /= sizeof(kmem_bufctl_t);
		}

		sizes->cs_dmacachep = kmem_cache_create(names->name_dma,
			sizes->cs_size, ARCH_KMALLOC_MINALIGN,
			(ARCH_KMALLOC_FLAGS | SLAB_CACHE_DMA | SLAB_PANIC),
			NULL, NULL);

		sizes++;
		names++;
	}
	/* 4) Replace the bootstrap head arrays */
	{
		void * ptr;
		
		ptr = kmalloc(sizeof(struct arraycache_init), GFP_KERNEL);
		local_irq_disable();
		BUG_ON(ac_data(&cache_cache) != &initarray_cache.cache);
		memcpy(ptr, ac_data(&cache_cache), sizeof(struct arraycache_init));
		cache_cache.array[smp_processor_id()] = ptr;
		local_irq_enable();
	
		ptr = kmalloc(sizeof(struct arraycache_init), GFP_KERNEL);
		local_irq_disable();
		BUG_ON(ac_data(malloc_sizes[0].cs_cachep) != &initarray_generic.cache);
		memcpy(ptr, ac_data(malloc_sizes[0].cs_cachep),
				sizeof(struct arraycache_init));
		malloc_sizes[0].cs_cachep->array[smp_processor_id()] = ptr;
		local_irq_enable();
	}

	/* 5) resize the head arrays to their final sizes */
	{
		kmem_cache_t *cachep;
		down(&cache_chain_sem);
		list_for_each_entry(cachep, &cache_chain, next)
			enable_cpucache(cachep);
		up(&cache_chain_sem);
	}

	/* Done! */
	g_cpucache_up = FULL;

	/* Register a cpu startup notifier callback
	 * that initializes ac_data for all new cpus
	 */
	register_cpu_notifier(&cpucache_notifier);
	

	/* The reap timers are started later, with a module init call:
	 * That part of the kernel is not yet operational.
	 */
}

int __init cpucache_init(void)
{
	int cpu;

	/* 
	 * Register the timers that return unneeded
	 * pages to gfp.
	 */
	for (cpu = 0; cpu < NR_CPUS; cpu++) {
		if (cpu_online(cpu))
			start_cpu_timer(cpu);
	}

	return 0;
}

__initcall(cpucache_init);

/*
 * Interface to system's page allocator. No need to hold the cache-lock.
 *
 * If we requested dmaable memory, we will get it. Even if we
 * did not request dmaable memory, we might get it, but that
 * would be relatively rare and ignorable.
 */
static void *kmem_getpages(kmem_cache_t *cachep, int flags, int nodeid)
{
	struct page *page;
	void *addr;
	int i;

	flags |= cachep->gfpflags;
	if (likely(nodeid == -1)) {
		addr = (void*)__get_free_pages(flags, cachep->gfporder);
		if (!addr)
			return NULL;
		page = virt_to_page(addr);
	} else {
		page = alloc_pages_node(nodeid, flags, cachep->gfporder);
		if (!page)
			return NULL;
		addr = page_address(page);
	}

	i = (1 << cachep->gfporder);
	if (cachep->flags & SLAB_RECLAIM_ACCOUNT)
		atomic_add(i, &slab_reclaim_pages);
	add_page_state(nr_slab, i);
	while (i--) {
		SetPageSlab(page);
		page++;
	}
	return addr;
}

/*
 * Interface to system's page release.
 */
static void kmem_freepages(kmem_cache_t *cachep, void *addr)
{
	unsigned long i = (1<<cachep->gfporder);
	struct page *page = virt_to_page(addr);
	const unsigned long nr_freed = i;

	while (i--) {
		if (!TestClearPageSlab(page))
			BUG();
		page++;
	}
	sub_page_state(nr_slab, nr_freed);
	if (current->reclaim_state)
		current->reclaim_state->reclaimed_slab += nr_freed;
	free_pages((unsigned long)addr, cachep->gfporder);
	if (cachep->flags & SLAB_RECLAIM_ACCOUNT) 
		atomic_sub(1<<cachep->gfporder, &slab_reclaim_pages);
}

#if DEBUG

#ifdef CONFIG_DEBUG_PAGEALLOC
static void store_stackinfo(kmem_cache_t *cachep, unsigned long *addr, unsigned long caller)
{
	int size = obj_reallen(cachep);

	addr = (unsigned long *)&((char*)addr)[obj_dbghead(cachep)];

	if (size < 5*sizeof(unsigned long))
		return;

	*addr++=0x12345678;
	*addr++=caller;
	*addr++=smp_processor_id();
	size -= 3*sizeof(unsigned long);
	{
		unsigned long *sptr = &caller;
		unsigned long svalue;

		while (!kstack_end(sptr)) {
			svalue = *sptr++;
			if (kernel_text_address(svalue)) {
				*addr++=svalue;
				size -= sizeof(unsigned long);
				if (size <= sizeof(unsigned long))
					break;
			}
		}

	}
	*addr++=0x87654321;
}
#endif

static void poison_obj(kmem_cache_t *cachep, void *addr, unsigned char val)
{
	int size = obj_reallen(cachep);
	addr = &((char*)addr)[obj_dbghead(cachep)];

	memset(addr, val, size);
	*(unsigned char *)(addr+size-1) = POISON_END;
}

static void dump_line(char *data, int offset, int limit)
{
	int i;
	printk(KERN_ERR "%03x:", offset);
	for (i=0;i<limit;i++) {
		printk(" %02x", (unsigned char)data[offset+i]);
	}
	printk("\n");
}
#endif

static void print_objinfo(kmem_cache_t *cachep, void *objp, int lines)
{
#if DEBUG
	int i, size;
	char *realobj;

	if (cachep->flags & SLAB_RED_ZONE) {
		printk(KERN_ERR "Redzone: 0x%lx/0x%lx.\n",
			*dbg_redzone1(cachep, objp),
			*dbg_redzone2(cachep, objp));
	}

	if (cachep->flags & SLAB_STORE_USER) {
		printk(KERN_ERR "Last user: [<%p>]", *dbg_userword(cachep, objp));
		print_symbol("(%s)", (unsigned long)*dbg_userword(cachep, objp));
		printk("\n");
	}
	realobj = (char*)objp+obj_dbghead(cachep);
	size = obj_reallen(cachep);
	for (i=0; i<size && lines;i+=16, lines--) {
		int limit;
		limit = 16;
		if (i+limit > size)
			limit = size-i;
		dump_line(realobj, i, limit);
	}
#endif
}

#if DEBUG

static void check_poison_obj(kmem_cache_t *cachep, void *objp)
{
	char *realobj;
	int size, i;
	int lines = 0;

	realobj = (char*)objp+obj_dbghead(cachep);
	size = obj_reallen(cachep);

	for (i=0;i<size;i++) {
		char exp = POISON_FREE;
		if (i == size-1)
			exp = POISON_END;
		if (realobj[i] != exp) {
			int limit;
			/* Mismatch ! */
			/* Print header */
			if (lines == 0) {
				printk(KERN_ERR "Slab corruption: start=%p, len=%d\n",
						realobj, size);
				print_objinfo(cachep, objp, 0);
			}
			/* Hexdump the affected line */
			i = (i/16)*16;
			limit = 16;
			if (i+limit > size)
				limit = size-i;
			dump_line(realobj, i, limit);
			i += 16;
			lines++;
			/* Limit to 5 lines */
			if (lines > 5)
				break;
		}
	}
	if (lines != 0) {
		/* Print some data about the neighboring objects, if they
		 * exist:
		 */
		struct slab *slabp = GET_PAGE_SLAB(virt_to_page(objp));
		int objnr;

		objnr = (objp-slabp->s_mem)/cachep->objsize;
		if (objnr) {
			objp = slabp->s_mem+(objnr-1)*cachep->objsize;
			realobj = (char*)objp+obj_dbghead(cachep);
			printk(KERN_ERR "Prev obj: start=%p, len=%d\n",
						realobj, size);
			print_objinfo(cachep, objp, 2);
		}
		if (objnr+1 < cachep->num) {
			objp = slabp->s_mem+(objnr+1)*cachep->objsize;
			realobj = (char*)objp+obj_dbghead(cachep);
			printk(KERN_ERR "Next obj: start=%p, len=%d\n",
						realobj, size);
			print_objinfo(cachep, objp, 2);
		}
	}
}
#endif

/* Destroy all the objs in a slab, and release the mem back to the system.
 * Before calling the slab must have been unlinked from the cache.
 * The cache-lock is not held/needed.
 */
static void slab_destroy (kmem_cache_t *cachep, struct slab *slabp)
{
#if DEBUG
	int i;
	for (i = 0; i < cachep->num; i++) {
		void *objp = slabp->s_mem + cachep->objsize * i;

		if (cachep->flags & SLAB_POISON) {
#ifdef CONFIG_DEBUG_PAGEALLOC
			if ((cachep->objsize%PAGE_SIZE)==0 && OFF_SLAB(cachep))
				kernel_map_pages(virt_to_page(objp), cachep->objsize/PAGE_SIZE,1);
			else
				check_poison_obj(cachep, objp);
#else
			check_poison_obj(cachep, objp);
#endif
		}
		if (cachep->flags & SLAB_RED_ZONE) {
			if (*dbg_redzone1(cachep, objp) != RED_INACTIVE)
				slab_error(cachep, "start of a freed object "
							"was overwritten");
			if (*dbg_redzone2(cachep, objp) != RED_INACTIVE)
				slab_error(cachep, "end of a freed object "
							"was overwritten");
		}
		if (cachep->dtor && !(cachep->flags & SLAB_POISON))
			(cachep->dtor)(objp+obj_dbghead(cachep), cachep, 0);
	}
#else
	if (cachep->dtor) {
		int i;
		for (i = 0; i < cachep->num; i++) {
			void* objp = slabp->s_mem+cachep->objsize*i;
			(cachep->dtor)(objp, cachep, 0);
		}
	}
#endif
	
	kmem_freepages(cachep, slabp->s_mem-slabp->colouroff);
	if (OFF_SLAB(cachep))
		kmem_cache_free(cachep->slabp_cache, slabp);
}

/**
 * kmem_cache_create - Create a cache.
 * @name: A string which is used in /proc/slabinfo to identify this cache.
 * @size: The size of objects to be created in this cache.
 * @align: The required alignment for the objects.
 * @flags: SLAB flags
 * @ctor: A constructor for the objects.
 * @dtor: A destructor for the objects.
 *
 * Returns a ptr to the cache on success, NULL on failure.
 * Cannot be called within a int, but can be interrupted.
 * The @ctor is run when new pages are allocated by the cache
 * and the @dtor is run before the pages are handed back.
 *
 * @name must be valid until the cache is destroyed. This implies that
 * the module calling this has to destroy the cache before getting 
 * unloaded.
 * 
 * The flags are
 *
 * %SLAB_POISON - Poison the slab with a known test pattern (a5a5a5a5)
 * to catch references to uninitialised memory.
 *
 * %SLAB_RED_ZONE - Insert `Red' zones around the allocated memory to check
 * for buffer overruns.
 *
 * %SLAB_NO_REAP - Don't automatically reap this cache when we're under
 * memory pressure.
 *
 * %SLAB_HWCACHE_ALIGN - Align the objects in this cache to a hardware
 * cacheline.  This can be beneficial if you're counting cycles as closely
 * as davem.
 */
kmem_cache_t *
kmem_cache_create (const char *name, size_t size, size_t align,
	unsigned long flags, void (*ctor)(void*, kmem_cache_t *, unsigned long),
	void (*dtor)(void*, kmem_cache_t *, unsigned long))
{
	size_t left_over, slab_size;
	kmem_cache_t *cachep = NULL;

	/*
	 * Sanity checks... these are all serious usage bugs.
	 */
	if ((!name) ||
		in_interrupt() ||
		(size < BYTES_PER_WORD) ||
		(size > (1<<MAX_OBJ_ORDER)*PAGE_SIZE) ||
		(dtor && !ctor)) {
			printk(KERN_ERR "%s: Early error in slab %s\n",
					__FUNCTION__, name);
			BUG();
		}

#if DEBUG
	WARN_ON(strchr(name, ' '));	/* It confuses parsers */
	if ((flags & SLAB_DEBUG_INITIAL) && !ctor) {
		/* No constructor, but inital state check requested */
		printk(KERN_ERR "%s: No con, but init state check "
				"requested - %s\n", __FUNCTION__, name);
		flags &= ~SLAB_DEBUG_INITIAL;
	}

#if FORCED_DEBUG
	/*
	 * Enable redzoning and last user accounting, except for caches with
	 * large objects, if the increased size would increase the object size
	 * above the next power of two: caches with object sizes just above a
	 * power of two have a significant amount of internal fragmentation.
	 */
	if ((size < 4096 || fls(size-1) == fls(size-1+3*BYTES_PER_WORD)))
		flags |= SLAB_RED_ZONE|SLAB_STORE_USER;
	flags |= SLAB_POISON;
#endif
#endif
	/*
	 * Always checks flags, a caller might be expecting debug
	 * support which isn't available.
	 */
	if (flags & ~CREATE_MASK)
		BUG();

	if (align) {
		/* combinations of forced alignment and advanced debugging is
		 * not yet implemented.
		 */
		flags &= ~(SLAB_RED_ZONE|SLAB_STORE_USER);
	} else {
		if (flags & SLAB_HWCACHE_ALIGN) {
			/* Default alignment: as specified by the arch code.
			 * Except if an object is really small, then squeeze multiple
			 * into one cacheline.
			 */
			align = cache_line_size();
			while (size <= align/2)
				align /= 2;
		} else {
			align = BYTES_PER_WORD;
		}
	}

	/* Get cache's description obj. */
	cachep = (kmem_cache_t *) kmem_cache_alloc(&cache_cache, SLAB_KERNEL);
	if (!cachep)
		goto opps;
	memset(cachep, 0, sizeof(kmem_cache_t));

	/* Check that size is in terms of words.  This is needed to avoid
	 * unaligned accesses for some archs when redzoning is used, and makes
	 * sure any on-slab bufctl's are also correctly aligned.
	 */
	if (size & (BYTES_PER_WORD-1)) {
		size += (BYTES_PER_WORD-1);
		size &= ~(BYTES_PER_WORD-1);
	}
	
#if DEBUG
	cachep->reallen = size;

	if (flags & SLAB_RED_ZONE) {
		/* redzoning only works with word aligned caches */
		align = BYTES_PER_WORD;

		/* add space for red zone words */
		cachep->dbghead += BYTES_PER_WORD;
		size += 2*BYTES_PER_WORD;
	}
	if (flags & SLAB_STORE_USER) {
		/* user store requires word alignment and
		 * one word storage behind the end of the real
		 * object.
		 */
		align = BYTES_PER_WORD;
		size += BYTES_PER_WORD;
	}
#if FORCED_DEBUG && defined(CONFIG_DEBUG_PAGEALLOC)
	if (size > 128 && cachep->reallen > cache_line_size() && size < PAGE_SIZE) {
		cachep->dbghead += PAGE_SIZE - size;
		size = PAGE_SIZE;
	}
#endif
#endif

	/* Determine if the slab management is 'on' or 'off' slab. */
	if (size >= (PAGE_SIZE>>3))
		/*
		 * Size is large, assume best to place the slab management obj
		 * off-slab (should allow better packing of objs).
		 */
		flags |= CFLGS_OFF_SLAB;

	size = ALIGN(size, align);

	if ((flags & SLAB_RECLAIM_ACCOUNT) && size <= PAGE_SIZE) {
		/*
		 * A VFS-reclaimable slab tends to have most allocations
		 * as GFP_NOFS and we really don't want to have to be allocating
		 * higher-order pages when we are unable to shrink dcache.
		 */
		cachep->gfporder = 0;
		cache_estimate(cachep->gfporder, size, align, flags,
					&left_over, &cachep->num);
	} else {
		/*
		 * Calculate size (in pages) of slabs, and the num of objs per
		 * slab.  This could be made much more intelligent.  For now,
		 * try to avoid using high page-orders for slabs.  When the
		 * gfp() funcs are more friendly towards high-order requests,
		 * this should be changed.
		 */
		do {
			unsigned int break_flag = 0;
cal_wastage:
			cache_estimate(cachep->gfporder, size, align, flags,
						&left_over, &cachep->num);
			if (break_flag)
				break;
			if (cachep->gfporder >= MAX_GFP_ORDER)
				break;
			if (!cachep->num)
				goto next;
			if (flags & CFLGS_OFF_SLAB &&
					cachep->num > offslab_limit) {
				/* This num of objs will cause problems. */
				cachep->gfporder--;
				break_flag++;
				goto cal_wastage;
			}

			/*
			 * Large num of objs is good, but v. large slabs are
			 * currently bad for the gfp()s.
			 */
			if (cachep->gfporder >= slab_break_gfp_order)
				break;

			if ((left_over*8) <= (PAGE_SIZE<<cachep->gfporder))
				break;	/* Acceptable internal fragmentation. */
next:
			cachep->gfporder++;
		} while (1);
	}

	if (!cachep->num) {
		printk("kmem_cache_create: couldn't create cache %s.\n", name);
		kmem_cache_free(&cache_cache, cachep);
		cachep = NULL;
		goto opps;
	}
	slab_size = ALIGN(cachep->num*sizeof(kmem_bufctl_t)
				+ sizeof(struct slab), align);

	/*
	 * If the slab has been placed off-slab, and we have enough space then
	 * move it on-slab. This is at the expense of any extra colouring.
	 */
	if (flags & CFLGS_OFF_SLAB && left_over >= slab_size) {
		flags &= ~CFLGS_OFF_SLAB;
		left_over -= slab_size;
	}

	if (flags & CFLGS_OFF_SLAB) {
		/* really off slab. No need for manual alignment */
		slab_size = cachep->num*sizeof(kmem_bufctl_t)+sizeof(struct slab);
	}

	cachep->colour_off = cache_line_size();
	/* Offset must be a multiple of the alignment. */
	if (cachep->colour_off < align)
		cachep->colour_off = align;
	cachep->colour = left_over/cachep->colour_off;
	cachep->slab_size = slab_size;
	cachep->flags = flags;
	cachep->gfpflags = 0;
	if (flags & SLAB_CACHE_DMA)
		cachep->gfpflags |= GFP_DMA;
	spin_lock_init(&cachep->spinlock);
	cachep->objsize = size;
	/* NUMA */
	INIT_LIST_HEAD(&cachep->lists.slabs_full);
	INIT_LIST_HEAD(&cachep->lists.slabs_partial);
	INIT_LIST_HEAD(&cachep->lists.slabs_free);

	if (flags & CFLGS_OFF_SLAB)
		cachep->slabp_cache = kmem_find_general_cachep(slab_size,0);
	cachep->ctor = ctor;
	cachep->dtor = dtor;
	cachep->name = name;

	/* Don't let CPUs to come and go */
	lock_cpu_hotplug();

	if (g_cpucache_up == FULL) {
		enable_cpucache(cachep);
	} else {
		if (g_cpucache_up == NONE) {
			/* Note: the first kmem_cache_create must create
			 * the cache that's used by kmalloc(24), otherwise
			 * the creation of further caches will BUG().
			 */
			cachep->array[smp_processor_id()] = &initarray_generic.cache;
			g_cpucache_up = PARTIAL;
		} else {
			cachep->array[smp_processor_id()] = kmalloc(sizeof(struct arraycache_init),GFP_KERNEL);
		}
		BUG_ON(!ac_data(cachep));
		ac_data(cachep)->avail = 0;
		ac_data(cachep)->limit = BOOT_CPUCACHE_ENTRIES;
		ac_data(cachep)->batchcount = 1;
		ac_data(cachep)->touched = 0;
		cachep->batchcount = 1;
		cachep->limit = BOOT_CPUCACHE_ENTRIES;
		cachep->free_limit = (1+num_online_cpus())*cachep->batchcount
					+ cachep->num;
	} 

	cachep->lists.next_reap = jiffies + REAPTIMEOUT_LIST3 +
					((unsigned long)cachep)%REAPTIMEOUT_LIST3;

	/* Need the semaphore to access the chain. */
	down(&cache_chain_sem);
	{
		struct list_head *p;
		mm_segment_t old_fs;

		old_fs = get_fs();
		set_fs(KERNEL_DS);
		list_for_each(p, &cache_chain) {
			kmem_cache_t *pc = list_entry(p, kmem_cache_t, next);
			char tmp;
			/* This happens when the module gets unloaded and doesn't
			   destroy its slab cache and noone else reuses the vmalloc
			   area of the module. Print a warning. */
			if (__get_user(tmp,pc->name)) { 
				printk("SLAB: cache with size %d has lost its name\n", 
					pc->objsize); 
				continue; 
			} 	
			if (!strcmp(pc->name,name)) { 
				printk("kmem_cache_create: duplicate cache %s\n",name); 
				up(&cache_chain_sem); 
				unlock_cpu_hotplug();
				BUG(); 
			}	
		}
		set_fs(old_fs);
	}

	/* cache setup completed, link it into the list */
	list_add(&cachep->next, &cache_chain);
	up(&cache_chain_sem);
	unlock_cpu_hotplug();
opps:
	if (!cachep && (flags & SLAB_PANIC))
		panic("kmem_cache_create(): failed to create slab `%s'\n",
			name);
	return cachep;
}
EXPORT_SYMBOL(kmem_cache_create);

#if DEBUG
static void check_irq_off(void)
{
	BUG_ON(!irqs_disabled());
}

static void check_irq_on(void)
{
	BUG_ON(irqs_disabled());
}

static void check_spinlock_acquired(kmem_cache_t *cachep)
{
#ifdef CONFIG_SMP
	check_irq_off();
	BUG_ON(spin_trylock(&cachep->spinlock));
#endif
}
#else
#define check_irq_off()	do { } while(0)
#define check_irq_on()	do { } while(0)
#define check_spinlock_acquired(x) do { } while(0)
#endif

/*
 * Waits for all CPUs to execute func().
 */
static void smp_call_function_all_cpus(void (*func) (void *arg), void *arg)
{
	check_irq_on();
	preempt_disable();

	local_irq_disable();
	func(arg);
	local_irq_enable();

	if (smp_call_function(func, arg, 1, 1))
		BUG();

	preempt_enable();
}

static void drain_array_locked(kmem_cache_t* cachep,
				struct array_cache *ac, int force);

static void do_drain(void *arg)
{
	kmem_cache_t *cachep = (kmem_cache_t*)arg;
	struct array_cache *ac;

	check_irq_off();
	ac = ac_data(cachep);
	spin_lock(&cachep->spinlock);
	free_block(cachep, &ac_entry(ac)[0], ac->avail);
	spin_unlock(&cachep->spinlock);
	ac->avail = 0;
}

static void drain_cpu_caches(kmem_cache_t *cachep)
{
	smp_call_function_all_cpus(do_drain, cachep);
	check_irq_on();
	spin_lock_irq(&cachep->spinlock);
	if (cachep->lists.shared)
		drain_array_locked(cachep, cachep->lists.shared, 1);
	spin_unlock_irq(&cachep->spinlock);
}


/* NUMA shrink all list3s */
static int __cache_shrink(kmem_cache_t *cachep)
{
	struct slab *slabp;
	int ret;

	drain_cpu_caches(cachep);

	check_irq_on();
	spin_lock_irq(&cachep->spinlock);

	for(;;) {
		struct list_head *p;

		p = cachep->lists.slabs_free.prev;
		if (p == &cachep->lists.slabs_free)
			break;

		slabp = list_entry(cachep->lists.slabs_free.prev, struct slab, list);
#if DEBUG
		if (slabp->inuse)
			BUG();
#endif
		list_del(&slabp->list);

		cachep->lists.free_objects -= cachep->num;
		spin_unlock_irq(&cachep->spinlock);
		slab_destroy(cachep, slabp);
		spin_lock_irq(&cachep->spinlock);
	}
	ret = !list_empty(&cachep->lists.slabs_full) ||
		!list_empty(&cachep->lists.slabs_partial);
	spin_unlock_irq(&cachep->spinlock);
	return ret;
}

/**
 * kmem_cache_shrink - Shrink a cache.
 * @cachep: The cache to shrink.
 *
 * Releases as many slabs as possible for a cache.
 * To help debugging, a zero exit status indicates all slabs were released.
 */
int kmem_cache_shrink(kmem_cache_t *cachep)
{
	if (!cachep || in_interrupt())
		BUG();

	return __cache_shrink(cachep);
}

EXPORT_SYMBOL(kmem_cache_shrink);

/**
 * kmem_cache_destroy - delete a cache
 * @cachep: the cache to destroy
 *
 * Remove a kmem_cache_t object from the slab cache.
 * Returns 0 on success.
 *
 * It is expected this function will be called by a module when it is
 * unloaded.  This will remove the cache completely, and avoid a duplicate
 * cache being allocated each time a module is loaded and unloaded, if the
 * module doesn't have persistent in-kernel storage across loads and unloads.
 *
 * The cache must be empty before calling this function.
 *
 * The caller must guarantee that noone will allocate memory from the cache
 * during the kmem_cache_destroy().
 */
int kmem_cache_destroy (kmem_cache_t * cachep)
{
	int i;

	if (!cachep || in_interrupt())
		BUG();

	/* Don't let CPUs to come and go */
	lock_cpu_hotplug();

	/* Find the cache in the chain of caches. */
	down(&cache_chain_sem);
	/*
	 * the chain is never empty, cache_cache is never destroyed
	 */
	list_del(&cachep->next);
	up(&cache_chain_sem);

	if (__cache_shrink(cachep)) {
		slab_error(cachep, "Can't free all objects");
		down(&cache_chain_sem);
		list_add(&cachep->next,&cache_chain);
		up(&cache_chain_sem);
		unlock_cpu_hotplug();
		return 1;
	}

	/* no cpu_online check required here since we clear the percpu
	 * array on cpu offline and set this to NULL.
	 */
	for (i = 0; i < NR_CPUS; i++)
		kfree(cachep->array[i]);

	/* NUMA: free the list3 structures */
	kfree(cachep->lists.shared);
	cachep->lists.shared = NULL;
	kmem_cache_free(&cache_cache, cachep);

	unlock_cpu_hotplug();

	return 0;
}

EXPORT_SYMBOL(kmem_cache_destroy);

/* Get the memory for a slab management obj. */
static struct slab* alloc_slabmgmt (kmem_cache_t *cachep,
			void *objp, int colour_off, int local_flags)
{
	struct slab *slabp;
	
	if (OFF_SLAB(cachep)) {
		/* Slab management obj is off-slab. */
		slabp = kmem_cache_alloc(cachep->slabp_cache, local_flags);
		if (!slabp)
			return NULL;
	} else {
		slabp = objp+colour_off;
		colour_off += cachep->slab_size;
	}
	slabp->inuse = 0;
	slabp->colouroff = colour_off;
	slabp->s_mem = objp+colour_off;

	return slabp;
}

static inline kmem_bufctl_t *slab_bufctl(struct slab *slabp)
{
	return (kmem_bufctl_t *)(slabp+1);
}

static void cache_init_objs (kmem_cache_t * cachep,
			struct slab * slabp, unsigned long ctor_flags)
{
	int i;

	for (i = 0; i < cachep->num; i++) {
		void* objp = slabp->s_mem+cachep->objsize*i;
#if DEBUG
		/* need to poison the objs? */
		if (cachep->flags & SLAB_POISON)
			poison_obj(cachep, objp, POISON_FREE);
		if (cachep->flags & SLAB_STORE_USER)
			*dbg_userword(cachep, objp) = NULL;

		if (cachep->flags & SLAB_RED_ZONE) {
			*dbg_redzone1(cachep, objp) = RED_INACTIVE;
			*dbg_redzone2(cachep, objp) = RED_INACTIVE;
		}
		/*
		 * Constructors are not allowed to allocate memory from
		 * the same cache which they are a constructor for.
		 * Otherwise, deadlock. They must also be threaded.
		 */
		if (cachep->ctor && !(cachep->flags & SLAB_POISON))
			cachep->ctor(objp+obj_dbghead(cachep), cachep, ctor_flags);

		if (cachep->flags & SLAB_RED_ZONE) {
			if (*dbg_redzone2(cachep, objp) != RED_INACTIVE)
				slab_error(cachep, "constructor overwrote the"
							" end of an object");
			if (*dbg_redzone1(cachep, objp) != RED_INACTIVE)
				slab_error(cachep, "constructor overwrote the"
							" start of an object");
		}
		if ((cachep->objsize % PAGE_SIZE) == 0 && OFF_SLAB(cachep) && cachep->flags & SLAB_POISON)
	       		kernel_map_pages(virt_to_page(objp), cachep->objsize/PAGE_SIZE, 0);
#else
		if (cachep->ctor)
			cachep->ctor(objp, cachep, ctor_flags);
#endif
		slab_bufctl(slabp)[i] = i+1;
	}
	slab_bufctl(slabp)[i-1] = BUFCTL_END;
	slabp->free = 0;
}

static void kmem_flagcheck(kmem_cache_t *cachep, int flags)
{
	if (flags & SLAB_DMA) {
		if (!(cachep->gfpflags & GFP_DMA))
			BUG();
	} else {
		if (cachep->gfpflags & GFP_DMA)
			BUG();
	}
}

static void set_slab_attr(kmem_cache_t *cachep, struct slab *slabp, void *objp)
{
	int i;
	struct page *page;

	/* Nasty!!!!!! I hope this is OK. */
	i = 1 << cachep->gfporder;
	page = virt_to_page(objp);
	do {
		SET_PAGE_CACHE(page, cachep);
		SET_PAGE_SLAB(page, slabp);
		page++;
	} while (--i);
}

/*
 * Grow (by 1) the number of slabs within a cache.  This is called by
 * kmem_cache_alloc() when there are no active objs left in a cache.
 */
static int cache_grow (kmem_cache_t * cachep, int flags)
{
	struct slab	*slabp;
	void		*objp;
	size_t		 offset;
	int		 local_flags;
	unsigned long	 ctor_flags;

	/* Be lazy and only check for valid flags here,
 	 * keeping it out of the critical path in kmem_cache_alloc().
	 */
	if (flags & ~(SLAB_DMA|SLAB_LEVEL_MASK|SLAB_NO_GROW))
		BUG();
	if (flags & SLAB_NO_GROW)
		return 0;

	ctor_flags = SLAB_CTOR_CONSTRUCTOR;
	local_flags = (flags & SLAB_LEVEL_MASK);
	if (!(local_flags & __GFP_WAIT))
		/*
		 * Not allowed to sleep.  Need to tell a constructor about
		 * this - it might need to know...
		 */
		ctor_flags |= SLAB_CTOR_ATOMIC;

	/* About to mess with non-constant members - lock. */
	check_irq_off();
	spin_lock(&cachep->spinlock);

	/* Get colour for the slab, and cal the next value. */
	offset = cachep->colour_next;
	cachep->colour_next++;
	if (cachep->colour_next >= cachep->colour)
		cachep->colour_next = 0;
	offset *= cachep->colour_off;

	spin_unlock(&cachep->spinlock);

	if (local_flags & __GFP_WAIT)
		local_irq_enable();

	/*
	 * The test for missing atomic flag is performed here, rather than
	 * the more obvious place, simply to reduce the critical path length
	 * in kmem_cache_alloc(). If a caller is seriously mis-behaving they
	 * will eventually be caught here (where it matters).
	 */
	kmem_flagcheck(cachep, flags);


	/* Get mem for the objs. */
	if (!(objp = kmem_getpages(cachep, flags, -1)))
		goto failed;

	/* Get slab management. */
	if (!(slabp = alloc_slabmgmt(cachep, objp, offset, local_flags)))
		goto opps1;

	set_slab_attr(cachep, slabp, objp);

	cache_init_objs(cachep, slabp, ctor_flags);

	if (local_flags & __GFP_WAIT)
		local_irq_disable();
	check_irq_off();
	spin_lock(&cachep->spinlock);

	/* Make slab active. */
	list_add_tail(&slabp->list, &(list3_data(cachep)->slabs_free));
	STATS_INC_GROWN(cachep);
	list3_data(cachep)->free_objects += cachep->num;
	spin_unlock(&cachep->spinlock);
	return 1;
opps1:
	kmem_freepages(cachep, objp);
failed:
	if (local_flags & __GFP_WAIT)
		local_irq_disable();
	return 0;
}

#if DEBUG

/*
 * Perform extra freeing checks:
 * - detect bad pointers.
 * - POISON/RED_ZONE checking
 * - destructor calls, for caches with POISON+dtor
 */
static void kfree_debugcheck(const void *objp)
{
	struct page *page;

	if (!virt_addr_valid(objp)) {
		printk(KERN_ERR "kfree_debugcheck: out of range ptr %lxh.\n",
			(unsigned long)objp);	
		BUG();	
	}
	page = virt_to_page(objp);
	if (!PageSlab(page)) {
		printk(KERN_ERR "kfree_debugcheck: bad ptr %lxh.\n", (unsigned long)objp);
		BUG();
	}
}

static void *cache_free_debugcheck (kmem_cache_t * cachep, void * objp, void *caller)
{
	struct page *page;
	unsigned int objnr;
	struct slab *slabp;

	objp -= obj_dbghead(cachep);
	kfree_debugcheck(objp);
	page = virt_to_page(objp);

	if (GET_PAGE_CACHE(page) != cachep) {
		printk(KERN_ERR "mismatch in kmem_cache_free: expected cache %p, got %p\n",
				GET_PAGE_CACHE(page),cachep);
		printk(KERN_ERR "%p is %s.\n", cachep, cachep->name);
		printk(KERN_ERR "%p is %s.\n", GET_PAGE_CACHE(page), GET_PAGE_CACHE(page)->name);
		WARN_ON(1);
	}
	slabp = GET_PAGE_SLAB(page);

	if (cachep->flags & SLAB_RED_ZONE) {
		if (*dbg_redzone1(cachep, objp) != RED_ACTIVE || *dbg_redzone2(cachep, objp) != RED_ACTIVE) {
			slab_error(cachep, "double free, or memory outside"
						" object was overwritten");
			printk(KERN_ERR "%p: redzone 1: 0x%lx, redzone 2: 0x%lx.\n",
					objp, *dbg_redzone1(cachep, objp), *dbg_redzone2(cachep, objp));
		}
		*dbg_redzone1(cachep, objp) = RED_INACTIVE;
		*dbg_redzone2(cachep, objp) = RED_INACTIVE;
	}
	if (cachep->flags & SLAB_STORE_USER)
		*dbg_userword(cachep, objp) = caller;

	objnr = (objp-slabp->s_mem)/cachep->objsize;

	BUG_ON(objnr >= cachep->num);
	BUG_ON(objp != slabp->s_mem + objnr*cachep->objsize);

	if (cachep->flags & SLAB_DEBUG_INITIAL) {
		/* Need to call the slab's constructor so the
		 * caller can perform a verify of its state (debugging).
		 * Called without the cache-lock held.
		 */
		cachep->ctor(objp+obj_dbghead(cachep),
					cachep, SLAB_CTOR_CONSTRUCTOR|SLAB_CTOR_VERIFY);
	}
	if (cachep->flags & SLAB_POISON && cachep->dtor) {
		/* we want to cache poison the object,
		 * call the destruction callback
		 */
		cachep->dtor(objp+obj_dbghead(cachep), cachep, 0);
	}
	if (cachep->flags & SLAB_POISON) {
#ifdef CONFIG_DEBUG_PAGEALLOC
		if ((cachep->objsize % PAGE_SIZE) == 0 && OFF_SLAB(cachep)) {
			store_stackinfo(cachep, objp, (unsigned long)caller);
	       		kernel_map_pages(virt_to_page(objp), cachep->objsize/PAGE_SIZE, 0);
		} else {
			poison_obj(cachep, objp, POISON_FREE);
		}
#else
		poison_obj(cachep, objp, POISON_FREE);
#endif
	}
	return objp;
}

static void check_slabp(kmem_cache_t *cachep, struct slab *slabp)
{
	int i;
	int entries = 0;
	
	check_spinlock_acquired(cachep);
	/* Check slab's freelist to see if this obj is there. */
	for (i = slabp->free; i != BUFCTL_END; i = slab_bufctl(slabp)[i]) {
		entries++;
		if (entries > cachep->num || i < 0 || i >= cachep->num)
			goto bad;
	}
	if (entries != cachep->num - slabp->inuse) {
		int i;
bad:
		printk(KERN_ERR "slab: Internal list corruption detected in cache '%s'(%d), slabp %p(%d). Hexdump:\n",
				cachep->name, cachep->num, slabp, slabp->inuse);
		for (i=0;i<sizeof(slabp)+cachep->num*sizeof(kmem_bufctl_t);i++) {
			if ((i%16)==0)
				printk("\n%03x:", i);
			printk(" %02x", ((unsigned char*)slabp)[i]);
		}
		printk("\n");
		BUG();
	}
}
#else
#define kfree_debugcheck(x) do { } while(0)
#define cache_free_debugcheck(x,objp,z) (objp)
#define check_slabp(x,y) do { } while(0)
#endif

static void* cache_alloc_refill(kmem_cache_t* cachep, int flags)
{
	int batchcount;
	struct kmem_list3 *l3;
	struct array_cache *ac;

	check_irq_off();
	ac = ac_data(cachep);
retry:
	batchcount = ac->batchcount;
	if (!ac->touched && batchcount > BATCHREFILL_LIMIT) {
		/* if there was little recent activity on this
		 * cache, then perform only a partial refill.
		 * Otherwise we could generate refill bouncing.
		 */
		batchcount = BATCHREFILL_LIMIT;
	}
	l3 = list3_data(cachep);

	BUG_ON(ac->avail > 0);
	spin_lock(&cachep->spinlock);
	if (l3->shared) {
		struct array_cache *shared_array = l3->shared;
		if (shared_array->avail) {
			if (batchcount > shared_array->avail)
				batchcount = shared_array->avail;
			shared_array->avail -= batchcount;
			ac->avail = batchcount;
			memcpy(ac_entry(ac), &ac_entry(shared_array)[shared_array->avail],
					sizeof(void*)*batchcount);
			shared_array->touched = 1;
			goto alloc_done;
		}
	}
	while (batchcount > 0) {
		struct list_head *entry;
		struct slab *slabp;
		/* Get slab alloc is to come from. */
		entry = l3->slabs_partial.next;
		if (entry == &l3->slabs_partial) {
			l3->free_touched = 1;
			entry = l3->slabs_free.next;
			if (entry == &l3->slabs_free)
				goto must_grow;
		}

		slabp = list_entry(entry, struct slab, list);
		check_slabp(cachep, slabp);
		check_spinlock_acquired(cachep);
		while (slabp->inuse < cachep->num && batchcount--) {
			kmem_bufctl_t next;
			STATS_INC_ALLOCED(cachep);
			STATS_INC_ACTIVE(cachep);
			STATS_SET_HIGH(cachep);

			/* get obj pointer */
			ac_entry(ac)[ac->avail++] = slabp->s_mem + slabp->free*cachep->objsize;

			slabp->inuse++;
			next = slab_bufctl(slabp)[slabp->free];
#if DEBUG
			slab_bufctl(slabp)[slabp->free] = BUFCTL_FREE;
#endif
		       	slabp->free = next;
		}
		check_slabp(cachep, slabp);

		/* move slabp to correct slabp list: */
		list_del(&slabp->list);
		if (slabp->free == BUFCTL_END)
			list_add(&slabp->list, &l3->slabs_full);
		else
			list_add(&slabp->list, &l3->slabs_partial);
	}

must_grow:
	l3->free_objects -= ac->avail;
alloc_done:
	spin_unlock(&cachep->spinlock);

	if (unlikely(!ac->avail)) {
		int x;
		x = cache_grow(cachep, flags);
		
		// cache_grow can reenable interrupts, then ac could change.
		ac = ac_data(cachep);
		if (!x && ac->avail == 0)	// no objects in sight? abort
			return NULL;

		if (!ac->avail)		// objects refilled by interrupt?
			goto retry;
	}
	ac->touched = 1;
	return ac_entry(ac)[--ac->avail];
}

static inline void
cache_alloc_debugcheck_before(kmem_cache_t *cachep, int flags)
{
	might_sleep_if(flags & __GFP_WAIT);
#if DEBUG
	kmem_flagcheck(cachep, flags);
#endif
}

#if DEBUG
static void *
cache_alloc_debugcheck_after(kmem_cache_t *cachep,
			unsigned long flags, void *objp, void *caller)
{
	if (!objp)	
		return objp;
 	if (cachep->flags & SLAB_POISON) {
#ifdef CONFIG_DEBUG_PAGEALLOC
		if ((cachep->objsize % PAGE_SIZE) == 0 && OFF_SLAB(cachep))
			kernel_map_pages(virt_to_page(objp), cachep->objsize/PAGE_SIZE, 1);
		else
			check_poison_obj(cachep, objp);
#else
		check_poison_obj(cachep, objp);
#endif
		poison_obj(cachep, objp, POISON_INUSE);
	}
	if (cachep->flags & SLAB_STORE_USER)
		*dbg_userword(cachep, objp) = caller;

	if (cachep->flags & SLAB_RED_ZONE) {
		if (*dbg_redzone1(cachep, objp) != RED_INACTIVE || *dbg_redzone2(cachep, objp) != RED_INACTIVE) {
			slab_error(cachep, "double free, or memory outside"
						" object was overwritten");
			printk(KERN_ERR "%p: redzone 1: 0x%lx, redzone 2: 0x%lx.\n",
					objp, *dbg_redzone1(cachep, objp), *dbg_redzone2(cachep, objp));
		}
		*dbg_redzone1(cachep, objp) = RED_ACTIVE;
		*dbg_redzone2(cachep, objp) = RED_ACTIVE;
	}
	objp += obj_dbghead(cachep);
	if (cachep->ctor && cachep->flags & SLAB_POISON) {
		unsigned long	ctor_flags = SLAB_CTOR_CONSTRUCTOR;

		if (!(flags & __GFP_WAIT))
			ctor_flags |= SLAB_CTOR_ATOMIC;

		cachep->ctor(objp, cachep, ctor_flags);
	}	
	return objp;
}
#else
#define cache_alloc_debugcheck_after(a,b,objp,d) (objp)
#endif


static inline void * __cache_alloc (kmem_cache_t *cachep, int flags)
{
	unsigned long save_flags;
	void* objp;
	struct array_cache *ac;

	cache_alloc_debugcheck_before(cachep, flags);

	local_irq_save(save_flags);
	ac = ac_data(cachep);
	if (likely(ac->avail)) {
		STATS_INC_ALLOCHIT(cachep);
		ac->touched = 1;
		objp = ac_entry(ac)[--ac->avail];
	} else {
		STATS_INC_ALLOCMISS(cachep);
		objp = cache_alloc_refill(cachep, flags);
	}
	local_irq_restore(save_flags);
	objp = cache_alloc_debugcheck_after(cachep, flags, objp, __builtin_return_address(0));
	return objp;
}

/* 
 * NUMA: different approach needed if the spinlock is moved into
 * the l3 structure
 */

static void free_block(kmem_cache_t *cachep, void **objpp, int nr_objects)
{
	int i;

	check_spinlock_acquired(cachep);

	/* NUMA: move add into loop */
	cachep->lists.free_objects += nr_objects;

	for (i = 0; i < nr_objects; i++) {
		void *objp = objpp[i];
		struct slab *slabp;
		unsigned int objnr;

		slabp = GET_PAGE_SLAB(virt_to_page(objp));
		list_del(&slabp->list);
		objnr = (objp - slabp->s_mem) / cachep->objsize;
		check_slabp(cachep, slabp);
#if DEBUG
		if (slab_bufctl(slabp)[objnr] != BUFCTL_FREE) {
			printk(KERN_ERR "slab: double free detected in cache '%s', objp %p.\n",
						cachep->name, objp);
			BUG();
		}
#endif
		slab_bufctl(slabp)[objnr] = slabp->free;
		slabp->free = objnr;
		STATS_DEC_ACTIVE(cachep);
		slabp->inuse--;
		check_slabp(cachep, slabp);

		/* fixup slab chains */
		if (slabp->inuse == 0) {
			if (cachep->lists.free_objects > cachep->free_limit) {
				cachep->lists.free_objects -= cachep->num;
				slab_destroy(cachep, slabp);
			} else {
				list_add(&slabp->list,
				&list3_data_ptr(cachep, objp)->slabs_free);
			}
		} else {
			/* Unconditionally move a slab to the end of the
			 * partial list on free - maximum time for the
			 * other objects to be freed, too.
			 */
			list_add_tail(&slabp->list,
				&list3_data_ptr(cachep, objp)->slabs_partial);
		}
	}
}

static void cache_flusharray (kmem_cache_t* cachep, struct array_cache *ac)
{
	int batchcount;

	batchcount = ac->batchcount;
#if DEBUG
	BUG_ON(!batchcount || batchcount > ac->avail);
#endif
	check_irq_off();
	spin_lock(&cachep->spinlock);
	if (cachep->lists.shared) {
		struct array_cache *shared_array = cachep->lists.shared;
		int max = shared_array->limit-shared_array->avail;
		if (max) {
			if (batchcount > max)
				batchcount = max;
			memcpy(&ac_entry(shared_array)[shared_array->avail],
					&ac_entry(ac)[0],
					sizeof(void*)*batchcount);
			shared_array->avail += batchcount;
			goto free_done;
		}
	}

	free_block(cachep, &ac_entry(ac)[0], batchcount);
free_done:
#if STATS
	{
		int i = 0;
		struct list_head *p;

		p = list3_data(cachep)->slabs_free.next;
		while (p != &(list3_data(cachep)->slabs_free)) {
			struct slab *slabp;

			slabp = list_entry(p, struct slab, list);
			BUG_ON(slabp->inuse);

			i++;
			p = p->next;
		}
		STATS_SET_FREEABLE(cachep, i);
	}
#endif
	spin_unlock(&cachep->spinlock);
	ac->avail -= batchcount;
	memmove(&ac_entry(ac)[0], &ac_entry(ac)[batchcount],
			sizeof(void*)*ac->avail);
}

/*
 * __cache_free
 * Release an obj back to its cache. If the obj has a constructed
 * state, it must be in this state _before_ it is released.
 *
 * Called with disabled ints.
 */
static inline void __cache_free (kmem_cache_t *cachep, void* objp)
{
	struct array_cache *ac = ac_data(cachep);

	check_irq_off();
	objp = cache_free_debugcheck(cachep, objp, __builtin_return_address(0));

	if (likely(ac->avail < ac->limit)) {
		STATS_INC_FREEHIT(cachep);
		ac_entry(ac)[ac->avail++] = objp;
		return;
	} else {
		STATS_INC_FREEMISS(cachep);
		cache_flusharray(cachep, ac);
		ac_entry(ac)[ac->avail++] = objp;
	}
}

/**
 * kmem_cache_alloc - Allocate an object
 * @cachep: The cache to allocate from.
 * @flags: See kmalloc().
 *
 * Allocate an object from this cache.  The flags are only relevant
 * if the cache has no available objects.
 */
void * kmem_cache_alloc (kmem_cache_t *cachep, int flags)
{
	return __cache_alloc(cachep, flags);
}

EXPORT_SYMBOL(kmem_cache_alloc);

/**
 * kmem_ptr_validate - check if an untrusted pointer might
 *	be a slab entry.
 * @cachep: the cache we're checking against
 * @ptr: pointer to validate
 *
 * This verifies that the untrusted pointer looks sane:
 * it is _not_ a guarantee that the pointer is actually
 * part of the slab cache in question, but it at least
 * validates that the pointer can be dereferenced and
 * looks half-way sane.
 *
 * Currently only used for dentry validation.
 */
int fastcall kmem_ptr_validate(kmem_cache_t *cachep, void *ptr)
{
	unsigned long addr = (unsigned long) ptr;
	unsigned long min_addr = PAGE_OFFSET;
	unsigned long align_mask = BYTES_PER_WORD-1;
	unsigned long size = cachep->objsize;
	struct page *page;

	if (unlikely(addr < min_addr))
		goto out;
	if (unlikely(addr > (unsigned long)high_memory - size))
		goto out;
	if (unlikely(addr & align_mask))
		goto out;
	if (unlikely(!kern_addr_valid(addr)))
		goto out;
	if (unlikely(!kern_addr_valid(addr + size - 1)))
		goto out;
	page = virt_to_page(ptr);
	if (unlikely(!PageSlab(page)))
		goto out;
	if (unlikely(GET_PAGE_CACHE(page) != cachep))
		goto out;
	return 1;
out:
	return 0;
}

/**
 * kmem_cache_alloc_node - Allocate an object on the specified node
 * @cachep: The cache to allocate from.
 * @flags: See kmalloc().
 * @nodeid: node number of the target node.
 *
 * Identical to kmem_cache_alloc, except that this function is slow
 * and can sleep. And it will allocate memory on the given node, which
 * can improve the performance for cpu bound structures.
 */
void *kmem_cache_alloc_node(kmem_cache_t *cachep, int nodeid)
{
	size_t offset;
	void *objp;
	struct slab *slabp;
	kmem_bufctl_t next;

	/* The main algorithms are not node aware, thus we have to cheat:
	 * We bypass all caches and allocate a new slab.
	 * The following code is a streamlined copy of cache_grow().
	 */

	/* Get colour for the slab, and update the next value. */
	spin_lock_irq(&cachep->spinlock);
	offset = cachep->colour_next;
	cachep->colour_next++;
	if (cachep->colour_next >= cachep->colour)
		cachep->colour_next = 0;
	offset *= cachep->colour_off;
	spin_unlock_irq(&cachep->spinlock);

	/* Get mem for the objs. */
	if (!(objp = kmem_getpages(cachep, GFP_KERNEL, nodeid)))
		goto failed;

	/* Get slab management. */
	if (!(slabp = alloc_slabmgmt(cachep, objp, offset, GFP_KERNEL)))
		goto opps1;

	set_slab_attr(cachep, slabp, objp);
	cache_init_objs(cachep, slabp, SLAB_CTOR_CONSTRUCTOR);

	/* The first object is ours: */
	objp = slabp->s_mem + slabp->free*cachep->objsize;
	slabp->inuse++;
	next = slab_bufctl(slabp)[slabp->free];
#if DEBUG
	slab_bufctl(slabp)[slabp->free] = BUFCTL_FREE;
#endif
	slabp->free = next;

	/* add the remaining objects into the cache */
	spin_lock_irq(&cachep->spinlock);
	check_slabp(cachep, slabp);
	STATS_INC_GROWN(cachep);
	/* Make slab active. */
	if (slabp->free == BUFCTL_END) {
		list_add_tail(&slabp->list, &(list3_data(cachep)->slabs_full));
	} else {
		list_add_tail(&slabp->list,
				&(list3_data(cachep)->slabs_partial));
		list3_data(cachep)->free_objects += cachep->num-1;
	}
	spin_unlock_irq(&cachep->spinlock);
	objp = cache_alloc_debugcheck_after(cachep, GFP_KERNEL, objp,
					__builtin_return_address(0));
	return objp;
opps1:
	kmem_freepages(cachep, objp);
failed:
	return NULL;

}
EXPORT_SYMBOL(kmem_cache_alloc_node);

/**
 * kmalloc - allocate memory
 * @size: how many bytes of memory are required.
 * @flags: the type of memory to allocate.
 *
 * kmalloc is the normal method of allocating memory
 * in the kernel.
 *
 * The @flags argument may be one of:
 *
 * %GFP_USER - Allocate memory on behalf of user.  May sleep.
 *
 * %GFP_KERNEL - Allocate normal kernel ram.  May sleep.
 *
 * %GFP_ATOMIC - Allocation will not sleep.  Use inside interrupt handlers.
 *
 * Additionally, the %GFP_DMA flag may be set to indicate the memory
 * must be suitable for DMA.  This can mean different things on different
 * platforms.  For example, on i386, it means that the memory must come
 * from the first 16MB.
 */
void * __kmalloc (size_t size, int flags)
{
	struct cache_sizes *csizep = malloc_sizes;

	for (; csizep->cs_size; csizep++) {
		if (size > csizep->cs_size)
			continue;
#if DEBUG
		/* This happens if someone tries to call
		 * kmem_cache_create(), or kmalloc(), before
		 * the generic caches are initialized.
		 */
		BUG_ON(csizep->cs_cachep == NULL);
#endif
		return __cache_alloc(flags & GFP_DMA ?
			 csizep->cs_dmacachep : csizep->cs_cachep, flags);
	}
	return NULL;
}

EXPORT_SYMBOL(__kmalloc);

#ifdef CONFIG_SMP
/**
 * __alloc_percpu - allocate one copy of the object for every present
 * cpu in the system, zeroing them.
 * Objects should be dereferenced using per_cpu_ptr/get_cpu_ptr
 * macros only.
 *
 * @size: how many bytes of memory are required.
 * @align: the alignment, which can't be greater than SMP_CACHE_BYTES.
 */
void *__alloc_percpu(size_t size, size_t align)
{
	int i;
	struct percpu_data *pdata = kmalloc(sizeof (*pdata), GFP_KERNEL);

	if (!pdata)
		return NULL;

	for (i = 0; i < NR_CPUS; i++) {
		if (!cpu_possible(i))
			continue;
		pdata->ptrs[i] = kmem_cache_alloc_node(
				kmem_find_general_cachep(size, GFP_KERNEL),
				cpu_to_node(i));

		if (!pdata->ptrs[i])
			goto unwind_oom;
		memset(pdata->ptrs[i], 0, size);
	}

	/* Catch derefs w/o wrappers */
	return (void *) (~(unsigned long) pdata);

unwind_oom:
	while (--i >= 0) {
		if (!cpu_possible(i))
			continue;
		kfree(pdata->ptrs[i]);
	}
	kfree(pdata);
	return NULL;
}

EXPORT_SYMBOL(__alloc_percpu);
#endif

/**
 * kmem_cache_free - Deallocate an object
 * @cachep: The cache the allocation was from.
 * @objp: The previously allocated object.
 *
 * Free an object which was previously allocated from this
 * cache.
 */
void kmem_cache_free (kmem_cache_t *cachep, void *objp)
{
	unsigned long flags;

	local_irq_save(flags);
	__cache_free(cachep, objp);
	local_irq_restore(flags);
}

EXPORT_SYMBOL(kmem_cache_free);

/**
 * kfree - free previously allocated memory
 * @objp: pointer returned by kmalloc.
 *
 * Don't free memory not originally allocated by kmalloc()
 * or you will run into trouble.
 */
void kfree (const void *objp)
{
	kmem_cache_t *c;
	unsigned long flags;

	if (!objp)
		return;
	local_irq_save(flags);
	kfree_debugcheck(objp);
	c = GET_PAGE_CACHE(virt_to_page(objp));
	__cache_free(c, (void*)objp);
	local_irq_restore(flags);
}

EXPORT_SYMBOL(kfree);

#ifdef CONFIG_SMP
/**
 * free_percpu - free previously allocated percpu memory
 * @objp: pointer returned by alloc_percpu.
 *
 * Don't free memory not originally allocated by alloc_percpu()
 * The complemented objp is to check for that.
 */
void
free_percpu(const void *objp)
{
	int i;
	struct percpu_data *p = (struct percpu_data *) (~(unsigned long) objp);

	for (i = 0; i < NR_CPUS; i++) {
		if (!cpu_possible(i))
			continue;
		kfree(p->ptrs[i]);
	}
}

EXPORT_SYMBOL(free_percpu);
#endif

unsigned int kmem_cache_size(kmem_cache_t *cachep)
{
	return obj_reallen(cachep);
}

EXPORT_SYMBOL(kmem_cache_size);

kmem_cache_t * kmem_find_general_cachep (size_t size, int gfpflags)
{
	struct cache_sizes *csizep = malloc_sizes;

	/* This function could be moved to the header file, and
	 * made inline so consumers can quickly determine what
	 * cache pointer they require.
	 */
	for ( ; csizep->cs_size; csizep++) {
		if (size > csizep->cs_size)
			continue;
		break;
	}
	return (gfpflags & GFP_DMA) ? csizep->cs_dmacachep : csizep->cs_cachep;
}

EXPORT_SYMBOL(kmem_find_general_cachep);

struct ccupdate_struct {
	kmem_cache_t *cachep;
	struct array_cache *new[NR_CPUS];
};

static void do_ccupdate_local(void *info)
{
	struct ccupdate_struct *new = (struct ccupdate_struct *)info;
	struct array_cache *old;

	check_irq_off();
	old = ac_data(new->cachep);
	
	new->cachep->array[smp_processor_id()] = new->new[smp_processor_id()];
	new->new[smp_processor_id()] = old;
}


static int do_tune_cpucache (kmem_cache_t* cachep, int limit, int batchcount, int shared)
{
	struct ccupdate_struct new;
	struct array_cache *new_shared;
	int i;

	memset(&new.new,0,sizeof(new.new));
	for (i = 0; i < NR_CPUS; i++) {
		if (cpu_online(i)) {
			new.new[i] = alloc_arraycache(i, limit, batchcount);
			if (!new.new[i]) {
				for (i--; i >= 0; i--) kfree(new.new[i]);
				return -ENOMEM;
			}
		} else {
			new.new[i] = NULL;
		}
	}
	new.cachep = cachep;

	smp_call_function_all_cpus(do_ccupdate_local, (void *)&new);
	
	check_irq_on();
	spin_lock_irq(&cachep->spinlock);
	cachep->batchcount = batchcount;
	cachep->limit = limit;
	cachep->free_limit = (1+num_online_cpus())*cachep->batchcount + cachep->num;
	spin_unlock_irq(&cachep->spinlock);

	for (i = 0; i < NR_CPUS; i++) {
		struct array_cache *ccold = new.new[i];
		if (!ccold)
			continue;
		spin_lock_irq(&cachep->spinlock);
		free_block(cachep, ac_entry(ccold), ccold->avail);
		spin_unlock_irq(&cachep->spinlock);
		kfree(ccold);
	}
	new_shared = alloc_arraycache(-1, batchcount*shared, 0xbaadf00d);
	if (new_shared) {
		struct array_cache *old;

		spin_lock_irq(&cachep->spinlock);
		old = cachep->lists.shared;
		cachep->lists.shared = new_shared;
		if (old)
			free_block(cachep, ac_entry(old), old->avail);
		spin_unlock_irq(&cachep->spinlock);
		kfree(old);
	}

	return 0;
}


static void enable_cpucache (kmem_cache_t *cachep)
{
	int err;
	int limit, shared;

	/* The head array serves three purposes:
	 * - create a LIFO ordering, i.e. return objects that are cache-warm
	 * - reduce the number of spinlock operations.
	 * - reduce the number of linked list operations on the slab and 
	 *   bufctl chains: array operations are cheaper.
	 * The numbers are guessed, we should auto-tune as described by
	 * Bonwick.
	 */
	if (cachep->objsize > 131072)
		limit = 1;
	else if (cachep->objsize > PAGE_SIZE)
		limit = 8;
	else if (cachep->objsize > 1024)
		limit = 24;
	else if (cachep->objsize > 256)
		limit = 54;
	else
		limit = 120;

	/* Cpu bound tasks (e.g. network routing) can exhibit cpu bound
	 * allocation behaviour: Most allocs on one cpu, most free operations
	 * on another cpu. For these cases, an efficient object passing between
	 * cpus is necessary. This is provided by a shared array. The array
	 * replaces Bonwick's magazine layer.
	 * On uniprocessor, it's functionally equivalent (but less efficient)
	 * to a larger limit. Thus disabled by default.
	 */
	shared = 0;
#ifdef CONFIG_SMP
	if (cachep->objsize <= PAGE_SIZE)
		shared = 8;
#endif

#if DEBUG
	/* With debugging enabled, large batchcount lead to excessively
	 * long periods with disabled local interrupts. Limit the 
	 * batchcount
	 */
	if (limit > 32)
		limit = 32;
#endif
	err = do_tune_cpucache(cachep, limit, (limit+1)/2, shared);
	if (err)
		printk(KERN_ERR "enable_cpucache failed for %s, error %d.\n",
					cachep->name, -err);
}

static void drain_array(kmem_cache_t *cachep, struct array_cache *ac)
{
	int tofree;

	check_irq_off();
	if (ac->touched) {
		ac->touched = 0;
	} else if (ac->avail) {
		tofree = (ac->limit+4)/5;
		if (tofree > ac->avail) {
			tofree = (ac->avail+1)/2;
		}
		spin_lock(&cachep->spinlock);
		free_block(cachep, ac_entry(ac), tofree);
		spin_unlock(&cachep->spinlock);
		ac->avail -= tofree;
		memmove(&ac_entry(ac)[0], &ac_entry(ac)[tofree],
					sizeof(void*)*ac->avail);
	}
}

static void drain_array_locked(kmem_cache_t *cachep,
				struct array_cache *ac, int force)
{
	int tofree;

	check_spinlock_acquired(cachep);
	if (ac->touched && !force) {
		ac->touched = 0;
	} else if (ac->avail) {
		tofree = force ? ac->avail : (ac->limit+4)/5;
		if (tofree > ac->avail) {
			tofree = (ac->avail+1)/2;
		}
		free_block(cachep, ac_entry(ac), tofree);
		ac->avail -= tofree;
		memmove(&ac_entry(ac)[0], &ac_entry(ac)[tofree],
					sizeof(void*)*ac->avail);
	}
}

/**
 * cache_reap - Reclaim memory from caches.
 *
 * Called from a timer, every few seconds
 * Purpose:
 * - clear the per-cpu caches for this CPU.
 * - return freeable pages to the main free memory pool.
 *
 * If we cannot acquire the cache chain semaphore then just give up - we'll
 * try again next timer interrupt.
 */
static void cache_reap (void)
{
	struct list_head *walk;

#if DEBUG
	BUG_ON(!in_interrupt());
	BUG_ON(in_irq());
#endif
	if (down_trylock(&cache_chain_sem))
		return;

	list_for_each(walk, &cache_chain) {
		kmem_cache_t *searchp;
		struct list_head* p;
		int tofree;
		struct slab *slabp;

		searchp = list_entry(walk, kmem_cache_t, next);

		if (searchp->flags & SLAB_NO_REAP)
			goto next;

		check_irq_on();
		local_irq_disable();
		drain_array(searchp, ac_data(searchp));

		if(time_after(searchp->lists.next_reap, jiffies))
			goto next_irqon;

		spin_lock(&searchp->spinlock);
		if(time_after(searchp->lists.next_reap, jiffies)) {
			goto next_unlock;
		}
		searchp->lists.next_reap = jiffies + REAPTIMEOUT_LIST3;

		if (searchp->lists.shared)
			drain_array_locked(searchp, searchp->lists.shared, 0);

		if (searchp->lists.free_touched) {
			searchp->lists.free_touched = 0;
			goto next_unlock;
		}

		tofree = (searchp->free_limit+5*searchp->num-1)/(5*searchp->num);
		do {
			p = list3_data(searchp)->slabs_free.next;
			if (p == &(list3_data(searchp)->slabs_free))
				break;

			slabp = list_entry(p, struct slab, list);
			BUG_ON(slabp->inuse);
			list_del(&slabp->list);
			STATS_INC_REAPED(searchp);

			/* Safe to drop the lock. The slab is no longer
			 * linked to the cache.
			 * searchp cannot disappear, we hold
			 * cache_chain_lock
			 */
			searchp->lists.free_objects -= searchp->num;
			spin_unlock_irq(&searchp->spinlock);
			slab_destroy(searchp, slabp);
			spin_lock_irq(&searchp->spinlock);
		} while(--tofree > 0);
next_unlock:
		spin_unlock(&searchp->spinlock);
next_irqon:
		local_irq_enable();
next:
		;
	}
	check_irq_on();
	up(&cache_chain_sem);
}

/*
 * This is a timer handler.  There is one per CPU.  It is called periodially
 * to shrink this CPU's caches.  Otherwise there could be memory tied up
 * for long periods (or for ever) due to load changes.
 */
static void reap_timer_fnc(unsigned long cpu)
{
	struct timer_list *rt = &__get_cpu_var(reap_timers);

	/* CPU hotplug can drag us off cpu: don't run on wrong CPU */
	if (!cpu_is_offline(cpu)) {
		cache_reap();
		mod_timer(rt, jiffies + REAPTIMEOUT_CPUC + cpu);
	}
}

#ifdef CONFIG_PROC_FS

static void *s_start(struct seq_file *m, loff_t *pos)
{
	loff_t n = *pos;
	struct list_head *p;

	down(&cache_chain_sem);
	if (!n) {
		/*
		 * Output format version, so at least we can change it
		 * without _too_ many complaints.
		 */
#if STATS
		seq_puts(m, "slabinfo - version: 2.0 (statistics)\n");
#else
		seq_puts(m, "slabinfo - version: 2.0\n");
#endif
		seq_puts(m, "# name            <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab>");
		seq_puts(m, " : tunables <batchcount> <limit> <sharedfactor>");
		seq_puts(m, " : slabdata <active_slabs> <num_slabs> <sharedavail>");
#if STATS
		seq_puts(m, " : globalstat <listallocs> <maxobjs> <grown> <reaped> <error> <maxfreeable> <freelimit>");
		seq_puts(m, " : cpustat <allochit> <allocmiss> <freehit> <freemiss>");
#endif
		seq_putc(m, '\n');
	}
	p = cache_chain.next;
	while (n--) {
		p = p->next;
		if (p == &cache_chain)
			return NULL;
	}
	return list_entry(p, kmem_cache_t, next);
}

static void *s_next(struct seq_file *m, void *p, loff_t *pos)
{
	kmem_cache_t *cachep = p;
	++*pos;
	return cachep->next.next == &cache_chain ? NULL
		: list_entry(cachep->next.next, kmem_cache_t, next);
}

static void s_stop(struct seq_file *m, void *p)
{
	up(&cache_chain_sem);
}

static int s_show(struct seq_file *m, void *p)
{
	kmem_cache_t *cachep = p;
	struct list_head *q;
	struct slab	*slabp;
	unsigned long	active_objs;
	unsigned long	num_objs;
	unsigned long	active_slabs = 0;
	unsigned long	num_slabs;
	const char *name; 
	char *error = NULL;

	check_irq_on();
	spin_lock_irq(&cachep->spinlock);
	active_objs = 0;
	num_slabs = 0;
	list_for_each(q,&cachep->lists.slabs_full) {
		slabp = list_entry(q, struct slab, list);
		if (slabp->inuse != cachep->num && !error)
			error = "slabs_full accounting error";
		active_objs += cachep->num;
		active_slabs++;
	}
	list_for_each(q,&cachep->lists.slabs_partial) {
		slabp = list_entry(q, struct slab, list);
		if (slabp->inuse == cachep->num && !error)
			error = "slabs_partial inuse accounting error";
		if (!slabp->inuse && !error)
			error = "slabs_partial/inuse accounting error";
		active_objs += slabp->inuse;
		active_slabs++;
	}
	list_for_each(q,&cachep->lists.slabs_free) {
		slabp = list_entry(q, struct slab, list);
		if (slabp->inuse && !error)
			error = "slabs_free/inuse accounting error";
		num_slabs++;
	}
	num_slabs+=active_slabs;
	num_objs = num_slabs*cachep->num;
	if (num_objs - active_objs != cachep->lists.free_objects && !error)
		error = "free_objects accounting error";

	name = cachep->name; 
	if (error)
		printk(KERN_ERR "slab: cache %s error: %s\n", name, error);

	seq_printf(m, "%-17s %6lu %6lu %6u %4u %4d",
		name, active_objs, num_objs, cachep->objsize,
		cachep->num, (1<<cachep->gfporder));
	seq_printf(m, " : tunables %4u %4u %4u",
			cachep->limit, cachep->batchcount,
			cachep->lists.shared->limit/cachep->batchcount);
	seq_printf(m, " : slabdata %6lu %6lu %6u",
			active_slabs, num_slabs, cachep->lists.shared->avail);
#if STATS
	{	/* list3 stats */
		unsigned long high = cachep->high_mark;
		unsigned long allocs = cachep->num_allocations;
		unsigned long grown = cachep->grown;
		unsigned long reaped = cachep->reaped;
		unsigned long errors = cachep->errors;
		unsigned long max_freeable = cachep->max_freeable;
		unsigned long free_limit = cachep->free_limit;

		seq_printf(m, " : globalstat %7lu %6lu %5lu %4lu %4lu %4lu %4lu",
				allocs, high, grown, reaped, errors, 
				max_freeable, free_limit);
	}
	/* cpu stats */
	{
		unsigned long allochit = atomic_read(&cachep->allochit);
		unsigned long allocmiss = atomic_read(&cachep->allocmiss);
		unsigned long freehit = atomic_read(&cachep->freehit);
		unsigned long freemiss = atomic_read(&cachep->freemiss);

		seq_printf(m, " : cpustat %6lu %6lu %6lu %6lu",
			allochit, allocmiss, freehit, freemiss);
	}
#endif
	seq_putc(m, '\n');
	spin_unlock_irq(&cachep->spinlock);
	return 0;
}

/*
 * slabinfo_op - iterator that generates /proc/slabinfo
 *
 * Output layout:
 * cache-name
 * num-active-objs
 * total-objs
 * object size
 * num-active-slabs
 * total-slabs
 * num-pages-per-slab
 * + further values on SMP and with statistics enabled
 */

struct seq_operations slabinfo_op = {
	.start	= s_start,
	.next	= s_next,
	.stop	= s_stop,
	.show	= s_show,
};

#define MAX_SLABINFO_WRITE 128
/**
 * slabinfo_write - Tuning for the slab allocator
 * @file: unused
 * @buffer: user buffer
 * @count: data length
 * @ppos: unused
 */
ssize_t slabinfo_write(struct file *file, const char __user *buffer,
				size_t count, loff_t *ppos)
{
	char kbuf[MAX_SLABINFO_WRITE+1], *tmp;
	int limit, batchcount, shared, res;
	struct list_head *p;
	
	if (count > MAX_SLABINFO_WRITE)
		return -EINVAL;
	if (copy_from_user(&kbuf, buffer, count))
		return -EFAULT;
	kbuf[MAX_SLABINFO_WRITE] = '\0'; 

	tmp = strchr(kbuf, ' ');
	if (!tmp)
		return -EINVAL;
	*tmp = '\0';
	tmp++;
	if (sscanf(tmp, " %d %d %d", &limit, &batchcount, &shared) != 3)
		return -EINVAL;

	/* Find the cache in the chain of caches. */
	down(&cache_chain_sem);
	res = -EINVAL;
	list_for_each(p,&cache_chain) {
		kmem_cache_t *cachep = list_entry(p, kmem_cache_t, next);

		if (!strcmp(cachep->name, kbuf)) {
			if (limit < 1 ||
			    batchcount < 1 ||
			    batchcount > limit ||
			    shared < 0) {
				res = -EINVAL;
			} else {
				res = do_tune_cpucache(cachep, limit, batchcount, shared);
			}
			break;
		}
	}
	up(&cache_chain_sem);
	if (res >= 0)
		res = count;
	return res;
}
#endif

unsigned int ksize(const void *objp)
{
	kmem_cache_t *c;
	unsigned long flags;
	unsigned int size = 0;

	if (likely(objp != NULL)) {
		local_irq_save(flags);
		c = GET_PAGE_CACHE(virt_to_page(objp));
		size = kmem_cache_size(c);
		local_irq_restore(flags);
	}

	return size;
}

void ptrinfo(unsigned long addr)
{
	struct page *page;

	printk("Dumping data about address %p.\n", (void*)addr);
	if (!virt_addr_valid((void*)addr)) {
		printk("virt addr invalid.\n");
		return;
	}
#ifdef CONFIG_MMU
	do {
		pgd_t *pgd = pgd_offset_k(addr);
		pmd_t *pmd;
		if (pgd_none(*pgd)) {
			printk("No pgd.\n");
			break;
		}
		pmd = pmd_offset(pgd, addr);
		if (pmd_none(*pmd)) {
			printk("No pmd.\n");
			break;
		}
#ifdef CONFIG_X86
		if (pmd_large(*pmd)) {
			printk("Large page.\n");
			break;
		}
#endif
		printk("normal page, pte_val 0x%llx\n",
		  (unsigned long long)pte_val(*pte_offset_kernel(pmd, addr)));
	} while(0);
#endif

	page = virt_to_page((void*)addr);
	printk("struct page at %p, flags %08lx\n",
			page, (unsigned long)page->flags);
	if (PageSlab(page)) {
		kmem_cache_t *c;
		struct slab *s;
		unsigned long flags;
		int objnr;
		void *objp;

		c = GET_PAGE_CACHE(page);
		printk("belongs to cache %s.\n",c->name);

		spin_lock_irqsave(&c->spinlock, flags);
		s = GET_PAGE_SLAB(page);
		printk("slabp %p with %d inuse objects (from %d).\n",
			s, s->inuse, c->num);
		check_slabp(c,s);

		objnr = (addr-(unsigned long)s->s_mem)/c->objsize;
		objp = s->s_mem+c->objsize*objnr;
		printk("points into object no %d, starting at %p, len %d.\n",
			objnr, objp, c->objsize);
		if (objnr >= c->num) {
			printk("Bad obj number.\n");
		} else {
			kernel_map_pages(virt_to_page(objp),
					c->objsize/PAGE_SIZE, 1);

			print_objinfo(c, objp, 2);
		}
		spin_unlock_irqrestore(&c->spinlock, flags);

	}
}
