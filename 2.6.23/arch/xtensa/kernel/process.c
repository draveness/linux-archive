/*
 * arch/xtensa/kernel/process.c
 *
 * Xtensa Processor version.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2001 - 2005 Tensilica Inc.
 *
 * Joe Taylor <joe@tensilica.com, joetylr@yahoo.com>
 * Chris Zankel <chris@zankel.net>
 * Marc Gauthier <marc@tensilica.com, marc@alumni.uwaterloo.ca>
 * Kevin Chea
 */

#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/stddef.h>
#include <linux/unistd.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/elf.h>
#include <linux/init.h>
#include <linux/prctl.h>
#include <linux/init_task.h>
#include <linux/module.h>
#include <linux/mqueue.h>
#include <linux/fs.h>

#include <asm/pgtable.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/processor.h>
#include <asm/platform.h>
#include <asm/mmu.h>
#include <asm/irq.h>
#include <asm/atomic.h>
#include <asm/asm-offsets.h>
#include <asm/regs.h>

extern void ret_from_fork(void);

struct task_struct *current_set[NR_CPUS] = {&init_task, };

void (*pm_power_off)(void) = NULL;
EXPORT_SYMBOL(pm_power_off);


/*
 * Powermanagement idle function, if any is provided by the platform.
 */

void cpu_idle(void)
{
  	local_irq_enable();

	/* endless idle loop with no priority at all */
	while (1) {
		while (!need_resched())
			platform_idle();
		preempt_enable_no_resched();
		schedule();
		preempt_disable();
	}
}

/*
 * Free current thread data structures etc..
 */

void exit_thread(void)
{
}

void flush_thread(void)
{
}

/*
 * Copy thread.
 *
 * The stack layout for the new thread looks like this:
 *
 *	+------------------------+ <- sp in childregs (= tos)
 *	|       childregs        |
 *	+------------------------+ <- thread.sp = sp in dummy-frame
 *	|      dummy-frame       |    (saved in dummy-frame spill-area)
 *	+------------------------+
 *
 * We create a dummy frame to return to ret_from_fork:
 *   a0 points to ret_from_fork (simulating a call4)
 *   sp points to itself (thread.sp)
 *   a2, a3 are unused.
 *
 * Note: This is a pristine frame, so we don't need any spill region on top of
 *       childregs.
 */

int copy_thread(int nr, unsigned long clone_flags, unsigned long usp,
		unsigned long unused,
                struct task_struct * p, struct pt_regs * regs)
{
	struct pt_regs *childregs;
	unsigned long tos;
	int user_mode = user_mode(regs);

	/* Set up new TSS. */
	tos = (unsigned long)task_stack_page(p) + THREAD_SIZE;
	if (user_mode)
		childregs = (struct pt_regs*)(tos - PT_USER_SIZE);
	else
		childregs = (struct pt_regs*)tos - 1;

	*childregs = *regs;

	/* Create a call4 dummy-frame: a0 = 0, a1 = childregs. */
	*((int*)childregs - 3) = (unsigned long)childregs;
	*((int*)childregs - 4) = 0;

	childregs->areg[1] = tos;
	childregs->areg[2] = 0;
	p->set_child_tid = p->clear_child_tid = NULL;
	p->thread.ra = MAKE_RA_FOR_CALL((unsigned long)ret_from_fork, 0x1);
	p->thread.sp = (unsigned long)childregs;
	if (user_mode(regs)) {

		int len = childregs->wmask & ~0xf;
		childregs->areg[1] = usp;
		memcpy(&childregs->areg[XCHAL_NUM_AREGS - len/4],
		       &regs->areg[XCHAL_NUM_AREGS - len/4], len);

		if (clone_flags & CLONE_SETTLS)
			childregs->areg[2] = childregs->areg[6];

	} else {
		/* In kernel space, we start a new thread with a new stack. */
		childregs->wmask = 1;
	}
	return 0;
}


/*
 * These bracket the sleeping functions..
 */

unsigned long get_wchan(struct task_struct *p)
{
	unsigned long sp, pc;
	unsigned long stack_page = (unsigned long) task_stack_page(p);
	int count = 0;

	if (!p || p == current || p->state == TASK_RUNNING)
		return 0;

	sp = p->thread.sp;
	pc = MAKE_PC_FROM_RA(p->thread.ra, p->thread.sp);

	do {
		if (sp < stack_page + sizeof(struct task_struct) ||
		    sp >= (stack_page + THREAD_SIZE) ||
		    pc == 0)
			return 0;
		if (!in_sched_functions(pc))
			return pc;

		/* Stack layout: sp-4: ra, sp-3: sp' */

		pc = MAKE_PC_FROM_RA(*(unsigned long*)sp - 4, sp);
		sp = *(unsigned long *)sp - 3;
	} while (count++ < 16);
	return 0;
}

