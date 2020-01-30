#ifndef _ASM_POWERPC_OF_PLATFORM_H
#define _ASM_POWERPC_OF_PLATFORM_H
/*
 *    Copyright (C) 2006 Benjamin Herrenschmidt, IBM Corp.
 *			 <benh@kernel.crashing.org>
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version
 *  2 of the License, or (at your option) any later version.
 *
 */

/* This is just here during the transition */
#include <linux/of_platform.h>

/* Platform drivers register/unregister */
extern int of_register_platform_driver(struct of_platform_driver *drv);
extern void of_unregister_platform_driver(struct of_platform_driver *drv);

/* Platform devices and busses creation */
extern struct of_device *of_platform_device_create(struct device_node *np,
						   const char *bus_id,
						   struct device *parent);
/* pseudo "matches" value to not do deep probe */
#define OF_NO_DEEP_PROBE ((struct of_device_id *)-1)

extern int of_platform_bus_probe(struct device_node *root,
				 struct of_device_id *matches,
				 struct device *parent);

extern struct of_device *of_find_device_by_phandle(phandle ph);

#endif	/* _ASM_POWERPC_OF_PLATFORM_H */
