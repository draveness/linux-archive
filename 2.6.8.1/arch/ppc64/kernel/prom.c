/*
 * 
 *
 * Procedures for interfacing to Open Firmware.
 *
 * Paul Mackerras	August 1996.
 * Copyright (C) 1996 Paul Mackerras.
 * 
 *  Adapted for 64bit PowerPC by Dave Engebretsen and Peter Bergner.
 *    {engebret|bergner}@us.ibm.com 
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#undef DEBUG_PROM

#include <stdarg.h>
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/threads.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/proc_fs.h>
#include <linux/stringify.h>
#include <linux/delay.h>
#include <linux/initrd.h>
#include <asm/prom.h>
#include <asm/rtas.h>
#include <asm/lmb.h>
#include <asm/abs_addr.h>
#include <asm/page.h>
#include <asm/processor.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/smp.h>
#include <asm/system.h>
#include <asm/mmu.h>
#include <asm/pgtable.h>
#include <asm/bitops.h>
#include <asm/naca.h>
#include <asm/pci.h>
#include <asm/iommu.h>
#include <asm/bootinfo.h>
#include <asm/ppcdebug.h>
#include <asm/btext.h>
#include <asm/sections.h>
#include <asm/machdep.h>
#include "open_pic.h"

#ifdef CONFIG_LOGO_LINUX_CLUT224
#include <linux/linux_logo.h>
extern const struct linux_logo logo_linux_clut224;
#endif

/*
 * Properties whose value is longer than this get excluded from our
 * copy of the device tree. This value does need to be big enough to
 * ensure that we don't lose things like the interrupt-map property
 * on a PCI-PCI bridge.
 */
#define MAX_PROPERTY_LENGTH	(1UL * 1024 * 1024)

/*
 * prom_init() is called very early on, before the kernel text
 * and data have been mapped to KERNELBASE.  At this point the code
 * is running at whatever address it has been loaded at, so
 * references to extern and static variables must be relocated
 * explicitly.  The procedure reloc_offset() returns the address
 * we're currently running at minus the address we were linked at.
 * (Note that strings count as static variables.)
 *
 * Because OF may have mapped I/O devices into the area starting at
 * KERNELBASE, particularly on CHRP machines, we can't safely call
 * OF once the kernel has been mapped to KERNELBASE.  Therefore all
 * OF calls should be done within prom_init(), and prom_init()
 * and all routines called within it must be careful to relocate
 * references as necessary.
 *
 * Note that the bss is cleared *after* prom_init runs, so we have
 * to make sure that any static or extern variables it accesses
 * are put in the data segment.
 */


#define PROM_BUG() do {						\
        prom_printf("kernel BUG at %s line 0x%x!\n",		\
		    RELOC(__FILE__), __LINE__);			\
        __asm__ __volatile__(".long " BUG_ILLEGAL_INSTR);	\
} while (0)

#ifdef DEBUG_PROM
#define prom_debug(x...)	prom_printf(x)
#else
#define prom_debug(x...)
#endif


struct pci_reg_property {
	struct pci_address addr;
	u32 size_hi;
	u32 size_lo;
};


struct isa_reg_property {
	u32 space;
	u32 address;
	u32 size;
};

struct pci_intr_map {
	struct pci_address addr;
	u32 dunno;
	phandle int_ctrler;
	u32 intr;
};


typedef unsigned long interpret_func(struct device_node *, unsigned long,
				     int, int, int);

#ifndef FB_MAX			/* avoid pulling in all of the fb stuff */
#define FB_MAX	8
#endif

/* prom structure */
struct prom_t prom;

char *prom_display_paths[FB_MAX] __initdata = { NULL, };
phandle prom_display_nodes[FB_MAX] __initdata;
unsigned int prom_num_displays = 0;
char *of_stdout_device = NULL;

static int iommu_force_on;
int ppc64_iommu_off;

extern struct rtas_t rtas;
extern unsigned long klimit;
extern struct lmb lmb;

#define MAX_PHB (32 * 6)  /* 32 drawers * 6 PHBs/drawer */
struct of_tce_table of_tce_table[MAX_PHB + 1];

char *bootpath = NULL;
char *bootdevice = NULL;

int boot_cpuid = 0;
#define MAX_CPU_THREADS 2

struct device_node *allnodes = NULL;
/* use when traversing tree through the allnext, child, sibling,
 * or parent members of struct device_node.
 */
static rwlock_t devtree_lock = RW_LOCK_UNLOCKED;

extern unsigned long reloc_offset(void);

extern void enter_prom(struct prom_args *args);
extern void copy_and_flush(unsigned long dest, unsigned long src,
			   unsigned long size, unsigned long offset);

unsigned long dev_tree_size;
unsigned long _get_PIR(void);

#ifdef CONFIG_HMT
struct {
	unsigned int pir;
	unsigned int threadid;
} hmt_thread_data[NR_CPUS];
#endif /* CONFIG_HMT */

char testString[] = "LINUX\n"; 

/*
 * This are used in calls to call_prom.  The 4th and following
 * arguments to call_prom should be 32-bit values.  64 bit values
 * are truncated to 32 bits (and fortunately don't get interpreted
 * as two arguments).
 */
#define ADDR(x)		(u32) ((unsigned long)(x) - offset)

/* This is the one and *ONLY* place where we actually call open
 * firmware from, since we need to make sure we're running in 32b
 * mode when we do.  We switch back to 64b mode upon return.
 */

#define PROM_ERROR	(-1)

static int __init call_prom(const char *service, int nargs, int nret, ...)
{
	int i;
	unsigned long offset = reloc_offset();
	struct prom_t *_prom = PTRRELOC(&prom);
	va_list list;

	_prom->args.service = ADDR(service);
	_prom->args.nargs = nargs;
	_prom->args.nret = nret;
	_prom->args.rets = (prom_arg_t *)&(_prom->args.args[nargs]);

	va_start(list, nret);
	for (i=0; i < nargs; i++)
		_prom->args.args[i] = va_arg(list, prom_arg_t);
	va_end(list);

	for (i=0; i < nret ;i++)
		_prom->args.rets[i] = 0;

	enter_prom(&_prom->args);

	return (nret > 0)? _prom->args.rets[0]: 0;
}


static void __init prom_print(const char *msg)
{
	const char *p, *q;
	unsigned long offset = reloc_offset();
	struct prom_t *_prom = PTRRELOC(&prom);

	if (_prom->stdout == 0)
		return;

	for (p = msg; *p != 0; p = q) {
		for (q = p; *q != 0 && *q != '\n'; ++q)
			;
		if (q > p)
			call_prom("write", 3, 1, _prom->stdout, p, q - p);
		if (*q == 0)
			break;
		++q;
		call_prom("write", 3, 1, _prom->stdout, ADDR("\r\n"), 2);
	}
}


static void __init prom_print_hex(unsigned long val)
{
	unsigned long offset = reloc_offset();
	int i, nibbles = sizeof(val)*2;
	char buf[sizeof(val)*2+1];
	struct prom_t *_prom = PTRRELOC(&prom);

	for (i = nibbles-1;  i >= 0;  i--) {
		buf[i] = (val & 0xf) + '0';
		if (buf[i] > '9')
			buf[i] += ('a'-'0'-10);
		val >>= 4;
	}
	buf[nibbles] = '\0';
	call_prom("write", 3, 1, _prom->stdout, buf, nibbles);
}


static void __init prom_printf(const char *format, ...)
{
	unsigned long offset = reloc_offset();
	const char *p, *q, *s;
	va_list args;
	unsigned long v;
	struct prom_t *_prom = PTRRELOC(&prom);

	va_start(args, format);
	for (p = PTRRELOC(format); *p != 0; p = q) {
		for (q = p; *q != 0 && *q != '\n' && *q != '%'; ++q)
			;
		if (q > p)
			call_prom("write", 3, 1, _prom->stdout, p, q - p);
		if (*q == 0)
			break;
		if (*q == '\n') {
			++q;
			call_prom("write", 3, 1, _prom->stdout,
				  ADDR("\r\n"), 2);
			continue;
		}
		++q;
		if (*q == 0)
			break;
		switch (*q) {
		case 's':
			++q;
			s = va_arg(args, const char *);
			prom_print(s);
			break;
		case 'x':
			++q;
			v = va_arg(args, unsigned long);
			prom_print_hex(v);
			break;
		}
	}
}


static void __init __attribute__((noreturn)) prom_panic(const char *reason)
{
	unsigned long offset = reloc_offset();

	prom_print(PTRRELOC(reason));
	/* ToDo: should put up an SRC here */
	call_prom("exit", 0, 0);

	for (;;)			/* should never get here */
		;
}


static int __init prom_next_node(phandle *nodep)
{
	phandle node;

	if ((node = *nodep) != 0
	    && (*nodep = call_prom("child", 1, 1, node)) != 0)
		return 1;
	if ((*nodep = call_prom("peer", 1, 1, node)) != 0)
		return 1;
	for (;;) {
		if ((node = call_prom("parent", 1, 1, node)) == 0)
			return 0;
		if ((*nodep = call_prom("peer", 1, 1, node)) != 0)
			return 1;
	}
}

static int __init prom_getprop(phandle node, const char *pname,
			       void *value, size_t valuelen)
{
	unsigned long offset = reloc_offset();

	return call_prom("getprop", 4, 1, node, ADDR(pname),
			 (u32)(unsigned long) value, (u32) valuelen);
}

