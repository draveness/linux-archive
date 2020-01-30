/*======================================================================

    A driver for PCMCIA IDE/ATA disk cards

    ide-cs.c 1.3 2002/10/26 05:45:31

    The contents of this file are subject to the Mozilla Public
    License Version 1.1 (the "License"); you may not use this file
    except in compliance with the License. You may obtain a copy of
    the License at http://www.mozilla.org/MPL/

    Software distributed under the License is distributed on an "AS
    IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
    implied. See the License for the specific language governing
    rights and limitations under the License.

    The initial developer of the original code is David A. Hinds
    <dahinds@users.sourceforge.net>.  Portions created by David A. Hinds
    are Copyright (C) 1999 David A. Hinds.  All Rights Reserved.

    Alternatively, the contents of this file may be used under the
    terms of the GNU General Public License version 2 (the "GPL"), in
    which case the provisions of the GPL are applicable instead of the
    above.  If you wish to allow the use of your version of this file
    only under the terms of the GPL and not to allow others to use
    your version of this file under the MPL, indicate your decision
    by deleting the provisions above and replace them with the notice
    and other provisions required by the GPL.  If you do not delete
    the provisions above, a recipient may use your version of this
    file under either the MPL or the GPL.
    
======================================================================*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/ioport.h>
#include <linux/ide.h>
#include <linux/hdreg.h>
#include <linux/major.h>
#include <asm/io.h>
#include <asm/system.h>

#include <pcmcia/version.h>
#include <pcmcia/cs_types.h>
#include <pcmcia/cs.h>
#include <pcmcia/cistpl.h>
#include <pcmcia/ds.h>
#include <pcmcia/cisreg.h>
#include <pcmcia/ciscode.h>

/*====================================================================*/

/* Module parameters */

MODULE_AUTHOR("David Hinds <dahinds@users.sourceforge.net>");
MODULE_DESCRIPTION("PCMCIA ATA/IDE card driver");
MODULE_LICENSE("Dual MPL/GPL");

#define INT_MODULE_PARM(n, v) static int n = v; MODULE_PARM(n, "i")

/* Bit map of interrupts to choose from */
INT_MODULE_PARM(irq_mask, 0xdeb8);
static int irq_list[4] = { -1 };
MODULE_PARM(irq_list, "1-4i");

#ifdef PCMCIA_DEBUG
INT_MODULE_PARM(pc_debug, PCMCIA_DEBUG);
#define DEBUG(n, args...) if (pc_debug>(n)) printk(KERN_DEBUG args)
static char *version =
"ide-cs.c 1.3 2002/10/26 05:45:31 (David Hinds)";
#else
#define DEBUG(n, args...)
#endif

/*====================================================================*/

static const char ide_major[] = {
    IDE0_MAJOR, IDE1_MAJOR, IDE2_MAJOR, IDE3_MAJOR,
    IDE4_MAJOR, IDE5_MAJOR
};

typedef struct ide_info_t {
    dev_link_t	link;
    int		ndev;
    dev_node_t	node;
    int		hd;
} ide_info_t;

static void ide_release(dev_link_t *);
static int ide_event(event_t event, int priority,
		     event_callback_args_t *args);

static dev_info_t dev_info = "ide-cs";

static dev_link_t *ide_attach(void);
static void ide_detach(dev_link_t *);

static dev_link_t *dev_list = NULL;

/*======================================================================

    ide_attach() creates an "instance" of the driver, allocating
    local data structures for one device.  The device is registered
    with Card Services.

======================================================================*/

static dev_link_t *ide_attach(void)
{
    ide_info_t *info;
    dev_link_t *link;
    client_reg_t client_reg;
    int i, ret;
    
    DEBUG(0, "ide_attach()\n");

    /* Create new ide device */
    info = kmalloc(sizeof(*info), GFP_KERNEL);
    if (!info) return NULL;
    memset(info, 0, sizeof(*info));
    link = &info->link; link->priv = info;

    link->io.Attributes1 = IO_DATA_PATH_WIDTH_AUTO;
    link->io.Attributes2 = IO_DATA_PATH_WIDTH_8;
    link->io.IOAddrLines = 3;
    link->irq.Attributes = IRQ_TYPE_EXCLUSIVE;
    link->irq.IRQInfo1 = IRQ_INFO2_VALID|IRQ_LEVEL_ID;
    if (irq_list[0] == -1)
	link->irq.IRQInfo2 = irq_mask;
    else
	for (i = 0; i < 4; i++)
	    link->irq.IRQInfo2 |= 1 << irq_list[i];
    link->conf.Attributes = CONF_ENABLE_IRQ;
    link->conf.Vcc = 50;
    link->conf.IntType = INT_MEMORY_AND_IO;
    
    /* Register with Card Services */
    link->next = dev_list;
    dev_list = link;
    client_reg.dev_info = &dev_info;
    client_reg.Attributes = INFO_IO_CLIENT | INFO_CARD_SHARE;
    client_reg.EventMask =
	CS_EVENT_CARD_INSERTION | CS_EVENT_CARD_REMOVAL |
	CS_EVENT_RESET_PHYSICAL | CS_EVENT_CARD_RESET |
	CS_EVENT_PM_SUSPEND | CS_EVENT_PM_RESUME;
    client_reg.event_handler = &ide_event;
    client_reg.Version = 0x0210;
    client_reg.event_callback_args.client_data = link;
    ret = pcmcia_register_client(&link->handle, &client_reg);
    if (ret != CS_SUCCESS) {
	cs_error(link->handle, RegisterClient, ret);
	ide_detach(link);
	return NULL;
    }
    
    return link;
} /* ide_attach */

