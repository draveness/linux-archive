/*
 *  linux/arch/i386/traps.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Pentium III FXSR, SSE support
 *	Gareth Hughes <gareth@valinux.com>, May 2000
 */

/*
 * 'Traps.c' handles hardware traps and faults after we have saved some
 * state in 'asm.s'.
 */
#include <linux/config.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/highmem.h>
#include <linux/kallsyms.h>
#include <linux/ptrace.h>
#include <linux/version.h>

#ifdef CONFIG_EISA
#include <linux/ioport.h>
#include <linux/eisa.h>
#endif

#ifdef CONFIG_MCA
#include <linux/mca.h>
#endif

#include <asm/processor.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/atomic.h>
#include <asm/debugreg.h>
#include <asm/desc.h>
#include <asm/i387.h>
#include <asm/nmi.h>

#include <asm/smp.h>
#include <asm/arch_hooks.h>

#include <linux/irq.h>
#include <linux/module.h>

#include "mach_traps.h"

asmlinkage int system_call(void);
asmlinkage void lcall7(void);
asmlinkage void lcall27(void);

struct desc_struct default_ldt[] = { { 0, 0 }, { 0, 0 }, { 0, 0 },
		{ 0, 0 }, { 0, 0 } };

/* Do we ignore FPU interrupts ? */
char ignore_fpu_irq = 0;

/*
 * The IDT has to be page-aligned to simplify the Pentium
 * F0 0F bug workaround.. We have a special link segment
 * for this.
 */
struct desc_struct idt_table[256] __attribute__((__section__(".data.idt"))) = { {0, 0}, };

asmlinkage void divide_error(void);
asmlinkage void debug(void);
asmlinkage void nmi(void);
asmlinkage void int3(void);
asmlinkage void overflow(void);
asmlinkage void bounds(void);
asmlinkage void invalid_op(void);
asmlinkage void device_not_available(void);
asmlinkage void coprocessor_segment_overrun(void);
asmlinkage void invalid_TSS(void);
asmlinkage void segment_not_present(void);
asmlinkage void stack_segment(void);
asmlinkage void general_protection(void);
asmlinkage void page_fault(void);
asmlinkage void coprocessor_error(void);
asmlinkage void simd_coprocessor_error(void);
asmlinkage void alignment_check(void);
asmlinkage void spurious_interrupt_bug(void);
asmlinkage void machine_check(void);

static int kstack_depth_to_print = 24;

static int valid_stack_ptr(struct task_struct *task, void *p)
{
	if (p <= (void *)task->thread_info)
		return 0;
	if (kstack_end(p))
		return 0;
	return 1;
}

#ifdef CONFIG_FRAME_POINTER
static void print_context_stack(struct task_struct *task, unsigned long *stack,
			 unsigned long ebp)
{
	unsigned long addr;

	while (valid_stack_ptr(task, (void *)ebp)) {
		addr = *(unsigned long *)(ebp + 4);
		printk(" [<%08lx>] ", addr);
		print_symbol("%s", addr);
		printk("\n");
		ebp = *(unsigned long *)ebp;
	}
}
#else
static void print_context_stack(struct task_struct *task, unsigned long *stack,
			 unsigned long ebp)
{
	unsigned long addr;

	while (!kstack_end(stack)) {
		addr = *stack++;
		if (__kernel_text_address(addr)) {
			printk(" [<%08lx>]", addr);
			print_symbol(" %s", addr);
			printk("\n");
		}
	}
}
#endif

void show_trace(struct task_struct *task, unsigned long * stack)
{
	unsigned long ebp;

	if (!task)
		task = current;

	if (!valid_stack_ptr(task, stack)) {
		printk("Stack pointer is garbage, not printing trace\n");
		return;
	}

	if (task == current) {
		/* Grab ebp right from our regs */
		asm ("movl %%ebp, %0" : "=r" (ebp) : );
	} else {
		/* ebp is the last reg pushed by switch_to */
		ebp = *(unsigned long *) task->thread.esp;
	}

	while (1) {
		struct thread_info *context;
		context = (struct thread_info *)
			((unsigned long)stack & (~(THREAD_SIZE - 1)));
		print_context_stack(task, stack, ebp);
		stack = (unsigned long*)context->previous_esp;
		if (!stack)
			break;
		printk(" =======================\n");
	}
}

