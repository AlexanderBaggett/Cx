/* Numeric kernels: dense matrix multiply, FFT, stencil, spectral norm, sparse matrix-vector product. */
#include "bench.cxh"

#include <math.h>
#include <string.h>

/* Checksum of an n x n row-major matrix: one mixed row sum per row. */
static uint64_t matrix_checksum(const double *m, size_t n) {
    uint64_t h = 0;
    for (size_t i = 0; i < n; i++) {
        double sum = 0.0;
        for (size_t j = 0; j < n; j++) sum += m[i * n + j];
        h = mix_double(h, sum);
    }
    return h;
}

/* ---- matmul_naive / matmul_blocked: C = A*B in double --------------------- */

enum { MM_N = 512, MM_TILE = 64 };

struct matmul {
    [[cx::owned]] double *a;
    [[cx::owned]] double *b;
    [[cx::owned]] double *c;
};

static void *matmul_setup(void) {
    struct matmul *s = (struct matmul *)bench_alloc(sizeof *s);
    size_t elems = (size_t)MM_N * MM_N;
    s->a = (double *)bench_alloc(elems * sizeof(double));
    s->b = (double *)bench_alloc(elems * sizeof(double));
    s->c = (double *)bench_alloc(elems * sizeof(double));
    struct rng r = { 0x2545f4914f6cdd1du };
    for (size_t i = 0; i < elems; i++) {
        s->a[i] = rng_unit(&r) * 2.0 - 1.0;
        s->b[i] = rng_unit(&r) * 2.0 - 1.0;
    }
    return s;
}

/* i-k-j order: the inner loop streams a row of B into a row of C. */
static void matmul_ikj(const double *a, const double *b, double *c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        double *ci = c + i * n;
        for (size_t j = 0; j < n; j++) ci[j] = 0.0;
        for (size_t k = 0; k < n; k++) {
            double aik = a[i * n + k];
            const double *bk = b + k * n;
            for (size_t j = 0; j < n; j++) ci[j] += aik * bk[j];
        }
    }
}

/* Same arithmetic in tiles. Each C element still accumulates k in ascending
 * order, so the result is bit-identical to matmul_ikj. */
static void matmul_tiled(const double *a, const double *b, double *c, size_t n, size_t tile) {
    for (size_t i = 0; i < n * n; i++) c[i] = 0.0;
    for (size_t ii = 0; ii < n; ii += tile) {
        size_t ie = ii + tile < n ? ii + tile : n;
        for (size_t kk = 0; kk < n; kk += tile) {
            size_t ke = kk + tile < n ? kk + tile : n;
            for (size_t jj = 0; jj < n; jj += tile) {
                size_t je = jj + tile < n ? jj + tile : n;
                for (size_t i = ii; i < ie; i++) {
                    double *ci = c + i * n;
                    for (size_t k = kk; k < ke; k++) {
                        double aik = a[i * n + k];
                        const double *bk = b + k * n;
                        for (size_t j = jj; j < je; j++) ci[j] += aik * bk[j];
                    }
                }
            }
        }
    }
}

static uint64_t matmul_naive_run(void *state) {
    const struct matmul *s = (const struct matmul *)state;
    matmul_ikj(s->a, s->b, s->c, MM_N);
    return matrix_checksum(s->c, MM_N);
}

static uint64_t matmul_blocked_run(void *state) {
    const struct matmul *s = (const struct matmul *)state;
    matmul_tiled(s->a, s->b, s->c, MM_N, MM_TILE);
    return matrix_checksum(s->c, MM_N);
}

static void matmul_teardown([[cx::escapes]] void *state) {
    struct matmul *s = (struct matmul *)state;
    bench_free(s->a);
    bench_free(s->b);
    bench_free(s->c);
    bench_free(s);
}

extern const struct bench bench_matmul_naive = {
    "matmul_naive", "kern", "double matrix multiply 512x512, i-k-j triple loop",
    matmul_setup, matmul_naive_run, matmul_teardown,
};

extern const struct bench bench_matmul_blocked = {
    "matmul_blocked", "kern", "double matrix multiply 512x512, 64x64 cache tiles",
    matmul_setup, matmul_blocked_run, matmul_teardown,
};

/* ---- fft_radix2: iterative radix-2 complex FFT, forward and inverse ------- */

enum { FFT_LOG2 = 19, FFT_N = 1 << FFT_LOG2 };

static const double fft_pi = 3.14159265358979323846;

struct cpx { double re, im; };

struct fft {
    [[cx::owned]] struct cpx *in;   /* input signal */
    [[cx::owned]] struct cpx *x;    /* work buffer, transformed in place */
    [[cx::owned]] struct cpx *tw;   /* twiddles exp(-2*pi*i*k/N), k < N/2 */
};

