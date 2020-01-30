/* 
 * Copyright (C) 2000, 2001, 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#ifndef __SYSDEP_I386_PTRACE_H
#define __SYSDEP_I386_PTRACE_H

#include "uml-config.h"

#ifdef UML_CONFIG_MODE_TT
#include "ptrace-tt.h"
#endif

#ifdef UML_CONFIG_MODE_SKAS
#include "ptrace-skas.h"
#endif

#include "choose-mode.h"

union uml_pt_regs {
#ifdef UML_CONFIG_MODE_TT
	struct tt_regs {
		long syscall;
		void *sc;
	} tt;
#endif
#ifdef UML_CONFIG_MODE_SKAS
	struct skas_regs {
		unsigned long regs[HOST_FRAME_SIZE];
		unsigned long fp[HOST_FP_SIZE];
		unsigned long xfp[HOST_XFP_SIZE];
		unsigned long fault_addr;
		unsigned long fault_type;
		unsigned long trap_type;
		long syscall;
		int is_user;
	} skas;
#endif
};

#define EMPTY_UML_PT_REGS { }

extern int mode_tt;

#define UPT_SC(r) ((r)->tt.sc)
#define UPT_IP(r) \
	CHOOSE_MODE(SC_IP(UPT_SC(r)), REGS_IP((r)->skas.regs))
#define UPT_SP(r) \
	CHOOSE_MODE(SC_SP(UPT_SC(r)), REGS_SP((r)->skas.regs))
#define UPT_EFLAGS(r) \
	CHOOSE_MODE(SC_EFLAGS(UPT_SC(r)), REGS_EFLAGS((r)->skas.regs))
#define UPT_EAX(r) \
	CHOOSE_MODE(SC_EAX(UPT_SC(r)), REGS_EAX((r)->skas.regs))
#define UPT_EBX(r) \
	CHOOSE_MODE(SC_EBX(UPT_SC(r)), REGS_EBX((r)->skas.regs))
#define UPT_ECX(r) \
	CHOOSE_MODE(SC_ECX(UPT_SC(r)), REGS_ECX((r)->skas.regs))
#define UPT_EDX(r) \
	CHOOSE_MODE(SC_EDX(UPT_SC(r)), REGS_EDX((r)->skas.regs))
#define UPT_ESI(r) \
	CHOOSE_MODE(SC_ESI(UPT_SC(r)), REGS_ESI((r)->skas.regs))
#define UPT_EDI(r) \
	CHOOSE_MODE(SC_EDI(UPT_SC(r)), REGS_EDI((r)->skas.regs))
#define UPT_EBP(r) \
	CHOOSE_MODE(SC_EBP(UPT_SC(r)), REGS_EBP((r)->skas.regs))
#define UPT_ORIG_EAX(r) \
	CHOOSE_MODE((r)->tt.syscall, (r)->skas.syscall)
#define UPT_CS(r) \
	CHOOSE_MODE(SC_CS(UPT_SC(r)), REGS_CS((r)->skas.regs))
#define UPT_SS(r) \
	CHOOSE_MODE(SC_SS(UPT_SC(r)), REGS_SS((r)->skas.regs))
#define UPT_DS(r) \
	CHOOSE_MODE(SC_DS(UPT_SC(r)), REGS_DS((r)->skas.regs))
#define UPT_ES(r) \
	CHOOSE_MODE(SC_ES(UPT_SC(r)), REGS_ES((r)->skas.regs))
#define UPT_FS(r) \
	CHOOSE_MODE(SC_FS(UPT_SC(r)), REGS_FS((r)->skas.regs))
#define UPT_GS(r) \
	CHOOSE_MODE(SC_GS(UPT_SC(r)), REGS_GS((r)->skas.regs))

#define UPT_SYSCALL_ARG1(r) UPT_EBX(r)
#define UPT_SYSCALL_ARG2(r) UPT_ECX(r)
#define UPT_SYSCALL_ARG3(r) UPT_EDX(r)
#define UPT_SYSCALL_ARG4(r) UPT_ESI(r)
#define UPT_SYSCALL_ARG5(r) UPT_EDI(r)
#define UPT_SYSCALL_ARG6(r) UPT_EBP(r)

extern int user_context(unsigned long sp);

#define UPT_IS_USER(r) \
	CHOOSE_MODE(user_context(UPT_SP(r)), (r)->skas.is_user)

struct syscall_args {
	unsigned long args[6];
};

#define SYSCALL_ARGS(r) ((struct syscall_args) \
                        { .args = { UPT_SYSCALL_ARG1(r), \
                                    UPT_SYSCALL_ARG2(r), \
 			            UPT_SYSCALL_ARG3(r), \
                                    UPT_SYSCALL_ARG4(r), \
		                    UPT_SYSCALL_ARG5(r), \
                                    UPT_SYSCALL_ARG6(r) } } )

#define UPT_REG(regs, reg) \
	({	unsigned long val; \
		switch(reg){ \
		case EIP: val = UPT_IP(regs); break; \
		case UESP: val = UPT_SP(regs); break; \
		case EAX: val = UPT_EAX(regs); break; \
		case EBX: val = UPT_EBX(regs); break; \
		case ECX: val = UPT_ECX(regs); break; \
		case EDX: val = UPT_EDX(regs); break; \
		case ESI: val = UPT_ESI(regs); break; \
		case EDI: val = UPT_EDI(regs); break; \
		case EBP: val = UPT_EBP(regs); break; \
		case ORIG_EAX: val = UPT_ORIG_EAX(regs); break; \
		case CS: val = UPT_CS(regs); break; \
		case SS: val = UPT_SS(regs); break; \
		case DS: val = UPT_DS(regs); break; \
		case ES: val = UPT_ES(regs); break; \
		case FS: val = UPT_FS(regs); break; \
		case GS: val = UPT_GS(regs); break; \
		case EFL: val = UPT_EFLAGS(regs); break; \
		default :  \
			panic("Bad register in UPT_REG : %d\n", reg);  \
			val = -1; \
		} \
	        val; \
	})
	

#define UPT_SET(regs, reg, val) \
	do { \
		switch(reg){ \
		case EIP: UPT_IP(regs) = val; break; \
		case UESP: UPT_SP(regs) = val; break; \
		case EAX: UPT_EAX(regs) = val; break; \
		case EBX: UPT_EBX(regs) = val; break; \
		case ECX: UPT_ECX(regs) = val; break; \
		case EDX: UPT_EDX(regs) = val; break; \
		case ESI: UPT_ESI(regs) = val; break; \
		case EDI: UPT_EDI(regs) = val; break; \
		case EBP: UPT_EBP(regs) = val; break; \
		case ORIG_EAX: UPT_ORIG_EAX(regs) = val; break; \
		case CS: UPT_CS(regs) = val; break; \
		case SS: UPT_SS(regs) = val; break; \
		case DS: UPT_DS(regs) = val; break; \
		case ES: UPT_ES(regs) = val; break; \
		case FS: UPT_FS(regs) = val; break; \
		case GS: UPT_GS(regs) = val; break; \
		case EFL: UPT_EFLAGS(regs) = val; break; \
		default :  \
			panic("Bad register in UPT_SET : %d\n", reg);  \
			break; \
		} \
	} while (0)

#define UPT_SET_SYSCALL_RETURN(r, res) \
	CHOOSE_MODE(SC_SET_SYSCALL_RETURN(UPT_SC(r), (res)), \
                    REGS_SET_SYSCALL_RETURN((r)->skas.regs, (res)))

#define UPT_RESTART_SYSCALL(r) \
	CHOOSE_MODE(SC_RESTART_SYSCALL(UPT_SC(r)), \
		    REGS_RESTART_SYSCALL((r)->skas.regs))

#define UPT_ORIG_SYSCALL(r) UPT_EAX(r)
#define UPT_SYSCALL_NR(r) UPT_ORIG_EAX(r)
#define UPT_SYSCALL_RET(r) UPT_EAX(r)

#define UPT_SEGV_IS_FIXABLE(r) \
	CHOOSE_MODE(SC_SEGV_IS_FIXABLE(UPT_SC(r)), \
                    REGS_SEGV_IS_FIXABLE(&r->skas))

#define UPT_FAULT_ADDR(r) \
	CHOOSE_MODE(SC_FAULT_ADDR(UPT_SC(r)), REGS_FAULT_ADDR(&r->skas))

#define UPT_FAULT_WRITE(r) \
	CHOOSE_MODE(SC_FAULT_WRITE(UPT_SC(r)), REGS_FAULT_WRITE(&r->skas))

#endif

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
