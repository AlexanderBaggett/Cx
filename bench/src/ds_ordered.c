/* Ordered maps: a skip list of heap nodes and a B-tree. */
#include "bench.cxh"

#include <stdlib.h>

/* ---- skip_list: probabilistic skip list, insert/find/delete -------------- */

enum {
    SK_N = 1 << 16,             /* inserts (keys below 2^24: a few repeat) */
    SK_FINDS = 1 << 15,         /* lookups before and after the deletes */
    SK_MAX = 12,                /* maximum height; 4^12 is far above SK_N */
    SK_KEY_BITS = 24,
};

/* Links are shared, not owned: a node of height k is linked from k
 * predecessors. Level 0 links every node once, and the list is freed by
 * walking it. */
struct sk_node {
    uint32_t key;
    uint32_t val;
    uint32_t height;
    struct sk_node *next[];     /* height links, level 0 first */
};

struct skiplist {
    [[cx::owned]] struct sk_node *head;     /* sentinel with SK_MAX links */
    uint32_t level;                         /* levels in use, 1 .. SK_MAX */
    size_t count;
};

static struct sk_node *sk_new(uint32_t key, uint32_t val, uint32_t height) {
    struct sk_node *n = (struct sk_node *)bench_alloc(sizeof(struct sk_node) + height * sizeof(struct sk_node *));
    n->key = key;
    n->val = val;
    n->height = height;
    for (uint32_t i = 0; i < height; i++) n->next[i] = NULL;
    return n;
}

static void sk_init(struct skiplist *l) {
    l->head = sk_new(0, 0, SK_MAX);
    l->level = 1;
    l->count = 0;
}

/* Returns the value for key, or 0 if absent (stored values are never 0). */
static uint32_t sk_find(const struct skiplist *l, uint32_t key) {
    const struct sk_node *x = l->head;
    for (uint32_t i = l->level; i > 0; i--) {
        for (;;) {
            const struct sk_node *nx = x->next[i - 1];
            if (!nx || nx->key >= key) break;
            x = nx;
        }
    }
    const struct sk_node *c = x->next[0];
    return c && c->key == key ? c->val : 0;
}

/* Inserts key with a node of the given height, or updates its value; returns
 * true if a node was added. */
static bool sk_insert(struct skiplist *l, uint32_t key, uint32_t val, uint32_t height) {
    struct sk_node *update[SK_MAX];         /* last node before key, per level */
    struct sk_node *x = l->head;
    for (uint32_t i = l->level; i > 0; i--) {
        for (;;) {
            struct sk_node *nx = x->next[i - 1];
            if (!nx || nx->key >= key) break;
            x = nx;
        }
        update[i - 1] = x;
    }
    struct sk_node *c = x->next[0];
    if (c && c->key == key) {
        c->val = val;
        return false;
    }
    for (uint32_t i = l->level; i < height; i++) update[i] = l->head;
    if (height > l->level) l->level = height;
    struct sk_node *n = sk_new(key, val, height);
    for (uint32_t i = 0; i < height; i++) {
        n->next[i] = update[i]->next[i];
        update[i]->next[i] = n;
    }
    l->count++;
    return true;
}

/* Removes key; returns true if it was present. */
static bool sk_delete(struct skiplist *l, uint32_t key) {
    struct sk_node *update[SK_MAX];
    struct sk_node *x = l->head;
    for (uint32_t i = l->level; i > 0; i--) {
        for (;;) {
            struct sk_node *nx = x->next[i - 1];
            if (!nx || nx->key >= key) break;
            x = nx;
        }
        update[i - 1] = x;
    }
    struct sk_node *c = x->next[0];
    if (!c || c->key != key) return false;
    /* every level c is on is below l->level, and there update[i]->next[i] == c */
    for (uint32_t i = 0; i < c->height; i++) update[i]->next[i] = c->next[i];
    bench_free(c);
    while (l->level > 1 && l->head->next[l->level - 1] == NULL) l->level--;
    l->count--;
    return true;
}

static void sk_free(struct skiplist *l) {
    struct sk_node *n = l->head->next[0];
    while (n) {
        struct sk_node *next = n->next[0];
        bench_free(n);
        n = next;
    }
    bench_free(l->head);
    l->head = NULL;
}

/* Insert i adds keys[i] with a node of height[i] (geometric, p = 1/4, drawn
 * in setup); lookups probe[] are half inserted keys, half random keys. */
