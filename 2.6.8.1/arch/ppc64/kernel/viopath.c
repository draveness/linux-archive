/* -*- linux-c -*-
 *  arch/ppc64/kernel/viopath.c
 *
 *  iSeries Virtual I/O Message Path code
 *
 *  Authors: Dave Boutcher <boutcher@us.ibm.com>
 *           Ryan Arnold <ryanarn@us.ibm.com>
 *           Colin Devilbiss <devilbis@us.ibm.com>
 *
 * (C) Copyright 2000-2003 IBM Corporation
 *
 * This code is used by the iSeries virtual disk, cd,
 * tape, and console to communicate with OS/400 in another
 * partition.
 *
 * This program is free software;  you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) anyu later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/proc_fs.h>
#include <linux/dma-mapping.h>
#include <linux/wait.h>
#include <linux/seq_file.h>
#include <linux/smp_lock.h>

#include <asm/hardirq.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/iSeries/LparData.h>
#include <asm/iSeries/HvLpEvent.h>
#include <asm/iSeries/HvLpConfig.h>
#include <asm/iSeries/HvCallCfg.h>
#include <asm/iSeries/mf.h>
#include <asm/iSeries/iSeries_proc.h>
#include <asm/iSeries/vio.h>

/* Status of the path to each other partition in the system.
 * This is overkill, since we will only ever establish connections
 * to our hosting partition and the primary partition on the system.
 * But this allows for other support in the future.
 */
static struct viopathStatus {
	int isOpen:1;		/* Did we open the path?            */
	int isActive:1;		/* Do we have a mon msg outstanding */
	int users[VIO_MAX_SUBTYPES];
	HvLpInstanceId mSourceInst;
	HvLpInstanceId mTargetInst;
	int numberAllocated;
} viopathStatus[HVMAXARCHITECTEDLPS];

static spinlock_t statuslock = SPIN_LOCK_UNLOCKED;

/*
 * For each kind of event we allocate a buffer that is
 * guaranteed not to cross a page boundary
 */
static unsigned char event_buffer[VIO_MAX_SUBTYPES * 256] __page_aligned;
static atomic_t event_buffer_available[VIO_MAX_SUBTYPES];
static int event_buffer_initialised;

static void handleMonitorEvent(struct HvLpEvent *event);

/*
 * We use this structure to handle asynchronous responses.  The caller
 * blocks on the semaphore and the handler posts the semaphore.  However,
 * if in_atomic() is true in the caller, then wait_atomic is used ...
 */
struct doneAllocParms_t {
	struct semaphore *sem;
	int number;
	atomic_t *wait_atomic;
	int used_wait_atomic;
};

/* Put a sequence number in each mon msg.  The value is not
 * important.  Start at something other than 0 just for
 * readability.  wrapping this is ok.
 */
static u8 viomonseq = 22;

/* Our hosting logical partition.  We get this at startup
 * time, and different modules access this variable directly.
 */
HvLpIndex viopath_hostLp = 0xff;	/* HvLpIndexInvalid */
EXPORT_SYMBOL(viopath_hostLp);
HvLpIndex viopath_ourLp = 0xff;
EXPORT_SYMBOL(viopath_ourLp);

/* For each kind of incoming event we set a pointer to a
 * routine to call.
 */
static vio_event_handler_t *vio_handler[VIO_MAX_SUBTYPES];

#define VIOPATH_KERN_WARN	KERN_WARNING "viopath: "
#define VIOPATH_KERN_INFO	KERN_INFO "viopath: "

