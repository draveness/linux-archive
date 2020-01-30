/*
 * Copyright (c) 2000, 2003 Silicon Graphics, Inc.  All rights reserved.
 * Copyright (c) 2001 Intel Corp.
 * Copyright (c) 2001 Tony Luck <tony.luck@intel.com>
 * Copyright (c) 2002 NEC Corp.
 * Copyright (c) 2002 Kimio Suganuma <k-suganuma@da.jp.nec.com>
 */

/*
 * Platform initialization for Discontig Memory
 */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/bootmem.h>
#include <linux/acpi.h>
#include <linux/efi.h>
#include <asm/pgalloc.h>
#include <asm/tlb.h>
#include <asm/meminit.h>
#include <asm/numa.h>
#include <asm/sections.h>

/*
 * Track per-node information needed to setup the boot memory allocator, the
 * per-node areas, and the real VM.
 */
struct early_node_data {
	struct ia64_node_data *node_data;
	pg_data_t *pgdat;
	unsigned long pernode_addr;
	unsigned long pernode_size;
	struct bootmem_data bootmem_data;
	unsigned long num_physpages;
	unsigned long num_dma_physpages;
	unsigned long min_pfn;
	unsigned long max_pfn;
};

static struct early_node_data mem_data[NR_NODES] __initdata;

/**
 * reassign_cpu_only_nodes - called from find_memory to move CPU-only nodes to a memory node
 *
 * This function will move nodes with only CPUs (no memory)
 * to a node with memory which is at the minimum numa_slit distance.
 * Any reassigments will result in the compression of the nodes
 * and renumbering the nid values where appropriate.
 * The static declarations below are to avoid large stack size which
 * makes the code not re-entrant.
 */
static void __init reassign_cpu_only_nodes(void)
{
	struct node_memblk_s *p;
	int i, j, k, nnode, nid, cpu, cpunid;
	u8 cslit, slit;
	static DECLARE_BITMAP(nodes_with_mem, NR_NODES) __initdata;
	static u8 numa_slit_fix[MAX_NUMNODES * MAX_NUMNODES] __initdata;
	static int node_flip[NR_NODES] __initdata;

	for (nnode = 0, p = &node_memblk[0]; p < &node_memblk[num_node_memblks]; p++)
		if (!test_bit(p->nid, (void *) nodes_with_mem)) {
			set_bit(p->nid, (void *) nodes_with_mem);
			nnode++;
		}

	/*
	 * All nids with memory.
	 */
	if (nnode == numnodes)
		return;

	/*
	 * Change nids and attempt to migrate CPU-only nodes
	 * to the best numa_slit (closest neighbor) possible.
	 * For reassigned CPU nodes a nid can't be arrived at
	 * until after this loop because the target nid's new
	 * identity might not have been established yet. So
	 * new nid values are fabricated above numnodes and
	 * mapped back later to their true value.
	 */
	for (nid = 0, i = 0; i < numnodes; i++)  {
		if (test_bit(i, (void *) nodes_with_mem)) {
			/*
			 * Save original nid value for numa_slit
			 * fixup and node_cpuid reassignments.
			 */
			node_flip[nid] = i;

			if (i == nid) {
				nid++;
				continue;
			}

			for (p = &node_memblk[0]; p < &node_memblk[num_node_memblks]; p++)
				if (p->nid == i)
					p->nid = nid;

			cpunid = nid;
			nid++;
		} else
			cpunid = numnodes;

		for (cpu = 0; cpu < NR_CPUS; cpu++)
			if (node_cpuid[cpu].nid == i) {
				/* For nodes not being reassigned just fix the cpu's nid. */
				if (cpunid < numnodes) {
					node_cpuid[cpu].nid = cpunid;
					continue;
				}

				/*
				 * For nodes being reassigned, find best node by
				 * numa_slit information and then make a temporary
				 * nid value based on current nid and numnodes.
				 */
				for (slit = 0xff, k = numnodes + numnodes, j = 0; j < numnodes; j++)
					if (i == j)
						continue;
					else if (test_bit(j, (void *) nodes_with_mem)) {
						cslit = numa_slit[i * numnodes + j];
						if (cslit < slit) {
							k = numnodes + j;
							slit = cslit;
						}
					}

				node_cpuid[cpu].nid = k;
			}
	}

	/*
	 * Fixup temporary nid values for CPU-only nodes.
	 */
	for (cpu = 0; cpu < NR_CPUS; cpu++)
		if (node_cpuid[cpu].nid == (numnodes + numnodes))
			node_cpuid[cpu].nid = nnode - 1;
		else
			for (i = 0; i < nnode; i++)
				if (node_flip[i] == (node_cpuid[cpu].nid - numnodes)) {
					node_cpuid[cpu].nid = i;
					break;
				}

	/*
	 * Fix numa_slit by compressing from larger
	 * nid array to reduced nid array.
	 */
	for (i = 0; i < nnode; i++)
		for (j = 0; j < nnode; j++)
			numa_slit_fix[i * nnode + j] =
				numa_slit[node_flip[i] * numnodes + node_flip[j]];

	memcpy(numa_slit, numa_slit_fix, sizeof (numa_slit));

	for (i = nnode; i < numnodes; i++)
		node_set_offline(i);

	numnodes = nnode;

	return;
}

