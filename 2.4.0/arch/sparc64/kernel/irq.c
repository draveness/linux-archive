/* $Id: irq.c,v 1.94 2000/09/21 06:27:10 anton Exp $
 * irq.c: UltraSparc IRQ handling/init/registry.
 *
 * Copyright (C) 1997  David S. Miller  (davem@caip.rutgers.edu)
 * Copyright (C) 1998  Eddie C. Dost    (ecd@skynet.be)
 * Copyright (C) 1998  Jakub Jelinek    (jj@ultra.linux.cz)
 */

#include <linux/config.h>
#include <linux/ptrace.h>
#include <linux/errno.h>
#include <linux/kernel_stat.h>
#include <linux/signal.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/malloc.h>
#include <linux/random.h> /* XXX ADD add_foo_randomness() calls... -DaveM */
#include <linux/init.h>
#include <linux/delay.h>

#include <asm/ptrace.h>
#include <asm/processor.h>
#include <asm/atomic.h>
#include <asm/system.h>
#include <asm/irq.h>
#include <asm/sbus.h>
#include <asm/iommu.h>
#include <asm/upa.h>
#include <asm/oplib.h>
#include <asm/timer.h>
#include <asm/smp.h>
#include <asm/hardirq.h>
#include <asm/softirq.h>
#include <asm/starfire.h>

/* Internal flag, should not be visible elsewhere at all. */
#define SA_IMAP_MASKED		0x100
#define SA_DMA_SYNC		0x200

#ifdef CONFIG_SMP
static void distribute_irqs(void);
#endif

/* UPA nodes send interrupt packet to UltraSparc with first data reg
 * value low 5 (7 on Starfire) bits holding the IRQ identifier being
 * delivered.  We must translate this into a non-vector IRQ so we can
 * set the softint on this cpu.
 *
 * To make processing these packets efficient and race free we use
 * an array of irq buckets below.  The interrupt vector handler in
 * entry.S feeds incoming packets into per-cpu pil-indexed lists.
 * The IVEC handler does not need to act atomically, the PIL dispatch
 * code uses CAS to get an atomic snapshot of the list and clear it
 * at the same time.
 */

struct ino_bucket ivector_table[NUM_IVECS] __attribute__ ((aligned (64)));

#ifndef CONFIG_SMP
unsigned int __up_workvec[16] __attribute__ ((aligned (64)));
#define irq_work(__cpu, __pil)	&(__up_workvec[(void)(__cpu), (__pil)])
#else
#define irq_work(__cpu, __pil)	&(cpu_data[(__cpu)].irq_worklists[(__pil)])
#endif

#ifdef CONFIG_PCI
/* This is a table of physical addresses used to deal with SA_DMA_SYNC.
 * It is used for PCI only to synchronize DMA transfers with IRQ delivery
 * for devices behind busses other than APB on Sabre systems.
 *
 * Currently these physical addresses are just config space accesses
 * to the command register for that device.
 */
unsigned long pci_dma_wsync;
unsigned long dma_sync_reg_table[256];
unsigned char dma_sync_reg_table_entry = 0;
#endif

/* This is based upon code in the 32-bit Sparc kernel written mostly by
 * David Redman (djhr@tadpole.co.uk).
 */
#define MAX_STATIC_ALLOC	4
static struct irqaction static_irqaction[MAX_STATIC_ALLOC];
static int static_irq_count = 0;

/* This is exported so that fast IRQ handlers can get at it... -DaveM */
struct irqaction *irq_action[NR_IRQS+1] = {
	  NULL, NULL, NULL, NULL, NULL, NULL , NULL, NULL,
	  NULL, NULL, NULL, NULL, NULL, NULL , NULL, NULL
};

int get_irq_list(char *buf)
{
	int i, len = 0;
	struct irqaction *action;
#ifdef CONFIG_SMP
	int j;
#endif

	for(i = 0; i < (NR_IRQS + 1); i++) {
		if(!(action = *(i + irq_action)))
			continue;
		len += sprintf(buf + len, "%3d: ", i);
#ifndef CONFIG_SMP
		len += sprintf(buf + len, "%10u ", kstat_irqs(i));
#else
		for (j = 0; j < smp_num_cpus; j++)
			len += sprintf(buf + len, "%10u ",
				       kstat.irqs[cpu_logical_map(j)][i]);
#endif
		len += sprintf(buf + len, "%c %s",
			       (action->flags & SA_INTERRUPT) ? '+' : ' ',
			       action->name);
		for(action = action->next; action; action = action->next) {
			len += sprintf(buf+len, ",%s %s",
				       (action->flags & SA_INTERRUPT) ? " +" : "",
				       action->name);
		}
		len += sprintf(buf + len, "\n");
	}
	return len;
}

