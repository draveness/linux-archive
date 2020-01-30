/* -*- mode: c; c-basic-offset: 8 -*- */

/* Driver for 53c700 and 53c700-66 chips from NCR and Symbios
 *
 * Copyright (C) 2001 by James.Bottomley@HansenPartnership.com
 */

#ifndef _53C700_H
#define _53C700_H

#include <linux/interrupt.h>
#include <asm/io.h>

#include <scsi/scsi_device.h>


#if defined(CONFIG_53C700_MEM_MAPPED) && defined(CONFIG_53C700_IO_MAPPED)
#define CONFIG_53C700_BOTH_MAPPED
#endif

/* Turn on for general debugging---too verbose for normal use */
#undef	NCR_700_DEBUG
/* Debug the tag queues, checking hash queue allocation and deallocation
 * and search for duplicate tags */
#undef NCR_700_TAG_DEBUG

#ifdef NCR_700_DEBUG
#define DEBUG(x)	printk x
#else
#define DEBUG(x)
#endif

/* The number of available command slots */
#define NCR_700_COMMAND_SLOTS_PER_HOST	64
/* The maximum number of Scatter Gathers we allow */
#define NCR_700_SG_SEGMENTS		32
/* The maximum number of luns (make this of the form 2^n) */
#define NCR_700_MAX_LUNS		32
#define NCR_700_LUN_MASK		(NCR_700_MAX_LUNS - 1)
/* Maximum number of tags the driver ever allows per device */
#define NCR_700_MAX_TAGS		16
/* Tag depth the driver starts out with (can be altered in sysfs) */
#define NCR_700_DEFAULT_TAGS		4
/* This is the default number of commands per LUN in the untagged case.
 * two is a good value because it means we can have one command active and
 * one command fully prepared and waiting
 */
#define NCR_700_CMD_PER_LUN		2
/* magic byte identifying an internally generated REQUEST_SENSE command */
#define NCR_700_INTERNAL_SENSE_MAGIC	0x42

/* WARNING: Leave this in for now: the dependency preprocessor doesn't
 * pick up file specific flags, so must define here if they are not
 * set */
#if !defined(CONFIG_53C700_IO_MAPPED) && !defined(CONFIG_53C700_MEM_MAPPED)
#error "Config.in must define either CONFIG_53C700_IO_MAPPED or CONFIG_53C700_MEM_MAPPED to use this scsi core."
#endif

struct NCR_700_Host_Parameters;

/* These are the externally used routines */
struct Scsi_Host *NCR_700_detect(struct scsi_host_template *,
		struct NCR_700_Host_Parameters *);
int NCR_700_release(struct Scsi_Host *host);
irqreturn_t NCR_700_intr(int, void *, struct pt_regs *);


enum NCR_700_Host_State {
	NCR_700_HOST_BUSY,
	NCR_700_HOST_FREE,
};

struct NCR_700_SG_List {
	/* The following is a script fragment to move the buffer onto the
	 * bus and then link the next fragment or return */
	#define	SCRIPT_MOVE_DATA_IN		0x09000000
	#define	SCRIPT_MOVE_DATA_OUT		0x08000000
	__u32	ins;
	__u32	pAddr;
	#define	SCRIPT_NOP			0x80000000
	#define	SCRIPT_RETURN			0x90080000
};

/* We use device->hostdata to store negotiated parameters.  This is
 * supposed to be a pointer to a device private area, but we cannot
 * really use it as such since it will never be freed, so just use the
 * 32 bits to cram the information.  The SYNC negotiation sequence looks
 * like:
 * 
 * If DEV_NEGOTIATED_SYNC not set, tack and SDTR message on to the
 * initial identify for the device and set DEV_BEGIN_SYNC_NEGOTATION
 * If we get an SDTR reply, work out the SXFER parameters, squirrel
 * them away here, clear DEV_BEGIN_SYNC_NEGOTIATION and set
 * DEV_NEGOTIATED_SYNC.  If we get a REJECT msg, squirrel
 *
 *
 * 0:7	SXFER_REG negotiated value for this device
 * 8:15 Current queue depth
 * 16	negotiated SYNC flag
 * 17 begin SYNC negotiation flag 
 * 18 device supports tag queueing */