/*
 * To prevent cache aliasing effects, align per-node structures so that they
 * start at addresses that are strided by node number.
 */
#define NODEDATA_ALIGN(addr, node)						\
	((((addr) + 1024*1024-1) & ~(1024*1024-1)) + (node)*PERCPU_PAGE_SIZE)

/**
 * build_node_maps - callback to setup bootmem structs for each node
 * @start: physical start of range
 * @len: length of range
 * @node: node where this range resides
 *
 * We allocate a struct bootmem_data for each piece of memory that we wish to
 * treat as a virtually contiguous block (i.e. each node). Each such block
 * must start on an %IA64_GRANULE_SIZE boundary, so we round the address down
 * if necessary.  Any non-existent pages will simply be part of the virtual
 * memmap.  We also update min_low_pfn and max_low_pfn here as we receive
 * memory ranges from the caller.
 */
static int __init build_node_maps(unsigned long start, unsigned long len,
				  int node)
{
	unsigned long cstart, epfn, end = start + len;
	struct bootmem_data *bdp = &mem_data[node].bootmem_data;

	epfn = GRANULEROUNDUP(end) >> PAGE_SHIFT;
	cstart = GRANULEROUNDDOWN(start);

	if (!bdp->node_low_pfn) {
		bdp->node_boot_start = cstart;
		bdp->node_low_pfn = epfn;
	} else {
		bdp->node_boot_start = min(cstart, bdp->node_boot_start);
		bdp->node_low_pfn = max(epfn, bdp->node_low_pfn);
	}

	min_low_pfn = min(min_low_pfn, bdp->node_boot_start>>PAGE_SHIFT);
	max_low_pfn = max(max_low_pfn, bdp->node_low_pfn);

	return 0;
}

/**
 * early_nr_cpus_node - return number of cpus on a given node
 * @node: node to check
 *
 * Count the number of cpus on @node.  We can't use nr_cpus_node() yet because
 * acpi_boot_init() (which builds the node_to_cpu_mask array) hasn't been
 * called yet.
 */
static int early_nr_cpus_node(int node)
{
	int cpu, n = 0;

	for (cpu = 0; cpu < NR_CPUS; cpu++)
		if (node == node_cpuid[cpu].nid)
			n++;

	return n;
}

