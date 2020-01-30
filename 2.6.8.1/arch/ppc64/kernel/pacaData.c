/*
 * c 2001 PPC 64 Team, IBM Corp
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#include <asm/types.h>
#include <asm/page.h>
#include <stddef.h>
#include <linux/config.h>
#include <linux/threads.h>
#include <asm/processor.h>
#include <asm/ptrace.h>

#include <asm/iSeries/ItLpPaca.h>
#include <asm/iSeries/ItLpQueue.h>
#include <asm/naca.h>
#include <asm/paca.h>

struct naca_struct *naca;
struct systemcfg *systemcfg;

/* This symbol is provided by the linker - let it fill in the paca
 * field correctly */
extern unsigned long __toc_start;

/* Stack space used when we detect a bad kernel stack pointer, and
 * early in SMP boots before relocation is enabled.
 *
 * ABI requires stack to be 128-byte aligned
 */
char emergency_stack[PAGE_SIZE * NR_CPUS] __attribute__((aligned(128)));

/* The Paca is an array with one entry per processor.  Each contains an 
 * ItLpPaca, which contains the information shared between the 
 * hypervisor and Linux.  Each also contains an ItLpRegSave area which
 * is used by the hypervisor to save registers.
 * On systems with hardware multi-threading, there are two threads
 * per processor.  The Paca array must contain an entry for each thread.
 * The VPD Areas will give a max logical processors = 2 * max physical
 * processors.  The processor VPD array needs one entry per physical
 * processor (not thread).
 */
#define PACAINITDATA(number,start,lpq,asrr,asrv)			    \
{									    \
	.lppaca_ptr = &paca[number].lppaca,				    \
	.reg_save_ptr = &paca[number].reg_save,				    \
	.lock_token = 0x8000,						    \
	.paca_index = (number),		/* Paca Index */		    \
	.lpqueue_ptr = (lpq),		/* &xItLpQueue, */		    \
	.default_decr = 0x00ff0000,	/* Initial Decr */		    \
	.kernel_toc = (unsigned long)(&__toc_start) + 0x8000UL,		    \
	.stab_real = (asrr), 		/* Real pointer to segment table */ \
	.stab_addr = (asrv),		/* Virt pointer to segment table */ \
	.emergency_sp = &emergency_stack[((number)+1) * PAGE_SIZE],	    \
	.cpu_start = (start),		/* Processor start */		    \
	.lppaca = {							    \
		.xDesc = 0xd397d781,	/* "LpPa" */			    \
		.xSize = sizeof(struct ItLpPaca),			    \
		.xFPRegsInUse = 1,					    \
		.xDynProcStatus = 2,					    \
		.xDecrVal = 0x00ff0000,					    \
		.xEndOfQuantum = 0xfffffffffffffffful,			    \
		.xSLBCount = 64,					    \
	},								    \
	.reg_save = {							    \
		.xDesc = 0xd397d9e2,	/* "LpRS" */			    \
		.xSize = sizeof(struct ItLpRegSave)			    \
	},								    \
}

