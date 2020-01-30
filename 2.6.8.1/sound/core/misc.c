/*
 *  Misc and compatibility things
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <sound/driver.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/time.h>
#include <sound/core.h>

int snd_task_name(struct task_struct *task, char *name, size_t size)
{
	unsigned int idx;

	snd_assert(task != NULL && name != NULL && size >= 2, return -EINVAL);
	for (idx = 0; idx < sizeof(task->comm) && idx + 1 < size; idx++)
		name[idx] = task->comm[idx];
	name[idx] = '\0';
	return 0;
}

#ifdef CONFIG_SND_VERBOSE_PRINTK
void snd_verbose_printk(const char *file, int line, const char *format, ...)
{
	va_list args;
	char tmpbuf[512];
	
	if (format[0] == '<' && format[1] >= '0' && format[1] <= '9' && format[2] == '>') {
		char tmp[] = "<0>";
		tmp[1] = format[1];
		printk("%sALSA %s:%d: ", tmp, file, line);
		format += 3;
	} else {
		printk("ALSA %s:%d: ", file, line);
	}
	va_start(args, format);
	vsnprintf(tmpbuf, sizeof(tmpbuf), format, args);
	va_end(args);
	printk(tmpbuf);
}
#endif

#if defined(CONFIG_SND_DEBUG) && defined(CONFIG_SND_VERBOSE_PRINTK)
void snd_verbose_printd(const char *file, int line, const char *format, ...)
{
	va_list args;
	char tmpbuf[512];
	
	if (format[0] == '<' && format[1] >= '0' && format[1] <= '9' && format[2] == '>') {
		char tmp[] = "<0>";
		tmp[1] = format[1];
		printk("%sALSA %s:%d: ", tmp, file, line);
		format += 3;
	} else {
		printk(KERN_DEBUG "ALSA %s:%d: ", file, line);
	}
	va_start(args, format);
	vsnprintf(tmpbuf, sizeof(tmpbuf), format, args);
	va_end(args);
	printk(tmpbuf);

}
#endif
