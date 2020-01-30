/*
 * Copyright (C) 2001 Momchil Velikov
 * Portions Copyright (C) 2001 Christoph Hellwig
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/radix-tree.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/gfp.h>
#include <linux/string.h>
#include <linux/bitops.h>


#ifdef __KERNEL__
#define RADIX_TREE_MAP_SHIFT	6
#else
#define RADIX_TREE_MAP_SHIFT	3	/* For more stressful testing */
#endif
#define RADIX_TREE_TAGS		2

#define RADIX_TREE_MAP_SIZE	(1UL << RADIX_TREE_MAP_SHIFT)
#define RADIX_TREE_MAP_MASK	(RADIX_TREE_MAP_SIZE-1)

#define RADIX_TREE_TAG_LONGS	\
	((RADIX_TREE_MAP_SIZE + BITS_PER_LONG - 1) / BITS_PER_LONG)

struct radix_tree_node {
	unsigned int	count;
	void		*slots[RADIX_TREE_MAP_SIZE];
	unsigned long	tags[RADIX_TREE_TAGS][RADIX_TREE_TAG_LONGS];
};

struct radix_tree_path {
	struct radix_tree_node *node, **slot;
	int offset;
};

#define RADIX_TREE_INDEX_BITS  (8 /* CHAR_BIT */ * sizeof(unsigned long))
#define RADIX_TREE_MAX_PATH (RADIX_TREE_INDEX_BITS/RADIX_TREE_MAP_SHIFT + 2)

static unsigned long height_to_maxindex[RADIX_TREE_MAX_PATH];

/*
 * Radix tree node cache.
 */
static kmem_cache_t *radix_tree_node_cachep;

/*
 * Per-cpu pool of preloaded nodes
 */
struct radix_tree_preload {
	int nr;
	struct radix_tree_node *nodes[RADIX_TREE_MAX_PATH];
};
DEFINE_PER_CPU(struct radix_tree_preload, radix_tree_preloads) = { 0, };

/*
 * This assumes that the caller has performed appropriate preallocation, and
 * that the caller has pinned this thread of control to the current CPU.
 */
static struct radix_tree_node *
radix_tree_node_alloc(struct radix_tree_root *root)
{
	struct radix_tree_node *ret;

	ret = kmem_cache_alloc(radix_tree_node_cachep, root->gfp_mask);
	if (ret == NULL && !(root->gfp_mask & __GFP_WAIT)) {
		struct radix_tree_preload *rtp;

		rtp = &__get_cpu_var(radix_tree_preloads);
		if (rtp->nr) {
			ret = rtp->nodes[rtp->nr - 1];
			rtp->nodes[rtp->nr - 1] = NULL;
			rtp->nr--;
		}
	}
	return ret;
}

static inline void
radix_tree_node_free(struct radix_tree_node *node)
{
	kmem_cache_free(radix_tree_node_cachep, node);
}

/*
 * Load up this CPU's radix_tree_node buffer with sufficient objects to
 * ensure that the addition of a single element in the tree cannot fail.  On
 * success, return zero, with preemption disabled.  On error, return -ENOMEM
 * with preemption not disabled.
 */
int radix_tree_preload(int gfp_mask)
{
	struct radix_tree_preload *rtp;
	struct radix_tree_node *node;
	int ret = -ENOMEM;

	preempt_disable();
	rtp = &__get_cpu_var(radix_tree_preloads);
	while (rtp->nr < ARRAY_SIZE(rtp->nodes)) {
		preempt_enable();
		node = kmem_cache_alloc(radix_tree_node_cachep, gfp_mask);
		if (node == NULL)
			goto out;
		preempt_disable();
		rtp = &__get_cpu_var(radix_tree_preloads);
		if (rtp->nr < ARRAY_SIZE(rtp->nodes))
			rtp->nodes[rtp->nr++] = node;
		else
			kmem_cache_free(radix_tree_node_cachep, node);
	}
	ret = 0;
out:
	return ret;
}

