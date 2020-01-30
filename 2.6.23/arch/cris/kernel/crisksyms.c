#include <linux/module.h>
#include <linux/user.h>
#include <linux/elfcore.h>
#include <linux/sched.h>
#include <linux/in6.h>
#include <linux/interrupt.h>
#include <linux/pm.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/tty.h>

#include <asm/semaphore.h>
#include <asm/processor.h>
#include <asm/uaccess.h>
#include <asm/checksum.h>
#include <asm/io.h>
#include <asm/delay.h>
#include <asm/irq.h>
#include <asm/pgtable.h>
#include <asm/fasttimer.h>

extern unsigned long get_cmos_time(void);
extern void __Udiv(void);
extern void __Umod(void);
extern void __Div(void);
extern void __Mod(void);
extern void __ashldi3(void);
extern void __ashrdi3(void);
extern void __lshrdi3(void);
extern void iounmap(volatile void * __iomem);

/* Platform dependent support */
EXPORT_SYMBOL(kernel_thread);
EXPORT_SYMBOL(get_cmos_time);
EXPORT_SYMBOL(loops_per_usec);

/* String functions */
EXPORT_SYMBOL(memcmp);
EXPORT_SYMBOL(memmove);
EXPORT_SYMBOL(strstr);
EXPORT_SYMBOL(strcpy);
EXPORT_SYMBOL(strchr);
EXPORT_SYMBOL(strcmp);
EXPORT_SYMBOL(strlen);
EXPORT_SYMBOL(strcat);
EXPORT_SYMBOL(strncat);
EXPORT_SYMBOL(strncmp);
EXPORT_SYMBOL(strncpy);

/* Math functions */
EXPORT_SYMBOL(__Udiv);
EXPORT_SYMBOL(__Umod);
EXPORT_SYMBOL(__Div);
EXPORT_SYMBOL(__Mod);
EXPORT_SYMBOL(__ashldi3);
EXPORT_SYMBOL(__ashrdi3);
EXPORT_SYMBOL(__lshrdi3);

/* Memory functions */
EXPORT_SYMBOL(__ioremap);
EXPORT_SYMBOL(iounmap);

/* Semaphore functions */
EXPORT_SYMBOL(__up);
EXPORT_SYMBOL(__down);
EXPORT_SYMBOL(__down_interruptible);
EXPORT_SYMBOL(__down_trylock);

/* Userspace access functions */
EXPORT_SYMBOL(__copy_user_zeroing);
EXPORT_SYMBOL(__copy_user);

#undef memcpy
#undef memset
extern void * memset(void *, int, __kernel_size_t);
extern void * memcpy(void *, const void *, __kernel_size_t);
EXPORT_SYMBOL(memcpy);
EXPORT_SYMBOL(memset);

#ifdef CONFIG_ETRAX_FAST_TIMER
/* Fast timer functions */
EXPORT_SYMBOL(fast_timer_list);
EXPORT_SYMBOL(start_one_shot_timer);
EXPORT_SYMBOL(del_fast_timer);
EXPORT_SYMBOL(schedule_usleep);
#endif

