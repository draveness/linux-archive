/*
 *  include/asm-s390/string.h
 *
 *  S390 version
 *    Copyright (C) 1999 IBM Deutschland Entwicklung GmbH, IBM Corporation
 *    Author(s): Martin Schwidefsky (schwidefsky@de.ibm.com),
 */

#ifndef _S390_STRING_H_
#define _S390_STRING_H_

#ifdef __KERNEL__

#ifndef _LINUX_TYPES_H
#include <linux/types.h>
#endif

#define __HAVE_ARCH_MEMCHR
#define __HAVE_ARCH_MEMCPY
#define __HAVE_ARCH_MEMSET
#define __HAVE_ARCH_STRCAT
#define __HAVE_ARCH_STRCMP
#define __HAVE_ARCH_STRCPY
#define __HAVE_ARCH_STRLEN
#define __HAVE_ARCH_STRNCPY

#undef __HAVE_ARCH_MEMMOVE
#undef __HAVE_ARCH_STRNICMP
#undef __HAVE_ARCH_STRNCAT
#undef __HAVE_ARCH_STRNCMP
#undef __HAVE_ARCH_STRCHR
#undef __HAVE_ARCH_STRRCHR
#undef __HAVE_ARCH_STRNLEN
#undef __HAVE_ARCH_STRSPN
#undef __HAVE_ARCH_STRPBRK
#undef __HAVE_ARCH_STRTOK
#undef __HAVE_ARCH_BCOPY
#undef __HAVE_ARCH_MEMCMP
#undef __HAVE_ARCH_MEMSCAN
#undef __HAVE_ARCH_STRSTR

extern void *memset(void *, int, size_t);

extern inline void * memchr(const void * cs,int c,size_t count)
{
    void *ptr;

    __asm__ __volatile__ ("   lr    0,%2\n"
                          "   la    %0,0(%3,%1)\n"
                          "0: srst  %0,%1\n"
                          "   jo    0b\n"
                          "   brc   13,1f\n"
                          "   slr   %0,%0\n"
                          "1:"
                          : "=a" (ptr) : "a" (cs), "d" (c), "d" (count)
                          : "cc", "0" );
    return ptr;
}

extern __inline__ char *strcpy(char *dest, const char *src)
{
    char *tmp = dest;

    __asm__ __volatile__ ("   sr    0,0\n"
                          "0: mvst  %0,%1\n"
                          "   jo    0b"
                          : "+&a" (dest), "+&a" (src) :
                          : "cc", "memory", "0" );
    return tmp;
}

extern __inline__ size_t strlen(const char *s)
{
    size_t len;

    __asm__ __volatile__ ("   sr    0,0\n"
                          "   lr    %0,%1\n"
                          "0: srst  0,%0\n"
                          "   jo    0b\n"
                          "   lr    %0,0\n"
                          "   sr    %0,%1"
                          : "=&a" (len) : "a" (s) 
                          : "cc", "0" );
    return len;
}

extern __inline__ char *strcat(char *dest, const char *src)
{
    char *tmp = dest;

    __asm__ __volatile__ ("   sr    0,0\n"
                          "0: srst  0,%0\n"
                          "   jo    0b\n"
                          "   lr    %0,0\n"
                          "   sr    0,0\n"
                          "1: mvst  %0,%1\n"
                          "   jo    1b"
                          : "+&a" (dest), "+&a" (src) :
                          : "cc", "memory", "0" );
    return tmp;
}


#endif /* __KERNEL__ */

#endif /* __S390_STRING_H_ */

