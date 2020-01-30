/*
 *  linux/fs/nfs/file.c
 *
 *  Copyright (C) 1992  Rick Sladkey
 *
 *  Changes Copyright (C) 1994 by Florian La Roche
 *   - Do not copy data too often around in the kernel.
 *   - In nfs_file_read the return value of kmalloc wasn't checked.
 *   - Put in a better version of read look-ahead buffering. Original idea
 *     and implementation by Wai S Kok elekokws@ee.nus.sg.
 *
 *  Expire cache on write to a file by Wai S Kok (Oct 1994).
 *
 *  Total rewrite of read side for new NFS buffer cache.. Linus.
 *
 *  nfs regular file handling functions
 */

#include <linux/time.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_mount.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/smp_lock.h>

#include <asm/uaccess.h>
#include <asm/system.h>

#define NFSDBG_FACILITY		NFSDBG_FILE

static int nfs_file_open(struct inode *, struct file *);
static int nfs_file_release(struct inode *, struct file *);
static int  nfs_file_mmap(struct file *, struct vm_area_struct *);
static ssize_t nfs_file_sendfile(struct file *, loff_t *, size_t, read_actor_t, void *);
static ssize_t nfs_file_read(struct kiocb *, char __user *, size_t, loff_t);
static ssize_t nfs_file_write(struct kiocb *, const char __user *, size_t, loff_t);
static int  nfs_file_flush(struct file *);
static int  nfs_fsync(struct file *, struct dentry *dentry, int datasync);
static int nfs_check_flags(int flags);

struct file_operations nfs_file_operations = {
	.llseek		= remote_llseek,
	.read		= do_sync_read,
	.write		= do_sync_write,
	.aio_read		= nfs_file_read,
	.aio_write		= nfs_file_write,
	.mmap		= nfs_file_mmap,
	.open		= nfs_file_open,
	.flush		= nfs_file_flush,
	.release	= nfs_file_release,
	.fsync		= nfs_fsync,
	.lock		= nfs_lock,
	.sendfile	= nfs_file_sendfile,
	.check_flags	= nfs_check_flags,
};

struct inode_operations nfs_file_inode_operations = {
	.permission	= nfs_permission,
	.getattr	= nfs_getattr,
	.setattr	= nfs_setattr,
};

/* Hack for future NFS swap support */
#ifndef IS_SWAPFILE
# define IS_SWAPFILE(inode)	(0)
#endif

static int nfs_check_flags(int flags)
{
	if ((flags & (O_APPEND | O_DIRECT)) == (O_APPEND | O_DIRECT))
		return -EINVAL;

	return 0;
}

/*
 * Open file
 */
static int
nfs_file_open(struct inode *inode, struct file *filp)
{
	struct nfs_server *server = NFS_SERVER(inode);
	int (*open)(struct inode *, struct file *);
	int res;

	res = nfs_check_flags(filp->f_flags);
	if (res)
		return res;

	lock_kernel();
	/* Do NFSv4 open() call */
	if ((open = server->rpc_ops->file_open) != NULL)
		res = open(inode, filp);
	unlock_kernel();
	return res;
}

static int
nfs_file_release(struct inode *inode, struct file *filp)
{
	return NFS_PROTO(inode)->file_release(inode, filp);
}

/*
 * Flush all dirty pages, and check for write errors.
 *
 */
static int
nfs_file_flush(struct file *file)
{
	struct inode	*inode = file->f_dentry->d_inode;
	int		status;

	dfprintk(VFS, "nfs: flush(%s/%ld)\n", inode->i_sb->s_id, inode->i_ino);

	if ((file->f_mode & FMODE_WRITE) == 0)
		return 0;
	lock_kernel();
	/* Ensure that data+attribute caches are up to date after close() */
	status = nfs_wb_all(inode);
	if (!status) {
		status = file->f_error;
		file->f_error = 0;
		if (!status)
			__nfs_revalidate_inode(NFS_SERVER(inode), inode);
	}
	unlock_kernel();
	return status;
}

