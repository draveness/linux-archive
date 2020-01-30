/*======================================================================

    Common support code for the PCMCIA control functionality of
    integrated SOCs like the SA-11x0 and PXA2xx microprocessors.

    The contents of this file are subject to the Mozilla Public
    License Version 1.1 (the "License"); you may not use this file
    except in compliance with the License. You may obtain a copy of
    the License at http://www.mozilla.org/MPL/

    Software distributed under the License is distributed on an "AS
    IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
    implied. See the License for the specific language governing
    rights and limitations under the License.

    The initial developer of the original code is John G. Dorsey
    <john+@cs.cmu.edu>.  Portions created by John G. Dorsey are
    Copyright (C) 1999 John G. Dorsey.  All Rights Reserved.

    Alternatively, the contents of this file may be used under the
    terms of the GNU Public License version 2 (the "GPL"), in which
    case the provisions of the GPL are applicable instead of the
    above.  If you wish to allow the use of your version of this file
    only under the terms of the GPL and not to allow others to use
    your version of this file under the MPL, indicate your decision
    by deleting the provisions above and replace them with the notice
    and other provisions required by the GPL.  If you do not delete
    the provisions above, a recipient may use your version of this
    file under either the MPL or the GPL.

======================================================================*/


#include <linux/config.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/cpufreq.h>

#include <asm/hardware.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/system.h>

#include "soc_common.h"

#ifdef DEBUG

static int pc_debug;
module_param(pc_debug, int, 0644);

void soc_pcmcia_debug(struct soc_pcmcia_socket *skt, const char *func,
		      int lvl, const char *fmt, ...)
{
	va_list args;
	if (pc_debug > lvl) {
		printk(KERN_DEBUG "skt%u: %s: ", skt->nr, func);
		va_start(args, fmt);
		printk(fmt, args);
		va_end(args);
	}
}

#endif

#define to_soc_pcmcia_socket(x)	container_of(x, struct soc_pcmcia_socket, socket)

static unsigned short
calc_speed(unsigned short *spds, int num, unsigned short dflt)
{
	unsigned short speed = 0;
	int i;

	for (i = 0; i < num; i++)
		if (speed < spds[i])
			speed = spds[i];
	if (speed == 0)
		speed = dflt;

	return speed;
}

void soc_common_pcmcia_get_timing(struct soc_pcmcia_socket *skt, struct soc_pcmcia_timing *timing)
{
	timing->io = calc_speed(skt->spd_io, MAX_IO_WIN, SOC_PCMCIA_IO_ACCESS);
	timing->mem = calc_speed(skt->spd_mem, MAX_WIN, SOC_PCMCIA_3V_MEM_ACCESS);
	timing->attr = calc_speed(skt->spd_attr, MAX_WIN, SOC_PCMCIA_3V_MEM_ACCESS);
}
EXPORT_SYMBOL(soc_common_pcmcia_get_timing);

static unsigned int soc_common_pcmcia_skt_state(struct soc_pcmcia_socket *skt)
{
	struct pcmcia_state state;
	unsigned int stat;

	memset(&state, 0, sizeof(struct pcmcia_state));

	skt->ops->socket_state(skt, &state);

	stat = state.detect  ? SS_DETECT : 0;
	stat |= state.ready  ? SS_READY  : 0;
	stat |= state.wrprot ? SS_WRPROT : 0;
	stat |= state.vs_3v  ? SS_3VCARD : 0;
	stat |= state.vs_Xv  ? SS_XVCARD : 0;

	/* The power status of individual sockets is not available
	 * explicitly from the hardware, so we just remember the state
	 * and regurgitate it upon request:
	 */
	stat |= skt->cs_state.Vcc ? SS_POWERON : 0;

	if (skt->cs_state.flags & SS_IOCARD)
		stat |= state.bvd1 ? SS_STSCHG : 0;
	else {
		if (state.bvd1 == 0)
			stat |= SS_BATDEAD;
		else if (state.bvd2 == 0)
			stat |= SS_BATWARN;
	}
	return stat;
}

/*
 * soc_common_pcmcia_config_skt
 * ^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *
 * Convert PCMCIA socket state to our socket configure structure.
 */
