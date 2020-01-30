/*
 * proc/fs/generic.c --- generic routines for the proc-fs
 *
 * This file contains generic proc-fs routines for handling
 * directories and files.
 * 
 * Copyright (C) 1991, 1992 Linus Torvalds.
 * Copyright (C) 1997 Theodore Ts'o
 */

#include <linux/errno.h>
#include <linux/time.h>
#include <linux/proc_fs.h>
#include <linux/stat.h>
#include <linux/module.h>
#include <linux/mount.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/idr.h>
#include <linux/namei.h>
#include <asm/uaccess.h>
#include <asm/bitops.h>

static ssize_t proc_file_read(struct file *file, char __user *buf,
			      size_t nbytes, loff_t *ppos);
static ssize_t proc_file_write(struct file *file, const char __user *buffer,
			       size_t count, loff_t *ppos);
static loff_t proc_file_lseek(struct file *, loff_t, int);

int proc_match(int len, const char *name, struct proc_dir_entry *de)
{
	if (de->namelen != len)
		return 0;
	return !memcmp(name, de->name, len);
}

static struct file_operations proc_file_operations = {
	.llseek		= proc_file_lseek,
	.read		= proc_file_read,
	.write		= proc_file_write,
};

/* buffer size is one page but our output routines use some slack for overruns */
#define PROC_BLOCK_SIZE	(PAGE_SIZE - 1024)

static ssize_t
proc_file_read(struct file *file, char __user *buf, size_t nbytes,
	       loff_t *ppos)
{
	struct inode * inode = file->f_dentry->d_inode;
	char 	*page;
	ssize_t	retval=0;
	int	eof=0;
	ssize_t	n, count;
	char	*start;
	struct proc_dir_entry * dp;

	dp = PDE(inode);
	if (!(page = (char*) __get_free_page(GFP_KERNEL)))
		return -ENOMEM;

	while ((nbytes > 0) && !eof) {
		count = min_t(ssize_t, PROC_BLOCK_SIZE, nbytes);

		start = NULL;
		if (dp->get_info) {
			/* Handle old net routines */
			n = dp->get_info(page, &start, *ppos, count);
			if (n < count)
				eof = 1;
		} else if (dp->read_proc) {
			/*
			 * How to be a proc read function
			 * ------------------------------
			 * Prototype:
			 *    int f(char *buffer, char **start, off_t offset,
			 *          int count, int *peof, void *dat)
			 *
			 * Assume that the buffer is "count" bytes in size.
			 *
			 * If you know you have supplied all the data you
			 * have, set *peof.
			 *
			 * You have three ways to return data:
			 * 0) Leave *start = NULL.  (This is the default.)
			 *    Put the data of the requested offset at that
			 *    offset within the buffer.  Return the number (n)
			 *    of bytes there are from the beginning of the
			 *    buffer up to the last byte of data.  If the
			 *    number of supplied bytes (= n - offset) is 
			 *    greater than zero and you didn't signal eof
			 *    and the reader is prepared to take more data
			 *    you will be called again with the requested
			 *    offset advanced by the number of bytes 
			 *    absorbed.  This interface is useful for files
			 *    no larger than the buffer.
			 * 1) Set *start = an unsigned long value less than
			 *    the buffer address but greater than zero.
			 *    Put the data of the requested offset at the
			 *    beginning of the buffer.  Return the number of
			 *    bytes of data placed there.  If this number is
			 *    greater than zero and you didn't signal eof
			 *    and the reader is prepared to take more data
			 *    you will be called again with the requested
			 *    offset advanced by *start.  This interface is
			 *    useful when you have a large file consisting
			 *    of a series of blocks which you want to count
			 *    and return as wholes.
			 *    (Hack by Paul.Russell@rustcorp.com.au)
			 * 2) Set *start = an address within the buffer.
			 *    Put the data of the requested offset at *start.
			 *    Return the number of bytes of data placed there.
			 *    If this number is greater than zero and you
			 *    didn't signal eof and the reader is prepared to
			 *    take more data you will be called again with the
			 *    requested offset advanced by the number of bytes
			 *    absorbed.
			 */
			n = dp->read_proc(page, &start, *ppos,
					  count, &eof, dp->data);
		} else
			break;

		if (n == 0)   /* end of file */
			break;
		if (n < 0) {  /* error */
			if (retval == 0)
				retval = n;
			break;
		}

		if (start == NULL) {
			if (n > PAGE_SIZE) {
				printk(KERN_ERR
				       "proc_file_read: Apparent buffer overflow!\n");
				n = PAGE_SIZE;
			}
			n -= *ppos;
			if (n <= 0)
				break;
			if (n > count)
				n = count;
			start = page + *ppos;
		} else if (start < page) {
			if (n > PAGE_SIZE) {
				printk(KERN_ERR
				       "proc_file_read: Apparent buffer overflow!\n");
				n = PAGE_SIZE;
			}
			if (n > count) {
				/*
				 * Don't reduce n because doing so might
				 * cut off part of a data block.
				 */
				printk(KERN_WARNING
				       "proc_file_read: Read count exceeded\n");
			}
		} else /* start >= page */ {
			unsigned long startoff = (unsigned long)(start - page);
			if (n > (PAGE_SIZE - startoff)) {
				printk(KERN_ERR
				       "proc_file_read: Apparent buffer overflow!\n");
				n = PAGE_SIZE - startoff;
			}
			if (n > count)
				n = count;
		}
		
 		n -= copy_to_user(buf, start < page ? page : start, n);
		if (n == 0) {
			if (retval == 0)
				retval = -EFAULT;
			break;
		}

		*ppos += start < page ? (unsigned long)start : n;
		nbytes -= n;
		buf += n;
		retval += n;
	}
	free_page((unsigned long) page);
	return retval;
}

