/*
 *  linux/fs/ext2/super.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/inode.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/parser.h>
#include <linux/random.h>
#include <linux/buffer_head.h>
#include <linux/smp_lock.h>
#include <linux/vfs.h>
#include <asm/uaccess.h>
#include "ext2.h"
#include "xattr.h"
#include "acl.h"

static void ext2_sync_super(struct super_block *sb,
			    struct ext2_super_block *es);
static int ext2_remount (struct super_block * sb, int * flags, char * data);
static int ext2_statfs (struct super_block * sb, struct kstatfs * buf);

static char error_buf[1024];

void ext2_error (struct super_block * sb, const char * function,
		 const char * fmt, ...)
{
	va_list args;
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_super_block *es = sbi->s_es;

	if (!(sb->s_flags & MS_RDONLY)) {
		sbi->s_mount_state |= EXT2_ERROR_FS;
		es->s_state =
			cpu_to_le16(le16_to_cpu(es->s_state) | EXT2_ERROR_FS);
		ext2_sync_super(sb, es);
	}
	va_start (args, fmt);
	vsprintf (error_buf, fmt, args);
	va_end (args);
	if (test_opt (sb, ERRORS_PANIC))
		panic ("EXT2-fs panic (device %s): %s: %s\n",
		       sb->s_id, function, error_buf);
	printk (KERN_CRIT "EXT2-fs error (device %s): %s: %s\n",
		sb->s_id, function, error_buf);
	if (test_opt (sb, ERRORS_RO)) {
		printk ("Remounting filesystem read-only\n");
		sb->s_flags |= MS_RDONLY;
	}
}

NORET_TYPE void ext2_panic (struct super_block * sb, const char * function,
			    const char * fmt, ...)
{
	va_list args;
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	if (!(sb->s_flags & MS_RDONLY)) {
		sbi->s_mount_state |= EXT2_ERROR_FS;
		sbi->s_es->s_state =
			cpu_to_le16(le16_to_cpu(sbi->s_es->s_state) | EXT2_ERROR_FS);
		mark_buffer_dirty(sbi->s_sbh);
		sb->s_dirt = 1;
	}
	va_start (args, fmt);
	vsprintf (error_buf, fmt, args);
	va_end (args);
	sb->s_flags |= MS_RDONLY;
	panic ("EXT2-fs panic (device %s): %s: %s\n",
	       sb->s_id, function, error_buf);
}

void ext2_warning (struct super_block * sb, const char * function,
		   const char * fmt, ...)
{
	va_list args;

	va_start (args, fmt);
	vsprintf (error_buf, fmt, args);
	va_end (args);
	printk (KERN_WARNING "EXT2-fs warning (device %s): %s: %s\n",
		sb->s_id, function, error_buf);
}

void ext2_update_dynamic_rev(struct super_block *sb)
{
	struct ext2_super_block *es = EXT2_SB(sb)->s_es;

	if (le32_to_cpu(es->s_rev_level) > EXT2_GOOD_OLD_REV)
		return;

	ext2_warning(sb, __FUNCTION__,
		     "updating to rev %d because of new feature flag, "
		     "running e2fsck is recommended",
		     EXT2_DYNAMIC_REV);

	es->s_first_ino = cpu_to_le32(EXT2_GOOD_OLD_FIRST_INO);
	es->s_inode_size = cpu_to_le16(EXT2_GOOD_OLD_INODE_SIZE);
	es->s_rev_level = cpu_to_le32(EXT2_DYNAMIC_REV);
	/* leave es->s_feature_*compat flags alone */
	/* es->s_uuid will be set by e2fsck if empty */

	/*
	 * The rest of the superblock fields should be zero, and if not it
	 * means they are likely already in use, so leave them alone.  We
	 * can leave it up to e2fsck to clean up any inconsistencies there.
	 */
}

static void ext2_put_super (struct super_block * sb)
{
	int db_count;
	int i;
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	ext2_xattr_put_super(sb);
	if (!(sb->s_flags & MS_RDONLY)) {
		struct ext2_super_block *es = sbi->s_es;

		es->s_state = le16_to_cpu(sbi->s_mount_state);
		ext2_sync_super(sb, es);
	}
	db_count = sbi->s_gdb_count;
	for (i = 0; i < db_count; i++)
		if (sbi->s_group_desc[i])
			brelse (sbi->s_group_desc[i]);
	kfree(sbi->s_group_desc);
	kfree(sbi->s_debts);
	brelse (sbi->s_sbh);
	sb->s_fs_info = NULL;
	kfree(sbi);

	return;
}

static kmem_cache_t * ext2_inode_cachep;

static struct inode *ext2_alloc_inode(struct super_block *sb)
{
	struct ext2_inode_info *ei;
	ei = (struct ext2_inode_info *)kmem_cache_alloc(ext2_inode_cachep, SLAB_KERNEL);
	if (!ei)
		return NULL;
#ifdef CONFIG_EXT2_FS_POSIX_ACL
	ei->i_acl = EXT2_ACL_NOT_CACHED;
	ei->i_default_acl = EXT2_ACL_NOT_CACHED;
#endif
	ei->vfs_inode.i_version = 1;
	return &ei->vfs_inode;
}

