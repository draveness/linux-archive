/**
 * eCryptfs: Linux filesystem encryption layer
 *
 * Copyright (C) 1997-2004 Erez Zadok
 * Copyright (C) 2001-2004 Stony Brook University
 * Copyright (C) 2004-2007 International Business Machines Corp.
 *   Author(s): Michael A. Halcrow <mhalcrow@us.ibm.com>
 *   		Michael C. Thompson <mcthomps@us.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include <linux/file.h>
#include <linux/poll.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/security.h>
#include <linux/compat.h>
#include <linux/fs_stack.h>
#include "ecryptfs_kernel.h"

/**
 * ecryptfs_read_update_atime
 *
 * generic_file_read updates the atime of upper layer inode.  But, it
 * doesn't give us a chance to update the atime of the lower layer
 * inode.  This function is a wrapper to generic_file_read.  It
 * updates the atime of the lower level inode if generic_file_read
 * returns without any errors. This is to be used only for file reads.
 * The function to be used for directory reads is ecryptfs_read.
 */
static ssize_t ecryptfs_read_update_atime(struct kiocb *iocb,
				const struct iovec *iov,
				unsigned long nr_segs, loff_t pos)
{
	int rc;
	struct dentry *lower_dentry;
	struct vfsmount *lower_vfsmount;
	struct file *file = iocb->ki_filp;

	rc = generic_file_aio_read(iocb, iov, nr_segs, pos);
	/*
	 * Even though this is a async interface, we need to wait
	 * for IO to finish to update atime
	 */
	if (-EIOCBQUEUED == rc)
		rc = wait_on_sync_kiocb(iocb);
	if (rc >= 0) {
		lower_dentry = ecryptfs_dentry_to_lower(file->f_path.dentry);
		lower_vfsmount = ecryptfs_dentry_to_lower_mnt(file->f_path.dentry);
		touch_atime(lower_vfsmount, lower_dentry);
	}
	return rc;
}

struct ecryptfs_getdents_callback {
	void *dirent;
	struct dentry *dentry;
	filldir_t filldir;
	int err;
	int filldir_called;
	int entries_written;
};

/* Inspired by generic filldir in fs/readir.c */
static int
ecryptfs_filldir(void *dirent, const char *name, int namelen, loff_t offset,
		 u64 ino, unsigned int d_type)
{
	struct ecryptfs_crypt_stat *crypt_stat;
	struct ecryptfs_getdents_callback *buf =
	    (struct ecryptfs_getdents_callback *)dirent;
	int rc;
	int decoded_length;
	char *decoded_name;

	crypt_stat = ecryptfs_dentry_to_private(buf->dentry)->crypt_stat;
	buf->filldir_called++;
	decoded_length = ecryptfs_decode_filename(crypt_stat, name, namelen,
						  &decoded_name);
	if (decoded_length < 0) {
		rc = decoded_length;
		goto out;
	}
	rc = buf->filldir(buf->dirent, decoded_name, decoded_length, offset,
			  ino, d_type);
	kfree(decoded_name);
	if (rc >= 0)
		buf->entries_written++;
out:
	return rc;
}

/**
 * ecryptfs_readdir
 * @file: The ecryptfs file struct
 * @dirent: Directory entry
 * @filldir: The filldir callback function
 */
static int ecryptfs_readdir(struct file *file, void *dirent, filldir_t filldir)
{
	int rc;
	struct file *lower_file;
	struct inode *inode;
	struct ecryptfs_getdents_callback buf;

	lower_file = ecryptfs_file_to_lower(file);
	lower_file->f_pos = file->f_pos;
	inode = file->f_path.dentry->d_inode;
	memset(&buf, 0, sizeof(buf));
	buf.dirent = dirent;
	buf.dentry = file->f_path.dentry;
	buf.filldir = filldir;
retry:
	buf.filldir_called = 0;
	buf.entries_written = 0;
	buf.err = 0;
	rc = vfs_readdir(lower_file, ecryptfs_filldir, (void *)&buf);
	if (buf.err)
		rc = buf.err;
	if (buf.filldir_called && !buf.entries_written)
		goto retry;
	file->f_pos = lower_file->f_pos;
	if (rc >= 0)
		fsstack_copy_attr_atime(inode, lower_file->f_path.dentry->d_inode);
	return rc;
}

struct kmem_cache *ecryptfs_file_info_cache;

int ecryptfs_open_lower_file(struct file **lower_file,
			     struct dentry *lower_dentry,
			     struct vfsmount *lower_mnt, int flags)
{
	int rc = 0;

	flags |= O_LARGEFILE;
	dget(lower_dentry);
	mntget(lower_mnt);
	*lower_file = dentry_open(lower_dentry, lower_mnt, flags);
	if (IS_ERR(*lower_file)) {
		printk(KERN_ERR "Error opening lower file for lower_dentry "
		       "[0x%p], lower_mnt [0x%p], and flags [0x%x]\n",
		       lower_dentry, lower_mnt, flags);
		rc = PTR_ERR(*lower_file);
		*lower_file = NULL;
		goto out;
	}
out:
	return rc;
}