static void __init prom_initialize_naca(void)
{
	phandle node;
	char type[64];
	unsigned long num_cpus = 0;
	unsigned long offset = reloc_offset();
	struct prom_t *_prom = PTRRELOC(&prom);
	struct naca_struct *_naca = RELOC(naca);
	struct systemcfg *_systemcfg = RELOC(systemcfg);

	/* NOTE: _naca->debug_switch is already initialized. */
	prom_debug("prom_initialize_naca: start...\n");

	_naca->pftSize = 0;	/* ilog2 of htab size.  computed below. */

	for (node = 0; prom_next_node(&node); ) {
		type[0] = 0;
		prom_getprop(node, "device_type", type, sizeof(type));

		if (!strcmp(type, RELOC("cpu"))) {
			num_cpus += 1;

			/* We're assuming *all* of the CPUs have the same
			 * d-cache and i-cache sizes... -Peter
			 */
			if ( num_cpus == 1 ) {
				u32 size, lsize;
				const char *dc, *ic;

				if (_systemcfg->platform == PLATFORM_POWERMAC){
					dc = "d-cache-block-size";
					ic = "i-cache-block-size";
				} else {
					dc = "d-cache-line-size";
					ic = "i-cache-line-size";
				}

				prom_getprop(node, "d-cache-size",
					     &size, sizeof(size));

				prom_getprop(node, dc, &lsize, sizeof(lsize));

				_systemcfg->dCacheL1Size = size;
				_systemcfg->dCacheL1LineSize = lsize;
				_naca->dCacheL1LogLineSize = __ilog2(lsize);
				_naca->dCacheL1LinesPerPage = PAGE_SIZE/lsize;

				prom_getprop(node, "i-cache-size",
					     &size, sizeof(size));

				prom_getprop(node, ic, &lsize, sizeof(lsize));

				_systemcfg->iCacheL1Size = size;
				_systemcfg->iCacheL1LineSize = lsize;
				_naca->iCacheL1LogLineSize = __ilog2(lsize);
				_naca->iCacheL1LinesPerPage = PAGE_SIZE/lsize;

				if (_systemcfg->platform == PLATFORM_PSERIES_LPAR) {
					u32 pft_size[2];
					prom_getprop(node, "ibm,pft-size",
						&pft_size, sizeof(pft_size));
				/* pft_size[0] is the NUMA CEC cookie */
					_naca->pftSize = pft_size[1];
				}
			}
		} else if (!strcmp(type, RELOC("serial"))) {
			phandle isa, pci;
			struct isa_reg_property reg;
			union pci_range ranges;

			if (_systemcfg->platform == PLATFORM_POWERMAC)
				continue;
			type[0] = 0;
			prom_getprop(node, "ibm,aix-loc", type, sizeof(type));

			if (strcmp(type, RELOC("S1")))
				continue;

			prom_getprop(node, "reg", &reg, sizeof(reg));

			isa = call_prom("parent", 1, 1, node);
			if (!isa)
				PROM_BUG();
			pci = call_prom("parent", 1, 1, isa);
			if (!pci)
				PROM_BUG();

			prom_getprop(pci, "ranges", &ranges, sizeof(ranges));

			if ( _prom->encode_phys_size == 32 )
				_naca->serialPortAddr = ranges.pci32.phys+reg.address;
			else {
				_naca->serialPortAddr = 
					((((unsigned long)ranges.pci64.phys_hi) << 32) |
					 (ranges.pci64.phys_lo)) + reg.address;
			}
		}
	}

	if (_systemcfg->platform == PLATFORM_POWERMAC)
		_naca->interrupt_controller = IC_OPEN_PIC;
	else {
		_naca->interrupt_controller = IC_INVALID;
		for (node = 0; prom_next_node(&node); ) {
			type[0] = 0;
			prom_getprop(node, "name", type, sizeof(type));
			if (strcmp(type, RELOC("interrupt-controller")))
				continue;
			prom_getprop(node, "compatible", type, sizeof(type));
			if (strstr(type, RELOC("open-pic")))
				_naca->interrupt_controller = IC_OPEN_PIC;
			else if (strstr(type, RELOC("ppc-xicp")))
				_naca->interrupt_controller = IC_PPC_XIC;
			else
				prom_printf("prom: failed to recognize"
					    " interrupt-controller\n");
			break;
		}
	}

	if (_naca->interrupt_controller == IC_INVALID) {
		prom_printf("prom: failed to find interrupt-controller\n");
		PROM_BUG();
	}

	/* We gotta have at least 1 cpu... */
	if ( (_systemcfg->processorCount = num_cpus) < 1 )
		PROM_BUG();

	_systemcfg->physicalMemorySize = lmb_phys_mem_size();

	if (_systemcfg->platform == PLATFORM_PSERIES ||
	    _systemcfg->platform == PLATFORM_POWERMAC) {
		unsigned long rnd_mem_size, pteg_count;

		/* round mem_size up to next power of 2 */
		rnd_mem_size = 1UL << __ilog2(_systemcfg->physicalMemorySize);
		if (rnd_mem_size < _systemcfg->physicalMemorySize)
			rnd_mem_size <<= 1;

		/* # pages / 2 */
		pteg_count = (rnd_mem_size >> (12 + 1));

		_naca->pftSize = __ilog2(pteg_count << 7);
	}

	if (_naca->pftSize == 0) {
		prom_printf("prom: failed to compute pftSize!\n");
		PROM_BUG();
	}

	/* Add an eye catcher and the systemcfg layout version number */
	strcpy(_systemcfg->eye_catcher, RELOC("SYSTEMCFG:PPC64"));
	_systemcfg->version.major = SYSTEMCFG_MAJOR;
	_systemcfg->version.minor = SYSTEMCFG_MINOR;
	_systemcfg->processor = _get_PVR();

	prom_debug("systemcfg->processorCount       = 0x%x\n",
		   _systemcfg->processorCount);
	prom_debug("systemcfg->physicalMemorySize   = 0x%x\n",
		   _systemcfg->physicalMemorySize);
	prom_debug("naca->pftSize                   = 0x%x\n",
		   _naca->pftSize);
	prom_debug("systemcfg->dCacheL1LineSize     = 0x%x\n",
		   _systemcfg->dCacheL1LineSize);
	prom_debug("systemcfg->iCacheL1LineSize     = 0x%x\n",
		   _systemcfg->iCacheL1LineSize);
	prom_debug("naca->serialPortAddr            = 0x%x\n",
		   _naca->serialPortAddr);
	prom_debug("naca->interrupt_controller      = 0x%x\n",
		   _naca->interrupt_controller);
	prom_debug("systemcfg->platform             = 0x%x\n",
		   _systemcfg->platform);
	prom_debug("prom_initialize_naca: end...\n");
}


static void __init early_cmdline_parse(void)
{
	unsigned long offset = reloc_offset();
	char *opt;
#ifndef CONFIG_PMAC_DART
	struct systemcfg *_systemcfg = RELOC(systemcfg);
#endif

	opt = strstr(RELOC(cmd_line), RELOC("iommu="));
	if (opt) {
		prom_printf("opt is:%s\n", opt);
		opt += 6;
		while (*opt && *opt == ' ')
			opt++;
		if (!strncmp(opt, RELOC("off"), 3))
			RELOC(ppc64_iommu_off) = 1;
		else if (!strncmp(opt, RELOC("force"), 5))
			RELOC(iommu_force_on) = 1;
	}

#ifndef CONFIG_PMAC_DART
	if (_systemcfg->platform == PLATFORM_POWERMAC) {
		RELOC(ppc64_iommu_off) = 1;
		prom_printf("DART disabled on PowerMac !\n");
	}
#endif
}

#ifdef DEBUG_PROM
void prom_dump_lmb(void)
{
	unsigned long i;
	unsigned long offset = reloc_offset();
	struct lmb *_lmb  = PTRRELOC(&lmb);

	prom_printf("\nprom_dump_lmb:\n");
	prom_printf("    memory.cnt		  = 0x%x\n",
		    _lmb->memory.cnt);
	prom_printf("    memory.size		 = 0x%x\n",
		    _lmb->memory.size);
	for (i=0; i < _lmb->memory.cnt ;i++) {
		prom_printf("    memory.region[0x%x].base       = 0x%x\n",
			    i, _lmb->memory.region[i].base);
		prom_printf("		      .physbase = 0x%x\n",
			    _lmb->memory.region[i].physbase);
		prom_printf("		      .size     = 0x%x\n",
			    _lmb->memory.region[i].size);
	}

	prom_printf("\n    reserved.cnt		  = 0x%x\n",
		    _lmb->reserved.cnt);
	prom_printf("    reserved.size		 = 0x%x\n",
		    _lmb->reserved.size);
	for (i=0; i < _lmb->reserved.cnt ;i++) {
		prom_printf("    reserved.region[0x%x\n].base       = 0x%x\n",
			    i, _lmb->reserved.region[i].base);
		prom_printf("		      .physbase = 0x%x\n",
			    _lmb->reserved.region[i].physbase);
		prom_printf("		      .size     = 0x%x\n",
			    _lmb->reserved.region[i].size);
	}
}
#endif /* DEBUG_PROM */

static void __init prom_initialize_lmb(void)
{
	phandle node;
	char type[64];
	unsigned long i, offset = reloc_offset();
	struct prom_t *_prom = PTRRELOC(&prom);
	struct systemcfg *_systemcfg = RELOC(systemcfg);
	union lmb_reg_property reg;
	unsigned long lmb_base, lmb_size;
	unsigned long num_regs, bytes_per_reg = (_prom->encode_phys_size*2)/8;

	lmb_init();

	/* XXX Quick HACK. Proper fix is to drop those structures and properly use
	 * #address-cells. PowerMac has #size-cell set to 1 and #address-cells to 2
	 */
	if (_systemcfg->platform == PLATFORM_POWERMAC)
		bytes_per_reg = 12;

	for (node = 0; prom_next_node(&node); ) {
		type[0] = 0;
		prom_getprop(node, "device_type", type, sizeof(type));

		if (strcmp(type, RELOC("memory")))
			continue;

		num_regs = prom_getprop(node, "reg", &reg, sizeof(reg))
			/ bytes_per_reg;

		for (i=0; i < num_regs ;i++) {
			if (_systemcfg->platform == PLATFORM_POWERMAC) {
				lmb_base = ((unsigned long)reg.addrPM[i].address_hi) << 32;
				lmb_base |= (unsigned long)reg.addrPM[i].address_lo;
				lmb_size = reg.addrPM[i].size;
			} else if (_prom->encode_phys_size == 32) {
				lmb_base = reg.addr32[i].address;
				lmb_size = reg.addr32[i].size;
			} else {
				lmb_base = reg.addr64[i].address;
				lmb_size = reg.addr64[i].size;
			}

			/* We limit memory to 2GB if the IOMMU is off */
			if (RELOC(ppc64_iommu_off)) {
				if (lmb_base >= 0x80000000UL)
					continue;

				if ((lmb_base + lmb_size) > 0x80000000UL)
					lmb_size = 0x80000000UL - lmb_base;
			}

			if (lmb_add(lmb_base, lmb_size) < 0)
				prom_printf("Too many LMB's, discarding this one...\n");
		}

	}

	lmb_analyze();
#ifdef DEBUG_PROM
	prom_dump_lmb();
#endif /* DEBUG_PROM */
}

static void __init
prom_instantiate_rtas(void)
{
	unsigned long offset = reloc_offset();
	struct prom_t *_prom = PTRRELOC(&prom);
	struct rtas_t *_rtas = PTRRELOC(&rtas);
	struct systemcfg *_systemcfg = RELOC(systemcfg);
	ihandle prom_rtas;
	u32 getprop_rval;
	char hypertas_funcs[4];

	prom_debug("prom_instantiate_rtas: start...\n");

	prom_rtas = call_prom("finddevice", 1, 1, ADDR("/rtas"));
	if (prom_rtas != (ihandle) -1) {
		unsigned long x;
		x = prom_getprop(prom_rtas, "ibm,hypertas-functions",
				 hypertas_funcs, sizeof(hypertas_funcs));

		if (x != PROM_ERROR) {
			prom_printf("Hypertas detected, assuming LPAR !\n");
			_systemcfg->platform = PLATFORM_PSERIES_LPAR;
		}

		prom_getprop(prom_rtas, "rtas-size",
			     &getprop_rval, sizeof(getprop_rval));
		_rtas->size = getprop_rval;
		prom_printf("instantiating rtas");
		if (_rtas->size != 0) {
			unsigned long rtas_region = RTAS_INSTANTIATE_MAX;

			/* Grab some space within the first RTAS_INSTANTIATE_MAX bytes
			 * of physical memory (or within the RMO region) because RTAS
			 * runs in 32-bit mode and relocate off.
			 */
			if ( _systemcfg->platform == PLATFORM_PSERIES_LPAR ) {
				struct lmb *_lmb  = PTRRELOC(&lmb);
				rtas_region = min(_lmb->rmo_size, RTAS_INSTANTIATE_MAX);
			}

			_rtas->base = lmb_alloc_base(_rtas->size, PAGE_SIZE, rtas_region);

			prom_printf(" at 0x%x", _rtas->base);

			prom_rtas = call_prom("open", 1, 1, ADDR("/rtas"));
			prom_printf("...");

			if (call_prom("call-method", 3, 2,
				      ADDR("instantiate-rtas"),
				      prom_rtas,
				      _rtas->base) != PROM_ERROR) {
				_rtas->entry = (long)_prom->args.rets[1];
			}
			RELOC(rtas_rmo_buf)
				= lmb_alloc_base(RTAS_RMOBUF_MAX, PAGE_SIZE,
							rtas_region);
		}

		if (_rtas->entry <= 0) {
			prom_printf(" failed\n");
		} else {
			prom_printf(" done\n");
		}

		prom_debug("rtas->base		= 0x%x\n", _rtas->base);
		prom_debug("rtas->entry	       = 0x%x\n", _rtas->entry);
		prom_debug("rtas->size		= 0x%x\n", _rtas->size);
	}
	prom_debug("prom_instantiate_rtas: end...\n");
}


#ifdef CONFIG_PMAC_DART
static void __init prom_initialize_dart_table(void)
{
	unsigned long offset = reloc_offset();
	extern unsigned long dart_tablebase;
	extern unsigned long dart_tablesize;

	/* Only reserve DART space if machine has more than 2GB of RAM
	 * or if requested with iommu=on on cmdline.
	 */
	if (lmb_end_of_DRAM() <= 0x80000000ull && !RELOC(iommu_force_on))
		return;

	/* 512 pages (2MB) is max DART tablesize. */
	RELOC(dart_tablesize) = 1UL << 21;
	/* 16MB (1 << 24) alignment. We allocate a full 16Mb chuck since we
	 * will blow up an entire large page anyway in the kernel mapping
	 */
	RELOC(dart_tablebase) = (unsigned long)
		abs_to_virt(lmb_alloc_base(1UL<<24, 1UL<<24, 0x80000000L));

	prom_printf("Dart at: %x\n", RELOC(dart_tablebase));
}
#endif /* CONFIG_PMAC_DART */

