/*
 *      sd.c Copyright (C) 1992 Drew Eckhardt
 *           Copyright (C) 1993, 1994, 1995, 1999 Eric Youngdale
 *
 *      Linux scsi disk driver
 *              Initial versions: Drew Eckhardt
 *              Subsequent revisions: Eric Youngdale
 *	Modification history:
 *       - Drew Eckhardt <drew@colorado.edu> original
 *       - Eric Youngdale <eric@andante.org> add scatter-gather, multiple 
 *         outstanding request, and other enhancements.
 *         Support loadable low-level scsi drivers.
 *       - Jirka Hanika <geo@ff.cuni.cz> support more scsi disks using 
 *         eight major numbers.
 *       - Richard Gooch <rgooch@atnf.csiro.au> support devfs.
 *	 - Torben Mathiasen <tmm@image.dk> Resource allocation fixes in 
 *	   sd_init and cleanups.
 *	 - Alex Davis <letmein@erols.com> Fix problem where partition info
 *	   not being read in sd_open. Fix problem where removable media 
 *	   could be ejected after sd_open.
 *	 - Douglas Gilbert <dgilbert@interlog.com> cleanup for lk 2.5.x
 *	 - Badari Pulavarty <pbadari@us.ibm.com>, Matthew Wilcox 
 *	   <willy@debian.org>, Kurt Garloff <garloff@suse.de>: 
 *	   Support 32k/1M disks.
 *
 *	Logging policy (needs CONFIG_SCSI_LOGGING defined):
 *	 - setting up transfer: SCSI_LOG_HLQUEUE levels 1 and 2
 *	 - end of transfer (bh + scsi_lib): SCSI_LOG_HLCOMPLETE level 1
 *	 - entering sd_ioctl: SCSI_LOG_IOCTL level 1
 *	 - entering other commands: SCSI_LOG_HLQUEUE level 3
 *	Note: when the logging level is set by the user, it must be greater
 *	than the level indicated above to trigger output.	
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/bio.h>
#include <linux/genhd.h>
#include <linux/hdreg.h>
#include <linux/errno.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/blkpg.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <asm/uaccess.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_driver.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_ioctl.h>
#include <scsi/scsicam.h>
#include <scsi/sd.h>

#include "scsi_logging.h"

MODULE_AUTHOR("Eric Youngdale");
MODULE_DESCRIPTION("SCSI disk (sd) driver");
MODULE_LICENSE("GPL");

MODULE_ALIAS_BLOCKDEV_MAJOR(SCSI_DISK0_MAJOR);
MODULE_ALIAS_BLOCKDEV_MAJOR(SCSI_DISK1_MAJOR);
MODULE_ALIAS_BLOCKDEV_MAJOR(SCSI_DISK2_MAJOR);
MODULE_ALIAS_BLOCKDEV_MAJOR(SCSI_DISK3_MAJOR);
MODULE_ALIAS_BLOCKDEV_MAJOR(SCSI_DISK4_MAJOR);
MODULE_ALIAS_BLOCKDEV_MAJOR(SCSI_DISK5_MAJOR);
MODULE_ALIAS_BLOCKDEV_MAJOR(SCSI_DISK6_MAJOR);
MODULE_ALIAS_BLOCKDEV_MAJOR(SCSI_DISK7_MAJOR);
MODULE_ALIAS_BLOCKDEV_MAJOR(SCSI_DISK8_MAJOR);
MODULE_ALIAS_BLOCKDEV_MAJOR(SCSI_DISK9_MAJOR);
MODULE_ALIAS_BLOCKDEV_MAJOR(SCSI_DISK10_MAJOR);
MODULE_ALIAS_BLOCKDEV_MAJOR(SCSI_DISK11_MAJOR);
MODULE_ALIAS_BLOCKDEV_MAJOR(SCSI_DISK12_MAJOR);
MODULE_ALIAS_BLOCKDEV_MAJOR(SCSI_DISK13_MAJOR);
MODULE_ALIAS_BLOCKDEV_MAJOR(SCSI_DISK14_MAJOR);
MODULE_ALIAS_BLOCKDEV_MAJOR(SCSI_DISK15_MAJOR);
MODULE_ALIAS_SCSI_DEVICE(TYPE_DISK);
MODULE_ALIAS_SCSI_DEVICE(TYPE_MOD);
MODULE_ALIAS_SCSI_DEVICE(TYPE_RBC);

static DEFINE_IDR(sd_index_idr);
static DEFINE_SPINLOCK(sd_index_lock);

/* This semaphore is used to mediate the 0->1 reference get in the
 * face of object destruction (i.e. we can't allow a get on an
 * object after last put) */
static DEFINE_MUTEX(sd_ref_mutex);

static const char *sd_cache_types[] = {
	"write through", "none", "write back",
	"write back, no read (daft)"
};

static ssize_t sd_store_cache_type(struct class_device *cdev, const char *buf,
				   size_t count)
{
	int i, ct = -1, rcd, wce, sp;
	struct scsi_disk *sdkp = to_scsi_disk(cdev);
	struct scsi_device *sdp = sdkp->device;
	char buffer[64];
	char *buffer_data;
	struct scsi_mode_data data;
	struct scsi_sense_hdr sshdr;
	int len;

	if (sdp->type != TYPE_DISK)
		/* no cache control on RBC devices; theoretically they
		 * can do it, but there's probably so many exceptions
		 * it's not worth the risk */
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(sd_cache_types); i++) {
		const int len = strlen(sd_cache_types[i]);
		if (strncmp(sd_cache_types[i], buf, len) == 0 &&
		    buf[len] == '\n') {
			ct = i;
			break;
		}
	}
	if (ct < 0)
		return -EINVAL;
	rcd = ct & 0x01 ? 1 : 0;
	wce = ct & 0x02 ? 1 : 0;
	if (scsi_mode_sense(sdp, 0x08, 8, buffer, sizeof(buffer), SD_TIMEOUT,
			    SD_MAX_RETRIES, &data, NULL))
		return -EINVAL;
	len = min_t(size_t, sizeof(buffer), data.length - data.header_length -
		  data.block_descriptor_length);
	buffer_data = buffer + data.header_length +
		data.block_descriptor_length;
	buffer_data[2] &= ~0x05;
	buffer_data[2] |= wce << 2 | rcd;
	sp = buffer_data[0] & 0x80 ? 1 : 0;

	if (scsi_mode_select(sdp, 1, sp, 8, buffer_data, len, SD_TIMEOUT,
			     SD_MAX_RETRIES, &data, &sshdr)) {
		if (scsi_sense_valid(&sshdr))
			sd_print_sense_hdr(sdkp, &sshdr);
		return -EINVAL;
	}
	sd_revalidate_disk(sdkp->disk);
	return count;
}

static ssize_t sd_store_manage_start_stop(struct class_device *cdev,
					  const char *buf, size_t count)
{
	struct scsi_disk *sdkp = to_scsi_disk(cdev);
	struct scsi_device *sdp = sdkp->device;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	sdp->manage_start_stop = simple_strtoul(buf, NULL, 10);

	return count;
}

static ssize_t sd_store_allow_restart(struct class_device *cdev, const char *buf,
				      size_t count)
{
	struct scsi_disk *sdkp = to_scsi_disk(cdev);
	struct scsi_device *sdp = sdkp->device;

	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	if (sdp->type != TYPE_DISK)
		return -EINVAL;

	sdp->allow_restart = simple_strtoul(buf, NULL, 10);

	return count;
}

static ssize_t sd_show_cache_type(struct class_device *cdev, char *buf)
{
	struct scsi_disk *sdkp = to_scsi_disk(cdev);
	int ct = sdkp->RCD + 2*sdkp->WCE;

	return snprintf(buf, 40, "%s\n", sd_cache_types[ct]);
}

static ssize_t sd_show_fua(struct class_device *cdev, char *buf)
{
	struct scsi_disk *sdkp = to_scsi_disk(cdev);

	return snprintf(buf, 20, "%u\n", sdkp->DPOFUA);
}

static ssize_t sd_show_manage_start_stop(struct class_device *cdev, char *buf)
{
	struct scsi_disk *sdkp = to_scsi_disk(cdev);
	struct scsi_device *sdp = sdkp->device;

	return snprintf(buf, 20, "%u\n", sdp->manage_start_stop);
}

