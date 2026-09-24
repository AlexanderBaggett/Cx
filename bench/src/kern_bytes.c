/* Byte-stream kernels: LEB128 varints, validating UTF-8 decoding, MurmurHash3. */
#include "bench.cxh"
#include "kern_util.cxh"

#include <string.h>

/* ---- varint_codec: LEB128 encode + decode through a buffer struct -------- */

enum {
    VI_COUNT = 5000000,
    VI_MAXLEN = 10,       /* bytes in the longest encoding of a uint64_t */
};

/* A byte buffer with a write end and a read cursor. The encoder and decoder
 * keep all of their state in these fields and update it through the struct
 * pointer after every byte, as stream code does. In C the byte stores may
 * alias the fields; in Cx they cannot (spec §5.3, §5.4). */
struct vbuf {
    [[cx::owned]] uint8_t *data;
    size_t cap;           /* allocated bytes */
    size_t len;           /* bytes written */
    size_t pos;           /* read cursor, <= len */
    uint32_t err;         /* 1: buffer full; 2: truncated or over-long input */
};

struct varint {
    [[cx::owned]] uint64_t *vals;    /* input values */
    [[cx::owned]] uint64_t *back;    /* decoded values */
    size_t n;
    struct vbuf buf;
};

/* Appends v as unsigned LEB128: 7 bits per byte, low bits first, bit 7 set
 * on every byte but the last. */
static void vbuf_put(struct vbuf *b, uint64_t v) {
    if (b->cap - b->len < VI_MAXLEN) {
        b->err |= 1u;
        return;
    }
    while (v >= 0x80u) {
        b->data[b->len] = (uint8_t)((v & 0x7fu) | 0x80u);
        b->len++;
        v >>= 7;
    }
    b->data[b->len] = (uint8_t)v;
    b->len++;
}

/* Reads one value at the cursor. Truncated input, or an encoding that does
 * not fit in 64 bits, sets err and yields 0. */
static uint64_t vbuf_get(struct vbuf *b) {
    uint64_t v = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (b->pos >= b->len) break;
        uint64_t byte = (uint64_t)b->data[b->pos];
        b->pos++;
        if (shift == 63 && byte > 1u) break;       /* bits beyond bit 63 */
        v |= (byte & 0x7fu) << shift;
        if (byte < 0x80u) return v;
    }
    b->err |= 2u;
    return 0;
}

static void varint_encode(struct vbuf *b, const uint64_t *vals, size_t n) {
    b->len = 0;
    for (size_t i = 0; i < n; i++) vbuf_put(b, vals[i]);
}

/* Decodes up to n values into out; returns how many were decoded. */
static size_t varint_decode(struct vbuf *b, uint64_t *out, size_t n) {
    b->pos = 0;
    size_t k = 0;
    while (k < n && b->pos < b->len) {
        out[k] = vbuf_get(b);
        k++;
    }
    return k;
}

static size_t varint_size(uint64_t v) {
    size_t len = 1;
    for (; v >= 0x80u; v >>= 7) len++;
    return len;
}

