/*
 *  linux/fs/open.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/string.h>
#include <linux/mm.h>
#include <linux/utime.h>
#include <linux/file.h>
#include <linux/smp_lock.h>
#include <linux/quotaops.h>
#include <linux/dnotify.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/tty.h>
#include <linux/namei.h>
#include <linux/backing-dev.h>
#include <linux/security.h>
#include <linux/mount.h>
#include <linux/vfs.h>
#include <asm/uaccess.h>
#include <linux/fs.h>
#include <linux/pagemap.h>

#include <asm/unistd.h>

int vfs_statfs(struct super_block *sb, struct kstatfs *buf)
{
	int retval = -ENODEV;

	if (sb) {
		retval = -ENOSYS;
		if (sb->s_op->statfs) {
			memset(buf, 0, sizeof(*buf));
			retval = security_sb_statfs(sb);
			if (retval)
				return retval;
			retval = sb->s_op->statfs(sb, buf);
			if (retval == 0 && buf->f_frsize == 0)
				buf->f_frsize = buf->f_bsize;
		}
	}
	return retval;
}

EXPORT_SYMBOL(vfs_statfs);

static int vfs_statfs_native(struct super_block *sb, struct statfs *buf)
{
	struct kstatfs st;
	int retval;

	retval = vfs_statfs(sb, &st);
	if (retval)
		return retval;

	if (sizeof(*buf) == sizeof(st))
		memcpy(buf, &st, sizeof(st));
	else {
		if (sizeof buf->f_blocks == 4) {
			if ((st.f_blocks | st.f_bfree | st.f_bavail) &
			    0xffffffff00000000ULL)
				return -EOVERFLOW;
			/*
			 * f_files and f_ffree may be -1; it's okay to stuff
			 * that into 32 bits
			 */
			if (st.f_files != -1 &&
			    (st.f_files & 0xffffffff00000000ULL))
				return -EOVERFLOW;
			if (st.f_ffree != -1 &&
			    (st.f_ffree & 0xffffffff00000000ULL))
				return -EOVERFLOW;
		}

		buf->f_type = st.f_type;
		buf->f_bsize = st.f_bsize;
		buf->f_blocks = st.f_blocks;
		buf->f_bfree = st.f_bfree;
		buf->f_bavail = st.f_bavail;
		buf->f_files = st.f_files;
		buf->f_ffree = st.f_ffree;
		buf->f_fsid = st.f_fsid;
		buf->f_namelen = st.f_namelen;
		buf->f_frsize = st.f_frsize;
		memset(buf->f_spare, 0, sizeof(buf->f_spare));
	}
	return 0;
}

static int vfs_statfs64(struct super_block *sb, struct statfs64 *buf)
{
	struct kstatfs st;
	int retval;

	retval = vfs_statfs(sb, &st);
	if (retval)
		return retval;

	if (sizeof(*buf) == sizeof(st))
		memcpy(buf, &st, sizeof(st));
	else {
		buf->f_type = st.f_type;
		buf->f_bsize = st.f_bsize;
		buf->f_blocks = st.f_blocks;
		buf->f_bfree = st.f_bfree;
		buf->f_bavail = st.f_bavail;
		buf->f_files = st.f_files;
		buf->f_ffree = st.f_ffree;
		buf->f_fsid = st.f_fsid;
		buf->f_namelen = st.f_namelen;
		buf->f_frsize = st.f_frsize;
		memset(buf->f_spare, 0, sizeof(buf->f_spare));
	}
	return 0;
}

asmlinkage long sys_statfs(const char __user * path, struct statfs __user * buf)
{
	struct nameidata nd;
	int error;

	error = user_path_walk(path, &nd);
	if (!error) {
		struct statfs tmp;
		error = vfs_statfs_native(nd.dentry->d_inode->i_sb, &tmp);
		if (!error && copy_to_user(buf, &tmp, sizeof(tmp)))
			error = -EFAULT;
		path_release(&nd);
	}
	return error;
}


asmlinkage long sys_statfs64(const char __user *path, size_t sz, struct statfs64 __user *buf)
{
	struct nameidata nd;
	long error;

	if (sz != sizeof(*buf))
		return -EINVAL;
	error = user_path_walk(path, &nd);
	if (!error) {
		struct statfs64 tmp;
		error = vfs_statfs64(nd.dentry->d_inode->i_sb, &tmp);
		if (!error && copy_to_user(buf, &tmp, sizeof(tmp)))
			error = -EFAULT;
		path_release(&nd);
	}
	return error;
}


