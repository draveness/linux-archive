#ifndef __ASM_SH_BUGS_H
#define __ASM_SH_BUGS_H

/*
 * This is included by init/main.c to check for architecture-dependent bugs.
 *
 * Needs:
 *	void check_bugs(void);
 */

/*
 * I don't know of any Super-H bugs yet.
 */

#include <asm/processor.h>

static void __init check_bugs(void)
{
	extern char *get_cpu_subtype(void);
	extern unsigned long loops_per_jiffy;
	char *p= &system_utsname.machine[2]; /* "sh" */

	cpu_data->loops_per_jiffy = loops_per_jiffy;
	
	switch (cpu_data->type) {
	case CPU_SH7604:
		*p++ = '2';
		break;
	case CPU_SH7705 ... CPU_SH7300:
		*p++ = '3';
		break;
	case CPU_SH7750 ... CPU_ST40GX1:
		*p++ = '4';
		break;
	default:
		*p++ = '?';
		*p++ = '!';
		break;
	}

	printk("CPU: %s\n", get_cpu_subtype());

#ifndef __LITTLE_ENDIAN__
	/* 'eb' means 'Endian Big' */
	*p++ = 'e';
	*p++ = 'b';
#endif
	*p = '\0';
}
#endif /* __ASM_SH_BUGS_H */
