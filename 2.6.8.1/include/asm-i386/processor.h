/*
 * include/asm-i386/processor.h
 *
 * Copyright (C) 1994 Linus Torvalds
 */

#ifndef __ASM_I386_PROCESSOR_H
#define __ASM_I386_PROCESSOR_H

#include <asm/vm86.h>
#include <asm/math_emu.h>
#include <asm/segment.h>
#include <asm/page.h>
#include <asm/types.h>
#include <asm/sigcontext.h>
#include <asm/cpufeature.h>
#include <asm/msr.h>
#include <asm/system.h>
#include <linux/cache.h>
#include <linux/config.h>
#include <linux/threads.h>

/* flag for disabling the tsc */
extern int tsc_disable;

struct desc_struct {
	unsigned long a,b;
};

#define desc_empty(desc) \
		(!((desc)->a + (desc)->b))

#define desc_equal(desc1, desc2) \
		(((desc1)->a == (desc2)->a) && ((desc1)->b == (desc2)->b))
/*
 * Default implementation of macro that returns current
 * instruction pointer ("program counter").
 */
#define current_text_addr() ({ void *pc; __asm__("movl $1f,%0\n1:":"=g" (pc)); pc; })

/*
 *  CPU type and hardware bug flags. Kept separately for each CPU.
 *  Members of this structure are referenced in head.S, so think twice
 *  before touching them. [mj]
 */

struct cpuinfo_x86 {
	__u8	x86;		/* CPU family */
	__u8	x86_vendor;	/* CPU vendor */
	__u8	x86_model;
	__u8	x86_mask;
	char	wp_works_ok;	/* It doesn't on 386's */
	char	hlt_works_ok;	/* Problems on some 486Dx4's and old 386's */
	char	hard_math;
	char	rfu;
       	int	cpuid_level;	/* Maximum supported CPUID level, -1=no CPUID */
	unsigned long	x86_capability[NCAPINTS];
	char	x86_vendor_id[16];
	char	x86_model_id[64];
	int 	x86_cache_size;  /* in KB - valid for CPUS which support this
				    call  */
	int 	x86_cache_alignment;	/* In bytes */
	int	fdiv_bug;
	int	f00f_bug;
	int	coma_bug;
	unsigned long loops_per_jiffy;
} __attribute__((__aligned__(SMP_CACHE_BYTES)));

#define X86_VENDOR_INTEL 0
#define X86_VENDOR_CYRIX 1
#define X86_VENDOR_AMD 2
#define X86_VENDOR_UMC 3
#define X86_VENDOR_NEXGEN 4
#define X86_VENDOR_CENTAUR 5
#define X86_VENDOR_RISE 6
#define X86_VENDOR_TRANSMETA 7
#define X86_VENDOR_NSC 8
#define X86_VENDOR_NUM 9
#define X86_VENDOR_UNKNOWN 0xff

/*
 * capabilities of CPUs
 */

extern struct cpuinfo_x86 boot_cpu_data;
extern struct cpuinfo_x86 new_cpu_data;
extern struct tss_struct init_tss[NR_CPUS];
extern struct tss_struct doublefault_tss;

#ifdef CONFIG_SMP
extern struct cpuinfo_x86 cpu_data[];
#define current_cpu_data cpu_data[smp_processor_id()]
#else
#define cpu_data (&boot_cpu_data)
#define current_cpu_data boot_cpu_data
#endif

extern char ignore_fpu_irq;

extern void identify_cpu(struct cpuinfo_x86 *);
extern void print_cpu_info(struct cpuinfo_x86 *);
extern void dodgy_tsc(void);

/*
 * EFLAGS bits
 */
