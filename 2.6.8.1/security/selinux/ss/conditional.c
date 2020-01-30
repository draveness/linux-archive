/* Authors: Karl MacMillan <kmacmillan@tresys.com>
 *          Frank Mayer <mayerf@tresys.com>
 *
 * Copyright (C) 2003 - 2004 Tresys Technology, LLC
 *	This program is free software; you can redistribute it and/or modify
 *  	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation, version 2.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/spinlock.h>
#include <asm/semaphore.h>
#include <linux/slab.h>

#include "security.h"
#include "conditional.h"

/*
 * cond_evaluate_expr evaluates a conditional expr
 * in reverse polish notation. It returns true (1), false (0),
 * or undefined (-1). Undefined occurs when the expression
 * exceeds the stack depth of COND_EXPR_MAXDEPTH.
 */
static int cond_evaluate_expr(struct policydb *p, struct cond_expr *expr)
{

	struct cond_expr *cur;
	int s[COND_EXPR_MAXDEPTH];
	int sp = -1;

	for (cur = expr; cur != NULL; cur = cur->next) {
		switch (cur->expr_type) {
		case COND_BOOL:
			if (sp == (COND_EXPR_MAXDEPTH - 1))
				return -1;
			sp++;
			s[sp] = p->bool_val_to_struct[cur->bool - 1]->state;
			break;
		case COND_NOT:
			if (sp < 0)
				return -1;
			s[sp] = !s[sp];
			break;
		case COND_OR:
			if (sp < 1)
				return -1;
			sp--;
			s[sp] |= s[sp + 1];
			break;
		case COND_AND:
			if (sp < 1)
				return -1;
			sp--;
			s[sp] &= s[sp + 1];
			break;
		case COND_XOR:
			if (sp < 1)
				return -1;
			sp--;
			s[sp] ^= s[sp + 1];
			break;
		case COND_EQ:
			if (sp < 1)
				return -1;
			sp--;
			s[sp] = (s[sp] == s[sp + 1]);
			break;
		case COND_NEQ:
			if (sp < 1)
				return -1;
			sp--;
			s[sp] = (s[sp] != s[sp + 1]);
			break;
		default:
			return -1;
		}
	}
	return s[0];
}

/*
 * evaluate_cond_node evaluates the conditional stored in
 * a struct cond_node and if the result is different than the
 * current state of the node it sets the rules in the true/false
 * list appropriately. If the result of the expression is undefined
 * all of the rules are disabled for safety.
 */
int evaluate_cond_node(struct policydb *p, struct cond_node *node)
{
	int new_state;
	struct cond_av_list* cur;

	new_state = cond_evaluate_expr(p, node->expr);
	if (new_state != node->cur_state) {
		node->cur_state = new_state;
		if (new_state == -1)
			printk(KERN_ERR "security: expression result was undefined - disabling all rules.\n");
		/* turn the rules on or off */
		for (cur = node->true_list; cur != NULL; cur = cur->next) {
			if (new_state <= 0) {
				cur->node->datum.specified &= ~AVTAB_ENABLED;
			} else {
				cur->node->datum.specified |= AVTAB_ENABLED;
			}
		}

		for (cur = node->false_list; cur != NULL; cur = cur->next) {
			/* -1 or 1 */
			if (new_state) {
				cur->node->datum.specified &= ~AVTAB_ENABLED;
			} else {
				cur->node->datum.specified |= AVTAB_ENABLED;
			}
		}
	}
	return 0;
}

int cond_policydb_init(struct policydb *p)
{
	p->bool_val_to_struct = NULL;
	p->cond_list = NULL;
	if (avtab_init(&p->te_cond_avtab))
		return -1;

	return 0;
}

static void cond_av_list_destroy(struct cond_av_list *list)
{
	struct cond_av_list *cur, *next;
	for (cur = list; cur != NULL; cur = next) {
		next = cur->next;
		/* the avtab_ptr_t node is destroy by the avtab */
		kfree(cur);
	}
}

static void cond_node_destroy(struct cond_node *node)
{
	struct cond_expr *cur_expr, *next_expr;

	for (cur_expr = node->expr; cur_expr != NULL; cur_expr = next_expr) {
		next_expr = cur_expr->next;
		kfree(cur_expr);
	}
	cond_av_list_destroy(node->true_list);
	cond_av_list_destroy(node->false_list);
	kfree(node);
}

static void cond_list_destroy(struct cond_node *list)
{
	struct cond_node *next, *cur;

	if (list == NULL)
		return;

	for (cur = list; cur != NULL; cur = next) {
		next = cur->next;
		cond_node_destroy(cur);
	}
}