/* Now these are always passed a true fully specified sun4u INO. */
void enable_irq(unsigned int irq)
{
	struct ino_bucket *bucket = __bucket(irq);
	unsigned long imap;
	unsigned long tid;

	imap = bucket->imap;
	if (imap == 0UL)
		return;

	if(this_is_starfire == 0) {
		/* We set it to our UPA MID. */
		__asm__ __volatile__("ldxa [%%g0] %1, %0"
				     : "=r" (tid)
				     : "i" (ASI_UPA_CONFIG));
		tid = ((tid & UPA_CONFIG_MID) << 9);
	} else {
		tid = (starfire_translate(imap, current->processor) << 26);
	}

	/* NOTE NOTE NOTE, IGN and INO are read-only, IGN is a product
	 * of this SYSIO's preconfigured IGN in the SYSIO Control
	 * Register, the hardware just mirrors that value here.
	 * However for Graphics and UPA Slave devices the full
	 * IMAP_INR field can be set by the programmer here.
	 *
	 * Things like FFB can now be handled via the new IRQ mechanism.
	 */
	upa_writel(IMAP_VALID | (tid & IMAP_TID), imap);
}

/* This now gets passed true ino's as well. */
void disable_irq(unsigned int irq)
{
	struct ino_bucket *bucket = __bucket(irq);
	unsigned long imap;

	imap = bucket->imap;
	if (imap != 0UL) {
		u32 tmp;

		/* NOTE: We do not want to futz with the IRQ clear registers
		 *       and move the state to IDLE, the SCSI code does call
		 *       disable_irq() to assure atomicity in the queue cmd
		 *       SCSI adapter driver code.  Thus we'd lose interrupts.
		 */
		tmp = upa_readl(imap);
		tmp &= ~IMAP_VALID;
		upa_writel(tmp, imap);
	}
}

/* The timer is the one "weird" interrupt which is generated by
 * the CPU %tick register and not by some normal vectored interrupt
 * source.  To handle this special case, we use this dummy INO bucket.
 */
static struct ino_bucket pil0_dummy_bucket = {
	0,	/* irq_chain */
	0,	/* pil */
	0,	/* pending */
	0,	/* flags */
	0,	/* __unused */
	NULL,	/* irq_info */
	0UL,	/* iclr */
	0UL,	/* imap */
};

unsigned int build_irq(int pil, int inofixup, unsigned long iclr, unsigned long imap)
{
	struct ino_bucket *bucket;
	int ino;

	if(pil == 0) {
		if(iclr != 0UL || imap != 0UL) {
			prom_printf("Invalid dummy bucket for PIL0 (%lx:%lx)\n",
				    iclr, imap);
			prom_halt();
		}
		return __irq(&pil0_dummy_bucket);
	}

	/* RULE: Both must be specified in all other cases. */
	if (iclr == 0UL || imap == 0UL) {
		prom_printf("Invalid build_irq %d %d %016lx %016lx\n",
			    pil, inofixup, iclr, imap);
		prom_halt();
	}
	
	ino = (upa_readl(imap) & (IMAP_IGN | IMAP_INO)) + inofixup;
	if(ino > NUM_IVECS) {
		prom_printf("Invalid INO %04x (%d:%d:%016lx:%016lx)\n",
			    ino, pil, inofixup, iclr, imap);
		prom_halt();
	}

	/* Ok, looks good, set it up.  Don't touch the irq_chain or
	 * the pending flag.
	 */
	bucket = &ivector_table[ino];
	if ((bucket->flags & IBF_ACTIVE) ||
	    (bucket->irq_info != NULL)) {
		/* This is a gross fatal error if it happens here. */
		prom_printf("IRQ: Trying to reinit INO bucket, fatal error.\n");
		prom_printf("IRQ: Request INO %04x (%d:%d:%016lx:%016lx)\n",
			    ino, pil, inofixup, iclr, imap);
		prom_printf("IRQ: Existing (%d:%016lx:%016lx)\n",
			    bucket->pil, bucket->iclr, bucket->imap);
		prom_printf("IRQ: Cannot continue, halting...\n");
		prom_halt();
	}
	bucket->imap  = imap;
	bucket->iclr  = iclr;
	bucket->pil   = pil;
	bucket->flags = 0;

	bucket->irq_info = NULL;

	return __irq(bucket);
}

static void atomic_bucket_insert(struct ino_bucket *bucket)
{
	unsigned long pstate;
	unsigned int *ent;

	__asm__ __volatile__("rdpr %%pstate, %0" : "=r" (pstate));
	__asm__ __volatile__("wrpr %0, %1, %%pstate"
			     : : "r" (pstate), "i" (PSTATE_IE));
	ent = irq_work(smp_processor_id(), bucket->pil);
	bucket->irq_chain = *ent;
	*ent = __irq(bucket);
	__asm__ __volatile__("wrpr %0, 0x0, %%pstate" : : "r" (pstate));
}

