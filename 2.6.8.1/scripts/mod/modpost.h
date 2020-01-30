#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <elf.h>

#include "elfconfig.h"

#if KERNEL_ELFCLASS == ELFCLASS32

#define Elf_Ehdr    Elf32_Ehdr 
#define Elf_Shdr    Elf32_Shdr 
#define Elf_Sym     Elf32_Sym
#define ELF_ST_BIND ELF32_ST_BIND
#define ELF_ST_TYPE ELF32_ST_TYPE

#else

#define Elf_Ehdr    Elf64_Ehdr 
#define Elf_Shdr    Elf64_Shdr 
#define Elf_Sym     Elf64_Sym
#define ELF_ST_BIND ELF64_ST_BIND
#define ELF_ST_TYPE ELF64_ST_TYPE

#endif

#if KERNEL_ELFDATA != HOST_ELFDATA

static inline void __endian(const void *src, void *dest, unsigned int size)
{
	unsigned int i;
	for (i = 0; i < size; i++)
		((unsigned char*)dest)[i] = ((unsigned char*)src)[size - i-1];
}



#define TO_NATIVE(x)						\
({								\
	typeof(x) __x;						\
	__endian(&(x), &(__x), sizeof(__x));			\
	__x;							\
})

#else /* endianness matches */

#define TO_NATIVE(x) (x)

#endif

#define NOFAIL(ptr)   do_nofail((ptr), __FILE__, __LINE__, #ptr)
void *do_nofail(void *ptr, const char *file, int line, const char *expr);

struct buffer {
	char *p;
	int pos;
	int size;
};

void __attribute__((format(printf, 2, 3)))
buf_printf(struct buffer *buf, const char *fmt, ...);

void
buf_write(struct buffer *buf, const char *s, int len);

struct module {
	struct module *next;
	const char *name;
	struct symbol *unres;
	int seen;
	int skip;
	struct buffer dev_table_buf;
};

struct elf_info {
	unsigned long size;
	Elf_Ehdr     *hdr;
	Elf_Shdr     *sechdrs;
	Elf_Sym      *symtab_start;
	Elf_Sym      *symtab_stop;
	const char   *strtab;
	char	     *modinfo;
	unsigned int modinfo_len;
};

void handle_moddevtable(struct module *mod, struct elf_info *info,
			Elf_Sym *sym, const char *symname);

void add_moddevtable(struct buffer *buf, struct module *mod);

void maybe_frob_version(const char *modfilename,
			void *modinfo,
			unsigned long modinfo_len,
			unsigned long modinfo_offset);

void *grab_file(const char *filename, unsigned long *size);
char* get_next_line(unsigned long *pos, void *file, unsigned long size);
void release_file(void *file, unsigned long size);
