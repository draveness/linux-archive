/*
 *  arch/s390/kernel/ptrace.c
 *
 *  S390 version
 *    Copyright (C) 1999,2000 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Denis Joseph Barrow (djbarrow@de.ibm.com,barrow_dj@yahoo.com),
 *               Martin Schwidefsky (schwidefsky@de.ibm.com)
 *
 *  Based on PowerPC version 
 *    Copyright (C) 1995-1996 Gary Thomas (gdt@linuxppc.org)
 *
 *  Derived from "arch/m68k/kernel/ptrace.c"
 *  Copyright (C) 1994 by Hamish Macdonald
 *  Taken from linux/kernel/ptrace.c and modified for M680x0.
 *  linux/kernel/ptrace.c is by Ross Biro 1/23/92, edited by Linus Torvalds
 *
 * Modified by Cort Dougan (cort@cs.nmt.edu) 
 *
 *
 * This file is subject to the terms and conditions of the GNU General
 * Public License.  See the file README.legal in the main directory of
 * this archive for more details.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/errno.h>
#include <linux/ptrace.h>
#include <linux/user.h>
#include <linux/security.h>

#include <asm/segment.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/system.h>
#include <asm/uaccess.h>

#ifdef CONFIG_S390_SUPPORT
#include "compat_ptrace.h"
#endif

static void
FixPerRegisters(struct task_struct *task)
{
	struct pt_regs *regs;
	per_struct *per_info;

	regs = __KSTK_PTREGS(task);
	per_info = (per_struct *) &task->thread.per_info;
	per_info->control_regs.bits.em_instruction_fetch =
		per_info->single_step | per_info->instruction_fetch;
	
	if (per_info->single_step) {
		per_info->control_regs.bits.starting_addr = 0;
#ifdef CONFIG_S390_SUPPORT
		if (test_thread_flag(TIF_31BIT))
			per_info->control_regs.bits.ending_addr = 0x7fffffffUL;
		else
#endif
			per_info->control_regs.bits.ending_addr = PSW_ADDR_INSN;
	} else {
		per_info->control_regs.bits.starting_addr =
			per_info->starting_addr;
		per_info->control_regs.bits.ending_addr =
			per_info->ending_addr;
	}
	/*
	 * if any of the control reg tracing bits are on 
	 * we switch on per in the psw
	 */
	if (per_info->control_regs.words.cr[0] & PER_EM_MASK)
		regs->psw.mask |= PSW_MASK_PER;
	else
		regs->psw.mask &= ~PSW_MASK_PER;

	if (per_info->control_regs.bits.em_storage_alteration)
		per_info->control_regs.bits.storage_alt_space_ctl = 1;
	else
		per_info->control_regs.bits.storage_alt_space_ctl = 0;
}

void
set_single_step(struct task_struct *task)
{
	task->thread.per_info.single_step = 1;
	FixPerRegisters(task);
}

void
clear_single_step(struct task_struct *task)
{
	task->thread.per_info.single_step = 0;
	FixPerRegisters(task);
}

/*
 * Called by kernel/ptrace.c when detaching..
 *
 * Make sure single step bits etc are not set.
 */
void
ptrace_disable(struct task_struct *child)
{
	/* make sure the single step bit is not set. */
	clear_single_step(child);
}

#ifndef CONFIG_ARCH_S390X
# define __ADDR_MASK 3
#else
# define __ADDR_MASK 7
#endif

/*
 * Read the word at offset addr from the user area of a process. The
 * trouble here is that the information is littered over different
 * locations. The process registers are found on the kernel stack,
 * the floating point stuff and the trace settings are stored in
 * the task structure. In addition the different structures in
 * struct user contain pad bytes that should be read as zeroes.
 * Lovely...
 */