static ssize_t sd_show_allow_restart(struct class_device *cdev, char *buf)
{
	struct scsi_disk *sdkp = to_scsi_disk(cdev);

	return snprintf(buf, 40, "%d\n", sdkp->device->allow_restart);
}

static struct class_device_attribute sd_disk_attrs[] = {
	__ATTR(cache_type, S_IRUGO|S_IWUSR, sd_show_cache_type,
	       sd_store_cache_type),
	__ATTR(FUA, S_IRUGO, sd_show_fua, NULL),
	__ATTR(allow_restart, S_IRUGO|S_IWUSR, sd_show_allow_restart,
	       sd_store_allow_restart),
	__ATTR(manage_start_stop, S_IRUGO|S_IWUSR, sd_show_manage_start_stop,
	       sd_store_manage_start_stop),
	__ATTR_NULL,
};

static struct class sd_disk_class = {
	.name		= "scsi_disk",
	.owner		= THIS_MODULE,
	.release	= scsi_disk_release,
	.class_dev_attrs = sd_disk_attrs,
};

static struct scsi_driver sd_template = {
	.owner			= THIS_MODULE,
	.gendrv = {
		.name		= "sd",
		.probe		= sd_probe,
		.remove		= sd_remove,
		.suspend	= sd_suspend,
		.resume		= sd_resume,
		.shutdown	= sd_shutdown,
	},
	.rescan			= sd_rescan,
	.init_command		= sd_init_command,
};

/*
 * Device no to disk mapping:
 * 
 *       major         disc2     disc  p1
 *   |............|.............|....|....| <- dev_t
 *    31        20 19          8 7  4 3  0
 * 
 * Inside a major, we have 16k disks, however mapped non-
 * contiguously. The first 16 disks are for major0, the next
 * ones with major1, ... Disk 256 is for major0 again, disk 272 
 * for major1, ... 
 * As we stay compatible with our numbering scheme, we can reuse 
 * the well-know SCSI majors 8, 65--71, 136--143.
 */
static int sd_major(int major_idx)
{
	switch (major_idx) {
	case 0:
		return SCSI_DISK0_MAJOR;
	case 1 ... 7:
		return SCSI_DISK1_MAJOR + major_idx - 1;
	case 8 ... 15:
		return SCSI_DISK8_MAJOR + major_idx - 8;
	default:
		BUG();
		return 0;	/* shut up gcc */
	}
}

static inline struct scsi_disk *scsi_disk(struct gendisk *disk)
{
	return container_of(disk->private_data, struct scsi_disk, driver);
}

static struct scsi_disk *__scsi_disk_get(struct gendisk *disk)
{
	struct scsi_disk *sdkp = NULL;

	if (disk->private_data) {
		sdkp = scsi_disk(disk);
		if (scsi_device_get(sdkp->device) == 0)
			class_device_get(&sdkp->cdev);
		else
			sdkp = NULL;
	}
	return sdkp;
}

static struct scsi_disk *scsi_disk_get(struct gendisk *disk)
{
	struct scsi_disk *sdkp;

	mutex_lock(&sd_ref_mutex);
	sdkp = __scsi_disk_get(disk);
	mutex_unlock(&sd_ref_mutex);
	return sdkp;
}

static struct scsi_disk *scsi_disk_get_from_dev(struct device *dev)
{
	struct scsi_disk *sdkp;

	mutex_lock(&sd_ref_mutex);
	sdkp = dev_get_drvdata(dev);
	if (sdkp)
		sdkp = __scsi_disk_get(sdkp->disk);
	mutex_unlock(&sd_ref_mutex);
	return sdkp;
}

static void scsi_disk_put(struct scsi_disk *sdkp)
{
	struct scsi_device *sdev = sdkp->device;

	mutex_lock(&sd_ref_mutex);
	class_device_put(&sdkp->cdev);
	scsi_device_put(sdev);
	mutex_unlock(&sd_ref_mutex);
}

/**
 *	sd_init_command - build a scsi (read or write) command from
 *	information in the request structure.
 *	@SCpnt: pointer to mid-level's per scsi command structure that
 *	contains request and into which the scsi command is written
 *
 *	Returns 1 if successful and 0 if error (or cannot be done now).
 **/
static int sd_init_command(struct scsi_cmnd * SCpnt)
{
	struct scsi_device *sdp = SCpnt->device;
	struct request *rq = SCpnt->request;
	struct gendisk *disk = rq->rq_disk;
	sector_t block = rq->sector;
	unsigned int this_count = SCpnt->request_bufflen >> 9;
	unsigned int timeout = sdp->timeout;

	SCSI_LOG_HLQUEUE(1, scmd_printk(KERN_INFO, SCpnt,
					"sd_init_command: block=%llu, "
					"count=%d\n",
					(unsigned long long)block,
					this_count));

	if (!sdp || !scsi_device_online(sdp) ||
 	    block + rq->nr_sectors > get_capacity(disk)) {
		SCSI_LOG_HLQUEUE(2, scmd_printk(KERN_INFO, SCpnt,
						"Finishing %ld sectors\n",
						rq->nr_sectors));
		SCSI_LOG_HLQUEUE(2, scmd_printk(KERN_INFO, SCpnt,
						"Retry with 0x%p\n", SCpnt));
		return 0;
	}

	if (sdp->changed) {
		/*
		 * quietly refuse to do anything to a changed disc until 
		 * the changed bit has been reset
		 */
		/* printk("SCSI disk has been changed. Prohibiting further I/O.\n"); */
		return 0;
	}
	SCSI_LOG_HLQUEUE(2, scmd_printk(KERN_INFO, SCpnt, "block=%llu\n",
					(unsigned long long)block));

	/*
	 * If we have a 1K hardware sectorsize, prevent access to single
	 * 512 byte sectors.  In theory we could handle this - in fact
	 * the scsi cdrom driver must be able to handle this because
	 * we typically use 1K blocksizes, and cdroms typically have
	 * 2K hardware sectorsizes.  Of course, things are simpler
	 * with the cdrom, since it is read-only.  For performance
	 * reasons, the filesystems should be able to handle this
	 * and not force the scsi disk driver to use bounce buffers
	 * for this.
	 */
	if (sdp->sector_size == 1024) {
		if ((block & 1) || (rq->nr_sectors & 1)) {
			scmd_printk(KERN_ERR, SCpnt,
				    "Bad block number requested\n");
			return 0;
		} else {
			block = block >> 1;
			this_count = this_count >> 1;
		}
	}
	if (sdp->sector_size == 2048) {
		if ((block & 3) || (rq->nr_sectors & 3)) {
			scmd_printk(KERN_ERR, SCpnt,
				    "Bad block number requested\n");
			return 0;
		} else {
			block = block >> 2;
			this_count = this_count >> 2;
		}
	}
	if (sdp->sector_size == 4096) {
		if ((block & 7) || (rq->nr_sectors & 7)) {
			scmd_printk(KERN_ERR, SCpnt,
				    "Bad block number requested\n");
			return 0;
		} else {
			block = block >> 3;
			this_count = this_count >> 3;
		}
	}
	if (rq_data_dir(rq) == WRITE) {
		if (!sdp->writeable) {
			return 0;
		}
		SCpnt->cmnd[0] = WRITE_6;
		SCpnt->sc_data_direction = DMA_TO_DEVICE;
	} else if (rq_data_dir(rq) == READ) {
		SCpnt->cmnd[0] = READ_6;
		SCpnt->sc_data_direction = DMA_FROM_DEVICE;
	} else {
		scmd_printk(KERN_ERR, SCpnt, "Unknown command %x\n", rq->cmd_flags);
		return 0;
	}

	SCSI_LOG_HLQUEUE(2, scmd_printk(KERN_INFO, SCpnt,
					"%s %d/%ld 512 byte blocks.\n",
					(rq_data_dir(rq) == WRITE) ?
					"writing" : "reading", this_count,
					rq->nr_sectors));

	SCpnt->cmnd[1] = 0;
	
	if (block > 0xffffffff) {
		SCpnt->cmnd[0] += READ_16 - READ_6;
		SCpnt->cmnd[1] |= blk_fua_rq(rq) ? 0x8 : 0;
		SCpnt->cmnd[2] = sizeof(block) > 4 ? (unsigned char) (block >> 56) & 0xff : 0;
		SCpnt->cmnd[3] = sizeof(block) > 4 ? (unsigned char) (block >> 48) & 0xff : 0;
		SCpnt->cmnd[4] = sizeof(block) > 4 ? (unsigned char) (block >> 40) & 0xff : 0;
		SCpnt->cmnd[5] = sizeof(block) > 4 ? (unsigned char) (block >> 32) & 0xff : 0;
		SCpnt->cmnd[6] = (unsigned char) (block >> 24) & 0xff;
		SCpnt->cmnd[7] = (unsigned char) (block >> 16) & 0xff;
		SCpnt->cmnd[8] = (unsigned char) (block >> 8) & 0xff;
		SCpnt->cmnd[9] = (unsigned char) block & 0xff;
		SCpnt->cmnd[10] = (unsigned char) (this_count >> 24) & 0xff;
		SCpnt->cmnd[11] = (unsigned char) (this_count >> 16) & 0xff;
		SCpnt->cmnd[12] = (unsigned char) (this_count >> 8) & 0xff;
		SCpnt->cmnd[13] = (unsigned char) this_count & 0xff;
		SCpnt->cmnd[14] = SCpnt->cmnd[15] = 0;
	} else if ((this_count > 0xff) || (block > 0x1fffff) ||
		   SCpnt->device->use_10_for_rw) {
		if (this_count > 0xffff)
			this_count = 0xffff;

		SCpnt->cmnd[0] += READ_10 - READ_6;
		SCpnt->cmnd[1] |= blk_fua_rq(rq) ? 0x8 : 0;
		SCpnt->cmnd[2] = (unsigned char) (block >> 24) & 0xff;
		SCpnt->cmnd[3] = (unsigned char) (block >> 16) & 0xff;
		SCpnt->cmnd[4] = (unsigned char) (block >> 8) & 0xff;
		SCpnt->cmnd[5] = (unsigned char) block & 0xff;
		SCpnt->cmnd[6] = SCpnt->cmnd[9] = 0;
		SCpnt->cmnd[7] = (unsigned char) (this_count >> 8) & 0xff;
		SCpnt->cmnd[8] = (unsigned char) this_count & 0xff;
	} else {
		if (unlikely(blk_fua_rq(rq))) {
			/*
			 * This happens only if this drive failed
			 * 10byte rw command with ILLEGAL_REQUEST
			 * during operation and thus turned off
			 * use_10_for_rw.
			 */
			scmd_printk(KERN_ERR, SCpnt,
				    "FUA write on READ/WRITE(6) drive\n");
			return 0;
		}

		SCpnt->cmnd[1] |= (unsigned char) ((block >> 16) & 0x1f);
		SCpnt->cmnd[2] = (unsigned char) ((block >> 8) & 0xff);
		SCpnt->cmnd[3] = (unsigned char) block & 0xff;
		SCpnt->cmnd[4] = (unsigned char) this_count;
		SCpnt->cmnd[5] = 0;
	}
	SCpnt->request_bufflen = this_count * sdp->sector_size;

	/*
	 * We shouldn't disconnect in the middle of a sector, so with a dumb
	 * host adapter, it's safe to assume that we can at least transfer
	 * this many bytes between each connect / disconnect.
	 */
	SCpnt->transfersize = sdp->sector_size;
	SCpnt->underflow = this_count << 9;
	SCpnt->allowed = SD_MAX_RETRIES;
	SCpnt->timeout_per_command = timeout;

	/*
	 * This is the completion routine we use.  This is matched in terms
	 * of capability to this function.
	 */
	SCpnt->done = sd_rw_intr;

	/*
	 * This indicates that the command is ready from our end to be
	 * queued.
	 */
	return 1;
}

