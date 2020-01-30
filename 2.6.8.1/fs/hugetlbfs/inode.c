/*
 * hugetlbpage-backed filesystem.  Based on ramfs.
 *
 * William Irwin, 2002
 *
 * Copyright (C) 2002 Linus Torvalds.
 */

#include <linux/module.h>
#include <linux/thread_info.h>
#include <asm/current.h>
#include <linux/sched.h>		/* remove ASAP */
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/file.h>
#include <linux/writeback.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/hugetlb.h>
#include <linux/pagevec.h>
#include <linux/quotaops.h>
#include <linux/slab.h>
#include <linux/dnotify.h>
#include <linux/statfs.h>
#include <linux/security.h>

#include <asm/uaccess.h>

/* some random number */
#define HUGETLBFS_MAGIC	0x958458f6

static struct super_operations hugetlbfs_ops;
static struct address_space_operations hugetlbfs_aops;
struct file_operations hugetlbfs_file_operations;
static struct inode_operations hugetlbfs_dir_inode_operations;
static struct inode_operations hugetlbfs_inode_operations;

static struct backing_dev_info hugetlbfs_backing_dev_info = {
	.ra_pages	= 0,	/* No readahead */
	.memory_backed	= 1,	/* Does not contribute to dirty memory */
};

int sysctl_hugetlb_shm_group;

static int hugetlbfs_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file->f_dentry->d_inode;
	struct address_space *mapping = inode->i_mapping;
	loff_t len, vma_len;
	int ret;

	if (vma->vm_pgoff & (HPAGE_SIZE / PAGE_SIZE - 1))
		return -EINVAL;

	if (vma->vm_start & ~HPAGE_MASK)
		return -EINVAL;

	if (vma->vm_end & ~HPAGE_MASK)
		return -EINVAL;

	if (vma->vm_end - vma->vm_start < HPAGE_SIZE)
		return -EINVAL;

	vma_len = (loff_t)(vma->vm_end - vma->vm_start);

	down(&inode->i_sem);
	file_accessed(file);
	vma->vm_flags |= VM_HUGETLB | VM_RESERVED;
	vma->vm_ops = &hugetlb_vm_ops;
	ret = hugetlb_prefault(mapping, vma);
	len = vma_len +	((loff_t)vma->vm_pgoff << PAGE_SHIFT);
	if (ret == 0 && inode->i_size < len)
		inode->i_size = len;
	up(&inode->i_sem);

	return ret;
}

/*
 * Called under down_write(mmap_sem), page_table_lock is not held
 */

#ifdef HAVE_ARCH_HUGETLB_UNMAPPED_AREA
unsigned long hugetlb_get_unmapped_area(struct file *file, unsigned long addr,
		unsigned long len, unsigned long pgoff, unsigned long flags);
#else
static unsigned long
hugetlb_get_unmapped_area(struct file *file, unsigned long addr,
		unsigned long len, unsigned long pgoff, unsigned long flags)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;

	if (len & ~HPAGE_MASK)
		return -EINVAL;
	if (len > TASK_SIZE)
		return -ENOMEM;

	if (addr) {
		addr = ALIGN(addr, HPAGE_SIZE);
		vma = find_vma(mm, addr);
		if (TASK_SIZE - len >= addr &&
		    (!vma || addr + len <= vma->vm_start))
			return addr;
	}

	addr = ALIGN(mm->free_area_cache, HPAGE_SIZE);

	for (vma = find_vma(mm, addr); ; vma = vma->vm_next) {
		/* At this point:  (!vma || addr < vma->vm_end). */
		if (TASK_SIZE - len < addr)
			return -ENOMEM;
		if (!vma || addr + len <= vma->vm_start)
			return addr;
		addr = ALIGN(vma->vm_end, HPAGE_SIZE);
	}
}
#endif

/*
 * Read a page. Again trivial. If it didn't already exist
 * in the page cache, it is zero-filled.
 */
static int hugetlbfs_readpage(struct file *file, struct page * page)
{
	unlock_page(page);
	return -EINVAL;
}

static int hugetlbfs_prepare_write(struct file *file,
			struct page *page, unsigned offset, unsigned to)
{
	return -EINVAL;
}

