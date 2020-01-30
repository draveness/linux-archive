#if (!defined(dprintk))
# define dprintk(x)
#endif

/*------------------------------------------------------------------------------
 *              D E F I N E S
 *----------------------------------------------------------------------------*/

#define MAXIMUM_NUM_CONTAINERS	32
#define MAXIMUM_NUM_ADAPTERS	8

#define AAC_NUM_FIB		(256 + 64)
#define AAC_NUM_IO_FIB		100

#define AAC_MAX_LUN		(8)

#define AAC_MAX_HOSTPHYSMEMPAGES (0xfffff)

/*
 * These macros convert from physical channels to virtual channels
 */
#define CONTAINER_CHANNEL		(0)
#define ID_LUN_TO_CONTAINER(id, lun)	(id)
#define CONTAINER_TO_CHANNEL(cont)	(CONTAINER_CHANNEL)
#define CONTAINER_TO_ID(cont)		(cont)
#define CONTAINER_TO_LUN(cont)		(0)

#define aac_phys_to_logical(x)  (x+1)
#define aac_logical_to_phys(x)  (x?x-1:0)

#define AAC_DETAILED_STATUS_INFO

extern int nondasd;
extern int paemode;

struct diskparm
{
	int heads;
	int sectors;
	int cylinders;
};


/*
 *	DON'T CHANGE THE ORDER, this is set by the firmware
 */
 
#define		CT_NONE			0
#define		CT_VOLUME		1
#define		CT_MIRROR		2
#define		CT_STRIPE		3
#define		CT_RAID5		4
#define		CT_SSRW			5
#define		CT_SSRO			6
#define		CT_MORPH		7
#define		CT_PASSTHRU		8
#define		CT_RAID4		9
#define		CT_RAID10		10	/* stripe of mirror */
#define		CT_RAID00		11	/* stripe of stripe */
#define		CT_VOLUME_OF_MIRRORS	12	/* volume of mirror */
#define		CT_PSEUDO_RAID		13	/* really raid4 */
#define		CT_LAST_VOLUME_TYPE	14

/*
 *	Types of objects addressable in some fashion by the client.
 *	This is a superset of those objects handled just by the filesystem
 *	and includes "raw" objects that an administrator would use to
 *	configure containers and filesystems.
 */

#define		FT_REG		1	/* regular file */
#define		FT_DIR		2	/* directory */
#define		FT_BLK		3	/* "block" device - reserved */
#define		FT_CHR		4	/* "character special" device - reserved */
#define		FT_LNK		5	/* symbolic link */
#define		FT_SOCK		6	/* socket */
#define		FT_FIFO		7	/* fifo */
#define		FT_FILESYS	8	/* ADAPTEC's "FSA"(tm) filesystem */
#define		FT_DRIVE	9	/* physical disk - addressable in scsi by bus/id/lun */
#define		FT_SLICE	10	/* virtual disk - raw volume - slice */
#define		FT_PARTITION	11	/* FSA partition - carved out of a slice - building block for containers */
#define		FT_VOLUME	12	/* Container - Volume Set */
#define		FT_STRIPE	13	/* Container - Stripe Set */
#define		FT_MIRROR	14	/* Container - Mirror Set */
#define		FT_RAID5	15	/* Container - Raid 5 Set */
#define		FT_DATABASE	16	/* Storage object with "foreign" content manager */

/*
 *	Host side memory scatter gather list
 *	Used by the adapter for read, write, and readdirplus operations
 *	We have separate 32 and 64 bit version because even
 *	on 64 bit systems not all cards support the 64 bit version
 */
struct sgentry {
	u32	addr;	/* 32-bit address. */
	u32	count;	/* Length. */
};

struct sgentry64 {
	u32	addr[2];	/* 64-bit addr. 2 pieces for data alignment */
	u32	count;	/* Length. */
};

/*
 *	SGMAP
 *
 *	This is the SGMAP structure for all commands that use
 *	32-bit addressing.
 */

struct sgmap {
	u32		count;
	struct sgentry	sg[1]; 
};

struct sgmap64 {
	u32		count;
	struct sgentry64 sg[1];
};

struct creation_info
{
	u8 		buildnum;		/* e.g., 588 */
	u8 		usec;			/* e.g., 588 */
	u8	 	via;			/* e.g., 1 = FSU,
						 * 	 2 = API
						 */
	u8	 	year;		 	/* e.g., 1997 = 97 */
	u32		date;			/*
						 * unsigned 	Month		:4;	// 1 - 12
						 * unsigned 	Day		:6;	// 1 - 32
						 * unsigned 	Hour		:6;	// 0 - 23
						 * unsigned 	Minute		:6;	// 0 - 60
						 * unsigned 	Second		:6;	// 0 - 60
						 */
	u32		serial[2];			/* e.g., 0x1DEADB0BFAFAF001 */
};


/*
 *	Define all the constants needed for the communication interface
 */

/*
 *	Define how many queue entries each queue will have and the total
 *	number of entries for the entire communication interface. Also define
 *	how many queues we support.
 *
 *	This has to match the controller
 */

#define NUMBER_OF_COMM_QUEUES  8   // 4 command; 4 response
#define HOST_HIGH_CMD_ENTRIES  4
#define HOST_NORM_CMD_ENTRIES  8
#define ADAP_HIGH_CMD_ENTRIES  4
#define ADAP_NORM_CMD_ENTRIES  512
#define HOST_HIGH_RESP_ENTRIES 4
#define HOST_NORM_RESP_ENTRIES 512
#define ADAP_HIGH_RESP_ENTRIES 4
#define ADAP_NORM_RESP_ENTRIES 8

#define TOTAL_QUEUE_ENTRIES  \
    (HOST_NORM_CMD_ENTRIES + HOST_HIGH_CMD_ENTRIES + ADAP_NORM_CMD_ENTRIES + ADAP_HIGH_CMD_ENTRIES + \
	    HOST_NORM_RESP_ENTRIES + HOST_HIGH_RESP_ENTRIES + ADAP_NORM_RESP_ENTRIES + ADAP_HIGH_RESP_ENTRIES)


/*
 *	Set the queues on a 16 byte alignment
 */
 
#define QUEUE_ALIGNMENT		16

/*
 *	The queue headers define the Communication Region queues. These
 *	are physically contiguous and accessible by both the adapter and the
 *	host. Even though all queue headers are in the same contiguous block
 *	they will be represented as individual units in the data structures.
 */

struct aac_entry {
	u32 size;          /* Size in bytes of Fib which this QE points to */
	u32 addr; /* Receiver address of the FIB */
};

/*
 *	The adapter assumes the ProducerIndex and ConsumerIndex are grouped
 *	adjacently and in that order.
 */
 
struct aac_qhdr {
	u64 header_addr;		/* Address to hand the adapter to access to this queue head */
	u32 *producer;			/* The producer index for this queue (host address) */
	u32 *consumer;			/* The consumer index for this queue (host address) */
};

