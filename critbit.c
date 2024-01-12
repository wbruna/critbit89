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
static void cbt_traverse_delete(cb_tree_t *tree, cb_child_t p)
{
	if (p.type == TYPE_NODE) {
		cb_node_t *q = p.ptr.node;
		cbt_traverse_delete(tree, q->child[0]);
		cbt_traverse_delete(tree, q->child[1]);
		tree->free(q, tree->baton);
	} else {
		tree->free(p.ptr.leaf, tree->baton);
	}
}

static int cbt_traverse_prefixed(cb_child_t top,
	int (*callback)(const char *, void *), void *baton)
{
	if (top.type == TYPE_NODE) {
		cb_node_t *q = top.ptr.node;
		int ret = 0;

		ret = cbt_traverse_prefixed(q->child[0], callback, baton);
		if (ret != 0) {
			return ret;
		}
		ret = cbt_traverse_prefixed(q->child[1], callback, baton);
		if (ret != 0) {
			return ret;
		}
		return 0;
	}

	return (callback)((const char *)top.ptr.leaf, baton);
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
	cb_child_t p = tree->root;

	if (p.type == TYPE_EMPTY) {
		return 0;
	}

	while (p.type == TYPE_NODE) {
		cb_node_t *q = p.ptr.node;
		uint8_t c = 0;
		int direction;

		if (q->byte < ulen) {
			c = ubytes[q->byte];
		}
		direction = (1 + (q->otherbits | c)) >> 8;

		p = q->child[direction];
	}

	return (strcmp(str, (const char*)p.ptr.leaf) == 0);
}

/*! Inserts str into tree, returns 0 on success */
int cb_tree_insert(cb_tree_t *tree, const char *str)
{
	const uint8_t *const ubytes = (const uint8_t *)str;
	const size_t ulen = strlen(str);
	cb_child_t p = tree->root;
	uint8_t c, *x;
	uint32_t newbyte;
	uint32_t newotherbits;
	int direction, newdirection;
	cb_node_t *newnode;
	cb_child_t *wherep;

	if (p.type == TYPE_EMPTY) {
		x = (uint8_t *)tree->malloc(ulen + 1, tree->baton);
		if (x == NULL) {
			return ENOMEM;
		}
		memcpy(x, str, ulen + 1);
		tree->root.ptr.leaf = x;
		tree->root.type = TYPE_LEAF;
		return 0;
	}

	while (p.type == TYPE_NODE) {
		cb_node_t *q = p.ptr.node;
		c = 0;
		if (q->byte < ulen) {
			c = ubytes[q->byte];
		}
		direction = (1 + (q->otherbits | c)) >> 8;

		p = q->child[direction];
	}

	for (newbyte = 0; newbyte < ulen; ++newbyte) {
		if (p.ptr.leaf[newbyte] != ubytes[newbyte]) {
			newotherbits = p.ptr.leaf[newbyte] ^ ubytes[newbyte];
			goto different_byte_found;
		}
	}

	if (p.ptr.leaf[newbyte] != 0) {
		newotherbits = p.ptr.leaf[newbyte];
		goto different_byte_found;
	}
	return 1;

different_byte_found:
	newotherbits |= newotherbits >> 1;
	newotherbits |= newotherbits >> 2;
	newotherbits |= newotherbits >> 4;
	newotherbits = (newotherbits & ~(newotherbits >> 1)) ^ 255;
	c = p.ptr.leaf[newbyte];
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
	wherep = &tree->root;
	for (;;) {
		cb_node_t *q;
		p = *wherep;
		if (p.type == TYPE_LEAF) {
			break;
		}

		q = p.ptr.node;
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
		wherep = q->child + direction;
	}

	newnode->child[newdirection] = *wherep;
	wherep->ptr.node = newnode;
	wherep->type = TYPE_NODE;
	return 0;
}

/*! Deletes str from the tree, returns 0 on success */
int cb_tree_delete(cb_tree_t *tree, const char *str)
{
	const uint8_t *ubytes = (const uint8_t *)str;
	const size_t ulen = strlen(str);
	cb_child_t p = tree->root;
	cb_child_t *wherep = 0, *whereq = 0;
	cb_node_t *q = 0;
	int direction = 0;

	if (tree->root.type == TYPE_EMPTY) {
		return 1;
	}
	wherep = &tree->root;

	while (p.type == TYPE_NODE) {
		uint8_t c = 0;
		whereq = wherep;
		q = p.ptr.node;

		if (q->byte < ulen) {
			c = ubytes[q->byte];
		}
		direction = (1 + (q->otherbits | c)) >> 8;
		wherep = q->child + direction;
		p = *wherep;
	}

	if (strcmp(str, (const char *)p.ptr.leaf) != 0) {
		return 1;
	}
	tree->free(p.ptr.leaf, tree->baton);

	if (!whereq) {
		tree->root.type = TYPE_EMPTY;
		tree->root.ptr.leaf = NULL;
		return 0;
	}

	*whereq = q->child[1 - direction];
	tree->free(q, tree->baton);
	return 0;
}

/*! Clears the given tree */
void cb_tree_clear(cb_tree_t *tree)
{
	if (tree->root.type != TYPE_EMPTY) {
		cbt_traverse_delete(tree, tree->root);
	}
	tree->root.type = TYPE_EMPTY;
}

/*! Calls callback for all strings in tree with the given prefix */
int cb_tree_walk_prefixed(cb_tree_t *tree, const char *prefix,
	int (*callback)(const char *, void *), void *baton)
{
	const uint8_t *ubytes = (const uint8_t *)prefix;
	const size_t ulen = strlen(prefix);
	cb_child_t p = tree->root;
	cb_child_t top = p;

	if (p.type == TYPE_EMPTY) {
		return 0;
	}

	while (p.type == TYPE_NODE) {
		cb_node_t *q = p.ptr.node;
		uint8_t c = 0;
		int direction;

		if (q->byte < ulen) {
			c = ubytes[q->byte];
		}
		direction = (1 + (q->otherbits | c)) >> 8;

		p = q->child[direction];
		if (q->byte < ulen) {
			top = p;
		}
	}

	if (strlen((const char *)p.ptr.leaf) < ulen || memcmp(p.ptr.leaf, prefix, ulen) != 0) {
		/* No strings match */
		return 0;
	}

	return cbt_traverse_prefixed(top, callback, baton);
}
