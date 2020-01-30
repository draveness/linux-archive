/*
 * JFFS -- Journalling Flash File System, Linux implementation.
 *
 * Copyright (C) 1999, 2000  Axis Communications AB.
 *
 * Created by Finn Hakansson <finn@axis.com>.
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * $Id: inode-v23.c,v 1.43 2000/08/22 08:00:22 dwmw2 Exp $
 *
 *
 * Ported to Linux 2.3.x and MTD:
 * Copyright (C) 2000  Alexander Larsson (alex@cendio.se), Cendio Systems AB
 *
 */

/* inode.c -- Contains the code that is called from the VFS.  */

/* TODO-ALEX:
 * uid and gid are just 16 bit.
 * jffs_file_write reads from user-space pointers without xx_from_user
 * maybe other stuff do to.
 */

/* Argh. Some architectures have kernel_thread in asm/processor.h
   Some have it in unistd.h and you need to define __KERNEL_SYSCALLS__
   Pass me a baseball bat and the person responsible.
   dwmw2
*/
#define __KERNEL_SYSCALLS__
#include <linux/sched.h>
#include <linux/unistd.h>

#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/malloc.h>
#include <linux/jffs.h>
#include <linux/fs.h>
#include <linux/locks.h>
#include <linux/smp_lock.h>
#include <linux/ioctl.h>
#include <linux/stat.h>
#include <linux/blkdev.h>
#include <linux/quotaops.h>
#include <asm/semaphore.h>
#include <asm/byteorder.h>
#include <asm/uaccess.h>
#include "jffs_fm.h"
#include "intrep.h"

static int jffs_remove(struct inode *dir, struct dentry *dentry, int type);

static struct super_operations jffs_ops;
static struct file_operations jffs_file_operations;
static struct inode_operations jffs_file_inode_operations;
static struct file_operations jffs_dir_operations;
static struct inode_operations jffs_dir_inode_operations;
static struct address_space_operations jffs_address_operations;


/* Called by the VFS at mount time to initialize the whole file system.  */
static struct super_block *
jffs_read_super(struct super_block *sb, void *data, int silent)
{
	kdev_t dev = sb->s_dev;
	struct inode *root_inode;
	struct jffs_control *c;

	D1(printk(KERN_NOTICE "JFFS: Trying to mount device %s.\n",
		  kdevname(dev)));

	if (MAJOR(dev) != MTD_BLOCK_MAJOR) {
		printk(KERN_WARNING "JFFS: Trying to mount a "
		       "non-mtd device.\n");
		return 0;
	}

	sb->s_blocksize = PAGE_CACHE_SIZE;
	sb->s_blocksize_bits = PAGE_CACHE_SHIFT;
	sb->u.generic_sbp = (void *) 0;

	/* Build the file system.  */
	if (jffs_build_fs(sb) < 0) {
		goto jffs_sb_err1;
	}

	/*
	 * set up enough so that we can read an inode
	 */
	sb->s_magic = JFFS_MAGIC_SB_BITMASK;
	sb->s_op = &jffs_ops;

	root_inode = iget(sb, JFFS_MIN_INO);
	if (!root_inode)
	        goto jffs_sb_err2;

	/* Get the root directory of this file system.  */
	if (!(sb->s_root = d_alloc_root(root_inode))) {
		goto jffs_sb_err3;
	}

	c = (struct jffs_control *) sb->u.generic_sbp;

	/* Set the Garbage Collection thresholds */

	/* GC if free space goes below 5% of the total size */
	c->gc_minfree_threshold = c->fmc->flash_size / 20;

	if (c->gc_minfree_threshold < c->fmc->sector_size)
		c->gc_minfree_threshold = c->fmc->sector_size;

	/* GC if dirty space exceeds 33% of the total size. */
	c->gc_maxdirty_threshold = c->fmc->flash_size / 3;

	if (c->gc_maxdirty_threshold < c->fmc->sector_size)
		c->gc_maxdirty_threshold = c->fmc->sector_size;


	c->thread_pid = kernel_thread (jffs_garbage_collect_thread, 
				        (void *) c, 
				        CLONE_FS | CLONE_FILES | CLONE_SIGHAND);
	D1(printk(KERN_NOTICE "JFFS: GC thread pid=%d.\n", (int) c->thread_pid));

	D1(printk(KERN_NOTICE "JFFS: Successfully mounted device %s.\n",
	       kdevname(dev)));
	return sb;

jffs_sb_err3:
	iput(root_inode);
jffs_sb_err2:
	jffs_cleanup_control((struct jffs_control *)sb->u.generic_sbp);
jffs_sb_err1:
	printk(KERN_WARNING "JFFS: Failed to mount device %s.\n",
	       kdevname(dev));
	return 0;
}


/* This function is called when the file system is umounted.  */
static void
jffs_put_super(struct super_block *sb)
{
	struct jffs_control *c = (struct jffs_control *) sb->u.generic_sbp;
	D1(kdev_t dev = sb->s_dev);

	D2(printk("jffs_put_super()\n"));

	if (c->gc_task) {
		D1(printk (KERN_NOTICE "jffs_put_super(): Telling gc thread to die.\n"));
		send_sig(SIGKILL, c->gc_task, 1);
	}
	down (&c->gc_thread_sem);

	D1(printk (KERN_NOTICE "jffs_put_super(): Successfully waited on thread.\n"));

	sb->s_dev = 0;
	jffs_cleanup_control((struct jffs_control *)sb->u.generic_sbp);
	D1(printk(KERN_NOTICE "JFFS: Successfully unmounted device %s.\n",
	       kdevname(dev)));
}


/* This function is called when user commands like chmod, chgrp and
   chown are executed. System calls like trunc() results in a call
   to this function.  */
