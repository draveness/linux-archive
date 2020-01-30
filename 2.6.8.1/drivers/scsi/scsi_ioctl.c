/*
 * Changes:
 * Arnaldo Carvalho de Melo <acme@conectiva.com.br> 08/23/2000
 * - get rid of some verify_areas and use __copy*user and __get/put_user
 *   for the ones that remain
 */
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <asm/uaccess.h>

#include <scsi/scsi.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_ioctl.h>
#include <scsi/scsi_request.h>

#include "scsi_logging.h"

#define NORMAL_RETRIES			5
#define IOCTL_NORMAL_TIMEOUT			(10 * HZ)
#define FORMAT_UNIT_TIMEOUT		(2 * 60 * 60 * HZ)
#define START_STOP_TIMEOUT		(60 * HZ)
#define MOVE_MEDIUM_TIMEOUT		(5 * 60 * HZ)
#define READ_ELEMENT_STATUS_TIMEOUT	(5 * 60 * HZ)
#define READ_DEFECT_DATA_TIMEOUT	(60 * HZ )  /* ZIP-250 on parallel port takes as long! */

#define MAX_BUF PAGE_SIZE

/*
 * If we are told to probe a host, we will return 0 if  the host is not
 * present, 1 if the host is present, and will return an identifying
 * string at *arg, if arg is non null, filling to the length stored at
 * (int *) arg
 */

static int ioctl_probe(struct Scsi_Host *host, void __user *buffer)
{
	unsigned int len, slen;
	const char *string;
	int temp = host->hostt->present;

	if (temp && buffer) {
		if (get_user(len, (unsigned int __user *) buffer))
			return -EFAULT;

		if (host->hostt->info)
			string = host->hostt->info(host);
		else
			string = host->hostt->name;
		if (string) {
			slen = strlen(string);
			if (len > slen)
				len = slen + 1;
			if (copy_to_user(buffer, string, len))
				return -EFAULT;
		}
	}
	return temp;
}

/*

 * The SCSI_IOCTL_SEND_COMMAND ioctl sends a command out to the SCSI host.
 * The IOCTL_NORMAL_TIMEOUT and NORMAL_RETRIES  variables are used.  
 * 
 * dev is the SCSI device struct ptr, *(int *) arg is the length of the
 * input data, if any, not including the command string & counts, 
 * *((int *)arg + 1) is the output buffer size in bytes.
 * 
 * *(char *) ((int *) arg)[2] the actual command byte.   
 * 
 * Note that if more than MAX_BUF bytes are requested to be transferred,
 * the ioctl will fail with error EINVAL.
 * 
 * This size *does not* include the initial lengths that were passed.
 * 
 * The SCSI command is read from the memory location immediately after the
 * length words, and the input data is right after the command.  The SCSI
 * routines know the command size based on the opcode decode.  
 * 
 * The output area is then filled in starting from the command byte. 
 */

static int ioctl_internal_command(struct scsi_device *sdev, char *cmd,
				  int timeout, int retries)
{
	struct scsi_request *sreq;
	int result;

	SCSI_LOG_IOCTL(1, printk("Trying ioctl with scsi command %d\n", *cmd));

	sreq = scsi_allocate_request(sdev, GFP_KERNEL);
	if (!sreq) {
		printk("SCSI internal ioctl failed, no memory\n");
		return -ENOMEM;
	}

	sreq->sr_data_direction = DMA_NONE;
        scsi_wait_req(sreq, cmd, NULL, 0, timeout, retries);

	SCSI_LOG_IOCTL(2, printk("Ioctl returned  0x%x\n", sreq->sr_result));

	if (driver_byte(sreq->sr_result)) {
		switch (sreq->sr_sense_buffer[2] & 0xf) {
		case ILLEGAL_REQUEST:
			if (cmd[0] == ALLOW_MEDIUM_REMOVAL)
				sdev->lockable = 0;
			else
				printk("SCSI device (ioctl) reports ILLEGAL REQUEST.\n");
			break;
		case NOT_READY:	/* This happens if there is no disc in drive */
			if (sdev->removable && (cmd[0] != TEST_UNIT_READY)) {
				printk(KERN_INFO "Device not ready.  Make sure there is a disc in the drive.\n");
				break;
			}
		case UNIT_ATTENTION:
			if (sdev->removable) {
				sdev->changed = 1;
				sreq->sr_result = 0;	/* This is no longer considered an error */
				break;
			}
		default:	/* Fall through for non-removable media */
			printk("SCSI error: host %d id %d lun %d return code = %x\n",
			       sdev->host->host_no,
			       sdev->id,
			       sdev->lun,
			       sreq->sr_result);
			printk("\tSense class %x, sense error %x, extended sense %x\n",
			       sense_class(sreq->sr_sense_buffer[0]),
			       sense_error(sreq->sr_sense_buffer[0]),
			       sreq->sr_sense_buffer[2] & 0xf);

		}
	}

	result = sreq->sr_result;
	SCSI_LOG_IOCTL(2, printk("IOCTL Releasing command\n"));
	scsi_release_request(sreq);
	return result;
}

