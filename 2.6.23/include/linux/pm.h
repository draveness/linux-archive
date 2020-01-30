/*
 *  pm.h - Power management interface
 *
 *  Copyright (C) 2000 Andrew Henroid
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef _LINUX_PM_H
#define _LINUX_PM_H

#ifdef __KERNEL__

#include <linux/list.h>
#include <asm/atomic.h>
#include <asm/errno.h>

/*
 * Power management requests... these are passed to pm_send_all() and friends.
 *
 * these functions are old and deprecated, see below.
 */
typedef int __bitwise pm_request_t;

#define PM_SUSPEND	((__force pm_request_t) 1)	/* enter D1-D3 */
#define PM_RESUME	((__force pm_request_t) 2)	/* enter D0 */


/*
 * Device types... these are passed to pm_register
 */
typedef int __bitwise pm_dev_t;

#define PM_UNKNOWN_DEV	((__force pm_dev_t) 0)	/* generic */
#define PM_SYS_DEV	((__force pm_dev_t) 1)	/* system device (fan, KB controller, ...) */
#define PM_PCI_DEV	((__force pm_dev_t) 2)	/* PCI device */
#define PM_USB_DEV	((__force pm_dev_t) 3)	/* USB device */
#define PM_SCSI_DEV	((__force pm_dev_t) 4)	/* SCSI device */
#define PM_ISA_DEV	((__force pm_dev_t) 5)	/* ISA device */
#define	PM_MTD_DEV	((__force pm_dev_t) 6)	/* Memory Technology Device */

/*
 * System device hardware ID (PnP) values
 */
enum
{
	PM_SYS_UNKNOWN = 0x00000000, /* generic */
	PM_SYS_KBC =	 0x41d00303, /* keyboard controller */
	PM_SYS_COM =	 0x41d00500, /* serial port */
	PM_SYS_IRDA =	 0x41d00510, /* IRDA controller */
	PM_SYS_FDC =	 0x41d00700, /* floppy controller */
	PM_SYS_VGA =	 0x41d00900, /* VGA controller */
	PM_SYS_PCMCIA =	 0x41d00e00, /* PCMCIA controller */
};

/*
 * Device identifier
 */
#define PM_PCI_ID(dev) ((dev)->bus->number << 16 | (dev)->devfn)

/*
 * Request handler callback
 */
struct pm_dev;

typedef int (*pm_callback)(struct pm_dev *dev, pm_request_t rqst, void *data);

/*
 * Dynamic device information
 */
struct pm_dev
{
	pm_dev_t	 type;
	unsigned long	 id;
	pm_callback	 callback;
	void		*data;

	unsigned long	 flags;
	unsigned long	 state;
	unsigned long	 prev_state;

	struct list_head entry;
};

/* Functions above this comment are list-based old-style power
 * managment. Please avoid using them.  */

/*
 * Callbacks for platform drivers to implement.
 */
extern void (*pm_idle)(void);
extern void (*pm_power_off)(void);
extern void (*pm_power_off_prepare)(void);

typedef int __bitwise suspend_state_t;

#define PM_SUSPEND_ON		((__force suspend_state_t) 0)
#define PM_SUSPEND_STANDBY	((__force suspend_state_t) 1)
#define PM_SUSPEND_MEM		((__force suspend_state_t) 3)
#define PM_SUSPEND_MAX		((__force suspend_state_t) 4)