static inline void tag_set(struct radix_tree_node *node, int tag, int offset)
{
	if (!test_bit(offset, &node->tags[tag][0]))
		__set_bit(offset, &node->tags[tag][0]);
}

static inline void tag_clear(struct radix_tree_node *node, int tag, int offset)
{
	__clear_bit(offset, &node->tags[tag][0]);
}

static inline int tag_get(struct radix_tree_node *node, int tag, int offset)
{
	return test_bit(offset, &node->tags[tag][0]);
}

/*
 *	Return the maximum key which can be store into a
 *	radix tree with height HEIGHT.
 */
static inline unsigned long radix_tree_maxindex(unsigned int height)
{
	return height_to_maxindex[height];
}

/*
 *	Extend a radix tree so it can store key @index.
 */
static int radix_tree_extend(struct radix_tree_root *root, unsigned long index)
{
	struct radix_tree_node *node;
	unsigned int height;
	char tags[RADIX_TREE_TAGS];
	int tag;

	/* Figure out what the height should be.  */
	height = root->height + 1;
	while (index > radix_tree_maxindex(height))
		height++;

	if (root->rnode == NULL) {
		root->height = height;
		goto out;
	}

	/*
	 * Prepare the tag status of the top-level node for propagation
	 * into the newly-pushed top-level node(s)
	 */
	for (tag = 0; tag < RADIX_TREE_TAGS; tag++) {
		int idx;

		tags[tag] = 0;
		for (idx = 0; idx < RADIX_TREE_TAG_LONGS; idx++) {
			if (root->rnode->tags[tag][idx]) {
				tags[tag] = 1;
				break;
			}
		}
	}

	do {
		if (!(node = radix_tree_node_alloc(root)))
			return -ENOMEM;

		/* Increase the height.  */
		node->slots[0] = root->rnode;

		/* Propagate the aggregated tag info into the new root */
		for (tag = 0; tag < RADIX_TREE_TAGS; tag++) {
			if (tags[tag])
				tag_set(node, tag, 0);
		}

		node->count = 1;
		root->rnode = node;
		root->height++;
	} while (height > root->height);
out:
	return 0;
}

/**
 *	radix_tree_insert    -    insert into a radix tree
 *	@root:		radix tree root
 *	@index:		index key
 *	@item:		item to insert
 *
 *	Insert an item into the radix tree at position @index.
 */
int radix_tree_insert(struct radix_tree_root *root,
			unsigned long index, void *item)
{
	struct radix_tree_node *node = NULL, *tmp, **slot;
	unsigned int height, shift;
	int offset;
	int error;

	/* Make sure the tree is high enough.  */
	if ((!index && !root->rnode) ||
			index > radix_tree_maxindex(root->height)) {
		error = radix_tree_extend(root, index);
		if (error)
			return error;
	}

	slot = &root->rnode;
	height = root->height;
	shift = (height-1) * RADIX_TREE_MAP_SHIFT;

	offset = 0;			/* uninitialised var warning */
	while (height > 0) {
		if (*slot == NULL) {
			/* Have to add a child node.  */
			if (!(tmp = radix_tree_node_alloc(root)))
				return -ENOMEM;
			*slot = tmp;
			if (node)
				node->count++;
		}

		/* Go a level down */
		offset = (index >> shift) & RADIX_TREE_MAP_MASK;
		node = *slot;
		slot = (struct radix_tree_node **)(node->slots + offset);
		shift -= RADIX_TREE_MAP_SHIFT;
		height--;
	}

	if (*slot != NULL)
		return -EEXIST;
	if (node) {
		node->count++;
		BUG_ON(tag_get(node, 0, offset));
		BUG_ON(tag_get(node, 1, offset));
	}

	*slot = item;
	return 0;
}
EXPORT_SYMBOL(radix_tree_insert);

/**
 *	radix_tree_lookup    -    perform lookup operation on a radix tree
 *	@root:		radix tree root
 *	@index:		index key
 *
 *	Lookup the item at the position @index in the radix tree @root.
 */
