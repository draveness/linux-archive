/*
 *  linux/drivers/message/fusion/mptbase.c
 *      High performance SCSI + LAN / Fibre Channel device drivers.
 *      This is the Fusion MPT base driver which supports multiple
 *      (SCSI + LAN) specialized protocol drivers.
 *      For use with PCI chip/adapter(s):
 *          LSIFC9xx/LSI409xx Fibre Channel
 *      running LSI Logic Fusion MPT (Message Passing Technology) firmware.
 *
 *  Credits:
 *      There are lots of people not mentioned below that deserve credit
 *      and thanks but won't get it here - sorry in advance that you
 *      got overlooked.
 *
 *      This driver would not exist if not for Alan Cox's development
 *      of the linux i2o driver.
 *
 *      A special thanks to Noah Romer (LSI Logic) for tons of work
 *      and tough debugging on the LAN driver, especially early on;-)
 *      And to Roger Hickerson (LSI Logic) for tirelessly supporting
 *      this driver project.
 *
 *      A special thanks to Pamela Delaney (LSI Logic) for tons of work
 *      and countless enhancements while adding support for the 1030
 *      chip family.  Pam has been instrumental in the development of
 *      of the 2.xx.xx series fusion drivers, and her contributions are
 *      far too numerous to hope to list in one place.
 *
 *      All manner of help from Stephen Shirron (LSI Logic):
 *      low-level FC analysis, debug + various fixes in FCxx firmware,
 *      initial port to alpha platform, various driver code optimizations,
 *      being a faithful sounding board on all sorts of issues & ideas,
 *      etc.
 *
 *      A huge debt of gratitude is owed to David S. Miller (DaveM)
 *      for fixing much of the stupid and broken stuff in the early
 *      driver while porting to sparc64 platform.  THANK YOU!
 *
 *      Special thanks goes to the I2O LAN driver people at the
 *      University of Helsinki, who, unbeknownst to them, provided
 *      the inspiration and initial structure for this driver.
 *
 *      A really huge debt of gratitude is owed to Eddie C. Dost
 *      for gobs of hard work fixing and optimizing LAN code.
 *      THANK YOU!
 *
 *  Copyright (c) 1999-2004 LSI Logic Corporation
 *  Originally By: Steven J. Ralston
 *  (mailto:sjralston1@netscape.net)
 *  (mailto:mpt_linux_developer@lsil.com)
 *
 *  $Id: mptbase.c,v 1.126 2002/12/16 15:28:45 pdelaney Exp $
 */
/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; version 2 of the License.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    NO WARRANTY
    THE PROGRAM IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OR
    CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED INCLUDING, WITHOUT
    LIMITATION, ANY WARRANTIES OR CONDITIONS OF TITLE, NON-INFRINGEMENT,
    MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Each Recipient is
    solely responsible for determining the appropriateness of using and
    distributing the Program and assumes all risks associated with its
    exercise of rights under this Agreement, including but not limited to
    the risks and costs of program errors, damage to or loss of data,
    programs or equipment, and unavailability or interruption of operations.

    DISCLAIMER OF LIABILITY
    NEITHER RECIPIENT NOR ANY CONTRIBUTORS SHALL HAVE ANY LIABILITY FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
    DAMAGES (INCLUDING WITHOUT LIMITATION LOST PROFITS), HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
    TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
    USE OR DISTRIBUTION OF THE PROGRAM OR THE EXERCISE OF ANY RIGHTS GRANTED
    HEREUNDER, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGES

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/

#include <linux/config.h>
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/kdev_t.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/interrupt.h>		/* needed for in_interrupt() proto */
#include <asm/io.h>
#ifdef CONFIG_MTRR
#include <asm/mtrr.h>
#endif
#ifdef __sparc__
#include <asm/irq.h>			/* needed for __irq_itoa() proto */
#endif

#include "mptbase.h"

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
#define my_NAME		"Fusion MPT base driver"
#define my_VERSION	MPT_LINUX_VERSION_COMMON
#define MYNAM		"mptbase"

MODULE_AUTHOR(MODULEAUTHOR);
MODULE_DESCRIPTION(my_NAME);
MODULE_LICENSE("GPL");

/*
 *  cmd line parameters
 */
#ifdef MFCNT
static int mfcounter = 0;
#define PRINT_MF_COUNT 20000
#endif

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *  Public data...
 */
int mpt_lan_index = -1;
int mpt_stm_index = -1;

struct proc_dir_entry *mpt_proc_root_dir;

DmpServices_t *DmpService;

void *mpt_v_ASCQ_TablePtr;
const char **mpt_ScsiOpcodesPtr;
int mpt_ASCQ_TableSz;


#define WHOINIT_UNKNOWN		0xAA

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *  Private data...
 */
					/* Adapter link list */
LIST_HEAD(ioc_list);
					/* Callback lookup table */
static MPT_CALLBACK		 MptCallbacks[MPT_MAX_PROTOCOL_DRIVERS];
					/* Protocol driver class lookup table */
static int			 MptDriverClass[MPT_MAX_PROTOCOL_DRIVERS];
					/* Event handler lookup table */
static MPT_EVHANDLER		 MptEvHandlers[MPT_MAX_PROTOCOL_DRIVERS];
					/* Reset handler lookup table */
static MPT_RESETHANDLER		 MptResetHandlers[MPT_MAX_PROTOCOL_DRIVERS];
static struct mpt_pci_driver 	*MptDeviceDriverHandlers[MPT_MAX_PROTOCOL_DRIVERS];

static int	FusionInitCalled = 0;
static int	mpt_base_index = -1;
static int	last_drv_idx = -1;
static int	isense_idx = -1;

static DECLARE_WAIT_QUEUE_HEAD(mpt_waitq);

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *  Forward protos...
 */
static irqreturn_t mpt_interrupt(int irq, void *bus_id, struct pt_regs *r);
static int	mpt_base_reply(MPT_ADAPTER *ioc, MPT_FRAME_HDR *req, MPT_FRAME_HDR *reply);

static int	mpt_do_ioc_recovery(MPT_ADAPTER *ioc, u32 reason, int sleepFlag);
static void	mpt_detect_bound_ports(MPT_ADAPTER *this, struct pci_dev *pdev);
static void	mpt_adapter_disable(MPT_ADAPTER *ioc, int freeup);
static void	mpt_adapter_dispose(MPT_ADAPTER *ioc);

static void	MptDisplayIocCapabilities(MPT_ADAPTER *ioc);
static int	MakeIocReady(MPT_ADAPTER *ioc, int force, int sleepFlag);
//static u32	mpt_GetIocState(MPT_ADAPTER *ioc, int cooked);
static int	GetIocFacts(MPT_ADAPTER *ioc, int sleepFlag, int reason);
static int	GetPortFacts(MPT_ADAPTER *ioc, int portnum, int sleepFlag);
static int	SendIocInit(MPT_ADAPTER *ioc, int sleepFlag);
static int	SendPortEnable(MPT_ADAPTER *ioc, int portnum, int sleepFlag);
static int	mpt_do_upload(MPT_ADAPTER *ioc, int sleepFlag);
static int	mpt_downloadboot(MPT_ADAPTER *ioc, int sleepFlag);
static int	mpt_diag_reset(MPT_ADAPTER *ioc, int ignore, int sleepFlag);
static int	KickStart(MPT_ADAPTER *ioc, int ignore, int sleepFlag);
static int	SendIocReset(MPT_ADAPTER *ioc, u8 reset_type, int sleepFlag);
static int	PrimeIocFifos(MPT_ADAPTER *ioc);
static int	WaitForDoorbellAck(MPT_ADAPTER *ioc, int howlong, int sleepFlag);
static int	WaitForDoorbellInt(MPT_ADAPTER *ioc, int howlong, int sleepFlag);
static int	WaitForDoorbellReply(MPT_ADAPTER *ioc, int howlong, int sleepFlag);
static int	GetLanConfigPages(MPT_ADAPTER *ioc);
static int	GetFcPortPage0(MPT_ADAPTER *ioc, int portnum);
static int	GetIoUnitPage2(MPT_ADAPTER *ioc);
static int	mpt_GetScsiPortSettings(MPT_ADAPTER *ioc, int portnum);
static int	mpt_readScsiDevicePageHeaders(MPT_ADAPTER *ioc, int portnum);
static void 	mpt_read_ioc_pg_1(MPT_ADAPTER *ioc);
static void 	mpt_read_ioc_pg_4(MPT_ADAPTER *ioc);
static void	mpt_timer_expired(unsigned long data);
static int	SendEventNotification(MPT_ADAPTER *ioc, u8 EvSwitch);
static int	SendEventAck(MPT_ADAPTER *ioc, EventNotificationReply_t *evnp);

#ifdef CONFIG_PROC_FS
static int	procmpt_summary_read(char *buf, char **start, off_t offset,
				int request, int *eof, void *data);
static int	procmpt_version_read(char *buf, char **start, off_t offset,
				int request, int *eof, void *data);
static int	procmpt_iocinfo_read(char *buf, char **start, off_t offset,
				int request, int *eof, void *data);
#endif
static void	mpt_get_fw_exp_ver(char *buf, MPT_ADAPTER *ioc);

//int		mpt_HardResetHandler(MPT_ADAPTER *ioc, int sleepFlag);
static int	ProcessEventNotification(MPT_ADAPTER *ioc, EventNotificationReply_t *evReply, int *evHandlers);
static void	mpt_sp_ioc_info(MPT_ADAPTER *ioc, u32 ioc_status, MPT_FRAME_HDR *mf);
static void	mpt_fc_log_info(MPT_ADAPTER *ioc, u32 log_info);
static void	mpt_sp_log_info(MPT_ADAPTER *ioc, u32 log_info);

/* module entry point */
static int  __devinit mptbase_probe (struct pci_dev *, const struct pci_device_id *);
static void __devexit mptbase_remove(struct pci_dev *);
static void mptbase_shutdown(struct device * );
static int  __init    fusion_init  (void);
static void __exit    fusion_exit  (void);

/****************************************************************************
 * Supported hardware
 */

static struct pci_device_id mptbase_pci_table[] = {
	{ PCI_VENDOR_ID_LSI_LOGIC, PCI_DEVICE_ID_LSI_FC909,
		PCI_ANY_ID, PCI_ANY_ID },
	{ PCI_VENDOR_ID_LSI_LOGIC, PCI_DEVICE_ID_LSI_FC929,
		PCI_ANY_ID, PCI_ANY_ID },
	{ PCI_VENDOR_ID_LSI_LOGIC, PCI_DEVICE_ID_LSI_FC919,
		PCI_ANY_ID, PCI_ANY_ID },
	{ PCI_VENDOR_ID_LSI_LOGIC, PCI_DEVICE_ID_LSI_FC929X,
		PCI_ANY_ID, PCI_ANY_ID },
	{ PCI_VENDOR_ID_LSI_LOGIC, PCI_DEVICE_ID_LSI_FC919X,
		PCI_ANY_ID, PCI_ANY_ID },
	{ PCI_VENDOR_ID_LSI_LOGIC, PCI_DEVICE_ID_LSI_53C1030,
		PCI_ANY_ID, PCI_ANY_ID },
	{ PCI_VENDOR_ID_LSI_LOGIC, PCI_DEVICE_ID_LSI_1030_53C1035,
		PCI_ANY_ID, PCI_ANY_ID },
	{0}	/* Terminating entry */
};
MODULE_DEVICE_TABLE(pci, mptbase_pci_table);

#define CHIPREG_READ32(addr) 		readl_relaxed(addr)
#define CHIPREG_READ32_dmasync(addr)	readl(addr)
#define CHIPREG_WRITE32(addr,val) 	writel(val, addr)
#define CHIPREG_PIO_WRITE32(addr,val)	outl(val, (unsigned long)addr)
#define CHIPREG_PIO_READ32(addr) 	inl((unsigned long)addr)

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mpt_interrupt - MPT adapter (IOC) specific interrupt handler.
 *	@irq: irq number (not used)
 *	@bus_id: bus identifier cookie == pointer to MPT_ADAPTER structure
 *	@r: pt_regs pointer (not used)
 *
 *	This routine is registered via the request_irq() kernel API call,
 *	and handles all interrupts generated from a specific MPT adapter
 *	(also referred to as a IO Controller or IOC).
 *	This routine must clear the interrupt from the adapter and does
 *	so by reading the reply FIFO.  Multiple replies may be processed
 *	per single call to this routine; up to MPT_MAX_REPLIES_PER_ISR
 *	which is currently set to 32 in mptbase.h.
 *
 *	This routine handles register-level access of the adapter but
 *	dispatches (calls) a protocol-specific callback routine to handle
 *	the protocol-specific details of the MPT request completion.
 */
