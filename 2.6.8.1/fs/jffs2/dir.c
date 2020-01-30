/*
 * JFFS2 -- Journalling Flash File System, Version 2.
 *
 * Copyright (C) 2001-2003 Red Hat, Inc.
 *
 * Created by David Woodhouse <dwmw2@redhat.com>
 *
 * For licensing information, see the file 'LICENCE' in this directory.
 *
 * $Id: dir.c,v 1.82 2003/10/11 11:47:23 dwmw2 Exp $
 *
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/crc32.h>
#include <linux/jffs2.h>
#include <linux/jffs2_fs_i.h>
#include <linux/jffs2_fs_sb.h>
#include <linux/time.h>
#include "nodelist.h"

/* Urgh. Please tell me there's a nicer way of doing these. */
#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,48)
typedef int mknod_arg_t;
#define NAMEI_COMPAT(x) ((void *)x)
#else
typedef dev_t mknod_arg_t;
#define NAMEI_COMPAT(x) (x)
#endif

static int jffs2_readdir (struct file *, void *, filldir_t);

static int jffs2_create (struct inode *,struct dentry *,int,
			 struct nameidata *);
static struct dentry *jffs2_lookup (struct inode *,struct dentry *,
				    struct nameidata *);
static int jffs2_link (struct dentry *,struct inode *,struct dentry *);
static int jffs2_unlink (struct inode *,struct dentry *);
static int jffs2_symlink (struct inode *,struct dentry *,const char *);
static int jffs2_mkdir (struct inode *,struct dentry *,int);
static int jffs2_rmdir (struct inode *,struct dentry *);
static int jffs2_mknod (struct inode *,struct dentry *,int,mknod_arg_t);
static int jffs2_rename (struct inode *, struct dentry *,
                        struct inode *, struct dentry *);

struct file_operations jffs2_dir_operations =
{
	.read =		generic_read_dir,
	.readdir =	jffs2_readdir,
	.ioctl =	jffs2_ioctl,
	.fsync =	jffs2_fsync
};


struct inode_operations jffs2_dir_inode_operations =
{
	.create =	NAMEI_COMPAT(jffs2_create),
	.lookup =	NAMEI_COMPAT(jffs2_lookup),
	.link =		jffs2_link,
	.unlink =	jffs2_unlink,
	.symlink =	jffs2_symlink,
	.mkdir =	jffs2_mkdir,
	.rmdir =	jffs2_rmdir,
	.mknod =	jffs2_mknod,
	.rename =	jffs2_rename,
	.setattr =	jffs2_setattr,
};

/***********************************************************************/


/* We keep the dirent list sorted in increasing order of name hash,
   and we use the same hash function as the dentries. Makes this 
   nice and simple
*/
static struct dentry *jffs2_lookup(struct inode *dir_i, struct dentry *target,
				   struct nameidata *nd)
{
	struct jffs2_inode_info *dir_f;
	struct jffs2_sb_info *c;
	struct jffs2_full_dirent *fd = NULL, *fd_list;
	uint32_t ino = 0;
	struct inode *inode = NULL;

	D1(printk(KERN_DEBUG "jffs2_lookup()\n"));

	dir_f = JFFS2_INODE_INFO(dir_i);
	c = JFFS2_SB_INFO(dir_i->i_sb);

	down(&dir_f->sem);

	/* NB: The 2.2 backport will need to explicitly check for '.' and '..' here */
	for (fd_list = dir_f->dents; fd_list && fd_list->nhash <= target->d_name.hash; fd_list = fd_list->next) {
		if (fd_list->nhash == target->d_name.hash && 
		    (!fd || fd_list->version > fd->version) &&
		    strlen(fd_list->name) == target->d_name.len &&
		    !strncmp(fd_list->name, target->d_name.name, target->d_name.len)) {
			fd = fd_list;
		}
	}
	if (fd)
		ino = fd->ino;
	up(&dir_f->sem);
	if (ino) {
		inode = iget(dir_i->i_sb, ino);
		if (!inode) {
			printk(KERN_WARNING "iget() failed for ino #%u\n", ino);
			return (ERR_PTR(-EIO));
		}
	}

	d_add(target, inode);

	return NULL;
}

/***********************************************************************/