/**
 * struct pm_ops - Callbacks for managing platform dependent system sleep
 *	states.
 *
 * @valid: Callback to determine if given system sleep state is supported by
 *	the platform.
 *	Valid (ie. supported) states are advertised in /sys/power/state.  Note
 *	that it still may be impossible to enter given system sleep state if the
 *	conditions aren't right.
 *	There is the %pm_valid_only_mem function available that can be assigned
 *	to this if the platform only supports mem sleep.
 *
 * @set_target: Tell the platform which system sleep state is going to be
 *	entered.
 *	@set_target() is executed right prior to suspending devices.  The
 *	information conveyed to the platform code by @set_target() should be
 *	disregarded by the platform as soon as @finish() is executed and if
 *	@prepare() fails.  If @set_target() fails (ie. returns nonzero),
 *	@prepare(), @enter() and @finish() will not be called by the PM core.
 *	This callback is optional.  However, if it is implemented, the argument
 *	passed to @prepare(), @enter() and @finish() is meaningless and should
 *	be ignored.
 *
 * @prepare: Prepare the platform for entering the system sleep state indicated
 *	by @set_target() or represented by the argument if @set_target() is not
 *	implemented.
 *	@prepare() is called right after devices have been suspended (ie. the
 *	appropriate .suspend() method has been executed for each device) and
 *	before the nonboot CPUs are disabled (it is executed with IRQs enabled).
 *	This callback is optional.  It returns 0 on success or a negative
 *	error code otherwise, in which case the system cannot enter the desired
 *	sleep state (@enter() and @finish() will not be called in that case).
 *
 * @enter: Enter the system sleep state indicated by @set_target() or
 *	represented by the argument if @set_target() is not implemented.
 *	This callback is mandatory.  It returns 0 on success or a negative
 *	error code otherwise, in which case the system cannot enter the desired
 *	sleep state.
 *
 * @finish: Called when the system has just left a sleep state, right after
 *	the nonboot CPUs have been enabled and before devices are resumed (it is
 *	executed with IRQs enabled).  If @set_target() is not implemented, the
 *	argument represents the sleep state being left.
 *	This callback is optional, but should be implemented by the platforms
 *	that implement @prepare().  If implemented, it is always called after
 *	@enter() (even if @enter() fails).
 */
struct pm_ops {
	int (*valid)(suspend_state_t state);
	int (*set_target)(suspend_state_t state);
	int (*prepare)(suspend_state_t state);
	int (*enter)(suspend_state_t state);
	int (*finish)(suspend_state_t state);
};

#ifdef CONFIG_SUSPEND
extern struct pm_ops *pm_ops;

/**
 * pm_set_ops - set platform dependent power management ops
 * @pm_ops: The new power management operations to set.
 */
extern void pm_set_ops(struct pm_ops *pm_ops);
extern int pm_valid_only_mem(suspend_state_t state);

/**
 * arch_suspend_disable_irqs - disable IRQs for suspend
 *
 * Disables IRQs (in the default case). This is a weak symbol in the common
 * code and thus allows architectures to override it if more needs to be
 * done. Not called for suspend to disk.
 */
extern void arch_suspend_disable_irqs(void);

/**
 * arch_suspend_enable_irqs - enable IRQs after suspend
 *
 * Enables IRQs (in the default case). This is a weak symbol in the common
 * code and thus allows architectures to override it if more needs to be
 * done. Not called for suspend to disk.
 */
extern void arch_suspend_enable_irqs(void);

extern int pm_suspend(suspend_state_t state);
#else /* !CONFIG_SUSPEND */
#define suspend_valid_only_mem	NULL

static inline void pm_set_ops(struct pm_ops *pm_ops) {}
static inline int pm_suspend(suspend_state_t state) { return -ENOSYS; }
#endif /* !CONFIG_SUSPEND */

/*
 * Device power management
 */

struct device;

typedef struct pm_message {
	int event;
} pm_message_t;

