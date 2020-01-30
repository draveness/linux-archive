/*
 * drivers/s390/cio/device_fsm.c
 * finite state machine for device handling
 *
 *    Copyright (C) 2002 IBM Deutschland Entwicklung GmbH,
 *			 IBM Corporation
 *    Author(s): Cornelia Huck (cornelia.huck@de.ibm.com)
 *		 Martin Schwidefsky (schwidefsky@de.ibm.com)
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/string.h>

#include <asm/ccwdev.h>
#include <asm/cio.h>
#include <asm/chpid.h>

#include "cio.h"
#include "cio_debug.h"
#include "css.h"
#include "device.h"
#include "chsc.h"
#include "ioasm.h"
#include "chp.h"

int
device_is_online(struct subchannel *sch)
{
	struct ccw_device *cdev;

	if (!sch->dev.driver_data)
		return 0;
	cdev = sch->dev.driver_data;
	return (cdev->private->state == DEV_STATE_ONLINE);
}

int
device_is_disconnected(struct subchannel *sch)
{
	struct ccw_device *cdev;

	if (!sch->dev.driver_data)
		return 0;
	cdev = sch->dev.driver_data;
	return (cdev->private->state == DEV_STATE_DISCONNECTED ||
		cdev->private->state == DEV_STATE_DISCONNECTED_SENSE_ID);
}

void
device_set_disconnected(struct subchannel *sch)
{
	struct ccw_device *cdev;

	if (!sch->dev.driver_data)
		return;
	cdev = sch->dev.driver_data;
	ccw_device_set_timeout(cdev, 0);
	cdev->private->flags.fake_irb = 0;
	cdev->private->state = DEV_STATE_DISCONNECTED;
}

void device_set_intretry(struct subchannel *sch)
{
	struct ccw_device *cdev;

	cdev = sch->dev.driver_data;
	if (!cdev)
		return;
	cdev->private->flags.intretry = 1;
}

int device_trigger_verify(struct subchannel *sch)
{
	struct ccw_device *cdev;

	cdev = sch->dev.driver_data;
	if (!cdev || !cdev->online)
		return -EINVAL;
	dev_fsm_event(cdev, DEV_EVENT_VERIFY);
	return 0;
}

/*
 * Timeout function. It just triggers a DEV_EVENT_TIMEOUT.
 */
static void
ccw_device_timeout(unsigned long data)
{
	struct ccw_device *cdev;

	cdev = (struct ccw_device *) data;
	spin_lock_irq(cdev->ccwlock);
	dev_fsm_event(cdev, DEV_EVENT_TIMEOUT);
	spin_unlock_irq(cdev->ccwlock);
}

/*
 * Set timeout
 */
void
ccw_device_set_timeout(struct ccw_device *cdev, int expires)
{
	if (expires == 0) {
		del_timer(&cdev->private->timer);
		return;
	}
	if (timer_pending(&cdev->private->timer)) {
		if (mod_timer(&cdev->private->timer, jiffies + expires))
			return;
	}
	cdev->private->timer.function = ccw_device_timeout;
	cdev->private->timer.data = (unsigned long) cdev;
	cdev->private->timer.expires = jiffies + expires;
	add_timer(&cdev->private->timer);
}

/* Kill any pending timers after machine check. */
void
device_kill_pending_timer(struct subchannel *sch)
{
	struct ccw_device *cdev;

	if (!sch->dev.driver_data)
		return;
	cdev = sch->dev.driver_data;
	ccw_device_set_timeout(cdev, 0);
}

/*
 * Cancel running i/o. This is called repeatedly since halt/clear are
 * asynchronous operations. We do one try with cio_cancel, two tries
 * with cio_halt, 255 tries with cio_clear. If everythings fails panic.
 * Returns 0 if device now idle, -ENODEV for device not operational and
 * -EBUSY if an interrupt is expected (either from halt/clear or from a
 * status pending).
 */
int
ccw_device_cancel_halt_clear(struct ccw_device *cdev)
{
	struct subchannel *sch;
	int ret;

	sch = to_subchannel(cdev->dev.parent);
	ret = stsch(sch->schid, &sch->schib);
	if (ret || !sch->schib.pmcw.dnv)
		return -ENODEV; 
	if (!sch->schib.pmcw.ena)
		/* Not operational -> done. */
		return 0;
	/* Stage 1: cancel io. */
	if (!(sch->schib.scsw.actl & SCSW_ACTL_HALT_PEND) &&
	    !(sch->schib.scsw.actl & SCSW_ACTL_CLEAR_PEND)) {
		ret = cio_cancel(sch);
		if (ret != -EINVAL)
			return ret;
		/* cancel io unsuccessful. From now on it is asynchronous. */
		cdev->private->iretry = 3;	/* 3 halt retries. */
	}
	if (!(sch->schib.scsw.actl & SCSW_ACTL_CLEAR_PEND)) {
		/* Stage 2: halt io. */
		if (cdev->private->iretry) {
			cdev->private->iretry--;
			ret = cio_halt(sch);
			if (ret != -EBUSY)
				return (ret == 0) ? -EBUSY : ret;
		}
		/* halt io unsuccessful. */
		cdev->private->iretry = 255;	/* 255 clear retries. */
	}
	/* Stage 3: clear io. */
	if (cdev->private->iretry) {
		cdev->private->iretry--;
		ret = cio_clear (sch);
		return (ret == 0) ? -EBUSY : ret;
	}
	panic("Can't stop i/o on subchannel.\n");
}

static int
ccw_device_handle_oper(struct ccw_device *cdev)
{
	struct subchannel *sch;

	sch = to_subchannel(cdev->dev.parent);
	cdev->private->flags.recog_done = 1;
	/*
	 * Check if cu type and device type still match. If
	 * not, it is certainly another device and we have to
	 * de- and re-register.
	 */
	if (cdev->id.cu_type != cdev->private->senseid.cu_type ||
	    cdev->id.cu_model != cdev->private->senseid.cu_model ||
	    cdev->id.dev_type != cdev->private->senseid.dev_type ||
	    cdev->id.dev_model != cdev->private->senseid.dev_model) {
		PREPARE_WORK(&cdev->private->kick_work,
			     ccw_device_do_unreg_rereg);
		queue_work(ccw_device_work, &cdev->private->kick_work);
		return 0;
	}
	cdev->private->flags.donotify = 1;
	return 1;
}

