/*
 *  linux/fs/ufs/inode.c
 *
 * Copyright (C) 1998
 * Daniel Pirkl <daniel.pirkl@email.cz>
 * Charles University, Faculty of Mathematics and Physics
 *
 *  from
 *
 *  linux/fs/ext2/inode.c
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
 *  Goal-directed block allocation by Stephen Tweedie (sct@dcs.ed.ac.uk), 1993
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 */

#include <asm/uaccess.h>
#include <asm/system.h>

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ufs_fs.h>
#include <linux/sched.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/locks.h>
#include <linux/mm.h>
#include <linux/smp_lock.h>

#include "swab.h"
#include "util.h"

#undef UFS_INODE_DEBUG
#undef UFS_INODE_DEBUG_MORE

#ifdef UFS_INODE_DEBUG
#define UFSD(x) printk("(%s, %d), %s: ", __FILE__, __LINE__, __FUNCTION__); printk x;
#else
#define UFSD(x)
#endif

#ifdef UFS_INODE_DEBUG_MORE
static void ufs_print_inode(struct inode * inode)
{
	unsigned swab = inode->i_sb->u.ufs_sb.s_swab;
	printk("ino %lu  mode 0%6.6o  nlink %d  uid %d  gid %d"
	       "  size %lu blocks %lu\n",
	       inode->i_ino, inode->i_mode, inode->i_nlink,
	       inode->i_uid, inode->i_gid, 
	       inode->i_size, inode->i_blocks);
	printk("  db <%u %u %u %u %u %u %u %u %u %u %u %u>\n",
		SWAB32(inode->u.ufs_i.i_u1.i_data[0]),
		SWAB32(inode->u.ufs_i.i_u1.i_data[1]),
		SWAB32(inode->u.ufs_i.i_u1.i_data[2]),
		SWAB32(inode->u.ufs_i.i_u1.i_data[3]),
		SWAB32(inode->u.ufs_i.i_u1.i_data[4]),
		SWAB32(inode->u.ufs_i.i_u1.i_data[5]),
		SWAB32(inode->u.ufs_i.i_u1.i_data[6]),
		SWAB32(inode->u.ufs_i.i_u1.i_data[7]),
		SWAB32(inode->u.ufs_i.i_u1.i_data[8]),
		SWAB32(inode->u.ufs_i.i_u1.i_data[9]),
		SWAB32(inode->u.ufs_i.i_u1.i_data[10]),
		SWAB32(inode->u.ufs_i.i_u1.i_data[11]));
	printk("  gen %u ib <%u %u %u>\n",
		inode->u.ufs_i.i_gen,
		SWAB32(inode->u.ufs_i.i_u1.i_data[UFS_IND_BLOCK]),
		SWAB32(inode->u.ufs_i.i_u1.i_data[UFS_DIND_BLOCK]),
		SWAB32(inode->u.ufs_i.i_u1.i_data[UFS_TIND_BLOCK]));
}
#endif

#define ufs_inode_bmap(inode, nr) \
	(SWAB32((inode)->u.ufs_i.i_u1.i_data[(nr) >> uspi->s_fpbshift]) + ((nr) & uspi->s_fpbmask))

static inline unsigned int ufs_block_bmap (struct buffer_head * bh, unsigned nr, 
	struct ufs_sb_private_info * uspi, unsigned swab)
{
	unsigned int tmp;

	UFSD(("ENTER, nr %u\n", nr))
	if (!bh)
		return 0;
	tmp = SWAB32(((u32 *) bh->b_data)[nr >> uspi->s_fpbshift]) + (nr & uspi->s_fpbmask);
	brelse (bh);
	UFSD(("EXIT, result %u\n", tmp))
	return tmp;
}

