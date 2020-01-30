/******************************************************************************
 *
 * Name: acinterp.h - Interpreter subcomponent prototypes and defines
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

#ifndef __ACINTERP_H__
#define __ACINTERP_H__


#define ACPI_WALK_OPERANDS       (&(walk_state->operands [walk_state->num_operands -1]))


acpi_status
acpi_ex_resolve_operands (
	u16                             opcode,
	union acpi_operand_object       **stack_ptr,
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_check_object_type (
	acpi_object_type                type_needed,
	acpi_object_type                this_type,
	void                            *object);

/*
 * exxface - External interpreter interfaces
 */

acpi_status
acpi_ex_load_table (
	acpi_table_type                 table_id);

acpi_status
acpi_ex_execute_method (
	struct acpi_namespace_node      *method_node,
	union acpi_operand_object       **params,
	union acpi_operand_object       **return_obj_desc);


/*
 * exconvrt - object conversion
 */

acpi_status
acpi_ex_convert_to_integer (
	union acpi_operand_object       *obj_desc,
	union acpi_operand_object       **result_desc,
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_convert_to_buffer (
	union acpi_operand_object       *obj_desc,
	union acpi_operand_object       **result_desc,
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_convert_to_string (
	union acpi_operand_object       *obj_desc,
	union acpi_operand_object       **result_desc,
	u32                             base,
	u32                             max_length,
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_convert_to_target_type (
	acpi_object_type                destination_type,
	union acpi_operand_object       *source_desc,
	union acpi_operand_object       **result_desc,
	struct acpi_walk_state          *walk_state);

u32
acpi_ex_convert_to_ascii (
	acpi_integer                    integer,
	u32                             base,
	u8                              *string,
	u8                              max_length);

/*
 * exfield - ACPI AML (p-code) execution - field manipulation
 */

acpi_status
acpi_ex_extract_from_field (
	union acpi_operand_object       *obj_desc,
	void                            *buffer,
	u32                             buffer_length);

acpi_status
acpi_ex_insert_into_field (
	union acpi_operand_object       *obj_desc,
	void                            *buffer,
	u32                             buffer_length);

acpi_status
acpi_ex_setup_region (
	union acpi_operand_object       *obj_desc,
	u32                             field_datum_byte_offset);

acpi_status
acpi_ex_access_region (
	union acpi_operand_object       *obj_desc,
	u32                             field_datum_byte_offset,
	acpi_integer                    *value,
	u32                             read_write);

u8
acpi_ex_register_overflow (
	union acpi_operand_object       *obj_desc,
	acpi_integer                    value);

acpi_status
acpi_ex_field_datum_io (
	union acpi_operand_object       *obj_desc,
	u32                             field_datum_byte_offset,
	acpi_integer                    *value,
	u32                             read_write);

acpi_status
acpi_ex_write_with_update_rule (
	union acpi_operand_object       *obj_desc,
	acpi_integer                    mask,
	acpi_integer                    field_value,
	u32                             field_datum_byte_offset);

void
acpi_ex_get_buffer_datum(
	acpi_integer                    *datum,
	void                            *buffer,
	u32                             buffer_length,
	u32                             byte_granularity,
	u32                             buffer_offset);

void
acpi_ex_set_buffer_datum (
	acpi_integer                    merged_datum,
	void                            *buffer,
	u32                             buffer_length,
	u32                             byte_granularity,
	u32                             buffer_offset);

acpi_status
acpi_ex_read_data_from_field (
	struct acpi_walk_state          *walk_state,
	union acpi_operand_object       *obj_desc,
	union acpi_operand_object       **ret_buffer_desc);

acpi_status
acpi_ex_write_data_to_field (
	union acpi_operand_object       *source_desc,
	union acpi_operand_object       *obj_desc,
	union acpi_operand_object       **result_desc);

/*
 * exmisc - ACPI AML (p-code) execution - specific opcodes
 */

acpi_status
acpi_ex_opcode_3A_0T_0R (
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_opcode_3A_1T_1R (
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_opcode_6A_0T_1R (
	struct acpi_walk_state          *walk_state);

u8
acpi_ex_do_match (
	u32                             match_op,
	acpi_integer                    package_value,
	acpi_integer                    match_value);

acpi_status
acpi_ex_get_object_reference (
	union acpi_operand_object       *obj_desc,
	union acpi_operand_object       **return_desc,
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_resolve_multiple (
	struct acpi_walk_state          *walk_state,
	union acpi_operand_object       *operand,
	acpi_object_type                *return_type,
	union acpi_operand_object       **return_desc);

acpi_status
acpi_ex_concat_template (
	union acpi_operand_object       *obj_desc,
	union acpi_operand_object       *obj_desc2,
	union acpi_operand_object       **actual_return_desc,
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_do_concatenate (
	union acpi_operand_object       *obj_desc,
	union acpi_operand_object       *obj_desc2,
	union acpi_operand_object       **actual_return_desc,
	struct acpi_walk_state          *walk_state);

u8
acpi_ex_do_logical_op (
	u16                             opcode,
	acpi_integer                    operand0,
	acpi_integer                    operand1);

acpi_integer
acpi_ex_do_math_op (
	u16                             opcode,
	acpi_integer                    operand0,
	acpi_integer                    operand1);

acpi_status
acpi_ex_create_mutex (
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_create_processor (
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_create_power_resource (
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_create_region (
	u8                              *aml_start,
	u32                             aml_length,
	u8                              region_space,
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_create_table_region (
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_create_event (
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_create_alias (
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_create_method (
	u8                              *aml_start,
	u32                             aml_length,
	struct acpi_walk_state          *walk_state);


/*
 * exconfig - dynamic table load/unload
 */

acpi_status
acpi_ex_add_table (
	struct acpi_table_header        *table,
	struct acpi_namespace_node      *parent_node,
	union acpi_operand_object       **ddb_handle);

acpi_status
acpi_ex_load_op (
	union acpi_operand_object       *obj_desc,
	union acpi_operand_object       *target,
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_load_table_op (
	struct acpi_walk_state          *walk_state,
	union acpi_operand_object       **return_desc);

acpi_status
acpi_ex_unload_table (
	union acpi_operand_object       *ddb_handle);


/*
 * exmutex - mutex support
 */

acpi_status
acpi_ex_acquire_mutex (
	union acpi_operand_object       *time_desc,
	union acpi_operand_object       *obj_desc,
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_release_mutex (
	union acpi_operand_object       *obj_desc,
	struct acpi_walk_state          *walk_state);

void
acpi_ex_release_all_mutexes (
	struct acpi_thread_state        *thread);

void
acpi_ex_unlink_mutex (
	union acpi_operand_object       *obj_desc);

void
acpi_ex_link_mutex (
	union acpi_operand_object       *obj_desc,
	struct acpi_thread_state        *thread);

/*
 * exprep - ACPI AML (p-code) execution - prep utilities
 */

acpi_status
acpi_ex_prep_common_field_object (
	union acpi_operand_object       *obj_desc,
	u8                              field_flags,
	u8                              field_attribute,
	u32                             field_bit_position,
	u32                             field_bit_length);

acpi_status
acpi_ex_prep_field_value (
	struct acpi_create_field_info   *info);

/*
 * exsystem - Interface to OS services
 */

acpi_status
acpi_ex_system_do_notify_op (
	union acpi_operand_object       *value,
	union acpi_operand_object       *obj_desc);

acpi_status
acpi_ex_system_do_suspend(
	u32                             time);

acpi_status
acpi_ex_system_do_stall (
	u32                             time);

acpi_status
acpi_ex_system_acquire_mutex(
	union acpi_operand_object       *time,
	union acpi_operand_object       *obj_desc);

acpi_status
acpi_ex_system_release_mutex(
	union acpi_operand_object       *obj_desc);

acpi_status
acpi_ex_system_signal_event(
	union acpi_operand_object       *obj_desc);

acpi_status
acpi_ex_system_wait_event(
	union acpi_operand_object       *time,
	union acpi_operand_object       *obj_desc);

acpi_status
acpi_ex_system_reset_event(
	union acpi_operand_object       *obj_desc);

acpi_status
acpi_ex_system_wait_semaphore (
	acpi_handle                     semaphore,
	u16                             timeout);


/*
 * exmonadic - ACPI AML (p-code) execution, monadic operators
 */

acpi_status
acpi_ex_opcode_1A_0T_0R (
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_opcode_1A_0T_1R (
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_opcode_1A_1T_1R (
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_opcode_1A_1T_0R (
	struct acpi_walk_state          *walk_state);

/*
 * exdyadic - ACPI AML (p-code) execution, dyadic operators
 */

acpi_status
acpi_ex_opcode_2A_0T_0R (
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_opcode_2A_0T_1R (
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_opcode_2A_1T_1R (
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_opcode_2A_2T_1R (
	struct acpi_walk_state          *walk_state);


/*
 * exresolv  - Object resolution and get value functions
 */

acpi_status
acpi_ex_resolve_to_value (
	union acpi_operand_object       **stack_ptr,
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_resolve_node_to_value (
	struct acpi_namespace_node      **stack_ptr,
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_resolve_object_to_value (
	union acpi_operand_object       **stack_ptr,
	struct acpi_walk_state          *walk_state);


/*
 * exdump - Scanner debug output routines
 */

void
acpi_ex_dump_operand (
	union acpi_operand_object       *entry_desc);

void
acpi_ex_dump_operands (
	union acpi_operand_object       **operands,
	acpi_interpreter_mode           interpreter_mode,
	char                            *ident,
	u32                             num_levels,
	char                            *note,
	char                            *module_name,
	u32                             line_number);

void
acpi_ex_dump_object_descriptor (
	union acpi_operand_object       *object,
	u32                             flags);

void
acpi_ex_dump_node (
	struct acpi_namespace_node      *node,
	u32                             flags);

void
acpi_ex_out_string (
	char                            *title,
	char                            *value);

void
acpi_ex_out_pointer (
	char                            *title,
	void                            *value);

void
acpi_ex_out_integer (
	char                            *title,
	u32                             value);

void
acpi_ex_out_address (
	char                            *title,
	acpi_physical_address           value);


/*
 * exnames - interpreter/scanner name load/execute
 */

char *
acpi_ex_allocate_name_string (
	u32                             prefix_count,
	u32                             num_name_segs);

u32
acpi_ex_good_char (
	u32                             character);

acpi_status
acpi_ex_name_segment (
	u8                              **in_aml_address,
	char                            *name_string);

acpi_status
acpi_ex_get_name_string (
	acpi_object_type                data_type,
	u8                              *in_aml_address,
	char                            **out_name_string,
	u32                             *out_name_length);

acpi_status
acpi_ex_do_name (
	acpi_object_type                data_type,
	acpi_interpreter_mode           load_exec_mode);


/*
 * exstore - Object store support
 */

acpi_status
acpi_ex_store (
	union acpi_operand_object       *val_desc,
	union acpi_operand_object       *dest_desc,
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_store_object_to_index (
	union acpi_operand_object       *val_desc,
	union acpi_operand_object       *dest_desc,
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_store_object_to_node (
	union acpi_operand_object       *source_desc,
	struct acpi_namespace_node      *node,
	struct acpi_walk_state          *walk_state);


/*
 * exstoren
 */

acpi_status
acpi_ex_resolve_object (
	union acpi_operand_object       **source_desc_ptr,
	acpi_object_type                target_type,
	struct acpi_walk_state          *walk_state);

acpi_status
acpi_ex_store_object_to_object (
	union acpi_operand_object       *source_desc,
	union acpi_operand_object       *dest_desc,
	union acpi_operand_object       **new_desc,
	struct acpi_walk_state          *walk_state);


/*
 * excopy - object copy
 */

acpi_status
acpi_ex_store_buffer_to_buffer (
	union acpi_operand_object       *source_desc,
	union acpi_operand_object       *target_desc);

acpi_status
acpi_ex_store_string_to_string (
	union acpi_operand_object       *source_desc,
	union acpi_operand_object       *target_desc);

acpi_status
acpi_ex_copy_integer_to_index_field (
	union acpi_operand_object       *source_desc,
	union acpi_operand_object       *target_desc);

acpi_status
acpi_ex_copy_integer_to_bank_field (
	union acpi_operand_object       *source_desc,
	union acpi_operand_object       *target_desc);

acpi_status
acpi_ex_copy_data_to_named_field (
	union acpi_operand_object       *source_desc,
	struct acpi_namespace_node      *node);

acpi_status
acpi_ex_copy_integer_to_buffer_field (
	union acpi_operand_object       *source_desc,
	union acpi_operand_object       *target_desc);

/*
 * exutils - interpreter/scanner utilities
 */

acpi_status
acpi_ex_enter_interpreter (
	void);

void
acpi_ex_exit_interpreter (
	void);

void
acpi_ex_truncate_for32bit_table (
	union acpi_operand_object       *obj_desc);

u8
acpi_ex_acquire_global_lock (
	u32                             rule);

void
acpi_ex_release_global_lock (
	u8                              locked);

u32
acpi_ex_digits_needed (
	acpi_integer                    value,
	u32                             base);

void
acpi_ex_eisa_id_to_string (
	u32                             numeric_id,
	char                            *out_string);

void
acpi_ex_unsigned_integer_to_string (
	acpi_integer                    value,
	char                            *out_string);


/*
 * exregion - default op_region handlers
 */

acpi_status
acpi_ex_system_memory_space_handler (
	u32                             function,
	acpi_physical_address           address,
	u32                             bit_width,
	acpi_integer                    *value,
	void                            *handler_context,
	void                            *region_context);

acpi_status
acpi_ex_system_io_space_handler (
	u32                             function,
	acpi_physical_address           address,
	u32                             bit_width,
	acpi_integer                    *value,
	void                            *handler_context,
	void                            *region_context);

acpi_status
acpi_ex_pci_config_space_handler (
	u32                             function,
	acpi_physical_address           address,
	u32                             bit_width,
	acpi_integer                    *value,
	void                            *handler_context,
	void                            *region_context);

acpi_status
acpi_ex_cmos_space_handler (
	u32                             function,
	acpi_physical_address           address,
	u32                             bit_width,
	acpi_integer                    *value,
	void                            *handler_context,
	void                            *region_context);

acpi_status
acpi_ex_pci_bar_space_handler (
	u32                             function,
	acpi_physical_address           address,
	u32                             bit_width,
	acpi_integer                    *value,
	void                            *handler_context,
	void                            *region_context);

acpi_status
acpi_ex_embedded_controller_space_handler (
	u32                             function,
	acpi_physical_address           address,
	u32                             bit_width,
	acpi_integer                    *value,
	void                            *handler_context,
	void                            *region_context);

acpi_status
acpi_ex_sm_bus_space_handler (
	u32                             function,
	acpi_physical_address           address,
	u32                             bit_width,
	acpi_integer                    *value,
	void                            *handler_context,
	void                            *region_context);


acpi_status
acpi_ex_data_table_space_handler (
	u32                             function,
	acpi_physical_address           address,
	u32                             bit_width,
	acpi_integer                    *value,
	void                            *handler_context,
	void                            *region_context);

#endif /* __INTERP_H__ */