static void *varint_setup(void) {
    struct varint *s = (struct varint *)bench_alloc(sizeof *s);
    s->n = VI_COUNT;
    s->vals = (uint64_t *)bench_alloc(s->n * sizeof(uint64_t));
    s->back = (uint64_t *)bench_alloc(s->n * sizeof(uint64_t));
    /* Mixed magnitudes: mostly small values (1-2 bytes), some up to the full
     * 64 bits (10 bytes). */
    static const unsigned bits[16] = { 1, 4, 7, 7, 7, 7, 7, 10, 14, 14, 21, 21, 28, 35, 49, 64 };
    struct rng r = { 0x9e3779b97f4a7c15u };
    size_t total = 0;
    for (size_t i = 0; i < s->n; i++) {
        unsigned w = bits[rng_below(&r, 16)];
        uint64_t v = rng_next(&r) >> (64u - w);
        s->vals[i] = v;
        total += varint_size(v);
    }
    /* exactly enough room: the capacity check never fails */
    s->buf.cap = total + VI_MAXLEN;
    s->buf.data = (uint8_t *)bench_alloc(s->buf.cap);
    s->buf.len = s->buf.pos = 0;
    s->buf.err = 0;
    varint_encode(&s->buf, s->vals, s->n);
    size_t k = varint_decode(&s->buf, s->back, s->n);
    if (s->buf.len != total || k != s->n || s->buf.pos != total || s->buf.err != 0 ||
        memcmp(s->back, s->vals, s->n * sizeof(uint64_t)) != 0)
        kern_fail("varint_codec: round trip mismatch");
    /* The decoder rejects truncation and encodings beyond 64 bits. */
    static const uint8_t bad_trunc[2] = { 0x80u, 0x80u };
    static const uint8_t bad_long[10] = { 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0x02u };
    static const uint8_t max_ok[10] = { 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0xffu, 0x01u };
    struct vbuf t = { (uint8_t *)bench_alloc(VI_MAXLEN), VI_MAXLEN, 0, 0, 0 };
    memcpy(t.data, bad_trunc, sizeof bad_trunc);
    t.len = sizeof bad_trunc;
    uint64_t v0 = vbuf_get(&t);
    uint32_t e0 = t.err;
    memcpy(t.data, bad_long, sizeof bad_long);
    t.len = sizeof bad_long;
    t.pos = 0;
    t.err = 0;
    uint64_t v1 = vbuf_get(&t);
    uint32_t e1 = t.err;
    memcpy(t.data, max_ok, sizeof max_ok);
    t.pos = 0;
    t.err = 0;
    uint64_t v2 = vbuf_get(&t);
    if (v0 != 0 || e0 != 2u || v1 != 0 || e1 != 2u || v2 != ~(uint64_t)0 || t.err != 0 || t.pos != 10)
        kern_fail("varint_codec: malformed-input handling");
    bench_free(t.data);
    return s;
}

/* Checksum: lengths, error flags, and every 64th encoded byte and decoded value. */
static uint64_t varint_run(void *state) {
    struct varint *s = (struct varint *)state;
    struct vbuf *b = &s->buf;
    b->err = 0;
    varint_encode(b, s->vals, s->n);
    size_t k = varint_decode(b, s->back, s->n);
    uint64_t h = mix(mix(mix(mix(0, b->len), b->pos), k), b->err);
    for (size_t i = 0; i < b->len; i += 64) h = mix(h, b->data[i]);
    for (size_t i = 0; i < k; i += 64) h = mix(h, s->back[i]);
    return h;
}

static void varint_teardown([[cx::escapes]] void *state) {
    struct varint *s = (struct varint *)state;
    bench_free(s->vals);
    bench_free(s->back);
    bench_free(s->buf.data);
    bench_free(s);
}

extern const struct bench bench_varint_codec = {
    "varint_codec", "kern", "LEB128 encode + decode of 5M mixed-size uint64 values, cursors in a struct",
    varint_setup, varint_run, varint_teardown,
};

/* ---- utf8_decode: validating UTF-8 to code point decoding ---------------- */

enum {
    U8_BYTES = 16 << 20,
    U8_BAD_EVERY = 4096,   /* one invalid sequence per this many code points, on average */
    U8_REPLACEMENT = 0xfffd,
};

struct utf8 {
    [[cx::owned]] uint8_t *text;
    [[cx::owned]] uint32_t *cps;   /* decoded code points (up to one per byte) */
    size_t len;                    /* bytes of text */
};

/* Decodes one sequence from p[0..avail), avail >= 1, into *cp. Returns its
 * length, or 0 if it is invalid: a continuation or invalid lead byte, a
 * missing continuation byte, truncation at the end of the input, an overlong
 * form, a surrogate, or a value above U+10FFFF. */