int scsi_set_medium_removal(struct scsi_device *sdev, char state)
{
	char scsi_cmd[MAX_COMMAND_SIZE];
	int ret;

	if (!sdev->removable || !sdev->lockable)
	       return 0;

	scsi_cmd[0] = ALLOW_MEDIUM_REMOVAL;
	scsi_cmd[1] = 0;
	scsi_cmd[2] = 0;
	scsi_cmd[3] = 0;
	scsi_cmd[4] = state;
	scsi_cmd[5] = 0;

	ret = ioctl_internal_command(sdev, scsi_cmd,
			IOCTL_NORMAL_TIMEOUT, NORMAL_RETRIES);
	if (ret == 0)
		sdev->locked = (state == SCSI_REMOVAL_PREVENT);
	return ret;
}

/*
 * This interface is deprecated - users should use the scsi generic (sg)
 * interface instead, as this is a more flexible approach to performing
 * generic SCSI commands on a device.
 *
 * The structure that we are passed should look like:
 *
 * struct sdata {
 *  unsigned int inlen;      [i] Length of data to be written to device 
 *  unsigned int outlen;     [i] Length of data to be read from device 
 *  unsigned char cmd[x];    [i] SCSI command (6 <= x <= 12).
 *                           [o] Data read from device starts here.
 *                           [o] On error, sense buffer starts here.
 *  unsigned char wdata[y];  [i] Data written to device starts here.
 * };
 * Notes:
 *   -  The SCSI command length is determined by examining the 1st byte
 *      of the given command. There is no way to override this.
 *   -  Data transfers are limited to PAGE_SIZE (4K on i386, 8K on alpha).
 *   -  The length (x + y) must be at least OMAX_SB_LEN bytes long to
 *      accommodate the sense buffer when an error occurs.
 *      The sense buffer is truncated to OMAX_SB_LEN (16) bytes so that
 *      old code will not be surprised.
 *   -  If a Unix error occurs (e.g. ENOMEM) then the user will receive
 *      a negative return and the Unix error code in 'errno'. 
 *      If the SCSI command succeeds then 0 is returned.
 *      Positive numbers returned are the compacted SCSI error codes (4 
 *      bytes in one int) where the lowest byte is the SCSI status.
 *      See the drivers/scsi/scsi.h file for more information on this.
 *
 */
#define OMAX_SB_LEN 16		/* Old sense buffer length */

int scsi_ioctl_send_command(struct scsi_device *sdev,
			    struct scsi_ioctl_command __user *sic)
{
	char *buf;
	unsigned char cmd[MAX_COMMAND_SIZE];
	char __user *cmd_in;
	struct scsi_request *sreq;
	unsigned char opcode;
	unsigned int inlen, outlen, cmdlen;
	unsigned int needed, buf_needed;
	int timeout, retries, result;
	int data_direction, gfp_mask = GFP_KERNEL;

	if (!sic)
		return -EINVAL;

	if (sdev->host->unchecked_isa_dma)
		gfp_mask |= GFP_DMA;

	/*
	 * Verify that we can read at least this much.
	 */
	if (verify_area(VERIFY_READ, sic, sizeof(Scsi_Ioctl_Command)))
		return -EFAULT;

