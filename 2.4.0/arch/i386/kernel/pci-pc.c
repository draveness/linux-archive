/*
 *	Low-Level PCI Support for PC
 *
 *	(c) 1999--2000 Martin Mares <mj@suse.cz>
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/ioport.h>

#include <asm/segment.h>
#include <asm/io.h>

#include "pci-i386.h"

unsigned int pci_probe = PCI_PROBE_BIOS | PCI_PROBE_CONF1 | PCI_PROBE_CONF2;

int pcibios_last_bus = -1;
struct pci_bus *pci_root_bus;
struct pci_ops *pci_root_ops;

/*
 * Direct access to PCI hardware...
 */

#ifdef CONFIG_PCI_DIRECT

/*
 * Functions for accessing PCI configuration space with type 1 accesses
 */

#define CONFIG_CMD(dev, where)   (0x80000000 | (dev->bus->number << 16) | (dev->devfn << 8) | (where & ~3))

static int pci_conf1_read_config_byte(struct pci_dev *dev, int where, u8 *value)
{
	outl(CONFIG_CMD(dev,where), 0xCF8);
	*value = inb(0xCFC + (where&3));
	return PCIBIOS_SUCCESSFUL;
}

static int pci_conf1_read_config_word(struct pci_dev *dev, int where, u16 *value)
{
	outl(CONFIG_CMD(dev,where), 0xCF8);    
	*value = inw(0xCFC + (where&2));
	return PCIBIOS_SUCCESSFUL;    
}

static int pci_conf1_read_config_dword(struct pci_dev *dev, int where, u32 *value)
{
	outl(CONFIG_CMD(dev,where), 0xCF8);
	*value = inl(0xCFC);
	return PCIBIOS_SUCCESSFUL;    
}

static int pci_conf1_write_config_byte(struct pci_dev *dev, int where, u8 value)
{
	outl(CONFIG_CMD(dev,where), 0xCF8);    
	outb(value, 0xCFC + (where&3));
	return PCIBIOS_SUCCESSFUL;
}

static int pci_conf1_write_config_word(struct pci_dev *dev, int where, u16 value)
{
	outl(CONFIG_CMD(dev,where), 0xCF8);
	outw(value, 0xCFC + (where&2));
	return PCIBIOS_SUCCESSFUL;
}

static int pci_conf1_write_config_dword(struct pci_dev *dev, int where, u32 value)
{
	outl(CONFIG_CMD(dev,where), 0xCF8);
	outl(value, 0xCFC);
	return PCIBIOS_SUCCESSFUL;
}

#undef CONFIG_CMD

static struct pci_ops pci_direct_conf1 = {
	pci_conf1_read_config_byte,
	pci_conf1_read_config_word,
	pci_conf1_read_config_dword,
	pci_conf1_write_config_byte,
	pci_conf1_write_config_word,
	pci_conf1_write_config_dword
};

/*
 * Functions for accessing PCI configuration space with type 2 accesses
 */

#define IOADDR(devfn, where)	((0xC000 | ((devfn & 0x78) << 5)) + where)
#define FUNC(devfn)		(((devfn & 7) << 1) | 0xf0)
#define SET(dev)		if (dev->devfn & 0x80) return PCIBIOS_DEVICE_NOT_FOUND;		\
				outb(FUNC(dev->devfn), 0xCF8);					\
				outb(dev->bus->number, 0xCFA);

static int pci_conf2_read_config_byte(struct pci_dev *dev, int where, u8 *value)
{
	SET(dev);
	*value = inb(IOADDR(dev->devfn,where));
	outb (0, 0xCF8);
	return PCIBIOS_SUCCESSFUL;
}

static int pci_conf2_read_config_word(struct pci_dev *dev, int where, u16 *value)
{
	SET(dev);
	*value = inw(IOADDR(dev->devfn,where));
	outb (0, 0xCF8);
	return PCIBIOS_SUCCESSFUL;
}

static int pci_conf2_read_config_dword(struct pci_dev *dev, int where, u32 *value)
{
	SET(dev);
	*value = inl (IOADDR(dev->devfn,where));    
	outb (0, 0xCF8);    
	return PCIBIOS_SUCCESSFUL;
}

static int pci_conf2_write_config_byte(struct pci_dev *dev, int where, u8 value)
{
	SET(dev);
	outb (value, IOADDR(dev->devfn,where));
	outb (0, 0xCF8);    
	return PCIBIOS_SUCCESSFUL;
}

static int pci_conf2_write_config_word(struct pci_dev *dev, int where, u16 value)
{
	SET(dev);
	outw (value, IOADDR(dev->devfn,where));
	outb (0, 0xCF8);    
	return PCIBIOS_SUCCESSFUL;
}

