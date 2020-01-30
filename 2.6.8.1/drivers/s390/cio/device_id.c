/*
 * drivers/s390/cio/device_id.c
 *
 *    Copyright (C) 2002 IBM Deutschland Entwicklung GmbH,
 *			 IBM Corporation
 *    Author(s): Cornelia Huck(cohuck@de.ibm.com)
 *		 Martin Schwidefsky (schwidefsky@de.ibm.com)
 *
 * Sense ID functions.
 */

#include <linux/module.h>
#include <linux/config.h>
#include <linux/init.h>

#include <asm/ccwdev.h>
#include <asm/delay.h>
#include <asm/cio.h>
#include <asm/lowcore.h>

#include "cio.h"
#include "cio_debug.h"
#include "css.h"
#include "device.h"
#include "ioasm.h"

/*
 * diag210 is used under VM to get information about a virtual device
 */
#ifdef CONFIG_ARCH_S390X
int
diag210(struct diag210 * addr)
{
	/*
	 * diag 210 needs its data below the 2GB border, so we
	 * use a static data area to be sure
	 */
	static struct diag210 diag210_tmp;
	static spinlock_t diag210_lock = SPIN_LOCK_UNLOCKED;
	unsigned long flags;
	int ccode;

	spin_lock_irqsave(&diag210_lock, flags);
	diag210_tmp = *addr;

	asm volatile (
		"   lhi	  %0,-1\n"
		"   sam31\n"
		"   diag  %1,0,0x210\n"
		"0: ipm	  %0\n"
		"   srl	  %0,28\n"
		"1: sam64\n"
		".section __ex_table,\"a\"\n"
		"    .align 8\n"
		"    .quad 0b,1b\n"
		".previous"
		: "=&d" (ccode) : "a" (__pa(&diag210_tmp)) : "cc", "memory" );

	*addr = diag210_tmp;
	spin_unlock_irqrestore(&diag210_lock, flags);

	return ccode;
}
#else
int
diag210(struct diag210 * addr)
{
	int ccode;

	asm volatile (
		"   lhi	  %0,-1\n"
		"   diag  %1,0,0x210\n"
		"0: ipm	  %0\n"
		"   srl	  %0,28\n"
		"1:\n"
		".section __ex_table,\"a\"\n"
		"    .align 4\n"
		"    .long 0b,1b\n"
		".previous"
		: "=&d" (ccode) : "a" (__pa(addr)) : "cc", "memory" );

	return ccode;
}
#endif

/*
 * Input :
 *   devno - device number
 *   ps	   - pointer to sense ID data area
 * Output : none
 */
static void
VM_virtual_device_info (__u16 devno, struct senseid *ps)
{
	static struct {
		int vrdcvcla, vrdcvtyp, cu_type;
	} vm_devices[] = {
		{ 0x08, 0x01, 0x3480 },
		{ 0x08, 0x02, 0x3430 },
		{ 0x08, 0x10, 0x3420 },
		{ 0x08, 0x42, 0x3424 },
		{ 0x08, 0x44, 0x9348 },
		{ 0x08, 0x81, 0x3490 },
		{ 0x08, 0x82, 0x3422 },
		{ 0x10, 0x41, 0x1403 },
		{ 0x10, 0x42, 0x3211 },
		{ 0x10, 0x43, 0x3203 },
		{ 0x10, 0x45, 0x3800 },
		{ 0x10, 0x47, 0x3262 },
		{ 0x10, 0x48, 0x3820 },
		{ 0x10, 0x49, 0x3800 },
		{ 0x10, 0x4a, 0x4245 },
		{ 0x10, 0x4b, 0x4248 },
		{ 0x10, 0x4d, 0x3800 },
		{ 0x10, 0x4e, 0x3820 },
		{ 0x10, 0x4f, 0x3820 },
		{ 0x10, 0x82, 0x2540 },
		{ 0x10, 0x84, 0x3525 },
		{ 0x20, 0x81, 0x2501 },
		{ 0x20, 0x82, 0x2540 },
		{ 0x20, 0x84, 0x3505 },
		{ 0x40, 0x01, 0x3278 },
		{ 0x40, 0x04, 0x3277 },
		{ 0x40, 0x80, 0x2250 },
		{ 0x40, 0xc0, 0x5080 },
		{ 0x80, 0x00, 0x3215 },
	};
	struct diag210 diag_data;
	int ccode, i;

	CIO_TRACE_EVENT (4, "VMvdinf");

	diag_data = (struct diag210) {
		.vrdcdvno = devno,
		.vrdclen = sizeof (diag_data),
	};

	ccode = diag210 (&diag_data);
	ps->reserved = 0xff;

	/* Special case for bloody osa devices. */
	if (diag_data.vrdcvcla == 0x02 &&
	    diag_data.vrdcvtyp == 0x20) {
		ps->cu_type = 0x3088;
		ps->cu_model = 0x60;
		return;
	}
	for (i = 0; i < sizeof(vm_devices) / sizeof(vm_devices[0]); i++)
		if (diag_data.vrdcvcla == vm_devices[i].vrdcvcla &&
		    diag_data.vrdcvtyp == vm_devices[i].vrdcvtyp) {
			ps->cu_type = vm_devices[i].cu_type;
			return;
		}
	CIO_MSG_EVENT(0, "DIAG X'210' for device %04X returned (cc = %d):"
		      "vdev class : %02X, vdev type : %04X \n ...  "
		      "rdev class : %02X, rdev type : %04X, "
		      "rdev model: %02X\n",
		      devno, ccode,
		      diag_data.vrdcvcla, diag_data.vrdcvtyp,
		      diag_data.vrdcrccl, diag_data.vrdccrty,
		      diag_data.vrdccrmd);
}