void show_stack(struct task_struct *task, unsigned long *esp)
{
	unsigned long *stack;
	int i;

	if (esp == NULL) {
		if (task)
			esp = (unsigned long*)task->thread.esp;
		else
			esp = (unsigned long *)&esp;
	}

	stack = esp;
	for(i = 0; i < kstack_depth_to_print; i++) {
		if (kstack_end(stack))
			break;
		if (i && ((i % 8) == 0))
			printk("\n       ");
		printk("%08lx ", *stack++);
	}
	printk("\nCall Trace:\n");
	show_trace(task, esp);
}

/*
 * The architecture-independent dump_stack generator
 */
void dump_stack(void)
{
	unsigned long stack;

	show_trace(current, &stack);
}

EXPORT_SYMBOL(dump_stack);

void show_registers(struct pt_regs *regs)
{
	int i;
	int in_kernel = 1;
	unsigned long esp;
	unsigned short ss;

	esp = (unsigned long) (&regs->esp);
	ss = __KERNEL_DS;
	if (regs->xcs & 3) {
		in_kernel = 0;
		esp = regs->esp;
		ss = regs->xss & 0xffff;
	}
	print_modules();
	printk("CPU:    %d\nEIP:    %04x:[<%08lx>]    %s\nEFLAGS: %08lx"
			"   (%s) \n",
		smp_processor_id(), 0xffff & regs->xcs, regs->eip,
		print_tainted(), regs->eflags, UTS_RELEASE);
	print_symbol("EIP is at %s\n", regs->eip);
	printk("eax: %08lx   ebx: %08lx   ecx: %08lx   edx: %08lx\n",
		regs->eax, regs->ebx, regs->ecx, regs->edx);
	printk("esi: %08lx   edi: %08lx   ebp: %08lx   esp: %08lx\n",
		regs->esi, regs->edi, regs->ebp, esp);
	printk("ds: %04x   es: %04x   ss: %04x\n",
		regs->xds & 0xffff, regs->xes & 0xffff, ss);
	printk("Process %s (pid: %d, threadinfo=%p task=%p)",
		current->comm, current->pid, current_thread_info(), current);
	/*
	 * When in-kernel, we also print out the stack and code at the
	 * time of the fault..
	 */
	if (in_kernel) {

		printk("\nStack: ");
		show_stack(NULL, (unsigned long*)esp);

		printk("Code: ");
		if(regs->eip < PAGE_OFFSET)
			goto bad;

		for(i=0;i<20;i++)
		{
			unsigned char c;
			if(__get_user(c, &((unsigned char*)regs->eip)[i])) {
bad:
				printk(" Bad EIP value.");
				break;
			}
			printk("%02x ", c);
		}
	}
	printk("\n");
}	

static void handle_BUG(struct pt_regs *regs)
{
	unsigned short ud2;
	unsigned short line;
	char *file;
	char c;
	unsigned long eip;

	if (regs->xcs & 3)
		goto no_bug;		/* Not in kernel */

	eip = regs->eip;

	if (eip < PAGE_OFFSET)
		goto no_bug;
	if (__get_user(ud2, (unsigned short *)eip))
		goto no_bug;
	if (ud2 != 0x0b0f)
		goto no_bug;
	if (__get_user(line, (unsigned short *)(eip + 2)))
		goto bug;
	if (__get_user(file, (char **)(eip + 4)) ||
		(unsigned long)file < PAGE_OFFSET || __get_user(c, file))
		file = "<bad filename>";

	printk("------------[ cut here ]------------\n");
	printk(KERN_ALERT "kernel BUG at %s:%d!\n", file, line);

no_bug:
	return;

	/* Here we know it was a BUG but file-n-line is unavailable */
bug:
	printk("Kernel BUG\n");
}

spinlock_t die_lock = SPIN_LOCK_UNLOCKED;

