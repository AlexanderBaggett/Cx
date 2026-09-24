/* Array-backed structures: binary heap, ring buffer, bitset, union-find. */
#include "bench.h"

#include <stdbit.h>
#include <stdlib.h>

/* ---- heap_pq: binary min-heap driving a discrete-event simulation --------- */

enum { HEAP_INIT = 1 << 16, HEAP_EVENTS = 3 << 17, HEAP_STREAM = 1 << 16 };

struct hp_event { uint64_t time; uint32_t id; uint32_t kind; };
struct hp_heap { [[cx::owned]] struct hp_event *a; size_t len; size_t cap; };

/* Order by time, ties broken by id (ids are unique), so the order is total. */
static bool hp_before(struct hp_event x, struct hp_event y) {
    return x.time < y.time || (x.time == y.time && x.id < y.id);
}

static void hp_push(struct hp_heap *q, struct hp_event e) {
    if (q->len == q->cap) {
        size_t cap = q->cap ? q->cap * 2 : 64;
        struct hp_event *p = (struct hp_event *)realloc(q->a, cap * sizeof(struct hp_event));
        if (!p) abort();
        q->a = p;
        q->cap = cap;
    }
    size_t i = q->len++;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!hp_before(e, q->a[parent])) break;
        q->a[i] = q->a[parent];
        i = parent;
    }
    q->a[i] = e;
}

/* Precondition: q->len > 0. */
static struct hp_event hp_pop(struct hp_heap *q) {
    struct hp_event top = q->a[0];
    q->len--;
    size_t n = q->len;
    if (n == 0) return top;
    struct hp_event last = q->a[n];
    size_t i = 0;
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && hp_before(q->a[c + 1], q->a[c])) c++;
        if (!hp_before(q->a[c], last)) break;
        q->a[i] = q->a[c];
        i = c;
    }
    q->a[i] = last;
    return top;
}

struct hp_state { [[cx::owned]] uint64_t *stream; };

static void *hp_setup(void) {
    struct hp_state *s = (struct hp_state *)bench_alloc(sizeof *s);
    s->stream = (uint64_t *)bench_alloc(HEAP_STREAM * sizeof(uint64_t));
    struct rng r = { 0x9216d5d98979fb1bu };
    for (size_t i = 0; i < HEAP_STREAM; i++) s->stream[i] = rng_next(&r);
    return s;
}

static uint64_t hp_run(void *state) {
    const struct hp_state *s = (const struct hp_state *)state;
    struct hp_heap q = { NULL, 0, 0 };
    uint32_t next_id = 0;
    for (size_t i = 0; i < HEAP_INIT; i++) {
        uint64_t x = s->stream[i & (HEAP_STREAM - 1)];
        struct hp_event e = { x & 0xfffffu, next_id++, (uint32_t)(x >> 60) };
        hp_push(&q, e);
    }
    uint64_t h = 0, work = 0;
    for (size_t step = 0; step < HEAP_EVENTS; step++) {
        struct hp_event e = hp_pop(&q);
        work += e.kind;
        if ((step & 31u) == 0) h = mix(h, e.time ^ ((uint64_t)e.id << 40));
        /* each event schedules 0, 1 or 2 follow-ups; the population stays
         * between HEAP_INIT/2 and 2*HEAP_INIT */
        uint64_t x = s->stream[(step * 7) & (HEAP_STREAM - 1)] ^ e.time;
        uint64_t spawn = q.len < HEAP_INIT / 2 ? 2u : q.len > 2 * HEAP_INIT ? 0u : x % 3u;
        for (uint64_t k = 0; k < spawn; k++) {
            /* delays up to 2^16, heavier towards short ones */
            uint64_t d = 1 + ((x >> (8 + 20 * k)) & 0xffffu) / (1 + ((x >> (24 + 20 * k)) & 15u));
            struct hp_event f = { e.time + d, next_id++, (e.kind + (uint32_t)k + 1) & 15u };
            hp_push(&q, f);
        }
    }
    h = mix(mix(mix(h, work), q.len), next_id);
    bench_free(q.a);
    return h;
}

static void hp_teardown([[cx::escapes]] void *state) {
    struct hp_state *s = (struct hp_state *)state;
    bench_free(s->stream);
    bench_free(s);
}