/*
 * The machine won't give us any notification by machine check if a chpid has
 * been varied online on the SE so we have to find out by magic (i. e. driving
 * the channel subsystem to device selection and updating our path masks).
 */
static void
__recover_lost_chpids(struct subchannel *sch, int old_lpm)
{
	int mask, i;
	struct chp_id chpid;

	chp_id_init(&chpid);
	for (i = 0; i<8; i++) {
		mask = 0x80 >> i;
		if (!(sch->lpm & mask))
			continue;
		if (old_lpm & mask)
			continue;
		chpid.id = sch->schib.pmcw.chpid[i];
		if (!chp_is_registered(chpid))
			css_schedule_eval_all();
	}
}

/*
 * Stop device recognition.
 */
static void
ccw_device_recog_done(struct ccw_device *cdev, int state)
{
	struct subchannel *sch;
	int notify, old_lpm, same_dev;

	sch = to_subchannel(cdev->dev.parent);

	ccw_device_set_timeout(cdev, 0);
	cio_disable_subchannel(sch);
	/*
	 * Now that we tried recognition, we have performed device selection
	 * through ssch() and the path information is up to date.
	 */
	old_lpm = sch->lpm;
	stsch(sch->schid, &sch->schib);
	sch->lpm = sch->schib.pmcw.pam & sch->opm;
	/* Check since device may again have become not operational. */
	if (!sch->schib.pmcw.dnv)
		state = DEV_STATE_NOT_OPER;
	if (cdev->private->state == DEV_STATE_DISCONNECTED_SENSE_ID)
		/* Force reprobe on all chpids. */
		old_lpm = 0;
	if (sch->lpm != old_lpm)
		__recover_lost_chpids(sch, old_lpm);
	if (cdev->private->state == DEV_STATE_DISCONNECTED_SENSE_ID) {
		if (state == DEV_STATE_NOT_OPER) {
			cdev->private->flags.recog_done = 1;
			cdev->private->state = DEV_STATE_DISCONNECTED;
			return;
		}
		/* Boxed devices don't need extra treatment. */
	}
	notify = 0;
	same_dev = 0; /* Keep the compiler quiet... */
	switch (state) {
	case DEV_STATE_NOT_OPER:
		CIO_DEBUG(KERN_WARNING, 2,
			  "cio: SenseID : unknown device %04x on subchannel "
			  "0.%x.%04x\n", cdev->private->dev_id.devno,
			  sch->schid.ssid, sch->schid.sch_no);
		break;
	case DEV_STATE_OFFLINE:
		if (cdev->private->state == DEV_STATE_DISCONNECTED_SENSE_ID) {
			same_dev = ccw_device_handle_oper(cdev);
			notify = 1;
		}
		/* fill out sense information */
		memset(&cdev->id, 0, sizeof(cdev->id));
		cdev->id.cu_type   = cdev->private->senseid.cu_type;
		cdev->id.cu_model  = cdev->private->senseid.cu_model;
		cdev->id.dev_type  = cdev->private->senseid.dev_type;
		cdev->id.dev_model = cdev->private->senseid.dev_model;
		if (notify) {
			cdev->private->state = DEV_STATE_OFFLINE;
			if (same_dev) {
				/* Get device online again. */
				ccw_device_online(cdev);
				wake_up(&cdev->private->wait_q);
			}
			return;
		}
		/* Issue device info message. */
		CIO_DEBUG(KERN_INFO, 2,
			  "cio: SenseID : device 0.%x.%04x reports: "
			  "CU  Type/Mod = %04X/%02X, Dev Type/Mod = "
			  "%04X/%02X\n",
			  cdev->private->dev_id.ssid,
			  cdev->private->dev_id.devno,
			  cdev->id.cu_type, cdev->id.cu_model,
			  cdev->id.dev_type, cdev->id.dev_model);
		break;
	case DEV_STATE_BOXED:
		CIO_DEBUG(KERN_WARNING, 2,
			  "cio: SenseID : boxed device %04x on subchannel "
			  "0.%x.%04x\n", cdev->private->dev_id.devno,
			  sch->schid.ssid, sch->schid.sch_no);
		break;
	}
	cdev->private->state = state;
	io_subchannel_recog_done(cdev);
	if (state != DEV_STATE_NOT_OPER)
		wake_up(&cdev->private->wait_q);
}

/*
 * Function called from device_id.c after sense id has completed.
 */
void
ccw_device_sense_id_done(struct ccw_device *cdev, int err)
{
	switch (err) {
	case 0:
		ccw_device_recog_done(cdev, DEV_STATE_OFFLINE);
		break;
	case -ETIME:		/* Sense id stopped by timeout. */
		ccw_device_recog_done(cdev, DEV_STATE_BOXED);
		break;
	default:
		ccw_device_recog_done(cdev, DEV_STATE_NOT_OPER);
		break;
	}
}

static void
ccw_device_oper_notify(struct work_struct *work)
{
	struct ccw_device_private *priv;
	struct ccw_device *cdev;
	struct subchannel *sch;
	int ret;
	unsigned long flags;

	priv = container_of(work, struct ccw_device_private, kick_work);
	cdev = priv->cdev;
	spin_lock_irqsave(cdev->ccwlock, flags);
	sch = to_subchannel(cdev->dev.parent);
	if (sch->driver && sch->driver->notify) {
		spin_unlock_irqrestore(cdev->ccwlock, flags);
		ret = sch->driver->notify(&sch->dev, CIO_OPER);
		spin_lock_irqsave(cdev->ccwlock, flags);
	} else
		ret = 0;
	if (ret) {
		/* Reenable channel measurements, if needed. */
		spin_unlock_irqrestore(cdev->ccwlock, flags);
		cmf_reenable(cdev);
		spin_lock_irqsave(cdev->ccwlock, flags);
		wake_up(&cdev->private->wait_q);
	}
	spin_unlock_irqrestore(cdev->ccwlock, flags);
	if (!ret)
		/* Driver doesn't want device back. */
		ccw_device_do_unreg_rereg(work);
}

