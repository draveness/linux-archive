/*
 *  scsi_error.c Copyright (C) 1997 Eric Youngdale
 *
 *  SCSI error/timeout handling
 *      Initial versions: Eric Youngdale.  Based upon conversations with
 *                        Leonard Zubkoff and David Miller at Linux Expo, 
 *                        ideas originating from all over the place.
 *
 *	Restructured scsi_unjam_host and associated functions.
 *	September 04, 2002 Mike Anderson (andmike@us.ibm.com)
 *
 *	Forward port of Russell King's (rmk@arm.linux.org.uk) changes and
 *	minor  cleanups.
 *	September 30, 2002 Mike Anderson (andmike@us.ibm.com)
 */

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/blkdev.h>
#include <linux/smp_lock.h>

#include <scsi/scsi.h>
#include <scsi/scsi_dbg.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_ioctl.h>
#include <scsi/scsi_request.h>

#include "scsi_priv.h"
#include "scsi_logging.h"

#define SENSE_TIMEOUT		(10*HZ)
#define START_UNIT_TIMEOUT	(30*HZ)

/*
 * These should *probably* be handled by the host itself.
 * Since it is allowed to sleep, it probably should.
 */
#define BUS_RESET_SETTLE_TIME   (10*HZ)
#define HOST_RESET_SETTLE_TIME  (10*HZ)

/* called with shost->host_lock held */
void scsi_eh_wakeup(struct Scsi_Host *shost)
{
	if (shost->host_busy == shost->host_failed) {
		up(shost->eh_wait);
		SCSI_LOG_ERROR_RECOVERY(5,
				printk("Waking error handler thread\n"));
	}
}

/**
 * scsi_eh_scmd_add - add scsi cmd to error handling.
 * @scmd:	scmd to run eh on.
 * @eh_flag:	optional SCSI_EH flag.
 *
 * Return value:
 *	0 on failure.
 **/
int scsi_eh_scmd_add(struct scsi_cmnd *scmd, int eh_flag)
{
	struct Scsi_Host *shost = scmd->device->host;
	unsigned long flags;

	if (shost->eh_wait == NULL)
		return 0;

	spin_lock_irqsave(shost->host_lock, flags);

	scsi_eh_eflags_set(scmd, eh_flag);
	/*
	 * FIXME: Can we stop setting owner and state.
	 */
	scmd->owner = SCSI_OWNER_ERROR_HANDLER;
	scmd->state = SCSI_STATE_FAILED;
	/*
	 * Set the serial_number_at_timeout to the current
	 * serial_number
	 */
	scmd->serial_number_at_timeout = scmd->serial_number;
	list_add_tail(&scmd->eh_entry, &shost->eh_cmd_q);
	set_bit(SHOST_RECOVERY, &shost->shost_state);
	shost->host_failed++;
	scsi_eh_wakeup(shost);
	spin_unlock_irqrestore(shost->host_lock, flags);
	return 1;
}

/**
 * scsi_add_timer - Start timeout timer for a single scsi command.
 * @scmd:	scsi command that is about to start running.
 * @timeout:	amount of time to allow this command to run.
 * @complete:	timeout function to call if timer isn't canceled.
 *
 * Notes:
 *    This should be turned into an inline function.  Each scsi command
 *    has its own timer, and as it is added to the queue, we set up the
 *    timer.  When the command completes, we cancel the timer.
 **/
void scsi_add_timer(struct scsi_cmnd *scmd, int timeout,
		    void (*complete)(struct scsi_cmnd *))
{

	/*
	 * If the clock was already running for this command, then
	 * first delete the timer.  The timer handling code gets rather
	 * confused if we don't do this.
	 */
	if (scmd->eh_timeout.function)
		del_timer(&scmd->eh_timeout);

	scmd->eh_timeout.data = (unsigned long)scmd;
	scmd->eh_timeout.expires = jiffies + timeout;
	scmd->eh_timeout.function = (void (*)(unsigned long)) complete;

	SCSI_LOG_ERROR_RECOVERY(5, printk("%s: scmd: %p, time:"
					  " %d, (%p)\n", __FUNCTION__,
					  scmd, timeout, complete));

	add_timer(&scmd->eh_timeout);
}

/**
 * scsi_delete_timer - Delete/cancel timer for a given function.
 * @scmd:	Cmd that we are canceling timer for
 *
 * Notes:
 *     This should be turned into an inline function.
 *
 * Return value:
 *     1 if we were able to detach the timer.  0 if we blew it, and the
 *     timer function has already started to run.
 **/
int scsi_delete_timer(struct scsi_cmnd *scmd)
{
	int rtn;

	rtn = del_timer(&scmd->eh_timeout);

	SCSI_LOG_ERROR_RECOVERY(5, printk("%s: scmd: %p,"
					 " rtn: %d\n", __FUNCTION__,
					 scmd, rtn));

	scmd->eh_timeout.data = (unsigned long)NULL;
	scmd->eh_timeout.function = NULL;

	return rtn;
}

/**
 * scsi_times_out - Timeout function for normal scsi commands.
 * @scmd:	Cmd that is timing out.
 *
 * Notes:
 *     We do not need to lock this.  There is the potential for a race
 *     only in that the normal completion handling might run, but if the
 *     normal completion function determines that the timer has already
 *     fired, then it mustn't do anything.
 **/
void scsi_times_out(struct scsi_cmnd *scmd)
{
	scsi_log_completion(scmd, TIMEOUT_ERROR);

	if (scmd->device->host->hostt->eh_timed_out)
		switch (scmd->device->host->hostt->eh_timed_out(scmd)) {
		case EH_HANDLED:
			__scsi_done(scmd);
			return;
		case EH_RESET_TIMER:
			/* This allows a single retry even of a command
			 * with allowed == 0 */
			if (scmd->retries++ > scmd->allowed)
				break;
			scsi_add_timer(scmd, scmd->timeout_per_command,
				       scsi_times_out);
			return;
		case EH_NOT_HANDLED:
			break;
		}

	if (unlikely(!scsi_eh_scmd_add(scmd, SCSI_EH_CANCEL_CMD))) {
		panic("Error handler thread not present at %p %p %s %d",
		      scmd, scmd->device->host, __FILE__, __LINE__);
	}
}

/**
 * scsi_block_when_processing_errors - Prevent cmds from being queued.
 * @sdev:	Device on which we are performing recovery.
 *
 * Description:
 *     We block until the host is out of error recovery, and then check to
 *     see whether the host or the device is offline.
 *
 * Return value:
 *     0 when dev was taken offline by error recovery. 1 OK to proceed.
 **/
int scsi_block_when_processing_errors(struct scsi_device *sdev)
{
	int online;

	wait_event(sdev->host->host_wait, (!test_bit(SHOST_RECOVERY, &sdev->host->shost_state)));

	online = scsi_device_online(sdev);

	SCSI_LOG_ERROR_RECOVERY(5, printk("%s: rtn: %d\n", __FUNCTION__,
					  online));

	return online;
}

#ifdef CONFIG_SCSI_LOGGING
/**
 * scsi_eh_prt_fail_stats - Log info on failures.
 * @shost:	scsi host being recovered.
 * @work_q:	Queue of scsi cmds to process.
 **/
static inline void scsi_eh_prt_fail_stats(struct Scsi_Host *shost,
					  struct list_head *work_q)
{
	struct scsi_cmnd *scmd;
	struct scsi_device *sdev;
	int total_failures = 0;
	int cmd_failed = 0;
	int cmd_cancel = 0;
	int devices_failed = 0;

