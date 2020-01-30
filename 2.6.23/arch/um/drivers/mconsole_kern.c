/*
 * Copyright (C) 2001 Lennert Buytenhek (buytenh@gnu.org)
 * Copyright (C) 2001 - 2003 Jeff Dike (jdike@addtoit.com)
 * Licensed under the GPL
 */

#include "linux/kernel.h"
#include "linux/slab.h"
#include "linux/init.h"
#include "linux/notifier.h"
#include "linux/reboot.h"
#include "linux/utsname.h"
#include "linux/ctype.h"
#include "linux/interrupt.h"
#include "linux/sysrq.h"
#include "linux/workqueue.h"
#include "linux/module.h"
#include "linux/file.h"
#include "linux/fs.h"
#include "linux/namei.h"
#include "linux/proc_fs.h"
#include "linux/syscalls.h"
#include "linux/list.h"
#include "linux/mm.h"
#include "linux/console.h"
#include "asm/irq.h"
#include "asm/uaccess.h"
#include "kern_util.h"
#include "kern.h"
#include "mconsole.h"
#include "mconsole_kern.h"
#include "irq_user.h"
#include "init.h"
#include "os.h"
#include "irq_kern.h"
#include "choose-mode.h"

static int do_unlink_socket(struct notifier_block *notifier,
			    unsigned long what, void *data)
{
	return(mconsole_unlink_socket());
}


static struct notifier_block reboot_notifier = {
	.notifier_call		= do_unlink_socket,
	.priority		= 0,
};

/* Safe without explicit locking for now.  Tasklets provide their own
 * locking, and the interrupt handler is safe because it can't interrupt
 * itself and it can only happen on CPU 0.
 */

static LIST_HEAD(mc_requests);

static void mc_work_proc(struct work_struct *unused)
{
	struct mconsole_entry *req;
	unsigned long flags;

	while(!list_empty(&mc_requests)){
		local_irq_save(flags);
		req = list_entry(mc_requests.next, struct mconsole_entry,
				 list);
		list_del(&req->list);
		local_irq_restore(flags);
		req->request.cmd->handler(&req->request);
		kfree(req);
	}
}

static DECLARE_WORK(mconsole_work, mc_work_proc);

static irqreturn_t mconsole_interrupt(int irq, void *dev_id)
{
	/* long to avoid size mismatch warnings from gcc */
	long fd;
	struct mconsole_entry *new;
	static struct mc_request req;	/* that's OK */

	fd = (long) dev_id;
	while (mconsole_get_request(fd, &req)){
		if(req.cmd->context == MCONSOLE_INTR)
			(*req.cmd->handler)(&req);
		else {
			new = kmalloc(sizeof(*new), GFP_NOWAIT);
			if(new == NULL)
				mconsole_reply(&req, "Out of memory", 1, 0);
			else {
				new->request = req;
				new->request.regs = get_irq_regs()->regs;
				list_add(&new->list, &mc_requests);
			}
		}
	}
	if(!list_empty(&mc_requests))
		schedule_work(&mconsole_work);
	reactivate_fd(fd, MCONSOLE_IRQ);
	return(IRQ_HANDLED);
}

void mconsole_version(struct mc_request *req)
{
	char version[256];

	sprintf(version, "%s %s %s %s %s", utsname()->sysname,
		utsname()->nodename, utsname()->release,
		utsname()->version, utsname()->machine);
	mconsole_reply(req, version, 0, 0);
}

void mconsole_log(struct mc_request *req)
{
	int len;
	char *ptr = req->request.data;

	ptr += strlen("log ");

	len = req->len - (ptr - req->request.data);
	printk("%.*s", len, ptr);
	mconsole_reply(req, "", 0, 0);
}

/* This is a more convoluted version of mconsole_proc, which has some stability
 * problems; however, we need it fixed, because it is expected that UML users
 * mount HPPFS instead of procfs on /proc. And we want mconsole_proc to still
 * show the real procfs content, not the ones from hppfs.*/
