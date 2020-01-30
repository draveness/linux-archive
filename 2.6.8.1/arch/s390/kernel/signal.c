/*
 *  arch/s390/kernel/signal.c
 *
 *  S390 version
 *    Copyright (C) 1999,2000 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Denis Joseph Barrow (djbarrow@de.ibm.com,barrow_dj@yahoo.com)
 *
 *    Based on Intel version
 * 
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  1997-11-28  Modified for POSIX.1b signals by Richard Henderson
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
#include <linux/ptrace.h>
#include <linux/unistd.h>
#include <linux/stddef.h>
#include <linux/tty.h>
#include <linux/personality.h>
#include <linux/binfmts.h>
#include <asm/ucontext.h>
#include <asm/uaccess.h>
#include <asm/lowcore.h>

#define _BLOCKABLE (~(sigmask(SIGKILL) | sigmask(SIGSTOP)))


typedef struct 
{
	__u8 callee_used_stack[__SIGNAL_FRAMESIZE];
	struct sigcontext sc;
	_sigregs sregs;
	int signo;
	__u8 retcode[S390_SYSCALL_SIZE];
} sigframe;

typedef struct 
{
	__u8 callee_used_stack[__SIGNAL_FRAMESIZE];
	__u8 retcode[S390_SYSCALL_SIZE];
	struct siginfo info;
	struct ucontext uc;
} rt_sigframe;

int do_signal(struct pt_regs *regs, sigset_t *oldset);

/*
 * Atomically swap in the new signal mask, and wait for a signal.
 */
asmlinkage int
sys_sigsuspend(struct pt_regs * regs, int history0, int history1,
	       old_sigset_t mask)
{
	sigset_t saveset;

	mask &= _BLOCKABLE;
	spin_lock_irq(&current->sighand->siglock);
	saveset = current->blocked;
	siginitset(&current->blocked, mask);
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);
	regs->gprs[2] = -EINTR;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
		if (do_signal(regs, &saveset))
			return -EINTR;
	}
}

asmlinkage long
sys_rt_sigsuspend(struct pt_regs *regs, sigset_t __user *unewset,
						size_t sigsetsize)
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
	regs->gprs[2] = -EINTR;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
		if (do_signal(regs, &saveset))
			return -EINTR;
	}
}

asmlinkage long
sys_sigaction(int sig, const struct old_sigaction __user *act,
	      struct old_sigaction __user *oact)
{
	struct k_sigaction new_ka, old_ka;
	int ret;

	if (act) {
		old_sigset_t mask;
		if (verify_area(VERIFY_READ, act, sizeof(*act)) ||
		    __get_user(new_ka.sa.sa_handler, &act->sa_handler) ||
		    __get_user(new_ka.sa.sa_restorer, &act->sa_restorer))
			return -EFAULT;
		__get_user(new_ka.sa.sa_flags, &act->sa_flags);
		__get_user(mask, &act->sa_mask);
		siginitset(&new_ka.sa.sa_mask, mask);
	}

	ret = do_sigaction(sig, act ? &new_ka : NULL, oact ? &old_ka : NULL);

	if (!ret && oact) {
		if (verify_area(VERIFY_WRITE, oact, sizeof(*oact)) ||
		    __put_user(old_ka.sa.sa_handler, &oact->sa_handler) ||
		    __put_user(old_ka.sa.sa_restorer, &oact->sa_restorer))
			return -EFAULT;
		__put_user(old_ka.sa.sa_flags, &oact->sa_flags);
		__put_user(old_ka.sa.sa_mask.sig[0], &oact->sa_mask);
	}

	return ret;
}

asmlinkage long
sys_sigaltstack(const stack_t __user *uss, stack_t __user *uoss,
					struct pt_regs *regs)
{
	return do_sigaltstack(uss, uoss, regs->gprs[15]);
}



