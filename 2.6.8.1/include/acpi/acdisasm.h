/******************************************************************************
 *
 * Name: acdisasm.h - AML disassembler
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

#ifndef __ACDISASM_H__
#define __ACDISASM_H__

#include "amlresrc.h"


#define BLOCK_NONE              0
#define BLOCK_PAREN             1
#define BLOCK_BRACE             2
#define BLOCK_COMMA_LIST        4

extern const char                       *acpi_gbl_io_decode[2];
extern const char                       *acpi_gbl_word_decode[4];
extern const char                       *acpi_gbl_consume_decode[2];
extern const char                       *acpi_gbl_min_decode[2];
extern const char                       *acpi_gbl_max_decode[2];
extern const char                       *acpi_gbl_DECdecode[2];
extern const char                       *acpi_gbl_RNGdecode[4];
extern const char                       *acpi_gbl_MEMdecode[4];
extern const char                       *acpi_gbl_RWdecode[2];
extern const char                       *acpi_gbl_irq_decode[2];
extern const char                       *acpi_gbl_HEdecode[2];
extern const char                       *acpi_gbl_LLdecode[2];
extern const char                       *acpi_gbl_SHRdecode[2];
extern const char                       *acpi_gbl_TYPdecode[4];
extern const char                       *acpi_gbl_BMdecode[2];
extern const char                       *acpi_gbl_SIZdecode[4];
extern const char                       *acpi_gbl_lock_rule[ACPI_NUM_LOCK_RULES];
extern const char                       *acpi_gbl_access_types[ACPI_NUM_ACCESS_TYPES];
extern const char                       *acpi_gbl_update_rules[ACPI_NUM_UPDATE_RULES];
extern const char                       *acpi_gbl_match_ops[ACPI_NUM_MATCH_OPS];


struct acpi_op_walk_info
{
	u32                             level;
	u32                             bit_offset;
};

typedef
acpi_status (*asl_walk_callback) (
	union acpi_parse_object             *op,
	u32                                 level,
	void                                *context);


/*
 * dmwalk
 */

void
acpi_dm_walk_parse_tree (
	union acpi_parse_object         *op,
	asl_walk_callback               descending_callback,
	asl_walk_callback               ascending_callback,
	void                            *context);

acpi_status
acpi_dm_descending_op (
	union acpi_parse_object         *op,
	u32                             level,
	void                            *context);

acpi_status
acpi_dm_ascending_op (
	union acpi_parse_object         *op,
	u32                             level,
	void                            *context);


/*
 * dmopcode
 */

void
acpi_dm_validate_name (
	char                            *name,
	union acpi_parse_object         *op);

u32
acpi_dm_dump_name (
	char                            *name);

void
acpi_dm_unicode (
	union acpi_parse_object         *op);

void
acpi_dm_disassemble (
	struct acpi_walk_state          *walk_state,
	union acpi_parse_object         *origin,
	u32                             num_opcodes);

void
acpi_dm_namestring (
	char                            *name);

void
acpi_dm_display_path (
	union acpi_parse_object         *op);

void
acpi_dm_disassemble_one_op (
	struct acpi_walk_state          *walk_state,
	struct acpi_op_walk_info        *info,
	union acpi_parse_object         *op);

void
acpi_dm_decode_internal_object (
	union acpi_operand_object       *obj_desc);

u32
acpi_dm_block_type (
	union acpi_parse_object         *op);

u32
acpi_dm_list_type (
	union acpi_parse_object         *op);

acpi_status
acpi_ps_display_object_pathname (
	struct acpi_walk_state          *walk_state,
	union acpi_parse_object         *op);

void
acpi_dm_method_flags (
	union acpi_parse_object         *op);

void
acpi_dm_field_flags (
	union acpi_parse_object         *op);

void
acpi_dm_address_space (
	u8                              space_id);

void
acpi_dm_region_flags (
	union acpi_parse_object         *op);

void
acpi_dm_match_op (
	union acpi_parse_object         *op);

void
acpi_dm_match_keyword (
	union acpi_parse_object         *op);

u8
acpi_dm_comma_if_list_member (
	union acpi_parse_object         *op);

