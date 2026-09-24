/* Hash maps: open addressing with integer keys, separate chaining with
 * string keys. */
#include "bench.cxh"

#include <string.h>

/* ---- hash_int: linear probing uint32 -> uint32, backward-shift delete ----- */

enum { HI_N = 3 << 17, HI_MIN_BITS = 4 };

/* key 0 marks an empty slot; stored keys are never 0 */
struct hi_slot { uint32_t key; uint32_t val; };
struct hi_map { [[cx::owned]] struct hi_slot *slots; size_t mask; uint32_t bits; size_t count; };

/* Fibonacci hashing: the product of two values below 2^32 fits in 64 bits. */
static size_t hi_home(uint32_t key, uint32_t bits) {
    return (size_t)((((uint64_t)key * 0x9e3779b1u) & 0xffffffffu) >> (32u - bits));
}

static void hi_init(struct hi_map *m, uint32_t bits) {
    m->slots = (struct hi_slot *)bench_zalloc(((size_t)1 << bits) * sizeof(struct hi_slot));
    m->mask = ((size_t)1 << bits) - 1;
    m->bits = bits;
    m->count = 0;
}

static void hi_grow(struct hi_map *m) {
    struct hi_slot *old = m->slots;
    size_t old_cap = m->mask + 1;
    hi_init(m, m->bits + 1);
    for (size_t i = 0; i < old_cap; i++) {
        uint32_t k = old[i].key;
        if (k == 0) continue;
        size_t j = hi_home(k, m->bits);
        while (m->slots[j].key != 0) j = (j + 1) & m->mask;
        m->slots[j] = old[i];
        m->count++;
    }
    bench_free(old);
}

static void hi_put(struct hi_map *m, uint32_t key, uint32_t val) {
    if ((m->count + 1) * 4 > (m->mask + 1) * 3) hi_grow(m);
    size_t i = hi_home(key, m->bits);
    for (;;) {
        struct hi_slot *sl = &m->slots[i];
        if (sl->key == key) { sl->val = val; return; }
        if (sl->key == 0) { sl->key = key; sl->val = val; m->count++; return; }
        i = (i + 1) & m->mask;
    }
}

/* Returns the value, or 0 if absent (stored values are never 0). */
static uint32_t hi_get(const struct hi_map *m, uint32_t key) {
    size_t i = hi_home(key, m->bits);
    for (;;) {
        const struct hi_slot *sl = &m->slots[i];
        if (sl->key == key) return sl->val;
        if (sl->key == 0) return 0;
        i = (i + 1) & m->mask;
    }
}

static bool hi_del(struct hi_map *m, uint32_t key) {
    size_t i = hi_home(key, m->bits);
    for (;;) {
        uint32_t k = m->slots[i].key;
        if (k == 0) return false;
        if (k == key) break;
        i = (i + 1) & m->mask;
    }
    size_t cap = m->mask + 1;
    size_t j = i;
    for (;;) {
        j = (j + 1) & m->mask;
        uint32_t k = m->slots[j].key;
        if (k == 0) break;
        /* move j into the hole unless its home lies cyclically in (i, j] */
        size_t home = hi_home(k, m->bits);
        if (((j + cap - home) & m->mask) >= ((j + cap - i) & m->mask)) {
            m->slots[i] = m->slots[j];
            i = j;
        }
    }
    m->slots[i].key = 0;
    m->slots[i].val = 0;
    m->count--;
    return true;
}

struct hi_state { [[cx::owned]] uint32_t *keys; [[cx::owned]] uint32_t *miss; };

static uint32_t hi_nonzero(struct rng *r) {
    uint32_t k = (uint32_t)rng_next(r);
    return k ? k : 1u;
}

static void *hi_setup(void) {
    struct hi_state *s = (struct hi_state *)bench_alloc(sizeof *s);
    s->keys = (uint32_t *)bench_alloc(HI_N * sizeof(uint32_t));
    s->miss = (uint32_t *)bench_alloc(HI_N * sizeof(uint32_t));
    struct rng r = { 0x452821e638d01377u };
    for (size_t i = 0; i < HI_N; i++) s->keys[i] = hi_nonzero(&r);
    for (size_t i = 0; i < HI_N; i++) s->miss[i] = hi_nonzero(&r);
    return s;
}

