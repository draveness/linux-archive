/*
 * QNX4 file system, Linux implementation.
 *
 * Version : 0.2.1
 *
 * Using parts of the xiafs filesystem.
 *
 * History :
 *
 * 28-05-1998 by Richard Frowijn : first release.
 * 20-06-1998 by Frank Denis : Linux 2.1.99+ & dcache support.
 */

#include <linux/config.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/qnx4_fs.h>
#include <linux/stat.h>

#include <asm/segment.h>

static int qnx4_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct inode *inode = filp->f_dentry->d_inode;
	unsigned int offset;
	struct buffer_head *bh;
	struct qnx4_inode_entry *de;
	struct qnx4_link_info *le;
	unsigned long blknum;
	int ix, ino;
	int size;

	QNX4DEBUG(("qnx4_readdir:i_size = %ld\n", (long) inode->i_size));
	QNX4DEBUG(("filp->f_pos         = %ld\n", (long) filp->f_pos));

	while (filp->f_pos < inode->i_size) {
		blknum = qnx4_block_map( inode, filp->f_pos >> QNX4_BLOCK_SIZE_BITS );
		bh = bread(inode->i_dev, blknum, QNX4_BLOCK_SIZE);
		if(bh==NULL) {
			printk(KERN_ERR "qnx4_readdir: bread failed (%ld)\n", blknum);
			break;
		}
		ix = (int)(filp->f_pos >> QNX4_DIR_ENTRY_SIZE_BITS) % QNX4_INODES_PER_BLOCK;
		while (ix < QNX4_INODES_PER_BLOCK) {
			offset = ix * QNX4_DIR_ENTRY_SIZE;
			de = (struct qnx4_inode_entry *) (bh->b_data + offset);
			size = strlen(de->di_fname);
			if (size) {
				if ( !( de->di_status & QNX4_FILE_LINK ) && size > QNX4_SHORT_NAME_MAX )
					size = QNX4_SHORT_NAME_MAX;
				else if ( size > QNX4_NAME_MAX )
					size = QNX4_NAME_MAX;

				if ( ( de->di_status & (QNX4_FILE_USED|QNX4_FILE_LINK) ) != 0 ) {
					QNX4DEBUG(("qnx4_readdir:%.*s\n", size, de->di_fname));
					if ( ( de->di_status & QNX4_FILE_LINK ) == 0 )
						ino = blknum * QNX4_INODES_PER_BLOCK + ix - 1;
					else {
						le  = (struct qnx4_link_info*)de;
						ino = ( le->dl_inode_blk - 1 ) *
							QNX4_INODES_PER_BLOCK +
							le->dl_inode_ndx;
					}
					if (filldir(dirent, de->di_fname, size, filp->f_pos, ino, DT_UNKNOWN) < 0) {
						brelse(bh);
						return 0;
					}
				}
			}
			ix++;
			filp->f_pos += QNX4_DIR_ENTRY_SIZE;
		}
		brelse(bh);
	}
	UPDATE_ATIME(inode);

	return 0;
}

struct file_operations qnx4_dir_operations =
{
	read:		generic_read_dir,
	readdir:	qnx4_readdir,
	fsync:		file_fsync,
};

struct inode_operations qnx4_dir_inode_operations =
{
	lookup:		qnx4_lookup,
#ifdef CONFIG_QNX4FS_RW
	create:		qnx4_create,
	unlink:		qnx4_unlink,
	rmdir:		qnx4_rmdir,
#endif
};
