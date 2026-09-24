/* Sorting: quicksort, mergesort, heapsort, radix sort, libc qsort, insertion
 * sort of small arrays, and a quicksort of string pointers. */
#include "bench.cxh"

#include <stdlib.h>
#include <string.h>

/* ---- shared helpers ------------------------------------------------------ */

/* n int32 values spread over the whole int32 range. */
static void gen_i32(int32_t *a, size_t n, uint64_t seed) {
    struct rng r = { seed };
    for (size_t i = 0; i < n; i++)
        a[i] = (int32_t)((int64_t)(rng_next(&r) >> 32) - INT64_C(0x80000000));
}

/* Checksum of a sorted int32 array: its length and every 16th element. */
static uint64_t sum_i32(const int32_t *a, size_t n) {
    uint64_t h = mix(0, n);
    for (size_t i = 0; i < n; i += 16) h = mix(h, (uint64_t)a[i]);
    return mix(h, (uint64_t)a[n - 1]);
}

static void i32_swap(int32_t *a, size_t i, size_t j) {
    int32_t t = a[i];
    a[i] = a[j];
    a[j] = t;
}

static void i32_insertion_sort(int32_t *a, size_t n) {
    for (size_t i = 1; i < n; i++) {
        int32_t v = a[i];
        size_t j = i;
        while (j > 0 && a[j - 1] > v) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = v;
    }
}

/* ---- sort_quick: quicksort, median-of-3, insertion sort below a cutoff ---- */

enum { QUICK_N = 800000, QUICK_CUTOFF = 16 };

static void i32_quick_sort(int32_t *a, size_t n) {
    while (n > QUICK_CUTOFF) {
        size_t mid = n / 2, last = n - 1;
        /* median of three: afterwards a[0] <= a[mid] <= a[last] */
        if (a[mid] < a[0]) i32_swap(a, mid, 0);
        if (a[last] < a[mid]) {
            i32_swap(a, last, mid);
            if (a[mid] < a[0]) i32_swap(a, mid, 0);
        }
        int32_t pivot = a[mid];
        /* Hoare partition; a[0] and a[last] stop the scans, so i and j stay in
         * [0, last] and j never steps below 0 */
        size_t i = 0, j = last;
        for (;;) {
            do i++; while (a[i] < pivot);
            do j--; while (pivot < a[j]);
            if (i >= j) break;
            i32_swap(a, i, j);
        }
        /* a[0..j] <= pivot <= a[j+1..n), both parts non-empty; recurse on the
         * smaller part, loop on the larger */
        size_t left = j + 1, right = n - left;
        if (left < right) {
            i32_quick_sort(a, left);
            a += left;
            n = right;
        } else {
            i32_quick_sort(a + left, right);
            n = left;
        }
    }
    i32_insertion_sort(a, n);
}

struct sort_quick { [[cx::owned]] int32_t *in; [[cx::owned]] int32_t *buf; };

static void *sort_quick_setup(void) {
    struct sort_quick *s = (struct sort_quick *)bench_alloc(sizeof *s);
    s->in = (int32_t *)bench_alloc(QUICK_N * sizeof(int32_t));
    s->buf = (int32_t *)bench_alloc(QUICK_N * sizeof(int32_t));
    gen_i32(s->in, QUICK_N, 0x5eed0001u);
    return s;
}

static uint64_t sort_quick_run(void *state) {
    const struct sort_quick *s = (const struct sort_quick *)state;
    memcpy(s->buf, s->in, QUICK_N * sizeof(int32_t));
    i32_quick_sort(s->buf, QUICK_N);
    return sum_i32(s->buf, QUICK_N);
}

static void sort_quick_teardown([[cx::escapes]] void *state) {
    struct sort_quick *s = (struct sort_quick *)state;
    bench_free(s->in);
    bench_free(s->buf);
    bench_free(s);
}

