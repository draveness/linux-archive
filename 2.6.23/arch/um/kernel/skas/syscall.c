/*
 * Copyright (C) 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include "linux/sys.h"
#include "linux/ptrace.h"
#include "asm/errno.h"
#include "asm/unistd.h"
#include "asm/ptrace.h"
#include "asm/current.h"
#include "sysdep/syscalls.h"
#include "kern_util.h"
#include "syscall.h"

void handle_syscall(union uml_pt_regs *r)
{
	struct pt_regs *regs = container_of(r, struct pt_regs, regs);
	long result;
	int syscall;

	syscall_trace(r, 0);

	current->thread.nsyscalls++;
	nsyscalls++;

	/* This should go in the declaration of syscall, but when I do that,
	 * strace -f -c bash -c 'ls ; ls' breaks, sometimes not tracing
	 * children at all, sometimes hanging when bash doesn't see the first
	 * ls exit.
	 * The assembly looks functionally the same to me.  This is
	 *     gcc version 4.0.1 20050727 (Red Hat 4.0.1-5)
	 * in case it's a compiler bug.
	 */
	syscall = UPT_SYSCALL_NR(r);
	if((syscall >= NR_syscalls) || (syscall < 0))
		result = -ENOSYS;
	else result = EXECUTE_SYSCALL(syscall, regs);

	REGS_SET_SYSCALL_RETURN(r->skas.regs, result);

	syscall_trace(r, 1);
}
