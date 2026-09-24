/* Prefix sums (scans) over int64: inclusive, segmented and strided
 * (column-wise) scans, and exclusive scans that turn flags or counts into
 * output offsets for stream compaction and counting sort. */
#include "bench.cxh"

#include <string.h>

/* ---- prefix_sum ---------------------------------------------------------- */

enum {
    PS_N = 1 << 16,             /* elements */
    PS_COLS = 256,              /* the strided scan sees PS_ROWS rows of PS_COLS */
    PS_ROWS = PS_N / PS_COLS,
    PS_BUCKETS = 1 << 8,        /* counting-sort keys (one radix digit) */
    PS_SEG = 48,                /* mean segment length of the segmented scan */
    PS_VMAX = 1 << 20,          /* |x| <= PS_VMAX */
    PS_PASSES = 192,            /* pass p adds bias p to every value: |sums| < 2^37 */
    PS_SORT_EVERY = 4,          /* passes per counting sort, so its scatter does not dominate */
};

/* out[i] = (x[0] + bias) + ... + (x[i] + bias) */
static void scan_inclusive(const int64_t *x, int64_t *out, size_t n, int64_t bias) {
    int64_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc += x[i] + bias;
        out[i] = acc;
    }
}

/* Inclusive scan that restarts wherever head[i] != 0. */
static void scan_segmented(const int64_t *x, const uint8_t *head, int64_t *out, size_t n, int64_t bias) {
    int64_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (head[i] ? 0 : acc) + x[i] + bias;
        out[i] = acc;
    }
}

/* Strided scan: x is rows x cols, row-major, and each column is scanned on
 * its own (stride cols), as in a summed-area table. */
static void scan_columns(const int64_t *x, int64_t *out, size_t rows, size_t cols, int64_t bias) {
    for (size_t c = 0; c < cols; c++) out[c] = x[c] + bias;
    for (size_t r = 1; r < rows; r++) {
        const int64_t *xr = x + r * cols;
        const int64_t *above = out + (r - 1) * cols;
        int64_t *row = out + r * cols;
        for (size_t c = 0; c < cols; c++) row[c] = above[c] + xr[c] + bias;
    }
}

/* Stream compaction: out[0..k) = the elements of x greater than t, in order;
 * returns k. pos[i] = the number of kept elements before i, an exclusive scan
 * of the keep flags, is where x[i] goes. The scatter is branch-free: a
 * dropped element lands in the slot the next kept element overwrites (or in
 * out[k], past the result). */
static size_t compact(const int64_t *x, size_t n, int64_t t, uint32_t *pos, int64_t *out) {
    uint32_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        pos[i] = kept;
        kept += x[i] > t ? 1u : 0u;
    }
    for (size_t i = 0; i < n; i++) out[pos[i]] = x[i];
    return kept;
}

/* Stable counting sort of x by (key + shift) mod PS_BUCKETS: histogram, an
 * exclusive scan of the counts into bucket offsets, then a scatter. */
static void counting_sort(const int64_t *x, const uint16_t *key, size_t n, uint32_t shift,
                          uint32_t *count, int64_t *out) {
    memset(count, 0, PS_BUCKETS * sizeof(uint32_t));
    for (size_t i = 0; i < n; i++) count[((uint32_t)key[i] + shift) & (PS_BUCKETS - 1)]++;
    uint32_t sum = 0;
    for (size_t b = 0; b < PS_BUCKETS; b++) {
        uint32_t c = count[b];
        count[b] = sum;
        sum += c;
    }
    for (size_t i = 0; i < n; i++) {
        size_t b = ((uint32_t)key[i] + shift) & (PS_BUCKETS - 1);
        out[count[b]] = x[i];
        count[b]++;
    }
}

/* Every 512th element and the last one. */
static uint64_t sample_i64(uint64_t h, const int64_t *a, size_t n) {
    for (size_t i = 0; i < n; i += 512) h = mix(h, (uint64_t)a[i]);
    return mix(h, (uint64_t)a[n - 1]);
}

struct prefix_sum {
    [[cx::owned]] int64_t *x;           /* values in [-PS_VMAX, PS_VMAX] */
    [[cx::owned]] uint8_t *head;        /* 1 where a segment starts */
    [[cx::owned]] uint16_t *key;        /* counting-sort keys < PS_BUCKETS */
    [[cx::owned]] int64_t *out;
    [[cx::owned]] uint32_t *pos;        /* exclusive-scan offsets, one per element */
    [[cx::owned]] uint32_t *count;      /* PS_BUCKETS counts, then offsets */
};

static void *prefix_sum_setup(void) {
    struct prefix_sum *s = (struct prefix_sum *)bench_alloc(sizeof *s);
    s->x = (int64_t *)bench_alloc(PS_N * sizeof(int64_t));
    s->head = (uint8_t *)bench_alloc(PS_N);
    s->key = (uint16_t *)bench_alloc(PS_N * sizeof(uint16_t));
    s->out = (int64_t *)bench_alloc(PS_N * sizeof(int64_t));
    s->pos = (uint32_t *)bench_alloc(PS_N * sizeof(uint32_t));
    s->count = (uint32_t *)bench_alloc(PS_BUCKETS * sizeof(uint32_t));
    struct rng r = { 0x5eed0401u };
    for (size_t i = 0; i < PS_N; i++) {
        s->x[i] = (int64_t)rng_below(&r, 2 * PS_VMAX + 1) - PS_VMAX;
        s->head[i] = (uint8_t)(i == 0 || rng_below(&r, PS_SEG) == 0 ? 1 : 0);
        s->key[i] = (uint16_t)rng_below(&r, PS_BUCKETS);
    }
    return s;
}

static uint64_t prefix_sum_run(void *state) {
    const struct prefix_sum *s = (const struct prefix_sum *)state;
    uint64_t h = 0;
    for (uint32_t p = 0; p < PS_PASSES; p++) {
        int64_t bias = (int64_t)p;
        scan_inclusive(s->x, s->out, PS_N, bias);
        h = sample_i64(h, s->out, PS_N);
        scan_segmented(s->x, s->head, s->out, PS_N, bias);
        h = sample_i64(h, s->out, PS_N);
        scan_columns(s->x, s->out, PS_ROWS, PS_COLS, bias);
        h = sample_i64(h, s->out, PS_N);
        /* thresholds sweep from keeping almost everything to almost nothing */
        int64_t t = (int64_t)p * (2 * PS_VMAX / PS_PASSES) - PS_VMAX;
        size_t kept = compact(s->x, PS_N, t, s->pos, s->out);
        h = mix(h, kept);
        if (kept > 0) h = sample_i64(h, s->out, kept);
        if (p % PS_SORT_EVERY == 0) {
            counting_sort(s->x, s->key, PS_N, p, s->count, s->out);
            h = sample_i64(h, s->out, PS_N);
        }
    }
    return h;
}

static void prefix_sum_teardown([[cx::escapes]] void *state) {
    struct prefix_sum *s = (struct prefix_sum *)state;
    bench_free(s->x);
    bench_free(s->head);
    bench_free(s->key);
    bench_free(s->out);
    bench_free(s->pos);
    bench_free(s->count);
    bench_free(s);
}

extern const struct bench bench_prefix_sum = {
    "prefix_sum", "alg", "int64 scans: inclusive, segmented, strided (column-wise); exclusive scans as offsets for compaction and counting sort",
    prefix_sum_setup, prefix_sum_run, prefix_sum_teardown,
};