/*
 * do_copy_regs() gathers information from 'struct pt_regs' and
 * 'current->thread.areg[]' to fill in the xtensa_gregset_t
 * structure.
 *
 * xtensa_gregset_t and 'struct pt_regs' are vastly different formats
 * of processor registers.  Besides different ordering,
 * xtensa_gregset_t contains non-live register information that
 * 'struct pt_regs' does not.  Exception handling (primarily) uses
 * 'struct pt_regs'.  Core files and ptrace use xtensa_gregset_t.
 *
 */

void do_copy_regs (xtensa_gregset_t *elfregs, struct pt_regs *regs,
		   struct task_struct *tsk)
{
	int i, n, wb_offset;

	elfregs->xchal_config_id0 = XCHAL_HW_CONFIGID0;
	elfregs->xchal_config_id1 = XCHAL_HW_CONFIGID1;

	__asm__ __volatile__ ("rsr  %0, 176\n" : "=a" (i));
 	elfregs->cpux = i;
	__asm__ __volatile__ ("rsr  %0, 208\n" : "=a" (i));
 	elfregs->cpuy = i;

	/* Note:  PS.EXCM is not set while user task is running; its
	 * being set in regs->ps is for exception handling convenience.
	 */

	elfregs->pc		= regs->pc;
	elfregs->ps		= (regs->ps & ~(1 << PS_EXCM_BIT));
	elfregs->exccause	= regs->exccause;
	elfregs->excvaddr	= regs->excvaddr;
	elfregs->windowbase	= regs->windowbase;
	elfregs->windowstart	= regs->windowstart;
	elfregs->lbeg		= regs->lbeg;
	elfregs->lend		= regs->lend;
	elfregs->lcount		= regs->lcount;
	elfregs->sar		= regs->sar;
	elfregs->syscall	= regs->syscall;

	/* Copy register file.
	 * The layout looks like this:
	 *
	 * |  a0 ... a15  | Z ... Z |  arX ... arY  |
	 *  current window  unused    saved frames
	 */

	memset (elfregs->ar, 0, sizeof(elfregs->ar));

	wb_offset = regs->windowbase * 4;
	n = (regs->wmask&1)? 4 : (regs->wmask&2)? 8 : (regs->wmask&4)? 12 : 16;

	for (i = 0; i < n; i++)
		elfregs->ar[(wb_offset + i) % XCHAL_NUM_AREGS] = regs->areg[i];

	n = (regs->wmask >> 4) * 4;

	for (i = XCHAL_NUM_AREGS - n; n > 0; i++, n--)
		elfregs->ar[(wb_offset + i) % XCHAL_NUM_AREGS] = regs->areg[i];
}

void xtensa_elf_core_copy_regs (xtensa_gregset_t *elfregs, struct pt_regs *regs)
{
	do_copy_regs ((xtensa_gregset_t *)elfregs, regs, current);
}


/* The inverse of do_copy_regs().  No error or sanity checking. */

void do_restore_regs (xtensa_gregset_t *elfregs, struct pt_regs *regs,
		      struct task_struct *tsk)
{
	int i, n, wb_offset;

	/* Note:  PS.EXCM is not set while user task is running; it
	 * needs to be set in regs->ps is for exception handling convenience.
	 */

	regs->pc		= elfregs->pc;
	regs->ps		= (elfregs->ps | (1 << PS_EXCM_BIT));
	regs->exccause		= elfregs->exccause;
	regs->excvaddr		= elfregs->excvaddr;
	regs->windowbase	= elfregs->windowbase;
	regs->windowstart	= elfregs->windowstart;
	regs->lbeg		= elfregs->lbeg;
	regs->lend		= elfregs->lend;
	regs->lcount		= elfregs->lcount;
	regs->sar		= elfregs->sar;
	regs->syscall	= elfregs->syscall;

	/* Clear everything. */

	memset (regs->areg, 0, sizeof(regs->areg));

	/* Copy regs from live window frame. */

	wb_offset = regs->windowbase * 4;
	n = (regs->wmask&1)? 4 : (regs->wmask&2)? 8 : (regs->wmask&4)? 12 : 16;

	for (i = 0; i < n; i++)
		regs->areg[(wb_offset+i) % XCHAL_NUM_AREGS] = elfregs->ar[i];

	n = (regs->wmask >> 4) * 4;

	for (i = XCHAL_NUM_AREGS - n; n > 0; i++, n--)
		regs->areg[(wb_offset+i) % XCHAL_NUM_AREGS] = elfregs->ar[i];
}

/*
 * do_save_fpregs() gathers information from 'struct pt_regs' and
 * 'current->thread' to fill in the elf_fpregset_t structure.
 *
 * Core files and ptrace use elf_fpregset_t.
 */

