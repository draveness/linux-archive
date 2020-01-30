/*
 *  sr.c Copyright (C) 1992 David Giller
 *           Copyright (C) 1993, 1994, 1995, 1999 Eric Youngdale
 *
 *  adapted from:
 *      sd.c Copyright (C) 1992 Drew Eckhardt
 *      Linux scsi disk driver by
 *              Drew Eckhardt <drew@colorado.edu>
 *
 *	Modified by Eric Youngdale ericy@andante.org to
 *	add scatter-gather, multiple outstanding request, and other
 *	enhancements.
 *
 *      Modified by Eric Youngdale eric@andante.org to support loadable
 *      low-level scsi drivers.
 *
 *      Modified by Thomas Quinot thomas@melchior.cuivre.fdn.fr to
 *      provide auto-eject.
 *
 *      Modified by Gerd Knorr <kraxel@cs.tu-berlin.de> to support the
 *      generic cdrom interface
 *
 *      Modified by Jens Axboe <axboe@suse.de> - Uniform sr_packet()
 *      interface, capabilities probe additions, ioctl cleanups, etc.
 *
 *	Modified by Richard Gooch <rgooch@atnf.csiro.au> to support devfs
 *
 *	Modified by Jens Axboe <axboe@suse.de> - support DVD-RAM
 *	transparently and lose the GHOST hack
 *
 *	Modified by Arnaldo Carvalho de Melo <acme@conectiva.com.br>
 *	check resource allocation in sr_init and some cleanups
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/bio.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/cdrom.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <asm/uaccess.h>

#include <scsi/scsi.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_driver.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_ioctl.h>	/* For the door lock/unlock commands */
#include <scsi/scsi_request.h>

#include "scsi_logging.h"
#include "sr.h"


MODULE_PARM(xa_test, "i");	/* see sr_ioctl.c */


#define SR_DISKS	256

#define MAX_RETRIES	3
#define SR_TIMEOUT	(30 * HZ)
#define SR_CAPABILITIES \
	(CDC_CLOSE_TRAY|CDC_OPEN_TRAY|CDC_LOCK|CDC_SELECT_SPEED| \
	 CDC_SELECT_DISC|CDC_MULTI_SESSION|CDC_MCN|CDC_MEDIA_CHANGED| \
	 CDC_PLAY_AUDIO|CDC_RESET|CDC_IOCTLS|CDC_DRIVE_STATUS| \
	 CDC_CD_R|CDC_CD_RW|CDC_DVD|CDC_DVD_R|CDC_DVD_RAM|CDC_GENERIC_PACKET| \
	 CDC_MRW|CDC_MRW_W|CDC_RAM)

static int sr_probe(struct device *);
static int sr_remove(struct device *);
static int sr_init_command(struct scsi_cmnd *);

static struct scsi_driver sr_template = {
	.owner			= THIS_MODULE,
	.gendrv = {
		.name   	= "sr",
		.probe		= sr_probe,
		.remove		= sr_remove,
	},
	.init_command		= sr_init_command,
};

static unsigned long sr_index_bits[SR_DISKS / BITS_PER_LONG];
static spinlock_t sr_index_lock = SPIN_LOCK_UNLOCKED;

/* This semaphore is used to mediate the 0->1 reference get in the
 * face of object destruction (i.e. we can't allow a get on an
 * object after last put) */
static DECLARE_MUTEX(sr_ref_sem);

static int sr_open(struct cdrom_device_info *, int);
static void sr_release(struct cdrom_device_info *);

static void get_sectorsize(struct scsi_cd *);
static void get_capabilities(struct scsi_cd *);

static int sr_media_change(struct cdrom_device_info *, int);
static int sr_packet(struct cdrom_device_info *, struct packet_command *);

