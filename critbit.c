/*
 * critbit89 - A crit-bit tree implementation for strings in C89
 * Written by Jonas Gehring <jonas@jgehring.net>
 */

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "critbit.h"

#define TYPE_LEAF 1
#define TYPE_NODE 2

/*
Prefix nodes have:
- the prefix as the left child;
- the subtree of keys that share this prefix on the right child;
- a byte position equal to the prefix length;
- the value 0xff for the bitmask.
For keys longer than the prefix, this mask will always direct the search to
proceed to the right child. Shorter keys will go to the left node, ending at
the prefix leaf.
*/
#define PREFIX_MASK 0xff

/*
The starting direction from the root node.
The root node is actually a sentinel: it always has a single child, on a
fixed direction, and the search starts one step ahead, only looking at the
child and its type.
*/
#define ROOT_DIRECTION 1

typedef unsigned char cb_byte_t;
#if UINT_MAX > (1 << 16)
  typedef unsigned int cb_keylen_t;
#else
  typedef unsigned long cb_keylen_t;
#endif

typedef struct {
	struct cb_node_t *node;
	cb_byte_t *leaf;
} cb_child_t;

typedef struct cb_node_t {
	cb_child_t child[2];
	cb_keylen_t byte;
	cb_byte_t type[2];
	cb_byte_t otherbits;
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
		tree->free((char*)q, tree->baton);
	}
}

static int cbt_traverse_prefixed(cb_node_t * par, int dir,
	int (*callback)(const cb_byte_t*, void *), void *baton)
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

	return (callback)(par->child[dir].leaf, baton);
}

static int numbit(cb_byte_t mask)
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
		printf("%s+-- %d N off=%d bit=%d\n", prefix, dir, (int)q->byte, numbit(q->otherbits));
		if (lprefix < MAX_PREFIX - 5)
			sprintf(prefix + lprefix, "%c   ", (dir || (!*prefix)) ? ' ' : '|');
		cbt_traverse_print(tree, q, 0, prefix);
		cbt_traverse_print(tree, q, 1, prefix);
		prefix[lprefix] = 0;
	}
	else {
		const char * leaf = (const char*)par->child[dir].leaf;
		printf("%s+-- %d L \"%s\"\n", prefix, dir, leaf ? leaf : "(nil)");
	}
}

void cb_tree_print(cb_tree_t *tree)
{
	char prefix[MAX_PREFIX] = "";
	puts("");
	if (tree->root == NULL) {
		puts("(empty tree)");
	}
	else {
		cbt_traverse_print(tree, tree->root, ROOT_DIRECTION, prefix);
	}
	puts("");
}

/*! Creates a new, empty critbit tree */
cb_tree_t cb_tree_make()
{
	cb_tree_t tree;
	tree.root = NULL;
	tree.malloc = &malloc_std;
	tree.free = &free_std;
	tree.baton = NULL;
	return tree;
}


static cb_keylen_t cb_get_keylen(const cb_byte_t * key)
{
	return strlen((const char*)key);
}

static int cb_tree_contains_i(cb_tree_t *tree, const cb_byte_t *ubytes, cb_keylen_t ulen)
{
	cb_node_t *p;
	int direction;
	const cb_byte_t *leaf;
	cb_keylen_t llen;

	if (tree->root == NULL) {
		return 0;
	}

	p = tree->root;
	direction = ROOT_DIRECTION;

	while (p->type[direction] == TYPE_NODE) {
		p = p->child[direction].node;
		direction = 0;
		if (p->byte < ulen) {
			cb_byte_t c = ubytes[p->byte];
			direction = (1 + (p->otherbits | c)) >> 8;
		}
	}

	leaf = p->child[direction].leaf;
	llen = cb_get_keylen(leaf);
	return (ulen == llen) && (memcmp(ubytes, leaf, ulen) == 0);
}

/*! Returns non-zero if tree contains str */
int cb_tree_contains(cb_tree_t *tree, const char *str)
{
	return cb_tree_contains_i (tree, (const cb_byte_t *)str, strlen(str));
}

