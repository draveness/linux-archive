/*
 * I/O SAPIC support.
 *
 * Copyright (C) 1999 Intel Corp.
 * Copyright (C) 1999 Asit Mallick <asit.k.mallick@intel.com>
 * Copyright (C) 2000-2002 J.I. Lee <jung-ik.lee@intel.com>
 * Copyright (C) 1999-2000, 2002-2003 Hewlett-Packard Co.
 *	David Mosberger-Tang <davidm@hpl.hp.com>
 * Copyright (C) 1999 VA Linux Systems
 * Copyright (C) 1999,2000 Walt Drummond <drummond@valinux.com>
 *
 * 00/04/19	D. Mosberger	Rewritten to mirror more closely the x86 I/O
 *				APIC code.  In particular, we now have separate
 *				handlers for edge and level triggered
 *				interrupts.
 * 00/10/27	Asit Mallick, Goutham Rao <goutham.rao@intel.com> IRQ vector
 *				allocation PCI to vector mapping, shared PCI
 *				interrupts.
 * 00/10/27	D. Mosberger	Document things a bit more to make them more
 *				understandable.  Clean up much of the old
 *				IOSAPIC cruft.
 * 01/07/27	J.I. Lee	PCI irq routing, Platform/Legacy interrupts
 *				and fixes for ACPI S5(SoftOff) support.
 * 02/01/23	J.I. Lee	iosapic pgm fixes for PCI irq routing from _PRT
 * 02/01/07     E. Focht        <efocht@ess.nec.de> Redirectable interrupt
 *				vectors in iosapic_set_affinity(),
 *				initializations for /proc/irq/#/smp_affinity
 * 02/04/02	P. Diefenbaugh	Cleaned up ACPI PCI IRQ routing.
 * 02/04/18	J.I. Lee	bug fix in iosapic_init_pci_irq
 * 02/04/30	J.I. Lee	bug fix in find_iosapic to fix ACPI PCI IRQ to
 *				IOSAPIC mapping error
 * 02/07/29	T. Kochi	Allocate interrupt vectors dynamically
 * 02/08/04	T. Kochi	Cleaned up terminology (irq, global system
 *				interrupt, vector, etc.)
 * 02/09/20	D. Mosberger	Simplified by taking advantage of ACPI's
 *				pci_irq code.
 * 03/02/19	B. Helgaas	Make pcat_compat system-wide, not per-IOSAPIC.
 *				Remove iosapic_address & gsi_base from
 *				external interfaces.  Rationalize
 *				__init/__devinit attributes.
 * 04/12/04 Ashok Raj	<ashok.raj@intel.com> Intel Corporation 2004
 *				Updated to work with irq migration necessary
 *				for CPU Hotplug
 */
/*
 * Here is what the interrupt logic between a PCI device and the kernel looks
 * like:
 *
 * (1) A PCI device raises one of the four interrupt pins (INTA, INTB, INTC,
 *     INTD).  The device is uniquely identified by its bus-, and slot-number
 *     (the function number does not matter here because all functions share
 *     the same interrupt lines).
 *
 * (2) The motherboard routes the interrupt line to a pin on a IOSAPIC
 *     controller.  Multiple interrupt lines may have to share the same
 *     IOSAPIC pin (if they're level triggered and use the same polarity).
 *     Each interrupt line has a unique Global System Interrupt (GSI) number
 *     which can be calculated as the sum of the controller's base GSI number
 *     and the IOSAPIC pin number to which the line connects.
 *
 * (3) The IOSAPIC uses an internal routing table entries (RTEs) to map the
 * IOSAPIC pin into the IA-64 interrupt vector.  This interrupt vector is then
 * sent to the CPU.
 *
 * (4) The kernel recognizes an interrupt as an IRQ.  The IRQ interface is
 *     used as architecture-independent interrupt handling mechanism in Linux.
 *     As an IRQ is a number, we have to have
 *     IA-64 interrupt vector number <-> IRQ number mapping.  On smaller
 *     systems, we use one-to-one mapping between IA-64 vector and IRQ.  A
 *     platform can implement platform_irq_to_vector(irq) and
 *     platform_local_vector_to_irq(vector) APIs to differentiate the mapping.
 *     Please see also include/asm-ia64/hw_irq.h for those APIs.
 *
 * To sum up, there are three levels of mappings involved:
 *
 *	PCI pin -> global system interrupt (GSI) -> IA-64 vector <-> IRQ
 *
 * Note: The term "IRQ" is loosely used everywhere in Linux kernel to
 * describeinterrupts.  Now we use "IRQ" only for Linux IRQ's.  ISA IRQ
 * (isa_irq) is the only exception in this source code.
 */

#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/smp.h>
#include <linux/string.h>
#include <linux/bootmem.h>

#include <asm/delay.h>
#include <asm/hw_irq.h>
#include <asm/io.h>
#include <asm/iosapic.h>
#include <asm/machvec.h>
#include <asm/processor.h>
#include <asm/ptrace.h>
#include <asm/system.h>

#undef DEBUG_INTERRUPT_ROUTING

