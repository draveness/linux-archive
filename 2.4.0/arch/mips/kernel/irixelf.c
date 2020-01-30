/*
 * irixelf.c: Code to load IRIX ELF executables which conform to
 *            the MIPS ABI.
 *
 * Copyright (C) 1996 David S. Miller (dm@engr.sgi.com)
 *
 * Based upon work which is:
 * Copyright 1993, 1994: Eric Youngdale (ericy@cais.com).
 */

#include <linux/module.h>

#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/a.out.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/signal.h>
#include <linux/binfmts.h>
#include <linux/string.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/malloc.h>
#include <linux/shm.h>
#include <linux/personality.h>
#include <linux/elfcore.h>
#include <linux/smp_lock.h>

#include <asm/uaccess.h>
#include <asm/pgalloc.h>
#include <asm/mipsregs.h>
#include <asm/prctl.h>

#define DLINFO_ITEMS 12

#include <linux/elf.h>

#undef DEBUG_ELF

static int load_irix_binary(struct linux_binprm * bprm, struct pt_regs * regs);
static int load_irix_library(struct file *);
static int irix_core_dump(long signr, struct pt_regs * regs,
                          struct file *file);
extern int dump_fpu (elf_fpregset_t *);

static struct linux_binfmt irix_format = {
	NULL, THIS_MODULE, load_irix_binary, load_irix_library,
	irix_core_dump, PAGE_SIZE
};

#ifndef elf_addr_t
#define elf_addr_t unsigned long
#define elf_caddr_t char *
#endif

#ifdef DEBUG_ELF
/* Debugging routines. */
static char *get_elf_p_type(Elf32_Word p_type)
{
	int i = (int) p_type;

	switch(i) {
	case PT_NULL: return("PT_NULL"); break;
	case PT_LOAD: return("PT_LOAD"); break;
	case PT_DYNAMIC: return("PT_DYNAMIC"); break;
	case PT_INTERP: return("PT_INTERP"); break;
	case PT_NOTE: return("PT_NOTE"); break;
	case PT_SHLIB: return("PT_SHLIB"); break;
	case PT_PHDR: return("PT_PHDR"); break;
	case PT_LOPROC: return("PT_LOPROC/REGINFO"); break;
	case PT_HIPROC: return("PT_HIPROC"); break;
	default: return("PT_BOGUS"); break;
	}
}

static void print_elfhdr(struct elfhdr *ehp)
{
	int i;

	printk("ELFHDR: e_ident<");
	for(i = 0; i < (EI_NIDENT - 1); i++) printk("%x ", ehp->e_ident[i]);
	printk("%x>\n", ehp->e_ident[i]);
	printk("        e_type[%04x] e_machine[%04x] e_version[%08lx]\n",
	       (unsigned short) ehp->e_type, (unsigned short) ehp->e_machine,
	       (unsigned long) ehp->e_version);
	printk("        e_entry[%08lx] e_phoff[%08lx] e_shoff[%08lx] "
	       "e_flags[%08lx]\n",
	       (unsigned long) ehp->e_entry, (unsigned long) ehp->e_phoff,
	       (unsigned long) ehp->e_shoff, (unsigned long) ehp->e_flags);
	printk("        e_ehsize[%04x] e_phentsize[%04x] e_phnum[%04x]\n",
	       (unsigned short) ehp->e_ehsize, (unsigned short) ehp->e_phentsize,
	       (unsigned short) ehp->e_phnum);
	printk("        e_shentsize[%04x] e_shnum[%04x] e_shstrndx[%04x]\n",
	       (unsigned short) ehp->e_shentsize, (unsigned short) ehp->e_shnum,
	       (unsigned short) ehp->e_shstrndx);
}

static void print_phdr(int i, struct elf_phdr *ep)
{
	printk("PHDR[%d]: p_type[%s] p_offset[%08lx] p_vaddr[%08lx] "
	       "p_paddr[%08lx]\n", i, get_elf_p_type(ep->p_type),
	       (unsigned long) ep->p_offset, (unsigned long) ep->p_vaddr,
	       (unsigned long) ep->p_paddr);
	printk("         p_filesz[%08lx] p_memsz[%08lx] p_flags[%08lx] "
	       "p_align[%08lx]\n", (unsigned long) ep->p_filesz,
	       (unsigned long) ep->p_memsz, (unsigned long) ep->p_flags,
	       (unsigned long) ep->p_align);
}

static void dump_phdrs(struct elf_phdr *ep, int pnum)
{
	int i;

	for(i = 0; i < pnum; i++, ep++) {
		if((ep->p_type == PT_LOAD) ||
		   (ep->p_type == PT_INTERP) ||
		   (ep->p_type == PT_PHDR))
			print_phdr(i, ep);
	}
}
#endif /* (DEBUG_ELF) */

static void set_brk(unsigned long start, unsigned long end)
{
	start = PAGE_ALIGN(start);
	end = PAGE_ALIGN(end);
	if (end <= start) 
		return;
	do_brk(start, end - start);
}


/* We need to explicitly zero any fractional pages
 * after the data section (i.e. bss).  This would
 * contain the junk from the file that should not
 * be in memory.
 */
static void padzero(unsigned long elf_bss)
{
	unsigned long nbyte;

	nbyte = elf_bss & (PAGE_SIZE-1);
	if (nbyte) {
		nbyte = PAGE_SIZE - nbyte;
		clear_user((void *) elf_bss, nbyte);
	}
}

