/*
 * Copyright (C) 2000, 2001, 2002 Jeff Dike (jdike@karaya.com)
 * Licensed under the GPL
 */

#include "linux/init.h"
#include "linux/bootmem.h"
#include "linux/initrd.h"
#include "asm/types.h"
#include "user_util.h"
#include "kern_util.h"
#include "initrd.h"
#include "init.h"
#include "os.h"

/* Changed by uml_initrd_setup, which is a setup */
static char *initrd __initdata = NULL;

static int __init read_initrd(void)
{
	void *area;
	long long size;
	int err;

	if(initrd == NULL) return 0;
	err = os_file_size(initrd, &size);
	if(err) return 0;
	area = alloc_bootmem(size);
	if(area == NULL) return 0;
	if(load_initrd(initrd, area, size) == -1) return 0;
	initrd_start = (unsigned long) area;
	initrd_end = initrd_start + size;
	return 0;
}

__uml_postsetup(read_initrd);

static int __init uml_initrd_setup(char *line, int *add)
{
	initrd = line;
	return 0;
}

__uml_setup("initrd=", uml_initrd_setup, 
"initrd=<initrd image>\n"
"    This is used to boot UML from an initrd image.  The argument is the\n"
"    name of the file containing the image.\n\n"
);

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