#define NCR_700_DEV_NEGOTIATED_SYNC	(1<<16)
#define NCR_700_DEV_BEGIN_SYNC_NEGOTIATION	(1<<17)
#define NCR_700_DEV_BEGIN_TAG_QUEUEING	(1<<18)
#define NCR_700_DEV_PRINT_SYNC_NEGOTIATION (1<<19)

static inline void
NCR_700_set_depth(struct scsi_device *SDp, __u8 depth)
{
	long l = (long)SDp->hostdata;

	l &= 0xffff00ff;
	l |= 0xff00 & (depth << 8);
	SDp->hostdata = (void *)l;
}
static inline __u8
NCR_700_get_depth(struct scsi_device *SDp)
{
	return ((((unsigned long)SDp->hostdata) & 0xff00)>>8);
}
static inline int
NCR_700_is_flag_set(struct scsi_device *SDp, __u32 flag)
{
	return (((unsigned long)SDp->hostdata) & flag) == flag;
}
static inline int
NCR_700_is_flag_clear(struct scsi_device *SDp, __u32 flag)
{
	return (((unsigned long)SDp->hostdata) & flag) == 0;
}
static inline void
NCR_700_set_flag(struct scsi_device *SDp, __u32 flag)
{
	SDp->hostdata = (void *)((long)SDp->hostdata | (flag & 0xffff0000));
}
static inline void
NCR_700_clear_flag(struct scsi_device *SDp, __u32 flag)
{
	SDp->hostdata = (void *)((long)SDp->hostdata & ~(flag & 0xffff0000));
}

struct NCR_700_command_slot {
	struct NCR_700_SG_List	SG[NCR_700_SG_SEGMENTS+1];
	struct NCR_700_SG_List	*pSG;
	#define NCR_700_SLOT_MASK 0xFC
	#define NCR_700_SLOT_MAGIC 0xb8
	#define	NCR_700_SLOT_FREE (0|NCR_700_SLOT_MAGIC) /* slot may be used */
	#define NCR_700_SLOT_BUSY (1|NCR_700_SLOT_MAGIC) /* slot has command active on HA */
	#define NCR_700_SLOT_QUEUED (2|NCR_700_SLOT_MAGIC) /* slot has command to be made active on HA */
	__u8	state;
	int	tag;
	__u32	resume_offset;
	struct scsi_cmnd *cmnd;
	/* The pci_mapped address of the actual command in cmnd */
	dma_addr_t	pCmd;
	__u32		temp;
	/* if this command is a pci_single mapping, holds the dma address
	 * for later unmapping in the done routine */
	dma_addr_t	dma_handle;
	/* historical remnant, now used to link free commands */
	struct NCR_700_command_slot *ITL_forw;
};

struct NCR_700_Host_Parameters {
	/* These must be filled in by the calling driver */
	int	clock;			/* board clock speed in MHz */
	unsigned long	base;		/* the base for the port (copied to host) */
	struct device	*dev;
	__u32	dmode_extra;	/* adjustable bus settings */
	__u32	differential:1;	/* if we are differential */
#ifdef CONFIG_53C700_LE_ON_BE
	/* This option is for HP only.  Set it if your chip is wired for
	 * little endian on this platform (which is big endian) */
	__u32	force_le_on_be:1;
#endif
	__u32	chip710:1;	/* set if really a 710 not 700 */
	__u32	burst_disable:1;	/* set to 1 to disable 710 bursting */

	/* NOTHING BELOW HERE NEEDS ALTERING */
	__u32	fast:1;		/* if we can alter the SCSI bus clock
                                   speed (so can negiotiate sync) */
#ifdef CONFIG_53C700_BOTH_MAPPED
	__u32	mem_mapped;	/* set if memory mapped */
#endif
	int	sync_clock;	/* The speed of the SYNC core */

	__u32	*script;		/* pointer to script location */
	__u32	pScript;		/* physical mem addr of script */