static int pci_conf2_write_config_dword(struct pci_dev *dev, int where, u32 value)
{
	SET(dev);
	outl (value, IOADDR(dev->devfn,where));    
	outb (0, 0xCF8);    
	return PCIBIOS_SUCCESSFUL;
}

#undef SET
#undef IOADDR
#undef FUNC

static struct pci_ops pci_direct_conf2 = {
	pci_conf2_read_config_byte,
	pci_conf2_read_config_word,
	pci_conf2_read_config_dword,
	pci_conf2_write_config_byte,
	pci_conf2_write_config_word,
	pci_conf2_write_config_dword
};

/*
 * Before we decide to use direct hardware access mechanisms, we try to do some
 * trivial checks to ensure it at least _seems_ to be working -- we just test
 * whether bus 00 contains a host bridge (this is similar to checking
 * techniques used in XFree86, but ours should be more reliable since we
 * attempt to make use of direct access hints provided by the PCI BIOS).
 *
 * This should be close to trivial, but it isn't, because there are buggy
 * chipsets (yes, you guessed it, by Intel and Compaq) that have no class ID.
 */
static int __init pci_sanity_check(struct pci_ops *o)
{
	u16 x;
	struct pci_bus bus;		/* Fake bus and device */
	struct pci_dev dev;

	if (pci_probe & PCI_NO_CHECKS)
		return 1;
	bus.number = 0;
	dev.bus = &bus;
	for(dev.devfn=0; dev.devfn < 0x100; dev.devfn++)
		if ((!o->read_word(&dev, PCI_CLASS_DEVICE, &x) &&
		     (x == PCI_CLASS_BRIDGE_HOST || x == PCI_CLASS_DISPLAY_VGA)) ||
		    (!o->read_word(&dev, PCI_VENDOR_ID, &x) &&
		     (x == PCI_VENDOR_ID_INTEL || x == PCI_VENDOR_ID_COMPAQ)))
			return 1;
	DBG("PCI: Sanity check failed\n");
	return 0;
}

static struct pci_ops * __init pci_check_direct(void)
{
	unsigned int tmp;
	unsigned long flags;

	__save_flags(flags); __cli();

	/*
	 * Check if configuration type 1 works.
	 */
	if (pci_probe & PCI_PROBE_CONF1) {
		outb (0x01, 0xCFB);
		tmp = inl (0xCF8);
		outl (0x80000000, 0xCF8);
		if (inl (0xCF8) == 0x80000000 &&
		    pci_sanity_check(&pci_direct_conf1)) {
			outl (tmp, 0xCF8);
			__restore_flags(flags);
			printk("PCI: Using configuration type 1\n");
			request_region(0xCF8, 8, "PCI conf1");
			return &pci_direct_conf1;
		}
		outl (tmp, 0xCF8);
	}

	/*
	 * Check if configuration type 2 works.
	 */
	if (pci_probe & PCI_PROBE_CONF2) {
		outb (0x00, 0xCFB);
		outb (0x00, 0xCF8);
		outb (0x00, 0xCFA);
		if (inb (0xCF8) == 0x00 && inb (0xCFA) == 0x00 &&
		    pci_sanity_check(&pci_direct_conf2)) {
			__restore_flags(flags);
			printk("PCI: Using configuration type 2\n");
			request_region(0xCF8, 4, "PCI conf2");
			return &pci_direct_conf2;
		}
	}

	__restore_flags(flags);
	return NULL;
}

#endif

/*
 * BIOS32 and PCI BIOS handling.
 */

#ifdef CONFIG_PCI_BIOS

#define PCIBIOS_PCI_FUNCTION_ID 	0xb1XX
#define PCIBIOS_PCI_BIOS_PRESENT 	0xb101
#define PCIBIOS_FIND_PCI_DEVICE		0xb102
#define PCIBIOS_FIND_PCI_CLASS_CODE	0xb103
#define PCIBIOS_GENERATE_SPECIAL_CYCLE	0xb106
#define PCIBIOS_READ_CONFIG_BYTE	0xb108
#define PCIBIOS_READ_CONFIG_WORD	0xb109
#define PCIBIOS_READ_CONFIG_DWORD	0xb10a
#define PCIBIOS_WRITE_CONFIG_BYTE	0xb10b
#define PCIBIOS_WRITE_CONFIG_WORD	0xb10c
#define PCIBIOS_WRITE_CONFIG_DWORD	0xb10d
#define PCIBIOS_GET_ROUTING_OPTIONS	0xb10e
#define PCIBIOS_SET_PCI_HW_INT		0xb10f

/* BIOS32 signature: "_32_" */
#define BIOS32_SIGNATURE	(('_' << 0) + ('3' << 8) + ('2' << 16) + ('_' << 24))