unsigned long * create_irix_tables(char * p, int argc, int envc,
				   struct elfhdr * exec, unsigned int load_addr,
				   unsigned int interp_load_addr,
				   struct pt_regs *regs, struct elf_phdr *ephdr)
{
	elf_caddr_t *argv;
	elf_caddr_t *envp;
	elf_addr_t *sp, *csp;
	
#ifdef DEBUG_ELF
	printk("create_irix_tables: p[%p] argc[%d] envc[%d] "
	       "load_addr[%08x] interp_load_addr[%08x]\n",
	       p, argc, envc, load_addr, interp_load_addr);
#endif
	sp = (elf_addr_t *) (~15UL & (unsigned long) p);
	csp = sp;
	csp -= exec ? DLINFO_ITEMS*2 : 2;
	csp -= envc+1;
	csp -= argc+1;
	csp -= 1;		/* argc itself */
	if ((unsigned long)csp & 15UL) {
		sp -= (16UL - ((unsigned long)csp & 15UL)) / sizeof(*sp);
	}

	/*
	 * Put the ELF interpreter info on the stack
	 */
#define NEW_AUX_ENT(nr, id, val) \
	  __put_user ((id), sp+(nr*2)); \
	  __put_user ((val), sp+(nr*2+1)); \

	sp -= 2;
	NEW_AUX_ENT(0, AT_NULL, 0);

	if(exec) {
		sp -= 11*2;

		NEW_AUX_ENT (0, AT_PHDR, load_addr + exec->e_phoff);
		NEW_AUX_ENT (1, AT_PHENT, sizeof (struct elf_phdr));
		NEW_AUX_ENT (2, AT_PHNUM, exec->e_phnum);
		NEW_AUX_ENT (3, AT_PAGESZ, ELF_EXEC_PAGESIZE);
		NEW_AUX_ENT (4, AT_BASE, interp_load_addr);
		NEW_AUX_ENT (5, AT_FLAGS, 0);
		NEW_AUX_ENT (6, AT_ENTRY, (elf_addr_t) exec->e_entry);
		NEW_AUX_ENT (7, AT_UID, (elf_addr_t) current->uid);
		NEW_AUX_ENT (8, AT_EUID, (elf_addr_t) current->euid);
		NEW_AUX_ENT (9, AT_GID, (elf_addr_t) current->gid);
		NEW_AUX_ENT (10, AT_EGID, (elf_addr_t) current->egid);
	}
#undef NEW_AUX_ENT

	sp -= envc+1;
	envp = (elf_caddr_t *) sp;
	sp -= argc+1;
	argv = (elf_caddr_t *) sp;

	__put_user((elf_addr_t)argc,--sp);
	current->mm->arg_start = (unsigned long) p;
	while (argc-->0) {
		__put_user((elf_caddr_t)(unsigned long)p,argv++);
		p += strlen_user(p);
	}
	__put_user(NULL, argv);
	current->mm->arg_end = current->mm->env_start = (unsigned long) p;
	while (envc-->0) {
		__put_user((elf_caddr_t)(unsigned long)p,envp++);
		p += strlen_user(p);
	}
	__put_user(NULL, envp);
	current->mm->env_end = (unsigned long) p;
	return sp;
}


/* This is much more generalized than the library routine read function,
 * so we keep this separate.  Technically the library read function
 * is only provided so that we can read a.out libraries that have
 * an ELF header.
 */
