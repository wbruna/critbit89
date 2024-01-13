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
	if (par->child[dir].type == TYPE_NODE) {
		cb_node_t *q = par->child[dir].ptr.node;
		cbt_traverse_delete(tree, q, 0);
		cbt_traverse_delete(tree, q, 1);
		tree->free(q, tree->baton);
	} else {
		tree->free(par->child[dir].ptr.leaf, tree->baton);
	}
}

static int cbt_traverse_prefixed(cb_node_t * par, int dir,
	int (*callback)(const char *, void *), void *baton)
{
	if (par->child[dir].type == TYPE_NODE) {
		cb_node_t *q = par->child[dir].ptr.node;
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

	return (callback)((const char *)par->child[dir].ptr.leaf, baton);
}


/*! Creates a new, empty critbit tree */
cb_tree_t cb_tree_make()
{
	cb_tree_t tree;
	tree.root.ptr.leaf = NULL;
	tree.root.type = TYPE_EMPTY;
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

	if (tree->root.type == TYPE_EMPTY) {
		return 0;
	}

	sentinel.child[0] = tree->root;
	p = &sentinel;
	direction = 0;

	while (p->child[direction].type == TYPE_NODE) {
		cb_node_t *q = p->child[direction].ptr.node;
		uint8_t c = 0;

		if (q->byte < ulen) {
			c = ubytes[q->byte];
		}
		direction = (1 + (q->otherbits | c)) >> 8;

		p = q;
	}

	return (strcmp(str, (const char*)p->child[direction].ptr.leaf) == 0);
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
	cb_node_t sentinel; /* used as a temporary sentinel */

	if (tree->root.type == TYPE_EMPTY) {
		x = (uint8_t *)tree->malloc(ulen + 1, tree->baton);
		if (x == NULL) {
			return ENOMEM;
		}
		memcpy(x, str, ulen + 1);
		tree->root.ptr.leaf = x;
		tree->root.type = TYPE_LEAF;
		return 0;
	}

	sentinel.child[0] = tree->root;

	p = &sentinel;
	direction = 0;

	while (p->child[direction].type == TYPE_NODE) {
		cb_node_t *q = p->child[direction].ptr.node;
		c = 0;
		if (q->byte < ulen) {
			c = ubytes[q->byte];
		}
		direction = (1 + (q->otherbits | c)) >> 8;

		p = q;
	}

	leaf = p->child[direction].ptr.leaf;
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
	newotherbits = (newotherbits & ~(newotherbits >> 1)) ^ 255;
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
	newnode->child[1 - newdirection].ptr.leaf = x;
	newnode->child[1 - newdirection].type = TYPE_LEAF;

	/* Insert into tree */
	p = &sentinel;
	direction = 0;

	for (;;) {
		cb_node_t *q;
		if (p->child[direction].type == TYPE_LEAF) {
			break;
		}

		q = p->child[direction].ptr.node;
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
	p->child[direction].ptr.node = newnode;
	p->child[direction].type = TYPE_NODE;

	/* the root node may have been modified by the insertion */
	tree->root = sentinel.child[0];

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
	cb_node_t sentinel; /* used as a temporary sentinel */

	if (tree->root.type == TYPE_EMPTY) {
		return 1;
	}

	sentinel.child[0] = tree->root;
	p = NULL;
	q = &sentinel;
	pdirection = direction = 0;

	while (q->child[direction].type == TYPE_NODE) {
		uint8_t c = 0;
		p = q;
		pdirection = direction;
		q = q->child[direction].ptr.node;

		if (q->byte < ulen) {
			c = ubytes[q->byte];
		}
		direction = (1 + (q->otherbits | c)) >> 8;
	}

	if (strcmp(str, (const char *)q->child[direction].ptr.leaf) != 0) {
		return 1;
	}
	tree->free(q->child[direction].ptr.leaf, tree->baton);

	if (!p) {
		tree->root.type = TYPE_EMPTY;
		tree->root.ptr.leaf = NULL;
		return 0;
	}

	p->child[pdirection] = q->child[1 - direction];
	tree->free(q, tree->baton);

	/* the root node may have been modified by the deletion */
	tree->root = sentinel.child[0];

	return 0;
}

/*! Clears the given tree */
void cb_tree_clear(cb_tree_t *tree)
{
	cb_node_t sentinel;
	sentinel.child[0] = tree->root;
	if (sentinel.child[0].type != TYPE_EMPTY) {
		cbt_traverse_delete(tree, &sentinel, 0);
	}
	tree->root.type = TYPE_EMPTY;
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

	if (tree->root.type == TYPE_EMPTY) {
		return 0;
	}

	sentinel.child[0] = tree->root;
	top = p = &sentinel;
	tdirection = direction = 0;

	while (p->child[direction].type == TYPE_NODE) {
		cb_node_t *q = p->child[direction].ptr.node;
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

	leaf = (const char *)p->child[direction].ptr.leaf;
	if (strlen(leaf) < ulen || memcmp(leaf, prefix, ulen) != 0) {
		/* No strings match */
		return 0;
	}

	return cbt_traverse_prefixed(top, tdirection, callback, baton);
}