static int
jffs_setattr(struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = dentry->d_inode;
	struct jffs_raw_inode raw_inode;
	struct jffs_control *c;
	struct jffs_fmcontrol *fmc;
	struct jffs_file *f;
	struct jffs_node *new_node;
	int update_all;
	int res;
	int recoverable = 0;

	if ((res = inode_change_ok(inode, iattr)))
		return res;

	c = (struct jffs_control *)inode->i_sb->u.generic_sbp;
	fmc = c->fmc;

	D3(printk (KERN_NOTICE "notify_change(): down biglock\n"));
	down(&fmc->biglock);

	f = jffs_find_file(c, inode->i_ino);

	ASSERT(if (!f) {
		printk("jffs_setattr(): Invalid inode number: %lu\n",
		       inode->i_ino);
		D3(printk (KERN_NOTICE "notify_change(): up biglock\n"));
		up(&fmc->biglock);
		return -EINVAL;
	});

	D1(printk("***jffs_setattr(): file: \"%s\", ino: %u\n",
		  f->name, f->ino));

	update_all = iattr->ia_valid & ATTR_FORCE;

	if ( (update_all || iattr->ia_valid & ATTR_SIZE)
	     && (iattr->ia_size + 128 < f->size) ) {
		/* We're shrinking the file by more than 128 bytes.
		   We'll be able to GC and recover this space, so
		   allow it to go into the reserved space. */
		recoverable = 1;
        }

	if (!(new_node = (struct jffs_node *)
			 kmalloc(sizeof(struct jffs_node), GFP_KERNEL))) {
		D(printk("jffs_setattr(): Allocation failed!\n"));
		D3(printk (KERN_NOTICE "notify_change(): up biglock\n"));
		up(&fmc->biglock);
		return -ENOMEM;
	}

	DJM(no_jffs_node++);
	new_node->data_offset = 0;
	new_node->removed_size = 0;
	raw_inode.magic = JFFS_MAGIC_BITMASK;
	raw_inode.ino = f->ino;
	raw_inode.pino = f->pino;
	raw_inode.mode = f->mode;
	raw_inode.uid = f->uid;
	raw_inode.gid = f->gid;
	raw_inode.atime = f->atime;
	raw_inode.mtime = f->mtime;
	raw_inode.ctime = f->ctime;
	raw_inode.dsize = 0;
	raw_inode.offset = 0;
	raw_inode.rsize = 0;
	raw_inode.dsize = 0;
	raw_inode.nsize = f->nsize;
	raw_inode.nlink = f->nlink;
	raw_inode.spare = 0;
	raw_inode.rename = 0;
	raw_inode.deleted = 0;

	if (update_all || iattr->ia_valid & ATTR_MODE) {
		raw_inode.mode = iattr->ia_mode;
		inode->i_mode = iattr->ia_mode;
	}
	if (update_all || iattr->ia_valid & ATTR_UID) {
		raw_inode.uid = iattr->ia_uid;
		inode->i_uid = iattr->ia_uid;
	}
	if (update_all || iattr->ia_valid & ATTR_GID) {
		raw_inode.gid = iattr->ia_gid;
		inode->i_gid = iattr->ia_gid;
	}
	if (update_all || iattr->ia_valid & ATTR_SIZE) {
		int len;
		D1(printk("jffs_notify_change(): Changing size "
			  "to %lu bytes!\n", (long)iattr->ia_size));
		raw_inode.offset = iattr->ia_size;

		/* Calculate how many bytes need to be removed from
		   the end.  */
		if (f->size < iattr->ia_size) {
			len = 0;
		}
		else {
			len = f->size - iattr->ia_size;
		}

		raw_inode.rsize = len;

		/* The updated node will be a removal node, with
		   base at the new size and size of the nbr of bytes
		   to be removed.  */
		new_node->data_offset = iattr->ia_size;
		new_node->removed_size = len;
		inode->i_size = iattr->ia_size;
		inode->i_blocks = (inode->i_size + 511) >> 9;

		if (len) {
			invalidate_inode_pages(inode);
		}
		inode->i_ctime = CURRENT_TIME;
		inode->i_mtime = inode->i_ctime;
	}
	if (update_all || iattr->ia_valid & ATTR_ATIME) {
		raw_inode.atime = iattr->ia_atime;
		inode->i_atime = iattr->ia_atime;
	}
	if (update_all || iattr->ia_valid & ATTR_MTIME) {
		raw_inode.mtime = iattr->ia_mtime;
		inode->i_mtime = iattr->ia_mtime;
	}
	if (update_all || iattr->ia_valid & ATTR_CTIME) {
		raw_inode.ctime = iattr->ia_ctime;
		inode->i_ctime = iattr->ia_ctime;
	}

	/* Write this node to the flash.  */
	if ((res = jffs_write_node(c, new_node, &raw_inode, f->name, 0, recoverable, f)) < 0) {
		D(printk("jffs_notify_change(): The write failed!\n"));
		kfree(new_node);
		DJM(no_jffs_node--);
		D3(printk (KERN_NOTICE "n_c(): up biglock\n"));
		up(&c->fmc->biglock);
		return res;
	}

	jffs_insert_node(c, f, &raw_inode, 0, new_node);

	mark_inode_dirty(inode);
	D3(printk (KERN_NOTICE "n_c(): up biglock\n"));
	up(&c->fmc->biglock);
	return 0;
} /* jffs_notify_change()  */


struct inode *
jffs_new_inode(const struct inode * dir, struct jffs_raw_inode *raw_inode,
	       int * err)
{
	struct super_block * sb;
	struct inode * inode;
	struct jffs_control *c;

	sb = dir->i_sb;
	inode = new_inode(sb);
	if (!inode) {
		*err = -ENOMEM;
		return NULL;
	}

	c = (struct jffs_control *)sb->u.generic_sbp;

	inode->i_ino = raw_inode->ino;
	inode->i_mode = raw_inode->mode;
	inode->i_nlink = raw_inode->nlink;
	inode->i_uid = raw_inode->uid;
	inode->i_gid = raw_inode->gid;
	inode->i_rdev = 0;
	inode->i_size = raw_inode->dsize;
	inode->i_atime = raw_inode->atime;
	inode->i_mtime = raw_inode->mtime;
	inode->i_ctime = raw_inode->ctime;
	inode->i_blksize = PAGE_SIZE;
	inode->i_blocks = (inode->i_size + 511) >> 9;
	inode->i_version = 0;
	inode->u.generic_ip = (void *)jffs_find_file(c, raw_inode->ino);

	insert_inode_hash(inode);

	return inode;
}

/* Get statistics of the file system.  */
int
jffs_statfs(struct super_block *sb, struct statfs *buf)
{
	struct jffs_control *c = (struct jffs_control *) sb->u.generic_sbp;
	struct jffs_fmcontrol *fmc = c->fmc;

	D2(printk("jffs_statfs()\n"));

	buf->f_type = JFFS_MAGIC_SB_BITMASK;
	buf->f_bsize = PAGE_CACHE_SIZE;
	buf->f_blocks = (fmc->flash_size / PAGE_CACHE_SIZE)
		       - (fmc->min_free_size / PAGE_CACHE_SIZE);
	buf->f_bfree = (jffs_free_size1(fmc) / PAGE_CACHE_SIZE
		       + jffs_free_size2(fmc) / PAGE_CACHE_SIZE)
		      - (fmc->min_free_size / PAGE_CACHE_SIZE);
	buf->f_bavail = buf->f_bfree;

	/* Find out how many files there are in the filesystem.  */
	buf->f_files = jffs_foreach_file(c, jffs_file_count);
	buf->f_ffree = buf->f_bfree;
	/* buf->f_fsid = 0; */
	buf->f_namelen = JFFS_MAX_NAME_LEN;
	return 0;
}


/* Rename a file.  */
int
jffs_rename(struct inode *old_dir, struct dentry *old_dentry,
	    struct inode *new_dir, struct dentry *new_dentry)
{
	struct jffs_raw_inode raw_inode;
	struct jffs_control *c;
	struct jffs_file *old_dir_f;
	struct jffs_file *new_dir_f;
	struct jffs_file *del_f;
	struct jffs_file *f;
	struct jffs_node *node;
	struct inode *inode;
	int result = 0;
	__u32 rename_data = 0;

	D2(printk("***jffs_rename()\n"));
	
	D(printk("jffs_rename(): old_dir: 0x%p, old name: 0x%p, "
		 "new_dir: 0x%p, new name: 0x%p\n",
		 old_dir, old_dentry->d_name.name,
		 new_dir, new_dentry->d_name.name));

	c = (struct jffs_control *)old_dir->i_sb->u.generic_sbp;
	ASSERT(if (!c) {
		printk(KERN_ERR "jffs_rename(): The old_dir inode "
		       "didn't have a reference to a jffs_file struct\n");
		return -EIO;
	});

	result = -ENOTDIR;
	if (!(old_dir_f = (struct jffs_file *)old_dir->u.generic_ip)) {
		D(printk("jffs_rename(): Old dir invalid.\n"));
		goto jffs_rename_end;
	}

	/* Try to find the file to move.  */
	result = -ENOENT;
	if (!(f = jffs_find_child(old_dir_f, old_dentry->d_name.name,
				  old_dentry->d_name.len))) {
		goto jffs_rename_end;
	}