static unsigned int load_irix_interp(struct elfhdr * interp_elf_ex,
				     struct file * interpreter,
				     unsigned int *interp_load_addr)
{
	struct elf_phdr *elf_phdata  =  NULL;
	struct elf_phdr *eppnt;
	unsigned int len;
	unsigned int load_addr;
	int elf_bss;
	int retval;
	unsigned int last_bss;
	int error;
	int i;
	unsigned int k;

	elf_bss = 0;
	last_bss = 0;
	error = load_addr = 0;
	
#ifdef DEBUG_ELF
	print_elfhdr(interp_elf_ex);
#endif

	/* First of all, some simple consistency checks */
	if ((interp_elf_ex->e_type != ET_EXEC &&
	     interp_elf_ex->e_type != ET_DYN) ||
	     !irix_elf_check_arch(interp_elf_ex) ||
	     !interpreter->f_op->mmap) {
		printk("IRIX interp has bad e_type %d\n", interp_elf_ex->e_type);
		return 0xffffffff;
	}

	/* Now read in all of the header information */
	if(sizeof(struct elf_phdr) * interp_elf_ex->e_phnum > PAGE_SIZE) {
	    printk("IRIX interp header bigger than a page (%d)\n",
		   (sizeof(struct elf_phdr) * interp_elf_ex->e_phnum));
	    return 0xffffffff;
	}

	elf_phdata =  (struct elf_phdr *) 
		kmalloc(sizeof(struct elf_phdr) * interp_elf_ex->e_phnum,
			GFP_KERNEL);

	if(!elf_phdata) {
          printk("Cannot kmalloc phdata for IRIX interp.\n");
	  return 0xffffffff;
	}

	/* If the size of this structure has changed, then punt, since
	 * we will be doing the wrong thing.
	 */
	if(interp_elf_ex->e_phentsize != 32) {
		printk("IRIX interp e_phentsize == %d != 32 ",
		       interp_elf_ex->e_phentsize);
		kfree(elf_phdata);
		return 0xffffffff;
	}

	retval = kernel_read(interpreter, interp_elf_ex->e_phoff,
			   (char *) elf_phdata,
			   sizeof(struct elf_phdr) * interp_elf_ex->e_phnum);

#ifdef DEBUG_ELF
	dump_phdrs(elf_phdata, interp_elf_ex->e_phnum);
#endif

	eppnt = elf_phdata;
	for(i=0; i<interp_elf_ex->e_phnum; i++, eppnt++) {
	  if(eppnt->p_type == PT_LOAD) {
	    int elf_type = MAP_PRIVATE | MAP_DENYWRITE;
	    int elf_prot = 0;
	    unsigned long vaddr = 0;
	    if (eppnt->p_flags & PF_R) elf_prot =  PROT_READ;
	    if (eppnt->p_flags & PF_W) elf_prot |= PROT_WRITE;
	    if (eppnt->p_flags & PF_X) elf_prot |= PROT_EXEC;
	    elf_type |= MAP_FIXED;
	    vaddr = eppnt->p_vaddr;

#ifdef DEBUG_ELF
	    printk("INTERP do_mmap(%p, %08lx, %08lx, %08lx, %08lx, %08lx) ",
		   interpreter, vaddr,
		   (unsigned long) (eppnt->p_filesz + (eppnt->p_vaddr & 0xfff)),
		   (unsigned long) elf_prot, (unsigned long) elf_type,
		   (unsigned long) (eppnt->p_offset & 0xfffff000));
#endif
	    down(&current->mm->mmap_sem);
	    error = do_mmap(interpreter, vaddr,
			    eppnt->p_filesz + (eppnt->p_vaddr & 0xfff),
			    elf_prot, elf_type,
			    eppnt->p_offset & 0xfffff000);
	    up(&current->mm->mmap_sem);

	    if(error < 0 && error > -1024) {
		    printk("Aieee IRIX interp mmap error=%d\n", error);
		    break;  /* Real error */
	    }
#ifdef DEBUG_ELF
	    printk("error=%08lx ", (unsigned long) error);
#endif
	    if(!load_addr && interp_elf_ex->e_type == ET_DYN) {
	      load_addr = error;
#ifdef DEBUG_ELF
              printk("load_addr = error ");
#endif
	    }

	    /* Find the end of the file  mapping for this phdr, and keep
	     * track of the largest address we see for this.
	     */
	    k = eppnt->p_vaddr + eppnt->p_filesz;
	    if(k > elf_bss) elf_bss = k;

	    /* Do the same thing for the memory mapping - between
	     * elf_bss and last_bss is the bss section.
	     */
	    k = eppnt->p_memsz + eppnt->p_vaddr;
	    if(k > last_bss) last_bss = k;
#ifdef DEBUG_ELF
	    printk("\n");
#endif
	  }
	}

	/* Now use mmap to map the library into memory. */
	if(error < 0 && error > -1024) {
#ifdef DEBUG_ELF
		printk("got error %d\n", error);
#endif
		kfree(elf_phdata);
		return 0xffffffff;
	}

	/* Now fill out the bss section.  First pad the last page up
	 * to the page boundary, and then perform a mmap to make sure
	 * that there are zero-mapped pages up to and including the
	 * last bss page.
	 */
#ifdef DEBUG_ELF
	printk("padzero(%08lx) ", (unsigned long) (elf_bss));
#endif
	padzero(elf_bss);
	len = (elf_bss + 0xfff) & 0xfffff000; /* What we have mapped so far */

#ifdef DEBUG_ELF
	printk("last_bss[%08lx] len[%08lx]\n", (unsigned long) last_bss,
	       (unsigned long) len);
#endif

	/* Map the last of the bss segment */
	if (last_bss > len) {
		do_brk(len, (last_bss - len));
	}
	kfree(elf_phdata);

	*interp_load_addr = load_addr;
	return ((unsigned int) interp_elf_ex->e_entry);
}

/* Check sanity of IRIX elf executable header. */
static int verify_binary(struct elfhdr *ehp, struct linux_binprm *bprm)
{
	if (memcmp(ehp->e_ident, ELFMAG, SELFMAG) != 0)
		return -ENOEXEC;

	/* First of all, some simple consistency checks */
	if((ehp->e_type != ET_EXEC && ehp->e_type != ET_DYN) || 
	    !irix_elf_check_arch(ehp) || !bprm->file->f_op->mmap) {
		return -ENOEXEC;
	}

	/* Only support MIPS ARCH2 or greater IRIX binaries for now. */
	if(!(ehp->e_flags & EF_MIPS_ARCH) && !(ehp->e_flags & 0x04)) {
		return -ENOEXEC;
	}

	/* XXX Don't support N32 or 64bit binaries yet because they can
	 * XXX and do execute 64 bit instructions and expect all registers
	 * XXX to be 64 bit as well.  We need to make the kernel save
	 * XXX all registers as 64bits on cpu's capable of this at
	 * XXX exception time plus frob the XTLB exception vector.
	 */
	if((ehp->e_flags & 0x20)) {
		return -ENOEXEC;
	}

	return 0; /* It's ok. */
}

#define IRIX_INTERP_PREFIX "/usr/gnemul/irix"

/* Look for an IRIX ELF interpreter. */
static inline int look_for_irix_interpreter(char **name,
					    struct file **interpreter,
					    struct elfhdr *interp_elf_ex,
					    struct elf_phdr *epp,
					    struct linux_binprm *bprm, int pnum)
{
	int i;
	int retval = -EINVAL;
	struct file *file = NULL;

	*name = NULL;
	for(i = 0; i < pnum; i++, epp++) {
		if (epp->p_type != PT_INTERP)
			continue;

		/* It is illegal to have two interpreters for one executable. */
		if (*name != NULL)
			goto out;

		*name = (char *) kmalloc((epp->p_filesz +
					  strlen(IRIX_INTERP_PREFIX)),
					 GFP_KERNEL);
		if (!*name)
			return -ENOMEM;

		strcpy(*name, IRIX_INTERP_PREFIX);
		retval = kernel_read(bprm->file, epp->p_offset, (*name + 16),
		                     epp->p_filesz);
		if (retval < 0)
			goto out;

		file = open_exec(*name);
		if (IS_ERR(file)) {
			retval = PTR_ERR(file);
			goto out;
		}
		retval = kernel_read(file, 0, bprm->buf, 128);
		if (retval < 0)
			goto dput_and_out;

		*interp_elf_ex = *(struct elfhdr *) bprm->buf;
	}
	*interpreter = file;
	return 0;

dput_and_out:
	fput(file);
out:
	kfree(*name);
	return retval;
}