static int jffs2_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct jffs2_inode_info *f;
	struct jffs2_sb_info *c;
	struct inode *inode = filp->f_dentry->d_inode;
	struct jffs2_full_dirent *fd;
	unsigned long offset, curofs;

	D1(printk(KERN_DEBUG "jffs2_readdir() for dir_i #%lu\n", filp->f_dentry->d_inode->i_ino));

	f = JFFS2_INODE_INFO(inode);
	c = JFFS2_SB_INFO(inode->i_sb);

	offset = filp->f_pos;

	if (offset == 0) {
		D1(printk(KERN_DEBUG "Dirent 0: \".\", ino #%lu\n", inode->i_ino));
		if (filldir(dirent, ".", 1, 0, inode->i_ino, DT_DIR) < 0)
			goto out;
		offset++;
	}
	if (offset == 1) {
		unsigned long pino = parent_ino(filp->f_dentry);
		D1(printk(KERN_DEBUG "Dirent 1: \"..\", ino #%lu\n", pino));
		if (filldir(dirent, "..", 2, 1, pino, DT_DIR) < 0)
			goto out;
		offset++;
	}

	curofs=1;
	down(&f->sem);
	for (fd = f->dents; fd; fd = fd->next) {

		curofs++;
		/* First loop: curofs = 2; offset = 2 */
		if (curofs < offset) {
			D2(printk(KERN_DEBUG "Skipping dirent: \"%s\", ino #%u, type %d, because curofs %ld < offset %ld\n", 
				  fd->name, fd->ino, fd->type, curofs, offset));
			continue;
		}
		if (!fd->ino) {
			D2(printk(KERN_DEBUG "Skipping deletion dirent \"%s\"\n", fd->name));
			offset++;
			continue;
		}
		D2(printk(KERN_DEBUG "Dirent %ld: \"%s\", ino #%u, type %d\n", offset, fd->name, fd->ino, fd->type));
		if (filldir(dirent, fd->name, strlen(fd->name), offset, fd->ino, fd->type) < 0)
			break;
		offset++;
	}
	up(&f->sem);
 out:
	filp->f_pos = offset;
	return 0;
}

/***********************************************************************/


static int jffs2_create(struct inode *dir_i, struct dentry *dentry, int mode,
			struct nameidata *nd)
{
	struct jffs2_raw_inode *ri;
	struct jffs2_inode_info *f, *dir_f;
	struct jffs2_sb_info *c;
	struct inode *inode;
	int ret;

	ri = jffs2_alloc_raw_inode();
	if (!ri)
		return -ENOMEM;
	
	c = JFFS2_SB_INFO(dir_i->i_sb);

	D1(printk(KERN_DEBUG "jffs2_create()\n"));

	inode = jffs2_new_inode(dir_i, mode, ri);

	if (IS_ERR(inode)) {
		D1(printk(KERN_DEBUG "jffs2_new_inode() failed\n"));
		jffs2_free_raw_inode(ri);
		return PTR_ERR(inode);
	}

	inode->i_op = &jffs2_file_inode_operations;
	inode->i_fop = &jffs2_file_operations;
	inode->i_mapping->a_ops = &jffs2_file_address_operations;
	inode->i_mapping->nrpages = 0;

	f = JFFS2_INODE_INFO(inode);
	dir_f = JFFS2_INODE_INFO(dir_i);

	ret = jffs2_do_create(c, dir_f, f, ri, 
			      dentry->d_name.name, dentry->d_name.len);

	if (ret) {
		jffs2_clear_inode(inode);
		make_bad_inode(inode);
		iput(inode);
		jffs2_free_raw_inode(ri);
		return ret;
	}

	dir_i->i_mtime = dir_i->i_ctime = ITIME(je32_to_cpu(ri->ctime));

	jffs2_free_raw_inode(ri);
	d_instantiate(dentry, inode);

	D1(printk(KERN_DEBUG "jffs2_create: Created ino #%lu with mode %o, nlink %d(%d). nrpages %ld\n",
		  inode->i_ino, inode->i_mode, inode->i_nlink, f->inocache->nlink, inode->i_mapping->nrpages));
	return 0;
}

/***********************************************************************/