#if 0
void mconsole_proc(struct mc_request *req)
{
	struct nameidata nd;
	struct file_system_type *proc;
	struct super_block *super;
	struct file *file;
	int n, err;
	char *ptr = req->request.data, *buf;

	ptr += strlen("proc");
	while(isspace(*ptr)) ptr++;

	proc = get_fs_type("proc");
	if(proc == NULL){
		mconsole_reply(req, "procfs not registered", 1, 0);
		goto out;
	}

	super = (*proc->get_sb)(proc, 0, NULL, NULL);
	put_filesystem(proc);
	if(super == NULL){
		mconsole_reply(req, "Failed to get procfs superblock", 1, 0);
		goto out;
	}
	up_write(&super->s_umount);

	nd.dentry = super->s_root;
	nd.mnt = NULL;
	nd.flags = O_RDONLY + 1;
	nd.last_type = LAST_ROOT;

	/* START: it was experienced that the stability problems are closed
	 * if commenting out these two calls + the below read cycle. To
	 * make UML crash again, it was enough to readd either one.*/
	err = link_path_walk(ptr, &nd);
	if(err){
		mconsole_reply(req, "Failed to look up file", 1, 0);
		goto out_kill;
	}

	file = dentry_open(nd.dentry, nd.mnt, O_RDONLY);
	if(IS_ERR(file)){
		mconsole_reply(req, "Failed to open file", 1, 0);
		goto out_kill;
	}
	/*END*/

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if(buf == NULL){
		mconsole_reply(req, "Failed to allocate buffer", 1, 0);
		goto out_fput;
	}

	if((file->f_op != NULL) && (file->f_op->read != NULL)){
		do {
			n = (*file->f_op->read)(file, buf, PAGE_SIZE - 1,
						&file->f_pos);
			if(n >= 0){
				buf[n] = '\0';
				mconsole_reply(req, buf, 0, (n > 0));
			}
			else {
				mconsole_reply(req, "Read of file failed",
					       1, 0);
				goto out_free;
			}
		} while(n > 0);
	}
	else mconsole_reply(req, "", 0, 0);

 out_free:
	kfree(buf);
 out_fput:
	fput(file);
 out_kill:
	deactivate_super(super);
 out: ;
}
#endif

void mconsole_proc(struct mc_request *req)
{
	char path[64];
	char *buf;
	int len;
	int fd;
	int first_chunk = 1;
	char *ptr = req->request.data;

	ptr += strlen("proc");
	while(isspace(*ptr)) ptr++;
	snprintf(path, sizeof(path), "/proc/%s", ptr);

	fd = sys_open(path, 0, 0);
	if (fd < 0) {
		mconsole_reply(req, "Failed to open file", 1, 0);
		printk("open %s: %d\n",path,fd);
		goto out;
	}

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if(buf == NULL){
		mconsole_reply(req, "Failed to allocate buffer", 1, 0);
		goto out_close;
	}

	for (;;) {
		len = sys_read(fd, buf, PAGE_SIZE-1);
		if (len < 0) {
			mconsole_reply(req, "Read of file failed", 1, 0);
			goto out_free;
		}
		/*Begin the file content on his own line.*/
		if (first_chunk) {
			mconsole_reply(req, "\n", 0, 1);
			first_chunk = 0;
		}
		if (len == PAGE_SIZE-1) {
			buf[len] = '\0';
			mconsole_reply(req, buf, 0, 1);
		} else {
			buf[len] = '\0';
			mconsole_reply(req, buf, 0, 0);
			break;
		}
	}

 out_free:
	kfree(buf);
 out_close:
	sys_close(fd);
 out:
	/* nothing */;
}

#define UML_MCONSOLE_HELPTEXT \
"Commands: \n\
    version - Get kernel version \n\
    help - Print this message \n\
    halt - Halt UML \n\
    reboot - Reboot UML \n\
    config <dev>=<config> - Add a new device to UML;  \n\
	same syntax as command line \n\
    config <dev> - Query the configuration of a device \n\
    remove <dev> - Remove a device from UML \n\
    sysrq <letter> - Performs the SysRq action controlled by the letter \n\
    cad - invoke the Ctrl-Alt-Del handler \n\
    stop - pause the UML; it will do nothing until it receives a 'go' \n\
    go - continue the UML after a 'stop' \n\
    log <string> - make UML enter <string> into the kernel log\n\
    proc <file> - returns the contents of the UML's /proc/<file>\n\
    stack <pid> - returns the stack of the specified pid\n\
"

void mconsole_help(struct mc_request *req)
{
	mconsole_reply(req, UML_MCONSOLE_HELPTEXT, 0, 0);
}

void mconsole_halt(struct mc_request *req)
{
	mconsole_reply(req, "", 0, 0);
	machine_halt();
}

