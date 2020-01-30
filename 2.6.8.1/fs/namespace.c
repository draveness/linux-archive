/*
 *  linux/fs/namespace.c
 *
 * (C) Copyright Al Viro 2000, 2001
 *	Released under GPL v2.
 *
 * Based on code from fs/super.c, copyright Linus Torvalds and others.
 * Heavily rewritten.
 */

#include <linux/config.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/quotaops.h>
#include <linux/acct.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/namespace.h>
#include <linux/namei.h>
#include <linux/security.h>
#include <linux/mount.h>
#include <asm/uaccess.h>
#include <asm/unistd.h>

extern int __init init_rootfs(void);

#ifdef CONFIG_SYSFS
extern int __init sysfs_init(void);
#else
static inline int sysfs_init(void)
{
	return 0;
}
#endif

/* spinlock for vfsmount related operations, inplace of dcache_lock */
spinlock_t vfsmount_lock __cacheline_aligned_in_smp = SPIN_LOCK_UNLOCKED;

static struct list_head *mount_hashtable;
static int hash_mask, hash_bits;
static kmem_cache_t *mnt_cache; 

static inline unsigned long hash(struct vfsmount *mnt, struct dentry *dentry)
{
	unsigned long tmp = ((unsigned long) mnt / L1_CACHE_BYTES);
	tmp += ((unsigned long) dentry / L1_CACHE_BYTES);
	tmp = tmp + (tmp >> hash_bits);
	return tmp & hash_mask;
}

struct vfsmount *alloc_vfsmnt(const char *name)
{
	struct vfsmount *mnt = kmem_cache_alloc(mnt_cache, GFP_KERNEL); 
	if (mnt) {
		memset(mnt, 0, sizeof(struct vfsmount));
		atomic_set(&mnt->mnt_count,1);
		INIT_LIST_HEAD(&mnt->mnt_hash);
		INIT_LIST_HEAD(&mnt->mnt_child);
		INIT_LIST_HEAD(&mnt->mnt_mounts);
		INIT_LIST_HEAD(&mnt->mnt_list);
		INIT_LIST_HEAD(&mnt->mnt_fslink);
		if (name) {
			int size = strlen(name)+1;
			char *newname = kmalloc(size, GFP_KERNEL);
			if (newname) {
				memcpy(newname, name, size);
				mnt->mnt_devname = newname;
			}
		}
	}
	return mnt;
}

void free_vfsmnt(struct vfsmount *mnt)
{
	kfree(mnt->mnt_devname);
	kmem_cache_free(mnt_cache, mnt);
}

/*
 * Now, lookup_mnt increments the ref count before returning
 * the vfsmount struct.
 */
struct vfsmount *lookup_mnt(struct vfsmount *mnt, struct dentry *dentry)
{
	struct list_head * head = mount_hashtable + hash(mnt, dentry);
	struct list_head * tmp = head;
	struct vfsmount *p, *found = NULL;

	spin_lock(&vfsmount_lock);
	for (;;) {
		tmp = tmp->next;
		p = NULL;
		if (tmp == head)
			break;
		p = list_entry(tmp, struct vfsmount, mnt_hash);
		if (p->mnt_parent == mnt && p->mnt_mountpoint == dentry) {
			found = mntget(p);
			break;
		}
	}
	spin_unlock(&vfsmount_lock);
	return found;
}

EXPORT_SYMBOL(lookup_mnt);

static inline int check_mnt(struct vfsmount *mnt)
{
	return mnt->mnt_namespace == current->namespace;
}

static void detach_mnt(struct vfsmount *mnt, struct nameidata *old_nd)
{
	old_nd->dentry = mnt->mnt_mountpoint;
	old_nd->mnt = mnt->mnt_parent;
	mnt->mnt_parent = mnt;
	mnt->mnt_mountpoint = mnt->mnt_root;
	list_del_init(&mnt->mnt_child);
	list_del_init(&mnt->mnt_hash);
	old_nd->dentry->d_mounted--;
}

static void attach_mnt(struct vfsmount *mnt, struct nameidata *nd)
{
	mnt->mnt_parent = mntget(nd->mnt);
	mnt->mnt_mountpoint = dget(nd->dentry);
	list_add(&mnt->mnt_hash, mount_hashtable+hash(nd->mnt, nd->dentry));
	list_add_tail(&mnt->mnt_child, &nd->mnt->mnt_mounts);
	nd->dentry->d_mounted++;
}

static struct vfsmount *next_mnt(struct vfsmount *p, struct vfsmount *root)
{
	struct list_head *next = p->mnt_mounts.next;
	if (next == &p->mnt_mounts) {
		while (1) {
			if (p == root)
				return NULL;
			next = p->mnt_child.next;
			if (next != &p->mnt_parent->mnt_mounts)
				break;
			p = p->mnt_parent;
		}
	}
	return list_entry(next, struct vfsmount, mnt_child);
}

static struct vfsmount *
clone_mnt(struct vfsmount *old, struct dentry *root)
{
	struct super_block *sb = old->mnt_sb;
	struct vfsmount *mnt = alloc_vfsmnt(old->mnt_devname);

	if (mnt) {
		mnt->mnt_flags = old->mnt_flags;
		atomic_inc(&sb->s_active);
		mnt->mnt_sb = sb;
		mnt->mnt_root = dget(root);
		mnt->mnt_mountpoint = mnt->mnt_root;
		mnt->mnt_parent = mnt;
		mnt->mnt_namespace = old->mnt_namespace;

		/* stick the duplicate mount on the same expiry list
		 * as the original if that was on one */
		spin_lock(&vfsmount_lock);
		if (!list_empty(&old->mnt_fslink))
			list_add(&mnt->mnt_fslink, &old->mnt_fslink);
		spin_unlock(&vfsmount_lock);
	}
	return mnt;
}