/*
 *	Define all the events which the adapter would like to notify
 *	the host of.
 */
 
#define		HostNormCmdQue		1	/* Change in host normal priority command queue */
#define		HostHighCmdQue		2	/* Change in host high priority command queue */
#define		HostNormRespQue		3	/* Change in host normal priority response queue */
#define		HostHighRespQue		4	/* Change in host high priority response queue */
#define		AdapNormRespNotFull	5
#define		AdapHighRespNotFull	6
#define		AdapNormCmdNotFull	7
#define		AdapHighCmdNotFull	8
#define		SynchCommandComplete	9
#define		AdapInternalError	0xfe    /* The adapter detected an internal error shutting down */

/*
 *	Define all the events the host wishes to notify the
 *	adapter of. The first four values much match the Qid the
 *	corresponding queue.
 */

#define		AdapNormCmdQue		2
#define		AdapHighCmdQue		3
#define		AdapNormRespQue		6
#define		AdapHighRespQue		7
#define		HostShutdown		8
#define		HostPowerFail		9
#define		FatalCommError		10
#define		HostNormRespNotFull	11
#define		HostHighRespNotFull	12
#define		HostNormCmdNotFull	13
#define		HostHighCmdNotFull	14
#define		FastIo			15
#define		AdapPrintfDone		16

/*
 *	Define all the queues that the adapter and host use to communicate
 *	Number them to match the physical queue layout.
 */

enum aac_queue_types {
        HostNormCmdQueue = 0,	/* Adapter to host normal priority command traffic */
        HostHighCmdQueue,	/* Adapter to host high priority command traffic */
        AdapNormCmdQueue,	/* Host to adapter normal priority command traffic */
        AdapHighCmdQueue,	/* Host to adapter high priority command traffic */
        HostNormRespQueue,	/* Adapter to host normal priority response traffic */
        HostHighRespQueue,	/* Adapter to host high priority response traffic */
        AdapNormRespQueue,	/* Host to adapter normal priority response traffic */
        AdapHighRespQueue	/* Host to adapter high priority response traffic */
};

/*
 *	Assign type values to the FSA communication data structures
 */

#define		FIB_MAGIC	0x0001

/*
 *	Define the priority levels the FSA communication routines support.
 */

#define		FsaNormal	1
#define		FsaHigh		2

/*
 * Define the FIB. The FIB is the where all the requested data and
 * command information are put to the application on the FSA adapter.
 */

struct aac_fibhdr {
	u32 XferState;			// Current transfer state for this CCB
	u16 Command;			// Routing information for the destination
	u8 StructType;			// Type FIB
	u8 Flags;			// Flags for FIB
	u16 Size;			// Size of this FIB in bytes
	u16 SenderSize;			// Size of the FIB in the sender (for response sizing)
	u32 SenderFibAddress;		// Host defined data in the FIB
	u32 ReceiverFibAddress;		// Logical address of this FIB for the adapter
	u32 SenderData;			// Place holder for the sender to store data
	union {
		struct {
		    u32 _ReceiverTimeStart; 	// Timestamp for receipt of fib
		    u32 _ReceiverTimeDone;	// Timestamp for completion of fib
		} _s;
	} _u;
};

#define FIB_DATA_SIZE_IN_BYTES (512 - sizeof(struct aac_fibhdr))


struct hw_fib {
	struct aac_fibhdr header;
	u8 data[FIB_DATA_SIZE_IN_BYTES];		// Command specific data
};

/*
 *	FIB commands
 */

#define 	TestCommandResponse		1
#define		TestAdapterCommand		2
/*
 *	Lowlevel and comm commands
 */
#define		LastTestCommand			100
#define		ReinitHostNormCommandQueue	101
#define		ReinitHostHighCommandQueue	102
#define		ReinitHostHighRespQueue		103
#define		ReinitHostNormRespQueue		104
#define		ReinitAdapNormCommandQueue	105
#define		ReinitAdapHighCommandQueue	107
#define		ReinitAdapHighRespQueue		108
#define		ReinitAdapNormRespQueue		109
#define		InterfaceShutdown		110
#define		DmaCommandFib			120
#define		StartProfile			121
#define		TermProfile			122
#define		SpeedTest			123
#define		TakeABreakPt			124
#define		RequestPerfData			125
#define		SetInterruptDefTimer		126
#define		SetInterruptDefCount		127
#define		GetInterruptDefStatus		128
#define		LastCommCommand			129
/*
 *	Filesystem commands
 */
#define		NuFileSystem			300
#define		UFS				301
#define		HostFileSystem			302
#define		LastFileSystemCommand		303
/*
 *	Container Commands
 */
#define		ContainerCommand		500
#define		ContainerCommand64		501
/*
 *	Cluster Commands
 */
#define		ClusterCommand	 		550
/*
 *	Scsi Port commands (scsi passthrough)
 */
#define		ScsiPortCommand			600
#define		ScsiPortCommand64		601
/*
 *	Misc house keeping and generic adapter initiated commands
 */
#define		AifRequest			700
#define		CheckRevision			701
#define		FsaHostShutdown			702
#define		RequestAdapterInfo		703
#define		IsAdapterPaused			704
#define		SendHostTime			705
#define		LastMiscCommand			706

//
// Commands that will target the failover level on the FSA adapter
//

enum fib_xfer_state {
	HostOwned 			= (1<<0),
	AdapterOwned 			= (1<<1),
	FibInitialized 			= (1<<2),
	FibEmpty 			= (1<<3),
	AllocatedFromPool 		= (1<<4),
	SentFromHost 			= (1<<5),
	SentFromAdapter 		= (1<<6),
	ResponseExpected 		= (1<<7),
	NoResponseExpected 		= (1<<8),
	AdapterProcessed 		= (1<<9),
	HostProcessed 			= (1<<10),
	HighPriority 			= (1<<11),
	NormalPriority 			= (1<<12),
	Async				= (1<<13),
	AsyncIo				= (1<<13),	// rpbfix: remove with new regime
	PageFileIo			= (1<<14),	// rpbfix: remove with new regime
	ShutdownRequest			= (1<<15),
	LazyWrite			= (1<<16),	// rpbfix: remove with new regime
	AdapterMicroFib			= (1<<17),
	BIOSFibPath			= (1<<18),
	FastResponseCapable		= (1<<19),
	ApiFib				= (1<<20)	// Its an API Fib.
};

/*
 *	The following defines needs to be updated any time there is an
 *	incompatible change made to the aac_init structure.
 */

#define ADAPTER_INIT_STRUCT_REVISION		3

struct aac_init
{
	u32	InitStructRevision;
	u32	MiniPortRevision;
	u32	fsrev;
	u32	CommHeaderAddress;
	u32	FastIoCommAreaAddress;
	u32	AdapterFibsPhysicalAddress;
	u32	AdapterFibsVirtualAddress;
	u32	AdapterFibsSize;
	u32	AdapterFibAlign;
	u32	printfbuf;
	u32	printfbufsiz;
	u32	HostPhysMemPages;		// number of 4k pages of host physical memory
	u32	HostElapsedSeconds;		// number of seconds since 1970.
};