asmlinkage long sys_fstatfs(unsigned int fd, struct statfs __user * buf)
{
	struct file * file;
	struct statfs tmp;
	int error;

	error = -EBADF;
	file = fget(fd);
	if (!file)
		goto out;
	error = vfs_statfs_native(file->f_dentry->d_inode->i_sb, &tmp);
	if (!error && copy_to_user(buf, &tmp, sizeof(tmp)))
		error = -EFAULT;
	fput(file);
out:
	return error;
}

asmlinkage long sys_fstatfs64(unsigned int fd, size_t sz, struct statfs64 __user *buf)
{
	struct file * file;
	struct statfs64 tmp;
	int error;

	if (sz != sizeof(*buf))
		return -EINVAL;

	error = -EBADF;
	file = fget(fd);
	if (!file)
		goto out;
	error = vfs_statfs64(file->f_dentry->d_inode->i_sb, &tmp);
	if (!error && copy_to_user(buf, &tmp, sizeof(tmp)))
		error = -EFAULT;
	fput(file);
out:
	return error;
}

int do_truncate(struct dentry *dentry, loff_t length)
{
	int err;
	struct iattr newattrs;

	/* Not pretty: "inode->i_size" shouldn't really be signed. But it is. */
	if (length < 0)
		return -EINVAL;

	newattrs.ia_size = length;
	newattrs.ia_valid = ATTR_SIZE | ATTR_CTIME;
	down(&dentry->d_inode->i_sem);
	down_write(&dentry->d_inode->i_alloc_sem);
	err = notify_change(dentry, &newattrs);
	up_write(&dentry->d_inode->i_alloc_sem);
	up(&dentry->d_inode->i_sem);
	return err;
}

static inline long do_sys_truncate(const char __user * path, loff_t length)
{
	struct nameidata nd;
	struct inode * inode;
	int error;

	error = -EINVAL;
	if (length < 0)	/* sorry, but loff_t says... */
		goto out;

	error = user_path_walk(path, &nd);
	if (error)
		goto out;
	inode = nd.dentry->d_inode;

	/* For directories it's -EISDIR, for other non-regulars - -EINVAL */
	error = -EISDIR;
	if (S_ISDIR(inode->i_mode))
		goto dput_and_out;

	error = -EINVAL;
	if (!S_ISREG(inode->i_mode))
		goto dput_and_out;

	error = permission(inode,MAY_WRITE,&nd);
	if (error)
		goto dput_and_out;

	error = -EROFS;
	if (IS_RDONLY(inode))
		goto dput_and_out;

	error = -EPERM;
	if (IS_IMMUTABLE(inode) || IS_APPEND(inode))
		goto dput_and_out;

	/*
	 * Make sure that there are no leases.
	 */
	error = break_lease(inode, FMODE_WRITE);
	if (error)
		goto dput_and_out;

	error = get_write_access(inode);
	if (error)
		goto dput_and_out;

	error = locks_verify_truncate(inode, NULL, length);
	if (!error) {
		DQUOT_INIT(inode);
		error = do_truncate(nd.dentry, length);
	}
	put_write_access(inode);

dput_and_out:
	path_release(&nd);
out:
	return error;
}

asmlinkage long sys_truncate(const char __user * path, unsigned long length)
{
	/* on 32-bit boxen it will cut the range 2^31--2^32-1 off */
	return do_sys_truncate(path, (long)length);
}

static inline long do_sys_ftruncate(unsigned int fd, loff_t length, int small)
{
	struct inode * inode;
	struct dentry *dentry;
	struct file * file;
	int error;

	error = -EINVAL;
	if (length < 0)
		goto out;
	error = -EBADF;
	file = fget(fd);
	if (!file)
		goto out;

	/* explicitly opened as large or we are on 64-bit box */
	if (file->f_flags & O_LARGEFILE)
		small = 0;

	dentry = file->f_dentry;
	inode = dentry->d_inode;
	error = -EINVAL;
	if (!S_ISREG(inode->i_mode) || !(file->f_mode & FMODE_WRITE))
		goto out_putf;

	error = -EINVAL;
	/* Cannot ftruncate over 2^31 bytes without large file support */
	if (small && length > MAX_NON_LFS)
		goto out_putf;

	error = -EPERM;
	if (IS_APPEND(inode))
		goto out_putf;

	error = locks_verify_truncate(inode, file, length);
	if (!error)
		error = do_truncate(dentry, length);
out_putf:
	fput(file);
out:
	return error;
}