static uint64_t hi_run(void *state) {
    const struct hi_state *s = (const struct hi_state *)state;
    struct hi_map m;
    hi_init(&m, HI_MIN_BITS);
    for (size_t i = 0; i < HI_N; i++) hi_put(&m, s->keys[i], (uint32_t)i + 1);
    uint64_t h = mix(0, m.count);
    /* lookups: every key once, interleaved with (mostly) missing keys */
    uint64_t found = 0, sum = 0;
    for (size_t i = 0; i < HI_N; i++) {
        uint32_t a = hi_get(&m, s->keys[i ^ 5]);
        uint32_t b = hi_get(&m, s->miss[i]);
        found += (a != 0 ? 1u : 0u) + (b != 0 ? 1u : 0u);
        sum += a + b;
    }
    h = mix(mix(h, found), sum);
    /* delete every other key, then look everything up again */
    uint64_t deleted = 0;
    for (size_t i = 0; i < HI_N; i += 2) deleted += hi_del(&m, s->keys[i]) ? 1u : 0u;
    sum = 0;
    for (size_t i = 0; i < HI_N; i++) sum += hi_get(&m, s->keys[i]);
    h = mix(mix(mix(h, deleted), sum), m.count);
    /* reinsert half of the deleted keys with new values */
    for (size_t i = 0; i < HI_N; i += 4) hi_put(&m, s->keys[i], (uint32_t)i + 7);
    sum = 0;
    for (size_t i = 0; i < HI_N; i += 3) sum += hi_get(&m, s->keys[i]);
    h = mix(mix(h, sum), m.count);
    bench_free(m.slots);
    return h;
}

static void hi_teardown([[cx::escapes]] void *state) {
    struct hi_state *s = (struct hi_state *)state;
    bench_free(s->keys);
    bench_free(s->miss);
    bench_free(s);
}

extern const struct bench bench_hash_int = {
    "hash_int", "ds", "linear-probing hash map uint32->uint32: insert/grow, hit/miss lookups, backward-shift delete",
    hi_setup, hi_run, hi_teardown,
};

/* ---- hash_str: separate chaining with string keys, FNV-1a ----------------- */

enum { HS_N = 3 << 15, HS_LOOKUPS = 3 << 16, HS_KEY_MAX = 24 };

struct hs_entry {
    [[cx::owned]] struct hs_entry *next;
    uint32_t hash;
    uint32_t len;
    uint32_t val;
    uint8_t key[];              /* copy of the key bytes */
};

struct hs_map {
    [[cx::owned]] struct hs_entry **buckets;    /* each bucket owns its chain */
    size_t mask;
    size_t count;
};

/* FNV-1a 32-bit, computed in 64-bit arithmetic: h < 2^32 and the prime is
 * below 2^25, so the product never exceeds 2^57. */
static uint32_t hs_fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = ((h ^ (uint64_t)p[i]) * 16777619u) & 0xffffffffu;
    return (uint32_t)h;
}

static void hs_grow(struct hs_map *m) {
    size_t old_n = m->mask + 1, n = old_n * 2;
    struct hs_entry **nb = (struct hs_entry **)bench_zalloc(n * sizeof(struct hs_entry *));
    for (size_t i = 0; i < old_n; i++) {
        struct hs_entry *e = m->buckets[i];
        m->buckets[i] = NULL;
        while (e) {
            struct hs_entry *next = e->next;
            size_t b = e->hash & (n - 1);
            e->next = nb[b];
            nb[b] = e;
            e = next;
        }
    }
    struct hs_entry **ob = m->buckets;
    m->buckets = nb;
    m->mask = n - 1;
    bench_free(ob);
}

/* Returns the entry for key, or NULL. */
static const struct hs_entry *hs_find(const struct hs_map *m, const uint8_t *key, size_t len, uint32_t hash) {
    for (const struct hs_entry *e = m->buckets[hash & m->mask]; e; e = e->next)
        if (e->hash == hash && e->len == len && memcmp(e->key, key, len) == 0) return e;
    return NULL;
}

