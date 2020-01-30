#ifndef A2091_H
#define A2091_H

/* $Id: a2091.h,v 1.4 1997/01/19 23:07:09 davem Exp $
 *
 * Header file for the Commodore A2091 Zorro II SCSI controller for Linux
 *
 * Written and (C) 1993, Hamish Macdonald, see a2091.c for more info
 *
 */

#include <linux/types.h>

int a2091_detect(Scsi_Host_Template *);
int a2091_release(struct Scsi_Host *);
const char *wd33c93_info(void);
int wd33c93_queuecommand(Scsi_Cmnd *, void (*done)(Scsi_Cmnd *));
int wd33c93_abort(Scsi_Cmnd *);
int wd33c93_reset(Scsi_Cmnd *, unsigned int);

#ifndef NULL
#define NULL 0
#endif

#ifndef CMD_PER_LUN
#define CMD_PER_LUN 2
#endif

#ifndef CAN_QUEUE
#define CAN_QUEUE 16
#endif

#define A2091_SCSI {  proc_name:	   "A2901", \
		      name:                "Commodore A2091/A590 SCSI", \
		      detect:              a2091_detect,    \
		      release:             a2091_release,   \
		      queuecommand:        wd33c93_queuecommand, \
		      abort:               wd33c93_abort,   \
		      reset:               wd33c93_reset,   \
		      can_queue:           CAN_QUEUE,       \
		      this_id:             7,               \
		      sg_tablesize:        SG_ALL,          \
		      cmd_per_lun:	   CMD_PER_LUN,     \
		      use_clustering:      DISABLE_CLUSTERING }

/*
 * if the transfer address ANDed with this results in a non-zero
 * result, then we can't use DMA.
 */
#define A2091_XFER_MASK  (0xff000001)

typedef struct {
             unsigned char      pad1[64];
    volatile unsigned short     ISTR;
    volatile unsigned short     CNTR;
             unsigned char      pad2[60];
    volatile unsigned int       WTC;
    volatile unsigned long      ACR;
             unsigned char      pad3[6];
    volatile unsigned short     DAWR;
             unsigned char      pad4;
    volatile unsigned char      SASR;
             unsigned char      pad5;
    volatile unsigned char      SCMD;
             unsigned char      pad6[76];
    volatile unsigned short     ST_DMA;
    volatile unsigned short     SP_DMA;
    volatile unsigned short     CINT;
             unsigned char      pad7[2];
    volatile unsigned short     FLUSH;
} a2091_scsiregs;

#define DAWR_A2091		(3)

/* CNTR bits. */
#define CNTR_TCEN		(1<<7)
#define CNTR_PREST		(1<<6)
#define CNTR_PDMD		(1<<5)
#define CNTR_INTEN		(1<<4)
#define CNTR_DDIR		(1<<3)

/* ISTR bits. */
#define ISTR_INTX		(1<<8)
#define ISTR_INT_F		(1<<7)
#define ISTR_INTS		(1<<6)
#define ISTR_E_INT		(1<<5)
#define ISTR_INT_P		(1<<4)
#define ISTR_UE_INT		(1<<3)
#define ISTR_OE_INT		(1<<2)
#define ISTR_FF_FLG		(1<<1)
#define ISTR_FE_FLG		(1<<0)

#endif /* A2091_H */
