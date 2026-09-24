/* String and byte operations: libc string functions, byte buffers accessed
 * through struct fields, and hand-written integer parsing and formatting. */
#include "bench.cxh"

#include <string.h>

/* ---- str_libc: strlen, strcmp, strchr and strstr ------------------------ */

enum {
    STR_COUNT = 1 << 14, STR_MINLEN = 8, STR_MAXLEN = 192, STR_BASE = STR_MAXLEN,
    STR_NEEDLE = 4, STR_PASSES = 48,
};

/* Strings are letters 'a'..'p'. Each starts with a random-length prefix of a
 * shared base string, so strcmp has to scan before it finds a difference. */
struct strs { char *pool; uint32_t *off; char *needle; unsigned char *ch; };

static void *strs_setup(void) {
    struct strs *s = (struct strs *)bench_alloc(sizeof *s);
    s->pool = (char *)bench_alloc(STR_COUNT * (STR_MAXLEN + 1));
    s->off = (uint32_t *)bench_alloc(STR_COUNT * sizeof(uint32_t));
    s->needle = (char *)bench_alloc(STR_COUNT * STR_NEEDLE);
    s->ch = (unsigned char *)bench_alloc(STR_COUNT);
    struct rng r = { 0x57a1b0u };
    char base[STR_BASE];
    for (size_t c = 0; c < STR_BASE; c++) base[c] = (char)('a' + rng_below(&r, 16));
    uint32_t o = 0;
    for (size_t i = 0; i < STR_COUNT; i++) {
        uint32_t len = STR_MINLEN + rng_below(&r, STR_MAXLEN - STR_MINLEN + 1);
        uint32_t pre = rng_below(&r, len + 1);
        char *str = s->pool + o;
        for (uint32_t c = 0; c < len; c++) str[c] = (char)(c < pre ? (uint32_t)base[c] : 'a' + rng_below(&r, 16));
        str[len] = '\0';
        s->off[i] = o;
        o += len + 1;
        /* half the needles occur in their string, half are random */
        char *nd = s->needle + i * STR_NEEDLE;
        if (rng_below(&r, 2)) {
            uint32_t at = rng_below(&r, len - (STR_NEEDLE - 1) + 1);
            for (size_t c = 0; c < STR_NEEDLE - 1; c++) nd[c] = str[at + c];
        } else {
            for (size_t c = 0; c < STR_NEEDLE - 1; c++) nd[c] = (char)('a' + rng_below(&r, 16));
        }
        nd[STR_NEEDLE - 1] = '\0';
        /* half the search characters never occur ('q'..'z') */
        s->ch[i] = (unsigned char)(rng_below(&r, 2) ? 'a' + rng_below(&r, 16) : 'q' + rng_below(&r, 10));
    }
    return s;
}

static uint64_t strs_run(void *state) {
    const struct strs *s = (const struct strs *)state;
    uint64_t h = 0;
    for (size_t pass = 0; pass < STR_PASSES; pass++) {
        uint64_t total = 0, cmp = 0, chr = 0, sub = 0;
        for (size_t i = 0; i < STR_COUNT; i++) {
            const char *str = s->pool + s->off[i];
            const char *other = s->pool + s->off[(i + 1 + pass) & (STR_COUNT - 1)];
            total += strlen(str);
            int c = strcmp(str, other);
            cmp = mix(cmp, c < 0 ? 1u : c > 0 ? 2u : 3u);
            const char *f = strchr(str, s->ch[(i + pass) & (STR_COUNT - 1)]);
            chr += f ? (uint64_t)(f - str) : 1000u;
            const char *g = strstr(str, s->needle + i * STR_NEEDLE);
            sub += g ? (uint64_t)(g - str) : 1000u;
        }
        h = mix(mix(mix(mix(h, total), cmp), chr), sub);
    }
    return h;
}

static void strs_teardown([[cx::escapes]] void *state) {
    struct strs *s = (struct strs *)state;
    bench_free(s->pool);
    bench_free(s->off);
    bench_free(s->needle);
    bench_free(s->ch);
    bench_free(s);
}