void __mntput(struct vfsmount *mnt)
{
	struct super_block *sb = mnt->mnt_sb;
	dput(mnt->mnt_root);
	free_vfsmnt(mnt);
	deactivate_super(sb);
}

EXPORT_SYMBOL(__mntput);

/* iterator */
static void *m_start(struct seq_file *m, loff_t *pos)
{
	struct namespace *n = m->private;
	struct list_head *p;
	loff_t l = *pos;

	down_read(&n->sem);
	list_for_each(p, &n->list)
		if (!l--)
			return list_entry(p, struct vfsmount, mnt_list);
	return NULL;
}

static void *m_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct namespace *n = m->private;
	struct list_head *p = ((struct vfsmount *)v)->mnt_list.next;
	(*pos)++;
	return p==&n->list ? NULL : list_entry(p, struct vfsmount, mnt_list);
}

static void m_stop(struct seq_file *m, void *v)
{
	struct namespace *n = m->private;
	up_read(&n->sem);
}

static inline void mangle(struct seq_file *m, const char *s)
{
	seq_escape(m, s, " \t\n\\");
}

static int show_vfsmnt(struct seq_file *m, void *v)
{
	struct vfsmount *mnt = v;
	int err = 0;
	static struct proc_fs_info {
		int flag;
		char *str;
	} fs_info[] = {
		{ MS_SYNCHRONOUS, ",sync" },
		{ MS_DIRSYNC, ",dirsync" },
		{ MS_MANDLOCK, ",mand" },
		{ MS_NOATIME, ",noatime" },
		{ MS_NODIRATIME, ",nodiratime" },
		{ 0, NULL }
	};
	static struct proc_fs_info mnt_info[] = {
		{ MNT_NOSUID, ",nosuid" },
		{ MNT_NODEV, ",nodev" },
		{ MNT_NOEXEC, ",noexec" },
		{ 0, NULL }
	};
	struct proc_fs_info *fs_infop;

	mangle(m, mnt->mnt_devname ? mnt->mnt_devname : "none");
	seq_putc(m, ' ');
	seq_path(m, mnt, mnt->mnt_root, " \t\n\\");
	seq_putc(m, ' ');
	mangle(m, mnt->mnt_sb->s_type->name);
	seq_puts(m, mnt->mnt_sb->s_flags & MS_RDONLY ? " ro" : " rw");
	for (fs_infop = fs_info; fs_infop->flag; fs_infop++) {
		if (mnt->mnt_sb->s_flags & fs_infop->flag)
			seq_puts(m, fs_infop->str);
	}
	for (fs_infop = mnt_info; fs_infop->flag; fs_infop++) {
		if (mnt->mnt_flags & fs_infop->flag)
			seq_puts(m, fs_infop->str);
	}
	if (mnt->mnt_sb->s_op->show_options)
		err = mnt->mnt_sb->s_op->show_options(m, mnt);
	seq_puts(m, " 0 0\n");
	return err;
}

struct seq_operations mounts_op = {
	.start	= m_start,
	.next	= m_next,
	.stop	= m_stop,
	.show	= show_vfsmnt
};

/**
 * may_umount_tree - check if a mount tree is busy
 * @mnt: root of mount tree
 *
 * This is called to check if a tree of mounts has any
 * open files, pwds, chroots or sub mounts that are
 * busy.
 */
int may_umount_tree(struct vfsmount *mnt)
{
	struct list_head *next;
	struct vfsmount *this_parent = mnt;
	int actual_refs;
	int minimum_refs;

	spin_lock(&vfsmount_lock);
	actual_refs = atomic_read(&mnt->mnt_count);
	minimum_refs = 2;
repeat:
	next = this_parent->mnt_mounts.next;
resume:
	while (next != &this_parent->mnt_mounts) {
		struct vfsmount *p = list_entry(next, struct vfsmount, mnt_child);

		next = next->next;

		actual_refs += atomic_read(&p->mnt_count);
		minimum_refs += 2;

		if (!list_empty(&p->mnt_mounts)) {
			this_parent = p;
			goto repeat;
		}
	}

	if (this_parent != mnt) {
		next = this_parent->mnt_child.next;
		this_parent = this_parent->mnt_parent;
		goto resume;
	}
	spin_unlock(&vfsmount_lock);

	if (actual_refs > minimum_refs)
		return -EBUSY;

	return 0;
}

EXPORT_SYMBOL(may_umount_tree);

/**
 * may_umount - check if a mount point is busy
 * @mnt: root of mount
 *
 * This is called to check if a mount point has any
 * open files, pwds, chroots or sub mounts. If the
 * mount has sub mounts this will return busy
 * regardless of whether the sub mounts are busy.
 *
 * Doesn't take quota and stuff into account. IOW, in some cases it will
 * give false negatives. The main reason why it's here is that we need
 * a non-destructive way to look for easily umountable filesystems.
 */
int may_umount(struct vfsmount *mnt)
{
	if (atomic_read(&mnt->mnt_count) > 2)
		return -EBUSY;
	return 0;
}

EXPORT_SYMBOL(may_umount);

void umount_tree(struct vfsmount *mnt)
{
	struct vfsmount *p;
	LIST_HEAD(kill);

	for (p = mnt; p; p = next_mnt(p, mnt)) {
		list_del(&p->mnt_list);
		list_add(&p->mnt_list, &kill);
	}

	while (!list_empty(&kill)) {
		mnt = list_entry(kill.next, struct vfsmount, mnt_list);
		list_del_init(&mnt->mnt_list);
		list_del_init(&mnt->mnt_fslink);
		if (mnt->mnt_parent == mnt) {
			spin_unlock(&vfsmount_lock);
		} else {
			struct nameidata old_nd;
			detach_mnt(mnt, &old_nd);
			spin_unlock(&vfsmount_lock);
			path_release(&old_nd);
		}
		mntput(mnt);
		spin_lock(&vfsmount_lock);
	}
}

