/*
 * Copyright (c) 2001-2002 by David Brownell
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* this file is part of ehci-hcd.c */

#define ehci_dbg(ehci, fmt, args...) \
	dev_dbg (ehci_to_hcd(ehci)->self.controller , fmt , ## args )
#define ehci_err(ehci, fmt, args...) \
	dev_err (ehci_to_hcd(ehci)->self.controller , fmt , ## args )
#define ehci_info(ehci, fmt, args...) \
	dev_info (ehci_to_hcd(ehci)->self.controller , fmt , ## args )
#define ehci_warn(ehci, fmt, args...) \
	dev_warn (ehci_to_hcd(ehci)->self.controller , fmt , ## args )

#ifdef EHCI_VERBOSE_DEBUG
#	define vdbg dbg
#	define ehci_vdbg ehci_dbg
#else
#	define vdbg(fmt,args...) do { } while (0)
#	define ehci_vdbg(ehci, fmt, args...) do { } while (0)
#endif

#ifdef	DEBUG

/* check the values in the HCSPARAMS register
 * (host controller _Structural_ parameters)
 * see EHCI spec, Table 2-4 for each value
 */
static void dbg_hcs_params (struct ehci_hcd *ehci, char *label)
{
	u32	params = ehci_readl(ehci, &ehci->caps->hcs_params);

	ehci_dbg (ehci,
		"%s hcs_params 0x%x dbg=%d%s cc=%d pcc=%d%s%s ports=%d\n",
		label, params,
		HCS_DEBUG_PORT (params),
		HCS_INDICATOR (params) ? " ind" : "",
		HCS_N_CC (params),
		HCS_N_PCC (params),
		HCS_PORTROUTED (params) ? "" : " ordered",
		HCS_PPC (params) ? "" : " !ppc",
		HCS_N_PORTS (params)
		);
	/* Port routing, per EHCI 0.95 Spec, Section 2.2.5 */
	if (HCS_PORTROUTED (params)) {
		int i;
		char buf [46], tmp [7], byte;

		buf[0] = 0;
		for (i = 0; i < HCS_N_PORTS (params); i++) {
			// FIXME MIPS won't readb() ...
			byte = readb (&ehci->caps->portroute[(i>>1)]);
			sprintf(tmp, "%d ",
				((i & 0x1) ? ((byte)&0xf) : ((byte>>4)&0xf)));
			strcat(buf, tmp);
		}
		ehci_dbg (ehci, "%s portroute %s\n",
				label, buf);
	}
}
#else

static inline void dbg_hcs_params (struct ehci_hcd *ehci, char *label) {}

#endif

#ifdef	DEBUG

/* check the values in the HCCPARAMS register
 * (host controller _Capability_ parameters)
 * see EHCI Spec, Table 2-5 for each value
 * */
static void dbg_hcc_params (struct ehci_hcd *ehci, char *label)
{
	u32	params = ehci_readl(ehci, &ehci->caps->hcc_params);

	if (HCC_ISOC_CACHE (params)) {
		ehci_dbg (ehci,
			"%s hcc_params %04x caching frame %s%s%s\n",
			label, params,
			HCC_PGM_FRAMELISTLEN(params) ? "256/512/1024" : "1024",
			HCC_CANPARK(params) ? " park" : "",
			HCC_64BIT_ADDR(params) ? " 64 bit addr" : "");
	} else {
		ehci_dbg (ehci,
			"%s hcc_params %04x thresh %d uframes %s%s%s\n",
			label,
			params,
			HCC_ISOC_THRES(params),
			HCC_PGM_FRAMELISTLEN(params) ? "256/512/1024" : "1024",
			HCC_CANPARK(params) ? " park" : "",
			HCC_64BIT_ADDR(params) ? " 64 bit addr" : "");
	}
}
#else

static inline void dbg_hcc_params (struct ehci_hcd *ehci, char *label) {}

#endif

#ifdef	DEBUG

static void __maybe_unused
dbg_qtd (const char *label, struct ehci_hcd *ehci, struct ehci_qtd *qtd)
{
	ehci_dbg(ehci, "%s td %p n%08x %08x t%08x p0=%08x\n", label, qtd,
		hc32_to_cpup(ehci, &qtd->hw_next),
		hc32_to_cpup(ehci, &qtd->hw_alt_next),
		hc32_to_cpup(ehci, &qtd->hw_token),
		hc32_to_cpup(ehci, &qtd->hw_buf [0]));
	if (qtd->hw_buf [1])
		ehci_dbg(ehci, "  p1=%08x p2=%08x p3=%08x p4=%08x\n",
			hc32_to_cpup(ehci, &qtd->hw_buf[1]),
			hc32_to_cpup(ehci, &qtd->hw_buf[2]),
			hc32_to_cpup(ehci, &qtd->hw_buf[3]),
			hc32_to_cpup(ehci, &qtd->hw_buf[4]));
}

static void __maybe_unused
dbg_qh (const char *label, struct ehci_hcd *ehci, struct ehci_qh *qh)
{
	ehci_dbg (ehci, "%s qh %p n%08x info %x %x qtd %x\n", label,
		qh, qh->hw_next, qh->hw_info1, qh->hw_info2,
		qh->hw_current);
	dbg_qtd ("overlay", ehci, (struct ehci_qtd *) &qh->hw_qtd_next);
}

static void __maybe_unused
dbg_itd (const char *label, struct ehci_hcd *ehci, struct ehci_itd *itd)
{
	ehci_dbg (ehci, "%s [%d] itd %p, next %08x, urb %p\n",
		label, itd->frame, itd, hc32_to_cpu(ehci, itd->hw_next),
		itd->urb);
	ehci_dbg (ehci,
		"  trans: %08x %08x %08x %08x %08x %08x %08x %08x\n",
		hc32_to_cpu(ehci, itd->hw_transaction[0]),
		hc32_to_cpu(ehci, itd->hw_transaction[1]),
		hc32_to_cpu(ehci, itd->hw_transaction[2]),
		hc32_to_cpu(ehci, itd->hw_transaction[3]),
		hc32_to_cpu(ehci, itd->hw_transaction[4]),
		hc32_to_cpu(ehci, itd->hw_transaction[5]),
		hc32_to_cpu(ehci, itd->hw_transaction[6]),
		hc32_to_cpu(ehci, itd->hw_transaction[7]));
	ehci_dbg (ehci,
		"  buf:   %08x %08x %08x %08x %08x %08x %08x\n",
		hc32_to_cpu(ehci, itd->hw_bufp[0]),
		hc32_to_cpu(ehci, itd->hw_bufp[1]),
		hc32_to_cpu(ehci, itd->hw_bufp[2]),
		hc32_to_cpu(ehci, itd->hw_bufp[3]),
		hc32_to_cpu(ehci, itd->hw_bufp[4]),
		hc32_to_cpu(ehci, itd->hw_bufp[5]),
		hc32_to_cpu(ehci, itd->hw_bufp[6]));
	ehci_dbg (ehci, "  index: %d %d %d %d %d %d %d %d\n",
		itd->index[0], itd->index[1], itd->index[2],
		itd->index[3], itd->index[4], itd->index[5],
		itd->index[6], itd->index[7]);
}

static void __maybe_unused
dbg_sitd (const char *label, struct ehci_hcd *ehci, struct ehci_sitd *sitd)
{
	ehci_dbg (ehci, "%s [%d] sitd %p, next %08x, urb %p\n",
		label, sitd->frame, sitd, hc32_to_cpu(ehci, sitd->hw_next),
		sitd->urb);
	ehci_dbg (ehci,
		"  addr %08x sched %04x result %08x buf %08x %08x\n",
		hc32_to_cpu(ehci, sitd->hw_fullspeed_ep),
		hc32_to_cpu(ehci, sitd->hw_uframe),
		hc32_to_cpu(ehci, sitd->hw_results),
		hc32_to_cpu(ehci, sitd->hw_buf[0]),
		hc32_to_cpu(ehci, sitd->hw_buf[1]));
}

static int __maybe_unused
dbg_status_buf (char *buf, unsigned len, const char *label, u32 status)
{
	return scnprintf (buf, len,
		"%s%sstatus %04x%s%s%s%s%s%s%s%s%s%s",
		label, label [0] ? " " : "", status,
		(status & STS_ASS) ? " Async" : "",
		(status & STS_PSS) ? " Periodic" : "",
		(status & STS_RECL) ? " Recl" : "",
		(status & STS_HALT) ? " Halt" : "",
		(status & STS_IAA) ? " IAA" : "",
		(status & STS_FATAL) ? " FATAL" : "",
		(status & STS_FLR) ? " FLR" : "",
		(status & STS_PCD) ? " PCD" : "",
		(status & STS_ERR) ? " ERR" : "",
		(status & STS_INT) ? " INT" : ""
		);
}

static int __maybe_unused
dbg_intr_buf (char *buf, unsigned len, const char *label, u32 enable)
{
	return scnprintf (buf, len,
		"%s%sintrenable %02x%s%s%s%s%s%s",
		label, label [0] ? " " : "", enable,
		(enable & STS_IAA) ? " IAA" : "",
		(enable & STS_FATAL) ? " FATAL" : "",
		(enable & STS_FLR) ? " FLR" : "",
		(enable & STS_PCD) ? " PCD" : "",
		(enable & STS_ERR) ? " ERR" : "",
		(enable & STS_INT) ? " INT" : ""
		);
}

static const char *const fls_strings [] =
    { "1024", "512", "256", "??" };

static int
dbg_command_buf (char *buf, unsigned len, const char *label, u32 command)
{
	return scnprintf (buf, len,
		"%s%scommand %06x %s=%d ithresh=%d%s%s%s%s period=%s%s %s",
		label, label [0] ? " " : "", command,
		(command & CMD_PARK) ? "park" : "(park)",
		CMD_PARK_CNT (command),
		(command >> 16) & 0x3f,
		(command & CMD_LRESET) ? " LReset" : "",
		(command & CMD_IAAD) ? " IAAD" : "",
		(command & CMD_ASE) ? " Async" : "",
		(command & CMD_PSE) ? " Periodic" : "",
		fls_strings [(command >> 2) & 0x3],
		(command & CMD_RESET) ? " Reset" : "",
		(command & CMD_RUN) ? "RUN" : "HALT"
		);
}

static int
dbg_port_buf (char *buf, unsigned len, const char *label, int port, u32 status)
{
	char	*sig;

	/* signaling state */
	switch (status & (3 << 10)) {
	case 0 << 10: sig = "se0"; break;
	case 1 << 10: sig = "k"; break;		/* low speed */
	case 2 << 10: sig = "j"; break;
	default: sig = "?"; break;
	}

	return scnprintf (buf, len,
		"%s%sport %d status %06x%s%s sig=%s%s%s%s%s%s%s%s%s%s",
		label, label [0] ? " " : "", port, status,
		(status & PORT_POWER) ? " POWER" : "",
		(status & PORT_OWNER) ? " OWNER" : "",
		sig,
		(status & PORT_RESET) ? " RESET" : "",
		(status & PORT_SUSPEND) ? " SUSPEND" : "",
		(status & PORT_RESUME) ? " RESUME" : "",
		(status & PORT_OCC) ? " OCC" : "",
		(status & PORT_OC) ? " OC" : "",
		(status & PORT_PEC) ? " PEC" : "",
		(status & PORT_PE) ? " PE" : "",
		(status & PORT_CSC) ? " CSC" : "",
		(status & PORT_CONNECT) ? " CONNECT" : "");
}

#else
static inline void __maybe_unused
dbg_qh (char *label, struct ehci_hcd *ehci, struct ehci_qh *qh)
{}

static inline int __maybe_unused
dbg_status_buf (char *buf, unsigned len, const char *label, u32 status)
{ return 0; }

static inline int __maybe_unused
dbg_command_buf (char *buf, unsigned len, const char *label, u32 command)
{ return 0; }

static inline int __maybe_unused
dbg_intr_buf (char *buf, unsigned len, const char *label, u32 enable)
{ return 0; }

static inline int __maybe_unused
dbg_port_buf (char *buf, unsigned len, const char *label, int port, u32 status)
{ return 0; }

#endif	/* DEBUG */

/* functions have the "wrong" filename when they're output... */
#define dbg_status(ehci, label, status) { \
	char _buf [80]; \
	dbg_status_buf (_buf, sizeof _buf, label, status); \
	ehci_dbg (ehci, "%s\n", _buf); \
}

#define dbg_cmd(ehci, label, command) { \
	char _buf [80]; \
	dbg_command_buf (_buf, sizeof _buf, label, command); \
	ehci_dbg (ehci, "%s\n", _buf); \
}

#define dbg_port(ehci, label, port, status) { \
	char _buf [80]; \
	dbg_port_buf (_buf, sizeof _buf, label, port, status); \
	ehci_dbg (ehci, "%s\n", _buf); \
}

/*-------------------------------------------------------------------------*/

#ifdef STUB_DEBUG_FILES

static inline void create_debug_files (struct ehci_hcd *bus) { }
static inline void remove_debug_files (struct ehci_hcd *bus) { }

#else

/* troubleshooting help: expose state in sysfs */

#define speed_char(info1) ({ char tmp; \
		switch (info1 & (3 << 12)) { \
		case 0 << 12: tmp = 'f'; break; \
		case 1 << 12: tmp = 'l'; break; \
		case 2 << 12: tmp = 'h'; break; \
		default: tmp = '?'; break; \
		}; tmp; })

