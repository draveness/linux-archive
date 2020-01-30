/*
 * Resizable virtual memory filesystem for Linux.
 *
 * Copyright (C) 2000 Linus Torvalds.
 *		 2000 Transmeta Corp.
 *		 2000-2001 Christoph Rohland
 *		 2000-2001 SAP AG
 *		 2002 Red Hat Inc.
 * Copyright (C) 2002-2003 Hugh Dickins.
 * Copyright (C) 2002-2003 VERITAS Software Corporation.
 * Copyright (C) 2004 Andi Kleen, SuSE Labs
 *
 * This file is released under the GPL.
 */

/*
 * This virtual memory filesystem is heavily based on the ramfs. It
 * extends ramfs by the ability to use swap and honor resource limits
 * which makes it a completely usable filesystem.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/devfs_fs_kernel.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/file.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/backing-dev.h>
#include <linux/shmem_fs.h>
#include <linux/mount.h>
#include <linux/writeback.h>
#include <linux/vfs.h>
#include <linux/blkdev.h>
#include <linux/security.h>
#include <linux/swapops.h>
#include <linux/mempolicy.h>
#include <linux/namei.h>
#include <asm/uaccess.h>
#include <asm/div64.h>
#include <asm/pgtable.h>

/* This magic number is used in glibc for posix shared memory */
#define TMPFS_MAGIC	0x01021994

#define ENTRIES_PER_PAGE (PAGE_CACHE_SIZE/sizeof(unsigned long))
#define ENTRIES_PER_PAGEPAGE (ENTRIES_PER_PAGE*ENTRIES_PER_PAGE)
#define BLOCKS_PER_PAGE  (PAGE_CACHE_SIZE/512)

#define SHMEM_MAX_INDEX  (SHMEM_NR_DIRECT + (ENTRIES_PER_PAGEPAGE/2) * (ENTRIES_PER_PAGE+1))
#define SHMEM_MAX_BYTES  ((unsigned long long)SHMEM_MAX_INDEX << PAGE_CACHE_SHIFT)

#define VM_ACCT(size)    (PAGE_CACHE_ALIGN(size) >> PAGE_SHIFT)

/* info->flags needs VM_flags to handle pagein/truncate races efficiently */
#define SHMEM_PAGEIN	 VM_READ
#define SHMEM_TRUNCATE	 VM_WRITE

/* Pretend that each entry is of this size in directory's i_size */
#define BOGO_DIRENT_SIZE 20

/* Keep swapped page count in private field of indirect struct page */
#define nr_swapped		private

/* Flag allocation requirements to shmem_getpage and shmem_swp_alloc */
enum sgp_type {
	SGP_QUICK,	/* don't try more than file page cache lookup */
	SGP_READ,	/* don't exceed i_size, don't allocate page */
	SGP_CACHE,	/* don't exceed i_size, may allocate page */
	SGP_WRITE,	/* may exceed i_size, may allocate page */
};

static int shmem_getpage(struct inode *inode, unsigned long idx,
			 struct page **pagep, enum sgp_type sgp, int *type);

static inline struct page *shmem_dir_alloc(unsigned int gfp_mask)
{
	/*
	 * The above definition of ENTRIES_PER_PAGE, and the use of
	 * BLOCKS_PER_PAGE on indirect pages, assume PAGE_CACHE_SIZE:
	 * might be reconsidered if it ever diverges from PAGE_SIZE.
	 */
	return alloc_pages(gfp_mask, PAGE_CACHE_SHIFT-PAGE_SHIFT);
}

static inline void shmem_dir_free(struct page *page)
{
	__free_pages(page, PAGE_CACHE_SHIFT-PAGE_SHIFT);
}

static struct page **shmem_dir_map(struct page *page)
{
	return (struct page **)kmap_atomic(page, KM_USER0);
}

static inline void shmem_dir_unmap(struct page **dir)
{
	kunmap_atomic(dir, KM_USER0);
}

static swp_entry_t *shmem_swp_map(struct page *page)
{
	return (swp_entry_t *)kmap_atomic(page, KM_USER1);
}

static inline void shmem_swp_balance_unmap(void)
{
	/*
	 * When passing a pointer to an i_direct entry, to code which
	 * also handles indirect entries and so will shmem_swp_unmap,
	 * we must arrange for the preempt count to remain in balance.
	 * What kmap_atomic of a lowmem page does depends on config
	 * and architecture, so pretend to kmap_atomic some lowmem page.
	 */
	(void) kmap_atomic(ZERO_PAGE(0), KM_USER1);
}

static inline void shmem_swp_unmap(swp_entry_t *entry)
{
	kunmap_atomic(entry, KM_USER1);
}

static inline struct shmem_sb_info *SHMEM_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

/*
 * shmem_file_setup pre-accounts the whole fixed size of a VM object,
 * for shared memory and for shared anonymous (/dev/zero) mappings
 * (unless MAP_NORESERVE and sysctl_overcommit_memory <= 1),
 * consistent with the pre-accounting of private mappings ...
 */
static inline int shmem_acct_size(unsigned long flags, loff_t size)
{
	return (flags & VM_ACCOUNT)?
		security_vm_enough_memory(VM_ACCT(size)): 0;
}

static inline void shmem_unacct_size(unsigned long flags, loff_t size)
{
	if (flags & VM_ACCOUNT)
		vm_unacct_memory(VM_ACCT(size));
}

/*
 * ... whereas tmpfs objects are accounted incrementally as
 * pages are allocated, in order to allow huge sparse files.
 * shmem_getpage reports shmem_acct_block failure as -ENOSPC not -ENOMEM,
 * so that a failure on a sparse tmpfs mapping will give SIGBUS not OOM.
 */
static inline int shmem_acct_block(unsigned long flags)
{
	return (flags & VM_ACCOUNT)?
		0: security_vm_enough_memory(VM_ACCT(PAGE_CACHE_SIZE));
}

static inline void shmem_unacct_blocks(unsigned long flags, long pages)
{
	if (!(flags & VM_ACCOUNT))
		vm_unacct_memory(pages * VM_ACCT(PAGE_CACHE_SIZE));
}

static struct super_operations shmem_ops;
static struct address_space_operations shmem_aops;
static struct file_operations shmem_file_operations;
static struct inode_operations shmem_inode_operations;
static struct inode_operations shmem_dir_inode_operations;
static struct vm_operations_struct shmem_vm_ops;

static struct backing_dev_info shmem_backing_dev_info = {
	.ra_pages	= 0,	/* No readahead */
	.memory_backed	= 1,	/* Does not contribute to dirty memory */
	.unplug_io_fn = default_unplug_io_fn,
};

LIST_HEAD(shmem_inodes);
static spinlock_t shmem_ilock = SPIN_LOCK_UNLOCKED;

static void shmem_free_block(struct inode *inode)
{
	struct shmem_sb_info *sbinfo = SHMEM_SB(inode->i_sb);
	spin_lock(&sbinfo->stat_lock);
	sbinfo->free_blocks++;
	inode->i_blocks -= BLOCKS_PER_PAGE;
	spin_unlock(&sbinfo->stat_lock);
}

/*
 * shmem_recalc_inode - recalculate the size of an inode
 *
 * @inode: inode to recalc
 *
 * We have to calculate the free blocks since the mm can drop
 * undirtied hole pages behind our back.
 *
 * But normally   info->alloced == inode->i_mapping->nrpages + info->swapped
 * So mm freed is info->alloced - (inode->i_mapping->nrpages + info->swapped)
 *
 * It has to be called with the spinlock held.
 */
static void shmem_recalc_inode(struct inode *inode)
{
	struct shmem_inode_info *info = SHMEM_I(inode);
	long freed;

	freed = info->alloced - info->swapped - inode->i_mapping->nrpages;
	if (freed > 0) {
		struct shmem_sb_info *sbinfo = SHMEM_SB(inode->i_sb);
		info->alloced -= freed;
		spin_lock(&sbinfo->stat_lock);
		sbinfo->free_blocks += freed;
		inode->i_blocks -= freed*BLOCKS_PER_PAGE;
		spin_unlock(&sbinfo->stat_lock);
		shmem_unacct_blocks(info->flags, freed);
	}
}

