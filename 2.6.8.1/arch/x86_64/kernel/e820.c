/* 
 * Handle the memory map.
 * The functions here do the job until bootmem takes over.
 * $Id: e820.c,v 1.4 2002/09/19 19:25:32 ak Exp $
 */
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/bootmem.h>
#include <linux/ioport.h>
#include <linux/string.h>
#include <asm/page.h>
#include <asm/e820.h>
#include <asm/proto.h>
#include <asm/bootsetup.h>

extern char _end[];

/* 
 * PFN of last memory page.
 */
unsigned long end_pfn; 

/* 
 * end_pfn only includes RAM, while end_pfn_map includes all e820 entries.
 * The direct mapping extends to end_pfn_map, so that we can directly access
 * apertures, ACPI and other tables without having to play with fixmaps.
 */ 
unsigned long end_pfn_map; 

/* 
 * Last pfn which the user wants to use.
 */
unsigned long end_user_pfn = MAXMEM>>PAGE_SHIFT;  

extern struct resource code_resource, data_resource;

/* Check for some hardcoded bad areas that early boot is not allowed to touch */ 
static inline int bad_addr(unsigned long *addrp, unsigned long size)
{ 
	unsigned long addr = *addrp, last = addr + size; 

	/* various gunk below that needed for SMP startup */
	if (addr < 0x8000) { 
		*addrp = 0x8000;
		return 1; 
	}

	/* direct mapping tables of the kernel */
	if (last >= table_start<<PAGE_SHIFT && addr < table_end<<PAGE_SHIFT) { 
		*addrp = table_end << PAGE_SHIFT; 
		return 1;
	} 

	/* initrd */ 
#ifdef CONFIG_BLK_DEV_INITRD
	if (LOADER_TYPE && INITRD_START && last >= INITRD_START && 
	    addr < INITRD_START+INITRD_SIZE) { 
		*addrp = INITRD_START + INITRD_SIZE; 
		return 1;
	} 
#endif
	/* kernel code + 640k memory hole (later should not be needed, but 
	   be paranoid for now) */
	if (last >= 640*1024 && addr < __pa_symbol(&_end)) { 
		*addrp = __pa_symbol(&_end);
		return 1;
	}
	/* XXX ramdisk image here? */ 
	return 0;
} 

int __init e820_mapped(unsigned long start, unsigned long end, unsigned type) 
{ 
	int i;
	for (i = 0; i < e820.nr_map; i++) { 
		struct e820entry *ei = &e820.map[i]; 
		if (type && ei->type != type) 
			continue;
		if (ei->addr >= end || ei->addr + ei->size < start) 
			continue; 
		return 1; 
	} 
	return 0;
}

/* 
 * Find a free area in a specific range. 
 */ 
unsigned long __init find_e820_area(unsigned long start, unsigned long end, unsigned size) 
{ 
	int i; 
	for (i = 0; i < e820.nr_map; i++) { 
		struct e820entry *ei = &e820.map[i]; 
		unsigned long addr = ei->addr, last; 
		if (ei->type != E820_RAM) 
			continue; 
		if (addr < start) 
			addr = start;
		if (addr > ei->addr + ei->size) 
			continue; 
		while (bad_addr(&addr, size) && addr+size < ei->addr + ei->size)
			;
		last = addr + size;
		if (last > ei->addr + ei->size)
			continue;
		if (last > end) 
			continue;
		return addr; 
	} 
	return -1UL;		
} 

/* 
 * Free bootmem based on the e820 table for a node.
 */
void __init e820_bootmem_free(pg_data_t *pgdat, unsigned long start,unsigned long end)
{
	int i;
	for (i = 0; i < e820.nr_map; i++) {
		struct e820entry *ei = &e820.map[i]; 
		unsigned long last, addr;

		if (ei->type != E820_RAM || 
		    ei->addr+ei->size <= start || 
		    ei->addr > end)
			continue;

		addr = round_up(ei->addr, PAGE_SIZE);
		if (addr < start) 
			addr = start;

		last = round_down(ei->addr + ei->size, PAGE_SIZE); 
		if (last >= end)
			last = end; 

		if (last > addr && last-addr >= PAGE_SIZE)
			free_bootmem_node(pgdat, addr, last-addr);
	}
}

/*
 * Find the highest page frame number we have available
 */
