#include <linux/config.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/user.h>
#include <linux/sched.h>
#include <linux/in6.h>
#include <linux/interrupt.h>
#include <linux/smp_lock.h>
#include <linux/pm.h>
#include <linux/pci.h>
#include <linux/apm_bios.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/tty.h>
#include <linux/ioctl32.h>

#include <asm/semaphore.h>
#include <asm/processor.h>
#include <asm/i387.h>
#include <asm/uaccess.h>
#include <asm/checksum.h>
#include <asm/io.h>
#include <asm/hardirq.h>
#include <asm/delay.h>
#include <asm/irq.h>
#include <asm/mmx.h>
#include <asm/desc.h>
#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/nmi.h>
#include <asm/kdebug.h>
#include <asm/unistd.h>
#include <asm/delay.h>
#include <asm/tlbflush.h>

extern spinlock_t rtc_lock;

#ifdef CONFIG_SMP
extern void __write_lock_failed(rwlock_t *rw);
extern void __read_lock_failed(rwlock_t *rw);
#endif

#if defined(CONFIG_BLK_DEV_IDE) || defined(CONFIG_BLK_DEV_HD) || defined(CONFIG_BLK_DEV_IDE_MODULE) || defined(CONFIG_BLK_DEV_HD_MODULE)
extern struct drive_info_struct drive_info;
EXPORT_SYMBOL(drive_info);
#endif

extern unsigned long get_cmos_time(void);

/* platform dependent support */
EXPORT_SYMBOL(boot_cpu_data);
//EXPORT_SYMBOL(dump_fpu);
EXPORT_SYMBOL(__ioremap);
EXPORT_SYMBOL(ioremap_nocache);
EXPORT_SYMBOL(iounmap);
EXPORT_SYMBOL(enable_irq);
EXPORT_SYMBOL(disable_irq);
EXPORT_SYMBOL(disable_irq_nosync);
EXPORT_SYMBOL(probe_irq_mask);
EXPORT_SYMBOL(kernel_thread);
EXPORT_SYMBOL(pm_idle);
EXPORT_SYMBOL(pm_power_off);
EXPORT_SYMBOL(get_cmos_time);

EXPORT_SYMBOL_NOVERS(__down_failed);
EXPORT_SYMBOL_NOVERS(__down_failed_interruptible);
EXPORT_SYMBOL_NOVERS(__down_failed_trylock);
EXPORT_SYMBOL_NOVERS(__up_wakeup);
/* Networking helper routines. */
EXPORT_SYMBOL(csum_partial_copy_nocheck);
EXPORT_SYMBOL(ip_compute_csum);
/* Delay loops */
EXPORT_SYMBOL(__udelay);
EXPORT_SYMBOL(__ndelay);
EXPORT_SYMBOL(__delay);
EXPORT_SYMBOL(__const_udelay);

EXPORT_SYMBOL_NOVERS(__get_user_1);
EXPORT_SYMBOL_NOVERS(__get_user_2);
EXPORT_SYMBOL_NOVERS(__get_user_4);
EXPORT_SYMBOL_NOVERS(__get_user_8);
EXPORT_SYMBOL_NOVERS(__put_user_1);
EXPORT_SYMBOL_NOVERS(__put_user_2);
EXPORT_SYMBOL_NOVERS(__put_user_4);
EXPORT_SYMBOL_NOVERS(__put_user_8);

EXPORT_SYMBOL(strpbrk);
EXPORT_SYMBOL(strstr);

EXPORT_SYMBOL(strncpy_from_user);
EXPORT_SYMBOL(__strncpy_from_user);
EXPORT_SYMBOL(clear_user);
EXPORT_SYMBOL(__clear_user);
EXPORT_SYMBOL(copy_user_generic);
EXPORT_SYMBOL(copy_from_user);
EXPORT_SYMBOL(copy_to_user);
EXPORT_SYMBOL(copy_in_user);
EXPORT_SYMBOL(strnlen_user);

#ifdef CONFIG_PCI
EXPORT_SYMBOL(pci_alloc_consistent);
EXPORT_SYMBOL(pci_free_consistent);
#endif

#ifdef CONFIG_PCI
EXPORT_SYMBOL(pcibios_penalize_isa_irq);
EXPORT_SYMBOL(pci_mem_start);
#endif