void *radix_tree_lookup(struct radix_tree_root *root, unsigned long index)
{
	unsigned int height, shift;
	struct radix_tree_node **slot;

	height = root->height;
	if (index > radix_tree_maxindex(height))
		return NULL;

	shift = (height-1) * RADIX_TREE_MAP_SHIFT;
	slot = &root->rnode;

	while (height > 0) {
		if (*slot == NULL)
			return NULL;

		slot = (struct radix_tree_node **)
			((*slot)->slots +
				((index >> shift) & RADIX_TREE_MAP_MASK));
		shift -= RADIX_TREE_MAP_SHIFT;
		height--;
	}

	return *slot;
}
EXPORT_SYMBOL(radix_tree_lookup);

/**
 *	radix_tree_tag_set - set a tag on a radix tree node
 *	@root:		radix tree root
 *	@index:		index key
 *	@tag: 		tag index
 *
 *	Set the search tag corresponging to @index in the radix tree.  From
 *	the root all the way down to the leaf node.
 *
 *	Returns the address of the tagged item.   Setting a tag on a not-present
 *	item is a bug.
 */
void *radix_tree_tag_set(struct radix_tree_root *root,
			unsigned long index, int tag)
{
	unsigned int height, shift;
	struct radix_tree_node **slot;

	height = root->height;
	if (index > radix_tree_maxindex(height))
		return NULL;

	shift = (height - 1) * RADIX_TREE_MAP_SHIFT;
	slot = &root->rnode;

	while (height > 0) {
		int offset;

		offset = (index >> shift) & RADIX_TREE_MAP_MASK;
		tag_set(*slot, tag, offset);
		slot = (struct radix_tree_node **)((*slot)->slots + offset);
		BUG_ON(*slot == NULL);
		shift -= RADIX_TREE_MAP_SHIFT;
		height--;
	}

	return *slot;
}
EXPORT_SYMBOL(radix_tree_tag_set);

/**
 *	radix_tree_tag_clear - clear a tag on a radix tree node
 *	@root:		radix tree root
 *	@index:		index key
 *	@tag: 		tag index
 *
 *	Clear the search tag corresponging to @index in the radix tree.  If
 *	this causes the leaf node to have no tags set then clear the tag in the
 *	next-to-leaf node, etc.
 *
 *	Returns the address of the tagged item on success, else NULL.  ie:
 *	has the same return value and semantics as radix_tree_lookup().
 */
void *radix_tree_tag_clear(struct radix_tree_root *root,
			unsigned long index, int tag)
{
	struct radix_tree_path path[RADIX_TREE_MAX_PATH], *pathp = path;
	unsigned int height, shift;
	void *ret = NULL;

	height = root->height;
	if (index > radix_tree_maxindex(height))
		goto out;

	shift = (height - 1) * RADIX_TREE_MAP_SHIFT;
	pathp->node = NULL;
	pathp->slot = &root->rnode;

	while (height > 0) {
		int offset;

		if (*pathp->slot == NULL)
			goto out;

		offset = (index >> shift) & RADIX_TREE_MAP_MASK;
		pathp[1].offset = offset;
		pathp[1].node = *pathp[0].slot;
		pathp[1].slot = (struct radix_tree_node **)
				(pathp[1].node->slots + offset);
		pathp++;
		shift -= RADIX_TREE_MAP_SHIFT;
		height--;
	}

	ret = *pathp[0].slot;
	if (ret == NULL)
		goto out;

	do {
		int idx;

		tag_clear(pathp[0].node, tag, pathp[0].offset);
		for (idx = 0; idx < RADIX_TREE_TAG_LONGS; idx++) {
			if (pathp[0].node->tags[tag][idx])
				goto out;
		}
		pathp--;
	} while (pathp[0].node);
out:
	return ret;
}
EXPORT_SYMBOL(radix_tree_tag_clear);