/* PCI signature: "PCI " */
#define PCI_SIGNATURE		(('P' << 0) + ('C' << 8) + ('I' << 16) + (' ' << 24))

/* PCI service signature: "$PCI" */
#define PCI_SERVICE		(('$' << 0) + ('P' << 8) + ('C' << 16) + ('I' << 24))

/* PCI BIOS hardware mechanism flags */
#define PCIBIOS_HW_TYPE1		0x01
#define PCIBIOS_HW_TYPE2		0x02
#define PCIBIOS_HW_TYPE1_SPEC		0x10
#define PCIBIOS_HW_TYPE2_SPEC		0x20

/*
 * This is the standard structure used to identify the entry point
 * to the BIOS32 Service Directory, as documented in
 * 	Standard BIOS 32-bit Service Directory Proposal
 * 	Revision 0.4 May 24, 1993
 * 	Phoenix Technologies Ltd.
 *	Norwood, MA
 * and the PCI BIOS specification.
 */

union bios32 {
	struct {
		unsigned long signature;	/* _32_ */
		unsigned long entry;		/* 32 bit physical address */
		unsigned char revision;		/* Revision level, 0 */
		unsigned char length;		/* Length in paragraphs should be 01 */
		unsigned char checksum;		/* All bytes must add up to zero */
		unsigned char reserved[5]; 	/* Must be zero */
	} fields;
	char chars[16];
};

/*
 * Physical address of the service directory.  I don't know if we're
 * allowed to have more than one of these or not, so just in case
 * we'll make pcibios_present() take a memory start parameter and store
 * the array there.
 */

static struct {
	unsigned long address;
	unsigned short segment;
} bios32_indirect = { 0, __KERNEL_CS };

/*
 * Returns the entry point for the given service, NULL on error
 */

static unsigned long bios32_service(unsigned long service)
{
	unsigned char return_code;	/* %al */
	unsigned long address;		/* %ebx */
	unsigned long length;		/* %ecx */
	unsigned long entry;		/* %edx */
	unsigned long flags;

	__save_flags(flags); __cli();
	__asm__("lcall (%%edi); cld"
		: "=a" (return_code),
		  "=b" (address),
		  "=c" (length),
		  "=d" (entry)
		: "0" (service),
		  "1" (0),
		  "D" (&bios32_indirect));
	__restore_flags(flags);

	switch (return_code) {
		case 0:
			return address + entry;
		case 0x80:	/* Not present */
			printk("bios32_service(0x%lx): not present\n", service);
			return 0;
		default: /* Shouldn't happen */
			printk("bios32_service(0x%lx): returned 0x%x -- BIOS bug!\n",
				service, return_code);
			return 0;
	}
}

static struct {
	unsigned long address;
	unsigned short segment;
} pci_indirect = { 0, __KERNEL_CS };

static int pci_bios_present;

static int __init check_pcibios(void)
{
	u32 signature, eax, ebx, ecx;
	u8 status, major_ver, minor_ver, hw_mech;
	unsigned long flags, pcibios_entry;

	if ((pcibios_entry = bios32_service(PCI_SERVICE))) {
		pci_indirect.address = pcibios_entry + PAGE_OFFSET;

		__save_flags(flags); __cli();
		__asm__(
			"lcall (%%edi); cld\n\t"
			"jc 1f\n\t"
			"xor %%ah, %%ah\n"
			"1:"
			: "=d" (signature),
			  "=a" (eax),
			  "=b" (ebx),
			  "=c" (ecx)
			: "1" (PCIBIOS_PCI_BIOS_PRESENT),
			  "D" (&pci_indirect)
			: "memory");
		__restore_flags(flags);

		status = (eax >> 8) & 0xff;
		hw_mech = eax & 0xff;
		major_ver = (ebx >> 8) & 0xff;
		minor_ver = ebx & 0xff;
		if (pcibios_last_bus < 0)
			pcibios_last_bus = ecx & 0xff;
		DBG("PCI: BIOS probe returned s=%02x hw=%02x ver=%02x.%02x l=%02x\n",
			status, hw_mech, major_ver, minor_ver, pcibios_last_bus);
		if (status || signature != PCI_SIGNATURE) {
			printk (KERN_ERR "PCI: BIOS BUG #%x[%08x] found, report to <mj@suse.cz>\n",
				status, signature);
			return 0;
		}
		printk("PCI: PCI BIOS revision %x.%02x entry at 0x%lx, last bus=%d\n",
			major_ver, minor_ver, pcibios_entry, pcibios_last_bus);
#ifdef CONFIG_PCI_DIRECT
		if (!(hw_mech & PCIBIOS_HW_TYPE1))
			pci_probe &= ~PCI_PROBE_CONF1;
		if (!(hw_mech & PCIBIOS_HW_TYPE2))
			pci_probe &= ~PCI_PROBE_CONF2;
#endif
		return 1;
	}
	return 0;
}