static size_t utf8_next(const uint8_t *p, size_t avail, uint32_t *cp) {
    uint32_t b0 = p[0];
    if (b0 < 0x80u) {
        *cp = b0;
        return 1;
    }
    if (b0 < 0xc2u || b0 > 0xf4u) return 0;
    if (b0 < 0xe0u) {
        if (avail < 2) return 0;
        uint32_t b1 = p[1];
        if ((b1 & 0xc0u) != 0x80u) return 0;
        *cp = (b0 & 0x1fu) << 6 | (b1 & 0x3fu);
        return 2;
    }
    if (b0 < 0xf0u) {
        if (avail < 3) return 0;
        uint32_t b1 = p[1], b2 = p[2];
        if ((b1 & 0xc0u) != 0x80u || (b2 & 0xc0u) != 0x80u) return 0;
        uint32_t c = (b0 & 0x0fu) << 12 | (b1 & 0x3fu) << 6 | (b2 & 0x3fu);
        if (c < 0x800u || (c >= 0xd800u && c <= 0xdfffu)) return 0;
        *cp = c;
        return 3;
    }
    if (avail < 4) return 0;
    uint32_t b1 = p[1], b2 = p[2], b3 = p[3];
    if ((b1 & 0xc0u) != 0x80u || (b2 & 0xc0u) != 0x80u || (b3 & 0xc0u) != 0x80u) return 0;
    uint32_t c = (b0 & 0x07u) << 18 | (b1 & 0x3fu) << 12 | (b2 & 0x3fu) << 6 | (b3 & 0x3fu);
    if (c < 0x10000u || c > 0x10ffffu) return 0;
    *cp = c;
    return 4;
}

struct utf8_stats { size_t count; size_t errors; uint64_t sum; };

/* Decodes text[0..n) into out (room for n entries). Each invalid byte becomes
 * one U+FFFD and decoding resumes at the next byte. */
static struct utf8_stats utf8_decode(const uint8_t *text, size_t n, uint32_t *out) {
    struct utf8_stats st = { 0, 0, 0 };
    size_t i = 0;
    while (i < n) {
        uint32_t cp = 0;
        size_t len = utf8_next(text + i, n - i, &cp);
        if (len == 0) {
            cp = U8_REPLACEMENT;
            len = 1;
            st.errors++;
        }
        out[st.count] = cp;
        st.count++;
        st.sum += cp;
        i += len;
    }
    return st;
}

/* Encodes a valid scalar value c; returns the byte count. */
static size_t utf8_put(uint8_t *p, uint32_t c) {
    if (c < 0x80u) {
        p[0] = (uint8_t)c;
        return 1;
    }
    if (c < 0x800u) {
        p[0] = (uint8_t)(0xc0u | (c >> 6));
        p[1] = (uint8_t)(0x80u | (c & 0x3fu));
        return 2;
    }
    if (c < 0x10000u) {
        p[0] = (uint8_t)(0xe0u | (c >> 12));
        p[1] = (uint8_t)(0x80u | ((c >> 6) & 0x3fu));
        p[2] = (uint8_t)(0x80u | (c & 0x3fu));
        return 3;
    }
    p[0] = (uint8_t)(0xf0u | (c >> 18));
    p[1] = (uint8_t)(0x80u | ((c >> 12) & 0x3fu));
    p[2] = (uint8_t)(0x80u | ((c >> 6) & 0x3fu));
    p[3] = (uint8_t)(0x80u | (c & 0x3fu));
    return 4;
}

/* A random scalar value whose encoding has cls + 1 bytes. */
static uint32_t utf8_random_cp(struct rng *r, uint32_t cls) {
    if (cls == 0) return rng_below(r, 0x80);
    if (cls == 1) return 0x80u + rng_below(r, 0x800 - 0x80);
    if (cls == 2) {
        uint32_t c = 0x800u + rng_below(r, 0x10000 - 0x800 - 0x800);
        return c < 0xd800u ? c : c + 0x800u;                /* skip the surrogates */
    }
    return 0x10000u + rng_below(r, 0x110000 - 0x10000);
}