static ssize_t
proc_file_write(struct file *file, const char __user *buffer,
		size_t count, loff_t *ppos)
{
	struct inode *inode = file->f_dentry->d_inode;
	struct proc_dir_entry * dp;
	
	dp = PDE(inode);

	if (!dp->write_proc)
		return -EIO;

	/* FIXME: does this routine need ppos?  probably... */
	return dp->write_proc(file, buffer, count, dp->data);
}


static loff_t
proc_file_lseek(struct file *file, loff_t offset, int orig)
{
    lock_kernel();

    switch (orig) {
    case 0:
	if (offset < 0)
	    goto out;
	file->f_pos = offset;
	unlock_kernel();
	return(file->f_pos);
    case 1:
	if (offset + file->f_pos < 0)
	    goto out;
	file->f_pos += offset;
	unlock_kernel();
	return(file->f_pos);
    case 2:
	goto out;
    default:
	goto out;
    }

out:
    unlock_kernel();
    return -EINVAL;
}

static int proc_notify_change(struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = dentry->d_inode;
	struct proc_dir_entry *de = PDE(inode);
	int error;

	error = inode_change_ok(inode, iattr);
	if (error)
		goto out;

	error = inode_setattr(inode, iattr);
	if (error)
		goto out;
	
	de->uid = inode->i_uid;
	de->gid = inode->i_gid;
	de->mode = inode->i_mode;
out:
	return error;
}

static struct inode_operations proc_file_inode_operations = {
	.setattr	= proc_notify_change,
};

/*
 * This function parses a name such as "tty/driver/serial", and
 * returns the struct proc_dir_entry for "/proc/tty/driver", and
 * returns "serial" in residual.
 */
static int xlate_proc_name(const char *name,
			   struct proc_dir_entry **ret, const char **residual)
{
	const char     		*cp = name, *next;
	struct proc_dir_entry	*de;
	int			len;

