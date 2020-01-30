/*
 *  drivers/s390/char/tape_block.c
 *    block device frontend for tape device driver
 *
 *  S390 and zSeries version
 *    Copyright (C) 2001,2003 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Carsten Otte <cotte@de.ibm.com>
 *		 Tuan Ngo-Anh <ngoanh@de.ibm.com>
 *		 Martin Schwidefsky <schwidefsky@de.ibm.com>
 *		 Stefan Bader <shbader@de.ibm.com>
 */

#include <linux/fs.h>
#include <linux/config.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/interrupt.h>
#include <linux/buffer_head.h>

#include <asm/debug.h>

#define TAPE_DBF_AREA	tape_core_dbf

#include "tape.h"

#define PRINTK_HEADER "TAPE_BLOCK: "

#define TAPEBLOCK_MAX_SEC	100
#define TAPEBLOCK_MIN_REQUEUE	3

/*
 * 2003/11/25  Stefan Bader <shbader@de.ibm.com>
 *
 * In 2.5/2.6 the block device request function is very likely to be called
 * with disabled interrupts (e.g. generic_unplug_device). So the driver can't
 * just call any function that tries to allocate CCW requests from that con-
 * text since it might sleep. There are two choices to work around this:
 *	a) do not allocate with kmalloc but use its own memory pool
 *      b) take requests from the queue outside that context, knowing that
 *         allocation might sleep
 */

/*
 * file operation structure for tape block frontend
 */
static int tapeblock_open(struct inode *, struct file *);
static int tapeblock_release(struct inode *, struct file *);
static int tapeblock_ioctl(struct inode *, struct file *, unsigned int,
				unsigned long);
static int tapeblock_medium_changed(struct gendisk *);
static int tapeblock_revalidate_disk(struct gendisk *);

static struct block_device_operations tapeblock_fops = {
	.owner		 = THIS_MODULE,
	.open		 = tapeblock_open,
	.release	 = tapeblock_release,
	.ioctl           = tapeblock_ioctl,
	.media_changed   = tapeblock_medium_changed,
	.revalidate_disk = tapeblock_revalidate_disk,
};

static int tapeblock_major = 0;

static void
tapeblock_trigger_requeue(struct tape_device *device)
{
	/* Protect against rescheduling. */
	if (atomic_compare_and_swap(0, 1, &device->blk_data.requeue_scheduled))
		return;
	schedule_work(&device->blk_data.requeue_task);
}

/*
 * Post finished request.
 */
static inline void
tapeblock_end_request(struct request *req, int uptodate)
{
	if (end_that_request_first(req, uptodate, req->hard_nr_sectors))
		BUG();
	end_that_request_last(req);
}

static void
__tapeblock_end_request(struct tape_request *ccw_req, void *data)
{
	struct tape_device *device;
	struct request *req;

	DBF_LH(6, "__tapeblock_end_request()\n");

	device = ccw_req->device;
	req = (struct request *) data;
	tapeblock_end_request(req, ccw_req->rc == 0);
	if (ccw_req->rc == 0)
		/* Update position. */
		device->blk_data.block_position =
			(req->sector + req->nr_sectors) >> TAPEBLOCK_HSEC_S2B;
	else
		/* We lost the position information due to an error. */
		device->blk_data.block_position = -1;
	device->discipline->free_bread(ccw_req);
	if (!list_empty(&device->req_queue) ||
	    elv_next_request(device->blk_data.request_queue))
		tapeblock_trigger_requeue(device);
}

/*
 * Feed the tape device CCW queue with requests supplied in a list.
 */
static inline int
tapeblock_start_request(struct tape_device *device, struct request *req)
{
	struct tape_request *	ccw_req;
	int			rc;

	DBF_LH(6, "tapeblock_start_request(%p, %p)\n", device, req);

	ccw_req = device->discipline->bread(device, req);
	if (IS_ERR(ccw_req)) {
		DBF_EVENT(1, "TBLOCK: bread failed\n");
		tapeblock_end_request(req, 0);
		return PTR_ERR(ccw_req);
	}
	ccw_req->callback = __tapeblock_end_request;
	ccw_req->callback_data = (void *) req;
	ccw_req->retries = TAPEBLOCK_RETRIES;

	rc = tape_do_io_async(device, ccw_req);
	if (rc) {
		/*
		 * Start/enqueueing failed. No retries in
		 * this case.
		 */
		tapeblock_end_request(req, 0);
		device->discipline->free_bread(ccw_req);
	}

	return rc;
}

/*
 * Move requests from the block device request queue to the tape device ccw
 * queue.
 */
