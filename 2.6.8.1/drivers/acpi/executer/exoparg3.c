
/******************************************************************************
 *
 * Module Name: exoparg3 - AML execution - opcodes with 3 arguments
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
#include <acpi/acinterp.h>
#include <acpi/acparser.h>
#include <acpi/amlcode.h>


#define _COMPONENT          ACPI_EXECUTER
	 ACPI_MODULE_NAME    ("exoparg3")


/*!
 * Naming convention for AML interpreter execution routines.
 *
 * The routines that begin execution of AML opcodes are named with a common
 * convention based upon the number of arguments, the number of target operands,
 * and whether or not a value is returned:
 *
 *      AcpiExOpcode_xA_yT_zR
 *
 * Where:
 *
 * xA - ARGUMENTS:    The number of arguments (input operands) that are
 *                    required for this opcode type (1 through 6 args).
 * yT - TARGETS:      The number of targets (output operands) that are required
 *                    for this opcode type (0, 1, or 2 targets).
 * zR - RETURN VALUE: Indicates whether this opcode type returns a value
 *                    as the function return (0 or 1).
 *
 * The AcpiExOpcode* functions are called via the Dispatcher component with
 * fully resolved operands.
!*/


/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_opcode_3A_0T_0R
 *
 * PARAMETERS:  walk_state          - Current walk state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute Triadic operator (3 operands)
 *
 ******************************************************************************/

acpi_status
acpi_ex_opcode_3A_0T_0R (
	struct acpi_walk_state          *walk_state)
{
	union acpi_operand_object       **operand = &walk_state->operands[0];
	struct acpi_signal_fatal_info   *fatal;
	acpi_status                     status = AE_OK;


	ACPI_FUNCTION_TRACE_STR ("ex_opcode_3A_0T_0R", acpi_ps_get_opcode_name (walk_state->opcode));


	switch (walk_state->opcode) {
	case AML_FATAL_OP:          /* Fatal (fatal_type fatal_code fatal_arg)   */

		ACPI_DEBUG_PRINT ((ACPI_DB_INFO,
			"fatal_op: Type %X Code %X Arg %X <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<\n",
			(u32) operand[0]->integer.value,
			(u32) operand[1]->integer.value,
			(u32) operand[2]->integer.value));

		fatal = ACPI_MEM_ALLOCATE (sizeof (struct acpi_signal_fatal_info));
		if (fatal) {
			fatal->type     = (u32) operand[0]->integer.value;
			fatal->code     = (u32) operand[1]->integer.value;
			fatal->argument = (u32) operand[2]->integer.value;
		}

		/*
		 * Always signal the OS!
		 */
		status = acpi_os_signal (ACPI_SIGNAL_FATAL, fatal);

		/* Might return while OS is shutting down, just continue */

		ACPI_MEM_FREE (fatal);
		break;


	default:

		ACPI_REPORT_ERROR (("acpi_ex_opcode_3A_0T_0R: Unknown opcode %X\n",
				walk_state->opcode));
		status = AE_AML_BAD_OPCODE;
		goto cleanup;
	}


cleanup:

	return_ACPI_STATUS (status);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ex_opcode_3A_1T_1R
 *
 * PARAMETERS:  walk_state          - Current walk state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Execute Triadic operator (3 operands)
 *
 ******************************************************************************/

acpi_status
acpi_ex_opcode_3A_1T_1R (
	struct acpi_walk_state          *walk_state)
{
	union acpi_operand_object       **operand = &walk_state->operands[0];
	union acpi_operand_object       *return_desc = NULL;
	char                            *buffer;
	acpi_status                     status = AE_OK;
	acpi_native_uint                index;
	acpi_size                       length;


	ACPI_FUNCTION_TRACE_STR ("ex_opcode_3A_1T_1R", acpi_ps_get_opcode_name (walk_state->opcode));


	switch (walk_state->opcode) {
	case AML_MID_OP:        /* Mid  (Source[0], Index[1], Length[2], Result[3]) */

		/*
		 * Create the return object.  The Source operand is guaranteed to be
		 * either a String or a Buffer, so just use its type.
		 */
		return_desc = acpi_ut_create_internal_object (ACPI_GET_OBJECT_TYPE (operand[0]));
		if (!return_desc) {
			status = AE_NO_MEMORY;
			goto cleanup;
		}

		/* Get the Integer values from the objects */

		index = (acpi_native_uint) operand[1]->integer.value;
		length = (acpi_size) operand[2]->integer.value;

		/*
		 * If the index is beyond the length of the String/Buffer, or if the
		 * requested length is zero, return a zero-length String/Buffer
		 */
		if ((index < operand[0]->string.length) &&
			(length > 0)) {
			/* Truncate request if larger than the actual String/Buffer */

			if ((index + length) >
				operand[0]->string.length) {
				length = (acpi_size) operand[0]->string.length - index;
			}

			/* Allocate a new buffer for the String/Buffer */

			buffer = ACPI_MEM_CALLOCATE ((acpi_size) length + 1);
			if (!buffer) {
				status = AE_NO_MEMORY;
				goto cleanup;
			}

			/* Copy the portion requested */

			ACPI_MEMCPY (buffer, operand[0]->string.pointer + index,
					  length);

			/* Set the length of the new String/Buffer */

			return_desc->string.pointer = buffer;
			return_desc->string.length = (u32) length;
		}
		break;


	default:

		ACPI_REPORT_ERROR (("acpi_ex_opcode_3A_0T_0R: Unknown opcode %X\n",
				walk_state->opcode));
		status = AE_AML_BAD_OPCODE;
		goto cleanup;
	}

	/* Store the result in the target */

	status = acpi_ex_store (return_desc, operand[3], walk_state);

cleanup:

	/* Delete return object on error */

	if (ACPI_FAILURE (status)) {
		acpi_ut_remove_reference (return_desc);
	}

	/* Set the return object and exit */

	if (!walk_state->result_obj) {
		walk_state->result_obj = return_desc;
	}
	return_ACPI_STATUS (status);
}