int request_irq(unsigned int irq, void (*handler)(int, void *, struct pt_regs *),
		unsigned long irqflags, const char *name, void *dev_id)
{
	struct irqaction *action, *tmp = NULL;
	struct ino_bucket *bucket = __bucket(irq);
	unsigned long flags;
	int pending = 0;

	if ((bucket != &pil0_dummy_bucket) &&
	    (bucket < &ivector_table[0] ||
	     bucket >= &ivector_table[NUM_IVECS])) {
		unsigned int *caller;

		__asm__ __volatile__("mov %%i7, %0" : "=r" (caller));
		printk(KERN_CRIT "request_irq: Old style IRQ registry attempt "
		       "from %p, irq %08x.\n", caller, irq);
		return -EINVAL;
	}	
	if(!handler)
	    return -EINVAL;

	if (!bucket->pil)
		irqflags &= ~SA_IMAP_MASKED;
	else {
		irqflags |= SA_IMAP_MASKED;
		if (bucket->flags & IBF_PCI) {
			/*
			 * PCI IRQs should never use SA_INTERRUPT.
			 */
			irqflags &= ~(SA_INTERRUPT);

			/*
			 * Check wether we _should_ use DMA Write Sync
			 * (for devices behind bridges behind APB). 
			 */
			if (bucket->flags & IBF_DMA_SYNC)
				irqflags |= SA_DMA_SYNC;
		}
	}

	save_and_cli(flags);

	action = *(bucket->pil + irq_action);
	if(action) {
		if((action->flags & SA_SHIRQ) && (irqflags & SA_SHIRQ))
			for (tmp = action; tmp->next; tmp = tmp->next)
				;
		else {
			restore_flags(flags);
			return -EBUSY;
		}
		if((action->flags & SA_INTERRUPT) ^ (irqflags & SA_INTERRUPT)) {
			printk("Attempt to mix fast and slow interrupts on IRQ%d "
			       "denied\n", bucket->pil);
			restore_flags(flags);
			return -EBUSY;
		}   
		action = NULL;		/* Or else! */
	}

	/* If this is flagged as statically allocated then we use our
	 * private struct which is never freed.
	 */
	if(irqflags & SA_STATIC_ALLOC) {
	    if(static_irq_count < MAX_STATIC_ALLOC)
		action = &static_irqaction[static_irq_count++];
	    else
		printk("Request for IRQ%d (%s) SA_STATIC_ALLOC failed "
		       "using kmalloc\n", irq, name);
	}	
	if(action == NULL)
	    action = (struct irqaction *)kmalloc(sizeof(struct irqaction),
						 GFP_KERNEL);
	
	if(!action) { 
		restore_flags(flags);
		return -ENOMEM;
	}

	if ((irqflags & SA_IMAP_MASKED) == 0) {
		bucket->irq_info = action;
		bucket->flags |= IBF_ACTIVE;
	} else {
		if((bucket->flags & IBF_ACTIVE) != 0) {
			void *orig = bucket->irq_info;
			void **vector = NULL;

			if((bucket->flags & IBF_PCI) == 0) {
				printk("IRQ: Trying to share non-PCI bucket.\n");
				goto free_and_ebusy;
			}
			if((bucket->flags & IBF_MULTI) == 0) {
				vector = kmalloc(sizeof(void *) * 4, GFP_KERNEL);
				if(vector == NULL)
					goto free_and_enomem;

				/* We might have slept. */
				if ((bucket->flags & IBF_MULTI) != 0) {
					int ent;

					kfree(vector);
					vector = (void **)bucket->irq_info;
					for(ent = 0; ent < 4; ent++) {
						if (vector[ent] == NULL) {
							vector[ent] = action;
							break;
						}
					}
					if (ent == 4)
						goto free_and_ebusy;
				} else {
					vector[0] = orig;
					vector[1] = action;
					vector[2] = NULL;
					vector[3] = NULL;
					bucket->irq_info = vector;
					bucket->flags |= IBF_MULTI;
				}
			} else {
				int ent;

				vector = (void **)orig;
				for(ent = 0; ent < 4; ent++) {
					if(vector[ent] == NULL) {
						vector[ent] = action;
						break;
					}
				}
				if (ent == 4)
					goto free_and_ebusy;
			}
		} else {
			bucket->irq_info = action;
			bucket->flags |= IBF_ACTIVE;
		}
		pending = bucket->pending;
		if(pending)
			bucket->pending = 0;
	}

	action->mask = (unsigned long) bucket;
	action->handler = handler;
	action->flags = irqflags;
	action->name = name;
	action->next = NULL;
	action->dev_id = dev_id;

	if(tmp)
		tmp->next = action;
	else
		*(bucket->pil + irq_action) = action;

	enable_irq(irq);

	/* We ate the IVEC already, this makes sure it does not get lost. */
	if(pending) {
		atomic_bucket_insert(bucket);
		set_softint(1 << bucket->pil);
	}
	restore_flags(flags);

#ifdef CONFIG_SMP
	distribute_irqs();
#endif
	return 0;

free_and_ebusy:
	kfree(action);
	restore_flags(flags);
	return -EBUSY;

free_and_enomem:
	kfree(action);
	restore_flags(flags);
	return -ENOMEM;
}

