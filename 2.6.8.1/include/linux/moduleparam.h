#ifndef _LINUX_MODULE_PARAMS_H
#define _LINUX_MODULE_PARAMS_H
/* (C) Copyright 2001, 2002 Rusty Russell IBM Corporation */
#include <linux/init.h>
#include <linux/stringify.h>
#include <linux/kernel.h>

/* You can override this manually, but generally this should match the
   module name. */
#ifdef MODULE
#define MODULE_PARAM_PREFIX /* empty */
#else
#define MODULE_PARAM_PREFIX __stringify(KBUILD_MODNAME) "."
#endif

struct kernel_param;

/* Returns 0, or -errno.  arg is in kp->arg. */
typedef int (*param_set_fn)(const char *val, struct kernel_param *kp);
/* Returns length written or -errno.  Buffer is 4k (ie. be short!) */
typedef int (*param_get_fn)(char *buffer, struct kernel_param *kp);

struct kernel_param {
	const char *name;
	unsigned int perm;
	param_set_fn set;
	param_get_fn get;
	void *arg;
};

/* Special one for strings we want to copy into */
struct kparam_string {
	unsigned int maxlen;
	char *string;
};

/* Special one for arrays */
struct kparam_array
{
	unsigned int max;
	unsigned int *num;
	param_set_fn set;
	param_get_fn get;
	unsigned int elemsize;
	void *elem;
};

/* This is the fundamental function for registering boot/module
   parameters.  perm sets the visibility in driverfs: 000 means it's
   not there, read bits mean it's readable, write bits mean it's
   writable. */
#define __module_param_call(prefix, name, set, get, arg, perm)		\
	static char __param_str_##name[] = prefix #name;		\
	static struct kernel_param const __param_##name			\
	__attribute_used__						\
    __attribute__ ((unused,__section__ ("__param"),aligned(sizeof(void *)))) \
	= { __param_str_##name, perm, set, get, arg }

#define module_param_call(name, set, get, arg, perm)			      \
	__module_param_call(MODULE_PARAM_PREFIX, name, set, get, arg, perm)

/* Helper functions: type is byte, short, ushort, int, uint, long,
   ulong, charp, bool or invbool, or XXX if you define param_get_XXX,
   param_set_XXX and param_check_XXX. */
#define module_param_named(name, value, type, perm)			   \
	param_check_##type(name, &(value));				   \
	module_param_call(name, param_set_##type, param_get_##type, &value, perm)

#define module_param(name, type, perm)				\
	module_param_named(name, name, type, perm)

/* Actually copy string: maxlen param is usually sizeof(string). */
#define module_param_string(name, string, len, perm)			\
	static struct kparam_string __param_string_##name		\
		= { len, string };					\
	module_param_call(name, param_set_copystring, param_get_charp,	\
		   &__param_string_##name, perm)

/* Called on module insert or kernel boot */
extern int parse_args(const char *name,
		      char *args,
		      struct kernel_param *params,
		      unsigned num,
		      int (*unknown)(char *param, char *val));

/* All the helper functions */
/* The macros to do compile-time type checking stolen from Jakub
   Jelinek, who IIRC came up with this idea for the 2.4 module init code. */
#define __param_check(name, p, type) \
	static inline type *__check_##name(void) { return(p); }

extern int param_set_short(const char *val, struct kernel_param *kp);
extern int param_get_short(char *buffer, struct kernel_param *kp);
#define param_check_short(name, p) __param_check(name, p, short)

extern int param_set_ushort(const char *val, struct kernel_param *kp);
extern int param_get_ushort(char *buffer, struct kernel_param *kp);
#define param_check_ushort(name, p) __param_check(name, p, unsigned short)

extern int param_set_int(const char *val, struct kernel_param *kp);
extern int param_get_int(char *buffer, struct kernel_param *kp);
#define param_check_int(name, p) __param_check(name, p, int)

extern int param_set_uint(const char *val, struct kernel_param *kp);
extern int param_get_uint(char *buffer, struct kernel_param *kp);
#define param_check_uint(name, p) __param_check(name, p, unsigned int)

extern int param_set_long(const char *val, struct kernel_param *kp);
extern int param_get_long(char *buffer, struct kernel_param *kp);
#define param_check_long(name, p) __param_check(name, p, long)

extern int param_set_ulong(const char *val, struct kernel_param *kp);
extern int param_get_ulong(char *buffer, struct kernel_param *kp);
#define param_check_ulong(name, p) __param_check(name, p, unsigned long)

extern int param_set_charp(const char *val, struct kernel_param *kp);
extern int param_get_charp(char *buffer, struct kernel_param *kp);
#define param_check_charp(name, p) __param_check(name, p, char *)

extern int param_set_bool(const char *val, struct kernel_param *kp);
extern int param_get_bool(char *buffer, struct kernel_param *kp);
#define param_check_bool(name, p) __param_check(name, p, int)

extern int param_set_invbool(const char *val, struct kernel_param *kp);
extern int param_get_invbool(char *buffer, struct kernel_param *kp);
#define param_check_invbool(name, p) __param_check(name, p, int)

/* Comma-separated array: num is set to number they actually specified. */
#define module_param_array_named(name, array, type, num, perm)		\
	static struct kparam_array __param_arr_##name			\
	= { ARRAY_SIZE(array), &num, param_set_##type, param_get_##type,\
	    sizeof(array[0]), array };					\
	module_param_call(name, param_array_set, param_array_get, 	\
			  &__param_arr_##name, perm)

#define module_param_array(name, type, num, perm)		\
	module_param_array_named(name, name, type, num, perm)

extern int param_array_set(const char *val, struct kernel_param *kp);
extern int param_array_get(char *buffer, struct kernel_param *kp);

extern int param_set_copystring(const char *val, struct kernel_param *kp);

int param_array(const char *name,
		const char *val,
		unsigned int min, unsigned int max,
		void *elem, int elemsize,
		int (*set)(const char *, struct kernel_param *kp),
		int *num);
#endif /* _LINUX_MODULE_PARAMS_H */
