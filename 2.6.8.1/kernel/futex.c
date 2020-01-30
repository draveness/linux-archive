/*
 *  Fast Userspace Mutexes (which I call "Futexes!").
 *  (C) Rusty Russell, IBM 2002
 *
 *  Generalized futexes, futex requeueing, misc fixes by Ingo Molnar
 *  (C) Copyright 2003 Red Hat Inc, All Rights Reserved
 *
 *  Removed page pinning, fix privately mapped COW pages and other cleanups
 *  (C) Copyright 2003 Jamie Lokier
 *
 *  Thanks to Ben LaHaise for yelling "hashed waitqueues" loudly
 *  enough at me, Linus for the original (flawed) idea, Matthew
 *  Kirkwood for proof-of-concept implementation.
 *
 *  "The futexes are also cursed."
 *  "But they come in a choice of three flavours!"
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <linux/slab.h>
#include <linux/poll.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/jhash.h>
#include <linux/init.h>
#include <linux/futex.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/syscalls.h>

#define FUTEX_HASHBITS 8

/*
 * Futexes are matched on equal values of this key.
 * The key type depends on whether it's a shared or private mapping.
 * Don't rearrange members without looking at hash_futex().
 *
 * offset is aligned to a multiple of sizeof(u32) (== 4) by definition.
 * We set bit 0 to indicate if it's an inode-based key.
 */
union futex_key {
	struct {
		unsigned long pgoff;
		struct inode *inode;
		int offset;
	} shared;
	struct {
		unsigned long uaddr;
		struct mm_struct *mm;
		int offset;
	} private;
	struct {
		unsigned long word;
		void *ptr;
		int offset;
	} both;
};

/*
 * We use this hashed waitqueue instead of a normal wait_queue_t, so
 * we can wake only the relevant ones (hashed queues may be shared).
 *
 * A futex_q has a woken state, just like tasks have TASK_RUNNING.
 * It is considered woken when list_empty(&q->list) || q->lock_ptr == 0.
 * The order of wakup is always to make the first condition true, then
 * wake up q->waiters, then make the second condition true.
 */
struct futex_q {
	struct list_head list;
	wait_queue_head_t waiters;

	/* Which hash list lock to use. */
	spinlock_t *lock_ptr;

	/* Key which the futex is hashed on. */
	union futex_key key;

	/* For fd, sigio sent using these. */
	int fd;
	struct file *filp;
};

/*
 * Split the global futex_lock into every hash list lock.
 */
struct futex_hash_bucket {
       spinlock_t              lock;
       unsigned int	    nqueued;
       struct list_head       chain;
};

static struct futex_hash_bucket futex_queues[1<<FUTEX_HASHBITS];

/* Futex-fs vfsmount entry: */
static struct vfsmount *futex_mnt;

/*
 * We hash on the keys returned from get_futex_key (see below).
 */
static struct futex_hash_bucket *hash_futex(union futex_key *key)
{
	u32 hash = jhash2((u32*)&key->both.word,
			  (sizeof(key->both.word)+sizeof(key->both.ptr))/4,
			  key->both.offset);
	return &futex_queues[hash & ((1 << FUTEX_HASHBITS)-1)];
}

/*
 * Return 1 if two futex_keys are equal, 0 otherwise.
 */
static inline int match_futex(union futex_key *key1, union futex_key *key2)
{
	return (key1->both.word == key2->both.word
		&& key1->both.ptr == key2->both.ptr
		&& key1->both.offset == key2->both.offset);
}

/*
 * Get parameters which are the keys for a futex.
 *
 * For shared mappings, it's (page->index, vma->vm_file->f_dentry->d_inode,
 * offset_within_page).  For private mappings, it's (uaddr, current->mm).
 * We can usually work out the index without swapping in the page.
 *
 * Returns: 0, or negative error code.
 * The key words are stored in *key on success.
 *
 * Should be called with &current->mm->mmap_sem but NOT any spinlocks.
 */