/*======================================================================

    This deletes a driver "instance".  The device is de-registered
    with Card Services.  If it has been released, all local data
    structures are freed.  Otherwise, the structures will be freed
    when the device is released.

======================================================================*/

static void ide_detach(dev_link_t *link)
{
    dev_link_t **linkp;
    int ret;

    DEBUG(0, "ide_detach(0x%p)\n", link);
    
    /* Locate device structure */
    for (linkp = &dev_list; *linkp; linkp = &(*linkp)->next)
	if (*linkp == link) break;
    if (*linkp == NULL)
	return;

    if (link->state & DEV_CONFIG)
	ide_release(link);
    
    if (link->handle) {
	ret = pcmcia_deregister_client(link->handle);
	if (ret != CS_SUCCESS)
	    cs_error(link->handle, DeregisterClient, ret);
    }
    
    /* Unlink, free device structure */
    *linkp = link->next;
    kfree(link->priv);
    
} /* ide_detach */

static int idecs_register(unsigned long io, unsigned long ctl, unsigned long irq)
{
    hw_regs_t hw;
    memset(&hw, 0, sizeof(hw));
    ide_init_hwif_ports(&hw, io, ctl, NULL);
    hw.irq = irq;
    hw.chipset = ide_pci;
    return ide_register_hw(&hw, NULL);
}

/*======================================================================

    ide_config() is scheduled to run after a CARD_INSERTION event
    is received, to configure the PCMCIA socket, and to make the
    ide device available to the system.

======================================================================*/

#define CS_CHECK(fn, ret) \
do { last_fn = (fn); if ((last_ret = (ret)) != 0) goto cs_failed; } while (0)

