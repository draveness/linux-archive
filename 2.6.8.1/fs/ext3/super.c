/*
 *  linux/fs/ext3/super.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/inode.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/time.h>
#include <linux/jbd.h>
#include <linux/ext3_fs.h>
#include <linux/ext3_jbd.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/parser.h>
#include <linux/smp_lock.h>
#include <linux/buffer_head.h>
#include <linux/vfs.h>
#include <linux/random.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/quotaops.h>
#include <asm/uaccess.h>
#include "xattr.h"
#include "acl.h"

static int ext3_load_journal(struct super_block *, struct ext3_super_block *);
static int ext3_create_journal(struct super_block *, struct ext3_super_block *,
			       int);
static void ext3_commit_super (struct super_block * sb,
			       struct ext3_super_block * es,
			       int sync);
static void ext3_mark_recovery_complete(struct super_block * sb,
					struct ext3_super_block * es);
static void ext3_clear_journal_err(struct super_block * sb,
				   struct ext3_super_block * es);
static int ext3_sync_fs(struct super_block *sb, int wait);

/* 
 * Wrappers for journal_start/end.
 *
 * The only special thing we need to do here is to make sure that all
 * journal_end calls result in the superblock being marked dirty, so
 * that sync() will call the filesystem's write_super callback if
 * appropriate. 
 */
handle_t *ext3_journal_start(struct inode *inode, int nblocks)
{
	journal_t *journal;

	if (inode->i_sb->s_flags & MS_RDONLY)
		return ERR_PTR(-EROFS);

	/* Special case here: if the journal has aborted behind our
	 * backs (eg. EIO in the commit thread), then we still need to
	 * take the FS itself readonly cleanly. */
	journal = EXT3_JOURNAL(inode);
	if (is_journal_aborted(journal)) {
		ext3_abort(inode->i_sb, __FUNCTION__,
			   "Detected aborted journal");
		return ERR_PTR(-EROFS);
	}

	return journal_start(journal, nblocks);
}

/* 
 * The only special thing we need to do here is to make sure that all
 * journal_stop calls result in the superblock being marked dirty, so
 * that sync() will call the filesystem's write_super callback if
 * appropriate. 
 */
int __ext3_journal_stop(const char *where, handle_t *handle)
{
	struct super_block *sb;
	int err;
	int rc;

	sb = handle->h_transaction->t_journal->j_private;
	err = handle->h_err;
	rc = journal_stop(handle);

	if (!err)
		err = rc;
	if (err)
		__ext3_std_error(sb, where, err);
	return err;
}

void ext3_journal_abort_handle(const char *caller, const char *err_fn,
		struct buffer_head *bh, handle_t *handle, int err)
{
	char nbuf[16];
	const char *errstr = ext3_decode_error(NULL, err, nbuf);

	printk(KERN_ERR "%s: aborting transaction: %s in %s", 
	       caller, errstr, err_fn);

	if (bh)
		BUFFER_TRACE(bh, "abort");
	journal_abort_handle(handle);
	if (!handle->h_err)
		handle->h_err = err;
}

static char error_buf[1024];

/* Deal with the reporting of failure conditions on a filesystem such as
 * inconsistencies detected or read IO failures.
 *
 * On ext2, we can store the error state of the filesystem in the
 * superblock.  That is not possible on ext3, because we may have other
 * write ordering constraints on the superblock which prevent us from
 * writing it out straight away; and given that the journal is about to
 * be aborted, we can't rely on the current, or future, transactions to
 * write out the superblock safely.
 *
 * We'll just use the journal_abort() error code to record an error in
 * the journal instead.  On recovery, the journal will compain about
 * that error until we've noted it down and cleared it.
 */

static void ext3_handle_error(struct super_block *sb)
{
	struct ext3_super_block *es = EXT3_SB(sb)->s_es;

	EXT3_SB(sb)->s_mount_state |= EXT3_ERROR_FS;
	es->s_state |= cpu_to_le32(EXT3_ERROR_FS);

	if (sb->s_flags & MS_RDONLY)
		return;

	if (test_opt (sb, ERRORS_PANIC))
		panic ("EXT3-fs (device %s): panic forced after error\n",
		       sb->s_id);
	if (test_opt (sb, ERRORS_RO)) {
		printk (KERN_CRIT "Remounting filesystem read-only\n");
		sb->s_flags |= MS_RDONLY;
	} else {
		journal_t *journal = EXT3_SB(sb)->s_journal;

		EXT3_SB(sb)->s_mount_opt |= EXT3_MOUNT_ABORT;
		if (journal)
			journal_abort(journal, -EIO);
	}
	ext3_commit_super(sb, es, 1);
}

void ext3_error (struct super_block * sb, const char * function,
		 const char * fmt, ...)
{
	va_list args;

	va_start (args, fmt);
	vsprintf (error_buf, fmt, args);
	va_end (args);

	printk (KERN_CRIT "EXT3-fs error (device %s): %s: %s\n",
		sb->s_id, function, error_buf);

	ext3_handle_error(sb);
}

const char *ext3_decode_error(struct super_block * sb, int errno, char nbuf[16])
{
	char *errstr = NULL;

	switch (errno) {
	case -EIO:
		errstr = "IO failure";
		break;
	case -ENOMEM:
		errstr = "Out of memory";
		break;
	case -EROFS:
		if (!sb || EXT3_SB(sb)->s_journal->j_flags & JFS_ABORT)
			errstr = "Journal has aborted";
		else
			errstr = "Readonly filesystem";
		break;
	default:
		/* If the caller passed in an extra buffer for unknown
		 * errors, textualise them now.  Else we just return
		 * NULL. */
		if (nbuf) {
			/* Check for truncated error codes... */
			if (snprintf(nbuf, 16, "error %d", -errno) >= 0)
				errstr = nbuf;
		}
		break;
	}

	return errstr;
}

/* __ext3_std_error decodes expected errors from journaling functions
 * automatically and invokes the appropriate error response.  */

void __ext3_std_error (struct super_block * sb, const char * function,
		       int errno)
{
	char nbuf[16];
	const char *errstr = ext3_decode_error(sb, errno, nbuf);

	printk (KERN_CRIT "EXT3-fs error (device %s) in %s: %s\n",
		sb->s_id, function, errstr);

	ext3_handle_error(sb);
}

/*
 * ext3_abort is a much stronger failure handler than ext3_error.  The
 * abort function may be used to deal with unrecoverable failures such
 * as journal IO errors or ENOMEM at a critical moment in log management.
 *
 * We unconditionally force the filesystem into an ABORT|READONLY state,
 * unless the error response on the fs has been set to panic in which
 * case we take the easy way out and panic immediately.
 */

void ext3_abort (struct super_block * sb, const char * function,
		 const char * fmt, ...)
{
	va_list args;

	printk (KERN_CRIT "ext3_abort called.\n");

	va_start (args, fmt);
	vsprintf (error_buf, fmt, args);
	va_end (args);

	if (test_opt (sb, ERRORS_PANIC))
		panic ("EXT3-fs panic (device %s): %s: %s\n",
		       sb->s_id, function, error_buf);

	printk (KERN_CRIT "EXT3-fs abort (device %s): %s: %s\n",
		sb->s_id, function, error_buf);

	if (sb->s_flags & MS_RDONLY)
		return;

	printk (KERN_CRIT "Remounting filesystem read-only\n");
	EXT3_SB(sb)->s_mount_state |= EXT3_ERROR_FS;
	sb->s_flags |= MS_RDONLY;
	EXT3_SB(sb)->s_mount_opt |= EXT3_MOUNT_ABORT;
	journal_abort(EXT3_SB(sb)->s_journal, -EIO);
}

/* Deal with the reporting of failure conditions while running, such as
 * inconsistencies in operation or invalid system states.
 *
 * Use ext3_error() for cases of invalid filesystem states, as that will
 * record an error on disk and force a filesystem check on the next boot.
 */
NORET_TYPE void ext3_panic (struct super_block * sb, const char * function,
			    const char * fmt, ...)
{
	va_list args;

	va_start (args, fmt);
	vsprintf (error_buf, fmt, args);
	va_end (args);

	/* this is to prevent panic from syncing this filesystem */
	/* AKPM: is this sufficient? */
	sb->s_flags |= MS_RDONLY;
	panic ("EXT3-fs panic (device %s): %s: %s\n",
	       sb->s_id, function, error_buf);
}

void ext3_warning (struct super_block * sb, const char * function,
		   const char * fmt, ...)
{
	va_list args;

	va_start (args, fmt);
	vsprintf (error_buf, fmt, args);
	va_end (args);
	printk (KERN_WARNING "EXT3-fs warning (device %s): %s: %s\n",
		sb->s_id, function, error_buf);
}

void ext3_update_dynamic_rev(struct super_block *sb)
{
	struct ext3_super_block *es = EXT3_SB(sb)->s_es;

	if (le32_to_cpu(es->s_rev_level) > EXT3_GOOD_OLD_REV)
		return;

	ext3_warning(sb, __FUNCTION__,
		     "updating to rev %d because of new feature flag, "
		     "running e2fsck is recommended",
		     EXT3_DYNAMIC_REV);

	es->s_first_ino = cpu_to_le32(EXT3_GOOD_OLD_FIRST_INO);
	es->s_inode_size = cpu_to_le16(EXT3_GOOD_OLD_INODE_SIZE);
	es->s_rev_level = cpu_to_le32(EXT3_DYNAMIC_REV);
	/* leave es->s_feature_*compat flags alone */
	/* es->s_uuid will be set by e2fsck if empty */

	/*
	 * The rest of the superblock fields should be zero, and if not it
	 * means they are likely already in use, so leave them alone.  We
	 * can leave it up to e2fsck to clean up any inconsistencies there.
	 */
}

/*
 * Open the external journal device
 */
static struct block_device *ext3_blkdev_get(dev_t dev)
{
	struct block_device *bdev;
	char b[BDEVNAME_SIZE];

	bdev = open_by_devnum(dev, FMODE_READ|FMODE_WRITE);
	if (IS_ERR(bdev))
		goto fail;
	return bdev;

fail:
	printk(KERN_ERR "EXT3: failed to open journal device %s: %ld\n",
			__bdevname(dev, b), PTR_ERR(bdev));
	return NULL;
}

/*
 * Release the journal device
 */
static int ext3_blkdev_put(struct block_device *bdev)
{
	bd_release(bdev);
	return blkdev_put(bdev);
}

static int ext3_blkdev_remove(struct ext3_sb_info *sbi)
{
	struct block_device *bdev;
	int ret = -ENODEV;

	bdev = sbi->journal_bdev;
	if (bdev) {
		ret = ext3_blkdev_put(bdev);
		sbi->journal_bdev = NULL;
	}
	return ret;
}

