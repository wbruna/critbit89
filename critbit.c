/*
 * critbit89 - A crit-bit tree implementation for strings in C89
 * Written by Jonas Gehring <jonas@jgehring.net>
 */

/*
 * The code makes the assumption that malloc returns pointers aligned at at
 * least a two-byte boundary. Since the C standard requires that malloc return
 * pointers that can store any type, there are no commonly-used toolchains for
 * which this assumption is false.
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "critbit.h"

#ifdef _MSC_VER /* MSVC */
 typedef unsigned __int8 uint8_t;
 typedef unsigned __int32 uint32_t;
 #ifdef _WIN64
  typedef signed __int64 intptr_t;
 #else
  typedef _W64 signed int intptr_t;
 #endif
#else /* Not MSVC */
 #include <stdint.h>
#endif

#define TYPE_LEAF 0
#define TYPE_NODE 1
#define TYPE_EMPTY -1

typedef struct cb_node_t {
	cb_child_t child[2];
	uint32_t byte;
	int8_t type[2];
	uint8_t otherbits;
} cb_node_t;

/* Standard memory allocation functions */
static void *malloc_std(size_t size, void *baton) {
	(void)baton; /* Prevent compiler warnings */
	return malloc(size);
}

static void free_std(void *ptr, void *baton) {
	(void)baton; /* Prevent compiler warnings */
	free(ptr);
}

/* Static helper functions */
static void cbt_traverse_delete(cb_tree_t *tree, cb_node_t *par, int dir)
{
	if (par->type[dir] == TYPE_NODE) {
		cb_node_t *q = par->child[dir].node;
		cbt_traverse_delete(tree, q, 0);
		cbt_traverse_delete(tree, q, 1);
		tree->free(q, tree->baton);
	} else {
		tree->free(par->child[dir].leaf, tree->baton);
	}
}

static int cbt_traverse_prefixed(cb_node_t * par, int dir,
	int (*callback)(const char *, void *), void *baton)
{
	if (par->type[dir] == TYPE_NODE) {
		cb_node_t *q = par->child[dir].node;
		int ret = 0;

		ret = cbt_traverse_prefixed(q, 0, callback, baton);
		if (ret != 0) {
			return ret;
		}
		ret = cbt_traverse_prefixed(q, 1, callback, baton);
		if (ret != 0) {
			return ret;
		}
		return 0;
	}

	return (callback)((const char *)par->child[dir].leaf, baton);
}

static int numbit(uint8_t mask)
{
	switch(mask) {
		case (1<<0) ^255: return 7;
		case (1<<1) ^255: return 6;
		case (1<<2) ^255: return 5;
		case (1<<3) ^255: return 4;
		case (1<<4) ^255: return 3;
		case (1<<5) ^255: return 2;
		case (1<<6) ^255: return 1;
		case (1<<7) ^255: return 0;
	}
	return -1;
}

#define MAX_PREFIX 200

static void cbt_traverse_print(cb_tree_t *tree, cb_node_t *par, int dir, char* prefix)
{
	size_t lprefix = strlen(prefix);
	if (par->type[dir] == TYPE_NODE) {
		cb_node_t *q = par->child[dir].node;
		printf("%s+-- %d %p %d %d\n", prefix, dir, (void*)q, (int)q->byte, numbit(q->otherbits));
		if (lprefix < MAX_PREFIX - 5)
			sprintf(prefix + lprefix, "%c   ", (dir || (!*prefix)) ? ' ' : '|');
		cbt_traverse_print(tree, q, 0, prefix);
		cbt_traverse_print(tree, q, 1, prefix);
		prefix[lprefix] = 0;
	}
	else {
		const char * leaf = (const char*)par->child[dir].leaf;
		printf("%s+-- %d %p %s\n", prefix, dir, (void*)leaf, leaf ? leaf : "(nil)");
	}
}

void cb_tree_print(cb_tree_t *tree)
{
	char prefix[MAX_PREFIX] = "";
	cb_node_t sentinel;
	sentinel.child[0] = tree->root;
	sentinel.type[0] = tree->root_type;
	puts("");
	cbt_traverse_print(tree, &sentinel, 0, prefix);
	puts("");
}