static int __init pci_bios_find_device (unsigned short vendor, unsigned short device_id,
					unsigned short index, unsigned char *bus, unsigned char *device_fn)
{
	unsigned short bx;
	unsigned short ret;

	__asm__("lcall (%%edi); cld\n\t"
		"jc 1f\n\t"
		"xor %%ah, %%ah\n"
		"1:"
		: "=b" (bx),
		  "=a" (ret)
		: "1" (PCIBIOS_FIND_PCI_DEVICE),
		  "c" (device_id),
		  "d" (vendor),
		  "S" ((int) index),
		  "D" (&pci_indirect));
	*bus = (bx >> 8) & 0xff;
	*device_fn = bx & 0xff;
	return (int) (ret & 0xff00) >> 8;
}

static int pci_bios_read_config_byte(struct pci_dev *dev, int where, u8 *value)
{
	unsigned long ret;
	unsigned long bx = (dev->bus->number << 8) | dev->devfn;

	__asm__("lcall (%%esi); cld\n\t"
		"jc 1f\n\t"
		"xor %%ah, %%ah\n"
		"1:"
		: "=c" (*value),
		  "=a" (ret)
		: "1" (PCIBIOS_READ_CONFIG_BYTE),
		  "b" (bx),
		  "D" ((long) where),
		  "S" (&pci_indirect));
	return (int) (ret & 0xff00) >> 8;
}

static int pci_bios_read_config_word(struct pci_dev *dev, int where, u16 *value)
{
	unsigned long ret;
	unsigned long bx = (dev->bus->number << 8) | dev->devfn;

	__asm__("lcall (%%esi); cld\n\t"
		"jc 1f\n\t"
		"xor %%ah, %%ah\n"
		"1:"
		: "=c" (*value),
		  "=a" (ret)
		: "1" (PCIBIOS_READ_CONFIG_WORD),
		  "b" (bx),
		  "D" ((long) where),
		  "S" (&pci_indirect));
	return (int) (ret & 0xff00) >> 8;
}

static int pci_bios_read_config_dword(struct pci_dev *dev, int where, u32 *value)
{
	unsigned long ret;
	unsigned long bx = (dev->bus->number << 8) | dev->devfn;

	__asm__("lcall (%%esi); cld\n\t"
		"jc 1f\n\t"
		"xor %%ah, %%ah\n"
		"1:"
		: "=c" (*value),
		  "=a" (ret)
		: "1" (PCIBIOS_READ_CONFIG_DWORD),
		  "b" (bx),
		  "D" ((long) where),
		  "S" (&pci_indirect));
	return (int) (ret & 0xff00) >> 8;
}

static int pci_bios_write_config_byte(struct pci_dev *dev, int where, u8 value)
{
	unsigned long ret;
	unsigned long bx = (dev->bus->number << 8) | dev->devfn;

	__asm__("lcall (%%esi); cld\n\t"
		"jc 1f\n\t"
		"xor %%ah, %%ah\n"
		"1:"
		: "=a" (ret)
		: "0" (PCIBIOS_WRITE_CONFIG_BYTE),
		  "c" (value),
		  "b" (bx),
		  "D" ((long) where),
		  "S" (&pci_indirect));
	return (int) (ret & 0xff00) >> 8;
}

static int pci_bios_write_config_word(struct pci_dev *dev, int where, u16 value)
{
	unsigned long ret;
	unsigned long bx = (dev->bus->number << 8) | dev->devfn;

	__asm__("lcall (%%esi); cld\n\t"
		"jc 1f\n\t"
		"xor %%ah, %%ah\n"
		"1:"
		: "=a" (ret)
		: "0" (PCIBIOS_WRITE_CONFIG_WORD),
		  "c" (value),
		  "b" (bx),
		  "D" ((long) where),
		  "S" (&pci_indirect));
	return (int) (ret & 0xff00) >> 8;
}

static int pci_bios_write_config_dword(struct pci_dev *dev, int where, u32 value)
{
	unsigned long ret;
	unsigned long bx = (dev->bus->number << 8) | dev->devfn;

	__asm__("lcall (%%esi); cld\n\t"
		"jc 1f\n\t"
		"xor %%ah, %%ah\n"
		"1:"
		: "=a" (ret)
		: "0" (PCIBIOS_WRITE_CONFIG_DWORD),
		  "c" (value),
		  "b" (bx),
		  "D" ((long) where),
		  "S" (&pci_indirect));
	return (int) (ret & 0xff00) >> 8;
}

/*
 * Function table for BIOS32 access
 */