static int jffs2_unlink(struct inode *dir_i, struct dentry *dentry)
{
	struct jffs2_sb_info *c = JFFS2_SB_INFO(dir_i->i_sb);
	struct jffs2_inode_info *dir_f = JFFS2_INODE_INFO(dir_i);
	struct jffs2_inode_info *dead_f = JFFS2_INODE_INFO(dentry->d_inode);
	int ret;

	ret = jffs2_do_unlink(c, dir_f, dentry->d_name.name, 
			       dentry->d_name.len, dead_f);
	if (dead_f->inocache)
		dentry->d_inode->i_nlink = dead_f->inocache->nlink;
	return ret;
}
/***********************************************************************/


static int jffs2_link (struct dentry *old_dentry, struct inode *dir_i, struct dentry *dentry)
{
	struct jffs2_sb_info *c = JFFS2_SB_INFO(old_dentry->d_inode->i_sb);
	struct jffs2_inode_info *f = JFFS2_INODE_INFO(old_dentry->d_inode);
	struct jffs2_inode_info *dir_f = JFFS2_INODE_INFO(dir_i);
	int ret;
	uint8_t type;

	/* Don't let people make hard links to bad inodes. */
	if (!f->inocache)
		return -EIO;

	if (S_ISDIR(old_dentry->d_inode->i_mode))
		return -EPERM;

	/* XXX: This is ugly */
	type = (old_dentry->d_inode->i_mode & S_IFMT) >> 12;
	if (!type) type = DT_REG;

	ret = jffs2_do_link(c, dir_f, f->inocache->ino, type, dentry->d_name.name, dentry->d_name.len);

	if (!ret) {
		down(&f->sem);
		old_dentry->d_inode->i_nlink = ++f->inocache->nlink;
		up(&f->sem);
		d_instantiate(dentry, old_dentry->d_inode);
		atomic_inc(&old_dentry->d_inode->i_count);
	}
	return ret;
}

/***********************************************************************/

