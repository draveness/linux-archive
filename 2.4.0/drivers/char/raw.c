/*
 * linux/drivers/char/raw.c
 *
 * Front-end raw character devices.  These can be bound to any block
 * devices to provide genuine Unix raw character device semantics.
 *
 * We reserve minor number 0 for a control interface.  ioctl()s on this
 * device are used to bind the other minor numbers to block devices.
 */

#include <linux/fs.h>
#include <linux/iobuf.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/raw.h>
#include <linux/capability.h>
#include <linux/smp_lock.h>
#include <asm/uaccess.h>

#define dprintk(x...) 

static struct block_device *raw_device_bindings[256];
static int raw_device_inuse[256];
static int raw_device_sector_size[256];
static int raw_device_sector_bits[256];

static ssize_t rw_raw_dev(int rw, struct file *, char *, size_t, loff_t *);

ssize_t	raw_read(struct file *, char *, size_t, loff_t *);
ssize_t	raw_write(struct file *, const char *, size_t, loff_t *);
int	raw_open(struct inode *, struct file *);
int	raw_release(struct inode *, struct file *);
int	raw_ctl_ioctl(struct inode *, struct file *, unsigned int, unsigned long);


static struct file_operations raw_fops = {
	read:		raw_read,
	write:		raw_write,
	open:		raw_open,
	release:	raw_release,
};

static struct file_operations raw_ctl_fops = {
	ioctl:		raw_ctl_ioctl,
	open:		raw_open,
};

void __init raw_init(void)
{
	register_chrdev(RAW_MAJOR, "raw", &raw_fops);
}

/* 
 * Open/close code for raw IO.
 */

int raw_open(struct inode *inode, struct file *filp)
{
	int minor;
	struct block_device * bdev;
	kdev_t rdev;	/* it should eventually go away */
	int err;
	int sector_size;
	int sector_bits;

	minor = MINOR(inode->i_rdev);
	
	/* 
	 * Is it the control device? 
	 */
	
	if (minor == 0) {
		filp->f_op = &raw_ctl_fops;
		return 0;
	}
	
	/*
	 * No, it is a normal raw device.  All we need to do on open is
	 * to check that the device is bound, and force the underlying
	 * block device to a sector-size blocksize. 
	 */

	bdev = raw_device_bindings[minor];
	if (!bdev)
		return -ENODEV;

	rdev = to_kdev_t(bdev->bd_dev);
	err = blkdev_get(bdev, filp->f_mode, 0, BDEV_RAW);
	if (err)
		return err;
	
	/*
	 * Don't change the blocksize if we already have users using
	 * this device 
	 */

	if (raw_device_inuse[minor]++)
		return 0;
	
	/* 
	 * Don't interfere with mounted devices: we cannot safely set
	 * the blocksize on a device which is already mounted.  
	 */
	
	sector_size = 512;
	if (get_super(rdev) != NULL) {
		if (blksize_size[MAJOR(rdev)])
			sector_size = blksize_size[MAJOR(rdev)][MINOR(rdev)];
	} else {
		if (hardsect_size[MAJOR(rdev)])
			sector_size = hardsect_size[MAJOR(rdev)][MINOR(rdev)];
	}

	set_blocksize(rdev, sector_size);
	raw_device_sector_size[minor] = sector_size;

	for (sector_bits = 0; !(sector_size & 1); )
		sector_size>>=1, sector_bits++;
	raw_device_sector_bits[minor] = sector_bits;
	
	return 0;
}

int raw_release(struct inode *inode, struct file *filp)
{
	int minor;
	struct block_device *bdev;
	
	minor = MINOR(inode->i_rdev);
	lock_kernel();
	bdev = raw_device_bindings[minor];
	blkdev_put(bdev, BDEV_RAW);
	raw_device_inuse[minor]--;
	unlock_kernel();
	return 0;
}



/*
 * Deal with ioctls against the raw-device control interface, to bind
 * and unbind other raw devices.  
 */

