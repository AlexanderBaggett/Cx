#include "bench.cxh"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

extern uint64_t mix_double(uint64_t h, double v) {
    if (v != v) return mix(h, 0x7ff8000000000000u);        /* NaN */
    if (v == 0.0) return mix(h, 0);
    int e = 0;
    double m = frexp(v, &e);                                /* v = m * 2^e */
    /* keep 40 significant bits of the mantissa */
    int64_t q = (int64_t)(m * 0x1.0p40);
    return mix(mix(h, (uint64_t)(q < 0 ? -q : q)), (uint64_t)(e + 2048) | (q < 0 ? 0x10000u : 0u));
}

extern void *bench_alloc(size_t bytes) {
    void *p = malloc(bytes ? bytes : 1);
    if (!p) { fputs("bench: out of memory\n", stderr); abort(); }
    return p;
}

extern void *bench_zalloc(size_t bytes) {
    void *p = calloc(1, bytes ? bytes : 1);
    if (!p) { fputs("bench: out of memory\n", stderr); abort(); }
    return p;
}

extern void bench_free([[cx::escapes, cx::nullable]] void *p) { free(p); }
