/*
 * Kernel exception handling table support.  Derived from arch/alpha/mm/extable.c.
 *
 * Copyright (C) 1998, 1999, 2001-2002, 2004 Hewlett-Packard Co
 *	David Mosberger-Tang <davidm@hpl.hp.com>
 */

#include <linux/config.h>

#include <asm/uaccess.h>
#include <asm/module.h>

static inline int
compare_entries (struct exception_table_entry *l, struct exception_table_entry *r)
{
	u64 lip = (u64) &l->addr + l->addr;
	u64 rip = (u64) &r->addr + r->addr;

	if (lip < rip)
		return -1;
	if (lip == rip)
		return 0;
	else
		return 1;
}

static inline void
swap_entries (struct exception_table_entry *l, struct exception_table_entry *r)
{
	u64 delta = (u64) r - (u64) l;
	struct exception_table_entry tmp;

	tmp = *l;
	l->addr = r->addr + delta;
	l->cont = r->cont + delta;
	r->addr = tmp.addr - delta;
	r->cont = tmp.cont - delta;
}

/*
 * Sort the exception table.  It's usually already sorted, but there may be unordered
 * entries due to multiple text sections (such as the .init text section).  Note that the
 * exception-table-entries contain location-relative addresses, which requires a bit of
 * care during sorting to avoid overflows in the offset members (e.g., it would not be
 * safe to make a temporary copy of an exception-table entry on the stack, because the
 * stack may be more than 2GB away from the exception-table).
 */
void
sort_extable (struct exception_table_entry *start, struct exception_table_entry *finish)
{
	struct exception_table_entry *p, *q;

 	/* insertion sort */
	for (p = start + 1; p < finish; ++p)
		/* start .. p-1 is sorted; push p down to it's proper place */
		for (q = p; q > start && compare_entries(&q[0], &q[-1]) < 0; --q)
			swap_entries(&q[0], &q[-1]);
}

const struct exception_table_entry *
search_extable (const struct exception_table_entry *first,
		const struct exception_table_entry *last,
		unsigned long ip)
{
	const struct exception_table_entry *mid;
	unsigned long mid_ip;
	long diff;

        while (first <= last) {
		mid = &first[(last - first)/2];
		mid_ip = (u64) &mid->addr + mid->addr;
		diff = mid_ip - ip;
                if (diff == 0)
                        return mid;
                else if (diff < 0)
                        first = mid + 1;
                else
                        last = mid - 1;
        }
        return 0;
}

void
ia64_handle_exception (struct pt_regs *regs, const struct exception_table_entry *e)
{
	long fix = (u64) &e->cont + e->cont;

	regs->r8 = -EFAULT;
	if (fix & 4)
		regs->r9 = 0;
	regs->cr_iip = fix & ~0xf;
	ia64_psr(regs)->ri = fix & 0x3;		/* set continuation slot number */
}