static int
soc_common_pcmcia_config_skt(struct soc_pcmcia_socket *skt, socket_state_t *state)
{
	int ret;

	ret = skt->ops->configure_socket(skt, state);
	if (ret == 0) {
		/*
		 * This really needs a better solution.  The IRQ
		 * may or may not be claimed by the driver.
		 */
		if (skt->irq_state != 1 && state->io_irq) {
			skt->irq_state = 1;
			set_irq_type(skt->irq, IRQT_FALLING);
		} else if (skt->irq_state == 1 && state->io_irq == 0) {
			skt->irq_state = 0;
			set_irq_type(skt->irq, IRQT_NOEDGE);
		}

		skt->cs_state = *state;
	}

	if (ret < 0)
		printk(KERN_ERR "soc_common_pcmcia: unable to configure "
		       "socket %d\n", skt->nr);

	return ret;
}

/* soc_common_pcmcia_sock_init()
 * ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *
 * (Re-)Initialise the socket, turning on status interrupts
 * and PCMCIA bus.  This must wait for power to stabilise
 * so that the card status signals report correctly.
 *
 * Returns: 0
 */
static int soc_common_pcmcia_sock_init(struct pcmcia_socket *sock)
{
	struct soc_pcmcia_socket *skt = to_soc_pcmcia_socket(sock);

	debug(skt, 2, "initializing socket\n");

	skt->ops->socket_init(skt);
	return 0;
}


/*
 * soc_common_pcmcia_suspend()
 * ^^^^^^^^^^^^^^^^^^^^^^^^^^^
 *
 * Remove power on the socket, disable IRQs from the card.
 * Turn off status interrupts, and disable the PCMCIA bus.
 *
 * Returns: 0
 */
static int soc_common_pcmcia_suspend(struct pcmcia_socket *sock)
{
	struct soc_pcmcia_socket *skt = to_soc_pcmcia_socket(sock);
	int ret;

	debug(skt, 2, "suspending socket\n");

	ret = soc_common_pcmcia_config_skt(skt, &dead_socket);
	if (ret == 0)
		skt->ops->socket_suspend(skt);

	return ret;
}

static spinlock_t status_lock = SPIN_LOCK_UNLOCKED;

static void soc_common_check_status(struct soc_pcmcia_socket *skt)
{
	unsigned int events;

	debug(skt, 4, "entering PCMCIA monitoring thread\n");

	do {
		unsigned int status;
		unsigned long flags;

		status = soc_common_pcmcia_skt_state(skt);

		spin_lock_irqsave(&status_lock, flags);
		events = (status ^ skt->status) & skt->cs_state.csc_mask;
		skt->status = status;
		spin_unlock_irqrestore(&status_lock, flags);

		debug(skt, 4, "events: %s%s%s%s%s%s\n",
			events == 0         ? "<NONE>"   : "",
			events & SS_DETECT  ? "DETECT "  : "",
			events & SS_READY   ? "READY "   : "",
			events & SS_BATDEAD ? "BATDEAD " : "",
			events & SS_BATWARN ? "BATWARN " : "",
			events & SS_STSCHG  ? "STSCHG "  : "");

		if (events)
			pcmcia_parse_events(&skt->socket, events);
	} while (events);
}

/* Let's poll for events in addition to IRQs since IRQ only is unreliable... */
static void soc_common_pcmcia_poll_event(unsigned long dummy)
{
	struct soc_pcmcia_socket *skt = (struct soc_pcmcia_socket *)dummy;
	debug(skt, 4, "polling for events\n");

	mod_timer(&skt->poll_timer, jiffies + SOC_PCMCIA_POLL_PERIOD);

	soc_common_check_status(skt);
}


/*
 * Service routine for socket driver interrupts (requested by the
 * low-level PCMCIA init() operation via soc_common_pcmcia_thread()).
 * The actual interrupt-servicing work is performed by
 * soc_common_pcmcia_thread(), largely because the Card Services event-
 * handling code performs scheduling operations which cannot be
 * executed from within an interrupt context.
 */
static irqreturn_t soc_common_pcmcia_interrupt(int irq, void *dev, struct pt_regs *regs)
{
	struct soc_pcmcia_socket *skt = dev;

	debug(skt, 3, "servicing IRQ %d\n", irq);

	soc_common_check_status(skt);

	return IRQ_HANDLED;
}