asmlinkage long sys_ftruncate(unsigned int fd, unsigned long length)
{
	return do_sys_ftruncate(fd, length, 1);
}

/* LFS versions of truncate are only needed on 32 bit machines */
#if BITS_PER_LONG == 32
asmlinkage long sys_truncate64(const char __user * path, loff_t length)
{
	return do_sys_truncate(path, length);
}

asmlinkage long sys_ftruncate64(unsigned int fd, loff_t length)
{
	return do_sys_ftruncate(fd, length, 0);
}
#endif

#ifdef __ARCH_WANT_SYS_UTIME

/*
 * sys_utime() can be implemented in user-level using sys_utimes().
 * Is this for backwards compatibility?  If so, why not move it
 * into the appropriate arch directory (for those architectures that
 * need it).
 */

/* If times==NULL, set access and modification to current time,
 * must be owner or have write permission.
 * Else, update from *times, must be owner or super user.
 */
asmlinkage long sys_utime(char __user * filename, struct utimbuf __user * times)
{
	int error;
	struct nameidata nd;
	struct inode * inode;
	struct iattr newattrs;

	error = user_path_walk(filename, &nd);
	if (error)
		goto out;
	inode = nd.dentry->d_inode;

	error = -EROFS;
	if (IS_RDONLY(inode))
		goto dput_and_out;

	/* Don't worry, the checks are done in inode_change_ok() */
	newattrs.ia_valid = ATTR_CTIME | ATTR_MTIME | ATTR_ATIME;
	if (times) {
		error = -EPERM;
		if (IS_APPEND(inode) || IS_IMMUTABLE(inode))
			goto dput_and_out;

		error = get_user(newattrs.ia_atime.tv_sec, &times->actime);
		newattrs.ia_atime.tv_nsec = 0;
		if (!error) 
			error = get_user(newattrs.ia_mtime.tv_sec, &times->modtime);
		newattrs.ia_mtime.tv_nsec = 0;
		if (error)
			goto dput_and_out;

		newattrs.ia_valid |= ATTR_ATIME_SET | ATTR_MTIME_SET;
	} else {
                error = -EACCES;
                if (IS_IMMUTABLE(inode))
                        goto dput_and_out;

		if (current->fsuid != inode->i_uid &&
		    (error = permission(inode,MAY_WRITE,&nd)) != 0)
			goto dput_and_out;
	}
	down(&inode->i_sem);
	error = notify_change(nd.dentry, &newattrs);
	up(&inode->i_sem);
dput_and_out:
	path_release(&nd);
out:
	return error;
}

#endif

/* If times==NULL, set access and modification to current time,
 * must be owner or have write permission.
 * Else, update from *times, must be owner or super user.
 */
long do_utimes(char __user * filename, struct timeval * times)
{
	int error;
	struct nameidata nd;
	struct inode * inode;
	struct iattr newattrs;

	error = user_path_walk(filename, &nd);

	if (error)
		goto out;
	inode = nd.dentry->d_inode;

	error = -EROFS;
	if (IS_RDONLY(inode))
		goto dput_and_out;

	/* Don't worry, the checks are done in inode_change_ok() */
	newattrs.ia_valid = ATTR_CTIME | ATTR_MTIME | ATTR_ATIME;
	if (times) {
		error = -EPERM;
                if (IS_APPEND(inode) || IS_IMMUTABLE(inode))
                        goto dput_and_out;

		newattrs.ia_atime.tv_sec = times[0].tv_sec;
		newattrs.ia_atime.tv_nsec = times[0].tv_usec * 1000;
		newattrs.ia_mtime.tv_sec = times[1].tv_sec;
		newattrs.ia_mtime.tv_nsec = times[1].tv_usec * 1000;
		newattrs.ia_valid |= ATTR_ATIME_SET | ATTR_MTIME_SET;
	} else {
		error = -EACCES;
                if (IS_IMMUTABLE(inode))
                        goto dput_and_out;

		if (current->fsuid != inode->i_uid &&
		    (error = permission(inode,MAY_WRITE,&nd)) != 0)
			goto dput_and_out;
	}
	down(&inode->i_sem);
	error = notify_change(nd.dentry, &newattrs);
	up(&inode->i_sem);
dput_and_out:
	path_release(&nd);
out:
	return error;
}

