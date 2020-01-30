
/******************************************************************************
 *
 * Module Name: exutils - interpreter/scanner utilities
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


/*
 * DEFINE_AML_GLOBALS is tested in amlcode.h
 * to determine whether certain global names should be "defined" or only
 * "declared" in the current compilation.  This enhances maintainability
 * by enabling a single header file to embody all knowledge of the names
 * in question.
 *
 * Exactly one module of any executable should #define DEFINE_GLOBALS
 * before #including the header files which use this convention.  The
 * names in question will be defined and initialized in that module,
 * and declared as extern in all other modules which #include those
 * header files.
 */

#define DEFINE_AML_GLOBALS

#include <acpi/acpi.h>
#include <acpi/acinterp.h>
#include <acpi/amlcode.h>
#include <acpi/acevents.h>

#define _COMPONENT          ACPI_EXECUTER
	 ACPI_MODULE_NAME    ("exutils")


#ifndef ACPI_NO_METHOD_EXECUTION

/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_enter_interpreter
 *
 * PARAMETERS:  None
 *
 * DESCRIPTION: Enter the interpreter execution region.  Failure to enter
 *              the interpreter region is a fatal system error
 *
 ******************************************************************************/

acpi_status
acpi_ex_enter_interpreter (void)
{
	acpi_status                     status;

	ACPI_FUNCTION_TRACE ("ex_enter_interpreter");


	status = acpi_ut_acquire_mutex (ACPI_MTX_EXECUTE);
	if (ACPI_FAILURE (status)) {
		ACPI_REPORT_ERROR (("Could not acquire interpreter mutex\n"));
	}

	return_ACPI_STATUS (status);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_exit_interpreter
 *
 * PARAMETERS:  None
 *
 * DESCRIPTION: Exit the interpreter execution region
 *
 * Cases where the interpreter is unlocked:
 *      1) Completion of the execution of a control method
 *      2) Method blocked on a Sleep() AML opcode
 *      3) Method blocked on an Acquire() AML opcode
 *      4) Method blocked on a Wait() AML opcode
 *      5) Method blocked to acquire the global lock
 *      6) Method blocked to execute a serialized control method that is
 *          already executing
 *      7) About to invoke a user-installed opregion handler
 *
 ******************************************************************************/

