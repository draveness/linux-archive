/*
 *  linux/arch/ppc64/kernel/signal.c
 *
 *  PowerPC version 
 *    Copyright (C) 1995-1996 Gary Thomas (gdt@linuxppc.org)
 *
 *  Derived from "arch/i386/kernel/signal.c"
 *    Copyright (C) 1991, 1992 Linus Torvalds
 *    1997-11-28  Modified for POSIX.1b signals by Richard Henderson
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 */

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/kernel.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/wait.h>
#include <linux/unistd.h>
#include <linux/stddef.h>
#include <linux/elf.h>
#include <asm/ppc32.h>
#include <asm/sigcontext.h>
#include <asm/ucontext.h>
#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/ppcdebug.h>
#include <asm/unistd.h>
#include <asm/cacheflush.h>

#define DEBUG_SIG 0

#define _BLOCKABLE (~(sigmask(SIGKILL) | sigmask(SIGSTOP)))

#ifndef MIN
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif

#define GP_REGS_SIZE	MIN(sizeof(elf_gregset_t), sizeof(struct pt_regs))
#define FP_REGS_SIZE	sizeof(elf_fpregset_t)

#define TRAMP_TRACEBACK	3
#define TRAMP_SIZE	6

/*
 * When we have signals to deliver, we set up on the user stack,
 * going down from the original stack pointer:
 *	1) a rt_sigframe struct which contains the ucontext	
 *	2) a gap of __SIGNAL_FRAMESIZE bytes which acts as a dummy caller
 *	   frame for the signal handler.
 */

struct rt_sigframe {
	/* sys_rt_sigreturn requires the ucontext be the first field */
	struct ucontext uc;
	unsigned long _unused[2];
	unsigned int tramp[TRAMP_SIZE];
	struct siginfo *pinfo;
	void *puc;
	struct siginfo info;
	/* 64 bit ABI allows for 288 bytes below sp before decrementing it. */
	char abigap[288];
};


/*
 * Atomically swap in the new signal mask, and wait for a signal.
 */
long sys_rt_sigsuspend(sigset_t __user *unewset, size_t sigsetsize, int p3, int p4,
		       int p6, int p7, struct pt_regs *regs)
{
	sigset_t saveset, newset;

	/* XXX: Don't preclude handling different sized sigset_t's.  */
	if (sigsetsize != sizeof(sigset_t))
		return -EINVAL;

	if (copy_from_user(&newset, unewset, sizeof(newset)))
		return -EFAULT;
	sigdelsetmask(&newset, ~_BLOCKABLE);

	spin_lock_irq(&current->sighand->siglock);
	saveset = current->blocked;
	current->blocked = newset;
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);

	regs->result = -EINTR;
	regs->gpr[3] = EINTR;
	regs->ccr |= 0x10000000;
	while (1) {
		current->state = TASK_INTERRUPTIBLE;
		schedule();
		if (do_signal(&saveset, regs))
			return regs->gpr[3];
	}
}

long sys_sigaltstack(const stack_t __user *uss, stack_t __user *uoss, unsigned long r5,
		     unsigned long r6, unsigned long r7, unsigned long r8,
		     struct pt_regs *regs)
{
	return do_sigaltstack(uss, uoss, regs->gpr[1]);
}


/*
 * Set up the sigcontext for the signal frame.
 */

