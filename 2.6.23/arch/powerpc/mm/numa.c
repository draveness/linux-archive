/*
 * pSeries NUMA support
 *
 * Copyright (C) 2002 Anton Blanchard <anton@au.ibm.com>, IBM
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#include <linux/threads.h>
#include <linux/bootmem.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/nodemask.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <asm/sparsemem.h>
#include <asm/lmb.h>
#include <asm/system.h>
#include <asm/smp.h>

static int numa_enabled = 1;

static int numa_debug;
#define dbg(args...) if (numa_debug) { printk(KERN_INFO args); }

int numa_cpu_lookup_table[NR_CPUS];
cpumask_t numa_cpumask_lookup_table[MAX_NUMNODES];
struct pglist_data *node_data[MAX_NUMNODES];

EXPORT_SYMBOL(numa_cpu_lookup_table);
EXPORT_SYMBOL(numa_cpumask_lookup_table);
EXPORT_SYMBOL(node_data);

static bootmem_data_t __initdata plat_node_bdata[MAX_NUMNODES];
static int min_common_depth;
static int n_mem_addr_cells, n_mem_size_cells;

static void __cpuinit map_cpu_to_node(int cpu, int node)
{
	numa_cpu_lookup_table[cpu] = node;

	dbg("adding cpu %d to node %d\n", cpu, node);

	if (!(cpu_isset(cpu, numa_cpumask_lookup_table[node])))
		cpu_set(cpu, numa_cpumask_lookup_table[node]);
}

#ifdef CONFIG_HOTPLUG_CPU
static void unmap_cpu_from_node(unsigned long cpu)
{
	int node = numa_cpu_lookup_table[cpu];

	dbg("removing cpu %lu from node %d\n", cpu, node);

	if (cpu_isset(cpu, numa_cpumask_lookup_table[node])) {
		cpu_clear(cpu, numa_cpumask_lookup_table[node]);
	} else {
		printk(KERN_ERR "WARNING: cpu %lu not found in node %d\n",
		       cpu, node);
	}
}
#endif /* CONFIG_HOTPLUG_CPU */

static struct device_node * __cpuinit find_cpu_node(unsigned int cpu)
{
	unsigned int hw_cpuid = get_hard_smp_processor_id(cpu);
	struct device_node *cpu_node = NULL;
	const unsigned int *interrupt_server, *reg;
	int len;

	while ((cpu_node = of_find_node_by_type(cpu_node, "cpu")) != NULL) {
		/* Try interrupt server first */
		interrupt_server = of_get_property(cpu_node,
					"ibm,ppc-interrupt-server#s", &len);

		len = len / sizeof(u32);

		if (interrupt_server && (len > 0)) {
			while (len--) {
				if (interrupt_server[len] == hw_cpuid)
					return cpu_node;
			}
		} else {
			reg = of_get_property(cpu_node, "reg", &len);
			if (reg && (len > 0) && (reg[0] == hw_cpuid))
				return cpu_node;
		}
	}

	return NULL;
}

/* must hold reference to node during call */
static const int *of_get_associativity(struct device_node *dev)
{
	return of_get_property(dev, "ibm,associativity", NULL);
}

/* Returns nid in the range [0..MAX_NUMNODES-1], or -1 if no useful numa
 * info is found.
 */
static int of_node_to_nid_single(struct device_node *device)
{
	int nid = -1;
	const unsigned int *tmp;

	if (min_common_depth == -1)
		goto out;

	tmp = of_get_associativity(device);
	if (!tmp)
		goto out;

	if (tmp[0] >= min_common_depth)
		nid = tmp[min_common_depth];

	/* POWER4 LPAR uses 0xffff as invalid node */
	if (nid == 0xffff || nid >= MAX_NUMNODES)
		nid = -1;
out:
	return nid;
}

