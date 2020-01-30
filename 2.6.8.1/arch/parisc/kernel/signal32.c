/*    Signal support for 32-bit kernel builds
 *
 *    Copyright (C) 2001 Matthew Wilcox <willy at parisc-linux.org>
 *    Code was mostly borrowed from kernel/signal.c.
 *    See kernel/signal.c for additional Copyrights.
 *
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/config.h>
#include <linux/compat.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/unistd.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/syscalls.h>
#include <linux/types.h>
#include <linux/errno.h>

#include <asm/compat_signal.h>
#include <asm/uaccess.h>

#include "signal32.h"
#include "sys32.h"

#define DEBUG_COMPAT_SIG 0 
#define DEBUG_COMPAT_SIG_LEVEL 2

#if DEBUG_COMPAT_SIG
#define DBG(LEVEL, ...) \
	((DEBUG_COMPAT_SIG_LEVEL >= LEVEL) \
	? printk(__VA_ARGS__) : (void) 0)
#else
#define DBG(LEVEL, ...)
#endif

#define _BLOCKABLE (~(sigmask(SIGKILL) | sigmask(SIGSTOP)))

inline void
sigset_32to64(sigset_t *s64, compat_sigset_t *s32)
{
	s64->sig[0] = s32->sig[0] | ((unsigned long)s32->sig[1] << 32);
}

inline void
sigset_64to32(compat_sigset_t *s32, sigset_t *s64)
{
	s32->sig[0] = s64->sig[0] & 0xffffffffUL;
	s32->sig[1] = (s64->sig[0] >> 32) & 0xffffffffUL;
}

static int
put_sigset32(compat_sigset_t *up, sigset_t *set, size_t sz)
{
	compat_sigset_t s;

	if (sz != sizeof *set) panic("put_sigset32()");
	sigset_64to32(&s, set);

	return copy_to_user(up, &s, sizeof s);
}

static int
get_sigset32(compat_sigset_t *up, sigset_t *set, size_t sz)
{
	compat_sigset_t s;
	int r;

	if (sz != sizeof *set) panic("put_sigset32()");

	if ((r = copy_from_user(&s, up, sz)) == 0) {
		sigset_32to64(set, &s);
	}

	return r;
}

int sys32_rt_sigprocmask(int how, compat_sigset_t *set, compat_sigset_t *oset,
				    unsigned int sigsetsize)
{
	sigset_t old_set, new_set;
	int ret;

	if (set && get_sigset32(set, &new_set, sigsetsize))
		return -EFAULT;
	
	KERNEL_SYSCALL(ret, sys_rt_sigprocmask, how, set ? &new_set : NULL,
				 oset ? &old_set : NULL, sigsetsize);

	if (!ret && oset && put_sigset32(oset, &old_set, sigsetsize))
		return -EFAULT;

	return ret;
}


int sys32_rt_sigpending(compat_sigset_t *uset, unsigned int sigsetsize)
{
	int ret;
	sigset_t set;

	KERNEL_SYSCALL(ret, sys_rt_sigpending, &set, sigsetsize);

	if (!ret && put_sigset32(uset, &set, sigsetsize))
		return -EFAULT;

	return ret;
}

long
sys32_rt_sigaction(int sig, const struct sigaction32 *act, struct sigaction32 *oact,
                 size_t sigsetsize)
{
	struct k_sigaction32 new_sa32, old_sa32;
	struct k_sigaction new_sa, old_sa;
	int ret = -EINVAL;

	if (act) {
		if (copy_from_user(&new_sa32.sa, act, sizeof new_sa32.sa))
			return -EFAULT;
		new_sa.sa.sa_handler = (__sighandler_t)(unsigned long)new_sa32.sa.sa_handler;
		new_sa.sa.sa_flags = new_sa32.sa.sa_flags;
		sigset_32to64(&new_sa.sa.sa_mask, &new_sa32.sa.sa_mask);
	}

	ret = do_sigaction(sig, act ? &new_sa : NULL, oact ? &old_sa : NULL);

	if (!ret && oact) {
		sigset_64to32(&old_sa32.sa.sa_mask, &old_sa.sa.sa_mask);
		old_sa32.sa.sa_flags = old_sa.sa.sa_flags;
		old_sa32.sa.sa_handler = (__sighandler_t32)(unsigned long)old_sa.sa.sa_handler;
		if (copy_to_user(oact, &old_sa32.sa, sizeof old_sa32.sa))
			return -EFAULT;
	}
	return ret;
}

int 
do_sigaltstack32 (const compat_stack_t *uss32, compat_stack_t *uoss32, unsigned long sp)
{
	compat_stack_t ss32, oss32;
	stack_t ss, oss;
	stack_t *ssp = NULL, *ossp = NULL;
	int ret;

	if (uss32) {
		if (copy_from_user(&ss32, uss32, sizeof ss32))
			return -EFAULT;

		ss.ss_sp = (void *)(unsigned long)ss32.ss_sp;
		ss.ss_flags = ss32.ss_flags;
		ss.ss_size = ss32.ss_size;

		ssp = &ss;
	}

	if (uoss32)
		ossp = &oss;

	KERNEL_SYSCALL(ret, do_sigaltstack, ssp, ossp, sp);

	if (!ret && uoss32) {
		oss32.ss_sp = (unsigned int)(unsigned long)oss.ss_sp;
		oss32.ss_flags = oss.ss_flags;
		oss32.ss_size = oss.ss_size;
		if (copy_to_user(uoss32, &oss32, sizeof *uoss32))
			return -EFAULT;
	}

	return ret;
}

long
restore_sigcontext32(struct compat_sigcontext *sc, struct compat_regfile * rf,
		struct pt_regs *regs)
{
	long err = 0;
	compat_uint_t compat_reg;
	compat_uint_t compat_regt;
	int regn;
	
	/* When loading 32-bit values into 64-bit registers make
	   sure to clear the upper 32-bits */
	DBG(2,"restore_sigcontext32: PER_LINUX32 process\n");
	DBG(2,"restore_sigcontext32: sc = 0x%p, rf = 0x%p, regs = 0x%p\n", sc, rf, regs);
	DBG(2,"restore_sigcontext32: compat_sigcontext is %#lx bytes\n", sizeof(*sc));
	for(regn=0; regn < 32; regn++){
		err |= __get_user(compat_reg,&sc->sc_gr[regn]);
		regs->gr[regn] = compat_reg;
		/* Load upper half */
		err |= __get_user(compat_regt,&rf->rf_gr[regn]);
		regs->gr[regn] = ((u64)compat_regt << 32) | (u64)compat_reg;
		DBG(3,"restore_sigcontext32: gr%02d = %#lx (%#x / %#x)\n", 
				regn, regs->gr[regn], compat_regt, compat_reg);
	}
	DBG(2,"restore_sigcontext32: sc->sc_fr = 0x%p (%#lx)\n",sc->sc_fr, sizeof(sc->sc_fr));
	/* XXX: BE WARNED FR's are 64-BIT! */
	err |= __copy_from_user(regs->fr, sc->sc_fr, sizeof(regs->fr));
		
	/* Better safe than sorry, pass __get_user two things of
	   the same size and let gcc do the upward conversion to 
	   64-bits */		
	err |= __get_user(compat_reg, &sc->sc_iaoq[0]);
	/* Load upper half */
	err |= __get_user(compat_regt, &rf->rf_iaoq[0]);
	regs->iaoq[0] = ((u64)compat_regt << 32) | (u64)compat_reg;
	DBG(2,"restore_sigcontext32: upper half of iaoq[0] = %#lx\n", compat_regt);
	DBG(2,"restore_sigcontext32: sc->sc_iaoq[0] = %p => %#x\n", 
			&sc->sc_iaoq[0], compat_reg);

	err |= __get_user(compat_reg, &sc->sc_iaoq[1]);
	/* Load upper half */
	err |= __get_user(compat_regt, &rf->rf_iaoq[1]);
	regs->iaoq[1] = ((u64)compat_regt << 32) | (u64)compat_reg;
	DBG(2,"restore_sigcontext32: upper half of iaoq[1] = %#lx\n", compat_regt);
	DBG(2,"restore_sigcontext32: sc->sc_iaoq[1] = %p => %#x\n", 
			&sc->sc_iaoq[1],compat_reg);	
	DBG(2,"restore_sigcontext32: iaoq is %#lx / %#lx\n", 
			regs->iaoq[0],regs->iaoq[1]);		
		
	err |= __get_user(compat_reg, &sc->sc_iasq[0]);
	/* Load the upper half for iasq */
	err |= __get_user(compat_regt, &rf->rf_iasq[0]);
	regs->iasq[0] = ((u64)compat_regt << 32) | (u64)compat_reg;
	DBG(2,"restore_sigcontext32: upper half of iasq[0] = %#lx\n", compat_regt);
	
	err |= __get_user(compat_reg, &sc->sc_iasq[1]);
	/* Load the upper half for iasq */
	err |= __get_user(compat_regt, &rf->rf_iasq[1]);
	regs->iasq[1] = ((u64)compat_regt << 32) | (u64)compat_reg;
	DBG(2,"restore_sigcontext32: upper half of iasq[1] = %#lx\n", compat_regt);
	DBG(2,"restore_sigcontext32: iasq is %#lx / %#lx\n", 
		regs->iasq[0],regs->iasq[1]);		

	err |= __get_user(compat_reg, &sc->sc_sar);
	/* Load the upper half for sar */
	err |= __get_user(compat_regt, &rf->rf_sar);
	regs->sar = ((u64)compat_regt << 32) | (u64)compat_reg;	
	DBG(2,"restore_sigcontext32: upper_half & sar = %#lx\n", compat_regt);	
	DBG(2,"restore_sigcontext32: sar is %#lx\n", regs->sar);		
	DBG(2,"restore_sigcontext32: r28 is %ld\n", regs->gr[28]);
	
	return err;
}

