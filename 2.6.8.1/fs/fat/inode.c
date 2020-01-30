/*
 *  linux/fs/fat/inode.c
 *
 *  Written 1992,1993 by Werner Almesberger
 *  VFAT extensions by Gordon Chaffee, merged with msdos fs by Henrik Storner
 *  Rewritten for the constant inumbers support by Al Viro
 *
 *  Fixes:
 *
 *  	Max Cohan: Fixed invalid FSINFO offset when info_sector is 0
 */

#include <linux/module.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/seq_file.h>
#include <linux/msdos_fs.h>
#include <linux/pagemap.h>
#include <linux/buffer_head.h>
#include <linux/mount.h>
#include <linux/vfs.h>
#include <linux/parser.h>
#include <asm/unaligned.h>

#ifndef CONFIG_FAT_DEFAULT_IOCHARSET
/* if user don't select VFAT, this is undefined. */
#define CONFIG_FAT_DEFAULT_IOCHARSET	""
#endif

static int fat_default_codepage = CONFIG_FAT_DEFAULT_CODEPAGE;
static char fat_default_iocharset[] = CONFIG_FAT_DEFAULT_IOCHARSET;

/*
 * New FAT inode stuff. We do the following:
 *	a) i_ino is constant and has nothing with on-disk location.
 *	b) FAT manages its own cache of directory entries.
 *	c) *This* cache is indexed by on-disk location.
 *	d) inode has an associated directory entry, all right, but
 *		it may be unhashed.
 *	e) currently entries are stored within struct inode. That should
 *		change.
 *	f) we deal with races in the following way:
 *		1. readdir() and lookup() do FAT-dir-cache lookup.
 *		2. rename() unhashes the F-d-c entry and rehashes it in
 *			a new place.
 *		3. unlink() and rmdir() unhash F-d-c entry.
 *		4. fat_write_inode() checks whether the thing is unhashed.
 *			If it is we silently return. If it isn't we do bread(),
 *			check if the location is still valid and retry if it
 *			isn't. Otherwise we do changes.
 *		5. Spinlock is used to protect hash/unhash/location check/lookup
 *		6. fat_clear_inode() unhashes the F-d-c entry.
 *		7. lookup() and readdir() do igrab() if they find a F-d-c entry
 *			and consider negative result as cache miss.
 */

#define FAT_HASH_BITS	8
#define FAT_HASH_SIZE	(1UL << FAT_HASH_BITS)
#define FAT_HASH_MASK	(FAT_HASH_SIZE-1)
static struct list_head fat_inode_hashtable[FAT_HASH_SIZE];
spinlock_t fat_inode_lock = SPIN_LOCK_UNLOCKED;

void fat_hash_init(void)
{
	int i;
	for(i = 0; i < FAT_HASH_SIZE; i++) {
		INIT_LIST_HEAD(&fat_inode_hashtable[i]);
	}
}

static inline unsigned long fat_hash(struct super_block *sb, loff_t i_pos)
{
	unsigned long tmp = (unsigned long)i_pos | (unsigned long) sb;
	tmp = tmp + (tmp >> FAT_HASH_BITS) + (tmp >> FAT_HASH_BITS * 2);
	return tmp & FAT_HASH_MASK;
}

void fat_attach(struct inode *inode, loff_t i_pos)
{
	spin_lock(&fat_inode_lock);
	MSDOS_I(inode)->i_pos = i_pos;
	list_add(&MSDOS_I(inode)->i_fat_hash,
		fat_inode_hashtable + fat_hash(inode->i_sb, i_pos));
	spin_unlock(&fat_inode_lock);
}

void fat_detach(struct inode *inode)
{
	spin_lock(&fat_inode_lock);
	MSDOS_I(inode)->i_pos = 0;
	list_del_init(&MSDOS_I(inode)->i_fat_hash);
	spin_unlock(&fat_inode_lock);
}

struct inode *fat_iget(struct super_block *sb, loff_t i_pos)
{
	struct list_head *p = fat_inode_hashtable + fat_hash(sb, i_pos);
	struct list_head *walk;
	struct msdos_inode_info *i;
	struct inode *inode = NULL;

	spin_lock(&fat_inode_lock);
	list_for_each(walk, p) {
		i = list_entry(walk, struct msdos_inode_info, i_fat_hash);
		if (i->vfs_inode.i_sb != sb)
			continue;
		if (i->i_pos != i_pos)
			continue;
		inode = igrab(&i->vfs_inode);
		if (inode)
			break;
	}
	spin_unlock(&fat_inode_lock);
	return inode;
}

static int fat_fill_inode(struct inode *inode, struct msdos_dir_entry *de);

struct inode *fat_build_inode(struct super_block *sb,
			struct msdos_dir_entry *de, loff_t i_pos, int *res)
{
	struct inode *inode;
	*res = 0;
	inode = fat_iget(sb, i_pos);
	if (inode)
		goto out;
	inode = new_inode(sb);
	*res = -ENOMEM;
	if (!inode)
		goto out;
	inode->i_ino = iunique(sb, MSDOS_ROOT_INO);
	inode->i_version = 1;
	*res = fat_fill_inode(inode, de);
	if (*res < 0) {
		iput(inode);
		inode = NULL;
		goto out;
	}
	fat_attach(inode, i_pos);
	insert_inode_hash(inode);
out:
	return inode;
}

void fat_delete_inode(struct inode *inode)
{
	if (!is_bad_inode(inode)) {
		inode->i_size = 0;
		fat_truncate(inode);
	}
	clear_inode(inode);
}

void fat_clear_inode(struct inode *inode)
{
	if (is_bad_inode(inode))
		return;
	lock_kernel();
	spin_lock(&fat_inode_lock);
	fat_cache_inval_inode(inode);
	list_del_init(&MSDOS_I(inode)->i_fat_hash);
	spin_unlock(&fat_inode_lock);
	unlock_kernel();
}

