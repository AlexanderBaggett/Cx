/* Trees: an AVL search tree of heap nodes and an array-backed 26-way trie. */
#include "bench.h"

#include <stdlib.h>
#include <string.h>

/* ---- bst_avl: AVL tree inserts, hit/miss lookups, in-order traversal ------ */

enum { AVL_N = 1 << 16, AVL_LOOKUPS = 1 << 17 };

/* Each node owns its subtrees. */
struct avl {
    [[cx::owned]] struct avl *left;
    [[cx::owned]] struct avl *right;
    uint32_t key;
    uint32_t val;
    int32_t height;
};

static int32_t avl_height([[cx::nullable]] const struct avl *n) {
    return n ? n->height : 0;
}

static void avl_update(struct avl *n) {
    int32_t hl = avl_height(n->left), hr = avl_height(n->right);
    n->height = (hl > hr ? hl : hr) + 1;
}

/* Rotations work on the link that owns the subtree root, so ownership moves
 * by overwriting links: no node is ever owned by two links after a step. */
static void avl_rotate_right(struct avl **link) {
    struct avl *y = *link;
    struct avl *x = y->left;
    y->left = x->right;
    x->right = y;
    avl_update(y);
    avl_update(x);
    *link = x;
}

static void avl_rotate_left(struct avl **link) {
    struct avl *y = *link;
    struct avl *x = y->right;
    y->right = x->left;
    x->left = y;
    avl_update(y);
    avl_update(x);
    *link = x;
}

static void avl_rebalance(struct avl **link) {
    struct avl *n = *link;
    int32_t bal = avl_height(n->left) - avl_height(n->right);
    if (bal > 1) {
        if (avl_height(n->left->left) < avl_height(n->left->right)) avl_rotate_left(&n->left);
        avl_rotate_right(link);
    } else if (bal < -1) {
        if (avl_height(n->right->right) < avl_height(n->right->left)) avl_rotate_right(&n->right);
        avl_rotate_left(link);
    } else {
        avl_update(n);
    }
}

/* Inserts or updates key under *link; returns true if a node was added. */
static bool avl_insert(struct avl **link, uint32_t key, uint32_t val) {
    struct avl *n = *link;
    if (!n) {
        n = (struct avl *)bench_alloc(sizeof *n);
        n->left = NULL;
        n->right = NULL;
        n->key = key;
        n->val = val;
        n->height = 1;
        *link = n;
        return true;
    }
    bool added;
    if (key < n->key) {
        added = avl_insert(&n->left, key, val);
    } else if (key > n->key) {
        added = avl_insert(&n->right, key, val);
    } else {
        n->val = val;
        return false;
    }
    if (added) avl_rebalance(link);
    return added;
}

/* Returns the value for key, or 0 if absent (stored values are never 0). */
static uint32_t avl_find([[cx::nullable]] const struct avl *root, uint32_t key) {
    const struct avl *n = root;
    while (n) {
        if (key < n->key) n = n->left;
        else if (key > n->key) n = n->right;
        else return n->val;
    }
    return 0;
}

struct avl_acc { uint64_t h; uint64_t sum; uint64_t count; };

static void avl_walk([[cx::nullable]] const struct avl *n, struct avl_acc *acc) {
    if (!n) return;
    avl_walk(n->left, acc);
    if ((acc->count++ & 15u) == 0) acc->h = mix(acc->h, n->key);
    acc->sum += n->val;
    avl_walk(n->right, acc);
}

static void avl_free([[cx::escapes, cx::nullable]] struct avl *n) {
    if (!n) return;
    avl_free(n->left);
    avl_free(n->right);
    bench_free(n);
}

struct avl_state { [[cx::owned]] uint32_t *keys; [[cx::owned]] uint32_t *probe; };

