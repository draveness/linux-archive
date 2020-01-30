#ifndef _X86_64_PTRACE_H
#define _X86_64_PTRACE_H

#if defined(__ASSEMBLY__) || defined(__FRAME_OFFSETS) 
#define R15 0
#define R14 8
#define R13 16
#define R12 24
#define RBP 32
#define RBX 40
/* arguments: interrupts/non tracing syscalls only save upto here*/
#define R11 48
#define R10 56	
#define R9 64
#define R8 72
#define RAX 80
#define RCX 88
#define RDX 96
#define RSI 104
#define RDI 112
#define ORIG_RAX 120       /* = ERROR */ 
/* end of arguments */ 	
/* cpu exception frame or undefined in case of fast syscall. */
#define RIP 128
#define CS 136
#define EFLAGS 144
#define RSP 152
#define SS 160
#define ARGOFFSET R11
#endif /* __ASSEMBLY__ */

/* top of stack page */ 
#define FRAME_SIZE 168

#define PTRACE_OLDSETOPTIONS         21

#ifndef __ASSEMBLY__ 

struct pt_regs {
	unsigned long r15;
	unsigned long r14;
	unsigned long r13;
	unsigned long r12;
	unsigned long rbp;
	unsigned long rbx;
/* arguments: non interrupts/non tracing syscalls only save upto here*/
 	unsigned long r11;
	unsigned long r10;	
	unsigned long r9;
	unsigned long r8;
	unsigned long rax;
	unsigned long rcx;
	unsigned long rdx;
	unsigned long rsi;
	unsigned long rdi;
	unsigned long orig_rax;
/* end of arguments */ 	
/* cpu exception frame or undefined */
	unsigned long rip;
	unsigned long cs;
	unsigned long eflags; 
	unsigned long rsp; 
	unsigned long ss;
/* top of stack page */ 
};

#endif

/* Arbitrarily choose the same ptrace numbers as used by the Sparc code. */
#define PTRACE_GETREGS            12
#define PTRACE_SETREGS            13
#define PTRACE_GETFPREGS          14
#define PTRACE_SETFPREGS          15
#define PTRACE_GETFPXREGS         18
#define PTRACE_SETFPXREGS         19

/* only useful for access 32bit programs */
#define PTRACE_GET_THREAD_AREA    25
#define PTRACE_SET_THREAD_AREA    26

#define PTRACE_ARCH_PRCTL	  30	/* arch_prctl for child */

#if defined(__KERNEL__) && !defined(__ASSEMBLY__) 
#define user_mode(regs) (!!((regs)->cs & 3))
#define instruction_pointer(regs) ((regs)->rip)
void signal_fault(struct pt_regs *regs, void __user *frame, char *where);

enum {
        EF_CF   = 0x00000001,
        EF_PF   = 0x00000004,
        EF_AF   = 0x00000010,
        EF_ZF   = 0x00000040,
        EF_SF   = 0x00000080,
        EF_TF   = 0x00000100,
        EF_IE   = 0x00000200,
        EF_DF   = 0x00000400,
        EF_OF   = 0x00000800,
        EF_IOPL = 0x00003000,
        EF_IOPL_RING0 = 0x00000000,
        EF_IOPL_RING1 = 0x00001000,
        EF_IOPL_RING2 = 0x00002000,
        EF_NT   = 0x00004000,   /* nested task */
        EF_RF   = 0x00010000,   /* resume */
        EF_VM   = 0x00020000,   /* virtual mode */
        EF_AC   = 0x00040000,   /* alignment */
        EF_VIF  = 0x00080000,   /* virtual interrupt */
        EF_VIP  = 0x00100000,   /* virtual interrupt pending */
        EF_ID   = 0x00200000,   /* id */
};

#endif

#endif
