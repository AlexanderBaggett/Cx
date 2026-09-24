/* Memory operations: memcpy/memset/memcmp, DRAM bandwidth and latency,
 * strided access, and malloc/free churn. */
#include "bench.h"

#include <string.h>

/* Fill a byte buffer with pseudo-random bytes. */
static void fill_random_bytes(unsigned char *p, size_t n, struct rng *r) {
    uint64_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if ((i & 7) == 0) w = rng_next(r);
        p[i] = (unsigned char)(w & 0xff);
        w >>= 8;
    }
}

/* Fletcher-style sums over a byte range; position-sensitive. */
static uint64_t fletcher_bytes(const unsigned char *p, size_t n) {
    uint64_t a = 0, b = 0;
    for (size_t i = 0; i < n; i++) {
        a += (uint64_t)p[i];
        b += a;
    }
    return mix(mix(0, a), b);
}

/* ---- mem_copy: memcpy of small, 4 KB and 1 MB blocks --------------------- */

enum {
    MC_SRC_SIZE = 4 << 20,
    MC_SMALL_N = 1 << 14, MC_SMALL_REPS = 64, MC_SMALL_SRC = 1 << 18, MC_SMALL_REGION = 1 << 16,
    MC_MED_N = 1 << 10, MC_MED_REPS = 64, MC_MED_SRC = 1 << 20, MC_MED_REGION = 1 << 18, MC_MED_SIZE = 4096,
    MC_BIG_N = 96, MC_BIG_SIZE = 1 << 20,
    MC_DST_MED = MC_BIG_SIZE,
    MC_DST_SMALL = MC_DST_MED + MC_MED_REGION + MC_MED_SIZE,
    MC_DST_SIZE = MC_DST_SMALL + MC_SMALL_REGION + 256,
};

struct mcopy {
    unsigned char *src; unsigned char *dst;
    uint32_t *s_src; uint32_t *s_dst; uint32_t *s_len;
    uint32_t *m_src; uint32_t *m_dst;
    uint32_t *b_src;
};

static void *mcopy_setup(void) {
    struct mcopy *s = (struct mcopy *)bench_alloc(sizeof *s);
    s->src = (unsigned char *)bench_alloc(MC_SRC_SIZE);
    s->dst = (unsigned char *)bench_zalloc(MC_DST_SIZE);
    s->s_src = (uint32_t *)bench_alloc(MC_SMALL_N * sizeof(uint32_t));
    s->s_dst = (uint32_t *)bench_alloc(MC_SMALL_N * sizeof(uint32_t));
    s->s_len = (uint32_t *)bench_alloc(MC_SMALL_N * sizeof(uint32_t));
    s->m_src = (uint32_t *)bench_alloc(MC_MED_N * sizeof(uint32_t));
    s->m_dst = (uint32_t *)bench_alloc(MC_MED_N * sizeof(uint32_t));
    s->b_src = (uint32_t *)bench_alloc(MC_BIG_N * sizeof(uint32_t));
    struct rng r = { 0x3e3c0b1u };
    fill_random_bytes(s->src, MC_SRC_SIZE, &r);
    for (size_t k = 0; k < MC_SMALL_N; k++) {
        s->s_len[k] = 8 + rng_below(&r, 249);                 /* 8..256 bytes */
        s->s_src[k] = rng_below(&r, MC_SMALL_SRC - 256);
        s->s_dst[k] = rng_below(&r, MC_SMALL_REGION);
    }
    for (size_t k = 0; k < MC_MED_N; k++) {
        s->m_src[k] = rng_below(&r, MC_MED_SRC - MC_MED_SIZE);
        s->m_dst[k] = rng_below(&r, MC_MED_REGION);
    }
    for (size_t k = 0; k < MC_BIG_N; k++)
        s->b_src[k] = rng_below(&r, MC_SRC_SIZE - MC_BIG_SIZE) & ~63u;
    return s;
}

