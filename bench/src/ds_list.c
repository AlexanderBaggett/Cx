/* Linked lists: a singly linked list of heap nodes and an LRU cache built from
 * a doubly linked list plus an open-addressing index. */
#include "bench.h"

/* ---- slist: push-front, traverse, remove every k-th node, free ------------ */

enum { SL_N = 1 << 14, SL_ROUNDS = 48 };

struct sl_node { [[cx::owned]] struct sl_node *next; uint32_t val; };
struct slist { [[cx::owned]] struct sl_node *head; size_t len; };

static void sl_push_front(struct slist *l, uint32_t v) {
    struct sl_node *n = (struct sl_node *)bench_alloc(sizeof *n);
    n->val = v;
    n->next = l->head;
    l->head = n;
    l->len++;
}

/* Sum and order-dependent hash of the list. */
static uint64_t sl_traverse(const struct slist *l) {
    uint64_t sum = 0, h = 0;
    size_t pos = 0;
    for (const struct sl_node *n = l->head; n; n = n->next) {
        sum += n->val;
        if ((pos++ & 63u) == 0) h = mix(h, n->val);
    }
    return mix(h, sum);
}

/* Remove the nodes at positions k, 2k, 3k, ... (1-based, k >= 2). */
static uint64_t sl_remove_every(struct slist *l, size_t k) {
    uint64_t removed = 0;
    struct sl_node *prev = l->head;
    size_t pos = 1;
    while (prev && prev->next) {
        pos++;
        if (pos % k == 0) {
            struct sl_node *dead = prev->next;
            prev->next = dead->next;
            removed += dead->val;
            bench_free(dead);
            l->len--;
        } else {
            prev = prev->next;
        }
    }
    return removed;
}

static void sl_free_all(struct slist *l) {
    struct sl_node *n = l->head;
    l->head = NULL;
    l->len = 0;
    while (n) {
        struct sl_node *next = n->next;
        bench_free(n);
        n = next;
    }
}

struct sl_state { [[cx::owned]] uint32_t *vals; };

static void *sl_setup(void) {
    struct sl_state *s = (struct sl_state *)bench_alloc(sizeof *s);
    s->vals = (uint32_t *)bench_alloc(SL_N * sizeof(uint32_t));
    struct rng r = { 0xa4093822299f31d0u };
    for (size_t i = 0; i < SL_N; i++) s->vals[i] = rng_below(&r, 1u << 30);
    return s;
}

static uint64_t sl_run(void *state) {
    const struct sl_state *s = (const struct sl_state *)state;
    uint64_t h = 0;
    for (size_t round = 0; round < SL_ROUNDS; round++) {
        struct slist l = { NULL, 0 };
        for (size_t i = 0; i < SL_N; i++) sl_push_front(&l, s->vals[i]);
        h = mix(h, sl_traverse(&l));
        h = mix(h, sl_remove_every(&l, 3 + round));
        /* refill: the new nodes reuse the freed blocks, scattering the list */
        for (size_t i = 0; i < SL_N / 4; i++) sl_push_front(&l, s->vals[i] >> 1);
        h = mix(h, sl_traverse(&l));
        h = mix(h, sl_remove_every(&l, 2));
        h = mix(mix(h, sl_traverse(&l)), l.len);
        sl_free_all(&l);
    }
    return h;
}

static void sl_teardown([[cx::escapes]] void *state) {
    struct sl_state *s = (struct sl_state *)state;
    bench_free(s->vals);
    bench_free(s);
}

extern const struct bench bench_slist = {
    "slist", "ds", "singly linked list: push-front, traverse-sum, remove every k-th node, free",
    sl_setup, sl_run, sl_teardown,
};

/* ---- dlist_lru: LRU cache, doubly linked list + linear-probing index ------ */

enum {
    LRU_CAP = 1 << 15,          /* cached entries */
    LRU_SLOT_BITS = 16,
    LRU_SLOTS = 1 << LRU_SLOT_BITS,     /* index slots (load <= 0.5) */
    LRU_HOT = 26000,            /* hot keys ... */
    LRU_HOT_PCT = 89,           /* ... get this share of the requests: ~81% hits */
    LRU_OPS = 3 << 19,
};

/* List links are shared (not owned): every node is reachable from its
 * neighbours, from head/tail and from the index. */
struct lru_node { struct lru_node *prev; struct lru_node *next; uint32_t key; uint32_t val; };
struct lru_slot { struct lru_node *node; uint32_t key; };      /* node == NULL: empty */

struct lru {
    struct lru_node *head;      /* most recently used */
    struct lru_node *tail;      /* least recently used */
    [[cx::owned]] struct lru_slot *slots;
    size_t count;
};

static size_t lru_home(uint32_t key) {
    return (size_t)((((uint64_t)key * 0x9e3779b1u) & 0xffffffffu) >> (32 - LRU_SLOT_BITS));
}

static struct lru_node *lru_lookup(const struct lru *c, uint32_t key) {
    size_t i = lru_home(key);
    for (;;) {
        const struct lru_slot *sl = &c->slots[i];
        if (!sl->node) return NULL;
        if (sl->key == key) return sl->node;
        i = (i + 1) & (LRU_SLOTS - 1);
    }
}