static inline char token_mark(struct ehci_hcd *ehci, __hc32 token)
{
	__u32 v = hc32_to_cpu(ehci, token);

	if (v & QTD_STS_ACTIVE)
		return '*';
	if (v & QTD_STS_HALT)
		return '-';
	if (!IS_SHORT_READ (v))
		return ' ';
	/* tries to advance through hw_alt_next */
	return '/';
}

static void qh_lines (
	struct ehci_hcd *ehci,
	struct ehci_qh *qh,
	char **nextp,
	unsigned *sizep
)
{
	u32			scratch;
	u32			hw_curr;
	struct list_head	*entry;
	struct ehci_qtd		*td;
	unsigned		temp;
	unsigned		size = *sizep;
	char			*next = *nextp;
	char			mark;
	u32			list_end = EHCI_LIST_END(ehci);

	if (qh->hw_qtd_next == list_end)	/* NEC does this */
		mark = '@';
	else
		mark = token_mark(ehci, qh->hw_token);
	if (mark == '/') {	/* qh_alt_next controls qh advance? */
		if ((qh->hw_alt_next & QTD_MASK(ehci))
				== ehci->async->hw_alt_next)
			mark = '#';	/* blocked */
		else if (qh->hw_alt_next == list_end)
			mark = '.';	/* use hw_qtd_next */
		/* else alt_next points to some other qtd */
	}
	scratch = hc32_to_cpup(ehci, &qh->hw_info1);
	hw_curr = (mark == '*') ? hc32_to_cpup(ehci, &qh->hw_current) : 0;
	temp = scnprintf (next, size,
			"qh/%p dev%d %cs ep%d %08x %08x (%08x%c %s nak%d)",
			qh, scratch & 0x007f,
			speed_char (scratch),
			(scratch >> 8) & 0x000f,
			scratch, hc32_to_cpup(ehci, &qh->hw_info2),
			hc32_to_cpup(ehci, &qh->hw_token), mark,
			(cpu_to_hc32(ehci, QTD_TOGGLE) & qh->hw_token)
				? "data1" : "data0",
			(hc32_to_cpup(ehci, &qh->hw_alt_next) >> 1) & 0x0f);
	size -= temp;
	next += temp;

	/* hc may be modifying the list as we read it ... */
	list_for_each (entry, &qh->qtd_list) {
		td = list_entry (entry, struct ehci_qtd, qtd_list);
		scratch = hc32_to_cpup(ehci, &td->hw_token);
		mark = ' ';
		if (hw_curr == td->qtd_dma)
			mark = '*';
		else if (qh->hw_qtd_next == cpu_to_hc32(ehci, td->qtd_dma))
			mark = '+';
		else if (QTD_LENGTH (scratch)) {
			if (td->hw_alt_next == ehci->async->hw_alt_next)
				mark = '#';
			else if (td->hw_alt_next != list_end)
				mark = '/';
		}
		temp = snprintf (next, size,
				"\n\t%p%c%s len=%d %08x urb %p",
				td, mark, ({ char *tmp;
				 switch ((scratch>>8)&0x03) {
				 case 0: tmp = "out"; break;
				 case 1: tmp = "in"; break;
				 case 2: tmp = "setup"; break;
				 default: tmp = "?"; break;
				 } tmp;}),
				(scratch >> 16) & 0x7fff,
				scratch,
				td->urb);
		if (temp < 0)
			temp = 0;
		else if (size < temp)
			temp = size;
		size -= temp;
		next += temp;
		if (temp == size)
			goto done;
	}

	temp = snprintf (next, size, "\n");
	if (temp < 0)
		temp = 0;
	else if (size < temp)
		temp = size;
	size -= temp;
	next += temp;

done:
	*sizep = size;
	*nextp = next;
}