struct sk_state {
    [[cx::owned]] uint32_t *keys;
    [[cx::owned]] uint8_t *height;
    [[cx::owned]] uint32_t *probe;
};

static void *sk_setup(void) {
    struct sk_state *s = (struct sk_state *)bench_alloc(sizeof *s);
    s->keys = (uint32_t *)bench_alloc(SK_N * sizeof(uint32_t));
    s->height = (uint8_t *)bench_alloc(SK_N);
    s->probe = (uint32_t *)bench_alloc(SK_FINDS * sizeof(uint32_t));
    struct rng r = { 0x7b54a41dc25a59b5u };
    for (size_t i = 0; i < SK_N; i++) {
        s->keys[i] = rng_below(&r, 1u << SK_KEY_BITS);
        uint64_t x = rng_next(&r);
        uint32_t hgt = 1;
        while (hgt < SK_MAX && (x & 3u) == 0) {
            hgt++;
            x >>= 2;
        }
        s->height[i] = (uint8_t)hgt;
    }
    for (size_t i = 0; i < SK_FINDS; i++) {
        uint32_t hit = rng_below(&r, 2);
        s->probe[i] = hit ? s->keys[rng_below(&r, SK_N)] : rng_below(&r, 1u << SK_KEY_BITS);
    }
    return s;
}

static uint64_t sk_run(void *state) {
    const struct sk_state *s = (const struct sk_state *)state;
    struct skiplist l;
    sk_init(&l);
    uint64_t added = 0;
    for (size_t i = 0; i < SK_N; i++)
        added += sk_insert(&l, s->keys[i], (uint32_t)i + 1, s->height[i]) ? 1u : 0u;
    uint64_t h = mix(mix(0, added), l.level);
    uint64_t found = 0, sum = 0;
    for (size_t i = 0; i < SK_FINDS; i++) {
        uint32_t v = sk_find(&l, s->probe[i]);
        found += v != 0 ? 1u : 0u;
        sum += v;
    }
    h = mix(mix(h, found), sum);
    /* delete every other inserted key (repeated keys are gone the second
     * time), then look everything up again */
    uint64_t deleted = 0;
    for (size_t i = 0; i < SK_N; i += 2) deleted += sk_delete(&l, s->keys[i]) ? 1u : 0u;
    found = 0;
    sum = 0;
    for (size_t i = 0; i < SK_FINDS; i++) {
        uint32_t v = sk_find(&l, s->probe[i]);
        found += v != 0 ? 1u : 0u;
        sum += v;
    }
    h = mix(mix(mix(mix(h, deleted), found), sum), l.count);
    /* in-order walk along level 0 */
    uint64_t pos = 0, vsum = 0;
    for (const struct sk_node *n = l.head->next[0]; n; n = n->next[0]) {
        if ((pos & 63u) == 0) h = mix(h, n->key);
        vsum += n->val;
        pos++;
    }
    h = mix(mix(mix(h, pos), vsum), l.level);
    sk_free(&l);
    return h;
}

static void sk_teardown([[cx::escapes]] void *state) {
    struct sk_state *s = (struct sk_state *)state;
    bench_free(s->keys);
    bench_free(s->height);
    bench_free(s->probe);
    bench_free(s);
}

extern const struct bench bench_skip_list = {
    "skip_list", "ds", "skip list (p = 1/4) of uint32 keys: inserts, hit/miss finds, deletes, in-order walk",
    sk_setup, sk_run, sk_teardown,
};

/* ---- btree: B-tree inserts with splits, lookups, range scans ------------- */

enum {
    BT_T = 16,                  /* minimum degree: a node holds BT_T-1 .. 2*BT_T-1 keys */
    BT_MAXK = 2 * BT_T - 1,     /* 31 keys, 32 children */
    BT_N = 3 << 16,             /* inserts (keys below 2^24: a few repeat) */
    BT_LOOKUPS = 3 << 16,
    BT_RANGES = 3 << 10,        /* range scans ... */
    BT_RANGE_BITS = 17,         /* ... of width below 2^17: ~1000 keys on average */
    BT_KEY_BITS = 24,
    /* every node but the root holds at least BT_T-1 keys */
    BT_POOL = BT_N / (BT_T - 1) + 2,
};

