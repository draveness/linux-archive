/*
 *  Registers for the SGS-Thomson M48T35 Timekeeper RAM chip
 */

#ifndef _ASM_M48T35_H
#define _ASM_M48T35_H

struct m48t35_rtc {
        volatile u8         pad[0x7ff8];    /* starts at 0x7ff8 */
        volatile u8         control;
        volatile u8         sec;
        volatile u8         min;
        volatile u8         hour;
        volatile u8         day;
        volatile u8         date;
        volatile u8         month;
        volatile u8         year;
};

#define M48T35_RTC_SET      0x80
#define M48T35_RTC_STOPPED  0x80
#define M48T35_RTC_READ     0x40

#ifndef BCD_TO_BIN
#define BCD_TO_BIN(x)   ((x)=((x)&15) + ((x)>>4)*10)
#endif

#ifndef BIN_TO_BCD
#define BIN_TO_BCD(x)   ((x)=(((x)/10)<<4) + (x)%10)
#endif

#endif
