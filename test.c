/*
 * critbit89 - A crit-bit tree implementation for strings in C89
 * Written by Jonas Gehring <jonas@jgehring.net>
 */


#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "critbit.h"


/*
 * Sample dictionary: 100 random words from /usr/share/dict/words
 * Generated using random.org:
 * MAX=`wc -l < /usr/share/dict/words | tr -d " "`
 * for i in `curl "http://www.random.org/integers/?num=100&min=1&max=$MAX&col=1&base=10&format=plain&rnd=new"`; do
 *   nl /usr/share/dict/words | grep -w $i | tr -d "0-9\t "
 * done
 */
static const char *dict[] = {
	"catagmatic", "prevaricator", "statoscope", "workhand", "benzamide",
	"alluvia", "fanciful", "bladish", "Tarsius", "unfast", "appropriative",
	"seraphically", "monkeypod", "deflectometer", "tanglesome", "zodiacal",
	"physiologically", "economizer", "forcepslike", "betrumpet",
	"Danization", "broadthroat", "randir", "usherette", "nephropyosis",
	"hematocyanin", "chrysohermidin", "uncave", "mirksome", "podophyllum",
	"siphonognathous", "indoor", "featheriness", "forwardation",
	"archruler", "soricoid", "Dailamite", "carmoisin", "controllability",
	"unpragmatical", "childless", "transumpt", "productive",
	"thyreotoxicosis", "oversorrow", "disshadow", "osse", "roar",
	"pantomnesia", "talcer", "hydrorrhoea", "Satyridae", "undetesting",
	"smoothbored", "widower", "sivathere", "pendle", "saltation",
	"autopelagic", "campfight", "unexplained", "Macrorhamphosus",
	"absconsa", "counterflory", "interdependent", "triact", "reconcentration",
	"oversharpness", "sarcoenchondroma", "superstimulate", "assessory",
	"pseudepiscopacy", "telescopically", "ventriloque", "politicaster",
	"Caesalpiniaceae", "inopportunity", "Helion", "uncompatible",
	"cephaloclasia", "oversearch", "Mahayanistic", "quarterspace",
	"bacillogenic", "hamartite", "polytheistical", "unescapableness",
	"Pterophorus", "cradlemaking", "Hippoboscidae", "overindustrialize",
	"perishless", "cupidity", "semilichen", "gadge", "detrimental",
	"misencourage", "toparchia", "lurchingly", "apocatastasis"
};

#define dict_size (sizeof(dict) / sizeof(const char *))

static int tnum = 0;


/* Insertions */
static void test_insert(cb_tree_t *tree)
{
	int i;

	for (i = 0; i < dict_size; i++) {
		if (cb_tree_insert(tree, dict[i]) != 0) {
			fprintf(stderr, "Insertion failed\n");
			abort();
		}
	}
}

/* Insertion of duplicate element */
static void test_insert_dup(cb_tree_t *tree)
{
	int i;

	for (i = 0; i < dict_size; i++) {
		if (!cb_tree_contains(tree, dict[i])) {
			continue;
		}
		if (cb_tree_insert(tree, dict[i]) != 1) {
			fprintf(stderr, "Insertion of duplicate '%s' should fail\n", dict[i]);
			abort();
		}
	}
}

/* Searching */
static void test_contains(cb_tree_t *tree)
{
	char *in;
	const char *notin = "not in tree";

	in = (char*)malloc(strlen(dict[23 % dict_size])+1);
	strcpy(in, dict[23 % dict_size]);

	if (cb_tree_contains(tree, in) != 1) {
		fprintf(stderr, "Tree should contain '%s'\n", in);
		abort();
	}
	if (cb_tree_contains(tree, notin) != 0) {
		fprintf(stderr, "Tree should not contain '%s'\n", notin);
		abort();
	}
	if (cb_tree_contains(tree, "") != 0) {
		fprintf(stderr, "Tree should not contain empty string\n");
		abort();
	}
	in[strlen(in)/2] = '\0';
	if (cb_tree_contains(tree, in) != 0) {
		fprintf(stderr, "Tree should not contain prefix of '%s'\n", in);
		abort();
	}

	free(in);
}

