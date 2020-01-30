/*
 *	Intel IO-APIC support for multi-Pentium hosts.
 *
 *	Copyright (C) 1997, 1998, 1999, 2000 Ingo Molnar, Hajnalka Szabo
 *
 *	Many thanks to Stig Venaas for trying out countless experimental
 *	patches and reporting/debugging problems patiently!
 *
 *	(c) 1999, Multiple IO-APIC support, developed by
 *	Ken-ichi Yaku <yaku@css1.kbnes.nec.co.jp> and
 *      Hidemi Kishimoto <kisimoto@css1.kbnes.nec.co.jp>,
 *	further tested and cleaned up by Zach Brown <zab@redhat.com>
 *	and Ingo Molnar <mingo@redhat.com>
 *
 *	Fixes
 *	Maciej W. Rozycki	:	Bits for genuine 82489DX APICs;
 *					thanks to Eric Gilmore
 *					and Rolf G. Tews
 *					for testing these extensively
 *	Paul Diefenbaugh	:	Added full ACPI support
 */

#include <linux/mm.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/config.h>
#include <linux/smp_lock.h>
#include <linux/mc146818rtc.h>
#include <linux/acpi.h>

#include <asm/io.h>
#include <asm/smp.h>
#include <asm/desc.h>
#include <asm/proto.h>

int sis_apic_bug; /* not actually supported, dummy for compile */

#undef APIC_LOCKUP_DEBUG

#define APIC_LOCKUP_DEBUG

static spinlock_t ioapic_lock = SPIN_LOCK_UNLOCKED;

/*
 * # of IRQ routing registers
 */
int nr_ioapic_registers[MAX_IO_APICS];

/*
 * Rough estimation of how many shared IRQs there are, can
 * be changed anytime.
 */
#define MAX_PLUS_SHARED_IRQS NR_IRQS
#define PIN_MAP_SIZE (MAX_PLUS_SHARED_IRQS + NR_IRQS)

/*
 * This is performance-critical, we want to do it O(1)
 *
 * the indexing order of this array favors 1:1 mappings
 * between pins and IRQs.
 */

static struct irq_pin_list {
	short apic, pin, next;
} irq_2_pin[PIN_MAP_SIZE];

int vector_irq[NR_VECTORS] = { [0 ... NR_VECTORS - 1] = -1};
#ifdef CONFIG_PCI_MSI
#define vector_to_irq(vector) 	\
	(platform_legacy_irq(vector) ? vector : vector_irq[vector])
#else
#define vector_to_irq(vector)	(vector)
#endif

/*
 * The common case is 1:1 IRQ<->pin mappings. Sometimes there are
 * shared ISA-space IRQs, so we have to support them. We are super
 * fast in the common case, and fast for shared ISA-space IRQs.
 */
static void __init add_pin_to_irq(unsigned int irq, int apic, int pin)
{
	static int first_free_entry = NR_IRQS;
	struct irq_pin_list *entry = irq_2_pin + irq;

	while (entry->next)
		entry = irq_2_pin + entry->next;

	if (entry->pin != -1) {
		entry->next = first_free_entry;
		entry = irq_2_pin + entry->next;
		if (++first_free_entry >= PIN_MAP_SIZE)
			panic("io_apic.c: whoops");
	}
	entry->apic = apic;
	entry->pin = pin;
}

#define __DO_ACTION(R, ACTION, FINAL)					\
									\
{									\
	int pin;							\
	struct irq_pin_list *entry = irq_2_pin + irq;			\
									\
	for (;;) {							\
		unsigned int reg;					\
		pin = entry->pin;					\
		if (pin == -1)						\
			break;						\
		reg = io_apic_read(entry->apic, 0x10 + R + pin*2);	\
		reg ACTION;						\
		io_apic_modify(entry->apic, reg);			\
		if (!entry->next)					\
			break;						\
		entry = irq_2_pin + entry->next;			\
	}								\
	FINAL;								\
}

#define DO_ACTION(name,R,ACTION, FINAL)					\
									\
	static void name##_IO_APIC_irq (unsigned int irq)		\
	__DO_ACTION(R, ACTION, FINAL)

DO_ACTION( __mask,             0, |= 0x00010000, io_apic_sync(entry->apic) )
						/* mask = 1 */
DO_ACTION( __unmask,           0, &= 0xfffeffff, )
						/* mask = 0 */
DO_ACTION( __mask_and_edge,    0, = (reg & 0xffff7fff) | 0x00010000, )
						/* mask = 1, trigger = 0 */
DO_ACTION( __unmask_and_level, 0, = (reg & 0xfffeffff) | 0x00008000, )
						/* mask = 0, trigger = 1 */

static void mask_IO_APIC_irq (unsigned int irq)
{
	unsigned long flags;

	spin_lock_irqsave(&ioapic_lock, flags);
	__mask_IO_APIC_irq(irq);
	spin_unlock_irqrestore(&ioapic_lock, flags);
}

static void unmask_IO_APIC_irq (unsigned int irq)
{
	unsigned long flags;

	spin_lock_irqsave(&ioapic_lock, flags);
	__unmask_IO_APIC_irq(irq);
	spin_unlock_irqrestore(&ioapic_lock, flags);
}

void clear_IO_APIC_pin(unsigned int apic, unsigned int pin)
{
	struct IO_APIC_route_entry entry;
	unsigned long flags;

	/* Check delivery_mode to be sure we're not clearing an SMI pin */
	spin_lock_irqsave(&ioapic_lock, flags);
	*(((int*)&entry) + 0) = io_apic_read(apic, 0x10 + 2 * pin);
	*(((int*)&entry) + 1) = io_apic_read(apic, 0x11 + 2 * pin);
	spin_unlock_irqrestore(&ioapic_lock, flags);
	if (entry.delivery_mode == dest_SMI)
		return;
	/*
	 * Disable it in the IO-APIC irq-routing table:
	 */
	memset(&entry, 0, sizeof(entry));
	entry.mask = 1;
	spin_lock_irqsave(&ioapic_lock, flags);
	io_apic_write(apic, 0x10 + 2 * pin, *(((int *)&entry) + 0));
	io_apic_write(apic, 0x11 + 2 * pin, *(((int *)&entry) + 1));
	spin_unlock_irqrestore(&ioapic_lock, flags);
}

static void clear_IO_APIC (void)
{
	int apic, pin;

	for (apic = 0; apic < nr_ioapics; apic++)
		for (pin = 0; pin < nr_ioapic_registers[apic]; pin++)
			clear_IO_APIC_pin(apic, pin);
}

/*
 * support for broken MP BIOSs, enables hand-redirection of PIRQ0-7 to
 * specific CPU-side IRQs.
 */

#define MAX_PIRQS 8
int pirq_entries [MAX_PIRQS];
int pirqs_enabled;
int skip_ioapic_setup;
int ioapic_force;

/* dummy parsing: see setup.c */

static int __init disable_ioapic_setup(char *str)
{
	skip_ioapic_setup = 1;
	return 1;
}

static int __init enable_ioapic_setup(char *str)
{
	ioapic_force = 1;
	skip_ioapic_setup = 0;
	return 1;
}

__setup("noapic", disable_ioapic_setup);
__setup("apic", enable_ioapic_setup);

#include <asm/pci-direct.h>
#include <linux/pci_ids.h>
#include <linux/pci.h>

/* Temporary Hack. Nvidia and VIA boards currently only work with IO-APIC
   off. Check for an Nvidia or VIA PCI bridge and turn it off.
   Use pci direct infrastructure because this runs before the PCI subsystem. 

   Can be overwritten with "apic"

   And another hack to disable the IOMMU on VIA chipsets.

   Kludge-O-Rama. */
