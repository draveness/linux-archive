/* 
 * Copyright (C) 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <linux/stddef.h>
#include <sys/ptrace.h>
#include <asm/ptrace.h>
#include <asm/user.h>
#include "kern_util.h"
#include "sysdep/thread.h"
#include "user.h"
#include "os.h"

int ptrace_getregs(long pid, unsigned long *regs_out)
{
	return(ptrace(PTRACE_GETREGS, pid, 0, regs_out));
}

int ptrace_setregs(long pid, unsigned long *regs)
{
	return(ptrace(PTRACE_SETREGS, pid, 0, regs));
}

int ptrace_getfpregs(long pid, unsigned long *regs)
{
	return(ptrace(PTRACE_GETFPREGS, pid, 0, regs));
}

static void write_debugregs(int pid, unsigned long *regs)
{
	struct user *dummy;
	int nregs, i;

	dummy = NULL;
	nregs = sizeof(dummy->u_debugreg)/sizeof(dummy->u_debugreg[0]);
	for(i = 0; i < nregs; i++){
		if((i == 4) || (i == 5)) continue;
		if(ptrace(PTRACE_POKEUSR, pid, &dummy->u_debugreg[i],
			  regs[i]) < 0)
			printk("write_debugregs - ptrace failed, "
			       "errno = %d\n", errno);
	}
}

static void read_debugregs(int pid, unsigned long *regs)
{
	struct user *dummy;
	int nregs, i;

	dummy = NULL;
	nregs = sizeof(dummy->u_debugreg)/sizeof(dummy->u_debugreg[0]);
	for(i = 0; i < nregs; i++){
		regs[i] = ptrace(PTRACE_PEEKUSR, pid, 
				 &dummy->u_debugreg[i], 0);
	}
}

/* Accessed only by the tracing thread */
static unsigned long kernel_debugregs[8] = { [ 0 ... 7 ] = 0 };
static int debugregs_seq = 0;

void arch_enter_kernel(void *task, int pid)
{
	read_debugregs(pid, TASK_DEBUGREGS(task));
	write_debugregs(pid, kernel_debugregs);
}

void arch_leave_kernel(void *task, int pid)
{
	read_debugregs(pid, kernel_debugregs);
	write_debugregs(pid, TASK_DEBUGREGS(task));
}

void ptrace_pokeuser(unsigned long addr, unsigned long data)
{
	if((addr < offsetof(struct user, u_debugreg[0])) ||
	   (addr > offsetof(struct user, u_debugreg[7])))
		return;
	addr -= offsetof(struct user, u_debugreg[0]);
	addr = addr >> 2;
	if(kernel_debugregs[addr] == data) return;

	kernel_debugregs[addr] = data;
	debugregs_seq++;
}

static void update_debugregs_cb(void *arg)
{
	int pid = *((int *) arg);

	write_debugregs(pid, kernel_debugregs);
}

void update_debugregs(int seq)
{
	int me;

	if(seq == debugregs_seq) return;

	me = os_getpid();
	initial_thread_cb(update_debugregs_cb, &me);
}

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