#define X86_EFLAGS_CF	0x00000001 /* Carry Flag */
#define X86_EFLAGS_PF	0x00000004 /* Parity Flag */
#define X86_EFLAGS_AF	0x00000010 /* Auxillary carry Flag */
#define X86_EFLAGS_ZF	0x00000040 /* Zero Flag */
#define X86_EFLAGS_SF	0x00000080 /* Sign Flag */
#define X86_EFLAGS_TF	0x00000100 /* Trap Flag */
#define X86_EFLAGS_IF	0x00000200 /* Interrupt Flag */
#define X86_EFLAGS_DF	0x00000400 /* Direction Flag */
#define X86_EFLAGS_OF	0x00000800 /* Overflow Flag */
#define X86_EFLAGS_IOPL	0x00003000 /* IOPL mask */
#define X86_EFLAGS_NT	0x00004000 /* Nested Task */
#define X86_EFLAGS_RF	0x00010000 /* Resume Flag */
#define X86_EFLAGS_VM	0x00020000 /* Virtual Mode */
#define X86_EFLAGS_AC	0x00040000 /* Alignment Check */
#define X86_EFLAGS_VIF	0x00080000 /* Virtual Interrupt Flag */
#define X86_EFLAGS_VIP	0x00100000 /* Virtual Interrupt Pending */
#define X86_EFLAGS_ID	0x00200000 /* CPUID detection flag */

/*
 * Generic CPUID function
 */
static inline void cpuid(int op, int *eax, int *ebx, int *ecx, int *edx)
{
	__asm__("cpuid"
		: "=a" (*eax),
		  "=b" (*ebx),
		  "=c" (*ecx),
		  "=d" (*edx)
		: "0" (op));
}

/*
 * CPUID functions returning a single datum
 */
static inline unsigned int cpuid_eax(unsigned int op)
{
	unsigned int eax;

	__asm__("cpuid"
		: "=a" (eax)
		: "0" (op)
		: "bx", "cx", "dx");
	return eax;
}
static inline unsigned int cpuid_ebx(unsigned int op)
{
	unsigned int eax, ebx;

	__asm__("cpuid"
		: "=a" (eax), "=b" (ebx)
		: "0" (op)
		: "cx", "dx" );
	return ebx;
}
static inline unsigned int cpuid_ecx(unsigned int op)
{
	unsigned int eax, ecx;

	__asm__("cpuid"
		: "=a" (eax), "=c" (ecx)
		: "0" (op)
		: "bx", "dx" );
	return ecx;
}
static inline unsigned int cpuid_edx(unsigned int op)
{
	unsigned int eax, edx;

	__asm__("cpuid"
		: "=a" (eax), "=d" (edx)
		: "0" (op)
		: "bx", "cx");
	return edx;
}

#define load_cr3(pgdir) \
	asm volatile("movl %0,%%cr3": :"r" (__pa(pgdir)))


/*
 * Intel CPU features in CR4
 */
#define X86_CR4_VME		0x0001	/* enable vm86 extensions */
#define X86_CR4_PVI		0x0002	/* virtual interrupts flag enable */
#define X86_CR4_TSD		0x0004	/* disable time stamp at ipl 3 */
#define X86_CR4_DE		0x0008	/* enable debugging extensions */
#define X86_CR4_PSE		0x0010	/* enable page size extensions */
#define X86_CR4_PAE		0x0020	/* enable physical address extensions */
#define X86_CR4_MCE		0x0040	/* Machine check enable */
#define X86_CR4_PGE		0x0080	/* enable global pages */
#define X86_CR4_PCE		0x0100	/* enable performance counters at ipl 3 */
#define X86_CR4_OSFXSR		0x0200	/* enable fast FPU save and restore */
#define X86_CR4_OSXMMEXCPT	0x0400	/* enable unmasked SSE exceptions */

/*
 * Save the cr4 feature set we're using (ie
 * Pentium 4MB enable and PPro Global page
 * enable), so that any CPU's that boot up
 * after us can get the correct flags.
 */
extern unsigned long mmu_cr4_features;

static inline void set_in_cr4 (unsigned long mask)
{
	mmu_cr4_features |= mask;
	__asm__("movl %%cr4,%%eax\n\t"
		"orl %0,%%eax\n\t"
		"movl %%eax,%%cr4\n"
		: : "irg" (mask)
		:"ax");
}

