/*
 * arch/sh/kernel/setup.c
 *
 * This file handles the architecture-dependent parts of initialization
 *
 *  Copyright (C) 1999  Niibe Yutaka
 *  Copyright (C) 2002 - 2007 Paul Mundt
 */
#include <linux/screen_info.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/bootmem.h>
#include <linux/console.h>
#include <linux/seq_file.h>
#include <linux/root_dev.h>
#include <linux/utsname.h>
#include <linux/nodemask.h>
#include <linux/cpu.h>
#include <linux/pfn.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/kexec.h>
#include <linux/module.h>
#include <asm/uaccess.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/sections.h>
#include <asm/irq.h>
#include <asm/setup.h>
#include <asm/clock.h>
#include <asm/mmu_context.h>

extern void * __rd_start, * __rd_end;

/*
 * Machine setup..
 */

/*
 * Initialize loops_per_jiffy as 10000000 (1000MIPS).
 * This value will be used at the very early stage of serial setup.
 * The bigger value means no problem.
 */
struct sh_cpuinfo boot_cpu_data = { CPU_SH_NONE, 10000000, };

/*
 * The machine vector. First entry in .machvec.init, or clobbered by
 * sh_mv= on the command line, prior to .machvec.init teardown.
 */
struct sh_machine_vector sh_mv = { .mv_name = "generic", };

#ifdef CONFIG_VT
struct screen_info screen_info;
#endif

extern int root_mountflags;

/*
 * This is set up by the setup-routine at boot-time
 */
#define PARAM	((unsigned char *)empty_zero_page)

#define MOUNT_ROOT_RDONLY (*(unsigned long *) (PARAM+0x000))
#define RAMDISK_FLAGS (*(unsigned long *) (PARAM+0x004))
#define ORIG_ROOT_DEV (*(unsigned long *) (PARAM+0x008))
#define LOADER_TYPE (*(unsigned long *) (PARAM+0x00c))
#define INITRD_START (*(unsigned long *) (PARAM+0x010))
#define INITRD_SIZE (*(unsigned long *) (PARAM+0x014))
/* ... */
#define COMMAND_LINE ((char *) (PARAM+0x100))

#define RAMDISK_IMAGE_START_MASK	0x07FF
#define RAMDISK_PROMPT_FLAG		0x8000
#define RAMDISK_LOAD_FLAG		0x4000

static char __initdata command_line[COMMAND_LINE_SIZE] = { 0, };

static struct resource code_resource = { .name = "Kernel code", };
static struct resource data_resource = { .name = "Kernel data", };

unsigned long memory_start;
EXPORT_SYMBOL(memory_start);

unsigned long memory_end;
EXPORT_SYMBOL(memory_end);

static int __init early_parse_mem(char *p)
{
	unsigned long size;

	memory_start = (unsigned long)PAGE_OFFSET+__MEMORY_START;
	size = memparse(p, &p);
	memory_end = memory_start + size;

	return 0;
}
early_param("mem", early_parse_mem);

/*
 * Register fully available low RAM pages with the bootmem allocator.
 */
static void __init register_bootmem_low_pages(void)
{
	unsigned long curr_pfn, last_pfn, pages;

	/*
	 * We are rounding up the start address of usable memory:
	 */
	curr_pfn = PFN_UP(__MEMORY_START);

	/*
	 * ... and at the end of the usable range downwards:
	 */
	last_pfn = PFN_DOWN(__pa(memory_end));

	if (last_pfn > max_low_pfn)
		last_pfn = max_low_pfn;

	pages = last_pfn - curr_pfn;
	free_bootmem(PFN_PHYS(curr_pfn), PFN_PHYS(pages));
}