/*
 * Finished with online/offline processing.
 */
static void
ccw_device_done(struct ccw_device *cdev, int state)
{
	struct subchannel *sch;

	sch = to_subchannel(cdev->dev.parent);

	ccw_device_set_timeout(cdev, 0);

	if (state != DEV_STATE_ONLINE)
		cio_disable_subchannel(sch);

	/* Reset device status. */
	memset(&cdev->private->irb, 0, sizeof(struct irb));

	cdev->private->state = state;


	if (state == DEV_STATE_BOXED)
		CIO_DEBUG(KERN_WARNING, 2,
			  "cio: Boxed device %04x on subchannel %04x\n",
			  cdev->private->dev_id.devno, sch->schid.sch_no);

	if (cdev->private->flags.donotify) {
		cdev->private->flags.donotify = 0;
		PREPARE_WORK(&cdev->private->kick_work, ccw_device_oper_notify);
		queue_work(ccw_device_notify_work, &cdev->private->kick_work);
	}
	wake_up(&cdev->private->wait_q);

	if (css_init_done && state != DEV_STATE_ONLINE)
		put_device (&cdev->dev);
}

static int cmp_pgid(struct pgid *p1, struct pgid *p2)
{
	char *c1;
	char *c2;

	c1 = (char *)p1;
	c2 = (char *)p2;

	return memcmp(c1 + 1, c2 + 1, sizeof(struct pgid) - 1);
}

static void __ccw_device_get_common_pgid(struct ccw_device *cdev)
{
	int i;
	int last;

	last = 0;
	for (i = 0; i < 8; i++) {
		if (cdev->private->pgid[i].inf.ps.state1 == SNID_STATE1_RESET)
			/* No PGID yet */
			continue;
		if (cdev->private->pgid[last].inf.ps.state1 ==
		    SNID_STATE1_RESET) {
			/* First non-zero PGID */
			last = i;
			continue;
		}
		if (cmp_pgid(&cdev->private->pgid[i],
			     &cdev->private->pgid[last]) == 0)
			/* Non-conflicting PGIDs */
			continue;

		/* PGID mismatch, can't pathgroup. */
		CIO_MSG_EVENT(0, "SNID - pgid mismatch for device "
			      "0.%x.%04x, can't pathgroup\n",
			      cdev->private->dev_id.ssid,
			      cdev->private->dev_id.devno);
		cdev->private->options.pgroup = 0;
		return;
	}
	if (cdev->private->pgid[last].inf.ps.state1 ==
	    SNID_STATE1_RESET)
		/* No previous pgid found */
		memcpy(&cdev->private->pgid[0], &css[0]->global_pgid,
		       sizeof(struct pgid));
	else
		/* Use existing pgid */
		memcpy(&cdev->private->pgid[0], &cdev->private->pgid[last],
		       sizeof(struct pgid));
}

/*
 * Function called from device_pgid.c after sense path ground has completed.
 */
void
ccw_device_sense_pgid_done(struct ccw_device *cdev, int err)
{
	struct subchannel *sch;

	sch = to_subchannel(cdev->dev.parent);
	switch (err) {
	case -EOPNOTSUPP: /* path grouping not supported, use nop instead. */
		cdev->private->options.pgroup = 0;
		break;
	case 0: /* success */
	case -EACCES: /* partial success, some paths not operational */
		/* Check if all pgids are equal or 0. */
		__ccw_device_get_common_pgid(cdev);
		break;
	case -ETIME:		/* Sense path group id stopped by timeout. */
	case -EUSERS:		/* device is reserved for someone else. */
		ccw_device_done(cdev, DEV_STATE_BOXED);
		return;
	default:
		ccw_device_done(cdev, DEV_STATE_NOT_OPER);
		return;
	}
	/* Start Path Group verification. */
	cdev->private->state = DEV_STATE_VERIFY;
	cdev->private->flags.doverify = 0;
	ccw_device_verify_start(cdev);
}

/*
 * Start device recognition.
 */
int
ccw_device_recognition(struct ccw_device *cdev)
{
	struct subchannel *sch;
	int ret;

	if ((cdev->private->state != DEV_STATE_NOT_OPER) &&
	    (cdev->private->state != DEV_STATE_BOXED))
		return -EINVAL;
	sch = to_subchannel(cdev->dev.parent);
	ret = cio_enable_subchannel(sch, sch->schib.pmcw.isc);
	if (ret != 0)
		/* Couldn't enable the subchannel for i/o. Sick device. */
		return ret;

	/* After 60s the device recognition is considered to have failed. */
	ccw_device_set_timeout(cdev, 60*HZ);

	/*
	 * We used to start here with a sense pgid to find out whether a device
	 * is locked by someone else. Unfortunately, the sense pgid command
	 * code has other meanings on devices predating the path grouping
	 * algorithm, so we start with sense id and box the device after an
	 * timeout (or if sense pgid during path verification detects the device
	 * is locked, as may happen on newer devices).
	 */
	cdev->private->flags.recog_done = 0;
	cdev->private->state = DEV_STATE_SENSE_ID;
	ccw_device_sense_id_start(cdev);
	return 0;
}

/*
 * Handle timeout in device recognition.
 */
static void
ccw_device_recog_timeout(struct ccw_device *cdev, enum dev_event dev_event)
{
	int ret;

	ret = ccw_device_cancel_halt_clear(cdev);
	switch (ret) {
	case 0:
		ccw_device_recog_done(cdev, DEV_STATE_BOXED);
		break;
	case -ENODEV:
		ccw_device_recog_done(cdev, DEV_STATE_NOT_OPER);
		break;
	default:
		ccw_device_set_timeout(cdev, 3*HZ);
	}
}


