/*******************************************************************************
 *
 * Module Name: nsalloc - Namespace allocation and deletion utilities
 *
 ******************************************************************************/

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
#include <acpi/acnamesp.h>


#define _COMPONENT          ACPI_NAMESPACE
	 ACPI_MODULE_NAME    ("nsalloc")


/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_create_node
 *
 * PARAMETERS:  acpi_name       - Name of the new node
 *
 * RETURN:      None
 *
 * DESCRIPTION: Create a namespace node
 *
 ******************************************************************************/

struct acpi_namespace_node *
acpi_ns_create_node (
	u32                             name)
{
	struct acpi_namespace_node      *node;


	ACPI_FUNCTION_TRACE ("ns_create_node");


	node = ACPI_MEM_CALLOCATE (sizeof (struct acpi_namespace_node));
	if (!node) {
		return_PTR (NULL);
	}

	ACPI_MEM_TRACKING (acpi_gbl_memory_lists[ACPI_MEM_LIST_NSNODE].total_allocated++);

	node->name.integer   = name;
	node->reference_count = 1;
	ACPI_SET_DESCRIPTOR_TYPE (node, ACPI_DESC_TYPE_NAMED);

	return_PTR (node);
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_delete_node
 *
 * PARAMETERS:  Node            - Node to be deleted
 *
 * RETURN:      None
 *
 * DESCRIPTION: Delete a namespace node
 *
 ******************************************************************************/

void
acpi_ns_delete_node (
	struct acpi_namespace_node      *node)
{
	struct acpi_namespace_node      *parent_node;
	struct acpi_namespace_node      *prev_node;
	struct acpi_namespace_node      *next_node;


	ACPI_FUNCTION_TRACE_PTR ("ns_delete_node", node);


	parent_node = acpi_ns_get_parent_node (node);

	prev_node = NULL;
	next_node = parent_node->child;

	/* Find the node that is the previous peer in the parent's child list */

	while (next_node != node) {
		prev_node = next_node;
		next_node = prev_node->peer;
	}

	if (prev_node) {
		/* Node is not first child, unlink it */

		prev_node->peer = next_node->peer;
		if (next_node->flags & ANOBJ_END_OF_PEER_LIST) {
			prev_node->flags |= ANOBJ_END_OF_PEER_LIST;
		}
	}
	else {
		/* Node is first child (has no previous peer) */

		if (next_node->flags & ANOBJ_END_OF_PEER_LIST) {
			/* No peers at all */

			parent_node->child = NULL;
		}
		else {   /* Link peer list to parent */

			parent_node->child = next_node->peer;
		}
	}


	ACPI_MEM_TRACKING (acpi_gbl_memory_lists[ACPI_MEM_LIST_NSNODE].total_freed++);

	/*
	 * Detach an object if there is one then delete the node
	 */
	acpi_ns_detach_object (node);
	ACPI_MEM_FREE (node);
	return_VOID;
}


#ifdef ACPI_ALPHABETIC_NAMESPACE
/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_compare_names
 *
 * PARAMETERS:  Name1           - First name to compare
 *              Name2           - Second name to compare
 *
 * RETURN:      value from strncmp
 *
 * DESCRIPTION: Compare two ACPI names.  Names that are prefixed with an
 *              underscore are forced to be alphabetically first.
 *
 ******************************************************************************/

int
acpi_ns_compare_names (
	char                            *name1,
	char                            *name2)
{
	char                            reversed_name1[ACPI_NAME_SIZE];
	char                            reversed_name2[ACPI_NAME_SIZE];
	u32                             i;
	u32                             j;


	/*
	 * Replace all instances of "underscore" with a value that is smaller so
	 * that all names that are prefixed with underscore(s) are alphabetically
	 * first.
	 *
	 * Reverse the name bytewise so we can just do a 32-bit compare instead
	 * of a strncmp.
	 */
	for (i = 0, j= (ACPI_NAME_SIZE - 1); i < ACPI_NAME_SIZE; i++, j--) {
		reversed_name1[j] = name1[i];
		if (name1[i] == '_') {
			reversed_name1[j] = '*';
		}

		reversed_name2[j] = name2[i];
		if (name2[i] == '_') {
			reversed_name2[j] = '*';
		}
	}

	return (*(int *) reversed_name1 - *(int *) reversed_name2);
}
#endif


/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_install_node
 *
 * PARAMETERS:  walk_state      - Current state of the walk
 *              parent_node     - The parent of the new Node
 *              Node            - The new Node to install
 *              Type            - ACPI object type of the new Node
 *
 * RETURN:      None
 *
 * DESCRIPTION: Initialize a new namespace node and install it amongst
 *              its peers.
 *
 *              Note: Current namespace lookup is linear search.  However, the
 *              nodes are linked in alphabetical order to 1) put all reserved
 *              names (start with underscore) first, and to 2) make a readable
 *              namespace dump.
 *
 ******************************************************************************/