static inline struct inode *orphan_list_entry(struct list_head *l)
{
	return &list_entry(l, struct ext3_inode_info, i_orphan)->vfs_inode;
}

static void dump_orphan_list(struct super_block *sb, struct ext3_sb_info *sbi)
{
	struct list_head *l;

	printk(KERN_ERR "sb orphan head is %d\n", 
	       le32_to_cpu(sbi->s_es->s_last_orphan));

	printk(KERN_ERR "sb_info orphan list:\n");
	list_for_each(l, &sbi->s_orphan) {
		struct inode *inode = orphan_list_entry(l);
		printk(KERN_ERR "  "
		       "inode %s:%ld at %p: mode %o, nlink %d, next %d\n",
		       inode->i_sb->s_id, inode->i_ino, inode,
		       inode->i_mode, inode->i_nlink, 
		       le32_to_cpu(NEXT_ORPHAN(inode)));
	}
}

void ext3_put_super (struct super_block * sb)
{
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	struct ext3_super_block *es = sbi->s_es;
	int i;

	ext3_xattr_put_super(sb);
	journal_destroy(sbi->s_journal);
	if (!(sb->s_flags & MS_RDONLY)) {
		EXT3_CLEAR_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_RECOVER);
		es->s_state = le16_to_cpu(sbi->s_mount_state);
		BUFFER_TRACE(sbi->s_sbh, "marking dirty");
		mark_buffer_dirty(sbi->s_sbh);
		ext3_commit_super(sb, es, 1);
	}

	for (i = 0; i < sbi->s_gdb_count; i++)
		brelse(sbi->s_group_desc[i]);
	kfree(sbi->s_group_desc);
	kfree(sbi->s_debts);
	brelse(sbi->s_sbh);
#ifdef CONFIG_QUOTA
	for (i = 0; i < MAXQUOTAS; i++) {
		if (sbi->s_qf_names[i])
			kfree(sbi->s_qf_names[i]);
	}
#endif

	/* Debugging code just in case the in-memory inode orphan list
	 * isn't empty.  The on-disk one can be non-empty if we've
	 * detected an error and taken the fs readonly, but the
	 * in-memory list had better be clean by this point. */
	if (!list_empty(&sbi->s_orphan))
		dump_orphan_list(sb, sbi);
	J_ASSERT(list_empty(&sbi->s_orphan));

	invalidate_bdev(sb->s_bdev, 0);
	if (sbi->journal_bdev && sbi->journal_bdev != sb->s_bdev) {
		/*
		 * Invalidate the journal device's buffers.  We don't want them
		 * floating about in memory - the physical journal device may
		 * hotswapped, and it breaks the `ro-after' testing code.
		 */
		sync_blockdev(sbi->journal_bdev);
		invalidate_bdev(sbi->journal_bdev, 0);
		ext3_blkdev_remove(sbi);
	}
	sb->s_fs_info = NULL;
	kfree(sbi);
	return;
}

static kmem_cache_t *ext3_inode_cachep;

/*
 * Called inside transaction, so use GFP_NOFS
 */
static struct inode *ext3_alloc_inode(struct super_block *sb)
{
	struct ext3_inode_info *ei;

	ei = kmem_cache_alloc(ext3_inode_cachep, SLAB_NOFS);
	if (!ei)
		return NULL;
#ifdef CONFIG_EXT3_FS_POSIX_ACL
	ei->i_acl = EXT3_ACL_NOT_CACHED;
	ei->i_default_acl = EXT3_ACL_NOT_CACHED;
#endif
	ei->vfs_inode.i_version = 1;
	return &ei->vfs_inode;
}

static void ext3_destroy_inode(struct inode *inode)
{
	kmem_cache_free(ext3_inode_cachep, EXT3_I(inode));
}

static void init_once(void * foo, kmem_cache_t * cachep, unsigned long flags)
{
	struct ext3_inode_info *ei = (struct ext3_inode_info *) foo;

	if ((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) ==
	    SLAB_CTOR_CONSTRUCTOR) {
		INIT_LIST_HEAD(&ei->i_orphan);
#ifdef CONFIG_EXT3_FS_XATTR
		init_rwsem(&ei->xattr_sem);
#endif
		init_MUTEX(&ei->truncate_sem);
		inode_init_once(&ei->vfs_inode);
	}
}
 
