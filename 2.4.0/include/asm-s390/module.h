#ifndef _ASM_S390_MODULE_H
#define _ASM_S390_MODULE_H
/*
 * This file contains the s390 architecture specific module code.
 */

#define module_map(x)		vmalloc(x)
#define module_unmap(x)		vfree(x)
#define module_arch_init(x)	(0)

#endif /* _ASM_S390_MODULE_H */