	shost_for_each_device(sdev, shost) {
		list_for_each_entry(scmd, work_q, eh_entry) {
			if (scmd->device == sdev) {
				++total_failures;
				if (scsi_eh_eflags_chk(scmd,
						       SCSI_EH_CANCEL_CMD))
					++cmd_cancel;
				else 
					++cmd_failed;
			}
		}

		if (cmd_cancel || cmd_failed) {
			SCSI_LOG_ERROR_RECOVERY(3,
				printk("%s: %d:%d:%d:%d cmds failed: %d,"
				       " cancel: %d\n",
				       __FUNCTION__, shost->host_no,
				       sdev->channel, sdev->id, sdev->lun,
				       cmd_failed, cmd_cancel));
			cmd_cancel = 0;
			cmd_failed = 0;
			++devices_failed;
		}
	}

	SCSI_LOG_ERROR_RECOVERY(2, printk("Total of %d commands on %d"
					  " devices require eh work\n",
				  total_failures, devices_failed));
}
#endif

/**
 * scsi_check_sense - Examine scsi cmd sense
 * @scmd:	Cmd to have sense checked.
 *
 * Return value:
 * 	SUCCESS or FAILED or NEEDS_RETRY
 **/
static int scsi_check_sense(struct scsi_cmnd *scmd)
{
	if (!SCSI_SENSE_VALID(scmd))
		return FAILED;

	if (scmd->sense_buffer[2] & 0xe0)
		return SUCCESS;

	switch (scmd->sense_buffer[2] & 0xf) {
	case NO_SENSE:
		return SUCCESS;
	case RECOVERED_ERROR:
		return /* soft_error */ SUCCESS;

	case ABORTED_COMMAND:
		return NEEDS_RETRY;
	case NOT_READY:
	case UNIT_ATTENTION:
		/*
		 * if we are expecting a cc/ua because of a bus reset that we
		 * performed, treat this just as a retry.  otherwise this is
		 * information that we should pass up to the upper-level driver
		 * so that we can deal with it there.
		 */
		if (scmd->device->expecting_cc_ua) {
			scmd->device->expecting_cc_ua = 0;
			return NEEDS_RETRY;
		}
		/*
		 * if the device is in the process of becoming ready, we 
		 * should retry.
		 */
		if ((scmd->sense_buffer[12] == 0x04) &&
			(scmd->sense_buffer[13] == 0x01)) {
			return NEEDS_RETRY;
		}
		/*
		 * if the device is not started, we need to wake
		 * the error handler to start the motor
		 */
		if (scmd->device->allow_restart &&
		    (scmd->sense_buffer[12] == 0x04) &&
		    (scmd->sense_buffer[13] == 0x02)) {
			return FAILED;
		}
		return SUCCESS;

		/* these three are not supported */
	case COPY_ABORTED:
	case VOLUME_OVERFLOW:
	case MISCOMPARE:
		return SUCCESS;

	case MEDIUM_ERROR:
		return NEEDS_RETRY;

	case ILLEGAL_REQUEST:
	case BLANK_CHECK:
	case DATA_PROTECT:
	case HARDWARE_ERROR:
	default:
		return SUCCESS;
	}
}

/**
 * scsi_eh_completed_normally - Disposition a eh cmd on return from LLD.
 * @scmd:	SCSI cmd to examine.
 *
 * Notes:
 *    This is *only* called when we are examining the status of commands
 *    queued during error recovery.  the main difference here is that we
 *    don't allow for the possibility of retries here, and we are a lot
 *    more restrictive about what we consider acceptable.
 **/
static int scsi_eh_completed_normally(struct scsi_cmnd *scmd)
{
	/*
	 * first check the host byte, to see if there is anything in there
	 * that would indicate what we need to do.
	 */
	if (host_byte(scmd->result) == DID_RESET) {
		/*
		 * rats.  we are already in the error handler, so we now
		 * get to try and figure out what to do next.  if the sense
		 * is valid, we have a pretty good idea of what to do.
		 * if not, we mark it as FAILED.
		 */
		return scsi_check_sense(scmd);
	}
	if (host_byte(scmd->result) != DID_OK)
		return FAILED;

	/*
	 * next, check the message byte.
	 */
	if (msg_byte(scmd->result) != COMMAND_COMPLETE)
		return FAILED;

	/*
	 * now, check the status byte to see if this indicates
	 * anything special.
	 */
	switch (status_byte(scmd->result)) {
	case GOOD:
	case COMMAND_TERMINATED:
		return SUCCESS;
	case CHECK_CONDITION:
		return scsi_check_sense(scmd);
	case CONDITION_GOOD:
	case INTERMEDIATE_GOOD:
	case INTERMEDIATE_C_GOOD:
		/*
		 * who knows?  FIXME(eric)
		 */
		return SUCCESS;
	case BUSY:
	case QUEUE_FULL:
	case RESERVATION_CONFLICT:
	default:
		return FAILED;
	}
	return FAILED;
}

/**
 * scsi_eh_times_out - timeout function for error handling.
 * @scmd:	Cmd that is timing out.
 *
 * Notes:
 *    During error handling, the kernel thread will be sleeping waiting
 *    for some action to complete on the device.  our only job is to
 *    record that it timed out, and to wake up the thread.
 **/
static void scsi_eh_times_out(struct scsi_cmnd *scmd)
{
	scsi_eh_eflags_set(scmd, SCSI_EH_REC_TIMEOUT);
	SCSI_LOG_ERROR_RECOVERY(3, printk("%s: scmd:%p\n", __FUNCTION__,
					  scmd));

	if (scmd->device->host->eh_action)
		up(scmd->device->host->eh_action);
}

/**
 * scsi_eh_done - Completion function for error handling.
 * @scmd:	Cmd that is done.
 **/
static void scsi_eh_done(struct scsi_cmnd *scmd)
{
	/*
	 * if the timeout handler is already running, then just set the
	 * flag which says we finished late, and return.  we have no
	 * way of stopping the timeout handler from running, so we must
	 * always defer to it.
	 */
	if (del_timer(&scmd->eh_timeout)) {
		scmd->request->rq_status = RQ_SCSI_DONE;
		scmd->owner = SCSI_OWNER_ERROR_HANDLER;

		SCSI_LOG_ERROR_RECOVERY(3, printk("%s scmd: %p result: %x\n",
					   __FUNCTION__, scmd, scmd->result));

		if (scmd->device->host->eh_action)
			up(scmd->device->host->eh_action);
	}
}

/**
 * scsi_send_eh_cmnd  - send a cmd to a device as part of error recovery.
 * @scmd:	SCSI Cmd to send.
 * @timeout:	Timeout for cmd.
 *
 * Notes:
 *    The initialization of the structures is quite a bit different in
 *    this case, and furthermore, there is a different completion handler
 *    vs scsi_dispatch_cmd.
 * Return value:
 *    SUCCESS or FAILED or NEEDS_RETRY
 **/