	/* Find the new directory.  */
	result = -ENOTDIR;
	if (!(new_dir_f = (struct jffs_file *)new_dir->u.generic_ip)) {
		D(printk("jffs_rename(): New dir invalid.\n"));
		goto jffs_rename_end;
	}
	D3(printk (KERN_NOTICE "rename(): down biglock\n"));
	down(&c->fmc->biglock);
	/* Create a node and initialize as much as needed.  */
	result = -ENOMEM;
	if (!(node = (struct jffs_node *) kmalloc(sizeof(struct jffs_node),
						  GFP_KERNEL))) {
		D(printk("jffs_rename(): Allocation failed: node == 0\n"));
		goto jffs_rename_end;
	}
	DJM(no_jffs_node++);
	node->data_offset = 0;
	node->removed_size = 0;

	/* Initialize the raw inode.  */
	raw_inode.magic = JFFS_MAGIC_BITMASK;
	raw_inode.ino = f->ino;
	raw_inode.pino = new_dir_f->ino;
/*  	raw_inode.version = f->highest_version + 1; */
	raw_inode.mode = f->mode;
	raw_inode.uid = current->fsuid;
	raw_inode.gid = current->fsgid;
#if 0
	raw_inode.uid = f->uid;
	raw_inode.gid = f->gid;
#endif
	raw_inode.atime = CURRENT_TIME;
	raw_inode.mtime = raw_inode.atime;
	raw_inode.ctime = f->ctime;
	raw_inode.offset = 0;
	raw_inode.dsize = 0;
	raw_inode.rsize = 0;
	raw_inode.nsize = new_dentry->d_name.len;
	raw_inode.nlink = f->nlink;
	raw_inode.spare = 0;
	raw_inode.rename = 0;
	raw_inode.deleted = 0;

	/* See if there already exists a file with the same name as
	   new_name.  */
	if ((del_f = jffs_find_child(new_dir_f, new_dentry->d_name.name,
				     new_dentry->d_name.len))) {
		raw_inode.rename = 1;
		raw_inode.dsize = sizeof(__u32);
		rename_data = del_f->ino;
	}

	/* Write the new node to the flash memory.  */
	if ((result = jffs_write_node(c, node, &raw_inode,
				      new_dentry->d_name.name,
				      (unsigned char*)&rename_data, 0, f)) < 0) {
		D(printk("jffs_rename(): Failed to write node to flash.\n"));
		kfree(node);
		DJM(no_jffs_node--);
		goto jffs_rename_end;
	}
	raw_inode.dsize = 0;

	if (raw_inode.rename) {
		/* The file with the same name must be deleted.  */
		//FIXME deadlock	        down(&c->fmc->gclock);
		if ((result = jffs_remove(new_dir, new_dentry,
					  del_f->mode)) < 0) {
			/* This is really bad.  */
			printk(KERN_ERR "JFFS: An error occurred in "
			       "rename().\n");
		}
		//		up(&c->fmc->gclock);
	}

	if (old_dir_f != new_dir_f) {
		/* Remove the file from its old position in the
		   filesystem tree.  */
		jffs_unlink_file_from_tree(f);
	}

	/* Insert the new node into the file system.  */
	if ((result = jffs_insert_node(c, f, &raw_inode,
				       new_dentry->d_name.name, node)) < 0) {
		D(printk(KERN_ERR "jffs_rename(): jffs_insert_node() "
			 "failed!\n"));
	}

	if (old_dir_f != new_dir_f) {
		/* Insert the file to its new position in the
		   file system.  */
		jffs_insert_file_into_tree(f);
	}

	/* This is a kind of update of the inode we're about to make
	   here.  This is what they do in ext2fs.  Kind of.  */
	if ((inode = iget(new_dir->i_sb, f->ino))) {
		inode->i_ctime = CURRENT_TIME;
		mark_inode_dirty(inode);
		iput(inode);
	}

jffs_rename_end:
	D3(printk (KERN_NOTICE "rename(): up biglock\n"));
	up(&c->fmc->biglock);
	return result;
} /* jffs_rename()  */


/* Read the contents of a directory.  Used by programs like `ls'
   for instance.  */
static int
jffs_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct jffs_file *f;
	struct dentry *dentry = filp->f_dentry;
	struct inode *inode = dentry->d_inode;
	struct jffs_control *c = (struct jffs_control *)inode->i_sb->u.generic_sbp;
	int j;
	int ddino;
	D3(printk (KERN_NOTICE "readdir(): down biglock\n"));
	down(&c->fmc->biglock);

	D2(printk("jffs_readdir(): inode: 0x%p, filp: 0x%p\n", inode, filp));
	if (filp->f_pos == 0) {
		D3(printk("jffs_readdir(): \".\" %lu\n", inode->i_ino));
		if (filldir(dirent, ".", 1, filp->f_pos, inode->i_ino, DT_DIR) < 0) {
		  D3(printk (KERN_NOTICE "readdir(): up biglock\n"));
			up(&c->fmc->biglock);
			return 0;
		}
		filp->f_pos = 1;
	}
	if (filp->f_pos == 1) {
		if (inode->i_ino == JFFS_MIN_INO) {
			ddino = JFFS_MIN_INO;
		}
		else {
			ddino = ((struct jffs_file *)
				 inode->u.generic_ip)->pino;
		}
		D3(printk("jffs_readdir(): \"..\" %u\n", ddino));
		if (filldir(dirent, "..", 2, filp->f_pos, ddino, DT_DIR) < 0) {
		  D3(printk (KERN_NOTICE "readdir(): up biglock\n"));
			up(&c->fmc->biglock);
			return 0;
		}
		filp->f_pos++;
	}
	f = ((struct jffs_file *)inode->u.generic_ip)->children;
	for (j = 2; (j < filp->f_pos) && f; j++) {
	        f = f->sibling_next;
	}
	for (; f ; f = f->sibling_next) {
		D3(printk("jffs_readdir(): \"%s\" ino: %u\n",
			  (f->name ? f->name : ""), f->ino));
		if (filldir(dirent, f->name, f->nsize,
			    filp->f_pos , f->ino, DT_UNKNOWN) < 0) {
		        D3(printk (KERN_NOTICE "readdir(): up biglock\n"));
			up(&c->fmc->biglock);
			return 0;
		}
		filp->f_pos++;
	}
	D3(printk (KERN_NOTICE "readdir(): up biglock\n"));
	up(&c->fmc->biglock);
	return filp->f_pos;
} /* jffs_readdir()  */


/* Find a file in a directory. If the file exists, return its
   corresponding dentry.  */