void
acpi_dm_comma_if_field_member (
	union acpi_parse_object         *op);


/*
 * dmobject
 */

void
acpi_dm_decode_node (
	struct acpi_namespace_node      *node);

void
acpi_dm_display_internal_object (
	union acpi_operand_object       *obj_desc,
	struct acpi_walk_state          *walk_state);

void
acpi_dm_display_arguments (
	struct acpi_walk_state          *walk_state);

void
acpi_dm_display_locals (
	struct acpi_walk_state          *walk_state);

void
acpi_dm_dump_method_info (
	acpi_status                     status,
	struct acpi_walk_state          *walk_state,
	union acpi_parse_object         *op);


/*
 * dmbuffer
 */

void
acpi_is_eisa_id (
	union acpi_parse_object         *op);

void
acpi_dm_eisa_id (
	u32                             encoded_id);

u8
acpi_dm_is_unicode_buffer (
	union acpi_parse_object         *op);

u8
acpi_dm_is_string_buffer (
	union acpi_parse_object         *op);


/*
 * dmresrc
 */

void
acpi_dm_disasm_byte_list (
	u32                             level,
	u8                              *byte_data,
	u32                             byte_count);

void
acpi_dm_byte_list (
	struct acpi_op_walk_info        *info,
	union acpi_parse_object         *op);

void
acpi_dm_resource_descriptor (
	struct acpi_op_walk_info        *info,
	u8                              *byte_data,
	u32                             byte_count);

u8
acpi_dm_is_resource_descriptor (
	union acpi_parse_object         *op);

void
acpi_dm_indent (
	u32                             level);

void
acpi_dm_bit_list (
	u16                             mask);

void
acpi_dm_decode_attribute (
	u8                              attribute);

/*
 * dmresrcl
 */

void
acpi_dm_io_flags (
		u8                          flags);

void
acpi_dm_memory_flags (
	u8                              flags,
	u8                              specific_flags);

void
acpi_dm_word_descriptor (
	struct asl_word_address_desc    *resource,
	u32                             length,
	u32                             level);

void
acpi_dm_dword_descriptor (
	struct asl_dword_address_desc   *resource,
	u32                             length,
	u32                             level);

void
acpi_dm_qword_descriptor (
	struct asl_qword_address_desc   *resource,
	u32                             length,
	u32                             level);

void
acpi_dm_memory24_descriptor (
	struct asl_memory_24_desc       *resource,
	u32                             length,
	u32                             level);

void
acpi_dm_memory32_descriptor (
	struct asl_memory_32_desc       *resource,
	u32                             length,
	u32                             level);

void
acpi_dm_fixed_mem32_descriptor (
	struct asl_fixed_memory_32_desc *resource,
	u32                             length,
	u32                             level);

void
acpi_dm_generic_register_descriptor (
	struct asl_general_register_desc *resource,
	u32                             length,
	u32                             level);

void
acpi_dm_interrupt_descriptor (
	struct asl_extended_xrupt_desc *resource,
	u32                             length,
	u32                             level);

void
acpi_dm_vendor_large_descriptor (
	struct asl_large_vendor_desc    *resource,
	u32                             length,
	u32                             level);


/*
 * dmresrcs
 */

void
acpi_dm_irq_descriptor (
	struct asl_irq_format_desc      *resource,
	u32                             length,
	u32                             level);

void
acpi_dm_dma_descriptor (
	struct asl_dma_format_desc      *resource,
	u32                             length,
	u32                             level);

void
acpi_dm_io_descriptor (
	struct asl_io_port_desc         *resource,
	u32                             length,
	u32                             level);

void
acpi_dm_fixed_io_descriptor (
	struct asl_fixed_io_port_desc   *resource,
	u32                             length,
	u32                             level);

void
acpi_dm_start_dependent_descriptor (
	struct asl_start_dependent_desc *resource,
	u32                             length,
	u32                             level);

void
acpi_dm_end_dependent_descriptor (
	struct asl_start_dependent_desc *resource,
	u32                             length,
	u32                             level);

void
acpi_dm_vendor_small_descriptor (
	struct asl_small_vendor_desc    *resource,
	u32                             length,
	u32                             level);


#endif  /* __ACDISASM_H__ */
