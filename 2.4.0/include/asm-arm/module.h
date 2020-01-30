#ifndef _ASM_ARM_MODULE_H
#define _ASM_ARM_MODULE_H
/*
 * This file contains the arm architecture specific module code.
 */

#define module_map(x)		vmalloc(x)
#define module_unmap(x)		vfree(x)
#define module_arch_init(x)	(0)

#endif /* _ASM_ARM_MODULE_H */
