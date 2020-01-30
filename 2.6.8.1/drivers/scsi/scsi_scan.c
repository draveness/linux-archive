/*
 * scsi_scan.c
 *
 * Copyright (C) 2000 Eric Youngdale,
 * Copyright (C) 2002 Patrick Mansfield
 *
 * The general scanning/probing algorithm is as follows, exceptions are
 * made to it depending on device specific flags, compilation options, and
 * global variable (boot or module load time) settings.
 *
 * A specific LUN is scanned via an INQUIRY command; if the LUN has a
 * device attached, a Scsi_Device is allocated and setup for it.
 *
 * For every id of every channel on the given host:
 *
 * 	Scan LUN 0; if the target responds to LUN 0 (even if there is no
 * 	device or storage attached to LUN 0):
 *
 * 		If LUN 0 has a device attached, allocate and setup a
 * 		Scsi_Device for it.
 *
 * 		If target is SCSI-3 or up, issue a REPORT LUN, and scan
 * 		all of the LUNs returned by the REPORT LUN; else,
 * 		sequentially scan LUNs up until some maximum is reached,
 * 		or a LUN is seen that cannot have a device attached to it.
 */

#include <linux/config.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <asm/semaphore.h>

#include <scsi/scsi.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_driver.h>
#include <scsi/scsi_devinfo.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_request.h>
#include <scsi/scsi_transport.h>

#include "scsi_priv.h"
#include "scsi_logging.h"

#define ALLOC_FAILURE_MSG	KERN_ERR "%s: Allocation failure during" \
	" SCSI scanning, some SCSI devices might not be configured\n"

/*
 * Default timeout
 */
#define SCSI_TIMEOUT (2*HZ)

/*
 * Prefix values for the SCSI id's (stored in driverfs name field)
 */
#define SCSI_UID_SER_NUM 'S'
#define SCSI_UID_UNKNOWN 'Z'

/*
 * Return values of some of the scanning functions.
 *
 * SCSI_SCAN_NO_RESPONSE: no valid response received from the target, this
 * includes allocation or general failures preventing IO from being sent.
 *
 * SCSI_SCAN_TARGET_PRESENT: target responded, but no device is available
 * on the given LUN.
 *
 * SCSI_SCAN_LUN_PRESENT: target responded, and a device is available on a
 * given LUN.
 */
#define SCSI_SCAN_NO_RESPONSE		0
#define SCSI_SCAN_TARGET_PRESENT	1
#define SCSI_SCAN_LUN_PRESENT		2

static char *scsi_null_device_strs = "nullnullnullnull";

#define MAX_SCSI_LUNS	512

#ifdef CONFIG_SCSI_MULTI_LUN
static unsigned int max_scsi_luns = MAX_SCSI_LUNS;
#else
static unsigned int max_scsi_luns = 1;
#endif

