/*
 *  linux/arch/arm/kernel/fiq.c
 *
 *  Copyright (C) 1998 Russell King
 *  Copyright (C) 1998, 1999 Phil Blundell
 *
 *  FIQ support written by Philip Blundell <philb@gnu.org>, 1998.
 *
 *  FIQ support re-written by Russell King to be more generic
 *
 * We now properly support a method by which the FIQ handlers can
 * be stacked onto the vector.  We still do not support sharing
 * the FIQ vector itself.
 *
 * Operation is as follows:
 *  1. Owner A claims FIQ:
 *     - default_fiq relinquishes control.
 *  2. Owner A:
 *     - inserts code.
 *     - sets any registers,
 *     - enables FIQ.
 *  3. Owner B claims FIQ:
 *     - if owner A has a relinquish function.
 *       - disable FIQs.
 *       - saves any registers.
 *       - returns zero.
 *  4. Owner B:
 *     - inserts code.
 *     - sets any registers,
 *     - enables FIQ.
 *  5. Owner B releases FIQ:
 *     - Owner A is asked to reacquire FIQ:
 *	 - inserts code.
 *	 - restores saved registers.
 *	 - enables FIQ.
 *  6. Goto 3
 */
#include <linux/config.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/init.h>

#include <asm/fiq.h>
#include <asm/io.h>
#include <asm/pgalloc.h>
#include <asm/system.h>
#include <asm/uaccess.h>

#define FIQ_VECTOR 0x1c

static unsigned long no_fiq_insn;

#ifdef CONFIG_CPU_32
static inline void unprotect_page_0(void)
{
	modify_domain(DOMAIN_USER, DOMAIN_MANAGER);
}

static inline void protect_page_0(void)
{
	modify_domain(DOMAIN_USER, DOMAIN_CLIENT);
}
#else

#define unprotect_page_0()
#define protect_page_0()

#endif

/* Default reacquire function
 * - we always relinquish FIQ control
 * - we always reacquire FIQ control
 */
int fiq_def_op(void *ref, int relinquish)
{
	if (!relinquish) {
		unprotect_page_0();
		*(unsigned long *)FIQ_VECTOR = no_fiq_insn;
		protect_page_0();
		flush_icache_range(FIQ_VECTOR, FIQ_VECTOR + 4);
	}

	return 0;
}

static struct fiq_handler default_owner = {
	name:	"default",
	fiq_op:	fiq_def_op,
};

static struct fiq_handler *current_fiq = &default_owner;

int get_fiq_list(char *buf)
{
	char *p = buf;

	if (current_fiq != &default_owner)
		p += sprintf(p, "FIQ:              %s\n",
			     current_fiq->name);

	return p - buf;
}

void set_fiq_handler(void *start, unsigned int length)
{
	unprotect_page_0();

	memcpy((void *)FIQ_VECTOR, start, length);

	protect_page_0();
	flush_icache_range(FIQ_VECTOR, FIQ_VECTOR + length);
}

/*
 * Taking an interrupt in FIQ mode is death, so both these functions
 * disable irqs for the duration. 
 */
void set_fiq_regs(struct pt_regs *regs)
{
	register unsigned long tmp, tmp2;
	__asm__ volatile (
#ifdef CONFIG_CPU_26
	"mov	%0, pc
	bic	%1, %0, #0x3
	orr	%1, %1, #0x0c000001
	teqp	%1, #0		@ select FIQ mode
	mov	r0, r0
	ldmia	%2, {r8 - r14}
	teqp	%0, #0		@ return to SVC mode
	mov	r0, r0"
#endif
#ifdef CONFIG_CPU_32
	"mrs	%0, cpsr
	mov	%1, #0xc1
	msr	cpsr_c, %1	@ select FIQ mode
	mov	r0, r0
	ldmia	%2, {r8 - r14}
	msr	cpsr_c, %0	@ return to SVC mode
	mov	r0, r0"
#endif
	: "=&r" (tmp), "=&r" (tmp2)
	: "r" (&regs->ARM_r8)
	/* These registers aren't modified by the above code in a way
	   visible to the compiler, but we mark them as clobbers anyway
	   so that GCC won't put any of the input or output operands in
	   them.  */
	: "r8", "r9", "r10", "r11", "r12", "r13", "r14");
}

void get_fiq_regs(struct pt_regs *regs)
{
	register unsigned long tmp, tmp2;
	__asm__ volatile (
#ifdef CONFIG_CPU_26
	"mov	%0, pc
	bic	%1, %0, #0x3
	orr	%1, %1, #0x0c000001
	teqp	%1, #0		@ select FIQ mode
	mov	r0, r0
	stmia	%2, {r8 - r14}
	teqp	%0, #0		@ return to SVC mode
	mov	r0, r0"
#endif
#ifdef CONFIG_CPU_32
	"mrs	%0, cpsr
	mov	%1, #0xc1
	msr	cpsr_c, %1	@ select FIQ mode
	mov	r0, r0
	stmia	%2, {r8 - r14}
	msr	cpsr_c, %0	@ return to SVC mode
	mov	r0, r0"
#endif
	: "=&r" (tmp), "=&r" (tmp2)
	: "r" (&regs->ARM_r8)
	/* These registers aren't modified by the above code in a way
	   visible to the compiler, but we mark them as clobbers anyway
	   so that GCC won't put any of the input or output operands in
	   them.  */
	: "r8", "r9", "r10", "r11", "r12", "r13", "r14");
}

int claim_fiq(struct fiq_handler *f)
{
	int ret = 0;

	if (current_fiq) {
		ret = -EBUSY;

		if (current_fiq->fiq_op != NULL)
			ret = current_fiq->fiq_op(current_fiq->dev_id, 1);
	}

	if (!ret) {
		f->next = current_fiq;
		current_fiq = f;
	}

	return ret;
}

void release_fiq(struct fiq_handler *f)
{
	if (current_fiq != f) {
		printk(KERN_ERR "%s FIQ trying to release %s FIQ\n",
		       f->name, current_fiq->name);
#ifdef CONFIG_DEBUG_ERRORS
		__backtrace();
#endif
		return;
	}

	do
		current_fiq = current_fiq->next;
	while (current_fiq->fiq_op(current_fiq->dev_id, 0));
}

void __init init_FIQ(void)
{
	no_fiq_insn = *(unsigned long *)FIQ_VECTOR;
	set_fs(get_fs());
}
