/*
 *  linux/fs/binfmt_script.c
 *
 *  Copyright (C) 1996  Martin von L�wis
 *  original #!-checking implemented by tytso.
 */

#include <linux/module.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/malloc.h>
#include <linux/binfmts.h>
#include <linux/init.h>
#include <linux/file.h>
#include <linux/smp_lock.h>

static int load_script(struct linux_binprm *bprm,struct pt_regs *regs)
{
	char *cp, *i_name, *i_arg;
	struct file *file;
	char interp[BINPRM_BUF_SIZE];
	int retval;

	if ((bprm->buf[0] != '#') || (bprm->buf[1] != '!') || (bprm->sh_bang)) 
		return -ENOEXEC;
	/*
	 * This section does the #! interpretation.
	 * Sorta complicated, but hopefully it will work.  -TYT
	 */

	bprm->sh_bang++;
	allow_write_access(bprm->file);
	fput(bprm->file);
	bprm->file = NULL;

	bprm->buf[BINPRM_BUF_SIZE - 1] = '\0';
	if ((cp = strchr(bprm->buf, '\n')) == NULL)
		cp = bprm->buf+BINPRM_BUF_SIZE-1;
	*cp = '\0';
	while (cp > bprm->buf) {
		cp--;
		if ((*cp == ' ') || (*cp == '\t'))
			*cp = '\0';
		else
			break;
	}
	for (cp = bprm->buf+2; (*cp == ' ') || (*cp == '\t'); cp++);
	if (*cp == '\0') 
		return -ENOEXEC; /* No interpreter name found */
	i_name = cp;
	i_arg = 0;
	for ( ; *cp && (*cp != ' ') && (*cp != '\t'); cp++)
		/* nothing */ ;
	while ((*cp == ' ') || (*cp == '\t'))
		*cp++ = '\0';
	if (*cp)
		i_arg = cp;
	strcpy (interp, i_name);
	/*
	 * OK, we've parsed out the interpreter name and
	 * (optional) argument.
	 * Splice in (1) the interpreter's name for argv[0]
	 *           (2) (optional) argument to interpreter
	 *           (3) filename of shell script (replace argv[0])
	 *
	 * This is done in reverse order, because of how the
	 * user environment and arguments are stored.
	 */
	remove_arg_zero(bprm);
	retval = copy_strings_kernel(1, &bprm->filename, bprm);
	if (retval < 0) return retval; 
	bprm->argc++;
	if (i_arg) {
		retval = copy_strings_kernel(1, &i_arg, bprm);
		if (retval < 0) return retval; 
		bprm->argc++;
	}
	retval = copy_strings_kernel(1, &i_name, bprm);
	if (retval) return retval; 
	bprm->argc++;
	/*
	 * OK, now restart the process with the interpreter's dentry.
	 */
	file = open_exec(interp);
	if (IS_ERR(file))
		return PTR_ERR(file);

	bprm->file = file;
	retval = prepare_binprm(bprm);
	if (retval < 0)
		return retval;
	return search_binary_handler(bprm,regs);
}

struct linux_binfmt script_format = {
	NULL, THIS_MODULE, load_script, NULL, NULL, 0
};

static int __init init_script_binfmt(void)
{
	return register_binfmt(&script_format);
}

static void __exit exit_script_binfmt(void)
{
	unregister_binfmt(&script_format);
}

module_init(init_script_binfmt)
module_exit(exit_script_binfmt)