/* Walk the device tree upwards, looking for an associativity id */
int of_node_to_nid(struct device_node *device)
{
	struct device_node *tmp;
	int nid = -1;

	of_node_get(device);
	while (device) {
		nid = of_node_to_nid_single(device);
		if (nid != -1)
			break;

	        tmp = device;
		device = of_get_parent(tmp);
		of_node_put(tmp);
	}
	of_node_put(device);

	return nid;
}
EXPORT_SYMBOL_GPL(of_node_to_nid);

/*
 * In theory, the "ibm,associativity" property may contain multiple
 * associativity lists because a resource may be multiply connected
 * into the machine.  This resource then has different associativity
 * characteristics relative to its multiple connections.  We ignore
 * this for now.  We also assume that all cpu and memory sets have
 * their distances represented at a common level.  This won't be
 * true for hierarchical NUMA.
 *
 * In any case the ibm,associativity-reference-points should give
 * the correct depth for a normal NUMA system.
 *
 * - Dave Hansen <haveblue@us.ibm.com>
 */
static int __init find_min_common_depth(void)
{
	int depth;
	const unsigned int *ref_points;
	struct device_node *rtas_root;
	unsigned int len;

	rtas_root = of_find_node_by_path("/rtas");

	if (!rtas_root)
		return -1;

	/*
	 * this property is 2 32-bit integers, each representing a level of
	 * depth in the associativity nodes.  The first is for an SMP
	 * configuration (should be all 0's) and the second is for a normal
	 * NUMA configuration.
	 */
	ref_points = of_get_property(rtas_root,
			"ibm,associativity-reference-points", &len);

	if ((len >= 1) && ref_points) {
		depth = ref_points[1];
	} else {
		dbg("NUMA: ibm,associativity-reference-points not found.\n");
		depth = -1;
	}
	of_node_put(rtas_root);

	return depth;
}

static void __init get_n_mem_cells(int *n_addr_cells, int *n_size_cells)
{
	struct device_node *memory = NULL;

	memory = of_find_node_by_type(memory, "memory");
	if (!memory)
		panic("numa.c: No memory nodes found!");

	*n_addr_cells = of_n_addr_cells(memory);
	*n_size_cells = of_n_size_cells(memory);
	of_node_put(memory);
}

static unsigned long __devinit read_n_cells(int n, const unsigned int **buf)
{
	unsigned long result = 0;

	while (n--) {
		result = (result << 32) | **buf;
		(*buf)++;
	}
	return result;
}

/*
 * Figure out to which domain a cpu belongs and stick it there.
 * Return the id of the domain used.
 */
static int __cpuinit numa_setup_cpu(unsigned long lcpu)
{
	int nid = 0;
	struct device_node *cpu = find_cpu_node(lcpu);

	if (!cpu) {
		WARN_ON(1);
		goto out;
	}

	nid = of_node_to_nid_single(cpu);

	if (nid < 0 || !node_online(nid))
		nid = any_online_node(NODE_MASK_ALL);
out:
	map_cpu_to_node(lcpu, nid);

	of_node_put(cpu);

	return nid;
}

static int __cpuinit cpu_numa_callback(struct notifier_block *nfb,
			     unsigned long action,
			     void *hcpu)
{
	unsigned long lcpu = (unsigned long)hcpu;
	int ret = NOTIFY_DONE;

	switch (action) {
	case CPU_UP_PREPARE:
	case CPU_UP_PREPARE_FROZEN:
		numa_setup_cpu(lcpu);
		ret = NOTIFY_OK;
		break;
#ifdef CONFIG_HOTPLUG_CPU
	case CPU_DEAD:
	case CPU_DEAD_FROZEN:
	case CPU_UP_CANCELED:
	case CPU_UP_CANCELED_FROZEN:
		unmap_cpu_from_node(lcpu);
		break;
		ret = NOTIFY_OK;
#endif
	}
	return ret;
}

/*
 * Check and possibly modify a memory region to enforce the memory limit.
 *
 * Returns the size the region should have to enforce the memory limit.
 * This will either be the original value of size, a truncated value,
 * or zero. If the returned value of size is 0 the region should be
 * discarded as it lies wholy above the memory limit.
 */
