#ifndef __LINUX_PARPORT_PC_H
#define __LINUX_PARPORT_PC_H

#include <asm/io.h>

/* --- register definitions ------------------------------- */

#define ECONTROL(p) ((p)->base_hi + 0x2)
#define CONFIGB(p)  ((p)->base_hi + 0x1)
#define CONFIGA(p)  ((p)->base_hi + 0x0)
#define FIFO(p)     ((p)->base_hi + 0x0)
#define EPPDATA(p)  ((p)->base    + 0x4)
#define EPPADDR(p)  ((p)->base    + 0x3)
#define CONTROL(p)  ((p)->base    + 0x2)
#define STATUS(p)   ((p)->base    + 0x1)
#define DATA(p)     ((p)->base    + 0x0)

struct parport_pc_private {
	/* Contents of CTR. */
	unsigned char ctr;

	/* Bitmask of writable CTR bits. */
	unsigned char ctr_writable;

	/* Whether or not there's an ECR. */
	int ecr;

	/* Number of PWords that FIFO will hold. */
	int fifo_depth;

	/* Number of bytes per portword. */
	int pword;

	/* Not used yet. */
	int readIntrThreshold;
	int writeIntrThreshold;

	/* buffer suitable for DMA, if DMA enabled */
	char *dma_buf;
	dma_addr_t dma_handle;
	struct pci_dev *dev;
};

extern __inline__ void parport_pc_write_data(struct parport *p, unsigned char d)
{
#ifdef DEBUG_PARPORT
	printk (KERN_DEBUG "parport_pc_write_data(%p,0x%02x)\n", p, d);
#endif
	outb(d, DATA(p));
}

extern __inline__ unsigned char parport_pc_read_data(struct parport *p)
{
	unsigned char val = inb (DATA (p));
#ifdef DEBUG_PARPORT
	printk (KERN_DEBUG "parport_pc_read_data(%p) = 0x%02x\n",
		p, val);
#endif
	return val;
}

/* __parport_pc_frob_control differs from parport_pc_frob_control in that
 * it doesn't do any extra masking. */
static __inline__ unsigned char __parport_pc_frob_control (struct parport *p,
							   unsigned char mask,
							   unsigned char val)
{
	struct parport_pc_private *priv = p->physport->private_data;
	unsigned char ctr = priv->ctr;
#ifdef DEBUG_PARPORT
	printk (KERN_DEBUG
		"__parport_pc_frob_control(%02x,%02x): %02x -> %02x\n",
		mask, val, ctr, ((ctr & ~mask) ^ val) & priv->ctr_writable);
#endif
	ctr = (ctr & ~mask) ^ val;
	ctr &= priv->ctr_writable; /* only write writable bits. */
	outb (ctr, CONTROL (p));
	priv->ctr = ctr;	/* Update soft copy */
	return ctr;
}

extern __inline__ void parport_pc_data_reverse (struct parport *p)
{
	__parport_pc_frob_control (p, 0x20, 0x20);
}

extern __inline__ void parport_pc_data_forward (struct parport *p)
{
	__parport_pc_frob_control (p, 0x20, 0x00);
}

extern __inline__ void parport_pc_write_control (struct parport *p,
						 unsigned char d)
{
	const unsigned char wm = (PARPORT_CONTROL_STROBE |
				  PARPORT_CONTROL_AUTOFD |
				  PARPORT_CONTROL_INIT |
				  PARPORT_CONTROL_SELECT);

	/* Take this out when drivers have adapted to newer interface. */
	if (d & 0x20) {
		printk (KERN_DEBUG "%s (%s): use data_reverse for this!\n",
			p->name, p->cad->name);
		parport_pc_data_reverse (p);
	}

	__parport_pc_frob_control (p, wm, d & wm);
}

extern __inline__ unsigned char parport_pc_read_control(struct parport *p)
{
	const unsigned char rm = (PARPORT_CONTROL_STROBE |
				  PARPORT_CONTROL_AUTOFD |
				  PARPORT_CONTROL_INIT |
				  PARPORT_CONTROL_SELECT);
	const struct parport_pc_private *priv = p->physport->private_data;
	return priv->ctr & rm; /* Use soft copy */
}

extern __inline__ unsigned char parport_pc_frob_control (struct parport *p,
							 unsigned char mask,
							 unsigned char val)
{
	const unsigned char wm = (PARPORT_CONTROL_STROBE |
				  PARPORT_CONTROL_AUTOFD |
				  PARPORT_CONTROL_INIT |
				  PARPORT_CONTROL_SELECT);

	/* Take this out when drivers have adapted to newer interface. */
	if (mask & 0x20) {
		printk (KERN_DEBUG "%s (%s): use data_%s for this!\n",
			p->name, p->cad->name,
			(val & 0x20) ? "reverse" : "forward");
		if (val & 0x20)
			parport_pc_data_reverse (p);
		else
			parport_pc_data_forward (p);
	}

	/* Restrict mask and val to control lines. */
	mask &= wm;
	val &= wm;

	return __parport_pc_frob_control (p, mask, val);
}

extern __inline__ unsigned char parport_pc_read_status(struct parport *p)
{
	return inb(STATUS(p));
}


extern __inline__ void parport_pc_disable_irq(struct parport *p)
{
	__parport_pc_frob_control (p, 0x10, 0x00);
}

extern __inline__ void parport_pc_enable_irq(struct parport *p)
{
	__parport_pc_frob_control (p, 0x10, 0x10);
}

extern void parport_pc_release_resources(struct parport *p);

extern int parport_pc_claim_resources(struct parport *p);

extern void parport_pc_init_state(struct pardevice *, struct parport_state *s);

extern void parport_pc_save_state(struct parport *p, struct parport_state *s);

extern void parport_pc_restore_state(struct parport *p, struct parport_state *s);

extern void parport_pc_inc_use_count(void);

extern void parport_pc_dec_use_count(void);

/* PCMCIA code will want to get us to look at a port.  Provide a mechanism. */
extern struct parport *parport_pc_probe_port (unsigned long base,
					      unsigned long base_hi,
					      int irq, int dma,
					      struct pci_dev *dev);

#endif