static void *fft_setup(void) {
    struct fft *s = (struct fft *)bench_alloc(sizeof *s);
    s->in = (struct cpx *)bench_alloc(FFT_N * sizeof(struct cpx));
    s->x = (struct cpx *)bench_alloc(FFT_N * sizeof(struct cpx));
    s->tw = (struct cpx *)bench_alloc(FFT_N / 2 * sizeof(struct cpx));
    struct rng r = { 0x5851f42d4c957f2du };
    for (size_t i = 0; i < FFT_N; i++) {
        s->in[i].re = rng_unit(&r) * 2.0 - 1.0;
        s->in[i].im = rng_unit(&r) * 2.0 - 1.0;
    }
    for (size_t k = 0; k < FFT_N / 2; k++) {
        double ang = -2.0 * fft_pi * (double)k / (double)FFT_N;
        s->tw[k].re = cos(ang);
        s->tw[k].im = sin(ang);
    }
    return s;
}

/* In place; inverse uses conjugated twiddles and does not scale. */
static void fft_transform(struct cpx *x, const struct cpx *tw, size_t n, int inverse) {
    /* bit-reversal permutation */
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            struct cpx t = x[i];
            x[i] = x[j];
            x[j] = t;
        }
    }
    double sign = inverse ? -1.0 : 1.0;
    for (size_t len = 2; len <= n; len <<= 1) {
        size_t half = len >> 1, step = n / len;
        for (size_t i = 0; i < n; i += len) {
            for (size_t k = 0; k < half; k++) {
                double wr = tw[k * step].re, wi = sign * tw[k * step].im;
                struct cpx u = x[i + k], v = x[i + k + half];
                double vr = v.re * wr - v.im * wi;
                double vi = v.re * wi + v.im * wr;
                x[i + k].re = u.re + vr;
                x[i + k].im = u.im + vi;
                x[i + k + half].re = u.re - vr;
                x[i + k + half].im = u.im - vi;
            }
        }
    }
}

static uint64_t fft_run(void *state) {
    const struct fft *s = (const struct fft *)state;
    memcpy(s->x, s->in, FFT_N * sizeof(struct cpx));
    fft_transform(s->x, s->tw, FFT_N, 0);
    double sr = 0.0, si = 0.0, sa = 0.0;
    for (size_t k = 0; k < FFT_N; k++) {
        sr += s->x[k].re;
        si += s->x[k].im;
        sa += fabs(s->x[k].re) + fabs(s->x[k].im);
    }
    uint64_t h = mix_double(mix_double(mix_double(0, sr), si), sa);
    fft_transform(s->x, s->tw, FFT_N, 1);
    double scale = 1.0 / (double)FFT_N, back = 0.0;
    uint64_t bad = 0;
    for (size_t k = 0; k < FFT_N; k++) {
        double re = s->x[k].re * scale, im = s->x[k].im * scale;
        back += re - im;
        if (fabs(re - s->in[k].re) > 1e-9 || fabs(im - s->in[k].im) > 1e-9) bad++;
    }
    return mix(mix_double(h, back), bad);
}

static void fft_teardown([[cx::escapes]] void *state) {
    struct fft *s = (struct fft *)state;
    bench_free(s->in);
    bench_free(s->x);
    bench_free(s->tw);
    bench_free(s);
}

extern const struct bench bench_fft_radix2 = {
    "fft_radix2", "kern", "iterative radix-2 complex FFT of 2^19 points, forward + inverse",
    fft_setup, fft_run, fft_teardown,
};

/* ---- stencil_2d: 5-point Jacobi iterations on a 2D grid ------------------- */

enum { ST_N = 1024, ST_STEPS = 40 };

struct stencil {
    [[cx::owned]] double *init;
    [[cx::owned]] double *a;
    [[cx::owned]] double *b;
};

static void *stencil_setup(void) {
    struct stencil *s = (struct stencil *)bench_alloc(sizeof *s);
    size_t elems = (size_t)ST_N * ST_N;
    s->init = (double *)bench_alloc(elems * sizeof(double));
    s->a = (double *)bench_alloc(elems * sizeof(double));
    s->b = (double *)bench_alloc(elems * sizeof(double));
    struct rng r = { 0x7f4a7c159e3779b9u };
    for (size_t i = 0; i < ST_N; i++)
        for (size_t j = 0; j < ST_N; j++) {
            double v = rng_unit(&r);
            if (i == 0) v = 100.0;                      /* hot top edge */
            else if (i == ST_N - 1 || j == 0 || j == ST_N - 1) v = 0.0;
            s->init[i * ST_N + j] = v;
        }
    return s;
}