/*
 *  Implements the get_status() operation for the in-kernel PCMCIA
 * service (formerly SS_GetStatus in Card Services). Essentially just
 * fills in bits in `status' according to internal driver state or
 * the value of the voltage detect chipselect register.
 *
 * As a debugging note, during card startup, the PCMCIA core issues
 * three set_socket() commands in a row the first with RESET deasserted,
 * the second with RESET asserted, and the last with RESET deasserted
 * again. Following the third set_socket(), a get_status() command will
 * be issued. The kernel is looking for the SS_READY flag (see
 * setup_socket(), reset_socket(), and unreset_socket() in cs.c).
 *
 * Returns: 0
 */
static int
soc_common_pcmcia_get_status(struct pcmcia_socket *sock, unsigned int *status)
{
	struct soc_pcmcia_socket *skt = to_soc_pcmcia_socket(sock);

	skt->status = soc_common_pcmcia_skt_state(skt);
	*status = skt->status;

	return 0;
}


/*
 * Implements the get_socket() operation for the in-kernel PCMCIA
 * service (formerly SS_GetSocket in Card Services). Not a very
 * exciting routine.
 *
 * Returns: 0
 */
static int
soc_common_pcmcia_get_socket(struct pcmcia_socket *sock, socket_state_t *state)
{
	struct soc_pcmcia_socket *skt = to_soc_pcmcia_socket(sock);

	debug(skt, 2, "\n");

	*state = skt->cs_state;

	return 0;
}

/*
 * Implements the set_socket() operation for the in-kernel PCMCIA
 * service (formerly SS_SetSocket in Card Services). We more or
 * less punt all of this work and let the kernel handle the details
 * of power configuration, reset, &c. We also record the value of
 * `state' in order to regurgitate it to the PCMCIA core later.
 *
 * Returns: 0
 */
static int
soc_common_pcmcia_set_socket(struct pcmcia_socket *sock, socket_state_t *state)
{
	struct soc_pcmcia_socket *skt = to_soc_pcmcia_socket(sock);

	debug(skt, 2, "mask: %s%s%s%s%s%sflags: %s%s%s%s%s%sVcc %d Vpp %d irq %d\n",
			(state->csc_mask==0)?"<NONE> ":"",
			(state->csc_mask&SS_DETECT)?"DETECT ":"",
			(state->csc_mask&SS_READY)?"READY ":"",
			(state->csc_mask&SS_BATDEAD)?"BATDEAD ":"",
			(state->csc_mask&SS_BATWARN)?"BATWARN ":"",
			(state->csc_mask&SS_STSCHG)?"STSCHG ":"",
			(state->flags==0)?"<NONE> ":"",
			(state->flags&SS_PWR_AUTO)?"PWR_AUTO ":"",
			(state->flags&SS_IOCARD)?"IOCARD ":"",
			(state->flags&SS_RESET)?"RESET ":"",
			(state->flags&SS_SPKR_ENA)?"SPKR_ENA ":"",
			(state->flags&SS_OUTPUT_ENA)?"OUTPUT_ENA ":"",
			state->Vcc, state->Vpp, state->io_irq);

	return soc_common_pcmcia_config_skt(skt, state);
}


/*
 * Implements the set_io_map() operation for the in-kernel PCMCIA
 * service (formerly SS_SetIOMap in Card Services). We configure
 * the map speed as requested, but override the address ranges
 * supplied by Card Services.
 *
 * Returns: 0 on success, -1 on error
 */