static struct dentry *
jffs_lookup(struct inode *dir, struct dentry *dentry)
{
	struct jffs_file *d;
	struct jffs_file *f;
	struct jffs_control *c = (struct jffs_control *)dir->i_sb->u.generic_sbp;
	int len;
	int r = 0;
	const char *name;
	struct inode *inode = NULL;

	len = dentry->d_name.len;
	name = dentry->d_name.name;

	D3({
		char *s = (char *)kmalloc(len + 1, GFP_KERNEL);
		memcpy(s, name, len);
		s[len] = '\0';
		printk("jffs_lookup(): dir: 0x%p, name: \"%s\"\n", dir, s);
		kfree(s);
	});

	D3(printk (KERN_NOTICE "lookup(): down biglock\n"));
	down(&c->fmc->biglock);

	r = -ENAMETOOLONG;
	if (len > JFFS_MAX_NAME_LEN) {
		goto jffs_lookup_end;
	}

	r = -EACCES;
	if (!(d = (struct jffs_file *)dir->u.generic_ip)) {
		D(printk("jffs_lookup(): No such inode! (%lu)\n",
			 dir->i_ino));
		goto jffs_lookup_end;
	}

	/* Get the corresponding inode to the file.  */

	/* iget calls jffs_read_inode, so we need to drop the biglock
           before calling iget.  Unfortunately, the GC has a tendency
           to sneak in here, because iget sometimes calls schedule (). 
         */

	if ((len == 1) && (name[0] == '.')) {
                D3(printk (KERN_NOTICE "lookup(): up biglock\n"));
		up(&c->fmc->biglock);
		if (!(inode = iget(dir->i_sb, d->ino))) {
			D(printk("jffs_lookup(): . iget() ==> NULL\n"));
			goto jffs_lookup_end_no_biglock;
		}
		D3(printk (KERN_NOTICE "lookup(): down biglock\n"));
		down(&c->fmc->biglock);
	} else if ((len == 2) && (name[0] == '.') && (name[1] == '.')) {
	        D3(printk (KERN_NOTICE "lookup(): up biglock\n"));
		up(&c->fmc->biglock);
 		if (!(inode = iget(dir->i_sb, d->pino))) {
			D(printk("jffs_lookup(): .. iget() ==> NULL\n"));
			goto jffs_lookup_end_no_biglock;
		}
		D3(printk (KERN_NOTICE "lookup(): down biglock\n"));
		down(&c->fmc->biglock);
	} else if ((f = jffs_find_child(d, name, len))) {
	        D3(printk (KERN_NOTICE "lookup(): up biglock\n"));
		up(&c->fmc->biglock);
		if (!(inode = iget(dir->i_sb, f->ino))) {
			D(printk("jffs_lookup(): iget() ==> NULL\n"));
			goto jffs_lookup_end_no_biglock;
		}
		D3(printk (KERN_NOTICE "lookup(): down biglock\n"));
		down(&c->fmc->biglock);
	} else {
		D3(printk("jffs_lookup(): Couldn't find the file. "
			  "f = 0x%p, name = \"%s\", d = 0x%p, d->ino = %u\n",
			  f, name, d, d->ino));
		inode = NULL;
	}

	d_add(dentry, inode);
	D3(printk (KERN_NOTICE "lookup(): up biglock\n"));
	up(&c->fmc->biglock);
	return NULL;

jffs_lookup_end:
	D3(printk (KERN_NOTICE "lookup(): up biglock\n"));
	up(&c->fmc->biglock);

jffs_lookup_end_no_biglock:
	return ERR_PTR(r);
} /* jffs_lookup()  */


/* Try to read a page of data from a file.  */
static int
jffs_readpage(struct file *file, struct page *page)
{
	void *buf;
	unsigned long read_len;
	int result = -EIO;
	struct inode *inode = page->mapping->host;
	struct jffs_file *f = (struct jffs_file *)inode->u.generic_ip;
	struct jffs_control *c = (struct jffs_control *)inode->i_sb->u.generic_sbp;
	int r;
	loff_t offset;

	D2(printk("***jffs_readpage(): file = \"%s\", page->index = %lu\n",
		  (f->name ? f->name : ""), (long)page->index));

	get_page(page);
	/* Don't LockPage(page), should be locked already */
	buf = page_address(page);
	ClearPageUptodate(page);
	ClearPageError(page);

	D3(printk (KERN_NOTICE "readpage(): down biglock\n"));
	down(&c->fmc->biglock);

	offset = page->index << PAGE_CACHE_SHIFT;
	if (offset < inode->i_size) {
		read_len = jffs_min(inode->i_size - offset, PAGE_SIZE);
		r = jffs_read_data(f, buf, offset, read_len);
		if (r == read_len) {
			if (read_len < PAGE_SIZE) {
				memset(buf + read_len, 0,
				       PAGE_SIZE - read_len);
			}
			SetPageUptodate(page);
			result = 0;
		}
		D(else {
			printk("***jffs_readpage(): Read error! "
			       "Wanted to read %lu bytes but only "
			       "read %d bytes.\n", read_len, r);
		});
	}

	D3(printk (KERN_NOTICE "readpage(): up biglock\n"));
	up(&c->fmc->biglock);
	
	if (result) {
		memset(buf, 0, PAGE_SIZE);
	        SetPageError(page);
	}
	flush_dcache_page(page);

	UnlockPage(page);

	put_page(page);

	D3(printk("jffs_readpage(): Leaving...\n"));

	return result;
} /* jffs_readpage()  */


/* Create a new directory.  */
static int
jffs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	struct jffs_raw_inode raw_inode;
	struct jffs_control *c;
	struct jffs_node *node;
	struct jffs_file *dir_f;
	struct inode *inode;
	int dir_mode;
	int result = 0;
	int err;

	D1({
	        int len = dentry->d_name.len;
		char *_name = (char *) kmalloc(len + 1, GFP_KERNEL);
		memcpy(_name, dentry->d_name.name, len);
		_name[len] = '\0';
		printk("***jffs_mkdir(): dir = 0x%p, name = \"%s\", "
		       "len = %d, mode = 0x%08x\n", dir, _name, len, mode);
		kfree(_name);
	});

	dir_f = (struct jffs_file *)dir->u.generic_ip;

	ASSERT(if (!dir_f) {
		printk(KERN_ERR "jffs_mkdir(): No reference to a "
		       "jffs_file struct in inode.\n");
		return -EIO;
	});

	c = dir_f->c;
	D3(printk (KERN_NOTICE "mkdir(): down biglock\n"));
	down(&c->fmc->biglock);

	dir_mode = S_IFDIR | (mode & (S_IRWXUGO|S_ISVTX)
			      & ~current->fs->umask);
	if (dir->i_mode & S_ISGID) {
		dir_mode |= S_ISGID;
	}

	/* Create a node and initialize it as much as needed.  */
	if (!(node = (struct jffs_node *) kmalloc(sizeof(struct jffs_node),
						  GFP_KERNEL))) {
		D(printk("jffs_mkdir(): Allocation failed: node == 0\n"));
		result = -ENOMEM;
		goto jffs_mkdir_end;
	}
	DJM(no_jffs_node++);
	node->data_offset = 0;
	node->removed_size = 0;

	/* Initialize the raw inode.  */
	raw_inode.magic = JFFS_MAGIC_BITMASK;
	raw_inode.ino = c->next_ino++;
	raw_inode.pino = dir_f->ino;
	raw_inode.version = 1;
	raw_inode.mode = dir_mode;
	raw_inode.uid = current->fsuid;
	raw_inode.gid = (dir->i_mode & S_ISGID) ? dir->i_gid : current->fsgid;
	/*	raw_inode.gid = current->fsgid; */
	raw_inode.atime = CURRENT_TIME;
	raw_inode.mtime = raw_inode.atime;
	raw_inode.ctime = raw_inode.atime;
	raw_inode.offset = 0;
	raw_inode.dsize = 0;
	raw_inode.rsize = 0;
	raw_inode.nsize = dentry->d_name.len;
	raw_inode.nlink = 1;
	raw_inode.spare = 0;
	raw_inode.rename = 0;
	raw_inode.deleted = 0;

	/* Write the new node to the flash.  */
	if ((result = jffs_write_node(c, node, &raw_inode,
				      dentry->d_name.name, 0, 0, NULL)) < 0) {
		D(printk("jffs_mkdir(): jffs_write_node() failed.\n"));
		kfree(node);
		DJM(no_jffs_node--);
		goto jffs_mkdir_end;
	}

	/* Insert the new node into the file system.  */
	if ((result = jffs_insert_node(c, 0, &raw_inode, dentry->d_name.name,
				       node)) < 0) {
		goto jffs_mkdir_end;
	}

	inode = jffs_new_inode(dir, &raw_inode, &err);
	if (inode == NULL) {
		result = err;
		goto jffs_mkdir_end;
	}

	inode->i_op = &jffs_dir_inode_operations;
	inode->i_fop = &jffs_dir_operations;

	mark_inode_dirty(dir);
	d_instantiate(dentry, inode);

	result = 0;