	de = &proc_root;
	while (1) {
		next = strchr(cp, '/');
		if (!next)
			break;

		len = next - cp;
		for (de = de->subdir; de ; de = de->next) {
			if (proc_match(len, cp, de))
				break;
		}
		if (!de)
			return -ENOENT;
		cp += len + 1;
	}
	*residual = cp;
	*ret = de;
	return 0;
}

static DEFINE_IDR(proc_inum_idr);
static spinlock_t proc_inum_lock = SPIN_LOCK_UNLOCKED; /* protects the above */

#define PROC_DYNAMIC_FIRST 0xF0000000UL

/*
 * Return an inode number between PROC_DYNAMIC_FIRST and
 * 0xffffffff, or zero on failure.
 */
static unsigned int get_inode_number(void)
{
	int i, inum = 0;
	int error;

retry:
	if (idr_pre_get(&proc_inum_idr, GFP_KERNEL) == 0)
		return 0;

	spin_lock(&proc_inum_lock);
	error = idr_get_new(&proc_inum_idr, NULL, &i);
	spin_unlock(&proc_inum_lock);
	if (error == -EAGAIN)
		goto retry;
	else if (error)
		return 0;

	inum = (i & MAX_ID_MASK) + PROC_DYNAMIC_FIRST;

	/* inum will never be more than 0xf0ffffff, so no check
	 * for overflow.
	 */

	return inum;
}

static void release_inode_number(unsigned int inum)
{
	int id = (inum - PROC_DYNAMIC_FIRST) | ~MAX_ID_MASK;

	spin_lock(&proc_inum_lock);
	idr_remove(&proc_inum_idr, id);
	spin_unlock(&proc_inum_lock);
}

static int proc_follow_link(struct dentry *dentry, struct nameidata *nd)
{
	nd_set_link(nd, PDE(dentry->d_inode)->data);
	return 0;
}

static struct inode_operations proc_link_inode_operations = {
	.readlink	= generic_readlink,
	.follow_link	= proc_follow_link,
};

/*
 * As some entries in /proc are volatile, we want to 
 * get rid of unused dentries.  This could be made 
 * smarter: we could keep a "volatile" flag in the 
 * inode to indicate which ones to keep.
 */
static int proc_delete_dentry(struct dentry * dentry)
{
	return 1;
}

static struct dentry_operations proc_dentry_operations =
{
	.d_delete	= proc_delete_dentry,
};

/*
 * Don't create negative dentries here, return -ENOENT by hand
 * instead.
 */
struct dentry *proc_lookup(struct inode * dir, struct dentry *dentry, struct nameidata *nd)
{
	struct inode *inode = NULL;
	struct proc_dir_entry * de;
	int error = -ENOENT;

	lock_kernel();
	de = PDE(dir);
	if (de) {
		for (de = de->subdir; de ; de = de->next) {
			if (de->namelen != dentry->d_name.len)
				continue;
			if (!memcmp(dentry->d_name.name, de->name, de->namelen)) {
				unsigned int ino = de->low_ino;

				error = -EINVAL;
				inode = proc_get_inode(dir->i_sb, ino, de);
				break;
			}
		}
	}
	unlock_kernel();

	if (inode) {
		dentry->d_op = &proc_dentry_operations;
		d_add(dentry, inode);
		return NULL;
	}
	return ERR_PTR(error);
}

/*
 * This returns non-zero if at EOF, so that the /proc
 * root directory can use this and check if it should
 * continue with the <pid> entries..
 *
 * Note that the VFS-layer doesn't care about the return
 * value of the readdir() call, as long as it's non-negative
 * for success..
 */