void die(const char * str, struct pt_regs * regs, long err)
{
	static int die_counter;
	int nl = 0;

	console_verbose();
	spin_lock_irq(&die_lock);
	bust_spinlocks(1);
	handle_BUG(regs);
	printk(KERN_ALERT "%s: %04lx [#%d]\n", str, err & 0xffff, ++die_counter);
#ifdef CONFIG_PREEMPT
	printk("PREEMPT ");
	nl = 1;
#endif
#ifdef CONFIG_SMP
	printk("SMP ");
	nl = 1;
#endif
#ifdef CONFIG_DEBUG_PAGEALLOC
	printk("DEBUG_PAGEALLOC");
	nl = 1;
#endif
	if (nl)
		printk("\n");
	show_registers(regs);
	bust_spinlocks(0);
	spin_unlock_irq(&die_lock);
	if (in_interrupt())
		panic("Fatal exception in interrupt");

	if (panic_on_oops) {
		printk(KERN_EMERG "Fatal exception: panic in 5 seconds\n");
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(5 * HZ);
		panic("Fatal exception");
	}
	do_exit(SIGSEGV);
}

static inline void die_if_kernel(const char * str, struct pt_regs * regs, long err)
{
	if (!(regs->eflags & VM_MASK) && !(3 & regs->xcs))
		die(str, regs, err);
}

static inline unsigned long get_cr2(void)
{
	unsigned long address;

	/* get the address */
	__asm__("movl %%cr2,%0":"=r" (address));
	return address;
}

static inline void do_trap(int trapnr, int signr, char *str, int vm86,
			   struct pt_regs * regs, long error_code, siginfo_t *info)
{
	if (regs->eflags & VM_MASK) {
		if (vm86)
			goto vm86_trap;
		goto trap_signal;
	}

	if (!(regs->xcs & 3))
		goto kernel_trap;

	trap_signal: {
		struct task_struct *tsk = current;
		tsk->thread.error_code = error_code;
		tsk->thread.trap_no = trapnr;
		if (info)
			force_sig_info(signr, info, tsk);
		else
			force_sig(signr, tsk);
		return;
	}

	kernel_trap: {
		if (!fixup_exception(regs))
			die(str, regs, error_code);
		return;
	}

	vm86_trap: {
		int ret = handle_vm86_trap((struct kernel_vm86_regs *) regs, error_code, trapnr);
		if (ret) goto trap_signal;
		return;
	}
}

#define DO_ERROR(trapnr, signr, str, name) \
asmlinkage void do_##name(struct pt_regs * regs, long error_code) \
{ \
	do_trap(trapnr, signr, str, 0, regs, error_code, NULL); \
}

#define DO_ERROR_INFO(trapnr, signr, str, name, sicode, siaddr) \
asmlinkage void do_##name(struct pt_regs * regs, long error_code) \
{ \
	siginfo_t info; \
	info.si_signo = signr; \
	info.si_errno = 0; \
	info.si_code = sicode; \
	info.si_addr = (void __user *)siaddr; \
	do_trap(trapnr, signr, str, 0, regs, error_code, &info); \
}

#define DO_VM86_ERROR(trapnr, signr, str, name) \
asmlinkage void do_##name(struct pt_regs * regs, long error_code) \
{ \
	do_trap(trapnr, signr, str, 1, regs, error_code, NULL); \
}

#define DO_VM86_ERROR_INFO(trapnr, signr, str, name, sicode, siaddr) \
asmlinkage void do_##name(struct pt_regs * regs, long error_code) \
{ \
	siginfo_t info; \
	info.si_signo = signr; \
	info.si_errno = 0; \
	info.si_code = sicode; \
	info.si_addr = (void __user *)siaddr; \
	do_trap(trapnr, signr, str, 1, regs, error_code, &info); \
}

DO_VM86_ERROR_INFO( 0, SIGFPE,  "divide error", divide_error, FPE_INTDIV, regs->eip)
DO_VM86_ERROR( 3, SIGTRAP, "int3", int3)
DO_VM86_ERROR( 4, SIGSEGV, "overflow", overflow)
DO_VM86_ERROR( 5, SIGSEGV, "bounds", bounds)
DO_ERROR_INFO( 6, SIGILL,  "invalid operand", invalid_op, ILL_ILLOPN, regs->eip)
DO_ERROR( 9, SIGFPE,  "coprocessor segment overrun", coprocessor_segment_overrun)
DO_ERROR(10, SIGSEGV, "invalid TSS", invalid_TSS)
DO_ERROR(11, SIGBUS,  "segment not present", segment_not_present)
DO_ERROR(12, SIGBUS,  "stack segment", stack_segment)
DO_ERROR_INFO(17, SIGBUS, "alignment check", alignment_check, BUS_ADRALN, get_cr2())