unsigned long __init e820_end_of_ram(void)
{
	int i;
	unsigned long end_pfn = 0;
	
	for (i = 0; i < e820.nr_map; i++) {
		struct e820entry *ei = &e820.map[i]; 
		unsigned long start, end;

		start = round_up(ei->addr, PAGE_SIZE); 
		end = round_down(ei->addr + ei->size, PAGE_SIZE); 
		if (start >= end)
			continue;
		if (ei->type == E820_RAM) { 
		if (end > end_pfn<<PAGE_SHIFT)
			end_pfn = end>>PAGE_SHIFT;
		} else { 
			if (end > end_pfn_map<<PAGE_SHIFT) 
				end_pfn_map = end>>PAGE_SHIFT;
		} 
	}

	if (end_pfn > end_pfn_map) 
		end_pfn_map = end_pfn;
	if (end_pfn_map > MAXMEM>>PAGE_SHIFT)
		end_pfn_map = MAXMEM>>PAGE_SHIFT;
	if (end_pfn > end_user_pfn)
		end_pfn = end_user_pfn;
	if (end_pfn > end_pfn_map) 
		end_pfn = end_pfn_map; 

	return end_pfn;	
}

/* 
 * Mark e820 reserved areas as busy for the resource manager.
 */
void __init e820_reserve_resources(void)
{
	int i;
	for (i = 0; i < e820.nr_map; i++) {
		struct resource *res;
		if (e820.map[i].addr + e820.map[i].size > 0x100000000ULL)
			continue;
		res = alloc_bootmem_low(sizeof(struct resource));
		switch (e820.map[i].type) {
		case E820_RAM:	res->name = "System RAM"; break;
		case E820_ACPI:	res->name = "ACPI Tables"; break;
		case E820_NVS:	res->name = "ACPI Non-volatile Storage"; break;
		default:	res->name = "reserved";
		}
		res->start = e820.map[i].addr;
		res->end = res->start + e820.map[i].size - 1;
		res->flags = IORESOURCE_MEM | IORESOURCE_BUSY;
		request_resource(&iomem_resource, res);
		if (e820.map[i].type == E820_RAM) {
			/*
			 *  We don't know which RAM region contains kernel data,
			 *  so we try it repeatedly and let the resource manager
			 *  test it.
			 */
			request_resource(res, &code_resource);
			request_resource(res, &data_resource);
		}
	}
}

/* 
 * Add a memory region to the kernel e820 map.
 */ 
void __init add_memory_region(unsigned long start, unsigned long size, int type)
{
	int x = e820.nr_map;

	if (x == E820MAX) {
		printk(KERN_ERR "Ooops! Too many entries in the memory map!\n");
		return;
	}

	e820.map[x].addr = start;
	e820.map[x].size = size;
	e820.map[x].type = type;
	e820.nr_map++;
}

void __init e820_print_map(char *who)
{
	int i;

	for (i = 0; i < e820.nr_map; i++) {
		printk(" %s: %016Lx - %016Lx ", who,
			(unsigned long long) e820.map[i].addr,
			(unsigned long long) (e820.map[i].addr + e820.map[i].size));
		switch (e820.map[i].type) {
		case E820_RAM:	printk("(usable)\n");
				break;
		case E820_RESERVED:
				printk("(reserved)\n");
				break;
		case E820_ACPI:
				printk("(ACPI data)\n");
				break;
		case E820_NVS:
				printk("(ACPI NVS)\n");
				break;
		default:	printk("type %u\n", e820.map[i].type);
				break;
		}
	}
}

/*
 * Sanitize the BIOS e820 map.
 *
 * Some e820 responses include overlapping entries.  The following 
 * replaces the original e820 map with a new one, removing overlaps.
 *
 */