static ssize_t
nfs_file_read(struct kiocb *iocb, char __user * buf, size_t count, loff_t pos)
{
	struct dentry * dentry = iocb->ki_filp->f_dentry;
	struct inode * inode = dentry->d_inode;
	ssize_t result;

#ifdef CONFIG_NFS_DIRECTIO
	if (iocb->ki_filp->f_flags & O_DIRECT)
		return nfs_file_direct_read(iocb, buf, count, pos);
#endif

	dfprintk(VFS, "nfs: read(%s/%s, %lu@%lu)\n",
		dentry->d_parent->d_name.name, dentry->d_name.name,
		(unsigned long) count, (unsigned long) pos);

	result = nfs_revalidate_inode(NFS_SERVER(inode), inode);
	if (!result)
		result = generic_file_aio_read(iocb, buf, count, pos);
	return result;
}

static ssize_t
nfs_file_sendfile(struct file *filp, loff_t *ppos, size_t count,
		read_actor_t actor, void *target)
{
	struct dentry *dentry = filp->f_dentry;
	struct inode *inode = dentry->d_inode;
	ssize_t res;

	dfprintk(VFS, "nfs: sendfile(%s/%s, %lu@%Lu)\n",
		dentry->d_parent->d_name.name, dentry->d_name.name,
		(unsigned long) count, (unsigned long long) *ppos);

	res = nfs_revalidate_inode(NFS_SERVER(inode), inode);
	if (!res)
		res = generic_file_sendfile(filp, ppos, count, actor, target);
	return res;
}

static int
nfs_file_mmap(struct file * file, struct vm_area_struct * vma)
{
	struct dentry *dentry = file->f_dentry;
	struct inode *inode = dentry->d_inode;
	int	status;

	dfprintk(VFS, "nfs: mmap(%s/%s)\n",
		dentry->d_parent->d_name.name, dentry->d_name.name);

	status = nfs_revalidate_inode(NFS_SERVER(inode), inode);
	if (!status)
		status = generic_file_mmap(file, vma);
	return status;
}

/*
 * Flush any dirty pages for this process, and check for write errors.
 * The return status from this call provides a reliable indication of
 * whether any write errors occurred for this process.
 */
static int
nfs_fsync(struct file *file, struct dentry *dentry, int datasync)
{
	struct inode *inode = dentry->d_inode;
	int status;

	dfprintk(VFS, "nfs: fsync(%s/%ld)\n", inode->i_sb->s_id, inode->i_ino);

	lock_kernel();
	status = nfs_wb_all(inode);
	if (!status) {
		status = file->f_error;
		file->f_error = 0;
	}
	unlock_kernel();
	return status;
}

/*
 * This does the "real" work of the write. The generic routine has
 * allocated the page, locked it, done all the page alignment stuff
 * calculations etc. Now we should just copy the data from user
 * space and write it back to the real medium..
 *
 * If the writer ends up delaying the write, the writer needs to
 * increment the page use counts until he is done with the page.
 */
static int nfs_prepare_write(struct file *file, struct page *page, unsigned offset, unsigned to)
{
	return nfs_flush_incompatible(file, page);
}

static int nfs_commit_write(struct file *file, struct page *page, unsigned offset, unsigned to)
{
	long status;

	lock_kernel();
	status = nfs_updatepage(file, page, offset, to-offset);
	unlock_kernel();
	return status;
}

struct address_space_operations nfs_file_aops = {
	.readpage = nfs_readpage,
	.readpages = nfs_readpages,
	.set_page_dirty = __set_page_dirty_nobuffers,
	.writepage = nfs_writepage,
	.writepages = nfs_writepages,
	.prepare_write = nfs_prepare_write,
	.commit_write = nfs_commit_write,
#ifdef CONFIG_NFS_DIRECTIO
	.direct_IO = nfs_direct_IO,
#endif
};