int ufs_frag_map(struct inode *inode, int frag)
{
	struct super_block *sb;
	struct ufs_sb_private_info *uspi;
	unsigned int swab;
	int i, ret;

	ret = 0;
	lock_kernel();
	
	sb = inode->i_sb;
	uspi = sb->u.ufs_sb.s_uspi;
	swab = sb->u.ufs_sb.s_swab;
	if (frag < 0) {
		ufs_warning(sb, "ufs_frag_map", "frag < 0");
		goto out;
	}
	if (frag >=
	    ((UFS_NDADDR + uspi->s_apb + uspi->s_2apb + uspi->s_3apb)
	     << uspi->s_fpbshift)) {
		ufs_warning(sb, "ufs_frag_map", "frag > big");
		goto out;
	}

	if (frag < UFS_NDIR_FRAGMENT) {
		ret = uspi->s_sbbase + ufs_inode_bmap(inode, frag);
		goto out;
	}

	frag -= UFS_NDIR_FRAGMENT;
	if (frag < (1 << (uspi->s_apbshift + uspi->s_fpbshift))) {
		i = ufs_inode_bmap(inode,
				   UFS_IND_FRAGMENT + (frag >> uspi->s_apbshift));
		if (!i)
			goto out;
		ret = (uspi->s_sbbase +
		       ufs_block_bmap(bread(sb->s_dev, uspi->s_sbbase + i,
					    sb->s_blocksize),
				      frag & uspi->s_apbmask, uspi, swab));
		goto out;
	}
	frag -= 1 << (uspi->s_apbshift + uspi->s_fpbshift);
	if (frag < (1 << (uspi->s_2apbshift + uspi->s_fpbshift))) {
		i = ufs_inode_bmap (inode,
				    UFS_DIND_FRAGMENT + (frag >> uspi->s_2apbshift));
		if (!i)
			goto out;
		i = ufs_block_bmap(bread(sb->s_dev, uspi->s_sbbase + i,
					 sb->s_blocksize),
				   (frag >> uspi->s_apbshift) & uspi->s_apbmask,
				   uspi, swab);
		if (!i)
			goto out;
		ret = (uspi->s_sbbase +
		       ufs_block_bmap(bread(sb->s_dev, uspi->s_sbbase + i,
					    sb->s_blocksize),
				      (frag & uspi->s_apbmask), uspi, swab));
		goto out;
	}
	frag -= 1 << (uspi->s_2apbshift + uspi->s_fpbshift);
	i = ufs_inode_bmap(inode,
			   UFS_TIND_FRAGMENT + (frag >> uspi->s_3apbshift));
	if (!i)
		goto out;
	i = ufs_block_bmap(bread(sb->s_dev, uspi->s_sbbase + i, sb->s_blocksize),
			   (frag >> uspi->s_2apbshift) & uspi->s_apbmask,
			   uspi, swab);
	if (!i)
		goto out;
	i = ufs_block_bmap(bread(sb->s_dev, uspi->s_sbbase + i, sb->s_blocksize),
			   (frag >> uspi->s_apbshift) & uspi->s_apbmask,
			   uspi, swab);
	if (!i)
		goto out;
	ret = (uspi->s_sbbase +
	       ufs_block_bmap(bread(sb->s_dev, uspi->s_sbbase + i, sb->s_blocksize),
			      (frag & uspi->s_apbmask), uspi, swab));
out:
	unlock_kernel();
	return ret;
}