void __init check_ioapic(void) 
{ 
	int num,slot,func; 
	if (ioapic_force) 
		return; 

	/* Poor man's PCI discovery */
	for (num = 0; num < 32; num++) { 
		for (slot = 0; slot < 32; slot++) { 
			for (func = 0; func < 8; func++) { 
				u32 class;
				u32 vendor;
				u8 type;
				class = read_pci_config(num,slot,func,
							PCI_CLASS_REVISION);
				if (class == 0xffffffff)
					break; 

		       		if ((class >> 16) != PCI_CLASS_BRIDGE_PCI)
					continue; 

				vendor = read_pci_config(num, slot, func, 
							 PCI_VENDOR_ID);
				vendor &= 0xffff;
				switch (vendor) { 
				case PCI_VENDOR_ID_VIA:
#ifdef CONFIG_GART_IOMMU
					if ((end_pfn >= (0xffffffff>>PAGE_SHIFT) ||
					     force_iommu) &&
					    !iommu_aperture_allowed) {
						printk(KERN_INFO
    "Looks like a VIA chipset. Disabling IOMMU. Overwrite with \"iommu=allowed\"\n");
						iommu_aperture_disabled = 1;
					}
#endif
					return;
				case PCI_VENDOR_ID_NVIDIA:
#ifndef CONFIG_SMP
					printk(KERN_INFO 
     "PCI bridge %02x:%02x from %x found. Setting \"noapic\". Overwrite with \"apic\"\n",
					       num,slot,vendor); 
					skip_ioapic_setup = 1;
#endif
					return;
				} 

				/* No multi-function device? */
				type = read_pci_config_byte(num,slot,func,
							    PCI_HEADER_TYPE);
				if (!(type & 0x80))
					break;
			} 
		}
	}
} 

static int __init ioapic_pirq_setup(char *str)
{
	int i, max;
	int ints[MAX_PIRQS+1];

	get_options(str, ARRAY_SIZE(ints), ints);

	for (i = 0; i < MAX_PIRQS; i++)
		pirq_entries[i] = -1;

	pirqs_enabled = 1;
	printk(KERN_INFO "PIRQ redirection, working around broken MP-BIOS.\n");
	max = MAX_PIRQS;
	if (ints[0] < MAX_PIRQS)
		max = ints[0];

	for (i = 0; i < max; i++) {
		printk(KERN_DEBUG "... PIRQ%d -> IRQ %d\n", i, ints[i+1]);
		/*
		 * PIRQs are mapped upside down, usually.
		 */
		pirq_entries[MAX_PIRQS-i-1] = ints[i+1];
	}
	return 1;
}

__setup("pirq=", ioapic_pirq_setup);

/*
 * Find the IRQ entry number of a certain pin.
 */
static int __init find_irq_entry(int apic, int pin, int type)
{
	int i;

	for (i = 0; i < mp_irq_entries; i++)
		if (mp_irqs[i].mpc_irqtype == type &&
		    (mp_irqs[i].mpc_dstapic == mp_ioapics[apic].mpc_apicid ||
		     mp_irqs[i].mpc_dstapic == MP_APIC_ALL) &&
		    mp_irqs[i].mpc_dstirq == pin)
			return i;

	return -1;
}

/*
 * Find the pin to which IRQ[irq] (ISA) is connected
 */
static int __init find_isa_irq_pin(int irq, int type)
{
	int i;

	for (i = 0; i < mp_irq_entries; i++) {
		int lbus = mp_irqs[i].mpc_srcbus;

		if ((mp_bus_id_to_type[lbus] == MP_BUS_ISA ||
		     mp_bus_id_to_type[lbus] == MP_BUS_EISA ||
		     mp_bus_id_to_type[lbus] == MP_BUS_MCA) &&
		    (mp_irqs[i].mpc_irqtype == type) &&
		    (mp_irqs[i].mpc_srcbusirq == irq))

			return mp_irqs[i].mpc_dstirq;
	}
	return -1;
}

/*
 * Find a specific PCI IRQ entry.
 * Not an __init, possibly needed by modules
 */
static int pin_2_irq(int idx, int apic, int pin);

int IO_APIC_get_PCI_irq_vector(int bus, int slot, int pin)
{
	int apic, i, best_guess = -1;

	Dprintk("querying PCI -> IRQ mapping bus:%d, slot:%d, pin:%d.\n",
		bus, slot, pin);
	if (mp_bus_id_to_pci_bus[bus] == -1) {
		printk(KERN_WARNING "PCI BIOS passed nonexistent PCI bus %d!\n", bus);
		return -1;
	}
	for (i = 0; i < mp_irq_entries; i++) {
		int lbus = mp_irqs[i].mpc_srcbus;

		for (apic = 0; apic < nr_ioapics; apic++)
			if (mp_ioapics[apic].mpc_apicid == mp_irqs[i].mpc_dstapic ||
			    mp_irqs[i].mpc_dstapic == MP_APIC_ALL)
				break;

		if ((mp_bus_id_to_type[lbus] == MP_BUS_PCI) &&
		    !mp_irqs[i].mpc_irqtype &&
		    (bus == lbus) &&
		    (slot == ((mp_irqs[i].mpc_srcbusirq >> 2) & 0x1f))) {
			int irq = pin_2_irq(i,apic,mp_irqs[i].mpc_dstirq);

			if (!(apic || IO_APIC_IRQ(irq)))
				continue;

			if (pin == (mp_irqs[i].mpc_srcbusirq & 3))
				return irq;
			/*
			 * Use the first all-but-pin matching entry as a
			 * best-guess fuzzy result for broken mptables.
			 */
			if (best_guess < 0)
				best_guess = irq;
		}
	}
	return best_guess;
}

/*
 * EISA Edge/Level control register, ELCR
 */
static int __init EISA_ELCR(unsigned int irq)
{
	if (irq < 16) {
		unsigned int port = 0x4d0 + (irq >> 3);
		return (inb(port) >> (irq & 7)) & 1;
	}
	printk(KERN_INFO "Broken MPtable reports ISA irq %d\n", irq);
	return 0;
}

/* EISA interrupts are always polarity zero and can be edge or level
 * trigger depending on the ELCR value.  If an interrupt is listed as
 * EISA conforming in the MP table, that means its trigger type must
 * be read in from the ELCR */

#define default_EISA_trigger(idx)	(EISA_ELCR(mp_irqs[idx].mpc_srcbusirq))
#define default_EISA_polarity(idx)	(0)

/* ISA interrupts are always polarity zero edge triggered,
 * when listed as conforming in the MP table. */

#define default_ISA_trigger(idx)	(0)
#define default_ISA_polarity(idx)	(0)

/* PCI interrupts are always polarity one level triggered,
 * when listed as conforming in the MP table. */

#define default_PCI_trigger(idx)	(1)
#define default_PCI_polarity(idx)	(1)

/* MCA interrupts are always polarity zero level triggered,
 * when listed as conforming in the MP table. */

#define default_MCA_trigger(idx)	(1)
#define default_MCA_polarity(idx)	(0)

static int __init MPBIOS_polarity(int idx)
{
	int bus = mp_irqs[idx].mpc_srcbus;
	int polarity;

	/*
	 * Determine IRQ line polarity (high active or low active):
	 */
	switch (mp_irqs[idx].mpc_irqflag & 3)
	{
		case 0: /* conforms, ie. bus-type dependent polarity */
		{
			switch (mp_bus_id_to_type[bus])
			{
				case MP_BUS_ISA: /* ISA pin */
				{
					polarity = default_ISA_polarity(idx);
					break;
				}
				case MP_BUS_EISA: /* EISA pin */
				{
					polarity = default_EISA_polarity(idx);
					break;
				}
				case MP_BUS_PCI: /* PCI pin */
				{
					polarity = default_PCI_polarity(idx);
					break;
				}
				case MP_BUS_MCA: /* MCA pin */
				{
					polarity = default_MCA_polarity(idx);
					break;
				}
				default:
				{
					printk(KERN_WARNING "broken BIOS!!\n");
					polarity = 1;
					break;
				}
			}
			break;
		}
		case 1: /* high active */
		{
			polarity = 0;
			break;
		}
		case 2: /* reserved */
		{
			printk(KERN_WARNING "broken BIOS!!\n");
			polarity = 1;
			break;
		}
		case 3: /* low active */
		{
			polarity = 1;
			break;
		}
		default: /* invalid */
		{
			printk(KERN_WARNING "broken BIOS!!\n");
			polarity = 1;
			break;
		}
	}
	return polarity;
}