static int do_umount(struct vfsmount *mnt, int flags)
{
	struct super_block * sb = mnt->mnt_sb;
	int retval;

	retval = security_sb_umount(mnt, flags);
	if (retval)
		return retval;

	/*
	 * Allow userspace to request a mountpoint be expired rather than
	 * unmounting unconditionally. Unmount only happens if:
	 *  (1) the mark is already set (the mark is cleared by mntput())
	 *  (2) the usage count == 1 [parent vfsmount] + 1 [sys_umount]
	 */
	if (flags & MNT_EXPIRE) {
		if (mnt == current->fs->rootmnt ||
		    flags & (MNT_FORCE | MNT_DETACH))
			return -EINVAL;

		if (atomic_read(&mnt->mnt_count) != 2)
			return -EBUSY;

		if (!xchg(&mnt->mnt_expiry_mark, 1))
			return -EAGAIN;
	}

	/*
	 * If we may have to abort operations to get out of this
	 * mount, and they will themselves hold resources we must
	 * allow the fs to do things. In the Unix tradition of
	 * 'Gee thats tricky lets do it in userspace' the umount_begin
	 * might fail to complete on the first run through as other tasks
	 * must return, and the like. Thats for the mount program to worry
	 * about for the moment.
	 */

	lock_kernel();
	if( (flags&MNT_FORCE) && sb->s_op->umount_begin)
		sb->s_op->umount_begin(sb);
	unlock_kernel();

	/*
	 * No sense to grab the lock for this test, but test itself looks
	 * somewhat bogus. Suggestions for better replacement?
	 * Ho-hum... In principle, we might treat that as umount + switch
	 * to rootfs. GC would eventually take care of the old vfsmount.
	 * Actually it makes sense, especially if rootfs would contain a
	 * /reboot - static binary that would close all descriptors and
	 * call reboot(9). Then init(8) could umount root and exec /reboot.
	 */
	if (mnt == current->fs->rootmnt && !(flags & MNT_DETACH)) {
		/*
		 * Special case for "unmounting" root ...
		 * we just try to remount it readonly.
		 */
		down_write(&sb->s_umount);
		if (!(sb->s_flags & MS_RDONLY)) {
			lock_kernel();
			retval = do_remount_sb(sb, MS_RDONLY, NULL, 0);
			unlock_kernel();
		}
		up_write(&sb->s_umount);
		return retval;
	}

	down_write(&current->namespace->sem);
	spin_lock(&vfsmount_lock);

	if (atomic_read(&sb->s_active) == 1) {
		/* last instance - try to be smart */
		spin_unlock(&vfsmount_lock);
		lock_kernel();
		DQUOT_OFF(sb);
		acct_auto_close(sb);
		unlock_kernel();
		security_sb_umount_close(mnt);
		spin_lock(&vfsmount_lock);
	}
	retval = -EBUSY;
	if (atomic_read(&mnt->mnt_count) == 2 || flags & MNT_DETACH) {
		if (!list_empty(&mnt->mnt_list))
			umount_tree(mnt);
		retval = 0;
	}
	spin_unlock(&vfsmount_lock);
	if (retval)
		security_sb_umount_busy(mnt);
	up_write(&current->namespace->sem);
	return retval;
}

/*
 * Now umount can handle mount points as well as block devices.
 * This is important for filesystems which use unnamed block devices.
 *
 * We now support a flag for forced unmount like the other 'big iron'
 * unixes. Our API is identical to OSF/1 to avoid making a mess of AMD
 */

asmlinkage long sys_umount(char __user * name, int flags)
{
	struct nameidata nd;
	int retval;

	retval = __user_walk(name, LOOKUP_FOLLOW, &nd);
	if (retval)
		goto out;
	retval = -EINVAL;
	if (nd.dentry != nd.mnt->mnt_root)
		goto dput_and_out;
	if (!check_mnt(nd.mnt))
		goto dput_and_out;

	retval = -EPERM;
	if (!capable(CAP_SYS_ADMIN))
		goto dput_and_out;

	retval = do_umount(nd.mnt, flags);
dput_and_out:
	path_release_on_umount(&nd);
out:
	return retval;
}

#ifdef __ARCH_WANT_SYS_OLDUMOUNT

/*
 *	The 2.0 compatible umount. No flags. 
 */
 
asmlinkage long sys_oldumount(char __user * name)
{
	return sys_umount(name,0);
}

#endif

static int mount_is_safe(struct nameidata *nd)
{
	if (capable(CAP_SYS_ADMIN))
		return 0;
	return -EPERM;
#ifdef notyet
	if (S_ISLNK(nd->dentry->d_inode->i_mode))
		return -EPERM;
	if (nd->dentry->d_inode->i_mode & S_ISVTX) {
		if (current->uid != nd->dentry->d_inode->i_uid)
			return -EPERM;
	}
	if (permission(nd->dentry->d_inode, MAY_WRITE, nd))
		return -EPERM;
	return 0;
#endif
}

static int
lives_below_in_same_fs(struct dentry *d, struct dentry *dentry)
{
	while (1) {
		if (d == dentry)
			return 1;
		if (d == NULL || d == d->d_parent)
			return 0;
		d = d->d_parent;
	}
}

static struct vfsmount *copy_tree(struct vfsmount *mnt, struct dentry *dentry)
{
	struct vfsmount *res, *p, *q, *r, *s;
	struct list_head *h;
	struct nameidata nd;

