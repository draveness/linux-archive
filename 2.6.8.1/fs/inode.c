/*
 * linux/fs/inode.c
 *
 * (C) 1997 Linus Torvalds
 */

#include <linux/config.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/dcache.h>
#include <linux/init.h>
#include <linux/quotaops.h>
#include <linux/slab.h>
#include <linux/writeback.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/wait.h>
#include <linux/hash.h>
#include <linux/swap.h>
#include <linux/security.h>
#include <linux/pagemap.h>
#include <linux/cdev.h>
#include <linux/bootmem.h>

/*
 * This is needed for the following functions:
 *  - inode_has_buffers
 *  - invalidate_inode_buffers
 *  - fsync_bdev
 *  - invalidate_bdev
 *
 * FIXME: remove all knowledge of the buffer layer from this file
 */
#include <linux/buffer_head.h>

/*
 * New inode.c implementation.
 *
 * This implementation has the basic premise of trying
 * to be extremely low-overhead and SMP-safe, yet be
 * simple enough to be "obviously correct".
 *
 * Famous last words.
 */

/* inode dynamic allocation 1999, Andrea Arcangeli <andrea@suse.de> */

/* #define INODE_PARANOIA 1 */
/* #define INODE_DEBUG 1 */

/*
 * Inode lookup is no longer as critical as it used to be:
 * most of the lookups are going to be through the dcache.
 */
#define I_HASHBITS	i_hash_shift
#define I_HASHMASK	i_hash_mask

static unsigned int i_hash_mask;
static unsigned int i_hash_shift;

/*
 * Each inode can be on two separate lists. One is
 * the hash list of the inode, used for lookups. The
 * other linked list is the "type" list:
 *  "in_use" - valid inode, i_count > 0, i_nlink > 0
 *  "dirty"  - as "in_use" but also dirty
 *  "unused" - valid inode, i_count = 0
 *
 * A "dirty" list is maintained for each super block,
 * allowing for low-overhead inode sync() operations.
 */

LIST_HEAD(inode_in_use);
LIST_HEAD(inode_unused);
static struct hlist_head *inode_hashtable;

/*
 * A simple spinlock to protect the list manipulations.
 *
 * NOTE! You also have to own the lock if you change
 * the i_state of an inode while it is in use..
 */
spinlock_t inode_lock = SPIN_LOCK_UNLOCKED;

/*
 * iprune_sem provides exclusion between the kswapd or try_to_free_pages
 * icache shrinking path, and the umount path.  Without this exclusion,
 * by the time prune_icache calls iput for the inode whose pages it has
 * been invalidating, or by the time it calls clear_inode & destroy_inode
 * from its final dispose_list, the struct super_block they refer to
 * (for inode->i_sb->s_op) may already have been freed and reused.
 */
DECLARE_MUTEX(iprune_sem);

/*
 * Statistics gathering..
 */
struct inodes_stat_t inodes_stat;

static kmem_cache_t * inode_cachep;

static struct inode *alloc_inode(struct super_block *sb)
{
	static struct address_space_operations empty_aops;
	static struct inode_operations empty_iops;
	static struct file_operations empty_fops;
	struct inode *inode;

	if (sb->s_op->alloc_inode)
		inode = sb->s_op->alloc_inode(sb);
	else
		inode = (struct inode *) kmem_cache_alloc(inode_cachep, SLAB_KERNEL);

	if (inode) {
		struct address_space * const mapping = &inode->i_data;

		inode->i_sb = sb;
		inode->i_blkbits = sb->s_blocksize_bits;
		inode->i_flags = 0;
		atomic_set(&inode->i_count, 1);
		inode->i_sock = 0;
		inode->i_op = &empty_iops;
		inode->i_fop = &empty_fops;
		inode->i_nlink = 1;
		atomic_set(&inode->i_writecount, 0);
		inode->i_size = 0;
		inode->i_blocks = 0;
		inode->i_bytes = 0;
		inode->i_generation = 0;
#ifdef CONFIG_QUOTA
		memset(&inode->i_dquot, 0, sizeof(inode->i_dquot));
#endif
		inode->i_pipe = NULL;
		inode->i_bdev = NULL;
		inode->i_cdev = NULL;
		inode->i_rdev = 0;
		inode->i_security = NULL;
		inode->dirtied_when = 0;
		if (security_inode_alloc(inode)) {
			if (inode->i_sb->s_op->destroy_inode)
				inode->i_sb->s_op->destroy_inode(inode);
			else
				kmem_cache_free(inode_cachep, (inode));
			return NULL;
		}

		mapping->a_ops = &empty_aops;
 		mapping->host = inode;
		mapping->flags = 0;
		mapping_set_gfp_mask(mapping, GFP_HIGHUSER);
		mapping->assoc_mapping = NULL;
		mapping->backing_dev_info = &default_backing_dev_info;

		/*
		 * If the block_device provides a backing_dev_info for client
		 * inodes then use that.  Otherwise the inode share the bdev's
		 * backing_dev_info.
		 */
		if (sb->s_bdev) {
			struct backing_dev_info *bdi;

			bdi = sb->s_bdev->bd_inode_backing_dev_info;
			if (!bdi)
				bdi = sb->s_bdev->bd_inode->i_mapping->backing_dev_info;
			mapping->backing_dev_info = bdi;
		}
		memset(&inode->u, 0, sizeof(inode->u));
		inode->i_mapping = mapping;
	}
	return inode;
}