static ssize_t
show_async (struct class_device *class_dev, char *buf)
{
	struct usb_bus		*bus;
	struct usb_hcd		*hcd;
	struct ehci_hcd		*ehci;
	unsigned long		flags;
	unsigned		temp, size;
	char			*next;
	struct ehci_qh		*qh;

	*buf = 0;

	bus = class_get_devdata(class_dev);
	hcd = bus_to_hcd(bus);
	ehci = hcd_to_ehci (hcd);
	next = buf;
	size = PAGE_SIZE;

	/* dumps a snapshot of the async schedule.
	 * usually empty except for long-term bulk reads, or head.
	 * one QH per line, and TDs we know about
	 */
	spin_lock_irqsave (&ehci->lock, flags);
	for (qh = ehci->async->qh_next.qh; size > 0 && qh; qh = qh->qh_next.qh)
		qh_lines (ehci, qh, &next, &size);
	if (ehci->reclaim && size > 0) {
		temp = scnprintf (next, size, "\nreclaim =\n");
		size -= temp;
		next += temp;

		for (qh = ehci->reclaim; size > 0 && qh; qh = qh->reclaim)
			qh_lines (ehci, qh, &next, &size);
	}
	spin_unlock_irqrestore (&ehci->lock, flags);

	return strlen (buf);
}
static CLASS_DEVICE_ATTR (async, S_IRUGO, show_async, NULL);