static int init_inodecache(void)
{
	ext3_inode_cachep = kmem_cache_create("ext3_inode_cache",
					     sizeof(struct ext3_inode_info),
					     0, SLAB_HWCACHE_ALIGN|SLAB_RECLAIM_ACCOUNT,
					     init_once, NULL);
	if (ext3_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	if (kmem_cache_destroy(ext3_inode_cachep))
		printk(KERN_INFO "ext3_inode_cache: not all structures were freed\n");
}

#ifdef CONFIG_EXT3_FS_POSIX_ACL

static void ext3_clear_inode(struct inode *inode)
{
       if (EXT3_I(inode)->i_acl &&
           EXT3_I(inode)->i_acl != EXT3_ACL_NOT_CACHED) {
               posix_acl_release(EXT3_I(inode)->i_acl);
               EXT3_I(inode)->i_acl = EXT3_ACL_NOT_CACHED;
       }
       if (EXT3_I(inode)->i_default_acl &&
           EXT3_I(inode)->i_default_acl != EXT3_ACL_NOT_CACHED) {
               posix_acl_release(EXT3_I(inode)->i_default_acl);
               EXT3_I(inode)->i_default_acl = EXT3_ACL_NOT_CACHED;
       }
}

#else
# define ext3_clear_inode NULL
#endif

#ifdef CONFIG_QUOTA

#define QTYPE2NAME(t) ((t)==USRQUOTA?"user":"group")
#define QTYPE2MOPT(on, t) ((t)==USRQUOTA?((on)##USRJQUOTA):((on)##GRPJQUOTA))

static int ext3_dquot_initialize(struct inode *inode, int type);
static int ext3_dquot_drop(struct inode *inode);
static int ext3_write_dquot(struct dquot *dquot);
static int ext3_acquire_dquot(struct dquot *dquot);
static int ext3_release_dquot(struct dquot *dquot);
static int ext3_mark_dquot_dirty(struct dquot *dquot);
static int ext3_write_info(struct super_block *sb, int type);
static int ext3_quota_on(struct super_block *sb, int type, int format_id, char *path);
static int ext3_quota_on_mount(struct super_block *sb, int type);
static int ext3_quota_off_mount(struct super_block *sb, int type);

static struct dquot_operations ext3_quota_operations = {
	.initialize	= ext3_dquot_initialize,
	.drop		= ext3_dquot_drop,
	.alloc_space	= dquot_alloc_space,
	.alloc_inode	= dquot_alloc_inode,
	.free_space	= dquot_free_space,
	.free_inode	= dquot_free_inode,
	.transfer	= dquot_transfer,
	.write_dquot	= ext3_write_dquot,
	.acquire_dquot	= ext3_acquire_dquot,
	.release_dquot	= ext3_release_dquot,
	.mark_dirty	= ext3_mark_dquot_dirty,
	.write_info	= ext3_write_info
};

static struct quotactl_ops ext3_qctl_operations = {
	.quota_on	= ext3_quota_on,
	.quota_off	= vfs_quota_off,
	.quota_sync	= vfs_quota_sync,
	.get_info	= vfs_get_dqinfo,
	.set_info	= vfs_set_dqinfo,
	.get_dqblk	= vfs_get_dqblk,
	.set_dqblk	= vfs_set_dqblk
};
#endif

static struct super_operations ext3_sops = {
	.alloc_inode	= ext3_alloc_inode,
	.destroy_inode	= ext3_destroy_inode,
	.read_inode	= ext3_read_inode,
	.write_inode	= ext3_write_inode,
	.dirty_inode	= ext3_dirty_inode,
	.put_inode	= ext3_put_inode,
	.delete_inode	= ext3_delete_inode,
	.put_super	= ext3_put_super,
	.write_super	= ext3_write_super,
	.sync_fs	= ext3_sync_fs,
	.write_super_lockfs = ext3_write_super_lockfs,
	.unlockfs	= ext3_unlockfs,
	.statfs		= ext3_statfs,
	.remount_fs	= ext3_remount,
	.clear_inode	= ext3_clear_inode,
};

struct dentry *ext3_get_parent(struct dentry *child);
static struct export_operations ext3_export_ops = {
	.get_parent = ext3_get_parent,
};

enum {
	Opt_bsd_df, Opt_minix_df, Opt_grpid, Opt_nogrpid,
	Opt_resgid, Opt_resuid, Opt_sb, Opt_err_cont, Opt_err_panic, Opt_err_ro,
	Opt_nouid32, Opt_check, Opt_nocheck, Opt_debug, Opt_oldalloc, Opt_orlov,
	Opt_user_xattr, Opt_nouser_xattr, Opt_acl, Opt_noacl, Opt_noload,
	Opt_commit, Opt_journal_update, Opt_journal_inum,
	Opt_abort, Opt_data_journal, Opt_data_ordered, Opt_data_writeback,
	Opt_usrjquota, Opt_grpjquota, Opt_offusrjquota, Opt_offgrpjquota,
	Opt_jqfmt_vfsold, Opt_jqfmt_vfsv0,
	Opt_ignore, Opt_err,
};

static match_table_t tokens = {
	{Opt_bsd_df, "bsddf"},
	{Opt_minix_df, "minixdf"},
	{Opt_grpid, "grpid"},
	{Opt_grpid, "bsdgroups"},
	{Opt_nogrpid, "nogrpid"},
	{Opt_nogrpid, "sysvgroups"},
	{Opt_resgid, "resgid=%u"},
	{Opt_resuid, "resuid=%u"},
	{Opt_sb, "sb=%u"},
	{Opt_err_cont, "errors=continue"},
	{Opt_err_panic, "errors=panic"},
	{Opt_err_ro, "errors=remount-ro"},
	{Opt_nouid32, "nouid32"},
	{Opt_nocheck, "nocheck"},
	{Opt_nocheck, "check=none"},
	{Opt_check, "check"},
	{Opt_debug, "debug"},
	{Opt_oldalloc, "oldalloc"},
	{Opt_orlov, "orlov"},
	{Opt_user_xattr, "user_xattr"},
	{Opt_nouser_xattr, "nouser_xattr"},
	{Opt_acl, "acl"},
	{Opt_noacl, "noacl"},
	{Opt_noload, "noload"},
	{Opt_commit, "commit=%u"},
	{Opt_journal_update, "journal=update"},
	{Opt_journal_inum, "journal=%u"},
	{Opt_abort, "abort"},
	{Opt_data_journal, "data=journal"},
	{Opt_data_ordered, "data=ordered"},
	{Opt_data_writeback, "data=writeback"},
	{Opt_offusrjquota, "usrjquota="},
	{Opt_usrjquota, "usrjquota=%s"},
	{Opt_offgrpjquota, "grpjquota="},
	{Opt_grpjquota, "grpjquota=%s"},
	{Opt_jqfmt_vfsold, "jqfmt=vfsold"},
	{Opt_jqfmt_vfsv0, "jqfmt=vfsv0"},
	{Opt_ignore, "grpquota"},
	{Opt_ignore, "noquota"},
	{Opt_ignore, "quota"},
	{Opt_ignore, "usrquota"},
	{Opt_err, NULL}
};

static unsigned long get_sb_block(void **data)
{
	unsigned long 	sb_block;
	char 		*options = (char *) *data;

	if (!options || strncmp(options, "sb=", 3) != 0)
		return 1;	/* Default location */
	options += 3;
	sb_block = simple_strtoul(options, &options, 0);
	if (*options && *options != ',') {
		printk("EXT3-fs: Invalid sb specification: %s\n",
		       (char *) *data);
		return 1;
	}
	if (*options == ',')
		options++;
	*data = (void *) options;
	return sb_block;
}

static int parse_options (char * options, struct super_block *sb,
			  unsigned long * inum, int is_remount)
{
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	char * p;
	substring_t args[MAX_OPT_ARGS];
	int data_opt = 0;
	int option;
#ifdef CONFIG_QUOTA
	int qtype;
#endif

	if (!options)
		return 1;

	while ((p = strsep (&options, ",")) != NULL) {
		int token;
		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_bsd_df:
			clear_opt (sbi->s_mount_opt, MINIX_DF);
			break;
		case Opt_minix_df:
			set_opt (sbi->s_mount_opt, MINIX_DF);
			break;
		case Opt_grpid:
			set_opt (sbi->s_mount_opt, GRPID);
			break;
		case Opt_nogrpid:
			clear_opt (sbi->s_mount_opt, GRPID);
			break;
		case Opt_resuid:
			if (match_int(&args[0], &option))
				return 0;
			sbi->s_resuid = option;
			break;
		case Opt_resgid:
			if (match_int(&args[0], &option))
				return 0;
			sbi->s_resgid = option;
			break;
		case Opt_sb:
			/* handled by get_sb_block() instead of here */
			/* *sb_block = match_int(&args[0]); */
			break;
		case Opt_err_panic:
			clear_opt (sbi->s_mount_opt, ERRORS_CONT);
			clear_opt (sbi->s_mount_opt, ERRORS_RO);
			set_opt (sbi->s_mount_opt, ERRORS_PANIC);
			break;
		case Opt_err_ro:
			clear_opt (sbi->s_mount_opt, ERRORS_CONT);
			clear_opt (sbi->s_mount_opt, ERRORS_PANIC);
			set_opt (sbi->s_mount_opt, ERRORS_RO);
			break;
		case Opt_err_cont:
			clear_opt (sbi->s_mount_opt, ERRORS_RO);
			clear_opt (sbi->s_mount_opt, ERRORS_PANIC);
			set_opt (sbi->s_mount_opt, ERRORS_CONT);
			break;
		case Opt_nouid32:
			set_opt (sbi->s_mount_opt, NO_UID32);
			break;
		case Opt_check:
#ifdef CONFIG_EXT3_CHECK
			set_opt (sbi->s_mount_opt, CHECK);
#else
			printk(KERN_ERR
			       "EXT3 Check option not supported\n");
#endif
			break;
		case Opt_nocheck:
			clear_opt (sbi->s_mount_opt, CHECK);
			break;
		case Opt_debug:
			set_opt (sbi->s_mount_opt, DEBUG);
			break;
		case Opt_oldalloc:
			set_opt (sbi->s_mount_opt, OLDALLOC);
			break;
		case Opt_orlov:
			clear_opt (sbi->s_mount_opt, OLDALLOC);
			break;
#ifdef CONFIG_EXT3_FS_XATTR
		case Opt_user_xattr:
			set_opt (sbi->s_mount_opt, XATTR_USER);
			break;
		case Opt_nouser_xattr:
			clear_opt (sbi->s_mount_opt, XATTR_USER);
			break;
#else
		case Opt_user_xattr:
		case Opt_nouser_xattr:
			printk("EXT3 (no)user_xattr options not supported\n");
			break;
#endif
#ifdef CONFIG_EXT3_FS_POSIX_ACL
		case Opt_acl:
			set_opt(sbi->s_mount_opt, POSIX_ACL);
			break;
		case Opt_noacl:
			clear_opt(sbi->s_mount_opt, POSIX_ACL);
			break;
#else
		case Opt_acl:
		case Opt_noacl:
			printk("EXT3 (no)acl options not supported\n");
			break;
#endif
		case Opt_journal_update:
			/* @@@ FIXME */
			/* Eventually we will want to be able to create
			   a journal file here.  For now, only allow the
			   user to specify an existing inode to be the
			   journal file. */
			if (is_remount) {
				printk(KERN_ERR "EXT3-fs: cannot specify "
				       "journal on remount\n");
				return 0;
			}
			set_opt (sbi->s_mount_opt, UPDATE_JOURNAL);
			break;
		case Opt_journal_inum:
			if (is_remount) {
				printk(KERN_ERR "EXT3-fs: cannot specify "
				       "journal on remount\n");
				return 0;
			}
			if (match_int(&args[0], &option))
				return 0;
			*inum = option;
			break;
		case Opt_noload:
			set_opt (sbi->s_mount_opt, NOLOAD);
			break;
		case Opt_commit:
			if (match_int(&args[0], &option))
				return 0;
			if (option < 0)
				return 0;
			if (option == 0)
				option = JBD_DEFAULT_MAX_COMMIT_AGE;
			sbi->s_commit_interval = HZ * option;
			break;
		case Opt_data_journal:
			data_opt = EXT3_MOUNT_JOURNAL_DATA;
			goto datacheck;
		case Opt_data_ordered:
			data_opt = EXT3_MOUNT_ORDERED_DATA;
			goto datacheck;
		case Opt_data_writeback:
			data_opt = EXT3_MOUNT_WRITEBACK_DATA;
		datacheck:
			if (is_remount) {
				if ((sbi->s_mount_opt & EXT3_MOUNT_DATA_FLAGS)
						!= data_opt) {
					printk(KERN_ERR
						"EXT3-fs: cannot change data "
						"mode on remount\n");
					return 0;
				}
			} else {
				sbi->s_mount_opt &= ~EXT3_MOUNT_DATA_FLAGS;
				sbi->s_mount_opt |= data_opt;
			}
			break;
#ifdef CONFIG_QUOTA
		case Opt_usrjquota:
			qtype = USRQUOTA;
			goto set_qf_name;
		case Opt_grpjquota:
			qtype = GRPQUOTA;
set_qf_name:
			if (sb_any_quota_enabled(sb)) {
				printk(KERN_ERR
					"EXT3-fs: Cannot change journalled "
					"quota options when quota turned on.\n");
				return 0;
			}
			if (sbi->s_qf_names[qtype]) {
				printk(KERN_ERR
					"EXT3-fs: %s quota file already "
					"specified.\n", QTYPE2NAME(qtype));
				return 0;
			}
			sbi->s_qf_names[qtype] = match_strdup(&args[0]);
			if (!sbi->s_qf_names[qtype]) {
				printk(KERN_ERR
					"EXT3-fs: not enough memory for "
					"storing quotafile name.\n");
				return 0;
			}
			if (strchr(sbi->s_qf_names[qtype], '/')) {
				printk(KERN_ERR
					"EXT3-fs: quotafile must be on "
					"filesystem root.\n");
				kfree(sbi->s_qf_names[qtype]);
				sbi->s_qf_names[qtype] = NULL;
				return 0;
			}
			break;
		case Opt_offusrjquota:
			qtype = USRQUOTA;
			goto clear_qf_name;
		case Opt_offgrpjquota:
			qtype = GRPQUOTA;
clear_qf_name:
			if (sb_any_quota_enabled(sb)) {
				printk(KERN_ERR "EXT3-fs: Cannot change "
					"journalled quota options when "
					"quota turned on.\n");
				return 0;
			}
			if (sbi->s_qf_names[qtype]) {
				kfree(sbi->s_qf_names[qtype]);
				sbi->s_qf_names[qtype] = NULL;
			}
			break;
		case Opt_jqfmt_vfsold:
			sbi->s_jquota_fmt = QFMT_VFS_OLD;
			break;
		case Opt_jqfmt_vfsv0:
			sbi->s_jquota_fmt = QFMT_VFS_V0;
			break;
#else
		case Opt_usrjquota:
		case Opt_grpjquota:
		case Opt_offusrjquota:
		case Opt_offgrpjquota:
		case Opt_jqfmt_vfsold:
		case Opt_jqfmt_vfsv0:
			printk(KERN_ERR
				"EXT3-fs: journalled quota options not "
				"supported.\n");
			break;
#endif
		case Opt_abort:
			set_opt(sbi->s_mount_opt, ABORT);
			break;
		case Opt_ignore:
			break;
		default:
			printk (KERN_ERR
				"EXT3-fs: Unrecognized mount option \"%s\" "
				"or missing value\n", p);
			return 0;
		}
	}
#ifdef CONFIG_QUOTA
	if (!sbi->s_jquota_fmt && (sbi->s_qf_names[USRQUOTA] ||
	    sbi->s_qf_names[GRPQUOTA])) {
		printk(KERN_ERR
			"EXT3-fs: journalled quota format not specified.\n");
		return 0;
	}
#endif

	return 1;
}