void cond_policydb_destroy(struct policydb *p)
{
	if (p->bool_val_to_struct != NULL)
		kfree(p->bool_val_to_struct);
	avtab_destroy(&p->te_cond_avtab);
	cond_list_destroy(p->cond_list);
}

int cond_init_bool_indexes(struct policydb *p)
{
	if (p->bool_val_to_struct)
		kfree(p->bool_val_to_struct);
	p->bool_val_to_struct = (struct cond_bool_datum**)
		kmalloc(p->p_bools.nprim * sizeof(struct cond_bool_datum*), GFP_KERNEL);
	if (!p->bool_val_to_struct)
		return -1;
	return 0;
}

int cond_destroy_bool(void *key, void *datum, void *p)
{
	if (key)
		kfree(key);
	kfree(datum);
	return 0;
}

int cond_index_bool(void *key, void *datum, void *datap)
{
	struct policydb *p;
	struct cond_bool_datum *booldatum;

	booldatum = datum;
	p = datap;

	if (!booldatum->value || booldatum->value > p->p_bools.nprim)
		return -EINVAL;

	p->p_bool_val_to_name[booldatum->value - 1] = key;
	p->bool_val_to_struct[booldatum->value -1] = booldatum;

	return 0;
}

int bool_isvalid(struct cond_bool_datum *b)
{
	if (!(b->state == 0 || b->state == 1))
		return 0;
	return 1;
}

int cond_read_bool(struct policydb *p, struct hashtab *h, void *fp)
{
	char *key = NULL;
	struct cond_bool_datum *booldatum;
	__u32 *buf, len;

	booldatum = kmalloc(sizeof(struct cond_bool_datum), GFP_KERNEL);
	if (!booldatum)
		return -1;
	memset(booldatum, 0, sizeof(struct cond_bool_datum));

	buf = next_entry(fp, sizeof(__u32) * 3);
	if (!buf)
		goto err;

	booldatum->value = le32_to_cpu(buf[0]);
	booldatum->state = le32_to_cpu(buf[1]);

	if (!bool_isvalid(booldatum))
		goto err;

	len = le32_to_cpu(buf[2]);

	buf = next_entry(fp, len);
	if (!buf)
		goto err;
	key = kmalloc(len + 1, GFP_KERNEL);
	if (!key)
		goto err;
	memcpy(key, buf, len);
	key[len] = 0;
	if (hashtab_insert(h, key, booldatum))
		goto err;

	return 0;
err:
	cond_destroy_bool(key, booldatum, NULL);
	return -1;
}

static int cond_read_av_list(struct policydb *p, void *fp, struct cond_av_list **ret_list,
			     struct cond_av_list *other)
{
	struct cond_av_list *list, *last = NULL, *cur;
	struct avtab_key key;
	struct avtab_datum datum;
	struct avtab_node *node_ptr;
	int len, i;
	__u32 *buf;
	__u8 found;

	*ret_list = NULL;

	len = 0;
	buf = next_entry(fp, sizeof(__u32));
	if (!buf)
		return -1;

	len = le32_to_cpu(buf[0]);
	if (len == 0) {
		return 0;
	}

	for (i = 0; i < len; i++) {
		if (avtab_read_item(fp, &datum, &key))
			goto err;

		/*
		 * For type rules we have to make certain there aren't any
		 * conflicting rules by searching the te_avtab and the
		 * cond_te_avtab.
		 */
		if (datum.specified & AVTAB_TYPE) {
			if (avtab_search(&p->te_avtab, &key, AVTAB_TYPE)) {
				printk("security: type rule already exists outside of a conditional.");
				goto err;
			}
			/*
			 * If we are reading the false list other will be a pointer to
			 * the true list. We can have duplicate entries if there is only
			 * 1 other entry and it is in our true list.
			 *
			 * If we are reading the true list (other == NULL) there shouldn't
			 * be any other entries.
			 */
			if (other) {
				node_ptr = avtab_search_node(&p->te_cond_avtab, &key, AVTAB_TYPE);
				if (node_ptr) {
					if (avtab_search_node_next(node_ptr, AVTAB_TYPE)) {
						printk("security: too many conflicting type rules.");
						goto err;
					}
					found = 0;
					for (cur = other; cur != NULL; cur = cur->next) {
						if (cur->node == node_ptr) {
							found = 1;
							break;
						}
					}
					if (!found) {
						printk("security: conflicting type rules.");
						goto err;
					}
				}
			} else {
				if (avtab_search(&p->te_cond_avtab, &key, AVTAB_TYPE)) {
					printk("security: conflicting type rules when adding type rule for true.");
					goto err;
				}
			}
		}
		node_ptr = avtab_insert_nonunique(&p->te_cond_avtab, &key, &datum);
		if (!node_ptr) {
			printk("security: could not insert rule.");
			goto err;
		}

		list = kmalloc(sizeof(struct cond_av_list), GFP_KERNEL);
		if (!list)
			goto err;
		memset(list, 0, sizeof(struct cond_av_list));

		list->node = node_ptr;
		if (i == 0)
			*ret_list = list;
		else
			last->next = list;
		last = list;

	}