void fat_put_super(struct super_block *sb)
{
	struct msdos_sb_info *sbi = MSDOS_SB(sb);

	if (!(sb->s_flags & MS_RDONLY))
		fat_clusters_flush(sb);

	if (sbi->nls_disk) {
		unload_nls(sbi->nls_disk);
		sbi->nls_disk = NULL;
		sbi->options.codepage = fat_default_codepage;
	}
	if (sbi->nls_io) {
		unload_nls(sbi->nls_io);
		sbi->nls_io = NULL;
	}
	if (sbi->options.iocharset != fat_default_iocharset) {
		kfree(sbi->options.iocharset);
		sbi->options.iocharset = fat_default_iocharset;
	}

	sb->s_fs_info = NULL;
	kfree(sbi);
}

static int fat_show_options(struct seq_file *m, struct vfsmount *mnt)
{
	struct msdos_sb_info *sbi = MSDOS_SB(mnt->mnt_sb);
	struct fat_mount_options *opts = &sbi->options;
	int isvfat = opts->isvfat;

	if (opts->fs_uid != 0)
		seq_printf(m, ",uid=%u", opts->fs_uid);
	if (opts->fs_gid != 0)
		seq_printf(m, ",gid=%u", opts->fs_gid);
	seq_printf(m, ",fmask=%04o", opts->fs_fmask);
	seq_printf(m, ",dmask=%04o", opts->fs_dmask);
	if (sbi->nls_disk && opts->codepage != fat_default_codepage)
		seq_printf(m, ",codepage=%s", sbi->nls_disk->charset);
	if (isvfat) {
		if (sbi->nls_io &&
		    strcmp(opts->iocharset, fat_default_iocharset))
			seq_printf(m, ",iocharset=%s", sbi->nls_io->charset);

		switch (opts->shortname) {
		case VFAT_SFN_DISPLAY_WIN95 | VFAT_SFN_CREATE_WIN95:
			seq_puts(m, ",shortname=win95");
			break;
		case VFAT_SFN_DISPLAY_WINNT | VFAT_SFN_CREATE_WINNT:
			seq_puts(m, ",shortname=winnt");
			break;
		case VFAT_SFN_DISPLAY_WINNT | VFAT_SFN_CREATE_WIN95:
			seq_puts(m, ",shortname=mixed");
			break;
		case VFAT_SFN_DISPLAY_LOWER | VFAT_SFN_CREATE_WIN95:
			/* seq_puts(m, ",shortname=lower"); */
			break;
		default:
			seq_puts(m, ",shortname=unknown");
			break;
		}
	}
	if (opts->name_check != 'n')
		seq_printf(m, ",check=%c", opts->name_check);
	if (opts->quiet)
		seq_puts(m, ",quiet");
	if (opts->showexec)
		seq_puts(m, ",showexec");
	if (opts->sys_immutable)
		seq_puts(m, ",sys_immutable");
	if (!isvfat) {
		if (opts->dotsOK)
			seq_puts(m, ",dotsOK=yes");
		if (opts->nocase)
			seq_puts(m, ",nocase");
	} else {
		if (opts->utf8)
			seq_puts(m, ",utf8");
		if (opts->unicode_xlate)
			seq_puts(m, ",uni_xlate");
		if (!opts->numtail)
			seq_puts(m, ",nonumtail");
	}

	return 0;
}

enum {
	Opt_check_n, Opt_check_r, Opt_check_s, Opt_uid, Opt_gid,
	Opt_umask, Opt_dmask, Opt_fmask, Opt_codepage, Opt_nocase,
	Opt_quiet, Opt_showexec, Opt_debug, Opt_immutable,
	Opt_dots, Opt_nodots,
	Opt_charset, Opt_shortname_lower, Opt_shortname_win95,
	Opt_shortname_winnt, Opt_shortname_mixed, Opt_utf8_no, Opt_utf8_yes,
	Opt_uni_xl_no, Opt_uni_xl_yes, Opt_nonumtail_no, Opt_nonumtail_yes,
	Opt_obsolate, Opt_err,
};

static match_table_t fat_tokens = {
	{Opt_check_r, "check=relaxed"},
	{Opt_check_s, "check=strict"},
	{Opt_check_n, "check=normal"},
	{Opt_check_r, "check=r"},
	{Opt_check_s, "check=s"},
	{Opt_check_n, "check=n"},
	{Opt_uid, "uid=%u"},
	{Opt_gid, "gid=%u"},
	{Opt_umask, "umask=%o"},
	{Opt_dmask, "dmask=%o"},
	{Opt_fmask, "fmask=%o"},
	{Opt_codepage, "codepage=%u"},
	{Opt_nocase, "nocase"},
	{Opt_quiet, "quiet"},
	{Opt_showexec, "showexec"},
	{Opt_debug, "debug"},
	{Opt_immutable, "sys_immutable"},
	{Opt_obsolate, "conv=binary"},
	{Opt_obsolate, "conv=text"},
	{Opt_obsolate, "conv=auto"},
	{Opt_obsolate, "conv=b"},
	{Opt_obsolate, "conv=t"},
	{Opt_obsolate, "conv=a"},
	{Opt_obsolate, "fat=%u"},
	{Opt_obsolate, "blocksize=%u"},
	{Opt_obsolate, "cvf_format=%20s"},
	{Opt_obsolate, "cvf_options=%100s"},
	{Opt_obsolate, "posix"},
	{Opt_err, NULL}
};
static match_table_t msdos_tokens = {
	{Opt_nodots, "nodots"},
	{Opt_nodots, "dotsOK=no"},
	{Opt_dots, "dots"},
	{Opt_dots, "dotsOK=yes"},
	{Opt_err, NULL}
};
static match_table_t vfat_tokens = {
	{Opt_charset, "iocharset=%s"},
	{Opt_shortname_lower, "shortname=lower"},
	{Opt_shortname_win95, "shortname=win95"},
	{Opt_shortname_winnt, "shortname=winnt"},
	{Opt_shortname_mixed, "shortname=mixed"},
	{Opt_utf8_no, "utf8=0"},		/* 0 or no or false */
	{Opt_utf8_no, "utf8=no"},
	{Opt_utf8_no, "utf8=false"},
	{Opt_utf8_yes, "utf8=1"},		/* empty or 1 or yes or true */
	{Opt_utf8_yes, "utf8=yes"},
	{Opt_utf8_yes, "utf8=true"},
	{Opt_utf8_yes, "utf8"},
	{Opt_uni_xl_no, "uni_xlate=0"},		/* 0 or no or false */
	{Opt_uni_xl_no, "uni_xlate=no"},
	{Opt_uni_xl_no, "uni_xlate=false"},
	{Opt_uni_xl_yes, "uni_xlate=1"},	/* empty or 1 or yes or true */
	{Opt_uni_xl_yes, "uni_xlate=yes"},
	{Opt_uni_xl_yes, "uni_xlate=true"},
	{Opt_uni_xl_yes, "uni_xlate"},
	{Opt_nonumtail_no, "nonumtail=0"},	/* 0 or no or false */
	{Opt_nonumtail_no, "nonumtail=no"},
	{Opt_nonumtail_no, "nonumtail=false"},
	{Opt_nonumtail_yes, "nonumtail=1"},	/* empty or 1 or yes or true */
	{Opt_nonumtail_yes, "nonumtail=yes"},
	{Opt_nonumtail_yes, "nonumtail=true"},
	{Opt_nonumtail_yes, "nonumtail"},
	{Opt_err, NULL}
};

