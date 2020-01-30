/* 
 * Copyright (C) 2000, 2001, 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#ifndef __USER_UTIL_H__
#define __USER_UTIL_H__

#include "sysdep/ptrace.h"

extern int mode_tt;

extern int grantpt(int __fd);
extern int unlockpt(int __fd);
extern char *ptsname(int __fd);

enum { OP_NONE, OP_EXEC, OP_FORK, OP_TRACE_ON, OP_REBOOT, OP_HALT, OP_CB };

struct cpu_task {
	int pid;
	void *task;
};

extern struct cpu_task cpu_tasks[];

struct signal_info {
	void (*handler)(int, union uml_pt_regs *);
	int is_irq;
};

extern struct signal_info sig_info[];

extern unsigned long low_physmem;
extern unsigned long high_physmem;
extern unsigned long uml_physmem;
extern unsigned long uml_reserved;
extern unsigned long end_vm;
extern unsigned long start_vm;
extern unsigned long highmem;

extern char host_info[];

extern char saved_command_line[];
extern char command_line[];

extern char *tempdir;

extern unsigned long _stext, _etext, _sdata, _edata, __bss_start, _end;
extern unsigned long _unprotected_end;
extern unsigned long brk_start;

extern int pty_output_sigio;
extern int pty_close_sigio;

extern void stop(void);
extern void stack_protections(unsigned long address);
extern void task_protections(unsigned long address);
extern int wait_for_stop(int pid, int sig, int cont_type, void *relay);
extern void *add_signal_handler(int sig, void (*handler)(int));
extern int start_fork_tramp(void *arg, unsigned long temp_stack, 
			    int clone_flags, int (*tramp)(void *));
extern int clone_and_wait(int (*fn)(void *), void *arg, void *sp, int flags);
extern int linux_main(int argc, char **argv);
extern void set_cmdline(char *cmd);
extern void input_cb(void (*proc)(void *), void *arg, int arg_len);
extern int get_pty(void);
extern void *um_kmalloc(int size);
extern int raw(int fd, int complain);
extern int switcheroo(int fd, int prot, void *from, void *to, int size);
extern void setup_machinename(char *machine_out);
extern void setup_hostinfo(void);
extern void add_arg(char *cmd_line, char *arg);
extern void init_new_thread_stack(void *sig_stack, void (*usr1_handler)(int));
extern void init_new_thread_signals(int altstack);
extern void do_exec(int old_pid, int new_pid);
extern void tracer_panic(char *msg, ...);
extern char *get_umid(int only_if_set);
extern void do_longjmp(void *p, int val);
extern void suspend_new_thread(int fd);
extern int detach(int pid, int sig);
extern int attach(int pid);
extern void kill_child_dead(int pid);
extern int cont(int pid);
extern void check_ptrace(void);
extern void check_sigio(void);
extern int run_kernel_thread(int (*fn)(void *), void *arg, void **jmp_ptr);
extern void write_sigio_workaround(void);
extern void arch_check_bugs(void);
extern int arch_handle_signal(int sig, union uml_pt_regs *regs);
extern int arch_fixup(unsigned long address, void *sc_ptr);
extern void forward_pending_sigio(int target);
extern int can_do_skas(void);
 
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