extern const struct bench bench_str_libc = {
    "str_libc", "ops", "strlen, strcmp, strchr and strstr over 16K generated strings",
    strs_setup, strs_run, strs_teardown,
};

/* ---- byte_buffer: unsigned char buffers accessed through struct fields ---
 * In C a store through `unsigned char *` may modify any object, including
 * the `data` and `len` fields it was reached through, so they are reloaded
 * after every byte store. */

enum { BB_LEN = 1 << 16, BB_PASSES = 150 };

struct bytebuf { [[cx::owned]] unsigned char *data; size_t len; };
struct bytewriter { [[cx::owned]] unsigned char *data; size_t len; size_t cap; };

struct bbstate {
    struct bytebuf a; struct bytebuf b; struct bytewriter w;
    [[cx::owned]] unsigned char *tab;   /* 256-entry substitution table */
    [[cx::owned]] unsigned char *key;   /* 16-byte key */
};

static void bb_fill(struct bytebuf *b, size_t seed) {
    for (size_t i = 0; i < b->len; i++) b->data[i] = (unsigned char)((i * 7 + seed) & 0xff);
}

/* dst->len <= src->len */
static void bb_map(struct bytebuf *dst, const struct bytebuf *src, const unsigned char *tab) {
    for (size_t i = 0; i < dst->len; i++) dst->data[i] = tab[src->data[i]];
}

static void bb_xor(struct bytebuf *b, const unsigned char *key) {
    for (size_t i = 0; i < b->len; i++) b->data[i] = (unsigned char)(b->data[i] ^ key[i & 15]);
}

/* append each byte, and a second byte for multiples of 4 */
static void bb_emit(struct bytewriter *w, const struct bytebuf *src) {
    for (size_t i = 0; i < src->len; i++) {
        unsigned char c = src->data[i];
        if (w->len < w->cap) {
            w->data[w->len] = c;
            w->len += 1;
        }
        if ((c & 3) == 0 && w->len < w->cap) {
            w->data[w->len] = (unsigned char)(c >> 2);
            w->len += 1;
        }
    }
}

static uint64_t bb_sum(const struct bytewriter *w) {
    uint64_t a = 0, b = 0;
    for (size_t i = 0; i < w->len; i++) {
        a += (uint64_t)w->data[i];
        b += a;
    }
    return mix(mix(0, a), b);
}

static void *bb_setup(void) {
    struct bbstate *s = (struct bbstate *)bench_alloc(sizeof *s);
    s->a.data = (unsigned char *)bench_alloc(BB_LEN);
    s->a.len = BB_LEN;
    s->b.data = (unsigned char *)bench_alloc(BB_LEN);
    s->b.len = BB_LEN;
    s->w.data = (unsigned char *)bench_alloc(2 * BB_LEN);
    s->w.len = 0;
    s->w.cap = 2 * BB_LEN;
    s->tab = (unsigned char *)bench_alloc(256);
    s->key = (unsigned char *)bench_alloc(16);
    struct rng r = { 0xb17eb0ffu };
    /* a random permutation of the byte values */
    for (size_t i = 0; i < 256; i++) s->tab[i] = (unsigned char)i;
    for (size_t i = 255; i > 0; i--) {
        size_t j = rng_below(&r, (uint32_t)i + 1);
        unsigned char t = s->tab[i];
        s->tab[i] = s->tab[j];
        s->tab[j] = t;
    }
    for (size_t i = 0; i < 16; i++) s->key[i] = (unsigned char)rng_below(&r, 256);
    return s;
}

static uint64_t bb_run(void *state) {
    struct bbstate *s = (struct bbstate *)state;
    uint64_t h = 0;
    for (size_t pass = 0; pass < BB_PASSES; pass++) {
        bb_fill(&s->a, pass);
        bb_map(&s->b, &s->a, s->tab);
        bb_xor(&s->b, s->key);
        s->w.len = 0;
        bb_emit(&s->w, &s->b);
        h = mix(mix(h, s->w.len), bb_sum(&s->w));
    }
    return h;
}

static void bb_teardown([[cx::escapes]] void *state) {
    struct bbstate *s = (struct bbstate *)state;
    bench_free(s->a.data);
    bench_free(s->b.data);
    bench_free(s->w.data);
    bench_free(s->tab);
    bench_free(s->key);
    bench_free(s);
}