static int hugetlbfs_commit_write(struct file *file,
			struct page *page, unsigned offset, unsigned to)
{
	return -EINVAL;
}

void huge_pagevec_release(struct pagevec *pvec)
{
	int i;

	for (i = 0; i < pagevec_count(pvec); ++i)
		put_page(pvec->pages[i]);

	pagevec_reinit(pvec);
}

void truncate_huge_page(struct page *page)
{
	clear_page_dirty(page);
	ClearPageUptodate(page);
	remove_from_page_cache(page);
	put_page(page);
}

void truncate_hugepages(struct address_space *mapping, loff_t lstart)
{
	const pgoff_t start = lstart >> HPAGE_SHIFT;
	struct pagevec pvec;
	pgoff_t next;
	int i;

	pagevec_init(&pvec, 0);
	next = start;
	while (1) {
		if (!pagevec_lookup(&pvec, mapping, next, PAGEVEC_SIZE)) {
			if (next == start)
				break;
			next = start;
			continue;
		}

		for (i = 0; i < pagevec_count(&pvec); ++i) {
			struct page *page = pvec.pages[i];

			lock_page(page);
			if (page->index > next)
				next = page->index;
			++next;
			truncate_huge_page(page);
			unlock_page(page);
			hugetlb_put_quota(mapping);
		}
		huge_pagevec_release(&pvec);
	}
	BUG_ON(!lstart && mapping->nrpages);
}

static void hugetlbfs_delete_inode(struct inode *inode)
{
	struct hugetlbfs_sb_info *sbinfo = HUGETLBFS_SB(inode->i_sb);

	hlist_del_init(&inode->i_hash);
	list_del_init(&inode->i_list);
	inode->i_state |= I_FREEING;
	inodes_stat.nr_inodes--;
	spin_unlock(&inode_lock);

	if (inode->i_data.nrpages)
		truncate_hugepages(&inode->i_data, 0);

	security_inode_delete(inode);

	if (sbinfo->free_inodes >= 0) {
		spin_lock(&sbinfo->stat_lock);
		sbinfo->free_inodes++;
		spin_unlock(&sbinfo->stat_lock);
	}

	clear_inode(inode);
	destroy_inode(inode);
}

static void hugetlbfs_forget_inode(struct inode *inode)
{
	struct super_block *super_block = inode->i_sb;
	struct hugetlbfs_sb_info *sbinfo = HUGETLBFS_SB(super_block);

	if (hlist_unhashed(&inode->i_hash))
		goto out_truncate;

	if (!(inode->i_state & (I_DIRTY|I_LOCK))) {
		list_del(&inode->i_list);
		list_add(&inode->i_list, &inode_unused);
	}
	inodes_stat.nr_unused++;
	if (!super_block || (super_block->s_flags & MS_ACTIVE)) {
		spin_unlock(&inode_lock);
		return;
	}

	/* write_inode_now() ? */
	inodes_stat.nr_unused--;
	hlist_del_init(&inode->i_hash);
out_truncate:
	list_del_init(&inode->i_list);
	inode->i_state |= I_FREEING;
	inodes_stat.nr_inodes--;
	spin_unlock(&inode_lock);
	if (inode->i_data.nrpages)
		truncate_hugepages(&inode->i_data, 0);

	if (sbinfo->free_inodes >= 0) {
		spin_lock(&sbinfo->stat_lock);
		sbinfo->free_inodes++;
		spin_unlock(&sbinfo->stat_lock);
	}

	clear_inode(inode);
	destroy_inode(inode);
}

static void hugetlbfs_drop_inode(struct inode *inode)
{
	if (!inode->i_nlink)
		hugetlbfs_delete_inode(inode);
	else
		hugetlbfs_forget_inode(inode);
}

/*
 * h_pgoff is in HPAGE_SIZE units.
 * vma->vm_pgoff is in PAGE_SIZE units.
 */