static int
peek_user(struct task_struct *child, addr_t addr, addr_t data)
{
	struct user *dummy = NULL;
	addr_t offset, tmp;

	/*
	 * Stupid gdb peeks/pokes the access registers in 64 bit with
	 * an alignment of 4. Programmers from hell...
	 */
	if ((addr & 3) || addr > sizeof(struct user) - __ADDR_MASK)
		return -EIO;

	if (addr < (addr_t) &dummy->regs.acrs) {
		/*
		 * psw and gprs are stored on the stack
		 */
		tmp = *(addr_t *)((addr_t) &__KSTK_PTREGS(child)->psw + addr);
		if (addr == (addr_t) &dummy->regs.psw.mask)
			/* Remove per bit from user psw. */
			tmp &= ~PSW_MASK_PER;

	} else if (addr < (addr_t) &dummy->regs.orig_gpr2) {
		/*
		 * access registers are stored in the thread structure
		 */
		offset = addr - (addr_t) &dummy->regs.acrs;
		tmp = *(addr_t *)((addr_t) &child->thread.acrs + offset);

	} else if (addr == (addr_t) &dummy->regs.orig_gpr2) {
		/*
		 * orig_gpr2 is stored on the kernel stack
		 */
		tmp = (addr_t) __KSTK_PTREGS(child)->orig_gpr2;

	} else if (addr < (addr_t) (&dummy->regs.fp_regs + 1)) {
		/* 
		 * floating point regs. are stored in the thread structure
		 */
		offset = addr - (addr_t) &dummy->regs.fp_regs;
		tmp = *(addr_t *)((addr_t) &child->thread.fp_regs + offset);

	} else if (addr < (addr_t) (&dummy->regs.per_info + 1)) {
		/*
		 * per_info is found in the thread structure
		 */
		offset = addr - (addr_t) &dummy->regs.per_info;
		tmp = *(addr_t *)((addr_t) &child->thread.per_info + offset);

	} else
		tmp = 0;

	return put_user(tmp, (addr_t __user *) data);
}

/*
 * Write a word to the user area of a process at location addr. This
 * operation does have an additional problem compared to peek_user.
 * Stores to the program status word and on the floating point
 * control register needs to get checked for validity.
 */
static int
poke_user(struct task_struct *child, addr_t addr, addr_t data)
{
	struct user *dummy = NULL;
	addr_t offset;

	/*
	 * Stupid gdb peeks/pokes the access registers in 64 bit with
	 * an alignment of 4. Programmers from hell indeed...
	 */
	if ((addr & 3) || addr > sizeof(struct user) - __ADDR_MASK)
		return -EIO;

	if (addr < (addr_t) &dummy->regs.acrs) {
		/*
		 * psw and gprs are stored on the stack
		 */
		if (addr == (addr_t) &dummy->regs.psw.mask &&
#ifdef CONFIG_S390_SUPPORT
		    data != PSW_MASK_MERGE(PSW_USER32_BITS, data) &&
#endif
		    data != PSW_MASK_MERGE(PSW_USER_BITS, data))
			/* Invalid psw mask. */
			return -EINVAL;
#ifndef CONFIG_ARCH_S390X
		if (addr == (addr_t) &dummy->regs.psw.addr)
			/* I'd like to reject addresses without the
			   high order bit but older gdb's rely on it */
			data |= PSW_ADDR_AMODE;
#endif
		*(addr_t *)((addr_t) &__KSTK_PTREGS(child)->psw + addr) = data;

	} else if (addr < (addr_t) (&dummy->regs.orig_gpr2)) {
		/*
		 * access registers are stored in the thread structure
		 */
		offset = addr - (addr_t) &dummy->regs.acrs;
		*(addr_t *)((addr_t) &child->thread.acrs + offset) = data;

	} else if (addr == (addr_t) &dummy->regs.orig_gpr2) {
		/*
		 * orig_gpr2 is stored on the kernel stack
		 */
		__KSTK_PTREGS(child)->orig_gpr2 = data;

	} else if (addr < (addr_t) (&dummy->regs.fp_regs + 1)) {
		/*
		 * floating point regs. are stored in the thread structure
		 */
		if (addr == (addr_t) &dummy->regs.fp_regs.fpc &&
		    (data & ~FPC_VALID_MASK) != 0)
			return -EINVAL;
		offset = addr - (addr_t) &dummy->regs.fp_regs;
		*(addr_t *)((addr_t) &child->thread.fp_regs + offset) = data;

	} else if (addr < (addr_t) (&dummy->regs.per_info + 1)) {
		/*
		 * per_info is found in the thread structure 
		 */
		offset = addr - (addr_t) &dummy->regs.per_info;
		*(addr_t *)((addr_t) &child->thread.per_info + offset) = data;

	}

	FixPerRegisters(child);
	return 0;
}