static uint64_t mcopy_run(void *state) {
    const struct mcopy *s = (const struct mcopy *)state;
    uint64_t acc = 0;
    unsigned char *dsmall = s->dst + MC_DST_SMALL;
    for (int rep = 0; rep < MC_SMALL_REPS; rep++)
        for (size_t k = 0; k < MC_SMALL_N; k++) {
            memcpy(dsmall + s->s_dst[k], s->src + s->s_src[k], s->s_len[k]);
            acc += (uint64_t)dsmall[s->s_dst[k] + s->s_len[k] - 1];
        }
    unsigned char *dmed = s->dst + MC_DST_MED;
    for (int rep = 0; rep < MC_MED_REPS; rep++)
        for (size_t k = 0; k < MC_MED_N; k++) {
            memcpy(dmed + s->m_dst[k], s->src + s->m_src[k], MC_MED_SIZE);
            acc += (uint64_t)dmed[s->m_dst[k]];
        }
    for (size_t k = 0; k < MC_BIG_N; k++) {
        memcpy(s->dst, s->src + s->b_src[k], MC_BIG_SIZE);
        acc += (uint64_t)s->dst[k * 4099];
    }
    return mix(acc, fletcher_bytes(s->dst, MC_DST_SIZE));
}

static void mcopy_teardown([[cx::escapes]] void *state) {
    struct mcopy *s = (struct mcopy *)state;
    bench_free(s->src);
    bench_free(s->dst);
    bench_free(s->s_src);
    bench_free(s->s_dst);
    bench_free(s->s_len);
    bench_free(s->m_src);
    bench_free(s->m_dst);
    bench_free(s->b_src);
    bench_free(s);
}

extern const struct bench bench_mem_copy = {
    "mem_copy", "ops", "memcpy of 8-256 B, 4 KB and 1 MB blocks",
    mcopy_setup, mcopy_run, mcopy_teardown,
};

/* ---- mem_set_cmp: memset then memcmp of the same ranges ------------------ */

enum { MSC_BUF = 1 << 20, MSC_OPS = 1 << 14 };

struct msc {
    unsigned char *x; unsigned char *y;
    uint32_t *off; uint32_t *len; uint32_t *pos;
    uint8_t *val; uint8_t *flip;
};

static void *msc_setup(void) {
    struct msc *s = (struct msc *)bench_alloc(sizeof *s);
    s->x = (unsigned char *)bench_zalloc(MSC_BUF);
    s->y = (unsigned char *)bench_zalloc(MSC_BUF);
    s->off = (uint32_t *)bench_alloc(MSC_OPS * sizeof(uint32_t));
    s->len = (uint32_t *)bench_alloc(MSC_OPS * sizeof(uint32_t));
    s->pos = (uint32_t *)bench_alloc(MSC_OPS * sizeof(uint32_t));
    s->val = (uint8_t *)bench_alloc(MSC_OPS);
    s->flip = (uint8_t *)bench_alloc(MSC_OPS);
    struct rng r = { 0x5e7c3bu };
    for (size_t k = 0; k < MSC_OPS; k++) {
        uint32_t kind = rng_below(&r, 10);
        uint32_t len = kind < 6 ? 16 + rng_below(&r, 497)            /* 16 B .. 512 B */
                     : kind < 9 ? 1024 + rng_below(&r, 15 * 1024 + 1) /* 1 KB .. 16 KB */
                     : (64u << 10) + rng_below(&r, (192u << 10) + 1); /* 64 KB .. 256 KB */
        s->len[k] = len;
        s->off[k] = rng_below(&r, MSC_BUF - len + 1);
        s->pos[k] = len / 2 + rng_below(&r, len - len / 2);      /* second half */
        s->val[k] = (uint8_t)rng_below(&r, 256);
        s->flip[k] = (uint8_t)(rng_below(&r, 2) ? 1 + rng_below(&r, 255) : 0);
    }
    return s;
}

static uint64_t msc_run(void *state) {
    const struct msc *s = (const struct msc *)state;
    uint64_t h = 0;
    for (size_t k = 0; k < MSC_OPS; k++) {
        unsigned char *xp = s->x + s->off[k];
        unsigned char *yp = s->y + s->off[k];
        size_t len = s->len[k];
        memset(xp, s->val[k], len);
        memset(yp, s->val[k], len);
        if (s->flip[k]) yp[s->pos[k]] = (unsigned char)(s->val[k] ^ s->flip[k]);
        int c = memcmp(xp, yp, len);
        h = mix(h, c < 0 ? 1u : c > 0 ? 2u : 3u);
    }
    return h;
}

static void msc_teardown([[cx::escapes]] void *state) {
    struct msc *s = (struct msc *)state;
    bench_free(s->x);
    bench_free(s->y);
    bench_free(s->off);
    bench_free(s->len);
    bench_free(s->pos);
    bench_free(s->val);
    bench_free(s->flip);
    bench_free(s);
}

extern const struct bench bench_mem_set_cmp = {
    "mem_set_cmp", "ops", "memset two ranges then memcmp them (16 B to 256 KB)",
    msc_setup, msc_run, msc_teardown,
};