static inline int verify_irix_interpreter(struct elfhdr *ihp)
{
	if (memcmp(ihp->e_ident, ELFMAG, SELFMAG) != 0)
		return -ELIBBAD;
	return 0;
}

#define EXEC_MAP_FLAGS (MAP_FIXED | MAP_PRIVATE | MAP_DENYWRITE | MAP_EXECUTABLE)

static inline void map_executable(struct file *fp, struct elf_phdr *epp, int pnum,
				  unsigned int *estack, unsigned int *laddr,
				  unsigned int *scode, unsigned int *ebss,
				  unsigned int *ecode, unsigned int *edata,
				  unsigned int *ebrk)
{
	unsigned int tmp;
	int i, prot;

	for(i = 0; i < pnum; i++, epp++) {
		if(epp->p_type != PT_LOAD)
			continue;

		/* Map it. */
		prot  = (epp->p_flags & PF_R) ? PROT_READ : 0;
		prot |= (epp->p_flags & PF_W) ? PROT_WRITE : 0;
		prot |= (epp->p_flags & PF_X) ? PROT_EXEC : 0;
	        down(&current->mm->mmap_sem);
		(void) do_mmap(fp, (epp->p_vaddr & 0xfffff000),
			       (epp->p_filesz + (epp->p_vaddr & 0xfff)),
			       prot, EXEC_MAP_FLAGS,
			       (epp->p_offset & 0xfffff000));
	        up(&current->mm->mmap_sem);

		/* Fixup location tracking vars. */
		if((epp->p_vaddr & 0xfffff000) < *estack)
			*estack = (epp->p_vaddr & 0xfffff000);
		if(!*laddr)
			*laddr = epp->p_vaddr - epp->p_offset;
		if(epp->p_vaddr < *scode)
			*scode = epp->p_vaddr;

		tmp = epp->p_vaddr + epp->p_filesz;
		if(tmp > *ebss)
			*ebss = tmp;
		if((epp->p_flags & PF_X) && *ecode < tmp)
			*ecode = tmp;
		if(*edata < tmp)
			*edata = tmp;

		tmp = epp->p_vaddr + epp->p_memsz;
		if(tmp > *ebrk)
			*ebrk = tmp;
	}

}

static inline int map_interpreter(struct elf_phdr *epp, struct elfhdr *ihp,
				  struct file *interp, unsigned int *iladdr,
				  int pnum, mm_segment_t old_fs,
				  unsigned int *eentry)
{
	int i;

	*eentry = 0xffffffff;
	for(i = 0; i < pnum; i++, epp++) {
		if(epp->p_type != PT_INTERP)
			continue;

		/* We should have fielded this error elsewhere... */
		if(*eentry != 0xffffffff)
			return -1;

		set_fs(old_fs);
		*eentry = load_irix_interp(ihp, interp, iladdr);
		old_fs = get_fs();
		set_fs(get_ds());

		fput(interp);

		if (*eentry == 0xffffffff)
			return -1;
	}
	return 0;
}

/*
 * IRIX maps a page at 0x200000 that holds information about the 
 * process and the system, here we map the page and fill the
 * structure
 */
void irix_map_prda_page (void)
{
	unsigned long v;
	struct prda *pp;

	v =  do_brk (PRDA_ADDRESS, PAGE_SIZE);
	
	if (v < 0)
		return;

	pp = (struct prda *) v;
	pp->prda_sys.t_pid  = current->pid;
	pp->prda_sys.t_prid = read_32bit_cp0_register (CP0_PRID);
	pp->prda_sys.t_rpid = current->pid;

	/* We leave the rest set to zero */
}
	

	
/* These are the functions used to load ELF style executables and shared
 * libraries.  There is no binary dependent code anywhere else.
 */