static int __init sanitize_e820_map(struct e820entry * biosmap, char * pnr_map)
{
	struct change_member {
		struct e820entry *pbios; /* pointer to original bios entry */
		unsigned long long addr; /* address for this change point */
	};
	static struct change_member change_point_list[2*E820MAX] __initdata;
	static struct change_member *change_point[2*E820MAX] __initdata;
	static struct e820entry *overlap_list[E820MAX] __initdata;
	static struct e820entry new_bios[E820MAX] __initdata;
	struct change_member *change_tmp;
	unsigned long current_type, last_type;
	unsigned long long last_addr;
	int chgidx, still_changing;
	int overlap_entries;
	int new_bios_entry;
	int old_nr, new_nr;
	int i;

	/*
		Visually we're performing the following (1,2,3,4 = memory types)...

		Sample memory map (w/overlaps):
		   ____22__________________
		   ______________________4_
		   ____1111________________
		   _44_____________________
		   11111111________________
		   ____________________33__
		   ___________44___________
		   __________33333_________
		   ______________22________
		   ___________________2222_
		   _________111111111______
		   _____________________11_
		   _________________4______

		Sanitized equivalent (no overlap):
		   1_______________________
		   _44_____________________
		   ___1____________________
		   ____22__________________
		   ______11________________
		   _________1______________
		   __________3_____________
		   ___________44___________
		   _____________33_________
		   _______________2________
		   ________________1_______
		   _________________4______
		   ___________________2____
		   ____________________33__
		   ______________________4_
	*/

	/* if there's only one memory region, don't bother */
	if (*pnr_map < 2)
		return -1;

	old_nr = *pnr_map;

	/* bail out if we find any unreasonable addresses in bios map */
	for (i=0; i<old_nr; i++)
		if (biosmap[i].addr + biosmap[i].size < biosmap[i].addr)
			return -1;

	/* create pointers for initial change-point information (for sorting) */
	for (i=0; i < 2*old_nr; i++)
		change_point[i] = &change_point_list[i];

	/* record all known change-points (starting and ending addresses) */
	chgidx = 0;
	for (i=0; i < old_nr; i++)	{
		change_point[chgidx]->addr = biosmap[i].addr;
		change_point[chgidx++]->pbios = &biosmap[i];
		change_point[chgidx]->addr = biosmap[i].addr + biosmap[i].size;
		change_point[chgidx++]->pbios = &biosmap[i];
	}

	/* sort change-point list by memory addresses (low -> high) */
	still_changing = 1;
	while (still_changing)	{
		still_changing = 0;
		for (i=1; i < 2*old_nr; i++)  {
			/* if <current_addr> > <last_addr>, swap */
			/* or, if current=<start_addr> & last=<end_addr>, swap */
			if ((change_point[i]->addr < change_point[i-1]->addr) ||
				((change_point[i]->addr == change_point[i-1]->addr) &&
				 (change_point[i]->addr == change_point[i]->pbios->addr) &&
				 (change_point[i-1]->addr != change_point[i-1]->pbios->addr))
			   )
			{
				change_tmp = change_point[i];
				change_point[i] = change_point[i-1];
				change_point[i-1] = change_tmp;
				still_changing=1;
			}
		}
	}

	/* create a new bios memory map, removing overlaps */
	overlap_entries=0;	 /* number of entries in the overlap table */
	new_bios_entry=0;	 /* index for creating new bios map entries */
	last_type = 0;		 /* start with undefined memory type */
	last_addr = 0;		 /* start with 0 as last starting address */
	/* loop through change-points, determining affect on the new bios map */
	for (chgidx=0; chgidx < 2*old_nr; chgidx++)
	{
		/* keep track of all overlapping bios entries */
		if (change_point[chgidx]->addr == change_point[chgidx]->pbios->addr)
		{
			/* add map entry to overlap list (> 1 entry implies an overlap) */
			overlap_list[overlap_entries++]=change_point[chgidx]->pbios;
		}
		else
		{
			/* remove entry from list (order independent, so swap with last) */
			for (i=0; i<overlap_entries; i++)
			{
				if (overlap_list[i] == change_point[chgidx]->pbios)
					overlap_list[i] = overlap_list[overlap_entries-1];
			}
			overlap_entries--;
		}
		/* if there are overlapping entries, decide which "type" to use */
		/* (larger value takes precedence -- 1=usable, 2,3,4,4+=unusable) */
		current_type = 0;
		for (i=0; i<overlap_entries; i++)
			if (overlap_list[i]->type > current_type)
				current_type = overlap_list[i]->type;
		/* continue building up new bios map based on this information */
		if (current_type != last_type)	{
			if (last_type != 0)	 {
				new_bios[new_bios_entry].size =
					change_point[chgidx]->addr - last_addr;
				/* move forward only if the new size was non-zero */
				if (new_bios[new_bios_entry].size != 0)
					if (++new_bios_entry >= E820MAX)
						break; 	/* no more space left for new bios entries */
			}
			if (current_type != 0)	{
				new_bios[new_bios_entry].addr = change_point[chgidx]->addr;
				new_bios[new_bios_entry].type = current_type;
				last_addr=change_point[chgidx]->addr;
			}
			last_type = current_type;
		}
	}
	new_nr = new_bios_entry;   /* retain count for new bios entries */

	/* copy new bios mapping into original location */
	memcpy(biosmap, new_bios, new_nr*sizeof(struct e820entry));
	*pnr_map = new_nr;

	return 0;
}

