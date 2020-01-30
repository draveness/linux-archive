#include "linux/kernel.h"
#include "linux/smp.h"
#include "linux/sched.h"
#include "asm/ptrace.h"
#include "sysrq.h"

void show_regs(struct pt_regs *regs)
{
        printk("\n");
        printk("EIP: %04lx:[<%08lx>] CPU: %d %s", 
	       0xffff & PT_REGS_CS(regs), PT_REGS_IP(regs),
	       smp_processor_id(), print_tainted());
        if (PT_REGS_CS(regs) & 3)
                printk(" ESP: %04lx:%08lx", 0xffff & PT_REGS_SS(regs),
		       PT_REGS_SP(regs));
        printk(" EFLAGS: %08lx\n    %s\n", PT_REGS_EFLAGS(regs),
	       print_tainted());
        printk("EAX: %08lx EBX: %08lx ECX: %08lx EDX: %08lx\n",
                PT_REGS_EAX(regs), PT_REGS_EBX(regs), 
	       PT_REGS_ECX(regs), 
	       PT_REGS_EDX(regs));
        printk("ESI: %08lx EDI: %08lx EBP: %08lx",
	       PT_REGS_ESI(regs), PT_REGS_EDI(regs), 
	       PT_REGS_EBP(regs));
        printk(" DS: %04lx ES: %04lx\n",
	       0xffff & PT_REGS_DS(regs), 
	       0xffff & PT_REGS_ES(regs));

        show_trace((unsigned long *) &regs);
}