/*
 * shmem_swp_entry - find the swap vector position in the info structure
 *
 * @info:  info structure for the inode
 * @index: index of the page to find
 * @page:  optional page to add to the structure. Has to be preset to
 *         all zeros
 *
 * If there is no space allocated yet it will return NULL when
 * page is NULL, else it will use the page for the needed block,
 * setting it to NULL on return to indicate that it has been used.
 *
 * The swap vector is organized the following way:
 *
 * There are SHMEM_NR_DIRECT entries directly stored in the
 * shmem_inode_info structure. So small files do not need an addional
 * allocation.
 *
 * For pages with index > SHMEM_NR_DIRECT there is the pointer
 * i_indirect which points to a page which holds in the first half
 * doubly indirect blocks, in the second half triple indirect blocks:
 *
 * For an artificial ENTRIES_PER_PAGE = 4 this would lead to the
 * following layout (for SHMEM_NR_DIRECT == 16):
 *
 * i_indirect -> dir --> 16-19
 * 	      |	     +-> 20-23
 * 	      |
 * 	      +-->dir2 --> 24-27
 * 	      |	       +-> 28-31
 * 	      |	       +-> 32-35
 * 	      |	       +-> 36-39
 * 	      |
 * 	      +-->dir3 --> 40-43
 * 	       	       +-> 44-47
 * 	      	       +-> 48-51
 * 	      	       +-> 52-55
 */
static swp_entry_t *shmem_swp_entry(struct shmem_inode_info *info, unsigned long index, struct page **page)
{
	unsigned long offset;
	struct page **dir;
	struct page *subdir;

	if (index < SHMEM_NR_DIRECT) {
		shmem_swp_balance_unmap();
		return info->i_direct+index;
	}
	if (!info->i_indirect) {
		if (page) {
			info->i_indirect = *page;
			*page = NULL;
		}
		return NULL;			/* need another page */
	}

	index -= SHMEM_NR_DIRECT;
	offset = index % ENTRIES_PER_PAGE;
	index /= ENTRIES_PER_PAGE;
	dir = shmem_dir_map(info->i_indirect);

	if (index >= ENTRIES_PER_PAGE/2) {
		index -= ENTRIES_PER_PAGE/2;
		dir += ENTRIES_PER_PAGE/2 + index/ENTRIES_PER_PAGE;
		index %= ENTRIES_PER_PAGE;
		subdir = *dir;
		if (!subdir) {
			if (page) {
				*dir = *page;
				*page = NULL;
			}
			shmem_dir_unmap(dir);
			return NULL;		/* need another page */
		}
		shmem_dir_unmap(dir);
		dir = shmem_dir_map(subdir);
	}

	dir += index;
	subdir = *dir;
	if (!subdir) {
		if (!page || !(subdir = *page)) {
			shmem_dir_unmap(dir);
			return NULL;		/* need a page */
		}
		*dir = subdir;
		*page = NULL;
	}
	shmem_dir_unmap(dir);
	return shmem_swp_map(subdir) + offset;
}

static void shmem_swp_set(struct shmem_inode_info *info, swp_entry_t *entry, unsigned long value)
{
	long incdec = value? 1: -1;

	entry->val = value;
	info->swapped += incdec;
	if ((unsigned long)(entry - info->i_direct) >= SHMEM_NR_DIRECT)
		kmap_atomic_to_page(entry)->nr_swapped += incdec;
}

/*
 * shmem_swp_alloc - get the position of the swap entry for the page.
 *                   If it does not exist allocate the entry.
 *
 * @info:	info structure for the inode
 * @index:	index of the page to find
 * @sgp:	check and recheck i_size? skip allocation?
 */
static swp_entry_t *shmem_swp_alloc(struct shmem_inode_info *info, unsigned long index, enum sgp_type sgp)
{
	struct inode *inode = &info->vfs_inode;
	struct shmem_sb_info *sbinfo = SHMEM_SB(inode->i_sb);
	struct page *page = NULL;
	swp_entry_t *entry;

	if (sgp != SGP_WRITE &&
	    ((loff_t) index << PAGE_CACHE_SHIFT) >= i_size_read(inode))
		return ERR_PTR(-EINVAL);

	while (!(entry = shmem_swp_entry(info, index, &page))) {
		if (sgp == SGP_READ)
			return shmem_swp_map(ZERO_PAGE(0));
		/*
		 * Test free_blocks against 1 not 0, since we have 1 data
		 * page (and perhaps indirect index pages) yet to allocate:
		 * a waste to allocate index if we cannot allocate data.
		 */
		spin_lock(&sbinfo->stat_lock);
		if (sbinfo->free_blocks <= 1) {
			spin_unlock(&sbinfo->stat_lock);
			return ERR_PTR(-ENOSPC);
		}
		sbinfo->free_blocks--;
		inode->i_blocks += BLOCKS_PER_PAGE;
		spin_unlock(&sbinfo->stat_lock);

		spin_unlock(&info->lock);
		page = shmem_dir_alloc(mapping_gfp_mask(inode->i_mapping));
		if (page) {
			clear_highpage(page);
			page->nr_swapped = 0;
		}
		spin_lock(&info->lock);

		if (!page) {
			shmem_free_block(inode);
			return ERR_PTR(-ENOMEM);
		}
		if (sgp != SGP_WRITE &&
		    ((loff_t) index << PAGE_CACHE_SHIFT) >= i_size_read(inode)) {
			entry = ERR_PTR(-EINVAL);
			break;
		}
		if (info->next_index <= index)
			info->next_index = index + 1;
	}
	if (page) {
		/* another task gave its page, or truncated the file */
		shmem_free_block(inode);
		shmem_dir_free(page);
	}
	if (info->next_index <= index && !IS_ERR(entry))
		info->next_index = index + 1;
	return entry;
}

/*
 * shmem_free_swp - free some swap entries in a directory
 *
 * @dir:   pointer to the directory
 * @edir:  pointer after last entry of the directory
 */
static int shmem_free_swp(swp_entry_t *dir, swp_entry_t *edir)
{
	swp_entry_t *ptr;
	int freed = 0;

	for (ptr = dir; ptr < edir; ptr++) {
		if (ptr->val) {
			free_swap_and_cache(*ptr);
			*ptr = (swp_entry_t){0};
			freed++;
		}
	}
	return freed;
}