enum aac_log_level {
	LOG_INIT			= 10,
	LOG_INFORMATIONAL		= 20,
	LOG_WARNING			= 30,
	LOG_LOW_ERROR			= 40,
	LOG_MEDIUM_ERROR		= 50,
	LOG_HIGH_ERROR			= 60,
	LOG_PANIC			= 70,
	LOG_DEBUG			= 80,
	LOG_WINDBG_PRINT		= 90
};

#define FSAFS_NTC_GET_ADAPTER_FIB_CONTEXT	0x030b
#define FSAFS_NTC_FIB_CONTEXT			0x030c

struct aac_dev;

struct adapter_ops
{
	void (*adapter_interrupt)(struct aac_dev *dev);
	void (*adapter_notify)(struct aac_dev *dev, u32 event);
	void (*adapter_enable_int)(struct aac_dev *dev, u32 event);
	void (*adapter_disable_int)(struct aac_dev *dev, u32 event);
	int  (*adapter_sync_cmd)(struct aac_dev *dev, u32 command, u32 p1, u32 *status);
	int  (*adapter_check_health)(struct aac_dev *dev);
};

/*
 *	Define which interrupt handler needs to be installed
 */

struct aac_driver_ident
{
	int 	(*init)(struct aac_dev *dev);
	char *	name;
	char *	vname;
	char *	model;
	u16	channels;
	int	quirks;
};
/*
 * Some adapter firmware needs communication memory 
 * below 2gig. This tells the init function to set the
 * dma mask such that fib memory will be allocated where the
 * adapter firmware can get to it.
 */
#define AAC_QUIRK_31BIT	1

/*
 *	The adapter interface specs all queues to be located in the same
 *	physically contigous block. The host structure that defines the
 *	commuication queues will assume they are each a separate physically
 *	contigous memory region that will support them all being one big
 *	contigous block. 
 *	There is a command and response queue for each level and direction of
 *	commuication. These regions are accessed by both the host and adapter.
 */
 
struct aac_queue {
	u64		 	logical;	/*address we give the adapter */
	struct aac_entry	*base;		/*system virtual address */
	struct aac_qhdr 	headers;       	/*producer,consumer q headers*/
	u32	 		entries;	/*Number of queue entries */
	wait_queue_head_t	qfull;		/*Event to wait on if q full */
	wait_queue_head_t	cmdready;	/*Cmd ready from the adapter */
                  /* This is only valid for adapter to host command queues. */ 
	spinlock_t	 	*lock;		/* Spinlock for this queue must take this lock before accessing the lock */
	spinlock_t		lockdata;	/* Actual lock (used only on one side of the lock) */
	unsigned long		SavedIrql;     	/* Previous IRQL when the spin lock is taken */
	u32			padding;	/* Padding - FIXME - can remove I believe */
	struct list_head 	cmdq;	   	/* A queue of FIBs which need to be prcessed by the FS thread. This is */
                                		/* only valid for command queues which receive entries from the adapter. */
	struct list_head	pendingq;	/* A queue of outstanding fib's to the adapter. */
	u32			numpending;	/* Number of entries on outstanding queue. */
	struct aac_dev *	dev;		/* Back pointer to adapter structure */
};

/*
 *	Message queues. The order here is important, see also the 
 *	queue type ordering
 */

struct aac_queue_block
{
	struct aac_queue queue[8];
};

/*
 *	SaP1 Message Unit Registers
 */
 
struct sa_drawbridge_CSR {
						//	 Offset |	Name
	u32	reserved[10];			//	00h-27h |   Reserved
	u8	LUT_Offset;			//	28h	|	Looup Table Offset
	u8	reserved1[3];			// 	29h-2bh	|	Reserved
	u32	LUT_Data;			//	2ch	|	Looup Table Data	
	u32	reserved2[26];			//	30h-97h	|	Reserved
	u16	PRICLEARIRQ;			//	98h	|	Primary Clear Irq
	u16	SECCLEARIRQ;			//	9ah	|	Secondary Clear Irq
	u16	PRISETIRQ;			//	9ch	|	Primary Set Irq
	u16	SECSETIRQ;			//	9eh	|	Secondary Set Irq
	u16	PRICLEARIRQMASK;		//	a0h	|	Primary Clear Irq Mask
	u16	SECCLEARIRQMASK;		//	a2h	|	Secondary Clear Irq Mask
	u16	PRISETIRQMASK;			//	a4h	|	Primary Set Irq Mask
	u16	SECSETIRQMASK;			//	a6h	|	Secondary Set Irq Mask
	u32	MAILBOX0;			//	a8h	|	Scratchpad 0
	u32	MAILBOX1;			//	ach	|	Scratchpad 1
	u32	MAILBOX2;			//	b0h	|	Scratchpad 2
	u32	MAILBOX3;			//	b4h	|	Scratchpad 3
	u32	MAILBOX4;			//	b8h	|	Scratchpad 4
	u32	MAILBOX5;			//	bch	|	Scratchpad 5
	u32	MAILBOX6;			//	c0h	|	Scratchpad 6
	u32	MAILBOX7;			//	c4h	|	Scratchpad 7

	u32	ROM_Setup_Data;			//	c8h | 	Rom Setup and Data
	u32	ROM_Control_Addr;		//	cch | 	Rom Control and Address

	u32	reserved3[12];			//	d0h-ffh	| 	reserved
	u32	LUT[64];			// 100h-1ffh|	Lookup Table Entries

	//
	//  TO DO
	//	need to add DMA, I2O, UART, etc registers form 80h to 364h
	//

};

#define Mailbox0	SaDbCSR.MAILBOX0
#define Mailbox1	SaDbCSR.MAILBOX1
#define Mailbox2	SaDbCSR.MAILBOX2
#define Mailbox3	SaDbCSR.MAILBOX3
#define Mailbox4	SaDbCSR.MAILBOX4
#define Mailbox5	SaDbCSR.MAILBOX5
#define Mailbox7	SaDbCSR.MAILBOX7
	
#define DoorbellReg_p SaDbCSR.PRISETIRQ
#define DoorbellReg_s SaDbCSR.SECSETIRQ
#define DoorbellClrReg_p SaDbCSR.PRICLEARIRQ


#define	DOORBELL_0	cpu_to_le16(0x0001)
#define DOORBELL_1	cpu_to_le16(0x0002)
#define DOORBELL_2	cpu_to_le16(0x0004)
#define DOORBELL_3	cpu_to_le16(0x0008)
#define DOORBELL_4	cpu_to_le16(0x0010)
#define DOORBELL_5	cpu_to_le16(0x0020)
#define DOORBELL_6	cpu_to_le16(0x0040)

	
#define PrintfReady	DOORBELL_5
#define PrintfDone	DOORBELL_5
	