/**
 *	sd_open - open a scsi disk device
 *	@inode: only i_rdev member may be used
 *	@filp: only f_mode and f_flags may be used
 *
 *	Returns 0 if successful. Returns a negated errno value in case 
 *	of error.
 *
 *	Note: This can be called from a user context (e.g. fsck(1) )
 *	or from within the kernel (e.g. as a result of a mount(1) ).
 *	In the latter case @inode and @filp carry an abridged amount
 *	of information as noted above.
 **/
static int sd_open(struct inode *inode, struct file *filp)
{
	struct gendisk *disk = inode->i_bdev->bd_disk;
	struct scsi_disk *sdkp;
	struct scsi_device *sdev;
	int retval;

	if (!(sdkp = scsi_disk_get(disk)))
		return -ENXIO;


	SCSI_LOG_HLQUEUE(3, sd_printk(KERN_INFO, sdkp, "sd_open\n"));

	sdev = sdkp->device;

	/*
	 * If the device is in error recovery, wait until it is done.
	 * If the device is offline, then disallow any access to it.
	 */
	retval = -ENXIO;
	if (!scsi_block_when_processing_errors(sdev))
		goto error_out;

	if (sdev->removable || sdkp->write_prot)
		check_disk_change(inode->i_bdev);

	/*
	 * If the drive is empty, just let the open fail.
	 */
	retval = -ENOMEDIUM;
	if (sdev->removable && !sdkp->media_present &&
	    !(filp->f_flags & O_NDELAY))
		goto error_out;

	/*
	 * If the device has the write protect tab set, have the open fail
	 * if the user expects to be able to write to the thing.
	 */
	retval = -EROFS;
	if (sdkp->write_prot && (filp->f_mode & FMODE_WRITE))
		goto error_out;

	/*
	 * It is possible that the disk changing stuff resulted in
	 * the device being taken offline.  If this is the case,
	 * report this to the user, and don't pretend that the
	 * open actually succeeded.
	 */
	retval = -ENXIO;
	if (!scsi_device_online(sdev))
		goto error_out;

	if (!sdkp->openers++ && sdev->removable) {
		if (scsi_block_when_processing_errors(sdev))
			scsi_set_medium_removal(sdev, SCSI_REMOVAL_PREVENT);
	}

	return 0;

error_out:
	scsi_disk_put(sdkp);
	return retval;	
}

/**
 *	sd_release - invoked when the (last) close(2) is called on this
 *	scsi disk.
 *	@inode: only i_rdev member may be used
 *	@filp: only f_mode and f_flags may be used
 *
 *	Returns 0. 
 *
 *	Note: may block (uninterruptible) if error recovery is underway
 *	on this disk.
 **/
static int sd_release(struct inode *inode, struct file *filp)
{
	struct gendisk *disk = inode->i_bdev->bd_disk;
	struct scsi_disk *sdkp = scsi_disk(disk);
	struct scsi_device *sdev = sdkp->device;

	SCSI_LOG_HLQUEUE(3, sd_printk(KERN_INFO, sdkp, "sd_release\n"));

	if (!--sdkp->openers && sdev->removable) {
		if (scsi_block_when_processing_errors(sdev))
			scsi_set_medium_removal(sdev, SCSI_REMOVAL_ALLOW);
	}

	/*
	 * XXX and what if there are packets in flight and this close()
	 * XXX is followed by a "rmmod sd_mod"?
	 */
	scsi_disk_put(sdkp);
	return 0;
}

static int sd_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	struct scsi_disk *sdkp = scsi_disk(bdev->bd_disk);
	struct scsi_device *sdp = sdkp->device;
	struct Scsi_Host *host = sdp->host;
	int diskinfo[4];

	/* default to most commonly used values */
        diskinfo[0] = 0x40;	/* 1 << 6 */
       	diskinfo[1] = 0x20;	/* 1 << 5 */
       	diskinfo[2] = sdkp->capacity >> 11;
	
	/* override with calculated, extended default, or driver values */
	if (host->hostt->bios_param)
		host->hostt->bios_param(sdp, bdev, sdkp->capacity, diskinfo);
	else
		scsicam_bios_param(bdev, sdkp->capacity, diskinfo);

	geo->heads = diskinfo[0];
	geo->sectors = diskinfo[1];
	geo->cylinders = diskinfo[2];
	return 0;
}

/**
 *	sd_ioctl - process an ioctl
 *	@inode: only i_rdev/i_bdev members may be used
 *	@filp: only f_mode and f_flags may be used
 *	@cmd: ioctl command number
 *	@arg: this is third argument given to ioctl(2) system call.
 *	Often contains a pointer.
 *
 *	Returns 0 if successful (some ioctls return postive numbers on
 *	success as well). Returns a negated errno value in case of error.
 *
 *	Note: most ioctls are forward onto the block subsystem or further
 *	down in the scsi subsytem.
 **/
