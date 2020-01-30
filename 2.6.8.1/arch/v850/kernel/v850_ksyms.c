#include <linux/module.h>
#include <linux/linkage.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/user.h>
#include <linux/elfcore.h>
#include <linux/in6.h>
#include <linux/interrupt.h>
#include <linux/config.h>

#include <asm/pgalloc.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/semaphore.h>
#include <asm/checksum.h>
#include <asm/hardirq.h>
#include <asm/current.h>


extern void *trap_table;
EXPORT_SYMBOL (trap_table);

/* platform dependent support */
extern void dump_thread (struct pt_regs *, struct user *);
EXPORT_SYMBOL (dump_thread);
EXPORT_SYMBOL (kernel_thread);
EXPORT_SYMBOL (enable_irq);
EXPORT_SYMBOL (disable_irq);
EXPORT_SYMBOL (disable_irq_nosync);
EXPORT_SYMBOL (__bug);

/* Networking helper routines. */
EXPORT_SYMBOL (csum_partial_copy);
EXPORT_SYMBOL (csum_partial_copy_from_user);
EXPORT_SYMBOL (ip_compute_csum);
EXPORT_SYMBOL (ip_fast_csum);

/* string / mem functions */
EXPORT_SYMBOL_NOVERS (strcpy);
EXPORT_SYMBOL_NOVERS (strncpy);
EXPORT_SYMBOL_NOVERS (strcat);
EXPORT_SYMBOL_NOVERS (strncat);
EXPORT_SYMBOL_NOVERS (strcmp);
EXPORT_SYMBOL_NOVERS (strncmp);
EXPORT_SYMBOL_NOVERS (strchr);
EXPORT_SYMBOL_NOVERS (strlen);
EXPORT_SYMBOL_NOVERS (strnlen);
EXPORT_SYMBOL_NOVERS (strpbrk);
EXPORT_SYMBOL_NOVERS (strrchr);
EXPORT_SYMBOL_NOVERS (strstr);
EXPORT_SYMBOL_NOVERS (memset);
EXPORT_SYMBOL_NOVERS (memcpy);
EXPORT_SYMBOL_NOVERS (memmove);
EXPORT_SYMBOL_NOVERS (memcmp);
EXPORT_SYMBOL_NOVERS (memscan);

/* semaphores */
EXPORT_SYMBOL_NOVERS (__down);
EXPORT_SYMBOL_NOVERS (__down_interruptible);
EXPORT_SYMBOL_NOVERS (__down_trylock);
EXPORT_SYMBOL_NOVERS (__up);

/*
 * libgcc functions - functions that are used internally by the
 * compiler...  (prototypes are not correct though, but that
 * doesn't really matter since they're not versioned).
 */
extern void __ashldi3 (void);
extern void __ashrdi3 (void);
extern void __lshrdi3 (void);
extern void __muldi3 (void);
extern void __negdi2 (void);

EXPORT_SYMBOL_NOVERS (__ashldi3);
EXPORT_SYMBOL_NOVERS (__ashrdi3);
EXPORT_SYMBOL_NOVERS (__lshrdi3);
EXPORT_SYMBOL_NOVERS (__muldi3);
EXPORT_SYMBOL_NOVERS (__negdi2);