/*
 * Set up the sigcontext structure for this process.
 * This is not an easy task if the kernel is 64-bit, it will require
 * that we examine the process personality to determine if we need to
 * truncate for a 32-bit userspace.
 */
long
setup_sigcontext32(struct compat_sigcontext *sc, struct compat_regfile * rf, 
		struct pt_regs *regs, int in_syscall)		 
{
	compat_int_t flags = 0;
	long err = 0;
	compat_uint_t compat_reg;
	compat_uint_t compat_regb;
	int regn;
	
	if (on_sig_stack((unsigned long) sc))
		flags |= PARISC_SC_FLAG_ONSTACK;
	
	if (in_syscall) {
		
		DBG(1,"setup_sigcontext32: in_syscall\n");
		
		flags |= PARISC_SC_FLAG_IN_SYSCALL;
		/* Truncate gr31 */
		compat_reg = (compat_uint_t)(regs->gr[31]);
		/* regs->iaoq is undefined in the syscall return path */
		err |= __put_user(compat_reg, &sc->sc_iaoq[0]);
		DBG(2,"setup_sigcontext32: sc->sc_iaoq[0] = %p <= %#x\n",
				&sc->sc_iaoq[0], compat_reg);
		
		/* Store upper half */
		compat_reg = (compat_uint_t)(regs->gr[32] >> 32);
		err |= __put_user(compat_reg, &rf->rf_iaoq[0]);
		DBG(2,"setup_sigcontext32: upper half iaoq[0] = %#x\n", compat_reg);
		
		
		compat_reg = (compat_uint_t)(regs->gr[31]+4);
		err |= __put_user(compat_reg, &sc->sc_iaoq[1]);
		DBG(2,"setup_sigcontext32: sc->sc_iaoq[1] = %p <= %#x\n",
				&sc->sc_iaoq[1], compat_reg);
		/* Store upper half */
		compat_reg = (compat_uint_t)((regs->gr[32]+4) >> 32);
		err |= __put_user(compat_reg, &rf->rf_iaoq[1]);
		DBG(2,"setup_sigcontext32: upper half iaoq[1] = %#x\n", compat_reg);
		
		/* Truncate sr3 */
		compat_reg = (compat_uint_t)(regs->sr[3]);
		err |= __put_user(compat_reg, &sc->sc_iasq[0]);
		err |= __put_user(compat_reg, &sc->sc_iasq[1]);		
		
		/* Store upper half */
		compat_reg = (compat_uint_t)(regs->sr[3] >> 32);
		err |= __put_user(compat_reg, &rf->rf_iasq[0]);
		err |= __put_user(compat_reg, &rf->rf_iasq[1]);		
		
		DBG(2,"setup_sigcontext32: upper half iasq[0] = %#x\n", compat_reg);
		DBG(2,"setup_sigcontext32: upper half iasq[1] = %#x\n", compat_reg);		
		DBG(1,"setup_sigcontext32: iaoq %#lx / %#lx\n",				
			regs->gr[31], regs->gr[31]+4);
		
	} else {
		
		compat_reg = (compat_uint_t)(regs->iaoq[0]);
		err |= __put_user(compat_reg, &sc->sc_iaoq[0]);
		DBG(2,"setup_sigcontext32: sc->sc_iaoq[0] = %p <= %#x\n",
				&sc->sc_iaoq[0], compat_reg);
		/* Store upper half */
		compat_reg = (compat_uint_t)(regs->iaoq[0] >> 32);
		err |= __put_user(compat_reg, &rf->rf_iaoq[0]);	
		DBG(2,"setup_sigcontext32: upper half iaoq[0] = %#x\n", compat_reg);
		
		compat_reg = (compat_uint_t)(regs->iaoq[1]);
		err |= __put_user(compat_reg, &sc->sc_iaoq[1]);
		DBG(2,"setup_sigcontext32: sc->sc_iaoq[1] = %p <= %#x\n",
				&sc->sc_iaoq[1], compat_reg);
		/* Store upper half */
		compat_reg = (compat_uint_t)(regs->iaoq[1] >> 32);
		err |= __put_user(compat_reg, &rf->rf_iaoq[1]);
		DBG(2,"setup_sigcontext32: upper half iaoq[1] = %#x\n", compat_reg);
		
		
		compat_reg = (compat_uint_t)(regs->iasq[0]);
		err |= __put_user(compat_reg, &sc->sc_iasq[0]);
		DBG(2,"setup_sigcontext32: sc->sc_iasq[0] = %p <= %#x\n",
				&sc->sc_iasq[0], compat_reg);
		/* Store upper half */
		compat_reg = (compat_uint_t)(regs->iasq[0] >> 32);
		err |= __put_user(compat_reg, &rf->rf_iasq[0]);
		DBG(2,"setup_sigcontext32: upper half iasq[0] = %#x\n", compat_reg);
		
		
		compat_reg = (compat_uint_t)(regs->iasq[1]);
		err |= __put_user(compat_reg, &sc->sc_iasq[1]);
		DBG(2,"setup_sigcontext32: sc->sc_iasq[1] = %p <= %#x\n",
				&sc->sc_iasq[1], compat_reg);
		/* Store upper half */
		compat_reg = (compat_uint_t)(regs->iasq[1] >> 32);
		err |= __put_user(compat_reg, &rf->rf_iasq[1]);
		DBG(2,"setup_sigcontext32: upper half iasq[1] = %#x\n", compat_reg);

		/* Print out the IAOQ for debugging */		
		DBG(1,"setup_sigcontext32: ia0q %#lx / %#lx\n", 
			regs->iaoq[0], regs->iaoq[1]);
	}