void free_irq(unsigned int irq, void *dev_id)
{
	struct irqaction *action;
	struct irqaction *tmp = NULL;
	unsigned long flags;
	struct ino_bucket *bucket = __bucket(irq), *bp;

	if ((bucket != &pil0_dummy_bucket) &&
	    (bucket < &ivector_table[0] ||
	     bucket >= &ivector_table[NUM_IVECS])) {
		unsigned int *caller;

		__asm__ __volatile__("mov %%i7, %0" : "=r" (caller));
		printk(KERN_CRIT "free_irq: Old style IRQ removal attempt "
		       "from %p, irq %08x.\n", caller, irq);
		return;
	}
	
	action = *(bucket->pil + irq_action);
	if(!action->handler) {
		printk("Freeing free IRQ %d\n", bucket->pil);
		return;
	}
	if(dev_id) {
		for( ; action; action = action->next) {
			if(action->dev_id == dev_id)
				break;
			tmp = action;
		}
		if(!action) {
			printk("Trying to free free shared IRQ %d\n", bucket->pil);
			return;
		}
	} else if(action->flags & SA_SHIRQ) {
		printk("Trying to free shared IRQ %d with NULL device ID\n", bucket->pil);
		return;
	}

	if(action->flags & SA_STATIC_ALLOC) {
		printk("Attempt to free statically allocated IRQ %d (%s)\n",
		       bucket->pil, action->name);
		return;
	}

	save_and_cli(flags);
	if(action && tmp)
		tmp->next = action->next;
	else
		*(bucket->pil + irq_action) = action->next;

	if(action->flags & SA_IMAP_MASKED) {
		unsigned long imap = bucket->imap;
		void **vector, *orig;
		int ent;

		orig = bucket->irq_info;
		vector = (void **)orig;

		if ((bucket->flags & IBF_MULTI) != 0) {
			int other = 0;
			void *orphan = NULL;
			for(ent = 0; ent < 4; ent++) {
				if(vector[ent] == action)
					vector[ent] = NULL;
				else if(vector[ent] != NULL) {
					orphan = vector[ent];
					other++;
				}
			}

			/* Only free when no other shared irq
			 * uses this bucket.
			 */
			if(other) {
				if (other == 1) {
					/* Convert back to non-shared bucket. */
					bucket->irq_info = orphan;
					bucket->flags &= ~(IBF_MULTI);
					kfree(vector);
				}
				goto out;
			}
		} else {
			bucket->irq_info = NULL;
		}

		/* This unique interrupt source is now inactive. */
		bucket->flags &= ~IBF_ACTIVE;

		/* See if any other buckets share this bucket's IMAP
		 * and are still active.
		 */
		for(ent = 0; ent < NUM_IVECS; ent++) {
			bp = &ivector_table[ent];
			if(bp != bucket		&&
			   bp->imap == imap	&&
			   (bp->flags & IBF_ACTIVE) != 0)
				break;
		}

		/* Only disable when no other sub-irq levels of
		 * the same IMAP are active.
		 */
		if (ent == NUM_IVECS)
			disable_irq(irq);
	}

out:
	kfree(action);
	restore_flags(flags);
}

#ifdef CONFIG_SMP

/* Who has the global irq brlock */
unsigned char global_irq_holder = NO_PROC_ID;

static void show(char * str)
{
	int cpu = smp_processor_id();
	int i;

	printk("\n%s, CPU %d:\n", str, cpu);
	printk("irq:  %d [ ", irqs_running());
	for (i = 0; i < smp_num_cpus; i++)
		printk("%u ", __brlock_array[i][BR_GLOBALIRQ_LOCK]);
	printk("]\nbh:   %d [ ",
	       (spin_is_locked(&global_bh_lock) ? 1 : 0));
	for (i = 0; i < smp_num_cpus; i++)
		printk("%u ", local_bh_count(i));
	printk("]\n");
}

#define MAXCOUNT 100000000