int proc_readdir(struct file * filp,
	void * dirent, filldir_t filldir)
{
	struct proc_dir_entry * de;
	unsigned int ino;
	int i;
	struct inode *inode = filp->f_dentry->d_inode;
	int ret = 0;

	lock_kernel();

	ino = inode->i_ino;
	de = PDE(inode);
	if (!de) {
		ret = -EINVAL;
		goto out;
	}
	i = filp->f_pos;
	switch (i) {
		case 0:
			if (filldir(dirent, ".", 1, i, ino, DT_DIR) < 0)
				goto out;
			i++;
			filp->f_pos++;
			/* fall through */
		case 1:
			if (filldir(dirent, "..", 2, i,
				    parent_ino(filp->f_dentry),
				    DT_DIR) < 0)
				goto out;
			i++;
			filp->f_pos++;
			/* fall through */
		default:
			de = de->subdir;
			i -= 2;
			for (;;) {
				if (!de) {
					ret = 1;
					goto out;
				}
				if (!i)
					break;
				de = de->next;
				i--;
			}

			do {
				if (filldir(dirent, de->name, de->namelen, filp->f_pos,
					    de->low_ino, de->mode >> 12) < 0)
					goto out;
				filp->f_pos++;
				de = de->next;
			} while (de);
	}
	ret = 1;
out:	unlock_kernel();
	return ret;	
}

/*
 * These are the generic /proc directory operations. They
 * use the in-memory "struct proc_dir_entry" tree to parse
 * the /proc directory.
 */
static struct file_operations proc_dir_operations = {
	.read			= generic_read_dir,
	.readdir		= proc_readdir,
};

/*
 * proc directories can do almost nothing..
 */
static struct inode_operations proc_dir_inode_operations = {
	.lookup		= proc_lookup,
	.setattr	= proc_notify_change,
};

static int proc_register(struct proc_dir_entry * dir, struct proc_dir_entry * dp)
{
	unsigned int i;
	
	i = get_inode_number();
	if (i == 0)
		return -EAGAIN;
	dp->low_ino = i;
	dp->next = dir->subdir;
	dp->parent = dir;
	dir->subdir = dp;
	if (S_ISDIR(dp->mode)) {
		if (dp->proc_iops == NULL) {
			dp->proc_fops = &proc_dir_operations;
			dp->proc_iops = &proc_dir_inode_operations;
		}
		dir->nlink++;
	} else if (S_ISLNK(dp->mode)) {
		if (dp->proc_iops == NULL)
			dp->proc_iops = &proc_link_inode_operations;
	} else if (S_ISREG(dp->mode)) {
		if (dp->proc_fops == NULL)
			dp->proc_fops = &proc_file_operations;
		if (dp->proc_iops == NULL)
			dp->proc_iops = &proc_file_inode_operations;
	}
	return 0;
}

/*
 * Kill an inode that got unregistered..
 */
static void proc_kill_inodes(struct proc_dir_entry *de)
{
	struct list_head *p;
	struct super_block *sb = proc_mnt->mnt_sb;

	/*
	 * Actually it's a partial revoke().
	 */
	file_list_lock();
	list_for_each(p, &sb->s_files) {
		struct file * filp = list_entry(p, struct file, f_list);
		struct dentry * dentry = filp->f_dentry;
		struct inode * inode;
		struct file_operations *fops;

		if (dentry->d_op != &proc_dentry_operations)
			continue;
		inode = dentry->d_inode;
		if (PDE(inode) != de)
			continue;
		fops = filp->f_op;
		filp->f_op = NULL;
		fops_put(fops);
	}
	file_list_unlock();
}

static struct proc_dir_entry *proc_create(struct proc_dir_entry **parent,
					  const char *name,
					  mode_t mode,
					  nlink_t nlink)
{
	struct proc_dir_entry *ent = NULL;
	const char *fn = name;
	int len;

	/* make sure name is valid */
	if (!name || !strlen(name)) goto out;

	if (!(*parent) && xlate_proc_name(name, parent, &fn) != 0)
		goto out;
	len = strlen(fn);

	ent = kmalloc(sizeof(struct proc_dir_entry) + len + 1, GFP_KERNEL);
	if (!ent) goto out;

	memset(ent, 0, sizeof(struct proc_dir_entry));
	memcpy(((char *) ent) + sizeof(struct proc_dir_entry), fn, len + 1);
	ent->name = ((char *) ent) + sizeof(*ent);
	ent->namelen = len;
	ent->mode = mode;
	ent->nlink = nlink;
 out:
	return ent;
}