void
acpi_ex_exit_interpreter (void)
{
	acpi_status                     status;


	ACPI_FUNCTION_TRACE ("ex_exit_interpreter");


	status = acpi_ut_release_mutex (ACPI_MTX_EXECUTE);
	if (ACPI_FAILURE (status)) {
		ACPI_REPORT_ERROR (("Could not release interpreter mutex\n"));
	}

	return_VOID;
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_truncate_for32bit_table
 *
 * PARAMETERS:  obj_desc        - Object to be truncated
 *
 * RETURN:      none
 *
 * DESCRIPTION: Truncate a number to 32-bits if the currently executing method
 *              belongs to a 32-bit ACPI table.
 *
 ******************************************************************************/

void
acpi_ex_truncate_for32bit_table (
	union acpi_operand_object       *obj_desc)
{

	ACPI_FUNCTION_ENTRY ();


	/*
	 * Object must be a valid number and we must be executing
	 * a control method
	 */
	if ((!obj_desc) ||
		(ACPI_GET_OBJECT_TYPE (obj_desc) != ACPI_TYPE_INTEGER)) {
		return;
	}

	if (acpi_gbl_integer_byte_width == 4) {
		/*
		 * We are running a method that exists in a 32-bit ACPI table.
		 * Truncate the value to 32 bits by zeroing out the upper 32-bit field
		 */
		obj_desc->integer.value &= (acpi_integer) ACPI_UINT32_MAX;
	}
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_acquire_global_lock
 *
 * PARAMETERS:  field_flags           - Flags with Lock rule:
 *                                      always_lock or never_lock
 *
 * RETURN:      TRUE/FALSE indicating whether the lock was actually acquired
 *
 * DESCRIPTION: Obtain the global lock and keep track of this fact via two
 *              methods.  A global variable keeps the state of the lock, and
 *              the state is returned to the caller.
 *
 ******************************************************************************/

u8
acpi_ex_acquire_global_lock (
	u32                             field_flags)
{
	u8                              locked = FALSE;
	acpi_status                     status;


	ACPI_FUNCTION_TRACE ("ex_acquire_global_lock");


	/* Only attempt lock if the always_lock bit is set */

	if (field_flags & AML_FIELD_LOCK_RULE_MASK) {
		/* We should attempt to get the lock, wait forever */

		status = acpi_ev_acquire_global_lock (ACPI_WAIT_FOREVER);
		if (ACPI_SUCCESS (status)) {
			locked = TRUE;
		}
		else {
			ACPI_DEBUG_PRINT ((ACPI_DB_ERROR, "Could not acquire Global Lock, %s\n",
				acpi_format_exception (status)));
		}
	}

	return_VALUE (locked);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_release_global_lock
 *
 * PARAMETERS:  locked_by_me    - Return value from corresponding call to
 *                                acquire_global_lock.
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Release the global lock if it is locked.
 *
 ******************************************************************************/

void
acpi_ex_release_global_lock (
	u8                              locked_by_me)
{
	acpi_status                     status;


	ACPI_FUNCTION_TRACE ("ex_release_global_lock");


	/* Only attempt unlock if the caller locked it */

	if (locked_by_me) {
		/* OK, now release the lock */

		status = acpi_ev_release_global_lock ();
		if (ACPI_FAILURE (status)) {
			/* Report the error, but there isn't much else we can do */

			ACPI_REPORT_ERROR (("Could not release ACPI Global Lock, %s\n",
				acpi_format_exception (status)));
		}
	}

	return_VOID;
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_digits_needed
 *
 * PARAMETERS:  Value           - Value to be represented
 *              Base            - Base of representation
 *
 * RETURN:      the number of digits needed to represent Value in Base
 *
 ******************************************************************************/

u32
acpi_ex_digits_needed (
	acpi_integer                    value,
	u32                             base)
{
	u32                             num_digits;
	acpi_integer                    current_value;
	acpi_integer                    quotient;


	ACPI_FUNCTION_TRACE ("ex_digits_needed");


	/*
	 * acpi_integer is unsigned, so we don't worry about a '-'
	 */
	if ((current_value = value) == 0) {
		return_VALUE (1);
	}

	num_digits = 0;

	while (current_value) {
		(void) acpi_ut_short_divide (&current_value, base, &quotient, NULL);
		num_digits++;
		current_value = quotient;
	}

	return_VALUE (num_digits);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_eisa_id_to_string
 *
 * PARAMETERS:  numeric_id      - EISA ID to be converted
 *              out_string      - Where to put the converted string (8 bytes)
 *
 * DESCRIPTION: Convert a numeric EISA ID to string representation
 *
 ******************************************************************************/

void
acpi_ex_eisa_id_to_string (
	u32                             numeric_id,
	char                            *out_string)
{
	u32                             eisa_id;


	ACPI_FUNCTION_ENTRY ();


	/* Swap ID to big-endian to get contiguous bits */

	eisa_id = acpi_ut_dword_byte_swap (numeric_id);

	out_string[0] = (char) ('@' + (((unsigned long) eisa_id >> 26) & 0x1f));
	out_string[1] = (char) ('@' + ((eisa_id >> 21) & 0x1f));
	out_string[2] = (char) ('@' + ((eisa_id >> 16) & 0x1f));
	out_string[3] = acpi_ut_hex_to_ascii_char ((acpi_integer) eisa_id, 12);
	out_string[4] = acpi_ut_hex_to_ascii_char ((acpi_integer) eisa_id, 8);
	out_string[5] = acpi_ut_hex_to_ascii_char ((acpi_integer) eisa_id, 4);
	out_string[6] = acpi_ut_hex_to_ascii_char ((acpi_integer) eisa_id, 0);
	out_string[7] = 0;
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_unsigned_integer_to_string
 *
 * PARAMETERS:  Value           - Value to be converted
 *              out_string      - Where to put the converted string (8 bytes)
 *
 * RETURN:      Convert a number to string representation
 *
 ******************************************************************************/

void
acpi_ex_unsigned_integer_to_string (
	acpi_integer                    value,
	char                            *out_string)
{
	u32                             count;
	u32                             digits_needed;
	u32                             remainder;
	acpi_integer                    quotient;


	ACPI_FUNCTION_ENTRY ();


	digits_needed = acpi_ex_digits_needed (value, 10);
	out_string[digits_needed] = 0;

	for (count = digits_needed; count > 0; count--) {
		(void) acpi_ut_short_divide (&value, 10, &quotient, &remainder);
		out_string[count-1] = (char) ('0' + remainder);\
		value = quotient;
	}
}

#endif
