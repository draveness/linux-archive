/*
 * Xen event channels
 *
 * Xen models interrupts with abstract event channels.  Because each
 * domain gets 1024 event channels, but NR_IRQ is not that large, we
 * must dynamically map irqs<->event channels.  The event channels
 * interface with the rest of the kernel by defining a xen interrupt
 * chip.  When an event is recieved, it is mapped to an irq and sent
 * through the normal interrupt processing path.
 *
 * There are four kinds of events which can be mapped to an event
 * channel:
 *
 * 1. Inter-domain notifications.  This includes all the virtual
 *    device events, since they're driven by front-ends in another domain
 *    (typically dom0).
 * 2. VIRQs, typically used for timers.  These are per-cpu events.
 * 3. IPIs.
 * 4. Hardware interrupts. Not supported at present.
 *
 * Jeremy Fitzhardinge <jeremy@xensource.com>, XenSource Inc, 2007
 */

#include <linux/linkage.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/string.h>

#include <asm/ptrace.h>
#include <asm/irq.h>
#include <asm/sync_bitops.h>
#include <asm/xen/hypercall.h>
#include <asm/xen/hypervisor.h>

#include <xen/events.h>
#include <xen/interface/xen.h>
#include <xen/interface/event_channel.h>

#include "xen-ops.h"

/*
 * This lock protects updates to the following mapping and reference-count
 * arrays. The lock does not need to be acquired to read the mapping tables.
 */
static DEFINE_SPINLOCK(irq_mapping_update_lock);

/* IRQ <-> VIRQ mapping. */
static DEFINE_PER_CPU(int, virq_to_irq[NR_VIRQS]) = {[0 ... NR_VIRQS-1] = -1};

/* IRQ <-> IPI mapping */
static DEFINE_PER_CPU(int, ipi_to_irq[XEN_NR_IPIS]) = {[0 ... XEN_NR_IPIS-1] = -1};

/* Packed IRQ information: binding type, sub-type index, and event channel. */
struct packed_irq
{
	unsigned short evtchn;
	unsigned char index;
	unsigned char type;
};

static struct packed_irq irq_info[NR_IRQS];

/* Binding types. */
enum {
	IRQT_UNBOUND,
	IRQT_PIRQ,
	IRQT_VIRQ,
	IRQT_IPI,
	IRQT_EVTCHN
};

/* Convenient shorthand for packed representation of an unbound IRQ. */
#define IRQ_UNBOUND	mk_irq_info(IRQT_UNBOUND, 0, 0)

static int evtchn_to_irq[NR_EVENT_CHANNELS] = {
	[0 ... NR_EVENT_CHANNELS-1] = -1
};
static unsigned long cpu_evtchn_mask[NR_CPUS][NR_EVENT_CHANNELS/BITS_PER_LONG];
static u8 cpu_evtchn[NR_EVENT_CHANNELS];

/* Reference counts for bindings to IRQs. */
static int irq_bindcount[NR_IRQS];

/* Xen will never allocate port zero for any purpose. */
#define VALID_EVTCHN(chn)	((chn) != 0)

/*
 * Force a proper event-channel callback from Xen after clearing the
 * callback mask. We do this in a very simple manner, by making a call
 * down into Xen. The pending flag will be checked by Xen on return.
 */
void force_evtchn_callback(void)
{
	(void)HYPERVISOR_xen_version(0, NULL);
}
EXPORT_SYMBOL_GPL(force_evtchn_callback);

static struct irq_chip xen_dynamic_chip;

/* Constructor for packed IRQ information. */
static inline struct packed_irq mk_irq_info(u32 type, u32 index, u32 evtchn)
{
	return (struct packed_irq) { evtchn, index, type };
}

/*
 * Accessors for packed IRQ information.
 */
static inline unsigned int evtchn_from_irq(int irq)
{
	return irq_info[irq].evtchn;
}

static inline unsigned int index_from_irq(int irq)
{
	return irq_info[irq].index;
}

static inline unsigned int type_from_irq(int irq)
{
	return irq_info[irq].type;
}

static inline unsigned long active_evtchns(unsigned int cpu,
					   struct shared_info *sh,
					   unsigned int idx)
{
	return (sh->evtchn_pending[idx] &
		cpu_evtchn_mask[cpu][idx] &
		~sh->evtchn_mask[idx]);
}

static void bind_evtchn_to_cpu(unsigned int chn, unsigned int cpu)
{
	int irq = evtchn_to_irq[chn];

	BUG_ON(irq == -1);
#ifdef CONFIG_SMP
	irq_desc[irq].affinity = cpumask_of_cpu(cpu);
#endif

	__clear_bit(chn, cpu_evtchn_mask[cpu_evtchn[chn]]);
	__set_bit(chn, cpu_evtchn_mask[cpu]);

	cpu_evtchn[chn] = cpu;
}