static struct buffer_head * ufs_inode_getfrag (struct inode *inode,
	unsigned int fragment, unsigned int new_fragment,
	unsigned int required, int *err, int metadata, long *phys, int *new)
{
	struct super_block * sb;
	struct ufs_sb_private_info * uspi;
	struct buffer_head * result;
	unsigned block, blockoff, lastfrag, lastblock, lastblockoff;
	unsigned tmp, goal;
	u32 * p, * p2;
	unsigned int swab;

	UFSD(("ENTER, ino %lu, fragment %u, new_fragment %u, required %u\n",
		inode->i_ino, fragment, new_fragment, required))         

	sb = inode->i_sb;
	swab = sb->u.ufs_sb.s_swab;
	uspi = sb->u.ufs_sb.s_uspi;
	block = ufs_fragstoblks (fragment);
	blockoff = ufs_fragnum (fragment);
	p = inode->u.ufs_i.i_u1.i_data + block;
	goal = 0;

repeat:
	tmp = SWAB32(*p);
	lastfrag = inode->u.ufs_i.i_lastfrag;
	if (tmp && fragment < lastfrag) {
		if (metadata) {
			result = getblk (sb->s_dev, uspi->s_sbbase + tmp + blockoff,
					 sb->s_blocksize);
			if (tmp == SWAB32(*p)) {
				UFSD(("EXIT, result %u\n", tmp + blockoff))
				return result;
			}
			brelse (result);
			goto repeat;
		} else {
			*phys = tmp;
			return NULL;
		}
	}

	lastblock = ufs_fragstoblks (lastfrag);
	lastblockoff = ufs_fragnum (lastfrag);
	/*
	 * We will extend file into new block beyond last allocated block
	 */
	if (lastblock < block) {
		/*
		 * We must reallocate last allocated block
		 */
		if (lastblockoff) {
			p2 = inode->u.ufs_i.i_u1.i_data + lastblock;
			tmp = ufs_new_fragments (inode, p2, lastfrag, 
				SWAB32(*p2), uspi->s_fpb - lastblockoff, err);
			if (!tmp) {
				if (lastfrag != inode->u.ufs_i.i_lastfrag)
					goto repeat;
				else
					return NULL;
			}
			lastfrag = inode->u.ufs_i.i_lastfrag;
			
		}
		goal = SWAB32(inode->u.ufs_i.i_u1.i_data[lastblock]) + uspi->s_fpb;
		tmp = ufs_new_fragments (inode, p, fragment - blockoff, 
			goal, required + blockoff, err);
	}
	/*
	 * We will extend last allocated block
	 */
	else if (lastblock == block) {
		tmp = ufs_new_fragments (inode, p, fragment - (blockoff - lastblockoff),
			SWAB32(*p), required +  (blockoff - lastblockoff), err);
	}
	/*
	 * We will allocate new block before last allocated block
	 */
	else /* (lastblock > block) */ {
		if (lastblock && (tmp = SWAB32(inode->u.ufs_i.i_u1.i_data[lastblock-1])))
			goal = tmp + uspi->s_fpb;
		tmp = ufs_new_fragments (inode, p, fragment - blockoff, 
			goal, uspi->s_fpb, err);
	}
	if (!tmp) {
		if ((!blockoff && SWAB32(*p)) || 
		    (blockoff && lastfrag != inode->u.ufs_i.i_lastfrag))
			goto repeat;
		*err = -ENOSPC;
		return NULL;
	}

	/* The nullification of framgents done in ufs/balloc.c is
	 * something I don't have the stomache to move into here right
	 * now. -DaveM
	 */
	if (metadata) {
		result = getblk (inode->i_dev, tmp + blockoff, sb->s_blocksize);
	} else {
		*phys = tmp;
		result = NULL;
		*err = 0;
		*new = 1;
	}

	inode->i_ctime = CURRENT_TIME;
	if (IS_SYNC(inode))
		ufs_sync_inode (inode);
	mark_inode_dirty(inode);
	UFSD(("EXIT, result %u\n", tmp + blockoff))
	return result;
}

static struct buffer_head * ufs_block_getfrag (struct inode *inode,
	struct buffer_head *bh, unsigned int fragment, unsigned int new_fragment, 
	unsigned int blocksize, int * err, int metadata, long *phys, int *new)
{
	struct super_block * sb;
	struct ufs_sb_private_info * uspi;
	struct buffer_head * result;
	unsigned tmp, goal, block, blockoff;
	u32 * p;
	unsigned int swab;

	sb = inode->i_sb;
	swab = sb->u.ufs_sb.s_swab;
	uspi = sb->u.ufs_sb.s_uspi;
	block = ufs_fragstoblks (fragment);
	blockoff = ufs_fragnum (fragment);

	UFSD(("ENTER, ino %lu, fragment %u, new_fragment %u\n", inode->i_ino, fragment, new_fragment))	

	result = NULL;
	if (!bh)
		goto out;
	if (!buffer_uptodate(bh)) {
		ll_rw_block (READ, 1, &bh);
		wait_on_buffer (bh);
		if (!buffer_uptodate(bh))
			goto out;
	}

	p = (u32 *) bh->b_data + block;
repeat:
	tmp = SWAB32(*p);
	if (tmp) {
		if (metadata) {
			result = getblk (bh->b_dev, uspi->s_sbbase + tmp + blockoff,
					 sb->s_blocksize);
			if (tmp == SWAB32(*p))
				goto out;
			brelse (result);
			goto repeat;
		} else {
			*phys = tmp;
			goto out;
		}
	}

	if (block && (tmp = SWAB32(((u32*)bh->b_data)[block-1]) + uspi->s_fpb))
		goal = tmp + uspi->s_fpb;
	else
		goal = bh->b_blocknr + uspi->s_fpb;
	tmp = ufs_new_fragments (inode, p, ufs_blknum(new_fragment), goal, uspi->s_fpb, err);
	if (!tmp) {
		if (SWAB32(*p))
			goto repeat;
		goto out;
	}		

	/* The nullification of framgents done in ufs/balloc.c is
	 * something I don't have the stomache to move into here right
	 * now. -DaveM
	 */
	if (metadata) {
		result = getblk (bh->b_dev, tmp + blockoff, sb->s_blocksize);
	} else {
		*phys = tmp;
		*new = 1;
	}

	mark_buffer_dirty(bh);
	if (IS_SYNC(inode)) {
		ll_rw_block (WRITE, 1, &bh);
		wait_on_buffer (bh);
	}
	inode->i_ctime = CURRENT_TIME;
	mark_inode_dirty(inode);
out:
	brelse (bh);
	UFSD(("EXIT, result %u\n", tmp + blockoff))
	return result;
}

