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

#include <linux/config.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/bio.h>
#include <linux/genhd.h>
#include <linux/hdreg.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/blkpg.h>
#include <linux/kref.h>
#include <asm/uaccess.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_driver.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_ioctl.h>
#include <scsi/scsi_request.h>
#include <scsi/scsicam.h>

#include "scsi_logging.h"


/*
 * Remaining dev_t-handling stuff
 */
#define SD_MAJORS	16
#define SD_DISKS	32768	/* anything between 256 and 262144 */

/*
 * Time out in seconds for disks and Magneto-opticals (which are slower).
 */
#define SD_TIMEOUT		(30 * HZ)
#define SD_MOD_TIMEOUT		(75 * HZ)

/*
 * Number of allowed retries
 */
#define SD_MAX_RETRIES		5

static void scsi_disk_release(struct kref *kref);

struct scsi_disk {
	struct scsi_driver *driver;	/* always &sd_template */
	struct scsi_device *device;
	struct kref	kref;
	struct gendisk	*disk;
	unsigned int	openers;	/* protected by BKL for now, yuck */
	sector_t	capacity;	/* size in 512-byte sectors */
	u32		index;
	u8		media_present;
	u8		write_prot;
	unsigned	WCE : 1;	/* state of disk WCE bit */
	unsigned	RCD : 1;	/* state of disk RCD bit, unused */
};


static unsigned long sd_index_bits[SD_DISKS / BITS_PER_LONG];
static spinlock_t sd_index_lock = SPIN_LOCK_UNLOCKED;

/* This semaphore is used to mediate the 0->1 reference get in the
 * face of object destruction (i.e. we can't allow a get on an
 * object after last put) */
static DECLARE_MUTEX(sd_ref_sem);

static int sd_revalidate_disk(struct gendisk *disk);
static void sd_rw_intr(struct scsi_cmnd * SCpnt);

static int sd_probe(struct device *);
static int sd_remove(struct device *);
static void sd_shutdown(struct device *dev);
static void sd_rescan(struct device *);
static int sd_init_command(struct scsi_cmnd *);
static void sd_read_capacity(struct scsi_disk *sdkp, char *diskname,
		 struct scsi_request *SRpnt, unsigned char *buffer);

static struct scsi_driver sd_template = {
	.owner			= THIS_MODULE,
	.gendrv = {
		.name		= "sd",
		.probe		= sd_probe,
		.remove		= sd_remove,
		.shutdown	= sd_shutdown,
	},
	.rescan			= sd_rescan,
	.init_command		= sd_init_command,
};

/* Device no to disk mapping:
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

static unsigned int make_sd_dev(unsigned int sd_nr, unsigned int part)
{
	return  (part & 0xf) | ((sd_nr & 0xf) << 4) |
		(sd_major((sd_nr & 0xf0) >> 4) << 20) | (sd_nr & 0xfff00);
}

/* reverse mapping dev -> (sd_nr, part) not currently needed */

#define to_scsi_disk(obj) container_of(obj,struct scsi_disk,kref)

static inline struct scsi_disk *scsi_disk(struct gendisk *disk)
{
	return container_of(disk->private_data, struct scsi_disk, driver);
}

static struct scsi_disk *scsi_disk_get(struct gendisk *disk)
{
	struct scsi_disk *sdkp = NULL;

	down(&sd_ref_sem);
	if (disk->private_data == NULL)
		goto out;
	sdkp = scsi_disk(disk);
	if (!kref_get(&sdkp->kref))
		goto out_sdkp;
	if (scsi_device_get(sdkp->device))
		goto out_put;
	up(&sd_ref_sem);
	return sdkp;

 out_put:
	kref_put(&sdkp->kref);
 out_sdkp:
	sdkp = NULL;
 out:
	up(&sd_ref_sem);
	return sdkp;
}

