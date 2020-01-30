/*
 * Branch and jump emulation.
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1996, 1997 by Ralf Baechle
 */
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <asm/branch.h>
#include <asm/inst.h>
#include <asm/ptrace.h>
#include <asm/uaccess.h>

/*
 * Compute the return address and do emulate branch simulation, if required.
 */
int __compute_return_epc(struct pt_regs *regs)
{
	unsigned int *addr, bit, fcr31;
	long epc;
	union mips_instruction insn;

	epc = regs->cp0_epc;
	if (epc & 3) {
		printk("%s: unaligned epc - sending SIGBUS.\n", current->comm);
		force_sig(SIGBUS, current);
		return -EFAULT;
	}

	/*
	 * Read the instruction
	 */
	addr = (unsigned int *) (unsigned long) epc;
	if (__get_user(insn.word, addr)) {
		force_sig(SIGSEGV, current);
		return -EFAULT;
	}

	regs->regs[0] = 0;
	switch (insn.i_format.opcode) {
	/*
	 * jr and jalr are in r_format format.
	 */
	case spec_op:
		switch (insn.r_format.func) {
		case jalr_op:
			regs->regs[insn.r_format.rd] = epc + 8;
			/* Fall through */
		case jr_op:
			regs->cp0_epc = regs->regs[insn.r_format.rs];
			break;
		}
		break;

	/*
	 * This group contains:
	 * bltz_op, bgez_op, bltzl_op, bgezl_op,
	 * bltzal_op, bgezal_op, bltzall_op, bgezall_op.
	 */
	case bcond_op:
		switch (insn.i_format.rt) {
	 	case bltz_op:
		case bltzl_op:
			if (regs->regs[insn.i_format.rs] < 0)
				epc = epc + 4 + (insn.i_format.simmediate << 2);
			else
				epc += 8;
			regs->cp0_epc = epc;
			break;

		case bgez_op:
		case bgezl_op:
			if (regs->regs[insn.i_format.rs] >= 0)
				epc = epc + 4 + (insn.i_format.simmediate << 2);
			else
				epc += 8;
			regs->cp0_epc = epc;
			break;

		case bltzal_op:
		case bltzall_op:
			regs->regs[31] = epc + 8;
			if (regs->regs[insn.i_format.rs] < 0)
				epc = epc + 4 + (insn.i_format.simmediate << 2);
			else
				epc += 8;
			regs->cp0_epc = epc;
			break;

		case bgezal_op:
		case bgezall_op:
			regs->regs[31] = epc + 8;
			if (regs->regs[insn.i_format.rs] >= 0)
				epc = epc + 4 + (insn.i_format.simmediate << 2);
			else
				epc += 8;
			regs->cp0_epc = epc;
			break;
		}
		break;

	/*
	 * These are unconditional and in j_format.
	 */
	case jal_op:
		regs->regs[31] = regs->cp0_epc + 8;
	case j_op:
		epc += 4;
		epc >>= 28;
		epc <<= 28;
		epc |= (insn.j_format.target << 2);
		regs->cp0_epc = epc;
		break;

	/*
	 * These are conditional and in i_format.
	 */
	case beq_op:
	case beql_op:
		if (regs->regs[insn.i_format.rs] ==
		    regs->regs[insn.i_format.rt])
			epc = epc + 4 + (insn.i_format.simmediate << 2);
		else
			epc += 8;
		regs->cp0_epc = epc;
		break;

	case bne_op:
	case bnel_op:
		if (regs->regs[insn.i_format.rs] !=
		    regs->regs[insn.i_format.rt])
			epc = epc + 4 + (insn.i_format.simmediate << 2);
		else
			epc += 8;
		regs->cp0_epc = epc;
		break;

	case blez_op: /* not really i_format */
	case blezl_op:
		/* rt field assumed to be zero */
		if (regs->regs[insn.i_format.rs] <= 0)
			epc = epc + 4 + (insn.i_format.simmediate << 2);
		else
			epc += 8;
		regs->cp0_epc = epc;
		break;

	case bgtz_op:
	case bgtzl_op:
		/* rt field assumed to be zero */
		if (regs->regs[insn.i_format.rs] > 0)
			epc = epc + 4 + (insn.i_format.simmediate << 2);
		else
			epc += 8;
		regs->cp0_epc = epc;
		break;

	/*
	 * And now the FPA/cp1 branch instructions.
	 */
	case cop1_op:
		asm ("cfc1\t%0,$31":"=r" (fcr31));
		bit = (insn.i_format.rt >> 2);
		bit += (bit != 0);
		bit += 23;
		switch (insn.i_format.rt) {
		case 0:	/* bc1f */
		case 2:	/* bc1fl */
			if (~fcr31 & (1 << bit))
				epc = epc + 4 + (insn.i_format.simmediate << 2);
			else
				epc += 8;
			regs->cp0_epc = epc;
			break;

		case 1:	/* bc1t */
		case 3:	/* bc1tl */
			if (fcr31 & (1 << bit))
				epc = epc + 4 + (insn.i_format.simmediate << 2);
			else
				epc += 8;
			regs->cp0_epc = epc;
			break;
		}
		break;
	}

	return 0;
}
