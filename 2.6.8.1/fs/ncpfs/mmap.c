/*
 *  mmap.c
 *
 *  Copyright (C) 1995, 1996 by Volker Lendecke
 *  Modified 1997 Peter Waltenberg, Bill Hawes, David Woodhouse for 2.1 dcache
 *
 */

#include <linux/stat.h>
#include <linux/time.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/shm.h>
#include <linux/errno.h>
#include <linux/mman.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/fcntl.h>
#include <linux/ncp_fs.h>

#include "ncplib_kernel.h"
#include <asm/uaccess.h>
#include <asm/system.h>

/*
 * Fill in the supplied page for mmap
 */
static struct page* ncp_file_mmap_nopage(struct vm_area_struct *area,
				     unsigned long address, int *type)
{
	struct file *file = area->vm_file;
	struct dentry *dentry = file->f_dentry;
	struct inode *inode = dentry->d_inode;
	struct page* page;
	char *pg_addr;
	unsigned int already_read;
	unsigned int count;
	int bufsize;
	int pos;

	page = alloc_page(GFP_HIGHUSER); /* ncpfs has nothing against high pages
	           as long as recvmsg and memset works on it */
	if (!page)
		return page;
	pg_addr = kmap(page);
	address &= PAGE_MASK;
	pos = address - area->vm_start + (area->vm_pgoff << PAGE_SHIFT);

	count = PAGE_SIZE;
	if (address + PAGE_SIZE > area->vm_end) {
		count = area->vm_end - address;
	}
	/* what we can read in one go */
	bufsize = NCP_SERVER(inode)->buffer_size;

	already_read = 0;
	if (ncp_make_open(inode, O_RDONLY) >= 0) {
		while (already_read < count) {
			int read_this_time;
			int to_read;

			to_read = bufsize - (pos % bufsize);

			to_read = min_t(unsigned int, to_read, count - already_read);

			if (ncp_read_kernel(NCP_SERVER(inode),
				     NCP_FINFO(inode)->file_handle,
				     pos, to_read,
				     pg_addr + already_read,
				     &read_this_time) != 0) {
				read_this_time = 0;
			}
			pos += read_this_time;
			already_read += read_this_time;

			if (read_this_time < to_read) {
				break;
			}
		}
		ncp_inode_close(inode);

	}

	if (already_read < PAGE_SIZE)
		memset(pg_addr + already_read, 0, PAGE_SIZE - already_read);
	flush_dcache_page(page);
	kunmap(page);

	/*
	 * If I understand ncp_read_kernel() properly, the above always
	 * fetches from the network, here the analogue of disk.
	 * -- wli
	 */
	if (type)
		*type = VM_FAULT_MAJOR;
	inc_page_state(pgmajfault);
	return page;
}

static struct vm_operations_struct ncp_file_mmap =
{
	.nopage	= ncp_file_mmap_nopage,
};


/* This is used for a general mmap of a ncp file */
int ncp_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file->f_dentry->d_inode;
	
	DPRINTK("ncp_mmap: called\n");

	if (!ncp_conn_valid(NCP_SERVER(inode))) {
		return -EIO;
	}
	/* only PAGE_COW or read-only supported now */
	if (vma->vm_flags & VM_SHARED)
		return -EINVAL;
	if (!inode->i_sb || !S_ISREG(inode->i_mode))
		return -EACCES;
	/* we do not support files bigger than 4GB... We eventually 
	   supports just 4GB... */
	if (((vma->vm_end - vma->vm_start) >> PAGE_SHIFT) + vma->vm_pgoff 
	   > (1U << (32 - PAGE_SHIFT)))
		return -EFBIG;
	if (!IS_RDONLY(inode)) {
		inode->i_atime = CURRENT_TIME;
	}

	vma->vm_ops = &ncp_file_mmap;
	return 0;
}