#if 0
#define SYNC_OTHER_ULTRAS(x)	udelay(x+1)
#else
#define SYNC_OTHER_ULTRAS(x)	membar("#Sync");
#endif

void synchronize_irq(void)
{
	if (irqs_running()) {
		cli();
		sti();
	}
}

static inline void get_irqlock(int cpu)
{
	int count;

	if ((unsigned char)cpu == global_irq_holder)
		return;

	count = MAXCOUNT;
again:
	br_write_lock(BR_GLOBALIRQ_LOCK);
	for (;;) {
		spinlock_t *lock;

		if (!irqs_running() &&
		    (local_bh_count(smp_processor_id()) || !spin_is_locked(&global_bh_lock)))
			break;

		br_write_unlock(BR_GLOBALIRQ_LOCK);
		lock = &__br_write_locks[BR_GLOBALIRQ_LOCK].lock;
		while (irqs_running() ||
		       spin_is_locked(lock) ||
		       (!local_bh_count(smp_processor_id()) && spin_is_locked(&global_bh_lock))) {
			if (!--count) {
				show("get_irqlock");
				count = (~0 >> 1);
			}
			__sti();
			SYNC_OTHER_ULTRAS(cpu);
			__cli();
		}
		goto again;
	}

	global_irq_holder = cpu;
}

void __global_cli(void)
{
	unsigned long flags;

	__save_flags(flags);
	if(flags == 0) {
		int cpu = smp_processor_id();
		__cli();
		if (! local_irq_count(cpu))
			get_irqlock(cpu);
	}
}

void __global_sti(void)
{
	int cpu = smp_processor_id();

	if (! local_irq_count(cpu))
		release_irqlock(cpu);
	__sti();
}

unsigned long __global_save_flags(void)
{
	unsigned long flags, local_enabled, retval;

	__save_flags(flags);
	local_enabled = ((flags == 0) ? 1 : 0);
	retval = 2 + local_enabled;
	if (! local_irq_count(smp_processor_id())) {
		if (local_enabled)
			retval = 1;
		if (global_irq_holder == (unsigned char) smp_processor_id())
			retval = 0;
	}
	return retval;
}

void __global_restore_flags(unsigned long flags)
{
	switch (flags) {
	case 0:
		__global_cli();
		break;
	case 1:
		__global_sti();
		break;
	case 2:
		__cli();
		break;
	case 3:
		__sti();
		break;
	default:
	{
		unsigned long pc;
		__asm__ __volatile__("mov %%i7, %0" : "=r" (pc));
		printk("global_restore_flags: Bogon flags(%016lx) caller %016lx\n",
		       flags, pc);
	}
	}
}

#endif /* CONFIG_SMP */

void catch_disabled_ivec(struct pt_regs *regs)
{
	int cpu = smp_processor_id();
	struct ino_bucket *bucket = __bucket(*irq_work(cpu, 0));

	/* We can actually see this on Ultra/PCI PCI cards, which are bridges
	 * to other devices.  Here a single IMAP enabled potentially multiple
	 * unique interrupt sources (which each do have a unique ICLR register.
	 *
	 * So what we do is just register that the IVEC arrived, when registered
	 * for real the request_irq() code will check the bit and signal
	 * a local CPU interrupt for it.
	 */
#if 0
	printk("IVEC: Spurious interrupt vector (%x) received at (%016lx)\n",
	       bucket - &ivector_table[0], regs->tpc);
#endif
	*irq_work(cpu, 0) = 0;
	bucket->pending = 1;
}

/* Tune this... */
#define FORWARD_VOLUME		12