static inline void clear_in_cr4 (unsigned long mask)
{
	mmu_cr4_features &= ~mask;
	__asm__("movl %%cr4,%%eax\n\t"
		"andl %0,%%eax\n\t"
		"movl %%eax,%%cr4\n"
		: : "irg" (~mask)
		:"ax");
}

/*
 *      NSC/Cyrix CPU configuration register indexes
 */

#define CX86_PCR0 0x20
#define CX86_GCR  0xb8
#define CX86_CCR0 0xc0
#define CX86_CCR1 0xc1
#define CX86_CCR2 0xc2
#define CX86_CCR3 0xc3
#define CX86_CCR4 0xe8
#define CX86_CCR5 0xe9
#define CX86_CCR6 0xea
#define CX86_CCR7 0xeb
#define CX86_PCR1 0xf0
#define CX86_DIR0 0xfe
#define CX86_DIR1 0xff
#define CX86_ARR_BASE 0xc4
#define CX86_RCR_BASE 0xdc

/*
 *      NSC/Cyrix CPU indexed register access macros
 */

#define getCx86(reg) ({ outb((reg), 0x22); inb(0x23); })

#define setCx86(reg, data) do { \
	outb((reg), 0x22); \
	outb((data), 0x23); \
} while (0)

/*
 * Bus types (default is ISA, but people can check others with these..)
 */
extern int MCA_bus;

static inline void __monitor(const void *eax, unsigned long ecx,
		unsigned long edx)
{
	/* "monitor %eax,%ecx,%edx;" */
	asm volatile(
		".byte 0x0f,0x01,0xc8;"
		: :"a" (eax), "c" (ecx), "d"(edx));
}

static inline void __mwait(unsigned long eax, unsigned long ecx)
{
	/* "mwait %eax,%ecx;" */
	asm volatile(
		".byte 0x0f,0x01,0xc9;"
		: :"a" (eax), "c" (ecx));
}

/* from system description table in BIOS.  Mostly for MCA use, but
others may find it useful. */
extern unsigned int machine_id;
extern unsigned int machine_submodel_id;
extern unsigned int BIOS_revision;
extern unsigned int mca_pentium_flag;

/*
 * User space process size: 3GB (default).
 */
#define TASK_SIZE	(PAGE_OFFSET)

/* This decides where the kernel will search for a free chunk of vm
 * space during mmap's.
 */
#define TASK_UNMAPPED_BASE	(PAGE_ALIGN(TASK_SIZE / 3))

/*
 * Size of io_bitmap.
 */
#define IO_BITMAP_BITS  65536
#define IO_BITMAP_BYTES (IO_BITMAP_BITS/8)
#define IO_BITMAP_LONGS (IO_BITMAP_BYTES/sizeof(long))
#define IO_BITMAP_OFFSET offsetof(struct tss_struct,io_bitmap)
#define INVALID_IO_BITMAP_OFFSET 0x8000

struct i387_fsave_struct {
	long	cwd;
	long	swd;
	long	twd;
	long	fip;
	long	fcs;
	long	foo;
	long	fos;
	long	st_space[20];	/* 8*10 bytes for each FP-reg = 80 bytes */
	long	status;		/* software status information */
};

struct i387_fxsave_struct {
	unsigned short	cwd;
	unsigned short	swd;
	unsigned short	twd;
	unsigned short	fop;
	long	fip;
	long	fcs;
	long	foo;
	long	fos;
	long	mxcsr;
	long	mxcsr_mask;
	long	st_space[32];	/* 8*16 bytes for each FP-reg = 128 bytes */
	long	xmm_space[32];	/* 8*16 bytes for each XMM-reg = 128 bytes */
	long	padding[56];
} __attribute__ ((aligned (16)));

struct i387_soft_struct {
	long	cwd;
	long	swd;
	long	twd;
	long	fip;
	long	fcs;
	long	foo;
	long	fos;
	long	st_space[20];	/* 8*10 bytes for each FP-reg = 80 bytes */
	unsigned char	ftop, changed, lookahead, no_update, rm, alimit;
	struct info	*info;
	unsigned long	entry_eip;
};