static int ext3_setup_super(struct super_block *sb, struct ext3_super_block *es,
			    int read_only)
{
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	int res = 0;

	if (le32_to_cpu(es->s_rev_level) > EXT3_MAX_SUPP_REV) {
		printk (KERN_ERR "EXT3-fs warning: revision level too high, "
			"forcing read-only mode\n");
		res = MS_RDONLY;
	}
	if (read_only)
		return res;
	if (!(sbi->s_mount_state & EXT3_VALID_FS))
		printk (KERN_WARNING "EXT3-fs warning: mounting unchecked fs, "
			"running e2fsck is recommended\n");
	else if ((sbi->s_mount_state & EXT3_ERROR_FS))
		printk (KERN_WARNING
			"EXT3-fs warning: mounting fs with errors, "
			"running e2fsck is recommended\n");
	else if ((__s16) le16_to_cpu(es->s_max_mnt_count) >= 0 &&
		 le16_to_cpu(es->s_mnt_count) >=
		 (unsigned short) (__s16) le16_to_cpu(es->s_max_mnt_count))
		printk (KERN_WARNING
			"EXT3-fs warning: maximal mount count reached, "
			"running e2fsck is recommended\n");
	else if (le32_to_cpu(es->s_checkinterval) &&
		(le32_to_cpu(es->s_lastcheck) +
			le32_to_cpu(es->s_checkinterval) <= get_seconds()))
		printk (KERN_WARNING
			"EXT3-fs warning: checktime reached, "
			"running e2fsck is recommended\n");
#if 0
		/* @@@ We _will_ want to clear the valid bit if we find
                   inconsistencies, to force a fsck at reboot.  But for
                   a plain journaled filesystem we can keep it set as
                   valid forever! :) */
	es->s_state = cpu_to_le16(le16_to_cpu(es->s_state) & ~EXT3_VALID_FS);
#endif
	if (!(__s16) le16_to_cpu(es->s_max_mnt_count))
		es->s_max_mnt_count =
			(__s16) cpu_to_le16(EXT3_DFL_MAX_MNT_COUNT);
	es->s_mnt_count=cpu_to_le16(le16_to_cpu(es->s_mnt_count) + 1);
	es->s_mtime = cpu_to_le32(get_seconds());
	ext3_update_dynamic_rev(sb);
	EXT3_SET_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_RECOVER);

	ext3_commit_super(sb, es, 1);
	if (test_opt(sb, DEBUG))
		printk(KERN_INFO "[EXT3 FS bs=%lu, gc=%lu, "
				"bpg=%lu, ipg=%lu, mo=%04lx]\n",
			sb->s_blocksize,
			sbi->s_groups_count,
			EXT3_BLOCKS_PER_GROUP(sb),
			EXT3_INODES_PER_GROUP(sb),
			sbi->s_mount_opt);

	printk(KERN_INFO "EXT3 FS on %s, ", sb->s_id);
	if (EXT3_SB(sb)->s_journal->j_inode == NULL) {
		char b[BDEVNAME_SIZE];

		printk("external journal on %s\n",
			bdevname(EXT3_SB(sb)->s_journal->j_dev, b));
	} else {
		printk("internal journal\n");
	}
#ifdef CONFIG_EXT3_CHECK
	if (test_opt (sb, CHECK)) {
		ext3_check_blocks_bitmap (sb);
		ext3_check_inodes_bitmap (sb);
	}
#endif
	return res;
}

static int ext3_check_descriptors (struct super_block * sb)
{
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	unsigned long block = le32_to_cpu(sbi->s_es->s_first_data_block);
	struct ext3_group_desc * gdp = NULL;
	int desc_block = 0;
	int i;

	ext3_debug ("Checking group descriptors");

	for (i = 0; i < sbi->s_groups_count; i++)
	{
		if ((i % EXT3_DESC_PER_BLOCK(sb)) == 0)
			gdp = (struct ext3_group_desc *)
					sbi->s_group_desc[desc_block++]->b_data;
		if (le32_to_cpu(gdp->bg_block_bitmap) < block ||
		    le32_to_cpu(gdp->bg_block_bitmap) >=
				block + EXT3_BLOCKS_PER_GROUP(sb))
		{
			ext3_error (sb, "ext3_check_descriptors",
				    "Block bitmap for group %d"
				    " not in group (block %lu)!",
				    i, (unsigned long)
					le32_to_cpu(gdp->bg_block_bitmap));
			return 0;
		}
		if (le32_to_cpu(gdp->bg_inode_bitmap) < block ||
		    le32_to_cpu(gdp->bg_inode_bitmap) >=
				block + EXT3_BLOCKS_PER_GROUP(sb))
		{
			ext3_error (sb, "ext3_check_descriptors",
				    "Inode bitmap for group %d"
				    " not in group (block %lu)!",
				    i, (unsigned long)
					le32_to_cpu(gdp->bg_inode_bitmap));
			return 0;
		}
		if (le32_to_cpu(gdp->bg_inode_table) < block ||
		    le32_to_cpu(gdp->bg_inode_table) + sbi->s_itb_per_group >=
		    block + EXT3_BLOCKS_PER_GROUP(sb))
		{
			ext3_error (sb, "ext3_check_descriptors",
				    "Inode table for group %d"
				    " not in group (block %lu)!",
				    i, (unsigned long)
					le32_to_cpu(gdp->bg_inode_table));
			return 0;
		}
		block += EXT3_BLOCKS_PER_GROUP(sb);
		gdp++;
	}

	sbi->s_es->s_free_blocks_count=cpu_to_le32(ext3_count_free_blocks(sb));
	sbi->s_es->s_free_inodes_count=cpu_to_le32(ext3_count_free_inodes(sb));
	return 1;
}


/* ext3_orphan_cleanup() walks a singly-linked list of inodes (starting at
 * the superblock) which were deleted from all directories, but held open by
 * a process at the time of a crash.  We walk the list and try to delete these
 * inodes at recovery time (only with a read-write filesystem).
 *
 * In order to keep the orphan inode chain consistent during traversal (in
 * case of crash during recovery), we link each inode into the superblock
 * orphan list_head and handle it the same way as an inode deletion during
 * normal operation (which journals the operations for us).
 *
 * We only do an iget() and an iput() on each inode, which is very safe if we
 * accidentally point at an in-use or already deleted inode.  The worst that
 * can happen in this case is that we get a "bit already cleared" message from
 * ext3_free_inode().  The only reason we would point at a wrong inode is if
 * e2fsck was run on this filesystem, and it must have already done the orphan
 * inode cleanup for us, so we can safely abort without any further action.
 */
static void ext3_orphan_cleanup (struct super_block * sb,
				 struct ext3_super_block * es)
{
	unsigned int s_flags = sb->s_flags;
	int nr_orphans = 0, nr_truncates = 0;
#ifdef CONFIG_QUOTA
	int i;
#endif
	if (!es->s_last_orphan) {
		jbd_debug(4, "no orphan inodes to clean up\n");
		return;
	}

	if (EXT3_SB(sb)->s_mount_state & EXT3_ERROR_FS) {
		if (es->s_last_orphan)
			jbd_debug(1, "Errors on filesystem, "
				  "clearing orphan list.\n");
		es->s_last_orphan = 0;
		jbd_debug(1, "Skipping orphan recovery on fs with errors.\n");
		return;
	}

	if (s_flags & MS_RDONLY) {
		printk(KERN_INFO "EXT3-fs: %s: orphan cleanup on readonly fs\n",
		       sb->s_id);
		sb->s_flags &= ~MS_RDONLY;
	}
#ifdef CONFIG_QUOTA
	/* Needed for iput() to work correctly and not trash data */
	sb->s_flags |= MS_ACTIVE;
	/* Turn on quotas so that they are updated correctly */
	for (i = 0; i < MAXQUOTAS; i++) {
		if (EXT3_SB(sb)->s_qf_names[i]) {
			int ret = ext3_quota_on_mount(sb, i);
			if (ret < 0)
				printk(KERN_ERR
					"EXT3-fs: Cannot turn on journalled "
					"quota: error %d\n", ret);
		}
	}
#endif

	while (es->s_last_orphan) {
		struct inode *inode;

		if (!(inode =
		      ext3_orphan_get(sb, le32_to_cpu(es->s_last_orphan)))) {
			es->s_last_orphan = 0;
			break;
		}

		list_add(&EXT3_I(inode)->i_orphan, &EXT3_SB(sb)->s_orphan);
		DQUOT_INIT(inode);
		if (inode->i_nlink) {
			printk(KERN_DEBUG
				"%s: truncating inode %ld to %Ld bytes\n",
				__FUNCTION__, inode->i_ino, inode->i_size);
			jbd_debug(2, "truncating inode %ld to %Ld bytes\n",
				  inode->i_ino, inode->i_size);
			ext3_truncate(inode);
			nr_truncates++;
		} else {
			printk(KERN_DEBUG
				"%s: deleting unreferenced inode %ld\n",
				__FUNCTION__, inode->i_ino);
			jbd_debug(2, "deleting unreferenced inode %ld\n",
				  inode->i_ino);
			nr_orphans++;
		}
		iput(inode);  /* The delete magic happens here! */
	}

#define PLURAL(x) (x), ((x)==1) ? "" : "s"

	if (nr_orphans)
		printk(KERN_INFO "EXT3-fs: %s: %d orphan inode%s deleted\n",
		       sb->s_id, PLURAL(nr_orphans));
	if (nr_truncates)
		printk(KERN_INFO "EXT3-fs: %s: %d truncate%s cleaned up\n",
		       sb->s_id, PLURAL(nr_truncates));
#ifdef CONFIG_QUOTA
	/* Turn quotas off */
	for (i = 0; i < MAXQUOTAS; i++) {
		if (sb_dqopt(sb)->files[i])
			ext3_quota_off_mount(sb, i);
	}
#endif
	sb->s_flags = s_flags; /* Restore MS_RDONLY status */
}

#define log2(n) ffz(~(n))

/*
 * Maximal file size.  There is a direct, and {,double-,triple-}indirect
 * block limit, and also a limit of (2^32 - 1) 512-byte sectors in i_blocks.
 * We need to be 1 filesystem block less than the 2^32 sector limit.
 */
static loff_t ext3_max_size(int bits)
{
	loff_t res = EXT3_NDIR_BLOCKS;
	res += 1LL << (bits-2);
	res += 1LL << (2*(bits-2));
	res += 1LL << (3*(bits-2));
	res <<= bits;
	if (res > (512LL << 32) - (1 << bits))
		res = (512LL << 32) - (1 << bits);
	return res;
}

static unsigned long descriptor_loc(struct super_block *sb,
				    unsigned long logic_sb_block,
				    int nr)
{
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	unsigned long bg, first_data_block, first_meta_bg;
	int has_super = 0;

	first_data_block = le32_to_cpu(sbi->s_es->s_first_data_block);
	first_meta_bg = le32_to_cpu(sbi->s_es->s_first_meta_bg);

	if (!EXT3_HAS_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_META_BG) ||
	    nr < first_meta_bg)
		return (logic_sb_block + nr + 1);
	bg = sbi->s_desc_per_block * nr;
	if (ext3_bg_has_super(sb, bg))
		has_super = 1;
	return (first_data_block + has_super + (bg * sbi->s_blocks_per_group));
}


