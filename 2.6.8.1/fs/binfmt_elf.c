/*
 * linux/fs/binfmt_elf.c
 *
 * These are the functions used to load ELF format executables as used
 * on SVr4 machines.  Information on the format may be found in the book
 * "UNIX SYSTEM V RELEASE 4 Programmers Guide: Ansi C and Programming Support
 * Tools".
 *
 * Copyright 1993, 1994: Eric Youngdale (ericy@cais.com).
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/time.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/a.out.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/binfmts.h>
#include <linux/string.h>
#include <linux/file.h>
#include <linux/fcntl.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/shm.h>
#include <linux/personality.h>
#include <linux/elfcore.h>
#include <linux/init.h>
#include <linux/highuid.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/compiler.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/security.h>
#include <linux/syscalls.h>

#include <asm/uaccess.h>
#include <asm/param.h>

#include <linux/elf.h>

static int load_elf_binary(struct linux_binprm * bprm, struct pt_regs * regs);
static int load_elf_library(struct file*);
static unsigned long elf_map (struct file *, unsigned long, struct elf_phdr *, int, int);
extern int dump_fpu (struct pt_regs *, elf_fpregset_t *);

#ifndef elf_addr_t
#define elf_addr_t unsigned long
#endif

/*
 * If we don't support core dumping, then supply a NULL so we
 * don't even try.
 */
#ifdef USE_ELF_CORE_DUMP
static int elf_core_dump(long signr, struct pt_regs * regs, struct file * file);
#else
#define elf_core_dump	NULL
#endif

#if ELF_EXEC_PAGESIZE > PAGE_SIZE
# define ELF_MIN_ALIGN	ELF_EXEC_PAGESIZE
#else
# define ELF_MIN_ALIGN	PAGE_SIZE
#endif

#define ELF_PAGESTART(_v) ((_v) & ~(unsigned long)(ELF_MIN_ALIGN-1))
#define ELF_PAGEOFFSET(_v) ((_v) & (ELF_MIN_ALIGN-1))
#define ELF_PAGEALIGN(_v) (((_v) + ELF_MIN_ALIGN - 1) & ~(ELF_MIN_ALIGN - 1))

static struct linux_binfmt elf_format = {
		.module		= THIS_MODULE,
		.load_binary	= load_elf_binary,
		.load_shlib	= load_elf_library,
		.core_dump	= elf_core_dump,
		.min_coredump	= ELF_EXEC_PAGESIZE
};

#define BAD_ADDR(x)	((unsigned long)(x) > TASK_SIZE)

static int set_brk(unsigned long start, unsigned long end)
{
	start = ELF_PAGEALIGN(start);
	end = ELF_PAGEALIGN(end);
	if (end > start) {
		unsigned long addr = do_brk(start, end - start);
		if (BAD_ADDR(addr))
			return addr;
	}
	current->mm->start_brk = current->mm->brk = end;
	return 0;
}


/* We need to explicitly zero any fractional pages
   after the data section (i.e. bss).  This would
   contain the junk from the file that should not
   be in memory */


static void padzero(unsigned long elf_bss)
{
	unsigned long nbyte;

	nbyte = ELF_PAGEOFFSET(elf_bss);
	if (nbyte) {
		nbyte = ELF_MIN_ALIGN - nbyte;
		clear_user((void __user *) elf_bss, nbyte);
	}
}

/* Let's use some macros to make this stack manipulation a litle clearer */
#ifdef CONFIG_STACK_GROWSUP
#define STACK_ADD(sp, items) ((elf_addr_t __user *)(sp) + (items))
#define STACK_ROUND(sp, items) \
	((15 + (unsigned long) ((sp) + (items))) &~ 15UL)
#define STACK_ALLOC(sp, len) ({ elf_addr_t __user *old_sp = (elf_addr_t __user *)sp; sp += len; old_sp; })
#else
#define STACK_ADD(sp, items) ((elf_addr_t __user *)(sp) - (items))
#define STACK_ROUND(sp, items) \
	(((unsigned long) (sp - items)) &~ 15UL)
#define STACK_ALLOC(sp, len) ({ sp -= len ; sp; })
#endif