static void shmem_truncate(struct inode *inode)
{
	struct shmem_inode_info *info = SHMEM_I(inode);
	unsigned long idx;
	unsigned long size;
	unsigned long limit;
	unsigned long stage;
	struct page **dir;
	struct page *subdir;
	struct page *empty;
	swp_entry_t *ptr;
	int offset;
	int freed;

	inode->i_ctime = inode->i_mtime = CURRENT_TIME;
	idx = (inode->i_size + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
	if (idx >= info->next_index)
		return;

	spin_lock(&info->lock);
	info->flags |= SHMEM_TRUNCATE;
	limit = info->next_index;
	info->next_index = idx;
	if (info->swapped && idx < SHMEM_NR_DIRECT) {
		ptr = info->i_direct;
		size = limit;
		if (size > SHMEM_NR_DIRECT)
			size = SHMEM_NR_DIRECT;
		info->swapped -= shmem_free_swp(ptr+idx, ptr+size);
	}
	if (!info->i_indirect)
		goto done2;

	BUG_ON(limit <= SHMEM_NR_DIRECT);
	limit -= SHMEM_NR_DIRECT;
	idx = (idx > SHMEM_NR_DIRECT)? (idx - SHMEM_NR_DIRECT): 0;
	offset = idx % ENTRIES_PER_PAGE;
	idx -= offset;

	empty = NULL;
	dir = shmem_dir_map(info->i_indirect);
	stage = ENTRIES_PER_PAGEPAGE/2;
	if (idx < ENTRIES_PER_PAGEPAGE/2)
		dir += idx/ENTRIES_PER_PAGE;
	else {
		dir += ENTRIES_PER_PAGE/2;
		dir += (idx - ENTRIES_PER_PAGEPAGE/2)/ENTRIES_PER_PAGEPAGE;
		while (stage <= idx)
			stage += ENTRIES_PER_PAGEPAGE;
		if (*dir) {
			subdir = *dir;
			size = ((idx - ENTRIES_PER_PAGEPAGE/2) %
				ENTRIES_PER_PAGEPAGE) / ENTRIES_PER_PAGE;
			if (!size && !offset) {
				empty = subdir;
				*dir = NULL;
			}
			shmem_dir_unmap(dir);
			dir = shmem_dir_map(subdir) + size;
		} else {
			offset = 0;
			idx = stage;
		}
	}

	for (; idx < limit; idx += ENTRIES_PER_PAGE, dir++) {
		if (unlikely(idx == stage)) {
			shmem_dir_unmap(dir-1);
			dir = shmem_dir_map(info->i_indirect) +
			    ENTRIES_PER_PAGE/2 + idx/ENTRIES_PER_PAGEPAGE;
			while (!*dir) {
				dir++;
				idx += ENTRIES_PER_PAGEPAGE;
				if (idx >= limit)
					goto done1;
			}
			stage = idx + ENTRIES_PER_PAGEPAGE;
			subdir = *dir;
			*dir = NULL;
			shmem_dir_unmap(dir);
			if (empty) {
				shmem_dir_free(empty);
				shmem_free_block(inode);
			}
			empty = subdir;
			cond_resched_lock(&info->lock);
			dir = shmem_dir_map(subdir);
		}
		subdir = *dir;
		if (subdir && subdir->nr_swapped) {
			ptr = shmem_swp_map(subdir);
			size = limit - idx;
			if (size > ENTRIES_PER_PAGE)
				size = ENTRIES_PER_PAGE;
			freed = shmem_free_swp(ptr+offset, ptr+size);
			shmem_swp_unmap(ptr);
			info->swapped -= freed;
			subdir->nr_swapped -= freed;
			BUG_ON(subdir->nr_swapped > offset);
		}
		if (offset)
			offset = 0;
		else if (subdir) {
			*dir = NULL;
			shmem_dir_free(subdir);
			shmem_free_block(inode);
		}
	}
done1:
	shmem_dir_unmap(dir-1);
	if (empty) {
		shmem_dir_free(empty);
		shmem_free_block(inode);
	}
	if (info->next_index <= SHMEM_NR_DIRECT) {
		shmem_dir_free(info->i_indirect);
		info->i_indirect = NULL;
		shmem_free_block(inode);
	}
done2:
	BUG_ON(info->swapped > info->next_index);
	if (inode->i_mapping->nrpages && (info->flags & SHMEM_PAGEIN)) {
		/*
		 * Call truncate_inode_pages again: racing shmem_unuse_inode
		 * may have swizzled a page in from swap since vmtruncate or
		 * generic_delete_inode did it, before we lowered next_index.
		 * Also, though shmem_getpage checks i_size before adding to
		 * cache, no recheck after: so fix the narrow window there too.
		 */
		spin_unlock(&info->lock);
		truncate_inode_pages(inode->i_mapping, inode->i_size);
		spin_lock(&info->lock);
	}
	info->flags &= ~SHMEM_TRUNCATE;
	shmem_recalc_inode(inode);
	spin_unlock(&info->lock);
}

static int shmem_notify_change(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	struct page *page = NULL;
	int error;

	if (attr->ia_valid & ATTR_SIZE) {
		if (attr->ia_size < inode->i_size) {
			/*
			 * If truncating down to a partial page, then
			 * if that page is already allocated, hold it
			 * in memory until the truncation is over, so
			 * truncate_partial_page cannnot miss it were
			 * it assigned to swap.
			 */
			if (attr->ia_size & (PAGE_CACHE_SIZE-1)) {
				(void) shmem_getpage(inode,
					attr->ia_size>>PAGE_CACHE_SHIFT,
						&page, SGP_READ, NULL);
			}
			/*
			 * Reset SHMEM_PAGEIN flag so that shmem_truncate can
			 * detect if any pages might have been added to cache
			 * after truncate_inode_pages.  But we needn't bother
			 * if it's being fully truncated to zero-length: the
			 * nrpages check is efficient enough in that case.
			 */
			if (attr->ia_size) {
				struct shmem_inode_info *info = SHMEM_I(inode);
				spin_lock(&info->lock);
				info->flags &= ~SHMEM_PAGEIN;
				spin_unlock(&info->lock);
			}
		}
	}

	error = inode_change_ok(inode, attr);
	if (!error)
		error = inode_setattr(inode, attr);
	if (page)
		page_cache_release(page);
	return error;
}

static void shmem_delete_inode(struct inode *inode)
{
	struct shmem_sb_info *sbinfo = SHMEM_SB(inode->i_sb);
	struct shmem_inode_info *info = SHMEM_I(inode);

	if (inode->i_op->truncate == shmem_truncate) {
		spin_lock(&shmem_ilock);
		list_del(&info->list);
		spin_unlock(&shmem_ilock);
		shmem_unacct_size(info->flags, inode->i_size);
		inode->i_size = 0;
		shmem_truncate(inode);
	}
	BUG_ON(inode->i_blocks);
	spin_lock(&sbinfo->stat_lock);
	sbinfo->free_inodes++;
	spin_unlock(&sbinfo->stat_lock);
	clear_inode(inode);
}

static inline int shmem_find_swp(swp_entry_t entry, swp_entry_t *dir, swp_entry_t *edir)
{
	swp_entry_t *ptr;

	for (ptr = dir; ptr < edir; ptr++) {
		if (ptr->val == entry.val)
			return ptr - dir;
	}
	return -1;
}

static int shmem_unuse_inode(struct shmem_inode_info *info, swp_entry_t entry, struct page *page)
{
	struct inode *inode;
	unsigned long idx;
	unsigned long size;
	unsigned long limit;
	unsigned long stage;
	struct page **dir;
	struct page *subdir;
	swp_entry_t *ptr;
	int offset;

	idx = 0;
	ptr = info->i_direct;
	spin_lock(&info->lock);
	limit = info->next_index;
	size = limit;
	if (size > SHMEM_NR_DIRECT)
		size = SHMEM_NR_DIRECT;
	offset = shmem_find_swp(entry, ptr, ptr+size);
	if (offset >= 0) {
		shmem_swp_balance_unmap();
		goto found;
	}
	if (!info->i_indirect)
		goto lost2;
	/* we might be racing with shmem_truncate */
	if (limit <= SHMEM_NR_DIRECT)
		goto lost2;

	dir = shmem_dir_map(info->i_indirect);
	stage = SHMEM_NR_DIRECT + ENTRIES_PER_PAGEPAGE/2;

	for (idx = SHMEM_NR_DIRECT; idx < limit; idx += ENTRIES_PER_PAGE, dir++) {
		if (unlikely(idx == stage)) {
			shmem_dir_unmap(dir-1);
			dir = shmem_dir_map(info->i_indirect) +
			    ENTRIES_PER_PAGE/2 + idx/ENTRIES_PER_PAGEPAGE;
			while (!*dir) {
				dir++;
				idx += ENTRIES_PER_PAGEPAGE;
				if (idx >= limit)
					goto lost1;
			}
			stage = idx + ENTRIES_PER_PAGEPAGE;
			subdir = *dir;
			shmem_dir_unmap(dir);
			dir = shmem_dir_map(subdir);
		}
		subdir = *dir;
		if (subdir && subdir->nr_swapped) {
			ptr = shmem_swp_map(subdir);
			size = limit - idx;
			if (size > ENTRIES_PER_PAGE)
				size = ENTRIES_PER_PAGE;
			offset = shmem_find_swp(entry, ptr, ptr+size);
			if (offset >= 0) {
				shmem_dir_unmap(dir);
				goto found;
			}
			shmem_swp_unmap(ptr);
		}
	}
lost1:
	shmem_dir_unmap(dir-1);
lost2:
	spin_unlock(&info->lock);
	return 0;
found:
	idx += offset;
	inode = &info->vfs_inode;
	if (move_from_swap_cache(page, idx, inode->i_mapping) == 0) {
		info->flags |= SHMEM_PAGEIN;
		shmem_swp_set(info, ptr + offset, 0);
	}
	shmem_swp_unmap(ptr);
	spin_unlock(&info->lock);
	/*
	 * Decrement swap count even when the entry is left behind:
	 * try_to_unuse will skip over mms, then reincrement count.
	 */
	swap_free(entry);
	return 1;
}

/*
 * shmem_unuse() search for an eventually swapped out shmem page.
 */
int shmem_unuse(swp_entry_t entry, struct page *page)
{
	struct list_head *p;
	struct shmem_inode_info *info;
	int found = 0;

	spin_lock(&shmem_ilock);
	list_for_each(p, &shmem_inodes) {
		info = list_entry(p, struct shmem_inode_info, list);

		if (info->swapped && shmem_unuse_inode(info, entry, page)) {
			/* move head to start search for next from here */
			list_move_tail(&shmem_inodes, &info->list);
			found = 1;
			break;
		}
	}
	spin_unlock(&shmem_ilock);
	return found;
}

/*
 * Move the page from the page cache to the swap cache.
 */
static int shmem_writepage(struct page *page, struct writeback_control *wbc)
{
	struct shmem_inode_info *info;
	swp_entry_t *entry, swap;
	struct address_space *mapping;
	unsigned long index;
	struct inode *inode;

	BUG_ON(!PageLocked(page));
	BUG_ON(page_mapped(page));

	mapping = page->mapping;
	index = page->index;
	inode = mapping->host;
	info = SHMEM_I(inode);
	if (info->flags & VM_LOCKED)
		goto redirty;
	swap = get_swap_page();
	if (!swap.val)
		goto redirty;

	spin_lock(&info->lock);
	shmem_recalc_inode(inode);
	if (index >= info->next_index) {
		BUG_ON(!(info->flags & SHMEM_TRUNCATE));
		goto unlock;
	}
	entry = shmem_swp_entry(info, index, NULL);
	BUG_ON(!entry);
	BUG_ON(entry->val);

	if (move_to_swap_cache(page, swap) == 0) {
		shmem_swp_set(info, entry, swap.val);
		shmem_swp_unmap(entry);
		spin_unlock(&info->lock);
		unlock_page(page);
		return 0;
	}

	shmem_swp_unmap(entry);
unlock:
	spin_unlock(&info->lock);
	swap_free(swap);
redirty:
	set_page_dirty(page);
	return WRITEPAGE_ACTIVATE;	/* Return with the page locked */
}

#ifdef CONFIG_NUMA
static struct page *shmem_swapin_async(struct shared_policy *p,
				       swp_entry_t entry, unsigned long idx)
{
	struct page *page;
	struct vm_area_struct pvma;

	/* Create a pseudo vma that just contains the policy */
	memset(&pvma, 0, sizeof(struct vm_area_struct));
	pvma.vm_end = PAGE_SIZE;
	pvma.vm_pgoff = idx;
	pvma.vm_policy = mpol_shared_policy_lookup(p, idx);
	page = read_swap_cache_async(entry, &pvma, 0);
	mpol_free(pvma.vm_policy);
	return page;
}

struct page *shmem_swapin(struct shmem_inode_info *info, swp_entry_t entry,
			  unsigned long idx)
{
	struct shared_policy *p = &info->policy;
	int i, num;
	struct page *page;
	unsigned long offset;

	num = valid_swaphandles(entry, &offset);
	for (i = 0; i < num; offset++, i++) {
		page = shmem_swapin_async(p,
				swp_entry(swp_type(entry), offset), idx);
		if (!page)
			break;
		page_cache_release(page);
	}
	lru_add_drain();	/* Push any new pages onto the LRU now */
	return shmem_swapin_async(p, entry, idx);
}

static struct page *
shmem_alloc_page(unsigned long gfp, struct shmem_inode_info *info,
		 unsigned long idx)
{
	struct vm_area_struct pvma;
	struct page *page;

	memset(&pvma, 0, sizeof(struct vm_area_struct));
	pvma.vm_policy = mpol_shared_policy_lookup(&info->policy, idx);
	pvma.vm_pgoff = idx;
	pvma.vm_end = PAGE_SIZE;
	page = alloc_page_vma(gfp, &pvma, 0);
	mpol_free(pvma.vm_policy);
	return page;
}
#else
static inline struct page *
shmem_swapin(struct shmem_inode_info *info,swp_entry_t entry,unsigned long idx)
{
	swapin_readahead(entry, 0, NULL);
	return read_swap_cache_async(entry, NULL, 0);
}

static inline struct page *
shmem_alloc_page(unsigned long gfp,struct shmem_inode_info *info,
				 unsigned long idx)
{
	return alloc_page(gfp);
}
#endif

/*
 * shmem_getpage - either get the page from swap or allocate a new one
 *
 * If we allocate a new one we do not mark it dirty. That's up to the
 * vm. If we swap it in we mark it dirty since we also free the swap
 * entry since a page cannot live in both the swap and page cache
 */
static int shmem_getpage(struct inode *inode, unsigned long idx,
			struct page **pagep, enum sgp_type sgp, int *type)
{
	struct address_space *mapping = inode->i_mapping;
	struct shmem_inode_info *info = SHMEM_I(inode);
	struct shmem_sb_info *sbinfo;
	struct page *filepage = *pagep;
	struct page *swappage;
	swp_entry_t *entry;
	swp_entry_t swap;
	int error, majmin = VM_FAULT_MINOR;

	if (idx >= SHMEM_MAX_INDEX)
		return -EFBIG;
	/*
	 * Normally, filepage is NULL on entry, and either found
	 * uptodate immediately, or allocated and zeroed, or read
	 * in under swappage, which is then assigned to filepage.
	 * But shmem_prepare_write passes in a locked filepage,
	 * which may be found not uptodate by other callers too,
	 * and may need to be copied from the swappage read in.
	 */
repeat:
	if (!filepage)
		filepage = find_lock_page(mapping, idx);
	if (filepage && PageUptodate(filepage))
		goto done;
	error = 0;
	if (sgp == SGP_QUICK)
		goto failed;

	spin_lock(&info->lock);
	shmem_recalc_inode(inode);
	entry = shmem_swp_alloc(info, idx, sgp);
	if (IS_ERR(entry)) {
		spin_unlock(&info->lock);
		error = PTR_ERR(entry);
		goto failed;
	}
	swap = *entry;

	if (swap.val) {
		/* Look it up and read it in.. */
		swappage = lookup_swap_cache(swap);
		if (!swappage) {
			shmem_swp_unmap(entry);
			spin_unlock(&info->lock);
			/* here we actually do the io */
			if (majmin == VM_FAULT_MINOR && type)
				inc_page_state(pgmajfault);
			majmin = VM_FAULT_MAJOR;
			swappage = shmem_swapin(info, swap, idx);
			if (!swappage) {
				spin_lock(&info->lock);
				entry = shmem_swp_alloc(info, idx, sgp);
				if (IS_ERR(entry))
					error = PTR_ERR(entry);
				else {
					if (entry->val == swap.val)
						error = -ENOMEM;
					shmem_swp_unmap(entry);
				}
				spin_unlock(&info->lock);
				if (error)
					goto failed;
				goto repeat;
			}
			wait_on_page_locked(swappage);
			page_cache_release(swappage);
			goto repeat;
		}

		/* We have to do this with page locked to prevent races */
		if (TestSetPageLocked(swappage)) {
			shmem_swp_unmap(entry);
			spin_unlock(&info->lock);
			wait_on_page_locked(swappage);
			page_cache_release(swappage);
			goto repeat;
		}
		if (PageWriteback(swappage)) {
			shmem_swp_unmap(entry);
			spin_unlock(&info->lock);
			wait_on_page_writeback(swappage);
			unlock_page(swappage);
			page_cache_release(swappage);
			goto repeat;
		}
		if (!PageUptodate(swappage)) {
			shmem_swp_unmap(entry);
			spin_unlock(&info->lock);
			unlock_page(swappage);
			page_cache_release(swappage);
			error = -EIO;
			goto failed;
		}

		if (filepage) {
			shmem_swp_set(info, entry, 0);
			shmem_swp_unmap(entry);
			delete_from_swap_cache(swappage);
			spin_unlock(&info->lock);
			copy_highpage(filepage, swappage);
			unlock_page(swappage);
			page_cache_release(swappage);
			flush_dcache_page(filepage);
			SetPageUptodate(filepage);
			set_page_dirty(filepage);
			swap_free(swap);
		} else if (!(error = move_from_swap_cache(
				swappage, idx, mapping))) {
			info->flags |= SHMEM_PAGEIN;
			shmem_swp_set(info, entry, 0);
			shmem_swp_unmap(entry);
			spin_unlock(&info->lock);
			filepage = swappage;
			swap_free(swap);
		} else {
			shmem_swp_unmap(entry);
			spin_unlock(&info->lock);
			unlock_page(swappage);
			page_cache_release(swappage);
			if (error == -ENOMEM) {
				/* let kswapd refresh zone for GFP_ATOMICs */
				blk_congestion_wait(WRITE, HZ/50);
			}
			goto repeat;
		}
	} else if (sgp == SGP_READ && !filepage) {
		shmem_swp_unmap(entry);
		filepage = find_get_page(mapping, idx);
		if (filepage &&
		    (!PageUptodate(filepage) || TestSetPageLocked(filepage))) {
			spin_unlock(&info->lock);
			wait_on_page_locked(filepage);
			page_cache_release(filepage);
			filepage = NULL;
			goto repeat;
		}
		spin_unlock(&info->lock);
	} else {
		shmem_swp_unmap(entry);
		sbinfo = SHMEM_SB(inode->i_sb);
		spin_lock(&sbinfo->stat_lock);
		if (sbinfo->free_blocks == 0 || shmem_acct_block(info->flags)) {
			spin_unlock(&sbinfo->stat_lock);
			spin_unlock(&info->lock);
			error = -ENOSPC;
			goto failed;
		}
		sbinfo->free_blocks--;
		inode->i_blocks += BLOCKS_PER_PAGE;
		spin_unlock(&sbinfo->stat_lock);

		if (!filepage) {
			spin_unlock(&info->lock);
			filepage = shmem_alloc_page(mapping_gfp_mask(mapping),
						    info,
						    idx);
			if (!filepage) {
				shmem_unacct_blocks(info->flags, 1);
				shmem_free_block(inode);
				error = -ENOMEM;
				goto failed;
			}

			spin_lock(&info->lock);
			entry = shmem_swp_alloc(info, idx, sgp);
			if (IS_ERR(entry))
				error = PTR_ERR(entry);
			else {
				swap = *entry;
				shmem_swp_unmap(entry);
			}
			if (error || swap.val || 0 != add_to_page_cache_lru(
					filepage, mapping, idx, GFP_ATOMIC)) {
				spin_unlock(&info->lock);
				page_cache_release(filepage);
				shmem_unacct_blocks(info->flags, 1);
				shmem_free_block(inode);
				filepage = NULL;
				if (error)
					goto failed;
				goto repeat;
			}
			info->flags |= SHMEM_PAGEIN;
		}

		info->alloced++;
		spin_unlock(&info->lock);
		clear_highpage(filepage);
		flush_dcache_page(filepage);
		SetPageUptodate(filepage);
	}
done:
	if (!*pagep) {
		if (filepage) {
			unlock_page(filepage);
			*pagep = filepage;
		} else
			*pagep = ZERO_PAGE(0);
	}
	if (type)
		*type = majmin;
	return 0;

failed:
	if (*pagep != filepage) {
		unlock_page(filepage);
		page_cache_release(filepage);
	}
	return error;
}

struct page *shmem_nopage(struct vm_area_struct *vma, unsigned long address, int *type)
{
	struct inode *inode = vma->vm_file->f_dentry->d_inode;
	struct page *page = NULL;
	unsigned long idx;
	int error;

	idx = (address - vma->vm_start) >> PAGE_SHIFT;
	idx += vma->vm_pgoff;
	idx >>= PAGE_CACHE_SHIFT - PAGE_SHIFT;

	error = shmem_getpage(inode, idx, &page, SGP_CACHE, type);
	if (error)
		return (error == -ENOMEM)? NOPAGE_OOM: NOPAGE_SIGBUS;

	mark_page_accessed(page);
	return page;
}

static int shmem_populate(struct vm_area_struct *vma,
	unsigned long addr, unsigned long len,
	pgprot_t prot, unsigned long pgoff, int nonblock)
{
	struct inode *inode = vma->vm_file->f_dentry->d_inode;
	struct mm_struct *mm = vma->vm_mm;
	enum sgp_type sgp = nonblock? SGP_QUICK: SGP_CACHE;
	unsigned long size;

	size = (i_size_read(inode) + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (pgoff >= size || pgoff + (len >> PAGE_SHIFT) > size)
		return -EINVAL;

	while ((long) len > 0) {
		struct page *page = NULL;
		int err;
		/*
		 * Will need changing if PAGE_CACHE_SIZE != PAGE_SIZE
		 */
		err = shmem_getpage(inode, pgoff, &page, sgp, NULL);
		if (err)
			return err;
		if (page) {
			mark_page_accessed(page);
			err = install_page(mm, vma, addr, page, prot);
			if (err) {
				page_cache_release(page);
				return err;
			}
		} else if (nonblock) {
    			err = install_file_pte(mm, vma, addr, pgoff, prot);
			if (err)
	    			return err;
		}

		len -= PAGE_SIZE;
		addr += PAGE_SIZE;
		pgoff++;
	}
	return 0;
}

#ifdef CONFIG_NUMA
int shmem_set_policy(struct vm_area_struct *vma, struct mempolicy *new)
{
	struct inode *i = vma->vm_file->f_dentry->d_inode;
	return mpol_set_shared_policy(&SHMEM_I(i)->policy, vma, new);
}

struct mempolicy *
shmem_get_policy(struct vm_area_struct *vma, unsigned long addr)
{
	struct inode *i = vma->vm_file->f_dentry->d_inode;
	unsigned long idx;

	idx = ((addr - vma->vm_start) >> PAGE_SHIFT) + vma->vm_pgoff;
	return mpol_shared_policy_lookup(&SHMEM_I(i)->policy, idx);
}
#endif

void shmem_lock(struct file *file, int lock)
{
	struct inode *inode = file->f_dentry->d_inode;
	struct shmem_inode_info *info = SHMEM_I(inode);

	spin_lock(&info->lock);
	if (lock)
		info->flags |= VM_LOCKED;
	else
		info->flags &= ~VM_LOCKED;
	spin_unlock(&info->lock);
}

static int shmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	file_accessed(file);
	vma->vm_ops = &shmem_vm_ops;
	return 0;
}

static struct inode *
shmem_get_inode(struct super_block *sb, int mode, dev_t dev)
{
	struct inode *inode;
	struct shmem_inode_info *info;
	struct shmem_sb_info *sbinfo = SHMEM_SB(sb);

	spin_lock(&sbinfo->stat_lock);
	if (!sbinfo->free_inodes) {
		spin_unlock(&sbinfo->stat_lock);
		return NULL;
	}
	sbinfo->free_inodes--;
	spin_unlock(&sbinfo->stat_lock);

	inode = new_inode(sb);
	if (inode) {
		inode->i_mode = mode;
		inode->i_uid = current->fsuid;
		inode->i_gid = current->fsgid;
		inode->i_blksize = PAGE_CACHE_SIZE;
		inode->i_blocks = 0;
		inode->i_mapping->a_ops = &shmem_aops;
		inode->i_mapping->backing_dev_info = &shmem_backing_dev_info;
		inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
		info = SHMEM_I(inode);
		memset(info, 0, (char *)inode - (char *)info);
		spin_lock_init(&info->lock);
 		mpol_shared_policy_init(&info->policy);
		switch (mode & S_IFMT) {
		default:
			init_special_inode(inode, mode, dev);
			break;
		case S_IFREG:
			inode->i_op = &shmem_inode_operations;
			inode->i_fop = &shmem_file_operations;
			spin_lock(&shmem_ilock);
			list_add_tail(&info->list, &shmem_inodes);
			spin_unlock(&shmem_ilock);
			break;
		case S_IFDIR:
			inode->i_nlink++;
			/* Some things misbehave if size == 0 on a directory */
			inode->i_size = 2 * BOGO_DIRENT_SIZE;
			inode->i_op = &shmem_dir_inode_operations;
			inode->i_fop = &simple_dir_operations;
			break;
		case S_IFLNK:
			break;
		}
	}
	return inode;
}

static int shmem_set_size(struct shmem_sb_info *info,
			  unsigned long max_blocks, unsigned long max_inodes)
{
	int error;
	unsigned long blocks, inodes;

	spin_lock(&info->stat_lock);
	blocks = info->max_blocks - info->free_blocks;
	inodes = info->max_inodes - info->free_inodes;
	error = -EINVAL;
	if (max_blocks < blocks)
		goto out;
	if (max_inodes < inodes)
		goto out;
	error = 0;
	info->max_blocks  = max_blocks;
	info->free_blocks = max_blocks - blocks;
	info->max_inodes  = max_inodes;
	info->free_inodes = max_inodes - inodes;
out:
	spin_unlock(&info->stat_lock);
	return error;
}

#ifdef CONFIG_TMPFS

static struct inode_operations shmem_symlink_inode_operations;
static struct inode_operations shmem_symlink_inline_operations;

/*
 * Normally tmpfs makes no use of shmem_prepare_write, but it
 * lets a tmpfs file be used read-write below the loop driver.
 */
static int
shmem_prepare_write(struct file *file, struct page *page, unsigned offset, unsigned to)
{
	struct inode *inode = page->mapping->host;
	return shmem_getpage(inode, page->index, &page, SGP_WRITE, NULL);
}

static ssize_t
shmem_file_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct inode	*inode = file->f_dentry->d_inode;
	loff_t		pos;
	unsigned long	written;
	int		err;

	if ((ssize_t) count < 0)
		return -EINVAL;

	if (!access_ok(VERIFY_READ, buf, count))
		return -EFAULT;

	down(&inode->i_sem);

	pos = *ppos;
	written = 0;

	err = generic_write_checks(file, &pos, &count, 0);
	if (err || !count)
		goto out;

	err = remove_suid(file->f_dentry);
	if (err)
		goto out;

	inode->i_ctime = inode->i_mtime = CURRENT_TIME;

	do {
		struct page *page = NULL;
		unsigned long bytes, index, offset;
		char *kaddr;
		int left;

		offset = (pos & (PAGE_CACHE_SIZE -1)); /* Within page */
		index = pos >> PAGE_CACHE_SHIFT;
		bytes = PAGE_CACHE_SIZE - offset;
		if (bytes > count)
			bytes = count;

		/*
		 * We don't hold page lock across copy from user -
		 * what would it guard against? - so no deadlock here.
		 * But it still may be a good idea to prefault below.
		 */

		err = shmem_getpage(inode, index, &page, SGP_WRITE, NULL);
		if (err)
			break;

		left = bytes;
		if (PageHighMem(page)) {
			volatile unsigned char dummy;
			__get_user(dummy, buf);
			__get_user(dummy, buf + bytes - 1);

			kaddr = kmap_atomic(page, KM_USER0);
			left = __copy_from_user(kaddr + offset, buf, bytes);
			kunmap_atomic(kaddr, KM_USER0);
		}
		if (left) {
			kaddr = kmap(page);
			left = __copy_from_user(kaddr + offset, buf, bytes);
			kunmap(page);
		}

		written += bytes;
		count -= bytes;
		pos += bytes;
		buf += bytes;
		if (pos > inode->i_size)
			i_size_write(inode, pos);

		flush_dcache_page(page);
		set_page_dirty(page);
		mark_page_accessed(page);
		page_cache_release(page);

		if (left) {
			pos -= left;
			written -= left;
			err = -EFAULT;
			break;
		}

		/*
		 * Our dirty pages are not counted in nr_dirty,
		 * and we do not attempt to balance dirty pages.
		 */

		cond_resched();
	} while (count);

	*ppos = pos;
	if (written)
		err = written;
out:
	up(&inode->i_sem);
	return err;
}