static int sd_ioctl(struct inode * inode, struct file * filp, 
		    unsigned int cmd, unsigned long arg)
{
	struct block_device *bdev = inode->i_bdev;
	struct gendisk *disk = bdev->bd_disk;
	struct scsi_device *sdp = scsi_disk(disk)->device;
	void __user *p = (void __user *)arg;
	int error;
    
	SCSI_LOG_IOCTL(1, printk("sd_ioctl: disk=%s, cmd=0x%x\n",
						disk->disk_name, cmd));

	/*
	 * If we are in the middle of error recovery, don't let anyone
	 * else try and use this device.  Also, if error recovery fails, it
	 * may try and take the device offline, in which case all further
	 * access to the device is prohibited.
	 */
	error = scsi_nonblockable_ioctl(sdp, cmd, p, filp);
	if (!scsi_block_when_processing_errors(sdp) || !error)
		return error;

	/*
	 * Send SCSI addressing ioctls directly to mid level, send other
	 * ioctls to block level and then onto mid level if they can't be
	 * resolved.
	 */
	switch (cmd) {
		case SCSI_IOCTL_GET_IDLUN:
		case SCSI_IOCTL_GET_BUS_NUMBER:
			return scsi_ioctl(sdp, cmd, p);
		default:
			error = scsi_cmd_ioctl(filp, disk->queue, disk, cmd, p);
			if (error != -ENOTTY)
				return error;
	}
	return scsi_ioctl(sdp, cmd, p);
}

static void set_media_not_present(struct scsi_disk *sdkp)
{
	sdkp->media_present = 0;
	sdkp->capacity = 0;
	sdkp->device->changed = 1;
}

/**
 *	sd_media_changed - check if our medium changed
 *	@disk: kernel device descriptor 
 *
 *	Returns 0 if not applicable or no change; 1 if change
 *
 *	Note: this function is invoked from the block subsystem.
 **/
static int sd_media_changed(struct gendisk *disk)
{
	struct scsi_disk *sdkp = scsi_disk(disk);
	struct scsi_device *sdp = sdkp->device;
	int retval;

	SCSI_LOG_HLQUEUE(3, sd_printk(KERN_INFO, sdkp, "sd_media_changed\n"));

	if (!sdp->removable)
		return 0;

	/*
	 * If the device is offline, don't send any commands - just pretend as
	 * if the command failed.  If the device ever comes back online, we
	 * can deal with it then.  It is only because of unrecoverable errors
	 * that we would ever take a device offline in the first place.
	 */
	if (!scsi_device_online(sdp))
		goto not_present;

	/*
	 * Using TEST_UNIT_READY enables differentiation between drive with
	 * no cartridge loaded - NOT READY, drive with changed cartridge -
	 * UNIT ATTENTION, or with same cartridge - GOOD STATUS.
	 *
	 * Drives that auto spin down. eg iomega jaz 1G, will be started
	 * by sd_spinup_disk() from sd_revalidate_disk(), which happens whenever
	 * sd_revalidate() is called.
	 */
	retval = -ENODEV;
	if (scsi_block_when_processing_errors(sdp))
		retval = scsi_test_unit_ready(sdp, SD_TIMEOUT, SD_MAX_RETRIES);

	/*
	 * Unable to test, unit probably not ready.   This usually
	 * means there is no disc in the drive.  Mark as changed,
	 * and we will figure it out later once the drive is
	 * available again.
	 */
	if (retval)
		 goto not_present;

	/*
	 * For removable scsi disk we have to recognise the presence
	 * of a disk in the drive. This is kept in the struct scsi_disk
	 * struct and tested at open !  Daniel Roche (dan@lectra.fr)
	 */
	sdkp->media_present = 1;

	retval = sdp->changed;
	sdp->changed = 0;

	return retval;

not_present:
	set_media_not_present(sdkp);
	return 1;
}

static int sd_sync_cache(struct scsi_disk *sdkp)
{
	int retries, res;
	struct scsi_device *sdp = sdkp->device;
	struct scsi_sense_hdr sshdr;

	if (!scsi_device_online(sdp))
		return -ENODEV;


	for (retries = 3; retries > 0; --retries) {
		unsigned char cmd[10] = { 0 };

		cmd[0] = SYNCHRONIZE_CACHE;
		/*
		 * Leave the rest of the command zero to indicate
		 * flush everything.
		 */
		res = scsi_execute_req(sdp, cmd, DMA_NONE, NULL, 0, &sshdr,
				       SD_TIMEOUT, SD_MAX_RETRIES);
		if (res == 0)
			break;
	}

	if (res) {
		sd_print_result(sdkp, res);
		if (driver_byte(res) & DRIVER_SENSE)
			sd_print_sense_hdr(sdkp, &sshdr);
	}

	if (res)
		return -EIO;
	return 0;
}

static int sd_issue_flush(struct request_queue *q, struct gendisk *disk,
			  sector_t *error_sector)
{
	int ret = 0;
	struct scsi_device *sdp = q->queuedata;
	struct scsi_disk *sdkp;

	if (sdp->sdev_state != SDEV_RUNNING)
		return -ENXIO;

	sdkp = scsi_disk_get_from_dev(&sdp->sdev_gendev);

	if (!sdkp)
               return -ENODEV;

	if (sdkp->WCE)
		ret = sd_sync_cache(sdkp);
	scsi_disk_put(sdkp);
	return ret;
}

static void sd_prepare_flush(struct request_queue *q, struct request *rq)
{
	memset(rq->cmd, 0, sizeof(rq->cmd));
	rq->cmd_type = REQ_TYPE_BLOCK_PC;
	rq->timeout = SD_TIMEOUT;
	rq->cmd[0] = SYNCHRONIZE_CACHE;
	rq->cmd_len = 10;
}

static void sd_rescan(struct device *dev)
{
	struct scsi_disk *sdkp = scsi_disk_get_from_dev(dev);

	if (sdkp) {
		sd_revalidate_disk(sdkp->disk);
		scsi_disk_put(sdkp);
	}
}


#ifdef CONFIG_COMPAT
/* 
 * This gets directly called from VFS. When the ioctl 
 * is not recognized we go back to the other translation paths. 
 */
static long sd_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct block_device *bdev = file->f_path.dentry->d_inode->i_bdev;
	struct gendisk *disk = bdev->bd_disk;
	struct scsi_device *sdev = scsi_disk(disk)->device;

	/*
	 * If we are in the middle of error recovery, don't let anyone
	 * else try and use this device.  Also, if error recovery fails, it
	 * may try and take the device offline, in which case all further
	 * access to the device is prohibited.
	 */
	if (!scsi_block_when_processing_errors(sdev))
		return -ENODEV;
	       
	if (sdev->host->hostt->compat_ioctl) {
		int ret;

		ret = sdev->host->hostt->compat_ioctl(sdev, cmd, (void __user *)arg);

		return ret;
	}

	/* 
	 * Let the static ioctl translation table take care of it.
	 */
	return -ENOIOCTLCMD; 
}
#endif

static struct block_device_operations sd_fops = {
	.owner			= THIS_MODULE,
	.open			= sd_open,
	.release		= sd_release,
	.ioctl			= sd_ioctl,
	.getgeo			= sd_getgeo,
#ifdef CONFIG_COMPAT
	.compat_ioctl		= sd_compat_ioctl,
#endif
	.media_changed		= sd_media_changed,
	.revalidate_disk	= sd_revalidate_disk,
};

/**
 *	sd_rw_intr - bottom half handler: called when the lower level
 *	driver has completed (successfully or otherwise) a scsi command.
 *	@SCpnt: mid-level's per command structure.
 *
 *	Note: potentially run from within an ISR. Must not block.
 **/
