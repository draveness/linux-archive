/*
 *  drivers/s390/cio/device_ops.c
 *
 *   $Revision: 1.50 $
 *
 *    Copyright (C) 2002 IBM Deutschland Entwicklung GmbH,
 *			 IBM Corporation
 *    Author(s): Martin Schwidefsky (schwidefsky@de.ibm.com)
 *               Cornelia Huck (cohuck@de.ibm.com)
 */
#include <linux/config.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/device.h>
#include <linux/delay.h>

#include <asm/ccwdev.h>
#include <asm/idals.h>
#include <asm/qdio.h>

#include "cio.h"
#include "cio_debug.h"
#include "css.h"
#include "device.h"
#include "qdio.h"

int
ccw_device_set_options(struct ccw_device *cdev, unsigned long flags)
{
       /*
	* The flag usage is mutal exclusive ...
	*/
	if ((flags & CCWDEV_EARLY_NOTIFICATION) &&
	    (flags & CCWDEV_REPORT_ALL))
		return -EINVAL;
	cdev->private->options.fast = (flags & CCWDEV_EARLY_NOTIFICATION) != 0;
	cdev->private->options.repall = (flags & CCWDEV_REPORT_ALL) != 0;
	cdev->private->options.pgroup = (flags & CCWDEV_DO_PATHGROUP) != 0;
	cdev->private->options.force = (flags & CCWDEV_ALLOW_FORCE) != 0;
	return 0;
}

int
ccw_device_clear(struct ccw_device *cdev, unsigned long intparm)
{
	struct subchannel *sch;
	int ret;

	if (!cdev)
		return -ENODEV;
	if (cdev->private->state == DEV_STATE_NOT_OPER)
		return -ENODEV;
	if (cdev->private->state != DEV_STATE_ONLINE &&
	    cdev->private->state != DEV_STATE_W4SENSE)
		return -EINVAL;
	sch = to_subchannel(cdev->dev.parent);
	if (!sch)
		return -ENODEV;
	ret = cio_clear(sch);
	if (ret == 0)
		cdev->private->intparm = intparm;
	return ret;
}

int
ccw_device_start(struct ccw_device *cdev, struct ccw1 *cpa,
		 unsigned long intparm, __u8 lpm, unsigned long flags)
{
	struct subchannel *sch;
	int ret;

	if (!cdev)
		return -ENODEV;
	sch = to_subchannel(cdev->dev.parent);
	if (!sch)
		return -ENODEV;
	if (cdev->private->state == DEV_STATE_NOT_OPER)
		return -ENODEV;
	if (cdev->private->state != DEV_STATE_ONLINE ||
	    ((sch->schib.scsw.stctl & SCSW_STCTL_PRIM_STATUS) &&
	     !(sch->schib.scsw.stctl & SCSW_STCTL_SEC_STATUS)) ||
	    cdev->private->flags.doverify)
		return -EBUSY;
	ret = cio_set_options (sch, flags);
	if (ret)
		return ret;
	ret = cio_start (sch, cpa, lpm);
	if (ret == 0)
		cdev->private->intparm = intparm;
	return ret;
}

int
ccw_device_start_timeout(struct ccw_device *cdev, struct ccw1 *cpa,
			 unsigned long intparm, __u8 lpm, unsigned long flags,
			 int expires)
{
	int ret;

	if (!cdev)
		return -ENODEV;
	ccw_device_set_timeout(cdev, expires);
	ret = ccw_device_start(cdev, cpa, intparm, lpm, flags);
	if (ret != 0)
		ccw_device_set_timeout(cdev, 0);
	return ret;
}

int
ccw_device_halt(struct ccw_device *cdev, unsigned long intparm)
{
	struct subchannel *sch;
	int ret;

	if (!cdev)
		return -ENODEV;
	if (cdev->private->state == DEV_STATE_NOT_OPER)
		return -ENODEV;
	if (cdev->private->state != DEV_STATE_ONLINE &&
	    cdev->private->state != DEV_STATE_W4SENSE)
		return -EINVAL;
	sch = to_subchannel(cdev->dev.parent);
	if (!sch)
		return -ENODEV;
	ret = cio_halt(sch);
	if (ret == 0)
		cdev->private->intparm = intparm;
	return ret;
}

int
ccw_device_resume(struct ccw_device *cdev)
{
	struct subchannel *sch;

	if (!cdev)
		return -ENODEV;
	sch = to_subchannel(cdev->dev.parent);
	if (!sch)
		return -ENODEV;
	if (cdev->private->state == DEV_STATE_NOT_OPER)
		return -ENODEV;
	if (cdev->private->state != DEV_STATE_ONLINE ||
	    !(sch->schib.scsw.actl & SCSW_ACTL_SUSPENDED))
		return -EINVAL;
	return cio_resume(sch);
}

