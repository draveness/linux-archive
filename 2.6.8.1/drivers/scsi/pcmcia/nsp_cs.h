/*=======================================================/
  Header file for nsp_cs.c
      By: YOKOTA Hiroshi <yokota@netlab.is.tsukuba.ac.jp>

    Ver.1.0 : Cut unused lines.
    Ver 0.1 : Initial version.

    This software may be used and distributed according to the terms of
    the GNU General Public License.

=========================================================*/

/* $Id: nsp_cs.h,v 1.19 2003/08/18 11:09:19 elca Exp $ */

#ifndef  __nsp_cs__
#define  __nsp_cs__

/* for debugging */
//#define NSP_DEBUG 9

/*
#define static
#define inline
*/

/************************************
 * Some useful macros...
 */
#define BIT(x)      (1L << (x))

/* SCSI initiator must be ID 7 */
#define NSP_INITIATOR_ID  7

#define NSP_SELTIMEOUT 200

/***************************************************************************
 * register definitions
 ***************************************************************************/
/*========================================================================
 * base register
 ========================================================================*/
#define	IRQCONTROL	0x00  /* R */
#  define IRQCONTROL_RESELECT_CLEAR     BIT(0)
#  define IRQCONTROL_PHASE_CHANGE_CLEAR BIT(1)
#  define IRQCONTROL_TIMER_CLEAR        BIT(2)
#  define IRQCONTROL_FIFO_CLEAR         BIT(3)
#  define IRQCONTROL_ALLMASK            0xff
#  define IRQCONTROL_ALLCLEAR           (IRQCONTROL_RESELECT_CLEAR     | \
					 IRQCONTROL_PHASE_CHANGE_CLEAR | \
					 IRQCONTROL_TIMER_CLEAR        | \
					 IRQCONTROL_FIFO_CLEAR          )
#  define IRQCONTROL_IRQDISABLE         0xf0

#define	IRQSTATUS	0x00  /* W */
#  define IRQSTATUS_SCSI  BIT(0)
#  define IRQSTATUS_TIMER BIT(2)
#  define IRQSTATUS_FIFO  BIT(3)
#  define IRQSTATUS_MASK  0x0f

#define	IFSELECT	0x01 /* W */
#  define IF_IFSEL    BIT(0)
#  define IF_REGSEL   BIT(2)

#define	FIFOSTATUS	0x01 /* R */
#  define FIFOSTATUS_CHIP_REVISION_MASK 0x0f
#  define FIFOSTATUS_CHIP_ID_MASK       0x70
#  define FIFOSTATUS_FULL_EMPTY         BIT(7)

#define	INDEXREG	0x02 /* R/W */
#define	DATAREG		0x03 /* R/W */
#define	FIFODATA	0x04 /* R/W */
#define	FIFODATA1	0x05 /* R/W */
#define	FIFODATA2	0x06 /* R/W */
#define	FIFODATA3	0x07 /* R/W */

/*====================================================================
 * indexed register
 ====================================================================*/
#define EXTBUSCTRL	0x10 /* R/W,deleted */

#define CLOCKDIV	0x11 /* R/W */
#  define CLOCK_40M 0x02
#  define CLOCK_20M 0x01
#  define FAST_20   BIT(2)

#define TERMPWRCTRL	0x13 /* R/W */
#  define POWER_ON BIT(0)

#define SCSIIRQMODE	0x15 /* R/W */
#  define SCSI_PHASE_CHANGE_EI BIT(0)
#  define RESELECT_EI          BIT(4)
#  define FIFO_IRQ_EI          BIT(5)
#  define SCSI_RESET_IRQ_EI    BIT(6)

#define IRQPHASESENCE	0x16 /* R */
#  define LATCHED_MSG      BIT(0)
#  define LATCHED_IO       BIT(1)
#  define LATCHED_CD       BIT(2)
#  define LATCHED_BUS_FREE BIT(3)
#  define PHASE_CHANGE_IRQ BIT(4)
#  define RESELECT_IRQ     BIT(5)
#  define FIFO_IRQ         BIT(6)
#  define SCSI_RESET_IRQ   BIT(7)

#define TIMERCOUNT	0x17 /* R/W */

#define SCSIBUSCTRL	0x18 /* R/W */
#  define SCSI_SEL         BIT(0)
#  define SCSI_RST         BIT(1)
#  define SCSI_DATAOUT_ENB BIT(2)
#  define SCSI_ATN         BIT(3)
#  define SCSI_ACK         BIT(4)
#  define SCSI_BSY         BIT(5)
#  define AUTODIRECTION    BIT(6)
#  define ACKENB           BIT(7)

#define SCSIBUSMON	0x19 /* R */

