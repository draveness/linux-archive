/*
 *  linux/asm-arm/proc-armv/assembler.h
 *
 *  Copyright (C) 1996-2000 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 *  This file contains ARM processor specifics for
 *  the ARM6 and better processors.
 */
#define MODE_USR	USR_MODE
#define MODE_FIQ	FIQ_MODE
#define MODE_IRQ	IRQ_MODE
#define MODE_SVC	SVC_MODE

#define DEFAULT_FIQ	MODE_FIQ

/*
 * LOADREGS - ldm with PC in register list (eg, ldmfd sp!, {pc})
 */
#ifdef __STDC__
#define LOADREGS(cond, base, reglist...)\
	ldm##cond	base,reglist
#else
#define LOADREGS(cond, base, reglist...)\
	ldm/**/cond	base,reglist
#endif

/*
 * Build a return instruction for this processor type.
 */
#define RETINSTR(instr, regs...)\
	instr	regs

/*
 * Save the current IRQ state and disable IRQs.  Note that this macro
 * assumes FIQs are enabled, and that the processor is in SVC mode.
 */
	.macro	save_and_disable_irqs, oldcpsr, temp
	mrs	\oldcpsr, cpsr
	mov	\temp, #I_BIT | MODE_SVC
	msr	cpsr_c, \temp
	.endm

/*
 * Restore interrupt state previously stored in a register.  We don't
 * guarantee that this will preserve the flags.
 */
	.macro	restore_irqs, oldcpsr
	msr	cpsr_c, \oldcpsr
	.endm

/*
 * These two are used to save LR/restore PC over a user-based access.
 * The old 26-bit architecture requires that we do.  On 32-bit
 * architecture, we can safely ignore this requirement.
 */
	.macro	save_lr
	.endm

	.macro	restore_pc
	mov	pc, lr
	.endm

#define USER(x...)				\
9999:	x;					\
	.section __ex_table,"a";		\
	.align	3;				\
	.long	9999b,9001f;			\
	.previous