int ecryptfs_close_lower_file(struct file *lower_file)
{
	fput(lower_file);
	return 0;
}

/**
 * ecryptfs_open
 * @inode: inode speciying file to open
 * @file: Structure to return filled in
 *
 * Opens the file specified by inode.
 *
 * Returns zero on success; non-zero otherwise
 */
static int ecryptfs_open(struct inode *inode, struct file *file)
{
	int rc = 0;
	struct ecryptfs_crypt_stat *crypt_stat = NULL;
	struct ecryptfs_mount_crypt_stat *mount_crypt_stat;
	struct dentry *ecryptfs_dentry = file->f_path.dentry;
	/* Private value of ecryptfs_dentry allocated in
	 * ecryptfs_lookup() */
	struct dentry *lower_dentry = ecryptfs_dentry_to_lower(ecryptfs_dentry);
	struct inode *lower_inode = NULL;
	struct file *lower_file = NULL;
	struct vfsmount *lower_mnt;
	struct ecryptfs_file_info *file_info;
	int lower_flags;

	mount_crypt_stat = &ecryptfs_superblock_to_private(
		ecryptfs_dentry->d_sb)->mount_crypt_stat;
	if ((mount_crypt_stat->flags & ECRYPTFS_ENCRYPTED_VIEW_ENABLED)
	    && ((file->f_flags & O_WRONLY) || (file->f_flags & O_RDWR)
		|| (file->f_flags & O_CREAT) || (file->f_flags & O_TRUNC)
		|| (file->f_flags & O_APPEND))) {
		printk(KERN_WARNING "Mount has encrypted view enabled; "
		       "files may only be read\n");
		rc = -EPERM;
		goto out;
	}
	/* Released in ecryptfs_release or end of function if failure */
	file_info = kmem_cache_zalloc(ecryptfs_file_info_cache, GFP_KERNEL);
	ecryptfs_set_file_private(file, file_info);
	if (!file_info) {
		ecryptfs_printk(KERN_ERR,
				"Error attempting to allocate memory\n");
		rc = -ENOMEM;
		goto out;
	}
	lower_dentry = ecryptfs_dentry_to_lower(ecryptfs_dentry);
	crypt_stat = &ecryptfs_inode_to_private(inode)->crypt_stat;
	mutex_lock(&crypt_stat->cs_mutex);
	if (!(crypt_stat->flags & ECRYPTFS_POLICY_APPLIED)) {
		ecryptfs_printk(KERN_DEBUG, "Setting flags for stat...\n");
		/* Policy code enabled in future release */
		crypt_stat->flags |= ECRYPTFS_POLICY_APPLIED;
		crypt_stat->flags |= ECRYPTFS_ENCRYPTED;
	}
	mutex_unlock(&crypt_stat->cs_mutex);
	lower_flags = file->f_flags;
	if ((lower_flags & O_ACCMODE) == O_WRONLY)
		lower_flags = (lower_flags & O_ACCMODE) | O_RDWR;
	if (file->f_flags & O_APPEND)
		lower_flags &= ~O_APPEND;
	lower_mnt = ecryptfs_dentry_to_lower_mnt(ecryptfs_dentry);
	/* Corresponding fput() in ecryptfs_release() */
	if ((rc = ecryptfs_open_lower_file(&lower_file, lower_dentry, lower_mnt,
					   lower_flags))) {
		ecryptfs_printk(KERN_ERR, "Error opening lower file\n");
		goto out_puts;
	}
	ecryptfs_set_file_lower(file, lower_file);
	/* Isn't this check the same as the one in lookup? */
	lower_inode = lower_dentry->d_inode;
	if (S_ISDIR(ecryptfs_dentry->d_inode->i_mode)) {
		ecryptfs_printk(KERN_DEBUG, "This is a directory\n");
		crypt_stat->flags &= ~(ECRYPTFS_ENCRYPTED);
		rc = 0;
		goto out;
	}
	mutex_lock(&crypt_stat->cs_mutex);
	if (!(crypt_stat->flags & ECRYPTFS_POLICY_APPLIED)
	    || !(crypt_stat->flags & ECRYPTFS_KEY_VALID)) {
		rc = ecryptfs_read_metadata(ecryptfs_dentry, lower_file);
		if (rc) {
			ecryptfs_printk(KERN_DEBUG,
					"Valid headers not found\n");
			if (!(mount_crypt_stat->flags
			      & ECRYPTFS_PLAINTEXT_PASSTHROUGH_ENABLED)) {
				rc = -EIO;
				printk(KERN_WARNING "Attempt to read file that "
				       "is not in a valid eCryptfs format, "
				       "and plaintext passthrough mode is not "
				       "enabled; returning -EIO\n");
				mutex_unlock(&crypt_stat->cs_mutex);
				goto out_puts;
			}
			rc = 0;
			crypt_stat->flags &= ~(ECRYPTFS_ENCRYPTED);
			mutex_unlock(&crypt_stat->cs_mutex);
			goto out;
		}
	}
	mutex_unlock(&crypt_stat->cs_mutex);
	ecryptfs_printk(KERN_DEBUG, "inode w/ addr = [0x%p], i_ino = [0x%.16x] "
			"size: [0x%.16x]\n", inode, inode->i_ino,
			i_size_read(inode));
	ecryptfs_set_file_lower(file, lower_file);
	goto out;
out_puts:
	mntput(lower_mnt);
	dput(lower_dentry);
	kmem_cache_free(ecryptfs_file_info_cache,
			ecryptfs_file_to_private(file));
out:
	return rc;
}

