#ifndef _X86_64_PAGE_H
#define _X86_64_PAGE_H

#include <linux/config.h>

/* PAGE_SHIFT determines the page size */
#define PAGE_SHIFT	12
#ifdef __ASSEMBLY__
#define PAGE_SIZE	(0x1 << PAGE_SHIFT)
#else
#define PAGE_SIZE	(1UL << PAGE_SHIFT)
#endif
#define PAGE_MASK	(~(PAGE_SIZE-1))
#define PHYSICAL_PAGE_MASK	(~(PAGE_SIZE-1) & (__PHYSICAL_MASK << PAGE_SHIFT))

#define THREAD_ORDER 1 
#ifdef __ASSEMBLY__
#define THREAD_SIZE  (1 << (PAGE_SHIFT + THREAD_ORDER))
#else
#define THREAD_SIZE  (1UL << (PAGE_SHIFT + THREAD_ORDER))
#endif
#define CURRENT_MASK (~(THREAD_SIZE-1))

#define LARGE_PAGE_MASK (~(LARGE_PAGE_SIZE-1))
#define LARGE_PAGE_SIZE (1UL << PMD_SHIFT)

#define HPAGE_SHIFT PMD_SHIFT
#define HPAGE_SIZE	((1UL) << HPAGE_SHIFT)
#define HPAGE_MASK	(~(HPAGE_SIZE - 1))
#define HUGETLB_PAGE_ORDER	(HPAGE_SHIFT - PAGE_SHIFT)

#ifdef __KERNEL__
#ifndef __ASSEMBLY__

void clear_page(void *);
void copy_page(void *, void *);

#define clear_user_page(page, vaddr, pg)	clear_page(page)
#define copy_user_page(to, from, vaddr, pg)	copy_page(to, from)

/*
 * These are used to make use of C type-checking..
 */
typedef struct { unsigned long pte; } pte_t;
typedef struct { unsigned long pmd; } pmd_t;
typedef struct { unsigned long pgd; } pgd_t;
typedef struct { unsigned long pml4; } pml4_t;
#define PTE_MASK	PHYSICAL_PAGE_MASK

typedef struct { unsigned long pgprot; } pgprot_t;

#define pte_val(x)	((x).pte)
#define pmd_val(x)	((x).pmd)
#define pgd_val(x)	((x).pgd)
#define pml4_val(x)	((x).pml4)
#define pgprot_val(x)	((x).pgprot)

#define __pte(x) ((pte_t) { (x) } )
#define __pmd(x) ((pmd_t) { (x) } )
#define __pgd(x) ((pgd_t) { (x) } )
#define __pml4(x) ((pml4_t) { (x) } )
#define __pgprot(x)	((pgprot_t) { (x) } )

extern unsigned long vm_stack_flags, vm_stack_flags32;
extern unsigned long vm_data_default_flags, vm_data_default_flags32;
extern unsigned long vm_force_exec32;

#define __START_KERNEL		0xffffffff80100000UL
#define __START_KERNEL_map	0xffffffff80000000UL
#define __PAGE_OFFSET           0x0000010000000000UL	/* 1 << 40 */

#else
#define __START_KERNEL		0xffffffff80100000
#define __START_KERNEL_map	0xffffffff80000000
#define __PAGE_OFFSET           0x0000010000000000	/* 1 << 40 */
#endif /* !__ASSEMBLY__ */

/* to align the pointer to the (next) page boundary */
#define PAGE_ALIGN(addr)	(((addr)+PAGE_SIZE-1)&PAGE_MASK)

/* See Documentation/x86_64/mm.txt for a description of the memory map. */
#define __PHYSICAL_MASK_SHIFT	40
#define __PHYSICAL_MASK		((1UL << __PHYSICAL_MASK_SHIFT) - 1)
#define __VIRTUAL_MASK_SHIFT	48
#define __VIRTUAL_MASK		((1UL << __VIRTUAL_MASK_SHIFT) - 1)

#define KERNEL_TEXT_SIZE  (40UL*1024*1024)
#define KERNEL_TEXT_START 0xffffffff80000000UL 

#ifndef __ASSEMBLY__

#include <asm/bug.h>

/* Pure 2^n version of get_order */
extern __inline__ int get_order(unsigned long size)
{
	int order;

	size = (size-1) >> (PAGE_SHIFT-1);
	order = -1;
	do {
		size >>= 1;
		order++;
	} while (size);
	return order;
}

#endif /* __ASSEMBLY__ */

#define PAGE_OFFSET		((unsigned long)__PAGE_OFFSET)

/* Note: __pa(&symbol_visible_to_c) should be always replaced with __pa_symbol.
   Otherwise you risk miscompilation. */ 
#define __pa(x)			(((unsigned long)(x)>=__START_KERNEL_map)?(unsigned long)(x) - (unsigned long)__START_KERNEL_map:(unsigned long)(x) - PAGE_OFFSET)
/* __pa_symbol should be used for C visible symbols.
   This seems to be the official gcc blessed way to do such arithmetic. */ 
#define __pa_symbol(x)		\
	({unsigned long v;  \
	  asm("" : "=r" (v) : "0" (x)); \
	  __pa(v); })

#define __va(x)			((void *)((unsigned long)(x)+PAGE_OFFSET))
#ifndef CONFIG_DISCONTIGMEM
#define pfn_to_page(pfn)	(mem_map + (pfn))
#define page_to_pfn(page)	((unsigned long)((page) - mem_map))
#define pfn_valid(pfn)		((pfn) < max_mapnr)
#endif

#define virt_to_page(kaddr)	pfn_to_page(__pa(kaddr) >> PAGE_SHIFT)
#define virt_addr_valid(kaddr)	pfn_valid(__pa(kaddr) >> PAGE_SHIFT)
#define pfn_to_kaddr(pfn)      __va((pfn) << PAGE_SHIFT)

#define __VM_DATA_DEFAULT_FLAGS	(VM_READ | VM_WRITE | VM_EXEC | \
				 VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC)
#define __VM_STACK_FLAGS 	(VM_GROWSDOWN | VM_READ | VM_WRITE | VM_EXEC | \
                                VM_MAYREAD | VM_MAYWRITE | VM_MAYEXEC)

#define VM_DATA_DEFAULT_FLAGS \
	(test_thread_flag(TIF_IA32) ? vm_data_default_flags32 : \
	  vm_data_default_flags) 

#define VM_STACK_DEFAULT_FLAGS \
	(test_thread_flag(TIF_IA32) ? vm_stack_flags32 : vm_stack_flags) 
	
#define CONFIG_ARCH_GATE_AREA 1	

#ifndef __ASSEMBLY__
struct task_struct;
struct vm_area_struct *get_gate_vma(struct task_struct *tsk);
int in_gate_area(struct task_struct *task, unsigned long addr);
#endif

#endif /* __KERNEL__ */

#endif /* _X86_64_PAGE_H */