void handler_irq(int irq, struct pt_regs *regs)
{
	struct ino_bucket *bp, *nbp;
	int cpu = smp_processor_id();
#ifdef CONFIG_SMP
	int should_forward = (this_is_starfire == 0	&&
			      irq < 10			&&
			      current->pid != 0);
	unsigned int buddy = 0;

	/* 'cpu' is the MID (ie. UPAID), calculate the MID
	 * of our buddy.
	 */
	if (should_forward != 0) {
		buddy = cpu_number_map(cpu) + 1;
		if (buddy >= NR_CPUS ||
		    (buddy = cpu_logical_map(buddy)) == -1)
			buddy = cpu_logical_map(0);

		/* Voo-doo programming. */
		if (cpu_data[buddy].idle_volume < FORWARD_VOLUME)
			should_forward = 0;
		buddy <<= 26;
	}
#endif

#ifndef CONFIG_SMP
	/*
	 * Check for TICK_INT on level 14 softint.
	 */
	if ((irq == 14) && (get_softint() & (1UL << 0)))
		irq = 0;
#endif
	clear_softint(1 << irq);

	irq_enter(cpu, irq);
	kstat.irqs[cpu][irq]++;

	/* Sliiiick... */
#ifndef CONFIG_SMP
	bp = ((irq != 0) ?
	      __bucket(xchg32(irq_work(cpu, irq), 0)) :
	      &pil0_dummy_bucket);
#else
	bp = __bucket(xchg32(irq_work(cpu, irq), 0));
#endif
	for ( ; bp != NULL; bp = nbp) {
		unsigned char flags = bp->flags;

		nbp = __bucket(bp->irq_chain);
		if ((flags & IBF_ACTIVE) != 0) {
#ifdef CONFIG_PCI
			if ((flags & IBF_DMA_SYNC) != 0) {
				upa_readl(dma_sync_reg_table[bp->synctab_ent]);
				upa_readq(pci_dma_wsync);
			}
#endif
			if ((flags & IBF_MULTI) == 0) {
				struct irqaction *ap = bp->irq_info;
				ap->handler(__irq(bp), ap->dev_id, regs);
			} else {
				void **vector = (void **)bp->irq_info;
				int ent;
				for (ent = 0; ent < 4; ent++) {
					struct irqaction *ap = vector[ent];
					if (ap != NULL)
						ap->handler(__irq(bp), ap->dev_id, regs);
				}
			}
			/* Only the dummy bucket lacks IMAP/ICLR. */
			if (bp->pil != 0) {
#ifdef CONFIG_SMP
				/* Ok, here is what is going on:
				 * 1) Retargeting IRQs on Starfire is very
				 *    expensive so just forget about it on them.
				 * 2) Moving around very high priority interrupts
				 *    is a losing game.
				 * 3) If the current cpu is idle, interrupts are
				 *    useful work, so keep them here.  But do not
				 *    pass to our neighbour if he is not very idle.
				 */
				if (should_forward != 0) {
					/* Push it to our buddy. */
					should_forward = 0;
					upa_writel(buddy | IMAP_VALID, bp->imap);
				}
#endif
				upa_writel(ICLR_IDLE, bp->iclr);
			}
		} else
			bp->pending = 1;
	}
	irq_exit(cpu, irq);
}

#ifdef CONFIG_BLK_DEV_FD
extern void floppy_interrupt(int irq, void *dev_cookie, struct pt_regs *regs);

void sparc_floppy_irq(int irq, void *dev_cookie, struct pt_regs *regs)
{
	struct irqaction *action = *(irq + irq_action);
	struct ino_bucket *bucket;
	int cpu = smp_processor_id();

	irq_enter(cpu, irq);
	kstat.irqs[cpu][irq]++;

	*(irq_work(cpu, irq)) = 0;
	bucket = (struct ino_bucket *)action->mask;

	floppy_interrupt(irq, dev_cookie, regs);
	upa_writel(ICLR_IDLE, bucket->iclr);

	irq_exit(cpu, irq);
}
#endif

/* The following assumes that the branch lies before the place we
 * are branching to.  This is the case for a trap vector...
 * You have been warned.
 */
#define SPARC_BRANCH(dest_addr, inst_addr) \
          (0x10800000 | ((((dest_addr)-(inst_addr))>>2)&0x3fffff))

#define SPARC_NOP (0x01000000)

static void install_fast_irq(unsigned int cpu_irq,
			     void (*handler)(int, void *, struct pt_regs *))
{
	extern unsigned long sparc64_ttable_tl0;
	unsigned long ttent = (unsigned long) &sparc64_ttable_tl0;
	unsigned int *insns;

	ttent += 0x820;
	ttent += (cpu_irq - 1) << 5;
	insns = (unsigned int *) ttent;
	insns[0] = SPARC_BRANCH(((unsigned long) handler),
				((unsigned long)&insns[0]));
	insns[1] = SPARC_NOP;
	__asm__ __volatile__("membar #StoreStore; flush %0" : : "r" (ttent));
}