static int ext3_fill_super (struct super_block *sb, void *data, int silent)
{
	struct buffer_head * bh;
	struct ext3_super_block *es = NULL;
	struct ext3_sb_info *sbi;
	unsigned long block;
	unsigned long sb_block = get_sb_block(&data);
	unsigned long logic_sb_block;
	unsigned long offset = 0;
	unsigned long journal_inum = 0;
	unsigned long def_mount_opts;
	struct inode *root;
	int blocksize;
	int hblock;
	int db_count;
	int i;
	int needs_recovery;

	sbi = kmalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;
	memset(sbi, 0, sizeof(*sbi));
	sbi->s_mount_opt = 0;
	sbi->s_resuid = EXT3_DEF_RESUID;
	sbi->s_resgid = EXT3_DEF_RESGID;

	blocksize = sb_min_blocksize(sb, EXT3_MIN_BLOCK_SIZE);
	if (!blocksize) {
		printk(KERN_ERR "EXT3-fs: unable to set blocksize\n");
		goto out_fail;
	}

	/*
	 * The ext3 superblock will not be buffer aligned for other than 1kB
	 * block sizes.  We need to calculate the offset from buffer start.
	 */
	if (blocksize != EXT3_MIN_BLOCK_SIZE) {
		logic_sb_block = (sb_block * EXT3_MIN_BLOCK_SIZE) / blocksize;
		offset = (sb_block * EXT3_MIN_BLOCK_SIZE) % blocksize;
	} else {
		logic_sb_block = sb_block;
	}

	if (!(bh = sb_bread(sb, logic_sb_block))) {
		printk (KERN_ERR "EXT3-fs: unable to read superblock\n");
		goto out_fail;
	}
	/*
	 * Note: s_es must be initialized as soon as possible because
	 *       some ext3 macro-instructions depend on its value
	 */
	es = (struct ext3_super_block *) (((char *)bh->b_data) + offset);
	sbi->s_es = es;
	sb->s_magic = le16_to_cpu(es->s_magic);
	if (sb->s_magic != EXT3_SUPER_MAGIC) {
		if (!silent)
			printk(KERN_ERR 
			       "VFS: Can't find ext3 filesystem on dev %s.\n",
			       sb->s_id);
		goto failed_mount;
	}

	/* Set defaults before we parse the mount options */
	def_mount_opts = le32_to_cpu(es->s_default_mount_opts);
	if (def_mount_opts & EXT3_DEFM_DEBUG)
		set_opt(sbi->s_mount_opt, DEBUG);
	if (def_mount_opts & EXT3_DEFM_BSDGROUPS)
		set_opt(sbi->s_mount_opt, GRPID);
	if (def_mount_opts & EXT3_DEFM_UID16)
		set_opt(sbi->s_mount_opt, NO_UID32);
	if (def_mount_opts & EXT3_DEFM_XATTR_USER)
		set_opt(sbi->s_mount_opt, XATTR_USER);
	if (def_mount_opts & EXT3_DEFM_ACL)
		set_opt(sbi->s_mount_opt, POSIX_ACL);
	if ((def_mount_opts & EXT3_DEFM_JMODE) == EXT3_DEFM_JMODE_DATA)
		sbi->s_mount_opt |= EXT3_MOUNT_JOURNAL_DATA;
	else if ((def_mount_opts & EXT3_DEFM_JMODE) == EXT3_DEFM_JMODE_ORDERED)
		sbi->s_mount_opt |= EXT3_MOUNT_ORDERED_DATA;
	else if ((def_mount_opts & EXT3_DEFM_JMODE) == EXT3_DEFM_JMODE_WBACK)
		sbi->s_mount_opt |= EXT3_MOUNT_WRITEBACK_DATA;

	if (le16_to_cpu(sbi->s_es->s_errors) == EXT3_ERRORS_PANIC)
		set_opt(sbi->s_mount_opt, ERRORS_PANIC);
	else if (le16_to_cpu(sbi->s_es->s_errors) == EXT3_ERRORS_RO)
		set_opt(sbi->s_mount_opt, ERRORS_RO);

	sbi->s_resuid = le16_to_cpu(es->s_def_resuid);
	sbi->s_resgid = le16_to_cpu(es->s_def_resgid);

	if (!parse_options ((char *) data, sb, &journal_inum, 0))
		goto failed_mount;

	sb->s_flags |= MS_ONE_SECOND;
	sb->s_flags = (sb->s_flags & ~MS_POSIXACL) |
		((sbi->s_mount_opt & EXT3_MOUNT_POSIX_ACL) ? MS_POSIXACL : 0);

	if (le32_to_cpu(es->s_rev_level) == EXT3_GOOD_OLD_REV &&
	    (EXT3_HAS_COMPAT_FEATURE(sb, ~0U) ||
	     EXT3_HAS_RO_COMPAT_FEATURE(sb, ~0U) ||
	     EXT3_HAS_INCOMPAT_FEATURE(sb, ~0U)))
		printk(KERN_WARNING 
		       "EXT3-fs warning: feature flags set on rev 0 fs, "
		       "running e2fsck is recommended\n");
	/*
	 * Check feature flags regardless of the revision level, since we
	 * previously didn't change the revision level when setting the flags,
	 * so there is a chance incompat flags are set on a rev 0 filesystem.
	 */
	if ((i = EXT3_HAS_INCOMPAT_FEATURE(sb, ~EXT3_FEATURE_INCOMPAT_SUPP))) {
		printk(KERN_ERR "EXT3-fs: %s: couldn't mount because of "
		       "unsupported optional features (%x).\n",
		       sb->s_id, i);
		goto failed_mount;
	}
	if (!(sb->s_flags & MS_RDONLY) &&
	    (i = EXT3_HAS_RO_COMPAT_FEATURE(sb, ~EXT3_FEATURE_RO_COMPAT_SUPP))){
		printk(KERN_ERR "EXT3-fs: %s: couldn't mount RDWR because of "
		       "unsupported optional features (%x).\n",
		       sb->s_id, i);
		goto failed_mount;
	}
	blocksize = BLOCK_SIZE << le32_to_cpu(es->s_log_block_size);

	if (blocksize < EXT3_MIN_BLOCK_SIZE ||
	    blocksize > EXT3_MAX_BLOCK_SIZE) {
		printk(KERN_ERR 
		       "EXT3-fs: Unsupported filesystem blocksize %d on %s.\n",
		       blocksize, sb->s_id);
		goto failed_mount;
	}

	hblock = bdev_hardsect_size(sb->s_bdev);
	if (sb->s_blocksize != blocksize) {
		/*
		 * Make sure the blocksize for the filesystem is larger
		 * than the hardware sectorsize for the machine.
		 */
		if (blocksize < hblock) {
			printk(KERN_ERR "EXT3-fs: blocksize %d too small for "
			       "device blocksize %d.\n", blocksize, hblock);
			goto failed_mount;
		}

		brelse (bh);
		sb_set_blocksize(sb, blocksize);
		logic_sb_block = (sb_block * EXT3_MIN_BLOCK_SIZE) / blocksize;
		offset = (sb_block * EXT3_MIN_BLOCK_SIZE) % blocksize;
		bh = sb_bread(sb, logic_sb_block);
		if (!bh) {
			printk(KERN_ERR 
			       "EXT3-fs: Can't read superblock on 2nd try.\n");
			goto failed_mount;
		}
		es = (struct ext3_super_block *)(((char *)bh->b_data) + offset);
		sbi->s_es = es;
		if (es->s_magic != le16_to_cpu(EXT3_SUPER_MAGIC)) {
			printk (KERN_ERR 
				"EXT3-fs: Magic mismatch, very weird !\n");
			goto failed_mount;
		}
	}

	sb->s_maxbytes = ext3_max_size(sb->s_blocksize_bits);

	if (le32_to_cpu(es->s_rev_level) == EXT3_GOOD_OLD_REV) {
		sbi->s_inode_size = EXT3_GOOD_OLD_INODE_SIZE;
		sbi->s_first_ino = EXT3_GOOD_OLD_FIRST_INO;
	} else {
		sbi->s_inode_size = le16_to_cpu(es->s_inode_size);
		sbi->s_first_ino = le32_to_cpu(es->s_first_ino);
		if ((sbi->s_inode_size < EXT3_GOOD_OLD_INODE_SIZE) ||
		    (sbi->s_inode_size & (sbi->s_inode_size - 1)) ||
		    (sbi->s_inode_size > blocksize)) {
			printk (KERN_ERR
				"EXT3-fs: unsupported inode size: %d\n",
				sbi->s_inode_size);
			goto failed_mount;
		}
	}
	sbi->s_frag_size = EXT3_MIN_FRAG_SIZE <<
				   le32_to_cpu(es->s_log_frag_size);
	if (blocksize != sbi->s_frag_size) {
		printk(KERN_ERR
		       "EXT3-fs: fragsize %lu != blocksize %u (unsupported)\n",
		       sbi->s_frag_size, blocksize);
		goto failed_mount;
	}
	sbi->s_frags_per_block = 1;
	sbi->s_blocks_per_group = le32_to_cpu(es->s_blocks_per_group);
	sbi->s_frags_per_group = le32_to_cpu(es->s_frags_per_group);
	sbi->s_inodes_per_group = le32_to_cpu(es->s_inodes_per_group);
	sbi->s_inodes_per_block = blocksize / EXT3_INODE_SIZE(sb);
	sbi->s_itb_per_group = sbi->s_inodes_per_group /sbi->s_inodes_per_block;
	sbi->s_desc_per_block = blocksize / sizeof(struct ext3_group_desc);
	sbi->s_sbh = bh;
	sbi->s_mount_state = le16_to_cpu(es->s_state);
	sbi->s_addr_per_block_bits = log2(EXT3_ADDR_PER_BLOCK(sb));
	sbi->s_desc_per_block_bits = log2(EXT3_DESC_PER_BLOCK(sb));
	for (i=0; i < 4; i++)
		sbi->s_hash_seed[i] = le32_to_cpu(es->s_hash_seed[i]);
	sbi->s_def_hash_version = es->s_def_hash_version;

	if (sbi->s_blocks_per_group > blocksize * 8) {
		printk (KERN_ERR
			"EXT3-fs: #blocks per group too big: %lu\n",
			sbi->s_blocks_per_group);
		goto failed_mount;
	}
	if (sbi->s_frags_per_group > blocksize * 8) {
		printk (KERN_ERR
			"EXT3-fs: #fragments per group too big: %lu\n",
			sbi->s_frags_per_group);
		goto failed_mount;
	}
	if (sbi->s_inodes_per_group > blocksize * 8) {
		printk (KERN_ERR
			"EXT3-fs: #inodes per group too big: %lu\n",
			sbi->s_inodes_per_group);
		goto failed_mount;
	}

	sbi->s_groups_count = (le32_to_cpu(es->s_blocks_count) -
			       le32_to_cpu(es->s_first_data_block) +
			       EXT3_BLOCKS_PER_GROUP(sb) - 1) /
			      EXT3_BLOCKS_PER_GROUP(sb);
	db_count = (sbi->s_groups_count + EXT3_DESC_PER_BLOCK(sb) - 1) /
		   EXT3_DESC_PER_BLOCK(sb);
	sbi->s_group_desc = kmalloc(db_count * sizeof (struct buffer_head *),
				    GFP_KERNEL);
	if (sbi->s_group_desc == NULL) {
		printk (KERN_ERR "EXT3-fs: not enough memory\n");
		goto failed_mount;
	}
	sbi->s_debts = kmalloc(sbi->s_groups_count * sizeof(u8),
			GFP_KERNEL);
	if (!sbi->s_debts) {
		printk("EXT3-fs: not enough memory to allocate s_bgi\n");
		goto failed_mount2;
	}
	memset(sbi->s_debts, 0,  sbi->s_groups_count * sizeof(u8));

	percpu_counter_init(&sbi->s_freeblocks_counter);
	percpu_counter_init(&sbi->s_freeinodes_counter);
	percpu_counter_init(&sbi->s_dirs_counter);
	bgl_lock_init(&sbi->s_blockgroup_lock);

	for (i = 0; i < db_count; i++) {
		block = descriptor_loc(sb, logic_sb_block, i);
		sbi->s_group_desc[i] = sb_bread(sb, block);
		if (!sbi->s_group_desc[i]) {
			printk (KERN_ERR "EXT3-fs: "
				"can't read group descriptor %d\n", i);
			db_count = i;
			goto failed_mount2;
		}
	}
	if (!ext3_check_descriptors (sb)) {
		printk (KERN_ERR "EXT3-fs: group descriptors corrupted !\n");
		goto failed_mount2;
	}
	sbi->s_gdb_count = db_count;
	get_random_bytes(&sbi->s_next_generation, sizeof(u32));
	spin_lock_init(&sbi->s_next_gen_lock);
	/*
	 * set up enough so that it can read an inode
	 */
	sb->s_op = &ext3_sops;
	sb->s_export_op = &ext3_export_ops;
#ifdef CONFIG_QUOTA
	sb->s_qcop = &ext3_qctl_operations;
	sb->dq_op = &ext3_quota_operations;
#endif
	INIT_LIST_HEAD(&sbi->s_orphan); /* unlinked but open files */

	sb->s_root = NULL;

	needs_recovery = (es->s_last_orphan != 0 ||
			  EXT3_HAS_INCOMPAT_FEATURE(sb,
				    EXT3_FEATURE_INCOMPAT_RECOVER));

	/*
	 * The first inode we look at is the journal inode.  Don't try
	 * root first: it may be modified in the journal!
	 */
	if (!test_opt(sb, NOLOAD) &&
	    EXT3_HAS_COMPAT_FEATURE(sb, EXT3_FEATURE_COMPAT_HAS_JOURNAL)) {
		if (ext3_load_journal(sb, es))
			goto failed_mount2;
	} else if (journal_inum) {
		if (ext3_create_journal(sb, es, journal_inum))
			goto failed_mount2;
	} else {
		if (!silent)
			printk (KERN_ERR
				"ext3: No journal on filesystem on %s\n",
				sb->s_id);
		goto failed_mount2;
	}

	/* We have now updated the journal if required, so we can
	 * validate the data journaling mode. */
	switch (test_opt(sb, DATA_FLAGS)) {
	case 0:
		/* No mode set, assume a default based on the journal
                   capabilities: ORDERED_DATA if the journal can
                   cope, else JOURNAL_DATA */
		if (journal_check_available_features
		    (sbi->s_journal, 0, 0, JFS_FEATURE_INCOMPAT_REVOKE))
			set_opt(sbi->s_mount_opt, ORDERED_DATA);
		else
			set_opt(sbi->s_mount_opt, JOURNAL_DATA);
		break;

	case EXT3_MOUNT_ORDERED_DATA:
	case EXT3_MOUNT_WRITEBACK_DATA:
		if (!journal_check_available_features
		    (sbi->s_journal, 0, 0, JFS_FEATURE_INCOMPAT_REVOKE)) {
			printk(KERN_ERR "EXT3-fs: Journal does not support "
			       "requested data journaling mode\n");
			goto failed_mount3;
		}
	default:
		break;
	}

	/*
	 * The journal_load will have done any necessary log recovery,
	 * so we can safely mount the rest of the filesystem now.
	 */

	root = iget(sb, EXT3_ROOT_INO);
	sb->s_root = d_alloc_root(root);
	if (!sb->s_root) {
		printk(KERN_ERR "EXT3-fs: get root inode failed\n");
		iput(root);
		goto failed_mount3;
	}
	if (!S_ISDIR(root->i_mode) || !root->i_blocks || !root->i_size) {
		dput(sb->s_root);
		sb->s_root = NULL;
		printk(KERN_ERR "EXT3-fs: corrupt root inode, run e2fsck\n");
		goto failed_mount3;
	}

	ext3_setup_super (sb, es, sb->s_flags & MS_RDONLY);
	/*
	 * akpm: core read_super() calls in here with the superblock locked.
	 * That deadlocks, because orphan cleanup needs to lock the superblock
	 * in numerous places.  Here we just pop the lock - it's relatively
	 * harmless, because we are now ready to accept write_super() requests,
	 * and aviro says that's the only reason for hanging onto the
	 * superblock lock.
	 */
	EXT3_SB(sb)->s_mount_state |= EXT3_ORPHAN_FS;
	ext3_orphan_cleanup(sb, es);
	EXT3_SB(sb)->s_mount_state &= ~EXT3_ORPHAN_FS;
	if (needs_recovery)
		printk (KERN_INFO "EXT3-fs: recovery complete.\n");
	ext3_mark_recovery_complete(sb, es);
	printk (KERN_INFO "EXT3-fs: mounted filesystem with %s data mode.\n",
		test_opt(sb,DATA_FLAGS) == EXT3_MOUNT_JOURNAL_DATA ? "journal":
		test_opt(sb,DATA_FLAGS) == EXT3_MOUNT_ORDERED_DATA ? "ordered":
		"writeback");

	percpu_counter_mod(&sbi->s_freeblocks_counter,
		ext3_count_free_blocks(sb));
	percpu_counter_mod(&sbi->s_freeinodes_counter,
		ext3_count_free_inodes(sb));
	percpu_counter_mod(&sbi->s_dirs_counter,
		ext3_count_dirs(sb));

	return 0;

failed_mount3:
	journal_destroy(sbi->s_journal);
failed_mount2:
	kfree(sbi->s_debts);
	for (i = 0; i < db_count; i++)
		brelse(sbi->s_group_desc[i]);
	kfree(sbi->s_group_desc);
failed_mount:
#ifdef CONFIG_QUOTA
	for (i = 0; i < MAXQUOTAS; i++) {
		if (sbi->s_qf_names[i])
			kfree(sbi->s_qf_names[i]);
	}
#endif
	ext3_blkdev_remove(sbi);
	brelse(bh);
out_fail:
	sb->s_fs_info = NULL;
	kfree(sbi);
	return -EINVAL;
}