static void do_shmem_file_read(struct file *filp, loff_t *ppos, read_descriptor_t *desc, read_actor_t actor)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct address_space *mapping = inode->i_mapping;
	unsigned long index, offset;

	index = *ppos >> PAGE_CACHE_SHIFT;
	offset = *ppos & ~PAGE_CACHE_MASK;

	for (;;) {
		struct page *page = NULL;
		unsigned long end_index, nr, ret;
		loff_t i_size = i_size_read(inode);

		end_index = i_size >> PAGE_CACHE_SHIFT;
		if (index > end_index)
			break;
		if (index == end_index) {
			nr = i_size & ~PAGE_CACHE_MASK;
			if (nr <= offset)
				break;
		}

		desc->error = shmem_getpage(inode, index, &page, SGP_READ, NULL);
		if (desc->error) {
			if (desc->error == -EINVAL)
				desc->error = 0;
			break;
		}

		/*
		 * We must evaluate after, since reads (unlike writes)
		 * are called without i_sem protection against truncate
		 */
		nr = PAGE_CACHE_SIZE;
		i_size = i_size_read(inode);
		end_index = i_size >> PAGE_CACHE_SHIFT;
		if (index == end_index) {
			nr = i_size & ~PAGE_CACHE_MASK;
			if (nr <= offset) {
				page_cache_release(page);
				break;
			}
		}
		nr -= offset;

		if (page != ZERO_PAGE(0)) {
			/*
			 * If users can be writing to this page using arbitrary
			 * virtual addresses, take care about potential aliasing
			 * before reading the page on the kernel side.
			 */
			if (mapping_writably_mapped(mapping))
				flush_dcache_page(page);
			/*
			 * Mark the page accessed if we read the beginning.
			 */
			if (!offset)
				mark_page_accessed(page);
		}

		/*
		 * Ok, we have the page, and it's up-to-date, so
		 * now we can copy it to user space...
		 *
		 * The actor routine returns how many bytes were actually used..
		 * NOTE! This may not be the same as how much of a user buffer
		 * we filled up (we may be padding etc), so we can only update
		 * "pos" here (the actor routine has to update the user buffer
		 * pointers and the remaining count).
		 */
		ret = actor(desc, page, offset, nr);
		offset += ret;
		index += offset >> PAGE_CACHE_SHIFT;
		offset &= ~PAGE_CACHE_MASK;

		page_cache_release(page);
		if (ret != nr || !desc->count)
			break;

		cond_resched();
	}

	*ppos = ((loff_t) index << PAGE_CACHE_SHIFT) + offset;
	file_accessed(filp);
}

