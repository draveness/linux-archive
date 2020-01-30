/*
 * OHCI HCD (Host Controller Driver) for USB.
 *
 * (C) Copyright 1999 Roman Weissgaerber <weissg@vienna.at>
 * (C) Copyright 2000-2002 David Brownell <dbrownell@users.sourceforge.net>
 *
 * This file is licenced under the GPL.
 */

/*-------------------------------------------------------------------------*/

#ifdef DEBUG

#define edstring(ed_type) ({ char *temp; \
	switch (ed_type) { \
	case PIPE_CONTROL:	temp = "ctrl"; break; \
	case PIPE_BULK:		temp = "bulk"; break; \
	case PIPE_INTERRUPT:	temp = "intr"; break; \
	default: 		temp = "isoc"; break; \
	}; temp;})
#define pipestring(pipe) edstring(usb_pipetype(pipe))

/* debug| print the main components of an URB
 * small: 0) header + data packets 1) just header
 */
static void __attribute__((unused))
urb_print (struct urb * urb, char * str, int small)
{
	unsigned int pipe= urb->pipe;

	if (!urb->dev || !urb->dev->bus) {
		dbg("%s URB: no dev", str);
		return;
	}

#ifndef	OHCI_VERBOSE_DEBUG
	if (urb->status != 0)
#endif
	dbg("%s %p dev=%d ep=%d%s-%s flags=%x len=%d/%d stat=%d",
		    str,
		    urb,
		    usb_pipedevice (pipe),
		    usb_pipeendpoint (pipe),
		    usb_pipeout (pipe)? "out" : "in",
		    pipestring (pipe),
		    urb->transfer_flags,
		    urb->actual_length,
		    urb->transfer_buffer_length,
		    urb->status);

#ifdef	OHCI_VERBOSE_DEBUG
	if (!small) {
		int i, len;

		if (usb_pipecontrol (pipe)) {
			printk (KERN_DEBUG __FILE__ ": setup(8):");
			for (i = 0; i < 8 ; i++)
				printk (" %02x", ((__u8 *) urb->setup_packet) [i]);
			printk ("\n");
		}
		if (urb->transfer_buffer_length > 0 && urb->transfer_buffer) {
			printk (KERN_DEBUG __FILE__ ": data(%d/%d):",
				urb->actual_length,
				urb->transfer_buffer_length);
			len = usb_pipeout (pipe)?
						urb->transfer_buffer_length: urb->actual_length;
			for (i = 0; i < 16 && i < len; i++)
				printk (" %02x", ((__u8 *) urb->transfer_buffer) [i]);
			printk ("%s stat:%d\n", i < len? "...": "", urb->status);
		}
	}
#endif
}