static int __init MPBIOS_trigger(int idx)
{
	int bus = mp_irqs[idx].mpc_srcbus;
	int trigger;

	/*
	 * Determine IRQ trigger mode (edge or level sensitive):
	 */
	switch ((mp_irqs[idx].mpc_irqflag>>2) & 3)
	{
		case 0: /* conforms, ie. bus-type dependent */
		{
			switch (mp_bus_id_to_type[bus])
			{
				case MP_BUS_ISA: /* ISA pin */
				{
					trigger = default_ISA_trigger(idx);
					break;
				}
				case MP_BUS_EISA: /* EISA pin */
				{
					trigger = default_EISA_trigger(idx);
					break;
				}
				case MP_BUS_PCI: /* PCI pin */
				{
					trigger = default_PCI_trigger(idx);
					break;
				}
				case MP_BUS_MCA: /* MCA pin */
				{
					trigger = default_MCA_trigger(idx);
					break;
				}
				default:
				{
					printk(KERN_WARNING "broken BIOS!!\n");
					trigger = 1;
					break;
				}
			}
			break;
		}
		case 1: /* edge */
		{
			trigger = 0;
			break;
		}
		case 2: /* reserved */
		{
			printk(KERN_WARNING "broken BIOS!!\n");
			trigger = 1;
			break;
		}
		case 3: /* level */
		{
			trigger = 1;
			break;
		}
		default: /* invalid */
		{
			printk(KERN_WARNING "broken BIOS!!\n");
			trigger = 0;
			break;
		}
	}
	return trigger;
}

static inline int irq_polarity(int idx)
{
	return MPBIOS_polarity(idx);
}

static inline int irq_trigger(int idx)
{
	return MPBIOS_trigger(idx);
}

static int pin_2_irq(int idx, int apic, int pin)
{
	int irq, i;
	int bus = mp_irqs[idx].mpc_srcbus;

	/*
	 * Debugging check, we are in big trouble if this message pops up!
	 */
	if (mp_irqs[idx].mpc_dstirq != pin)
		printk(KERN_ERR "broken BIOS or MPTABLE parser, ayiee!!\n");

	switch (mp_bus_id_to_type[bus])
	{
		case MP_BUS_ISA: /* ISA pin */
		case MP_BUS_EISA:
		case MP_BUS_MCA:
		{
			irq = mp_irqs[idx].mpc_srcbusirq;
			break;
		}
		case MP_BUS_PCI: /* PCI pin */
		{
			/*
			 * PCI IRQs are mapped in order
			 */
			i = irq = 0;
			while (i < apic)
				irq += nr_ioapic_registers[i++];
			irq += pin;
			break;
		}
		default:
		{
			printk(KERN_ERR "unknown bus type %d.\n",bus); 
			irq = 0;
			break;
		}
	}

	/*
	 * PCI IRQ command line redirection. Yes, limits are hardcoded.
	 */
	if ((pin >= 16) && (pin <= 23)) {
		if (pirq_entries[pin-16] != -1) {
			if (!pirq_entries[pin-16]) {
				printk(KERN_DEBUG "disabling PIRQ%d\n", pin-16);
			} else {
				irq = pirq_entries[pin-16];
				printk(KERN_DEBUG "using PIRQ%d -> IRQ %d\n",
						pin-16, irq);
			}
		}
	}
	return irq;
}

static inline int IO_APIC_irq_trigger(int irq)
{
	int apic, idx, pin;

	for (apic = 0; apic < nr_ioapics; apic++) {
		for (pin = 0; pin < nr_ioapic_registers[apic]; pin++) {
			idx = find_irq_entry(apic,pin,mp_INT);
			if ((idx != -1) && (irq == pin_2_irq(idx,apic,pin)))
				return irq_trigger(idx);
		}
	}
	/*
	 * nonexistent IRQs are edge default
	 */
	return 0;
}

/* irq_vectors is indexed by the sum of all RTEs in all I/O APICs. */
u8 irq_vector[NR_IRQ_VECTORS] = { FIRST_DEVICE_VECTOR , 0 };

#ifdef CONFIG_PCI_MSI
int assign_irq_vector(int irq)
#else
int __init assign_irq_vector(int irq)
#endif
{
	static int current_vector = FIRST_DEVICE_VECTOR, offset = 0;

	BUG_ON(irq >= NR_IRQ_VECTORS);
	if (IO_APIC_VECTOR(irq) > 0)
		return IO_APIC_VECTOR(irq);
next:
	current_vector += 8;
	if (current_vector == IA32_SYSCALL_VECTOR)
		goto next;

	if (current_vector >= FIRST_SYSTEM_VECTOR) {
		offset++;
		if (!(offset%8))
			return -ENOSPC;
		current_vector = FIRST_DEVICE_VECTOR + offset;
	}

	vector_irq[current_vector] = irq;
	if (irq != AUTO_ASSIGN)
		IO_APIC_VECTOR(irq) = current_vector;

	return current_vector;
}

extern void (*interrupt[NR_IRQS])(void);
static struct hw_interrupt_type ioapic_level_type;
static struct hw_interrupt_type ioapic_edge_type;

#define IOAPIC_AUTO	-1
#define IOAPIC_EDGE	0
#define IOAPIC_LEVEL	1

static inline void ioapic_register_intr(int irq, int vector, unsigned long trigger)
{
	if (use_pci_vector() && !platform_legacy_irq(irq)) {
		if ((trigger == IOAPIC_AUTO && IO_APIC_irq_trigger(irq)) ||
				trigger == IOAPIC_LEVEL)
			irq_desc[vector].handler = &ioapic_level_type;
		else
			irq_desc[vector].handler = &ioapic_edge_type;
		set_intr_gate(vector, interrupt[vector]);
	} else	{
		if ((trigger == IOAPIC_AUTO && IO_APIC_irq_trigger(irq)) ||
				trigger == IOAPIC_LEVEL)
			irq_desc[irq].handler = &ioapic_level_type;
		else
			irq_desc[irq].handler = &ioapic_edge_type;
		set_intr_gate(vector, interrupt[irq]);
	}
}

void __init setup_IO_APIC_irqs(void)
{
	struct IO_APIC_route_entry entry;
	int apic, pin, idx, irq, first_notcon = 1, vector;
	unsigned long flags;

	printk(KERN_DEBUG "init IO_APIC IRQs\n");

	for (apic = 0; apic < nr_ioapics; apic++) {
	for (pin = 0; pin < nr_ioapic_registers[apic]; pin++) {

		/*
		 * add it to the IO-APIC irq-routing table:
		 */
		memset(&entry,0,sizeof(entry));

		entry.delivery_mode = dest_LowestPrio;
		entry.dest_mode = INT_DELIVERY_MODE;
		entry.mask = 0;				/* enable IRQ */
		entry.dest.logical.logical_dest = TARGET_CPUS;

		idx = find_irq_entry(apic,pin,mp_INT);
		if (idx == -1) {
			if (first_notcon) {
				printk(KERN_DEBUG " IO-APIC (apicid-pin) %d-%d", mp_ioapics[apic].mpc_apicid, pin);
				first_notcon = 0;
			} else
				printk(", %d-%d", mp_ioapics[apic].mpc_apicid, pin);
			continue;
		}

		entry.trigger = irq_trigger(idx);
		entry.polarity = irq_polarity(idx);

		if (irq_trigger(idx)) {
			entry.trigger = 1;
			entry.mask = 1;
			entry.dest.logical.logical_dest = TARGET_CPUS;
		}

		irq = pin_2_irq(idx, apic, pin);
		add_pin_to_irq(irq, apic, pin);

		if (!apic && !IO_APIC_IRQ(irq))
			continue;

		if (IO_APIC_IRQ(irq)) {
			vector = assign_irq_vector(irq);
			entry.vector = vector;

			ioapic_register_intr(irq, vector, IOAPIC_AUTO);
			if (!apic && (irq < 16))
				disable_8259A_irq(irq);
		}
		spin_lock_irqsave(&ioapic_lock, flags);
		io_apic_write(apic, 0x11+2*pin, *(((int *)&entry)+1));
		io_apic_write(apic, 0x10+2*pin, *(((int *)&entry)+0));
		spin_unlock_irqrestore(&ioapic_lock, flags);
	}
	}

	if (!first_notcon)
		printk(" not connected.\n");
}

/*
 * Set up the 8259A-master output pin as broadcast to all
 * CPUs.
 */
