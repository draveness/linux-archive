/* 
 * Copyright (C) 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include "linux/kernel.h"
#include "linux/mm.h"
#include "asm/signal.h"
#include "asm/ptrace.h"
#include "asm/uaccess.h"
#include "asm/pgalloc.h"
#include "asm/tlbflush.h"
#include "user_util.h"
#include "kern_util.h"
#include "irq_user.h"
#include "time_user.h"
#include "mem_user.h"
#include "os.h"
#include "tlb.h"

static int exec_tramp(void *sig_stack)
{
	init_new_thread_stack(sig_stack, NULL);
	init_new_thread_signals(1);
	os_stop_process(os_getpid());
	return(0);
}

void flush_thread_tt(void)
{
	unsigned long stack;
	int new_pid;

	stack = alloc_stack(0, 0);
	if(stack == 0){
		printk(KERN_ERR 
		       "flush_thread : failed to allocate temporary stack\n");
		do_exit(SIGKILL);
	}
		
	new_pid = start_fork_tramp((void *) current->thread.kernel_stack,
				   stack, 0, exec_tramp);
	if(new_pid < 0){
		printk(KERN_ERR 
		       "flush_thread : new thread failed, errno = %d\n",
		       -new_pid);
		do_exit(SIGKILL);
	}

	if(current->thread_info->cpu == 0)
		forward_interrupts(new_pid);
	current->thread.request.op = OP_EXEC;
	current->thread.request.u.exec.pid = new_pid;
	unprotect_stack((unsigned long) current->thread_info);
	os_usr1_process(os_getpid());

	enable_timer();
	free_page(stack);
	protect_memory(uml_reserved, high_physmem - uml_reserved, 1, 1, 0, 1);
	task_protections((unsigned long) current->thread_info);
	force_flush_all();
	unblock_signals();
}

void start_thread_tt(struct pt_regs *regs, unsigned long eip, 
		     unsigned long esp)
{
	set_fs(USER_DS);
	flush_tlb_mm(current->mm);
	PT_REGS_IP(regs) = eip;
	PT_REGS_SP(regs) = esp;
	PT_FIX_EXEC_STACK(esp);
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
