#ifdef __KERNEL__
#ifndef _PPC64_MACHDEP_H
#define _PPC64_MACHDEP_H

/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/config.h>
#include <linux/seq_file.h>
#include <linux/init.h>
#include <linux/dma-mapping.h>

#include <asm/setup.h>

struct pt_regs;
struct pci_bus;	
struct device_node;
struct iommu_table;
struct rtc_time;

#ifdef CONFIG_SMP
struct smp_ops_t {
	void  (*message_pass)(int target, int msg);
	int   (*probe)(void);
	void  (*kick_cpu)(int nr);
	void  (*setup_cpu)(int nr);
	void  (*take_timebase)(void);
	void  (*give_timebase)(void);
};
#endif

struct machdep_calls {
	void            (*hpte_invalidate)(unsigned long slot,
					   unsigned long va,
					   int large,
					   int local);
	long		(*hpte_updatepp)(unsigned long slot, 
					 unsigned long newpp, 
					 unsigned long va,
					 int large,
					 int local);
	void            (*hpte_updateboltedpp)(unsigned long newpp, 
					       unsigned long ea);
	long		(*hpte_insert)(unsigned long hpte_group,
				       unsigned long va,
				       unsigned long prpn,
				       int secondary, 
				       unsigned long hpteflags, 
				       int bolted,
				       int large);
	long		(*hpte_remove)(unsigned long hpte_group);
	void		(*flush_hash_range)(unsigned long context,
					    unsigned long number,
					    int local);

	void		(*tce_build)(struct iommu_table * tbl,
				     long index,
				     long npages,
				     unsigned long uaddr,
				     enum dma_data_direction direction);
	void		(*tce_free)(struct iommu_table *tbl,
				    long index,
				    long npages);
	void		(*tce_flush)(struct iommu_table *tbl);

	void		(*setup_arch)(void);
	/* Optional, may be NULL. */
	void		(*get_cpuinfo)(struct seq_file *m);

	void		(*init_IRQ)(void);
	int		(*get_irq)(struct pt_regs *);

	/* PCI stuff */
	void		(*pcibios_fixup)(void);

	/* Optional, may be NULL. */
	void		(*init)(void);

	void		(*restart)(char *cmd);
	void		(*power_off)(void);
	void		(*halt)(void);
	void		(*panic)(char *str);

	int		(*set_rtc_time)(struct rtc_time *);
	void		(*get_rtc_time)(struct rtc_time *);
	void		(*get_boot_time)(struct rtc_time *);

	void		(*calibrate_decr)(void);

	void		(*progress)(char *, unsigned short);

	/* Debug interface.  Low level I/O to some terminal device */
	void		(*udbg_putc)(unsigned char c);
	unsigned char	(*udbg_getc)(void);
	int		(*udbg_getc_poll)(void);

	/* Interface for platform error logging */
	void 		(*log_error)(char *buf, unsigned int err_type, int fatal);

	ssize_t		(*nvram_write)(char *buf, size_t count, loff_t *index);
	ssize_t		(*nvram_read)(char *buf, size_t count, loff_t *index);	
	ssize_t		(*nvram_size)(void);		
	int		(*nvram_sync)(void);

	/* Motherboard/chipset features. This is a kind of general purpose
	 * hook used to control some machine specific features (like reset
	 * lines, chip power control, etc...).
	 */
	long	 	(*feature_call)(unsigned int feature, ...);

};

extern struct machdep_calls ppc_md;
extern char cmd_line[COMMAND_LINE_SIZE];

/* Functions to produce codes on the leds.
 * The SRC code should be unique for the message category and should
 * be limited to the lower 24 bits (the upper 8 are set by these funcs),
 * and (for boot & dump) should be sorted numerically in the order
 * the events occur.
 */
/* Print a boot progress message. */
void ppc64_boot_msg(unsigned int src, const char *msg);
/* Print a termination message (print only -- does not stop the kernel) */
void ppc64_terminate_msg(unsigned int src, const char *msg);
/* Print something that needs attention (device error, etc) */
void ppc64_attention_msg(unsigned int src, const char *msg);
/* Print a dump progress message. */
void ppc64_dump_msg(unsigned int src, const char *msg);

static inline void log_error(char *buf, unsigned int err_type, int fatal)
{
	if (ppc_md.log_error)
		ppc_md.log_error(buf, err_type, fatal);
}

#endif /* _PPC64_MACHDEP_H */
#endif /* __KERNEL__ */
