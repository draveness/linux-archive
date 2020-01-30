#ifndef _LINUX_RMAP_H
#define _LINUX_RMAP_H
/*
 * Declarations for Reverse Mapping functions in mm/rmap.c
 */

#include <linux/config.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define page_map_lock(page) \
	bit_spin_lock(PG_maplock, (unsigned long *)&(page)->flags)
#define page_map_unlock(page) \
	bit_spin_unlock(PG_maplock, (unsigned long *)&(page)->flags)

/*
 * The anon_vma heads a list of private "related" vmas, to scan if
 * an anonymous page pointing to this anon_vma needs to be unmapped:
 * the vmas on the list will be related by forking, or by splitting.
 *
 * Since vmas come and go as they are split and merged (particularly
 * in mprotect), the mapping field of an anonymous page cannot point
 * directly to a vma: instead it points to an anon_vma, on whose list
 * the related vmas can be easily linked or unlinked.
 *
 * After unlinking the last vma on the list, we must garbage collect
 * the anon_vma object itself: we're guaranteed no page can be
 * pointing to this anon_vma once its vma list is empty.
 */
struct anon_vma {
	spinlock_t lock;	/* Serialize access to vma list */
	struct list_head head;	/* List of private "related" vmas */
};

#ifdef CONFIG_MMU

extern kmem_cache_t *anon_vma_cachep;

static inline struct anon_vma *anon_vma_alloc(void)
{
	return kmem_cache_alloc(anon_vma_cachep, SLAB_KERNEL);
}

static inline void anon_vma_free(struct anon_vma *anon_vma)
{
	kmem_cache_free(anon_vma_cachep, anon_vma);
}

static inline void anon_vma_lock(struct vm_area_struct *vma)
{
	struct anon_vma *anon_vma = vma->anon_vma;
	if (anon_vma)
		spin_lock(&anon_vma->lock);
}

static inline void anon_vma_unlock(struct vm_area_struct *vma)
{
	struct anon_vma *anon_vma = vma->anon_vma;
	if (anon_vma)
		spin_unlock(&anon_vma->lock);
}

/*
 * anon_vma helper functions.
 */
void anon_vma_init(void);	/* create anon_vma_cachep */
int  anon_vma_prepare(struct vm_area_struct *);
void __anon_vma_merge(struct vm_area_struct *, struct vm_area_struct *);
void anon_vma_unlink(struct vm_area_struct *);
void anon_vma_link(struct vm_area_struct *);
void __anon_vma_link(struct vm_area_struct *);

/*
 * rmap interfaces called when adding or removing pte of page
 */
void page_add_anon_rmap(struct page *, struct vm_area_struct *, unsigned long);
void page_add_file_rmap(struct page *);
void page_remove_rmap(struct page *);

/**
 * page_dup_rmap - duplicate pte mapping to a page
 * @page:	the page to add the mapping to
 *
 * For copy_page_range only: minimal extract from page_add_rmap,
 * avoiding unnecessary tests (already checked) so it's quicker.
 */
static inline void page_dup_rmap(struct page *page)
{
	page_map_lock(page);
	page->mapcount++;
	page_map_unlock(page);
}

/*
 * Called from mm/vmscan.c to handle paging out
 */
int page_referenced(struct page *);
int try_to_unmap(struct page *);

#else	/* !CONFIG_MMU */

#define anon_vma_init()		do {} while (0)
#define anon_vma_prepare(vma)	(0)
#define anon_vma_link(vma)	do {} while (0)

#define page_referenced(page)	TestClearPageReferenced(page)
#define try_to_unmap(page)	SWAP_FAIL

#endif	/* CONFIG_MMU */

/*
 * Return values of try_to_unmap
 */
#define SWAP_SUCCESS	0
#define SWAP_AGAIN	1
#define SWAP_FAIL	2

#endif	/* _LINUX_RMAP_H */
