/*
 * URB OHCI HCD (Host Controller Driver) for USB.
 * 
 * (C) Copyright 1999 Roman Weissgaerber <weissg@vienna.at>
 * (C) Copyright 2000 David Brownell <david-b@pacbell.net>
 * 
 * usb-ohci.h
 */

 
static int cc_to_error[16] = { 

/* mapping of the OHCI CC status to error codes */ 
	/* No  Error  */               USB_ST_NOERROR,
	/* CRC Error  */               USB_ST_CRC,
	/* Bit Stuff  */               USB_ST_BITSTUFF,
	/* Data Togg  */               USB_ST_CRC,
	/* Stall      */               USB_ST_STALL,
	/* DevNotResp */               USB_ST_NORESPONSE,
	/* PIDCheck   */               USB_ST_BITSTUFF,
	/* UnExpPID   */               USB_ST_BITSTUFF,
	/* DataOver   */               USB_ST_DATAOVERRUN,
	/* DataUnder  */               USB_ST_DATAUNDERRUN,
	/* reservd    */               USB_ST_NORESPONSE,
	/* reservd    */               USB_ST_NORESPONSE,
	/* BufferOver */               USB_ST_BUFFEROVERRUN,
	/* BuffUnder  */               USB_ST_BUFFERUNDERRUN,
	/* Not Access */               USB_ST_NORESPONSE,
	/* Not Access */               USB_ST_NORESPONSE 
};


/* ED States */

#define ED_NEW 		0x00
#define ED_UNLINK 	0x01
#define ED_OPER		0x02
#define ED_DEL		0x04
#define ED_URB_DEL  	0x08

/* usb_ohci_ed */
typedef struct ed {
	__u32 hwINFO;       
	__u32 hwTailP;
	__u32 hwHeadP;
	__u32 hwNextED;

	struct ed * ed_prev;  
	__u8 int_period;
	__u8 int_branch;
	__u8 int_load; 
	__u8 int_interval;
	__u8 state;
	__u8 type; 
	__u16 last_iso;
    struct ed * ed_rm_list;
   
} ed_t;

 
/* TD info field */
#define TD_CC       0xf0000000
#define TD_CC_GET(td_p) ((td_p >>28) & 0x0f)
#define TD_CC_SET(td_p, cc) (td_p) = ((td_p) & 0x0fffffff) | (((cc) & 0x0f) << 28)
#define TD_EC       0x0C000000
#define TD_T        0x03000000
#define TD_T_DATA0  0x02000000
#define TD_T_DATA1  0x03000000
#define TD_T_TOGGLE 0x00000000
#define TD_R        0x00040000
#define TD_DI       0x00E00000
#define TD_DI_SET(X) (((X) & 0x07)<< 21)
#define TD_DP       0x00180000
#define TD_DP_SETUP 0x00000000
#define TD_DP_IN    0x00100000
#define TD_DP_OUT   0x00080000

#define TD_ISO		0x00010000
#define TD_DEL      0x00020000

/* CC Codes */
#define TD_CC_NOERROR      0x00
#define TD_CC_CRC          0x01
#define TD_CC_BITSTUFFING  0x02
#define TD_CC_DATATOGGLEM  0x03
#define TD_CC_STALL        0x04
#define TD_DEVNOTRESP      0x05
#define TD_PIDCHECKFAIL    0x06
#define TD_UNEXPECTEDPID   0x07
#define TD_DATAOVERRUN     0x08
#define TD_DATAUNDERRUN    0x09
#define TD_BUFFEROVERRUN   0x0C
#define TD_BUFFERUNDERRUN  0x0D
#define TD_NOTACCESSED     0x0F


#define MAXPSW 1

typedef struct td { 
	__u32 hwINFO;
  	__u32 hwCBP;		/* Current Buffer Pointer */
  	__u32 hwNextTD;		/* Next TD Pointer */
  	__u32 hwBE;		/* Memory Buffer End Pointer */
  	__u16 hwPSW[MAXPSW];

  	__u8 unused;
  	__u8 index;
  	struct ed * ed;
  	struct td * next_dl_td;
  	urb_t * urb;
} td_t;


#define OHCI_ED_SKIP	(1 << 14)

/*
 * The HCCA (Host Controller Communications Area) is a 256 byte
 * structure defined in the OHCI spec. that the host controller is
 * told the base address of.  It must be 256-byte aligned.
 */
 
#define NUM_INTS 32	/* part of the OHCI standard */
struct ohci_hcca {
    __u32	int_table[NUM_INTS];	/* Interrupt ED table */
	__u16	frame_no;		/* current frame number */
	__u16	pad1;			/* set to 0 on each frame_no change */
	__u32	done_head;		/* info returned for an interrupt */
	u8		reserved_for_hc[116];
} __attribute((aligned(256)));

  
/*
 * Maximum number of root hub ports.  
 */
