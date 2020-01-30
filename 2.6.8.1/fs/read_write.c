/*
 *  linux/fs/read_write.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/slab.h> 
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/smp_lock.h>
#include <linux/dnotify.h>
#include <linux/security.h>
#include <linux/module.h>

#include <asm/uaccess.h>
#include <asm/unistd.h>

struct file_operations generic_ro_fops = {
	.llseek		= generic_file_llseek,
	.read		= generic_file_read,
	.mmap		= generic_file_readonly_mmap,
	.sendfile	= generic_file_sendfile,
};

EXPORT_SYMBOL(generic_ro_fops);

loff_t generic_file_llseek(struct file *file, loff_t offset, int origin)
{
	long long retval;
	struct inode *inode = file->f_mapping->host;

	down(&inode->i_sem);
	switch (origin) {
		case 2:
			offset += inode->i_size;
			break;
		case 1:
			offset += file->f_pos;
	}
	retval = -EINVAL;
	if (offset>=0 && offset<=inode->i_sb->s_maxbytes) {
		if (offset != file->f_pos) {
			file->f_pos = offset;
			file->f_version = 0;
		}
		retval = offset;
	}
	up(&inode->i_sem);
	return retval;
}

EXPORT_SYMBOL(generic_file_llseek);

loff_t remote_llseek(struct file *file, loff_t offset, int origin)
{
	long long retval;

	lock_kernel();
	switch (origin) {
		case 2:
			offset += i_size_read(file->f_dentry->d_inode);
			break;
		case 1:
			offset += file->f_pos;
	}
	retval = -EINVAL;
	if (offset>=0 && offset<=file->f_dentry->d_inode->i_sb->s_maxbytes) {
		if (offset != file->f_pos) {
			file->f_pos = offset;
			file->f_version = 0;
		}
		retval = offset;
	}
	unlock_kernel();
	return retval;
}
EXPORT_SYMBOL(remote_llseek);

loff_t no_llseek(struct file *file, loff_t offset, int origin)
{
	return -ESPIPE;
}
EXPORT_SYMBOL(no_llseek);

loff_t default_llseek(struct file *file, loff_t offset, int origin)
{
	long long retval;

	lock_kernel();
	switch (origin) {
		case 2:
			offset += i_size_read(file->f_dentry->d_inode);
			break;
		case 1:
			offset += file->f_pos;
	}
	retval = -EINVAL;
	if (offset >= 0) {
		if (offset != file->f_pos) {
			file->f_pos = offset;
			file->f_version = 0;
		}
		retval = offset;
	}
	unlock_kernel();
	return retval;
}
EXPORT_SYMBOL(default_llseek);

loff_t vfs_llseek(struct file *file, loff_t offset, int origin)
{
	loff_t (*fn)(struct file *, loff_t, int);

	fn = no_llseek;
	if (file->f_mode & FMODE_LSEEK) {
		fn = default_llseek;
		if (file->f_op && file->f_op->llseek)
			fn = file->f_op->llseek;
	}
	return fn(file, offset, origin);
}
EXPORT_SYMBOL(vfs_llseek);

asmlinkage off_t sys_lseek(unsigned int fd, off_t offset, unsigned int origin)
{
	off_t retval;
	struct file * file;
	int fput_needed;

	retval = -EBADF;
	file = fget_light(fd, &fput_needed);
	if (!file)
		goto bad;

	retval = -EINVAL;
	if (origin <= 2) {
		loff_t res = vfs_llseek(file, offset, origin);
		retval = res;
		if (res != (loff_t)retval)
			retval = -EOVERFLOW;	/* LFS: should only happen on 32 bit platforms */
	}
	fput_light(file, fput_needed);
bad:
	return retval;
}
EXPORT_SYMBOL_GPL(sys_lseek);

#ifdef __ARCH_WANT_SYS_LLSEEK
asmlinkage long sys_llseek(unsigned int fd, unsigned long offset_high,
			   unsigned long offset_low, loff_t __user * result,
			   unsigned int origin)
{
	int retval;
	struct file * file;
	loff_t offset;
	int fput_needed;

	retval = -EBADF;
	file = fget_light(fd, &fput_needed);
	if (!file)
		goto bad;

	retval = -EINVAL;
	if (origin > 2)
		goto out_putf;

	offset = vfs_llseek(file, ((loff_t) offset_high << 32) | offset_low,
			origin);

	retval = (int)offset;
	if (offset >= 0) {
		retval = -EFAULT;
		if (!copy_to_user(result, &offset, sizeof(offset)))
			retval = 0;
	}
out_putf:
	fput_light(file, fput_needed);
bad:
	return retval;
}
#endif