static inline void
hugetlb_vmtruncate_list(struct prio_tree_root *root, unsigned long h_pgoff)
{
	struct vm_area_struct *vma = NULL;
	struct prio_tree_iter iter;

	while ((vma = vma_prio_tree_next(vma, root, &iter,
					h_pgoff, ULONG_MAX)) != NULL) {
		unsigned long h_vm_pgoff;
		unsigned long v_length;
		unsigned long v_offset;

		h_vm_pgoff = vma->vm_pgoff >> (HPAGE_SHIFT - PAGE_SHIFT);
		v_offset = (h_pgoff - h_vm_pgoff) << HPAGE_SHIFT;
		/*
		 * Is this VMA fully outside the truncation point?
		 */
		if (h_vm_pgoff >= h_pgoff)
			v_offset = 0;

		v_length = vma->vm_end - vma->vm_start;

		zap_hugepage_range(vma,
				vma->vm_start + v_offset,
				v_length - v_offset);
	}
}

/*
 * Expanding truncates are not allowed.
 */
static int hugetlb_vmtruncate(struct inode *inode, loff_t offset)
{
	unsigned long pgoff;
	struct address_space *mapping = inode->i_mapping;

	if (offset > inode->i_size)
		return -EINVAL;

	BUG_ON(offset & ~HPAGE_MASK);
	pgoff = offset >> HPAGE_SHIFT;

	inode->i_size = offset;
	spin_lock(&mapping->i_mmap_lock);
	if (!prio_tree_empty(&mapping->i_mmap))
		hugetlb_vmtruncate_list(&mapping->i_mmap, pgoff);
	spin_unlock(&mapping->i_mmap_lock);
	truncate_hugepages(mapping, offset);
	return 0;
}

static int hugetlbfs_setattr(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	int error;
	unsigned int ia_valid = attr->ia_valid;

	BUG_ON(!inode);

	error = inode_change_ok(inode, attr);
	if (error)
		goto out;

	if (ia_valid & ATTR_SIZE) {
		error = -EINVAL;
		if (!(attr->ia_size & ~HPAGE_MASK))
			error = hugetlb_vmtruncate(inode, attr->ia_size);
		if (error)
			goto out;
		attr->ia_valid &= ~ATTR_SIZE;
	}
	error = inode_setattr(inode, attr);
out:
	return error;
}

static struct inode *hugetlbfs_get_inode(struct super_block *sb, uid_t uid, 
					gid_t gid, int mode, dev_t dev)
{
	struct inode *inode;
	struct hugetlbfs_sb_info *sbinfo = HUGETLBFS_SB(sb);

	if (sbinfo->free_inodes >= 0) {
		spin_lock(&sbinfo->stat_lock);
		if (!sbinfo->free_inodes) {
			spin_unlock(&sbinfo->stat_lock);
			return NULL;
		}
		sbinfo->free_inodes--;
		spin_unlock(&sbinfo->stat_lock);
	}

	inode = new_inode(sb);
	if (inode) {
		struct hugetlbfs_inode_info *info;
		inode->i_mode = mode;
		inode->i_uid = uid;
		inode->i_gid = gid;
		inode->i_blksize = HPAGE_SIZE;
		inode->i_blocks = 0;
		inode->i_mapping->a_ops = &hugetlbfs_aops;
		inode->i_mapping->backing_dev_info =&hugetlbfs_backing_dev_info;
		inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
		info = HUGETLBFS_I(inode);
		mpol_shared_policy_init(&info->policy);
		switch (mode & S_IFMT) {
		default:
			init_special_inode(inode, mode, dev);
			break;
		case S_IFREG:
			inode->i_op = &hugetlbfs_inode_operations;
			inode->i_fop = &hugetlbfs_file_operations;
			break;
		case S_IFDIR:
			inode->i_op = &hugetlbfs_dir_inode_operations;
			inode->i_fop = &simple_dir_operations;

			/* directory inodes start off with i_nlink == 2 (for "." entry) */
			inode->i_nlink++;
			break;
		case S_IFLNK:
			inode->i_op = &page_symlink_inode_operations;
			break;
		}
	}
	return inode;
}

/*
 * File creation. Allocate an inode, and we're done..
 */
static int hugetlbfs_mknod(struct inode *dir,
			struct dentry *dentry, int mode, dev_t dev)
{
	struct inode *inode;
	int error = -ENOSPC;
	gid_t gid;