void destroy_inode(struct inode *inode) 
{
	if (inode_has_buffers(inode))
		BUG();
	security_inode_free(inode);
	if (inode->i_sb->s_op->destroy_inode)
		inode->i_sb->s_op->destroy_inode(inode);
	else
		kmem_cache_free(inode_cachep, (inode));
}


/*
 * These are initializations that only need to be done
 * once, because the fields are idempotent across use
 * of the inode, so let the slab aware of that.
 */
void inode_init_once(struct inode *inode)
{
	memset(inode, 0, sizeof(*inode));
	INIT_HLIST_NODE(&inode->i_hash);
	INIT_LIST_HEAD(&inode->i_dentry);
	INIT_LIST_HEAD(&inode->i_devices);
	sema_init(&inode->i_sem, 1);
	init_rwsem(&inode->i_alloc_sem);
	INIT_RADIX_TREE(&inode->i_data.page_tree, GFP_ATOMIC);
	spin_lock_init(&inode->i_data.tree_lock);
	spin_lock_init(&inode->i_data.i_mmap_lock);
	atomic_set(&inode->i_data.truncate_count, 0);
	INIT_LIST_HEAD(&inode->i_data.private_list);
	spin_lock_init(&inode->i_data.private_lock);
	INIT_PRIO_TREE_ROOT(&inode->i_data.i_mmap);
	INIT_LIST_HEAD(&inode->i_data.i_mmap_nonlinear);
	spin_lock_init(&inode->i_lock);
	i_size_ordered_init(inode);
}

EXPORT_SYMBOL(inode_init_once);

static void init_once(void * foo, kmem_cache_t * cachep, unsigned long flags)
{
	struct inode * inode = (struct inode *) foo;

	if ((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) ==
	    SLAB_CTOR_CONSTRUCTOR)
		inode_init_once(inode);
}

/*
 * inode_lock must be held
 */
void __iget(struct inode * inode)
{
	if (atomic_read(&inode->i_count)) {
		atomic_inc(&inode->i_count);
		return;
	}
	atomic_inc(&inode->i_count);
	if (!(inode->i_state & (I_DIRTY|I_LOCK)))
		list_move(&inode->i_list, &inode_in_use);
	inodes_stat.nr_unused--;
}

/**
 * clear_inode - clear an inode
 * @inode: inode to clear
 *
 * This is called by the filesystem to tell us
 * that the inode is no longer useful. We just
 * terminate it with extreme prejudice.
 */
void clear_inode(struct inode *inode)
{
	invalidate_inode_buffers(inode);
       
	if (inode->i_data.nrpages)
		BUG();
	if (!(inode->i_state & I_FREEING))
		BUG();
	if (inode->i_state & I_CLEAR)
		BUG();
	wait_on_inode(inode);
	DQUOT_DROP(inode);
	if (inode->i_sb && inode->i_sb->s_op->clear_inode)
		inode->i_sb->s_op->clear_inode(inode);
	if (inode->i_bdev)
		bd_forget(inode);
	if (inode->i_cdev)
		cd_forget(inode);
	inode->i_state = I_CLEAR;
}

EXPORT_SYMBOL(clear_inode);

/*
 * dispose_list - dispose of the contents of a local list
 * @head: the head of the list to free
 *
 * Dispose-list gets a local list with local inodes in it, so it doesn't
 * need to worry about list corruption and SMP locks.
 */
static void dispose_list(struct list_head *head)
{
	int nr_disposed = 0;

	while (!list_empty(head)) {
		struct inode *inode;

		inode = list_entry(head->next, struct inode, i_list);
		list_del(&inode->i_list);

		if (inode->i_data.nrpages)
			truncate_inode_pages(&inode->i_data, 0);
		clear_inode(inode);
		destroy_inode(inode);
		nr_disposed++;
	}
	spin_lock(&inode_lock);
	inodes_stat.nr_inodes -= nr_disposed;
	spin_unlock(&inode_lock);
}

/*
 * Invalidate all inodes for a device.
 */
static int invalidate_list(struct list_head *head, struct super_block * sb, struct list_head * dispose)
{
	struct list_head *next;
	int busy = 0, count = 0;

	next = head->next;
	for (;;) {
		struct list_head * tmp = next;
		struct inode * inode;

		next = next->next;
		if (tmp == head)
			break;
		inode = list_entry(tmp, struct inode, i_list);
		if (inode->i_sb != sb)
			continue;
		invalidate_inode_buffers(inode);
		if (!atomic_read(&inode->i_count)) {
			hlist_del_init(&inode->i_hash);
			list_move(&inode->i_list, dispose);
			inode->i_state |= I_FREEING;
			count++;
			continue;
		}
		busy = 1;
	}
	/* only unused inodes may be cached with i_count zero */
	inodes_stat.nr_unused -= count;
	return busy;
}

/*
 * This is a two-stage process. First we collect all
 * offending inodes onto the throw-away list, and in
 * the second stage we actually dispose of them. This
 * is because we don't want to sleep while messing
 * with the global lists..
 */
 
/**
 *	invalidate_inodes	- discard the inodes on a device
 *	@sb: superblock
 *
 *	Discard all of the inodes for a given superblock. If the discard
 *	fails because there are busy inodes then a non zero value is returned.
 *	If the discard is successful all the inodes have been discarded.
 */
