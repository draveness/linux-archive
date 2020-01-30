/******************************************************************************
 *
 * Name: acoutput.h -- debug output
 *       $Revision: 66 $
 *
 *****************************************************************************/

/*
 *  Copyright (C) 2000 R. Byron Moore
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef __ACOUTPUT_H__
#define __ACOUTPUT_H__

/*
 * Debug levels and component IDs.  These are used to control the
 * granularity of the output of the DEBUG_PRINT macro -- on a per-
 * component basis and a per-exception-type basis.
 */

/* Component IDs -- used in the global "Debug_layer" */

#define GLOBAL                      0x00000001
#define COMMON                      0x00000002
#define PARSER                      0x00000004
#define DISPATCHER                  0x00000008
#define INTERPRETER                 0x00000010
#define NAMESPACE                   0x00000020
#define RESOURCE_MANAGER            0x00000040
#define TABLE_MANAGER               0x00000080
#define EVENT_HANDLING              0x00000100
#define HARDWARE                    0x00000200
#define MISCELLANEOUS               0x00000400
#define OS_DEPENDENT                0x00000800

#define BUS_MANAGER                 0x00001000

#define PROCESSOR_CONTROL           0x00002000
#define SYSTEM_CONTROL              0x00004000
#define THERMAL_CONTROL             0x00008000
#define POWER_CONTROL               0x00010000

#define EMBEDDED_CONTROLLER         0x00020000
#define BATTERY                     0x00040000

#define DEBUGGER                    0x00100000
#define ALL_COMPONENTS              0x001FFFFF

#define COMPONENT_DEFAULT           (ALL_COMPONENTS)


/* Exception level -- used in the global "Debug_level" */

#define ACPI_OK                     0x00000001
#define ACPI_INFO                   0x00000002
#define ACPI_WARN                   0x00000004
#define ACPI_ERROR                  0x00000008
#define ACPI_FATAL                  0x00000010
#define ACPI_DEBUG_OBJECT           0x00000020
#define ACPI_ALL                    0x0000003F


/* Trace level -- also used in the global "Debug_level" */

#define TRACE_PARSE                 0x00000100
#define TRACE_DISPATCH              0x00000200
#define TRACE_LOAD                  0x00000400
#define TRACE_EXEC                  0x00000800
#define TRACE_NAMES                 0x00001000
#define TRACE_OPREGION              0x00002000
#define TRACE_BFIELD                0x00004000
#define TRACE_TRASH                 0x00008000
#define TRACE_TABLES                0x00010000
#define TRACE_FUNCTIONS             0x00020000
#define TRACE_VALUES                0x00040000
#define TRACE_OBJECTS               0x00080000
#define TRACE_ALLOCATIONS           0x00100000
#define TRACE_RESOURCES             0x00200000
#define TRACE_IO                    0x00400000
#define TRACE_INTERRUPTS            0x00800000
#define TRACE_USER_REQUESTS         0x01000000
#define TRACE_PACKAGE               0x02000000
#define TRACE_MUTEX                 0x04000000

#define TRACE_ALL                   0x0FFFFF00


/* Exceptionally verbose output -- also used in the global "Debug_level" */

#define VERBOSE_AML_DISASSEMBLE     0x10000000
#define VERBOSE_INFO                0x20000000
#define VERBOSE_TABLES              0x40000000
#define VERBOSE_EVENTS              0x80000000

#define VERBOSE_ALL                 0xF0000000


/* Defaults for Debug_level, debug and normal */

#define DEBUG_DEFAULT               (ACPI_OK | ACPI_WARN | ACPI_ERROR | ACPI_DEBUG_OBJECT)
#define NORMAL_DEFAULT              (ACPI_OK | ACPI_WARN | ACPI_ERROR | ACPI_DEBUG_OBJECT)
#define DEBUG_ALL                   (VERBOSE_AML_DISASSEMBLE | TRACE_ALL | ACPI_ALL)

/* Misc defines */

#define HEX                         0x01
#define ASCII                       0x02
#define FULL_ADDRESS                0x04
#define CHARS_PER_LINE              16          /* used in Dump_buf function */


#endif /* __ACOUTPUT_H__ */