static int parse_options(char *options, int is_vfat, int *debug,
			 struct fat_mount_options *opts)
{
	char *p;
	substring_t args[MAX_OPT_ARGS];
	int option;
	char *iocharset;

	opts->isvfat = is_vfat;

	opts->fs_uid = current->uid;
	opts->fs_gid = current->gid;
	opts->fs_fmask = opts->fs_dmask = current->fs->umask;
	opts->codepage = fat_default_codepage;
	opts->iocharset = fat_default_iocharset;
	if (is_vfat)
		opts->shortname = VFAT_SFN_DISPLAY_LOWER|VFAT_SFN_CREATE_WIN95;
	else
		opts->shortname = 0;
	opts->name_check = 'n';
	opts->quiet = opts->showexec = opts->sys_immutable = opts->dotsOK =  0;
	opts->utf8 = opts->unicode_xlate = 0;
	opts->numtail = 1;
	opts->nocase = 0;
	*debug = 0;

	if (!options)
		return 0;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;
		if (!*p)
			continue;

		token = match_token(p, fat_tokens, args);
		if (token == Opt_err) {
			if (is_vfat)
				token = match_token(p, vfat_tokens, args);
			else
				token = match_token(p, msdos_tokens, args);
		}
		switch (token) {
		case Opt_check_s:
			opts->name_check = 's';
			break;
		case Opt_check_r:
			opts->name_check = 'r';
			break;
		case Opt_check_n:
			opts->name_check = 'n';
			break;
		case Opt_nocase:
			if (!is_vfat)
				opts->nocase = 1;
			else {
				/* for backward compatibility */
				opts->shortname = VFAT_SFN_DISPLAY_WIN95
					| VFAT_SFN_CREATE_WIN95;
			}
			break;
		case Opt_quiet:
			opts->quiet = 1;
			break;
		case Opt_showexec:
			opts->showexec = 1;
			break;
		case Opt_debug:
			*debug = 1;
			break;
		case Opt_immutable:
			opts->sys_immutable = 1;
			break;
		case Opt_uid:
			if (match_int(&args[0], &option))
				return 0;
			opts->fs_uid = option;
			break;
		case Opt_gid:
			if (match_int(&args[0], &option))
				return 0;
			opts->fs_gid = option;
			break;
		case Opt_umask:
			if (match_octal(&args[0], &option))
				return 0;
			opts->fs_fmask = opts->fs_dmask = option;
			break;
		case Opt_dmask:
			if (match_octal(&args[0], &option))
				return 0;
			opts->fs_dmask = option;
			break;
		case Opt_fmask:
			if (match_octal(&args[0], &option))
				return 0;
			opts->fs_fmask = option;
			break;
		case Opt_codepage:
			if (match_int(&args[0], &option))
				return 0;
			opts->codepage = option;
			break;

		/* msdos specific */
		case Opt_dots:
			opts->dotsOK = 1;
			break;
		case Opt_nodots:
			opts->dotsOK = 0;
			break;

		/* vfat specific */
		case Opt_charset:
			if (opts->iocharset != fat_default_iocharset)
				kfree(opts->iocharset);
			iocharset = match_strdup(&args[0]);
			if (!iocharset)
				return -ENOMEM;
			opts->iocharset = iocharset;
			break;
		case Opt_shortname_lower:
			opts->shortname = VFAT_SFN_DISPLAY_LOWER
					| VFAT_SFN_CREATE_WIN95;
			break;
		case Opt_shortname_win95:
			opts->shortname = VFAT_SFN_DISPLAY_WIN95
					| VFAT_SFN_CREATE_WIN95;
			break;
		case Opt_shortname_winnt:
			opts->shortname = VFAT_SFN_DISPLAY_WINNT
					| VFAT_SFN_CREATE_WINNT;
			break;
		case Opt_shortname_mixed:
			opts->shortname = VFAT_SFN_DISPLAY_WINNT
					| VFAT_SFN_CREATE_WIN95;
			break;
		case Opt_utf8_no:		/* 0 or no or false */
			opts->utf8 = 0;
			break;
		case Opt_utf8_yes:		/* empty or 1 or yes or true */
			opts->utf8 = 1;
			break;
		case Opt_uni_xl_no:		/* 0 or no or false */
			opts->unicode_xlate = 0;
			break;
		case Opt_uni_xl_yes:		/* empty or 1 or yes or true */
			opts->unicode_xlate = 1;
			break;
		case Opt_nonumtail_no:		/* 0 or no or false */
			opts->numtail = 1;	/* negated option */
			break;
		case Opt_nonumtail_yes:		/* empty or 1 or yes or true */
			opts->numtail = 0;	/* negated option */
			break;

		/* obsolete mount options */
		case Opt_obsolate:
			printk(KERN_INFO "FAT: \"%s\" option is obsolete, "
			       "not supported now\n", p);
			break;
		/* unknown option */
		default:
			printk(KERN_ERR "FAT: Unrecognized mount option \"%s\" "
			       "or missing value\n", p);
			return -EINVAL;
		}
	}
	/* UTF8 doesn't provide FAT semantics */
	if (!strcmp(opts->iocharset, "utf8")) {
		printk(KERN_ERR "FAT: utf8 is not a recommended IO charset"
		       " for FAT filesystems, filesystem will be case sensitive!\n");
	}

	if (opts->unicode_xlate)
		opts->utf8 = 0;
	
	return 0;
}