static void init_evtchn_cpu_bindings(void)
{
#ifdef CONFIG_SMP
	int i;
	/* By default all event channels notify CPU#0. */
	for (i = 0; i < NR_IRQS; i++)
		irq_desc[i].affinity = cpumask_of_cpu(0);
#endif

	memset(cpu_evtchn, 0, sizeof(cpu_evtchn));
	memset(cpu_evtchn_mask[0], ~0, sizeof(cpu_evtchn_mask[0]));
}

static inline unsigned int cpu_from_evtchn(unsigned int evtchn)
{
	return cpu_evtchn[evtchn];
}

static inline void clear_evtchn(int port)
{
	struct shared_info *s = HYPERVISOR_shared_info;
	sync_clear_bit(port, &s->evtchn_pending[0]);
}

static inline void set_evtchn(int port)
{
	struct shared_info *s = HYPERVISOR_shared_info;
	sync_set_bit(port, &s->evtchn_pending[0]);
}


/**
 * notify_remote_via_irq - send event to remote end of event channel via irq
 * @irq: irq of event channel to send event to
 *
 * Unlike notify_remote_via_evtchn(), this is safe to use across
 * save/restore. Notifications on a broken connection are silently
 * dropped.
 */
void notify_remote_via_irq(int irq)
{
	int evtchn = evtchn_from_irq(irq);

	if (VALID_EVTCHN(evtchn))
		notify_remote_via_evtchn(evtchn);
}
EXPORT_SYMBOL_GPL(notify_remote_via_irq);

static void mask_evtchn(int port)
{
	struct shared_info *s = HYPERVISOR_shared_info;
	sync_set_bit(port, &s->evtchn_mask[0]);
}

static void unmask_evtchn(int port)
{
	struct shared_info *s = HYPERVISOR_shared_info;
	unsigned int cpu = get_cpu();

	BUG_ON(!irqs_disabled());

	/* Slow path (hypercall) if this is a non-local port. */
	if (unlikely(cpu != cpu_from_evtchn(port))) {
		struct evtchn_unmask unmask = { .port = port };
		(void)HYPERVISOR_event_channel_op(EVTCHNOP_unmask, &unmask);
	} else {
		struct vcpu_info *vcpu_info = __get_cpu_var(xen_vcpu);

		sync_clear_bit(port, &s->evtchn_mask[0]);

		/*
		 * The following is basically the equivalent of
		 * 'hw_resend_irq'. Just like a real IO-APIC we 'lose
		 * the interrupt edge' if the channel is masked.
		 */
		if (sync_test_bit(port, &s->evtchn_pending[0]) &&
		    !sync_test_and_set_bit(port / BITS_PER_LONG,
					   &vcpu_info->evtchn_pending_sel))
			vcpu_info->evtchn_upcall_pending = 1;
	}

	put_cpu();
}

static int find_unbound_irq(void)
{
	int irq;

	/* Only allocate from dynirq range */
	for (irq = 0; irq < NR_IRQS; irq++)
		if (irq_bindcount[irq] == 0)
			break;

	if (irq == NR_IRQS)
		panic("No available IRQ to bind to: increase NR_IRQS!\n");

	return irq;
}

int bind_evtchn_to_irq(unsigned int evtchn)
{
	int irq;

	spin_lock(&irq_mapping_update_lock);

	irq = evtchn_to_irq[evtchn];

	if (irq == -1) {
		irq = find_unbound_irq();

		dynamic_irq_init(irq);
		set_irq_chip_and_handler_name(irq, &xen_dynamic_chip,
					      handle_level_irq, "event");

		evtchn_to_irq[evtchn] = irq;
		irq_info[irq] = mk_irq_info(IRQT_EVTCHN, 0, evtchn);
	}

	irq_bindcount[irq]++;

	spin_unlock(&irq_mapping_update_lock);

	return irq;
}
EXPORT_SYMBOL_GPL(bind_evtchn_to_irq);