struct sa_registers {
	struct sa_drawbridge_CSR	SaDbCSR;			/* 98h - c4h */
};
	

#define Sa_MINIPORT_REVISION			1

#define sa_readw(AEP, CSR)		readl(&((AEP)->regs.sa->CSR))
#define sa_readl(AEP,  CSR)		readl(&((AEP)->regs.sa->CSR))
#define sa_writew(AEP, CSR, value)	writew(value, &((AEP)->regs.sa->CSR))
#define sa_writel(AEP, CSR, value)	writel(value, &((AEP)->regs.sa->CSR))

/*
 *	Rx Message Unit Registers
 */

struct rx_mu_registers {
						//	 Local	|   PCI*	|	Name
						//			|		|
	u32	ARSR;				//	1300h	|	00h	|	APIC Register Select Register
	u32	reserved0;			//	1304h	|	04h	|	Reserved
	u32	AWR;				//	1308h	|	08h	|	APIC Window Register
	u32	reserved1;			//	130Ch	|	0Ch	|	Reserved
	u32	IMRx[2];			//	1310h	|	10h	|	Inbound Message Registers
	u32	OMRx[2];			//	1318h	|	18h	|	Outbound Message Registers
	u32	IDR;				//	1320h	|	20h	|	Inbound Doorbell Register
	u32	IISR;				//	1324h	|	24h	|	Inbound Interrupt Status Register
	u32	IIMR;				//	1328h	|	28h	|	Inbound Interrupt Mask Register
	u32	ODR;				//	132Ch	|	2Ch	|	Outbound Doorbell Register
	u32	OISR;				//	1330h	|	30h	|	Outbound Interrupt Status Register
	u32	OIMR;				//	1334h	|	34h	|	Outbound Interrupt Mask Register
						// * Must access through ATU Inbound Translation Window
};

struct rx_inbound {
	u32	Mailbox[8];
};

#define	InboundMailbox0		IndexRegs.Mailbox[0]
#define	InboundMailbox1		IndexRegs.Mailbox[1]
#define	InboundMailbox2		IndexRegs.Mailbox[2]
#define	InboundMailbox3		IndexRegs.Mailbox[3]
#define	InboundMailbox4		IndexRegs.Mailbox[4]
#define	InboundMailbox5		IndexRegs.Mailbox[5]
#define	InboundMailbox6		IndexRegs.Mailbox[6]
#define	InboundMailbox7		IndexRegs.Mailbox[7]

#define	INBOUNDDOORBELL_0	cpu_to_le32(0x00000001)
#define INBOUNDDOORBELL_1	cpu_to_le32(0x00000002)
#define INBOUNDDOORBELL_2	cpu_to_le32(0x00000004)
#define INBOUNDDOORBELL_3	cpu_to_le32(0x00000008)
#define INBOUNDDOORBELL_4	cpu_to_le32(0x00000010)
#define INBOUNDDOORBELL_5	cpu_to_le32(0x00000020)
#define INBOUNDDOORBELL_6	cpu_to_le32(0x00000040)

#define	OUTBOUNDDOORBELL_0	cpu_to_le32(0x00000001)
#define OUTBOUNDDOORBELL_1	cpu_to_le32(0x00000002)
#define OUTBOUNDDOORBELL_2	cpu_to_le32(0x00000004)
#define OUTBOUNDDOORBELL_3	cpu_to_le32(0x00000008)
#define OUTBOUNDDOORBELL_4	cpu_to_le32(0x00000010)

#define InboundDoorbellReg	MUnit.IDR
#define OutboundDoorbellReg	MUnit.ODR

struct rx_registers {
	struct rx_mu_registers		MUnit;		// 1300h - 1334h
	u32				reserved1[6];	// 1338h - 134ch
	struct rx_inbound		IndexRegs;
};

#define rx_readb(AEP, CSR)		readb(&((AEP)->regs.rx->CSR))
#define rx_readl(AEP, CSR)		readl(&((AEP)->regs.rx->CSR))
#define rx_writeb(AEP, CSR, value)	writeb(value, &((AEP)->regs.rx->CSR))
#define rx_writel(AEP, CSR, value)	writel(value, &((AEP)->regs.rx->CSR))

/*
 *	Rkt Message Unit Registers (same as Rx, except a larger reserve region)
 */

#define rkt_mu_registers rx_mu_registers
#define rkt_inbound rx_inbound

struct rkt_registers {
	struct rkt_mu_registers		MUnit;		 /* 1300h - 1334h */
	u32				reserved1[1010]; /* 1338h - 22fch */
	struct rkt_inbound		IndexRegs;	 /* 2300h - */
};

#define rkt_readb(AEP, CSR)		readb(&((AEP)->regs.rkt->CSR))
#define rkt_readl(AEP, CSR)		readl(&((AEP)->regs.rkt->CSR))
#define rkt_writeb(AEP, CSR, value)	writeb(value, &((AEP)->regs.rkt->CSR))
#define rkt_writel(AEP, CSR, value)	writel(value, &((AEP)->regs.rkt->CSR))

struct fib;

typedef void (*fib_callback)(void *ctxt, struct fib *fibctx);

struct aac_fib_context {
	s16	 		type;		// used for verification of structure	
	s16	 		size;
	u32			unique;		// unique value representing this context
	ulong			jiffies;	// used for cleanup - dmb changed to ulong
	struct list_head	next;		// used to link context's into a linked list
	struct semaphore 	wait_sem;	// this is used to wait for the next fib to arrive.
	int			wait;		// Set to true when thread is in WaitForSingleObject
	unsigned long		count;		// total number of FIBs on FibList
	struct list_head	fib_list;	// this holds fibs and their attachd hw_fibs
};

struct fsa_scsi_hba {
	u32		size[MAXIMUM_NUM_CONTAINERS];
	u32		type[MAXIMUM_NUM_CONTAINERS];
	u8		valid[MAXIMUM_NUM_CONTAINERS];
	u8		ro[MAXIMUM_NUM_CONTAINERS];
	u8		locked[MAXIMUM_NUM_CONTAINERS];
	u8		deleted[MAXIMUM_NUM_CONTAINERS];
	char		devname[MAXIMUM_NUM_CONTAINERS][8];
};

struct fib {
	void			*next;	/* this is used by the allocator */
	s16			type;
	s16			size;
	/*
	 *	The Adapter that this I/O is destined for.
	 */
	struct aac_dev 		*dev;
	/*
	 *	This is the event the sendfib routine will wait on if the
	 *	caller did not pass one and this is synch io.
	 */
	struct semaphore 	event_wait;
	spinlock_t		event_lock;