static long setup_sigcontext(struct sigcontext __user *sc, struct pt_regs *regs,
		 int signr, sigset_t *set, unsigned long handler)
{
	/* When CONFIG_ALTIVEC is set, we _always_ setup v_regs even if the
	 * process never used altivec yet (MSR_VEC is zero in pt_regs of
	 * the context). This is very important because we must ensure we
	 * don't lose the VRSAVE content that may have been set prior to
	 * the process doing its first vector operation
	 * Userland shall check AT_HWCAP to know wether it can rely on the
	 * v_regs pointer or not
	 */
#ifdef CONFIG_ALTIVEC
	elf_vrreg_t __user *v_regs = (elf_vrreg_t __user *)(((unsigned long)sc->vmx_reserve) & ~0xful);
#endif
	long err = 0;

	flush_fp_to_thread(current);

	/* Make sure signal doesn't get spurrious FP exceptions */
	current->thread.fpscr = 0;

#ifdef CONFIG_ALTIVEC
	err |= __put_user(v_regs, &sc->v_regs);

	/* save altivec registers */
	if (current->thread.used_vr) {
		flush_altivec_to_thread(current);
		/* Copy 33 vec registers (vr0..31 and vscr) to the stack */
		err |= __copy_to_user(v_regs, current->thread.vr, 33 * sizeof(vector128));
		/* set MSR_VEC in the MSR value in the frame to indicate that sc->v_reg)
		 * contains valid data.
		 */
		regs->msr |= MSR_VEC;
	}
	/* We always copy to/from vrsave, it's 0 if we don't have or don't
	 * use altivec.
	 */
	err |= __put_user(current->thread.vrsave, (u32 __user *)&v_regs[33]);
#else /* CONFIG_ALTIVEC */
	err |= __put_user(0, &sc->v_regs);
#endif /* CONFIG_ALTIVEC */
	err |= __put_user(&sc->gp_regs, &sc->regs);
	err |= __copy_to_user(&sc->gp_regs, regs, GP_REGS_SIZE);
	err |= __copy_to_user(&sc->fp_regs, &current->thread.fpr, FP_REGS_SIZE);
	err |= __put_user(signr, &sc->signal);
	err |= __put_user(handler, &sc->handler);
	if (set != NULL)
		err |=  __put_user(set->sig[0], &sc->oldmask);

	return err;
}

/*
 * Restore the sigcontext from the signal frame.
 */

static long restore_sigcontext(struct pt_regs *regs, sigset_t *set, int sig,
			      struct sigcontext __user *sc)
{
#ifdef CONFIG_ALTIVEC
	elf_vrreg_t __user *v_regs;
#endif
	unsigned long err = 0;
	unsigned long save_r13;
	elf_greg_t *gregs = (elf_greg_t *)regs;
	int i;

	/* If this is not a signal return, we preserve the TLS in r13 */
	if (!sig)
		save_r13 = regs->gpr[13];

	/* copy everything before MSR */
	err |= __copy_from_user(regs, &sc->gp_regs,
				PT_MSR*sizeof(unsigned long));

	/* skip MSR and SOFTE */
	for (i = PT_MSR+1; i <= PT_RESULT; i++) {
		if (i == PT_SOFTE)
			continue;
		err |= __get_user(gregs[i], &sc->gp_regs[i]);
	}

	if (!sig)
		regs->gpr[13] = save_r13;
	err |= __copy_from_user(&current->thread.fpr, &sc->fp_regs, FP_REGS_SIZE);
	if (set != NULL)
		err |=  __get_user(set->sig[0], &sc->oldmask);

#ifdef CONFIG_ALTIVEC
	err |= __get_user(v_regs, &sc->v_regs);
	if (err)
		return err;
	/* Copy 33 vec registers (vr0..31 and vscr) from the stack */
	if (v_regs != 0 && (regs->msr & MSR_VEC) != 0)
		err |= __copy_from_user(current->thread.vr, v_regs, 33 * sizeof(vector128));
	else if (current->thread.used_vr)
		memset(&current->thread.vr, 0, 33);
	/* Always get VRSAVE back */
	if (v_regs != 0)
		err |= __get_user(current->thread.vrsave, (u32 __user *)&v_regs[33]);
	else
		current->thread.vrsave = 0;
#endif /* CONFIG_ALTIVEC */

	/* Force reload of FP/VEC */
	regs->msr &= ~(MSR_FP | MSR_FE0 | MSR_FE1 | MSR_VEC);

	return err;
}

/*
 * Allocate space for the signal frame
 */