static void hs_put(struct hs_map *m, const uint8_t *key, size_t len, uint32_t val) {
    uint32_t hash = hs_fnv1a(key, len);
    for (struct hs_entry *e = m->buckets[hash & m->mask]; e; e = e->next)
        if (e->hash == hash && e->len == len && memcmp(e->key, key, len) == 0) {
            e->val = val;
            return;
        }
    if (m->count >= m->mask + 1) hs_grow(m);
    struct hs_entry *e = (struct hs_entry *)bench_alloc(sizeof(struct hs_entry) + len);
    e->hash = hash;
    e->len = (uint32_t)len;
    e->val = val;
    memcpy(e->key, key, len);
    size_t b = hash & m->mask;
    e->next = m->buckets[b];
    m->buckets[b] = e;
    m->count++;
}

static uint32_t hs_get(const struct hs_map *m, const uint8_t *key, size_t len) {
    const struct hs_entry *e = hs_find(m, key, len, hs_fnv1a(key, len));
    return e ? e->val : 0;
}

/* Keys live in one pool: key i is pool[off[i] .. off[i] + len[i]). Keys
 * HS_N .. 2*HS_N-1 are built like the others but start with an uppercase
 * letter, so they are never in the map. */
struct hs_state {
    [[cx::owned]] uint8_t *pool;
    [[cx::owned]] uint32_t *off;
    [[cx::owned]] uint8_t *len;
    [[cx::owned]] uint32_t *probe;      /* lookup stream: key indices */
};

static void *hs_setup(void) {
    static const uint8_t alpha[38] = {
        'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's',
        't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '_', '.',
    };
    struct hs_state *s = (struct hs_state *)bench_alloc(sizeof *s);
    s->pool = (uint8_t *)bench_alloc(2 * HS_N * HS_KEY_MAX);
    s->off = (uint32_t *)bench_alloc(2 * HS_N * sizeof(uint32_t));
    s->len = (uint8_t *)bench_alloc(2 * HS_N);
    s->probe = (uint32_t *)bench_alloc(HS_LOOKUPS * sizeof(uint32_t));
    struct rng r = { 0xbe5466cf34e90c6cu };
    uint32_t at = 0;
    for (size_t i = 0; i < 2 * HS_N; i++) {
        uint32_t n = 4 + rng_below(&r, HS_KEY_MAX - 3);
        s->off[i] = at;
        s->len[i] = (uint8_t)n;
        for (uint32_t j = 0; j < n; j++) s->pool[at + j] = alpha[rng_below(&r, j == 0 ? 26u : 38u)];
        if (i >= HS_N) s->pool[at] = (uint8_t)(s->pool[at] - 'a' + 'A');
        at += n;
    }
    /* 3/4 of the lookups hit (some keys repeat), 1/4 miss */
    for (size_t i = 0; i < HS_LOOKUPS; i++) {
        uint32_t k = rng_below(&r, HS_N);
        s->probe[i] = rng_below(&r, 4) == 0 ? k + HS_N : k;
    }
    return s;
}

static uint64_t hs_run(void *state) {
    const struct hs_state *s = (const struct hs_state *)state;
    struct hs_map m = { NULL, 15, 0 };
    m.buckets = (struct hs_entry **)bench_zalloc(16 * sizeof(struct hs_entry *));
    for (size_t i = 0; i < HS_N; i++) hs_put(&m, &s->pool[s->off[i]], s->len[i], (uint32_t)i + 1);
    uint64_t h = mix(0, m.count), sum = 0, found = 0;
    for (size_t i = 0; i < HS_LOOKUPS; i++) {
        uint32_t k = s->probe[i];
        uint32_t v = hs_get(&m, &s->pool[s->off[k]], s->len[k]);
        found += v != 0 ? 1u : 0u;
        sum += v;
        if ((i & 255u) == 0) h = mix(h, sum);
    }
    h = mix(mix(h, found), sum);
    for (size_t i = 0; i <= m.mask; i++) {
        struct hs_entry *e = m.buckets[i];
        m.buckets[i] = NULL;
        while (e) {
            struct hs_entry *next = e->next;
            bench_free(e);
            e = next;
        }
    }
    bench_free(m.buckets);
    return h;
}

static void hs_teardown([[cx::escapes]] void *state) {
    struct hs_state *s = (struct hs_state *)state;
    bench_free(s->pool);
    bench_free(s->off);
    bench_free(s->len);
    bench_free(s->probe);
    bench_free(s);
}

extern const struct bench bench_hash_str = {
    "hash_str", "ds", "separate-chaining hash map with string keys (FNV-1a): insert/grow and lookups",
    hs_setup, hs_run, hs_teardown,
};