static int bind_ipi_to_irq(unsigned int ipi, unsigned int cpu)
{
	struct evtchn_bind_ipi bind_ipi;
	int evtchn, irq;

	spin_lock(&irq_mapping_update_lock);

	irq = per_cpu(ipi_to_irq, cpu)[ipi];
	if (irq == -1) {
		irq = find_unbound_irq();
		if (irq < 0)
			goto out;

		dynamic_irq_init(irq);
		set_irq_chip_and_handler_name(irq, &xen_dynamic_chip,
					      handle_level_irq, "ipi");

		bind_ipi.vcpu = cpu;
		if (HYPERVISOR_event_channel_op(EVTCHNOP_bind_ipi,
						&bind_ipi) != 0)
			BUG();
		evtchn = bind_ipi.port;

		evtchn_to_irq[evtchn] = irq;
		irq_info[irq] = mk_irq_info(IRQT_IPI, ipi, evtchn);

		per_cpu(ipi_to_irq, cpu)[ipi] = irq;

		bind_evtchn_to_cpu(evtchn, cpu);
	}

	irq_bindcount[irq]++;

 out:
	spin_unlock(&irq_mapping_update_lock);
	return irq;
}


static int bind_virq_to_irq(unsigned int virq, unsigned int cpu)
{
	struct evtchn_bind_virq bind_virq;
	int evtchn, irq;

	spin_lock(&irq_mapping_update_lock);

	irq = per_cpu(virq_to_irq, cpu)[virq];

	if (irq == -1) {
		bind_virq.virq = virq;
		bind_virq.vcpu = cpu;
		if (HYPERVISOR_event_channel_op(EVTCHNOP_bind_virq,
						&bind_virq) != 0)
			BUG();
		evtchn = bind_virq.port;

		irq = find_unbound_irq();

		dynamic_irq_init(irq);
		set_irq_chip_and_handler_name(irq, &xen_dynamic_chip,
					      handle_level_irq, "virq");

		evtchn_to_irq[evtchn] = irq;
		irq_info[irq] = mk_irq_info(IRQT_VIRQ, virq, evtchn);

		per_cpu(virq_to_irq, cpu)[virq] = irq;

		bind_evtchn_to_cpu(evtchn, cpu);
	}

	irq_bindcount[irq]++;

	spin_unlock(&irq_mapping_update_lock);

	return irq;
}

static void unbind_from_irq(unsigned int irq)
{
	struct evtchn_close close;
	int evtchn = evtchn_from_irq(irq);

	spin_lock(&irq_mapping_update_lock);

	if (VALID_EVTCHN(evtchn) && (--irq_bindcount[irq] == 0)) {
		close.port = evtchn;
		if (HYPERVISOR_event_channel_op(EVTCHNOP_close, &close) != 0)
			BUG();

		switch (type_from_irq(irq)) {
		case IRQT_VIRQ:
			per_cpu(virq_to_irq, cpu_from_evtchn(evtchn))
				[index_from_irq(irq)] = -1;
			break;
		default:
			break;
		}

		/* Closed ports are implicitly re-bound to VCPU0. */
		bind_evtchn_to_cpu(evtchn, 0);

		evtchn_to_irq[evtchn] = -1;
		irq_info[irq] = IRQ_UNBOUND;

		dynamic_irq_init(irq);
	}

	spin_unlock(&irq_mapping_update_lock);
}

int bind_evtchn_to_irqhandler(unsigned int evtchn,
			      irqreturn_t (*handler)(int, void *),
			      unsigned long irqflags,
			      const char *devname, void *dev_id)
{
	unsigned int irq;
	int retval;

	irq = bind_evtchn_to_irq(evtchn);
	retval = request_irq(irq, handler, irqflags, devname, dev_id);
	if (retval != 0) {
		unbind_from_irq(irq);
		return retval;
	}

	return irq;
}
EXPORT_SYMBOL_GPL(bind_evtchn_to_irqhandler);

int bind_virq_to_irqhandler(unsigned int virq, unsigned int cpu,
			    irqreturn_t (*handler)(int, void *),
			    unsigned long irqflags, const char *devname, void *dev_id)
{
	unsigned int irq;
	int retval;

	irq = bind_virq_to_irq(virq, cpu);
	retval = request_irq(irq, handler, irqflags, devname, dev_id);
	if (retval != 0) {
		unbind_from_irq(irq);
		return retval;
	}

	return irq;
}
EXPORT_SYMBOL_GPL(bind_virq_to_irqhandler);

int bind_ipi_to_irqhandler(enum ipi_vector ipi,
			   unsigned int cpu,
			   irq_handler_t handler,
			   unsigned long irqflags,
			   const char *devname,
			   void *dev_id)
{
	int irq, retval;

	irq = bind_ipi_to_irq(ipi, cpu);
	if (irq < 0)
		return irq;

	retval = request_irq(irq, handler, irqflags, devname, dev_id);
	if (retval != 0) {
		unbind_from_irq(irq);
		return retval;
	}

	return irq;
}