/*
 * Copy the BIOS e820 map into a safe place.
 *
 * Sanity-check it while we're at it..
 *
 * If we're lucky and live on a modern system, the setup code
 * will have given us a memory map that we can use to properly
 * set up memory.  If we aren't, we'll fake a memory map.
 *
 * We check to see that the memory map contains at least 2 elements
 * before we'll use it, because the detection code in setup.S may
 * not be perfect and most every PC known to man has two memory
 * regions: one from 0 to 640k, and one from 1mb up.  (The IBM
 * thinkpad 560x, for example, does not cooperate with the memory
 * detection code.)
 */
static int __init copy_e820_map(struct e820entry * biosmap, int nr_map)
{
	/* Only one memory region (or negative)? Ignore it */
	if (nr_map < 2)
		return -1;

	do {
		unsigned long start = biosmap->addr;
		unsigned long size = biosmap->size;
		unsigned long end = start + size;
		unsigned long type = biosmap->type;

		/* Overflow in 64 bits? Ignore the memory map. */
		if (start > end)
			return -1;

		/*
		 * Some BIOSes claim RAM in the 640k - 1M region.
		 * Not right. Fix it up.
		 * 
		 * This should be removed on Hammer which is supposed to not
		 * have non e820 covered ISA mappings there, but I had some strange
		 * problems so it stays for now.  -AK
		 */
		if (type == E820_RAM) {
			if (start < 0x100000ULL && end > 0xA0000ULL) {
				if (start < 0xA0000ULL)
					add_memory_region(start, 0xA0000ULL-start, type);
				if (end <= 0x100000ULL)
					continue;
				start = 0x100000ULL;
				size = end - start;
			}
		}

		add_memory_region(start, size, type);
	} while (biosmap++,--nr_map);
	return 0;
}

void __init setup_memory_region(void)
{
	char *who = "BIOS-e820";

	/*
	 * Try to copy the BIOS-supplied E820-map.
	 *
	 * Otherwise fake a memory map; one section from 0k->640k,
	 * the next section from 1mb->appropriate_mem_k
	 */
	sanitize_e820_map(E820_MAP, &E820_MAP_NR);
	if (copy_e820_map(E820_MAP, E820_MAP_NR) < 0) {
		unsigned long mem_size;

		/* compare results from other methods and take the greater */
		if (ALT_MEM_K < EXT_MEM_K) {
			mem_size = EXT_MEM_K;
			who = "BIOS-88";
		} else {
			mem_size = ALT_MEM_K;
			who = "BIOS-e801";
		}

		e820.nr_map = 0;
		add_memory_region(0, LOWMEMSIZE(), E820_RAM);
		add_memory_region(HIGH_MEMORY, mem_size << 10, E820_RAM);
  	}
	printk(KERN_INFO "BIOS-provided physical RAM map:\n");
	e820_print_map(who);
}

void __init parse_memopt(char *p, char **from) 
{ 
	/*
	 * mem=XXX[kKmM] limits kernel memory to XXX+1MB
	 *
	 * It would be more logical to count from 0 instead of from
	 * HIGH_MEMORY, but we keep that for now for i386 compatibility. 
	 *	
	 * No support for custom mapping like i386.  The reason is
	 * that we need to read the e820 map anyways to handle the
	 * ACPI mappings in the direct map.  Also on x86-64 there
	 * should be always a good e820 map. This is only an upper
	 * limit, you cannot force usage of memory not in e820.
	 *
	 * -AK
			 */
	end_user_pfn = memparse(p, from) + HIGH_MEMORY;
	end_user_pfn >>= PAGE_SHIFT;	
} 

