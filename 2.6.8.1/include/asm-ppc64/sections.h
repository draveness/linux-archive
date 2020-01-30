#ifndef _PPC64_SECTIONS_H
#define _PPC64_SECTIONS_H

extern char _end[];

#include <asm-generic/sections.h>

#define __pmac
#define __pmacdata

#define __prep
#define __prepdata

#define __chrp
#define __chrpdata

#define __openfirmware
#define __openfirmwaredata

#endif
