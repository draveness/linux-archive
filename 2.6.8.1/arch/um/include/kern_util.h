/* 
 * Copyright (C) 2000, 2001, 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#ifndef __KERN_UTIL_H__
#define __KERN_UTIL_H__

#include "linux/threads.h"
#include "sysdep/ptrace.h"

extern int ncpus;
extern char *linux_prog;
extern char *gdb_init;
extern int kmalloc_ok;
extern int timer_irq_inited;
extern int jail;
extern int nsyscalls;

extern struct task_struct *idle_threads[NR_CPUS];

#define UML_ROUND_DOWN(addr) ((void *)(((unsigned long) addr) & PAGE_MASK))
#define UML_ROUND_UP(addr) \
	UML_ROUND_DOWN(((unsigned long) addr) + PAGE_SIZE - 1)

extern int kernel_fork(unsigned long flags, int (*fn)(void *), void * arg);
extern unsigned long stack_sp(unsigned long page);
extern int kernel_thread_proc(void *data);
extern void syscall_segv(int sig);
extern int current_pid(void);
extern unsigned long alloc_stack(int order, int atomic);
extern int do_signal(int error);
extern int is_stack_fault(unsigned long sp);
extern unsigned long segv(unsigned long address, unsigned long ip, 
			  int is_write, int is_user, void *sc);
extern int handle_page_fault(unsigned long address, unsigned long ip,
			     int is_write, int is_user, int *code_out);
extern void syscall_ready(void);
extern void set_tracing(void *t, int tracing);
extern int is_tracing(void *task);
extern int segv_syscall(void);
extern void kern_finish_exec(void *task, int new_pid, unsigned long stack);
extern int page_size(void);
extern int page_mask(void);
extern int need_finish_fork(void);
extern void free_stack(unsigned long stack, int order);
extern void add_input_request(int op, void (*proc)(int), void *arg);
extern char *current_cmd(void);
extern void timer_handler(int sig, union uml_pt_regs *regs);
extern int set_signals(int enable);
extern void force_sigbus(void);
extern int pid_to_processor_id(int pid);
extern void block_signals(void);
extern void unblock_signals(void);
extern void deliver_signals(void *t);
extern int next_syscall_index(int max);
extern int next_trap_index(int max);
extern void default_idle(void);
extern void finish_fork(void);
extern void paging_init(void);
extern void init_flush_vm(void);
extern void *syscall_sp(void *t);
extern void syscall_trace(void);
extern int hz(void);
extern void idle_timer(void);
extern unsigned int do_IRQ(int irq, union uml_pt_regs *regs);
extern int external_pid(void *t);
extern int pid_to_processor_id(int pid);
extern void boot_timer_handler(int sig);
extern void interrupt_end(void);
extern void initial_thread_cb(void (*proc)(void *), void *arg);
extern int debugger_signal(int status, int pid);
extern void debugger_parent_signal(int status, int pid);
extern void child_signal(int pid, int status);
extern int init_ptrace_proxy(int idle_pid, int startup, int stop);
extern int init_parent_proxy(int pid);
extern int singlestepping(void *t);
extern void check_stack_overflow(void *ptr);
extern void relay_signal(int sig, union uml_pt_regs *regs);
extern void not_implemented(void);
extern int user_context(unsigned long sp);
extern void timer_irq(union uml_pt_regs *regs);
extern void unprotect_stack(unsigned long stack);
extern void do_uml_exitcalls(void);
extern int attach_debugger(int idle_pid, int pid, int stop);
extern void bad_segv(unsigned long address, unsigned long ip, int is_write);
extern int config_gdb(char *str);
extern int remove_gdb(void);
extern char *uml_strdup(char *string);
extern void unprotect_kernel_mem(void);
extern void protect_kernel_mem(void);
extern void set_kmem_end(unsigned long);
extern void uml_cleanup(void);
extern int pid_to_processor_id(int pid);
extern void set_current(void *t);
extern void lock_signalled_task(void *t);
extern void IPI_handler(int cpu);
extern int jail_setup(char *line, int *add);
extern void *get_init_task(void);
extern int clear_user_proc(void *buf, int size);
extern int copy_to_user_proc(void *to, void *from, int size);
extern int copy_from_user_proc(void *to, void *from, int size);
extern void bus_handler(int sig, union uml_pt_regs *regs);
extern long execute_syscall(void *r);
extern int smp_sigio_handler(void);
extern void *get_current(void);
extern struct task_struct *get_task(int pid, int require);
extern void machine_halt(void);
extern int is_syscall(unsigned long addr);
extern void arch_switch(void);
extern void free_irq(unsigned int, void *);
extern int um_in_interrupt(void);
extern int cpu(void);
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
