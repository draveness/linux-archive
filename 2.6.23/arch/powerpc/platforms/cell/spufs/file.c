/*
 * SPU file system -- file contents
 *
 * (C) Copyright IBM Deutschland Entwicklung GmbH 2005
 *
 * Author: Arnd Bergmann <arndb@de.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#undef DEBUG

#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/poll.h>
#include <linux/ptrace.h>
#include <linux/seq_file.h>

#include <asm/io.h>
#include <asm/semaphore.h>
#include <asm/spu.h>
#include <asm/spu_info.h>
#include <asm/uaccess.h>

#include "spufs.h"

#define SPUFS_MMAP_4K (PAGE_SIZE == 0x1000)


static int
spufs_mem_open(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	struct spu_context *ctx = i->i_ctx;

	mutex_lock(&ctx->mapping_lock);
	file->private_data = ctx;
	if (!i->i_openers++)
		ctx->local_store = inode->i_mapping;
	mutex_unlock(&ctx->mapping_lock);
	return 0;
}

static int
spufs_mem_release(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	struct spu_context *ctx = i->i_ctx;

	mutex_lock(&ctx->mapping_lock);
	if (!--i->i_openers)
		ctx->local_store = NULL;
	mutex_unlock(&ctx->mapping_lock);
	return 0;
}

static ssize_t
__spufs_mem_read(struct spu_context *ctx, char __user *buffer,
			size_t size, loff_t *pos)
{
	char *local_store = ctx->ops->get_ls(ctx);
	return simple_read_from_buffer(buffer, size, pos, local_store,
					LS_SIZE);
}

static ssize_t
spufs_mem_read(struct file *file, char __user *buffer,
				size_t size, loff_t *pos)
{
	struct spu_context *ctx = file->private_data;
	ssize_t ret;

	spu_acquire(ctx);
	ret = __spufs_mem_read(ctx, buffer, size, pos);
	spu_release(ctx);
	return ret;
}

static ssize_t
spufs_mem_write(struct file *file, const char __user *buffer,
					size_t size, loff_t *ppos)
{
	struct spu_context *ctx = file->private_data;
	char *local_store;
	loff_t pos = *ppos;
	int ret;

	if (pos < 0)
		return -EINVAL;
	if (pos > LS_SIZE)
		return -EFBIG;
	if (size > LS_SIZE - pos)
		size = LS_SIZE - pos;

	spu_acquire(ctx);
	local_store = ctx->ops->get_ls(ctx);
	ret = copy_from_user(local_store + pos, buffer, size);
	spu_release(ctx);

	if (ret)
		return -EFAULT;
	*ppos = pos + size;
	return size;
}

static unsigned long spufs_mem_mmap_nopfn(struct vm_area_struct *vma,
					  unsigned long address)
{
	struct spu_context *ctx	= vma->vm_file->private_data;
	unsigned long pfn, offset, addr0 = address;
#ifdef CONFIG_SPU_FS_64K_LS
	struct spu_state *csa = &ctx->csa;
	int psize;

	/* Check what page size we are using */
	psize = get_slice_psize(vma->vm_mm, address);

	/* Some sanity checking */
	BUG_ON(csa->use_big_pages != (psize == MMU_PAGE_64K));

	/* Wow, 64K, cool, we need to align the address though */
	if (csa->use_big_pages) {
		BUG_ON(vma->vm_start & 0xffff);
		address &= ~0xfffful;
	}
#endif /* CONFIG_SPU_FS_64K_LS */

	offset = (address - vma->vm_start) + (vma->vm_pgoff << PAGE_SHIFT);
	if (offset >= LS_SIZE)
		return NOPFN_SIGBUS;

	pr_debug("spufs_mem_mmap_nopfn address=0x%lx -> 0x%lx, offset=0x%lx\n",
		 addr0, address, offset);

	spu_acquire(ctx);

	if (ctx->state == SPU_STATE_SAVED) {
		vma->vm_page_prot = __pgprot(pgprot_val(vma->vm_page_prot)
							& ~_PAGE_NO_CACHE);
		pfn = vmalloc_to_pfn(ctx->csa.lscsa->ls + offset);
	} else {
		vma->vm_page_prot = __pgprot(pgprot_val(vma->vm_page_prot)
					     | _PAGE_NO_CACHE);
		pfn = (ctx->spu->local_store_phys + offset) >> PAGE_SHIFT;
	}
	vm_insert_pfn(vma, address, pfn);

	spu_release(ctx);

	return NOPFN_REFAULT;
}


static struct vm_operations_struct spufs_mem_mmap_vmops = {
	.nopfn = spufs_mem_mmap_nopfn,
};

static int spufs_mem_mmap(struct file *file, struct vm_area_struct *vma)
{
#ifdef CONFIG_SPU_FS_64K_LS
	struct spu_context	*ctx = file->private_data;
	struct spu_state	*csa = &ctx->csa;

	/* Sanity check VMA alignment */
	if (csa->use_big_pages) {
		pr_debug("spufs_mem_mmap 64K, start=0x%lx, end=0x%lx,"
			 " pgoff=0x%lx\n", vma->vm_start, vma->vm_end,
			 vma->vm_pgoff);
		if (vma->vm_start & 0xffff)
			return -EINVAL;
		if (vma->vm_pgoff & 0xf)
			return -EINVAL;
	}
#endif /* CONFIG_SPU_FS_64K_LS */

	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	vma->vm_flags |= VM_IO | VM_PFNMAP;
	vma->vm_page_prot = __pgprot(pgprot_val(vma->vm_page_prot)
				     | _PAGE_NO_CACHE);

	vma->vm_ops = &spufs_mem_mmap_vmops;
	return 0;
}

#ifdef CONFIG_SPU_FS_64K_LS
unsigned long spufs_get_unmapped_area(struct file *file, unsigned long addr,
				      unsigned long len, unsigned long pgoff,
				      unsigned long flags)
{
	struct spu_context	*ctx = file->private_data;
	struct spu_state	*csa = &ctx->csa;

	/* If not using big pages, fallback to normal MM g_u_a */
	if (!csa->use_big_pages)
		return current->mm->get_unmapped_area(file, addr, len,
						      pgoff, flags);

	/* Else, try to obtain a 64K pages slice */
	return slice_get_unmapped_area(addr, len, flags,
				       MMU_PAGE_64K, 1, 0);
}
#endif /* CONFIG_SPU_FS_64K_LS */

static const struct file_operations spufs_mem_fops = {
	.open			= spufs_mem_open,
	.release		= spufs_mem_release,
	.read			= spufs_mem_read,
	.write			= spufs_mem_write,
	.llseek			= generic_file_llseek,
	.mmap			= spufs_mem_mmap,
#ifdef CONFIG_SPU_FS_64K_LS
	.get_unmapped_area	= spufs_get_unmapped_area,
#endif
};

static unsigned long spufs_ps_nopfn(struct vm_area_struct *vma,
				    unsigned long address,
				    unsigned long ps_offs,
				    unsigned long ps_size)
{
	struct spu_context *ctx = vma->vm_file->private_data;
	unsigned long area, offset = address - vma->vm_start;
	int ret;

	offset += vma->vm_pgoff << PAGE_SHIFT;
	if (offset >= ps_size)
		return NOPFN_SIGBUS;

	/* error here usually means a signal.. we might want to test
	 * the error code more precisely though
	 */
	ret = spu_acquire_runnable(ctx, 0);
	if (ret)
		return NOPFN_REFAULT;

	area = ctx->spu->problem_phys + ps_offs;
	vm_insert_pfn(vma, address, (area + offset) >> PAGE_SHIFT);
	spu_release(ctx);

	return NOPFN_REFAULT;
}

#if SPUFS_MMAP_4K
static unsigned long spufs_cntl_mmap_nopfn(struct vm_area_struct *vma,
					   unsigned long address)
{
	return spufs_ps_nopfn(vma, address, 0x4000, 0x1000);
}

static struct vm_operations_struct spufs_cntl_mmap_vmops = {
	.nopfn = spufs_cntl_mmap_nopfn,
};

/*
 * mmap support for problem state control area [0x4000 - 0x4fff].
 */
static int spufs_cntl_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	vma->vm_flags |= VM_IO | VM_PFNMAP;
	vma->vm_page_prot = __pgprot(pgprot_val(vma->vm_page_prot)
				     | _PAGE_NO_CACHE | _PAGE_GUARDED);

	vma->vm_ops = &spufs_cntl_mmap_vmops;
	return 0;
}
#else /* SPUFS_MMAP_4K */
#define spufs_cntl_mmap NULL
#endif /* !SPUFS_MMAP_4K */