asmlinkage long sys_utimes(char __user * filename, struct timeval __user * utimes)
{
	struct timeval times[2];

	if (utimes && copy_from_user(&times, utimes, sizeof(times)))
		return -EFAULT;
	return do_utimes(filename, utimes ? times : NULL);
}


/*
 * access() needs to use the real uid/gid, not the effective uid/gid.
 * We do this by temporarily clearing all FS-related capabilities and
 * switching the fsuid/fsgid around to the real ones.
 */
asmlinkage long sys_access(const char __user * filename, int mode)
{
	struct nameidata nd;
	int old_fsuid, old_fsgid;
	kernel_cap_t old_cap;
	int res;

	if (mode & ~S_IRWXO)	/* where's F_OK, X_OK, W_OK, R_OK? */
		return -EINVAL;

	old_fsuid = current->fsuid;
	old_fsgid = current->fsgid;
	old_cap = current->cap_effective;

	current->fsuid = current->uid;
	current->fsgid = current->gid;

	/*
	 * Clear the capabilities if we switch to a non-root user
	 *
	 * FIXME: There is a race here against sys_capset.  The
	 * capabilities can change yet we will restore the old
	 * value below.  We should hold task_capabilities_lock,
	 * but we cannot because user_path_walk can sleep.
	 */
	if (current->uid)
		cap_clear(current->cap_effective);
	else
		current->cap_effective = current->cap_permitted;

	res = __user_walk(filename, LOOKUP_FOLLOW|LOOKUP_ACCESS, &nd);
	if (!res) {
		res = permission(nd.dentry->d_inode, mode, &nd);
		/* SuS v2 requires we report a read only fs too */
		if(!res && (mode & S_IWOTH) && IS_RDONLY(nd.dentry->d_inode)
		   && !special_file(nd.dentry->d_inode->i_mode))
			res = -EROFS;
		path_release(&nd);
	}

	current->fsuid = old_fsuid;
	current->fsgid = old_fsgid;
	current->cap_effective = old_cap;

	return res;
}

asmlinkage long sys_chdir(const char __user * filename)
{
	struct nameidata nd;
	int error;

	error = __user_walk(filename, LOOKUP_FOLLOW|LOOKUP_DIRECTORY, &nd);
	if (error)
		goto out;

	error = permission(nd.dentry->d_inode,MAY_EXEC,&nd);
	if (error)
		goto dput_and_out;

	set_fs_pwd(current->fs, nd.mnt, nd.dentry);

dput_and_out:
	path_release(&nd);
out:
	return error;
}

asmlinkage long sys_fchdir(unsigned int fd)
{
	struct file *file;
	struct dentry *dentry;
	struct inode *inode;
	struct vfsmount *mnt;
	int error;

	error = -EBADF;
	file = fget(fd);
	if (!file)
		goto out;

	dentry = file->f_dentry;
	mnt = file->f_vfsmnt;
	inode = dentry->d_inode;

	error = -ENOTDIR;
	if (!S_ISDIR(inode->i_mode))
		goto out_putf;

	error = permission(inode, MAY_EXEC, NULL);
	if (!error)
		set_fs_pwd(current->fs, mnt, dentry);
out_putf:
	fput(file);
out:
	return error;
}

asmlinkage long sys_chroot(const char __user * filename)
{
	struct nameidata nd;
	int error;

	error = __user_walk(filename, LOOKUP_FOLLOW | LOOKUP_DIRECTORY | LOOKUP_NOALT, &nd);
	if (error)
		goto out;

	error = permission(nd.dentry->d_inode,MAY_EXEC,&nd);
	if (error)
		goto dput_and_out;

	error = -EPERM;
	if (!capable(CAP_SYS_CHROOT))
		goto dput_and_out;

	set_fs_root(current->fs, nd.mnt, nd.dentry);
	set_fs_altroot();
	error = 0;
dput_and_out:
	path_release(&nd);
out:
	return error;
}