static int
soc_common_pcmcia_set_io_map(struct pcmcia_socket *sock, struct pccard_io_map *map)
{
	struct soc_pcmcia_socket *skt = to_soc_pcmcia_socket(sock);
	unsigned short speed = map->speed;

	debug(skt, 2, "map %u  speed %u start 0x%08x stop 0x%08x\n",
		map->map, map->speed, map->start, map->stop);
	debug(skt, 2, "flags: %s%s%s%s%s%s%s%s\n",
		(map->flags==0)?"<NONE>":"",
		(map->flags&MAP_ACTIVE)?"ACTIVE ":"",
		(map->flags&MAP_16BIT)?"16BIT ":"",
		(map->flags&MAP_AUTOSZ)?"AUTOSZ ":"",
		(map->flags&MAP_0WS)?"0WS ":"",
		(map->flags&MAP_WRPROT)?"WRPROT ":"",
		(map->flags&MAP_USE_WAIT)?"USE_WAIT ":"",
		(map->flags&MAP_PREFETCH)?"PREFETCH ":"");

	if (map->map >= MAX_IO_WIN) {
		printk(KERN_ERR "%s(): map (%d) out of range\n", __FUNCTION__,
		       map->map);
		return -1;
	}

	if (map->flags & MAP_ACTIVE) {
		if (speed == 0)
			speed = SOC_PCMCIA_IO_ACCESS;
	} else {
		speed = 0;
	}

	skt->spd_io[map->map] = speed;
	skt->ops->set_timing(skt);

	if (map->stop == 1)
		map->stop = PAGE_SIZE-1;

	map->stop -= map->start;
	map->stop += (unsigned long)skt->virt_io;
	map->start = (unsigned long)skt->virt_io;

	return 0;
}


/*
 * Implements the set_mem_map() operation for the in-kernel PCMCIA
 * service (formerly SS_SetMemMap in Card Services). We configure
 * the map speed as requested, but override the address ranges
 * supplied by Card Services.
 *
 * Returns: 0 on success, -1 on error
 */
static int
soc_common_pcmcia_set_mem_map(struct pcmcia_socket *sock, struct pccard_mem_map *map)
{
	struct soc_pcmcia_socket *skt = to_soc_pcmcia_socket(sock);
	struct resource *res;
	unsigned short speed = map->speed;

	debug(skt, 2, "map %u speed %u card_start %08x\n",
		map->map, map->speed, map->card_start);
	debug(skt, 2, "flags: %s%s%s%s%s%s%s%s\n",
		(map->flags==0)?"<NONE>":"",
		(map->flags&MAP_ACTIVE)?"ACTIVE ":"",
		(map->flags&MAP_16BIT)?"16BIT ":"",
		(map->flags&MAP_AUTOSZ)?"AUTOSZ ":"",
		(map->flags&MAP_0WS)?"0WS ":"",
		(map->flags&MAP_WRPROT)?"WRPROT ":"",
		(map->flags&MAP_ATTRIB)?"ATTRIB ":"",
		(map->flags&MAP_USE_WAIT)?"USE_WAIT ":"");

	if (map->map >= MAX_WIN)
		return -EINVAL;

	if (map->flags & MAP_ACTIVE) {
		if (speed == 0)
			speed = 300;
	} else {
		speed = 0;
	}

	if (map->flags & MAP_ATTRIB) {
		res = &skt->res_attr;
		skt->spd_attr[map->map] = speed;
		skt->spd_mem[map->map] = 0;
	} else {
		res = &skt->res_mem;
		skt->spd_attr[map->map] = 0;
		skt->spd_mem[map->map] = speed;
	}

	skt->ops->set_timing(skt);

	map->sys_stop -= map->sys_start;
	map->sys_stop += res->start + map->card_start;
	map->sys_start = res->start + map->card_start;

	return 0;
}

struct bittbl {
	unsigned int mask;
	const char *name;
};

static struct bittbl status_bits[] = {
	{ SS_WRPROT,		"SS_WRPROT"	},
	{ SS_BATDEAD,		"SS_BATDEAD"	},
	{ SS_BATWARN,		"SS_BATWARN"	},
	{ SS_READY,		"SS_READY"	},
	{ SS_DETECT,		"SS_DETECT"	},
	{ SS_POWERON,		"SS_POWERON"	},
	{ SS_STSCHG,		"SS_STSCHG"	},
	{ SS_3VCARD,		"SS_3VCARD"	},
	{ SS_XVCARD,		"SS_XVCARD"	},
};

static struct bittbl conf_bits[] = {
	{ SS_PWR_AUTO,		"SS_PWR_AUTO"	},
	{ SS_IOCARD,		"SS_IOCARD"	},
	{ SS_RESET,		"SS_RESET"	},
	{ SS_DMA_MODE,		"SS_DMA_MODE"	},
	{ SS_SPKR_ENA,		"SS_SPKR_ENA"	},
	{ SS_OUTPUT_ENA,	"SS_OUTPUT_ENA"	},
};