static u64 spufs_cntl_get(void *data)
{
	struct spu_context *ctx = data;
	u64 val;

	spu_acquire(ctx);
	val = ctx->ops->status_read(ctx);
	spu_release(ctx);

	return val;
}

static void spufs_cntl_set(void *data, u64 val)
{
	struct spu_context *ctx = data;

	spu_acquire(ctx);
	ctx->ops->runcntl_write(ctx, val);
	spu_release(ctx);
}

static int spufs_cntl_open(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	struct spu_context *ctx = i->i_ctx;

	mutex_lock(&ctx->mapping_lock);
	file->private_data = ctx;
	if (!i->i_openers++)
		ctx->cntl = inode->i_mapping;
	mutex_unlock(&ctx->mapping_lock);
	return simple_attr_open(inode, file, spufs_cntl_get,
					spufs_cntl_set, "0x%08lx");
}

static int
spufs_cntl_release(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	struct spu_context *ctx = i->i_ctx;

	simple_attr_close(inode, file);

	mutex_lock(&ctx->mapping_lock);
	if (!--i->i_openers)
		ctx->cntl = NULL;
	mutex_unlock(&ctx->mapping_lock);
	return 0;
}

static const struct file_operations spufs_cntl_fops = {
	.open = spufs_cntl_open,
	.release = spufs_cntl_release,
	.read = simple_attr_read,
	.write = simple_attr_write,
	.mmap = spufs_cntl_mmap,
};

static int
spufs_regs_open(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	file->private_data = i->i_ctx;
	return 0;
}

static ssize_t
__spufs_regs_read(struct spu_context *ctx, char __user *buffer,
			size_t size, loff_t *pos)
{
	struct spu_lscsa *lscsa = ctx->csa.lscsa;
	return simple_read_from_buffer(buffer, size, pos,
				      lscsa->gprs, sizeof lscsa->gprs);
}

static ssize_t
spufs_regs_read(struct file *file, char __user *buffer,
		size_t size, loff_t *pos)
{
	int ret;
	struct spu_context *ctx = file->private_data;

	spu_acquire_saved(ctx);
	ret = __spufs_regs_read(ctx, buffer, size, pos);
	spu_release_saved(ctx);
	return ret;
}

static ssize_t
spufs_regs_write(struct file *file, const char __user *buffer,
		 size_t size, loff_t *pos)
{
	struct spu_context *ctx = file->private_data;
	struct spu_lscsa *lscsa = ctx->csa.lscsa;
	int ret;

	size = min_t(ssize_t, sizeof lscsa->gprs - *pos, size);
	if (size <= 0)
		return -EFBIG;
	*pos += size;

	spu_acquire_saved(ctx);

	ret = copy_from_user(lscsa->gprs + *pos - size,
			     buffer, size) ? -EFAULT : size;

	spu_release_saved(ctx);
	return ret;
}

static const struct file_operations spufs_regs_fops = {
	.open	 = spufs_regs_open,
	.read    = spufs_regs_read,
	.write   = spufs_regs_write,
	.llseek  = generic_file_llseek,
};

static ssize_t
__spufs_fpcr_read(struct spu_context *ctx, char __user * buffer,
			size_t size, loff_t * pos)
{
	struct spu_lscsa *lscsa = ctx->csa.lscsa;
	return simple_read_from_buffer(buffer, size, pos,
				      &lscsa->fpcr, sizeof(lscsa->fpcr));
}

static ssize_t
spufs_fpcr_read(struct file *file, char __user * buffer,
		size_t size, loff_t * pos)
{
	int ret;
	struct spu_context *ctx = file->private_data;

	spu_acquire_saved(ctx);
	ret = __spufs_fpcr_read(ctx, buffer, size, pos);
	spu_release_saved(ctx);
	return ret;
}

static ssize_t
spufs_fpcr_write(struct file *file, const char __user * buffer,
		 size_t size, loff_t * pos)
{
	struct spu_context *ctx = file->private_data;
	struct spu_lscsa *lscsa = ctx->csa.lscsa;
	int ret;

	size = min_t(ssize_t, sizeof(lscsa->fpcr) - *pos, size);
	if (size <= 0)
		return -EFBIG;
	*pos += size;

	spu_acquire_saved(ctx);

	ret = copy_from_user((char *)&lscsa->fpcr + *pos - size,
			     buffer, size) ? -EFAULT : size;

	spu_release_saved(ctx);
	return ret;
}

static const struct file_operations spufs_fpcr_fops = {
	.open = spufs_regs_open,
	.read = spufs_fpcr_read,
	.write = spufs_fpcr_write,
	.llseek = generic_file_llseek,
};

/* generic open function for all pipe-like files */
static int spufs_pipe_open(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	file->private_data = i->i_ctx;

	return nonseekable_open(inode, file);
}

/*
 * Read as many bytes from the mailbox as possible, until
 * one of the conditions becomes true:
 *
 * - no more data available in the mailbox
 * - end of the user provided buffer
 * - end of the mapped area
 */
static ssize_t spufs_mbox_read(struct file *file, char __user *buf,
			size_t len, loff_t *pos)
{
	struct spu_context *ctx = file->private_data;
	u32 mbox_data, __user *udata;
	ssize_t count;

	if (len < 4)
		return -EINVAL;

	if (!access_ok(VERIFY_WRITE, buf, len))
		return -EFAULT;

	udata = (void __user *)buf;

	spu_acquire(ctx);
	for (count = 0; (count + 4) <= len; count += 4, udata++) {
		int ret;
		ret = ctx->ops->mbox_read(ctx, &mbox_data);
		if (ret == 0)
			break;

		/*
		 * at the end of the mapped area, we can fault
		 * but still need to return the data we have
		 * read successfully so far.
		 */
		ret = __put_user(mbox_data, udata);
		if (ret) {
			if (!count)
				count = -EFAULT;
			break;
		}
	}
	spu_release(ctx);

	if (!count)
		count = -EAGAIN;

	return count;
}

static const struct file_operations spufs_mbox_fops = {
	.open	= spufs_pipe_open,
	.read	= spufs_mbox_read,
};

static ssize_t spufs_mbox_stat_read(struct file *file, char __user *buf,
			size_t len, loff_t *pos)
{
	struct spu_context *ctx = file->private_data;
	u32 mbox_stat;

	if (len < 4)
		return -EINVAL;

	spu_acquire(ctx);

	mbox_stat = ctx->ops->mbox_stat_read(ctx) & 0xff;

	spu_release(ctx);

	if (copy_to_user(buf, &mbox_stat, sizeof mbox_stat))
		return -EFAULT;

	return 4;
}

static const struct file_operations spufs_mbox_stat_fops = {
	.open	= spufs_pipe_open,
	.read	= spufs_mbox_stat_read,
};

/* low-level ibox access function */
size_t spu_ibox_read(struct spu_context *ctx, u32 *data)
{
	return ctx->ops->ibox_read(ctx, data);
}

static int spufs_ibox_fasync(int fd, struct file *file, int on)
{
	struct spu_context *ctx = file->private_data;

	return fasync_helper(fd, file, on, &ctx->ibox_fasync);
}

/* interrupt-level ibox callback function. */
void spufs_ibox_callback(struct spu *spu)
{
	struct spu_context *ctx = spu->ctx;

	wake_up_all(&ctx->ibox_wq);
	kill_fasync(&ctx->ibox_fasync, SIGIO, POLLIN);
}

/*
 * Read as many bytes from the interrupt mailbox as possible, until
 * one of the conditions becomes true:
 *
 * - no more data available in the mailbox
 * - end of the user provided buffer
 * - end of the mapped area
 *
 * If the file is opened without O_NONBLOCK, we wait here until
 * any data is available, but return when we have been able to
 * read something.
 */
static ssize_t spufs_ibox_read(struct file *file, char __user *buf,
			size_t len, loff_t *pos)
{
	struct spu_context *ctx = file->private_data;
	u32 ibox_data, __user *udata;
	ssize_t count;

	if (len < 4)
		return -EINVAL;

	if (!access_ok(VERIFY_WRITE, buf, len))
		return -EFAULT;

	udata = (void __user *)buf;

	spu_acquire(ctx);

	/* wait only for the first element */
	count = 0;
	if (file->f_flags & O_NONBLOCK) {
		if (!spu_ibox_read(ctx, &ibox_data))
			count = -EAGAIN;
	} else {
		count = spufs_wait(ctx->ibox_wq, spu_ibox_read(ctx, &ibox_data));
	}
	if (count)
		goto out;

	/* if we can't write at all, return -EFAULT */
	count = __put_user(ibox_data, udata);
	if (count)
		goto out;

	for (count = 4, udata++; (count + 4) <= len; count += 4, udata++) {
		int ret;
		ret = ctx->ops->ibox_read(ctx, &ibox_data);
		if (ret == 0)
			break;
		/*
		 * at the end of the mapped area, we can fault
		 * but still need to return the data we have
		 * read successfully so far.
		 */
		ret = __put_user(ibox_data, udata);
		if (ret)
			break;
	}

out:
	spu_release(ctx);

	return count;
}