union i387_union {
	struct i387_fsave_struct	fsave;
	struct i387_fxsave_struct	fxsave;
	struct i387_soft_struct soft;
};

typedef struct {
	unsigned long seg;
} mm_segment_t;

struct tss_struct {
	unsigned short	back_link,__blh;
	unsigned long	esp0;
	unsigned short	ss0,__ss0h;
	unsigned long	esp1;
	unsigned short	ss1,__ss1h;	/* ss1 is used to cache MSR_IA32_SYSENTER_CS */
	unsigned long	esp2;
	unsigned short	ss2,__ss2h;
	unsigned long	__cr3;
	unsigned long	eip;
	unsigned long	eflags;
	unsigned long	eax,ecx,edx,ebx;
	unsigned long	esp;
	unsigned long	ebp;
	unsigned long	esi;
	unsigned long	edi;
	unsigned short	es, __esh;
	unsigned short	cs, __csh;
	unsigned short	ss, __ssh;
	unsigned short	ds, __dsh;
	unsigned short	fs, __fsh;
	unsigned short	gs, __gsh;
	unsigned short	ldt, __ldth;
	unsigned short	trace, io_bitmap_base;
	/*
	 * The extra 1 is there because the CPU will access an
	 * additional byte beyond the end of the IO permission
	 * bitmap. The extra byte must be all 1 bits, and must
	 * be within the limit.
	 */
	unsigned long	io_bitmap[IO_BITMAP_LONGS + 1];
	/*
	 * pads the TSS to be cacheline-aligned (size is 0x100)
	 */
	unsigned long __cacheline_filler[37];
	/*
	 * .. and then another 0x100 bytes for emergency kernel stack
	 */
	unsigned long stack[64];
} __attribute__((packed));

#define ARCH_MIN_TASKALIGN	16

struct thread_struct {
/* cached TLS descriptors. */
	struct desc_struct tls_array[GDT_ENTRY_TLS_ENTRIES];
	unsigned long	esp0;
	unsigned long	sysenter_cs;
	unsigned long	eip;
	unsigned long	esp;
	unsigned long	fs;
	unsigned long	gs;
/* Hardware debugging registers */
	unsigned long	debugreg[8];  /* %%db0-7 debug registers */
/* fault info */
	unsigned long	cr2, trap_no, error_code;
/* floating point info */
	union i387_union	i387;
/* virtual 86 mode info */
	struct vm86_struct __user * vm86_info;
	unsigned long		screen_bitmap;
	unsigned long		v86flags, v86mask, saved_esp0;
	unsigned int		saved_fs, saved_gs;
/* IO permissions */
	unsigned long	*io_bitmap_ptr;
};

#define INIT_THREAD  {							\
	.vm86_info = NULL,						\
	.sysenter_cs = __KERNEL_CS,					\
	.io_bitmap_ptr = NULL,						\
}

/*
 * Note that the .io_bitmap member must be extra-big. This is because
 * the CPU will access an additional byte beyond the end of the IO
 * permission bitmap. The extra byte must be all 1 bits, and must
 * be within the limit.
 */
#define INIT_TSS  {							\
	.esp0		= sizeof(init_stack) + (long)&init_stack,	\
	.ss0		= __KERNEL_DS,					\
	.esp1		= sizeof(init_tss[0]) + (long)&init_tss[0],	\
	.ss1		= __KERNEL_CS,					\
	.ldt		= GDT_ENTRY_LDT,				\
	.io_bitmap_base	= INVALID_IO_BITMAP_OFFSET,			\
	.io_bitmap	= { [ 0 ... IO_BITMAP_LONGS] = ~0 },		\
}