int invalidate_inodes(struct super_block * sb)
{
	int busy;
	LIST_HEAD(throw_away);

	down(&iprune_sem);
	spin_lock(&inode_lock);
	busy = invalidate_list(&inode_in_use, sb, &throw_away);
	busy |= invalidate_list(&inode_unused, sb, &throw_away);
	busy |= invalidate_list(&sb->s_dirty, sb, &throw_away);
	busy |= invalidate_list(&sb->s_io, sb, &throw_away);
	spin_unlock(&inode_lock);

	dispose_list(&throw_away);
	up(&iprune_sem);

	return busy;
}

EXPORT_SYMBOL(invalidate_inodes);
 
int __invalidate_device(struct block_device *bdev, int do_sync)
{
	struct super_block *sb;
	int res;

	if (do_sync)
		fsync_bdev(bdev);

	res = 0;
	sb = get_super(bdev);
	if (sb) {
		/*
		 * no need to lock the super, get_super holds the
		 * read semaphore so the filesystem cannot go away
		 * under us (->put_super runs with the write lock
		 * hold).
		 */
		shrink_dcache_sb(sb);
		res = invalidate_inodes(sb);
		drop_super(sb);
	}
	invalidate_bdev(bdev, 0);
	return res;
}

EXPORT_SYMBOL(__invalidate_device);

static int can_unuse(struct inode *inode)
{
	if (inode->i_state)
		return 0;
	if (inode_has_buffers(inode))
		return 0;
	if (atomic_read(&inode->i_count))
		return 0;
	if (inode->i_data.nrpages)
		return 0;
	return 1;
}

/*
 * Scan `goal' inodes on the unused list for freeable ones. They are moved to
 * a temporary list and then are freed outside inode_lock by dispose_list().
 *
 * Any inodes which are pinned purely because of attached pagecache have their
 * pagecache removed.  We expect the final iput() on that inode to add it to
 * the front of the inode_unused list.  So look for it there and if the
 * inode is still freeable, proceed.  The right inode is found 99.9% of the
 * time in testing on a 4-way.
 *
 * If the inode has metadata buffers attached to mapping->private_list then
 * try to remove them.
 */
static void prune_icache(int nr_to_scan)
{
	LIST_HEAD(freeable);
	int nr_pruned = 0;
	int nr_scanned;
	unsigned long reap = 0;

	down(&iprune_sem);
	spin_lock(&inode_lock);
	for (nr_scanned = 0; nr_scanned < nr_to_scan; nr_scanned++) {
		struct inode *inode;

		if (list_empty(&inode_unused))
			break;

		inode = list_entry(inode_unused.prev, struct inode, i_list);

		if (inode->i_state || atomic_read(&inode->i_count)) {
			list_move(&inode->i_list, &inode_unused);
			continue;
		}
		if (inode_has_buffers(inode) || inode->i_data.nrpages) {
			__iget(inode);
			spin_unlock(&inode_lock);
			if (remove_inode_buffers(inode))
				reap += invalidate_inode_pages(&inode->i_data);
			iput(inode);
			spin_lock(&inode_lock);

			if (inode != list_entry(inode_unused.next,
						struct inode, i_list))
				continue;	/* wrong inode or list_empty */
			if (!can_unuse(inode))
				continue;
		}
		hlist_del_init(&inode->i_hash);
		list_move(&inode->i_list, &freeable);
		inode->i_state |= I_FREEING;
		nr_pruned++;
	}
	inodes_stat.nr_unused -= nr_pruned;
	spin_unlock(&inode_lock);

	dispose_list(&freeable);
	up(&iprune_sem);

	if (current_is_kswapd())
		mod_page_state(kswapd_inodesteal, reap);
	else
		mod_page_state(pginodesteal, reap);
}

/*
 * shrink_icache_memory() will attempt to reclaim some unused inodes.  Here,
 * "unused" means that no dentries are referring to the inodes: the files are
 * not open and the dcache references to those inodes have already been
 * reclaimed.
 *
 * This function is passed the number of inodes to scan, and it returns the
 * total number of remaining possibly-reclaimable inodes.
 */
static int shrink_icache_memory(int nr, unsigned int gfp_mask)
{
	if (nr) {
		/*
		 * Nasty deadlock avoidance.  We may hold various FS locks,
		 * and we don't want to recurse into the FS that called us
		 * in clear_inode() and friends..
	 	 */
		if (gfp_mask & __GFP_FS)
			prune_icache(nr);
	}
	return (inodes_stat.nr_unused / 100) * sysctl_vfs_cache_pressure;
}

static void __wait_on_freeing_inode(struct inode *inode);
/*
 * Called with the inode lock held.
 * NOTE: we are not increasing the inode-refcount, you must call __iget()
 * by hand after calling find_inode now! This simplifies iunique and won't
 * add any additional branch in the common code.
 */
static struct inode * find_inode(struct super_block * sb, struct hlist_head *head, int (*test)(struct inode *, void *), void *data)
{
	struct hlist_node *node;
	struct inode * inode = NULL;

repeat:
	hlist_for_each (node, head) { 
		inode = hlist_entry(node, struct inode, i_hash);
		if (inode->i_sb != sb)
			continue;
		if (!test(inode, data))
			continue;
		if (inode->i_state & (I_FREEING|I_CLEAR)) {
			__wait_on_freeing_inode(inode);
			goto repeat;
		}
		break;
	}
	return node ? inode : NULL;
}