static void
create_elf_tables(struct linux_binprm *bprm, struct elfhdr * exec,
		int interp_aout, unsigned long load_addr,
		unsigned long interp_load_addr)
{
	unsigned long p = bprm->p;
	int argc = bprm->argc;
	int envc = bprm->envc;
	elf_addr_t __user *argv;
	elf_addr_t __user *envp;
	elf_addr_t __user *sp;
	elf_addr_t __user *u_platform;
	const char *k_platform = ELF_PLATFORM;
	int items;
	elf_addr_t *elf_info;
	int ei_index = 0;
	struct task_struct *tsk = current;

	/*
	 * If this architecture has a platform capability string, copy it
	 * to userspace.  In some cases (Sparc), this info is impossible
	 * for userspace to get any other way, in others (i386) it is
	 * merely difficult.
	 */

	u_platform = NULL;
	if (k_platform) {
		size_t len = strlen(k_platform) + 1;

#ifdef CONFIG_X86_HT
		/*
		 * In some cases (e.g. Hyper-Threading), we want to avoid L1
		 * evictions by the processes running on the same package. One
		 * thing we can do is to shuffle the initial stack for them.
		 *
		 * The conditionals here are unneeded, but kept in to make the
		 * code behaviour the same as pre change unless we have
		 * hyperthreaded processors. This should be cleaned up
		 * before 2.6
		 */
	 
		if (smp_num_siblings > 1)
			STACK_ALLOC(p, ((current->pid % 64) << 7));
#endif
		u_platform = (elf_addr_t __user *)STACK_ALLOC(p, len);
		__copy_to_user(u_platform, k_platform, len);
	}

	/* Create the ELF interpreter info */
	elf_info = (elf_addr_t *) current->mm->saved_auxv;
#define NEW_AUX_ENT(id, val) \
	do { elf_info[ei_index++] = id; elf_info[ei_index++] = val; } while (0)

#ifdef ARCH_DLINFO
	/* 
	 * ARCH_DLINFO must come first so PPC can do its special alignment of
	 * AUXV.
	 */
	ARCH_DLINFO;
#endif
	NEW_AUX_ENT(AT_HWCAP, ELF_HWCAP);
	NEW_AUX_ENT(AT_PAGESZ, ELF_EXEC_PAGESIZE);
	NEW_AUX_ENT(AT_CLKTCK, CLOCKS_PER_SEC);
	NEW_AUX_ENT(AT_PHDR, load_addr + exec->e_phoff);
	NEW_AUX_ENT(AT_PHENT, sizeof (struct elf_phdr));
	NEW_AUX_ENT(AT_PHNUM, exec->e_phnum);
	NEW_AUX_ENT(AT_BASE, interp_load_addr);
	NEW_AUX_ENT(AT_FLAGS, 0);
	NEW_AUX_ENT(AT_ENTRY, exec->e_entry);
	NEW_AUX_ENT(AT_UID, (elf_addr_t) tsk->uid);
	NEW_AUX_ENT(AT_EUID, (elf_addr_t) tsk->euid);
	NEW_AUX_ENT(AT_GID, (elf_addr_t) tsk->gid);
	NEW_AUX_ENT(AT_EGID, (elf_addr_t) tsk->egid);
 	NEW_AUX_ENT(AT_SECURE, (elf_addr_t) security_bprm_secureexec(bprm));
	if (k_platform) {
		NEW_AUX_ENT(AT_PLATFORM, (elf_addr_t)(unsigned long)u_platform);
	}
	if (bprm->interp_flags & BINPRM_FLAGS_EXECFD) {
		NEW_AUX_ENT(AT_EXECFD, (elf_addr_t) bprm->interp_data);
	}
#undef NEW_AUX_ENT
	/* AT_NULL is zero; clear the rest too */
	memset(&elf_info[ei_index], 0,
	       sizeof current->mm->saved_auxv - ei_index * sizeof elf_info[0]);

	/* And advance past the AT_NULL entry.  */
	ei_index += 2;

	sp = STACK_ADD(p, ei_index);

	items = (argc + 1) + (envc + 1);
	if (interp_aout) {
		items += 3; /* a.out interpreters require argv & envp too */
	} else {
		items += 1; /* ELF interpreters only put argc on the stack */
	}
	bprm->p = STACK_ROUND(sp, items);

	/* Point sp at the lowest address on the stack */
#ifdef CONFIG_STACK_GROWSUP
	sp = (elf_addr_t __user *)bprm->p - items - ei_index;
	bprm->exec = (unsigned long) sp; /* XXX: PARISC HACK */
#else
	sp = (elf_addr_t __user *)bprm->p;
#endif

	/* Now, let's put argc (and argv, envp if appropriate) on the stack */
	__put_user(argc, sp++);
	if (interp_aout) {
		argv = sp + 2;
		envp = argv + argc + 1;
		__put_user((elf_addr_t)(unsigned long)argv, sp++);
		__put_user((elf_addr_t)(unsigned long)envp, sp++);
	} else {
		argv = sp;
		envp = argv + argc + 1;
	}

	/* Populate argv and envp */
	p = current->mm->arg_start;
	while (argc-- > 0) {
		size_t len;
		__put_user((elf_addr_t)p, argv++);
		len = strnlen_user((void __user *)p, PAGE_SIZE*MAX_ARG_PAGES);
		if (!len || len > PAGE_SIZE*MAX_ARG_PAGES)
			return;
		p += len;
	}
	__put_user(0, argv);
	current->mm->arg_end = current->mm->env_start = p;
	while (envc-- > 0) {
		size_t len;
		__put_user((elf_addr_t)p, envp++);
		len = strnlen_user((void __user *)p, PAGE_SIZE*MAX_ARG_PAGES);
		if (!len || len > PAGE_SIZE*MAX_ARG_PAGES)
			return;
		p += len;
	}
	__put_user(0, envp);
	current->mm->env_end = p;

	/* Put the elf_info on the stack in the right place.  */
	sp = (elf_addr_t __user *)envp + 1;
	copy_to_user(sp, elf_info, ei_index * sizeof(elf_addr_t));
}

#ifndef elf_map

static unsigned long elf_map(struct file *filep, unsigned long addr,
			struct elf_phdr *eppnt, int prot, int type)
{
	unsigned long map_addr;

	down_write(&current->mm->mmap_sem);
	map_addr = do_mmap(filep, ELF_PAGESTART(addr),
			   eppnt->p_filesz + ELF_PAGEOFFSET(eppnt->p_vaddr), prot, type,
			   eppnt->p_offset - ELF_PAGEOFFSET(eppnt->p_vaddr));
	up_write(&current->mm->mmap_sem);
	return(map_addr);
}

#endif /* !elf_map */

/* This is much more generalized than the library routine read function,
   so we keep this separate.  Technically the library read function
   is only provided so that we can read a.out libraries that have
   an ELF header */

static unsigned long load_elf_interp(struct elfhdr * interp_elf_ex,
				     struct file * interpreter,
				     unsigned long *interp_load_addr)
{
	struct elf_phdr *elf_phdata;
	struct elf_phdr *eppnt;
	unsigned long load_addr = 0;
	int load_addr_set = 0;
	unsigned long last_bss = 0, elf_bss = 0;
	unsigned long error = ~0UL;
	int retval, i, size;

	/* First of all, some simple consistency checks */
	if (interp_elf_ex->e_type != ET_EXEC &&
	    interp_elf_ex->e_type != ET_DYN)
		goto out;
	if (!elf_check_arch(interp_elf_ex))
		goto out;
	if (!interpreter->f_op || !interpreter->f_op->mmap)
		goto out;

	/*
	 * If the size of this structure has changed, then punt, since
	 * we will be doing the wrong thing.
	 */
	if (interp_elf_ex->e_phentsize != sizeof(struct elf_phdr))
		goto out;
	if (interp_elf_ex->e_phnum > 65536U / sizeof(struct elf_phdr))
		goto out;

	/* Now read in all of the header information */

	size = sizeof(struct elf_phdr) * interp_elf_ex->e_phnum;
	if (size > ELF_MIN_ALIGN)
		goto out;
	elf_phdata = (struct elf_phdr *) kmalloc(size, GFP_KERNEL);
	if (!elf_phdata)
		goto out;

	retval = kernel_read(interpreter,interp_elf_ex->e_phoff,(char *)elf_phdata,size);
	error = retval;
	if (retval < 0)
		goto out_close;

	eppnt = elf_phdata;
	for (i=0; i<interp_elf_ex->e_phnum; i++, eppnt++) {
	  if (eppnt->p_type == PT_LOAD) {
	    int elf_type = MAP_PRIVATE | MAP_DENYWRITE;
	    int elf_prot = 0;
	    unsigned long vaddr = 0;
	    unsigned long k, map_addr;

	    if (eppnt->p_flags & PF_R) elf_prot =  PROT_READ;
	    if (eppnt->p_flags & PF_W) elf_prot |= PROT_WRITE;
	    if (eppnt->p_flags & PF_X) elf_prot |= PROT_EXEC;
	    vaddr = eppnt->p_vaddr;
	    if (interp_elf_ex->e_type == ET_EXEC || load_addr_set)
	    	elf_type |= MAP_FIXED;

	    map_addr = elf_map(interpreter, load_addr + vaddr, eppnt, elf_prot, elf_type);
	    error = map_addr;
	    if (BAD_ADDR(map_addr))
	    	goto out_close;

	    if (!load_addr_set && interp_elf_ex->e_type == ET_DYN) {
		load_addr = map_addr - ELF_PAGESTART(vaddr);
		load_addr_set = 1;
	    }

	    /*
	     * Check to see if the section's size will overflow the
	     * allowed task size. Note that p_filesz must always be
	     * <= p_memsize so it is only necessary to check p_memsz.
	     */
	    k = load_addr + eppnt->p_vaddr;
	    if (k > TASK_SIZE || eppnt->p_filesz > eppnt->p_memsz ||
		eppnt->p_memsz > TASK_SIZE || TASK_SIZE - eppnt->p_memsz < k) {
	        error = -ENOMEM;
		goto out_close;
	    }

	    /*
	     * Find the end of the file mapping for this phdr, and keep
	     * track of the largest address we see for this.
	     */
	    k = load_addr + eppnt->p_vaddr + eppnt->p_filesz;
	    if (k > elf_bss)
		elf_bss = k;

	    /*
	     * Do the same thing for the memory mapping - between
	     * elf_bss and last_bss is the bss section.
	     */
	    k = load_addr + eppnt->p_memsz + eppnt->p_vaddr;
	    if (k > last_bss)
		last_bss = k;
	  }
	}

	/*
	 * Now fill out the bss section.  First pad the last page up
	 * to the page boundary, and then perform a mmap to make sure
	 * that there are zero-mapped pages up to and including the 
	 * last bss page.
	 */
	padzero(elf_bss);
	elf_bss = ELF_PAGESTART(elf_bss + ELF_MIN_ALIGN - 1);	/* What we have mapped so far */

	/* Map the last of the bss segment */
	if (last_bss > elf_bss) {
		error = do_brk(elf_bss, last_bss - elf_bss);
		if (BAD_ADDR(error))
			goto out_close;
	}

	*interp_load_addr = load_addr;
	error = ((unsigned long) interp_elf_ex->e_entry) + load_addr;

out_close:
	kfree(elf_phdata);
out:
	return error;
}