	enum NCR_700_Host_State state; /* protected by state lock */
	struct scsi_cmnd *cmd;
	/* Note: pScript contains the single consistent block of
	 * memory.  All the msgin, msgout and status are allocated in
	 * this memory too (at separate cache lines).  TOTAL_MEM_SIZE
	 * represents the total size of this area */
#define	MSG_ARRAY_SIZE	8
#define	MSGOUT_OFFSET	(L1_CACHE_ALIGN(sizeof(SCRIPT)))
	__u8	*msgout;
#define MSGIN_OFFSET	(MSGOUT_OFFSET + L1_CACHE_ALIGN(MSG_ARRAY_SIZE))
	__u8	*msgin;
#define STATUS_OFFSET	(MSGIN_OFFSET + L1_CACHE_ALIGN(MSG_ARRAY_SIZE))
	__u8	*status;
#define SLOTS_OFFSET	(STATUS_OFFSET + L1_CACHE_ALIGN(MSG_ARRAY_SIZE))
	struct NCR_700_command_slot	*slots;
#define	TOTAL_MEM_SIZE	(SLOTS_OFFSET + L1_CACHE_ALIGN(sizeof(struct NCR_700_command_slot) * NCR_700_COMMAND_SLOTS_PER_HOST))
	int	saved_slot_position;
	int	command_slot_count; /* protected by state lock */
	__u8	tag_negotiated;
	__u8	rev;
	__u8	reselection_id;
	__u8	min_period;

	/* Free list, singly linked by ITL_forw elements */
	struct NCR_700_command_slot *free_list;
	/* Completion for waited for ops, like reset, abort or
	 * device reset.
	 *
	 * NOTE: relies on single threading in the error handler to
	 * have only one outstanding at once */
	struct completion *eh_complete;
};

/*
 *	53C700 Register Interface - the offset from the Selected base
 *	I/O address */
#ifdef CONFIG_53C700_LE_ON_BE
#define bE	(hostdata->force_le_on_be ? 0 : 3)
#define	bSWAP	(hostdata->force_le_on_be)
#elif defined(__BIG_ENDIAN)
#define bE	3
#define bSWAP	0
#elif defined(__LITTLE_ENDIAN)
#define bE	0
#define bSWAP	0
#else
#error "__BIG_ENDIAN or __LITTLE_ENDIAN must be defined, did you include byteorder.h?"
#endif
#define bS_to_cpu(x)	(bSWAP ? le32_to_cpu(x) : (x))
#define bS_to_host(x)	(bSWAP ? cpu_to_le32(x) : (x))

/* NOTE: These registers are in the LE register space only, the required byte
 * swapping is done by the NCR_700_{read|write}[b] functions */