static unsigned long __init numa_enforce_memory_limit(unsigned long start,
						      unsigned long size)
{
	/*
	 * We use lmb_end_of_DRAM() in here instead of memory_limit because
	 * we've already adjusted it for the limit and it takes care of
	 * having memory holes below the limit.
	 */

	if (! memory_limit)
		return size;

	if (start + size <= lmb_end_of_DRAM())
		return size;

	if (start >= lmb_end_of_DRAM())
		return 0;

	return lmb_end_of_DRAM() - start;
}

/*
 * Extract NUMA information from the ibm,dynamic-reconfiguration-memory
 * node.  This assumes n_mem_{addr,size}_cells have been set.
 */
static void __init parse_drconf_memory(struct device_node *memory)
{
	const unsigned int *lm, *dm, *aa;
	unsigned int ls, ld, la;
	unsigned int n, aam, aalen;
	unsigned long lmb_size, size, start;
	int nid, default_nid = 0;
	unsigned int ai, flags;

	lm = of_get_property(memory, "ibm,lmb-size", &ls);
	dm = of_get_property(memory, "ibm,dynamic-memory", &ld);
	aa = of_get_property(memory, "ibm,associativity-lookup-arrays", &la);
	if (!lm || !dm || !aa ||
	    ls < sizeof(unsigned int) || ld < sizeof(unsigned int) ||
	    la < 2 * sizeof(unsigned int))
		return;

	lmb_size = read_n_cells(n_mem_size_cells, &lm);
	n = *dm++;		/* number of LMBs */
	aam = *aa++;		/* number of associativity lists */
	aalen = *aa++;		/* length of each associativity list */
	if (ld < (n * (n_mem_addr_cells + 4) + 1) * sizeof(unsigned int) ||
	    la < (aam * aalen + 2) * sizeof(unsigned int))
		return;

	for (; n != 0; --n) {
		start = read_n_cells(n_mem_addr_cells, &dm);
		ai = dm[2];
		flags = dm[3];
		dm += 4;
		/* 0x80 == reserved, 0x8 = assigned to us */
		if ((flags & 0x80) || !(flags & 0x8))
			continue;
		nid = default_nid;
		/* flags & 0x40 means associativity index is invalid */
		if (min_common_depth > 0 && min_common_depth <= aalen &&
		    (flags & 0x40) == 0 && ai < aam) {
			/* this is like of_node_to_nid_single */
			nid = aa[ai * aalen + min_common_depth - 1];
			if (nid == 0xffff || nid >= MAX_NUMNODES)
				nid = default_nid;
		}
		node_set_online(nid);

		size = numa_enforce_memory_limit(start, lmb_size);
		if (!size)
			continue;

		add_active_range(nid, start >> PAGE_SHIFT,
				 (start >> PAGE_SHIFT) + (size >> PAGE_SHIFT));
	}
}