static int ufs_getfrag_block (struct inode *inode, long fragment, struct buffer_head *bh_result, int create)
{
	struct super_block * sb;
	struct ufs_sb_private_info * uspi;
	struct buffer_head * bh;
	unsigned int swab;
	int ret, err, new;
	unsigned long ptr, phys;
	
	sb = inode->i_sb;
	uspi = sb->u.ufs_sb.s_uspi;
	swab = sb->u.ufs_sb.s_swab;

	if (!create) {
		phys = ufs_frag_map(inode, fragment);
		if (phys) {
			bh_result->b_dev = inode->i_dev;
			bh_result->b_blocknr = phys;
			bh_result->b_state |= (1UL << BH_Mapped);
		}
		return 0;
	}

	err = -EIO;
	new = 0;
	ret = 0;
	bh = NULL;

	lock_kernel();

	UFSD(("ENTER, ino %lu, fragment %u\n", inode->i_ino, fragment))
	if (fragment < 0)
		goto abort_negative;
	if (fragment >
	    ((UFS_NDADDR + uspi->s_apb + uspi->s_2apb + uspi->s_3apb)
	     << uspi->s_fpbshift))
		goto abort_too_big;

	err = 0;
	ptr = fragment;
	  
	/*
	 * ok, these macros clean the logic up a bit and make
	 * it much more readable:
	 */
#define GET_INODE_DATABLOCK(x) \
		ufs_inode_getfrag(inode, x, fragment, 1, &err, 0, &phys, &new)
#define GET_INODE_PTR(x) \
		ufs_inode_getfrag(inode, x, fragment, uspi->s_fpb, &err, 1, NULL, NULL)
#define GET_INDIRECT_DATABLOCK(x) \
		ufs_block_getfrag(inode, bh, x, fragment, sb->s_blocksize, \
				  &err, 0, &phys, &new);
#define GET_INDIRECT_PTR(x) \
		ufs_block_getfrag(inode, bh, x, fragment, sb->s_blocksize, \
				  &err, 1, NULL, NULL);

	if (ptr < UFS_NDIR_FRAGMENT) {
		bh = GET_INODE_DATABLOCK(ptr);
		goto out;
	}
	ptr -= UFS_NDIR_FRAGMENT;
	if (ptr < (1 << (uspi->s_apbshift + uspi->s_fpbshift))) {
		bh = GET_INODE_PTR(UFS_IND_FRAGMENT + (ptr >> uspi->s_apbshift));
		goto get_indirect;
	}
	ptr -= 1 << (uspi->s_apbshift + uspi->s_fpbshift);
	if (ptr < (1 << (uspi->s_2apbshift + uspi->s_fpbshift))) {
		bh = GET_INODE_PTR(UFS_DIND_FRAGMENT + (ptr >> uspi->s_2apbshift));
		goto get_double;
	}
	ptr -= 1 << (uspi->s_2apbshift + uspi->s_fpbshift);
	bh = GET_INODE_PTR(UFS_TIND_FRAGMENT + (ptr >> uspi->s_3apbshift));
	bh = GET_INDIRECT_PTR((ptr >> uspi->s_2apbshift) & uspi->s_apbmask);
get_double:
	bh = GET_INDIRECT_PTR((ptr >> uspi->s_apbshift) & uspi->s_apbmask);
get_indirect:
	bh = GET_INDIRECT_DATABLOCK(ptr & uspi->s_apbmask);

#undef GET_INODE_DATABLOCK
#undef GET_INODE_PTR
#undef GET_INDIRECT_DATABLOCK
#undef GET_INDIRECT_PTR

out:
	if (err)
		goto abort;
	bh_result->b_dev = inode->i_dev;
	bh_result->b_blocknr = phys;
	bh_result->b_state |= (1UL << BH_Mapped);
	if (new)
		bh_result->b_state |= (1UL << BH_New);
abort:
	unlock_kernel();
	return err;

abort_negative:
	ufs_warning(sb, "ufs_get_block", "block < 0");
	goto abort;

abort_too_big:
	ufs_warning(sb, "ufs_get_block", "block > big");
	goto abort;
}

