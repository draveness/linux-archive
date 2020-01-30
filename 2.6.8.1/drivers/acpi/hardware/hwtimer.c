
/******************************************************************************
 *
 * Name: hwtimer.c - ACPI Power Management Timer Interface
 *
 *****************************************************************************/

/*
 * Copyright (C) 2000 - 2004, R. Byron Moore
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    substantially similar to the "NO WARRANTY" disclaimer below
 *    ("Disclaimer") and any redistribution must be conditioned upon
 *    including a substantially similar Disclaimer requirement for further
 *    binary redistribution.
 * 3. Neither the names of the above-listed copyright holders nor the names
 *    of any contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES.
 */

#include <acpi/acpi.h>

#define _COMPONENT          ACPI_HARDWARE
	 ACPI_MODULE_NAME    ("hwtimer")


/******************************************************************************
 *
 * FUNCTION:    acpi_get_timer_resolution
 *
 * PARAMETERS:  none
 *
 * RETURN:      Number of bits of resolution in the PM Timer (24 or 32).
 *
 * DESCRIPTION: Obtains resolution of the ACPI PM Timer.
 *
 ******************************************************************************/

acpi_status
acpi_get_timer_resolution (
	u32                             *resolution)
{
	ACPI_FUNCTION_TRACE ("acpi_get_timer_resolution");


	if (!resolution) {
		return_ACPI_STATUS (AE_BAD_PARAMETER);
	}

	if (0 == acpi_gbl_FADT->tmr_val_ext) {
		*resolution = 24;
	}
	else {
		*resolution = 32;
	}

	return_ACPI_STATUS (AE_OK);
}


/******************************************************************************
 *
 * FUNCTION:    acpi_get_timer
 *
 * PARAMETERS:  none
 *
 * RETURN:      Current value of the ACPI PM Timer (in ticks).
 *
 * DESCRIPTION: Obtains current value of ACPI PM Timer.
 *
 ******************************************************************************/

acpi_status
acpi_get_timer (
	u32                             *ticks)
{
	acpi_status                     status;


	ACPI_FUNCTION_TRACE ("acpi_get_timer");


	if (!ticks) {
		return_ACPI_STATUS (AE_BAD_PARAMETER);
	}

	status = acpi_hw_low_level_read (32, ticks, &acpi_gbl_FADT->xpm_tmr_blk);

	return_ACPI_STATUS (status);
}


/******************************************************************************
 *
 * FUNCTION:    acpi_get_timer_duration
 *
 * PARAMETERS:  start_ticks
 *              end_ticks
 *              time_elapsed
 *
 * RETURN:      time_elapsed
 *
 * DESCRIPTION: Computes the time elapsed (in microseconds) between two
 *              PM Timer time stamps, taking into account the possibility of
 *              rollovers, the timer resolution, and timer frequency.
 *
 *              The PM Timer's clock ticks at roughly 3.6 times per
 *              _microsecond_, and its clock continues through Cx state
 *              transitions (unlike many CPU timestamp counters) -- making it
 *              a versatile and accurate timer.
 *
 *              Note that this function accommodates only a single timer
 *              rollover.  Thus for 24-bit timers, this function should only
 *              be used for calculating durations less than ~4.6 seconds
 *              (~20 minutes for 32-bit timers) -- calculations below
 *
 *              2**24 Ticks / 3,600,000 Ticks/Sec = 4.66 sec
 *              2**32 Ticks / 3,600,000 Ticks/Sec = 1193 sec or 19.88 minutes
 *
 ******************************************************************************/

acpi_status
acpi_get_timer_duration (
	u32                             start_ticks,
	u32                             end_ticks,
	u32                             *time_elapsed)
{
	u32                             delta_ticks = 0;
	union uint64_overlay            normalized_ticks;
	acpi_status                     status;
	acpi_integer                    out_quotient;


	ACPI_FUNCTION_TRACE ("acpi_get_timer_duration");


	if (!time_elapsed) {
		return_ACPI_STATUS (AE_BAD_PARAMETER);
	}

	/*
	 * Compute Tick Delta:
	 * -------------------
	 * Handle (max one) timer rollovers on 24- versus 32-bit timers.
	 */
	if (start_ticks < end_ticks) {
		delta_ticks = end_ticks - start_ticks;
	}
	else if (start_ticks > end_ticks) {
		if (0 == acpi_gbl_FADT->tmr_val_ext) {
			/* 24-bit Timer */

			delta_ticks = (((0x00FFFFFF - start_ticks) + end_ticks) & 0x00FFFFFF);
		}
		else {
			/* 32-bit Timer */

			delta_ticks = (0xFFFFFFFF - start_ticks) + end_ticks;
		}
	}
	else {
		*time_elapsed = 0;
		return_ACPI_STATUS (AE_OK);
	}

	/*
	 * Compute Duration:
	 * -----------------
	 *
	 * Requires a 64-bit divide:
	 *
	 * time_elapsed = (delta_ticks * 1000000) / PM_TIMER_FREQUENCY;
	 */
	normalized_ticks.full = ((u64) delta_ticks) * 1000000;

	status = acpi_ut_short_divide (&normalized_ticks.full, PM_TIMER_FREQUENCY,
			   &out_quotient, NULL);

	*time_elapsed = (u32) out_quotient;
	return_ACPI_STATUS (status);
}