static unsigned long load_aout_interp(struct exec * interp_ex,
			     struct file * interpreter)
{
	unsigned long text_data, elf_entry = ~0UL;
	char __user * addr;
	loff_t offset;

	current->mm->end_code = interp_ex->a_text;
	text_data = interp_ex->a_text + interp_ex->a_data;
	current->mm->end_data = text_data;
	current->mm->brk = interp_ex->a_bss + text_data;

	switch (N_MAGIC(*interp_ex)) {
	case OMAGIC:
		offset = 32;
		addr = (char __user *)0;
		break;
	case ZMAGIC:
	case QMAGIC:
		offset = N_TXTOFF(*interp_ex);
		addr = (char __user *) N_TXTADDR(*interp_ex);
		break;
	default:
		goto out;
	}

	do_brk(0, text_data);
	if (!interpreter->f_op || !interpreter->f_op->read)
		goto out;
	if (interpreter->f_op->read(interpreter, addr, text_data, &offset) < 0)
		goto out;
	flush_icache_range((unsigned long)addr,
	                   (unsigned long)addr + text_data);

	do_brk(ELF_PAGESTART(text_data + ELF_MIN_ALIGN - 1),
		interp_ex->a_bss);
	elf_entry = interp_ex->a_entry;

out:
	return elf_entry;
}

/*
 * These are the functions used to load ELF style executables and shared
 * libraries.  There is no binary dependent code anywhere else.
 */

#define INTERPRETER_NONE 0
#define INTERPRETER_AOUT 1
#define INTERPRETER_ELF 2