void __init setup_ExtINT_IRQ0_pin(unsigned int pin, int vector)
{
	struct IO_APIC_route_entry entry;
	unsigned long flags;

	memset(&entry,0,sizeof(entry));

	disable_8259A_irq(0);

	/* mask LVT0 */
	apic_write_around(APIC_LVT0, APIC_LVT_MASKED | APIC_DM_EXTINT);

	/*
	 * We use logical delivery to get the timer IRQ
	 * to the first CPU.
	 */
	entry.dest_mode = INT_DELIVERY_MODE;
	entry.mask = 0;					/* unmask IRQ now */
	entry.dest.logical.logical_dest = TARGET_CPUS;
	entry.delivery_mode = dest_LowestPrio;
	entry.polarity = 0;
	entry.trigger = 0;
	entry.vector = vector;

	/*
	 * The timer IRQ doesn't have to know that behind the
	 * scene we have a 8259A-master in AEOI mode ...
	 */
	irq_desc[0].handler = &ioapic_edge_type;

	/*
	 * Add it to the IO-APIC irq-routing table:
	 */
	spin_lock_irqsave(&ioapic_lock, flags);
	io_apic_write(0, 0x11+2*pin, *(((int *)&entry)+1));
	io_apic_write(0, 0x10+2*pin, *(((int *)&entry)+0));
	spin_unlock_irqrestore(&ioapic_lock, flags);

	enable_8259A_irq(0);
}

void __init UNEXPECTED_IO_APIC(void)
{
#if 0
	printk(KERN_WARNING " WARNING: unexpected IO-APIC, please mail\n");
	printk(KERN_WARNING "          to linux-smp@vger.kernel.org\n");
#endif
}

void __init print_IO_APIC(void)
{
	int apic, i;
	union IO_APIC_reg_00 reg_00;
	union IO_APIC_reg_01 reg_01;
	union IO_APIC_reg_02 reg_02;
	unsigned long flags;

 	printk(KERN_DEBUG "number of MP IRQ sources: %d.\n", mp_irq_entries);
	for (i = 0; i < nr_ioapics; i++)
		printk(KERN_DEBUG "number of IO-APIC #%d registers: %d.\n",
		       mp_ioapics[i].mpc_apicid, nr_ioapic_registers[i]);

	/*
	 * We are a bit conservative about what we expect.  We have to
	 * know about every hardware change ASAP.
	 */
	printk(KERN_INFO "testing the IO APIC.......................\n");

	for (apic = 0; apic < nr_ioapics; apic++) {

	spin_lock_irqsave(&ioapic_lock, flags);
	reg_00.raw = io_apic_read(apic, 0);
	reg_01.raw = io_apic_read(apic, 1);
	if (reg_01.bits.version >= 0x10)
		reg_02.raw = io_apic_read(apic, 2);
	spin_unlock_irqrestore(&ioapic_lock, flags);

	printk("\n");
	printk(KERN_DEBUG "IO APIC #%d......\n", mp_ioapics[apic].mpc_apicid);
	printk(KERN_DEBUG ".... register #00: %08X\n", reg_00.raw);
	printk(KERN_DEBUG ".......    : physical APIC id: %02X\n", reg_00.bits.ID);
	if (reg_00.bits.__reserved_1 || reg_00.bits.__reserved_2)
		UNEXPECTED_IO_APIC();

	printk(KERN_DEBUG ".... register #01: %08X\n", *(int *)&reg_01);
	printk(KERN_DEBUG ".......     : max redirection entries: %04X\n", reg_01.bits.entries);
	if (	(reg_01.bits.entries != 0x0f) && /* older (Neptune) boards */
		(reg_01.bits.entries != 0x17) && /* typical ISA+PCI boards */
		(reg_01.bits.entries != 0x1b) && /* Compaq Proliant boards */
		(reg_01.bits.entries != 0x1f) && /* dual Xeon boards */
		(reg_01.bits.entries != 0x22) && /* bigger Xeon boards */
		(reg_01.bits.entries != 0x2E) &&
		(reg_01.bits.entries != 0x3F) &&
		(reg_01.bits.entries != 0x03) 
	)
		UNEXPECTED_IO_APIC();

	printk(KERN_DEBUG ".......     : PRQ implemented: %X\n", reg_01.bits.PRQ);
	printk(KERN_DEBUG ".......     : IO APIC version: %04X\n", reg_01.bits.version);
	if (	(reg_01.bits.version != 0x01) && /* 82489DX IO-APICs */
		(reg_01.bits.version != 0x02) && /* 82801BA IO-APICs (ICH2) */
		(reg_01.bits.version != 0x10) && /* oldest IO-APICs */
		(reg_01.bits.version != 0x11) && /* Pentium/Pro IO-APICs */
		(reg_01.bits.version != 0x13) && /* Xeon IO-APICs */
		(reg_01.bits.version != 0x20)    /* Intel P64H (82806 AA) */
	)
		UNEXPECTED_IO_APIC();
	if (reg_01.bits.__reserved_1 || reg_01.bits.__reserved_2)
		UNEXPECTED_IO_APIC();

	if (reg_01.bits.version >= 0x10) {
		printk(KERN_DEBUG ".... register #02: %08X\n", reg_02.raw);
		printk(KERN_DEBUG ".......     : arbitration: %02X\n", reg_02.bits.arbitration);
		if (reg_02.bits.__reserved_1 || reg_02.bits.__reserved_2)
			UNEXPECTED_IO_APIC();
	}

	printk(KERN_DEBUG ".... IRQ redirection table:\n");

	printk(KERN_DEBUG " NR Log Phy Mask Trig IRR Pol"
			  " Stat Dest Deli Vect:   \n");

	for (i = 0; i <= reg_01.bits.entries; i++) {
		struct IO_APIC_route_entry entry;

		spin_lock_irqsave(&ioapic_lock, flags);
		*(((int *)&entry)+0) = io_apic_read(apic, 0x10+i*2);
		*(((int *)&entry)+1) = io_apic_read(apic, 0x11+i*2);
		spin_unlock_irqrestore(&ioapic_lock, flags);

		printk(KERN_DEBUG " %02x %03X %02X  ",
			i,
			entry.dest.logical.logical_dest,
			entry.dest.physical.physical_dest
		);

		printk("%1d    %1d    %1d   %1d   %1d    %1d    %1d    %02X\n",
			entry.mask,
			entry.trigger,
			entry.irr,
			entry.polarity,
			entry.delivery_status,
			entry.dest_mode,
			entry.delivery_mode,
			entry.vector
		);
	}
	}
	if (use_pci_vector())
		printk(KERN_INFO "Using vector-based indexing\n");
	printk(KERN_DEBUG "IRQ to pin mappings:\n");
	for (i = 0; i < NR_IRQS; i++) {
		struct irq_pin_list *entry = irq_2_pin + i;
		if (entry->pin < 0)
			continue;
 		if (use_pci_vector() && !platform_legacy_irq(i))
			printk(KERN_DEBUG "IRQ%d ", IO_APIC_VECTOR(i));
		else
			printk(KERN_DEBUG "IRQ%d ", i);
		for (;;) {
			printk("-> %d:%d", entry->apic, entry->pin);
			if (!entry->next)
				break;
			entry = irq_2_pin + entry->next;
		}
		printk("\n");
	}

	printk(KERN_INFO ".................................... done.\n");

	return;
}

static void print_APIC_bitfield (int base)
{
	unsigned int v;
	int i, j;

	printk(KERN_DEBUG "0123456789abcdef0123456789abcdef\n" KERN_DEBUG);
	for (i = 0; i < 8; i++) {
		v = apic_read(base + i*0x10);
		for (j = 0; j < 32; j++) {
			if (v & (1<<j))
				printk("1");
			else
				printk("0");
		}
		printk("\n");
	}
}