static inline void __user * get_sigframe(struct k_sigaction *ka, struct pt_regs *regs,
				  size_t frame_size)
{
        unsigned long newsp;

        /* Default to using normal stack */
        newsp = regs->gpr[1];

	if (ka->sa.sa_flags & SA_ONSTACK) {
		if (! on_sig_stack(regs->gpr[1]))
			newsp = (current->sas_ss_sp + current->sas_ss_size);
	}

        return (void __user *)((newsp - frame_size) & -8ul);
}

/*
 * Setup the trampoline code on the stack
 */
static long setup_trampoline(unsigned int syscall, unsigned int __user *tramp)
{
	int i;
	long err = 0;

	/* addi r1, r1, __SIGNAL_FRAMESIZE  # Pop the dummy stackframe */
	err |= __put_user(0x38210000UL | (__SIGNAL_FRAMESIZE & 0xffff), &tramp[0]);
	/* li r0, __NR_[rt_]sigreturn| */
	err |= __put_user(0x38000000UL | (syscall & 0xffff), &tramp[1]);
	/* sc */
	err |= __put_user(0x44000002UL, &tramp[2]);

	/* Minimal traceback info */
	for (i=TRAMP_TRACEBACK; i < TRAMP_SIZE ;i++)
		err |= __put_user(0, &tramp[i]);

	if (!err)
		flush_icache_range((unsigned long) &tramp[0],
			   (unsigned long) &tramp[TRAMP_SIZE]);

	return err;
}

/*
 * Restore the user process's signal mask (also used by signal32.c)
 */
void restore_sigmask(sigset_t *set)
{
	sigdelsetmask(set, ~_BLOCKABLE);
	spin_lock_irq(&current->sighand->siglock);
	current->blocked = *set;
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);
}


/*
 * Handle {get,set,swap}_context operations
 */
int sys_swapcontext(struct ucontext __user *old_ctx,
		    struct ucontext __user *new_ctx,
		    long ctx_size, long r6, long r7, long r8, struct pt_regs *regs)
{
	unsigned char tmp;
	sigset_t set;

	/* Context size is for future use. Right now, we only make sure
	 * we are passed something we understand
	 */
	if (ctx_size < sizeof(struct ucontext))
		return -EINVAL;

	if (old_ctx != NULL) {
		if (verify_area(VERIFY_WRITE, old_ctx, sizeof(*old_ctx))
		    || setup_sigcontext(&old_ctx->uc_mcontext, regs, 0, NULL, 0)
		    || __copy_to_user(&old_ctx->uc_sigmask,
				      &current->blocked, sizeof(sigset_t)))
			return -EFAULT;
	}
	if (new_ctx == NULL)
		return 0;
	if (verify_area(VERIFY_READ, new_ctx, sizeof(*new_ctx))
	    || __get_user(tmp, (u8 __user *) new_ctx)
	    || __get_user(tmp, (u8 __user *) (new_ctx + 1) - 1))
		return -EFAULT;

	/*
	 * If we get a fault copying the context into the kernel's
	 * image of the user's registers, we can't just return -EFAULT
	 * because the user's registers will be corrupted.  For instance
	 * the NIP value may have been updated but not some of the
	 * other registers.  Given that we have done the verify_area
	 * and successfully read the first and last bytes of the region
	 * above, this should only happen in an out-of-memory situation
	 * or if another thread unmaps the region containing the context.
	 * We kill the task with a SIGSEGV in this situation.
	 */

	if (__copy_from_user(&set, &new_ctx->uc_sigmask, sizeof(set)))
		do_exit(SIGSEGV);
	restore_sigmask(&set);
	if (restore_sigcontext(regs, NULL, 0, &new_ctx->uc_mcontext))
		do_exit(SIGSEGV);

	/* This returns like rt_sigreturn */
	return 0;
}


/*
 * Do a signal return; undo the signal stack.
 */

