/******************************************************************************
 *
 * Module Name: tbutils - Table manipulation utilities
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
#include <acpi/actables.h>


#define _COMPONENT          ACPI_TABLES
	 ACPI_MODULE_NAME    ("tbutils")


/*******************************************************************************
 *
 * FUNCTION:    acpi_tb_handle_to_object
 *
 * PARAMETERS:  table_id            - Id for which the function is searching
 *              table_desc          - Pointer to return the matching table
 *                                      descriptor.
 *
 * RETURN:      Search the tables to find one with a matching table_id and
 *              return a pointer to that table descriptor.
 *
 ******************************************************************************/

acpi_status
acpi_tb_handle_to_object (
	u16                             table_id,
	struct acpi_table_desc          **return_table_desc)
{
	u32                             i;
	struct acpi_table_desc          *table_desc;


	ACPI_FUNCTION_NAME ("tb_handle_to_object");


	for (i = 0; i < ACPI_TABLE_MAX; i++) {
		table_desc = acpi_gbl_table_lists[i].next;
		while (table_desc) {
			if (table_desc->table_id == table_id) {
				*return_table_desc = table_desc;
				return (AE_OK);
			}

			table_desc = table_desc->next;
		}
	}

	ACPI_DEBUG_PRINT ((ACPI_DB_ERROR, "table_id=%X does not exist\n", table_id));
	return (AE_BAD_PARAMETER);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_tb_validate_table_header
 *
 * PARAMETERS:  table_header        - Logical pointer to the table
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Check an ACPI table header for validity
 *
 * NOTE:  Table pointers are validated as follows:
 *          1) Table pointer must point to valid physical memory
 *          2) Signature must be 4 ASCII chars, even if we don't recognize the
 *             name
 *          3) Table must be readable for length specified in the header
 *          4) Table checksum must be valid (with the exception of the FACS
 *              which has no checksum because it contains variable fields)
 *
 ******************************************************************************/

acpi_status
acpi_tb_validate_table_header (
	struct acpi_table_header        *table_header)
{
	acpi_name                       signature;


	ACPI_FUNCTION_NAME ("tb_validate_table_header");


	/* Verify that this is a valid address */

	if (!acpi_os_readable (table_header, sizeof (struct acpi_table_header))) {
		ACPI_DEBUG_PRINT ((ACPI_DB_ERROR,
			"Cannot read table header at %p\n", table_header));
		return (AE_BAD_ADDRESS);
	}

	/* Ensure that the signature is 4 ASCII characters */

	ACPI_MOVE_32_TO_32 (&signature, table_header->signature);
	if (!acpi_ut_valid_acpi_name (signature)) {
		ACPI_DEBUG_PRINT ((ACPI_DB_ERROR,
			"Table signature at %p [%p] has invalid characters\n",
			table_header, &signature));

		ACPI_REPORT_WARNING (("Invalid table signature found: [%4.4s]\n",
			(char *) &signature));
		ACPI_DUMP_BUFFER (table_header, sizeof (struct acpi_table_header));
		return (AE_BAD_SIGNATURE);
	}

	/* Validate the table length */

	if (table_header->length < sizeof (struct acpi_table_header)) {
		ACPI_DEBUG_PRINT ((ACPI_DB_ERROR,
			"Invalid length in table header %p name %4.4s\n",
			table_header, (char *) &signature));

		ACPI_REPORT_WARNING (("Invalid table header length (0x%X) found\n",
			(u32) table_header->length));
		ACPI_DUMP_BUFFER (table_header, sizeof (struct acpi_table_header));
		return (AE_BAD_HEADER);
	}

	return (AE_OK);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_tb_verify_table_checksum
 *
 * PARAMETERS:  *table_header           - ACPI table to verify
 *
 * RETURN:      8 bit checksum of table
 *
 * DESCRIPTION: Does an 8 bit checksum of table and returns status.  A correct
 *              table should have a checksum of 0.
 *
 ******************************************************************************/

acpi_status
acpi_tb_verify_table_checksum (
	struct acpi_table_header        *table_header)
{
	u8                              checksum;
	acpi_status                     status = AE_OK;


	ACPI_FUNCTION_TRACE ("tb_verify_table_checksum");


	/* Compute the checksum on the table */

	checksum = acpi_tb_checksum (table_header, table_header->length);

	/* Return the appropriate exception */

	if (checksum) {
		ACPI_REPORT_WARNING (("Invalid checksum in table [%4.4s] (%02X, sum %02X is not zero)\n",
			table_header->signature, (u32) table_header->checksum, (u32) checksum));

		status = AE_BAD_CHECKSUM;
	}
	return_ACPI_STATUS (status);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_tb_checksum
 *
 * PARAMETERS:  Buffer              - Buffer to checksum
 *              Length              - Size of the buffer
 *
 * RETURNS      8 bit checksum of buffer
 *
 * DESCRIPTION: Computes an 8 bit checksum of the buffer(length) and returns it.
 *
 ******************************************************************************/

u8
acpi_tb_checksum (
	void                            *buffer,
	u32                             length)
{
	const u8                        *limit;
	const u8                        *rover;
	u8                              sum = 0;


	if (buffer && length) {
		/*  Buffer and Length are valid   */

		limit = (u8 *) buffer + length;

		for (rover = buffer; rover < limit; rover++) {
			sum = (u8) (sum + *rover);
		}
	}
	return (sum);
}