static inline void load_esp0(struct tss_struct *tss, struct thread_struct *thread)
{
	tss->esp0 = thread->esp0;
	/* This can only happen when SEP is enabled, no need to test "SEP"arately */
	if (unlikely(tss->ss1 != thread->sysenter_cs)) {
		tss->ss1 = thread->sysenter_cs;
		wrmsr(MSR_IA32_SYSENTER_CS, thread->sysenter_cs, 0);
	}
}

#define start_thread(regs, new_eip, new_esp) do {		\
	__asm__("movl %0,%%fs ; movl %0,%%gs": :"r" (0));	\
	set_fs(USER_DS);					\
	regs->xds = __USER_DS;					\
	regs->xes = __USER_DS;					\
	regs->xss = __USER_DS;					\
	regs->xcs = __USER_CS;					\
	regs->eip = new_eip;					\
	regs->esp = new_esp;					\
} while (0)

/* Forward declaration, a strange C thing */
struct task_struct;
struct mm_struct;

/* Free all resources held by a thread. */
extern void release_thread(struct task_struct *);

/* Prepare to copy thread state - unlazy all lazy status */
extern void prepare_to_copy(struct task_struct *tsk);

/*
 * create a kernel thread without removing it from tasklists
 */
extern int kernel_thread(int (*fn)(void *), void * arg, unsigned long flags);

extern unsigned long thread_saved_pc(struct task_struct *tsk);
void show_trace(struct task_struct *task, unsigned long *stack);

unsigned long get_wchan(struct task_struct *p);

#define THREAD_SIZE_LONGS      (THREAD_SIZE/sizeof(unsigned long))
#define KSTK_TOP(info)                                                 \
({                                                                     \
       unsigned long *__ptr = (unsigned long *)(info);                 \
       (unsigned long)(&__ptr[THREAD_SIZE_LONGS]);                     \
})

#define task_pt_regs(task)                                             \
({                                                                     \
       struct pt_regs *__regs__;                                       \
       __regs__ = (struct pt_regs *)KSTK_TOP((task)->thread_info);     \
       __regs__ - 1;                                                   \
})

#define KSTK_EIP(task) (task_pt_regs(task)->eip)
#define KSTK_ESP(task) (task_pt_regs(task)->esp)


struct microcode_header {
	unsigned int hdrver;
	unsigned int rev;
	unsigned int date;
	unsigned int sig;
	unsigned int cksum;
	unsigned int ldrver;
	unsigned int pf;
	unsigned int datasize;
	unsigned int totalsize;
	unsigned int reserved[3];
};

struct microcode {
	struct microcode_header hdr;
	unsigned int bits[0];
};

typedef struct microcode microcode_t;
typedef struct microcode_header microcode_header_t;

/* microcode format is extended from prescott processors */
struct extended_signature {
	unsigned int sig;
	unsigned int pf;
	unsigned int cksum;
};

struct extended_sigtable {
	unsigned int count;
	unsigned int cksum;
	unsigned int reserved[3];
	struct extended_signature sigs[0];
};
/* '6' because it used to be for P6 only (but now covers Pentium 4 as well) */
#define MICROCODE_IOCFREE	_IO('6',0)

/* REP NOP (PAUSE) is a good thing to insert into busy-wait loops. */
static inline void rep_nop(void)
{
	__asm__ __volatile__("rep;nop": : :"memory");
}

#define cpu_relax()	rep_nop()

/* generic versions from gas */
#define GENERIC_NOP1	".byte 0x90\n"
#define GENERIC_NOP2    	".byte 0x89,0xf6\n"
#define GENERIC_NOP3        ".byte 0x8d,0x76,0x00\n"
#define GENERIC_NOP4        ".byte 0x8d,0x74,0x26,0x00\n"
#define GENERIC_NOP5        GENERIC_NOP1 GENERIC_NOP4
#define GENERIC_NOP6	".byte 0x8d,0xb6,0x00,0x00,0x00,0x00\n"
#define GENERIC_NOP7	".byte 0x8d,0xb4,0x26,0x00,0x00,0x00,0x00\n"
#define GENERIC_NOP8	GENERIC_NOP1 GENERIC_NOP7