static struct pci_ops pci_bios_access = {
      pci_bios_read_config_byte,
      pci_bios_read_config_word,
      pci_bios_read_config_dword,
      pci_bios_write_config_byte,
      pci_bios_write_config_word,
      pci_bios_write_config_dword
};

/*
 * Try to find PCI BIOS.
 */

static struct pci_ops * __init pci_find_bios(void)
{
	union bios32 *check;
	unsigned char sum;
	int i, length;

	/*
	 * Follow the standard procedure for locating the BIOS32 Service
	 * directory by scanning the permissible address range from
	 * 0xe0000 through 0xfffff for a valid BIOS32 structure.
	 */

	for (check = (union bios32 *) __va(0xe0000);
	     check <= (union bios32 *) __va(0xffff0);
	     ++check) {
		if (check->fields.signature != BIOS32_SIGNATURE)
			continue;
		length = check->fields.length * 16;
		if (!length)
			continue;
		sum = 0;
		for (i = 0; i < length ; ++i)
			sum += check->chars[i];
		if (sum != 0)
			continue;
		if (check->fields.revision != 0) {
			printk("PCI: unsupported BIOS32 revision %d at 0x%p, report to <mj@suse.cz>\n",
				check->fields.revision, check);
			continue;
		}
		DBG("PCI: BIOS32 Service Directory structure at 0x%p\n", check);
		if (check->fields.entry >= 0x100000) {
			printk("PCI: BIOS32 entry (0x%p) in high memory, cannot use.\n", check);
			return NULL;
		} else {
			unsigned long bios32_entry = check->fields.entry;
			DBG("PCI: BIOS32 Service Directory entry at 0x%lx\n", bios32_entry);
			bios32_indirect.address = bios32_entry + PAGE_OFFSET;
			if (check_pcibios())
				return &pci_bios_access;
		}
		break;	/* Hopefully more than one BIOS32 cannot happen... */
	}

	return NULL;
}

/*
 * Sort the device list according to PCI BIOS. Nasty hack, but since some
 * fool forgot to define the `correct' device order in the PCI BIOS specs
 * and we want to be (possibly bug-to-bug ;-]) compatible with older kernels
 * which used BIOS ordering, we are bound to do this...
 */

static void __init pcibios_sort(void)
{
	LIST_HEAD(sorted_devices);
	struct list_head *ln;
	struct pci_dev *dev, *d;
	int idx, found;
	unsigned char bus, devfn;

	DBG("PCI: Sorting device list...\n");
	while (!list_empty(&pci_devices)) {
		ln = pci_devices.next;
		dev = pci_dev_g(ln);
		idx = found = 0;
		while (pci_bios_find_device(dev->vendor, dev->device, idx, &bus, &devfn) == PCIBIOS_SUCCESSFUL) {
			idx++;
			for (ln=pci_devices.next; ln != &pci_devices; ln=ln->next) {
				d = pci_dev_g(ln);
				if (d->bus->number == bus && d->devfn == devfn) {
					list_del(&d->global_list);
					list_add_tail(&d->global_list, &sorted_devices);
					if (d == dev)
						found = 1;
					break;
				}
			}
			if (ln == &pci_devices) {
				printk("PCI: BIOS reporting unknown device %02x:%02x\n", bus, devfn);
				/*
				 * We must not continue scanning as several buggy BIOSes
				 * return garbage after the last device. Grr.
				 */
				break;
			}
		}
		if (!found) {
			printk("PCI: Device %02x:%02x not found by BIOS\n",
				dev->bus->number, dev->devfn);
			list_del(&dev->global_list);
			list_add_tail(&dev->global_list, &sorted_devices);
		}
	}
	list_splice(&sorted_devices, &pci_devices);
}

/*
 *  BIOS Functions for IRQ Routing
 */

struct irq_routing_options {
	u16 size;
	struct irq_info *table;
	u16 segment;
} __attribute__((packed));

struct irq_routing_table * __init pcibios_get_irq_routing_table(void)
{
	struct irq_routing_options opt;
	struct irq_routing_table *rt = NULL;
	int ret, map;
	unsigned long page;

	if (!pci_bios_present)
		return NULL;
	page = __get_free_page(GFP_KERNEL);
	if (!page)
		return NULL;
	opt.table = (struct irq_info *) page;
	opt.size = PAGE_SIZE;
	opt.segment = __KERNEL_DS;