/* Returns non-zero on fault. */
static int save_sigregs(struct pt_regs *regs, _sigregs __user *sregs)
{
	unsigned long old_mask = regs->psw.mask;
	int err;
  
	save_access_regs(current->thread.acrs);

	/* Copy a 'clean' PSW mask to the user to avoid leaking
	   information about whether PER is currently on.  */
	regs->psw.mask = PSW_MASK_MERGE(PSW_USER_BITS, regs->psw.mask);
	err = __copy_to_user(&sregs->regs.psw, &regs->psw,
			     sizeof(sregs->regs.psw)+sizeof(sregs->regs.gprs));
	regs->psw.mask = old_mask;
	if (err != 0)
		return err;
	err = __copy_to_user(&sregs->regs.acrs, current->thread.acrs,
			     sizeof(sregs->regs.acrs));
	if (err != 0)
		return err;
	/* 
	 * We have to store the fp registers to current->thread.fp_regs
	 * to merge them with the emulated registers.
	 */
	save_fp_regs(&current->thread.fp_regs);
	return __copy_to_user(&sregs->fpregs, &current->thread.fp_regs,
			      sizeof(s390_fp_regs));
}

/* Returns positive number on error */
static int restore_sigregs(struct pt_regs *regs, _sigregs __user *sregs)
{
	unsigned long old_mask = regs->psw.mask;
	int err;

	/* Alwys make any pending restarted system call return -EINTR */
	current_thread_info()->restart_block.fn = do_no_restart_syscall;

	err = __copy_from_user(&regs->psw, &sregs->regs.psw,
			       sizeof(sregs->regs.psw)+sizeof(sregs->regs.gprs));
	regs->psw.mask = PSW_MASK_MERGE(old_mask, regs->psw.mask);
	regs->psw.addr |= PSW_ADDR_AMODE;
	if (err)
		return err;
	err = __copy_from_user(&current->thread.acrs, &sregs->regs.acrs,
			       sizeof(sregs->regs.acrs));
	if (err)
		return err;
	restore_access_regs(current->thread.acrs);

	err = __copy_from_user(&current->thread.fp_regs, &sregs->fpregs,
			       sizeof(s390_fp_regs));
	current->thread.fp_regs.fpc &= FPC_VALID_MASK;
	if (err)
		return err;

	restore_fp_regs(&current->thread.fp_regs);
	regs->trap = -1;	/* disable syscall checks */
	return 0;
}

asmlinkage long sys_sigreturn(struct pt_regs *regs)
{
	sigframe __user *frame = (sigframe __user *)regs->gprs[15];
	sigset_t set;

	if (verify_area(VERIFY_READ, frame, sizeof(*frame)))
		goto badframe;
	if (__copy_from_user(&set.sig, &frame->sc.oldmask, _SIGMASK_COPY_SIZE))
		goto badframe;

	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sighand->siglock);
	current->blocked = set;
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);

	if (restore_sigregs(regs, &frame->sregs))
		goto badframe;

	return regs->gprs[2];

badframe:
	force_sig(SIGSEGV, current);
	return 0;
}

asmlinkage long sys_rt_sigreturn(struct pt_regs *regs)
{
	rt_sigframe __user *frame = (rt_sigframe __user *)regs->gprs[15];
	sigset_t set;

	if (verify_area(VERIFY_READ, frame, sizeof(*frame)))
		goto badframe;
	if (__copy_from_user(&set.sig, &frame->uc.uc_sigmask, sizeof(set)))
		goto badframe;

	sigdelsetmask(&set, ~_BLOCKABLE);
	spin_lock_irq(&current->sighand->siglock);
	current->blocked = set;
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);

	if (restore_sigregs(regs, &frame->uc.uc_mcontext))
		goto badframe;

	/* It is more difficult to avoid calling this function than to
	   call it and ignore errors.  */
	do_sigaltstack(&frame->uc.uc_stack, NULL, regs->gprs[15]);
	return regs->gprs[2];

badframe:
	force_sig(SIGSEGV, current);
	return 0;
}

/*
 * Set up a signal frame.
 */


/*
 * Determine which stack to use..
 */
static inline void __user *
get_sigframe(struct k_sigaction *ka, struct pt_regs * regs, size_t frame_size)
{
	unsigned long sp;

	/* Default to using normal stack */
	sp = regs->gprs[15];

	/* This is the X/Open sanctioned signal stack switching.  */
	if (ka->sa.sa_flags & SA_ONSTACK) {
		if (! on_sig_stack(sp))
			sp = current->sas_ss_sp + current->sas_ss_size;
	}

	/* This is the legacy signal stack switching. */
	else if (!user_mode(regs) &&
		 !(ka->sa.sa_flags & SA_RESTORER) &&
		 ka->sa.sa_restorer) {
		sp = (unsigned long) ka->sa.sa_restorer;
	}

	return (void __user *)((sp - frame_size) & -8ul);
}