static void ext2_destroy_inode(struct inode *inode)
{
	kmem_cache_free(ext2_inode_cachep, EXT2_I(inode));
}

static void init_once(void * foo, kmem_cache_t * cachep, unsigned long flags)
{
	struct ext2_inode_info *ei = (struct ext2_inode_info *) foo;

	if ((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) ==
	    SLAB_CTOR_CONSTRUCTOR) {
		rwlock_init(&ei->i_meta_lock);
#ifdef CONFIG_EXT2_FS_XATTR
		init_rwsem(&ei->xattr_sem);
#endif
		inode_init_once(&ei->vfs_inode);
	}
}
 
static int init_inodecache(void)
{
	ext2_inode_cachep = kmem_cache_create("ext2_inode_cache",
					     sizeof(struct ext2_inode_info),
					     0, SLAB_HWCACHE_ALIGN|SLAB_RECLAIM_ACCOUNT,
					     init_once, NULL);
	if (ext2_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	if (kmem_cache_destroy(ext2_inode_cachep))
		printk(KERN_INFO "ext2_inode_cache: not all structures were freed\n");
}

#ifdef CONFIG_EXT2_FS_POSIX_ACL

static void ext2_clear_inode(struct inode *inode)
{
	struct ext2_inode_info *ei = EXT2_I(inode);

	if (ei->i_acl && ei->i_acl != EXT2_ACL_NOT_CACHED) {
		posix_acl_release(ei->i_acl);
		ei->i_acl = EXT2_ACL_NOT_CACHED;
	}
	if (ei->i_default_acl && ei->i_default_acl != EXT2_ACL_NOT_CACHED) {
		posix_acl_release(ei->i_default_acl);
		ei->i_default_acl = EXT2_ACL_NOT_CACHED;
	}
}

#else
# define ext2_clear_inode NULL
#endif

static struct super_operations ext2_sops = {
	.alloc_inode	= ext2_alloc_inode,
	.destroy_inode	= ext2_destroy_inode,
	.read_inode	= ext2_read_inode,
	.write_inode	= ext2_write_inode,
	.put_inode	= ext2_put_inode,
	.delete_inode	= ext2_delete_inode,
	.put_super	= ext2_put_super,
	.write_super	= ext2_write_super,
	.statfs		= ext2_statfs,
	.remount_fs	= ext2_remount,
	.clear_inode	= ext2_clear_inode,
};

/* Yes, most of these are left as NULL!!
 * A NULL value implies the default, which works with ext2-like file
 * systems, but can be improved upon.
 * Currently only get_parent is required.
 */
struct dentry *ext2_get_parent(struct dentry *child);
static struct export_operations ext2_export_ops = {
	.get_parent = ext2_get_parent,
};

static unsigned long get_sb_block(void **data)
{
	unsigned long 	sb_block;
	char 		*options = (char *) *data;

	if (!options || strncmp(options, "sb=", 3) != 0)
		return 1;	/* Default location */
	options += 3;
	sb_block = simple_strtoul(options, &options, 0);
	if (*options && *options != ',') {
		printk("EXT2-fs: Invalid sb specification: %s\n",
		       (char *) *data);
		return 1;
	}
	if (*options == ',')
		options++;
	*data = (void *) options;
	return sb_block;
}

enum {
	Opt_bsd_df, Opt_minix_df, Opt_grpid, Opt_nogrpid,
	Opt_resgid, Opt_resuid, Opt_sb, Opt_err_cont, Opt_err_panic, Opt_err_ro,
	Opt_nouid32, Opt_check, Opt_nocheck, Opt_debug, Opt_oldalloc, Opt_orlov, Opt_nobh,
	Opt_user_xattr, Opt_nouser_xattr, Opt_acl, Opt_noacl,
	Opt_ignore, Opt_err,
};

static match_table_t tokens = {
	{Opt_bsd_df, "bsddf"},
	{Opt_minix_df, "minixdf"},
	{Opt_grpid, "grpid"},
	{Opt_grpid, "bsdgroups"},
	{Opt_nogrpid, "nogrpid"},
	{Opt_nogrpid, "sysvgroups"},
	{Opt_resgid, "resgid=%u"},
	{Opt_resuid, "resuid=%u"},
	{Opt_sb, "sb=%u"},
	{Opt_err_cont, "errors=continue"},
	{Opt_err_panic, "errors=panic"},
	{Opt_err_ro, "errors=remount-ro"},
	{Opt_nouid32, "nouid32"},
	{Opt_nocheck, "check=none"},
	{Opt_nocheck, "nocheck"},
	{Opt_check, "check"},
	{Opt_debug, "debug"},
	{Opt_oldalloc, "oldalloc"},
	{Opt_orlov, "orlov"},
	{Opt_nobh, "nobh"},
	{Opt_user_xattr, "user_xattr"},
	{Opt_nouser_xattr, "nouser_xattr"},
	{Opt_acl, "acl"},
	{Opt_noacl, "noacl"},
	{Opt_ignore, "grpquota"},
	{Opt_ignore, "noquota"},
	{Opt_ignore, "quota"},
	{Opt_ignore, "usrquota"},
	{Opt_err, NULL}
};

static int parse_options (char * options,
			  struct ext2_sb_info *sbi)
{
	char * p;
	substring_t args[MAX_OPT_ARGS];
	unsigned long kind = EXT2_MOUNT_ERRORS_CONT;
	int option;

	if (!options)
		return 1;

	while ((p = strsep (&options, ",")) != NULL) {
		int token;
		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_bsd_df:
			clear_opt (sbi->s_mount_opt, MINIX_DF);
			break;
		case Opt_minix_df:
			set_opt (sbi->s_mount_opt, MINIX_DF);
			break;
		case Opt_grpid:
			set_opt (sbi->s_mount_opt, GRPID);
			break;
		case Opt_nogrpid:
			clear_opt (sbi->s_mount_opt, GRPID);
			break;
		case Opt_resuid:
			if (match_int(&args[0], &option))
				return 0;
			sbi->s_resuid = option;
			break;
		case Opt_resgid:
			if (match_int(&args[0], &option))
				return 0;
			sbi->s_resgid = option;
			break;
		case Opt_sb:
			/* handled by get_sb_block() instead of here */
			/* *sb_block = match_int(&args[0]); */
			break;
		case Opt_err_panic:
			kind = EXT2_MOUNT_ERRORS_PANIC;
			break;
		case Opt_err_ro:
			kind = EXT2_MOUNT_ERRORS_RO;
			break;
		case Opt_err_cont:
			kind = EXT2_MOUNT_ERRORS_CONT;
			break;
		case Opt_nouid32:
			set_opt (sbi->s_mount_opt, NO_UID32);
			break;
		case Opt_check:
#ifdef CONFIG_EXT2_CHECK
			set_opt (sbi->s_mount_opt, CHECK);
#else
			printk("EXT2 Check option not supported\n");
#endif
			break;
		case Opt_nocheck:
			clear_opt (sbi->s_mount_opt, CHECK);
			break;
		case Opt_debug:
			set_opt (sbi->s_mount_opt, DEBUG);
			break;
		case Opt_oldalloc:
			set_opt (sbi->s_mount_opt, OLDALLOC);
			break;
		case Opt_orlov:
			clear_opt (sbi->s_mount_opt, OLDALLOC);
			break;
		case Opt_nobh:
			set_opt (sbi->s_mount_opt, NOBH);
			break;
#ifdef CONFIG_EXT2_FS_XATTR
		case Opt_user_xattr:
			set_opt (sbi->s_mount_opt, XATTR_USER);
			break;
		case Opt_nouser_xattr:
			clear_opt (sbi->s_mount_opt, XATTR_USER);
			break;
#else
		case Opt_user_xattr:
		case Opt_nouser_xattr:
			printk("EXT2 (no)user_xattr options not supported\n");
			break;
#endif
#ifdef CONFIG_EXT2_FS_POSIX_ACL
		case Opt_acl:
			set_opt(sbi->s_mount_opt, POSIX_ACL);
			break;
		case Opt_noacl:
			clear_opt(sbi->s_mount_opt, POSIX_ACL);
			break;
#else
		case Opt_acl:
		case Opt_noacl:
			printk("EXT2 (no)acl options not supported\n");
			break;
#endif
		case Opt_ignore:
			break;
		default:
			return 0;
		}
	}
	sbi->s_mount_opt |= kind;
	return 1;
}