static int jffs2_symlink (struct inode *dir_i, struct dentry *dentry, const char *target)
{
	struct jffs2_inode_info *f, *dir_f;
	struct jffs2_sb_info *c;
	struct inode *inode;
	struct jffs2_raw_inode *ri;
	struct jffs2_raw_dirent *rd;
	struct jffs2_full_dnode *fn;
	struct jffs2_full_dirent *fd;
	int namelen;
	uint32_t alloclen, phys_ofs;
	int ret;

	/* FIXME: If you care. We'd need to use frags for the target
	   if it grows much more than this */
	if (strlen(target) > 254)
		return -EINVAL;

	ri = jffs2_alloc_raw_inode();

	if (!ri)
		return -ENOMEM;
	
	c = JFFS2_SB_INFO(dir_i->i_sb);
	
	/* Try to reserve enough space for both node and dirent. 
	 * Just the node will do for now, though 
	 */
	namelen = dentry->d_name.len;
	ret = jffs2_reserve_space(c, sizeof(*ri) + strlen(target), &phys_ofs, &alloclen, ALLOC_NORMAL);

	if (ret) {
		jffs2_free_raw_inode(ri);
		return ret;
	}

	inode = jffs2_new_inode(dir_i, S_IFLNK | S_IRWXUGO, ri);

	if (IS_ERR(inode)) {
		jffs2_free_raw_inode(ri);
		jffs2_complete_reservation(c);
		return PTR_ERR(inode);
	}

	inode->i_op = &jffs2_symlink_inode_operations;

	f = JFFS2_INODE_INFO(inode);

	inode->i_size = strlen(target);
	ri->isize = ri->dsize = ri->csize = cpu_to_je32(inode->i_size);
	ri->totlen = cpu_to_je32(sizeof(*ri) + inode->i_size);
	ri->hdr_crc = cpu_to_je32(crc32(0, ri, sizeof(struct jffs2_unknown_node)-4));

	ri->compr = JFFS2_COMPR_NONE;
	ri->data_crc = cpu_to_je32(crc32(0, target, strlen(target)));
	ri->node_crc = cpu_to_je32(crc32(0, ri, sizeof(*ri)-8));
	
	fn = jffs2_write_dnode(c, f, ri, target, strlen(target), phys_ofs, ALLOC_NORMAL);

	jffs2_free_raw_inode(ri);

	if (IS_ERR(fn)) {
		/* Eeek. Wave bye bye */
		up(&f->sem);
		jffs2_complete_reservation(c);
		jffs2_clear_inode(inode);
		return PTR_ERR(fn);
	}
	/* No data here. Only a metadata node, which will be 
	   obsoleted by the first data write
	*/
	f->metadata = fn;
	up(&f->sem);

	jffs2_complete_reservation(c);
	ret = jffs2_reserve_space(c, sizeof(*rd)+namelen, &phys_ofs, &alloclen, ALLOC_NORMAL);
	if (ret) {
		/* Eep. */
		jffs2_clear_inode(inode);
		return ret;
	}

	rd = jffs2_alloc_raw_dirent();
	if (!rd) {
		/* Argh. Now we treat it like a normal delete */
		jffs2_complete_reservation(c);
		jffs2_clear_inode(inode);
		return -ENOMEM;
	}

	dir_f = JFFS2_INODE_INFO(dir_i);
	down(&dir_f->sem);

	rd->magic = cpu_to_je16(JFFS2_MAGIC_BITMASK);
	rd->nodetype = cpu_to_je16(JFFS2_NODETYPE_DIRENT);
	rd->totlen = cpu_to_je32(sizeof(*rd) + namelen);
	rd->hdr_crc = cpu_to_je32(crc32(0, rd, sizeof(struct jffs2_unknown_node)-4));

	rd->pino = cpu_to_je32(dir_i->i_ino);
	rd->version = cpu_to_je32(++dir_f->highest_version);
	rd->ino = cpu_to_je32(inode->i_ino);
	rd->mctime = cpu_to_je32(get_seconds());
	rd->nsize = namelen;
	rd->type = DT_LNK;
	rd->node_crc = cpu_to_je32(crc32(0, rd, sizeof(*rd)-8));
	rd->name_crc = cpu_to_je32(crc32(0, dentry->d_name.name, namelen));

	fd = jffs2_write_dirent(c, dir_f, rd, dentry->d_name.name, namelen, phys_ofs, ALLOC_NORMAL);

	if (IS_ERR(fd)) {
		/* dirent failed to write. Delete the inode normally 
		   as if it were the final unlink() */
		jffs2_complete_reservation(c);
		jffs2_free_raw_dirent(rd);
		up(&dir_f->sem);
		jffs2_clear_inode(inode);
		return PTR_ERR(fd);
	}

	dir_i->i_mtime = dir_i->i_ctime = ITIME(je32_to_cpu(rd->mctime));

	jffs2_free_raw_dirent(rd);

	/* Link the fd into the inode's list, obsoleting an old
	   one if necessary. */
	jffs2_add_fd_to_list(c, fd, &dir_f->dents);

	up(&dir_f->sem);
	jffs2_complete_reservation(c);

	d_instantiate(dentry, inode);
	return 0;
}