/*
 * Several driver power state transitions are externally visible, affecting
 * the state of pending I/O queues and (for drivers that touch hardware)
 * interrupts, wakeups, DMA, and other hardware state.  There may also be
 * internal transitions to various low power modes, which are transparent
 * to the rest of the driver stack (such as a driver that's ON gating off
 * clocks which are not in active use).
 *
 * One transition is triggered by resume(), after a suspend() call; the
 * message is implicit:
 *
 * ON		Driver starts working again, responding to hardware events
 * 		and software requests.  The hardware may have gone through
 * 		a power-off reset, or it may have maintained state from the
 * 		previous suspend() which the driver will rely on while
 * 		resuming.  On most platforms, there are no restrictions on
 * 		availability of resources like clocks during resume().
 *
 * Other transitions are triggered by messages sent using suspend().  All
 * these transitions quiesce the driver, so that I/O queues are inactive.
 * That commonly entails turning off IRQs and DMA; there may be rules
 * about how to quiesce that are specific to the bus or the device's type.
 * (For example, network drivers mark the link state.)  Other details may
 * differ according to the message:
 *
 * SUSPEND	Quiesce, enter a low power device state appropriate for
 * 		the upcoming system state (such as PCI_D3hot), and enable
 * 		wakeup events as appropriate.
 *
 * FREEZE	Quiesce operations so that a consistent image can be saved;
 * 		but do NOT otherwise enter a low power device state, and do
 * 		NOT emit system wakeup events.
 *
 * PRETHAW	Quiesce as if for FREEZE; additionally, prepare for restoring
 * 		the system from a snapshot taken after an earlier FREEZE.
 * 		Some drivers will need to reset their hardware state instead
 * 		of preserving it, to ensure that it's never mistaken for the
 * 		state which that earlier snapshot had set up.
 *
 * A minimally power-aware driver treats all messages as SUSPEND, fully
 * reinitializes its device during resume() -- whether or not it was reset
 * during the suspend/resume cycle -- and can't issue wakeup events.
 *
 * More power-aware drivers may also use low power states at runtime as
 * well as during system sleep states like PM_SUSPEND_STANDBY.  They may
 * be able to use wakeup events to exit from runtime low-power states,
 * or from system low-power states such as standby or suspend-to-RAM.
 */

#define PM_EVENT_ON 0
#define PM_EVENT_FREEZE 1
#define PM_EVENT_SUSPEND 2
#define PM_EVENT_PRETHAW 3

#define PMSG_FREEZE	((struct pm_message){ .event = PM_EVENT_FREEZE, })
#define PMSG_PRETHAW	((struct pm_message){ .event = PM_EVENT_PRETHAW, })
#define PMSG_SUSPEND	((struct pm_message){ .event = PM_EVENT_SUSPEND, })
#define PMSG_ON		((struct pm_message){ .event = PM_EVENT_ON, })

struct dev_pm_info {
	pm_message_t		power_state;
	unsigned		can_wakeup:1;
#ifdef	CONFIG_PM_SLEEP
	unsigned		should_wakeup:1;
	struct list_head	entry;
#endif
};

extern int device_power_down(pm_message_t state);
extern void device_power_up(void);
extern void device_resume(void);

#ifdef CONFIG_PM_SLEEP
extern int device_suspend(pm_message_t state);
extern int device_prepare_suspend(pm_message_t state);

#define device_set_wakeup_enable(dev,val) \
	((dev)->power.should_wakeup = !!(val))
#define device_may_wakeup(dev) \
	(device_can_wakeup(dev) && (dev)->power.should_wakeup)

extern void __suspend_report_result(const char *function, void *fn, int ret);

#define suspend_report_result(fn, ret)					\
	do {								\
		__suspend_report_result(__FUNCTION__, fn, ret);		\
	} while (0)

/*
 * Platform hook to activate device wakeup capability, if that's not already
 * handled by enable_irq_wake() etc.
 * Returns zero on success, else negative errno
 */
extern int (*platform_enable_wakeup)(struct device *dev, int is_on);

static inline int call_platform_enable_wakeup(struct device *dev, int is_on)
{
	if (platform_enable_wakeup)
		return (*platform_enable_wakeup)(dev, is_on);
	return 0;
}

#else /* !CONFIG_PM_SLEEP */

static inline int device_suspend(pm_message_t state)
{
	return 0;
}

#define device_set_wakeup_enable(dev,val)	do{}while(0)
#define device_may_wakeup(dev)			(0)

#define suspend_report_result(fn, ret) do { } while (0)

static inline int call_platform_enable_wakeup(struct device *dev, int is_on)
{
	return 0;
}

#endif /* !CONFIG_PM_SLEEP */

/* changes to device_may_wakeup take effect on the next pm state change.
 * by default, devices should wakeup if they can.
 */
#define device_can_wakeup(dev) \
	((dev)->power.can_wakeup)
#define device_init_wakeup(dev,val) \
	do { \
		device_can_wakeup(dev) = !!(val); \
		device_set_wakeup_enable(dev,val); \
	} while(0)

#endif /* __KERNEL__ */

#endif /* _LINUX_PM_H */