static int ext2_setup_super (struct super_block * sb,
			      struct ext2_super_block * es,
			      int read_only)
{
	int res = 0;
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	if (le32_to_cpu(es->s_rev_level) > EXT2_MAX_SUPP_REV) {
		printk ("EXT2-fs warning: revision level too high, "
			"forcing read-only mode\n");
		res = MS_RDONLY;
	}
	if (read_only)
		return res;
	if (!(sbi->s_mount_state & EXT2_VALID_FS))
		printk ("EXT2-fs warning: mounting unchecked fs, "
			"running e2fsck is recommended\n");
	else if ((sbi->s_mount_state & EXT2_ERROR_FS))
		printk ("EXT2-fs warning: mounting fs with errors, "
			"running e2fsck is recommended\n");
	else if ((__s16) le16_to_cpu(es->s_max_mnt_count) >= 0 &&
		 le16_to_cpu(es->s_mnt_count) >=
		 (unsigned short) (__s16) le16_to_cpu(es->s_max_mnt_count))
		printk ("EXT2-fs warning: maximal mount count reached, "
			"running e2fsck is recommended\n");
	else if (le32_to_cpu(es->s_checkinterval) &&
		(le32_to_cpu(es->s_lastcheck) + le32_to_cpu(es->s_checkinterval) <= get_seconds()))
		printk ("EXT2-fs warning: checktime reached, "
			"running e2fsck is recommended\n");
	if (!(__s16) le16_to_cpu(es->s_max_mnt_count))
		es->s_max_mnt_count = (__s16) cpu_to_le16(EXT2_DFL_MAX_MNT_COUNT);
	es->s_mnt_count=cpu_to_le16(le16_to_cpu(es->s_mnt_count) + 1);
	ext2_write_super(sb);
	if (test_opt (sb, DEBUG))
		printk ("[EXT II FS %s, %s, bs=%lu, fs=%lu, gc=%lu, "
			"bpg=%lu, ipg=%lu, mo=%04lx]\n",
			EXT2FS_VERSION, EXT2FS_DATE, sb->s_blocksize,
			sbi->s_frag_size,
			sbi->s_groups_count,
			EXT2_BLOCKS_PER_GROUP(sb),
			EXT2_INODES_PER_GROUP(sb),
			sbi->s_mount_opt);
#ifdef CONFIG_EXT2_CHECK
	if (test_opt (sb, CHECK)) {
		ext2_check_blocks_bitmap (sb);
		ext2_check_inodes_bitmap (sb);
	}
#endif
	return res;
}

