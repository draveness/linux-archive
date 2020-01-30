/**
 * @file oprof.h
 *
 * @remark Copyright 2002 OProfile authors
 * @remark Read the file COPYING
 *
 * @author John Levon <levon@movementarian.org>
 */

#ifndef OPROF_H
#define OPROF_H

int oprofile_setup(void);
void oprofile_shutdown(void); 

int oprofilefs_register(void);
void oprofilefs_unregister(void);

int oprofile_start(void);
void oprofile_stop(void);

struct oprofile_operations;
 
extern unsigned long fs_buffer_size;
extern unsigned long fs_cpu_buffer_size;
extern unsigned long fs_buffer_watershed;
extern struct oprofile_operations oprofile_ops;
extern unsigned long oprofile_started;
extern unsigned long backtrace_depth;
 
struct super_block;
struct dentry;

void oprofile_create_files(struct super_block * sb, struct dentry * root);
void oprofile_timer_init(struct oprofile_operations * ops);

int oprofile_set_backtrace(unsigned long depth);
 
#endif /* OPROF_H */