static int __init parse_numa_properties(void)
{
	struct device_node *cpu = NULL;
	struct device_node *memory = NULL;
	int default_nid = 0;
	unsigned long i;

	if (numa_enabled == 0) {
		printk(KERN_WARNING "NUMA disabled by user\n");
		return -1;
	}

	min_common_depth = find_min_common_depth();

	if (min_common_depth < 0)
		return min_common_depth;

	dbg("NUMA associativity depth for CPU/Memory: %d\n", min_common_depth);

	/*
	 * Even though we connect cpus to numa domains later in SMP
	 * init, we need to know the node ids now. This is because
	 * each node to be onlined must have NODE_DATA etc backing it.
	 */
	for_each_present_cpu(i) {
		int nid;

		cpu = find_cpu_node(i);
		BUG_ON(!cpu);
		nid = of_node_to_nid_single(cpu);
		of_node_put(cpu);

		/*
		 * Don't fall back to default_nid yet -- we will plug
		 * cpus into nodes once the memory scan has discovered
		 * the topology.
		 */
		if (nid < 0)
			continue;
		node_set_online(nid);
	}

	get_n_mem_cells(&n_mem_addr_cells, &n_mem_size_cells);
	memory = NULL;
	while ((memory = of_find_node_by_type(memory, "memory")) != NULL) {
		unsigned long start;
		unsigned long size;
		int nid;
		int ranges;
		const unsigned int *memcell_buf;
		unsigned int len;

		memcell_buf = of_get_property(memory,
			"linux,usable-memory", &len);
		if (!memcell_buf || len <= 0)
			memcell_buf = of_get_property(memory, "reg", &len);
		if (!memcell_buf || len <= 0)
			continue;

		/* ranges in cell */
		ranges = (len >> 2) / (n_mem_addr_cells + n_mem_size_cells);
new_range:
		/* these are order-sensitive, and modify the buffer pointer */
		start = read_n_cells(n_mem_addr_cells, &memcell_buf);
		size = read_n_cells(n_mem_size_cells, &memcell_buf);

		/*
		 * Assumption: either all memory nodes or none will
		 * have associativity properties.  If none, then
		 * everything goes to default_nid.
		 */
		nid = of_node_to_nid_single(memory);
		if (nid < 0)
			nid = default_nid;
		node_set_online(nid);

		if (!(size = numa_enforce_memory_limit(start, size))) {
			if (--ranges)
				goto new_range;
			else
				continue;
		}

		add_active_range(nid, start >> PAGE_SHIFT,
				(start >> PAGE_SHIFT) + (size >> PAGE_SHIFT));

		if (--ranges)
			goto new_range;
	}

	/*
	 * Now do the same thing for each LMB listed in the ibm,dynamic-memory
	 * property in the ibm,dynamic-reconfiguration-memory node.
	 */
	memory = of_find_node_by_path("/ibm,dynamic-reconfiguration-memory");
	if (memory)
		parse_drconf_memory(memory);

	return 0;
}

static void __init setup_nonnuma(void)
{
	unsigned long top_of_ram = lmb_end_of_DRAM();
	unsigned long total_ram = lmb_phys_mem_size();
	unsigned long start_pfn, end_pfn;
	unsigned int i;

	printk(KERN_DEBUG "Top of RAM: 0x%lx, Total RAM: 0x%lx\n",
	       top_of_ram, total_ram);
	printk(KERN_DEBUG "Memory hole size: %ldMB\n",
	       (top_of_ram - total_ram) >> 20);

	for (i = 0; i < lmb.memory.cnt; ++i) {
		start_pfn = lmb.memory.region[i].base >> PAGE_SHIFT;
		end_pfn = start_pfn + lmb_size_pages(&lmb.memory, i);
		add_active_range(0, start_pfn, end_pfn);
	}
	node_set_online(0);
}

void __init dump_numa_cpu_topology(void)
{
	unsigned int node;
	unsigned int cpu, count;

	if (min_common_depth == -1 || !numa_enabled)
		return;

	for_each_online_node(node) {
		printk(KERN_DEBUG "Node %d CPUs:", node);

		count = 0;
		/*
		 * If we used a CPU iterator here we would miss printing
		 * the holes in the cpumap.
		 */
		for (cpu = 0; cpu < NR_CPUS; cpu++) {
			if (cpu_isset(cpu, numa_cpumask_lookup_table[node])) {
				if (count == 0)
					printk(" %u", cpu);
				++count;
			} else {
				if (count > 1)
					printk("-%u", cpu - 1);
				count = 0;
			}
		}

		if (count > 1)
			printk("-%u", NR_CPUS - 1);
		printk("\n");
	}
}

static void __init dump_numa_memory_topology(void)
{
	unsigned int node;
	unsigned int count;

	if (min_common_depth == -1 || !numa_enabled)
		return;

	for_each_online_node(node) {
		unsigned long i;

		printk(KERN_DEBUG "Node %d Memory:", node);

		count = 0;

		for (i = 0; i < lmb_end_of_DRAM();
		     i += (1 << SECTION_SIZE_BITS)) {
			if (early_pfn_to_nid(i >> PAGE_SHIFT) == node) {
				if (count == 0)
					printk(" 0x%lx", i);
				++count;
			} else {
				if (count > 0)
					printk("-0x%lx", i);
				count = 0;
			}
		}

		if (count > 0)
			printk("-0x%lx", i);
		printk("\n");
	}
}