extern const struct bench bench_heap_pq = {
    "heap_pq", "ds", "binary min-heap priority queue driving a discrete-event simulation",
    hp_setup, hp_run, hp_teardown,
};

/* ---- ring_queue: bounded ring-buffer FIFO under bursty traffic ------------ */

enum { RING_CAP = 1 << 12, RING_BURSTS = 1 << 15, RING_BURST_MAX = 1500 };

struct ring { [[cx::owned]] uint64_t *buf; size_t head; size_t count; size_t mask; };

static bool ring_push(struct ring *q, uint64_t v) {
    if (q->count > q->mask) return false;          /* full */
    q->buf[(q->head + q->count) & q->mask] = v;
    q->count++;
    return true;
}

static bool ring_pop(struct ring *q, uint64_t *out) {
    if (q->count == 0) return false;
    *out = q->buf[q->head];
    q->head = (q->head + 1) & q->mask;
    q->count--;
    return true;
}

/* Burst i: enq[i] enqueue attempts, then deq[i] dequeue attempts. */
struct ring_state { [[cx::owned]] uint16_t *enq; [[cx::owned]] uint16_t *deq; };

static void *ring_setup(void) {
    struct ring_state *s = (struct ring_state *)bench_alloc(sizeof *s);
    s->enq = (uint16_t *)bench_alloc(RING_BURSTS * sizeof(uint16_t));
    s->deq = (uint16_t *)bench_alloc(RING_BURSTS * sizeof(uint16_t));
    struct rng r = { 0xd1310ba698dfb5acu };
    for (size_t i = 0; i < RING_BURSTS; i++) {
        s->enq[i] = (uint16_t)rng_below(&r, RING_BURST_MAX);
        s->deq[i] = (uint16_t)rng_below(&r, RING_BURST_MAX);
    }
    return s;
}

static uint64_t ring_run(void *state) {
    const struct ring_state *s = (const struct ring_state *)state;
    struct ring q = { NULL, 0, 0, RING_CAP - 1 };
    q.buf = (uint64_t *)bench_alloc(RING_CAP * sizeof(uint64_t));
    uint64_t seq = 0, dropped = 0, empty = 0, acc = 0, popped = 0;
    for (size_t b = 0; b < RING_BURSTS; b++) {
        uint32_t ne = s->enq[b], nd = s->deq[b];
        for (uint32_t i = 0; i < ne; i++) {
            if (ring_push(&q, seq)) seq++;
            else dropped++;
        }
        for (uint32_t i = 0; i < nd; i++) {
            uint64_t v;
            if (ring_pop(&q, &v)) {
                acc ^= v + popped;
                popped++;
            } else {
                empty++;
            }
        }
        if ((b & 63u) == 0) acc = mix(acc, q.count);
    }
    uint64_t h = mix(mix(mix(mix(0, acc), seq), dropped), empty);
    bench_free(q.buf);
    return h;
}

static void ring_teardown([[cx::escapes]] void *state) {
    struct ring_state *s = (struct ring_state *)state;
    bench_free(s->enq);
    bench_free(s->deq);
    bench_free(s);
}

extern const struct bench bench_ring_queue = {
    "ring_queue", "ds", "bounded ring-buffer FIFO of uint64: bursts of enqueue and dequeue",
    ring_setup, ring_run, ring_teardown,
};

/* ---- bitset_ops: set/clear/test, word-wise and/or/xor, popcount ----------- */

enum { BS_BITS = 1 << 20, BS_WORDS = BS_BITS / 64, BS_OPS = 1 << 18, BS_PASSES = 20 };

struct bitset { [[cx::owned]] uint64_t *w; size_t nwords; };

static void bs_set(struct bitset *b, uint32_t i) { b->w[i >> 6] |= (uint64_t)1 << (i & 63u); }
static void bs_clear(struct bitset *b, uint32_t i) { b->w[i >> 6] &= ~((uint64_t)1 << (i & 63u)); }
static bool bs_test(const struct bitset *b, uint32_t i) { return ((b->w[i >> 6] >> (i & 63u)) & 1u) != 0; }