static int cb_tree_insert_node(cb_tree_t *tree, cb_node_t *newnode, cb_byte_t *ubytes)
{
	const cb_keylen_t ulen = cb_get_keylen(ubytes);
	cb_node_t *p;
	cb_byte_t c;
	const cb_byte_t *leaf;
	cb_keylen_t llen, clen;
	cb_keylen_t newbyte;
	cb_keylen_t newotherbits;
	int direction, newdirection;

	if (tree->root == NULL) {
		memset (newnode, 0, sizeof (*newnode));
		newnode->child[ROOT_DIRECTION].leaf = ubytes;
		newnode->type[ROOT_DIRECTION] = TYPE_LEAF;
		tree->root = newnode;
		return 0;
	}

	p = tree->root;
	direction = ROOT_DIRECTION;

	while (p->type[direction] == TYPE_NODE) {
		p = p->child[direction].node;
		direction = 0;
		if (p->byte < ulen) {
			c = ubytes[p->byte];
			direction = (1 + (p->otherbits | c)) >> 8;
		}
	}

	leaf = p->child[direction].leaf;
	llen = strlen((const char*)leaf);

	/* compare the new key with the leaf */

	if (llen < ulen) {
		/* the leaf could be a prefix of the new key */
		clen = llen;
		newdirection = 0;
		newotherbits = PREFIX_MASK;
	}
	else if (ulen < llen) {
		/* the new key could be a prefix of the leaf */
		clen = ulen;
		newdirection = 1;
		newotherbits = PREFIX_MASK;
	}
	else {
		/* can't have prefixes */
		clen = ulen;
		/* newotherbits doubles as a "possibly equal" flag */
		newotherbits = 0;
	}

	for (newbyte = 0; newbyte < clen; ++newbyte) {
		if (leaf[newbyte] != ubytes[newbyte]) {
			newotherbits = leaf[newbyte] ^ ubytes[newbyte];
			/* different_byte_found */
			newotherbits |= newotherbits >> 1;
			newotherbits |= newotherbits >> 2;
			newotherbits |= newotherbits >> 4;
			/* (set just the bits above msb) | (move msb out of the way) */
			newotherbits = (newotherbits ^ 255) | (newotherbits >> 1);
			c = leaf[newbyte];
			newdirection = (1 + (newotherbits | c)) >> 8;
			break;
		}
	}

	if (newbyte == clen && newotherbits == 0) {
		return 1;
	}

	newnode->byte = newbyte;
	newnode->otherbits = newotherbits;
	newnode->child[1 - newdirection].leaf = ubytes;
	newnode->type[1 - newdirection] = TYPE_LEAF;

	/* Insert into tree */
	p = tree->root;
	direction = ROOT_DIRECTION;

	/* The prefix node mask shoud conceptually be lower than any other mask
	value, since a prefix will come before any other byte comparison with
	the same offset. However, its value is higher than any other mask.
	The increment-and-mask changes it to zero, without affecting any other
	comparison result. */
	newotherbits = (newotherbits + 1) & 0xff;

	while (p->type[direction] == TYPE_NODE) {
		cb_node_t *q = p->child[direction].node;
		if (q->byte >= newbyte) {
			if (q->byte > newbyte) {
				break;
			}
			else if (((q->otherbits + 1) & 0xff) > newotherbits) {
				break;
			}
		}

		direction = 0;
		if (q->byte < ulen) {
			c = ubytes[q->byte];
			direction = (1 + (q->otherbits | c)) >> 8;
		}
		p = q;
	}

	newnode->child[newdirection] = p->child[direction];
	newnode->type[newdirection] = p->type[direction];
	p->child[direction].node = newnode;
	p->type[direction] = TYPE_NODE;

	return 0;
}

/*! Inserts str into tree, returns 0 on success */
int cb_tree_insert(cb_tree_t *tree, const char *str)
{
	const size_t ulen = strlen(str);
	cb_byte_t *x;
	char * buffer;
	cb_node_t *newnode;
	int res;

	buffer = (char*)tree->malloc(sizeof (cb_node_t) + ulen + 1, tree->baton);
	if (buffer == NULL) {
		return ENOMEM;
	}

	newnode = (cb_node_t *) buffer;
	x = (cb_byte_t *)(buffer + sizeof (cb_node_t));
	memcpy(x, str, ulen + 1);
	res = cb_tree_insert_node (tree, newnode, x);
	if (res != 0) {
		free(buffer);
	}

	return res;
}