static int get_futex_key(unsigned long uaddr, union futex_key *key)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	struct page *page;
	int err;

	/*
	 * The futex address must be "naturally" aligned.
	 */
	key->both.offset = uaddr % PAGE_SIZE;
	if (unlikely((key->both.offset % sizeof(u32)) != 0))
		return -EINVAL;
	uaddr -= key->both.offset;

	/*
	 * The futex is hashed differently depending on whether
	 * it's in a shared or private mapping.  So check vma first.
	 */
	vma = find_extend_vma(mm, uaddr);
	if (unlikely(!vma))
		return -EFAULT;

	/*
	 * Permissions.
	 */
	if (unlikely((vma->vm_flags & (VM_IO|VM_READ)) != VM_READ))
		return (vma->vm_flags & VM_IO) ? -EPERM : -EACCES;

	/*
	 * Private mappings are handled in a simple way.
	 *
	 * NOTE: When userspace waits on a MAP_SHARED mapping, even if
	 * it's a read-only handle, it's expected that futexes attach to
	 * the object not the particular process.  Therefore we use
	 * VM_MAYSHARE here, not VM_SHARED which is restricted to shared
	 * mappings of _writable_ handles.
	 */
	if (likely(!(vma->vm_flags & VM_MAYSHARE))) {
		key->private.mm = mm;
		key->private.uaddr = uaddr;
		return 0;
	}

	/*
	 * Linear file mappings are also simple.
	 */
	key->shared.inode = vma->vm_file->f_dentry->d_inode;
	key->both.offset++; /* Bit 0 of offset indicates inode-based key. */
	if (likely(!(vma->vm_flags & VM_NONLINEAR))) {
		key->shared.pgoff = (((uaddr - vma->vm_start) >> PAGE_SHIFT)
				     + vma->vm_pgoff);
		return 0;
	}

	/*
	 * We could walk the page table to read the non-linear
	 * pte, and get the page index without fetching the page
	 * from swap.  But that's a lot of code to duplicate here
	 * for a rare case, so we simply fetch the page.
	 */

	/*
	 * Do a quick atomic lookup first - this is the fastpath.
	 */
	spin_lock(&current->mm->page_table_lock);
	page = follow_page(mm, uaddr, 0);
	if (likely(page != NULL)) {
		key->shared.pgoff =
			page->index << (PAGE_CACHE_SHIFT - PAGE_SHIFT);
		spin_unlock(&current->mm->page_table_lock);
		return 0;
	}
	spin_unlock(&current->mm->page_table_lock);

	/*
	 * Do it the general way.
	 */
	err = get_user_pages(current, mm, uaddr, 1, 0, 0, &page, NULL);
	if (err >= 0) {
		key->shared.pgoff =
			page->index << (PAGE_CACHE_SHIFT - PAGE_SHIFT);
		put_page(page);
		return 0;
	}
	return err;
}

/*
 * Take a reference to the resource addressed by a key.
 * Can be called while holding spinlocks.
 *
 * NOTE: mmap_sem MUST be held between get_futex_key() and calling this
 * function, if it is called at all.  mmap_sem keeps key->shared.inode valid.
 */
static inline void get_key_refs(union futex_key *key)
{
	if (key->both.ptr != 0) {
		if (key->both.offset & 1)
			atomic_inc(&key->shared.inode->i_count);
		else
			atomic_inc(&key->private.mm->mm_count);
	}
}

/*
 * Drop a reference to the resource addressed by a key.
 * The hash bucket spinlock must not be held.
 */
static void drop_key_refs(union futex_key *key)
{
	if (key->both.ptr != 0) {
		if (key->both.offset & 1)
			iput(key->shared.inode);
		else
			mmdrop(key->private.mm);
	}
}

/*
 * The hash bucket lock must be held when this is called.
 * Afterwards, the futex_q must not be accessed.
 */
static void wake_futex(struct futex_q *q)
{
	list_del_init(&q->list);
	if (q->filp)
		send_sigio(&q->filp->f_owner, q->fd, POLL_IN);
	/*
	 * The lock in wake_up_all() is a crucial memory barrier after the
	 * list_del_init() and also before assigning to q->lock_ptr.
	 */
	wake_up_all(&q->waiters);
	/*
	 * The waiting task can free the futex_q as soon as this is written,
	 * without taking any locks.  This must come last.
	 */
	q->lock_ptr = NULL;
}

/*
 * Wake up all waiters hashed on the physical page that is mapped
 * to this virtual address:
 */