static inline int map_signal(int sig)
{
	if (current_thread_info()->exec_domain
	    && current_thread_info()->exec_domain->signal_invmap
	    && sig < 32)
		return current_thread_info()->exec_domain->signal_invmap[sig];
	else
		return sig;
}

static void setup_frame(int sig, struct k_sigaction *ka,
			sigset_t *set, struct pt_regs * regs)
{
	sigframe __user *frame;

	frame = get_sigframe(ka, regs, sizeof(sigframe));
	if (!access_ok(VERIFY_WRITE, frame, sizeof(sigframe)))
		goto give_sigsegv;

	if (__copy_to_user(&frame->sc.oldmask, &set->sig, _SIGMASK_COPY_SIZE))
		goto give_sigsegv;

	if (save_sigregs(regs, &frame->sregs))
		goto give_sigsegv;
	if (__put_user(&frame->sregs, &frame->sc.sregs))
		goto give_sigsegv;

	/* Set up to return from userspace.  If provided, use a stub
	   already in userspace.  */
	if (ka->sa.sa_flags & SA_RESTORER) {
                regs->gprs[14] = (unsigned long)
			ka->sa.sa_restorer | PSW_ADDR_AMODE;
	} else {
                regs->gprs[14] = (unsigned long)
			frame->retcode | PSW_ADDR_AMODE;
		if (__put_user(S390_SYSCALL_OPCODE | __NR_sigreturn,
	                       (u16 __user *)(frame->retcode)))
			goto give_sigsegv;
	}

	/* Set up backchain. */
	if (__put_user(regs->gprs[15], (addr_t __user *) frame))
		goto give_sigsegv;

	/* Set up registers for signal handler */
	regs->gprs[15] = (unsigned long) frame;
	regs->psw.addr = (unsigned long) ka->sa.sa_handler | PSW_ADDR_AMODE;

	regs->gprs[2] = map_signal(sig);
	regs->gprs[3] = (unsigned long) &frame->sc;

	/* We forgot to include these in the sigcontext.
	   To avoid breaking binary compatibility, they are passed as args. */
	regs->gprs[4] = current->thread.trap_no;
	regs->gprs[5] = current->thread.prot_addr;

	/* Place signal number on stack to allow backtrace from handler.  */
	if (__put_user(regs->gprs[2], (int __user *) &frame->signo))
		goto give_sigsegv;
	return;

give_sigsegv:
	if (sig == SIGSEGV)
		ka->sa.sa_handler = SIG_DFL;
	force_sig(SIGSEGV, current);
}

static void setup_rt_frame(int sig, struct k_sigaction *ka, siginfo_t *info,
			   sigset_t *set, struct pt_regs * regs)
{
	int err = 0;
	rt_sigframe __user *frame;

	frame = get_sigframe(ka, regs, sizeof(rt_sigframe));
	if (!access_ok(VERIFY_WRITE, frame, sizeof(rt_sigframe)))
		goto give_sigsegv;

	if (copy_siginfo_to_user(&frame->info, info))
		goto give_sigsegv;

	/* Create the ucontext.  */
	err |= __put_user(0, &frame->uc.uc_flags);
	err |= __put_user(0, &frame->uc.uc_link);
	err |= __put_user((void *)current->sas_ss_sp, &frame->uc.uc_stack.ss_sp);
	err |= __put_user(sas_ss_flags(regs->gprs[15]),
			  &frame->uc.uc_stack.ss_flags);
	err |= __put_user(current->sas_ss_size, &frame->uc.uc_stack.ss_size);
	err |= save_sigregs(regs, &frame->uc.uc_mcontext);
	err |= __copy_to_user(&frame->uc.uc_sigmask, set, sizeof(*set));
	if (err)
		goto give_sigsegv;

	/* Set up to return from userspace.  If provided, use a stub
	   already in userspace.  */
	if (ka->sa.sa_flags & SA_RESTORER) {
                regs->gprs[14] = (unsigned long)
			ka->sa.sa_restorer | PSW_ADDR_AMODE;
	} else {
                regs->gprs[14] = (unsigned long)
			frame->retcode | PSW_ADDR_AMODE;
		err |= __put_user(S390_SYSCALL_OPCODE | __NR_rt_sigreturn,
	                          (u16 __user *)(frame->retcode));
	}

	/* Set up backchain. */
	if (__put_user(regs->gprs[15], (addr_t __user *) frame))
		goto give_sigsegv;

	/* Set up registers for signal handler */
	regs->gprs[15] = (unsigned long) frame;
	regs->psw.addr = (unsigned long) ka->sa.sa_handler | PSW_ADDR_AMODE;

	regs->gprs[2] = map_signal(sig);
	regs->gprs[3] = (unsigned long) &frame->info;
	regs->gprs[4] = (unsigned long) &frame->uc;
	return;

give_sigsegv:
	if (sig == SIGSEGV)
		ka->sa.sa_handler = SIG_DFL;
	force_sig(SIGSEGV, current);
}

