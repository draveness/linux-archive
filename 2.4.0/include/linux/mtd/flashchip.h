
/* 
 * struct flchip definition
 * 
 * Contains information about the location and state of a given flash device 
 *
 * (C) 2000 Red Hat. GPLd.
 *
 * $Id: flashchip.h,v 1.4 2000/07/03 12:58:41 dwmw2 Exp $
 *
 */

#ifndef __MTD_FLASHCHIP_H__
#define __MTD_FLASHCHIP_H__

/* For spinlocks. sched.h includes spinlock.h from whichever directory it
 * happens to be in - so we don't have to care whether we're on 2.2, which
 * has asm/spinlock.h, or 2.4, which has linux/spinlock.h 
 */
#include <linux/sched.h>

typedef enum { 
	FL_READY,
	FL_STATUS,
	FL_CFI_QUERY,
	FL_JEDEC_QUERY,
	FL_ERASING,
	FL_ERASE_SUSPENDED,
	FL_WRITING,
	FL_WRITE_SUSPENDED,
	FL_PM_SUSPENDED,
	FL_SYNCING,
	FL_UNLOADING,
	FL_UNKNOWN
} flstate_t;



/* NOTE: confusingly, this can be used to refer to more than one chip at a time, 
   if they're interleaved. */

struct flchip {
	unsigned long start; /* Offset within the map */
	//	unsigned long len;
	/* We omit len for now, because when we group them together
	   we insist that they're all of the same size, and the chip size
	   is held in the next level up. If we get more versatile later,
	   it'll make it a damn sight harder to find which chip we want from
	   a given offset, and we'll want to add the per-chip length field
	   back in.
	*/
	flstate_t state;
	flstate_t oldstate;
	spinlock_t *mutex;
	spinlock_t _spinlock; /* We do it like this because sometimes they'll be shared. */
	wait_queue_head_t wq; /* Wait on here when we're waiting for the chip
			     to be ready */
	int word_write_time;
	int buffer_write_time;
	int erase_time;
};



#endif /* __MTD_FLASHCHIP_H__ */
