/* Codec kernels: CRC-32, base64, SHA-256, LZ77 match finding, run-length coding. */
#include "bench.h"

#include <string.h>

/* Fills p[0..n) with pseudo-random bytes. */
static void fill_random(uint8_t *p, size_t n, uint64_t seed) {
    struct rng r = { seed };
    for (size_t i = 0; i < n; i += 8) {
        uint64_t x = rng_next(&r);
        for (size_t k = 0; k < 8 && i + k < n; k++) p[i + k] = (uint8_t)((x >> (8 * k)) & 0xffu);
    }
}

/* ---- crc32: table-driven CRC-32 (IEEE 802.3, reflected) ------------------- */

enum { CRC_BYTES = 20 << 20 };

struct crc {
    uint32_t table[256];
    [[cx::owned]] uint8_t *buf;
};

static void *crc_setup(void) {
    struct crc *s = (struct crc *)bench_alloc(sizeof *s);
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        s->table[n] = c;
    }
    s->buf = (uint8_t *)bench_alloc(CRC_BYTES);
    fill_random(s->buf, CRC_BYTES, 0x3c6ef372fe94f82bu);
    return s;
}

static uint32_t crc32_update(const uint32_t *table, const uint8_t *p, size_t n, uint32_t crc) {
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ (uint32_t)p[i]) & 0xffu] ^ (crc >> 8);
    return crc;
}

static uint64_t crc_run(void *state) {
    const struct crc *s = (const struct crc *)state;
    uint32_t c = crc32_update(s->table, s->buf, CRC_BYTES, 0xffffffffu) ^ 0xffffffffu;
    return mix(0, c);
}

static void crc_teardown([[cx::escapes]] void *state) {
    struct crc *s = (struct crc *)state;
    bench_free(s->buf);
    bench_free(s);
}

extern const struct bench bench_crc32 = {
    "crc32", "kern", "table-driven byte-at-a-time CRC-32 over 20 MB",
    crc_setup, crc_run, crc_teardown,
};

/* ---- base64: encode then decode random bytes, verify the round trip ------- */

enum { B64_BYTES = 16 << 20, B64_TEXT = (B64_BYTES + 2) / 3 * 4 };

struct b64 {
    [[cx::owned]] uint8_t *enc;    /* 6-bit value -> character (64 entries) */
    [[cx::owned]] uint8_t *dec;    /* character -> 6-bit value, 0xff if invalid (256) */
    [[cx::owned]] uint8_t *src;
    [[cx::owned]] uint8_t *text;
    [[cx::owned]] uint8_t *back;
};

static void *b64_setup(void) {
    struct b64 *s = (struct b64 *)bench_alloc(sizeof *s);
    s->enc = (uint8_t *)bench_alloc(64);
    s->dec = (uint8_t *)bench_alloc(256);
    for (int i = 0; i < 26; i++) {
        s->enc[i] = (uint8_t)('A' + i);
        s->enc[26 + i] = (uint8_t)('a' + i);
    }
    for (int i = 0; i < 10; i++) s->enc[52 + i] = (uint8_t)('0' + i);
    s->enc[62] = (uint8_t)'+';
    s->enc[63] = (uint8_t)'/';
    for (int i = 0; i < 256; i++) s->dec[i] = 0xffu;
    for (int i = 0; i < 64; i++) s->dec[s->enc[i]] = (uint8_t)i;
    s->src = (uint8_t *)bench_alloc(B64_BYTES);
    s->text = (uint8_t *)bench_alloc(B64_TEXT);
    s->back = (uint8_t *)bench_alloc(B64_BYTES + 3);
    fill_random(s->src, B64_BYTES, 0xa54ff53a5f1d36f1u);
    return s;
}

static size_t b64_encode(const uint8_t *alpha, const uint8_t *in, size_t n, uint8_t *out) {
    size_t i = 0, o = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | (uint32_t)in[i + 2];
        out[o] = alpha[v >> 18];
        out[o + 1] = alpha[(v >> 12) & 63u];
        out[o + 2] = alpha[(v >> 6) & 63u];
        out[o + 3] = alpha[v & 63u];
        o += 4;
    }
    size_t rem = n - i;
    if (rem) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (rem == 2) v |= (uint32_t)in[i + 1] << 8;
        out[o] = alpha[v >> 18];
        out[o + 1] = alpha[(v >> 12) & 63u];
        out[o + 2] = (uint8_t)(rem == 2 ? alpha[(v >> 6) & 63u] : '=');
        out[o + 3] = (uint8_t)'=';
        o += 4;
    }
    return o;
}