#define SETARBIT	0x1A /* W */
#  define ARBIT_GO         BIT(0)
#  define ARBIT_FLAG_CLEAR BIT(1)

#define ARBITSTATUS	0x1A /* R */
/*#  define ARBIT_GO        BIT(0)*/
#  define ARBIT_WIN        BIT(1)
#  define ARBIT_FAIL       BIT(2)
#  define RESELECT_FLAG    BIT(3)

#define PARITYCTRL	0x1B  /* W */
#define PARITYSTATUS	0x1B  /* R */

#define COMMANDCTRL	0x1C  /* W */
#  define CLEAR_COMMAND_POINTER BIT(0)
#  define AUTO_COMMAND_GO       BIT(1)

#define RESELECTID	0x1C  /* R   */
#define COMMANDDATA	0x1D  /* R/W */

#define POINTERCLR	0x1E  /*   W */
#  define POINTER_CLEAR      BIT(0)
#  define ACK_COUNTER_CLEAR  BIT(1)
#  define REQ_COUNTER_CLEAR  BIT(2)
#  define HOST_COUNTER_CLEAR BIT(3)
#  define READ_SOURCE        (BIT(4) | BIT(5))
#    define ACK_COUNTER        (0)
#    define REQ_COUNTER        (BIT(4))
#    define HOST_COUNTER       (BIT(5))

#define TRANSFERCOUNT	0x1E  /* R   */

#define TRANSFERMODE	0x20  /* R/W */
#  define MODE_MEM8   BIT(0)
#  define MODE_MEM32  BIT(1)
#  define MODE_ADR24  BIT(2)
#  define MODE_ADR32  BIT(3)
#  define MODE_IO8    BIT(4)
#  define MODE_IO32   BIT(5)
#  define TRANSFER_GO BIT(6)
#  define BRAIND      BIT(7)

#define SYNCREG		0x21 /* R/W */
#  define SYNCREG_OFFSET_MASK  0x0f
#  define SYNCREG_PERIOD_MASK  0xf0
#  define SYNCREG_PERIOD_SHIFT 4

#define SCSIDATALATCH	0x22 /*   W */
#define SCSIDATAIN	0x22 /* R   */
#define SCSIDATAWITHACK	0x23 /* R/W */
#define SCAMCONTROL	0x24 /*   W */
#define SCAMSTATUS	0x24 /* R   */
#define SCAMDATA	0x25 /* R/W */

#define OTHERCONTROL	0x26 /* R/W */
#  define TPL_ROM_WRITE_EN BIT(0)
#  define TPWR_OUT         BIT(1)
#  define TPWR_SENSE       BIT(2)
#  define RA8_CONTROL      BIT(3)

#define ACKWIDTH	0x27 /* R/W */
#define CLRTESTPNT	0x28 /*   W */
#define ACKCNTLD	0x29 /*   W */
#define REQCNTLD	0x2A /*   W */
#define HSTCNTLD	0x2B /*   W */
#define CHECKSUM	0x2C /* R/W */

/************************************************************************
 * Input status bit definitions.
 ************************************************************************/
#define S_MESSAGE	BIT(0)    /* Message line from SCSI bus      */
#define S_IO		BIT(1)    /* Input/Output line from SCSI bus */
#define S_CD		BIT(2)    /* Command/Data line from SCSI bus */
#define S_BUSY		BIT(3)    /* Busy line from SCSI bus         */
#define S_ACK		BIT(4)    /* Acknowlege line from SCSI bus   */
#define S_REQUEST	BIT(5)    /* Request line from SCSI bus      */
#define S_SELECT	BIT(6)	  /*                                 */
#define S_ATN		BIT(7)	  /*                                 */

/***********************************************************************
 * Useful Bus Monitor status combinations.
 ***********************************************************************/
#define BUSMON_SEL         S_SELECT
#define BUSMON_BSY         S_BUSY
#define BUSMON_REQ         S_REQUEST
#define BUSMON_IO          S_IO
#define BUSMON_ACK         S_ACK
#define BUSMON_BUS_FREE    0
#define BUSMON_COMMAND     ( S_BUSY | S_CD |                    S_REQUEST )
#define BUSMON_MESSAGE_IN  ( S_BUSY | S_CD | S_IO | S_MESSAGE | S_REQUEST )
#define BUSMON_MESSAGE_OUT ( S_BUSY | S_CD |        S_MESSAGE | S_REQUEST )
#define BUSMON_DATA_IN     ( S_BUSY |        S_IO |             S_REQUEST )
#define BUSMON_DATA_OUT    ( S_BUSY |                           S_REQUEST )
#define BUSMON_STATUS      ( S_BUSY | S_CD | S_IO |             S_REQUEST )
#define BUSMON_SELECT      (                 S_IO |                        S_SELECT )
#define BUSMON_RESELECT    (                 S_IO |                        S_SELECT )
#define BUSMON_PHASE_MASK  (          S_CD | S_IO | S_MESSAGE |            S_SELECT )