static void bs_or(struct bitset *d, const struct bitset *s) {
    for (size_t i = 0; i < d->nwords; i++) d->w[i] |= s->w[i];
}
static void bs_and(struct bitset *d, const struct bitset *s) {
    for (size_t i = 0; i < d->nwords; i++) d->w[i] &= s->w[i];
}
static void bs_xor(struct bitset *d, const struct bitset *s) {
    for (size_t i = 0; i < d->nwords; i++) d->w[i] ^= s->w[i];
}
static void bs_andnot(struct bitset *d, const struct bitset *s) {
    for (size_t i = 0; i < d->nwords; i++) d->w[i] &= ~s->w[i];
}

static uint64_t bs_count(const struct bitset *b) {
    uint64_t c = 0;
    for (size_t i = 0; i < b->nwords; i++) c += stdc_count_ones(b->w[i]);
    return c;
}

/* Sum of the indices of the set bits, visiting them in order. */
static uint64_t bs_index_sum(const struct bitset *b) {
    uint64_t sum = 0;
    for (size_t i = 0; i < b->nwords; i++) {
        uint64_t x = b->w[i];
        while (x != 0) {
            sum += i * 64 + stdc_trailing_zeros(x);
            x &= x - 1;
        }
    }
    return sum;
}

struct bs_state { [[cx::owned]] uint32_t *ia; [[cx::owned]] uint32_t *ib; };

static void *bs_setup(void) {
    struct bs_state *s = (struct bs_state *)bench_alloc(sizeof *s);
    s->ia = (uint32_t *)bench_alloc(BS_OPS * sizeof(uint32_t));
    s->ib = (uint32_t *)bench_alloc(BS_OPS * sizeof(uint32_t));
    struct rng r = { 0x2ffd72dbd01adfb7u };
    for (size_t i = 0; i < BS_OPS; i++) s->ia[i] = rng_below(&r, BS_BITS);
    for (size_t i = 0; i < BS_OPS; i++) s->ib[i] = rng_below(&r, BS_BITS);
    return s;
}

static uint64_t bs_run(void *state) {
    const struct bs_state *s = (const struct bs_state *)state;
    struct bitset a = { NULL, BS_WORDS }, b = { NULL, BS_WORDS }, t = { NULL, BS_WORDS };
    a.w = (uint64_t *)bench_zalloc(BS_WORDS * sizeof(uint64_t));
    b.w = (uint64_t *)bench_zalloc(BS_WORDS * sizeof(uint64_t));
    t.w = (uint64_t *)bench_zalloc(BS_WORDS * sizeof(uint64_t));
    uint64_t h = 0;
    for (uint32_t pass = 0; pass < BS_PASSES; pass++) {
        uint32_t shift = pass * 4099u;
        for (size_t i = 0; i < BS_OPS; i++) bs_set(&a, (s->ia[i] + shift) & (BS_BITS - 1));
        for (size_t i = 0; i < BS_OPS; i++) bs_set(&b, (s->ib[i] ^ shift) & (BS_BITS - 1));
        for (size_t i = 0; i < BS_OPS; i += 3) bs_clear(&a, s->ib[i]);
        uint64_t hits = 0;
        for (size_t i = 0; i < BS_OPS; i++) hits += bs_test(&a, s->ib[i] ^ shift) ? 1u : 0u;
        h = mix(mix(h, hits), bs_count(&a));
        /* word-wise algebra: t = a ^ b, t |= b, t &= a, t &= ~b */
        for (uint32_t rep = 0; rep < 8; rep++) {
            for (size_t i = 0; i < BS_WORDS; i++) t.w[i] = a.w[i];
            bs_xor(&t, &b);
            h = mix(h, bs_count(&t));
            bs_or(&t, &b);
            h = mix(h, bs_count(&t));
            bs_and(&t, &a);
            h = mix(h, bs_count(&t));
            bs_andnot(&t, &b);
            h = mix(h, bs_count(&t));
        }
        h = mix(h, bs_index_sum(&a));
        /* a becomes a xor b, b loses a's bits: new inputs for the next pass */
        bs_andnot(&b, &a);
        bs_and(&a, &t);
        bs_xor(&a, &b);
        h = mix(mix(h, bs_count(&a)), bs_count(&b));
    }
    bench_free(a.w);
    bench_free(b.w);
    bench_free(t.w);
    return h;
}

