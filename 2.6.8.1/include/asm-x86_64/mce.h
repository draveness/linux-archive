#ifndef _ASM_MCE_H
#define _ASM_MCE_H 1

#include <asm/ioctls.h>
#include <asm/types.h>

/* 
 * Machine Check support for x86
 */

#define MCG_CTL_P        (1UL<<8)   /* MCG_CAP register available */

#define MCG_STATUS_RIPV  (1UL<<0)   /* restart ip valid */
#define MCG_STATUS_EIPV  (1UL<<1)   /* eip points to correct instruction */
#define MCG_STATUS_MCIP  (1UL<<2)   /* machine check in progress */

#define MCI_STATUS_VAL   (1UL<<63)  /* valid error */
#define MCI_STATUS_OVER  (1UL<<62)  /* previous errors lost */
#define MCI_STATUS_UC    (1UL<<61)  /* uncorrected error */
#define MCI_STATUS_EN    (1UL<<60)  /* error enabled */
#define MCI_STATUS_MISCV (1UL<<59)  /* misc error reg. valid */
#define MCI_STATUS_ADDRV (1UL<<58)  /* addr reg. valid */
#define MCI_STATUS_PCC   (1UL<<57)  /* processor context corrupt */

/* Fields are zero when not available */
struct mce {
	__u64 status;
	__u64 misc;
	__u64 addr;
	__u64 mcgstatus;
	__u64 rip;	
	__u64 tsc;	/* cpu time stamp counter */
	__u64 res1;	/* for future extension */	
	__u64 res2;	/* dito. */
	__u8  cs;		/* code segment */
	__u8  bank;	/* machine check bank */
	__u8  cpu;	/* cpu that raised the error */
	__u8  finished;   /* entry is valid */
	__u32 pad;   
};

/* 
 * This structure contains all data related to the MCE log.
 * Also carries a signature to make it easier to find from external debugging tools.
 * Each entry is only valid when its finished flag is set.
 */

#define MCE_LOG_LEN 32

struct mce_log { 
	char signature[12]; /* "MACHINECHECK" */ 
	unsigned len;  	    /* = MCE_LOG_LEN */ 
	unsigned next;
	unsigned flags;
	unsigned pad0; 
	struct mce entry[MCE_LOG_LEN];
};

#define MCE_OVERFLOW 0		/* bit 0 in flags means overflow */

#define MCE_LOG_SIGNATURE 	"MACHINECHECK"

#define MCE_GET_RECORD_LEN   _IOR('M', 1, int)
#define MCE_GET_LOG_LEN      _IOR('M', 2, int)
#define MCE_GETCLEAR_FLAGS   _IOR('M', 3, int)

#endif