#ifndef __KERNEL__	/* Only the test harness uses this at present */
/**
 *	radix_tree_tag_get - get a tag on a radix tree node
 *	@root:		radix tree root
 *	@index:		index key
 *	@tag: 		tag index
 *
 *	Return the search tag corresponging to @index in the radix tree.
 *
 *	Returns zero if the tag is unset, or if there is no corresponding item
 *	in the tree.
 */
int radix_tree_tag_get(struct radix_tree_root *root,
			unsigned long index, int tag)
{
	unsigned int height, shift;
	struct radix_tree_node **slot;
	int saw_unset_tag = 0;

	height = root->height;
	if (index > radix_tree_maxindex(height))
		return 0;

	shift = (height - 1) * RADIX_TREE_MAP_SHIFT;
	slot = &root->rnode;

	for ( ; ; ) {
		int offset;

		if (*slot == NULL)
			return 0;

		offset = (index >> shift) & RADIX_TREE_MAP_MASK;

		/*
		 * This is just a debug check.  Later, we can bale as soon as
		 * we see an unset tag.
		 */
		if (!tag_get(*slot, tag, offset))
			saw_unset_tag = 1;
		if (height == 1) {
			int ret = tag_get(*slot, tag, offset);

			BUG_ON(ret && saw_unset_tag);
			return ret;
		}
		slot = (struct radix_tree_node **)((*slot)->slots + offset);
		shift -= RADIX_TREE_MAP_SHIFT;
		height--;
	}
}
EXPORT_SYMBOL(radix_tree_tag_get);
#endif

static unsigned int
__lookup(struct radix_tree_root *root, void **results, unsigned long index,
	unsigned int max_items, unsigned long *next_index)
{
	unsigned int nr_found = 0;
	unsigned int shift;
	unsigned int height = root->height;
	struct radix_tree_node *slot;

	shift = (height-1) * RADIX_TREE_MAP_SHIFT;
	slot = root->rnode;

	while (height > 0) {
		unsigned long i = (index >> shift) & RADIX_TREE_MAP_MASK;

		for ( ; i < RADIX_TREE_MAP_SIZE; i++) {
			if (slot->slots[i] != NULL)
				break;
			index &= ~((1UL << shift) - 1);
			index += 1UL << shift;
			if (index == 0)
				goto out;	/* 32-bit wraparound */
		}
		if (i == RADIX_TREE_MAP_SIZE)
			goto out;
		height--;
		if (height == 0) {	/* Bottom level: grab some items */
			unsigned long j = index & RADIX_TREE_MAP_MASK;

			for ( ; j < RADIX_TREE_MAP_SIZE; j++) {
				index++;
				if (slot->slots[j]) {
					results[nr_found++] = slot->slots[j];
					if (nr_found == max_items)
						goto out;
				}
			}
		}
		shift -= RADIX_TREE_MAP_SHIFT;
		slot = slot->slots[i];
	}
out:
	*next_index = index;
	return nr_found;
}

/**
 *	radix_tree_gang_lookup - perform multiple lookup on a radix tree
 *	@root:		radix tree root
 *	@results:	where the results of the lookup are placed
 *	@first_index:	start the lookup from this key
 *	@max_items:	place up to this many items at *results
 *
 *	Performs an index-ascending scan of the tree for present items.  Places
 *	them at *@results and returns the number of items which were placed at
 *	*@results.
 *
 *	The implementation is naive.
 */
unsigned int
radix_tree_gang_lookup(struct radix_tree_root *root, void **results,
			unsigned long first_index, unsigned int max_items)
{
	const unsigned long max_index = radix_tree_maxindex(root->height);
	unsigned long cur_index = first_index;
	unsigned int ret = 0;

	while (ret < max_items) {
		unsigned int nr_found;
		unsigned long next_index;	/* Index of next search */

		if (cur_index > max_index)
			break;
		nr_found = __lookup(root, results + ret, cur_index,
					max_items - ret, &next_index);
		ret += nr_found;
		if (next_index == 0)
			break;
		cur_index = next_index;
	}
	return ret;
}
EXPORT_SYMBOL(radix_tree_gang_lookup);