static void lru_index_add(struct lru *c, [[cx::escapes]] struct lru_node *n) {
    size_t i = lru_home(n->key);
    while (c->slots[i].node) i = (i + 1) & (LRU_SLOTS - 1);
    c->slots[i].node = n;
    c->slots[i].key = n->key;
}

/* Remove a present key; backward-shift deletion keeps probe chains intact. */
static void lru_index_remove(struct lru *c, uint32_t key) {
    size_t i = lru_home(key);
    while (c->slots[i].key != key || !c->slots[i].node) i = (i + 1) & (LRU_SLOTS - 1);
    size_t j = i;
    for (;;) {
        j = (j + 1) & (LRU_SLOTS - 1);
        if (!c->slots[j].node) break;
        size_t home = lru_home(c->slots[j].key);
        if (((j + LRU_SLOTS - home) & (LRU_SLOTS - 1)) >= ((j + LRU_SLOTS - i) & (LRU_SLOTS - 1))) {
            c->slots[i] = c->slots[j];
            i = j;
        }
    }
    c->slots[i].node = NULL;
    c->slots[i].key = 0;
}

static void lru_unlink(struct lru *c, struct lru_node *n) {
    if (n->prev) n->prev->next = n->next; else c->head = n->next;
    if (n->next) n->next->prev = n->prev; else c->tail = n->prev;
}

static void lru_push_front(struct lru *c, [[cx::escapes]] struct lru_node *n) {
    n->prev = NULL;
    n->next = c->head;
    if (c->head) c->head->prev = n; else c->tail = n;
    c->head = n;
}

/* Insert a key that is not cached, evicting the least recently used entry
 * when full (its node is reused). */
static void lru_insert(struct lru *c, uint32_t key, uint32_t val) {
    struct lru_node *n;
    if (c->count == LRU_CAP) {
        n = c->tail;
        lru_unlink(c, n);
        lru_index_remove(c, n->key);
    } else {
        n = (struct lru_node *)bench_alloc(sizeof *n);
        c->count++;
    }
    n->key = key;
    n->val = val;
    lru_push_front(c, n);
    lru_index_add(c, n);
}

static void lru_touch(struct lru *c, [[cx::escapes]] struct lru_node *n) {
    if (c->head != n) {
        lru_unlink(c, n);
        lru_push_front(c, n);
    }
}

/* One request: key, and val == 0 for a get or val != 0 for a put. */
struct lru_op { uint32_t key; uint32_t val; };

struct lru_state { [[cx::owned]] struct lru_op *ops; };

static void *lru_setup(void) {
    struct lru_state *s = (struct lru_state *)bench_alloc(sizeof *s);
    s->ops = (struct lru_op *)bench_alloc(LRU_OPS * sizeof(struct lru_op));
    uint32_t *hot = (uint32_t *)bench_alloc(LRU_HOT * sizeof(uint32_t));
    struct rng r = { 0x082efa98ec4e6c89u };
    for (size_t i = 0; i < LRU_HOT; i++) hot[i] = (uint32_t)rng_next(&r);
    for (size_t i = 0; i < LRU_OPS; i++) {
        uint32_t pick = rng_below(&r, 100);
        uint32_t key = pick < LRU_HOT_PCT ? hot[rng_below(&r, LRU_HOT)] : (uint32_t)rng_next(&r);
        uint32_t kind = rng_below(&r, 4);
        s->ops[i].key = key;
        s->ops[i].val = kind == 0 ? 1 + rng_below(&r, 1u << 30) : 0u;
    }
    bench_free(hot);
    return s;
}

static uint64_t lru_run(void *state) {
    const struct lru_state *s = (const struct lru_state *)state;
    struct lru c = { NULL, NULL, NULL, 0 };
    c.slots = (struct lru_slot *)bench_zalloc(LRU_SLOTS * sizeof(struct lru_slot));
    uint64_t h = 0, hits = 0, sum = 0;
    for (size_t i = 0; i < LRU_OPS; i++) {
        uint32_t key = s->ops[i].key, val = s->ops[i].val;
        struct lru_node *n = lru_lookup(&c, key);
        if (n) {
            hits++;
            if (val) n->val = val;
            sum += n->val;
            lru_touch(&c, n);
        } else {
            /* miss: load the value (cache-aside) and insert it */
            lru_insert(&c, key, val ? val : (key >> 2) + 1);
        }
        if ((i & 1023u) == 0) h = mix(h, sum);
    }
    h = mix(mix(mix(h, hits), sum), c.count);
    /* walk from most to least recently used, then free */
    struct lru_node *p = c.head;
    size_t pos = 0;
    while (p) {
        struct lru_node *next = p->next;
        if ((pos++ & 255u) == 0) h = mix(h, p->key);
        bench_free(p);
        p = next;
    }
    bench_free(c.slots);
    return h;
}

static void lru_teardown([[cx::escapes]] void *state) {
    struct lru_state *s = (struct lru_state *)state;
    bench_free(s->ops);
    bench_free(s);
}

extern const struct bench bench_dlist_lru = {
    "dlist_lru", "ds", "LRU cache: doubly linked list + linear-probing index, ~80% hit get/put stream",
    lru_setup, lru_run, lru_teardown,
};
