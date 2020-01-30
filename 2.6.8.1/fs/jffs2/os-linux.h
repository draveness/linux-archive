/*
 * JFFS2 -- Journalling Flash File System, Version 2.
 *
 * Copyright (C) 2002-2003 Red Hat, Inc.
 *
 * Created by David Woodhouse <dwmw2@redhat.com>
 *
 * For licensing information, see the file 'LICENCE' in this directory.
 *
 * $Id: os-linux.h,v 1.47 2004/07/14 13:20:23 dwmw2 Exp $
 *
 */

#ifndef __JFFS2_OS_LINUX_H__
#define __JFFS2_OS_LINUX_H__
#include <linux/version.h>

/* JFFS2 uses Linux mode bits natively -- no need for conversion */
#define os_to_jffs2_mode(x) (x)
#define jffs2_to_os_mode(x) (x)

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,5,73)
#define kstatfs statfs
#endif

struct kstatfs;
struct kvec;

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,5,2)
#define JFFS2_INODE_INFO(i) (list_entry(i, struct jffs2_inode_info, vfs_inode))
#define OFNI_EDONI_2SFFJ(f)  (&(f)->vfs_inode)
#define JFFS2_SB_INFO(sb) (sb->s_fs_info)
#define OFNI_BS_2SFFJ(c)  ((struct super_block *)c->os_priv)
#elif defined(JFFS2_OUT_OF_KERNEL)
#define JFFS2_INODE_INFO(i) ((struct jffs2_inode_info *) &(i)->u)
#define OFNI_EDONI_2SFFJ(f)  ((struct inode *) ( ((char *)f) - ((char *)(&((struct inode *)NULL)->u)) ) )
#define JFFS2_SB_INFO(sb) ((struct jffs2_sb_info *) &(sb)->u)
#define OFNI_BS_2SFFJ(c)  ((struct super_block *) ( ((char *)c) - ((char *)(&((struct super_block *)NULL)->u)) ) )
#else
#define JFFS2_INODE_INFO(i) (&i->u.jffs2_i)
#define OFNI_EDONI_2SFFJ(f)  ((struct inode *) ( ((char *)f) - ((char *)(&((struct inode *)NULL)->u)) ) )
#define JFFS2_SB_INFO(sb) (&sb->u.jffs2_sb)
#define OFNI_BS_2SFFJ(c)  ((struct super_block *) ( ((char *)c) - ((char *)(&((struct super_block *)NULL)->u)) ) )
#endif


#define JFFS2_F_I_SIZE(f) (OFNI_EDONI_2SFFJ(f)->i_size)
#define JFFS2_F_I_MODE(f) (OFNI_EDONI_2SFFJ(f)->i_mode)
#define JFFS2_F_I_UID(f) (OFNI_EDONI_2SFFJ(f)->i_uid)
#define JFFS2_F_I_GID(f) (OFNI_EDONI_2SFFJ(f)->i_gid)

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,5,1)
#define JFFS2_F_I_RDEV_MIN(f) (iminor(OFNI_EDONI_2SFFJ(f)))
#define JFFS2_F_I_RDEV_MAJ(f) (imajor(OFNI_EDONI_2SFFJ(f)))
#else
#define JFFS2_F_I_RDEV_MIN(f) (MINOR(to_kdev_t(OFNI_EDONI_2SFFJ(f)->i_rdev)))
#define JFFS2_F_I_RDEV_MAJ(f) (MAJOR(to_kdev_t(OFNI_EDONI_2SFFJ(f)->i_rdev)))
#endif

