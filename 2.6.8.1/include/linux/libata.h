/*
   Copyright 2003-2004 Red Hat, Inc.  All rights reserved.
   Copyright 2003-2004 Jeff Garzik

   The contents of this file are subject to the Open
   Software License version 1.1 that can be found at
   http://www.opensource.org/licenses/osl-1.1.txt and is included herein
   by reference.

   Alternatively, the contents of this file may be used under the terms
   of the GNU General Public License version 2 (the "GPL") as distributed
   in the kernel source COPYING file, in which case the provisions of
   the GPL are applicable instead of the above.  If you wish to allow
   the use of your version of this file only under the terms of the
   GPL and not to allow others to use your version of this file under
   the OSL, indicate your decision by deleting the provisions above and
   replace them with the notice and other provisions required by the GPL.
   If you do not delete the provisions above, a recipient may use your
   version of this file under either the OSL or the GPL.

 */

#ifndef __LINUX_LIBATA_H__
#define __LINUX_LIBATA_H__

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <asm/io.h>
#include <linux/ata.h>
#include <linux/workqueue.h>

/*
 * compile-time options
 */
#undef ATA_FORCE_PIO		/* do not configure or use DMA */
#undef ATA_DEBUG		/* debugging output */
#undef ATA_VERBOSE_DEBUG	/* yet more debugging output */
#undef ATA_IRQ_TRAP		/* define to ack screaming irqs */
#undef ATA_NDEBUG		/* define to disable quick runtime checks */
#undef ATA_ENABLE_ATAPI		/* define to enable ATAPI support */
#undef ATA_ENABLE_PATA		/* define to enable PATA support in some
				 * low-level drivers */
#undef ATAPI_ENABLE_DMADIR	/* enables ATAPI DMADIR bridge support */


/* note: prints function name for you */
#ifdef ATA_DEBUG
#define DPRINTK(fmt, args...) printk(KERN_ERR "%s: " fmt, __FUNCTION__, ## args)
#ifdef ATA_VERBOSE_DEBUG
#define VPRINTK(fmt, args...) printk(KERN_ERR "%s: " fmt, __FUNCTION__, ## args)
#else
#define VPRINTK(fmt, args...)
#endif	/* ATA_VERBOSE_DEBUG */
#else
#define DPRINTK(fmt, args...)
#define VPRINTK(fmt, args...)
#endif	/* ATA_DEBUG */