asmlinkage void do_general_protection(struct pt_regs * regs, long error_code)
{
	if (regs->eflags & X86_EFLAGS_IF)
		local_irq_enable();
 
	if (regs->eflags & VM_MASK)
		goto gp_in_vm86;

	if (!(regs->xcs & 3))
		goto gp_in_kernel;

	current->thread.error_code = error_code;
	current->thread.trap_no = 13;
	force_sig(SIGSEGV, current);
	return;

gp_in_vm86:
	local_irq_enable();
	handle_vm86_fault((struct kernel_vm86_regs *) regs, error_code);
	return;

gp_in_kernel:
	if (!fixup_exception(regs))
		die("general protection fault", regs, error_code);
}

static void mem_parity_error(unsigned char reason, struct pt_regs * regs)
{
	printk("Uhhuh. NMI received. Dazed and confused, but trying to continue\n");
	printk("You probably have a hardware problem with your RAM chips\n");

	/* Clear and disable the memory parity error line. */
	clear_mem_error(reason);
}

static void io_check_error(unsigned char reason, struct pt_regs * regs)
{
	unsigned long i;

	printk("NMI: IOCK error (debug interrupt?)\n");
	show_registers(regs);

	/* Re-enable the IOCK line, wait for a few seconds */
	reason = (reason & 0xf) | 8;
	outb(reason, 0x61);
	i = 2000;
	while (--i) udelay(1000);
	reason &= ~8;
	outb(reason, 0x61);
}

static void unknown_nmi_error(unsigned char reason, struct pt_regs * regs)
{
#ifdef CONFIG_MCA
	/* Might actually be able to figure out what the guilty party
	* is. */
	if( MCA_bus ) {
		mca_handle_nmi();
		return;
	}
#endif
	printk("Uhhuh. NMI received for unknown reason %02x on CPU %d.\n",
		reason, smp_processor_id());
	printk("Dazed and confused, but trying to continue\n");
	printk("Do you have a strange power saving mode enabled?\n");
}

static void default_do_nmi(struct pt_regs * regs)
{
	unsigned char reason = get_nmi_reason();
 
	if (!(reason & 0xc0)) {
#ifdef CONFIG_X86_LOCAL_APIC
		/*
		 * Ok, so this is none of the documented NMI sources,
		 * so it must be the NMI watchdog.
		 */
		if (nmi_watchdog) {
			nmi_watchdog_tick(regs);
			return;
		}
#endif
		unknown_nmi_error(reason, regs);
		return;
	}
	if (reason & 0x80)
		mem_parity_error(reason, regs);
	if (reason & 0x40)
		io_check_error(reason, regs);
	/*
	 * Reassert NMI in case it became active meanwhile
	 * as it's edge-triggered.
	 */
	reassert_nmi();
}

static int dummy_nmi_callback(struct pt_regs * regs, int cpu)
{
	return 0;
}
 
static nmi_callback_t nmi_callback = dummy_nmi_callback;
 
asmlinkage void do_nmi(struct pt_regs * regs, long error_code)
{
	int cpu;

	nmi_enter();

	cpu = smp_processor_id();
	++nmi_count(cpu);

	if (!nmi_callback(regs, cpu))
		default_do_nmi(regs);

	nmi_exit();
}

void set_nmi_callback(nmi_callback_t callback)
{
	nmi_callback = callback;
}

void unset_nmi_callback(void)
{
	nmi_callback = dummy_nmi_callback;
}

/*
 * Our handling of the processor debug registers is non-trivial.
 * We do not clear them on entry and exit from the kernel. Therefore
 * it is possible to get a watchpoint trap here from inside the kernel.
 * However, the code in ./ptrace.c has ensured that the user can
 * only set watchpoints on userspace addresses. Therefore the in-kernel
 * watchpoint trap can only occur in code which is reading/writing
 * from user space. Such code must not hold kernel locks (since it
 * can equally take a page fault), therefore it is safe to call
 * force_sig_info even though that claims and releases locks.
 * 
 * Code in ./signal.c ensures that the debug control register
 * is restored before we deliver any signal, and therefore that
 * user code runs with the correct debug control register even though
 * we clear it here.
 *
 * Being careful here means that we don't have to be as careful in a
 * lot of more complicated places (task switching can be a bit lazy
 * about restoring all the debug state, and ptrace doesn't have to
 * find every occurrence of the TF bit that could be saved away even
 * by user code)
 */
