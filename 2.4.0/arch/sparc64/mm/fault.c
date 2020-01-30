/* $Id: fault.c,v 1.51 2000/09/14 06:22:32 anton Exp $
 * arch/sparc64/mm/fault.c: Page fault handlers for the 64-bit Sparc.
 *
 * Copyright (C) 1996 David S. Miller (davem@caip.rutgers.edu)
 * Copyright (C) 1997, 1999 Jakub Jelinek (jj@ultra.linux.cz)
 */

#include <asm/head.h>

#include <linux/string.h>
#include <linux/types.h>
#include <linux/ptrace.h>
#include <linux/mman.h>
#include <linux/signal.h>
#include <linux/mm.h>
#include <linux/smp_lock.h>
#include <linux/init.h>
#include <linux/interrupt.h>

#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/openprom.h>
#include <asm/oplib.h>
#include <asm/uaccess.h>

#define ELEMENTS(arr) (sizeof (arr)/sizeof (arr[0]))

extern struct sparc_phys_banks sp_banks[SPARC_PHYS_BANKS];

/* Nice, simple, prom library does all the sweating for us. ;) */
unsigned long __init prom_probe_memory (void)
{
	register struct linux_mlist_p1275 *mlist;
	register unsigned long bytes, base_paddr, tally;
	register int i;

	i = 0;
	mlist = *prom_meminfo()->p1275_available;
	bytes = tally = mlist->num_bytes;
	base_paddr = mlist->start_adr;
  
	sp_banks[0].base_addr = base_paddr;
	sp_banks[0].num_bytes = bytes;

	while (mlist->theres_more != (void *) 0) {
		i++;
		mlist = mlist->theres_more;
		bytes = mlist->num_bytes;
		tally += bytes;
		if (i >= SPARC_PHYS_BANKS-1) {
			printk ("The machine has more banks than "
				"this kernel can support\n"
				"Increase the SPARC_PHYS_BANKS "
				"setting (currently %d)\n",
				SPARC_PHYS_BANKS);
			i = SPARC_PHYS_BANKS-1;
			break;
		}
    
		sp_banks[i].base_addr = mlist->start_adr;
		sp_banks[i].num_bytes = mlist->num_bytes;
	}

	i++;
	sp_banks[i].base_addr = 0xdeadbeefbeefdeadUL;
	sp_banks[i].num_bytes = 0;

	/* Now mask all bank sizes on a page boundary, it is all we can
	 * use anyways.
	 */
	for (i = 0; sp_banks[i].num_bytes != 0; i++)
		sp_banks[i].num_bytes &= PAGE_MASK;

	return tally;
}

void unhandled_fault(unsigned long address, struct task_struct *tsk,
                     struct pt_regs *regs)
{
	if ((unsigned long) address < PAGE_SIZE) {
		printk(KERN_ALERT "Unable to handle kernel NULL "
		       "pointer dereference\n");
	} else {
		printk(KERN_ALERT "Unable to handle kernel paging request "
		       "at virtual address %016lx\n", (unsigned long)address);
	}
	printk(KERN_ALERT "tsk->{mm,active_mm}->context = %016lx\n",
	       (tsk->mm ? tsk->mm->context : tsk->active_mm->context));
	printk(KERN_ALERT "tsk->{mm,active_mm}->pgd = %016lx\n",
	       (tsk->mm ? (unsigned long) tsk->mm->pgd :
		          (unsigned long) tsk->active_mm->pgd));
	die_if_kernel("Oops", regs);
}

static unsigned int get_user_insn(unsigned long tpc)
{
	pgd_t *pgdp = pgd_offset(current->mm, tpc);
	pmd_t *pmdp;
	pte_t *ptep, pte;
	unsigned long pa;
	u32 insn = 0;

	if (pgd_none(*pgdp))
		goto out;
	pmdp = pmd_offset(pgdp, tpc);
	if (pmd_none(*pmdp))
		goto out;
	ptep = pte_offset(pmdp, tpc);
	pte = *ptep;
	if (!pte_present(pte))
		goto out;

	pa  = (pte_val(pte) & _PAGE_PADDR);
	pa += (tpc & ~PAGE_MASK);

	/* Use phys bypass so we don't pollute dtlb/dcache. */
	__asm__ __volatile__("lduwa [%1] %2, %0"
			     : "=r" (insn)
			     : "r" (pa), "i" (ASI_PHYS_USE_EC));

out:
	return insn;
}

static void do_fault_siginfo(int code, int sig, unsigned long address)
{
	siginfo_t info;

	info.si_code = code;
	info.si_signo = sig;
	info.si_errno = 0;
	info.si_addr = (void *) address;
	info.si_trapno = 0;
	force_sig_info(sig, &info, current);
}

extern int handle_ldf_stq(u32, struct pt_regs *);
extern int handle_ld_nf(u32, struct pt_regs *);