#define DBG_SCHED_LIMIT 64

static ssize_t
show_periodic (struct class_device *class_dev, char *buf)
{
	struct usb_bus		*bus;
	struct usb_hcd		*hcd;
	struct ehci_hcd		*ehci;
	unsigned long		flags;
	union ehci_shadow	p, *seen;
	unsigned		temp, size, seen_count;
	char			*next;
	unsigned		i;
	__hc32			tag;

	if (!(seen = kmalloc (DBG_SCHED_LIMIT * sizeof *seen, GFP_ATOMIC)))
		return 0;
	seen_count = 0;

	bus = class_get_devdata(class_dev);
	hcd = bus_to_hcd(bus);
	ehci = hcd_to_ehci (hcd);
	next = buf;
	size = PAGE_SIZE;

	temp = scnprintf (next, size, "size = %d\n", ehci->periodic_size);
	size -= temp;
	next += temp;

	/* dump a snapshot of the periodic schedule.
	 * iso changes, interrupt usually doesn't.
	 */
	spin_lock_irqsave (&ehci->lock, flags);
	for (i = 0; i < ehci->periodic_size; i++) {
		p = ehci->pshadow [i];
		if (likely (!p.ptr))
			continue;
		tag = Q_NEXT_TYPE(ehci, ehci->periodic [i]);

		temp = scnprintf (next, size, "%4d: ", i);
		size -= temp;
		next += temp;

		do {
			switch (hc32_to_cpu(ehci, tag)) {
			case Q_TYPE_QH:
				temp = scnprintf (next, size, " qh%d-%04x/%p",
						p.qh->period,
						hc32_to_cpup(ehci,
								&p.qh->hw_info2)
							/* uframe masks */
							& (QH_CMASK | QH_SMASK),
						p.qh);
				size -= temp;
				next += temp;
				/* don't repeat what follows this qh */
				for (temp = 0; temp < seen_count; temp++) {
					if (seen [temp].ptr != p.ptr)
						continue;
					if (p.qh->qh_next.ptr)
						temp = scnprintf (next, size,
							" ...");
					p.ptr = NULL;
					break;
				}
				/* show more info the first time around */
				if (temp == seen_count && p.ptr) {
					u32	scratch = hc32_to_cpup(ehci,
							&p.qh->hw_info1);
					struct ehci_qtd	*qtd;
					char		*type = "";

					/* count tds, get ep direction */
					temp = 0;
					list_for_each_entry (qtd,
							&p.qh->qtd_list,
							qtd_list) {
						temp++;
						switch (0x03 & (hc32_to_cpu(
							ehci,
							qtd->hw_token) >> 8)) {
						case 0: type = "out"; continue;
						case 1: type = "in"; continue;
						}
					}

					temp = scnprintf (next, size,
						" (%c%d ep%d%s "
						"[%d/%d] q%d p%d)",
						speed_char (scratch),
						scratch & 0x007f,
						(scratch >> 8) & 0x000f, type,
						p.qh->usecs, p.qh->c_usecs,
						temp,
						0x7ff & (scratch >> 16));

					if (seen_count < DBG_SCHED_LIMIT)
						seen [seen_count++].qh = p.qh;
				} else
					temp = 0;
				if (p.qh) {
					tag = Q_NEXT_TYPE(ehci, p.qh->hw_next);
					p = p.qh->qh_next;
				}
				break;
			case Q_TYPE_FSTN:
				temp = scnprintf (next, size,
					" fstn-%8x/%p", p.fstn->hw_prev,
					p.fstn);
				tag = Q_NEXT_TYPE(ehci, p.fstn->hw_next);
				p = p.fstn->fstn_next;
				break;
			case Q_TYPE_ITD:
				temp = scnprintf (next, size,
					" itd/%p", p.itd);
				tag = Q_NEXT_TYPE(ehci, p.itd->hw_next);
				p = p.itd->itd_next;
				break;
			case Q_TYPE_SITD:
				temp = scnprintf (next, size,
					" sitd%d-%04x/%p",
					p.sitd->stream->interval,
					hc32_to_cpup(ehci, &p.sitd->hw_uframe)
						& 0x0000ffff,
					p.sitd);
				tag = Q_NEXT_TYPE(ehci, p.sitd->hw_next);
				p = p.sitd->sitd_next;
				break;
			}
			size -= temp;
			next += temp;
		} while (p.ptr);

		temp = scnprintf (next, size, "\n");
		size -= temp;
		next += temp;
	}
	spin_unlock_irqrestore (&ehci->lock, flags);
	kfree (seen);

	return PAGE_SIZE - size;
}
static CLASS_DEVICE_ATTR (periodic, S_IRUGO, show_periodic, NULL);

