/* Simulation and number kernels: N-body, Mandelbrot, prime sieve. */
#include "bench.h"

#include <math.h>
#include <stdbit.h>
#include <string.h>

/* ---- nbody: 5-body Jovian planets simulation (symplectic Euler) ----------- */

enum { NB_BODIES = 5, NB_STEPS = 600000 };

static const double nb_solar_mass = 39.47841760435743;   /* 4 * pi^2 */
static const double nb_days_per_year = 365.24;
static const double nb_dt = 0.01;

struct body { double x, y, z, vx, vy, vz, mass; };

/* Sun, Jupiter, Saturn, Uranus, Neptune: position (AU), velocity (AU/day),
 * mass (solar masses). */
static const double nb_init[NB_BODIES][7] = {
    { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0 },
    { 4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
      1.66007664274403694e-03, 7.69901118419740425e-03, -6.90460016972063023e-05,
      9.54791938424326609e-04 },
    { 8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
      -2.76742510726862411e-03, 4.99852801234917238e-03, 2.30417297573763929e-05,
      2.85885980666130812e-04 },
    { 1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
      2.96460137564761618e-03, 2.37847173959480950e-03, -2.96589568540237556e-05,
      4.36624404335156298e-05 },
    { 1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
      2.68067772490389322e-03, 1.62824170038242295e-03, -9.51592254519715870e-05,
      5.15138902046611451e-05 },
};

struct nbody {
    struct body start[NB_BODIES];
    struct body work[NB_BODIES];
};

static void *nbody_setup(void) {
    struct nbody *s = (struct nbody *)bench_alloc(sizeof *s);
    for (size_t i = 0; i < NB_BODIES; i++) {
        struct body *b = &s->start[i];
        b->x = nb_init[i][0];
        b->y = nb_init[i][1];
        b->z = nb_init[i][2];
        b->vx = nb_init[i][3] * nb_days_per_year;
        b->vy = nb_init[i][4] * nb_days_per_year;
        b->vz = nb_init[i][5] * nb_days_per_year;
        b->mass = nb_init[i][6] * nb_solar_mass;
    }
    /* offset momentum so the system's centre of mass is at rest */
    double px = 0.0, py = 0.0, pz = 0.0;
    for (size_t i = 0; i < NB_BODIES; i++) {
        px += s->start[i].vx * s->start[i].mass;
        py += s->start[i].vy * s->start[i].mass;
        pz += s->start[i].vz * s->start[i].mass;
    }
    s->start[0].vx = -px / nb_solar_mass;
    s->start[0].vy = -py / nb_solar_mass;
    s->start[0].vz = -pz / nb_solar_mass;
    return s;
}

static double nbody_energy(const struct body *b, size_t n) {
    double e = 0.0;
    for (size_t i = 0; i < n; i++) {
        e += 0.5 * b[i].mass * (b[i].vx * b[i].vx + b[i].vy * b[i].vy + b[i].vz * b[i].vz);
        for (size_t j = i + 1; j < n; j++) {
            double dx = b[i].x - b[j].x, dy = b[i].y - b[j].y, dz = b[i].z - b[j].z;
            e -= b[i].mass * b[j].mass / sqrt(dx * dx + dy * dy + dz * dz);
        }
    }
    return e;
}

static void nbody_advance(struct body *b, size_t n, double dt) {
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            double dx = b[i].x - b[j].x, dy = b[i].y - b[j].y, dz = b[i].z - b[j].z;
            double d2 = dx * dx + dy * dy + dz * dz;
            double mag = dt / (d2 * sqrt(d2));
            double mi = b[i].mass * mag, mj = b[j].mass * mag;
            b[i].vx -= dx * mj;
            b[i].vy -= dy * mj;
            b[i].vz -= dz * mj;
            b[j].vx += dx * mi;
            b[j].vy += dy * mi;
            b[j].vz += dz * mi;
        }
    }
    for (size_t i = 0; i < n; i++) {
        b[i].x += dt * b[i].vx;
        b[i].y += dt * b[i].vy;
        b[i].z += dt * b[i].vz;
    }
}

static uint64_t nbody_run(void *state) {
    struct nbody *s = (struct nbody *)state;
    memcpy(s->work, s->start, sizeof s->work);
    uint64_t h = mix_double(0, nbody_energy(s->work, NB_BODIES));
    for (int step = 0; step < NB_STEPS; step++) nbody_advance(s->work, NB_BODIES, nb_dt);
    h = mix_double(h, nbody_energy(s->work, NB_BODIES));
    for (size_t i = 0; i < NB_BODIES; i++)
        h = mix_double(mix_double(mix_double(h, s->work[i].x), s->work[i].y), s->work[i].z);
    return h;
}

static void nbody_teardown([[cx::escapes]] void *state) {
    bench_free(state);
}

extern const struct bench bench_nbody = {
    "nbody", "kern", "5-body planetary simulation, 600k steps (double, sqrt)",
    nbody_setup, nbody_run, nbody_teardown,
};