static void sd_rw_intr(struct scsi_cmnd * SCpnt)
{
	int result = SCpnt->result;
 	unsigned int xfer_size = SCpnt->request_bufflen;
 	unsigned int good_bytes = result ? 0 : xfer_size;
 	u64 start_lba = SCpnt->request->sector;
 	u64 bad_lba;
	struct scsi_sense_hdr sshdr;
	int sense_valid = 0;
	int sense_deferred = 0;
	int info_valid;

	if (result) {
		sense_valid = scsi_command_normalize_sense(SCpnt, &sshdr);
		if (sense_valid)
			sense_deferred = scsi_sense_is_deferred(&sshdr);
	}
#ifdef CONFIG_SCSI_LOGGING
	SCSI_LOG_HLCOMPLETE(1, scsi_print_result(SCpnt));
	if (sense_valid) {
		SCSI_LOG_HLCOMPLETE(1, scmd_printk(KERN_INFO, SCpnt,
						   "sd_rw_intr: sb[respc,sk,asc,"
						   "ascq]=%x,%x,%x,%x\n",
						   sshdr.response_code,
						   sshdr.sense_key, sshdr.asc,
						   sshdr.ascq));
	}
#endif
	if (driver_byte(result) != DRIVER_SENSE &&
	    (!sense_valid || sense_deferred))
		goto out;

	switch (sshdr.sense_key) {
	case HARDWARE_ERROR:
	case MEDIUM_ERROR:
		if (!blk_fs_request(SCpnt->request))
			goto out;
		info_valid = scsi_get_sense_info_fld(SCpnt->sense_buffer,
						     SCSI_SENSE_BUFFERSIZE,
						     &bad_lba);
		if (!info_valid)
			goto out;
		if (xfer_size <= SCpnt->device->sector_size)
			goto out;
		switch (SCpnt->device->sector_size) {
		case 256:
			start_lba <<= 1;
			break;
		case 512:
			break;
		case 1024:
			start_lba >>= 1;
			break;
		case 2048:
			start_lba >>= 2;
			break;
		case 4096:
			start_lba >>= 3;
			break;
		default:
			/* Print something here with limiting frequency. */
			goto out;
			break;
		}
		/* This computation should always be done in terms of
		 * the resolution of the device's medium.
		 */
		good_bytes = (bad_lba - start_lba)*SCpnt->device->sector_size;
		break;
	case RECOVERED_ERROR:
	case NO_SENSE:
		/* Inform the user, but make sure that it's not treated
		 * as a hard error.
		 */
		scsi_print_sense("sd", SCpnt);
		SCpnt->result = 0;
		memset(SCpnt->sense_buffer, 0, SCSI_SENSE_BUFFERSIZE);
		good_bytes = xfer_size;
		break;
	case ILLEGAL_REQUEST:
		if (SCpnt->device->use_10_for_rw &&
		    (SCpnt->cmnd[0] == READ_10 ||
		     SCpnt->cmnd[0] == WRITE_10))
			SCpnt->device->use_10_for_rw = 0;
		if (SCpnt->device->use_10_for_ms &&
		    (SCpnt->cmnd[0] == MODE_SENSE_10 ||
		     SCpnt->cmnd[0] == MODE_SELECT_10))
			SCpnt->device->use_10_for_ms = 0;
		break;
	default:
		break;
	}
 out:
	scsi_io_completion(SCpnt, good_bytes);
}

static int media_not_present(struct scsi_disk *sdkp,
			     struct scsi_sense_hdr *sshdr)
{

	if (!scsi_sense_valid(sshdr))
		return 0;
	/* not invoked for commands that could return deferred errors */
	if (sshdr->sense_key != NOT_READY &&
	    sshdr->sense_key != UNIT_ATTENTION)
		return 0;
	if (sshdr->asc != 0x3A) /* medium not present */
		return 0;

	set_media_not_present(sdkp);
	return 1;
}

/*
 * spinup disk - called only in sd_revalidate_disk()
 */
static void
sd_spinup_disk(struct scsi_disk *sdkp)
{
	unsigned char cmd[10];
	unsigned long spintime_expire = 0;
	int retries, spintime;
	unsigned int the_result;
	struct scsi_sense_hdr sshdr;
	int sense_valid = 0;

	spintime = 0;

	/* Spin up drives, as required.  Only do this at boot time */
	/* Spinup needs to be done for module loads too. */
	do {
		retries = 0;

		do {
			cmd[0] = TEST_UNIT_READY;
			memset((void *) &cmd[1], 0, 9);

			the_result = scsi_execute_req(sdkp->device, cmd,
						      DMA_NONE, NULL, 0,
						      &sshdr, SD_TIMEOUT,
						      SD_MAX_RETRIES);

			/*
			 * If the drive has indicated to us that it
			 * doesn't have any media in it, don't bother
			 * with any more polling.
			 */
			if (media_not_present(sdkp, &sshdr))
				return;

			if (the_result)
				sense_valid = scsi_sense_valid(&sshdr);
			retries++;
		} while (retries < 3 && 
			 (!scsi_status_is_good(the_result) ||
			  ((driver_byte(the_result) & DRIVER_SENSE) &&
			  sense_valid && sshdr.sense_key == UNIT_ATTENTION)));

		if ((driver_byte(the_result) & DRIVER_SENSE) == 0) {
			/* no sense, TUR either succeeded or failed
			 * with a status error */
			if(!spintime && !scsi_status_is_good(the_result)) {
				sd_printk(KERN_NOTICE, sdkp, "Unit Not Ready\n");
				sd_print_result(sdkp, the_result);
			}
			break;
		}
					
		/*
		 * The device does not want the automatic start to be issued.
		 */
		if (sdkp->device->no_start_on_add) {
			break;
		}

		/*
		 * If manual intervention is required, or this is an
		 * absent USB storage device, a spinup is meaningless.
		 */
		if (sense_valid &&
		    sshdr.sense_key == NOT_READY &&
		    sshdr.asc == 4 && sshdr.ascq == 3) {
			break;		/* manual intervention required */

		/*
		 * Issue command to spin up drive when not ready
		 */
		} else if (sense_valid && sshdr.sense_key == NOT_READY) {
			if (!spintime) {
				sd_printk(KERN_NOTICE, sdkp, "Spinning up disk...");
				cmd[0] = START_STOP;
				cmd[1] = 1;	/* Return immediately */
				memset((void *) &cmd[2], 0, 8);
				cmd[4] = 1;	/* Start spin cycle */
				scsi_execute_req(sdkp->device, cmd, DMA_NONE,
						 NULL, 0, &sshdr,
						 SD_TIMEOUT, SD_MAX_RETRIES);
				spintime_expire = jiffies + 100 * HZ;
				spintime = 1;
			}
			/* Wait 1 second for next try */
			msleep(1000);
			printk(".");

		/*
		 * Wait for USB flash devices with slow firmware.
		 * Yes, this sense key/ASC combination shouldn't
		 * occur here.  It's characteristic of these devices.
		 */
		} else if (sense_valid &&
				sshdr.sense_key == UNIT_ATTENTION &&
				sshdr.asc == 0x28) {
			if (!spintime) {
				spintime_expire = jiffies + 5 * HZ;
				spintime = 1;
			}
			/* Wait 1 second for next try */
			msleep(1000);
		} else {
			/* we don't understand the sense code, so it's
			 * probably pointless to loop */
			if(!spintime) {
				sd_printk(KERN_NOTICE, sdkp, "Unit Not Ready\n");
				sd_print_sense_hdr(sdkp, &sshdr);
			}
			break;
		}
				
	} while (spintime && time_before_eq(jiffies, spintime_expire));

	if (spintime) {
		if (scsi_status_is_good(the_result))
			printk("ready\n");
		else
			printk("not responding...\n");
	}
}

/*
 * read disk capacity
 */