extern const struct bench bench_sort_quick = {
    "sort_quick", "alg", "quicksort (median-of-3, insertion sort below 16) of 800K int32",
    sort_quick_setup, sort_quick_run, sort_quick_teardown,
};

/* ---- sort_merge: bottom-up mergesort with a scratch buffer --------------- */

enum { MERGE_N = 1 << 20 };

/* Merge src[lo..mid) and src[mid..hi) into dst[lo..hi). Stable. */
static void i32_merge(int32_t *dst, const int32_t *src, size_t lo, size_t mid, size_t hi) {
    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (src[j] < src[i]) {
            dst[k] = src[j];
            j++;
        } else {
            dst[k] = src[i];
            i++;
        }
        k++;
    }
    while (i < mid) { dst[k] = src[i]; i++; k++; }
    while (j < hi) { dst[k] = src[j]; j++; k++; }
}

struct sort_merge { [[cx::owned]] int32_t *in; [[cx::owned]] int32_t *a; [[cx::owned]] int32_t *b; };

static void *sort_merge_setup(void) {
    struct sort_merge *s = (struct sort_merge *)bench_alloc(sizeof *s);
    s->in = (int32_t *)bench_alloc(MERGE_N * sizeof(int32_t));
    s->a = (int32_t *)bench_alloc(MERGE_N * sizeof(int32_t));
    s->b = (int32_t *)bench_alloc(MERGE_N * sizeof(int32_t));
    gen_i32(s->in, MERGE_N, 0x5eed0002u);
    return s;
}

static uint64_t sort_merge_run(void *state) {
    const struct sort_merge *s = (const struct sort_merge *)state;
    const size_t n = MERGE_N;
    int32_t *src = s->a, *dst = s->b;
    memcpy(src, s->in, n * sizeof(int32_t));
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n;
            size_t hi = mid + w < n ? mid + w : n;
            i32_merge(dst, src, lo, mid, hi);
        }
        int32_t *t = src;
        src = dst;
        dst = t;
    }
    return sum_i32(src, n);
}

static void sort_merge_teardown([[cx::escapes]] void *state) {
    struct sort_merge *s = (struct sort_merge *)state;
    bench_free(s->in);
    bench_free(s->a);
    bench_free(s->b);
    bench_free(s);
}

extern const struct bench bench_sort_merge = {
    "sort_merge", "alg", "bottom-up mergesort of 1M int32 with a scratch buffer",
    sort_merge_setup, sort_merge_run, sort_merge_teardown,
};

/* ---- sort_heap: heapsort ------------------------------------------------- */

enum { HEAP_N = 1 << 19 };

/* Restore the max-heap property below root in a[0..n). */
static void i32_sift_down(int32_t *a, size_t root, size_t n) {
    int32_t v = a[root];
    size_t i = root;
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && a[c] < a[c + 1]) c++;
        if (!(v < a[c])) break;
        a[i] = a[c];
        i = c;
    }
    a[i] = v;
}

static void i32_heap_sort(int32_t *a, size_t n) {
    for (size_t i = n / 2; i > 0; i--) i32_sift_down(a, i - 1, n);
    for (size_t end = n; end > 1; end--) {
        i32_swap(a, 0, end - 1);
        i32_sift_down(a, 0, end - 1);
    }
}

struct sort_heap { [[cx::owned]] int32_t *in; [[cx::owned]] int32_t *buf; };

static void *sort_heap_setup(void) {
    struct sort_heap *s = (struct sort_heap *)bench_alloc(sizeof *s);
    s->in = (int32_t *)bench_alloc(HEAP_N * sizeof(int32_t));
    s->buf = (int32_t *)bench_alloc(HEAP_N * sizeof(int32_t));
    gen_i32(s->in, HEAP_N, 0x5eed0003u);
    return s;
}

static uint64_t sort_heap_run(void *state) {
    const struct sort_heap *s = (const struct sort_heap *)state;
    memcpy(s->buf, s->in, HEAP_N * sizeof(int32_t));
    i32_heap_sort(s->buf, HEAP_N);
    return sum_i32(s->buf, HEAP_N);
}