static int load_elf_binary(struct linux_binprm * bprm, struct pt_regs * regs)
{
	struct file *interpreter = NULL; /* to shut gcc up */
 	unsigned long load_addr = 0, load_bias = 0;
	int load_addr_set = 0;
	char * elf_interpreter = NULL;
	unsigned int interpreter_type = INTERPRETER_NONE;
	unsigned char ibcs2_interpreter = 0;
	unsigned long error;
	struct elf_phdr * elf_ppnt, *elf_phdata;
	unsigned long elf_bss, elf_brk;
	int elf_exec_fileno;
	int retval, i;
	unsigned int size;
	unsigned long elf_entry, interp_load_addr = 0;
	unsigned long start_code, end_code, start_data, end_data;
	unsigned long reloc_func_desc = 0;
	struct elfhdr elf_ex;
	struct elfhdr interp_elf_ex;
  	struct exec interp_ex;
	char passed_fileno[6];
	struct files_struct *files;
	int have_pt_gnu_stack, executable_stack = EXSTACK_DEFAULT;
	unsigned long def_flags = 0;
	
	/* Get the exec-header */
	elf_ex = *((struct elfhdr *) bprm->buf);

	retval = -ENOEXEC;
	/* First of all, some simple consistency checks */
	if (memcmp(elf_ex.e_ident, ELFMAG, SELFMAG) != 0)
		goto out;

	if (elf_ex.e_type != ET_EXEC && elf_ex.e_type != ET_DYN)
		goto out;
	if (!elf_check_arch(&elf_ex))
		goto out;
	if (!bprm->file->f_op||!bprm->file->f_op->mmap)
		goto out;

	/* Now read in all of the header information */

	retval = -ENOMEM;
	if (elf_ex.e_phentsize != sizeof(struct elf_phdr))
		goto out;
	if (elf_ex.e_phnum > 65536U / sizeof(struct elf_phdr))
		goto out;
	size = elf_ex.e_phnum * sizeof(struct elf_phdr);
	elf_phdata = (struct elf_phdr *) kmalloc(size, GFP_KERNEL);
	if (!elf_phdata)
		goto out;

	retval = kernel_read(bprm->file, elf_ex.e_phoff, (char *) elf_phdata, size);
	if (retval < 0)
		goto out_free_ph;

	files = current->files;		/* Refcounted so ok */
	retval = unshare_files();
	if (retval < 0)
		goto out_free_ph;
	if (files == current->files) {
		put_files_struct(files);
		files = NULL;
	}

	/* exec will make our files private anyway, but for the a.out
	   loader stuff we need to do it earlier */

	retval = get_unused_fd();
	if (retval < 0)
		goto out_free_fh;
	get_file(bprm->file);
	fd_install(elf_exec_fileno = retval, bprm->file);

	elf_ppnt = elf_phdata;
	elf_bss = 0;
	elf_brk = 0;

	start_code = ~0UL;
	end_code = 0;
	start_data = 0;
	end_data = 0;

	for (i = 0; i < elf_ex.e_phnum; i++) {
		if (elf_ppnt->p_type == PT_INTERP) {
			/* This is the program interpreter used for
			 * shared libraries - for now assume that this
			 * is an a.out format binary
			 */

			retval = -ENOMEM;
			if (elf_ppnt->p_filesz > PATH_MAX)
				goto out_free_file;
			elf_interpreter = (char *) kmalloc(elf_ppnt->p_filesz,
							   GFP_KERNEL);
			if (!elf_interpreter)
				goto out_free_file;

			retval = kernel_read(bprm->file, elf_ppnt->p_offset,
					   elf_interpreter,
					   elf_ppnt->p_filesz);
			if (retval < 0)
				goto out_free_interp;
			/* If the program interpreter is one of these two,
			 * then assume an iBCS2 image. Otherwise assume
			 * a native linux image.
			 */
			if (strcmp(elf_interpreter,"/usr/lib/libc.so.1") == 0 ||
			    strcmp(elf_interpreter,"/usr/lib/ld.so.1") == 0)
				ibcs2_interpreter = 1;

			/*
			 * The early SET_PERSONALITY here is so that the lookup
			 * for the interpreter happens in the namespace of the 
			 * to-be-execed image.  SET_PERSONALITY can select an
			 * alternate root.
			 *
			 * However, SET_PERSONALITY is NOT allowed to switch
			 * this task into the new images's memory mapping
			 * policy - that is, TASK_SIZE must still evaluate to
			 * that which is appropriate to the execing application.
			 * This is because exit_mmap() needs to have TASK_SIZE
			 * evaluate to the size of the old image.
			 *
			 * So if (say) a 64-bit application is execing a 32-bit
			 * application it is the architecture's responsibility
			 * to defer changing the value of TASK_SIZE until the
			 * switch really is going to happen - do this in
			 * flush_thread().	- akpm
			 */
			SET_PERSONALITY(elf_ex, ibcs2_interpreter);

			interpreter = open_exec(elf_interpreter);
			retval = PTR_ERR(interpreter);
			if (IS_ERR(interpreter))
				goto out_free_interp;
			retval = kernel_read(interpreter, 0, bprm->buf, BINPRM_BUF_SIZE);
			if (retval < 0)
				goto out_free_dentry;

			/* Get the exec headers */
			interp_ex = *((struct exec *) bprm->buf);
			interp_elf_ex = *((struct elfhdr *) bprm->buf);
			break;
		}
		elf_ppnt++;
	}

	elf_ppnt = elf_phdata;
	for (i = 0; i < elf_ex.e_phnum; i++, elf_ppnt++)
		if (elf_ppnt->p_type == PT_GNU_STACK) {
			if (elf_ppnt->p_flags & PF_X)
				executable_stack = EXSTACK_ENABLE_X;
			else
				executable_stack = EXSTACK_DISABLE_X;
			break;
		}
	have_pt_gnu_stack = (i < elf_ex.e_phnum);

	/* Some simple consistency checks for the interpreter */
	if (elf_interpreter) {
		interpreter_type = INTERPRETER_ELF | INTERPRETER_AOUT;

		/* Now figure out which format our binary is */
		if ((N_MAGIC(interp_ex) != OMAGIC) &&
		    (N_MAGIC(interp_ex) != ZMAGIC) &&
		    (N_MAGIC(interp_ex) != QMAGIC))
			interpreter_type = INTERPRETER_ELF;

		if (memcmp(interp_elf_ex.e_ident, ELFMAG, SELFMAG) != 0)
			interpreter_type &= ~INTERPRETER_ELF;

		retval = -ELIBBAD;
		if (!interpreter_type)
			goto out_free_dentry;

		/* Make sure only one type was selected */
		if ((interpreter_type & INTERPRETER_ELF) &&
		     interpreter_type != INTERPRETER_ELF) {
	     		// FIXME - ratelimit this before re-enabling
			// printk(KERN_WARNING "ELF: Ambiguous type, using ELF\n");
			interpreter_type = INTERPRETER_ELF;
		}
		/* Verify the interpreter has a valid arch */
		if ((interpreter_type == INTERPRETER_ELF) &&
		    !elf_check_arch(&interp_elf_ex))
			goto out_free_dentry;
	} else {
		/* Executables without an interpreter also need a personality  */
		SET_PERSONALITY(elf_ex, ibcs2_interpreter);
	}

	/* OK, we are done with that, now set up the arg stuff,
	   and then start this sucker up */

	if ((!bprm->sh_bang) && (interpreter_type == INTERPRETER_AOUT)) {
		char *passed_p = passed_fileno;
		sprintf(passed_fileno, "%d", elf_exec_fileno);

		if (elf_interpreter) {
			retval = copy_strings_kernel(1, &passed_p, bprm);
			if (retval)
				goto out_free_dentry; 
			bprm->argc++;
		}
	}

	/* Flush all traces of the currently running executable */
	retval = flush_old_exec(bprm);
	if (retval)
		goto out_free_dentry;

	/* Discard our unneeded old files struct */
	if (files) {
		steal_locks(files);
		put_files_struct(files);
		files = NULL;
	}

	/* OK, This is the point of no return */
	current->mm->start_data = 0;
	current->mm->end_data = 0;
	current->mm->end_code = 0;
	current->mm->mmap = NULL;
	current->flags &= ~PF_FORKNOEXEC;
	current->mm->def_flags = def_flags;

	/* Do this immediately, since STACK_TOP as used in setup_arg_pages
	   may depend on the personality.  */
	SET_PERSONALITY(elf_ex, ibcs2_interpreter);
	if (elf_read_implies_exec(elf_ex, have_pt_gnu_stack))
		current->personality |= READ_IMPLIES_EXEC;

	/* Do this so that we can load the interpreter, if need be.  We will
	   change some of these later */
	current->mm->rss = 0;
	current->mm->free_area_cache = TASK_UNMAPPED_BASE;
	retval = setup_arg_pages(bprm, executable_stack);
	if (retval < 0) {
		send_sig(SIGKILL, current, 0);
		goto out_free_dentry;
	}
	
	current->mm->start_stack = bprm->p;

	/* Now we do a little grungy work by mmaping the ELF image into
	   the correct location in memory.  At this point, we assume that
	   the image should be loaded at fixed address, not at a variable
	   address. */

	for(i = 0, elf_ppnt = elf_phdata; i < elf_ex.e_phnum; i++, elf_ppnt++) {
		int elf_prot = 0, elf_flags;
		unsigned long k, vaddr;

		if (elf_ppnt->p_type != PT_LOAD)
			continue;

		if (unlikely (elf_brk > elf_bss)) {
			unsigned long nbyte;
	            
			/* There was a PT_LOAD segment with p_memsz > p_filesz
			   before this one. Map anonymous pages, if needed,
			   and clear the area.  */
			retval = set_brk (elf_bss + load_bias,
					  elf_brk + load_bias);
			if (retval) {
				send_sig(SIGKILL, current, 0);
				goto out_free_dentry;
			}
			nbyte = ELF_PAGEOFFSET(elf_bss);
			if (nbyte) {
				nbyte = ELF_MIN_ALIGN - nbyte;
				if (nbyte > elf_brk - elf_bss)
					nbyte = elf_brk - elf_bss;
				clear_user((void __user *) elf_bss + load_bias, nbyte);
			}
		}

		if (elf_ppnt->p_flags & PF_R) elf_prot |= PROT_READ;
		if (elf_ppnt->p_flags & PF_W) elf_prot |= PROT_WRITE;
		if (elf_ppnt->p_flags & PF_X) elf_prot |= PROT_EXEC;

		elf_flags = MAP_PRIVATE|MAP_DENYWRITE|MAP_EXECUTABLE;

		vaddr = elf_ppnt->p_vaddr;
		if (elf_ex.e_type == ET_EXEC || load_addr_set) {
			elf_flags |= MAP_FIXED;
		} else if (elf_ex.e_type == ET_DYN) {
			/* Try and get dynamic programs out of the way of the default mmap
			   base, as well as whatever program they might try to exec.  This
			   is because the brk will follow the loader, and is not movable.  */
			load_bias = ELF_PAGESTART(ELF_ET_DYN_BASE - vaddr);
		}

		error = elf_map(bprm->file, load_bias + vaddr, elf_ppnt, elf_prot, elf_flags);
		if (BAD_ADDR(error))
			continue;

		if (!load_addr_set) {
			load_addr_set = 1;
			load_addr = (elf_ppnt->p_vaddr - elf_ppnt->p_offset);
			if (elf_ex.e_type == ET_DYN) {
				load_bias += error -
				             ELF_PAGESTART(load_bias + vaddr);
				load_addr += load_bias;
				reloc_func_desc = load_bias;
			}
		}
		k = elf_ppnt->p_vaddr;
		if (k < start_code) start_code = k;
		if (start_data < k) start_data = k;

		/*
		 * Check to see if the section's size will overflow the
		 * allowed task size. Note that p_filesz must always be
		 * <= p_memsz so it is only necessary to check p_memsz.
		 */
		if (k > TASK_SIZE || elf_ppnt->p_filesz > elf_ppnt->p_memsz ||
		    elf_ppnt->p_memsz > TASK_SIZE ||
		    TASK_SIZE - elf_ppnt->p_memsz < k) {
			/* set_brk can never work.  Avoid overflows.  */
			send_sig(SIGKILL, current, 0);
			goto out_free_dentry;
		}

		k = elf_ppnt->p_vaddr + elf_ppnt->p_filesz;

		if (k > elf_bss)
			elf_bss = k;
		if ((elf_ppnt->p_flags & PF_X) && end_code < k)
			end_code = k;
		if (end_data < k)
			end_data = k;
		k = elf_ppnt->p_vaddr + elf_ppnt->p_memsz;
		if (k > elf_brk)
			elf_brk = k;
	}

	elf_ex.e_entry += load_bias;
	elf_bss += load_bias;
	elf_brk += load_bias;
	start_code += load_bias;
	end_code += load_bias;
	start_data += load_bias;
	end_data += load_bias;

	/* Calling set_brk effectively mmaps the pages that we need
	 * for the bss and break sections.  We must do this before
	 * mapping in the interpreter, to make sure it doesn't wind
	 * up getting placed where the bss needs to go.
	 */
	retval = set_brk(elf_bss, elf_brk);
	if (retval) {
		send_sig(SIGKILL, current, 0);
		goto out_free_dentry;
	}
	padzero(elf_bss);

	if (elf_interpreter) {
		if (interpreter_type == INTERPRETER_AOUT)
			elf_entry = load_aout_interp(&interp_ex,
						     interpreter);
		else
			elf_entry = load_elf_interp(&interp_elf_ex,
						    interpreter,
						    &interp_load_addr);
		if (BAD_ADDR(elf_entry)) {
			printk(KERN_ERR "Unable to load interpreter\n");
			send_sig(SIGSEGV, current, 0);
			retval = -ENOEXEC; /* Nobody gets to see this, but.. */
			goto out_free_dentry;
		}
		reloc_func_desc = interp_load_addr;

		allow_write_access(interpreter);
		fput(interpreter);
		kfree(elf_interpreter);
	} else {
		elf_entry = elf_ex.e_entry;
	}

	kfree(elf_phdata);

	if (interpreter_type != INTERPRETER_AOUT)
		sys_close(elf_exec_fileno);

	set_binfmt(&elf_format);

	compute_creds(bprm);
	current->flags &= ~PF_FORKNOEXEC;
	create_elf_tables(bprm, &elf_ex, (interpreter_type == INTERPRETER_AOUT),
			load_addr, interp_load_addr);
	/* N.B. passed_fileno might not be initialized? */
	if (interpreter_type == INTERPRETER_AOUT)
		current->mm->arg_start += strlen(passed_fileno) + 1;
	current->mm->end_code = end_code;
	current->mm->start_code = start_code;
	current->mm->start_data = start_data;
	current->mm->end_data = end_data;
	current->mm->start_stack = bprm->p;

	if (current->personality & MMAP_PAGE_ZERO) {
		/* Why this, you ask???  Well SVr4 maps page 0 as read-only,
		   and some applications "depend" upon this behavior.
		   Since we do not have the power to recompile these, we
		   emulate the SVr4 behavior.  Sigh.  */
		down_write(&current->mm->mmap_sem);
		error = do_mmap(NULL, 0, PAGE_SIZE, PROT_READ | PROT_EXEC,
				MAP_FIXED | MAP_PRIVATE, 0);
		up_write(&current->mm->mmap_sem);
	}

#ifdef ELF_PLAT_INIT
	/*
	 * The ABI may specify that certain registers be set up in special
	 * ways (on i386 %edx is the address of a DT_FINI function, for
	 * example.  In addition, it may also specify (eg, PowerPC64 ELF)
	 * that the e_entry field is the address of the function descriptor
	 * for the startup routine, rather than the address of the startup
	 * routine itself.  This macro performs whatever initialization to
	 * the regs structure is required as well as any relocations to the
	 * function descriptor entries when executing dynamically links apps.
	 */
	ELF_PLAT_INIT(regs, reloc_func_desc);
#endif

	start_thread(regs, elf_entry, bprm->p);
	if (unlikely(current->ptrace & PT_PTRACED)) {
		if (current->ptrace & PT_TRACE_EXEC)
			ptrace_notify ((PTRACE_EVENT_EXEC << 8) | SIGTRAP);
		else
			send_sig(SIGTRAP, current, 0);
	}
	retval = 0;
out:
	return retval;

	/* error cleanup */
out_free_dentry:
	allow_write_access(interpreter);
	if (interpreter)
		fput(interpreter);
out_free_interp:
	if (elf_interpreter)
		kfree(elf_interpreter);
out_free_file:
	sys_close(elf_exec_fileno);
out_free_fh:
	if (files) {
		put_files_struct(current->files);
		current->files = files;
	}
out_free_ph:
	kfree(elf_phdata);
	goto out;
}