	res = q = clone_mnt(mnt, dentry);
	if (!q)
		goto Enomem;
	q->mnt_mountpoint = mnt->mnt_mountpoint;

	p = mnt;
	for (h = mnt->mnt_mounts.next; h != &mnt->mnt_mounts; h = h->next) {
		r = list_entry(h, struct vfsmount, mnt_child);
		if (!lives_below_in_same_fs(r->mnt_mountpoint, dentry))
			continue;

		for (s = r; s; s = next_mnt(s, r)) {
			while (p != s->mnt_parent) {
				p = p->mnt_parent;
				q = q->mnt_parent;
			}
			p = s;
			nd.mnt = q;
			nd.dentry = p->mnt_mountpoint;
			q = clone_mnt(p, p->mnt_root);
			if (!q)
				goto Enomem;
			spin_lock(&vfsmount_lock);
			list_add_tail(&q->mnt_list, &res->mnt_list);
			attach_mnt(q, &nd);
			spin_unlock(&vfsmount_lock);
		}
	}
	return res;
 Enomem:
	if (res) {
		spin_lock(&vfsmount_lock);
		umount_tree(res);
		spin_unlock(&vfsmount_lock);
	}
	return NULL;
}

static int graft_tree(struct vfsmount *mnt, struct nameidata *nd)
{
	int err;
	if (mnt->mnt_sb->s_flags & MS_NOUSER)
		return -EINVAL;

	if (S_ISDIR(nd->dentry->d_inode->i_mode) !=
	      S_ISDIR(mnt->mnt_root->d_inode->i_mode))
		return -ENOTDIR;

	err = -ENOENT;
	down(&nd->dentry->d_inode->i_sem);
	if (IS_DEADDIR(nd->dentry->d_inode))
		goto out_unlock;

	err = security_sb_check_sb(mnt, nd);
	if (err)
		goto out_unlock;

	err = -ENOENT;
	spin_lock(&vfsmount_lock);
	if (IS_ROOT(nd->dentry) || !d_unhashed(nd->dentry)) {
		struct list_head head;

		attach_mnt(mnt, nd);
		list_add_tail(&head, &mnt->mnt_list);
		list_splice(&head, current->namespace->list.prev);
		mntget(mnt);
		err = 0;
	}
	spin_unlock(&vfsmount_lock);
out_unlock:
	up(&nd->dentry->d_inode->i_sem);
	if (!err)
		security_sb_post_addmount(mnt, nd);
	return err;
}

/*
 * do loopback mount.
 */
static int do_loopback(struct nameidata *nd, char *old_name, int recurse)
{
	struct nameidata old_nd;
	struct vfsmount *mnt = NULL;
	int err = mount_is_safe(nd);
	if (err)
		return err;
	if (!old_name || !*old_name)
		return -EINVAL;
	err = path_lookup(old_name, LOOKUP_FOLLOW, &old_nd);
	if (err)
		return err;

	down_write(&current->namespace->sem);
	err = -EINVAL;
	if (check_mnt(nd->mnt) && (!recurse || check_mnt(old_nd.mnt))) {
		err = -ENOMEM;
		if (recurse)
			mnt = copy_tree(old_nd.mnt, old_nd.dentry);
		else
			mnt = clone_mnt(old_nd.mnt, old_nd.dentry);
	}

	if (mnt) {
		/* stop bind mounts from expiring */
		spin_lock(&vfsmount_lock);
		list_del_init(&mnt->mnt_fslink);
		spin_unlock(&vfsmount_lock);

		err = graft_tree(mnt, nd);
		if (err) {
			spin_lock(&vfsmount_lock);
			umount_tree(mnt);
			spin_unlock(&vfsmount_lock);
		} else
			mntput(mnt);
	}

	up_write(&current->namespace->sem);
	path_release(&old_nd);
	return err;
}

/*
 * change filesystem flags. dir should be a physical root of filesystem.
 * If you've mounted a non-root directory somewhere and want to do remount
 * on it - tough luck.
 */

static int do_remount(struct nameidata *nd, int flags, int mnt_flags,
		      void *data)
{
	int err;
	struct super_block * sb = nd->mnt->mnt_sb;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (!check_mnt(nd->mnt))
		return -EINVAL;

	if (nd->dentry != nd->mnt->mnt_root)
		return -EINVAL;

	down_write(&sb->s_umount);
	err = do_remount_sb(sb, flags, data, 0);
	if (!err)
		nd->mnt->mnt_flags=mnt_flags;
	up_write(&sb->s_umount);
	if (!err)
		security_sb_post_remount(nd->mnt, flags, data);
	return err;
}

static int do_move_mount(struct nameidata *nd, char *old_name)
{
	struct nameidata old_nd, parent_nd;
	struct vfsmount *p;
	int err = 0;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!old_name || !*old_name)
		return -EINVAL;
	err = path_lookup(old_name, LOOKUP_FOLLOW, &old_nd);
	if (err)
		return err;

	down_write(&current->namespace->sem);
	while(d_mountpoint(nd->dentry) && follow_down(&nd->mnt, &nd->dentry))
		;
	err = -EINVAL;
	if (!check_mnt(nd->mnt) || !check_mnt(old_nd.mnt))
		goto out;

	err = -ENOENT;
	down(&nd->dentry->d_inode->i_sem);
	if (IS_DEADDIR(nd->dentry->d_inode))
		goto out1;

	spin_lock(&vfsmount_lock);
	if (!IS_ROOT(nd->dentry) && d_unhashed(nd->dentry))
		goto out2;

	err = -EINVAL;
	if (old_nd.dentry != old_nd.mnt->mnt_root)
		goto out2;

	if (old_nd.mnt == old_nd.mnt->mnt_parent)
		goto out2;

	if (S_ISDIR(nd->dentry->d_inode->i_mode) !=
	      S_ISDIR(old_nd.dentry->d_inode->i_mode))
		goto out2;

	err = -ELOOP;
	for (p = nd->mnt; p->mnt_parent!=p; p = p->mnt_parent)
		if (p == old_nd.mnt)
			goto out2;
	err = 0;

	detach_mnt(old_nd.mnt, &parent_nd);
	attach_mnt(old_nd.mnt, nd);

	/* if the mount is moved, it should no longer be expire
	 * automatically */
	list_del_init(&old_nd.mnt->mnt_fslink);