/*
 * Allocate some memory, satisfying the lmb or bootmem allocator where
 * required. nid is the preferred node and end is the physical address of
 * the highest address in the node.
 *
 * Returns the physical address of the memory.
 */
static void __init *careful_allocation(int nid, unsigned long size,
				       unsigned long align,
				       unsigned long end_pfn)
{
	int new_nid;
	unsigned long ret = __lmb_alloc_base(size, align, end_pfn << PAGE_SHIFT);

	/* retry over all memory */
	if (!ret)
		ret = __lmb_alloc_base(size, align, lmb_end_of_DRAM());

	if (!ret)
		panic("numa.c: cannot allocate %lu bytes on node %d",
		      size, nid);

	/*
	 * If the memory came from a previously allocated node, we must
	 * retry with the bootmem allocator.
	 */
	new_nid = early_pfn_to_nid(ret >> PAGE_SHIFT);
	if (new_nid < nid) {
		ret = (unsigned long)__alloc_bootmem_node(NODE_DATA(new_nid),
				size, align, 0);

		if (!ret)
			panic("numa.c: cannot allocate %lu bytes on node %d",
			      size, new_nid);

		ret = __pa(ret);

		dbg("alloc_bootmem %lx %lx\n", ret, size);
	}

	return (void *)ret;
}

static struct notifier_block __cpuinitdata ppc64_numa_nb = {
	.notifier_call = cpu_numa_callback,
	.priority = 1 /* Must run before sched domains notifier. */
};

void __init do_init_bootmem(void)
{
	int nid;
	unsigned int i;

	min_low_pfn = 0;
	max_low_pfn = lmb_end_of_DRAM() >> PAGE_SHIFT;
	max_pfn = max_low_pfn;

	if (parse_numa_properties())
		setup_nonnuma();
	else
		dump_numa_memory_topology();

	register_cpu_notifier(&ppc64_numa_nb);
	cpu_numa_callback(&ppc64_numa_nb, CPU_UP_PREPARE,
			  (void *)(unsigned long)boot_cpuid);

	for_each_online_node(nid) {
		unsigned long start_pfn, end_pfn;
		unsigned long bootmem_paddr;
		unsigned long bootmap_pages;

		get_pfn_range_for_nid(nid, &start_pfn, &end_pfn);

		/* Allocate the node structure node local if possible */
		NODE_DATA(nid) = careful_allocation(nid,
					sizeof(struct pglist_data),
					SMP_CACHE_BYTES, end_pfn);
		NODE_DATA(nid) = __va(NODE_DATA(nid));
		memset(NODE_DATA(nid), 0, sizeof(struct pglist_data));

  		dbg("node %d\n", nid);
		dbg("NODE_DATA() = %p\n", NODE_DATA(nid));

		NODE_DATA(nid)->bdata = &plat_node_bdata[nid];
		NODE_DATA(nid)->node_start_pfn = start_pfn;
		NODE_DATA(nid)->node_spanned_pages = end_pfn - start_pfn;

		if (NODE_DATA(nid)->node_spanned_pages == 0)
  			continue;

  		dbg("start_paddr = %lx\n", start_pfn << PAGE_SHIFT);
  		dbg("end_paddr = %lx\n", end_pfn << PAGE_SHIFT);

		bootmap_pages = bootmem_bootmap_pages(end_pfn - start_pfn);
		bootmem_paddr = (unsigned long)careful_allocation(nid,
					bootmap_pages << PAGE_SHIFT,
					PAGE_SIZE, end_pfn);
		memset(__va(bootmem_paddr), 0, bootmap_pages << PAGE_SHIFT);

		dbg("bootmap_paddr = %lx\n", bootmem_paddr);

		init_bootmem_node(NODE_DATA(nid), bootmem_paddr >> PAGE_SHIFT,
				  start_pfn, end_pfn);

		free_bootmem_with_active_regions(nid, end_pfn);

		/* Mark reserved regions on this node */
		for (i = 0; i < lmb.reserved.cnt; i++) {
			unsigned long physbase = lmb.reserved.region[i].base;
			unsigned long size = lmb.reserved.region[i].size;
			unsigned long start_paddr = start_pfn << PAGE_SHIFT;
			unsigned long end_paddr = end_pfn << PAGE_SHIFT;

			if (early_pfn_to_nid(physbase >> PAGE_SHIFT) != nid &&
			    early_pfn_to_nid((physbase+size-1) >> PAGE_SHIFT) != nid)
				continue;

			if (physbase < end_paddr &&
			    (physbase+size) > start_paddr) {
				/* overlaps */
				if (physbase < start_paddr) {
					size -= start_paddr - physbase;
					physbase = start_paddr;
				}

				if (size > end_paddr - physbase)
					size = end_paddr - physbase;

				dbg("reserve_bootmem %lx %lx\n", physbase,
				    size);
				reserve_bootmem_node(NODE_DATA(nid), physbase,
						     size);
			}
		}

		sparse_memory_present_with_active_regions(nid);
	}
}