/* Opteron nops */
#define K8_NOP1 GENERIC_NOP1
#define K8_NOP2	".byte 0x66,0x90\n" 
#define K8_NOP3	".byte 0x66,0x66,0x90\n" 
#define K8_NOP4	".byte 0x66,0x66,0x66,0x90\n" 
#define K8_NOP5	K8_NOP3 K8_NOP2 
#define K8_NOP6	K8_NOP3 K8_NOP3
#define K8_NOP7	K8_NOP4 K8_NOP3
#define K8_NOP8	K8_NOP4 K8_NOP4

/* K7 nops */
/* uses eax dependencies (arbitary choice) */
#define K7_NOP1  GENERIC_NOP1
#define K7_NOP2	".byte 0x8b,0xc0\n" 
#define K7_NOP3	".byte 0x8d,0x04,0x20\n"
#define K7_NOP4	".byte 0x8d,0x44,0x20,0x00\n"
#define K7_NOP5	K7_NOP4 ASM_NOP1
#define K7_NOP6	".byte 0x8d,0x80,0,0,0,0\n"
#define K7_NOP7        ".byte 0x8D,0x04,0x05,0,0,0,0\n"
#define K7_NOP8        K7_NOP7 ASM_NOP1

#ifdef CONFIG_MK8
#define ASM_NOP1 K8_NOP1
#define ASM_NOP2 K8_NOP2
#define ASM_NOP3 K8_NOP3
#define ASM_NOP4 K8_NOP4
#define ASM_NOP5 K8_NOP5
#define ASM_NOP6 K8_NOP6
#define ASM_NOP7 K8_NOP7
#define ASM_NOP8 K8_NOP8
#elif defined(CONFIG_MK7)
#define ASM_NOP1 K7_NOP1
#define ASM_NOP2 K7_NOP2
#define ASM_NOP3 K7_NOP3
#define ASM_NOP4 K7_NOP4
#define ASM_NOP5 K7_NOP5
#define ASM_NOP6 K7_NOP6
#define ASM_NOP7 K7_NOP7
#define ASM_NOP8 K7_NOP8
#else
#define ASM_NOP1 GENERIC_NOP1
#define ASM_NOP2 GENERIC_NOP2
#define ASM_NOP3 GENERIC_NOP3
#define ASM_NOP4 GENERIC_NOP4
#define ASM_NOP5 GENERIC_NOP5
#define ASM_NOP6 GENERIC_NOP6
#define ASM_NOP7 GENERIC_NOP7
#define ASM_NOP8 GENERIC_NOP8
#endif

#define ASM_NOP_MAX 8

/* Prefetch instructions for Pentium III and AMD Athlon */
/* It's not worth to care about 3dnow! prefetches for the K6
   because they are microcoded there and very slow.
   However we don't do prefetches for pre XP Athlons currently
   That should be fixed. */
#define ARCH_HAS_PREFETCH
extern inline void prefetch(const void *x)
{
	alternative_input(ASM_NOP4,
			  "prefetchnta (%1)",
			  X86_FEATURE_XMM,
			  "r" (x));
}

#define ARCH_HAS_PREFETCH
#define ARCH_HAS_PREFETCHW
#define ARCH_HAS_SPINLOCK_PREFETCH

/* 3dnow! prefetch to get an exclusive cache line. Useful for 
   spinlocks to avoid one state transition in the cache coherency protocol. */
extern inline void prefetchw(const void *x)
{
	alternative_input(ASM_NOP4,
			  "prefetchw (%1)",
			  X86_FEATURE_3DNOW,
			  "r" (x));
}
#define spin_lock_prefetch(x)	prefetchw(x)

extern void select_idle_routine(const struct cpuinfo_x86 *c);

#define cache_line_size() (boot_cpu_data.x86_cache_alignment)

#ifdef CONFIG_SCHED_SMT
#define ARCH_HAS_SCHED_DOMAIN
#define ARCH_HAS_SCHED_WAKE_IDLE
#endif

#endif /* __ASM_I386_PROCESSOR_H */
