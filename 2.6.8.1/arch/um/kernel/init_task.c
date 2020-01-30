/* 
 * Copyright (C) 2000 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include "linux/config.h"
#include "linux/mm.h"
#include "linux/module.h"
#include "linux/sched.h"
#include "linux/init_task.h"
#include "linux/version.h"
#include "linux/mqueue.h"
#include "asm/uaccess.h"
#include "asm/pgtable.h"
#include "user_util.h"
#include "mem_user.h"

static struct fs_struct init_fs = INIT_FS;
struct mm_struct init_mm = INIT_MM(init_mm);
static struct files_struct init_files = INIT_FILES;
static struct signal_struct init_signals = INIT_SIGNALS(init_signals);

EXPORT_SYMBOL(init_mm);

/*
 * Initial task structure.
 *
 * All other task structs will be allocated on slabs in fork.c
 */

struct task_struct init_task = INIT_TASK(init_task);

EXPORT_SYMBOL(init_task);

/*
 * Initial thread structure.
 *
 * We need to make sure that this is 16384-byte aligned due to the
 * way process stacks are handled. This is done by having a special
 * "init_task" linker map entry..
 */

union thread_union init_thread_union 
__attribute__((__section__(".data.init_task"))) = 
{ INIT_THREAD_INFO(init_task) };

struct task_struct *alloc_task_struct(void)
{
	return((struct task_struct *) 
	       __get_free_pages(GFP_KERNEL, CONFIG_KERNEL_STACK_ORDER));
}

void unprotect_stack(unsigned long stack)
{
	protect_memory(stack, (1 << CONFIG_KERNEL_STACK_ORDER) * PAGE_SIZE, 
		       1, 1, 0, 1);
}

void free_task_struct(struct task_struct *task)
{
	/* free_pages decrements the page counter and only actually frees
	 * the pages if they are now not accessed by anything.
	 */
	free_pages((unsigned long) task, CONFIG_KERNEL_STACK_ORDER);
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