	DBG("PCI: Fetching IRQ routing table... ");
	__asm__("push %%es\n\t"
		"push %%ds\n\t"
		"pop  %%es\n\t"
		"lcall (%%esi); cld\n\t"
		"pop %%es\n\t"
		"jc 1f\n\t"
		"xor %%ah, %%ah\n"
		"1:"
		: "=a" (ret),
		  "=b" (map)
		: "0" (PCIBIOS_GET_ROUTING_OPTIONS),
		  "1" (0),
		  "D" ((long) &opt),
		  "S" (&pci_indirect));
	DBG("OK  ret=%d, size=%d, map=%x\n", ret, opt.size, map);
	if (ret & 0xff00)
		printk(KERN_ERR "PCI: Error %02x when fetching IRQ routing table.\n", (ret >> 8) & 0xff);
	else if (opt.size) {
		rt = kmalloc(sizeof(struct irq_routing_table) + opt.size, GFP_KERNEL);
		if (rt) {
			memset(rt, 0, sizeof(struct irq_routing_table));
			rt->size = opt.size + sizeof(struct irq_routing_table);
			rt->exclusive_irqs = map;
			memcpy(rt->slots, (void *) page, opt.size);
			printk("PCI: Using BIOS Interrupt Routing Table\n");
		}
	}
	free_page(page);
	return rt;
}


int pcibios_set_irq_routing(struct pci_dev *dev, int pin, int irq)
{
	int ret;

	__asm__("lcall (%%esi); cld\n\t"
		"jc 1f\n\t"
		"xor %%ah, %%ah\n"
		"1:"
		: "=a" (ret)
		: "0" (PCIBIOS_SET_PCI_HW_INT),
		  "b" ((dev->bus->number << 8) | dev->devfn),
		  "c" ((irq << 8) | (pin + 10)),
		  "S" (&pci_indirect));
	return !(ret & 0xff00);
}

#endif

/*
 * Several buggy motherboards address only 16 devices and mirror
 * them to next 16 IDs. We try to detect this `feature' on all
 * primary buses (those containing host bridges as they are
 * expected to be unique) and remove the ghost devices.
 */

static void __init pcibios_fixup_ghosts(struct pci_bus *b)
{
	struct list_head *ln, *mn;
	struct pci_dev *d, *e;
	int mirror = PCI_DEVFN(16,0);
	int seen_host_bridge = 0;
	int i;

	DBG("PCI: Scanning for ghost devices on bus %d\n", b->number);
	for (ln=b->devices.next; ln != &b->devices; ln=ln->next) {
		d = pci_dev_b(ln);
		if ((d->class >> 8) == PCI_CLASS_BRIDGE_HOST)
			seen_host_bridge++;
		for (mn=ln->next; mn != &b->devices; mn=mn->next) {
			e = pci_dev_b(mn);
			if (e->devfn != d->devfn + mirror ||
			    e->vendor != d->vendor ||
			    e->device != d->device ||
			    e->class != d->class)
				continue;
			for(i=0; i<PCI_NUM_RESOURCES; i++)
				if (e->resource[i].start != d->resource[i].start ||
				    e->resource[i].end != d->resource[i].end ||
				    e->resource[i].flags != d->resource[i].flags)
					continue;
			break;
		}
		if (mn == &b->devices)
			return;
	}
	if (!seen_host_bridge)
		return;
	printk("PCI: Ignoring ghost devices on bus %02x\n", b->number);

	ln = &b->devices;
	while (ln->next != &b->devices) {
		d = pci_dev_b(ln->next);
		if (d->devfn >= mirror) {
			list_del(&d->global_list);
			list_del(&d->bus_list);
			kfree(d);
		} else
			ln = ln->next;
	}
}

/*
 * Discover remaining PCI buses in case there are peer host bridges.
 * We use the number of last PCI bus provided by the PCI BIOS.
 */
static void __init pcibios_fixup_peer_bridges(void)
{
	int n;
	struct pci_bus bus;
	struct pci_dev dev;
	u16 l;

	if (pcibios_last_bus <= 0 || pcibios_last_bus >= 0xff)
		return;
	DBG("PCI: Peer bridge fixup\n");
	for (n=0; n <= pcibios_last_bus; n++) {
		if (pci_bus_exists(&pci_root_buses, n))
			continue;
		bus.number = n;
		bus.ops = pci_root_ops;
		dev.bus = &bus;
		for(dev.devfn=0; dev.devfn<256; dev.devfn += 8)
			if (!pci_read_config_word(&dev, PCI_VENDOR_ID, &l) &&
			    l != 0x0000 && l != 0xffff) {
				DBG("Found device at %02x:%02x [%04x]\n", n, dev.devfn, l);
				printk("PCI: Discovered peer bus %02x\n", n);
				pci_scan_bus(n, pci_root_ops, NULL);
				break;
			}
	}
}

/*
 * Exceptions for specific devices. Usually work-arounds for fatal design flaws.
 */