asmlinkage void do_debug(struct pt_regs * regs, long error_code)
{
	unsigned int condition;
	struct task_struct *tsk = current;
	siginfo_t info;

	__asm__ __volatile__("movl %%db6,%0" : "=r" (condition));

	/* It's safe to allow irq's after DR6 has been saved */
	if (regs->eflags & X86_EFLAGS_IF)
		local_irq_enable();

	/* Mask out spurious debug traps due to lazy DR7 setting */
	if (condition & (DR_TRAP0|DR_TRAP1|DR_TRAP2|DR_TRAP3)) {
		if (!tsk->thread.debugreg[7])
			goto clear_dr7;
	}

	if (regs->eflags & VM_MASK)
		goto debug_vm86;

	/* Save debug status register where ptrace can see it */
	tsk->thread.debugreg[6] = condition;

	/* Mask out spurious TF errors due to lazy TF clearing */
	if (condition & DR_STEP) {
		/*
		 * The TF error should be masked out only if the current
		 * process is not traced and if the TRAP flag has been set
		 * previously by a tracing process (condition detected by
		 * the PT_DTRACE flag); remember that the i386 TRAP flag
		 * can be modified by the process itself in user mode,
		 * allowing programs to debug themselves without the ptrace()
		 * interface.
		 */
		if ((regs->xcs & 3) == 0)
			goto clear_TF_reenable;
		if ((tsk->ptrace & (PT_DTRACE|PT_PTRACED)) == PT_DTRACE)
			goto clear_TF;
	}

	/* Ok, finally something we can handle */
	tsk->thread.trap_no = 1;
	tsk->thread.error_code = error_code;
	info.si_signo = SIGTRAP;
	info.si_errno = 0;
	info.si_code = TRAP_BRKPT;
	
	/* If this is a kernel mode trap, save the user PC on entry to 
	 * the kernel, that's what the debugger can make sense of.
	 */
	info.si_addr = ((regs->xcs & 3) == 0) ? (void __user *)tsk->thread.eip
	                                      : (void __user *)regs->eip;
	force_sig_info(SIGTRAP, &info, tsk);

	/* Disable additional traps. They'll be re-enabled when
	 * the signal is delivered.
	 */
clear_dr7:
	__asm__("movl %0,%%db7"
		: /* no output */
		: "r" (0));
	return;

debug_vm86:
	handle_vm86_trap((struct kernel_vm86_regs *) regs, error_code, 1);
	return;

clear_TF_reenable:
	set_tsk_thread_flag(tsk, TIF_SINGLESTEP);
clear_TF:
	regs->eflags &= ~TF_MASK;
	return;
}

/*
 * Note that we play around with the 'TS' bit in an attempt to get
 * the correct behaviour even in the presence of the asynchronous
 * IRQ13 behaviour
 */
void math_error(void __user *eip)
{
	struct task_struct * task;
	siginfo_t info;
	unsigned short cwd, swd;

	/*
	 * Save the info for the exception handler and clear the error.
	 */
	task = current;
	save_init_fpu(task);
	task->thread.trap_no = 16;
	task->thread.error_code = 0;
	info.si_signo = SIGFPE;
	info.si_errno = 0;
	info.si_code = __SI_FAULT;
	info.si_addr = eip;
	/*
	 * (~cwd & swd) will mask out exceptions that are not set to unmasked
	 * status.  0x3f is the exception bits in these regs, 0x200 is the
	 * C1 reg you need in case of a stack fault, 0x040 is the stack
	 * fault bit.  We should only be taking one exception at a time,
	 * so if this combination doesn't produce any single exception,
	 * then we have a bad program that isn't syncronizing its FPU usage
	 * and it will suffer the consequences since we won't be able to
	 * fully reproduce the context of the exception
	 */
	cwd = get_fpu_cwd(task);
	swd = get_fpu_swd(task);
	switch (((~cwd) & swd & 0x3f) | (swd & 0x240)) {
		case 0x000:
		default:
			break;
		case 0x001: /* Invalid Op */
		case 0x041: /* Stack Fault */
		case 0x241: /* Stack Fault | Direction */
			info.si_code = FPE_FLTINV;
			/* Should we clear the SF or let user space do it ???? */
			break;
		case 0x002: /* Denormalize */
		case 0x010: /* Underflow */
			info.si_code = FPE_FLTUND;
			break;
		case 0x004: /* Zero Divide */
			info.si_code = FPE_FLTDIV;
			break;
		case 0x008: /* Overflow */
			info.si_code = FPE_FLTOVF;
			break;
		case 0x020: /* Precision */
			info.si_code = FPE_FLTRES;
			break;
	}
	force_sig_info(SIGFPE, &info, task);
}