#define	SCNTL0_REG			0x00
#define		FULL_ARBITRATION	0xc0
#define 	PARITY			0x08
#define		ENABLE_PARITY		0x04
#define 	AUTO_ATN		0x02
#define	SCNTL1_REG			0x01
#define 	SLOW_BUS		0x80
#define		ENABLE_SELECT		0x20
#define		ASSERT_RST		0x08
#define		ASSERT_EVEN_PARITY	0x04
#define	SDID_REG			0x02
#define	SIEN_REG			0x03
#define 	PHASE_MM_INT		0x80
#define 	FUNC_COMP_INT		0x40
#define 	SEL_TIMEOUT_INT		0x20
#define 	SELECT_INT		0x10
#define 	GROSS_ERR_INT		0x08
#define 	UX_DISC_INT		0x04
#define 	RST_INT			0x02
#define 	PAR_ERR_INT		0x01
#define	SCID_REG			0x04
#define SXFER_REG			0x05
#define		ASYNC_OPERATION		0x00
#define SODL_REG                        0x06
#define	SOCL_REG			0x07
#define	SFBR_REG			0x08
#define	SIDL_REG			0x09
#define	SBDL_REG			0x0A
#define	SBCL_REG			0x0B
/* read bits */
#define		SBCL_IO			0x01
/*write bits */
#define		SYNC_DIV_AS_ASYNC	0x00
#define		SYNC_DIV_1_0		0x01
#define		SYNC_DIV_1_5		0x02
#define		SYNC_DIV_2_0		0x03
#define	DSTAT_REG			0x0C
#define		ILGL_INST_DETECTED	0x01
#define		WATCH_DOG_INTERRUPT	0x02
#define		SCRIPT_INT_RECEIVED	0x04
#define		ABORTED			0x10
#define	SSTAT0_REG			0x0D
#define		PARITY_ERROR		0x01
#define		SCSI_RESET_DETECTED	0x02
#define		UNEXPECTED_DISCONNECT	0x04
#define		SCSI_GROSS_ERROR	0x08
#define		SELECTED		0x10
#define		SELECTION_TIMEOUT	0x20
#define		FUNCTION_COMPLETE	0x40
#define		PHASE_MISMATCH 		0x80
#define	SSTAT1_REG			0x0E
#define		SIDL_REG_FULL		0x80
#define		SODR_REG_FULL		0x40
#define		SODL_REG_FULL		0x20
#define SSTAT2_REG                      0x0F
#define CTEST0_REG                      0x14
#define		BTB_TIMER_DISABLE	0x40
#define CTEST1_REG                      0x15
#define CTEST2_REG                      0x16
#define CTEST3_REG                      0x17
#define CTEST4_REG                      0x18
#define         DISABLE_FIFO            0x00
#define         SLBE                    0x10
#define         SFWR                    0x08
#define         BYTE_LANE0              0x04
#define         BYTE_LANE1              0x05
#define         BYTE_LANE2              0x06
#define         BYTE_LANE3              0x07
#define         SCSI_ZMODE              0x20
#define         ZMODE                   0x40
#define CTEST5_REG                      0x19
#define         MASTER_CONTROL          0x10
#define         DMA_DIRECTION           0x08
#define CTEST7_REG                      0x1B
#define		BURST_DISABLE		0x80 /* 710 only */
#define		SEL_TIMEOUT_DISABLE	0x10 /* 710 only */
#define         DFP                     0x08
#define         EVP                     0x04
#define		DIFF			0x01
#define CTEST6_REG                      0x1A
#define	TEMP_REG			0x1C
#define	DFIFO_REG			0x20
#define		FLUSH_DMA_FIFO		0x80
#define		CLR_FIFO		0x40
#define	ISTAT_REG			0x21
#define		ABORT_OPERATION		0x80
#define		SOFTWARE_RESET_710	0x40
#define		DMA_INT_PENDING		0x01
#define		SCSI_INT_PENDING	0x02
#define		CONNECTED		0x08
#define CTEST8_REG                      0x22
#define         LAST_DIS_ENBL           0x01
#define		SHORTEN_FILTERING	0x04
#define		ENABLE_ACTIVE_NEGATION	0x10
#define		GENERATE_RECEIVE_PARITY	0x20
#define		CLR_FIFO_710		0x04
#define		FLUSH_DMA_FIFO_710	0x08
#define CTEST9_REG                      0x23
#define	DBC_REG				0x24
#define	DCMD_REG			0x27
#define	DNAD_REG			0x28
#define	DIEN_REG			0x39
#define		BUS_FAULT		0x20
#define 	ABORT_INT		0x10
#define 	INT_INST_INT		0x04
#define 	WD_INT			0x02
#define 	ILGL_INST_INT		0x01
#define	DCNTL_REG			0x3B
#define		SOFTWARE_RESET		0x01
#define		COMPAT_700_MODE		0x01
#define 	SCRPTS_16BITS		0x20
#define		ASYNC_DIV_2_0		0x00
#define		ASYNC_DIV_1_5		0x40
#define		ASYNC_DIV_1_0		0x80
#define		ASYNC_DIV_3_0		0xc0
#define DMODE_710_REG			0x38
#define	DMODE_700_REG			0x34
#define		BURST_LENGTH_1		0x00
#define		BURST_LENGTH_2		0x40
#define		BURST_LENGTH_4		0x80
#define		BURST_LENGTH_8		0xC0
#define		DMODE_FC1		0x10
#define		DMODE_FC2		0x20
#define 	BW16			32 
#define 	MODE_286		16
#define 	IO_XFER			8
#define 	FIXED_ADDR		4

#define DSP_REG                         0x2C
#define DSPS_REG                        0x30

/* Parameters to begin SDTR negotiations.  Empirically, I find that
 * the 53c700-66 cannot handle an offset >8, so don't change this  */
#define NCR_700_MAX_OFFSET	8
/* Was hoping the max offset would be greater for the 710, but
 * empirically it seems to be 8 also */
#define NCR_710_MAX_OFFSET	8
#define NCR_700_MIN_XFERP	1
#define NCR_710_MIN_XFERP	0
#define NCR_700_MIN_PERIOD	25 /* for SDTR message, 100ns */