static void
tapeblock_requeue(void *data) {
	struct tape_device *	device;
	request_queue_t *	queue;
	int			nr_queued;
	struct request *	req;
	struct list_head *	l;
	int			rc;

	device = (struct tape_device *) data;
	if (!device)
		return;

	spin_lock_irq(get_ccwdev_lock(device->cdev));
	queue  = device->blk_data.request_queue;

	/* Count number of requests on ccw queue. */
	nr_queued = 0;
	list_for_each(l, &device->req_queue)
		nr_queued++;
	spin_unlock(get_ccwdev_lock(device->cdev));

	spin_lock(&device->blk_data.request_queue_lock);
	while (
		!blk_queue_plugged(queue) &&
		elv_next_request(queue)   &&
		nr_queued < TAPEBLOCK_MIN_REQUEUE
	) {
		req = elv_next_request(queue);
		if (rq_data_dir(req) == WRITE) {
			DBF_EVENT(1, "TBLOCK: Rejecting write request\n");
			blkdev_dequeue_request(req);
			tapeblock_end_request(req, 0);
			continue;
		}
		spin_unlock_irq(&device->blk_data.request_queue_lock);
		rc = tapeblock_start_request(device, req);
		spin_lock_irq(&device->blk_data.request_queue_lock);
		blkdev_dequeue_request(req);
		nr_queued++;
	}
	spin_unlock_irq(&device->blk_data.request_queue_lock);
	atomic_set(&device->blk_data.requeue_scheduled, 0);
}

/*
 * Tape request queue function. Called from ll_rw_blk.c
 */
static void
tapeblock_request_fn(request_queue_t *queue)
{
	struct tape_device *device;

	device = (struct tape_device *) queue->queuedata;
	DBF_LH(6, "tapeblock_request_fn(device=%p)\n", device);
	if (device == NULL)
		BUG();

	tapeblock_trigger_requeue(device);
}

/*
 * This function is called for every new tapedevice
 */
int
tapeblock_setup_device(struct tape_device * device)
{
	struct tape_blk_data *	blkdat;
	struct gendisk *	disk;
	int			rc;

	blkdat = &device->blk_data;
	spin_lock_init(&blkdat->request_queue_lock);
	atomic_set(&blkdat->requeue_scheduled, 0);

	blkdat->request_queue = blk_init_queue(
		tapeblock_request_fn,
		&blkdat->request_queue_lock
	);
	if (!blkdat->request_queue)
		return -ENOMEM;

	elevator_exit(blkdat->request_queue);
	rc = elevator_init(blkdat->request_queue, &elevator_noop);
	if (rc)
		goto cleanup_queue;

	blk_queue_hardsect_size(blkdat->request_queue, TAPEBLOCK_HSEC_SIZE);
	blk_queue_max_sectors(blkdat->request_queue, TAPEBLOCK_MAX_SEC);
	blk_queue_max_phys_segments(blkdat->request_queue, -1L);
	blk_queue_max_hw_segments(blkdat->request_queue, -1L);
	blk_queue_max_segment_size(blkdat->request_queue, -1L);
	blk_queue_segment_boundary(blkdat->request_queue, -1L);

	disk = alloc_disk(1);
	if (!disk) {
		rc = -ENOMEM;
		goto cleanup_queue;
	}

	disk->major = tapeblock_major;
	disk->first_minor = device->first_minor;
	disk->fops = &tapeblock_fops;
	disk->private_data = tape_get_device_reference(device);
	disk->queue = blkdat->request_queue;
	set_capacity(disk, 0);
	sprintf(disk->disk_name, "btibm%d",
		device->first_minor / TAPE_MINORS_PER_DEV);

	blkdat->disk = disk;
	blkdat->medium_changed = 1;
	blkdat->request_queue->queuedata = tape_get_device_reference(device);

	add_disk(disk);

	INIT_WORK(&blkdat->requeue_task, tapeblock_requeue,
		tape_get_device_reference(device));

	return 0;

cleanup_queue:
	blk_cleanup_queue(blkdat->request_queue);
	blkdat->request_queue = NULL;

	return rc;
}

void
tapeblock_cleanup_device(struct tape_device *device)
{
	flush_scheduled_work();
	device->blk_data.requeue_task.data = tape_put_device(device);

	if (!device->blk_data.disk) {
		PRINT_ERR("(%s): No gendisk to clean up!\n",
			device->cdev->dev.bus_id);
		goto cleanup_queue;
	}

	del_gendisk(device->blk_data.disk);
	device->blk_data.disk->private_data =
		tape_put_device(device->blk_data.disk->private_data);
	put_disk(device->blk_data.disk);

	device->blk_data.disk = NULL;
cleanup_queue:
	device->blk_data.request_queue->queuedata = tape_put_device(device);

	blk_cleanup_queue(device->blk_data.request_queue);
	device->blk_data.request_queue = NULL;
}

/*
 * Detect number of blocks of the tape.
 * FIXME: can we extent this to detect the blocks size as well ?
 */