static void
ccw_device_nopath_notify(struct work_struct *work)
{
	struct ccw_device_private *priv;
	struct ccw_device *cdev;
	struct subchannel *sch;
	int ret;
	unsigned long flags;

	priv = container_of(work, struct ccw_device_private, kick_work);
	cdev = priv->cdev;
	spin_lock_irqsave(cdev->ccwlock, flags);
	sch = to_subchannel(cdev->dev.parent);
	/* Extra sanity. */
	if (sch->lpm)
		goto out_unlock;
	if (sch->driver && sch->driver->notify) {
		spin_unlock_irqrestore(cdev->ccwlock, flags);
		ret = sch->driver->notify(&sch->dev, CIO_NO_PATH);
		spin_lock_irqsave(cdev->ccwlock, flags);
	} else
		ret = 0;
	if (!ret) {
		if (get_device(&sch->dev)) {
			/* Driver doesn't want to keep device. */
			cio_disable_subchannel(sch);
			if (get_device(&cdev->dev)) {
				PREPARE_WORK(&cdev->private->kick_work,
					     ccw_device_call_sch_unregister);
				queue_work(ccw_device_work,
					   &cdev->private->kick_work);
			} else
				put_device(&sch->dev);
		}
	} else {
		cio_disable_subchannel(sch);
		ccw_device_set_timeout(cdev, 0);
		cdev->private->flags.fake_irb = 0;
		cdev->private->state = DEV_STATE_DISCONNECTED;
		wake_up(&cdev->private->wait_q);
	}
out_unlock:
	spin_unlock_irqrestore(cdev->ccwlock, flags);
}

void
ccw_device_verify_done(struct ccw_device *cdev, int err)
{
	struct subchannel *sch;

	sch = to_subchannel(cdev->dev.parent);
	/* Update schib - pom may have changed. */
	stsch(sch->schid, &sch->schib);
	/* Update lpm with verified path mask. */
	sch->lpm = sch->vpm;
	/* Repeat path verification? */
	if (cdev->private->flags.doverify) {
		cdev->private->flags.doverify = 0;
		ccw_device_verify_start(cdev);
		return;
	}
	switch (err) {
	case -EOPNOTSUPP: /* path grouping not supported, just set online. */
		cdev->private->options.pgroup = 0;
	case 0:
		ccw_device_done(cdev, DEV_STATE_ONLINE);
		/* Deliver fake irb to device driver, if needed. */
		if (cdev->private->flags.fake_irb) {
			memset(&cdev->private->irb, 0, sizeof(struct irb));
			cdev->private->irb.scsw.cc = 1;
			cdev->private->irb.scsw.fctl = SCSW_FCTL_START_FUNC;
			cdev->private->irb.scsw.actl = SCSW_ACTL_START_PEND;
			cdev->private->irb.scsw.stctl = SCSW_STCTL_STATUS_PEND;
			cdev->private->flags.fake_irb = 0;
			if (cdev->handler)
				cdev->handler(cdev, cdev->private->intparm,
					      &cdev->private->irb);
			memset(&cdev->private->irb, 0, sizeof(struct irb));
		}
		break;
	case -ETIME:
		/* Reset oper notify indication after verify error. */
		cdev->private->flags.donotify = 0;
		ccw_device_done(cdev, DEV_STATE_BOXED);
		break;
	default:
		/* Reset oper notify indication after verify error. */
		cdev->private->flags.donotify = 0;
		if (cdev->online) {
			PREPARE_WORK(&cdev->private->kick_work,
				     ccw_device_nopath_notify);
			queue_work(ccw_device_notify_work,
				   &cdev->private->kick_work);
		} else
			ccw_device_done(cdev, DEV_STATE_NOT_OPER);
		break;
	}
}

/*
 * Get device online.
 */
int
ccw_device_online(struct ccw_device *cdev)
{
	struct subchannel *sch;
	int ret;

	if ((cdev->private->state != DEV_STATE_OFFLINE) &&
	    (cdev->private->state != DEV_STATE_BOXED))
		return -EINVAL;
	sch = to_subchannel(cdev->dev.parent);
	if (css_init_done && !get_device(&cdev->dev))
		return -ENODEV;
	ret = cio_enable_subchannel(sch, sch->schib.pmcw.isc);
	if (ret != 0) {
		/* Couldn't enable the subchannel for i/o. Sick device. */
		if (ret == -ENODEV)
			dev_fsm_event(cdev, DEV_EVENT_NOTOPER);
		return ret;
	}
	/* Do we want to do path grouping? */
	if (!cdev->private->options.pgroup) {
		/* Start initial path verification. */
		cdev->private->state = DEV_STATE_VERIFY;
		cdev->private->flags.doverify = 0;
		ccw_device_verify_start(cdev);
		return 0;
	}
	/* Do a SensePGID first. */
	cdev->private->state = DEV_STATE_SENSE_PGID;
	ccw_device_sense_pgid_start(cdev);
	return 0;
}

void
ccw_device_disband_done(struct ccw_device *cdev, int err)
{
	switch (err) {
	case 0:
		ccw_device_done(cdev, DEV_STATE_OFFLINE);
		break;
	case -ETIME:
		ccw_device_done(cdev, DEV_STATE_BOXED);
		break;
	default:
		cdev->private->flags.donotify = 0;
		if (get_device(&cdev->dev)) {
			PREPARE_WORK(&cdev->private->kick_work,
				     ccw_device_call_sch_unregister);
			queue_work(ccw_device_work, &cdev->private->kick_work);
		}
		ccw_device_done(cdev, DEV_STATE_NOT_OPER);
		break;
	}
}

/*
 * Shutdown device.
 */