/* ---- mem_stream: STREAM triad a[i] = b[i] + s*c[i] ----------------------- */

enum { STREAM_N = 1 << 21, STREAM_REPS = 8, STREAM_SAMPLE = 4099 };   /* 16 MB per array */

struct stream { double *a; double *b; double *c; };

static void *stream_setup(void) {
    struct stream *s = (struct stream *)bench_alloc(sizeof *s);
    s->a = (double *)bench_alloc(STREAM_N * sizeof(double));
    s->b = (double *)bench_alloc(STREAM_N * sizeof(double));
    s->c = (double *)bench_alloc(STREAM_N * sizeof(double));
    struct rng r = { 0x57a3au };
    for (size_t i = 0; i < STREAM_N; i++) {
        s->a[i] = 0.0;
        s->b[i] = rng_unit(&r);
        s->c[i] = rng_unit(&r);
    }
    return s;
}

static void stream_triad(double *a, const double *b, const double *c, double scalar, size_t n) {
    for (size_t i = 0; i < n; i++) a[i] = b[i] + scalar * c[i];
}

static uint64_t stream_run(void *state) {
    const struct stream *s = (const struct stream *)state;
    uint64_t h = 0;
    for (int rep = 0; rep < STREAM_REPS; rep++) {
        stream_triad(s->a, s->b, s->c, 1.0 + 0.25 * (double)rep, STREAM_N);
        double sample = 0.0;
        for (size_t i = (size_t)rep; i < STREAM_N; i += STREAM_SAMPLE) sample += s->a[i];
        h = mix_double(h, sample);
    }
    return h;
}

static void stream_teardown([[cx::escapes]] void *state) {
    struct stream *s = (struct stream *)state;
    bench_free(s->a);
    bench_free(s->b);
    bench_free(s->c);
    bench_free(s);
}

extern const struct bench bench_mem_stream = {
    "mem_stream", "ops", "STREAM triad a = b + s*c over three 16 MB double arrays",
    stream_setup, stream_run, stream_teardown,
};

/* ---- mem_chase: pointer chasing through a random single-cycle permutation - */

enum { CHASE_CELLS = 1 << 21, CHASE_STEPS = 1 << 19 };   /* 16-byte cells: 32 MB */

struct chase_cell { struct chase_cell *next; uint64_t val; };

struct chase { struct chase_cell *cells; };