int request_fast_irq(unsigned int irq,
		     void (*handler)(int, void *, struct pt_regs *),
		     unsigned long irqflags, const char *name, void *dev_id)
{
	struct irqaction *action;
	struct ino_bucket *bucket = __bucket(irq);
	unsigned long flags;

	/* No pil0 dummy buckets allowed here. */
	if (bucket < &ivector_table[0] ||
	    bucket >= &ivector_table[NUM_IVECS]) {
		unsigned int *caller;

		__asm__ __volatile__("mov %%i7, %0" : "=r" (caller));
		printk(KERN_CRIT "request_fast_irq: Old style IRQ registry attempt "
		       "from %p, irq %08x.\n", caller, irq);
		return -EINVAL;
	}	
	
	/* Only IMAP style interrupts can be registered as fast. */
	if(bucket->pil == 0)
		return -EINVAL;

	if(!handler)
		return -EINVAL;

	if ((bucket->pil == 0) || (bucket->pil == 14)) {
		printk("request_fast_irq: Trying to register shared IRQ 0 or 14.\n");
		return -EBUSY;
	}

	action = *(bucket->pil + irq_action);
	if(action) {
		if(action->flags & SA_SHIRQ)
			panic("Trying to register fast irq when already shared.\n");
		if(irqflags & SA_SHIRQ)
			panic("Trying to register fast irq as shared.\n");
		printk("request_fast_irq: Trying to register yet already owned.\n");
		return -EBUSY;
	}

	save_and_cli(flags);
	if(irqflags & SA_STATIC_ALLOC) {
		if(static_irq_count < MAX_STATIC_ALLOC)
			action = &static_irqaction[static_irq_count++];
		else
			printk("Request for IRQ%d (%s) SA_STATIC_ALLOC failed "
			       "using kmalloc\n", bucket->pil, name);
	}
	if(action == NULL)
		action = (struct irqaction *)kmalloc(sizeof(struct irqaction),
						     GFP_KERNEL);
	if(!action) {
		restore_flags(flags);
		return -ENOMEM;
	}
	install_fast_irq(bucket->pil, handler);

	bucket->irq_info = action;
	bucket->flags |= IBF_ACTIVE;

	action->mask = (unsigned long) bucket;
	action->handler = handler;
	action->flags = irqflags | SA_IMAP_MASKED;
	action->dev_id = NULL;
	action->name = name;
	action->next = NULL;

	*(bucket->pil + irq_action) = action;
	enable_irq(irq);

	restore_flags(flags);

#ifdef CONFIG_SMP
	distribute_irqs();
#endif
	return 0;
}

/* We really don't need these at all on the Sparc.  We only have
 * stubs here because they are exported to modules.
 */
unsigned long probe_irq_on(void)
{
	return 0;
}

int probe_irq_off(unsigned long mask)
{
	return 0;
}

/* This is gets the master TICK_INT timer going. */
void init_timers(void (*cfunc)(int, void *, struct pt_regs *),
		 unsigned long *clock)
{
	unsigned long pstate;
	extern unsigned long timer_tick_offset;
	int node, err;
#ifdef CONFIG_SMP
	extern void smp_tick_init(void);
#endif

	node = linux_cpus[0].prom_node;
	*clock = prom_getint(node, "clock-frequency");
	timer_tick_offset = *clock / HZ;
#ifdef CONFIG_SMP
	smp_tick_init();
#endif

	/* Register IRQ handler. */
	err = request_irq(build_irq(0, 0, 0UL, 0UL), cfunc, (SA_INTERRUPT | SA_STATIC_ALLOC),
			  "timer", NULL);

	if(err) {
		prom_printf("Serious problem, cannot register TICK_INT\n");
		prom_halt();
	}

	/* Guarentee that the following sequences execute
	 * uninterrupted.
	 */
	__asm__ __volatile__("rdpr	%%pstate, %0\n\t"
			     "wrpr	%0, %1, %%pstate"
			     : "=r" (pstate)
			     : "i" (PSTATE_IE));

	/* Set things up so user can access tick register for profiling
	 * purposes.  Also workaround BB_ERRATA_1 by doing a dummy
	 * read back of %tick after writing it.
	 */
	__asm__ __volatile__("
		sethi	%%hi(0x80000000), %%g1
		ba,pt	%%xcc, 1f
		 sllx	%%g1, 32, %%g1
		.align	64
	1:	rd	%%tick, %%g2
		add	%%g2, 6, %%g2
		andn	%%g2, %%g1, %%g2
		wrpr	%%g2, 0, %%tick
		rdpr	%%tick, %%g0"
	: /* no outputs */
	: /* no inputs */
	: "g1", "g2");

	/* Workaround for Spitfire Errata (#54 I think??), I discovered
	 * this via Sun BugID 4008234, mentioned in Solaris-2.5.1 patch
	 * number 103640.
	 *
	 * On Blackbird writes to %tick_cmpr can fail, the
	 * workaround seems to be to execute the wr instruction
	 * at the start of an I-cache line, and perform a dummy
	 * read back from %tick_cmpr right after writing to it. -DaveM
	 */
	__asm__ __volatile__("
		rd	%%tick, %%g1
		ba,pt	%%xcc, 1f
		 add	%%g1, %0, %%g1
		.align	64
	1:	wr	%%g1, 0x0, %%tick_cmpr
		rd	%%tick_cmpr, %%g0"
	: /* no outputs */
	: "r" (timer_tick_offset)
	: "g1");

	/* Restore PSTATE_IE. */
	__asm__ __volatile__("wrpr	%0, 0x0, %%pstate"
			     : /* no outputs */
			     : "r" (pstate));

	sti();
}

