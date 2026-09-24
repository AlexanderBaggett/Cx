/* Integer operations: arithmetic chains, division/modulo, bit operations. */
#include "bench.cxh"

#include <stdbit.h>

/* ---- int_arith: add/sub/mul/shift chains over an array ------------------- */

enum { INT_ARITH_N = 1 << 16, INT_ARITH_PASSES = 640 };

struct int_arith { [[cx::owned]] int32_t *a; };

static void *int_arith_setup(void) {
    struct int_arith *s = (struct int_arith *)bench_alloc(sizeof *s);
    s->a = (int32_t *)bench_alloc(INT_ARITH_N * sizeof(int32_t));
    struct rng r = { 0x9e3779b97f4a7c15u };
    for (size_t i = 0; i < INT_ARITH_N; i++) s->a[i] = (int32_t)rng_below(&r, 2001) - 1000;
    return s;
}

static uint64_t int_arith_run(void *state) {
    const struct int_arith *s = (const struct int_arith *)state;
    int64_t acc = 0;
    for (int pass = 0; pass < INT_ARITH_PASSES; pass++) {
        int64_t k = pass - 80;
        for (size_t i = 0; i < INT_ARITH_N; i++) {
            int64_t x = s->a[i];
            /* bounded values: |x| <= 1000, so nothing below can overflow */
            acc += (x * 7 - k) * 3 + x * 4 - x / 2;
            acc ^= x * x;
        }
    }
    return mix(0, (uint64_t)acc);
}

static void int_arith_teardown([[cx::escapes]] void *state) {
    struct int_arith *s = (struct int_arith *)state;
    bench_free(s->a);
    bench_free(s);
}

extern const struct bench bench_int_arith = {
    "int_arith", "ops", "int64 add/sub/mul/shift/xor chains over an int32 array",
    int_arith_setup, int_arith_run, int_arith_teardown,
};

/* ---- int_divmod: division and modulo by run-time divisors ----------------- */

enum { DIVMOD_N = 1 << 16, DIVMOD_PASSES = 240 };

struct divmod { [[cx::owned]] uint32_t *num; [[cx::owned]] uint32_t *den; };

static void *divmod_setup(void) {
    struct divmod *s = (struct divmod *)bench_alloc(sizeof *s);
    s->num = (uint32_t *)bench_alloc(DIVMOD_N * sizeof(uint32_t));
    s->den = (uint32_t *)bench_alloc(DIVMOD_N * sizeof(uint32_t));
    struct rng r = { 0x1234567u };
    for (size_t i = 0; i < DIVMOD_N; i++) {
        s->num[i] = (uint32_t)rng_next(&r);
        s->den[i] = rng_below(&r, 1000) + 1;
    }
    return s;
}

/* Every quotient and remainder is summed (not xored: the shifts repeat every 8
 * passes, so xored remainders would cancel). Bounds: q <= 240 * 2^16 * (2^32 + 1),
 * m < 240 * 2^16 * 1000, both far below 2^64. */
static uint64_t divmod_run(void *state) {
    const struct divmod *s = (const struct divmod *)state;
    uint64_t q = 0, m = 0;
    for (int pass = 0; pass < DIVMOD_PASSES; pass++)
        for (size_t i = 0; i < DIVMOD_N; i++) {
            uint32_t n = s->num[i] >> (pass & 7);
            q += n / s->den[i];
            m += n % s->den[i];
            q += (int64_t)(int32_t)(n >> 1) / -(int64_t)s->den[i] < 0 ? 1u : 0u;
        }
    return mix(mix(0, q), m);
}

static void divmod_teardown([[cx::escapes]] void *state) {
    struct divmod *s = (struct divmod *)state;
    bench_free(s->num);
    bench_free(s->den);
    bench_free(s);
}

extern const struct bench bench_int_divmod = {
    "int_divmod", "ops", "unsigned and signed division/modulo by run-time divisors",
    divmod_setup, divmod_run, divmod_teardown,
};

/* ---- bit_ops: popcount, leading/trailing zeros, rotate, byte swap -------- */

enum { BITOPS_N = 1 << 16, BITOPS_PASSES = 240 };

struct bitops { [[cx::owned]] uint64_t *w; };

static void *bitops_setup(void) {
    struct bitops *s = (struct bitops *)bench_alloc(sizeof *s);
    s->w = (uint64_t *)bench_alloc(BITOPS_N * sizeof(uint64_t));
    struct rng r = { 0xabcdefu };
    for (size_t i = 0; i < BITOPS_N; i++) {
        /* two statements: the two rng calls must happen in a fixed order */
        uint64_t v = rng_next(&r);
        s->w[i] = v >> rng_below(&r, 64);
    }
    return s;
}

static uint64_t bitops_run(void *state) {
    const struct bitops *s = (const struct bitops *)state;
    uint64_t h = 0;
    for (int pass = 0; pass < BITOPS_PASSES; pass++) {
        uint64_t cnt = 0;
        for (size_t i = 0; i < BITOPS_N; i++) {
            uint64_t x = s->w[i];
            cnt += stdc_count_ones(x);
            cnt += x ? stdc_leading_zeros(x) : 64u;
            cnt += x ? stdc_trailing_zeros(x) : 64u;
            unsigned rot = (unsigned)pass & 63u;
            uint64_t y = rot ? (x << rot) | (x >> (64u - rot)) : x;
            cnt += y & 0xffu;
        }
        h = mix(h, cnt);
    }
    return h;
}