static int ecryptfs_flush(struct file *file, fl_owner_t td)
{
	int rc = 0;
	struct file *lower_file = NULL;

	lower_file = ecryptfs_file_to_lower(file);
	if (lower_file->f_op && lower_file->f_op->flush)
		rc = lower_file->f_op->flush(lower_file, td);
	return rc;
}

static int ecryptfs_release(struct inode *inode, struct file *file)
{
	struct file *lower_file = ecryptfs_file_to_lower(file);
	struct ecryptfs_file_info *file_info = ecryptfs_file_to_private(file);
	struct inode *lower_inode = ecryptfs_inode_to_lower(inode);
	int rc;

	if ((rc = ecryptfs_close_lower_file(lower_file))) {
		printk(KERN_ERR "Error closing lower_file\n");
		goto out;
	}
	inode->i_blocks = lower_inode->i_blocks;
	kmem_cache_free(ecryptfs_file_info_cache, file_info);
out:
	return rc;
}

static int
ecryptfs_fsync(struct file *file, struct dentry *dentry, int datasync)
{
	struct file *lower_file = ecryptfs_file_to_lower(file);
	struct dentry *lower_dentry = ecryptfs_dentry_to_lower(dentry);
	struct inode *lower_inode = lower_dentry->d_inode;
	int rc = -EINVAL;

	if (lower_inode->i_fop->fsync) {
		mutex_lock(&lower_inode->i_mutex);
		rc = lower_inode->i_fop->fsync(lower_file, lower_dentry,
					       datasync);
		mutex_unlock(&lower_inode->i_mutex);
	}
	return rc;
}

static int ecryptfs_fasync(int fd, struct file *file, int flag)
{
	int rc = 0;
	struct file *lower_file = NULL;

	lower_file = ecryptfs_file_to_lower(file);
	if (lower_file->f_op && lower_file->f_op->fasync)
		rc = lower_file->f_op->fasync(fd, lower_file, flag);
	return rc;
}

static ssize_t ecryptfs_splice_read(struct file *file, loff_t * ppos,
				    struct pipe_inode_info *pipe, size_t count,
				    unsigned int flags)
{
	struct file *lower_file = NULL;
	int rc = -EINVAL;

	lower_file = ecryptfs_file_to_lower(file);
	if (lower_file->f_op && lower_file->f_op->splice_read)
		rc = lower_file->f_op->splice_read(lower_file, ppos, pipe,
						count, flags);

	return rc;
}

static int ecryptfs_ioctl(struct inode *inode, struct file *file,
			  unsigned int cmd, unsigned long arg);

const struct file_operations ecryptfs_dir_fops = {
	.readdir = ecryptfs_readdir,
	.ioctl = ecryptfs_ioctl,
	.mmap = generic_file_mmap,
	.open = ecryptfs_open,
	.flush = ecryptfs_flush,
	.release = ecryptfs_release,
	.fsync = ecryptfs_fsync,
	.fasync = ecryptfs_fasync,
	.splice_read = ecryptfs_splice_read,
};

const struct file_operations ecryptfs_main_fops = {
	.llseek = generic_file_llseek,
	.read = do_sync_read,
	.aio_read = ecryptfs_read_update_atime,
	.write = do_sync_write,
	.aio_write = generic_file_aio_write,
	.readdir = ecryptfs_readdir,
	.ioctl = ecryptfs_ioctl,
	.mmap = generic_file_mmap,
	.open = ecryptfs_open,
	.flush = ecryptfs_flush,
	.release = ecryptfs_release,
	.fsync = ecryptfs_fsync,
	.fasync = ecryptfs_fasync,
	.splice_read = ecryptfs_splice_read,
};

static int
ecryptfs_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
	       unsigned long arg)
{
	int rc = 0;
	struct file *lower_file = NULL;

	if (ecryptfs_file_to_private(file))
		lower_file = ecryptfs_file_to_lower(file);
	if (lower_file && lower_file->f_op && lower_file->f_op->ioctl)
		rc = lower_file->f_op->ioctl(ecryptfs_inode_to_lower(inode),
					     lower_file, cmd, arg);
	else
		rc = -ENOTTY;
	return rc;
}
