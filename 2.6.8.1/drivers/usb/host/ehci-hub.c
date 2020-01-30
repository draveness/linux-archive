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

/*-------------------------------------------------------------------------*/

/*
 * EHCI Root Hub ... the nonsharable stuff
 *
 * Registers don't need cpu_to_le32, that happens transparently
 */

/*-------------------------------------------------------------------------*/

#ifdef	CONFIG_PM

static int ehci_hub_suspend (struct usb_hcd *hcd)
{
	struct ehci_hcd		*ehci = hcd_to_ehci (hcd);
	struct usb_device	*root = hcd_to_bus (&ehci->hcd)->root_hub;
	int			port;
	int			status = 0;

	if (root->dev.power.power_state != 0)
		return 0;
	if (time_before (jiffies, ehci->next_statechange))
		return -EAGAIN;

	port = HCS_N_PORTS (ehci->hcs_params);
	spin_lock_irq (&ehci->lock);

	/* suspend any active/unsuspended ports, maybe allow wakeup */
	while (port--) {
		u32	t1 = readl (&ehci->regs->port_status [port]);
		u32	t2 = t1;

		if ((t1 & PORT_PE) && !(t1 & PORT_OWNER))
			t2 |= PORT_SUSPEND;
		if (ehci->hcd.remote_wakeup)
			t2 |= PORT_WKOC_E|PORT_WKDISC_E|PORT_WKCONN_E;
		else
			t2 &= ~(PORT_WKOC_E|PORT_WKDISC_E|PORT_WKCONN_E);

		if (t1 != t2) {
			ehci_vdbg (ehci, "port %d, %08x -> %08x\n",
				port + 1, t1, t2);
			writel (t2, &ehci->regs->port_status [port]);
		}
	}

	/* stop schedules, then turn off HC and clean any completed work */
	if (hcd->state == USB_STATE_RUNNING)
		ehci_ready (ehci);
	ehci->command = readl (&ehci->regs->command);
	writel (ehci->command & ~CMD_RUN, &ehci->regs->command);
	if (ehci->reclaim)
		ehci->reclaim_ready = 1;
	ehci_work(ehci, NULL);
	(void) handshake (&ehci->regs->status, STS_HALT, STS_HALT, 2000);

	root->dev.power.power_state = 3;
	ehci->next_statechange = jiffies + msecs_to_jiffies(10);
	spin_unlock_irq (&ehci->lock);
	return status;
}


/* caller owns root->serialize, and should reset/reinit on error */
static int ehci_hub_resume (struct usb_hcd *hcd)
{
	struct ehci_hcd		*ehci = hcd_to_ehci (hcd);
	struct usb_device	*root = hcd_to_bus (&ehci->hcd)->root_hub;
	u32			temp;
	int			i;

	if (!root->dev.power.power_state)
		return 0;
	if (time_before (jiffies, ehci->next_statechange))
		return -EAGAIN;

	/* re-init operational registers in case we lost power */
	if (readl (&ehci->regs->intr_enable) == 0) {
		writel (INTR_MASK, &ehci->regs->intr_enable);
		writel (0, &ehci->regs->segment);
		writel (ehci->periodic_dma, &ehci->regs->frame_list);
		writel ((u32)ehci->async->qh_dma, &ehci->regs->async_next);
		/* FIXME will this work even (pci) vAUX was lost? */
	}

	/* restore CMD_RUN, framelist size, and irq threshold */
	writel (ehci->command, &ehci->regs->command);

	/* take ports out of suspend */
	i = HCS_N_PORTS (ehci->hcs_params);
	while (i--) {
		temp = readl (&ehci->regs->port_status [i]);
		temp &= ~(PORT_WKOC_E|PORT_WKDISC_E|PORT_WKCONN_E);
		if (temp & PORT_SUSPEND) {
			ehci->reset_done [i] = jiffies + msecs_to_jiffies (20);
			temp |= PORT_RESUME;
		}
		writel (temp, &ehci->regs->port_status [i]);
	}
	i = HCS_N_PORTS (ehci->hcs_params);
	msleep (20);
	while (i--) {
		temp = readl (&ehci->regs->port_status [i]);
		if ((temp & PORT_SUSPEND) == 0)
			continue;
		temp &= ~PORT_RESUME;
		writel (temp, &ehci->regs->port_status [i]);
		ehci_vdbg (ehci, "resumed port %d\n", i + 1);
	}
	(void) readl (&ehci->regs->command);

	/* maybe re-activate the schedule(s) */
	temp = 0;
	if (ehci->async->qh_next.qh)
		temp |= CMD_ASE;
	if (ehci->periodic_sched)
		temp |= CMD_PSE;
	if (temp)
		writel (ehci->command | temp, &ehci->regs->command);

	root->dev.power.power_state = 0;
	ehci->next_statechange = jiffies + msecs_to_jiffies(5);
	ehci->hcd.state = USB_STATE_RUNNING;
	return 0;
}

