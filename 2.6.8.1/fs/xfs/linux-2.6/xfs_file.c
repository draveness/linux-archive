/*
 * Copyright (c) 2000-2004 Silicon Graphics, Inc.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Further, this software is distributed without any warranty that it is
 * free of the rightful claim of any third person regarding infringement
 * or the like.  Any license provided herein, whether implied or
 * otherwise, applies only to this software file.  Patent licenses, if
 * any, provided herein do not apply to combinations of this program with
 * other software, or any other product whatsoever.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston MA 02111-1307, USA.
 *
 * Contact information: Silicon Graphics, Inc., 1600 Amphitheatre Pkwy,
 * Mountain View, CA  94043, or:
 *
 * http://www.sgi.com
 *
 * For further information regarding this notice, see:
 *
 * http://oss.sgi.com/projects/GenInfo/SGIGPLNoticeExplan/
 */

#include "xfs.h"
#include "xfs_inum.h"
#include "xfs_log.h"
#include "xfs_sb.h"
#include "xfs_dir.h"
#include "xfs_dir2.h"
#include "xfs_trans.h"
#include "xfs_dmapi.h"
#include "xfs_mount.h"
#include "xfs_bmap_btree.h"
#include "xfs_alloc_btree.h"
#include "xfs_ialloc_btree.h"
#include "xfs_alloc.h"
#include "xfs_btree.h"
#include "xfs_attr_sf.h"
#include "xfs_dir_sf.h"
#include "xfs_dir2_sf.h"
#include "xfs_dinode.h"
#include "xfs_inode.h"
#include "xfs_error.h"
#include "xfs_rw.h"

#include <linux/dcache.h>
#include <linux/smp_lock.h>

static struct vm_operations_struct linvfs_file_vm_ops;


STATIC inline ssize_t
__linvfs_read(
	struct kiocb		*iocb,
	char __user		*buf,
	int			ioflags,
	size_t			count,
	loff_t			pos)
{
	struct iovec		iov = {buf, count};
	struct file		*file = iocb->ki_filp;
	vnode_t			*vp = LINVFS_GET_VP(file->f_dentry->d_inode);
	ssize_t			rval;

	BUG_ON(iocb->ki_pos != pos);

	if (unlikely(file->f_flags & O_DIRECT))
		ioflags |= IO_ISDIRECT;
	VOP_READ(vp, iocb, &iov, 1, &iocb->ki_pos, ioflags, NULL, rval);
	return rval;
}


STATIC ssize_t
linvfs_read(
	struct kiocb		*iocb,
	char __user		*buf,
	size_t			count,
	loff_t			pos)
{
	return __linvfs_read(iocb, buf, 0, count, pos);
}

STATIC ssize_t
linvfs_read_invis(
	struct kiocb		*iocb,
	char __user		*buf,
	size_t			count,
	loff_t			pos)
{
	return __linvfs_read(iocb, buf, IO_INVIS, count, pos);
}


STATIC inline ssize_t
__linvfs_write(
	struct kiocb	*iocb,
	const char 	*buf,
	int		ioflags,
	size_t		count,
	loff_t		pos)
{
	struct iovec	iov = {(void *)buf, count};
	struct file	*file = iocb->ki_filp;
	struct inode	*inode = file->f_mapping->host;
	vnode_t		*vp = LINVFS_GET_VP(inode);
	ssize_t		rval;

	BUG_ON(iocb->ki_pos != pos);
	if (unlikely(file->f_flags & O_DIRECT)) {
		ioflags |= IO_ISDIRECT;
		VOP_WRITE(vp, iocb, &iov, 1, &iocb->ki_pos,
				ioflags, NULL, rval);
	} else {
		down(&inode->i_sem);
		VOP_WRITE(vp, iocb, &iov, 1, &iocb->ki_pos,
				ioflags, NULL, rval);
		up(&inode->i_sem);
	}

	return rval;
}


STATIC ssize_t
linvfs_write(
	struct kiocb		*iocb,
	const char __user	*buf,
	size_t			count,
	loff_t			pos)
{
	return __linvfs_write(iocb, buf, 0, count, pos);
}