	if (dir->i_mode & S_ISGID) {
		gid = dir->i_gid;
		if (S_ISDIR(mode))
			mode |= S_ISGID;
	} else {
		gid = current->fsgid;
	}
	inode = hugetlbfs_get_inode(dir->i_sb, current->fsuid, gid, mode, dev);
	if (inode) {
		dir->i_ctime = dir->i_mtime = CURRENT_TIME;
		d_instantiate(dentry, inode);
		dget(dentry);	/* Extra count - pin the dentry in core */
		error = 0;
	}
	return error;
}

static int hugetlbfs_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	int retval = hugetlbfs_mknod(dir, dentry, mode | S_IFDIR, 0);
	if (!retval)
		dir->i_nlink++;
	return retval;
}

static int hugetlbfs_create(struct inode *dir, struct dentry *dentry, int mode, struct nameidata *nd)
{
	return hugetlbfs_mknod(dir, dentry, mode | S_IFREG, 0);
}

static int hugetlbfs_symlink(struct inode *dir,
			struct dentry *dentry, const char *symname)
{
	struct inode *inode;
	int error = -ENOSPC;
	gid_t gid;

	if (dir->i_mode & S_ISGID)
		gid = dir->i_gid;
	else
		gid = current->fsgid;

	inode = hugetlbfs_get_inode(dir->i_sb, current->fsuid,
					gid, S_IFLNK|S_IRWXUGO, 0);
	if (inode) {
		int l = strlen(symname)+1;
		error = page_symlink(inode, symname, l);
		if (!error) {
			d_instantiate(dentry, inode);
			dget(dentry);
		} else
			iput(inode);
	}
	dir->i_ctime = dir->i_mtime = CURRENT_TIME;

	return error;
}

/*
 * For direct-IO reads into hugetlb pages
 */
int hugetlbfs_set_page_dirty(struct page *page)
{
	return 0;
}

static int hugetlbfs_statfs(struct super_block *sb, struct kstatfs *buf)
{
	struct hugetlbfs_sb_info *sbinfo = HUGETLBFS_SB(sb);

	buf->f_type = HUGETLBFS_MAGIC;
	buf->f_bsize = HPAGE_SIZE;
	if (sbinfo) {
		spin_lock(&sbinfo->stat_lock);
		buf->f_blocks = sbinfo->max_blocks;
		buf->f_bavail = buf->f_bfree = sbinfo->free_blocks;
		buf->f_files = sbinfo->max_inodes;
		buf->f_ffree = sbinfo->free_inodes;
		spin_unlock(&sbinfo->stat_lock);
	}
	buf->f_namelen = NAME_MAX;
	return 0;
}

static void hugetlbfs_put_super(struct super_block *sb)
{
	struct hugetlbfs_sb_info *sbi = HUGETLBFS_SB(sb);

	if (sbi) {
		sb->s_fs_info = NULL;
		kfree(sbi);
	}
}

static kmem_cache_t *hugetlbfs_inode_cachep;

static struct inode *hugetlbfs_alloc_inode(struct super_block *sb)
{
	struct hugetlbfs_inode_info *p;

	p = kmem_cache_alloc(hugetlbfs_inode_cachep, SLAB_KERNEL);
	if (!p)
		return NULL;
	return &p->vfs_inode;
}

static void init_once(void *foo, kmem_cache_t *cachep, unsigned long flags)
{
	struct hugetlbfs_inode_info *ei = (struct hugetlbfs_inode_info *)foo;

	if ((flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) ==
	    SLAB_CTOR_CONSTRUCTOR)
		inode_init_once(&ei->vfs_inode);
}

static void hugetlbfs_destroy_inode(struct inode *inode)
{
	mpol_free_shared_policy(&HUGETLBFS_I(inode)->policy);
	kmem_cache_free(hugetlbfs_inode_cachep, HUGETLBFS_I(inode));
}

static struct address_space_operations hugetlbfs_aops = {
	.readpage	= hugetlbfs_readpage,
	.prepare_write	= hugetlbfs_prepare_write,
	.commit_write	= hugetlbfs_commit_write,
	.set_page_dirty	= hugetlbfs_set_page_dirty,
};

struct file_operations hugetlbfs_file_operations = {
	.mmap			= hugetlbfs_file_mmap,
	.fsync			= simple_sync_file,
	.get_unmapped_area	= hugetlb_get_unmapped_area,
};