static void
sd_read_capacity(struct scsi_disk *sdkp, unsigned char *buffer)
{
	unsigned char cmd[16];
	int the_result, retries;
	int sector_size = 0;
	int longrc = 0;
	struct scsi_sense_hdr sshdr;
	int sense_valid = 0;
	struct scsi_device *sdp = sdkp->device;

repeat:
	retries = 3;
	do {
		if (longrc) {
			memset((void *) cmd, 0, 16);
			cmd[0] = SERVICE_ACTION_IN;
			cmd[1] = SAI_READ_CAPACITY_16;
			cmd[13] = 12;
			memset((void *) buffer, 0, 12);
		} else {
			cmd[0] = READ_CAPACITY;
			memset((void *) &cmd[1], 0, 9);
			memset((void *) buffer, 0, 8);
		}
		
		the_result = scsi_execute_req(sdp, cmd, DMA_FROM_DEVICE,
					      buffer, longrc ? 12 : 8, &sshdr,
					      SD_TIMEOUT, SD_MAX_RETRIES);

		if (media_not_present(sdkp, &sshdr))
			return;

		if (the_result)
			sense_valid = scsi_sense_valid(&sshdr);
		retries--;

	} while (the_result && retries);

	if (the_result && !longrc) {
		sd_printk(KERN_NOTICE, sdkp, "READ CAPACITY failed\n");
		sd_print_result(sdkp, the_result);
		if (driver_byte(the_result) & DRIVER_SENSE)
			sd_print_sense_hdr(sdkp, &sshdr);
		else
			sd_printk(KERN_NOTICE, sdkp, "Sense not available.\n");

		/* Set dirty bit for removable devices if not ready -
		 * sometimes drives will not report this properly. */
		if (sdp->removable &&
		    sense_valid && sshdr.sense_key == NOT_READY)
			sdp->changed = 1;

		/* Either no media are present but the drive didn't tell us,
		   or they are present but the read capacity command fails */
		/* sdkp->media_present = 0; -- not always correct */
		sdkp->capacity = 0; /* unknown mapped to zero - as usual */

		return;
	} else if (the_result && longrc) {
		/* READ CAPACITY(16) has been failed */
		sd_printk(KERN_NOTICE, sdkp, "READ CAPACITY(16) failed\n");
		sd_print_result(sdkp, the_result);
		sd_printk(KERN_NOTICE, sdkp, "Use 0xffffffff as device size\n");

		sdkp->capacity = 1 + (sector_t) 0xffffffff;		
		goto got_data;
	}	
	
	if (!longrc) {
		sector_size = (buffer[4] << 24) |
			(buffer[5] << 16) | (buffer[6] << 8) | buffer[7];
		if (buffer[0] == 0xff && buffer[1] == 0xff &&
		    buffer[2] == 0xff && buffer[3] == 0xff) {
			if(sizeof(sdkp->capacity) > 4) {
				sd_printk(KERN_NOTICE, sdkp, "Very big device. "
					  "Trying to use READ CAPACITY(16).\n");
				longrc = 1;
				goto repeat;
			}
			sd_printk(KERN_ERR, sdkp, "Too big for this kernel. Use "
				  "a kernel compiled with support for large "
				  "block devices.\n");
			sdkp->capacity = 0;
			goto got_data;
		}
		sdkp->capacity = 1 + (((sector_t)buffer[0] << 24) |
			(buffer[1] << 16) |
			(buffer[2] << 8) |
			buffer[3]);			
	} else {
		sdkp->capacity = 1 + (((u64)buffer[0] << 56) |
			((u64)buffer[1] << 48) |
			((u64)buffer[2] << 40) |
			((u64)buffer[3] << 32) |
			((sector_t)buffer[4] << 24) |
			((sector_t)buffer[5] << 16) |
			((sector_t)buffer[6] << 8)  |
			(sector_t)buffer[7]);
			
		sector_size = (buffer[8] << 24) |
			(buffer[9] << 16) | (buffer[10] << 8) | buffer[11];
	}	

	/* Some devices return the total number of sectors, not the
	 * highest sector number.  Make the necessary adjustment. */
	if (sdp->fix_capacity) {
		--sdkp->capacity;

	/* Some devices have version which report the correct sizes
	 * and others which do not. We guess size according to a heuristic
	 * and err on the side of lowering the capacity. */
	} else {
		if (sdp->guess_capacity)
			if (sdkp->capacity & 0x01) /* odd sizes are odd */
				--sdkp->capacity;
	}

got_data:
	if (sector_size == 0) {
		sector_size = 512;
		sd_printk(KERN_NOTICE, sdkp, "Sector size 0 reported, "
			  "assuming 512.\n");
	}

	if (sector_size != 512 &&
	    sector_size != 1024 &&
	    sector_size != 2048 &&
	    sector_size != 4096 &&
	    sector_size != 256) {
		sd_printk(KERN_NOTICE, sdkp, "Unsupported sector size %d.\n",
			  sector_size);
		/*
		 * The user might want to re-format the drive with
		 * a supported sectorsize.  Once this happens, it
		 * would be relatively trivial to set the thing up.
		 * For this reason, we leave the thing in the table.
		 */
		sdkp->capacity = 0;
		/*
		 * set a bogus sector size so the normal read/write
		 * logic in the block layer will eventually refuse any
		 * request on this device without tripping over power
		 * of two sector size assumptions
		 */
		sector_size = 512;
	}
	{
		/*
		 * The msdos fs needs to know the hardware sector size
		 * So I have created this table. See ll_rw_blk.c
		 * Jacques Gelinas (Jacques@solucorp.qc.ca)
		 */
		int hard_sector = sector_size;
		sector_t sz = (sdkp->capacity/2) * (hard_sector/256);
		struct request_queue *queue = sdp->request_queue;
		sector_t mb = sz;

		blk_queue_hardsect_size(queue, hard_sector);
		/* avoid 64-bit division on 32-bit platforms */
		sector_div(sz, 625);
		mb -= sz - 974;
		sector_div(mb, 1950);

		sd_printk(KERN_NOTICE, sdkp,
			  "%llu %d-byte hardware sectors (%llu MB)\n",
			  (unsigned long long)sdkp->capacity,
			  hard_sector, (unsigned long long)mb);
	}

	/* Rescale capacity to 512-byte units */
	if (sector_size == 4096)
		sdkp->capacity <<= 3;
	else if (sector_size == 2048)
		sdkp->capacity <<= 2;
	else if (sector_size == 1024)
		sdkp->capacity <<= 1;
	else if (sector_size == 256)
		sdkp->capacity >>= 1;

	sdkp->device->sector_size = sector_size;
}

/* called with buffer of length 512 */
static inline int
sd_do_mode_sense(struct scsi_device *sdp, int dbd, int modepage,
		 unsigned char *buffer, int len, struct scsi_mode_data *data,
		 struct scsi_sense_hdr *sshdr)
{
	return scsi_mode_sense(sdp, dbd, modepage, buffer, len,
			       SD_TIMEOUT, SD_MAX_RETRIES, data,
			       sshdr);
}

/*
 * read write protect setting, if possible - called only in sd_revalidate_disk()
 * called with buffer of length SD_BUF_SIZE
 */
static void
sd_read_write_protect_flag(struct scsi_disk *sdkp, unsigned char *buffer)
{
	int res;
	struct scsi_device *sdp = sdkp->device;
	struct scsi_mode_data data;

	set_disk_ro(sdkp->disk, 0);
	if (sdp->skip_ms_page_3f) {
		sd_printk(KERN_NOTICE, sdkp, "Assuming Write Enabled\n");
		return;
	}

	if (sdp->use_192_bytes_for_3f) {
		res = sd_do_mode_sense(sdp, 0, 0x3F, buffer, 192, &data, NULL);
	} else {
		/*
		 * First attempt: ask for all pages (0x3F), but only 4 bytes.
		 * We have to start carefully: some devices hang if we ask
		 * for more than is available.
		 */
		res = sd_do_mode_sense(sdp, 0, 0x3F, buffer, 4, &data, NULL);

		/*
		 * Second attempt: ask for page 0 When only page 0 is
		 * implemented, a request for page 3F may return Sense Key
		 * 5: Illegal Request, Sense Code 24: Invalid field in
		 * CDB.
		 */
		if (!scsi_status_is_good(res))
			res = sd_do_mode_sense(sdp, 0, 0, buffer, 4, &data, NULL);

		/*
		 * Third attempt: ask 255 bytes, as we did earlier.
		 */
		if (!scsi_status_is_good(res))
			res = sd_do_mode_sense(sdp, 0, 0x3F, buffer, 255,
					       &data, NULL);
	}

	if (!scsi_status_is_good(res)) {
		sd_printk(KERN_WARNING, sdkp,
			  "Test WP failed, assume Write Enabled\n");
	} else {
		sdkp->write_prot = ((data.device_specific & 0x80) != 0);
		set_disk_ro(sdkp->disk, sdkp->write_prot);
		sd_printk(KERN_NOTICE, sdkp, "Write Protect is %s\n",
			  sdkp->write_prot ? "on" : "off");
		sd_printk(KERN_DEBUG, sdkp,
			  "Mode Sense: %02x %02x %02x %02x\n",
			  buffer[0], buffer[1], buffer[2], buffer[3]);
	}
}

/*
 * sd_read_cache_type - called only from sd_revalidate_disk()
 * called with buffer of length SD_BUF_SIZE
 */