static void *avl_setup(void) {
    struct avl_state *s = (struct avl_state *)bench_alloc(sizeof *s);
    s->keys = (uint32_t *)bench_alloc(AVL_N * sizeof(uint32_t));
    s->probe = (uint32_t *)bench_alloc(AVL_LOOKUPS * sizeof(uint32_t));
    struct rng r = { 0xc0ac29b7c97c50ddu };
    /* keys below 2^24: a few repeat, which exercises the update path */
    for (size_t i = 0; i < AVL_N; i++) s->keys[i] = rng_below(&r, 1u << 24);
    /* half of the lookups are inserted keys, half are random (mostly misses) */
    for (size_t i = 0; i < AVL_LOOKUPS; i++) {
        uint32_t hit = rng_below(&r, 2);
        s->probe[i] = hit ? s->keys[rng_below(&r, AVL_N)] : rng_below(&r, 1u << 24);
    }
    return s;
}

static uint64_t avl_run(void *state) {
    const struct avl_state *s = (const struct avl_state *)state;
    struct avl *root = NULL;
    uint64_t added = 0;
    for (size_t i = 0; i < AVL_N; i++) added += avl_insert(&root, s->keys[i], (uint32_t)i + 1) ? 1u : 0u;
    uint64_t h = mix(mix(0, added), (uint64_t)avl_height(root));
    uint64_t found = 0, sum = 0;
    for (size_t i = 0; i < AVL_LOOKUPS; i++) {
        uint32_t v = avl_find(root, s->probe[i]);
        found += v != 0 ? 1u : 0u;
        sum += v;
    }
    h = mix(mix(h, found), sum);
    struct avl_acc acc = { 0, 0, 0 };
    avl_walk(root, &acc);
    h = mix(mix(mix(h, acc.h), acc.sum), acc.count);
    avl_free(root);
    return h;
}

static void avl_teardown([[cx::escapes]] void *state) {
    struct avl_state *s = (struct avl_state *)state;
    bench_free(s->keys);
    bench_free(s->probe);
    bench_free(s);
}

extern const struct bench bench_bst_avl = {
    "bst_avl", "ds", "AVL tree: random inserts with rotations, hit/miss lookups, in-order traversal",
    avl_setup, avl_run, avl_teardown,
};

/* ---- trie_words: 26-way trie over generated lowercase words --------------- */

enum {
    TRIE_WORDS = 3 << 15,       /* words inserted */
    TRIE_LOOKUPS = 3 << 16,     /* words looked up */
    TRIE_SYL = 40,              /* syllables the words are built from */
    TRIE_WORD_MAX = 16,
    TRIE_TOTAL = TRIE_WORDS + TRIE_WORDS / 2,
};

/* Nodes live in one growable array; child index 0 means "none" (node 0 is
 * the root, which is nobody's child). */
struct trie_node { uint32_t child[26]; uint32_t val; };
struct trie { [[cx::owned]] struct trie_node *nodes; size_t len; size_t cap; };

static uint32_t trie_new_node(struct trie *t) {
    if (t->len == t->cap) {
        size_t cap = t->cap * 2;
        struct trie_node *p = (struct trie_node *)realloc(t->nodes, cap * sizeof(struct trie_node));
        if (!p) abort();
        t->nodes = p;
        t->cap = cap;
    }
    memset(&t->nodes[t->len], 0, sizeof(struct trie_node));
    uint32_t id = (uint32_t)t->len;
    t->len++;
    return id;
}

static void trie_insert(struct trie *t, const uint8_t *w, size_t n, uint32_t val) {
    uint32_t cur = 0;
    for (size_t i = 0; i < n; i++) {
        size_t c = (size_t)(w[i] - 'a');
        uint32_t next = t->nodes[cur].child[c];
        if (next == 0) {
            next = trie_new_node(t);
            t->nodes[cur].child[c] = next;
        }
        cur = next;
    }
    t->nodes[cur].val = val;
}

