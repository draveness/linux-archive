#ifndef _M68K_MACHDEP_H
#define _M68K_MACHDEP_H

#include <linux/seq_file.h>
#include <linux/interrupt.h>

struct pt_regs;
struct mktime;
struct rtc_time;
struct rtc_pll_info;
struct buffer_head;

extern void (*mach_sched_init) (irqreturn_t (*handler)(int, void *, struct pt_regs *));
/* machine dependent irq functions */
extern void (*mach_init_IRQ) (void);
extern irqreturn_t (*(*mach_default_handler)[]) (int, void *, struct pt_regs *);
extern int (*mach_request_irq) (unsigned int irq, irqreturn_t (*handler)(int, void *, struct pt_regs *),
                                unsigned long flags, const char *devname, void *dev_id);
extern void (*mach_free_irq) (unsigned int irq, void *dev_id);
extern void (*mach_get_model) (char *model);
extern int (*mach_get_hardware_list) (char *buffer);
extern int (*mach_get_irq_list) (struct seq_file *p, void *v);
extern irqreturn_t (*mach_process_int) (int irq, struct pt_regs *fp);
/* machine dependent timer functions */
extern unsigned long (*mach_gettimeoffset)(void);
extern int (*mach_hwclk)(int, struct rtc_time*);
extern unsigned int (*mach_get_ss)(void);
extern int (*mach_get_rtc_pll)(struct rtc_pll_info *);
extern int (*mach_set_rtc_pll)(struct rtc_pll_info *);
extern int (*mach_set_clock_mmss)(unsigned long);
extern void (*mach_reset)( void );
extern void (*mach_halt)( void );
extern void (*mach_power_off)( void );
extern unsigned long (*mach_hd_init) (unsigned long, unsigned long);
extern void (*mach_hd_setup)(char *, int *);
extern long mach_max_dma_address;
extern void (*mach_floppy_setup)(char *, int *);
extern void (*mach_heartbeat) (int);
extern void (*mach_l2_flush) (int);
extern void (*mach_beep) (unsigned int, unsigned int);

#endif /* _M68K_MACHDEP_H */
