/* $Id: sgialib.h,v 1.5 2000/03/19 01:28:58 ralf Exp $
 * sgialib.h: SGI ARCS firmware interface library for the Linux kernel.
 *
 * Copyright (C) 1996 David S. Miller (dm@engr.sgi.com)
 */

#ifndef _ASM_SGIALIB_H
#define _ASM_SGIALIB_H

#include <asm/sgiarcs.h>

extern struct linux_promblock *sgi_pblock;
extern struct linux_romvec *romvec;
extern int prom_argc;
extern char **prom_argv, **prom_envp;

extern int prom_flags;
#define PROM_FLAG_ARCS  1

/*
 * Init the PROM library and it's internal data structures.  Called
 * at boot time from head.S before start_kernel is invoked.
 */
extern int prom_init(int argc, char **argv, char **envp, int *prom_vec);

/* Simple char-by-char console I/O. */
extern void prom_putchar(char c);
extern char prom_getchar(void);

/* Generic printf() using ARCS console I/O. */
extern void prom_printf(char *fmt, ...);

/* Memory descriptor management. */
#define PROM_MAX_PMEMBLOCKS    32
struct prom_pmemblock {
	unsigned long base; /* Within KSEG0. */
	unsigned int size;  /* In bytes. */
        unsigned int type;  /* free or prom memory */
};

/* Get next memory descriptor after CURR, returns first descriptor
 * in chain is CURR is NULL.
 */
extern struct linux_mdesc *prom_getmdesc(struct linux_mdesc *curr);
#define PROM_NULL_MDESC   ((struct linux_mdesc *) 0)

/* Called by prom_init to setup the physical memory pmemblock
 * array.
 */
extern void prom_meminit(void);
extern void prom_fixup_mem_map(unsigned long start_mem, unsigned long end_mem);

/* Returns pointer to PROM physical memory block array. */
extern struct prom_pmemblock *prom_getpblock_array(void);

/* PROM device tree library routines. */
#define PROM_NULL_COMPONENT ((pcomponent *) 0)

/* Get sibling component of THIS. */
extern pcomponent *prom_getsibling(pcomponent *this);

/* Get child component of THIS. */
extern pcomponent *prom_getchild(pcomponent *this);

/* Get parent component of CHILD. */
extern pcomponent *prom_getparent(pcomponent *child);

/* Copy component opaque data of component THIS into BUFFER
 * if component THIS has opaque data.  Returns success or
 * failure status.
 */
extern long prom_getcdata(void *buffer, pcomponent *this);

/* Other misc. component routines. */
extern pcomponent *prom_childadd(pcomponent *this, pcomponent *tmp, void *data);
extern long prom_delcomponent(pcomponent *this);
extern pcomponent *prom_componentbypath(char *path);

/* This is called at prom_init time to identify the
 * ARC architecture we are running on
 */
extern void prom_identify_arch(void);

/* Environemt variable routines. */
extern PCHAR ArcGetEnvironmentVariable(CHAR *name);
extern LONG SetEnvironmentVariable(PCHAR name, PCHAR value);

/* ARCS command line acquisition and parsing. */
extern char *prom_getcmdline(void);
extern void prom_init_cmdline(void);

/* Acquiring info about the current time, etc. */
extern struct linux_tinfo *prom_gettinfo(void);
extern unsigned long prom_getrtime(void);

/* File operations. */
extern long prom_getvdirent(unsigned long fd, struct linux_vdirent *ent, unsigned long num, unsigned long *cnt);
extern long prom_open(char *name, enum linux_omode md, unsigned long *fd);
extern long prom_close(unsigned long fd);
extern long prom_read(unsigned long fd, void *buf, unsigned long num, unsigned long *cnt);
extern long prom_getrstatus(unsigned long fd);
extern long prom_write(unsigned long fd, void *buf, unsigned long num, unsigned long *cnt);
extern long prom_seek(unsigned long fd, struct linux_bigint *off, enum linux_seekmode sm);
extern long prom_mount(char *name, enum linux_mountops op);
extern long prom_getfinfo(unsigned long fd, struct linux_finfo *buf);
extern long prom_setfinfo(unsigned long fd, unsigned long flags, unsigned long msk);

/* Running stand-along programs. */
extern long prom_load(char *name, unsigned long end, unsigned long *pc, unsigned long *eaddr);
extern long prom_invoke(unsigned long pc, unsigned long sp, long argc, char **argv, char **envp);
extern long prom_exec(char *name, long argc, char **argv, char **envp);

/* Misc. routines. */
extern void prom_halt(void) __attribute__((noreturn));
extern void prom_powerdown(void) __attribute__((noreturn));
extern void prom_restart(void) __attribute__((noreturn));
extern void prom_reboot(void) __attribute__((noreturn));
extern void prom_imode(void) __attribute__((noreturn));
extern long prom_cfgsave(void);
extern struct linux_sysid *prom_getsysid(void);
extern void prom_cacheflush(void);

#endif /* _ASM_SGIALIB_H */
