/* $Id: parport_sunbpp.c,v 1.10 2000/03/27 01:47:56 anton Exp $
 * Parallel-port routines for Sun architecture
 * 
 * Author: Derrick J. Brashear <shadow@dementia.org>
 *
 * based on work by:
 *          Phil Blundell <Philip.Blundell@pobox.com>
 *          Tim Waugh <tim@cyberelk.demon.co.uk>
 *	    Jose Renau <renau@acm.org>
 *          David Campbell <campbell@tirian.che.curtin.edu.au>
 *          Grant Guenther <grant@torque.net>
 *          Eddie C. Dost <ecd@skynet.be>
 *          Stephen Williams (steve@icarus.com)
 *          Gus Baldauf (gbaldauf@ix.netcom.com)
 *          Peter Zaitcev
 *          Tom Dyas
 */

#include <linux/string.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/malloc.h>
#include <linux/init.h>

#include <linux/parport.h>

#include <asm/ptrace.h>
#include <linux/interrupt.h>

#include <asm/io.h>
#include <asm/oplib.h>           /* OpenProm Library */
#include <asm/sbus.h>
#include <asm/dma.h>             /* BPP uses LSI 64854 for DMA */
#include <asm/irq.h>
#include <asm/sunbpp.h>

#undef __SUNBPP_DEBUG
#ifdef __SUNBPP_DEBUG
#define dprintk(x) printk x
#else
#define dprintk(x)
#endif

static void parport_sunbpp_interrupt(int irq, void *dev_id, struct pt_regs *regs)
{
	parport_generic_irq(irq, (struct parport *) dev_id, regs);
}

static void parport_sunbpp_disable_irq(struct parport *p)
{
	struct bpp_regs *regs = (struct bpp_regs *)p->base;
	u32 tmp;

	tmp = sbus_readl(&regs->p_csr);
	tmp &= ~DMA_INT_ENAB;
	sbus_writel(tmp, &regs->p_csr);
}

static void parport_sunbpp_enable_irq(struct parport *p)
{
	struct bpp_regs *regs = (struct bpp_regs *)p->base;
	u32 tmp;

	tmp = sbus_readl(&regs->p_csr);
	tmp |= DMA_INT_ENAB;
	sbus_writel(tmp, &regs->p_csr);
}

static void parport_sunbpp_write_data(struct parport *p, unsigned char d)
{
	struct bpp_regs *regs = (struct bpp_regs *)p->base;

	sbus_writeb(d, &regs->p_dr);
	dprintk(("wrote 0x%x\n", d));
}

static unsigned char parport_sunbpp_read_data(struct parport *p)
{
	struct bpp_regs *regs = (struct bpp_regs *)p->base;

	return sbus_readb(&regs->p_dr);
}

#if 0
static void control_pc_to_sunbpp(struct parport *p, unsigned char status)
{
	struct bpp_regs *regs = (struct bpp_regs *)p->base;
	unsigned char value_tcr = sbus_readb(&regs->p_tcr);
	unsigned char value_or = sbus_readb(&regs->p_or);

	if (status & PARPORT_CONTROL_STROBE) 
		value_tcr |= P_TCR_DS;
	if (status & PARPORT_CONTROL_AUTOFD) 
		value_or |= P_OR_AFXN;
	if (status & PARPORT_CONTROL_INIT) 
		value_or |= P_OR_INIT;
	if (status & PARPORT_CONTROL_SELECT) 
		value_or |= P_OR_SLCT_IN;

	sbus_writeb(value_or, &regs->p_or);
	sbus_writeb(value_tcr, &regs->p_tcr);
}
#endif

static unsigned char status_sunbpp_to_pc(struct parport *p)
{
	struct bpp_regs *regs = (struct bpp_regs *)p->base;
	unsigned char bits = 0;
	unsigned char value_tcr = sbus_readb(&regs->p_tcr);
	unsigned char value_ir = sbus_readb(&regs->p_ir);

	if (!(value_ir & P_IR_ERR))
		bits |= PARPORT_STATUS_ERROR;
	if (!(value_ir & P_IR_SLCT))
		bits |= PARPORT_STATUS_SELECT;
	if (!(value_ir & P_IR_PE))
		bits |= PARPORT_STATUS_PAPEROUT;
	if (value_tcr & P_TCR_ACK)
		bits |= PARPORT_STATUS_ACK;
	if (!(value_tcr & P_TCR_BUSY))
		bits |= PARPORT_STATUS_BUSY;

	dprintk(("tcr 0x%x ir 0x%x\n", regs->p_tcr, regs->p_ir));
	dprintk(("read status 0x%x\n", bits));
	return bits;
}

