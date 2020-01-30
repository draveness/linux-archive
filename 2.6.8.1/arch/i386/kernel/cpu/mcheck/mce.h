#include <linux/init.h>

void amd_mcheck_init(struct cpuinfo_x86 *c);
void intel_p4_mcheck_init(struct cpuinfo_x86 *c);
void intel_p5_mcheck_init(struct cpuinfo_x86 *c);
void intel_p6_mcheck_init(struct cpuinfo_x86 *c);
void winchip_mcheck_init(struct cpuinfo_x86 *c);

/* Call the installed machine check handler for this CPU setup. */
extern asmlinkage void (*machine_check_vector)(struct pt_regs *, long error_code);

extern int mce_disabled __initdata;
extern int nr_mce_banks;

