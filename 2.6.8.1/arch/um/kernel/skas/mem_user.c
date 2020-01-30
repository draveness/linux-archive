/* 
 * Copyright (C) 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include <errno.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include "mem_user.h"
#include "user.h"
#include "os.h"
#include "proc_mm.h"

void map(int fd, unsigned long virt, unsigned long phys, unsigned long len, 
	 int r, int w, int x)
{
	struct proc_mm_op map;
	struct mem_region *region;
	int prot, n;

	prot = (r ? PROT_READ : 0) | (w ? PROT_WRITE : 0) | 
		(x ? PROT_EXEC : 0);
	region = phys_region(phys);

	map = ((struct proc_mm_op) { .op 	= MM_MMAP,
				     .u 	= 
				     { .mmap	= 
				       { .addr 		= virt,
					 .len		= len,
					 .prot		= prot,
					 .flags		= MAP_SHARED | 
					                  MAP_FIXED,
					 .fd		= region->fd,
					 .offset	= phys_offset(phys)
				       } } } );
	n = os_write_file(fd, &map, sizeof(map));
	if(n != sizeof(map)) 
		printk("map : /proc/mm map failed, errno = %d\n", errno);
}

int unmap(int fd, void *addr, int len)
{
	struct proc_mm_op unmap;
	int n;

	unmap = ((struct proc_mm_op) { .op 	= MM_MUNMAP,
				       .u 	= 
				       { .munmap	= 
					 { .addr 	= (unsigned long) addr,
					   .len		= len } } } );
	n = os_write_file(fd, &unmap, sizeof(unmap));
	if((n != 0) && (n != sizeof(unmap)))
		return(-errno);
	return(0);
}

int protect(int fd, unsigned long addr, unsigned long len, int r, int w, 
	    int x, int must_succeed)
{
	struct proc_mm_op protect;
	int prot, n;

	prot = (r ? PROT_READ : 0) | (w ? PROT_WRITE : 0) | 
		(x ? PROT_EXEC : 0);

	protect = ((struct proc_mm_op) { .op 	= MM_MPROTECT,
				       .u 	= 
				       { .mprotect	= 
					 { .addr 	= (unsigned long) addr,
					   .len		= len,
					   .prot	= prot } } } );

	n = os_write_file(fd, &protect, sizeof(protect));
	if((n != 0) && (n != sizeof(protect))){
		if(must_succeed)
			panic("protect failed, errno = %d", errno);
		return(-errno);
	}
	return(0);
}

void before_mem_skas(unsigned long unused)
{
}

/*
 * Overrides for Emacs so that we follow Linus's tabbing style.
 * Emacs will notice this stuff at the end of the file and automatically
 * adjust the settings for this buffer only.  This must remain at the end
 * of the file.
 * ---------------------------------------------------------------------------
 * Local variables:
 * c-file-style: "linux"
 * End:
 */