struct buffer_head *ufs_getfrag(struct inode *inode, unsigned int fragment,
				int create, int *err)
{
	struct buffer_head dummy;
	int error;

	dummy.b_state = 0;
	dummy.b_blocknr = -1000;
	error = ufs_getfrag_block(inode, fragment, &dummy, create);
	*err = error;
	if (!error && buffer_mapped(&dummy)) {
		struct buffer_head *bh;
		bh = getblk(dummy.b_dev, dummy.b_blocknr, inode->i_sb->s_blocksize);
		if (buffer_new(&dummy)) {
			memset(bh->b_data, 0, inode->i_sb->s_blocksize);
			mark_buffer_uptodate(bh, 1);
			mark_buffer_dirty(bh);
		}
		return bh;
	}
	return NULL;
}

struct buffer_head * ufs_bread (struct inode * inode, unsigned fragment,
	int create, int * err)
{
	struct buffer_head * bh;

	UFSD(("ENTER, ino %lu, fragment %u\n", inode->i_ino, fragment))
	bh = ufs_getfrag (inode, fragment, create, err);
	if (!bh || buffer_uptodate(bh)) 		
		return bh;
	ll_rw_block (READ, 1, &bh);
	wait_on_buffer (bh);
	if (buffer_uptodate(bh))
		return bh;
	brelse (bh);
	*err = -EIO;
	return NULL;
}

static int ufs_writepage(struct page *page)
{
	return block_write_full_page(page,ufs_getfrag_block);
}
static int ufs_readpage(struct file *file, struct page *page)
{
	return block_read_full_page(page,ufs_getfrag_block);
}
static int ufs_prepare_write(struct file *file, struct page *page, unsigned from, unsigned to)
{
	return block_prepare_write(page,from,to,ufs_getfrag_block);
}
static int ufs_bmap(struct address_space *mapping, long block)
{
	return generic_block_bmap(mapping,block,ufs_getfrag_block);
}
struct address_space_operations ufs_aops = {
	readpage: ufs_readpage,
	writepage: ufs_writepage,
	sync_page: block_sync_page,
	prepare_write: ufs_prepare_write,
	commit_write: generic_commit_write,
	bmap: ufs_bmap
};