static void bs_teardown([[cx::escapes]] void *state) {
    struct bs_state *s = (struct bs_state *)state;
    bench_free(s->ia);
    bench_free(s->ib);
    bench_free(s);
}

extern const struct bench bench_bitset_ops = {
    "bitset_ops", "ds", "1M-bit bitsets: random set/clear/test, word-wise and/or/xor/andnot, popcount, bit iteration",
    bs_setup, bs_run, bs_teardown,
};

/* ---- union_find: disjoint sets, union by rank + path compression ---------- */

enum { UF_N = 1 << 20, UF_UNIONS = 1 << 20 };

struct uf { [[cx::owned]] uint32_t *parent; [[cx::owned]] uint8_t *rank; size_t n; };

static uint32_t uf_find(struct uf *u, uint32_t x) {
    uint32_t root = x;
    while (u->parent[root] != root) root = u->parent[root];
    while (u->parent[x] != root) {
        uint32_t next = u->parent[x];
        u->parent[x] = root;
        x = next;
    }
    return root;
}

static bool uf_union(struct uf *u, uint32_t a, uint32_t b) {
    uint32_t ra = uf_find(u, a), rb = uf_find(u, b);
    if (ra == rb) return false;
    if (u->rank[ra] < u->rank[rb]) {
        u->parent[ra] = rb;
    } else if (u->rank[ra] > u->rank[rb]) {
        u->parent[rb] = ra;
    } else {
        u->parent[rb] = ra;
        u->rank[ra] = (uint8_t)(u->rank[ra] + 1);
    }
    return true;
}

/* Operation i: union(ea[i], eb[i]); every fourth operation also asks whether
 * qa[i] and qb[i] are connected. */
struct uf_state { [[cx::owned]] uint32_t *ea; [[cx::owned]] uint32_t *eb; };

static void *uf_setup(void) {
    struct uf_state *s = (struct uf_state *)bench_alloc(sizeof *s);
    s->ea = (uint32_t *)bench_alloc(UF_UNIONS * sizeof(uint32_t));
    s->eb = (uint32_t *)bench_alloc(UF_UNIONS * sizeof(uint32_t));
    struct rng r = { 0xb8e1afed6a267e96u };
    for (size_t i = 0; i < UF_UNIONS; i++) {
        s->ea[i] = rng_below(&r, UF_N);
        s->eb[i] = rng_below(&r, UF_N);
    }
    return s;
}

static uint64_t uf_run(void *state) {
    const struct uf_state *s = (const struct uf_state *)state;
    struct uf u = { NULL, NULL, UF_N };
    u.parent = (uint32_t *)bench_alloc(UF_N * sizeof(uint32_t));
    u.rank = (uint8_t *)bench_zalloc(UF_N);
    for (uint32_t i = 0; i < UF_N; i++) u.parent[i] = i;
    uint64_t merged = 0, connected = 0, h = 0;
    for (size_t i = 0; i < UF_UNIONS; i++) {
        merged += uf_union(&u, s->ea[i], s->eb[i]) ? 1u : 0u;
        if ((i & 3u) == 3u) {
            /* query a pair from an earlier edge list position */
            uint32_t qa = s->ea[i >> 1], qb = s->eb[i >> 2];
            uint32_t ra = uf_find(&u, qa);
            uint32_t rb = uf_find(&u, qb);
            connected += ra == rb ? 1u : 0u;
        }
        if ((i & 0xffffu) == 0) h = mix(h, merged);
    }
    /* count the components and fold in each element's root */
    uint64_t roots = 0, rsum = 0;
    for (uint32_t i = 0; i < UF_N; i++) {
        uint32_t root = uf_find(&u, i);
        roots += root == i ? 1u : 0u;
        rsum += root & 0xffffu;
    }
    h = mix(mix(mix(mix(h, merged), connected), roots), rsum);
    bench_free(u.parent);
    bench_free(u.rank);
    return h;
}

static void uf_teardown([[cx::escapes]] void *state) {
    struct uf_state *s = (struct uf_state *)state;
    bench_free(s->ea);
    bench_free(s->eb);
    bench_free(s);
}

extern const struct bench bench_union_find = {
    "union_find", "ds", "disjoint-set forest: random unions and connectivity queries, path compression, union by rank",
    uf_setup, uf_run, uf_teardown,
};