static int load_irix_binary(struct linux_binprm * bprm, struct pt_regs * regs)
{
	struct elfhdr elf_ex, interp_elf_ex;
	struct file *interpreter;
	struct elf_phdr *elf_phdata, *elf_ihdr, *elf_ephdr;
	unsigned int load_addr, elf_bss, elf_brk;
	unsigned int elf_entry, interp_load_addr = 0;
	unsigned int start_code, end_code, end_data, elf_stack;
	int retval, has_interp, has_ephdr, size, i;
	char *elf_interpreter;
	mm_segment_t old_fs;
	
	load_addr = 0;
	has_interp = has_ephdr = 0;
	elf_ihdr = elf_ephdr = 0;
	elf_ex = *((struct elfhdr *) bprm->buf);
	retval = -ENOEXEC;

	if (verify_binary(&elf_ex, bprm))
		goto out;

#ifdef DEBUG_ELF
	print_elfhdr(&elf_ex);
#endif

	/* Now read in all of the header information */
	size = elf_ex.e_phentsize * elf_ex.e_phnum;
	if (size > 65536)
		goto out;
	elf_phdata = (struct elf_phdr *) kmalloc(size, GFP_KERNEL);
	if (elf_phdata == NULL) {
		retval = -ENOMEM;
		goto out;
	}

	retval = kernel_read(bprm->file, elf_ex.e_phoff, (char *)elf_phdata, size);
	if (retval < 0)
		goto out_free_ph;

#ifdef DEBUG_ELF
	dump_phdrs(elf_phdata, elf_ex.e_phnum);
#endif

	/* Set some things for later. */
	for(i = 0; i < elf_ex.e_phnum; i++) {
		switch(elf_phdata[i].p_type) {
		case PT_INTERP:
			has_interp = 1;
			elf_ihdr = &elf_phdata[i];
			break;
		case PT_PHDR:
			has_ephdr = 1;
			elf_ephdr = &elf_phdata[i];
			break;
		};
	}
#ifdef DEBUG_ELF
	printk("\n");
#endif

	elf_bss = 0;
	elf_brk = 0;

	elf_stack = 0xffffffff;
	elf_interpreter = NULL;
	start_code = 0xffffffff;
	end_code = 0;
	end_data = 0;

	retval = look_for_irix_interpreter(&elf_interpreter,
	                                   &interpreter,
					   &interp_elf_ex, elf_phdata, bprm,
					   elf_ex.e_phnum);
	if (retval)
		goto out_free_file;

	if (elf_interpreter) {
		retval = verify_irix_interpreter(&interp_elf_ex);
		if(retval)
			goto out_free_interp;
	}

	/* OK, we are done with that, now set up the arg stuff,
	 * and then start this sucker up.
	 */
	retval = -E2BIG;
	if (!bprm->sh_bang && !bprm->p)
		goto out_free_interp;

	/* Flush all traces of the currently running executable */
	retval = flush_old_exec(bprm);
	if (retval)
		goto out_free_dentry;

	/* OK, This is the point of no return */
	current->mm->end_data = 0;
	current->mm->end_code = 0;
	current->mm->mmap = NULL;
	current->flags &= ~PF_FORKNOEXEC;
	elf_entry = (unsigned int) elf_ex.e_entry;
	
	/* Do this so that we can load the interpreter, if need be.  We will
	 * change some of these later.
	 */
	current->mm->rss = 0;
	setup_arg_pages(bprm);
	current->mm->start_stack = bprm->p;

	/* At this point, we assume that the image should be loaded at
	 * fixed address, not at a variable address.
	 */
	old_fs = get_fs();
	set_fs(get_ds());

	map_executable(bprm->file, elf_phdata, elf_ex.e_phnum, &elf_stack,
	               &load_addr, &start_code, &elf_bss, &end_code,
	               &end_data, &elf_brk);

	if(elf_interpreter) {
		retval = map_interpreter(elf_phdata, &interp_elf_ex,
					 interpreter, &interp_load_addr,
					 elf_ex.e_phnum, old_fs, &elf_entry);
		kfree(elf_interpreter);
		if(retval) {
			set_fs(old_fs);
			printk("Unable to load IRIX ELF interpreter\n");
			send_sig(SIGSEGV, current, 0);
			retval = 0;
			goto out_free_file;
		}
	}

	set_fs(old_fs);

	kfree(elf_phdata);
	set_personality(PER_IRIX32);
	set_binfmt(&irix_format);
	compute_creds(bprm);
	current->flags &= ~PF_FORKNOEXEC;
	bprm->p = (unsigned long) 
	  create_irix_tables((char *)bprm->p, bprm->argc, bprm->envc,
			(elf_interpreter ? &elf_ex : NULL),
			load_addr, interp_load_addr, regs, elf_ephdr);
	current->mm->start_brk = current->mm->brk = elf_brk;
	current->mm->end_code = end_code;
	current->mm->start_code = start_code;
	current->mm->end_data = end_data;
	current->mm->start_stack = bprm->p;

	/* Calling set_brk effectively mmaps the pages that we need for the
	 * bss and break sections.
	 */
	set_brk(elf_bss, elf_brk);

	/*
	 * IRIX maps a page at 0x200000 which holds some system
	 * information.  Programs depend on this.
	 */
	irix_map_prda_page ();

	padzero(elf_bss);

#ifdef DEBUG_ELF
	printk("(start_brk) %lx\n" , (long) current->mm->start_brk);
	printk("(end_code) %lx\n" , (long) current->mm->end_code);
	printk("(start_code) %lx\n" , (long) current->mm->start_code);
	printk("(end_data) %lx\n" , (long) current->mm->end_data);
	printk("(start_stack) %lx\n" , (long) current->mm->start_stack);
	printk("(brk) %lx\n" , (long) current->mm->brk);
#endif

#if 0 /* XXX No fucking way dude... */
	/* Why this, you ask???  Well SVr4 maps page 0 as read-only,
	 * and some applications "depend" upon this behavior.
	 * Since we do not have the power to recompile these, we
	 * emulate the SVr4 behavior.  Sigh.
	 */
	down(&current->mm->mmap_sem);
	(void) do_mmap(NULL, 0, 4096, PROT_READ | PROT_EXEC,
		       MAP_FIXED | MAP_PRIVATE, 0);
	up(&current->mm->mmap_sem);
#endif

	start_thread(regs, elf_entry, bprm->p);
	if (current->ptrace & PT_PTRACED)
		send_sig(SIGTRAP, current, 0);
	return 0;
out:
	return retval;

out_free_dentry:
	allow_write_access(interpreter);
	fput(interpreter);
out_free_interp:
	if (elf_interpreter)
		kfree(elf_interpreter);
out_free_file:
out_free_ph:
	kfree (elf_phdata);
	goto out;
}

/* This is really simpleminded and specialized - we are loading an
 * a.out library that is given an ELF header.
 */