static unsigned int spufs_ibox_poll(struct file *file, poll_table *wait)
{
	struct spu_context *ctx = file->private_data;
	unsigned int mask;

	poll_wait(file, &ctx->ibox_wq, wait);

	spu_acquire(ctx);
	mask = ctx->ops->mbox_stat_poll(ctx, POLLIN | POLLRDNORM);
	spu_release(ctx);

	return mask;
}

static const struct file_operations spufs_ibox_fops = {
	.open	= spufs_pipe_open,
	.read	= spufs_ibox_read,
	.poll	= spufs_ibox_poll,
	.fasync	= spufs_ibox_fasync,
};

static ssize_t spufs_ibox_stat_read(struct file *file, char __user *buf,
			size_t len, loff_t *pos)
{
	struct spu_context *ctx = file->private_data;
	u32 ibox_stat;

	if (len < 4)
		return -EINVAL;

	spu_acquire(ctx);
	ibox_stat = (ctx->ops->mbox_stat_read(ctx) >> 16) & 0xff;
	spu_release(ctx);

	if (copy_to_user(buf, &ibox_stat, sizeof ibox_stat))
		return -EFAULT;

	return 4;
}

static const struct file_operations spufs_ibox_stat_fops = {
	.open	= spufs_pipe_open,
	.read	= spufs_ibox_stat_read,
};

/* low-level mailbox write */
size_t spu_wbox_write(struct spu_context *ctx, u32 data)
{
	return ctx->ops->wbox_write(ctx, data);
}

static int spufs_wbox_fasync(int fd, struct file *file, int on)
{
	struct spu_context *ctx = file->private_data;
	int ret;

	ret = fasync_helper(fd, file, on, &ctx->wbox_fasync);

	return ret;
}

/* interrupt-level wbox callback function. */
void spufs_wbox_callback(struct spu *spu)
{
	struct spu_context *ctx = spu->ctx;

	wake_up_all(&ctx->wbox_wq);
	kill_fasync(&ctx->wbox_fasync, SIGIO, POLLOUT);
}

/*
 * Write as many bytes to the interrupt mailbox as possible, until
 * one of the conditions becomes true:
 *
 * - the mailbox is full
 * - end of the user provided buffer
 * - end of the mapped area
 *
 * If the file is opened without O_NONBLOCK, we wait here until
 * space is availabyl, but return when we have been able to
 * write something.
 */
static ssize_t spufs_wbox_write(struct file *file, const char __user *buf,
			size_t len, loff_t *pos)
{
	struct spu_context *ctx = file->private_data;
	u32 wbox_data, __user *udata;
	ssize_t count;

	if (len < 4)
		return -EINVAL;

	udata = (void __user *)buf;
	if (!access_ok(VERIFY_READ, buf, len))
		return -EFAULT;

	if (__get_user(wbox_data, udata))
		return -EFAULT;

	spu_acquire(ctx);

	/*
	 * make sure we can at least write one element, by waiting
	 * in case of !O_NONBLOCK
	 */
	count = 0;
	if (file->f_flags & O_NONBLOCK) {
		if (!spu_wbox_write(ctx, wbox_data))
			count = -EAGAIN;
	} else {
		count = spufs_wait(ctx->wbox_wq, spu_wbox_write(ctx, wbox_data));
	}

	if (count)
		goto out;

	/* write aѕ much as possible */
	for (count = 4, udata++; (count + 4) <= len; count += 4, udata++) {
		int ret;
		ret = __get_user(wbox_data, udata);
		if (ret)
			break;

		ret = spu_wbox_write(ctx, wbox_data);
		if (ret == 0)
			break;
	}

out:
	spu_release(ctx);
	return count;
}

static unsigned int spufs_wbox_poll(struct file *file, poll_table *wait)
{
	struct spu_context *ctx = file->private_data;
	unsigned int mask;

	poll_wait(file, &ctx->wbox_wq, wait);

	spu_acquire(ctx);
	mask = ctx->ops->mbox_stat_poll(ctx, POLLOUT | POLLWRNORM);
	spu_release(ctx);

	return mask;
}

static const struct file_operations spufs_wbox_fops = {
	.open	= spufs_pipe_open,
	.write	= spufs_wbox_write,
	.poll	= spufs_wbox_poll,
	.fasync	= spufs_wbox_fasync,
};

static ssize_t spufs_wbox_stat_read(struct file *file, char __user *buf,
			size_t len, loff_t *pos)
{
	struct spu_context *ctx = file->private_data;
	u32 wbox_stat;

	if (len < 4)
		return -EINVAL;

	spu_acquire(ctx);
	wbox_stat = (ctx->ops->mbox_stat_read(ctx) >> 8) & 0xff;
	spu_release(ctx);

	if (copy_to_user(buf, &wbox_stat, sizeof wbox_stat))
		return -EFAULT;

	return 4;
}

static const struct file_operations spufs_wbox_stat_fops = {
	.open	= spufs_pipe_open,
	.read	= spufs_wbox_stat_read,
};

static int spufs_signal1_open(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	struct spu_context *ctx = i->i_ctx;

	mutex_lock(&ctx->mapping_lock);
	file->private_data = ctx;
	if (!i->i_openers++)
		ctx->signal1 = inode->i_mapping;
	mutex_unlock(&ctx->mapping_lock);
	return nonseekable_open(inode, file);
}

static int
spufs_signal1_release(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	struct spu_context *ctx = i->i_ctx;

	mutex_lock(&ctx->mapping_lock);
	if (!--i->i_openers)
		ctx->signal1 = NULL;
	mutex_unlock(&ctx->mapping_lock);
	return 0;
}

static ssize_t __spufs_signal1_read(struct spu_context *ctx, char __user *buf,
			size_t len, loff_t *pos)
{
	int ret = 0;
	u32 data;

	if (len < 4)
		return -EINVAL;

	if (ctx->csa.spu_chnlcnt_RW[3]) {
		data = ctx->csa.spu_chnldata_RW[3];
		ret = 4;
	}

	if (!ret)
		goto out;

	if (copy_to_user(buf, &data, 4))
		return -EFAULT;

out:
	return ret;
}

static ssize_t spufs_signal1_read(struct file *file, char __user *buf,
			size_t len, loff_t *pos)
{
	int ret;
	struct spu_context *ctx = file->private_data;

	spu_acquire_saved(ctx);
	ret = __spufs_signal1_read(ctx, buf, len, pos);
	spu_release_saved(ctx);

	return ret;
}

static ssize_t spufs_signal1_write(struct file *file, const char __user *buf,
			size_t len, loff_t *pos)
{
	struct spu_context *ctx;
	u32 data;

	ctx = file->private_data;

	if (len < 4)
		return -EINVAL;

	if (copy_from_user(&data, buf, 4))
		return -EFAULT;

	spu_acquire(ctx);
	ctx->ops->signal1_write(ctx, data);
	spu_release(ctx);

	return 4;
}

static unsigned long spufs_signal1_mmap_nopfn(struct vm_area_struct *vma,
					      unsigned long address)
{
#if PAGE_SIZE == 0x1000
	return spufs_ps_nopfn(vma, address, 0x14000, 0x1000);
#elif PAGE_SIZE == 0x10000
	/* For 64k pages, both signal1 and signal2 can be used to mmap the whole
	 * signal 1 and 2 area
	 */
	return spufs_ps_nopfn(vma, address, 0x10000, 0x10000);
#else
#error unsupported page size
#endif
}

static struct vm_operations_struct spufs_signal1_mmap_vmops = {
	.nopfn = spufs_signal1_mmap_nopfn,
};

static int spufs_signal1_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	vma->vm_flags |= VM_IO | VM_PFNMAP;
	vma->vm_page_prot = __pgprot(pgprot_val(vma->vm_page_prot)
				     | _PAGE_NO_CACHE | _PAGE_GUARDED);

	vma->vm_ops = &spufs_signal1_mmap_vmops;
	return 0;
}