#ifdef DEBUG_INTERRUPT_ROUTING
#define DBG(fmt...)	printk(fmt)
#else
#define DBG(fmt...)
#endif

#define NR_PREALLOCATE_RTE_ENTRIES \
	(PAGE_SIZE / sizeof(struct iosapic_rte_info))
#define RTE_PREALLOCATED	(1)

static DEFINE_SPINLOCK(iosapic_lock);

/*
 * These tables map IA-64 vectors to the IOSAPIC pin that generates this
 * vector.
 */

#define NO_REF_RTE	0

static struct iosapic {
	char __iomem	*addr;		/* base address of IOSAPIC */
	unsigned int	gsi_base;	/* GSI base */
	unsigned short	num_rte;	/* # of RTEs on this IOSAPIC */
	int		rtes_inuse;	/* # of RTEs in use on this IOSAPIC */
#ifdef CONFIG_NUMA
	unsigned short	node;		/* numa node association via pxm */
#endif
	spinlock_t	lock;		/* lock for indirect reg access */
} iosapic_lists[NR_IOSAPICS];

struct iosapic_rte_info {
	struct list_head rte_list;	/* RTEs sharing the same vector */
	char		rte_index;	/* IOSAPIC RTE index */
	int		refcnt;		/* reference counter */
	unsigned int	flags;		/* flags */
	struct iosapic	*iosapic;
} ____cacheline_aligned;

static struct iosapic_intr_info {
	struct list_head rtes;		/* RTEs using this vector (empty =>
					 * not an IOSAPIC interrupt) */
	int		count;		/* # of registered RTEs */
	u32		low32;		/* current value of low word of
					 * Redirection table entry */
	unsigned int	dest;		/* destination CPU physical ID */
	unsigned char	dmode	: 3;	/* delivery mode (see iosapic.h) */
	unsigned char 	polarity: 1;	/* interrupt polarity
					 * (see iosapic.h) */
	unsigned char	trigger	: 1;	/* trigger mode (see iosapic.h) */
} iosapic_intr_info[NR_IRQS];

static unsigned char pcat_compat __devinitdata;	/* 8259 compatibility flag */

static int iosapic_kmalloc_ok;
static LIST_HEAD(free_rte_list);

static inline void
iosapic_write(struct iosapic *iosapic, unsigned int reg, u32 val)
{
	unsigned long flags;

	spin_lock_irqsave(&iosapic->lock, flags);
	__iosapic_write(iosapic->addr, reg, val);
	spin_unlock_irqrestore(&iosapic->lock, flags);
}

/*
 * Find an IOSAPIC associated with a GSI
 */
static inline int
find_iosapic (unsigned int gsi)
{
	int i;

	for (i = 0; i < NR_IOSAPICS; i++) {
		if ((unsigned) (gsi - iosapic_lists[i].gsi_base) <
		    iosapic_lists[i].num_rte)
			return i;
	}

	return -1;
}

static inline int __gsi_to_irq(unsigned int gsi)
{
	int irq;
	struct iosapic_intr_info *info;
	struct iosapic_rte_info *rte;

	for (irq = 0; irq < NR_IRQS; irq++) {
		info = &iosapic_intr_info[irq];
		list_for_each_entry(rte, &info->rtes, rte_list)
			if (rte->iosapic->gsi_base + rte->rte_index == gsi)
				return irq;
	}
	return -1;
}

/*
 * Translate GSI number to the corresponding IA-64 interrupt vector.  If no
 * entry exists, return -1.
 */
inline int
gsi_to_vector (unsigned int gsi)
{
	int irq = __gsi_to_irq(gsi);
	if (check_irq_used(irq) < 0)
		return -1;
	return irq_to_vector(irq);
}

int
gsi_to_irq (unsigned int gsi)
{
	unsigned long flags;
	int irq;

	spin_lock_irqsave(&iosapic_lock, flags);
	irq = __gsi_to_irq(gsi);
	spin_unlock_irqrestore(&iosapic_lock, flags);
	return irq;
}

static struct iosapic_rte_info *find_rte(unsigned int irq, unsigned int gsi)
{
	struct iosapic_rte_info *rte;

	list_for_each_entry(rte, &iosapic_intr_info[irq].rtes, rte_list)
		if (rte->iosapic->gsi_base + rte->rte_index == gsi)
			return rte;
	return NULL;
}