asmlinkage void do_coprocessor_error(struct pt_regs * regs, long error_code)
{
	ignore_fpu_irq = 1;
	math_error((void __user *)regs->eip);
}

void simd_math_error(void __user *eip)
{
	struct task_struct * task;
	siginfo_t info;
	unsigned short mxcsr;

	/*
	 * Save the info for the exception handler and clear the error.
	 */
	task = current;
	save_init_fpu(task);
	task->thread.trap_no = 19;
	task->thread.error_code = 0;
	info.si_signo = SIGFPE;
	info.si_errno = 0;
	info.si_code = __SI_FAULT;
	info.si_addr = eip;
	/*
	 * The SIMD FPU exceptions are handled a little differently, as there
	 * is only a single status/control register.  Thus, to determine which
	 * unmasked exception was caught we must mask the exception mask bits
	 * at 0x1f80, and then use these to mask the exception bits at 0x3f.
	 */
	mxcsr = get_fpu_mxcsr(task);
	switch (~((mxcsr & 0x1f80) >> 7) & (mxcsr & 0x3f)) {
		case 0x000:
		default:
			break;
		case 0x001: /* Invalid Op */
			info.si_code = FPE_FLTINV;
			break;
		case 0x002: /* Denormalize */
		case 0x010: /* Underflow */
			info.si_code = FPE_FLTUND;
			break;
		case 0x004: /* Zero Divide */
			info.si_code = FPE_FLTDIV;
			break;
		case 0x008: /* Overflow */
			info.si_code = FPE_FLTOVF;
			break;
		case 0x020: /* Precision */
			info.si_code = FPE_FLTRES;
			break;
	}
	force_sig_info(SIGFPE, &info, task);
}

asmlinkage void do_simd_coprocessor_error(struct pt_regs * regs,
					  long error_code)
{
	if (cpu_has_xmm) {
		/* Handle SIMD FPU exceptions on PIII+ processors. */
		ignore_fpu_irq = 1;
		simd_math_error((void __user *)regs->eip);
	} else {
		/*
		 * Handle strange cache flush from user space exception
		 * in all other cases.  This is undocumented behaviour.
		 */
		if (regs->eflags & VM_MASK) {
			handle_vm86_fault((struct kernel_vm86_regs *)regs,
					  error_code);
			return;
		}
		die_if_kernel("cache flush denied", regs, error_code);
		current->thread.trap_no = 19;
		current->thread.error_code = error_code;
		force_sig(SIGSEGV, current);
	}
}

asmlinkage void do_spurious_interrupt_bug(struct pt_regs * regs,
					  long error_code)
{
#if 0
	/* No need to warn about this any longer. */
	printk("Ignoring P6 Local APIC Spurious Interrupt Bug...\n");
#endif
}

/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 *
 * Careful.. There are problems with IBM-designed IRQ13 behaviour.
 * Don't touch unless you *really* know how it works.
 *
 * Must be called with kernel preemption disabled (in this case,
 * local interrupts are disabled at the call-site in entry.S).
 */
asmlinkage void math_state_restore(struct pt_regs regs)
{
	struct thread_info *thread = current_thread_info();
	struct task_struct *tsk = thread->task;

	clts();		/* Allow maths ops (or we recurse) */
	if (!tsk->used_math)
		init_fpu(tsk);
	restore_fpu(tsk);
	thread->status |= TS_USEDFPU;	/* So we fnsave on switch_to() */
}

#ifndef CONFIG_MATH_EMULATION

