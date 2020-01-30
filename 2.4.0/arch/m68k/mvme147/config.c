/*
 *  arch/m68k/mvme147/config.c
 *
 *  Copyright (C) 1996 Dave Frascone [chaos@mindspring.com]
 *  Cloned from        Richard Hirst [richard@sleepie.demon.co.uk]
 *
 * Based on:
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
#include <asm/mvme147hw.h>


extern void mvme147_process_int (int level, struct pt_regs *regs);
extern void mvme147_init_IRQ (void);
extern void mvme147_free_irq (unsigned int, void *);
extern int  mvme147_get_irq_list (char *);
extern void mvme147_enable_irq (unsigned int);
extern void mvme147_disable_irq (unsigned int);
static void mvme147_get_model(char *model);
static int  mvme147_get_hardware_list(char *buffer);
extern int mvme147_request_irq (unsigned int irq, void (*handler)(int, void *, struct pt_regs *), unsigned long flags, const char *devname, void *dev_id);
extern void mvme147_sched_init(void (*handler)(int, void *, struct pt_regs *));
extern int mvme147_keyb_init(void);
extern int mvme147_kbdrate (struct kbd_repeat *);
extern unsigned long mvme147_gettimeoffset (void);
extern void mvme147_gettod (int *year, int *mon, int *day, int *hour,
                           int *min, int *sec);
extern int mvme147_hwclk (int, struct hwclk_time *);
extern int mvme147_set_clock_mmss (unsigned long);
extern void mvme147_check_partition (struct gendisk *hd, unsigned int dev);
extern void mvme147_reset (void);
extern void mvme147_waitbut(void);


static int bcd2int (unsigned char b);

/* Save tick handler routine pointer, will point to do_timer() in
 * kernel/sched.c, called via mvme147_process_int() */

void (*tick_handler)(int, void *, struct pt_regs *);


int mvme147_parse_bootinfo(const struct bi_record *bi)
{
	if (bi->tag == BI_VME_TYPE || bi->tag == BI_VME_BRDINFO)
		return 0;
	else
		return 1;
}

int mvme147_kbdrate (struct kbd_repeat *k)
{
	return 0;
}

void mvme147_reset()
{
	printk ("\r\n\nCalled mvme147_reset\r\n");
	m147_pcc->watchdog = 0x0a;	/* Clear timer */
	m147_pcc->watchdog = 0xa5;	/* Enable watchdog - 100ms to reset */
	while (1)
		;
}

static void mvme147_get_model(char *model)
{
	sprintf(model, "Motorola MVME147");
}


static int mvme147_get_hardware_list(char *buffer)
{
	*buffer = '\0';

	return 0;
}


void __init config_mvme147(void)
{
	mach_max_dma_address	= 0x01000000;
	mach_sched_init		= mvme147_sched_init;
	mach_keyb_init		= mvme147_keyb_init;
	mach_kbdrate		= mvme147_kbdrate;
	mach_init_IRQ		= mvme147_init_IRQ;
	mach_gettimeoffset	= mvme147_gettimeoffset;
	mach_gettod		= mvme147_gettod;
	mach_hwclk		= mvme147_hwclk;
	mach_set_clock_mmss	= mvme147_set_clock_mmss;
	mach_reset		= mvme147_reset;
	mach_free_irq		= mvme147_free_irq;
	mach_process_int	= mvme147_process_int;
	mach_get_irq_list	= mvme147_get_irq_list;
	mach_request_irq	= mvme147_request_irq;
	enable_irq		= mvme147_enable_irq;
	disable_irq		= mvme147_disable_irq;
	mach_get_model		= mvme147_get_model;
	mach_get_hardware_list	= mvme147_get_hardware_list;

	/* Board type is only set by newer versions of vmelilo/tftplilo */
	if (!vme_brdtype)
		vme_brdtype = VME_TYPE_MVME147;
}


/* Using pcc tick timer 1 */

static void mvme147_timer_int (int irq, void *dev_id, struct pt_regs *fp)
{
	m147_pcc->t1_int_cntrl = PCC_TIMER_INT_CLR;  
	m147_pcc->t1_int_cntrl = PCC_INT_ENAB|PCC_LEVEL_TIMER1;   
	tick_handler(irq, dev_id, fp);
}


void mvme147_sched_init (void (*timer_routine)(int, void *, struct pt_regs *))
{
	tick_handler = timer_routine;
	request_irq (PCC_IRQ_TIMER1, mvme147_timer_int, 
		IRQ_FLG_REPLACE, "timer 1", NULL);
	
	/* Init the clock with a value */
	/* our clock goes off every 6.25us */
	m147_pcc->t1_preload = PCC_TIMER_PRELOAD;
	m147_pcc->t1_cntrl = 0x0;   	/* clear timer */
	m147_pcc->t1_cntrl = 0x3; 	/* start timer */
	m147_pcc->t1_int_cntrl = PCC_TIMER_INT_CLR;  /* clear pending ints */
	m147_pcc->t1_int_cntrl = PCC_INT_ENAB|PCC_LEVEL_TIMER1;   
}

/* This is always executed with interrupts disabled.  */
/* XXX There are race hazards in this code XXX */
unsigned long mvme147_gettimeoffset (void)
{
	volatile unsigned short *cp = (volatile unsigned short *)0xfffe1012;
	unsigned short n;

	n = *cp;
	while (n != *cp)
		n = *cp;

	n -= PCC_TIMER_PRELOAD;
	return (unsigned long)n * 25 / 4;
}

extern void mvme147_gettod (int *year, int *mon, int *day, int *hour,
                           int *min, int *sec)
{
	m147_rtc->ctrl = RTC_READ;
	*year = bcd2int (m147_rtc->bcd_year);
	*mon = bcd2int (m147_rtc->bcd_mth);
	*day = bcd2int (m147_rtc->bcd_dom);
	*hour = bcd2int (m147_rtc->bcd_hr);
	*min = bcd2int (m147_rtc->bcd_min);
	*sec = bcd2int (m147_rtc->bcd_sec);
	m147_rtc->ctrl = 0;
}

static int bcd2int (unsigned char b)
{
	return ((b>>4)*10 + (b&15));
}

int mvme147_hwclk(int op, struct hwclk_time *t)
{
	return 0;
}

int mvme147_set_clock_mmss (unsigned long nowtime)
{
	return 0;
}

int mvme147_keyb_init (void)
{
	return 0;
}

/*-------------------  Serial console stuff ------------------------*/

static void scc_delay (void)
{
	int n;
	volatile int trash;

	for (n = 0; n < 20; n++)
		trash = n;
}

static void scc_write (char ch)
{
	volatile char *p = (volatile char *)M147_SCC_A_ADDR;

	do {
		scc_delay();
	}
	while (!(*p & 4));
	scc_delay();
	*p = 8;
	scc_delay();
	*p = ch;
}


void m147_scc_write (struct console *co, const char *str, unsigned count)
{
	unsigned long	flags;

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


static int m147_scc_wait_key (struct console *co)
{
	volatile unsigned char *p = (volatile char *)M147_SCC_A_ADDR;
	unsigned long	flags;
	int		c;

	/* wait for rx buf filled */
	while ((*p & 0x01) == 0)
		;

	save_flags(flags);
	cli();

	*p = 8;
	scc_delay();
	c = *p;

	restore_flags(flags);
	return c;
}


void mvme147_init_console_port (struct console *co, int cflag)
{
	co->write    = m147_scc_write;
	co->wait_key = m147_scc_wait_key;
}