static int fat_calc_dir_size(struct inode *inode)
{
	struct msdos_sb_info *sbi = MSDOS_SB(inode->i_sb);
	int ret, fclus, dclus;

	inode->i_size = 0;
	if (MSDOS_I(inode)->i_start == 0)
		return 0;

	ret = fat_get_cluster(inode, FAT_ENT_EOF, &fclus, &dclus);
	if (ret < 0)
		return ret;
	inode->i_size = (fclus + 1) << sbi->cluster_bits;

	return 0;
}

static int fat_read_root(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	int error;

	MSDOS_I(inode)->file_cluster = MSDOS_I(inode)->disk_cluster = 0;
	MSDOS_I(inode)->i_pos = 0;
	inode->i_uid = sbi->options.fs_uid;
	inode->i_gid = sbi->options.fs_gid;
	inode->i_version++;
	inode->i_generation = 0;
	inode->i_mode = (S_IRWXUGO & ~sbi->options.fs_dmask) | S_IFDIR;
	inode->i_op = sbi->dir_ops;
	inode->i_fop = &fat_dir_operations;
	if (sbi->fat_bits == 32) {
		MSDOS_I(inode)->i_start = sbi->root_cluster;
		error = fat_calc_dir_size(inode);
		if (error < 0)
			return error;
	} else {
		MSDOS_I(inode)->i_start = 0;
		inode->i_size = sbi->dir_entries * sizeof(struct msdos_dir_entry);
	}
	inode->i_blksize = sbi->cluster_size;
	inode->i_blocks = ((inode->i_size + (sbi->cluster_size - 1))
			   & ~((loff_t)sbi->cluster_size - 1)) >> 9;
	MSDOS_I(inode)->i_logstart = 0;
	MSDOS_I(inode)->mmu_private = inode->i_size;

	MSDOS_I(inode)->i_attrs = 0;
	inode->i_mtime.tv_sec = inode->i_atime.tv_sec = inode->i_ctime.tv_sec = 0;
	inode->i_mtime.tv_nsec = inode->i_atime.tv_nsec = inode->i_ctime.tv_nsec = 0;
	MSDOS_I(inode)->i_ctime_ms = 0;
	inode->i_nlink = fat_subdirs(inode)+2;

	return 0;
}

/*
 * a FAT file handle with fhtype 3 is
 *  0/  i_ino - for fast, reliable lookup if still in the cache
 *  1/  i_generation - to see if i_ino is still valid
 *          bit 0 == 0 iff directory
 *  2/  i_pos(8-39) - if ino has changed, but still in cache
 *  3/  i_pos(4-7)|i_logstart - to semi-verify inode found at i_pos
 *  4/  i_pos(0-3)|parent->i_logstart - maybe used to hunt for the file on disc
 *
 * Hack for NFSv2: Maximum FAT entry number is 28bits and maximum
 * i_pos is 40bits (blocknr(32) + dir offset(8)), so two 4bits
 * of i_logstart is used to store the directory entry offset.
 */

struct dentry *fat_decode_fh(struct super_block *sb, __u32 *fh,
			     int len, int fhtype, 
			     int (*acceptable)(void *context, struct dentry *de),
			     void *context)
{

	if (fhtype != 3)
		return ERR_PTR(-ESTALE);
	if (len < 5)
		return ERR_PTR(-ESTALE);

	return sb->s_export_op->find_exported_dentry(sb, fh, NULL, acceptable, context);
}

struct dentry *fat_get_dentry(struct super_block *sb, void *inump)
{
	struct inode *inode = NULL;
	struct dentry *result;
	__u32 *fh = inump;

	inode = iget(sb, fh[0]);
	if (!inode || is_bad_inode(inode) || inode->i_generation != fh[1]) {
		if (inode)
			iput(inode);
		inode = NULL;
	}
	if (!inode) {
		loff_t i_pos;
		int i_logstart = fh[3] & 0x0fffffff;

		i_pos = (loff_t)fh[2] << 8;
		i_pos |= ((fh[3] >> 24) & 0xf0) | (fh[4] >> 28);

		/* try 2 - see if i_pos is in F-d-c
		 * require i_logstart to be the same
		 * Will fail if you truncate and then re-write
		 */

		inode = fat_iget(sb, i_pos);
		if (inode && MSDOS_I(inode)->i_logstart != i_logstart) {
			iput(inode);
			inode = NULL;
		}
	}
	if (!inode) {
		/* For now, do nothing
		 * What we could do is:
		 * follow the file starting at fh[4], and record
		 * the ".." entry, and the name of the fh[2] entry.
		 * The follow the ".." file finding the next step up.
		 * This way we build a path to the root of
		 * the tree. If this works, we lookup the path and so
		 * get this inode into the cache.
		 * Finally try the fat_iget lookup again
		 * If that fails, then weare totally out of luck
		 * But all that is for another day
		 */
	}
	if (!inode)
		return ERR_PTR(-ESTALE);

	
	/* now to find a dentry.
	 * If possible, get a well-connected one
	 */
	result = d_alloc_anon(inode);
	if (result == NULL) {
		iput(inode);
		return ERR_PTR(-ENOMEM);
	}
	result->d_op = sb->s_root->d_op;
	return result;
}