void ide_config(dev_link_t *link)
{
    client_handle_t handle = link->handle;
    ide_info_t *info = link->priv;
    tuple_t tuple;
    struct {
	u_short		buf[128];
	cisparse_t	parse;
	config_info_t	conf;
	cistpl_cftable_entry_t dflt;
    } *stk = NULL;
    cistpl_cftable_entry_t *cfg;
    int i, pass, last_ret = 0, last_fn = 0, hd, is_kme = 0;
    unsigned long io_base, ctl_base;

    DEBUG(0, "ide_config(0x%p)\n", link);

    stk = kmalloc(sizeof(*stk), GFP_KERNEL);
    if (!stk) goto err_mem;
    memset(stk, 0, sizeof(*stk));
    cfg = &stk->parse.cftable_entry;

    tuple.TupleData = (cisdata_t *)&stk->buf;
    tuple.TupleOffset = 0;
    tuple.TupleDataMax = 255;
    tuple.Attributes = 0;
    tuple.DesiredTuple = CISTPL_CONFIG;
    CS_CHECK(GetFirstTuple, pcmcia_get_first_tuple(handle, &tuple));
    CS_CHECK(GetTupleData, pcmcia_get_tuple_data(handle, &tuple));
    CS_CHECK(ParseTuple, pcmcia_parse_tuple(handle, &tuple, &stk->parse));
    link->conf.ConfigBase = stk->parse.config.base;
    link->conf.Present = stk->parse.config.rmask[0];

    tuple.DesiredTuple = CISTPL_MANFID;
    if (!pcmcia_get_first_tuple(handle, &tuple) &&
	!pcmcia_get_tuple_data(handle, &tuple) &&
	!pcmcia_parse_tuple(handle, &tuple, &stk->parse))
	is_kme = ((stk->parse.manfid.manf == MANFID_KME) &&
		  ((stk->parse.manfid.card == PRODID_KME_KXLC005_A) ||
		   (stk->parse.manfid.card == PRODID_KME_KXLC005_B)));

    /* Configure card */
    link->state |= DEV_CONFIG;

    /* Not sure if this is right... look up the current Vcc */
    CS_CHECK(GetConfigurationInfo, pcmcia_get_configuration_info(handle, &stk->conf));
    link->conf.Vcc = stk->conf.Vcc;

    pass = io_base = ctl_base = 0;
    tuple.DesiredTuple = CISTPL_CFTABLE_ENTRY;
    tuple.Attributes = 0;
    CS_CHECK(GetFirstTuple, pcmcia_get_first_tuple(handle, &tuple));
    while (1) {
    	if (pcmcia_get_tuple_data(handle, &tuple) != 0) goto next_entry;
	if (pcmcia_parse_tuple(handle, &tuple, &stk->parse) != 0) goto next_entry;

	/* Check for matching Vcc, unless we're desperate */
	if (!pass) {
	    if (cfg->vcc.present & (1 << CISTPL_POWER_VNOM)) {
		if (stk->conf.Vcc != cfg->vcc.param[CISTPL_POWER_VNOM] / 10000)
		    goto next_entry;
	    } else if (stk->dflt.vcc.present & (1 << CISTPL_POWER_VNOM)) {
		if (stk->conf.Vcc != stk->dflt.vcc.param[CISTPL_POWER_VNOM] / 10000)
		    goto next_entry;
	    }
	}

	if (cfg->vpp1.present & (1 << CISTPL_POWER_VNOM))
	    link->conf.Vpp1 = link->conf.Vpp2 =
		cfg->vpp1.param[CISTPL_POWER_VNOM] / 10000;
	else if (stk->dflt.vpp1.present & (1 << CISTPL_POWER_VNOM))
	    link->conf.Vpp1 = link->conf.Vpp2 =
		stk->dflt.vpp1.param[CISTPL_POWER_VNOM] / 10000;

	if ((cfg->io.nwin > 0) || (stk->dflt.io.nwin > 0)) {
	    cistpl_io_t *io = (cfg->io.nwin) ? &cfg->io : &stk->dflt.io;
	    link->conf.ConfigIndex = cfg->index;
	    link->io.BasePort1 = io->win[0].base;
	    link->io.IOAddrLines = io->flags & CISTPL_IO_LINES_MASK;
	    if (!(io->flags & CISTPL_IO_16BIT))
		link->io.Attributes1 = IO_DATA_PATH_WIDTH_8;
	    if (io->nwin == 2) {
		link->io.NumPorts1 = 8;
		link->io.BasePort2 = io->win[1].base;
		link->io.NumPorts2 = (is_kme) ? 2 : 1;
		if (pcmcia_request_io(link->handle, &link->io) != 0)
			goto next_entry;
		io_base = link->io.BasePort1;
		ctl_base = link->io.BasePort2;
	    } else if ((io->nwin == 1) && (io->win[0].len >= 16)) {
		link->io.NumPorts1 = io->win[0].len;
		link->io.NumPorts2 = 0;
		if (pcmcia_request_io(link->handle, &link->io) != 0)
			goto next_entry;
		io_base = link->io.BasePort1;
		ctl_base = link->io.BasePort1 + 0x0e;
	    } else goto next_entry;
	    /* If we've got this far, we're done */
	    break;
	}

    next_entry:
	if (cfg->flags & CISTPL_CFTABLE_DEFAULT)
	    memcpy(&stk->dflt, cfg, sizeof(stk->dflt));
	if (pass) {
	    CS_CHECK(GetNextTuple, pcmcia_get_next_tuple(handle, &tuple));
	} else if (pcmcia_get_next_tuple(handle, &tuple) != 0) {
	    CS_CHECK(GetFirstTuple, pcmcia_get_first_tuple(handle, &tuple));
	    memset(&stk->dflt, 0, sizeof(stk->dflt));
	    pass++;
	}
    }

    CS_CHECK(RequestIRQ, pcmcia_request_irq(handle, &link->irq));
    CS_CHECK(RequestConfiguration, pcmcia_request_configuration(handle, &link->conf));

    /* disable drive interrupts during IDE probe */
    outb(0x02, ctl_base);

    /* special setup for KXLC005 card */
    if (is_kme)
	outb(0x81, ctl_base+1);

    /* retry registration in case device is still spinning up */
    for (hd = -1, i = 0; i < 10; i++) {
	hd = idecs_register(io_base, ctl_base, link->irq.AssignedIRQ);
	if (hd >= 0) break;
	if (link->io.NumPorts1 == 0x20) {
	    outb(0x02, ctl_base + 0x10);
	    hd = idecs_register(io_base + 0x10, ctl_base + 0x10,
				link->irq.AssignedIRQ);
	    if (hd >= 0) {
		io_base += 0x10;
		ctl_base += 0x10;
		break;
	    }
	}
	__set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_timeout(HZ/10);
    }

    if (hd < 0) {
	printk(KERN_NOTICE "ide-cs: ide_register() at 0x%3lx & 0x%3lx"
	       ", irq %u failed\n", io_base, ctl_base,
	       link->irq.AssignedIRQ);
	goto failed;
    }

    info->ndev = 1;
    sprintf(info->node.dev_name, "hd%c", 'a' + (hd * 2));
    info->node.major = ide_major[hd];
    info->node.minor = 0;
    info->hd = hd;
    link->dev = &info->node;
    printk(KERN_INFO "ide-cs: %s: Vcc = %d.%d, Vpp = %d.%d\n",
	   info->node.dev_name, link->conf.Vcc / 10, link->conf.Vcc % 10,
	   link->conf.Vpp1 / 10, link->conf.Vpp1 % 10);

    link->state &= ~DEV_CONFIG_PENDING;
    kfree(stk);
    return;

err_mem:
    printk(KERN_NOTICE "ide-cs: ide_config failed memory allocation\n");
    goto failed;

cs_failed:
    cs_error(link->handle, last_fn, last_ret);
failed:
    kfree(stk);
    ide_release(link);
    link->state &= ~DEV_CONFIG_PENDING;
} /* ide_config */