/* Returns the value stored for the word, or 0 (absent or only a prefix). */
static uint32_t trie_find(const struct trie *t, const uint8_t *w, size_t n) {
    uint32_t cur = 0;
    for (size_t i = 0; i < n; i++) {
        cur = t->nodes[cur].child[(size_t)(w[i] - 'a')];
        if (cur == 0) return 0;
    }
    return t->nodes[cur].val;
}

/* Words 0 .. TRIE_WORDS-1 are inserted; words TRIE_WORDS .. are lookups only
 * (built from the same syllables, so many share prefixes or are present). */
struct trie_state {
    [[cx::owned]] uint8_t *pool;        /* word i at pool[i * TRIE_WORD_MAX], length len[i] */
    [[cx::owned]] uint8_t *len;
    [[cx::owned]] uint32_t *probe;      /* lookup stream: word indices */
};

static void *trie_setup(void) {
    struct trie_state *s = (struct trie_state *)bench_alloc(sizeof *s);
    s->pool = (uint8_t *)bench_alloc((size_t)TRIE_TOTAL * TRIE_WORD_MAX);
    s->len = (uint8_t *)bench_alloc(TRIE_TOTAL);
    s->probe = (uint32_t *)bench_alloc(TRIE_LOOKUPS * sizeof(uint32_t));
    struct rng r = { 0x3f84d5b5b5470917u };
    uint8_t syl[TRIE_SYL][3];
    uint8_t syl_len[TRIE_SYL];
    for (size_t i = 0; i < TRIE_SYL; i++) {
        syl_len[i] = (uint8_t)(1 + rng_below(&r, 3));
        for (size_t j = 0; j < 3; j++) syl[i][j] = (uint8_t)('a' + rng_below(&r, 26));
    }
    for (size_t i = 0; i < TRIE_TOTAL; i++) {
        uint8_t *w = &s->pool[i * TRIE_WORD_MAX];
        size_t n = 0;
        uint32_t parts = 2 + rng_below(&r, 4);
        for (uint32_t p = 0; p < parts; p++) {
            size_t k = rng_below(&r, TRIE_SYL);
            size_t sl = syl_len[k];
            if (n + sl > TRIE_WORD_MAX) break;
            for (size_t j = 0; j < sl; j++) w[n++] = syl[k][j];
        }
        s->len[i] = (uint8_t)n;
    }
    for (size_t i = 0; i < TRIE_LOOKUPS; i++) s->probe[i] = rng_below(&r, TRIE_TOTAL);
    return s;
}

static uint64_t trie_run(void *state) {
    const struct trie_state *s = (const struct trie_state *)state;
    struct trie t = { NULL, 0, 1024 };
    t.nodes = (struct trie_node *)bench_alloc(t.cap * sizeof(struct trie_node));
    trie_new_node(&t);                              /* root */
    for (size_t i = 0; i < TRIE_WORDS; i++)
        trie_insert(&t, &s->pool[i * TRIE_WORD_MAX], s->len[i], (uint32_t)i + 1);
    uint64_t h = mix(0, t.len), found = 0, sum = 0;
    for (size_t i = 0; i < TRIE_LOOKUPS; i++) {
        uint32_t k = s->probe[i];
        uint32_t v = trie_find(&t, &s->pool[(size_t)k * TRIE_WORD_MAX], s->len[k]);
        found += v != 0 ? 1u : 0u;
        sum += v;
        if ((i & 255u) == 0) h = mix(h, sum);
    }
    h = mix(mix(h, found), sum);
    bench_free(t.nodes);
    return h;
}

static void trie_teardown([[cx::escapes]] void *state) {
    struct trie_state *s = (struct trie_state *)state;
    bench_free(s->pool);
    bench_free(s->len);
    bench_free(s->probe);
    bench_free(s);
}

extern const struct bench bench_trie_words = {
    "trie_words", "ds", "array-backed 26-way trie over generated words: insert and lookup",
    trie_setup, trie_run, trie_teardown,
};