int fat_encode_fh(struct dentry *de, __u32 *fh, int *lenp, int connectable)
{
	int len = *lenp;
	struct inode *inode =  de->d_inode;
	u32 ipos_h, ipos_m, ipos_l;
	
	if (len < 5)
		return 255; /* no room */

	ipos_h = MSDOS_I(inode)->i_pos >> 8;
	ipos_m = (MSDOS_I(inode)->i_pos & 0xf0) << 24;
	ipos_l = (MSDOS_I(inode)->i_pos & 0x0f) << 28;
	*lenp = 5;
	fh[0] = inode->i_ino;
	fh[1] = inode->i_generation;
	fh[2] = ipos_h;
	fh[3] = ipos_m | MSDOS_I(inode)->i_logstart;
	spin_lock(&de->d_lock);
	fh[4] = ipos_l | MSDOS_I(de->d_parent->d_inode)->i_logstart;
	spin_unlock(&de->d_lock);
	return 3;
}

struct dentry *fat_get_parent(struct dentry *child)
{
	struct buffer_head *bh=NULL;
	struct msdos_dir_entry *de = NULL;
	struct dentry *parent = NULL;
	int res;
	loff_t i_pos = 0;
	struct inode *inode;

	lock_kernel();
	res = fat_scan(child->d_inode, MSDOS_DOTDOT, &bh, &de, &i_pos);

	if (res < 0)
		goto out;
	inode = fat_build_inode(child->d_sb, de, i_pos, &res);
	if (res)
		goto out;
	if (!inode)
		res = -EACCES;
	else {
		parent = d_alloc_anon(inode);
		if (!parent) {
			iput(inode);
			res = -ENOMEM;
		}
	}

 out:
	if(bh)
		brelse(bh);
	unlock_kernel();
	if (res)
		return ERR_PTR(res);
	else
		return parent;
}

static kmem_cache_t *fat_inode_cachep;

static struct inode *fat_alloc_inode(struct super_block *sb)
{
	struct msdos_inode_info *ei;
	ei = (struct msdos_inode_info *)kmem_cache_alloc(fat_inode_cachep, SLAB_KERNEL);
	if (!ei)
		return NULL;
	return &ei->vfs_inode;
}

static void fat_destroy_inode(struct inode *inode)
{
	kmem_cache_free(fat_inode_cachep, MSDOS_I(inode));
}

static void init_once(void * foo, kmem_cache_t * cachep, unsigned long flags)
{
	struct msdos_inode_info *ei = (struct msdos_inode_info *) foo;

	if ((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) ==
	    SLAB_CTOR_CONSTRUCTOR) {
		INIT_LIST_HEAD(&ei->i_fat_hash);
		inode_init_once(&ei->vfs_inode);
	}
}
 