/* This is really simpleminded and specialized - we are loading an
   a.out library that is given an ELF header. */

static int load_elf_library(struct file *file)
{
	struct elf_phdr *elf_phdata;
	unsigned long elf_bss, bss, len;
	int retval, error, i, j;
	struct elfhdr elf_ex;

	error = -ENOEXEC;
	retval = kernel_read(file, 0, (char *) &elf_ex, sizeof(elf_ex));
	if (retval != sizeof(elf_ex))
		goto out;

	if (memcmp(elf_ex.e_ident, ELFMAG, SELFMAG) != 0)
		goto out;

	/* First of all, some simple consistency checks */
	if (elf_ex.e_type != ET_EXEC || elf_ex.e_phnum > 2 ||
	   !elf_check_arch(&elf_ex) || !file->f_op || !file->f_op->mmap)
		goto out;

	/* Now read in all of the header information */

	j = sizeof(struct elf_phdr) * elf_ex.e_phnum;
	/* j < ELF_MIN_ALIGN because elf_ex.e_phnum <= 2 */

	error = -ENOMEM;
	elf_phdata = (struct elf_phdr *) kmalloc(j, GFP_KERNEL);
	if (!elf_phdata)
		goto out;

	error = -ENOEXEC;
	retval = kernel_read(file, elf_ex.e_phoff, (char *) elf_phdata, j);
	if (retval != j)
		goto out_free_ph;

	for (j = 0, i = 0; i<elf_ex.e_phnum; i++)
		if ((elf_phdata + i)->p_type == PT_LOAD) j++;
	if (j != 1)
		goto out_free_ph;

	while (elf_phdata->p_type != PT_LOAD) elf_phdata++;

	/* Now use mmap to map the library into memory. */
	down_write(&current->mm->mmap_sem);
	error = do_mmap(file,
			ELF_PAGESTART(elf_phdata->p_vaddr),
			(elf_phdata->p_filesz +
			 ELF_PAGEOFFSET(elf_phdata->p_vaddr)),
			PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_FIXED | MAP_PRIVATE | MAP_DENYWRITE,
			(elf_phdata->p_offset -
			 ELF_PAGEOFFSET(elf_phdata->p_vaddr)));
	up_write(&current->mm->mmap_sem);
	if (error != ELF_PAGESTART(elf_phdata->p_vaddr))
		goto out_free_ph;

	elf_bss = elf_phdata->p_vaddr + elf_phdata->p_filesz;
	padzero(elf_bss);

	len = ELF_PAGESTART(elf_phdata->p_filesz + elf_phdata->p_vaddr + ELF_MIN_ALIGN - 1);
	bss = elf_phdata->p_memsz + elf_phdata->p_vaddr;
	if (bss > len)
		do_brk(len, bss - len);
	error = 0;

out_free_ph:
	kfree(elf_phdata);
out:
	return error;
}