/* Urgh. The things we do to keep the 2.4 build working */
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,5,47)
#define ITIME(sec) ((struct timespec){sec, 0})
#define I_SEC(tv) ((tv).tv_sec)
#define JFFS2_F_I_CTIME(f) (OFNI_EDONI_2SFFJ(f)->i_ctime.tv_sec)
#define JFFS2_F_I_MTIME(f) (OFNI_EDONI_2SFFJ(f)->i_mtime.tv_sec)
#define JFFS2_F_I_ATIME(f) (OFNI_EDONI_2SFFJ(f)->i_atime.tv_sec)
#else
#define ITIME(x) (x)
#define I_SEC(x) (x)
#define JFFS2_F_I_CTIME(f) (OFNI_EDONI_2SFFJ(f)->i_ctime)
#define JFFS2_F_I_MTIME(f) (OFNI_EDONI_2SFFJ(f)->i_mtime)
#define JFFS2_F_I_ATIME(f) (OFNI_EDONI_2SFFJ(f)->i_atime)
#endif

#define sleep_on_spinunlock(wq, s)				\
	do {							\
		DECLARE_WAITQUEUE(__wait, current);		\
		add_wait_queue((wq), &__wait);			\
		set_current_state(TASK_UNINTERRUPTIBLE);	\
		spin_unlock(s);					\
		schedule();					\
		remove_wait_queue((wq), &__wait);		\
	} while(0)

static inline void jffs2_init_inode_info(struct jffs2_inode_info *f)
{
#if LINUX_VERSION_CODE > KERNEL_VERSION(2,5,2)
	f->highest_version = 0;
	f->fragtree = RB_ROOT;
	f->metadata = NULL;
	f->dents = NULL;
	f->flags = 0;
	f->usercompr = 0;
#else
	memset(f, 0, sizeof(*f));
	init_MUTEX_LOCKED(&f->sem);
#endif
}

#define jffs2_is_readonly(c) (OFNI_BS_2SFFJ(c)->s_flags & MS_RDONLY)

#ifndef CONFIG_JFFS2_FS_NAND
#define jffs2_can_mark_obsolete(c) (1)
#define jffs2_cleanmarker_oob(c) (0)
#define jffs2_write_nand_cleanmarker(c,jeb) (-EIO)

#define jffs2_flash_write(c, ofs, len, retlen, buf) ((c)->mtd->write((c)->mtd, ofs, len, retlen, buf))
#define jffs2_flash_read(c, ofs, len, retlen, buf) ((c)->mtd->read((c)->mtd, ofs, len, retlen, buf))
#define jffs2_flush_wbuf_pad(c) ({ (void)(c), 0; })
#define jffs2_flush_wbuf_gc(c, i) ({ (void)(c), (void) i, 0; })
#define jffs2_write_nand_badblock(c,jeb,bad_offset) (1)
#define jffs2_nand_flash_setup(c) (0)
#define jffs2_nand_flash_cleanup(c) do {} while(0)
#define jffs2_wbuf_dirty(c) (0)
#define jffs2_flash_writev(a,b,c,d,e,f) jffs2_flash_direct_writev(a,b,c,d,e)
#define jffs2_wbuf_timeout NULL
#define jffs2_wbuf_process NULL

#else /* NAND support present */

#define jffs2_can_mark_obsolete(c) (c->mtd->type == MTD_NORFLASH || c->mtd->type == MTD_RAM)
#define jffs2_cleanmarker_oob(c) (c->mtd->type == MTD_NANDFLASH)

#define jffs2_flash_write_oob(c, ofs, len, retlen, buf) ((c)->mtd->write_oob((c)->mtd, ofs, len, retlen, buf))
#define jffs2_flash_read_oob(c, ofs, len, retlen, buf) ((c)->mtd->read_oob((c)->mtd, ofs, len, retlen, buf))
#define jffs2_wbuf_dirty(c) (!!(c)->wbuf_len)

