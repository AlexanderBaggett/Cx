/* Floating-point operations: mul/add chains, division and square root, libm
 * calls, and float reductions in source order. */
#include "bench.h"

#include <math.h>

/* ---- fp_poly: Horner evaluation of a degree-12 polynomial ---------------- */

enum { POLY_N = 1 << 14, POLY_PASSES = 400, POLY_DEG = 12 };

struct poly { double *x; double *acc; double *c; };

static void *poly_setup(void) {
    struct poly *s = (struct poly *)bench_alloc(sizeof *s);
    s->x = (double *)bench_alloc(POLY_N * sizeof(double));
    s->acc = (double *)bench_alloc(POLY_N * sizeof(double));
    s->c = (double *)bench_alloc((POLY_DEG + 1) * sizeof(double));
    struct rng r = { 0x9017a11ce5u };
    for (size_t i = 0; i < POLY_N; i++) s->x[i] = rng_unit(&r) * 2.0 - 1.0;
    for (size_t k = 0; k <= POLY_DEG; k++) s->c[k] = (rng_unit(&r) * 2.0 - 1.0) / (double)(k + 1);
    return s;
}

/* acc[i] += p(x[i] * scale); |p| <= sum 1/(k+1) < 3.2 on [-1, 1] */
static void poly_kernel(double *acc, const double *x, const double *c, size_t n, double scale) {
    for (size_t i = 0; i < n; i++) {
        double y = x[i] * scale;
        double p = c[POLY_DEG];
        for (int k = POLY_DEG - 1; k >= 0; k--) p = p * y + c[k];
        acc[i] += p;
    }
}

static uint64_t poly_run(void *state) {
    const struct poly *s = (const struct poly *)state;
    for (size_t i = 0; i < POLY_N; i++) s->acc[i] = 0.0;
    for (int pass = 0; pass < POLY_PASSES; pass++)
        poly_kernel(s->acc, s->x, s->c, POLY_N, 0.5 + 0.5 * (double)pass / POLY_PASSES);
    double sum = 0.0;
    uint64_t h = 0;
    for (size_t i = 0; i < POLY_N; i++) {
        sum += s->acc[i];
        if ((i & 1023) == 0) h = mix_double(h, s->acc[i]);
    }
    return mix_double(h, sum);
}

static void poly_teardown([[cx::escapes]] void *state) {
    struct poly *s = (struct poly *)state;
    bench_free(s->x);
    bench_free(s->acc);
    bench_free(s->c);
    bench_free(s);
}

extern const struct bench bench_fp_poly = {
    "fp_poly", "ops", "Horner evaluation of a degree-12 polynomial over a double array",
    poly_setup, poly_run, poly_teardown,
};

/* ---- fp_divsqrt: double division and square root ------------------------- */

enum { DS_N = 1 << 14, DS_PASSES = 200 };

struct divsqrt { double *a; double *b; double *acc; };

static void *divsqrt_setup(void) {
    struct divsqrt *s = (struct divsqrt *)bench_alloc(sizeof *s);
    s->a = (double *)bench_alloc(DS_N * sizeof(double));
    s->b = (double *)bench_alloc(DS_N * sizeof(double));
    s->acc = (double *)bench_alloc(DS_N * sizeof(double));
    struct rng r = { 0xd1f5e7u };
    for (size_t i = 0; i < DS_N; i++) {
        s->a[i] = 1.0 + rng_unit(&r);
        s->b[i] = 0.5 + rng_unit(&r);
    }
    return s;
}

/* all operands are positive: a in [1, 2), b in [0.5, 1.5), k > 0 */
static void divsqrt_kernel(double *acc, const double *a, const double *b, size_t n, double k) {
    for (size_t i = 0; i < n; i++) {
        double q = a[i] / b[i];
        acc[i] += sqrt(q + k) / (a[i] + k);
    }
}