static void
dump_bits(char **p, const char *prefix, unsigned int val, struct bittbl *bits, int sz)
{
	char *b = *p;
	int i;

	b += sprintf(b, "%-9s:", prefix);
	for (i = 0; i < sz; i++)
		if (val & bits[i].mask)
			b += sprintf(b, " %s", bits[i].name);
	*b++ = '\n';
	*p = b;
}

/*
 * Implements the /sys/class/pcmcia_socket/??/status file.
 *
 * Returns: the number of characters added to the buffer
 */
static ssize_t show_status(struct class_device *class_dev, char *buf)
{
	struct soc_pcmcia_socket *skt =
		container_of(class_dev, struct soc_pcmcia_socket, socket.dev);
	char *p = buf;

	p+=sprintf(p, "slot     : %d\n", skt->nr);

	dump_bits(&p, "status", skt->status,
		  status_bits, ARRAY_SIZE(status_bits));
	dump_bits(&p, "csc_mask", skt->cs_state.csc_mask,
		  status_bits, ARRAY_SIZE(status_bits));
	dump_bits(&p, "cs_flags", skt->cs_state.flags,
		  conf_bits, ARRAY_SIZE(conf_bits));

	p+=sprintf(p, "Vcc      : %d\n", skt->cs_state.Vcc);
	p+=sprintf(p, "Vpp      : %d\n", skt->cs_state.Vpp);
	p+=sprintf(p, "IRQ      : %d (%d)\n", skt->cs_state.io_irq, skt->irq);
	if (skt->ops->show_timing)
		p+=skt->ops->show_timing(skt, p);

	return p-buf;
}
static CLASS_DEVICE_ATTR(status, S_IRUGO, show_status, NULL);


static struct pccard_operations soc_common_pcmcia_operations = {
	.init			= soc_common_pcmcia_sock_init,
	.suspend		= soc_common_pcmcia_suspend,
	.get_status		= soc_common_pcmcia_get_status,
	.get_socket		= soc_common_pcmcia_get_socket,
	.set_socket		= soc_common_pcmcia_set_socket,
	.set_io_map		= soc_common_pcmcia_set_io_map,
	.set_mem_map		= soc_common_pcmcia_set_mem_map,
};


int soc_pcmcia_request_irqs(struct soc_pcmcia_socket *skt,
			    struct pcmcia_irqs *irqs, int nr)
{
	int i, res = 0;

	for (i = 0; i < nr; i++) {
		if (irqs[i].sock != skt->nr)
			continue;
		res = request_irq(irqs[i].irq, soc_common_pcmcia_interrupt,
				  SA_INTERRUPT, irqs[i].str, skt);
		if (res)
			break;
		set_irq_type(irqs[i].irq, IRQT_NOEDGE);
	}

	if (res) {
		printk(KERN_ERR "PCMCIA: request for IRQ%d failed (%d)\n",
			irqs[i].irq, res);

		while (i--)
			if (irqs[i].sock == skt->nr)
				free_irq(irqs[i].irq, skt);
	}
	return res;
}
EXPORT_SYMBOL(soc_pcmcia_request_irqs);

void soc_pcmcia_free_irqs(struct soc_pcmcia_socket *skt,
			  struct pcmcia_irqs *irqs, int nr)
{
	int i;

	for (i = 0; i < nr; i++)
		if (irqs[i].sock == skt->nr)
			free_irq(irqs[i].irq, skt);
}
EXPORT_SYMBOL(soc_pcmcia_free_irqs);

void soc_pcmcia_disable_irqs(struct soc_pcmcia_socket *skt,
			     struct pcmcia_irqs *irqs, int nr)
{
	int i;

	for (i = 0; i < nr; i++)
		if (irqs[i].sock == skt->nr)
			set_irq_type(irqs[i].irq, IRQT_NOEDGE);
}
EXPORT_SYMBOL(soc_pcmcia_disable_irqs);

void soc_pcmcia_enable_irqs(struct soc_pcmcia_socket *skt,
			    struct pcmcia_irqs *irqs, int nr)
{
	int i;