static void __init prom_initialize_tce_table(void)
{
	phandle node;
	ihandle phb_node;
	unsigned long offset = reloc_offset();
	char compatible[64], path[64], type[64], model[64];
	unsigned long i, table = 0;
	unsigned long base, vbase, align;
	unsigned int minalign, minsize;
	struct of_tce_table *prom_tce_table = RELOC(of_tce_table);
	unsigned long tce_entry, *tce_entryp;

	if (RELOC(ppc64_iommu_off))
		return;

	prom_debug("starting prom_initialize_tce_table\n");

	/* Search all nodes looking for PHBs. */
	for (node = 0; prom_next_node(&node); ) {
		if (table == MAX_PHB) {
			prom_printf("WARNING: PCI host bridge ignored, "
				    "need to increase MAX_PHB\n");
			continue;
		}

		compatible[0] = 0;
		type[0] = 0;
		model[0] = 0;
		prom_getprop(node, "compatible",
			     compatible, sizeof(compatible));
		prom_getprop(node, "device_type", type, sizeof(type));
		prom_getprop(node, "model", model, sizeof(model));

		/* Keep the old logic in tack to avoid regression. */
		if (compatible[0] != 0) {
			if ((strstr(compatible, RELOC("python")) == NULL) &&
			    (strstr(compatible, RELOC("Speedwagon")) == NULL) &&
			    (strstr(compatible, RELOC("Winnipeg")) == NULL))
				continue;
		} else if (model[0] != 0) {
			if ((strstr(model, RELOC("ython")) == NULL) &&
			    (strstr(model, RELOC("peedwagon")) == NULL) &&
			    (strstr(model, RELOC("innipeg")) == NULL))
				continue;
		}

		if ((type[0] == 0) || (strstr(type, RELOC("pci")) == NULL)) {
			continue;
		}

		if (prom_getprop(node, "tce-table-minalign", &minalign,
				 sizeof(minalign)) == PROM_ERROR) {
			minalign = 0;
		}

		if (prom_getprop(node, "tce-table-minsize", &minsize,
				 sizeof(minsize)) == PROM_ERROR) {
			minsize = 4UL << 20;
		}

		/*
		 * Even though we read what OF wants, we just set the table
		 * size to 4 MB.  This is enough to map 2GB of PCI DMA space.
		 * By doing this, we avoid the pitfalls of trying to DMA to
		 * MMIO space and the DMA alias hole.
		 *
		 * On POWER4, firmware sets the TCE region by assuming
		 * each TCE table is 8MB. Using this memory for anything
		 * else will impact performance, so we always allocate 8MB.
		 * Anton
		 */
		if (__is_processor(PV_POWER4) || __is_processor(PV_POWER4p))
			minsize = 8UL << 20;
		else
			minsize = 4UL << 20;

		/* Align to the greater of the align or size */
		align = max(minalign, minsize);

		/* Carve out storage for the TCE table. */
		base = lmb_alloc(minsize, align);

		if ( !base ) {
			prom_panic("ERROR, cannot find space for TCE table.\n");
		}

		vbase = (unsigned long)abs_to_virt(base);

		/* Save away the TCE table attributes for later use. */
		prom_tce_table[table].node = node;
		prom_tce_table[table].base = vbase;
		prom_tce_table[table].size = minsize;

		prom_debug("TCE table: 0x%x\n", table);
		prom_debug("\tnode = 0x%x\n", node);
		prom_debug("\tbase = 0x%x\n", vbase);
		prom_debug("\tsize = 0x%x\n", minsize);

		/* Initialize the table to have a one-to-one mapping
		 * over the allocated size.
		 */
		tce_entryp = (unsigned long *)base;
		for (i = 0; i < (minsize >> 3) ;tce_entryp++, i++) {
			tce_entry = (i << PAGE_SHIFT);
			tce_entry |= 0x3;
			*tce_entryp = tce_entry;
		}

		/* It seems OF doesn't null-terminate the path :-( */
		memset(path, 0, sizeof(path));
		/* Call OF to setup the TCE hardware */
		if (call_prom("package-to-path", 3, 1, node,
			      path, sizeof(path)-1) == PROM_ERROR) {
			prom_printf("package-to-path failed\n");
		} else {
			prom_printf("opening PHB %s", path);
		}

		phb_node = call_prom("open", 1, 1, path);
		if ( (long)phb_node <= 0) {
			prom_printf("... failed\n");
		} else {
			prom_printf("... done\n");
		}
		call_prom("call-method", 6, 0, ADDR("set-64-bit-addressing"),
			  phb_node, -1, minsize,
			  (u32) base, (u32) (base >> 32));
		call_prom("close", 1, 0, phb_node);

		table++;
	}

	/* Flag the first invalid entry */
	prom_tce_table[table].node = 0;
	prom_debug("ending prom_initialize_tce_table\n");
}

/*
 * With CHRP SMP we need to use the OF to start the other
 * processors so we can't wait until smp_boot_cpus (the OF is
 * trashed by then) so we have to put the processors into
 * a holding pattern controlled by the kernel (not OF) before
 * we destroy the OF.
 *
 * This uses a chunk of low memory, puts some holding pattern
 * code there and sends the other processors off to there until
 * smp_boot_cpus tells them to do something.  The holding pattern
 * checks that address until its cpu # is there, when it is that
 * cpu jumps to __secondary_start().  smp_boot_cpus() takes care
 * of setting those values.
 *
 * We also use physical address 0x4 here to tell when a cpu
 * is in its holding pattern code.
 *
 * Fixup comment... DRENG / PPPBBB - Peter
 *
 * -- Cort
 */
static void __init prom_hold_cpus(unsigned long mem)
{
	unsigned long i;
	unsigned int reg;
	phandle node;
	unsigned long offset = reloc_offset();
	char type[64], *path;
	int cpuid = 0;
	unsigned int interrupt_server[MAX_CPU_THREADS];
	unsigned int cpu_threads, hw_cpu_num;
	int propsize;
	extern void __secondary_hold(void);
	extern unsigned long __secondary_hold_spinloop;
	extern unsigned long __secondary_hold_acknowledge;
	unsigned long *spinloop
		= (void *)virt_to_abs(&__secondary_hold_spinloop);
	unsigned long *acknowledge
		= (void *)virt_to_abs(&__secondary_hold_acknowledge);
	unsigned long secondary_hold
		= virt_to_abs(*PTRRELOC((unsigned long *)__secondary_hold));
	struct systemcfg *_systemcfg = RELOC(systemcfg);
	struct paca_struct *lpaca = PTRRELOC(&paca[0]);
	struct prom_t *_prom = PTRRELOC(&prom);
#ifdef CONFIG_SMP
	struct naca_struct *_naca = RELOC(naca);
#endif

	/* On pmac, we just fill out the various global bitmasks and
	 * arrays indicating our CPUs are here, they are actually started
	 * later on from pmac_smp
	 */
	if (_systemcfg->platform == PLATFORM_POWERMAC) {
		for (node = 0; prom_next_node(&node); ) {
			type[0] = 0;
			prom_getprop(node, "device_type", type, sizeof(type));
			if (strcmp(type, RELOC("cpu")) != 0)
				continue;
			reg = -1;
			prom_getprop(node, "reg", &reg, sizeof(reg));
			lpaca[cpuid].hw_cpu_id = reg;

#ifdef CONFIG_SMP
			cpu_set(cpuid, RELOC(cpu_available_map));
			cpu_set(cpuid, RELOC(cpu_possible_map));
			cpu_set(cpuid, RELOC(cpu_present_at_boot));
			if (reg == 0)
				cpu_set(cpuid, RELOC(cpu_online_map));
#endif /* CONFIG_SMP */
			cpuid++;
		}
		return;
	}

	/* Initially, we must have one active CPU. */
	_systemcfg->processorCount = 1;

	prom_debug("prom_hold_cpus: start...\n");
	prom_debug("    1) spinloop       = 0x%x\n", (unsigned long)spinloop);
	prom_debug("    1) *spinloop      = 0x%x\n", *spinloop);
	prom_debug("    1) acknowledge    = 0x%x\n",
		   (unsigned long)acknowledge);
	prom_debug("    1) *acknowledge   = 0x%x\n", *acknowledge);
	prom_debug("    1) secondary_hold = 0x%x\n", secondary_hold);

	/* Set the common spinloop variable, so all of the secondary cpus
	 * will block when they are awakened from their OF spinloop.
	 * This must occur for both SMP and non SMP kernels, since OF will
	 * be trashed when we move the kernel.
	 */
	*spinloop = 0;

#ifdef CONFIG_HMT
	for (i=0; i < NR_CPUS; i++) {
		RELOC(hmt_thread_data)[i].pir = 0xdeadbeef;
	}
#endif
	/* look for cpus */
	for (node = 0; prom_next_node(&node); ) {
		type[0] = 0;
		prom_getprop(node, "device_type", type, sizeof(type));
		if (strcmp(type, RELOC("cpu")) != 0)
			continue;

		/* Skip non-configured cpus. */
		prom_getprop(node, "status", type, sizeof(type));
		if (strcmp(type, RELOC("okay")) != 0)
			continue;

		reg = -1;
		prom_getprop(node, "reg", &reg, sizeof(reg));

		path = (char *) mem;
		memset(path, 0, 256);
		if (call_prom("package-to-path", 3, 1,
			      node, path, 255) == PROM_ERROR)
			continue;

		prom_debug("\ncpuid        = 0x%x\n", cpuid);
		prom_debug("cpu hw idx   = 0x%x\n", reg);
		lpaca[cpuid].hw_cpu_id = reg;

		/* Init the acknowledge var which will be reset by
		 * the secondary cpu when it awakens from its OF
		 * spinloop.
		 */
		*acknowledge = (unsigned long)-1;

		propsize = prom_getprop(node, "ibm,ppc-interrupt-server#s",
					&interrupt_server,
					sizeof(interrupt_server));
		if (propsize < 0) {
			/* no property.  old hardware has no SMT */
			cpu_threads = 1;
			interrupt_server[0] = reg; /* fake it with phys id */
		} else {
			/* We have a threaded processor */
			cpu_threads = propsize / sizeof(u32);
			if (cpu_threads > MAX_CPU_THREADS) {
				prom_printf("SMT: too many threads!\n"
					    "SMT: found %x, max is %x\n",
					    cpu_threads, MAX_CPU_THREADS);
				cpu_threads = 1; /* ToDo: panic? */
			}
		}

		hw_cpu_num = interrupt_server[0];
		if (hw_cpu_num != _prom->cpu) {
			/* Primary Thread of non-boot cpu */
			prom_printf("%x : starting cpu %s... ", cpuid, path);
			call_prom("start-cpu", 3, 0, node,
				  secondary_hold, cpuid);

			for ( i = 0 ; (i < 100000000) && 
			      (*acknowledge == ((unsigned long)-1)); i++ ) ;

			if (*acknowledge == cpuid) {
				prom_printf("... done\n");
				/* We have to get every CPU out of OF,
				 * even if we never start it. */
				if (cpuid >= NR_CPUS)
					goto next;
#ifdef CONFIG_SMP
				/* Set the number of active processors. */
				_systemcfg->processorCount++;
				cpu_set(cpuid, RELOC(cpu_available_map));
				cpu_set(cpuid, RELOC(cpu_possible_map));
				cpu_set(cpuid, RELOC(cpu_present_at_boot));
#endif
			} else {
				prom_printf("... failed: %x\n", *acknowledge);
			}
		}
#ifdef CONFIG_SMP
		else {
			prom_printf("%x : booting  cpu %s\n", cpuid, path);
			cpu_set(cpuid, RELOC(cpu_available_map));
			cpu_set(cpuid, RELOC(cpu_possible_map));
			cpu_set(cpuid, RELOC(cpu_online_map));
			cpu_set(cpuid, RELOC(cpu_present_at_boot));
		}
#endif
next:
#ifdef CONFIG_SMP
		/* Init paca for secondary threads.   They start later. */
		for (i=1; i < cpu_threads; i++) {
			cpuid++;
			if (cpuid >= NR_CPUS)
				continue;
			lpaca[cpuid].hw_cpu_id = interrupt_server[i];
			prom_printf("%x : preparing thread ... ",
				    interrupt_server[i]);
			if (_naca->smt_state) {
				cpu_set(cpuid, RELOC(cpu_available_map));
				cpu_set(cpuid, RELOC(cpu_present_at_boot));
				prom_printf("available\n");
			} else {
				prom_printf("not available\n");
			}
		}
#endif
		cpuid++;
	}
#ifdef CONFIG_HMT
	/* Only enable HMT on processors that provide support. */
	if (__is_processor(PV_PULSAR) || 
	    __is_processor(PV_ICESTAR) ||
	    __is_processor(PV_SSTAR)) {
		prom_printf("    starting secondary threads\n");

		for (i = 0; i < NR_CPUS; i += 2) {
			if (!cpu_online(i))
				continue;

			if (i == 0) {
				unsigned long pir = _get_PIR();
				if (__is_processor(PV_PULSAR)) {
					RELOC(hmt_thread_data)[i].pir = 
						pir & 0x1f;
				} else {
					RELOC(hmt_thread_data)[i].pir = 
						pir & 0x3ff;
				}
			}
/* 			cpu_set(i+1, cpu_online_map); */
			cpu_set(i+1, RELOC(cpu_possible_map));
		}
		_systemcfg->processorCount *= 2;
	} else {
		prom_printf("Processor is not HMT capable\n");
	}
#endif

	if (cpuid > NR_CPUS)
		prom_printf("WARNING: maximum CPUs (" __stringify(NR_CPUS)
			    ") exceeded: ignoring extras\n");

	prom_debug("prom_hold_cpus: end...\n");
}

