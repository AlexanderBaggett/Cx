/* Cx vs C head-to-head benchmark harness.
 *
 * Every benchmark is written once, in the subset of C23 that is also valid
 * Cx (see bench/README.md, "Source rules"): no removed keywords, no unsigned
 * wraparound, no mixed signedness, no implicit narrowing, no union punning,
 * no variadic definitions. Cx-specific intent is written with [[cx::...]]
 * attributes, which a plain C compiler ignores.
 */
#ifndef CX_BENCH_H
#define CX_BENCH_H

#include <stddef.h>
#include <stdint.h>

/* One benchmark.
 * setup():    builds inputs (not timed) and returns the state.
 * run():      the timed work. Must return the same checksum on every call
 *             and with every compiler.
 * teardown(): frees the state.
 */
struct bench {
    const char *name;
    const char *category;   /* "ops", "ds", "alg" or "kern" */
    const char *what;       /* one-line description */
    void *(*setup)(void);
    uint64_t (*run)(void *state);
    void (*teardown)([[cx::escapes]] void *state);
};

/* Deterministic PRNG: xorshift64. Shifts and xors only, so it never relies
 * on arithmetic wraparound (a violation in Cx). */
struct rng { uint64_t s; };

static inline uint64_t rng_next(struct rng *r) {
    uint64_t x = r->s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    r->s = x;
    return x;
}

/* Uniform-ish value in [0, n), n > 0. */
static inline uint32_t rng_below(struct rng *r, uint32_t n) {
    return (uint32_t)(rng_next(r) % n);
}

/* Double in [0, 1). */
static inline double rng_unit(struct rng *r) {
    return (double)(rng_next(r) >> 11) * 0x1.0p-53;
}

/* Checksum mixing: xor then rotate. No arithmetic, so no overflow. */
static inline uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v;
    return (h << 7) | (h >> 57);
}

/* Fold a double into a checksum. Values are rounded to a fixed number of
 * significant bits so the result does not depend on the last ulp. */
uint64_t mix_double(uint64_t h, double v);

/* malloc/calloc that abort on failure. */
void *bench_alloc(size_t bytes);
void *bench_zalloc(size_t bytes);
void bench_free([[cx::escapes, cx::nullable]] void *p);

#endif
