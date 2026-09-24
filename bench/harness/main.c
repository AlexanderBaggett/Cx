/* Benchmark driver.
 *
 *   bench --list                 list benchmarks as "name category what"
 *   bench NAME [--iters K]       run one benchmark: setup, 1 warm-up run,
 *                                K timed runs (default 7); print one JSON line
 *   bench --all [--iters K]      run every benchmark in this process (smoke test)
 *
 * The timed region is exactly one call to run(). Output:
 *   {"name":"...","checksum":"0x...","ns":[t1,...,tK]}
 * The process exits non-zero if the checksum changes between runs.
 */
#define _POSIX_C_SOURCE 200809L
#include "bench.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define B(name) extern const struct bench bench_##name;
#include "list.h"
#undef B

static const struct bench *const all[] = {
#define B(name) &bench_##name,
#include "list.h"
#undef B
};
static const size_t n_all = sizeof all / sizeof all[0];

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static int run_one(const struct bench *b, unsigned iters) {
    void *st = b->setup();
    uint64_t first = b->run(st);                 /* warm-up, not timed */
    printf("{\"name\":\"%s\",\"checksum\":\"0x%016llx\",\"ns\":[", b->name,
           (unsigned long long)first);
    int ok = 1;
    for (unsigned i = 0; i < iters; i++) {
        uint64_t t0 = now_ns();
        uint64_t c = b->run(st);
        uint64_t t1 = now_ns();
        if (c != first) ok = 0;
        printf("%s%llu", i ? "," : "", (unsigned long long)(t1 - t0));
    }
    printf("],\"stable\":%s}\n", ok ? "true" : "false");
    fflush(stdout);
    b->teardown(st);
    if (!ok) fprintf(stderr, "bench: %s: checksum changed between runs\n", b->name);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    unsigned iters = 7;
    const char *name = NULL;
    int list = 0, every = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--all")) every = 1;
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = (unsigned)strtoul(argv[++i], NULL, 10);
        else name = argv[i];
    }
    if (list) {
        for (size_t i = 0; i < n_all; i++)
            printf("%s %s %s\n", all[i]->name, all[i]->category, all[i]->what);
        return 0;
    }
    int rc = 0;
    for (size_t i = 0; i < n_all; i++)
        if (every || (name && !strcmp(name, all[i]->name))) {
            rc |= run_one(all[i], iters);
            if (!every) return rc;
        }
    if (!every) { fprintf(stderr, "bench: unknown benchmark '%s' (try --list)\n", name ? name : ""); return 2; }
    return rc;
}
