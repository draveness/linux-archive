/* -*- c -*- ------------------------------------------------------------- *
 *   
 * linux/fs/autofs/autofs_i.h
 *
 *   Copyright 1997-1998 Transmeta Corporation - All Rights Reserved
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * ----------------------------------------------------------------------- */

/* Internal header file for autofs */

#include <linux/auto_fs4.h>
#include <linux/list.h>

/* This is the range of ioctl() numbers we claim as ours */
#define AUTOFS_IOC_FIRST     AUTOFS_IOC_READY
#define AUTOFS_IOC_COUNT     32

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <asm/current.h>
#include <asm/uaccess.h>

/* #define DEBUG */

#ifdef DEBUG
#define DPRINTK(fmt,args...) do { printk(KERN_DEBUG "pid %d: %s: " fmt "\n" , current->pid , __FUNCTION__ , ##args); } while(0)
#else
#define DPRINTK(fmt,args...) do {} while(0)
#endif

#define AUTOFS_SUPER_MAGIC 0x0187

/*
 * If the daemon returns a negative response (AUTOFS_IOC_FAIL) then the
 * kernel will keep the negative response cached for up to the time given
 * here, although the time can be shorter if the kernel throws the dcache
 * entry away.  This probably should be settable from user space.
 */
#define AUTOFS_NEGATIVE_TIMEOUT (60*HZ)	/* 1 minute */

/* Unified info structure.  This is pointed to by both the dentry and
   inode structures.  Each file in the filesystem has an instance of this
   structure.  It holds a reference to the dentry, so dentries are never
   flushed while the file exists.  All name lookups are dealt with at the
   dentry level, although the filesystem can interfere in the validation
   process.  Readdir is implemented by traversing the dentry lists. */
struct autofs_info {
	struct dentry	*dentry;
	struct inode	*inode;

	int		flags;

	struct autofs_sb_info *sbi;
	unsigned long last_used;

	mode_t	mode;
	size_t	size;

	void (*free)(struct autofs_info *);
	union {
		const char *symlink;
	} u;
};

#define AUTOFS_INF_EXPIRING	(1<<0) /* dentry is in the process of expiring */

struct autofs_wait_queue {
	wait_queue_head_t queue;
	struct autofs_wait_queue *next;
	autofs_wqt_t wait_queue_token;
	/* We use the following to see what we are waiting for */
	int hash;
	int len;
	char *name;
	/* This is for status reporting upon return */
	int status;
	atomic_t wait_ctr;
};

#define AUTOFS_SBI_MAGIC 0x6d4a556d

struct autofs_sb_info {
	u32 magic;
	struct file *pipe;
	pid_t oz_pgrp;
	int catatonic;
	int version;
	int sub_version;
	unsigned long exp_timeout;
	int reghost_enabled;
	int needs_reghost;
	struct super_block *sb;
	struct semaphore wq_sem;
	struct autofs_wait_queue *queues; /* Wait queue pointer */
};

static inline struct autofs_sb_info *autofs4_sbi(struct super_block *sb)
{
	return (struct autofs_sb_info *)(sb->s_fs_info);
}

static inline struct autofs_info *autofs4_dentry_ino(struct dentry *dentry)
{
	return (struct autofs_info *)(dentry->d_fsdata);
}

/* autofs4_oz_mode(): do we see the man behind the curtain?  (The
   processes which do manipulations for us in user space sees the raw
   filesystem without "magic".) */

static inline int autofs4_oz_mode(struct autofs_sb_info *sbi) {
	return sbi->catatonic || process_group(current) == sbi->oz_pgrp;
}

/* Does a dentry have some pending activity? */
static inline int autofs4_ispending(struct dentry *dentry)
{
	struct autofs_info *inf = autofs4_dentry_ino(dentry);

	return (dentry->d_flags & DCACHE_AUTOFS_PENDING) ||
		(inf != NULL && inf->flags & AUTOFS_INF_EXPIRING);
}

static inline void autofs4_copy_atime(struct file *src, struct file *dst)
{
	dst->f_dentry->d_inode->i_atime = src->f_dentry->d_inode->i_atime;
	return;
}

struct inode *autofs4_get_inode(struct super_block *, struct autofs_info *);
struct autofs_info *autofs4_init_inf(struct autofs_sb_info *, mode_t mode);
void autofs4_free_ino(struct autofs_info *);

/* Expiration */
int is_autofs4_dentry(struct dentry *);
int autofs4_expire_run(struct super_block *, struct vfsmount *,
			struct autofs_sb_info *,
			struct autofs_packet_expire __user *);
int autofs4_expire_multi(struct super_block *, struct vfsmount *,
			struct autofs_sb_info *, int __user *);

/* Operations structures */

extern struct inode_operations autofs4_symlink_inode_operations;
extern struct inode_operations autofs4_dir_inode_operations;
extern struct inode_operations autofs4_root_inode_operations;
extern struct file_operations autofs4_dir_operations;
extern struct file_operations autofs4_root_operations;

/* Initializing function */

int autofs4_fill_super(struct super_block *, void *, int);
struct autofs_info *autofs4_init_ino(struct autofs_info *, struct autofs_sb_info *sbi, mode_t mode);

/* Queue management functions */

enum autofs_notify
{
	NFY_NONE,
	NFY_MOUNT,
	NFY_EXPIRE
};

int autofs4_wait(struct autofs_sb_info *,struct dentry *, enum autofs_notify);
int autofs4_wait_release(struct autofs_sb_info *,autofs_wqt_t,int);
void autofs4_catatonic_mode(struct autofs_sb_info *);

static inline int simple_positive(struct dentry *dentry)
{
	return dentry->d_inode && !d_unhashed(dentry);
}

static inline int simple_empty_nolock(struct dentry *dentry)
{
	struct dentry *child;
	int ret = 0;

	list_for_each_entry(child, &dentry->d_subdirs, d_child)
		if (simple_positive(child))
			goto out;
	ret = 1;
out:
	return ret;
}