/* Count number of items */
static int count_cb(const char *s, void *n) { (*(int *)n)++; return 0; }
static void test_complete(cb_tree_t *tree, int n)
{
	int i = 0;
	if (cb_tree_walk_prefixed(tree, "", count_cb, &i) != 0) {
		fprintf(stderr, "Walking with empty prefix failed\n");
		abort();
	}
	if (i != n) {
		fprintf(stderr, "%d items expected, but %d walked\n", n, i);
		abort();
	}
}

/* Deletion */
static void test_delete(cb_tree_t *tree)
{
	if (cb_tree_delete(tree, dict[91 % dict_size]) != 0) {
		fprintf(stderr, "Deletion failed\n");
		abort();
	}
	if (cb_tree_delete(tree, "most likely not in tree") != 1) {
		fprintf(stderr, "Deletion of item not in tree should fail\n");
		abort();
	}
}

/* Complete deletion */
static void test_delete_all(cb_tree_t *tree)
{
	int i;

	for (i = 0; i < dict_size; i++) {
		if (!cb_tree_contains(tree, dict[i])) {
			continue;
		}
		if (cb_tree_delete(tree, dict[i]) != 0) {
			fprintf(stderr, "Deletion of '%s' failed\n", dict[i]);
			abort();
		}
	}
}

/* Fake allocator */
static void *fake_malloc(size_t s, void *b) { return NULL; }
static void test_allocator(cb_tree_t *unused)
{
	cb_tree_t tree = cb_tree_make();
	tree.malloc = fake_malloc;
	if (cb_tree_insert(&tree, dict[0]) != ENOMEM) {
		fprintf(stderr, "ENOMEM failure expected\n");
		abort();
	}
}

/* Empty tree */
static void test_empty(cb_tree_t *tree)
{
	if (cb_tree_contains(tree, dict[1]) != 0) {
		fprintf(stderr, "Empty tree expected\n");
		abort();
	}
	if (cb_tree_delete(tree, dict[1]) == 0) {
		fprintf(stderr, "Empty tree expected\n");
		abort();
	}
}

/* Prefix walking */
static void test_prefixes(cb_tree_t *tree)
{
	int i = 0;
	if ((cb_tree_insert(tree, "1str") != 0) ||
			(cb_tree_insert(tree, "11str2") != 0) ||
			(cb_tree_insert(tree, "12str") != 0) ||
			(cb_tree_insert(tree, "11str") != 0)) {
		fprintf(stderr, "Insertion failed\n");
		abort();
	}

	if (cb_tree_walk_prefixed(tree, "11", count_cb, &i) != 0) {
		fprintf(stderr, "Walking with prefix failed\n");
		abort();
	}
	if (i != 2) {
		fprintf(stderr, "2 items expected, but %d walked\n", i);
		abort();
	}

	i = 0;
	if (cb_tree_walk_prefixed(tree, "13", count_cb, &i) != 0) {
		fprintf(stderr, "Walking with non-matching prefix failed\n");
		abort();
	}
	if (i != 0) {
		fprintf(stderr, "0 items expected, but %d walked\n", i);
		abort();
	}

	i = 0;
	if (cb_tree_walk_prefixed(tree, "12345678", count_cb, &i) != 0) {
		fprintf(stderr, "Walking with long prefix failed\n");
		abort();
	}
	if (i != 0) {
		fprintf(stderr, "0 items expected, but %d walked\n", i);
		abort();
	}

	i = 0;
	if (cb_tree_walk_prefixed(tree, "11str", count_cb, &i) != 0) {
		fprintf(stderr, "Walking with exactly matching prefix failed\n");
		abort();
	}
	if (i != 2) {
		fprintf(stderr, "2 items expected, but %d walked\n", i);
		abort();
	}
}