void ufs_read_inode (struct inode * inode)
{
	struct super_block * sb;
	struct ufs_sb_private_info * uspi;
	struct ufs_inode * ufs_inode;	
	struct buffer_head * bh;
	unsigned i;
	unsigned flags, swab;
	
	UFSD(("ENTER, ino %lu\n", inode->i_ino))
	
	sb = inode->i_sb;
	uspi = sb->u.ufs_sb.s_uspi;
	flags = sb->u.ufs_sb.s_flags;
	swab = sb->u.ufs_sb.s_swab;

	if (inode->i_ino < UFS_ROOTINO || 
	    inode->i_ino > (uspi->s_ncg * uspi->s_ipg)) {
		ufs_warning (sb, "ufs_read_inode", "bad inode number (%lu)\n", inode->i_ino);
		return;
	}
	
	bh = bread (sb->s_dev, uspi->s_sbbase + ufs_inotofsba(inode->i_ino), sb->s_blocksize);
	if (!bh) {
		ufs_warning (sb, "ufs_read_inode", "unable to read inode %lu\n", inode->i_ino);
		return;
	}
	ufs_inode = (struct ufs_inode *) (bh->b_data + sizeof(struct ufs_inode) * ufs_inotofsbo(inode->i_ino));

	/*
	 * Copy data to the in-core inode.
	 */
	inode->i_mode = SWAB16(ufs_inode->ui_mode);
	inode->i_nlink = SWAB16(ufs_inode->ui_nlink);
	if (inode->i_nlink == 0)
		ufs_error (sb, "ufs_read_inode", "inode %lu has zero nlink\n", inode->i_ino);
	
	/*
	 * Linux now has 32-bit uid and gid, so we can support EFT.
	 */
	inode->i_uid = ufs_get_inode_uid(ufs_inode);
	inode->i_gid = ufs_get_inode_gid(ufs_inode);
	
	/*
	 * Linux i_size can be 32 on some architectures. We will mark 
	 * big files as read only and let user access first 32 bits.
	 */
	inode->u.ufs_i.i_size = SWAB64(ufs_inode->ui_size);
	inode->i_size = (off_t) inode->u.ufs_i.i_size;
	if (sizeof(off_t) == 4 && (inode->u.ufs_i.i_size >> 32))
		inode->i_size = (__u32)-1;

	inode->i_atime = SWAB32(ufs_inode->ui_atime.tv_sec);
	inode->i_ctime = SWAB32(ufs_inode->ui_ctime.tv_sec);
	inode->i_mtime = SWAB32(ufs_inode->ui_mtime.tv_sec);
	inode->i_blocks = SWAB32(ufs_inode->ui_blocks);
	inode->i_blksize = PAGE_SIZE;   /* This is the optimal IO size (for stat) */
	inode->i_version = ++event;

	inode->u.ufs_i.i_flags = SWAB32(ufs_inode->ui_flags);
	inode->u.ufs_i.i_gen = SWAB32(ufs_inode->ui_gen);
	inode->u.ufs_i.i_shadow = SWAB32(ufs_inode->ui_u3.ui_sun.ui_shadow);
	inode->u.ufs_i.i_oeftflag = SWAB32(ufs_inode->ui_u3.ui_sun.ui_oeftflag);
	inode->u.ufs_i.i_lastfrag = (inode->i_size + uspi->s_fsize - 1) >> uspi->s_fshift;
	
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		;
	else if (inode->i_blocks) {
		for (i = 0; i < (UFS_NDADDR + UFS_NINDIR); i++)
			inode->u.ufs_i.i_u1.i_data[i] = ufs_inode->ui_u2.ui_addr.ui_db[i];
	}
	else {
		for (i = 0; i < (UFS_NDADDR + UFS_NINDIR) * 4; i++)
			inode->u.ufs_i.i_u1.i_symlink[i] = ufs_inode->ui_u2.ui_symlink[i];
	}


	if (S_ISREG(inode->i_mode)) {
		inode->i_op = &ufs_file_inode_operations;
		inode->i_fop = &ufs_file_operations;
		inode->i_mapping->a_ops = &ufs_aops;
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &ufs_dir_inode_operations;
		inode->i_fop = &ufs_dir_operations;
	} else if (S_ISLNK(inode->i_mode)) {
		if (!inode->i_blocks)
			inode->i_op = &ufs_fast_symlink_inode_operations;
		else {
			inode->i_op = &page_symlink_inode_operations;
			inode->i_mapping->a_ops = &ufs_aops;
		}
	} else
		init_special_inode(inode, inode->i_mode,
				   SWAB32(ufs_inode->ui_u2.ui_addr.ui_db[0]));

	brelse (bh);

#ifdef UFS_INODE_DEBUG_MORE
	ufs_print_inode (inode);
#endif
	UFSD(("EXIT\n"))
}

