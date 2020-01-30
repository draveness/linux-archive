/*
 * Copyright (C) 2003 Broadcom Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/wait.h>
#include <linux/ptrace.h>
#include <linux/unistd.h>
#include <linux/compat.h>

#include <asm/asm.h>
#include <asm/bitops.h>
#include <asm/cacheflush.h>
#include <asm/sim.h>
#include <asm/uaccess.h>
#include <asm/ucontext.h>
#include <asm/system.h>
#include <asm/fpu.h>

/*
 * Including <asm/unistd.h would give use the 64-bit syscall numbers ...
 */
#define __NR_N32_rt_sigreturn		6211
#define __NR_N32_restart_syscall	6214

#define _BLOCKABLE (~(sigmask(SIGKILL) | sigmask(SIGSTOP)))

/* IRIX compatible stack_t  */
typedef struct sigaltstack32 {
	s32 ss_sp;
	compat_size_t ss_size;
	int ss_flags;
} stack32_t;

struct ucontextn32 {
	u32                 uc_flags;
	s32                 uc_link;
	stack32_t           uc_stack;
	struct sigcontext   uc_mcontext;
	sigset_t            uc_sigmask;   /* mask last for extensibility */
};

struct rt_sigframe_n32 {
	u32 rs_ass[4];			/* argument save space for o32 */
	u32 rs_code[2];			/* signal trampoline */
	struct siginfo rs_info;
	struct ucontextn32 rs_uc;
};

extern asmlinkage int restore_sigcontext(struct pt_regs *regs, struct sigcontext *sc);
extern int inline setup_sigcontext(struct pt_regs *regs, struct sigcontext *sc);

asmlinkage void sysn32_rt_sigreturn(nabi_no_regargs struct pt_regs regs)
{
	struct rt_sigframe_n32 *frame;
	sigset_t set;
	stack_t st;
	s32 sp;

	frame = (struct rt_sigframe_n32 *) regs.regs[29];
	if (!access_ok(VERIFY_READ, frame, sizeof(*frame)))
		goto badframe;
	if (__copy_from_user(&set, &frame->rs_uc.uc_sigmask, sizeof(set)))
		goto badframe;

	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sighand->siglock);
	current->blocked = set;
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);

	if (restore_sigcontext(&regs, &frame->rs_uc.uc_mcontext))
		goto badframe;

	/* The ucontext contains a stack32_t, so we must convert!  */
	if (__get_user(sp, &frame->rs_uc.uc_stack.ss_sp))
		goto badframe;
	st.ss_size = (long) sp;
	if (__get_user(st.ss_size, &frame->rs_uc.uc_stack.ss_size))
		goto badframe;
	if (__get_user(st.ss_flags, &frame->rs_uc.uc_stack.ss_flags))
		goto badframe;

	/* It is more difficult to avoid calling this function than to
	   call it and ignore errors.  */
	do_sigaltstack(&st, NULL, regs.regs[29]);

	/*
	 * Don't let your children do this ...
	 */
	__asm__ __volatile__(
		"move\t$29, %0\n\t"
		"j\tsyscall_exit"
		:/* no outputs */
		:"r" (&regs));
	/* Unreached */

badframe:
	force_sig(SIGSEGV, current);
}

/*
 * Determine which stack to use..
 */
static inline void *get_sigframe(struct k_sigaction *ka, struct pt_regs *regs,
	size_t frame_size)
{
	unsigned long sp;

	/* Default to using normal stack */
	sp = regs->regs[29];

	/*
 	 * FPU emulator may have it's own trampoline active just
 	 * above the user stack, 16-bytes before the next lowest
 	 * 16 byte boundary.  Try to avoid trashing it.
 	 */
 	sp -= 32;

	/* This is the X/Open sanctioned signal stack switching.  */
	if ((ka->sa.sa_flags & SA_ONSTACK) && (sas_ss_flags (sp) == 0))
		sp = current->sas_ss_sp + current->sas_ss_size;

	return (void *)((sp - frame_size) & ALMASK);
}

void setup_rt_frame_n32(struct k_sigaction * ka,
	struct pt_regs *regs, int signr, sigset_t *set, siginfo_t *info)
{
	struct rt_sigframe_n32 *frame;
	int err = 0;
	s32 sp;

	frame = get_sigframe(ka, regs, sizeof(*frame));
	if (!access_ok(VERIFY_WRITE, frame, sizeof (*frame)))
		goto give_sigsegv;

	/*
	 * Set up the return code ...
	 *
	 *         li      v0, __NR_rt_sigreturn
	 *         syscall
	 */
	err |= __put_user(0x24020000 + __NR_N32_rt_sigreturn, frame->rs_code + 0);
	err |= __put_user(0x0000000c                    , frame->rs_code + 1);
	flush_cache_sigtramp((unsigned long) frame->rs_code);

	/* Create siginfo.  */
	err |= copy_siginfo_to_user(&frame->rs_info, info);

	/* Create the ucontext.  */
	err |= __put_user(0, &frame->rs_uc.uc_flags);
	err |= __put_user(0, &frame->rs_uc.uc_link);
        sp = (int) (long) current->sas_ss_sp;
	err |= __put_user(sp,
	                  &frame->rs_uc.uc_stack.ss_sp);
	err |= __put_user(sas_ss_flags(regs->regs[29]),
	                  &frame->rs_uc.uc_stack.ss_flags);
	err |= __put_user(current->sas_ss_size,
	                  &frame->rs_uc.uc_stack.ss_size);
	err |= setup_sigcontext(regs, &frame->rs_uc.uc_mcontext);
	err |= __copy_to_user(&frame->rs_uc.uc_sigmask, set, sizeof(*set));

	if (err)
		goto give_sigsegv;

	/*
	 * Arguments to signal handler:
	 *
	 *   a0 = signal number
	 *   a1 = 0 (should be cause)
	 *   a2 = pointer to ucontext
	 *
	 * $25 and c0_epc point to the signal handler, $29 points to
	 * the struct rt_sigframe.
	 */
	regs->regs[ 4] = signr;
	regs->regs[ 5] = (unsigned long) &frame->rs_info;
	regs->regs[ 6] = (unsigned long) &frame->rs_uc;
	regs->regs[29] = (unsigned long) frame;
	regs->regs[31] = (unsigned long) frame->rs_code;
	regs->cp0_epc = regs->regs[25] = (unsigned long) ka->sa.sa_handler;

#if DEBUG_SIG
	printk("SIG deliver (%s:%d): sp=0x%p pc=0x%lx ra=0x%p\n",
	       current->comm, current->pid,
	       frame, regs->cp0_epc, regs->regs[31]);
#endif
	return;

give_sigsegv:
	if (signr == SIGSEGV)
		ka->sa.sa_handler = SIG_DFL;
	force_sig(SIGSEGV, current);
}