static void bitops_teardown([[cx::escapes]] void *state) {
    struct bitops *s = (struct bitops *)state;
    bench_free(s->w);
    bench_free(s);
}

extern const struct bench bench_bit_ops = {
    "bit_ops", "ops", "popcount, clz, ctz and rotate over 64-bit words",
    bitops_setup, bitops_run, bitops_teardown,
};

/* ---- index_u32: loops indexed by uint32_t with offsets and strides -------
 * In C, uint32_t index arithmetic wraps modulo 2^32, so `i + off` and
 * `i * stride` are not affine 64-bit addresses: the compiler keeps 32-bit
 * arithmetic with a zero extension per access, and the vectorizer needs
 * wraparound checks. In Cx, unsigned wraparound is a violation (spec §7.1,
 * [E05]), so the index arithmetic is `nuw`. All bounds below keep every index
 * far below 2^32, so nothing ever wraps. */

enum {
    IDX_N = 1 << 14, IDX_OFFS = 1 << 12,     /* axpy: dst has IDX_N + IDX_OFFS elements */
    IDX_GN = 1 << 13, IDX_GSTRIDE = 8,       /* gather: g has IDX_GN * IDX_GSTRIDE + 64 elements */
    IDX_PASSES = 8192,
};

struct idx {
    [[cx::owned]] int32_t *src; [[cx::owned]] int32_t *dst;
    [[cx::owned]] int32_t *dst0;   /* initial dst, restored at the start of each run */
    [[cx::owned]] int32_t *g;
};

/* dst[off .. off+n) += src[0 .. n) * k */
static void idx_axpy(int32_t *dst, const int32_t *src, uint32_t n, uint32_t off, int32_t k) {
    for (uint32_t i = 0; i < n; i++) dst[i + off] = src[i] * k + dst[i + off];
}

/* sum of a[base + i * stride] for i < n */
static int64_t idx_gather(const int32_t *a, uint32_t n, uint32_t base, uint32_t stride) {
    int64_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum += a[base + i * stride];
    return sum;
}

static void *idx_setup(void) {
    struct idx *s = (struct idx *)bench_alloc(sizeof *s);
    s->src = (int32_t *)bench_alloc(IDX_N * sizeof(int32_t));
    s->dst = (int32_t *)bench_alloc((IDX_N + IDX_OFFS) * sizeof(int32_t));
    s->dst0 = (int32_t *)bench_alloc((IDX_N + IDX_OFFS) * sizeof(int32_t));
    s->g = (int32_t *)bench_alloc((IDX_GN * IDX_GSTRIDE + 64) * sizeof(int32_t));
    struct rng r = { 0x1d3c32u };
    for (size_t i = 0; i < IDX_N; i++) s->src[i] = (int32_t)rng_below(&r, 2001) - 1000;
    for (size_t i = 0; i < IDX_N + IDX_OFFS; i++) s->dst0[i] = (int32_t)rng_below(&r, 2001) - 1000;
    for (size_t i = 0; i < IDX_GN * IDX_GSTRIDE + 64; i++) s->g[i] = (int32_t)rng_below(&r, 1u << 20);
    return s;
}

static uint64_t idx_run(void *state) {
    const struct idx *s = (const struct idx *)state;
    for (size_t i = 0; i < IDX_N + IDX_OFFS; i++) s->dst[i] = s->dst0[i];
    uint64_t h = 0;
    for (uint32_t pass = 0; pass < IDX_PASSES; pass++) {
        /* |src * k| <= 3000 per pass: |dst| <= 1000 + 3000 * IDX_PASSES < 2^25 */
        uint32_t off = (pass * 97u) & (IDX_OFFS - 1);
        int32_t k = (int32_t)(pass % 7u) - 3;
        idx_axpy(s->dst, s->src, IDX_N, off, k);
        /* base < 64, stride <= 8: every index < IDX_GN * IDX_GSTRIDE + 64 */
        int64_t g = idx_gather(s->g, IDX_GN, pass & 63u, 1 + (pass & (IDX_GSTRIDE - 1)));
        h = mix(h, (uint64_t)g);
    }
    for (size_t i = 0; i < IDX_N + IDX_OFFS; i += 64) h = mix(h, (uint64_t)(int64_t)s->dst[i]);
    return h;
}

static void idx_teardown([[cx::escapes]] void *state) {
    struct idx *s = (struct idx *)state;
    bench_free(s->src);
    bench_free(s->dst);
    bench_free(s->dst0);
    bench_free(s->g);
    bench_free(s);
}

extern const struct bench bench_index_u32 = {
    "index_u32", "ops", "uint32_t-indexed loops: dst[i + off] += src[i] * k and sum of a[base + i * stride]",
    idx_setup, idx_run, idx_teardown,
};
