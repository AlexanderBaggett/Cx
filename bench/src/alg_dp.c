/* Dynamic programming and backtracking: Levenshtein distance, 0/1 knapsack,
 * N-queens. */
#include "bench.cxh"

#include <string.h>

/* ---- dp_edit_distance: Levenshtein distance, two-row DP ------------------ */

enum { ED_PAIRS = 10, ED_LEN = 2000, ED_MAXLEN = 2 * ED_LEN };

/* Edit distance between a[0..n) and b[0..m); row0 and row1 hold m + 1 cells. */
static uint32_t edit_distance(const uint8_t *a, size_t n, const uint8_t *b, size_t m,
                              uint32_t *row0, uint32_t *row1) {
    uint32_t *prev = row0, *cur = row1;
    for (size_t j = 0; j <= m; j++) prev[j] = (uint32_t)j;
    for (size_t i = 1; i <= n; i++) {
        uint8_t ai = a[i - 1];
        cur[0] = (uint32_t)i;
        for (size_t j = 1; j <= m; j++) {
            uint32_t sub = prev[j - 1] + (ai != b[j - 1] ? 1u : 0u);
            uint32_t del = prev[j] + 1u;
            uint32_t ins = cur[j - 1] + 1u;
            uint32_t best = sub < del ? sub : del;
            cur[j] = best < ins ? best : ins;
        }
        uint32_t *t = prev;
        prev = cur;
        cur = t;
    }
    return prev[m];
}

struct dp_edit_distance {
    [[cx::owned]] uint8_t *str;     /* 2 * ED_PAIRS strings of up to ED_MAXLEN bytes */
    size_t len[2 * ED_PAIRS];
    [[cx::owned]] uint32_t *row0;
    [[cx::owned]] uint32_t *row1;
};

static void *dp_edit_distance_setup(void) {
    struct dp_edit_distance *s = (struct dp_edit_distance *)bench_alloc(sizeof *s);
    s->str = (uint8_t *)bench_alloc(2 * ED_PAIRS * ED_MAXLEN);
    s->row0 = (uint32_t *)bench_alloc((ED_MAXLEN + 1) * sizeof(uint32_t));
    s->row1 = (uint32_t *)bench_alloc((ED_MAXLEN + 1) * sizeof(uint32_t));
    struct rng r = { 0x5eed0301u };
    for (size_t k = 0; k < ED_PAIRS; k++) {
        /* a is random over 4 letters; b is a copied with ~1 edit in 8 */
        uint8_t *a = s->str + 2 * k * ED_MAXLEN;
        uint8_t *b = a + ED_MAXLEN;
        size_t n = ED_LEN - 200 + rng_below(&r, 401);
        for (size_t i = 0; i < n; i++) a[i] = (uint8_t)('a' + rng_below(&r, 4));
        size_t m = 0;
        for (size_t i = 0; i < n && m + 2 <= ED_MAXLEN; i++) {
            uint32_t op = rng_below(&r, 32);
            if (op == 0) continue;                                           /* delete */
            if (op == 1) { b[m] = (uint8_t)('a' + rng_below(&r, 4)); m++; }  /* insert */
            if (op == 2) b[m] = (uint8_t)('a' + rng_below(&r, 4));           /* substitute */
            else b[m] = a[i];                                                /* keep */
            m++;
        }
        s->len[2 * k] = n;
        s->len[2 * k + 1] = m;
    }
    return s;
}

static uint64_t dp_edit_distance_run(void *state) {
    const struct dp_edit_distance *s = (const struct dp_edit_distance *)state;
    uint64_t h = 0;
    for (size_t k = 0; k < ED_PAIRS; k++) {
        const uint8_t *a = s->str + 2 * k * ED_MAXLEN;
        const uint8_t *b = a + ED_MAXLEN;
        uint32_t d = edit_distance(a, s->len[2 * k], b, s->len[2 * k + 1], s->row0, s->row1);
        h = mix(h, d);
    }
    return h;
}

static void dp_edit_distance_teardown([[cx::escapes]] void *state) {
    struct dp_edit_distance *s = (struct dp_edit_distance *)state;
    bench_free(s->str);
    bench_free(s->row0);
    bench_free(s->row1);
    bench_free(s);
}

extern const struct bench bench_dp_edit_distance = {
    "dp_edit_distance", "alg", "Levenshtein distance (two-row DP) of 10 pairs of ~2000-char strings",
    dp_edit_distance_setup, dp_edit_distance_run, dp_edit_distance_teardown,
};