#define MAX_ROOT_PORTS	15	/* maximum OHCI root hub ports */

/*
 * This is the structure of the OHCI controller's memory mapped I/O
 * region.  This is Memory Mapped I/O.  You must use the readl() and
 * writel() macros defined in asm/io.h to access these!!
 */
struct ohci_regs {
	/* control and status registers */
	__u32	revision;
	__u32	control;
	__u32	cmdstatus;
	__u32	intrstatus;
	__u32	intrenable;
	__u32	intrdisable;
	/* memory pointers */
	__u32	hcca;
	__u32	ed_periodcurrent;
	__u32	ed_controlhead;
	__u32	ed_controlcurrent;
	__u32	ed_bulkhead;
	__u32	ed_bulkcurrent;
	__u32	donehead;
	/* frame counters */
	__u32	fminterval;
	__u32	fmremaining;
	__u32	fmnumber;
	__u32	periodicstart;
	__u32	lsthresh;
	/* Root hub ports */
	struct	ohci_roothub_regs {
		__u32	a;
		__u32	b;
		__u32	status;
		__u32	portstatus[MAX_ROOT_PORTS];
	} roothub;
} __attribute((aligned(32)));


/* OHCI CONTROL AND STATUS REGISTER MASKS */

/*
 * HcControl (control) register masks
 */
#define OHCI_CTRL_CBSR	(3 << 0)	/* control/bulk service ratio */
#define OHCI_CTRL_PLE	(1 << 2)	/* periodic list enable */
#define OHCI_CTRL_IE	(1 << 3)	/* isochronous enable */
#define OHCI_CTRL_CLE	(1 << 4)	/* control list enable */
#define OHCI_CTRL_BLE	(1 << 5)	/* bulk list enable */
#define OHCI_CTRL_HCFS	(3 << 6)	/* host controller functional state */
#define OHCI_CTRL_IR	(1 << 8)	/* interrupt routing */
#define OHCI_CTRL_RWC	(1 << 9)	/* remote wakeup connected */
#define OHCI_CTRL_RWE	(1 << 10)	/* remote wakeup enable */

/* pre-shifted values for HCFS */
#	define OHCI_USB_RESET	(0 << 6)
#	define OHCI_USB_RESUME	(1 << 6)
#	define OHCI_USB_OPER	(2 << 6)
#	define OHCI_USB_SUSPEND	(3 << 6)

/*
 * HcCommandStatus (cmdstatus) register masks
 */
#define OHCI_HCR	(1 << 0)	/* host controller reset */
#define OHCI_CLF  	(1 << 1)	/* control list filled */
#define OHCI_BLF  	(1 << 2)	/* bulk list filled */
#define OHCI_OCR  	(1 << 3)	/* ownership change request */
#define OHCI_SOC  	(3 << 16)	/* scheduling overrun count */

/*
 * masks used with interrupt registers:
 * HcInterruptStatus (intrstatus)
 * HcInterruptEnable (intrenable)
 * HcInterruptDisable (intrdisable)
 */
#define OHCI_INTR_SO	(1 << 0)	/* scheduling overrun */
#define OHCI_INTR_WDH	(1 << 1)	/* writeback of done_head */
#define OHCI_INTR_SF	(1 << 2)	/* start frame */
#define OHCI_INTR_RD	(1 << 3)	/* resume detect */
#define OHCI_INTR_UE	(1 << 4)	/* unrecoverable error */
#define OHCI_INTR_FNO	(1 << 5)	/* frame number overflow */
#define OHCI_INTR_RHSC	(1 << 6)	/* root hub status change */
#define OHCI_INTR_OC	(1 << 30)	/* ownership change */
#define OHCI_INTR_MIE	(1 << 31)	/* master interrupt enable */



/* Virtual Root HUB */
struct virt_root_hub {
	int devnum; /* Address of Root Hub endpoint */ 
	void * urb;
	void * int_addr;
	int send;
	int interval;
	struct timer_list rh_int_timer;
};


/* USB HUB CONSTANTS (not OHCI-specific; see hub.h) */
 
/* destination of request */
#define RH_INTERFACE               0x01
#define RH_ENDPOINT                0x02
#define RH_OTHER                   0x03

#define RH_CLASS                   0x20
#define RH_VENDOR                  0x40