struct bt_node {
    uint32_t n;                             /* keys in use */
    bool leaf;
    uint32_t key[BT_MAXK];                  /* ascending */
    uint32_t val[BT_MAXK];
    uint32_t child[BT_MAXK + 1];            /* child[0..n] of an internal node */
};

/* Nodes live in a pool allocated at full size by setup(), like the pages of
 * a database's buffer pool, and children are pool indices. run() builds the
 * tree in an empty pool and frees it by emptying the pool. (With one malloc
 * per ~400-byte node, glibc handed the freed heap back to the kernel at the
 * end of every run and page-faulted it back in during the next.) */
struct btree {
    [[cx::owned]] struct bt_node *pool;
    size_t len;
    size_t cap;
    uint32_t root;
    size_t count;
};

static uint32_t bt_new(struct btree *t, bool leaf) {
    if (t->len == t->cap) abort();          /* cannot happen: see BT_POOL */
    uint32_t id = (uint32_t)t->len;
    t->len++;
    t->pool[id].n = 0;
    t->pool[id].leaf = leaf;
    return id;
}

/* Index of the first key of x that is >= key (x->n if none). */
static uint32_t bt_lower(const struct bt_node *x, uint32_t key) {
    uint32_t i = 0;
    while (i < x->n && x->key[i] < key) i++;
    return i;
}

/* Split the full child y = child i of node xi (which is not full): y keeps
 * its lower BT_T-1 keys, a new right sibling takes the upper BT_T-1, and the
 * median moves up into xi. */
static void bt_split_child(struct btree *t, uint32_t xi, uint32_t i) {
    uint32_t yi = t->pool[xi].child[i];
    uint32_t zi = bt_new(t, t->pool[yi].leaf);
    struct bt_node *x = &t->pool[xi], *y = &t->pool[yi], *z = &t->pool[zi];
    z->n = BT_T - 1;
    for (uint32_t j = 0; j < BT_T - 1; j++) {
        z->key[j] = y->key[j + BT_T];
        z->val[j] = y->val[j + BT_T];
    }
    if (!y->leaf)
        for (uint32_t j = 0; j < BT_T; j++) z->child[j] = y->child[j + BT_T];
    y->n = BT_T - 1;
    for (uint32_t j = x->n; j > i; j--) {
        x->child[j + 1] = x->child[j];
        x->key[j] = x->key[j - 1];
        x->val[j] = x->val[j - 1];
    }
    x->child[i + 1] = zi;
    x->key[i] = y->key[BT_T - 1];
    x->val[i] = y->val[BT_T - 1];
    x->n++;
}

/* Inserts key or updates its value; returns true if a key was added. Full
 * nodes are split on the way down, so a split never propagates upwards. */
static bool bt_insert(struct btree *t, uint32_t key, uint32_t val) {
    if (t->pool[t->root].n == BT_MAXK) {
        uint32_t ri = bt_new(t, false);
        t->pool[ri].child[0] = t->root;
        t->root = ri;
        bt_split_child(t, ri, 0);
    }
    uint32_t xi = t->root;
    for (;;) {
        struct bt_node *x = &t->pool[xi];
        uint32_t i = bt_lower(x, key);
        if (i < x->n && x->key[i] == key) {
            x->val[i] = val;
            return false;
        }
        if (x->leaf) {
            for (uint32_t j = x->n; j > i; j--) {
                x->key[j] = x->key[j - 1];
                x->val[j] = x->val[j - 1];
            }
            x->key[i] = key;
            x->val[i] = val;
            x->n++;
            t->count++;
            return true;
        }
        if (t->pool[x->child[i]].n == BT_MAXK) {
            bt_split_child(t, xi, i);
            if (key == x->key[i]) {
                x->val[i] = val;
                return false;
            }
            if (key > x->key[i]) i++;
        }
        xi = x->child[i];
    }
}

/* Returns the value for key, or 0 if absent (stored values are never 0). */
static uint32_t bt_find(const struct btree *t, uint32_t key) {
    const struct bt_node *x = &t->pool[t->root];
    for (;;) {
        uint32_t i = bt_lower(x, key);
        if (i < x->n && x->key[i] == key) return x->val[i];
        if (x->leaf) return 0;
        x = &t->pool[x->child[i]];
    }
}

struct bt_acc { uint64_t count; uint64_t sum; uint64_t h; };