	for (i = 0; i < nr; i++)
		if (irqs[i].sock == skt->nr) {
			set_irq_type(irqs[i].irq, IRQT_RISING);
			set_irq_type(irqs[i].irq, IRQT_BOTHEDGE);
		}
}
EXPORT_SYMBOL(soc_pcmcia_enable_irqs);


LIST_HEAD(soc_pcmcia_sockets);
DECLARE_MUTEX(soc_pcmcia_sockets_lock);

static const char *skt_names[] = {
	"PCMCIA socket 0",
	"PCMCIA socket 1",
};

struct skt_dev_info {
	int nskt;
	struct soc_pcmcia_socket skt[0];
};

#define SKT_DEV_INFO_SIZE(n) \
	(sizeof(struct skt_dev_info) + (n)*sizeof(struct soc_pcmcia_socket))

#ifdef CONFIG_CPU_FREQ
static int
soc_pcmcia_notifier(struct notifier_block *nb, unsigned long val, void *data)
{
	struct soc_pcmcia_socket *skt;
	struct cpufreq_freqs *freqs = data;
	int ret = 0;

	down(&soc_pcmcia_sockets_lock);
	list_for_each_entry(skt, &soc_pcmcia_sockets, node)
		if ( skt->ops->frequency_change )
			ret += skt->ops->frequency_change(skt, val, freqs);
	up(&soc_pcmcia_sockets_lock);

	return ret;
}

static struct notifier_block soc_pcmcia_notifier_block = {
	.notifier_call	= soc_pcmcia_notifier
};

static int soc_pcmcia_cpufreq_register(void)
{
	int ret;

	ret = cpufreq_register_notifier(&soc_pcmcia_notifier_block,
					CPUFREQ_TRANSITION_NOTIFIER);
	if (ret < 0)
		printk(KERN_ERR "Unable to register CPU frequency change "
				"notifier for PCMCIA (%d)\n", ret);
	return ret;
}

static void soc_pcmcia_cpufreq_unregister(void)
{
	cpufreq_unregister_notifier(&soc_pcmcia_notifier_block, CPUFREQ_TRANSITION_NOTIFIER);
}

#else
#define soc_pcmcia_cpufreq_register()
#define soc_pcmcia_cpufreq_unregister()
#endif