/**
 * find_pernode_space - allocate memory for memory map and per-node structures
 * @start: physical start of range
 * @len: length of range
 * @node: node where this range resides
 *
 * This routine reserves space for the per-cpu data struct, the list of
 * pg_data_ts and the per-node data struct.  Each node will have something like
 * the following in the first chunk of addr. space large enough to hold it.
 *
 *    ________________________
 *   |                        |
 *   |~~~~~~~~~~~~~~~~~~~~~~~~| <-- NODEDATA_ALIGN(start, node) for the first
 *   |    PERCPU_PAGE_SIZE *  |     start and length big enough
 *   |        NR_CPUS         |
 *   |------------------------|
 *   |   local pg_data_t *    |
 *   |------------------------|
 *   |  local ia64_node_data  |
 *   |------------------------|
 *   |          ???           |
 *   |________________________|
 *
 * Once this space has been set aside, the bootmem maps are initialized.  We
 * could probably move the allocation of the per-cpu and ia64_node_data space
 * outside of this function and use alloc_bootmem_node(), but doing it here
 * is straightforward and we get the alignments we want so...
 */
static int __init find_pernode_space(unsigned long start, unsigned long len,
				     int node)
{
	unsigned long epfn, cpu, cpus;
	unsigned long pernodesize = 0, pernode, pages, mapsize;
	void *cpu_data;
	struct bootmem_data *bdp = &mem_data[node].bootmem_data;

	epfn = (start + len) >> PAGE_SHIFT;

	pages = bdp->node_low_pfn - (bdp->node_boot_start >> PAGE_SHIFT);
	mapsize = bootmem_bootmap_pages(pages) << PAGE_SHIFT;

	/*
	 * Make sure this memory falls within this node's usable memory
	 * since we may have thrown some away in build_maps().
	 */
	if (start < bdp->node_boot_start || epfn > bdp->node_low_pfn)
		return 0;

	/* Don't setup this node's local space twice... */
	if (mem_data[node].pernode_addr)
		return 0;

	/*
	 * Calculate total size needed, incl. what's necessary
	 * for good alignment and alias prevention.
	 */
	cpus = early_nr_cpus_node(node);
	pernodesize += PERCPU_PAGE_SIZE * cpus;
	pernodesize += L1_CACHE_ALIGN(sizeof(pg_data_t));
	pernodesize += L1_CACHE_ALIGN(sizeof(struct ia64_node_data));
	pernodesize = PAGE_ALIGN(pernodesize);
	pernode = NODEDATA_ALIGN(start, node);

	/* Is this range big enough for what we want to store here? */
	if (start + len > (pernode + pernodesize + mapsize)) {
		mem_data[node].pernode_addr = pernode;
		mem_data[node].pernode_size = pernodesize;
		memset(__va(pernode), 0, pernodesize);

		cpu_data = (void *)pernode;
		pernode += PERCPU_PAGE_SIZE * cpus;

		mem_data[node].pgdat = __va(pernode);
		pernode += L1_CACHE_ALIGN(sizeof(pg_data_t));

		mem_data[node].node_data = __va(pernode);
		pernode += L1_CACHE_ALIGN(sizeof(struct ia64_node_data));

		mem_data[node].pgdat->bdata = bdp;
		pernode += L1_CACHE_ALIGN(sizeof(pg_data_t));

		/*
		 * Copy the static per-cpu data into the region we
		 * just set aside and then setup __per_cpu_offset
		 * for each CPU on this node.
		 */
		for (cpu = 0; cpu < NR_CPUS; cpu++) {
			if (node == node_cpuid[cpu].nid) {
				memcpy(__va(cpu_data), __phys_per_cpu_start,
				       __per_cpu_end - __per_cpu_start);
				__per_cpu_offset[cpu] = (char*)__va(cpu_data) -
					__per_cpu_start;
				cpu_data += PERCPU_PAGE_SIZE;
			}
		}
	}

	return 0;
}

/**
 * free_node_bootmem - free bootmem allocator memory for use
 * @start: physical start of range
 * @len: length of range
 * @node: node where this range resides
 *
 * Simply calls the bootmem allocator to free the specified ranged from
 * the given pg_data_t's bdata struct.  After this function has been called
 * for all the entries in the EFI memory map, the bootmem allocator will
 * be ready to service allocation requests.
 */