/*======================================================================

    After a card is removed, ide_release() will unregister the net
    device, and release the PCMCIA configuration.  If the device is
    still open, this will be postponed until it is closed.
    
======================================================================*/

void ide_release(dev_link_t *link)
{
    ide_info_t *info = link->priv;
    
    DEBUG(0, "ide_release(0x%p)\n", link);

    if (info->ndev) {
	/* FIXME: if this fails we need to queue the cleanup somehow
	   -- need to investigate the required PCMCIA magic */
	ide_unregister(info->hd);
	/* deal with brain dead IDE resource management */
	request_region(link->io.BasePort1, link->io.NumPorts1,
		       info->node.dev_name);
	if (link->io.NumPorts2)
	    request_region(link->io.BasePort2, link->io.NumPorts2,
			   info->node.dev_name);
    }
    info->ndev = 0;
    link->dev = NULL;
    
    pcmcia_release_configuration(link->handle);
    pcmcia_release_io(link->handle, &link->io);
    pcmcia_release_irq(link->handle, &link->irq);
    
    link->state &= ~DEV_CONFIG;

} /* ide_release */

/*======================================================================

    The card status event handler.  Mostly, this schedules other
    stuff to run after an event is received.  A CARD_REMOVAL event
    also sets some flags to discourage the ide drivers from
    talking to the ports.
    
======================================================================*/

int ide_event(event_t event, int priority,
	      event_callback_args_t *args)
{
    dev_link_t *link = args->client_data;

    DEBUG(1, "ide_event(0x%06x)\n", event);
    
    switch (event) {
    case CS_EVENT_CARD_REMOVAL:
	link->state &= ~DEV_PRESENT;
	if (link->state & DEV_CONFIG)
		ide_release(link);
	break;
    case CS_EVENT_CARD_INSERTION:
	link->state |= DEV_PRESENT | DEV_CONFIG_PENDING;
	ide_config(link);
	break;
    case CS_EVENT_PM_SUSPEND:
	link->state |= DEV_SUSPEND;
	/* Fall through... */
    case CS_EVENT_RESET_PHYSICAL:
	if (link->state & DEV_CONFIG)
	    pcmcia_release_configuration(link->handle);
	break;
    case CS_EVENT_PM_RESUME:
	link->state &= ~DEV_SUSPEND;
	/* Fall through... */
    case CS_EVENT_CARD_RESET:
	if (DEV_OK(link))
	    pcmcia_request_configuration(link->handle, &link->conf);
	break;
    }
    return 0;
} /* ide_event */

static struct pcmcia_driver ide_cs_driver = {
	.owner		= THIS_MODULE,
	.drv		= {
		.name	= "ide-cs",
	},
	.attach		= ide_attach,
	.detach		= ide_detach,
};

static int __init init_ide_cs(void)
{
	return pcmcia_register_driver(&ide_cs_driver);
}

static void __exit exit_ide_cs(void)
{
	pcmcia_unregister_driver(&ide_cs_driver);
	while (dev_list != NULL)
		ide_detach(dev_list);
}

module_init(init_ide_cs);
module_exit(exit_ide_cs);