ssize_t do_sync_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
	struct kiocb kiocb;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	kiocb.ki_pos = *ppos;
	ret = filp->f_op->aio_read(&kiocb, buf, len, kiocb.ki_pos);
	if (-EIOCBQUEUED == ret)
		ret = wait_on_sync_kiocb(&kiocb);
	*ppos = kiocb.ki_pos;
	return ret;
}

EXPORT_SYMBOL(do_sync_read);

ssize_t vfs_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	struct inode *inode = file->f_dentry->d_inode;
	ssize_t ret;

	if (!(file->f_mode & FMODE_READ))
		return -EBADF;
	if (!file->f_op || (!file->f_op->read && !file->f_op->aio_read))
		return -EINVAL;

	ret = locks_verify_area(FLOCK_VERIFY_READ, inode, file, *pos, count);
	if (!ret) {
		ret = security_file_permission (file, MAY_READ);
		if (!ret) {
			if (file->f_op->read)
				ret = file->f_op->read(file, buf, count, pos);
			else
				ret = do_sync_read(file, buf, count, pos);
			if (ret > 0)
				dnotify_parent(file->f_dentry, DN_ACCESS);
		}
	}

	return ret;
}

EXPORT_SYMBOL(vfs_read);

ssize_t do_sync_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{
	struct kiocb kiocb;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	kiocb.ki_pos = *ppos;
	ret = filp->f_op->aio_write(&kiocb, buf, len, kiocb.ki_pos);
	if (-EIOCBQUEUED == ret)
		ret = wait_on_sync_kiocb(&kiocb);
	*ppos = kiocb.ki_pos;
	return ret;
}

EXPORT_SYMBOL(do_sync_write);

ssize_t vfs_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	struct inode *inode = file->f_dentry->d_inode;
	ssize_t ret;

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (!file->f_op || (!file->f_op->write && !file->f_op->aio_write))
		return -EINVAL;

	ret = locks_verify_area(FLOCK_VERIFY_WRITE, inode, file, *pos, count);
	if (!ret) {
		ret = security_file_permission (file, MAY_WRITE);
		if (!ret) {
			if (file->f_op->write)
				ret = file->f_op->write(file, buf, count, pos);
			else
				ret = do_sync_write(file, buf, count, pos);
			if (ret > 0)
				dnotify_parent(file->f_dentry, DN_MODIFY);
		}
	}

	return ret;
}

EXPORT_SYMBOL(vfs_write);

static inline loff_t file_pos_read(struct file *file)
{
	return file->f_pos;
}

static inline void file_pos_write(struct file *file, loff_t pos)
{
	file->f_pos = pos;
}

asmlinkage ssize_t sys_read(unsigned int fd, char __user * buf, size_t count)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	file = fget_light(fd, &fput_needed);
	if (file) {
		loff_t pos = file_pos_read(file);
		ret = vfs_read(file, buf, count, &pos);
		file_pos_write(file, pos);
		fput_light(file, fput_needed);
	}

	return ret;
}
EXPORT_SYMBOL_GPL(sys_read);

asmlinkage ssize_t sys_write(unsigned int fd, const char __user * buf, size_t count)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	file = fget_light(fd, &fput_needed);
	if (file) {
		loff_t pos = file_pos_read(file);
		ret = vfs_write(file, buf, count, &pos);
		file_pos_write(file, pos);
		fput_light(file, fput_needed);
	}

	return ret;
}

asmlinkage ssize_t sys_pread64(unsigned int fd, char __user *buf,
			     size_t count, loff_t pos)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	if (pos < 0)
		return -EINVAL;

	file = fget_light(fd, &fput_needed);
	if (file) {
		ret = -ESPIPE;
		if (file->f_mode & FMODE_PREAD)
			ret = vfs_read(file, buf, count, &pos);
		fput_light(file, fput_needed);
	}

	return ret;
}

asmlinkage ssize_t sys_pwrite64(unsigned int fd, const char __user *buf,
			      size_t count, loff_t pos)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	if (pos < 0)
		return -EINVAL;

	file = fget_light(fd, &fput_needed);
	if (file) {
		ret = -ESPIPE;
		if (file->f_mode & FMODE_PWRITE)  
			ret = vfs_write(file, buf, count, &pos);
		fput_light(file, fput_needed);
	}

	return ret;
}

/*
 * Reduce an iovec's length in-place.  Return the resulting number of segments
 */
unsigned long iov_shorten(struct iovec *iov, unsigned long nr_segs, size_t to)
{
	unsigned long seg = 0;
	size_t len = 0;

	while (seg < nr_segs) {
		seg++;
		if (len + iov->iov_len >= to) {
			iov->iov_len = to - len;
			break;
		}
		len += iov->iov_len;
		iov++;
	}
	return seg;
}