void mconsole_reboot(struct mc_request *req)
{
	mconsole_reply(req, "", 0, 0);
	machine_restart(NULL);
}

void mconsole_cad(struct mc_request *req)
{
	mconsole_reply(req, "", 0, 0);
	ctrl_alt_del();
}

void mconsole_go(struct mc_request *req)
{
	mconsole_reply(req, "Not stopped", 1, 0);
}

void mconsole_stop(struct mc_request *req)
{
	deactivate_fd(req->originating_fd, MCONSOLE_IRQ);
	os_set_fd_block(req->originating_fd, 1);
	mconsole_reply(req, "stopped", 0, 0);
	while (mconsole_get_request(req->originating_fd, req)) {
		if (req->cmd->handler == mconsole_go)
			break;
		if (req->cmd->handler == mconsole_stop) {
			mconsole_reply(req, "Already stopped", 1, 0);
			continue;
		}
		if (req->cmd->handler == mconsole_sysrq) {
			struct pt_regs *old_regs;
			old_regs = set_irq_regs((struct pt_regs *)&req->regs);
			mconsole_sysrq(req);
			set_irq_regs(old_regs);
			continue;
		}
		(*req->cmd->handler)(req);
	}
	os_set_fd_block(req->originating_fd, 0);
	reactivate_fd(req->originating_fd, MCONSOLE_IRQ);
	mconsole_reply(req, "", 0, 0);
}

static DEFINE_SPINLOCK(mc_devices_lock);
static LIST_HEAD(mconsole_devices);

void mconsole_register_dev(struct mc_device *new)
{
	spin_lock(&mc_devices_lock);
	BUG_ON(!list_empty(&new->list));
	list_add(&new->list, &mconsole_devices);
	spin_unlock(&mc_devices_lock);
}

static struct mc_device *mconsole_find_dev(char *name)
{
	struct list_head *ele;
	struct mc_device *dev;

	list_for_each(ele, &mconsole_devices){
		dev = list_entry(ele, struct mc_device, list);
		if(!strncmp(name, dev->name, strlen(dev->name)))
			return(dev);
	}
	return(NULL);
}

#define UNPLUGGED_PER_PAGE \
	((PAGE_SIZE - sizeof(struct list_head)) / sizeof(unsigned long))

struct unplugged_pages {
	struct list_head list;
	void *pages[UNPLUGGED_PER_PAGE];
};

static DECLARE_MUTEX(plug_mem_mutex);
static unsigned long long unplugged_pages_count = 0;
static LIST_HEAD(unplugged_pages);
static int unplug_index = UNPLUGGED_PER_PAGE;

static int mem_config(char *str, char **error_out)
{
	unsigned long long diff;
	int err = -EINVAL, i, add;
	char *ret;

	if(str[0] != '='){
		*error_out = "Expected '=' after 'mem'";
		goto out;
	}

	str++;
	if(str[0] == '-')
		add = 0;
	else if(str[0] == '+'){
		add = 1;
	}
	else {
		*error_out = "Expected increment to start with '-' or '+'";
		goto out;
	}

	str++;
	diff = memparse(str, &ret);
	if(*ret != '\0'){
		*error_out = "Failed to parse memory increment";
		goto out;
	}

	diff /= PAGE_SIZE;

	down(&plug_mem_mutex);
	for(i = 0; i < diff; i++){
		struct unplugged_pages *unplugged;
		void *addr;

		if(add){
			if(list_empty(&unplugged_pages))
				break;

			unplugged = list_entry(unplugged_pages.next,
					       struct unplugged_pages, list);
			if(unplug_index > 0)
				addr = unplugged->pages[--unplug_index];
			else {
				list_del(&unplugged->list);
				addr = unplugged;
				unplug_index = UNPLUGGED_PER_PAGE;
			}

			free_page((unsigned long) addr);
			unplugged_pages_count--;
		}
		else {
			struct page *page;

			page = alloc_page(GFP_ATOMIC);
			if(page == NULL)
				break;

			unplugged = page_address(page);
			if(unplug_index == UNPLUGGED_PER_PAGE){
				list_add(&unplugged->list, &unplugged_pages);
				unplug_index = 0;
			}
			else {
				struct list_head *entry = unplugged_pages.next;
				addr = unplugged;

				unplugged = list_entry(entry,
						       struct unplugged_pages,
						       list);
				err = os_drop_memory(addr, PAGE_SIZE);
				if(err){
					printk("Failed to release memory - "
					       "errno = %d\n", err);
					*error_out = "Failed to release memory";
					goto out_unlock;
				}
				unplugged->pages[unplug_index++] = addr;
			}

			unplugged_pages_count++;
		}
	}

	err = 0;
out_unlock:
	up(&plug_mem_mutex);
out:
	return err;
}