/* Writes one invalid sequence of 1-4 bytes. Every byte of it is rejected
 * on its own (an invalid lead, or a continuation byte without a lead), so it
 * decodes to one U+FFFD per byte provided a lead or ASCII byte follows. */
static size_t utf8_put_invalid(uint8_t *p, struct rng *r) {
    uint8_t c0 = (uint8_t)(0x80u + rng_below(r, 64));
    uint8_t c1 = (uint8_t)(0x80u + rng_below(r, 64));
    switch (rng_below(r, 9)) {
    case 0:                                               /* stray continuation */
        p[0] = c0;
        return 1;
    case 1:                                               /* overlong 2-byte */
        p[0] = (uint8_t)(0xc0u + rng_below(r, 2));
        p[1] = c0;
        return 2;
    case 2:                                               /* overlong 3-byte */
        p[0] = 0xe0u;
        p[1] = (uint8_t)(0x80u + rng_below(r, 32));
        p[2] = c0;
        return 3;
    case 3:                                               /* surrogate */
        p[0] = 0xedu;
        p[1] = (uint8_t)(0xa0u + rng_below(r, 32));
        p[2] = c0;
        return 3;
    case 4:                                               /* above U+10FFFF */
        p[0] = 0xf4u;
        p[1] = (uint8_t)(0x90u + rng_below(r, 48));
        p[2] = c0;
        p[3] = c1;
        return 4;
    case 5:                                               /* never a valid lead */
        p[0] = (uint8_t)(0xf5u + rng_below(r, 11));
        return 1;
    case 6:                                               /* 3-byte, one continuation missing */
        p[0] = (uint8_t)(0xe1u + rng_below(r, 12));
        p[1] = c0;
        return 2;
    case 7:                                               /* 4-byte, one continuation missing */
        p[0] = (uint8_t)(0xf1u + rng_below(r, 3));
        p[1] = c0;
        p[2] = c1;
        return 3;
    default:                                              /* overlong 4-byte */
        p[0] = 0xf0u;
        p[1] = (uint8_t)(0x80u + rng_below(r, 16));
        p[2] = c0;
        p[3] = c1;
        return 4;
    }
}

/* Text in runs of one sequence length (like words of one script), with an
 * occasional invalid sequence. want receives the code points a correct
 * decoder produces and *nbad the number of invalid bytes; returns the number
 * of code points. */
static size_t utf8_gen(uint8_t *text, size_t n, uint32_t *want, size_t *nbad, uint64_t seed) {
    static const uint32_t cls_of[10] = { 0, 0, 0, 0, 1, 1, 2, 2, 2, 3 };
    struct rng r = { seed };
    size_t pos = 0, k = 0;
    while (n - pos >= 8) {
        uint32_t cls = cls_of[rng_below(&r, 10)];
        uint32_t run = 1 + rng_below(&r, 12);
        for (uint32_t j = 0; j < run && n - pos >= 8; j++) {
            if (rng_below(&r, U8_BAD_EVERY) == 0) {
                size_t bad = utf8_put_invalid(text + pos, &r);
                for (size_t q = 0; q < bad; q++) want[k++] = U8_REPLACEMENT;
                pos += bad;
                *nbad += bad;
            }
            uint32_t c = utf8_random_cp(&r, cls);           /* a lead or ASCII byte follows */
            pos += utf8_put(text + pos, c);
            want[k++] = c;
        }
    }
    while (pos < n) {
        text[pos++] = (uint8_t)'.';
        want[k++] = '.';
    }
    return k;
}