asmlinkage long sys_fchmod(unsigned int fd, mode_t mode)
{
	struct inode * inode;
	struct dentry * dentry;
	struct file * file;
	int err = -EBADF;
	struct iattr newattrs;

	file = fget(fd);
	if (!file)
		goto out;

	dentry = file->f_dentry;
	inode = dentry->d_inode;

	err = -EROFS;
	if (IS_RDONLY(inode))
		goto out_putf;
	err = -EPERM;
	if (IS_IMMUTABLE(inode) || IS_APPEND(inode))
		goto out_putf;
	down(&inode->i_sem);
	if (mode == (mode_t) -1)
		mode = inode->i_mode;
	newattrs.ia_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO);
	newattrs.ia_valid = ATTR_MODE | ATTR_CTIME;
	err = notify_change(dentry, &newattrs);
	up(&inode->i_sem);

out_putf:
	fput(file);
out:
	return err;
}

asmlinkage long sys_chmod(const char __user * filename, mode_t mode)
{
	struct nameidata nd;
	struct inode * inode;
	int error;
	struct iattr newattrs;

	error = user_path_walk(filename, &nd);
	if (error)
		goto out;
	inode = nd.dentry->d_inode;

	error = -EROFS;
	if (IS_RDONLY(inode))
		goto dput_and_out;

	error = -EPERM;
	if (IS_IMMUTABLE(inode) || IS_APPEND(inode))
		goto dput_and_out;

	down(&inode->i_sem);
	if (mode == (mode_t) -1)
		mode = inode->i_mode;
	newattrs.ia_mode = (mode & S_IALLUGO) | (inode->i_mode & ~S_IALLUGO);
	newattrs.ia_valid = ATTR_MODE | ATTR_CTIME;
	error = notify_change(nd.dentry, &newattrs);
	up(&inode->i_sem);

dput_and_out:
	path_release(&nd);
out:
	return error;
}

static int chown_common(struct dentry * dentry, uid_t user, gid_t group)
{
	struct inode * inode;
	int error;
	struct iattr newattrs;

	error = -ENOENT;
	if (!(inode = dentry->d_inode)) {
		printk(KERN_ERR "chown_common: NULL inode\n");
		goto out;
	}
	error = -EROFS;
	if (IS_RDONLY(inode))
		goto out;
	error = -EPERM;
	if (IS_IMMUTABLE(inode) || IS_APPEND(inode))
		goto out;
	newattrs.ia_valid =  ATTR_CTIME;
	if (user != (uid_t) -1) {
		newattrs.ia_valid |= ATTR_UID;
		newattrs.ia_uid = user;
	}
	if (group != (gid_t) -1) {
		newattrs.ia_valid |= ATTR_GID;
		newattrs.ia_gid = group;
	}
	if (!S_ISDIR(inode->i_mode))
		newattrs.ia_valid |= ATTR_KILL_SUID|ATTR_KILL_SGID;
	down(&inode->i_sem);
	error = notify_change(dentry, &newattrs);
	up(&inode->i_sem);
out:
	return error;
}

asmlinkage long sys_chown(const char __user * filename, uid_t user, gid_t group)
{
	struct nameidata nd;
	int error;

	error = user_path_walk(filename, &nd);
	if (!error) {
		error = chown_common(nd.dentry, user, group);
		path_release(&nd);
	}
	return error;
}

asmlinkage long sys_lchown(const char __user * filename, uid_t user, gid_t group)
{
	struct nameidata nd;
	int error;

	error = user_path_walk_link(filename, &nd);
	if (!error) {
		error = chown_common(nd.dentry, user, group);
		path_release(&nd);
	}
	return error;
}


asmlinkage long sys_fchown(unsigned int fd, uid_t user, gid_t group)
{
	struct file * file;
	int error = -EBADF;

	file = fget(fd);
	if (file) {
		error = chown_common(file->f_dentry, user, group);
		fput(file);
	}
	return error;
}

/*
 * Note that while the flag value (low two bits) for sys_open means:
 *	00 - read-only
 *	01 - write-only
 *	10 - read-write
 *	11 - special
 * it is changed into
 *	00 - no permissions needed
 *	01 - read-permission
 *	10 - write-permission
 *	11 - read-write
 * for the internal routines (ie open_namei()/follow_link() etc). 00 is
 * used by symlinks.
 */
struct file *filp_open(const char * filename, int flags, int mode)
{
	int namei_flags, error;
	struct nameidata nd;

	namei_flags = flags;
	if ((namei_flags+1) & O_ACCMODE)
		namei_flags++;
	if (namei_flags & O_TRUNC)
		namei_flags |= 2;