/* One Jacobi sweep of the interior; the boundary of dst is left untouched. */
static void jacobi_step(const double *src, double *dst, size_t n) {
    for (size_t i = 1; i + 1 < n; i++) {
        const double *up = src + (i - 1) * n, *mid = src + i * n, *down = src + (i + 1) * n;
        double *out = dst + i * n;
        for (size_t j = 1; j + 1 < n; j++)
            out[j] = 0.25 * ((up[j] + down[j]) + (mid[j - 1] + mid[j + 1]));
    }
}

static uint64_t stencil_run(void *state) {
    const struct stencil *s = (const struct stencil *)state;
    size_t bytes = (size_t)ST_N * ST_N * sizeof(double);
    memcpy(s->a, s->init, bytes);
    memcpy(s->b, s->init, bytes);
    double *src = s->a, *dst = s->b;
    for (int step = 0; step < ST_STEPS; step++) {
        jacobi_step(src, dst, ST_N);
        double *t = src;
        src = dst;
        dst = t;
    }
    return matrix_checksum(src, ST_N);
}

static void stencil_teardown([[cx::escapes]] void *state) {
    struct stencil *s = (struct stencil *)state;
    bench_free(s->init);
    bench_free(s->a);
    bench_free(s->b);
    bench_free(s);
}

extern const struct bench bench_stencil_2d = {
    "stencil_2d", "kern", "5-point Jacobi stencil, 1024x1024 double grid, 40 sweeps",
    stencil_setup, stencil_run, stencil_teardown,
};

/* ---- spectral_norm: power iteration on the classic infinite matrix -------- */

enum { SN_N = 1000, SN_ITERS = 10 };

/* The size and iteration count are copied into the state in setup, so the
 * kernel cannot be specialized to compile-time constants. */
struct spectral {
    [[cx::owned]] double *u;
    [[cx::owned]] double *v;
    [[cx::owned]] double *tmp;
    size_t n;
    int iters;
};

static void *spectral_setup(void) {
    struct spectral *s = (struct spectral *)bench_alloc(sizeof *s);
    s->n = SN_N;
    s->iters = SN_ITERS;
    s->u = (double *)bench_alloc(s->n * sizeof(double));
    s->v = (double *)bench_alloc(s->n * sizeof(double));
    s->tmp = (double *)bench_alloc(s->n * sizeof(double));
    return s;
}

/* A(i,j) = 1 / ((i+j)(i+j+1)/2 + i + 1); i, j < n = SN_N, so no overflow. */
static double spectral_a(size_t i, size_t j) {
    size_t ij = i + j;
    return 1.0 / (double)(ij * (ij + 1) / 2 + i + 1);
}

static void spectral_av(const double *v, double *av, size_t n) {
    for (size_t i = 0; i < n; i++) {
        double sum = 0.0;
        for (size_t j = 0; j < n; j++) sum += spectral_a(i, j) * v[j];
        av[i] = sum;
    }
}

static void spectral_atv(const double *v, double *atv, size_t n) {
    for (size_t i = 0; i < n; i++) {
        double sum = 0.0;
        for (size_t j = 0; j < n; j++) sum += spectral_a(j, i) * v[j];
        atv[i] = sum;
    }
}

static void spectral_atav(const double *v, double *out, double *tmp, size_t n) {
    spectral_av(v, tmp, n);
    spectral_atv(tmp, out, n);
}

static uint64_t spectral_run(void *state) {
    const struct spectral *s = (const struct spectral *)state;
    size_t n = s->n;
    for (size_t i = 0; i < n; i++) s->u[i] = 1.0;
    for (int it = 0; it < s->iters; it++) {
        spectral_atav(s->u, s->v, s->tmp, n);
        spectral_atav(s->v, s->u, s->tmp, n);
    }
    double vbv = 0.0, vv = 0.0;
    for (size_t i = 0; i < n; i++) {
        vbv += s->u[i] * s->v[i];
        vv += s->v[i] * s->v[i];
    }
    return mix_double(mix_double(0, sqrt(vbv / vv)), s->u[n - 1]);
}

static void spectral_teardown([[cx::escapes]] void *state) {
    struct spectral *s = (struct spectral *)state;
    bench_free(s->u);
    bench_free(s->v);
    bench_free(s->tmp);
    bench_free(s);
}

extern const struct bench bench_spectral_norm = {
    "spectral_norm", "kern", "spectral norm by power iteration, N=1000, 10 iterations",
    spectral_setup, spectral_run, spectral_teardown,
};

/* ---- spmv_csr: sparse matrix-vector products in CSR form ----------------- */

#ifndef SP_BAND_X
#define SP_BAND_X 16384
#define SP_FAR_X 1
#define SP_REPS_X 3
#endif
enum { SP_ROWS = 1 << 20, SP_MINNZ = 8, SP_MAXNZ = 10, SP_REPS = SP_REPS_X, SP_SAMPLE = 1024, SP_BAND = SP_BAND_X, SP_FAR = SP_FAR_X };