static void sort_heap_teardown([[cx::escapes]] void *state) {
    struct sort_heap *s = (struct sort_heap *)state;
    bench_free(s->in);
    bench_free(s->buf);
    bench_free(s);
}

extern const struct bench bench_sort_heap = {
    "sort_heap", "alg", "heapsort of 512K int32",
    sort_heap_setup, sort_heap_run, sort_heap_teardown,
};

/* ---- sort_radix: LSD radix sort, 8-bit digits ---------------------------- */

enum { RADIX_N = 1 << 21 };

/* Sort a[0..n) using tmp[0..n) as scratch; four passes leave the result in a. */
static void u32_radix_sort(uint32_t *a, uint32_t *tmp, size_t n) {
    size_t count[4 * 256];
    memset(count, 0, sizeof count);
    for (size_t i = 0; i < n; i++) {
        uint32_t x = a[i];
        count[x & 255u]++;
        count[256 + ((x >> 8) & 255u)]++;
        count[512 + ((x >> 16) & 255u)]++;
        count[768 + (x >> 24)]++;
    }
    for (size_t p = 0; p < 4; p++) {
        size_t sum = 0;
        for (size_t d = 0; d < 256; d++) {
            size_t c = count[p * 256 + d];
            count[p * 256 + d] = sum;
            sum += c;
        }
    }
    uint32_t *src = a, *dst = tmp;
    for (size_t p = 0; p < 4; p++) {
        size_t *pos = count + p * 256;
        unsigned shift = (unsigned)(8 * p);
        for (size_t i = 0; i < n; i++) {
            uint32_t x = src[i];
            size_t d = (x >> shift) & 255u;
            dst[pos[d]] = x;
            pos[d]++;
        }
        uint32_t *t = src;
        src = dst;
        dst = t;
    }
}

struct sort_radix { [[cx::owned]] uint32_t *in; [[cx::owned]] uint32_t *a; [[cx::owned]] uint32_t *tmp; };

static void *sort_radix_setup(void) {
    struct sort_radix *s = (struct sort_radix *)bench_alloc(sizeof *s);
    s->in = (uint32_t *)bench_alloc(RADIX_N * sizeof(uint32_t));
    s->a = (uint32_t *)bench_alloc(RADIX_N * sizeof(uint32_t));
    s->tmp = (uint32_t *)bench_alloc(RADIX_N * sizeof(uint32_t));
    struct rng r = { 0x5eed0004u };
    for (size_t i = 0; i < RADIX_N; i++) s->in[i] = (uint32_t)(rng_next(&r) >> 32);
    return s;
}

static uint64_t sort_radix_run(void *state) {
    const struct sort_radix *s = (const struct sort_radix *)state;
    memcpy(s->a, s->in, RADIX_N * sizeof(uint32_t));
    u32_radix_sort(s->a, s->tmp, RADIX_N);
    uint64_t h = mix(0, RADIX_N);
    for (size_t i = 0; i < RADIX_N; i += 16) h = mix(h, s->a[i]);
    return mix(h, s->a[RADIX_N - 1]);
}

static void sort_radix_teardown([[cx::escapes]] void *state) {
    struct sort_radix *s = (struct sort_radix *)state;
    bench_free(s->in);
    bench_free(s->a);
    bench_free(s->tmp);
    bench_free(s);
}

extern const struct bench bench_sort_radix = {
    "sort_radix", "alg", "LSD radix sort (four 8-bit digit passes) of 2M uint32",
    sort_radix_setup, sort_radix_run, sort_radix_teardown,
};

/* ---- sort_qsort: libc qsort with a comparator function ------------------- */

enum { QSORT_N = 500000 };

static int i32_cmp(const void *pa, const void *pb) {
    int32_t a = *(const int32_t *)pa;
    int32_t b = *(const int32_t *)pb;
    return (a > b) - (a < b);
}