#define BUSPHASE_SELECT      ( BUSMON_SELECT      & BUSMON_PHASE_MASK )
#define BUSPHASE_COMMAND     ( BUSMON_COMMAND     & BUSMON_PHASE_MASK )
#define BUSPHASE_MESSAGE_IN  ( BUSMON_MESSAGE_IN  & BUSMON_PHASE_MASK )
#define BUSPHASE_MESSAGE_OUT ( BUSMON_MESSAGE_OUT & BUSMON_PHASE_MASK )
#define BUSPHASE_DATA_IN     ( BUSMON_DATA_IN     & BUSMON_PHASE_MASK )
#define BUSPHASE_DATA_OUT    ( BUSMON_DATA_OUT    & BUSMON_PHASE_MASK )
#define BUSPHASE_STATUS      ( BUSMON_STATUS      & BUSMON_PHASE_MASK )

/*====================================================================*/

typedef struct scsi_info_t {
	dev_link_t             link;
	struct Scsi_Host      *host;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,74))
	dev_node_t             node;
#else
	int	               ndev;
	dev_node_t             node[8];
	struct bus_operations *bus;
#endif
	int                    stop;
} scsi_info_t;


/* synchronous transfer negotiation data */
typedef struct _sync_data {
	unsigned int SyncNegotiation;
#define SYNC_NOT_YET 0
#define SYNC_OK      1
#define SYNC_NG      2

	unsigned int  SyncPeriod;
	unsigned int  SyncOffset;
	unsigned char SyncRegister;
	unsigned char AckWidth;
} sync_data;

typedef struct _nsp_hw_data {
	unsigned int  BaseAddress;
	unsigned int  NumAddress;
	unsigned int  IrqNumber;

	unsigned long MmioAddress;
#define NSP_MMIO_OFFSET 0x0800
	unsigned long MmioLength;

	unsigned char ScsiClockDiv;

	unsigned char TransferMode;

	int           TimerCount;
	int           SelectionTimeOut;
	Scsi_Cmnd    *CurrentSC;
	//int           CurrnetTarget;

	int           FifoCount;

#define MSGBUF_SIZE 20
	unsigned char MsgBuffer[MSGBUF_SIZE];
	int MsgLen;

#define N_TARGET 8
	sync_data     Sync[N_TARGET];

	char nspinfo[110];     /* description */
	spinlock_t Lock;

	scsi_info_t   *ScsiInfo; /* attach <-> detect glue */


#ifdef NSP_DEBUG
	int CmdId; /* Accepted command serial number.
		      Used for debugging.             */
#endif
} nsp_hw_data;


/****************************************************************************
 *
 */

/* Card service functions */
static dev_link_t *nsp_cs_attach (void);
static void        nsp_cs_detach (dev_link_t *link);
static void        nsp_cs_release(dev_link_t *link);
static void        nsp_cs_config (dev_link_t *link);
static int         nsp_cs_event  (event_t event, int priority, event_callback_args_t *args);

/* Linux SCSI subsystem specific functions */
static struct Scsi_Host *nsp_detect     (Scsi_Host_Template *sht);
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,5,0))
static        int        nsp_detect_old (Scsi_Host_Template *sht);
static        int        nsp_release_old(struct Scsi_Host *shpnt);
#endif
static const  char      *nsp_info       (struct Scsi_Host *shpnt);
static        int        nsp_proc_info  (
#if (LINUX_VERSION_CODE > KERNEL_VERSION(2,5,73))
	                                 struct Scsi_Host *host,
#endif
					 char   *buffer,
					 char  **start,
					 off_t   offset,
					 int     length,
#if !(LINUX_VERSION_CODE > KERNEL_VERSION(2,5,73))
					 int     hostno,
#endif
					 int     inout);
static        int        nsp_queuecommand(Scsi_Cmnd *SCpnt, void (* done)(Scsi_Cmnd *SCpnt));

/* Error handler */
/*static int nsp_eh_abort       (Scsi_Cmnd *SCpnt);*/
/*static int nsp_eh_device_reset(Scsi_Cmnd *SCpnt);*/
static int nsp_eh_bus_reset    (Scsi_Cmnd *SCpnt);
static int nsp_eh_host_reset   (Scsi_Cmnd *SCpnt);
static int nsp_bus_reset       (nsp_hw_data *data);