static int futex_wake(unsigned long uaddr, int nr_wake)
{
	union futex_key key;
	struct futex_hash_bucket *bh;
	struct list_head *head;
	struct futex_q *this, *next;
	int ret;

	down_read(&current->mm->mmap_sem);

	ret = get_futex_key(uaddr, &key);
	if (unlikely(ret != 0))
		goto out;

	bh = hash_futex(&key);
	spin_lock(&bh->lock);
	head = &bh->chain;

	list_for_each_entry_safe(this, next, head, list) {
		if (match_futex (&this->key, &key)) {
			wake_futex(this);
			if (++ret >= nr_wake)
				break;
		}
	}

	spin_unlock(&bh->lock);
out:
	up_read(&current->mm->mmap_sem);
	return ret;
}

/*
 * Requeue all waiters hashed on one physical page to another
 * physical page.
 */
static int futex_requeue(unsigned long uaddr1, unsigned long uaddr2,
			 int nr_wake, int nr_requeue, int *valp)
{
	union futex_key key1, key2;
	struct futex_hash_bucket *bh1, *bh2;
	struct list_head *head1;
	struct futex_q *this, *next;
	int ret, drop_count = 0;
	unsigned int nqueued;

	down_read(&current->mm->mmap_sem);

	ret = get_futex_key(uaddr1, &key1);
	if (unlikely(ret != 0))
		goto out;
	ret = get_futex_key(uaddr2, &key2);
	if (unlikely(ret != 0))
		goto out;

	bh1 = hash_futex(&key1);
	bh2 = hash_futex(&key2);

	nqueued = bh1->nqueued;
	if (likely(valp != NULL)) {
		int curval;

		/* In order to avoid doing get_user while
		   holding bh1->lock and bh2->lock, nqueued
		   (monotonically increasing field) must be first
		   read, then *uaddr1 fetched from userland and
		   after acquiring lock nqueued field compared with
		   the stored value.  The smp_mb () below
		   makes sure that bh1->nqueued is read from memory
		   before *uaddr1.  */
		smp_mb();

		if (get_user(curval, (int __user *)uaddr1) != 0) {
			ret = -EFAULT;
			goto out;
		}
		if (curval != *valp) {
			ret = -EAGAIN;
			goto out;
		}
	}

	if (bh1 < bh2)
		spin_lock(&bh1->lock);
	spin_lock(&bh2->lock);
	if (bh1 > bh2)
		spin_lock(&bh1->lock);

	if (unlikely(nqueued != bh1->nqueued && valp != NULL)) {
		ret = -EAGAIN;
		goto out_unlock;
	}

	head1 = &bh1->chain;
	list_for_each_entry_safe(this, next, head1, list) {
		if (!match_futex (&this->key, &key1))
			continue;
		if (++ret <= nr_wake) {
			wake_futex(this);
		} else {
			list_move_tail(&this->list, &bh2->chain);
			this->lock_ptr = &bh2->lock;
			this->key = key2;
			get_key_refs(&key2);
			drop_count++;

			if (ret - nr_wake >= nr_requeue)
				break;
			/* Make sure to stop if key1 == key2 */
			if (head1 == &bh2->chain && head1 != &next->list)
				head1 = &this->list;
		}
	}

out_unlock:
	spin_unlock(&bh1->lock);
	if (bh1 != bh2)
		spin_unlock(&bh2->lock);

	/* drop_key_refs() must be called outside the spinlocks. */
	while (--drop_count >= 0)
		drop_key_refs(&key1);

out:
	up_read(&current->mm->mmap_sem);
	return ret;
}

/*
 * queue_me and unqueue_me must be called as a pair, each
 * exactly once.  They are called with the hashed spinlock held.
 */

/* The key must be already stored in q->key. */
static void queue_me(struct futex_q *q, int fd, struct file *filp)
{
	struct futex_hash_bucket *bh;

	q->fd = fd;
	q->filp = filp;

	init_waitqueue_head(&q->waiters);

	get_key_refs(&q->key);
	bh = hash_futex(&q->key);
	q->lock_ptr = &bh->lock;

	spin_lock(&bh->lock);
	bh->nqueued++;
	list_add_tail(&q->list, &bh->chain);
	spin_unlock(&bh->lock);
}

