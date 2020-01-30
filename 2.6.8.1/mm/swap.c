/*
 *  linux/mm/swap.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 */

/*
 * This file contains the default values for the opereation of the
 * Linux VM subsystem. Fine-tuning documentation can be found in
 * Documentation/sysctl/vm.txt.
 * Started 18.12.91
 * Swap aging added 23.2.95, Stephen Tweedie.
 * Buffermem limits added 12.3.98, Rik van Riel.
 */

#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/mman.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mm_inline.h>
#include <linux/buffer_head.h>	/* for try_to_release_page() */
#include <linux/module.h>
#include <linux/percpu_counter.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/init.h>

/* How many pages do we try to swap or page in/out together? */
int page_cluster;

#ifdef CONFIG_HUGETLB_PAGE

void put_page(struct page *page)
{
	if (unlikely(PageCompound(page))) {
		page = (struct page *)page->private;
		if (put_page_testzero(page)) {
			void (*dtor)(struct page *page);

			dtor = (void (*)(struct page *))page[1].mapping;
			(*dtor)(page);
		}
		return;
	}
	if (!PageReserved(page) && put_page_testzero(page))
		__page_cache_release(page);
}
EXPORT_SYMBOL(put_page);
#endif

/*
 * Writeback is about to end against a page which has been marked for immediate
 * reclaim.  If it still appears to be reclaimable, move it to the tail of the
 * inactive list.  The page still has PageWriteback set, which will pin it.
 *
 * We don't expect many pages to come through here, so don't bother batching
 * things up.
 *
 * To avoid placing the page at the tail of the LRU while PG_writeback is still
 * set, this function will clear PG_writeback before performing the page
 * motion.  Do that inside the lru lock because once PG_writeback is cleared
 * we may not touch the page.
 *
 * Returns zero if it cleared PG_writeback.
 */
int rotate_reclaimable_page(struct page *page)
{
	struct zone *zone;
	unsigned long flags;

	if (PageLocked(page))
		return 1;
	if (PageDirty(page))
		return 1;
	if (PageActive(page))
		return 1;
	if (!PageLRU(page))
		return 1;

	zone = page_zone(page);
	spin_lock_irqsave(&zone->lru_lock, flags);
	if (PageLRU(page) && !PageActive(page)) {
		list_del(&page->lru);
		list_add_tail(&page->lru, &zone->inactive_list);
		inc_page_state(pgrotated);
	}
	if (!test_clear_page_writeback(page))
		BUG();
	spin_unlock_irqrestore(&zone->lru_lock, flags);
	return 0;
}

/*
 * FIXME: speed this up?
 */
void fastcall activate_page(struct page *page)
{
	struct zone *zone = page_zone(page);

	spin_lock_irq(&zone->lru_lock);
	if (PageLRU(page) && !PageActive(page)) {
		del_page_from_inactive_list(zone, page);
		SetPageActive(page);
		add_page_to_active_list(zone, page);
		inc_page_state(pgactivate);
	}
	spin_unlock_irq(&zone->lru_lock);
}

/*
 * Mark a page as having seen activity.
 *
 * inactive,unreferenced	->	inactive,referenced
 * inactive,referenced		->	active,unreferenced
 * active,unreferenced		->	active,referenced
 */
void fastcall mark_page_accessed(struct page *page)
{
	if (!PageActive(page) && PageReferenced(page) && PageLRU(page)) {
		activate_page(page);
		ClearPageReferenced(page);
	} else if (!PageReferenced(page)) {
		SetPageReferenced(page);
	}
}

EXPORT_SYMBOL(mark_page_accessed);

/**
 * lru_cache_add: add a page to the page lists
 * @page: the page to add
 */
static DEFINE_PER_CPU(struct pagevec, lru_add_pvecs) = { 0, };
static DEFINE_PER_CPU(struct pagevec, lru_add_active_pvecs) = { 0, };

void fastcall lru_cache_add(struct page *page)
{
	struct pagevec *pvec = &get_cpu_var(lru_add_pvecs);

	page_cache_get(page);
	if (!pagevec_add(pvec, page))
		__pagevec_lru_add(pvec);
	put_cpu_var(lru_add_pvecs);
}

void fastcall lru_cache_add_active(struct page *page)
{
	struct pagevec *pvec = &get_cpu_var(lru_add_active_pvecs);

	page_cache_get(page);
	if (!pagevec_add(pvec, page))
		__pagevec_lru_add_active(pvec);
	put_cpu_var(lru_add_active_pvecs);
}

void lru_add_drain(void)
{
	struct pagevec *pvec = &get_cpu_var(lru_add_pvecs);

	if (pagevec_count(pvec))
		__pagevec_lru_add(pvec);
	pvec = &__get_cpu_var(lru_add_active_pvecs);
	if (pagevec_count(pvec))
		__pagevec_lru_add_active(pvec);
	put_cpu_var(lru_add_pvecs);
}