static struct cdrom_device_ops sr_dops = {
	.open			= sr_open,
	.release	 	= sr_release,
	.drive_status	 	= sr_drive_status,
	.media_changed		= sr_media_change,
	.tray_move		= sr_tray_move,
	.lock_door		= sr_lock_door,
	.select_speed		= sr_select_speed,
	.get_last_session	= sr_get_last_session,
	.get_mcn		= sr_get_mcn,
	.reset			= sr_reset,
	.audio_ioctl		= sr_audio_ioctl,
	.dev_ioctl		= sr_dev_ioctl,
	.capability		= SR_CAPABILITIES,
	.generic_packet		= sr_packet,
};

static void sr_kref_release(struct kref *kref);

static inline struct scsi_cd *scsi_cd(struct gendisk *disk)
{
	return container_of(disk->private_data, struct scsi_cd, driver);
}

/*
 * The get and put routines for the struct scsi_cd.  Note this entity
 * has a scsi_device pointer and owns a reference to this.
 */
static inline struct scsi_cd *scsi_cd_get(struct gendisk *disk)
{
	struct scsi_cd *cd = NULL;

	down(&sr_ref_sem);
	if (disk->private_data == NULL)
		goto out;
	cd = scsi_cd(disk);
	if (!kref_get(&cd->kref))
		goto out_null;
	if (scsi_device_get(cd->device))
		goto out_put;
	goto out;

 out_put:
	kref_put(&cd->kref);
 out_null:
	cd = NULL;
 out:
	up(&sr_ref_sem);
	return cd;
}

static inline void scsi_cd_put(struct scsi_cd *cd)
{
	down(&sr_ref_sem);
	scsi_device_put(cd->device);
	kref_put(&cd->kref);
	up(&sr_ref_sem);
}

/*
 * This function checks to see if the media has been changed in the
 * CDROM drive.  It is possible that we have already sensed a change,
 * or the drive may have sensed one and not yet reported it.  We must
 * be ready for either case. This function always reports the current
 * value of the changed bit.  If flag is 0, then the changed bit is reset.
 * This function could be done as an ioctl, but we would need to have
 * an inode for that to work, and we do not always have one.
 */

int sr_media_change(struct cdrom_device_info *cdi, int slot)
{
	struct scsi_cd *cd = cdi->handle;
	int retval;

	if (CDSL_CURRENT != slot) {
		/* no changer support */
		return -EINVAL;
	}

	retval = scsi_ioctl(cd->device, SCSI_IOCTL_TEST_UNIT_READY, NULL);
	if (retval) {
		/* Unable to test, unit probably not ready.  This usually
		 * means there is no disc in the drive.  Mark as changed,
		 * and we will figure it out later once the drive is
		 * available again.  */
		cd->device->changed = 1;
		return 1;	/* This will force a flush, if called from
				 * check_disk_change */
	};

	retval = cd->device->changed;
	cd->device->changed = 0;
	/* If the disk changed, the capacity will now be different,
	 * so we force a re-read of this information */
	if (retval) {
		/* check multisession offset etc */
		sr_cd_check(cdi);

		/* 
		 * If the disk changed, the capacity will now be different,
		 * so we force a re-read of this information 
		 * Force 2048 for the sector size so that filesystems won't
		 * be trying to use something that is too small if the disc
		 * has changed.
		 */
		cd->needs_sector_size = 1;
		cd->device->sector_size = 2048;
	}
	return retval;
}
 
/*
 * rw_intr is the interrupt routine for the device driver.
 *
 * It will be notified on the end of a SCSI read / write, and will take on
 * of several actions based on success or failure.
 */