extern const struct bench bench_byte_buffer = {
    "byte_buffer", "ops", "fill/map/xor/append on unsigned char buffers reached through struct fields",
    bb_setup, bb_run, bb_teardown,
};

/* ---- int_parse_format: decimal text <-> int64 by hand -------------------- */

enum { IPF_COUNT = 1 << 16, IPF_MAXCHARS = 16, IPF_PASSES = 20 };

struct ipf {
    unsigned char *text; size_t text_len;
    int64_t *vals; unsigned char *out;
};

/* Writes v in decimal (at most 20 bytes); returns the number of bytes. */
static size_t ipf_format(unsigned char *dst, int64_t v) {
    unsigned char tmp[24];
    size_t n = 0, k = 0;
    uint64_t u;
    if (v < 0) {
        dst[k] = '-';
        k++;
        u = (uint64_t)(-v);            /* |v| < 10^13, so -v cannot overflow */
    } else {
        u = (uint64_t)v;
    }
    do {
        tmp[n] = (unsigned char)('0' + u % 10);
        n++;
        u /= 10;
    } while (u);
    while (n) {
        n--;
        dst[k] = tmp[n];
        k++;
    }
    return k;
}

static void *ipf_setup(void) {
    struct ipf *s = (struct ipf *)bench_alloc(sizeof *s);
    s->text = (unsigned char *)bench_alloc(IPF_COUNT * IPF_MAXCHARS);
    s->vals = (int64_t *)bench_alloc(IPF_COUNT * sizeof(int64_t));
    s->out = (unsigned char *)bench_alloc(IPF_COUNT * IPF_MAXCHARS);
    struct rng r = { 0x1a7f0a7u };
    size_t pos = 0;
    for (size_t k = 0; k < IPF_COUNT; k++) {
        /* 1 to 12 digits, uniformly, then a sign */
        uint64_t lim = 10;
        for (uint32_t d = rng_below(&r, 12); d > 0; d--) lim *= 10;
        int64_t v = (int64_t)(rng_next(&r) % lim);
        if (rng_below(&r, 2)) v = -v;
        pos += ipf_format(s->text + pos, v);
        s->text[pos] = (unsigned char)((k & 7) == 7 ? '\n' : ' ');
        pos++;
    }
    s->text_len = pos;
    return s;
}

static uint64_t ipf_run(void *state) {
    const struct ipf *s = (const struct ipf *)state;
    const unsigned char *t = s->text;
    size_t n = s->text_len;
    uint64_t h = 0;
    for (size_t pass = 0; pass < IPF_PASSES; pass++) {
        /* parse */
        size_t i = 0, count = 0;
        int64_t sum = 0;
        while (i < n) {
            if (t[i] == ' ' || t[i] == '\n') { i++; continue; }
            int neg = 0;
            if (t[i] == '-') { neg = 1; i++; }
            int64_t v = 0;
            while (i < n && t[i] >= '0' && t[i] <= '9') {
                v = v * 10 + (int64_t)(t[i] - '0');
                i++;
            }
            if (neg) v = -v;
            s->vals[count] = v;
            count++;
            sum += v;
        }
        /* format the values, shifted by the pass number */
        size_t o = 0;
        for (size_t k = 0; k < count; k++) {
            o += ipf_format(s->out + o, s->vals[k] + (int64_t)pass);
            s->out[o] = (unsigned char)((k & 7) == 7 ? '\n' : ' ');
            o++;
        }
        h = mix(mix(mix(h, (uint64_t)sum), count), o);
        h = mix(h, (uint64_t)s->out[o / 2] | ((uint64_t)s->out[o - 2] << 8));
    }
    return h;
}

static void ipf_teardown([[cx::escapes]] void *state) {
    struct ipf *s = (struct ipf *)state;
    bench_free(s->text);
    bench_free(s->vals);
    bench_free(s->out);
    bench_free(s);
}

extern const struct bench bench_int_parse_format = {
    "int_parse_format", "ops", "parse 64K decimal integers from text and format them back, by hand",
    ipf_setup, ipf_run, ipf_teardown,
};