static int ext2_check_descriptors (struct super_block * sb)
{
	int i;
	int desc_block = 0;
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	unsigned long block = le32_to_cpu(sbi->s_es->s_first_data_block);
	struct ext2_group_desc * gdp = NULL;

	ext2_debug ("Checking group descriptors");

	for (i = 0; i < sbi->s_groups_count; i++)
	{
		if ((i % EXT2_DESC_PER_BLOCK(sb)) == 0)
			gdp = (struct ext2_group_desc *) sbi->s_group_desc[desc_block++]->b_data;
		if (le32_to_cpu(gdp->bg_block_bitmap) < block ||
		    le32_to_cpu(gdp->bg_block_bitmap) >= block + EXT2_BLOCKS_PER_GROUP(sb))
		{
			ext2_error (sb, "ext2_check_descriptors",
				    "Block bitmap for group %d"
				    " not in group (block %lu)!",
				    i, (unsigned long) le32_to_cpu(gdp->bg_block_bitmap));
			return 0;
		}
		if (le32_to_cpu(gdp->bg_inode_bitmap) < block ||
		    le32_to_cpu(gdp->bg_inode_bitmap) >= block + EXT2_BLOCKS_PER_GROUP(sb))
		{
			ext2_error (sb, "ext2_check_descriptors",
				    "Inode bitmap for group %d"
				    " not in group (block %lu)!",
				    i, (unsigned long) le32_to_cpu(gdp->bg_inode_bitmap));
			return 0;
		}
		if (le32_to_cpu(gdp->bg_inode_table) < block ||
		    le32_to_cpu(gdp->bg_inode_table) + sbi->s_itb_per_group >=
		    block + EXT2_BLOCKS_PER_GROUP(sb))
		{
			ext2_error (sb, "ext2_check_descriptors",
				    "Inode table for group %d"
				    " not in group (block %lu)!",
				    i, (unsigned long) le32_to_cpu(gdp->bg_inode_table));
			return 0;
		}
		block += EXT2_BLOCKS_PER_GROUP(sb);
		gdp++;
	}
	return 1;
}

#define log2(n) ffz(~(n))
 
/*
 * Maximal file size.  There is a direct, and {,double-,triple-}indirect
 * block limit, and also a limit of (2^32 - 1) 512-byte sectors in i_blocks.
 * We need to be 1 filesystem block less than the 2^32 sector limit.
 */
static loff_t ext2_max_size(int bits)
{
	loff_t res = EXT2_NDIR_BLOCKS;
	res += 1LL << (bits-2);
	res += 1LL << (2*(bits-2));
	res += 1LL << (3*(bits-2));
	res <<= bits;
	if (res > (512LL << 32) - (1 << bits))
		res = (512LL << 32) - (1 << bits);
	return res;
}

static unsigned long descriptor_loc(struct super_block *sb,
				    unsigned long logic_sb_block,
				    int nr)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	unsigned long bg, first_data_block, first_meta_bg;
	int has_super = 0;
	
	first_data_block = le32_to_cpu(sbi->s_es->s_first_data_block);
	first_meta_bg = le32_to_cpu(sbi->s_es->s_first_meta_bg);

	if (!EXT2_HAS_INCOMPAT_FEATURE(sb, EXT2_FEATURE_INCOMPAT_META_BG) ||
	    nr < first_meta_bg)
		return (logic_sb_block + nr + 1);
	bg = sbi->s_desc_per_block * nr;
	if (ext2_bg_has_super(sb, bg))
		has_super = 1;
	return (first_data_block + has_super + (bg * sbi->s_blocks_per_group));
}