void __init setup_bootmem_allocator(unsigned long free_pfn)
{
	unsigned long bootmap_size;

	/*
	 * Find a proper area for the bootmem bitmap. After this
	 * bootstrap step all allocations (until the page allocator
	 * is intact) must be done via bootmem_alloc().
	 */
	bootmap_size = init_bootmem_node(NODE_DATA(0), free_pfn,
					 min_low_pfn, max_low_pfn);

	add_active_range(0, min_low_pfn, max_low_pfn);
	register_bootmem_low_pages();

	node_set_online(0);

	/*
	 * Reserve the kernel text and
	 * Reserve the bootmem bitmap. We do this in two steps (first step
	 * was init_bootmem()), because this catches the (definitely buggy)
	 * case of us accidentally initializing the bootmem allocator with
	 * an invalid RAM area.
	 */
	reserve_bootmem(__MEMORY_START+PAGE_SIZE,
		(PFN_PHYS(free_pfn)+bootmap_size+PAGE_SIZE-1)-__MEMORY_START);

	/*
	 * reserve physical page 0 - it's a special BIOS page on many boxes,
	 * enabling clean reboots, SMP operation, laptop functions.
	 */
	reserve_bootmem(__MEMORY_START, PAGE_SIZE);

	sparse_memory_present_with_active_regions(0);

#ifdef CONFIG_BLK_DEV_INITRD
	ROOT_DEV = MKDEV(RAMDISK_MAJOR, 0);
	if (&__rd_start != &__rd_end) {
		LOADER_TYPE = 1;
		INITRD_START = PHYSADDR((unsigned long)&__rd_start) -
					__MEMORY_START;
		INITRD_SIZE = (unsigned long)&__rd_end -
			      (unsigned long)&__rd_start;
	}

	if (LOADER_TYPE && INITRD_START) {
		if (INITRD_START + INITRD_SIZE <= (max_low_pfn << PAGE_SHIFT)) {
			reserve_bootmem(INITRD_START + __MEMORY_START,
					INITRD_SIZE);
			initrd_start = INITRD_START + PAGE_OFFSET +
					__MEMORY_START;
			initrd_end = initrd_start + INITRD_SIZE;
		} else {
			printk("initrd extends beyond end of memory "
			    "(0x%08lx > 0x%08lx)\ndisabling initrd\n",
				    INITRD_START + INITRD_SIZE,
				    max_low_pfn << PAGE_SHIFT);
			initrd_start = 0;
		}
	}
#endif
#ifdef CONFIG_KEXEC
	if (crashk_res.start != crashk_res.end)
		reserve_bootmem(crashk_res.start,
			crashk_res.end - crashk_res.start + 1);
#endif
}

#ifndef CONFIG_NEED_MULTIPLE_NODES
static void __init setup_memory(void)
{
	unsigned long start_pfn;

	/*
	 * Partially used pages are not usable - thus
	 * we are rounding upwards:
	 */
	start_pfn = PFN_UP(__pa(_end));
	setup_bootmem_allocator(start_pfn);
}
#else
extern void __init setup_memory(void);
#endif

void __init setup_arch(char **cmdline_p)
{
	enable_mmu();

	ROOT_DEV = old_decode_dev(ORIG_ROOT_DEV);

#ifdef CONFIG_BLK_DEV_RAM
	rd_image_start = RAMDISK_FLAGS & RAMDISK_IMAGE_START_MASK;
	rd_prompt = ((RAMDISK_FLAGS & RAMDISK_PROMPT_FLAG) != 0);
	rd_doload = ((RAMDISK_FLAGS & RAMDISK_LOAD_FLAG) != 0);
#endif

	if (!MOUNT_ROOT_RDONLY)
		root_mountflags &= ~MS_RDONLY;
	init_mm.start_code = (unsigned long) _text;
	init_mm.end_code = (unsigned long) _etext;
	init_mm.end_data = (unsigned long) _edata;
	init_mm.brk = (unsigned long) _end;

	code_resource.start = virt_to_phys(_text);
	code_resource.end = virt_to_phys(_etext)-1;
	data_resource.start = virt_to_phys(_etext);
	data_resource.end = virt_to_phys(_edata)-1;

	memory_start = (unsigned long)PAGE_OFFSET+__MEMORY_START;
	memory_end = memory_start + __MEMORY_SIZE;

#ifdef CONFIG_CMDLINE_BOOL
	strlcpy(command_line, CONFIG_CMDLINE, sizeof(command_line));
#else
	strlcpy(command_line, COMMAND_LINE, sizeof(command_line));
#endif

	/* Save unparsed command line copy for /proc/cmdline */
	memcpy(boot_command_line, command_line, COMMAND_LINE_SIZE);
	*cmdline_p = command_line;

	parse_early_param();

	sh_mv_setup();

	/*
	 * Find the highest page frame number we have available
	 */
	max_pfn = PFN_DOWN(__pa(memory_end));

	/*
	 * Determine low and high memory ranges:
	 */
	max_low_pfn = max_pfn;
	min_low_pfn = __MEMORY_START >> PAGE_SHIFT;

	nodes_clear(node_online_map);

	/* Setup bootmem with available RAM */
	setup_memory();
	sparse_init();

#ifdef CONFIG_DUMMY_CONSOLE
	conswitchp = &dummy_con;
#endif

	/* Perform the machine specific initialisation */
	if (likely(sh_mv.mv_setup))
		sh_mv.mv_setup(cmdline_p);

	paging_init();
}