static uint64_t divsqrt_run(void *state) {
    const struct divsqrt *s = (const struct divsqrt *)state;
    for (size_t i = 0; i < DS_N; i++) s->acc[i] = 0.0;
    for (int pass = 0; pass < DS_PASSES; pass++)
        divsqrt_kernel(s->acc, s->a, s->b, DS_N, 0.25 + (double)pass / DS_PASSES);
    double sum = 0.0;
    uint64_t h = 0;
    for (size_t i = 0; i < DS_N; i++) {
        sum += s->acc[i];
        if ((i & 1023) == 0) h = mix_double(h, s->acc[i]);
    }
    return mix_double(h, sum);
}

static void divsqrt_teardown([[cx::escapes]] void *state) {
    struct divsqrt *s = (struct divsqrt *)state;
    bench_free(s->a);
    bench_free(s->b);
    bench_free(s->acc);
    bench_free(s);
}

extern const struct bench bench_fp_divsqrt = {
    "fp_divsqrt", "ops", "double division and sqrt over arrays",
    divsqrt_setup, divsqrt_run, divsqrt_teardown,
};

/* ---- fp_libm: exp, log, sin, cos and pow -------------------------------- */

enum { LIBM_N = 1 << 12, LIBM_PASSES = 40 };

struct libm { double *x; };

static void *libm_setup(void) {
    struct libm *s = (struct libm *)bench_alloc(sizeof *s);
    s->x = (double *)bench_alloc(LIBM_N * sizeof(double));
    struct rng r = { 0x11b3a7eu };
    for (size_t i = 0; i < LIBM_N; i++) s->x[i] = 0.1 + 2.0 * rng_unit(&r);
    return s;
}

static uint64_t libm_run(void *state) {
    const struct libm *s = (const struct libm *)state;
    uint64_t h = 0;
    for (int pass = 0; pass < LIBM_PASSES; pass++) {
        double t = 0.5 + (double)pass / LIBM_PASSES;   /* t in [0.5, 1.5) */
        double e = 0.0, l = 0.0, sc = 0.0, p = 0.0;
        for (size_t i = 0; i < LIBM_N; i++) {
            double x = s->x[i];                         /* x in [0.1, 2.1) */
            e += exp(x * t * 0.5);
            l += log(x + t);
            sc += sin(x * t) + cos(x - t);
            p += pow(x, t);
        }
        h = mix_double(mix_double(mix_double(mix_double(h, e), l), sc), p);
    }
    return h;
}

static void libm_teardown([[cx::escapes]] void *state) {
    struct libm *s = (struct libm *)state;
    bench_free(s->x);
    bench_free(s);
}

extern const struct bench bench_fp_libm = {
    "fp_libm", "ops", "exp, log, sin, cos and pow over a double array",
    libm_setup, libm_run, libm_teardown,
};

/* ---- fp_reduce: float sum and dot product in source order ---------------- */

enum { RED_N = 1 << 16, RED_PASSES = 300 };

struct fred { float *a; float *b; };

static void *fred_setup(void) {
    struct fred *s = (struct fred *)bench_alloc(sizeof *s);
    s->a = (float *)bench_alloc(RED_N * sizeof(float));
    s->b = (float *)bench_alloc(RED_N * sizeof(float));
    struct rng r = { 0xf10a7u };
    for (size_t i = 0; i < RED_N; i++) {
        s->a[i] = (float)rng_unit(&r);
        s->b[i] = (float)(rng_unit(&r) * 2.0 - 1.0);
    }
    return s;
}

static uint64_t fred_run(void *state) {
    const struct fred *s = (const struct fred *)state;
    uint64_t h = 0;
    for (int pass = 0; pass < RED_PASSES; pass++) {
        /* a pass-dependent start value keeps each pass's sums distinct */
        float sum = (float)pass;
        for (size_t i = 0; i < RED_N; i++) sum += s->a[i];
        float dot = (float)pass * 0.5f;
        for (size_t i = 0; i < RED_N; i++) dot += s->a[i] * s->b[i];
        h = mix_double(mix_double(h, (double)sum), (double)dot);
    }
    return h;
}

static void fred_teardown([[cx::escapes]] void *state) {
    struct fred *s = (struct fred *)state;
    bench_free(s->a);
    bench_free(s->b);
    bench_free(s);
}

extern const struct bench bench_fp_reduce = {
    "fp_reduce", "ops", "float sum and float dot product reductions in source order",
    fred_setup, fred_run, fred_teardown,
};