static int scsi_send_eh_cmnd(struct scsi_cmnd *scmd, int timeout)
{
	struct Scsi_Host *host = scmd->device->host;
	DECLARE_MUTEX_LOCKED(sem);
	unsigned long flags;
	int rtn = SUCCESS;

	/*
	 * we will use a queued command if possible, otherwise we will
	 * emulate the queuing and calling of completion function ourselves.
	 */
	scmd->owner = SCSI_OWNER_LOWLEVEL;

	if (scmd->device->scsi_level <= SCSI_2)
		scmd->cmnd[1] = (scmd->cmnd[1] & 0x1f) |
			(scmd->device->lun << 5 & 0xe0);

	scsi_add_timer(scmd, timeout, scsi_eh_times_out);

	/*
	 * set up the semaphore so we wait for the command to complete.
	 */
	scmd->device->host->eh_action = &sem;
	scmd->request->rq_status = RQ_SCSI_BUSY;

	spin_lock_irqsave(scmd->device->host->host_lock, flags);
	scsi_log_send(scmd);
	host->hostt->queuecommand(scmd, scsi_eh_done);
	spin_unlock_irqrestore(scmd->device->host->host_lock, flags);

	down(&sem);
	scsi_log_completion(scmd, SUCCESS);

	scmd->device->host->eh_action = NULL;

	/*
	 * see if timeout.  if so, tell the host to forget about it.
	 * in other words, we don't want a callback any more.
	 */
	if (scsi_eh_eflags_chk(scmd, SCSI_EH_REC_TIMEOUT)) {
		scsi_eh_eflags_clr(scmd,  SCSI_EH_REC_TIMEOUT);
		scmd->owner = SCSI_OWNER_LOWLEVEL;

		/*
		 * as far as the low level driver is
		 * concerned, this command is still active, so
		 * we must give the low level driver a chance
		 * to abort it. (db) 
		 *
		 * FIXME(eric) - we are not tracking whether we could
		 * abort a timed out command or not.  not sure how
		 * we should treat them differently anyways.
		 */
		spin_lock_irqsave(scmd->device->host->host_lock, flags);
		if (scmd->device->host->hostt->eh_abort_handler)
			scmd->device->host->hostt->eh_abort_handler(scmd);
		spin_unlock_irqrestore(scmd->device->host->host_lock, flags);
			
		scmd->request->rq_status = RQ_SCSI_DONE;
		scmd->owner = SCSI_OWNER_ERROR_HANDLER;
			
		rtn = FAILED;
	}

	SCSI_LOG_ERROR_RECOVERY(3, printk("%s: scmd: %p, rtn:%x\n",
					  __FUNCTION__, scmd, rtn));

	/*
	 * now examine the actual status codes to see whether the command
	 * actually did complete normally.
	 */
	if (rtn == SUCCESS) {
		rtn = scsi_eh_completed_normally(scmd);
		SCSI_LOG_ERROR_RECOVERY(3,
			printk("%s: scsi_eh_completed_normally %x\n",
			       __FUNCTION__, rtn));
		switch (rtn) {
		case SUCCESS:
		case NEEDS_RETRY:
		case FAILED:
			break;
		default:
			rtn = FAILED;
			break;
		}
	}

	return rtn;
}

/**
 * scsi_request_sense - Request sense data from a particular target.
 * @scmd:	SCSI cmd for request sense.
 *
 * Notes:
 *    Some hosts automatically obtain this information, others require
 *    that we obtain it on our own. This function will *not* return until
 *    the command either times out, or it completes.
 **/
static int scsi_request_sense(struct scsi_cmnd *scmd)
{
	static unsigned char generic_sense[6] =
	{REQUEST_SENSE, 0, 0, 0, 252, 0};
	unsigned char *scsi_result;
	int saved_result;
	int rtn;

	memcpy(scmd->cmnd, generic_sense, sizeof(generic_sense));

	scsi_result = kmalloc(252, GFP_ATOMIC | (scmd->device->host->hostt->unchecked_isa_dma) ? __GFP_DMA : 0);


	if (unlikely(!scsi_result)) {
		printk(KERN_ERR "%s: cannot allocate scsi_result.\n",
		       __FUNCTION__);
		return FAILED;
	}

	/*
	 * zero the sense buffer.  some host adapters automatically always
	 * request sense, so it is not a good idea that
	 * scmd->request_buffer and scmd->sense_buffer point to the same
	 * address (db).  0 is not a valid sense code. 
	 */
	memset(scmd->sense_buffer, 0, sizeof(scmd->sense_buffer));
	memset(scsi_result, 0, 252);

	saved_result = scmd->result;
	scmd->request_buffer = scsi_result;
	scmd->request_bufflen = 252;
	scmd->use_sg = 0;
	scmd->cmd_len = COMMAND_SIZE(scmd->cmnd[0]);
	scmd->sc_data_direction = DMA_FROM_DEVICE;
	scmd->underflow = 0;

	rtn = scsi_send_eh_cmnd(scmd, SENSE_TIMEOUT);

	/* last chance to have valid sense data */
	if(!SCSI_SENSE_VALID(scmd)) {
		memcpy(scmd->sense_buffer, scmd->request_buffer,
		       sizeof(scmd->sense_buffer));
	}

	kfree(scsi_result);

	/*
	 * when we eventually call scsi_finish, we really wish to complete
	 * the original request, so let's restore the original data. (db)
	 */
	scsi_setup_cmd_retry(scmd);
	scmd->result = saved_result;
	return rtn;
}

/**
 * scsi_eh_finish_cmd - Handle a cmd that eh is finished with.
 * @scmd:	Original SCSI cmd that eh has finished.
 * @done_q:	Queue for processed commands.
 *
 * Notes:
 *    We don't want to use the normal command completion while we are are
 *    still handling errors - it may cause other commands to be queued,
 *    and that would disturb what we are doing.  thus we really want to
 *    keep a list of pending commands for final completion, and once we
 *    are ready to leave error handling we handle completion for real.
 **/
static void scsi_eh_finish_cmd(struct scsi_cmnd *scmd,
			       struct list_head *done_q)
{
	scmd->device->host->host_failed--;
	scmd->state = SCSI_STATE_BHQUEUE;

	scsi_eh_eflags_clr_all(scmd);

	/*
	 * set this back so that the upper level can correctly free up
	 * things.
	 */
	scsi_setup_cmd_retry(scmd);
	list_move_tail(&scmd->eh_entry, done_q);
}

/**
 * scsi_eh_get_sense - Get device sense data.
 * @work_q:	Queue of commands to process.
 * @done_q:	Queue of proccessed commands..
 *
 * Description:
 *    See if we need to request sense information.  if so, then get it
 *    now, so we have a better idea of what to do.  
 *
 * Notes:
 *    This has the unfortunate side effect that if a shost adapter does
 *    not automatically request sense information, that we end up shutting
 *    it down before we request it.  All shosts should be doing this
 *    anyways, so for now all I have to say is tough noogies if you end up
 *    in here.  On second thought, this is probably a good idea.  We
 *    *really* want to give authors an incentive to automatically request
 *    this.
 *
 *    In 2.5 this capability will be going away.
 *
 *    Really?  --hch
 **/
static int scsi_eh_get_sense(struct list_head *work_q,
			     struct list_head *done_q)
{
	struct list_head *lh, *lh_sf;
	struct scsi_cmnd *scmd;
	int rtn;

	list_for_each_safe(lh, lh_sf, work_q) {
		scmd = list_entry(lh, struct scsi_cmnd, eh_entry);
		if (scsi_eh_eflags_chk(scmd, SCSI_EH_CANCEL_CMD) ||
		    SCSI_SENSE_VALID(scmd))
			continue;

		SCSI_LOG_ERROR_RECOVERY(2, printk("%s: requesting sense"
						  " for id: %d\n",
						  current->comm,
						  scmd->device->id));
		rtn = scsi_request_sense(scmd);
		if (rtn != SUCCESS)
			continue;

		SCSI_LOG_ERROR_RECOVERY(3, printk("sense requested for %p"
						  " result %x\n", scmd,
						  scmd->result));
		SCSI_LOG_ERROR_RECOVERY(3, scsi_print_sense("bh", scmd));

		rtn = scsi_decide_disposition(scmd);

		/*
		 * if the result was normal, then just pass it along to the
		 * upper level.
		 */
		if (rtn == SUCCESS)
			/* we don't want this command reissued, just
			 * finished with the sense data, so set
			 * retries to the max allowed to ensure it
			 * won't get reissued */
			scmd->retries = scmd->allowed;
		else if (rtn != NEEDS_RETRY)
			continue;

		scsi_eh_finish_cmd(scmd, done_q);
	}

	return list_empty(work_q);
}

/**
 * scsi_try_to_abort_cmd - Ask host to abort a running command.
 * @scmd:	SCSI cmd to abort from Lower Level.
 *
 * Notes:
 *    This function will not return until the user's completion function
 *    has been called.  there is no timeout on this operation.  if the
 *    author of the low-level driver wishes this operation to be timed,
 *    they can provide this facility themselves.  helper functions in
 *    scsi_error.c can be supplied to make this easier to do.
 **/