static void
set_rte (unsigned int gsi, unsigned int irq, unsigned int dest, int mask)
{
	unsigned long pol, trigger, dmode;
	u32 low32, high32;
	int rte_index;
	char redir;
	struct iosapic_rte_info *rte;
	ia64_vector vector = irq_to_vector(irq);

	DBG(KERN_DEBUG"IOSAPIC: routing vector %d to 0x%x\n", vector, dest);

	rte = find_rte(irq, gsi);
	if (!rte)
		return;		/* not an IOSAPIC interrupt */

	rte_index = rte->rte_index;
	pol     = iosapic_intr_info[irq].polarity;
	trigger = iosapic_intr_info[irq].trigger;
	dmode   = iosapic_intr_info[irq].dmode;

	redir = (dmode == IOSAPIC_LOWEST_PRIORITY) ? 1 : 0;

#ifdef CONFIG_SMP
	set_irq_affinity_info(irq, (int)(dest & 0xffff), redir);
#endif

	low32 = ((pol << IOSAPIC_POLARITY_SHIFT) |
		 (trigger << IOSAPIC_TRIGGER_SHIFT) |
		 (dmode << IOSAPIC_DELIVERY_SHIFT) |
		 ((mask ? 1 : 0) << IOSAPIC_MASK_SHIFT) |
		 vector);

	/* dest contains both id and eid */
	high32 = (dest << IOSAPIC_DEST_SHIFT);

	iosapic_write(rte->iosapic, IOSAPIC_RTE_HIGH(rte_index), high32);
	iosapic_write(rte->iosapic, IOSAPIC_RTE_LOW(rte_index), low32);
	iosapic_intr_info[irq].low32 = low32;
	iosapic_intr_info[irq].dest = dest;
}

static void
nop (unsigned int irq)
{
	/* do nothing... */
}


#ifdef CONFIG_KEXEC
void
kexec_disable_iosapic(void)
{
	struct iosapic_intr_info *info;
	struct iosapic_rte_info *rte;
	ia64_vector vec;
	int irq;

	for (irq = 0; irq < NR_IRQS; irq++) {
		info = &iosapic_intr_info[irq];
		vec = irq_to_vector(irq);
		list_for_each_entry(rte, &info->rtes,
				rte_list) {
			iosapic_write(rte->iosapic,
					IOSAPIC_RTE_LOW(rte->rte_index),
					IOSAPIC_MASK|vec);
			iosapic_eoi(rte->iosapic->addr, vec);
		}
	}
}
#endif

static void
mask_irq (unsigned int irq)
{
	u32 low32;
	int rte_index;
	struct iosapic_rte_info *rte;

	if (!iosapic_intr_info[irq].count)
		return;			/* not an IOSAPIC interrupt! */

	/* set only the mask bit */
	low32 = iosapic_intr_info[irq].low32 |= IOSAPIC_MASK;
	list_for_each_entry(rte, &iosapic_intr_info[irq].rtes, rte_list) {
		rte_index = rte->rte_index;
		iosapic_write(rte->iosapic, IOSAPIC_RTE_LOW(rte_index), low32);
	}
}

static void
unmask_irq (unsigned int irq)
{
	u32 low32;
	int rte_index;
	struct iosapic_rte_info *rte;

	if (!iosapic_intr_info[irq].count)
		return;			/* not an IOSAPIC interrupt! */

	low32 = iosapic_intr_info[irq].low32 &= ~IOSAPIC_MASK;
	list_for_each_entry(rte, &iosapic_intr_info[irq].rtes, rte_list) {
		rte_index = rte->rte_index;
		iosapic_write(rte->iosapic, IOSAPIC_RTE_LOW(rte_index), low32);
	}
}


static void
iosapic_set_affinity (unsigned int irq, cpumask_t mask)
{
#ifdef CONFIG_SMP
	u32 high32, low32;
	int dest, rte_index;
	int redir = (irq & IA64_IRQ_REDIRECTED) ? 1 : 0;
	struct iosapic_rte_info *rte;
	struct iosapic *iosapic;

	irq &= (~IA64_IRQ_REDIRECTED);

	cpus_and(mask, mask, cpu_online_map);
	if (cpus_empty(mask))
		return;

	if (reassign_irq_vector(irq, first_cpu(mask)))
		return;

	dest = cpu_physical_id(first_cpu(mask));

	if (!iosapic_intr_info[irq].count)
		return;			/* not an IOSAPIC interrupt */

	set_irq_affinity_info(irq, dest, redir);

	/* dest contains both id and eid */
	high32 = dest << IOSAPIC_DEST_SHIFT;

	low32 = iosapic_intr_info[irq].low32 & ~(7 << IOSAPIC_DELIVERY_SHIFT);
	if (redir)
		/* change delivery mode to lowest priority */
		low32 |= (IOSAPIC_LOWEST_PRIORITY << IOSAPIC_DELIVERY_SHIFT);
	else
		/* change delivery mode to fixed */
		low32 |= (IOSAPIC_FIXED << IOSAPIC_DELIVERY_SHIFT);
	low32 &= IOSAPIC_VECTOR_MASK;
	low32 |= irq_to_vector(irq);

	iosapic_intr_info[irq].low32 = low32;
	iosapic_intr_info[irq].dest = dest;
	list_for_each_entry(rte, &iosapic_intr_info[irq].rtes, rte_list) {
		iosapic = rte->iosapic;
		rte_index = rte->rte_index;
		iosapic_write(iosapic, IOSAPIC_RTE_HIGH(rte_index), high32);
		iosapic_write(iosapic, IOSAPIC_RTE_LOW(rte_index), low32);
	}
#endif
}

/*
 * Handlers for level-triggered interrupts.
 */

static unsigned int
iosapic_startup_level_irq (unsigned int irq)
{
	unmask_irq(irq);
	return 0;
}