/*
 * find_inode_fast is the fast path version of find_inode, see the comment at
 * iget_locked for details.
 */
static struct inode * find_inode_fast(struct super_block * sb, struct hlist_head *head, unsigned long ino)
{
	struct hlist_node *node;
	struct inode * inode = NULL;

repeat:
	hlist_for_each (node, head) {
		inode = hlist_entry(node, struct inode, i_hash);
		if (inode->i_ino != ino)
			continue;
		if (inode->i_sb != sb)
			continue;
		if (inode->i_state & (I_FREEING|I_CLEAR)) {
			__wait_on_freeing_inode(inode);
			goto repeat;
		}
		break;
	}
	return node ? inode : NULL;
}

/**
 *	new_inode 	- obtain an inode
 *	@sb: superblock
 *
 *	Allocates a new inode for given superblock.
 */
struct inode *new_inode(struct super_block *sb)
{
	static unsigned long last_ino;
	struct inode * inode;

	spin_lock_prefetch(&inode_lock);
	
	inode = alloc_inode(sb);
	if (inode) {
		spin_lock(&inode_lock);
		inodes_stat.nr_inodes++;
		list_add(&inode->i_list, &inode_in_use);
		inode->i_ino = ++last_ino;
		inode->i_state = 0;
		spin_unlock(&inode_lock);
	}
	return inode;
}

EXPORT_SYMBOL(new_inode);

void unlock_new_inode(struct inode *inode)
{
	/*
	 * This is special!  We do not need the spinlock
	 * when clearing I_LOCK, because we're guaranteed
	 * that nobody else tries to do anything about the
	 * state of the inode when it is locked, as we
	 * just created it (so there can be no old holders
	 * that haven't tested I_LOCK).
	 */
	inode->i_state &= ~(I_LOCK|I_NEW);
	wake_up_inode(inode);
}

EXPORT_SYMBOL(unlock_new_inode);

/*
 * This is called without the inode lock held.. Be careful.
 *
 * We no longer cache the sb_flags in i_flags - see fs.h
 *	-- rmk@arm.uk.linux.org
 */
static struct inode * get_new_inode(struct super_block *sb, struct hlist_head *head, int (*test)(struct inode *, void *), int (*set)(struct inode *, void *), void *data)
{
	struct inode * inode;

	inode = alloc_inode(sb);
	if (inode) {
		struct inode * old;

		spin_lock(&inode_lock);
		/* We released the lock, so.. */
		old = find_inode(sb, head, test, data);
		if (!old) {
			if (set(inode, data))
				goto set_failed;

			inodes_stat.nr_inodes++;
			list_add(&inode->i_list, &inode_in_use);
			hlist_add_head(&inode->i_hash, head);
			inode->i_state = I_LOCK|I_NEW;
			spin_unlock(&inode_lock);

			/* Return the locked inode with I_NEW set, the
			 * caller is responsible for filling in the contents
			 */
			return inode;
		}

		/*
		 * Uhhuh, somebody else created the same inode under
		 * us. Use the old inode instead of the one we just
		 * allocated.
		 */
		__iget(old);
		spin_unlock(&inode_lock);
		destroy_inode(inode);
		inode = old;
		wait_on_inode(inode);
	}
	return inode;

set_failed:
	spin_unlock(&inode_lock);
	destroy_inode(inode);
	return NULL;
}

/*
 * get_new_inode_fast is the fast path version of get_new_inode, see the
 * comment at iget_locked for details.
 */
static struct inode * get_new_inode_fast(struct super_block *sb, struct hlist_head *head, unsigned long ino)
{
	struct inode * inode;

	inode = alloc_inode(sb);
	if (inode) {
		struct inode * old;

		spin_lock(&inode_lock);
		/* We released the lock, so.. */
		old = find_inode_fast(sb, head, ino);
		if (!old) {
			inode->i_ino = ino;
			inodes_stat.nr_inodes++;
			list_add(&inode->i_list, &inode_in_use);
			hlist_add_head(&inode->i_hash, head);
			inode->i_state = I_LOCK|I_NEW;
			spin_unlock(&inode_lock);

			/* Return the locked inode with I_NEW set, the
			 * caller is responsible for filling in the contents
			 */
			return inode;
		}

		/*
		 * Uhhuh, somebody else created the same inode under
		 * us. Use the old inode instead of the one we just
		 * allocated.
		 */
		__iget(old);
		spin_unlock(&inode_lock);
		destroy_inode(inode);
		inode = old;
		wait_on_inode(inode);
	}
	return inode;
}

static inline unsigned long hash(struct super_block *sb, unsigned long hashval)
{
	unsigned long tmp;

	tmp = (hashval * (unsigned long)sb) ^ (GOLDEN_RATIO_PRIME + hashval) /
			L1_CACHE_BYTES;
	tmp = tmp ^ ((tmp ^ GOLDEN_RATIO_PRIME) >> I_HASHBITS);
	return tmp & I_HASHMASK;
}

/**
 *	iunique - get a unique inode number
 *	@sb: superblock
 *	@max_reserved: highest reserved inode number
 *
 *	Obtain an inode number that is unique on the system for a given
 *	superblock. This is used by file systems that have no natural
 *	permanent inode numbering system. An inode number is returned that
 *	is higher than the reserved limit but unique.
 *
 *	BUGS:
 *	With a large number of inodes live on the file system this function
 *	currently becomes quite slow.
 */
