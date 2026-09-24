/* Aliasing and ownership: buffers owned by a struct and reached through it
 * (spec §5.3, §18.3), and in-place operations whose pointer parameters are
 * declared to overlap ([[cx::alias]], spec §5.1). */
#include "bench.cxh"

/* ---- owned_fields: two owned float buffers reached through a struct -------
 * The spec §18.3 pattern; every access goes through `f->`. In C, a float
 * store to f->out[i] may modify f->in[], f->gain, f->bias, f->limit or
 * f->peak. So filter_apply vectorizes only behind runtime alias checks, and
 * in filter_clip the running f->peak must be stored and reloaded around
 * every f->out[i] store, which puts a store-to-load forward on the loop's
 * critical path. In Cx, f, f->in and f->out are distinct owned regions: the
 * fields are hoisted or promoted to registers and no alias checks are
 * needed. Float results are computed in source order (no reassociation);
 * max and min are exact. */

enum { OF_N = 1 << 13, OF_PASSES = 2600 };

struct filter {
    [[cx::owned]] float *in;
    [[cx::owned]] float *out;
    size_t n;
    float gain;
    float bias;
    float limit;
    float peak;       /* running maximum of out[] before clipping (filter_clip) */
    uint64_t clipped; /* number of out[] values clipped to limit (filter_clip) */
};

/* out = in * gain + bias */
static void filter_apply(struct filter *f) {
    for (size_t i = 0; i < f->n; i++) f->out[i] = f->in[i] * f->gain + f->bias;
}

/* clip out[] to at most limit in place, accumulating the peak and the
 * number of clipped values into the struct fields */
static void filter_clip(struct filter *f) {
    for (size_t i = 0; i < f->n; i++) {
        f->peak = f->out[i] > f->peak ? f->out[i] : f->peak;
        f->clipped += f->out[i] > f->limit ? 1u : 0u;
        f->out[i] = f->out[i] > f->limit ? f->limit : f->out[i];
    }
}

static void *of_setup(void) {
    struct filter *f = (struct filter *)bench_alloc(sizeof *f);
    f->in = (float *)bench_alloc(OF_N * sizeof(float));
    f->out = (float *)bench_alloc(OF_N * sizeof(float));
    f->n = OF_N;
    f->gain = 1.0f;
    f->bias = 0.0f;
    f->limit = 0.0f;
    f->peak = 0.0f;
    f->clipped = 0;
    struct rng r = { 0x0f1e1d5u };
    for (size_t i = 0; i < OF_N; i++) f->in[i] = (float)(rng_unit(&r) * 2.0 - 1.0);
    return f;
}

static uint64_t of_run(void *state) {
    struct filter *f = (struct filter *)state;
    uint64_t h = 0;
    for (uint32_t pass = 0; pass < OF_PASSES; pass++) {
        /* pass-dependent parameters, so no pass repeats the previous one */
        f->gain = 0.5f + (float)(pass & 15) * 0.0625f;
        f->bias = (float)(pass & 7) * 0.125f - 0.5f;
        f->limit = 0.25f + (float)(pass & 3) * 0.125f;
        f->peak = -1.0e30f;
        f->clipped = 0;
        filter_apply(f);
        filter_clip(f);
        h = mix(mix_double(h, (double)f->peak), f->clipped);
    }
    for (size_t i = 0; i < f->n; i += 64) h = mix_double(h, (double)f->out[i]);
    return h;
}

static void of_teardown([[cx::escapes]] void *state) {
    struct filter *f = (struct filter *)state;
    bench_free(f->in);
    bench_free(f->out);
    bench_free(f);
}

extern const struct bench bench_owned_fields = {
    "owned_fields", "ops", "out = in*gain+bias, then clip with peak/count in fields, all via a struct with owned buffers",
    of_setup, of_run, of_teardown,
};

/* ---- inplace_alias: overlapping source and destination --------------------
 * dst[i] = (src[i] + src[i+1]) / 2, over int32 and double arrays, called in
 * place (dst == src: a smoothing pass) and shifted (dst == src + 1: each
 * result feeds the next step, a first-order recursive filter). Both pointer
 * parameters are [[cx::alias]], so Cx must keep C's semantics: no noalias,
 * and the overlap is handled exactly as in C. This checks that Cx does not
 * regress (or miscompile) where aliasing is declared. */

enum { IA_N = 1 << 14, IA_PASSES = 900 };   /* arrays have IA_N + 1 elements */

struct inplace {
    [[cx::owned]] int32_t *x; [[cx::owned]] int32_t *x0;   /* x0: initial x */
    [[cx::owned]] double *d; [[cx::owned]] double *d0;     /* d0: initial d */
};

/* src has n + 1 elements; |src[i]| <= 2^20, so the sum cannot overflow */
static void ia_smooth_i32([[cx::alias]] int32_t *dst, [[cx::alias]] const int32_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = (src[i] + src[i + 1]) / 2;
}

static void ia_smooth_f64([[cx::alias]] double *dst, [[cx::alias]] const double *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = (src[i] + src[i + 1]) * 0.5;
}

static void *ia_setup(void) {
    struct inplace *s = (struct inplace *)bench_alloc(sizeof *s);
    s->x = (int32_t *)bench_alloc((IA_N + 1) * sizeof(int32_t));
    s->x0 = (int32_t *)bench_alloc((IA_N + 1) * sizeof(int32_t));
    s->d = (double *)bench_alloc((IA_N + 1) * sizeof(double));
    s->d0 = (double *)bench_alloc((IA_N + 1) * sizeof(double));
    struct rng r = { 0x1a5a11a5u };
    for (size_t i = 0; i <= IA_N; i++) {
        s->x0[i] = (int32_t)rng_below(&r, (2u << 20) + 1) - (1 << 20);
        s->d0[i] = rng_unit(&r) * 2.0 - 1.0;
    }
    return s;
}

static uint64_t ia_run(void *state) {
    const struct inplace *s = (const struct inplace *)state;
    for (size_t i = 0; i <= IA_N; i++) {
        s->x[i] = s->x0[i];
        s->d[i] = s->d0[i];
    }
    uint64_t h = 0;
    for (size_t pass = 0; pass < IA_PASSES; pass++) {
        ia_smooth_i32(s->x, s->x, IA_N);        /* in place */
        ia_smooth_i32(s->x + 1, s->x, IA_N);    /* shifted by one: recursive */
        ia_smooth_f64(s->d, s->d, IA_N);
        ia_smooth_f64(s->d + 1, s->d, IA_N);
        size_t k = (pass * 131) & (IA_N - 1);
        h = mix_double(mix(h, (uint64_t)(int64_t)s->x[k]), s->d[k]);
    }
    for (size_t i = 0; i <= IA_N; i += 64) h = mix_double(mix(h, (uint64_t)(int64_t)s->x[i]), s->d[i]);
    return h;
}

static void ia_teardown([[cx::escapes]] void *state) {
    struct inplace *s = (struct inplace *)state;
    bench_free(s->x);
    bench_free(s->x0);
    bench_free(s->d);
    bench_free(s->d0);
    bench_free(s);
}

extern const struct bench bench_inplace_alias = {
    "inplace_alias", "ops", "(src[i]+src[i+1])/2 with [[cx::alias]] params, dst == src and dst == src+1",
    ia_setup, ia_run, ia_teardown,
};