static void
iosapic_end_level_irq (unsigned int irq)
{
	ia64_vector vec = irq_to_vector(irq);
	struct iosapic_rte_info *rte;
	int do_unmask_irq = 0;

	if (unlikely(irq_desc[irq].status & IRQ_MOVE_PENDING)) {
		do_unmask_irq = 1;
		mask_irq(irq);
	}

	list_for_each_entry(rte, &iosapic_intr_info[irq].rtes, rte_list)
		iosapic_eoi(rte->iosapic->addr, vec);

	if (unlikely(do_unmask_irq)) {
		move_masked_irq(irq);
		unmask_irq(irq);
	}
}

#define iosapic_shutdown_level_irq	mask_irq
#define iosapic_enable_level_irq	unmask_irq
#define iosapic_disable_level_irq	mask_irq
#define iosapic_ack_level_irq		nop

struct irq_chip irq_type_iosapic_level = {
	.name =		"IO-SAPIC-level",
	.startup =	iosapic_startup_level_irq,
	.shutdown =	iosapic_shutdown_level_irq,
	.enable =	iosapic_enable_level_irq,
	.disable =	iosapic_disable_level_irq,
	.ack =		iosapic_ack_level_irq,
	.end =		iosapic_end_level_irq,
	.mask =		mask_irq,
	.unmask =	unmask_irq,
	.set_affinity =	iosapic_set_affinity
};

/*
 * Handlers for edge-triggered interrupts.
 */

static unsigned int
iosapic_startup_edge_irq (unsigned int irq)
{
	unmask_irq(irq);
	/*
	 * IOSAPIC simply drops interrupts pended while the
	 * corresponding pin was masked, so we can't know if an
	 * interrupt is pending already.  Let's hope not...
	 */
	return 0;
}

static void
iosapic_ack_edge_irq (unsigned int irq)
{
	irq_desc_t *idesc = irq_desc + irq;

	move_native_irq(irq);
	/*
	 * Once we have recorded IRQ_PENDING already, we can mask the
	 * interrupt for real. This prevents IRQ storms from unhandled
	 * devices.
	 */
	if ((idesc->status & (IRQ_PENDING|IRQ_DISABLED)) ==
	    (IRQ_PENDING|IRQ_DISABLED))
		mask_irq(irq);
}

#define iosapic_enable_edge_irq		unmask_irq
#define iosapic_disable_edge_irq	nop
#define iosapic_end_edge_irq		nop

struct irq_chip irq_type_iosapic_edge = {
	.name =		"IO-SAPIC-edge",
	.startup =	iosapic_startup_edge_irq,
	.shutdown =	iosapic_disable_edge_irq,
	.enable =	iosapic_enable_edge_irq,
	.disable =	iosapic_disable_edge_irq,
	.ack =		iosapic_ack_edge_irq,
	.end =		iosapic_end_edge_irq,
	.mask =		mask_irq,
	.unmask =	unmask_irq,
	.set_affinity =	iosapic_set_affinity
};

unsigned int
iosapic_version (char __iomem *addr)
{
	/*
	 * IOSAPIC Version Register return 32 bit structure like:
	 * {
	 *	unsigned int version   : 8;
	 *	unsigned int reserved1 : 8;
	 *	unsigned int max_redir : 8;
	 *	unsigned int reserved2 : 8;
	 * }
	 */
	return __iosapic_read(addr, IOSAPIC_VERSION);
}

static int iosapic_find_sharable_irq(unsigned long trigger, unsigned long pol)
{
	int i, irq = -ENOSPC, min_count = -1;
	struct iosapic_intr_info *info;

	/*
	 * shared vectors for edge-triggered interrupts are not
	 * supported yet
	 */
	if (trigger == IOSAPIC_EDGE)
		return -EINVAL;

	for (i = 0; i <= NR_IRQS; i++) {
		info = &iosapic_intr_info[i];
		if (info->trigger == trigger && info->polarity == pol &&
		    (info->dmode == IOSAPIC_FIXED ||
		     info->dmode == IOSAPIC_LOWEST_PRIORITY) &&
		    can_request_irq(i, IRQF_SHARED)) {
			if (min_count == -1 || info->count < min_count) {
				irq = i;
				min_count = info->count;
			}
		}
	}
	return irq;
}

/*
 * if the given vector is already owned by other,
 *  assign a new vector for the other and make the vector available
 */
static void __init
iosapic_reassign_vector (int irq)
{
	int new_irq;

	if (iosapic_intr_info[irq].count) {
		new_irq = create_irq();
		if (new_irq < 0)
			panic("%s: out of interrupt vectors!\n", __FUNCTION__);
		printk(KERN_INFO "Reassigning vector %d to %d\n",
		       irq_to_vector(irq), irq_to_vector(new_irq));
		memcpy(&iosapic_intr_info[new_irq], &iosapic_intr_info[irq],
		       sizeof(struct iosapic_intr_info));
		INIT_LIST_HEAD(&iosapic_intr_info[new_irq].rtes);
		list_move(iosapic_intr_info[irq].rtes.next,
			  &iosapic_intr_info[new_irq].rtes);
		memset(&iosapic_intr_info[irq], 0,
		       sizeof(struct iosapic_intr_info));
		iosapic_intr_info[irq].low32 = IOSAPIC_MASK;
		INIT_LIST_HEAD(&iosapic_intr_info[irq].rtes);
	}
}