static int __init free_node_bootmem(unsigned long start, unsigned long len,
				    int node)
{
	free_bootmem_node(mem_data[node].pgdat, start, len);

	return 0;
}

/**
 * reserve_pernode_space - reserve memory for per-node space
 *
 * Reserve the space used by the bootmem maps & per-node space in the boot
 * allocator so that when we actually create the real mem maps we don't
 * use their memory.
 */
static void __init reserve_pernode_space(void)
{
	unsigned long base, size, pages;
	struct bootmem_data *bdp;
	int node;

	for (node = 0; node < numnodes; node++) {
		pg_data_t *pdp = mem_data[node].pgdat;

		bdp = pdp->bdata;

		/* First the bootmem_map itself */
		pages = bdp->node_low_pfn - (bdp->node_boot_start>>PAGE_SHIFT);
		size = bootmem_bootmap_pages(pages) << PAGE_SHIFT;
		base = __pa(bdp->node_bootmem_map);
		reserve_bootmem_node(pdp, base, size);

		/* Now the per-node space */
		size = mem_data[node].pernode_size;
		base = __pa(mem_data[node].pernode_addr);
		reserve_bootmem_node(pdp, base, size);
	}
}

/**
 * initialize_pernode_data - fixup per-cpu & per-node pointers
 *
 * Each node's per-node area has a copy of the global pg_data_t list, so
 * we copy that to each node here, as well as setting the per-cpu pointer
 * to the local node data structure.  The active_cpus field of the per-node
 * structure gets setup by the platform_cpu_init() function later.
 */
static void __init initialize_pernode_data(void)
{
	int cpu, node;
	pg_data_t *pgdat_list[NR_NODES];

	for (node = 0; node < numnodes; node++)
		pgdat_list[node] = mem_data[node].pgdat;

	/* Copy the pg_data_t list to each node and init the node field */
	for (node = 0; node < numnodes; node++) {
		memcpy(mem_data[node].node_data->pg_data_ptrs, pgdat_list,
		       sizeof(pgdat_list));
	}

	/* Set the node_data pointer for each per-cpu struct */
	for (cpu = 0; cpu < NR_CPUS; cpu++) {
		node = node_cpuid[cpu].nid;
		per_cpu(cpu_info, cpu).node_data = mem_data[node].node_data;
	}
}

/**
 * find_memory - walk the EFI memory map and setup the bootmem allocator
 *
 * Called early in boot to setup the bootmem allocator, and to
 * allocate the per-cpu and per-node structures.
 */
void __init find_memory(void)
{
	int node;

	reserve_memory();

	if (numnodes == 0) {
		printk(KERN_ERR "node info missing!\n");
		numnodes = 1;
	}

	min_low_pfn = -1;
	max_low_pfn = 0;

	if (numnodes > 1)
		reassign_cpu_only_nodes();

	/* These actually end up getting called by call_pernode_memory() */
	efi_memmap_walk(filter_rsvd_memory, build_node_maps);
	efi_memmap_walk(filter_rsvd_memory, find_pernode_space);

	/*
	 * Initialize the boot memory maps in reverse order since that's
	 * what the bootmem allocator expects
	 */
	for (node = numnodes - 1; node >= 0; node--) {
		unsigned long pernode, pernodesize, map;
		struct bootmem_data *bdp;

		bdp = &mem_data[node].bootmem_data;
		pernode = mem_data[node].pernode_addr;
		pernodesize = mem_data[node].pernode_size;
		map = pernode + pernodesize;

		/* Sanity check... */
		if (!pernode)
			panic("pernode space for node %d "
			      "could not be allocated!", node);

		init_bootmem_node(mem_data[node].pgdat,
				  map>>PAGE_SHIFT,
				  bdp->node_boot_start>>PAGE_SHIFT,
				  bdp->node_low_pfn);
	}

	efi_memmap_walk(filter_rsvd_memory, free_node_bootmem);

	reserve_pernode_space();
	initialize_pernode_data();

	max_pfn = max_low_pfn;

	find_initrd();
}