	err |= __put_user(flags, &sc->sc_flags);
	
	DBG(1,"setup_sigcontext32: Truncating general registers.\n");
	
	for(regn=0; regn < 32; regn++){
		/* Truncate a general register */
		compat_reg = (compat_uint_t)(regs->gr[regn]);
		err |= __put_user(compat_reg, &sc->sc_gr[regn]);
		/* Store upper half */
		compat_regb = (compat_uint_t)(regs->gr[regn] >> 32);
		err |= __put_user(compat_regb, &rf->rf_gr[regn]);

		/* DEBUG: Write out the "upper / lower" register data */
		DBG(2,"setup_sigcontext32: gr%02d = %#x / %#x\n", regn, 
				compat_regb, compat_reg);
	}
	
	/* Copy the floating point registers (same size)
	   XXX: BE WARNED FR's are 64-BIT! */	
	DBG(1,"setup_sigcontext32: Copying from regs to sc, "
	      "sc->sc_fr size = %#lx, regs->fr size = %#lx\n",
		sizeof(regs->fr), sizeof(sc->sc_fr));
	err |= __copy_to_user(sc->sc_fr, regs->fr, sizeof(regs->fr));

	compat_reg = (compat_uint_t)(regs->sar);
	err |= __put_user(compat_reg, &sc->sc_sar);
	DBG(2,"setup_sigcontext32: sar is %#x\n", compat_reg);
	/* Store upper half */
	compat_reg = (compat_uint_t)(regs->sar >> 32);
	err |= __put_user(compat_reg, &rf->rf_sar);	
	DBG(2,"setup_sigcontext32: upper half sar = %#x\n", compat_reg);
	DBG(1,"setup_sigcontext32: r28 is %ld\n", regs->gr[28]);

	return err;
}
