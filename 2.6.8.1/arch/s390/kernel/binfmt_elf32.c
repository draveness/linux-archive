/*
 * Support for 32-bit Linux for S390 ELF binaries.
 *
 * Copyright (C) 2000 IBM Deutschland Entwicklung GmbH, IBM Corporation
 * Author(s): Gerhard Tonn (ton@de.ibm.com)
 *
 * Heavily inspired by the 32-bit Sparc compat code which is
 * Copyright (C) 1995, 1996, 1997, 1998 David S. Miller (davem@redhat.com)
 * Copyright (C) 1995, 1996, 1997, 1998 Jakub Jelinek   (jj@ultra.linux.cz)
 */

#define __ASMS390_ELF_H

#include <linux/time.h>

/*
 * These are used to set parameters in the core dumps.
 */
#define ELF_CLASS	ELFCLASS32
#define ELF_DATA	ELFDATA2MSB
#define ELF_ARCH	EM_S390

/*
 * This is used to ensure we don't load something for the wrong architecture.
 */
#define elf_check_arch(x) \
	(((x)->e_machine == EM_S390 || (x)->e_machine == EM_S390_OLD) \
         && (x)->e_ident[EI_CLASS] == ELF_CLASS)

/* ELF register definitions */
#define NUM_GPRS      16
#define NUM_FPRS      16
#define NUM_ACRS      16    

/* For SVR4/S390 the function pointer to be registered with `atexit` is
   passed in R14. */
#define ELF_PLAT_INIT(_r, load_addr) \
	do { \
		_r->gprs[14] = 0; \
	} while(0)

#define USE_ELF_CORE_DUMP
#define ELF_EXEC_PAGESIZE       4096

/* This is the location that an ET_DYN program is loaded if exec'ed.  Typical
   use of this is to invoke "./ld.so someprog" to test out a new version of
   the loader.  We need to make sure that it is out of the way of the program
   that it will "exec", and that there is sufficient room for the brk.  */

#define ELF_ET_DYN_BASE         (TASK_SIZE / 3 * 2)

/* Wow, the "main" arch needs arch dependent functions too.. :) */

/* regs is struct pt_regs, pr_reg is elf_gregset_t (which is
   now struct_user_regs, they are different) */

#define ELF_CORE_COPY_REGS(pr_reg, regs) dump_regs32(regs, &pr_reg);


/* This yields a mask that user programs can use to figure out what
   instruction set this CPU supports. */

#define ELF_HWCAP (0)

/* This yields a string that ld.so will use to load implementation
   specific libraries for optimization.  This is more specific in
   intent than poking at uname or /proc/cpuinfo.

   For the moment, we have only optimizations for the Intel generations,
   but that could change... */

#define ELF_PLATFORM (NULL)

#define SET_PERSONALITY(ex, ibcs2)			\
do {							\
	if (ibcs2)                                      \
		set_personality(PER_SVR4);              \
	else if (current->personality != PER_LINUX32)   \
		set_personality(PER_LINUX);             \
	set_thread_flag(TIF_31BIT);			\
} while (0)

#include "compat_linux.h"

typedef _s390_fp_regs32 elf_fpregset_t;

typedef struct
{
	
	_psw_t32	psw;
	__u32		gprs[__NUM_GPRS]; 
	__u32		acrs[__NUM_ACRS]; 
	__u32		orig_gpr2;
} s390_regs32;
typedef s390_regs32 elf_gregset_t;

static inline int dump_regs32(struct pt_regs *ptregs, elf_gregset_t *regs)
{
	int i;

	memcpy(&regs->psw.mask, &ptregs->psw.mask, 4);
	memcpy(&regs->psw.addr, &ptregs->psw.addr, 4);
	for (i = 0; i < NUM_GPRS; i++)
		regs->gprs[i] = ptregs->gprs[i];
	regs->orig_gpr2 = ptregs->orig_gpr2;
	return 1;
}

#include <asm/processor.h>
#include <linux/module.h>
#include <linux/config.h>
#include <linux/elfcore.h>
#include <linux/binfmts.h>
#include <linux/compat.h>

int setup_arg_pages32(struct linux_binprm *bprm, int executable_stack);

#define elf_prstatus elf_prstatus32
struct elf_prstatus32
{
	struct elf_siginfo pr_info;	/* Info associated with signal */
	short	pr_cursig;		/* Current signal */
	u32	pr_sigpend;	/* Set of pending signals */
	u32	pr_sighold;	/* Set of held signals */
	pid_t	pr_pid;
	pid_t	pr_ppid;
	pid_t	pr_pgrp;
	pid_t	pr_sid;
	struct compat_timeval pr_utime;	/* User time */
	struct compat_timeval pr_stime;	/* System time */
	struct compat_timeval pr_cutime;	/* Cumulative user time */
	struct compat_timeval pr_cstime;	/* Cumulative system time */
	elf_gregset_t pr_reg;	/* GP registers */
	int pr_fpvalid;		/* True if math co-processor being used.  */
};

#define elf_prpsinfo elf_prpsinfo32
struct elf_prpsinfo32
{
	char	pr_state;	/* numeric process state */
	char	pr_sname;	/* char for pr_state */
	char	pr_zomb;	/* zombie */
	char	pr_nice;	/* nice val */
	u32	pr_flag;	/* flags */
	u16	pr_uid;
	u16	pr_gid;
	pid_t	pr_pid, pr_ppid, pr_pgrp, pr_sid;
	/* Lots missing */
	char	pr_fname[16];	/* filename of executable */
	char	pr_psargs[ELF_PRARGSZ];	/* initial part of arg list */
};

#include <linux/highuid.h>

#undef NEW_TO_OLD_UID
#undef NEW_TO_OLD_GID
#define NEW_TO_OLD_UID(uid) ((uid) > 65535) ? (u16)overflowuid : (u16)(uid)
#define NEW_TO_OLD_GID(gid) ((gid) > 65535) ? (u16)overflowgid : (u16)(gid) 

#define elf_addr_t	u32
/*
#define init_elf_binfmt init_elf32_binfmt
*/

#undef start_thread
#define start_thread                    start_thread31 
#define setup_arg_pages(bprm, exec)     setup_arg_pages32(bprm, exec)

MODULE_DESCRIPTION("Binary format loader for compatibility with 32bit Linux for S390 binaries,"
                   " Copyright 2000 IBM Corporation"); 
MODULE_AUTHOR("Gerhard Tonn <ton@de.ibm.com>");

#undef MODULE_DESCRIPTION
#undef MODULE_AUTHOR

#define jiffies_to_timeval jiffies_to_compat_timeval
static __inline__ void
jiffies_to_compat_timeval(unsigned long jiffies, struct compat_timeval *value)
{
	value->tv_usec = (jiffies % HZ) * (1000000L / HZ);
	value->tv_sec = jiffies / HZ;
}

#include "../../../fs/binfmt_elf.c"