	if(__get_user(inlen, &sic->inlen))
		return -EFAULT;
		
	if(__get_user(outlen, &sic->outlen))
		return -EFAULT;

	/*
	 * We do not transfer more than MAX_BUF with this interface.
	 * If the user needs to transfer more data than this, they
	 * should use scsi_generics (sg) instead.
	 */
	if (inlen > MAX_BUF)
		return -EINVAL;
	if (outlen > MAX_BUF)
		return -EINVAL;

	cmd_in = sic->data;
	if(get_user(opcode, cmd_in))
		return -EFAULT;

	needed = buf_needed = (inlen > outlen ? inlen : outlen);
	if (buf_needed) {
		buf_needed = (buf_needed + 511) & ~511;
		if (buf_needed > MAX_BUF)
			buf_needed = MAX_BUF;
		buf = kmalloc(buf_needed, gfp_mask);
		if (!buf)
			return -ENOMEM;
		memset(buf, 0, buf_needed);
		if (inlen == 0) {
			data_direction = DMA_FROM_DEVICE;
		} else if (outlen == 0 ) {
			data_direction = DMA_TO_DEVICE;
		} else {
			/*
			 * Can this ever happen?
			 */
			data_direction = DMA_BIDIRECTIONAL;
		}

	} else {
		buf = NULL;
		data_direction = DMA_NONE;
	}

	/*
	 * Obtain the command from the user's address space.
	 */
	cmdlen = COMMAND_SIZE(opcode);
	
	result = -EFAULT;

	if (verify_area(VERIFY_READ, cmd_in, cmdlen + inlen))
		goto error;

	if(__copy_from_user(cmd, cmd_in, cmdlen))
		goto error;

	/*
	 * Obtain the data to be sent to the device (if any).
	 */

	if(copy_from_user(buf, cmd_in + cmdlen, inlen))
		goto error;

	switch (opcode) {
	case SEND_DIAGNOSTIC:
	case FORMAT_UNIT:
		timeout = FORMAT_UNIT_TIMEOUT;
		retries = 1;
		break;
	case START_STOP:
		timeout = START_STOP_TIMEOUT;
		retries = NORMAL_RETRIES;
		break;
	case MOVE_MEDIUM:
		timeout = MOVE_MEDIUM_TIMEOUT;
		retries = NORMAL_RETRIES;
		break;
	case READ_ELEMENT_STATUS:
		timeout = READ_ELEMENT_STATUS_TIMEOUT;
		retries = NORMAL_RETRIES;
		break;
	case READ_DEFECT_DATA:
		timeout = READ_DEFECT_DATA_TIMEOUT;
		retries = 1;
		break;
	default:
		timeout = IOCTL_NORMAL_TIMEOUT;
		retries = NORMAL_RETRIES;
		break;
	}

	sreq = scsi_allocate_request(sdev, GFP_KERNEL);
        if (!sreq) {
                result = -EINTR;
                goto error;
        }

	sreq->sr_data_direction = data_direction;
        scsi_wait_req(sreq, cmd, buf, needed, timeout, retries);

	/* 
	 * If there was an error condition, pass the info back to the user. 
	 */
	result = sreq->sr_result;
	if (result) {
		int sb_len = sizeof(sreq->sr_sense_buffer);

		sb_len = (sb_len > OMAX_SB_LEN) ? OMAX_SB_LEN : sb_len;
		if (copy_to_user(cmd_in, sreq->sr_sense_buffer, sb_len))
			result = -EFAULT;
	} else {
		if (copy_to_user(cmd_in, buf, outlen))
			result = -EFAULT;
	}	

	scsi_release_request(sreq);
error:
	kfree(buf);
	return result;
}

/*
 * The scsi_ioctl_get_pci() function places into arg the value
 * pci_dev::slot_name (8 characters) for the PCI device (if any).
 * Returns: 0 on success
 *          -ENXIO if there isn't a PCI device pointer
 *                 (could be because the SCSI driver hasn't been
 *                  updated yet, or because it isn't a SCSI
 *                  device)
 *          any copy_to_user() error on failure there
 */