int
ccw_device_offline(struct ccw_device *cdev)
{
	struct subchannel *sch;

	if (ccw_device_is_orphan(cdev)) {
		ccw_device_done(cdev, DEV_STATE_OFFLINE);
		return 0;
	}
	sch = to_subchannel(cdev->dev.parent);
	if (stsch(sch->schid, &sch->schib) || !sch->schib.pmcw.dnv)
		return -ENODEV;
	if (cdev->private->state != DEV_STATE_ONLINE) {
		if (sch->schib.scsw.actl != 0)
			return -EBUSY;
		return -EINVAL;
	}
	if (sch->schib.scsw.actl != 0)
		return -EBUSY;
	/* Are we doing path grouping? */
	if (!cdev->private->options.pgroup) {
		/* No, set state offline immediately. */
		ccw_device_done(cdev, DEV_STATE_OFFLINE);
		return 0;
	}
	/* Start Set Path Group commands. */
	cdev->private->state = DEV_STATE_DISBAND_PGID;
	ccw_device_disband_start(cdev);
	return 0;
}

/*
 * Handle timeout in device online/offline process.
 */
static void
ccw_device_onoff_timeout(struct ccw_device *cdev, enum dev_event dev_event)
{
	int ret;

	ret = ccw_device_cancel_halt_clear(cdev);
	switch (ret) {
	case 0:
		ccw_device_done(cdev, DEV_STATE_BOXED);
		break;
	case -ENODEV:
		ccw_device_done(cdev, DEV_STATE_NOT_OPER);
		break;
	default:
		ccw_device_set_timeout(cdev, 3*HZ);
	}
}

/*
 * Handle not oper event in device recognition.
 */
static void
ccw_device_recog_notoper(struct ccw_device *cdev, enum dev_event dev_event)
{
	ccw_device_recog_done(cdev, DEV_STATE_NOT_OPER);
}

/*
 * Handle not operational event while offline.
 */
static void
ccw_device_offline_notoper(struct ccw_device *cdev, enum dev_event dev_event)
{
	struct subchannel *sch;

	cdev->private->state = DEV_STATE_NOT_OPER;
	sch = to_subchannel(cdev->dev.parent);
	if (get_device(&cdev->dev)) {
		PREPARE_WORK(&cdev->private->kick_work,
			     ccw_device_call_sch_unregister);
		queue_work(ccw_device_work, &cdev->private->kick_work);
	}
	wake_up(&cdev->private->wait_q);
}

/*
 * Handle not operational event while online.
 */
static void
ccw_device_online_notoper(struct ccw_device *cdev, enum dev_event dev_event)
{
	struct subchannel *sch;
	int ret;

	sch = to_subchannel(cdev->dev.parent);
	if (sch->driver->notify) {
		spin_unlock_irq(cdev->ccwlock);
		ret = sch->driver->notify(&sch->dev,
					  sch->lpm ? CIO_GONE : CIO_NO_PATH);
		spin_lock_irq(cdev->ccwlock);
	} else
		ret = 0;
	if (ret) {
		ccw_device_set_timeout(cdev, 0);
		cdev->private->flags.fake_irb = 0;
		cdev->private->state = DEV_STATE_DISCONNECTED;
		wake_up(&cdev->private->wait_q);
		return;
	}
	cdev->private->state = DEV_STATE_NOT_OPER;
	cio_disable_subchannel(sch);
	if (sch->schib.scsw.actl != 0) {
		// FIXME: not-oper indication to device driver ?
		ccw_device_call_handler(cdev);
	}
	if (get_device(&cdev->dev)) {
		PREPARE_WORK(&cdev->private->kick_work,
			     ccw_device_call_sch_unregister);
		queue_work(ccw_device_work, &cdev->private->kick_work);
	}
	wake_up(&cdev->private->wait_q);
}

/*
 * Handle path verification event.
 */
static void
ccw_device_online_verify(struct ccw_device *cdev, enum dev_event dev_event)
{
	struct subchannel *sch;

	if (cdev->private->state == DEV_STATE_W4SENSE) {
		cdev->private->flags.doverify = 1;
		return;
	}
	sch = to_subchannel(cdev->dev.parent);
	/*
	 * Since we might not just be coming from an interrupt from the
	 * subchannel we have to update the schib.
	 */
	stsch(sch->schid, &sch->schib);

	if (sch->schib.scsw.actl != 0 ||
	    (sch->schib.scsw.stctl & SCSW_STCTL_STATUS_PEND) ||
	    (cdev->private->irb.scsw.stctl & SCSW_STCTL_STATUS_PEND)) {
		/*
		 * No final status yet or final status not yet delivered
		 * to the device driver. Can't do path verfication now,
		 * delay until final status was delivered.
		 */
		cdev->private->flags.doverify = 1;
		return;
	}
	/* Device is idle, we can do the path verification. */
	cdev->private->state = DEV_STATE_VERIFY;
	cdev->private->flags.doverify = 0;
	ccw_device_verify_start(cdev);
}

/*
 * Got an interrupt for a normal io (state online).
 */
static void
ccw_device_irq(struct ccw_device *cdev, enum dev_event dev_event)
{
	struct irb *irb;

	irb = (struct irb *) __LC_IRB;
	/* Check for unsolicited interrupt. */
	if ((irb->scsw.stctl ==
	    		(SCSW_STCTL_STATUS_PEND | SCSW_STCTL_ALERT_STATUS))
	    && (!irb->scsw.cc)) {
		if ((irb->scsw.dstat & DEV_STAT_UNIT_CHECK) &&
		    !irb->esw.esw0.erw.cons) {
			/* Unit check but no sense data. Need basic sense. */
			if (ccw_device_do_sense(cdev, irb) != 0)
				goto call_handler_unsol;
			memcpy(&cdev->private->irb, irb, sizeof(struct irb));
			cdev->private->state = DEV_STATE_W4SENSE;
			cdev->private->intparm = 0;
			return;
		}
call_handler_unsol:
		if (cdev->handler)
			cdev->handler (cdev, 0, irb);
		if (cdev->private->flags.doverify)
			ccw_device_online_verify(cdev, 0);
		return;
	}
	/* Accumulate status and find out if a basic sense is needed. */
	ccw_device_accumulate_irb(cdev, irb);
	if (cdev->private->flags.dosense) {
		if (ccw_device_do_sense(cdev, irb) == 0) {
			cdev->private->state = DEV_STATE_W4SENSE;
		}
		return;
	}
	/* Call the handler. */
	if (ccw_device_call_handler(cdev) && cdev->private->flags.doverify)
		/* Start delayed path verification. */
		ccw_device_online_verify(cdev, 0);
}