static int scsi_try_to_abort_cmd(struct scsi_cmnd *scmd)
{
	unsigned long flags;
	int rtn = FAILED;

	if (!scmd->device->host->hostt->eh_abort_handler)
		return rtn;

	/*
	 * scsi_done was called just after the command timed out and before
	 * we had a chance to process it. (db)
	 */
	if (scmd->serial_number == 0)
		return SUCCESS;

	scmd->owner = SCSI_OWNER_LOWLEVEL;

	spin_lock_irqsave(scmd->device->host->host_lock, flags);
	rtn = scmd->device->host->hostt->eh_abort_handler(scmd);
	spin_unlock_irqrestore(scmd->device->host->host_lock, flags);

	return rtn;
}

/**
 * scsi_eh_tur - Send TUR to device.
 * @scmd:	Scsi cmd to send TUR
 *
 * Return value:
 *    0 - Device is ready. 1 - Device NOT ready.
 **/
static int scsi_eh_tur(struct scsi_cmnd *scmd)
{
	static unsigned char tur_command[6] = {TEST_UNIT_READY, 0, 0, 0, 0, 0};
	int retry_cnt = 1, rtn;

retry_tur:
	memcpy(scmd->cmnd, tur_command, sizeof(tur_command));

	/*
	 * zero the sense buffer.  the scsi spec mandates that any
	 * untransferred sense data should be interpreted as being zero.
	 */
	memset(scmd->sense_buffer, 0, sizeof(scmd->sense_buffer));

	scmd->request_buffer = NULL;
	scmd->request_bufflen = 0;
	scmd->use_sg = 0;
	scmd->cmd_len = COMMAND_SIZE(scmd->cmnd[0]);
	scmd->underflow = 0;
	scmd->sc_data_direction = DMA_NONE;

	rtn = scsi_send_eh_cmnd(scmd, SENSE_TIMEOUT);

	/*
	 * when we eventually call scsi_finish, we really wish to complete
	 * the original request, so let's restore the original data. (db)
	 */
	scsi_setup_cmd_retry(scmd);

	/*
	 * hey, we are done.  let's look to see what happened.
	 */
	SCSI_LOG_ERROR_RECOVERY(3, printk("%s: scmd %p rtn %x\n",
		__FUNCTION__, scmd, rtn));
	if (rtn == SUCCESS)
		return 0;
	else if (rtn == NEEDS_RETRY)
		if (retry_cnt--)
			goto retry_tur;
	return 1;
}

/**
 * scsi_eh_abort_cmds - abort canceled commands.
 * @shost:	scsi host being recovered.
 * @eh_done_q:	list_head for processed commands.
 *
 * Decription:
 *    Try and see whether or not it makes sense to try and abort the
 *    running command.  this only works out to be the case if we have one
 *    command that has timed out.  if the command simply failed, it makes
 *    no sense to try and abort the command, since as far as the shost
 *    adapter is concerned, it isn't running.
 **/
static int scsi_eh_abort_cmds(struct list_head *work_q,
			      struct list_head *done_q)
{
	struct list_head *lh, *lh_sf;
	struct scsi_cmnd *scmd;
	int rtn;

	list_for_each_safe(lh, lh_sf, work_q) {
		scmd = list_entry(lh, struct scsi_cmnd, eh_entry);
		if (!scsi_eh_eflags_chk(scmd, SCSI_EH_CANCEL_CMD))
			continue;
		SCSI_LOG_ERROR_RECOVERY(3, printk("%s: aborting cmd:"
						  "0x%p\n", current->comm,
						  scmd));
		rtn = scsi_try_to_abort_cmd(scmd);
		if (rtn == SUCCESS) {
			scsi_eh_eflags_clr(scmd,  SCSI_EH_CANCEL_CMD);
			if (!scsi_device_online(scmd->device) ||
			    !scsi_eh_tur(scmd)) {
				scsi_eh_finish_cmd(scmd, done_q);
			}
				
		} else
			SCSI_LOG_ERROR_RECOVERY(3, printk("%s: aborting"
							  " cmd failed:"
							  "0x%p\n",
							  current->comm,
							  scmd));
	}

	return list_empty(work_q);
}

/**
 * scsi_try_bus_device_reset - Ask host to perform a BDR on a dev
 * @scmd:	SCSI cmd used to send BDR	
 *
 * Notes:
 *    There is no timeout for this operation.  if this operation is
 *    unreliable for a given host, then the host itself needs to put a
 *    timer on it, and set the host back to a consistent state prior to
 *    returning.
 **/
static int scsi_try_bus_device_reset(struct scsi_cmnd *scmd)
{
	unsigned long flags;
	int rtn = FAILED;

	if (!scmd->device->host->hostt->eh_device_reset_handler)
		return rtn;

	scmd->owner = SCSI_OWNER_LOWLEVEL;

	spin_lock_irqsave(scmd->device->host->host_lock, flags);
	rtn = scmd->device->host->hostt->eh_device_reset_handler(scmd);
	spin_unlock_irqrestore(scmd->device->host->host_lock, flags);

	if (rtn == SUCCESS) {
		scmd->device->was_reset = 1;
		scmd->device->expecting_cc_ua = 1;
	}

	return rtn;
}

/**
 * scsi_eh_try_stu - Send START_UNIT to device.
 * @scmd:	Scsi cmd to send START_UNIT
 *
 * Return value:
 *    0 - Device is ready. 1 - Device NOT ready.
 **/
static int scsi_eh_try_stu(struct scsi_cmnd *scmd)
{
	static unsigned char stu_command[6] = {START_STOP, 0, 0, 0, 1, 0};
	int rtn;

	if (!scmd->device->allow_restart)
		return 1;

	memcpy(scmd->cmnd, stu_command, sizeof(stu_command));

	/*
	 * zero the sense buffer.  the scsi spec mandates that any
	 * untransferred sense data should be interpreted as being zero.
	 */
	memset(scmd->sense_buffer, 0, sizeof(scmd->sense_buffer));

	scmd->request_buffer = NULL;
	scmd->request_bufflen = 0;
	scmd->use_sg = 0;
	scmd->cmd_len = COMMAND_SIZE(scmd->cmnd[0]);
	scmd->underflow = 0;
	scmd->sc_data_direction = DMA_NONE;

	rtn = scsi_send_eh_cmnd(scmd, START_UNIT_TIMEOUT);

	/*
	 * when we eventually call scsi_finish, we really wish to complete
	 * the original request, so let's restore the original data. (db)
	 */
	scsi_setup_cmd_retry(scmd);

	/*
	 * hey, we are done.  let's look to see what happened.
	 */
	SCSI_LOG_ERROR_RECOVERY(3, printk("%s: scmd %p rtn %x\n",
		__FUNCTION__, scmd, rtn));
	if (rtn == SUCCESS)
		return 0;
	return 1;
}

 /**
 * scsi_eh_stu - send START_UNIT if needed
 * @shost:	scsi host being recovered.
 * @eh_done_q:	list_head for processed commands.
 *
 * Notes:
 *    If commands are failing due to not ready, initializing command required,
 *	try revalidating the device, which will end up sending a start unit. 
 **/
static int scsi_eh_stu(struct Scsi_Host *shost,
			      struct list_head *work_q,
			      struct list_head *done_q)
{
	struct list_head *lh, *lh_sf;
	struct scsi_cmnd *scmd, *stu_scmd;
	struct scsi_device *sdev;