/*
 * Start Sense ID helper function.
 * Try to obtain the 'control unit'/'device type' information
 *  associated with the subchannel.
 */
static int
__ccw_device_sense_id_start(struct ccw_device *cdev)
{
	struct subchannel *sch;
	struct ccw1 *ccw;
	int ret;

	sch = to_subchannel(cdev->dev.parent);
	/* Setup sense channel program. */
	ccw = cdev->private->iccws;
	if (sch->schib.pmcw.pim != 0x80) {
		/* more than one path installed. */
		ccw->cmd_code = CCW_CMD_SUSPEND_RECONN;
		ccw->cda = 0;
		ccw->count = 0;
		ccw->flags = CCW_FLAG_SLI | CCW_FLAG_CC;
		ccw++;
	}
	ccw->cmd_code = CCW_CMD_SENSE_ID;
	ccw->cda = (__u32) __pa (&cdev->private->senseid);
	ccw->count = sizeof (struct senseid);
	ccw->flags = CCW_FLAG_SLI;

	/* Reset device status. */
	memset(&cdev->private->irb, 0, sizeof(struct irb));

	/* Try on every path. */
	ret = -ENODEV;
	while (cdev->private->imask != 0) {
		if ((sch->opm & cdev->private->imask) != 0 &&
		    cdev->private->iretry > 0) {
			cdev->private->iretry--;
			ret = cio_start (sch, cdev->private->iccws,
					 cdev->private->imask);
			/* ret is 0, -EBUSY, -EACCES or -ENODEV */
			if (ret != -EACCES)
				return ret;
		}
		cdev->private->imask >>= 1;
		cdev->private->iretry = 5;
	}
	return ret;
}

void
ccw_device_sense_id_start(struct ccw_device *cdev)
{
	int ret;

	memset (&cdev->private->senseid, 0, sizeof (struct senseid));
	cdev->private->senseid.cu_type = 0xFFFF;
	cdev->private->imask = 0x80;
	cdev->private->iretry = 5;
	ret = __ccw_device_sense_id_start(cdev);
	if (ret && ret != -EBUSY)
		ccw_device_sense_id_done(cdev, ret);
}

/*
 * Called from interrupt context to check if a valid answer
 * to Sense ID was received.
 */