jffs_mkdir_end:
	D3(printk (KERN_NOTICE "mkdir(): up biglock\n"));
	up(&c->fmc->biglock);
	return result;
} /* jffs_mkdir()  */


/* Remove a directory.  */
static int
jffs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct jffs_control *c = (struct jffs_control *)dir->i_sb->u.generic_sbp;
	int ret;
	D3(printk("***jffs_rmdir()\n"));
	D3(printk (KERN_NOTICE "rmdir(): down biglock\n"));
	down(&c->fmc->biglock);
	ret = jffs_remove(dir, dentry, S_IFDIR);
	D3(printk (KERN_NOTICE "rmdir(): up biglock\n"));
	up(&c->fmc->biglock);
	return ret;
}


/* Remove any kind of file except for directories.  */
static int
jffs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct jffs_control *c = (struct jffs_control *)dir->i_sb->u.generic_sbp;
	int ret; 

	D3(printk("***jffs_unlink()\n"));
	D3(printk (KERN_NOTICE "unlink(): down biglock\n"));
	down(&c->fmc->biglock);
	ret = jffs_remove(dir, dentry, 0);
	D3(printk (KERN_NOTICE "unlink(): up biglock\n"));
	up(&c->fmc->biglock);
	return ret;
}


/* Remove a JFFS entry, i.e. plain files, directories, etc.  Here we
   shouldn't test for free space on the device.  */
static int
jffs_remove(struct inode *dir, struct dentry *dentry, int type)
{
	struct jffs_raw_inode raw_inode;
	struct jffs_control *c;
	struct jffs_file *dir_f; /* The file-to-remove's parent.  */
	struct jffs_file *del_f; /* The file to remove.  */
	struct jffs_node *del_node;
	struct inode *inode = 0;
	int result = 0;

	D1({
	        int len = dentry->d_name.len;
	        const char *name = dentry->d_name.name;
		char *_name = (char *) kmalloc(len + 1, GFP_KERNEL);
		memcpy(_name, name, len);
		_name[len] = '\0';
		printk("***jffs_remove(): file = \"%s\", ino = %ld\n", _name, dentry->d_inode->i_ino);
		kfree(_name);
	});

	dir_f = (struct jffs_file *) dir->u.generic_ip;
	c = dir_f->c;

	result = -ENOENT;
	if (!(del_f = jffs_find_child(dir_f, dentry->d_name.name,
				      dentry->d_name.len))) {
		D(printk("jffs_remove(): jffs_find_child() failed.\n"));
		goto jffs_remove_end;
	}

	if (S_ISDIR(type)) {
		if (del_f->children) {
			result = -ENOTEMPTY;
			goto jffs_remove_end;
		}
	}
	else if (S_ISDIR(del_f->mode)) {
		D(printk("jffs_remove(): node is a directory "
			 "but it shouldn't be.\n"));
		result = -EPERM;
		goto jffs_remove_end;
	}

	inode = dentry->d_inode;

	result = -EIO;
	if (del_f->ino != inode->i_ino)
		goto jffs_remove_end;

	if (!inode->i_nlink) {
		printk("Deleting nonexistent file inode: %lu, nlink: %d\n",
		       inode->i_ino, inode->i_nlink);
		inode->i_nlink=1;
	}

	/* Create a node for the deletion.  */
	result = -ENOMEM;
	if (!(del_node = (struct jffs_node *)
			 kmalloc(sizeof(struct jffs_node), GFP_KERNEL))) {
		D(printk("jffs_remove(): Allocation failed!\n"));
		goto jffs_remove_end;
	}
	DJM(no_jffs_node++);
	del_node->data_offset = 0;
	del_node->removed_size = 0;

	/* Initialize the raw inode.  */
	raw_inode.magic = JFFS_MAGIC_BITMASK;
	raw_inode.ino = del_f->ino;
	raw_inode.pino = del_f->pino;
/*  	raw_inode.version = del_f->highest_version + 1; */
	raw_inode.mode = del_f->mode;
	raw_inode.uid = current->fsuid;
	raw_inode.gid = current->fsgid;
	raw_inode.atime = CURRENT_TIME;
	raw_inode.mtime = del_f->mtime;
	raw_inode.ctime = raw_inode.atime;
	raw_inode.offset = 0;
	raw_inode.dsize = 0;
	raw_inode.rsize = 0;
	raw_inode.nsize = 0;
	raw_inode.nlink = del_f->nlink;
	raw_inode.spare = 0;
	raw_inode.rename = 0;
	raw_inode.deleted = 1;

	/* Write the new node to the flash memory.  */
	if (jffs_write_node(c, del_node, &raw_inode, 0, 0, 1, del_f) < 0) {
		kfree(del_node);
		DJM(no_jffs_node--);
		result = -EIO;
		goto jffs_remove_end;
	}

	/* Update the file.  This operation will make the file disappear
	   from the in-memory file system structures.  */
	jffs_insert_node(c, del_f, &raw_inode, 0, del_node);

	dir->i_version = ++event;
	dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	mark_inode_dirty(dir);
	inode->i_nlink--;
	if (inode->i_nlink == 0) {
		inode->u.generic_ip = 0;
	}
	inode->i_ctime = dir->i_ctime;
	mark_inode_dirty(inode);

	d_delete(dentry);	/* This also frees the inode */

	result = 0;
jffs_remove_end:
	return result;
} /* jffs_remove()  */


