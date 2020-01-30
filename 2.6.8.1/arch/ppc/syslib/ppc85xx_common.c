/*
 * arch/ppc/syslib/ppc85xx_common.c
 *
 * MPC85xx support routines
 *
 * Maintainer: Kumar Gala <kumar.gala@freescale.com>
 *
 * Copyright 2004 Freescale Semiconductor Inc.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/init.h>

#include <asm/mpc85xx.h>
#include <asm/mmu.h>
#include <asm/ocp.h>

/* ************************************************************************ */
/* Return the value of CCSRBAR for the current board */

phys_addr_t
get_ccsrbar(void)
{
        return BOARD_CCSRBAR;
}

/* ************************************************************************ */
/* Update the 85xx OCP tables paddr field */
void
mpc85xx_update_paddr_ocp(struct ocp_device *dev, void *arg)
{
	phys_addr_t ccsrbar;
	if (arg) {
		ccsrbar = *(phys_addr_t *)arg;
		dev->def->paddr += ccsrbar;
	}
}

EXPORT_SYMBOL(get_ccsrbar);