/*
 * Setup any per-fs journal parameters now.  We'll do this both on
 * initial mount, once the journal has been initialised but before we've
 * done any recovery; and again on any subsequent remount. 
 */
static void ext3_init_journal_params(struct ext3_sb_info *sbi, 
				     journal_t *journal)
{
	if (sbi->s_commit_interval)
		journal->j_commit_interval = sbi->s_commit_interval;
	/* We could also set up an ext3-specific default for the commit
	 * interval here, but for now we'll just fall back to the jbd
	 * default. */
}


static journal_t *ext3_get_journal(struct super_block *sb, int journal_inum)
{
	struct inode *journal_inode;
	journal_t *journal;

	/* First, test for the existence of a valid inode on disk.  Bad
	 * things happen if we iget() an unused inode, as the subsequent
	 * iput() will try to delete it. */

	journal_inode = iget(sb, journal_inum);
	if (!journal_inode) {
		printk(KERN_ERR "EXT3-fs: no journal found.\n");
		return NULL;
	}
	if (!journal_inode->i_nlink) {
		make_bad_inode(journal_inode);
		iput(journal_inode);
		printk(KERN_ERR "EXT3-fs: journal inode is deleted.\n");
		return NULL;
	}

	jbd_debug(2, "Journal inode found at %p: %Ld bytes\n",
		  journal_inode, journal_inode->i_size);
	if (is_bad_inode(journal_inode) || !S_ISREG(journal_inode->i_mode)) {
		printk(KERN_ERR "EXT3-fs: invalid journal inode.\n");
		iput(journal_inode);
		return NULL;
	}

	journal = journal_init_inode(journal_inode);
	if (!journal) {
		printk(KERN_ERR "EXT3-fs: Could not load journal inode\n");
		iput(journal_inode);
		return NULL;
	}
	journal->j_private = sb;
	ext3_init_journal_params(EXT3_SB(sb), journal);
	return journal;
}

