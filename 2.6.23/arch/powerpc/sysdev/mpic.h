#ifndef _POWERPC_SYSDEV_MPIC_H
#define _POWERPC_SYSDEV_MPIC_H

/*
 * Copyright 2006-2007, Michael Ellerman, IBM Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2 of the
 * License.
 *
 */

#ifdef CONFIG_PCI_MSI
extern void mpic_msi_reserve_hwirq(struct mpic *mpic, irq_hw_number_t hwirq);
extern int mpic_msi_init_allocator(struct mpic *mpic);
extern irq_hw_number_t mpic_msi_alloc_hwirqs(struct mpic *mpic, int num);
extern void mpic_msi_free_hwirqs(struct mpic *mpic, int offset, int num);
extern int mpic_u3msi_init(struct mpic *mpic);
#else
static inline void mpic_msi_reserve_hwirq(struct mpic *mpic,
					  irq_hw_number_t hwirq)
{
	return;
}

static inline int mpic_u3msi_init(struct mpic *mpic)
{
	return -1;
}
#endif

extern int mpic_set_irq_type(unsigned int virq, unsigned int flow_type);
extern void mpic_end_irq(unsigned int irq);
extern void mpic_mask_irq(unsigned int irq);
extern void mpic_unmask_irq(unsigned int irq);

#endif /* _POWERPC_SYSDEV_MPIC_H */