module_param_named(max_luns, max_scsi_luns, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(max_luns,
		 "last scsi LUN (should be between 1 and 2^32-1)");

/*
 * max_scsi_report_luns: the maximum number of LUNS that will be
 * returned from the REPORT LUNS command. 8 times this value must
 * be allocated. In theory this could be up to an 8 byte value, but
 * in practice, the maximum number of LUNs suppored by any device
 * is about 16k.
 */
static unsigned int max_scsi_report_luns = 511;

module_param_named(max_report_luns, max_scsi_report_luns, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(max_report_luns,
		 "REPORT LUNS maximum number of LUNS received (should be"
		 " between 1 and 16384)");

static unsigned int scsi_inq_timeout = SCSI_TIMEOUT/HZ+3;

module_param_named(inq_timeout, scsi_inq_timeout, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(inq_timeout, 
		 "Timeout (in seconds) waiting for devices to answer INQUIRY."
		 " Default is 5. Some non-compliant devices need more.");

/**
 * scsi_unlock_floptical - unlock device via a special MODE SENSE command
 * @sreq:	used to send the command
 * @result:	area to store the result of the MODE SENSE
 *
 * Description:
 *     Send a vendor specific MODE SENSE (not a MODE SELECT) command using
 *     @sreq to unlock a device, storing the (unused) results into result.
 *     Called for BLIST_KEY devices.
 **/
static void scsi_unlock_floptical(struct scsi_request *sreq,
				  unsigned char *result)
{
	unsigned char scsi_cmd[MAX_COMMAND_SIZE];

	printk(KERN_NOTICE "scsi: unlocking floptical drive\n");
	scsi_cmd[0] = MODE_SENSE;
	scsi_cmd[1] = 0;
	scsi_cmd[2] = 0x2e;
	scsi_cmd[3] = 0;
	scsi_cmd[4] = 0x2a;	/* size */
	scsi_cmd[5] = 0;
	sreq->sr_cmd_len = 0;
	sreq->sr_data_direction = DMA_FROM_DEVICE;
	scsi_wait_req(sreq, scsi_cmd, result, 0x2a /* size */, SCSI_TIMEOUT, 3);
}

/**
 * print_inquiry - printk the inquiry information
 * @inq_result:	printk this SCSI INQUIRY
 *
 * Description:
 *     printk the vendor, model, and other information found in the
 *     INQUIRY data in @inq_result.
 *
 * Notes:
 *     Remove this, and replace with a hotplug event that logs any
 *     relevant information.
 **/
static void print_inquiry(unsigned char *inq_result)
{
	int i;

	printk(KERN_NOTICE "  Vendor: ");
	for (i = 8; i < 16; i++)
		if (inq_result[i] >= 0x20 && i < inq_result[4] + 5)
			printk("%c", inq_result[i]);
		else
			printk(" ");

	printk("  Model: ");
	for (i = 16; i < 32; i++)
		if (inq_result[i] >= 0x20 && i < inq_result[4] + 5)
			printk("%c", inq_result[i]);
		else
			printk(" ");

	printk("  Rev: ");
	for (i = 32; i < 36; i++)
		if (inq_result[i] >= 0x20 && i < inq_result[4] + 5)
			printk("%c", inq_result[i]);
		else
			printk(" ");

	printk("\n");

	i = inq_result[0] & 0x1f;

	printk(KERN_NOTICE "  Type:   %s ",
	       i <
	       MAX_SCSI_DEVICE_CODE ? scsi_device_types[i] :
	       "Unknown          ");
	printk("                 ANSI SCSI revision: %02x",
	       inq_result[2] & 0x07);
	if ((inq_result[2] & 0x07) == 1 && (inq_result[3] & 0x0f) == 1)
		printk(" CCS\n");
	else
		printk("\n");
}

/**
 * scsi_alloc_sdev - allocate and setup a scsi_Device
 *
 * Description:
 *     Allocate, initialize for io, and return a pointer to a scsi_Device.
 *     Stores the @shost, @channel, @id, and @lun in the scsi_Device, and
 *     adds scsi_Device to the appropriate list.
 *
 * Return value:
 *     scsi_Device pointer, or NULL on failure.
 **/
static struct scsi_device *scsi_alloc_sdev(struct Scsi_Host *shost,
	       	uint channel, uint id, uint lun)
{
	struct scsi_device *sdev, *device;
	unsigned long flags;

	sdev = kmalloc(sizeof(*sdev) + shost->transportt->size, GFP_ATOMIC);
	if (!sdev)
		goto out;

	memset(sdev, 0, sizeof(*sdev));
	sdev->vendor = scsi_null_device_strs;
	sdev->model = scsi_null_device_strs;
	sdev->rev = scsi_null_device_strs;
	sdev->host = shost;
	sdev->id = id;
	sdev->lun = lun;
	sdev->channel = channel;
	sdev->sdev_state = SDEV_CREATED;
	INIT_LIST_HEAD(&sdev->siblings);
	INIT_LIST_HEAD(&sdev->same_target_siblings);
	INIT_LIST_HEAD(&sdev->cmd_list);
	INIT_LIST_HEAD(&sdev->starved_entry);
	spin_lock_init(&sdev->list_lock);


	/* if the device needs this changing, it may do so in the
	 * slave_configure function */
	sdev->max_device_blocked = SCSI_DEFAULT_DEVICE_BLOCKED;

	/*
	 * Some low level driver could use device->type
	 */
	sdev->type = -1;

	/*
	 * Assume that the device will have handshaking problems,
	 * and then fix this field later if it turns out it
	 * doesn't
	 */
	sdev->borken = 1;

	spin_lock_init(&sdev->sdev_lock);
	sdev->request_queue = scsi_alloc_queue(sdev);
	if (!sdev->request_queue)
		goto out_free_dev;

	sdev->request_queue->queuedata = sdev;
	scsi_adjust_queue_depth(sdev, 0, sdev->host->cmd_per_lun);

	if (shost->hostt->slave_alloc) {
		if (shost->hostt->slave_alloc(sdev))
			goto out_free_queue;
	}

	if (shost->transportt->setup) {
		if (shost->transportt->setup(sdev))
			goto out_cleanup_slave;
	}

	if (get_device(&sdev->host->shost_gendev)) {

		device_initialize(&sdev->sdev_gendev);
		sdev->sdev_gendev.parent = &sdev->host->shost_gendev;
		sdev->sdev_gendev.bus = &scsi_bus_type;
		sdev->sdev_gendev.release = scsi_device_dev_release;
		sprintf(sdev->sdev_gendev.bus_id,"%d:%d:%d:%d",
			sdev->host->host_no, sdev->channel, sdev->id,
			sdev->lun);

		class_device_initialize(&sdev->sdev_classdev);
		sdev->sdev_classdev.dev = &sdev->sdev_gendev;
		sdev->sdev_classdev.class = &sdev_class;
		snprintf(sdev->sdev_classdev.class_id, BUS_ID_SIZE,
			 "%d:%d:%d:%d", sdev->host->host_no,
			 sdev->channel, sdev->id, sdev->lun);

		class_device_initialize(&sdev->transport_classdev);
		sdev->transport_classdev.dev = &sdev->sdev_gendev;
		sdev->transport_classdev.class = sdev->host->transportt->class;
		snprintf(sdev->transport_classdev.class_id, BUS_ID_SIZE,
			 "%d:%d:%d:%d", sdev->host->host_no,
			 sdev->channel, sdev->id, sdev->lun);
	} else
		goto out_cleanup_transport;

	/*
	 * If there are any same target siblings, add this to the
	 * sibling list
	 */
	spin_lock_irqsave(shost->host_lock, flags);
	list_for_each_entry(device, &shost->__devices, siblings) {
		if (device->id == sdev->id &&
		    device->channel == sdev->channel) {
			list_add_tail(&sdev->same_target_siblings,
				      &device->same_target_siblings);
			sdev->scsi_level = device->scsi_level;
			break;
		}
	}

	/*
	 * If there wasn't another lun already configured at this
	 * target, then default this device to SCSI_2 until we
	 * know better
	 */
	if (!sdev->scsi_level)
		sdev->scsi_level = SCSI_2;

	list_add_tail(&sdev->siblings, &shost->__devices);
	spin_unlock_irqrestore(shost->host_lock, flags);
	return sdev;

out_cleanup_transport:
	if (shost->transportt->cleanup)
		shost->transportt->cleanup(sdev);
out_cleanup_slave:
	if (shost->hostt->slave_destroy)
		shost->hostt->slave_destroy(sdev);
out_free_queue:
	scsi_free_queue(sdev->request_queue);
out_free_dev:
	kfree(sdev);
out:
	printk(ALLOC_FAILURE_MSG, __FUNCTION__);
	return NULL;
}

/**
 * scsi_probe_lun - probe a single LUN using a SCSI INQUIRY
 * @sreq:	used to send the INQUIRY
 * @inq_result:	area to store the INQUIRY result
 * @bflags:	store any bflags found here
 *
 * Description:
 *     Probe the lun associated with @sreq using a standard SCSI INQUIRY;
 *
 *     If the INQUIRY is successful, sreq->sr_result is zero and: the
 *     INQUIRY data is in @inq_result; the scsi_level and INQUIRY length
 *     are copied to the Scsi_Device at @sreq->sr_device (sdev);
 *     any flags value is stored in *@bflags.
 **/
static void scsi_probe_lun(struct scsi_request *sreq, char *inq_result,
			   int *bflags)
{
	struct scsi_device *sdev = sreq->sr_device;	/* a bit ugly */
	unsigned char scsi_cmd[MAX_COMMAND_SIZE];
	int possible_inq_resp_len;
	int count = 0;

	*bflags = 0;
 repeat_inquiry:
	SCSI_LOG_SCAN_BUS(3, printk(KERN_INFO "scsi scan: INQUIRY to host %d"
			" channel %d id %d lun %d\n", sdev->host->host_no,
			sdev->channel, sdev->id, sdev->lun));

	memset(scsi_cmd, 0, 6);
	scsi_cmd[0] = INQUIRY;
	scsi_cmd[4] = 36;	/* issue conservative alloc_length */
	sreq->sr_cmd_len = 0;
	sreq->sr_data_direction = DMA_FROM_DEVICE;

	memset(inq_result, 0, 36);
	scsi_wait_req(sreq, (void *) scsi_cmd, (void *) inq_result, 36,
		      HZ/2 + HZ*scsi_inq_timeout, 3);

	SCSI_LOG_SCAN_BUS(3, printk(KERN_INFO "scsi scan: 1st INQUIRY %s with"
			" code 0x%x\n", sreq->sr_result ?
			"failed" : "successful", sreq->sr_result));
	++count;

	if (sreq->sr_result) {
		if ((driver_byte(sreq->sr_result) & DRIVER_SENSE) != 0 &&
		    (sreq->sr_sense_buffer[2] & 0xf) == UNIT_ATTENTION &&
		    (sreq->sr_sense_buffer[12] == 0x28 ||
		     sreq->sr_sense_buffer[12] == 0x29) &&
		    sreq->sr_sense_buffer[13] == 0) {
			/* not-ready to ready transition or power-on - good */
			/* dpg: bogus? INQUIRY never returns UNIT_ATTENTION */
			/* Supposedly, but many buggy devices do so anyway */
			if (count < 3)
				goto repeat_inquiry;
		}
		/*
		 * assume no peripheral if any other sort of error
		 */
		return;
	}

	/*
	 * Get any flags for this device.
	 *
	 * XXX add a bflags to Scsi_Device, and replace the corresponding
	 * bit fields in Scsi_Device, so bflags need not be passed as an
	 * argument.
	 */
	*bflags |= scsi_get_device_flags(sdev, &inq_result[8], &inq_result[16]);

	possible_inq_resp_len = (unsigned char) inq_result[4] + 5;
	if (BLIST_INQUIRY_36 & *bflags)
		possible_inq_resp_len = 36;
	else if (BLIST_INQUIRY_58 & *bflags)
		possible_inq_resp_len = 58;
	else if (possible_inq_resp_len > 255)
		possible_inq_resp_len = 36;	/* sanity */

	if (possible_inq_resp_len > 36) {	/* do additional INQUIRY */
		memset(scsi_cmd, 0, 6);
		scsi_cmd[0] = INQUIRY;
		scsi_cmd[4] = (unsigned char) possible_inq_resp_len;
		sreq->sr_cmd_len = 0;
		sreq->sr_data_direction = DMA_FROM_DEVICE;
		/*
		 * re-zero inq_result just to be safe.
		 */
		memset(inq_result, 0, possible_inq_resp_len);
		scsi_wait_req(sreq, (void *) scsi_cmd,
			      (void *) inq_result,
			      possible_inq_resp_len, (1+scsi_inq_timeout)*(HZ/2), 3);
		SCSI_LOG_SCAN_BUS(3, printk(KERN_INFO "scsi scan: 2nd INQUIRY"
				" %s with code 0x%x\n", sreq->sr_result ?
				"failed" : "successful", sreq->sr_result));
		if (sreq->sr_result) {
			/* if the longer inquiry has failed, flag the device
			 * as only accepting 36 byte inquiries and retry the
			 * 36 byte inquiry */
			printk(KERN_INFO "scsi scan: %d byte inquiry failed"
			       " with code %d.  Consider BLIST_INQUIRY_36 for"
			       " this device\n", possible_inq_resp_len,
			       sreq->sr_result);
			*bflags = BLIST_INQUIRY_36;
			goto repeat_inquiry;
		}

		/*
		 * The INQUIRY can change, this means the length can change.
		 */
		possible_inq_resp_len = (unsigned char) inq_result[4] + 5;
		if (BLIST_INQUIRY_58 & *bflags)
			possible_inq_resp_len = 58;
		else if (possible_inq_resp_len > 255)
			possible_inq_resp_len = 36;	/* sanity */
	}

	sdev->inquiry_len = possible_inq_resp_len;

	/*
	 * XXX Abort if the response length is less than 36? If less than
	 * 32, the lookup of the device flags (above) could be invalid,
	 * and it would be possible to take an incorrect action - we do
	 * not want to hang because of a short INQUIRY. On the flip side,
	 * if the device is spun down or becoming ready (and so it gives a
	 * short INQUIRY), an abort here prevents any further use of the
	 * device, including spin up.
	 *
	 * Related to the above issue:
	 *
	 * XXX Devices (disk or all?) should be sent a TEST UNIT READY,
	 * and if not ready, sent a START_STOP to start (maybe spin up) and
	 * then send the INQUIRY again, since the INQUIRY can change after
	 * a device is initialized.
	 *
	 * Ideally, start a device if explicitly asked to do so.  This
	 * assumes that a device is spun up on power on, spun down on
	 * request, and then spun up on request.
	 */

	/*
	 * The scanning code needs to know the scsi_level, even if no
	 * device is attached at LUN 0 (SCSI_SCAN_TARGET_PRESENT) so
	 * non-zero LUNs can be scanned.
	 */
	sdev->scsi_level = inq_result[2] & 0x07;
	if (sdev->scsi_level >= 2 ||
	    (sdev->scsi_level == 1 && (inq_result[3] & 0x0f) == 1))
		sdev->scsi_level++;

	return;
}

/**
 * scsi_add_lun - allocate and fully initialze a Scsi_Device
 * @sdevscan:	holds information to be stored in the new Scsi_Device
 * @sdevnew:	store the address of the newly allocated Scsi_Device
 * @inq_result:	holds the result of a previous INQUIRY to the LUN
 * @bflags:	black/white list flag
 *
 * Description:
 *     Allocate and initialize a Scsi_Device matching sdevscan. Optionally
 *     set fields based on values in *@bflags. If @sdevnew is not
 *     NULL, store the address of the new Scsi_Device in *@sdevnew (needed
 *     when scanning a particular LUN).
 *
 * Return:
 *     SCSI_SCAN_NO_RESPONSE: could not allocate or setup a Scsi_Device
 *     SCSI_SCAN_LUN_PRESENT: a new Scsi_Device was allocated and initialized
 **/
static int scsi_add_lun(struct scsi_device *sdev, char *inq_result, int *bflags)
{
	struct scsi_device *sdev_sibling;
	struct scsi_target *starget;
	unsigned long flags;

	/*
	 * XXX do not save the inquiry, since it can change underneath us,
	 * save just vendor/model/rev.
	 *
	 * Rather than save it and have an ioctl that retrieves the saved
	 * value, have an ioctl that executes the same INQUIRY code used
	 * in scsi_probe_lun, let user level programs doing INQUIRY
	 * scanning run at their own risk, or supply a user level program
	 * that can correctly scan.
	 */
	sdev->inquiry = kmalloc(sdev->inquiry_len, GFP_ATOMIC);
	if (sdev->inquiry == NULL) {
		return SCSI_SCAN_NO_RESPONSE;
	}

	memcpy(sdev->inquiry, inq_result, sdev->inquiry_len);
	sdev->vendor = (char *) (sdev->inquiry + 8);
	sdev->model = (char *) (sdev->inquiry + 16);
	sdev->rev = (char *) (sdev->inquiry + 32);

	if (*bflags & BLIST_ISROM) {
		/*
		 * It would be better to modify sdev->type, and set
		 * sdev->removable, but then the print_inquiry() output
		 * would not show TYPE_ROM; if print_inquiry() is removed
		 * the issue goes away.
		 */
		inq_result[0] = TYPE_ROM;
		inq_result[1] |= 0x80;	/* removable */
	}

	switch (sdev->type = (inq_result[0] & 0x1f)) {
	case TYPE_TAPE:
	case TYPE_DISK:
	case TYPE_PRINTER:
	case TYPE_MOD:
	case TYPE_PROCESSOR:
	case TYPE_SCANNER:
	case TYPE_MEDIUM_CHANGER:
	case TYPE_ENCLOSURE:
	case TYPE_COMM:
		sdev->writeable = 1;
		break;
	case TYPE_WORM:
	case TYPE_ROM:
		sdev->writeable = 0;
		break;
	default:
		printk(KERN_INFO "scsi: unknown device type %d\n", sdev->type);
	}

	print_inquiry(inq_result);

	/*
	 * For a peripheral qualifier (PQ) value of 1 (001b), the SCSI
	 * spec says: The device server is capable of supporting the
	 * specified peripheral device type on this logical unit. However,
	 * the physical device is not currently connected to this logical
	 * unit.
	 *
	 * The above is vague, as it implies that we could treat 001 and
	 * 011 the same. Stay compatible with previous code, and create a
	 * Scsi_Device for a PQ of 1
	 *
	 * Don't set the device offline here; rather let the upper
	 * level drivers eval the PQ to decide whether they should
	 * attach. So remove ((inq_result[0] >> 5) & 7) == 1 check.
	 */ 

	sdev->inq_periph_qual = (inq_result[0] >> 5) & 7;
	sdev->removable = (0x80 & inq_result[1]) >> 7;
	sdev->lockable = sdev->removable;
	sdev->soft_reset = (inq_result[7] & 1) && ((inq_result[3] & 7) == 2);

	if (sdev->scsi_level >= SCSI_3 || (sdev->inquiry_len > 56 &&
		inq_result[56] & 0x04))
		sdev->ppr = 1;
	if (inq_result[7] & 0x60)
		sdev->wdtr = 1;
	if (inq_result[7] & 0x10)
		sdev->sdtr = 1;

	sprintf(sdev->devfs_name, "scsi/host%d/bus%d/target%d/lun%d",
				sdev->host->host_no, sdev->channel,
				sdev->id, sdev->lun);

	/*
	 * End driverfs/devfs code.
	 */

	if ((sdev->scsi_level >= SCSI_2) && (inq_result[7] & 2) &&
	    !(*bflags & BLIST_NOTQ))
		sdev->tagged_supported = 1;
	/*
	 * Some devices (Texel CD ROM drives) have handshaking problems
	 * when used with the Seagate controllers. borken is initialized
	 * to 1, and then set it to 0 here.
	 */
	if ((*bflags & BLIST_BORKEN) == 0)
		sdev->borken = 0;

	/*
	 * Some devices may not want to have a start command automatically
	 * issued when a device is added.
	 */
	if (*bflags & BLIST_NOSTARTONADD)
		sdev->no_start_on_add = 1;

	/*
	 * If we need to allow I/O to only one of the luns attached to
	 * this target id at a time set single_lun, and allocate or modify
	 * sdev_target.
	 */
	if (*bflags & BLIST_SINGLELUN) {
		sdev->single_lun = 1;
		spin_lock_irqsave(sdev->host->host_lock, flags);
		starget = NULL;
		/*
		 * Search for an existing target for this sdev.
		 */
		list_for_each_entry(sdev_sibling, &sdev->same_target_siblings,
				    same_target_siblings) {
			if (sdev_sibling->sdev_target != NULL) {
				starget = sdev_sibling->sdev_target;
				break;
			}
		}
		if (!starget) {
			starget = kmalloc(sizeof(*starget), GFP_ATOMIC);
			if (!starget) {
				printk(ALLOC_FAILURE_MSG, __FUNCTION__);
				spin_unlock_irqrestore(sdev->host->host_lock,
						       flags);
				return SCSI_SCAN_NO_RESPONSE;
			}
			starget->starget_refcnt = 0;
			starget->starget_sdev_user = NULL;
		}
		starget->starget_refcnt++;
		sdev->sdev_target = starget;
		spin_unlock_irqrestore(sdev->host->host_lock, flags);
	}

	sdev->use_10_for_rw = 1;

	if (*bflags & BLIST_MS_SKIP_PAGE_08)
		sdev->skip_ms_page_8 = 1;

	if (*bflags & BLIST_MS_SKIP_PAGE_3F)
		sdev->skip_ms_page_3f = 1;

	if (*bflags & BLIST_USE_10_BYTE_MS)
		sdev->use_10_for_ms = 1;

	/* set the device running here so that slave configure
	 * may do I/O */
	scsi_device_set_state(sdev, SDEV_RUNNING);

	if (*bflags & BLIST_MS_192_BYTES_FOR_3F)
		sdev->use_192_bytes_for_3f = 1;

	if (*bflags & BLIST_NOT_LOCKABLE)
		sdev->lockable = 0;

	if(sdev->host->hostt->slave_configure)
		sdev->host->hostt->slave_configure(sdev);

	/*
	 * Ok, the device is now all set up, we can
	 * register it and tell the rest of the kernel
	 * about it.
	 */
	scsi_sysfs_add_sdev(sdev);

	return SCSI_SCAN_LUN_PRESENT;
}

/**
 * scsi_probe_and_add_lun - probe a LUN, if a LUN is found add it
 * @sdevscan:	probe the LUN corresponding to this Scsi_Device
 * @sdevnew:	store the value of any new Scsi_Device allocated
 * @bflagsp:	store bflags here if not NULL
 *
 * Description:
 *     Call scsi_probe_lun, if a LUN with an attached device is found,
 *     allocate and set it up by calling scsi_add_lun.
 *
 * Return:
 *     SCSI_SCAN_NO_RESPONSE: could not allocate or setup a Scsi_Device
 *     SCSI_SCAN_TARGET_PRESENT: target responded, but no device is
 *         attached at the LUN
 *     SCSI_SCAN_LUN_PRESENT: a new Scsi_Device was allocated and initialized
 **/
static int scsi_probe_and_add_lun(struct Scsi_Host *host,
		uint channel, uint id, uint lun, int *bflagsp,
		struct scsi_device **sdevp, int rescan)
{
	struct scsi_device *sdev;
	struct scsi_request *sreq;
	unsigned char *result;
	int bflags, res = SCSI_SCAN_NO_RESPONSE;

	/*
	 * The rescan flag is used as an optimization, the first scan of a
	 * host adapter calls into here with rescan == 0.
	 */
	if (rescan) {
		sdev = scsi_device_lookup(host, channel, id, lun);
		if (sdev) {
			SCSI_LOG_SCAN_BUS(3, printk(KERN_INFO
				"scsi scan: device exists on <%d:%d:%d:%d>\n",
				host->host_no, channel, id, lun));
			if (sdevp)
				*sdevp = sdev;
			if (bflagsp)
				*bflagsp = scsi_get_device_flags(sdev,
								 sdev->vendor,
								 sdev->model);
			/* XXX: bandaid until callers do refcounting */
			scsi_device_put(sdev);
			return SCSI_SCAN_LUN_PRESENT;
		}
	}

	sdev = scsi_alloc_sdev(host, channel, id, lun);
	if (!sdev)
		goto out;
	sreq = scsi_allocate_request(sdev, GFP_ATOMIC);
	if (!sreq)
		goto out_free_sdev;
	result = kmalloc(256, GFP_ATOMIC |
			(host->unchecked_isa_dma) ? __GFP_DMA : 0);
	if (!result)
		goto out_free_sreq;

	scsi_probe_lun(sreq, result, &bflags);
	if (sreq->sr_result)
		goto out_free_result;

	/*
	 * result contains valid SCSI INQUIRY data.
	 */
	if ((result[0] >> 5) == 3) {
		/*
		 * For a Peripheral qualifier 3 (011b), the SCSI
		 * spec says: The device server is not capable of
		 * supporting a physical device on this logical
		 * unit.
		 *
		 * For disks, this implies that there is no
		 * logical disk configured at sdev->lun, but there
		 * is a target id responding.
		 */
		SCSI_LOG_SCAN_BUS(3, printk(KERN_INFO
					"scsi scan: peripheral qualifier of 3,"
					" no device added\n"));
		res = SCSI_SCAN_TARGET_PRESENT;
		goto out_free_result;
	}

	res = scsi_add_lun(sdev, result, &bflags);
	if (res == SCSI_SCAN_LUN_PRESENT) {
		if (bflags & BLIST_KEY) {
			sdev->lockable = 0;
			scsi_unlock_floptical(sreq, result);
		}
		if (bflagsp)
			*bflagsp = bflags;
	}

 out_free_result:
	kfree(result);
 out_free_sreq:
	scsi_release_request(sreq);
 out_free_sdev:
	if (res == SCSI_SCAN_LUN_PRESENT) {
		if (sdevp)
			*sdevp = sdev;
	} else {
		if (sdev->host->hostt->slave_destroy)
			sdev->host->hostt->slave_destroy(sdev);
		if (sdev->host->transportt->cleanup)
			sdev->host->transportt->cleanup(sdev);
		put_device(&sdev->sdev_gendev);
	}
 out:
	return res;
}

/**
 * scsi_sequential_lun_scan - sequentially scan a SCSI target
 * @sdevscan:	scan the host, channel, and id of this Scsi_Device
 * @bflags:	black/white list flag for LUN 0
 * @lun0_res:	result of scanning LUN 0
 *
 * Description:
 *     Generally, scan from LUN 1 (LUN 0 is assumed to already have been
 *     scanned) to some maximum lun until a LUN is found with no device
 *     attached. Use the bflags to figure out any oddities.
 *
 *     Modifies sdevscan->lun.
 **/
static void scsi_sequential_lun_scan(struct Scsi_Host *shost, uint channel,
		uint id, int bflags, int lun0_res, int scsi_level, int rescan)
{
	unsigned int sparse_lun, lun, max_dev_lun;

	SCSI_LOG_SCAN_BUS(3, printk(KERN_INFO "scsi scan: Sequential scan of"
			" host %d channel %d id %d\n", shost->host_no,
			channel, id));

	max_dev_lun = min(max_scsi_luns, shost->max_lun);
	/*
	 * If this device is known to support sparse multiple units,
	 * override the other settings, and scan all of them. Normally,
	 * SCSI-3 devices should be scanned via the REPORT LUNS.
	 */
	if (bflags & BLIST_SPARSELUN) {
		max_dev_lun = shost->max_lun;
		sparse_lun = 1;
	} else
		sparse_lun = 0;

	/*
	 * If not sparse lun and no device attached at LUN 0 do not scan
	 * any further.
	 */
	if (!sparse_lun && (lun0_res != SCSI_SCAN_LUN_PRESENT))
		return;

	/*
	 * If less than SCSI_1_CSS, and no special lun scaning, stop
	 * scanning; this matches 2.4 behaviour, but could just be a bug
	 * (to continue scanning a SCSI_1_CSS device).
	 *
	 * This test is broken.  We might not have any device on lun0 for
	 * a sparselun device, and if that's the case then how would we
	 * know the real scsi_level, eh?  It might make sense to just not
	 * scan any SCSI_1 device for non-0 luns, but that check would best
	 * go into scsi_alloc_sdev() and just have it return null when asked
	 * to alloc an sdev for lun > 0 on an already found SCSI_1 device.
	 *
	if ((sdevscan->scsi_level < SCSI_1_CCS) &&
	    ((bflags & (BLIST_FORCELUN | BLIST_SPARSELUN | BLIST_MAX5LUN))
	     == 0))
		return;
	 */
	/*
	 * If this device is known to support multiple units, override
	 * the other settings, and scan all of them.
	 */
	if (bflags & BLIST_FORCELUN)
		max_dev_lun = shost->max_lun;
	/*
	 * REGAL CDC-4X: avoid hang after LUN 4
	 */
	if (bflags & BLIST_MAX5LUN)
		max_dev_lun = min(5U, max_dev_lun);
	/*
	 * Do not scan SCSI-2 or lower device past LUN 7, unless
	 * BLIST_LARGELUN.
	 */
	if (scsi_level < SCSI_3 && !(bflags & BLIST_LARGELUN))
		max_dev_lun = min(8U, max_dev_lun);

	/*
	 * We have already scanned LUN 0, so start at LUN 1. Keep scanning
	 * until we reach the max, or no LUN is found and we are not
	 * sparse_lun.
	 */
	for (lun = 1; lun < max_dev_lun; ++lun)
		if ((scsi_probe_and_add_lun(shost, channel, id, lun,
		      NULL, NULL, rescan) != SCSI_SCAN_LUN_PRESENT) &&
		    !sparse_lun)
			return;
}

/**
 * scsilun_to_int: convert a scsi_lun to an int
 * @scsilun:	struct scsi_lun to be converted.
 *
 * Description:
 *     Convert @scsilun from a struct scsi_lun to a four byte host byte-ordered
 *     integer, and return the result. The caller must check for
 *     truncation before using this function.
 *
 * Notes:
 *     The struct scsi_lun is assumed to be four levels, with each level
 *     effectively containing a SCSI byte-ordered (big endian) short; the
 *     addressing bits of each level are ignored (the highest two bits).
 *     For a description of the LUN format, post SCSI-3 see the SCSI
 *     Architecture Model, for SCSI-3 see the SCSI Controller Commands.
 *
 *     Given a struct scsi_lun of: 0a 04 0b 03 00 00 00 00, this function returns
 *     the integer: 0x0b030a04
 **/
static int scsilun_to_int(struct scsi_lun *scsilun)
{
	int i;
	unsigned int lun;

	lun = 0;
	for (i = 0; i < sizeof(lun); i += 2)
		lun = lun | (((scsilun->scsi_lun[i] << 8) |
			      scsilun->scsi_lun[i + 1]) << (i * 8));
	return lun;
}

/**
 * scsi_report_lun_scan - Scan using SCSI REPORT LUN results
 * @sdevscan:	scan the host, channel, and id of this Scsi_Device
 *
 * Description:
 *     If @sdevscan is for a SCSI-3 or up device, send a REPORT LUN
 *     command, and scan the resulting list of LUNs by calling
 *     scsi_probe_and_add_lun.
 *
 *     Modifies sdevscan->lun.
 *
 * Return:
 *     0: scan completed (or no memory, so further scanning is futile)
 *     1: no report lun scan, or not configured
 **/
static int scsi_report_lun_scan(struct scsi_device *sdev, int bflags,
				int rescan)
{
	char devname[64];
	unsigned char scsi_cmd[MAX_COMMAND_SIZE];
	unsigned int length;
	unsigned int lun;
	unsigned int num_luns;
	unsigned int retries;
	struct scsi_lun *lunp, *lun_data;
	struct scsi_request *sreq;
	u8 *data;

	/*
	 * Only support SCSI-3 and up devices if BLIST_NOREPORTLUN is not set.
	 * Also allow SCSI-2 if BLIST_REPORTLUN2 is set and host adapter does
	 * support more than 8 LUNs.
	 */
	if ((bflags & BLIST_NOREPORTLUN) || 
	     sdev->scsi_level < SCSI_2 ||
	    (sdev->scsi_level < SCSI_3 && 
	     (!(bflags & BLIST_REPORTLUN2) || sdev->host->max_lun <= 8)) )
		return 1;
	if (bflags & BLIST_NOLUN)
		return 0;

	sreq = scsi_allocate_request(sdev, GFP_ATOMIC);
	if (!sreq)
		goto out;

	sprintf(devname, "host %d channel %d id %d",
		sdev->host->host_no, sdev->channel, sdev->id);

	/*
	 * Allocate enough to hold the header (the same size as one scsi_lun)
	 * plus the max number of luns we are requesting.
	 *
	 * Reallocating and trying again (with the exact amount we need)
	 * would be nice, but then we need to somehow limit the size
	 * allocated based on the available memory and the limits of
	 * kmalloc - we don't want a kmalloc() failure of a huge value to
	 * prevent us from finding any LUNs on this target.
	 */
	length = (max_scsi_report_luns + 1) * sizeof(struct scsi_lun);
	lun_data = kmalloc(length, GFP_ATOMIC |
			   (sdev->host->unchecked_isa_dma ? __GFP_DMA : 0));
	if (!lun_data)
		goto out_release_request;

	scsi_cmd[0] = REPORT_LUNS;

	/*
	 * bytes 1 - 5: reserved, set to zero.
	 */
	memset(&scsi_cmd[1], 0, 5);

	/*
	 * bytes 6 - 9: length of the command.
	 */
	scsi_cmd[6] = (unsigned char) (length >> 24) & 0xff;
	scsi_cmd[7] = (unsigned char) (length >> 16) & 0xff;
	scsi_cmd[8] = (unsigned char) (length >> 8) & 0xff;
	scsi_cmd[9] = (unsigned char) length & 0xff;

	scsi_cmd[10] = 0;	/* reserved */
	scsi_cmd[11] = 0;	/* control */
	sreq->sr_cmd_len = 0;
	sreq->sr_data_direction = DMA_FROM_DEVICE;

	/*
	 * We can get a UNIT ATTENTION, for example a power on/reset, so
	 * retry a few times (like sd.c does for TEST UNIT READY).
	 * Experience shows some combinations of adapter/devices get at
	 * least two power on/resets.
	 *
	 * Illegal requests (for devices that do not support REPORT LUNS)
	 * should come through as a check condition, and will not generate
	 * a retry.
	 */
	for (retries = 0; retries < 3; retries++) {
		SCSI_LOG_SCAN_BUS(3, printk (KERN_INFO "scsi scan: Sending"
				" REPORT LUNS to %s (try %d)\n", devname,
				retries));
		scsi_wait_req(sreq, scsi_cmd, lun_data, length,
				SCSI_TIMEOUT + 4*HZ, 3);
		SCSI_LOG_SCAN_BUS(3, printk (KERN_INFO "scsi scan: REPORT LUNS"
				" %s (try %d) result 0x%x\n", sreq->sr_result
				?  "failed" : "successful", retries,
				sreq->sr_result));
		if (sreq->sr_result == 0 ||
		    sreq->sr_sense_buffer[2] != UNIT_ATTENTION)
			break;
	}

	if (sreq->sr_result) {
		/*
		 * The device probably does not support a REPORT LUN command
		 */
		kfree(lun_data);
		scsi_release_request(sreq);
		return 1;
	}
	scsi_release_request(sreq);

	/*
	 * Get the length from the first four bytes of lun_data.
	 */
	data = (u8 *) lun_data->scsi_lun;
	length = ((data[0] << 24) | (data[1] << 16) |
		  (data[2] << 8) | (data[3] << 0));

	num_luns = (length / sizeof(struct scsi_lun));
	if (num_luns > max_scsi_report_luns) {
		printk(KERN_WARNING "scsi: On %s only %d (max_scsi_report_luns)"
		       " of %d luns reported, try increasing"
		       " max_scsi_report_luns.\n", devname,
		       max_scsi_report_luns, num_luns);
		num_luns = max_scsi_report_luns;
	}

	SCSI_LOG_SCAN_BUS(3, printk (KERN_INFO "scsi scan: REPORT LUN scan of"
			" host %d channel %d id %d\n", sdev->host->host_no,
			sdev->channel, sdev->id));

	/*
	 * Scan the luns in lun_data. The entry at offset 0 is really
	 * the header, so start at 1 and go up to and including num_luns.
	 */
	for (lunp = &lun_data[1]; lunp <= &lun_data[num_luns]; lunp++) {
		lun = scsilun_to_int(lunp);

		/*
		 * Check if the unused part of lunp is non-zero, and so
		 * does not fit in lun.
		 */
		if (memcmp(&lunp->scsi_lun[sizeof(lun)], "\0\0\0\0", 4)) {
			int i;

			/*
			 * Output an error displaying the LUN in byte order,
			 * this differs from what linux would print for the
			 * integer LUN value.
			 */
			printk(KERN_WARNING "scsi: %s lun 0x", devname);
			data = (char *)lunp->scsi_lun;
			for (i = 0; i < sizeof(struct scsi_lun); i++)
				printk("%02x", data[i]);
			printk(" has a LUN larger than currently supported.\n");
		} else if (lun == 0) {
			/*
			 * LUN 0 has already been scanned.
			 */
		} else if (lun > sdev->host->max_lun) {
			printk(KERN_WARNING "scsi: %s lun%d has a LUN larger"
			       " than allowed by the host adapter\n",
			       devname, lun);
		} else {
			int res;

			res = scsi_probe_and_add_lun(sdev->host, sdev->channel,
				sdev->id, lun, NULL, NULL, rescan);
			if (res == SCSI_SCAN_NO_RESPONSE) {
				/*
				 * Got some results, but now none, abort.
				 */
				printk(KERN_ERR "scsi: Unexpected response"
				       " from %s lun %d while scanning, scan"
				       " aborted\n", devname, lun);
				break;
			}
		}
	}

	kfree(lun_data);
	return 0;

 out_release_request:
	scsi_release_request(sreq);
 out:
	/*
	 * We are out of memory, don't try scanning any further.
	 */
	printk(ALLOC_FAILURE_MSG, __FUNCTION__);
	return 0;
}

struct scsi_device *scsi_add_device(struct Scsi_Host *shost,
				    uint channel, uint id, uint lun)
{
	struct scsi_device *sdev;
	int res;

	down(&shost->scan_mutex);
	res = scsi_probe_and_add_lun(shost, channel, id, lun, NULL, &sdev, 1);
	if (res != SCSI_SCAN_LUN_PRESENT)
		sdev = ERR_PTR(-ENODEV);
	up(&shost->scan_mutex);

	return sdev;
}

void scsi_rescan_device(struct device *dev)
{
	struct scsi_driver *drv;
	
	if (!dev->driver)
		return;

	drv = to_scsi_driver(dev->driver);
	if (try_module_get(drv->owner)) {
		if (drv->rescan)
			drv->rescan(dev);
		module_put(drv->owner);
	}
}

/**
 * scsi_scan_target - scan a target id, possibly including all LUNs on the
 *     target.
 * @sdevsca:	Scsi_Device handle for scanning
 * @shost:	host to scan
 * @channel:	channel to scan
 * @id:		target id to scan
 *
 * Description:
 *     Scan the target id on @shost, @channel, and @id. Scan at least LUN
 *     0, and possibly all LUNs on the target id.
 *
 *     Use the pre-allocated @sdevscan as a handle for the scanning. This
 *     function sets sdevscan->host, sdevscan->id and sdevscan->lun; the
 *     scanning functions modify sdevscan->lun.
 *
 *     First try a REPORT LUN scan, if that does not scan the target, do a
 *     sequential scan of LUNs on the target id.
 **/
static void scsi_scan_target(struct Scsi_Host *shost, unsigned int channel,
			     unsigned int id, unsigned int lun, int rescan)
{
	int bflags = 0;
	int res;
	struct scsi_device *sdev;

	if (shost->this_id == id)
		/*
		 * Don't scan the host adapter
		 */
		return;

	if (lun != SCAN_WILD_CARD) {
		/*
		 * Scan for a specific host/chan/id/lun.
		 */
		scsi_probe_and_add_lun(shost, channel, id, lun, NULL, NULL,
				       rescan);
		return;
	}

	/*
	 * Scan LUN 0, if there is some response, scan further. Ideally, we
	 * would not configure LUN 0 until all LUNs are scanned.
	 */
	res = scsi_probe_and_add_lun(shost, channel, id, 0, &bflags, &sdev,
				     rescan);
	if (res == SCSI_SCAN_LUN_PRESENT) {
		if (scsi_report_lun_scan(sdev, bflags, rescan) != 0)
			/*
			 * The REPORT LUN did not scan the target,
			 * do a sequential scan.
			 */
			scsi_sequential_lun_scan(shost, channel, id, bflags,
				       	res, sdev->scsi_level, rescan);
	} else if (res == SCSI_SCAN_TARGET_PRESENT) {
		/*
		 * There's a target here, but lun 0 is offline so we
		 * can't use the report_lun scan.  Fall back to a
		 * sequential lun scan with a bflags of SPARSELUN and
		 * a default scsi level of SCSI_2
		 */
		scsi_sequential_lun_scan(shost, channel, id, BLIST_SPARSELUN,
				SCSI_SCAN_TARGET_PRESENT, SCSI_2, rescan);
	}
}

static void scsi_scan_channel(struct Scsi_Host *shost, unsigned int channel,
			      unsigned int id, unsigned int lun, int rescan)
{
	uint order_id;

	if (id == SCAN_WILD_CARD)
		for (id = 0; id < shost->max_id; ++id) {
			/*
			 * XXX adapter drivers when possible (FCP, iSCSI)
			 * could modify max_id to match the current max,
			 * not the absolute max.
			 *
			 * XXX add a shost id iterator, so for example,
			 * the FC ID can be the same as a target id
			 * without a huge overhead of sparse id's.
			 */
			if (shost->reverse_ordering)
				/*
				 * Scan from high to low id.
				 */
				order_id = shost->max_id - id - 1;
			else
				order_id = id;
			scsi_scan_target(shost, channel, order_id, lun, rescan);
		}
	else
		scsi_scan_target(shost, channel, id, lun, rescan);
}

int scsi_scan_host_selected(struct Scsi_Host *shost, unsigned int channel,
			    unsigned int id, unsigned int lun, int rescan)
{
	SCSI_LOG_SCAN_BUS(3, printk (KERN_INFO "%s: <%u:%u:%u:%u>\n",
		__FUNCTION__, shost->host_no, channel, id, lun));

	if (((channel != SCAN_WILD_CARD) && (channel > shost->max_channel)) ||
	    ((id != SCAN_WILD_CARD) && (id > shost->max_id)) ||
	    ((lun != SCAN_WILD_CARD) && (lun > shost->max_lun)))
		return -EINVAL;

	down(&shost->scan_mutex);
	if (channel == SCAN_WILD_CARD) 
		for (channel = 0; channel <= shost->max_channel; channel++)
			scsi_scan_channel(shost, channel, id, lun, rescan);
	else
		scsi_scan_channel(shost, channel, id, lun, rescan);
	up(&shost->scan_mutex);

	return 0;
}

/**
 * scsi_scan_host - scan the given adapter
 * @shost:	adapter to scan
 **/
void scsi_scan_host(struct Scsi_Host *shost)
{
	scsi_scan_host_selected(shost, SCAN_WILD_CARD, SCAN_WILD_CARD,
				SCAN_WILD_CARD, 0);
}

void scsi_forget_host(struct Scsi_Host *shost)
{
	struct scsi_device *sdev, *tmp;
	unsigned long flags;

	/*
	 * Ok, this look a bit strange.  We always look for the first device
	 * on the list as scsi_remove_device removes them from it - thus we
	 * also have to release the lock.
	 * We don't need to get another reference to the device before
	 * releasing the lock as we already own the reference from
	 * scsi_register_device that's release in scsi_remove_device.  And
	 * after that we don't look at sdev anymore.
	 */
	spin_lock_irqsave(shost->host_lock, flags);
	list_for_each_entry_safe(sdev, tmp, &shost->__devices, siblings) {
		spin_unlock_irqrestore(shost->host_lock, flags);
		scsi_remove_device(sdev);
		spin_lock_irqsave(shost->host_lock, flags);
	}
	spin_unlock_irqrestore(shost->host_lock, flags);
}

/*
 * Function:    scsi_get_host_dev()
 *
 * Purpose:     Create a Scsi_Device that points to the host adapter itself.
 *
 * Arguments:   SHpnt   - Host that needs a Scsi_Device
 *
 * Lock status: None assumed.
 *
 * Returns:     The Scsi_Device or NULL
 *
 * Notes:
 *	Attach a single Scsi_Device to the Scsi_Host - this should
 *	be made to look like a "pseudo-device" that points to the
 *	HA itself.
 *
 *	Note - this device is not accessible from any high-level
 *	drivers (including generics), which is probably not
 *	optimal.  We can add hooks later to attach 
 */
struct scsi_device *scsi_get_host_dev(struct Scsi_Host *shost)
{
	struct scsi_device *sdev;

	sdev = scsi_alloc_sdev(shost, 0, shost->this_id, 0);
	if (sdev) {
		sdev->borken = 0;
	}
	return sdev;
}

/*
 * Function:    scsi_free_host_dev()
 *
 * Purpose:     Free a scsi_device that points to the host adapter itself.
 *
 * Arguments:   SHpnt   - Host that needs a Scsi_Device
 *
 * Lock status: None assumed.
 *
 * Returns:     Nothing
 *
 * Notes:
 */
void scsi_free_host_dev(struct scsi_device *sdev)
{
	BUG_ON(sdev->id != sdev->host->this_id);

	if (sdev->host->hostt->slave_destroy)
		sdev->host->hostt->slave_destroy(sdev);
	if (sdev->host->transportt->cleanup)
		sdev->host->transportt->cleanup(sdev);
	put_device(&sdev->sdev_gendev);
}