/* Row i holds entries rowptr[i] .. rowptr[i+1]-1 of col and val, with
 * distinct columns in ascending order, spread uniformly over the matrix.
 * The sizes are copied into the state in setup, so the kernel cannot be
 * specialized to compile-time constants. */
struct spmv {
    [[cx::owned]] uint32_t *rowptr;   /* rows + 1 entries */
    [[cx::owned]] uint32_t *col;
    [[cx::owned]] double *val;
    [[cx::owned]] double *x;          /* input vector */
    [[cx::owned]] double *y0;         /* products, ping-pong */
    [[cx::owned]] double *y1;
    size_t rows, reps;
};

static void *spmv_setup(void) {
    struct spmv *s = (struct spmv *)bench_alloc(sizeof *s);
    s->rows = SP_ROWS;
    s->reps = SP_REPS;
    size_t n = s->rows;
    s->rowptr = (uint32_t *)bench_alloc((n + 1) * sizeof(uint32_t));
    s->col = (uint32_t *)bench_alloc(n * SP_MAXNZ * sizeof(uint32_t));
    s->val = (double *)bench_alloc(n * SP_MAXNZ * sizeof(double));
    s->x = (double *)bench_alloc(n * sizeof(double));
    s->y0 = (double *)bench_alloc(n * sizeof(double));
    s->y1 = (double *)bench_alloc(n * sizeof(double));
    struct rng r = { 0x94d049bb133111ebu };
    uint32_t nnz = 0;
    s->rowptr[0] = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t k = SP_MINNZ + rng_below(&r, SP_MAXNZ - SP_MINNZ + 1);
        uint32_t *c = s->col + nnz;
        for (uint32_t j = 0; j < k; j++) {
            uint32_t v = 0;
            int dup = 1;
            while (dup) {                                   /* distinct columns */
                if (j < SP_FAR) v = rng_below(&r, (uint32_t)n);
                else {
                    size_t lo = i > SP_BAND ? i - SP_BAND : 0, hi = i + SP_BAND < n ? i + SP_BAND : n - 1;
                    v = (uint32_t)(lo + rng_below(&r, (uint32_t)(hi - lo + 1)));
                }
                dup = 0;
                for (uint32_t q = 0; q < j; q++)
                    if (c[q] == v) dup = 1;
            }
            uint32_t q = j;                                 /* insertion sort */
            for (; q > 0 && c[q - 1] > v; q--) c[q] = c[q - 1];
            c[q] = v;
        }
        for (uint32_t j = 0; j < k; j++) s->val[nnz + j] = rng_unit(&r) * 2.0 - 1.0;
        nnz += k;
        s->rowptr[i + 1] = nnz;
    }
    for (size_t i = 0; i < n; i++) s->x[i] = rng_unit(&r) * 2.0 - 1.0;
    return s;
}

/* y = A*x; each row is summed in column order. */
static void spmv_csr(const uint32_t *rowptr, const uint32_t *col, const double *val,
                     const double *x, double *y, size_t rows) {
    for (size_t i = 0; i < rows; i++) {
        double sum = 0.0;
        size_t end = rowptr[i + 1];
        for (size_t k = rowptr[i]; k < end; k++) sum += val[k] * x[col[k]];
        y[i] = sum;
    }
}

/* reps chained products A*x, A*(A*x), ...; checksum of the last one: every
 * SP_SAMPLE-th entry and the sum of all entries. */
static uint64_t spmv_run(void *state) {
    const struct spmv *s = (const struct spmv *)state;
    size_t n = s->rows;
    const double *in = s->x;
    double *out = s->y0;
    for (size_t rep = 0; rep < s->reps; rep++) {
        spmv_csr(s->rowptr, s->col, s->val, in, out, n);
        in = out;
        out = out == s->y0 ? s->y1 : s->y0;
    }
    uint64_t h = 0;
    double sum = 0.0;
    for (size_t i = 0; i < n; i++) sum += in[i];
    for (size_t i = 0; i < n; i += SP_SAMPLE) h = mix_double(h, in[i]);
    return mix_double(h, sum);
}

static void spmv_teardown([[cx::escapes]] void *state) {
    struct spmv *s = (struct spmv *)state;
    bench_free(s->rowptr);
    bench_free(s->col);
    bench_free(s->val);
    bench_free(s->x);
    bench_free(s->y0);
    bench_free(s->y1);
    bench_free(s);
}

extern const struct bench bench_spmv_csr = {
    "spmv_csr", "kern", "double CSR sparse matrix-vector product, 1M rows x 8-10 random columns, 4 chained",
    spmv_setup, spmv_run, spmv_teardown,
};