static int
do_ptrace_normal(struct task_struct *child, long request, long addr, long data)
{
	unsigned long tmp;
	ptrace_area parea; 
	int copied, ret;

	switch (request) {
	case PTRACE_PEEKTEXT:
	case PTRACE_PEEKDATA:
		/* Remove high order bit from address (only for 31 bit). */
		addr &= PSW_ADDR_INSN;
		/* read word at location addr. */
		copied = access_process_vm(child, addr, &tmp, sizeof(tmp), 0);
		if (copied != sizeof(tmp))
			return -EIO;
		return put_user(tmp, (unsigned long __user *) data);

	case PTRACE_PEEKUSR:
		/* read the word at location addr in the USER area. */
		return peek_user(child, addr, data);

	case PTRACE_POKETEXT:
	case PTRACE_POKEDATA:
		/* Remove high order bit from address (only for 31 bit). */
		addr &= PSW_ADDR_INSN;
		/* write the word at location addr. */
		copied = access_process_vm(child, addr, &data, sizeof(data),1);
		if (copied != sizeof(data))
			return -EIO;
		return 0;

	case PTRACE_POKEUSR:
		/* write the word at location addr in the USER area */
		return poke_user(child, addr, data);

	case PTRACE_PEEKUSR_AREA:
	case PTRACE_POKEUSR_AREA:
		if (copy_from_user(&parea, (void __user *) addr,
							sizeof(parea)))
			return -EFAULT;
		addr = parea.kernel_addr;
		data = parea.process_addr;
		copied = 0;
		while (copied < parea.len) {
			if (request == PTRACE_PEEKUSR_AREA)
				ret = peek_user(child, addr, data);
			else {
				addr_t tmp;
				if (get_user (tmp, (addr_t __user *) data))
					return -EFAULT;
				ret = poke_user(child, addr, tmp);
			}
			if (ret)
				return ret;
			addr += sizeof(unsigned long);
			data += sizeof(unsigned long);
			copied += sizeof(unsigned long);
		}
		return 0;
	}
	return ptrace_request(child, request, addr, data);
}

#ifdef CONFIG_S390_SUPPORT
/*
 * Now the fun part starts... a 31 bit program running in the
 * 31 bit emulation tracing another program. PTRACE_PEEKTEXT,
 * PTRACE_PEEKDATA, PTRACE_POKETEXT and PTRACE_POKEDATA are easy
 * to handle, the difference to the 64 bit versions of the requests
 * is that the access is done in multiples of 4 byte instead of
 * 8 bytes (sizeof(unsigned long) on 31/64 bit).
 * The ugly part are PTRACE_PEEKUSR, PTRACE_PEEKUSR_AREA,
 * PTRACE_POKEUSR and PTRACE_POKEUSR_AREA. If the traced program
 * is a 31 bit program too, the content of struct user can be
 * emulated. A 31 bit program peeking into the struct user of
 * a 64 bit program is a no-no.
 */

/*
 * Same as peek_user but for a 31 bit program.
 */