	u32			done;	/* gets set to 1 when fib is complete */
	fib_callback 		callback;
	void 			*callback_data;
	u32			flags; // u32 dmb was ulong
	/*
	 *	The following is used to put this fib context onto the 
	 *	Outstanding I/O queue.
	 */
	struct list_head	queue;
	/*
	 *	And for the internal issue/reply queues (we may be able
	 *	to merge these two)
	 */
	struct list_head	fiblink;
	void 			*data;
	struct hw_fib		*hw_fib;		/* Actual shared object */
	dma_addr_t		hw_fib_pa;		/* physical address of hw_fib*/
};

/*
 *	Adapter Information Block
 *
 *	This is returned by the RequestAdapterInfo block
 */
 
struct aac_adapter_info
{
	u32	platform;
	u32	cpu;
	u32	subcpu;
	u32	clock;
	u32	execmem;
	u32	buffermem;
	u32	totalmem;
	u32	kernelrev;
	u32	kernelbuild;
	u32	monitorrev;
	u32	monitorbuild;
	u32	hwrev;
	u32	hwbuild;
	u32	biosrev;
	u32	biosbuild;
	u32	cluster;
	u32	clusterchannelmask; 
	u32	serial[2];
	u32	battery;
	u32	options;
	u32	OEM;
};

/*
 * Battery platforms
 */
#define AAC_BAT_REQ_PRESENT	(1)
#define AAC_BAT_REQ_NOTPRESENT	(2)
#define AAC_BAT_OPT_PRESENT	(3)
#define AAC_BAT_OPT_NOTPRESENT	(4)
#define AAC_BAT_NOT_SUPPORTED	(5)
/*
 * cpu types
 */
#define AAC_CPU_SIMULATOR	(1)
#define AAC_CPU_I960		(2)
#define AAC_CPU_STRONGARM	(3)

/*
 * Supported Options
 */
#define AAC_OPT_SNAPSHOT		cpu_to_le32(1)
#define AAC_OPT_CLUSTERS		cpu_to_le32(1<<1)
#define AAC_OPT_WRITE_CACHE		cpu_to_le32(1<<2)
#define AAC_OPT_64BIT_DATA		cpu_to_le32(1<<3)
#define AAC_OPT_HOST_TIME_FIB		cpu_to_le32(1<<4)
#define AAC_OPT_RAID50			cpu_to_le32(1<<5)
#define AAC_OPT_4GB_WINDOW		cpu_to_le32(1<<6)
#define AAC_OPT_SCSI_UPGRADEABLE 	cpu_to_le32(1<<7)
#define AAC_OPT_SOFT_ERR_REPORT		cpu_to_le32(1<<8)
#define AAC_OPT_SUPPORTED_RECONDITION 	cpu_to_le32(1<<9)
#define AAC_OPT_SGMAP_HOST64		cpu_to_le32(1<<10)
#define AAC_OPT_ALARM			cpu_to_le32(1<<11)
#define AAC_OPT_NONDASD			cpu_to_le32(1<<12)

struct aac_dev
{
	struct aac_dev		*next;
	const char		*name;
	int			id;

	u16			irq_mask;
	/*
	 *	Map for 128 fib objects (64k)
	 */	
	dma_addr_t		hw_fib_pa;
	struct hw_fib		*hw_fib_va;
	struct hw_fib		*aif_base_va;
	/*
	 *	Fib Headers
	 */
	struct fib              *fibs;

	struct fib		*free_fib;
	struct fib		*timeout_fib;
	spinlock_t		fib_lock;
	
	struct aac_queue_block *queues;
	/*
	 *	The user API will use an IOCTL to register itself to receive
	 *	FIBs from the adapter.  The following list is used to keep
	 *	track of all the threads that have requested these FIBs.  The
	 *	mutex is used to synchronize access to all data associated 
	 *	with the adapter fibs.
	 */
	struct list_head	fib_list;

	struct adapter_ops	a_ops;
	unsigned long		fsrev;		/* Main driver's revision number */
	
	struct aac_init		*init;		/* Holds initialization info to communicate with adapter */
	dma_addr_t		init_pa; 	/* Holds physical address of the init struct */
	
	struct pci_dev		*pdev;		/* Our PCI interface */
	void *			printfbuf;	/* pointer to buffer used for printf's from the adapter */
	void *			comm_addr;	/* Base address of Comm area */
	dma_addr_t		comm_phys;	/* Physical Address of Comm area */
	size_t			comm_size;

	struct Scsi_Host	*scsi_host_ptr;
	struct fsa_scsi_hba	fsa_dev;
	pid_t			thread_pid;
	int			cardtype;
	
	/*
	 *	The following is the device specific extension.
	 */
	union
	{
		struct sa_registers *sa;
		struct rx_registers *rx;
		struct rkt_registers *rkt;
	} regs;
	u32			OIMR; /* Mask Register Cache */
	/*
	 *	AIF thread states
	 */
	u32			aif_thread;
	struct completion	aif_completion;
	struct aac_adapter_info adapter_info;
	/* These are in adapter info but they are in the io flow so
	 * lets break them out so we don't have to do an AND to check them
	 */
	u8			nondasd_support; 
	u8			pae_support;
};

#define AllocateAndMapFibSpace(dev, MapFibContext) \
	(dev)->a_ops.AllocateAndMapFibSpace(dev, MapFibContext)

#define UnmapAndFreeFibSpace(dev, MapFibContext) \
	(dev)->a_ops.UnmapAndFreeFibSpace(dev, MapFibContext)

#define aac_adapter_interrupt(dev) \
	(dev)->a_ops.adapter_interrupt(dev)

#define aac_adapter_notify(dev, event) \
	(dev)->a_ops.adapter_notify(dev, event)

#define aac_adapter_enable_int(dev, event) \
	(dev)->a_ops.adapter_enable_int(dev, event)

#define aac_adapter_disable_int(dev, event) \
	dev->a_ops.adapter_disable_int(dev, event)

#define aac_adapter_check_health(dev) \
	(dev)->a_ops.adapter_check_health(dev)


#define FIB_CONTEXT_FLAG_TIMED_OUT		(0x00000001)

/*
 *	Define the command values
 */
 
#define		Null			0
#define 	GetAttributes		1
#define 	SetAttributes		2
#define 	Lookup			3
#define 	ReadLink		4
#define 	Read			5
#define 	Write			6
#define		Create			7
#define		MakeDirectory		8
#define		SymbolicLink		9
#define		MakeNode		10
#define		Removex			11
#define		RemoveDirectoryx	12
#define		Rename			13
#define		Link			14
#define		ReadDirectory		15
#define		ReadDirectoryPlus	16
#define		FileSystemStatus	17
#define		FileSystemInfo		18
#define		PathConfigure		19
#define		Commit			20
#define		Mount			21
#define		UnMount			22
#define		Newfs			23
#define		FsCheck			24
#define		FsSync			25
#define		SimReadWrite		26
#define		SetFileSystemStatus	27
#define		BlockRead		28
#define		BlockWrite		29
#define		NvramIoctl		30
#define		FsSyncWait		31
#define		ClearArchiveBit		32
#define		SetAcl			33
#define		GetAcl			34
#define		AssignAcl		35
#define		FaultInsertion		36	/* Fault Insertion Command */
#define		CrazyCache		37	/* Crazycache */

