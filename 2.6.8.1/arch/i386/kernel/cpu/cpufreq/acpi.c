/*
 * acpi-cpufreq-io.c - ACPI Processor P-States Driver ($Revision: 1.3 $)
 *
 *  Copyright (C) 2001, 2002 Andy Grover <andrew.grover@intel.com>
 *  Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 *  Copyright (C) 2002 - 2004 Dominik Brodowski <linux@brodo.de>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <asm/io.h>
#include <asm/delay.h>
#include <asm/uaccess.h>

#include <linux/acpi.h>
#include <acpi/processor.h>

#define ACPI_PROCESSOR_COMPONENT	0x01000000
#define ACPI_PROCESSOR_CLASS		"processor"
#define ACPI_PROCESSOR_DRIVER_NAME	"ACPI Processor P-States Driver"
#define ACPI_PROCESSOR_DEVICE_NAME	"Processor"

#define _COMPONENT		ACPI_PROCESSOR_COMPONENT
ACPI_MODULE_NAME		("acpi_processor_perf")

MODULE_AUTHOR("Paul Diefenbaugh, Dominik Brodowski");
MODULE_DESCRIPTION(ACPI_PROCESSOR_DRIVER_NAME);
MODULE_LICENSE("GPL");


struct cpufreq_acpi_io {
	struct acpi_processor_performance	acpi_data;
	struct cpufreq_frequency_table		*freq_table;
};

static struct cpufreq_acpi_io	*acpi_io_data[NR_CPUS];


static int
acpi_processor_write_port(
	u16	port,
	u8	bit_width,
	u32	value)
{
	if (bit_width <= 8) {
		outb(value, port);
	} else if (bit_width <= 16) {
		outw(value, port);
	} else if (bit_width <= 32) {
		outl(value, port);
	} else {
		return -ENODEV;
	}
	return 0;
}

static int
acpi_processor_read_port(
	u16	port,
	u8	bit_width,
	u32	*ret)
{
	*ret = 0;
	if (bit_width <= 8) {
		*ret = inb(port);
	} else if (bit_width <= 16) {
		*ret = inw(port);
	} else if (bit_width <= 32) {
		*ret = inl(port);
	} else {
		return -ENODEV;
	}
	return 0;
}

static int
acpi_processor_set_performance (
	struct cpufreq_acpi_io	*data,
	unsigned int		cpu,
	int			state)
{
	u16			port = 0;
	u8			bit_width = 0;
	int			ret = 0;
	u32			value = 0;
	int			i = 0;
	struct cpufreq_freqs    cpufreq_freqs;

	ACPI_FUNCTION_TRACE("acpi_processor_set_performance");

	if (state == data->acpi_data.state) {
		ACPI_DEBUG_PRINT((ACPI_DB_INFO, 
			"Already at target state (P%d)\n", state));
		return_VALUE(0);
	}

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, "Transitioning from P%d to P%d\n",
		data->acpi_data.state, state));

	/* cpufreq frequency struct */
	cpufreq_freqs.cpu = cpu;
	cpufreq_freqs.old = data->freq_table[data->acpi_data.state].frequency;
	cpufreq_freqs.new = data->freq_table[state].frequency;

	/* notify cpufreq */
	cpufreq_notify_transition(&cpufreq_freqs, CPUFREQ_PRECHANGE);

	/*
	 * First we write the target state's 'control' value to the
	 * control_register.
	 */

	port = data->acpi_data.control_register.address;
	bit_width = data->acpi_data.control_register.bit_width;
	value = (u32) data->acpi_data.states[state].control;

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, 
		"Writing 0x%08x to port 0x%04x\n", value, port));

	ret = acpi_processor_write_port(port, bit_width, value);
	if (ret) {
		ACPI_DEBUG_PRINT((ACPI_DB_WARN,
			"Invalid port width 0x%04x\n", bit_width));
		return_VALUE(ret);
	}

	/*
	 * Then we read the 'status_register' and compare the value with the
	 * target state's 'status' to make sure the transition was successful.
	 * Note that we'll poll for up to 1ms (100 cycles of 10us) before
	 * giving up.
	 */

	port = data->acpi_data.status_register.address;
	bit_width = data->acpi_data.status_register.bit_width;

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, 
		"Looking for 0x%08x from port 0x%04x\n",
		(u32) data->acpi_data.states[state].status, port));

	for (i=0; i<100; i++) {
		ret = acpi_processor_read_port(port, bit_width, &value);
		if (ret) {	
			ACPI_DEBUG_PRINT((ACPI_DB_WARN,
				"Invalid port width 0x%04x\n", bit_width));
			return_VALUE(ret);
		}
		if (value == (u32) data->acpi_data.states[state].status)
			break;
		udelay(10);
	}

	/* notify cpufreq */
	cpufreq_notify_transition(&cpufreq_freqs, CPUFREQ_POSTCHANGE);

	if (value != (u32) data->acpi_data.states[state].status) {
		unsigned int tmp = cpufreq_freqs.new;
		cpufreq_freqs.new = cpufreq_freqs.old;
		cpufreq_freqs.old = tmp;
		cpufreq_notify_transition(&cpufreq_freqs, CPUFREQ_PRECHANGE);
		cpufreq_notify_transition(&cpufreq_freqs, CPUFREQ_POSTCHANGE);
		ACPI_DEBUG_PRINT((ACPI_DB_WARN, "Transition failed\n"));
		return_VALUE(-ENODEV);
	}

	ACPI_DEBUG_PRINT((ACPI_DB_INFO, 
		"Transition successful after %d microseconds\n",
		i * 10));

	data->acpi_data.state = state;

	return_VALUE(0);
}


