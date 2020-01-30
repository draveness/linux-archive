/*
 *  arch/m68k/bvme6000/config.c
 *
 *  Copyright (C) 1997 Richard Hirst [richard@sleepie.demon.co.uk]
 *
 * Based on:
 *
 *  linux/amiga/config.c
 *
 *  Copyright (C) 1993 Hamish Macdonald
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file README.legal in the main directory of this archive
 * for more details.
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/kd.h>
#include <linux/tty.h>
#include <linux/console.h>
#include <linux/linkage.h>
#include <linux/init.h>
#include <linux/major.h>

#include <asm/bootinfo.h>
#include <asm/system.h>
#include <asm/pgtable.h>
#include <asm/setup.h>
#include <asm/irq.h>
#include <asm/traps.h>
#include <asm/machdep.h>
#include <asm/bvme6000hw.h>

extern void bvme6000_process_int (int level, struct pt_regs *regs);
extern void bvme6000_init_IRQ (void);
extern void bvme6000_free_irq (unsigned int, void *);
extern int  bvme6000_get_irq_list (char *);
extern void bvme6000_enable_irq (unsigned int);
extern void bvme6000_disable_irq (unsigned int);
static void bvme6000_get_model(char *model);
static int  bvme6000_get_hardware_list(char *buffer);
extern int  bvme6000_request_irq(unsigned int irq, void (*handler)(int, void *, struct pt_regs *), unsigned long flags, const char *devname, void *dev_id);
extern void bvme6000_sched_init(void (*handler)(int, void *, struct pt_regs *));
extern int  bvme6000_keyb_init(void);
extern int  bvme6000_kbdrate (struct kbd_repeat *);
extern unsigned long bvme6000_gettimeoffset (void);
extern void bvme6000_gettod (int *year, int *mon, int *day, int *hour,
                           int *min, int *sec);
extern int bvme6000_hwclk (int, struct hwclk_time *);
extern int bvme6000_set_clock_mmss (unsigned long);
extern void bvme6000_check_partition (struct gendisk *hd, unsigned int dev);
extern void bvme6000_mksound( unsigned int count, unsigned int ticks );
extern void bvme6000_reset (void);
extern void bvme6000_waitbut(void);
void bvme6000_set_vectors (void);

static unsigned char bcd2bin (unsigned char b);
static unsigned char bin2bcd (unsigned char b);

/* Save tick handler routine pointer, will point to do_timer() in
 * kernel/sched.c, called via bvme6000_process_int() */

static void (*tick_handler)(int, void *, struct pt_regs *);


int bvme6000_parse_bootinfo(const struct bi_record *bi)
{
	if (bi->tag == BI_VME_TYPE)
		return 0;
	else
		return 1;
}

int bvme6000_kbdrate (struct kbd_repeat *k)
{
	return 0;
}

void bvme6000_mksound( unsigned int count, unsigned int ticks )
{
}

void bvme6000_reset()
{
	volatile PitRegsPtr pit = (PitRegsPtr)BVME_PIT_BASE;

	printk ("\r\n\nCalled bvme6000_reset\r\n"
			"\r\r\r\r\r\r\r\r\r\r\r\r\r\r\r\r\r\r");
	/* The string of returns is to delay the reset until the whole
	 * message is output. */
	/* Enable the watchdog, via PIT port C bit 4 */

	pit->pcddr	|= 0x10;	/* WDOG enable */

	while(1)
		;
}

static void bvme6000_get_model(char *model)
{
    sprintf(model, "BVME%d000", m68k_cputype == CPU_68060 ? 6 : 4);
}


/* No hardware options on BVME6000? */

static int bvme6000_get_hardware_list(char *buffer)
{
    *buffer = '\0';
    return 0;
}