/* ---- dp_knapsack: 0/1 knapsack with a 1-D DP array ----------------------- */

enum { KS_ITEMS = 1000, KS_CAP = 100000, KS_WMIN = 20, KS_WMAX = 2000, KS_VMAX = 1000 };

struct dp_knapsack { [[cx::owned]] uint32_t *wt; [[cx::owned]] uint32_t *val; [[cx::owned]] uint32_t *best; };

static void *dp_knapsack_setup(void) {
    struct dp_knapsack *s = (struct dp_knapsack *)bench_alloc(sizeof *s);
    s->wt = (uint32_t *)bench_alloc(KS_ITEMS * sizeof(uint32_t));
    s->val = (uint32_t *)bench_alloc(KS_ITEMS * sizeof(uint32_t));
    s->best = (uint32_t *)bench_alloc((KS_CAP + 1) * sizeof(uint32_t));
    struct rng r = { 0x5eed0302u };
    for (size_t i = 0; i < KS_ITEMS; i++) {
        s->wt[i] = KS_WMIN + rng_below(&r, KS_WMAX - KS_WMIN + 1);
        s->val[i] = 1 + rng_below(&r, KS_VMAX);
    }
    return s;
}

/* best[c] = maximum value with total weight <= c; values stay below
 * KS_ITEMS * KS_VMAX. */
static void knapsack(const uint32_t *wt, const uint32_t *val, size_t items, uint32_t *best, size_t cap) {
    memset(best, 0, (cap + 1) * sizeof(uint32_t));
    for (size_t k = 0; k < items; k++) {
        size_t w = wt[k];
        uint32_t v = val[k];
        for (size_t c = cap; c >= w; c--) {
            uint32_t take = best[c - w] + v;
            if (take > best[c]) best[c] = take;
        }
    }
}

static uint64_t dp_knapsack_run(void *state) {
    const struct dp_knapsack *s = (const struct dp_knapsack *)state;
    knapsack(s->wt, s->val, KS_ITEMS, s->best, KS_CAP);
    uint64_t h = mix(0, s->best[KS_CAP]);
    for (size_t c = 0; c <= KS_CAP; c += 997) h = mix(h, s->best[c]);
    return h;
}

static void dp_knapsack_teardown([[cx::escapes]] void *state) {
    struct dp_knapsack *s = (struct dp_knapsack *)state;
    bench_free(s->wt);
    bench_free(s->val);
    bench_free(s->best);
    bench_free(s);
}

extern const struct bench bench_dp_knapsack = {
    "dp_knapsack", "alg", "0/1 knapsack, 1000 items, capacity 100000, 1-D DP array",
    dp_knapsack_setup, dp_knapsack_run, dp_knapsack_teardown,
};

/* ---- backtrack_nqueens: count N-queens solutions with bitmasks ------------ */

enum { NQ_MIN = 12, NQ_MAX = 13 };

/* Solutions for the remaining rows; cols/left/right are the attacked columns
 * of the next row, all within the mask `all`. */
static uint64_t nqueens(uint32_t all, uint32_t cols, uint32_t left, uint32_t right) {
    if (cols == all) return 1;
    uint64_t count = 0;
    uint32_t avail = all & ~(cols | left | right);
    while (avail != 0) {
        uint32_t bit = avail & ~(avail - 1u);   /* lowest set bit */
        avail ^= bit;
        count += nqueens(all, cols | bit, ((left | bit) << 1) & all, (right | bit) >> 1);
    }
    return count;
}

struct backtrack_nqueens { uint32_t nmin, nmax; };

static void *backtrack_nqueens_setup(void) {
    struct backtrack_nqueens *s = (struct backtrack_nqueens *)bench_alloc(sizeof *s);
    s->nmin = NQ_MIN;
    s->nmax = NQ_MAX;
    return s;
}

static uint64_t backtrack_nqueens_run(void *state) {
    const struct backtrack_nqueens *s = (const struct backtrack_nqueens *)state;
    uint64_t h = 0;
    for (uint32_t n = s->nmin; n <= s->nmax; n++) {
        uint32_t all = (1u << n) - 1u;
        h = mix(mix(h, n), nqueens(all, 0, 0, 0));
    }
    return h;
}

static void backtrack_nqueens_teardown([[cx::escapes]] void *state) {
    bench_free(state);
}

extern const struct bench bench_backtrack_nqueens = {
    "backtrack_nqueens", "alg", "count 12- and 13-queens solutions by bitmask backtracking",
    backtrack_nqueens_setup, backtrack_nqueens_run, backtrack_nqueens_teardown,
};