	shost_for_each_device(sdev, shost) {
		stu_scmd = NULL;
		list_for_each_entry(scmd, work_q, eh_entry)
			if (scmd->device == sdev && SCSI_SENSE_VALID(scmd) &&
			    scsi_check_sense(scmd) == FAILED ) {
				stu_scmd = scmd;
				break;
			}

		if (!stu_scmd)
			continue;

		SCSI_LOG_ERROR_RECOVERY(3, printk("%s: Sending START_UNIT to sdev:"
						  " 0x%p\n", current->comm, sdev));

		if (!scsi_eh_try_stu(stu_scmd)) {
			if (!scsi_device_online(sdev) ||
			    !scsi_eh_tur(stu_scmd)) {
				list_for_each_safe(lh, lh_sf, work_q) {
					scmd = list_entry(lh, struct scsi_cmnd, eh_entry);
					if (scmd->device == sdev)
						scsi_eh_finish_cmd(scmd, done_q);
				}
			}
		} else {
			SCSI_LOG_ERROR_RECOVERY(3,
						printk("%s: START_UNIT failed to sdev:"
						       " 0x%p\n", current->comm, sdev));
		}
	}

	return list_empty(work_q);
}


/**
 * scsi_eh_bus_device_reset - send bdr if needed
 * @shost:	scsi host being recovered.
 * @eh_done_q:	list_head for processed commands.
 *
 * Notes:
 *    Try a bus device reset.  still, look to see whether we have multiple
 *    devices that are jammed or not - if we have multiple devices, it
 *    makes no sense to try bus_device_reset - we really would need to try
 *    a bus_reset instead. 
 **/
static int scsi_eh_bus_device_reset(struct Scsi_Host *shost,
				    struct list_head *work_q,
				    struct list_head *done_q)
{
	struct list_head *lh, *lh_sf;
	struct scsi_cmnd *scmd, *bdr_scmd;
	struct scsi_device *sdev;
	int rtn;

	shost_for_each_device(sdev, shost) {
		bdr_scmd = NULL;
		list_for_each_entry(scmd, work_q, eh_entry)
			if (scmd->device == sdev) {
				bdr_scmd = scmd;
				break;
			}

		if (!bdr_scmd)
			continue;

		SCSI_LOG_ERROR_RECOVERY(3, printk("%s: Sending BDR sdev:"
						  " 0x%p\n", current->comm,
						  sdev));
		rtn = scsi_try_bus_device_reset(bdr_scmd);
		if (rtn == SUCCESS) {
			if (!scsi_device_online(sdev) ||
			    !scsi_eh_tur(bdr_scmd)) {
				list_for_each_safe(lh, lh_sf,
						   work_q) {
					scmd = list_entry(lh, struct
							  scsi_cmnd,
							  eh_entry);
					if (scmd->device == sdev)
						scsi_eh_finish_cmd(scmd,
								   done_q);
				}
			}
		} else {
			SCSI_LOG_ERROR_RECOVERY(3, printk("%s: BDR"
							  " failed sdev:"
							  "0x%p\n",
							  current->comm,
							   sdev));
		}
	}

	return list_empty(work_q);
}

/**
 * scsi_try_bus_reset - ask host to perform a bus reset
 * @scmd:	SCSI cmd to send bus reset.
 **/
static int scsi_try_bus_reset(struct scsi_cmnd *scmd)
{
	unsigned long flags;
	int rtn;

	SCSI_LOG_ERROR_RECOVERY(3, printk("%s: Snd Bus RST\n",
					  __FUNCTION__));
	scmd->owner = SCSI_OWNER_LOWLEVEL;
	scmd->serial_number_at_timeout = scmd->serial_number;

	if (!scmd->device->host->hostt->eh_bus_reset_handler)
		return FAILED;

	spin_lock_irqsave(scmd->device->host->host_lock, flags);
	rtn = scmd->device->host->hostt->eh_bus_reset_handler(scmd);
	spin_unlock_irqrestore(scmd->device->host->host_lock, flags);

	if (rtn == SUCCESS) {
		if (!scmd->device->host->hostt->skip_settle_delay)
			scsi_sleep(BUS_RESET_SETTLE_TIME);
		spin_lock_irqsave(scmd->device->host->host_lock, flags);
		scsi_report_bus_reset(scmd->device->host, scmd->device->channel);
		spin_unlock_irqrestore(scmd->device->host->host_lock, flags);
	}

	return rtn;
}

/**
 * scsi_try_host_reset - ask host adapter to reset itself
 * @scmd:	SCSI cmd to send hsot reset.
 **/
static int scsi_try_host_reset(struct scsi_cmnd *scmd)
{
	unsigned long flags;
	int rtn;

	SCSI_LOG_ERROR_RECOVERY(3, printk("%s: Snd Host RST\n",
					  __FUNCTION__));
	scmd->owner = SCSI_OWNER_LOWLEVEL;
	scmd->serial_number_at_timeout = scmd->serial_number;

	if (!scmd->device->host->hostt->eh_host_reset_handler)
		return FAILED;

	spin_lock_irqsave(scmd->device->host->host_lock, flags);
	rtn = scmd->device->host->hostt->eh_host_reset_handler(scmd);
	spin_unlock_irqrestore(scmd->device->host->host_lock, flags);

	if (rtn == SUCCESS) {
		if (!scmd->device->host->hostt->skip_settle_delay)
			scsi_sleep(HOST_RESET_SETTLE_TIME);
		spin_lock_irqsave(scmd->device->host->host_lock, flags);
		scsi_report_bus_reset(scmd->device->host, scmd->device->channel);
		spin_unlock_irqrestore(scmd->device->host->host_lock, flags);
	}

	return rtn;
}

/**
 * scsi_eh_bus_reset - send a bus reset 
 * @shost:	scsi host being recovered.
 * @eh_done_q:	list_head for processed commands.
 **/
static int scsi_eh_bus_reset(struct Scsi_Host *shost,
			     struct list_head *work_q,
			     struct list_head *done_q)
{
	struct list_head *lh, *lh_sf;
	struct scsi_cmnd *scmd;
	struct scsi_cmnd *chan_scmd;
	unsigned int channel;
	int rtn;

	/*
	 * we really want to loop over the various channels, and do this on
	 * a channel by channel basis.  we should also check to see if any
	 * of the failed commands are on soft_reset devices, and if so, skip
	 * the reset.  
	 */

	for (channel = 0; channel <= shost->max_channel; channel++) {
		chan_scmd = NULL;
		list_for_each_entry(scmd, work_q, eh_entry) {
			if (channel == scmd->device->channel) {
				chan_scmd = scmd;
				break;
				/*
				 * FIXME add back in some support for
				 * soft_reset devices.
				 */
			}
		}

		if (!chan_scmd)
			continue;
		SCSI_LOG_ERROR_RECOVERY(3, printk("%s: Sending BRST chan:"
						  " %d\n", current->comm,
						  channel));
		rtn = scsi_try_bus_reset(chan_scmd);
		if (rtn == SUCCESS) {
			list_for_each_safe(lh, lh_sf, work_q) {
				scmd = list_entry(lh, struct scsi_cmnd,
						  eh_entry);
				if (channel == scmd->device->channel)
					if (!scsi_device_online(scmd->device) ||
					    !scsi_eh_tur(scmd))
						scsi_eh_finish_cmd(scmd,
								   done_q);
			}
		} else {
			SCSI_LOG_ERROR_RECOVERY(3, printk("%s: BRST"
							  " failed chan: %d\n",
							  current->comm,
							  channel));
		}
	}
	return list_empty(work_q);
}

/**
 * scsi_eh_host_reset - send a host reset 
 * @work_q:	list_head for processed commands.
 * @done_q:	list_head for processed commands.
 **/
static int scsi_eh_host_reset(struct list_head *work_q,
			      struct list_head *done_q)
{
	int rtn;
	struct list_head *lh, *lh_sf;
	struct scsi_cmnd *scmd;