/* Visit the keys in [lo, hi] under node xi in ascending order. */
static void bt_range(const struct btree *t, uint32_t xi, uint32_t lo, uint32_t hi, struct bt_acc *acc) {
    const struct bt_node *x = &t->pool[xi];
    for (uint32_t i = bt_lower(x, lo); i < x->n; i++) {
        if (!x->leaf) bt_range(t, x->child[i], lo, hi, acc);
        if (x->key[i] > hi) return;
        if ((acc->count & 63u) == 0) acc->h = mix(acc->h, x->key[i]);
        acc->count++;
        acc->sum += x->val[i];
    }
    if (!x->leaf) bt_range(t, x->child[x->n], lo, hi, acc);
}

static uint32_t bt_height(const struct btree *t) {
    uint32_t hgt = 1;
    for (const struct bt_node *x = &t->pool[t->root]; !x->leaf; x = &t->pool[x->child[0]]) hgt++;
    return hgt;
}

struct bt_state {
    [[cx::owned]] uint32_t *keys;
    [[cx::owned]] uint32_t *probe;          /* half inserted keys, half random */
    [[cx::owned]] uint32_t *range_lo;
    [[cx::owned]] uint32_t *range_hi;
    struct btree t;
};

static void *bt_setup(void) {
    struct bt_state *s = (struct bt_state *)bench_alloc(sizeof *s);
    s->keys = (uint32_t *)bench_alloc(BT_N * sizeof(uint32_t));
    s->probe = (uint32_t *)bench_alloc(BT_LOOKUPS * sizeof(uint32_t));
    s->range_lo = (uint32_t *)bench_alloc(BT_RANGES * sizeof(uint32_t));
    s->range_hi = (uint32_t *)bench_alloc(BT_RANGES * sizeof(uint32_t));
    s->t.pool = (struct bt_node *)bench_alloc(BT_POOL * sizeof(struct bt_node));
    s->t.len = 0;
    s->t.cap = BT_POOL;
    struct rng r = { 0x5a5b6ad8c1fa7d95u };
    for (size_t i = 0; i < BT_N; i++) s->keys[i] = rng_below(&r, 1u << BT_KEY_BITS);
    for (size_t i = 0; i < BT_LOOKUPS; i++) {
        uint32_t hit = rng_below(&r, 2);
        s->probe[i] = hit ? s->keys[rng_below(&r, BT_N)] : rng_below(&r, 1u << BT_KEY_BITS);
    }
    for (size_t i = 0; i < BT_RANGES; i++) {
        s->range_lo[i] = rng_below(&r, 1u << BT_KEY_BITS);
        s->range_hi[i] = s->range_lo[i] + rng_below(&r, 1u << BT_RANGE_BITS);
    }
    return s;
}

static uint64_t bt_run(void *state) {
    struct bt_state *s = (struct bt_state *)state;
    struct btree *t = &s->t;
    t->len = 0;
    t->count = 0;
    t->root = bt_new(t, true);
    uint64_t added = 0;
    for (size_t i = 0; i < BT_N; i++) added += bt_insert(t, s->keys[i], (uint32_t)i + 1) ? 1u : 0u;
    uint64_t h = mix(mix(mix(mix(0, added), t->count), t->len), bt_height(t));
    uint64_t found = 0, sum = 0;
    for (size_t i = 0; i < BT_LOOKUPS; i++) {
        uint32_t v = bt_find(t, s->probe[i]);
        found += v != 0 ? 1u : 0u;
        sum += v;
    }
    h = mix(mix(h, found), sum);
    for (size_t i = 0; i < BT_RANGES; i++) {
        struct bt_acc acc = { 0, 0, 0 };
        bt_range(t, t->root, s->range_lo[i], s->range_hi[i], &acc);
        h = mix(mix(mix(h, acc.count), acc.sum), acc.h);
    }
    t->len = 0;                             /* free every node */
    return h;
}

static void bt_teardown([[cx::escapes]] void *state) {
    struct bt_state *s = (struct bt_state *)state;
    bench_free(s->keys);
    bench_free(s->probe);
    bench_free(s->range_lo);
    bench_free(s->range_hi);
    bench_free(s->t.pool);
    bench_free(s);
}

extern const struct bench bench_btree = {
    "btree", "ds", "B-tree (min degree 16, pooled nodes) of uint32 keys: inserts with splits, hit/miss lookups, range scans",
    bt_setup, bt_run, bt_teardown,
};