static const struct file_operations spufs_signal1_fops = {
	.open = spufs_signal1_open,
	.release = spufs_signal1_release,
	.read = spufs_signal1_read,
	.write = spufs_signal1_write,
	.mmap = spufs_signal1_mmap,
};

static const struct file_operations spufs_signal1_nosched_fops = {
	.open = spufs_signal1_open,
	.release = spufs_signal1_release,
	.write = spufs_signal1_write,
	.mmap = spufs_signal1_mmap,
};

static int spufs_signal2_open(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	struct spu_context *ctx = i->i_ctx;

	mutex_lock(&ctx->mapping_lock);
	file->private_data = ctx;
	if (!i->i_openers++)
		ctx->signal2 = inode->i_mapping;
	mutex_unlock(&ctx->mapping_lock);
	return nonseekable_open(inode, file);
}

static int
spufs_signal2_release(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	struct spu_context *ctx = i->i_ctx;

	mutex_lock(&ctx->mapping_lock);
	if (!--i->i_openers)
		ctx->signal2 = NULL;
	mutex_unlock(&ctx->mapping_lock);
	return 0;
}

static ssize_t __spufs_signal2_read(struct spu_context *ctx, char __user *buf,
			size_t len, loff_t *pos)
{
	int ret = 0;
	u32 data;

	if (len < 4)
		return -EINVAL;

	if (ctx->csa.spu_chnlcnt_RW[4]) {
		data =  ctx->csa.spu_chnldata_RW[4];
		ret = 4;
	}

	if (!ret)
		goto out;

	if (copy_to_user(buf, &data, 4))
		return -EFAULT;

out:
	return ret;
}

static ssize_t spufs_signal2_read(struct file *file, char __user *buf,
			size_t len, loff_t *pos)
{
	struct spu_context *ctx = file->private_data;
	int ret;

	spu_acquire_saved(ctx);
	ret = __spufs_signal2_read(ctx, buf, len, pos);
	spu_release_saved(ctx);

	return ret;
}

static ssize_t spufs_signal2_write(struct file *file, const char __user *buf,
			size_t len, loff_t *pos)
{
	struct spu_context *ctx;
	u32 data;

	ctx = file->private_data;

	if (len < 4)
		return -EINVAL;

	if (copy_from_user(&data, buf, 4))
		return -EFAULT;

	spu_acquire(ctx);
	ctx->ops->signal2_write(ctx, data);
	spu_release(ctx);

	return 4;
}

#if SPUFS_MMAP_4K
static unsigned long spufs_signal2_mmap_nopfn(struct vm_area_struct *vma,
					      unsigned long address)
{
#if PAGE_SIZE == 0x1000
	return spufs_ps_nopfn(vma, address, 0x1c000, 0x1000);
#elif PAGE_SIZE == 0x10000
	/* For 64k pages, both signal1 and signal2 can be used to mmap the whole
	 * signal 1 and 2 area
	 */
	return spufs_ps_nopfn(vma, address, 0x10000, 0x10000);
#else
#error unsupported page size
#endif
}

static struct vm_operations_struct spufs_signal2_mmap_vmops = {
	.nopfn = spufs_signal2_mmap_nopfn,
};

static int spufs_signal2_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	vma->vm_flags |= VM_IO | VM_PFNMAP;
	vma->vm_page_prot = __pgprot(pgprot_val(vma->vm_page_prot)
				     | _PAGE_NO_CACHE | _PAGE_GUARDED);

	vma->vm_ops = &spufs_signal2_mmap_vmops;
	return 0;
}
#else /* SPUFS_MMAP_4K */
#define spufs_signal2_mmap NULL
#endif /* !SPUFS_MMAP_4K */

static const struct file_operations spufs_signal2_fops = {
	.open = spufs_signal2_open,
	.release = spufs_signal2_release,
	.read = spufs_signal2_read,
	.write = spufs_signal2_write,
	.mmap = spufs_signal2_mmap,
};

static const struct file_operations spufs_signal2_nosched_fops = {
	.open = spufs_signal2_open,
	.release = spufs_signal2_release,
	.write = spufs_signal2_write,
	.mmap = spufs_signal2_mmap,
};

static void spufs_signal1_type_set(void *data, u64 val)
{
	struct spu_context *ctx = data;

	spu_acquire(ctx);
	ctx->ops->signal1_type_set(ctx, val);
	spu_release(ctx);
}

static u64 __spufs_signal1_type_get(void *data)
{
	struct spu_context *ctx = data;
	return ctx->ops->signal1_type_get(ctx);
}

static u64 spufs_signal1_type_get(void *data)
{
	struct spu_context *ctx = data;
	u64 ret;

	spu_acquire(ctx);
	ret = __spufs_signal1_type_get(data);
	spu_release(ctx);

	return ret;
}
DEFINE_SIMPLE_ATTRIBUTE(spufs_signal1_type, spufs_signal1_type_get,
					spufs_signal1_type_set, "%llu");

static void spufs_signal2_type_set(void *data, u64 val)
{
	struct spu_context *ctx = data;

	spu_acquire(ctx);
	ctx->ops->signal2_type_set(ctx, val);
	spu_release(ctx);
}

static u64 __spufs_signal2_type_get(void *data)
{
	struct spu_context *ctx = data;
	return ctx->ops->signal2_type_get(ctx);
}

static u64 spufs_signal2_type_get(void *data)
{
	struct spu_context *ctx = data;
	u64 ret;

	spu_acquire(ctx);
	ret = __spufs_signal2_type_get(data);
	spu_release(ctx);

	return ret;
}
DEFINE_SIMPLE_ATTRIBUTE(spufs_signal2_type, spufs_signal2_type_get,
					spufs_signal2_type_set, "%llu");

#if SPUFS_MMAP_4K
static unsigned long spufs_mss_mmap_nopfn(struct vm_area_struct *vma,
					  unsigned long address)
{
	return spufs_ps_nopfn(vma, address, 0x0000, 0x1000);
}

static struct vm_operations_struct spufs_mss_mmap_vmops = {
	.nopfn = spufs_mss_mmap_nopfn,
};

/*
 * mmap support for problem state MFC DMA area [0x0000 - 0x0fff].
 */
static int spufs_mss_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	vma->vm_flags |= VM_IO | VM_PFNMAP;
	vma->vm_page_prot = __pgprot(pgprot_val(vma->vm_page_prot)
				     | _PAGE_NO_CACHE | _PAGE_GUARDED);

	vma->vm_ops = &spufs_mss_mmap_vmops;
	return 0;
}
#else /* SPUFS_MMAP_4K */
#define spufs_mss_mmap NULL
#endif /* !SPUFS_MMAP_4K */

static int spufs_mss_open(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	struct spu_context *ctx = i->i_ctx;

	file->private_data = i->i_ctx;

	mutex_lock(&ctx->mapping_lock);
	if (!i->i_openers++)
		ctx->mss = inode->i_mapping;
	mutex_unlock(&ctx->mapping_lock);
	return nonseekable_open(inode, file);
}

static int
spufs_mss_release(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	struct spu_context *ctx = i->i_ctx;

	mutex_lock(&ctx->mapping_lock);
	if (!--i->i_openers)
		ctx->mss = NULL;
	mutex_unlock(&ctx->mapping_lock);
	return 0;
}

static const struct file_operations spufs_mss_fops = {
	.open	 = spufs_mss_open,
	.release = spufs_mss_release,
	.mmap	 = spufs_mss_mmap,
};

static unsigned long spufs_psmap_mmap_nopfn(struct vm_area_struct *vma,
					    unsigned long address)
{
	return spufs_ps_nopfn(vma, address, 0x0000, 0x20000);
}

static struct vm_operations_struct spufs_psmap_mmap_vmops = {
	.nopfn = spufs_psmap_mmap_nopfn,
};

/*
 * mmap support for full problem state area [0x00000 - 0x1ffff].
 */
static int spufs_psmap_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	vma->vm_flags |= VM_IO | VM_PFNMAP;
	vma->vm_page_prot = __pgprot(pgprot_val(vma->vm_page_prot)
				     | _PAGE_NO_CACHE | _PAGE_GUARDED);

	vma->vm_ops = &spufs_psmap_mmap_vmops;
	return 0;
}

static int spufs_psmap_open(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	struct spu_context *ctx = i->i_ctx;

	mutex_lock(&ctx->mapping_lock);
	file->private_data = i->i_ctx;
	if (!i->i_openers++)
		ctx->psmap = inode->i_mapping;
	mutex_unlock(&ctx->mapping_lock);
	return nonseekable_open(inode, file);
}