#define script_patch_32(script, symbol, value) \
{ \
	int i; \
	for(i=0; i< (sizeof(A_##symbol##_used) / sizeof(__u32)); i++) { \
		__u32 val = bS_to_cpu((script)[A_##symbol##_used[i]]) + value; \
		(script)[A_##symbol##_used[i]] = bS_to_host(val); \
		dma_cache_sync(&(script)[A_##symbol##_used[i]], 4, DMA_TO_DEVICE); \
		DEBUG((" script, patching %s at %d to 0x%lx\n", \
		       #symbol, A_##symbol##_used[i], (value))); \
	} \
}

#define script_patch_32_abs(script, symbol, value) \
{ \
	int i; \
	for(i=0; i< (sizeof(A_##symbol##_used) / sizeof(__u32)); i++) { \
		(script)[A_##symbol##_used[i]] = bS_to_host(value); \
		dma_cache_sync(&(script)[A_##symbol##_used[i]], 4, DMA_TO_DEVICE); \
		DEBUG((" script, patching %s at %d to 0x%lx\n", \
		       #symbol, A_##symbol##_used[i], (value))); \
	} \
}

/* Used for patching the SCSI ID in the SELECT instruction */
#define script_patch_ID(script, symbol, value) \
{ \
	int i; \
	for(i=0; i< (sizeof(A_##symbol##_used) / sizeof(__u32)); i++) { \
		__u32 val = bS_to_cpu((script)[A_##symbol##_used[i]]); \
		val &= 0xff00ffff; \
		val |= ((value) & 0xff) << 16; \
		(script)[A_##symbol##_used[i]] = bS_to_host(val); \
		dma_cache_sync(&(script)[A_##symbol##_used[i]], 4, DMA_TO_DEVICE); \
		DEBUG((" script, patching ID field %s at %d to 0x%x\n", \
		       #symbol, A_##symbol##_used[i], val)); \
	} \
}

#define script_patch_16(script, symbol, value) \
{ \
	int i; \
	for(i=0; i< (sizeof(A_##symbol##_used) / sizeof(__u32)); i++) { \
		__u32 val = bS_to_cpu((script)[A_##symbol##_used[i]]); \
		val &= 0xffff0000; \
		val |= ((value) & 0xffff); \
		(script)[A_##symbol##_used[i]] = bS_to_host(val); \
		dma_cache_sync(&(script)[A_##symbol##_used[i]], 4, DMA_TO_DEVICE); \
		DEBUG((" script, patching short field %s at %d to 0x%x\n", \
		       #symbol, A_##symbol##_used[i], val)); \
	} \
}


static inline __u8
NCR_700_mem_readb(struct Scsi_Host *host, __u32 reg)
{
	const struct NCR_700_Host_Parameters *hostdata __attribute__((unused))
		= (struct NCR_700_Host_Parameters *)host->hostdata[0];

	return readb(host->base + (reg^bE));
}

static inline __u32
NCR_700_mem_readl(struct Scsi_Host *host, __u32 reg)
{
	__u32 value = __raw_readl(host->base + reg);
	const struct NCR_700_Host_Parameters *hostdata __attribute__((unused))
		= (struct NCR_700_Host_Parameters *)host->hostdata[0];
#if 1
	/* sanity check the register */
	if((reg & 0x3) != 0)
		BUG();
#endif

	return bS_to_cpu(value);
}

static inline void
NCR_700_mem_writeb(__u8 value, struct Scsi_Host *host, __u32 reg)
{
	const struct NCR_700_Host_Parameters *hostdata __attribute__((unused))
		= (struct NCR_700_Host_Parameters *)host->hostdata[0];

	writeb(value, host->base + (reg^bE));
}

static inline void
NCR_700_mem_writel(__u32 value, struct Scsi_Host *host, __u32 reg)
{
	const struct NCR_700_Host_Parameters *hostdata __attribute__((unused))
		= (struct NCR_700_Host_Parameters *)host->hostdata[0];

#if 1
	/* sanity check the register */
	if((reg & 0x3) != 0)
		BUG();
#endif

	__raw_writel(bS_to_host(value), host->base + reg);
}

static inline __u8
NCR_700_io_readb(struct Scsi_Host *host, __u32 reg)
{
	const struct NCR_700_Host_Parameters *hostdata __attribute__((unused))
		= (struct NCR_700_Host_Parameters *)host->hostdata[0];

	return inb(host->base + (reg^bE));
}

static inline __u32
NCR_700_io_readl(struct Scsi_Host *host, __u32 reg)
{
	__u32 value = inl(host->base + reg);
	const struct NCR_700_Host_Parameters *hostdata __attribute__((unused))
		= (struct NCR_700_Host_Parameters *)host->hostdata[0];

#if 1
	/* sanity check the register */
	if((reg & 0x3) != 0)
		BUG();
#endif

	return bS_to_cpu(value);
}

static inline void
NCR_700_io_writeb(__u8 value, struct Scsi_Host *host, __u32 reg)
{
	const struct NCR_700_Host_Parameters *hostdata __attribute__((unused))
		= (struct NCR_700_Host_Parameters *)host->hostdata[0];

	outb(value, host->base + (reg^bE));
}

static inline void
NCR_700_io_writel(__u32 value, struct Scsi_Host *host, __u32 reg)
{
	const struct NCR_700_Host_Parameters *hostdata __attribute__((unused))
		= (struct NCR_700_Host_Parameters *)host->hostdata[0];

#if 1
	/* sanity check the register */
	if((reg & 0x3) != 0)
		BUG();
#endif

	outl(bS_to_host(value), host->base + reg);
}

#ifdef CONFIG_53C700_BOTH_MAPPED

static inline __u8
NCR_700_readb(struct Scsi_Host *host, __u32 reg)
{
	__u8 val;

	const struct NCR_700_Host_Parameters *hostdata __attribute__((unused))
		= (struct NCR_700_Host_Parameters *)host->hostdata[0];

	if(hostdata->mem_mapped)
		val = NCR_700_mem_readb(host, reg);
	else
		val = NCR_700_io_readb(host, reg);

	return val;
}

static inline __u32
NCR_700_readl(struct Scsi_Host *host, __u32 reg)
{
	__u32 val;

	const struct NCR_700_Host_Parameters *hostdata __attribute__((unused))
		= (struct NCR_700_Host_Parameters *)host->hostdata[0];

	if(hostdata->mem_mapped)
		val = NCR_700_mem_readl(host, reg);
	else
		val = NCR_700_io_readl(host, reg);

	return val;
}

static inline void
NCR_700_writeb(__u8 value, struct Scsi_Host *host, __u32 reg)
{
	const struct NCR_700_Host_Parameters *hostdata __attribute__((unused))
		= (struct NCR_700_Host_Parameters *)host->hostdata[0];

	if(hostdata->mem_mapped)
		NCR_700_mem_writeb(value, host, reg);
	else
		NCR_700_io_writeb(value, host, reg);
}

static inline void
NCR_700_writel(__u32 value, struct Scsi_Host *host, __u32 reg)
{
	const struct NCR_700_Host_Parameters *hostdata __attribute__((unused))
		= (struct NCR_700_Host_Parameters *)host->hostdata[0];

	if(hostdata->mem_mapped)
		NCR_700_mem_writel(value, host, reg);
	else
		NCR_700_io_writel(value, host, reg);
}

static inline void
NCR_700_set_mem_mapped(struct NCR_700_Host_Parameters *hostdata)
{
	hostdata->mem_mapped = 1;
}

static inline void
NCR_700_set_io_mapped(struct NCR_700_Host_Parameters *hostdata)
{
	hostdata->mem_mapped = 0;
}


#elif defined(CONFIG_53C700_IO_MAPPED)

#define NCR_700_readb NCR_700_io_readb
#define NCR_700_readl NCR_700_io_readl
#define NCR_700_writeb NCR_700_io_writeb
#define NCR_700_writel NCR_700_io_writel

#define NCR_700_set_io_mapped(x)
#define NCR_700_set_mem_mapped(x)	error I/O mapped only

#elif defined(CONFIG_53C700_MEM_MAPPED)

#define NCR_700_readb NCR_700_mem_readb
#define NCR_700_readl NCR_700_mem_readl
#define NCR_700_writeb NCR_700_mem_writeb
#define NCR_700_writel NCR_700_mem_writel

#define NCR_700_set_io_mapped(x)	error MEM mapped only
#define NCR_700_set_mem_mapped(x)

#else
#error neither CONFIG_53C700_MEM_MAPPED nor CONFIG_53C700_IO_MAPPED is set
#endif

#endif