#ifdef CONFIG_X86_USE_3DNOW
EXPORT_SYMBOL(_mmx_memcpy);
EXPORT_SYMBOL(mmx_clear_page);
EXPORT_SYMBOL(mmx_copy_page);
#endif

EXPORT_SYMBOL(cpu_pda);
#ifdef CONFIG_SMP
EXPORT_SYMBOL(cpu_data);
EXPORT_SYMBOL(cpu_online_map);
EXPORT_SYMBOL_NOVERS(__write_lock_failed);
EXPORT_SYMBOL_NOVERS(__read_lock_failed);

EXPORT_SYMBOL(synchronize_irq);
EXPORT_SYMBOL(smp_call_function);
EXPORT_SYMBOL(cpu_callout_map);
#endif

#ifdef CONFIG_VT
EXPORT_SYMBOL(screen_info);
#endif

EXPORT_SYMBOL(get_wchan);

EXPORT_SYMBOL(rtc_lock);

EXPORT_SYMBOL_GPL(set_nmi_callback);
EXPORT_SYMBOL_GPL(unset_nmi_callback);

/* Export string functions. We normally rely on gcc builtin for most of these,
   but gcc sometimes decides not to inline them. */    
#undef memcpy
#undef memset
#undef memmove
#undef memchr
#undef strlen
#undef strcpy
#undef strncmp
#undef strncpy
#undef strchr	
#undef strcmp 
#undef strcpy 
#undef strcat
#undef memcmp

extern void * memset(void *,int,__kernel_size_t);
extern size_t strlen(const char *);
extern void * memmove(void * dest,const void *src,size_t count);
extern char * strcpy(char * dest,const char *src);
extern int strcmp(const char * cs,const char * ct);
extern void *memchr(const void *s, int c, size_t n);
extern void * memcpy(void *,const void *,__kernel_size_t);
extern void * __memcpy(void *,const void *,__kernel_size_t);
extern char * strcat(char *, const char *);
extern int memcmp(const void * cs,const void * ct,size_t count);

EXPORT_SYMBOL_NOVERS(memset);
EXPORT_SYMBOL_NOVERS(strlen);
EXPORT_SYMBOL_NOVERS(memmove);
EXPORT_SYMBOL_NOVERS(strcpy);
EXPORT_SYMBOL_NOVERS(strncmp);
EXPORT_SYMBOL_NOVERS(strncpy);
EXPORT_SYMBOL_NOVERS(strchr);
EXPORT_SYMBOL_NOVERS(strcmp); 
EXPORT_SYMBOL_NOVERS(strcat);
EXPORT_SYMBOL_NOVERS(strncat);
EXPORT_SYMBOL_NOVERS(memchr);
EXPORT_SYMBOL_NOVERS(strrchr);
EXPORT_SYMBOL_NOVERS(strnlen);
EXPORT_SYMBOL_NOVERS(memscan);
EXPORT_SYMBOL_NOVERS(memcpy);
EXPORT_SYMBOL_NOVERS(__memcpy);
EXPORT_SYMBOL_NOVERS(memcmp);

/* syscall export needed for misdesigned sound drivers. */
EXPORT_SYMBOL(sys_read);
EXPORT_SYMBOL(sys_lseek);
EXPORT_SYMBOL(sys_open);

EXPORT_SYMBOL(empty_zero_page);

#ifdef CONFIG_HAVE_DEC_LOCK
EXPORT_SYMBOL(atomic_dec_and_lock);
#endif

EXPORT_SYMBOL(die_chain);

#ifdef CONFIG_SMP
EXPORT_SYMBOL(cpu_sibling_map);
EXPORT_SYMBOL(smp_num_siblings);
#endif

extern void do_softirq_thunk(void);
EXPORT_SYMBOL_NOVERS(do_softirq_thunk);

void out_of_line_bug(void);
EXPORT_SYMBOL(out_of_line_bug);

EXPORT_SYMBOL(init_level4_pgt);

extern unsigned long __supported_pte_mask;
EXPORT_SYMBOL(__supported_pte_mask);

EXPORT_SYMBOL(clear_page);

#ifdef CONFIG_SMP
EXPORT_SYMBOL(flush_tlb_page);
EXPORT_SYMBOL_GPL(flush_tlb_all);
#endif