/* Requests: bRequest << 8 | bmRequestType */
#define RH_GET_STATUS           0x0080
#define RH_CLEAR_FEATURE        0x0100
#define RH_SET_FEATURE          0x0300
#define RH_SET_ADDRESS		0x0500
#define RH_GET_DESCRIPTOR	0x0680
#define RH_SET_DESCRIPTOR       0x0700
#define RH_GET_CONFIGURATION	0x0880
#define RH_SET_CONFIGURATION	0x0900
#define RH_GET_STATE            0x0280
#define RH_GET_INTERFACE        0x0A80
#define RH_SET_INTERFACE        0x0B00
#define RH_SYNC_FRAME           0x0C80
/* Our Vendor Specific Request */
#define RH_SET_EP               0x2000


/* Hub port features */
#define RH_PORT_CONNECTION         0x00
#define RH_PORT_ENABLE             0x01
#define RH_PORT_SUSPEND            0x02
#define RH_PORT_OVER_CURRENT       0x03
#define RH_PORT_RESET              0x04
#define RH_PORT_POWER              0x08
#define RH_PORT_LOW_SPEED          0x09

#define RH_C_PORT_CONNECTION       0x10
#define RH_C_PORT_ENABLE           0x11
#define RH_C_PORT_SUSPEND          0x12
#define RH_C_PORT_OVER_CURRENT     0x13
#define RH_C_PORT_RESET            0x14  

/* Hub features */
#define RH_C_HUB_LOCAL_POWER       0x00
#define RH_C_HUB_OVER_CURRENT      0x01

#define RH_DEVICE_REMOTE_WAKEUP    0x00
#define RH_ENDPOINT_STALL          0x01

#define RH_ACK                     0x01
#define RH_REQ_ERR                 -1
#define RH_NACK                    0x00


/* OHCI ROOT HUB REGISTER MASKS */
 
/* roothub.portstatus [i] bits */
#define RH_PS_CCS            0x00000001   	/* current connect status */
#define RH_PS_PES            0x00000002   	/* port enable status*/
#define RH_PS_PSS            0x00000004   	/* port suspend status */
#define RH_PS_POCI           0x00000008   	/* port over current indicator */
#define RH_PS_PRS            0x00000010  	/* port reset status */
#define RH_PS_PPS            0x00000100   	/* port power status */
#define RH_PS_LSDA           0x00000200    	/* low speed device attached */
#define RH_PS_CSC            0x00010000 	/* connect status change */
#define RH_PS_PESC           0x00020000   	/* port enable status change */
#define RH_PS_PSSC           0x00040000    	/* port suspend status change */
#define RH_PS_OCIC           0x00080000    	/* over current indicator change */
#define RH_PS_PRSC           0x00100000   	/* port reset status change */

/* roothub.status bits */
#define RH_HS_LPS	     0x00000001		/* local power status */
#define RH_HS_OCI	     0x00000002		/* over current indicator */
#define RH_HS_DRWE	     0x00008000		/* device remote wakeup enable */
#define RH_HS_LPSC	     0x00010000		/* local power status change */
#define RH_HS_OCIC	     0x00020000		/* over current indicator change */
#define RH_HS_CRWE	     0x80000000		/* clear remote wakeup enable */

/* roothub.b masks */
#define RH_B_DR		0x0000ffff		/* device removable flags */
#define RH_B_PPCM	0xffff0000		/* port power control mask */

/* roothub.a masks */
#define	RH_A_NDP	(0xff << 0)		/* number of downstream ports */
#define	RH_A_PSM	(1 << 8)		/* power switching mode */
#define	RH_A_NPS	(1 << 9)		/* no power switching */
#define	RH_A_DT		(1 << 10)		/* device type (mbz) */
#define	RH_A_OCPM	(1 << 11)		/* over current protection mode */
#define	RH_A_NOCP	(1 << 12)		/* no over current protection */
#define	RH_A_POTPGT	(0xff << 24)		/* power on to power good time */

#define min(a,b) (((a)<(b))?(a):(b))  
 

/* urb */
typedef struct 
{
	ed_t * ed;
	__u16 length;	// number of tds associated with this request
	__u16 td_cnt;	// number of tds already serviced
	int   state;
	wait_queue_head_t * wait;
	td_t * td[0];	// list pointer to all corresponding TDs associated with this request

} urb_priv_t;
#define URB_DEL 1

/*
 * This is the full ohci controller description
 *
 * Note how the "proper" USB information is just
 * a subset of what the full implementation needs. (Linus)
 */