static int ext2_fill_super(struct super_block *sb, void *data, int silent)
{
	struct buffer_head * bh;
	struct ext2_sb_info * sbi;
	struct ext2_super_block * es;
	struct inode *root;
	unsigned long block;
	unsigned long sb_block = get_sb_block(&data);
	unsigned long logic_sb_block;
	unsigned long offset = 0;
	unsigned long def_mount_opts;
	int blocksize = BLOCK_SIZE;
	int db_count;
	int i, j;

	sbi = kmalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;
	memset(sbi, 0, sizeof(*sbi));

	/*
	 * See what the current blocksize for the device is, and
	 * use that as the blocksize.  Otherwise (or if the blocksize
	 * is smaller than the default) use the default.
	 * This is important for devices that have a hardware
	 * sectorsize that is larger than the default.
	 */
	blocksize = sb_min_blocksize(sb, BLOCK_SIZE);
	if (!blocksize) {
		printk ("EXT2-fs: unable to set blocksize\n");
		goto failed_sbi;
	}

	/*
	 * If the superblock doesn't start on a hardware sector boundary,
	 * calculate the offset.  
	 */
	if (blocksize != BLOCK_SIZE) {
		logic_sb_block = (sb_block*BLOCK_SIZE) / blocksize;
		offset = (sb_block*BLOCK_SIZE) % blocksize;
	} else {
		logic_sb_block = sb_block;
	}

	if (!(bh = sb_bread(sb, logic_sb_block))) {
		printk ("EXT2-fs: unable to read superblock\n");
		goto failed_sbi;
	}
	/*
	 * Note: s_es must be initialized as soon as possible because
	 *       some ext2 macro-instructions depend on its value
	 */
	es = (struct ext2_super_block *) (((char *)bh->b_data) + offset);
	sbi->s_es = es;
	sb->s_magic = le16_to_cpu(es->s_magic);
	sb->s_flags |= MS_ONE_SECOND;
	if (sb->s_magic != EXT2_SUPER_MAGIC) {
		if (!silent)
			printk ("VFS: Can't find ext2 filesystem on dev %s.\n",
				sb->s_id);
		goto failed_mount;
	}

	/* Set defaults before we parse the mount options */
	def_mount_opts = le32_to_cpu(es->s_default_mount_opts);
	if (def_mount_opts & EXT2_DEFM_DEBUG)
		set_opt(sbi->s_mount_opt, DEBUG);
	if (def_mount_opts & EXT2_DEFM_BSDGROUPS)
		set_opt(sbi->s_mount_opt, GRPID);
	if (def_mount_opts & EXT2_DEFM_UID16)
		set_opt(sbi->s_mount_opt, NO_UID32);
	if (def_mount_opts & EXT2_DEFM_XATTR_USER)
		set_opt(sbi->s_mount_opt, XATTR_USER);
	if (def_mount_opts & EXT2_DEFM_ACL)
		set_opt(sbi->s_mount_opt, POSIX_ACL);
	
	if (le16_to_cpu(sbi->s_es->s_errors) == EXT2_ERRORS_PANIC)
		set_opt(sbi->s_mount_opt, ERRORS_PANIC);
	else if (le16_to_cpu(sbi->s_es->s_errors) == EXT2_ERRORS_RO)
		set_opt(sbi->s_mount_opt, ERRORS_RO);

	sbi->s_resuid = le16_to_cpu(es->s_def_resuid);
	sbi->s_resgid = le16_to_cpu(es->s_def_resgid);
	
	if (!parse_options ((char *) data, sbi))
		goto failed_mount;

	sb->s_flags = (sb->s_flags & ~MS_POSIXACL) |
		((EXT2_SB(sb)->s_mount_opt & EXT2_MOUNT_POSIX_ACL) ?
		 MS_POSIXACL : 0);

	if (le32_to_cpu(es->s_rev_level) == EXT2_GOOD_OLD_REV &&
	    (EXT2_HAS_COMPAT_FEATURE(sb, ~0U) ||
	     EXT2_HAS_RO_COMPAT_FEATURE(sb, ~0U) ||
	     EXT2_HAS_INCOMPAT_FEATURE(sb, ~0U)))
		printk("EXT2-fs warning: feature flags set on rev 0 fs, "
		       "running e2fsck is recommended\n");
	/*
	 * Check feature flags regardless of the revision level, since we
	 * previously didn't change the revision level when setting the flags,
	 * so there is a chance incompat flags are set on a rev 0 filesystem.
	 */
	if ((i = EXT2_HAS_INCOMPAT_FEATURE(sb, ~EXT2_FEATURE_INCOMPAT_SUPP))) {
		printk("EXT2-fs: %s: couldn't mount because of "
		       "unsupported optional features (%x).\n",
		       sb->s_id, i);
		goto failed_mount;
	}
	if (!(sb->s_flags & MS_RDONLY) &&
	    (i = EXT2_HAS_RO_COMPAT_FEATURE(sb, ~EXT2_FEATURE_RO_COMPAT_SUPP))){
		printk("EXT2-fs: %s: couldn't mount RDWR because of "
		       "unsupported optional features (%x).\n",
		       sb->s_id, i);
		goto failed_mount;
	}
	blocksize = BLOCK_SIZE << le32_to_cpu(sbi->s_es->s_log_block_size);
	/* If the blocksize doesn't match, re-read the thing.. */
	if (sb->s_blocksize != blocksize) {
		brelse(bh);

		if (!sb_set_blocksize(sb, blocksize)) {
			printk(KERN_ERR "EXT2-fs: blocksize too small for device.\n");
			goto failed_sbi;
		}

		logic_sb_block = (sb_block*BLOCK_SIZE) / blocksize;
		offset = (sb_block*BLOCK_SIZE) % blocksize;
		bh = sb_bread(sb, logic_sb_block);
		if(!bh) {
			printk("EXT2-fs: Couldn't read superblock on "
			       "2nd try.\n");
			goto failed_sbi;
		}
		es = (struct ext2_super_block *) (((char *)bh->b_data) + offset);
		sbi->s_es = es;
		if (es->s_magic != le16_to_cpu(EXT2_SUPER_MAGIC)) {
			printk ("EXT2-fs: Magic mismatch, very weird !\n");
			goto failed_mount;
		}
	}

	sb->s_maxbytes = ext2_max_size(sb->s_blocksize_bits);

	if (le32_to_cpu(es->s_rev_level) == EXT2_GOOD_OLD_REV) {
		sbi->s_inode_size = EXT2_GOOD_OLD_INODE_SIZE;
		sbi->s_first_ino = EXT2_GOOD_OLD_FIRST_INO;
	} else {
		sbi->s_inode_size = le16_to_cpu(es->s_inode_size);
		sbi->s_first_ino = le32_to_cpu(es->s_first_ino);
		if ((sbi->s_inode_size < EXT2_GOOD_OLD_INODE_SIZE) ||
		    (sbi->s_inode_size & (sbi->s_inode_size - 1)) ||
		    (sbi->s_inode_size > blocksize)) {
			printk ("EXT2-fs: unsupported inode size: %d\n",
				sbi->s_inode_size);
			goto failed_mount;
		}
	}
	sbi->s_frag_size = EXT2_MIN_FRAG_SIZE <<
				   le32_to_cpu(es->s_log_frag_size);
	if (sbi->s_frag_size)
		sbi->s_frags_per_block = sb->s_blocksize /
						  sbi->s_frag_size;
	else
		sb->s_magic = 0;
	sbi->s_blocks_per_group = le32_to_cpu(es->s_blocks_per_group);
	sbi->s_frags_per_group = le32_to_cpu(es->s_frags_per_group);
	sbi->s_inodes_per_group = le32_to_cpu(es->s_inodes_per_group);
	sbi->s_inodes_per_block = sb->s_blocksize /
					   EXT2_INODE_SIZE(sb);
	sbi->s_itb_per_group = sbi->s_inodes_per_group /
				        sbi->s_inodes_per_block;
	sbi->s_desc_per_block = sb->s_blocksize /
					 sizeof (struct ext2_group_desc);
	sbi->s_sbh = bh;
	sbi->s_mount_state = le16_to_cpu(es->s_state);
	sbi->s_addr_per_block_bits =
		log2 (EXT2_ADDR_PER_BLOCK(sb));
	sbi->s_desc_per_block_bits =
		log2 (EXT2_DESC_PER_BLOCK(sb));
	if (sb->s_magic != EXT2_SUPER_MAGIC) {
		if (!silent)
			printk ("VFS: Can't find an ext2 filesystem on dev "
				"%s.\n",
				sb->s_id);
		goto failed_mount;
	}
	if (sb->s_blocksize != bh->b_size) {
		if (!silent)
			printk ("VFS: Unsupported blocksize on dev "
				"%s.\n", sb->s_id);
		goto failed_mount;
	}

	if (sb->s_blocksize != sbi->s_frag_size) {
		printk ("EXT2-fs: fragsize %lu != blocksize %lu (not supported yet)\n",
			sbi->s_frag_size, sb->s_blocksize);
		goto failed_mount;
	}

	if (sbi->s_blocks_per_group > sb->s_blocksize * 8) {
		printk ("EXT2-fs: #blocks per group too big: %lu\n",
			sbi->s_blocks_per_group);
		goto failed_mount;
	}
	if (sbi->s_frags_per_group > sb->s_blocksize * 8) {
		printk ("EXT2-fs: #fragments per group too big: %lu\n",
			sbi->s_frags_per_group);
		goto failed_mount;
	}
	if (sbi->s_inodes_per_group > sb->s_blocksize * 8) {
		printk ("EXT2-fs: #inodes per group too big: %lu\n",
			sbi->s_inodes_per_group);
		goto failed_mount;
	}

	sbi->s_groups_count = (le32_to_cpu(es->s_blocks_count) -
				        le32_to_cpu(es->s_first_data_block) +
				       EXT2_BLOCKS_PER_GROUP(sb) - 1) /
				       EXT2_BLOCKS_PER_GROUP(sb);
	db_count = (sbi->s_groups_count + EXT2_DESC_PER_BLOCK(sb) - 1) /
		   EXT2_DESC_PER_BLOCK(sb);
	sbi->s_group_desc = kmalloc (db_count * sizeof (struct buffer_head *), GFP_KERNEL);
	if (sbi->s_group_desc == NULL) {
		printk ("EXT2-fs: not enough memory\n");
		goto failed_mount;
	}
	percpu_counter_init(&sbi->s_freeblocks_counter);
	percpu_counter_init(&sbi->s_freeinodes_counter);
	percpu_counter_init(&sbi->s_dirs_counter);
	bgl_lock_init(&sbi->s_blockgroup_lock);
	sbi->s_debts = kmalloc(sbi->s_groups_count * sizeof(*sbi->s_debts),
			       GFP_KERNEL);
	if (!sbi->s_debts) {
		printk ("EXT2-fs: not enough memory\n");
		goto failed_mount_group_desc;
	}
	memset(sbi->s_debts, 0, sbi->s_groups_count * sizeof(*sbi->s_debts));
	for (i = 0; i < db_count; i++) {
		block = descriptor_loc(sb, logic_sb_block, i);
		sbi->s_group_desc[i] = sb_bread(sb, block);
		if (!sbi->s_group_desc[i]) {
			for (j = 0; j < i; j++)
				brelse (sbi->s_group_desc[j]);
			printk ("EXT2-fs: unable to read group descriptors\n");
			goto failed_mount_group_desc;
		}
	}
	if (!ext2_check_descriptors (sb)) {
		printk ("EXT2-fs: group descriptors corrupted!\n");
		db_count = i;
		goto failed_mount2;
	}
	sbi->s_gdb_count = db_count;
	get_random_bytes(&sbi->s_next_generation, sizeof(u32));
	spin_lock_init(&sbi->s_next_gen_lock);
	/*
	 * set up enough so that it can read an inode
	 */
	sb->s_op = &ext2_sops;
	sb->s_export_op = &ext2_export_ops;
	root = iget(sb, EXT2_ROOT_INO);
	sb->s_root = d_alloc_root(root);
	if (!sb->s_root) {
		iput(root);
		printk(KERN_ERR "EXT2-fs: get root inode failed\n");
		goto failed_mount2;
	}
	if (!S_ISDIR(root->i_mode) || !root->i_blocks || !root->i_size) {
		dput(sb->s_root);
		sb->s_root = NULL;
		printk(KERN_ERR "EXT2-fs: corrupt root inode, run e2fsck\n");
		goto failed_mount2;
	}
	if (EXT2_HAS_COMPAT_FEATURE(sb, EXT3_FEATURE_COMPAT_HAS_JOURNAL))
		ext2_warning(sb, __FUNCTION__,
			"mounting ext3 filesystem as ext2\n");
	ext2_setup_super (sb, es, sb->s_flags & MS_RDONLY);
	percpu_counter_mod(&sbi->s_freeblocks_counter,
				ext2_count_free_blocks(sb));
	percpu_counter_mod(&sbi->s_freeinodes_counter,
				ext2_count_free_inodes(sb));
	percpu_counter_mod(&sbi->s_dirs_counter,
				ext2_count_dirs(sb));
	return 0;
failed_mount2:
	for (i = 0; i < db_count; i++)
		brelse(sbi->s_group_desc[i]);
failed_mount_group_desc:
	kfree(sbi->s_group_desc);
	if (sbi->s_debts)
		kfree(sbi->s_debts);
failed_mount:
	brelse(bh);
failed_sbi:
	sb->s_fs_info = NULL;
	kfree(sbi);
	return -EINVAL;
}