static int jffs2_mkdir (struct inode *dir_i, struct dentry *dentry, int mode)
{
	struct jffs2_inode_info *f, *dir_f;
	struct jffs2_sb_info *c;
	struct inode *inode;
	struct jffs2_raw_inode *ri;
	struct jffs2_raw_dirent *rd;
	struct jffs2_full_dnode *fn;
	struct jffs2_full_dirent *fd;
	int namelen;
	uint32_t alloclen, phys_ofs;
	int ret;

	mode |= S_IFDIR;

	ri = jffs2_alloc_raw_inode();
	if (!ri)
		return -ENOMEM;
	
	c = JFFS2_SB_INFO(dir_i->i_sb);

	/* Try to reserve enough space for both node and dirent. 
	 * Just the node will do for now, though 
	 */
	namelen = dentry->d_name.len;
	ret = jffs2_reserve_space(c, sizeof(*ri), &phys_ofs, &alloclen, ALLOC_NORMAL);

	if (ret) {
		jffs2_free_raw_inode(ri);
		return ret;
	}

	inode = jffs2_new_inode(dir_i, mode, ri);

	if (IS_ERR(inode)) {
		jffs2_free_raw_inode(ri);
		jffs2_complete_reservation(c);
		return PTR_ERR(inode);
	}

	inode->i_op = &jffs2_dir_inode_operations;
	inode->i_fop = &jffs2_dir_operations;
	/* Directories get nlink 2 at start */
	inode->i_nlink = 2;

	f = JFFS2_INODE_INFO(inode);

	ri->data_crc = cpu_to_je32(0);
	ri->node_crc = cpu_to_je32(crc32(0, ri, sizeof(*ri)-8));
	
	fn = jffs2_write_dnode(c, f, ri, NULL, 0, phys_ofs, ALLOC_NORMAL);

	jffs2_free_raw_inode(ri);

	if (IS_ERR(fn)) {
		/* Eeek. Wave bye bye */
		up(&f->sem);
		jffs2_complete_reservation(c);
		jffs2_clear_inode(inode);
		return PTR_ERR(fn);
	}
	/* No data here. Only a metadata node, which will be 
	   obsoleted by the first data write
	*/
	f->metadata = fn;
	up(&f->sem);

	jffs2_complete_reservation(c);
	ret = jffs2_reserve_space(c, sizeof(*rd)+namelen, &phys_ofs, &alloclen, ALLOC_NORMAL);
	if (ret) {
		/* Eep. */
		jffs2_clear_inode(inode);
		return ret;
	}
	
	rd = jffs2_alloc_raw_dirent();
	if (!rd) {
		/* Argh. Now we treat it like a normal delete */
		jffs2_complete_reservation(c);
		jffs2_clear_inode(inode);
		return -ENOMEM;
	}

	dir_f = JFFS2_INODE_INFO(dir_i);
	down(&dir_f->sem);

	rd->magic = cpu_to_je16(JFFS2_MAGIC_BITMASK);
	rd->nodetype = cpu_to_je16(JFFS2_NODETYPE_DIRENT);
	rd->totlen = cpu_to_je32(sizeof(*rd) + namelen);
	rd->hdr_crc = cpu_to_je32(crc32(0, rd, sizeof(struct jffs2_unknown_node)-4));

	rd->pino = cpu_to_je32(dir_i->i_ino);
	rd->version = cpu_to_je32(++dir_f->highest_version);
	rd->ino = cpu_to_je32(inode->i_ino);
	rd->mctime = cpu_to_je32(get_seconds());
	rd->nsize = namelen;
	rd->type = DT_DIR;
	rd->node_crc = cpu_to_je32(crc32(0, rd, sizeof(*rd)-8));
	rd->name_crc = cpu_to_je32(crc32(0, dentry->d_name.name, namelen));

	fd = jffs2_write_dirent(c, dir_f, rd, dentry->d_name.name, namelen, phys_ofs, ALLOC_NORMAL);
	
	if (IS_ERR(fd)) {
		/* dirent failed to write. Delete the inode normally 
		   as if it were the final unlink() */
		jffs2_complete_reservation(c);
		jffs2_free_raw_dirent(rd);
		up(&dir_f->sem);
		jffs2_clear_inode(inode);
		return PTR_ERR(fd);
	}

	dir_i->i_mtime = dir_i->i_ctime = ITIME(je32_to_cpu(rd->mctime));
	dir_i->i_nlink++;

	jffs2_free_raw_dirent(rd);

	/* Link the fd into the inode's list, obsoleting an old
	   one if necessary. */
	jffs2_add_fd_to_list(c, fd, &dir_f->dents);

	up(&dir_f->sem);
	jffs2_complete_reservation(c);

	d_instantiate(dentry, inode);
	return 0;
}

static int jffs2_rmdir (struct inode *dir_i, struct dentry *dentry)
{
	struct jffs2_inode_info *f = JFFS2_INODE_INFO(dentry->d_inode);
	struct jffs2_full_dirent *fd;
	int ret;

	for (fd = f->dents ; fd; fd = fd->next) {
		if (fd->ino)
			return -ENOTEMPTY;
	}
	ret = jffs2_unlink(dir_i, dentry);
	if (!ret)
		dir_i->i_nlink--;
	return ret;
}

