/**
 * @file cpu_buffer.h
 *
 * @remark Copyright 2002 OProfile authors
 * @remark Read the file COPYING
 *
 * @author John Levon <levon@movementarian.org>
 */

#ifndef OPROFILE_CPU_BUFFER_H
#define OPROFILE_CPU_BUFFER_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/cache.h>
 
struct task_struct;
 
/* allocate a sample buffer for each CPU */
int alloc_cpu_buffers(void);

void free_cpu_buffers(void);

/* CPU buffer is composed of such entries (which are
 * also used for context switch notes)
 */
struct op_sample {
	unsigned long eip;
	unsigned long event;
};
 
struct oprofile_cpu_buffer {
	volatile unsigned long head_pos;
	volatile unsigned long tail_pos;
	unsigned long buffer_size;
	struct task_struct * last_task;
	int last_is_kernel;
	struct op_sample * buffer;
	unsigned long sample_received;
	unsigned long sample_lost_overflow;
	unsigned long sample_lost_task_exit;
} ____cacheline_aligned;

extern struct oprofile_cpu_buffer cpu_buffer[];

void cpu_buffer_reset(struct oprofile_cpu_buffer *cpu_buf);

#endif /* OPROFILE_CPU_BUFFER_H */