#define		MAX_FSACOMMAND_NUM	38


/*
 *	Define the status returns. These are very unixlike although
 *	most are not in fact used
 */

#define		ST_OK		0
#define		ST_PERM		1
#define		ST_NOENT	2
#define		ST_IO		5
#define		ST_NXIO		6
#define		ST_E2BIG	7
#define		ST_ACCES	13
#define		ST_EXIST	17
#define		ST_XDEV		18
#define		ST_NODEV	19
#define		ST_NOTDIR	20
#define		ST_ISDIR	21
#define		ST_INVAL	22
#define		ST_FBIG		27
#define		ST_NOSPC	28
#define		ST_ROFS		30
#define		ST_MLINK	31
#define		ST_WOULDBLOCK	35
#define		ST_NAMETOOLONG	63
#define		ST_NOTEMPTY	66
#define		ST_DQUOT	69
#define		ST_STALE	70
#define		ST_REMOTE	71
#define		ST_BADHANDLE	10001
#define		ST_NOT_SYNC	10002
#define		ST_BAD_COOKIE	10003
#define		ST_NOTSUPP	10004
#define		ST_TOOSMALL	10005
#define		ST_SERVERFAULT	10006
#define		ST_BADTYPE	10007
#define		ST_JUKEBOX	10008
#define		ST_NOTMOUNTED	10009
#define		ST_MAINTMODE	10010
#define		ST_STALEACL	10011

/*
 *	On writes how does the client want the data written.
 */

#define	CACHE_CSTABLE		1
#define CACHE_UNSTABLE		2

/*
 *	Lets the client know at which level the data was commited on
 *	a write request
 */

#define	CMFILE_SYNCH_NVRAM	1
#define	CMDATA_SYNCH_NVRAM	2
#define	CMFILE_SYNCH		3
#define CMDATA_SYNCH		4
#define CMUNSTABLE		5

struct aac_read
{
	u32	 	command;
	u32 		cid;
	u32 		block;
	u32 		count;
	struct sgmap	sg;	// Must be last in struct because it is variable
};

struct aac_read64
{
	u32	 	command;
	u16 		cid;
	u16 		sector_count;
	u32 		block;
	u16		pad;
	u16		flags;
	struct sgmap64	sg;	// Must be last in struct because it is variable
};

struct aac_read_reply
{
	u32	 	status;
	u32 		count;
};

struct aac_write
{
	u32		command;
	u32 		cid;
	u32 		block;
	u32 		count;
	u32	 	stable;	// Not used
	struct sgmap	sg;	// Must be last in struct because it is variable
};

struct aac_write64
{
	u32	 	command;
	u16 		cid;
	u16 		sector_count;
	u32 		block;
	u16		pad;
	u16		flags;
	struct sgmap64	sg;	// Must be last in struct because it is variable
};
struct aac_write_reply
{
	u32		status;
	u32 		count;
	u32		committed;
};

struct aac_srb
{
	u32		function;
	u32		channel;
	u32		id;
	u32		lun;
	u32		timeout;
	u32		flags;
	u32		count;		// Data xfer size
	u32		retry_limit;
	u32		cdb_size;
	u8		cdb[16];
	struct	sgmap	sg;
};



#define		AAC_SENSE_BUFFERSIZE	 30

struct aac_srb_reply
{
	u32		status;
	u32		srb_status;
	u32		scsi_status;
	u32		data_xfer_length;
	u32		sense_data_size;
	u8		sense_data[AAC_SENSE_BUFFERSIZE]; // Can this be SCSI_SENSE_BUFFERSIZE
};
/*
 * SRB Flags
 */
#define		SRB_NoDataXfer		 0x0000
#define		SRB_DisableDisconnect	 0x0004
#define		SRB_DisableSynchTransfer 0x0008
#define 	SRB_BypassFrozenQueue	 0x0010
#define		SRB_DisableAutosense	 0x0020
#define		SRB_DataIn		 0x0040
#define 	SRB_DataOut		 0x0080

/*
 * SRB Functions - set in aac_srb->function
 */
#define	SRBF_ExecuteScsi	0x0000
#define	SRBF_ClaimDevice	0x0001
#define	SRBF_IO_Control		0x0002
#define	SRBF_ReceiveEvent	0x0003
#define	SRBF_ReleaseQueue	0x0004
#define	SRBF_AttachDevice	0x0005
#define	SRBF_ReleaseDevice	0x0006
#define	SRBF_Shutdown		0x0007
#define	SRBF_Flush		0x0008
#define	SRBF_AbortCommand	0x0010
#define	SRBF_ReleaseRecovery	0x0011
#define	SRBF_ResetBus		0x0012
#define	SRBF_ResetDevice	0x0013
#define	SRBF_TerminateIO	0x0014
#define	SRBF_FlushQueue		0x0015
#define	SRBF_RemoveDevice	0x0016
#define	SRBF_DomainValidation	0x0017

/* 
 * SRB SCSI Status - set in aac_srb->scsi_status
 */
#define SRB_STATUS_PENDING                  0x00
#define SRB_STATUS_SUCCESS                  0x01
#define SRB_STATUS_ABORTED                  0x02
#define SRB_STATUS_ABORT_FAILED             0x03
#define SRB_STATUS_ERROR                    0x04
#define SRB_STATUS_BUSY                     0x05
#define SRB_STATUS_INVALID_REQUEST          0x06
#define SRB_STATUS_INVALID_PATH_ID          0x07
#define SRB_STATUS_NO_DEVICE                0x08
#define SRB_STATUS_TIMEOUT                  0x09
#define SRB_STATUS_SELECTION_TIMEOUT        0x0A
#define SRB_STATUS_COMMAND_TIMEOUT          0x0B
#define SRB_STATUS_MESSAGE_REJECTED         0x0D
#define SRB_STATUS_BUS_RESET                0x0E
#define SRB_STATUS_PARITY_ERROR             0x0F
#define SRB_STATUS_REQUEST_SENSE_FAILED     0x10
#define SRB_STATUS_NO_HBA                   0x11
#define SRB_STATUS_DATA_OVERRUN             0x12
#define SRB_STATUS_UNEXPECTED_BUS_FREE      0x13
#define SRB_STATUS_PHASE_SEQUENCE_FAILURE   0x14
#define SRB_STATUS_BAD_SRB_BLOCK_LENGTH     0x15
#define SRB_STATUS_REQUEST_FLUSHED          0x16
#define SRB_STATUS_DELAYED_RETRY	    0x17
#define SRB_STATUS_INVALID_LUN              0x20
#define SRB_STATUS_INVALID_TARGET_ID        0x21
#define SRB_STATUS_BAD_FUNCTION             0x22
#define SRB_STATUS_ERROR_RECOVERY           0x23
#define SRB_STATUS_NOT_STARTED		    0x24
#define SRB_STATUS_NOT_IN_USE		    0x30
#define SRB_STATUS_FORCE_ABORT		    0x31
#define SRB_STATUS_DOMAIN_VALIDATION_FAIL   0x32