void unbind_from_irqhandler(unsigned int irq, void *dev_id)
{
	free_irq(irq, dev_id);
	unbind_from_irq(irq);
}
EXPORT_SYMBOL_GPL(unbind_from_irqhandler);

void xen_send_IPI_one(unsigned int cpu, enum ipi_vector vector)
{
	int irq = per_cpu(ipi_to_irq, cpu)[vector];
	BUG_ON(irq < 0);
	notify_remote_via_irq(irq);
}


/*
 * Search the CPUs pending events bitmasks.  For each one found, map
 * the event number to an irq, and feed it into do_IRQ() for
 * handling.
 *
 * Xen uses a two-level bitmap to speed searching.  The first level is
 * a bitset of words which contain pending event bits.  The second
 * level is a bitset of pending events themselves.
 */
fastcall void xen_evtchn_do_upcall(struct pt_regs *regs)
{
	int cpu = get_cpu();
	struct shared_info *s = HYPERVISOR_shared_info;
	struct vcpu_info *vcpu_info = __get_cpu_var(xen_vcpu);
	unsigned long pending_words;

	vcpu_info->evtchn_upcall_pending = 0;

	/* NB. No need for a barrier here -- XCHG is a barrier on x86. */
	pending_words = xchg(&vcpu_info->evtchn_pending_sel, 0);
	while (pending_words != 0) {
		unsigned long pending_bits;
		int word_idx = __ffs(pending_words);
		pending_words &= ~(1UL << word_idx);

		while ((pending_bits = active_evtchns(cpu, s, word_idx)) != 0) {
			int bit_idx = __ffs(pending_bits);
			int port = (word_idx * BITS_PER_LONG) + bit_idx;
			int irq = evtchn_to_irq[port];

			if (irq != -1) {
				regs->orig_eax = ~irq;
				do_IRQ(regs);
			}
		}
	}

	put_cpu();
}

/* Rebind an evtchn so that it gets delivered to a specific cpu */
static void rebind_irq_to_cpu(unsigned irq, unsigned tcpu)
{
	struct evtchn_bind_vcpu bind_vcpu;
	int evtchn = evtchn_from_irq(irq);

	if (!VALID_EVTCHN(evtchn))
		return;

	/* Send future instances of this interrupt to other vcpu. */
	bind_vcpu.port = evtchn;
	bind_vcpu.vcpu = tcpu;

	/*
	 * If this fails, it usually just indicates that we're dealing with a
	 * virq or IPI channel, which don't actually need to be rebound. Ignore
	 * it, but don't do the xenlinux-level rebind in that case.
	 */
	if (HYPERVISOR_event_channel_op(EVTCHNOP_bind_vcpu, &bind_vcpu) >= 0)
		bind_evtchn_to_cpu(evtchn, tcpu);
}


static void set_affinity_irq(unsigned irq, cpumask_t dest)
{
	unsigned tcpu = first_cpu(dest);
	rebind_irq_to_cpu(irq, tcpu);
}

static void enable_dynirq(unsigned int irq)
{
	int evtchn = evtchn_from_irq(irq);

	if (VALID_EVTCHN(evtchn))
		unmask_evtchn(evtchn);
}

static void disable_dynirq(unsigned int irq)
{
	int evtchn = evtchn_from_irq(irq);

	if (VALID_EVTCHN(evtchn))
		mask_evtchn(evtchn);
}

static void ack_dynirq(unsigned int irq)
{
	int evtchn = evtchn_from_irq(irq);

	move_native_irq(irq);

	if (VALID_EVTCHN(evtchn))
		clear_evtchn(evtchn);
}

static int retrigger_dynirq(unsigned int irq)
{
	int evtchn = evtchn_from_irq(irq);
	int ret = 0;

	if (VALID_EVTCHN(evtchn)) {
		set_evtchn(evtchn);
		ret = 1;
	}

	return ret;
}

static struct irq_chip xen_dynamic_chip __read_mostly = {
	.name		= "xen-dyn",
	.mask		= disable_dynirq,
	.unmask		= enable_dynirq,
	.ack		= ack_dynirq,
	.set_affinity	= set_affinity_irq,
	.retrigger	= retrigger_dynirq,
};

void __init xen_init_IRQ(void)
{
	int i;

	init_evtchn_cpu_bindings();

	/* No event channels are 'live' right now. */
	for (i = 0; i < NR_EVENT_CHANNELS; i++)
		mask_evtchn(i);

	/* Dynamic IRQ space is currently unbound. Zero the refcnts. */
	for (i = 0; i < NR_IRQS; i++)
		irq_bindcount[i] = 0;

	irq_ctx_init(smp_processor_id());
}
