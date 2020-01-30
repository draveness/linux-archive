/*
 * c 2001 PPC 64 Team, IBM Corp
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */
#ifndef __PPC_KERNEL_PCI_H__
#define __PPC_KERNEL_PCI_H__

#include <linux/pci.h>
#include <asm/pci-bridge.h>

extern unsigned long isa_io_base;

extern struct pci_controller* pci_alloc_pci_controller(enum phb_types controller_type);
extern struct pci_controller* pci_find_hose_for_OF_device(struct device_node* node);

extern struct pci_controller* hose_head;
extern struct pci_controller** hose_tail;

extern int global_phb_number;

/*******************************************************************
 * Platform functions that are brand specific implementation. 
 *******************************************************************/
extern unsigned long find_and_init_phbs(void);

extern struct pci_dev *ppc64_isabridge_dev;	/* may be NULL if no ISA bus */

/*******************************************************************
 * PCI device_node operations
 *******************************************************************/
struct device_node;
typedef void *(*traverse_func)(struct device_node *me, void *data);
void *traverse_pci_devices(struct device_node *start, traverse_func pre,
		void *data);

void pci_devs_phb_init(void);
void pci_fix_bus_sysdata(void);
struct device_node *fetch_dev_dn(struct pci_dev *dev);

#define PCI_GET_PHB_PTR(dev)    (((struct device_node *)(dev)->sysdata)->phb)

/* PCI address cache management routines */
void pci_addr_cache_insert_device(struct pci_dev *dev);
void pci_addr_cache_remove_device(struct pci_dev *dev);

/* From pSeries_pci.h */
void init_pci_config_tokens (void);
unsigned long get_phb_buid (struct device_node *);


#endif /* __PPC_KERNEL_PCI_H__ */