void /*__init*/ print_local_APIC(void * dummy)
{
	unsigned int v, ver, maxlvt;

	printk("\n" KERN_DEBUG "printing local APIC contents on CPU#%d/%d:\n",
		smp_processor_id(), hard_smp_processor_id());
	v = apic_read(APIC_ID);
	printk(KERN_INFO "... APIC ID:      %08x (%01x)\n", v, GET_APIC_ID(v));
	v = apic_read(APIC_LVR);
	printk(KERN_INFO "... APIC VERSION: %08x\n", v);
	ver = GET_APIC_VERSION(v);
	maxlvt = get_maxlvt();

	v = apic_read(APIC_TASKPRI);
	printk(KERN_DEBUG "... APIC TASKPRI: %08x (%02x)\n", v, v & APIC_TPRI_MASK);

	if (APIC_INTEGRATED(ver)) {			/* !82489DX */
		v = apic_read(APIC_ARBPRI);
		printk(KERN_DEBUG "... APIC ARBPRI: %08x (%02x)\n", v,
			v & APIC_ARBPRI_MASK);
		v = apic_read(APIC_PROCPRI);
		printk(KERN_DEBUG "... APIC PROCPRI: %08x\n", v);
	}

	v = apic_read(APIC_EOI);
	printk(KERN_DEBUG "... APIC EOI: %08x\n", v);
	v = apic_read(APIC_RRR);
	printk(KERN_DEBUG "... APIC RRR: %08x\n", v);
	v = apic_read(APIC_LDR);
	printk(KERN_DEBUG "... APIC LDR: %08x\n", v);
	v = apic_read(APIC_DFR);
	printk(KERN_DEBUG "... APIC DFR: %08x\n", v);
	v = apic_read(APIC_SPIV);
	printk(KERN_DEBUG "... APIC SPIV: %08x\n", v);

	printk(KERN_DEBUG "... APIC ISR field:\n");
	print_APIC_bitfield(APIC_ISR);
	printk(KERN_DEBUG "... APIC TMR field:\n");
	print_APIC_bitfield(APIC_TMR);
	printk(KERN_DEBUG "... APIC IRR field:\n");
	print_APIC_bitfield(APIC_IRR);

	if (APIC_INTEGRATED(ver)) {		/* !82489DX */
		if (maxlvt > 3)		/* Due to the Pentium erratum 3AP. */
			apic_write(APIC_ESR, 0);
		v = apic_read(APIC_ESR);
		printk(KERN_DEBUG "... APIC ESR: %08x\n", v);
	}

	v = apic_read(APIC_ICR);
	printk(KERN_DEBUG "... APIC ICR: %08x\n", v);
	v = apic_read(APIC_ICR2);
	printk(KERN_DEBUG "... APIC ICR2: %08x\n", v);

	v = apic_read(APIC_LVTT);
	printk(KERN_DEBUG "... APIC LVTT: %08x\n", v);

	if (maxlvt > 3) {                       /* PC is LVT#4. */
		v = apic_read(APIC_LVTPC);
		printk(KERN_DEBUG "... APIC LVTPC: %08x\n", v);
	}
	v = apic_read(APIC_LVT0);
	printk(KERN_DEBUG "... APIC LVT0: %08x\n", v);
	v = apic_read(APIC_LVT1);
	printk(KERN_DEBUG "... APIC LVT1: %08x\n", v);

	if (maxlvt > 2) {			/* ERR is LVT#3. */
		v = apic_read(APIC_LVTERR);
		printk(KERN_DEBUG "... APIC LVTERR: %08x\n", v);
	}

	v = apic_read(APIC_TMICT);
	printk(KERN_DEBUG "... APIC TMICT: %08x\n", v);
	v = apic_read(APIC_TMCCT);
	printk(KERN_DEBUG "... APIC TMCCT: %08x\n", v);
	v = apic_read(APIC_TDCR);
	printk(KERN_DEBUG "... APIC TDCR: %08x\n", v);
	printk("\n");
}

void print_all_local_APICs (void)
{
	on_each_cpu(print_local_APIC, NULL, 1, 1);
}

void /*__init*/ print_PIC(void)
{
	extern spinlock_t i8259A_lock;
	unsigned int v;
	unsigned long flags;

	printk(KERN_DEBUG "\nprinting PIC contents\n");

	spin_lock_irqsave(&i8259A_lock, flags);

	v = inb(0xa1) << 8 | inb(0x21);
	printk(KERN_DEBUG "... PIC  IMR: %04x\n", v);

	v = inb(0xa0) << 8 | inb(0x20);
	printk(KERN_DEBUG "... PIC  IRR: %04x\n", v);

	outb(0x0b,0xa0);
	outb(0x0b,0x20);
	v = inb(0xa0) << 8 | inb(0x20);
	outb(0x0a,0xa0);
	outb(0x0a,0x20);

	spin_unlock_irqrestore(&i8259A_lock, flags);

	printk(KERN_DEBUG "... PIC  ISR: %04x\n", v);

	v = inb(0x4d1) << 8 | inb(0x4d0);
	printk(KERN_DEBUG "... PIC ELCR: %04x\n", v);
}

static void __init enable_IO_APIC(void)
{
	union IO_APIC_reg_01 reg_01;
	int i;
	unsigned long flags;

	for (i = 0; i < PIN_MAP_SIZE; i++) {
		irq_2_pin[i].pin = -1;
		irq_2_pin[i].next = 0;
	}
	if (!pirqs_enabled)
		for (i = 0; i < MAX_PIRQS; i++)
			pirq_entries[i] = -1;

	/*
	 * The number of IO-APIC IRQ registers (== #pins):
	 */
	for (i = 0; i < nr_ioapics; i++) {
		spin_lock_irqsave(&ioapic_lock, flags);
		reg_01.raw = io_apic_read(i, 1);
		spin_unlock_irqrestore(&ioapic_lock, flags);
		nr_ioapic_registers[i] = reg_01.bits.entries+1;
	}

	/*
	 * Do not trust the IO-APIC being empty at bootup
	 */
	clear_IO_APIC();
}

/*
 * Not an __init, needed by the reboot code
 */
void disable_IO_APIC(void)
{
	/*
	 * Clear the IO-APIC before rebooting:
	 */
	clear_IO_APIC();

	disconnect_bsp_APIC();
}

/*
 * function to set the IO-APIC physical IDs based on the
 * values stored in the MPC table.
 *
 * by Matt Domsch <Matt_Domsch@dell.com>  Tue Dec 21 12:25:05 CST 1999
 */

static void __init setup_ioapic_ids_from_mpc (void)
{
	union IO_APIC_reg_00 reg_00;
	physid_mask_t phys_id_present_map = phys_cpu_present_map;
	int apic;
	int i;
	unsigned char old_id;
	unsigned long flags;

	/*
	 * Set the IOAPIC ID to the value stored in the MPC table.
	 */
	for (apic = 0; apic < nr_ioapics; apic++) {

		/* Read the register 0 value */
		spin_lock_irqsave(&ioapic_lock, flags);
		reg_00.raw = io_apic_read(apic, 0);
		spin_unlock_irqrestore(&ioapic_lock, flags);
		
		old_id = mp_ioapics[apic].mpc_apicid;

		if (mp_ioapics[apic].mpc_apicid >= 0xf) {
			printk(KERN_ERR "BIOS bug, IO-APIC#%d ID is %d in the MPC table!...\n",
				apic, mp_ioapics[apic].mpc_apicid);
			printk(KERN_ERR "... fixing up to %d. (tell your hw vendor)\n",
				reg_00.bits.ID);
			mp_ioapics[apic].mpc_apicid = reg_00.bits.ID;
		}

		/*
		 * Sanity check, is the ID really free? Every APIC in a
		 * system must have a unique ID or we get lots of nice
		 * 'stuck on smp_invalidate_needed IPI wait' messages.
	 	 */
		if (physid_isset(mp_ioapics[apic].mpc_apicid, phys_id_present_map)) {
			printk(KERN_ERR "BIOS bug, IO-APIC#%d ID %d is already used!...\n",
				apic, mp_ioapics[apic].mpc_apicid);
			for (i = 0; i < 0xf; i++)
				if (!physid_isset(i, phys_id_present_map))
					break;
			if (i >= 0xf)
				panic("Max APIC ID exceeded!\n");
			printk(KERN_ERR "... fixing up to %d. (tell your hw vendor)\n",
				i);
			physid_set(i, phys_id_present_map);
			mp_ioapics[apic].mpc_apicid = i;
		} else {
			printk(KERN_INFO 
			       "Using IO-APIC %d\n", mp_ioapics[apic].mpc_apicid);
			physid_set(mp_ioapics[apic].mpc_apicid, phys_id_present_map);
		}


		/*
		 * We need to adjust the IRQ routing table
		 * if the ID changed.
		 */
		if (old_id != mp_ioapics[apic].mpc_apicid)
			for (i = 0; i < mp_irq_entries; i++)
				if (mp_irqs[i].mpc_dstapic == old_id)
					mp_irqs[i].mpc_dstapic
						= mp_ioapics[apic].mpc_apicid;

		/*
		 * Read the right value from the MPC table and
		 * write it into the ID register.
	 	 */
		printk(KERN_INFO "...changing IO-APIC physical APIC ID to %d ...",
				mp_ioapics[apic].mpc_apicid);

		reg_00.bits.ID = mp_ioapics[apic].mpc_apicid;
		spin_lock_irqsave(&ioapic_lock, flags);
		io_apic_write(apic, 0, reg_00.raw);
		spin_unlock_irqrestore(&ioapic_lock, flags);

		/*
		 * Sanity check
		 */
		spin_lock_irqsave(&ioapic_lock, flags);
		reg_00.raw = io_apic_read(apic, 0);
		spin_unlock_irqrestore(&ioapic_lock, flags);
		if (reg_00.bits.ID != mp_ioapics[apic].mpc_apicid)
			panic("could not set ID!\n");
		else
			printk(" ok.\n");
	}
}