asmlinkage void math_emulate(long arg)
{
	printk("math-emulation not enabled and no coprocessor found.\n");
	printk("killing %s.\n",current->comm);
	force_sig(SIGFPE,current);
	schedule();
}

#endif /* CONFIG_MATH_EMULATION */

#ifdef CONFIG_X86_F00F_BUG
void __init trap_init_f00f_bug(void)
{
	__set_fixmap(FIX_F00F_IDT, __pa(&idt_table), PAGE_KERNEL_RO);

	/*
	 * Update the IDT descriptor and reload the IDT so that
	 * it uses the read-only mapped virtual address.
	 */
	idt_descr.address = fix_to_virt(FIX_F00F_IDT);
	__asm__ __volatile__("lidt %0" : : "m" (idt_descr));
}
#endif

#define _set_gate(gate_addr,type,dpl,addr,seg) \
do { \
  int __d0, __d1; \
  __asm__ __volatile__ ("movw %%dx,%%ax\n\t" \
	"movw %4,%%dx\n\t" \
	"movl %%eax,%0\n\t" \
	"movl %%edx,%1" \
	:"=m" (*((long *) (gate_addr))), \
	 "=m" (*(1+(long *) (gate_addr))), "=&a" (__d0), "=&d" (__d1) \
	:"i" ((short) (0x8000+(dpl<<13)+(type<<8))), \
	 "3" ((char *) (addr)),"2" ((seg) << 16)); \
} while (0)


/*
 * This needs to use 'idt_table' rather than 'idt', and
 * thus use the _nonmapped_ version of the IDT, as the
 * Pentium F0 0F bugfix can have resulted in the mapped
 * IDT being write-protected.
 */
void set_intr_gate(unsigned int n, void *addr)
{
	_set_gate(idt_table+n,14,0,addr,__KERNEL_CS);
}

static void __init set_trap_gate(unsigned int n, void *addr)
{
	_set_gate(idt_table+n,15,0,addr,__KERNEL_CS);
}

static void __init set_system_gate(unsigned int n, void *addr)
{
	_set_gate(idt_table+n,15,3,addr,__KERNEL_CS);
}

static void __init set_call_gate(void *a, void *addr)
{
	_set_gate(a,12,3,addr,__KERNEL_CS);
}

static void __init set_task_gate(unsigned int n, unsigned int gdt_entry)
{
	_set_gate(idt_table+n,5,0,0,(gdt_entry<<3));
}


void __init trap_init(void)
{
#ifdef CONFIG_EISA
	if (isa_readl(0x0FFFD9) == 'E'+('I'<<8)+('S'<<16)+('A'<<24)) {
		EISA_bus = 1;
	}
#endif

#ifdef CONFIG_X86_LOCAL_APIC
	init_apic_mappings();
#endif

	set_trap_gate(0,&divide_error);
	set_intr_gate(1,&debug);
	set_intr_gate(2,&nmi);
	set_system_gate(3,&int3);	/* int3-5 can be called from all */
	set_system_gate(4,&overflow);
	set_system_gate(5,&bounds);
	set_trap_gate(6,&invalid_op);
	set_trap_gate(7,&device_not_available);
	set_task_gate(8,GDT_ENTRY_DOUBLEFAULT_TSS);
	set_trap_gate(9,&coprocessor_segment_overrun);
	set_trap_gate(10,&invalid_TSS);
	set_trap_gate(11,&segment_not_present);
	set_trap_gate(12,&stack_segment);
	set_trap_gate(13,&general_protection);
	set_intr_gate(14,&page_fault);
	set_trap_gate(15,&spurious_interrupt_bug);
	set_trap_gate(16,&coprocessor_error);
	set_trap_gate(17,&alignment_check);
#ifdef CONFIG_X86_MCE
	set_trap_gate(18,&machine_check);
#endif
	set_trap_gate(19,&simd_coprocessor_error);

	set_system_gate(SYSCALL_VECTOR,&system_call);

	/*
	 * default LDT is a single-entry callgate to lcall7 for iBCS
	 * and a callgate to lcall27 for Solaris/x86 binaries
	 */
	set_call_gate(&default_ldt[0],lcall7);
	set_call_gate(&default_ldt[4],lcall27);

	/*
	 * Should be a barrier for any external CPU state.
	 */
	cpu_init();

	trap_init_hook();
}