typedef struct ohci {
	struct ohci_hcca hcca;		/* hcca */

	int irq;
	int disabled;			/* e.g. got a UE, we're hung */
	atomic_t resume_count;		/* defending against multiple resumes */

	struct ohci_regs * regs;	/* OHCI controller's memory */
	struct list_head ohci_hcd_list;	/* list of all ohci_hcd */

	struct ohci * next; 		// chain of uhci device contexts
	// struct list_head urb_list; 	// list of all pending urbs
	// spinlock_t urb_list_lock; 	// lock to keep consistency 
  
	int ohci_int_load[32];		/* load of the 32 Interrupt Chains (for load balancing)*/
	ed_t * ed_rm_list[2];     /* lists of all endpoints to be removed */
	ed_t * ed_bulktail;       /* last endpoint of bulk list */
	ed_t * ed_controltail;    /* last endpoint of control list */
 	ed_t * ed_isotail;        /* last endpoint of iso list */
	int intrstatus;
	__u32 hc_control;		/* copy of the hc control reg */
	struct usb_bus * bus;    
	struct usb_device * dev[128];
	struct virt_root_hub rh;

	/* PCI device handle and settings */
	struct pci_dev	*ohci_dev;
	u8		pci_latency;
} ohci_t;


#define NUM_TDS	0		/* num of preallocated transfer descriptors */
#define NUM_EDS 32		/* num of preallocated endpoint descriptors */

struct ohci_device {
	ed_t 	ed[NUM_EDS];
	int ed_cnt;
	wait_queue_head_t * wait;
};

// #define ohci_to_usb(ohci)	((ohci)->usb)
#define usb_to_ohci(usb)	((struct ohci_device *)(usb)->hcpriv)

/* hcd */
/* endpoint */
static int ep_link(ohci_t * ohci, ed_t * ed);
static int ep_unlink(ohci_t * ohci, ed_t * ed);
static ed_t * ep_add_ed(struct usb_device * usb_dev, unsigned int pipe, int interval, int load);
static void ep_rm_ed(struct usb_device * usb_dev, ed_t * ed);
/* td */
static void td_fill(unsigned int info, void * data, int len, urb_t * urb, int index);
static void td_submit_urb(urb_t * urb);
/* root hub */
static int rh_submit_urb(urb_t * urb);
static int rh_unlink_urb(urb_t * urb);
static int rh_init_int_timer(urb_t * urb);

/*-------------------------------------------------------------------------*/

#define ALLOC_FLAGS (in_interrupt () ? GFP_ATOMIC : GFP_KERNEL)
 
#ifdef OHCI_MEM_SLAB
#define	__alloc(t,c) kmem_cache_alloc(c,ALLOC_FLAGS)
#define	__free(c,x) kmem_cache_free(c,x)
static kmem_cache_t *td_cache, *ed_cache;

/*
 * WARNING:  do NOT use this with "forced slab debug"; it won't respect
 * our hardware alignment requirement.
 */
#ifndef OHCI_MEM_FLAGS
#define	OHCI_MEM_FLAGS 0
#endif

static int ohci_mem_init (void)
{
	/* redzoning (or forced debug!) breaks alignment */
	int	flags = (OHCI_MEM_FLAGS) & ~SLAB_RED_ZONE;

	/* TDs accessed by controllers and host */
	td_cache = kmem_cache_create ("ohci_td", sizeof (struct td), 0,
		flags | SLAB_HWCACHE_ALIGN, NULL, NULL);
	if (!td_cache) {
		dbg ("no TD cache?");
		return -ENOMEM;
	}

	/* EDs are accessed by controllers and host;  dev part is host-only */
	ed_cache = kmem_cache_create ("ohci_ed", sizeof (struct ohci_device), 0,
		flags | SLAB_HWCACHE_ALIGN, NULL, NULL);
	if (!ed_cache) {
		dbg ("no ED cache?");
		kmem_cache_destroy (td_cache);
		td_cache = 0;
		return -ENOMEM;
	}
	dbg ("slab flags 0x%x", flags);
	return 0;
}

static void ohci_mem_cleanup (void)
{
	if (ed_cache && kmem_cache_destroy (ed_cache))
		err ("ed_cache remained");
	ed_cache = 0;

	if (td_cache && kmem_cache_destroy (td_cache))
		err ("td_cache remained");
	td_cache = 0;
}

#else
#define	__alloc(t,c) kmalloc(sizeof(t),ALLOC_FLAGS)
#define	__free(dev,x) kfree(x)
#define td_cache 0
#define ed_cache 0

static inline int ohci_mem_init (void) { return 0; }
static inline void ohci_mem_cleanup (void) { return; }

/* FIXME: pci_consistent version */

#endif


/* TDs ... */
static inline struct td *
td_alloc (struct ohci *hc)
{
	struct td *td = (struct td *) __alloc (struct td, td_cache);
	return td;
}

static inline void
td_free (struct ohci *hc, struct td *td)
{
	__free (td_cache, td);
}


/* DEV + EDs ... only the EDs need to be consistent */
static inline struct ohci_device *
dev_alloc (struct ohci *hc)
{
	struct ohci_device *dev = (struct ohci_device *)
		__alloc (struct ohci_device, ed_cache);
	return dev;
}

static inline void
dev_free (struct ohci_device *dev)
{
	__free (ed_cache, dev);
}