static int mem_get_config(char *name, char *str, int size, char **error_out)
{
	char buf[sizeof("18446744073709551615")];
	int len = 0;

	sprintf(buf, "%ld", uml_physmem);
	CONFIG_CHUNK(str, size, len, buf, 1);

	return len;
}

static int mem_id(char **str, int *start_out, int *end_out)
{
	*start_out = 0;
	*end_out = 0;

	return 0;
}

static int mem_remove(int n, char **error_out)
{
	*error_out = "Memory doesn't support the remove operation";
	return -EBUSY;
}

static struct mc_device mem_mc = {
	.list		= LIST_HEAD_INIT(mem_mc.list),
	.name		= "mem",
	.config		= mem_config,
	.get_config	= mem_get_config,
	.id		= mem_id,
	.remove		= mem_remove,
};

static int __init mem_mc_init(void)
{
	if(can_drop_memory())
		mconsole_register_dev(&mem_mc);
	else printk("Can't release memory to the host - memory hotplug won't "
		    "be supported\n");
	return 0;
}

__initcall(mem_mc_init);

#define CONFIG_BUF_SIZE 64

static void mconsole_get_config(int (*get_config)(char *, char *, int,
						  char **),
				struct mc_request *req, char *name)
{
	char default_buf[CONFIG_BUF_SIZE], *error, *buf;
	int n, size;

	if(get_config == NULL){
		mconsole_reply(req, "No get_config routine defined", 1, 0);
		return;
	}

	error = NULL;
	size = ARRAY_SIZE(default_buf);
	buf = default_buf;

	while(1){
		n = (*get_config)(name, buf, size, &error);
		if(error != NULL){
			mconsole_reply(req, error, 1, 0);
			goto out;
		}

		if(n <= size){
			mconsole_reply(req, buf, 0, 0);
			goto out;
		}

		if(buf != default_buf)
			kfree(buf);

		size = n;
		buf = kmalloc(size, GFP_KERNEL);
		if(buf == NULL){
			mconsole_reply(req, "Failed to allocate buffer", 1, 0);
			return;
		}
	}
 out:
	if(buf != default_buf)
		kfree(buf);
}

void mconsole_config(struct mc_request *req)
{
	struct mc_device *dev;
	char *ptr = req->request.data, *name, *error_string = "";
	int err;

	ptr += strlen("config");
	while(isspace(*ptr)) ptr++;
	dev = mconsole_find_dev(ptr);
	if(dev == NULL){
		mconsole_reply(req, "Bad configuration option", 1, 0);
		return;
	}

	name = &ptr[strlen(dev->name)];
	ptr = name;
	while((*ptr != '=') && (*ptr != '\0'))
		ptr++;

	if(*ptr == '='){
		err = (*dev->config)(name, &error_string);
		mconsole_reply(req, error_string, err, 0);
	}
	else mconsole_get_config(dev->get_config, req, name);
}

void mconsole_remove(struct mc_request *req)
{
	struct mc_device *dev;
	char *ptr = req->request.data, *err_msg = "";
	char error[256];
	int err, start, end, n;

	ptr += strlen("remove");
	while(isspace(*ptr)) ptr++;
	dev = mconsole_find_dev(ptr);
	if(dev == NULL){
		mconsole_reply(req, "Bad remove option", 1, 0);
		return;
	}

	ptr = &ptr[strlen(dev->name)];

	err = 1;
	n = (*dev->id)(&ptr, &start, &end);
	if(n < 0){
		err_msg = "Couldn't parse device number";
		goto out;
	}
	else if((n < start) || (n > end)){
		sprintf(error, "Invalid device number - must be between "
			"%d and %d", start, end);
		err_msg = error;
		goto out;
	}

	err_msg = NULL;
	err = (*dev->remove)(n, &err_msg);
	switch(err){
	case 0:
		err_msg = "";
		break;
	case -ENODEV:
		if(err_msg == NULL)
			err_msg = "Device doesn't exist";
		break;
	case -EBUSY:
		if(err_msg == NULL)
			err_msg = "Device is currently open";
		break;
	default:
		break;
	}
out:
	mconsole_reply(req, err_msg, err, 0);
}