/*
 * Object-Server / Volume-Manager Dispatch Classes
 */

#define		VM_Null			0
#define		VM_NameServe		1
#define		VM_ContainerConfig	2
#define		VM_Ioctl		3
#define		VM_FilesystemIoctl	4
#define		VM_CloseAll		5
#define		VM_CtBlockRead		6
#define		VM_CtBlockWrite		7
#define		VM_SliceBlockRead	8	/* raw access to configured "storage objects" */
#define		VM_SliceBlockWrite	9
#define		VM_DriveBlockRead	10	/* raw access to physical devices */
#define		VM_DriveBlockWrite	11
#define		VM_EnclosureMgt		12	/* enclosure management */
#define		VM_Unused		13	/* used to be diskset management */
#define		VM_CtBlockVerify	14
#define		VM_CtPerf		15	/* performance test */
#define		VM_CtBlockRead64	16
#define		VM_CtBlockWrite64	17
#define		VM_CtBlockVerify64	18
#define		VM_CtHostRead64		19
#define		VM_CtHostWrite64	20

#define		MAX_VMCOMMAND_NUM	21	/* used for sizing stats array - leave last */

/*
 *	Descriptive information (eg, vital stats)
 *	that a content manager might report.  The
 *	FileArray filesystem component is one example
 *	of a content manager.  Raw mode might be
 *	another.
 */

struct aac_fsinfo {
	u32  fsTotalSize;	/* Consumed by fs, incl. metadata */
	u32  fsBlockSize;
	u32  fsFragSize;
	u32  fsMaxExtendSize;
	u32  fsSpaceUnits;
	u32  fsMaxNumFiles;
	u32  fsNumFreeFiles;
	u32  fsInodeDensity;
};	/* valid iff ObjType == FT_FILESYS && !(ContentState & FSCS_NOTCLEAN) */

union aac_contentinfo {
	struct aac_fsinfo filesys;	/* valid iff ObjType == FT_FILESYS && !(ContentState & FSCS_NOTCLEAN) */
};

/*
 *	Query for "mountable" objects, ie, objects that are typically
 *	associated with a drive letter on the client (host) side.
 */

struct aac_mntent {
	u32    			oid;
	u8			name[16];	// if applicable
	struct creation_info	create_info;	// if applicable
	u32			capacity;
	u32			vol;    	// substrate structure
	u32			obj;	        // FT_FILESYS, FT_DATABASE, etc.
	u32			state;		// unready for mounting, readonly, etc.
	union aac_contentinfo	fileinfo;	// Info specific to content manager (eg, filesystem)
	u32			altoid;		// != oid <==> snapshot or broken mirror exists
};

#define FSCS_NOTCLEAN	0x0001  /* fsck is neccessary before mounting */
#define FSCS_READONLY	0x0002	/* possible result of broken mirror */
#define FSCS_HIDDEN	0x0004	/* should be ignored - set during a clear */

struct aac_query_mount {
	u32		command;
	u32		type;
	u32		count;
};

struct aac_mount {
	u32		status;
	u32	   	type;           /* should be same as that requested */
	u32		count;
	struct aac_mntent mnt[1];
};

/*
 * The following command is sent to shut down each container.
 */

struct aac_close {
	u32	command;
	u32	cid;
};

struct aac_query_disk
{
	s32	cnum;
	s32	bus;
	s32	id;
	s32	lun;
	u32	valid;
	u32	locked;
	u32	deleted;
	s32	instance;
	s8	name[10];
	u32	unmapped;
};

struct aac_delete_disk {
	u32	disknum;
	u32	cnum;
};
 
struct fib_ioctl
{
	u32	fibctx;
	s32	wait;
	char	__user *fib;
};

struct revision
{
	u32 compat;
	u32 version;
	u32 build;
};
	
/*
 * 	Ugly - non Linux like ioctl coding for back compat.
 */

#define CTL_CODE(function, method) (                 \
    (4<< 16) | ((function) << 2) | (method) \
)

/*
 *	Define the method codes for how buffers are passed for I/O and FS 
 *	controls
 */

#define METHOD_BUFFERED                 0
#define METHOD_NEITHER                  3

/*
 *	Filesystem ioctls
 */

#define FSACTL_SENDFIB                  	CTL_CODE(2050, METHOD_BUFFERED)
#define FSACTL_SEND_RAW_SRB               	CTL_CODE(2067, METHOD_BUFFERED)
#define FSACTL_DELETE_DISK			0x163
#define FSACTL_QUERY_DISK			0x173
#define FSACTL_OPEN_GET_ADAPTER_FIB		CTL_CODE(2100, METHOD_BUFFERED)
#define FSACTL_GET_NEXT_ADAPTER_FIB		CTL_CODE(2101, METHOD_BUFFERED)
#define FSACTL_CLOSE_GET_ADAPTER_FIB		CTL_CODE(2102, METHOD_BUFFERED)
#define FSACTL_MINIPORT_REV_CHECK               CTL_CODE(2107, METHOD_BUFFERED)
#define FSACTL_GET_PCI_INFO               	CTL_CODE(2119, METHOD_BUFFERED)
#define FSACTL_FORCE_DELETE_DISK		CTL_CODE(2120, METHOD_NEITHER)
#define FSACTL_GET_CONTAINERS			2131


struct aac_common
{
	/*
	 *	If this value is set to 1 then interrupt moderation will occur 
	 *	in the base commuication support.
	 */
	u32 irq_mod;
	u32 peak_fibs;
	u32 zero_fibs;
	u32 fib_timeouts;
	/*
	 *	Statistical counters in debug mode
	 */
#ifdef DBG
	u32 FibsSent;
	u32 FibRecved;
	u32 NoResponseSent;
	u32 NoResponseRecved;
	u32 AsyncSent;
	u32 AsyncRecved;
	u32 NormalSent;
	u32 NormalRecved;
#endif
};

extern struct aac_common aac_config;


/*
 *	The following macro is used when sending and receiving FIBs. It is
 *	only used for debugging.
 */
 
#ifdef DBG
#define	FIB_COUNTER_INCREMENT(counter)		(counter)++
#else
#define	FIB_COUNTER_INCREMENT(counter)		
#endif

/*
 *	Adapter direct commands
 *	Monitor/Kernel API
 */

#define	BREAKPOINT_REQUEST		cpu_to_le32(0x00000004)
#define	INIT_STRUCT_BASE_ADDRESS	cpu_to_le32(0x00000005)
#define READ_PERMANENT_PARAMETERS	cpu_to_le32(0x0000000a)
#define WRITE_PERMANENT_PARAMETERS	cpu_to_le32(0x0000000b)
#define HOST_CRASHING			cpu_to_le32(0x0000000d)
#define	SEND_SYNCHRONOUS_FIB		cpu_to_le32(0x0000000c)
#define	COMMAND_POST_RESULTS		cpu_to_le32(0x00000014)
#define GET_ADAPTER_PROPERTIES		cpu_to_le32(0x00000019)
#define RE_INIT_ADAPTER			cpu_to_le32(0x000000ee)