	if (!list_empty(work_q)) {
		scmd = list_entry(work_q->next,
				  struct scsi_cmnd, eh_entry);

		SCSI_LOG_ERROR_RECOVERY(3, printk("%s: Sending HRST\n"
						  , current->comm));

		rtn = scsi_try_host_reset(scmd);
		if (rtn == SUCCESS) {
			list_for_each_safe(lh, lh_sf, work_q) {
				scmd = list_entry(lh, struct scsi_cmnd, eh_entry);
				if (!scsi_device_online(scmd->device) ||
				    (!scsi_eh_try_stu(scmd) && !scsi_eh_tur(scmd)) ||
				    !scsi_eh_tur(scmd))
					scsi_eh_finish_cmd(scmd, done_q);
			}
		} else {
			SCSI_LOG_ERROR_RECOVERY(3, printk("%s: HRST"
							  " failed\n",
							  current->comm));
		}
	}
	return list_empty(work_q);
}

/**
 * scsi_eh_offline_sdevs - offline scsi devices that fail to recover
 * @work_q:	list_head for processed commands.
 * @done_q:	list_head for processed commands.
 *
 **/
static void scsi_eh_offline_sdevs(struct list_head *work_q,
				  struct list_head *done_q)
{
	struct list_head *lh, *lh_sf;
	struct scsi_cmnd *scmd;

	list_for_each_safe(lh, lh_sf, work_q) {
		scmd = list_entry(lh, struct scsi_cmnd, eh_entry);
		printk(KERN_INFO "scsi: Device offlined - not"
		       		" ready after error recovery: host"
				" %d channel %d id %d lun %d\n",
				scmd->device->host->host_no,
				scmd->device->channel,
				scmd->device->id,
				scmd->device->lun);
		scsi_device_set_state(scmd->device, SDEV_OFFLINE);
		if (scsi_eh_eflags_chk(scmd, SCSI_EH_CANCEL_CMD)) {
			/*
			 * FIXME: Handle lost cmds.
			 */
		}
		scsi_eh_finish_cmd(scmd, done_q);
	}
	return;
}

/**
 * scsi_sleep_done - timer function for scsi_sleep
 * @sem:	semphore to signal
 *
 **/
static void scsi_sleep_done(unsigned long data)
{
	struct semaphore *sem = (struct semaphore *)data;

	if (sem)
		up(sem);
}

/**
 * scsi_sleep - sleep for specified timeout
 * @timeout:	timeout value
 *
 **/
void scsi_sleep(int timeout)
{
	DECLARE_MUTEX_LOCKED(sem);
	struct timer_list timer;

	init_timer(&timer);
	timer.data = (unsigned long)&sem;
	timer.expires = jiffies + timeout;
	timer.function = (void (*)(unsigned long))scsi_sleep_done;

	SCSI_LOG_ERROR_RECOVERY(5, printk("sleeping for timer tics %d\n",
					  timeout));

	add_timer(&timer);

	down(&sem);
	del_timer(&timer);
}

/**
 * scsi_decide_disposition - Disposition a cmd on return from LLD.
 * @scmd:	SCSI cmd to examine.
 *
 * Notes:
 *    This is *only* called when we are examining the status after sending
 *    out the actual data command.  any commands that are queued for error
 *    recovery (e.g. test_unit_ready) do *not* come through here.
 *
 *    When this routine returns failed, it means the error handler thread
 *    is woken.  In cases where the error code indicates an error that
 *    doesn't require the error handler read (i.e. we don't need to
 *    abort/reset), this function should return SUCCESS.
 **/
int scsi_decide_disposition(struct scsi_cmnd *scmd)
{
	int rtn;

	/*
	 * if the device is offline, then we clearly just pass the result back
	 * up to the top level.
	 */
	if (!scsi_device_online(scmd->device)) {
		SCSI_LOG_ERROR_RECOVERY(5, printk("%s: device offline - report"
						  " as SUCCESS\n",
						  __FUNCTION__));
		return SUCCESS;
	}

	/*
	 * first check the host byte, to see if there is anything in there
	 * that would indicate what we need to do.
	 */
	switch (host_byte(scmd->result)) {
	case DID_PASSTHROUGH:
		/*
		 * no matter what, pass this through to the upper layer.
		 * nuke this special code so that it looks like we are saying
		 * did_ok.
		 */
		scmd->result &= 0xff00ffff;
		return SUCCESS;
	case DID_OK:
		/*
		 * looks good.  drop through, and check the next byte.
		 */
		break;
	case DID_NO_CONNECT:
	case DID_BAD_TARGET:
	case DID_ABORT:
		/*
		 * note - this means that we just report the status back
		 * to the top level driver, not that we actually think
		 * that it indicates SUCCESS.
		 */
		return SUCCESS;
		/*
		 * when the low level driver returns did_soft_error,
		 * it is responsible for keeping an internal retry counter 
		 * in order to avoid endless loops (db)
		 *
		 * actually this is a bug in this function here.  we should
		 * be mindful of the maximum number of retries specified
		 * and not get stuck in a loop.
		 */
	case DID_SOFT_ERROR:
		goto maybe_retry;
	case DID_IMM_RETRY:
		return NEEDS_RETRY;

	case DID_ERROR:
		if (msg_byte(scmd->result) == COMMAND_COMPLETE &&
		    status_byte(scmd->result) == RESERVATION_CONFLICT)
			/*
			 * execute reservation conflict processing code
			 * lower down
			 */
			break;
		/* fallthrough */

	case DID_BUS_BUSY:
	case DID_PARITY:
		goto maybe_retry;
	case DID_TIME_OUT:
		/*
		 * when we scan the bus, we get timeout messages for
		 * these commands if there is no device available.
		 * other hosts report did_no_connect for the same thing.
		 */
		if ((scmd->cmnd[0] == TEST_UNIT_READY ||
		     scmd->cmnd[0] == INQUIRY)) {
			return SUCCESS;
		} else {
			return FAILED;
		}
	case DID_RESET:
		return SUCCESS;
	default:
		return FAILED;
	}

	/*
	 * next, check the message byte.
	 */
	if (msg_byte(scmd->result) != COMMAND_COMPLETE)
		return FAILED;

	/*
	 * check the status byte to see if this indicates anything special.
	 */
	switch (status_byte(scmd->result)) {
	case QUEUE_FULL:
		/*
		 * the case of trying to send too many commands to a
		 * tagged queueing device.
		 */
	case BUSY:
		/*
		 * device can't talk to us at the moment.  Should only
		 * occur (SAM-3) when the task queue is empty, so will cause
		 * the empty queue handling to trigger a stall in the
		 * device.
		 */
		return ADD_TO_MLQUEUE;
	case GOOD:
	case COMMAND_TERMINATED:
		return SUCCESS;
	case CHECK_CONDITION:
		rtn = scsi_check_sense(scmd);
		if (rtn == NEEDS_RETRY)
			goto maybe_retry;
		/* if rtn == FAILED, we have no sense information;
		 * returning FAILED will wake the error handler thread
		 * to collect the sense and redo the decide
		 * disposition */
		return rtn;
	case CONDITION_GOOD:
	case INTERMEDIATE_GOOD:
	case INTERMEDIATE_C_GOOD:
		/*
		 * who knows?  FIXME(eric)
		 */
		return SUCCESS;

	case RESERVATION_CONFLICT:
		printk("scsi%d (%d,%d,%d) : reservation conflict\n",
		       scmd->device->host->host_no, scmd->device->channel,
		       scmd->device->id, scmd->device->lun);
		return SUCCESS; /* causes immediate i/o error */
	default:
		return FAILED;
	}
	return FAILED;

      maybe_retry:

	/* we requeue for retry because the error was retryable, and
	 * the request was not marked fast fail.  Note that above,
	 * even if the request is marked fast fail, we still requeue
	 * for queue congestion conditions (QUEUE_FULL or BUSY) */
	if ((++scmd->retries) < scmd->allowed 
	    && !blk_noretry_request(scmd->request)) {
		return NEEDS_RETRY;
	} else {
		/*
		 * no more retries - report this one back to upper level.
		 */
		return SUCCESS;
	}
}