/*
 * Pass interrupt to device driver.
 */
int
ccw_device_call_handler(struct ccw_device *cdev)
{
	struct subchannel *sch;
	unsigned int stctl;
	int ending_status;

	sch = to_subchannel(cdev->dev.parent);

	/*
	 * we allow for the device action handler if .
	 *  - we received ending status
	 *  - the action handler requested to see all interrupts
	 *  - we received an intermediate status
	 *  - fast notification was requested (primary status)
	 *  - unsolicited interrupts
	 */
	stctl = cdev->private->irb.scsw.stctl;
	ending_status = (stctl & SCSW_STCTL_SEC_STATUS) ||
		(stctl == (SCSW_STCTL_ALERT_STATUS | SCSW_STCTL_STATUS_PEND)) ||
		(stctl == SCSW_STCTL_STATUS_PEND);
	if (!ending_status &&
	    !cdev->private->options.repall &&
	    !(stctl & SCSW_STCTL_INTER_STATUS) &&
	    !(cdev->private->options.fast &&
	      (stctl & SCSW_STCTL_PRIM_STATUS)))
		return 0;

	/*
	 * Now we are ready to call the device driver interrupt handler.
	 */
	if (cdev->handler)
		cdev->handler(cdev, cdev->private->intparm,
			      &cdev->private->irb);

	/*
	 * Clear the old and now useless interrupt response block.
	 */
	memset(&cdev->private->irb, 0, sizeof(struct irb));

	return 1;
}

/*
 * Search for CIW command in extended sense data.
 */
struct ciw *
ccw_device_get_ciw(struct ccw_device *cdev, __u32 ct)
{
	int ciw_cnt;

	if (cdev->private->flags.esid == 0)
		return NULL;
	for (ciw_cnt = 0; ciw_cnt < MAX_CIWS; ciw_cnt++)
		if (cdev->private->senseid.ciw[ciw_cnt].ct == ct)
			return cdev->private->senseid.ciw + ciw_cnt;
	return NULL;
}

__u8
ccw_device_get_path_mask(struct ccw_device *cdev)
{
	struct subchannel *sch;

	sch = to_subchannel(cdev->dev.parent);
	if (!sch)
		return 0;
	else
		return sch->vpm;
}

static void
ccw_device_wake_up(struct ccw_device *cdev, unsigned long ip, struct irb *irb)
{
	if (!ip)
		/* unsolicited interrupt */
		return;

	/* Abuse intparm for error reporting. */
	if (IS_ERR(irb))
		cdev->private->intparm = -EIO;
	else if ((irb->scsw.dstat !=
		  (DEV_STAT_CHN_END|DEV_STAT_DEV_END)) ||
		 (irb->scsw.cstat != 0)) {
		/*
		 * We didn't get channel end / device end. Check if path
		 * verification has been started; we can retry after it has
		 * finished. We also retry unit checks except for command reject
		 * or intervention required.
		 */
		 if (cdev->private->flags.doverify ||
			 cdev->private->state == DEV_STATE_VERIFY)
			 cdev->private->intparm = -EAGAIN;
		 if ((irb->scsw.dstat & DEV_STAT_UNIT_CHECK) &&
		     !(irb->ecw[0] &
		       (SNS0_CMD_REJECT | SNS0_INTERVENTION_REQ)))
			 cdev->private->intparm = -EAGAIN;
		 else
			 cdev->private->intparm = -EIO;
			 
	} else
		cdev->private->intparm = 0;
	wake_up(&cdev->private->wait_q);
}

static inline int
__ccw_device_retry_loop(struct ccw_device *cdev, struct ccw1 *ccw, long magic)
{
	int ret;
	struct subchannel *sch;

	sch = to_subchannel(cdev->dev.parent);
	do {
		ret = cio_start (sch, ccw, 0);
		if ((ret == -EBUSY) || (ret == -EACCES)) {
			/* Try again later. */
			spin_unlock_irq(&sch->lock);
			msleep(10);
			spin_lock_irq(&sch->lock);
			continue;
		}
		if (ret != 0)
			/* Non-retryable error. */
			break;
		/* Wait for end of request. */
		cdev->private->intparm = magic;
		spin_unlock_irq(&sch->lock);
		wait_event(cdev->private->wait_q,
			   (cdev->private->intparm == -EIO) ||
			   (cdev->private->intparm == -EAGAIN) ||
			   (cdev->private->intparm == 0));
		spin_lock_irq(&sch->lock);
		/* Check at least for channel end / device end */
		if (cdev->private->intparm == -EIO) {
			/* Non-retryable error. */
			ret = -EIO;
			break;
		}
		if (cdev->private->intparm == 0)
			/* Success. */
			break;
		/* Try again later. */
		spin_unlock_irq(&sch->lock);
		msleep(10);
		spin_lock_irq(&sch->lock);
	} while (1);

	return ret;
}