struct b64_result { size_t len; uint32_t bad; };

/* n must be a non-zero multiple of 4; bad != 0 if any character was invalid. */
static struct b64_result b64_decode(const uint8_t *dec, const uint8_t *in, size_t n, uint8_t *out) {
    struct b64_result res = { 0, 1 };
    if (n == 0 || n % 4 != 0) return res;
    size_t o = 0, last = n - 4;
    uint32_t bad = 0;
    for (size_t i = 0; i < last; i += 4) {
        uint32_t a = dec[in[i]], b = dec[in[i + 1]], c = dec[in[i + 2]], d = dec[in[i + 3]];
        bad |= a | b | c | d;
        uint32_t v = a << 18 | b << 12 | c << 6 | d;
        out[o] = (uint8_t)((v >> 16) & 0xffu);
        out[o + 1] = (uint8_t)((v >> 8) & 0xffu);
        out[o + 2] = (uint8_t)(v & 0xffu);
        o += 3;
    }
    /* final quantum, possibly padded */
    const uint8_t *q = in + last;
    size_t pad = 0;
    if (q[3] == '=') pad = q[2] == '=' ? 2u : 1u;
    uint32_t a = dec[q[0]], b = dec[q[1]];
    uint32_t c = pad >= 2 ? 0u : (uint32_t)dec[q[2]], d = pad >= 1 ? 0u : (uint32_t)dec[q[3]];
    bad |= a | b | c | d;
    uint32_t v = a << 18 | b << 12 | c << 6 | d;
    out[o++] = (uint8_t)((v >> 16) & 0xffu);
    if (pad < 2) out[o++] = (uint8_t)((v >> 8) & 0xffu);
    if (pad < 1) out[o++] = (uint8_t)(v & 0xffu);
    res.len = o;
    res.bad = bad & 0xc0u;
    return res;
}

static uint64_t b64_run(void *state) {
    const struct b64 *s = (const struct b64 *)state;
    size_t tlen = b64_encode(s->enc, s->src, B64_BYTES, s->text);
    struct b64_result r = b64_decode(s->dec, s->text, tlen, s->back);
    uint64_t mismatches = 0, tsum = 0;
    for (size_t i = 0; i < B64_BYTES; i++)
        if (s->back[i] != s->src[i]) mismatches++;
    for (size_t i = 0; i < tlen; i++) tsum += (uint64_t)s->text[i];
    return mix(mix(mix(mix(mix(0, tlen), r.len), r.bad), mismatches), tsum);
}

static void b64_teardown([[cx::escapes]] void *state) {
    struct b64 *s = (struct b64 *)state;
    bench_free(s->enc);
    bench_free(s->dec);
    bench_free(s->src);
    bench_free(s->text);
    bench_free(s->back);
    bench_free(s);
}

extern const struct bench bench_base64 = {
    "base64", "kern", "base64 encode + decode of 16 MB random bytes, round-trip check",
    b64_setup, b64_run, b64_teardown,
};

/* ---- sha256: SHA-256 digest of a buffer ----------------------------------- */

enum { SHA_BYTES = 10 << 20 };

static const uint32_t sha_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

struct sha { [[cx::owned]] uint8_t *buf; };

static void *sha_setup(void) {
    struct sha *s = (struct sha *)bench_alloc(sizeof *s);
    s->buf = (uint8_t *)bench_alloc(SHA_BYTES);
    fill_random(s->buf, SHA_BYTES, 0x510e527fade682d1u);
    return s;
}