static int jffs2_mknod (struct inode *dir_i, struct dentry *dentry, int mode, mknod_arg_t rdev)
{
	struct jffs2_inode_info *f, *dir_f;
	struct jffs2_sb_info *c;
	struct inode *inode;
	struct jffs2_raw_inode *ri;
	struct jffs2_raw_dirent *rd;
	struct jffs2_full_dnode *fn;
	struct jffs2_full_dirent *fd;
	int namelen;
	jint16_t dev;
	int devlen = 0;
	uint32_t alloclen, phys_ofs;
	int ret;

	if (!old_valid_dev(rdev))
		return -EINVAL;

	ri = jffs2_alloc_raw_inode();
	if (!ri)
		return -ENOMEM;
	
	c = JFFS2_SB_INFO(dir_i->i_sb);
	
	if (S_ISBLK(mode) || S_ISCHR(mode)) {
		dev = cpu_to_je16(old_encode_dev(rdev));
		devlen = sizeof(dev);
	}
	
	/* Try to reserve enough space for both node and dirent. 
	 * Just the node will do for now, though 
	 */
	namelen = dentry->d_name.len;
	ret = jffs2_reserve_space(c, sizeof(*ri) + devlen, &phys_ofs, &alloclen, ALLOC_NORMAL);

	if (ret) {
		jffs2_free_raw_inode(ri);
		return ret;
	}

	inode = jffs2_new_inode(dir_i, mode, ri);

	if (IS_ERR(inode)) {
		jffs2_free_raw_inode(ri);
		jffs2_complete_reservation(c);
		return PTR_ERR(inode);
	}
	inode->i_op = &jffs2_file_inode_operations;
	init_special_inode(inode, inode->i_mode, rdev);

	f = JFFS2_INODE_INFO(inode);

	ri->dsize = ri->csize = cpu_to_je32(devlen);
	ri->totlen = cpu_to_je32(sizeof(*ri) + devlen);
	ri->hdr_crc = cpu_to_je32(crc32(0, ri, sizeof(struct jffs2_unknown_node)-4));

	ri->compr = JFFS2_COMPR_NONE;
	ri->data_crc = cpu_to_je32(crc32(0, &dev, devlen));
	ri->node_crc = cpu_to_je32(crc32(0, ri, sizeof(*ri)-8));
	
	fn = jffs2_write_dnode(c, f, ri, (char *)&dev, devlen, phys_ofs, ALLOC_NORMAL);

	jffs2_free_raw_inode(ri);

	if (IS_ERR(fn)) {
		/* Eeek. Wave bye bye */
		up(&f->sem);
		jffs2_complete_reservation(c);
		jffs2_clear_inode(inode);
		return PTR_ERR(fn);
	}
	/* No data here. Only a metadata node, which will be 
	   obsoleted by the first data write
	*/
	f->metadata = fn;
	up(&f->sem);

	jffs2_complete_reservation(c);
	ret = jffs2_reserve_space(c, sizeof(*rd)+namelen, &phys_ofs, &alloclen, ALLOC_NORMAL);
	if (ret) {
		/* Eep. */
		jffs2_clear_inode(inode);
		return ret;
	}

	rd = jffs2_alloc_raw_dirent();
	if (!rd) {
		/* Argh. Now we treat it like a normal delete */
		jffs2_complete_reservation(c);
		jffs2_clear_inode(inode);
		return -ENOMEM;
	}

	dir_f = JFFS2_INODE_INFO(dir_i);
	down(&dir_f->sem);

	rd->magic = cpu_to_je16(JFFS2_MAGIC_BITMASK);
	rd->nodetype = cpu_to_je16(JFFS2_NODETYPE_DIRENT);
	rd->totlen = cpu_to_je32(sizeof(*rd) + namelen);
	rd->hdr_crc = cpu_to_je32(crc32(0, rd, sizeof(struct jffs2_unknown_node)-4));

	rd->pino = cpu_to_je32(dir_i->i_ino);
	rd->version = cpu_to_je32(++dir_f->highest_version);
	rd->ino = cpu_to_je32(inode->i_ino);
	rd->mctime = cpu_to_je32(get_seconds());
	rd->nsize = namelen;

	/* XXX: This is ugly. */
	rd->type = (mode & S_IFMT) >> 12;

	rd->node_crc = cpu_to_je32(crc32(0, rd, sizeof(*rd)-8));
	rd->name_crc = cpu_to_je32(crc32(0, dentry->d_name.name, namelen));

	fd = jffs2_write_dirent(c, dir_f, rd, dentry->d_name.name, namelen, phys_ofs, ALLOC_NORMAL);
	
	if (IS_ERR(fd)) {
		/* dirent failed to write. Delete the inode normally 
		   as if it were the final unlink() */
		jffs2_complete_reservation(c);
		jffs2_free_raw_dirent(rd);
		up(&dir_f->sem);
		jffs2_clear_inode(inode);
		return PTR_ERR(fd);
	}

	dir_i->i_mtime = dir_i->i_ctime = ITIME(je32_to_cpu(rd->mctime));

	jffs2_free_raw_dirent(rd);

	/* Link the fd into the inode's list, obsoleting an old
	   one if necessary. */
	jffs2_add_fd_to_list(c, fd, &dir_f->dents);

	up(&dir_f->sem);
	jffs2_complete_reservation(c);

	d_instantiate(dentry, inode);

	return 0;
}