static void do_kernel_fault(struct pt_regs *regs, int si_code, int fault_code,
			    unsigned int insn, unsigned long address)
{
	unsigned long g2;
	unsigned char asi = ASI_P;
		
	if (!insn) {
		if (regs->tstate & TSTATE_PRIV) {
			if (!regs->tpc || (regs->tpc & 0x3))
				goto cannot_handle;
			insn = *(unsigned int *)regs->tpc;
		} else {
			insn = get_user_insn(regs->tpc);
		}
	}

	/* If user insn could be read (thus insn is zero), that
	 * is fine.  We will just gun down the process with a signal
	 * in that case.
	 */

	if (!(fault_code & FAULT_CODE_WRITE) &&
	    (insn & 0xc0800000) == 0xc0800000) {
		if (insn & 0x2000)
			asi = (regs->tstate >> 24);
		else
			asi = (insn >> 5);
		if ((asi & 0xf2) == 0x82) {
			if (insn & 0x1000000) {
				handle_ldf_stq(insn, regs);
			} else {
				/* This was a non-faulting load. Just clear the
				 * destination register(s) and continue with the next
				 * instruction. -jj
				 */
				handle_ld_nf(insn, regs);
			}
			return;
		}
	}
		
	g2 = regs->u_regs[UREG_G2];

	/* Is this in ex_table? */
	if (regs->tstate & TSTATE_PRIV) {
		unsigned long fixup;

		if (asi == ASI_P && (insn & 0xc0800000) == 0xc0800000) {
			if (insn & 0x2000)
				asi = (regs->tstate >> 24);
			else
				asi = (insn >> 5);
		}
	
		/* Look in asi.h: All _S asis have LS bit set */
		if ((asi & 0x1) &&
		    (fixup = search_exception_table (regs->tpc, &g2))) {
			regs->tpc = fixup;
			regs->tnpc = regs->tpc + 4;
			regs->u_regs[UREG_G2] = g2;
			return;
		}
	} else {
		/* The si_code was set to make clear whether
		 * this was a SEGV_MAPERR or SEGV_ACCERR fault.
		 */
		do_fault_siginfo(si_code, SIGSEGV, address);
		return;
	}

cannot_handle:
	unhandled_fault (address, current, regs);
}

asmlinkage void do_sparc64_fault(struct pt_regs *regs)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;
	unsigned int insn = 0;
	int si_code, fault_code;
	unsigned long address;

	si_code = SEGV_MAPERR;
	fault_code = current->thread.fault_code;
	address = current->thread.fault_address;

	if ((fault_code & FAULT_CODE_ITLB) &&
	    (fault_code & FAULT_CODE_DTLB))
		BUG();

	/*
	 * If we're in an interrupt or have no user
	 * context, we must not take the fault..
	 */
	if (in_interrupt() || !mm)
		goto handle_kernel_fault;

	down(&mm->mmap_sem);
	vma = find_vma(mm, address);
	if (!vma)
		goto bad_area;

	/* Pure DTLB misses do not tell us whether the fault causing
	 * load/store/atomic was a write or not, it only says that there
	 * was no match.  So in such a case we (carefully) read the
	 * instruction to try and figure this out.  It's an optimization
	 * so it's ok if we can't do this.
	 *
	 * Special hack, window spill/fill knows the exact fault type.
	 */
	if (((fault_code &
	      (FAULT_CODE_DTLB | FAULT_CODE_WRITE | FAULT_CODE_WINFIXUP)) == FAULT_CODE_DTLB) &&
	    (vma->vm_flags & VM_WRITE) != 0) {
		unsigned long tpc = regs->tpc;

		if (tpc & 0x3)
			goto continue_fault;

		if (regs->tstate & TSTATE_PRIV)
			insn = *(unsigned int *)tpc;
		else
			insn = get_user_insn(tpc);

		if ((insn & 0xc0200000) == 0xc0200000 &&
		    (insn & 0x1780000) != 0x1680000) {
			/* Don't bother updating thread struct value,
			 * because update_mmu_cache only cares which tlb
			 * the access came from.
			 */
			fault_code |= FAULT_CODE_WRITE;
		}
	}
continue_fault:

	if (vma->vm_start <= address)
		goto good_area;
	if (!(vma->vm_flags & VM_GROWSDOWN))
		goto bad_area;
	if (expand_stack(vma, address))
		goto bad_area;
	/*
	 * Ok, we have a good vm_area for this memory access, so
	 * we can handle it..
	 */
good_area:
	si_code = SEGV_ACCERR;
	if (fault_code & FAULT_CODE_WRITE) {
		if (!(vma->vm_flags & VM_WRITE))
			goto bad_area;
		if ((vma->vm_flags & VM_EXEC) != 0 &&
		    vma->vm_file != NULL)
			current->thread.use_blkcommit = 1;
	} else {
		/* Allow reads even for write-only mappings */
		if (!(vma->vm_flags & (VM_READ | VM_EXEC)))
			goto bad_area;
	}

	switch (handle_mm_fault(mm, vma, address, (fault_code & FAULT_CODE_WRITE))) {
	case 1:
		current->min_flt++;
		break;
	case 2:
		current->maj_flt++;
		break;
	case 0:
		goto do_sigbus;
	default:
		goto out_of_memory;
	}

	up(&mm->mmap_sem);
	goto fault_done;

	/*
	 * Something tried to access memory that isn't in our memory map..
	 * Fix it, but check if it's kernel or user first..
	 */
bad_area:
	up(&mm->mmap_sem);

handle_kernel_fault:
	do_kernel_fault(regs, si_code, fault_code, insn, address);

	goto fault_done;

/*
 * We ran out of memory, or some other thing happened to us that made
 * us unable to handle the page fault gracefully.
 */
out_of_memory:
	up(&mm->mmap_sem);
	printk("VM: killing process %s\n", current->comm);
	if (!(regs->tstate & TSTATE_PRIV))
		do_exit(SIGKILL);
	goto handle_kernel_fault;

do_sigbus:
	up(&mm->mmap_sem);

	/*
	 * Send a sigbus, regardless of whether we were in kernel
	 * or user mode.
	 */
	do_fault_siginfo(BUS_ADRERR, SIGBUS, address);

	/* Kernel mode? Handle exceptions or die */
	if (regs->tstate & TSTATE_PRIV)
		goto handle_kernel_fault;

fault_done:
	/* These values are no longer needed, clear them. */
	current->thread.fault_code = 0;
	current->thread.use_blkcommit = 0;
	current->thread.fault_address = 0;
}
