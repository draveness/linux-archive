/* 
 * Copyright (C) 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#ifndef __MODE_TT_H__
#define __MODE_TT_H__

#include "sysdep/ptrace.h"

extern int tracing_pid;

extern int tracer(int (*init_proc)(void *), void *sp);
extern void user_time_init_tt(void);
extern int copy_sc_from_user_tt(void *to_ptr, void *from_ptr, void *data);
extern int copy_sc_to_user_tt(void *to_ptr, void *fp, void *from_ptr, 
			      void *data);
extern void sig_handler_common_tt(int sig, void *sc);
extern void syscall_handler_tt(int sig, union uml_pt_regs *regs);
extern void reboot_tt(void);
extern void halt_tt(void);
extern int is_tracer_winch(int pid, int fd, void *data);
extern void kill_off_processes_tt(void);

#endif

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