static void
sd_read_cache_type(struct scsi_disk *sdkp, unsigned char *buffer)
{
	int len = 0, res;
	struct scsi_device *sdp = sdkp->device;

	int dbd;
	int modepage;
	struct scsi_mode_data data;
	struct scsi_sense_hdr sshdr;

	if (sdp->skip_ms_page_8)
		goto defaults;

	if (sdp->type == TYPE_RBC) {
		modepage = 6;
		dbd = 8;
	} else {
		modepage = 8;
		dbd = 0;
	}

	/* cautiously ask */
	res = sd_do_mode_sense(sdp, dbd, modepage, buffer, 4, &data, &sshdr);

	if (!scsi_status_is_good(res))
		goto bad_sense;

	if (!data.header_length) {
		modepage = 6;
		sd_printk(KERN_ERR, sdkp, "Missing header in MODE_SENSE response\n");
	}

	/* that went OK, now ask for the proper length */
	len = data.length;

	/*
	 * We're only interested in the first three bytes, actually.
	 * But the data cache page is defined for the first 20.
	 */
	if (len < 3)
		goto bad_sense;
	if (len > 20)
		len = 20;

	/* Take headers and block descriptors into account */
	len += data.header_length + data.block_descriptor_length;
	if (len > SD_BUF_SIZE)
		goto bad_sense;

	/* Get the data */
	res = sd_do_mode_sense(sdp, dbd, modepage, buffer, len, &data, &sshdr);

	if (scsi_status_is_good(res)) {
		int offset = data.header_length + data.block_descriptor_length;

		if (offset >= SD_BUF_SIZE - 2) {
			sd_printk(KERN_ERR, sdkp, "Malformed MODE SENSE response\n");
			goto defaults;
		}

		if ((buffer[offset] & 0x3f) != modepage) {
			sd_printk(KERN_ERR, sdkp, "Got wrong page\n");
			goto defaults;
		}

		if (modepage == 8) {
			sdkp->WCE = ((buffer[offset + 2] & 0x04) != 0);
			sdkp->RCD = ((buffer[offset + 2] & 0x01) != 0);
		} else {
			sdkp->WCE = ((buffer[offset + 2] & 0x01) == 0);
			sdkp->RCD = 0;
		}

		sdkp->DPOFUA = (data.device_specific & 0x10) != 0;
		if (sdkp->DPOFUA && !sdkp->device->use_10_for_rw) {
			sd_printk(KERN_NOTICE, sdkp,
				  "Uses READ/WRITE(6), disabling FUA\n");
			sdkp->DPOFUA = 0;
		}

		sd_printk(KERN_NOTICE, sdkp,
		       "Write cache: %s, read cache: %s, %s\n",
		       sdkp->WCE ? "enabled" : "disabled",
		       sdkp->RCD ? "disabled" : "enabled",
		       sdkp->DPOFUA ? "supports DPO and FUA"
		       : "doesn't support DPO or FUA");

		return;
	}

bad_sense:
	if (scsi_sense_valid(&sshdr) &&
	    sshdr.sense_key == ILLEGAL_REQUEST &&
	    sshdr.asc == 0x24 && sshdr.ascq == 0x0)
		/* Invalid field in CDB */
		sd_printk(KERN_NOTICE, sdkp, "Cache data unavailable\n");
	else
		sd_printk(KERN_ERR, sdkp, "Asking for cache data failed\n");

defaults:
	sd_printk(KERN_ERR, sdkp, "Assuming drive cache: write through\n");
	sdkp->WCE = 0;
	sdkp->RCD = 0;
	sdkp->DPOFUA = 0;
}

/**
 *	sd_revalidate_disk - called the first time a new disk is seen,
 *	performs disk spin up, read_capacity, etc.
 *	@disk: struct gendisk we care about
 **/
static int sd_revalidate_disk(struct gendisk *disk)
{
	struct scsi_disk *sdkp = scsi_disk(disk);
	struct scsi_device *sdp = sdkp->device;
	unsigned char *buffer;
	unsigned ordered;

	SCSI_LOG_HLQUEUE(3, sd_printk(KERN_INFO, sdkp,
				      "sd_revalidate_disk\n"));

	/*
	 * If the device is offline, don't try and read capacity or any
	 * of the other niceties.
	 */
	if (!scsi_device_online(sdp))
		goto out;

	buffer = kmalloc(SD_BUF_SIZE, GFP_KERNEL);
	if (!buffer) {
		sd_printk(KERN_WARNING, sdkp, "sd_revalidate_disk: Memory "
			  "allocation failure.\n");
		goto out;
	}

	/* defaults, until the device tells us otherwise */
	sdp->sector_size = 512;
	sdkp->capacity = 0;
	sdkp->media_present = 1;
	sdkp->write_prot = 0;
	sdkp->WCE = 0;
	sdkp->RCD = 0;

	sd_spinup_disk(sdkp);

	/*
	 * Without media there is no reason to ask; moreover, some devices
	 * react badly if we do.
	 */
	if (sdkp->media_present) {
		sd_read_capacity(sdkp, buffer);
		sd_read_write_protect_flag(sdkp, buffer);
		sd_read_cache_type(sdkp, buffer);
	}

	/*
	 * We now have all cache related info, determine how we deal
	 * with ordered requests.  Note that as the current SCSI
	 * dispatch function can alter request order, we cannot use
	 * QUEUE_ORDERED_TAG_* even when ordered tag is supported.
	 */
	if (sdkp->WCE)
		ordered = sdkp->DPOFUA
			? QUEUE_ORDERED_DRAIN_FUA : QUEUE_ORDERED_DRAIN_FLUSH;
	else
		ordered = QUEUE_ORDERED_DRAIN;

	blk_queue_ordered(sdkp->disk->queue, ordered, sd_prepare_flush);

	set_capacity(disk, sdkp->capacity);
	kfree(buffer);

 out:
	return 0;
}

/**
 *	sd_probe - called during driver initialization and whenever a
 *	new scsi device is attached to the system. It is called once
 *	for each scsi device (not just disks) present.
 *	@dev: pointer to device object
 *
 *	Returns 0 if successful (or not interested in this scsi device 
 *	(e.g. scanner)); 1 when there is an error.
 *
 *	Note: this function is invoked from the scsi mid-level.
 *	This function sets up the mapping between a given 
 *	<host,channel,id,lun> (found in sdp) and new device name 
 *	(e.g. /dev/sda). More precisely it is the block device major 
 *	and minor number that is chosen here.
 *
 *	Assume sd_attach is not re-entrant (for time being)
 *	Also think about sd_attach() and sd_remove() running coincidentally.
 **/
static int sd_probe(struct device *dev)
{
	struct scsi_device *sdp = to_scsi_device(dev);
	struct scsi_disk *sdkp;
	struct gendisk *gd;
	u32 index;
	int error;

	error = -ENODEV;
	if (sdp->type != TYPE_DISK && sdp->type != TYPE_MOD && sdp->type != TYPE_RBC)
		goto out;

	SCSI_LOG_HLQUEUE(3, sdev_printk(KERN_INFO, sdp,
					"sd_attach\n"));

	error = -ENOMEM;
	sdkp = kzalloc(sizeof(*sdkp), GFP_KERNEL);
	if (!sdkp)
		goto out;

	gd = alloc_disk(16);
	if (!gd)
		goto out_free;

	if (!idr_pre_get(&sd_index_idr, GFP_KERNEL))
		goto out_put;

	spin_lock(&sd_index_lock);
	error = idr_get_new(&sd_index_idr, NULL, &index);
	spin_unlock(&sd_index_lock);

	if (index >= SD_MAX_DISKS)
		error = -EBUSY;
	if (error)
		goto out_put;

	sdkp->device = sdp;
	sdkp->driver = &sd_template;
	sdkp->disk = gd;
	sdkp->index = index;
	sdkp->openers = 0;

	if (!sdp->timeout) {
		if (sdp->type != TYPE_MOD)
			sdp->timeout = SD_TIMEOUT;
		else
			sdp->timeout = SD_MOD_TIMEOUT;
	}

	class_device_initialize(&sdkp->cdev);
	sdkp->cdev.dev = &sdp->sdev_gendev;
	sdkp->cdev.class = &sd_disk_class;
	strncpy(sdkp->cdev.class_id, sdp->sdev_gendev.bus_id, BUS_ID_SIZE);

	if (class_device_add(&sdkp->cdev))
		goto out_put;

	get_device(&sdp->sdev_gendev);

	gd->major = sd_major((index & 0xf0) >> 4);
	gd->first_minor = ((index & 0xf) << 4) | (index & 0xfff00);
	gd->minors = 16;
	gd->fops = &sd_fops;

	if (index < 26) {
		sprintf(gd->disk_name, "sd%c", 'a' + index % 26);
	} else if (index < (26 + 1) * 26) {
		sprintf(gd->disk_name, "sd%c%c",
			'a' + index / 26 - 1,'a' + index % 26);
	} else {
		const unsigned int m1 = (index / 26 - 1) / 26 - 1;
		const unsigned int m2 = (index / 26 - 1) % 26;
		const unsigned int m3 =  index % 26;
		sprintf(gd->disk_name, "sd%c%c%c",
			'a' + m1, 'a' + m2, 'a' + m3);
	}

	gd->private_data = &sdkp->driver;
	gd->queue = sdkp->device->request_queue;

	sd_revalidate_disk(gd);

	blk_queue_issue_flush_fn(sdp->request_queue, sd_issue_flush);

	gd->driverfs_dev = &sdp->sdev_gendev;
	gd->flags = GENHD_FL_DRIVERFS;
	if (sdp->removable)
		gd->flags |= GENHD_FL_REMOVABLE;

	dev_set_drvdata(dev, sdkp);
	add_disk(gd);

	sd_printk(KERN_NOTICE, sdkp, "Attached SCSI %sdisk\n",
		  sdp->removable ? "removable " : "");

	return 0;

 out_put:
	put_disk(gd);
 out_free:
	kfree(sdkp);
 out:
	return error;
}