static unsigned char control_sunbpp_to_pc(struct parport *p)
{
	struct bpp_regs *regs = (struct bpp_regs *)p->base;
	unsigned char bits = 0;
	unsigned char value_tcr = sbus_readb(&regs->p_tcr);
	unsigned char value_or = sbus_readb(&regs->p_or);

	if (!(value_tcr & P_TCR_DS))
		bits |= PARPORT_CONTROL_STROBE;
	if (!(value_or & P_OR_AFXN))
		bits |= PARPORT_CONTROL_AUTOFD;
	if (!(value_or & P_OR_INIT))
		bits |= PARPORT_CONTROL_INIT;
	if (value_or & P_OR_SLCT_IN)
		bits |= PARPORT_CONTROL_SELECT;

	dprintk(("tcr 0x%x or 0x%x\n", regs->p_tcr, regs->p_or));
	dprintk(("read control 0x%x\n", bits));
	return bits;
}

static unsigned char parport_sunbpp_read_control(struct parport *p)
{
	return control_sunbpp_to_pc(p);
}

static unsigned char parport_sunbpp_frob_control(struct parport *p,
						 unsigned char mask,
						 unsigned char val)
{
	struct bpp_regs *regs = (struct bpp_regs *)p->base;
	unsigned char value_tcr = sbus_readb(&regs->p_tcr);
	unsigned char value_or = sbus_readb(&regs->p_or);

	dprintk(("frob1: tcr 0x%x or 0x%x\n", regs->p_tcr, regs->p_or));
	if (mask & PARPORT_CONTROL_STROBE) {
		if (val & PARPORT_CONTROL_STROBE) {
			value_tcr &= ~P_TCR_DS;
		} else {
			value_tcr |= P_TCR_DS;
		}
	}
	if (mask & PARPORT_CONTROL_AUTOFD) {
		if (val & PARPORT_CONTROL_AUTOFD) {
			value_or &= ~P_OR_AFXN;
		} else {
			value_or |= P_OR_AFXN;
		}
	}
	if (mask & PARPORT_CONTROL_INIT) {
		if (val & PARPORT_CONTROL_INIT) {
			value_or &= ~P_OR_INIT;
		} else {
			value_or |= P_OR_INIT;
		}
	}
	if (mask & PARPORT_CONTROL_SELECT) {
		if (val & PARPORT_CONTROL_SELECT) {
			value_or |= P_OR_SLCT_IN;
		} else {
			value_or &= ~P_OR_SLCT_IN;
		}
	}

	sbus_writeb(value_or, &regs->p_or);
	sbus_writeb(value_tcr, &regs->p_tcr);
	dprintk(("frob2: tcr 0x%x or 0x%x\n", regs->p_tcr, regs->p_or));
	return parport_sunbpp_read_control(p);
}

static void parport_sunbpp_write_control(struct parport *p, unsigned char d)
{
	const unsigned char wm = (PARPORT_CONTROL_STROBE |
				  PARPORT_CONTROL_AUTOFD |
				  PARPORT_CONTROL_INIT |
				  PARPORT_CONTROL_SELECT);

	parport_sunbpp_frob_control (p, wm, d & wm);
}

static unsigned char parport_sunbpp_read_status(struct parport *p)
{
	return status_sunbpp_to_pc(p);
}

static void parport_sunbpp_data_forward (struct parport *p)
{
	struct bpp_regs *regs = (struct bpp_regs *)p->base;
	unsigned char value_tcr = sbus_readb(&regs->p_tcr);

	dprintk(("forward\n"));
	value_tcr &= ~P_TCR_DIR;
	sbus_writeb(value_tcr, &regs->p_tcr);
}

static void parport_sunbpp_data_reverse (struct parport *p)
{
	struct bpp_regs *regs = (struct bpp_regs *)p->base;
	u8 val = sbus_readb(&regs->p_tcr);

	dprintk(("reverse\n"));
	val |= P_TCR_DIR;
	sbus_writeb(val, &regs->p_tcr);
}

static void parport_sunbpp_init_state(struct pardevice *dev, struct parport_state *s)
{
	s->u.pc.ctr = 0xc;
	s->u.pc.ecr = 0x0;
}

static void parport_sunbpp_save_state(struct parport *p, struct parport_state *s)
{
	s->u.pc.ctr = parport_sunbpp_read_control(p);
}

static void parport_sunbpp_restore_state(struct parport *p, struct parport_state *s)
{
	parport_sunbpp_write_control(p, s->u.pc.ctr);
}

static void parport_sunbpp_inc_use_count(void)
{
#ifdef MODULE
	MOD_INC_USE_COUNT;
#endif
}

static void parport_sunbpp_dec_use_count(void)
{
#ifdef MODULE
	MOD_DEC_USE_COUNT;
#endif
}

