/*
 * linux/arch/sh/kernel/led_se.c
 *
 * Copyright (C) 2000 Stuart Menefy <stuart.menefy@st.com>
 *
 * May be copied or modified under the terms of the GNU General Public
 * License.  See linux/COPYING for more information.
 *
 * This file contains Solution Engine specific LED code.
 */

#include <linux/config.h>
#include <asm/se/se.h>

static void mach_led(int position, int value)
{
	volatile unsigned short* p = (volatile unsigned short*)PA_LED;

	if (value) {
		*p |= (1<<8);
	} else {
		*p &= ~(1<<8);
	}
}

#ifdef CONFIG_HEARTBEAT

#include <linux/sched.h>

/* Cycle the LED's in the clasic Knightrider/Sun pattern */
void heartbeat_se(void)
{
	static unsigned int cnt = 0, period = 0;
	volatile unsigned short* p = (volatile unsigned short*)PA_LED;
	static unsigned bit = 0, up = 1;

	cnt += 1;
	if (cnt < period) {
		return;
	}

	cnt = 0;

	/* Go through the points (roughly!):
	 * f(0)=10, f(1)=16, f(2)=20, f(5)=35,f(inf)->110
	 */
	period = 110 - ( (300<<FSHIFT)/
			 ((avenrun[0]/5) + (3<<FSHIFT)) );

	if (up) {
		if (bit == 7) {
			bit--;
			up=0;
		} else {
			bit ++;
		}
	} else {
		if (bit == 0) {
			bit++;
			up=1;
		} else {
			bit--;
		}
	}
	*p = 1<<(bit+8);

}
#endif /* CONFIG_HEARTBEAT */