static int
acpi_cpufreq_target (
	struct cpufreq_policy   *policy,
	unsigned int target_freq,
	unsigned int relation)
{
	struct cpufreq_acpi_io *data = acpi_io_data[policy->cpu];
	unsigned int next_state = 0;
	unsigned int result = 0;

	ACPI_FUNCTION_TRACE("acpi_cpufreq_setpolicy");

	result = cpufreq_frequency_table_target(policy,
			data->freq_table,
			target_freq,
			relation,
			&next_state);
	if (result)
		return_VALUE(result);

	result = acpi_processor_set_performance (data, policy->cpu, next_state);

	return_VALUE(result);
}


static int
acpi_cpufreq_verify (
	struct cpufreq_policy   *policy)
{
	unsigned int result = 0;
	struct cpufreq_acpi_io *data = acpi_io_data[policy->cpu];

	ACPI_FUNCTION_TRACE("acpi_cpufreq_verify");

	result = cpufreq_frequency_table_verify(policy, 
			data->freq_table);

	return_VALUE(result);
}


static unsigned long
acpi_cpufreq_guess_freq (
	struct cpufreq_acpi_io	*data,
	unsigned int		cpu)
{
	if (cpu_khz) {
		/* search the closest match to cpu_khz */
		unsigned int i;
		unsigned long freq;
		unsigned long freqn = data->acpi_data.states[0].core_frequency * 1000;

		for (i=0; i < (data->acpi_data.state_count - 1); i++) {
			freq = freqn;
			freqn = data->acpi_data.states[i+1].core_frequency * 1000;
			if ((2 * cpu_khz) > (freqn + freq)) {
				data->acpi_data.state = i;
				return (freq);
			}
		}
		data->acpi_data.state = data->acpi_data.state_count - 1;
		return (freqn);
	} else
		/* assume CPU is at P0... */
		data->acpi_data.state = 0;
		return data->acpi_data.states[0].core_frequency * 1000;
	
}

static int
acpi_cpufreq_cpu_init (
	struct cpufreq_policy   *policy)
{
	unsigned int		i;
	unsigned int		cpu = policy->cpu;
	struct cpufreq_acpi_io	*data;
	unsigned int		result = 0;

	ACPI_FUNCTION_TRACE("acpi_cpufreq_cpu_init");

	data = kmalloc(sizeof(struct cpufreq_acpi_io), GFP_KERNEL);
	if (!data)
		return_VALUE(-ENOMEM);
	memset(data, 0, sizeof(struct cpufreq_acpi_io));

	acpi_io_data[cpu] = data;

	result = acpi_processor_register_performance(&data->acpi_data, cpu);
	if (result)
		goto err_free;