static const char *cpu_name[] = {
	[CPU_SH7206]	= "SH7206",	[CPU_SH7619]	= "SH7619",
	[CPU_SH7705]	= "SH7705",	[CPU_SH7706]	= "SH7706",
	[CPU_SH7707]	= "SH7707",	[CPU_SH7708]	= "SH7708",
	[CPU_SH7709]	= "SH7709",	[CPU_SH7710]	= "SH7710",
	[CPU_SH7712]	= "SH7712",
	[CPU_SH7729]	= "SH7729",	[CPU_SH7750]	= "SH7750",
	[CPU_SH7750S]	= "SH7750S",	[CPU_SH7750R]	= "SH7750R",
	[CPU_SH7751]	= "SH7751",	[CPU_SH7751R]	= "SH7751R",
	[CPU_SH7760]	= "SH7760",
	[CPU_ST40RA]	= "ST40RA",	[CPU_ST40GX1]	= "ST40GX1",
	[CPU_SH4_202]	= "SH4-202",	[CPU_SH4_501]	= "SH4-501",
	[CPU_SH7770]	= "SH7770",	[CPU_SH7780]	= "SH7780",
	[CPU_SH7781]	= "SH7781",	[CPU_SH7343]	= "SH7343",
	[CPU_SH7785]	= "SH7785",	[CPU_SH7722]	= "SH7722",
	[CPU_SHX3]	= "SH-X3",	[CPU_SH_NONE]	= "Unknown"
};

const char *get_cpu_subtype(struct sh_cpuinfo *c)
{
	return cpu_name[c->type];
}

#ifdef CONFIG_PROC_FS
/* Symbolic CPU flags, keep in sync with asm/cpu-features.h */
static const char *cpu_flags[] = {
	"none", "fpu", "p2flush", "mmuassoc", "dsp", "perfctr",
	"ptea", "llsc", "l2", "op32", NULL
};

static void show_cpuflags(struct seq_file *m, struct sh_cpuinfo *c)
{
	unsigned long i;

	seq_printf(m, "cpu flags\t:");

	if (!c->flags) {
		seq_printf(m, " %s\n", cpu_flags[0]);
		return;
	}

	for (i = 0; cpu_flags[i]; i++)
		if ((c->flags & (1 << i)))
			seq_printf(m, " %s", cpu_flags[i+1]);

	seq_printf(m, "\n");
}

static void show_cacheinfo(struct seq_file *m, const char *type,
			   struct cache_info info)
{
	unsigned int cache_size;

	cache_size = info.ways * info.sets * info.linesz;

	seq_printf(m, "%s size\t: %2dKiB (%d-way)\n",
		   type, cache_size >> 10, info.ways);
}

/*
 *	Get CPU information for use by the procfs.
 */
static int show_cpuinfo(struct seq_file *m, void *v)
{
	struct sh_cpuinfo *c = v;
	unsigned int cpu = c - cpu_data;

	if (!cpu_online(cpu))
		return 0;

	if (cpu == 0)
		seq_printf(m, "machine\t\t: %s\n", get_system_type());

	seq_printf(m, "processor\t: %d\n", cpu);
	seq_printf(m, "cpu family\t: %s\n", init_utsname()->machine);
	seq_printf(m, "cpu type\t: %s\n", get_cpu_subtype(c));

	show_cpuflags(m, c);

	seq_printf(m, "cache type\t: ");

	/*
	 * Check for what type of cache we have, we support both the
	 * unified cache on the SH-2 and SH-3, as well as the harvard
	 * style cache on the SH-4.
	 */
	if (c->icache.flags & SH_CACHE_COMBINED) {
		seq_printf(m, "unified\n");
		show_cacheinfo(m, "cache", c->icache);
	} else {
		seq_printf(m, "split (harvard)\n");
		show_cacheinfo(m, "icache", c->icache);
		show_cacheinfo(m, "dcache", c->dcache);
	}

	/* Optional secondary cache */
	if (c->flags & CPU_HAS_L2_CACHE)
		show_cacheinfo(m, "scache", c->scache);

	seq_printf(m, "bogomips\t: %lu.%02lu\n",
		     c->loops_per_jiffy/(500000/HZ),
		     (c->loops_per_jiffy/(5000/HZ)) % 100);

	return 0;
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	return *pos < NR_CPUS ? cpu_data + *pos : NULL;
}
static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return c_start(m, pos);
}
static void c_stop(struct seq_file *m, void *v)
{
}
struct seq_operations cpuinfo_op = {
	.start	= c_start,
	.next	= c_next,
	.stop	= c_stop,
	.show	= show_cpuinfo,
};
#endif /* CONFIG_PROC_FS */