#ifdef CONFIG_SMP
static int retarget_one_irq(struct irqaction *p, int goal_cpu)
{
	struct ino_bucket *bucket = __bucket(p->mask);
	unsigned long imap = bucket->imap;
	unsigned int tid;

	/* Never change this, it causes problems on Ex000 systems. */
	if (bucket->pil == 12)
		return goal_cpu;

	if(this_is_starfire == 0) {
		tid = __cpu_logical_map[goal_cpu] << 26;
	} else {
		tid = (starfire_translate(imap, __cpu_logical_map[goal_cpu]) << 26);
	}
	upa_writel(IMAP_VALID | (tid & IMAP_TID), imap);

	goal_cpu++;
	if(goal_cpu >= NR_CPUS ||
	   __cpu_logical_map[goal_cpu] == -1)
		goal_cpu = 0;
	return goal_cpu;
}

/* Called from request_irq. */
static void distribute_irqs(void)
{
	unsigned long flags;
	int cpu, level;

	save_and_cli(flags);
	cpu = 0;
	for(level = 0; level < NR_IRQS; level++) {
		struct irqaction *p = irq_action[level];
		while(p) {
			if(p->flags & SA_IMAP_MASKED)
				cpu = retarget_one_irq(p, cpu);
			p = p->next;
		}
	}
	restore_flags(flags);
}
#endif


struct sun5_timer *prom_timers;
static u64 prom_limit0, prom_limit1;

static void map_prom_timers(void)
{
	unsigned int addr[3];
	int tnode, err;

	/* PROM timer node hangs out in the top level of device siblings... */
	tnode = prom_finddevice("/counter-timer");

	/* Assume if node is not present, PROM uses different tick mechanism
	 * which we should not care about.
	 */
	if(tnode == 0 || tnode == -1) {
		prom_timers = (struct sun5_timer *) 0;
		return;
	}

	/* If PROM is really using this, it must be mapped by him. */
	err = prom_getproperty(tnode, "address", (char *)addr, sizeof(addr));
	if(err == -1) {
		prom_printf("PROM does not have timer mapped, trying to continue.\n");
		prom_timers = (struct sun5_timer *) 0;
		return;
	}
	prom_timers = (struct sun5_timer *) ((unsigned long)addr[0]);
}

static void kill_prom_timer(void)
{
	if(!prom_timers)
		return;

	/* Save them away for later. */
	prom_limit0 = prom_timers->limit0;
	prom_limit1 = prom_timers->limit1;

	/* Just as in sun4c/sun4m PROM uses timer which ticks at IRQ 14.
	 * We turn both off here just to be paranoid.
	 */
	prom_timers->limit0 = 0;
	prom_timers->limit1 = 0;

	/* Wheee, eat the interrupt packet too... */
	__asm__ __volatile__("
	mov	0x40, %%g2
	ldxa	[%%g0] %0, %%g1
	ldxa	[%%g2] %1, %%g1
	stxa	%%g0, [%%g0] %0
	membar	#Sync
"	: /* no outputs */
	: "i" (ASI_INTR_RECEIVE), "i" (ASI_UDB_INTR_R)
	: "g1", "g2");
}

void enable_prom_timer(void)
{
	if(!prom_timers)
		return;

	/* Set it to whatever was there before. */
	prom_timers->limit1 = prom_limit1;
	prom_timers->count1 = 0;
	prom_timers->limit0 = prom_limit0;
	prom_timers->count0 = 0;
}

void __init init_IRQ(void)
{
	static int called = 0;

	if (called == 0) {
		called = 1;
		map_prom_timers();
		kill_prom_timer();
		memset(&ivector_table[0], 0, sizeof(ivector_table));
#ifndef CONFIG_SMP
		memset(&__up_workvec[0], 0, sizeof(__up_workvec));
#endif
	}

	/* We need to clear any IRQ's pending in the soft interrupt
	 * registers, a spurious one could be left around from the
	 * PROM timer which we just disabled.
	 */
	clear_softint(get_softint());

	/* Now that ivector table is initialized, it is safe
	 * to receive IRQ vector traps.  We will normally take
	 * one or two right now, in case some device PROM used
	 * to boot us wants to speak to us.  We just ignore them.
	 */
	__asm__ __volatile__("rdpr	%%pstate, %%g1\n\t"
			     "or	%%g1, %0, %%g1\n\t"
			     "wrpr	%%g1, 0x0, %%pstate"
			     : /* No outputs */
			     : "i" (PSTATE_IE)
			     : "g1");
}

void init_irq_proc(void)
{
	/* For now, nothing... */
}