static int
spufs_psmap_release(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	struct spu_context *ctx = i->i_ctx;

	mutex_lock(&ctx->mapping_lock);
	if (!--i->i_openers)
		ctx->psmap = NULL;
	mutex_unlock(&ctx->mapping_lock);
	return 0;
}

static const struct file_operations spufs_psmap_fops = {
	.open	 = spufs_psmap_open,
	.release = spufs_psmap_release,
	.mmap	 = spufs_psmap_mmap,
};


#if SPUFS_MMAP_4K
static unsigned long spufs_mfc_mmap_nopfn(struct vm_area_struct *vma,
					  unsigned long address)
{
	return spufs_ps_nopfn(vma, address, 0x3000, 0x1000);
}

static struct vm_operations_struct spufs_mfc_mmap_vmops = {
	.nopfn = spufs_mfc_mmap_nopfn,
};

/*
 * mmap support for problem state MFC DMA area [0x0000 - 0x0fff].
 */
static int spufs_mfc_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	vma->vm_flags |= VM_IO | VM_PFNMAP;
	vma->vm_page_prot = __pgprot(pgprot_val(vma->vm_page_prot)
				     | _PAGE_NO_CACHE | _PAGE_GUARDED);

	vma->vm_ops = &spufs_mfc_mmap_vmops;
	return 0;
}
#else /* SPUFS_MMAP_4K */
#define spufs_mfc_mmap NULL
#endif /* !SPUFS_MMAP_4K */

static int spufs_mfc_open(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	struct spu_context *ctx = i->i_ctx;

	/* we don't want to deal with DMA into other processes */
	if (ctx->owner != current->mm)
		return -EINVAL;

	if (atomic_read(&inode->i_count) != 1)
		return -EBUSY;

	mutex_lock(&ctx->mapping_lock);
	file->private_data = ctx;
	if (!i->i_openers++)
		ctx->mfc = inode->i_mapping;
	mutex_unlock(&ctx->mapping_lock);
	return nonseekable_open(inode, file);
}

static int
spufs_mfc_release(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	struct spu_context *ctx = i->i_ctx;

	mutex_lock(&ctx->mapping_lock);
	if (!--i->i_openers)
		ctx->mfc = NULL;
	mutex_unlock(&ctx->mapping_lock);
	return 0;
}

/* interrupt-level mfc callback function. */
void spufs_mfc_callback(struct spu *spu)
{
	struct spu_context *ctx = spu->ctx;

	wake_up_all(&ctx->mfc_wq);

	pr_debug("%s %s\n", __FUNCTION__, spu->name);
	if (ctx->mfc_fasync) {
		u32 free_elements, tagstatus;
		unsigned int mask;

		/* no need for spu_acquire in interrupt context */
		free_elements = ctx->ops->get_mfc_free_elements(ctx);
		tagstatus = ctx->ops->read_mfc_tagstatus(ctx);

		mask = 0;
		if (free_elements & 0xffff)
			mask |= POLLOUT;
		if (tagstatus & ctx->tagwait)
			mask |= POLLIN;

		kill_fasync(&ctx->mfc_fasync, SIGIO, mask);
	}
}

static int spufs_read_mfc_tagstatus(struct spu_context *ctx, u32 *status)
{
	/* See if there is one tag group is complete */
	/* FIXME we need locking around tagwait */
	*status = ctx->ops->read_mfc_tagstatus(ctx) & ctx->tagwait;
	ctx->tagwait &= ~*status;
	if (*status)
		return 1;

	/* enable interrupt waiting for any tag group,
	   may silently fail if interrupts are already enabled */
	ctx->ops->set_mfc_query(ctx, ctx->tagwait, 1);
	return 0;
}

static ssize_t spufs_mfc_read(struct file *file, char __user *buffer,
			size_t size, loff_t *pos)
{
	struct spu_context *ctx = file->private_data;
	int ret = -EINVAL;
	u32 status;

	if (size != 4)
		goto out;

	spu_acquire(ctx);
	if (file->f_flags & O_NONBLOCK) {
		status = ctx->ops->read_mfc_tagstatus(ctx);
		if (!(status & ctx->tagwait))
			ret = -EAGAIN;
		else
			ctx->tagwait &= ~status;
	} else {
		ret = spufs_wait(ctx->mfc_wq,
			   spufs_read_mfc_tagstatus(ctx, &status));
	}
	spu_release(ctx);

	if (ret)
		goto out;

	ret = 4;
	if (copy_to_user(buffer, &status, 4))
		ret = -EFAULT;

out:
	return ret;
}

static int spufs_check_valid_dma(struct mfc_dma_command *cmd)
{
	pr_debug("queueing DMA %x %lx %x %x %x\n", cmd->lsa,
		 cmd->ea, cmd->size, cmd->tag, cmd->cmd);

	switch (cmd->cmd) {
	case MFC_PUT_CMD:
	case MFC_PUTF_CMD:
	case MFC_PUTB_CMD:
	case MFC_GET_CMD:
	case MFC_GETF_CMD:
	case MFC_GETB_CMD:
		break;
	default:
		pr_debug("invalid DMA opcode %x\n", cmd->cmd);
		return -EIO;
	}

	if ((cmd->lsa & 0xf) != (cmd->ea &0xf)) {
		pr_debug("invalid DMA alignment, ea %lx lsa %x\n",
				cmd->ea, cmd->lsa);
		return -EIO;
	}

	switch (cmd->size & 0xf) {
	case 1:
		break;
	case 2:
		if (cmd->lsa & 1)
			goto error;
		break;
	case 4:
		if (cmd->lsa & 3)
			goto error;
		break;
	case 8:
		if (cmd->lsa & 7)
			goto error;
		break;
	case 0:
		if (cmd->lsa & 15)
			goto error;
		break;
	error:
	default:
		pr_debug("invalid DMA alignment %x for size %x\n",
			cmd->lsa & 0xf, cmd->size);
		return -EIO;
	}

	if (cmd->size > 16 * 1024) {
		pr_debug("invalid DMA size %x\n", cmd->size);
		return -EIO;
	}

	if (cmd->tag & 0xfff0) {
		/* we reserve the higher tag numbers for kernel use */
		pr_debug("invalid DMA tag\n");
		return -EIO;
	}

	if (cmd->class) {
		/* not supported in this version */
		pr_debug("invalid DMA class\n");
		return -EIO;
	}

	return 0;
}

static int spu_send_mfc_command(struct spu_context *ctx,
				struct mfc_dma_command cmd,
				int *error)
{
	*error = ctx->ops->send_mfc_command(ctx, &cmd);
	if (*error == -EAGAIN) {
		/* wait for any tag group to complete
		   so we have space for the new command */
		ctx->ops->set_mfc_query(ctx, ctx->tagwait, 1);
		/* try again, because the queue might be
		   empty again */
		*error = ctx->ops->send_mfc_command(ctx, &cmd);
		if (*error == -EAGAIN)
			return 0;
	}
	return 1;
}

static ssize_t spufs_mfc_write(struct file *file, const char __user *buffer,
			size_t size, loff_t *pos)
{
	struct spu_context *ctx = file->private_data;
	struct mfc_dma_command cmd;
	int ret = -EINVAL;

	if (size != sizeof cmd)
		goto out;

	ret = -EFAULT;
	if (copy_from_user(&cmd, buffer, sizeof cmd))
		goto out;

	ret = spufs_check_valid_dma(&cmd);
	if (ret)
		goto out;

	ret = spu_acquire_runnable(ctx, 0);
	if (ret)
		goto out;

	if (file->f_flags & O_NONBLOCK) {
		ret = ctx->ops->send_mfc_command(ctx, &cmd);
	} else {
		int status;
		ret = spufs_wait(ctx->mfc_wq,
				 spu_send_mfc_command(ctx, cmd, &status));
		if (status)
			ret = status;
	}

	if (ret)
		goto out_unlock;

	ctx->tagwait |= 1 << cmd.tag;
	ret = size;

out_unlock:
	spu_release(ctx);
out:
	return ret;
}