static int
jffs_mknod(struct inode *dir, struct dentry *dentry, int mode, int rdev)
{
	struct jffs_raw_inode raw_inode;
	struct jffs_file *dir_f;
	struct jffs_node *node = 0;
	struct jffs_control *c;
	struct inode *inode;
	int result = 0;
	kdev_t dev = to_kdev_t(rdev);
	int err;

	D1(printk("***jffs_mknod()\n"));

	dir_f = (struct jffs_file *)dir->u.generic_ip;
	c = dir_f->c;

	D3(printk (KERN_NOTICE "mknod(): down biglock\n"));
	down(&c->fmc->biglock);

	/* Create and initialize a new node.  */
	if (!(node = (struct jffs_node *) kmalloc(sizeof(struct jffs_node),
						  GFP_KERNEL))) {
		D(printk("jffs_mknod(): Allocation failed!\n"));
		result = -ENOMEM;
		goto jffs_mknod_err;
	}
	DJM(no_jffs_node++);
	node->data_offset = 0;
	node->removed_size = 0;

	/* Initialize the raw inode.  */
	raw_inode.magic = JFFS_MAGIC_BITMASK;
	raw_inode.ino = c->next_ino++;
	raw_inode.pino = dir_f->ino;
	raw_inode.version = 1;
	raw_inode.mode = mode;
	raw_inode.uid = current->fsuid;
	raw_inode.gid = (dir->i_mode & S_ISGID) ? dir->i_gid : current->fsgid;
	/*	raw_inode.gid = current->fsgid; */
	raw_inode.atime = CURRENT_TIME;
	raw_inode.mtime = raw_inode.atime;
	raw_inode.ctime = raw_inode.atime;
	raw_inode.offset = 0;
	raw_inode.dsize = sizeof(kdev_t);
	raw_inode.rsize = 0;
	raw_inode.nsize = dentry->d_name.len;
	raw_inode.nlink = 1;
	raw_inode.spare = 0;
	raw_inode.rename = 0;
	raw_inode.deleted = 0;

	/* Write the new node to the flash.  */
	if ((err = jffs_write_node(c, node, &raw_inode, dentry->d_name.name,
				  (unsigned char *)&dev, 0, NULL)) < 0) {
		D(printk("jffs_mknod(): jffs_write_node() failed.\n"));
		result = err;
		goto jffs_mknod_err;
	}

	/* Insert the new node into the file system.  */
	if ((err = jffs_insert_node(c, 0, &raw_inode, dentry->d_name.name,
				    node)) < 0) {
		result = err;
		goto jffs_mknod_end;
	}

	inode = jffs_new_inode(dir, &raw_inode, &err);
	if (inode == NULL) {
		result = err;
		goto jffs_mknod_end;
	}

	init_special_inode(inode, mode, rdev);

	d_instantiate(dentry, inode);

	goto jffs_mknod_end;

jffs_mknod_err:
	if (node) {
		kfree(node);
		DJM(no_jffs_node--);
	}

jffs_mknod_end:
	D3(printk (KERN_NOTICE "mknod(): up biglock\n"));
	up(&c->fmc->biglock);
	return result;
} /* jffs_mknod()  */


static int
jffs_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	struct jffs_raw_inode raw_inode;
	struct jffs_control *c;
	struct jffs_file *dir_f;
	struct jffs_node *node;
	struct inode *inode;

	int symname_len = strlen(symname);
	int err;

	D1({
	        int len = dentry->d_name.len;
		char *_name = (char *)kmalloc(len + 1, GFP_KERNEL);
		char *_symname = (char *)kmalloc(symname_len + 1, GFP_KERNEL);
		memcpy(_name, dentry->d_name.name, len);
		_name[len] = '\0';
		memcpy(_symname, symname, symname_len);
		_symname[symname_len] = '\0';
		printk("***jffs_symlink(): dir = 0x%p, "
		       "dentry->dname.name = \"%s\", "
		       "symname = \"%s\"\n", dir, _name, _symname);
		kfree(_name);
		kfree(_symname);
	});

	dir_f = (struct jffs_file *)dir->u.generic_ip;
	ASSERT(if (!dir_f) {
		printk(KERN_ERR "jffs_symlink(): No reference to a "
		       "jffs_file struct in inode.\n");
		return -EIO;
	});

	c = dir_f->c;

	/* Create a node and initialize it as much as needed.  */
	if (!(node = (struct jffs_node *) kmalloc(sizeof(struct jffs_node),
						  GFP_KERNEL))) {
		D(printk("jffs_symlink(): Allocation failed: node = NULL\n"));
		return -ENOMEM;
	}
	D3(printk (KERN_NOTICE "symlink(): down biglock\n"));
	down(&c->fmc->biglock);

	DJM(no_jffs_node++);
	node->data_offset = 0;
	node->removed_size = 0;

	/* Initialize the raw inode.  */
	raw_inode.magic = JFFS_MAGIC_BITMASK;
	raw_inode.ino = c->next_ino++;
	raw_inode.pino = dir_f->ino;
	raw_inode.version = 1;
	raw_inode.mode = S_IFLNK | S_IRWXUGO;
	raw_inode.uid = current->fsuid;
	raw_inode.gid = (dir->i_mode & S_ISGID) ? dir->i_gid : current->fsgid;
	raw_inode.atime = CURRENT_TIME;
	raw_inode.mtime = raw_inode.atime;
	raw_inode.ctime = raw_inode.atime;
	raw_inode.offset = 0;
	raw_inode.dsize = symname_len;
	raw_inode.rsize = 0;
	raw_inode.nsize = dentry->d_name.len;
	raw_inode.nlink = 1;
	raw_inode.spare = 0;
	raw_inode.rename = 0;
	raw_inode.deleted = 0;

	/* Write the new node to the flash.  */
	if ((err = jffs_write_node(c, node, &raw_inode, dentry->d_name.name,
				   (const unsigned char *)symname, 0, NULL)) < 0) {
		D(printk("jffs_symlink(): jffs_write_node() failed.\n"));
		kfree(node);
		DJM(no_jffs_node--);
		goto jffs_symlink_end;
	}

	/* Insert the new node into the file system.  */
	if ((err = jffs_insert_node(c, 0, &raw_inode, dentry->d_name.name,
				    node)) < 0) {
		goto jffs_symlink_end;
	}

	inode = jffs_new_inode(dir, &raw_inode, &err);
	if (inode == NULL) {
		goto jffs_symlink_end;
	}
	err = 0;
	inode->i_op = &page_symlink_inode_operations;
	inode->i_mapping->a_ops = &jffs_address_operations;

	d_instantiate(dentry, inode);
 jffs_symlink_end:
	D3(printk (KERN_NOTICE "symlink(): up biglock\n"));
	up(&c->fmc->biglock);
	return err;
} /* jffs_symlink()  */


/* Create an inode inside a JFFS directory (dir) and return it.
 *
 * By the time this is called, we already have created
 * the directory cache entry for the new file, but it
 * is so far negative - it has no inode.
 *
 * If the create succeeds, we fill in the inode information
 * with d_instantiate().
 */
static int
jffs_create(struct inode *dir, struct dentry *dentry, int mode)
{
	struct jffs_raw_inode raw_inode;
	struct jffs_control *c;
	struct jffs_node *node;
	struct jffs_file *dir_f; /* JFFS representation of the directory.  */
	struct inode *inode;
	int err;

	D1({
	        int len = dentry->d_name.len;
		char *s = (char *)kmalloc(len + 1, GFP_KERNEL);
		memcpy(s, dentry->d_name.name, len);
		s[len] = '\0';
		printk("jffs_create(): dir: 0x%p, name: \"%s\"\n", dir, s);
		kfree(s);
	});

	dir_f = (struct jffs_file *)dir->u.generic_ip;
	ASSERT(if (!dir_f) {
		printk(KERN_ERR "jffs_create(): No reference to a "
		       "jffs_file struct in inode.\n");
		return -EIO;
	});

	c = dir_f->c;

	/* Create a node and initialize as much as needed.  */
	if (!(node = (struct jffs_node *) kmalloc(sizeof(struct jffs_node),
						  GFP_KERNEL))) {
		D(printk("jffs_create(): Allocation failed: node == 0\n"));
		return -ENOMEM;
	}
	D3(printk (KERN_NOTICE "create(): down biglock\n"));
	down(&c->fmc->biglock);

	DJM(no_jffs_node++);
	node->data_offset = 0;
	node->removed_size = 0;

	/* Initialize the raw inode.  */
	raw_inode.magic = JFFS_MAGIC_BITMASK;
	raw_inode.ino = c->next_ino++;
	raw_inode.pino = dir_f->ino;
	raw_inode.version = 1;
	raw_inode.mode = mode;
	raw_inode.uid = current->fsuid;
	raw_inode.gid = (dir->i_mode & S_ISGID) ? dir->i_gid : current->fsgid;
	raw_inode.atime = CURRENT_TIME;
	raw_inode.mtime = raw_inode.atime;
	raw_inode.ctime = raw_inode.atime;
	raw_inode.offset = 0;
	raw_inode.dsize = 0;
	raw_inode.rsize = 0;
	raw_inode.nsize = dentry->d_name.len;
	raw_inode.nlink = 1;
	raw_inode.spare = 0;
	raw_inode.rename = 0;
	raw_inode.deleted = 0;

	/* Write the new node to the flash.  */
	if ((err = jffs_write_node(c, node, &raw_inode,
				   dentry->d_name.name, 0, 0, NULL)) < 0) {
		D(printk("jffs_create(): jffs_write_node() failed.\n"));
		kfree(node);
		DJM(no_jffs_node--);
		goto jffs_create_end;
	}

	/* Insert the new node into the file system.  */
	if ((err = jffs_insert_node(c, 0, &raw_inode, dentry->d_name.name,
				    node)) < 0) {
		goto jffs_create_end;
	}

	/* Initialize an inode.  */
	inode = jffs_new_inode(dir, &raw_inode, &err);
	if (inode == NULL) {
		goto jffs_create_end;
	}
	err = 0;
	inode->i_op = &jffs_file_inode_operations;
	inode->i_fop = &jffs_file_operations;
	inode->i_mapping->a_ops = &jffs_address_operations;
	inode->i_mapping->nrpages = 0;

	d_instantiate(dentry, inode);
 jffs_create_end:
	D3(printk (KERN_NOTICE "create(): up biglock\n"));
	up(&c->fmc->biglock);
	return err;
} /* jffs_create()  */