/**
 * per_cpu_init - setup per-cpu variables
 *
 * find_pernode_space() does most of this already, we just need to set
 * local_per_cpu_offset
 */
void *per_cpu_init(void)
{
	int cpu;

	if (smp_processor_id() == 0) {
		for (cpu = 0; cpu < NR_CPUS; cpu++) {
			per_cpu(local_per_cpu_offset, cpu) =
				__per_cpu_offset[cpu];
		}
	}

	return __per_cpu_start + __per_cpu_offset[smp_processor_id()];
}

/**
 * show_mem - give short summary of memory stats
 *
 * Shows a simple page count of reserved and used pages in the system.
 * For discontig machines, it does this on a per-pgdat basis.
 */
void show_mem(void)
{
	int i, reserved = 0;
	int shared = 0, cached = 0;
	pg_data_t *pgdat;

	printk("Mem-info:\n");
	show_free_areas();
	printk("Free swap:       %6ldkB\n", nr_swap_pages<<(PAGE_SHIFT-10));
	for_each_pgdat(pgdat) {
		printk("Node ID: %d\n", pgdat->node_id);
		for(i = 0; i < pgdat->node_spanned_pages; i++) {
			if (!ia64_pfn_valid(pgdat->node_start_pfn+i))
				continue;
			if (PageReserved(pgdat->node_mem_map+i))
				reserved++;
			else if (PageSwapCache(pgdat->node_mem_map+i))
				cached++;
			else if (page_count(pgdat->node_mem_map+i))
				shared += page_count(pgdat->node_mem_map+i)-1;
		}
		printk("\t%ld pages of RAM\n", pgdat->node_present_pages);
		printk("\t%d reserved pages\n", reserved);
		printk("\t%d pages shared\n", shared);
		printk("\t%d pages swap cached\n", cached);
	}
	printk("Total of %ld pages in page table cache\n", pgtable_cache_size);
	printk("%d free buffer pages\n", nr_free_buffer_pages());
}

/**
 * call_pernode_memory - use SRAT to call callback functions with node info
 * @start: physical start of range
 * @len: length of range
 * @arg: function to call for each range
 *
 * efi_memmap_walk() knows nothing about layout of memory across nodes. Find
 * out to which node a block of memory belongs.  Ignore memory that we cannot
 * identify, and split blocks that run across multiple nodes.
 *
 * Take this opportunity to round the start address up and the end address
 * down to page boundaries.
 */
void call_pernode_memory(unsigned long start, unsigned long len, void *arg)
{
	unsigned long rs, re, end = start + len;
	void (*func)(unsigned long, unsigned long, int);
	int i;

	start = PAGE_ALIGN(start);
	end &= PAGE_MASK;
	if (start >= end)
		return;

	func = arg;

	if (!num_node_memblks) {
		/* No SRAT table, so assume one node (node 0) */
		if (start < end)
			(*func)(start, end - start, 0);
		return;
	}

	for (i = 0; i < num_node_memblks; i++) {
		rs = max(start, node_memblk[i].start_paddr);
		re = min(end, node_memblk[i].start_paddr +
			 node_memblk[i].size);

		if (rs < re)
			(*func)(rs, re - rs, node_memblk[i].nid);

		if (re == end)
			break;
	}
}

/**
 * count_node_pages - callback to build per-node memory info structures
 * @start: physical start of range
 * @len: length of range
 * @node: node where this range resides
 *
 * Each node has it's own number of physical pages, DMAable pages, start, and
 * end page frame number.  This routine will be called by call_pernode_memory()
 * for each piece of usable memory and will setup these values for each node.
 * Very similar to build_maps().
 */