/* 0 < n < 32; the left shift discards the rotated-out bits. */
static uint32_t rotr32(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

/* Addition modulo 2^32: the sum is formed in 64 bits and masked, so it never wraps. */
static uint32_t mod32(uint64_t sum) { return (uint32_t)(sum & 0xffffffffu); }

static void sha256_block(uint32_t *hs, const uint8_t *p) {
    uint32_t w[64];
    for (size_t t = 0; t < 16; t++)
        w[t] = (uint32_t)p[4 * t] << 24 | (uint32_t)p[4 * t + 1] << 16 |
               (uint32_t)p[4 * t + 2] << 8 | (uint32_t)p[4 * t + 3];
    for (size_t t = 16; t < 64; t++) {
        uint32_t s0 = rotr32(w[t - 15], 7) ^ rotr32(w[t - 15], 18) ^ (w[t - 15] >> 3);
        uint32_t s1 = rotr32(w[t - 2], 17) ^ rotr32(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = mod32((uint64_t)w[t - 16] + s0 + w[t - 7] + s1);
    }
    uint32_t a = hs[0], b = hs[1], c = hs[2], d = hs[3];
    uint32_t e = hs[4], f = hs[5], g = hs[6], h = hs[7];
    for (size_t t = 0; t < 64; t++) {
        uint32_t big1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = (uint64_t)h + big1 + ch + sha_k[t] + w[t];
        uint32_t big0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = (uint64_t)big0 + maj;
        h = g;
        g = f;
        f = e;
        e = mod32(d + t1);
        d = c;
        c = b;
        b = a;
        a = mod32(t1 + t2);
    }
    hs[0] = mod32((uint64_t)hs[0] + a);
    hs[1] = mod32((uint64_t)hs[1] + b);
    hs[2] = mod32((uint64_t)hs[2] + c);
    hs[3] = mod32((uint64_t)hs[3] + d);
    hs[4] = mod32((uint64_t)hs[4] + e);
    hs[5] = mod32((uint64_t)hs[5] + f);
    hs[6] = mod32((uint64_t)hs[6] + g);
    hs[7] = mod32((uint64_t)hs[7] + h);
}

static void sha256(const uint8_t *msg, size_t n, uint32_t *digest) {
    uint32_t hs[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };
    size_t full = n / 64;
    for (size_t i = 0; i < full; i++) sha256_block(hs, msg + 64 * i);
    /* padding: 0x80, zeros, 64-bit big-endian bit length */
    uint8_t tail[128] = { 0 };
    size_t rem = n - full * 64;
    memcpy(tail, msg + full * 64, rem);
    tail[rem] = 0x80u;
    size_t tlen = rem < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (size_t k = 0; k < 8; k++) tail[tlen - 1 - k] = (uint8_t)((bits >> (8 * k)) & 0xffu);
    for (size_t j = 0; j < tlen; j += 64) sha256_block(hs, tail + j);
    for (size_t i = 0; i < 8; i++) digest[i] = hs[i];
}

static uint64_t sha_run(void *state) {
    const struct sha *s = (const struct sha *)state;
    uint32_t digest[8];
    sha256(s->buf, SHA_BYTES, digest);
    uint64_t h = 0;
    for (size_t i = 0; i < 8; i++) h = mix(h, digest[i]);
    return h;
}

static void sha_teardown([[cx::escapes]] void *state) {
    struct sha *s = (struct sha *)state;
    bench_free(s->buf);
    bench_free(s);
}

extern const struct bench bench_sha256 = {
    "sha256", "kern", "SHA-256 of a 10 MB buffer (32-bit adds done in 64 bits and masked)",
    sha_setup, sha_run, sha_teardown,
};

/* ---- lz_match: LZ77 hash-chain longest-match finder ----------------------- */

enum {
    LZ_BYTES = 6 << 20,
    LZ_HASH_BITS = 15,
    LZ_WBITS = 15,
    LZ_WSIZE = 1 << LZ_WBITS,      /* window: matches are at most this far back */
    LZ_WMASK = LZ_WSIZE - 1,
    LZ_MIN = 4,                    /* shortest match; also the hashed prefix */
    LZ_MAX = 258,
    LZ_CHAIN = 32,                 /* candidates examined per position */
    LZ_VOCAB = 400,
};

struct lz {
    [[cx::owned]] uint8_t *text;
    [[cx::owned]] int32_t *head;   /* hash -> most recent position, -1 if none */
    [[cx::owned]] int32_t *prev;   /* position & LZ_WMASK -> previous position with that hash */
};

/* Word salad from a skewed vocabulary, with repeated earlier phrases. */
static void lz_gen_text(uint8_t *text, size_t n, uint64_t seed) {
    struct rng r = { seed };
    uint8_t vocab[LZ_VOCAB][10];
    size_t vlen[LZ_VOCAB];
    for (size_t w = 0; w < LZ_VOCAB; w++) {
        vlen[w] = 2 + rng_below(&r, 9);
        for (size_t k = 0; k < vlen[w]; k++) vocab[w][k] = (uint8_t)('a' + rng_below(&r, 26));
    }
    size_t pos = 0;
    while (pos < n) {
        uint32_t kind = rng_below(&r, 16);
        if (kind == 0 && pos > 1024) {
            size_t span = pos - 64 < 30000 ? pos - 64 : 30000;
            size_t from = pos - (64 + rng_below(&r, (uint32_t)span));
            size_t len = 16 + rng_below(&r, 240);
            for (size_t k = 0; k < len && pos < n; k++) text[pos++] = text[from + k];
        } else {
            uint32_t w = rng_below(&r, rng_below(&r, LZ_VOCAB) + 1);
            for (size_t k = 0; k < vlen[w] && pos < n; k++) text[pos++] = vocab[w][k];
            uint8_t sep = (uint8_t)' ';
            if (kind == 1) sep = (uint8_t)',';
            else if (kind == 2) sep = (uint8_t)'\n';
            if (pos < n) text[pos++] = sep;
        }
    }
}

static void *lz_setup(void) {
    struct lz *s = (struct lz *)bench_alloc(sizeof *s);
    s->text = (uint8_t *)bench_alloc(LZ_BYTES);
    s->head = (int32_t *)bench_alloc(((size_t)1 << LZ_HASH_BITS) * sizeof(int32_t));
    s->prev = (int32_t *)bench_alloc(LZ_WSIZE * sizeof(int32_t));
    lz_gen_text(s->text, LZ_BYTES, 0x1f83d9ab5be0cd19u);
    return s;
}

/* Multiplicative hash of 4 bytes. The 64-bit product cannot overflow; its
 * low 32 bits are the classic 32-bit Knuth hash. */
static uint32_t lz_hash(const uint8_t *p) {
    uint32_t x = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    uint64_t prod = (uint64_t)x * 2654435761u;
    return (uint32_t)((prod & 0xffffffffu) >> (32 - LZ_HASH_BITS));
}

/* Greedy parse. Returns a checksum of matches, literals, lengths and distances. */
static uint64_t lz_parse(const uint8_t *text, size_t n, int32_t *head, int32_t *prev) {
    for (size_t i = 0; i < ((size_t)1 << LZ_HASH_BITS); i++) head[i] = -1;
    uint64_t matches = 0, literals = 0, total_len = 0, total_dist = 0;
    size_t pos = 0;
    while (pos + LZ_MIN <= n) {
        uint32_t hv = lz_hash(text + pos);
        int32_t cand = head[hv];
        prev[pos & LZ_WMASK] = cand;
        head[hv] = (int32_t)pos;
        size_t maxlen = n - pos < LZ_MAX ? n - pos : LZ_MAX;
        size_t best = 0, best_dist = 0;
        const uint8_t *cur = text + pos;
        for (int chain = LZ_CHAIN; cand >= 0 && chain > 0; chain--) {
            size_t c = (size_t)cand;
            if (pos - c >= LZ_WSIZE) break;           /* prev[c & mask] would be stale */
            const uint8_t *old = text + c;
            if (old[best] == cur[best]) {
                size_t len = 0;
                while (len < maxlen && old[len] == cur[len]) len++;
                if (len > best) {
                    best = len;
                    best_dist = pos - c;
                    if (len == maxlen) break;
                }
            }
            cand = prev[c & LZ_WMASK];
        }
        if (best >= LZ_MIN) {
            matches++;
            total_len += best;
            total_dist += best_dist;
            size_t end = pos + best;
            for (size_t q = pos + 1; q < end && q + LZ_MIN <= n; q++) {
                uint32_t hq = lz_hash(text + q);
                prev[q & LZ_WMASK] = head[hq];
                head[hq] = (int32_t)q;
            }
            pos = end;
        } else {
            literals++;
            pos++;
        }
    }
    literals += n - pos;
    return mix(mix(mix(mix(0, matches), literals), total_len), total_dist);
}

static uint64_t lz_run(void *state) {
    const struct lz *s = (const struct lz *)state;
    return lz_parse(s->text, LZ_BYTES, s->head, s->prev);
}

static void lz_teardown([[cx::escapes]] void *state) {
    struct lz *s = (struct lz *)state;
    bench_free(s->text);
    bench_free(s->head);
    bench_free(s->prev);
    bench_free(s);
}

extern const struct bench bench_lz_match = {
    "lz_match", "kern", "LZ77 hash-chain longest-match search over 6 MB of generated text",
    lz_setup, lz_run, lz_teardown,
};

/* ---- rle_codec: PackBits-style run-length encode + decode ----------------- */

enum { RLE_BYTES = 24 << 20, RLE_MAXRUN = 130, RLE_MAXLIT = 128, RLE_CAP = RLE_BYTES + RLE_BYTES / 64 + 16 };

struct rle {
    [[cx::owned]] uint8_t *src;
    [[cx::owned]] uint8_t *enc;
    [[cx::owned]] uint8_t *dec;
};

static void *rle_setup(void) {
    struct rle *s = (struct rle *)bench_alloc(sizeof *s);
    s->src = (uint8_t *)bench_alloc(RLE_BYTES);
    s->enc = (uint8_t *)bench_alloc(RLE_CAP);
    s->dec = (uint8_t *)bench_alloc(RLE_BYTES);
    struct rng r = { 0x6a09e667f3bcc909u };
    size_t pos = 0;
    while (pos < RLE_BYTES) {
        if (rng_below(&r, 4) == 0) {                  /* noisy stretch */
            size_t len = 1 + rng_below(&r, 24);
            for (size_t k = 0; k < len && pos < RLE_BYTES; k++) s->src[pos++] = (uint8_t)rng_below(&r, 256);
        } else {                                      /* run, mostly long */
            size_t len = 1 + rng_below(&r, rng_below(&r, 600) + 1);
            uint8_t v = (uint8_t)rng_below(&r, 256);
            for (size_t k = 0; k < len && pos < RLE_BYTES; k++) s->src[pos++] = v;
        }
    }
    return s;
}

/* Control byte c < 128: c+1 literal bytes follow. c >= 128: the next byte
 * repeats c-125 times (3..130). */
static size_t rle_encode(const uint8_t *in, size_t n, uint8_t *out) {
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t v = in[i];
        size_t run = 1;
        while (run < RLE_MAXRUN && i + run < n && in[i + run] == v) run++;
        if (run >= 3) {
            out[o] = (uint8_t)(run + 125);
            out[o + 1] = v;
            o += 2;
            i += run;
            continue;
        }
        /* literals until a run of 3 starts; the first byte never starts one */
        size_t start = i, len = 0;
        while (i < n && len < RLE_MAXLIT) {
            if (i + 2 < n && in[i] == in[i + 1] && in[i] == in[i + 2]) break;
            i++;
            len++;
        }
        out[o++] = (uint8_t)(len - 1);
        for (size_t k = 0; k < len; k++) out[o + k] = in[start + k];
        o += len;
    }
    return o;
}

static size_t rle_decode(const uint8_t *in, size_t n, uint8_t *out, size_t cap) {
    size_t i = 0, o = 0;
    while (i < n) {
        size_t c = in[i];
        if (c < 128) {
            size_t len = c + 1;
            if (i + 1 + len > n || o + len > cap) break;
            for (size_t k = 0; k < len; k++) out[o + k] = in[i + 1 + k];
            i += 1 + len;
            o += len;
        } else {
            size_t len = c - 125;
            if (i + 2 > n || o + len > cap) break;
            uint8_t v = in[i + 1];
            for (size_t k = 0; k < len; k++) out[o + k] = v;
            i += 2;
            o += len;
        }
    }
    return o;
}

static uint64_t rle_run(void *state) {
    const struct rle *s = (const struct rle *)state;
    size_t elen = rle_encode(s->src, RLE_BYTES, s->enc);
    size_t dlen = rle_decode(s->enc, elen, s->dec, RLE_BYTES);
    uint64_t mismatches = 0, esum = 0;
    for (size_t i = 0; i < dlen; i++)
        if (s->dec[i] != s->src[i]) mismatches++;
    for (size_t i = 0; i < elen; i++) esum += (uint64_t)s->enc[i];
    return mix(mix(mix(mix(0, elen), dlen), mismatches), esum);
}

static void rle_teardown([[cx::escapes]] void *state) {
    struct rle *s = (struct rle *)state;
    bench_free(s->src);
    bench_free(s->enc);
    bench_free(s->dec);
    bench_free(s);
}

extern const struct bench bench_rle_codec = {
    "rle_codec", "kern", "PackBits-style run-length encode + decode of 24 MB with long runs",
    rle_setup, rle_run, rle_teardown,
};