static int
peek_user_emu31(struct task_struct *child, addr_t addr, addr_t data)
{
	struct user32 *dummy32 = NULL;
	per_struct32 *dummy_per32 = NULL;
	addr_t offset;
	__u32 tmp;

	if (!test_thread_flag(TIF_31BIT) ||
	    (addr & 3) || addr > sizeof(struct user) - 3)
		return -EIO;

	if (addr < (addr_t) &dummy32->regs.acrs) {
		/*
		 * psw and gprs are stored on the stack
		 */
		if (addr == (addr_t) &dummy32->regs.psw.mask) {
			/* Fake a 31 bit psw mask. */
			tmp = (__u32)(__KSTK_PTREGS(child)->psw.mask >> 32);
			tmp = PSW32_MASK_MERGE(PSW32_USER_BITS, tmp);
		} else if (addr == (addr_t) &dummy32->regs.psw.addr) {
			/* Fake a 31 bit psw address. */
			tmp = (__u32) __KSTK_PTREGS(child)->psw.addr |
				PSW32_ADDR_AMODE31;
		} else {
			/* gpr 0-15 */
			tmp = *(__u32 *)((addr_t) &__KSTK_PTREGS(child)->psw +
					 addr*2 + 4);
		}
	} else if (addr < (addr_t) (&dummy32->regs.orig_gpr2)) {
		/*
		 * access registers are stored in the thread structure
		 */
		offset = addr - (addr_t) &dummy32->regs.acrs;
		tmp = *(__u32*)((addr_t) &child->thread.acrs + offset);

	} else if (addr == (addr_t) (&dummy32->regs.orig_gpr2)) {
		/*
		 * orig_gpr2 is stored on the kernel stack
		 */
		tmp = *(__u32*)((addr_t) &__KSTK_PTREGS(child)->orig_gpr2 + 4);

	} else if (addr < (addr_t) (&dummy32->regs.fp_regs + 1)) {
		/*
		 * floating point regs. are stored in the thread structure 
		 */
	        offset = addr - (addr_t) &dummy32->regs.fp_regs;
		tmp = *(__u32 *)((addr_t) &child->thread.fp_regs + offset);

	} else if (addr < (addr_t) (&dummy32->regs.per_info + 1)) {
		/*
		 * per_info is found in the thread structure
		 */
		offset = addr - (addr_t) &dummy32->regs.per_info;
		/* This is magic. See per_struct and per_struct32. */
		if ((offset >= (addr_t) &dummy_per32->control_regs &&
		     offset < (addr_t) (&dummy_per32->control_regs + 1)) ||
		    (offset >= (addr_t) &dummy_per32->starting_addr &&
		     offset <= (addr_t) &dummy_per32->ending_addr) ||
		    offset == (addr_t) &dummy_per32->lowcore.words.address)
			offset = offset*2 + 4;
		else
			offset = offset*2;
		tmp = *(__u32 *)((addr_t) &child->thread.per_info + offset);

	} else
		tmp = 0;

	return put_user(tmp, (__u32 __user *) data);
}

/*
 * Same as poke_user but for a 31 bit program.
 */
static int
poke_user_emu31(struct task_struct *child, addr_t addr, addr_t data)
{
	struct user32 *dummy32 = NULL;
	per_struct32 *dummy_per32 = NULL;
	addr_t offset;
	__u32 tmp;

	if (!test_thread_flag(TIF_31BIT) ||
	    (addr & 3) || addr > sizeof(struct user32) - 3)
		return -EIO;

	tmp = (__u32) data;

	if (addr < (addr_t) &dummy32->regs.acrs) {
		/*
		 * psw, gprs, acrs and orig_gpr2 are stored on the stack
		 */
		if (addr == (addr_t) &dummy32->regs.psw.mask) {
			/* Build a 64 bit psw mask from 31 bit mask. */
			if (tmp != PSW32_MASK_MERGE(PSW32_USER_BITS, tmp))
				/* Invalid psw mask. */
				return -EINVAL;
			__KSTK_PTREGS(child)->psw.mask =
				PSW_MASK_MERGE(PSW_USER32_BITS, (__u64) tmp << 32);
		} else if (addr == (addr_t) &dummy32->regs.psw.addr) {
			/* Build a 64 bit psw address from 31 bit address. */
			__KSTK_PTREGS(child)->psw.addr = 
				(__u64) tmp & PSW32_ADDR_INSN;
		} else {
			/* gpr 0-15 */
			*(__u32*)((addr_t) &__KSTK_PTREGS(child)->psw
				  + addr*2 + 4) = tmp;
		}
	} else if (addr < (addr_t) (&dummy32->regs.orig_gpr2)) {
		/*
		 * access registers are stored in the thread structure
		 */
		offset = addr - (addr_t) &dummy32->regs.acrs;
		*(__u32*)((addr_t) &child->thread.acrs + offset) = tmp;

	} else if (addr == (addr_t) (&dummy32->regs.orig_gpr2)) {
		/*
		 * orig_gpr2 is stored on the kernel stack
		 */
		*(__u32*)((addr_t) &__KSTK_PTREGS(child)->orig_gpr2 + 4) = tmp;

	} else if (addr < (addr_t) (&dummy32->regs.fp_regs + 1)) {
		/*
		 * floating point regs. are stored in the thread structure 
		 */
		if (addr == (addr_t) &dummy32->regs.fp_regs.fpc &&
		    (tmp & ~FPC_VALID_MASK) != 0)
			/* Invalid floating point control. */
			return -EINVAL;
	        offset = addr - (addr_t) &dummy32->regs.fp_regs;
		*(__u32 *)((addr_t) &child->thread.fp_regs + offset) = tmp;

	} else if (addr < (addr_t) (&dummy32->regs.per_info + 1)) {
		/*
		 * per_info is found in the thread structure.
		 */
		offset = addr - (addr_t) &dummy32->regs.per_info;
		/*
		 * This is magic. See per_struct and per_struct32.
		 * By incident the offsets in per_struct are exactly
		 * twice the offsets in per_struct32 for all fields.
		 * The 8 byte fields need special handling though,
		 * because the second half (bytes 4-7) is needed and
		 * not the first half.
		 */
		if ((offset >= (addr_t) &dummy_per32->control_regs &&
		     offset < (addr_t) (&dummy_per32->control_regs + 1)) ||
		    (offset >= (addr_t) &dummy_per32->starting_addr &&
		     offset <= (addr_t) &dummy_per32->ending_addr) ||
		    offset == (addr_t) &dummy_per32->lowcore.words.address)
			offset = offset*2 + 4;
		else
			offset = offset*2;
		*(__u32 *)((addr_t) &child->thread.per_info + offset) = tmp;

	}

	FixPerRegisters(child);
	return 0;
}