#define TESTRANDOM_RANGE 4096
#define TESTRANDOM_LOOPS 100

char randombuf[TESTRANDOM_RANGE];

static void test_random (cb_tree_t *tree, int seed)
{
	int i;
	int complaints = 0;
	srand(seed);
	for (i = 0; i < TESTRANDOM_RANGE * TESTRANDOM_LOOPS; i++) {
		char key[10];
		int v = rand() % TESTRANDOM_RANGE;
		sprintf(key, "%x", v);
		if (randombuf[v]) {
			/* key should be inside the tree */
			if (!cb_tree_contains(tree, key)) {
				fprintf(stderr, "Random test (seed=%d): Tree should contain '%s'\n", seed, key);
				if (complaints++ < 10) abort();
			}
			if (cb_tree_delete(tree, key) != 0) {
				fprintf(stderr, "Random test (seed=%d): Deletion of '%s' failed\n", seed, key);
				if (complaints++ < 10) abort();
			}
			if (cb_tree_contains(tree, key)) {
				fprintf(stderr, "Random test (seed=%d): Tree should not contain '%s'\n", seed, key);
				if (complaints++ < 10) abort();
			}
			randombuf[v] = 0;
		}
		else {
			if (cb_tree_contains(tree, key)) {
				fprintf(stderr, "Random test (seed=%d): Tree should not contain '%s'\n", seed, key);
				if (complaints++ < 10) abort();
			}
			if (cb_tree_insert(tree, key) != 0) {
				fprintf(stderr, "Random test (seed=%d): Insertion of '%s' failed\n", seed, key);
				if (complaints++ < 10) abort();
			}
			if (!cb_tree_contains(tree, key)) {
				fprintf(stderr, "Random test (seed=%d): Tree should contain '%s'\n", seed, key);
				if (complaints++ < 10) abort();
			}
			randombuf[v] = 1;
		}
	}
}

/* Program entry point */
int main(int argc, char **argv)
{
	cb_tree_t tree = cb_tree_make();

	printf("%d ", ++tnum); fflush(stdout);
	test_insert(&tree);

	printf("%d ", ++tnum); fflush(stdout);
	test_complete(&tree, dict_size);

	printf("%d ", ++tnum); fflush(stdout);
	test_insert_dup(&tree);

	printf("%d ", ++tnum); fflush(stdout);
	test_contains(&tree);

	printf("%d ", ++tnum); fflush(stdout);
	test_delete(&tree);

	printf("%d ", ++tnum); fflush(stdout);
	cb_tree_clear(&tree);

	test_insert(&tree);
	test_complete(&tree, dict_size);

	printf("%d ", ++tnum); fflush(stdout);
	test_delete_all(&tree);

	printf("%d ", ++tnum); fflush(stdout);
	test_complete(&tree, 0);

	printf("%d ", ++tnum); fflush(stdout);
	test_allocator(&tree);

	printf("%d ", ++tnum); fflush(stdout);
	cb_tree_clear(&tree);
	test_empty(&tree);

	printf("%d ", ++tnum); fflush(stdout);
	test_insert(&tree);
	test_prefixes(&tree);

	cb_tree_clear(&tree);

	if (argc > 1) {
		int pr = 0;
		const char * arg = argv[1];
		int seed;
		printf("%d ", ++tnum); fflush(stdout);
		if (*arg == 'p') {
			pr = 1;
			arg++;
		}
		if (*arg == 0 || *arg == 'r') {
			seed = (int) time(NULL);
		}
		else {
			seed = atoi(argv[1]);
		}
		test_random(&tree, seed);
		if (pr)
			cb_tree_print(&tree);
		cb_tree_clear(&tree);
	}

	printf("ok\n");
	return 0;
}
