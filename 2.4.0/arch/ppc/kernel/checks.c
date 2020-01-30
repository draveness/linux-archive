#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/smp_lock.h>
#include <linux/stddef.h>
#include <linux/unistd.h>
#include <linux/ptrace.h>
#include <linux/malloc.h>
#include <linux/user.h>
#include <linux/a.out.h>

#include <asm/pgtable.h>
#include <asm/uaccess.h>
#include <asm/system.h>
#include <asm/io.h>

/*
 * Do various before compile checks of data structures
 * -- Cort
 */
int main(void)
{
	int ret = 0;
#if 0
	if ( sizeof(struct thread_struct) % 16 )
	{
		printf("Thread struct is not modulo 16 bytes: "
			"%d bytes total, %d bytes off\n",
			sizeof(struct thread_struct),
			sizeof(struct thread_struct)%16);
		ret = -1;
	}
#endif	

	if ( sizeof(struct pt_regs) % 16 )
	{
		printf("pt_regs struct is not modulo 16 bytes: "
			"%d bytes total, %d bytes off\n",
			sizeof(struct pt_regs),
			sizeof(struct pt_regs)%16);
		ret = -1;
		
	}

	printf("Task size        : %d bytes\n"
	       "Tss size         : %d bytes\n"
	       "pt_regs size     : %d bytes\n"
	       "Kernel stack size: %d bytes\n",
	       sizeof(struct task_struct), sizeof(struct thread_struct),
	       sizeof(struct pt_regs),
	       sizeof(union task_union) - sizeof(struct task_struct));
	return ret;
}
