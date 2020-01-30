/*
 * File: 	mca.h
 * Purpose: 	Machine check handling specific defines
 *
 * Copyright (C) 1999 Silicon Graphics, Inc.
 * Copyright (C) Vijay Chander (vijay@engr.sgi.com)
 * Copyright (C) Srinivasa Thirumalachar (sprasad@engr.sgi.com)
 */

/* XXX use this temporary define for MP systems trying to INIT */
#define SAL_MPINIT_WORKAROUND

#ifndef _ASM_IA64_MCA_H
#define _ASM_IA64_MCA_H

#if !defined(__ASSEMBLY__)
#include <linux/types.h>
#include <asm/param.h>
#include <asm/sal.h>
#include <asm/processor.h>
#include <asm/hw_irq.h>

/* These are the return codes from all the IA64_MCA specific interfaces */
typedef	int ia64_mca_return_code_t;

enum {
	IA64_MCA_SUCCESS	=	0,
	IA64_MCA_FAILURE	=	1
};

#define IA64_MCA_RENDEZ_TIMEOUT		(100 * HZ)	/* 1000 milliseconds */

/* Interrupt vectors reserved for MC handling. */
#define IA64_MCA_RENDEZ_INT_VECTOR	MCA_RENDEZ_IRQ	/* Rendez interrupt */
#define IA64_MCA_WAKEUP_INT_VECTOR	MCA_WAKEUP_IRQ	/* Wakeup interrupt */
#define IA64_MCA_CMC_INT_VECTOR		CMC_IRQ	/* Correctable machine check interrupt */

#define IA64_CMC_INT_DISABLE		0
#define IA64_CMC_INT_ENABLE		1


typedef u32 int_vector_t;
typedef u64 millisec_t;

typedef union cmcv_reg_u {
	u64	cmcv_regval;
	struct	{
		u64  	cmcr_vector		: 8;
		u64	cmcr_reserved1		: 4;
		u64	cmcr_ignored1		: 1;
		u64	cmcr_reserved2		: 3;
		u64	cmcr_mask		: 1;
		u64	cmcr_ignored2		: 47;
	} cmcv_reg_s;

} cmcv_reg_t;

#define cmcv_mask		cmcv_reg_s.cmcr_mask
#define cmcv_vector		cmcv_reg_s.cmcr_vector


#define IA64_MCA_UCMC_HANDLER_SIZE	0x10
#define IA64_INIT_HANDLER_SIZE		0x10

enum {
	IA64_MCA_RENDEZ_CHECKIN_NOTDONE	= 	0x0,
	IA64_MCA_RENDEZ_CHECKIN_DONE 	= 	0x1
};

#define IA64_MAXCPUS	64	/* Need to do something about this */

/* Information maintained by the MC infrastructure */
typedef struct ia64_mc_info_s {
	u64		imi_mca_handler;		
	size_t		imi_mca_handler_size;
	u64		imi_monarch_init_handler;
	size_t		imi_monarch_init_handler_size;
	u64		imi_slave_init_handler;
	size_t		imi_slave_init_handler_size;
	u8		imi_rendez_checkin[IA64_MAXCPUS];

} ia64_mc_info_t;

/* Possible rendez states passed from SAL to OS during MCA
 * handoff
 */
enum {
	IA64_MCA_RENDEZ_NOT_RQD 		= 	0x0,
	IA64_MCA_RENDEZ_DONE_WITHOUT_INIT	=	0x1,
	IA64_MCA_RENDEZ_DONE_WITH_INIT		=	0x2,
	IA64_MCA_RENDEZ_FAILURE			=	-1
};

typedef struct ia64_mca_sal_to_os_state_s {
	u64		imsto_os_gp;		/* GP of the os registered with the SAL */
	u64		imsto_pal_proc;		/* PAL_PROC entry point - physical addr */
	u64		imsto_sal_proc;		/* SAL_PROC entry point - physical addr */
	u64		imsto_sal_gp;		/* GP of the SAL - physical */
	u64		imsto_rendez_state;	/* Rendez state information */
	u64		imsto_sal_check_ra;	/* Return address in SAL_CHECK while going
						 * back to SAL from OS after MCA handling.
						 */
} ia64_mca_sal_to_os_state_t;

enum {
	IA64_MCA_CORRECTED 	= 	0x0,	/* Error has been corrected by OS_MCA */
	IA64_MCA_WARM_BOOT	=	-1,	/* Warm boot of the system need from SAL */
	IA64_MCA_COLD_BOOT	=	-2,	/* Cold boot of the system need from SAL */
	IA64_MCA_HALT		=	-3	/* System to be halted by SAL */
};
	
typedef struct ia64_mca_os_to_sal_state_s {
	u64		imots_os_status;	/*   OS status to SAL as to what happened
						 *   with the MCA handling.
						 */
	u64		imots_sal_gp;		/* GP of the SAL - physical */
	u64		imots_new_min_state;	/* Pointer to structure containing
						 * new values of registers in the min state
						 * save area.
						 */
	u64		imots_sal_check_ra;	/* Return address in SAL_CHECK while going
						 * back to SAL from OS after MCA handling.
						 */
} ia64_mca_os_to_sal_state_t;

typedef int (*prfunc_t)(const char * fmt, ...);

extern void ia64_mca_init(void);
extern void ia64_os_mca_dispatch(void);
extern void ia64_os_mca_dispatch_end(void);
extern void ia64_mca_ucmc_handler(void);
extern void ia64_monarch_init_handler(void);
extern void ia64_slave_init_handler(void);
extern void ia64_mca_rendez_int_handler(int,void *,struct pt_regs *);
extern void ia64_mca_wakeup_int_handler(int,void *,struct pt_regs *);
extern void ia64_mca_cmc_int_handler(int,void *,struct pt_regs *);
extern void ia64_log_print(int,int,prfunc_t);

#define PLATFORM_CALL(fn, args)	printk("Platform call TBD\n")

#undef 	MCA_TEST

#define IA64_MCA_DEBUG_INFO 1

#if defined(IA64_MCA_DEBUG_INFO)
# define IA64_MCA_DEBUG	printk
#else
# define IA64_MCA_DEBUG
#endif
#endif /* !__ASSEMBLY__ */
#endif /* _ASM_IA64_MCA_H */