void __init config_bvme6000(void)
{
    volatile PitRegsPtr pit = (PitRegsPtr)BVME_PIT_BASE;

    /* Board type is only set by newer versions of vmelilo/tftplilo */
    if (!vme_brdtype) {
	if (m68k_cputype == CPU_68060)
	    vme_brdtype = VME_TYPE_BVME6000;
	else
	    vme_brdtype = VME_TYPE_BVME4000;
    }
#if 0
    /* Call bvme6000_set_vectors() so ABORT will work, along with BVMBug
     * debugger.  Note trap_init() will splat the abort vector, but
     * bvme6000_init_IRQ() will put it back again.  Hopefully. */

    bvme6000_set_vectors();
#endif

    mach_max_dma_address = 0xffffffff;
    mach_sched_init      = bvme6000_sched_init;
    mach_keyb_init       = bvme6000_keyb_init;
    mach_kbdrate         = bvme6000_kbdrate;
    mach_init_IRQ        = bvme6000_init_IRQ;
    mach_gettimeoffset   = bvme6000_gettimeoffset;
    mach_gettod  	 = bvme6000_gettod;
    mach_hwclk           = bvme6000_hwclk;
    mach_set_clock_mmss	 = bvme6000_set_clock_mmss;
/*  mach_mksound         = bvme6000_mksound; */
    mach_reset		 = bvme6000_reset;
    mach_free_irq	 = bvme6000_free_irq;
    mach_process_int	 = bvme6000_process_int;
    mach_get_irq_list	 = bvme6000_get_irq_list;
    mach_request_irq	 = bvme6000_request_irq;
    enable_irq		 = bvme6000_enable_irq;
    disable_irq          = bvme6000_disable_irq;
    mach_get_model       = bvme6000_get_model;
    mach_get_hardware_list = bvme6000_get_hardware_list;

    printk ("Board is %sconfigured as a System Controller\n",
		*config_reg_ptr & BVME_CONFIG_SW1 ? "" : "not ");

    /* Now do the PIT configuration */

    pit->pgcr	= 0x00;	/* Unidirectional 8 bit, no handshake for now */
    pit->psrr	= 0x18;	/* PIACK and PIRQ fucntions enabled */
    pit->pacr	= 0x00;	/* Sub Mode 00, H2 i/p, no DMA */
    pit->padr	= 0x00;	/* Just to be tidy! */
    pit->paddr	= 0x00;	/* All inputs for now (safest) */
    pit->pbcr	= 0x80;	/* Sub Mode 1x, H4 i/p, no DMA */
    pit->pbdr	= 0xbc | (*config_reg_ptr & BVME_CONFIG_SW1 ? 0 : 0x40);
			/* PRI, SYSCON?, Level3, SCC clks from xtal */
    pit->pbddr	= 0xf3;	/* Mostly outputs */
    pit->pcdr	= 0x01;	/* PA transceiver disabled */
    pit->pcddr	= 0x03;	/* WDOG disable */

    /* Disable snooping for Ethernet and VME accesses */

    bvme_acr_addrctl = 0;
}


void bvme6000_abort_int (int irq, void *dev_id, struct pt_regs *fp)
{
        unsigned long *new = (unsigned long *)vectors;
        unsigned long *old = (unsigned long *)0xf8000000;

        /* Wait for button release */
	while (*config_reg_ptr & BVME_ABORT_STATUS)
		;

        *(new+4) = *(old+4);            /* Illegal instruction */
        *(new+9) = *(old+9);            /* Trace */
        *(new+47) = *(old+47);          /* Trap #15 */
        *(new+0x1f) = *(old+0x1f);      /* ABORT switch */
}


static void bvme6000_timer_int (int irq, void *dev_id, struct pt_regs *fp)
{
    volatile RtcPtr_t rtc = (RtcPtr_t)BVME_RTC_BASE;
    unsigned char msr = rtc->msr & 0xc0;

    rtc->msr = msr | 0x20;		/* Ack the interrupt */

    tick_handler(irq, dev_id, fp);
}

/*
 * Set up the RTC timer 1 to mode 2, so T1 output toggles every 5ms
 * (40000 x 125ns).  It will interrupt every 10ms, when T1 goes low.
 * So, when reading the elapsed time, you should read timer1,
 * subtract it from 39999, and then add 40000 if T1 is high.
 * That gives you the number of 125ns ticks in to the 10ms period,
 * so divide by 8 to get the microsecond result.
 */