static int count_node_pages(unsigned long start, unsigned long len, int node)
{
	unsigned long end = start + len;

	mem_data[node].num_physpages += len >> PAGE_SHIFT;
	if (start <= __pa(MAX_DMA_ADDRESS))
		mem_data[node].num_dma_physpages +=
			(min(end, __pa(MAX_DMA_ADDRESS)) - start) >>PAGE_SHIFT;
	start = GRANULEROUNDDOWN(start);
	start = ORDERROUNDDOWN(start);
	end = GRANULEROUNDUP(end);
	mem_data[node].max_pfn = max(mem_data[node].max_pfn,
				     end >> PAGE_SHIFT);
	mem_data[node].min_pfn = min(mem_data[node].min_pfn,
				     start >> PAGE_SHIFT);

	return 0;
}

/**
 * paging_init - setup page tables
 *
 * paging_init() sets up the page tables for each node of the system and frees
 * the bootmem allocator memory for general use.
 */
void paging_init(void)
{
	unsigned long max_dma;
	unsigned long zones_size[MAX_NR_ZONES];
	unsigned long zholes_size[MAX_NR_ZONES];
	unsigned long max_gap, pfn_offset = 0;
	int node;

	max_dma = virt_to_phys((void *) MAX_DMA_ADDRESS) >> PAGE_SHIFT;
	max_gap = 0;
	efi_memmap_walk(find_largest_hole, &max_gap);

	/* so min() will work in count_node_pages */
	for (node = 0; node < numnodes; node++)
		mem_data[node].min_pfn = ~0UL;

	efi_memmap_walk(filter_rsvd_memory, count_node_pages);

	for (node = 0; node < numnodes; node++) {
		memset(zones_size, 0, sizeof(zones_size));
		memset(zholes_size, 0, sizeof(zholes_size));

		num_physpages += mem_data[node].num_physpages;

		if (mem_data[node].min_pfn >= max_dma) {
			/* All of this node's memory is above ZONE_DMA */
			zones_size[ZONE_NORMAL] = mem_data[node].max_pfn -
				mem_data[node].min_pfn;
			zholes_size[ZONE_NORMAL] = mem_data[node].max_pfn -
				mem_data[node].min_pfn -
				mem_data[node].num_physpages;
		} else if (mem_data[node].max_pfn < max_dma) {
			/* All of this node's memory is in ZONE_DMA */
			zones_size[ZONE_DMA] = mem_data[node].max_pfn -
				mem_data[node].min_pfn;
			zholes_size[ZONE_DMA] = mem_data[node].max_pfn -
				mem_data[node].min_pfn -
				mem_data[node].num_dma_physpages;
		} else {
			/* This node has memory in both zones */
			zones_size[ZONE_DMA] = max_dma -
				mem_data[node].min_pfn;
			zholes_size[ZONE_DMA] = zones_size[ZONE_DMA] -
				mem_data[node].num_dma_physpages;
			zones_size[ZONE_NORMAL] = mem_data[node].max_pfn -
				max_dma;
			zholes_size[ZONE_NORMAL] = zones_size[ZONE_NORMAL] -
				(mem_data[node].num_physpages -
				 mem_data[node].num_dma_physpages);
		}

		if (node == 0) {
			vmalloc_end -=
				PAGE_ALIGN(max_low_pfn * sizeof(struct page));
			vmem_map = (struct page *) vmalloc_end;

			efi_memmap_walk(create_mem_map_page_table, 0);
			printk("Virtual mem_map starts at 0x%p\n", vmem_map);
		}

		pfn_offset = mem_data[node].min_pfn;

		free_area_init_node(node, NODE_DATA(node),
				    vmem_map + pfn_offset, zones_size,
				    pfn_offset, zholes_size);
	}

	zero_page_memmap_ptr = virt_to_page(ia64_imva(empty_zero_page));
}