static int
tapeblock_revalidate_disk(struct gendisk *disk)
{
	struct tape_device *	device;
	unsigned int		nr_of_blks;
	int			rc;

	device = (struct tape_device *) disk->private_data;
	if (!device)
		BUG();

	if (!device->blk_data.medium_changed)
		return 0;

	PRINT_INFO("Detecting media size...\n");
	rc = tape_mtop(device, MTFSFM, 1);
	if (rc)
		return rc;

	rc = tape_mtop(device, MTTELL, 1);
	if (rc < 0)
		return rc;

	DBF_LH(3, "Image file ends at %d\n", rc);
	nr_of_blks = rc;

	/* This will fail for the first file. Catch the error by checking the
	 * position. */
	tape_mtop(device, MTBSF, 1);

	rc = tape_mtop(device, MTTELL, 1);
	if (rc < 0)
		return rc;

	if (rc > nr_of_blks)
		return -EINVAL;

	DBF_LH(3, "Image file starts at %d\n", rc);
	device->bof = rc;
	nr_of_blks -= rc;

	PRINT_INFO("Found %i blocks on media\n", nr_of_blks);
	set_capacity(device->blk_data.disk,
		nr_of_blks*(TAPEBLOCK_HSEC_SIZE/512));

	device->blk_data.block_position = 0;
	device->blk_data.medium_changed = 0;
	return 0;
}

static int
tapeblock_medium_changed(struct gendisk *disk)
{
	struct tape_device *device;

	device = (struct tape_device *) disk->private_data;
	DBF_LH(6, "tapeblock_medium_changed(%p) = %d\n",
		device, device->blk_data.medium_changed);

	return device->blk_data.medium_changed;
}

/*
 * Block frontend tape device open function.
 */
static int
tapeblock_open(struct inode *inode, struct file *filp)
{
	struct gendisk *	disk;
	struct tape_device *	device;
	int			rc;

	disk   = inode->i_bdev->bd_disk;
	device = tape_get_device_reference(disk->private_data);

	if (device->required_tapemarks) {
		DBF_EVENT(2, "TBLOCK: missing tapemarks\n");
		PRINT_ERR("TBLOCK: Refusing to open tape with missing"
			" end of file marks.\n");
		rc = -EPERM;
		goto put_device;
	}

	rc = tape_open(device);
	if (rc)
		goto put_device;

	rc = tapeblock_revalidate_disk(disk);
	if (rc)
		goto release;

	/*
	 * Note: The reference to <device> is hold until the release function
	 *       is called.
	 */
	tape_state_set(device, TS_BLKUSE);
	return 0;

release:
	tape_release(device);
 put_device:
	tape_put_device(device);
	return rc;
}

/*
 * Block frontend tape device release function.
 *
 * Note: One reference to the tape device was made by the open function. So
 *       we just get the pointer here and release the reference.
 */
static int
tapeblock_release(struct inode *inode, struct file *filp)
{
	struct gendisk *disk = inode->i_bdev->bd_disk;
	struct tape_device *device = disk->private_data;

	tape_state_set(device, TS_IN_USE);
	tape_release(device);
	tape_put_device(device);

	return 0;
}

/*
 * Support of some generic block device IOCTLs.
 */
static int
tapeblock_ioctl(
	struct inode *		inode,
	struct file *		file,
	unsigned int		command,
	unsigned long		arg
) {
	int rc;
	int minor;
	struct gendisk *disk = inode->i_bdev->bd_disk;
	struct tape_device *device = disk->private_data;

	rc     = 0;
	disk   = inode->i_bdev->bd_disk;
	if (!disk)
		BUG();
	device = disk->private_data;
	if (!device)
		BUG();
	minor  = iminor(inode);

	DBF_LH(6, "tapeblock_ioctl(0x%0x)\n", command);
	DBF_LH(6, "device = %d:%d\n", tapeblock_major, minor);

	switch (command) {
		/* Refuse some IOCTL calls without complaining (mount). */
		case 0x5310:		/* CDROMMULTISESSION */
			rc = -EINVAL;
			break;
		default:
			PRINT_WARN("invalid ioctl 0x%x\n", command);
			rc = -EINVAL;
	}

	return rc;
}

/*
 * Initialize block device frontend.
 */
int
tapeblock_init(void)
{
	int rc;

	/* Register the tape major number to the kernel */
	rc = register_blkdev(tapeblock_major, "tBLK");
	if (rc < 0)
		return rc;

	if (tapeblock_major == 0)
		tapeblock_major = rc;
	PRINT_INFO("tape gets major %d for block device\n", tapeblock_major);
	return 0;
}

/*
 * Deregister major for block device frontend
 */
void
tapeblock_exit(void)
{
	unregister_blkdev(tapeblock_major, "tBLK");
}