static struct inode_operations hugetlbfs_dir_inode_operations = {
	.create		= hugetlbfs_create,
	.lookup		= simple_lookup,
	.link		= simple_link,
	.unlink		= simple_unlink,
	.symlink	= hugetlbfs_symlink,
	.mkdir		= hugetlbfs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= hugetlbfs_mknod,
	.rename		= simple_rename,
	.setattr	= hugetlbfs_setattr,
};

static struct inode_operations hugetlbfs_inode_operations = {
	.setattr	= hugetlbfs_setattr,
};

static struct super_operations hugetlbfs_ops = {
	.alloc_inode    = hugetlbfs_alloc_inode,
	.destroy_inode  = hugetlbfs_destroy_inode,
	.statfs		= hugetlbfs_statfs,
	.drop_inode	= hugetlbfs_drop_inode,
	.put_super	= hugetlbfs_put_super,
};

static int
hugetlbfs_parse_options(char *options, struct hugetlbfs_config *pconfig)
{
	char *opt, *value, *rest;

	if (!options)
		return 0;
	while ((opt = strsep(&options, ",")) != NULL) {
		if (!*opt)
			continue;

		value = strchr(opt, '=');
		if (!value || !*value)
			return -EINVAL;
		else
			*value++ = '\0';

		if (!strcmp(opt, "uid"))
			pconfig->uid = simple_strtoul(value, &value, 0);
		else if (!strcmp(opt, "gid"))
			pconfig->gid = simple_strtoul(value, &value, 0);
		else if (!strcmp(opt, "mode"))
			pconfig->mode = simple_strtoul(value,&value,0) & 0777U;
		else if (!strcmp(opt, "size")) {
			unsigned long long size = memparse(value, &rest);
			if (*rest == '%') {
				size <<= HPAGE_SHIFT;
				size *= max_huge_pages;
				do_div(size, 100);
				rest++;
			}
			size &= HPAGE_MASK;
			pconfig->nr_blocks = (size >> HPAGE_SHIFT);
			value = rest;
		} else if (!strcmp(opt,"nr_inodes")) {
			pconfig->nr_inodes = memparse(value, &rest);
			value = rest;
		} else
			return -EINVAL;

		if (*value)
			return -EINVAL;
	}
	return 0;
}

static int
hugetlbfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct inode * inode;
	struct dentry * root;
	int ret;
	struct hugetlbfs_config config;
	struct hugetlbfs_sb_info *sbinfo;

	config.nr_blocks = -1; /* No limit on size by default */
	config.nr_inodes = -1; /* No limit on number of inodes by default */
	config.uid = current->fsuid;
	config.gid = current->fsgid;
	config.mode = 0755;
	ret = hugetlbfs_parse_options(data, &config);

	if (ret)
		return ret;

	sbinfo = kmalloc(sizeof(struct hugetlbfs_sb_info), GFP_KERNEL);
	if (!sbinfo)
		return -ENOMEM;
	sb->s_fs_info = sbinfo;
	spin_lock_init(&sbinfo->stat_lock);
	sbinfo->max_blocks = config.nr_blocks;
	sbinfo->free_blocks = config.nr_blocks;
	sbinfo->max_inodes = config.nr_inodes;
	sbinfo->free_inodes = config.nr_inodes;
	sb->s_blocksize = HPAGE_SIZE;
	sb->s_blocksize_bits = HPAGE_SHIFT;
	sb->s_magic = HUGETLBFS_MAGIC;
	sb->s_op = &hugetlbfs_ops;
	inode = hugetlbfs_get_inode(sb, config.uid, config.gid,
					S_IFDIR | config.mode, 0);
	if (!inode)
		goto out_free;

	root = d_alloc_root(inode);
	if (!root) {
		iput(inode);
		goto out_free;
	}
	sb->s_root = root;
	return 0;
out_free:
	kfree(sbinfo);
	return -ENOMEM;
}

int hugetlb_get_quota(struct address_space *mapping)
{
	int ret = 0;
	struct hugetlbfs_sb_info *sbinfo = HUGETLBFS_SB(mapping->host->i_sb);

	if (sbinfo->free_blocks > -1) {
		spin_lock(&sbinfo->stat_lock);
		if (sbinfo->free_blocks > 0)
			sbinfo->free_blocks--;
		else
			ret = -ENOMEM;
		spin_unlock(&sbinfo->stat_lock);
	}

	return ret;
}