ino_t iunique(struct super_block *sb, ino_t max_reserved)
{
	static ino_t counter;
	struct inode *inode;
	struct hlist_head * head;
	ino_t res;
	spin_lock(&inode_lock);
retry:
	if (counter > max_reserved) {
		head = inode_hashtable + hash(sb,counter);
		res = counter++;
		inode = find_inode_fast(sb, head, res);
		if (!inode) {
			spin_unlock(&inode_lock);
			return res;
		}
	} else {
		counter = max_reserved + 1;
	}
	goto retry;
	
}

EXPORT_SYMBOL(iunique);

struct inode *igrab(struct inode *inode)
{
	spin_lock(&inode_lock);
	if (!(inode->i_state & I_FREEING))
		__iget(inode);
	else
		/*
		 * Handle the case where s_op->clear_inode is not been
		 * called yet, and somebody is calling igrab
		 * while the inode is getting freed.
		 */
		inode = NULL;
	spin_unlock(&inode_lock);
	return inode;
}

EXPORT_SYMBOL(igrab);

/**
 * ifind - internal function, you want ilookup5() or iget5().
 * @sb:		super block of file system to search
 * @head:       the head of the list to search
 * @test:	callback used for comparisons between inodes
 * @data:	opaque data pointer to pass to @test
 *
 * ifind() searches for the inode specified by @data in the inode
 * cache. This is a generalized version of ifind_fast() for file systems where
 * the inode number is not sufficient for unique identification of an inode.
 *
 * If the inode is in the cache, the inode is returned with an incremented
 * reference count.
 *
 * Otherwise NULL is returned.
 *
 * Note, @test is called with the inode_lock held, so can't sleep.
 */
static inline struct inode *ifind(struct super_block *sb,
		struct hlist_head *head, int (*test)(struct inode *, void *),
		void *data)
{
	struct inode *inode;

	spin_lock(&inode_lock);
	inode = find_inode(sb, head, test, data);
	if (inode) {
		__iget(inode);
		spin_unlock(&inode_lock);
		wait_on_inode(inode);
		return inode;
	}
	spin_unlock(&inode_lock);
	return NULL;
}

/**
 * ifind_fast - internal function, you want ilookup() or iget().
 * @sb:		super block of file system to search
 * @head:       head of the list to search
 * @ino:	inode number to search for
 *
 * ifind_fast() searches for the inode @ino in the inode cache. This is for
 * file systems where the inode number is sufficient for unique identification
 * of an inode.
 *
 * If the inode is in the cache, the inode is returned with an incremented
 * reference count.
 *
 * Otherwise NULL is returned.
 */
static inline struct inode *ifind_fast(struct super_block *sb,
		struct hlist_head *head, unsigned long ino)
{
	struct inode *inode;

	spin_lock(&inode_lock);
	inode = find_inode_fast(sb, head, ino);
	if (inode) {
		__iget(inode);
		spin_unlock(&inode_lock);
		wait_on_inode(inode);
		return inode;
	}
	spin_unlock(&inode_lock);
	return NULL;
}

/**
 * ilookup5 - search for an inode in the inode cache
 * @sb:		super block of file system to search
 * @hashval:	hash value (usually inode number) to search for
 * @test:	callback used for comparisons between inodes
 * @data:	opaque data pointer to pass to @test
 *
 * ilookup5() uses ifind() to search for the inode specified by @hashval and
 * @data in the inode cache. This is a generalized version of ilookup() for
 * file systems where the inode number is not sufficient for unique
 * identification of an inode.
 *
 * If the inode is in the cache, the inode is returned with an incremented
 * reference count.
 *
 * Otherwise NULL is returned.
 *
 * Note, @test is called with the inode_lock held, so can't sleep.
 */
struct inode *ilookup5(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *), void *data)
{
	struct hlist_head *head = inode_hashtable + hash(sb, hashval);

	return ifind(sb, head, test, data);
}

EXPORT_SYMBOL(ilookup5);

/**
 * ilookup - search for an inode in the inode cache
 * @sb:		super block of file system to search
 * @ino:	inode number to search for
 *
 * ilookup() uses ifind_fast() to search for the inode @ino in the inode cache.
 * This is for file systems where the inode number is sufficient for unique
 * identification of an inode.
 *
 * If the inode is in the cache, the inode is returned with an incremented
 * reference count.
 *
 * Otherwise NULL is returned.
 */
struct inode *ilookup(struct super_block *sb, unsigned long ino)
{
	struct hlist_head *head = inode_hashtable + hash(sb, ino);

	return ifind_fast(sb, head, ino);
}

EXPORT_SYMBOL(ilookup);

/**
 * iget5_locked - obtain an inode from a mounted file system
 * @sb:		super block of file system
 * @hashval:	hash value (usually inode number) to get
 * @test:	callback used for comparisons between inodes
 * @set:	callback used to initialize a new struct inode
 * @data:	opaque data pointer to pass to @test and @set
 *
 * This is iget() without the read_inode() portion of get_new_inode().
 *
 * iget5_locked() uses ifind() to search for the inode specified by @hashval
 * and @data in the inode cache and if present it is returned with an increased
 * reference count. This is a generalized version of iget_locked() for file
 * systems where the inode number is not sufficient for unique identification
 * of an inode.
 *
 * If the inode is not in cache, get_new_inode() is called to allocate a new
 * inode and this is returned locked, hashed, and with the I_NEW flag set. The
 * file system gets to fill it in before unlocking it via unlock_new_inode().
 *
 * Note both @test and @set are called with the inode_lock held, so can't sleep.
 */