static void __init pci_fixup_i450nx(struct pci_dev *d)
{
	/*
	 * i450NX -- Find and scan all secondary buses on all PXB's.
	 */
	int pxb, reg;
	u8 busno, suba, subb;
	printk("PCI: Searching for i450NX host bridges on %s\n", d->slot_name);
	reg = 0xd0;
	for(pxb=0; pxb<2; pxb++) {
		pci_read_config_byte(d, reg++, &busno);
		pci_read_config_byte(d, reg++, &suba);
		pci_read_config_byte(d, reg++, &subb);
		DBG("i450NX PXB %d: %02x/%02x/%02x\n", pxb, busno, suba, subb);
		if (busno)
			pci_scan_bus(busno, pci_root_ops, NULL);	/* Bus A */
		if (suba < subb)
			pci_scan_bus(suba+1, pci_root_ops, NULL);	/* Bus B */
	}
	pcibios_last_bus = -1;
}

static void __init pci_fixup_i450gx(struct pci_dev *d)
{
	/*
	 * i450GX and i450KX -- Find and scan all secondary buses.
	 * (called separately for each PCI bridge found)
	 */
	u8 busno;
	pci_read_config_byte(d, 0x4a, &busno);
	printk("PCI: i440KX/GX host bridge %s: secondary bus %02x\n", d->slot_name, busno);
	pci_scan_bus(busno, pci_root_ops, NULL);
	pcibios_last_bus = -1;
}

static void __init pci_fixup_serverworks(struct pci_dev *d)
{
	/*
	 * ServerWorks host bridges -- Find and scan all secondary buses.
	 * Register 0x44 contains first, 0x45 last bus number routed there.
	 */
	u8 busno;
	pci_read_config_byte(d, 0x44, &busno);
	printk("PCI: ServerWorks host bridge: secondary bus %02x\n", busno);
	pci_scan_bus(busno, pci_root_ops, NULL);
	pcibios_last_bus = -1;
}

static void __init pci_fixup_compaq(struct pci_dev *d)
{
	/*	
	 * Compaq host bridges -- Find and scan all secondary buses.
	 * This time registers 0xc8 and 0xc9.
	 */
	u8 busno;
	pci_read_config_byte(d, 0xc8, &busno);
	printk("PCI: Compaq host bridge: secondary bus %02x\n", busno);
	pci_scan_bus(busno, pci_root_ops, NULL);
	pcibios_last_bus = -1;
}

static void __init pci_fixup_umc_ide(struct pci_dev *d)
{
	/*
	 * UM8886BF IDE controller sets region type bits incorrectly,
	 * therefore they look like memory despite of them being I/O.
	 */
	int i;

	printk("PCI: Fixing base address flags for device %s\n", d->slot_name);
	for(i=0; i<4; i++)
		d->resource[i].flags |= PCI_BASE_ADDRESS_SPACE_IO;
}

static void __init pci_fixup_ide_bases(struct pci_dev *d)
{
	int i;

	/*
	 * PCI IDE controllers use non-standard I/O port decoding, respect it.
	 */
	if ((d->class >> 8) != PCI_CLASS_STORAGE_IDE)
		return;
	DBG("PCI: IDE base address fixup for %s\n", d->slot_name);
	for(i=0; i<4; i++) {
		struct resource *r = &d->resource[i];
		if ((r->start & ~0x80) == 0x374) {
			r->start |= 2;
			r->end = r->start;
		}
	}
}

static void __init pci_fixup_ide_trash(struct pci_dev *d)
{
	int i;

	/*
	 * There exist PCI IDE controllers which have utter garbage
	 * in first four base registers. Ignore that.
	 */
	DBG("PCI: IDE base address trash cleared for %s\n", d->slot_name);
	for(i=0; i<4; i++)
		d->resource[i].start = d->resource[i].end = d->resource[i].flags = 0;
}

static void __init pci_fixup_latency(struct pci_dev *d)
{
	/*
	 *  SiS 5597 and 5598 chipsets require latency timer set to
	 *  at most 32 to avoid lockups.
	 */
	DBG("PCI: Setting max latency to 32\n");
	pcibios_max_latency = 32;
}