/*
 * Note that some platforms still use traditional core dumps and not
 * the ELF core dump.  Each platform can select it as appropriate.
 */
#ifdef USE_ELF_CORE_DUMP

/*
 * ELF core dumper
 *
 * Modelled on fs/exec.c:aout_core_dump()
 * Jeremy Fitzhardinge <jeremy@sw.oz.au>
 */
/*
 * These are the only things you should do on a core-file: use only these
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

/*
 * Decide whether a segment is worth dumping; default is yes to be
 * sure (missing info is worse than too much; etc).
 * Personally I'd include everything, and use the coredump limit...
 *
 * I think we should skip something. But I am not sure how. H.J.
 */
static int maydump(struct vm_area_struct *vma)
{
	/*
	 * If we may not read the contents, don't allow us to dump
	 * them either. "dump_write()" can't handle it anyway.
	 */
	if (!(vma->vm_flags & VM_READ))
		return 0;

	/* Do not dump I/O mapped devices! -DaveM */
	if (vma->vm_flags & VM_IO)
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

/* An ELF note in memory */
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
	sz += roundup(strlen(en->name) + 1, 4);
	sz += roundup(en->datasz, 4);

	return sz;
}

#define DUMP_WRITE(addr, nr)	\
	do { if (!dump_write(file, (addr), (nr))) return 0; } while(0)
#define DUMP_SEEK(off)	\
	do { if (!dump_seek(file, (off))) return 0; } while(0)

static int writenote(struct memelfnote *men, struct file *file)
{
	struct elf_note en;

	en.n_namesz = strlen(men->name) + 1;
	en.n_descsz = men->datasz;
	en.n_type = men->type;

	DUMP_WRITE(&en, sizeof(en));
	DUMP_WRITE(men->name, en.n_namesz);
	/* XXX - cast from long long to long to avoid need for libgcc.a */
	DUMP_SEEK(roundup((unsigned long)file->f_pos, 4));	/* XXX */
	DUMP_WRITE(men->data, men->datasz);
	DUMP_SEEK(roundup((unsigned long)file->f_pos, 4));	/* XXX */

	return 1;
}
#undef DUMP_WRITE
#undef DUMP_SEEK

#define DUMP_WRITE(addr, nr)	\
	if ((size += (nr)) > limit || !dump_write(file, (addr), (nr))) \
		goto end_coredump;
#define DUMP_SEEK(off)	\
	if (!dump_seek(file, (off))) \
		goto end_coredump;

static inline void fill_elf_header(struct elfhdr *elf, int segs)
{
	memcpy(elf->e_ident, ELFMAG, SELFMAG);
	elf->e_ident[EI_CLASS] = ELF_CLASS;
	elf->e_ident[EI_DATA] = ELF_DATA;
	elf->e_ident[EI_VERSION] = EV_CURRENT;
	elf->e_ident[EI_OSABI] = ELF_OSABI;
	memset(elf->e_ident+EI_PAD, 0, EI_NIDENT-EI_PAD);

	elf->e_type = ET_CORE;
	elf->e_machine = ELF_ARCH;
	elf->e_version = EV_CURRENT;
	elf->e_entry = 0;
	elf->e_phoff = sizeof(struct elfhdr);
	elf->e_shoff = 0;
	elf->e_flags = 0;
	elf->e_ehsize = sizeof(struct elfhdr);
	elf->e_phentsize = sizeof(struct elf_phdr);
	elf->e_phnum = segs;
	elf->e_shentsize = 0;
	elf->e_shnum = 0;
	elf->e_shstrndx = 0;
	return;
}

static inline void fill_elf_note_phdr(struct elf_phdr *phdr, int sz, off_t offset)
{
	phdr->p_type = PT_NOTE;
	phdr->p_offset = offset;
	phdr->p_vaddr = 0;
	phdr->p_paddr = 0;
	phdr->p_filesz = sz;
	phdr->p_memsz = 0;
	phdr->p_flags = 0;
	phdr->p_align = 0;
	return;
}