struct inode *iget5_locked(struct super_block *sb, unsigned long hashval,
		int (*test)(struct inode *, void *),
		int (*set)(struct inode *, void *), void *data)
{
	struct hlist_head *head = inode_hashtable + hash(sb, hashval);
	struct inode *inode;

	inode = ifind(sb, head, test, data);
	if (inode)
		return inode;
	/*
	 * get_new_inode() will do the right thing, re-trying the search
	 * in case it had to block at any point.
	 */
	return get_new_inode(sb, head, test, set, data);
}

EXPORT_SYMBOL(iget5_locked);

/**
 * iget_locked - obtain an inode from a mounted file system
 * @sb:		super block of file system
 * @ino:	inode number to get
 *
 * This is iget() without the read_inode() portion of get_new_inode_fast().
 *
 * iget_locked() uses ifind_fast() to search for the inode specified by @ino in
 * the inode cache and if present it is returned with an increased reference
 * count. This is for file systems where the inode number is sufficient for
 * unique identification of an inode.
 *
 * If the inode is not in cache, get_new_inode_fast() is called to allocate a
 * new inode and this is returned locked, hashed, and with the I_NEW flag set.
 * The file system gets to fill it in before unlocking it via
 * unlock_new_inode().
 */
struct inode *iget_locked(struct super_block *sb, unsigned long ino)
{
	struct hlist_head *head = inode_hashtable + hash(sb, ino);
	struct inode *inode;

	inode = ifind_fast(sb, head, ino);
	if (inode)
		return inode;
	/*
	 * get_new_inode_fast() will do the right thing, re-trying the search
	 * in case it had to block at any point.
	 */
	return get_new_inode_fast(sb, head, ino);
}

EXPORT_SYMBOL(iget_locked);

/**
 *	__insert_inode_hash - hash an inode
 *	@inode: unhashed inode
 *	@hashval: unsigned long value used to locate this object in the
 *		inode_hashtable.
 *
 *	Add an inode to the inode hash for this superblock.
 */
void __insert_inode_hash(struct inode *inode, unsigned long hashval)
{
	struct hlist_head *head = inode_hashtable + hash(inode->i_sb, hashval);
	spin_lock(&inode_lock);
	hlist_add_head(&inode->i_hash, head);
	spin_unlock(&inode_lock);
}

EXPORT_SYMBOL(__insert_inode_hash);

/**
 *	remove_inode_hash - remove an inode from the hash
 *	@inode: inode to unhash
 *
 *	Remove an inode from the superblock.
 */
void remove_inode_hash(struct inode *inode)
{
	spin_lock(&inode_lock);
	hlist_del_init(&inode->i_hash);
	spin_unlock(&inode_lock);
}

EXPORT_SYMBOL(remove_inode_hash);

/*
 * Tell the filesystem that this inode is no longer of any interest and should
 * be completely destroyed.
 *
 * We leave the inode in the inode hash table until *after* the filesystem's
 * ->delete_inode completes.  This ensures that an iget (such as nfsd might
 * instigate) will always find up-to-date information either in the hash or on
 * disk.
 *
 * I_FREEING is set so that no-one will take a new reference to the inode while
 * it is being deleted.
 */
void generic_delete_inode(struct inode *inode)
{
	struct super_operations *op = inode->i_sb->s_op;

	list_del_init(&inode->i_list);
	inode->i_state|=I_FREEING;
	inodes_stat.nr_inodes--;
	spin_unlock(&inode_lock);

	if (inode->i_data.nrpages)
		truncate_inode_pages(&inode->i_data, 0);

	security_inode_delete(inode);

	if (op->delete_inode) {
		void (*delete)(struct inode *) = op->delete_inode;
		if (!is_bad_inode(inode))
			DQUOT_INIT(inode);
		/* s_op->delete_inode internally recalls clear_inode() */
		delete(inode);
	} else
		clear_inode(inode);
	spin_lock(&inode_lock);
	hlist_del_init(&inode->i_hash);
	spin_unlock(&inode_lock);
	wake_up_inode(inode);
	if (inode->i_state != I_CLEAR)
		BUG();
	destroy_inode(inode);
}

EXPORT_SYMBOL(generic_delete_inode);

static void generic_forget_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;

	if (!hlist_unhashed(&inode->i_hash)) {
		if (!(inode->i_state & (I_DIRTY|I_LOCK)))
			list_move(&inode->i_list, &inode_unused);
		inodes_stat.nr_unused++;
		spin_unlock(&inode_lock);
		if (!sb || (sb->s_flags & MS_ACTIVE))
			return;
		write_inode_now(inode, 1);
		spin_lock(&inode_lock);
		inodes_stat.nr_unused--;
		hlist_del_init(&inode->i_hash);
	}
	list_del_init(&inode->i_list);
	inode->i_state|=I_FREEING;
	inodes_stat.nr_inodes--;
	spin_unlock(&inode_lock);
	if (inode->i_data.nrpages)
		truncate_inode_pages(&inode->i_data, 0);
	clear_inode(inode);
	destroy_inode(inode);
}