static int proc_viopath_show(struct seq_file *m, void *v)
{
	char *buf;
	u16 vlanMap;
	dma_addr_t handle;
	HvLpEvent_Rc hvrc;
	DECLARE_MUTEX_LOCKED(Semaphore);

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		return 0;
	memset(buf, 0, PAGE_SIZE);

	handle = dma_map_single(iSeries_vio_dev, buf, PAGE_SIZE,
				DMA_FROM_DEVICE);

	hvrc = HvCallEvent_signalLpEventFast(viopath_hostLp,
			HvLpEvent_Type_VirtualIo,
			viomajorsubtype_config | vioconfigget,
			HvLpEvent_AckInd_DoAck, HvLpEvent_AckType_ImmediateAck,
			viopath_sourceinst(viopath_hostLp),
			viopath_targetinst(viopath_hostLp),
			(u64)(unsigned long)&Semaphore, VIOVERSION << 16,
			((u64)handle) << 32, PAGE_SIZE, 0, 0);

	if (hvrc != HvLpEvent_Rc_Good)
		printk(VIOPATH_KERN_WARN "hv error on op %d\n", (int)hvrc);

	down(&Semaphore);

	vlanMap = HvLpConfig_getVirtualLanIndexMap();

	buf[PAGE_SIZE-1] = '\0';
	seq_printf(m, "%s", buf);
	seq_printf(m, "AVAILABLE_VETH=%x\n", vlanMap);
	seq_printf(m, "SRLNBR=%c%c%c%c%c%c%c\n",
		   e2a(xItExtVpdPanel.mfgID[2]),
		   e2a(xItExtVpdPanel.mfgID[3]),
		   e2a(xItExtVpdPanel.systemSerial[1]),
		   e2a(xItExtVpdPanel.systemSerial[2]),
		   e2a(xItExtVpdPanel.systemSerial[3]),
		   e2a(xItExtVpdPanel.systemSerial[4]),
		   e2a(xItExtVpdPanel.systemSerial[5]));

	dma_unmap_single(iSeries_vio_dev, handle, PAGE_SIZE, DMA_FROM_DEVICE);
	kfree(buf);

	return 0;
}

static int proc_viopath_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_viopath_show, NULL);
}

