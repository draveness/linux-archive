/*
 *  linux/fs/hfsplus/options.c
 *
 * Copyright (C) 2001
 * Brad Boyer (flar@allandria.com)
 * (C) 2003 Ardis Technologies <roman@ardistech.com>
 *
 * Option parsing
 */

#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include "hfsplus_fs.h"

/* Initialize an options object to reasonable defaults */
void fill_defaults(struct hfsplus_sb_info *opts)
{
	if (!opts)
		return;

	opts->creator = HFSPLUS_DEF_CR_TYPE;
	opts->type = HFSPLUS_DEF_CR_TYPE;
	opts->umask = current->fs->umask;
	opts->uid = current->uid;
	opts->gid = current->gid;
	opts->part = -1;
	opts->session = -1;
}

/* convert a "four byte character" to a 32 bit int with error checks */
static int fill_fourchar(u32 *result, char *input)
{
	u32 out;
	int i;

	if (!result || !input || !*input || (strlen(input) != 4))
		return 0;

	for (out = 0, i = 0; i < 4; i++) {
		out <<= 8;
		out |= ((int)(input[i])) & 0xFF;
	}
	*result = out;
	return 1;
}

/* convert a string to int with error checks */
static int fill_int(int *result, char *input, int base)
{
	char *tmp = input;
	int intval;

	if (!result || !input || !*input)
		return 0;

	intval = simple_strtoul(tmp, &tmp, base);
	if (*tmp)
		return 0;

	*result = intval;
	return 1;
}

/* Parse options from mount. Returns 0 on failure */
/* input is the options passed to mount() as a string */
int parse_options(char *input, struct hfsplus_sb_info *results)
{
	char *curropt, *value;
	int tmp;

	if (!input)
		return 1;

	while ((curropt = strsep(&input,",")) != NULL) {
		if (!*curropt)
			continue;

		if ((value = strchr(curropt, '=')) != NULL)
			*value++ = '\0';

		if (!strcmp(curropt, "creator")) {
			if (!fill_fourchar(&(results->creator), value)) {
				printk("HFS+-fs: creator requires a 4 character value\n");
				return 0;
			}
		} else if (!strcmp(curropt, "type")) {
			if (!fill_fourchar(&(results->type), value)) {
				printk("HFS+-fs: type requires a 4 character value\n");
				return 0;
			}
		} else if (!strcmp(curropt, "umask")) {
			if (!fill_int(&tmp, value, 8)) {
				printk("HFS+-fs: umask requires a value\n");
				return 0;
			}
			results->umask = (umode_t)tmp;
		} else if (!strcmp(curropt, "uid")) {
			if (!fill_int(&tmp, value, 0)) {
				printk("HFS+-fs: uid requires an argument\n");
				return 0;
			}
			results->uid = (uid_t)tmp;
		} else if (!strcmp(curropt, "gid")) {
			if (!fill_int(&tmp, value, 0)) {
				printk("HFS+-fs: gid requires an argument\n");
				return 0;
			}
			results->gid = (gid_t)tmp;
		} else if (!strcmp(curropt, "part")) {
			if (!fill_int(&results->part, value, 0)) {
				printk("HFS+-fs: part requires an argument\n");
				return 0;
			}
		} else if (!strcmp(curropt, "session")) {
			if (!fill_int(&results->session, value, 0)) {
				printk("HFS+-fs: session requires an argument\n");
				return 0;
			}
		} else {
			printk("HFS+-fs: unknown option %s\n", curropt);
			return 0;
		}
	}

	return 1;
}