static void *utf8_setup(void) {
    struct utf8 *s = (struct utf8 *)bench_alloc(sizeof *s);
    s->len = U8_BYTES;
    s->text = (uint8_t *)bench_alloc(s->len);
    s->cps = (uint32_t *)bench_alloc(s->len * sizeof(uint32_t));
    uint32_t *want = (uint32_t *)bench_alloc(s->len * sizeof(uint32_t));
    size_t nbad = 0;
    size_t nwant = utf8_gen(s->text, s->len, want, &nbad, 0xbb67ae8584caa73bu);
    uint64_t sum = 0;
    for (size_t i = 0; i < nwant; i++) sum += want[i];
    struct utf8_stats st = utf8_decode(s->text, s->len, s->cps);
    if (st.count != nwant || st.sum != sum || st.errors != nbad || nbad == 0 ||
        memcmp(s->cps, want, nwant * sizeof(uint32_t)) != 0)
        kern_fail("utf8_decode: decoded code points differ from the generated ones");
    bench_free(want);
    return s;
}

/* Checksum: count, sum and errors, and every 64th code point. */
static uint64_t utf8_run(void *state) {
    const struct utf8 *s = (const struct utf8 *)state;
    struct utf8_stats st = utf8_decode(s->text, s->len, s->cps);
    uint64_t h = mix(mix(mix(0, st.count), st.errors), st.sum);
    for (size_t i = 0; i < st.count; i += 64) h = mix(h, s->cps[i]);
    return h;
}

static void utf8_teardown([[cx::escapes]] void *state) {
    struct utf8 *s = (struct utf8 *)state;
    bench_free(s->text);
    bench_free(s->cps);
    bench_free(s);
}

extern const struct bench bench_utf8_decode = {
    "utf8_decode", "kern", "validating UTF-8 decode of 16 MB of mixed 1-4 byte text to code points",
    utf8_setup, utf8_run, utf8_teardown,
};

/* ---- hash_murmur3: MurmurHash3 x86_32 over many short keys --------------- */

enum { MM_KEYS = 1 << 20, MM_MINLEN = 4, MM_MAXLEN = 64, MM_SEEDS = 2 };

/* MurmurHash3 relies on 32-bit wraparound, which is a violation in Cx: every
 * 32-bit product and sum is formed in 64 bits and masked. */
static uint32_t mul32(uint32_t a, uint32_t b) { return (uint32_t)(((uint64_t)a * b) & 0xffffffffu); }

/* 0 < n < 32; the left shift discards the rotated-out bits. */
static uint32_t rotl32(uint32_t x, unsigned n) { return (x << n) | (x >> (32u - n)); }

static uint32_t murmur3_32(const uint8_t *p, size_t n, uint32_t seed) {
    const uint32_t c1 = 0xcc9e2d51u, c2 = 0x1b873593u;
    uint32_t h = seed;
    size_t nblocks = n / 4;
    for (size_t i = 0; i < nblocks; i++) {
        const uint8_t *q = p + 4 * i;
        uint32_t k = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
        h ^= mul32(rotl32(mul32(k, c1), 15), c2);
        h = rotl32(h, 13);
        h = (uint32_t)(((uint64_t)h * 5u + 0xe6546b64u) & 0xffffffffu);
    }
    const uint8_t *tail = p + 4 * nblocks;
    size_t rem = n & 3u;
    uint32_t k = 0;
    if (rem >= 3) k ^= (uint32_t)tail[2] << 16;
    if (rem >= 2) k ^= (uint32_t)tail[1] << 8;
    if (rem >= 1) {
        k ^= (uint32_t)tail[0];
        h ^= mul32(rotl32(mul32(k, c1), 15), c2);
    }
    h ^= (uint32_t)(n & 0xffffffffu);
    h ^= h >> 16;
    h = mul32(h, 0x85ebca6bu);
    h ^= h >> 13;
    h = mul32(h, 0xc2b2ae35u);
    h ^= h >> 16;
    return h;
}