#else

#define ehci_hub_suspend	0
#define ehci_hub_resume		0

#endif	/* CONFIG_PM */

/*-------------------------------------------------------------------------*/

static int check_reset_complete (
	struct ehci_hcd	*ehci,
	int		index,
	int		port_status
) {
	if (!(port_status & PORT_CONNECT)) {
		ehci->reset_done [index] = 0;
		return port_status;
	}

	/* if reset finished and it's still not enabled -- handoff */
	if (!(port_status & PORT_PE)) {

		/* with integrated TT, there's nobody to hand it to! */
		if (ehci_is_ARC(ehci)) {
			ehci_dbg (ehci,
				"Failed to enable port %d on root hub TT\n",
				index+1);
			return port_status;
		}

		ehci_dbg (ehci, "port %d full speed --> companion\n",
			index + 1);

		// what happens if HCS_N_CC(params) == 0 ?
		port_status |= PORT_OWNER;
		writel (port_status, &ehci->regs->port_status [index]);

	} else
		ehci_dbg (ehci, "port %d high speed\n", index + 1);

	return port_status;
}

/*-------------------------------------------------------------------------*/


/* build "status change" packet (one or two bytes) from HC registers */

static int
ehci_hub_status_data (struct usb_hcd *hcd, char *buf)
{
	struct ehci_hcd	*ehci = hcd_to_ehci (hcd);
	u32		temp, status = 0;
	int		ports, i, retval = 1;
	unsigned long	flags;

	/* init status to no-changes */
	buf [0] = 0;
	ports = HCS_N_PORTS (ehci->hcs_params);
	if (ports > 7) {
		buf [1] = 0;
		retval++;
	}
	
	/* no hub change reports (bit 0) for now (power, ...) */

	/* port N changes (bit N)? */
	spin_lock_irqsave (&ehci->lock, flags);
	for (i = 0; i < ports; i++) {
		temp = readl (&ehci->regs->port_status [i]);
		if (temp & PORT_OWNER) {
			/* don't report this in GetPortStatus */
			if (temp & PORT_CSC) {
				temp &= ~PORT_CSC;
				writel (temp, &ehci->regs->port_status [i]);
			}
			continue;
		}
		if (!(temp & PORT_CONNECT))
			ehci->reset_done [i] = 0;
		if ((temp & (PORT_CSC | PORT_PEC | PORT_OCC)) != 0
				// PORT_STAT_C_SUSPEND?
				|| ((temp & PORT_RESUME) != 0
					&& time_after (jiffies,
						ehci->reset_done [i]))) {
			if (i < 7)
			    buf [0] |= 1 << (i + 1);
			else
			    buf [1] |= 1 << (i - 7);
			status = STS_PCD;
		}
	}
	spin_unlock_irqrestore (&ehci->lock, flags);
	return status ? retval : 0;
}

/*-------------------------------------------------------------------------*/

static void
ehci_hub_descriptor (
	struct ehci_hcd			*ehci,
	struct usb_hub_descriptor	*desc
) {
	int		ports = HCS_N_PORTS (ehci->hcs_params);
	u16		temp;

	desc->bDescriptorType = 0x29;
	desc->bPwrOn2PwrGood = 10;	/* ehci 1.0, 2.3.9 says 20ms max */
	desc->bHubContrCurrent = 0;

	desc->bNbrPorts = ports;
	temp = 1 + (ports / 8);
	desc->bDescLength = 7 + 2 * temp;

	/* two bitmaps:  ports removable, and usb 1.0 legacy PortPwrCtrlMask */
	memset (&desc->bitmap [0], 0, temp);
	memset (&desc->bitmap [temp], 0xff, temp);

	temp = 0x0008;			/* per-port overcurrent reporting */
	if (HCS_PPC (ehci->hcs_params))
		temp |= 0x0001;		/* per-port power control */
	if (HCS_INDICATOR (ehci->hcs_params))
		temp |= 0x0080;		/* per-port indicators (LEDs) */
	desc->wHubCharacteristics = cpu_to_le16 (temp);
}

/*-------------------------------------------------------------------------*/

#define	PORT_WAKE_BITS 	(PORT_WKOC_E|PORT_WKDISC_E|PORT_WKCONN_E)

