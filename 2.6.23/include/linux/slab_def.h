#ifndef _LINUX_SLAB_DEF_H
#define	_LINUX_SLAB_DEF_H

/*
 * Definitions unique to the original Linux SLAB allocator.
 *
 * What we provide here is a way to optimize the frequent kmalloc
 * calls in the kernel by selecting the appropriate general cache
 * if kmalloc was called with a size that can be established at
 * compile time.
 */

#include <linux/init.h>
#include <asm/page.h>		/* kmalloc_sizes.h needs PAGE_SIZE */
#include <asm/cache.h>		/* kmalloc_sizes.h needs L1_CACHE_BYTES */
#include <linux/compiler.h>

/* Size description struct for general caches. */
struct cache_sizes {
	size_t		 	cs_size;
	struct kmem_cache	*cs_cachep;
#ifdef CONFIG_ZONE_DMA
	struct kmem_cache	*cs_dmacachep;
#endif
};
extern struct cache_sizes malloc_sizes[];

void *kmem_cache_alloc(struct kmem_cache *, gfp_t);
void *__kmalloc(size_t size, gfp_t flags);

static inline void *kmalloc(size_t size, gfp_t flags)
{
	if (__builtin_constant_p(size)) {
		int i = 0;

		if (!size)
			return ZERO_SIZE_PTR;

#define CACHE(x) \
		if (size <= x) \
			goto found; \
		else \
			i++;
#include "kmalloc_sizes.h"
#undef CACHE
		{
			extern void __you_cannot_kmalloc_that_much(void);
			__you_cannot_kmalloc_that_much();
		}
found:
#ifdef CONFIG_ZONE_DMA
		if (flags & GFP_DMA)
			return kmem_cache_alloc(malloc_sizes[i].cs_dmacachep,
						flags);
#endif
		return kmem_cache_alloc(malloc_sizes[i].cs_cachep, flags);
	}
	return __kmalloc(size, flags);
}

#ifdef CONFIG_NUMA
extern void *__kmalloc_node(size_t size, gfp_t flags, int node);
extern void *kmem_cache_alloc_node(struct kmem_cache *, gfp_t flags, int node);

static inline void *kmalloc_node(size_t size, gfp_t flags, int node)
{
	if (__builtin_constant_p(size)) {
		int i = 0;

		if (!size)
			return ZERO_SIZE_PTR;

#define CACHE(x) \
		if (size <= x) \
			goto found; \
		else \
			i++;
#include "kmalloc_sizes.h"
#undef CACHE
		{
			extern void __you_cannot_kmalloc_that_much(void);
			__you_cannot_kmalloc_that_much();
		}
found:
#ifdef CONFIG_ZONE_DMA
		if (flags & GFP_DMA)
			return kmem_cache_alloc_node(malloc_sizes[i].cs_dmacachep,
						flags, node);
#endif
		return kmem_cache_alloc_node(malloc_sizes[i].cs_cachep,
						flags, node);
	}
	return __kmalloc_node(size, flags, node);
}

#endif	/* CONFIG_NUMA */

extern const struct seq_operations slabinfo_op;
ssize_t slabinfo_write(struct file *, const char __user *, size_t, loff_t *);

#endif	/* _LINUX_SLAB_DEF_H */