/* Return 1 if we were still queued (ie. 0 means we were woken) */
static int unqueue_me(struct futex_q *q)
{
	int ret = 0;
	spinlock_t *lock_ptr;

	/* In the common case we don't take the spinlock, which is nice. */
 retry:
	lock_ptr = q->lock_ptr;
	if (lock_ptr != 0) {
		spin_lock(lock_ptr);
		/*
		 * q->lock_ptr can change between reading it and
		 * spin_lock(), causing us to take the wrong lock.  This
		 * corrects the race condition.
		 *
		 * Reasoning goes like this: if we have the wrong lock,
		 * q->lock_ptr must have changed (maybe several times)
		 * between reading it and the spin_lock().  It can
		 * change again after the spin_lock() but only if it was
		 * already changed before the spin_lock().  It cannot,
		 * however, change back to the original value.  Therefore
		 * we can detect whether we acquired the correct lock.
		 */
		if (unlikely(lock_ptr != q->lock_ptr)) {
			spin_unlock(lock_ptr);
			goto retry;
		}
		WARN_ON(list_empty(&q->list));
		list_del(&q->list);
		spin_unlock(lock_ptr);
		ret = 1;
	}

	drop_key_refs(&q->key);
	return ret;
}

static int futex_wait(unsigned long uaddr, int val, unsigned long time)
{
	DECLARE_WAITQUEUE(wait, current);
	int ret, curval;
	struct futex_q q;

	down_read(&current->mm->mmap_sem);

	ret = get_futex_key(uaddr, &q.key);
	if (unlikely(ret != 0))
		goto out_release_sem;

	queue_me(&q, -1, NULL);

	/*
	 * Access the page after the futex is queued.
	 * We hold the mmap semaphore, so the mapping cannot have changed
	 * since we looked it up.
	 */
	if (get_user(curval, (int __user *)uaddr) != 0) {
		ret = -EFAULT;
		goto out_unqueue;
	}
	if (curval != val) {
		ret = -EWOULDBLOCK;
		goto out_unqueue;
	}

	/*
	 * Now the futex is queued and we have checked the data, we
	 * don't want to hold mmap_sem while we sleep.
	 */	
	up_read(&current->mm->mmap_sem);

	/*
	 * There might have been scheduling since the queue_me(), as we
	 * cannot hold a spinlock across the get_user() in case it
	 * faults, and we cannot just set TASK_INTERRUPTIBLE state when
	 * queueing ourselves into the futex hash.  This code thus has to
	 * rely on the futex_wake() code removing us from hash when it
	 * wakes us up.
	 */

	/* add_wait_queue is the barrier after __set_current_state. */
	__set_current_state(TASK_INTERRUPTIBLE);
	add_wait_queue(&q.waiters, &wait);
	/*
	 * !list_empty() is safe here without any lock.
	 * q.lock_ptr != 0 is not safe, because of ordering against wakeup.
	 */
	if (likely(!list_empty(&q.list)))
		time = schedule_timeout(time);
	__set_current_state(TASK_RUNNING);

	/*
	 * NOTE: we don't remove ourselves from the waitqueue because
	 * we are the only user of it.
	 */

	/* If we were woken (and unqueued), we succeeded, whatever. */
	if (!unqueue_me(&q))
		return 0;
	if (time == 0)
		return -ETIMEDOUT;
	/* A spurious wakeup should never happen. */
	WARN_ON(!signal_pending(current));
	return -EINTR;

 out_unqueue:
	/* If we were woken (and unqueued), we succeeded, whatever. */
	if (!unqueue_me(&q))
		ret = 0;
 out_release_sem:
	up_read(&current->mm->mmap_sem);
	return ret;
}

static int futex_close(struct inode *inode, struct file *filp)
{
	struct futex_q *q = filp->private_data;

	unqueue_me(q);
	kfree(q);
	return 0;
}

/* This is one-shot: once it's gone off you need a new fd */
static unsigned int futex_poll(struct file *filp,
			       struct poll_table_struct *wait)
{
	struct futex_q *q = filp->private_data;
	int ret = 0;

	poll_wait(filp, &q->waiters, wait);

	/*
	 * list_empty() is safe here without any lock.
	 * q->lock_ptr != 0 is not safe, because of ordering against wakeup.
	 */
	if (list_empty(&q->list))
		ret = POLLIN | POLLRDNORM;

	return ret;
}

static struct file_operations futex_fops = {
	.release	= futex_close,
	.poll		= futex_poll,
};

/*
 * Signal allows caller to avoid the race which would occur if they
 * set the sigio stuff up afterwards.
 */