/*
 * There is a nasty bug in some older SMP boards, their mptable lies
 * about the timer IRQ. We do the following to work around the situation:
 *
 *	- timer IRQ defaults to IO-APIC IRQ
 *	- if this function detects that timer IRQs are defunct, then we fall
 *	  back to ISA timer IRQs
 */
static int __init timer_irq_works(void)
{
	unsigned long t1 = jiffies;

	local_irq_enable();
	/* Let ten ticks pass... */
	mdelay((10 * 1000) / HZ);

	/*
	 * Expect a few ticks at least, to be sure some possible
	 * glue logic does not lock up after one or two first
	 * ticks in a non-ExtINT mode.  Also the local APIC
	 * might have cached one ExtINT interrupt.  Finally, at
	 * least one tick may be lost due to delays.
	 */

	/* jiffies wrap? */
	if (jiffies - t1 > 4)
		return 1;
	return 0;
}

/*
 * In the SMP+IOAPIC case it might happen that there are an unspecified
 * number of pending IRQ events unhandled. These cases are very rare,
 * so we 'resend' these IRQs via IPIs, to the same CPU. It's much
 * better to do it this way as thus we do not have to be aware of
 * 'pending' interrupts in the IRQ path, except at this point.
 */
/*
 * Edge triggered needs to resend any interrupt
 * that was delayed but this is now handled in the device
 * independent code.
 */

/*
 * Starting up a edge-triggered IO-APIC interrupt is
 * nasty - we need to make sure that we get the edge.
 * If it is already asserted for some reason, we need
 * return 1 to indicate that is was pending.
 *
 * This is not complete - we should be able to fake
 * an edge even if it isn't on the 8259A...
 */

static unsigned int startup_edge_ioapic_irq(unsigned int irq)
{
	int was_pending = 0;
	unsigned long flags;

	spin_lock_irqsave(&ioapic_lock, flags);
	if (irq < 16) {
		disable_8259A_irq(irq);
		if (i8259A_irq_pending(irq))
			was_pending = 1;
	}
	__unmask_IO_APIC_irq(irq);
	spin_unlock_irqrestore(&ioapic_lock, flags);

	return was_pending;
}

/*
 * Once we have recorded IRQ_PENDING already, we can mask the
 * interrupt for real. This prevents IRQ storms from unhandled
 * devices.
 */
static void ack_edge_ioapic_irq(unsigned int irq)
{
	if ((irq_desc[irq].status & (IRQ_PENDING | IRQ_DISABLED))
					== (IRQ_PENDING | IRQ_DISABLED))
		mask_IO_APIC_irq(irq);
	ack_APIC_irq();
}

/*
 * Level triggered interrupts can just be masked,
 * and shutting down and starting up the interrupt
 * is the same as enabling and disabling them -- except
 * with a startup need to return a "was pending" value.
 *
 * Level triggered interrupts are special because we
 * do not touch any IO-APIC register while handling
 * them. We ack the APIC in the end-IRQ handler, not
 * in the start-IRQ-handler. Protection against reentrance
 * from the same interrupt is still provided, both by the
 * generic IRQ layer and by the fact that an unacked local
 * APIC does not accept IRQs.
 */
static unsigned int startup_level_ioapic_irq (unsigned int irq)
{
	unmask_IO_APIC_irq(irq);

	return 0; /* don't check for pending */
}

static void end_level_ioapic_irq (unsigned int irq)
{
	unsigned long v;
	int i;

/*
 * It appears there is an erratum which affects at least version 0x11
 * of I/O APIC (that's the 82093AA and cores integrated into various
 * chipsets).  Under certain conditions a level-triggered interrupt is
 * erroneously delivered as edge-triggered one but the respective IRR
 * bit gets set nevertheless.  As a result the I/O unit expects an EOI
 * message but it will never arrive and further interrupts are blocked
 * from the source.  The exact reason is so far unknown, but the
 * phenomenon was observed when two consecutive interrupt requests
 * from a given source get delivered to the same CPU and the source is
 * temporarily disabled in between.
 *
 * A workaround is to simulate an EOI message manually.  We achieve it
 * by setting the trigger mode to edge and then to level when the edge
 * trigger mode gets detected in the TMR of a local APIC for a
 * level-triggered interrupt.  We mask the source for the time of the
 * operation to prevent an edge-triggered interrupt escaping meanwhile.
 * The idea is from Manfred Spraul.  --macro
 */
	i = IO_APIC_VECTOR(irq);
	v = apic_read(APIC_TMR + ((i & ~0x1f) >> 1));

	ack_APIC_irq();

	if (!(v & (1 << (i & 0x1f)))) {
#ifdef APIC_LOCKUP_DEBUG
		struct irq_pin_list *entry;
#endif

#ifdef APIC_MISMATCH_DEBUG
		atomic_inc(&irq_mis_count);
#endif
		spin_lock(&ioapic_lock);
		__mask_and_edge_IO_APIC_irq(irq);
#ifdef APIC_LOCKUP_DEBUG
		for (entry = irq_2_pin + irq;;) {
			unsigned int reg;

			if (entry->pin == -1)
				break;
			reg = io_apic_read(entry->apic, 0x10 + entry->pin * 2);
			if (reg & 0x00004000)
				printk(KERN_CRIT "Aieee!!!  Remote IRR"
					" still set after unlock!\n");
			if (!entry->next)
				break;
			entry = irq_2_pin + entry->next;
		}
#endif
		__unmask_and_level_IO_APIC_irq(irq);
		spin_unlock(&ioapic_lock);
	}
}

static void set_ioapic_affinity_irq(unsigned int irq, cpumask_t mask)
{
	unsigned long flags;
	unsigned int dest;

	dest = cpu_mask_to_apicid(mask);

	/*
	 * Only the first 8 bits are valid.
	 */
	dest = dest << 24;

	spin_lock_irqsave(&ioapic_lock, flags);
	__DO_ACTION(1, = dest, )
	spin_unlock_irqrestore(&ioapic_lock, flags);
}

#ifdef CONFIG_PCI_MSI
static unsigned int startup_edge_ioapic_vector(unsigned int vector)
{
	int irq = vector_to_irq(vector);

	return startup_edge_ioapic_irq(irq);
}

static void ack_edge_ioapic_vector(unsigned int vector)
{
	int irq = vector_to_irq(vector);

	ack_edge_ioapic_irq(irq);
}

static unsigned int startup_level_ioapic_vector (unsigned int vector)
{
	int irq = vector_to_irq(vector);

	return startup_level_ioapic_irq (irq);
}

static void end_level_ioapic_vector (unsigned int vector)
{
	int irq = vector_to_irq(vector);

	end_level_ioapic_irq(irq);
}

static void mask_IO_APIC_vector (unsigned int vector)
{
	int irq = vector_to_irq(vector);

	mask_IO_APIC_irq(irq);
}

static void unmask_IO_APIC_vector (unsigned int vector)
{
	int irq = vector_to_irq(vector);

	unmask_IO_APIC_irq(irq);
}

static void set_ioapic_affinity_vector (unsigned int vector,
					cpumask_t cpu_mask)
{
	int irq = vector_to_irq(vector);

	set_ioapic_affinity_irq(irq, cpu_mask);
}
#endif

/*
 * Level and edge triggered IO-APIC interrupts need different handling,
 * so we use two separate IRQ descriptors. Edge triggered IRQs can be
 * handled with the level-triggered descriptor, but that one has slightly
 * more overhead. Level-triggered interrupts cannot be handled with the
 * edge-triggered handler, without risking IRQ storms and other ugly
 * races.
 */

static struct hw_interrupt_type ioapic_edge_type = {
	.typename = "IO-APIC-edge",
	.startup 	= startup_edge_ioapic,
	.shutdown 	= shutdown_edge_ioapic,
	.enable 	= enable_edge_ioapic,
	.disable 	= disable_edge_ioapic,
	.ack 		= ack_edge_ioapic,
	.end 		= end_edge_ioapic,
	.set_affinity = set_ioapic_affinity,
};