void __init paging_init(void)
{
	unsigned long max_zone_pfns[MAX_NR_ZONES];
	memset(max_zone_pfns, 0, sizeof(max_zone_pfns));
	max_zone_pfns[ZONE_DMA] = lmb_end_of_DRAM() >> PAGE_SHIFT;
	free_area_init_nodes(max_zone_pfns);
}

static int __init early_numa(char *p)
{
	if (!p)
		return 0;

	if (strstr(p, "off"))
		numa_enabled = 0;

	if (strstr(p, "debug"))
		numa_debug = 1;

	return 0;
}
early_param("numa", early_numa);

#ifdef CONFIG_MEMORY_HOTPLUG
/*
 * Find the node associated with a hot added memory section.  Section
 * corresponds to a SPARSEMEM section, not an LMB.  It is assumed that
 * sections are fully contained within a single LMB.
 */
int hot_add_scn_to_nid(unsigned long scn_addr)
{
	struct device_node *memory = NULL;
	nodemask_t nodes;
	int default_nid = any_online_node(NODE_MASK_ALL);
	int nid;

	if (!numa_enabled || (min_common_depth < 0))
		return default_nid;

	while ((memory = of_find_node_by_type(memory, "memory")) != NULL) {
		unsigned long start, size;
		int ranges;
		const unsigned int *memcell_buf;
		unsigned int len;

		memcell_buf = of_get_property(memory, "reg", &len);
		if (!memcell_buf || len <= 0)
			continue;

		/* ranges in cell */
		ranges = (len >> 2) / (n_mem_addr_cells + n_mem_size_cells);
ha_new_range:
		start = read_n_cells(n_mem_addr_cells, &memcell_buf);
		size = read_n_cells(n_mem_size_cells, &memcell_buf);
		nid = of_node_to_nid_single(memory);

		/* Domains not present at boot default to 0 */
		if (nid < 0 || !node_online(nid))
			nid = default_nid;

		if ((scn_addr >= start) && (scn_addr < (start + size))) {
			of_node_put(memory);
			goto got_nid;
		}

		if (--ranges)		/* process all ranges in cell */
			goto ha_new_range;
	}
	BUG();	/* section address should be found above */
	return 0;

	/* Temporary code to ensure that returned node is not empty */
got_nid:
	nodes_setall(nodes);
	while (NODE_DATA(nid)->node_spanned_pages == 0) {
		node_clear(nid, nodes);
		nid = any_online_node(nodes);
	}
	return nid;
}
#endif /* CONFIG_MEMORY_HOTPLUG */