void
acpi_ns_install_node (
	struct acpi_walk_state          *walk_state,
	struct acpi_namespace_node      *parent_node,   /* Parent */
	struct acpi_namespace_node      *node,          /* New Child*/
	acpi_object_type                type)
{
	u16                             owner_id = 0;
	struct acpi_namespace_node      *child_node;
#ifdef ACPI_ALPHABETIC_NAMESPACE

	struct acpi_namespace_node      *previous_child_node;
#endif


	ACPI_FUNCTION_TRACE ("ns_install_node");


	/*
	 * Get the owner ID from the Walk state
	 * The owner ID is used to track table deletion and
	 * deletion of objects created by methods
	 */
	if (walk_state) {
		owner_id = walk_state->owner_id;
	}

	/* Link the new entry into the parent and existing children */

	child_node = parent_node->child;
	if (!child_node) {
		parent_node->child = node;
		node->flags |= ANOBJ_END_OF_PEER_LIST;
		node->peer = parent_node;
	}
	else {
#ifdef ACPI_ALPHABETIC_NAMESPACE
		/*
		 * Walk the list whilst searching for the the correct
		 * alphabetic placement.
		 */
		previous_child_node = NULL;
		while (acpi_ns_compare_names (acpi_ut_get_node_name (child_node), acpi_ut_get_node_name (node)) < 0) {
			if (child_node->flags & ANOBJ_END_OF_PEER_LIST) {
				/* Last peer;  Clear end-of-list flag */

				child_node->flags &= ~ANOBJ_END_OF_PEER_LIST;

				/* This node is the new peer to the child node */

				child_node->peer = node;

				/* This node is the new end-of-list */

				node->flags |= ANOBJ_END_OF_PEER_LIST;
				node->peer = parent_node;
				break;
			}

			/* Get next peer */

			previous_child_node = child_node;
			child_node = child_node->peer;
		}

		/* Did the node get inserted at the end-of-list? */

		if (!(node->flags & ANOBJ_END_OF_PEER_LIST)) {
			/*
			 * Loop above terminated without reaching the end-of-list.
			 * Insert the new node at the current location
			 */
			if (previous_child_node) {
				/* Insert node alphabetically */

				node->peer = child_node;
				previous_child_node->peer = node;
			}
			else {
				/* Insert node alphabetically at start of list */

				node->peer = child_node;
				parent_node->child = node;
			}
		}
#else
		while (!(child_node->flags & ANOBJ_END_OF_PEER_LIST)) {
			child_node = child_node->peer;
		}

		child_node->peer = node;

		/* Clear end-of-list flag */

		child_node->flags &= ~ANOBJ_END_OF_PEER_LIST;
		node->flags     |= ANOBJ_END_OF_PEER_LIST;
		node->peer = parent_node;
#endif
	}

	/* Init the new entry */

	node->owner_id = owner_id;
	node->type = (u8) type;

	ACPI_DEBUG_PRINT ((ACPI_DB_NAMES,
		"%4.4s (%s) [Node %p Owner %X] added to %4.4s (%s) [Node %p]\n",
		acpi_ut_get_node_name (node), acpi_ut_get_type_name (node->type), node, owner_id,
		acpi_ut_get_node_name (parent_node), acpi_ut_get_type_name (parent_node->type),
		parent_node));

	/*
	 * Increment the reference count(s) of all parents up to
	 * the root!
	 */
	while ((node = acpi_ns_get_parent_node (node)) != NULL) {
		node->reference_count++;
	}

	return_VOID;
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_delete_children
 *
 * PARAMETERS:  parent_node     - Delete this objects children
 *
 * RETURN:      None.
 *
 * DESCRIPTION: Delete all children of the parent object. In other words,
 *              deletes a "scope".
 *
 ******************************************************************************/

void
acpi_ns_delete_children (
	struct acpi_namespace_node      *parent_node)
{
	struct acpi_namespace_node      *child_node;
	struct acpi_namespace_node      *next_node;
	struct acpi_namespace_node      *node;
	u8                              flags;


	ACPI_FUNCTION_TRACE_PTR ("ns_delete_children", parent_node);


	if (!parent_node) {
		return_VOID;
	}

	/* If no children, all done! */

	child_node = parent_node->child;
	if (!child_node) {
		return_VOID;
	}

	/*
	 * Deallocate all children at this level
	 */
	do {
		/* Get the things we need */

		next_node   = child_node->peer;
		flags       = child_node->flags;

		/* Grandchildren should have all been deleted already */

		if (child_node->child) {
			ACPI_DEBUG_PRINT ((ACPI_DB_ERROR, "Found a grandchild! P=%p C=%p\n",
				parent_node, child_node));
		}

		/* Now we can free this child object */

		ACPI_MEM_TRACKING (acpi_gbl_memory_lists[ACPI_MEM_LIST_NSNODE].total_freed++);

		ACPI_DEBUG_PRINT ((ACPI_DB_ALLOCATIONS, "Object %p, Remaining %X\n",
			child_node, acpi_gbl_current_node_count));

		/*
		 * Detach an object if there is one, then free the child node
		 */
		acpi_ns_detach_object (child_node);

		/*
		 * Decrement the reference count(s) of all parents up to
		 * the root! (counts were incremented when the node was created)
		 */
		node = child_node;
		while ((node = acpi_ns_get_parent_node (node)) != NULL) {
			node->reference_count--;
		}

		/* There should be only one reference remaining on this node */

		if (child_node->reference_count != 1) {
			ACPI_REPORT_WARNING (("Existing references (%d) on node being deleted (%p)\n",
				child_node->reference_count, child_node));
		}

		/* Now we can delete the node */

		ACPI_MEM_FREE (child_node);

		/* And move on to the next child in the list */

		child_node = next_node;

	} while (!(flags & ANOBJ_END_OF_PEER_LIST));


	/* Clear the parent's child pointer */

	parent_node->child = NULL;

	return_VOID;
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_delete_namespace_subtree
 *
 * PARAMETERS:  parent_node     - Root of the subtree to be deleted
 *
 * RETURN:      None.
 *
 * DESCRIPTION: Delete a subtree of the namespace.  This includes all objects
 *              stored within the subtree.
 *
 ******************************************************************************/

void
acpi_ns_delete_namespace_subtree (
	struct acpi_namespace_node      *parent_node)
{
	struct acpi_namespace_node      *child_node = NULL;
	u32                             level = 1;


	ACPI_FUNCTION_TRACE ("ns_delete_namespace_subtree");


	if (!parent_node) {
		return_VOID;
	}

	/*
	 * Traverse the tree of objects until we bubble back up
	 * to where we started.
	 */
	while (level > 0) {
		/* Get the next node in this scope (NULL if none) */

		child_node = acpi_ns_get_next_node (ACPI_TYPE_ANY, parent_node,
				 child_node);
		if (child_node) {
			/* Found a child node - detach any attached object */

			acpi_ns_detach_object (child_node);

			/* Check if this node has any children */

			if (acpi_ns_get_next_node (ACPI_TYPE_ANY, child_node, NULL)) {
				/*
				 * There is at least one child of this node,
				 * visit the node
				 */
				level++;
				parent_node   = child_node;
				child_node    = NULL;
			}
		}
		else {
			/*
			 * No more children of this parent node.
			 * Move up to the grandparent.
			 */
			level--;

			/*
			 * Now delete all of the children of this parent
			 * all at the same time.
			 */
			acpi_ns_delete_children (parent_node);

			/* New "last child" is this parent node */

			child_node = parent_node;

			/* Move up the tree to the grandparent */

			parent_node = acpi_ns_get_parent_node (parent_node);
		}
	}

	return_VOID;
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_remove_reference
 *
 * PARAMETERS:  Node           - Named node whose reference count is to be
 *                               decremented
 *
 * RETURN:      None.
 *
 * DESCRIPTION: Remove a Node reference.  Decrements the reference count
 *              of all parent Nodes up to the root.  Any node along
 *              the way that reaches zero references is freed.
 *
 ******************************************************************************/

void
acpi_ns_remove_reference (
	struct acpi_namespace_node      *node)
{
	struct acpi_namespace_node      *parent_node;
	struct acpi_namespace_node      *this_node;


	ACPI_FUNCTION_ENTRY ();


	/*
	 * Decrement the reference count(s) of this node and all
	 * nodes up to the root,  Delete anything with zero remaining references.
	 */
	this_node = node;
	while (this_node) {
		/* Prepare to move up to parent */

		parent_node = acpi_ns_get_parent_node (this_node);

		/* Decrement the reference count on this node */

		this_node->reference_count--;

		/* Delete the node if no more references */

		if (!this_node->reference_count) {
			/* Delete all children and delete the node */

			acpi_ns_delete_children (this_node);
			acpi_ns_delete_node (this_node);
		}

		this_node = parent_node;
	}
}


/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_delete_namespace_by_owner
 *
 * PARAMETERS:  owner_id    - All nodes with this owner will be deleted
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Delete entries within the namespace that are owned by a
 *              specific ID.  Used to delete entire ACPI tables.  All
 *              reference counts are updated.
 *
 ******************************************************************************/

void
acpi_ns_delete_namespace_by_owner (
	u16                             owner_id)
{
	struct acpi_namespace_node      *child_node;
	struct acpi_namespace_node      *deletion_node;
	u32                             level;
	struct acpi_namespace_node      *parent_node;


	ACPI_FUNCTION_TRACE_U32 ("ns_delete_namespace_by_owner", owner_id);


	parent_node   = acpi_gbl_root_node;
	child_node    = NULL;
	deletion_node = NULL;
	level         = 1;

	/*
	 * Traverse the tree of nodes until we bubble back up
	 * to where we started.
	 */
	while (level > 0) {
		/*
		 * Get the next child of this parent node. When child_node is NULL,
		 * the first child of the parent is returned
		 */
		child_node = acpi_ns_get_next_node (ACPI_TYPE_ANY, parent_node, child_node);

		if (deletion_node) {
			acpi_ns_remove_reference (deletion_node);
			deletion_node = NULL;
		}

		if (child_node) {
			if (child_node->owner_id == owner_id) {
				/* Found a matching child node - detach any attached object */

				acpi_ns_detach_object (child_node);
			}

			/* Check if this node has any children */

			if (acpi_ns_get_next_node (ACPI_TYPE_ANY, child_node, NULL)) {
				/*
				 * There is at least one child of this node,
				 * visit the node
				 */
				level++;
				parent_node   = child_node;
				child_node    = NULL;
			}
			else if (child_node->owner_id == owner_id) {
				deletion_node = child_node;
			}
		}
		else {
			/*
			 * No more children of this parent node.
			 * Move up to the grandparent.
			 */
			level--;
			if (level != 0) {
				if (parent_node->owner_id == owner_id) {
					deletion_node = parent_node;
				}
			}

			/* New "last child" is this parent node */

			child_node = parent_node;

			/* Move up the tree to the grandparent */

			parent_node = acpi_ns_get_parent_node (parent_node);
		}
	}

	return_VOID;
}