static void fill_note(struct memelfnote *note, const char *name, int type, 
		unsigned int sz, void *data)
{
	note->name = name;
	note->type = type;
	note->datasz = sz;
	note->data = data;
	return;
}

/*
 * fill up all the fields in prstatus from the given task struct, except registers
 * which need to be filled up separately.
 */
static void fill_prstatus(struct elf_prstatus *prstatus,
			struct task_struct *p, long signr) 
{
	prstatus->pr_info.si_signo = prstatus->pr_cursig = signr;
	prstatus->pr_sigpend = p->pending.signal.sig[0];
	prstatus->pr_sighold = p->blocked.sig[0];
	prstatus->pr_pid = p->pid;
	prstatus->pr_ppid = p->parent->pid;
	prstatus->pr_pgrp = process_group(p);
	prstatus->pr_sid = p->signal->session;
	jiffies_to_timeval(p->utime, &prstatus->pr_utime);
	jiffies_to_timeval(p->stime, &prstatus->pr_stime);
	jiffies_to_timeval(p->cutime, &prstatus->pr_cutime);
	jiffies_to_timeval(p->cstime, &prstatus->pr_cstime);
}

static void fill_psinfo(struct elf_prpsinfo *psinfo, struct task_struct *p,
			struct mm_struct *mm)
{
	int i, len;
	
	/* first copy the parameters from user space */
	memset(psinfo, 0, sizeof(struct elf_prpsinfo));

	len = mm->arg_end - mm->arg_start;
	if (len >= ELF_PRARGSZ)
		len = ELF_PRARGSZ-1;
	copy_from_user(&psinfo->pr_psargs,
		       (const char __user *)mm->arg_start, len);
	for(i = 0; i < len; i++)
		if (psinfo->pr_psargs[i] == 0)
			psinfo->pr_psargs[i] = ' ';
	psinfo->pr_psargs[len] = 0;

	psinfo->pr_pid = p->pid;
	psinfo->pr_ppid = p->parent->pid;
	psinfo->pr_pgrp = process_group(p);
	psinfo->pr_sid = p->signal->session;

	i = p->state ? ffz(~p->state) + 1 : 0;
	psinfo->pr_state = i;
	psinfo->pr_sname = (i < 0 || i > 5) ? '.' : "RSDTZW"[i];
	psinfo->pr_zomb = psinfo->pr_sname == 'Z';
	psinfo->pr_nice = task_nice(p);
	psinfo->pr_flag = p->flags;
	SET_UID(psinfo->pr_uid, p->uid);
	SET_GID(psinfo->pr_gid, p->gid);
	strncpy(psinfo->pr_fname, p->comm, sizeof(psinfo->pr_fname));
	
	return;
}

/* Here is the structure in which status of each thread is captured. */
struct elf_thread_status
{
	struct list_head list;
	struct elf_prstatus prstatus;	/* NT_PRSTATUS */
	elf_fpregset_t fpu;		/* NT_PRFPREG */
#ifdef ELF_CORE_COPY_XFPREGS
	elf_fpxregset_t xfpu;		/* NT_PRXFPREG */
#endif
	struct memelfnote notes[3];
	int num_notes;
};

/*
 * In order to add the specific thread information for the elf file format,
 * we need to keep a linked list of every threads pr_status and then
 * create a single section for them in the final core file.
 */
static int elf_dump_thread_status(long signr, struct task_struct * p, struct list_head * thread_list)
{

	struct elf_thread_status *t;
	int sz = 0;

	t = kmalloc(sizeof(*t), GFP_ATOMIC);
	if (!t)
		return 0;
	memset(t, 0, sizeof(*t));

	INIT_LIST_HEAD(&t->list);
	t->num_notes = 0;

	fill_prstatus(&t->prstatus, p, signr);
	elf_core_copy_task_regs(p, &t->prstatus.pr_reg);	
	
	fill_note(&t->notes[0], "CORE", NT_PRSTATUS, sizeof(t->prstatus), &(t->prstatus));
	t->num_notes++;
	sz += notesize(&t->notes[0]);

	if ((t->prstatus.pr_fpvalid = elf_core_copy_task_fpregs(p, NULL, &t->fpu))) {
		fill_note(&t->notes[1], "CORE", NT_PRFPREG, sizeof(t->fpu), &(t->fpu));
		t->num_notes++;
		sz += notesize(&t->notes[1]);
	}

#ifdef ELF_CORE_COPY_XFPREGS
	if (elf_core_copy_task_xfpregs(p, &t->xfpu)) {
		fill_note(&t->notes[2], "LINUX", NT_PRXFPREG, sizeof(t->xfpu), &t->xfpu);
		t->num_notes++;
		sz += notesize(&t->notes[2]);
	}
#endif	
	list_add(&t->list, thread_list);
	return sz;
}

/*
 * Actual dumper
 *
 * This is a two-pass process; first we find the offsets of the bits,
 * and then they are actually written out.  If we run out of core limit
 * we just truncate.
 */