/**
 * read_dev_chars() - read device characteristics
 * @param cdev   target ccw device
 * @param buffer pointer to buffer for rdc data
 * @param length size of rdc data
 * @returns 0 for success, negative error value on failure
 *
 * Context:
 *   called for online device, lock not held
 **/
int
read_dev_chars (struct ccw_device *cdev, void **buffer, int length)
{
	void (*handler)(struct ccw_device *, unsigned long, struct irb *);
	struct subchannel *sch;
	int ret;
	struct ccw1 *rdc_ccw;

	if (!cdev)
		return -ENODEV;
	if (!buffer || !length)
		return -EINVAL;
	sch = to_subchannel(cdev->dev.parent);

	CIO_TRACE_EVENT (4, "rddevch");
	CIO_TRACE_EVENT (4, sch->dev.bus_id);

	rdc_ccw = kmalloc(sizeof(struct ccw1), GFP_KERNEL | GFP_DMA);
	if (!rdc_ccw)
		return -ENOMEM;
	memset(rdc_ccw, 0, sizeof(struct ccw1));
	rdc_ccw->cmd_code = CCW_CMD_RDC;
	rdc_ccw->count = length;
	rdc_ccw->flags = CCW_FLAG_SLI;
	ret = set_normalized_cda (rdc_ccw, (*buffer));
	if (ret != 0) {
		kfree(rdc_ccw);
		return ret;
	}

	spin_lock_irq(&sch->lock);
	/* Save interrupt handler. */
	handler = cdev->handler;
	/* Temporarily install own handler. */
	cdev->handler = ccw_device_wake_up;
	if (cdev->private->state != DEV_STATE_ONLINE)
		ret = -ENODEV;
	else if (((sch->schib.scsw.stctl & SCSW_STCTL_PRIM_STATUS) &&
		  !(sch->schib.scsw.stctl & SCSW_STCTL_SEC_STATUS)) ||
		 cdev->private->flags.doverify)
		ret = -EBUSY;
	else
		/* 0x00D9C4C3 == ebcdic "RDC" */
		ret = __ccw_device_retry_loop(cdev, rdc_ccw, 0x00D9C4C3);

	/* Restore interrupt handler. */
	cdev->handler = handler;
	spin_unlock_irq(&sch->lock);

	clear_normalized_cda (rdc_ccw);
	kfree(rdc_ccw);

	return ret;
}

/*
 *  Read Configuration data
 */
int
read_conf_data (struct ccw_device *cdev, void **buffer, int *length)
{
	void (*handler)(struct ccw_device *, unsigned long, struct irb *);
	struct subchannel *sch;
	struct ciw *ciw;
	char *rcd_buf;
	int ret;
	struct ccw1 *rcd_ccw;

	if (!cdev)
		return -ENODEV;
	if (!buffer || !length)
		return -EINVAL;
	sch = to_subchannel(cdev->dev.parent);

	CIO_TRACE_EVENT (4, "rdconf");
	CIO_TRACE_EVENT (4, sch->dev.bus_id);

	/*
	 * scan for RCD command in extended SenseID data
	 */
	ciw = ccw_device_get_ciw(cdev, CIW_TYPE_RCD);
	if (!ciw || ciw->cmd == 0)
		return -EOPNOTSUPP;

	rcd_ccw = kmalloc(sizeof(struct ccw1), GFP_KERNEL | GFP_DMA);
	if (!rcd_ccw)
		return -ENOMEM;
	memset(rcd_ccw, 0, sizeof(struct ccw1));
	rcd_buf = kmalloc(ciw->count, GFP_KERNEL | GFP_DMA);
 	if (!rcd_buf) {
		kfree(rcd_ccw);
		return -ENOMEM;
	}
 	memset (rcd_buf, 0, ciw->count);
	rcd_ccw->cmd_code = ciw->cmd;
	rcd_ccw->cda = (__u32) __pa (rcd_buf);
	rcd_ccw->count = ciw->count;
	rcd_ccw->flags = CCW_FLAG_SLI;

	spin_lock_irq(&sch->lock);
	/* Save interrupt handler. */
	handler = cdev->handler;
	/* Temporarily install own handler. */
	cdev->handler = ccw_device_wake_up;
	if (cdev->private->state != DEV_STATE_ONLINE)
		ret = -ENODEV;
	else if (((sch->schib.scsw.stctl & SCSW_STCTL_PRIM_STATUS) &&
		  !(sch->schib.scsw.stctl & SCSW_STCTL_SEC_STATUS)) ||
		 cdev->private->flags.doverify)
		ret = -EBUSY;
	else
		/* 0x00D9C3C4 == ebcdic "RCD" */
		ret = __ccw_device_retry_loop(cdev, rcd_ccw, 0x00D9C3C4);

	/* Restore interrupt handler. */
	cdev->handler = handler;
	spin_unlock_irq(&sch->lock);

 	/*
 	 * on success we update the user input parms
 	 */
 	if (ret) {
 		kfree (rcd_buf);
 		*buffer = NULL;
 		*length = 0;
 	} else {
		*length = ciw->count;
		*buffer = rcd_buf;
	}
	kfree(rcd_ccw);

	return ret;
}