	error = open_namei(filename, namei_flags, mode, &nd);
	if (!error)
		return dentry_open(nd.dentry, nd.mnt, flags);

	return ERR_PTR(error);
}

EXPORT_SYMBOL(filp_open);

struct file *dentry_open(struct dentry *dentry, struct vfsmount *mnt, int flags)
{
	struct file * f;
	struct inode *inode;
	int error;

	error = -ENFILE;
	f = get_empty_filp();
	if (!f)
		goto cleanup_dentry;
	f->f_flags = flags;
	f->f_mode = ((flags+1) & O_ACCMODE) | FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE;
	inode = dentry->d_inode;
	if (f->f_mode & FMODE_WRITE) {
		error = get_write_access(inode);
		if (error)
			goto cleanup_file;
	}

	f->f_mapping = inode->i_mapping;
	f->f_dentry = dentry;
	f->f_vfsmnt = mnt;
	f->f_pos = 0;
	f->f_op = fops_get(inode->i_fop);
	file_move(f, &inode->i_sb->s_files);

	if (f->f_op && f->f_op->open) {
		error = f->f_op->open(inode,f);
		if (error)
			goto cleanup_all;
	}
	f->f_flags &= ~(O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC);

	file_ra_state_init(&f->f_ra, f->f_mapping->host->i_mapping);

	/* NB: we're sure to have correct a_ops only after f_op->open */
	if (f->f_flags & O_DIRECT) {
		if (!f->f_mapping->a_ops || !f->f_mapping->a_ops->direct_IO) {
			fput(f);
			f = ERR_PTR(-EINVAL);
		}
	}

	return f;

cleanup_all:
	fops_put(f->f_op);
	if (f->f_mode & FMODE_WRITE)
		put_write_access(inode);
	file_kill(f);
	f->f_dentry = NULL;
	f->f_vfsmnt = NULL;
cleanup_file:
	put_filp(f);
cleanup_dentry:
	dput(dentry);
	mntput(mnt);
	return ERR_PTR(error);
}

EXPORT_SYMBOL(dentry_open);

/*
 * Find an empty file descriptor entry, and mark it busy.
 */
int get_unused_fd(void)
{
	struct files_struct * files = current->files;
	int fd, error;

  	error = -EMFILE;
	spin_lock(&files->file_lock);

repeat:
 	fd = find_next_zero_bit(files->open_fds->fds_bits, 
				files->max_fdset, 
				files->next_fd);

	/*
	 * N.B. For clone tasks sharing a files structure, this test
	 * will limit the total number of files that can be opened.
	 */
	if (fd >= current->rlim[RLIMIT_NOFILE].rlim_cur)
		goto out;

	/* Do we need to expand the fdset array? */
	if (fd >= files->max_fdset) {
		error = expand_fdset(files, fd);
		if (!error) {
			error = -EMFILE;
			goto repeat;
		}
		goto out;
	}
	
	/* 
	 * Check whether we need to expand the fd array.
	 */
	if (fd >= files->max_fds) {
		error = expand_fd_array(files, fd);
		if (!error) {
			error = -EMFILE;
			goto repeat;
		}
		goto out;
	}

	FD_SET(fd, files->open_fds);
	FD_CLR(fd, files->close_on_exec);
	files->next_fd = fd + 1;
#if 1
	/* Sanity check */
	if (files->fd[fd] != NULL) {
		printk(KERN_WARNING "get_unused_fd: slot %d not NULL!\n", fd);
		files->fd[fd] = NULL;
	}
#endif
	error = fd;

out:
	spin_unlock(&files->file_lock);
	return error;
}

EXPORT_SYMBOL(get_unused_fd);

static inline void __put_unused_fd(struct files_struct *files, unsigned int fd)
{
	__FD_CLR(fd, files->open_fds);
	if (fd < files->next_fd)
		files->next_fd = fd;
}

void fastcall put_unused_fd(unsigned int fd)
{
	struct files_struct *files = current->files;
	spin_lock(&files->file_lock);
	__put_unused_fd(files, fd);
	spin_unlock(&files->file_lock);
}

EXPORT_SYMBOL(put_unused_fd);

