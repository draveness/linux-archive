/*
 * addinitrd - program to add a initrd image to an ecoff kernel
 *
 * (C) 1999 Thomas Bogendoerfer
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "ecoff.h"

#define MIPS_PAGE_SIZE	4096
#define MIPS_PAGE_MASK	(MIPS_PAGE_SIZE-1)

#define swab16(x) \
        ((unsigned short)( \
                (((unsigned short)(x) & (unsigned short)0x00ffU) << 8) | \
                (((unsigned short)(x) & (unsigned short)0xff00U) >> 8) ))

#define swab32(x) \
        ((unsigned int)( \
                (((unsigned int)(x) & (unsigned int)0x000000ffUL) << 24) | \
                (((unsigned int)(x) & (unsigned int)0x0000ff00UL) <<  8) | \
                (((unsigned int)(x) & (unsigned int)0x00ff0000UL) >>  8) | \
                (((unsigned int)(x) & (unsigned int)0xff000000UL) >> 24) ))

#define SWAB(a)	(swab ? swab32(a) : (a))

void die (char *s)
{
	perror (s);
	exit (1);
}

int main (int argc, char *argv[])
{
	int fd_vmlinux,fd_initrd,fd_outfile;
	FILHDR efile;
	AOUTHDR eaout;
	SCNHDR esecs[3];
	struct stat st;
	char buf[1024];
	unsigned long loadaddr;
	unsigned long initrd_header[2];
	int i;
	int swab = 0;

	if (argc != 4) {
		printf ("Usage: %s <vmlinux> <initrd> <outfile>\n",argv[0]);
		exit (1);
	}

	if ((fd_vmlinux = open (argv[1],O_RDWR)) < 0)
		 die ("open vmlinux");
	if (read (fd_vmlinux, &efile, sizeof efile) != sizeof efile)
		die ("read file header");
	if (read (fd_vmlinux, &eaout, sizeof eaout) != sizeof eaout)
		die ("read aout header");
	if (read (fd_vmlinux, esecs, sizeof esecs) != sizeof esecs)
		die ("read section headers");

	/*
	 * check whether the file is good for us
	 */
	/* TBD */

	/*
	 * check, if we have to swab words
	 */
	if (ntohs(0xaa55) == 0xaa55) {
		if (efile.f_magic == swab16(MIPSELMAGIC))
			swab = 1;
	} else {
		if (efile.f_magic == swab16(MIPSEBMAGIC))
			swab = 1;
	}

	if ((fd_initrd = open (argv[2], O_RDONLY)) < 0)
		die ("open initrd");
	if (fstat (fd_initrd, &st) < 0)
		die ("fstat initrd");
	loadaddr = ((SWAB(esecs[2].s_vaddr) + SWAB(esecs[2].s_size) 
			+ MIPS_PAGE_SIZE-1) & ~MIPS_PAGE_MASK) - 8;
	if (loadaddr < (SWAB(esecs[2].s_vaddr) + SWAB(esecs[2].s_size)))
		loadaddr += MIPS_PAGE_SIZE;
	initrd_header[0] = SWAB(0x494E5244);
	initrd_header[1] = SWAB(st.st_size);
	eaout.dsize = esecs[1].s_size = initrd_header[1] = SWAB(st.st_size+8);
	eaout.data_start = esecs[1].s_vaddr = esecs[1].s_paddr = SWAB(loadaddr);

	if ((fd_outfile = open (argv[3], O_RDWR|O_CREAT|O_TRUNC,0666)) < 0)
		die ("open outfile");
	if (write (fd_outfile, &efile, sizeof efile) != sizeof efile)
		die ("write file header");
	if (write (fd_outfile, &eaout, sizeof eaout) != sizeof eaout)
		die ("write aout header");
	if (write (fd_outfile, esecs, sizeof esecs) != sizeof esecs)
		die ("write section headers");
	while ((i = read (fd_vmlinux, buf, sizeof buf)) > 0)
		if (write (fd_outfile, buf, i) != i)
			die ("write vmlinux");
	if (write (fd_outfile, initrd_header, sizeof initrd_header) != sizeof initrd_header)
		die ("write initrd header");
	while ((i = read (fd_initrd, buf, sizeof buf)) > 0)
		if (write (fd_outfile, buf, i) != i)
			die ("write initrd");
	close (fd_vmlinux);
	close (fd_initrd);
	return 0;
}