#define ohci_dbg_sw(ohci, next, size, format, arg...) \
	do { \
	if (next) { \
		unsigned s_len; \
		s_len = scnprintf (*next, *size, format, ## arg ); \
		*size -= s_len; *next += s_len; \
	} else \
		ohci_dbg(ohci,format, ## arg ); \
	} while (0);


static void ohci_dump_intr_mask (
	struct ohci_hcd *ohci,
	char *label,
	u32 mask,
	char **next,
	unsigned *size)
{
	ohci_dbg_sw (ohci, next, size, "%s 0x%08x%s%s%s%s%s%s%s%s%s\n",
		label,
		mask,
		(mask & OHCI_INTR_MIE) ? " MIE" : "",
		(mask & OHCI_INTR_OC) ? " OC" : "",
		(mask & OHCI_INTR_RHSC) ? " RHSC" : "",
		(mask & OHCI_INTR_FNO) ? " FNO" : "",
		(mask & OHCI_INTR_UE) ? " UE" : "",
		(mask & OHCI_INTR_RD) ? " RD" : "",
		(mask & OHCI_INTR_SF) ? " SF" : "",
		(mask & OHCI_INTR_WDH) ? " WDH" : "",
		(mask & OHCI_INTR_SO) ? " SO" : ""
		);
}

static void maybe_print_eds (
	struct ohci_hcd *ohci,
	char *label,
	u32 value,
	char **next,
	unsigned *size)
{
	if (value)
		ohci_dbg_sw (ohci, next, size, "%s %08x\n", label, value);
}

static char *hcfs2string (int state)
{
	switch (state) {
		case OHCI_USB_RESET:	return "reset";
		case OHCI_USB_RESUME:	return "resume";
		case OHCI_USB_OPER:	return "operational";
		case OHCI_USB_SUSPEND:	return "suspend";
	}
	return "?";
}

// dump control and status registers
static void
ohci_dump_status (struct ohci_hcd *controller, char **next, unsigned *size)
{
	struct ohci_regs	*regs = controller->regs;
	u32			temp;

	temp = ohci_readl (&regs->revision) & 0xff;
	ohci_dbg_sw (controller, next, size,
		"OHCI %d.%d, %s legacy support registers\n",
		0x03 & (temp >> 4), (temp & 0x0f),
		(temp & 0x10) ? "with" : "NO");

	temp = ohci_readl (&regs->control);
	ohci_dbg_sw (controller, next, size,
		"control 0x%03x%s%s%s HCFS=%s%s%s%s%s CBSR=%d\n",
		temp,
		(temp & OHCI_CTRL_RWE) ? " RWE" : "",
		(temp & OHCI_CTRL_RWC) ? " RWC" : "",
		(temp & OHCI_CTRL_IR) ? " IR" : "",
		hcfs2string (temp & OHCI_CTRL_HCFS),
		(temp & OHCI_CTRL_BLE) ? " BLE" : "",
		(temp & OHCI_CTRL_CLE) ? " CLE" : "",
		(temp & OHCI_CTRL_IE) ? " IE" : "",
		(temp & OHCI_CTRL_PLE) ? " PLE" : "",
		temp & OHCI_CTRL_CBSR
		);

	temp = ohci_readl (&regs->cmdstatus);
	ohci_dbg_sw (controller, next, size,
		"cmdstatus 0x%05x SOC=%d%s%s%s%s\n", temp,
		(temp & OHCI_SOC) >> 16,
		(temp & OHCI_OCR) ? " OCR" : "",
		(temp & OHCI_BLF) ? " BLF" : "",
		(temp & OHCI_CLF) ? " CLF" : "",
		(temp & OHCI_HCR) ? " HCR" : ""
		);

	ohci_dump_intr_mask (controller, "intrstatus",
			ohci_readl (&regs->intrstatus), next, size);
	ohci_dump_intr_mask (controller, "intrenable",
			ohci_readl (&regs->intrenable), next, size);
	// intrdisable always same as intrenable

	maybe_print_eds (controller, "ed_periodcurrent",
			ohci_readl (&regs->ed_periodcurrent), next, size);

	maybe_print_eds (controller, "ed_controlhead",
			ohci_readl (&regs->ed_controlhead), next, size);
	maybe_print_eds (controller, "ed_controlcurrent",
			ohci_readl (&regs->ed_controlcurrent), next, size);

	maybe_print_eds (controller, "ed_bulkhead",
			ohci_readl (&regs->ed_bulkhead), next, size);
	maybe_print_eds (controller, "ed_bulkcurrent",
			ohci_readl (&regs->ed_bulkcurrent), next, size);

	maybe_print_eds (controller, "donehead",
			ohci_readl (&regs->donehead), next, size);
}

#define dbg_port_sw(hc,num,value,next,size) \
	ohci_dbg_sw (hc, next, size, \
		"roothub.portstatus [%d] " \
		"0x%08x%s%s%s%s%s%s%s%s%s%s%s%s\n", \
		num, temp, \
		(temp & RH_PS_PRSC) ? " PRSC" : "", \
		(temp & RH_PS_OCIC) ? " OCIC" : "", \
		(temp & RH_PS_PSSC) ? " PSSC" : "", \
		(temp & RH_PS_PESC) ? " PESC" : "", \
		(temp & RH_PS_CSC) ? " CSC" : "", \
 		\
		(temp & RH_PS_LSDA) ? " LSDA" : "", \
		(temp & RH_PS_PPS) ? " PPS" : "", \
		(temp & RH_PS_PRS) ? " PRS" : "", \
		(temp & RH_PS_POCI) ? " POCI" : "", \
		(temp & RH_PS_PSS) ? " PSS" : "", \
 		\
		(temp & RH_PS_PES) ? " PES" : "", \
		(temp & RH_PS_CCS) ? " CCS" : "" \
		);


static void
ohci_dump_roothub (
	struct ohci_hcd *controller,
	int verbose,
	char **next,
	unsigned *size)
{
	u32			temp, ndp, i;

	temp = roothub_a (controller);
	if (temp == ~(u32)0)
		return;
	ndp = (temp & RH_A_NDP);

	if (verbose) {
		ohci_dbg_sw (controller, next, size,
			"roothub.a %08x POTPGT=%d%s%s%s%s%s NDP=%d\n", temp,
			((temp & RH_A_POTPGT) >> 24) & 0xff,
			(temp & RH_A_NOCP) ? " NOCP" : "",
			(temp & RH_A_OCPM) ? " OCPM" : "",
			(temp & RH_A_DT) ? " DT" : "",
			(temp & RH_A_NPS) ? " NPS" : "",
			(temp & RH_A_PSM) ? " PSM" : "",
			ndp
			);
		temp = roothub_b (controller);
		ohci_dbg_sw (controller, next, size,
			"roothub.b %08x PPCM=%04x DR=%04x\n",
			temp,
			(temp & RH_B_PPCM) >> 16,
			(temp & RH_B_DR)
			);
		temp = roothub_status (controller);
		ohci_dbg_sw (controller, next, size,
			"roothub.status %08x%s%s%s%s%s%s\n",
			temp,
			(temp & RH_HS_CRWE) ? " CRWE" : "",
			(temp & RH_HS_OCIC) ? " OCIC" : "",
			(temp & RH_HS_LPSC) ? " LPSC" : "",
			(temp & RH_HS_DRWE) ? " DRWE" : "",
			(temp & RH_HS_OCI) ? " OCI" : "",
			(temp & RH_HS_LPS) ? " LPS" : ""
			);
	}

	for (i = 0; i < ndp; i++) {
		temp = roothub_portstatus (controller, i);
		dbg_port_sw (controller, i, temp, next, size);
	}
}

static void ohci_dump (struct ohci_hcd *controller, int verbose)
{
	ohci_dbg (controller, "OHCI controller state\n");

	// dumps some of the state we know about
	ohci_dump_status (controller, NULL, NULL);
	if (controller->hcca)
		ohci_dbg (controller,
			"hcca frame #%04x\n", OHCI_FRAME_NO(controller->hcca));
	ohci_dump_roothub (controller, 1, NULL, NULL);
}

static const char data0 [] = "DATA0";
static const char data1 [] = "DATA1";

static void ohci_dump_td (const struct ohci_hcd *ohci, const char *label,
		const struct td *td)
{
	u32	tmp = le32_to_cpup (&td->hwINFO);

	ohci_dbg (ohci, "%s td %p%s; urb %p index %d; hw next td %08x\n",
		label, td,
		(tmp & TD_DONE) ? " (DONE)" : "",
		td->urb, td->index,
		le32_to_cpup (&td->hwNextTD));
	if ((tmp & TD_ISO) == 0) {
		const char	*toggle, *pid;
		u32	cbp, be;

		switch (tmp & TD_T) {
		case TD_T_DATA0: toggle = data0; break;
		case TD_T_DATA1: toggle = data1; break;
		case TD_T_TOGGLE: toggle = "(CARRY)"; break;
		default: toggle = "(?)"; break;
		}
		switch (tmp & TD_DP) {
		case TD_DP_SETUP: pid = "SETUP"; break;
		case TD_DP_IN: pid = "IN"; break;
		case TD_DP_OUT: pid = "OUT"; break;
		default: pid = "(bad pid)"; break;
		}
		ohci_dbg (ohci, "     info %08x CC=%x %s DI=%d %s %s\n", tmp,
			TD_CC_GET(tmp), /* EC, */ toggle,
			(tmp & TD_DI) >> 21, pid,
			(tmp & TD_R) ? "R" : "");
		cbp = le32_to_cpup (&td->hwCBP);
		be = le32_to_cpup (&td->hwBE);
		ohci_dbg (ohci, "     cbp %08x be %08x (len %d)\n", cbp, be,
			cbp ? (be + 1 - cbp) : 0);
	} else {
		unsigned	i;
		ohci_dbg (ohci, "  info %08x CC=%x FC=%d DI=%d SF=%04x\n", tmp,
			TD_CC_GET(tmp),
			(tmp >> 24) & 0x07,
			(tmp & TD_DI) >> 21,
			tmp & 0x0000ffff);
		ohci_dbg (ohci, "  bp0 %08x be %08x\n",
			le32_to_cpup (&td->hwCBP) & ~0x0fff,
			le32_to_cpup (&td->hwBE));
		for (i = 0; i < MAXPSW; i++) {
			u16	psw = le16_to_cpup (&td->hwPSW [i]);
			int	cc = (psw >> 12) & 0x0f;
			ohci_dbg (ohci, "    psw [%d] = %2x, CC=%x %s=%d\n", i,
				psw, cc,
				(cc >= 0x0e) ? "OFFSET" : "SIZE",
				psw & 0x0fff);
		}
	}
}

/* caller MUST own hcd spinlock if verbose is set! */
static void __attribute__((unused))
ohci_dump_ed (const struct ohci_hcd *ohci, const char *label,
		const struct ed *ed, int verbose)
{
	u32	tmp = ed->hwINFO;
	char	*type = "";

	ohci_dbg (ohci, "%s, ed %p state 0x%x type %s; next ed %08x\n",
		label,
		ed, ed->state, edstring (ed->type),
		le32_to_cpup (&ed->hwNextED));
	switch (tmp & (ED_IN|ED_OUT)) {
	case ED_OUT: type = "-OUT"; break;
	case ED_IN: type = "-IN"; break;
	/* else from TDs ... control */
	}
	ohci_dbg (ohci,
		"  info %08x MAX=%d%s%s%s%s EP=%d%s DEV=%d\n", le32_to_cpu (tmp),
		0x03ff & (le32_to_cpu (tmp) >> 16),
		(tmp & ED_DEQUEUE) ? " DQ" : "",
		(tmp & ED_ISO) ? " ISO" : "",
		(tmp & ED_SKIP) ? " SKIP" : "",
		(tmp & ED_LOWSPEED) ? " LOW" : "",
		0x000f & (le32_to_cpu (tmp) >> 7),
		type,
		0x007f & le32_to_cpu (tmp));
	ohci_dbg (ohci, "  tds: head %08x %s%s tail %08x%s\n",
		tmp = le32_to_cpup (&ed->hwHeadP),
		(ed->hwHeadP & ED_C) ? data1 : data0,
		(ed->hwHeadP & ED_H) ? " HALT" : "",
		le32_to_cpup (&ed->hwTailP),
		verbose ? "" : " (not listing)");
	if (verbose) {
		struct list_head	*tmp;

		/* use ed->td_list because HC concurrently modifies
		 * hwNextTD as it accumulates ed_donelist.
		 */
		list_for_each (tmp, &ed->td_list) {
			struct td		*td;
			td = list_entry (tmp, struct td, td_list);
			ohci_dump_td (ohci, "  ->", td);
		}
	}
}

#else
static inline void ohci_dump (struct ohci_hcd *controller, int verbose) {}

#undef OHCI_VERBOSE_DEBUG

#endif /* DEBUG */

/*-------------------------------------------------------------------------*/

#ifdef STUB_DEBUG_FILES

static inline void create_debug_files (struct ohci_hcd *bus) { }
static inline void remove_debug_files (struct ohci_hcd *bus) { }

#else

static inline struct ohci_hcd *dev_to_ohci (struct device *dev)
{
	struct usb_hcd	*hcd = dev_get_drvdata (dev);

	return hcd_to_ohci (hcd);
}

static ssize_t
show_list (struct ohci_hcd *ohci, char *buf, size_t count, struct ed *ed)
{
	unsigned		temp, size = count;

	if (!ed)
		return 0;

	/* print first --> last */
	while (ed->ed_prev)
		ed = ed->ed_prev;

	/* dump a snapshot of the bulk or control schedule */
	while (ed) {
		u32			info = ed->hwINFO;
		u32			scratch = cpu_to_le32p (&ed->hwINFO);
		struct list_head	*entry;
		struct td		*td;

		temp = scnprintf (buf, size,
			"ed/%p %cs dev%d ep%d%s max %d %08x%s%s %s",
			ed,
			(info & ED_LOWSPEED) ? 'l' : 'f',
			scratch & 0x7f,
			(scratch >> 7) & 0xf,
			(info & ED_IN) ? "in" : "out",
			0x03ff & (scratch >> 16),
			scratch,
			(info & ED_SKIP) ? " s" : "",
			(ed->hwHeadP & ED_H) ? " H" : "",
			(ed->hwHeadP & ED_C) ? data1 : data0);
		size -= temp;
		buf += temp;

		list_for_each (entry, &ed->td_list) {
			u32		cbp, be;

			td = list_entry (entry, struct td, td_list);
			scratch = cpu_to_le32p (&td->hwINFO);
			cbp = le32_to_cpup (&td->hwCBP);
			be = le32_to_cpup (&td->hwBE);
			temp = scnprintf (buf, size,
					"\n\ttd %p %s %d cc=%x urb %p (%08x)",
					td,
					({ char *pid;
					switch (scratch & TD_DP) {
					case TD_DP_SETUP: pid = "setup"; break;
					case TD_DP_IN: pid = "in"; break;
					case TD_DP_OUT: pid = "out"; break;
					default: pid = "(?)"; break;
					 } pid;}),
					cbp ? (be + 1 - cbp) : 0,
					TD_CC_GET (scratch), td->urb, scratch);
			size -= temp;
			buf += temp;
		}

		temp = scnprintf (buf, size, "\n");
		size -= temp;
		buf += temp;

		ed = ed->ed_next;
	}
	return count - size;
}

static ssize_t
show_async (struct class_device *class_dev, char *buf)
{
	struct usb_bus		*bus;
	struct usb_hcd		*hcd;
	struct ohci_hcd		*ohci;
	size_t			temp;
	unsigned long		flags;

	bus = to_usb_bus(class_dev);
	hcd = bus->hcpriv;
	ohci = hcd_to_ohci(hcd);

	/* display control and bulk lists together, for simplicity */
	spin_lock_irqsave (&ohci->lock, flags);
	temp = show_list (ohci, buf, PAGE_SIZE, ohci->ed_controltail);
	temp += show_list (ohci, buf + temp, PAGE_SIZE - temp, ohci->ed_bulktail);
	spin_unlock_irqrestore (&ohci->lock, flags);

	return temp;
}
static CLASS_DEVICE_ATTR (async, S_IRUGO, show_async, NULL);


#define DBG_SCHED_LIMIT 64

static ssize_t
show_periodic (struct class_device *class_dev, char *buf)
{
	struct usb_bus		*bus;
	struct usb_hcd		*hcd;
	struct ohci_hcd		*ohci;
	struct ed		**seen, *ed;
	unsigned long		flags;
	unsigned		temp, size, seen_count;
	char			*next;
	unsigned		i;

	if (!(seen = kmalloc (DBG_SCHED_LIMIT * sizeof *seen, SLAB_ATOMIC)))
		return 0;
	seen_count = 0;

	bus = to_usb_bus(class_dev);
	hcd = bus->hcpriv;
	ohci = hcd_to_ohci(hcd);
	next = buf;
	size = PAGE_SIZE;

	temp = scnprintf (next, size, "size = %d\n", NUM_INTS);
	size -= temp;
	next += temp;

	/* dump a snapshot of the periodic schedule (and load) */
	spin_lock_irqsave (&ohci->lock, flags);
	for (i = 0; i < NUM_INTS; i++) {
		if (!(ed = ohci->periodic [i]))
			continue;

		temp = scnprintf (next, size, "%2d [%3d]:", i, ohci->load [i]);
		size -= temp;
		next += temp;

		do {
			temp = scnprintf (next, size, " ed%d/%p",
				ed->interval, ed);
			size -= temp;
			next += temp;
			for (temp = 0; temp < seen_count; temp++) {
				if (seen [temp] == ed)
					break;
			}

			/* show more info the first time around */
			if (temp == seen_count) {
				u32	info = ed->hwINFO;
				u32	scratch = cpu_to_le32p (&ed->hwINFO);
				struct list_head	*entry;
				unsigned		qlen = 0;

				/* qlen measured here in TDs, not urbs */
				list_for_each (entry, &ed->td_list)
					qlen++;

				temp = scnprintf (next, size,
					" (%cs dev%d ep%d%s-%s qlen %u"
					" max %d %08x%s%s)",
					(info & ED_LOWSPEED) ? 'l' : 'f',
					scratch & 0x7f,
					(scratch >> 7) & 0xf,
					(info & ED_IN) ? "in" : "out",
					(info & ED_ISO) ? "iso" : "int",
					qlen,
					0x03ff & (scratch >> 16),
					scratch,
					(info & ED_SKIP) ? " K" : "",
					(ed->hwHeadP & ED_H) ? " H" : "");
				size -= temp;
				next += temp;

				if (seen_count < DBG_SCHED_LIMIT)
					seen [seen_count++] = ed;

				ed = ed->ed_next;

			} else {
				/* we've seen it and what's after */
				temp = 0;
				ed = NULL;
			}

		} while (ed);

		temp = scnprintf (next, size, "\n");
		size -= temp;
		next += temp;
	}
	spin_unlock_irqrestore (&ohci->lock, flags);
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
	struct ohci_hcd		*ohci;
	struct ohci_regs	*regs;
	unsigned long		flags;
	unsigned		temp, size;
	char			*next;
	u32			rdata;

	bus = to_usb_bus(class_dev);
	hcd = bus->hcpriv;
	ohci = hcd_to_ohci(hcd);
	regs = ohci->regs;
	next = buf;
	size = PAGE_SIZE;

	spin_lock_irqsave (&ohci->lock, flags);

	/* dump driver info, then registers in spec order */

	ohci_dbg_sw (ohci, &next, &size,
		"bus %s, device %s\n"
		"%s version " DRIVER_VERSION "\n",
		hcd->self.controller->bus->name,
		hcd->self.controller->bus_id,
		hcd_name);

	if (bus->controller->power.power_state) {
		size -= scnprintf (next, size,
			"SUSPENDED (no register access)\n");
		goto done;
	}

	ohci_dump_status(ohci, &next, &size);

	/* hcca */
	if (ohci->hcca)
		ohci_dbg_sw (ohci, &next, &size,
			"hcca frame 0x%04x\n", OHCI_FRAME_NO(ohci->hcca));

	/* other registers mostly affect frame timings */
	rdata = ohci_readl (&regs->fminterval);
	temp = scnprintf (next, size,
			"fmintvl 0x%08x %sFSMPS=0x%04x FI=0x%04x\n",
			rdata, (rdata >> 31) ? " FIT" : "",
			(rdata >> 16) & 0xefff, rdata & 0xffff);
	size -= temp;
	next += temp;

	rdata = ohci_readl (&regs->fmremaining);
	temp = scnprintf (next, size, "fmremaining 0x%08x %sFR=0x%04x\n",
			rdata, (rdata >> 31) ? " FRT" : "",
			rdata & 0x3fff);
	size -= temp;
	next += temp;

	rdata = ohci_readl (&regs->periodicstart);
	temp = scnprintf (next, size, "periodicstart 0x%04x\n",
			rdata & 0x3fff);
	size -= temp;
	next += temp;

	rdata = ohci_readl (&regs->lsthresh);
	temp = scnprintf (next, size, "lsthresh 0x%04x\n",
			rdata & 0x3fff);
	size -= temp;
	next += temp;

	/* roothub */
	ohci_dump_roothub (ohci, 1, &next, &size);

done:
	spin_unlock_irqrestore (&ohci->lock, flags);
	return PAGE_SIZE - size;
}
static CLASS_DEVICE_ATTR (registers, S_IRUGO, show_registers, NULL);


static inline void create_debug_files (struct ohci_hcd *bus)
{
	class_device_create_file(&bus->hcd.self.class_dev, &class_device_attr_async);
	class_device_create_file(&bus->hcd.self.class_dev, &class_device_attr_periodic);
	class_device_create_file(&bus->hcd.self.class_dev, &class_device_attr_registers);
	ohci_dbg (bus, "created debug files\n");
}

static inline void remove_debug_files (struct ohci_hcd *bus)
{
	class_device_remove_file(&bus->hcd.self.class_dev, &class_device_attr_async);
	class_device_remove_file(&bus->hcd.self.class_dev, &class_device_attr_periodic);
	class_device_remove_file(&bus->hcd.self.class_dev, &class_device_attr_registers);
}

#endif

/*-------------------------------------------------------------------------*/