/*
 * Got an timeout in online state.
 */
static void
ccw_device_online_timeout(struct ccw_device *cdev, enum dev_event dev_event)
{
	int ret;

	ccw_device_set_timeout(cdev, 0);
	ret = ccw_device_cancel_halt_clear(cdev);
	if (ret == -EBUSY) {
		ccw_device_set_timeout(cdev, 3*HZ);
		cdev->private->state = DEV_STATE_TIMEOUT_KILL;
		return;
	}
	if (ret == -ENODEV) {
		struct subchannel *sch;

		sch = to_subchannel(cdev->dev.parent);
		if (!sch->lpm) {
			PREPARE_WORK(&cdev->private->kick_work,
				     ccw_device_nopath_notify);
			queue_work(ccw_device_notify_work,
				   &cdev->private->kick_work);
		} else
			dev_fsm_event(cdev, DEV_EVENT_NOTOPER);
	} else if (cdev->handler)
		cdev->handler(cdev, cdev->private->intparm,
			      ERR_PTR(-ETIMEDOUT));
}

/*
 * Got an interrupt for a basic sense.
 */
static void
ccw_device_w4sense(struct ccw_device *cdev, enum dev_event dev_event)
{
	struct irb *irb;

	irb = (struct irb *) __LC_IRB;
	/* Check for unsolicited interrupt. */
	if (irb->scsw.stctl ==
	    		(SCSW_STCTL_STATUS_PEND | SCSW_STCTL_ALERT_STATUS)) {
		if (irb->scsw.cc == 1)
			/* Basic sense hasn't started. Try again. */
			ccw_device_do_sense(cdev, irb);
		else {
			CIO_MSG_EVENT(2, "Huh? 0.%x.%04x: unsolicited "
				      "interrupt during w4sense...\n",
				      cdev->private->dev_id.ssid,
				      cdev->private->dev_id.devno);
			if (cdev->handler)
				cdev->handler (cdev, 0, irb);
		}
		return;
	}
	/*
	 * Check if a halt or clear has been issued in the meanwhile. If yes,
	 * only deliver the halt/clear interrupt to the device driver as if it
	 * had killed the original request.
	 */
	if (irb->scsw.fctl & (SCSW_FCTL_CLEAR_FUNC | SCSW_FCTL_HALT_FUNC)) {
		/* Retry Basic Sense if requested. */
		if (cdev->private->flags.intretry) {
			cdev->private->flags.intretry = 0;
			ccw_device_do_sense(cdev, irb);
			return;
		}
		cdev->private->flags.dosense = 0;
		memset(&cdev->private->irb, 0, sizeof(struct irb));
		ccw_device_accumulate_irb(cdev, irb);
		goto call_handler;
	}
	/* Add basic sense info to irb. */
	ccw_device_accumulate_basic_sense(cdev, irb);
	if (cdev->private->flags.dosense) {
		/* Another basic sense is needed. */
		ccw_device_do_sense(cdev, irb);
		return;
	}
call_handler:
	cdev->private->state = DEV_STATE_ONLINE;
	/* Call the handler. */
	if (ccw_device_call_handler(cdev) && cdev->private->flags.doverify)
		/* Start delayed path verification. */
		ccw_device_online_verify(cdev, 0);
}

static void
ccw_device_clear_verify(struct ccw_device *cdev, enum dev_event dev_event)
{
	struct irb *irb;

	irb = (struct irb *) __LC_IRB;
	/* Accumulate status. We don't do basic sense. */
	ccw_device_accumulate_irb(cdev, irb);
	/* Remember to clear irb to avoid residuals. */
	memset(&cdev->private->irb, 0, sizeof(struct irb));
	/* Try to start delayed device verification. */
	ccw_device_online_verify(cdev, 0);
	/* Note: Don't call handler for cio initiated clear! */
}

static void
ccw_device_killing_irq(struct ccw_device *cdev, enum dev_event dev_event)
{
	struct subchannel *sch;

	sch = to_subchannel(cdev->dev.parent);
	ccw_device_set_timeout(cdev, 0);
	/* Start delayed path verification. */
	ccw_device_online_verify(cdev, 0);
	/* OK, i/o is dead now. Call interrupt handler. */
	if (cdev->handler)
		cdev->handler(cdev, cdev->private->intparm,
			      ERR_PTR(-EIO));
}

static void
ccw_device_killing_timeout(struct ccw_device *cdev, enum dev_event dev_event)
{
	int ret;

	ret = ccw_device_cancel_halt_clear(cdev);
	if (ret == -EBUSY) {
		ccw_device_set_timeout(cdev, 3*HZ);
		return;
	}
	/* Start delayed path verification. */
	ccw_device_online_verify(cdev, 0);
	if (cdev->handler)
		cdev->handler(cdev, cdev->private->intparm,
			      ERR_PTR(-EIO));
}

void device_kill_io(struct subchannel *sch)
{
	int ret;
	struct ccw_device *cdev;

	cdev = sch->dev.driver_data;
	ret = ccw_device_cancel_halt_clear(cdev);
	if (ret == -EBUSY) {
		ccw_device_set_timeout(cdev, 3*HZ);
		cdev->private->state = DEV_STATE_TIMEOUT_KILL;
		return;
	}
	/* Start delayed path verification. */
	ccw_device_online_verify(cdev, 0);
	if (cdev->handler)
		cdev->handler(cdev, cdev->private->intparm,
			      ERR_PTR(-EIO));
}

static void
ccw_device_delay_verify(struct ccw_device *cdev, enum dev_event dev_event)
{
	/* Start verification after current task finished. */
	cdev->private->flags.doverify = 1;
}