int soc_common_drv_pcmcia_probe(struct device *dev, struct pcmcia_low_level *ops, int first, int nr)
{
	struct skt_dev_info *sinfo;
	int ret, i;

	down(&soc_pcmcia_sockets_lock);

	sinfo = kmalloc(SKT_DEV_INFO_SIZE(nr), GFP_KERNEL);
	if (!sinfo) {
		ret = -ENOMEM;
		goto out;
	}

	memset(sinfo, 0, SKT_DEV_INFO_SIZE(nr));
	sinfo->nskt = nr;

	/*
	 * Initialise the per-socket structure.
	 */
	for (i = 0; i < nr; i++) {
		struct soc_pcmcia_socket *skt = &sinfo->skt[i];

		skt->socket.ops = &soc_common_pcmcia_operations;
		skt->socket.owner = ops->owner;
		skt->socket.dev.dev = dev;

		init_timer(&skt->poll_timer);
		skt->poll_timer.function = soc_common_pcmcia_poll_event;
		skt->poll_timer.data = (unsigned long)skt;
		skt->poll_timer.expires = jiffies + SOC_PCMCIA_POLL_PERIOD;

		skt->nr		= first + i;
		skt->irq	= NO_IRQ;
		skt->dev	= dev;
		skt->ops	= ops;

		skt->res_skt.start	= _PCMCIA(skt->nr);
		skt->res_skt.end	= _PCMCIA(skt->nr) + PCMCIASp - 1;
		skt->res_skt.name	= skt_names[skt->nr];
		skt->res_skt.flags	= IORESOURCE_MEM;

		ret = request_resource(&iomem_resource, &skt->res_skt);
		if (ret)
			goto out_err_1;

		skt->res_io.start	= _PCMCIAIO(skt->nr);
		skt->res_io.end		= _PCMCIAIO(skt->nr) + PCMCIAIOSp - 1;
		skt->res_io.name	= "io";
		skt->res_io.flags	= IORESOURCE_MEM | IORESOURCE_BUSY;

		ret = request_resource(&skt->res_skt, &skt->res_io);
		if (ret)
			goto out_err_2;

		skt->res_mem.start	= _PCMCIAMem(skt->nr);
		skt->res_mem.end	= _PCMCIAMem(skt->nr) + PCMCIAMemSp - 1;
		skt->res_mem.name	= "memory";
		skt->res_mem.flags	= IORESOURCE_MEM;

		ret = request_resource(&skt->res_skt, &skt->res_mem);
		if (ret)
			goto out_err_3;

		skt->res_attr.start	= _PCMCIAAttr(skt->nr);
		skt->res_attr.end	= _PCMCIAAttr(skt->nr) + PCMCIAAttrSp - 1;
		skt->res_attr.name	= "attribute";
		skt->res_attr.flags	= IORESOURCE_MEM;

		ret = request_resource(&skt->res_skt, &skt->res_attr);
		if (ret)
			goto out_err_4;

		skt->virt_io = ioremap(skt->res_io.start, 0x10000);
		if (skt->virt_io == NULL) {
			ret = -ENOMEM;
			goto out_err_5;
		}

		if ( list_empty(&soc_pcmcia_sockets) )
			soc_pcmcia_cpufreq_register();

		list_add(&skt->node, &soc_pcmcia_sockets);

		/*
		 * We initialize default socket timing here, because
		 * we are not guaranteed to see a SetIOMap operation at
		 * runtime.
		 */
		ops->set_timing(skt);

		ret = ops->hw_init(skt);
		if (ret)
			goto out_err_6;

		skt->socket.features = SS_CAP_STATIC_MAP|SS_CAP_PCCARD;
		skt->socket.irq_mask = 0;
		skt->socket.map_size = PAGE_SIZE;
		skt->socket.pci_irq = skt->irq;
		skt->socket.io_offset = (unsigned long)skt->virt_io;

		skt->status = soc_common_pcmcia_skt_state(skt);

		ret = pcmcia_register_socket(&skt->socket);
		if (ret)
			goto out_err_7;

		WARN_ON(skt->socket.sock != i);

		add_timer(&skt->poll_timer);

		class_device_create_file(&skt->socket.dev, &class_device_attr_status);
	}

	dev_set_drvdata(dev, sinfo);
	ret = 0;
	goto out;

	do {
		struct soc_pcmcia_socket *skt = &sinfo->skt[i];

		del_timer_sync(&skt->poll_timer);
		pcmcia_unregister_socket(&skt->socket);

 out_err_7:
		flush_scheduled_work();

		ops->hw_shutdown(skt);
 out_err_6:
 		list_del(&skt->node);
		iounmap(skt->virt_io);
 out_err_5:
		release_resource(&skt->res_attr);
 out_err_4:
		release_resource(&skt->res_mem);
 out_err_3:
		release_resource(&skt->res_io);
 out_err_2:
		release_resource(&skt->res_skt);
 out_err_1:
		i--;
	} while (i > 0);

	kfree(sinfo);

 out:
	up(&soc_pcmcia_sockets_lock);
	return ret;
}

int soc_common_drv_pcmcia_remove(struct device *dev)
{
	struct skt_dev_info *sinfo = dev_get_drvdata(dev);
	int i;

	dev_set_drvdata(dev, NULL);

	down(&soc_pcmcia_sockets_lock);
	for (i = 0; i < sinfo->nskt; i++) {
		struct soc_pcmcia_socket *skt = &sinfo->skt[i];

		del_timer_sync(&skt->poll_timer);

		pcmcia_unregister_socket(&skt->socket);

		flush_scheduled_work();

		skt->ops->hw_shutdown(skt);

		soc_common_pcmcia_config_skt(skt, &dead_socket);

		list_del(&skt->node);
		iounmap(skt->virt_io);
		skt->virt_io = NULL;
		release_resource(&skt->res_attr);
		release_resource(&skt->res_mem);
		release_resource(&skt->res_io);
		release_resource(&skt->res_skt);
	}
	if ( list_empty(&soc_pcmcia_sockets) )
		soc_pcmcia_cpufreq_unregister();

	up(&soc_pcmcia_sockets_lock);

	kfree(sinfo);

	return 0;
}
