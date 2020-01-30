#include <linux/sched.h>		/* for capable() */
#include <linux/blkdev.h>
#include <linux/blkpg.h>
#include <linux/backing-dev.h>
#include <linux/buffer_head.h>
#include <asm/uaccess.h>

static int blkpg_ioctl(struct block_device *bdev, struct blkpg_ioctl_arg __user *arg)
{
	struct block_device *bdevp;
	struct gendisk *disk;
	struct blkpg_ioctl_arg a;
	struct blkpg_partition p;
	long long start, length;
	int part;
	int i;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	if (copy_from_user(&a, arg, sizeof(struct blkpg_ioctl_arg)))
		return -EFAULT;
	if (copy_from_user(&p, a.data, sizeof(struct blkpg_partition)))
		return -EFAULT;
	disk = bdev->bd_disk;
	if (bdev != bdev->bd_contains)
		return -EINVAL;
	part = p.pno;
	if (part <= 0 || part >= disk->minors)
		return -EINVAL;
	switch (a.op) {
		case BLKPG_ADD_PARTITION:
			start = p.start >> 9;
			length = p.length >> 9;
			/* check for fit in a hd_struct */ 
			if (sizeof(sector_t) == sizeof(long) && 
			    sizeof(long long) > sizeof(long)) {
				long pstart = start, plength = length;
				if (pstart != start || plength != length
				    || pstart < 0 || plength < 0)
					return -EINVAL;
			}
			/* partition number in use? */
			down(&bdev->bd_sem);
			if (disk->part[part - 1]) {
				up(&bdev->bd_sem);
				return -EBUSY;
			}
			/* overlap? */
			for (i = 0; i < disk->minors - 1; i++) {
				struct hd_struct *s = disk->part[i];

				if (!s)
					continue;
				if (!(start+length <= s->start_sect ||
				      start >= s->start_sect + s->nr_sects)) {
					up(&bdev->bd_sem);
					return -EBUSY;
				}
			}
			/* all seems OK */
			add_partition(disk, part, start, length);
			up(&bdev->bd_sem);
			return 0;
		case BLKPG_DEL_PARTITION:
			if (!disk->part[part-1])
				return -ENXIO;
			if (disk->part[part - 1]->nr_sects == 0)
				return -ENXIO;
			bdevp = bdget_disk(disk, part);
			if (!bdevp)
				return -ENOMEM;
			down(&bdevp->bd_sem);
			if (bdevp->bd_openers) {
				up(&bdevp->bd_sem);
				bdput(bdevp);
				return -EBUSY;
			}
			/* all seems OK */
			fsync_bdev(bdevp);
			invalidate_bdev(bdevp, 0);

			down(&bdev->bd_sem);
			delete_partition(disk, part);
			up(&bdev->bd_sem);
			up(&bdevp->bd_sem);
			bdput(bdevp);

			return 0;
		default:
			return -EINVAL;
	}
}

static int blkdev_reread_part(struct block_device *bdev)
{
	struct gendisk *disk = bdev->bd_disk;
	int res;

	if (disk->minors == 1 || bdev != bdev->bd_contains)
		return -EINVAL;
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;
	if (down_trylock(&bdev->bd_sem))
		return -EBUSY;
	res = rescan_partitions(disk, bdev);
	up(&bdev->bd_sem);
	return res;
}

static int put_ushort(unsigned long arg, unsigned short val)
{
	return put_user(val, (unsigned short __user *)arg);
}

static int put_int(unsigned long arg, int val)
{
	return put_user(val, (int __user *)arg);
}

static int put_long(unsigned long arg, long val)
{
	return put_user(val, (long __user *)arg);
}

static int put_ulong(unsigned long arg, unsigned long val)
{
	return put_user(val, (unsigned long __user *)arg);
}

static int put_u64(unsigned long arg, u64 val)
{
	return put_user(val, (u64 __user *)arg);
}

int blkdev_ioctl(struct inode *inode, struct file *file, unsigned cmd,
			unsigned long arg)
{
	struct block_device *bdev = inode->i_bdev;
	struct gendisk *disk = bdev->bd_disk;
	struct backing_dev_info *bdi;
	int ret, n;

	switch (cmd) {
	case BLKRAGET:
	case BLKFRAGET:
		if (!arg)
			return -EINVAL;
		bdi = blk_get_backing_dev_info(bdev);
		if (bdi == NULL)
			return -ENOTTY;
		return put_long(arg, (bdi->ra_pages * PAGE_CACHE_SIZE) / 512);
	case BLKROGET:
		return put_int(arg, bdev_read_only(bdev) != 0);
	case BLKBSZGET: /* get the logical block size (cf. BLKSSZGET) */
		return put_int(arg, block_size(bdev));
	case BLKSSZGET: /* get block device hardware sector size */
		return put_int(arg, bdev_hardsect_size(bdev));
	case BLKSECTGET:
		return put_ushort(arg, bdev_get_queue(bdev)->max_sectors);
	case BLKRASET:
	case BLKFRASET:
		if(!capable(CAP_SYS_ADMIN))
			return -EACCES;
		bdi = blk_get_backing_dev_info(bdev);
		if (bdi == NULL)
			return -ENOTTY;
		bdi->ra_pages = (arg * 512) / PAGE_CACHE_SIZE;
		return 0;
	case BLKBSZSET:
		/* set the logical block size */
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		if (!arg)
			return -EINVAL;
		if (get_user(n, (int __user *) arg))
			return -EFAULT;
		if (bd_claim(bdev, file) < 0)
			return -EBUSY;
		ret = set_blocksize(bdev, n);
		bd_release(bdev);
		return ret;
	case BLKPG:
		return blkpg_ioctl(bdev, (struct blkpg_ioctl_arg __user *) arg);
	case BLKRRPART:
		return blkdev_reread_part(bdev);
	case BLKGETSIZE:
		if ((bdev->bd_inode->i_size >> 9) > ~0UL)
			return -EFBIG;
		return put_ulong(arg, bdev->bd_inode->i_size >> 9);
	case BLKGETSIZE64:
		return put_u64(arg, bdev->bd_inode->i_size);
	case BLKFLSBUF:
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		if (disk->fops->ioctl) {
			ret = disk->fops->ioctl(inode, file, cmd, arg);
			if (ret != -EINVAL)
				return ret;
		}
		fsync_bdev(bdev);
		invalidate_bdev(bdev, 0);
		return 0;
	case BLKROSET:
		if (disk->fops->ioctl) {
			ret = disk->fops->ioctl(inode, file, cmd, arg);
			/* -EINVAL to handle old uncorrected drivers */
			if (ret != -EINVAL && ret != -ENOTTY)
				return ret;
		}
		if (!capable(CAP_SYS_ADMIN))
			return -EACCES;
		if (get_user(n, (int __user *)(arg)))
			return -EFAULT;
		set_device_ro(bdev, n);
		return 0;
	default:
		if (disk->fops->ioctl)
			return disk->fops->ioctl(inode, file, cmd, arg);
	}
	return -ENOTTY;
}