STATIC ssize_t
linvfs_write_invis(
	struct kiocb		*iocb,
	const char __user	*buf,
	size_t			count,
	loff_t			pos)
{
	return __linvfs_write(iocb, buf, IO_INVIS, count, pos);
}


STATIC inline ssize_t
__linvfs_readv(
	struct file		*file,
	const struct iovec 	*iov,
	int			ioflags,
	unsigned long		nr_segs,
	loff_t			*ppos)
{
	struct inode	*inode = file->f_mapping->host;
	vnode_t		*vp = LINVFS_GET_VP(inode);
	struct		kiocb kiocb;
	ssize_t		rval;

	init_sync_kiocb(&kiocb, file);
	kiocb.ki_pos = *ppos;

	if (unlikely(file->f_flags & O_DIRECT))
		ioflags |= IO_ISDIRECT;
	VOP_READ(vp, &kiocb, iov, nr_segs, &kiocb.ki_pos, ioflags, NULL, rval);
	if (rval == -EIOCBQUEUED)
		rval = wait_on_sync_kiocb(&kiocb);

	*ppos = kiocb.ki_pos;
	return rval;
}

STATIC ssize_t
linvfs_readv(
	struct file		*file,
	const struct iovec 	*iov,
	unsigned long		nr_segs,
	loff_t			*ppos)
{
	return __linvfs_readv(file, iov, 0, nr_segs, ppos);
}

STATIC ssize_t
linvfs_readv_invis(
	struct file		*file,
	const struct iovec 	*iov,
	unsigned long		nr_segs,
	loff_t			*ppos)
{
	return __linvfs_readv(file, iov, IO_INVIS, nr_segs, ppos);
}


STATIC inline ssize_t
__linvfs_writev(
	struct file		*file,
	const struct iovec 	*iov,
	int			ioflags,
	unsigned long		nr_segs,
	loff_t			*ppos)
{
	struct inode	*inode = file->f_mapping->host;
	vnode_t		*vp = LINVFS_GET_VP(inode);
	struct		kiocb kiocb;
	ssize_t		rval;

	init_sync_kiocb(&kiocb, file);
	kiocb.ki_pos = *ppos;
	if (unlikely(file->f_flags & O_DIRECT)) {
		ioflags |= IO_ISDIRECT;
		VOP_WRITE(vp, &kiocb, iov, nr_segs, &kiocb.ki_pos,
				ioflags, NULL, rval);
	} else {
		down(&inode->i_sem);
		VOP_WRITE(vp, &kiocb, iov, nr_segs, &kiocb.ki_pos,
				ioflags, NULL, rval);
		up(&inode->i_sem);
	}

	if (rval == -EIOCBQUEUED)
		rval = wait_on_sync_kiocb(&kiocb);

	*ppos = kiocb.ki_pos;
	return rval;
}


STATIC ssize_t
linvfs_writev(
	struct file		*file,
	const struct iovec 	*iov,
	unsigned long		nr_segs,
	loff_t			*ppos)
{
	return __linvfs_writev(file, iov, 0, nr_segs, ppos);
}

STATIC ssize_t
linvfs_writev_invis(
	struct file		*file,
	const struct iovec 	*iov,
	unsigned long		nr_segs,
	loff_t			*ppos)
{
	return __linvfs_writev(file, iov, IO_INVIS, nr_segs, ppos);
}

STATIC ssize_t
linvfs_sendfile(
	struct file		*filp,
	loff_t			*ppos,
	size_t			count,
	read_actor_t		actor,
	void			*target)
{
	vnode_t			*vp = LINVFS_GET_VP(filp->f_dentry->d_inode);
	ssize_t			rval;

	VOP_SENDFILE(vp, filp, ppos, 0, count, actor, target, NULL, rval);
	return rval;
}


STATIC int
linvfs_open(
	struct inode	*inode,
	struct file	*filp)
{
	vnode_t		*vp = LINVFS_GET_VP(inode);
	int		error;

	if (!(filp->f_flags & O_LARGEFILE) && i_size_read(inode) > MAX_NON_LFS)
		return -EFBIG;

