#ifndef _SCSI_SCSI_TCQ_H
#define _SCSI_SCSI_TCQ_H

#include <linux/blkdev.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>


#define MSG_SIMPLE_TAG	0x20
#define MSG_HEAD_TAG	0x21
#define MSG_ORDERED_TAG	0x22

#define SCSI_NO_TAG	(-1)    /* identify no tag in use */


/**
 * scsi_activate_tcq - turn on tag command queueing
 * @SDpnt:	device to turn on TCQ for
 * @depth:	queue depth
 *
 * Notes:
 *	Eventually, I hope depth would be the maximum depth
 *	the device could cope with and the real queue depth
 *	would be adjustable from 0 to depth.
 **/
static inline void scsi_activate_tcq(struct scsi_device *sdev, int depth)
{
        if (sdev->tagged_supported) {
		if (!blk_queue_tagged(sdev->request_queue))
			blk_queue_init_tags(sdev->request_queue, depth, NULL);
		scsi_adjust_queue_depth(sdev, MSG_ORDERED_TAG, depth);
        }
}

/**
 * scsi_deactivate_tcq - turn off tag command queueing
 * @SDpnt:	device to turn off TCQ for
 **/
static inline void scsi_deactivate_tcq(struct scsi_device *sdev, int depth)
{
	if (blk_queue_tagged(sdev->request_queue))
		blk_queue_free_tags(sdev->request_queue);
	scsi_adjust_queue_depth(sdev, 0, depth);
}

/**
 * scsi_populate_tag_msg - place a tag message in a buffer
 * @SCpnt:	pointer to the Scsi_Cmnd for the tag
 * @msg:	pointer to the area to place the tag
 *
 * Notes:
 *	designed to create the correct type of tag message for the 
 *	particular request.  Returns the size of the tag message.
 *	May return 0 if TCQ is disabled for this device.
 **/
static inline int scsi_populate_tag_msg(struct scsi_cmnd *cmd, char *msg)
{
        struct request *req = cmd->request;

        if (blk_rq_tagged(req)) {
		if (req->flags & REQ_HARDBARRIER)
        	        *msg++ = MSG_ORDERED_TAG;
        	else
        	        *msg++ = MSG_SIMPLE_TAG;
        	*msg++ = req->tag;
        	return 2;
	}

	return 0;
}

/**
 * scsi_find_tag - find a tagged command by device
 * @SDpnt:	pointer to the ScSI device
 * @tag:	the tag number
 *
 * Notes:
 *	Only works with tags allocated by the generic blk layer.
 **/
static inline struct scsi_cmnd *scsi_find_tag(struct scsi_device *sdev, int tag)
{

        struct request *req;

        if (tag != SCSI_NO_TAG) {
        	req = blk_queue_find_tag(sdev->request_queue, tag);
	        return req ? (struct scsi_cmnd *)req->special : NULL;
	}

	/* single command, look in space */
	return sdev->current_cmnd;
}

#endif /* _SCSI_SCSI_TCQ_H */