struct sort_qsort { [[cx::owned]] int32_t *in; [[cx::owned]] int32_t *buf; };

static void *sort_qsort_setup(void) {
    struct sort_qsort *s = (struct sort_qsort *)bench_alloc(sizeof *s);
    s->in = (int32_t *)bench_alloc(QSORT_N * sizeof(int32_t));
    s->buf = (int32_t *)bench_alloc(QSORT_N * sizeof(int32_t));
    gen_i32(s->in, QSORT_N, 0x5eed0005u);
    return s;
}

static uint64_t sort_qsort_run(void *state) {
    const struct sort_qsort *s = (const struct sort_qsort *)state;
    memcpy(s->buf, s->in, QSORT_N * sizeof(int32_t));
    qsort(s->buf, QSORT_N, sizeof(int32_t), i32_cmp);
    return sum_i32(s->buf, QSORT_N);
}

static void sort_qsort_teardown([[cx::escapes]] void *state) {
    struct sort_qsort *s = (struct sort_qsort *)state;
    bench_free(s->in);
    bench_free(s->buf);
    bench_free(s);
}

extern const struct bench bench_sort_qsort = {
    "sort_qsort", "alg", "libc qsort of 500K int32 through a comparator function",
    sort_qsort_setup, sort_qsort_run, sort_qsort_teardown,
};

/* ---- sort_small: insertion sort of many 8..32-element arrays -------------- */

enum { SMALL_TOTAL = 1 << 22, SMALL_MIN = 8, SMALL_MAX = 32 };

struct sort_small {
    [[cx::owned]] int32_t *in;
    [[cx::owned]] int32_t *buf;
    [[cx::owned]] uint8_t *len;     /* length of each array; they are stored back to back */
    size_t nseg;
};

static void *sort_small_setup(void) {
    struct sort_small *s = (struct sort_small *)bench_alloc(sizeof *s);
    s->in = (int32_t *)bench_alloc(SMALL_TOTAL * sizeof(int32_t));
    s->buf = (int32_t *)bench_alloc(SMALL_TOTAL * sizeof(int32_t));
    s->len = (uint8_t *)bench_alloc(SMALL_TOTAL / SMALL_MIN + 1);
    gen_i32(s->in, SMALL_TOTAL, 0x5eed0006u);
    struct rng r = { 0x5eed0007u };
    size_t used = 0, k = 0;
    while (used < SMALL_TOTAL) {
        size_t n = SMALL_MIN + rng_below(&r, SMALL_MAX - SMALL_MIN + 1);
        if (n > SMALL_TOTAL - used) n = SMALL_TOTAL - used;
        s->len[k] = (uint8_t)n;
        k++;
        used += n;
    }
    s->nseg = k;
    return s;
}

static uint64_t sort_small_run(void *state) {
    const struct sort_small *s = (const struct sort_small *)state;
    memcpy(s->buf, s->in, SMALL_TOTAL * sizeof(int32_t));
    uint64_t h = mix(0, s->nseg);
    int32_t *a = s->buf;
    for (size_t k = 0; k < s->nseg; k++) {
        size_t n = s->len[k];
        i32_insertion_sort(a, n);
        h = mix(h, (uint64_t)a[0]);
        h = mix(h, (uint64_t)a[n / 2]);
        h = mix(h, (uint64_t)a[n - 1]);
        a += n;
    }
    return h;
}

static void sort_small_teardown([[cx::escapes]] void *state) {
    struct sort_small *s = (struct sort_small *)state;
    bench_free(s->in);
    bench_free(s->buf);
    bench_free(s->len);
    bench_free(s);
}

extern const struct bench bench_sort_small = {
    "sort_small", "alg", "insertion sort of ~200K int32 arrays of 8..32 elements",
    sort_small_setup, sort_small_run, sort_small_teardown,
};

/* ---- sort_strings: quicksort of string pointers with strcmp -------------- */