static void __init smt_setup(void)
{
	char *p, *q;
	char my_smt_enabled = SMT_DYNAMIC;
	ihandle prom_options = 0;
	char option[9];
	unsigned long offset = reloc_offset();
	struct naca_struct *_naca = RELOC(naca);
	char found = 0;

	if (strstr(RELOC(cmd_line), RELOC("smt-enabled="))) {
		for (q = RELOC(cmd_line); (p = strstr(q, RELOC("smt-enabled="))) != 0; ) {
			q = p + 12;
			if (p > RELOC(cmd_line) && p[-1] != ' ')
				continue;
			found = 1;
			if (q[0] == 'o' && q[1] == 'f' && 
			    q[2] == 'f' && (q[3] == ' ' || q[3] == '\0')) {
				my_smt_enabled = SMT_OFF;
			} else if (q[0]=='o' && q[1] == 'n' && 
				   (q[2] == ' ' || q[2] == '\0')) {
				my_smt_enabled = SMT_ON;
			} else {
				my_smt_enabled = SMT_DYNAMIC;
			} 
		}
	}
	if (!found) {
		prom_options = call_prom("finddevice", 1, 1, ADDR("/options"));
		if (prom_options != (ihandle) -1) {
			prom_getprop(prom_options, "ibm,smt-enabled",
				     option, sizeof(option));
			if (option[0] != 0) {
				found = 1;
				if (!strcmp(option, RELOC("off")))
					my_smt_enabled = SMT_OFF;
				else if (!strcmp(option, RELOC("on")))
					my_smt_enabled = SMT_ON;
				else
					my_smt_enabled = SMT_DYNAMIC;
			}
		}
	}

	if (!found )
		my_smt_enabled = SMT_DYNAMIC; /* default to on */

	_naca->smt_state = my_smt_enabled;
}


#ifdef CONFIG_BOOTX_TEXT

/* This function will enable the early boot text when doing OF booting. This
 * way, xmon output should work too
 */
static void __init setup_disp_fake_bi(ihandle dp)
{
	int width = 640, height = 480, depth = 8, pitch;
	unsigned address;
	struct pci_reg_property addrs[8];
	int i, naddrs;
	char name[64];
	unsigned long offset = reloc_offset();

	memset(name, 0, sizeof(name));
	prom_getprop(dp, "name", name, sizeof(name));
	name[sizeof(name)-1] = 0;
	prom_printf("Initializing fake screen: %s\n", name);

	prom_getprop(dp, "width", &width, sizeof(width));
	prom_getprop(dp, "height", &height, sizeof(height));
	prom_getprop(dp, "depth", &depth, sizeof(depth));
	pitch = width * ((depth + 7) / 8);
	prom_getprop(dp, "linebytes", &pitch, sizeof(pitch));
	if (pitch == 1)
		pitch = 0x1000;		/* for strange IBM display */
	address = 0;

	prom_printf("width %x height %x depth %x linebytes %x\n",
		    width, height, depth, depth);

	prom_getprop(dp, "address", &address, sizeof(address));
	if (address == 0) {
		/* look for an assigned address with a size of >= 1MB */
		naddrs = prom_getprop(dp, "assigned-addresses",
				      addrs, sizeof(addrs));
		naddrs /= sizeof(struct pci_reg_property);
		for (i = 0; i < naddrs; ++i) {
			if (addrs[i].size_lo >= (1 << 20)) {
				address = addrs[i].addr.a_lo;
				/* use the BE aperture if possible */
				if (addrs[i].size_lo >= (16 << 20))
					address += (8 << 20);
				break;
			}
		}
		if (address == 0) {
			prom_printf("Failed to get address of frame buffer\n");
			return;
		}
	}
	btext_setup_display(width, height, depth, pitch, address);
	prom_printf("Addr of fb: %x\n", address);
	RELOC(boot_text_mapped) = 0;
}
#endif /* CONFIG_BOOTX_TEXT */

static void __init prom_init_client_services(unsigned long pp)
{
	unsigned long offset = reloc_offset();
	struct prom_t *_prom = PTRRELOC(&prom);

	/* Get a handle to the prom entry point before anything else */
	_prom->entry = pp;

	/* Init default value for phys size */
	_prom->encode_phys_size = 32;

	/* get a handle for the stdout device */
	_prom->chosen = call_prom("finddevice", 1, 1, ADDR("/chosen"));
	if ((long)_prom->chosen <= 0)
		prom_panic("cannot find chosen"); /* msg won't be printed :( */

	/* get device tree root */
	_prom->root = call_prom("finddevice", 1, 1, ADDR("/"));
	if ((long)_prom->root <= 0)
		prom_panic("cannot find device tree root"); /* msg won't be printed :( */
}

static void __init prom_init_stdout(void)
{
	unsigned long offset = reloc_offset();
	struct prom_t *_prom = PTRRELOC(&prom);
	u32 val;

	if (prom_getprop(_prom->chosen, "stdout", &val, sizeof(val)) <= 0)
		prom_panic("cannot find stdout");

	_prom->stdout = val;
}

static int __init prom_find_machine_type(void)
{
	unsigned long offset = reloc_offset();
	struct prom_t *_prom = PTRRELOC(&prom);
	char compat[256];
	int len, i = 0;

	len = prom_getprop(_prom->root, "compatible",
			   compat, sizeof(compat)-1);
	if (len > 0) {
		compat[len] = 0;
		while (i < len) {
			char *p = &compat[i];
			int sl = strlen(p);
			if (sl == 0)
				break;
			if (strstr(p, RELOC("Power Macintosh")) ||
			    strstr(p, RELOC("MacRISC4")))
				return PLATFORM_POWERMAC;
			i += sl + 1;
		}
	}
	/* Default to pSeries */
	return PLATFORM_PSERIES;
}

static int __init prom_set_color(ihandle ih, int i, int r, int g, int b)
{
	unsigned long offset = reloc_offset();

	return call_prom("call-method", 6, 1, ADDR("color!"), ih, i, b, g, r);
}

/*
 * If we have a display that we don't know how to drive,
 * we will want to try to execute OF's open method for it
 * later.  However, OF will probably fall over if we do that
 * we've taken over the MMU.
 * So we check whether we will need to open the display,
 * and if so, open it now.
 */
static unsigned long __init check_display(unsigned long mem)
{
	phandle node;
	ihandle ih;
	int i, j;
	unsigned long offset = reloc_offset();
	struct prom_t *_prom = PTRRELOC(&prom);
	char type[16], *path;
	static unsigned char default_colors[] = {
		0x00, 0x00, 0x00,
		0x00, 0x00, 0xaa,
		0x00, 0xaa, 0x00,
		0x00, 0xaa, 0xaa,
		0xaa, 0x00, 0x00,
		0xaa, 0x00, 0xaa,
		0xaa, 0xaa, 0x00,
		0xaa, 0xaa, 0xaa,
		0x55, 0x55, 0x55,
		0x55, 0x55, 0xff,
		0x55, 0xff, 0x55,
		0x55, 0xff, 0xff,
		0xff, 0x55, 0x55,
		0xff, 0x55, 0xff,
		0xff, 0xff, 0x55,
		0xff, 0xff, 0xff
	};
	const unsigned char *clut;

	_prom->disp_node = 0;

	prom_printf("Looking for displays\n");
	if (RELOC(of_stdout_device) != 0)
		prom_printf("OF stdout is    : %s\n",
			    PTRRELOC(RELOC(of_stdout_device)));
	for (node = 0; prom_next_node(&node); ) {
		type[0] = 0;
		prom_getprop(node, "device_type", type, sizeof(type));
		if (strcmp(type, RELOC("display")) != 0)
			continue;
		/* It seems OF doesn't null-terminate the path :-( */
		path = (char *) mem;
		memset(path, 0, 256);

		/*
		 * leave some room at the end of the path for appending extra
		 * arguments
		 */
		if (call_prom("package-to-path", 3, 1, node, path, 250) < 0)
			continue;
		prom_printf("found display   : %s\n", path);
		
		/*
		 * If this display is the device that OF is using for stdout,
		 * move it to the front of the list.
		 */
		mem += strlen(path) + 1;
		i = RELOC(prom_num_displays);
		RELOC(prom_num_displays) = i + 1;
		if (RELOC(of_stdout_device) != 0 && i > 0
		    && strcmp(PTRRELOC(RELOC(of_stdout_device)), path) == 0) {
			for (; i > 0; --i) {
				RELOC(prom_display_paths[i])
					= RELOC(prom_display_paths[i-1]);
				RELOC(prom_display_nodes[i])
					= RELOC(prom_display_nodes[i-1]);
			}
			_prom->disp_node = node;
		}
		RELOC(prom_display_paths[i]) = PTRUNRELOC(path);
		RELOC(prom_display_nodes[i]) = node;
		if (_prom->disp_node == 0)
			_prom->disp_node = node;
		if (RELOC(prom_num_displays) >= FB_MAX)
			break;
	}
	prom_printf("Opening displays...\n");
	for (j = RELOC(prom_num_displays) - 1; j >= 0; j--) {
		path = PTRRELOC(RELOC(prom_display_paths[j]));
		prom_printf("opening display : %s", path);
		ih = call_prom("open", 1, 1, path);
		if (ih == (ihandle)0 || ih == (ihandle)-1) {
			prom_printf("... failed\n");
			continue;
		}

		prom_printf("... done\n");

		/* Setup a useable color table when the appropriate
		 * method is available. Should update this to set-colors */
		clut = RELOC(default_colors);
		for (i = 0; i < 32; i++, clut += 3)
			if (prom_set_color(ih, i, clut[0], clut[1],
					   clut[2]) != 0)
				break;

#ifdef CONFIG_LOGO_LINUX_CLUT224
		clut = PTRRELOC(RELOC(logo_linux_clut224.clut));
		for (i = 0; i < RELOC(logo_linux_clut224.clutsize); i++, clut += 3)
			if (prom_set_color(ih, i + 32, clut[0], clut[1],
					   clut[2]) != 0)
				break;
#endif /* CONFIG_LOGO_LINUX_CLUT224 */
	}

	return DOUBLEWORD_ALIGN(mem);
}

/* Return (relocated) pointer to this much memory: moves initrd if reqd. */
static void __init *__make_room(unsigned long *mem_start, unsigned long *mem_end,
				unsigned long needed, unsigned long align)
{
	void *ret;

	*mem_start = ALIGN(*mem_start, align);
	if (*mem_start + needed > *mem_end) {
#ifdef CONFIG_BLK_DEV_INITRD
		unsigned long offset = reloc_offset();
		/* FIXME: Apple OF doesn't map unclaimed mem.  If this
		 * ever happened on G5, we'd need to fix. */
		unsigned long initrd_len;

		if (*mem_end != RELOC(initrd_start))
			prom_panic("No memory for copy_device_tree");

		prom_printf("Huge device_tree: moving initrd\n");
		/* Move by 4M. */
		initrd_len = RELOC(initrd_end) - RELOC(initrd_start);
		*mem_end = RELOC(initrd_start) + 4 * 1024 * 1024;
		memmove((void *)*mem_end, (void *)RELOC(initrd_start),
			initrd_len);
		RELOC(initrd_start) = *mem_end;
		RELOC(initrd_end) = RELOC(initrd_start) + initrd_len;
#else
		prom_panic("No memory for copy_device_tree");
#endif
	}

	ret = (void *)*mem_start;
	*mem_start += needed;

	return ret;
}

#define make_room(startp, endp, type) \
	__make_room(startp, endp, sizeof(type), __alignof__(type))

static void __init
inspect_node(phandle node, struct device_node *dad,
	     unsigned long *mem_start, unsigned long *mem_end,
	     struct device_node ***allnextpp)
{
	int l;
	phandle child;
	struct device_node *np;
	struct property *pp, **prev_propp;
	char *prev_name, *namep;
	unsigned char *valp;
	unsigned long offset = reloc_offset();
	phandle ibm_phandle;

	np = make_room(mem_start, mem_end, struct device_node);
	memset(np, 0, sizeof(*np));

	np->node = node;
	**allnextpp = PTRUNRELOC(np);
	*allnextpp = &np->allnext;
	if (dad != 0) {
		np->parent = PTRUNRELOC(dad);
		/* we temporarily use the `next' field as `last_child'. */
		if (dad->next == 0)
			dad->child = PTRUNRELOC(np);
		else
			dad->next->sibling = PTRUNRELOC(np);
		dad->next = np;
	}

