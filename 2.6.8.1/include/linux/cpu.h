/*
 * include/linux/cpu.h - generic cpu definition
 *
 * This is mainly for topological representation. We define the 
 * basic 'struct cpu' here, which can be embedded in per-arch 
 * definitions of processors.
 *
 * Basic handling of the devices is done in drivers/base/cpu.c
 * and system devices are handled in drivers/base/sys.c. 
 *
 * CPUs are exported via driverfs in the class/cpu/devices/
 * directory. 
 *
 * Per-cpu interfaces can be implemented using a struct device_interface. 
 * See the following for how to do this: 
 * - drivers/base/intf.c 
 * - Documentation/driver-model/interface.txt
 */
#ifndef _LINUX_CPU_H_
#define _LINUX_CPU_H_

#include <linux/sysdev.h>
#include <linux/node.h>
#include <linux/compiler.h>
#include <linux/cpumask.h>
#include <asm/semaphore.h>

struct cpu {
	int node_id;		/* The node which contains the CPU */
	int no_control;		/* Should the sysfs control file be created? */
	struct sys_device sysdev;
};

extern int register_cpu(struct cpu *, int, struct node *);
struct notifier_block;

#ifdef CONFIG_SMP
/* Need to know about CPUs going up/down? */
extern int register_cpu_notifier(struct notifier_block *nb);
extern void unregister_cpu_notifier(struct notifier_block *nb);

int cpu_up(unsigned int cpu);

#else

static inline int register_cpu_notifier(struct notifier_block *nb)
{
	return 0;
}
static inline void unregister_cpu_notifier(struct notifier_block *nb)
{
}

#endif /* CONFIG_SMP */
extern struct sysdev_class cpu_sysdev_class;

#ifdef CONFIG_HOTPLUG_CPU
/* Stop CPUs going up and down. */
extern struct semaphore cpucontrol;
#define lock_cpu_hotplug()	down(&cpucontrol)
#define unlock_cpu_hotplug()	up(&cpucontrol)
#define lock_cpu_hotplug_interruptible() down_interruptible(&cpucontrol)
#define hotcpu_notifier(fn, pri) {				\
	static struct notifier_block fn##_nb =			\
		{ .notifier_call = fn, .priority = pri };	\
	register_cpu_notifier(&fn##_nb);			\
}
int cpu_down(unsigned int cpu);
#define cpu_is_offline(cpu) unlikely(!cpu_online(cpu))
#else
#define lock_cpu_hotplug()	do { } while (0)
#define unlock_cpu_hotplug()	do { } while (0)
#define lock_cpu_hotplug_interruptible() 0
#define hotcpu_notifier(fn, pri)

/* CPUs don't go offline once they're online w/o CONFIG_HOTPLUG_CPU */
#define cpu_is_offline(cpu) 0
#endif

#endif /* _LINUX_CPU_H_ */
