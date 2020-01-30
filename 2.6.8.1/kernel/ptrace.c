/*
 * linux/kernel/ptrace.c
 *
 * (C) Copyright 1999 Linus Torvalds
 *
 * Common interfaces for "ptrace()" which we do not want
 * to continually duplicate across every architecture.
 */

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/smp_lock.h>
#include <linux/ptrace.h>
#include <linux/security.h>

#include <asm/pgtable.h>
#include <asm/uaccess.h>

/*
 * ptrace a task: make the debugger its new parent and
 * move it to the ptrace list.
 *
 * Must be called with the tasklist lock write-held.
 */
void __ptrace_link(task_t *child, task_t *new_parent)
{
	if (!list_empty(&child->ptrace_list))
		BUG();
	if (child->parent == new_parent)
		return;
	list_add(&child->ptrace_list, &child->parent->ptrace_children);
	REMOVE_LINKS(child);
	child->parent = new_parent;
	SET_LINKS(child);
}
 
/*
 * unptrace a task: move it back to its original parent and
 * remove it from the ptrace list.
 *
 * Must be called with the tasklist lock write-held.
 */
void __ptrace_unlink(task_t *child)
{
	if (!child->ptrace)
		BUG();
	child->ptrace = 0;
	if (list_empty(&child->ptrace_list))
		return;
	list_del_init(&child->ptrace_list);
	REMOVE_LINKS(child);
	child->parent = child->real_parent;
	SET_LINKS(child);
}

/*
 * Check that we have indeed attached to the thing..
 */
int ptrace_check_attach(struct task_struct *child, int kill)
{
	if (!(child->ptrace & PT_PTRACED))
		return -ESRCH;

	if (child->parent != current)
		return -ESRCH;

	if (!kill) {
		if (child->state != TASK_STOPPED)
			return -ESRCH;
		wait_task_inactive(child);
	}

	/* All systems go.. */
	return 0;
}

int ptrace_attach(struct task_struct *task)
{
	int retval;
	task_lock(task);
	retval = -EPERM;
	if (task->pid <= 1)
		goto bad;
	if (task == current)
		goto bad;
	if (!task->mm)
		goto bad;
	if(((current->uid != task->euid) ||
	    (current->uid != task->suid) ||
	    (current->uid != task->uid) ||
 	    (current->gid != task->egid) ||
 	    (current->gid != task->sgid) ||
 	    (current->gid != task->gid)) && !capable(CAP_SYS_PTRACE))
		goto bad;
	rmb();
	if (!task->mm->dumpable && !capable(CAP_SYS_PTRACE))
		goto bad;
	/* the same process cannot be attached many times */
	if (task->ptrace & PT_PTRACED)
		goto bad;
	retval = security_ptrace(current, task);
	if (retval)
		goto bad;

	/* Go */
	task->ptrace |= PT_PTRACED;
	if (capable(CAP_SYS_PTRACE))
		task->ptrace |= PT_PTRACE_CAP;
	task_unlock(task);

	write_lock_irq(&tasklist_lock);
	__ptrace_link(task, current);
	write_unlock_irq(&tasklist_lock);

	force_sig_specific(SIGSTOP, task);
	return 0;

bad:
	task_unlock(task);
	return retval;
}

int ptrace_detach(struct task_struct *child, unsigned int data)
{
	if ((unsigned long) data > _NSIG)
		return	-EIO;

	/* Architecture-specific hardware disable .. */
	ptrace_disable(child);

	/* .. re-parent .. */
	child->exit_code = data;

	write_lock_irq(&tasklist_lock);
	__ptrace_unlink(child);
	/* .. and wake it up. */
	if (child->state != TASK_ZOMBIE)
		wake_up_process(child);
	write_unlock_irq(&tasklist_lock);

	return 0;
}

/*
 * Access another process' address space.
 * Source/target buffer must be kernel space, 
 * Do not walk the page table directly, use get_user_pages
 */

int access_process_vm(struct task_struct *tsk, unsigned long addr, void *buf, int len, int write)
{
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	struct page *page;
	void *old_buf = buf;

	mm = get_task_mm(tsk);
	if (!mm)
		return 0;

	down_read(&mm->mmap_sem);
	/* ignore errors, just check how much was sucessfully transfered */
	while (len) {
		int bytes, ret, offset;
		void *maddr;

		ret = get_user_pages(tsk, mm, addr, 1,
				write, 1, &page, &vma);
		if (ret <= 0)
			break;

		bytes = len;
		offset = addr & (PAGE_SIZE-1);
		if (bytes > PAGE_SIZE-offset)
			bytes = PAGE_SIZE-offset;

		flush_cache_page(vma, addr);

		maddr = kmap(page);
		if (write) {
			copy_to_user_page(vma, page, addr,
					  maddr + offset, buf, bytes);
			set_page_dirty_lock(page);
		} else {
			copy_from_user_page(vma, page, addr,
					    buf, maddr + offset, bytes);
		}
		kunmap(page);
		page_cache_release(page);
		len -= bytes;
		buf += bytes;
		addr += bytes;
	}
	up_read(&mm->mmap_sem);
	mmput(mm);
	
	return buf - old_buf;
}