int sys_rt_sigreturn(unsigned long r3, unsigned long r4, unsigned long r5,
		     unsigned long r6, unsigned long r7, unsigned long r8,
		     struct pt_regs *regs)
{
	struct ucontext __user *uc = (struct ucontext __user *)regs->gpr[1];
	sigset_t set;

	/* Always make any pending restarted system calls return -EINTR */
	current_thread_info()->restart_block.fn = do_no_restart_syscall;

	if (verify_area(VERIFY_READ, uc, sizeof(*uc)))
		goto badframe;

	if (__copy_from_user(&set, &uc->uc_sigmask, sizeof(set)))
		goto badframe;
	restore_sigmask(&set);
	if (restore_sigcontext(regs, NULL, 1, &uc->uc_mcontext))
		goto badframe;

	/* do_sigaltstack expects a __user pointer and won't modify
	 * what's in there anyway
	 */
	do_sigaltstack(&uc->uc_stack, NULL, regs->gpr[1]);

	return regs->result;

badframe:
#if DEBUG_SIG
	printk("badframe in sys_rt_sigreturn, regs=%p uc=%p &uc->uc_mcontext=%p\n",
	       regs, uc, &uc->uc_mcontext);
#endif
	do_exit(SIGSEGV);
}

static void setup_rt_frame(int signr, struct k_sigaction *ka, siginfo_t *info,
		sigset_t *set, struct pt_regs *regs)
{
	/* Handler is *really* a pointer to the function descriptor for
	 * the signal routine.  The first entry in the function
	 * descriptor is the entry address of signal and the second
	 * entry is the TOC value we need to use.
	 */
	func_descr_t __user *funct_desc_ptr;
	struct rt_sigframe __user *frame;
	unsigned long newsp = 0;
	long err = 0;

	frame = get_sigframe(ka, regs, sizeof(*frame));

	if (verify_area(VERIFY_WRITE, frame, sizeof(*frame)))
		goto badframe;

	err |= __put_user(&frame->info, &frame->pinfo);
	err |= __put_user(&frame->uc, &frame->puc);
	err |= copy_siginfo_to_user(&frame->info, info);
	if (err)
		goto badframe;

	/* Create the ucontext.  */
	err |= __put_user(0, &frame->uc.uc_flags);
	err |= __put_user(0, &frame->uc.uc_link);
	err |= __put_user(current->sas_ss_sp, &frame->uc.uc_stack.ss_sp);
	err |= __put_user(sas_ss_flags(regs->gpr[1]),
			  &frame->uc.uc_stack.ss_flags);
	err |= __put_user(current->sas_ss_size, &frame->uc.uc_stack.ss_size);
	err |= setup_sigcontext(&frame->uc.uc_mcontext, regs, signr, NULL,
				(unsigned long)ka->sa.sa_handler);
	err |= __copy_to_user(&frame->uc.uc_sigmask, set, sizeof(*set));
	if (err)
		goto badframe;

	/* Set up to return from userspace. */
	err |= setup_trampoline(__NR_rt_sigreturn, &frame->tramp[0]);
	if (err)
		goto badframe;

	funct_desc_ptr = (func_descr_t __user *) ka->sa.sa_handler;

	/* Allocate a dummy caller frame for the signal handler. */
	newsp = (unsigned long)frame - __SIGNAL_FRAMESIZE;
	err |= put_user(0, (unsigned long __user *)newsp);

	/* Set up "regs" so we "return" to the signal handler. */
	err |= get_user(regs->nip, &funct_desc_ptr->entry);
	regs->link = (unsigned long) &frame->tramp[0];
	regs->gpr[1] = newsp;
	err |= get_user(regs->gpr[2], &funct_desc_ptr->toc);
	regs->gpr[3] = signr;
	regs->result = 0;
	if (ka->sa.sa_flags & SA_SIGINFO) {
		err |= get_user(regs->gpr[4], (unsigned long __user *)&frame->pinfo);
		err |= get_user(regs->gpr[5], (unsigned long __user *)&frame->puc);
		regs->gpr[6] = (unsigned long) frame;
	} else {
		regs->gpr[4] = (unsigned long)&frame->uc.uc_mcontext;
	}
	if (err)
		goto badframe;

	return;

badframe:
#if DEBUG_SIG
	printk("badframe in setup_rt_frame, regs=%p frame=%p newsp=%lx\n",
	       regs, frame, newsp);
#endif
	do_exit(SIGSEGV);
}