out2:
	spin_unlock(&vfsmount_lock);
out1:
	up(&nd->dentry->d_inode->i_sem);
out:
	up_write(&current->namespace->sem);
	if (!err)
		path_release(&parent_nd);
	path_release(&old_nd);
	return err;
}

/*
 * create a new mount for userspace and request it to be added into the
 * namespace's tree
 */
static int do_new_mount(struct nameidata *nd, char *type, int flags,
			int mnt_flags, char *name, void *data)
{
	struct vfsmount *mnt;

	if (!type || !memchr(type, 0, PAGE_SIZE))
		return -EINVAL;

	/* we need capabilities... */
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	mnt = do_kern_mount(type, flags, name, data);
	if (IS_ERR(mnt))
		return PTR_ERR(mnt);

	return do_add_mount(mnt, nd, mnt_flags, NULL);
}

/*
 * add a mount into a namespace's mount tree
 * - provide the option of adding the new mount to an expiration list
 */
int do_add_mount(struct vfsmount *newmnt, struct nameidata *nd,
		 int mnt_flags, struct list_head *fslist)
{
	int err;

	down_write(&current->namespace->sem);
	/* Something was mounted here while we slept */
	while(d_mountpoint(nd->dentry) && follow_down(&nd->mnt, &nd->dentry))
		;
	err = -EINVAL;
	if (!check_mnt(nd->mnt))
		goto unlock;

	/* Refuse the same filesystem on the same mount point */
	err = -EBUSY;
	if (nd->mnt->mnt_sb == newmnt->mnt_sb &&
	    nd->mnt->mnt_root == nd->dentry)
		goto unlock;

	err = -EINVAL;
	if (S_ISLNK(newmnt->mnt_root->d_inode->i_mode))
		goto unlock;

	newmnt->mnt_flags = mnt_flags;
	err = graft_tree(newmnt, nd);

	if (err == 0 && fslist) {
		/* add to the specified expiration list */
		spin_lock(&vfsmount_lock);
		list_add_tail(&newmnt->mnt_fslink, fslist);
		spin_unlock(&vfsmount_lock);
	}

unlock:
	up_write(&current->namespace->sem);
	mntput(newmnt);
	return err;
}

EXPORT_SYMBOL_GPL(do_add_mount);

/*
 * process a list of expirable mountpoints with the intent of discarding any
 * mountpoints that aren't in use and haven't been touched since last we came
 * here
 */
void mark_mounts_for_expiry(struct list_head *mounts)
{
	struct namespace *namespace;
	struct vfsmount *mnt, *next;
	LIST_HEAD(graveyard);

	if (list_empty(mounts))
		return;

	spin_lock(&vfsmount_lock);

	/* extract from the expiration list every vfsmount that matches the
	 * following criteria:
	 * - only referenced by its parent vfsmount
	 * - still marked for expiry (marked on the last call here; marks are
	 *   cleared by mntput())
	 */
	list_for_each_entry_safe(mnt, next, mounts, mnt_fslink) {
		if (!xchg(&mnt->mnt_expiry_mark, 1) ||
		    atomic_read(&mnt->mnt_count) != 1)
			continue;

		mntget(mnt);
		list_move(&mnt->mnt_fslink, &graveyard);
	}

	/*
	 * go through the vfsmounts we've just consigned to the graveyard to
	 * - check that they're still dead
	 * - delete the vfsmount from the appropriate namespace under lock
	 * - dispose of the corpse
	 */
	while (!list_empty(&graveyard)) {
		mnt = list_entry(graveyard.next, struct vfsmount, mnt_fslink);
		list_del_init(&mnt->mnt_fslink);

		/* don't do anything if the namespace is dead - all the
		 * vfsmounts from it are going away anyway */
		namespace = mnt->mnt_namespace;
		if (!namespace || atomic_read(&namespace->count) <= 0)
			continue;
		get_namespace(namespace);

		spin_unlock(&vfsmount_lock);
		down_write(&namespace->sem);
		spin_lock(&vfsmount_lock);

		/* check that it is still dead: the count should now be 2 - as
		 * contributed by the vfsmount parent and the mntget above */
		if (atomic_read(&mnt->mnt_count) == 2) {
			struct vfsmount *xdmnt;
			struct dentry *xdentry;

			/* delete from the namespace */
			list_del_init(&mnt->mnt_list);
			list_del_init(&mnt->mnt_child);
			list_del_init(&mnt->mnt_hash);
			mnt->mnt_mountpoint->d_mounted--;

			xdentry = mnt->mnt_mountpoint;
			mnt->mnt_mountpoint = mnt->mnt_root;
			xdmnt = mnt->mnt_parent;
			mnt->mnt_parent = mnt;

			spin_unlock(&vfsmount_lock);

			mntput(xdmnt);
			dput(xdentry);

			/* now lay it to rest if this was the last ref on the
			 * superblock */
			if (atomic_read(&mnt->mnt_sb->s_active) == 1) {
				/* last instance - try to be smart */
				lock_kernel();
				DQUOT_OFF(mnt->mnt_sb);
				acct_auto_close(mnt->mnt_sb);
				unlock_kernel();
			}

			mntput(mnt);
		} else {
			/* someone brought it back to life whilst we didn't
			 * have any locks held so return it to the expiration
			 * list */
			list_add_tail(&mnt->mnt_fslink, mounts);
			spin_unlock(&vfsmount_lock);
		}

		up_write(&namespace->sem);

		mntput(mnt);
		put_namespace(namespace);

		spin_lock(&vfsmount_lock);
	}

	spin_unlock(&vfsmount_lock);
}