	ASSERT(vp);
	VOP_OPEN(vp, NULL, error);
	return -error;
}


STATIC int
linvfs_release(
	struct inode	*inode,
	struct file	*filp)
{
	vnode_t		*vp = LINVFS_GET_VP(inode);
	int		error = 0;

	if (vp)
		VOP_RELEASE(vp, error);
	return -error;
}


STATIC int
linvfs_fsync(
	struct file	*filp,
	struct dentry	*dentry,
	int		datasync)
{
	struct inode	*inode = dentry->d_inode;
	vnode_t		*vp = LINVFS_GET_VP(inode);
	int		error;
	int		flags = FSYNC_WAIT;

	if (datasync)
		flags |= FSYNC_DATA;

	ASSERT(vp);
	VOP_FSYNC(vp, flags, NULL, (xfs_off_t)0, (xfs_off_t)-1, error);
	return -error;
}

/*
 * linvfs_readdir maps to VOP_READDIR().
 * We need to build a uio, cred, ...
 */

#define nextdp(dp)      ((struct xfs_dirent *)((char *)(dp) + (dp)->d_reclen))

STATIC int
linvfs_readdir(
	struct file	*filp,
	void		*dirent,
	filldir_t	filldir)
{
	int		error = 0;
	vnode_t		*vp;
	uio_t		uio;
	iovec_t		iov;
	int		eof = 0;
	caddr_t		read_buf;
	int		namelen, size = 0;
	size_t		rlen = PAGE_CACHE_SIZE;
	xfs_off_t	start_offset, curr_offset;
	xfs_dirent_t	*dbp = NULL;

	vp = LINVFS_GET_VP(filp->f_dentry->d_inode);
	ASSERT(vp);

	/* Try fairly hard to get memory */
	do {
		if ((read_buf = (caddr_t)kmalloc(rlen, GFP_KERNEL)))
			break;
		rlen >>= 1;
	} while (rlen >= 1024);

	if (read_buf == NULL)
		return -ENOMEM;

	uio.uio_iov = &iov;
	uio.uio_segflg = UIO_SYSSPACE;
	curr_offset = filp->f_pos;
	if (filp->f_pos != 0x7fffffff)
		uio.uio_offset = filp->f_pos;
	else
		uio.uio_offset = 0xffffffff;

	while (!eof) {
		uio.uio_resid = iov.iov_len = rlen;
		iov.iov_base = read_buf;
		uio.uio_iovcnt = 1;

		start_offset = uio.uio_offset;

		VOP_READDIR(vp, &uio, NULL, &eof, error);
		if ((uio.uio_offset == start_offset) || error) {
			size = 0;
			break;
		}

		size = rlen - uio.uio_resid;
		dbp = (xfs_dirent_t *)read_buf;
		while (size > 0) {
			namelen = strlen(dbp->d_name);

			if (filldir(dirent, dbp->d_name, namelen,
					(loff_t) curr_offset & 0x7fffffff,
					(ino_t) dbp->d_ino,
					DT_UNKNOWN)) {
				goto done;
			}
			size -= dbp->d_reclen;
			curr_offset = (loff_t)dbp->d_off /* & 0x7fffffff */;
			dbp = nextdp(dbp);
		}
	}
done:
	if (!error) {
		if (size == 0)
			filp->f_pos = uio.uio_offset & 0x7fffffff;
		else if (dbp)
			filp->f_pos = curr_offset;
	}

	kfree(read_buf);
	return -error;
}


STATIC int
linvfs_file_mmap(
	struct file	*filp,
	struct vm_area_struct *vma)
{
	struct inode	*ip = filp->f_dentry->d_inode;
	vnode_t		*vp = LINVFS_GET_VP(ip);
	vattr_t		va = { .va_mask = XFS_AT_UPDATIME };
	int		error;

	if ((vp->v_type == VREG) && (vp->v_vfsp->vfs_flag & VFS_DMI)) {
		xfs_mount_t	*mp = XFS_VFSTOM(vp->v_vfsp);

		error = -XFS_SEND_MMAP(mp, vma, 0);
		if (error)
			return error;
	}