static journal_t *ext3_get_dev_journal(struct super_block *sb,
				       dev_t j_dev)
{
	struct buffer_head * bh;
	journal_t *journal;
	int start;
	int len;
	int hblock, blocksize;
	unsigned long sb_block;
	unsigned long offset;
	struct ext3_super_block * es;
	struct block_device *bdev;

	bdev = ext3_blkdev_get(j_dev);
	if (bdev == NULL)
		return NULL;

	if (bd_claim(bdev, sb)) {
		printk(KERN_ERR
		        "EXT3: failed to claim external journal device.\n");
		blkdev_put(bdev);
		return NULL;
	}

	blocksize = sb->s_blocksize;
	hblock = bdev_hardsect_size(bdev);
	if (blocksize < hblock) {
		printk(KERN_ERR
			"EXT3-fs: blocksize too small for journal device.\n");
		goto out_bdev;
	}

	sb_block = EXT3_MIN_BLOCK_SIZE / blocksize;
	offset = EXT3_MIN_BLOCK_SIZE % blocksize;
	set_blocksize(bdev, blocksize);
	if (!(bh = __bread(bdev, sb_block, blocksize))) {
		printk(KERN_ERR "EXT3-fs: couldn't read superblock of "
		       "external journal\n");
		goto out_bdev;
	}

	es = (struct ext3_super_block *) (((char *)bh->b_data) + offset);
	if ((le16_to_cpu(es->s_magic) != EXT3_SUPER_MAGIC) ||
	    !(le32_to_cpu(es->s_feature_incompat) &
	      EXT3_FEATURE_INCOMPAT_JOURNAL_DEV)) {
		printk(KERN_ERR "EXT3-fs: external journal has "
					"bad superblock\n");
		brelse(bh);
		goto out_bdev;
	}

	if (memcmp(EXT3_SB(sb)->s_es->s_journal_uuid, es->s_uuid, 16)) {
		printk(KERN_ERR "EXT3-fs: journal UUID does not match\n");
		brelse(bh);
		goto out_bdev;
	}

	len = le32_to_cpu(es->s_blocks_count);
	start = sb_block + 1;
	brelse(bh);	/* we're done with the superblock */

	journal = journal_init_dev(bdev, sb->s_bdev,
					start, len, blocksize);
	if (!journal) {
		printk(KERN_ERR "EXT3-fs: failed to create device journal\n");
		goto out_bdev;
	}
	journal->j_private = sb;
	ll_rw_block(READ, 1, &journal->j_sb_buffer);
	wait_on_buffer(journal->j_sb_buffer);
	if (!buffer_uptodate(journal->j_sb_buffer)) {
		printk(KERN_ERR "EXT3-fs: I/O error on journal device\n");
		goto out_journal;
	}
	if (ntohl(journal->j_superblock->s_nr_users) != 1) {
		printk(KERN_ERR "EXT3-fs: External journal has more than one "
					"user (unsupported) - %d\n",
			ntohl(journal->j_superblock->s_nr_users));
		goto out_journal;
	}
	EXT3_SB(sb)->journal_bdev = bdev;
	ext3_init_journal_params(EXT3_SB(sb), journal);
	return journal;
out_journal:
	journal_destroy(journal);
out_bdev:
	ext3_blkdev_put(bdev);
	return NULL;
}

static int ext3_load_journal(struct super_block * sb,
			     struct ext3_super_block * es)
{
	journal_t *journal;
	int journal_inum = le32_to_cpu(es->s_journal_inum);
	dev_t journal_dev = new_decode_dev(le32_to_cpu(es->s_journal_dev));
	int err = 0;
	int really_read_only;

	really_read_only = bdev_read_only(sb->s_bdev);

	/*
	 * Are we loading a blank journal or performing recovery after a
	 * crash?  For recovery, we need to check in advance whether we
	 * can get read-write access to the device.
	 */

	if (EXT3_HAS_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_RECOVER)) {
		if (sb->s_flags & MS_RDONLY) {
			printk(KERN_INFO "EXT3-fs: INFO: recovery "
					"required on readonly filesystem.\n");
			if (really_read_only) {
				printk(KERN_ERR "EXT3-fs: write access "
					"unavailable, cannot proceed.\n");
				return -EROFS;
			}
			printk (KERN_INFO "EXT3-fs: write access will "
					"be enabled during recovery.\n");
		}
	}

	if (journal_inum && journal_dev) {
		printk(KERN_ERR "EXT3-fs: filesystem has both journal "
		       "and inode journals!\n");
		return -EINVAL;
	}

	if (journal_inum) {
		if (!(journal = ext3_get_journal(sb, journal_inum)))
			return -EINVAL;
	} else {
		if (!(journal = ext3_get_dev_journal(sb, journal_dev)))
			return -EINVAL;
	}

	if (!really_read_only && test_opt(sb, UPDATE_JOURNAL)) {
		err = journal_update_format(journal);
		if (err)  {
			printk(KERN_ERR "EXT3-fs: error updating journal.\n");
			journal_destroy(journal);
			return err;
		}
	}

	if (!EXT3_HAS_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_RECOVER))
		err = journal_wipe(journal, !really_read_only);
	if (!err)
		err = journal_load(journal);

	if (err) {
		printk(KERN_ERR "EXT3-fs: error loading journal.\n");
		journal_destroy(journal);
		return err;
	}

	EXT3_SB(sb)->s_journal = journal;
	ext3_clear_journal_err(sb, es);
	return 0;
}

static int ext3_create_journal(struct super_block * sb,
			       struct ext3_super_block * es,
			       int journal_inum)
{
	journal_t *journal;

	if (sb->s_flags & MS_RDONLY) {
		printk(KERN_ERR "EXT3-fs: readonly filesystem when trying to "
				"create journal.\n");
		return -EROFS;
	}

	if (!(journal = ext3_get_journal(sb, journal_inum)))
		return -EINVAL;

	printk(KERN_INFO "EXT3-fs: creating new journal on inode %d\n",
	       journal_inum);

	if (journal_create(journal)) {
		printk(KERN_ERR "EXT3-fs: error creating journal.\n");
		journal_destroy(journal);
		return -EIO;
	}

	EXT3_SB(sb)->s_journal = journal;

	ext3_update_dynamic_rev(sb);
	EXT3_SET_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_RECOVER);
	EXT3_SET_COMPAT_FEATURE(sb, EXT3_FEATURE_COMPAT_HAS_JOURNAL);

	es->s_journal_inum = cpu_to_le32(journal_inum);
	sb->s_dirt = 1;

	/* Make sure we flush the recovery flag to disk. */
	ext3_commit_super(sb, es, 1);

	return 0;
}

static void ext3_commit_super (struct super_block * sb,
			       struct ext3_super_block * es,
			       int sync)
{
	struct buffer_head *sbh = EXT3_SB(sb)->s_sbh;

	if (!sbh)
		return;
	es->s_wtime = cpu_to_le32(get_seconds());
	es->s_free_blocks_count = cpu_to_le32(ext3_count_free_blocks(sb));
	es->s_free_inodes_count = cpu_to_le32(ext3_count_free_inodes(sb));
	BUFFER_TRACE(sbh, "marking dirty");
	mark_buffer_dirty(sbh);
	if (sync)
		sync_dirty_buffer(sbh);
}


/*
 * Have we just finished recovery?  If so, and if we are mounting (or
 * remounting) the filesystem readonly, then we will end up with a
 * consistent fs on disk.  Record that fact.
 */
static void ext3_mark_recovery_complete(struct super_block * sb,
					struct ext3_super_block * es)
{
	journal_t *journal = EXT3_SB(sb)->s_journal;

	journal_lock_updates(journal);
	journal_flush(journal);
	if (EXT3_HAS_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_RECOVER) &&
	    sb->s_flags & MS_RDONLY) {
		EXT3_CLEAR_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_RECOVER);
		sb->s_dirt = 0;
		ext3_commit_super(sb, es, 1);
	}
	journal_unlock_updates(journal);
}

/*
 * If we are mounting (or read-write remounting) a filesystem whose journal
 * has recorded an error from a previous lifetime, move that error to the
 * main filesystem now.
 */
static void ext3_clear_journal_err(struct super_block * sb,
				   struct ext3_super_block * es)
{
	journal_t *journal;
	int j_errno;
	const char *errstr;

	journal = EXT3_SB(sb)->s_journal;

	/*
	 * Now check for any error status which may have been recorded in the
	 * journal by a prior ext3_error() or ext3_abort()
	 */

	j_errno = journal_errno(journal);
	if (j_errno) {
		char nbuf[16];

		errstr = ext3_decode_error(sb, j_errno, nbuf);
		ext3_warning(sb, __FUNCTION__, "Filesystem error recorded "
			     "from previous mount: %s", errstr);
		ext3_warning(sb, __FUNCTION__, "Marking fs in need of "
			     "filesystem check.");

		EXT3_SB(sb)->s_mount_state |= EXT3_ERROR_FS;
		es->s_state |= cpu_to_le16(EXT3_ERROR_FS);
		ext3_commit_super (sb, es, 1);

		journal_clear_err(journal);
	}
}

/*
 * Force the running and committing transactions to commit,
 * and wait on the commit.
 */
int ext3_force_commit(struct super_block *sb)
{
	journal_t *journal;
	int ret;

	if (sb->s_flags & MS_RDONLY)
		return 0;

	journal = EXT3_SB(sb)->s_journal;
	sb->s_dirt = 0;
	ret = ext3_journal_force_commit(journal);
	return ret;
}

/*
 * Ext3 always journals updates to the superblock itself, so we don't
 * have to propagate any other updates to the superblock on disk at this
 * point.  Just start an async writeback to get the buffers on their way
 * to the disk.
 *
 * This implicitly triggers the writebehind on sync().
 */

void ext3_write_super (struct super_block * sb)
{
	if (down_trylock(&sb->s_lock) == 0)
		BUG();
	sb->s_dirt = 0;
}

static int ext3_sync_fs(struct super_block *sb, int wait)
{
	tid_t target;

	sb->s_dirt = 0;
	if (journal_start_commit(EXT3_SB(sb)->s_journal, &target)) {
		if (wait)
			log_wait_commit(EXT3_SB(sb)->s_journal, target);
	}
	return 0;
}

/*
 * LVM calls this function before a (read-only) snapshot is created.  This
 * gives us a chance to flush the journal completely and mark the fs clean.
 */
void ext3_write_super_lockfs(struct super_block *sb)
{
	sb->s_dirt = 0;

	if (!(sb->s_flags & MS_RDONLY)) {
		journal_t *journal = EXT3_SB(sb)->s_journal;

		/* Now we set up the journal barrier. */
		journal_lock_updates(journal);
		journal_flush(journal);

		/* Journal blocked and flushed, clear needs_recovery flag. */
		EXT3_CLEAR_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_RECOVER);
		ext3_commit_super(sb, EXT3_SB(sb)->s_es, 1);
	}
}

/*
 * Called by LVM after the snapshot is done.  We need to reset the RECOVER
 * flag here, even though the filesystem is not technically dirty yet.
 */
void ext3_unlockfs(struct super_block *sb)
{
	if (!(sb->s_flags & MS_RDONLY)) {
		lock_super(sb);
		/* Reser the needs_recovery flag before the fs is unlocked. */
		EXT3_SET_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_RECOVER);
		ext3_commit_super(sb, EXT3_SB(sb)->s_es, 1);
		unlock_super(sb);
		journal_unlock_updates(EXT3_SB(sb)->s_journal);
	}
}