struct pci_fixup pcibios_fixups[] = {
	{ PCI_FIXUP_HEADER,	PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82451NX,	pci_fixup_i450nx },
	{ PCI_FIXUP_HEADER,	PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82454GX,	pci_fixup_i450gx },
	{ PCI_FIXUP_HEADER,	PCI_VENDOR_ID_SERVERWORKS,	PCI_DEVICE_ID_SERVERWORKS_HE,		pci_fixup_serverworks },
	{ PCI_FIXUP_HEADER,	PCI_VENDOR_ID_SERVERWORKS,	PCI_DEVICE_ID_SERVERWORKS_LE,		pci_fixup_serverworks },
	{ PCI_FIXUP_HEADER,	PCI_VENDOR_ID_SERVERWORKS,	PCI_DEVICE_ID_SERVERWORKS_CMIC_HE,	pci_fixup_serverworks },
	{ PCI_FIXUP_HEADER,	PCI_VENDOR_ID_COMPAQ,	PCI_DEVICE_ID_COMPAQ_6010,	pci_fixup_compaq },
	{ PCI_FIXUP_HEADER,	PCI_VENDOR_ID_UMC,	PCI_DEVICE_ID_UMC_UM8886BF,	pci_fixup_umc_ide },
	{ PCI_FIXUP_HEADER,	PCI_VENDOR_ID_SI,	PCI_DEVICE_ID_SI_5513,		pci_fixup_ide_trash },
	{ PCI_FIXUP_HEADER,	PCI_ANY_ID,		PCI_ANY_ID,			pci_fixup_ide_bases },
	{ PCI_FIXUP_HEADER,	PCI_VENDOR_ID_SI,	PCI_DEVICE_ID_SI_5597,		pci_fixup_latency },
	{ PCI_FIXUP_HEADER,	PCI_VENDOR_ID_SI,	PCI_DEVICE_ID_SI_5598,		pci_fixup_latency },
	{ 0 }
};

/*
 *  Called after each bus is probed, but before its children
 *  are examined.
 */

void __init pcibios_fixup_bus(struct pci_bus *b)
{
	pcibios_fixup_ghosts(b);
	pci_read_bridge_bases(b);
}

/*
 * Initialization. Try all known PCI access methods. Note that we support
 * using both PCI BIOS and direct access: in such cases, we use I/O ports
 * to access config space, but we still keep BIOS order of cards to be
 * compatible with 2.0.X. This should go away some day.
 */

void __init pcibios_init(void)
{
	struct pci_ops *bios = NULL;
	struct pci_ops *dir = NULL;

#ifdef CONFIG_PCI_BIOS
	if ((pci_probe & PCI_PROBE_BIOS) && ((bios = pci_find_bios()))) {
		pci_probe |= PCI_BIOS_SORT;
		pci_bios_present = 1;
	}
#endif
#ifdef CONFIG_PCI_DIRECT
	if (pci_probe & (PCI_PROBE_CONF1 | PCI_PROBE_CONF2))
		dir = pci_check_direct();
#endif
	if (dir)
		pci_root_ops = dir;
	else if (bios)
		pci_root_ops = bios;
	else {
		printk("PCI: No PCI bus detected\n");
		return;
	}

	printk("PCI: Probing PCI hardware\n");
	pci_root_bus = pci_scan_bus(0, pci_root_ops, NULL);

	pcibios_irq_init();
	pcibios_fixup_peer_bridges();
	pcibios_fixup_irqs();
	pcibios_resource_survey();

#ifdef CONFIG_PCI_BIOS
	if ((pci_probe & PCI_BIOS_SORT) && !(pci_probe & PCI_NO_SORT))
		pcibios_sort();
#endif
}

char * __init pcibios_setup(char *str)
{
	if (!strcmp(str, "off")) {
		pci_probe = 0;
		return NULL;
	}
#ifdef CONFIG_PCI_BIOS
	else if (!strcmp(str, "bios")) {
		pci_probe = PCI_PROBE_BIOS;
		return NULL;
	} else if (!strcmp(str, "nobios")) {
		pci_probe &= ~PCI_PROBE_BIOS;
		return NULL;
	} else if (!strcmp(str, "nosort")) {
		pci_probe |= PCI_NO_SORT;
		return NULL;
	} else if (!strcmp(str, "biosirq")) {
		pci_probe |= PCI_BIOS_IRQ_SCAN;
		return NULL;
	}
#endif
#ifdef CONFIG_PCI_DIRECT
	else if (!strcmp(str, "conf1")) {
		pci_probe = PCI_PROBE_CONF1 | PCI_NO_CHECKS;
		return NULL;
	}
	else if (!strcmp(str, "conf2")) {
		pci_probe = PCI_PROBE_CONF2 | PCI_NO_CHECKS;
		return NULL;
	}
#endif
	else if (!strcmp(str, "rom")) {
		pci_probe |= PCI_ASSIGN_ROMS;
		return NULL;
	} else if (!strncmp(str, "irqmask=", 8)) {
		pcibios_irq_mask = simple_strtol(str+8, NULL, 0);
		return NULL;
	} else if (!strncmp(str, "lastbus=", 8)) {
		pcibios_last_bus = simple_strtol(str+8, NULL, 0);
		return NULL;
	}
	return str;
}

int pcibios_enable_device(struct pci_dev *dev)
{
	int err;

	if ((err = pcibios_enable_resources(dev)) < 0)
		return err;
	pcibios_enable_irq(dev);
	return 0;
}
