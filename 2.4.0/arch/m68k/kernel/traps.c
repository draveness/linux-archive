/*
 *  linux/arch/m68k/kernel/traps.c
 *
 *  Copyright (C) 1993, 1994 by Hamish Macdonald
 *
 *  68040 fixes by Michael Rausch
 *  68040 fixes by Martin Apel
 *  68040 fixes and writeback by Richard Zidlicky
 *  68060 fixes by Roman Hodek
 *  68060 fixes by Jesper Skov
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive
 * for more details.
 */

/*
 * Sets up all exception vectors
 */

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/a.out.h>
#include <linux/user.h>
#include <linux/string.h>
#include <linux/linkage.h>
#include <linux/init.h>

#include <asm/setup.h>
#include <asm/fpu.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/traps.h>
#include <asm/pgalloc.h>
#include <asm/machdep.h>
#include <asm/siginfo.h>

/* assembler routines */
asmlinkage void system_call(void);
asmlinkage void buserr(void);
asmlinkage void trap(void);
asmlinkage void inthandler(void);
asmlinkage void nmihandler(void);
#ifdef CONFIG_M68KFPU_EMU
asmlinkage void fpu_emu(void);
#endif

e_vector vectors[256] = {
	0, 0, buserr, trap, trap, trap, trap, trap,
	trap, trap, trap, trap, trap, trap, trap, trap,
	trap, trap, trap, trap, trap, trap, trap, trap,
	inthandler, inthandler, inthandler, inthandler,
	inthandler, inthandler, inthandler, inthandler,
	/* TRAP #0-15 */
	system_call, trap, trap, trap, trap, trap, trap, trap,
	trap, trap, trap, trap, trap, trap, trap, trap,
	0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

/* nmi handler for the Amiga */
asm(".text\n"
    __ALIGN_STR "\n"
    SYMBOL_NAME_STR(nmihandler) ": rte");

/*
 * this must be called very early as the kernel might
 * use some instruction that are emulated on the 060
 */
void __init base_trap_init(void)
{
	/* setup the exception vector table */
	__asm__ volatile ("movec %0,%%vbr" : : "r" ((void*)vectors));

	if (CPU_IS_060) {
		/* set up ISP entry points */
		asmlinkage void unimp_vec(void) asm ("_060_isp_unimp");

		vectors[VEC_UNIMPII] = unimp_vec;
	}
}

void __init trap_init (void)
{
	int i;

	for (i = 48; i < 64; i++)
		if (!vectors[i])
			vectors[i] = trap;

	for (i = 64; i < 256; i++)
		vectors[i] = inthandler;

#ifdef CONFIG_M68KFPU_EMU
	if (FPU_IS_EMU)
		vectors[VEC_LINE11] = fpu_emu;
#endif

	if (CPU_IS_040 && !FPU_IS_EMU) {
		/* set up FPSP entry points */
		asmlinkage void dz_vec(void) asm ("dz");
		asmlinkage void inex_vec(void) asm ("inex");
		asmlinkage void ovfl_vec(void) asm ("ovfl");
		asmlinkage void unfl_vec(void) asm ("unfl");
		asmlinkage void snan_vec(void) asm ("snan");
		asmlinkage void operr_vec(void) asm ("operr");
		asmlinkage void bsun_vec(void) asm ("bsun");
		asmlinkage void fline_vec(void) asm ("fline");
		asmlinkage void unsupp_vec(void) asm ("unsupp");

		vectors[VEC_FPDIVZ] = dz_vec;
		vectors[VEC_FPIR] = inex_vec;
		vectors[VEC_FPOVER] = ovfl_vec;
		vectors[VEC_FPUNDER] = unfl_vec;
		vectors[VEC_FPNAN] = snan_vec;
		vectors[VEC_FPOE] = operr_vec;
		vectors[VEC_FPBRUC] = bsun_vec;
		vectors[VEC_LINE11] = fline_vec;
		vectors[VEC_FPUNSUP] = unsupp_vec;
	}

	if (CPU_IS_060 && !FPU_IS_EMU) {
		/* set up IFPSP entry points */
		asmlinkage void snan_vec(void) asm ("_060_fpsp_snan");
		asmlinkage void operr_vec(void) asm ("_060_fpsp_operr");
		asmlinkage void ovfl_vec(void) asm ("_060_fpsp_ovfl");
		asmlinkage void unfl_vec(void) asm ("_060_fpsp_unfl");
		asmlinkage void dz_vec(void) asm ("_060_fpsp_dz");
		asmlinkage void inex_vec(void) asm ("_060_fpsp_inex");
		asmlinkage void fline_vec(void) asm ("_060_fpsp_fline");
		asmlinkage void unsupp_vec(void) asm ("_060_fpsp_unsupp");
		asmlinkage void effadd_vec(void) asm ("_060_fpsp_effadd");

		vectors[VEC_FPNAN] = snan_vec;
		vectors[VEC_FPOE] = operr_vec;
		vectors[VEC_FPOVER] = ovfl_vec;
		vectors[VEC_FPUNDER] = unfl_vec;
		vectors[VEC_FPDIVZ] = dz_vec;
		vectors[VEC_FPIR] = inex_vec;
		vectors[VEC_LINE11] = fline_vec;
		vectors[VEC_FPUNSUP] = unsupp_vec;
		vectors[VEC_UNIMPEA] = effadd_vec;
	}

        /* if running on an amiga, make the NMI interrupt do nothing */
	if (MACH_IS_AMIGA) {
		vectors[VEC_INT7] = nmihandler;
	}
}


static char *vec_names[] = {
	"RESET SP", "RESET PC", "BUS ERROR", "ADDRESS ERROR",
	"ILLEGAL INSTRUCTION", "ZERO DIVIDE", "CHK", "TRAPcc",
	"PRIVILEGE VIOLATION", "TRACE", "LINE 1010", "LINE 1111",
	"UNASSIGNED RESERVED 12", "COPROCESSOR PROTOCOL VIOLATION",
	"FORMAT ERROR", "UNINITIALIZED INTERRUPT",
	"UNASSIGNED RESERVED 16", "UNASSIGNED RESERVED 17",
	"UNASSIGNED RESERVED 18", "UNASSIGNED RESERVED 19",
	"UNASSIGNED RESERVED 20", "UNASSIGNED RESERVED 21",
	"UNASSIGNED RESERVED 22", "UNASSIGNED RESERVED 23",
	"SPURIOUS INTERRUPT", "LEVEL 1 INT", "LEVEL 2 INT", "LEVEL 3 INT",
	"LEVEL 4 INT", "LEVEL 5 INT", "LEVEL 6 INT", "LEVEL 7 INT",
	"SYSCALL", "TRAP #1", "TRAP #2", "TRAP #3",
	"TRAP #4", "TRAP #5", "TRAP #6", "TRAP #7",
	"TRAP #8", "TRAP #9", "TRAP #10", "TRAP #11",
	"TRAP #12", "TRAP #13", "TRAP #14", "TRAP #15",
	"FPCP BSUN", "FPCP INEXACT", "FPCP DIV BY 0", "FPCP UNDERFLOW",
	"FPCP OPERAND ERROR", "FPCP OVERFLOW", "FPCP SNAN",
	"FPCP UNSUPPORTED OPERATION",
	"MMU CONFIGURATION ERROR"
	};

#ifndef CONFIG_SUN3
static char *space_names[] = {
	"Space 0", "User Data", "User Program", "Space 3",
	"Space 4", "Super Data", "Super Program", "CPU"
	};
#else
static char *space_names[] = {
	"Space 0", "User Data", "User Program", "Control",
	"Space 4", "Super Data", "Super Program", "CPU"
	};
#endif

void die_if_kernel(char *,struct pt_regs *,int);
asmlinkage int do_page_fault(struct pt_regs *regs, unsigned long address,
                             unsigned long error_code);
int send_fault_sig(struct pt_regs *regs);

asmlinkage void trap_c(struct frame *fp);

#if defined (CONFIG_M68060)
static inline void access_error060 (struct frame *fp)
{
	unsigned long fslw = fp->un.fmt4.pc; /* is really FSLW for access error */

#ifdef DEBUG
	printk("fslw=%#lx, fa=%#lx\n", fslw, fp->un.fmt4.effaddr);
#endif

	if (fslw & MMU060_BPE) {
		/* branch prediction error -> clear branch cache */
		__asm__ __volatile__ ("movec %/cacr,%/d0\n\t"
				      "orl   #0x00400000,%/d0\n\t"
				      "movec %/d0,%/cacr"
				      : : : "d0" );
		/* return if there's no other error */
		if (!(fslw & MMU060_ERR_BITS) && !(fslw & MMU060_SEE))
			return;
	}

	if (fslw & (MMU060_DESC_ERR | MMU060_WP | MMU060_SP)) {
		unsigned long errorcode;
		unsigned long addr = fp->un.fmt4.effaddr;

		if (fslw & MMU060_MA)
			addr = (addr + 7) & -8;

		errorcode = 1;
		if (fslw & MMU060_DESC_ERR) {
			__flush_tlb040_one(addr);
			errorcode = 0;
		}
		if (fslw & MMU060_W)
			errorcode |= 2;
#ifdef DEBUG
		printk("errorcode = %d\n", errorcode );
#endif
		do_page_fault(&fp->ptregs, addr, errorcode);
	} else if (fslw & (MMU060_SEE)){
		/* Software Emulation Error.
		 * fault during mem_read/mem_write in ifpsp060/os.S
		 */
		send_fault_sig(&fp->ptregs);
	} else {
		printk("pc=%#lx, fa=%#lx\n", fp->ptregs.pc, fp->un.fmt4.effaddr);
		printk( "68060 access error, fslw=%lx\n", fslw );
		trap_c( fp );
	}
}
#endif /* CONFIG_M68060 */

#if defined (CONFIG_M68040)
static inline unsigned long probe040(int iswrite, unsigned long addr)
{
	unsigned long mmusr;

	asm volatile (".chip 68040");

	if (iswrite)
		asm volatile ("ptestw (%0)" : : "a" (addr));
	else
		asm volatile ("ptestr (%0)" : : "a" (addr));

	asm volatile ("movec %%mmusr,%0" : "=r" (mmusr));

	asm volatile (".chip 68k");

	return mmusr;
}

static inline int do_040writeback1(unsigned short wbs, unsigned long wba,
				   unsigned long wbd)
{
	int res = 0;

	set_fs(MAKE_MM_SEG(wbs));

	switch (wbs & WBSIZ_040) {
	case BA_SIZE_BYTE:
		res = put_user(wbd & 0xff, (char *)wba);
		break;
	case BA_SIZE_WORD:
		res = put_user(wbd & 0xffff, (short *)wba);
		break;
	case BA_SIZE_LONG:
		res = put_user(wbd, (int *)wba);
		break;
	}

#ifdef DEBUG
	printk("do_040writeback1, res=%d\n",res);
#endif

	return res;
}

/* after an exception in a writeback the stack frame coresponding
 * to that exception is discarded, set a few bits in the old frame 
 * to simulate what it should look like
 */
static inline void fix_xframe040(struct frame *fp, unsigned short wbs)
{
	fp->un.fmt7.faddr = current->thread.faddr;
	fp->un.fmt7.ssw = wbs & 0xff;
}

static inline void do_040writebacks(struct frame *fp)
{
	int res = 0;
#if 0
	if (fp->un.fmt7.wb1s & WBV_040)
		printk("access_error040: cannot handle 1st writeback. oops.\n");
#endif

	if ((fp->un.fmt7.wb2s & WBV_040) &&
	    !(fp->un.fmt7.wb2s & WBTT_040)) {
		res = do_040writeback1(fp->un.fmt7.wb2s, fp->un.fmt7.wb2a,
				       fp->un.fmt7.wb2d);
		if (res)
			fix_xframe040(fp, fp->un.fmt7.wb2s);
		else 
			fp->un.fmt7.wb2s = 0;
	}

	/* do the 2nd wb only if the first one was succesful (except for a kernel wb) */
	if (fp->un.fmt7.wb3s & WBV_040 && (!res || fp->un.fmt7.wb3s & 4)) {
		res = do_040writeback1(fp->un.fmt7.wb3s, fp->un.fmt7.wb3a,
				       fp->un.fmt7.wb3d);
		if (res)
			fix_xframe040(fp, fp->un.fmt7.wb3s);
		else
			fp->un.fmt7.wb3s = 0;
	}

	if (res)
		send_fault_sig(&fp->ptregs);
}

/*
 * called from sigreturn(), must ensure userspace code didn't
 * manipulate exception frame to circumvent protection, then complete
 * pending writebacks
 * we just clear TM2 to turn it into an userspace access
 */
asmlinkage void berr_040cleanup(struct frame *fp)
{
	mm_segment_t old_fs = get_fs();

	fp->un.fmt7.wb2s &= ~4;
	fp->un.fmt7.wb3s &= ~4;

	do_040writebacks(fp);
	set_fs(old_fs);
}

static inline void access_error040(struct frame *fp)
{
	unsigned short ssw = fp->un.fmt7.ssw;
	mm_segment_t old_fs = get_fs();
	unsigned long mmusr;

#ifdef DEBUG
	printk("ssw=%#x, fa=%#lx\n", ssw, fp->un.fmt7.faddr);
        printk("wb1s=%#x, wb2s=%#x, wb3s=%#x\n", fp->un.fmt7.wb1s,  
		fp->un.fmt7.wb2s, fp->un.fmt7.wb3s);
	printk ("wb2a=%lx, wb3a=%lx, wb2d=%lx, wb3d=%lx\n", 
		fp->un.fmt7.wb2a, fp->un.fmt7.wb3a,
		fp->un.fmt7.wb2d, fp->un.fmt7.wb3d);
#endif

	if (ssw & ATC_040) {
		unsigned long addr = fp->un.fmt7.faddr;
		unsigned long errorcode;

		/*
		 * The MMU status has to be determined AFTER the address
		 * has been corrected if there was a misaligned access (MA).
		 */
		if (ssw & MA_040)
			addr = (addr + 7) & -8;

		set_fs(MAKE_MM_SEG(ssw));
		/* MMU error, get the MMUSR info for this access */
		mmusr = probe040(!(ssw & RW_040), addr);
#ifdef DEBUG
		printk("mmusr = %lx\n", mmusr);
#endif
		errorcode = 1;
		if (!(mmusr & MMU_R_040)) {
			/* clear the invalid atc entry */
			__flush_tlb040_one(addr);
			errorcode = 0;
		}
		if (!(ssw & RW_040))
			errorcode |= 2;
		if (do_page_fault(&fp->ptregs, addr, errorcode)) {
#ifdef DEBUG
		        printk("do_page_fault() !=0 \n");
#endif
			if (user_mode(&fp->ptregs)){
				/* delay writebacks after signal delivery */
#ifdef DEBUG
			        printk(".. was usermode - return\n");
#endif
				return;
			}
			/* disable writeback into user space from kernel
			 * (if do_page_fault didn't fix the mapping,
                         * the writeback won't do good)
			 */
#ifdef DEBUG
			printk(".. disabling wb2\n");
#endif
			if (fp->un.fmt7.wb2a == fp->un.fmt7.faddr)
				fp->un.fmt7.wb2s &= ~WBV_040;
		}
	} else {
		printk("68040 access error, ssw=%x\n", ssw);
		trap_c(fp);
	}

	do_040writebacks(fp);
	set_fs(old_fs);
}
#endif /* CONFIG_M68040 */

#if defined(CONFIG_SUN3)
#include <asm/sun3mmu.h>

extern int mmu_emu_handle_fault (unsigned long, int, int);

/* sun3 version of bus_error030 */

extern inline void bus_error030 (struct frame *fp)
{
	unsigned char buserr_type = sun3_get_buserr ();
	unsigned long addr, errorcode;
	unsigned short ssw = fp->un.fmtb.ssw;

#if DEBUG
	if (ssw & (FC | FB))
		printk ("Instruction fault at %#010lx\n",
			ssw & FC ?
			fp->ptregs.format == 0xa ? fp->ptregs.pc + 2 : fp->un.fmtb.baddr - 2
			:
			fp->ptregs.format == 0xa ? fp->ptregs.pc + 4 : fp->un.fmtb.baddr);
	if (ssw & DF) 
		printk ("Data %s fault at %#010lx in %s (pc=%#lx)\n",
			ssw & RW ? "read" : "write",
			fp->un.fmtb.daddr,
			space_names[ssw & DFC], fp->ptregs.pc);
#endif

	/*
	 * Check if this page should be demand-mapped. This needs to go before
	 * the testing for a bad kernel-space access (demand-mapping applies
	 * to kernel accesses too).
	 */
	
	if ((ssw & DF)
	    && (buserr_type & (SUN3_BUSERR_PROTERR | SUN3_BUSERR_INVALID))) {
		if (mmu_emu_handle_fault (fp->un.fmtb.daddr, ssw & RW, 0))
			return;
	}

	/* Check for kernel-space pagefault (BAD). */
	if (fp->ptregs.sr & PS_S) {
		/* kernel fault must be a data fault to user space */
		if (! ((ssw & DF) && ((ssw & DFC) == USER_DATA))) {
		     // try checking the kernel mappings before surrender
		     if (mmu_emu_handle_fault (fp->un.fmtb.daddr, ssw & RW, 1))
			  return;
			/* instruction fault or kernel data fault! */
			if (ssw & (FC | FB))
				printk ("Instruction fault at %#010lx\n",
					fp->ptregs.pc);
			if (ssw & DF) {
				printk ("Data %s fault at %#010lx in %s (pc=%#lx)\n",
					ssw & RW ? "read" : "write",
					fp->un.fmtb.daddr,
					space_names[ssw & DFC], fp->ptregs.pc);
			}
			printk ("BAD KERNEL BUSERR\n");

			die_if_kernel("Oops", &fp->ptregs,0);
			force_sig(SIGKILL, current);
			return;
		}
	} else {
		/* user fault */
		if (!(ssw & (FC | FB)) && !(ssw & DF))
			/* not an instruction fault or data fault! BAD */
			panic ("USER BUSERR w/o instruction or data fault");
	}


	/* First handle the data fault, if any.  */
	if (ssw & DF) {
		addr = fp->un.fmtb.daddr;

// errorcode bit 0:	0 -> no page		1 -> protection fault
// errorcode bit 1:	0 -> read fault		1 -> write fault

// (buserr_type & SUN3_BUSERR_PROTERR)	-> protection fault
// (buserr_type & SUN3_BUSERR_INVALID)	-> invalid page fault

		if (buserr_type & SUN3_BUSERR_PROTERR)
			errorcode = 0x01;
		else if (buserr_type & SUN3_BUSERR_INVALID)
			errorcode = 0x00;
		else {
#ifdef DEBUG
			printk ("*** unexpected busfault type=%#04x\n", buserr_type);
			printk ("invalid %s access at %#lx from pc %#lx\n",
				!(ssw & RW) ? "write" : "read", addr,
				fp->ptregs.pc);
#endif
			die_if_kernel ("Oops", &fp->ptregs, buserr_type);
			force_sig (SIGBUS, current);
			return;
		}

//todo: wtf is RM bit? --m
		if (!(ssw & RW) || ssw & RM)
			errorcode |= 0x02;

		/* Handle page fault. */
		do_page_fault (&fp->ptregs, addr, errorcode);

		/* Retry the data fault now. */
		return;
	}

	/* Now handle the instruction fault. */

	/* Get the fault address. */
	if (fp->ptregs.format == 0xA)
		addr = fp->ptregs.pc + 4;
	else
		addr = fp->un.fmtb.baddr;
	if (ssw & FC)
		addr -= 2;

	if (buserr_type & SUN3_BUSERR_INVALID) {
		if (!mmu_emu_handle_fault (fp->un.fmtb.daddr, 1, 0))
			do_page_fault (&fp->ptregs, addr, 0);
       } else {
#ifdef DEBUG
		printk ("protection fault on insn access (segv).\n");
#endif
		force_sig (SIGSEGV, current);
       }	
}
#else
#if defined(CPU_M68020_OR_M68030)
static inline void bus_error030 (struct frame *fp)
{
	volatile unsigned short temp;
	unsigned short mmusr;
	unsigned long addr, errorcode;
	unsigned short ssw = fp->un.fmtb.ssw;
	int user_space_fault = 1;
#if DEBUG
	unsigned long desc;
#endif

#if DEBUG
	printk ("pid = %x  ", current->pid);
	printk ("SSW=%#06x  ", ssw);

	if (ssw & (FC | FB))
		printk ("Instruction fault at %#010lx\n",
			ssw & FC ?
			fp->ptregs.format == 0xa ? fp->ptregs.pc + 2 : fp->un.fmtb.baddr - 2
			:
			fp->ptregs.format == 0xa ? fp->ptregs.pc + 4 : fp->un.fmtb.baddr);
	if (ssw & DF)
		printk ("Data %s fault at %#010lx in %s (pc=%#lx)\n",
			ssw & RW ? "read" : "write",
			fp->un.fmtb.daddr,
			space_names[ssw & DFC], fp->ptregs.pc);
#endif

	if (fp->ptregs.sr & PS_S) {
		/* kernel fault must be a data fault to user space */
		if (! ((ssw & DF) && ((ssw & DFC) == USER_DATA))) {
			/* instruction fault or kernel data fault! */
			if (ssw & (FC | FB))
				printk ("Instruction fault at %#010lx\n",
					fp->ptregs.pc);
			if (ssw & DF) {
				printk ("Data %s fault at %#010lx in %s (pc=%#lx)\n",
					ssw & RW ? "read" : "write",
					fp->un.fmtb.daddr,
					space_names[ssw & DFC], fp->ptregs.pc);
			}
			printk ("BAD KERNEL BUSERR\n");
			die_if_kernel("Oops",&fp->ptregs,0);
			force_sig(SIGKILL, current);
			return;
		}
	} else {
		/* user fault */
		if (!(ssw & (FC | FB)) && !(ssw & DF))
			/* not an instruction fault or data fault! BAD */
			panic ("USER BUSERR w/o instruction or data fault");
		user_space_fault = 1;
#if DEBUG
		printk("User space bus-error\n");
#endif
	}

	/* ++andreas: If a data fault and an instruction fault happen
	   at the same time map in both pages.  */

	/* First handle the data fault, if any.  */
	if (ssw & DF)
	  {
	    addr = fp->un.fmtb.daddr;

	    mmusr = MMU_I;
	    if (user_space_fault) {
#if DEBUG
		    asm volatile ("ptestr #1,%2@,#7,%0\n\t"
				  "pmove %/psr,%1@"
				  : "=a&" (desc)
				  : "a" (&temp), "a" (addr));
#else
		    asm volatile ("ptestr #1,%1@,#7\n\t"
				  "pmove %/psr,%0@"
				  : : "a" (&temp), "a" (addr));
#endif
		    mmusr = temp;
	    }
      
#if DEBUG
	    printk ("mmusr is %#x for addr %#lx in task %p\n",
		    mmusr, addr, current);
	    printk ("descriptor address is %#lx, contents %#lx\n",
		    __va(desc), *(unsigned long *)__va(desc));
#endif

	    errorcode = (mmusr & MMU_I) ? 0 : 1;
	    if (!(ssw & RW) || (ssw & RM))
		    errorcode |= 2;

	    if (mmusr & (MMU_I | MMU_WP)) {
		/* Don't try to do anything further if an exception was
		   handled. */
		if (do_page_fault (&fp->ptregs, addr, errorcode) < 0)
			return;
	    } else if (mmusr & (MMU_B|MMU_L|MMU_S)) {
		    printk ("invalid %s access at %#lx from pc %#lx\n",
			    !(ssw & RW) ? "write" : "read", addr,
			    fp->ptregs.pc);
		    die_if_kernel("Oops",&fp->ptregs,mmusr);
		    force_sig(SIGSEGV, current);
		    return;
	    } else {
#if 0
		    static volatile long tlong;
#endif

		    printk ("weird %s access at %#lx from pc %#lx (ssw is %#x)\n",
			    !(ssw & RW) ? "write" : "read", addr,
			    fp->ptregs.pc, ssw);
		    asm volatile ("ptestr #1,%1@,#0\n\t"
				  "pmove %/psr,%0@"
				  : /* no outputs */
				  : "a" (&temp), "a" (addr));
		    mmusr = temp;

		    printk ("level 0 mmusr is %#x\n", mmusr);
#if 0
		    asm volatile ("pmove %/tt0,%0@"
				  : /* no outputs */
				  : "a" (&tlong));
		    printk ("tt0 is %#lx, ", tlong);
		    asm volatile ("pmove %/tt1,%0@"
				  : /* no outputs */
				  : "a" (&tlong));
		    printk ("tt1 is %#lx\n", tlong);
#endif
#if DEBUG
		    printk("Unknown SIGSEGV - 1\n");
#endif
		    die_if_kernel("Oops",&fp->ptregs,mmusr);
		    force_sig(SIGSEGV, current);
		    return;
	    }

	    /* setup an ATC entry for the access about to be retried */
	    if (!(ssw & RW))
		    asm volatile ("ploadw %1,%0@" : /* no outputs */
				  : "a" (addr), "d" (ssw));
	    else
		    asm volatile ("ploadr %1,%0@" : /* no outputs */
				  : "a" (addr), "d" (ssw));
	  }

	/* Now handle the instruction fault. */

	if (!(ssw & (FC|FB)))
		return;

	/* get the fault address */
	if (fp->ptregs.format == 10)
		addr = fp->ptregs.pc + 4;
	else
		addr = fp->un.fmtb.baddr;
	if (ssw & FC)
		addr -= 2;

	if ((ssw & DF) && ((addr ^ fp->un.fmtb.daddr) & PAGE_MASK) == 0)
		/* Insn fault on same page as data fault.  But we
		   should still create the ATC entry.  */
		goto create_atc_entry;

	mmusr = MMU_I;
	if (user_space_fault) {
#if DEBUG
		asm volatile ("ptestr #1,%2@,#7,%0\n\t"
			      "pmove %/psr,%1@"
			      : "=a&" (desc)
			      : "a" (&temp), "a" (addr));
#else
		asm volatile ("ptestr #1,%1@,#7\n\t"
			      "pmove %/psr,%0@"
			      : : "a" (&temp), "a" (addr));
#endif
		mmusr = temp;
	}
      
#ifdef DEBUG
	printk ("mmusr is %#x for addr %#lx in task %p\n",
		mmusr, addr, current);
	printk ("descriptor address is %#lx, contents %#lx\n",
		__va(desc), *(unsigned long *)__va(desc));
#endif

	if (mmusr & MMU_I)
		do_page_fault (&fp->ptregs, addr, 0);
	else if (mmusr & (MMU_B|MMU_L|MMU_S)) {
		printk ("invalid insn access at %#lx from pc %#lx\n",
			addr, fp->ptregs.pc);
#if DEBUG
		printk("Unknown SIGSEGV - 2\n");
#endif
		die_if_kernel("Oops",&fp->ptregs,mmusr);
		force_sig(SIGSEGV, current);
		return;
	}

create_atc_entry:
	/* setup an ATC entry for the access about to be retried */
	asm volatile ("ploadr #2,%0@" : /* no outputs */
		      : "a" (addr));
}
#endif /* CPU_M68020_OR_M68030 */
#endif /* !CONFIG_SUN3 */

asmlinkage void buserr_c(struct frame *fp)
{
	/* Only set esp0 if coming from user mode */
	if (user_mode(&fp->ptregs))
		current->thread.esp0 = (unsigned long) fp;

#if DEBUG
	printk ("*** Bus Error *** Format is %x\n", fp->ptregs.format);
#endif

	switch (fp->ptregs.format) {
#if defined (CONFIG_M68060)
	case 4:				/* 68060 access error */
	  access_error060 (fp);
	  break;
#endif
#if defined (CONFIG_M68040)
	case 0x7:			/* 68040 access error */
	  access_error040 (fp);
	  break;
#endif
#if defined (CPU_M68020_OR_M68030)
	case 0xa:
	case 0xb:
	  bus_error030 (fp);
	  break;
#endif
	default:
	  die_if_kernel("bad frame format",&fp->ptregs,0);
#if DEBUG
	  printk("Unknown SIGSEGV - 4\n");
#endif
	  force_sig(SIGSEGV, current);
	}
}


int kstack_depth_to_print = 48;

/* MODULE_RANGE is a guess of how much space is likely to be
   vmalloced.  */
#define MODULE_RANGE (8*1024*1024)

static void dump_stack(struct frame *fp)
{
	unsigned long *stack, *endstack, addr, module_start, module_end;
	extern char _start, _etext;
	int i;

	addr = (unsigned long)&fp->un;
	printk("Frame format=%X ", fp->ptregs.format);
	switch (fp->ptregs.format) {
	case 0x2:
	    printk("instr addr=%08lx\n", fp->un.fmt2.iaddr);
	    addr += sizeof(fp->un.fmt2);
	    break;
	case 0x3:
	    printk("eff addr=%08lx\n", fp->un.fmt3.effaddr);
	    addr += sizeof(fp->un.fmt3);
	    break;
	case 0x4:
	    printk((CPU_IS_060 ? "fault addr=%08lx fslw=%08lx\n"
		    : "eff addr=%08lx pc=%08lx\n"),
		   fp->un.fmt4.effaddr, fp->un.fmt4.pc);
	    addr += sizeof(fp->un.fmt4);
	    break;
	case 0x7:
	    printk("eff addr=%08lx ssw=%04x faddr=%08lx\n",
		   fp->un.fmt7.effaddr, fp->un.fmt7.ssw, fp->un.fmt7.faddr);
	    printk("wb 1 stat/addr/data: %04x %08lx %08lx\n",
		   fp->un.fmt7.wb1s, fp->un.fmt7.wb1a, fp->un.fmt7.wb1dpd0);
	    printk("wb 2 stat/addr/data: %04x %08lx %08lx\n",
		   fp->un.fmt7.wb2s, fp->un.fmt7.wb2a, fp->un.fmt7.wb2d);
	    printk("wb 3 stat/addr/data: %04x %08lx %08lx\n",
		   fp->un.fmt7.wb3s, fp->un.fmt7.wb3a, fp->un.fmt7.wb3d);
	    printk("push data: %08lx %08lx %08lx %08lx\n",
		   fp->un.fmt7.wb1dpd0, fp->un.fmt7.pd1, fp->un.fmt7.pd2,
		   fp->un.fmt7.pd3);
	    addr += sizeof(fp->un.fmt7);
	    break;
	case 0x9:
	    printk("instr addr=%08lx\n", fp->un.fmt9.iaddr);
	    addr += sizeof(fp->un.fmt9);
	    break;
	case 0xa:
	    printk("ssw=%04x isc=%04x isb=%04x daddr=%08lx dobuf=%08lx\n",
		   fp->un.fmta.ssw, fp->un.fmta.isc, fp->un.fmta.isb,
		   fp->un.fmta.daddr, fp->un.fmta.dobuf);
	    addr += sizeof(fp->un.fmta);
	    break;
	case 0xb:
	    printk("ssw=%04x isc=%04x isb=%04x daddr=%08lx dobuf=%08lx\n",
		   fp->un.fmtb.ssw, fp->un.fmtb.isc, fp->un.fmtb.isb,
		   fp->un.fmtb.daddr, fp->un.fmtb.dobuf);
	    printk("baddr=%08lx dibuf=%08lx ver=%x\n",
		   fp->un.fmtb.baddr, fp->un.fmtb.dibuf, fp->un.fmtb.ver);
	    addr += sizeof(fp->un.fmtb);
	    break;
	default:
	    printk("\n");
	}

	stack = (unsigned long *)addr;
	endstack = (unsigned long *)PAGE_ALIGN(addr);

	printk("Stack from %08lx:", (unsigned long)stack);
	for (i = 0; i < kstack_depth_to_print; i++) {
		if (stack + 1 > endstack)
			break;
		if (i % 8 == 0)
			printk("\n       ");
		printk(" %08lx", *stack++);
	}

	printk ("\nCall Trace:");
	stack = (unsigned long *) addr;
	i = 0;
	module_start = VMALLOC_START;
	module_end = module_start + MODULE_RANGE;
	while (stack + 1 <= endstack) {
		addr = *stack++;
		/*
		 * If the address is either in the text segment of the
		 * kernel, or in the region which contains vmalloc'ed
		 * memory, it *may* be the address of a calling
		 * routine; if so, print it so that someone tracing
		 * down the cause of the crash will be able to figure
		 * out the call path that was taken.
		 */
		if (((addr >= (unsigned long) &_start) &&
		     (addr <= (unsigned long) &_etext)) ||
		    ((addr >= module_start) && (addr <= module_end))) {
			if (i % 4 == 0)
				printk("\n       ");
			printk(" [<%08lx>]", addr);
			i++;
		}
	}
	printk("\nCode: ");
	for (i = 0; i < 10; i++)
		printk("%04x ", 0xffff & ((short *) fp->ptregs.pc)[i]);
	printk ("\n");
}

void bad_super_trap (struct frame *fp)
{
	console_verbose();
	if (fp->ptregs.vector < 4*sizeof(vec_names)/sizeof(vec_names[0]))
		printk ("*** %s ***   FORMAT=%X\n",
			vec_names[(fp->ptregs.vector) >> 2],
			fp->ptregs.format);
	else
		printk ("*** Exception %d ***   FORMAT=%X\n",
			(fp->ptregs.vector) >> 2, 
			fp->ptregs.format);
	if (fp->ptregs.vector >> 2 == VEC_ADDRERR && CPU_IS_020_OR_030) {
		unsigned short ssw = fp->un.fmtb.ssw;

		printk ("SSW=%#06x  ", ssw);

		if (ssw & RC)
			printk ("Pipe stage C instruction fault at %#010lx\n",
				(fp->ptregs.format) == 0xA ?
				fp->ptregs.pc + 2 : fp->un.fmtb.baddr - 2);
		if (ssw & RB)
			printk ("Pipe stage B instruction fault at %#010lx\n",
				(fp->ptregs.format) == 0xA ?
				fp->ptregs.pc + 4 : fp->un.fmtb.baddr);
		if (ssw & DF)
			printk ("Data %s fault at %#010lx in %s (pc=%#lx)\n",
				ssw & RW ? "read" : "write",
				fp->un.fmtb.daddr, space_names[ssw & DFC],
				fp->ptregs.pc);
	}
	printk ("Current process id is %d\n", current->pid);
	die_if_kernel("BAD KERNEL TRAP", &fp->ptregs, 0);
}

asmlinkage void trap_c(struct frame *fp)
{
	int sig;
	siginfo_t info;

	if (fp->ptregs.sr & PS_S) {
		if ((fp->ptregs.vector >> 2) == VEC_TRACE) {
			/* traced a trapping instruction */
			current->ptrace |= PT_DTRACE;
		} else
			bad_super_trap(fp);
		return;
	}

	/* send the appropriate signal to the user program */
	switch ((fp->ptregs.vector) >> 2) {
	    case VEC_ADDRERR:
		info.si_code = BUS_ADRALN;
		sig = SIGBUS;
		break;
	    case VEC_ILLEGAL:
	    case VEC_LINE10:
	    case VEC_LINE11:
		info.si_code = ILL_ILLOPC;
		sig = SIGILL;
		break;
	    case VEC_PRIV:
		info.si_code = ILL_PRVOPC;
		sig = SIGILL;
		break;
	    case VEC_COPROC:
		info.si_code = ILL_COPROC;
		sig = SIGILL;
		break;
	    case VEC_TRAP1:
	    case VEC_TRAP2:
	    case VEC_TRAP3:
	    case VEC_TRAP4:
	    case VEC_TRAP5:
	    case VEC_TRAP6:
	    case VEC_TRAP7:
	    case VEC_TRAP8:
	    case VEC_TRAP9:
	    case VEC_TRAP10:
	    case VEC_TRAP11:
	    case VEC_TRAP12:
	    case VEC_TRAP13:
	    case VEC_TRAP14:
		info.si_code = ILL_ILLTRP;
		sig = SIGILL;
		break;
	    case VEC_FPBRUC:
	    case VEC_FPOE:
	    case VEC_FPNAN:
		info.si_code = FPE_FLTINV;
		sig = SIGFPE;
		break;
	    case VEC_FPIR:
		info.si_code = FPE_FLTRES;
		sig = SIGFPE;
		break;
	    case VEC_FPDIVZ:
		info.si_code = FPE_FLTDIV;
		sig = SIGFPE;
		break;
	    case VEC_FPUNDER:
		info.si_code = FPE_FLTUND;
		sig = SIGFPE;
		break;
	    case VEC_FPOVER:
		info.si_code = FPE_FLTOVF;
		sig = SIGFPE;
		break;
	    case VEC_ZERODIV:
		info.si_code = FPE_INTDIV;
		sig = SIGFPE;
		break;
	    case VEC_CHK:
	    case VEC_TRAP:
		info.si_code = FPE_INTOVF;
		sig = SIGFPE;
		break;
	    case VEC_TRACE:		/* ptrace single step */
		info.si_code = TRAP_TRACE;
		sig = SIGTRAP;
		break;
	    case VEC_TRAP15:		/* breakpoint */
		info.si_code = TRAP_BRKPT;
		sig = SIGTRAP;
		break;
	    default:
		info.si_code = ILL_ILLOPC;
		sig = SIGILL;
		break;
	}
	info.si_signo = sig;
	info.si_errno = 0;
	switch (fp->ptregs.format) {
	    default:
		info.si_addr = (void *) fp->ptregs.pc;
		break;
	    case 2:
		info.si_addr = (void *) fp->un.fmt2.iaddr;
		break;
	    case 7:
		info.si_addr = (void *) fp->un.fmt7.effaddr;
		break;
	    case 9:
		info.si_addr = (void *) fp->un.fmt9.iaddr;
		break;
	    case 10:
		info.si_addr = (void *) fp->un.fmta.daddr;
		break;
	    case 11:
		info.si_addr = (void *) fp->un.fmtb.daddr;
		break;
	}
	force_sig_info (sig, &info, current);
}

void die_if_kernel (char *str, struct pt_regs *fp, int nr)
{
	if (!(fp->sr & PS_S))
		return;

	console_verbose();
	printk("%s: %08x\n",str,nr);
	printk("PC: [<%08lx>]\nSR: %04x  SP: %p  a2: %08lx\n",
	       fp->pc, fp->sr, fp, fp->a2);
	printk("d0: %08lx    d1: %08lx    d2: %08lx    d3: %08lx\n",
	       fp->d0, fp->d1, fp->d2, fp->d3);
	printk("d4: %08lx    d5: %08lx    a0: %08lx    a1: %08lx\n",
	       fp->d4, fp->d5, fp->a0, fp->a1);

	printk("Process %s (pid: %d, stackpage=%08lx)\n",
		current->comm, current->pid, PAGE_SIZE+(unsigned long)current);
	dump_stack((struct frame *)fp);
	do_exit(SIGSEGV);
}

/*
 * This function is called if an error occur while accessing
 * user-space from the fpsp040 code.
 */
asmlinkage void fpsp040_die(void)
{
	do_exit(SIGSEGV);
}

#ifdef CONFIG_M68KFPU_EMU
asmlinkage void fpemu_signal(int signal, int code, void *addr)
{
	siginfo_t info;

	info.si_signo = signal;
	info.si_errno = 0;
	info.si_code = code;
	info.si_addr = addr;
	force_sig_info(signal, &info, current);
}
#endif