static int cb_tree_delete_i(cb_tree_t *tree, const cb_byte_t *ubytes,
  int offset_node_from_leaf, cb_byte_t ** deleted_leaf)
{
	const cb_keylen_t ulen = cb_get_keylen(ubytes);
	cb_node_t *p;
	cb_node_t *q;
	cb_node_t *lnode;
	int direction;
	int pdirection;
	cb_byte_t *leaf;
	cb_keylen_t llen;

	if (tree->root == NULL) {
		return 1;
	}

	p = NULL;
	q = tree->root;
	pdirection = direction = ROOT_DIRECTION;

	while (q->type[direction] == TYPE_NODE) {
		p = q;
		pdirection = direction;
		q = q->child[direction].node;

		direction = 0;
		if (q->byte < ulen) {
			cb_byte_t c = ubytes[q->byte];
			direction = (1 + (q->otherbits | c)) >> 8;
		}
	}

	leaf = q->child[direction].leaf;
	llen = cb_get_keylen(leaf);

	if (llen != ulen || memcmp(ubytes, leaf, ulen) != 0) {
		return 1;
	}

	/* get the node allocated together with this leaf */
	lnode = (cb_node_t*) ((char*)leaf + offset_node_from_leaf);

	if (p == NULL) {
		tree->root = NULL;
	}
	else if (lnode == q) {
		/* the leaf node will be unused */
		p->child[pdirection] = q->child[1 - direction];
		p->type[pdirection] = q->type[1 - direction];
	}
	else if (lnode == tree->root) {
		p->child[pdirection] = q->child[1 - direction];
		p->type[pdirection] = q->type[1 - direction];
		*q = *lnode;
		tree->root = q;
	}
	else {
		/* The leaf node it still in use inside the tree, as one of our
		ancestors: replace it with the removed node q.
		See https://dotat.at/prog/qp/blog-2015-10-07.html for a detailed argument
		about why the leaf node must always be an ancestor of the removed leaf. */
		cb_node_t *t = tree->root;
		int tdirection = ROOT_DIRECTION;
		while (t->type[tdirection] == TYPE_NODE) {
			if (t->child[tdirection].node == lnode) {
				p->child[pdirection] = q->child[1 - direction];
				p->type[pdirection] = q->type[1 - direction];
				*q = *lnode;
				t->child[tdirection].node = q;
				break;
			}
			t = t->child[tdirection].node;
			tdirection = 0;
			if (t->byte < ulen) {
				cb_byte_t c = ubytes[t->byte];
				tdirection = (1 + (t->otherbits | c)) >> 8;
			}
		}
		assert (t->type[tdirection] == TYPE_NODE);
	}

	*deleted_leaf = leaf;
	return 0;
}

/*! Deletes str from the tree, returns 0 on success */
int cb_tree_delete(cb_tree_t *tree, const char *str)
{
	const cb_byte_t *ubytes = (const cb_byte_t *)str;
	cb_byte_t *leaf;
	int res;
	int offset = -((int)sizeof(cb_node_t));

	res = cb_tree_delete_i(tree, ubytes, offset, &leaf);

	if (res == 0) {
		char* buffer = (char*)leaf + offset;
		free(buffer);
	}

	return res;
}

/*! Clears the given tree */
void cb_tree_clear(cb_tree_t *tree)
{
	if (tree->root != NULL) {
		cbt_traverse_delete(tree, tree->root, 0);
		tree->free(tree->root, tree->baton);
	}
	tree->root = NULL;
}

static int cb_tree_walk_prefixed_i(cb_tree_t *tree,
	const cb_byte_t *prefix, cb_keylen_t prefixlen,
	int (*callback)(const cb_byte_t *, void *), void *baton)
{
	cb_node_t *p;
	int direction;
	cb_node_t *top;
	int tdirection;
	const cb_byte_t *leaf;
	cb_keylen_t llen;

	if (tree->root == NULL) {
		return 0;
	}

	top = p = tree->root;
	tdirection = direction = ROOT_DIRECTION;

	while (p->type[direction] == TYPE_NODE) {
		cb_node_t *q = p->child[direction].node;

		direction = 0;
		if (q->byte < prefixlen) {
			cb_byte_t c = prefix[q->byte];
			direction = (1 + (q->otherbits | c)) >> 8;
			top = q;
			tdirection = direction;
		}

		p = q;
	}

	leaf = p->child[direction].leaf;
	llen = cb_get_keylen(leaf);
	if (llen < prefixlen || memcmp(leaf, prefix, prefixlen) != 0) {
		/* No strings match */
		return 0;
	}

	return cbt_traverse_prefixed(top, tdirection, callback, baton);
}

struct callback_str {
	int (*callback)(const char *, void *);
	void * baton;
};

static int callback_str_wrapper(const cb_byte_t * key, void * baton)
{
	struct callback_str *param = (struct callback_str *)baton;
	return param->callback((const char*)key, param->baton);
}

/*! Calls callback for all strings in tree with the given prefix */
int cb_tree_walk_prefixed(cb_tree_t *tree, const char *prefix,
	int (*callback)(const char *, void *), void *baton)
{
	struct callback_str param;
	param.callback = callback;
	param.baton = baton;
	return cb_tree_walk_prefixed_i(tree, (const cb_byte_t*)prefix,
	  strlen(prefix), callback_str_wrapper, &param);
}

