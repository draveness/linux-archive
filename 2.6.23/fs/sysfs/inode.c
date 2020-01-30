/*
 * inode.c - basic inode and dentry operations.
 *
 * sysfs is Copyright (c) 2001-3 Patrick Mochel
 *
 * Please see Documentation/filesystems/sysfs.txt for more information.
 */

#undef DEBUG 

#include <linux/pagemap.h>
#include <linux/namei.h>
#include <linux/backing-dev.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <asm/semaphore.h>
#include "sysfs.h"

extern struct super_block * sysfs_sb;

static const struct address_space_operations sysfs_aops = {
	.readpage	= simple_readpage,
	.prepare_write	= simple_prepare_write,
	.commit_write	= simple_commit_write
};

static struct backing_dev_info sysfs_backing_dev_info = {
	.ra_pages	= 0,	/* No readahead */
	.capabilities	= BDI_CAP_NO_ACCT_DIRTY | BDI_CAP_NO_WRITEBACK,
};

static const struct inode_operations sysfs_inode_operations ={
	.setattr	= sysfs_setattr,
};

void sysfs_delete_inode(struct inode *inode)
{
	/* Free the shadowed directory inode operations */
	if (sysfs_is_shadowed_inode(inode)) {
		kfree(inode->i_op);
		inode->i_op = NULL;
	}
	return generic_delete_inode(inode);
}

int sysfs_setattr(struct dentry * dentry, struct iattr * iattr)
{
	struct inode * inode = dentry->d_inode;
	struct sysfs_dirent * sd = dentry->d_fsdata;
	struct iattr * sd_iattr;
	unsigned int ia_valid = iattr->ia_valid;
	int error;

	if (!sd)
		return -EINVAL;

	sd_iattr = sd->s_iattr;

	error = inode_change_ok(inode, iattr);
	if (error)
		return error;

	error = inode_setattr(inode, iattr);
	if (error)
		return error;

	if (!sd_iattr) {
		/* setting attributes for the first time, allocate now */
		sd_iattr = kzalloc(sizeof(struct iattr), GFP_KERNEL);
		if (!sd_iattr)
			return -ENOMEM;
		/* assign default attributes */
		sd_iattr->ia_mode = sd->s_mode;
		sd_iattr->ia_uid = 0;
		sd_iattr->ia_gid = 0;
		sd_iattr->ia_atime = sd_iattr->ia_mtime = sd_iattr->ia_ctime = CURRENT_TIME;
		sd->s_iattr = sd_iattr;
	}

	/* attributes were changed atleast once in past */

	if (ia_valid & ATTR_UID)
		sd_iattr->ia_uid = iattr->ia_uid;
	if (ia_valid & ATTR_GID)
		sd_iattr->ia_gid = iattr->ia_gid;
	if (ia_valid & ATTR_ATIME)
		sd_iattr->ia_atime = timespec_trunc(iattr->ia_atime,
						inode->i_sb->s_time_gran);
	if (ia_valid & ATTR_MTIME)
		sd_iattr->ia_mtime = timespec_trunc(iattr->ia_mtime,
						inode->i_sb->s_time_gran);
	if (ia_valid & ATTR_CTIME)
		sd_iattr->ia_ctime = timespec_trunc(iattr->ia_ctime,
						inode->i_sb->s_time_gran);
	if (ia_valid & ATTR_MODE) {
		umode_t mode = iattr->ia_mode;

		if (!in_group_p(inode->i_gid) && !capable(CAP_FSETID))
			mode &= ~S_ISGID;
		sd_iattr->ia_mode = sd->s_mode = mode;
	}

	return error;
}

static inline void set_default_inode_attr(struct inode * inode, mode_t mode)
{
	inode->i_mode = mode;
	inode->i_uid = 0;
	inode->i_gid = 0;
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
}

static inline void set_inode_attr(struct inode * inode, struct iattr * iattr)
{
	inode->i_mode = iattr->ia_mode;
	inode->i_uid = iattr->ia_uid;
	inode->i_gid = iattr->ia_gid;
	inode->i_atime = iattr->ia_atime;
	inode->i_mtime = iattr->ia_mtime;
	inode->i_ctime = iattr->ia_ctime;
}


/*
 * sysfs has a different i_mutex lock order behavior for i_mutex than other
 * filesystems; sysfs i_mutex is called in many places with subsystem locks
 * held. At the same time, many of the VFS locking rules do not apply to
 * sysfs at all (cross directory rename for example). To untangle this mess
 * (which gives false positives in lockdep), we're giving sysfs inodes their
 * own class for i_mutex.
 */
static struct lock_class_key sysfs_inode_imutex_key;

static void sysfs_init_inode(struct sysfs_dirent *sd, struct inode *inode)
{
	inode->i_blocks = 0;
	inode->i_mapping->a_ops = &sysfs_aops;
	inode->i_mapping->backing_dev_info = &sysfs_backing_dev_info;
	inode->i_op = &sysfs_inode_operations;
	inode->i_ino = sd->s_ino;
	lockdep_set_class(&inode->i_mutex, &sysfs_inode_imutex_key);

	if (sd->s_iattr) {
		/* sysfs_dirent has non-default attributes
		 * get them for the new inode from persistent copy
		 * in sysfs_dirent
		 */
		set_inode_attr(inode, sd->s_iattr);
	} else
		set_default_inode_attr(inode, sd->s_mode);
}

/**
 *	sysfs_get_inode - get inode for sysfs_dirent
 *	@sd: sysfs_dirent to allocate inode for
 *
 *	Get inode for @sd.  If such inode doesn't exist, a new inode
 *	is allocated and basics are initialized.  New inode is
 *	returned locked.
 *
 *	LOCKING:
 *	Kernel thread context (may sleep).
 *
 *	RETURNS:
 *	Pointer to allocated inode on success, NULL on failure.
 */
struct inode * sysfs_get_inode(struct sysfs_dirent *sd)
{
	struct inode *inode;

	inode = iget_locked(sysfs_sb, sd->s_ino);
	if (inode && (inode->i_state & I_NEW))
		sysfs_init_inode(sd, inode);

	return inode;
}

/**
 *	sysfs_instantiate - instantiate dentry
 *	@dentry: dentry to be instantiated
 *	@inode: inode associated with @sd
 *
 *	Unlock @inode if locked and instantiate @dentry with @inode.
 *
 *	LOCKING:
 *	None.
 */
void sysfs_instantiate(struct dentry *dentry, struct inode *inode)
{
	BUG_ON(!dentry || dentry->d_inode);

	if (inode->i_state & I_NEW)
		unlock_new_inode(inode);

	d_instantiate(dentry, inode);
}

int sysfs_hash_and_remove(struct sysfs_dirent *dir_sd, const char *name)
{
	struct sysfs_addrm_cxt acxt;
	struct sysfs_dirent **pos, *sd;

	if (!dir_sd)
		return -ENOENT;

	sysfs_addrm_start(&acxt, dir_sd);

	for (pos = &dir_sd->s_children; *pos; pos = &(*pos)->s_sibling) {
		sd = *pos;

		if (!sysfs_type(sd))
			continue;
		if (!strcmp(sd->s_name, name)) {
			*pos = sd->s_sibling;
			sd->s_sibling = NULL;
			sysfs_remove_one(&acxt, sd);
			break;
		}
	}

	if (sysfs_addrm_finish(&acxt))
		return 0;
	return -ENOENT;
}