static struct iosapic_rte_info * __init_refok iosapic_alloc_rte (void)
{
	int i;
	struct iosapic_rte_info *rte;
	int preallocated = 0;

	if (!iosapic_kmalloc_ok && list_empty(&free_rte_list)) {
		rte = alloc_bootmem(sizeof(struct iosapic_rte_info) *
				    NR_PREALLOCATE_RTE_ENTRIES);
		if (!rte)
			return NULL;
		for (i = 0; i < NR_PREALLOCATE_RTE_ENTRIES; i++, rte++)
			list_add(&rte->rte_list, &free_rte_list);
	}

	if (!list_empty(&free_rte_list)) {
		rte = list_entry(free_rte_list.next, struct iosapic_rte_info,
				 rte_list);
		list_del(&rte->rte_list);
		preallocated++;
	} else {
		rte = kmalloc(sizeof(struct iosapic_rte_info), GFP_ATOMIC);
		if (!rte)
			return NULL;
	}

	memset(rte, 0, sizeof(struct iosapic_rte_info));
	if (preallocated)
		rte->flags |= RTE_PREALLOCATED;

	return rte;
}

static inline int irq_is_shared (int irq)
{
	return (iosapic_intr_info[irq].count > 1);
}

static int
register_intr (unsigned int gsi, int irq, unsigned char delivery,
	       unsigned long polarity, unsigned long trigger)
{
	irq_desc_t *idesc;
	struct hw_interrupt_type *irq_type;
	int index;
	struct iosapic_rte_info *rte;

	index = find_iosapic(gsi);
	if (index < 0) {
		printk(KERN_WARNING "%s: No IOSAPIC for GSI %u\n",
		       __FUNCTION__, gsi);
		return -ENODEV;
	}

	rte = find_rte(irq, gsi);
	if (!rte) {
		rte = iosapic_alloc_rte();
		if (!rte) {
			printk(KERN_WARNING "%s: cannot allocate memory\n",
			       __FUNCTION__);
			return -ENOMEM;
		}

		rte->iosapic	= &iosapic_lists[index];
		rte->rte_index	= gsi - rte->iosapic->gsi_base;
		rte->refcnt++;
		list_add_tail(&rte->rte_list, &iosapic_intr_info[irq].rtes);
		iosapic_intr_info[irq].count++;
		iosapic_lists[index].rtes_inuse++;
	}
	else if (rte->refcnt == NO_REF_RTE) {
		struct iosapic_intr_info *info = &iosapic_intr_info[irq];
		if (info->count > 0 &&
		    (info->trigger != trigger || info->polarity != polarity)){
			printk (KERN_WARNING
				"%s: cannot override the interrupt\n",
				__FUNCTION__);
			return -EINVAL;
		}
		rte->refcnt++;
		iosapic_intr_info[irq].count++;
		iosapic_lists[index].rtes_inuse++;
	}

	iosapic_intr_info[irq].polarity = polarity;
	iosapic_intr_info[irq].dmode    = delivery;
	iosapic_intr_info[irq].trigger  = trigger;

	if (trigger == IOSAPIC_EDGE)
		irq_type = &irq_type_iosapic_edge;
	else
		irq_type = &irq_type_iosapic_level;

	idesc = irq_desc + irq;
	if (idesc->chip != irq_type) {
		if (idesc->chip != &no_irq_type)
			printk(KERN_WARNING
			       "%s: changing vector %d from %s to %s\n",
			       __FUNCTION__, irq_to_vector(irq),
			       idesc->chip->name, irq_type->name);
		idesc->chip = irq_type;
	}
	return 0;
}