/*
 *	Adapter Status Register
 *
 *  Phase Staus mailbox is 32bits:
 *	<31:16> = Phase Status
 *	<15:0>  = Phase
 *
 *	The adapter reports is present state through the phase.  Only
 *	a single phase should be ever be set.  Each phase can have multiple
 *	phase status bits to provide more detailed information about the 
 *	state of the board.  Care should be taken to ensure that any phase 
 *	status bits that are set when changing the phase are also valid
 *	for the new phase or be cleared out.  Adapter software (monitor,
 *	iflash, kernel) is responsible for properly maintining the phase 
 *	status mailbox when it is running.
 *											
 *	MONKER_API Phases							
 *
 *	Phases are bit oriented.  It is NOT valid  to have multiple bits set						
 */					

#define	SELF_TEST_FAILED		(cpu_to_le32(0x00000004))
#define MONITOR_PANIC			(cpu_to_le32(0x00000020))
#define	KERNEL_UP_AND_RUNNING		(cpu_to_le32(0x00000080))
#define	KERNEL_PANIC			(cpu_to_le32(0x00000100))

/*
 *	Doorbell bit defines
 */

#define DoorBellSyncCmdAvailable	cpu_to_le32(1<<0)	// Host -> Adapter
#define DoorBellPrintfDone		cpu_to_le32(1<<5)	// Host -> Adapter
#define DoorBellAdapterNormCmdReady	cpu_to_le32(1<<1)	// Adapter -> Host
#define DoorBellAdapterNormRespReady	cpu_to_le32(1<<2)	// Adapter -> Host
#define DoorBellAdapterNormCmdNotFull	cpu_to_le32(1<<3)	// Adapter -> Host
#define DoorBellAdapterNormRespNotFull	cpu_to_le32(1<<4)	// Adapter -> Host
#define DoorBellPrintfReady		cpu_to_le32(1<<5)	// Adapter -> Host

/*
 *	For FIB communication, we need all of the following things
 *	to send back to the user.
 */
 
#define 	AifCmdEventNotify	1	/* Notify of event */
#define			AifEnConfigChange	3	/* Adapter configuration change */
#define			AifEnContainerChange	4	/* Container configuration change */
#define			AifEnDeviceFailure	5	/* SCSI device failed */
#define			AifEnAddContainer	15	/* A new array was created */
#define			AifEnDeleteContainer	16	/* A container was deleted */
#define			AifEnExpEvent		23	/* Firmware Event Log */
#define			AifExeFirmwarePanic	3	/* Firmware Event Panic */
#define			AifHighPriority		3	/* Highest Priority Event */

#define		AifCmdJobProgress	2	/* Progress report */
#define			AifJobCtrZero	101	/* Array Zero progress */
#define			AifJobStsSuccess 1	/* Job completes */
#define		AifCmdAPIReport		3	/* Report from other user of API */
#define		AifCmdDriverNotify	4	/* Notify host driver of event */
#define			AifDenMorphComplete 200	/* A morph operation completed */
#define			AifDenVolumeExtendComplete 201 /* A volume extend completed */
#define		AifReqJobList		100	/* Gets back complete job list */
#define		AifReqJobsForCtr	101	/* Gets back jobs for specific container */
#define		AifReqJobsForScsi	102	/* Gets back jobs for specific SCSI device */ 
#define		AifReqJobReport		103	/* Gets back a specific job report or list of them */ 
#define		AifReqTerminateJob	104	/* Terminates job */
#define		AifReqSuspendJob	105	/* Suspends a job */
#define		AifReqResumeJob		106	/* Resumes a job */ 
#define		AifReqSendAPIReport	107	/* API generic report requests */
#define		AifReqAPIJobStart	108	/* Start a job from the API */
#define		AifReqAPIJobUpdate	109	/* Update a job report from the API */
#define		AifReqAPIJobFinish	110	/* Finish a job from the API */

/*
 *	Adapter Initiated FIB command structures. Start with the adapter
 *	initiated FIBs that really come from the adapter, and get responded
 *	to by the host.
 */

struct aac_aifcmd {
	u32 command;		/* Tell host what type of notify this is */
	u32 seqnum;		/* To allow ordering of reports (if necessary) */
	u8 data[1];		/* Undefined length (from kernel viewpoint) */
};

/**
 * 	Convert capacity to cylinders
 *  	accounting for the fact capacity could be a 64 bit value
 *
 */
static inline u32 cap_to_cyls(sector_t capacity, u32 divisor)
{
	sector_div(capacity, divisor);
	return (u32)capacity;
}

struct scsi_cmnd;

const char *aac_driverinfo(struct Scsi_Host *);
struct fib *fib_alloc(struct aac_dev *dev);
int fib_setup(struct aac_dev *dev);
void fib_map_free(struct aac_dev *dev);
void fib_free(struct fib * context);
void fib_init(struct fib * context);
void fib_dealloc(struct fib * context);
void aac_printf(struct aac_dev *dev, u32 val);
int fib_send(u16 command, struct fib * context, unsigned long size, int priority, int wait, int reply, fib_callback callback, void *ctxt);
int aac_consumer_get(struct aac_dev * dev, struct aac_queue * q, struct aac_entry **entry);
int aac_consumer_avail(struct aac_dev * dev, struct aac_queue * q);
void aac_consumer_free(struct aac_dev * dev, struct aac_queue * q, u32 qnum);
int fib_complete(struct fib * context);
#define fib_data(fibctx) ((void *)(fibctx)->hw_fib->data)
struct aac_dev *aac_init_adapter(struct aac_dev *dev);
int aac_get_containers(struct aac_dev *dev);
int aac_scsi_cmd(struct scsi_cmnd *cmd);
int aac_dev_ioctl(struct aac_dev *dev, int cmd, void __user *arg);
int aac_do_ioctl(struct aac_dev * dev, int cmd, void __user *arg);
int aac_rx_init(struct aac_dev *dev);
int aac_rkt_init(struct aac_dev *dev);
int aac_sa_init(struct aac_dev *dev);
unsigned int aac_response_normal(struct aac_queue * q);
unsigned int aac_command_normal(struct aac_queue * q);
int aac_command_thread(struct aac_dev * dev);
int aac_close_fib_context(struct aac_dev * dev, struct aac_fib_context *fibctx);
int fib_adapter_complete(struct fib * fibptr, unsigned short size);
struct aac_driver_ident* aac_get_driver_ident(int devtype);
int aac_get_adapter_info(struct aac_dev* dev);
int aac_send_shutdown(struct aac_dev *dev);