int __init fat_init_inodecache(void)
{
	fat_inode_cachep = kmem_cache_create("fat_inode_cache",
					     sizeof(struct msdos_inode_info),
					     0, SLAB_HWCACHE_ALIGN|SLAB_RECLAIM_ACCOUNT,
					     init_once, NULL);
	if (fat_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

void __exit fat_destroy_inodecache(void)
{
	if (kmem_cache_destroy(fat_inode_cachep))
		printk(KERN_INFO "fat_inode_cache: not all structures were freed\n");
}

static int fat_remount(struct super_block *sb, int *flags, char *data)
{
	*flags |= MS_NODIRATIME;
	return 0;
}

static struct super_operations fat_sops = { 
	.alloc_inode	= fat_alloc_inode,
	.destroy_inode	= fat_destroy_inode,
	.write_inode	= fat_write_inode,
	.delete_inode	= fat_delete_inode,
	.put_super	= fat_put_super,
	.statfs		= fat_statfs,
	.clear_inode	= fat_clear_inode,
	.remount_fs	= fat_remount,

	.read_inode	= make_bad_inode,

	.show_options	= fat_show_options,
};

static struct export_operations fat_export_ops = {
	.decode_fh	= fat_decode_fh,
	.encode_fh	= fat_encode_fh,
	.get_dentry	= fat_get_dentry,
	.get_parent	= fat_get_parent,
};

/*
 * Read the super block of an MS-DOS FS.
 */
int fat_fill_super(struct super_block *sb, void *data, int silent,
		   struct inode_operations *fs_dir_inode_ops, int isvfat)
{
	struct inode *root_inode = NULL;
	struct buffer_head *bh;
	struct fat_boot_sector *b;
	struct msdos_sb_info *sbi;
	u16 logical_sector_size;
	u32 total_sectors, total_clusters, fat_clusters, rootdir_sectors;
	int debug, first;
	unsigned int media;
	long error;
	char buf[50];

	sbi = kmalloc(sizeof(struct msdos_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;
	memset(sbi, 0, sizeof(struct msdos_sb_info));

	sb->s_flags |= MS_NODIRATIME;
	sb->s_magic = MSDOS_SUPER_MAGIC;
	sb->s_op = &fat_sops;
	sb->s_export_op = &fat_export_ops;
	sbi->dir_ops = fs_dir_inode_ops;

	error = parse_options(data, isvfat, &debug, &sbi->options);
	if (error)
		goto out_fail;

	fat_cache_init(sb);
	/* set up enough so that it can read an inode */
	init_MUTEX(&sbi->fat_lock);

	error = -EIO;
	sb_min_blocksize(sb, 512);
	bh = sb_bread(sb, 0);
	if (bh == NULL) {
		printk(KERN_ERR "FAT: unable to read boot sector\n");
		goto out_fail;
	}

	b = (struct fat_boot_sector *) bh->b_data;
	if (!b->reserved) {
		if (!silent)
			printk(KERN_ERR "FAT: bogus number of reserved sectors\n");
		brelse(bh);
		goto out_invalid;
	}
	if (!b->fats) {
		if (!silent)
			printk(KERN_ERR "FAT: bogus number of FAT structure\n");
		brelse(bh);
		goto out_invalid;
	}

	/*
	 * Earlier we checked here that b->secs_track and b->head are nonzero,
	 * but it turns out valid FAT filesystems can have zero there.
	 */

	media = b->media;
	if (!FAT_VALID_MEDIA(media)) {
		if (!silent)
			printk(KERN_ERR "FAT: invalid media value (0x%02x)\n",
			       media);
		brelse(bh);
		goto out_invalid;
	}
	logical_sector_size = CF_LE_W(get_unaligned((u16 *)&b->sector_size));
	if (!logical_sector_size
	    || (logical_sector_size & (logical_sector_size - 1))
	    || (logical_sector_size < 512)
	    || (PAGE_CACHE_SIZE < logical_sector_size)) {
		if (!silent)
			printk(KERN_ERR "FAT: bogus logical sector size %u\n",
			       logical_sector_size);
		brelse(bh);
		goto out_invalid;
	}
	sbi->sec_per_clus = b->sec_per_clus;
	if (!sbi->sec_per_clus
	    || (sbi->sec_per_clus & (sbi->sec_per_clus - 1))) {
		if (!silent)
			printk(KERN_ERR "FAT: bogus sectors per cluster %u\n",
			       sbi->sec_per_clus);
		brelse(bh);
		goto out_invalid;
	}

	if (logical_sector_size < sb->s_blocksize) {
		printk(KERN_ERR "FAT: logical sector size too small for device"
		       " (logical sector size = %u)\n", logical_sector_size);
		brelse(bh);
		goto out_fail;
	}
	if (logical_sector_size > sb->s_blocksize) {
		brelse(bh);

		if (!sb_set_blocksize(sb, logical_sector_size)) {
			printk(KERN_ERR "FAT: unable to set blocksize %u\n",
			       logical_sector_size);
			goto out_fail;
		}
		bh = sb_bread(sb, 0);
		if (bh == NULL) {
			printk(KERN_ERR "FAT: unable to read boot sector"
			       " (logical sector size = %lu)\n",
			       sb->s_blocksize);
			goto out_fail;
		}
		b = (struct fat_boot_sector *) bh->b_data;
	}

	sbi->cluster_size = sb->s_blocksize * sbi->sec_per_clus;
	sbi->cluster_bits = ffs(sbi->cluster_size) - 1;
	sbi->fats = b->fats;
	sbi->fat_bits = 0;		/* Don't know yet */
	sbi->fat_start = CF_LE_W(b->reserved);
	sbi->fat_length = CF_LE_W(b->fat_length);
	sbi->root_cluster = 0;
	sbi->free_clusters = -1;	/* Don't know yet */
	sbi->prev_free = -1;

	if (!sbi->fat_length && b->fat32_length) {
		struct fat_boot_fsinfo *fsinfo;
		struct buffer_head *fsinfo_bh;

		/* Must be FAT32 */
		sbi->fat_bits = 32;
		sbi->fat_length = CF_LE_L(b->fat32_length);
		sbi->root_cluster = CF_LE_L(b->root_cluster);

		sb->s_maxbytes = 0xffffffff;
		
		/* MC - if info_sector is 0, don't multiply by 0 */
		sbi->fsinfo_sector = CF_LE_W(b->info_sector);
		if (sbi->fsinfo_sector == 0)
			sbi->fsinfo_sector = 1;

		fsinfo_bh = sb_bread(sb, sbi->fsinfo_sector);
		if (fsinfo_bh == NULL) {
			printk(KERN_ERR "FAT: bread failed, FSINFO block"
			       " (sector = %lu)\n", sbi->fsinfo_sector);
			brelse(bh);
			goto out_fail;
		}

		fsinfo = (struct fat_boot_fsinfo *)fsinfo_bh->b_data;
		if (!IS_FSINFO(fsinfo)) {
			printk(KERN_WARNING
			       "FAT: Did not find valid FSINFO signature.\n"
			       "     Found signature1 0x%08x signature2 0x%08x"
			       " (sector = %lu)\n",
			       CF_LE_L(fsinfo->signature1),
			       CF_LE_L(fsinfo->signature2),
			       sbi->fsinfo_sector);
		} else {
			sbi->free_clusters = CF_LE_L(fsinfo->free_clusters);
			sbi->prev_free = CF_LE_L(fsinfo->next_cluster);
		}

		brelse(fsinfo_bh);
	}

	sbi->dir_per_block = sb->s_blocksize / sizeof(struct msdos_dir_entry);
	sbi->dir_per_block_bits = ffs(sbi->dir_per_block) - 1;

	sbi->dir_start = sbi->fat_start + sbi->fats * sbi->fat_length;
	sbi->dir_entries = CF_LE_W(get_unaligned((u16 *)&b->dir_entries));
	if (sbi->dir_entries & (sbi->dir_per_block - 1)) {
		if (!silent)
			printk(KERN_ERR "FAT: bogus directroy-entries per block"
			       " (%u)\n", sbi->dir_entries);
		brelse(bh);
		goto out_invalid;
	}

	rootdir_sectors = sbi->dir_entries
		* sizeof(struct msdos_dir_entry) / sb->s_blocksize;
	sbi->data_start = sbi->dir_start + rootdir_sectors;
	total_sectors = CF_LE_W(get_unaligned((u16 *)&b->sectors));
	if (total_sectors == 0)
		total_sectors = CF_LE_L(b->total_sect);

	total_clusters = (total_sectors - sbi->data_start) / sbi->sec_per_clus;

	if (sbi->fat_bits != 32)
		sbi->fat_bits = (total_clusters > MAX_FAT12) ? 16 : 12;

	/* check that FAT table does not overflow */
	fat_clusters = sbi->fat_length * sb->s_blocksize * 8 / sbi->fat_bits;
	total_clusters = min(total_clusters, fat_clusters - 2);
	if (total_clusters > MAX_FAT(sb)) {
		if (!silent)
			printk(KERN_ERR "FAT: count of clusters too big (%u)\n",
			       total_clusters);
		brelse(bh);
		goto out_invalid;
	}

	sbi->clusters = total_clusters;

	brelse(bh);

	/* validity check of FAT */
	first = __fat_access(sb, 0, -1);
	if (first < 0) {
		error = first;
		goto out_fail;
	}
	if (FAT_FIRST_ENT(sb, media) == first) {
		/* all is as it should be */
	} else if (media == 0xf8 && FAT_FIRST_ENT(sb, 0xfe) == first) {
		/* bad, reported on pc9800 */
	} else if (media == 0xf0 && FAT_FIRST_ENT(sb, 0xf8) == first) {
		/* bad, reported with a MO disk on win95/me */
	} else if (first == 0) {
		/* bad, reported with a SmartMedia card */
	} else {
		if (!silent)
			printk(KERN_ERR "FAT: invalid first entry of FAT "
			       "(0x%x != 0x%x)\n",
			       FAT_FIRST_ENT(sb, media), first);
		goto out_invalid;
	}

	error = -EINVAL;
	sprintf(buf, "cp%d", sbi->options.codepage);
	sbi->nls_disk = load_nls(buf);
	if (!sbi->nls_disk) {
		printk(KERN_ERR "FAT: codepage %s not found\n", buf);
		goto out_fail;
	}

	/* FIXME: utf8 is using iocharset for upper/lower conversion */
	if (sbi->options.isvfat) {
		sbi->nls_io = load_nls(sbi->options.iocharset);
		if (!sbi->nls_io) {
			printk(KERN_ERR "FAT: IO charset %s not found\n",
			       sbi->options.iocharset);
			goto out_fail;
		}
	}

	error = -ENOMEM;
	root_inode = new_inode(sb);
	if (!root_inode)
		goto out_fail;
	root_inode->i_ino = MSDOS_ROOT_INO;
	root_inode->i_version = 1;
	error = fat_read_root(root_inode);
	if (error < 0)
		goto out_fail;
	error = -ENOMEM;
	insert_inode_hash(root_inode);
	sb->s_root = d_alloc_root(root_inode);
	if (!sb->s_root) {
		printk(KERN_ERR "FAT: get root inode failed\n");
		goto out_fail;
	}

	return 0;

out_invalid:
	error = -EINVAL;
	if (!silent)
		printk(KERN_INFO "VFS: Can't find a valid FAT filesystem"
		       " on dev %s.\n", sb->s_id);

out_fail:
	if (root_inode)
		iput(root_inode);
	if (sbi->nls_io)
		unload_nls(sbi->nls_io);
	if (sbi->nls_disk)
		unload_nls(sbi->nls_disk);
	if (sbi->options.iocharset != fat_default_iocharset)
		kfree(sbi->options.iocharset);
	sb->s_fs_info = NULL;
	kfree(sbi);
	return error;
}

int fat_statfs(struct super_block *sb, struct kstatfs *buf)
{
	int free, nr, ret;
       
	if (MSDOS_SB(sb)->free_clusters != -1)
		free = MSDOS_SB(sb)->free_clusters;
	else {
		lock_fat(sb);
		if (MSDOS_SB(sb)->free_clusters != -1)
			free = MSDOS_SB(sb)->free_clusters;
		else {
			free = 0;
			for (nr = 2; nr < MSDOS_SB(sb)->clusters + 2; nr++) {
				ret = fat_access(sb, nr, -1);
				if (ret < 0) {
					unlock_fat(sb);
					return ret;
				} else if (ret == FAT_ENT_FREE)
					free++;
			}
			MSDOS_SB(sb)->free_clusters = free;
		}
		unlock_fat(sb);
	}

	buf->f_type = sb->s_magic;
	buf->f_bsize = MSDOS_SB(sb)->cluster_size;
	buf->f_blocks = MSDOS_SB(sb)->clusters;
	buf->f_bfree = free;
	buf->f_bavail = free;
	buf->f_namelen = MSDOS_SB(sb)->options.isvfat ? 260 : 12;

	return 0;
}

static int is_exec(unsigned char *extension)
{
	unsigned char *exe_extensions = "EXECOMBAT", *walk;

	for (walk = exe_extensions; *walk; walk += 3)
		if (!strncmp(extension, walk, 3))
			return 1;
	return 0;
}

static int fat_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page,fat_get_block, wbc);
}
static int fat_readpage(struct file *file, struct page *page)
{
	return block_read_full_page(page,fat_get_block);
}

static int
fat_prepare_write(struct file *file, struct page *page,
			unsigned from, unsigned to)
{
	kmap(page);
	return cont_prepare_write(page,from,to,fat_get_block,
		&MSDOS_I(page->mapping->host)->mmu_private);
}

static int
fat_commit_write(struct file *file, struct page *page,
			unsigned from, unsigned to)
{
	kunmap(page);
	return generic_commit_write(file, page, from, to);
}

static sector_t _fat_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping,block,fat_get_block);
}
static struct address_space_operations fat_aops = {
	.readpage = fat_readpage,
	.writepage = fat_writepage,
	.sync_page = block_sync_page,
	.prepare_write = fat_prepare_write,
	.commit_write = fat_commit_write,
	.bmap = _fat_bmap
};