	/* get and store all properties */
	prev_propp = &np->properties;
	prev_name = RELOC("");
	for (;;) {
		/* 32 is max len of name including nul. */
		namep = make_room(mem_start, mem_end, char[32]);
		if (call_prom("nextprop", 3, 1, node, prev_name, namep) <= 0) {
			/* No more nodes: unwind alloc */
			*mem_start = (unsigned long)namep;
			break;
		}
		/* Trim off some if we can */
		*mem_start = DOUBLEWORD_ALIGN((unsigned long)namep
					     + strlen(namep) + 1);
		pp = make_room(mem_start, mem_end, struct property);
		pp->name = PTRUNRELOC(namep);
		prev_name = namep;

		pp->length = call_prom("getproplen", 2, 1, node, namep);
		if (pp->length < 0)
			continue;
		if (pp->length > MAX_PROPERTY_LENGTH) {
			char path[128];

			prom_printf("WARNING: ignoring large property ");
			/* It seems OF doesn't null-terminate the path :-( */
			memset(path, 0, sizeof(path));
			if (call_prom("package-to-path", 3, 1, node,
				      path, sizeof(path)-1) > 0)
				prom_printf("[%s] ", path);
			prom_printf("%s length 0x%x\n", namep, pp->length);
			continue;
		}
		valp = __make_room(mem_start, mem_end, pp->length, 1);
		pp->value = PTRUNRELOC(valp);
		call_prom("getprop", 4, 1, node, namep, valp, pp->length);
		*prev_propp = PTRUNRELOC(pp);
		prev_propp = &pp->next;
	}

	/* Add a "linux,phandle" property. */
	namep = make_room(mem_start, mem_end, char[16]);
	strcpy(namep, RELOC("linux,phandle"));
	pp = make_room(mem_start, mem_end, struct property);
	pp->name = PTRUNRELOC(namep);
	pp->length = sizeof(phandle);
	valp = make_room(mem_start, mem_end, phandle);
	pp->value = PTRUNRELOC(valp);
	*(phandle *)valp = node;
	*prev_propp = PTRUNRELOC(pp);
	pp->next = NULL;

	/* Set np->linux_phandle to the value of the ibm,phandle property
	   if it exists, otherwise to the phandle for this node. */
	np->linux_phandle = node;
	if (prom_getprop(node, "ibm,phandle",
			 &ibm_phandle, sizeof(ibm_phandle)) > 0)
		np->linux_phandle = ibm_phandle;

	/* get the node's full name */
	namep = (char *)*mem_start;
	l = call_prom("package-to-path", 3, 1, node,
		      namep, *mem_end - *mem_start);
	if (l >= 0) {
		/* Didn't fit?  Get more room. */
		if (l+1 > *mem_end - *mem_start) {
			namep = __make_room(mem_start, mem_end, l+1, 1);
			call_prom("package-to-path", 3, 1, node, namep, l);
		}
		np->full_name = PTRUNRELOC(namep);
		namep[l] = '\0';
		*mem_start = DOUBLEWORD_ALIGN(*mem_start + l + 1);
	}

	/* do all our children */
	child = call_prom("child", 1, 1, node);
	while (child != (phandle)0) {
		inspect_node(child, np, mem_start, mem_end,
					 allnextpp);
		child = call_prom("peer", 1, 1, child);
	}
}

/*
 * Make a copy of the device tree from the PROM.
 */
static unsigned long __init
copy_device_tree(unsigned long mem_start)
{
	phandle root;
	struct device_node **allnextp;
	unsigned long offset = reloc_offset();
	unsigned long mem_end;

	/* We pass mem_end-mem_start to OF: keep it well under 32-bit */
	mem_end = mem_start + 1024*1024*1024;
#ifdef CONFIG_BLK_DEV_INITRD
	if (RELOC(initrd_start) && RELOC(initrd_start) > mem_start)
		mem_end = RELOC(initrd_start);
#endif /* CONFIG_BLK_DEV_INITRD */

	root = call_prom("peer", 1, 1, (phandle)0);
	if (root == (phandle)0) {
		prom_panic("couldn't get device tree root\n");
	}
	allnextp = &RELOC(allnodes);
	inspect_node(root, NULL, &mem_start, &mem_end, &allnextp);
	*allnextp = NULL;
	return mem_start;
}

/* Verify bi_recs are good */
static struct bi_record * __init prom_bi_rec_verify(struct bi_record *bi_recs)
{
	struct bi_record *first, *last;

	prom_debug("birec_verify: r6=0x%x\n", (unsigned long)bi_recs);
	if (bi_recs != NULL)
		prom_debug("  tag=0x%x\n", bi_recs->tag);

	if ( bi_recs == NULL || bi_recs->tag != BI_FIRST )
		return NULL;

	last = (struct bi_record *)(long)bi_recs->data[0];

	prom_debug("  last=0x%x\n", (unsigned long)last);
	if (last != NULL)
		prom_debug("  last_tag=0x%x\n", last->tag);

	if ( last == NULL || last->tag != BI_LAST )
		return NULL;

	first = (struct bi_record *)(long)last->data[0];
	prom_debug("  first=0x%x\n", (unsigned long)first);

	if ( first == NULL || first != bi_recs )
		return NULL;

	return bi_recs;
}

static void __init prom_bi_rec_reserve(void)
{
	unsigned long offset = reloc_offset();
	struct prom_t *_prom = PTRRELOC(&prom);
	struct bi_record *rec;

	if ( _prom->bi_recs != NULL) {

		for ( rec=_prom->bi_recs;
		      rec->tag != BI_LAST;
		      rec=bi_rec_next(rec) ) {
			prom_debug("bi: 0x%x\n", rec->tag);
			switch (rec->tag) {
#ifdef CONFIG_BLK_DEV_INITRD
			case BI_INITRD:
				RELOC(initrd_start) = (unsigned long)(rec->data[0]);
				RELOC(initrd_end) = RELOC(initrd_start) + rec->data[1];
				break;
#endif /* CONFIG_BLK_DEV_INITRD */
			}
		}
		/* The next use of this field will be after relocation
	 	 * is enabled, so convert this physical address into a
	 	 * virtual address.
	 	 */
		_prom->bi_recs = PTRUNRELOC(_prom->bi_recs);
	}
}

/*
 * We enter here early on, when the Open Firmware prom is still
 * handling exceptions and the MMU hash table for us.
 */

unsigned long __init
prom_init(unsigned long r3, unsigned long r4, unsigned long pp,
	  unsigned long r6, unsigned long r7)
{
	unsigned long mem;
	ihandle prom_cpu;
	phandle cpu_pkg;
	unsigned long offset = reloc_offset();
	long l;
	char *p, *d;
	unsigned long phys;
	u32 getprop_rval;
	struct systemcfg *_systemcfg;
	struct paca_struct *lpaca = PTRRELOC(&paca[0]);
	struct prom_t *_prom = PTRRELOC(&prom);

	/* First zero the BSS -- use memset, some arches don't have
	 * caches on yet */
	memset(PTRRELOC(&__bss_start), 0, __bss_stop - __bss_start);

	/* Setup systemcfg and NACA pointers now */
	RELOC(systemcfg) = _systemcfg = (struct systemcfg *)(SYSTEMCFG_VIRT_ADDR - offset);
	RELOC(naca) = (struct naca_struct *)(NACA_VIRT_ADDR - offset);

	/* Init interface to Open Firmware and pickup bi-recs */
	prom_init_client_services(pp);

	/* Init prom stdout device */
	prom_init_stdout();

	prom_debug("klimit=0x%x\n", RELOC(klimit));
	prom_debug("offset=0x%x\n", offset);
	prom_debug("->mem=0x%x\n", RELOC(klimit) - offset);

	/* check out if we have bi_recs */
	_prom->bi_recs = prom_bi_rec_verify((struct bi_record *)r6);
	if ( _prom->bi_recs != NULL ) {
		RELOC(klimit) = PTRUNRELOC((unsigned long)_prom->bi_recs +
					   _prom->bi_recs->data[1]);
		prom_debug("bi_recs=0x%x\n", (unsigned long)_prom->bi_recs);
		prom_debug("new mem=0x%x\n", RELOC(klimit) - offset);
	}

	/* If we don't have birec's or didn't find them, check for an initrd
	 * using the "yaboot" way
	 */
#ifdef CONFIG_BLK_DEV_INITRD
	if ( _prom->bi_recs == NULL && r3 && r4 && r4 != 0xdeadbeef) {
		RELOC(initrd_start) = (r3 >= KERNELBASE) ? __pa(r3) : r3;
		RELOC(initrd_end) = RELOC(initrd_start) + r4;
		RELOC(initrd_below_start_ok) = 1;
	}
#endif /* CONFIG_BLK_DEV_INITRD */

	/* Default machine type. */
	_systemcfg->platform = prom_find_machine_type();

	/* On pSeries, copy the CPU hold code */
	if (_systemcfg->platform == PLATFORM_PSERIES)
		copy_and_flush(0, KERNELBASE - offset, 0x100, 0);

	/* Start storing things at klimit */
	mem = RELOC(klimit) - offset;

	/* Get the full OF pathname of the stdout device */
	p = (char *) mem;
	memset(p, 0, 256);
	call_prom("instance-to-path", 3, 1, _prom->stdout, p, 255);
	RELOC(of_stdout_device) = PTRUNRELOC(p);
	mem += strlen(p) + 1;

	getprop_rval = 1;
	prom_getprop(_prom->root, "#size-cells",
		     &getprop_rval, sizeof(getprop_rval));
	_prom->encode_phys_size = (getprop_rval == 1) ? 32 : 64;

	/* Determine which cpu is actually running right _now_ */
	if (prom_getprop(_prom->chosen, "cpu",
			 &prom_cpu, sizeof(prom_cpu)) <= 0)
		prom_panic("cannot find boot cpu");

	cpu_pkg = call_prom("instance-to-package", 1, 1, prom_cpu);
	prom_getprop(cpu_pkg, "reg", &getprop_rval, sizeof(getprop_rval));
	_prom->cpu = getprop_rval;
	lpaca[0].hw_cpu_id = _prom->cpu;

	RELOC(boot_cpuid) = 0;

	prom_debug("Booting CPU hw index = 0x%x\n", _prom->cpu);

	/* Get the boot device and translate it to a full OF pathname. */
	p = (char *) mem;
	l = prom_getprop(_prom->chosen, "bootpath", p, 1<<20);
	if (l > 0) {
		p[l] = 0;	/* should already be null-terminated */
		RELOC(bootpath) = PTRUNRELOC(p);
		mem += l + 1;
		d = (char *) mem;
		*d = 0;
		call_prom("canon", 3, 1, p, d, 1<<20);
		RELOC(bootdevice) = PTRUNRELOC(d);
		mem = DOUBLEWORD_ALIGN(mem + strlen(d) + 1);
	}

	RELOC(cmd_line[0]) = 0;
	if ((long)_prom->chosen > 0) {
		prom_getprop(_prom->chosen, "bootargs", p, sizeof(cmd_line));
		if (p != NULL && p[0] != 0)
			strlcpy(RELOC(cmd_line), p, sizeof(cmd_line));
	}

	early_cmdline_parse();

	prom_initialize_lmb();

	prom_bi_rec_reserve();

	mem = check_display(mem);

	if (_systemcfg->platform != PLATFORM_POWERMAC)
		prom_instantiate_rtas();

	/* Initialize some system info into the Naca early... */
	prom_initialize_naca();

	smt_setup();

	/* If we are on an SMP machine, then we *MUST* do the
	 * following, regardless of whether we have an SMP
	 * kernel or not.
	 */
	prom_hold_cpus(mem);

	prom_debug("after basic inits, mem=0x%x\n", mem);
#ifdef CONFIG_BLK_DEV_INITRD
	prom_debug("initrd_start=0x%x\n", RELOC(initrd_start));
	prom_debug("initrd_end=0x%x\n", RELOC(initrd_end));
#endif /* CONFIG_BLK_DEV_INITRD */
	prom_debug("copying OF device tree...\n");

	mem = copy_device_tree(mem);

	RELOC(klimit) = mem + offset;

	prom_debug("new klimit is\n");
	prom_debug("klimit=0x%x\n", RELOC(klimit));
	prom_debug(" ->mem=0x%x\n", mem);

	lmb_reserve(0, __pa(RELOC(klimit)));

#ifdef CONFIG_BLK_DEV_INITRD
	if (RELOC(initrd_start)) {
		unsigned long initrd_len;
		initrd_len = RELOC(initrd_end) - RELOC(initrd_start);

		/* Move initrd if it's where we're going to copy kernel. */
		if (RELOC(initrd_start) < __pa(RELOC(klimit))) {
			memmove((void *)mem, (void *)RELOC(initrd_start),
				initrd_len);
			RELOC(initrd_start) = mem;
			RELOC(initrd_end) = mem + initrd_len;
		}

		lmb_reserve(RELOC(initrd_start), initrd_len);
	}
#endif /* CONFIG_BLK_DEV_INITRD */

	if (_systemcfg->platform == PLATFORM_PSERIES)
		prom_initialize_tce_table();

#ifdef CONFIG_PMAC_DART
	if (_systemcfg->platform == PLATFORM_POWERMAC)
		prom_initialize_dart_table();
#endif

#ifdef CONFIG_BOOTX_TEXT
	if (_prom->disp_node) {
		prom_printf("Setting up bi display...\n");
		setup_disp_fake_bi(_prom->disp_node);
	}
#endif /* CONFIG_BOOTX_TEXT */

	prom_printf("Calling quiesce ...\n");
	call_prom("quiesce", 0, 0);
	phys = KERNELBASE - offset;

#ifdef CONFIG_BLK_DEV_INITRD
	/* If we had an initrd, we convert its address to virtual */
	if (RELOC(initrd_start)) {
		RELOC(initrd_start) = (unsigned long)__va(RELOC(initrd_start));
		RELOC(initrd_end) = (unsigned long)__va(RELOC(initrd_end));
	}
#endif /* CONFIG_BLK_DEV_INITRD */

	prom_printf("returning from prom_init\n");
	return phys;
}