static void
ccw_device_stlck_done(struct ccw_device *cdev, enum dev_event dev_event)
{
	struct irb *irb;

	switch (dev_event) {
	case DEV_EVENT_INTERRUPT:
		irb = (struct irb *) __LC_IRB;
		/* Check for unsolicited interrupt. */
		if ((irb->scsw.stctl ==
		     (SCSW_STCTL_STATUS_PEND | SCSW_STCTL_ALERT_STATUS)) &&
		    (!irb->scsw.cc))
			/* FIXME: we should restart stlck here, but this
			 * is extremely unlikely ... */
			goto out_wakeup;

		ccw_device_accumulate_irb(cdev, irb);
		/* We don't care about basic sense etc. */
		break;
	default: /* timeout */
		break;
	}
out_wakeup:
	wake_up(&cdev->private->wait_q);
}

static void
ccw_device_start_id(struct ccw_device *cdev, enum dev_event dev_event)
{
	struct subchannel *sch;

	sch = to_subchannel(cdev->dev.parent);
	if (cio_enable_subchannel(sch, sch->schib.pmcw.isc) != 0)
		/* Couldn't enable the subchannel for i/o. Sick device. */
		return;

	/* After 60s the device recognition is considered to have failed. */
	ccw_device_set_timeout(cdev, 60*HZ);

	cdev->private->state = DEV_STATE_DISCONNECTED_SENSE_ID;
	ccw_device_sense_id_start(cdev);
}

void
device_trigger_reprobe(struct subchannel *sch)
{
	struct ccw_device *cdev;

	if (!sch->dev.driver_data)
		return;
	cdev = sch->dev.driver_data;
	if (cdev->private->state != DEV_STATE_DISCONNECTED)
		return;

	/* Update some values. */
	if (stsch(sch->schid, &sch->schib))
		return;
	if (!sch->schib.pmcw.dnv)
		return;
	/*
	 * The pim, pam, pom values may not be accurate, but they are the best
	 * we have before performing device selection :/
	 */
	sch->lpm = sch->schib.pmcw.pam & sch->opm;
	/* Re-set some bits in the pmcw that were lost. */
	sch->schib.pmcw.isc = 3;
	sch->schib.pmcw.csense = 1;
	sch->schib.pmcw.ena = 0;
	if ((sch->lpm & (sch->lpm - 1)) != 0)
		sch->schib.pmcw.mp = 1;
	sch->schib.pmcw.intparm = (__u32)(unsigned long)sch;
	/* We should also udate ssd info, but this has to wait. */
	/* Check if this is another device which appeared on the same sch. */
	if (sch->schib.pmcw.dev != cdev->private->dev_id.devno) {
		PREPARE_WORK(&cdev->private->kick_work,
			     ccw_device_move_to_orphanage);
		queue_work(ccw_device_work, &cdev->private->kick_work);
	} else
		ccw_device_start_id(cdev, 0);
}

static void
ccw_device_offline_irq(struct ccw_device *cdev, enum dev_event dev_event)
{
	struct subchannel *sch;

	sch = to_subchannel(cdev->dev.parent);
	/*
	 * An interrupt in state offline means a previous disable was not
	 * successful. Try again.
	 */
	cio_disable_subchannel(sch);
}

static void
ccw_device_change_cmfstate(struct ccw_device *cdev, enum dev_event dev_event)
{
	retry_set_schib(cdev);
	cdev->private->state = DEV_STATE_ONLINE;
	dev_fsm_event(cdev, dev_event);
}

static void ccw_device_update_cmfblock(struct ccw_device *cdev,
				       enum dev_event dev_event)
{
	cmf_retry_copy_block(cdev);
	cdev->private->state = DEV_STATE_ONLINE;
	dev_fsm_event(cdev, dev_event);
}

static void
ccw_device_quiesce_done(struct ccw_device *cdev, enum dev_event dev_event)
{
	ccw_device_set_timeout(cdev, 0);
	if (dev_event == DEV_EVENT_NOTOPER)
		cdev->private->state = DEV_STATE_NOT_OPER;
	else
		cdev->private->state = DEV_STATE_OFFLINE;
	wake_up(&cdev->private->wait_q);
}

static void
ccw_device_quiesce_timeout(struct ccw_device *cdev, enum dev_event dev_event)
{
	int ret;

	ret = ccw_device_cancel_halt_clear(cdev);
	switch (ret) {
	case 0:
		cdev->private->state = DEV_STATE_OFFLINE;
		wake_up(&cdev->private->wait_q);
		break;
	case -ENODEV:
		cdev->private->state = DEV_STATE_NOT_OPER;
		wake_up(&cdev->private->wait_q);
		break;
	default:
		ccw_device_set_timeout(cdev, HZ/10);
	}
}

/*
 * No operation action. This is used e.g. to ignore a timeout event in
 * state offline.
 */
static void
ccw_device_nop(struct ccw_device *cdev, enum dev_event dev_event)
{
}

/*
 * Bug operation action. 
 */
static void
ccw_device_bug(struct ccw_device *cdev, enum dev_event dev_event)
{
	CIO_MSG_EVENT(0, "dev_jumptable[%i][%i] == NULL\n",
		      cdev->private->state, dev_event);
	BUG();
}

/*
 * device statemachine
 */