static int scsi_ioctl_get_pci(struct scsi_device *sdev, void __user *arg)
{
	struct device *dev = scsi_get_device(sdev->host);

        if (!dev)
		return -ENXIO;
        return copy_to_user(arg, dev->bus_id, sizeof(dev->bus_id))? -EFAULT: 0;
}


/*
 * the scsi_ioctl() function differs from most ioctls in that it does
 * not take a major/minor number as the dev field.  Rather, it takes
 * a pointer to a scsi_devices[] element, a structure. 
 */
int scsi_ioctl(struct scsi_device *sdev, int cmd, void __user *arg)
{
	char scsi_cmd[MAX_COMMAND_SIZE];

	/* No idea how this happens.... */
	if (!sdev)
		return -ENXIO;

	/*
	 * If we are in the middle of error recovery, don't let anyone
	 * else try and use this device.  Also, if error recovery fails, it
	 * may try and take the device offline, in which case all further
	 * access to the device is prohibited.
	 */
	if (!scsi_block_when_processing_errors(sdev))
		return -ENODEV;

	switch (cmd) {
	case SCSI_IOCTL_GET_IDLUN:
		if (verify_area(VERIFY_WRITE, arg, sizeof(struct scsi_idlun)))
			return -EFAULT;

		__put_user((sdev->id & 0xff)
			 + ((sdev->lun & 0xff) << 8)
			 + ((sdev->channel & 0xff) << 16)
			 + ((sdev->host->host_no & 0xff) << 24),
			 &((struct scsi_idlun __user *)arg)->dev_id);
		__put_user(sdev->host->unique_id,
			 &((struct scsi_idlun __user *)arg)->host_unique_id);
		return 0;
	case SCSI_IOCTL_GET_BUS_NUMBER:
		return put_user(sdev->host->host_no, (int __user *)arg);
	case SCSI_IOCTL_PROBE_HOST:
		return ioctl_probe(sdev->host, arg);
	case SCSI_IOCTL_SEND_COMMAND:
		if (!capable(CAP_SYS_ADMIN) || !capable(CAP_SYS_RAWIO))
			return -EACCES;
		return scsi_ioctl_send_command(sdev, arg);
	case SCSI_IOCTL_DOORLOCK:
		return scsi_set_medium_removal(sdev, SCSI_REMOVAL_PREVENT);
	case SCSI_IOCTL_DOORUNLOCK:
		return scsi_set_medium_removal(sdev, SCSI_REMOVAL_ALLOW);
	case SCSI_IOCTL_TEST_UNIT_READY:
		scsi_cmd[0] = TEST_UNIT_READY;
		scsi_cmd[1] = 0;
		scsi_cmd[2] = scsi_cmd[3] = scsi_cmd[5] = 0;
		scsi_cmd[4] = 0;
		return ioctl_internal_command(sdev, scsi_cmd,
				   IOCTL_NORMAL_TIMEOUT, NORMAL_RETRIES);
	case SCSI_IOCTL_START_UNIT:
		scsi_cmd[0] = START_STOP;
		scsi_cmd[1] = 0;
		scsi_cmd[2] = scsi_cmd[3] = scsi_cmd[5] = 0;
		scsi_cmd[4] = 1;
		return ioctl_internal_command(sdev, scsi_cmd,
				     START_STOP_TIMEOUT, NORMAL_RETRIES);
	case SCSI_IOCTL_STOP_UNIT:
		scsi_cmd[0] = START_STOP;
		scsi_cmd[1] = 0;
		scsi_cmd[2] = scsi_cmd[3] = scsi_cmd[5] = 0;
		scsi_cmd[4] = 0;
		return ioctl_internal_command(sdev, scsi_cmd,
				     START_STOP_TIMEOUT, NORMAL_RETRIES);
        case SCSI_IOCTL_GET_PCI:
                return scsi_ioctl_get_pci(sdev, arg);
	default:
		if (sdev->host->hostt->ioctl)
			return sdev->host->hostt->ioctl(sdev, cmd, arg);
	}
	return -EINVAL;
}