	/* capability check */
	if (data->acpi_data.state_count <= 1) {
		ACPI_DEBUG_PRINT((ACPI_DB_ERROR, "No P-States\n"));
		result = -ENODEV;
		goto err_unreg;
	}
	if ((data->acpi_data.control_register.space_id != ACPI_ADR_SPACE_SYSTEM_IO) ||
	    (data->acpi_data.status_register.space_id != ACPI_ADR_SPACE_SYSTEM_IO)) {
		ACPI_DEBUG_PRINT((ACPI_DB_ERROR, "Unsupported address space [%d, %d]\n",
				  (u32) (data->acpi_data.control_register.space_id),
				  (u32) (data->acpi_data.status_register.space_id)));
		result = -ENODEV;
		goto err_unreg;
	}

	/* alloc freq_table */
	data->freq_table = kmalloc(sizeof(struct cpufreq_frequency_table) * (data->acpi_data.state_count + 1), GFP_KERNEL);
	if (!data->freq_table) {
		result = -ENOMEM;
		goto err_unreg;
	}

	/* detect transition latency */
	policy->cpuinfo.transition_latency = 0;
	for (i=0; i<data->acpi_data.state_count; i++) {
		if ((data->acpi_data.states[i].transition_latency * 1000) > policy->cpuinfo.transition_latency)
			policy->cpuinfo.transition_latency = data->acpi_data.states[i].transition_latency * 1000;
	}
	policy->governor = CPUFREQ_DEFAULT_GOVERNOR;

	/* The current speed is unknown and not detectable by ACPI...  */
	policy->cur = acpi_cpufreq_guess_freq(data, policy->cpu);

	/* table init */
	for (i=0; i<=data->acpi_data.state_count; i++)
	{
		data->freq_table[i].index = i;
		if (i<data->acpi_data.state_count)
			data->freq_table[i].frequency = data->acpi_data.states[i].core_frequency * 1000;
		else
			data->freq_table[i].frequency = CPUFREQ_TABLE_END;
	}

	result = cpufreq_frequency_table_cpuinfo(policy, data->freq_table);
	if (result) {
		goto err_freqfree;
	}
		

	printk(KERN_INFO "cpufreq: CPU%u - ACPI performance management activated.\n",
	       cpu);
	for (i = 0; i < data->acpi_data.state_count; i++)
		printk(KERN_INFO "cpufreq: %cP%d: %d MHz, %d mW, %d uS\n",
			(i == data->acpi_data.state?'*':' '), i,
			(u32) data->acpi_data.states[i].core_frequency,
			(u32) data->acpi_data.states[i].power,
			(u32) data->acpi_data.states[i].transition_latency);

	cpufreq_frequency_table_get_attr(data->freq_table, policy->cpu);
	return_VALUE(result);

 err_freqfree:
	kfree(data->freq_table);
 err_unreg:
	acpi_processor_unregister_performance(&data->acpi_data, cpu);
 err_free:
	kfree(data);
	acpi_io_data[cpu] = NULL;

	return_VALUE(result);
}


static int
acpi_cpufreq_cpu_exit (
	struct cpufreq_policy   *policy)
{
	struct cpufreq_acpi_io *data = acpi_io_data[policy->cpu];


	ACPI_FUNCTION_TRACE("acpi_cpufreq_cpu_exit");

	if (data) {
		cpufreq_frequency_table_put_attr(policy->cpu);
		acpi_io_data[policy->cpu] = NULL;
		acpi_processor_unregister_performance(&data->acpi_data, policy->cpu);
		kfree(data);
	}

	return_VALUE(0);
}


static struct freq_attr* acpi_cpufreq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

static struct cpufreq_driver acpi_cpufreq_driver = {
	.verify 	= acpi_cpufreq_verify,
	.target 	= acpi_cpufreq_target,
	.init		= acpi_cpufreq_cpu_init,
	.exit		= acpi_cpufreq_cpu_exit,
	.name		= "acpi-cpufreq",
	.owner		= THIS_MODULE,
	.attr           = acpi_cpufreq_attr,
};


static int __init
acpi_cpufreq_init (void)
{
	int                     result = 0;

	ACPI_FUNCTION_TRACE("acpi_cpufreq_init");

 	result = cpufreq_register_driver(&acpi_cpufreq_driver);
	
	return_VALUE(result);
}


static void __exit
acpi_cpufreq_exit (void)
{
	ACPI_FUNCTION_TRACE("acpi_cpufreq_exit");

	cpufreq_unregister_driver(&acpi_cpufreq_driver);

	return_VOID;
}


late_initcall(acpi_cpufreq_init);
module_exit(acpi_cpufreq_exit);