int raw_ctl_ioctl(struct inode *inode, 
		  struct file *flip,
		  unsigned int command, 
		  unsigned long arg)
{
	struct raw_config_request rq;
	int err = 0;
	int minor;
	
	switch (command) {
	case RAW_SETBIND:
	case RAW_GETBIND:

		/* First, find out which raw minor we want */

		err = copy_from_user(&rq, (void *) arg, sizeof(rq));
		if (err)
			break;
		
		minor = rq.raw_minor;
		if (minor == 0 || minor > MINORMASK) {
			err = -EINVAL;
			break;
		}

		if (command == RAW_SETBIND) {
			/*
			 * This is like making block devices, so demand the
			 * same capability
			 */
			if (!capable(CAP_SYS_ADMIN)) {
				err = -EPERM;
				break;
			}

			/* 
			 * For now, we don't need to check that the underlying
			 * block device is present or not: we can do that when
			 * the raw device is opened.  Just check that the
			 * major/minor numbers make sense. 
			 */

			if (rq.block_major == NODEV || 
			    rq.block_major > MAX_BLKDEV ||
			    rq.block_minor > MINORMASK) {
				err = -EINVAL;
				break;
			}
			
			if (raw_device_inuse[minor]) {
				err = -EBUSY;
				break;
			}
			if (raw_device_bindings[minor])
				bdput(raw_device_bindings[minor]);
			raw_device_bindings[minor] = 
				bdget(kdev_t_to_nr(MKDEV(rq.block_major, rq.block_minor)));
		} else {
			struct block_device *bdev;
			kdev_t dev;

			bdev = raw_device_bindings[minor];
			if (bdev) {
				dev = to_kdev_t(bdev->bd_dev);
				rq.block_major = MAJOR(dev);
				rq.block_minor = MINOR(dev);
			} else {
				rq.block_major = rq.block_minor = 0;
			}
			err = copy_to_user((void *) arg, &rq, sizeof(rq));
		}
		break;
		
	default:
		err = -EINVAL;
	}
	
	return err;
}



ssize_t	raw_read(struct file *filp, char * buf, 
		 size_t size, loff_t *offp)
{
	return rw_raw_dev(READ, filp, buf, size, offp);
}

ssize_t	raw_write(struct file *filp, const char *buf, 
		  size_t size, loff_t *offp)
{
	return rw_raw_dev(WRITE, filp, (char *) buf, size, offp);
}

#define SECTOR_BITS 9
#define SECTOR_SIZE (1U << SECTOR_BITS)
#define SECTOR_MASK (SECTOR_SIZE - 1)

ssize_t	rw_raw_dev(int rw, struct file *filp, char *buf, 
		   size_t size, loff_t *offp)
{
	struct kiobuf * iobuf;
	int		err;
	unsigned long	blocknr, blocks;
	unsigned long	b[KIO_MAX_SECTORS];
	size_t		transferred;
	int		iosize;
	int		i;
	int		minor;
	kdev_t		dev;
	unsigned long	limit;

	int		sector_size, sector_bits, sector_mask;
	int		max_sectors;
	
	/*
	 * First, a few checks on device size limits 
	 */

	minor = MINOR(filp->f_dentry->d_inode->i_rdev);
	dev = to_kdev_t(raw_device_bindings[minor]->bd_dev);
	sector_size = raw_device_sector_size[minor];
	sector_bits = raw_device_sector_bits[minor];
	sector_mask = sector_size- 1;
	max_sectors = KIO_MAX_SECTORS >> (sector_bits - 9);
	
	if (blk_size[MAJOR(dev)])
		limit = (((loff_t) blk_size[MAJOR(dev)][MINOR(dev)]) << BLOCK_SIZE_BITS) >> sector_bits;
	else
		limit = INT_MAX;
	dprintk ("rw_raw_dev: dev %d:%d (+%d)\n",
		 MAJOR(dev), MINOR(dev), limit);
	
	if ((*offp & sector_mask) || (size & sector_mask))
		return -EINVAL;
	if ((*offp >> sector_bits) > limit)
		return 0;

	/* 
	 * We'll just use one kiobuf
	 */

	err = alloc_kiovec(1, &iobuf);
	if (err)
		return err;

	/*
	 * Split the IO into KIO_MAX_SECTORS chunks, mapping and
	 * unmapping the single kiobuf as we go to perform each chunk of
	 * IO.  
	 */

	transferred = 0;
	blocknr = *offp >> sector_bits;
	while (size > 0) {
		blocks = size >> sector_bits;
		if (blocks > max_sectors)
			blocks = max_sectors;
		if (blocks > limit - blocknr)
			blocks = limit - blocknr;
		if (!blocks)
			break;

		iosize = blocks << sector_bits;

		err = map_user_kiobuf(rw, iobuf, (unsigned long) buf, iosize);
		if (err)
			break;
#if 0
		err = lock_kiovec(1, &iobuf, 1);
		if (err) 
			break;
#endif
	
		for (i=0; i < blocks; i++) 
			b[i] = blocknr++;
		
		err = brw_kiovec(rw, 1, &iobuf, dev, b, sector_size);

		if (err >= 0) {
			transferred += err;
			size -= err;
			buf += err;
		}

		unmap_kiobuf(iobuf); /* The unlock_kiobuf is implicit here */

		if (err != iosize)
			break;
	}
	
	free_kiovec(1, &iobuf);

	if (transferred) {
		*offp += transferred;
		return transferred;
	}
	
	return err;
}