/* doesn't deal with root inode */
static int fat_fill_inode(struct inode *inode, struct msdos_dir_entry *de)
{
	struct super_block *sb = inode->i_sb;
	struct msdos_sb_info *sbi = MSDOS_SB(sb);
	int error;

	MSDOS_I(inode)->file_cluster = MSDOS_I(inode)->disk_cluster = 0;
	MSDOS_I(inode)->i_pos = 0;
	inode->i_uid = sbi->options.fs_uid;
	inode->i_gid = sbi->options.fs_gid;
	inode->i_version++;
	inode->i_generation = get_seconds();
	
	if ((de->attr & ATTR_DIR) && !IS_FREE(de->name)) {
		inode->i_generation &= ~1;
		inode->i_mode = MSDOS_MKMODE(de->attr,
			S_IRWXUGO & ~sbi->options.fs_dmask) | S_IFDIR;
		inode->i_op = sbi->dir_ops;
		inode->i_fop = &fat_dir_operations;

		MSDOS_I(inode)->i_start = CF_LE_W(de->start);
		if (sbi->fat_bits == 32)
			MSDOS_I(inode)->i_start |= (CF_LE_W(de->starthi) << 16);

		MSDOS_I(inode)->i_logstart = MSDOS_I(inode)->i_start;
		error = fat_calc_dir_size(inode);
		if (error < 0)
			return error;
		MSDOS_I(inode)->mmu_private = inode->i_size;

		inode->i_nlink = fat_subdirs(inode);
	} else { /* not a directory */
		inode->i_generation |= 1;
		inode->i_mode = MSDOS_MKMODE(de->attr,
		    ((sbi->options.showexec &&
		       !is_exec(de->ext))
		    	? S_IRUGO|S_IWUGO : S_IRWXUGO)
		    & ~sbi->options.fs_fmask) | S_IFREG;
		MSDOS_I(inode)->i_start = CF_LE_W(de->start);
		if (sbi->fat_bits == 32)
			MSDOS_I(inode)->i_start |= (CF_LE_W(de->starthi) << 16);

		MSDOS_I(inode)->i_logstart = MSDOS_I(inode)->i_start;
		inode->i_size = CF_LE_L(de->size);
	        inode->i_op = &fat_file_inode_operations;
	        inode->i_fop = &fat_file_operations;
		inode->i_mapping->a_ops = &fat_aops;
		MSDOS_I(inode)->mmu_private = inode->i_size;
	}
	if(de->attr & ATTR_SYS)
		if (sbi->options.sys_immutable)
			inode->i_flags |= S_IMMUTABLE;
	MSDOS_I(inode)->i_attrs = de->attr & ATTR_UNUSED;
	/* this is as close to the truth as we can get ... */
	inode->i_blksize = sbi->cluster_size;
	inode->i_blocks = ((inode->i_size + (sbi->cluster_size - 1))
			   & ~((loff_t)sbi->cluster_size - 1)) >> 9;
	inode->i_mtime.tv_sec = inode->i_atime.tv_sec =
		date_dos2unix(CF_LE_W(de->time),CF_LE_W(de->date));
	inode->i_mtime.tv_nsec = inode->i_atime.tv_nsec = 0;
	inode->i_ctime.tv_sec =
		MSDOS_SB(sb)->options.isvfat
		? date_dos2unix(CF_LE_W(de->ctime),CF_LE_W(de->cdate))
		: inode->i_mtime.tv_sec;
	inode->i_ctime.tv_nsec = de->ctime_ms * 1000000;
	MSDOS_I(inode)->i_ctime_ms = de->ctime_ms;

	return 0;
}