/*
 * This path almost never happens for VM activity - pages are normally
 * freed via pagevecs.  But it gets used by networking.
 */
void fastcall __page_cache_release(struct page *page)
{
	unsigned long flags;
	struct zone *zone = page_zone(page);

	spin_lock_irqsave(&zone->lru_lock, flags);
	if (TestClearPageLRU(page))
		del_page_from_lru(zone, page);
	if (page_count(page) != 0)
		page = NULL;
	spin_unlock_irqrestore(&zone->lru_lock, flags);
	if (page)
		free_hot_page(page);
}

EXPORT_SYMBOL(__page_cache_release);

/*
 * Batched page_cache_release().  Decrement the reference count on all the
 * passed pages.  If it fell to zero then remove the page from the LRU and
 * free it.
 *
 * Avoid taking zone->lru_lock if possible, but if it is taken, retain it
 * for the remainder of the operation.
 *
 * The locking in this function is against shrink_cache(): we recheck the
 * page count inside the lock to see whether shrink_cache grabbed the page
 * via the LRU.  If it did, give up: shrink_cache will free it.
 */
void release_pages(struct page **pages, int nr, int cold)
{
	int i;
	struct pagevec pages_to_free;
	struct zone *zone = NULL;

	pagevec_init(&pages_to_free, cold);
	for (i = 0; i < nr; i++) {
		struct page *page = pages[i];
		struct zone *pagezone;

		if (PageReserved(page) || !put_page_testzero(page))
			continue;

		pagezone = page_zone(page);
		if (pagezone != zone) {
			if (zone)
				spin_unlock_irq(&zone->lru_lock);
			zone = pagezone;
			spin_lock_irq(&zone->lru_lock);
		}
		if (TestClearPageLRU(page))
			del_page_from_lru(zone, page);
		if (page_count(page) == 0) {
			if (!pagevec_add(&pages_to_free, page)) {
				spin_unlock_irq(&zone->lru_lock);
				__pagevec_free(&pages_to_free);
				pagevec_reinit(&pages_to_free);
				zone = NULL;	/* No lock is held */
			}
		}
	}
	if (zone)
		spin_unlock_irq(&zone->lru_lock);

	pagevec_free(&pages_to_free);
}

/*
 * The pages which we're about to release may be in the deferred lru-addition
 * queues.  That would prevent them from really being freed right now.  That's
 * OK from a correctness point of view but is inefficient - those pages may be
 * cache-warm and we want to give them back to the page allocator ASAP.
 *
 * So __pagevec_release() will drain those queues here.  __pagevec_lru_add()
 * and __pagevec_lru_add_active() call release_pages() directly to avoid
 * mutual recursion.
 */
void __pagevec_release(struct pagevec *pvec)
{
	lru_add_drain();
	release_pages(pvec->pages, pagevec_count(pvec), pvec->cold);
	pagevec_reinit(pvec);
}

/*
 * pagevec_release() for pages which are known to not be on the LRU
 *
 * This function reinitialises the caller's pagevec.
 */
void __pagevec_release_nonlru(struct pagevec *pvec)
{
	int i;
	struct pagevec pages_to_free;

	pagevec_init(&pages_to_free, pvec->cold);
	pages_to_free.cold = pvec->cold;
	for (i = 0; i < pagevec_count(pvec); i++) {
		struct page *page = pvec->pages[i];

		BUG_ON(PageLRU(page));
		if (put_page_testzero(page))
			pagevec_add(&pages_to_free, page);
	}
	pagevec_free(&pages_to_free);
	pagevec_reinit(pvec);
}

/*
 * Add the passed pages to the LRU, then drop the caller's refcount
 * on them.  Reinitialises the caller's pagevec.
 */
void __pagevec_lru_add(struct pagevec *pvec)
{
	int i;
	struct zone *zone = NULL;

	for (i = 0; i < pagevec_count(pvec); i++) {
		struct page *page = pvec->pages[i];
		struct zone *pagezone = page_zone(page);

		if (pagezone != zone) {
			if (zone)
				spin_unlock_irq(&zone->lru_lock);
			zone = pagezone;
			spin_lock_irq(&zone->lru_lock);
		}
		if (TestSetPageLRU(page))
			BUG();
		add_page_to_inactive_list(zone, page);
	}
	if (zone)
		spin_unlock_irq(&zone->lru_lock);
	release_pages(pvec->pages, pvec->nr, pvec->cold);
	pagevec_reinit(pvec);
}

EXPORT_SYMBOL(__pagevec_lru_add);