EXPORT_SYMBOL_GPL(mark_mounts_for_expiry);

int copy_mount_options (const void __user *data, unsigned long *where)
{
	int i;
	unsigned long page;
	unsigned long size;
	
	*where = 0;
	if (!data)
		return 0;

	if (!(page = __get_free_page(GFP_KERNEL)))
		return -ENOMEM;

	/* We only care that *some* data at the address the user
	 * gave us is valid.  Just in case, we'll zero
	 * the remainder of the page.
	 */
	/* copy_from_user cannot cross TASK_SIZE ! */
	size = TASK_SIZE - (unsigned long)data;
	if (size > PAGE_SIZE)
		size = PAGE_SIZE;

	i = size - copy_from_user((void *)page, data, size);
	if (!i) {
		free_page(page); 
		return -EFAULT;
	}
	if (i != PAGE_SIZE)
		memset((char *)page + i, 0, PAGE_SIZE - i);
	*where = page;
	return 0;
}

/*
 * Flags is a 32-bit value that allows up to 31 non-fs dependent flags to
 * be given to the mount() call (ie: read-only, no-dev, no-suid etc).
 *
 * data is a (void *) that can point to any structure up to
 * PAGE_SIZE-1 bytes, which can contain arbitrary fs-dependent
 * information (or be NULL).
 *
 * Pre-0.97 versions of mount() didn't have a flags word.
 * When the flags word was introduced its top half was required
 * to have the magic value 0xC0ED, and this remained so until 2.4.0-test9.
 * Therefore, if this magic number is present, it carries no information
 * and must be discarded.
 */
long do_mount(char * dev_name, char * dir_name, char *type_page,
		  unsigned long flags, void *data_page)
{
	struct nameidata nd;
	int retval = 0;
	int mnt_flags = 0;

	/* Discard magic */
	if ((flags & MS_MGC_MSK) == MS_MGC_VAL)
		flags &= ~MS_MGC_MSK;

	/* Basic sanity checks */

	if (!dir_name || !*dir_name || !memchr(dir_name, 0, PAGE_SIZE))
		return -EINVAL;
	if (dev_name && !memchr(dev_name, 0, PAGE_SIZE))
		return -EINVAL;

	if (data_page)
		((char *)data_page)[PAGE_SIZE - 1] = 0;

	/* Separate the per-mountpoint flags */
	if (flags & MS_NOSUID)
		mnt_flags |= MNT_NOSUID;
	if (flags & MS_NODEV)
		mnt_flags |= MNT_NODEV;
	if (flags & MS_NOEXEC)
		mnt_flags |= MNT_NOEXEC;
	flags &= ~(MS_NOSUID|MS_NOEXEC|MS_NODEV|MS_ACTIVE);

	/* ... and get the mountpoint */
	retval = path_lookup(dir_name, LOOKUP_FOLLOW, &nd);
	if (retval)
		return retval;

	retval = security_sb_mount(dev_name, &nd, type_page, flags, data_page);
	if (retval)
		goto dput_out;

	if (flags & MS_REMOUNT)
		retval = do_remount(&nd, flags & ~MS_REMOUNT, mnt_flags,
				    data_page);
	else if (flags & MS_BIND)
		retval = do_loopback(&nd, dev_name, flags & MS_REC);
	else if (flags & MS_MOVE)
		retval = do_move_mount(&nd, dev_name);
	else
		retval = do_new_mount(&nd, type_page, flags, mnt_flags,
				      dev_name, data_page);
dput_out:
	path_release(&nd);
	return retval;
}

int copy_namespace(int flags, struct task_struct *tsk)
{
	struct namespace *namespace = tsk->namespace;
	struct namespace *new_ns;
	struct vfsmount *rootmnt = NULL, *pwdmnt = NULL, *altrootmnt = NULL;
	struct fs_struct *fs = tsk->fs;
	struct vfsmount *p, *q;

	if (!namespace)
		return 0;

	get_namespace(namespace);

	if (!(flags & CLONE_NEWNS))
		return 0;

	if (!capable(CAP_SYS_ADMIN)) {
		put_namespace(namespace);
		return -EPERM;
	}

	new_ns = kmalloc(sizeof(struct namespace), GFP_KERNEL);
	if (!new_ns)
		goto out;

	atomic_set(&new_ns->count, 1);
	init_rwsem(&new_ns->sem);
	INIT_LIST_HEAD(&new_ns->list);

	down_write(&tsk->namespace->sem);
	/* First pass: copy the tree topology */
	new_ns->root = copy_tree(namespace->root, namespace->root->mnt_root);
	if (!new_ns->root) {
		up_write(&tsk->namespace->sem);
		kfree(new_ns);
		goto out;
	}
	spin_lock(&vfsmount_lock);
	list_add_tail(&new_ns->list, &new_ns->root->mnt_list);
	spin_unlock(&vfsmount_lock);

	/*
	 * Second pass: switch the tsk->fs->* elements and mark new vfsmounts
	 * as belonging to new namespace.  We have already acquired a private
	 * fs_struct, so tsk->fs->lock is not needed.
	 */
	p = namespace->root;
	q = new_ns->root;
	while (p) {
		q->mnt_namespace = new_ns;
		if (fs) {
			if (p == fs->rootmnt) {
				rootmnt = p;
				fs->rootmnt = mntget(q);
			}
			if (p == fs->pwdmnt) {
				pwdmnt = p;
				fs->pwdmnt = mntget(q);
			}
			if (p == fs->altrootmnt) {
				altrootmnt = p;
				fs->altrootmnt = mntget(q);
			}
		}
		p = next_mnt(p, namespace->root);
		q = next_mnt(q, new_ns->root);
	}
	up_write(&tsk->namespace->sem);

	tsk->namespace = new_ns;

	if (rootmnt)
		mntput(rootmnt);
	if (pwdmnt)
		mntput(pwdmnt);
	if (altrootmnt)
		mntput(altrootmnt);

	put_namespace(namespace);
	return 0;

out:
	put_namespace(namespace);
	return -ENOMEM;
}