static int
ccw_device_check_sense_id(struct ccw_device *cdev)
{
	struct subchannel *sch;
	struct irb *irb;

	sch = to_subchannel(cdev->dev.parent);
	irb = &cdev->private->irb;
	/* Did we get a proper answer ? */
	if (cdev->private->senseid.cu_type != 0xFFFF && 
	    cdev->private->senseid.reserved == 0xFF) {
		if (irb->scsw.count < sizeof (struct senseid) - 8)
			cdev->private->flags.esid = 1;
		return 0; /* Success */
	}
	/* Check the error cases. */
	if (irb->scsw.fctl & (SCSW_FCTL_HALT_FUNC | SCSW_FCTL_CLEAR_FUNC))
		return -ETIME;
	if (irb->esw.esw0.erw.cons && (irb->ecw[0] & SNS0_CMD_REJECT)) {
		/*
		 * if the device doesn't support the SenseID
		 *  command further retries wouldn't help ...
		 * NB: We don't check here for intervention required like we
		 *     did before, because tape devices with no tape inserted
		 *     may present this status *in conjunction with* the
		 *     sense id information. So, for intervention required,
		 *     we use the "whack it until it talks" strategy...
		 */
		CIO_MSG_EVENT(2, "SenseID : device %04x on Subchannel %04x "
			      "reports cmd reject\n",
			      cdev->private->devno, sch->irq);
		return -EOPNOTSUPP;
	}
	if (irb->esw.esw0.erw.cons) {
		CIO_MSG_EVENT(2, "SenseID : UC on dev %04x, "
			      "lpum %02X, cnt %02d, sns :"
			      " %02X%02X%02X%02X %02X%02X%02X%02X ...\n",
			      cdev->private->devno,
			      irb->esw.esw0.sublog.lpum,
			      irb->esw.esw0.erw.scnt,
			      irb->ecw[0], irb->ecw[1],
			      irb->ecw[2], irb->ecw[3],
			      irb->ecw[4], irb->ecw[5],
			      irb->ecw[6], irb->ecw[7]);
		return -EAGAIN;
	}
	if (irb->scsw.cc == 3) {
		if ((sch->orb.lpm &
		     sch->schib.pmcw.pim & sch->schib.pmcw.pam) != 0)
			CIO_MSG_EVENT(2, "SenseID : path %02X for device %04x on"
				      " subchannel %04x is 'not operational'\n",
				      sch->orb.lpm, cdev->private->devno,
				      sch->irq);
		return -EACCES;
	}
	/* Hmm, whatever happened, try again. */
	CIO_MSG_EVENT(2, "SenseID : start_IO() for device %04x on "
		      "subchannel %04x returns status %02X%02X\n",
		      cdev->private->devno, sch->irq,
		      irb->scsw.dstat, irb->scsw.cstat);
	return -EAGAIN;
}

/*
 * Got interrupt for Sense ID.
 */
void
ccw_device_sense_id_irq(struct ccw_device *cdev, enum dev_event dev_event)
{
	struct subchannel *sch;
	struct irb *irb;
	int ret;

	sch = to_subchannel(cdev->dev.parent);
	irb = (struct irb *) __LC_IRB;
	/* Retry sense id for cc=1. */
	if (irb->scsw.stctl ==
	    (SCSW_STCTL_STATUS_PEND | SCSW_STCTL_ALERT_STATUS)) {
		if (irb->scsw.cc == 1) {
			ret = __ccw_device_sense_id_start(cdev);
			if (ret && ret != -EBUSY)
				ccw_device_sense_id_done(cdev, ret);
		}
		return;
	}
	if (ccw_device_accumulate_and_sense(cdev, irb) != 0)
		return;
	ret = ccw_device_check_sense_id(cdev);
	switch (ret) {
	/* 0, -ETIME, -EOPNOTSUPP, -EAGAIN or -EACCES */
	case 0:			/* Sense id succeeded. */
	case -ETIME:		/* Sense id stopped by timeout. */
		ccw_device_sense_id_done(cdev, ret);
		break;
	case -EACCES:		/* channel is not operational. */
		sch->lpm &= ~cdev->private->imask;
		cdev->private->imask >>= 1;
		cdev->private->iretry = 5;
		/* fall through. */
	case -EAGAIN:		/* try again. */
		ret = __ccw_device_sense_id_start(cdev);
		if (ret == 0 || ret == -EBUSY)
			break;
		/* fall through. */
	default:		/* Sense ID failed. Try asking VM. */
		if (MACHINE_IS_VM) {
			VM_virtual_device_info (cdev->private->devno,
						&cdev->private->senseid);
			if (cdev->private->senseid.cu_type != 0xFFFF) {
				/* Got the device information from VM. */
				ccw_device_sense_id_done(cdev, 0);
				return;
			}
		}
		/*
		 * If we can't couldn't identify the device type we
		 *  consider the device "not operational".
		 */
		ccw_device_sense_id_done(cdev, -ENODEV);
		break;
	}
}

EXPORT_SYMBOL(diag210);