static unsigned int
get_target_cpu (unsigned int gsi, int irq)
{
#ifdef CONFIG_SMP
	static int cpu = -1;
	extern int cpe_vector;
	cpumask_t domain = irq_to_domain(irq);

	/*
	 * In case of vector shared by multiple RTEs, all RTEs that
	 * share the vector need to use the same destination CPU.
	 */
	if (iosapic_intr_info[irq].count)
		return iosapic_intr_info[irq].dest;

	/*
	 * If the platform supports redirection via XTP, let it
	 * distribute interrupts.
	 */
	if (smp_int_redirect & SMP_IRQ_REDIRECTION)
		return cpu_physical_id(smp_processor_id());

	/*
	 * Some interrupts (ACPI SCI, for instance) are registered
	 * before the BSP is marked as online.
	 */
	if (!cpu_online(smp_processor_id()))
		return cpu_physical_id(smp_processor_id());

#ifdef CONFIG_ACPI
	if (cpe_vector > 0 && irq_to_vector(irq) == IA64_CPEP_VECTOR)
		return get_cpei_target_cpu();
#endif

#ifdef CONFIG_NUMA
	{
		int num_cpus, cpu_index, iosapic_index, numa_cpu, i = 0;
		cpumask_t cpu_mask;

		iosapic_index = find_iosapic(gsi);
		if (iosapic_index < 0 ||
		    iosapic_lists[iosapic_index].node == MAX_NUMNODES)
			goto skip_numa_setup;

		cpu_mask = node_to_cpumask(iosapic_lists[iosapic_index].node);
		cpus_and(cpu_mask, cpu_mask, domain);
		for_each_cpu_mask(numa_cpu, cpu_mask) {
			if (!cpu_online(numa_cpu))
				cpu_clear(numa_cpu, cpu_mask);
		}

		num_cpus = cpus_weight(cpu_mask);

		if (!num_cpus)
			goto skip_numa_setup;

		/* Use irq assignment to distribute across cpus in node */
		cpu_index = irq % num_cpus;

		for (numa_cpu = first_cpu(cpu_mask) ; i < cpu_index ; i++)
			numa_cpu = next_cpu(numa_cpu, cpu_mask);

		if (numa_cpu != NR_CPUS)
			return cpu_physical_id(numa_cpu);
	}
skip_numa_setup:
#endif
	/*
	 * Otherwise, round-robin interrupt vectors across all the
	 * processors.  (It'd be nice if we could be smarter in the
	 * case of NUMA.)
	 */
	do {
		if (++cpu >= NR_CPUS)
			cpu = 0;
	} while (!cpu_online(cpu) || !cpu_isset(cpu, domain));

	return cpu_physical_id(cpu);
#else  /* CONFIG_SMP */
	return cpu_physical_id(smp_processor_id());
#endif
}

/*
 * ACPI can describe IOSAPIC interrupts via static tables and namespace
 * methods.  This provides an interface to register those interrupts and
 * program the IOSAPIC RTE.
 */
int
iosapic_register_intr (unsigned int gsi,
		       unsigned long polarity, unsigned long trigger)
{
	int irq, mask = 1, err;
	unsigned int dest;
	unsigned long flags;
	struct iosapic_rte_info *rte;
	u32 low32;

	/*
	 * If this GSI has already been registered (i.e., it's a
	 * shared interrupt, or we lost a race to register it),
	 * don't touch the RTE.
	 */
	spin_lock_irqsave(&iosapic_lock, flags);
	irq = __gsi_to_irq(gsi);
	if (irq > 0) {
		rte = find_rte(irq, gsi);
		if(iosapic_intr_info[irq].count == 0) {
			assign_irq_vector(irq);
			dynamic_irq_init(irq);
		} else if (rte->refcnt != NO_REF_RTE) {
			rte->refcnt++;
			goto unlock_iosapic_lock;
		}
	} else
		irq = create_irq();

	/* If vector is running out, we try to find a sharable vector */
	if (irq < 0) {
		irq = iosapic_find_sharable_irq(trigger, polarity);
		if (irq < 0)
			goto unlock_iosapic_lock;
	}

	spin_lock(&irq_desc[irq].lock);
	dest = get_target_cpu(gsi, irq);
	err = register_intr(gsi, irq, IOSAPIC_LOWEST_PRIORITY,
			    polarity, trigger);
	if (err < 0) {
		spin_unlock(&irq_desc[irq].lock);
		irq = err;
		goto unlock_iosapic_lock;
	}

	/*
	 * If the vector is shared and already unmasked for other
	 * interrupt sources, don't mask it.
	 */
	low32 = iosapic_intr_info[irq].low32;
	if (irq_is_shared(irq) && !(low32 & IOSAPIC_MASK))
		mask = 0;
	set_rte(gsi, irq, dest, mask);

	printk(KERN_INFO "GSI %u (%s, %s) -> CPU %d (0x%04x) vector %d\n",
	       gsi, (trigger == IOSAPIC_EDGE ? "edge" : "level"),
	       (polarity == IOSAPIC_POL_HIGH ? "high" : "low"),
	       cpu_logical_id(dest), dest, irq_to_vector(irq));

	spin_unlock(&irq_desc[irq].lock);
 unlock_iosapic_lock:
	spin_unlock_irqrestore(&iosapic_lock, flags);
	return irq;
}