struct paca_struct paca[] __page_aligned = {
#ifdef CONFIG_PPC_ISERIES
	PACAINITDATA( 0, 1, &xItLpQueue, 0, STAB0_VIRT_ADDR),
#else
	PACAINITDATA( 0, 1, NULL, STAB0_PHYS_ADDR, STAB0_VIRT_ADDR),
#endif
	PACAINITDATA( 1, 0, NULL, 0, 0),
	PACAINITDATA( 2, 0, NULL, 0, 0),
	PACAINITDATA( 3, 0, NULL, 0, 0),
	PACAINITDATA( 4, 0, NULL, 0, 0),
	PACAINITDATA( 5, 0, NULL, 0, 0),
	PACAINITDATA( 6, 0, NULL, 0, 0),
	PACAINITDATA( 7, 0, NULL, 0, 0),
	PACAINITDATA( 8, 0, NULL, 0, 0),
	PACAINITDATA( 9, 0, NULL, 0, 0),
	PACAINITDATA(10, 0, NULL, 0, 0),
	PACAINITDATA(11, 0, NULL, 0, 0),
	PACAINITDATA(12, 0, NULL, 0, 0),
	PACAINITDATA(13, 0, NULL, 0, 0),
	PACAINITDATA(14, 0, NULL, 0, 0),
	PACAINITDATA(15, 0, NULL, 0, 0),
	PACAINITDATA(16, 0, NULL, 0, 0),
	PACAINITDATA(17, 0, NULL, 0, 0),
	PACAINITDATA(18, 0, NULL, 0, 0),
	PACAINITDATA(19, 0, NULL, 0, 0),
	PACAINITDATA(20, 0, NULL, 0, 0),
	PACAINITDATA(21, 0, NULL, 0, 0),
	PACAINITDATA(22, 0, NULL, 0, 0),
	PACAINITDATA(23, 0, NULL, 0, 0),
	PACAINITDATA(24, 0, NULL, 0, 0),
	PACAINITDATA(25, 0, NULL, 0, 0),
	PACAINITDATA(26, 0, NULL, 0, 0),
	PACAINITDATA(27, 0, NULL, 0, 0),
	PACAINITDATA(28, 0, NULL, 0, 0),
	PACAINITDATA(29, 0, NULL, 0, 0),
	PACAINITDATA(30, 0, NULL, 0, 0),
	PACAINITDATA(31, 0, NULL, 0, 0),
#if NR_CPUS > 32
	PACAINITDATA(32, 0, NULL, 0, 0),
	PACAINITDATA(33, 0, NULL, 0, 0),
	PACAINITDATA(34, 0, NULL, 0, 0),
	PACAINITDATA(35, 0, NULL, 0, 0),
	PACAINITDATA(36, 0, NULL, 0, 0),
	PACAINITDATA(37, 0, NULL, 0, 0),
	PACAINITDATA(38, 0, NULL, 0, 0),
	PACAINITDATA(39, 0, NULL, 0, 0),
	PACAINITDATA(40, 0, NULL, 0, 0),
	PACAINITDATA(41, 0, NULL, 0, 0),
	PACAINITDATA(42, 0, NULL, 0, 0),
	PACAINITDATA(43, 0, NULL, 0, 0),
	PACAINITDATA(44, 0, NULL, 0, 0),
	PACAINITDATA(45, 0, NULL, 0, 0),
	PACAINITDATA(46, 0, NULL, 0, 0),
	PACAINITDATA(47, 0, NULL, 0, 0),
	PACAINITDATA(48, 0, NULL, 0, 0),
	PACAINITDATA(49, 0, NULL, 0, 0),
	PACAINITDATA(50, 0, NULL, 0, 0),
	PACAINITDATA(51, 0, NULL, 0, 0),
	PACAINITDATA(52, 0, NULL, 0, 0),
	PACAINITDATA(53, 0, NULL, 0, 0),
	PACAINITDATA(54, 0, NULL, 0, 0),
	PACAINITDATA(55, 0, NULL, 0, 0),
	PACAINITDATA(56, 0, NULL, 0, 0),
	PACAINITDATA(57, 0, NULL, 0, 0),
	PACAINITDATA(58, 0, NULL, 0, 0),
	PACAINITDATA(59, 0, NULL, 0, 0),
	PACAINITDATA(60, 0, NULL, 0, 0),
	PACAINITDATA(61, 0, NULL, 0, 0),
	PACAINITDATA(62, 0, NULL, 0, 0),
	PACAINITDATA(63, 0, NULL, 0, 0),
#if NR_CPUS > 64
	PACAINITDATA(64, 0, NULL, 0, 0),
	PACAINITDATA(65, 0, NULL, 0, 0),
	PACAINITDATA(66, 0, NULL, 0, 0),
	PACAINITDATA(67, 0, NULL, 0, 0),
	PACAINITDATA(68, 0, NULL, 0, 0),
	PACAINITDATA(69, 0, NULL, 0, 0),
	PACAINITDATA(70, 0, NULL, 0, 0),
	PACAINITDATA(71, 0, NULL, 0, 0),
	PACAINITDATA(72, 0, NULL, 0, 0),
	PACAINITDATA(73, 0, NULL, 0, 0),
	PACAINITDATA(74, 0, NULL, 0, 0),
	PACAINITDATA(75, 0, NULL, 0, 0),
	PACAINITDATA(76, 0, NULL, 0, 0),
	PACAINITDATA(77, 0, NULL, 0, 0),
	PACAINITDATA(78, 0, NULL, 0, 0),
	PACAINITDATA(79, 0, NULL, 0, 0),
	PACAINITDATA(80, 0, NULL, 0, 0),
	PACAINITDATA(81, 0, NULL, 0, 0),
	PACAINITDATA(82, 0, NULL, 0, 0),
	PACAINITDATA(83, 0, NULL, 0, 0),
	PACAINITDATA(84, 0, NULL, 0, 0),
	PACAINITDATA(85, 0, NULL, 0, 0),
	PACAINITDATA(86, 0, NULL, 0, 0),
	PACAINITDATA(87, 0, NULL, 0, 0),
	PACAINITDATA(88, 0, NULL, 0, 0),
	PACAINITDATA(89, 0, NULL, 0, 0),
	PACAINITDATA(90, 0, NULL, 0, 0),
	PACAINITDATA(91, 0, NULL, 0, 0),
	PACAINITDATA(92, 0, NULL, 0, 0),
	PACAINITDATA(93, 0, NULL, 0, 0),
	PACAINITDATA(94, 0, NULL, 0, 0),
	PACAINITDATA(95, 0, NULL, 0, 0),
	PACAINITDATA(96, 0, NULL, 0, 0),
	PACAINITDATA(97, 0, NULL, 0, 0),
	PACAINITDATA(98, 0, NULL, 0, 0),
	PACAINITDATA(99, 0, NULL, 0, 0),
	PACAINITDATA(100, 0, NULL, 0, 0),
	PACAINITDATA(101, 0, NULL, 0, 0),
	PACAINITDATA(102, 0, NULL, 0, 0),
	PACAINITDATA(103, 0, NULL, 0, 0),
	PACAINITDATA(104, 0, NULL, 0, 0),
	PACAINITDATA(105, 0, NULL, 0, 0),
	PACAINITDATA(106, 0, NULL, 0, 0),
	PACAINITDATA(107, 0, NULL, 0, 0),
	PACAINITDATA(108, 0, NULL, 0, 0),
	PACAINITDATA(109, 0, NULL, 0, 0),
	PACAINITDATA(110, 0, NULL, 0, 0),
	PACAINITDATA(111, 0, NULL, 0, 0),
	PACAINITDATA(112, 0, NULL, 0, 0),
	PACAINITDATA(113, 0, NULL, 0, 0),
	PACAINITDATA(114, 0, NULL, 0, 0),
	PACAINITDATA(115, 0, NULL, 0, 0),
	PACAINITDATA(116, 0, NULL, 0, 0),
	PACAINITDATA(117, 0, NULL, 0, 0),
	PACAINITDATA(118, 0, NULL, 0, 0),
	PACAINITDATA(119, 0, NULL, 0, 0),
	PACAINITDATA(120, 0, NULL, 0, 0),
	PACAINITDATA(121, 0, NULL, 0, 0),
	PACAINITDATA(122, 0, NULL, 0, 0),
	PACAINITDATA(123, 0, NULL, 0, 0),
	PACAINITDATA(124, 0, NULL, 0, 0),
	PACAINITDATA(125, 0, NULL, 0, 0),
	PACAINITDATA(126, 0, NULL, 0, 0),
	PACAINITDATA(127, 0, NULL, 0, 0),
#endif
#endif
};
