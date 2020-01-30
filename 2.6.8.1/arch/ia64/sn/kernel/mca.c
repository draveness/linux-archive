/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (c) 2000-2004 Silicon Graphics, Inc.  All Rights Reserved.
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/vmalloc.h>
#include <asm/sn/sgi.h>
#include <asm/mca.h>
#include <asm/sal.h>
#include <asm/sn/sn_sal.h>



/*
 * Interval for calling SAL to poll for errors that do NOT cause error
 * interrupts. SAL will raise a CPEI if any errors are present that
 * need to be logged.
 */
#define CPEI_INTERVAL	(5*HZ)


struct timer_list sn_cpei_timer;
void sn_init_cpei_timer(void);

/* Printing oemdata from mca uses data that is not passed through SAL, it is
 * global.  Only one user at a time.
 */
static DECLARE_MUTEX(sn_oemdata_mutex);
static u8 **sn_oemdata;
static u64 *sn_oemdata_size, sn_oemdata_bufsize;

/*
 * print_hook
 *
 * This function is the callback routine that SAL calls to log error
 * info for platform errors.  buf is appended to sn_oemdata, resizing as
 * required.
 */
static int
print_hook(const char *fmt, ...)
{
	char buf[400];
	int len;
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	len = strlen(buf);
	while (*sn_oemdata_size + len + 1 > sn_oemdata_bufsize) {
		u8 *newbuf = vmalloc(sn_oemdata_bufsize += 1000);
		if (!newbuf) {
			printk(KERN_ERR "%s: unable to extend sn_oemdata\n", __FUNCTION__);
			return 0;
		}
		memcpy(newbuf, *sn_oemdata, *sn_oemdata_size);
		vfree(*sn_oemdata);
		*sn_oemdata = newbuf;
	}
	memcpy(*sn_oemdata + *sn_oemdata_size, buf, len + 1);
	*sn_oemdata_size += len;
	return 0;
}


static void
sn_cpei_handler(int irq, void *devid, struct pt_regs *regs)
{
	/*
	 * this function's sole purpose is to call SAL when we receive
	 * a CE interrupt from SHUB or when the timer routine decides
	 * we need to call SAL to check for CEs.
	 */

	/* CALL SAL_LOG_CE */

	ia64_sn_plat_cpei_handler();
}


static void
sn_cpei_timer_handler(unsigned long dummy)
{
	sn_cpei_handler(-1, NULL, NULL);
	mod_timer(&sn_cpei_timer, jiffies + CPEI_INTERVAL);
}

void
sn_init_cpei_timer(void)
{
	init_timer(&sn_cpei_timer);
	sn_cpei_timer.expires = jiffies + CPEI_INTERVAL;
	sn_cpei_timer.function = sn_cpei_timer_handler;
	add_timer(&sn_cpei_timer);
}

static int
sn_platform_plat_specific_err_print(const u8 *sect_header, u8 **oemdata, u64 *oemdata_size)
{
	sal_log_plat_specific_err_info_t *psei = (sal_log_plat_specific_err_info_t *)sect_header;
	if (!psei->valid.oem_data)
		return 0;
	down(&sn_oemdata_mutex);
	sn_oemdata = oemdata;
	sn_oemdata_size = oemdata_size;
	sn_oemdata_bufsize = 0;
	ia64_sn_plat_specific_err_print(print_hook, (char *)psei);
	up(&sn_oemdata_mutex);
	return 0;
}

/* Callback when userspace salinfo wants to decode oem data via the platform
 * kernel and/or prom.
 */
int sn_salinfo_platform_oemdata(const u8 *sect_header, u8 **oemdata, u64 *oemdata_size)
{
	efi_guid_t guid = *(efi_guid_t *)sect_header;
	*oemdata_size = 0;
	vfree(*oemdata);
	*oemdata = NULL;
	if (efi_guidcmp(guid, SAL_PLAT_SPECIFIC_ERR_SECT_GUID) == 0 ||
	    efi_guidcmp(guid, SAL_PLAT_MEM_DEV_ERR_SECT_GUID) == 0)
		return sn_platform_plat_specific_err_print(sect_header, oemdata, oemdata_size);
	return 0;
}

static int __init sn_salinfo_init(void)
{
	salinfo_platform_oemdata = &sn_salinfo_platform_oemdata;
	return 0;
}

module_init(sn_salinfo_init)