static int elf_core_dump(long signr, struct pt_regs * regs, struct file * file)
{
#define	NUM_NOTES	6
	int has_dumped = 0;
	mm_segment_t fs;
	int segs;
	size_t size = 0;
	int i;
	struct vm_area_struct *vma;
	struct elfhdr *elf = NULL;
	off_t offset = 0, dataoff;
	unsigned long limit = current->rlim[RLIMIT_CORE].rlim_cur;
	int numnote;
	struct memelfnote *notes = NULL;
	struct elf_prstatus *prstatus = NULL;	/* NT_PRSTATUS */
	struct elf_prpsinfo *psinfo = NULL;	/* NT_PRPSINFO */
 	struct task_struct *g, *p;
 	LIST_HEAD(thread_list);
 	struct list_head *t;
	elf_fpregset_t *fpu = NULL;
#ifdef ELF_CORE_COPY_XFPREGS
	elf_fpxregset_t *xfpu = NULL;
#endif
	int thread_status_size = 0;
	elf_addr_t *auxv;

	/*
	 * We no longer stop all VM operations.
	 * 
	 * This is because those proceses that could possibly change map_count or
	 * the mmap / vma pages are now blocked in do_exit on current finishing
	 * this core dump.
	 *
	 * Only ptrace can touch these memory addresses, but it doesn't change
	 * the map_count or the pages allocated.  So no possibility of crashing
	 * exists while dumping the mm->vm_next areas to the core file.
	 */
  
	/* alloc memory for large data structures: too large to be on stack */
	elf = kmalloc(sizeof(*elf), GFP_KERNEL);
	if (!elf)
		goto cleanup;
	prstatus = kmalloc(sizeof(*prstatus), GFP_KERNEL);
	if (!prstatus)
		goto cleanup;
	psinfo = kmalloc(sizeof(*psinfo), GFP_KERNEL);
	if (!psinfo)
		goto cleanup;
	notes = kmalloc(NUM_NOTES * sizeof(struct memelfnote), GFP_KERNEL);
	if (!notes)
		goto cleanup;
	fpu = kmalloc(sizeof(*fpu), GFP_KERNEL);
	if (!fpu)
		goto cleanup;
#ifdef ELF_CORE_COPY_XFPREGS
	xfpu = kmalloc(sizeof(*xfpu), GFP_KERNEL);
	if (!xfpu)
		goto cleanup;
#endif

	/* capture the status of all other threads */
	if (signr) {
		read_lock(&tasklist_lock);
		do_each_thread(g,p)
			if (current->mm == p->mm && current != p) {
				int sz = elf_dump_thread_status(signr, p, &thread_list);
				if (!sz) {
					read_unlock(&tasklist_lock);
					goto cleanup;
				} else
					thread_status_size += sz;
			}
		while_each_thread(g,p);
		read_unlock(&tasklist_lock);
	}

	/* now collect the dump for the current */
	memset(prstatus, 0, sizeof(*prstatus));
	fill_prstatus(prstatus, current, signr);
	elf_core_copy_regs(&prstatus->pr_reg, regs);
	
	segs = current->mm->map_count;
#ifdef ELF_CORE_EXTRA_PHDRS
	segs += ELF_CORE_EXTRA_PHDRS;
#endif

	/* Set up header */
	fill_elf_header(elf, segs+1);	/* including notes section */

	has_dumped = 1;
	current->flags |= PF_DUMPCORE;

	/*
	 * Set up the notes in similar form to SVR4 core dumps made
	 * with info from their /proc.
	 */

	fill_note(notes +0, "CORE", NT_PRSTATUS, sizeof(*prstatus), prstatus);
	
	fill_psinfo(psinfo, current->group_leader, current->mm);
	fill_note(notes +1, "CORE", NT_PRPSINFO, sizeof(*psinfo), psinfo);
	
	fill_note(notes +2, "CORE", NT_TASKSTRUCT, sizeof(*current), current);
  
	numnote = 3;

	auxv = (elf_addr_t *) current->mm->saved_auxv;

	i = 0;
	do
		i += 2;
	while (auxv[i - 2] != AT_NULL);
	fill_note(&notes[numnote++], "CORE", NT_AUXV,
		  i * sizeof (elf_addr_t), auxv);

  	/* Try to dump the FPU. */
	if ((prstatus->pr_fpvalid = elf_core_copy_task_fpregs(current, regs, fpu)))
		fill_note(notes + numnote++,
			  "CORE", NT_PRFPREG, sizeof(*fpu), fpu);
#ifdef ELF_CORE_COPY_XFPREGS
	if (elf_core_copy_task_xfpregs(current, xfpu))
		fill_note(notes + numnote++,
			  "LINUX", NT_PRXFPREG, sizeof(*xfpu), xfpu);
#endif	
  
	fs = get_fs();
	set_fs(KERNEL_DS);

	DUMP_WRITE(elf, sizeof(*elf));
	offset += sizeof(*elf);				/* Elf header */
	offset += (segs+1) * sizeof(struct elf_phdr);	/* Program headers */

	/* Write notes phdr entry */
	{
		struct elf_phdr phdr;
		int sz = 0;

		for (i = 0; i < numnote; i++)
			sz += notesize(notes + i);
		
		sz += thread_status_size;

		fill_elf_note_phdr(&phdr, sz, offset);
		offset += sz;
		DUMP_WRITE(&phdr, sizeof(phdr));
	}

	/* Page-align dumped data */
	dataoff = offset = roundup(offset, ELF_EXEC_PAGESIZE);

	/* Write program headers for segments dump */
	for (vma = current->mm->mmap; vma != NULL; vma = vma->vm_next) {
		struct elf_phdr phdr;
		size_t sz;

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
		phdr.p_align = ELF_EXEC_PAGESIZE;

		DUMP_WRITE(&phdr, sizeof(phdr));
	}

#ifdef ELF_CORE_WRITE_EXTRA_PHDRS
	ELF_CORE_WRITE_EXTRA_PHDRS;
#endif

 	/* write out the notes section */
	for (i = 0; i < numnote; i++)
		if (!writenote(notes + i, file))
			goto end_coredump;

	/* write out the thread status notes section */
	list_for_each(t, &thread_list) {
		struct elf_thread_status *tmp = list_entry(t, struct elf_thread_status, list);
		for (i = 0; i < tmp->num_notes; i++)
			if (!writenote(&tmp->notes[i], file))
				goto end_coredump;
	}
 
	DUMP_SEEK(dataoff);

	for (vma = current->mm->mmap; vma != NULL; vma = vma->vm_next) {
		unsigned long addr;

		if (!maydump(vma))
			continue;

		for (addr = vma->vm_start;
		     addr < vma->vm_end;
		     addr += PAGE_SIZE) {
			struct page* page;
			struct vm_area_struct *vma;

			if (get_user_pages(current, current->mm, addr, 1, 0, 1,
						&page, &vma) <= 0) {
				DUMP_SEEK (file->f_pos + PAGE_SIZE);
			} else {
				if (page == ZERO_PAGE(addr)) {
					DUMP_SEEK (file->f_pos + PAGE_SIZE);
				} else {
					void *kaddr;
					flush_cache_page(vma, addr);
					kaddr = kmap(page);
					if ((size += PAGE_SIZE) > limit ||
					    !dump_write(file, kaddr,
					    PAGE_SIZE)) {
						kunmap(page);
						page_cache_release(page);
						goto end_coredump;
					}
					kunmap(page);
				}
				page_cache_release(page);
			}
		}
	}

#ifdef ELF_CORE_WRITE_EXTRA_DATA
	ELF_CORE_WRITE_EXTRA_DATA;
#endif

	if ((off_t) file->f_pos != offset) {
		/* Sanity check */
		printk("elf_core_dump: file->f_pos (%ld) != offset (%ld)\n",
		       (off_t) file->f_pos, offset);
	}

end_coredump:
	set_fs(fs);

cleanup:
	while(!list_empty(&thread_list)) {
		struct list_head *tmp = thread_list.next;
		list_del(tmp);
		kfree(list_entry(tmp, struct elf_thread_status, list));
	}

	kfree(elf);
	kfree(prstatus);
	kfree(psinfo);
	kfree(notes);
	kfree(fpu);
#ifdef ELF_CORE_COPY_XFPREGS
	kfree(xfpu);
#endif
	return has_dumped;
#undef NUM_NOTES
}

#endif		/* USE_ELF_CORE_DUMP */

static int __init init_elf_binfmt(void)
{
	return register_binfmt(&elf_format);
}

static void __exit exit_elf_binfmt(void)
{
	/* Remove the COFF and ELF loaders. */
	unregister_binfmt(&elf_format);
}

core_initcall(init_elf_binfmt);
module_exit(exit_elf_binfmt);
MODULE_LICENSE("GPL");