enum { STR_N = 200000, STR_MINLEN = 6, STR_MAXLEN = 20, STR_ALPHA = 12, STR_CUTOFF = 12 };

static void str_swap(const char **a, size_t i, size_t j) {
    const char *t = a[i];
    a[i] = a[j];
    a[j] = t;
}

static void str_insertion_sort(const char **a, size_t n) {
    for (size_t i = 1; i < n; i++) {
        const char *v = a[i];
        size_t j = i;
        while (j > 0 && strcmp(a[j - 1], v) > 0) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = v;
    }
}

/* Same scheme as i32_quick_sort, comparing with strcmp. */
static void str_quick_sort(const char **a, size_t n) {
    while (n > STR_CUTOFF) {
        size_t mid = n / 2, last = n - 1;
        if (strcmp(a[mid], a[0]) < 0) str_swap(a, mid, 0);
        if (strcmp(a[last], a[mid]) < 0) {
            str_swap(a, last, mid);
            if (strcmp(a[mid], a[0]) < 0) str_swap(a, mid, 0);
        }
        const char *pivot = a[mid];
        size_t i = 0, j = last;
        for (;;) {
            do i++; while (strcmp(a[i], pivot) < 0);
            do j--; while (strcmp(pivot, a[j]) < 0);
            if (i >= j) break;
            str_swap(a, i, j);
        }
        size_t left = j + 1, right = n - left;
        if (left < right) {
            str_quick_sort(a, left);
            a += left;
            n = right;
        } else {
            str_quick_sort(a + left, right);
            n = left;
        }
    }
    str_insertion_sort(a, n);
}

/* in[] and buf[] own their pointer arrays; the pointers in them are shared
 * references into text. */
struct sort_strings {
    [[cx::owned]] char *text;           /* all strings, NUL-terminated, back to back */
    [[cx::owned]] const char **in;      /* generation order */
    [[cx::owned]] const char **buf;
};

static void *sort_strings_setup(void) {
    struct sort_strings *s = (struct sort_strings *)bench_alloc(sizeof *s);
    s->text = (char *)bench_alloc(STR_N * (STR_MAXLEN + 1));
    s->in = (const char **)bench_alloc(STR_N * sizeof(const char *));
    s->buf = (const char **)bench_alloc(STR_N * sizeof(const char *));
    struct rng r = { 0x5eed0008u };
    size_t off = 0;
    for (size_t i = 0; i < STR_N; i++) {
        size_t len = STR_MINLEN + rng_below(&r, STR_MAXLEN - STR_MINLEN + 1);
        s->in[i] = s->text + off;
        for (size_t k = 0; k < len; k++) s->text[off + k] = (char)('a' + (int)rng_below(&r, STR_ALPHA));
        s->text[off + len] = '\0';
        off += len + 1;
    }
    return s;
}

static uint64_t sort_strings_run(void *state) {
    const struct sort_strings *s = (const struct sort_strings *)state;
    memcpy(s->buf, s->in, STR_N * sizeof(const char *));
    str_quick_sort(s->buf, STR_N);
    /* hash string contents (not addresses) so equal strings in either order agree */
    uint64_t h = mix(0, STR_N);
    for (size_t i = 0; i < STR_N; i += 8) {
        const char *p = s->buf[i];
        uint64_t v = 0;
        for (size_t k = 0; k < 8 && p[k] != '\0'; k++) v = (v << 8) | (uint64_t)(uint8_t)p[k];
        h = mix(h, v);
    }
    return h;
}

static void sort_strings_teardown([[cx::escapes]] void *state) {
    struct sort_strings *s = (struct sort_strings *)state;
    bench_free(s->text);
    bench_free(s->in);
    bench_free(s->buf);
    bench_free(s);
}

extern const struct bench bench_sort_strings = {
    "sort_strings", "alg", "quicksort of 200K string pointers with strcmp",
    sort_strings_setup, sort_strings_run, sort_strings_teardown,
};
