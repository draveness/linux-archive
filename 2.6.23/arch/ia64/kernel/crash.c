/*
 * arch/ia64/kernel/crash.c
 *
 * Architecture specific (ia64) functions for kexec based crash dumps.
 *
 * Created by: Khalid Aziz <khalid.aziz@hp.com>
 * Copyright (C) 2005 Hewlett-Packard Development Company, L.P.
 * Copyright (C) 2005 Intel Corp	Zou Nan hai <nanhai.zou@intel.com>
 *
 */
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/crash_dump.h>
#include <linux/bootmem.h>
#include <linux/kexec.h>
#include <linux/elfcore.h>
#include <linux/sysctl.h>
#include <linux/init.h>
#include <linux/kdebug.h>

#include <asm/mca.h>

int kdump_status[NR_CPUS];
static atomic_t kdump_cpu_frozen;
atomic_t kdump_in_progress;
static int kdump_on_init = 1;

static inline Elf64_Word
*append_elf_note(Elf64_Word *buf, char *name, unsigned type, void *data,
		size_t data_len)
{
	struct elf_note *note = (struct elf_note *)buf;
	note->n_namesz = strlen(name) + 1;
	note->n_descsz = data_len;
	note->n_type   = type;
	buf += (sizeof(*note) + 3)/4;
	memcpy(buf, name, note->n_namesz);
	buf += (note->n_namesz + 3)/4;
	memcpy(buf, data, data_len);
	buf += (data_len + 3)/4;
	return buf;
}

static void
final_note(void *buf)
{
	memset(buf, 0, sizeof(struct elf_note));
}

extern void ia64_dump_cpu_regs(void *);

static DEFINE_PER_CPU(struct elf_prstatus, elf_prstatus);

void
crash_save_this_cpu(void)
{
	void *buf;
	unsigned long cfm, sof, sol;

	int cpu = smp_processor_id();
	struct elf_prstatus *prstatus = &per_cpu(elf_prstatus, cpu);

	elf_greg_t *dst = (elf_greg_t *)&(prstatus->pr_reg);
	memset(prstatus, 0, sizeof(*prstatus));
	prstatus->pr_pid = current->pid;

	ia64_dump_cpu_regs(dst);
	cfm = dst[43];
	sol = (cfm >> 7) & 0x7f;
	sof = cfm & 0x7f;
	dst[46] = (unsigned long)ia64_rse_skip_regs((unsigned long *)dst[46],
			sof - sol);

	buf = (u64 *) per_cpu_ptr(crash_notes, cpu);
	if (!buf)
		return;
	buf = append_elf_note(buf, KEXEC_CORE_NOTE_NAME, NT_PRSTATUS, prstatus,
			sizeof(*prstatus));
	final_note(buf);
}

#ifdef CONFIG_SMP
static int
kdump_wait_cpu_freeze(void)
{
	int cpu_num = num_online_cpus() - 1;
	int timeout = 1000;
	while(timeout-- > 0) {
		if (atomic_read(&kdump_cpu_frozen) == cpu_num)
			return 0;
		udelay(1000);
	}
	return 1;
}
#endif

void
machine_crash_shutdown(struct pt_regs *pt)
{
	/* This function is only called after the system
	 * has paniced or is otherwise in a critical state.
	 * The minimum amount of code to allow a kexec'd kernel
	 * to run successfully needs to happen here.
	 *
	 * In practice this means shooting down the other cpus in
	 * an SMP system.
	 */
	kexec_disable_iosapic();
#ifdef CONFIG_SMP
	kdump_smp_send_stop();
	/* not all cpu response to IPI, send INIT to freeze them */
	if (kdump_wait_cpu_freeze() && kdump_on_init) 	{
		kdump_smp_send_init();
	}
#endif
}

static void
machine_kdump_on_init(void)
{
	if (!ia64_kimage) {
		printk(KERN_NOTICE "machine_kdump_on_init(): "
				"kdump not configured\n");
		return;
	}
	local_irq_disable();
	kexec_disable_iosapic();
	machine_kexec(ia64_kimage);
}

void
kdump_cpu_freeze(struct unw_frame_info *info, void *arg)
{
	int cpuid;
	local_irq_disable();
	cpuid = smp_processor_id();
	crash_save_this_cpu();
	current->thread.ksp = (__u64)info->sw - 16;
	atomic_inc(&kdump_cpu_frozen);
	kdump_status[cpuid] = 1;
	mb();
#ifdef CONFIG_HOTPLUG_CPU
	if (cpuid != 0)
		ia64_jump_to_sal(&sal_boot_rendez_state[cpuid]);
#endif
	for (;;)
		cpu_relax();
}

static int
kdump_init_notifier(struct notifier_block *self, unsigned long val, void *data)
{
	struct ia64_mca_notify_die *nd;
	struct die_args *args = data;

	if (!kdump_on_init)
		return NOTIFY_DONE;

	if (val != DIE_INIT_MONARCH_LEAVE &&
	    val != DIE_INIT_SLAVE_LEAVE &&
	    val != DIE_INIT_MONARCH_PROCESS &&
	    val != DIE_MCA_RENDZVOUS_LEAVE &&
	    val != DIE_MCA_MONARCH_LEAVE)
		return NOTIFY_DONE;

	nd = (struct ia64_mca_notify_die *)args->err;
	/* Reason code 1 means machine check rendezvous*/
	if ((val == DIE_INIT_MONARCH_LEAVE || val == DIE_INIT_SLAVE_LEAVE
	    || val == DIE_INIT_MONARCH_PROCESS) && nd->sos->rv_rc == 1)
		return NOTIFY_DONE;

	switch (val) {
		case DIE_INIT_MONARCH_PROCESS:
			atomic_set(&kdump_in_progress, 1);
			*(nd->monarch_cpu) = -1;
			break;
		case DIE_INIT_MONARCH_LEAVE:
			machine_kdump_on_init();
			break;
		case DIE_INIT_SLAVE_LEAVE:
			if (atomic_read(&kdump_in_progress))
				unw_init_running(kdump_cpu_freeze, NULL);
			break;
		case DIE_MCA_RENDZVOUS_LEAVE:
			if (atomic_read(&kdump_in_progress))
				unw_init_running(kdump_cpu_freeze, NULL);
			break;
		case DIE_MCA_MONARCH_LEAVE:
		     /* die_register->signr indicate if MCA is recoverable */
			if (!args->signr)
				machine_kdump_on_init();
			break;
	}
	return NOTIFY_DONE;
}

#ifdef CONFIG_SYSCTL
static ctl_table kdump_on_init_table[] = {
	{
		.ctl_name = CTL_UNNUMBERED,
		.procname = "kdump_on_init",
		.data = &kdump_on_init,
		.maxlen = sizeof(int),
		.mode = 0644,
		.proc_handler = &proc_dointvec,
	},
	{ .ctl_name = 0 }
};

static ctl_table sys_table[] = {
	{
	  .ctl_name = CTL_KERN,
	  .procname = "kernel",
	  .mode = 0555,
	  .child = kdump_on_init_table,
	},
	{ .ctl_name = 0 }
};
#endif

static int
machine_crash_setup(void)
{
	/* be notified before default_monarch_init_process */
	static struct notifier_block kdump_init_notifier_nb = {
		.notifier_call = kdump_init_notifier,
		.priority = 1,
	};
	int ret;
	if((ret = register_die_notifier(&kdump_init_notifier_nb)) != 0)
		return ret;
#ifdef CONFIG_SYSCTL
	register_sysctl_table(sys_table);
#endif
	return 0;
}

__initcall(machine_crash_setup);