/*
 * Find the device_node with a given phandle.
 */
static struct device_node * __devinit
find_phandle(phandle ph)
{
	struct device_node *np;

	for (np = allnodes; np != 0; np = np->allnext)
		if (np->linux_phandle == ph)
			return np;
	return NULL;
}

/*
 * Find the interrupt parent of a node.
 */
static struct device_node * __devinit
intr_parent(struct device_node *p)
{
	phandle *parp;

	parp = (phandle *) get_property(p, "interrupt-parent", NULL);
	if (parp == NULL)
		return p->parent;
	return find_phandle(*parp);
}

/*
 * Find out the size of each entry of the interrupts property
 * for a node.
 */
int __devinit prom_n_intr_cells(struct device_node *np)
{
	struct device_node *p;
	unsigned int *icp;

	for (p = np; (p = intr_parent(p)) != NULL; ) {
		icp = (unsigned int *)
			get_property(p, "#interrupt-cells", NULL);
		if (icp != NULL)
			return *icp;
		if (get_property(p, "interrupt-controller", NULL) != NULL
		    || get_property(p, "interrupt-map", NULL) != NULL) {
			printk("oops, node %s doesn't have #interrupt-cells\n",
			       p->full_name);
			return 1;
		}
	}
#ifdef DEBUG_IRQ
	printk("prom_n_intr_cells failed for %s\n", np->full_name);
#endif
	return 1;
}

/*
 * Map an interrupt from a device up to the platform interrupt
 * descriptor.
 */
static int __devinit
map_interrupt(unsigned int **irq, struct device_node **ictrler,
	      struct device_node *np, unsigned int *ints, int nintrc)
{
	struct device_node *p, *ipar;
	unsigned int *imap, *imask, *ip;
	int i, imaplen, match;
	int newintrc, newaddrc;
	unsigned int *reg;
	int naddrc;

	reg = (unsigned int *) get_property(np, "reg", NULL);
	naddrc = prom_n_addr_cells(np);
	p = intr_parent(np);
	while (p != NULL) {
		if (get_property(p, "interrupt-controller", NULL) != NULL)
			/* this node is an interrupt controller, stop here */
			break;
		imap = (unsigned int *)
			get_property(p, "interrupt-map", &imaplen);
		if (imap == NULL) {
			p = intr_parent(p);
			continue;
		}
		imask = (unsigned int *)
			get_property(p, "interrupt-map-mask", NULL);
		if (imask == NULL) {
			printk("oops, %s has interrupt-map but no mask\n",
			       p->full_name);
			return 0;
		}
		imaplen /= sizeof(unsigned int);
		match = 0;
		ipar = NULL;
		while (imaplen > 0 && !match) {
			/* check the child-interrupt field */
			match = 1;
			for (i = 0; i < naddrc && match; ++i)
				match = ((reg[i] ^ imap[i]) & imask[i]) == 0;
			for (; i < naddrc + nintrc && match; ++i)
				match = ((ints[i-naddrc] ^ imap[i]) & imask[i]) == 0;
			imap += naddrc + nintrc;
			imaplen -= naddrc + nintrc;
			/* grab the interrupt parent */
			ipar = find_phandle((phandle) *imap++);
			--imaplen;
			if (ipar == NULL) {
				printk("oops, no int parent %x in map of %s\n",
				       imap[-1], p->full_name);
				return 0;
			}
			/* find the parent's # addr and intr cells */
			ip = (unsigned int *)
				get_property(ipar, "#interrupt-cells", NULL);
			if (ip == NULL) {
				printk("oops, no #interrupt-cells on %s\n",
				       ipar->full_name);
				return 0;
			}
			newintrc = *ip;
			ip = (unsigned int *)
				get_property(ipar, "#address-cells", NULL);
			newaddrc = (ip == NULL)? 0: *ip;
			imap += newaddrc + newintrc;
			imaplen -= newaddrc + newintrc;
		}
		if (imaplen < 0) {
			printk("oops, error decoding int-map on %s, len=%d\n",
			       p->full_name, imaplen);
			return 0;
		}
		if (!match) {
#ifdef DEBUG_IRQ
			printk("oops, no match in %s int-map for %s\n",
			       p->full_name, np->full_name);
#endif
			return 0;
		}
		p = ipar;
		naddrc = newaddrc;
		nintrc = newintrc;
		ints = imap - nintrc;
		reg = ints - naddrc;
	}
	if (p == NULL) {
#ifdef DEBUG_IRQ
		printk("hmmm, int tree for %s doesn't have ctrler\n",
		       np->full_name);
#endif
		return 0;
	}
	*irq = ints;
	*ictrler = p;
	return nintrc;
}

static unsigned long __init
finish_node_interrupts(struct device_node *np, unsigned long mem_start,
		       int measure_only)
{
	unsigned int *ints;
	int intlen, intrcells, intrcount;
	int i, j, n;
	unsigned int *irq, virq;
	struct device_node *ic;

	ints = (unsigned int *) get_property(np, "interrupts", &intlen);
	if (ints == NULL)
		return mem_start;
	intrcells = prom_n_intr_cells(np);
	intlen /= intrcells * sizeof(unsigned int);
	np->intrs = (struct interrupt_info *) mem_start;
	mem_start += intlen * sizeof(struct interrupt_info);

	if (measure_only)
		return mem_start;

	intrcount = 0;
	for (i = 0; i < intlen; ++i, ints += intrcells) {
		n = map_interrupt(&irq, &ic, np, ints, intrcells);
		if (n <= 0)
			continue;

		/* don't map IRQ numbers under a cascaded 8259 controller */
		if (ic && device_is_compatible(ic, "chrp,iic")) {
			np->intrs[intrcount].line = irq[0];
		} else {
			virq = virt_irq_create_mapping(irq[0]);
			if (virq == NO_IRQ) {
				printk(KERN_CRIT "Could not allocate interrupt"
				       " number for %s\n", np->full_name);
				continue;
			}
			np->intrs[intrcount].line = irq_offset_up(virq);
		}

		/* We offset irq numbers for the u3 MPIC by 128 in PowerMac */
		if (systemcfg->platform == PLATFORM_POWERMAC && ic && ic->parent) {
			char *name = get_property(ic->parent, "name", NULL);
			if (name && !strcmp(name, "u3"))
				np->intrs[intrcount].line += 128;
		}
		np->intrs[intrcount].sense = 1;
		if (n > 1)
			np->intrs[intrcount].sense = irq[1];
		if (n > 2) {
			printk("hmmm, got %d intr cells for %s:", n,
			       np->full_name);
			for (j = 0; j < n; ++j)
				printk(" %d", irq[j]);
			printk("\n");
		}
		++intrcount;
	}
	np->n_intrs = intrcount;

	return mem_start;
}

static unsigned long __init
interpret_pci_props(struct device_node *np, unsigned long mem_start,
		    int naddrc, int nsizec, int measure_only)
{
	struct address_range *adr;
	struct pci_reg_property *pci_addrs;
	int i, l;

	pci_addrs = (struct pci_reg_property *)
		get_property(np, "assigned-addresses", &l);
	if (pci_addrs != 0 && l >= sizeof(struct pci_reg_property)) {
		i = 0;
		adr = (struct address_range *) mem_start;
		while ((l -= sizeof(struct pci_reg_property)) >= 0) {
			if (!measure_only) {
				adr[i].space = pci_addrs[i].addr.a_hi;
				adr[i].address = pci_addrs[i].addr.a_lo;
				adr[i].size = pci_addrs[i].size_lo;
			}
			++i;
		}
		np->addrs = adr;
		np->n_addrs = i;
		mem_start += i * sizeof(struct address_range);
	}
	return mem_start;
}

static unsigned long __init
interpret_dbdma_props(struct device_node *np, unsigned long mem_start,
		      int naddrc, int nsizec, int measure_only)
{
	struct reg_property32 *rp;
	struct address_range *adr;
	unsigned long base_address;
	int i, l;
	struct device_node *db;

	base_address = 0;
	if (!measure_only) {
		for (db = np->parent; db != NULL; db = db->parent) {
			if (!strcmp(db->type, "dbdma") && db->n_addrs != 0) {
				base_address = db->addrs[0].address;
				break;
			}
		}
	}

	rp = (struct reg_property32 *) get_property(np, "reg", &l);
	if (rp != 0 && l >= sizeof(struct reg_property32)) {
		i = 0;
		adr = (struct address_range *) mem_start;
		while ((l -= sizeof(struct reg_property32)) >= 0) {
			if (!measure_only) {
				adr[i].space = 2;
				adr[i].address = rp[i].address + base_address;
				adr[i].size = rp[i].size;
			}
			++i;
		}
		np->addrs = adr;
		np->n_addrs = i;
		mem_start += i * sizeof(struct address_range);
	}

	return mem_start;
}

static unsigned long __init
interpret_macio_props(struct device_node *np, unsigned long mem_start,
		      int naddrc, int nsizec, int measure_only)
{
	struct reg_property32 *rp;
	struct address_range *adr;
	unsigned long base_address;
	int i, l;
	struct device_node *db;

	base_address = 0;
	if (!measure_only) {
		for (db = np->parent; db != NULL; db = db->parent) {
			if (!strcmp(db->type, "mac-io") && db->n_addrs != 0) {
				base_address = db->addrs[0].address;
				break;
			}
		}
	}

	rp = (struct reg_property32 *) get_property(np, "reg", &l);
	if (rp != 0 && l >= sizeof(struct reg_property32)) {
		i = 0;
		adr = (struct address_range *) mem_start;
		while ((l -= sizeof(struct reg_property32)) >= 0) {
			if (!measure_only) {
				adr[i].space = 2;
				adr[i].address = rp[i].address + base_address;
				adr[i].size = rp[i].size;
			}
			++i;
		}
		np->addrs = adr;
		np->n_addrs = i;
		mem_start += i * sizeof(struct address_range);
	}

	return mem_start;
}

static unsigned long __init
interpret_isa_props(struct device_node *np, unsigned long mem_start,
		    int naddrc, int nsizec, int measure_only)
{
	struct isa_reg_property *rp;
	struct address_range *adr;
	int i, l;

	rp = (struct isa_reg_property *) get_property(np, "reg", &l);
	if (rp != 0 && l >= sizeof(struct isa_reg_property)) {
		i = 0;
		adr = (struct address_range *) mem_start;
		while ((l -= sizeof(struct reg_property)) >= 0) {
			if (!measure_only) {
				adr[i].space = rp[i].space;
				adr[i].address = rp[i].address;
				adr[i].size = rp[i].size;
			}
			++i;
		}
		np->addrs = adr;
		np->n_addrs = i;
		mem_start += i * sizeof(struct address_range);
	}

	return mem_start;
}

