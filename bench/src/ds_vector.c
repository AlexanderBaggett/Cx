/* Growable arrays: a dynamic vector and an append-heavy byte string builder. */
#include "bench.cxh"

#include <stdlib.h>
#include <string.h>

/* realloc that aborts on failure. */
static void *xrealloc([[cx::escapes, cx::nullable]] void *p, size_t bytes) {
    void *q = realloc(p, bytes);
    if (!q) abort();
    return q;
}

/* ---- vector_push: push with geometric growth, iterate/sum, pop all -------- */

enum { VEC_N = 1 << 20, VEC_ROUNDS = 24, VEC_STEP = 40961 };

struct vec { [[cx::owned]] uint32_t *data; size_t len; size_t cap; };

static void vec_push(struct vec *v, uint32_t x) {
    if (v->len == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 8;
        v->data = (uint32_t *)xrealloc(v->data, cap * sizeof *v->data);
        v->cap = cap;
    }
    v->data[v->len++] = x;
}

/* Precondition: v->len > 0. */
static uint32_t vec_pop(struct vec *v) {
    v->len--;
    return v->data[v->len];
}

struct vec_state { [[cx::owned]] uint32_t *in; };

static void *vec_setup(void) {
    struct vec_state *s = (struct vec_state *)bench_alloc(sizeof *s);
    s->in = (uint32_t *)bench_alloc(VEC_N * sizeof(uint32_t));
    struct rng r = { 0x243f6a8885a308d3u };
    for (size_t i = 0; i < VEC_N; i++) s->in[i] = rng_below(&r, 1u << 24);
    return s;
}

static uint64_t vec_run(void *state) {
    const struct vec_state *s = (const struct vec_state *)state;
    uint64_t h = 0;
    for (size_t round = 0; round < VEC_ROUNDS; round++) {
        struct vec v = { NULL, 0, 0 };
        size_t n = VEC_N - (round & 7u) * VEC_STEP;
        uint32_t salt = (uint32_t)round;
        for (size_t i = 0; i < n; i++) vec_push(&v, s->in[i] ^ salt);
        /* iterate: sum and count odd values (values < 2^24: no overflow) */
        uint64_t sum = 0, odd = 0;
        for (size_t i = 0; i < v.len; i++) {
            sum += v.data[i];
            odd += v.data[i] & 1u;
        }
        h = mix(mix(h, sum), odd);
        /* pop everything, folding the popped sequence in order */
        uint64_t acc = 0;
        while (v.len > 0) acc = mix(acc, vec_pop(&v));
        h = mix(h, acc);
        bench_free(v.data);
    }
    return h;
}

static void vec_teardown([[cx::escapes]] void *state) {
    struct vec_state *s = (struct vec_state *)state;
    bench_free(s->in);
    bench_free(s);
}

extern const struct bench bench_vector_push = {
    "vector_push", "ds", "dynamic uint32 array: geometric-growth push, iterate/sum, pop all",
    vec_setup, vec_run, vec_teardown,
};

/* ---- string_builder: appends of short strings and formatted numbers ------- */

enum { SB_RECORDS = 1 << 17, SB_ROUNDS = 6, SB_WORDS = 64, SB_WORD_MAX = 12 };

struct sb { [[cx::owned]] uint8_t *data; size_t len; size_t cap; };

static void sb_reserve(struct sb *b, size_t extra) {
    size_t need = b->len + extra;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) cap *= 2;
    b->data = (uint8_t *)xrealloc(b->data, cap);
    b->cap = cap;
}

static void sb_putc(struct sb *b, uint8_t c) {
    sb_reserve(b, 1);
    b->data[b->len++] = c;
}