fsm_func_t *dev_jumptable[NR_DEV_STATES][NR_DEV_EVENTS] = {
	[DEV_STATE_NOT_OPER] = {
		[DEV_EVENT_NOTOPER]	= ccw_device_nop,
		[DEV_EVENT_INTERRUPT]	= ccw_device_bug,
		[DEV_EVENT_TIMEOUT]	= ccw_device_nop,
		[DEV_EVENT_VERIFY]	= ccw_device_nop,
	},
	[DEV_STATE_SENSE_PGID] = {
		[DEV_EVENT_NOTOPER]	= ccw_device_online_notoper,
		[DEV_EVENT_INTERRUPT]	= ccw_device_sense_pgid_irq,
		[DEV_EVENT_TIMEOUT]	= ccw_device_onoff_timeout,
		[DEV_EVENT_VERIFY]	= ccw_device_nop,
	},
	[DEV_STATE_SENSE_ID] = {
		[DEV_EVENT_NOTOPER]	= ccw_device_recog_notoper,
		[DEV_EVENT_INTERRUPT]	= ccw_device_sense_id_irq,
		[DEV_EVENT_TIMEOUT]	= ccw_device_recog_timeout,
		[DEV_EVENT_VERIFY]	= ccw_device_nop,
	},
	[DEV_STATE_OFFLINE] = {
		[DEV_EVENT_NOTOPER]	= ccw_device_offline_notoper,
		[DEV_EVENT_INTERRUPT]	= ccw_device_offline_irq,
		[DEV_EVENT_TIMEOUT]	= ccw_device_nop,
		[DEV_EVENT_VERIFY]	= ccw_device_nop,
	},
	[DEV_STATE_VERIFY] = {
		[DEV_EVENT_NOTOPER]	= ccw_device_online_notoper,
		[DEV_EVENT_INTERRUPT]	= ccw_device_verify_irq,
		[DEV_EVENT_TIMEOUT]	= ccw_device_onoff_timeout,
		[DEV_EVENT_VERIFY]	= ccw_device_delay_verify,
	},
	[DEV_STATE_ONLINE] = {
		[DEV_EVENT_NOTOPER]	= ccw_device_online_notoper,
		[DEV_EVENT_INTERRUPT]	= ccw_device_irq,
		[DEV_EVENT_TIMEOUT]	= ccw_device_online_timeout,
		[DEV_EVENT_VERIFY]	= ccw_device_online_verify,
	},
	[DEV_STATE_W4SENSE] = {
		[DEV_EVENT_NOTOPER]	= ccw_device_online_notoper,
		[DEV_EVENT_INTERRUPT]	= ccw_device_w4sense,
		[DEV_EVENT_TIMEOUT]	= ccw_device_nop,
		[DEV_EVENT_VERIFY]	= ccw_device_online_verify,
	},
	[DEV_STATE_DISBAND_PGID] = {
		[DEV_EVENT_NOTOPER]	= ccw_device_online_notoper,
		[DEV_EVENT_INTERRUPT]	= ccw_device_disband_irq,
		[DEV_EVENT_TIMEOUT]	= ccw_device_onoff_timeout,
		[DEV_EVENT_VERIFY]	= ccw_device_nop,
	},
	[DEV_STATE_BOXED] = {
		[DEV_EVENT_NOTOPER]	= ccw_device_offline_notoper,
		[DEV_EVENT_INTERRUPT]	= ccw_device_stlck_done,
		[DEV_EVENT_TIMEOUT]	= ccw_device_stlck_done,
		[DEV_EVENT_VERIFY]	= ccw_device_nop,
	},
	/* states to wait for i/o completion before doing something */
	[DEV_STATE_CLEAR_VERIFY] = {
		[DEV_EVENT_NOTOPER]     = ccw_device_online_notoper,
		[DEV_EVENT_INTERRUPT]   = ccw_device_clear_verify,
		[DEV_EVENT_TIMEOUT]	= ccw_device_nop,
		[DEV_EVENT_VERIFY]	= ccw_device_nop,
	},
	[DEV_STATE_TIMEOUT_KILL] = {
		[DEV_EVENT_NOTOPER]	= ccw_device_online_notoper,
		[DEV_EVENT_INTERRUPT]	= ccw_device_killing_irq,
		[DEV_EVENT_TIMEOUT]	= ccw_device_killing_timeout,
		[DEV_EVENT_VERIFY]	= ccw_device_nop, //FIXME
	},
	[DEV_STATE_QUIESCE] = {
		[DEV_EVENT_NOTOPER]	= ccw_device_quiesce_done,
		[DEV_EVENT_INTERRUPT]	= ccw_device_quiesce_done,
		[DEV_EVENT_TIMEOUT]	= ccw_device_quiesce_timeout,
		[DEV_EVENT_VERIFY]	= ccw_device_nop,
	},
	/* special states for devices gone not operational */
	[DEV_STATE_DISCONNECTED] = {
		[DEV_EVENT_NOTOPER]	= ccw_device_nop,
		[DEV_EVENT_INTERRUPT]	= ccw_device_start_id,
		[DEV_EVENT_TIMEOUT]	= ccw_device_bug,
		[DEV_EVENT_VERIFY]	= ccw_device_start_id,
	},
	[DEV_STATE_DISCONNECTED_SENSE_ID] = {
		[DEV_EVENT_NOTOPER]	= ccw_device_recog_notoper,
		[DEV_EVENT_INTERRUPT]	= ccw_device_sense_id_irq,
		[DEV_EVENT_TIMEOUT]	= ccw_device_recog_timeout,
		[DEV_EVENT_VERIFY]	= ccw_device_nop,
	},
	[DEV_STATE_CMFCHANGE] = {
		[DEV_EVENT_NOTOPER]	= ccw_device_change_cmfstate,
		[DEV_EVENT_INTERRUPT]	= ccw_device_change_cmfstate,
		[DEV_EVENT_TIMEOUT]	= ccw_device_change_cmfstate,
		[DEV_EVENT_VERIFY]	= ccw_device_change_cmfstate,
	},
	[DEV_STATE_CMFUPDATE] = {
		[DEV_EVENT_NOTOPER]	= ccw_device_update_cmfblock,
		[DEV_EVENT_INTERRUPT]	= ccw_device_update_cmfblock,
		[DEV_EVENT_TIMEOUT]	= ccw_device_update_cmfblock,
		[DEV_EVENT_VERIFY]	= ccw_device_update_cmfblock,
	},
};

/*
 * io_subchannel_irq is called for "real" interrupts or for status
 * pending conditions on msch.
 */
void
io_subchannel_irq (struct device *pdev)
{
	struct ccw_device *cdev;

	cdev = to_subchannel(pdev)->dev.driver_data;

	CIO_TRACE_EVENT (3, "IRQ");
	CIO_TRACE_EVENT (3, pdev->bus_id);
	if (cdev)
		dev_fsm_event(cdev, DEV_EVENT_INTERRUPT);
}

EXPORT_SYMBOL_GPL(ccw_device_set_timeout);
