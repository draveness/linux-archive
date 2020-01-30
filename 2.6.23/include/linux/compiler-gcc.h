/* Never include this file directly.  Include <linux/compiler.h> instead.  */

/*
 * Common definitions for all gcc versions go here.
 */


/* Optimization barrier */
/* The "volatile" is due to gcc bugs */
#define barrier() __asm__ __volatile__("": : :"memory")

/* This macro obfuscates arithmetic on a variable address so that gcc
   shouldn't recognize the original var, and make assumptions about it */
/*
 * Versions of the ppc64 compiler before 4.1 had a bug where use of
 * RELOC_HIDE could trash r30. The bug can be worked around by changing
 * the inline assembly constraint from =g to =r, in this particular
 * case either is valid.
 */
#define RELOC_HIDE(ptr, off)					\
  ({ unsigned long __ptr;					\
    __asm__ ("" : "=r"(__ptr) : "0"(ptr));		\
    (typeof(ptr)) (__ptr + (off)); })

/* &a[0] degrades to a pointer: a different type from an array */
#define __must_be_array(a) \
  BUILD_BUG_ON_ZERO(__builtin_types_compatible_p(typeof(a), typeof(&a[0])))

#define inline		inline		__attribute__((always_inline))
#define __inline__	__inline__	__attribute__((always_inline))
#define __inline	__inline	__attribute__((always_inline))
#define __deprecated			__attribute__((deprecated))
#define __packed			__attribute__((packed))
#define __weak				__attribute__((weak))
#define __naked				__attribute__((naked))
#define __noreturn			__attribute__((noreturn))
#define __pure				__attribute__((pure))
#define __aligned(x)			__attribute__((aligned(x)))
#define __printf(a,b)			__attribute__((format(printf,a,b)))
#define  noinline			__attribute__((noinline))
#define __attribute_pure__		__attribute__((pure))
#define __attribute_const__		__attribute__((__const__))
#define __maybe_unused			__attribute__((unused))