/*! Creates a new, empty critbit tree */
cb_tree_t cb_tree_make()
{
	cb_tree_t tree;
	tree.root.leaf = NULL;
	tree.root_type = TYPE_EMPTY;
	tree.malloc = &malloc_std;
	tree.free = &free_std;
	tree.baton = NULL;
	return tree;
}

/*! Returns non-zero if tree contains str */
int cb_tree_contains(cb_tree_t *tree, const char *str)
{
	const uint8_t *ubytes = (const uint8_t *)str;
	const size_t ulen = strlen(str);
	cb_node_t *p;
	int direction;
	cb_node_t sentinel;

	if (tree->root_type == TYPE_EMPTY) {
		return 0;
	}

	sentinel.child[0] = tree->root;
	sentinel.type[0] = tree->root_type;
	p = &sentinel;
	direction = 0;

	while (p->type[direction] == TYPE_NODE) {
		cb_node_t *q = p->child[direction].node;
		uint8_t c = 0;

		if (q->byte < ulen) {
			c = ubytes[q->byte];
		}
		direction = (1 + (q->otherbits | c)) >> 8;

		p = q;
	}

	return (strcmp(str, (const char*)p->child[direction].leaf) == 0);
}

/*! Inserts str into tree, returns 0 on success */
int cb_tree_insert(cb_tree_t *tree, const char *str)
{
	const uint8_t *const ubytes = (const uint8_t *)str;
	const size_t ulen = strlen(str);
	cb_node_t *p;
	uint8_t c, *x;
	const uint8_t *leaf;
	uint32_t newbyte;
	uint32_t newotherbits;
	int direction, newdirection;
	cb_node_t *newnode;
	cb_node_t sentinel;

	if (tree->root_type == TYPE_EMPTY) {
		x = (uint8_t *)tree->malloc(ulen + 1, tree->baton);
		if (x == NULL) {
			return ENOMEM;
		}
		memcpy(x, str, ulen + 1);
		tree->root.leaf = x;
		tree->root_type = TYPE_LEAF;
		return 0;
	}

	sentinel.child[0] = tree->root;
	sentinel.type[0] = tree->root_type;

	p = &sentinel;
	direction = 0;

	while (p->type[direction] == TYPE_NODE) {
		cb_node_t *q = p->child[direction].node;
		c = 0;
		if (q->byte < ulen) {
			c = ubytes[q->byte];
		}
		direction = (1 + (q->otherbits | c)) >> 8;

		p = q;
	}

	leaf = p->child[direction].leaf;
	for (newbyte = 0; newbyte < ulen; ++newbyte) {
		if (leaf[newbyte] != ubytes[newbyte]) {
			newotherbits = leaf[newbyte] ^ ubytes[newbyte];
			goto different_byte_found;
		}
	}

	if (leaf[newbyte] != 0) {
		newotherbits = leaf[newbyte];
		goto different_byte_found;
	}
	return 1;

different_byte_found:
	newotherbits |= newotherbits >> 1;
	newotherbits |= newotherbits >> 2;
	newotherbits |= newotherbits >> 4;
	/* (set just the bits above msb) | (move msb out of the way) */
	newotherbits = (newotherbits ^ 255) | (newotherbits >> 1);
	c = leaf[newbyte];
	newdirection = (1 + (newotherbits | c)) >> 8;

	newnode = (cb_node_t *)tree->malloc(sizeof(cb_node_t), tree->baton);
	if (newnode == NULL) {
		return ENOMEM;
	}

	x = (uint8_t *)tree->malloc(ulen + 1, tree->baton);
	if (x == NULL) {
		tree->free(newnode, tree->baton);
		return ENOMEM;
	}

	memcpy(x, ubytes, ulen + 1);
	newnode->byte = newbyte;
	newnode->otherbits = newotherbits;
	newnode->child[1 - newdirection].leaf = x;
	newnode->type[1 - newdirection] = TYPE_LEAF;

	/* Insert into tree */
	p = &sentinel;
	direction = 0;

	for (;;) {
		cb_node_t *q;
		if (p->type[direction] == TYPE_LEAF) {
			break;
		}

		q = p->child[direction].node;
		if (q->byte > newbyte) {
			break;
		}
		if (q->byte == newbyte && q->otherbits > newotherbits) {
			break;
		}

		c = 0;
		if (q->byte < ulen) {
			c = ubytes[q->byte];
		}
		direction = (1 + (q->otherbits | c)) >> 8;
		p = q;
	}

	newnode->child[newdirection] = p->child[direction];
	newnode->type[newdirection] = p->type[direction];
	p->child[direction].node = newnode;
	p->type[direction] = TYPE_NODE;

	/* the root node may have been modified by the insertion */
	tree->root = sentinel.child[0];
	tree->root_type = sentinel.type[0];

	return 0;
}