void fat_write_inode(struct inode *inode, int wait)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	struct msdos_dir_entry *raw_entry;
	loff_t i_pos;

retry:
	i_pos = MSDOS_I(inode)->i_pos;
	if (inode->i_ino == MSDOS_ROOT_INO || !i_pos) {
		return;
	}
	lock_kernel();
	if (!(bh = sb_bread(sb, i_pos >> MSDOS_SB(sb)->dir_per_block_bits))) {
		printk(KERN_ERR "FAT: unable to read inode block "
		       "for updating (i_pos %lld)\n", i_pos);
		unlock_kernel();
		return /* -EIO */;
	}
	spin_lock(&fat_inode_lock);
	if (i_pos != MSDOS_I(inode)->i_pos) {
		spin_unlock(&fat_inode_lock);
		brelse(bh);
		unlock_kernel();
		goto retry;
	}

	raw_entry = &((struct msdos_dir_entry *) (bh->b_data))
	    [i_pos & (MSDOS_SB(sb)->dir_per_block - 1)];
	if (S_ISDIR(inode->i_mode)) {
		raw_entry->attr = ATTR_DIR;
		raw_entry->size = 0;
	}
	else {
		raw_entry->attr = ATTR_NONE;
		raw_entry->size = CT_LE_L(inode->i_size);
	}
	raw_entry->attr |= MSDOS_MKATTR(inode->i_mode) |
	    MSDOS_I(inode)->i_attrs;
	raw_entry->start = CT_LE_W(MSDOS_I(inode)->i_logstart);
	raw_entry->starthi = CT_LE_W(MSDOS_I(inode)->i_logstart >> 16);
	fat_date_unix2dos(inode->i_mtime.tv_sec,&raw_entry->time,&raw_entry->date);
	raw_entry->time = CT_LE_W(raw_entry->time);
	raw_entry->date = CT_LE_W(raw_entry->date);
	if (MSDOS_SB(sb)->options.isvfat) {
		fat_date_unix2dos(inode->i_ctime.tv_sec,&raw_entry->ctime,&raw_entry->cdate);
		raw_entry->ctime_ms = MSDOS_I(inode)->i_ctime_ms; /* use i_ctime.tv_nsec? */
		raw_entry->ctime = CT_LE_W(raw_entry->ctime);
		raw_entry->cdate = CT_LE_W(raw_entry->cdate);
	}
	spin_unlock(&fat_inode_lock);
	mark_buffer_dirty(bh);
	brelse(bh);
	unlock_kernel();
}


int fat_notify_change(struct dentry * dentry, struct iattr * attr)
{
	struct msdos_sb_info *sbi = MSDOS_SB(dentry->d_sb);
	struct inode *inode = dentry->d_inode;
	int mask, error = 0;

	lock_kernel();

	/* FAT cannot truncate to a longer file */
	if (attr->ia_valid & ATTR_SIZE) {
		if (attr->ia_size > inode->i_size) {
			error = -EPERM;
			goto out;
		}
	}

	error = inode_change_ok(inode, attr);
	if (error) {
		if (sbi->options.quiet)
			error = 0;
 		goto out;
	}

	if (((attr->ia_valid & ATTR_UID) && 
	     (attr->ia_uid != sbi->options.fs_uid)) ||
	    ((attr->ia_valid & ATTR_GID) && 
	     (attr->ia_gid != sbi->options.fs_gid)) ||
	    ((attr->ia_valid & ATTR_MODE) &&
	     (attr->ia_mode & ~MSDOS_VALID_MODE)))
		error = -EPERM;

	if (error) {
		if (sbi->options.quiet)  
			error = 0;
		goto out;
	}
	error = inode_setattr(inode, attr);
	if (error)
		goto out;

	if (S_ISDIR(inode->i_mode))
		mask = sbi->options.fs_dmask;
	else
		mask = sbi->options.fs_fmask;
	inode->i_mode &= S_IFMT | (S_IRWXUGO & ~mask);
out:
	unlock_kernel();
	return error;
}
MODULE_LICENSE("GPL");