static int ufs_update_inode(struct inode * inode, int do_sync)
{
	struct super_block * sb;
	struct ufs_sb_private_info * uspi;
	struct buffer_head * bh;
	struct ufs_inode * ufs_inode;
	unsigned i;
	unsigned flags, swab;

	UFSD(("ENTER, ino %lu\n", inode->i_ino))

	sb = inode->i_sb;
	uspi = sb->u.ufs_sb.s_uspi;
	flags = sb->u.ufs_sb.s_flags;
	swab = sb->u.ufs_sb.s_swab;

	if (inode->i_ino < UFS_ROOTINO || 
	    inode->i_ino > (uspi->s_ncg * uspi->s_ipg)) {
		ufs_warning (sb, "ufs_read_inode", "bad inode number (%lu)\n", inode->i_ino);
		return -1;
	}

	bh = bread (sb->s_dev, ufs_inotofsba(inode->i_ino), sb->s_blocksize);
	if (!bh) {
		ufs_warning (sb, "ufs_read_inode", "unable to read inode %lu\n", inode->i_ino);
		return -1;
	}
	ufs_inode = (struct ufs_inode *) (bh->b_data + ufs_inotofsbo(inode->i_ino) * sizeof(struct ufs_inode));

	ufs_inode->ui_mode = SWAB16(inode->i_mode);
	ufs_inode->ui_nlink = SWAB16(inode->i_nlink);

	ufs_set_inode_uid (ufs_inode, inode->i_uid);
	ufs_set_inode_gid (ufs_inode, inode->i_gid);
		
	ufs_inode->ui_size = SWAB64((u64)inode->i_size);
	ufs_inode->ui_atime.tv_sec = SWAB32(inode->i_atime);
	ufs_inode->ui_atime.tv_usec = SWAB32(0);
	ufs_inode->ui_ctime.tv_sec = SWAB32(inode->i_ctime);
	ufs_inode->ui_ctime.tv_usec = SWAB32(0);
	ufs_inode->ui_mtime.tv_sec = SWAB32(inode->i_mtime);
	ufs_inode->ui_mtime.tv_usec = SWAB32(0);
	ufs_inode->ui_blocks = SWAB32(inode->i_blocks);
	ufs_inode->ui_flags = SWAB32(inode->u.ufs_i.i_flags);
	ufs_inode->ui_gen = SWAB32(inode->u.ufs_i.i_gen);

	if ((flags & UFS_UID_MASK) == UFS_UID_EFT) {
		ufs_inode->ui_u3.ui_sun.ui_shadow = SWAB32(inode->u.ufs_i.i_shadow);
		ufs_inode->ui_u3.ui_sun.ui_oeftflag = SWAB32(inode->u.ufs_i.i_oeftflag);
	}

	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode))
		ufs_inode->ui_u2.ui_addr.ui_db[0] = SWAB32(kdev_t_to_nr(inode->i_rdev));
	else if (inode->i_blocks) {
		for (i = 0; i < (UFS_NDADDR + UFS_NINDIR); i++)
			ufs_inode->ui_u2.ui_addr.ui_db[i] = inode->u.ufs_i.i_u1.i_data[i];
	}
	else {
		for (i = 0; i < (UFS_NDADDR + UFS_NINDIR) * 4; i++)
			ufs_inode->ui_u2.ui_symlink[i] = inode->u.ufs_i.i_u1.i_symlink[i];
	}

	if (!inode->i_nlink)
		memset (ufs_inode, 0, sizeof(struct ufs_inode));
		
	mark_buffer_dirty(bh);
	if (do_sync) {
		ll_rw_block (WRITE, 1, &bh);
		wait_on_buffer (bh);
	}
	brelse (bh);
	
	UFSD(("EXIT\n"))
	return 0;
}

void ufs_write_inode (struct inode * inode, int wait)
{
	lock_kernel();
	ufs_update_inode (inode, wait);
	unlock_kernel();
}

int ufs_sync_inode (struct inode *inode)
{
	return ufs_update_inode (inode, 1);
}

void ufs_delete_inode (struct inode * inode)
{
	/*inode->u.ufs_i.i_dtime = CURRENT_TIME;*/
	lock_kernel();
	mark_inode_dirty(inode);
	ufs_update_inode(inode, IS_SYNC(inode));
	inode->i_size = 0;
	if (inode->i_blocks)
		ufs_truncate (inode);
	ufs_free_inode (inode);
	unlock_kernel();
}