/*
 * Try to break the lock on a boxed device.
 */
int
ccw_device_stlck(struct ccw_device *cdev)
{
	void *buf, *buf2;
	unsigned long flags;
	struct subchannel *sch;
	int ret;

	if (!cdev)
		return -ENODEV;

	if (cdev->drv && !cdev->private->options.force)
		return -EINVAL;

	sch = to_subchannel(cdev->dev.parent);
	
	CIO_TRACE_EVENT(2, "stl lock");
	CIO_TRACE_EVENT(2, cdev->dev.bus_id);

	buf = kmalloc(32*sizeof(char), GFP_DMA|GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	buf2 = kmalloc(32*sizeof(char), GFP_DMA|GFP_KERNEL);
	if (!buf2) {
		kfree(buf);
		return -ENOMEM;
	}
	spin_lock_irqsave(&sch->lock, flags);
	ret = cio_enable_subchannel(sch, 3);
	if (ret)
		goto out_unlock;
	/*
	 * Setup ccw. We chain an unconditional reserve and a release so we
	 * only break the lock.
	 */
	cdev->private->iccws[0].cmd_code = CCW_CMD_STLCK;
	cdev->private->iccws[0].cda = (__u32) __pa(buf);
	cdev->private->iccws[0].count = 32;
	cdev->private->iccws[0].flags = CCW_FLAG_CC;
	cdev->private->iccws[1].cmd_code = CCW_CMD_RELEASE;
	cdev->private->iccws[1].cda = (__u32) __pa(buf2);
	cdev->private->iccws[1].count = 32;
	cdev->private->iccws[1].flags = 0;
	ret = cio_start(sch, cdev->private->iccws, 0);
	if (ret) {
		cio_disable_subchannel(sch); //FIXME: return code?
		goto out_unlock;
	}
	cdev->private->irb.scsw.actl |= SCSW_ACTL_START_PEND;
	spin_unlock_irqrestore(&sch->lock, flags);
	wait_event(cdev->private->wait_q, cdev->private->irb.scsw.actl == 0);
	spin_lock_irqsave(&sch->lock, flags);
	cio_disable_subchannel(sch); //FIXME: return code?
	if ((cdev->private->irb.scsw.dstat !=
	     (DEV_STAT_CHN_END|DEV_STAT_DEV_END)) ||
	    (cdev->private->irb.scsw.cstat != 0))
		ret = -EIO;
	/* Clear irb. */
	memset(&cdev->private->irb, 0, sizeof(struct irb));
out_unlock:
	if (buf)
		kfree(buf);
	if (buf2)
		kfree(buf2);
	spin_unlock_irqrestore(&sch->lock, flags);
	return ret;
}

// FIXME: these have to go:

int
_ccw_device_get_subchannel_number(struct ccw_device *cdev)
{
	return cdev->private->irq;
}

int
_ccw_device_get_device_number(struct ccw_device *cdev)
{
	return cdev->private->devno;
}


MODULE_LICENSE("GPL");
EXPORT_SYMBOL(ccw_device_set_options);
EXPORT_SYMBOL(ccw_device_clear);
EXPORT_SYMBOL(ccw_device_halt);
EXPORT_SYMBOL(ccw_device_resume);
EXPORT_SYMBOL(ccw_device_start_timeout);
EXPORT_SYMBOL(ccw_device_start);
EXPORT_SYMBOL(ccw_device_get_ciw);
EXPORT_SYMBOL(ccw_device_get_path_mask);
EXPORT_SYMBOL(read_conf_data);
EXPORT_SYMBOL(read_dev_chars);
EXPORT_SYMBOL(_ccw_device_get_subchannel_number);
EXPORT_SYMBOL(_ccw_device_get_device_number);