#ifdef ATA_NDEBUG
#define assert(expr)
#else
#define assert(expr) \
        if(unlikely(!(expr))) {                                   \
        printk(KERN_ERR "Assertion failed! %s,%s,%s,line=%d\n", \
        #expr,__FILE__,__FUNCTION__,__LINE__);          \
        }
#endif

/* defines only for the constants which don't work well as enums */
#define ATA_TAG_POISON		0xfafbfcfdU

enum {
	/* various global constants */
	LIBATA_MAX_PRD		= ATA_MAX_PRD / 2,
	ATA_MAX_PORTS		= 8,
	ATA_DEF_QUEUE		= 1,
	ATA_MAX_QUEUE		= 1,
	ATA_MAX_SECTORS		= 200,	/* FIXME */
	ATA_MAX_BUS		= 2,
	ATA_DEF_BUSY_WAIT	= 10000,
	ATA_SHORT_PAUSE		= (HZ >> 6) + 1,

	ATA_SHT_EMULATED	= 1,
	ATA_SHT_CMD_PER_LUN	= 1,
	ATA_SHT_THIS_ID		= -1,
	ATA_SHT_USE_CLUSTERING	= 0,

	/* struct ata_device stuff */
	ATA_DFLAG_LBA48		= (1 << 0), /* device supports LBA48 */
	ATA_DFLAG_PIO		= (1 << 1), /* device currently in PIO mode */
	ATA_DFLAG_MASTER	= (1 << 2), /* is device 0? */
	ATA_DFLAG_WCACHE	= (1 << 3), /* has write cache we can
					     * (hopefully) flush? */
	ATA_DFLAG_LOCK_SECTORS	= (1 << 4), /* don't adjust max_sectors */

	ATA_DEV_UNKNOWN		= 0,	/* unknown device */
	ATA_DEV_ATA		= 1,	/* ATA device */
	ATA_DEV_ATA_UNSUP	= 2,	/* ATA device (unsupported) */
	ATA_DEV_ATAPI		= 3,	/* ATAPI device */
	ATA_DEV_ATAPI_UNSUP	= 4,	/* ATAPI device (unsupported) */
	ATA_DEV_NONE		= 5,	/* no device */

	/* struct ata_port flags */
	ATA_FLAG_SLAVE_POSS	= (1 << 1), /* host supports slave dev */
					    /* (doesn't imply presence) */
	ATA_FLAG_PORT_DISABLED	= (1 << 2), /* port is disabled, ignore it */
	ATA_FLAG_SATA		= (1 << 3),
	ATA_FLAG_NO_LEGACY	= (1 << 4), /* no legacy mode check */
	ATA_FLAG_SRST		= (1 << 5), /* use ATA SRST, not E.D.D. */
	ATA_FLAG_MMIO		= (1 << 6), /* use MMIO, not PIO */
	ATA_FLAG_SATA_RESET	= (1 << 7), /* use COMRESET */

	ATA_QCFLAG_ACTIVE	= (1 << 1), /* cmd not yet ack'd to scsi lyer */
	ATA_QCFLAG_DMA		= (1 << 2), /* data delivered via DMA */
	ATA_QCFLAG_SG		= (1 << 3), /* have s/g table? */
	ATA_QCFLAG_SINGLE	= (1 << 4), /* no s/g, just a single buffer */
	ATA_QCFLAG_DMAMAP	= ATA_QCFLAG_SG | ATA_QCFLAG_SINGLE,

	/* various lengths of time */
	ATA_TMOUT_EDD		= 5 * HZ,	/* hueristic */
	ATA_TMOUT_PIO		= 30 * HZ,
	ATA_TMOUT_BOOT		= 30 * HZ,	/* hueristic */
	ATA_TMOUT_BOOT_QUICK	= 7 * HZ,	/* hueristic */
	ATA_TMOUT_CDB		= 30 * HZ,
	ATA_TMOUT_CDB_QUICK	= 5 * HZ,

	/* ATA bus states */
	BUS_UNKNOWN		= 0,
	BUS_DMA			= 1,
	BUS_IDLE		= 2,
	BUS_NOINTR		= 3,
	BUS_NODATA		= 4,
	BUS_TIMER		= 5,
	BUS_PIO			= 6,
	BUS_EDD			= 7,
	BUS_IDENTIFY		= 8,
	BUS_PACKET		= 9,

	/* SATA port states */
	PORT_UNKNOWN		= 0,
	PORT_ENABLED		= 1,
	PORT_DISABLED		= 2,
};

enum pio_task_states {
	PIO_ST_UNKNOWN,
	PIO_ST_IDLE,
	PIO_ST_POLL,
	PIO_ST_TMOUT,
	PIO_ST,
	PIO_ST_LAST,
	PIO_ST_LAST_POLL,
	PIO_ST_ERR,
};

/* forward declarations */
struct scsi_device;
struct ata_port_operations;
struct ata_port;
struct ata_queued_cmd;

/* typedefs */
typedef int (*ata_qc_cb_t) (struct ata_queued_cmd *qc, u8 drv_stat);

struct ata_ioports {
	unsigned long		cmd_addr;
	unsigned long		data_addr;
	unsigned long		error_addr;
	unsigned long		feature_addr;
	unsigned long		nsect_addr;
	unsigned long		lbal_addr;
	unsigned long		lbam_addr;
	unsigned long		lbah_addr;
	unsigned long		device_addr;
	unsigned long		status_addr;
	unsigned long		command_addr;
	unsigned long		altstatus_addr;
	unsigned long		ctl_addr;
	unsigned long		bmdma_addr;
	unsigned long		scr_addr;
};

struct ata_probe_ent {
	struct list_head	node;
	struct pci_dev		*pdev;
	struct ata_port_operations	*port_ops;
	Scsi_Host_Template	*sht;
	struct ata_ioports	port[ATA_MAX_PORTS];
	unsigned int		n_ports;
	unsigned int		pio_mask;
	unsigned int		udma_mask;
	unsigned int		legacy_mode;
	unsigned long		irq;
	unsigned int		irq_flags;
	unsigned long		host_flags;
	void			*mmio_base;
	void			*private_data;
};

struct ata_host_set {
	spinlock_t		lock;
	struct pci_dev		*pdev;
	unsigned long		irq;
	void			*mmio_base;
	unsigned int		n_ports;
	void			*private_data;
	struct ata_port_operations *ops;
	struct ata_port *	ports[0];
};

struct ata_queued_cmd {
	struct ata_port		*ap;
	struct ata_device	*dev;

	struct scsi_cmnd	*scsicmd;
	void			(*scsidone)(struct scsi_cmnd *);

	unsigned long		flags;		/* ATA_QCFLAG_xxx */
	unsigned int		tag;
	unsigned int		n_elem;

	int			pci_dma_dir;

	unsigned int		nsect;
	unsigned int		cursect;
	unsigned int		cursg;
	unsigned int		cursg_ofs;

	struct ata_taskfile	tf;
	struct scatterlist	sgent;
	void			*buf_virt;

	struct scatterlist	*sg;

	ata_qc_cb_t		complete_fn;

	struct completion	*waiting;

	void			*private_data;
};

struct ata_host_stats {
	unsigned long		unhandled_irq;
	unsigned long		idle_irq;
	unsigned long		rw_reqbuf;
};

struct ata_device {
	u64			n_sectors;	/* size of device, if ATA */
	unsigned long		flags;		/* ATA_DFLAG_xxx */
	unsigned int		class;		/* ATA_DEV_xxx */
	unsigned int		devno;		/* 0 or 1 */
	u16			id[ATA_ID_WORDS]; /* IDENTIFY xxx DEVICE data */
	unsigned int		pio_mode;
	unsigned int		udma_mode;

	/* cache info about current transfer mode */
	u8			xfer_protocol;	/* taskfile xfer protocol */
	u8			read_cmd;	/* opcode to use on read */
	u8			write_cmd;	/* opcode to use on write */
};

struct ata_port {
	struct Scsi_Host	*host;	/* our co-allocated scsi host */
	struct ata_port_operations	*ops;
	unsigned long		flags;	/* ATA_FLAG_xxx */
	unsigned int		id;	/* unique id req'd by scsi midlyr */
	unsigned int		port_no; /* unique port #; from zero */

	struct ata_prd		*prd;	 /* our SG list */
	dma_addr_t		prd_dma; /* and its DMA mapping */

	struct ata_ioports	ioaddr;	/* ATA cmd/ctl/dma register blocks */

	u8			ctl;	/* cache of ATA control register */
	u8			last_ctl;	/* Cache last written value */
	unsigned int		bus_state;
	unsigned int		port_state;
	unsigned int		pio_mask;
	unsigned int		udma_mask;
	unsigned int		cbl;	/* cable type; ATA_CBL_xxx */

	struct ata_device	device[ATA_MAX_DEVICES];

	struct ata_queued_cmd	qcmd[ATA_MAX_QUEUE];
	unsigned long		qactive;
	unsigned int		active_tag;

	struct ata_host_stats	stats;
	struct ata_host_set	*host_set;

	struct work_struct	packet_task;

	struct work_struct	pio_task;
	unsigned int		pio_task_state;
	unsigned long		pio_task_timeout;

	void			*private_data;
};

struct ata_port_operations {
	void (*port_disable) (struct ata_port *);

	void (*dev_config) (struct ata_port *, struct ata_device *);

	void (*set_piomode) (struct ata_port *, struct ata_device *,
			     unsigned int);
	void (*set_udmamode) (struct ata_port *, struct ata_device *,
			     unsigned int);

	void (*tf_load) (struct ata_port *ap, struct ata_taskfile *tf);
	void (*tf_read) (struct ata_port *ap, struct ata_taskfile *tf);

	void (*exec_command)(struct ata_port *ap, struct ata_taskfile *tf);
	u8   (*check_status)(struct ata_port *ap);

	void (*phy_reset) (struct ata_port *ap);
	void (*post_set_mode) (struct ata_port *ap);

	void (*bmdma_setup) (struct ata_queued_cmd *qc);
	void (*bmdma_start) (struct ata_queued_cmd *qc);

	void (*qc_prep) (struct ata_queued_cmd *qc);
	int (*qc_issue) (struct ata_queued_cmd *qc);

	void (*eng_timeout) (struct ata_port *ap);

	irqreturn_t (*irq_handler)(int, void *, struct pt_regs *);
	void (*irq_clear) (struct ata_port *);

	u32 (*scr_read) (struct ata_port *ap, unsigned int sc_reg);
	void (*scr_write) (struct ata_port *ap, unsigned int sc_reg,
			   u32 val);

	int (*port_start) (struct ata_port *ap);
	void (*port_stop) (struct ata_port *ap);

	void (*host_stop) (struct ata_host_set *host_set);
};

struct ata_port_info {
	Scsi_Host_Template	*sht;
	unsigned long		host_flags;
	unsigned long		pio_mask;
	unsigned long		udma_mask;
	struct ata_port_operations	*port_ops;
};

struct pci_bits {
	unsigned int		reg;	/* PCI config register to read */
	unsigned int		width;	/* 1 (8 bit), 2 (16 bit), 4 (32 bit) */
	unsigned long		mask;
	unsigned long		val;
};

extern void ata_port_probe(struct ata_port *);
extern void sata_phy_reset(struct ata_port *ap);
extern void ata_bus_reset(struct ata_port *ap);
extern void ata_port_disable(struct ata_port *);
extern void ata_std_ports(struct ata_ioports *ioaddr);
extern int ata_pci_init_one (struct pci_dev *pdev, struct ata_port_info **port_info,
			     unsigned int n_ports);
extern void ata_pci_remove_one (struct pci_dev *pdev);
extern int ata_device_add(struct ata_probe_ent *ent);
extern int ata_scsi_detect(Scsi_Host_Template *sht);
extern int ata_scsi_queuecmd(struct scsi_cmnd *cmd, void (*done)(struct scsi_cmnd *));
extern int ata_scsi_error(struct Scsi_Host *host);
extern int ata_scsi_release(struct Scsi_Host *host);
extern unsigned int ata_host_intr(struct ata_port *ap, struct ata_queued_cmd *qc);
/*
 * Default driver ops implementations
 */
extern void ata_tf_load_pio(struct ata_port *ap, struct ata_taskfile *tf);
extern void ata_tf_load_mmio(struct ata_port *ap, struct ata_taskfile *tf);
extern void ata_tf_read_pio(struct ata_port *ap, struct ata_taskfile *tf);
extern void ata_tf_read_mmio(struct ata_port *ap, struct ata_taskfile *tf);
extern void ata_tf_to_fis(struct ata_taskfile *tf, u8 *fis, u8 pmp);
extern void ata_tf_from_fis(u8 *fis, struct ata_taskfile *tf);
extern u8 ata_check_status_pio(struct ata_port *ap);
extern u8 ata_check_status_mmio(struct ata_port *ap);
extern void ata_exec_command_pio(struct ata_port *ap, struct ata_taskfile *tf);
extern void ata_exec_command_mmio(struct ata_port *ap, struct ata_taskfile *tf);
extern int ata_port_start (struct ata_port *ap);
extern void ata_port_stop (struct ata_port *ap);
extern irqreturn_t ata_interrupt (int irq, void *dev_instance, struct pt_regs *regs);
extern void ata_qc_prep(struct ata_queued_cmd *qc);
extern int ata_qc_issue_prot(struct ata_queued_cmd *qc);
extern void ata_sg_init_one(struct ata_queued_cmd *qc, void *buf,
		unsigned int buflen);
extern void ata_sg_init(struct ata_queued_cmd *qc, struct scatterlist *sg,
		 unsigned int n_elem);
extern void ata_dev_id_string(struct ata_device *dev, unsigned char *s,
			      unsigned int ofs, unsigned int len);
extern void ata_bmdma_setup_mmio (struct ata_queued_cmd *qc);
extern void ata_bmdma_start_mmio (struct ata_queued_cmd *qc);
extern void ata_bmdma_setup_pio (struct ata_queued_cmd *qc);
extern void ata_bmdma_start_pio (struct ata_queued_cmd *qc);
extern void ata_bmdma_irq_clear(struct ata_port *ap);
extern int pci_test_config_bits(struct pci_dev *pdev, struct pci_bits *bits);
extern void ata_qc_complete(struct ata_queued_cmd *qc, u8 drv_stat);
extern void ata_eng_timeout(struct ata_port *ap);
extern int ata_std_bios_param(struct scsi_device *sdev,
			      struct block_device *bdev,
			      sector_t capacity, int geom[]);
extern int ata_scsi_slave_config(struct scsi_device *sdev);


static inline unsigned int ata_tag_valid(unsigned int tag)
{
	return (tag < ATA_MAX_QUEUE) ? 1 : 0;
}

static inline unsigned int ata_dev_present(struct ata_device *dev)
{
	return ((dev->class == ATA_DEV_ATA) ||
		(dev->class == ATA_DEV_ATAPI));
}

static inline u8 ata_chk_err(struct ata_port *ap)
{
	if (ap->flags & ATA_FLAG_MMIO) {
		return readb((void *) ap->ioaddr.error_addr);
	}
	return inb(ap->ioaddr.error_addr);
}

static inline u8 ata_chk_status(struct ata_port *ap)
{
	return ap->ops->check_status(ap);
}

static inline u8 ata_altstatus(struct ata_port *ap)
{
	if (ap->flags & ATA_FLAG_MMIO)
		return readb(ap->ioaddr.altstatus_addr);
	return inb(ap->ioaddr.altstatus_addr);
}

static inline void ata_pause(struct ata_port *ap)
{
	ata_altstatus(ap);
	ndelay(400);
}

static inline u8 ata_busy_wait(struct ata_port *ap, unsigned int bits,
			       unsigned int max)
{
	u8 status;

	do {
		udelay(10);
		status = ata_chk_status(ap);
		max--;
	} while ((status & bits) && (max > 0));

	return status;
}

static inline u8 ata_wait_idle(struct ata_port *ap)
{
	u8 status = ata_busy_wait(ap, ATA_BUSY | ATA_DRQ, 1000);

	if (status & (ATA_BUSY | ATA_DRQ)) {
		unsigned long l = ap->ioaddr.status_addr;
		printk(KERN_WARNING
		       "ATA: abnormal status 0x%X on port 0x%lX\n",
		       status, l);
	}

	return status;
}

static inline void ata_qc_set_polling(struct ata_queued_cmd *qc)
{
	qc->flags &= ~ATA_QCFLAG_DMA;
	qc->tf.ctl |= ATA_NIEN;
}

static inline struct ata_queued_cmd *ata_qc_from_tag (struct ata_port *ap,
						      unsigned int tag)
{
	if (likely(ata_tag_valid(tag)))
		return &ap->qcmd[tag];
	return NULL;
}

static inline void ata_tf_init(struct ata_port *ap, struct ata_taskfile *tf, unsigned int device)
{
	memset(tf, 0, sizeof(*tf));

	tf->ctl = ap->ctl;
	if (device == 0)
		tf->device = ATA_DEVICE_OBS;
	else
		tf->device = ATA_DEVICE_OBS | ATA_DEV1;
}

static inline u8 ata_irq_on(struct ata_port *ap)
{
	struct ata_ioports *ioaddr = &ap->ioaddr;
	u8 tmp;

	ap->ctl &= ~ATA_NIEN;
	ap->last_ctl = ap->ctl;

	if (ap->flags & ATA_FLAG_MMIO)
		writeb(ap->ctl, ioaddr->ctl_addr);
	else
		outb(ap->ctl, ioaddr->ctl_addr);
	tmp = ata_wait_idle(ap);

	ap->ops->irq_clear(ap);

	return tmp;
}

static inline u8 ata_irq_ack(struct ata_port *ap, unsigned int chk_drq)
{
	unsigned int bits = chk_drq ? ATA_BUSY | ATA_DRQ : ATA_BUSY;
	u8 host_stat, post_stat, status;

	status = ata_busy_wait(ap, bits, 1000);
	if (status & bits)
		DPRINTK("abnormal status 0x%X\n", status);

	/* get controller status; clear intr, err bits */
	if (ap->flags & ATA_FLAG_MMIO) {
		void *mmio = (void *) ap->ioaddr.bmdma_addr;
		host_stat = readb(mmio + ATA_DMA_STATUS);
		writeb(host_stat | ATA_DMA_INTR | ATA_DMA_ERR,
		       mmio + ATA_DMA_STATUS);

		post_stat = readb(mmio + ATA_DMA_STATUS);
	} else {
		host_stat = inb(ap->ioaddr.bmdma_addr + ATA_DMA_STATUS);
		outb(host_stat | ATA_DMA_INTR | ATA_DMA_ERR,
		     ap->ioaddr.bmdma_addr + ATA_DMA_STATUS);

		post_stat = inb(ap->ioaddr.bmdma_addr + ATA_DMA_STATUS);
	}

	VPRINTK("irq ack: host_stat 0x%X, new host_stat 0x%X, drv_stat 0x%X\n",
		host_stat, post_stat, status);

	return status;
}

static inline u32 scr_read(struct ata_port *ap, unsigned int reg)
{
	return ap->ops->scr_read(ap, reg);
}

static inline void scr_write(struct ata_port *ap, unsigned int reg, u32 val)
{
	ap->ops->scr_write(ap, reg, val);
}

static inline unsigned int sata_dev_present(struct ata_port *ap)
{
	return ((scr_read(ap, SCR_STATUS) & 0xf) == 0x3) ? 1 : 0;
}

static inline void ata_bmdma_stop(struct ata_port *ap)
{
	if (ap->flags & ATA_FLAG_MMIO) {
		void *mmio = (void *) ap->ioaddr.bmdma_addr;

		/* clear start/stop bit */
		writeb(readb(mmio + ATA_DMA_CMD) & ~ATA_DMA_START,
		      mmio + ATA_DMA_CMD);
	} else {
		/* clear start/stop bit */
		outb(inb(ap->ioaddr.bmdma_addr + ATA_DMA_CMD) & ~ATA_DMA_START,
		    ap->ioaddr.bmdma_addr + ATA_DMA_CMD);
	}

	/* one-PIO-cycle guaranteed wait, per spec, for HDMA1:0 transition */
	ata_altstatus(ap);	      /* dummy read */
}

static inline void ata_bmdma_ack_irq(struct ata_port *ap)
{
	if (ap->flags & ATA_FLAG_MMIO) {
		void *mmio = ((void *) ap->ioaddr.bmdma_addr) + ATA_DMA_STATUS;
		writeb(readb(mmio), mmio);
	} else {
		unsigned long addr = ap->ioaddr.bmdma_addr + ATA_DMA_STATUS;
		outb(inb(addr), addr);
	}
}

static inline u8 ata_bmdma_status(struct ata_port *ap)
{
	u8 host_stat;
	if (ap->flags & ATA_FLAG_MMIO) {
		void *mmio = (void *) ap->ioaddr.bmdma_addr;
		host_stat = readb(mmio + ATA_DMA_STATUS);
	} else
		host_stat = inb(ap->ioaddr.bmdma_addr + ATA_DMA_STATUS);
	return host_stat;
}

#endif /* __LINUX_LIBATA_H__ */