static void ext2_commit_super (struct super_block * sb,
			       struct ext2_super_block * es)
{
	es->s_wtime = cpu_to_le32(get_seconds());
	mark_buffer_dirty(EXT2_SB(sb)->s_sbh);
	sb->s_dirt = 0;
}

static void ext2_sync_super(struct super_block *sb, struct ext2_super_block *es)
{
	es->s_free_blocks_count = cpu_to_le32(ext2_count_free_blocks(sb));
	es->s_free_inodes_count = cpu_to_le32(ext2_count_free_inodes(sb));
	es->s_wtime = cpu_to_le32(get_seconds());
	mark_buffer_dirty(EXT2_SB(sb)->s_sbh);
	sync_dirty_buffer(EXT2_SB(sb)->s_sbh);
	sb->s_dirt = 0;
}

/*
 * In the second extended file system, it is not necessary to
 * write the super block since we use a mapping of the
 * disk super block in a buffer.
 *
 * However, this function is still used to set the fs valid
 * flags to 0.  We need to set this flag to 0 since the fs
 * may have been checked while mounted and e2fsck may have
 * set s_state to EXT2_VALID_FS after some corrections.
 */

void ext2_write_super (struct super_block * sb)
{
	struct ext2_super_block * es;
	lock_kernel();
	if (!(sb->s_flags & MS_RDONLY)) {
		es = EXT2_SB(sb)->s_es;

		if (le16_to_cpu(es->s_state) & EXT2_VALID_FS) {
			ext2_debug ("setting valid to 0\n");
			es->s_state = cpu_to_le16(le16_to_cpu(es->s_state) &
						  ~EXT2_VALID_FS);
			es->s_free_blocks_count = cpu_to_le32(ext2_count_free_blocks(sb));
			es->s_free_inodes_count = cpu_to_le32(ext2_count_free_inodes(sb));
			es->s_mtime = cpu_to_le32(get_seconds());
			ext2_sync_super(sb, es);
		} else
			ext2_commit_super (sb, es);
	}
	sb->s_dirt = 0;
	unlock_kernel();
}