static void *chase_setup(void) {
    struct chase *s = (struct chase *)bench_alloc(sizeof *s);
    s->cells = (struct chase_cell *)bench_alloc(CHASE_CELLS * sizeof(struct chase_cell));
    uint32_t *perm = (uint32_t *)bench_alloc(CHASE_CELLS * sizeof(uint32_t));
    for (uint32_t i = 0; i < CHASE_CELLS; i++) perm[i] = i;
    /* Sattolo's algorithm: i -> perm[i] is a single cycle through every cell */
    struct rng r = { 0xc4a5e5u };
    for (uint32_t i = CHASE_CELLS - 1; i > 0; i--) {
        uint32_t j = rng_below(&r, i);
        uint32_t t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
    for (size_t i = 0; i < CHASE_CELLS; i++) {
        s->cells[i].next = &s->cells[perm[i]];
        s->cells[i].val = i;
    }
    bench_free(perm);
    return s;
}

static uint64_t chase_run(void *state) {
    const struct chase *s = (const struct chase *)state;
    const struct chase_cell *p = &s->cells[0];
    uint64_t sum = 0;
    for (size_t k = 0; k < CHASE_STEPS; k++) {
        sum += p->val;
        p = p->next;
    }
    return mix(mix(0, sum), p->val);
}

static void chase_teardown([[cx::escapes]] void *state) {
    struct chase *s = (struct chase *)state;
    bench_free(s->cells);
    bench_free(s);
}

extern const struct bench bench_mem_chase = {
    "mem_chase", "ops", "pointer chasing through a random single-cycle permutation (32 MB)",
    chase_setup, chase_run, chase_teardown,
};

/* ---- mem_strided: column-order traversal of a row-major matrix ----------- */

enum { STRIDED_DIM = 2048, STRIDED_PASSES = 2 };   /* 2048 x 2048 doubles: 32 MB */

struct strided { double *m; double *col; };

static void *strided_setup(void) {
    struct strided *s = (struct strided *)bench_alloc(sizeof *s);
    s->m = (double *)bench_alloc((size_t)STRIDED_DIM * STRIDED_DIM * sizeof(double));
    s->col = (double *)bench_alloc(STRIDED_DIM * sizeof(double));
    struct rng r = { 0x57e1dedu };
    for (size_t i = 0; i < (size_t)STRIDED_DIM * STRIDED_DIM; i++) s->m[i] = rng_unit(&r);
    return s;
}

static uint64_t strided_run(void *state) {
    const struct strided *s = (const struct strided *)state;
    uint64_t h = 0;
    for (size_t pass = 0; pass < STRIDED_PASSES; pass++) {
        /* each pass starts at a different column */
        for (size_t jj = 0; jj < STRIDED_DIM; jj++) {
            size_t j = (jj + pass * 13) & (STRIDED_DIM - 1);
            double sum = 0.0;
            for (size_t i = 0; i < STRIDED_DIM; i++) sum += s->m[i * STRIDED_DIM + j];
            s->col[j] = sum;
        }
        double total = 0.0;
        for (size_t j = 0; j < STRIDED_DIM; j++) total += s->col[j] * (double)(j & 15);
        h = mix_double(mix_double(h, total), s->col[pass]);
    }
    return h;
}

static void strided_teardown([[cx::escapes]] void *state) {
    struct strided *s = (struct strided *)state;
    bench_free(s->m);
    bench_free(s->col);
    bench_free(s);
}

extern const struct bench bench_mem_strided = {
    "mem_strided", "ops", "column-order sums over a row-major 2048x2048 double matrix",
    strided_setup, strided_run, strided_teardown,
};

/* ---- alloc_churn: malloc/free with a sliding window of live blocks ------- */

enum { AC_OPS = 1 << 20, AC_WINDOW = 1024 };

struct churn {
    uint16_t *size; uint16_t *victim;
    unsigned char **slot; size_t *slot_len;
};

static void *churn_setup(void) {
    struct churn *s = (struct churn *)bench_alloc(sizeof *s);
    s->size = (uint16_t *)bench_alloc(AC_OPS * sizeof(uint16_t));
    s->victim = (uint16_t *)bench_alloc(AC_OPS * sizeof(uint16_t));
    s->slot = (unsigned char **)bench_alloc(AC_WINDOW * sizeof(unsigned char *));
    s->slot_len = (size_t *)bench_alloc(AC_WINDOW * sizeof(size_t));
    struct rng r = { 0xa110cu };
    for (size_t k = 0; k < AC_OPS; k++) {
        /* half the blocks are 16..256 B, half 16..4096 B */
        uint32_t n = (k & 1) ? 16 + rng_below(&r, 241) : 16 + rng_below(&r, 4081);
        s->size[k] = (uint16_t)n;
        s->victim[k] = (uint16_t)rng_below(&r, AC_WINDOW);
    }
    for (size_t w = 0; w < AC_WINDOW; w++) {
        s->slot[w] = NULL;
        s->slot_len[w] = 0;
    }
    return s;
}

static uint64_t churn_run(void *state) {
    const struct churn *s = (const struct churn *)state;
    uint64_t h = 0;
    for (size_t k = 0; k < AC_OPS; k++) {
        size_t w = s->victim[k];
        unsigned char *old = s->slot[w];
        if (old) {
            h = mix(h, ((uint64_t)old[0] << 8) | (uint64_t)old[s->slot_len[w] - 1]);
            bench_free(old);
        }
        size_t n = s->size[k];
        unsigned char *p = (unsigned char *)bench_alloc(n);
        p[0] = (unsigned char)(k & 0xff);
        p[n - 1] = (unsigned char)(n & 0xff);
        s->slot[w] = p;
        s->slot_len[w] = n;
    }
    for (size_t w = 0; w < AC_WINDOW; w++) {
        unsigned char *old = s->slot[w];
        if (old) {
            h = mix(h, ((uint64_t)old[0] << 8) | (uint64_t)old[s->slot_len[w] - 1]);
            bench_free(old);
            s->slot[w] = NULL;
        }
    }
    return h;
}

static void churn_teardown([[cx::escapes]] void *state) {
    struct churn *s = (struct churn *)state;
    bench_free(s->size);
    bench_free(s->victim);
    bench_free(s->slot);
    bench_free(s->slot_len);
    bench_free(s);
}

extern const struct bench bench_alloc_churn = {
    "alloc_churn", "ops", "malloc/free of 16-4096 B blocks, ~1000 live, random victims",
    churn_setup, churn_run, churn_teardown,
};