static struct file_operations proc_viopath_operations = {
	.open		= proc_viopath_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init vio_proc_init(void)
{
	struct proc_dir_entry *e;

	e = create_proc_entry("iSeries/config", 0, NULL);
	if (e)
		e->proc_fops = &proc_viopath_operations;

        return 0;
}
__initcall(vio_proc_init);

/* See if a given LP is active.  Allow for invalid lps to be passed in
 * and just return invalid
 */
int viopath_isactive(HvLpIndex lp)
{
	if (lp == HvLpIndexInvalid)
		return 0;
	if (lp < HVMAXARCHITECTEDLPS)
		return viopathStatus[lp].isActive;
	else
		return 0;
}
EXPORT_SYMBOL(viopath_isactive);

/*
 * We cache the source and target instance ids for each
 * partition.  
 */
HvLpInstanceId viopath_sourceinst(HvLpIndex lp)
{
	return viopathStatus[lp].mSourceInst;
}
EXPORT_SYMBOL(viopath_sourceinst);

HvLpInstanceId viopath_targetinst(HvLpIndex lp)
{
	return viopathStatus[lp].mTargetInst;
}
EXPORT_SYMBOL(viopath_targetinst);

/*
 * Send a monitor message.  This is a message with the acknowledge
 * bit on that the other side will NOT explicitly acknowledge.  When
 * the other side goes down, the hypervisor will acknowledge any
 * outstanding messages....so we will know when the other side dies.
 */
static void sendMonMsg(HvLpIndex remoteLp)
{
	HvLpEvent_Rc hvrc;

	viopathStatus[remoteLp].mSourceInst =
		HvCallEvent_getSourceLpInstanceId(remoteLp,
				HvLpEvent_Type_VirtualIo);
	viopathStatus[remoteLp].mTargetInst =
		HvCallEvent_getTargetLpInstanceId(remoteLp,
				HvLpEvent_Type_VirtualIo);

	/*
	 * Deliberately ignore the return code here.  if we call this
	 * more than once, we don't care.
	 */
	vio_setHandler(viomajorsubtype_monitor, handleMonitorEvent);

	hvrc = HvCallEvent_signalLpEventFast(remoteLp, HvLpEvent_Type_VirtualIo,
			viomajorsubtype_monitor, HvLpEvent_AckInd_DoAck,
			HvLpEvent_AckType_DeferredAck,
			viopathStatus[remoteLp].mSourceInst,
			viopathStatus[remoteLp].mTargetInst,
			viomonseq++, 0, 0, 0, 0, 0);

	if (hvrc == HvLpEvent_Rc_Good)
		viopathStatus[remoteLp].isActive = 1;
	else {
		printk(VIOPATH_KERN_WARN "could not connect to partition %d\n",
				remoteLp);
		viopathStatus[remoteLp].isActive = 0;
	}
}

static void handleMonitorEvent(struct HvLpEvent *event)
{
	HvLpIndex remoteLp;
	int i;

	/*
	 * This handler is _also_ called as part of the loop
	 * at the end of this routine, so it must be able to
	 * ignore NULL events...
	 */
	if (!event)
		return;

	/*
	 * First see if this is just a normal monitor message from the
	 * other partition
	 */
	if (event->xFlags.xFunction == HvLpEvent_Function_Int) {
		remoteLp = event->xSourceLp;
		if (!viopathStatus[remoteLp].isActive)
			sendMonMsg(remoteLp);
		return;
	}

	/*
	 * This path is for an acknowledgement; the other partition
	 * died
	 */
	remoteLp = event->xTargetLp;
	if ((event->xSourceInstanceId != viopathStatus[remoteLp].mSourceInst) ||
	    (event->xTargetInstanceId != viopathStatus[remoteLp].mTargetInst)) {
		printk(VIOPATH_KERN_WARN "ignoring ack....mismatched instances\n");
		return;
	}

	printk(VIOPATH_KERN_WARN "partition %d ended\n", remoteLp);

	viopathStatus[remoteLp].isActive = 0;

	/*
	 * For each active handler, pass them a NULL
	 * message to indicate that the other partition
	 * died
	 */
	for (i = 0; i < VIO_MAX_SUBTYPES; i++) {
		if (vio_handler[i] != NULL)
			(*vio_handler[i])(NULL);
	}
}

int vio_setHandler(int subtype, vio_event_handler_t *beh)
{
	subtype = subtype >> VIOMAJOR_SUBTYPE_SHIFT;
	if ((subtype < 0) || (subtype >= VIO_MAX_SUBTYPES))
		return -EINVAL;
	if (vio_handler[subtype] != NULL)
		return -EBUSY;
	vio_handler[subtype] = beh;
	return 0;
}
EXPORT_SYMBOL(vio_setHandler);

int vio_clearHandler(int subtype)
{
	subtype = subtype >> VIOMAJOR_SUBTYPE_SHIFT;
	if ((subtype < 0) || (subtype >= VIO_MAX_SUBTYPES))
		return -EINVAL;
	if (vio_handler[subtype] == NULL)
		return -EAGAIN;
	vio_handler[subtype] = NULL;
	return 0;
}
EXPORT_SYMBOL(vio_clearHandler);

static void handleConfig(struct HvLpEvent *event)
{
	if (!event)
		return;
	if (event->xFlags.xFunction == HvLpEvent_Function_Int) {
		printk(VIOPATH_KERN_WARN
		       "unexpected config request from partition %d",
		       event->xSourceLp);

		if ((event->xFlags.xFunction == HvLpEvent_Function_Int) &&
		    (event->xFlags.xAckInd == HvLpEvent_AckInd_DoAck)) {
			event->xRc = HvLpEvent_Rc_InvalidSubtype;
			HvCallEvent_ackLpEvent(event);
		}
		return;
	}

	up((struct semaphore *)event->xCorrelationToken);
}

/*
 * Initialization of the hosting partition
 */
void vio_set_hostlp(void)
{
	/*
	 * If this has already been set then we DON'T want to either change
	 * it or re-register the proc file system
	 */
	if (viopath_hostLp != HvLpIndexInvalid)
		return;

	/*
	 * Figure out our hosting partition.  This isn't allowed to change
	 * while we're active
	 */
	viopath_ourLp = HvLpConfig_getLpIndex();
	viopath_hostLp = HvCallCfg_getHostingLpIndex(viopath_ourLp);

	if (viopath_hostLp != HvLpIndexInvalid)
		vio_setHandler(viomajorsubtype_config, handleConfig);
}
EXPORT_SYMBOL(vio_set_hostlp);

static void vio_handleEvent(struct HvLpEvent *event, struct pt_regs *regs)
{
	HvLpIndex remoteLp;
	int subtype = (event->xSubtype & VIOMAJOR_SUBTYPE_MASK)
		>> VIOMAJOR_SUBTYPE_SHIFT;

	if (event->xFlags.xFunction == HvLpEvent_Function_Int) {
		remoteLp = event->xSourceLp;
		/*
		 * The isActive is checked because if the hosting partition
		 * went down and came back up it would not be active but it
		 * would have different source and target instances, in which
		 * case we'd want to reset them.  This case really protects
		 * against an unauthorized active partition sending interrupts
		 * or acks to this linux partition.
		 */
		if (viopathStatus[remoteLp].isActive
		    && (event->xSourceInstanceId !=
			viopathStatus[remoteLp].mTargetInst)) {
			printk(VIOPATH_KERN_WARN
			       "message from invalid partition. "
			       "int msg rcvd, source inst (%d) doesnt match (%d)\n",
			       viopathStatus[remoteLp].mTargetInst,
			       event->xSourceInstanceId);
			return;
		}

		if (viopathStatus[remoteLp].isActive
		    && (event->xTargetInstanceId !=
			viopathStatus[remoteLp].mSourceInst)) {
			printk(VIOPATH_KERN_WARN
			       "message from invalid partition. "
			       "int msg rcvd, target inst (%d) doesnt match (%d)\n",
			       viopathStatus[remoteLp].mSourceInst,
			       event->xTargetInstanceId);
			return;
		}
	} else {
		remoteLp = event->xTargetLp;
		if (event->xSourceInstanceId !=
		    viopathStatus[remoteLp].mSourceInst) {
			printk(VIOPATH_KERN_WARN
			       "message from invalid partition. "
			       "ack msg rcvd, source inst (%d) doesnt match (%d)\n",
			       viopathStatus[remoteLp].mSourceInst,
			       event->xSourceInstanceId);
			return;
		}

		if (event->xTargetInstanceId !=
		    viopathStatus[remoteLp].mTargetInst) {
			printk(VIOPATH_KERN_WARN
			       "message from invalid partition. "
			       "viopath: ack msg rcvd, target inst (%d) doesnt match (%d)\n",
			       viopathStatus[remoteLp].mTargetInst,
			       event->xTargetInstanceId);
			return;
		}
	}

	if (vio_handler[subtype] == NULL) {
		printk(VIOPATH_KERN_WARN
		       "unexpected virtual io event subtype %d from partition %d\n",
		       event->xSubtype, remoteLp);
		/* No handler.  Ack if necessary */
		if ((event->xFlags.xFunction == HvLpEvent_Function_Int) &&
		    (event->xFlags.xAckInd == HvLpEvent_AckInd_DoAck)) {
			event->xRc = HvLpEvent_Rc_InvalidSubtype;
			HvCallEvent_ackLpEvent(event);
		}
		return;
	}

	/* This innocuous little line is where all the real work happens */
	(*vio_handler[subtype])(event);
}

static void viopath_donealloc(void *parm, int number)
{
	struct doneAllocParms_t *parmsp = (struct doneAllocParms_t *)parm;

	parmsp->number = number;
	if (parmsp->used_wait_atomic)
		atomic_set(parmsp->wait_atomic, 0);
	else
		up(parmsp->sem);
}

static int allocateEvents(HvLpIndex remoteLp, int numEvents)
{
	struct doneAllocParms_t parms;
	DECLARE_MUTEX_LOCKED(Semaphore);
	atomic_t wait_atomic;

	if (in_atomic()) {
		parms.used_wait_atomic = 1;
		atomic_set(&wait_atomic, 1);
		parms.wait_atomic = &wait_atomic;
	} else {
		parms.used_wait_atomic = 0;
		parms.sem = &Semaphore;
	}
	mf_allocateLpEvents(remoteLp, HvLpEvent_Type_VirtualIo, 250,	/* It would be nice to put a real number here! */
			    numEvents, &viopath_donealloc, &parms);
	if (in_atomic()) {
		while (atomic_read(&wait_atomic))
			mb();
	} else
		down(&Semaphore);
	return parms.number;
}

int viopath_open(HvLpIndex remoteLp, int subtype, int numReq)
{
	int i;
	unsigned long flags;
	int tempNumAllocated;

	if ((remoteLp >= HvMaxArchitectedLps) || (remoteLp == HvLpIndexInvalid))
		return -EINVAL;

	subtype = subtype >> VIOMAJOR_SUBTYPE_SHIFT;
	if ((subtype < 0) || (subtype >= VIO_MAX_SUBTYPES))
		return -EINVAL;

	spin_lock_irqsave(&statuslock, flags);

	if (!event_buffer_initialised) {
		for (i = 0; i < VIO_MAX_SUBTYPES; i++)
			atomic_set(&event_buffer_available[i], 1);
		event_buffer_initialised = 1;
	}

	viopathStatus[remoteLp].users[subtype]++;

	if (!viopathStatus[remoteLp].isOpen) {
		viopathStatus[remoteLp].isOpen = 1;
		HvCallEvent_openLpEventPath(remoteLp, HvLpEvent_Type_VirtualIo);

		/*
		 * Don't hold the spinlock during an operation that
		 * can sleep.
		 */
		spin_unlock_irqrestore(&statuslock, flags);
		tempNumAllocated = allocateEvents(remoteLp, 1);
		spin_lock_irqsave(&statuslock, flags);

		viopathStatus[remoteLp].numberAllocated += tempNumAllocated;

		if (viopathStatus[remoteLp].numberAllocated == 0) {
			HvCallEvent_closeLpEventPath(remoteLp,
					HvLpEvent_Type_VirtualIo);

			spin_unlock_irqrestore(&statuslock, flags);
			return -ENOMEM;
		}

		viopathStatus[remoteLp].mSourceInst =
			HvCallEvent_getSourceLpInstanceId(remoteLp,
					HvLpEvent_Type_VirtualIo);
		viopathStatus[remoteLp].mTargetInst =
			HvCallEvent_getTargetLpInstanceId(remoteLp,
					HvLpEvent_Type_VirtualIo);
		HvLpEvent_registerHandler(HvLpEvent_Type_VirtualIo,
					  &vio_handleEvent);
		sendMonMsg(remoteLp);
		printk(VIOPATH_KERN_INFO "opening connection to partition %d, "
				"setting sinst %d, tinst %d\n",
				remoteLp, viopathStatus[remoteLp].mSourceInst,
				viopathStatus[remoteLp].mTargetInst);
	}

	spin_unlock_irqrestore(&statuslock, flags);
	tempNumAllocated = allocateEvents(remoteLp, numReq);
	spin_lock_irqsave(&statuslock, flags);
	viopathStatus[remoteLp].numberAllocated += tempNumAllocated;
	spin_unlock_irqrestore(&statuslock, flags);

	return 0;
}
EXPORT_SYMBOL(viopath_open);

int viopath_close(HvLpIndex remoteLp, int subtype, int numReq)
{
	unsigned long flags;
	int i;
	int numOpen;
	struct doneAllocParms_t doneAllocParms;
	DECLARE_MUTEX_LOCKED(Semaphore);

	if ((remoteLp >= HvMaxArchitectedLps) || (remoteLp == HvLpIndexInvalid))
		return -EINVAL;

	subtype = subtype >> VIOMAJOR_SUBTYPE_SHIFT;
	if ((subtype < 0) || (subtype >= VIO_MAX_SUBTYPES))
		return -EINVAL;

	spin_lock_irqsave(&statuslock, flags);
	/*
	 * If the viopath_close somehow gets called before a
	 * viopath_open it could decrement to -1 which is a non
	 * recoverable state so we'll prevent this from
	 * happening.
	 */
	if (viopathStatus[remoteLp].users[subtype] > 0)
		viopathStatus[remoteLp].users[subtype]--;

	spin_unlock_irqrestore(&statuslock, flags);

	doneAllocParms.used_wait_atomic = 0;
	doneAllocParms.sem = &Semaphore;
	mf_deallocateLpEvents(remoteLp, HvLpEvent_Type_VirtualIo,
			      numReq, &viopath_donealloc, &doneAllocParms);
	down(&Semaphore);

	spin_lock_irqsave(&statuslock, flags);
	for (i = 0, numOpen = 0; i < VIO_MAX_SUBTYPES; i++)
		numOpen += viopathStatus[remoteLp].users[i];

	if ((viopathStatus[remoteLp].isOpen) && (numOpen == 0)) {
		printk(VIOPATH_KERN_INFO "closing connection to partition %d",
				remoteLp);

		HvCallEvent_closeLpEventPath(remoteLp,
					     HvLpEvent_Type_VirtualIo);
		viopathStatus[remoteLp].isOpen = 0;
		viopathStatus[remoteLp].isActive = 0;

		for (i = 0; i < VIO_MAX_SUBTYPES; i++)
			atomic_set(&event_buffer_available[i], 0);
		event_buffer_initialised = 0;
	}
	spin_unlock_irqrestore(&statuslock, flags);
	return 0;
}
EXPORT_SYMBOL(viopath_close);

void *vio_get_event_buffer(int subtype)
{
	subtype = subtype >> VIOMAJOR_SUBTYPE_SHIFT;
	if ((subtype < 0) || (subtype >= VIO_MAX_SUBTYPES))
		return NULL;

	if (atomic_dec_if_positive(&event_buffer_available[subtype]) == 0)
		return &event_buffer[subtype * 256];
	else
		return NULL;
}
EXPORT_SYMBOL(vio_get_event_buffer);

void vio_free_event_buffer(int subtype, void *buffer)
{
	subtype = subtype >> VIOMAJOR_SUBTYPE_SHIFT;
	if ((subtype < 0) || (subtype >= VIO_MAX_SUBTYPES)) {
		printk(VIOPATH_KERN_WARN
		       "unexpected subtype %d freeing event buffer\n", subtype);
		return;
	}

	if (atomic_read(&event_buffer_available[subtype]) != 0) {
		printk(VIOPATH_KERN_WARN
		       "freeing unallocated event buffer, subtype %d\n",
		       subtype);
		return;
	}

	if (buffer != &event_buffer[subtype * 256]) {
		printk(VIOPATH_KERN_WARN
		       "freeing invalid event buffer, subtype %d\n", subtype);
	}

	atomic_set(&event_buffer_available[subtype], 1);
}
EXPORT_SYMBOL(vio_free_event_buffer);

static const struct vio_error_entry vio_no_error =
    { 0, 0, "Non-VIO Error" };
static const struct vio_error_entry vio_unknown_error =
    { 0, EIO, "Unknown Error" };

static const struct vio_error_entry vio_default_errors[] = {
	{0x0001, EIO, "No Connection"},
	{0x0002, EIO, "No Receiver"},
	{0x0003, EIO, "No Buffer Available"},
	{0x0004, EBADRQC, "Invalid Message Type"},
	{0x0000, 0, NULL},
};

const struct vio_error_entry *vio_lookup_rc(
		const struct vio_error_entry *local_table, u16 rc)
{
	const struct vio_error_entry *cur;

	if (!rc)
		return &vio_no_error;
	if (local_table)
		for (cur = local_table; cur->rc; ++cur)
			if (cur->rc == rc)
				return cur;
	for (cur = vio_default_errors; cur->rc; ++cur)
		if (cur->rc == rc)
			return cur;
	return &vio_unknown_error;
}
EXPORT_SYMBOL(vio_lookup_rc);