static int
do_ptrace_emu31(struct task_struct *child, long request, long addr, long data)
{
	unsigned int tmp;  /* 4 bytes !! */
	ptrace_area_emu31 parea; 
	int copied, ret;

	switch (request) {
	case PTRACE_PEEKTEXT:
	case PTRACE_PEEKDATA:
		/* read word at location addr. */
		copied = access_process_vm(child, addr, &tmp, sizeof(tmp), 0);
		if (copied != sizeof(tmp))
			return -EIO;
		return put_user(tmp, (unsigned int __user *) data);

	case PTRACE_PEEKUSR:
		/* read the word at location addr in the USER area. */
		return peek_user_emu31(child, addr, data);

	case PTRACE_POKETEXT:
	case PTRACE_POKEDATA:
		/* write the word at location addr. */
		tmp = data;
		copied = access_process_vm(child, addr, &tmp, sizeof(tmp), 1);
		if (copied != sizeof(tmp))
			return -EIO;
		return 0;

	case PTRACE_POKEUSR:
		/* write the word at location addr in the USER area */
		return poke_user_emu31(child, addr, data);

	case PTRACE_PEEKUSR_AREA:
	case PTRACE_POKEUSR_AREA:
		if (copy_from_user(&parea, (void __user *) addr,
							sizeof(parea)))
			return -EFAULT;
		addr = parea.kernel_addr;
		data = parea.process_addr;
		copied = 0;
		while (copied < parea.len) {
			if (request == PTRACE_PEEKUSR_AREA)
				ret = peek_user_emu31(child, addr, data);
			else {
				__u32 tmp;
				if (get_user (tmp, (__u32 __user *) data))
					return -EFAULT;
				ret = poke_user_emu31(child, addr, tmp);
			}
			if (ret)
				return ret;
			addr += sizeof(unsigned int);
			data += sizeof(unsigned int);
			copied += sizeof(unsigned int);
		}
		return 0;
	case PTRACE_GETEVENTMSG:
		return put_user((__u32) child->ptrace_message,
				(unsigned int __user *) data);
	case PTRACE_GETSIGINFO:
		if (child->last_siginfo == NULL)
			return -EINVAL;
		return copy_siginfo_to_user32((siginfo_t32 __user *) data,
					      child->last_siginfo);
	case PTRACE_SETSIGINFO:
		if (child->last_siginfo == NULL)
			return -EINVAL;
		return copy_siginfo_from_user32(child->last_siginfo,
						(siginfo_t32 __user *) data);
	}
	return ptrace_request(child, request, addr, data);
}
#endif

#define PT32_IEEE_IP 0x13c