/**
 * scsi_eh_lock_done - done function for eh door lock request
 * @scmd:	SCSI command block for the door lock request
 *
 * Notes:
 * 	We completed the asynchronous door lock request, and it has either
 * 	locked the door or failed.  We must free the command structures
 * 	associated with this request.
 **/
static void scsi_eh_lock_done(struct scsi_cmnd *scmd)
{
	struct scsi_request *sreq = scmd->sc_request;

	scsi_release_request(sreq);
}


/**
 * scsi_eh_lock_door - Prevent medium removal for the specified device
 * @sdev:	SCSI device to prevent medium removal
 *
 * Locking:
 * 	We must be called from process context; scsi_allocate_request()
 * 	may sleep.
 *
 * Notes:
 * 	We queue up an asynchronous "ALLOW MEDIUM REMOVAL" request on the
 * 	head of the devices request queue, and continue.
 *
 * Bugs:
 * 	scsi_allocate_request() may sleep waiting for existing requests to
 * 	be processed.  However, since we haven't kicked off any request
 * 	processing for this host, this may deadlock.
 *
 *	If scsi_allocate_request() fails for what ever reason, we
 *	completely forget to lock the door.
 **/
static void scsi_eh_lock_door(struct scsi_device *sdev)
{
	struct scsi_request *sreq = scsi_allocate_request(sdev, GFP_KERNEL);

	if (unlikely(!sreq)) {
		printk(KERN_ERR "%s: request allocate failed,"
		       "prevent media removal cmd not sent\n", __FUNCTION__);
		return;
	}

	sreq->sr_cmnd[0] = ALLOW_MEDIUM_REMOVAL;
	sreq->sr_cmnd[1] = 0;
	sreq->sr_cmnd[2] = 0;
	sreq->sr_cmnd[3] = 0;
	sreq->sr_cmnd[4] = SCSI_REMOVAL_PREVENT;
	sreq->sr_cmnd[5] = 0;
	sreq->sr_data_direction = DMA_NONE;
	sreq->sr_bufflen = 0;
	sreq->sr_buffer = NULL;
	sreq->sr_allowed = 5;
	sreq->sr_done = scsi_eh_lock_done;
	sreq->sr_timeout_per_command = 10 * HZ;
	sreq->sr_cmd_len = COMMAND_SIZE(sreq->sr_cmnd[0]);

	scsi_insert_special_req(sreq, 1);
}


/**
 * scsi_restart_operations - restart io operations to the specified host.
 * @shost:	Host we are restarting.
 *
 * Notes:
 *    When we entered the error handler, we blocked all further i/o to
 *    this device.  we need to 'reverse' this process.
 **/
static void scsi_restart_operations(struct Scsi_Host *shost)
{
	struct scsi_device *sdev;

	/*
	 * If the door was locked, we need to insert a door lock request
	 * onto the head of the SCSI request queue for the device.  There
	 * is no point trying to lock the door of an off-line device.
	 */
	shost_for_each_device(sdev, shost) {
		if (scsi_device_online(sdev) && sdev->locked)
			scsi_eh_lock_door(sdev);
	}

	/*
	 * next free up anything directly waiting upon the host.  this
	 * will be requests for character device operations, and also for
	 * ioctls to queued block devices.
	 */
	SCSI_LOG_ERROR_RECOVERY(3, printk("%s: waking up host to restart\n",
					  __FUNCTION__));

	clear_bit(SHOST_RECOVERY, &shost->shost_state);

	wake_up(&shost->host_wait);

	/*
	 * finally we need to re-initiate requests that may be pending.  we will
	 * have had everything blocked while error handling is taking place, and
	 * now that error recovery is done, we will need to ensure that these
	 * requests are started.
	 */
	scsi_run_host_queues(shost);
}

/**
 * scsi_eh_ready_devs - check device ready state and recover if not.
 * @shost: 	host to be recovered.
 * @eh_done_q:	list_head for processed commands.
 *
 **/
static void scsi_eh_ready_devs(struct Scsi_Host *shost,
			       struct list_head *work_q,
			       struct list_head *done_q)
{
	if (!scsi_eh_stu(shost, work_q, done_q))
		if (!scsi_eh_bus_device_reset(shost, work_q, done_q))
			if (!scsi_eh_bus_reset(shost, work_q, done_q))
				if (!scsi_eh_host_reset(work_q, done_q))
					scsi_eh_offline_sdevs(work_q, done_q);
}

/**
 * scsi_eh_flush_done_q - finish processed commands or retry them.
 * @done_q:	list_head of processed commands.
 *
 **/
static void scsi_eh_flush_done_q(struct list_head *done_q)
{
	struct list_head *lh, *lh_sf;
	struct scsi_cmnd *scmd;

	list_for_each_safe(lh, lh_sf, done_q) {
		scmd = list_entry(lh, struct scsi_cmnd, eh_entry);
		list_del_init(lh);
		if (scsi_device_online(scmd->device) &&
		    !blk_noretry_request(scmd->request) &&
		    (++scmd->retries < scmd->allowed)) {
			SCSI_LOG_ERROR_RECOVERY(3, printk("%s: flush"
							  " retry cmd: %p\n",
							  current->comm,
							  scmd));
				scsi_queue_insert(scmd, SCSI_MLQUEUE_EH_RETRY);
		} else {
			if (!scmd->result)
				scmd->result |= (DRIVER_TIMEOUT << 24);
			SCSI_LOG_ERROR_RECOVERY(3, printk("%s: flush finish"
							" cmd: %p\n",
							current->comm, scmd));
			scsi_finish_command(scmd);
		}
	}
}

/**
 * scsi_unjam_host - Attempt to fix a host which has a cmd that failed.
 * @shost:	Host to unjam.
 *
 * Notes:
 *    When we come in here, we *know* that all commands on the bus have
 *    either completed, failed or timed out.  we also know that no further
 *    commands are being sent to the host, so things are relatively quiet
 *    and we have freedom to fiddle with things as we wish.
 *
 *    This is only the *default* implementation.  it is possible for
 *    individual drivers to supply their own version of this function, and
 *    if the maintainer wishes to do this, it is strongly suggested that
 *    this function be taken as a template and modified.  this function
 *    was designed to correctly handle problems for about 95% of the
 *    different cases out there, and it should always provide at least a
 *    reasonable amount of error recovery.
 *
 *    Any command marked 'failed' or 'timeout' must eventually have
 *    scsi_finish_cmd() called for it.  we do all of the retry stuff
 *    here, so when we restart the host after we return it should have an
 *    empty queue.
 **/
static void scsi_unjam_host(struct Scsi_Host *shost)
{
	unsigned long flags;
	LIST_HEAD(eh_work_q);
	LIST_HEAD(eh_done_q);

	spin_lock_irqsave(shost->host_lock, flags);
	list_splice_init(&shost->eh_cmd_q, &eh_work_q);
	spin_unlock_irqrestore(shost->host_lock, flags);

	SCSI_LOG_ERROR_RECOVERY(1, scsi_eh_prt_fail_stats(shost, &eh_work_q));

	if (!scsi_eh_get_sense(&eh_work_q, &eh_done_q))
		if (!scsi_eh_abort_cmds(&eh_work_q, &eh_done_q))
			scsi_eh_ready_devs(shost, &eh_work_q, &eh_done_q);

	scsi_eh_flush_done_q(&eh_done_q);
}

/**
 * scsi_error_handler - Handle errors/timeouts of SCSI cmds.
 * @data:	Host for which we are running.
 *
 * Notes:
 *    This is always run in the context of a kernel thread.  The idea is
 *    that we start this thing up when the kernel starts up (one per host
 *    that we detect), and it immediately goes to sleep and waits for some
 *    event (i.e. failure).  When this takes place, we have the job of
 *    trying to unjam the bus and restarting things.
 **/