/*! Deletes str from the tree, returns 0 on success */
int cb_tree_delete(cb_tree_t *tree, const char *str)
{
	const uint8_t *ubytes = (const uint8_t *)str;
	const size_t ulen = strlen(str);
	cb_node_t *p;
	cb_node_t *q;
	int direction;
	int pdirection;
	cb_node_t sentinel;

	if (tree->root_type == TYPE_EMPTY) {
		return 1;
	}

	sentinel.child[0] = tree->root;
	sentinel.type[0] = tree->root_type;
	p = NULL;
	q = &sentinel;
	pdirection = direction = 0;

	while (q->type[direction] == TYPE_NODE) {
		uint8_t c = 0;
		p = q;
		pdirection = direction;
		q = q->child[direction].node;

		if (q->byte < ulen) {
			c = ubytes[q->byte];
		}
		direction = (1 + (q->otherbits | c)) >> 8;
	}

	if (strcmp(str, (const char *)q->child[direction].leaf) != 0) {
		return 1;
	}
	tree->free(q->child[direction].leaf, tree->baton);

	if (!p) {
		tree->root_type = TYPE_EMPTY;
		tree->root.leaf = NULL;
		return 0;
	}

	p->child[pdirection] = q->child[1 - direction];
	p->type[pdirection] = q->type[1 - direction];
	tree->free(q, tree->baton);

	/* the root node may have been modified by the deletion */
	tree->root = sentinel.child[0];
	tree->root_type = sentinel.type[0];

	return 0;
}

/*! Clears the given tree */
void cb_tree_clear(cb_tree_t *tree)
{
	if (tree->root_type != TYPE_EMPTY) {
		cb_node_t sentinel;
		sentinel.child[0] = tree->root;
		sentinel.type[0] = tree->root_type;
		cbt_traverse_delete(tree, &sentinel, 0);
	}
	tree->root_type = TYPE_EMPTY;
}

/*! Calls callback for all strings in tree with the given prefix */
int cb_tree_walk_prefixed(cb_tree_t *tree, const char *prefix,
	int (*callback)(const char *, void *), void *baton)
{
	const uint8_t *ubytes = (const uint8_t *)prefix;
	const size_t ulen = strlen(prefix);
	cb_node_t *p;
	int direction;
	cb_node_t *top;
	int tdirection;
	cb_node_t sentinel;
	const char * leaf;

	if (tree->root_type == TYPE_EMPTY) {
		return 0;
	}

	sentinel.child[0] = tree->root;
	sentinel.type[0] = tree->root_type;
	top = p = &sentinel;
	tdirection = direction = 0;

	while (p->type[direction] == TYPE_NODE) {
		cb_node_t *q = p->child[direction].node;
		uint8_t c = 0;

		if (q->byte < ulen) {
			c = ubytes[q->byte];
			direction = (1 + (q->otherbits | c)) >> 8;
			top = q;
			tdirection = direction;
		}
		else {
			direction = 0;
		}

		p = q;
	}

	leaf = (const char *)p->child[direction].leaf;
	if (strlen(leaf) < ulen || memcmp(leaf, prefix, ulen) != 0) {
		/* No strings match */
		return 0;
	}

	return cbt_traverse_prefixed(top, tdirection, callback, baton);
}