/* 
 * Write to a file (through the page cache).
 */
static ssize_t
nfs_file_write(struct kiocb *iocb, const char __user *buf, size_t count, loff_t pos)
{
	struct dentry * dentry = iocb->ki_filp->f_dentry;
	struct inode * inode = dentry->d_inode;
	ssize_t result;

#ifdef CONFIG_NFS_DIRECTIO
	if (iocb->ki_filp->f_flags & O_DIRECT)
		return nfs_file_direct_write(iocb, buf, count, pos);
#endif

	dfprintk(VFS, "nfs: write(%s/%s(%ld), %lu@%lu)\n",
		dentry->d_parent->d_name.name, dentry->d_name.name,
		inode->i_ino, (unsigned long) count, (unsigned long) pos);

	result = -EBUSY;
	if (IS_SWAPFILE(inode))
		goto out_swapfile;
	result = nfs_revalidate_inode(NFS_SERVER(inode), inode);
	if (result)
		goto out;

	result = count;
	if (!count)
		goto out;

	result = generic_file_aio_write(iocb, buf, count, pos);
out:
	return result;

out_swapfile:
	printk(KERN_INFO "NFS: attempt to write to active swap file!\n");
	goto out;
}

/*
 * Lock a (portion of) a file
 */
int
nfs_lock(struct file *filp, int cmd, struct file_lock *fl)
{
	struct inode * inode = filp->f_mapping->host;
	int	status = 0;
	int	status2;

	dprintk("NFS: nfs_lock(f=%s/%ld, t=%x, fl=%x, r=%Ld:%Ld)\n",
			inode->i_sb->s_id, inode->i_ino,
			fl->fl_type, fl->fl_flags,
			(long long)fl->fl_start, (long long)fl->fl_end);

	if (!inode)
		return -EINVAL;

	/* No mandatory locks over NFS */
	if ((inode->i_mode & (S_ISGID | S_IXGRP)) == S_ISGID)
		return -ENOLCK;

	if (NFS_PROTO(inode)->version != 4) {
		/* Fake OK code if mounted without NLM support */
		if (NFS_SERVER(inode)->flags & NFS_MOUNT_NONLM) {
			if (IS_GETLK(cmd))
				status = LOCK_USE_CLNT;
			goto out_ok;
		}
	}

	/*
	 * No BSD flocks over NFS allowed.
	 * Note: we could try to fake a POSIX lock request here by
	 * using ((u32) filp | 0x80000000) or some such as the pid.
	 * Not sure whether that would be unique, though, or whether
	 * that would break in other places.
	 */
	if (!fl->fl_owner || !(fl->fl_flags & FL_POSIX))
		return -ENOLCK;

	/*
	 * Flush all pending writes before doing anything
	 * with locks..
	 */
	status = filemap_fdatawrite(filp->f_mapping);
	down(&inode->i_sem);
	status2 = nfs_wb_all(inode);
	if (!status)
		status = status2;
	up(&inode->i_sem);
	status2 = filemap_fdatawait(filp->f_mapping);
	if (!status)
		status = status2;
	if (status < 0)
		return status;

	lock_kernel();
	status = NFS_PROTO(inode)->lock(filp, cmd, fl);
	unlock_kernel();
	if (status < 0)
		return status;
	
	status = 0;

	/*
	 * Make sure we clear the cache whenever we try to get the lock.
	 * This makes locking act as a cache coherency point.
	 */
 out_ok:
	if ((IS_SETLK(cmd) || IS_SETLKW(cmd)) && fl->fl_type != F_UNLCK) {
		filemap_fdatawrite(filp->f_mapping);
		down(&inode->i_sem);
		nfs_wb_all(inode);      /* we may have slept */
		up(&inode->i_sem);
		filemap_fdatawait(filp->f_mapping);
		nfs_zap_caches(inode);
	}
	return status;
}