static struct hw_interrupt_type ioapic_level_type = {
	.typename = "IO-APIC-level",
	.startup 	= startup_level_ioapic,
	.shutdown 	= shutdown_level_ioapic,
	.enable 	= enable_level_ioapic,
	.disable 	= disable_level_ioapic,
	.ack 		= mask_and_ack_level_ioapic,
	.end 		= end_level_ioapic,
	.set_affinity = set_ioapic_affinity,
};

static inline void init_IO_APIC_traps(void)
{
	int irq;

	/*
	 * NOTE! The local APIC isn't very good at handling
	 * multiple interrupts at the same interrupt level.
	 * As the interrupt level is determined by taking the
	 * vector number and shifting that right by 4, we
	 * want to spread these out a bit so that they don't
	 * all fall in the same interrupt level.
	 *
	 * Also, we've got to be careful not to trash gate
	 * 0x80, because int 0x80 is hm, kind of importantish. ;)
	 */
	for (irq = 0; irq < NR_IRQS ; irq++) {
		int tmp = irq;
		if (use_pci_vector()) {
			if (!platform_legacy_irq(tmp))
				if ((tmp = vector_to_irq(tmp)) == -1)
					continue;
		}
		if (IO_APIC_IRQ(tmp) && !IO_APIC_VECTOR(tmp)) {
			/*
			 * Hmm.. We don't have an entry for this,
			 * so default to an old-fashioned 8259
			 * interrupt if we can..
			 */
			if (irq < 16)
				make_8259A_irq(irq);
			else
				/* Strange. Oh, well.. */
				irq_desc[irq].handler = &no_irq_type;
		}
	}
}

static void enable_lapic_irq (unsigned int irq)
{
	unsigned long v;

	v = apic_read(APIC_LVT0);
	apic_write_around(APIC_LVT0, v & ~APIC_LVT_MASKED);
}

static void disable_lapic_irq (unsigned int irq)
{
	unsigned long v;

	v = apic_read(APIC_LVT0);
	apic_write_around(APIC_LVT0, v | APIC_LVT_MASKED);
}

static void ack_lapic_irq (unsigned int irq)
{
	ack_APIC_irq();
}

static void end_lapic_irq (unsigned int i) { /* nothing */ }

static struct hw_interrupt_type lapic_irq_type = {
	.typename = "local-APIC-edge",
	.startup = NULL, /* startup_irq() not used for IRQ0 */
	.shutdown = NULL, /* shutdown_irq() not used for IRQ0 */
	.enable = enable_lapic_irq,
	.disable = disable_lapic_irq,
	.ack = ack_lapic_irq,
	.end = end_lapic_irq,
};

static void setup_nmi (void)
{
	/*
 	 * Dirty trick to enable the NMI watchdog ...
	 * We put the 8259A master into AEOI mode and
	 * unmask on all local APICs LVT0 as NMI.
	 *
	 * The idea to use the 8259A in AEOI mode ('8259A Virtual Wire')
	 * is from Maciej W. Rozycki - so we do not have to EOI from
	 * the NMI handler or the timer interrupt.
	 */ 
	printk(KERN_INFO "activating NMI Watchdog ...");

	enable_NMI_through_LVT0(NULL);

	printk(" done.\n");
}

/*
 * This looks a bit hackish but it's about the only one way of sending
 * a few INTA cycles to 8259As and any associated glue logic.  ICR does
 * not support the ExtINT mode, unfortunately.  We need to send these
 * cycles as some i82489DX-based boards have glue logic that keeps the
 * 8259A interrupt line asserted until INTA.  --macro
 */
static inline void unlock_ExtINT_logic(void)
{
	int pin, i;
	struct IO_APIC_route_entry entry0, entry1;
	unsigned char save_control, save_freq_select;
	unsigned long flags;

	pin = find_isa_irq_pin(8, mp_INT);
	if (pin == -1)
		return;

	spin_lock_irqsave(&ioapic_lock, flags);
	*(((int *)&entry0) + 1) = io_apic_read(0, 0x11 + 2 * pin);
	*(((int *)&entry0) + 0) = io_apic_read(0, 0x10 + 2 * pin);
	spin_unlock_irqrestore(&ioapic_lock, flags);
	clear_IO_APIC_pin(0, pin);

	memset(&entry1, 0, sizeof(entry1));

	entry1.dest_mode = 0;			/* physical delivery */
	entry1.mask = 0;			/* unmask IRQ now */
	entry1.dest.physical.physical_dest = hard_smp_processor_id();
	entry1.delivery_mode = dest_ExtINT;
	entry1.polarity = entry0.polarity;
	entry1.trigger = 0;
	entry1.vector = 0;

	spin_lock_irqsave(&ioapic_lock, flags);
	io_apic_write(0, 0x11 + 2 * pin, *(((int *)&entry1) + 1));
	io_apic_write(0, 0x10 + 2 * pin, *(((int *)&entry1) + 0));
	spin_unlock_irqrestore(&ioapic_lock, flags);

	save_control = CMOS_READ(RTC_CONTROL);
	save_freq_select = CMOS_READ(RTC_FREQ_SELECT);
	CMOS_WRITE((save_freq_select & ~RTC_RATE_SELECT) | 0x6,
		   RTC_FREQ_SELECT);
	CMOS_WRITE(save_control | RTC_PIE, RTC_CONTROL);

	i = 100;
	while (i-- > 0) {
		mdelay(10);
		if ((CMOS_READ(RTC_INTR_FLAGS) & RTC_PF) == RTC_PF)
			i -= 10;
	}

	CMOS_WRITE(save_control, RTC_CONTROL);
	CMOS_WRITE(save_freq_select, RTC_FREQ_SELECT);
	clear_IO_APIC_pin(0, pin);

	spin_lock_irqsave(&ioapic_lock, flags);
	io_apic_write(0, 0x11 + 2 * pin, *(((int *)&entry0) + 1));
	io_apic_write(0, 0x10 + 2 * pin, *(((int *)&entry0) + 0));
	spin_unlock_irqrestore(&ioapic_lock, flags);
}

/*
 * This code may look a bit paranoid, but it's supposed to cooperate with
 * a wide range of boards and BIOS bugs.  Fortunately only the timer IRQ
 * is so screwy.  Thanks to Brian Perkins for testing/hacking this beast
 * fanatically on his truly buggy board.
 */
static inline void check_timer(void)
{
	int pin1, pin2;
	int vector;

	/*
	 * get/set the timer IRQ vector:
	 */
	disable_8259A_irq(0);
	vector = assign_irq_vector(0);
	set_intr_gate(vector, interrupt[0]);

	/*
	 * Subtle, code in do_timer_interrupt() expects an AEOI
	 * mode for the 8259A whenever interrupts are routed
	 * through I/O APICs.  Also IRQ0 has to be enabled in
	 * the 8259A which implies the virtual wire has to be
	 * disabled in the local APIC.
	 */
	apic_write_around(APIC_LVT0, APIC_LVT_MASKED | APIC_DM_EXTINT);
	init_8259A(1);
	enable_8259A_irq(0);

	pin1 = find_isa_irq_pin(0, mp_INT);
	pin2 = find_isa_irq_pin(0, mp_ExtINT);

	printk(KERN_INFO "..TIMER: vector=0x%02X pin1=%d pin2=%d\n", vector, pin1, pin2);

	if (pin1 != -1) {
		/*
		 * Ok, does IRQ0 through the IOAPIC work?
		 */
		unmask_IO_APIC_irq(0);
		if (timer_irq_works()) {
			nmi_watchdog_default();
			if (nmi_watchdog == NMI_IO_APIC) {
				disable_8259A_irq(0);
				setup_nmi();
				enable_8259A_irq(0);
				check_nmi_watchdog();
			}
			return;
		}
		clear_IO_APIC_pin(0, pin1);
		printk(KERN_ERR "..MP-BIOS bug: 8254 timer not connected to IO-APIC\n");
	}

	printk(KERN_INFO "...trying to set up timer (IRQ0) through the 8259A ... ");
	if (pin2 != -1) {
		printk("\n..... (found pin %d) ...", pin2);
		/*
		 * legacy devices should be connected to IO APIC #0
		 */
		setup_ExtINT_IRQ0_pin(pin2, vector);
		if (timer_irq_works()) {
			printk("works.\n");
			nmi_watchdog_default();
			if (nmi_watchdog == NMI_IO_APIC) {
				setup_nmi();
				check_nmi_watchdog();
			}
			return;
		}
		/*
		 * Cleanup, just in case ...
		 */
		clear_IO_APIC_pin(0, pin2);
	}
	printk(" failed.\n");

	if (nmi_watchdog) {
		printk(KERN_WARNING "timer doesn't work through the IO-APIC - disabling NMI Watchdog!\n");
		nmi_watchdog = 0;
	}

	printk(KERN_INFO "...trying to set up timer as Virtual Wire IRQ...");

	disable_8259A_irq(0);
	irq_desc[0].handler = &lapic_irq_type;
	apic_write_around(APIC_LVT0, APIC_DM_FIXED | vector);	/* Fixed mode */
	enable_8259A_irq(0);

	if (timer_irq_works()) {
		printk(" works.\n");
		return;
	}
	apic_write_around(APIC_LVT0, APIC_LVT_MASKED | APIC_DM_FIXED | vector);
	printk(" failed.\n");

	printk(KERN_INFO "...trying to set up timer as ExtINT IRQ...");

	init_8259A(0);
	make_8259A_irq(0);
	apic_write_around(APIC_LVT0, APIC_DM_EXTINT);

	unlock_ExtINT_logic();

	if (timer_irq_works()) {
		printk(" works.\n");
		return;
	}
	printk(" failed :(.\n");
	panic("IO-APIC + timer doesn't work! Try using the 'noapic' kernel parameter\n");
}