static void sb_append(struct sb *b, const uint8_t *p, size_t n) {
    sb_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void sb_append_u32(struct sb *b, uint32_t v) {
    uint8_t tmp[10];
    size_t k = 0;
    do {
        tmp[k++] = (uint8_t)('0' + v % 10u);
        v /= 10u;
    } while (v != 0);
    sb_reserve(b, k);
    while (k > 0) {
        k--;
        b->data[b->len++] = tmp[k];
    }
}

static void sb_append_i32(struct sb *b, int32_t v) {
    if (v < 0) {
        sb_putc(b, '-');
        sb_append_u32(b, (uint32_t)(-(int64_t)v));
    } else {
        sb_append_u32(b, (uint32_t)v);
    }
}

static void sb_append_hex(struct sb *b, uint32_t v) {
    static const uint8_t digits[16] = {
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    sb_reserve(b, 8);
    for (unsigned sh = 32; sh > 0; sh -= 4) b->data[b->len++] = digits[(v >> (sh - 4)) & 15u];
}

struct sb_state {
    [[cx::owned]] uint8_t *words;     /* SB_WORDS words of SB_WORD_MAX bytes */
    [[cx::owned]] uint8_t *wlen;
    [[cx::owned]] uint8_t *wid;       /* per record: word index */
    [[cx::owned]] uint32_t *u;        /* per record: unsigned field */
    [[cx::owned]] int32_t *sv;        /* per record: signed field */
};

static void *sb_setup(void) {
    struct sb_state *s = (struct sb_state *)bench_alloc(sizeof *s);
    s->words = (uint8_t *)bench_alloc(SB_WORDS * SB_WORD_MAX);
    s->wlen = (uint8_t *)bench_alloc(SB_WORDS);
    s->wid = (uint8_t *)bench_alloc(SB_RECORDS);
    s->u = (uint32_t *)bench_alloc(SB_RECORDS * sizeof(uint32_t));
    s->sv = (int32_t *)bench_alloc(SB_RECORDS * sizeof(int32_t));
    struct rng r = { 0x13198a2e03707344u };
    for (size_t i = 0; i < SB_WORDS; i++) {
        uint32_t len = 2 + rng_below(&r, SB_WORD_MAX - 1);
        s->wlen[i] = (uint8_t)len;
        for (uint32_t j = 0; j < len; j++) s->words[i * SB_WORD_MAX + j] = (uint8_t)('a' + rng_below(&r, 26));
    }
    for (size_t i = 0; i < SB_RECORDS; i++) {
        s->wid[i] = (uint8_t)rng_below(&r, SB_WORDS);
        uint64_t x = rng_next(&r);
        s->u[i] = (uint32_t)(x >> (32 + rng_below(&r, 32)));
        s->sv[i] = (int32_t)rng_below(&r, 2000001) - 1000000;
    }
    return s;
}

static uint64_t sb_run(void *state) {
    const struct sb_state *s = (const struct sb_state *)state;
    static const uint8_t sep[3] = { ',', ' ', '"' };
    uint64_t h = 0;
    for (size_t round = 0; round < SB_ROUNDS; round++) {
        struct sb b = { NULL, 0, 0 };
        size_t n = SB_RECORDS - round * 1021;
        for (size_t i = 0; i < n; i++) {
            size_t w = s->wid[i];
            sb_putc(&b, '"');
            sb_append(&b, &s->words[w * SB_WORD_MAX], s->wlen[w]);
            sb_putc(&b, '"');
            sb_putc(&b, ':');
            sb_append_u32(&b, s->u[i]);
            sb_append(&b, sep, 2);
            sb_append_i32(&b, s->sv[i]);
            if ((i & 7u) == 0) {
                sb_append(&b, sep, 3);
                sb_append_hex(&b, s->u[i]);
                sb_putc(&b, '"');
            }
            sb_putc(&b, (uint8_t)((i & 15u) == 15u ? '\n' : ';'));
        }
        /* cheap checksum, so the appends dominate: the length, every 64th
         * byte and the last byte */
        uint64_t f = 0;
        for (size_t i = 0; i < b.len; i += 64) f = mix(f, (uint64_t)b.data[i]);
        h = mix(mix(mix(h, b.len), f), (uint64_t)b.data[b.len - 1]);
        bench_free(b.data);
    }
    return h;
}

static void sb_teardown([[cx::escapes]] void *state) {
    struct sb_state *s = (struct sb_state *)state;
    bench_free(s->words);
    bench_free(s->wlen);
    bench_free(s->wid);
    bench_free(s->u);
    bench_free(s->sv);
    bench_free(s);
}

extern const struct bench bench_string_builder = {
    "string_builder", "ds", "byte string builder: appends of short strings and formatted integers",
    sb_setup, sb_run, sb_teardown,
};