static unsigned long __init
interpret_root_props(struct device_node *np, unsigned long mem_start,
		     int naddrc, int nsizec, int measure_only)
{
	struct address_range *adr;
	int i, l;
	unsigned int *rp;
	int rpsize = (naddrc + nsizec) * sizeof(unsigned int);

	rp = (unsigned int *) get_property(np, "reg", &l);
	if (rp != 0 && l >= rpsize) {
		i = 0;
		adr = (struct address_range *) mem_start;
		while ((l -= rpsize) >= 0) {
			if (!measure_only) {
				adr[i].space = 0;
				adr[i].address = rp[naddrc - 1];
				adr[i].size = rp[naddrc + nsizec - 1];
			}
			++i;
			rp += naddrc + nsizec;
		}
		np->addrs = adr;
		np->n_addrs = i;
		mem_start += i * sizeof(struct address_range);
	}

	return mem_start;
}

static unsigned long __init
finish_node(struct device_node *np, unsigned long mem_start,
	    interpret_func *ifunc, int naddrc, int nsizec, int measure_only)
{
	struct device_node *child;
	int *ip;

	np->name = get_property(np, "name", NULL);
	np->type = get_property(np, "device_type", NULL);

	if (!np->name)
		np->name = "<NULL>";
	if (!np->type)
		np->type = "<NULL>";

	/* get the device addresses and interrupts */
	if (ifunc != NULL)
		mem_start = ifunc(np, mem_start, naddrc, nsizec, measure_only);

	mem_start = finish_node_interrupts(np, mem_start, measure_only);

	/* Look for #address-cells and #size-cells properties. */
	ip = (int *) get_property(np, "#address-cells", NULL);
	if (ip != NULL)
		naddrc = *ip;
	ip = (int *) get_property(np, "#size-cells", NULL);
	if (ip != NULL)
		nsizec = *ip;

	/* the f50 sets the name to 'display' and 'compatible' to what we
	 * expect for the name -- Cort
	 */
	if (!strcmp(np->name, "display"))
		np->name = get_property(np, "compatible", NULL);

	if (!strcmp(np->name, "device-tree") || np->parent == NULL)
		ifunc = interpret_root_props;
	else if (np->type == 0)
		ifunc = NULL;
	else if (!strcmp(np->type, "pci") || !strcmp(np->type, "vci"))
		ifunc = interpret_pci_props;
	else if (!strcmp(np->type, "dbdma"))
		ifunc = interpret_dbdma_props;
	else if (!strcmp(np->type, "mac-io") || ifunc == interpret_macio_props)
		ifunc = interpret_macio_props;
	else if (!strcmp(np->type, "isa"))
		ifunc = interpret_isa_props;
	else if (!strcmp(np->name, "uni-n") || !strcmp(np->name, "u3"))
		ifunc = interpret_root_props;
	else if (!((ifunc == interpret_dbdma_props
		    || ifunc == interpret_macio_props)
		   && (!strcmp(np->type, "escc")
		       || !strcmp(np->type, "media-bay"))))
		ifunc = NULL;

	for (child = np->child; child != NULL; child = child->sibling)
		mem_start = finish_node(child, mem_start, ifunc,
					naddrc, nsizec, measure_only);

	return mem_start;
}

/**
 * finish_device_tree is called once things are running normally
 * (i.e. with text and data mapped to the address they were linked at).
 * It traverses the device tree and fills in the name, type,
 * {n_}addrs and {n_}intrs fields of each node.
 */
void __init
finish_device_tree(void)
{
	unsigned long mem = klimit;

	virt_irq_init();

	dev_tree_size = finish_node(allnodes, 0, NULL, 0, 0, 1);
	mem = (long)abs_to_virt(lmb_alloc(dev_tree_size,
					  __alignof__(struct device_node)));
	if (finish_node(allnodes, mem, NULL, 0, 0, 0) != mem + dev_tree_size)
		BUG();
	rtas.dev = of_find_node_by_name(NULL, "rtas");
}

int
prom_n_addr_cells(struct device_node* np)
{
	int* ip;
	do {
		if (np->parent)
			np = np->parent;
		ip = (int *) get_property(np, "#address-cells", NULL);
		if (ip != NULL)
			return *ip;
	} while (np->parent);
	/* No #address-cells property for the root node, default to 1 */
	return 1;
}

int
prom_n_size_cells(struct device_node* np)
{
	int* ip;
	do {
		if (np->parent)
			np = np->parent;
		ip = (int *) get_property(np, "#size-cells", NULL);
		if (ip != NULL)
			return *ip;
	} while (np->parent);
	/* No #size-cells property for the root node, default to 1 */
	return 1;
}

/**
 * Work out the sense (active-low level / active-high edge)
 * of each interrupt from the device tree.
 */
void __init
prom_get_irq_senses(unsigned char *senses, int off, int max)
{
	struct device_node *np;
	int i, j;

	/* default to level-triggered */
	memset(senses, 1, max - off);

	for (np = allnodes; np != 0; np = np->allnext) {
		for (j = 0; j < np->n_intrs; j++) {
			i = np->intrs[j].line;
			if (i >= off && i < max)
				senses[i-off] = np->intrs[j].sense;
		}
	}
}

/**
 * Construct and return a list of the device_nodes with a given name.
 */
struct device_node *
find_devices(const char *name)
{
	struct device_node *head, **prevp, *np;

	prevp = &head;
	for (np = allnodes; np != 0; np = np->allnext) {
		if (np->name != 0 && strcasecmp(np->name, name) == 0) {
			*prevp = np;
			prevp = &np->next;
		}
	}
	*prevp = NULL;
	return head;
}

/**
 * Construct and return a list of the device_nodes with a given type.
 */
struct device_node *
find_type_devices(const char *type)
{
	struct device_node *head, **prevp, *np;

	prevp = &head;
	for (np = allnodes; np != 0; np = np->allnext) {
		if (np->type != 0 && strcasecmp(np->type, type) == 0) {
			*prevp = np;
			prevp = &np->next;
		}
	}
	*prevp = NULL;
	return head;
}

/**
 * Returns all nodes linked together
 */
struct device_node *
find_all_nodes(void)
{
	struct device_node *head, **prevp, *np;

	prevp = &head;
	for (np = allnodes; np != 0; np = np->allnext) {
		*prevp = np;
		prevp = &np->next;
	}
	*prevp = NULL;
	return head;
}

/** Checks if the given "compat" string matches one of the strings in
 * the device's "compatible" property
 */
int
device_is_compatible(struct device_node *device, const char *compat)
{
	const char* cp;
	int cplen, l;

	cp = (char *) get_property(device, "compatible", &cplen);
	if (cp == NULL)
		return 0;
	while (cplen > 0) {
		if (strncasecmp(cp, compat, strlen(compat)) == 0)
			return 1;
		l = strlen(cp) + 1;
		cp += l;
		cplen -= l;
	}

	return 0;
}


/**
 * Indicates whether the root node has a given value in its
 * compatible property.
 */
int
machine_is_compatible(const char *compat)
{
	struct device_node *root;
	int rc = 0;

	root = of_find_node_by_path("/");
	if (root) {
		rc = device_is_compatible(root, compat);
		of_node_put(root);
	}
	return rc;
}

/**
 * Construct and return a list of the device_nodes with a given type
 * and compatible property.
 */
struct device_node *
find_compatible_devices(const char *type, const char *compat)
{
	struct device_node *head, **prevp, *np;

	prevp = &head;
	for (np = allnodes; np != 0; np = np->allnext) {
		if (type != NULL
		    && !(np->type != 0 && strcasecmp(np->type, type) == 0))
			continue;
		if (device_is_compatible(np, compat)) {
			*prevp = np;
			prevp = &np->next;
		}
	}
	*prevp = NULL;
	return head;
}

/**
 * Find the device_node with a given full_name.
 */
struct device_node *
find_path_device(const char *path)
{
	struct device_node *np;

	for (np = allnodes; np != 0; np = np->allnext)
		if (np->full_name != 0 && strcasecmp(np->full_name, path) == 0)
			return np;
	return NULL;
}

/*******
 *
 * New implementation of the OF "find" APIs, return a refcounted
 * object, call of_node_put() when done.  The device tree and list
 * are protected by a rw_lock.
 *
 * Note that property management will need some locking as well,
 * this isn't dealt with yet.
 *
 *******/

/**
 *	of_find_node_by_name - Find a node by its "name" property
 *	@from:	The node to start searching from or NULL, the node
 *		you pass will not be searched, only the next one
 *		will; typically, you pass what the previous call
 *		returned. of_node_put() will be called on it
 *	@name:	The name string to match against
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_node_by_name(struct device_node *from,
	const char *name)
{
	struct device_node *np;

	read_lock(&devtree_lock);
	np = from ? from->allnext : allnodes;
	for (; np != 0; np = np->allnext)
		if (np->name != 0 && strcasecmp(np->name, name) == 0
		    && of_node_get(np))
			break;
	if (from)
		of_node_put(from);
	read_unlock(&devtree_lock);
	return np;
}
EXPORT_SYMBOL(of_find_node_by_name);

/**
 *	of_find_node_by_type - Find a node by its "device_type" property
 *	@from:	The node to start searching from or NULL, the node
 *		you pass will not be searched, only the next one
 *		will; typically, you pass what the previous call
 *		returned. of_node_put() will be called on it
 *	@name:	The type string to match against
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_node_by_type(struct device_node *from,
	const char *type)
{
	struct device_node *np;

	read_lock(&devtree_lock);
	np = from ? from->allnext : allnodes;
	for (; np != 0; np = np->allnext)
		if (np->type != 0 && strcasecmp(np->type, type) == 0
		    && of_node_get(np))
			break;
	if (from)
		of_node_put(from);
	read_unlock(&devtree_lock);
	return np;
}
EXPORT_SYMBOL(of_find_node_by_type);

/**
 *	of_find_compatible_node - Find a node based on type and one of the
 *                                tokens in its "compatible" property
 *	@from:		The node to start searching from or NULL, the node
 *			you pass will not be searched, only the next one
 *			will; typically, you pass what the previous call
 *			returned. of_node_put() will be called on it
 *	@type:		The type string to match "device_type" or NULL to ignore
 *	@compatible:	The string to match to one of the tokens in the device
 *			"compatible" list.
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_compatible_node(struct device_node *from,
	const char *type, const char *compatible)
{
	struct device_node *np;

	read_lock(&devtree_lock);
	np = from ? from->allnext : allnodes;
	for (; np != 0; np = np->allnext) {
		if (type != NULL
		    && !(np->type != 0 && strcasecmp(np->type, type) == 0))
			continue;
		if (device_is_compatible(np, compatible) && of_node_get(np))
			break;
	}
	if (from)
		of_node_put(from);
	read_unlock(&devtree_lock);
	return np;
}
EXPORT_SYMBOL(of_find_compatible_node);

/**
 *	of_find_node_by_path - Find a node matching a full OF path
 *	@path:	The full path to match
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_node_by_path(const char *path)
{
	struct device_node *np = allnodes;

	read_lock(&devtree_lock);
	for (; np != 0; np = np->allnext)
		if (np->full_name != 0 && strcasecmp(np->full_name, path) == 0
		    && of_node_get(np))
			break;
	read_unlock(&devtree_lock);
	return np;
}
EXPORT_SYMBOL(of_find_node_by_path);

/**
 *	of_find_all_nodes - Get next node in global list
 *	@prev:	Previous node or NULL to start iteration
 *		of_node_put() will be called on it
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_find_all_nodes(struct device_node *prev)
{
	struct device_node *np;

	read_lock(&devtree_lock);
	np = prev ? prev->allnext : allnodes;
	for (; np != 0; np = np->allnext)
		if (of_node_get(np))
			break;
	if (prev)
		of_node_put(prev);
	read_unlock(&devtree_lock);
	return np;
}
EXPORT_SYMBOL(of_find_all_nodes);

/**
 *	of_get_parent - Get a node's parent if any
 *	@node:	Node to get parent
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_get_parent(const struct device_node *node)
{
	struct device_node *np;

	if (!node)
		return NULL;

	read_lock(&devtree_lock);
	np = of_node_get(node->parent);
	read_unlock(&devtree_lock);
	return np;
}
EXPORT_SYMBOL(of_get_parent);

/**
 *	of_get_next_child - Iterate a node childs
 *	@node:	parent node
 *	@prev:	previous child of the parent node, or NULL to get first
 *
 *	Returns a node pointer with refcount incremented, use
 *	of_node_put() on it when done.
 */
struct device_node *of_get_next_child(const struct device_node *node,
	struct device_node *prev)
{
	struct device_node *next;

	read_lock(&devtree_lock);
	next = prev ? prev->sibling : node->child;
	for (; next != 0; next = next->sibling)
		if (of_node_get(next))
			break;
	if (prev)
		of_node_put(prev);
	read_unlock(&devtree_lock);
	return next;
}
EXPORT_SYMBOL(of_get_next_child);