static struct parport_operations parport_sunbpp_ops = 
{
	parport_sunbpp_write_data,
	parport_sunbpp_read_data,

	parport_sunbpp_write_control,
	parport_sunbpp_read_control,
	parport_sunbpp_frob_control,

	parport_sunbpp_read_status,

	parport_sunbpp_enable_irq,
        parport_sunbpp_disable_irq,

        parport_sunbpp_data_forward,
        parport_sunbpp_data_reverse,

        parport_sunbpp_init_state,
        parport_sunbpp_save_state,
        parport_sunbpp_restore_state,

        parport_sunbpp_inc_use_count,
        parport_sunbpp_dec_use_count,

        parport_ieee1284_epp_write_data,
        parport_ieee1284_epp_read_data,
        parport_ieee1284_epp_write_addr,
        parport_ieee1284_epp_read_addr,

        parport_ieee1284_ecp_write_data,
        parport_ieee1284_ecp_read_data,
        parport_ieee1284_ecp_write_addr,

        parport_ieee1284_write_compat,
        parport_ieee1284_read_nibble,
        parport_ieee1284_read_byte,
};

static int __init init_one_port(struct sbus_dev *sdev)
{
	struct parport *p;
	/* at least in theory there may be a "we don't dma" case */
	struct parport_operations *ops;
	unsigned long base;
	int irq, dma, err, size;
	struct bpp_regs *regs;
	unsigned char value_tcr;

	dprintk(("init_one_port(%p): ranges, alloc_io, ", sdev));
	irq = sdev->irqs[0];
	base = sbus_ioremap(&sdev->resource[0], 0,
			    sdev->reg_addrs[0].reg_size, 
			    "sunbpp");
	size = sdev->reg_addrs[0].reg_size;
	dma = PARPORT_DMA_NONE;

	dprintk(("alloc(ppops), "));
	ops = kmalloc (sizeof (struct parport_operations), GFP_KERNEL);
        if (!ops) {
		sbus_iounmap(base, size);
		return 0;
        }

        memcpy (ops, &parport_sunbpp_ops, sizeof (struct parport_operations));

	dprintk(("register_port, "));
	if (!(p = parport_register_port(base, irq, dma, ops))) {
		kfree(ops);
		sbus_iounmap(base, size);
		return 0;
	}

	p->size = size;

	dprintk(("init_one_port: request_irq(%08x:%p:%x:%s:%p) ",
		p->irq, parport_sunbpp_interrupt, SA_SHIRQ, p->name, p));
	if ((err = request_irq(p->irq, parport_sunbpp_interrupt,
			       SA_SHIRQ, p->name, p)) != 0) {
		dprintk(("ERROR %d\n", err));
		parport_unregister_port(p);
		kfree(ops);
		sbus_iounmap(base, size);
		return err;
	} else {
		dprintk(("OK\n"));
		parport_sunbpp_enable_irq(p);
	}

	regs = (struct bpp_regs *)p->base;
	dprintk(("forward\n"));
	value_tcr = sbus_readb(&regs->p_tcr);
	value_tcr &= ~P_TCR_DIR;
	sbus_writeb(value_tcr, &regs->p_tcr);

	printk(KERN_INFO "%s: sunbpp at 0x%lx\n", p->name, p->base);
	parport_proc_register(p);
	parport_announce_port (p);

	return 1;
}

EXPORT_NO_SYMBOLS;

#ifdef MODULE
int init_module(void)
#else
int __init parport_sunbpp_init(void)
#endif
{
        struct sbus_bus *sbus;
        struct sbus_dev *sdev;
	int count = 0;

	for_each_sbus(sbus) {
		for_each_sbusdev(sdev, sbus) {
			if (!strcmp(sdev->prom_name, "SUNW,bpp"))
				count += init_one_port(sdev);
		}
	}
	return count ? 0 : -ENODEV;
}

#ifdef MODULE
MODULE_AUTHOR("Derrick J Brashear");
MODULE_DESCRIPTION("Parport Driver for Sparc bidirectional Port");
MODULE_SUPPORTED_DEVICE("Sparc Bidirectional Parallel Port");

void
cleanup_module(void)
{
	struct parport *p = parport_enumerate();

	while (p) {
		struct parport *next = p->next;

		if (1/*p->modes & PARPORT_MODE_PCSPP*/) { 
			struct parport_operations *ops = p->ops;

			if (p->irq != PARPORT_IRQ_NONE) {
				parport_sunbpp_disable_irq(p);
				free_irq(p->irq, p);
			}
			sbus_iounmap(p->base, p->size);
			parport_proc_unregister(p);
			parport_unregister_port(p);
			kfree (ops);
		}
		p = next;
	}
}
#endif