EXPORT_SYMBOL(iov_shorten);

static ssize_t do_readv_writev(int type, struct file *file,
			       const struct iovec __user * uvector,
			       unsigned long nr_segs, loff_t *pos)
{
	typedef ssize_t (*io_fn_t)(struct file *, char __user *, size_t, loff_t *);
	typedef ssize_t (*iov_fn_t)(struct file *, const struct iovec *, unsigned long, loff_t *);

	size_t tot_len;
	struct iovec iovstack[UIO_FASTIOV];
	struct iovec *iov=iovstack, *vector;
	ssize_t ret;
	int seg;
	io_fn_t fn;
	iov_fn_t fnv;
	struct inode *inode;

	/*
	 * SuS says "The readv() function *may* fail if the iovcnt argument
	 * was less than or equal to 0, or greater than {IOV_MAX}.  Linux has
	 * traditionally returned zero for zero segments, so...
	 */
	ret = 0;
	if (nr_segs == 0)
		goto out;

	/*
	 * First get the "struct iovec" from user memory and
	 * verify all the pointers
	 */
	ret = -EINVAL;
	if ((nr_segs > UIO_MAXIOV) || (nr_segs <= 0))
		goto out;
	if (!file->f_op)
		goto out;
	if (nr_segs > UIO_FASTIOV) {
		ret = -ENOMEM;
		iov = kmalloc(nr_segs*sizeof(struct iovec), GFP_KERNEL);
		if (!iov)
			goto out;
	}
	ret = -EFAULT;
	if (copy_from_user(iov, uvector, nr_segs*sizeof(*uvector)))
		goto out;

	/*
	 * Single unix specification:
	 * We should -EINVAL if an element length is not >= 0 and fitting an
	 * ssize_t.  The total length is fitting an ssize_t
	 *
	 * Be careful here because iov_len is a size_t not an ssize_t
	 */
	tot_len = 0;
	ret = -EINVAL;
	for (seg = 0; seg < nr_segs; seg++) {
		ssize_t len = (ssize_t)iov[seg].iov_len;

		if (len < 0)	/* size_t not fitting an ssize_t .. */
			goto out;
		tot_len += len;
		if ((ssize_t)tot_len < 0) /* maths overflow on the ssize_t */
			goto out;
	}
	if (tot_len == 0) {
		ret = 0;
		goto out;
	}

	inode = file->f_dentry->d_inode;
	/* VERIFY_WRITE actually means a read, as we write to user space */
	ret = locks_verify_area((type == READ 
				 ? FLOCK_VERIFY_READ : FLOCK_VERIFY_WRITE),
				inode, file, *pos, tot_len);
	if (ret)
		goto out;

	fnv = NULL;
	if (type == READ) {
		fn = file->f_op->read;
		fnv = file->f_op->readv;
	} else {
		fn = (io_fn_t)file->f_op->write;
		fnv = file->f_op->writev;
	}
	if (fnv) {
		ret = fnv(file, iov, nr_segs, pos);
		goto out;
	}

	/* Do it by hand, with file-ops */
	ret = 0;
	vector = iov;
	while (nr_segs > 0) {
		void __user * base;
		size_t len;
		ssize_t nr;

		base = vector->iov_base;
		len = vector->iov_len;
		vector++;
		nr_segs--;

		nr = fn(file, base, len, pos);

		if (nr < 0) {
			if (!ret) ret = nr;
			break;
		}
		ret += nr;
		if (nr != len)
			break;
	}
out:
	if (iov != iovstack)
		kfree(iov);
	if ((ret + (type == READ)) > 0)
		dnotify_parent(file->f_dentry,
				(type == READ) ? DN_ACCESS : DN_MODIFY);
	return ret;
}

ssize_t vfs_readv(struct file *file, const struct iovec __user *vec,
		  unsigned long vlen, loff_t *pos)
{
	if (!(file->f_mode & FMODE_READ))
		return -EBADF;
	if (!file->f_op || (!file->f_op->readv && !file->f_op->read))
		return -EINVAL;

	return do_readv_writev(READ, file, vec, vlen, pos);
}

EXPORT_SYMBOL(vfs_readv);

ssize_t vfs_writev(struct file *file, const struct iovec __user *vec,
		   unsigned long vlen, loff_t *pos)
{
	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (!file->f_op || (!file->f_op->writev && !file->f_op->write))
		return -EINVAL;

	return do_readv_writev(WRITE, file, vec, vlen, pos);
}

EXPORT_SYMBOL(vfs_writev);