static ssize_t shmem_file_read(struct file *filp, char __user *buf, size_t count, loff_t *ppos)
{
	read_descriptor_t desc;

	if ((ssize_t) count < 0)
		return -EINVAL;
	if (!access_ok(VERIFY_WRITE, buf, count))
		return -EFAULT;
	if (!count)
		return 0;

	desc.written = 0;
	desc.count = count;
	desc.arg.buf = buf;
	desc.error = 0;

	do_shmem_file_read(filp, ppos, &desc, file_read_actor);
	if (desc.written)
		return desc.written;
	return desc.error;
}

static ssize_t shmem_file_sendfile(struct file *in_file, loff_t *ppos,
			 size_t count, read_actor_t actor, void *target)
{
	read_descriptor_t desc;

	if (!count)
		return 0;

	desc.written = 0;
	desc.count = count;
	desc.arg.data = target;
	desc.error = 0;

	do_shmem_file_read(in_file, ppos, &desc, actor);
	if (desc.written)
		return desc.written;
	return desc.error;
}

static int shmem_statfs(struct super_block *sb, struct kstatfs *buf)
{
	struct shmem_sb_info *sbinfo = SHMEM_SB(sb);

	buf->f_type = TMPFS_MAGIC;
	buf->f_bsize = PAGE_CACHE_SIZE;
	spin_lock(&sbinfo->stat_lock);
	buf->f_blocks = sbinfo->max_blocks;
	buf->f_bavail = buf->f_bfree = sbinfo->free_blocks;
	buf->f_files = sbinfo->max_inodes;
	buf->f_ffree = sbinfo->free_inodes;
	spin_unlock(&sbinfo->stat_lock);
	buf->f_namelen = NAME_MAX;
	return 0;
}

