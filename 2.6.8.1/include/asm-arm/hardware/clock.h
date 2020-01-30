/*
 *  linux/include/asm-arm/hardware/clock.h
 *
 *  Copyright (C) 2004 ARM Limited.
 *  Written by Deep Blue Solutions Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef ASMARM_CLOCK_H
#define ASMARM_CLOCK_H

struct device;

/*
 * The base API.
 */


/*
 * struct clk - an machine class defined object / cookie.
 */
struct clk;

/**
 * clk_get - lookup and obtain a reference to a clock producer.
 * @dev: device for clock "consumer"
 * @id: device ID
 *
 * Returns a struct clk corresponding to the clock producer, or
 * valid IS_ERR() condition containing errno.
 */
struct clk *clk_get(struct device *dev, const char *id);

/**
 * clk_enable - inform the system when the clock source should be running.
 * @clk: clock source
 *
 * If the clock can not be enabled/disabled, this should return success.
 *
 * Returns success (0) or negative errno.
 */
int clk_enable(struct clk *clk);

/**
 * clk_disable - inform the system when the clock source is no longer required.
 * @clk: clock source
 */
void clk_disable(struct clk *clk);

/**
 * clk_use - increment the use count
 * @clk: clock source
 *
 * Returns success (0) or negative errno.
 */
int clk_use(struct clk *clk);

/**
 * clk_unuse - decrement the use count
 * @clk: clock source
 */
void clk_unuse(struct clk *clk);

/**
 * clk_get_rate - obtain the current clock rate for a clock source.
 *		  This is only valid once the clock source has been enabled.
 * @clk: clock source
 */
unsigned long clk_get_rate(struct clk *clk);

/**
 * clk_put	- "free" the clock source
 * @clk: clock source
 */
void clk_put(struct clk *clk);


/*
 * The remaining APIs are optional for machine class support.
 */


/**
 * clk_round_rate - adjust a rate to the exact rate a clock can provide
 * @clk: clock source
 * @rate: desired clock rate in kHz
 *
 * Returns rounded clock rate, or negative errno.
 */
long clk_round_rate(struct clk *clk, unsigned long rate);
 
/**
 * clk_set_rate - set the clock rate for a clock source
 * @clk: clock source
 * @rate: desired clock rate in kHz
 *
 * Returns success (0) or negative errno.
 */
int clk_set_rate(struct clk *clk, unsigned long rate);
 
/**
 * clk_set_parent - set the parent clock source for this clock
 * @clk: clock source
 * @parent: parent clock source
 *
 * Returns success (0) or negative errno.
 */
int clk_set_parent(struct clk *clk, struct clk *parent);

/**
 * clk_get_parent - get the parent clock source for this clock
 * @clk: clock source
 *
 * Returns struct clk corresponding to parent clock source, or
 * valid IS_ERR() condition containing errno.
 */
struct clk *clk_get_parent(struct clk *clk);

#endif