/*
 *
 * IRQ's that are handled by the PIC in the MPS IOAPIC case.
 * - IRQ2 is the cascade IRQ, and cannot be a io-apic IRQ.
 *   Linux doesn't really care, as it's not actually used
 *   for any interrupt handling anyway.
 */
#define PIC_IRQS	(1<<2)

void __init setup_IO_APIC(void)
{
	enable_IO_APIC();

	if (acpi_ioapic)
		io_apic_irqs = ~0;	/* all IRQs go through IOAPIC */
	else
		io_apic_irqs = ~PIC_IRQS;

	printk("ENABLING IO-APIC IRQs\n");

	/*
	 * Set up the IO-APIC IRQ routing table.
	 */
	if (!acpi_ioapic)
		setup_ioapic_ids_from_mpc();
	sync_Arb_IDs();
	setup_IO_APIC_irqs();
	init_IO_APIC_traps();
	check_timer();
	if (!acpi_ioapic)
		print_IO_APIC();
}

/* --------------------------------------------------------------------------
                          ACPI-based IOAPIC Configuration
   -------------------------------------------------------------------------- */

#ifdef CONFIG_ACPI_BOOT

#define IO_APIC_MAX_ID		15

int __init io_apic_get_unique_id (int ioapic, int apic_id)
{
	union IO_APIC_reg_00 reg_00;
	static physid_mask_t apic_id_map;
	unsigned long flags;
	int i = 0;

	/*
	 * The P4 platform supports up to 256 APIC IDs on two separate APIC 
	 * buses (one for LAPICs, one for IOAPICs), where predecessors only 
	 * supports up to 16 on one shared APIC bus.
	 * 
	 * TBD: Expand LAPIC/IOAPIC support on P4-class systems to take full
	 *      advantage of new APIC bus architecture.
	 */

	if (physids_empty(apic_id_map))
		apic_id_map = phys_cpu_present_map;

	spin_lock_irqsave(&ioapic_lock, flags);
	reg_00.raw = io_apic_read(ioapic, 0);
	spin_unlock_irqrestore(&ioapic_lock, flags);

	if (apic_id >= IO_APIC_MAX_ID) {
		printk(KERN_WARNING "IOAPIC[%d]: Invalid apic_id %d, trying "
			"%d\n", ioapic, apic_id, reg_00.bits.ID);
		apic_id = reg_00.bits.ID;
	}

	/*
	 * Every APIC in a system must have a unique ID or we get lots of nice 
	 * 'stuck on smp_invalidate_needed IPI wait' messages.
	 */
	if (physid_isset(apic_id, apic_id_map)) {

		for (i = 0; i < IO_APIC_MAX_ID; i++) {
			if (!physid_isset(i, apic_id_map))
				break;
		}

		if (i == IO_APIC_MAX_ID)
			panic("Max apic_id exceeded!\n");

		printk(KERN_WARNING "IOAPIC[%d]: apic_id %d already used, "
			"trying %d\n", ioapic, apic_id, i);

		apic_id = i;
	} 

	physid_set(apic_id, apic_id_map);

	if (reg_00.bits.ID != apic_id) {
		reg_00.bits.ID = apic_id;

		spin_lock_irqsave(&ioapic_lock, flags);
		io_apic_write(ioapic, 0, reg_00.raw);
		reg_00.raw = io_apic_read(ioapic, 0);
		spin_unlock_irqrestore(&ioapic_lock, flags);

		/* Sanity check */
		if (reg_00.bits.ID != apic_id)
			panic("IOAPIC[%d]: Unable change apic_id!\n", ioapic);
	}

	printk(KERN_INFO "IOAPIC[%d]: Assigned apic_id %d\n", ioapic, apic_id);

	return apic_id;
}


int __init io_apic_get_version (int ioapic)
{
	union IO_APIC_reg_01	reg_01;
	unsigned long flags;

	spin_lock_irqsave(&ioapic_lock, flags);
	reg_01.raw = io_apic_read(ioapic, 1);
	spin_unlock_irqrestore(&ioapic_lock, flags);

	return reg_01.bits.version;
}


int __init io_apic_get_redir_entries (int ioapic)
{
	union IO_APIC_reg_01	reg_01;
	unsigned long flags;

	spin_lock_irqsave(&ioapic_lock, flags);
	reg_01.raw = io_apic_read(ioapic, 1);
	spin_unlock_irqrestore(&ioapic_lock, flags);

	return reg_01.bits.entries;
}


int io_apic_set_pci_routing (int ioapic, int pin, int irq, int edge_level, int active_high_low)
{
	struct IO_APIC_route_entry entry;
	unsigned long flags;

	if (!IO_APIC_IRQ(irq)) {
		printk(KERN_ERR "IOAPIC[%d]: Invalid reference to IRQ 0\n",
			ioapic);
		return -EINVAL;
	}

	/*
	 * Generate a PCI IRQ routing entry and program the IOAPIC accordingly.
	 * Note that we mask (disable) IRQs now -- these get enabled when the
	 * corresponding device driver registers for this IRQ.
	 */

	memset(&entry,0,sizeof(entry));

	entry.delivery_mode = dest_LowestPrio;
	entry.dest_mode = INT_DELIVERY_MODE;
	entry.dest.logical.logical_dest = TARGET_CPUS;
	entry.trigger = edge_level;
	entry.polarity = active_high_low;
	entry.mask = 1;					 /* Disabled (masked) */

	/*
	 * IRQs < 16 are already in the irq_2_pin[] map
	 */
	if (irq >= 16)
		add_pin_to_irq(irq, ioapic, pin);

	entry.vector = assign_irq_vector(irq);

	printk(KERN_DEBUG "IOAPIC[%d]: Set PCI routing entry (%d-%d -> 0x%x -> "
		"IRQ %d Mode:%i Active:%i)\n", ioapic, 
	       mp_ioapics[ioapic].mpc_apicid, pin, entry.vector, irq,
	       edge_level, active_high_low);

 	if (use_pci_vector() && !platform_legacy_irq(irq))
		irq = IO_APIC_VECTOR(irq);
	if (edge_level) {
		irq_desc[irq].handler = &ioapic_level_type;
	} else {
		irq_desc[irq].handler = &ioapic_edge_type;
	}

	set_intr_gate(entry.vector, interrupt[irq]);

	if (!ioapic && (irq < 16))
		disable_8259A_irq(irq);

	spin_lock_irqsave(&ioapic_lock, flags);
	io_apic_write(ioapic, 0x11+2*pin, *(((int *)&entry)+1));
	io_apic_write(ioapic, 0x10+2*pin, *(((int *)&entry)+0));
	spin_unlock_irqrestore(&ioapic_lock, flags);

	return 0;
}

#endif /*CONFIG_ACPI_BOOT*/

#ifndef CONFIG_SMP
void send_IPI_self(int vector)
{
	unsigned int cfg;

       /*
        * Wait for idle.
        */
	apic_wait_icr_idle();
	cfg = APIC_DM_FIXED | APIC_DEST_SELF | vector | APIC_DEST_LOGICAL;

	/*
	 * Send the IPI. The write to APIC_ICR fires this off.
	 */
	apic_write_around(APIC_ICR, cfg);
}
#endif