#undef DBG_SCHED_LIMIT

static ssize_t
show_registers (struct class_device *class_dev, char *buf)
{
	struct usb_bus		*bus;
	struct usb_hcd		*hcd;
	struct ehci_hcd		*ehci;
	unsigned long		flags;
	unsigned		temp, size, i;
	char			*next, scratch [80];
	static char		fmt [] = "%*s\n";
	static char		label [] = "";

	bus = class_get_devdata(class_dev);
	hcd = bus_to_hcd(bus);
	ehci = hcd_to_ehci (hcd);
	next = buf;
	size = PAGE_SIZE;

	spin_lock_irqsave (&ehci->lock, flags);

	if (bus->controller->power.power_state.event) {
		size = scnprintf (next, size,
			"bus %s, device %s (driver " DRIVER_VERSION ")\n"
			"%s\n"
			"SUSPENDED (no register access)\n",
			hcd->self.controller->bus->name,
			hcd->self.controller->bus_id,
			hcd->product_desc);
		goto done;
	}

	/* Capability Registers */
	i = HC_VERSION(ehci_readl(ehci, &ehci->caps->hc_capbase));
	temp = scnprintf (next, size,
		"bus %s, device %s (driver " DRIVER_VERSION ")\n"
		"%s\n"
		"EHCI %x.%02x, hcd state %d\n",
		hcd->self.controller->bus->name,
		hcd->self.controller->bus_id,
		hcd->product_desc,
		i >> 8, i & 0x0ff, hcd->state);
	size -= temp;
	next += temp;

#ifdef	CONFIG_PCI
	/* EHCI 0.96 and later may have "extended capabilities" */
	if (hcd->self.controller->bus == &pci_bus_type) {
		struct pci_dev	*pdev;
		u32		offset, cap, cap2;
		unsigned	count = 256/4;

		pdev = to_pci_dev(ehci_to_hcd(ehci)->self.controller);
		offset = HCC_EXT_CAPS(ehci_readl(ehci,
				&ehci->caps->hcc_params));
		while (offset && count--) {
			pci_read_config_dword (pdev, offset, &cap);
			switch (cap & 0xff) {
			case 1:
				temp = scnprintf (next, size,
					"ownership %08x%s%s\n", cap,
					(cap & (1 << 24)) ? " linux" : "",
					(cap & (1 << 16)) ? " firmware" : "");
				size -= temp;
				next += temp;

				offset += 4;
				pci_read_config_dword (pdev, offset, &cap2);
				temp = scnprintf (next, size,
					"SMI sts/enable 0x%08x\n", cap2);
				size -= temp;
				next += temp;
				break;
			case 0:		/* illegal reserved capability */
				cap = 0;
				/* FALLTHROUGH */
			default:		/* unknown */
				break;
			}
			temp = (cap >> 8) & 0xff;
		}
	}
#endif

	// FIXME interpret both types of params
	i = ehci_readl(ehci, &ehci->caps->hcs_params);
	temp = scnprintf (next, size, "structural params 0x%08x\n", i);
	size -= temp;
	next += temp;

	i = ehci_readl(ehci, &ehci->caps->hcc_params);
	temp = scnprintf (next, size, "capability params 0x%08x\n", i);
	size -= temp;
	next += temp;

	/* Operational Registers */
	temp = dbg_status_buf (scratch, sizeof scratch, label,
			ehci_readl(ehci, &ehci->regs->status));
	temp = scnprintf (next, size, fmt, temp, scratch);
	size -= temp;
	next += temp;

	temp = dbg_command_buf (scratch, sizeof scratch, label,
			ehci_readl(ehci, &ehci->regs->command));
	temp = scnprintf (next, size, fmt, temp, scratch);
	size -= temp;
	next += temp;

	temp = dbg_intr_buf (scratch, sizeof scratch, label,
			ehci_readl(ehci, &ehci->regs->intr_enable));
	temp = scnprintf (next, size, fmt, temp, scratch);
	size -= temp;
	next += temp;

	temp = scnprintf (next, size, "uframe %04x\n",
			ehci_readl(ehci, &ehci->regs->frame_index));
	size -= temp;
	next += temp;

	for (i = 1; i <= HCS_N_PORTS (ehci->hcs_params); i++) {
		temp = dbg_port_buf (scratch, sizeof scratch, label, i,
				ehci_readl(ehci,
					&ehci->regs->port_status[i - 1]));
		temp = scnprintf (next, size, fmt, temp, scratch);
		size -= temp;
		next += temp;
		if (i == HCS_DEBUG_PORT(ehci->hcs_params) && ehci->debug) {
			temp = scnprintf (next, size,
					"    debug control %08x\n",
					ehci_readl(ehci,
						&ehci->debug->control));
			size -= temp;
			next += temp;
		}
	}

	if (ehci->reclaim) {
		temp = scnprintf (next, size, "reclaim qh %p%s\n",
				ehci->reclaim,
				ehci->reclaim_ready ? " ready" : "");
		size -= temp;
		next += temp;
	}

#ifdef EHCI_STATS
	temp = scnprintf (next, size,
		"irq normal %ld err %ld reclaim %ld (lost %ld)\n",
		ehci->stats.normal, ehci->stats.error, ehci->stats.reclaim,
		ehci->stats.lost_iaa);
	size -= temp;
	next += temp;

	temp = scnprintf (next, size, "complete %ld unlink %ld\n",
		ehci->stats.complete, ehci->stats.unlink);
	size -= temp;
	next += temp;
#endif

done:
	spin_unlock_irqrestore (&ehci->lock, flags);

	return PAGE_SIZE - size;
}
static CLASS_DEVICE_ATTR (registers, S_IRUGO, show_registers, NULL);

static inline void create_debug_files (struct ehci_hcd *ehci)
{
	struct class_device *cldev = ehci_to_hcd(ehci)->self.class_dev;
	int retval;

	retval = class_device_create_file(cldev, &class_device_attr_async);
	retval = class_device_create_file(cldev, &class_device_attr_periodic);
	retval = class_device_create_file(cldev, &class_device_attr_registers);
}

static inline void remove_debug_files (struct ehci_hcd *ehci)
{
	struct class_device *cldev = ehci_to_hcd(ehci)->self.class_dev;

	class_device_remove_file(cldev, &class_device_attr_async);
	class_device_remove_file(cldev, &class_device_attr_periodic);
	class_device_remove_file(cldev, &class_device_attr_registers);
}

#endif /* STUB_DEBUG_FILES */