static irqreturn_t
mpt_interrupt(int irq, void *bus_id, struct pt_regs *r)
{
	MPT_ADAPTER	*ioc;
	MPT_FRAME_HDR	*mf;
	MPT_FRAME_HDR	*mr;
	u32		 pa;
	int		 req_idx;
	int		 cb_idx;
	int		 type;
	int		 freeme;

	ioc = bus_id;

	/*
	 *  Drain the reply FIFO!
	 *
	 * NOTES: I've seen up to 10 replies processed in this loop, so far...
	 * Update: I've seen up to 9182 replies processed in this loop! ??
	 * Update: Limit ourselves to processing max of N replies
	 *	(bottom of loop).
	 */
	while (1) {

		if ((pa = CHIPREG_READ32_dmasync(&ioc->chip->ReplyFifo)) == 0xFFFFFFFF)
			return IRQ_HANDLED;

		cb_idx = 0;
		freeme = 0;

		/*
		 *  Check for non-TURBO reply!
		 */
		if (pa & MPI_ADDRESS_REPLY_A_BIT) {
			u32 reply_dma_low;
			u16 ioc_stat;

			/* non-TURBO reply!  Hmmm, something may be up...
			 *  Newest turbo reply mechanism; get address
			 *  via left shift 1 (get rid of MPI_ADDRESS_REPLY_A_BIT)!
			 */

			/* Map DMA address of reply header to cpu address.
			 * pa is 32 bits - but the dma address may be 32 or 64 bits
			 * get offset based only only the low addresses
			 */
			reply_dma_low = (pa = (pa << 1));
			mr = (MPT_FRAME_HDR *)((u8 *)ioc->reply_frames +
					 (reply_dma_low - ioc->reply_frames_low_dma));

			req_idx = le16_to_cpu(mr->u.frame.hwhdr.msgctxu.fld.req_idx);
			cb_idx = mr->u.frame.hwhdr.msgctxu.fld.cb_idx;
			mf = MPT_INDEX_2_MFPTR(ioc, req_idx);

			dprintk((MYIOC_s_INFO_FMT "Got non-TURBO reply=%p\n",
					ioc->name, mr));
			DBG_DUMP_REPLY_FRAME(mr)

			/* NEW!  20010301 -sralston
			 *  Check/log IOC log info
			 */
			ioc_stat = le16_to_cpu(mr->u.reply.IOCStatus);
			if (ioc_stat & MPI_IOCSTATUS_FLAG_LOG_INFO_AVAILABLE) {
				u32	 log_info = le32_to_cpu(mr->u.reply.IOCLogInfo);
				if ((int)ioc->chip_type <= (int)FC929)
					mpt_fc_log_info(ioc, log_info);
				else
					mpt_sp_log_info(ioc, log_info);
			}
			if (ioc_stat & MPI_IOCSTATUS_MASK) {
				if ((int)ioc->chip_type <= (int)FC929)
						;
				else
					mpt_sp_ioc_info(ioc, (u32)ioc_stat, mf);
			}
		} else {
			/*
			 *  Process turbo (context) reply...
			 */
			dirqprintk((MYIOC_s_INFO_FMT "Got TURBO reply(=%08x)\n", ioc->name, pa));
			type = (pa >> MPI_CONTEXT_REPLY_TYPE_SHIFT);
			if (type == MPI_CONTEXT_REPLY_TYPE_SCSI_TARGET) {
				cb_idx = mpt_stm_index;
				mf = NULL;
				mr = (MPT_FRAME_HDR *) CAST_U32_TO_PTR(pa);
			} else if (type == MPI_CONTEXT_REPLY_TYPE_LAN) {
				cb_idx = mpt_lan_index;
				/*
				 * BUG FIX!  20001218 -sralston
				 *  Blind set of mf to NULL here was fatal
				 *  after lan_reply says "freeme"
				 *  Fix sort of combined with an optimization here;
				 *  added explicit check for case where lan_reply
				 *  was just returning 1 and doing nothing else.
				 *  For this case skip the callback, but set up
				 *  proper mf value first here:-)
				 */
				if ((pa & 0x58000000) == 0x58000000) {
					req_idx = pa & 0x0000FFFF;
					mf = MPT_INDEX_2_MFPTR(ioc, req_idx);
					freeme = 1;
					/*
					 *  IMPORTANT!  Invalidate the callback!
					 */
					cb_idx = 0;
				} else {
					mf = NULL;
				}
				mr = (MPT_FRAME_HDR *) CAST_U32_TO_PTR(pa);
			} else {
				req_idx = pa & 0x0000FFFF;
				cb_idx = (pa & 0x00FF0000) >> 16;
				mf = MPT_INDEX_2_MFPTR(ioc, req_idx);
				mr = NULL;
			}
			pa = 0;					/* No reply flush! */
		}

#ifdef MPT_DEBUG_IRQ
		if ((int)ioc->chip_type > (int)FC929) {
			/* Verify mf, mr are reasonable.
			 */
			if ((mf) && ((mf >= MPT_INDEX_2_MFPTR(ioc, ioc->req_depth))
				|| (mf < ioc->req_frames)) ) {
				printk(MYIOC_s_WARN_FMT
					"mpt_interrupt: Invalid mf (%p) req_idx (%d)!\n", ioc->name, (void *)mf, req_idx);
				cb_idx = 0;
				pa = 0;
				freeme = 0;
			}
			if ((pa) && (mr) && ((mr >= MPT_INDEX_2_RFPTR(ioc, ioc->req_depth))
				|| (mr < ioc->reply_frames)) ) {
				printk(MYIOC_s_WARN_FMT
					"mpt_interrupt: Invalid rf (%p)!\n", ioc->name, (void *)mr);
				cb_idx = 0;
				pa = 0;
				freeme = 0;
			}
			if (cb_idx > (MPT_MAX_PROTOCOL_DRIVERS-1)) {
				printk(MYIOC_s_WARN_FMT
					"mpt_interrupt: Invalid cb_idx (%d)!\n", ioc->name, cb_idx);
				cb_idx = 0;
				pa = 0;
				freeme = 0;
			}
		}
#endif

		/*  Check for (valid) IO callback!  */
		if (cb_idx) {
			/*  Do the callback!  */
			freeme = (*(MptCallbacks[cb_idx]))(ioc, mf, mr);
		}

		if (pa) {
			/*  Flush (non-TURBO) reply with a WRITE!  */
			CHIPREG_WRITE32(&ioc->chip->ReplyFifo, pa);
		}

		if (freeme) {
			unsigned long flags;

			/*  Put Request back on FreeQ!  */
			spin_lock_irqsave(&ioc->FreeQlock, flags);
			Q_ADD_TAIL(&ioc->FreeQ, &mf->u.frame.linkage, MPT_FRAME_HDR);
#ifdef MFCNT
			ioc->mfcnt--;
#endif
			spin_unlock_irqrestore(&ioc->FreeQlock, flags);
		}

		mb();
	}	/* drain reply FIFO */

	return IRQ_HANDLED;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mpt_base_reply - MPT base driver's callback routine; all base driver
 *	"internal" request/reply processing is routed here.
 *	Currently used for EventNotification and EventAck handling.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@mf: Pointer to original MPT request frame
 *	@reply: Pointer to MPT reply frame (NULL if TurboReply)
 *
	*	Returns 1 indicating original alloc'd request frame ptr
 *	should be freed, or 0 if it shouldn't.
 */
static int
mpt_base_reply(MPT_ADAPTER *ioc, MPT_FRAME_HDR *mf, MPT_FRAME_HDR *reply)
{
	int freereq = 1;
	u8 func;

	dprintk((MYIOC_s_INFO_FMT "mpt_base_reply() called\n", ioc->name));

	if ((mf == NULL) ||
	    (mf >= MPT_INDEX_2_MFPTR(ioc, ioc->req_depth))) {
		printk(MYIOC_s_ERR_FMT "NULL or BAD request frame ptr! (=%p)\n",
				ioc->name, (void *)mf);
		return 1;
	}

	if (reply == NULL) {
		dprintk((MYIOC_s_ERR_FMT "Unexpected NULL Event (turbo?) reply!\n",
				ioc->name));
		return 1;
	}

	if (!(reply->u.hdr.MsgFlags & MPI_MSGFLAGS_CONTINUATION_REPLY)) {
		dmfprintk((KERN_INFO MYNAM ": Original request frame (@%p) header\n", mf));
		DBG_DUMP_REQUEST_FRAME_HDR(mf)
	}

	func = reply->u.hdr.Function;
	dprintk((MYIOC_s_INFO_FMT "mpt_base_reply, Function=%02Xh\n",
			ioc->name, func));

	if (func == MPI_FUNCTION_EVENT_NOTIFICATION) {
		EventNotificationReply_t *pEvReply = (EventNotificationReply_t *) reply;
		int evHandlers = 0;
		int results;

		results = ProcessEventNotification(ioc, pEvReply, &evHandlers);
		if (results != evHandlers) {
			/* CHECKME! Any special handling needed here? */
			dprintk((MYIOC_s_WARN_FMT "Called %d event handlers, sum results = %d\n",
					ioc->name, evHandlers, results));
		}

		/*
		 *	Hmmm...  It seems that EventNotificationReply is an exception
		 *	to the rule of one reply per request.
		 */
		if (pEvReply->MsgFlags & MPI_MSGFLAGS_CONTINUATION_REPLY)
			freereq = 0;

#ifdef CONFIG_PROC_FS
//		LogEvent(ioc, pEvReply);
#endif

	} else if (func == MPI_FUNCTION_EVENT_ACK) {
		dprintk((MYIOC_s_INFO_FMT "mpt_base_reply, EventAck reply received\n",
				ioc->name));
	} else if (func == MPI_FUNCTION_CONFIG ||
		   func == MPI_FUNCTION_TOOLBOX) {
		CONFIGPARMS *pCfg;
		unsigned long flags;

		dcprintk((MYIOC_s_INFO_FMT "config_complete (mf=%p,mr=%p)\n",
				ioc->name, mf, reply));

		pCfg = * ((CONFIGPARMS **)((u8 *) mf + ioc->req_sz - sizeof(void *)));

		if (pCfg) {
			/* disable timer and remove from linked list */
			del_timer(&pCfg->timer);

			spin_lock_irqsave(&ioc->FreeQlock, flags);
			Q_DEL_ITEM(&pCfg->linkage);
			spin_unlock_irqrestore(&ioc->FreeQlock, flags);

			/*
			 *	If IOC Status is SUCCESS, save the header
			 *	and set the status code to GOOD.
			 */
			pCfg->status = MPT_CONFIG_ERROR;
			if (reply) {
				ConfigReply_t	*pReply = (ConfigReply_t *)reply;
				u16		 status;

				status = le16_to_cpu(pReply->IOCStatus) & MPI_IOCSTATUS_MASK;
				dcprintk((KERN_NOTICE "  IOCStatus=%04xh, IOCLogInfo=%08xh\n",
				     status, le32_to_cpu(pReply->IOCLogInfo)));

				pCfg->status = status;
				if (status == MPI_IOCSTATUS_SUCCESS) {
					pCfg->hdr->PageVersion = pReply->Header.PageVersion;
					pCfg->hdr->PageLength = pReply->Header.PageLength;
					pCfg->hdr->PageNumber = pReply->Header.PageNumber;
					pCfg->hdr->PageType = pReply->Header.PageType;
				}
			}

			/*
			 *	Wake up the original calling thread
			 */
			pCfg->wait_done = 1;
			wake_up(&mpt_waitq);
		}
	} else {
		printk(MYIOC_s_ERR_FMT "Unexpected msg function (=%02Xh) reply received!\n",
				ioc->name, func);
	}

	/*
	 *	Conditionally tell caller to free the original
	 *	EventNotification/EventAck/unexpected request frame!
	 */
	return freereq;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_register - Register protocol-specific main callback handler.
 *	@cbfunc: callback function pointer
 *	@dclass: Protocol driver's class (%MPT_DRIVER_CLASS enum value)
 *
 *	This routine is called by a protocol-specific driver (SCSI host,
 *	LAN, SCSI target) to register it's reply callback routine.  Each
 *	protocol-specific driver must do this before it will be able to
 *	use any IOC resources, such as obtaining request frames.
 *
 *	NOTES: The SCSI protocol driver currently calls this routine thrice
 *	in order to register separate callbacks; one for "normal" SCSI IO;
 *	one for MptScsiTaskMgmt requests; one for Scan/DV requests.
 *
 *	Returns a positive integer valued "handle" in the
 *	range (and S.O.D. order) {N,...,7,6,5,...,1} if successful.
 *	Any non-positive return value (including zero!) should be considered
 *	an error by the caller.
 */
int
mpt_register(MPT_CALLBACK cbfunc, MPT_DRIVER_CLASS dclass)
{
	int i;

	last_drv_idx = -1;

#ifndef MODULE
	/*
	 *  Handle possibility of the mptscsih_detect() routine getting
	 *  called *before* fusion_init!
	 */
	if (!FusionInitCalled) {
		dprintk((KERN_INFO MYNAM ": Hmmm, calling fusion_init from mpt_register!\n"));
		/*
		 *  NOTE! We'll get recursion here, as fusion_init()
		 *  calls mpt_register()!
		 */
		fusion_init();
		FusionInitCalled++;
	}
#endif

	/*
	 *  Search for empty callback slot in this order: {N,...,7,6,5,...,1}
	 *  (slot/handle 0 is reserved!)
	 */
	for (i = MPT_MAX_PROTOCOL_DRIVERS-1; i; i--) {
		if (MptCallbacks[i] == NULL) {
			MptCallbacks[i] = cbfunc;
			MptDriverClass[i] = dclass;
			MptEvHandlers[i] = NULL;
			last_drv_idx = i;
			break;
		}
	}

	return last_drv_idx;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_deregister - Deregister a protocol drivers resources.
 *	@cb_idx: previously registered callback handle
 *
 *	Each protocol-specific driver should call this routine when it's
 *	module is unloaded.
 */
void
mpt_deregister(int cb_idx)
{
	if ((cb_idx >= 0) && (cb_idx < MPT_MAX_PROTOCOL_DRIVERS)) {
		MptCallbacks[cb_idx] = NULL;
		MptDriverClass[cb_idx] = MPTUNKNOWN_DRIVER;
		MptEvHandlers[cb_idx] = NULL;

		last_drv_idx++;
		if (isense_idx != -1 && isense_idx <= cb_idx)
			isense_idx++;
	}
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_event_register - Register protocol-specific event callback
 *	handler.
 *	@cb_idx: previously registered (via mpt_register) callback handle
 *	@ev_cbfunc: callback function
 *
 *	This routine can be called by one or more protocol-specific drivers
 *	if/when they choose to be notified of MPT events.
 *
 *	Returns 0 for success.
 */
int
mpt_event_register(int cb_idx, MPT_EVHANDLER ev_cbfunc)
{
	if (cb_idx < 1 || cb_idx >= MPT_MAX_PROTOCOL_DRIVERS)
		return -1;

	MptEvHandlers[cb_idx] = ev_cbfunc;
	return 0;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_event_deregister - Deregister protocol-specific event callback
 *	handler.
 *	@cb_idx: previously registered callback handle
 *
 *	Each protocol-specific driver should call this routine
 *	when it does not (or can no longer) handle events,
 *	or when it's module is unloaded.
 */
void
mpt_event_deregister(int cb_idx)
{
	if (cb_idx < 1 || cb_idx >= MPT_MAX_PROTOCOL_DRIVERS)
		return;

	MptEvHandlers[cb_idx] = NULL;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_reset_register - Register protocol-specific IOC reset handler.
 *	@cb_idx: previously registered (via mpt_register) callback handle
 *	@reset_func: reset function
 *
 *	This routine can be called by one or more protocol-specific drivers
 *	if/when they choose to be notified of IOC resets.
 *
 *	Returns 0 for success.
 */
int
mpt_reset_register(int cb_idx, MPT_RESETHANDLER reset_func)
{
	if (cb_idx < 1 || cb_idx >= MPT_MAX_PROTOCOL_DRIVERS)
		return -1;

	MptResetHandlers[cb_idx] = reset_func;
	return 0;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_reset_deregister - Deregister protocol-specific IOC reset handler.
 *	@cb_idx: previously registered callback handle
 *
 *	Each protocol-specific driver should call this routine
 *	when it does not (or can no longer) handle IOC reset handling,
 *	or when it's module is unloaded.
 */
void
mpt_reset_deregister(int cb_idx)
{
	if (cb_idx < 1 || cb_idx >= MPT_MAX_PROTOCOL_DRIVERS)
		return;

	MptResetHandlers[cb_idx] = NULL;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_device_driver_register - Register device driver hooks
 */
int
mpt_device_driver_register(struct mpt_pci_driver * dd_cbfunc, int cb_idx)
{
	MPT_ADAPTER	*ioc;
	int 		error=0;

	if (cb_idx < 1 || cb_idx >= MPT_MAX_PROTOCOL_DRIVERS) {
		error= -EINVAL;
		return error;
	}

	MptDeviceDriverHandlers[cb_idx] = dd_cbfunc;

	/* call per pci device probe entry point */
	list_for_each_entry(ioc, &ioc_list, list) {
		if(dd_cbfunc->probe) {
			error = dd_cbfunc->probe(ioc->pcidev,
			  ioc->pcidev->driver->id_table);
			if(error != 0)
				return error;
  		}
	 }

	return error;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_device_driver_deregister - DeRegister device driver hooks
 */
void
mpt_device_driver_deregister(int cb_idx)
{
	struct mpt_pci_driver *dd_cbfunc;
	MPT_ADAPTER	*ioc;

	if (cb_idx < 1 || cb_idx >= MPT_MAX_PROTOCOL_DRIVERS)
		return;

	dd_cbfunc = MptDeviceDriverHandlers[cb_idx];

	list_for_each_entry(ioc, &ioc_list, list) {
		if (dd_cbfunc->remove)
			dd_cbfunc->remove(ioc->pcidev);
	}
	
	MptDeviceDriverHandlers[cb_idx] = NULL;
}


/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_get_msg_frame - Obtain a MPT request frame from the pool (of 1024)
 *	allocated per MPT adapter.
 *	@handle: Handle of registered MPT protocol driver
 *	@iocid: IOC unique identifier (integer)
 *
 *	Returns pointer to a MPT request frame or %NULL if none are available
 *	or IOC is not active.
 */
MPT_FRAME_HDR*
mpt_get_msg_frame(int handle, MPT_ADAPTER *iocp)
{
	MPT_FRAME_HDR *mf;
	unsigned long flags;

#ifdef MFCNT
	if (!iocp->active)
		printk(KERN_WARNING "IOC Not Active! mpt_get_msg_frame returning NULL!\n");
#endif

	/* If interrupts are not attached, do not return a request frame */
	if (!iocp->active)
		return NULL;

	spin_lock_irqsave(&iocp->FreeQlock, flags);
	if (! Q_IS_EMPTY(&iocp->FreeQ)) {
		int req_offset;

		mf = iocp->FreeQ.head;
		Q_DEL_ITEM(&mf->u.frame.linkage);
		mf->u.frame.hwhdr.msgctxu.fld.cb_idx = handle;	/* byte */
		req_offset = (u8 *)mf - (u8 *)iocp->req_frames;
								/* u16! */
		mf->u.frame.hwhdr.msgctxu.fld.req_idx =
				cpu_to_le16(req_offset / iocp->req_sz);
		mf->u.frame.hwhdr.msgctxu.fld.rsvd = 0;
#ifdef MFCNT
		iocp->mfcnt++;
#endif
	}
	else
		mf = NULL;
	spin_unlock_irqrestore(&iocp->FreeQlock, flags);

#ifdef MFCNT
	if (mf == NULL)
		printk(KERN_WARNING "IOC Active. No free Msg Frames! Count 0x%x Max 0x%x\n", iocp->mfcnt, iocp->req_depth);
	mfcounter++;
	if (mfcounter == PRINT_MF_COUNT)
		printk(KERN_INFO "MF Count 0x%x Max 0x%x \n", iocp->mfcnt, iocp->req_depth);
#endif

	dmfprintk((KERN_INFO MYNAM ": %s: mpt_get_msg_frame(%d,%d), got mf=%p\n",
			iocp->name, handle, iocid, mf));
	return mf;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_put_msg_frame - Send a protocol specific MPT request frame
 *	to a IOC.
 *	@handle: Handle of registered MPT protocol driver
 *	@iocid: IOC unique identifier (integer)
 *	@mf: Pointer to MPT request frame
 *
 *	This routine posts a MPT request frame to the request post FIFO of a
 *	specific MPT adapter.
 */
void
mpt_put_msg_frame(int handle, MPT_ADAPTER *iocp, MPT_FRAME_HDR *mf)
{
	u32 mf_dma_addr;
	int req_offset;

	/* ensure values are reset properly! */
	mf->u.frame.hwhdr.msgctxu.fld.cb_idx = handle;		/* byte */
	req_offset = (u8 *)mf - (u8 *)iocp->req_frames;
									/* u16! */
	mf->u.frame.hwhdr.msgctxu.fld.req_idx = cpu_to_le16(req_offset / iocp->req_sz);
	mf->u.frame.hwhdr.msgctxu.fld.rsvd = 0;

#ifdef MPT_DEBUG_MSG_FRAME
	{
		u32	*m = mf->u.frame.hwhdr.__hdr;
		int	 ii, n;

		printk(KERN_INFO MYNAM ": %s: About to Put msg frame @ %p:\n" KERN_INFO " ",
				iocp->name, m);
		n = iocp->req_sz/4 - 1;
		while (m[n] == 0)
			n--;
		for (ii=0; ii<=n; ii++) {
			if (ii && ((ii%8)==0))
				printk("\n" KERN_INFO " ");
			printk(" %08x", le32_to_cpu(m[ii]));
		}
		printk("\n");
	}
#endif

	mf_dma_addr = iocp->req_frames_low_dma + req_offset;
	CHIPREG_WRITE32(&iocp->chip->RequestFifo, mf_dma_addr);
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_free_msg_frame - Place MPT request frame back on FreeQ.
 *	@handle: Handle of registered MPT protocol driver
 *	@iocid: IOC unique identifier (integer)
 *	@mf: Pointer to MPT request frame
 *
 *	This routine places a MPT request frame back on the MPT adapter's
 *	FreeQ.
 */
void
mpt_free_msg_frame(int handle, MPT_ADAPTER *iocp, MPT_FRAME_HDR *mf)
{
	unsigned long flags;

	/*  Put Request back on FreeQ!  */
	spin_lock_irqsave(&iocp->FreeQlock, flags);
	Q_ADD_TAIL(&iocp->FreeQ, &mf->u.frame.linkage, MPT_FRAME_HDR);
#ifdef MFCNT
	iocp->mfcnt--;
#endif
	spin_unlock_irqrestore(&iocp->FreeQlock, flags);
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_add_sge - Place a simple SGE at address pAddr.
 *	@pAddr: virtual address for SGE
 *	@flagslength: SGE flags and data transfer length
 *	@dma_addr: Physical address
 *
 *	This routine places a MPT request frame back on the MPT adapter's
 *	FreeQ.
 */
void
mpt_add_sge(char *pAddr, u32 flagslength, dma_addr_t dma_addr)
{
	if (sizeof(dma_addr_t) == sizeof(u64)) {
		SGESimple64_t *pSge = (SGESimple64_t *) pAddr;
		u32 tmp = dma_addr & 0xFFFFFFFF;

		pSge->FlagsLength = cpu_to_le32(flagslength);
		pSge->Address.Low = cpu_to_le32(tmp);
		tmp = (u32) ((u64)dma_addr >> 32);
		pSge->Address.High = cpu_to_le32(tmp);

	} else {
		SGESimple32_t *pSge = (SGESimple32_t *) pAddr;
		pSge->FlagsLength = cpu_to_le32(flagslength);
		pSge->Address = cpu_to_le32(dma_addr);
	}
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_add_chain - Place a chain SGE at address pAddr.
 *	@pAddr: virtual address for SGE
 *	@next: nextChainOffset value (u32's)
 *	@length: length of next SGL segment
 *	@dma_addr: Physical address
 *
 *	This routine places a MPT request frame back on the MPT adapter's
 *	FreeQ.
 */
void
mpt_add_chain(char *pAddr, u8 next, u16 length, dma_addr_t dma_addr)
{
	if (sizeof(dma_addr_t) == sizeof(u64)) {
		SGEChain64_t *pChain = (SGEChain64_t *) pAddr;
		u32 tmp = dma_addr & 0xFFFFFFFF;

		pChain->Length = cpu_to_le16(length);
		pChain->Flags = MPI_SGE_FLAGS_CHAIN_ELEMENT | mpt_addr_size();

		pChain->NextChainOffset = next;

		pChain->Address.Low = cpu_to_le32(tmp);
		tmp = (u32) ((u64)dma_addr >> 32);
		pChain->Address.High = cpu_to_le32(tmp);
	} else {
		SGEChain32_t *pChain = (SGEChain32_t *) pAddr;
		pChain->Length = cpu_to_le16(length);
		pChain->Flags = MPI_SGE_FLAGS_CHAIN_ELEMENT | mpt_addr_size();
		pChain->NextChainOffset = next;
		pChain->Address = cpu_to_le32(dma_addr);
	}
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_send_handshake_request - Send MPT request via doorbell
 *	handshake method.
 *	@handle: Handle of registered MPT protocol driver
 *	@iocid: IOC unique identifier (integer)
 *	@reqBytes: Size of the request in bytes
 *	@req: Pointer to MPT request frame
 *	@sleepFlag: Use schedule if CAN_SLEEP else use udelay.
 *
 *	This routine is used exclusively to send MptScsiTaskMgmt
 *	requests since they are required to be sent via doorbell handshake.
 *
 *	NOTE: It is the callers responsibility to byte-swap fields in the
 *	request which are greater than 1 byte in size.
 *
 *	Returns 0 for success, non-zero for failure.
 */
int
mpt_send_handshake_request(int handle, MPT_ADAPTER *iocp, int reqBytes, u32 *req, int sleepFlag)
{
	int		 r = 0;
	u8	*req_as_bytes;
	int	 ii;

	/* State is known to be good upon entering
	 * this function so issue the bus reset
	 * request.
	 */

	/*
	 * Emulate what mpt_put_msg_frame() does /wrt to sanity
	 * setting cb_idx/req_idx.  But ONLY if this request
	 * is in proper (pre-alloc'd) request buffer range...
	 */
	ii = MFPTR_2_MPT_INDEX(iocp,(MPT_FRAME_HDR*)req);
	if (reqBytes >= 12 && ii >= 0 && ii < iocp->req_depth) {
		MPT_FRAME_HDR *mf = (MPT_FRAME_HDR*)req;
		mf->u.frame.hwhdr.msgctxu.fld.req_idx = cpu_to_le16(ii);
		mf->u.frame.hwhdr.msgctxu.fld.cb_idx = handle;
	}

	/* Make sure there are no doorbells */
	CHIPREG_WRITE32(&iocp->chip->IntStatus, 0);
	CHIPREG_WRITE32(&iocp->chip->Doorbell,
			((MPI_FUNCTION_HANDSHAKE<<MPI_DOORBELL_FUNCTION_SHIFT) |
			 ((reqBytes/4)<<MPI_DOORBELL_ADD_DWORDS_SHIFT)));

	/* Wait for IOC doorbell int */
	ii = WaitForDoorbellInt(iocp, 5, sleepFlag);
	if (ii < 0)
		return ii;

	/* Read doorbell and check for active bit */
	if (!(CHIPREG_READ32(&iocp->chip->Doorbell) & MPI_DOORBELL_ACTIVE))
		return -5;

	dhsprintk((KERN_INFO MYNAM ": %s: mpt_send_handshake_request start, WaitCnt=%d\n",
			iocp->name, ii));

	CHIPREG_WRITE32(&iocp->chip->IntStatus, 0);

	r = WaitForDoorbellAck(iocp, 5, sleepFlag);
	if (r < 0)
		return -2;

	/* Send request via doorbell handshake */
	req_as_bytes = (u8 *) req;
	for (ii = 0; ii < reqBytes/4; ii++) {
		u32 word;

		word = ((req_as_bytes[(ii*4) + 0] <<  0) |
			(req_as_bytes[(ii*4) + 1] <<  8) |
			(req_as_bytes[(ii*4) + 2] << 16) |
			(req_as_bytes[(ii*4) + 3] << 24));
		CHIPREG_WRITE32(&iocp->chip->Doorbell, word);
		r = WaitForDoorbellAck(iocp, 5, sleepFlag);
		if (r < 0) {
			r = -3;
			break;
		}
	}

	if (r >= 0 && WaitForDoorbellInt(iocp, 10, sleepFlag) >= 0)
		r = 0;
	else
		r = -4;

	/* Make sure there are no doorbells */
	CHIPREG_WRITE32(&iocp->chip->IntStatus, 0);
	return r;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_verify_adapter - Given a unique IOC identifier, set pointer to
 *	the associated MPT adapter structure.
 *	@iocid: IOC unique identifier (integer)
 *	@iocpp: Pointer to pointer to IOC adapter
 *
 *	Returns iocid and sets iocpp.
 */
int
mpt_verify_adapter(int iocid, MPT_ADAPTER **iocpp)
{
	MPT_ADAPTER *ioc;

	list_for_each_entry(ioc,&ioc_list,list) {
		if (ioc->id == iocid) {
			*iocpp =ioc;
			return iocid;
		} 
	}
	
	*iocpp = NULL;
	return -1;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mptbase_probe - Install a PCI intelligent MPT adapter.
 *	@pdev: Pointer to pci_dev structure
 *
 *	This routine performs all the steps necessary to bring the IOC of
 *	a MPT adapter to a OPERATIONAL state.  This includes registering
 *	memory regions, registering the interrupt, and allocating request
 *	and reply memory pools.
 *
 *	This routine also pre-fetches the LAN MAC address of a Fibre Channel
 *	MPT adapter.
 *
 *	Returns 0 for success, non-zero for failure.
 *
 *	TODO: Add support for polled controllers
 */
static int __devinit
mptbase_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	MPT_ADAPTER	*ioc;
	u8		*mem;
	unsigned long	 mem_phys;
	unsigned long	 port;
	u32		 msize;
	u32		 psize;
	int		 ii;
	int		 r = -ENODEV;
	u64		 mask = 0xffffffffffffffffULL;
	u8		 revision;
	u8		 pcixcmd;
	static int	 mpt_ids = 0;
	struct proc_dir_entry *dent, *ent;

	if (pci_enable_device(pdev))
		return r;
	
	if (!pci_set_dma_mask(pdev, mask)) {
		dprintk((KERN_INFO MYNAM
			": 64 BIT PCI BUS DMA ADDRESSING SUPPORTED\n"));
	} else if (pci_set_dma_mask(pdev, (u64) 0xffffffff)) {
		printk(KERN_WARNING MYNAM ": 32 BIT PCI BUS DMA ADDRESSING NOT SUPPORTED\n");
		return r;
	}

	if (!pci_set_consistent_dma_mask(pdev, mask))
		dprintk((KERN_INFO MYNAM
			": Using 64 bit consistent mask\n"));
	else
		dprintk((KERN_INFO MYNAM
			": Not using 64 bit consistent mask\n"));

	ioc = kmalloc(sizeof(MPT_ADAPTER), GFP_ATOMIC);
	if (ioc == NULL) {
		printk(KERN_ERR MYNAM ": ERROR - Insufficient memory to add adapter!\n");
		return -ENOMEM;
	}
	memset(ioc, 0, sizeof(MPT_ADAPTER));
	ioc->alloc_total = sizeof(MPT_ADAPTER);
	ioc->req_sz = MPT_DEFAULT_FRAME_SIZE;		/* avoid div by zero! */
	ioc->reply_sz = MPT_REPLY_FRAME_SIZE;

	ioc->pcidev = pdev;

	ioc->diagPending = 0;
	spin_lock_init(&ioc->diagLock);

	/* Initialize the event logging.
	 */
	ioc->eventTypes = 0;	/* None */
	ioc->eventContext = 0;
	ioc->eventLogSize = 0;
	ioc->events = NULL;

#ifdef MFCNT
	ioc->mfcnt = 0;
#endif

	ioc->cached_fw = NULL;

	/* Initilize SCSI Config Data structure
	 */
	memset(&ioc->spi_data, 0, sizeof(ScsiCfgData));

	/* Initialize the running configQ head.
	 */
	Q_INIT(&ioc->configQ, Q_ITEM);

	/* Find lookup slot. */
	INIT_LIST_HEAD(&ioc->list);
	ioc->id = mpt_ids++;
	
	mem_phys = msize = 0;
	port = psize = 0;
	for (ii=0; ii < DEVICE_COUNT_RESOURCE; ii++) {
		if (pci_resource_flags(pdev, ii) & PCI_BASE_ADDRESS_SPACE_IO) {
			/* Get I/O space! */
			port = pci_resource_start(pdev, ii);
			psize = pci_resource_len(pdev,ii);
		} else {
			/* Get memmap */
			mem_phys = pci_resource_start(pdev, ii);
			msize = pci_resource_len(pdev,ii);
			break;
		}
	}
	ioc->mem_size = msize;

	if (ii == DEVICE_COUNT_RESOURCE) {
		printk(KERN_ERR MYNAM ": ERROR - MPT adapter has no memory regions defined!\n");
		kfree(ioc);
		return -EINVAL;
	}

	dprintk((KERN_INFO MYNAM ": MPT adapter @ %lx, msize=%dd bytes\n", mem_phys, msize));
	dprintk((KERN_INFO MYNAM ": (port i/o @ %lx, psize=%dd bytes)\n", port, psize));

	mem = NULL;
	/* Get logical ptr for PciMem0 space */
	/*mem = ioremap(mem_phys, msize);*/
	mem = ioremap(mem_phys, 0x100);
	if (mem == NULL) {
		printk(KERN_ERR MYNAM ": ERROR - Unable to map adapter memory!\n");
		kfree(ioc);
		return -EINVAL;
	}
	ioc->memmap = mem;
	dprintk((KERN_INFO MYNAM ": mem = %p, mem_phys = %lx\n", mem, mem_phys));

	dprintk((KERN_INFO MYNAM ": facts @ %p, pfacts[0] @ %p\n",
			&ioc->facts, &ioc->pfacts[0]));

	ioc->mem_phys = mem_phys;
	ioc->chip = (SYSIF_REGS*)mem;

	/* Save Port IO values incase we need to do downloadboot */
	{
		u8 *pmem = (u8*)port;
		ioc->pio_mem_phys = port;
		ioc->pio_chip = (SYSIF_REGS*)pmem;
	}

	ioc->chip_type = FCUNK;
	if (pdev->device == MPI_MANUFACTPAGE_DEVICEID_FC909) {
		ioc->chip_type = FC909;
		ioc->prod_name = "LSIFC909";
	}
	if (pdev->device == MPI_MANUFACTPAGE_DEVICEID_FC929) {
		ioc->chip_type = FC929;
		ioc->prod_name = "LSIFC929";
	}
	else if (pdev->device == MPI_MANUFACTPAGE_DEVICEID_FC919) {
		ioc->chip_type = FC919;
		ioc->prod_name = "LSIFC919";
	}
	else if (pdev->device == MPI_MANUFACTPAGE_DEVICEID_FC929X) {
		ioc->chip_type = FC929X;
		pci_read_config_byte(pdev, PCI_CLASS_REVISION, &revision);
		if (revision < XL_929) {
			ioc->prod_name = "LSIFC929X";
			/* 929X Chip Fix. Set Split transactions level
		 	* for PCIX. Set MOST bits to zero.
		 	*/
			pci_read_config_byte(pdev, 0x6a, &pcixcmd);
			pcixcmd &= 0x8F;
			pci_write_config_byte(pdev, 0x6a, pcixcmd);
		} else {
			ioc->prod_name = "LSIFC929XL";
			/* 929XL Chip Fix. Set MMRBC to 0x08.
		 	*/
			pci_read_config_byte(pdev, 0x6a, &pcixcmd);
			pcixcmd |= 0x08;
			pci_write_config_byte(pdev, 0x6a, pcixcmd);
		}
	}
	else if (pdev->device == MPI_MANUFACTPAGE_DEVICEID_FC919X) {
		ioc->chip_type = FC919X;
		ioc->prod_name = "LSIFC919X";
		/* 919X Chip Fix. Set Split transactions level
		 * for PCIX. Set MOST bits to zero.
		 */
		pci_read_config_byte(pdev, 0x6a, &pcixcmd);
		pcixcmd &= 0x8F;
		pci_write_config_byte(pdev, 0x6a, pcixcmd);
	}
	else if (pdev->device == MPI_MANUFACTPAGE_DEVID_53C1030) {
		ioc->chip_type = C1030;
		ioc->prod_name = "LSI53C1030";
		/* 1030 Chip Fix. Disable Split transactions
		 * for PCIX. Set MOST bits to zero if Rev < C0( = 8).
		 */
		pci_read_config_byte(pdev, PCI_CLASS_REVISION, &revision);
		if (revision < C0_1030) {
			pci_read_config_byte(pdev, 0x6a, &pcixcmd);
			pcixcmd &= 0x8F;
			pci_write_config_byte(pdev, 0x6a, pcixcmd);
		}
	}
	else if (pdev->device == MPI_MANUFACTPAGE_DEVID_1030_53C1035) {
		ioc->chip_type = C1035;
		ioc->prod_name = "LSI53C1035";
	}

	sprintf(ioc->name, "ioc%d", ioc->id);

	Q_INIT(&ioc->FreeQ, MPT_FRAME_HDR);
	spin_lock_init(&ioc->FreeQlock);

	/* Disable all! */
	CHIPREG_WRITE32(&ioc->chip->IntMask, 0xFFFFFFFF);
	ioc->active = 0;
	CHIPREG_WRITE32(&ioc->chip->IntStatus, 0);

	/* Set lookup ptr. */
	list_add_tail(&ioc->list, &ioc_list);

	ioc->pci_irq = -1;
	if (pdev->irq) {
		r = request_irq(pdev->irq, mpt_interrupt, SA_SHIRQ, ioc->name, ioc);

		if (r < 0) {
#ifndef __sparc__
			printk(MYIOC_s_ERR_FMT "Unable to allocate interrupt %d!\n",
					ioc->name, pdev->irq);
#else
			printk(MYIOC_s_ERR_FMT "Unable to allocate interrupt %s!\n",
					ioc->name, __irq_itoa(pdev->irq));
#endif
			Q_DEL_ITEM(ioc);
			list_del(&ioc->list);
			iounmap(mem);
			kfree(ioc);
			return -EBUSY;
		}

		ioc->pci_irq = pdev->irq;

		pci_set_master(pdev);			/* ?? */
		pci_set_drvdata(pdev, ioc);

#ifndef __sparc__
		dprintk((KERN_INFO MYNAM ": %s installed at interrupt %d\n", ioc->name, pdev->irq));
#else
		dprintk((KERN_INFO MYNAM ": %s installed at interrupt %s\n", ioc->name, __irq_itoa(pdev->irq)));
#endif
	}

	/* NEW!  20010220 -sralston
	 * Check for "bound ports" (929, 929X, 1030, 1035) to reduce redundant resets.
	 */
	if ((ioc->chip_type == FC929) || (ioc->chip_type == C1030)
			|| (ioc->chip_type == C1035) || (ioc->chip_type == FC929X))
		mpt_detect_bound_ports(ioc, pdev);

	if ((r = mpt_do_ioc_recovery(ioc,
	  MPT_HOSTEVENT_IOC_BRINGUP, CAN_SLEEP)) != 0) {
		printk(KERN_WARNING MYNAM
		  ": WARNING - %s did not initialize properly! (%d)\n",
		  ioc->name, r);

		Q_DEL_ITEM(ioc);
		list_del(&ioc->list);
		free_irq(ioc->pci_irq, ioc);
		iounmap(mem);
		kfree(ioc);
		pci_set_drvdata(pdev, NULL);
		return r;
	}

	/* call per device driver probe entry point */
	for(ii=0; ii<MPT_MAX_PROTOCOL_DRIVERS; ii++) {
		if(MptDeviceDriverHandlers[ii] &&
		  MptDeviceDriverHandlers[ii]->probe) {
			MptDeviceDriverHandlers[ii]->probe(pdev,id);
		}
	}

	/*
	 *  Create "/proc/mpt/iocN" subdirectory entry for each MPT adapter.
	 */
	dent = proc_mkdir(ioc->name, mpt_proc_root_dir);
	if (dent) {
		ent = create_proc_entry("info", S_IFREG|S_IRUGO, dent);
		if (ent) {
			ent->read_proc = procmpt_iocinfo_read;
			ent->data = ioc;
		}
		ent = create_proc_entry("summary", S_IFREG|S_IRUGO, dent);
		if (ent) {
			ent->read_proc = procmpt_summary_read;
			ent->data = ioc;
		}
	}

	return 0;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mptbase_remove - Remove a PCI intelligent MPT adapter.
 *	@pdev: Pointer to pci_dev structure
 *
 */

static void __devexit
mptbase_remove(struct pci_dev *pdev)
{
	MPT_ADAPTER 	*ioc = pci_get_drvdata(pdev);
	char pname[32];
	int ii;

	sprintf(pname, MPT_PROCFS_MPTBASEDIR "/%s/summary", ioc->name);
	remove_proc_entry(pname, NULL);
	sprintf(pname, MPT_PROCFS_MPTBASEDIR "/%s/info", ioc->name);
	remove_proc_entry(pname, NULL);
	sprintf(pname, MPT_PROCFS_MPTBASEDIR "/%s", ioc->name);
	remove_proc_entry(pname, NULL);
	
	/* call per device driver remove entry point */
	for(ii=0; ii<MPT_MAX_PROTOCOL_DRIVERS; ii++) {
		if(MptDeviceDriverHandlers[ii] &&
		  MptDeviceDriverHandlers[ii]->remove) {
			MptDeviceDriverHandlers[ii]->remove(pdev);
		}
	}
	
	/* Disable interrupts! */
	CHIPREG_WRITE32(&ioc->chip->IntMask, 0xFFFFFFFF);

	ioc->active = 0;
	synchronize_irq(pdev->irq);

	/* Clear any lingering interrupt */
	CHIPREG_WRITE32(&ioc->chip->IntStatus, 0);

	CHIPREG_READ32(&ioc->chip->IntStatus);

	mpt_adapter_dispose(ioc);

	pci_set_drvdata(pdev, NULL);
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mptbase_shutdown -
 *
 */
static void
mptbase_shutdown(struct device * dev)
{
	int ii;

	/* call per device driver shutdown entry point */
	for(ii=0; ii<MPT_MAX_PROTOCOL_DRIVERS; ii++) {
		if(MptDeviceDriverHandlers[ii] &&
		  MptDeviceDriverHandlers[ii]->shutdown) {
			MptDeviceDriverHandlers[ii]->shutdown(dev);
		}
	}

}


/**************************************************************************
 * Power Management
 */
#ifdef CONFIG_PM
/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mptbase_suspend - Fusion MPT base driver suspend routine.
 *
 *
 */
static int
mptbase_suspend(struct pci_dev *pdev, u32 state)
{
	u32 device_state;
	MPT_ADAPTER *ioc = pci_get_drvdata(pdev);
	int ii;

	switch(state)
	{
		case 1: /* S1 */
			device_state=1; /* D1 */;
			break;
		case 3: /* S3 */
		case 4: /* S4 */
			device_state=3; /* D3 */;
			break;
		default:
			return -EAGAIN /*FIXME*/;
			break;
	}

	printk(MYIOC_s_INFO_FMT
	"pci-suspend: pdev=0x%p, slot=%s, Entering operating state [D%d]\n",
		ioc->name, pdev, pci_name(pdev), device_state);

	/* call per device driver suspend entry point */
	for(ii=0; ii<MPT_MAX_PROTOCOL_DRIVERS; ii++) {
		if(MptDeviceDriverHandlers[ii] &&
		  MptDeviceDriverHandlers[ii]->suspend) {
			MptDeviceDriverHandlers[ii]->suspend(pdev, state);
		}
	}

	pci_save_state(pdev, ioc->PciState);

	/* put ioc into READY_STATE */
	if(SendIocReset(ioc, MPI_FUNCTION_IOC_MESSAGE_UNIT_RESET, CAN_SLEEP)) {
		printk(MYIOC_s_ERR_FMT
		"pci-suspend:  IOC msg unit reset failed!\n", ioc->name);
	}

	/* disable interrupts */
	CHIPREG_WRITE32(&ioc->chip->IntMask, 0xFFFFFFFF);
	ioc->active = 0;

	/* Clear any lingering interrupt */
	CHIPREG_WRITE32(&ioc->chip->IntStatus, 0);

	pci_disable_device(pdev);
	pci_set_power_state(pdev, device_state);

	return 0;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mptbase_resume - Fusion MPT base driver resume routine.
 *
 *
 */
static int
mptbase_resume(struct pci_dev *pdev)
{
	MPT_ADAPTER *ioc = pci_get_drvdata(pdev);
	u32 device_state = pdev->current_state;
	int recovery_state;
	int ii;

	printk(MYIOC_s_INFO_FMT
	"pci-resume: pdev=0x%p, slot=%s, Previous operating state [D%d]\n",
		ioc->name, pdev, pci_name(pdev), device_state);

	pci_set_power_state(pdev, 0);
	pci_restore_state(pdev, ioc->PciState);
	pci_enable_device(pdev);

	/* enable interrupts */
	CHIPREG_WRITE32(&ioc->chip->IntMask, ~(MPI_HIM_RIM));
	ioc->active = 1;

	/* F/W not running */
	if(!CHIPREG_READ32(&ioc->chip->Doorbell)) {
		/* enable domain validation flags */
		for (ii=0; ii < MPT_MAX_SCSI_DEVICES; ii++) {
			ioc->spi_data.dvStatus[ii] |= MPT_SCSICFG_NEED_DV;
		}
	}

	printk(MYIOC_s_INFO_FMT
		"pci-resume: ioc-state=0x%x,doorbell=0x%x\n",
		ioc->name,
		(mpt_GetIocState(ioc, 1) >> MPI_IOC_STATE_SHIFT),
		CHIPREG_READ32(&ioc->chip->Doorbell));

	/* bring ioc to operational state */
	if ((recovery_state = mpt_do_ioc_recovery(ioc,
	    MPT_HOSTEVENT_IOC_RECOVER, CAN_SLEEP)) != 0) {
		printk(MYIOC_s_INFO_FMT
			"pci-resume: Cannot recover, error:[%x]\n",
			ioc->name, recovery_state);
	} else {
		printk(MYIOC_s_INFO_FMT
			"pci-resume: success\n", ioc->name);
	}

	/* call per device driver resume entry point */
	for(ii=0; ii<MPT_MAX_PROTOCOL_DRIVERS; ii++) {
		if(MptDeviceDriverHandlers[ii] &&
		  MptDeviceDriverHandlers[ii]->resume) {
			MptDeviceDriverHandlers[ii]->resume(pdev);
		}
	}

	return 0;
}
#endif

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mpt_do_ioc_recovery - Initialize or recover MPT adapter.
 *	@ioc: Pointer to MPT adapter structure
 *	@reason: Event word / reason
 *	@sleepFlag: Use schedule if CAN_SLEEP else use udelay.
 *
 *	This routine performs all the steps necessary to bring the IOC
 *	to a OPERATIONAL state.
 *
 *	This routine also pre-fetches the LAN MAC address of a Fibre Channel
 *	MPT adapter.
 *
 *	Returns:
 *		 0 for success
 *		-1 if failed to get board READY
 *		-2 if READY but IOCFacts Failed
 *		-3 if READY but PrimeIOCFifos Failed
 *		-4 if READY but IOCInit Failed
 */
static int
mpt_do_ioc_recovery(MPT_ADAPTER *ioc, u32 reason, int sleepFlag)
{
	int	 hard_reset_done = 0;
	int	 alt_ioc_ready = 0;
	int	 hard;
	int	 r;
	int	 ii;
	int	 handlers;
	int	 ret = 0;
	int	 reset_alt_ioc_active = 0;

	printk(KERN_INFO MYNAM ": Initiating %s %s\n",
			ioc->name, reason==MPT_HOSTEVENT_IOC_BRINGUP ? "bringup" : "recovery");

	/* Disable reply interrupts (also blocks FreeQ) */
	CHIPREG_WRITE32(&ioc->chip->IntMask, 0xFFFFFFFF);
	ioc->active = 0;

	if (ioc->alt_ioc) {
		if (ioc->alt_ioc->active)
			reset_alt_ioc_active = 1;

		/* Disable alt-IOC's reply interrupts (and FreeQ) for a bit ... */
		CHIPREG_WRITE32(&ioc->alt_ioc->chip->IntMask, 0xFFFFFFFF);
		ioc->alt_ioc->active = 0;
	}

	hard = 1;
	if (reason == MPT_HOSTEVENT_IOC_BRINGUP)
		hard = 0;

	if ((hard_reset_done = MakeIocReady(ioc, hard, sleepFlag)) < 0) {
		if (hard_reset_done == -4) {
			printk(KERN_WARNING MYNAM ": %s Owned by PEER..skipping!\n",
					ioc->name);

			if (reset_alt_ioc_active && ioc->alt_ioc) {
				/* (re)Enable alt-IOC! (reply interrupt, FreeQ) */
				dprintk((KERN_INFO MYNAM ": alt-%s reply irq re-enabled\n",
						ioc->alt_ioc->name));
				CHIPREG_WRITE32(&ioc->alt_ioc->chip->IntMask, ~(MPI_HIM_RIM));
				ioc->alt_ioc->active = 1;
			}

		} else {
			printk(KERN_WARNING MYNAM ": %s NOT READY WARNING!\n",
					ioc->name);
		}
		return -1;
	}

	/* hard_reset_done = 0 if a soft reset was performed
	 * and 1 if a hard reset was performed.
	 */
	if (hard_reset_done && reset_alt_ioc_active && ioc->alt_ioc) {
		if ((r = MakeIocReady(ioc->alt_ioc, 0, sleepFlag)) == 0)
			alt_ioc_ready = 1;
		else
			printk(KERN_WARNING MYNAM
					": alt-%s: (%d) Not ready WARNING!\n",
					ioc->alt_ioc->name, r);
	}

	for (ii=0; ii<5; ii++) {
		/* Get IOC facts! Allow 1 retry */
		if ((r = GetIocFacts(ioc, sleepFlag, reason)) != 0) {
			dinitprintk((MYIOC_s_INFO_FMT 
			     "ii=%d IocFacts failed r=%x\n", ioc->name, ii, r));
		} else
			break;
	}

	if (r) {
		dinitprintk((MYIOC_s_INFO_FMT "Retry IocFacts failed r=%x\n", ioc->name, r));
		ret = -2;
	} else if (reason == MPT_HOSTEVENT_IOC_BRINGUP) {
		MptDisplayIocCapabilities(ioc);
	}

	if (alt_ioc_ready) {
		if ((r = GetIocFacts(ioc->alt_ioc, sleepFlag, reason)) != 0) {
			dinitprintk((MYIOC_s_INFO_FMT "Initial Alt IocFacts failed r=%x\n", ioc->name, r));
			/* Retry - alt IOC was initialized once
			 */
			r = GetIocFacts(ioc->alt_ioc, sleepFlag, reason);
		}
		if (r) {
			dinitprintk((MYIOC_s_INFO_FMT "Retry Alt IocFacts failed r=%x\n", ioc->name, r));
			alt_ioc_ready = 0;
			reset_alt_ioc_active = 0;
		} else if (reason == MPT_HOSTEVENT_IOC_BRINGUP) {
			MptDisplayIocCapabilities(ioc->alt_ioc);
		}
	}

	/* Prime reply & request queues!
	 * (mucho alloc's) Must be done prior to
	 * init as upper addresses are needed for init.
	 * If fails, continue with alt-ioc processing
	 */
	if ((ret == 0) && ((r = PrimeIocFifos(ioc)) != 0))
		ret = -3;

	/* May need to check/upload firmware & data here!
	 * If fails, continue with alt-ioc processing
	 */
	if ((ret == 0) && ((r = SendIocInit(ioc, sleepFlag)) != 0))
		ret = -4;
// NEW!
	if (alt_ioc_ready && ((r = PrimeIocFifos(ioc->alt_ioc)) != 0)) {
		printk(KERN_WARNING MYNAM ": alt-%s: (%d) FIFO mgmt alloc WARNING!\n",
				ioc->alt_ioc->name, r);
		alt_ioc_ready = 0;
		reset_alt_ioc_active = 0;
	}

	if (alt_ioc_ready) {
		if ((r = SendIocInit(ioc->alt_ioc, sleepFlag)) != 0) {
			alt_ioc_ready = 0;
			reset_alt_ioc_active = 0;
			printk(KERN_WARNING MYNAM
				": alt-%s: (%d) init failure WARNING!\n",
					ioc->alt_ioc->name, r);
		}
	}

	if (reason == MPT_HOSTEVENT_IOC_BRINGUP){
		if (ioc->upload_fw) {
			ddlprintk((MYIOC_s_INFO_FMT
				"firmware upload required!\n", ioc->name));

			/* Controller is not operational, cannot do upload
			 */
			if (ret == 0) {
				r = mpt_do_upload(ioc, sleepFlag);
				if (r != 0)
					printk(KERN_WARNING MYNAM ": firmware upload failure!\n");
			}

			/* Handle the alt IOC too */
			if ((alt_ioc_ready) && (ioc->alt_ioc->upload_fw)){
				ddlprintk((MYIOC_s_INFO_FMT
					"Alt-ioc firmware upload required!\n",
					ioc->name));
				r = mpt_do_upload(ioc->alt_ioc, sleepFlag);
				if (r != 0)
					printk(KERN_WARNING MYNAM ": firmware upload failure!\n");
			}
		}
	}

	if (ret == 0) {
		/* Enable! (reply interrupt) */
		CHIPREG_WRITE32(&ioc->chip->IntMask, ~(MPI_HIM_RIM));
		ioc->active = 1;
	}

	if (reset_alt_ioc_active && ioc->alt_ioc) {
		/* (re)Enable alt-IOC! (reply interrupt) */
		dprintk((KERN_INFO MYNAM ": alt-%s reply irq re-enabled\n",
				ioc->alt_ioc->name));
		CHIPREG_WRITE32(&ioc->alt_ioc->chip->IntMask, ~(MPI_HIM_RIM));
		ioc->alt_ioc->active = 1;
	}

	/* NEW!  20010120 -sralston
	 *  Enable MPT base driver management of EventNotification
	 *  and EventAck handling.
	 */
	if ((ret == 0) && (!ioc->facts.EventState))
		(void) SendEventNotification(ioc, 1);	/* 1=Enable EventNotification */

	if (ioc->alt_ioc && alt_ioc_ready && !ioc->alt_ioc->facts.EventState)
		(void) SendEventNotification(ioc->alt_ioc, 1);	/* 1=Enable EventNotification */

	/* (Bugzilla:fibrebugs, #513)
	 * Bug fix (part 2)!  20010905 -sralston
	 *	Add additional "reason" check before call to GetLanConfigPages
	 *	(combined with GetIoUnitPage2 call).  This prevents a somewhat
	 *	recursive scenario; GetLanConfigPages times out, timer expired
	 *	routine calls HardResetHandler, which calls into here again,
	 *	and we try GetLanConfigPages again...
	 */
	if ((ret == 0) && (reason == MPT_HOSTEVENT_IOC_BRINGUP)) {
		if ((int)ioc->chip_type <= (int)FC929) {
			/*
			 *  Pre-fetch FC port WWN and stuff...
			 *  (FCPortPage0_t stuff)
			 */
			for (ii=0; ii < ioc->facts.NumberOfPorts; ii++) {
				(void) GetFcPortPage0(ioc, ii);
			}

			if ((ioc->pfacts[0].ProtocolFlags & MPI_PORTFACTS_PROTOCOL_LAN) &&
			    (ioc->lan_cnfg_page0.Header.PageLength == 0)) {
				/*
				 *  Pre-fetch the ports LAN MAC address!
				 *  (LANPage1_t stuff)
				 */
				(void) GetLanConfigPages(ioc);
#ifdef MPT_DEBUG
				{
					u8 *a = (u8*)&ioc->lan_cnfg_page1.HardwareAddressLow;
					dprintk((MYIOC_s_INFO_FMT "LanAddr = %02X:%02X:%02X:%02X:%02X:%02X\n",
							ioc->name, a[5], a[4], a[3], a[2], a[1], a[0] ));
				}
#endif
			}
		} else {
			/* Get NVRAM and adapter maximums from SPP 0 and 2
			 */
			mpt_GetScsiPortSettings(ioc, 0);

			/* Get version and length of SDP 1
			 */
			mpt_readScsiDevicePageHeaders(ioc, 0);

			/* Find IM volumes
			 */
			if (ioc->facts.MsgVersion >= 0x0102)
				mpt_findImVolumes(ioc);

			/* Check, and possibly reset, the coalescing value
			 */
			mpt_read_ioc_pg_1(ioc);

			mpt_read_ioc_pg_4(ioc);
		}

		GetIoUnitPage2(ioc);
	}

	/*
	 * Call each currently registered protocol IOC reset handler
	 * with post-reset indication.
	 * NOTE: If we're doing _IOC_BRINGUP, there can be no
	 * MptResetHandlers[] registered yet.
	 */
	if (hard_reset_done) {
		r = handlers = 0;
		for (ii=MPT_MAX_PROTOCOL_DRIVERS-1; ii; ii--) {
			if ((ret == 0) && MptResetHandlers[ii]) {
				dprintk((MYIOC_s_INFO_FMT "Calling IOC post_reset handler #%d\n",
						ioc->name, ii));
				r += (*(MptResetHandlers[ii]))(ioc, MPT_IOC_POST_RESET);
				handlers++;
			}

			if (alt_ioc_ready && MptResetHandlers[ii]) {
				dprintk((MYIOC_s_INFO_FMT "Calling alt-%s post_reset handler #%d\n",
						ioc->name, ioc->alt_ioc->name, ii));
				r += (*(MptResetHandlers[ii]))(ioc->alt_ioc, MPT_IOC_POST_RESET);
				handlers++;
			}
		}
		/* FIXME?  Examine results here? */
	}

	return ret;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mpt_detect_bound_ports - Search for PCI bus/dev_function
 *	which matches PCI bus/dev_function (+/-1) for newly discovered 929,
 *	929X, 1030 or 1035.
 *	@ioc: Pointer to MPT adapter structure
 *	@pdev: Pointer to (struct pci_dev) structure
 *
 *	If match on PCI dev_function +/-1 is found, bind the two MPT adapters
 *	using alt_ioc pointer fields in their %MPT_ADAPTER structures.
 */
static void
mpt_detect_bound_ports(MPT_ADAPTER *ioc, struct pci_dev *pdev)
{
	unsigned int match_lo, match_hi;
	MPT_ADAPTER *ioc_srch;

	match_lo = pdev->devfn-1;
	match_hi = pdev->devfn+1;
	dprintk((MYIOC_s_INFO_FMT "PCI bus/devfn=%x/%x, searching for devfn match on %x or %x\n",
			ioc->name, pdev->bus->number, pdev->devfn, match_lo, match_hi));

	list_for_each_entry(ioc_srch, &ioc_list, list) {
		struct pci_dev *_pcidev = ioc_srch->pcidev;

		if ((_pcidev->device == pdev->device) &&
		    (_pcidev->bus->number == pdev->bus->number) &&
		    (_pcidev->devfn == match_lo || _pcidev->devfn == match_hi) ) {
			/* Paranoia checks */
			if (ioc->alt_ioc != NULL) {
				printk(KERN_WARNING MYNAM ": Oops, already bound (%s <==> %s)!\n",
						ioc->name, ioc->alt_ioc->name);
				break;
			} else if (ioc_srch->alt_ioc != NULL) {
				printk(KERN_WARNING MYNAM ": Oops, already bound (%s <==> %s)!\n",
						ioc_srch->name, ioc_srch->alt_ioc->name);
				break;
			}
			dprintk((KERN_INFO MYNAM ": FOUND! binding %s <==> %s\n",
					ioc->name, ioc_srch->name));
			ioc_srch->alt_ioc = ioc;
			ioc->alt_ioc = ioc_srch;
			break;
		}
	}
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mpt_adapter_disable - Disable misbehaving MPT adapter.
 *	@this: Pointer to MPT adapter structure
 *	@free: Free up alloc'd reply, request, etc.
 */
static void
mpt_adapter_disable(MPT_ADAPTER *this, int freeup)
{
	if (this != NULL) {
		int sz=0;
		int ret;

		if (this->cached_fw != NULL) {
			ddlprintk((KERN_INFO MYNAM ": Pushing FW onto adapter\n"));

			if ((ret = mpt_downloadboot(this, NO_SLEEP)) < 0) {
				printk(KERN_WARNING MYNAM
					": firmware downloadboot failure (%d)!\n", ret);
			}
		}

		/* Disable adapter interrupts! */
		CHIPREG_WRITE32(&this->chip->IntMask, 0xFFFFFFFF);
		this->active = 0;
		/* Clear any lingering interrupt */
		CHIPREG_WRITE32(&this->chip->IntStatus, 0);

		if (freeup && this->fifo_pool != NULL) {
			pci_free_consistent(this->pcidev,
				this->fifo_pool_sz,
				this->fifo_pool, this->fifo_pool_dma);
			this->reply_frames = NULL;
			this->reply_alloc = NULL;
			this->req_frames = NULL;
			this->req_alloc = NULL;
			this->chain_alloc = NULL;
			this->fifo_pool = NULL;
			this->alloc_total -= this->fifo_pool_sz;
		}
		if (freeup && this->sense_buf_pool != NULL) {
			sz = (this->req_depth * MPT_SENSE_BUFFER_ALLOC);
			pci_free_consistent(this->pcidev, sz,
					this->sense_buf_pool, this->sense_buf_pool_dma);
			this->sense_buf_pool = NULL;
			this->alloc_total -= sz;
		}

		if (freeup && this->events != NULL){
			sz = MPTCTL_EVENT_LOG_SIZE * sizeof(MPT_IOCTL_EVENTS);
			kfree(this->events);
			this->events = NULL;
			this->alloc_total -= sz;
		}

		if (freeup && this->cached_fw != NULL) {

			sz = this->facts.FWImageSize;
			pci_free_consistent(this->pcidev, sz,
				this->cached_fw, this->cached_fw_dma);
			this->cached_fw = NULL;
			this->alloc_total -= sz;
		}

		if (freeup && this->spi_data.nvram != NULL) {
			kfree(this->spi_data.nvram);
			this->spi_data.nvram = NULL;
		}

		if (freeup && this->spi_data.pIocPg3 != NULL) {
			kfree(this->spi_data.pIocPg3);
			this->spi_data.pIocPg3 = NULL;
		}

		if (freeup && this->spi_data.pIocPg4 != NULL) {
			sz = this->spi_data.IocPg4Sz;
			pci_free_consistent(this->pcidev, sz,
				this->spi_data.pIocPg4,
				this->spi_data.IocPg4_dma);
			this->spi_data.pIocPg4 = NULL;
			this->alloc_total -= sz;
		}
	}
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mpt_adapter_dispose - Free all resources associated with a MPT
 *	adapter.
 *	@this: Pointer to MPT adapter structure
 *
 *	This routine unregisters h/w resources and frees all alloc'd memory
 *	associated with a MPT adapter structure.
 */
static void
mpt_adapter_dispose(MPT_ADAPTER *this)
{
	if (this != NULL) {
		int sz_first, sz_last;

		sz_first = this->alloc_total;

		mpt_adapter_disable(this, 1);

		if (this->pci_irq != -1) {
			free_irq(this->pci_irq, this);
			this->pci_irq = -1;
		}

		if (this->memmap != NULL)
			iounmap((u8 *) this->memmap);

#if defined(CONFIG_MTRR) && 0
		if (this->mtrr_reg > 0) {
			mtrr_del(this->mtrr_reg, 0, 0);
			dprintk((KERN_INFO MYNAM ": %s: MTRR region de-registered\n", this->name));
		}
#endif

		/*  Zap the adapter lookup ptr!  */
		list_del(&this->list);

		sz_last = this->alloc_total;
		dprintk((KERN_INFO MYNAM ": %s: free'd %d of %d bytes\n",
				this->name, sz_first-sz_last+(int)sizeof(*this), sz_first));
		kfree(this);
	}
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	MptDisplayIocCapabilities - Disply IOC's capacilities.
 *	@ioc: Pointer to MPT adapter structure
 */
static void
MptDisplayIocCapabilities(MPT_ADAPTER *ioc)
{
	int i = 0;

	printk(KERN_INFO "%s: ", ioc->name);
	if (ioc->prod_name && strlen(ioc->prod_name) > 3)
		printk("%s: ", ioc->prod_name+3);
	printk("Capabilities={");

	if (ioc->pfacts[0].ProtocolFlags & MPI_PORTFACTS_PROTOCOL_INITIATOR) {
		printk("Initiator");
		i++;
	}

	if (ioc->pfacts[0].ProtocolFlags & MPI_PORTFACTS_PROTOCOL_TARGET) {
		printk("%sTarget", i ? "," : "");
		i++;
	}

	if (ioc->pfacts[0].ProtocolFlags & MPI_PORTFACTS_PROTOCOL_LAN) {
		printk("%sLAN", i ? "," : "");
		i++;
	}

#if 0
	/*
	 *  This would probably evoke more questions than it's worth
	 */
	if (ioc->pfacts[0].ProtocolFlags & MPI_PORTFACTS_PROTOCOL_TARGET) {
		printk("%sLogBusAddr", i ? "," : "");
		i++;
	}
#endif

	printk("}\n");
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	MakeIocReady - Get IOC to a READY state, using KickStart if needed.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@force: Force hard KickStart of IOC
 *	@sleepFlag: Specifies whether the process can sleep
 *
 *	Returns:
 *		 1 - DIAG reset and READY
 *		 0 - READY initially OR soft reset and READY
 *		-1 - Any failure on KickStart
 *		-2 - Msg Unit Reset Failed
 *		-3 - IO Unit Reset Failed
 *		-4 - IOC owned by a PEER
 */
static int
MakeIocReady(MPT_ADAPTER *ioc, int force, int sleepFlag)
{
	u32	 ioc_state;
	int	 statefault = 0;
	int	 cntdn;
	int	 hard_reset_done = 0;
	int	 r;
	int	 ii;
	int	 whoinit;

	/* Get current [raw] IOC state  */
	ioc_state = mpt_GetIocState(ioc, 0);
	dhsprintk((KERN_INFO MYNAM "::MakeIocReady, %s [raw] state=%08x\n", ioc->name, ioc_state));

	/*
	 *	Check to see if IOC got left/stuck in doorbell handshake
	 *	grip of death.  If so, hard reset the IOC.
	 */
	if (ioc_state & MPI_DOORBELL_ACTIVE) {
		statefault = 1;
		printk(MYIOC_s_WARN_FMT "Unexpected doorbell active!\n",
				ioc->name);
	}

	/* Is it already READY? */
	if (!statefault && (ioc_state & MPI_IOC_STATE_MASK) == MPI_IOC_STATE_READY) {
		if ((int)ioc->chip_type <= (int)FC929)
			return 0;
		else {
			return 0;
			/* Workaround from broken 1030 FW.
			 * Force a diagnostic reset if fails.
			 */
/*			if ((r = SendIocReset(ioc, MPI_FUNCTION_IOC_MESSAGE_UNIT_RESET, sleepFlag)) == 0)
				return 0;
			else
				statefault = 4; */
		}
	}

	/*
	 *	Check to see if IOC is in FAULT state.
	 */
	if ((ioc_state & MPI_IOC_STATE_MASK) == MPI_IOC_STATE_FAULT) {
		statefault = 2;
		printk(MYIOC_s_WARN_FMT "IOC is in FAULT state!!!\n",
				ioc->name);
		printk(KERN_WARNING "           FAULT code = %04xh\n",
				ioc_state & MPI_DOORBELL_DATA_MASK);
	}

	/*
	 *	Hmmm...  Did it get left operational?
	 */
	if ((ioc_state & MPI_IOC_STATE_MASK) == MPI_IOC_STATE_OPERATIONAL) {
		dinitprintk((MYIOC_s_WARN_FMT "IOC operational unexpected\n",
				ioc->name));

		/* Check WhoInit.
		 * If PCI Peer, exit.
		 * Else, if no fault conditions are present, issue a MessageUnitReset
		 * Else, fall through to KickStart case
		 */
		whoinit = (ioc_state & MPI_DOORBELL_WHO_INIT_MASK) >> MPI_DOORBELL_WHO_INIT_SHIFT;
		dprintk((KERN_WARNING MYNAM
			": whoinit 0x%x\n statefault %d force %d\n",
			whoinit, statefault, force));
		if (whoinit == MPI_WHOINIT_PCI_PEER)
			return -4;
		else {
			if ((statefault == 0 ) && (force == 0)) {
				if ((r = SendIocReset(ioc, MPI_FUNCTION_IOC_MESSAGE_UNIT_RESET, sleepFlag)) == 0)
					return 0;
			}
			statefault = 3;
		}
	}

	hard_reset_done = KickStart(ioc, statefault||force, sleepFlag);
	if (hard_reset_done < 0)
		return -1;

	/*
	 *  Loop here waiting for IOC to come READY.
	 */
	ii = 0;
	cntdn = ((sleepFlag == CAN_SLEEP) ? HZ : 1000) * 15;	/* 15 seconds */

	while ((ioc_state = mpt_GetIocState(ioc, 1)) != MPI_IOC_STATE_READY) {
		if (ioc_state == MPI_IOC_STATE_OPERATIONAL) {
			/*
			 *  BIOS or previous driver load left IOC in OP state.
			 *  Reset messaging FIFOs.
			 */
			if ((r = SendIocReset(ioc, MPI_FUNCTION_IOC_MESSAGE_UNIT_RESET, sleepFlag)) != 0) {
				printk(MYIOC_s_ERR_FMT "IOC msg unit reset failed!\n", ioc->name);
				return -2;
			}
		} else if (ioc_state == MPI_IOC_STATE_RESET) {
			/*
			 *  Something is wrong.  Try to get IOC back
			 *  to a known state.
			 */
			if ((r = SendIocReset(ioc, MPI_FUNCTION_IO_UNIT_RESET, sleepFlag)) != 0) {
				printk(MYIOC_s_ERR_FMT "IO unit reset failed!\n", ioc->name);
				return -3;
			}
		}

		ii++; cntdn--;
		if (!cntdn) {
			printk(MYIOC_s_ERR_FMT "Wait IOC_READY state timeout(%d)!\n",
					ioc->name, (ii+5)/HZ);
			return -ETIME;
		}

		if (sleepFlag == CAN_SLEEP) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(1);
		} else {
			mdelay (1);	/* 1 msec delay */
		}

	}

	if (statefault < 3) {
		printk(MYIOC_s_INFO_FMT "Recovered from %s\n",
				ioc->name,
				statefault==1 ? "stuck handshake" : "IOC FAULT");
	}

	return hard_reset_done;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mpt_GetIocState - Get the current state of a MPT adapter.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@cooked: Request raw or cooked IOC state
 *
 *	Returns all IOC Doorbell register bits if cooked==0, else just the
 *	Doorbell bits in MPI_IOC_STATE_MASK.
 */
u32
mpt_GetIocState(MPT_ADAPTER *ioc, int cooked)
{
	u32 s, sc;

	/*  Get!  */
	s = CHIPREG_READ32(&ioc->chip->Doorbell);
//	dprintk((MYIOC_s_INFO_FMT "raw state = %08x\n", ioc->name, s));
	sc = s & MPI_IOC_STATE_MASK;

	/*  Save!  */
	ioc->last_state = sc;

	return cooked ? sc : s;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	GetIocFacts - Send IOCFacts request to MPT adapter.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@sleepFlag: Specifies whether the process can sleep
 *	@reason: If recovery, only update facts.
 *
 *	Returns 0 for success, non-zero for failure.
 */
static int
GetIocFacts(MPT_ADAPTER *ioc, int sleepFlag, int reason)
{
	IOCFacts_t		 get_facts;
	IOCFactsReply_t		*facts;
	int			 r;
	int			 req_sz;
	int			 reply_sz;
	u32			 status;

	/* IOC *must* NOT be in RESET state! */
	if (ioc->last_state == MPI_IOC_STATE_RESET) {
		printk(KERN_ERR MYNAM ": ERROR - Can't get IOCFacts, %s NOT READY! (%08x)\n",
				ioc->name,
				ioc->last_state );
		return -44;
	}

	facts = &ioc->facts;

	/* Destination (reply area)... */
	reply_sz = sizeof(*facts);
	memset(facts, 0, reply_sz);

	/* Request area (get_facts on the stack right now!) */
	req_sz = sizeof(get_facts);
	memset(&get_facts, 0, req_sz);

	get_facts.Function = MPI_FUNCTION_IOC_FACTS;
	/* Assert: All other get_facts fields are zero! */

	dinitprintk((MYIOC_s_INFO_FMT "Sending get IocFacts request\n", ioc->name));

	/* No non-zero fields in the get_facts request are greater than
	 * 1 byte in size, so we can just fire it off as is.
	 */
	r = mpt_handshake_req_reply_wait(ioc, req_sz, (u32*)&get_facts,
			reply_sz, (u16*)facts, 5 /*seconds*/, sleepFlag);
	if (r != 0)
		return r;

	/*
	 * Now byte swap (GRRR) the necessary fields before any further
	 * inspection of reply contents.
	 *
	 * But need to do some sanity checks on MsgLength (byte) field
	 * to make sure we don't zero IOC's req_sz!
	 */
	/* Did we get a valid reply? */
	if (facts->MsgLength > offsetof(IOCFactsReply_t, RequestFrameSize)/sizeof(u32)) {
		if (reason == MPT_HOSTEVENT_IOC_BRINGUP) {
			/*
			 * If not been here, done that, save off first WhoInit value
			 */
			if (ioc->FirstWhoInit == WHOINIT_UNKNOWN)
				ioc->FirstWhoInit = facts->WhoInit;
		}

		facts->MsgVersion = le16_to_cpu(facts->MsgVersion);
		facts->MsgContext = le32_to_cpu(facts->MsgContext);
		facts->IOCExceptions = le16_to_cpu(facts->IOCExceptions);
		facts->IOCStatus = le16_to_cpu(facts->IOCStatus);
		facts->IOCLogInfo = le32_to_cpu(facts->IOCLogInfo);
		status = facts->IOCStatus & MPI_IOCSTATUS_MASK;
		/* CHECKME! IOCStatus, IOCLogInfo */

		facts->ReplyQueueDepth = le16_to_cpu(facts->ReplyQueueDepth);
		facts->RequestFrameSize = le16_to_cpu(facts->RequestFrameSize);

		/*
		 * FC f/w version changed between 1.1 and 1.2
		 *	Old: u16{Major(4),Minor(4),SubMinor(8)}
		 *	New: u32{Major(8),Minor(8),Unit(8),Dev(8)}
		 */
		if (facts->MsgVersion < 0x0102) {
			/*
			 *	Handle old FC f/w style, convert to new...
			 */
			u16	 oldv = le16_to_cpu(facts->Reserved_0101_FWVersion);
			facts->FWVersion.Word =
					((oldv<<12) & 0xFF000000) |
					((oldv<<8)  & 0x000FFF00);
		} else
			facts->FWVersion.Word = le32_to_cpu(facts->FWVersion.Word);

		facts->ProductID = le16_to_cpu(facts->ProductID);
		facts->CurrentHostMfaHighAddr =
				le32_to_cpu(facts->CurrentHostMfaHighAddr);
		facts->GlobalCredits = le16_to_cpu(facts->GlobalCredits);
		facts->CurrentSenseBufferHighAddr =
				le32_to_cpu(facts->CurrentSenseBufferHighAddr);
		facts->CurReplyFrameSize =
				le16_to_cpu(facts->CurReplyFrameSize);

		/*
		 * Handle NEW (!) IOCFactsReply fields in MPI-1.01.xx
		 * Older MPI-1.00.xx struct had 13 dwords, and enlarged
		 * to 14 in MPI-1.01.0x.
		 */
		if (facts->MsgLength >= (offsetof(IOCFactsReply_t,FWImageSize) + 7)/4 &&
		    facts->MsgVersion > 0x0100) {
			facts->FWImageSize = le32_to_cpu(facts->FWImageSize);
		}

		if (!facts->RequestFrameSize) {
			/*  Something is wrong!  */
			printk(MYIOC_s_ERR_FMT "IOC reported invalid 0 request size!\n",
					ioc->name);
			return -55;
		}

		if (reason == MPT_HOSTEVENT_IOC_BRINGUP) {
			/*
			 * Set values for this IOC's request & reply frame sizes,
			 * and request & reply queue depths...
			 */
			ioc->req_sz = min(MPT_DEFAULT_FRAME_SIZE, facts->RequestFrameSize * 4);
			ioc->req_depth = min_t(int, MPT_MAX_REQ_DEPTH, facts->GlobalCredits);
			ioc->reply_sz = MPT_REPLY_FRAME_SIZE;
			ioc->reply_depth = min_t(int, MPT_DEFAULT_REPLY_DEPTH, facts->ReplyQueueDepth);

			dprintk((MYIOC_s_INFO_FMT "reply_sz=%3d, reply_depth=%4d\n",
				ioc->name, ioc->reply_sz, ioc->reply_depth));
			dprintk((MYIOC_s_INFO_FMT "req_sz  =%3d, req_depth  =%4d\n",
				ioc->name, ioc->req_sz, ioc->req_depth));

			/* Get port facts! */
			if ( (r = GetPortFacts(ioc, 0, sleepFlag)) != 0 )
				return r;
		}
	} else {
		printk(MYIOC_s_ERR_FMT 
		     "Invalid IOC facts reply, msgLength=%d offsetof=%d!\n",
		     ioc->name, facts->MsgLength, (offsetof(IOCFactsReply_t,
		     RequestFrameSize)/sizeof(u32)));
		return -66;
	}

	return 0;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	GetPortFacts - Send PortFacts request to MPT adapter.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@portnum: Port number
 *	@sleepFlag: Specifies whether the process can sleep
 *
 *	Returns 0 for success, non-zero for failure.
 */
static int
GetPortFacts(MPT_ADAPTER *ioc, int portnum, int sleepFlag)
{
	PortFacts_t		 get_pfacts;
	PortFactsReply_t	*pfacts;
	int			 ii;
	int			 req_sz;
	int			 reply_sz;

	/* IOC *must* NOT be in RESET state! */
	if (ioc->last_state == MPI_IOC_STATE_RESET) {
		printk(KERN_ERR MYNAM ": ERROR - Can't get PortFacts, %s NOT READY! (%08x)\n",
				ioc->name,
				ioc->last_state );
		return -4;
	}

	pfacts = &ioc->pfacts[portnum];

	/* Destination (reply area)...  */
	reply_sz = sizeof(*pfacts);
	memset(pfacts, 0, reply_sz);

	/* Request area (get_pfacts on the stack right now!) */
	req_sz = sizeof(get_pfacts);
	memset(&get_pfacts, 0, req_sz);

	get_pfacts.Function = MPI_FUNCTION_PORT_FACTS;
	get_pfacts.PortNumber = portnum;
	/* Assert: All other get_pfacts fields are zero! */

	dprintk((MYIOC_s_INFO_FMT "Sending get PortFacts(%d) request\n",
			ioc->name, portnum));

	/* No non-zero fields in the get_pfacts request are greater than
	 * 1 byte in size, so we can just fire it off as is.
	 */
	ii = mpt_handshake_req_reply_wait(ioc, req_sz, (u32*)&get_pfacts,
				reply_sz, (u16*)pfacts, 5 /*seconds*/, sleepFlag);
	if (ii != 0)
		return ii;

	/* Did we get a valid reply? */

	/* Now byte swap the necessary fields in the response. */
	pfacts->MsgContext = le32_to_cpu(pfacts->MsgContext);
	pfacts->IOCStatus = le16_to_cpu(pfacts->IOCStatus);
	pfacts->IOCLogInfo = le32_to_cpu(pfacts->IOCLogInfo);
	pfacts->MaxDevices = le16_to_cpu(pfacts->MaxDevices);
	pfacts->PortSCSIID = le16_to_cpu(pfacts->PortSCSIID);
	pfacts->ProtocolFlags = le16_to_cpu(pfacts->ProtocolFlags);
	pfacts->MaxPostedCmdBuffers = le16_to_cpu(pfacts->MaxPostedCmdBuffers);
	pfacts->MaxPersistentIDs = le16_to_cpu(pfacts->MaxPersistentIDs);
	pfacts->MaxLanBuckets = le16_to_cpu(pfacts->MaxLanBuckets);

	return 0;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	SendIocInit - Send IOCInit request to MPT adapter.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@sleepFlag: Specifies whether the process can sleep
 *
 *	Send IOCInit followed by PortEnable to bring IOC to OPERATIONAL state.
 *
 *	Returns 0 for success, non-zero for failure.
 */
static int
SendIocInit(MPT_ADAPTER *ioc, int sleepFlag)
{
	IOCInit_t		 ioc_init;
	MPIDefaultReply_t	 init_reply;
	u32			 state;
	int			 r;
	int			 count;
	int			 cntdn;

	memset(&ioc_init, 0, sizeof(ioc_init));
	memset(&init_reply, 0, sizeof(init_reply));

	ioc_init.WhoInit = MPI_WHOINIT_HOST_DRIVER;
/*	ioc_init.ChainOffset = 0;			*/
	ioc_init.Function = MPI_FUNCTION_IOC_INIT;
/*	ioc_init.Flags = 0;				*/

	/* If we are in a recovery mode and we uploaded the FW image,
	 * then this pointer is not NULL. Skip the upload a second time.
	 * Set this flag if cached_fw set for either IOC.
	 */
	ioc->upload_fw = 0;
	ioc_init.Flags = 0;
	if (ioc->facts.Flags & MPI_IOCFACTS_FLAGS_FW_DOWNLOAD_BOOT) {
		if ((ioc->cached_fw) || (ioc->alt_ioc && ioc->alt_ioc->cached_fw))
			ioc_init.Flags = MPI_IOCINIT_FLAGS_DISCARD_FW_IMAGE;
		else
			ioc->upload_fw = 1;
	}
	ddlprintk((MYIOC_s_INFO_FMT "flags %d, upload_fw %d \n",
		   ioc->name, ioc_init.Flags, ioc->upload_fw));

	if ((int)ioc->chip_type <= (int)FC929) {
		ioc_init.MaxDevices = MPT_MAX_FC_DEVICES;
	} else {
		ioc_init.MaxDevices = MPT_MAX_SCSI_DEVICES;
	}
	ioc_init.MaxBuses = MPT_MAX_BUS;

/*	ioc_init.MsgFlags = 0;				*/
/*	ioc_init.MsgContext = cpu_to_le32(0x00000000);	*/
	ioc_init.ReplyFrameSize = cpu_to_le16(ioc->reply_sz);	/* in BYTES */
		
	ioc->facts.RequestFrameSize = ioc_init.ReplyFrameSize;

	if (sizeof(dma_addr_t) == sizeof(u64)) {
		/* Save the upper 32-bits of the request
		 * (reply) and sense buffers.
		 */
		ioc_init.HostMfaHighAddr = cpu_to_le32((u32)((u64)ioc->req_frames_dma >> 32));
		ioc_init.SenseBufferHighAddr = cpu_to_le32((u32)((u64)ioc->sense_buf_pool_dma >> 32));
	} else {
		/* Force 32-bit addressing */
		ioc_init.HostMfaHighAddr = cpu_to_le32(0);
		ioc_init.SenseBufferHighAddr = cpu_to_le32(0);
	}
		
	ioc->facts.CurrentHostMfaHighAddr = ioc_init.HostMfaHighAddr;
	ioc->facts.CurrentSenseBufferHighAddr = ioc_init.SenseBufferHighAddr;

	dhsprintk((MYIOC_s_INFO_FMT "Sending IOCInit (req @ %p)\n",
			ioc->name, &ioc_init));

	r = mpt_handshake_req_reply_wait(ioc, sizeof(IOCInit_t), (u32*)&ioc_init,
				sizeof(MPIDefaultReply_t), (u16*)&init_reply, 10 /*seconds*/, sleepFlag);
	if (r != 0)
		return r;

	/* No need to byte swap the multibyte fields in the reply
	 * since we don't even look at it's contents.
	 */

	dhsprintk((MYIOC_s_INFO_FMT "Sending PortEnable (req @ %p)\n",
			ioc->name, &ioc_init));
	
	if ((r = SendPortEnable(ioc, 0, sleepFlag)) != 0)
		return r;

	/* YIKES!  SUPER IMPORTANT!!!
	 *  Poll IocState until _OPERATIONAL while IOC is doing
	 *  LoopInit and TargetDiscovery!
	 */
	count = 0;
	cntdn = ((sleepFlag == CAN_SLEEP) ? HZ : 1000) * 60;	/* 60 seconds */
	state = mpt_GetIocState(ioc, 1);
	while (state != MPI_IOC_STATE_OPERATIONAL && --cntdn) {
		if (sleepFlag == CAN_SLEEP) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(1);
		} else {
			mdelay(1);
		}

		if (!cntdn) {
			printk(MYIOC_s_ERR_FMT "Wait IOC_OP state timeout(%d)!\n",
					ioc->name, (count+5)/HZ);
			return -9;
		}

		state = mpt_GetIocState(ioc, 1);
		count++;
	}
	dhsprintk((MYIOC_s_INFO_FMT "INFO - Wait IOC_OPERATIONAL state (cnt=%d)\n",
			ioc->name, count));

	return r;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	SendPortEnable - Send PortEnable request to MPT adapter port.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@portnum: Port number to enable
 *	@sleepFlag: Specifies whether the process can sleep
 *
 *	Send PortEnable to bring IOC to OPERATIONAL state.
 *
 *	Returns 0 for success, non-zero for failure.
 */
static int
SendPortEnable(MPT_ADAPTER *ioc, int portnum, int sleepFlag)
{
	PortEnable_t		 port_enable;
	MPIDefaultReply_t	 reply_buf;
	int	 ii;
	int	 req_sz;
	int	 reply_sz;

	/*  Destination...  */
	reply_sz = sizeof(MPIDefaultReply_t);
	memset(&reply_buf, 0, reply_sz);

	req_sz = sizeof(PortEnable_t);
	memset(&port_enable, 0, req_sz);

	port_enable.Function = MPI_FUNCTION_PORT_ENABLE;
	port_enable.PortNumber = portnum;
/*	port_enable.ChainOffset = 0;		*/
/*	port_enable.MsgFlags = 0;		*/
/*	port_enable.MsgContext = 0;		*/

	dprintk((MYIOC_s_INFO_FMT "Sending Port(%d)Enable (req @ %p)\n",
			ioc->name, portnum, &port_enable));

	/* RAID FW may take a long time to enable
	 */
	if ((int)ioc->chip_type <= (int)FC929) {
		ii = mpt_handshake_req_reply_wait(ioc, req_sz, (u32*)&port_enable,
				reply_sz, (u16*)&reply_buf, 65 /*seconds*/, sleepFlag);
	} else {
		ii = mpt_handshake_req_reply_wait(ioc, req_sz, (u32*)&port_enable,
				reply_sz, (u16*)&reply_buf, 300 /*seconds*/, sleepFlag);
	}

	if (ii != 0)
		return ii;

	/* We do not even look at the reply, so we need not
	 * swap the multi-byte fields.
	 */

	return 0;
}

/*
 * Inputs: size - total FW bytes
 * Outputs: frags - number of fragments needed
 * Return NULL if failed.
 */
void
mpt_alloc_fw_memory(MPT_ADAPTER *ioc, int size)
{
	/* cached_fw
	 */

	if ( (ioc->cached_fw = pci_alloc_consistent(ioc->pcidev, size, &ioc->cached_fw_dma) ) )
		ioc->alloc_total += size;
}

/*
 * If alt_img is NULL, delete from ioc structure.
 * Else, delete a secondary image in same format.
 */
void
mpt_free_fw_memory(MPT_ADAPTER *ioc)
{
	int sz;

	sz = ioc->facts.FWImageSize;
	pci_free_consistent(ioc->pcidev, sz,
			ioc->cached_fw, ioc->cached_fw_dma);
	ioc->cached_fw = NULL;

	return;
}


/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mpt_do_upload - Construct and Send FWUpload request to MPT adapter port.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@sleepFlag: Specifies whether the process can sleep
 *
 *	Returns 0 for success, >0 for handshake failure
 *		<0 for fw upload failure.
 *
 *	Remark: If bound IOC and a successful FWUpload was performed
 *	on the bound IOC, the second image is discarded
 *	and memory is free'd. Both channels must upload to prevent
 *	IOC from running in degraded mode.
 */
static int
mpt_do_upload(MPT_ADAPTER *ioc, int sleepFlag)
{
	u8			 request[ioc->req_sz];
	u8			 reply[sizeof(FWUploadReply_t)];
	FWUpload_t		*prequest;
	FWUploadReply_t		*preply;
	FWUploadTCSGE_t		*ptcsge;
	int			 sgeoffset;
	u32			 flagsLength;
	int			 ii, sz, reply_sz;
	int			 cmdStatus, freeMem = 0;

	/* If the image size is 0 or if the pointer is
	 * not NULL (error), we are done.
	 */
	if (((sz = ioc->facts.FWImageSize) == 0) || ioc->cached_fw)
		return 0;

	if ( sz & 0x01 )
		sz += 1;
	if ( sz & 0x02 )
		sz += 2;

	mpt_alloc_fw_memory(ioc, sz);

	if (ioc->cached_fw == NULL) {
		/* Major Failure.
		 */
		return -ENOMEM;
	}

	dinitprintk((KERN_WARNING MYNAM ": FW Image  @ %p[%p], sz=%d[%x] bytes\n",
		 ioc->cached_fw, (void *)(ulong)ioc->cached_fw_dma, sz, sz));

	prequest = (FWUpload_t *)&request;
	preply = (FWUploadReply_t *)&reply;

	/*  Destination...  */
	memset(prequest, 0, ioc->req_sz);

	reply_sz = sizeof(reply);
	memset(preply, 0, reply_sz);

	prequest->ImageType = MPI_FW_UPLOAD_ITYPE_FW_IOC_MEM;
	prequest->Function = MPI_FUNCTION_FW_UPLOAD;

	ptcsge = (FWUploadTCSGE_t *) &prequest->SGL;
	ptcsge->DetailsLength = 12;
	ptcsge->Flags = MPI_SGE_FLAGS_TRANSACTION_ELEMENT;
	ptcsge->ImageSize = cpu_to_le32(sz);

	sgeoffset = sizeof(FWUpload_t) - sizeof(SGE_MPI_UNION) + sizeof(FWUploadTCSGE_t);

	flagsLength = MPT_SGE_FLAGS_SSIMPLE_READ | sz;
	mpt_add_sge(&request[sgeoffset], flagsLength, ioc->cached_fw_dma);

	sgeoffset += sizeof(u32) + sizeof(dma_addr_t);
	dinitprintk((KERN_WARNING MYNAM "Sending FW Upload (req @ %p) sgeoffset=%d \n",
			prequest, sgeoffset));
	DBG_DUMP_FW_REQUEST_FRAME(prequest)

	ii = mpt_handshake_req_reply_wait(ioc, sgeoffset, (u32*)prequest,
				reply_sz, (u16*)preply, 65 /*seconds*/, sleepFlag);

	dinitprintk((KERN_WARNING MYNAM "FW Upload completed rc=%x \n", ii));

	cmdStatus = -EFAULT;
	if (ii == 0) {
		/* Handshake transfer was complete and successful.
		 * Check the Reply Frame.
		 */
		int status, transfer_sz;
		status = le16_to_cpu(preply->IOCStatus);
		if (status == MPI_IOCSTATUS_SUCCESS) {
			transfer_sz = le32_to_cpu(preply->ActualImageSize);
			if (transfer_sz == sz)
				cmdStatus = 0;
		}
	}
	dinitprintk((MYIOC_s_INFO_FMT ": do_upload status %d \n",
			ioc->name, cmdStatus));

	/* Check to see if we have a copy of this image in
	 * host memory already.
	 */
	if (cmdStatus == 0) {
		ioc->upload_fw = 0;
		if (ioc->alt_ioc && ioc->alt_ioc->cached_fw)
			freeMem = 1;
	}

	/* We already have a copy of this image or
	 * we had some type of an error  - either the handshake
	 * failed (i != 0) or the command did not complete successfully.
	 */
	if (cmdStatus || freeMem) {

		ddlprintk((MYIOC_s_INFO_FMT ": do_upload freeing %s image \n",
			ioc->name, cmdStatus ? "incomplete" : "duplicate"));
		mpt_free_fw_memory(ioc);
	}

	return cmdStatus;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mpt_downloadboot - DownloadBoot code
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@flag: Specify which part of IOC memory is to be uploaded.
 *	@sleepFlag: Specifies whether the process can sleep
 *
 *	FwDownloadBoot requires Programmed IO access.
 *
 *	Returns 0 for success
 *		-1 FW Image size is 0
 *		-2 No valid cached_fw Pointer
 *		<0 for fw upload failure.
 */
static int
mpt_downloadboot(MPT_ADAPTER *ioc, int sleepFlag)
{
	MpiFwHeader_t		*pFwHeader;
	MpiExtImageHeader_t	*pExtImage;
	u32			 fwSize;
	u32			 diag0val;
#ifdef MPT_DEBUG
	u32			 diag1val = 0;
#endif
	int			 count = 0;
	u32			*ptrFw;
	u32			 diagRwData;
	u32			 nextImage;
	u32			 load_addr;
	u32 			 ioc_state;

	ddlprintk((MYIOC_s_INFO_FMT "downloadboot: fw size 0x%x, ioc FW Ptr %p\n",
				ioc->name, ioc->facts.FWImageSize, ioc->cached_fw));

	/* Get dma_addr and data transfer size.
	 */
	if ( ioc->facts.FWImageSize == 0 )
		return -1;

	/* Get the DMA from ioc or ioc->alt_ioc */
	if (ioc->cached_fw == NULL)
		return -2;

	CHIPREG_WRITE32(&ioc->chip->WriteSequence, 0xFF);
	CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_1ST_KEY_VALUE);
	CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_2ND_KEY_VALUE);
	CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_3RD_KEY_VALUE);
	CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_4TH_KEY_VALUE);
	CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_5TH_KEY_VALUE);

	diag0val = CHIPREG_READ32(&ioc->chip->Diagnostic);
	diag0val |= (MPI_DIAG_PREVENT_IOC_BOOT | MPI_DIAG_DISABLE_ARM);
	CHIPREG_WRITE32(&ioc->chip->Diagnostic, diag0val);

	/* wait 100 msec */
	if (sleepFlag == CAN_SLEEP) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(100 * HZ / 1000);
	} else {
		mdelay (100);
	}

	CHIPREG_WRITE32(&ioc->chip->Diagnostic, diag0val | MPI_DIAG_RESET_ADAPTER);

	for (count = 0; count < 30; count ++) {
		diag0val = CHIPREG_READ32(&ioc->chip->Diagnostic);
		if (!(diag0val & MPI_DIAG_RESET_ADAPTER)) {
			ddlprintk((MYIOC_s_INFO_FMT "RESET_ADAPTER cleared, count=%d\n",
				ioc->name, count));
			break;
		}
		/* wait 1 sec */
		if (sleepFlag == CAN_SLEEP) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ);
		} else {
			mdelay (1000);
		}
	}

	if ( count == 30 ) {
		ddlprintk((MYIOC_s_INFO_FMT "downloadboot failed! Unable to RESET_ADAPTER diag0val=%x\n",
		ioc->name, diag0val));
		return -3;
	}

	CHIPREG_WRITE32(&ioc->chip->WriteSequence, 0xFF);
	CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_1ST_KEY_VALUE);
	CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_2ND_KEY_VALUE);
	CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_3RD_KEY_VALUE);
	CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_4TH_KEY_VALUE);
	CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_5TH_KEY_VALUE);

	/* Set the DiagRwEn and Disable ARM bits */
	diag0val = CHIPREG_READ32(&ioc->chip->Diagnostic);
	CHIPREG_WRITE32(&ioc->chip->Diagnostic, (diag0val | MPI_DIAG_RW_ENABLE | MPI_DIAG_DISABLE_ARM));

	pFwHeader = (MpiFwHeader_t *) ioc->cached_fw;
	fwSize = (pFwHeader->ImageSize + 3)/4;
	ptrFw = (u32 *) pFwHeader;

	/* Write the LoadStartAddress to the DiagRw Address Register
	 * using Programmed IO
	 */
	CHIPREG_PIO_WRITE32(&ioc->pio_chip->DiagRwAddress, pFwHeader->LoadStartAddress);
	ddlprintk((MYIOC_s_INFO_FMT "LoadStart addr written 0x%x \n",
		ioc->name, pFwHeader->LoadStartAddress));

	ddlprintk((MYIOC_s_INFO_FMT "Write FW Image: 0x%x bytes @ %p\n",
				ioc->name, fwSize*4, ptrFw));
	while (fwSize--) {
		CHIPREG_PIO_WRITE32(&ioc->pio_chip->DiagRwData, *ptrFw++);
	}

	nextImage = pFwHeader->NextImageHeaderOffset;
	while (nextImage) {
		pExtImage = (MpiExtImageHeader_t *) ((char *)pFwHeader + nextImage);

		load_addr = pExtImage->LoadStartAddress;

		fwSize = (pExtImage->ImageSize + 3) >> 2;
		ptrFw = (u32 *)pExtImage;

		ddlprintk((MYIOC_s_INFO_FMT "Write Ext Image: 0x%x bytes @ %p load_addr=%x\n",
						ioc->name, fwSize*4, ptrFw, load_addr));
		CHIPREG_PIO_WRITE32(&ioc->pio_chip->DiagRwAddress, load_addr);

		while (fwSize--) {
			CHIPREG_PIO_WRITE32(&ioc->pio_chip->DiagRwData, *ptrFw++);
		}
		nextImage = pExtImage->NextImageHeaderOffset;
	}

	/* Write the IopResetVectorRegAddr */
	ddlprintk((MYIOC_s_INFO_FMT "Write IopResetVector Addr=%x! \n", ioc->name, 	pFwHeader->IopResetRegAddr));
	CHIPREG_PIO_WRITE32(&ioc->pio_chip->DiagRwAddress, pFwHeader->IopResetRegAddr);

	/* Write the IopResetVectorValue */
	ddlprintk((MYIOC_s_INFO_FMT "Write IopResetVector Value=%x! \n", ioc->name, pFwHeader->IopResetVectorValue));
	CHIPREG_PIO_WRITE32(&ioc->pio_chip->DiagRwData, pFwHeader->IopResetVectorValue);

	/* clear the PREVENT_IOC_BOOT bit */
	diag0val = CHIPREG_READ32(&ioc->chip->Diagnostic);
	ddlprintk((MYIOC_s_INFO_FMT "downloadboot diag0val=%x, turning off PREVENT_IOC_BOOT\n",
		ioc->name, diag0val));
	diag0val &= ~(MPI_DIAG_PREVENT_IOC_BOOT);
	ddlprintk((MYIOC_s_INFO_FMT "downloadboot now diag0val=%x\n",
		ioc->name, diag0val));
	CHIPREG_WRITE32(&ioc->chip->Diagnostic, diag0val);

	/* Clear the internal flash bad bit - autoincrementing register,
	 * so must do two writes.
	 */
	CHIPREG_PIO_WRITE32(&ioc->pio_chip->DiagRwAddress, 0x3F000000);
	diagRwData = CHIPREG_PIO_READ32(&ioc->pio_chip->DiagRwData);
	diagRwData |= 0x4000000;
	CHIPREG_PIO_WRITE32(&ioc->pio_chip->DiagRwAddress, 0x3F000000);
	CHIPREG_PIO_WRITE32(&ioc->pio_chip->DiagRwData, diagRwData);

	diag0val = CHIPREG_READ32(&ioc->chip->Diagnostic);
	ddlprintk((MYIOC_s_INFO_FMT "downloadboot diag0val=%x, turning off DISABLE_ARM, RW_ENABLE, RESET_HISTORY\n",
		ioc->name, diag0val));
	diag0val &= ~(MPI_DIAG_DISABLE_ARM | MPI_DIAG_RW_ENABLE | MPI_DIAG_RESET_HISTORY);
	ddlprintk((MYIOC_s_INFO_FMT "downloadboot now diag0val=%x\n",
		ioc->name, diag0val));
	CHIPREG_WRITE32(&ioc->chip->Diagnostic, diag0val);

	/* wait 100 msec */
	if (sleepFlag == CAN_SLEEP) {
		ddlprintk((MYIOC_s_INFO_FMT "CAN_SLEEP 100 msec before reset the sequencer\n", ioc->name));
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_timeout(100 * HZ / 1000);
	} else {
		ddlprintk((MYIOC_s_INFO_FMT "mdelay 100 msec before reset the sequencer\n", ioc->name));
		mdelay (100);
	}

	diag0val = CHIPREG_READ32(&ioc->chip->Diagnostic);
	if ( diag0val & (MPI_DIAG_FLASH_BAD_SIG | MPI_DIAG_DISABLE_ARM) ) {
		ddlprintk((MYIOC_s_INFO_FMT "downloadboot failed, diag0val=%x FLASH_BAD_SIG | DISABLE_ARM on\n ",
			ioc->name, diag0val));
	}
	/* Write 0xFF to reset the sequencer */
	CHIPREG_WRITE32(&ioc->chip->WriteSequence, 0xFF);

	for (count=0; count<HZ*20; count++) {
		if ((ioc_state = mpt_GetIocState(ioc, 0)) & MPI_IOC_STATE_READY) {
			ddlprintk((MYIOC_s_INFO_FMT "downloadboot successful! (count=%d) IocState=%x\n",
					ioc->name, count, ioc_state));
/*			if ((r = GetIocFacts(ioc, sleepFlag, MPT_HOSTEVENT_IOC_BRINGUP)) != 0) {
				if ((r = GetIocFacts(ioc, sleepFlag, MPT_HOSTEVENT_IOC_BRINGUP)) != 0) {
					ddlprintk((MYIOC_s_INFO_FMT "GetIocFacts failed\n",
						ioc->name));
						return -EFAULT;
				}
			} */
			/* wait 2 sec */
/*			if (sleepFlag == CAN_SLEEP) {
				set_current_state(TASK_INTERRUPTIBLE);
				schedule_timeout(5000 * HZ / 1000);
			} else {
				mdelay (5000);
			} */

			return 0;
		}
		if (sleepFlag == CAN_SLEEP) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(1);
		} else {
			mdelay (10);
		}
	}
	ddlprintk((MYIOC_s_INFO_FMT "downloadboot failed! IocState=%x\n",
		ioc->name, ioc_state));
	return -EFAULT;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	KickStart - Perform hard reset of MPT adapter.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@force: Force hard reset
 *	@sleepFlag: Specifies whether the process can sleep
 *
 *	This routine places MPT adapter in diagnostic mode via the
 *	WriteSequence register, and then performs a hard reset of adapter
 *	via the Diagnostic register.
 *
 *	Inputs:   sleepflag - CAN_SLEEP (non-interrupt thread)
 *			or NO_SLEEP (interrupt thread, use mdelay)
 *		  force - 1 if doorbell active, board fault state
 *				board operational, IOC_RECOVERY or
 *				IOC_BRINGUP and there is an alt_ioc.
 *			  0 else
 *
 *	Returns:
 *		 1 - hard reset, READY	
 *		 0 - no reset due to History bit, READY	
 *		-1 - no reset due to History bit but not READY	
 *		     OR reset but failed to come READY
 *		-2 - no reset, could not enter DIAG mode
 *		-3 - reset but bad FW bit
 */
static int
KickStart(MPT_ADAPTER *ioc, int force, int sleepFlag)
{
	int hard_reset_done = 0;
	u32 ioc_state;
	int cntdn, cnt = 0;

	dinitprintk((KERN_WARNING MYNAM ": KickStarting %s!\n", ioc->name));
	if ((int)ioc->chip_type > (int)FC929) {
		/* Always issue a Msg Unit Reset first. This will clear some
		 * SCSI bus hang conditions.
		 */
		SendIocReset(ioc, MPI_FUNCTION_IOC_MESSAGE_UNIT_RESET, sleepFlag);

		if (sleepFlag == CAN_SLEEP) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(HZ);
		} else {
			mdelay (1000);
		}
	}

	hard_reset_done = mpt_diag_reset(ioc, force, sleepFlag);
	if (hard_reset_done < 0)
		return hard_reset_done;

	dprintk((MYIOC_s_INFO_FMT "Diagnostic reset successful!\n",
			ioc->name));

	cntdn = ((sleepFlag == CAN_SLEEP) ? HZ : 1000) * 20;	/* 20 seconds */
	for (cnt=0; cnt<cntdn; cnt++) {
		if ((ioc_state = mpt_GetIocState(ioc, 1)) == MPI_IOC_STATE_READY) {
			dprintk((MYIOC_s_INFO_FMT "KickStart successful! (cnt=%d)\n",
					ioc->name, cnt));
			return hard_reset_done;
		}
		if (sleepFlag == CAN_SLEEP) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(1);
		} else {
			mdelay (10);
		}
	}

	printk(MYIOC_s_ERR_FMT "Failed to come READY after reset!\n",
			ioc->name);
	return -1;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mpt_diag_reset - Perform hard reset of the adapter.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@ignore: Set if to honor and clear to ignore
 *		the reset history bit
 *	@sleepflag: CAN_SLEEP if called in a non-interrupt thread,
 *		else set to NO_SLEEP (use mdelay instead)
 *
 *	This routine places the adapter in diagnostic mode via the
 *	WriteSequence register and then performs a hard reset of adapter
 *	via the Diagnostic register. Adapter should be in ready state
 *	upon successful completion.
 *
 *	Returns:  1  hard reset successful
 *		  0  no reset performed because reset history bit set
 *		 -2  enabling diagnostic mode failed
 *		 -3  diagnostic reset failed
 */
static int
mpt_diag_reset(MPT_ADAPTER *ioc, int ignore, int sleepFlag)
{
	u32 diag0val;
	u32 doorbell;
	int hard_reset_done = 0;
	int count = 0;
#ifdef MPT_DEBUG
	u32 diag1val = 0;
#endif

	/* Clear any existing interrupts */
	CHIPREG_WRITE32(&ioc->chip->IntStatus, 0);

	/* Use "Diagnostic reset" method! (only thing available!) */
	diag0val = CHIPREG_READ32(&ioc->chip->Diagnostic);

#ifdef MPT_DEBUG
	if (ioc->alt_ioc)
		diag1val = CHIPREG_READ32(&ioc->alt_ioc->chip->Diagnostic);
	dprintk((MYIOC_s_INFO_FMT "DbG1: diag0=%08x, diag1=%08x\n",
			ioc->name, diag0val, diag1val));
#endif

	/* Do the reset if we are told to ignore the reset history
	 * or if the reset history is 0
	 */
	if (ignore || !(diag0val & MPI_DIAG_RESET_HISTORY)) {
		while ((diag0val & MPI_DIAG_DRWE) == 0) {
			/* Write magic sequence to WriteSequence register
			 * Loop until in diagnostic mode
			 */
			CHIPREG_WRITE32(&ioc->chip->WriteSequence, 0xFF);
			CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_1ST_KEY_VALUE);
			CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_2ND_KEY_VALUE);
			CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_3RD_KEY_VALUE);
			CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_4TH_KEY_VALUE);
			CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_5TH_KEY_VALUE);

			/* wait 100 msec */
			if (sleepFlag == CAN_SLEEP) {
				set_current_state(TASK_INTERRUPTIBLE);
				schedule_timeout(100 * HZ / 1000);
			} else {
				mdelay (100);
			}

			count++;
			if (count > 20) {
				printk(MYIOC_s_ERR_FMT "Enable Diagnostic mode FAILED! (%02xh)\n",
						ioc->name, diag0val);
				return -2;

			}

			diag0val = CHIPREG_READ32(&ioc->chip->Diagnostic);

			dprintk((MYIOC_s_INFO_FMT "Wrote magic DiagWriteEn sequence (%x)\n",
					ioc->name, diag0val));
		}

#ifdef MPT_DEBUG
		if (ioc->alt_ioc)
			diag1val = CHIPREG_READ32(&ioc->alt_ioc->chip->Diagnostic);
		dprintk((MYIOC_s_INFO_FMT "DbG2: diag0=%08x, diag1=%08x\n",
				ioc->name, diag0val, diag1val));
#endif
		/* Write the PreventIocBoot bit */
		if ((ioc->cached_fw) || (ioc->alt_ioc && ioc->alt_ioc->cached_fw)) {
			diag0val |= MPI_DIAG_PREVENT_IOC_BOOT;
			CHIPREG_WRITE32(&ioc->chip->Diagnostic, diag0val);
		}

		/*
		 * Disable the ARM (Bug fix)
		 *
		 */
		CHIPREG_WRITE32(&ioc->chip->Diagnostic, diag0val | MPI_DIAG_DISABLE_ARM);
		mdelay (1);

		/*
		 * Now hit the reset bit in the Diagnostic register
		 * (THE BIG HAMMER!) (Clears DRWE bit).
		 */
		CHIPREG_WRITE32(&ioc->chip->Diagnostic, diag0val | MPI_DIAG_RESET_ADAPTER);
		hard_reset_done = 1;
		dprintk((MYIOC_s_INFO_FMT "Diagnostic reset performed\n",
				ioc->name));

		/*
		 * Call each currently registered protocol IOC reset handler
		 * with pre-reset indication.
		 * NOTE: If we're doing _IOC_BRINGUP, there can be no
		 * MptResetHandlers[] registered yet.
		 */
		{
			int	 ii;
			int	 r = 0;

			for (ii=MPT_MAX_PROTOCOL_DRIVERS-1; ii; ii--) {
				if (MptResetHandlers[ii]) {
					dprintk((MYIOC_s_INFO_FMT "Calling IOC pre_reset handler #%d\n",
							ioc->name, ii));
					r += (*(MptResetHandlers[ii]))(ioc, MPT_IOC_PRE_RESET);
					if (ioc->alt_ioc) {
						dprintk((MYIOC_s_INFO_FMT "Calling alt-%s pre_reset handler #%d\n",
								ioc->name, ioc->alt_ioc->name, ii));
						r += (*(MptResetHandlers[ii]))(ioc->alt_ioc, MPT_IOC_PRE_RESET);
					}
				}
			}
			/* FIXME?  Examine results here? */
		}

		if ((ioc->cached_fw) || (ioc->alt_ioc && ioc->alt_ioc->cached_fw)) {
			/* If the DownloadBoot operation fails, the
			 * IOC will be left unusable. This is a fatal error
			 * case.  _diag_reset will return < 0
			 */
			for (count = 0; count < 30; count ++) {
				diag0val = CHIPREG_READ32(&ioc->chip->Diagnostic);
#ifdef MPT_DEBUG
				if (ioc->alt_ioc)
					diag1val = CHIPREG_READ32(&ioc->alt_ioc->chip->Diagnostic);
				dprintk((MYIOC_s_INFO_FMT
					"DbG2b: diag0=%08x, diag1=%08x\n",
					ioc->name, diag0val, diag1val));
#endif
				if (!(diag0val & MPI_DIAG_RESET_ADAPTER)) {
					break;
				}

				/* wait 1 sec */
				if (sleepFlag == CAN_SLEEP) {
					set_current_state(TASK_INTERRUPTIBLE);
					schedule_timeout(HZ);
				} else {
					mdelay (1000);
				}
			}
			if ((count = mpt_downloadboot(ioc, sleepFlag)) < 0) {
				printk(KERN_WARNING MYNAM
					": firmware downloadboot failure (%d)!\n", count);
			}

		} else {
			/* Wait for FW to reload and for board
			 * to go to the READY state.
			 * Maximum wait is 60 seconds.
			 * If fail, no error will check again
			 * with calling program.
			 */
			for (count = 0; count < 60; count ++) {
				doorbell = CHIPREG_READ32(&ioc->chip->Doorbell);
				doorbell &= MPI_IOC_STATE_MASK;

				if (doorbell == MPI_IOC_STATE_READY) {
					break;
				}

				/* wait 1 sec */
				if (sleepFlag == CAN_SLEEP) {
					set_current_state(TASK_INTERRUPTIBLE);
					schedule_timeout(HZ);
				} else {
					mdelay (1000);
				}
			}
		}
	}

	diag0val = CHIPREG_READ32(&ioc->chip->Diagnostic);
#ifdef MPT_DEBUG
	if (ioc->alt_ioc)
		diag1val = CHIPREG_READ32(&ioc->alt_ioc->chip->Diagnostic);
	dprintk((MYIOC_s_INFO_FMT "DbG3: diag0=%08x, diag1=%08x\n",
		ioc->name, diag0val, diag1val));
#endif

	/* Clear RESET_HISTORY bit!  Place board in the
	 * diagnostic mode to update the diag register.
	 */
	diag0val = CHIPREG_READ32(&ioc->chip->Diagnostic);
	count = 0;
	while ((diag0val & MPI_DIAG_DRWE) == 0) {
		/* Write magic sequence to WriteSequence register
		 * Loop until in diagnostic mode
		 */
		CHIPREG_WRITE32(&ioc->chip->WriteSequence, 0xFF);
		CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_1ST_KEY_VALUE);
		CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_2ND_KEY_VALUE);
		CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_3RD_KEY_VALUE);
		CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_4TH_KEY_VALUE);
		CHIPREG_WRITE32(&ioc->chip->WriteSequence, MPI_WRSEQ_5TH_KEY_VALUE);

		/* wait 100 msec */
		if (sleepFlag == CAN_SLEEP) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(100 * HZ / 1000);
		} else {
			mdelay (100);
		}

		count++;
		if (count > 20) {
			printk(MYIOC_s_ERR_FMT "Enable Diagnostic mode FAILED! (%02xh)\n",
					ioc->name, diag0val);
			break;
		}
		diag0val = CHIPREG_READ32(&ioc->chip->Diagnostic);
	}
	diag0val &= ~MPI_DIAG_RESET_HISTORY;
	CHIPREG_WRITE32(&ioc->chip->Diagnostic, diag0val);
	diag0val = CHIPREG_READ32(&ioc->chip->Diagnostic);
	if (diag0val & MPI_DIAG_RESET_HISTORY) {
		printk(MYIOC_s_WARN_FMT "ResetHistory bit failed to clear!\n",
				ioc->name);
	}

	/* Disable Diagnostic Mode
	 */
	CHIPREG_WRITE32(&ioc->chip->WriteSequence, 0xFFFFFFFF);

	/* Check FW reload status flags.
	 */
	diag0val = CHIPREG_READ32(&ioc->chip->Diagnostic);
	if (diag0val & (MPI_DIAG_FLASH_BAD_SIG | MPI_DIAG_RESET_ADAPTER | MPI_DIAG_DISABLE_ARM)) {
		printk(MYIOC_s_ERR_FMT "Diagnostic reset FAILED! (%02xh)\n",
				ioc->name, diag0val);
		return -3;
	}

#ifdef MPT_DEBUG
	if (ioc->alt_ioc)
		diag1val = CHIPREG_READ32(&ioc->alt_ioc->chip->Diagnostic);
	dprintk((MYIOC_s_INFO_FMT "DbG4: diag0=%08x, diag1=%08x\n",
			ioc->name, diag0val, diag1val));
#endif

	/*
	 * Reset flag that says we've enabled event notification
	 */
	ioc->facts.EventState = 0;

	if (ioc->alt_ioc)
		ioc->alt_ioc->facts.EventState = 0;

	return hard_reset_done;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	SendIocReset - Send IOCReset request to MPT adapter.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@reset_type: reset type, expected values are
 *	%MPI_FUNCTION_IOC_MESSAGE_UNIT_RESET or %MPI_FUNCTION_IO_UNIT_RESET
 *
 *	Send IOCReset request to the MPT adapter.
 *
 *	Returns 0 for success, non-zero for failure.
 */
static int
SendIocReset(MPT_ADAPTER *ioc, u8 reset_type, int sleepFlag)
{
	int r;
	u32 state;
	int cntdn, count;

	drsprintk((KERN_WARNING MYNAM ": %s: Sending IOC reset(0x%02x)!\n",
			ioc->name, reset_type));
	CHIPREG_WRITE32(&ioc->chip->Doorbell, reset_type<<MPI_DOORBELL_FUNCTION_SHIFT);
	if ((r = WaitForDoorbellAck(ioc, 5, sleepFlag)) < 0)
		return r;

	/* FW ACK'd request, wait for READY state
	 */
	count = 0;
	cntdn = ((sleepFlag == CAN_SLEEP) ? HZ : 1000) * 15;	/* 15 seconds */

	while ((state = mpt_GetIocState(ioc, 1)) != MPI_IOC_STATE_READY) {
		cntdn--;
		count++;
		if (!cntdn) {
			if (sleepFlag != CAN_SLEEP)
				count *= 10;

			printk(KERN_ERR MYNAM ": %s: ERROR - Wait IOC_READY state timeout(%d)!\n",
					ioc->name, (count+5)/HZ);
			return -ETIME;
		}

		if (sleepFlag == CAN_SLEEP) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(1);
		} else {
			mdelay (1);	/* 1 msec delay */
		}
	}

	/* TODO!
	 *  Cleanup all event stuff for this IOC; re-issue EventNotification
	 *  request if needed.
	 */
	if (ioc->facts.Function)
		ioc->facts.EventState = 0;

	return 0;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	PrimeIocFifos - Initialize IOC request and reply FIFOs.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *
 *	This routine allocates memory for the MPT reply and request frame
 *	pools (if necessary), and primes the IOC reply FIFO with
 *	reply frames.
 *
 *	Returns 0 for success, non-zero for failure.
 */
static int
PrimeIocFifos(MPT_ADAPTER *ioc)
{
	MPT_FRAME_HDR *mf;
	unsigned long b;
	unsigned long flags;
	dma_addr_t aligned_mem_dma;
	u8 *aligned_mem;
	int i, sz;
	int chain_buffer_sz, reply_buffer_sz, request_buffer_sz;
	int scale, num_sge, num_chain;

	/* request buffer size,  rounding UP to nearest 4-kB boundary */
	request_buffer_sz = (ioc->req_sz * ioc->req_depth) + 128;
	request_buffer_sz = ((request_buffer_sz + 0x1000UL - 1UL) / 0x1000) * 0x1000;

	/* reply buffer size */
	reply_buffer_sz = (ioc->reply_sz * ioc->reply_depth) + 128;

	/* chain buffer size, copied from from mptscsih_initChainBuffers()
	 *
	 * Calculate the number of chain buffers needed(plus 1) per I/O
	 * then multiply the the maximum number of simultaneous cmds
	 *
	 * num_sge = num sge in request frame + last chain buffer
	 * scale = num sge per chain buffer if no chain element
	 */

	scale = ioc->req_sz/(sizeof(dma_addr_t) + sizeof(u32));
	if (sizeof(dma_addr_t) == sizeof(u64))
		num_sge =  scale + (ioc->req_sz - 60) / (sizeof(dma_addr_t) + sizeof(u32));
	else
		num_sge =  1 + scale + (ioc->req_sz - 64) / (sizeof(dma_addr_t) + sizeof(u32));

	num_chain = 1;
	while (MPT_SCSI_SG_DEPTH - num_sge > 0) {
		num_chain++;
		num_sge += (scale - 1);
	}
	num_chain++;

	if ((int)ioc->chip_type > (int) FC929)
		num_chain *= MPT_SCSI_CAN_QUEUE;
	else
		num_chain *= MPT_FC_CAN_QUEUE;

	chain_buffer_sz = num_chain * ioc->req_sz;

	if(ioc->fifo_pool == NULL) {

		ioc->fifo_pool_sz = request_buffer_sz +
			reply_buffer_sz + chain_buffer_sz;

		ioc->fifo_pool = pci_alloc_consistent(ioc->pcidev,
		ioc->fifo_pool_sz, &ioc->fifo_pool_dma);

		if( ioc->fifo_pool == NULL)
			goto out_fail;

		ioc->alloc_total += ioc->fifo_pool_sz;
		memset(ioc->fifo_pool, 0, ioc->fifo_pool_sz);

		/* reply fifo pointers */
		ioc->reply_alloc = ioc->fifo_pool;
		ioc->reply_alloc_dma = ioc->fifo_pool_dma;
		/* request fifo pointers */
		ioc->req_alloc = ioc->reply_alloc+reply_buffer_sz;
		ioc->req_alloc_dma = ioc->reply_alloc_dma+reply_buffer_sz;
		/* chain buffer pointers */
		ioc->chain_alloc = ioc->req_alloc+request_buffer_sz;
		ioc->chain_alloc_dma = ioc->req_alloc_dma+request_buffer_sz;
		ioc->chain_alloc_sz = chain_buffer_sz;

		/*  Prime reply FIFO...  */
		dprintk((KERN_INFO MYNAM ": %s.reply_alloc  @ %p[%p], sz=%d bytes\n",
			ioc->name, ioc->reply_alloc, 
			(void *)(ulong)ioc->reply_alloc_dma, reply_buffer_sz));

		b = (unsigned long) ioc->reply_alloc;
		b = (b + (0x80UL - 1UL)) & ~(0x80UL - 1UL); /* round up to 128-byte boundary */
		aligned_mem = (u8 *) b;
		ioc->reply_frames = (MPT_FRAME_HDR *) aligned_mem;
		ioc->reply_frames_dma =
			(ioc->reply_alloc_dma + (aligned_mem - ioc->reply_alloc));

		ioc->reply_frames_low_dma = (u32) (ioc->reply_frames_dma & 0xFFFFFFFF);
	
		/*  Request FIFO - WE manage this!  */
		dprintk((KERN_INFO MYNAM ": %s.req_alloc    @ %p[%p], sz=%d bytes\n",
			ioc->name, ioc->req_alloc,
			(void *)(ulong)ioc->req_alloc_dma, request_buffer_sz));

		b = (unsigned long) ioc->req_alloc;
		b = (b + (0x80UL - 1UL)) & ~(0x80UL - 1UL); /* round up to 128-byte boundary */
		aligned_mem = (u8 *) b;
		ioc->req_frames = (MPT_FRAME_HDR *) aligned_mem;
		ioc->req_frames_dma =
			(ioc->req_alloc_dma + (aligned_mem - ioc->req_alloc));

		ioc->req_frames_low_dma = (u32) (ioc->req_frames_dma & 0xFFFFFFFF);

#if defined(CONFIG_MTRR) && 0
		/*
		 *  Enable Write Combining MTRR for IOC's memory region.
		 *  (at least as much as we can; "size and base must be
		 *  multiples of 4 kiB"
		 */
		ioc->mtrr_reg = mtrr_add(ioc->fifo_pool,
					 ioc->fifo_pool_sz,
					 MTRR_TYPE_WRCOMB, 1);
		dprintk((MYIOC_s_INFO_FMT "MTRR region registered (base:size=%08x:%x)\n",
				ioc->name, ioc->fifo_pool, ioc->fifo_pool_sz));
#endif

	} /* ioc->fifo_pool == NULL */
	
	/* Post Reply frames to FIFO
	 */
	aligned_mem_dma = ioc->reply_frames_dma;
	dprintk((KERN_INFO MYNAM ": %s.reply_frames @ %p[%p]\n",
		 	ioc->name, ioc->reply_frames, (void *)(ulong)aligned_mem_dma));

	for (i = 0; i < ioc->reply_depth; i++) {
		/*  Write each address to the IOC!  */
		CHIPREG_WRITE32(&ioc->chip->ReplyFifo, aligned_mem_dma);
		aligned_mem_dma += ioc->reply_sz;
	}


	/* Initialize Request frames linked list
	 */
	aligned_mem_dma = ioc->req_frames_dma;
	aligned_mem = (u8 *) ioc->req_frames;
	dprintk((KERN_INFO MYNAM ": %s.req_frames   @ %p[%p]\n",
		 	ioc->name, aligned_mem, (void *)(ulong)aligned_mem_dma));

	spin_lock_irqsave(&ioc->FreeQlock, flags);
	Q_INIT(&ioc->FreeQ, MPT_FRAME_HDR);
	for (i = 0; i < ioc->req_depth; i++) {
		mf = (MPT_FRAME_HDR *) aligned_mem;

		/*  Queue REQUESTs *internally*!  */
		Q_ADD_TAIL(&ioc->FreeQ.head, &mf->u.frame.linkage, MPT_FRAME_HDR);
		aligned_mem += ioc->req_sz;
	}
	spin_unlock_irqrestore(&ioc->FreeQlock, flags);


	if (ioc->sense_buf_pool == NULL) {
		sz = (ioc->req_depth * MPT_SENSE_BUFFER_ALLOC);
		ioc->sense_buf_pool =
				pci_alloc_consistent(ioc->pcidev, sz, &ioc->sense_buf_pool_dma);
		if (ioc->sense_buf_pool == NULL)
			goto out_fail;

		ioc->sense_buf_low_dma = (u32) (ioc->sense_buf_pool_dma & 0xFFFFFFFF);
		ioc->alloc_total += sz;
	}

	return 0;

out_fail:
	if (ioc->fifo_pool != NULL) {
		pci_free_consistent(ioc->pcidev,
				ioc->fifo_pool_sz,
				ioc->fifo_pool, ioc->fifo_pool_dma);
		ioc->reply_frames = NULL;
		ioc->reply_alloc = NULL;
		ioc->req_frames = NULL;
		ioc->req_alloc = NULL;
		ioc->chain_alloc = NULL;
		ioc->fifo_pool = NULL;
		ioc->alloc_total -= ioc->fifo_pool_sz;
#if defined(CONFIG_MTRR) && 0
		if (ioc->mtrr_reg > 0) {
			mtrr_del(ioc->mtrr_reg, 0, 0);
			dprintk((MYIOC_s_INFO_FMT "MTRR region de-registered\n",
					ioc->name));
		}
#endif
	}
	if (ioc->sense_buf_pool != NULL) {
		sz = (ioc->req_depth * MPT_SENSE_BUFFER_ALLOC);
		pci_free_consistent(ioc->pcidev,
				sz,
				ioc->sense_buf_pool, ioc->sense_buf_pool_dma);
		ioc->sense_buf_pool = NULL;
	}
	return -1;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_handshake_req_reply_wait - Send MPT request to and receive reply
 *	from IOC via doorbell handshake method.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@reqBytes: Size of the request in bytes
 *	@req: Pointer to MPT request frame
 *	@replyBytes: Expected size of the reply in bytes
 *	@u16reply: Pointer to area where reply should be written
 *	@maxwait: Max wait time for a reply (in seconds)
 *	@sleepFlag: Specifies whether the process can sleep
 *
 *	NOTES: It is the callers responsibility to byte-swap fields in the
 *	request which are greater than 1 byte in size.  It is also the
 *	callers responsibility to byte-swap response fields which are
 *	greater than 1 byte in size.
 *
 *	Returns 0 for success, non-zero for failure.
 */
int
mpt_handshake_req_reply_wait(MPT_ADAPTER *ioc, int reqBytes, u32 *req,
				int replyBytes, u16 *u16reply, int maxwait, int sleepFlag)
{
	MPIDefaultReply_t *mptReply;
	int failcnt = 0;
	int t;

	/*
	 * Get ready to cache a handshake reply
	 */
	ioc->hs_reply_idx = 0;
	mptReply = (MPIDefaultReply_t *) ioc->hs_reply;
	mptReply->MsgLength = 0;

	/*
	 * Make sure there are no doorbells (WRITE 0 to IntStatus reg),
	 * then tell IOC that we want to handshake a request of N words.
	 * (WRITE u32val to Doorbell reg).
	 */
	CHIPREG_WRITE32(&ioc->chip->IntStatus, 0);
	CHIPREG_WRITE32(&ioc->chip->Doorbell,
			((MPI_FUNCTION_HANDSHAKE<<MPI_DOORBELL_FUNCTION_SHIFT) |
			 ((reqBytes/4)<<MPI_DOORBELL_ADD_DWORDS_SHIFT)));

	/*
	 * Wait for IOC's doorbell handshake int
	 */
	if ((t = WaitForDoorbellInt(ioc, 5, sleepFlag)) < 0)
		failcnt++;

	dhsprintk((MYIOC_s_INFO_FMT "HandShake request start, WaitCnt=%d%s\n",
			ioc->name, t, failcnt ? " - MISSING DOORBELL HANDSHAKE!" : ""));

	/* Read doorbell and check for active bit */
	if (!(CHIPREG_READ32(&ioc->chip->Doorbell) & MPI_DOORBELL_ACTIVE))
			return -1;

	/*
	 * Clear doorbell int (WRITE 0 to IntStatus reg),
	 * then wait for IOC to ACKnowledge that it's ready for
	 * our handshake request.
	 */
	CHIPREG_WRITE32(&ioc->chip->IntStatus, 0);
	if (!failcnt && (t = WaitForDoorbellAck(ioc, 5, sleepFlag)) < 0)
		failcnt++;

	if (!failcnt) {
		int	 ii;
		u8	*req_as_bytes = (u8 *) req;

		/*
		 * Stuff request words via doorbell handshake,
		 * with ACK from IOC for each.
		 */
		for (ii = 0; !failcnt && ii < reqBytes/4; ii++) {
			u32 word = ((req_as_bytes[(ii*4) + 0] <<  0) |
				    (req_as_bytes[(ii*4) + 1] <<  8) |
				    (req_as_bytes[(ii*4) + 2] << 16) |
				    (req_as_bytes[(ii*4) + 3] << 24));

			CHIPREG_WRITE32(&ioc->chip->Doorbell, word);
			if ((t = WaitForDoorbellAck(ioc, 5, sleepFlag)) < 0)
				failcnt++;
		}

		dmfprintk((KERN_INFO MYNAM ": Handshake request frame (@%p) header\n", req));
		DBG_DUMP_REQUEST_FRAME_HDR(req)

		dhsprintk((MYIOC_s_INFO_FMT "HandShake request post done, WaitCnt=%d%s\n",
				ioc->name, t, failcnt ? " - MISSING DOORBELL ACK!" : ""));

		/*
		 * Wait for completion of doorbell handshake reply from the IOC
		 */
		if (!failcnt && (t = WaitForDoorbellReply(ioc, maxwait, sleepFlag)) < 0)
			failcnt++;
		
		dhsprintk((MYIOC_s_INFO_FMT "HandShake reply count=%d%s\n",
				ioc->name, t, failcnt ? " - MISSING DOORBELL REPLY!" : ""));

		/*
		 * Copy out the cached reply...
		 */
		for (ii=0; ii < min(replyBytes/2,mptReply->MsgLength*2); ii++)
			u16reply[ii] = ioc->hs_reply[ii];
	} else {
		return -99;
	}

	return -failcnt;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	WaitForDoorbellAck - Wait for IOC to clear the IOP_DOORBELL_STATUS bit
 *	in it's IntStatus register.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@howlong: How long to wait (in seconds)
 *	@sleepFlag: Specifies whether the process can sleep
 *
 *	This routine waits (up to ~2 seconds max) for IOC doorbell
 *	handshake ACKnowledge.
 *
 *	Returns a negative value on failure, else wait loop count.
 */
static int
WaitForDoorbellAck(MPT_ADAPTER *ioc, int howlong, int sleepFlag)
{
	int cntdn;
	int count = 0;
	u32 intstat=0;

	cntdn = ((sleepFlag == CAN_SLEEP) ? HZ : 1000) * howlong;

	if (sleepFlag == CAN_SLEEP) {
		while (--cntdn) {
			intstat = CHIPREG_READ32(&ioc->chip->IntStatus);
			if (! (intstat & MPI_HIS_IOP_DOORBELL_STATUS))
				break;
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(1);
			count++;
		}
	} else {
		while (--cntdn) {
			intstat = CHIPREG_READ32(&ioc->chip->IntStatus);
			if (! (intstat & MPI_HIS_IOP_DOORBELL_STATUS))
				break;
			mdelay (1);
			count++;
		}
	}

	if (cntdn) {
		dprintk((MYIOC_s_INFO_FMT "WaitForDoorbell ACK (count=%d)\n",
				ioc->name, count));
		return count;
	}

	printk(MYIOC_s_ERR_FMT "Doorbell ACK timeout (count=%d), IntStatus=%x!\n",
			ioc->name, count, intstat);
	return -1;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	WaitForDoorbellInt - Wait for IOC to set the HIS_DOORBELL_INTERRUPT bit
 *	in it's IntStatus register.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@howlong: How long to wait (in seconds)
 *	@sleepFlag: Specifies whether the process can sleep
 *
 *	This routine waits (up to ~2 seconds max) for IOC doorbell interrupt.
 *
 *	Returns a negative value on failure, else wait loop count.
 */
static int
WaitForDoorbellInt(MPT_ADAPTER *ioc, int howlong, int sleepFlag)
{
	int cntdn;
	int count = 0;
	u32 intstat=0;

	cntdn = ((sleepFlag == CAN_SLEEP) ? HZ : 1000) * howlong;
	if (sleepFlag == CAN_SLEEP) {
		while (--cntdn) {
			intstat = CHIPREG_READ32(&ioc->chip->IntStatus);
			if (intstat & MPI_HIS_DOORBELL_INTERRUPT)
				break;
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(1);
			count++;
		}
	} else {
		while (--cntdn) {
			intstat = CHIPREG_READ32(&ioc->chip->IntStatus);
			if (intstat & MPI_HIS_DOORBELL_INTERRUPT)
				break;
			mdelay(1);
			count++;
		}
	}

	if (cntdn) {
		dprintk((MYIOC_s_INFO_FMT "WaitForDoorbell INT (cnt=%d) howlong=%d\n",
				ioc->name, count, howlong));
		return count;
	}

	printk(MYIOC_s_ERR_FMT "Doorbell INT timeout (count=%d), IntStatus=%x!\n",
			ioc->name, count, intstat);
	return -1;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	WaitForDoorbellReply - Wait for and capture a IOC handshake reply.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@howlong: How long to wait (in seconds)
 *	@sleepFlag: Specifies whether the process can sleep
 *
 *	This routine polls the IOC for a handshake reply, 16 bits at a time.
 *	Reply is cached to IOC private area large enough to hold a maximum
 *	of 128 bytes of reply data.
 *
 *	Returns a negative value on failure, else size of reply in WORDS.
 */
static int
WaitForDoorbellReply(MPT_ADAPTER *ioc, int howlong, int sleepFlag)
{
	int u16cnt = 0;
	int failcnt = 0;
	int t;
	u16 *hs_reply = ioc->hs_reply;
	volatile MPIDefaultReply_t *mptReply = (MPIDefaultReply_t *) ioc->hs_reply;
	u16 hword;

	hs_reply[0] = hs_reply[1] = hs_reply[7] = 0;

	/*
	 * Get first two u16's so we can look at IOC's intended reply MsgLength
	 */
	u16cnt=0;
	if ((t = WaitForDoorbellInt(ioc, howlong, sleepFlag)) < 0) {
		failcnt++;
	} else {
		hs_reply[u16cnt++] = le16_to_cpu(CHIPREG_READ32(&ioc->chip->Doorbell) & 0x0000FFFF);
		CHIPREG_WRITE32(&ioc->chip->IntStatus, 0);
		if ((t = WaitForDoorbellInt(ioc, 5, sleepFlag)) < 0)
			failcnt++;
		else {
			hs_reply[u16cnt++] = le16_to_cpu(CHIPREG_READ32(&ioc->chip->Doorbell) & 0x0000FFFF);
			CHIPREG_WRITE32(&ioc->chip->IntStatus, 0);
		}
	}

	dhsprintk((MYIOC_s_INFO_FMT "WaitCnt=%d First handshake reply word=%08x%s\n",
			ioc->name, t, le32_to_cpu(*(u32 *)hs_reply), 
			failcnt ? " - MISSING DOORBELL HANDSHAKE!" : ""));

	/*
	 * If no error (and IOC said MsgLength is > 0), piece together
	 * reply 16 bits at a time.
	 */
	for (u16cnt=2; !failcnt && u16cnt < (2 * mptReply->MsgLength); u16cnt++) {
		if ((t = WaitForDoorbellInt(ioc, 5, sleepFlag)) < 0)
			failcnt++;
		hword = le16_to_cpu(CHIPREG_READ32(&ioc->chip->Doorbell) & 0x0000FFFF);
		/* don't overflow our IOC hs_reply[] buffer! */
		if (u16cnt < sizeof(ioc->hs_reply) / sizeof(ioc->hs_reply[0]))
			hs_reply[u16cnt] = hword;
		CHIPREG_WRITE32(&ioc->chip->IntStatus, 0);
	}

	if (!failcnt && (t = WaitForDoorbellInt(ioc, 5, sleepFlag)) < 0)
		failcnt++;
	CHIPREG_WRITE32(&ioc->chip->IntStatus, 0);

	if (failcnt) {
		printk(MYIOC_s_ERR_FMT "Handshake reply failure!\n",
				ioc->name);
		return -failcnt;
	}
#if 0
	else if (u16cnt != (2 * mptReply->MsgLength)) {
		return -101;
	}
	else if ((mptReply->IOCStatus & MPI_IOCSTATUS_MASK) != MPI_IOCSTATUS_SUCCESS) {
		return -102;
	}
#endif

	dmfprintk((MYIOC_s_INFO_FMT "Got Handshake reply:\n", ioc->name));
	DBG_DUMP_REPLY_FRAME(mptReply)

	dhsprintk((MYIOC_s_INFO_FMT "WaitForDoorbell REPLY WaitCnt=%d (sz=%d)\n",
			ioc->name, t, u16cnt/2));
	return u16cnt/2;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	GetLanConfigPages - Fetch LANConfig pages.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *
 *	Return: 0 for success
 *	-ENOMEM if no memory available
 *		-EPERM if not allowed due to ISR context
 *		-EAGAIN if no msg frames currently available
 *		-EFAULT for non-successful reply or no reply (timeout)
 */
static int
GetLanConfigPages(MPT_ADAPTER *ioc)
{
	ConfigPageHeader_t	 hdr;
	CONFIGPARMS		 cfg;
	LANPage0_t		*ppage0_alloc;
	dma_addr_t		 page0_dma;
	LANPage1_t		*ppage1_alloc;
	dma_addr_t		 page1_dma;
	int			 rc = 0;
	int			 data_sz;
	int			 copy_sz;

	/* Get LAN Page 0 header */
	hdr.PageVersion = 0;
	hdr.PageLength = 0;
	hdr.PageNumber = 0;
	hdr.PageType = MPI_CONFIG_PAGETYPE_LAN;
	cfg.hdr = &hdr;
	cfg.physAddr = -1;
	cfg.action = MPI_CONFIG_ACTION_PAGE_HEADER;
	cfg.dir = 0;
	cfg.pageAddr = 0;
	cfg.timeout = 0;

	if ((rc = mpt_config(ioc, &cfg)) != 0)
		return rc;

	if (hdr.PageLength > 0) {
		data_sz = hdr.PageLength * 4;
		ppage0_alloc = (LANPage0_t *) pci_alloc_consistent(ioc->pcidev, data_sz, &page0_dma);
		rc = -ENOMEM;
		if (ppage0_alloc) {
			memset((u8 *)ppage0_alloc, 0, data_sz);
			cfg.physAddr = page0_dma;
			cfg.action = MPI_CONFIG_ACTION_PAGE_READ_CURRENT;

			if ((rc = mpt_config(ioc, &cfg)) == 0) {
				/* save the data */
				copy_sz = min_t(int, sizeof(LANPage0_t), data_sz);
				memcpy(&ioc->lan_cnfg_page0, ppage0_alloc, copy_sz);

			}

			pci_free_consistent(ioc->pcidev, data_sz, (u8 *) ppage0_alloc, page0_dma);

			/* FIXME!
			 *	Normalize endianness of structure data,
			 *	by byte-swapping all > 1 byte fields!
			 */

		}

		if (rc)
			return rc;
	}

	/* Get LAN Page 1 header */
	hdr.PageVersion = 0;
	hdr.PageLength = 0;
	hdr.PageNumber = 1;
	hdr.PageType = MPI_CONFIG_PAGETYPE_LAN;
	cfg.hdr = &hdr;
	cfg.physAddr = -1;
	cfg.action = MPI_CONFIG_ACTION_PAGE_HEADER;
	cfg.dir = 0;
	cfg.pageAddr = 0;

	if ((rc = mpt_config(ioc, &cfg)) != 0)
		return rc;

	if (hdr.PageLength == 0)
		return 0;

	data_sz = hdr.PageLength * 4;
	rc = -ENOMEM;
	ppage1_alloc = (LANPage1_t *) pci_alloc_consistent(ioc->pcidev, data_sz, &page1_dma);
	if (ppage1_alloc) {
		memset((u8 *)ppage1_alloc, 0, data_sz);
		cfg.physAddr = page1_dma;
		cfg.action = MPI_CONFIG_ACTION_PAGE_READ_CURRENT;

		if ((rc = mpt_config(ioc, &cfg)) == 0) {
			/* save the data */
			copy_sz = min_t(int, sizeof(LANPage1_t), data_sz);
			memcpy(&ioc->lan_cnfg_page1, ppage1_alloc, copy_sz);
		}

		pci_free_consistent(ioc->pcidev, data_sz, (u8 *) ppage1_alloc, page1_dma);

		/* FIXME!
		 *	Normalize endianness of structure data,
		 *	by byte-swapping all > 1 byte fields!
		 */

	}

	return rc;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	GetFcPortPage0 - Fetch FCPort config Page0.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@portnum: IOC Port number
 *
 *	Return: 0 for success
 *	-ENOMEM if no memory available
 *		-EPERM if not allowed due to ISR context
 *		-EAGAIN if no msg frames currently available
 *		-EFAULT for non-successful reply or no reply (timeout)
 */
static int
GetFcPortPage0(MPT_ADAPTER *ioc, int portnum)
{
	ConfigPageHeader_t	 hdr;
	CONFIGPARMS		 cfg;
	FCPortPage0_t		*ppage0_alloc;
	FCPortPage0_t		*pp0dest;
	dma_addr_t		 page0_dma;
	int			 data_sz;
	int			 copy_sz;
	int			 rc;

	/* Get FCPort Page 0 header */
	hdr.PageVersion = 0;
	hdr.PageLength = 0;
	hdr.PageNumber = 0;
	hdr.PageType = MPI_CONFIG_PAGETYPE_FC_PORT;
	cfg.hdr = &hdr;
	cfg.physAddr = -1;
	cfg.action = MPI_CONFIG_ACTION_PAGE_HEADER;
	cfg.dir = 0;
	cfg.pageAddr = portnum;
	cfg.timeout = 0;

	if ((rc = mpt_config(ioc, &cfg)) != 0)
		return rc;

	if (hdr.PageLength == 0)
		return 0;

	data_sz = hdr.PageLength * 4;
	rc = -ENOMEM;
	ppage0_alloc = (FCPortPage0_t *) pci_alloc_consistent(ioc->pcidev, data_sz, &page0_dma);
	if (ppage0_alloc) {
		memset((u8 *)ppage0_alloc, 0, data_sz);
		cfg.physAddr = page0_dma;
		cfg.action = MPI_CONFIG_ACTION_PAGE_READ_CURRENT;

		if ((rc = mpt_config(ioc, &cfg)) == 0) {
			/* save the data */
			pp0dest = &ioc->fc_port_page0[portnum];
			copy_sz = min_t(int, sizeof(FCPortPage0_t), data_sz);
			memcpy(pp0dest, ppage0_alloc, copy_sz);

			/*
			 *	Normalize endianness of structure data,
			 *	by byte-swapping all > 1 byte fields!
			 */
			pp0dest->Flags = le32_to_cpu(pp0dest->Flags);
			pp0dest->PortIdentifier = le32_to_cpu(pp0dest->PortIdentifier);
			pp0dest->WWNN.Low = le32_to_cpu(pp0dest->WWNN.Low);
			pp0dest->WWNN.High = le32_to_cpu(pp0dest->WWNN.High);
			pp0dest->WWPN.Low = le32_to_cpu(pp0dest->WWPN.Low);
			pp0dest->WWPN.High = le32_to_cpu(pp0dest->WWPN.High);
			pp0dest->SupportedServiceClass = le32_to_cpu(pp0dest->SupportedServiceClass);
			pp0dest->SupportedSpeeds = le32_to_cpu(pp0dest->SupportedSpeeds);
			pp0dest->CurrentSpeed = le32_to_cpu(pp0dest->CurrentSpeed);
			pp0dest->MaxFrameSize = le32_to_cpu(pp0dest->MaxFrameSize);
			pp0dest->FabricWWNN.Low = le32_to_cpu(pp0dest->FabricWWNN.Low);
			pp0dest->FabricWWNN.High = le32_to_cpu(pp0dest->FabricWWNN.High);
			pp0dest->FabricWWPN.Low = le32_to_cpu(pp0dest->FabricWWPN.Low);
			pp0dest->FabricWWPN.High = le32_to_cpu(pp0dest->FabricWWPN.High);
			pp0dest->DiscoveredPortsCount = le32_to_cpu(pp0dest->DiscoveredPortsCount);
			pp0dest->MaxInitiators = le32_to_cpu(pp0dest->MaxInitiators);

		}

		pci_free_consistent(ioc->pcidev, data_sz, (u8 *) ppage0_alloc, page0_dma);
	}

	return rc;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	GetIoUnitPage2 - Retrieve BIOS version and boot order information.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *
 *	Returns: 0 for success
 *	-ENOMEM if no memory available
 *		-EPERM if not allowed due to ISR context
 *		-EAGAIN if no msg frames currently available
 *		-EFAULT for non-successful reply or no reply (timeout)
 */
static int
GetIoUnitPage2(MPT_ADAPTER *ioc)
{
	ConfigPageHeader_t	 hdr;
	CONFIGPARMS		 cfg;
	IOUnitPage2_t		*ppage_alloc;
	dma_addr_t		 page_dma;
	int			 data_sz;
	int			 rc;

	/* Get the page header */
	hdr.PageVersion = 0;
	hdr.PageLength = 0;
	hdr.PageNumber = 2;
	hdr.PageType = MPI_CONFIG_PAGETYPE_IO_UNIT;
	cfg.hdr = &hdr;
	cfg.physAddr = -1;
	cfg.action = MPI_CONFIG_ACTION_PAGE_HEADER;
	cfg.dir = 0;
	cfg.pageAddr = 0;
	cfg.timeout = 0;

	if ((rc = mpt_config(ioc, &cfg)) != 0)
		return rc;

	if (hdr.PageLength == 0)
		return 0;

	/* Read the config page */
	data_sz = hdr.PageLength * 4;
	rc = -ENOMEM;
	ppage_alloc = (IOUnitPage2_t *) pci_alloc_consistent(ioc->pcidev, data_sz, &page_dma);
	if (ppage_alloc) {
		memset((u8 *)ppage_alloc, 0, data_sz);
		cfg.physAddr = page_dma;
		cfg.action = MPI_CONFIG_ACTION_PAGE_READ_CURRENT;

		/* If Good, save data */
		if ((rc = mpt_config(ioc, &cfg)) == 0)
			ioc->biosVersion = le32_to_cpu(ppage_alloc->BiosVersion);

		pci_free_consistent(ioc->pcidev, data_sz, (u8 *) ppage_alloc, page_dma);
	}

	return rc;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*	mpt_GetScsiPortSettings - read SCSI Port Page 0 and 2
 *	@ioc: Pointer to a Adapter Strucutre
 *	@portnum: IOC port number
 *
 *	Return: -EFAULT if read of config page header fails
 *			or if no nvram
 *	If read of SCSI Port Page 0 fails,
 *		NVRAM = MPT_HOST_NVRAM_INVALID  (0xFFFFFFFF)
 *		Adapter settings: async, narrow
 *		Return 1
 *	If read of SCSI Port Page 2 fails,
 *		Adapter settings valid
 *		NVRAM = MPT_HOST_NVRAM_INVALID  (0xFFFFFFFF)
 *		Return 1
 *	Else
 *		Both valid
 *		Return 0
 *	CHECK - what type of locking mechanisms should be used????
 */
static int
mpt_GetScsiPortSettings(MPT_ADAPTER *ioc, int portnum)
{
	u8			*pbuf;
	dma_addr_t		 buf_dma;
	CONFIGPARMS		 cfg;
	ConfigPageHeader_t	 header;
	int			 ii;
	int			 data, rc = 0;

	/* Allocate memory
	 */
	if (!ioc->spi_data.nvram) {
		int	 sz;
		u8	*mem;
		sz = MPT_MAX_SCSI_DEVICES * sizeof(int);
		mem = kmalloc(sz, GFP_ATOMIC);
		if (mem == NULL)
			return -EFAULT;

		ioc->spi_data.nvram = (int *) mem;

		dprintk((MYIOC_s_INFO_FMT "SCSI device NVRAM settings @ %p, sz=%d\n",
			ioc->name, ioc->spi_data.nvram, sz));
	}

	/* Invalidate NVRAM information
	 */
	for (ii=0; ii < MPT_MAX_SCSI_DEVICES; ii++) {
		ioc->spi_data.nvram[ii] = MPT_HOST_NVRAM_INVALID;
	}

	/* Read SPP0 header, allocate memory, then read page.
	 */
	header.PageVersion = 0;
	header.PageLength = 0;
	header.PageNumber = 0;
	header.PageType = MPI_CONFIG_PAGETYPE_SCSI_PORT;
	cfg.hdr = &header;
	cfg.physAddr = -1;
	cfg.pageAddr = portnum;
	cfg.action = MPI_CONFIG_ACTION_PAGE_HEADER;
	cfg.dir = 0;
	cfg.timeout = 0;	/* use default */
	if (mpt_config(ioc, &cfg) != 0)
		 return -EFAULT;

	if (header.PageLength > 0) {
		pbuf = pci_alloc_consistent(ioc->pcidev, header.PageLength * 4, &buf_dma);
		if (pbuf) {
			cfg.action = MPI_CONFIG_ACTION_PAGE_READ_CURRENT;
			cfg.physAddr = buf_dma;
			if (mpt_config(ioc, &cfg) != 0) {
				ioc->spi_data.maxBusWidth = MPT_NARROW;
				ioc->spi_data.maxSyncOffset = 0;
				ioc->spi_data.minSyncFactor = MPT_ASYNC;
				ioc->spi_data.busType = MPT_HOST_BUS_UNKNOWN;
				rc = 1;
			} else {
				/* Save the Port Page 0 data
				 */
				SCSIPortPage0_t  *pPP0 = (SCSIPortPage0_t  *) pbuf;
				pPP0->Capabilities = le32_to_cpu(pPP0->Capabilities);
				pPP0->PhysicalInterface = le32_to_cpu(pPP0->PhysicalInterface);

				if ( (pPP0->Capabilities & MPI_SCSIPORTPAGE0_CAP_QAS) == 0 )
					ioc->spi_data.noQas |= MPT_TARGET_NO_NEGO_QAS;

				ioc->spi_data.maxBusWidth = pPP0->Capabilities & MPI_SCSIPORTPAGE0_CAP_WIDE ? 1 : 0;
				data = pPP0->Capabilities & MPI_SCSIPORTPAGE0_CAP_MAX_SYNC_OFFSET_MASK;
				if (data) {
					ioc->spi_data.maxSyncOffset = (u8) (data >> 16);
					data = pPP0->Capabilities & MPI_SCSIPORTPAGE0_CAP_MIN_SYNC_PERIOD_MASK;
					ioc->spi_data.minSyncFactor = (u8) (data >> 8);
				} else {
					ioc->spi_data.maxSyncOffset = 0;
					ioc->spi_data.minSyncFactor = MPT_ASYNC;
				}

				ioc->spi_data.busType = pPP0->PhysicalInterface & MPI_SCSIPORTPAGE0_PHY_SIGNAL_TYPE_MASK;

				/* Update the minSyncFactor based on bus type.
				 */
				if ((ioc->spi_data.busType == MPI_SCSIPORTPAGE0_PHY_SIGNAL_HVD) ||
					(ioc->spi_data.busType == MPI_SCSIPORTPAGE0_PHY_SIGNAL_SE))  {

					if (ioc->spi_data.minSyncFactor < MPT_ULTRA)
						ioc->spi_data.minSyncFactor = MPT_ULTRA;
				}
			}
			if (pbuf) {
				pci_free_consistent(ioc->pcidev, header.PageLength * 4, pbuf, buf_dma);
			}
		}
	}

	/* SCSI Port Page 2 - Read the header then the page.
	 */
	header.PageVersion = 0;
	header.PageLength = 0;
	header.PageNumber = 2;
	header.PageType = MPI_CONFIG_PAGETYPE_SCSI_PORT;
	cfg.hdr = &header;
	cfg.physAddr = -1;
	cfg.pageAddr = portnum;
	cfg.action = MPI_CONFIG_ACTION_PAGE_HEADER;
	cfg.dir = 0;
	if (mpt_config(ioc, &cfg) != 0)
		return -EFAULT;

	if (header.PageLength > 0) {
		/* Allocate memory and read SCSI Port Page 2
		 */
		pbuf = pci_alloc_consistent(ioc->pcidev, header.PageLength * 4, &buf_dma);
		if (pbuf) {
			cfg.action = MPI_CONFIG_ACTION_PAGE_READ_NVRAM;
			cfg.physAddr = buf_dma;
			if (mpt_config(ioc, &cfg) != 0) {
				/* Nvram data is left with INVALID mark
				 */
				rc = 1;
			} else {
				SCSIPortPage2_t *pPP2 = (SCSIPortPage2_t  *) pbuf;
				MpiDeviceInfo_t	*pdevice = NULL;

				/* Save the Port Page 2 data
				 * (reformat into a 32bit quantity)
				 */
				data = le32_to_cpu(pPP2->PortFlags) & MPI_SCSIPORTPAGE2_PORT_FLAGS_DV_MASK;
				ioc->spi_data.PortFlags = data;
				for (ii=0; ii < MPT_MAX_SCSI_DEVICES; ii++) {
					pdevice = &pPP2->DeviceSettings[ii];
					data = (le16_to_cpu(pdevice->DeviceFlags) << 16) |
						(pdevice->SyncFactor << 8) | pdevice->Timeout;
					ioc->spi_data.nvram[ii] = data;
				}
			}

			pci_free_consistent(ioc->pcidev, header.PageLength * 4, pbuf, buf_dma);
		}
	}

	/* Update Adapter limits with those from NVRAM
	 * Comment: Don't need to do this. Target performance
	 * parameters will never exceed the adapters limits.
	 */

	return rc;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*	mpt_readScsiDevicePageHeaders - save version and length of SDP1
 *	@ioc: Pointer to a Adapter Strucutre
 *	@portnum: IOC port number
 *
 *	Return: -EFAULT if read of config page header fails
 *		or 0 if success.
 */
static int
mpt_readScsiDevicePageHeaders(MPT_ADAPTER *ioc, int portnum)
{
	CONFIGPARMS		 cfg;
	ConfigPageHeader_t	 header;

	/* Read the SCSI Device Page 1 header
	 */
	header.PageVersion = 0;
	header.PageLength = 0;
	header.PageNumber = 1;
	header.PageType = MPI_CONFIG_PAGETYPE_SCSI_DEVICE;
	cfg.hdr = &header;
	cfg.physAddr = -1;
	cfg.pageAddr = portnum;
	cfg.action = MPI_CONFIG_ACTION_PAGE_HEADER;
	cfg.dir = 0;
	cfg.timeout = 0;
	if (mpt_config(ioc, &cfg) != 0)
		 return -EFAULT;

	ioc->spi_data.sdp1version = cfg.hdr->PageVersion;
	ioc->spi_data.sdp1length = cfg.hdr->PageLength;

	header.PageVersion = 0;
	header.PageLength = 0;
	header.PageNumber = 0;
	header.PageType = MPI_CONFIG_PAGETYPE_SCSI_DEVICE;
	if (mpt_config(ioc, &cfg) != 0)
		 return -EFAULT;

	ioc->spi_data.sdp0version = cfg.hdr->PageVersion;
	ioc->spi_data.sdp0length = cfg.hdr->PageLength;

	dcprintk((MYIOC_s_INFO_FMT "Headers: 0: version %d length %d\n",
			ioc->name, ioc->spi_data.sdp0version, ioc->spi_data.sdp0length));

	dcprintk((MYIOC_s_INFO_FMT "Headers: 1: version %d length %d\n",
			ioc->name, ioc->spi_data.sdp1version, ioc->spi_data.sdp1length));
	return 0;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_findImVolumes - Identify IDs of hidden disks and RAID Volumes
 *	@ioc: Pointer to a Adapter Strucutre
 *	@portnum: IOC port number
 *
 *	Return:
 *	0 on success
 *	-EFAULT if read of config page header fails or data pointer not NULL
 *	-ENOMEM if pci_alloc failed
 */
int
mpt_findImVolumes(MPT_ADAPTER *ioc)
{
	IOCPage2_t		*pIoc2;
	u8			*mem;
	ConfigPageIoc2RaidVol_t	*pIocRv;
	dma_addr_t		 ioc2_dma;
	CONFIGPARMS		 cfg;
	ConfigPageHeader_t	 header;
	int			 jj;
	int			 rc = 0;
	int			 iocpage2sz;
	u8			 nVols, nPhys;
	u8			 vid, vbus, vioc;

	/* Read IOCP2 header then the page.
	 */
	header.PageVersion = 0;
	header.PageLength = 0;
	header.PageNumber = 2;
	header.PageType = MPI_CONFIG_PAGETYPE_IOC;
	cfg.hdr = &header;
	cfg.physAddr = -1;
	cfg.pageAddr = 0;
	cfg.action = MPI_CONFIG_ACTION_PAGE_HEADER;
	cfg.dir = 0;
	cfg.timeout = 0;
	if (mpt_config(ioc, &cfg) != 0)
		 return -EFAULT;

	if (header.PageLength == 0)
		return -EFAULT;

	iocpage2sz = header.PageLength * 4;
	pIoc2 = pci_alloc_consistent(ioc->pcidev, iocpage2sz, &ioc2_dma);
	if (!pIoc2)
		return -ENOMEM;

	cfg.action = MPI_CONFIG_ACTION_PAGE_READ_CURRENT;
	cfg.physAddr = ioc2_dma;
	if (mpt_config(ioc, &cfg) != 0)
		goto done_and_free;

	if ( (mem = (u8 *)ioc->spi_data.pIocPg2) == NULL ) {
		mem = kmalloc(iocpage2sz, GFP_ATOMIC);
		if (mem) {
			ioc->spi_data.pIocPg2 = (IOCPage2_t *) mem;
		} else {
			goto done_and_free;
		}
	}
	memcpy(mem, (u8 *)pIoc2, iocpage2sz);

	/* Identify RAID Volume Id's */
	nVols = pIoc2->NumActiveVolumes;
	if ( nVols == 0) {
		/* No RAID Volume.
		 */
		goto done_and_free;
	} else {
		/* At least 1 RAID Volume
		 */
		pIocRv = pIoc2->RaidVolume;
		ioc->spi_data.isRaid = 0;
		for (jj = 0; jj < nVols; jj++, pIocRv++) {
			vid = pIocRv->VolumeID;
			vbus = pIocRv->VolumeBus;
			vioc = pIocRv->VolumeIOC;

			/* find the match
			 */
			if (vbus == 0) {
				ioc->spi_data.isRaid |= (1 << vid);
			} else {
				/* Error! Always bus 0
				 */
			}
		}
	}

	/* Identify Hidden Physical Disk Id's */
	nPhys = pIoc2->NumActivePhysDisks;
	if (nPhys == 0) {
		/* No physical disks.
		 */
	} else {
		mpt_read_ioc_pg_3(ioc);
	}

done_and_free:
	pci_free_consistent(ioc->pcidev, iocpage2sz, pIoc2, ioc2_dma);

	return rc;
}

int
mpt_read_ioc_pg_3(MPT_ADAPTER *ioc)
{
	IOCPage3_t		*pIoc3;
	u8			*mem;
	CONFIGPARMS		 cfg;
	ConfigPageHeader_t	 header;
	dma_addr_t		 ioc3_dma;
	int			 iocpage3sz = 0;

	/* Free the old page
	 */
	if (ioc->spi_data.pIocPg3) {
		kfree(ioc->spi_data.pIocPg3);
		ioc->spi_data.pIocPg3 = NULL;
	}

	/* There is at least one physical disk.
	 * Read and save IOC Page 3
	 */
	header.PageVersion = 0;
	header.PageLength = 0;
	header.PageNumber = 3;
	header.PageType = MPI_CONFIG_PAGETYPE_IOC;
	cfg.hdr = &header;
	cfg.physAddr = -1;
	cfg.pageAddr = 0;
	cfg.action = MPI_CONFIG_ACTION_PAGE_HEADER;
	cfg.dir = 0;
	cfg.timeout = 0;
	if (mpt_config(ioc, &cfg) != 0)
		return 0;

	if (header.PageLength == 0)
		return 0;

	/* Read Header good, alloc memory
	 */
	iocpage3sz = header.PageLength * 4;
	pIoc3 = pci_alloc_consistent(ioc->pcidev, iocpage3sz, &ioc3_dma);
	if (!pIoc3)
		return 0;

	/* Read the Page and save the data
	 * into malloc'd memory.
	 */
	cfg.physAddr = ioc3_dma;
	cfg.action = MPI_CONFIG_ACTION_PAGE_READ_CURRENT;
	if (mpt_config(ioc, &cfg) == 0) {
		mem = kmalloc(iocpage3sz, GFP_ATOMIC);
		if (mem) {
			memcpy(mem, (u8 *)pIoc3, iocpage3sz);
			ioc->spi_data.pIocPg3 = (IOCPage3_t *) mem;
		}
	}

	pci_free_consistent(ioc->pcidev, iocpage3sz, pIoc3, ioc3_dma);

	return 0;
}

static void
mpt_read_ioc_pg_4(MPT_ADAPTER *ioc)
{
	IOCPage4_t		*pIoc4;
	CONFIGPARMS		 cfg;
	ConfigPageHeader_t	 header;
	dma_addr_t		 ioc4_dma;
	int			 iocpage4sz;

	/* Read and save IOC Page 4
	 */
	header.PageVersion = 0;
	header.PageLength = 0;
	header.PageNumber = 4;
	header.PageType = MPI_CONFIG_PAGETYPE_IOC;
	cfg.hdr = &header;
	cfg.physAddr = -1;
	cfg.pageAddr = 0;
	cfg.action = MPI_CONFIG_ACTION_PAGE_HEADER;
	cfg.dir = 0;
	cfg.timeout = 0;
	if (mpt_config(ioc, &cfg) != 0)
		return;

	if (header.PageLength == 0)
		return;

	if ( (pIoc4 = ioc->spi_data.pIocPg4) == NULL ) {
		iocpage4sz = (header.PageLength + 4) * 4; /* Allow 4 additional SEP's */
		pIoc4 = pci_alloc_consistent(ioc->pcidev, iocpage4sz, &ioc4_dma);
		if (!pIoc4)
			return;
	} else {
		ioc4_dma = ioc->spi_data.IocPg4_dma;
		iocpage4sz = ioc->spi_data.IocPg4Sz;
	}

	/* Read the Page into dma memory.
	 */
	cfg.physAddr = ioc4_dma;
	cfg.action = MPI_CONFIG_ACTION_PAGE_READ_CURRENT;
	if (mpt_config(ioc, &cfg) == 0) {
		ioc->spi_data.pIocPg4 = (IOCPage4_t *) pIoc4;
		ioc->spi_data.IocPg4_dma = ioc4_dma;
		ioc->spi_data.IocPg4Sz = iocpage4sz;
	} else {
		pci_free_consistent(ioc->pcidev, iocpage4sz, pIoc4, ioc4_dma);
		ioc->spi_data.pIocPg4 = NULL;
	}
}

static void
mpt_read_ioc_pg_1(MPT_ADAPTER *ioc)
{
	IOCPage1_t		*pIoc1;
	CONFIGPARMS		 cfg;
	ConfigPageHeader_t	 header;
	dma_addr_t		 ioc1_dma;
	int			 iocpage1sz = 0;
	u32			 tmp;

	/* Check the Coalescing Timeout in IOC Page 1
	 */
	header.PageVersion = 0;
	header.PageLength = 0;
	header.PageNumber = 1;
	header.PageType = MPI_CONFIG_PAGETYPE_IOC;
	cfg.hdr = &header;
	cfg.physAddr = -1;
	cfg.pageAddr = 0;
	cfg.action = MPI_CONFIG_ACTION_PAGE_HEADER;
	cfg.dir = 0;
	cfg.timeout = 0;
	if (mpt_config(ioc, &cfg) != 0)
		return;

	if (header.PageLength == 0)
		return;

	/* Read Header good, alloc memory
	 */
	iocpage1sz = header.PageLength * 4;
	pIoc1 = pci_alloc_consistent(ioc->pcidev, iocpage1sz, &ioc1_dma);
	if (!pIoc1)
		return;

	/* Read the Page and check coalescing timeout
	 */
	cfg.physAddr = ioc1_dma;
	cfg.action = MPI_CONFIG_ACTION_PAGE_READ_CURRENT;
	if (mpt_config(ioc, &cfg) == 0) {
		
		tmp = le32_to_cpu(pIoc1->Flags) & MPI_IOCPAGE1_REPLY_COALESCING;
		if (tmp == MPI_IOCPAGE1_REPLY_COALESCING) {
			tmp = le32_to_cpu(pIoc1->CoalescingTimeout);

			dprintk((MYIOC_s_INFO_FMT "Coalescing Enabled Timeout = %d\n",
					ioc->name, tmp));

			if (tmp > MPT_COALESCING_TIMEOUT) {
				pIoc1->CoalescingTimeout = cpu_to_le32(MPT_COALESCING_TIMEOUT);

				/* Write NVRAM and current
				 */
				cfg.dir = 1;
				cfg.action = MPI_CONFIG_ACTION_PAGE_WRITE_CURRENT;
				if (mpt_config(ioc, &cfg) == 0) {
					dprintk((MYIOC_s_INFO_FMT "Reset Current Coalescing Timeout to = %d\n",
							ioc->name, MPT_COALESCING_TIMEOUT));

					cfg.action = MPI_CONFIG_ACTION_PAGE_WRITE_NVRAM;
					if (mpt_config(ioc, &cfg) == 0) {
						dprintk((MYIOC_s_INFO_FMT "Reset NVRAM Coalescing Timeout to = %d\n",
								ioc->name, MPT_COALESCING_TIMEOUT));
					} else {
						dprintk((MYIOC_s_INFO_FMT "Reset NVRAM Coalescing Timeout Failed\n",
									ioc->name));
					}

				} else {
					dprintk((MYIOC_s_WARN_FMT "Reset of Current Coalescing Timeout Failed!\n",
								ioc->name));
				}
			}

		} else {
			dprintk((MYIOC_s_WARN_FMT "Coalescing Disabled\n", ioc->name));
		}
	}

	pci_free_consistent(ioc->pcidev, iocpage1sz, pIoc1, ioc1_dma);

	return;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	SendEventNotification - Send EventNotification (on or off) request
 *	to MPT adapter.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@EvSwitch: Event switch flags
 */
static int
SendEventNotification(MPT_ADAPTER *ioc, u8 EvSwitch)
{
	EventNotification_t	*evnp;

	evnp = (EventNotification_t *) mpt_get_msg_frame(mpt_base_index, ioc);
	if (evnp == NULL) {
		dprintk((MYIOC_s_WARN_FMT "Unable to allocate event request frame!\n",
				ioc->name));
		return 0;
	}
	memset(evnp, 0, sizeof(*evnp));

	dprintk((MYIOC_s_INFO_FMT "Sending EventNotification(%d)\n", ioc->name, EvSwitch));

	evnp->Function = MPI_FUNCTION_EVENT_NOTIFICATION;
	evnp->ChainOffset = 0;
	evnp->MsgFlags = 0;
	evnp->Switch = EvSwitch;

	mpt_put_msg_frame(mpt_base_index, ioc, (MPT_FRAME_HDR *)evnp);

	return 0;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	SendEventAck - Send EventAck request to MPT adapter.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@evnp: Pointer to original EventNotification request
 */
static int
SendEventAck(MPT_ADAPTER *ioc, EventNotificationReply_t *evnp)
{
	EventAck_t	*pAck;

	if ((pAck = (EventAck_t *) mpt_get_msg_frame(mpt_base_index, ioc)) == NULL) {
		printk(MYIOC_s_WARN_FMT "Unable to allocate event ACK request frame!\n",
				ioc->name);
		return -1;
	}
	memset(pAck, 0, sizeof(*pAck));

	dprintk((MYIOC_s_INFO_FMT "Sending EventAck\n", ioc->name));

	pAck->Function     = MPI_FUNCTION_EVENT_ACK;
	pAck->ChainOffset  = 0;
	pAck->MsgFlags     = 0;
	pAck->Event        = evnp->Event;
	pAck->EventContext = evnp->EventContext;

	mpt_put_msg_frame(mpt_base_index, ioc, (MPT_FRAME_HDR *)pAck);

	return 0;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_config - Generic function to issue config message
 *	@ioc - Pointer to an adapter structure
 *	@cfg - Pointer to a configuration structure. Struct contains
 *		action, page address, direction, physical address
 *		and pointer to a configuration page header
 *		Page header is updated.
 *
 *	Returns 0 for success
 *	-EPERM if not allowed due to ISR context
 *	-EAGAIN if no msg frames currently available
 *	-EFAULT for non-successful reply or no reply (timeout)
 */
int
mpt_config(MPT_ADAPTER *ioc, CONFIGPARMS *pCfg)
{
	Config_t	*pReq;
	MPT_FRAME_HDR	*mf;
	unsigned long	 flags;
	int		 ii, rc;
	u32		 flagsLength;
	int		 in_isr;

	/* (Bugzilla:fibrebugs, #513)
	 * Bug fix (part 1)!  20010905 -sralston
	 *	Prevent calling wait_event() (below), if caller happens
	 *	to be in ISR context, because that is fatal!
	 */
	in_isr = in_interrupt();
	if (in_isr) {
		dcprintk((MYIOC_s_WARN_FMT "Config request not allowed in ISR context!\n",
				ioc->name));
		return -EPERM;
	}

	/* Get and Populate a free Frame
	 */
	if ((mf = mpt_get_msg_frame(mpt_base_index, ioc)) == NULL) {
		dcprintk((MYIOC_s_WARN_FMT "mpt_config: no msg frames!\n",
				ioc->name));
		return -EAGAIN;
	}
	pReq = (Config_t *)mf;
	pReq->Action = pCfg->action;
	pReq->Reserved = 0;
	pReq->ChainOffset = 0;
	pReq->Function = MPI_FUNCTION_CONFIG;
	pReq->ExtPageLength = 0;
	pReq->ExtPageType = 0;
	pReq->MsgFlags = 0;
	for (ii=0; ii < 8; ii++)
		pReq->Reserved2[ii] = 0;

	pReq->Header.PageVersion = pCfg->hdr->PageVersion;
	pReq->Header.PageLength = pCfg->hdr->PageLength;
	pReq->Header.PageNumber = pCfg->hdr->PageNumber;
	pReq->Header.PageType = (pCfg->hdr->PageType & MPI_CONFIG_PAGETYPE_MASK);
	pReq->PageAddress = cpu_to_le32(pCfg->pageAddr);

	/* Add a SGE to the config request.
	 */
	if (pCfg->dir)
		flagsLength = MPT_SGE_FLAGS_SSIMPLE_WRITE;
	else
		flagsLength = MPT_SGE_FLAGS_SSIMPLE_READ;

	flagsLength |= pCfg->hdr->PageLength * 4;

	mpt_add_sge((char *)&pReq->PageBufferSGE, flagsLength, pCfg->physAddr);

	dcprintk((MYIOC_s_INFO_FMT "Sending Config request type %d, page %d and action %d\n",
		ioc->name, pReq->Header.PageType, pReq->Header.PageNumber, pReq->Action));

	/* Append pCfg pointer to end of mf
	 */
	*((void **) (((u8 *) mf) + (ioc->req_sz - sizeof(void *)))) =  (void *) pCfg;

	/* Initalize the timer
	 */
	init_timer(&pCfg->timer);
	pCfg->timer.data = (unsigned long) ioc;
	pCfg->timer.function = mpt_timer_expired;
	pCfg->wait_done = 0;

	/* Set the timer; ensure 10 second minimum */
	if (pCfg->timeout < 10)
		pCfg->timer.expires = jiffies + HZ*10;
	else
		pCfg->timer.expires = jiffies + HZ*pCfg->timeout;

	/* Add to end of Q, set timer and then issue this command */
	spin_lock_irqsave(&ioc->FreeQlock, flags);
	Q_ADD_TAIL(&ioc->configQ.head, &pCfg->linkage, Q_ITEM);
	spin_unlock_irqrestore(&ioc->FreeQlock, flags);

	add_timer(&pCfg->timer);
	mpt_put_msg_frame(mpt_base_index, ioc, mf);
	wait_event(mpt_waitq, pCfg->wait_done);

	/* mf has been freed - do not access */

	rc = pCfg->status;

	return rc;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_toolbox - Generic function to issue toolbox message
 *	@ioc - Pointer to an adapter structure
 *	@cfg - Pointer to a toolbox structure. Struct contains
 *		action, page address, direction, physical address
 *		and pointer to a configuration page header
 *		Page header is updated.
 *
 *	Returns 0 for success
 *	-EPERM if not allowed due to ISR context
 *	-EAGAIN if no msg frames currently available
 *	-EFAULT for non-successful reply or no reply (timeout)
 */
int
mpt_toolbox(MPT_ADAPTER *ioc, CONFIGPARMS *pCfg)
{
	ToolboxIstwiReadWriteRequest_t	*pReq;
	struct pci_dev	*pdev;
	MPT_FRAME_HDR	*mf;
	unsigned long	 flags;
	int		 rc;
	u32		 flagsLength;
	int		 in_isr;

	/* (Bugzilla:fibrebugs, #513)
	 * Bug fix (part 1)!  20010905 -sralston
	 *	Prevent calling wait_event() (below), if caller happens
	 *	to be in ISR context, because that is fatal!
	 */
	in_isr = in_interrupt();
	if (in_isr) {
		dcprintk((MYIOC_s_WARN_FMT "toobox request not allowed in ISR context!\n",
				ioc->name));
		return -EPERM;
	}

	/* Get and Populate a free Frame
	 */
	if ((mf = mpt_get_msg_frame(mpt_base_index, ioc)) == NULL) {
		dcprintk((MYIOC_s_WARN_FMT "mpt_toolbox: no msg frames!\n",
				ioc->name));
		return -EAGAIN;
	}
	pReq = (ToolboxIstwiReadWriteRequest_t	*)mf;
	pReq->Tool = pCfg->action;
	pReq->Reserved = 0;
	pReq->ChainOffset = 0;
	pReq->Function = MPI_FUNCTION_TOOLBOX;
	pReq->Reserved1 = 0;
	pReq->Reserved2 = 0;
	pReq->MsgFlags = 0;
	pReq->Flags = pCfg->dir;
	pReq->BusNum = 0;
	pReq->Reserved3 = 0;
	pReq->NumAddressBytes = 0x01;
	pReq->Reserved4 = 0;
	pReq->DataLength = 0x04;
	pdev = (struct pci_dev *) ioc->pcidev;
	if (pdev->devfn & 1)
		pReq->DeviceAddr = 0xB2;
	else
		pReq->DeviceAddr = 0xB0;
	pReq->Addr1 = 0;
	pReq->Addr2 = 0;
	pReq->Addr3 = 0;
	pReq->Reserved5 = 0;

	/* Add a SGE to the config request.
	 */

	flagsLength = MPT_SGE_FLAGS_SSIMPLE_READ | 4;

	mpt_add_sge((char *)&pReq->SGL, flagsLength, pCfg->physAddr);

	dcprintk((MYIOC_s_INFO_FMT "Sending Toolbox request, Tool=%x\n",
		ioc->name, pReq->Tool));

	/* Append pCfg pointer to end of mf
	 */
	*((void **) (((u8 *) mf) + (ioc->req_sz - sizeof(void *)))) =  (void *) pCfg;

	/* Initalize the timer
	 */
	init_timer(&pCfg->timer);
	pCfg->timer.data = (unsigned long) ioc;
	pCfg->timer.function = mpt_timer_expired;
	pCfg->wait_done = 0;

	/* Set the timer; ensure 10 second minimum */
	if (pCfg->timeout < 10)
		pCfg->timer.expires = jiffies + HZ*10;
	else
		pCfg->timer.expires = jiffies + HZ*pCfg->timeout;

	/* Add to end of Q, set timer and then issue this command */
	spin_lock_irqsave(&ioc->FreeQlock, flags);
	Q_ADD_TAIL(&ioc->configQ.head, &pCfg->linkage, Q_ITEM);
	spin_unlock_irqrestore(&ioc->FreeQlock, flags);

	add_timer(&pCfg->timer);
	mpt_put_msg_frame(mpt_base_index, ioc, mf);
	wait_event(mpt_waitq, pCfg->wait_done);

	/* mf has been freed - do not access */

	rc = pCfg->status;

	return rc;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mpt_timer_expired - Call back for timer process.
 *	Used only internal config functionality.
 *	@data: Pointer to MPT_SCSI_HOST recast as an unsigned long
 */
static void
mpt_timer_expired(unsigned long data)
{
	MPT_ADAPTER *ioc = (MPT_ADAPTER *) data;

	dcprintk((MYIOC_s_WARN_FMT "mpt_timer_expired! \n", ioc->name));

	/* Perform a FW reload */
	if (mpt_HardResetHandler(ioc, NO_SLEEP) < 0)
		printk(MYIOC_s_WARN_FMT "Firmware Reload FAILED!\n", ioc->name);

	/* No more processing.
	 * Hard reset clean-up will wake up
	 * process and free all resources.
	 */
	dcprintk((MYIOC_s_WARN_FMT "mpt_timer_expired complete!\n", ioc->name));

	return;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mpt_ioc_reset - Base cleanup for hard reset
 *	@ioc: Pointer to the adapter structure
 *	@reset_phase: Indicates pre- or post-reset functionality
 *
 *	Remark: Free's resources with internally generated commands.
 */
static int
mpt_ioc_reset(MPT_ADAPTER *ioc, int reset_phase)
{
	CONFIGPARMS *pCfg;
	unsigned long flags;

	dprintk((KERN_WARNING MYNAM
			": IOC %s_reset routed to MPT base driver!\n",
			reset_phase==MPT_IOC_SETUP_RESET ? "setup" : (
			reset_phase==MPT_IOC_PRE_RESET ? "pre" : "post")));

	if (reset_phase == MPT_IOC_SETUP_RESET) {
		;
	} else if (reset_phase == MPT_IOC_PRE_RESET) {
		/* If the internal config Q is not empty -
		 * delete timer. MF resources will be freed when
		 * the FIFO's are primed.
		 */
		spin_lock_irqsave(&ioc->FreeQlock, flags);
		if (! Q_IS_EMPTY(&ioc->configQ)){
			pCfg = (CONFIGPARMS *)ioc->configQ.head;
			do {
				del_timer(&pCfg->timer);
				pCfg = (CONFIGPARMS *) (pCfg->linkage.forw);
			} while (pCfg != (CONFIGPARMS *)&ioc->configQ);
		}
		spin_unlock_irqrestore(&ioc->FreeQlock, flags);

	} else {
		CONFIGPARMS *pNext;

		/* Search the configQ for internal commands.
		 * Flush the Q, and wake up all suspended threads.
		 */
		spin_lock_irqsave(&ioc->FreeQlock, flags);
		if (! Q_IS_EMPTY(&ioc->configQ)){
			pCfg = (CONFIGPARMS *)ioc->configQ.head;
			do {
				pNext = (CONFIGPARMS *) pCfg->linkage.forw;

				Q_DEL_ITEM(&pCfg->linkage);

				pCfg->status = MPT_CONFIG_ERROR;
				pCfg->wait_done = 1;
				wake_up(&mpt_waitq);

				pCfg = pNext;
			} while (pCfg != (CONFIGPARMS *)&ioc->configQ);
		}
		spin_unlock_irqrestore(&ioc->FreeQlock, flags);
	}

	return 1;		/* currently means nothing really */
}


#ifdef CONFIG_PROC_FS		/* { */
/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	procfs (%MPT_PROCFS_MPTBASEDIR/...) support stuff...
 */
/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	procmpt_create - Create %MPT_PROCFS_MPTBASEDIR entries.
 *
 *	Returns 0 for success, non-zero for failure.
 */
static int
procmpt_create(void)
{
	struct proc_dir_entry	*ent;

	mpt_proc_root_dir = proc_mkdir(MPT_PROCFS_MPTBASEDIR, NULL);
	if (mpt_proc_root_dir == NULL)
		return -ENOTDIR;

	ent = create_proc_entry("summary", S_IFREG|S_IRUGO, mpt_proc_root_dir);
	if (ent)
		ent->read_proc = procmpt_summary_read;

	ent = create_proc_entry("version", S_IFREG|S_IRUGO, mpt_proc_root_dir);
	if (ent)
		ent->read_proc = procmpt_version_read;

	return 0;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	procmpt_destroy - Tear down %MPT_PROCFS_MPTBASEDIR entries.
 *
 *	Returns 0 for success, non-zero for failure.
 */
static void
procmpt_destroy(void)
{
	remove_proc_entry("version", mpt_proc_root_dir);
	remove_proc_entry("summary", mpt_proc_root_dir);
	remove_proc_entry(MPT_PROCFS_MPTBASEDIR, NULL);
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	procmpt_summary_read - Handle read request from /proc/mpt/summary
 *	or from /proc/mpt/iocN/summary.
 *	@buf: Pointer to area to write information
 *	@start: Pointer to start pointer
 *	@offset: Offset to start writing
 *	@request:
 *	@eof: Pointer to EOF integer
 *	@data: Pointer
 *
 *	Returns number of characters written to process performing the read.
 */
static int
procmpt_summary_read(char *buf, char **start, off_t offset, int request, int *eof, void *data)
{
	MPT_ADAPTER *ioc;
	char *out = buf;
	int len;

	if (data) {
		int more = 0;

		ioc = data;
		mpt_print_ioc_summary(ioc, out, &more, 0, 1);

		out += more;
	} else {
		list_for_each_entry(ioc, &ioc_list, list) {
			int	more = 0;

			mpt_print_ioc_summary(ioc, out, &more, 0, 1);

			out += more;
			if ((out-buf) >= request)
				break;
		}
	}

	len = out - buf;

	MPT_PROC_READ_RETURN(buf,start,offset,request,eof,len);
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	procmpt_version_read - Handle read request from /proc/mpt/version.
 *	@buf: Pointer to area to write information
 *	@start: Pointer to start pointer
 *	@offset: Offset to start writing
 *	@request:
 *	@eof: Pointer to EOF integer
 *	@data: Pointer
 *
 *	Returns number of characters written to process performing the read.
 */
static int
procmpt_version_read(char *buf, char **start, off_t offset, int request, int *eof, void *data)
{
	int	 ii;
	int	 scsi, lan, ctl, targ, dmp;
	char	*drvname;
	int	 len;

	len = sprintf(buf, "%s-%s\n", "mptlinux", MPT_LINUX_VERSION_COMMON);
	len += sprintf(buf+len, "  Fusion MPT base driver\n");

	scsi = lan = ctl = targ = dmp = 0;
	for (ii=MPT_MAX_PROTOCOL_DRIVERS-1; ii; ii--) {
		drvname = NULL;
		if (MptCallbacks[ii]) {
			switch (MptDriverClass[ii]) {
			case MPTSCSIH_DRIVER:
				if (!scsi++) drvname = "SCSI host";
				break;
			case MPTLAN_DRIVER:
				if (!lan++) drvname = "LAN";
				break;
			case MPTSTM_DRIVER:
				if (!targ++) drvname = "SCSI target";
				break;
			case MPTCTL_DRIVER:
				if (!ctl++) drvname = "ioctl";
				break;
			case MPTDMP_DRIVER:
				if (!dmp++) drvname = "DMP";
				break;
			}

			if (drvname)
				len += sprintf(buf+len, "  Fusion MPT %s driver\n", drvname);
			/*
			 *	Handle isense special case, because it
			 *	doesn't do a formal mpt_register call.
			 */
			if (isense_idx == ii)
				len += sprintf(buf+len, "  Fusion MPT isense driver\n");
		}
	}

	MPT_PROC_READ_RETURN(buf,start,offset,request,eof,len);
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	procmpt_iocinfo_read - Handle read request from /proc/mpt/iocN/info.
 *	@buf: Pointer to area to write information
 *	@start: Pointer to start pointer
 *	@offset: Offset to start writing
 *	@request:
 *	@eof: Pointer to EOF integer
 *	@data: Pointer
 *
 *	Returns number of characters written to process performing the read.
 */
static int
procmpt_iocinfo_read(char *buf, char **start, off_t offset, int request, int *eof, void *data)
{
	MPT_ADAPTER	*ioc = data;
	int		 len;
	char		 expVer[32];
	int		 sz;
	int		 p;

	mpt_get_fw_exp_ver(expVer, ioc);

	len = sprintf(buf, "%s:", ioc->name);
	if (ioc->facts.Flags & MPI_IOCFACTS_FLAGS_FW_DOWNLOAD_BOOT)
		len += sprintf(buf+len, "  (f/w download boot flag set)");
//	if (ioc->facts.IOCExceptions & MPI_IOCFACTS_EXCEPT_CONFIG_CHECKSUM_FAIL)
//		len += sprintf(buf+len, "  CONFIG_CHECKSUM_FAIL!");

	len += sprintf(buf+len, "\n  ProductID = 0x%04x (%s)\n",
			ioc->facts.ProductID,
			ioc->prod_name);
	len += sprintf(buf+len, "  FWVersion = 0x%08x%s", ioc->facts.FWVersion.Word, expVer);
	if (ioc->facts.FWImageSize)
		len += sprintf(buf+len, " (fw_size=%d)", ioc->facts.FWImageSize);
	len += sprintf(buf+len, "\n  MsgVersion = 0x%04x\n", ioc->facts.MsgVersion);
	len += sprintf(buf+len, "  FirstWhoInit = 0x%02x\n", ioc->FirstWhoInit);
	len += sprintf(buf+len, "  EventState = 0x%02x\n", ioc->facts.EventState);

	len += sprintf(buf+len, "  CurrentHostMfaHighAddr = 0x%08x\n",
			ioc->facts.CurrentHostMfaHighAddr);
	len += sprintf(buf+len, "  CurrentSenseBufferHighAddr = 0x%08x\n",
			ioc->facts.CurrentSenseBufferHighAddr);

	len += sprintf(buf+len, "  MaxChainDepth = 0x%02x frames\n", ioc->facts.MaxChainDepth);
	len += sprintf(buf+len, "  MinBlockSize = 0x%02x bytes\n", 4*ioc->facts.BlockSize);

	len += sprintf(buf+len, "  RequestFrames @ 0x%p (Dma @ 0x%p)\n",
					(void *)ioc->req_alloc, (void *)(ulong)ioc->req_alloc_dma);
	/*
	 *  Rounding UP to nearest 4-kB boundary here...
	 */
	sz = (ioc->req_sz * ioc->req_depth) + 128;
	sz = ((sz + 0x1000UL - 1UL) / 0x1000) * 0x1000;
	len += sprintf(buf+len, "    {CurReqSz=%d} x {CurReqDepth=%d} = %d bytes ^= 0x%x\n",
					ioc->req_sz, ioc->req_depth, ioc->req_sz*ioc->req_depth, sz);
	len += sprintf(buf+len, "    {MaxReqSz=%d}   {MaxReqDepth=%d}\n",
					4*ioc->facts.RequestFrameSize,
					ioc->facts.GlobalCredits);

	len += sprintf(buf+len, "  ReplyFrames   @ 0x%p (Dma @ 0x%p)\n",
					(void *)ioc->reply_alloc, (void *)(ulong)ioc->reply_alloc_dma);
	sz = (ioc->reply_sz * ioc->reply_depth) + 128;
	len += sprintf(buf+len, "    {CurRepSz=%d} x {CurRepDepth=%d} = %d bytes ^= 0x%x\n",
					ioc->reply_sz, ioc->reply_depth, ioc->reply_sz*ioc->reply_depth, sz);
	len += sprintf(buf+len, "    {MaxRepSz=%d}   {MaxRepDepth=%d}\n",
					ioc->facts.CurReplyFrameSize,
					ioc->facts.ReplyQueueDepth);

	len += sprintf(buf+len, "  MaxDevices = %d\n",
			(ioc->facts.MaxDevices==0) ? 255 : ioc->facts.MaxDevices);
	len += sprintf(buf+len, "  MaxBuses = %d\n", ioc->facts.MaxBuses);

	/* per-port info */
	for (p=0; p < ioc->facts.NumberOfPorts; p++) {
		len += sprintf(buf+len, "  PortNumber = %d (of %d)\n",
				p+1,
				ioc->facts.NumberOfPorts);
		if ((int)ioc->chip_type <= (int)FC929) {
			if (ioc->pfacts[p].ProtocolFlags & MPI_PORTFACTS_PROTOCOL_LAN) {
				u8 *a = (u8*)&ioc->lan_cnfg_page1.HardwareAddressLow;
				len += sprintf(buf+len, "    LanAddr = %02X:%02X:%02X:%02X:%02X:%02X\n",
						a[5], a[4], a[3], a[2], a[1], a[0]);
			}
			len += sprintf(buf+len, "    WWN = %08X%08X:%08X%08X\n",
					ioc->fc_port_page0[p].WWNN.High,
					ioc->fc_port_page0[p].WWNN.Low,
					ioc->fc_port_page0[p].WWPN.High,
					ioc->fc_port_page0[p].WWPN.Low);
		}
	}

	MPT_PROC_READ_RETURN(buf,start,offset,request,eof,len);
}

#endif		/* CONFIG_PROC_FS } */

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
static void
mpt_get_fw_exp_ver(char *buf, MPT_ADAPTER *ioc)
{
	buf[0] ='\0';
	if ((ioc->facts.FWVersion.Word >> 24) == 0x0E) {
		sprintf(buf, " (Exp %02d%02d)",
			(ioc->facts.FWVersion.Word >> 16) & 0x00FF,	/* Month */
			(ioc->facts.FWVersion.Word >> 8) & 0x1F);	/* Day */

		/* insider hack! */
		if ((ioc->facts.FWVersion.Word >> 8) & 0x80)
			strcat(buf, " [MDBG]");
	}
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_print_ioc_summary - Write ASCII summary of IOC to a buffer.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@buffer: Pointer to buffer where IOC summary info should be written
 *	@size: Pointer to number of bytes we wrote (set by this routine)
 *	@len: Offset at which to start writing in buffer
 *	@showlan: Display LAN stuff?
 *
 *	This routine writes (english readable) ASCII text, which represents
 *	a summary of IOC information, to a buffer.
 */
void
mpt_print_ioc_summary(MPT_ADAPTER *ioc, char *buffer, int *size, int len, int showlan)
{
	char expVer[32];
	int y;

	mpt_get_fw_exp_ver(expVer, ioc);

	/*
	 *  Shorter summary of attached ioc's...
	 */
	y = sprintf(buffer+len, "%s: %s, %s%08xh%s, Ports=%d, MaxQ=%d",
			ioc->name,
			ioc->prod_name,
			MPT_FW_REV_MAGIC_ID_STRING,	/* "FwRev=" or somesuch */
			ioc->facts.FWVersion.Word,
			expVer,
			ioc->facts.NumberOfPorts,
			ioc->req_depth);

	if (showlan && (ioc->pfacts[0].ProtocolFlags & MPI_PORTFACTS_PROTOCOL_LAN)) {
		u8 *a = (u8*)&ioc->lan_cnfg_page1.HardwareAddressLow;
		y += sprintf(buffer+len+y, ", LanAddr=%02X:%02X:%02X:%02X:%02X:%02X",
			a[5], a[4], a[3], a[2], a[1], a[0]);
	}

#ifndef __sparc__
	y += sprintf(buffer+len+y, ", IRQ=%d", ioc->pci_irq);
#else
	y += sprintf(buffer+len+y, ", IRQ=%s", __irq_itoa(ioc->pci_irq));
#endif

	if (!ioc->active)
		y += sprintf(buffer+len+y, " (disabled)");

	y += sprintf(buffer+len+y, "\n");

	*size = y;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	Reset Handling
 */
/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_HardResetHandler - Generic reset handler, issue SCSI Task
 *	Management call based on input arg values.  If TaskMgmt fails,
 *	return associated SCSI request.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@sleepFlag: Indicates if sleep or schedule must be called.
 *
 *	Remark: _HardResetHandler can be invoked from an interrupt thread (timer)
 *	or a non-interrupt thread.  In the former, must not call schedule().
 *
 *	Remark: A return of -1 is a FATAL error case, as it means a
 *	FW reload/initialization failed.
 *
 *	Returns 0 for SUCCESS or -1 if FAILED.
 */
int
mpt_HardResetHandler(MPT_ADAPTER *ioc, int sleepFlag)
{
	int		 rc;
	unsigned long	 flags;

	dtmprintk((MYIOC_s_INFO_FMT "HardResetHandler Entered!\n", ioc->name));
#ifdef MFCNT
	printk(MYIOC_s_INFO_FMT "HardResetHandler Entered!\n", ioc->name);
	printk("MF count 0x%x !\n", ioc->mfcnt);
#endif

	/* Reset the adapter. Prevent more than 1 call to
	 * mpt_do_ioc_recovery at any instant in time.
	 */
	spin_lock_irqsave(&ioc->diagLock, flags);
	if ((ioc->diagPending) || (ioc->alt_ioc && ioc->alt_ioc->diagPending)){
		spin_unlock_irqrestore(&ioc->diagLock, flags);
		return 0;
	} else {
		ioc->diagPending = 1;
	}
	spin_unlock_irqrestore(&ioc->diagLock, flags);

	/* FIXME: If do_ioc_recovery fails, repeat....
	 */

	/* The SCSI driver needs to adjust timeouts on all current
	 * commands prior to the diagnostic reset being issued.
	 * Prevents timeouts occuring during a diagnostic reset...very bad.
	 * For all other protocol drivers, this is a no-op.
	 */
	{
		int	 ii;
		int	 r = 0;

		for (ii=MPT_MAX_PROTOCOL_DRIVERS-1; ii; ii--) {
			if (MptResetHandlers[ii]) {
				dtmprintk((MYIOC_s_INFO_FMT "Calling IOC reset_setup handler #%d\n",
						ioc->name, ii));
				r += (*(MptResetHandlers[ii]))(ioc, MPT_IOC_SETUP_RESET);
				if (ioc->alt_ioc) {
					dtmprintk((MYIOC_s_INFO_FMT "Calling alt-%s setup reset handler #%d\n",
							ioc->name, ioc->alt_ioc->name, ii));
					r += (*(MptResetHandlers[ii]))(ioc->alt_ioc, MPT_IOC_SETUP_RESET);
				}
			}
		}
	}

	if ((rc = mpt_do_ioc_recovery(ioc, MPT_HOSTEVENT_IOC_RECOVER, sleepFlag)) != 0) {
		printk(KERN_WARNING MYNAM ": WARNING - (%d) Cannot recover %s\n",
			rc, ioc->name);
	}
	ioc->reload_fw = 0;
	if (ioc->alt_ioc)
		ioc->alt_ioc->reload_fw = 0;

	spin_lock_irqsave(&ioc->diagLock, flags);
	ioc->diagPending = 0;
	if (ioc->alt_ioc)
		ioc->alt_ioc->diagPending = 0;
	spin_unlock_irqrestore(&ioc->diagLock, flags);

	dtmprintk((MYIOC_s_INFO_FMT "HardResetHandler rc = %d!\n", ioc->name, rc));

	return rc;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
static char *
EventDescriptionStr(u8 event, u32 evData0)
{
	char *ds;

	switch(event) {
	case MPI_EVENT_NONE:
		ds = "None";
		break;
	case MPI_EVENT_LOG_DATA:
		ds = "Log Data";
		break;
	case MPI_EVENT_STATE_CHANGE:
		ds = "State Change";
		break;
	case MPI_EVENT_UNIT_ATTENTION:
		ds = "Unit Attention";
		break;
	case MPI_EVENT_IOC_BUS_RESET:
		ds = "IOC Bus Reset";
		break;
	case MPI_EVENT_EXT_BUS_RESET:
		ds = "External Bus Reset";
		break;
	case MPI_EVENT_RESCAN:
		ds = "Bus Rescan Event";
		/* Ok, do we need to do anything here? As far as
		   I can tell, this is when a new device gets added
		   to the loop. */
		break;
	case MPI_EVENT_LINK_STATUS_CHANGE:
		if (evData0 == MPI_EVENT_LINK_STATUS_FAILURE)
			ds = "Link Status(FAILURE) Change";
		else
			ds = "Link Status(ACTIVE) Change";
		break;
	case MPI_EVENT_LOOP_STATE_CHANGE:
		if (evData0 == MPI_EVENT_LOOP_STATE_CHANGE_LIP)
			ds = "Loop State(LIP) Change";
		else if (evData0 == MPI_EVENT_LOOP_STATE_CHANGE_LPE)
			ds = "Loop State(LPE) Change";			/* ??? */
		else
			ds = "Loop State(LPB) Change";			/* ??? */
		break;
	case MPI_EVENT_LOGOUT:
		ds = "Logout";
		break;
	case MPI_EVENT_EVENT_CHANGE:
		if (evData0)
			ds = "Events(ON) Change";
		else
			ds = "Events(OFF) Change";
		break;
	case MPI_EVENT_INTEGRATED_RAID:
		ds = "Integrated Raid";
		break;
	/*
	 *  MPT base "custom" events may be added here...
	 */
	default:
		ds = "Unknown";
		break;
	}
	return ds;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	ProcessEventNotification - Route a received EventNotificationReply to
 *	all currently regeistered event handlers.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@pEventReply: Pointer to EventNotification reply frame
 *	@evHandlers: Pointer to integer, number of event handlers
 *
 *	Returns sum of event handlers return values.
 */
static int
ProcessEventNotification(MPT_ADAPTER *ioc, EventNotificationReply_t *pEventReply, int *evHandlers)
{
	u16 evDataLen;
	u32 evData0 = 0;
//	u32 evCtx;
	int ii;
	int r = 0;
	int handlers = 0;
	char *evStr;
	u8 event;

	/*
	 *  Do platform normalization of values
	 */
	event = le32_to_cpu(pEventReply->Event) & 0xFF;
//	evCtx = le32_to_cpu(pEventReply->EventContext);
	evDataLen = le16_to_cpu(pEventReply->EventDataLength);
	if (evDataLen) {
		evData0 = le32_to_cpu(pEventReply->Data[0]);
	}

	evStr = EventDescriptionStr(event, evData0);
	dprintk((MYIOC_s_INFO_FMT "MPT event (%s=%02Xh) detected!\n",
			ioc->name,
			evStr,
			event));

#if defined(MPT_DEBUG) || defined(MPT_DEBUG_EVENTS)
	printk(KERN_INFO MYNAM ": Event data:\n" KERN_INFO);
	for (ii = 0; ii < evDataLen; ii++)
		printk(" %08x", le32_to_cpu(pEventReply->Data[ii]));
	printk("\n");
#endif

	/*
	 *  Do general / base driver event processing
	 */
	switch(event) {
	case MPI_EVENT_NONE:			/* 00 */
	case MPI_EVENT_LOG_DATA:		/* 01 */
	case MPI_EVENT_STATE_CHANGE:		/* 02 */
	case MPI_EVENT_UNIT_ATTENTION:		/* 03 */
	case MPI_EVENT_IOC_BUS_RESET:		/* 04 */
	case MPI_EVENT_EXT_BUS_RESET:		/* 05 */
	case MPI_EVENT_RESCAN:			/* 06 */
	case MPI_EVENT_LINK_STATUS_CHANGE:	/* 07 */
	case MPI_EVENT_LOOP_STATE_CHANGE:	/* 08 */
	case MPI_EVENT_LOGOUT:			/* 09 */
	case MPI_EVENT_INTEGRATED_RAID:		/* 0B */
	case MPI_EVENT_SCSI_DEVICE_STATUS_CHANGE:	/* 0C */
	default:
		break;
	case MPI_EVENT_EVENT_CHANGE:		/* 0A */
		if (evDataLen) {
			u8 evState = evData0 & 0xFF;

			/* CHECKME! What if evState unexpectedly says OFF (0)? */

			/* Update EventState field in cached IocFacts */
			if (ioc->facts.Function) {
				ioc->facts.EventState = evState;
			}
		}
		break;
	}

	/*
	 * Should this event be logged? Events are written sequentially.
	 * When buffer is full, start again at the top.
	 */
	if (ioc->events && (ioc->eventTypes & ( 1 << event))) {
		int idx;

		idx = ioc->eventContext % ioc->eventLogSize;

		ioc->events[idx].event = event;
		ioc->events[idx].eventContext = ioc->eventContext;

		for (ii = 0; ii < 2; ii++) {
			if (ii < evDataLen)
				ioc->events[idx].data[ii] = le32_to_cpu(pEventReply->Data[ii]);
			else
				ioc->events[idx].data[ii] =  0;
		}

		ioc->eventContext++;
	}


	/*
	 *  Call each currently registered protocol event handler.
	 */
	for (ii=MPT_MAX_PROTOCOL_DRIVERS-1; ii; ii--) {
		if (MptEvHandlers[ii]) {
			dprintk((MYIOC_s_INFO_FMT "Routing Event to event handler #%d\n",
					ioc->name, ii));
			r += (*(MptEvHandlers[ii]))(ioc, pEventReply);
			handlers++;
		}
	}
	/* FIXME?  Examine results here? */

	/*
	 *  If needed, send (a single) EventAck.
	 */
	if (pEventReply->AckRequired == MPI_EVENT_NOTIFICATION_ACK_REQUIRED) {
		if ((ii = SendEventAck(ioc, pEventReply)) != 0) {
			printk(MYIOC_s_WARN_FMT "SendEventAck returned %d\n",
					ioc->name, ii);
		}
	}

	*evHandlers = handlers;
	return r;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mpt_fc_log_info - Log information returned from Fibre Channel IOC.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@log_info: U32 LogInfo reply word from the IOC
 *
 *	Refer to lsi/fc_log.h.
 */
static void
mpt_fc_log_info(MPT_ADAPTER *ioc, u32 log_info)
{
	static char *subcl_str[8] = {
		"FCP Initiator", "FCP Target", "LAN", "MPI Message Layer",
		"FC Link", "Context Manager", "Invalid Field Offset", "State Change Info"
	};
	u8 subcl = (log_info >> 24) & 0x7;
//	u32 SubCl = log_info & 0x27000000;

	printk(MYIOC_s_INFO_FMT "LogInfo(0x%08x): SubCl={%s}",
			ioc->name, log_info, subcl_str[subcl]);
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mpt_sp_log_info - Log information returned from SCSI Parallel IOC.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@mr: Pointer to MPT reply frame
 *	@log_info: U32 LogInfo word from the IOC
 *
 *	Refer to lsi/sp_log.h.
 */
static void
mpt_sp_log_info(MPT_ADAPTER *ioc, u32 log_info)
{
	u32 info = log_info & 0x00FF0000;
	char *desc = "unknown";

	switch (info) {
	case 0x00010000:
		desc = "bug! MID not found";
		if (ioc->reload_fw == 0)
			ioc->reload_fw++;
		break;

	case 0x00020000:
		desc = "Parity Error";
		break;

	case 0x00030000:
		desc = "ASYNC Outbound Overrun";
		break;

	case 0x00040000:
		desc = "SYNC Offset Error";
		break;

	case 0x00050000:
		desc = "BM Change";
		break;

	case 0x00060000:
		desc = "Msg In Overflow";
		break;

	case 0x00070000:
		desc = "DMA Error";
		break;

	case 0x00080000:
		desc = "Outbound DMA Overrun";
		break;
	
	case 0x00090000:
		desc = "Task Management";
		break;

	case 0x000A0000:
		desc = "Device Problem";
		break;

	case 0x000B0000:
		desc = "Invalid Phase Change";
		break;

	case 0x000C0000:
		desc = "Untagged Table Size";
		break;
	
	}

	printk(MYIOC_s_INFO_FMT "LogInfo(0x%08x): F/W: %s\n", ioc->name, log_info, desc);
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	mpt_sp_ioc_info - IOC information returned from SCSI Parallel IOC.
 *	@ioc: Pointer to MPT_ADAPTER structure
 *	@ioc_status: U32 IOCStatus word from IOC
 *	@mf: Pointer to MPT request frame
 *
 *	Refer to lsi/mpi.h.
 */
static void
mpt_sp_ioc_info(MPT_ADAPTER *ioc, u32 ioc_status, MPT_FRAME_HDR *mf)
{
	u32 status = ioc_status & MPI_IOCSTATUS_MASK;
	char *desc = "";

	switch (status) {
	case MPI_IOCSTATUS_INVALID_FUNCTION: /* 0x0001 */
		desc = "Invalid Function";
		break;

	case MPI_IOCSTATUS_BUSY: /* 0x0002 */
		desc = "Busy";
		break;

	case MPI_IOCSTATUS_INVALID_SGL: /* 0x0003 */
		desc = "Invalid SGL";
		break;

	case MPI_IOCSTATUS_INTERNAL_ERROR: /* 0x0004 */
		desc = "Internal Error";
		break;

	case MPI_IOCSTATUS_RESERVED: /* 0x0005 */
		desc = "Reserved";
		break;

	case MPI_IOCSTATUS_INSUFFICIENT_RESOURCES: /* 0x0006 */
		desc = "Insufficient Resources";
		break;

	case MPI_IOCSTATUS_INVALID_FIELD: /* 0x0007 */
		desc = "Invalid Field";
		break;

	case MPI_IOCSTATUS_INVALID_STATE: /* 0x0008 */
		desc = "Invalid State";
		break;

	case MPI_IOCSTATUS_CONFIG_INVALID_ACTION: /* 0x0020 */
	case MPI_IOCSTATUS_CONFIG_INVALID_TYPE:   /* 0x0021 */
	case MPI_IOCSTATUS_CONFIG_INVALID_PAGE:   /* 0x0022 */
	case MPI_IOCSTATUS_CONFIG_INVALID_DATA:   /* 0x0023 */
	case MPI_IOCSTATUS_CONFIG_NO_DEFAULTS:    /* 0x0024 */
	case MPI_IOCSTATUS_CONFIG_CANT_COMMIT:    /* 0x0025 */
		/* No message for Config IOCStatus values */
		break;

	case MPI_IOCSTATUS_SCSI_RECOVERED_ERROR: /* 0x0040 */
		/* No message for recovered error
		desc = "SCSI Recovered Error";
		*/
		break;

	case MPI_IOCSTATUS_SCSI_INVALID_BUS: /* 0x0041 */
		desc = "SCSI Invalid Bus";
		break;

	case MPI_IOCSTATUS_SCSI_INVALID_TARGETID: /* 0x0042 */
		desc = "SCSI Invalid TargetID";
		break;

	case MPI_IOCSTATUS_SCSI_DEVICE_NOT_THERE: /* 0x0043 */
	  {
		SCSIIORequest_t *pScsiReq = (SCSIIORequest_t *) mf;
		U8 cdb = pScsiReq->CDB[0];
		if (cdb != 0x12) { /* Inquiry is issued for device scanning */
			desc = "SCSI Device Not There";
		}
		break;
	  }

	case MPI_IOCSTATUS_SCSI_DATA_OVERRUN: /* 0x0044 */
		desc = "SCSI Data Overrun";
		break;

	case MPI_IOCSTATUS_SCSI_DATA_UNDERRUN: /* 0x0045 */
		/* This error is checked in scsi_io_done(). Skip. 
		desc = "SCSI Data Underrun";
		*/
		break;

	case MPI_IOCSTATUS_SCSI_IO_DATA_ERROR: /* 0x0046 */
		desc = "SCSI I/O Data Error";
		break;

	case MPI_IOCSTATUS_SCSI_PROTOCOL_ERROR: /* 0x0047 */
		desc = "SCSI Protocol Error";
		break;

	case MPI_IOCSTATUS_SCSI_TASK_TERMINATED: /* 0x0048 */
		desc = "SCSI Task Terminated";
		break;

	case MPI_IOCSTATUS_SCSI_RESIDUAL_MISMATCH: /* 0x0049 */
		desc = "SCSI Residual Mismatch";
		break;

	case MPI_IOCSTATUS_SCSI_TASK_MGMT_FAILED: /* 0x004A */
		desc = "SCSI Task Management Failed";
		break;

	case MPI_IOCSTATUS_SCSI_IOC_TERMINATED: /* 0x004B */
		desc = "SCSI IOC Terminated";
		break;

	case MPI_IOCSTATUS_SCSI_EXT_TERMINATED: /* 0x004C */
		desc = "SCSI Ext Terminated";
		break;

	default:
		desc = "Others";
		break;
	}
	if (desc != "")
		printk(MYIOC_s_INFO_FMT "IOCStatus(0x%04x): %s\n", ioc->name, status, desc);
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_register_ascqops_strings - Register SCSI ASC/ASCQ and SCSI
 *	OpCode strings from the (optional) isense module.
 *	@ascqTable: Pointer to ASCQ_Table_t structure
 *	@ascqtbl_sz: Number of entries in ASCQ_Table
 *	@opsTable: Pointer to array of SCSI OpCode strings (char pointers)
 *
 *	Specialized driver registration routine for the isense driver.
 */
int
mpt_register_ascqops_strings(void *ascqTable, int ascqtbl_sz, const char **opsTable)
{
	int r = 0;

	if (ascqTable && ascqtbl_sz && opsTable) {
		mpt_v_ASCQ_TablePtr = ascqTable;
		mpt_ASCQ_TableSz = ascqtbl_sz;
		mpt_ScsiOpcodesPtr = opsTable;
		printk(KERN_INFO MYNAM ": English readable SCSI-3 strings enabled:-)\n");
		isense_idx = last_drv_idx;
		r = 1;
	}
	return r;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/**
 *	mpt_deregister_ascqops_strings - Deregister SCSI ASC/ASCQ and SCSI
 *	OpCode strings from the isense driver.
 *
 *	Specialized driver deregistration routine for the isense driver.
 */
void
mpt_deregister_ascqops_strings(void)
{
	mpt_v_ASCQ_TablePtr = NULL;
	mpt_ASCQ_TableSz = 0;
	mpt_ScsiOpcodesPtr = NULL;
	printk(KERN_INFO MYNAM ": English readable SCSI-3 strings disabled)-:\n");
	isense_idx = -1;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/

EXPORT_SYMBOL(ioc_list);
EXPORT_SYMBOL(mpt_proc_root_dir);
EXPORT_SYMBOL(DmpService);
EXPORT_SYMBOL(mpt_register);
EXPORT_SYMBOL(mpt_deregister);
EXPORT_SYMBOL(mpt_event_register);
EXPORT_SYMBOL(mpt_event_deregister);
EXPORT_SYMBOL(mpt_reset_register);
EXPORT_SYMBOL(mpt_reset_deregister);
EXPORT_SYMBOL(mpt_device_driver_register);
EXPORT_SYMBOL(mpt_device_driver_deregister);
EXPORT_SYMBOL(mpt_get_msg_frame);
EXPORT_SYMBOL(mpt_put_msg_frame);
EXPORT_SYMBOL(mpt_free_msg_frame);
EXPORT_SYMBOL(mpt_add_sge);
EXPORT_SYMBOL(mpt_add_chain);
EXPORT_SYMBOL(mpt_send_handshake_request);
EXPORT_SYMBOL(mpt_handshake_req_reply_wait);
EXPORT_SYMBOL(mpt_verify_adapter);
EXPORT_SYMBOL(mpt_GetIocState);
EXPORT_SYMBOL(mpt_print_ioc_summary);
EXPORT_SYMBOL(mpt_lan_index);
EXPORT_SYMBOL(mpt_stm_index);
EXPORT_SYMBOL(mpt_HardResetHandler);
EXPORT_SYMBOL(mpt_config);
EXPORT_SYMBOL(mpt_toolbox);
EXPORT_SYMBOL(mpt_findImVolumes);
EXPORT_SYMBOL(mpt_read_ioc_pg_3);
EXPORT_SYMBOL(mpt_alloc_fw_memory);
EXPORT_SYMBOL(mpt_free_fw_memory);

EXPORT_SYMBOL(mpt_register_ascqops_strings);
EXPORT_SYMBOL(mpt_deregister_ascqops_strings);
EXPORT_SYMBOL(mpt_v_ASCQ_TablePtr);
EXPORT_SYMBOL(mpt_ASCQ_TableSz);
EXPORT_SYMBOL(mpt_ScsiOpcodesPtr);


static struct pci_driver mptbase_driver = {
	.name		= "mptbase",
	.id_table	= mptbase_pci_table,
	.probe		= mptbase_probe,
	.remove		= __devexit_p(mptbase_remove),
	.driver         = {
		.shutdown = mptbase_shutdown,
        },
#ifdef CONFIG_PM
	.suspend	= mptbase_suspend,
	.resume		= mptbase_resume,
#endif
};

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	fusion_init - Fusion MPT base driver initialization routine.
 *
 *	Returns 0 for success, non-zero for failure.
 */
static int __init
fusion_init(void)
{
	int i;
	int r;

	if (FusionInitCalled++) {
		dprintk((KERN_INFO MYNAM ": INFO - Driver late-init entry point called\n"));
		return 0;
	}

	show_mptmod_ver(my_NAME, my_VERSION);
	printk(KERN_INFO COPYRIGHT "\n");

	for (i = 0; i < MPT_MAX_PROTOCOL_DRIVERS; i++) {
		MptCallbacks[i] = NULL;
		MptDriverClass[i] = MPTUNKNOWN_DRIVER;
		MptEvHandlers[i] = NULL;
		MptResetHandlers[i] = NULL;
	}

	DmpService = NULL;

	/* NEW!  20010120 -sralston
	 *  Register ourselves (mptbase) in order to facilitate
	 *  EventNotification handling.
	 */
	mpt_base_index = mpt_register(mpt_base_reply, MPTBASE_DRIVER);

	/* Register for hard reset handling callbacks.
	 */
	if (mpt_reset_register(mpt_base_index, mpt_ioc_reset) == 0) {
		dprintk((KERN_INFO MYNAM ": Register for IOC reset notification\n"));
	} else {
		/* FIXME! */
	}

#ifdef CONFIG_PROC_FS
	(void) procmpt_create();
#endif
	r = pci_module_init(&mptbase_driver);
	if(r)
		return(r);

	return r;
}

/*=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=*/
/*
 *	fusion_exit - Perform driver unload cleanup.
 *
 *	This routine frees all resources associated with each MPT adapter
 *	and removes all %MPT_PROCFS_MPTBASEDIR entries.
 */
static void __exit
fusion_exit(void)
{

	dexitprintk((KERN_INFO MYNAM ": fusion_exit() called!\n"));

	pci_unregister_driver(&mptbase_driver);
	mpt_reset_deregister(mpt_base_index);

#ifdef CONFIG_PROC_FS
	procmpt_destroy();
#endif
}


module_init(fusion_init);
module_exit(fusion_exit);