void bvme6000_sched_init (void (*timer_routine)(int, void *, struct pt_regs *))
{
    volatile RtcPtr_t rtc = (RtcPtr_t)BVME_RTC_BASE;
    unsigned char msr = rtc->msr & 0xc0;

    rtc->msr = 0;	/* Ensure timer registers accessible */

    tick_handler = timer_routine;
    if (request_irq(BVME_IRQ_RTC, bvme6000_timer_int, 0,
				"timer", bvme6000_timer_int))
	panic ("Couldn't register timer int");

    rtc->t1cr_omr = 0x04;	/* Mode 2, ext clk */
    rtc->t1msb = 39999 >> 8;
    rtc->t1lsb = 39999 & 0xff;
    rtc->irr_icr1 &= 0xef;	/* Route timer 1 to INTR pin */
    rtc->msr = 0x40;		/* Access int.cntrl, etc */
    rtc->pfr_icr0 = 0x80;	/* Just timer 1 ints enabled */
    rtc->irr_icr1 = 0;
    rtc->t1cr_omr = 0x0a;	/* INTR+T1 active lo, push-pull */
    rtc->t0cr_rtmr &= 0xdf;	/* Stop timers in standby */
    rtc->msr = 0;		/* Access timer 1 control */
    rtc->t1cr_omr = 0x05;	/* Mode 2, ext clk, GO */

    rtc->msr = msr;

    if (request_irq(BVME_IRQ_ABORT, bvme6000_abort_int, 0,
				"abort", bvme6000_abort_int))
	panic ("Couldn't register abort int");
}


/* This is always executed with interrupts disabled.  */

/*
 * NOTE:  Don't accept any readings within 5us of rollover, as
 * the T1INT bit may be a little slow getting set.  There is also
 * a fault in the chip, meaning that reads may produce invalid
 * results...
 */

unsigned long bvme6000_gettimeoffset (void)
{
    volatile RtcPtr_t rtc = (RtcPtr_t)BVME_RTC_BASE;
    volatile PitRegsPtr pit = (PitRegsPtr)BVME_PIT_BASE;
    unsigned char msr = rtc->msr & 0xc0;
    unsigned char t1int, t1op;
    unsigned long v = 800000, ov;

    rtc->msr = 0;	/* Ensure timer registers accessible */

    do {
	ov = v;
	t1int = rtc->msr & 0x20;
	t1op  = pit->pcdr & 0x04;
	rtc->t1cr_omr |= 0x40;		/* Latch timer1 */
	v = rtc->t1msb << 8;		/* Read timer1 */
	v |= rtc->t1lsb;		/* Read timer1 */
    } while (t1int != (rtc->msr & 0x20) ||
		t1op != (pit->pcdr & 0x04) ||
			abs(ov-v) > 80 ||
				v > 39960);

    v = 39999 - v;
    if (!t1op)				/* If in second half cycle.. */
	v += 40000;
    v /= 8;				/* Convert ticks to microseconds */
    if (t1int)
	v += 10000;			/* Int pending, + 10ms */
    rtc->msr = msr;

    return v;
}

extern void bvme6000_gettod (int *year, int *mon, int *day, int *hour,
                           int *min, int *sec)
{
	volatile RtcPtr_t rtc = (RtcPtr_t)BVME_RTC_BASE;
	unsigned char msr = rtc->msr & 0xc0;

	rtc->msr = 0;		/* Ensure clock accessible */

	do {	/* Loop until we get a reading with a stable seconds field */
		*sec = bcd2bin (rtc->bcd_sec);
		*min = bcd2bin (rtc->bcd_min);
		*hour = bcd2bin (rtc->bcd_hr);
		*day = bcd2bin (rtc->bcd_dom);
		*mon = bcd2bin (rtc->bcd_mth);
		*year = bcd2bin (rtc->bcd_year);
	} while (bcd2bin (rtc->bcd_sec) != *sec);

	rtc->msr = msr;
}

static unsigned char bcd2bin (unsigned char b)
{
	return ((b>>4)*10 + (b&15));
}

static unsigned char bin2bcd (unsigned char b)
{
	return (((b/10)*16) + (b%10));
}


/*
 * Looks like op is non-zero for setting the clock, and zero for
 * reading the clock.
 *
 *  struct hwclk_time {
 *         unsigned        sec;       0..59
 *         unsigned        min;       0..59
 *         unsigned        hour;      0..23
 *         unsigned        day;       1..31
 *         unsigned        mon;       0..11
 *         unsigned        year;      00...
 *         int             wday;      0..6, 0 is Sunday, -1 means unknown/don't set
 * };
 */