/* */
static int  nsphw_init           (nsp_hw_data *data);
static int  nsphw_start_selection(Scsi_Cmnd *SCpnt);
static void nsp_start_timer      (Scsi_Cmnd *SCpnt, int time);
static int  nsp_fifo_count       (Scsi_Cmnd *SCpnt);
static void nsp_pio_read         (Scsi_Cmnd *SCpnt);
static void nsp_pio_write        (Scsi_Cmnd *SCpnt);
static int  nsp_nexus            (Scsi_Cmnd *SCpnt);
static void nsp_scsi_done        (Scsi_Cmnd *SCpnt);
static int  nsp_analyze_sdtr     (Scsi_Cmnd *SCpnt);
static int  nsp_negate_signal    (Scsi_Cmnd *SCpnt, unsigned char mask, char *str);
static int  nsp_expect_signal    (Scsi_Cmnd *SCpnt, unsigned char current_phase, unsigned char  mask);
static int  nsp_xfer             (Scsi_Cmnd *SCpnt, int phase);
static int  nsp_dataphase_bypass (Scsi_Cmnd *SCpnt);
static int  nsp_reselected       (Scsi_Cmnd *SCpnt);
static struct Scsi_Host *nsp_detect(Scsi_Host_Template *sht);

/* Interrupt handler */
//static irqreturn_t nspintr(int irq, void *dev_id, struct pt_regs *regs);

/* Module entry point*/
static int  __init nsp_cs_init(void);
static void __exit nsp_cs_exit(void);


/* Debug */
#ifdef NSP_DEBUG
static void show_command (Scsi_Cmnd *SCpnt);
static void show_phase   (Scsi_Cmnd *SCpnt);
static void show_busphase(unsigned char stat);
static void show_message (nsp_hw_data *data);
#else
# define show_command(ptr)   /* */
# define show_phase(SCpnt)   /* */
# define show_busphase(stat) /* */
# define show_message(data)  /* */
#endif

/*
 * SCSI phase
 */
enum _scsi_phase {
	PH_UNDETERMINED ,
	PH_ARBSTART     ,
	PH_SELSTART     ,
	PH_SELECTED     ,
	PH_COMMAND      ,
	PH_DATA         ,
	PH_STATUS       ,
	PH_MSG_IN       ,
	PH_MSG_OUT      ,
	PH_DISCONNECT   ,
	PH_RESELECT     ,
	PH_ABORT        ,
	PH_RESET
};

enum _data_in_out {
	IO_UNKNOWN,
	IO_IN,
	IO_OUT
};

enum _burst_mode {
	BURST_IO8   = 0,
	BURST_IO32  = 1,
	BURST_MEM32 = 2,
};


/**************************************************************************
 * SCSI messaage
 */
#define MSG_COMMAND_COMPLETE 0x00
#define MSG_EXTENDED         0x01
#define MSG_ABORT            0x06
#define MSG_NO_OPERATION     0x08
#define MSG_BUS_DEVICE_RESET 0x0c

#define MSG_EXT_SDTR         0x01


/**************************************************************************
 * Compatibility functions
 */

/* for Kernel 2.4 */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0))
#  define scsi_register_host(template)   scsi_register_module(MODULE_SCSI_HA, template)
#  define scsi_unregister_host(template) scsi_unregister_module(MODULE_SCSI_HA, template)
#  define scsi_host_put(host)            scsi_unregister(host)

typedef void irqreturn_t;
#  define IRQ_NONE      /* */
#  define IRQ_HANDLED   /* */
#  define IRQ_RETVAL(x) /* */

/* This is ad-hoc version of scsi_host_get_next() */
static inline struct Scsi_Host *scsi_host_get_next(struct Scsi_Host *host)
{
	if (host == NULL) {
		return scsi_hostlist;
	} else {
		return host->next;
	}
}

/* This is ad-hoc version of scsi_host_hn_get() */
static inline struct Scsi_Host *scsi_host_hn_get(unsigned short hostno)
{
	struct Scsi_Host *host;

	for (host = scsi_host_get_next(NULL); host != NULL;
	     host = scsi_host_get_next(host)) {
		if (host->host_no == hostno) {
			break;
		}
	}

	return host;
}

static void cs_error(client_handle_t handle, int func, int ret)
{
	error_info_t err = { func, ret };
	pcmcia_report_error(handle, &err);
}

/* scatter-gather table */
#  define BUFFER_ADDR (SCpnt->SCp.buffer->address)
#endif

/* for Kernel 2.6 */
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,0))
/* scatter-gather table */
#  define BUFFER_ADDR ((char *)((unsigned int)(SCpnt->SCp.buffer->page) + SCpnt->SCp.buffer->offset))
#endif

#endif  /*__nsp_cs__*/
/* end */