struct proc_dir_entry *proc_symlink(const char *name,
		struct proc_dir_entry *parent, const char *dest)
{
	struct proc_dir_entry *ent;

	ent = proc_create(&parent,name,
			  (S_IFLNK | S_IRUGO | S_IWUGO | S_IXUGO),1);

	if (ent) {
		ent->data = kmalloc((ent->size=strlen(dest))+1, GFP_KERNEL);
		if (ent->data) {
			strcpy((char*)ent->data,dest);
			if (proc_register(parent, ent) < 0) {
				kfree(ent->data);
				kfree(ent);
				ent = NULL;
			}
		} else {
			kfree(ent);
			ent = NULL;
		}
	}
	return ent;
}

struct proc_dir_entry *proc_mkdir_mode(const char *name, mode_t mode,
		struct proc_dir_entry *parent)
{
	struct proc_dir_entry *ent;

	ent = proc_create(&parent, name, S_IFDIR | mode, 2);
	if (ent) {
		ent->proc_fops = &proc_dir_operations;
		ent->proc_iops = &proc_dir_inode_operations;

		if (proc_register(parent, ent) < 0) {
			kfree(ent);
			ent = NULL;
		}
	}
	return ent;
}

struct proc_dir_entry *proc_mkdir(const char *name,
		struct proc_dir_entry *parent)
{
	return proc_mkdir_mode(name, S_IRUGO | S_IXUGO, parent);
}

struct proc_dir_entry *create_proc_entry(const char *name, mode_t mode,
					 struct proc_dir_entry *parent)
{
	struct proc_dir_entry *ent;
	nlink_t nlink;

	if (S_ISDIR(mode)) {
		if ((mode & S_IALLUGO) == 0)
			mode |= S_IRUGO | S_IXUGO;
		nlink = 2;
	} else {
		if ((mode & S_IFMT) == 0)
			mode |= S_IFREG;
		if ((mode & S_IALLUGO) == 0)
			mode |= S_IRUGO;
		nlink = 1;
	}

	ent = proc_create(&parent,name,mode,nlink);
	if (ent) {
		if (S_ISDIR(mode)) {
			ent->proc_fops = &proc_dir_operations;
			ent->proc_iops = &proc_dir_inode_operations;
		}
		if (proc_register(parent, ent) < 0) {
			kfree(ent);
			ent = NULL;
		}
	}
	return ent;
}

void free_proc_entry(struct proc_dir_entry *de)
{
	unsigned int ino = de->low_ino;

	if (ino < PROC_DYNAMIC_FIRST)
		return;

	release_inode_number(ino);

	if (S_ISLNK(de->mode) && de->data)
		kfree(de->data);
	kfree(de);
}

/*
 * Remove a /proc entry and free it if it's not currently in use.
 * If it is in use, we set the 'deleted' flag.
 */
void remove_proc_entry(const char *name, struct proc_dir_entry *parent)
{
	struct proc_dir_entry **p;
	struct proc_dir_entry *de;
	const char *fn = name;
	int len;

	if (!parent && xlate_proc_name(name, &parent, &fn) != 0)
		goto out;
	len = strlen(fn);
	for (p = &parent->subdir; *p; p=&(*p)->next ) {
		if (!proc_match(len, fn, *p))
			continue;
		de = *p;
		*p = de->next;
		de->next = NULL;
		if (S_ISDIR(de->mode))
			parent->nlink--;
		proc_kill_inodes(de);
		de->nlink = 0;
		WARN_ON(de->subdir);
		if (!atomic_read(&de->count))
			free_proc_entry(de);
		else {
			de->deleted = 1;
			printk("remove_proc_entry: %s/%s busy, count=%d\n",
				parent->name, de->name, atomic_read(&de->count));
		}
		break;
	}
out:
	return;
}