static unsigned int spufs_mfc_poll(struct file *file,poll_table *wait)
{
	struct spu_context *ctx = file->private_data;
	u32 free_elements, tagstatus;
	unsigned int mask;

	poll_wait(file, &ctx->mfc_wq, wait);

	spu_acquire(ctx);
	ctx->ops->set_mfc_query(ctx, ctx->tagwait, 2);
	free_elements = ctx->ops->get_mfc_free_elements(ctx);
	tagstatus = ctx->ops->read_mfc_tagstatus(ctx);
	spu_release(ctx);

	mask = 0;
	if (free_elements & 0xffff)
		mask |= POLLOUT | POLLWRNORM;
	if (tagstatus & ctx->tagwait)
		mask |= POLLIN | POLLRDNORM;

	pr_debug("%s: free %d tagstatus %d tagwait %d\n", __FUNCTION__,
		free_elements, tagstatus, ctx->tagwait);

	return mask;
}

static int spufs_mfc_flush(struct file *file, fl_owner_t id)
{
	struct spu_context *ctx = file->private_data;
	int ret;

	spu_acquire(ctx);
#if 0
/* this currently hangs */
	ret = spufs_wait(ctx->mfc_wq,
			 ctx->ops->set_mfc_query(ctx, ctx->tagwait, 2));
	if (ret)
		goto out;
	ret = spufs_wait(ctx->mfc_wq,
			 ctx->ops->read_mfc_tagstatus(ctx) == ctx->tagwait);
out:
#else
	ret = 0;
#endif
	spu_release(ctx);

	return ret;
}

static int spufs_mfc_fsync(struct file *file, struct dentry *dentry,
			   int datasync)
{
	return spufs_mfc_flush(file, NULL);
}

static int spufs_mfc_fasync(int fd, struct file *file, int on)
{
	struct spu_context *ctx = file->private_data;

	return fasync_helper(fd, file, on, &ctx->mfc_fasync);
}

static const struct file_operations spufs_mfc_fops = {
	.open	 = spufs_mfc_open,
	.release = spufs_mfc_release,
	.read	 = spufs_mfc_read,
	.write	 = spufs_mfc_write,
	.poll	 = spufs_mfc_poll,
	.flush	 = spufs_mfc_flush,
	.fsync	 = spufs_mfc_fsync,
	.fasync	 = spufs_mfc_fasync,
	.mmap	 = spufs_mfc_mmap,
};

static void spufs_npc_set(void *data, u64 val)
{
	struct spu_context *ctx = data;
	spu_acquire(ctx);
	ctx->ops->npc_write(ctx, val);
	spu_release(ctx);
}

static u64 spufs_npc_get(void *data)
{
	struct spu_context *ctx = data;
	u64 ret;
	spu_acquire(ctx);
	ret = ctx->ops->npc_read(ctx);
	spu_release(ctx);
	return ret;
}
DEFINE_SIMPLE_ATTRIBUTE(spufs_npc_ops, spufs_npc_get, spufs_npc_set,
			"0x%llx\n")

static void spufs_decr_set(void *data, u64 val)
{
	struct spu_context *ctx = data;
	struct spu_lscsa *lscsa = ctx->csa.lscsa;
	spu_acquire_saved(ctx);
	lscsa->decr.slot[0] = (u32) val;
	spu_release_saved(ctx);
}

static u64 __spufs_decr_get(void *data)
{
	struct spu_context *ctx = data;
	struct spu_lscsa *lscsa = ctx->csa.lscsa;
	return lscsa->decr.slot[0];
}

static u64 spufs_decr_get(void *data)
{
	struct spu_context *ctx = data;
	u64 ret;
	spu_acquire_saved(ctx);
	ret = __spufs_decr_get(data);
	spu_release_saved(ctx);
	return ret;
}
DEFINE_SIMPLE_ATTRIBUTE(spufs_decr_ops, spufs_decr_get, spufs_decr_set,
			"0x%llx\n")

static void spufs_decr_status_set(void *data, u64 val)
{
	struct spu_context *ctx = data;
	spu_acquire_saved(ctx);
	if (val)
		ctx->csa.priv2.mfc_control_RW |= MFC_CNTL_DECREMENTER_RUNNING;
	else
		ctx->csa.priv2.mfc_control_RW &= ~MFC_CNTL_DECREMENTER_RUNNING;
	spu_release_saved(ctx);
}

static u64 __spufs_decr_status_get(void *data)
{
	struct spu_context *ctx = data;
	if (ctx->csa.priv2.mfc_control_RW & MFC_CNTL_DECREMENTER_RUNNING)
		return SPU_DECR_STATUS_RUNNING;
	else
		return 0;
}

static u64 spufs_decr_status_get(void *data)
{
	struct spu_context *ctx = data;
	u64 ret;
	spu_acquire_saved(ctx);
	ret = __spufs_decr_status_get(data);
	spu_release_saved(ctx);
	return ret;
}
DEFINE_SIMPLE_ATTRIBUTE(spufs_decr_status_ops, spufs_decr_status_get,
			spufs_decr_status_set, "0x%llx\n")

static void spufs_event_mask_set(void *data, u64 val)
{
	struct spu_context *ctx = data;
	struct spu_lscsa *lscsa = ctx->csa.lscsa;
	spu_acquire_saved(ctx);
	lscsa->event_mask.slot[0] = (u32) val;
	spu_release_saved(ctx);
}

static u64 __spufs_event_mask_get(void *data)
{
	struct spu_context *ctx = data;
	struct spu_lscsa *lscsa = ctx->csa.lscsa;
	return lscsa->event_mask.slot[0];
}

static u64 spufs_event_mask_get(void *data)
{
	struct spu_context *ctx = data;
	u64 ret;
	spu_acquire_saved(ctx);
	ret = __spufs_event_mask_get(data);
	spu_release_saved(ctx);
	return ret;
}
DEFINE_SIMPLE_ATTRIBUTE(spufs_event_mask_ops, spufs_event_mask_get,
			spufs_event_mask_set, "0x%llx\n")

static u64 __spufs_event_status_get(void *data)
{
	struct spu_context *ctx = data;
	struct spu_state *state = &ctx->csa;
	u64 stat;
	stat = state->spu_chnlcnt_RW[0];
	if (stat)
		return state->spu_chnldata_RW[0];
	return 0;
}

static u64 spufs_event_status_get(void *data)
{
	struct spu_context *ctx = data;
	u64 ret = 0;

	spu_acquire_saved(ctx);
	ret = __spufs_event_status_get(data);
	spu_release_saved(ctx);
	return ret;
}
DEFINE_SIMPLE_ATTRIBUTE(spufs_event_status_ops, spufs_event_status_get,
			NULL, "0x%llx\n")

static void spufs_srr0_set(void *data, u64 val)
{
	struct spu_context *ctx = data;
	struct spu_lscsa *lscsa = ctx->csa.lscsa;
	spu_acquire_saved(ctx);
	lscsa->srr0.slot[0] = (u32) val;
	spu_release_saved(ctx);
}

static u64 spufs_srr0_get(void *data)
{
	struct spu_context *ctx = data;
	struct spu_lscsa *lscsa = ctx->csa.lscsa;
	u64 ret;
	spu_acquire_saved(ctx);
	ret = lscsa->srr0.slot[0];
	spu_release_saved(ctx);
	return ret;
}
DEFINE_SIMPLE_ATTRIBUTE(spufs_srr0_ops, spufs_srr0_get, spufs_srr0_set,
			"0x%llx\n")

static u64 spufs_id_get(void *data)
{
	struct spu_context *ctx = data;
	u64 num;

	spu_acquire(ctx);
	if (ctx->state == SPU_STATE_RUNNABLE)
		num = ctx->spu->number;
	else
		num = (unsigned int)-1;
	spu_release(ctx);

	return num;
}
DEFINE_SIMPLE_ATTRIBUTE(spufs_id_ops, spufs_id_get, NULL, "0x%llx\n")

static u64 __spufs_object_id_get(void *data)
{
	struct spu_context *ctx = data;
	return ctx->object_id;
}

static u64 spufs_object_id_get(void *data)
{
	/* FIXME: Should there really be no locking here? */
	return __spufs_object_id_get(data);
}

static void spufs_object_id_set(void *data, u64 id)
{
	struct spu_context *ctx = data;
	ctx->object_id = id;
}

DEFINE_SIMPLE_ATTRIBUTE(spufs_object_id_ops, spufs_object_id_get,
		spufs_object_id_set, "0x%llx\n");

static u64 __spufs_lslr_get(void *data)
{
	struct spu_context *ctx = data;
	return ctx->csa.priv2.spu_lslr_RW;
}

static u64 spufs_lslr_get(void *data)
{
	struct spu_context *ctx = data;
	u64 ret;

	spu_acquire_saved(ctx);
	ret = __spufs_lslr_get(data);
	spu_release_saved(ctx);

	return ret;
}
DEFINE_SIMPLE_ATTRIBUTE(spufs_lslr_ops, spufs_lslr_get, NULL, "0x%llx\n")