asmlinkage ssize_t
sys_readv(unsigned long fd, const struct iovec __user *vec, unsigned long vlen)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	file = fget_light(fd, &fput_needed);
	if (file) {
		loff_t pos = file_pos_read(file);
		ret = vfs_readv(file, vec, vlen, &pos);
		file_pos_write(file, pos);
		fput_light(file, fput_needed);
	}

	return ret;
}

asmlinkage ssize_t
sys_writev(unsigned long fd, const struct iovec __user *vec, unsigned long vlen)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	file = fget_light(fd, &fput_needed);
	if (file) {
		loff_t pos = file_pos_read(file);
		ret = vfs_writev(file, vec, vlen, &pos);
		file_pos_write(file, pos);
		fput_light(file, fput_needed);
	}

	return ret;
}

static ssize_t do_sendfile(int out_fd, int in_fd, loff_t *ppos,
			   size_t count, loff_t max)
{
	struct file * in_file, * out_file;
	struct inode * in_inode, * out_inode;
	loff_t pos;
	ssize_t retval;
	int fput_needed_in, fput_needed_out;

	/*
	 * Get input file, and verify that it is ok..
	 */
	retval = -EBADF;
	in_file = fget_light(in_fd, &fput_needed_in);
	if (!in_file)
		goto out;
	if (!(in_file->f_mode & FMODE_READ))
		goto fput_in;
	retval = -EINVAL;
	in_inode = in_file->f_dentry->d_inode;
	if (!in_inode)
		goto fput_in;
	if (!in_file->f_op || !in_file->f_op->sendfile)
		goto fput_in;
	retval = -ESPIPE;
	if (!ppos)
		ppos = &in_file->f_pos;
	else
		if (!(in_file->f_mode & FMODE_PREAD))
			goto fput_in;
	retval = locks_verify_area(FLOCK_VERIFY_READ, in_inode, in_file, *ppos, count);
	if (retval)
		goto fput_in;

	retval = security_file_permission (in_file, MAY_READ);
	if (retval)
		goto fput_in;

	/*
	 * Get output file, and verify that it is ok..
	 */
	retval = -EBADF;
	out_file = fget_light(out_fd, &fput_needed_out);
	if (!out_file)
		goto fput_in;
	if (!(out_file->f_mode & FMODE_WRITE))
		goto fput_out;
	retval = -EINVAL;
	if (!out_file->f_op || !out_file->f_op->sendpage)
		goto fput_out;
	out_inode = out_file->f_dentry->d_inode;
	retval = locks_verify_area(FLOCK_VERIFY_WRITE, out_inode, out_file, out_file->f_pos, count);
	if (retval)
		goto fput_out;

	retval = security_file_permission (out_file, MAY_WRITE);
	if (retval)
		goto fput_out;

	if (!max)
		max = min(in_inode->i_sb->s_maxbytes, out_inode->i_sb->s_maxbytes);

	pos = *ppos;
	retval = -EINVAL;
	if (unlikely(pos < 0))
		goto fput_out;
	if (unlikely(pos + count > max)) {
		retval = -EOVERFLOW;
		if (pos >= max)
			goto fput_out;
		count = max - pos;
	}

	retval = in_file->f_op->sendfile(in_file, ppos, count, file_send_actor, out_file);

	if (*ppos > max)
		retval = -EOVERFLOW;

fput_out:
	fput_light(out_file, fput_needed_out);
fput_in:
	fput_light(in_file, fput_needed_in);
out:
	return retval;
}

asmlinkage ssize_t sys_sendfile(int out_fd, int in_fd, off_t __user *offset, size_t count)
{
	loff_t pos;
	off_t off;
	ssize_t ret;

	if (offset) {
		if (unlikely(get_user(off, offset)))
			return -EFAULT;
		pos = off;
		ret = do_sendfile(out_fd, in_fd, &pos, count, MAX_NON_LFS);
		if (unlikely(put_user(pos, offset)))
			return -EFAULT;
		return ret;
	}

	return do_sendfile(out_fd, in_fd, NULL, count, 0);
}

asmlinkage ssize_t sys_sendfile64(int out_fd, int in_fd, loff_t __user *offset, size_t count)
{
	loff_t pos;
	ssize_t ret;

	if (offset) {
		if (unlikely(copy_from_user(&pos, offset, sizeof(loff_t))))
			return -EFAULT;
		ret = do_sendfile(out_fd, in_fd, &pos, count, 0);
		if (unlikely(put_user(pos, offset)))
			return -EFAULT;
		return ret;
	}

	return do_sendfile(out_fd, in_fd, NULL, count, 0);
}