static void scsi_disk_put(struct scsi_disk *sdkp)
{
	down(&sd_ref_sem);
	scsi_device_put(sdkp->device);
	kref_put(&sdkp->kref);
	up(&sd_ref_sem);
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
	unsigned int this_count, timeout;
	struct gendisk *disk;
	sector_t block;
	struct scsi_device *sdp = SCpnt->device;

	timeout = sdp->timeout;

	/*
	 * these are already setup, just copy cdb basically
	 */
	if (SCpnt->request->flags & REQ_BLOCK_PC) {
		struct request *rq = SCpnt->request;

		if (sizeof(rq->cmd) > sizeof(SCpnt->cmnd))
			return 0;

		memcpy(SCpnt->cmnd, rq->cmd, sizeof(SCpnt->cmnd));
		if (rq_data_dir(rq) == WRITE)
			SCpnt->sc_data_direction = DMA_TO_DEVICE;
		else if (rq->data_len)
			SCpnt->sc_data_direction = DMA_FROM_DEVICE;
		else
			SCpnt->sc_data_direction = DMA_NONE;

		this_count = rq->data_len;
		if (rq->timeout)
			timeout = rq->timeout;

		SCpnt->transfersize = rq->data_len;
		goto queue;
	}

	/*
	 * we only do REQ_CMD and REQ_BLOCK_PC
	 */
	if (!(SCpnt->request->flags & REQ_CMD))
		return 0;

	disk = SCpnt->request->rq_disk;
	block = SCpnt->request->sector;
	this_count = SCpnt->request_bufflen >> 9;

	SCSI_LOG_HLQUEUE(1, printk("sd_init_command: disk=%s, block=%llu, "
			    "count=%d\n", disk->disk_name, (unsigned long long)block, this_count));

	if (!sdp || !scsi_device_online(sdp) ||
 	    block + SCpnt->request->nr_sectors > get_capacity(disk)) {
		SCSI_LOG_HLQUEUE(2, printk("Finishing %ld sectors\n", 
				 SCpnt->request->nr_sectors));
		SCSI_LOG_HLQUEUE(2, printk("Retry with 0x%p\n", SCpnt));
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
	SCSI_LOG_HLQUEUE(2, printk("%s : block=%llu\n",
				   disk->disk_name, (unsigned long long)block));

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
		if ((block & 1) || (SCpnt->request->nr_sectors & 1)) {
			printk(KERN_ERR "sd: Bad block number requested");
			return 0;
		} else {
			block = block >> 1;
			this_count = this_count >> 1;
		}
	}
	if (sdp->sector_size == 2048) {
		if ((block & 3) || (SCpnt->request->nr_sectors & 3)) {
			printk(KERN_ERR "sd: Bad block number requested");
			return 0;
		} else {
			block = block >> 2;
			this_count = this_count >> 2;
		}
	}
	if (sdp->sector_size == 4096) {
		if ((block & 7) || (SCpnt->request->nr_sectors & 7)) {
			printk(KERN_ERR "sd: Bad block number requested");
			return 0;
		} else {
			block = block >> 3;
			this_count = this_count >> 3;
		}
	}
	if (rq_data_dir(SCpnt->request) == WRITE) {
		if (!sdp->writeable) {
			return 0;
		}
		SCpnt->cmnd[0] = WRITE_6;
		SCpnt->sc_data_direction = DMA_TO_DEVICE;
	} else if (rq_data_dir(SCpnt->request) == READ) {
		SCpnt->cmnd[0] = READ_6;
		SCpnt->sc_data_direction = DMA_FROM_DEVICE;
	} else {
		printk(KERN_ERR "sd: Unknown command %lx\n", 
		       SCpnt->request->flags);
/* overkill 	panic("Unknown sd command %lx\n", SCpnt->request->flags); */
		return 0;
	}

	SCSI_LOG_HLQUEUE(2, printk("%s : %s %d/%ld 512 byte blocks.\n", 
		disk->disk_name, (rq_data_dir(SCpnt->request) == WRITE) ? 
		"writing" : "reading", this_count, SCpnt->request->nr_sectors));

	SCpnt->cmnd[1] = 0;
	
	if (block > 0xffffffff) {
		SCpnt->cmnd[0] += READ_16 - READ_6;
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
		SCpnt->cmnd[2] = (unsigned char) (block >> 24) & 0xff;
		SCpnt->cmnd[3] = (unsigned char) (block >> 16) & 0xff;
		SCpnt->cmnd[4] = (unsigned char) (block >> 8) & 0xff;
		SCpnt->cmnd[5] = (unsigned char) block & 0xff;
		SCpnt->cmnd[6] = SCpnt->cmnd[9] = 0;
		SCpnt->cmnd[7] = (unsigned char) (this_count >> 8) & 0xff;
		SCpnt->cmnd[8] = (unsigned char) this_count & 0xff;
	} else {
		if (this_count > 0xff)
			this_count = 0xff;

		SCpnt->cmnd[1] |= (unsigned char) ((block >> 16) & 0x1f);
		SCpnt->cmnd[2] = (unsigned char) ((block >> 8) & 0xff);
		SCpnt->cmnd[3] = (unsigned char) block & 0xff;
		SCpnt->cmnd[4] = (unsigned char) this_count;
		SCpnt->cmnd[5] = 0;
	}
	SCpnt->request_bufflen = SCpnt->bufflen =
			this_count * sdp->sector_size;

	/*
	 * We shouldn't disconnect in the middle of a sector, so with a dumb
	 * host adapter, it's safe to assume that we can at least transfer
	 * this many bytes between each connect / disconnect.
	 */
	SCpnt->transfersize = sdp->sector_size;
	SCpnt->underflow = this_count << 9;

queue:
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


	SCSI_LOG_HLQUEUE(3, printk("sd_open: disk=%s\n", disk->disk_name));

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

	SCSI_LOG_HLQUEUE(3, printk("sd_release: disk=%s\n", disk->disk_name));

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

static int sd_hdio_getgeo(struct block_device *bdev, struct hd_geometry __user *loc)
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

	if (put_user(diskinfo[0], &loc->heads))
		return -EFAULT;
	if (put_user(diskinfo[1], &loc->sectors))
		return -EFAULT;
	if (put_user(diskinfo[2], &loc->cylinders))
		return -EFAULT;
	if (put_user((unsigned)get_start_sect(bdev),
	             (unsigned long __user *)&loc->start))
		return -EFAULT;
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
	if (!scsi_block_when_processing_errors(sdp))
		return -ENODEV;

	if (cmd == HDIO_GETGEO) {
		if (!arg)
			return -EINVAL;
		return sd_hdio_getgeo(bdev, p);
	}

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
			error = scsi_cmd_ioctl(filp, disk, cmd, p);
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

	SCSI_LOG_HLQUEUE(3, printk("sd_media_changed: disk=%s\n",
						disk->disk_name));

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
		retval = scsi_ioctl(sdp, SCSI_IOCTL_TEST_UNIT_READY, NULL);

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

static void sd_rescan(struct device *dev)
{
	struct scsi_disk *sdkp = dev_get_drvdata(dev);
	sd_revalidate_disk(sdkp->disk);
}

static struct block_device_operations sd_fops = {
	.owner			= THIS_MODULE,
	.open			= sd_open,
	.release		= sd_release,
	.ioctl			= sd_ioctl,
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
	int this_count = SCpnt->bufflen;
	int good_bytes = (result == 0 ? this_count : 0);
	sector_t block_sectors = 1;
	sector_t error_sector;
#ifdef CONFIG_SCSI_LOGGING
	SCSI_LOG_HLCOMPLETE(1, printk("sd_rw_intr: %s: res=0x%x\n", 
				SCpnt->request->rq_disk->disk_name, result));
	if (0 != result) {
		SCSI_LOG_HLCOMPLETE(1, printk("sd_rw_intr: sb[0,2,asc,ascq]"
				"=%x,%x,%x,%x\n", SCpnt->sense_buffer[0],
			SCpnt->sense_buffer[2], SCpnt->sense_buffer[12],
			SCpnt->sense_buffer[13]));
	}
#endif
	/*
	   Handle MEDIUM ERRORs that indicate partial success.  Since this is a
	   relatively rare error condition, no care is taken to avoid
	   unnecessary additional work such as memcpy's that could be avoided.
	 */

	/* An error occurred */
	if (driver_byte(result) != 0 && 	/* An error occurred */
	    (SCpnt->sense_buffer[0] & 0x7f) == 0x70) { /* Sense current */
		switch (SCpnt->sense_buffer[2]) {
		case MEDIUM_ERROR:
			if (!(SCpnt->sense_buffer[0] & 0x80))
				break;
			if (!blk_fs_request(SCpnt->request))
				break;
			error_sector = (SCpnt->sense_buffer[3] << 24) |
			(SCpnt->sense_buffer[4] << 16) |
			(SCpnt->sense_buffer[5] << 8) |
			SCpnt->sense_buffer[6];
			if (SCpnt->request->bio != NULL)
				block_sectors = bio_sectors(SCpnt->request->bio);
			switch (SCpnt->device->sector_size) {
			case 1024:
				error_sector <<= 1;
				if (block_sectors < 2)
					block_sectors = 2;
				break;
			case 2048:
				error_sector <<= 2;
				if (block_sectors < 4)
					block_sectors = 4;
				break;
			case 4096:
				error_sector <<=3;
				if (block_sectors < 8)
					block_sectors = 8;
				break;
			case 256:
				error_sector >>= 1;
				break;
			default:
				break;
			}

			error_sector &= ~(block_sectors - 1);
			good_bytes = (error_sector - SCpnt->request->sector) << 9;
			if (good_bytes < 0 || good_bytes >= this_count)
				good_bytes = 0;
			break;

		case RECOVERED_ERROR: /* an error occurred, but it recovered */
		case NO_SENSE: /* LLDD got sense data */
			/*
			 * Inform the user, but make sure that it's not treated
			 * as a hard error.
			 */
			scsi_print_sense("sd", SCpnt);
			SCpnt->result = 0;
			SCpnt->sense_buffer[0] = 0x0;
			good_bytes = this_count;
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
	}
	/*
	 * This calls the generic completion function, now that we know
	 * how many actual sectors finished, and how many sectors we need
	 * to say have failed.
	 */
	scsi_io_completion(SCpnt, good_bytes, block_sectors << 9);
}

static int media_not_present(struct scsi_disk *sdkp, struct scsi_request *srp)
{
	if (!srp->sr_result)
		return 0;
	if (!(driver_byte(srp->sr_result) & DRIVER_SENSE))
		return 0;
	if (srp->sr_sense_buffer[2] != NOT_READY &&
	    srp->sr_sense_buffer[2] != UNIT_ATTENTION)
	    	return 0;
	if (srp->sr_sense_buffer[12] != 0x3A) /* medium not present */
		return 0;

	set_media_not_present(sdkp);
	return 1;
}

/*
 * spinup disk - called only in sd_revalidate_disk()
 */
static void
sd_spinup_disk(struct scsi_disk *sdkp, char *diskname,
	       struct scsi_request *SRpnt, unsigned char *buffer) {
	unsigned char cmd[10];
	unsigned long spintime_value = 0;
	int retries, spintime;
	unsigned int the_result;

	spintime = 0;

	/* Spin up drives, as required.  Only do this at boot time */
	/* Spinup needs to be done for module loads too. */
	do {
		retries = 0;

		do {
			cmd[0] = TEST_UNIT_READY;
			memset((void *) &cmd[1], 0, 9);

			SRpnt->sr_cmd_len = 0;
			SRpnt->sr_sense_buffer[0] = 0;
			SRpnt->sr_sense_buffer[2] = 0;
			SRpnt->sr_data_direction = DMA_NONE;

			scsi_wait_req (SRpnt, (void *) cmd, (void *) buffer,
				       0/*512*/, SD_TIMEOUT, SD_MAX_RETRIES);

			the_result = SRpnt->sr_result;
			retries++;
		} while (retries < 3 && 
			 (!scsi_status_is_good(the_result) ||
			  ((driver_byte(the_result) & DRIVER_SENSE) &&
			   SRpnt->sr_sense_buffer[2] == UNIT_ATTENTION)));

		/*
		 * If the drive has indicated to us that it doesn't have
		 * any media in it, don't bother with any of the rest of
		 * this crap.
		 */
		if (media_not_present(sdkp, SRpnt))
			return;

		if ((driver_byte(the_result) & DRIVER_SENSE) == 0) {
			/* no sense, TUR either succeeded or failed
			 * with a status error */
			if(!spintime && !scsi_status_is_good(the_result))
				printk(KERN_NOTICE "%s: Unit Not Ready, error = 0x%x\n", diskname, the_result);
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
		if (SRpnt->sr_sense_buffer[2] == NOT_READY &&
		    SRpnt->sr_sense_buffer[12] == 4 /* not ready */ &&
		    SRpnt->sr_sense_buffer[13] == 3) {
			break;		/* manual intervention required */

		/*
		 * Issue command to spin up drive when not ready
		 */
		} else if (SRpnt->sr_sense_buffer[2] == NOT_READY) {
			unsigned long time1;
			if (!spintime) {
				printk(KERN_NOTICE "%s: Spinning up disk...",
				       diskname);
				cmd[0] = START_STOP;
				cmd[1] = 1;	/* Return immediately */
				memset((void *) &cmd[2], 0, 8);
				cmd[4] = 1;	/* Start spin cycle */
				SRpnt->sr_cmd_len = 0;
				SRpnt->sr_sense_buffer[0] = 0;
				SRpnt->sr_sense_buffer[2] = 0;

				SRpnt->sr_data_direction = DMA_NONE;
				scsi_wait_req(SRpnt, (void *)cmd, 
					      (void *) buffer, 0/*512*/, 
					      SD_TIMEOUT, SD_MAX_RETRIES);
				spintime_value = jiffies;
			}
			spintime = 1;
			time1 = HZ;
			/* Wait 1 second for next try */
			do {
				current->state = TASK_UNINTERRUPTIBLE;
				time1 = schedule_timeout(time1);
			} while(time1);
			printk(".");
		} else {
			/* we don't understand the sense code, so it's
			 * probably pointless to loop */
			if(!spintime) {
				printk(KERN_NOTICE "%s: Unit Not Ready, sense:\n", diskname);
				scsi_print_req_sense("", SRpnt);
			}
			break;
		}
				
	} while (spintime &&
		 time_after(spintime_value + 100 * HZ, jiffies));

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
sd_read_capacity(struct scsi_disk *sdkp, char *diskname,
		 struct scsi_request *SRpnt, unsigned char *buffer) {
	unsigned char cmd[16];
	struct scsi_device *sdp = sdkp->device;
	int the_result, retries;
	int sector_size = 0;
	int longrc = 0;

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
		
		SRpnt->sr_cmd_len = 0;
		SRpnt->sr_sense_buffer[0] = 0;
		SRpnt->sr_sense_buffer[2] = 0;
		SRpnt->sr_data_direction = DMA_FROM_DEVICE;

		scsi_wait_req(SRpnt, (void *) cmd, (void *) buffer,
			      longrc ? 12 : 8, SD_TIMEOUT, SD_MAX_RETRIES);

		if (media_not_present(sdkp, SRpnt))
			return;

		the_result = SRpnt->sr_result;
		retries--;

	} while (the_result && retries);

	if (the_result && !longrc) {
		printk(KERN_NOTICE "%s : READ CAPACITY failed.\n"
		       "%s : status=%x, message=%02x, host=%d, driver=%02x \n",
		       diskname, diskname,
		       status_byte(the_result),
		       msg_byte(the_result),
		       host_byte(the_result),
		       driver_byte(the_result));

		if (driver_byte(the_result) & DRIVER_SENSE)
			scsi_print_req_sense("sd", SRpnt);
		else
			printk("%s : sense not available. \n", diskname);

		/* Set dirty bit for removable devices if not ready -
		 * sometimes drives will not report this properly. */
		if (sdp->removable &&
		    SRpnt->sr_sense_buffer[2] == NOT_READY)
			sdp->changed = 1;

		/* Either no media are present but the drive didn't tell us,
		   or they are present but the read capacity command fails */
		/* sdkp->media_present = 0; -- not always correct */
		sdkp->capacity = 0x200000; /* 1 GB - random */

		return;
	} else if (the_result && longrc) {
		/* READ CAPACITY(16) has been failed */
		printk(KERN_NOTICE "%s : READ CAPACITY(16) failed.\n"
		       "%s : status=%x, message=%02x, host=%d, driver=%02x \n",
		       diskname, diskname,
		       status_byte(the_result),
		       msg_byte(the_result),
		       host_byte(the_result),
		       driver_byte(the_result));
		printk(KERN_NOTICE "%s : use 0xffffffff as device size\n",
		       diskname);
		
		sdkp->capacity = 1 + (sector_t) 0xffffffff;		
		goto got_data;
	}	
	
	if (!longrc) {
		sector_size = (buffer[4] << 24) |
			(buffer[5] << 16) | (buffer[6] << 8) | buffer[7];
		if (buffer[0] == 0xff && buffer[1] == 0xff &&
		    buffer[2] == 0xff && buffer[3] == 0xff) {
			if(sizeof(sdkp->capacity) > 4) {
				printk(KERN_NOTICE "%s : very big device. try to use"
				       " READ CAPACITY(16).\n", diskname);
				longrc = 1;
				goto repeat;
			} else {
				printk(KERN_ERR "%s: too big for kernel.  Assuming maximum 2Tb\n", diskname);
			}
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

got_data:
	if (sector_size == 0) {
		sector_size = 512;
		printk(KERN_NOTICE "%s : sector size 0 reported, "
		       "assuming 512.\n", diskname);
	}

	if (sector_size != 512 &&
	    sector_size != 1024 &&
	    sector_size != 2048 &&
	    sector_size != 4096 &&
	    sector_size != 256) {
		printk(KERN_NOTICE "%s : unsupported sector size "
		       "%d.\n", diskname, sector_size);
		/*
		 * The user might want to re-format the drive with
		 * a supported sectorsize.  Once this happens, it
		 * would be relatively trivial to set the thing up.
		 * For this reason, we leave the thing in the table.
		 */
		sdkp->capacity = 0;
	}
	{
		/*
		 * The msdos fs needs to know the hardware sector size
		 * So I have created this table. See ll_rw_blk.c
		 * Jacques Gelinas (Jacques@solucorp.qc.ca)
		 */
		int hard_sector = sector_size;
		sector_t sz = sdkp->capacity * (hard_sector/256);
		request_queue_t *queue = sdp->request_queue;
		sector_t mb;

		blk_queue_hardsect_size(queue, hard_sector);
		/* avoid 64-bit division on 32-bit platforms */
		mb = sz >> 1;
		sector_div(sz, 1250);
		mb -= sz - 974;
		sector_div(mb, 1950);

		printk(KERN_NOTICE "SCSI device %s: "
		       "%llu %d-byte hdwr sectors (%llu MB)\n",
		       diskname, (unsigned long long)sdkp->capacity,
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
sd_do_mode_sense(struct scsi_request *SRpnt, int dbd, int modepage,
		 unsigned char *buffer, int len, struct scsi_mode_data *data)
{
	return __scsi_mode_sense(SRpnt, dbd, modepage, buffer, len,
				 SD_TIMEOUT, SD_MAX_RETRIES, data);
}

/*
 * read write protect setting, if possible - called only in sd_revalidate_disk()
 * called with buffer of length 512
 */
static void
sd_read_write_protect_flag(struct scsi_disk *sdkp, char *diskname,
		   struct scsi_request *SRpnt, unsigned char *buffer) {
	int res;
	struct scsi_mode_data data;

	set_disk_ro(sdkp->disk, 0);
	if (sdkp->device->skip_ms_page_3f) {
		printk(KERN_NOTICE "%s: assuming Write Enabled\n", diskname);
		return;
	}

	if (sdkp->device->use_192_bytes_for_3f) {
		res = sd_do_mode_sense(SRpnt, 0, 0x3F, buffer, 192, &data);
	} else {
		/*
		 * First attempt: ask for all pages (0x3F), but only 4 bytes.
		 * We have to start carefully: some devices hang if we ask
		 * for more than is available.
		 */
		res = sd_do_mode_sense(SRpnt, 0, 0x3F, buffer, 4, &data);

		/*
		 * Second attempt: ask for page 0 When only page 0 is
		 * implemented, a request for page 3F may return Sense Key
		 * 5: Illegal Request, Sense Code 24: Invalid field in
		 * CDB.
		 */
		if (!scsi_status_is_good(res))
			res = sd_do_mode_sense(SRpnt, 0, 0, buffer, 4, &data);

		/*
		 * Third attempt: ask 255 bytes, as we did earlier.
		 */
		if (!scsi_status_is_good(res))
			res = sd_do_mode_sense(SRpnt, 0, 0x3F, buffer, 255,
					       &data);
	}

	if (!scsi_status_is_good(res)) {
		printk(KERN_WARNING
		       "%s: test WP failed, assume Write Enabled\n", diskname);
	} else {
		sdkp->write_prot = ((data.device_specific & 0x80) != 0);
		set_disk_ro(sdkp->disk, sdkp->write_prot);
		printk(KERN_NOTICE "%s: Write Protect is %s\n", diskname,
		       sdkp->write_prot ? "on" : "off");
		printk(KERN_DEBUG "%s: Mode Sense: %02x %02x %02x %02x\n",
		       diskname, buffer[0], buffer[1], buffer[2], buffer[3]);
	}
}

/*
 * sd_read_cache_type - called only from sd_revalidate_disk()
 * called with buffer of length 512
 */
static void
sd_read_cache_type(struct scsi_disk *sdkp, char *diskname,
		   struct scsi_request *SRpnt, unsigned char *buffer) {
	int len = 0, res;

	const int dbd = 0;	   /* DBD */
	const int modepage = 0x08; /* current values, cache page */
	struct scsi_mode_data data;

	if (sdkp->device->skip_ms_page_8)
		goto defaults;

	/* cautiously ask */
	res = sd_do_mode_sense(SRpnt, dbd, modepage, buffer, 4, &data);

	if (!scsi_status_is_good(res))
		goto bad_sense;

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

	/* Get the data */
	res = sd_do_mode_sense(SRpnt, dbd, modepage, buffer, len, &data);

	if (scsi_status_is_good(res)) {
		const char *types[] = {
			"write through", "none", "write back",
			"write back, no read (daft)"
		};
		int ct = 0;
		int offset = data.header_length +
			data.block_descriptor_length + 2;

		sdkp->WCE = ((buffer[offset] & 0x04) != 0);
		sdkp->RCD = ((buffer[offset] & 0x01) != 0);

		ct =  sdkp->RCD + 2*sdkp->WCE;

		printk(KERN_NOTICE "SCSI device %s: drive cache: %s\n",
		       diskname, types[ct]);

		return;
	}

bad_sense:
	if ((SRpnt->sr_sense_buffer[0] & 0x70) == 0x70
	     && (SRpnt->sr_sense_buffer[2] & 0x0f) == ILLEGAL_REQUEST
	     /* ASC 0x24 ASCQ 0x00: Invalid field in CDB */
	     && SRpnt->sr_sense_buffer[12] == 0x24
	     && SRpnt->sr_sense_buffer[13] == 0x00) {
		printk(KERN_NOTICE "%s: cache data unavailable\n",
		       diskname);
	} else {
		printk(KERN_ERR "%s: asking for cache data failed\n",
		       diskname);
	}

defaults:
	printk(KERN_ERR "%s: assuming drive cache: write through\n",
	       diskname);
	sdkp->WCE = 0;
	sdkp->RCD = 0;
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
	struct scsi_request *sreq;
	unsigned char *buffer;

	SCSI_LOG_HLQUEUE(3, printk("sd_revalidate_disk: disk=%s\n", disk->disk_name));

	/*
	 * If the device is offline, don't try and read capacity or any
	 * of the other niceties.
	 */
	if (!scsi_device_online(sdp))
		goto out;

	sreq = scsi_allocate_request(sdp, GFP_KERNEL);
	if (!sreq) {
		printk(KERN_WARNING "(sd_revalidate_disk:) Request allocation "
		       "failure.\n");
		goto out;
	}

	buffer = kmalloc(512, GFP_KERNEL | __GFP_DMA);
	if (!buffer) {
		printk(KERN_WARNING "(sd_revalidate_disk:) Memory allocation "
		       "failure.\n");
		goto out_release_request;
	}

	/* defaults, until the device tells us otherwise */
	sdp->sector_size = 512;
	sdkp->capacity = 0;
	sdkp->media_present = 1;
	sdkp->write_prot = 0;
	sdkp->WCE = 0;
	sdkp->RCD = 0;

	sd_spinup_disk(sdkp, disk->disk_name, sreq, buffer);

	/*
	 * Without media there is no reason to ask; moreover, some devices
	 * react badly if we do.
	 */
	if (sdkp->media_present) {
		sd_read_capacity(sdkp, disk->disk_name, sreq, buffer);
		if (sdp->removable)
			sd_read_write_protect_flag(sdkp, disk->disk_name,
					sreq, buffer);
		sd_read_cache_type(sdkp, disk->disk_name, sreq, buffer);
	}
		
	set_capacity(disk, sdkp->capacity);
	kfree(buffer);

 out_release_request: 
	scsi_release_request(sreq);
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
	int error, devno;

	error = -ENODEV;
	if ((sdp->type != TYPE_DISK) && (sdp->type != TYPE_MOD))
		goto out;

	SCSI_LOG_HLQUEUE(3, printk("sd_attach: scsi device: <%d,%d,%d,%d>\n", 
			 sdp->host->host_no, sdp->channel, sdp->id, sdp->lun));

	error = -ENOMEM;
	sdkp = kmalloc(sizeof(*sdkp), GFP_KERNEL);
	if (!sdkp)
		goto out;

	memset (sdkp, 0, sizeof(*sdkp));
	kref_init(&sdkp->kref, scsi_disk_release);

	/* Note: We can accomodate 64 partitions, but the genhd code
	 * assumes partitions allocate consecutive minors, which they don't.
	 * So for now stay with max 16 partitions and leave two spare bits. 
	 * Later, we may change the genhd code and the alloc_disk() call
	 * and the ->minors assignment here. 	KG, 2004-02-10
	 */ 
	gd = alloc_disk(16);
	if (!gd)
		goto out_free;

	spin_lock(&sd_index_lock);
	index = find_first_zero_bit(sd_index_bits, SD_DISKS);
	if (index == SD_DISKS) {
		spin_unlock(&sd_index_lock);
		error = -EBUSY;
		goto out_put;
	}
	__set_bit(index, sd_index_bits);
	spin_unlock(&sd_index_lock);

	sdkp->device = sdp;
	sdkp->driver = &sd_template;
	sdkp->disk = gd;
	sdkp->index = index;
	sdkp->openers = 0;

	if (!sdp->timeout) {
		if (sdp->type == TYPE_DISK)
			sdp->timeout = SD_TIMEOUT;
		else
			sdp->timeout = SD_MOD_TIMEOUT;
	}

	devno = make_sd_dev(index, 0);
	gd->major = MAJOR(devno);
	gd->first_minor = MINOR(devno);
	gd->minors = 16;
	gd->fops = &sd_fops;

	if (index < 26) {
		sprintf(gd->disk_name, "sd%c", 'a' + index % 26);
	} else if (index < (26*27)) {
		sprintf(gd->disk_name, "sd%c%c",
			'a' + index / 26 - 1,'a' + index % 26);
	} else {
		const unsigned int m1 = (index / 26 - 1) / 26 - 1;
		const unsigned int m2 = (index / 26 - 1) % 26;
		const unsigned int m3 =  index % 26;
		sprintf(gd->disk_name, "sd%c%c%c",
			'a' + m1, 'a' + m2, 'a' + m3);
	}

	strcpy(gd->devfs_name, sdp->devfs_name);

	gd->private_data = &sdkp->driver;

	sd_revalidate_disk(gd);

	gd->driverfs_dev = &sdp->sdev_gendev;
	gd->flags = GENHD_FL_DRIVERFS;
	if (sdp->removable)
		gd->flags |= GENHD_FL_REMOVABLE;
	gd->queue = sdkp->device->request_queue;

	dev_set_drvdata(dev, sdkp);
	add_disk(gd);

	printk(KERN_NOTICE "Attached scsi %sdisk %s at scsi%d, channel %d, "
	       "id %d, lun %d\n", sdp->removable ? "removable " : "",
	       gd->disk_name, sdp->host->host_no, sdp->channel,
	       sdp->id, sdp->lun);

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

	del_gendisk(sdkp->disk);
	sd_shutdown(dev);
	down(&sd_ref_sem);
	kref_put(&sdkp->kref);
	up(&sd_ref_sem);

	return 0;
}

/**
 *	scsi_disk_release - Called to free the scsi_disk structure
 *	@kref: pointer to embedded kref
 *
 *	sd_ref_sem must be held entering this routine.  Because it is
 *	called on last put, you should always use the scsi_disk_get()
 *	scsi_disk_put() helpers which manipulate the semaphore directly
 *	and never do a direct kref_put().
 **/
static void scsi_disk_release(struct kref *kref)
{
	struct scsi_disk *sdkp = to_scsi_disk(kref);
	struct gendisk *disk = sdkp->disk;
	
	spin_lock(&sd_index_lock);
	clear_bit(sdkp->index, sd_index_bits);
	spin_unlock(&sd_index_lock);

	disk->private_data = NULL;

	put_disk(disk);

	kfree(sdkp);
}

/*
 * Send a SYNCHRONIZE CACHE instruction down to the device through
 * the normal SCSI command structure.  Wait for the command to
 * complete.
 */
static void sd_shutdown(struct device *dev)
{
	struct scsi_device *sdp = to_scsi_device(dev);
	struct scsi_disk *sdkp;
	struct scsi_request *sreq;
	int retries, res;

	sdkp = dev_get_drvdata(dev);
	if (!sdkp)
               return;         /* this can happen */

	if (!scsi_device_online(sdp) || !sdkp->WCE)
		return;

	printk(KERN_NOTICE "Synchronizing SCSI cache for disk %s: ",
			sdkp->disk->disk_name);

	sreq = scsi_allocate_request(sdp, GFP_KERNEL);
	if (!sreq) {
		printk("FAILED\n  No memory for request\n");
		return;
	}

	sreq->sr_data_direction = DMA_NONE;
	for (retries = 3; retries > 0; --retries) {
		unsigned char cmd[10] = { 0 };

		cmd[0] = SYNCHRONIZE_CACHE;
		/*
		 * Leave the rest of the command zero to indicate
		 * flush everything.
		 */
		scsi_wait_req(sreq, cmd, NULL, 0, SD_TIMEOUT, SD_MAX_RETRIES);
		if (sreq->sr_result == 0)
			break;
	}

	res = sreq->sr_result;
	if (res) {
		printk(KERN_WARNING "FAILED\n  status = %x, message = %02x, "
				    "host = %d, driver = %02x\n  ",
				    status_byte(res), msg_byte(res),
				    host_byte(res), driver_byte(res));
			if (driver_byte(res) & DRIVER_SENSE)
				scsi_print_req_sense("sd", sreq);
	}
	
	scsi_release_request(sreq);
	printk("\n");
}	

/**
 *	init_sd - entry point for this driver (both when built in or when
 *	a module).
 *
 *	Note: this function registers this driver with the scsi mid-level.
 **/
static int __init init_sd(void)
{
	int majors = 0, i;

	SCSI_LOG_HLQUEUE(3, printk("init_sd: sd driver entry point\n"));

	for (i = 0; i < SD_MAJORS; i++)
		if (register_blkdev(sd_major(i), "sd") == 0)
			majors++;

	if (!majors)
		return -ENODEV;

	return scsi_register_driver(&sd_template.gendrv);
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
	for (i = 0; i < SD_MAJORS; i++)
		unregister_blkdev(sd_major(i), "sd");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Eric Youngdale");
MODULE_DESCRIPTION("SCSI disk (sd) driver");

module_init(init_sd);
module_exit(exit_sd);