static int futex_fd(unsigned long uaddr, int signal)
{
	struct futex_q *q;
	struct file *filp;
	int ret, err;

	ret = -EINVAL;
	if (signal < 0 || signal > _NSIG)
		goto out;

	ret = get_unused_fd();
	if (ret < 0)
		goto out;
	filp = get_empty_filp();
	if (!filp) {
		put_unused_fd(ret);
		ret = -ENFILE;
		goto out;
	}
	filp->f_op = &futex_fops;
	filp->f_vfsmnt = mntget(futex_mnt);
	filp->f_dentry = dget(futex_mnt->mnt_root);
	filp->f_mapping = filp->f_dentry->d_inode->i_mapping;

	if (signal) {
		int err;
		err = f_setown(filp, current->pid, 1);
		if (err < 0) {
			put_unused_fd(ret);
			put_filp(filp);
			ret = err;
			goto out;
		}
		filp->f_owner.signum = signal;
	}

	q = kmalloc(sizeof(*q), GFP_KERNEL);
	if (!q) {
		put_unused_fd(ret);
		put_filp(filp);
		ret = -ENOMEM;
		goto out;
	}

	down_read(&current->mm->mmap_sem);
	err = get_futex_key(uaddr, &q->key);

	if (unlikely(err != 0)) {
		up_read(&current->mm->mmap_sem);
		put_unused_fd(ret);
		put_filp(filp);
		kfree(q);
		return err;
	}

	/*
	 * queue_me() must be called before releasing mmap_sem, because
	 * key->shared.inode needs to be referenced while holding it.
	 */
	filp->private_data = q;

	queue_me(q, ret, filp);
	up_read(&current->mm->mmap_sem);

	/* Now we map fd to filp, so userspace can access it */
	fd_install(ret, filp);
out:
	return ret;
}

long do_futex(unsigned long uaddr, int op, int val, unsigned long timeout,
		unsigned long uaddr2, int val2, int val3)
{
	int ret;

	switch (op) {
	case FUTEX_WAIT:
		ret = futex_wait(uaddr, val, timeout);
		break;
	case FUTEX_WAKE:
		ret = futex_wake(uaddr, val);
		break;
	case FUTEX_FD:
		/* non-zero val means F_SETOWN(getpid()) & F_SETSIG(val) */
		ret = futex_fd(uaddr, val);
		break;
	case FUTEX_REQUEUE:
		ret = futex_requeue(uaddr, uaddr2, val, val2, NULL);
		break;
	case FUTEX_CMP_REQUEUE:
		ret = futex_requeue(uaddr, uaddr2, val, val2, &val3);
		break;
	default:
		ret = -ENOSYS;
	}
	return ret;
}


asmlinkage long sys_futex(u32 __user *uaddr, int op, int val,
			  struct timespec __user *utime, u32 __user *uaddr2,
			  int val3)
{
	struct timespec t;
	unsigned long timeout = MAX_SCHEDULE_TIMEOUT;
	int val2 = 0;

	if ((op == FUTEX_WAIT) && utime) {
		if (copy_from_user(&t, utime, sizeof(t)) != 0)
			return -EFAULT;
		timeout = timespec_to_jiffies(&t) + 1;
	}
	/*
	 * requeue parameter in 'utime' if op == FUTEX_REQUEUE.
	 */
	if (op >= FUTEX_REQUEUE)
		val2 = (int) (unsigned long) utime;

	return do_futex((unsigned long)uaddr, op, val, timeout,
			(unsigned long)uaddr2, val2, val3);
}

static struct super_block *
futexfs_get_sb(struct file_system_type *fs_type,
	       int flags, const char *dev_name, void *data)
{
	return get_sb_pseudo(fs_type, "futex", NULL, 0xBAD1DEA);
}

static struct file_system_type futex_fs_type = {
	.name		= "futexfs",
	.get_sb		= futexfs_get_sb,
	.kill_sb	= kill_anon_super,
};

static int __init init(void)
{
	unsigned int i;

	register_filesystem(&futex_fs_type);
	futex_mnt = kern_mount(&futex_fs_type);

	for (i = 0; i < ARRAY_SIZE(futex_queues); i++) {
		INIT_LIST_HEAD(&futex_queues[i].chain);
		futex_queues[i].lock = SPIN_LOCK_UNLOCKED;
	}
	return 0;
}
__initcall(init);