int scsi_error_handler(void *data)
{
	struct Scsi_Host *shost = (struct Scsi_Host *) data;
	int rtn;
	DECLARE_MUTEX_LOCKED(sem);

	lock_kernel();

	/*
	 *    Flush resources
	 */

	daemonize("scsi_eh_%d", shost->host_no);

	current->flags |= PF_NOFREEZE;

	shost->eh_wait = &sem;
	shost->ehandler = current;

	unlock_kernel();

	/*
	 * Wake up the thread that created us.
	 */
	SCSI_LOG_ERROR_RECOVERY(3, printk("Wake up parent of"
					  " scsi_eh_%d\n",shost->host_no));

	complete(shost->eh_notify);

	while (1) {
		/*
		 * If we get a signal, it means we are supposed to go
		 * away and die.  This typically happens if the user is
		 * trying to unload a module.
		 */
		SCSI_LOG_ERROR_RECOVERY(1, printk("Error handler"
						  " scsi_eh_%d"
						  " sleeping\n",shost->host_no));

		/*
		 * Note - we always use down_interruptible with the semaphore
		 * even if the module was loaded as part of the kernel.  The
		 * reason is that down() will cause this thread to be counted
		 * in the load average as a running process, and down
		 * interruptible doesn't.  Given that we need to allow this
		 * thread to die if the driver was loaded as a module, using
		 * semaphores isn't unreasonable.
		 */
		down_interruptible(&sem);
		if (shost->eh_kill)
			break;

		SCSI_LOG_ERROR_RECOVERY(1, printk("Error handler"
						  " scsi_eh_%d waking"
						  " up\n",shost->host_no));

		shost->eh_active = 1;

		/*
		 * We have a host that is failing for some reason.  Figure out
		 * what we need to do to get it up and online again (if we can).
		 * If we fail, we end up taking the thing offline.
		 */
		if (shost->hostt->eh_strategy_handler) 
			rtn = shost->hostt->eh_strategy_handler(shost);
		else
			scsi_unjam_host(shost);

		shost->eh_active = 0;

		/*
		 * Note - if the above fails completely, the action is to take
		 * individual devices offline and flush the queue of any
		 * outstanding requests that may have been pending.  When we
		 * restart, we restart any I/O to any other devices on the bus
		 * which are still online.
		 */
		scsi_restart_operations(shost);

	}

	SCSI_LOG_ERROR_RECOVERY(1, printk("Error handler scsi_eh_%d"
					  " exiting\n",shost->host_no));

	/*
	 * Make sure that nobody tries to wake us up again.
	 */
	shost->eh_wait = NULL;

	/*
	 * Knock this down too.  From this point on, the host is flying
	 * without a pilot.  If this is because the module is being unloaded,
	 * that's fine.  If the user sent a signal to this thing, we are
	 * potentially in real danger.
	 */
	shost->eh_active = 0;
	shost->ehandler = NULL;

	/*
	 * If anyone is waiting for us to exit (i.e. someone trying to unload
	 * a driver), then wake up that process to let them know we are on
	 * the way out the door.
	 */
	complete_and_exit(shost->eh_notify, 0);
	return 0;
}

/*
 * Function:    scsi_report_bus_reset()
 *
 * Purpose:     Utility function used by low-level drivers to report that
 *		they have observed a bus reset on the bus being handled.
 *
 * Arguments:   shost       - Host in question
 *		channel     - channel on which reset was observed.
 *
 * Returns:     Nothing
 *
 * Lock status: Host lock must be held.
 *
 * Notes:       This only needs to be called if the reset is one which
 *		originates from an unknown location.  Resets originated
 *		by the mid-level itself don't need to call this, but there
 *		should be no harm.
 *
 *		The main purpose of this is to make sure that a CHECK_CONDITION
 *		is properly treated.
 */
void scsi_report_bus_reset(struct Scsi_Host *shost, int channel)
{
	struct scsi_device *sdev;

	__shost_for_each_device(sdev, shost) {
		if (channel == sdev->channel) {
			sdev->was_reset = 1;
			sdev->expecting_cc_ua = 1;
		}
	}
}

/*
 * Function:    scsi_report_device_reset()
 *
 * Purpose:     Utility function used by low-level drivers to report that
 *		they have observed a device reset on the device being handled.
 *
 * Arguments:   shost       - Host in question
 *		channel     - channel on which reset was observed
 *		target	    - target on which reset was observed
 *
 * Returns:     Nothing
 *
 * Lock status: Host lock must be held
 *
 * Notes:       This only needs to be called if the reset is one which
 *		originates from an unknown location.  Resets originated
 *		by the mid-level itself don't need to call this, but there
 *		should be no harm.
 *
 *		The main purpose of this is to make sure that a CHECK_CONDITION
 *		is properly treated.
 */
void scsi_report_device_reset(struct Scsi_Host *shost, int channel, int target)
{
	struct scsi_device *sdev;

	__shost_for_each_device(sdev, shost) {
		if (channel == sdev->channel &&
		    target == sdev->id) {
			sdev->was_reset = 1;
			sdev->expecting_cc_ua = 1;
		}
	}
}

static void
scsi_reset_provider_done_command(struct scsi_cmnd *scmd)
{
}

/*
 * Function:	scsi_reset_provider
 *
 * Purpose:	Send requested reset to a bus or device at any phase.
 *
 * Arguments:	device	- device to send reset to
 *		flag - reset type (see scsi.h)
 *
 * Returns:	SUCCESS/FAILURE.
 *
 * Notes:	This is used by the SCSI Generic driver to provide
 *		Bus/Device reset capability.
 */
int
scsi_reset_provider(struct scsi_device *dev, int flag)
{
	struct scsi_cmnd *scmd = scsi_get_command(dev, GFP_KERNEL);
	struct request req;
	int rtn;

	scmd->request = &req;
	memset(&scmd->eh_timeout, 0, sizeof(scmd->eh_timeout));
	scmd->request->rq_status      	= RQ_SCSI_BUSY;
	scmd->state                   	= SCSI_STATE_INITIALIZING;
	scmd->owner	     		= SCSI_OWNER_MIDLEVEL;
    
	memset(&scmd->cmnd, '\0', sizeof(scmd->cmnd));
    
	scmd->scsi_done		= scsi_reset_provider_done_command;
	scmd->done			= NULL;
	scmd->buffer			= NULL;
	scmd->bufflen			= 0;
	scmd->request_buffer		= NULL;
	scmd->request_bufflen		= 0;
	scmd->internal_timeout		= NORMAL_TIMEOUT;
	scmd->abort_reason		= DID_ABORT;

	scmd->cmd_len			= 0;

	scmd->sc_data_direction		= DMA_BIDIRECTIONAL;
	scmd->sc_request		= NULL;
	scmd->sc_magic			= SCSI_CMND_MAGIC;

	init_timer(&scmd->eh_timeout);

	/*
	 * Sometimes the command can get back into the timer chain,
	 * so use the pid as an identifier.
	 */
	scmd->pid			= 0;

	switch (flag) {
	case SCSI_TRY_RESET_DEVICE:
		rtn = scsi_try_bus_device_reset(scmd);
		if (rtn == SUCCESS)
			break;
		/* FALLTHROUGH */
	case SCSI_TRY_RESET_BUS:
		rtn = scsi_try_bus_reset(scmd);
		if (rtn == SUCCESS)
			break;
		/* FALLTHROUGH */
	case SCSI_TRY_RESET_HOST:
		rtn = scsi_try_host_reset(scmd);
		break;
	default:
		rtn = FAILED;
	}

	scsi_delete_timer(scmd);
	scsi_next_command(scmd);
	return rtn;
}
