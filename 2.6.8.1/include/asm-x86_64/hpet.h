#ifndef _ASM_X8664_HPET_H
#define _ASM_X8664_HPET_H 1

/*
 * Documentation on HPET can be found at:
 *      http://www.intel.com/ial/home/sp/pcmmspec.htm
 *      ftp://download.intel.com/ial/home/sp/mmts098.pdf
 */

#define HPET_MMAP_SIZE	1024

#define HPET_ID		0x000
#define HPET_PERIOD	0x004
#define HPET_CFG	0x010
#define HPET_STATUS	0x020
#define HPET_COUNTER	0x0f0
#define HPET_T0_CFG	0x100
#define HPET_T0_CMP	0x108
#define HPET_T0_ROUTE	0x110
#define HPET_T1_CFG	0x120
#define HPET_T1_CMP	0x128
#define HPET_T1_ROUTE	0x130
#define HPET_T2_CFG	0x140
#define HPET_T2_CMP	0x148
#define HPET_T2_ROUTE	0x150

#define HPET_ID_VENDOR	0xffff0000
#define HPET_ID_LEGSUP	0x00008000
#define HPET_ID_NUMBER	0x00001f00
#define HPET_ID_REV	0x000000ff

#define HPET_ID_VENDOR_SHIFT	16
#define HPET_ID_VENDOR_8086	0x8086

#define HPET_CFG_ENABLE	0x001
#define HPET_CFG_LEGACY	0x002

#define HPET_TN_ENABLE		0x004
#define HPET_TN_PERIODIC	0x008
#define HPET_TN_PERIODIC_CAP	0x010
#define HPET_TN_SETVAL		0x040
#define HPET_TN_32BIT		0x100

extern int is_hpet_enabled(void);
extern int hpet_rtc_timer_init(void);

#ifdef CONFIG_HPET_EMULATE_RTC
extern int hpet_mask_rtc_irq_bit(unsigned long bit_mask);
extern int hpet_set_rtc_irq_bit(unsigned long bit_mask);
extern int hpet_set_alarm_time(unsigned char hrs, unsigned char min, unsigned char sec);
extern int hpet_set_periodic_freq(unsigned long freq);
extern int hpet_rtc_dropped_irq(void);
extern int hpet_rtc_timer_init(void);
#endif /* CONFIG_HPET_EMULATE_RTC */

#endif