int ext3_remount (struct super_block * sb, int * flags, char * data)
{
	struct ext3_super_block * es;
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	unsigned long tmp;

	/*
	 * Allow the "check" option to be passed as a remount option.
	 */
	if (!parse_options(data, sb, &tmp, 1))
		return -EINVAL;

	if (sbi->s_mount_opt & EXT3_MOUNT_ABORT)
		ext3_abort(sb, __FUNCTION__, "Abort forced by user");

	sb->s_flags = (sb->s_flags & ~MS_POSIXACL) |
		((sbi->s_mount_opt & EXT3_MOUNT_POSIX_ACL) ? MS_POSIXACL : 0);

	es = sbi->s_es;

	ext3_init_journal_params(sbi, sbi->s_journal);

	if ((*flags & MS_RDONLY) != (sb->s_flags & MS_RDONLY)) {
		if (sbi->s_mount_opt & EXT3_MOUNT_ABORT)
			return -EROFS;

		if (*flags & MS_RDONLY) {
			/*
			 * First of all, the unconditional stuff we have to do
			 * to disable replay of the journal when we next remount
			 */
			sb->s_flags |= MS_RDONLY;

			/*
			 * OK, test if we are remounting a valid rw partition
			 * readonly, and if so set the rdonly flag and then
			 * mark the partition as valid again.
			 */
			if (!(es->s_state & cpu_to_le16(EXT3_VALID_FS)) &&
			    (sbi->s_mount_state & EXT3_VALID_FS))
				es->s_state = cpu_to_le16(sbi->s_mount_state);

			ext3_mark_recovery_complete(sb, es);
		} else {
			int ret;
			if ((ret = EXT3_HAS_RO_COMPAT_FEATURE(sb,
					~EXT3_FEATURE_RO_COMPAT_SUPP))) {
				printk(KERN_WARNING "EXT3-fs: %s: couldn't "
				       "remount RDWR because of unsupported "
				       "optional features (%x).\n",
				       sb->s_id, ret);
				return -EROFS;
			}
			/*
			 * Mounting a RDONLY partition read-write, so reread
			 * and store the current valid flag.  (It may have
			 * been changed by e2fsck since we originally mounted
			 * the partition.)
			 */
			ext3_clear_journal_err(sb, es);
			sbi->s_mount_state = le16_to_cpu(es->s_state);
			if (!ext3_setup_super (sb, es, 0))
				sb->s_flags &= ~MS_RDONLY;
		}
	}
	return 0;
}

int ext3_statfs (struct super_block * sb, struct kstatfs * buf)
{
	struct ext3_super_block *es = EXT3_SB(sb)->s_es;
	unsigned long overhead;
	int i;

	if (test_opt (sb, MINIX_DF))
		overhead = 0;
	else {
		/*
		 * Compute the overhead (FS structures)
		 */

		/*
		 * All of the blocks before first_data_block are
		 * overhead
		 */
		overhead = le32_to_cpu(es->s_first_data_block);

		/*
		 * Add the overhead attributed to the superblock and
		 * block group descriptors.  If the sparse superblocks
		 * feature is turned on, then not all groups have this.
		 */
		for (i = 0; i < EXT3_SB(sb)->s_groups_count; i++)
			overhead += ext3_bg_has_super(sb, i) +
				ext3_bg_num_gdb(sb, i);

		/*
		 * Every block group has an inode bitmap, a block
		 * bitmap, and an inode table.
		 */
		overhead += (EXT3_SB(sb)->s_groups_count *
			     (2 + EXT3_SB(sb)->s_itb_per_group));
	}

	buf->f_type = EXT3_SUPER_MAGIC;
	buf->f_bsize = sb->s_blocksize;
	buf->f_blocks = le32_to_cpu(es->s_blocks_count) - overhead;
	buf->f_bfree = ext3_count_free_blocks (sb);
	buf->f_bavail = buf->f_bfree - le32_to_cpu(es->s_r_blocks_count);
	if (buf->f_bfree < le32_to_cpu(es->s_r_blocks_count))
		buf->f_bavail = 0;
	buf->f_files = le32_to_cpu(es->s_inodes_count);
	buf->f_ffree = ext3_count_free_inodes (sb);
	buf->f_namelen = EXT3_NAME_LEN;
	return 0;
}

/* Helper function for writing quotas on sync - we need to start transaction before quota file
 * is locked for write. Otherwise the are possible deadlocks:
 * Process 1                         Process 2
 * ext3_create()                     quota_sync()
 *   journal_start()                   write_dquot()
 *   DQUOT_INIT()                        down(dqio_sem)
 *     down(dqio_sem)                    journal_start()
 *
 */

#ifdef CONFIG_QUOTA

static inline struct inode *dquot_to_inode(struct dquot *dquot)
{
	return sb_dqopt(dquot->dq_sb)->files[dquot->dq_type]->f_dentry->d_inode;
}

static int ext3_dquot_initialize(struct inode *inode, int type)
{
	handle_t *handle;
	int ret, err;

	/* We may create quota structure so we need to reserve enough blocks */
	handle = ext3_journal_start(inode, 2*EXT3_QUOTA_INIT_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	ret = dquot_initialize(inode, type);
	err = ext3_journal_stop(handle);
	if (!ret)
		ret = err;
	return ret;
}

static int ext3_dquot_drop(struct inode *inode)
{
	handle_t *handle;
	int ret, err;

	/* We may delete quota structure so we need to reserve enough blocks */
	handle = ext3_journal_start(inode, 2*EXT3_QUOTA_INIT_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	ret = dquot_drop(inode);
	err = ext3_journal_stop(handle);
	if (!ret)
		ret = err;
	return ret;
}

static int ext3_write_dquot(struct dquot *dquot)
{
	int ret, err;
	handle_t *handle;

	handle = ext3_journal_start(dquot_to_inode(dquot),
					EXT3_QUOTA_TRANS_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	ret = dquot_commit(dquot);
	err = ext3_journal_stop(handle);
	if (!ret)
		ret = err;
	return ret;
}

static int ext3_acquire_dquot(struct dquot *dquot)
{
	int ret, err;
	handle_t *handle;

	handle = ext3_journal_start(dquot_to_inode(dquot),
					EXT3_QUOTA_INIT_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	ret = dquot_acquire(dquot);
	err = ext3_journal_stop(handle);
	if (!ret)
		ret = err;
	return ret;
}

static int ext3_release_dquot(struct dquot *dquot)
{
	int ret, err;
	handle_t *handle;

	handle = ext3_journal_start(dquot_to_inode(dquot),
					EXT3_QUOTA_INIT_BLOCKS);
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	ret = dquot_release(dquot);
	err = ext3_journal_stop(handle);
	if (!ret)
		ret = err;
	return ret;
}

static int ext3_mark_dquot_dirty(struct dquot *dquot)
{
	/* Are we journalling quotas? */
	if (EXT3_SB(dquot->dq_sb)->s_qf_names[USRQUOTA] ||
	    EXT3_SB(dquot->dq_sb)->s_qf_names[GRPQUOTA]) {
		dquot_mark_dquot_dirty(dquot);
		return ext3_write_dquot(dquot);
	} else {
		return dquot_mark_dquot_dirty(dquot);
	}
}

static int ext3_write_info(struct super_block *sb, int type)
{
	int ret, err;
	handle_t *handle;

	/* Data block + inode block */
	handle = ext3_journal_start(sb->s_root->d_inode, 2);
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	ret = dquot_commit_info(sb, type);
	err = ext3_journal_stop(handle);
	if (!ret)
		ret = err;
	return ret;
}

/*
 * Turn on quotas during mount time - we need to find
 * the quota file and such...
 */
static int ext3_quota_on_mount(struct super_block *sb, int type)
{
	int err;
	struct dentry *dentry;
	struct qstr name = { .name = EXT3_SB(sb)->s_qf_names[type],
			     .hash = 0,
			     .len = strlen(EXT3_SB(sb)->s_qf_names[type])};

	dentry = lookup_hash(&name, sb->s_root);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);
	err = vfs_quota_on_mount(type, EXT3_SB(sb)->s_jquota_fmt, dentry);
	if (err)
		dput(dentry);
	/* We keep the dentry reference if everything went ok - we drop it
	 * on quota_off time */
	return err;
}

/* Turn quotas off during mount time */
static int ext3_quota_off_mount(struct super_block *sb, int type)
{
	int err;
	struct dentry *dentry;

	dentry = sb_dqopt(sb)->files[type]->f_dentry;
	err = vfs_quota_off_mount(sb, type);
	/* We invalidate dentry - it has at least wrong hash... */
	d_invalidate(dentry);
	dput(dentry);
	return err;
}

/*
 * Standard function to be called on quota_on
 */
static int ext3_quota_on(struct super_block *sb, int type, int format_id,
			 char *path)
{
	int err;
	struct nameidata nd;

	/* Not journalling quota? */
	if (!EXT3_SB(sb)->s_qf_names[USRQUOTA] &&
	    !EXT3_SB(sb)->s_qf_names[GRPQUOTA])
		return vfs_quota_on(sb, type, format_id, path);
	err = path_lookup(path, LOOKUP_FOLLOW, &nd);
	if (err)
		return err;
	/* Quotafile not on the same filesystem? */
	if (nd.mnt->mnt_sb != sb)
		return -EXDEV;
	/* Quotafile not of fs root? */
	if (nd.dentry->d_parent->d_inode != sb->s_root->d_inode)
		printk(KERN_WARNING
			"EXT3-fs: Quota file not on filesystem root. "
			"Journalled quota will not work.\n");
	if (!ext3_should_journal_data(nd.dentry->d_inode))
		printk(KERN_WARNING "EXT3-fs: Quota file does not have "
			"data-journalling. Journalled quota will not work.\n");
	path_release(&nd);
	return vfs_quota_on(sb, type, format_id, path);
}

#endif

static struct super_block *ext3_get_sb(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return get_sb_bdev(fs_type, flags, dev_name, data, ext3_fill_super);
}

static struct file_system_type ext3_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "ext3",
	.get_sb		= ext3_get_sb,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init init_ext3_fs(void)
{
	int err = init_ext3_xattr();
	if (err)
		return err;
	err = init_inodecache();
	if (err)
		goto out1;
        err = register_filesystem(&ext3_fs_type);
	if (err)
		goto out;
	return 0;
out:
	destroy_inodecache();
out1:
 	exit_ext3_xattr();
	return err;
}

static void __exit exit_ext3_fs(void)
{
	unregister_filesystem(&ext3_fs_type);
	destroy_inodecache();
	exit_ext3_xattr();
}

MODULE_AUTHOR("Remy Card, Stephen Tweedie, Andrew Morton, Andreas Dilger, Theodore Ts'o and others");
MODULE_DESCRIPTION("Second Extended Filesystem with journaling extensions");
MODULE_LICENSE("GPL");
module_init(init_ext3_fs)
module_exit(exit_ext3_fs)