/*
 * File creation. Allocate an inode, and we're done..
 */
static int
shmem_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t dev)
{
	struct inode *inode = shmem_get_inode(dir->i_sb, mode, dev);
	int error = -ENOSPC;

	if (inode) {
		if (dir->i_mode & S_ISGID) {
			inode->i_gid = dir->i_gid;
			if (S_ISDIR(mode))
				inode->i_mode |= S_ISGID;
		}
		dir->i_size += BOGO_DIRENT_SIZE;
		dir->i_ctime = dir->i_mtime = CURRENT_TIME;
		d_instantiate(dentry, inode);
		dget(dentry); /* Extra count - pin the dentry in core */
		error = 0;
	}
	return error;
}

static int shmem_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	int error;

	if ((error = shmem_mknod(dir, dentry, mode | S_IFDIR, 0)))
		return error;
	dir->i_nlink++;
	return 0;
}

static int shmem_create(struct inode *dir, struct dentry *dentry, int mode,
		struct nameidata *nd)
{
	return shmem_mknod(dir, dentry, mode | S_IFREG, 0);
}

/*
 * Link a file..
 */
static int shmem_link(struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = old_dentry->d_inode;

	dir->i_size += BOGO_DIRENT_SIZE;
	inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	inode->i_nlink++;
	atomic_inc(&inode->i_count);	/* New dentry reference */
	dget(dentry);		/* Extra pinning count for the created dentry */
	d_instantiate(dentry, inode);
	return 0;
}