/*
 * Normal UNIX filesystem behaviour: delete the
 * inode when the usage count drops to zero, and
 * i_nlink is zero.
 */
static void generic_drop_inode(struct inode *inode)
{
	if (!inode->i_nlink)
		generic_delete_inode(inode);
	else
		generic_forget_inode(inode);
}

/*
 * Called when we're dropping the last reference
 * to an inode. 
 *
 * Call the FS "drop()" function, defaulting to
 * the legacy UNIX filesystem behaviour..
 *
 * NOTE! NOTE! NOTE! We're called with the inode lock
 * held, and the drop function is supposed to release
 * the lock!
 */
static inline void iput_final(struct inode *inode)
{
	struct super_operations *op = inode->i_sb->s_op;
	void (*drop)(struct inode *) = generic_drop_inode;

	if (op && op->drop_inode)
		drop = op->drop_inode;
	drop(inode);
}

/**
 *	iput	- put an inode 
 *	@inode: inode to put
 *
 *	Puts an inode, dropping its usage count. If the inode use count hits
 *	zero the inode is also then freed and may be destroyed.
 */
void iput(struct inode *inode)
{
	if (inode) {
		struct super_operations *op = inode->i_sb->s_op;

		if (inode->i_state == I_CLEAR)
			BUG();

		if (op && op->put_inode)
			op->put_inode(inode);

		if (atomic_dec_and_lock(&inode->i_count, &inode_lock))
			iput_final(inode);
	}
}

EXPORT_SYMBOL(iput);

/**
 *	bmap	- find a block number in a file
 *	@inode: inode of file
 *	@block: block to find
 *
 *	Returns the block number on the device holding the inode that
 *	is the disk block number for the block of the file requested.
 *	That is, asked for block 4 of inode 1 the function will return the
 *	disk block relative to the disk start that holds that block of the 
 *	file.
 */
sector_t bmap(struct inode * inode, sector_t block)
{
	sector_t res = 0;
	if (inode->i_mapping->a_ops->bmap)
		res = inode->i_mapping->a_ops->bmap(inode->i_mapping, block);
	return res;
}

EXPORT_SYMBOL(bmap);

/*
 * Return true if the filesystem which backs this inode considers the two
 * passed timespecs to be sufficiently different to warrant flushing the
 * altered time out to disk.
 */
static int inode_times_differ(struct inode *inode,
			struct timespec *old, struct timespec *new)
{
	if (IS_ONE_SECOND(inode))
		return old->tv_sec != new->tv_sec;
	return !timespec_equal(old, new);
}

/**
 *	update_atime	-	update the access time
 *	@inode: inode accessed
 *
 *	Update the accessed time on an inode and mark it for writeback.
 *	This function automatically handles read only file systems and media,
 *	as well as the "noatime" flag and inode specific "noatime" markers.
 */
void update_atime(struct inode *inode)
{
	struct timespec now;

	if (IS_NOATIME(inode))
		return;
	if (IS_NODIRATIME(inode) && S_ISDIR(inode->i_mode))
		return;
	if (IS_RDONLY(inode))
		return;

	now = current_kernel_time();
	if (inode_times_differ(inode, &inode->i_atime, &now)) {
		inode->i_atime = now;
		mark_inode_dirty_sync(inode);
	} else {
		if (!timespec_equal(&inode->i_atime, &now))
			inode->i_atime = now;
	}
}

EXPORT_SYMBOL(update_atime);

/**
 *	inode_update_time	-	update mtime and ctime time
 *	@inode: inode accessed
 *	@ctime_too: update ctime too
 *
 *	Update the mtime time on an inode and mark it for writeback.
 *	When ctime_too is specified update the ctime too.
 */

void inode_update_time(struct inode *inode, int ctime_too)
{
	struct timespec now;
	int sync_it = 0;

	if (IS_NOCMTIME(inode))
		return;
	if (IS_RDONLY(inode))
		return;

	now = current_kernel_time();

	if (inode_times_differ(inode, &inode->i_mtime, &now))
		sync_it = 1;
	inode->i_mtime = now;

	if (ctime_too) {
		if (inode_times_differ(inode, &inode->i_ctime, &now))
			sync_it = 1;
		inode->i_ctime = now;
	}
	if (sync_it)
		mark_inode_dirty_sync(inode);
}

EXPORT_SYMBOL(inode_update_time);

int inode_needs_sync(struct inode *inode)
{
	if (IS_SYNC(inode))
		return 1;
	if (S_ISDIR(inode->i_mode) && IS_DIRSYNC(inode))
		return 1;
	return 0;
}

EXPORT_SYMBOL(inode_needs_sync);

/*
 *	Quota functions that want to walk the inode lists..
 */
#ifdef CONFIG_QUOTA

/* Function back in dquot.c */
int remove_inode_dquot_ref(struct inode *, int, struct list_head *);