/*
 * OK, we're invoking a handler
 */
static void handle_signal(unsigned long sig, struct k_sigaction *ka,
			  siginfo_t *info, sigset_t *oldset, struct pt_regs *regs)
{
	/* Set up Signal Frame */
	setup_rt_frame(sig, ka, info, oldset, regs);

	if (ka->sa.sa_flags & SA_ONESHOT)
		ka->sa.sa_handler = SIG_DFL;

	if (!(ka->sa.sa_flags & SA_NODEFER)) {
		spin_lock_irq(&current->sighand->siglock);
		sigorsets(&current->blocked,&current->blocked,&ka->sa.sa_mask);
		sigaddset(&current->blocked,sig);
		recalc_sigpending();
		spin_unlock_irq(&current->sighand->siglock);
	}
	return;
}

static inline void syscall_restart(struct pt_regs *regs, struct k_sigaction *ka)
{
	switch ((int)regs->result) {
	case -ERESTART_RESTARTBLOCK:
	case -ERESTARTNOHAND:
		/* ERESTARTNOHAND means that the syscall should only be
		 * restarted if there was no handler for the signal, and since
		 * we only get here if there is a handler, we dont restart.
		 */
		regs->result = -EINTR;
		break;
	case -ERESTARTSYS:
		/* ERESTARTSYS means to restart the syscall if there is no
		 * handler or the handler was registered with SA_RESTART
		 */
		if (!(ka->sa.sa_flags & SA_RESTART)) {
			regs->result = -EINTR;
			break;
		}
		/* fallthrough */
	case -ERESTARTNOINTR:
		/* ERESTARTNOINTR means that the syscall should be
		 * called again after the signal handler returns.
		 */
		regs->gpr[3] = regs->orig_gpr3;
		regs->nip -= 4;
		regs->result = 0;
		break;
	}
}

/*
 * Note that 'init' is a special process: it doesn't get signals it doesn't
 * want to handle. Thus you cannot kill init even with a SIGKILL even by
 * mistake.
 */
int do_signal(sigset_t *oldset, struct pt_regs *regs)
{
	siginfo_t info;
	int signr;

	/*
	 * If the current thread is 32 bit - invoke the
	 * 32 bit signal handling code
	 */
	if (test_thread_flag(TIF_32BIT))
		return do_signal32(oldset, regs);

	if (!oldset)
		oldset = &current->blocked;

	signr = get_signal_to_deliver(&info, regs, NULL);
	if (signr > 0) {
		struct k_sigaction *ka = &current->sighand->action[signr-1];

		/* Whee!  Actually deliver the signal.  */
		if (TRAP(regs) == 0x0C00)
			syscall_restart(regs, ka);
		handle_signal(signr, ka, &info, oldset, regs);
		return 1;
	}

	if (TRAP(regs) == 0x0C00) {	/* System Call! */
		if ((int)regs->result == -ERESTARTNOHAND ||
		    (int)regs->result == -ERESTARTSYS ||
		    (int)regs->result == -ERESTARTNOINTR) {
			regs->gpr[3] = regs->orig_gpr3;
			regs->nip -= 4; /* Back up & retry system call */
			regs->result = 0;
		} else if ((int)regs->result == -ERESTART_RESTARTBLOCK) {
			regs->gpr[0] = __NR_restart_syscall;
			regs->nip -= 4;
			regs->result = 0;
		}
	}

	return 0;
}