static int ext2_remount (struct super_block * sb, int * flags, char * data)
{
	struct ext2_sb_info * sbi = EXT2_SB(sb);
	struct ext2_super_block * es;

	/*
	 * Allow the "check" option to be passed as a remount option.
	 */
	if (!parse_options (data, sbi))
		return -EINVAL;

	sb->s_flags = (sb->s_flags & ~MS_POSIXACL) |
		((sbi->s_mount_opt & EXT2_MOUNT_POSIX_ACL) ? MS_POSIXACL : 0);

	es = sbi->s_es;
	if ((*flags & MS_RDONLY) == (sb->s_flags & MS_RDONLY))
		return 0;
	if (*flags & MS_RDONLY) {
		if (le16_to_cpu(es->s_state) & EXT2_VALID_FS ||
		    !(sbi->s_mount_state & EXT2_VALID_FS))
			return 0;
		/*
		 * OK, we are remounting a valid rw partition rdonly, so set
		 * the rdonly flag and then mark the partition as valid again.
		 */
		es->s_state = cpu_to_le16(sbi->s_mount_state);
		es->s_mtime = cpu_to_le32(get_seconds());
	} else {
		int ret;
		if ((ret = EXT2_HAS_RO_COMPAT_FEATURE(sb,
					       ~EXT2_FEATURE_RO_COMPAT_SUPP))) {
			printk("EXT2-fs: %s: couldn't remount RDWR because of "
			       "unsupported optional features (%x).\n",
			       sb->s_id, ret);
			return -EROFS;
		}
		/*
		 * Mounting a RDONLY partition read-write, so reread and
		 * store the current valid flag.  (It may have been changed
		 * by e2fsck since we originally mounted the partition.)
		 */
		sbi->s_mount_state = le16_to_cpu(es->s_state);
		if (!ext2_setup_super (sb, es, 0))
			sb->s_flags &= ~MS_RDONLY;
	}
	ext2_sync_super(sb, es);
	return 0;
}