static int spufs_info_open(struct inode *inode, struct file *file)
{
	struct spufs_inode_info *i = SPUFS_I(inode);
	struct spu_context *ctx = i->i_ctx;
	file->private_data = ctx;
	return 0;
}

static int spufs_caps_show(struct seq_file *s, void *private)
{
	struct spu_context *ctx = s->private;

	if (!(ctx->flags & SPU_CREATE_NOSCHED))
		seq_puts(s, "sched\n");
	if (!(ctx->flags & SPU_CREATE_ISOLATE))
		seq_puts(s, "step\n");
	return 0;
}

static int spufs_caps_open(struct inode *inode, struct file *file)
{
	return single_open(file, spufs_caps_show, SPUFS_I(inode)->i_ctx);
}

static const struct file_operations spufs_caps_fops = {
	.open		= spufs_caps_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static ssize_t __spufs_mbox_info_read(struct spu_context *ctx,
			char __user *buf, size_t len, loff_t *pos)
{
	u32 mbox_stat;
	u32 data;

	mbox_stat = ctx->csa.prob.mb_stat_R;
	if (mbox_stat & 0x0000ff) {
		data = ctx->csa.prob.pu_mb_R;
	}

	return simple_read_from_buffer(buf, len, pos, &data, sizeof data);
}

static ssize_t spufs_mbox_info_read(struct file *file, char __user *buf,
				   size_t len, loff_t *pos)
{
	int ret;
	struct spu_context *ctx = file->private_data;

	if (!access_ok(VERIFY_WRITE, buf, len))
		return -EFAULT;

	spu_acquire_saved(ctx);
	spin_lock(&ctx->csa.register_lock);
	ret = __spufs_mbox_info_read(ctx, buf, len, pos);
	spin_unlock(&ctx->csa.register_lock);
	spu_release_saved(ctx);

	return ret;
}

static const struct file_operations spufs_mbox_info_fops = {
	.open = spufs_info_open,
	.read = spufs_mbox_info_read,
	.llseek  = generic_file_llseek,
};

static ssize_t __spufs_ibox_info_read(struct spu_context *ctx,
				char __user *buf, size_t len, loff_t *pos)
{
	u32 ibox_stat;
	u32 data;

	ibox_stat = ctx->csa.prob.mb_stat_R;
	if (ibox_stat & 0xff0000) {
		data = ctx->csa.priv2.puint_mb_R;
	}

	return simple_read_from_buffer(buf, len, pos, &data, sizeof data);
}

static ssize_t spufs_ibox_info_read(struct file *file, char __user *buf,
				   size_t len, loff_t *pos)
{
	struct spu_context *ctx = file->private_data;
	int ret;

	if (!access_ok(VERIFY_WRITE, buf, len))
		return -EFAULT;

	spu_acquire_saved(ctx);
	spin_lock(&ctx->csa.register_lock);
	ret = __spufs_ibox_info_read(ctx, buf, len, pos);
	spin_unlock(&ctx->csa.register_lock);
	spu_release_saved(ctx);

	return ret;
}

static const struct file_operations spufs_ibox_info_fops = {
	.open = spufs_info_open,
	.read = spufs_ibox_info_read,
	.llseek  = generic_file_llseek,
};

static ssize_t __spufs_wbox_info_read(struct spu_context *ctx,
			char __user *buf, size_t len, loff_t *pos)
{
	int i, cnt;
	u32 data[4];
	u32 wbox_stat;

	wbox_stat = ctx->csa.prob.mb_stat_R;
	cnt = 4 - ((wbox_stat & 0x00ff00) >> 8);
	for (i = 0; i < cnt; i++) {
		data[i] = ctx->csa.spu_mailbox_data[i];
	}

	return simple_read_from_buffer(buf, len, pos, &data,
				cnt * sizeof(u32));
}

static ssize_t spufs_wbox_info_read(struct file *file, char __user *buf,
				   size_t len, loff_t *pos)
{
	struct spu_context *ctx = file->private_data;
	int ret;

	if (!access_ok(VERIFY_WRITE, buf, len))
		return -EFAULT;

	spu_acquire_saved(ctx);
	spin_lock(&ctx->csa.register_lock);
	ret = __spufs_wbox_info_read(ctx, buf, len, pos);
	spin_unlock(&ctx->csa.register_lock);
	spu_release_saved(ctx);

	return ret;
}

static const struct file_operations spufs_wbox_info_fops = {
	.open = spufs_info_open,
	.read = spufs_wbox_info_read,
	.llseek  = generic_file_llseek,
};

static ssize_t __spufs_dma_info_read(struct spu_context *ctx,
			char __user *buf, size_t len, loff_t *pos)
{
	struct spu_dma_info info;
	struct mfc_cq_sr *qp, *spuqp;
	int i;

	info.dma_info_type = ctx->csa.priv2.spu_tag_status_query_RW;
	info.dma_info_mask = ctx->csa.lscsa->tag_mask.slot[0];
	info.dma_info_status = ctx->csa.spu_chnldata_RW[24];
	info.dma_info_stall_and_notify = ctx->csa.spu_chnldata_RW[25];
	info.dma_info_atomic_command_status = ctx->csa.spu_chnldata_RW[27];
	for (i = 0; i < 16; i++) {
		qp = &info.dma_info_command_data[i];
		spuqp = &ctx->csa.priv2.spuq[i];

		qp->mfc_cq_data0_RW = spuqp->mfc_cq_data0_RW;
		qp->mfc_cq_data1_RW = spuqp->mfc_cq_data1_RW;
		qp->mfc_cq_data2_RW = spuqp->mfc_cq_data2_RW;
		qp->mfc_cq_data3_RW = spuqp->mfc_cq_data3_RW;
	}

	return simple_read_from_buffer(buf, len, pos, &info,
				sizeof info);
}

static ssize_t spufs_dma_info_read(struct file *file, char __user *buf,
			      size_t len, loff_t *pos)
{
	struct spu_context *ctx = file->private_data;
	int ret;

	if (!access_ok(VERIFY_WRITE, buf, len))
		return -EFAULT;

	spu_acquire_saved(ctx);
	spin_lock(&ctx->csa.register_lock);
	ret = __spufs_dma_info_read(ctx, buf, len, pos);
	spin_unlock(&ctx->csa.register_lock);
	spu_release_saved(ctx);

	return ret;
}

static const struct file_operations spufs_dma_info_fops = {
	.open = spufs_info_open,
	.read = spufs_dma_info_read,
};

static ssize_t __spufs_proxydma_info_read(struct spu_context *ctx,
			char __user *buf, size_t len, loff_t *pos)
{
	struct spu_proxydma_info info;
	struct mfc_cq_sr *qp, *puqp;
	int ret = sizeof info;
	int i;

	if (len < ret)
		return -EINVAL;

	if (!access_ok(VERIFY_WRITE, buf, len))
		return -EFAULT;

	info.proxydma_info_type = ctx->csa.prob.dma_querytype_RW;
	info.proxydma_info_mask = ctx->csa.prob.dma_querymask_RW;
	info.proxydma_info_status = ctx->csa.prob.dma_tagstatus_R;
	for (i = 0; i < 8; i++) {
		qp = &info.proxydma_info_command_data[i];
		puqp = &ctx->csa.priv2.puq[i];

		qp->mfc_cq_data0_RW = puqp->mfc_cq_data0_RW;
		qp->mfc_cq_data1_RW = puqp->mfc_cq_data1_RW;
		qp->mfc_cq_data2_RW = puqp->mfc_cq_data2_RW;
		qp->mfc_cq_data3_RW = puqp->mfc_cq_data3_RW;
	}

	return simple_read_from_buffer(buf, len, pos, &info,
				sizeof info);
}

static ssize_t spufs_proxydma_info_read(struct file *file, char __user *buf,
				   size_t len, loff_t *pos)
{
	struct spu_context *ctx = file->private_data;
	int ret;

	spu_acquire_saved(ctx);
	spin_lock(&ctx->csa.register_lock);
	ret = __spufs_proxydma_info_read(ctx, buf, len, pos);
	spin_unlock(&ctx->csa.register_lock);
	spu_release_saved(ctx);

	return ret;
}

static const struct file_operations spufs_proxydma_info_fops = {
	.open = spufs_info_open,
	.read = spufs_proxydma_info_read,
};

static int spufs_show_tid(struct seq_file *s, void *private)
{
	struct spu_context *ctx = s->private;

	seq_printf(s, "%d\n", ctx->tid);
	return 0;
}

static int spufs_tid_open(struct inode *inode, struct file *file)
{
	return single_open(file, spufs_show_tid, SPUFS_I(inode)->i_ctx);
}

static const struct file_operations spufs_tid_fops = {
	.open		= spufs_tid_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static const char *ctx_state_names[] = {
	"user", "system", "iowait", "loaded"
};

static unsigned long long spufs_acct_time(struct spu_context *ctx,
		enum spu_utilization_state state)
{
	struct timespec ts;
	unsigned long long time = ctx->stats.times[state];

	/*
	 * In general, utilization statistics are updated by the controlling
	 * thread as the spu context moves through various well defined
	 * state transitions, but if the context is lazily loaded its
	 * utilization statistics are not updated as the controlling thread
	 * is not tightly coupled with the execution of the spu context.  We
	 * calculate and apply the time delta from the last recorded state
	 * of the spu context.
	 */
	if (ctx->spu && ctx->stats.util_state == state) {
		ktime_get_ts(&ts);
		time += timespec_to_ns(&ts) - ctx->stats.tstamp;
	}

	return time / NSEC_PER_MSEC;
}

static unsigned long long spufs_slb_flts(struct spu_context *ctx)
{
	unsigned long long slb_flts = ctx->stats.slb_flt;

	if (ctx->state == SPU_STATE_RUNNABLE) {
		slb_flts += (ctx->spu->stats.slb_flt -
			     ctx->stats.slb_flt_base);
	}

	return slb_flts;
}

static unsigned long long spufs_class2_intrs(struct spu_context *ctx)
{
	unsigned long long class2_intrs = ctx->stats.class2_intr;

	if (ctx->state == SPU_STATE_RUNNABLE) {
		class2_intrs += (ctx->spu->stats.class2_intr -
				 ctx->stats.class2_intr_base);
	}

	return class2_intrs;
}


static int spufs_show_stat(struct seq_file *s, void *private)
{
	struct spu_context *ctx = s->private;

	spu_acquire(ctx);
	seq_printf(s, "%s %llu %llu %llu %llu "
		      "%llu %llu %llu %llu %llu %llu %llu %llu\n",
		ctx_state_names[ctx->stats.util_state],
		spufs_acct_time(ctx, SPU_UTIL_USER),
		spufs_acct_time(ctx, SPU_UTIL_SYSTEM),
		spufs_acct_time(ctx, SPU_UTIL_IOWAIT),
		spufs_acct_time(ctx, SPU_UTIL_IDLE_LOADED),
		ctx->stats.vol_ctx_switch,
		ctx->stats.invol_ctx_switch,
		spufs_slb_flts(ctx),
		ctx->stats.hash_flt,
		ctx->stats.min_flt,
		ctx->stats.maj_flt,
		spufs_class2_intrs(ctx),
		ctx->stats.libassist);
	spu_release(ctx);
	return 0;
}

static int spufs_stat_open(struct inode *inode, struct file *file)
{
	return single_open(file, spufs_show_stat, SPUFS_I(inode)->i_ctx);
}

static const struct file_operations spufs_stat_fops = {
	.open		= spufs_stat_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};


struct tree_descr spufs_dir_contents[] = {
	{ "capabilities", &spufs_caps_fops, 0444, },
	{ "mem",  &spufs_mem_fops,  0666, },
	{ "regs", &spufs_regs_fops,  0666, },
	{ "mbox", &spufs_mbox_fops, 0444, },
	{ "ibox", &spufs_ibox_fops, 0444, },
	{ "wbox", &spufs_wbox_fops, 0222, },
	{ "mbox_stat", &spufs_mbox_stat_fops, 0444, },
	{ "ibox_stat", &spufs_ibox_stat_fops, 0444, },
	{ "wbox_stat", &spufs_wbox_stat_fops, 0444, },
	{ "signal1", &spufs_signal1_fops, 0666, },
	{ "signal2", &spufs_signal2_fops, 0666, },
	{ "signal1_type", &spufs_signal1_type, 0666, },
	{ "signal2_type", &spufs_signal2_type, 0666, },
	{ "cntl", &spufs_cntl_fops,  0666, },
	{ "fpcr", &spufs_fpcr_fops, 0666, },
	{ "lslr", &spufs_lslr_ops, 0444, },
	{ "mfc", &spufs_mfc_fops, 0666, },
	{ "mss", &spufs_mss_fops, 0666, },
	{ "npc", &spufs_npc_ops, 0666, },
	{ "srr0", &spufs_srr0_ops, 0666, },
	{ "decr", &spufs_decr_ops, 0666, },
	{ "decr_status", &spufs_decr_status_ops, 0666, },
	{ "event_mask", &spufs_event_mask_ops, 0666, },
	{ "event_status", &spufs_event_status_ops, 0444, },
	{ "psmap", &spufs_psmap_fops, 0666, },
	{ "phys-id", &spufs_id_ops, 0666, },
	{ "object-id", &spufs_object_id_ops, 0666, },
	{ "mbox_info", &spufs_mbox_info_fops, 0444, },
	{ "ibox_info", &spufs_ibox_info_fops, 0444, },
	{ "wbox_info", &spufs_wbox_info_fops, 0444, },
	{ "dma_info", &spufs_dma_info_fops, 0444, },
	{ "proxydma_info", &spufs_proxydma_info_fops, 0444, },
	{ "tid", &spufs_tid_fops, 0444, },
	{ "stat", &spufs_stat_fops, 0444, },
	{},
};

struct tree_descr spufs_dir_nosched_contents[] = {
	{ "capabilities", &spufs_caps_fops, 0444, },
	{ "mem",  &spufs_mem_fops,  0666, },
	{ "mbox", &spufs_mbox_fops, 0444, },
	{ "ibox", &spufs_ibox_fops, 0444, },
	{ "wbox", &spufs_wbox_fops, 0222, },
	{ "mbox_stat", &spufs_mbox_stat_fops, 0444, },
	{ "ibox_stat", &spufs_ibox_stat_fops, 0444, },
	{ "wbox_stat", &spufs_wbox_stat_fops, 0444, },
	{ "signal1", &spufs_signal1_nosched_fops, 0222, },
	{ "signal2", &spufs_signal2_nosched_fops, 0222, },
	{ "signal1_type", &spufs_signal1_type, 0666, },
	{ "signal2_type", &spufs_signal2_type, 0666, },
	{ "mss", &spufs_mss_fops, 0666, },
	{ "mfc", &spufs_mfc_fops, 0666, },
	{ "cntl", &spufs_cntl_fops,  0666, },
	{ "npc", &spufs_npc_ops, 0666, },
	{ "psmap", &spufs_psmap_fops, 0666, },
	{ "phys-id", &spufs_id_ops, 0666, },
	{ "object-id", &spufs_object_id_ops, 0666, },
	{ "tid", &spufs_tid_fops, 0444, },
	{ "stat", &spufs_stat_fops, 0444, },
	{},
};

struct spufs_coredump_reader spufs_coredump_read[] = {
	{ "regs", __spufs_regs_read, NULL, 128 * 16 },
	{ "fpcr", __spufs_fpcr_read, NULL, 16 },
	{ "lslr", NULL, __spufs_lslr_get, 11 },
	{ "decr", NULL, __spufs_decr_get, 11 },
	{ "decr_status", NULL, __spufs_decr_status_get, 11 },
	{ "mem", __spufs_mem_read, NULL, 256 * 1024, },
	{ "signal1", __spufs_signal1_read, NULL, 4 },
	{ "signal1_type", NULL, __spufs_signal1_type_get, 2 },
	{ "signal2", __spufs_signal2_read, NULL, 4 },
	{ "signal2_type", NULL, __spufs_signal2_type_get, 2 },
	{ "event_mask", NULL, __spufs_event_mask_get, 8 },
	{ "event_status", NULL, __spufs_event_status_get, 8 },
	{ "mbox_info", __spufs_mbox_info_read, NULL, 4 },
	{ "ibox_info", __spufs_ibox_info_read, NULL, 4 },
	{ "wbox_info", __spufs_wbox_info_read, NULL, 16 },
	{ "dma_info", __spufs_dma_info_read, NULL, 69 * 8 },
	{ "proxydma_info", __spufs_proxydma_info_read, NULL, 35 * 8 },
	{ "object-id", NULL, __spufs_object_id_get, 19 },
	{ },
};
int spufs_coredump_num_notes = ARRAY_SIZE(spufs_coredump_read) - 1;

