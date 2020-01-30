/* 
 * Copyright (C) 2000, 2001, 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <asm/page.h>
#include <asm/unistd.h>
#include <asm/ptrace.h>
#include "init.h"
#include "sysdep/ptrace.h"
#include "sigcontext.h"
#include "sysdep/sigcontext.h"
#include "irq_user.h"
#include "frame_user.h"
#include "signal_user.h"
#include "time_user.h"
#include "task.h"
#include "mode.h"
#include "choose-mode.h"
#include "kern_util.h"
#include "user_util.h"
#include "os.h"

void kill_child_dead(int pid)
{
	kill(pid, SIGKILL);
	kill(pid, SIGCONT);
	while(waitpid(pid, NULL, 0) > 0) kill(pid, SIGCONT);
}

/* Unlocked - don't care if this is a bit off */
int nsegfaults = 0;

struct {
	unsigned long address;
	int is_write;
	int pid;
	unsigned long sp;
	int is_user;
} segfault_record[1024];

void segv_handler(int sig, union uml_pt_regs *regs)
{
	int index, max;

	if(UPT_IS_USER(regs) && !UPT_SEGV_IS_FIXABLE(regs)){
		bad_segv(UPT_FAULT_ADDR(regs), UPT_IP(regs), 
			 UPT_FAULT_WRITE(regs));
		return;
	}
	max = sizeof(segfault_record)/sizeof(segfault_record[0]);
	index = next_trap_index(max);

	nsegfaults++;
	segfault_record[index].address = UPT_FAULT_ADDR(regs);
	segfault_record[index].pid = os_getpid();
	segfault_record[index].is_write = UPT_FAULT_WRITE(regs);
	segfault_record[index].sp = UPT_SP(regs);
	segfault_record[index].is_user = UPT_IS_USER(regs);
	segv(UPT_FAULT_ADDR(regs), UPT_IP(regs), UPT_FAULT_WRITE(regs),
	     UPT_IS_USER(regs), regs);
}

void usr2_handler(int sig, union uml_pt_regs *regs)
{
	CHOOSE_MODE(syscall_handler_tt(sig, regs), (void) 0);
}

struct signal_info sig_info[] = {
	[ SIGTRAP ] { .handler 		= relay_signal,
		      .is_irq 		= 0 },
	[ SIGFPE ] { .handler 		= relay_signal,
		     .is_irq 		= 0 },
	[ SIGILL ] { .handler 		= relay_signal,
		     .is_irq 		= 0 },
	[ SIGBUS ] { .handler 		= bus_handler,
		     .is_irq 		= 0 },
	[ SIGSEGV] { .handler 		= segv_handler,
		     .is_irq 		= 0 },
	[ SIGIO ] { .handler 		= sigio_handler,
		    .is_irq 		= 1 },
	[ SIGVTALRM ] { .handler 	= timer_handler,
			.is_irq 	= 1 },
        [ SIGALRM ] { .handler          = timer_handler,
                      .is_irq           = 1 },
	[ SIGUSR2 ] { .handler 		= usr2_handler,
		      .is_irq 		= 0 },
};

void sig_handler(int sig, struct sigcontext sc)
{
	CHOOSE_MODE_PROC(sig_handler_common_tt, sig_handler_common_skas,
			 sig, &sc);
}

extern int timer_irq_inited, missed_ticks[];

void alarm_handler(int sig, struct sigcontext sc)
{
	if(!timer_irq_inited) return;
	missed_ticks[cpu()]++;

	if(sig == SIGALRM)
		switch_timers(0);

	CHOOSE_MODE_PROC(sig_handler_common_tt, sig_handler_common_skas,
			 sig, &sc);

	if(sig == SIGALRM)
		switch_timers(1);
}

void do_longjmp(void *b, int val)
{
	jmp_buf *buf = b;

	longjmp(*buf, val);
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