void
iosapic_unregister_intr (unsigned int gsi)
{
	unsigned long flags;
	int irq, index;
	irq_desc_t *idesc;
	u32 low32;
	unsigned long trigger, polarity;
	unsigned int dest;
	struct iosapic_rte_info *rte;

	/*
	 * If the irq associated with the gsi is not found,
	 * iosapic_unregister_intr() is unbalanced. We need to check
	 * this again after getting locks.
	 */
	irq = gsi_to_irq(gsi);
	if (irq < 0) {
		printk(KERN_ERR "iosapic_unregister_intr(%u) unbalanced\n",
		       gsi);
		WARN_ON(1);
		return;
	}

	spin_lock_irqsave(&iosapic_lock, flags);
	if ((rte = find_rte(irq, gsi)) == NULL) {
		printk(KERN_ERR "iosapic_unregister_intr(%u) unbalanced\n",
		       gsi);
		WARN_ON(1);
		goto out;
	}

	if (--rte->refcnt > 0)
		goto out;

	idesc = irq_desc + irq;
	rte->refcnt = NO_REF_RTE;

	/* Mask the interrupt */
	low32 = iosapic_intr_info[irq].low32 | IOSAPIC_MASK;
	iosapic_write(rte->iosapic, IOSAPIC_RTE_LOW(rte->rte_index), low32);

	iosapic_intr_info[irq].count--;
	index = find_iosapic(gsi);
	iosapic_lists[index].rtes_inuse--;
	WARN_ON(iosapic_lists[index].rtes_inuse < 0);

	trigger  = iosapic_intr_info[irq].trigger;
	polarity = iosapic_intr_info[irq].polarity;
	dest     = iosapic_intr_info[irq].dest;
	printk(KERN_INFO
	       "GSI %u (%s, %s) -> CPU %d (0x%04x) vector %d unregistered\n",
	       gsi, (trigger == IOSAPIC_EDGE ? "edge" : "level"),
	       (polarity == IOSAPIC_POL_HIGH ? "high" : "low"),
	       cpu_logical_id(dest), dest, irq_to_vector(irq));

	if (iosapic_intr_info[irq].count == 0) {
#ifdef CONFIG_SMP
		/* Clear affinity */
		cpus_setall(idesc->affinity);
#endif
		/* Clear the interrupt information */
		iosapic_intr_info[irq].dest = 0;
		iosapic_intr_info[irq].dmode = 0;
		iosapic_intr_info[irq].polarity = 0;
		iosapic_intr_info[irq].trigger = 0;
		iosapic_intr_info[irq].low32 |= IOSAPIC_MASK;

		/* Destroy and reserve IRQ */
		destroy_and_reserve_irq(irq);
	}
 out:
	spin_unlock_irqrestore(&iosapic_lock, flags);
}

/*
 * ACPI calls this when it finds an entry for a platform interrupt.
 */
int __init
iosapic_register_platform_intr (u32 int_type, unsigned int gsi,
				int iosapic_vector, u16 eid, u16 id,
				unsigned long polarity, unsigned long trigger)
{
	static const char * const name[] = {"unknown", "PMI", "INIT", "CPEI"};
	unsigned char delivery;
	int irq, vector, mask = 0;
	unsigned int dest = ((id << 8) | eid) & 0xffff;

	switch (int_type) {
	      case ACPI_INTERRUPT_PMI:
		irq = vector = iosapic_vector;
		bind_irq_vector(irq, vector, CPU_MASK_ALL);
		/*
		 * since PMI vector is alloc'd by FW(ACPI) not by kernel,
		 * we need to make sure the vector is available
		 */
		iosapic_reassign_vector(irq);
		delivery = IOSAPIC_PMI;
		break;
	      case ACPI_INTERRUPT_INIT:
		irq = create_irq();
		if (irq < 0)
			panic("%s: out of interrupt vectors!\n", __FUNCTION__);
		vector = irq_to_vector(irq);
		delivery = IOSAPIC_INIT;
		break;
	      case ACPI_INTERRUPT_CPEI:
		irq = vector = IA64_CPE_VECTOR;
		BUG_ON(bind_irq_vector(irq, vector, CPU_MASK_ALL));
		delivery = IOSAPIC_LOWEST_PRIORITY;
		mask = 1;
		break;
	      default:
		printk(KERN_ERR "%s: invalid int type 0x%x\n", __FUNCTION__,
		       int_type);
		return -1;
	}

	register_intr(gsi, irq, delivery, polarity, trigger);

	printk(KERN_INFO
	       "PLATFORM int %s (0x%x): GSI %u (%s, %s) -> CPU %d (0x%04x)"
	       " vector %d\n",
	       int_type < ARRAY_SIZE(name) ? name[int_type] : "unknown",
	       int_type, gsi, (trigger == IOSAPIC_EDGE ? "edge" : "level"),
	       (polarity == IOSAPIC_POL_HIGH ? "high" : "low"),
	       cpu_logical_id(dest), dest, vector);

	set_rte(gsi, irq, dest, mask);
	return vector;
}

/*
 * ACPI calls this when it finds an entry for a legacy ISA IRQ override.
 */
void __devinit
iosapic_override_isa_irq (unsigned int isa_irq, unsigned int gsi,
			  unsigned long polarity,
			  unsigned long trigger)
{
	int vector, irq;
	unsigned int dest = cpu_physical_id(smp_processor_id());

	irq = vector = isa_irq_to_vector(isa_irq);
	BUG_ON(bind_irq_vector(irq, vector, CPU_MASK_ALL));
	register_intr(gsi, irq, IOSAPIC_LOWEST_PRIORITY, polarity, trigger);

	DBG("ISA: IRQ %u -> GSI %u (%s,%s) -> CPU %d (0x%04x) vector %d\n",
	    isa_irq, gsi, trigger == IOSAPIC_EDGE ? "edge" : "level",
	    polarity == IOSAPIC_POL_HIGH ? "high" : "low",
	    cpu_logical_id(dest), dest, vector);

	set_rte(gsi, irq, dest, 1);
}

