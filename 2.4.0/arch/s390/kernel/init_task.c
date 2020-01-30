/*
 *  arch/s390/kernel/init_task.c
 *
 *  S390 version
 *
 *  Derived from "arch/i386/kernel/init_task.c"
 */

#include <linux/mm.h>
#include <linux/sched.h>

#include <asm/uaccess.h>
#include <asm/pgtable.h>

static struct vm_area_struct init_mmap = INIT_MMAP;
static struct fs_struct init_fs = INIT_FS;
static struct files_struct init_files = INIT_FILES;
static struct signal_struct init_signals = INIT_SIGNALS;
struct mm_struct init_mm = INIT_MM(init_mm);

/*
 * Initial task structure.
 *
 * We need to make sure that this is 8192-byte aligned due to the
 * way process stacks are handled. This is done by making sure
 * the linker maps this in the .text segment right after head.S,
 * and making head.S ensure the proper alignment.
 *
 * The things we do for performance..
 */
union task_union init_task_union __attribute__((aligned(8192))) = 
  { INIT_TASK(init_task_union.task) };
