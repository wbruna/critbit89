/*
 * critbit89 - A crit-bit tree implementation for strings in C89
 * Written by Jonas Gehring <jonas@jgehring.net>
 */

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "critbit.h"

#define TYPE_LEAF 1
#define TYPE_NODE 2
#define TYPE_EMPTY 3

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

#define UNUSED_NODE 1

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
	} else {
		char *buffer = (char*)par->child[dir].leaf - sizeof(cb_node_t);
		cb_node_t *q = (cb_node_t *)buffer;
		if (q->otherbits == UNUSED_NODE) {
			tree->free(buffer, tree->baton);
		}
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
	cb_node_t sentinel;
	sentinel.child[0] = tree->root;
	sentinel.type[0] = tree->root_type;
	puts("");
	if (tree->root_type == TYPE_EMPTY) {
		puts("(empty tree)");
	}
	else {
		cbt_traverse_print(tree, &sentinel, 0, prefix);
	}
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


static cb_keylen_t cb_get_keylen(const cb_byte_t * key)
{
	return strlen((const char*)key);
}

static int cb_tree_contains_i(cb_tree_t *tree, const cb_byte_t *ubytes, cb_keylen_t ulen)
{
	cb_node_t *p;
	int direction;
	cb_node_t sentinel;
	const cb_byte_t *leaf;
	cb_keylen_t llen;

	if (tree->root_type == TYPE_EMPTY) {
		return 0;
	}

	sentinel.child[0] = tree->root;
	sentinel.type[0] = tree->root_type;
	p = &sentinel;
	direction = 0;

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
	cb_node_t sentinel;

	if (tree->root_type == TYPE_EMPTY) {
		memset (newnode, 0, sizeof (*newnode));
		newnode->otherbits = UNUSED_NODE;
		tree->root.leaf = ubytes;
		tree->root_type = TYPE_LEAF;
		return 0;
	}

	sentinel.child[0] = tree->root;
	sentinel.type[0] = tree->root_type;

	p = &sentinel;
	direction = 0;

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
	p = &sentinel;
	direction = 0;

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

	/* the root node may have been modified by the insertion */
	tree->root = sentinel.child[0];
	tree->root_type = sentinel.type[0];

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
	cb_node_t sentinel;
	cb_byte_t *leaf;
	cb_keylen_t llen;

	if (tree->root_type == TYPE_EMPTY) {
		return 1;
	}

	sentinel.child[0] = tree->root;
	sentinel.type[0] = tree->root_type;
	p = NULL;
	q = &sentinel;
	pdirection = direction = 0;

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

	if (lnode == q || lnode->otherbits == UNUSED_NODE) {
		/* the leaf node is, or will be, unused */
		if (!p) {
			sentinel.child[0].leaf = NULL;
			sentinel.type[0] = TYPE_EMPTY;
		}
		else {
			p->child[pdirection] = q->child[1 - direction];
			p->type[pdirection] = q->type[1 - direction];
			/* mark q as unused; it'll either be free'd, or become the unused node */
			q->otherbits = UNUSED_NODE;
		}
	}
	else {
		/* The leaf node it still in use inside the tree, as one of our
		ancestors: replace it with the removed node q.
		See https://dotat.at/prog/qp/blog-2015-10-07.html for a detailed argument
		about why the leaf node must always be an ancestor of the removed leaf. */
		cb_node_t *t = &sentinel;
		int tdirection = 0;
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

	/* the root node may have been modified by the deletion */
	tree->root = sentinel.child[0];
	tree->root_type = sentinel.type[0];

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
	if (tree->root_type != TYPE_EMPTY) {
		cb_node_t sentinel;
		sentinel.child[0] = tree->root;
		sentinel.type[0] = tree->root_type;
		cbt_traverse_delete(tree, &sentinel, 0);
	}
	tree->root_type = TYPE_EMPTY;
}

static int cb_tree_walk_prefixed_i(cb_tree_t *tree,
	const cb_byte_t *prefix, cb_keylen_t prefixlen,
	int (*callback)(const cb_byte_t *, void *), void *baton)
{
	cb_node_t *p;
	int direction;
	cb_node_t *top;
	int tdirection;
	cb_node_t sentinel;
	const cb_byte_t *leaf;
	cb_keylen_t llen;

	if (tree->root_type == TYPE_EMPTY) {
		return 0;
	}

	sentinel.child[0] = tree->root;
	sentinel.type[0] = tree->root_type;
	top = p = &sentinel;
	tdirection = direction = 0;

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