static int shmem_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;

	dir->i_size -= BOGO_DIRENT_SIZE;
	inode->i_ctime = dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	inode->i_nlink--;
	dput(dentry);	/* Undo the count from "create" - this does all the work */
	return 0;
}

static int shmem_rmdir(struct inode *dir, struct dentry *dentry)
{
	if (!simple_empty(dentry))
		return -ENOTEMPTY;

	dir->i_nlink--;
	return shmem_unlink(dir, dentry);
}

/*
 * The VFS layer already does all the dentry stuff for rename,
 * we just have to decrement the usage count for the target if
 * it exists so that the VFS layer correctly free's it when it
 * gets overwritten.
 */
static int shmem_rename(struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir, struct dentry *new_dentry)
{
	struct inode *inode = old_dentry->d_inode;
	int they_are_dirs = S_ISDIR(inode->i_mode);

	if (!simple_empty(new_dentry))
		return -ENOTEMPTY;

	if (new_dentry->d_inode) {
		(void) shmem_unlink(new_dir, new_dentry);
		if (they_are_dirs)
			old_dir->i_nlink--;
	} else if (they_are_dirs) {
		old_dir->i_nlink--;
		new_dir->i_nlink++;
	}

	old_dir->i_size -= BOGO_DIRENT_SIZE;
	new_dir->i_size += BOGO_DIRENT_SIZE;
	old_dir->i_ctime = old_dir->i_mtime =
	new_dir->i_ctime = new_dir->i_mtime =
	inode->i_ctime = CURRENT_TIME;
	return 0;
}

static int shmem_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	int error;
	int len;
	struct inode *inode;
	struct page *page = NULL;
	char *kaddr;
	struct shmem_inode_info *info;

	len = strlen(symname) + 1;
	if (len > PAGE_CACHE_SIZE)
		return -ENAMETOOLONG;

	inode = shmem_get_inode(dir->i_sb, S_IFLNK|S_IRWXUGO, 0);
	if (!inode)
		return -ENOSPC;

	info = SHMEM_I(inode);
	inode->i_size = len-1;
	if (len <= (char *)inode - (char *)info) {
		/* do it inline */
		memcpy(info, symname, len);
		inode->i_op = &shmem_symlink_inline_operations;
	} else {
		error = shmem_getpage(inode, 0, &page, SGP_WRITE, NULL);
		if (error) {
			iput(inode);
			return error;
		}
		inode->i_op = &shmem_symlink_inode_operations;
		spin_lock(&shmem_ilock);
		list_add_tail(&info->list, &shmem_inodes);
		spin_unlock(&shmem_ilock);
		kaddr = kmap_atomic(page, KM_USER0);
		memcpy(kaddr, symname, len);
		kunmap_atomic(kaddr, KM_USER0);
		set_page_dirty(page);
		page_cache_release(page);
	}
	if (dir->i_mode & S_ISGID)
		inode->i_gid = dir->i_gid;
	dir->i_size += BOGO_DIRENT_SIZE;
	dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	d_instantiate(dentry, inode);
	dget(dentry);
	return 0;
}

static int shmem_follow_link_inline(struct dentry *dentry, struct nameidata *nd)
{
	nd_set_link(nd, (char *)SHMEM_I(dentry->d_inode));
	return 0;
}

static int shmem_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	struct page *page = NULL;
	int res = shmem_getpage(dentry->d_inode, 0, &page, SGP_READ, NULL);
	nd_set_link(nd, res ? ERR_PTR(res) : kmap(page));
	return 0;
}

static void shmem_put_link(struct dentry *dentry, struct nameidata *nd)
{
	if (!IS_ERR(nd_get_link(nd))) {
		struct page *page;

		page = find_get_page(dentry->d_inode->i_mapping, 0);
		if (!page)
			BUG();
		kunmap(page);
		mark_page_accessed(page);
		page_cache_release(page);
		page_cache_release(page);
	}
}

static struct inode_operations shmem_symlink_inline_operations = {
	.readlink	= generic_readlink,
	.follow_link	= shmem_follow_link_inline,
};

static struct inode_operations shmem_symlink_inode_operations = {
	.truncate	= shmem_truncate,
	.readlink	= generic_readlink,
	.follow_link	= shmem_follow_link,
	.put_link	= shmem_put_link,
};

static int shmem_parse_options(char *options, int *mode, uid_t *uid, gid_t *gid, unsigned long *blocks, unsigned long *inodes)
{
	char *this_char, *value, *rest;

	while ((this_char = strsep(&options, ",")) != NULL) {
		if (!*this_char)
			continue;
		if ((value = strchr(this_char,'=')) != NULL) {
			*value++ = 0;
		} else {
			printk(KERN_ERR
			    "tmpfs: No value for mount option '%s'\n",
			    this_char);
			return 1;
		}

		if (!strcmp(this_char,"size")) {
			unsigned long long size;
			size = memparse(value,&rest);
			if (*rest == '%') {
				size <<= PAGE_SHIFT;
				size *= totalram_pages;
				do_div(size, 100);
				rest++;
			}
			if (*rest)
				goto bad_val;
			*blocks = size >> PAGE_CACHE_SHIFT;
		} else if (!strcmp(this_char,"nr_blocks")) {
			*blocks = memparse(value,&rest);
			if (*rest)
				goto bad_val;
		} else if (!strcmp(this_char,"nr_inodes")) {
			*inodes = memparse(value,&rest);
			if (*rest)
				goto bad_val;
		} else if (!strcmp(this_char,"mode")) {
			if (!mode)
				continue;
			*mode = simple_strtoul(value,&rest,8);
			if (*rest)
				goto bad_val;
		} else if (!strcmp(this_char,"uid")) {
			if (!uid)
				continue;
			*uid = simple_strtoul(value,&rest,0);
			if (*rest)
				goto bad_val;
		} else if (!strcmp(this_char,"gid")) {
			if (!gid)
				continue;
			*gid = simple_strtoul(value,&rest,0);
			if (*rest)
				goto bad_val;
		} else {
			printk(KERN_ERR "tmpfs: Bad mount option %s\n",
			       this_char);
			return 1;
		}
	}
	return 0;

bad_val:
	printk(KERN_ERR "tmpfs: Bad value '%s' for mount option '%s'\n",
	       value, this_char);
	return 1;

}

static int shmem_remount_fs(struct super_block *sb, int *flags, char *data)
{
	struct shmem_sb_info *sbinfo = SHMEM_SB(sb);
	unsigned long max_blocks = sbinfo->max_blocks;
	unsigned long max_inodes = sbinfo->max_inodes;

	if (shmem_parse_options(data, NULL, NULL, NULL, &max_blocks, &max_inodes))
		return -EINVAL;
	return shmem_set_size(sbinfo, max_blocks, max_inodes);
}
#endif

