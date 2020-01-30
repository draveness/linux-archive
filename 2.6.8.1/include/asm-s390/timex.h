/*
 *  include/asm-s390/timex.h
 *
 *  S390 version
 *    Copyright (C) 1999 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *
 *  Derived from "include/asm-i386/timex.h"
 *    Copyright (C) 1992, Linus Torvalds
 */

#ifndef _ASM_S390_TIMEX_H
#define _ASM_S390_TIMEX_H

#define CLOCK_TICK_RATE	1193180 /* Underlying HZ */
#define CLOCK_TICK_FACTOR	20	/* Factor of both 1000000 and CLOCK_TICK_RATE */
#define FINETUNE ((((((long)LATCH * HZ - CLOCK_TICK_RATE) << SHIFT_HZ) * \
	(1000000/CLOCK_TICK_FACTOR) / (CLOCK_TICK_RATE/CLOCK_TICK_FACTOR)) \
		<< (SHIFT_SCALE-SHIFT_HZ)) / HZ)

typedef unsigned long long cycles_t;

extern cycles_t cacheflush_time;

static inline cycles_t get_cycles(void)
{
	cycles_t cycles;

	__asm__("stck 0(%1)" : "=m" (cycles) : "a" (&cycles) : "cc");
	return cycles >> 2;
}

static inline unsigned long long get_clock (void)
{
	unsigned long long clk;

	__asm__("stck 0(%1)" : "=m" (clk) : "a" (&clk) : "cc");
	return clk;
}

#endif
