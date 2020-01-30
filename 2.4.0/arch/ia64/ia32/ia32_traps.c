/*
 * IA32 exceptions handler
 *
 * 06/16/00	A. Mallick	added siginfo for most cases (close to IA32)
 */

#include <linux/kernel.h>
#include <linux/sched.h>

#include <asm/ia32.h>
#include <asm/ptrace.h>

int
ia32_exception (struct pt_regs *regs, unsigned long isr)
{
	struct siginfo siginfo;

	siginfo.si_errno = 0;
	switch ((isr >> 16) & 0xff) {
	      case 1:
	      case 2:
		siginfo.si_signo = SIGTRAP;
		if (isr == 0)
			siginfo.si_code = TRAP_TRACE;
		else if (isr & 0x4)
			siginfo.si_code = TRAP_BRANCH;
		else
			siginfo.si_code = TRAP_BRKPT;
		break;

	      case 3:
		siginfo.si_signo = SIGTRAP;
		siginfo.si_code = TRAP_BRKPT;
		break;

	      case 0:	/* Divide fault */
		siginfo.si_signo = SIGFPE;
		siginfo.si_code = FPE_INTDIV;
		break;

	      case 4:	/* Overflow */
	      case 5:	/* Bounds fault */
		siginfo.si_signo = SIGFPE;
		siginfo.si_code = 0;
		break;

	      case 6:	/* Invalid Op-code */
		siginfo.si_signo = SIGILL;
		siginfo.si_code = ILL_ILLOPN;
		break;

	      case 7:	/* FP DNA */
	      case 8:	/* Double Fault */
	      case 9:	/* Invalid TSS */
	      case 11:	/* Segment not present */
	      case 12:	/* Stack fault */
	      case 13:	/* General Protection Fault */
		siginfo.si_signo = SIGSEGV;
		siginfo.si_code = 0;
		break;

	      case 16:	/* Pending FP error */
		{
			unsigned long fsr, fcr;

			asm ("mov %0=ar.fsr;"
			     "mov %1=ar.fcr;"
			     : "=r"(fsr), "=r"(fcr));

			siginfo.si_signo = SIGFPE;
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
			switch(((~fcr) & (fsr & 0x3f)) | (fsr & 0x240)) {
				case 0x000:
				default:
					siginfo.si_code = 0;
					break;
				case 0x001: /* Invalid Op */
				case 0x040: /* Stack Fault */
				case 0x240: /* Stack Fault | Direction */
					siginfo.si_code = FPE_FLTINV;
					break;
				case 0x002: /* Denormalize */
				case 0x010: /* Underflow */
					siginfo.si_code = FPE_FLTUND;
					break;
				case 0x004: /* Zero Divide */
					siginfo.si_code = FPE_FLTDIV;
					break;
				case 0x008: /* Overflow */
					siginfo.si_code = FPE_FLTOVF;
					break;
				case 0x020: /* Precision */
					siginfo.si_code = FPE_FLTRES;
					break;
			}

			break;
		}

	      case 17:	/* Alignment check */
		siginfo.si_signo = SIGSEGV;
		siginfo.si_code = BUS_ADRALN;
		break;

	      case 19:	/* SSE Numeric error */
		siginfo.si_signo = SIGFPE;
		siginfo.si_code = 0;
		break;

	      default:
		return -1;
	}
	force_sig_info(siginfo.si_signo, &siginfo, current);
	return 0;
}