void __pagevec_lru_add_active(struct pagevec *pvec)
{
	int i;
	struct zone *zone = NULL;

	for (i = 0; i < pagevec_count(pvec); i++) {
		struct page *page = pvec->pages[i];
		struct zone *pagezone = page_zone(page);

		if (pagezone != zone) {
			if (zone)
				spin_unlock_irq(&zone->lru_lock);
			zone = pagezone;
			spin_lock_irq(&zone->lru_lock);
		}
		if (TestSetPageLRU(page))
			BUG();
		if (TestSetPageActive(page))
			BUG();
		add_page_to_active_list(zone, page);
	}
	if (zone)
		spin_unlock_irq(&zone->lru_lock);
	release_pages(pvec->pages, pvec->nr, pvec->cold);
	pagevec_reinit(pvec);
}

/*
 * Try to drop buffers from the pages in a pagevec
 */
void pagevec_strip(struct pagevec *pvec)
{
	int i;

	for (i = 0; i < pagevec_count(pvec); i++) {
		struct page *page = pvec->pages[i];

		if (PagePrivate(page) && !TestSetPageLocked(page)) {
			try_to_release_page(page, 0);
			unlock_page(page);
		}
	}
}

/**
 * pagevec_lookup - gang pagecache lookup
 * @pvec:	Where the resulting pages are placed
 * @mapping:	The address_space to search
 * @start:	The starting page index
 * @nr_pages:	The maximum number of pages
 *
 * pagevec_lookup() will search for and return a group of up to @nr_pages pages
 * in the mapping.  The pages are placed in @pvec.  pagevec_lookup() takes a
 * reference against the pages in @pvec.
 *
 * The search returns a group of mapping-contiguous pages with ascending
 * indexes.  There may be holes in the indices due to not-present pages.
 *
 * pagevec_lookup() returns the number of pages which were found.
 */
unsigned pagevec_lookup(struct pagevec *pvec, struct address_space *mapping,
		pgoff_t start, unsigned nr_pages)
{
	pvec->nr = find_get_pages(mapping, start, nr_pages, pvec->pages);
	return pagevec_count(pvec);
}

unsigned pagevec_lookup_tag(struct pagevec *pvec, struct address_space *mapping,
		pgoff_t *index, int tag, unsigned nr_pages)
{
	pvec->nr = find_get_pages_tag(mapping, index, tag,
					nr_pages, pvec->pages);
	return pagevec_count(pvec);
}


#ifdef CONFIG_SMP
/*
 * We tolerate a little inaccuracy to avoid ping-ponging the counter between
 * CPUs
 */
#define ACCT_THRESHOLD	max(16, NR_CPUS * 2)

static DEFINE_PER_CPU(long, committed_space) = 0;

void vm_acct_memory(long pages)
{
	long *local;

	preempt_disable();
	local = &__get_cpu_var(committed_space);
	*local += pages;
	if (*local > ACCT_THRESHOLD || *local < -ACCT_THRESHOLD) {
		atomic_add(*local, &vm_committed_space);
		*local = 0;
	}
	preempt_enable();
}
EXPORT_SYMBOL(vm_acct_memory);

#ifdef CONFIG_HOTPLUG_CPU
static void lru_drain_cache(unsigned int cpu)
{
	struct pagevec *pvec = &per_cpu(lru_add_pvecs, cpu);

	/* CPU is dead, so no locking needed. */
	if (pagevec_count(pvec))
		__pagevec_lru_add(pvec);
	pvec = &per_cpu(lru_add_active_pvecs, cpu);
	if (pagevec_count(pvec))
		__pagevec_lru_add_active(pvec);
}

/* Drop the CPU's cached committed space back into the central pool. */
static int cpu_swap_callback(struct notifier_block *nfb,
			     unsigned long action,
			     void *hcpu)
{
	long *committed;

	committed = &per_cpu(committed_space, (long)hcpu);
	if (action == CPU_DEAD) {
		atomic_add(*committed, &vm_committed_space);
		*committed = 0;
		lru_drain_cache((long)hcpu);
	}
	return NOTIFY_OK;
}
#endif /* CONFIG_HOTPLUG_CPU */
#endif /* CONFIG_SMP */

#ifdef CONFIG_SMP
void percpu_counter_mod(struct percpu_counter *fbc, long amount)
{
	long count;
	long *pcount;
	int cpu = get_cpu();

	pcount = per_cpu_ptr(fbc->counters, cpu);
	count = *pcount + amount;
	if (count >= FBC_BATCH || count <= -FBC_BATCH) {
		spin_lock(&fbc->lock);
		fbc->count += count;
		spin_unlock(&fbc->lock);
		count = 0;
	}
	*pcount = count;
	put_cpu();
}
EXPORT_SYMBOL(percpu_counter_mod);
#endif

/*
 * Perform any setup for the swap system
 */
void __init swap_setup(void)
{
	unsigned long megs = num_physpages >> (20 - PAGE_SHIFT);

	/* Use a smaller cluster for small-memory machines */
	if (megs < 16)
		page_cluster = 2;
	else
		page_cluster = 3;
	/*
	 * Right now other parts of the system means that we
	 * _really_ don't want to cluster much more
	 */
	hotcpu_notifier(cpu_swap_callback, 0);
}