/**
 *	of_node_get - Increment refcount of a node
 *	@node:	Node to inc refcount, NULL is supported to
 *		simplify writing of callers
 *
 *	Returns the node itself or NULL if gone.
 */
struct device_node *of_node_get(struct device_node *node)
{
	if (node && !OF_IS_STALE(node)) {
		atomic_inc(&node->_users);
		return node;
	}
	return NULL;
}
EXPORT_SYMBOL(of_node_get);

/**
 *	of_node_cleanup - release a dynamically allocated node
 *	@arg:  Node to be released
 */
static void of_node_cleanup(struct device_node *node)
{
	struct property *prop = node->properties;

	if (!OF_IS_DYNAMIC(node))
		return;
	while (prop) {
		struct property *next = prop->next;
		kfree(prop->name);
		kfree(prop->value);
		kfree(prop);
		prop = next;
	}
	kfree(node->intrs);
	kfree(node->addrs);
	kfree(node->full_name);
	kfree(node);
}

/**
 *	of_node_put - Decrement refcount of a node
 *	@node:	Node to dec refcount, NULL is supported to
 *		simplify writing of callers
 *
 */
void of_node_put(struct device_node *node)
{
	if (!node)
		return;

	WARN_ON(0 == atomic_read(&node->_users));

	if (OF_IS_STALE(node)) {
		if (atomic_dec_and_test(&node->_users)) {
			of_node_cleanup(node);
			return;
		}
	}
	else
		atomic_dec(&node->_users);
}
EXPORT_SYMBOL(of_node_put);

/**
 *	derive_parent - basically like dirname(1)
 *	@path:  the full_name of a node to be added to the tree
 *
 *	Returns the node which should be the parent of the node
 *	described by path.  E.g., for path = "/foo/bar", returns
 *	the node with full_name = "/foo".
 */
static struct device_node *derive_parent(const char *path)
{
	struct device_node *parent = NULL;
	char *parent_path = "/";
	size_t parent_path_len = strrchr(path, '/') - path + 1;

	/* reject if path is "/" */
	if (!strcmp(path, "/"))
		return NULL;

	if (strrchr(path, '/') != path) {
		parent_path = kmalloc(parent_path_len, GFP_KERNEL);
		if (!parent_path)
			return NULL;
		strlcpy(parent_path, path, parent_path_len);
	}
	parent = of_find_node_by_path(parent_path);
	if (strcmp(parent_path, "/"))
		kfree(parent_path);
	return parent;
}

/*
 * Routines for "runtime" addition and removal of device tree nodes.
 */
#ifdef CONFIG_PROC_DEVICETREE
/*
 * Add a node to /proc/device-tree.
 */
static void add_node_proc_entries(struct device_node *np)
{
	struct proc_dir_entry *ent;

	ent = proc_mkdir(strrchr(np->full_name, '/') + 1, np->parent->pde);
	if (ent)
		proc_device_tree_add_node(np, ent);
}

static void remove_node_proc_entries(struct device_node *np)
{
	struct property *pp = np->properties;
	struct device_node *parent = np->parent;

	while (pp) {
		remove_proc_entry(pp->name, np->pde);
		pp = pp->next;
	}

	/* Assuming that symlinks have the same parent directory as
	 * np->pde.
	 */
	if (np->name_link)
		remove_proc_entry(np->name_link->name, parent->pde);
	if (np->addr_link)
		remove_proc_entry(np->addr_link->name, parent->pde);
	if (np->pde)
		remove_proc_entry(np->pde->name, parent->pde);
}
#else /* !CONFIG_PROC_DEVICETREE */
static void add_node_proc_entries(struct device_node *np)
{
	return;
}

static void remove_node_proc_entries(struct device_node *np)
{
	return;
}
#endif /* CONFIG_PROC_DEVICETREE */

/*
 * Fix up n_intrs and intrs fields in a new device node
 *
 */
static int of_finish_dynamic_node_interrupts(struct device_node *node)
{
	int intrcells, intlen, i;
	unsigned *irq, *ints, virq;
	struct device_node *ic;

	ints = (unsigned int *)get_property(node, "interrupts", &intlen);
	intrcells = prom_n_intr_cells(node);
	intlen /= intrcells * sizeof(unsigned int);
	node->n_intrs = intlen;
	node->intrs = kmalloc(sizeof(struct interrupt_info) * intlen,
			      GFP_KERNEL);
	if (!node->intrs)
		return -ENOMEM;

	for (i = 0; i < intlen; ++i) {
		int n, j;
		node->intrs[i].line = 0;
		node->intrs[i].sense = 1;
		n = map_interrupt(&irq, &ic, node, ints, intrcells);
		if (n <= 0)
			continue;
		virq = virt_irq_create_mapping(irq[0]);
		if (virq == NO_IRQ) {
			printk(KERN_CRIT "Could not allocate interrupt "
			       "number for %s\n", node->full_name);
			return -ENOMEM;
		}
		node->intrs[i].line = irq_offset_up(virq);
		if (n > 1)
			node->intrs[i].sense = irq[1];
		if (n > 2) {
			printk(KERN_DEBUG "hmmm, got %d intr cells for %s:", n,
			       node->full_name);
			for (j = 0; j < n; ++j)
				printk(" %d", irq[j]);
			printk("\n");
		}
		ints += intrcells;
	}
	return 0;
}

/*
 * Fix up the uninitialized fields in a new device node:
 * name, type, n_addrs, addrs, n_intrs, intrs, and pci-specific fields
 *
 * A lot of boot-time code is duplicated here, because functions such
 * as finish_node_interrupts, interpret_pci_props, etc. cannot use the
 * slab allocator.
 *
 * This should probably be split up into smaller chunks.
 */

static int of_finish_dynamic_node(struct device_node *node)
{
	struct device_node *parent = of_get_parent(node);
	u32 *regs;
	int err = 0;
	phandle *ibm_phandle;

	node->name = get_property(node, "name", NULL);
	node->type = get_property(node, "device_type", NULL);

	if (!parent) {
		err = -ENODEV;
		goto out;
	}

	/* We don't support that function on PowerMac, at least
	 * not yet
	 */
	if (systemcfg->platform == PLATFORM_POWERMAC)
		return -ENODEV;

	/* fix up new node's linux_phandle field */
	if ((ibm_phandle = (unsigned int *)get_property(node, "ibm,phandle", NULL)))
		node->linux_phandle = *ibm_phandle;

	/* do the work of interpret_pci_props */
	if (parent->type && !strcmp(parent->type, "pci")) {
		struct address_range *adr;
		struct pci_reg_property *pci_addrs;
		int i, l;

		pci_addrs = (struct pci_reg_property *)
			get_property(node, "assigned-addresses", &l);
		if (pci_addrs != 0 && l >= sizeof(struct pci_reg_property)) {
			i = 0;
			adr = kmalloc(sizeof(struct address_range) * 
				      (l / sizeof(struct pci_reg_property)),
				      GFP_KERNEL);
			if (!adr) {
				err = -ENOMEM;
				goto out;
			}
			while ((l -= sizeof(struct pci_reg_property)) >= 0) {
				adr[i].space = pci_addrs[i].addr.a_hi;
				adr[i].address = pci_addrs[i].addr.a_lo;
				adr[i].size = pci_addrs[i].size_lo;
				++i;
			}
			node->addrs = adr;
			node->n_addrs = i;
		}
	}

	/* now do the work of finish_node_interrupts */
	if (get_property(node, "interrupts", NULL)) {
		err = of_finish_dynamic_node_interrupts(node);
		if (err) goto out;
	}

	/* now do the rough equivalent of update_dn_pci_info, this
	 * probably is not correct for phb's, but should work for
	 * IOAs and slots.
	 */

	node->phb = parent->phb;

	regs = (u32 *)get_property(node, "reg", NULL);
	if (regs) {
		node->busno = (regs[0] >> 16) & 0xff;
		node->devfn = (regs[0] >> 8) & 0xff;
	}

	/* fixing up iommu_table */

	if (strcmp(node->name, "pci") == 0 &&
	    get_property(node, "ibm,dma-window", NULL)) {
		node->bussubno = node->busno;
		iommu_devnode_init(node);
	} else
		node->iommu_table = parent->iommu_table;

out:
	of_node_put(parent);
	return err;
}

/*
 * Given a path and a property list, construct an OF device node, add
 * it to the device tree and global list, and place it in
 * /proc/device-tree.  This function may sleep.
 */
int of_add_node(const char *path, struct property *proplist)
{
	struct device_node *np;
	int err = 0;

	np = kmalloc(sizeof(struct device_node), GFP_KERNEL);
	if (!np)
		return -ENOMEM;

	memset(np, 0, sizeof(*np));

	np->full_name = kmalloc(strlen(path) + 1, GFP_KERNEL);
	if (!np->full_name) {
		kfree(np);
		return -ENOMEM;
	}
	strcpy(np->full_name, path);

	np->properties = proplist;
	OF_MARK_DYNAMIC(np);
	of_node_get(np);
	np->parent = derive_parent(path);
	if (!np->parent) {
		kfree(np);
		return -EINVAL; /* could also be ENOMEM, though */
	}

	if (0 != (err = of_finish_dynamic_node(np))) {
		kfree(np);
		return err;
	}

	write_lock(&devtree_lock);
	np->sibling = np->parent->child;
	np->allnext = allnodes;
	np->parent->child = np;
	allnodes = np;
	write_unlock(&devtree_lock);

	add_node_proc_entries(np);

	of_node_put(np->parent);
	of_node_put(np);
	return 0;
}

/*
 * Remove an OF device node from the system.
 * Caller should have already "gotten" np.
 */
int of_remove_node(struct device_node *np)
{
	struct device_node *parent, *child;

	parent = of_get_parent(np);
	if (!parent)
		return -EINVAL;

	if ((child = of_get_next_child(np, NULL))) {
		of_node_put(child);
		return -EBUSY;
	}

	write_lock(&devtree_lock);
	OF_MARK_STALE(np);
	remove_node_proc_entries(np);
	if (allnodes == np)
		allnodes = np->allnext;
	else {
		struct device_node *prev;
		for (prev = allnodes;
		     prev->allnext != np;
		     prev = prev->allnext)
			;
		prev->allnext = np->allnext;
	}

	if (parent->child == np)
		parent->child = np->sibling;
	else {
		struct device_node *prevsib;
		for (prevsib = np->parent->child;
		     prevsib->sibling != np;
		     prevsib = prevsib->sibling)
			;
		prevsib->sibling = np->sibling;
	}
	write_unlock(&devtree_lock);
	of_node_put(parent);
	return 0;
}

/*
 * Find a property with a given name for a given node
 * and return the value.
 */
unsigned char *
get_property(struct device_node *np, const char *name, int *lenp)
{
	struct property *pp;

	for (pp = np->properties; pp != 0; pp = pp->next)
		if (strcmp(pp->name, name) == 0) {
			if (lenp != 0)
				*lenp = pp->length;
			return pp->value;
		}
	return NULL;
}

/*
 * Add a property to a node
 */
void
prom_add_property(struct device_node* np, struct property* prop)
{
	struct property **next = &np->properties;

	prop->next = NULL;	
	while (*next)
		next = &(*next)->next;
	*next = prop;
}

#if 0
void
print_properties(struct device_node *np)
{
	struct property *pp;
	char *cp;
	int i, n;

	for (pp = np->properties; pp != 0; pp = pp->next) {
		printk(KERN_INFO "%s", pp->name);
		for (i = strlen(pp->name); i < 16; ++i)
			printk(" ");
		cp = (char *) pp->value;
		for (i = pp->length; i > 0; --i, ++cp)
			if ((i > 1 && (*cp < 0x20 || *cp > 0x7e))
			    || (i == 1 && *cp != 0))
				break;
		if (i == 0 && pp->length > 1) {
			/* looks like a string */
			printk(" %s\n", (char *) pp->value);
		} else {
			/* dump it in hex */
			n = pp->length;
			if (n > 64)
				n = 64;
			if (pp->length % 4 == 0) {
				unsigned int *p = (unsigned int *) pp->value;

				n /= 4;
				for (i = 0; i < n; ++i) {
					if (i != 0 && (i % 4) == 0)
						printk("\n                ");
					printk(" %08x", *p++);
				}
			} else {
				unsigned char *bp = pp->value;

				for (i = 0; i < n; ++i) {
					if (i != 0 && (i % 16) == 0)
						printk("\n                ");
					printk(" %02x", *bp++);
				}
			}
			printk("\n");
			if (pp->length > 64)
				printk("                 ... (length = %d)\n",
				       pp->length);
		}
	}
}
#endif
