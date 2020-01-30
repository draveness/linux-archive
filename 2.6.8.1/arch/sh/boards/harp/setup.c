/*
 * arch/sh/stboard/setup.c
 *
 * Copyright (C) 2001 Stuart Menefy (stuart.menefy@st.com)
 *
 * May be copied or modified under the terms of the GNU General Public
 * License.  See linux/COPYING for more information.
 *
 * STMicroelectronics ST40STB1 HARP and compatible support.
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <asm/io.h>
#include <asm/harp/harp.h>

const char *get_system_type(void)
{
	return "STB1 Harp";
}

/*
 * Initialize the board
 */
int __init platform_setup(void)
{
#ifdef CONFIG_SH_STB1_HARP
	unsigned long ic8_version, ic36_version;

	ic8_version = ctrl_inl(EPLD_REVID2);
	ic36_version = ctrl_inl(EPLD_REVID1);

        printk("STMicroelectronics STB1 HARP initialisaton\n");
        printk("EPLD versions: IC8: %d.%02d, IC36: %d.%02d\n",
               (ic8_version >> 4) & 0xf, ic8_version & 0xf,
               (ic36_version >> 4) & 0xf, ic36_version & 0xf);
#elif defined(CONFIG_SH_STB1_OVERDRIVE)
	unsigned long version;

	version = ctrl_inl(EPLD_REVID);

        printk("STMicroelectronics STB1 Overdrive initialisaton\n");
        printk("EPLD version: %d.%02d\n",
	       (version >> 4) & 0xf, version & 0xf);
#else
#error Undefined machine
#endif
 
        /* Currently all STB1 chips have problems with the sleep instruction,
         * so disable it here.
         */
	disable_hlt();

	return 0;
}