	vma->vm_ops = &linvfs_file_vm_ops;

	VOP_SETATTR(vp, &va, XFS_AT_UPDATIME, NULL, error);
	return 0;
}


STATIC int
linvfs_ioctl(
	struct inode	*inode,
	struct file	*filp,
	unsigned int	cmd,
	unsigned long	arg)
{
	int		error;
	vnode_t		*vp = LINVFS_GET_VP(inode);

	unlock_kernel();
	VOP_IOCTL(vp, inode, filp, 0, cmd, arg, error);
	VMODIFY(vp);
	lock_kernel();

	/* NOTE:  some of the ioctl's return positive #'s as a
	 *	  byte count indicating success, such as
	 *	  readlink_by_handle.  So we don't "sign flip"
	 *	  like most other routines.  This means true
	 *	  errors need to be returned as a negative value.
	 */
	return error;
}

STATIC int
linvfs_ioctl_invis(
	struct inode	*inode,
	struct file	*filp,
	unsigned int	cmd,
	unsigned long	arg)
{
	int		error;
	vnode_t		*vp = LINVFS_GET_VP(inode);

	unlock_kernel();
	ASSERT(vp);
	VOP_IOCTL(vp, inode, filp, IO_INVIS, cmd, arg, error);
	VMODIFY(vp);
	lock_kernel();

	/* NOTE:  some of the ioctl's return positive #'s as a
	 *	  byte count indicating success, such as
	 *	  readlink_by_handle.  So we don't "sign flip"
	 *	  like most other routines.  This means true
	 *	  errors need to be returned as a negative value.
	 */
	return error;
}

#ifdef HAVE_VMOP_MPROTECT
STATIC int
linvfs_mprotect(
	struct vm_area_struct *vma,
	unsigned int	newflags)
{
	vnode_t		*vp = LINVFS_GET_VP(vma->vm_file->f_dentry->d_inode);
	int		error = 0;

	if ((vp->v_type == VREG) && (vp->v_vfsp->vfs_flag & VFS_DMI)) {
		if ((vma->vm_flags & VM_MAYSHARE) &&
		    (newflags & VM_WRITE) && !(vma->vm_flags & VM_WRITE)) {
			xfs_mount_t	*mp = XFS_VFSTOM(vp->v_vfsp);

			error = XFS_SEND_MMAP(mp, vma, VM_WRITE);
		    }
	}
	return error;
}
#endif /* HAVE_VMOP_MPROTECT */


struct file_operations linvfs_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= do_sync_read,
	.write		= do_sync_write,
	.readv		= linvfs_readv,
	.writev		= linvfs_writev,
	.aio_read	= linvfs_read,
	.aio_write	= linvfs_write,
	.sendfile	= linvfs_sendfile,
	.ioctl		= linvfs_ioctl,
	.mmap		= linvfs_file_mmap,
	.open		= linvfs_open,
	.release	= linvfs_release,
	.fsync		= linvfs_fsync,
};

struct file_operations linvfs_invis_file_operations = {
	.llseek		= generic_file_llseek,
	.read		= do_sync_read,
	.write		= do_sync_write,
	.readv		= linvfs_readv_invis,
	.writev		= linvfs_writev_invis,
	.aio_read	= linvfs_read_invis,
	.aio_write	= linvfs_write_invis,
	.sendfile	= linvfs_sendfile,
	.ioctl		= linvfs_ioctl_invis,
	.mmap		= linvfs_file_mmap,
	.open		= linvfs_open,
	.release	= linvfs_release,
	.fsync		= linvfs_fsync,
};


struct file_operations linvfs_dir_operations = {
	.read		= generic_read_dir,
	.readdir	= linvfs_readdir,
	.ioctl		= linvfs_ioctl,
	.fsync		= linvfs_fsync,
};

static struct vm_operations_struct linvfs_file_vm_ops = {
	.nopage		= filemap_nopage,
#ifdef HAVE_VMOP_MPROTECT
	.mprotect	= linvfs_mprotect,
#endif
};