/* wbuf.c */
int jffs2_flash_writev(struct jffs2_sb_info *c, const struct kvec *vecs, unsigned long count, loff_t to, size_t *retlen, uint32_t ino);
int jffs2_flash_write(struct jffs2_sb_info *c, loff_t ofs, size_t len, size_t *retlen, const u_char *buf);
int jffs2_flash_read(struct jffs2_sb_info *c, loff_t ofs, size_t len, size_t *retlen, u_char *buf);
int jffs2_check_oob_empty(struct jffs2_sb_info *c, struct jffs2_eraseblock *jeb,int mode);
int jffs2_check_nand_cleanmarker(struct jffs2_sb_info *c, struct jffs2_eraseblock *jeb);
int jffs2_write_nand_cleanmarker(struct jffs2_sb_info *c, struct jffs2_eraseblock *jeb);
int jffs2_write_nand_badblock(struct jffs2_sb_info *c, struct jffs2_eraseblock *jeb, uint32_t bad_offset);
void jffs2_wbuf_timeout(unsigned long data);
void jffs2_wbuf_process(void *data);
int jffs2_nand_flash_setup(struct jffs2_sb_info *c);
void jffs2_nand_flash_cleanup(struct jffs2_sb_info *c);
#endif /* NAND */

/* erase.c */
static inline void jffs2_erase_pending_trigger(struct jffs2_sb_info *c)
{
	OFNI_BS_2SFFJ(c)->s_dirt = 1;
}

/* background.c */
int jffs2_start_garbage_collect_thread(struct jffs2_sb_info *c);
void jffs2_stop_garbage_collect_thread(struct jffs2_sb_info *c);
void jffs2_garbage_collect_trigger(struct jffs2_sb_info *c);

/* dir.c */
extern struct file_operations jffs2_dir_operations;
extern struct inode_operations jffs2_dir_inode_operations;

/* file.c */
extern struct file_operations jffs2_file_operations;
extern struct inode_operations jffs2_file_inode_operations;
extern struct address_space_operations jffs2_file_address_operations;
int jffs2_fsync(struct file *, struct dentry *, int);
int jffs2_do_readpage_nolock (struct inode *inode, struct page *pg);
int jffs2_do_readpage_unlock (struct inode *inode, struct page *pg);
int jffs2_readpage (struct file *, struct page *);
int jffs2_prepare_write (struct file *, struct page *, unsigned, unsigned);
int jffs2_commit_write (struct file *, struct page *, unsigned, unsigned);

/* ioctl.c */
int jffs2_ioctl(struct inode *, struct file *, unsigned int, unsigned long);

/* symlink.c */
extern struct inode_operations jffs2_symlink_inode_operations;

/* fs.c */
int jffs2_setattr (struct dentry *, struct iattr *);
void jffs2_read_inode (struct inode *);
void jffs2_clear_inode (struct inode *);
void jffs2_dirty_inode(struct inode *inode);
struct inode *jffs2_new_inode (struct inode *dir_i, int mode,
			       struct jffs2_raw_inode *ri);
int jffs2_statfs (struct super_block *, struct kstatfs *);
void jffs2_write_super (struct super_block *);
int jffs2_remount_fs (struct super_block *, int *, char *);
int jffs2_do_fill_super(struct super_block *sb, void *data, int silent);
void jffs2_gc_release_inode(struct jffs2_sb_info *c,
			    struct jffs2_inode_info *f);
struct jffs2_inode_info *jffs2_gc_fetch_inode(struct jffs2_sb_info *c,
					      int inum, int nlink);

unsigned char *jffs2_gc_fetch_page(struct jffs2_sb_info *c, 
				   struct jffs2_inode_info *f, 
				   unsigned long offset,
				   unsigned long *priv);
void jffs2_gc_release_page(struct jffs2_sb_info *c,
			   unsigned char *pg,
			   unsigned long *priv);
int jffs2_flash_setup(struct jffs2_sb_info *c);
void jffs2_flash_cleanup(struct jffs2_sb_info *c);
     

/* writev.c */
int jffs2_flash_direct_writev(struct jffs2_sb_info *c, const struct kvec *vecs, 
		       unsigned long count, loff_t to, size_t *retlen);


#endif /* __JFFS2_OS_LINUX_H__ */