/*
 * FIXME: the two tag_get()s here should use find_next_bit() instead of
 * open-coding the search.
 */
static unsigned int
__lookup_tag(struct radix_tree_root *root, void **results, unsigned long index,
	unsigned int max_items, unsigned long *next_index, int tag)
{
	unsigned int nr_found = 0;
	unsigned int shift;
	unsigned int height = root->height;
	struct radix_tree_node *slot;

	shift = (height - 1) * RADIX_TREE_MAP_SHIFT;
	slot = root->rnode;

	while (height > 0) {
		unsigned long i = (index >> shift) & RADIX_TREE_MAP_MASK;

		for ( ; i < RADIX_TREE_MAP_SIZE; i++) {
			if (tag_get(slot, tag, i)) {
				BUG_ON(slot->slots[i] == NULL);
				break;
			}
			index &= ~((1UL << shift) - 1);
			index += 1UL << shift;
			if (index == 0)
				goto out;	/* 32-bit wraparound */
		}
		if (i == RADIX_TREE_MAP_SIZE)
			goto out;
		height--;
		if (height == 0) {	/* Bottom level: grab some items */
			unsigned long j = index & RADIX_TREE_MAP_MASK;

			for ( ; j < RADIX_TREE_MAP_SIZE; j++) {
				index++;
				if (tag_get(slot, tag, j)) {
					BUG_ON(slot->slots[j] == NULL);
					results[nr_found++] = slot->slots[j];
					if (nr_found == max_items)
						goto out;
				}
			}
		}
		shift -= RADIX_TREE_MAP_SHIFT;
		slot = slot->slots[i];
	}
out:
	*next_index = index;
	return nr_found;
}

/**
 *	radix_tree_gang_lookup_tag - perform multiple lookup on a radix tree
 *	                             based on a tag
 *	@root:		radix tree root
 *	@results:	where the results of the lookup are placed
 *	@first_index:	start the lookup from this key
 *	@max_items:	place up to this many items at *results
 *	@tag:		the tag index
 *
 *	Performs an index-ascending scan of the tree for present items which
 *	have the tag indexed by @tag set.  Places the items at *@results and
 *	returns the number of items which were placed at *@results.
 */
unsigned int
radix_tree_gang_lookup_tag(struct radix_tree_root *root, void **results,
		unsigned long first_index, unsigned int max_items, int tag)
{
	const unsigned long max_index = radix_tree_maxindex(root->height);
	unsigned long cur_index = first_index;
	unsigned int ret = 0;

	while (ret < max_items) {
		unsigned int nr_found;
		unsigned long next_index;	/* Index of next search */

		if (cur_index > max_index)
			break;
		nr_found = __lookup_tag(root, results + ret, cur_index,
					max_items - ret, &next_index, tag);
		ret += nr_found;
		if (next_index == 0)
			break;
		cur_index = next_index;
	}
	return ret;
}
EXPORT_SYMBOL(radix_tree_gang_lookup_tag);

/**
 *	radix_tree_delete    -    delete an item from a radix tree
 *	@root:		radix tree root
 *	@index:		index key
 *
 *	Remove the item at @index from the radix tree rooted at @root.
 *
 *	Returns the address of the deleted item, or NULL if it was not present.
 */