/* Write, append or rewrite data to an existing file.  */
static ssize_t
jffs_file_write(struct file *filp, const char *buf, size_t count,
		loff_t *ppos)
{
	struct jffs_raw_inode raw_inode;
	struct jffs_control *c;
	struct jffs_file *f;
	struct jffs_node *node;
	struct dentry *dentry = filp->f_dentry;
	struct inode *inode = dentry->d_inode;
	unsigned char *vbuf;
	int recoverable = 0;
	size_t written = 0;
	__u32 thiscount = count;
	loff_t pos;
	int err;

	inode = filp->f_dentry->d_inode;

	D2(printk("***jffs_file_write(): inode: 0x%p (ino: %lu), "
		  "filp: 0x%p, buf: 0x%p, count: %d\n",
		  inode, inode->i_ino, filp, buf, count));

	err = filp->f_error;
	if (err) {
		filp->f_error = 0;
		return err;
	}

	down(&inode->i_sem);

	if (inode->i_sb->s_flags & MS_RDONLY) {
		D(printk("jffs_file_write(): MS_RDONLY\n"));
		err = -EROFS;
		goto out_isem;
	}

	err = -EINVAL;

	if (!S_ISREG(inode->i_mode)) {
		D(printk("jffs_file_write(): inode->i_mode == 0x%08x\n",
			 inode->i_mode));
		goto out_isem;
	}

	if (!(f = (struct jffs_file *)inode->u.generic_ip)) {
		D(printk("jffs_file_write(): inode->u.generic_ip = 0x%p\n",
			 inode->u.generic_ip));
		goto out_isem;
	}

	c = f->c;

	if (filp->f_flags & O_APPEND)
		pos = inode->i_size;
	else
		pos = *ppos;
	
	if (pos < 0) {
		goto out_isem;
	}
	
	thiscount = jffs_min(c->fmc->max_chunk_size - sizeof(struct jffs_raw_inode), count);

	if (!(vbuf = kmalloc(thiscount, GFP_KERNEL))) {
		D(printk("jffs_file_write(): failed to allocate bounce buffer. Fix me to use page cache\n"));
		err = -ENOMEM;
		goto out_isem;
	}

	D3(printk (KERN_NOTICE "file_write(): down biglock\n"));
	down(&c->fmc->biglock);

	/* Urgh. POSIX says we can do short writes if we feel like it. 
	 * In practice, we can't. Nothing will cope. So we loop until
	 * we're done.
	 *
	 * <_Anarchy_> posix and reality are not interconnected on this issue
	 */
	while (count) {

		/* FIXME: This is entirely gratuitous use of bounce buffers.
		   Get a clue and use the page cache. 
		   /me wanders off to get a crash course on Linux VFS
		   dwmw2
		*/
		if (copy_from_user(vbuf, buf, thiscount)) {
			err = -EFAULT;
			goto out;
		}
		
		/* Things are going to be written so we could allocate and
		   initialize the necessary data structures now.  */
		if (!(node = (struct jffs_node *) kmalloc(sizeof(struct jffs_node),
							  GFP_KERNEL))) {
			D(printk("jffs_file_write(): node == 0\n"));
			err = -ENOMEM;
			goto out;
		}
		DJM(no_jffs_node++);
		
		node->data_offset = pos;
		node->removed_size = 0;
		
		/* Initialize the raw inode.  */
		raw_inode.magic = JFFS_MAGIC_BITMASK;
		raw_inode.ino = f->ino;
		raw_inode.pino = f->pino;

		raw_inode.mode = f->mode;
		
		raw_inode.uid = f->uid;
		raw_inode.gid = f->gid;
		raw_inode.atime = CURRENT_TIME;
		raw_inode.mtime = raw_inode.atime;
		raw_inode.ctime = f->ctime;
		raw_inode.offset = pos;
		raw_inode.dsize = thiscount;
		raw_inode.rsize = 0;
		raw_inode.nsize = f->nsize;
		raw_inode.nlink = f->nlink;
		raw_inode.spare = 0;
		raw_inode.rename = 0;
		raw_inode.deleted = 0;
		
		if (pos < f->size) {
			node->removed_size = raw_inode.rsize = jffs_min(thiscount, f->size - pos);
			
			/* If this node is going entirely over the top of old data, 
			   we can allow it to go into the reserved space, because 
			   we can that GC can reclaim the space later.
			*/
			if (pos + thiscount < f->size) {
				/* If all the data we're overwriting are _real_,
				   not just holes, then:
				   recoverable = 1;
				*/
			}
		}
		
		/* Write the new node to the flash.  */
		/* NOTE: We would be quite happy if jffs_write_node() wrote a 
		   smaller node than we were expecting. There's no need for it 
		   to waste the space at the end of the flash just because it's 
		   a little smaller than what we asked for. But that's a whole
		   new can of worms which I'm not going to open this week. dwmw2.
		*/
		if ((err = jffs_write_node(c, node, &raw_inode, f->name,
					   (const unsigned char *)vbuf,
					   recoverable, f)) < 0) {
			D(printk("jffs_file_write(): jffs_write_node() failed.\n"));
			kfree(node);
			DJM(no_jffs_node--);
			goto out;
		}

		written += err;
		buf += err;
		count -= err;
		pos += err;

		/* Insert the new node into the file system.  */
		if ((err = jffs_insert_node(c, f, &raw_inode, 0, node)) < 0) {
			goto out;
		}

		D3(printk("jffs_file_write(): new f_pos %ld.\n", (long)pos));

		thiscount = jffs_min(c->fmc->max_chunk_size - sizeof(struct jffs_raw_inode), count);
	}
 out:
	D3(printk (KERN_NOTICE "file_write(): up biglock\n"));
	up(&c->fmc->biglock);
	*ppos = pos;
	kfree(vbuf);

	/* Fix things in the real inode.  */
	if (pos > inode->i_size) {
		inode->i_size = pos;
		inode->i_blocks = (inode->i_size + 511) >> 9;
	}
	inode->i_ctime = inode->i_mtime = CURRENT_TIME;
	mark_inode_dirty(inode);
	invalidate_inode_pages(inode);

 out_isem:
	up(&inode->i_sem);
	
	/* What if there was an error, _and_ we've written some data. */
	if (written)
		return written;
	else
		return err;
} /* jffs_file_write()  */


