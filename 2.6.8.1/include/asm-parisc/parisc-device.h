#include <linux/device.h>

struct parisc_device {
	unsigned long   hpa;		/* Hard Physical Address */
	struct parisc_device_id id;
	struct parisc_device *parent;
	struct parisc_device *sibling;
	struct parisc_device *child;
	struct parisc_driver *driver;	/* Driver for this device */
	void		*sysdata;	/* Driver instance private data */
	char		name[80];	/* The hardware description */
	int		irq;

	char		hw_path;        /* The module number on this bus */
	unsigned int	num_addrs;	/* some devices have additional address ranges. */
	unsigned long	*addr;          /* which will be stored here */
 
#ifdef __LP64__
	/* parms for pdc_pat_cell_module() call */
	unsigned long	pcell_loc;	/* Physical Cell location */
	unsigned long	mod_index;	/* PAT specific - Misc Module info */

	/* generic info returned from pdc_pat_cell_module() */
	unsigned long	mod_info;	/* PAT specific - Misc Module info */
	unsigned long	pmod_loc;	/* physical Module location */
#endif
	u64		dma_mask;	/* DMA mask for I/O */
	struct device 	dev;
};

struct parisc_driver {
	struct parisc_driver *next;
	char *name; 
	const struct parisc_device_id *id_table;
	int (*probe) (struct parisc_device *dev); /* New device discovered */
	int (*remove) (struct parisc_device *dev);
	struct device_driver drv;
};


#define to_parisc_device(d)	container_of(d, struct parisc_device, dev)
#define to_parisc_driver(d)	container_of(d, struct parisc_driver, drv)

extern struct bus_type parisc_bus_type;