asmlinkage long sys_mount(char __user * dev_name, char __user * dir_name,
			  char __user * type, unsigned long flags,
			  void __user * data)
{
	int retval;
	unsigned long data_page;
	unsigned long type_page;
	unsigned long dev_page;
	char *dir_page;

	retval = copy_mount_options (type, &type_page);
	if (retval < 0)
		return retval;

	dir_page = getname(dir_name);
	retval = PTR_ERR(dir_page);
	if (IS_ERR(dir_page))
		goto out1;

	retval = copy_mount_options (dev_name, &dev_page);
	if (retval < 0)
		goto out2;

	retval = copy_mount_options (data, &data_page);
	if (retval < 0)
		goto out3;

	lock_kernel();
	retval = do_mount((char*)dev_page, dir_page, (char*)type_page,
			  flags, (void*)data_page);
	unlock_kernel();
	free_page(data_page);

out3:
	free_page(dev_page);
out2:
	putname(dir_page);
out1:
	free_page(type_page);
	return retval;
}

/*
 * Replace the fs->{rootmnt,root} with {mnt,dentry}. Put the old values.
 * It can block. Requires the big lock held.
 */
void set_fs_root(struct fs_struct *fs, struct vfsmount *mnt,
		 struct dentry *dentry)
{
	struct dentry *old_root;
	struct vfsmount *old_rootmnt;
	write_lock(&fs->lock);
	old_root = fs->root;
	old_rootmnt = fs->rootmnt;
	fs->rootmnt = mntget(mnt);
	fs->root = dget(dentry);
	write_unlock(&fs->lock);
	if (old_root) {
		dput(old_root);
		mntput(old_rootmnt);
	}
}

EXPORT_SYMBOL(set_fs_root);

/*
 * Replace the fs->{pwdmnt,pwd} with {mnt,dentry}. Put the old values.
 * It can block. Requires the big lock held.
 */
void set_fs_pwd(struct fs_struct *fs, struct vfsmount *mnt,
		struct dentry *dentry)
{
	struct dentry *old_pwd;
	struct vfsmount *old_pwdmnt;

	write_lock(&fs->lock);
	old_pwd = fs->pwd;
	old_pwdmnt = fs->pwdmnt;
	fs->pwdmnt = mntget(mnt);
	fs->pwd = dget(dentry);
	write_unlock(&fs->lock);

	if (old_pwd) {
		dput(old_pwd);
		mntput(old_pwdmnt);
	}
}

EXPORT_SYMBOL(set_fs_pwd);

static void chroot_fs_refs(struct nameidata *old_nd, struct nameidata *new_nd)
{
	struct task_struct *g, *p;
	struct fs_struct *fs;

	read_lock(&tasklist_lock);
	do_each_thread(g, p) {
		task_lock(p);
		fs = p->fs;
		if (fs) {
			atomic_inc(&fs->count);
			task_unlock(p);
			if (fs->root==old_nd->dentry&&fs->rootmnt==old_nd->mnt)
				set_fs_root(fs, new_nd->mnt, new_nd->dentry);
			if (fs->pwd==old_nd->dentry&&fs->pwdmnt==old_nd->mnt)
				set_fs_pwd(fs, new_nd->mnt, new_nd->dentry);
			put_fs_struct(fs);
		} else
			task_unlock(p);
	} while_each_thread(g, p);
	read_unlock(&tasklist_lock);
}

/*
 * Moves the current root to put_root, and sets root/cwd of all processes
 * which had them on the old root to new_root.
 *
 * Note:
 *  - we don't move root/cwd if they are not at the root (reason: if something
 *    cared enough to change them, it's probably wrong to force them elsewhere)
 *  - it's okay to pick a root that isn't the root of a file system, e.g.
 *    /nfs/my_root where /nfs is the mount point. It must be a mountpoint,
 *    though, so you may need to say mount --bind /nfs/my_root /nfs/my_root
 *    first.
 */

