#ifndef _CSS_H
#define _CSS_H

#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/device.h>
#include <linux/types.h>

#include <asm/cio.h>
#include <asm/chpid.h>

#include "schid.h"

/*
 * path grouping stuff
 */
#define SPID_FUNC_SINGLE_PATH	   0x00
#define SPID_FUNC_MULTI_PATH	   0x80
#define SPID_FUNC_ESTABLISH	   0x00
#define SPID_FUNC_RESIGN	   0x40
#define SPID_FUNC_DISBAND	   0x20

#define SNID_STATE1_RESET	   0
#define SNID_STATE1_UNGROUPED	   2
#define SNID_STATE1_GROUPED	   3

#define SNID_STATE2_NOT_RESVD	   0
#define SNID_STATE2_RESVD_ELSE	   2
#define SNID_STATE2_RESVD_SELF	   3

#define SNID_STATE3_MULTI_PATH	   1
#define SNID_STATE3_SINGLE_PATH	   0

struct path_state {
	__u8  state1 : 2;	/* path state value 1 */
	__u8  state2 : 2;	/* path state value 2 */
	__u8  state3 : 1;	/* path state value 3 */
	__u8  resvd  : 3;	/* reserved */
} __attribute__ ((packed));

struct extended_cssid {
	u8 version;
	u8 cssid;
} __attribute__ ((packed));

struct pgid {
	union {
		__u8 fc;   	/* SPID function code */
		struct path_state ps;	/* SNID path state */
	} __attribute__ ((packed)) inf;
	union {
		__u32 cpu_addr	: 16;	/* CPU address */
		struct extended_cssid ext_cssid;
	} __attribute__ ((packed)) pgid_high;
	__u32 cpu_id	: 24;	/* CPU identification */
	__u32 cpu_model : 16;	/* CPU model */
	__u32 tod_high;		/* high word TOD clock */
} __attribute__ ((packed));

#define MAX_CIWS 8

/*
 * sense-id response buffer layout
 */
struct senseid {
	/* common part */
	__u8  reserved;     	/* always 0x'FF' */
	__u16 cu_type;	     	/* control unit type */
	__u8  cu_model;     	/* control unit model */
	__u16 dev_type;     	/* device type */
	__u8  dev_model;    	/* device model */
	__u8  unused;	     	/* padding byte */
	/* extended part */
	struct ciw ciw[MAX_CIWS];	/* variable # of CIWs */
}  __attribute__ ((packed,aligned(4)));

struct ccw_device_private {
	struct ccw_device *cdev;
	struct subchannel *sch;
	int state;		/* device state */
	atomic_t onoff;
	unsigned long registered;
	struct ccw_dev_id dev_id;	/* device id */
	struct subchannel_id schid;	/* subchannel number */
	__u8 imask;		/* lpm mask for SNID/SID/SPGID */
	int iretry;		/* retry counter SNID/SID/SPGID */
	struct {
		unsigned int fast:1;	/* post with "channel end" */
		unsigned int repall:1;	/* report every interrupt status */
		unsigned int pgroup:1;  /* do path grouping */
		unsigned int force:1;   /* allow forced online */
	} __attribute__ ((packed)) options;
	struct {
		unsigned int pgid_single:1; /* use single path for Set PGID */
		unsigned int esid:1;        /* Ext. SenseID supported by HW */
		unsigned int dosense:1;	    /* delayed SENSE required */
		unsigned int doverify:1;    /* delayed path verification */
		unsigned int donotify:1;    /* call notify function */
		unsigned int recog_done:1;  /* dev. recog. complete */
		unsigned int fake_irb:1;    /* deliver faked irb */
		unsigned int intretry:1;    /* retry internal operation */
	} __attribute__((packed)) flags;
	unsigned long intparm;	/* user interruption parameter */
	struct qdio_irq *qdio_data;
	struct irb irb;		/* device status */
	struct senseid senseid;	/* SenseID info */
	struct pgid pgid[8];	/* path group IDs per chpid*/
	struct ccw1 iccws[2];	/* ccws for SNID/SID/SPGID commands */
	struct work_struct kick_work;
	wait_queue_head_t wait_q;
	struct timer_list timer;
	void *cmb;			/* measurement information */
	struct list_head cmb_list;	/* list of measured devices */
	u64 cmb_start_time;		/* clock value of cmb reset */
	void *cmb_wait;			/* deferred cmb enable/disable */
};

/*
 * A css driver handles all subchannels of one type.
 * Currently, we only care about I/O subchannels (type 0), these
 * have a ccw_device connected to them.
 */
struct subchannel;
struct css_driver {
	unsigned int subchannel_type;
	struct device_driver drv;
	void (*irq)(struct device *);
	int (*notify)(struct device *, int);
	void (*verify)(struct device *);
	void (*termination)(struct device *);
	int (*probe)(struct subchannel *);
	int (*remove)(struct subchannel *);
	void (*shutdown)(struct subchannel *);
};

/*
 * all css_drivers have the css_bus_type
 */
extern struct bus_type css_bus_type;

extern void css_sch_device_unregister(struct subchannel *);
extern struct subchannel * get_subchannel_by_schid(struct subchannel_id);
extern int css_init_done;
extern int for_each_subchannel(int(*fn)(struct subchannel_id, void *), void *);
extern void css_process_crw(int, int);
extern void css_reiterate_subchannels(void);
void css_update_ssd_info(struct subchannel *sch);

#define __MAX_SUBCHANNEL 65535
#define __MAX_SSID 3

struct channel_subsystem {
	u8 cssid;
	int valid;
	struct channel_path *chps[__MAX_CHPID + 1];
	struct device device;
	struct pgid global_pgid;
	struct mutex mutex;
	/* channel measurement related */
	int cm_enabled;
	void *cub_addr1;
	void *cub_addr2;
	/* for orphaned ccw devices */
	struct subchannel *pseudo_subchannel;
};
#define to_css(dev) container_of(dev, struct channel_subsystem, device)

extern struct bus_type css_bus_type;
extern struct channel_subsystem *css[];

/* Some helper functions for disconnected state. */
int device_is_disconnected(struct subchannel *);
void device_set_disconnected(struct subchannel *);
void device_trigger_reprobe(struct subchannel *);

/* Helper functions for vary on/off. */
int device_is_online(struct subchannel *);
void device_kill_io(struct subchannel *);
void device_set_intretry(struct subchannel *sch);
int device_trigger_verify(struct subchannel *sch);

/* Machine check helper function. */
void device_kill_pending_timer(struct subchannel *);

/* Helper functions to build lists for the slow path. */
void css_schedule_eval(struct subchannel_id schid);
void css_schedule_eval_all(void);

int sch_is_pseudo_sch(struct subchannel *);

extern struct workqueue_struct *slow_path_wq;

int subchannel_add_files (struct device *);
extern struct attribute_group *subch_attr_groups[];
#endif