int ptrace_readdata(struct task_struct *tsk, unsigned long src, char __user *dst, int len)
{
	int copied = 0;

	while (len > 0) {
		char buf[128];
		int this_len, retval;

		this_len = (len > sizeof(buf)) ? sizeof(buf) : len;
		retval = access_process_vm(tsk, src, buf, this_len, 0);
		if (!retval) {
			if (copied)
				break;
			return -EIO;
		}
		if (copy_to_user(dst, buf, retval))
			return -EFAULT;
		copied += retval;
		src += retval;
		dst += retval;
		len -= retval;			
	}
	return copied;
}

int ptrace_writedata(struct task_struct *tsk, char __user *src, unsigned long dst, int len)
{
	int copied = 0;

	while (len > 0) {
		char buf[128];
		int this_len, retval;

		this_len = (len > sizeof(buf)) ? sizeof(buf) : len;
		if (copy_from_user(buf, src, this_len))
			return -EFAULT;
		retval = access_process_vm(tsk, dst, buf, this_len, 1);
		if (!retval) {
			if (copied)
				break;
			return -EIO;
		}
		copied += retval;
		src += retval;
		dst += retval;
		len -= retval;			
	}
	return copied;
}

static int ptrace_setoptions(struct task_struct *child, long data)
{
	child->ptrace &= ~PT_TRACE_MASK;

	if (data & PTRACE_O_TRACESYSGOOD)
		child->ptrace |= PT_TRACESYSGOOD;

	if (data & PTRACE_O_TRACEFORK)
		child->ptrace |= PT_TRACE_FORK;

	if (data & PTRACE_O_TRACEVFORK)
		child->ptrace |= PT_TRACE_VFORK;

	if (data & PTRACE_O_TRACECLONE)
		child->ptrace |= PT_TRACE_CLONE;

	if (data & PTRACE_O_TRACEEXEC)
		child->ptrace |= PT_TRACE_EXEC;

	if (data & PTRACE_O_TRACEVFORKDONE)
		child->ptrace |= PT_TRACE_VFORK_DONE;

	if (data & PTRACE_O_TRACEEXIT)
		child->ptrace |= PT_TRACE_EXIT;

	return (data & ~PTRACE_O_MASK) ? -EINVAL : 0;
}

static int ptrace_getsiginfo(struct task_struct *child, siginfo_t __user * data)
{
	if (child->last_siginfo == NULL)
		return -EINVAL;
	return copy_siginfo_to_user(data, child->last_siginfo);
}

static int ptrace_setsiginfo(struct task_struct *child, siginfo_t __user * data)
{
	if (child->last_siginfo == NULL)
		return -EINVAL;
	if (copy_from_user(child->last_siginfo, data, sizeof (siginfo_t)) != 0)
		return -EFAULT;
	return 0;
}

int ptrace_request(struct task_struct *child, long request,
		   long addr, long data)
{
	int ret = -EIO;

	switch (request) {
#ifdef PTRACE_OLDSETOPTIONS
	case PTRACE_OLDSETOPTIONS:
#endif
	case PTRACE_SETOPTIONS:
		ret = ptrace_setoptions(child, data);
		break;
	case PTRACE_GETEVENTMSG:
		ret = put_user(child->ptrace_message, (unsigned long __user *) data);
		break;
	case PTRACE_GETSIGINFO:
		ret = ptrace_getsiginfo(child, (siginfo_t __user *) data);
		break;
	case PTRACE_SETSIGINFO:
		ret = ptrace_setsiginfo(child, (siginfo_t __user *) data);
		break;
	default:
		break;
	}

	return ret;
}

void ptrace_notify(int exit_code)
{
	BUG_ON (!(current->ptrace & PT_PTRACED));

	/* Let the debugger run.  */
	current->exit_code = exit_code;
	set_current_state(TASK_STOPPED);
	notify_parent(current, SIGCHLD);
	schedule();

	/*
	 * Signals sent while we were stopped might set TIF_SIGPENDING.
	 */

	spin_lock_irq(&current->sighand->siglock);
	recalc_sigpending();
	spin_unlock_irq(&current->sighand->siglock);
}

EXPORT_SYMBOL(ptrace_notify);