asmlinkage long sys_pivot_root(const char __user *new_root, const char __user *put_old)
{
	struct vfsmount *tmp;
	struct nameidata new_nd, old_nd, parent_nd, root_parent, user_nd;
	int error;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	lock_kernel();

	error = __user_walk(new_root, LOOKUP_FOLLOW|LOOKUP_DIRECTORY, &new_nd);
	if (error)
		goto out0;
	error = -EINVAL;
	if (!check_mnt(new_nd.mnt))
		goto out1;

	error = __user_walk(put_old, LOOKUP_FOLLOW|LOOKUP_DIRECTORY, &old_nd);
	if (error)
		goto out1;

	error = security_sb_pivotroot(&old_nd, &new_nd);
	if (error) {
		path_release(&old_nd);
		goto out1;
	}

	read_lock(&current->fs->lock);
	user_nd.mnt = mntget(current->fs->rootmnt);
	user_nd.dentry = dget(current->fs->root);
	read_unlock(&current->fs->lock);
	down_write(&current->namespace->sem);
	down(&old_nd.dentry->d_inode->i_sem);
	error = -EINVAL;
	if (!check_mnt(user_nd.mnt))
		goto out2;
	error = -ENOENT;
	if (IS_DEADDIR(new_nd.dentry->d_inode))
		goto out2;
	if (d_unhashed(new_nd.dentry) && !IS_ROOT(new_nd.dentry))
		goto out2;
	if (d_unhashed(old_nd.dentry) && !IS_ROOT(old_nd.dentry))
		goto out2;
	error = -EBUSY;
	if (new_nd.mnt == user_nd.mnt || old_nd.mnt == user_nd.mnt)
		goto out2; /* loop */
	error = -EINVAL;
	if (user_nd.mnt->mnt_root != user_nd.dentry)
		goto out2;
	if (new_nd.mnt->mnt_root != new_nd.dentry)
		goto out2; /* not a mountpoint */
	tmp = old_nd.mnt; /* make sure we can reach put_old from new_root */
	spin_lock(&vfsmount_lock);
	if (tmp != new_nd.mnt) {
		for (;;) {
			if (tmp->mnt_parent == tmp)
				goto out3;
			if (tmp->mnt_parent == new_nd.mnt)
				break;
			tmp = tmp->mnt_parent;
		}
		if (!is_subdir(tmp->mnt_mountpoint, new_nd.dentry))
			goto out3;
	} else if (!is_subdir(old_nd.dentry, new_nd.dentry))
		goto out3;
	detach_mnt(new_nd.mnt, &parent_nd);
	detach_mnt(user_nd.mnt, &root_parent);
	attach_mnt(user_nd.mnt, &old_nd);
	attach_mnt(new_nd.mnt, &root_parent);
	spin_unlock(&vfsmount_lock);
	chroot_fs_refs(&user_nd, &new_nd);
	security_sb_post_pivotroot(&user_nd, &new_nd);
	error = 0;
	path_release(&root_parent);
	path_release(&parent_nd);
out2:
	up(&old_nd.dentry->d_inode->i_sem);
	up_write(&current->namespace->sem);
	path_release(&user_nd);
	path_release(&old_nd);
out1:
	path_release(&new_nd);
out0:
	unlock_kernel();
	return error;
out3:
	spin_unlock(&vfsmount_lock);
	goto out2;
}

static void __init init_mount_tree(void)
{
	struct vfsmount *mnt;
	struct namespace *namespace;
	struct task_struct *g, *p;

	mnt = do_kern_mount("rootfs", 0, "rootfs", NULL);
	if (IS_ERR(mnt))
		panic("Can't create rootfs");
	namespace = kmalloc(sizeof(*namespace), GFP_KERNEL);
	if (!namespace)
		panic("Can't allocate initial namespace");
	atomic_set(&namespace->count, 1);
	INIT_LIST_HEAD(&namespace->list);
	init_rwsem(&namespace->sem);
	list_add(&mnt->mnt_list, &namespace->list);
	namespace->root = mnt;
	mnt->mnt_namespace = namespace;

	init_task.namespace = namespace;
	read_lock(&tasklist_lock);
	do_each_thread(g, p) {
		get_namespace(namespace);
		p->namespace = namespace;
	} while_each_thread(g, p);
	read_unlock(&tasklist_lock);

	set_fs_pwd(current->fs, namespace->root, namespace->root->mnt_root);
	set_fs_root(current->fs, namespace->root, namespace->root->mnt_root);
}

void __init mnt_init(unsigned long mempages)
{
	struct list_head *d;
	unsigned long order;
	unsigned int nr_hash;
	int i;

	mnt_cache = kmem_cache_create("mnt_cache", sizeof(struct vfsmount),
			0, SLAB_HWCACHE_ALIGN|SLAB_PANIC, NULL, NULL);

	order = 0; 
	mount_hashtable = (struct list_head *)
		__get_free_pages(GFP_ATOMIC, order);

	if (!mount_hashtable)
		panic("Failed to allocate mount hash table\n");

	/*
	 * Find the power-of-two list-heads that can fit into the allocation..
	 * We don't guarantee that "sizeof(struct list_head)" is necessarily
	 * a power-of-two.
	 */
	nr_hash = (1UL << order) * PAGE_SIZE / sizeof(struct list_head);
	hash_bits = 0;
	do {
		hash_bits++;
	} while ((nr_hash >> hash_bits) != 0);
	hash_bits--;

	/*
	 * Re-calculate the actual number of entries and the mask
	 * from the number of bits we can fit.
	 */
	nr_hash = 1UL << hash_bits;
	hash_mask = nr_hash-1;

	printk("Mount-cache hash table entries: %d (order: %ld, %ld bytes)\n",
			nr_hash, order, (PAGE_SIZE << order));

	/* And initialize the newly allocated array */
	d = mount_hashtable;
	i = nr_hash;
	do {
		INIT_LIST_HEAD(d);
		d++;
		i--;
	} while (i);
	sysfs_init();
	init_rootfs();
	init_mount_tree();
}

void __put_namespace(struct namespace *namespace)
{
	struct vfsmount *mnt;

	down_write(&namespace->sem);
	spin_lock(&vfsmount_lock);

	list_for_each_entry(mnt, &namespace->list, mnt_list) {
		mnt->mnt_namespace = NULL;
	}

	umount_tree(namespace->root);
	spin_unlock(&vfsmount_lock);
	up_write(&namespace->sem);
	kfree(namespace);
}