int bvme6000_hwclk(int op, struct hwclk_time *t)
{
	volatile RtcPtr_t rtc = (RtcPtr_t)BVME_RTC_BASE;
	unsigned char msr = rtc->msr & 0xc0;

	rtc->msr = 0x40;	/* Ensure clock and real-time-mode-register
				 * are accessible */
	if (op)
	{	/* Write.... */
		rtc->t0cr_rtmr = t->year%4;
		rtc->bcd_tenms = 0;
		rtc->bcd_sec = bin2bcd(t->sec);
		rtc->bcd_min = bin2bcd(t->min);
		rtc->bcd_hr  = bin2bcd(t->hour);
		rtc->bcd_dom = bin2bcd(t->day);
		rtc->bcd_mth = bin2bcd(t->mon + 1);
		rtc->bcd_year = bin2bcd(t->year%100);
		if (t->wday >= 0)
			rtc->bcd_dow = bin2bcd(t->wday+1);
		rtc->t0cr_rtmr = t->year%4 | 0x08;
	}
	else
	{	/* Read....  */
		do {
			t->sec =  bcd2bin(rtc->bcd_sec);
			t->min =  bcd2bin(rtc->bcd_min);
			t->hour = bcd2bin(rtc->bcd_hr);
			t->day =  bcd2bin(rtc->bcd_dom);
			t->mon =  bcd2bin(rtc->bcd_mth)-1;
			t->year = bcd2bin(rtc->bcd_year);
			if (t->year < 70)
				t->year += 100;
			t->wday = bcd2bin(rtc->bcd_dow)-1;
		} while (t->sec != bcd2bin(rtc->bcd_sec));
	}

	rtc->msr = msr;

	return 0;
}

/*
 * Set the minutes and seconds from seconds value 'nowtime'.  Fail if
 * clock is out by > 30 minutes.  Logic lifted from atari code.
 * Algorithm is to wait for the 10ms register to change, and then to
 * wait a short while, and then set it.
 */

int bvme6000_set_clock_mmss (unsigned long nowtime)
{
	int retval = 0;
	short real_seconds = nowtime % 60, real_minutes = (nowtime / 60) % 60;
	unsigned char rtc_minutes, rtc_tenms;
	volatile RtcPtr_t rtc = (RtcPtr_t)BVME_RTC_BASE;
	unsigned char msr = rtc->msr & 0xc0;
	unsigned long flags;
	volatile int i;

	rtc->msr = 0;		/* Ensure clock accessible */
	rtc_minutes = bcd2bin (rtc->bcd_min);

	if ((rtc_minutes < real_minutes
		? real_minutes - rtc_minutes
			: rtc_minutes - real_minutes) < 30)
	{
		save_flags(flags);
		cli();
		rtc_tenms = rtc->bcd_tenms;
		while (rtc_tenms == rtc->bcd_tenms)
			;
		for (i = 0; i < 1000; i++)
			;
		rtc->bcd_min = bin2bcd(real_minutes);
		rtc->bcd_sec = bin2bcd(real_seconds);
		restore_flags(flags);
	}
	else
		retval = -1;

	rtc->msr = msr;

	return retval;
}


int bvme6000_keyb_init (void)
{
	return 0;
}

/*-------------------  Serial console stuff ------------------------*/

static void bvme_scc_write(struct console *co, const char *str, unsigned cnt);


void bvme6000_init_console_port (struct console *co, int cflag)
{
        co->write = bvme_scc_write;
}


static void scc_delay (void)
{
        int n;
	volatile int trash;

        for (n = 0; n < 20; n++)
		trash = n;
}

static void scc_write (char ch)
{
        volatile char *p = (volatile char *)BVME_SCC_A_ADDR;

        do {
                scc_delay();
        }
        while (!(*p & 4));
        scc_delay();
        *p = 8;
        scc_delay();
        *p = ch;
}


static void bvme_scc_write (struct console *co, const char *str, unsigned count)
{
        unsigned long   flags;

        save_flags(flags);
        cli();

        while (count--)
        {
                if (*str == '\n')
                        scc_write ('\r');
                scc_write (*str++);
        }
        restore_flags(flags);
}