static int
do_ptrace(struct task_struct *child, long request, long addr, long data)
{
	int ret;

	if (request == PTRACE_ATTACH)
		return ptrace_attach(child);

	/*
	 * Special cases to get/store the ieee instructions pointer.
	 */
	if (child == current) {
		if (request == PTRACE_PEEKUSR && addr == PT_IEEE_IP)
			return peek_user(child, addr, data);
		if (request == PTRACE_POKEUSR && addr == PT_IEEE_IP)
			return poke_user(child, addr, data);
#ifdef CONFIG_S390_SUPPORT
		if (request == PTRACE_PEEKUSR &&
		    addr == PT32_IEEE_IP && test_thread_flag(TIF_31BIT))
			return peek_user_emu31(child, addr, data);
		if (request == PTRACE_POKEUSR &&
		    addr == PT32_IEEE_IP && test_thread_flag(TIF_31BIT))
			return poke_user_emu31(child, addr, data);
#endif
	}

	ret = ptrace_check_attach(child, request == PTRACE_KILL);
	if (ret < 0)
		return ret;

	switch (request) {
	case PTRACE_SYSCALL:
		/* continue and stop at next (return from) syscall */
	case PTRACE_CONT:
		/* restart after signal. */
		if ((unsigned long) data >= _NSIG)
			return -EIO;
		if (request == PTRACE_SYSCALL)
			set_tsk_thread_flag(child, TIF_SYSCALL_TRACE);
		else
			clear_tsk_thread_flag(child, TIF_SYSCALL_TRACE);
		child->exit_code = data;
		/* make sure the single step bit is not set. */
		clear_single_step(child);
		wake_up_process(child);
		return 0;

	case PTRACE_KILL:
		/*
		 * make the child exit.  Best I can do is send it a sigkill. 
		 * perhaps it should be put in the status that it wants to 
		 * exit.
		 */
		if (child->state == TASK_ZOMBIE) /* already dead */
			return 0;
		child->exit_code = SIGKILL;
		/* make sure the single step bit is not set. */
		clear_single_step(child);
		wake_up_process(child);
		return 0;

	case PTRACE_SINGLESTEP:
		/* set the trap flag. */
		if ((unsigned long) data >= _NSIG)
			return -EIO;
		clear_tsk_thread_flag(child, TIF_SYSCALL_TRACE);
		child->exit_code = data;
		set_single_step(child);
		/* give it a chance to run. */
		wake_up_process(child);
		return 0;

	case PTRACE_DETACH:
		/* detach a process that was attached. */
		return ptrace_detach(child, data);


	/* Do requests that differ for 31/64 bit */
	default:
#ifdef CONFIG_S390_SUPPORT
		if (test_thread_flag(TIF_31BIT))
			return do_ptrace_emu31(child, request, addr, data);
#endif
		return do_ptrace_normal(child, request, addr, data);
	}
	/* Not reached.  */
	return -EIO;
}

asmlinkage long
sys_ptrace(long request, long pid, long addr, long data)
{
	struct task_struct *child;
	int ret;

	lock_kernel();

	if (request == PTRACE_TRACEME) {
		/* are we already being traced? */
		ret = -EPERM;
		if (current->ptrace & PT_PTRACED)
			goto out;
		ret = security_ptrace(current->parent, current);
		if (ret)
			goto out;
		/* set the ptrace bit in the process flags. */
		current->ptrace |= PT_PTRACED;
		goto out;
	}

	ret = -EPERM;
	if (pid == 1)		/* you may not mess with init */
		goto out;

	ret = -ESRCH;
	read_lock(&tasklist_lock);
	child = find_task_by_pid(pid);
	if (child)
		get_task_struct(child);
	read_unlock(&tasklist_lock);
	if (!child)
		goto out;

	ret = do_ptrace(child, request, addr, data);

	put_task_struct(child);
out:
	unlock_kernel();
	return ret;
}

asmlinkage void
syscall_trace(struct pt_regs *regs, int entryexit)
{
	if (unlikely(current->audit_context)) {
		if (!entryexit)
			audit_syscall_entry(current, regs->gprs[2],
					    regs->orig_gpr2, regs->gprs[3],
					    regs->gprs[4], regs->gprs[5]);
		else
			audit_syscall_exit(current, regs->gprs[2]);
	}
	if (!test_thread_flag(TIF_SYSCALL_TRACE))
		return;
	if (!(current->ptrace & PT_PTRACED))
		return;
	ptrace_notify(SIGTRAP | ((current->ptrace & PT_TRACESYSGOOD)
				 ? 0x80 : 0));

	/*
	 * this isn't the same as continuing with a signal, but it will do
	 * for normal use.  strace only continues with a signal if the
	 * stopping signal is not SIGTRAP.  -brl
	 */
	if (current->exit_code) {
		send_sig(current->exit_code, current, 1);
		current->exit_code = 0;
	}
}