static int load_irix_library(struct file *file)
{
	struct elfhdr elf_ex;
	struct elf_phdr *elf_phdata  =  NULL;
	unsigned int len = 0;
	int elf_bss = 0;
	int retval;
	unsigned int bss;
	int error;
	int i,j, k;

	error = kernel_read(file, 0, (char *) &elf_ex, sizeof(elf_ex));
	if (error != sizeof(elf_ex))
		return -ENOEXEC;

	if (memcmp(elf_ex.e_ident, ELFMAG, SELFMAG) != 0)
		return -ENOEXEC;

	/* First of all, some simple consistency checks. */
	if(elf_ex.e_type != ET_EXEC || elf_ex.e_phnum > 2 ||
	   !irix_elf_check_arch(&elf_ex) || !file->f_op->mmap)
		return -ENOEXEC;
	
	/* Now read in all of the header information. */
	if(sizeof(struct elf_phdr) * elf_ex.e_phnum > PAGE_SIZE)
		return -ENOEXEC;
	
	elf_phdata =  (struct elf_phdr *) 
		kmalloc(sizeof(struct elf_phdr) * elf_ex.e_phnum, GFP_KERNEL);
	if (elf_phdata == NULL)
		return -ENOMEM;
	
	retval = kernel_read(file, elf_ex.e_phoff, (char *) elf_phdata,
			   sizeof(struct elf_phdr) * elf_ex.e_phnum);
	
	j = 0;
	for(i=0; i<elf_ex.e_phnum; i++)
		if((elf_phdata + i)->p_type == PT_LOAD) j++;
	
	if(j != 1)  {
		kfree(elf_phdata);
		return -ENOEXEC;
	}
	
	while(elf_phdata->p_type != PT_LOAD) elf_phdata++;
	
	/* Now use mmap to map the library into memory. */
	down(&current->mm->mmap_sem);
	error = do_mmap(file,
			elf_phdata->p_vaddr & 0xfffff000,
			elf_phdata->p_filesz + (elf_phdata->p_vaddr & 0xfff),
			PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_FIXED | MAP_PRIVATE | MAP_DENYWRITE,
			elf_phdata->p_offset & 0xfffff000);
	up(&current->mm->mmap_sem);

	k = elf_phdata->p_vaddr + elf_phdata->p_filesz;
	if (k > elf_bss) elf_bss = k;

	if (error != (elf_phdata->p_vaddr & 0xfffff000)) {
		kfree(elf_phdata);
		return error;
	}

	padzero(elf_bss);

	len = (elf_phdata->p_filesz + elf_phdata->p_vaddr+ 0xfff) & 0xfffff000;
	bss = elf_phdata->p_memsz + elf_phdata->p_vaddr;
	if (bss > len)
	  do_brk(len, bss-len);
	kfree(elf_phdata);
	return 0;
}
	
/* Called through irix_syssgi() to map an elf image given an FD,
 * a phdr ptr USER_PHDRP in userspace, and a count CNT telling how many
 * phdrs there are in the USER_PHDRP array.  We return the vaddr the
 * first phdr was successfully mapped to.
 */
unsigned long irix_mapelf(int fd, struct elf_phdr *user_phdrp, int cnt)
{
	struct elf_phdr *hp;
	struct file *filp;
	int i, retval;

#ifdef DEBUG_ELF
	printk("irix_mapelf: fd[%d] user_phdrp[%p] cnt[%d]\n",
	       fd, user_phdrp, cnt);
#endif

	/* First get the verification out of the way. */
	hp = user_phdrp;
	retval = verify_area(VERIFY_READ, hp, (sizeof(struct elf_phdr) * cnt));
	if(retval) {
#ifdef DEBUG_ELF
		printk("irix_mapelf: verify_area fails!\n");
#endif
		return retval;
	}

#ifdef DEBUG_ELF
	dump_phdrs(user_phdrp, cnt);
#endif

	for(i = 0; i < cnt; i++, hp++)
		if(hp->p_type != PT_LOAD) {
			printk("irix_mapelf: One section is not PT_LOAD!\n");
			return -ENOEXEC;
		}

	filp = fget(fd);
	if (!filp)
		return -EACCES;
	if(!filp->f_op) {
		printk("irix_mapelf: Bogon filp!\n");
		fput(filp);
		return -EACCES;
	}

	hp = user_phdrp;
	for(i = 0; i < cnt; i++, hp++) {
		int prot;

		prot  = (hp->p_flags & PF_R) ? PROT_READ : 0;
		prot |= (hp->p_flags & PF_W) ? PROT_WRITE : 0;
		prot |= (hp->p_flags & PF_X) ? PROT_EXEC : 0;
		down(&current->mm->mmap_sem);
		retval = do_mmap(filp, (hp->p_vaddr & 0xfffff000),
				 (hp->p_filesz + (hp->p_vaddr & 0xfff)),
				 prot, (MAP_FIXED | MAP_PRIVATE | MAP_DENYWRITE),
				 (hp->p_offset & 0xfffff000));
		up(&current->mm->mmap_sem);

		if(retval != (hp->p_vaddr & 0xfffff000)) {
			printk("irix_mapelf: do_mmap fails with %d!\n", retval);
			fput(filp);
			return retval;
		}
	}

#ifdef DEBUG_ELF
	printk("irix_mapelf: Success, returning %08lx\n", user_phdrp->p_vaddr);
#endif
	fput(filp);
	return user_phdrp->p_vaddr;
}

/*
 * ELF core dumper
 *
 * Modelled on fs/exec.c:aout_core_dump()
 * Jeremy Fitzhardinge <jeremy@sw.oz.au>
 */

/* These are the only things you should do on a core-file: use only these
 * functions to write out all the necessary info.
 */
static int dump_write(struct file *file, const void *addr, int nr)
{
	return file->f_op->write(file, addr, nr, &file->f_pos) == nr;
}

static int dump_seek(struct file *file, off_t off)
{
	if (file->f_op->llseek) {
		if (file->f_op->llseek(file, off, 0) != off)
			return 0;
	} else
		file->f_pos = off;
	return 1;
}

/* Decide whether a segment is worth dumping; default is yes to be
 * sure (missing info is worse than too much; etc).
 * Personally I'd include everything, and use the coredump limit...
 *
 * I think we should skip something. But I am not sure how. H.J.
 */
