/*
 * lib/smp_processor_id.c
 *
 * DEBUG_PREEMPT variant of smp_processor_id().
 */
#include <linux/module.h>
#include <linux/kallsyms.h>
#include <linux/sched.h>

unsigned int debug_smp_processor_id(void)
{
	unsigned long preempt_count = preempt_count();
	int this_cpu = raw_smp_processor_id();
	cpumask_t this_mask;

	if (likely(preempt_count))
		goto out;

	if (irqs_disabled())
		goto out;

	/*
	 * Kernel threads bound to a single CPU can safely use
	 * smp_processor_id():
	 */
	this_mask = cpumask_of_cpu(this_cpu);

	if (cpus_equal(current->cpus_allowed, this_mask))
		goto out;

	/*
	 * It is valid to assume CPU-locality during early bootup:
	 */
	if (system_state != SYSTEM_RUNNING)
		goto out;

	/*
	 * Avoid recursion:
	 */
	preempt_disable();

	if (!printk_ratelimit())
		goto out_enable;

	printk(KERN_ERR "BUG: using smp_processor_id() in preemptible [%08x] code: %s/%d\n", preempt_count(), current->comm, current->pid);
	print_symbol("caller is %s\n", (long)__builtin_return_address(0));
	dump_stack();

out_enable:
	preempt_enable_no_resched();
out:
	return this_cpu;
}

EXPORT_SYMBOL(debug_smp_processor_id);