/* Published MurmurHash3_x86_32 test vectors. */
struct mm_vector { const char *text; size_t len; uint32_t seed, hash; };
static const struct mm_vector mm_vectors[] = {
    { "", 0, 0, 0 },
    { "", 0, 1, 0x514e28b7u },
    { "", 0, 0xffffffffu, 0x81f16f39u },
    { "\0\0\0\0", 4, 0, 0x2362f9deu },
    { "hello", 5, 0, 0x248bfa47u },
    { "a", 1, 0x9747b28cu, 0x7fa09ea6u },
    { "ab", 2, 0x9747b28cu, 0x74875592u },
    { "abc", 3, 0x9747b28cu, 0xc84a62ddu },
    { "abcd", 4, 0x9747b28cu, 0xf0478627u },
    { "Hello, world!", 13, 0x9747b28cu, 0x24884cbau },
    { "The quick brown fox jumps over the lazy dog", 43, 0x9747b28cu, 0x2fa826cdu },
    { "The quick brown fox jumps over the lazy dog", 43, 0, 0x2e4ff723u },
};

struct murmur {
    [[cx::owned]] uint8_t *keys;   /* all keys, back to back */
    [[cx::owned]] uint8_t *len;    /* length of each key, MM_MINLEN..MM_MAXLEN */
    size_t nkeys, nseeds;
    uint32_t seeds[MM_SEEDS];
};

static void *murmur_setup(void) {
    for (size_t i = 0; i < sizeof mm_vectors / sizeof mm_vectors[0]; i++) {
        const struct mm_vector *v = &mm_vectors[i];
        uint8_t buf[64];
        memcpy(buf, v->text, v->len);                     /* char -> byte representation */
        if (murmur3_32(buf, v->len, v->seed) != v->hash) kern_fail("hash_murmur3: test vector mismatch");
    }
    struct murmur *s = (struct murmur *)bench_alloc(sizeof *s);
    s->nkeys = MM_KEYS;
    s->nseeds = MM_SEEDS;
    s->seeds[0] = 0;
    s->seeds[1] = 0x9747b28cu;
    s->len = (uint8_t *)bench_alloc(s->nkeys);
    struct rng r = { 0xcbbb9d5dc1059ed8u };
    size_t total = 0;
    for (size_t i = 0; i < s->nkeys; i++) {
        s->len[i] = (uint8_t)(MM_MINLEN + rng_below(&r, MM_MAXLEN - MM_MINLEN + 1));
        total += s->len[i];
    }
    s->keys = (uint8_t *)bench_alloc(total);
    kern_fill_random(s->keys, total, 0x629a292a367cd507u);
    return s;
}

/* Checksum: xor and sum of all hashes, and every 64th hash. */
static uint64_t murmur_run(void *state) {
    const struct murmur *s = (const struct murmur *)state;
    uint64_t x = 0, sum = 0, h = 0;
    for (size_t j = 0; j < s->nseeds; j++) {
        uint32_t seed = s->seeds[j];
        size_t off = 0;
        for (size_t i = 0; i < s->nkeys; i++) {
            size_t n = s->len[i];
            uint32_t v = murmur3_32(s->keys + off, n, seed);
            off += n;
            x ^= v;
            sum += v;
            if ((i & 63u) == 0) h = mix(h, v);
        }
    }
    return mix(mix(h, x), sum);
}

static void murmur_teardown([[cx::escapes]] void *state) {
    struct murmur *s = (struct murmur *)state;
    bench_free(s->keys);
    bench_free(s->len);
    bench_free(s);
}

extern const struct bench bench_hash_murmur3 = {
    "hash_murmur3", "kern", "MurmurHash3 x86_32 of 1M keys of 4-64 bytes, 2 seeds (32-bit ops masked in 64 bits)",
    murmur_setup, murmur_run, murmur_teardown,
};