static int ext2_statfs (struct super_block * sb, struct kstatfs * buf)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	unsigned long overhead;
	int i;

	if (test_opt (sb, MINIX_DF))
		overhead = 0;
	else {
		/*
		 * Compute the overhead (FS structures)
		 */

		/*
		 * All of the blocks before first_data_block are
		 * overhead
		 */
		overhead = le32_to_cpu(sbi->s_es->s_first_data_block);

		/*
		 * Add the overhead attributed to the superblock and
		 * block group descriptors.  If the sparse superblocks
		 * feature is turned on, then not all groups have this.
		 */
		for (i = 0; i < sbi->s_groups_count; i++)
			overhead += ext2_bg_has_super(sb, i) +
				ext2_bg_num_gdb(sb, i);

		/*
		 * Every block group has an inode bitmap, a block
		 * bitmap, and an inode table.
		 */
		overhead += (sbi->s_groups_count *
			     (2 + sbi->s_itb_per_group));
	}

	buf->f_type = EXT2_SUPER_MAGIC;
	buf->f_bsize = sb->s_blocksize;
	buf->f_blocks = le32_to_cpu(sbi->s_es->s_blocks_count) - overhead;
	buf->f_bfree = ext2_count_free_blocks(sb);
	buf->f_bavail = buf->f_bfree - le32_to_cpu(sbi->s_es->s_r_blocks_count);
	if (buf->f_bfree < le32_to_cpu(sbi->s_es->s_r_blocks_count))
		buf->f_bavail = 0;
	buf->f_files = le32_to_cpu(sbi->s_es->s_inodes_count);
	buf->f_ffree = ext2_count_free_inodes (sb);
	buf->f_namelen = EXT2_NAME_LEN;
	return 0;
}

static struct super_block *ext2_get_sb(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return get_sb_bdev(fs_type, flags, dev_name, data, ext2_fill_super);
}

static struct file_system_type ext2_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "ext2",
	.get_sb		= ext2_get_sb,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init init_ext2_fs(void)
{
	int err = init_ext2_xattr();
	if (err)
		return err;
	err = init_inodecache();
	if (err)
		goto out1;
        err = register_filesystem(&ext2_fs_type);
	if (err)
		goto out;
	return 0;
out:
	destroy_inodecache();
out1:
	exit_ext2_xattr();
	return err;
}

static void __exit exit_ext2_fs(void)
{
	unregister_filesystem(&ext2_fs_type);
	destroy_inodecache();
	exit_ext2_xattr();
}

module_init(init_ext2_fs)
module_exit(exit_ext2_fs)