struct mconsole_output {
	struct list_head list;
	struct mc_request *req;
};

static DEFINE_SPINLOCK(client_lock);
static LIST_HEAD(clients);
static char console_buf[MCONSOLE_MAX_DATA];
static int console_index = 0;

static void console_write(struct console *console, const char *string,
			  unsigned len)
{
	struct list_head *ele;
	int n;

	if(list_empty(&clients))
		return;

	while(1){
		n = min((size_t) len, ARRAY_SIZE(console_buf) - console_index);
		strncpy(&console_buf[console_index], string, n);
		console_index += n;
		string += n;
		len -= n;
		if(len == 0)
			return;

		list_for_each(ele, &clients){
			struct mconsole_output *entry;

			entry = list_entry(ele, struct mconsole_output, list);
			mconsole_reply_len(entry->req, console_buf,
					   console_index, 0, 1);
		}

		console_index = 0;
	}
}

static struct console mc_console = { .name	= "mc",
				     .write	= console_write,
				     .flags	= CON_ENABLED,
				     .index	= -1 };

static int mc_add_console(void)
{
	register_console(&mc_console);
	return 0;
}

late_initcall(mc_add_console);

static void with_console(struct mc_request *req, void (*proc)(void *),
			 void *arg)
{
	struct mconsole_output entry;
	unsigned long flags;

	entry.req = req;
	spin_lock_irqsave(&client_lock, flags);
	list_add(&entry.list, &clients);
	spin_unlock_irqrestore(&client_lock, flags);

	(*proc)(arg);

	mconsole_reply_len(req, console_buf, console_index, 0, 0);
	console_index = 0;

	spin_lock_irqsave(&client_lock, flags);
	list_del(&entry.list);
	spin_unlock_irqrestore(&client_lock, flags);
}

#ifdef CONFIG_MAGIC_SYSRQ
static void sysrq_proc(void *arg)
{
	char *op = arg;
	handle_sysrq(*op, NULL);
}

void mconsole_sysrq(struct mc_request *req)
{
	char *ptr = req->request.data;

	ptr += strlen("sysrq");
	while(isspace(*ptr)) ptr++;

	/* With 'b', the system will shut down without a chance to reply,
	 * so in this case, we reply first.
	 */
	if(*ptr == 'b')
		mconsole_reply(req, "", 0, 0);

	with_console(req, sysrq_proc, ptr);
}
#else
void mconsole_sysrq(struct mc_request *req)
{
	mconsole_reply(req, "Sysrq not compiled in", 1, 0);
}
#endif

#ifdef CONFIG_MODE_SKAS

static void stack_proc(void *arg)
{
	struct task_struct *from = current, *to = arg;

	to->thread.saved_task = from;
	switch_to(from, to, from);
}

/* Mconsole stack trace
 *  Added by Allan Graves, Jeff Dike
 *  Dumps a stacks registers to the linux console.
 *  Usage stack <pid>.
 */
static void do_stack_trace(struct mc_request *req)
{
	char *ptr = req->request.data;
	int pid_requested= -1;
	struct task_struct *from = NULL;
	struct task_struct *to = NULL;

	/* Would be nice:
	 * 1) Send showregs output to mconsole.
	 * 2) Add a way to stack dump all pids.
	 */

	ptr += strlen("stack");
	while(isspace(*ptr)) ptr++;

	/* Should really check for multiple pids or reject bad args here */
	/* What do the arguments in mconsole_reply mean? */
	if(sscanf(ptr, "%d", &pid_requested) == 0){
		mconsole_reply(req, "Please specify a pid", 1, 0);
		return;
	}

	from = current;

	to = find_task_by_pid(pid_requested);
	if((to == NULL) || (pid_requested == 0)) {
		mconsole_reply(req, "Couldn't find that pid", 1, 0);
		return;
	}
	with_console(req, stack_proc, to);
}
#endif /* CONFIG_MODE_SKAS */

void mconsole_stack(struct mc_request *req)
{
	/* This command doesn't work in TT mode, so let's check and then
	 * get out of here
	 */
	CHOOSE_MODE(mconsole_reply(req, "Sorry, this doesn't work in TT mode",
				   1, 0),
		    do_stack_trace(req));
}