/**
 *	sd_remove - called whenever a scsi disk (previously recognized by
 *	sd_probe) is detached from the system. It is called (potentially
 *	multiple times) during sd module unload.
 *	@sdp: pointer to mid level scsi device object
 *
 *	Note: this function is invoked from the scsi mid-level.
 *	This function potentially frees up a device name (e.g. /dev/sdc)
 *	that could be re-used by a subsequent sd_probe().
 *	This function is not called when the built-in sd driver is "exit-ed".
 **/
static int sd_remove(struct device *dev)
{
	struct scsi_disk *sdkp = dev_get_drvdata(dev);

	class_device_del(&sdkp->cdev);
	del_gendisk(sdkp->disk);
	sd_shutdown(dev);

	mutex_lock(&sd_ref_mutex);
	dev_set_drvdata(dev, NULL);
	class_device_put(&sdkp->cdev);
	mutex_unlock(&sd_ref_mutex);

	return 0;
}

/**
 *	scsi_disk_release - Called to free the scsi_disk structure
 *	@cdev: pointer to embedded class device
 *
 *	sd_ref_mutex must be held entering this routine.  Because it is
 *	called on last put, you should always use the scsi_disk_get()
 *	scsi_disk_put() helpers which manipulate the semaphore directly
 *	and never do a direct class_device_put().
 **/
static void scsi_disk_release(struct class_device *cdev)
{
	struct scsi_disk *sdkp = to_scsi_disk(cdev);
	struct gendisk *disk = sdkp->disk;
	
	spin_lock(&sd_index_lock);
	idr_remove(&sd_index_idr, sdkp->index);
	spin_unlock(&sd_index_lock);

	disk->private_data = NULL;
	put_disk(disk);
	put_device(&sdkp->device->sdev_gendev);

	kfree(sdkp);
}

static int sd_start_stop_device(struct scsi_disk *sdkp, int start)
{
	unsigned char cmd[6] = { START_STOP };	/* START_VALID */
	struct scsi_sense_hdr sshdr;
	struct scsi_device *sdp = sdkp->device;
	int res;

	if (start)
		cmd[4] |= 1;	/* START */

	if (!scsi_device_online(sdp))
		return -ENODEV;

	res = scsi_execute_req(sdp, cmd, DMA_NONE, NULL, 0, &sshdr,
			       SD_TIMEOUT, SD_MAX_RETRIES);
	if (res) {
		sd_printk(KERN_WARNING, sdkp, "START_STOP FAILED\n");
		sd_print_result(sdkp, res);
		if (driver_byte(res) & DRIVER_SENSE)
			sd_print_sense_hdr(sdkp, &sshdr);
	}

	return res;
}

/*
 * Send a SYNCHRONIZE CACHE instruction down to the device through
 * the normal SCSI command structure.  Wait for the command to
 * complete.
 */
static void sd_shutdown(struct device *dev)
{
	struct scsi_disk *sdkp = scsi_disk_get_from_dev(dev);

	if (!sdkp)
		return;         /* this can happen */

	if (sdkp->WCE) {
		sd_printk(KERN_NOTICE, sdkp, "Synchronizing SCSI cache\n");
		sd_sync_cache(sdkp);
	}

	if (system_state != SYSTEM_RESTART && sdkp->device->manage_start_stop) {
		sd_printk(KERN_NOTICE, sdkp, "Stopping disk\n");
		sd_start_stop_device(sdkp, 0);
	}

	scsi_disk_put(sdkp);
}

static int sd_suspend(struct device *dev, pm_message_t mesg)
{
	struct scsi_disk *sdkp = scsi_disk_get_from_dev(dev);
	int ret = 0;

	if (!sdkp)
		return 0;	/* this can happen */

	if (sdkp->WCE) {
		sd_printk(KERN_NOTICE, sdkp, "Synchronizing SCSI cache\n");
		ret = sd_sync_cache(sdkp);
		if (ret)
			goto done;
	}

	if (mesg.event == PM_EVENT_SUSPEND &&
	    sdkp->device->manage_start_stop) {
		sd_printk(KERN_NOTICE, sdkp, "Stopping disk\n");
		ret = sd_start_stop_device(sdkp, 0);
	}

done:
	scsi_disk_put(sdkp);
	return ret;
}

static int sd_resume(struct device *dev)
{
	struct scsi_disk *sdkp = scsi_disk_get_from_dev(dev);
	int ret = 0;

	if (!sdkp->device->manage_start_stop)
		goto done;

	sd_printk(KERN_NOTICE, sdkp, "Starting disk\n");
	ret = sd_start_stop_device(sdkp, 1);

done:
	scsi_disk_put(sdkp);
	return ret;
}

/**
 *	init_sd - entry point for this driver (both when built in or when
 *	a module).
 *
 *	Note: this function registers this driver with the scsi mid-level.
 **/
static int __init init_sd(void)
{
	int majors = 0, i, err;

	SCSI_LOG_HLQUEUE(3, printk("init_sd: sd driver entry point\n"));

	for (i = 0; i < SD_MAJORS; i++)
		if (register_blkdev(sd_major(i), "sd") == 0)
			majors++;

	if (!majors)
		return -ENODEV;

	err = class_register(&sd_disk_class);
	if (err)
		goto err_out;

	err = scsi_register_driver(&sd_template.gendrv);
	if (err)
		goto err_out_class;

	return 0;

err_out_class:
	class_unregister(&sd_disk_class);
err_out:
	for (i = 0; i < SD_MAJORS; i++)
		unregister_blkdev(sd_major(i), "sd");
	return err;
}

/**
 *	exit_sd - exit point for this driver (when it is a module).
 *
 *	Note: this function unregisters this driver from the scsi mid-level.
 **/
static void __exit exit_sd(void)
{
	int i;

	SCSI_LOG_HLQUEUE(3, printk("exit_sd: exiting sd driver\n"));

	scsi_unregister_driver(&sd_template.gendrv);
	class_unregister(&sd_disk_class);

	for (i = 0; i < SD_MAJORS; i++)
		unregister_blkdev(sd_major(i), "sd");
}

module_init(init_sd);
module_exit(exit_sd);

static void sd_print_sense_hdr(struct scsi_disk *sdkp,
			       struct scsi_sense_hdr *sshdr)
{
	sd_printk(KERN_INFO, sdkp, "");
	scsi_show_sense_hdr(sshdr);
	sd_printk(KERN_INFO, sdkp, "");
	scsi_show_extd_sense(sshdr->asc, sshdr->ascq);
}

static void sd_print_result(struct scsi_disk *sdkp, int result)
{
	sd_printk(KERN_INFO, sdkp, "");
	scsi_show_result(result);
}

