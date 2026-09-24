/* Searching: binary search (lower_bound) and KMP substring search. */
#include "bench.cxh"

/* ---- search_binary: lower_bound queries on a sorted array ---------------- */

enum { BSEARCH_N = 1 << 20, BSEARCH_Q = 1 << 18 };

/* Index of the first element of a[0..n) that is >= key (n if none). */
static size_t lower_bound_u32(const uint32_t *a, size_t n, uint32_t key) {
    size_t lo = 0;
    while (n > 0) {
        size_t half = n / 2;
        if (a[lo + half] < key) {
            lo += half + 1;
            n -= half + 1;
        } else {
            n = half;
        }
    }
    return lo;
}

struct search_binary { uint32_t *a; uint32_t *q; };

static void *search_binary_setup(void) {
    struct search_binary *s = (struct search_binary *)bench_alloc(sizeof *s);
    s->a = (uint32_t *)bench_alloc(BSEARCH_N * sizeof(uint32_t));
    s->q = (uint32_t *)bench_alloc(BSEARCH_Q * sizeof(uint32_t));
    struct rng r = { 0x5eed0101u };
    uint32_t v = 0;
    for (size_t i = 0; i < BSEARCH_N; i++) {
        v += 1 + rng_below(&r, 8);      /* strictly increasing, max < 9.5M */
        s->a[i] = v;
    }
    uint32_t top = s->a[BSEARCH_N - 1] + 16;
    for (size_t i = 0; i < BSEARCH_Q; i++)
        s->q[i] = (rng_next(&r) & 1) ? s->a[rng_below(&r, BSEARCH_N)]   /* hit */
                                     : rng_below(&r, top);             /* mostly misses */
    return s;
}

static uint64_t search_binary_run(void *state) {
    const struct search_binary *s = (const struct search_binary *)state;
    uint64_t pos = 0, hits = 0, h = 0;
    for (size_t i = 0; i < BSEARCH_Q; i++) {
        uint32_t key = s->q[i];
        size_t k = lower_bound_u32(s->a, BSEARCH_N, key);
        pos += k;
        if (k < BSEARCH_N && s->a[k] == key) hits++;
        if ((i & 1023) == 0) h = mix(h, k);
    }
    return mix(mix(h, pos), hits);
}

static void search_binary_teardown([[cx::escapes]] void *state) {
    struct search_binary *s = (struct search_binary *)state;
    bench_free(s->a);
    bench_free(s->q);
    bench_free(s);
}

extern const struct bench bench_search_binary = {
    "search_binary", "alg", "256K lower_bound queries (hits and misses) on 1M sorted uint32",
    search_binary_setup, search_binary_run, search_binary_teardown,
};

/* ---- search_substring: Knuth-Morris-Pratt over a large text --------------- */

enum { KMP_TEXT = 1 << 20, KMP_PATS = 16, KMP_MINLEN = 6, KMP_MAXLEN = 24 };

/* fail[i] = length of the longest proper border of p[0..i]. */
static void kmp_build(const uint8_t *p, size_t m, size_t *fail) {
    size_t k = 0;
    fail[0] = 0;
    for (size_t i = 1; i < m; i++) {
        while (k > 0 && p[i] != p[k]) k = fail[k - 1];
        if (p[i] == p[k]) k++;
        fail[i] = k;
    }
}

struct kmp_result { uint64_t count; uint64_t pos_sum; };

/* Count (possibly overlapping) occurrences of p[0..m) in t[0..n). */
static struct kmp_result kmp_search(const uint8_t *t, size_t n, const uint8_t *p, size_t m,
                                    const size_t *fail) {
    struct kmp_result res = { 0, 0 };
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = t[i];
        while (k > 0 && p[k] != c) k = fail[k - 1];
        if (p[k] == c) k++;
        if (k == m) {
            res.count++;
            res.pos_sum += i + 1 - m;
            k = fail[k - 1];
        }
    }
    return res;
}

struct search_substring {
    uint8_t *text;
    uint8_t *pat;                   /* KMP_PATS patterns of KMP_MAXLEN bytes each */
    size_t plen[KMP_PATS];
};

static void *search_substring_setup(void) {
    struct search_substring *s = (struct search_substring *)bench_alloc(sizeof *s);
    s->text = (uint8_t *)bench_alloc(KMP_TEXT);
    s->pat = (uint8_t *)bench_alloc(KMP_PATS * KMP_MAXLEN);
    struct rng r = { 0x5eed0102u };
    /* 4-letter text with runs of a repeated motif, so partial matches and
     * KMP fallbacks are common */
    static const uint8_t alpha[4] = { 'A', 'C', 'G', 'T' };
    for (size_t i = 0; i < KMP_TEXT; i++) {
        if (i >= 8 && rng_below(&r, 4) == 0) s->text[i] = s->text[i - 3];
        else s->text[i] = alpha[rng_below(&r, 4)];
    }
    for (size_t k = 0; k < KMP_PATS; k++) {
        size_t m = KMP_MINLEN + rng_below(&r, KMP_MAXLEN - KMP_MINLEN + 1);
        uint8_t *p = s->pat + k * KMP_MAXLEN;
        if (k % 2 == 0) {
            /* taken from the text: at least one hit */
            size_t at = rng_below(&r, (uint32_t)(KMP_TEXT - m));
            for (size_t i = 0; i < m; i++) p[i] = s->text[at + i];
        } else {
            for (size_t i = 0; i < m; i++) p[i] = alpha[rng_below(&r, (k % 4 == 1) ? 2 : 4)];
        }
        s->plen[k] = m;
    }
    return s;
}

static uint64_t search_substring_run(void *state) {
    const struct search_substring *s = (const struct search_substring *)state;
    size_t fail[KMP_MAXLEN];
    uint64_t h = 0;
    for (size_t k = 0; k < KMP_PATS; k++) {
        const uint8_t *p = s->pat + k * KMP_MAXLEN;
        kmp_build(p, s->plen[k], fail);
        struct kmp_result res = kmp_search(s->text, KMP_TEXT, p, s->plen[k], fail);
        h = mix(mix(h, res.count), res.pos_sum);
    }
    return h;
}

static void search_substring_teardown([[cx::escapes]] void *state) {
    struct search_substring *s = (struct search_substring *)state;
    bench_free(s->text);
    bench_free(s->pat);
    bench_free(s);
}

extern const struct bench bench_search_substring = {
    "search_substring", "alg", "KMP search of 16 patterns in a 1 MB 4-letter text",
    search_substring_setup, search_substring_run, search_substring_teardown,
};