static int shmem_fill_super(struct super_block *sb,
			    void *data, int silent)
{
	struct inode *inode;
	struct dentry *root;
	unsigned long blocks, inodes;
	int mode   = S_IRWXUGO | S_ISVTX;
	uid_t uid = current->fsuid;
	gid_t gid = current->fsgid;
	struct shmem_sb_info *sbinfo;
	int err = -ENOMEM;

	sbinfo = kmalloc(sizeof(struct shmem_sb_info), GFP_KERNEL);
	if (!sbinfo)
		return -ENOMEM;
	sb->s_fs_info = sbinfo;
	memset(sbinfo, 0, sizeof(struct shmem_sb_info));

	/*
	 * Per default we only allow half of the physical ram per
	 * tmpfs instance
	 */
	blocks = inodes = totalram_pages / 2;

#ifdef CONFIG_TMPFS
	if (shmem_parse_options(data, &mode, &uid, &gid, &blocks, &inodes)) {
		err = -EINVAL;
		goto failed;
	}
#else
	sb->s_flags |= MS_NOUSER;
#endif

	spin_lock_init(&sbinfo->stat_lock);
	sbinfo->max_blocks = blocks;
	sbinfo->free_blocks = blocks;
	sbinfo->max_inodes = inodes;
	sbinfo->free_inodes = inodes;
	sb->s_maxbytes = SHMEM_MAX_BYTES;
	sb->s_blocksize = PAGE_CACHE_SIZE;
	sb->s_blocksize_bits = PAGE_CACHE_SHIFT;
	sb->s_magic = TMPFS_MAGIC;
	sb->s_op = &shmem_ops;
	inode = shmem_get_inode(sb, S_IFDIR | mode, 0);
	if (!inode)
		goto failed;
	inode->i_uid = uid;
	inode->i_gid = gid;
	root = d_alloc_root(inode);
	if (!root)
		goto failed_iput;
	sb->s_root = root;
	return 0;

failed_iput:
	iput(inode);
failed:
	kfree(sbinfo);
	sb->s_fs_info = NULL;
	return err;
}

static void shmem_put_super(struct super_block *sb)
{
	kfree(sb->s_fs_info);
	sb->s_fs_info = NULL;
}

static kmem_cache_t *shmem_inode_cachep;

static struct inode *shmem_alloc_inode(struct super_block *sb)
{
	struct shmem_inode_info *p;
	p = (struct shmem_inode_info *)kmem_cache_alloc(shmem_inode_cachep, SLAB_KERNEL);
	if (!p)
		return NULL;
	return &p->vfs_inode;
}

static void shmem_destroy_inode(struct inode *inode)
{
	mpol_free_shared_policy(&SHMEM_I(inode)->policy);
	kmem_cache_free(shmem_inode_cachep, SHMEM_I(inode));
}

static void init_once(void *foo, kmem_cache_t *cachep, unsigned long flags)
{
	struct shmem_inode_info *p = (struct shmem_inode_info *) foo;

	if ((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) ==
	    SLAB_CTOR_CONSTRUCTOR) {
		inode_init_once(&p->vfs_inode);
	}
}

static int init_inodecache(void)
{
	shmem_inode_cachep = kmem_cache_create("shmem_inode_cache",
				sizeof(struct shmem_inode_info),
				0, SLAB_HWCACHE_ALIGN|SLAB_RECLAIM_ACCOUNT,
				init_once, NULL);
	if (shmem_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	if (kmem_cache_destroy(shmem_inode_cachep))
		printk(KERN_INFO "shmem_inode_cache: not all structures were freed\n");
}

static struct address_space_operations shmem_aops = {
	.writepage	= shmem_writepage,
	.set_page_dirty	= __set_page_dirty_nobuffers,
#ifdef CONFIG_TMPFS
	.prepare_write	= shmem_prepare_write,
	.commit_write	= simple_commit_write,
#endif
};

static struct file_operations shmem_file_operations = {
	.mmap		= shmem_mmap,
#ifdef CONFIG_TMPFS
	.llseek		= generic_file_llseek,
	.read		= shmem_file_read,
	.write		= shmem_file_write,
	.fsync		= simple_sync_file,
	.sendfile	= shmem_file_sendfile,
#endif
};

static struct inode_operations shmem_inode_operations = {
	.truncate	= shmem_truncate,
	.setattr	= shmem_notify_change,
};

static struct inode_operations shmem_dir_inode_operations = {
#ifdef CONFIG_TMPFS
	.create		= shmem_create,
	.lookup		= simple_lookup,
	.link		= shmem_link,
	.unlink		= shmem_unlink,
	.symlink	= shmem_symlink,
	.mkdir		= shmem_mkdir,
	.rmdir		= shmem_rmdir,
	.mknod		= shmem_mknod,
	.rename		= shmem_rename,
#endif
};

static struct super_operations shmem_ops = {
	.alloc_inode	= shmem_alloc_inode,
	.destroy_inode	= shmem_destroy_inode,
#ifdef CONFIG_TMPFS
	.statfs		= shmem_statfs,
	.remount_fs	= shmem_remount_fs,
#endif
	.delete_inode	= shmem_delete_inode,
	.drop_inode	= generic_delete_inode,
	.put_super	= shmem_put_super,
};

static struct vm_operations_struct shmem_vm_ops = {
	.nopage		= shmem_nopage,
	.populate	= shmem_populate,
#ifdef CONFIG_NUMA
	.set_policy     = shmem_set_policy,
	.get_policy     = shmem_get_policy,
#endif
};

static struct super_block *shmem_get_sb(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return get_sb_nodev(fs_type, flags, data, shmem_fill_super);
}

static struct file_system_type tmpfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "tmpfs",
	.get_sb		= shmem_get_sb,
	.kill_sb	= kill_litter_super,
};
static struct vfsmount *shm_mnt;

static int __init init_tmpfs(void)
{
	int error;

	error = init_inodecache();
	if (error)
		goto out3;

	error = register_filesystem(&tmpfs_fs_type);
	if (error) {
		printk(KERN_ERR "Could not register tmpfs\n");
		goto out2;
	}
#ifdef CONFIG_TMPFS
	devfs_mk_dir("shm");
#endif
	shm_mnt = kern_mount(&tmpfs_fs_type);
	if (IS_ERR(shm_mnt)) {
		error = PTR_ERR(shm_mnt);
		printk(KERN_ERR "Could not kern_mount tmpfs\n");
		goto out1;
	}

	/* The internal instance should not do size checking */
	shmem_set_size(SHMEM_SB(shm_mnt->mnt_sb), ULONG_MAX, ULONG_MAX);
	return 0;

out1:
	unregister_filesystem(&tmpfs_fs_type);
out2:
	destroy_inodecache();
out3:
	shm_mnt = ERR_PTR(error);
	return error;
}
module_init(init_tmpfs)

/*
 * shmem_file_setup - get an unlinked file living in tmpfs
 *
 * @name: name for dentry (to be seen in /proc/<pid>/maps
 * @size: size to be set for the file
 *
 */
struct file *shmem_file_setup(char *name, loff_t size, unsigned long flags)
{
	int error;
	struct file *file;
	struct inode *inode;
	struct dentry *dentry, *root;
	struct qstr this;

	if (IS_ERR(shm_mnt))
		return (void *)shm_mnt;

	if (size > SHMEM_MAX_BYTES)
		return ERR_PTR(-EINVAL);

	if (shmem_acct_size(flags, size))
		return ERR_PTR(-ENOMEM);

	error = -ENOMEM;
	this.name = name;
	this.len = strlen(name);
	this.hash = 0; /* will go */
	root = shm_mnt->mnt_root;
	dentry = d_alloc(root, &this);
	if (!dentry)
		goto put_memory;

	error = -ENFILE;
	file = get_empty_filp();
	if (!file)
		goto put_dentry;

	error = -ENOSPC;
	inode = shmem_get_inode(root->d_sb, S_IFREG | S_IRWXUGO, 0);
	if (!inode)
		goto close_file;

	SHMEM_I(inode)->flags = flags & VM_ACCOUNT;
	d_instantiate(dentry, inode);
	inode->i_size = size;
	inode->i_nlink = 0;	/* It is unlinked */
	file->f_vfsmnt = mntget(shm_mnt);
	file->f_dentry = dentry;
	file->f_mapping = inode->i_mapping;
	file->f_op = &shmem_file_operations;
	file->f_mode = FMODE_WRITE | FMODE_READ;
	return(file);

close_file:
	put_filp(file);
put_dentry:
	dput(dentry);
put_memory:
	shmem_unacct_size(flags, size);
	return ERR_PTR(error);
}

/*
 * shmem_zero_setup - setup a shared anonymous mapping
 *
 * @vma: the vma to be mmapped is prepared by do_mmap_pgoff
 */
int shmem_zero_setup(struct vm_area_struct *vma)
{
	struct file *file;
	loff_t size = vma->vm_end - vma->vm_start;

	file = shmem_file_setup("dev/zero", size, vma->vm_flags);
	if (IS_ERR(file))
		return PTR_ERR(file);

	if (vma->vm_file)
		fput(vma->vm_file);
	vma->vm_file = file;
	vma->vm_ops = &shmem_vm_ops;
	return 0;
}

EXPORT_SYMBOL(shmem_file_setup);
