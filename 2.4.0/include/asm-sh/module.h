#ifndef _ASM_SH_MODULE_H
#define _ASM_SH_MODULE_H
/*
 * This file contains the SH architecture specific module code.
 */

#define module_map(x)		vmalloc(x)
#define module_unmap(x)		vfree(x)
#define module_arch_init(x)	(0)

#endif /* _ASM_SH_MODULE_H */
