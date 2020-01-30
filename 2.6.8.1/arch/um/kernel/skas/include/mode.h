/* 
 * Copyright (C) 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#ifndef __MODE_SKAS_H__
#define __MODE_SKAS_H__

extern unsigned long exec_regs[];
extern unsigned long exec_fp_regs[];
extern unsigned long exec_fpx_regs[];
extern int have_fpx_regs;

extern void user_time_init_skas(void);
extern int copy_sc_from_user_skas(union uml_pt_regs *regs, void *from_ptr);
extern int copy_sc_to_user_skas(void *to_ptr, void *fp, 
				union uml_pt_regs *regs, 
				unsigned long fault_addr, int fault_type);
extern void sig_handler_common_skas(int sig, void *sc_ptr);
extern void halt_skas(void);
extern void reboot_skas(void);
extern void kill_off_processes_skas(void);

#endif

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