static int ehci_hub_control (
	struct usb_hcd	*hcd,
	u16		typeReq,
	u16		wValue,
	u16		wIndex,
	char		*buf,
	u16		wLength
) {
	struct ehci_hcd	*ehci = hcd_to_ehci (hcd);
	int		ports = HCS_N_PORTS (ehci->hcs_params);
	u32		temp, status;
	unsigned long	flags;
	int		retval = 0;

	/*
	 * FIXME:  support SetPortFeatures USB_PORT_FEAT_INDICATOR.
	 * HCS_INDICATOR may say we can change LEDs to off/amber/green.
	 * (track current state ourselves) ... blink for diagnostics,
	 * power, "this is the one", etc.  EHCI spec supports this.
	 */

	spin_lock_irqsave (&ehci->lock, flags);
	switch (typeReq) {
	case ClearHubFeature:
		switch (wValue) {
		case C_HUB_LOCAL_POWER:
		case C_HUB_OVER_CURRENT:
			/* no hub-wide feature/status flags */
			break;
		default:
			goto error;
		}
		break;
	case ClearPortFeature:
		if (!wIndex || wIndex > ports)
			goto error;
		wIndex--;
		temp = readl (&ehci->regs->port_status [wIndex]);
		if (temp & PORT_OWNER)
			break;

		switch (wValue) {
		case USB_PORT_FEAT_ENABLE:
			writel (temp & ~PORT_PE,
				&ehci->regs->port_status [wIndex]);
			break;
		case USB_PORT_FEAT_C_ENABLE:
			writel (temp | PORT_PEC,
				&ehci->regs->port_status [wIndex]);
			break;
		case USB_PORT_FEAT_SUSPEND:
			if (temp & PORT_RESET)
				goto error;
			if (temp & PORT_SUSPEND) {
				if ((temp & PORT_PE) == 0)
					goto error;
				/* resume signaling for 20 msec */
				writel ((temp & ~PORT_WAKE_BITS) | PORT_RESUME,
					&ehci->regs->port_status [wIndex]);
				ehci->reset_done [wIndex] = jiffies
						+ msecs_to_jiffies (20);
			}
			break;
		case USB_PORT_FEAT_C_SUSPEND:
			/* we auto-clear this feature */
			break;
		case USB_PORT_FEAT_POWER:
			if (HCS_PPC (ehci->hcs_params))
				writel (temp & ~PORT_POWER,
					&ehci->regs->port_status [wIndex]);
			break;
		case USB_PORT_FEAT_C_CONNECTION:
			writel (temp | PORT_CSC,
				&ehci->regs->port_status [wIndex]);
			break;
		case USB_PORT_FEAT_C_OVER_CURRENT:
			writel (temp | PORT_OCC,
				&ehci->regs->port_status [wIndex]);
			break;
		case USB_PORT_FEAT_C_RESET:
			/* GetPortStatus clears reset */
			break;
		default:
			goto error;
		}
		readl (&ehci->regs->command);	/* unblock posted write */
		break;
	case GetHubDescriptor:
		ehci_hub_descriptor (ehci, (struct usb_hub_descriptor *)
			buf);
		break;
	case GetHubStatus:
		/* no hub-wide feature/status flags */
		memset (buf, 0, 4);
		//cpu_to_le32s ((u32 *) buf);
		break;
	case GetPortStatus:
		if (!wIndex || wIndex > ports)
			goto error;
		wIndex--;
		status = 0;
		temp = readl (&ehci->regs->port_status [wIndex]);

		// wPortChange bits
		if (temp & PORT_CSC)
			status |= 1 << USB_PORT_FEAT_C_CONNECTION;
		if (temp & PORT_PEC)
			status |= 1 << USB_PORT_FEAT_C_ENABLE;
		if (temp & PORT_OCC)
			status |= 1 << USB_PORT_FEAT_C_OVER_CURRENT;

		/* whoever resumes must GetPortStatus to complete it!! */
		if ((temp & PORT_RESUME)
				&& time_after (jiffies,
					ehci->reset_done [wIndex])) {
			status |= 1 << USB_PORT_FEAT_C_SUSPEND;
			ehci->reset_done [wIndex] = 0;

			/* stop resume signaling */
			temp = readl (&ehci->regs->port_status [wIndex]);
			writel (temp & ~PORT_RESUME,
				&ehci->regs->port_status [wIndex]);
			retval = handshake (
					&ehci->regs->port_status [wIndex],
					PORT_RESUME, 0, 2000 /* 2msec */);
			if (retval != 0) {
				ehci_err (ehci, "port %d resume error %d\n",
					wIndex + 1, retval);
				goto error;
			}
			temp &= ~(PORT_SUSPEND|PORT_RESUME|(3<<10));
		}

		/* whoever resets must GetPortStatus to complete it!! */
		if ((temp & PORT_RESET)
				&& time_after (jiffies,
					ehci->reset_done [wIndex])) {
			status |= 1 << USB_PORT_FEAT_C_RESET;
			ehci->reset_done [wIndex] = 0;

			/* force reset to complete */
			writel (temp & ~PORT_RESET,
					&ehci->regs->port_status [wIndex]);
			retval = handshake (
					&ehci->regs->port_status [wIndex],
					PORT_RESET, 0, 500);
			if (retval != 0) {
				ehci_err (ehci, "port %d reset error %d\n",
					wIndex + 1, retval);
				goto error;
			}

			/* see what we found out */
			temp = check_reset_complete (ehci, wIndex,
				readl (&ehci->regs->port_status [wIndex]));
		}

		// don't show wPortStatus if it's owned by a companion hc
		if (!(temp & PORT_OWNER)) {
			if (temp & PORT_CONNECT) {
				status |= 1 << USB_PORT_FEAT_CONNECTION;
				// status may be from integrated TT
				status |= ehci_port_speed(ehci, temp);
			}
			if (temp & PORT_PE)
				status |= 1 << USB_PORT_FEAT_ENABLE;
			if (temp & (PORT_SUSPEND|PORT_RESUME))
				status |= 1 << USB_PORT_FEAT_SUSPEND;
			if (temp & PORT_OC)
				status |= 1 << USB_PORT_FEAT_OVER_CURRENT;
			if (temp & PORT_RESET)
				status |= 1 << USB_PORT_FEAT_RESET;
			if (temp & PORT_POWER)
				status |= 1 << USB_PORT_FEAT_POWER;
		}

#ifndef	EHCI_VERBOSE_DEBUG
	if (status & ~0xffff)	/* only if wPortChange is interesting */
#endif
		dbg_port (ehci, "GetStatus", wIndex + 1, temp);
		// we "know" this alignment is good, caller used kmalloc()...
		*((u32 *) buf) = cpu_to_le32 (status);
		break;
	case SetHubFeature:
		switch (wValue) {
		case C_HUB_LOCAL_POWER:
		case C_HUB_OVER_CURRENT:
			/* no hub-wide feature/status flags */
			break;
		default:
			goto error;
		}
		break;
	case SetPortFeature:
		if (!wIndex || wIndex > ports)
			goto error;
		wIndex--;
		temp = readl (&ehci->regs->port_status [wIndex]);
		if (temp & PORT_OWNER)
			break;

		switch (wValue) {
		case USB_PORT_FEAT_SUSPEND:
			if ((temp & PORT_PE) == 0
					|| (temp & PORT_RESET) != 0)
				goto error;
			if (ehci->hcd.remote_wakeup)
				temp |= PORT_WAKE_BITS;
			writel (temp | PORT_SUSPEND,
				&ehci->regs->port_status [wIndex]);
			break;
		case USB_PORT_FEAT_POWER:
			if (HCS_PPC (ehci->hcs_params))
				writel (temp | PORT_POWER,
					&ehci->regs->port_status [wIndex]);
			break;
		case USB_PORT_FEAT_RESET:
			if (temp & PORT_RESUME)
				goto error;
			/* line status bits may report this as low speed,
			 * which can be fine if this root hub has a
			 * transaction translator built in.
			 */
			if ((temp & (PORT_PE|PORT_CONNECT)) == PORT_CONNECT
					&& !ehci_is_ARC(ehci)
					&& PORT_USB11 (temp)) {
				ehci_dbg (ehci,
					"port %d low speed --> companion\n",
					wIndex + 1);
				temp |= PORT_OWNER;
			} else {
				ehci_vdbg (ehci, "port %d reset\n", wIndex + 1);
				temp |= PORT_RESET;
				temp &= ~PORT_PE;

				/*
				 * caller must wait, then call GetPortStatus
				 * usb 2.0 spec says 50 ms resets on root
				 */
				ehci->reset_done [wIndex] = jiffies
						+ msecs_to_jiffies (50);
			}
			writel (temp, &ehci->regs->port_status [wIndex]);
			break;
		default:
			goto error;
		}
		readl (&ehci->regs->command);	/* unblock posted writes */
		break;

	default:
error:
		/* "stall" on error */
		retval = -EPIPE;
	}
	spin_unlock_irqrestore (&ehci->lock, flags);
	return retval;
}