/* ---- mandelbrot: escape-time iteration counts over a grid ----------------- */

enum { MB_W = 560, MB_H = 420, MB_MAXIT = 256 };

static const double mb_x0 = -2.2, mb_x1 = 0.8, mb_y0 = -1.2, mb_y1 = 1.2;

struct mandel { [[cx::owned]] uint16_t *img; };

static void *mandel_setup(void) {
    struct mandel *s = (struct mandel *)bench_alloc(sizeof *s);
    s->img = (uint16_t *)bench_alloc((size_t)MB_W * MB_H * sizeof(uint16_t));
    return s;
}

static void mandel_render(uint16_t *img, size_t w, size_t h, unsigned maxit) {
    double dx = (mb_x1 - mb_x0) / (double)w, dy = (mb_y1 - mb_y0) / (double)h;
    for (size_t py = 0; py < h; py++) {
        double ci = mb_y0 + (double)py * dy;
        for (size_t px = 0; px < w; px++) {
            double cr = mb_x0 + (double)px * dx;
            double zr = 0.0, zi = 0.0, zr2 = 0.0, zi2 = 0.0;
            unsigned it = 0;
            while (it < maxit && zr2 + zi2 <= 4.0) {
                zi = 2.0 * zr * zi + ci;
                zr = zr2 - zi2 + cr;
                zr2 = zr * zr;
                zi2 = zi * zi;
                it++;
            }
            img[py * w + px] = (uint16_t)it;
        }
    }
}

static uint64_t mandel_run(void *state) {
    const struct mandel *s = (const struct mandel *)state;
    mandel_render(s->img, MB_W, MB_H, MB_MAXIT);
    uint64_t h = 0;
    for (size_t py = 0; py < MB_H; py++) {
        uint64_t row = 0;
        for (size_t px = 0; px < MB_W; px++) row += (uint64_t)s->img[py * MB_W + px] * (px + 1);
        h = mix(h, row);
    }
    return h;
}

static void mandel_teardown([[cx::escapes]] void *state) {
    struct mandel *s = (struct mandel *)state;
    bench_free(s->img);
    bench_free(s);
}

extern const struct bench bench_mandelbrot = {
    "mandelbrot", "kern", "escape-time Mandelbrot, 560x420 grid, up to 256 iterations",
    mandel_setup, mandel_run, mandel_teardown,
};

/* ---- sieve_primes: sieve of Eratosthenes over an odd-only bit array ------- */

enum { SV_LIMIT = 50000000, SV_BITS = SV_LIMIT / 2, SV_WORDS = (SV_BITS + 63) / 64 };

/* Bit k of the array stands for the odd number 2k+1 (SV_LIMIT is even). */
struct sieve { [[cx::owned]] uint64_t *bits; };

static void *sieve_setup(void) {
    struct sieve *s = (struct sieve *)bench_alloc(sizeof *s);
    s->bits = (uint64_t *)bench_alloc(SV_WORDS * sizeof(uint64_t));
    return s;
}

/* Afterwards bit k is set iff 2k+1 is prime, for k < nbits. */
static void sieve_mark(uint64_t *bits, size_t nbits) {
    size_t nwords = (nbits + 63) / 64;
    for (size_t i = 0; i < nwords; i++) bits[i] = ~(uint64_t)0;
    if (nbits % 64) bits[nwords - 1] = ((uint64_t)1 << (nbits % 64)) - 1;
    bits[0] &= ~(uint64_t)1;                           /* 1 is not prime */
    for (size_t k = 1; (2 * k + 1) * (2 * k + 1) / 2 < nbits; k++) {
        if (!((bits[k >> 6] >> (k & 63)) & 1)) continue;
        size_t p = 2 * k + 1;
        for (size_t m = p * p / 2; m < nbits; m += p) bits[m >> 6] &= ~((uint64_t)1 << (m & 63));
    }
}

static uint64_t sieve_run(void *state) {
    const struct sieve *s = (const struct sieve *)state;
    sieve_mark(s->bits, SV_BITS);
    uint64_t count = 1, sum = 2;       /* the prime 2; sum of primes < 5e7 is < 2^48 */
    for (size_t i = 0; i < SV_WORDS; i++) {
        uint64_t w = s->bits[i];
        count += stdc_count_ones(w);
        while (w) {
            sum += 2 * (64 * i + stdc_trailing_zeros(w)) + 1;
            w &= w - 1;
        }
    }
    return mix(mix(0, count), sum);
}

static void sieve_teardown([[cx::escapes]] void *state) {
    struct sieve *s = (struct sieve *)state;
    bench_free(s->bits);
    bench_free(s);
}

extern const struct bench bench_sieve_primes = {
    "sieve_primes", "kern", "sieve of Eratosthenes up to 50M (odd-only bit array), count primes",
    sieve_setup, sieve_run, sieve_teardown,
};