static int jffs2_rename (struct inode *old_dir_i, struct dentry *old_dentry,
                        struct inode *new_dir_i, struct dentry *new_dentry)
{
	int ret;
	struct jffs2_sb_info *c = JFFS2_SB_INFO(old_dir_i->i_sb);
	struct jffs2_inode_info *victim_f = NULL;
	uint8_t type;

	/* The VFS will check for us and prevent trying to rename a 
	 * file over a directory and vice versa, but if it's a directory,
	 * the VFS can't check whether the victim is empty. The filesystem
	 * needs to do that for itself.
	 */
	if (new_dentry->d_inode) {
		victim_f = JFFS2_INODE_INFO(new_dentry->d_inode);
		if (S_ISDIR(new_dentry->d_inode->i_mode)) {
			struct jffs2_full_dirent *fd;

			down(&victim_f->sem);
			for (fd = victim_f->dents; fd; fd = fd->next) {
				if (fd->ino) {
					up(&victim_f->sem);
					return -ENOTEMPTY;
				}
			}
			up(&victim_f->sem);
		}
	}

	/* XXX: We probably ought to alloc enough space for
	   both nodes at the same time. Writing the new link, 
	   then getting -ENOSPC, is quite bad :)
	*/

	/* Make a hard link */
	
	/* XXX: This is ugly */
	type = (old_dentry->d_inode->i_mode & S_IFMT) >> 12;
	if (!type) type = DT_REG;

	ret = jffs2_do_link(c, JFFS2_INODE_INFO(new_dir_i), 
			    old_dentry->d_inode->i_ino, type,
			    new_dentry->d_name.name, new_dentry->d_name.len);

	if (ret)
		return ret;

	if (victim_f) {
		/* There was a victim. Kill it off nicely */
		new_dentry->d_inode->i_nlink--;
		/* Don't oops if the victim was a dirent pointing to an
		   inode which didn't exist. */
		if (victim_f->inocache) {
			down(&victim_f->sem);
			victim_f->inocache->nlink--;
			up(&victim_f->sem);
		}
	}

	/* If it was a directory we moved, and there was no victim, 
	   increase i_nlink on its new parent */
	if (S_ISDIR(old_dentry->d_inode->i_mode) && !victim_f)
		new_dir_i->i_nlink++;

	/* Unlink the original */
	ret = jffs2_do_unlink(c, JFFS2_INODE_INFO(old_dir_i), 
		      old_dentry->d_name.name, old_dentry->d_name.len, NULL);

	/* We don't touch inode->i_nlink */

	if (ret) {
		/* Oh shit. We really ought to make a single node which can do both atomically */
		struct jffs2_inode_info *f = JFFS2_INODE_INFO(old_dentry->d_inode);
		down(&f->sem);
		old_dentry->d_inode->i_nlink++;
		if (f->inocache)
			f->inocache->nlink++;
		up(&f->sem);

		printk(KERN_NOTICE "jffs2_rename(): Link succeeded, unlink failed (err %d). You now have a hard link\n", ret);
		/* Might as well let the VFS know */
		d_instantiate(new_dentry, old_dentry->d_inode);
		atomic_inc(&old_dentry->d_inode->i_count);
		return ret;
	}

	if (S_ISDIR(old_dentry->d_inode->i_mode))
		old_dir_i->i_nlink--;

	return 0;
}