void do_save_fpregs (elf_fpregset_t *fpregs, struct pt_regs *regs,
		     struct task_struct *tsk)
{
#if XCHAL_HAVE_CP

	extern unsigned char	_xtensa_reginfo_tables[];
	extern unsigned		_xtensa_reginfo_table_size;
	int i;
	unsigned long flags;

	/* Before dumping coprocessor state from memory,
	 * ensure any live coprocessor contents for this
	 * task are first saved to memory:
	 */
	local_irq_save(flags);

	for (i = 0; i < XCHAL_CP_MAX; i++) {
		if (tsk == coprocessor_info[i].owner) {
			enable_coprocessor(i);
			save_coprocessor_registers(
			    tsk->thread.cp_save+coprocessor_info[i].offset,i);
			disable_coprocessor(i);
		}
	}

	local_irq_restore(flags);

	/* Now dump coprocessor & extra state: */
	memcpy((unsigned char*)fpregs,
		_xtensa_reginfo_tables, _xtensa_reginfo_table_size);
	memcpy((unsigned char*)fpregs + _xtensa_reginfo_table_size,
		tsk->thread.cp_save, XTENSA_CP_EXTRA_SIZE);
#endif
}

/*
 * The inverse of do_save_fpregs().
 * Copies coprocessor and extra state from fpregs into regs and tsk->thread.
 * Returns 0 on success, non-zero if layout doesn't match.
 */

int  do_restore_fpregs (elf_fpregset_t *fpregs, struct pt_regs *regs,
		        struct task_struct *tsk)
{
#if XCHAL_HAVE_CP

	extern unsigned char	_xtensa_reginfo_tables[];
	extern unsigned		_xtensa_reginfo_table_size;
	int i;
	unsigned long flags;

	/* Make sure save area layouts match.
	 * FIXME:  in the future we could allow restoring from
	 * a different layout of the same registers, by comparing
	 * fpregs' table with _xtensa_reginfo_tables and matching
	 * entries and copying registers one at a time.
	 * Not too sure yet whether that's very useful.
	 */

	if( memcmp((unsigned char*)fpregs,
		_xtensa_reginfo_tables, _xtensa_reginfo_table_size) ) {
	    return -1;
	}

	/* Before restoring coprocessor state from memory,
	 * ensure any live coprocessor contents for this
	 * task are first invalidated.
	 */

	local_irq_save(flags);

	for (i = 0; i < XCHAL_CP_MAX; i++) {
		if (tsk == coprocessor_info[i].owner) {
			enable_coprocessor(i);
			save_coprocessor_registers(
			    tsk->thread.cp_save+coprocessor_info[i].offset,i);
			coprocessor_info[i].owner = 0;
			disable_coprocessor(i);
		}
	}

	local_irq_restore(flags);

	/*  Now restore coprocessor & extra state:  */

	memcpy(tsk->thread.cp_save,
		(unsigned char*)fpregs + _xtensa_reginfo_table_size,
		XTENSA_CP_EXTRA_SIZE);
#endif
	return 0;
}
/*
 * Fill in the CP structure for a core dump for a particular task.
 */

int
dump_task_fpu(struct pt_regs *regs, struct task_struct *task, elf_fpregset_t *r)
{
	return 0;	/* no coprocessors active on this processor */
}

/*
 * Fill in the CP structure for a core dump.
 * This includes any FPU coprocessor.
 * Here, we dump all coprocessors, and other ("extra") custom state.
 *
 * This function is called by elf_core_dump() in fs/binfmt_elf.c
 * (in which case 'regs' comes from calls to do_coredump, see signals.c).
 */
int  dump_fpu(struct pt_regs *regs, elf_fpregset_t *r)
{
	return dump_task_fpu(regs, current, r);
}

asmlinkage
long xtensa_clone(unsigned long clone_flags, unsigned long newsp,
                  void __user *parent_tid, void *child_tls,
                  void __user *child_tid, long a5,
                  struct pt_regs *regs)
{
        if (!newsp)
                newsp = regs->areg[1];
        return do_fork(clone_flags, newsp, regs, 0, parent_tid, child_tid);
}

/*
 *  * xtensa_execve() executes a new program.
 *   */

asmlinkage
long xtensa_execve(char __user *name, char __user * __user *argv,
                   char __user * __user *envp,
                   long a3, long a4, long a5,
                   struct pt_regs *regs)
{
	long error;
	char * filename;

	filename = getname(name);
	error = PTR_ERR(filename);
	if (IS_ERR(filename))
		goto out;
	// FIXME: release coprocessor??
	error = do_execve(filename, argv, envp, regs);
	if (error == 0) {
		task_lock(current);
		current->ptrace &= ~PT_DTRACE;
		task_unlock(current);
	}
	putname(filename);
out:
	return error;
}

