#ifndef __ASM_SH_SIGCONTEXT_H
#define __ASM_SH_SIGCONTEXT_H

struct sigcontext {
	unsigned long	oldmask;

	/* CPU registers */
	unsigned long sc_regs[16];
	unsigned long sc_pc;
	unsigned long sc_pr;
	unsigned long sc_sr;
	unsigned long sc_gbr;
	unsigned long sc_mach;
	unsigned long sc_macl;

#if defined(__SH4__)
	/* FPU registers */
	unsigned long sc_fpregs[16];
	unsigned long long sc_xdregs[8];
	unsigned int sc_fpscr;
	unsigned int sc_fpul;
	unsigned int sc_ownedfp;
#endif
};

#endif /* __ASM_SH_SIGCONTEXT_H */
