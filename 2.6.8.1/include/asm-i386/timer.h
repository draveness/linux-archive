#ifndef _ASMi386_TIMER_H
#define _ASMi386_TIMER_H

/**
 * struct timer_ops - used to define a timer source
 *
 * @name: name of the timer.
 * @init: Probes and initializes the timer. Takes clock= override 
 *        string as an argument. Returns 0 on success, anything else
 *        on failure.
 * @mark_offset: called by the timer interrupt.
 * @get_offset:  called by gettimeofday(). Returns the number of microseconds
 *               since the last timer interupt.
 * @monotonic_clock: returns the number of nanoseconds since the init of the
 *                   timer.
 * @delay: delays this many clock cycles.
 */
struct timer_opts{
	char* name;
	int (*init)(char *override);
	void (*mark_offset)(void);
	unsigned long (*get_offset)(void);
	unsigned long long (*monotonic_clock)(void);
	void (*delay)(unsigned long);
};

#define TICK_SIZE (tick_nsec / 1000)

extern struct timer_opts* select_timer(void);
extern void clock_fallback(void);

/* Modifiers for buggy PIT handling */

extern int pit_latch_buggy;

extern struct timer_opts *cur_timer;
extern int timer_ack;

/* list of externed timers */
extern struct timer_opts timer_none;
extern struct timer_opts timer_pit;
extern struct timer_opts timer_tsc;
#ifdef CONFIG_X86_CYCLONE_TIMER
extern struct timer_opts timer_cyclone;
#endif

extern unsigned long calibrate_tsc(void);
extern void init_cpu_khz(void);
#ifdef CONFIG_HPET_TIMER
extern struct timer_opts timer_hpet;
extern unsigned long calibrate_tsc_hpet(unsigned long *tsc_hpet_quotient_ptr);
#endif

#ifdef CONFIG_X86_PM_TIMER
extern struct timer_opts timer_pmtmr;
#endif
#endif
