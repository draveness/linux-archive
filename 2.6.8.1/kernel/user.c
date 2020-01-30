/*
 * The "user cache".
 *
 * (C) Copyright 1991-2000 Linus Torvalds
 *
 * We have a per-user structure to keep track of how many
 * processes, files etc the user has claimed, in order to be
 * able to have per-user limits for system resources. 
 */

#include <linux/init.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/bitops.h>

/*
 * UID task count cache, to get fast user lookup in "alloc_uid"
 * when changing user ID's (ie setuid() and friends).
 */
#define UIDHASH_BITS		8
#define UIDHASH_SZ		(1 << UIDHASH_BITS)
#define UIDHASH_MASK		(UIDHASH_SZ - 1)
#define __uidhashfn(uid)	(((uid >> UIDHASH_BITS) + uid) & UIDHASH_MASK)
#define uidhashentry(uid)	(uidhash_table + __uidhashfn((uid)))

static kmem_cache_t *uid_cachep;
static struct list_head uidhash_table[UIDHASH_SZ];
static spinlock_t uidhash_lock = SPIN_LOCK_UNLOCKED;

struct user_struct root_user = {
	.__count	= ATOMIC_INIT(1),
	.processes	= ATOMIC_INIT(1),
	.files		= ATOMIC_INIT(0),
	.sigpending	= ATOMIC_INIT(0),
	.mq_bytes	= 0
};

/*
 * These routines must be called with the uidhash spinlock held!
 */
static inline void uid_hash_insert(struct user_struct *up, struct list_head *hashent)
{
	list_add(&up->uidhash_list, hashent);
}

static inline void uid_hash_remove(struct user_struct *up)
{
	list_del(&up->uidhash_list);
}

static inline struct user_struct *uid_hash_find(uid_t uid, struct list_head *hashent)
{
	struct list_head *up;

	list_for_each(up, hashent) {
		struct user_struct *user;

		user = list_entry(up, struct user_struct, uidhash_list);

		if(user->uid == uid) {
			atomic_inc(&user->__count);
			return user;
		}
	}

	return NULL;
}

/*
 * Locate the user_struct for the passed UID.  If found, take a ref on it.  The
 * caller must undo that ref with free_uid().
 *
 * If the user_struct could not be found, return NULL.
 */
struct user_struct *find_user(uid_t uid)
{
	struct user_struct *ret;

	spin_lock(&uidhash_lock);
	ret = uid_hash_find(uid, uidhashentry(uid));
	spin_unlock(&uidhash_lock);
	return ret;
}

void free_uid(struct user_struct *up)
{
	if (up && atomic_dec_and_lock(&up->__count, &uidhash_lock)) {
		uid_hash_remove(up);
		kmem_cache_free(uid_cachep, up);
		spin_unlock(&uidhash_lock);
	}
}

struct user_struct * alloc_uid(uid_t uid)
{
	struct list_head *hashent = uidhashentry(uid);
	struct user_struct *up;

	spin_lock(&uidhash_lock);
	up = uid_hash_find(uid, hashent);
	spin_unlock(&uidhash_lock);

	if (!up) {
		struct user_struct *new;

		new = kmem_cache_alloc(uid_cachep, SLAB_KERNEL);
		if (!new)
			return NULL;
		new->uid = uid;
		atomic_set(&new->__count, 1);
		atomic_set(&new->processes, 0);
		atomic_set(&new->files, 0);
		atomic_set(&new->sigpending, 0);

		new->mq_bytes = 0;

		/*
		 * Before adding this, check whether we raced
		 * on adding the same user already..
		 */
		spin_lock(&uidhash_lock);
		up = uid_hash_find(uid, hashent);
		if (up) {
			kmem_cache_free(uid_cachep, new);
		} else {
			uid_hash_insert(new, hashent);
			up = new;
		}
		spin_unlock(&uidhash_lock);

	}
	return up;
}

void switch_uid(struct user_struct *new_user)
{
	struct user_struct *old_user;

	/* What if a process setreuid()'s and this brings the
	 * new uid over his NPROC rlimit?  We can check this now
	 * cheaply with the new uid cache, so if it matters
	 * we should be checking for it.  -DaveM
	 */
	old_user = current->user;
	atomic_inc(&new_user->processes);
	atomic_dec(&old_user->processes);
	current->user = new_user;
	free_uid(old_user);
}


static int __init uid_cache_init(void)
{
	int n;

	uid_cachep = kmem_cache_create("uid_cache", sizeof(struct user_struct),
			0, SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL, NULL);

	for(n = 0; n < UIDHASH_SZ; ++n)
		INIT_LIST_HEAD(uidhash_table + n);

	/* Insert the root user immediately (init already runs as root) */
	spin_lock(&uidhash_lock);
	uid_hash_insert(&root_user, uidhashentry(0));
	spin_unlock(&uidhash_lock);

	return 0;
}

module_init(uid_cache_init);