/*
 * OK, we're invoking a handler
 */	

static void
handle_signal(unsigned long sig, siginfo_t *info, sigset_t *oldset,
	struct pt_regs * regs)
{
	struct k_sigaction *ka = &current->sighand->action[sig-1];

	/* Set up the stack frame */
	if (ka->sa.sa_flags & SA_SIGINFO)
		setup_rt_frame(sig, ka, info, oldset, regs);
	else
		setup_frame(sig, ka, oldset, regs);

	if (ka->sa.sa_flags & SA_ONESHOT)
		ka->sa.sa_handler = SIG_DFL;

	if (!(ka->sa.sa_flags & SA_NODEFER)) {
		spin_lock_irq(&current->sighand->siglock);
		sigorsets(&current->blocked,&current->blocked,&ka->sa.sa_mask);
		sigaddset(&current->blocked,sig);
		recalc_sigpending();
		spin_unlock_irq(&current->sighand->siglock);
	}
}

/*
 * Note that 'init' is a special process: it doesn't get signals it doesn't
 * want to handle. Thus you cannot kill init even with a SIGKILL even by
 * mistake.
 *
 * Note that we go through the signals twice: once to check the signals that
 * the kernel can handle, and then we build all the user-level signal handling
 * stack-frames in one go after that.
 */
int do_signal(struct pt_regs *regs, sigset_t *oldset)
{
	unsigned long retval = 0, continue_addr = 0, restart_addr = 0;
	siginfo_t info;
	int signr;

	/*
	 * We want the common case to go fast, which
	 * is why we may in certain cases get here from
	 * kernel mode. Just return without doing anything
	 * if so.
	 */
	if (!user_mode(regs))
		return 1;

	if (!oldset)
		oldset = &current->blocked;

	/* Are we from a system call? */
	if (regs->trap == __LC_SVC_OLD_PSW) {
		continue_addr = regs->psw.addr;
		restart_addr = continue_addr - regs->ilc;
		retval = regs->gprs[2];

		/* Prepare for system call restart.  We do this here so that a
		   debugger will see the already changed PSW. */
		if (retval == -ERESTARTNOHAND ||
		    retval == -ERESTARTSYS ||
		    retval == -ERESTARTNOINTR) {
			regs->gprs[2] = regs->orig_gpr2;
			regs->psw.addr = restart_addr;
		} else if (retval == -ERESTART_RESTARTBLOCK) {
			regs->gprs[2] = -EINTR;
		}
	}

	/* Get signal to deliver.  When running under ptrace, at this point
	   the debugger may change all our registers ... */
	signr = get_signal_to_deliver(&info, regs, NULL);

	/* Depending on the signal settings we may need to revert the
	   decision to restart the system call. */
	if (signr > 0 && regs->psw.addr == restart_addr) {
		if (retval == -ERESTARTNOHAND
		    || (retval == -ERESTARTSYS
			 && !(current->sighand->action[signr-1].sa.sa_flags
			      & SA_RESTART))) {
			regs->gprs[2] = -EINTR;
			regs->psw.addr = continue_addr;
		}
	}

	if (signr > 0) {
		/* Whee!  Actually deliver the signal.  */
#ifdef CONFIG_S390_SUPPORT
		if (test_thread_flag(TIF_31BIT)) {
			extern void handle_signal32(unsigned long sig,
						    siginfo_t *info,
						    sigset_t *oldset,
						    struct pt_regs *regs);
			handle_signal32(signr, &info, oldset, regs);
			return 1;
	        }
#endif
		handle_signal(signr, &info, oldset, regs);
		return 1;
	}

	/* Restart a different system call. */
	if (retval == -ERESTART_RESTARTBLOCK
	    && regs->psw.addr == continue_addr) {
		regs->gprs[2] = __NR_restart_syscall;
		set_thread_flag(TIF_RESTART_SVC);
	}
	return 0;
}