static inline int maydump(struct vm_area_struct *vma)
{
	if (!(vma->vm_flags & (VM_READ|VM_WRITE|VM_EXEC)))
		return 0;
#if 1
	if (vma->vm_flags & (VM_WRITE|VM_GROWSUP|VM_GROWSDOWN))
		return 1;
	if (vma->vm_flags & (VM_READ|VM_EXEC|VM_EXECUTABLE|VM_SHARED))
		return 0;
#endif
	return 1;
}

#define roundup(x, y)  ((((x)+((y)-1))/(y))*(y))

/* An ELF note in memory. */
struct memelfnote
{
	const char *name;
	int type;
	unsigned int datasz;
	void *data;
};

static int notesize(struct memelfnote *en)
{
	int sz;

	sz = sizeof(struct elf_note);
	sz += roundup(strlen(en->name), 4);
	sz += roundup(en->datasz, 4);

	return sz;
}

/* #define DEBUG */

#define DUMP_WRITE(addr, nr)	\
	if (!dump_write(file, (addr), (nr))) \
		goto end_coredump;
#define DUMP_SEEK(off)	\
	if (!dump_seek(file, (off))) \
		goto end_coredump;

static int writenote(struct memelfnote *men, struct file *file)
{
	struct elf_note en;

	en.n_namesz = strlen(men->name);
	en.n_descsz = men->datasz;
	en.n_type = men->type;

	DUMP_WRITE(&en, sizeof(en));
	DUMP_WRITE(men->name, en.n_namesz);
	/* XXX - cast from long long to long to avoid need for libgcc.a */
	DUMP_SEEK(roundup((unsigned long)file->f_pos, 4));	/* XXX */
	DUMP_WRITE(men->data, men->datasz);
	DUMP_SEEK(roundup((unsigned long)file->f_pos, 4));	/* XXX */
	
	return 1;

end_coredump:
	return 0;
}
#undef DUMP_WRITE
#undef DUMP_SEEK

#define DUMP_WRITE(addr, nr)	\
	if (!dump_write(file, (addr), (nr))) \
		goto end_coredump;
#define DUMP_SEEK(off)	\
	if (!dump_seek(file, (off))) \
		goto end_coredump;

/* Actual dumper.
 *
 * This is a two-pass process; first we find the offsets of the bits,
 * and then they are actually written out.  If we run out of core limit
 * we just truncate.
 */
