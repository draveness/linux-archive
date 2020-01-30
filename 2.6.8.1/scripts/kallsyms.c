/* Generate assembler source containing symbol information
 *
 * Copyright 2002       by Kai Germaschewski
 *
 * This software may be used and distributed according to the terms
 * of the GNU General Public License, incorporated herein by reference.
 *
 * Usage: nm -n vmlinux | scripts/kallsyms [--all-symbols] > symbols.S
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct sym_entry {
	unsigned long long addr;
	char type;
	char *sym;
};


static struct sym_entry *table;
static int size, cnt;
static unsigned long long _stext, _etext, _sinittext, _einittext;
static int all_symbols = 0;

static void
usage(void)
{
	fprintf(stderr, "Usage: kallsyms [--all-symbols] < in.map > out.S\n");
	exit(1);
}

static int
read_symbol(FILE *in, struct sym_entry *s)
{
	char str[500];
	int rc;

	rc = fscanf(in, "%llx %c %499s\n", &s->addr, &s->type, str);
	if (rc != 3) {
		if (rc != EOF) {
			/* skip line */
			fgets(str, 500, in);
		}
		return -1;
	}

	/* Ignore most absolute/undefined (?) symbols. */
	if (strcmp(str, "_stext") == 0)
		_stext = s->addr;
	else if (strcmp(str, "_etext") == 0)
		_etext = s->addr;
	else if (strcmp(str, "_sinittext") == 0)
		_sinittext = s->addr;
	else if (strcmp(str, "_einittext") == 0)
		_einittext = s->addr;
	else if (toupper(s->type) == 'A' || toupper(s->type) == 'U')
		return -1;

	s->sym = strdup(str);
	return 0;
}

static int
symbol_valid(struct sym_entry *s)
{
	if (!all_symbols) {
		if ((s->addr < _stext || s->addr > _etext)
		    && (s->addr < _sinittext || s->addr > _einittext))
			return 0;
	}

	/* Exclude symbols which vary between passes.  Passes 1 and 2 must have
	 * identical symbol lists.  The kallsyms_* symbols below are only added
	 * after pass 1, they would be included in pass 2 when --all-symbols is
	 * specified so exclude them to get a stable symbol list.
	 */
	if (strstr(s->sym, "_compiled.") ||
	    strcmp(s->sym, "kallsyms_addresses") == 0 ||
	    strcmp(s->sym, "kallsyms_num_syms") == 0 ||
	    strcmp(s->sym, "kallsyms_names") == 0)
		return 0;

	/* Exclude linker generated symbols which vary between passes */
	if (strcmp(s->sym, "_SDA_BASE_") == 0 ||	/* ppc */
	    strcmp(s->sym, "_SDA2_BASE_") == 0)		/* ppc */
		return 0;

	return 1;
}

static void
read_map(FILE *in)
{
	while (!feof(in)) {
		if (cnt >= size) {
			size += 10000;
			table = realloc(table, sizeof(*table) * size);
			if (!table) {
				fprintf(stderr, "out of memory\n");
				exit (1);
			}
		}
		if (read_symbol(in, &table[cnt]) == 0)
			cnt++;
	}
}

static void
write_src(void)
{
	int i, valid = 0;
	char *prev;

	printf("#include <asm/types.h>\n");
	printf("#if BITS_PER_LONG == 64\n");
	printf("#define PTR .quad\n");
	printf("#define ALGN .align 8\n");
	printf("#else\n");
	printf("#define PTR .long\n");
	printf("#define ALGN .align 4\n");
	printf("#endif\n");

	printf(".data\n");

	printf(".globl kallsyms_addresses\n");
	printf("\tALGN\n");
	printf("kallsyms_addresses:\n");
	for (i = 0; i < cnt; i++) {
		if (!symbol_valid(&table[i]))
			continue;

		printf("\tPTR\t%#llx\n", table[i].addr);
		valid++;
	}
	printf("\n");

	printf(".globl kallsyms_num_syms\n");
	printf("\tALGN\n");
	printf("kallsyms_num_syms:\n");
	printf("\tPTR\t%d\n", valid);
	printf("\n");

	printf(".globl kallsyms_names\n");
	printf("\tALGN\n");
	printf("kallsyms_names:\n");
	prev = ""; 
	for (i = 0; i < cnt; i++) {
		int k;

		if (!symbol_valid(&table[i]))
			continue;

		for (k = 0; table[i].sym[k] && table[i].sym[k] == prev[k]; ++k)
			; 

		printf("\t.byte 0x%02x\n\t.asciz\t\"%s\"\n", k, table[i].sym + k);
		prev = table[i].sym;
	}
	printf("\n");
}

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--all-symbols") == 0)
		all_symbols = 1;
	else if (argc != 1)
		usage();

	read_map(stdin);
	write_src();

	return 0;
}