void hugetlb_put_quota(struct address_space *mapping)
{
	struct hugetlbfs_sb_info *sbinfo = HUGETLBFS_SB(mapping->host->i_sb);

	if (sbinfo->free_blocks > -1) {
		spin_lock(&sbinfo->stat_lock);
		sbinfo->free_blocks++;
		spin_unlock(&sbinfo->stat_lock);
	}
}

static struct super_block *hugetlbfs_get_sb(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return get_sb_nodev(fs_type, flags, data, hugetlbfs_fill_super);
}

static struct file_system_type hugetlbfs_fs_type = {
	.name		= "hugetlbfs",
	.get_sb		= hugetlbfs_get_sb,
	.kill_sb	= kill_litter_super,
};

static struct vfsmount *hugetlbfs_vfsmount;

/*
 * Return the next identifier for a shm file
 */
static unsigned long hugetlbfs_counter(void)
{
	static spinlock_t lock = SPIN_LOCK_UNLOCKED;
	static unsigned long counter;
	unsigned long ret;

	spin_lock(&lock);
	ret = ++counter;
	spin_unlock(&lock);
	return ret;
}

static int can_do_hugetlb_shm(void)
{
	return likely(capable(CAP_IPC_LOCK) ||
			in_group_p(sysctl_hugetlb_shm_group));
}

struct file *hugetlb_zero_setup(size_t size)
{
	int error;
	struct file *file;
	struct inode *inode;
	struct dentry *dentry, *root;
	struct qstr quick_string;
	char buf[16];

	if (!can_do_hugetlb_shm())
		return ERR_PTR(-EPERM);

	if (!is_hugepage_mem_enough(size))
		return ERR_PTR(-ENOMEM);

	root = hugetlbfs_vfsmount->mnt_root;
	snprintf(buf, 16, "%lu", hugetlbfs_counter());
	quick_string.name = buf;
	quick_string.len = strlen(quick_string.name);
	quick_string.hash = 0;
	dentry = d_alloc(root, &quick_string);
	if (!dentry)
		return ERR_PTR(-ENOMEM);

	error = -ENFILE;
	file = get_empty_filp();
	if (!file)
		goto out_dentry;

	error = -ENOSPC;
	inode = hugetlbfs_get_inode(root->d_sb, current->fsuid,
				current->fsgid, S_IFREG | S_IRWXUGO, 0);
	if (!inode)
		goto out_file;

	d_instantiate(dentry, inode);
	inode->i_size = size;
	inode->i_nlink = 0;
	file->f_vfsmnt = mntget(hugetlbfs_vfsmount);
	file->f_dentry = dentry;
	file->f_mapping = inode->i_mapping;
	file->f_op = &hugetlbfs_file_operations;
	file->f_mode = FMODE_WRITE | FMODE_READ;
	return file;

out_file:
	put_filp(file);
out_dentry:
	dput(dentry);
	return ERR_PTR(error);
}

static int __init init_hugetlbfs_fs(void)
{
	int error;
	struct vfsmount *vfsmount;

	hugetlbfs_inode_cachep = kmem_cache_create("hugetlbfs_inode_cache",
					sizeof(struct hugetlbfs_inode_info),
					0, SLAB_RECLAIM_ACCOUNT,
					init_once, NULL);
	if (hugetlbfs_inode_cachep == NULL)
		return -ENOMEM;

	error = register_filesystem(&hugetlbfs_fs_type);
	if (error)
		goto out;

	vfsmount = kern_mount(&hugetlbfs_fs_type);

	if (!IS_ERR(vfsmount)) {
		hugetlbfs_vfsmount = vfsmount;
		return 0;
	}

	error = PTR_ERR(vfsmount);

 out:
	if (error)
		kmem_cache_destroy(hugetlbfs_inode_cachep);
	return error;
}

static void __exit exit_hugetlbfs_fs(void)
{
	kmem_cache_destroy(hugetlbfs_inode_cachep);
	unregister_filesystem(&hugetlbfs_fs_type);
}

module_init(init_hugetlbfs_fs)
module_exit(exit_hugetlbfs_fs)

MODULE_LICENSE("GPL");