static int irix_core_dump(long signr, struct pt_regs * regs, struct file *file)
{
	int has_dumped = 0;
	mm_segment_t fs;
	int segs;
	int i;
	size_t size;
	struct vm_area_struct *vma;
	struct elfhdr elf;
	off_t offset = 0, dataoff;
	int limit = current->rlim[RLIMIT_CORE].rlim_cur;
	int numnote = 4;
	struct memelfnote notes[4];
	struct elf_prstatus prstatus;	/* NT_PRSTATUS */
	elf_fpregset_t fpu;		/* NT_PRFPREG */
	struct elf_prpsinfo psinfo;	/* NT_PRPSINFO */

	/* Count what's needed to dump, up to the limit of coredump size. */
	segs = 0;
	size = 0;
	for(vma = current->mm->mmap; vma != NULL; vma = vma->vm_next) {
		if (maydump(vma))
		{
			int sz = vma->vm_end-vma->vm_start;
		
			if (size+sz >= limit)
				break;
			else
				size += sz;
		}
		
		segs++;
	}
#ifdef DEBUG
	printk("irix_core_dump: %d segs taking %d bytes\n", segs, size);
#endif

	/* Set up header. */
	memcpy(elf.e_ident, ELFMAG, SELFMAG);
	elf.e_ident[EI_CLASS] = ELFCLASS32;
	elf.e_ident[EI_DATA] = ELFDATA2LSB;
	elf.e_ident[EI_VERSION] = EV_CURRENT;
	memset(elf.e_ident+EI_PAD, 0, EI_NIDENT-EI_PAD);

	elf.e_type = ET_CORE;
	elf.e_machine = ELF_ARCH;
	elf.e_version = EV_CURRENT;
	elf.e_entry = 0;
	elf.e_phoff = sizeof(elf);
	elf.e_shoff = 0;
	elf.e_flags = 0;
	elf.e_ehsize = sizeof(elf);
	elf.e_phentsize = sizeof(struct elf_phdr);
	elf.e_phnum = segs+1;		/* Include notes. */
	elf.e_shentsize = 0;
	elf.e_shnum = 0;
	elf.e_shstrndx = 0;
	
	fs = get_fs();
	set_fs(KERNEL_DS);

	has_dumped = 1;
	current->flags |= PF_DUMPCORE;

	DUMP_WRITE(&elf, sizeof(elf));
	offset += sizeof(elf);				/* Elf header. */
	offset += (segs+1) * sizeof(struct elf_phdr);	/* Program headers. */

	/* Set up the notes in similar form to SVR4 core dumps made
	 * with info from their /proc.
	 */
	memset(&psinfo, 0, sizeof(psinfo));
	memset(&prstatus, 0, sizeof(prstatus));

	notes[0].name = "CORE";
	notes[0].type = NT_PRSTATUS;
	notes[0].datasz = sizeof(prstatus);
	notes[0].data = &prstatus;
	prstatus.pr_info.si_signo = prstatus.pr_cursig = signr;
	prstatus.pr_sigpend = current->pending.signal.sig[0];
	prstatus.pr_sighold = current->blocked.sig[0];
	psinfo.pr_pid = prstatus.pr_pid = current->pid;
	psinfo.pr_ppid = prstatus.pr_ppid = current->p_pptr->pid;
	psinfo.pr_pgrp = prstatus.pr_pgrp = current->pgrp;
	psinfo.pr_sid = prstatus.pr_sid = current->session;
	prstatus.pr_utime.tv_sec = CT_TO_SECS(current->times.tms_utime);
	prstatus.pr_utime.tv_usec = CT_TO_USECS(current->times.tms_utime);
	prstatus.pr_stime.tv_sec = CT_TO_SECS(current->times.tms_stime);
	prstatus.pr_stime.tv_usec = CT_TO_USECS(current->times.tms_stime);
	prstatus.pr_cutime.tv_sec = CT_TO_SECS(current->times.tms_cutime);
	prstatus.pr_cutime.tv_usec = CT_TO_USECS(current->times.tms_cutime);
	prstatus.pr_cstime.tv_sec = CT_TO_SECS(current->times.tms_cstime);
	prstatus.pr_cstime.tv_usec = CT_TO_USECS(current->times.tms_cstime);
	if (sizeof(elf_gregset_t) != sizeof(struct pt_regs)) {
		printk("sizeof(elf_gregset_t) (%d) != sizeof(struct pt_regs) "
		       "(%d)\n", sizeof(elf_gregset_t), sizeof(struct pt_regs));
	} else {
		*(struct pt_regs *)&prstatus.pr_reg = *regs;
	}
	
	notes[1].name = "CORE";
	notes[1].type = NT_PRPSINFO;
	notes[1].datasz = sizeof(psinfo);
	notes[1].data = &psinfo;
	i = current->state ? ffz(~current->state) + 1 : 0;
	psinfo.pr_state = i;
	psinfo.pr_sname = (i < 0 || i > 5) ? '.' : "RSDZTD"[i];
	psinfo.pr_zomb = psinfo.pr_sname == 'Z';
	psinfo.pr_nice = current->nice;
	psinfo.pr_flag = current->flags;
	psinfo.pr_uid = current->uid;
	psinfo.pr_gid = current->gid;
	{
		int i, len;

		set_fs(fs);
		
		len = current->mm->arg_end - current->mm->arg_start;
		len = len >= ELF_PRARGSZ ? ELF_PRARGSZ : len;
		copy_from_user(&psinfo.pr_psargs,
			       (const char *)current->mm->arg_start, len);
		for(i = 0; i < len; i++)
			if (psinfo.pr_psargs[i] == 0)
				psinfo.pr_psargs[i] = ' ';
		psinfo.pr_psargs[len] = 0;

		set_fs(KERNEL_DS);
	}
	strncpy(psinfo.pr_fname, current->comm, sizeof(psinfo.pr_fname));

	notes[2].name = "CORE";
	notes[2].type = NT_TASKSTRUCT;
	notes[2].datasz = sizeof(*current);
	notes[2].data = current;

	/* Try to dump the FPU. */
	prstatus.pr_fpvalid = dump_fpu (&fpu);
	if (!prstatus.pr_fpvalid) {
		numnote--;
	} else {
		notes[3].name = "CORE";
		notes[3].type = NT_PRFPREG;
		notes[3].datasz = sizeof(fpu);
		notes[3].data = &fpu;
	}

	/* Write notes phdr entry. */
	{
		struct elf_phdr phdr;
		int sz = 0;

		for(i = 0; i < numnote; i++)
			sz += notesize(&notes[i]);
		
		phdr.p_type = PT_NOTE;
		phdr.p_offset = offset;
		phdr.p_vaddr = 0;
		phdr.p_paddr = 0;
		phdr.p_filesz = sz;
		phdr.p_memsz = 0;
		phdr.p_flags = 0;
		phdr.p_align = 0;

		offset += phdr.p_filesz;
		DUMP_WRITE(&phdr, sizeof(phdr));
	}

	/* Page-align dumped data. */
	dataoff = offset = roundup(offset, PAGE_SIZE);
	
	/* Write program headers for segments dump. */
	for(vma = current->mm->mmap, i = 0;
		i < segs && vma != NULL; vma = vma->vm_next) {
		struct elf_phdr phdr;
		size_t sz;

		i++;

		sz = vma->vm_end - vma->vm_start;
		
		phdr.p_type = PT_LOAD;
		phdr.p_offset = offset;
		phdr.p_vaddr = vma->vm_start;
		phdr.p_paddr = 0;
		phdr.p_filesz = maydump(vma) ? sz : 0;
		phdr.p_memsz = sz;
		offset += phdr.p_filesz;
		phdr.p_flags = vma->vm_flags & VM_READ ? PF_R : 0;
		if (vma->vm_flags & VM_WRITE) phdr.p_flags |= PF_W;
		if (vma->vm_flags & VM_EXEC) phdr.p_flags |= PF_X;
		phdr.p_align = PAGE_SIZE;

		DUMP_WRITE(&phdr, sizeof(phdr));
	}

	for(i = 0; i < numnote; i++)
		if (!writenote(&notes[i], file))
			goto end_coredump;
	
	set_fs(fs);

	DUMP_SEEK(dataoff);
	
	for(i = 0, vma = current->mm->mmap;
	    i < segs && vma != NULL;
	    vma = vma->vm_next) {
		unsigned long addr = vma->vm_start;
		unsigned long len = vma->vm_end - vma->vm_start;
		
		if (!maydump(vma))
			continue;
		i++;
#ifdef DEBUG
		printk("elf_core_dump: writing %08lx %lx\n", addr, len);
#endif
		DUMP_WRITE((void *)addr, len);
	}

	if ((off_t) file->f_pos != offset) {
		/* Sanity check. */
		printk("elf_core_dump: file->f_pos (%ld) != offset (%ld)\n",
		       (off_t) file->f_pos, offset);
	}

end_coredump:
	set_fs(fs);
	return has_dumped;
}

static int __init init_irix_binfmt(void)
{
	return register_binfmt(&irix_format);
}

static void __exit exit_irix_binfmt(void)
{
	/* Remove the IRIX ELF loaders. */
	unregister_binfmt(&irix_format);
}

module_init(init_irix_binfmt)
module_exit(exit_irix_binfmt)