static void rw_intr(struct scsi_cmnd * SCpnt)
{
	int result = SCpnt->result;
	int this_count = SCpnt->bufflen;
	int good_bytes = (result == 0 ? this_count : 0);
	int block_sectors = 0;
	long error_sector;
	struct scsi_cd *cd = scsi_cd(SCpnt->request->rq_disk);

#ifdef DEBUG
	printk("sr.c done: %x\n", result);
#endif

	/*
	 * Handle MEDIUM ERRORs or VOLUME OVERFLOWs that indicate partial
	 * success.  Since this is a relatively rare error condition, no
	 * care is taken to avoid unnecessary additional work such as
	 * memcpy's that could be avoided.
	 */
	if (driver_byte(result) != 0 &&		/* An error occurred */
	    (SCpnt->sense_buffer[0] & 0x7f) == 0x70) { /* Sense current */
		switch (SCpnt->sense_buffer[2]) {
		case MEDIUM_ERROR:
		case VOLUME_OVERFLOW:
		case ILLEGAL_REQUEST:
			if (!(SCpnt->sense_buffer[0] & 0x90))
				break;
			if (!blk_fs_request(SCpnt->request))
				break;
			error_sector = (SCpnt->sense_buffer[3] << 24) |
				(SCpnt->sense_buffer[4] << 16) |
				(SCpnt->sense_buffer[5] << 8) |
				SCpnt->sense_buffer[6];
			if (SCpnt->request->bio != NULL)
				block_sectors =
					bio_sectors(SCpnt->request->bio);
			if (block_sectors < 4)
				block_sectors = 4;
			if (cd->device->sector_size == 2048)
				error_sector <<= 2;
			error_sector &= ~(block_sectors - 1);
			good_bytes = (error_sector - SCpnt->request->sector) << 9;
			if (good_bytes < 0 || good_bytes >= this_count)
				good_bytes = 0;
			/*
			 * The SCSI specification allows for the value
			 * returned by READ CAPACITY to be up to 75 2K
			 * sectors past the last readable block.
			 * Therefore, if we hit a medium error within the
			 * last 75 2K sectors, we decrease the saved size
			 * value.
			 */
			if (error_sector < get_capacity(cd->disk) &&
			    cd->capacity - error_sector < 4 * 75)
				set_capacity(cd->disk, error_sector);
			break;

		case RECOVERED_ERROR:

			/*
			 * An error occured, but it recovered.  Inform the
			 * user, but make sure that it's not treated as a
			 * hard error.
			 */
			scsi_print_sense("sr", SCpnt);
			SCpnt->result = 0;
			SCpnt->sense_buffer[0] = 0x0;
			good_bytes = this_count;
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

static int sr_init_command(struct scsi_cmnd * SCpnt)
{
	int block=0, this_count, s_size, timeout = SR_TIMEOUT;
	struct scsi_cd *cd = scsi_cd(SCpnt->request->rq_disk);

	SCSI_LOG_HLQUEUE(1, printk("Doing sr request, dev = %s, block = %d\n",
				cd->disk->disk_name, block));

	if (!cd->device || !scsi_device_online(cd->device)) {
		SCSI_LOG_HLQUEUE(2, printk("Finishing %ld sectors\n",
					SCpnt->request->nr_sectors));
		SCSI_LOG_HLQUEUE(2, printk("Retry with 0x%p\n", SCpnt));
		return 0;
	}

	if (cd->device->changed) {
		/*
		 * quietly refuse to do anything to a changed disc until the
		 * changed bit has been reset
		 */
		return 0;
	}

	/*
	 * these are already setup, just copy cdb basically
	 */
	if (SCpnt->request->flags & REQ_BLOCK_PC) {
		struct request *rq = SCpnt->request;

		if (sizeof(rq->cmd) > sizeof(SCpnt->cmnd))
			return 0;

		memcpy(SCpnt->cmnd, rq->cmd, sizeof(SCpnt->cmnd));
		if (!rq->data_len)
			SCpnt->sc_data_direction = DMA_NONE;
		else if (rq_data_dir(rq) == WRITE)
			SCpnt->sc_data_direction = DMA_TO_DEVICE;
		else
			SCpnt->sc_data_direction = DMA_FROM_DEVICE;

		this_count = rq->data_len;
		if (rq->timeout)
			timeout = rq->timeout;

		SCpnt->transfersize = rq->data_len;
		goto queue;
	}

	if (!(SCpnt->request->flags & REQ_CMD)) {
		blk_dump_rq_flags(SCpnt->request, "sr unsup command");
		return 0;
	}

	/*
	 * we do lazy blocksize switching (when reading XA sectors,
	 * see CDROMREADMODE2 ioctl) 
	 */
	s_size = cd->device->sector_size;
	if (s_size > 2048) {
		if (!in_interrupt())
			sr_set_blocklength(cd, 2048);
		else
			printk("sr: can't switch blocksize: in interrupt\n");
	}

	if (s_size != 512 && s_size != 1024 && s_size != 2048) {
		printk("sr: bad sector size %d\n", s_size);
		return 0;
	}

	if (rq_data_dir(SCpnt->request) == WRITE) {
		if (!cd->device->writeable)
			return 0;
		SCpnt->cmnd[0] = WRITE_10;
		SCpnt->sc_data_direction = DMA_TO_DEVICE;
	} else if (rq_data_dir(SCpnt->request) == READ) {
		SCpnt->cmnd[0] = READ_10;
		SCpnt->sc_data_direction = DMA_FROM_DEVICE;
	} else {
		blk_dump_rq_flags(SCpnt->request, "Unknown sr command");
		return 0;
	}

	{
		struct scatterlist *sg = SCpnt->request_buffer;
		int i, size = 0;
		for (i = 0; i < SCpnt->use_sg; i++)
			size += sg[i].length;

		if (size != SCpnt->request_bufflen && SCpnt->use_sg) {
			printk(KERN_ERR "sr: mismatch count %d, bytes %d\n",
					size, SCpnt->request_bufflen);
			if (SCpnt->request_bufflen > size)
				SCpnt->request_bufflen = SCpnt->bufflen = size;
		}
	}

	/*
	 * request doesn't start on hw block boundary, add scatter pads
	 */
	if (((unsigned int)SCpnt->request->sector % (s_size >> 9)) ||
	    (SCpnt->request_bufflen % s_size)) {
		printk("sr: unaligned transfer\n");
		return 0;
	}

	this_count = (SCpnt->request_bufflen >> 9) / (s_size >> 9);


	SCSI_LOG_HLQUEUE(2, printk("%s : %s %d/%ld 512 byte blocks.\n",
				cd->cdi.name,
				(rq_data_dir(SCpnt->request) == WRITE) ?
					"writing" : "reading",
				this_count, SCpnt->request->nr_sectors));

	SCpnt->cmnd[1] = 0;
	block = (unsigned int)SCpnt->request->sector / (s_size >> 9);

	if (this_count > 0xffff) {
		this_count = 0xffff;
		SCpnt->request_bufflen = SCpnt->bufflen =
				this_count * s_size;
	}

	SCpnt->cmnd[2] = (unsigned char) (block >> 24) & 0xff;
	SCpnt->cmnd[3] = (unsigned char) (block >> 16) & 0xff;
	SCpnt->cmnd[4] = (unsigned char) (block >> 8) & 0xff;
	SCpnt->cmnd[5] = (unsigned char) block & 0xff;
	SCpnt->cmnd[6] = SCpnt->cmnd[9] = 0;
	SCpnt->cmnd[7] = (unsigned char) (this_count >> 8) & 0xff;
	SCpnt->cmnd[8] = (unsigned char) this_count & 0xff;

	/*
	 * We shouldn't disconnect in the middle of a sector, so with a dumb
	 * host adapter, it's safe to assume that we can at least transfer
	 * this many bytes between each connect / disconnect.
	 */
	SCpnt->transfersize = cd->device->sector_size;
	SCpnt->underflow = this_count << 9;

queue:
	SCpnt->allowed = MAX_RETRIES;
	SCpnt->timeout_per_command = timeout;

	/*
	 * This is the completion routine we use.  This is matched in terms
	 * of capability to this function.
	 */
	SCpnt->done = rw_intr;

	/*
	 * This indicates that the command is ready from our end to be
	 * queued.
	 */
	return 1;
}

static int sr_block_open(struct inode *inode, struct file *file)
{
	struct gendisk *disk = inode->i_bdev->bd_disk;
	struct scsi_cd *cd = scsi_cd(inode->i_bdev->bd_disk);
	int ret = 0;

	if(!(cd = scsi_cd_get(disk)))
		return -ENXIO;

	if((ret = cdrom_open(&cd->cdi, inode, file)) != 0)
		scsi_cd_put(cd);

	return ret;
}

static int sr_block_release(struct inode *inode, struct file *file)
{
	int ret;
	struct scsi_cd *cd = scsi_cd(inode->i_bdev->bd_disk);
	ret = cdrom_release(&cd->cdi, file);
	if(ret)
		return ret;
	
	scsi_cd_put(cd);

	return 0;
}

static int sr_block_ioctl(struct inode *inode, struct file *file, unsigned cmd,
			  unsigned long arg)
{
	struct scsi_cd *cd = scsi_cd(inode->i_bdev->bd_disk);
	struct scsi_device *sdev = cd->device;

        /*
         * Send SCSI addressing ioctls directly to mid level, send other
         * ioctls to cdrom/block level.
         */
        switch (cmd) {
                case SCSI_IOCTL_GET_IDLUN:
                case SCSI_IOCTL_GET_BUS_NUMBER:
                        return scsi_ioctl(sdev, cmd, (void __user *)arg);
	}
	return cdrom_ioctl(file, &cd->cdi, inode, cmd, arg);
}

static int sr_block_media_changed(struct gendisk *disk)
{
	struct scsi_cd *cd = scsi_cd(disk);
	return cdrom_media_changed(&cd->cdi);
}

struct block_device_operations sr_bdops =
{
	.owner		= THIS_MODULE,
	.open		= sr_block_open,
	.release	= sr_block_release,
	.ioctl		= sr_block_ioctl,
	.media_changed	= sr_block_media_changed,
};

static int sr_open(struct cdrom_device_info *cdi, int purpose)
{
	struct scsi_cd *cd = cdi->handle;
	struct scsi_device *sdev = cd->device;
	int retval;

	/*
	 * If the device is in error recovery, wait until it is done.
	 * If the device is offline, then disallow any access to it.
	 */
	retval = -ENXIO;
	if (!scsi_block_when_processing_errors(sdev))
		goto error_out;

	/*
	 * If this device did not have media in the drive at boot time, then
	 * we would have been unable to get the sector size.  Check to see if
	 * this is the case, and try again.
	 */
	if (cd->needs_sector_size)
		get_sectorsize(cd);
	return 0;

error_out:
	scsi_cd_put(cd);
	return retval;	
}

static void sr_release(struct cdrom_device_info *cdi)
{
	struct scsi_cd *cd = cdi->handle;

	if (cd->device->sector_size > 2048)
		sr_set_blocklength(cd, 2048);

}

static int sr_probe(struct device *dev)
{
	struct scsi_device *sdev = to_scsi_device(dev);
	struct gendisk *disk;
	struct scsi_cd *cd;
	int minor, error;

	error = -ENODEV;
	if (sdev->type != TYPE_ROM && sdev->type != TYPE_WORM)
		goto fail;

	error = -ENOMEM;
	cd = kmalloc(sizeof(*cd), GFP_KERNEL);
	if (!cd)
		goto fail;
	memset(cd, 0, sizeof(*cd));

	kref_init(&cd->kref, sr_kref_release);

	disk = alloc_disk(1);
	if (!disk)
		goto fail_free;

	spin_lock(&sr_index_lock);
	minor = find_first_zero_bit(sr_index_bits, SR_DISKS);
	if (minor == SR_DISKS) {
		spin_unlock(&sr_index_lock);
		error = -EBUSY;
		goto fail_put;
	}
	__set_bit(minor, sr_index_bits);
	spin_unlock(&sr_index_lock);

	disk->major = SCSI_CDROM_MAJOR;
	disk->first_minor = minor;
	sprintf(disk->disk_name, "sr%d", minor);
	disk->fops = &sr_bdops;
	disk->flags = GENHD_FL_CD;

	cd->device = sdev;
	cd->disk = disk;
	cd->driver = &sr_template;
	cd->disk = disk;
	cd->capacity = 0x1fffff;
	cd->needs_sector_size = 1;
	cd->device->changed = 1;	/* force recheck CD type */
	cd->use = 1;
	cd->readcd_known = 0;
	cd->readcd_cdda = 0;

	cd->cdi.ops = &sr_dops;
	cd->cdi.handle = cd;
	cd->cdi.mask = 0;
	cd->cdi.capacity = 1;
	sprintf(cd->cdi.name, "sr%d", minor);

	sdev->sector_size = 2048;	/* A guess, just in case */

	/* FIXME: need to handle a get_capabilities failure properly ?? */
	get_capabilities(cd);
	sr_vendor_init(cd);

	snprintf(disk->devfs_name, sizeof(disk->devfs_name),
			"%s/cd", sdev->devfs_name);
	disk->driverfs_dev = &sdev->sdev_gendev;
	set_capacity(disk, cd->capacity);
	disk->private_data = &cd->driver;
	disk->queue = sdev->request_queue;
	cd->cdi.disk = disk;

	if (register_cdrom(&cd->cdi))
		goto fail_put;

	dev_set_drvdata(dev, cd);
	disk->flags |= GENHD_FL_REMOVABLE;
	add_disk(disk);

	printk(KERN_DEBUG
	    "Attached scsi CD-ROM %s at scsi%d, channel %d, id %d, lun %d\n",
	    cd->cdi.name, sdev->host->host_no, sdev->channel,
	    sdev->id, sdev->lun);
	return 0;

fail_put:
	put_disk(disk);
fail_free:
	kfree(cd);
fail:
	return error;
}


static void get_sectorsize(struct scsi_cd *cd)
{
	unsigned char cmd[10];
	unsigned char *buffer;
	int the_result, retries = 3;
	int sector_size;
	struct scsi_request *SRpnt = NULL;
	request_queue_t *queue;

	buffer = kmalloc(512, GFP_KERNEL | GFP_DMA);
	if (!buffer)
		goto Enomem;
	SRpnt = scsi_allocate_request(cd->device, GFP_KERNEL);
	if (!SRpnt)
		goto Enomem;

	do {
		cmd[0] = READ_CAPACITY;
		memset((void *) &cmd[1], 0, 9);
		/* Mark as really busy */
		SRpnt->sr_request->rq_status = RQ_SCSI_BUSY;
		SRpnt->sr_cmd_len = 0;

		memset(buffer, 0, 8);

		/* Do the command and wait.. */
		SRpnt->sr_data_direction = DMA_FROM_DEVICE;
		scsi_wait_req(SRpnt, (void *) cmd, (void *) buffer,
			      8, SR_TIMEOUT, MAX_RETRIES);

		the_result = SRpnt->sr_result;
		retries--;

	} while (the_result && retries);


	scsi_release_request(SRpnt);
	SRpnt = NULL;

	if (the_result) {
		cd->capacity = 0x1fffff;
		sector_size = 2048;	/* A guess, just in case */
		cd->needs_sector_size = 1;
	} else {
#if 0
		if (cdrom_get_last_written(&cd->cdi,
					   &cd->capacity))
#endif
			cd->capacity = 1 + ((buffer[0] << 24) |
						    (buffer[1] << 16) |
						    (buffer[2] << 8) |
						    buffer[3]);
		sector_size = (buffer[4] << 24) |
		    (buffer[5] << 16) | (buffer[6] << 8) | buffer[7];
		switch (sector_size) {
			/*
			 * HP 4020i CD-Recorder reports 2340 byte sectors
			 * Philips CD-Writers report 2352 byte sectors
			 *
			 * Use 2k sectors for them..
			 */
		case 0:
		case 2340:
		case 2352:
			sector_size = 2048;
			/* fall through */
		case 2048:
			cd->capacity *= 4;
			/* fall through */
		case 512:
			break;
		default:
			printk("%s: unsupported sector size %d.\n",
			       cd->cdi.name, sector_size);
			cd->capacity = 0;
			cd->needs_sector_size = 1;
		}

		cd->device->sector_size = sector_size;

		/*
		 * Add this so that we have the ability to correctly gauge
		 * what the device is capable of.
		 */
		cd->needs_sector_size = 0;
		set_capacity(cd->disk, cd->capacity);
	}

	queue = cd->device->request_queue;
	blk_queue_hardsect_size(queue, sector_size);
out:
	kfree(buffer);
	return;

Enomem:
	cd->capacity = 0x1fffff;
	sector_size = 2048;	/* A guess, just in case */
	cd->needs_sector_size = 1;
	if (SRpnt)
		scsi_release_request(SRpnt);
	goto out;
}

static void get_capabilities(struct scsi_cd *cd)
{
	unsigned char *buffer;
	struct scsi_mode_data data;
	struct scsi_request *SRpnt;
	unsigned char cmd[MAX_COMMAND_SIZE];
	unsigned int the_result;
	int retries, rc, n;

	static char *loadmech[] =
	{
		"caddy",
		"tray",
		"pop-up",
		"",
		"changer",
		"cartridge changer",
		"",
		""
	};

	/* allocate a request for the TEST_UNIT_READY */
	SRpnt = scsi_allocate_request(cd->device, GFP_KERNEL);
	if (!SRpnt) {
		printk(KERN_WARNING "(get_capabilities:) Request allocation "
		       "failure.\n");
		return;
	}

	/* allocate transfer buffer */
	buffer = kmalloc(512, GFP_KERNEL | GFP_DMA);
	if (!buffer) {
		printk(KERN_ERR "sr: out of memory.\n");
		scsi_release_request(SRpnt);
		return;
	}

	/* issue TEST_UNIT_READY until the initial startup UNIT_ATTENTION
	 * conditions are gone, or a timeout happens
	 */
	retries = 0;
	do {
		memset((void *)cmd, 0, MAX_COMMAND_SIZE);
		cmd[0] = TEST_UNIT_READY;

		SRpnt->sr_cmd_len = 0;
		SRpnt->sr_sense_buffer[0] = 0;
		SRpnt->sr_sense_buffer[2] = 0;
		SRpnt->sr_data_direction = DMA_NONE;

		scsi_wait_req (SRpnt, (void *) cmd, buffer,
			       0, SR_TIMEOUT, MAX_RETRIES);

		the_result = SRpnt->sr_result;
		retries++;
	} while (retries < 5 && 
		 (!scsi_status_is_good(the_result) ||
		  ((driver_byte(the_result) & DRIVER_SENSE) &&
		   SRpnt->sr_sense_buffer[2] == UNIT_ATTENTION)));

	/* ask for mode page 0x2a */
	rc = scsi_mode_sense(cd->device, 0, 0x2a, buffer, 128,
			     SR_TIMEOUT, 3, &data);

	if (!scsi_status_is_good(rc)) {
		/* failed, drive doesn't have capabilities mode page */
		cd->cdi.speed = 1;
		cd->cdi.mask |= (CDC_CD_R | CDC_CD_RW | CDC_DVD_R |
					 CDC_DVD | CDC_DVD_RAM |
					 CDC_SELECT_DISC | CDC_SELECT_SPEED);
		scsi_release_request(SRpnt);
		kfree(buffer);
		printk("%s: scsi-1 drive\n", cd->cdi.name);
		return;
	}

	n = data.header_length + data.block_descriptor_length;
	cd->cdi.speed = ((buffer[n + 8] << 8) + buffer[n + 9]) / 176;
	cd->readcd_known = 1;
	cd->readcd_cdda = buffer[n + 5] & 0x01;
	/* print some capability bits */
	printk("%s: scsi3-mmc drive: %dx/%dx %s%s%s%s%s%s\n", cd->cdi.name,
	       ((buffer[n + 14] << 8) + buffer[n + 15]) / 176,
	       cd->cdi.speed,
	       buffer[n + 3] & 0x01 ? "writer " : "", /* CD Writer */
	       buffer[n + 3] & 0x20 ? "dvd-ram " : "",
	       buffer[n + 2] & 0x02 ? "cd/rw " : "", /* can read rewriteable */
	       buffer[n + 4] & 0x20 ? "xa/form2 " : "",	/* can read xa/from2 */
	       buffer[n + 5] & 0x01 ? "cdda " : "", /* can read audio data */
	       loadmech[buffer[n + 6] >> 5]);
	if ((buffer[n + 6] >> 5) == 0)
		/* caddy drives can't close tray... */
		cd->cdi.mask |= CDC_CLOSE_TRAY;
	if ((buffer[n + 2] & 0x8) == 0)
		/* not a DVD drive */
		cd->cdi.mask |= CDC_DVD;
	if ((buffer[n + 3] & 0x20) == 0) 
		/* can't write DVD-RAM media */
		cd->cdi.mask |= CDC_DVD_RAM;
	if ((buffer[n + 3] & 0x10) == 0)
		/* can't write DVD-R media */
		cd->cdi.mask |= CDC_DVD_R;
	if ((buffer[n + 3] & 0x2) == 0)
		/* can't write CD-RW media */
		cd->cdi.mask |= CDC_CD_RW;
	if ((buffer[n + 3] & 0x1) == 0)
		/* can't write CD-R media */
		cd->cdi.mask |= CDC_CD_R;
	if ((buffer[n + 6] & 0x8) == 0)
		/* can't eject */
		cd->cdi.mask |= CDC_OPEN_TRAY;

	if ((buffer[n + 6] >> 5) == mechtype_individual_changer ||
	    (buffer[n + 6] >> 5) == mechtype_cartridge_changer)
		cd->cdi.capacity =
		    cdrom_number_of_slots(&cd->cdi);
	if (cd->cdi.capacity <= 1)
		/* not a changer */
		cd->cdi.mask |= CDC_SELECT_DISC;
	/*else    I don't think it can close its tray
		cd->cdi.mask |= CDC_CLOSE_TRAY; */

	/*
	 * if DVD-RAM of MRW-W, we are randomly writeable
	 */
	if ((cd->cdi.mask & (CDC_DVD_RAM | CDC_MRW_W | CDC_RAM)) !=
			(CDC_DVD_RAM | CDC_MRW_W | CDC_RAM)) {
		cd->device->writeable = 1;
	}

	scsi_release_request(SRpnt);
	kfree(buffer);
}

/*
 * sr_packet() is the entry point for the generic commands generated
 * by the Uniform CD-ROM layer. 
 */
static int sr_packet(struct cdrom_device_info *cdi,
		struct packet_command *cgc)
{
	if (cgc->timeout <= 0)
		cgc->timeout = IOCTL_TIMEOUT;

	sr_do_ioctl(cdi->handle, cgc);

	return cgc->stat;
}

/**
 *	sr_kref_release - Called to free the scsi_cd structure
 *	@kref: pointer to embedded kref
 *
 *	sr_ref_sem must be held entering this routine.  Because it is
 *	called on last put, you should always use the scsi_cd_get()
 *	scsi_cd_put() helpers which manipulate the semaphore directly
 *	and never do a direct kref_put().
 **/
static void sr_kref_release(struct kref *kref)
{
	struct scsi_cd *cd = container_of(kref, struct scsi_cd, kref);
	struct gendisk *disk = cd->disk;

	spin_lock(&sr_index_lock);
	clear_bit(disk->first_minor, sr_index_bits);
	spin_unlock(&sr_index_lock);

	unregister_cdrom(&cd->cdi);

	disk->private_data = NULL;

	put_disk(disk);

	kfree(cd);
}

static int sr_remove(struct device *dev)
{
	struct scsi_cd *cd = dev_get_drvdata(dev);

	del_gendisk(cd->disk);

	down(&sr_ref_sem);
	kref_put(&cd->kref);
	up(&sr_ref_sem);

	return 0;
}

static int __init init_sr(void)
{
	int rc;

	rc = register_blkdev(SCSI_CDROM_MAJOR, "sr");
	if (rc)
		return rc;
	return scsi_register_driver(&sr_template.gendrv);
}

static void __exit exit_sr(void)
{
	scsi_unregister_driver(&sr_template.gendrv);
	unregister_blkdev(SCSI_CDROM_MAJOR, "sr");
}

module_init(init_sr);
module_exit(exit_sr);
MODULE_LICENSE("GPL");