void *radix_tree_delete(struct radix_tree_root *root, unsigned long index)
{
	struct radix_tree_path path[RADIX_TREE_MAX_PATH], *pathp = path;
	struct radix_tree_path *orig_pathp;
	unsigned int height, shift;
	void *ret = NULL;
	char tags[RADIX_TREE_TAGS];
	int nr_cleared_tags;

	height = root->height;
	if (index > radix_tree_maxindex(height))
		goto out;

	shift = (height - 1) * RADIX_TREE_MAP_SHIFT;
	pathp->node = NULL;
	pathp->slot = &root->rnode;

	while (height > 0) {
		int offset;

		if (*pathp->slot == NULL)
			goto out;

		offset = (index >> shift) & RADIX_TREE_MAP_MASK;
		pathp[1].offset = offset;
		pathp[1].node = *pathp[0].slot;
		pathp[1].slot = (struct radix_tree_node **)
				(pathp[1].node->slots + offset);
		pathp++;
		shift -= RADIX_TREE_MAP_SHIFT;
		height--;
	}

	ret = *pathp[0].slot;
	if (ret == NULL)
		goto out;

	orig_pathp = pathp;

	/*
	 * Clear all tags associated with the just-deleted item
	 */
	memset(tags, 0, sizeof(tags));
	do {
		int tag;

		nr_cleared_tags = RADIX_TREE_TAGS;
		for (tag = 0; tag < RADIX_TREE_TAGS; tag++) {
			int idx;

			if (!tags[tag])
				tag_clear(pathp[0].node, tag, pathp[0].offset);

			for (idx = 0; idx < RADIX_TREE_TAG_LONGS; idx++) {
				if (pathp[0].node->tags[tag][idx]) {
					tags[tag] = 1;
					nr_cleared_tags--;
					break;
				}
			}
		}
		pathp--;
	} while (pathp[0].node && nr_cleared_tags);

	pathp = orig_pathp;
	*pathp[0].slot = NULL;
	while (pathp[0].node && --pathp[0].node->count == 0) {
		pathp--;
		BUG_ON(*pathp[0].slot == NULL);
		*pathp[0].slot = NULL;
		radix_tree_node_free(pathp[1].node);
	}
	if (root->rnode == NULL)
		root->height = 0;
out:
	return ret;
}
EXPORT_SYMBOL(radix_tree_delete);

/**
 *	radix_tree_tagged - test whether any items in the tree are tagged
 *	@root:		radix tree root
 *	@tag:		tag to test
 */
int radix_tree_tagged(struct radix_tree_root *root, int tag)
{
	int idx;

	if (!root->rnode)
		return 0;
	for (idx = 0; idx < RADIX_TREE_TAG_LONGS; idx++) {
		if (root->rnode->tags[tag][idx])
			return 1;
	}
	return 0;
}
EXPORT_SYMBOL(radix_tree_tagged);

static void
radix_tree_node_ctor(void *node, kmem_cache_t *cachep, unsigned long flags)
{
	memset(node, 0, sizeof(struct radix_tree_node));
}

static __init unsigned long __maxindex(unsigned int height)
{
	unsigned int tmp = height * RADIX_TREE_MAP_SHIFT;
	unsigned long index = (~0UL >> (RADIX_TREE_INDEX_BITS - tmp - 1)) >> 1;

	if (tmp >= RADIX_TREE_INDEX_BITS)
		index = ~0UL;
	return index;
}

static __init void radix_tree_init_maxindex(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(height_to_maxindex); i++)
		height_to_maxindex[i] = __maxindex(i);
}

#ifdef CONFIG_HOTPLUG_CPU
static int radix_tree_callback(struct notifier_block *nfb,
                            unsigned long action,
                            void *hcpu)
{
       int cpu = (long)hcpu;
       struct radix_tree_preload *rtp;

       /* Free per-cpu pool of perloaded nodes */
       if (action == CPU_DEAD) {
               rtp = &per_cpu(radix_tree_preloads, cpu);
               while (rtp->nr) {
                       kmem_cache_free(radix_tree_node_cachep,
                                       rtp->nodes[rtp->nr-1]);
                       rtp->nodes[rtp->nr-1] = NULL;
                       rtp->nr--;
               }
       }
       return NOTIFY_OK;
}
#endif /* CONFIG_HOTPLUG_CPU */

void __init radix_tree_init(void)
{
	radix_tree_node_cachep = kmem_cache_create("radix_tree_node",
			sizeof(struct radix_tree_node), 0,
			SLAB_PANIC, radix_tree_node_ctor, NULL);
	radix_tree_init_maxindex();
	hotcpu_notifier(radix_tree_callback, 0);
}
