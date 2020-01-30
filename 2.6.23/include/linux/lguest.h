/* Things the lguest guest needs to know.  Note: like all lguest interfaces,
 * this is subject to wild and random change between versions. */
#ifndef _ASM_LGUEST_H
#define _ASM_LGUEST_H

#ifndef __ASSEMBLY__
#include <asm/irq.h>

#define LHCALL_FLUSH_ASYNC	0
#define LHCALL_LGUEST_INIT	1
#define LHCALL_CRASH		2
#define LHCALL_LOAD_GDT		3
#define LHCALL_NEW_PGTABLE	4
#define LHCALL_FLUSH_TLB	5
#define LHCALL_LOAD_IDT_ENTRY	6
#define LHCALL_SET_STACK	7
#define LHCALL_TS		8
#define LHCALL_SET_CLOCKEVENT	9
#define LHCALL_HALT		10
#define LHCALL_BIND_DMA		12
#define LHCALL_SEND_DMA		13
#define LHCALL_SET_PTE		14
#define LHCALL_SET_PMD		15
#define LHCALL_LOAD_TLS		16

#define LG_CLOCK_MIN_DELTA	100UL
#define LG_CLOCK_MAX_DELTA	ULONG_MAX

/*G:031 First, how does our Guest contact the Host to ask for privileged
 * operations?  There are two ways: the direct way is to make a "hypercall",
 * to make requests of the Host Itself.
 *
 * Our hypercall mechanism uses the highest unused trap code (traps 32 and
 * above are used by real hardware interrupts).  Seventeen hypercalls are
 * available: the hypercall number is put in the %eax register, and the
 * arguments (when required) are placed in %edx, %ebx and %ecx.  If a return
 * value makes sense, it's returned in %eax.
 *
 * Grossly invalid calls result in Sudden Death at the hands of the vengeful
 * Host, rather than returning failure.  This reflects Winston Churchill's
 * definition of a gentleman: "someone who is only rude intentionally". */
#define LGUEST_TRAP_ENTRY 0x1F

static inline unsigned long
hcall(unsigned long call,
      unsigned long arg1, unsigned long arg2, unsigned long arg3)
{
	/* "int" is the Intel instruction to trigger a trap. */
	asm volatile("int $" __stringify(LGUEST_TRAP_ENTRY)
		       /* The call is in %eax (aka "a"), and can be replaced */
		     : "=a"(call)
		       /* The other arguments are in %eax, %edx, %ebx & %ecx */
		     : "a"(call), "d"(arg1), "b"(arg2), "c"(arg3)
		       /* "memory" means this might write somewhere in memory.
			* This isn't true for all calls, but it's safe to tell
			* gcc that it might happen so it doesn't get clever. */
		     : "memory");
	return call;
}
/*:*/

void async_hcall(unsigned long call,
		 unsigned long arg1, unsigned long arg2, unsigned long arg3);

/* Can't use our min() macro here: needs to be a constant */
#define LGUEST_IRQS (NR_IRQS < 32 ? NR_IRQS: 32)

#define LHCALL_RING_SIZE 64
struct hcall_ring
{
	u32 eax, edx, ebx, ecx;
};

/*G:032 The second method of communicating with the Host is to via "struct
 * lguest_data".  The Guest's very first hypercall is to tell the Host where
 * this is, and then the Guest and Host both publish information in it. :*/
struct lguest_data
{
	/* 512 == enabled (same as eflags in normal hardware).  The Guest
	 * changes interrupts so often that a hypercall is too slow. */
	unsigned int irq_enabled;
	/* Fine-grained interrupt disabling by the Guest */
	DECLARE_BITMAP(blocked_interrupts, LGUEST_IRQS);

	/* The Host writes the virtual address of the last page fault here,
	 * which saves the Guest a hypercall.  CR2 is the native register where
	 * this address would normally be found. */
	unsigned long cr2;

	/* Wallclock time set by the Host. */
	struct timespec time;

	/* Async hypercall ring.  Instead of directly making hypercalls, we can
	 * place them in here for processing the next time the Host wants.
	 * This batching can be quite efficient. */

	/* 0xFF == done (set by Host), 0 == pending (set by Guest). */
	u8 hcall_status[LHCALL_RING_SIZE];
	/* The actual registers for the hypercalls. */
	struct hcall_ring hcalls[LHCALL_RING_SIZE];

/* Fields initialized by the Host at boot: */
	/* Memory not to try to access */
	unsigned long reserve_mem;
	/* ID of this Guest (used by network driver to set ethernet address) */
	u16 guestid;
	/* KHz for the TSC clock. */
	u32 tsc_khz;

/* Fields initialized by the Guest at boot: */
	/* Instruction range to suppress interrupts even if enabled */
	unsigned long noirq_start, noirq_end;
};
extern struct lguest_data lguest_data;
#endif /* __ASSEMBLY__ */
#endif	/* _ASM_LGUEST_H */