	return 0;
err:
	cond_av_list_destroy(*ret_list);
	*ret_list = NULL;
	return -1;
}

static int expr_isvalid(struct policydb *p, struct cond_expr *expr)
{
	if (expr->expr_type <= 0 || expr->expr_type > COND_LAST) {
		printk("security: conditional expressions uses unknown operator.\n");
		return 0;
	}

	if (expr->bool > p->p_bools.nprim) {
		printk("security: conditional expressions uses unknown bool.\n");
		return 0;
	}
	return 1;
}

static int cond_read_node(struct policydb *p, struct cond_node *node, void *fp)
{
	__u32 *buf;
	int len, i;
	struct cond_expr *expr = NULL, *last = NULL;

	buf = next_entry(fp, sizeof(__u32));
	if (!buf)
		return -1;

	node->cur_state = le32_to_cpu(buf[0]);

	len = 0;
	buf = next_entry(fp, sizeof(__u32));
	if (!buf)
		return -1;

	/* expr */
	len = le32_to_cpu(buf[0]);

	for (i = 0; i < len; i++ ) {
		buf = next_entry(fp, sizeof(__u32) * 2);
		if (!buf)
			goto err;

		expr = kmalloc(sizeof(struct cond_expr), GFP_KERNEL);
		if (!expr) {
			goto err;
		}
		memset(expr, 0, sizeof(struct cond_expr));

		expr->expr_type = le32_to_cpu(buf[0]);
		expr->bool = le32_to_cpu(buf[1]);

		if (!expr_isvalid(p, expr))
			goto err;

		if (i == 0) {
			node->expr = expr;
		} else {
			last->next = expr;
		}
		last = expr;
	}

	if (cond_read_av_list(p, fp, &node->true_list, NULL) != 0)
		goto err;
	if (cond_read_av_list(p, fp, &node->false_list, node->true_list) != 0)
		goto err;
	return 0;
err:
	cond_node_destroy(node);
	return -1;
}

int cond_read_list(struct policydb *p, void *fp)
{
	struct cond_node *node, *last = NULL;
	__u32 *buf;
	int i, len;

	buf = next_entry(fp, sizeof(__u32));
	if (!buf)
		return -1;

	len = le32_to_cpu(buf[0]);

	for (i = 0; i < len; i++) {
		node = kmalloc(sizeof(struct cond_node), GFP_KERNEL);
		if (!node)
			goto err;
		memset(node, 0, sizeof(struct cond_node));

		if (cond_read_node(p, node, fp) != 0)
			goto err;

		if (i == 0) {
			p->cond_list = node;
		} else {
			last->next = node;
		}
		last = node;
	}
	return 0;
err:
	cond_list_destroy(p->cond_list);
	return -1;
}

/* Determine whether additional permissions are granted by the conditional
 * av table, and if so, add them to the result
 */
void cond_compute_av(struct avtab *ctab, struct avtab_key *key, struct av_decision *avd)
{
	struct avtab_node *node;

	if(!ctab || !key || !avd)
		return;

	for(node = avtab_search_node(ctab, key, AVTAB_AV); node != NULL;
				node = avtab_search_node_next(node, AVTAB_AV)) {
		if ( (__u32) (AVTAB_ALLOWED|AVTAB_ENABLED) ==
		     (node->datum.specified & (AVTAB_ALLOWED|AVTAB_ENABLED)))
			avd->allowed |= avtab_allowed(&node->datum);
		if ( (__u32) (AVTAB_AUDITDENY|AVTAB_ENABLED) ==
		     (node->datum.specified & (AVTAB_AUDITDENY|AVTAB_ENABLED)))
			/* Since a '0' in an auditdeny mask represents a
			 * permission we do NOT want to audit (dontaudit), we use
			 * the '&' operand to ensure that all '0's in the mask
			 * are retained (much unlike the allow and auditallow cases).
			 */
			avd->auditdeny &= avtab_auditdeny(&node->datum);
		if ( (__u32) (AVTAB_AUDITALLOW|AVTAB_ENABLED) ==
		     (node->datum.specified & (AVTAB_AUDITALLOW|AVTAB_ENABLED)))
			avd->auditallow |= avtab_auditallow(&node->datum);
	}
	return;
}