/*
 * Install a file pointer in the fd array.  
 *
 * The VFS is full of places where we drop the files lock between
 * setting the open_fds bitmap and installing the file in the file
 * array.  At any such point, we are vulnerable to a dup2() race
 * installing a file in the array before us.  We need to detect this and
 * fput() the struct file we are about to overwrite in this case.
 *
 * It should never happen - if we allow dup2() do it, _really_ bad things
 * will follow.
 */

void fastcall fd_install(unsigned int fd, struct file * file)
{
	struct files_struct *files = current->files;
	spin_lock(&files->file_lock);
	if (unlikely(files->fd[fd] != NULL))
		BUG();
	files->fd[fd] = file;
	spin_unlock(&files->file_lock);
}

EXPORT_SYMBOL(fd_install);

asmlinkage long sys_open(const char __user * filename, int flags, int mode)
{
	char * tmp;
	int fd, error;

#if BITS_PER_LONG != 32
	flags |= O_LARGEFILE;
#endif
	tmp = getname(filename);
	fd = PTR_ERR(tmp);
	if (!IS_ERR(tmp)) {
		fd = get_unused_fd();
		if (fd >= 0) {
			struct file *f = filp_open(tmp, flags, mode);
			error = PTR_ERR(f);
			if (IS_ERR(f))
				goto out_error;
			fd_install(fd, f);
		}
out:
		putname(tmp);
	}
	return fd;

out_error:
	put_unused_fd(fd);
	fd = error;
	goto out;
}
EXPORT_SYMBOL_GPL(sys_open);

#ifndef __alpha__

/*
 * For backward compatibility?  Maybe this should be moved
 * into arch/i386 instead?
 */
asmlinkage long sys_creat(const char __user * pathname, int mode)
{
	return sys_open(pathname, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

#endif

/*
 * "id" is the POSIX thread ID. We use the
 * files pointer for this..
 */
int filp_close(struct file *filp, fl_owner_t id)
{
	int retval;

	/* Report and clear outstanding errors */
	retval = filp->f_error;
	if (retval)
		filp->f_error = 0;

	if (!file_count(filp)) {
		printk(KERN_ERR "VFS: Close: file count is 0\n");
		return retval;
	}

	if (filp->f_op && filp->f_op->flush) {
		int err = filp->f_op->flush(filp);
		if (!retval)
			retval = err;
	}

	dnotify_flush(filp, id);
	locks_remove_posix(filp, id);
	fput(filp);
	return retval;
}

EXPORT_SYMBOL(filp_close);

/*
 * Careful here! We test whether the file pointer is NULL before
 * releasing the fd. This ensures that one clone task can't release
 * an fd while another clone is opening it.
 */
asmlinkage long sys_close(unsigned int fd)
{
	struct file * filp;
	struct files_struct *files = current->files;

	spin_lock(&files->file_lock);
	if (fd >= files->max_fds)
		goto out_unlock;
	filp = files->fd[fd];
	if (!filp)
		goto out_unlock;
	files->fd[fd] = NULL;
	FD_CLR(fd, files->close_on_exec);
	__put_unused_fd(files, fd);
	spin_unlock(&files->file_lock);
	return filp_close(filp, files);

out_unlock:
	spin_unlock(&files->file_lock);
	return -EBADF;
}

EXPORT_SYMBOL(sys_close);

/*
 * This routine simulates a hangup on the tty, to arrange that users
 * are given clean terminals at login time.
 */
asmlinkage long sys_vhangup(void)
{
	if (capable(CAP_SYS_TTY_CONFIG)) {
		tty_vhangup(current->signal->tty);
		return 0;
	}
	return -EPERM;
}

/*
 * Called when an inode is about to be open.
 * We use this to disallow opening large files on 32bit systems if
 * the caller didn't specify O_LARGEFILE.  On 64bit systems we force
 * on this flag in sys_open.
 */
int generic_file_open(struct inode * inode, struct file * filp)
{
	if (!(filp->f_flags & O_LARGEFILE) && i_size_read(inode) > MAX_NON_LFS)
		return -EFBIG;
	return 0;
}

EXPORT_SYMBOL(generic_file_open);

/*
 * This is used by subsystems that don't want seekable
 * file descriptors
 */
int nonseekable_open(struct inode *inode, struct file *filp)
{
	filp->f_mode &= ~(FMODE_LSEEK | FMODE_PREAD | FMODE_PWRITE);
	return 0;
}

EXPORT_SYMBOL(nonseekable_open);