/* Changed by mconsole_setup, which is __setup, and called before SMP is
 * active.
 */
static char *notify_socket = NULL;

static int __init mconsole_init(void)
{
	/* long to avoid size mismatch warnings from gcc */
	long sock;
	int err;
	char file[256];

	if(umid_file_name("mconsole", file, sizeof(file))) return(-1);
	snprintf(mconsole_socket_name, sizeof(file), "%s", file);

	sock = os_create_unix_socket(file, sizeof(file), 1);
	if (sock < 0){
		printk("Failed to initialize management console\n");
		return(1);
	}

	register_reboot_notifier(&reboot_notifier);

	err = um_request_irq(MCONSOLE_IRQ, sock, IRQ_READ, mconsole_interrupt,
			     IRQF_DISABLED | IRQF_SHARED | IRQF_SAMPLE_RANDOM,
			     "mconsole", (void *)sock);
	if (err){
		printk("Failed to get IRQ for management console\n");
		return(1);
	}

	if(notify_socket != NULL){
		notify_socket = kstrdup(notify_socket, GFP_KERNEL);
		if(notify_socket != NULL)
			mconsole_notify(notify_socket, MCONSOLE_SOCKET,
					mconsole_socket_name,
					strlen(mconsole_socket_name) + 1);
		else printk(KERN_ERR "mconsole_setup failed to strdup "
			    "string\n");
	}

	printk("mconsole (version %d) initialized on %s\n",
	       MCONSOLE_VERSION, mconsole_socket_name);
	return(0);
}

__initcall(mconsole_init);

static int write_proc_mconsole(struct file *file, const char __user *buffer,
			       unsigned long count, void *data)
{
	char *buf;

	buf = kmalloc(count + 1, GFP_KERNEL);
	if(buf == NULL)
		return(-ENOMEM);

	if(copy_from_user(buf, buffer, count)){
		count = -EFAULT;
		goto out;
	}

	buf[count] = '\0';

	mconsole_notify(notify_socket, MCONSOLE_USER_NOTIFY, buf, count);
 out:
	kfree(buf);
	return(count);
}

static int create_proc_mconsole(void)
{
	struct proc_dir_entry *ent;

	if(notify_socket == NULL) return(0);

	ent = create_proc_entry("mconsole", S_IFREG | 0200, NULL);
	if(ent == NULL){
		printk(KERN_INFO "create_proc_mconsole : create_proc_entry failed\n");
		return(0);
	}

	ent->read_proc = NULL;
	ent->write_proc = write_proc_mconsole;
	return(0);
}

static DEFINE_SPINLOCK(notify_spinlock);

void lock_notify(void)
{
	spin_lock(&notify_spinlock);
}

void unlock_notify(void)
{
	spin_unlock(&notify_spinlock);
}

__initcall(create_proc_mconsole);

#define NOTIFY "=notify:"

static int mconsole_setup(char *str)
{
	if(!strncmp(str, NOTIFY, strlen(NOTIFY))){
		str += strlen(NOTIFY);
		notify_socket = str;
	}
	else printk(KERN_ERR "mconsole_setup : Unknown option - '%s'\n", str);
	return(1);
}

__setup("mconsole", mconsole_setup);

__uml_help(mconsole_setup,
"mconsole=notify:<socket>\n"
"    Requests that the mconsole driver send a message to the named Unix\n"
"    socket containing the name of the mconsole socket.  This also serves\n"
"    to notify outside processes when UML has booted far enough to respond\n"
"    to mconsole requests.\n\n"
);

static int notify_panic(struct notifier_block *self, unsigned long unused1,
			void *ptr)
{
	char *message = ptr;

	if(notify_socket == NULL) return(0);

	mconsole_notify(notify_socket, MCONSOLE_PANIC, message,
			strlen(message) + 1);
	return(0);
}

static struct notifier_block panic_exit_notifier = {
	.notifier_call 		= notify_panic,
	.next 			= NULL,
	.priority 		= 1
};

static int add_notifier(void)
{
	atomic_notifier_chain_register(&panic_notifier_list,
			&panic_exit_notifier);
	return(0);
}

__initcall(add_notifier);

char *mconsole_notify_socket(void)
{
	return(notify_socket);
}

EXPORT_SYMBOL(mconsole_notify_socket);