/* This is our ioctl() routine.  */
static int
jffs_ioctl(struct inode *inode, struct file *filp, unsigned int cmd,
	   unsigned long arg)
{
	struct jffs_control *c;
	int ret = 0;

	D2(printk("***jffs_ioctl(): cmd = 0x%08x, arg = 0x%08lx\n",
		  cmd, arg));

	if (!(c = (struct jffs_control *)inode->i_sb->u.generic_sbp)) {
		printk(KERN_ERR "JFFS: Bad inode in ioctl() call. "
		       "(cmd = 0x%08x)\n", cmd);
		return -EIO;
	}
	D3(printk (KERN_NOTICE "ioctl(): down biglock\n"));
	down(&c->fmc->biglock);

	switch (cmd) {
	case JFFS_PRINT_HASH:
		jffs_print_hash_table(c);
		break;
	case JFFS_PRINT_TREE:
		jffs_print_tree(c->root, 0);
		break;
	case JFFS_GET_STATUS:
		{
			struct jffs_flash_status fst;
			struct jffs_fmcontrol *fmc = c->fmc;
			printk("Flash status -- ");
			if (!access_ok(VERIFY_WRITE,
				       (struct jffs_flash_status *)arg,
				       sizeof(struct jffs_flash_status))) {
				D(printk("jffs_ioctl(): Bad arg in "
					 "JFFS_GET_STATUS ioctl!\n"));
				ret = -EFAULT;
				break;
			}
			fst.size = fmc->flash_size;
			fst.used = fmc->used_size;
			fst.dirty = fmc->dirty_size;
			fst.begin = fmc->head->offset;
			fst.end = fmc->tail->offset + fmc->tail->size;
			printk("size: %d, used: %d, dirty: %d, "
			       "begin: %d, end: %d\n",
			       fst.size, fst.used, fst.dirty,
			       fst.begin, fst.end);
			if (copy_to_user((struct jffs_flash_status *)arg,
					 &fst,
					 sizeof(struct jffs_flash_status))) {
			  ret = -EFAULT;
			}
		}
		break;
	default:
		ret = -ENOTTY;
	}
	D3(printk (KERN_NOTICE "ioctl(): up biglock\n"));
	up(&c->fmc->biglock);
	return ret;
} /* jffs_ioctl()  */


static struct address_space_operations jffs_address_operations = {
	readpage: jffs_readpage,
};

static int jffs_fsync(struct file *f, struct dentry *d, int datasync)
{
	/* We currently have O_SYNC operations at all times. 
	   Do nothing
	*/
	return 0;
}


static struct file_operations jffs_file_operations =
{
	read:  generic_file_read,    /* read */
	write: jffs_file_write,      /* write */
	ioctl: jffs_ioctl,           /* ioctl */
	mmap:  generic_file_mmap,    /* mmap */
	fsync: jffs_fsync,
};


static struct inode_operations jffs_file_inode_operations =
{
	lookup:  jffs_lookup,          /* lookup */
	setattr: jffs_setattr,
};


static struct file_operations jffs_dir_operations =
{
	readdir:	jffs_readdir,
};


static struct inode_operations jffs_dir_inode_operations =
{
	create:   jffs_create,
	lookup:   jffs_lookup,
	unlink:   jffs_unlink,
	symlink:  jffs_symlink,
	mkdir:    jffs_mkdir,
	rmdir:    jffs_rmdir,
	mknod:    jffs_mknod,
	rename:   jffs_rename,
	setattr:  jffs_setattr,
};


/* Initialize an inode for the VFS.  */
static void
jffs_read_inode(struct inode *inode)
{
	struct jffs_file *f;
	struct jffs_control *c;

	D3(printk("jffs_read_inode(): inode->i_ino == %lu\n", inode->i_ino));

	if (!inode->i_sb) {
		D(printk("jffs_read_inode(): !inode->i_sb ==> "
			 "No super block!\n"));
		return;
	}
	c = (struct jffs_control *)inode->i_sb->u.generic_sbp;
	D3(printk (KERN_NOTICE "read_inode(): down biglock\n"));
	down(&c->fmc->biglock);
	if (!(f = jffs_find_file(c, inode->i_ino))) {
		D(printk("jffs_read_inode(): No such inode (%lu).\n",
			 inode->i_ino));
		D3(printk (KERN_NOTICE "read_inode(): up biglock\n"));
		up(&c->fmc->biglock);
		return;
	}
	inode->u.generic_ip = (void *)f;
	inode->i_mode = f->mode;
	inode->i_nlink = f->nlink;
	inode->i_uid = f->uid;
	inode->i_gid = f->gid;
	inode->i_size = f->size;
	inode->i_atime = f->atime;
	inode->i_mtime = f->mtime;
	inode->i_ctime = f->ctime;
	inode->i_blksize = PAGE_SIZE;
	inode->i_blocks = (inode->i_size + 511) >> 9;
	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &jffs_file_inode_operations;
		inode->i_fop = &jffs_file_operations;
		inode->i_mapping->a_ops = &jffs_address_operations;
	}
	else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &jffs_dir_inode_operations;
		inode->i_fop = &jffs_dir_operations;
	}
	else if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &page_symlink_inode_operations;
		inode->i_mapping->a_ops = &jffs_address_operations;
	}
	else {
		/* If the node is a device of some sort, then the number of
		   the device should be read from the flash memory and then
		   added to the inode's i_rdev member.  */
		kdev_t rdev;
		jffs_read_data(f, (char *)&rdev, 0, sizeof(kdev_t));
		init_special_inode(inode, inode->i_mode, kdev_t_to_nr(rdev));
	}
	D3(printk (KERN_NOTICE "read_inode(): up biglock\n"));
	up(&c->fmc->biglock);
}


void
jffs_delete_inode(struct inode *inode)
{
	D3(printk("jffs_delete_inode(): inode->i_ino == %lu\n",
		  inode->i_ino));

	lock_kernel();
	inode->i_size = 0;
	inode->i_blocks = 0;
	clear_inode(inode);
	unlock_kernel();
}


void
jffs_write_super(struct super_block *sb)
{
	struct jffs_control *c = (struct jffs_control *)sb->u.generic_sbp;

	jffs_garbage_collect_trigger(c);
}


static struct super_operations jffs_ops =
{
	read_inode:   jffs_read_inode,
	delete_inode: jffs_delete_inode,
	put_super:    jffs_put_super,
	write_super:  jffs_write_super,
	statfs:       jffs_statfs,
};


static DECLARE_FSTYPE_DEV(jffs_fs_type, "jffs", jffs_read_super);

static int __init
init_jffs_fs(void)
{
	printk("JFFS version "
	       JFFS_VERSION_STRING
	       ", (C) 1999, 2000  Axis Communications AB\n");
	return register_filesystem(&jffs_fs_type);
}

static void __exit
exit_jffs_fs(void)
{
	unregister_filesystem(&jffs_fs_type);
}

EXPORT_NO_SYMBOLS;

module_init(init_jffs_fs)
module_exit(exit_jffs_fs)