void __init
iosapic_system_init (int system_pcat_compat)
{
	int irq;

	for (irq = 0; irq < NR_IRQS; ++irq) {
		iosapic_intr_info[irq].low32 = IOSAPIC_MASK;
		/* mark as unused */
		INIT_LIST_HEAD(&iosapic_intr_info[irq].rtes);

		iosapic_intr_info[irq].count = 0;
	}

	pcat_compat = system_pcat_compat;
	if (pcat_compat) {
		/*
		 * Disable the compatibility mode interrupts (8259 style),
		 * needs IN/OUT support enabled.
		 */
		printk(KERN_INFO
		       "%s: Disabling PC-AT compatible 8259 interrupts\n",
		       __FUNCTION__);
		outb(0xff, 0xA1);
		outb(0xff, 0x21);
	}
}

static inline int
iosapic_alloc (void)
{
	int index;

	for (index = 0; index < NR_IOSAPICS; index++)
		if (!iosapic_lists[index].addr)
			return index;

	printk(KERN_WARNING "%s: failed to allocate iosapic\n", __FUNCTION__);
	return -1;
}

static inline void
iosapic_free (int index)
{
	memset(&iosapic_lists[index], 0, sizeof(iosapic_lists[0]));
}

static inline int
iosapic_check_gsi_range (unsigned int gsi_base, unsigned int ver)
{
	int index;
	unsigned int gsi_end, base, end;

	/* check gsi range */
	gsi_end = gsi_base + ((ver >> 16) & 0xff);
	for (index = 0; index < NR_IOSAPICS; index++) {
		if (!iosapic_lists[index].addr)
			continue;

		base = iosapic_lists[index].gsi_base;
		end  = base + iosapic_lists[index].num_rte - 1;

		if (gsi_end < base || end < gsi_base)
			continue; /* OK */

		return -EBUSY;
	}
	return 0;
}

int __devinit
iosapic_init (unsigned long phys_addr, unsigned int gsi_base)
{
	int num_rte, err, index;
	unsigned int isa_irq, ver;
	char __iomem *addr;
	unsigned long flags;

	spin_lock_irqsave(&iosapic_lock, flags);
	index = find_iosapic(gsi_base);
	if (index >= 0) {
		spin_unlock_irqrestore(&iosapic_lock, flags);
		return -EBUSY;
	}

	addr = ioremap(phys_addr, 0);
	ver = iosapic_version(addr);
	if ((err = iosapic_check_gsi_range(gsi_base, ver))) {
		iounmap(addr);
		spin_unlock_irqrestore(&iosapic_lock, flags);
		return err;
	}

	/*
	 * The MAX_REDIR register holds the highest input pin number
	 * (starting from 0).  We add 1 so that we can use it for
	 * number of pins (= RTEs)
	 */
	num_rte = ((ver >> 16) & 0xff) + 1;

	index = iosapic_alloc();
	iosapic_lists[index].addr = addr;
	iosapic_lists[index].gsi_base = gsi_base;
	iosapic_lists[index].num_rte = num_rte;
#ifdef CONFIG_NUMA
	iosapic_lists[index].node = MAX_NUMNODES;
#endif
	spin_lock_init(&iosapic_lists[index].lock);
	spin_unlock_irqrestore(&iosapic_lock, flags);

	if ((gsi_base == 0) && pcat_compat) {
		/*
		 * Map the legacy ISA devices into the IOSAPIC data.  Some of
		 * these may get reprogrammed later on with data from the ACPI
		 * Interrupt Source Override table.
		 */
		for (isa_irq = 0; isa_irq < 16; ++isa_irq)
			iosapic_override_isa_irq(isa_irq, isa_irq,
						 IOSAPIC_POL_HIGH,
						 IOSAPIC_EDGE);
	}
	return 0;
}

#ifdef CONFIG_HOTPLUG
int
iosapic_remove (unsigned int gsi_base)
{
	int index, err = 0;
	unsigned long flags;

	spin_lock_irqsave(&iosapic_lock, flags);
	index = find_iosapic(gsi_base);
	if (index < 0) {
		printk(KERN_WARNING "%s: No IOSAPIC for GSI base %u\n",
		       __FUNCTION__, gsi_base);
		goto out;
	}

	if (iosapic_lists[index].rtes_inuse) {
		err = -EBUSY;
		printk(KERN_WARNING "%s: IOSAPIC for GSI base %u is busy\n",
		       __FUNCTION__, gsi_base);
		goto out;
	}

	iounmap(iosapic_lists[index].addr);
	iosapic_free(index);
 out:
	spin_unlock_irqrestore(&iosapic_lock, flags);
	return err;
}
#endif /* CONFIG_HOTPLUG */

#ifdef CONFIG_NUMA
void __devinit
map_iosapic_to_node(unsigned int gsi_base, int node)
{
	int index;

	index = find_iosapic(gsi_base);
	if (index < 0) {
		printk(KERN_WARNING "%s: No IOSAPIC for GSI %u\n",
		       __FUNCTION__, gsi_base);
		return;
	}
	iosapic_lists[index].node = node;
	return;
}
#endif

static int __init iosapic_enable_kmalloc (void)
{
	iosapic_kmalloc_ok = 1;
	return 0;
}
core_initcall (iosapic_enable_kmalloc);