void remove_dquot_ref(struct super_block *sb, int type, struct list_head *tofree_head)
{
	struct inode *inode;
	struct list_head *act_head;

	if (!sb->dq_op)
		return;	/* nothing to do */
	spin_lock(&inode_lock);	/* This lock is for inodes code */

	/* We hold dqptr_sem so we are safe against the quota code */
	list_for_each(act_head, &inode_in_use) {
		inode = list_entry(act_head, struct inode, i_list);
		if (inode->i_sb == sb && !IS_NOQUOTA(inode))
			remove_inode_dquot_ref(inode, type, tofree_head);
	}
	list_for_each(act_head, &inode_unused) {
		inode = list_entry(act_head, struct inode, i_list);
		if (inode->i_sb == sb && !IS_NOQUOTA(inode))
			remove_inode_dquot_ref(inode, type, tofree_head);
	}
	list_for_each(act_head, &sb->s_dirty) {
		inode = list_entry(act_head, struct inode, i_list);
		if (!IS_NOQUOTA(inode))
			remove_inode_dquot_ref(inode, type, tofree_head);
	}
	list_for_each(act_head, &sb->s_io) {
		inode = list_entry(act_head, struct inode, i_list);
		if (!IS_NOQUOTA(inode))
			remove_inode_dquot_ref(inode, type, tofree_head);
	}
	spin_unlock(&inode_lock);
}

#endif

/*
 * Hashed waitqueues for wait_on_inode().  The table is pretty small - the
 * kernel doesn't lock many inodes at the same time.
 */
#define I_WAIT_TABLE_ORDER	3
static struct i_wait_queue_head {
	wait_queue_head_t wqh;
} ____cacheline_aligned_in_smp i_wait_queue_heads[1<<I_WAIT_TABLE_ORDER];

/*
 * Return the address of the waitqueue_head to be used for this inode
 */
static wait_queue_head_t *i_waitq_head(struct inode *inode)
{
	return &i_wait_queue_heads[hash_ptr(inode, I_WAIT_TABLE_ORDER)].wqh;
}

void __wait_on_inode(struct inode *inode)
{
	DECLARE_WAITQUEUE(wait, current);
	wait_queue_head_t *wq = i_waitq_head(inode);

	add_wait_queue(wq, &wait);
repeat:
	set_current_state(TASK_UNINTERRUPTIBLE);
	if (inode->i_state & I_LOCK) {
		schedule();
		goto repeat;
	}
	remove_wait_queue(wq, &wait);
	__set_current_state(TASK_RUNNING);
}

/*
 * If we try to find an inode in the inode hash while it is being deleted, we
 * have to wait until the filesystem completes its deletion before reporting
 * that it isn't found.  This is because iget will immediately call
 * ->read_inode, and we want to be sure that evidence of the deletion is found
 * by ->read_inode.
 *
 * This call might return early if an inode which shares the waitq is woken up.
 * This is most easily handled by the caller which will loop around again
 * looking for the inode.
 *
 * This is called with inode_lock held.
 */
static void __wait_on_freeing_inode(struct inode *inode)
{
	DECLARE_WAITQUEUE(wait, current);
	wait_queue_head_t *wq = i_waitq_head(inode);

	add_wait_queue(wq, &wait);
	set_current_state(TASK_UNINTERRUPTIBLE);
	spin_unlock(&inode_lock);
	schedule();
	remove_wait_queue(wq, &wait);
	spin_lock(&inode_lock);
}

void wake_up_inode(struct inode *inode)
{
	wait_queue_head_t *wq = i_waitq_head(inode);

	/*
	 * Prevent speculative execution through spin_unlock(&inode_lock);
	 */
	smp_mb();
	if (waitqueue_active(wq))
		wake_up_all(wq);
}

static __initdata unsigned long ihash_entries;
static int __init set_ihash_entries(char *str)
{
	if (!str)
		return 0;
	ihash_entries = simple_strtoul(str, &str, 0);
	return 1;
}
__setup("ihash_entries=", set_ihash_entries);

/*
 * Initialize the waitqueues and inode hash table.
 */
void __init inode_init_early(void)
{
	int loop;

	inode_hashtable =
		alloc_large_system_hash("Inode-cache",
					sizeof(struct hlist_head),
					ihash_entries,
					14,
					0,
					&i_hash_shift,
					&i_hash_mask);

	for (loop = 0; loop < (1 << i_hash_shift); loop++)
		INIT_HLIST_HEAD(&inode_hashtable[loop]);
}

void __init inode_init(unsigned long mempages)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(i_wait_queue_heads); i++)
		init_waitqueue_head(&i_wait_queue_heads[i].wqh);

	/* inode slab cache */
	inode_cachep = kmem_cache_create("inode_cache", sizeof(struct inode),
				0, SLAB_HWCACHE_ALIGN|SLAB_PANIC, init_once,
				NULL);
	set_shrinker(DEFAULT_SEEKS, shrink_icache_memory);
}

void init_special_inode(struct inode *inode, umode_t mode, dev_t rdev)
{
	inode->i_mode = mode;
	if (S_ISCHR(mode)) {
		inode->i_fop = &def_chr_fops;
		inode->i_rdev = rdev;
	} else if (S_ISBLK(mode)) {
		inode->i_fop = &def_blk_fops;
		inode->i_rdev = rdev;
	} else if (S_ISFIFO(mode))
		inode->i_fop = &def_fifo_fops;
	else if (S_ISSOCK(mode))
		inode->i_fop = &bad_sock_fops;
	else
		printk(KERN_DEBUG "init_special_inode: bogus i_mode (%o)\n",
		       mode);
}
EXPORT_SYMBOL(init_special_inode);
