/* Integer operations: arithmetic chains, division/modulo, bit operations. */
#include "bench.cxh"

#include <stdbit.h>

/* ---- int_arith: add/sub/mul/shift chains over an array ------------------- */

enum { INT_ARITH_N = 1 << 16, INT_ARITH_PASSES = 640 };

struct int_arith { int32_t *a; };

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

struct divmod { uint32_t *num; uint32_t *den; };

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

static uint64_t divmod_run(void *state) {
    const struct divmod *s = (const struct divmod *)state;
    uint64_t q = 0, m = 0;
    for (int pass = 0; pass < DIVMOD_PASSES; pass++)
        for (size_t i = 0; i < DIVMOD_N; i++) {
            uint32_t n = s->num[i] >> (pass & 7);
            q += n / s->den[i];
            m ^= n % s->den[i];
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

struct bitops { uint64_t *w; };

static void *bitops_setup(void) {
    struct bitops *s = (struct bitops *)bench_alloc(sizeof *s);
    s->w = (uint64_t *)bench_alloc(BITOPS_N * sizeof(uint64_t));
    struct rng r = { 0xabcdefu };
    for (size_t i = 0; i < BITOPS_N; i++) s->w[i] = rng_next(&r) >> rng_below(&r, 64);
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
