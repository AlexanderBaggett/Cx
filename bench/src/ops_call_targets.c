/* Out-of-line call targets for call_direct, call_indirect and struct_pass
 * (ops_ctrl.c). They live in their own translation unit so that, without
 * LTO, neither compiler can inline them into the callers. This file defines
 * no benchmark of its own. Shared declarations are in ops_calls.h. */
#include "bench.cxh"
#include "ops_calls.cxh"

/* ---- call_direct targets ------------------------------------------------ */

extern int64_t ct_load(const int32_t *p) { return *p; }

extern int64_t ct_add(int64_t a, int64_t b) { return a + b; }

extern int64_t ct_clamp(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

extern uint32_t ct_hash(uint32_t x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

extern void ct_accum(struct ct_acc *acc, int64_t v) {
    acc->sum += v;
    acc->count += 1;
    if (v > acc->max) acc->max = v;
}

/* ---- call_indirect targets: binary ops on values in [0, 2^20) ------------ */

enum { CT_MASK = (1 << 20) - 1 };

extern int64_t ct_op_add(int64_t a, int64_t b) { return (a + b) & CT_MASK; }
extern int64_t ct_op_absdiff(int64_t a, int64_t b) { return a > b ? a - b : b - a; }
extern int64_t ct_op_mul(int64_t a, int64_t b) { return (a * b + 1) & CT_MASK; }
extern int64_t ct_op_min(int64_t a, int64_t b) { return a < b ? a : b; }
extern int64_t ct_op_max(int64_t a, int64_t b) { return a > b ? a : b; }
extern int64_t ct_op_xor(int64_t a, int64_t b) { return a ^ b; }
extern int64_t ct_op_avg(int64_t a, int64_t b) { return (a + b + 1) / 2; }
extern int64_t ct_op_shmix(int64_t a, int64_t b) { return ((a << 3) ^ (b >> 2)) & CT_MASK; }

/* ---- struct_pass targets: 16-, 24- and 32-byte structs by value ---------- */

extern struct sp16 ct_sp16_step(struct sp16 p, int64_t k) {
    struct sp16 r;
    r.a = (p.a + k) & 0xffffff;
    r.b = (p.b ^ (p.a * 3)) & 0xffffff;
    return r;
}

extern struct sp24 ct_sp24_lerp(struct sp24 p, struct sp24 q, double t) {
    struct sp24 r;
    r.x = p.x + (q.x - p.x) * t;
    r.y = p.y + (q.y - p.y) * t;
    r.z = p.z + (q.z - p.z) * t;
    return r;
}

extern struct sp32 ct_sp32_mix(struct sp32 x, struct sp32 y) {
    struct sp32 r;
    r.a = (x.a + y.b) & 0xffffffff;
    r.b = x.b ^ y.c;
    r.c = x.c > y.d ? x.c - (y.d >> 1) : y.d;
    r.d = (x.d + y.a + 1) & 0xffffffff;
    return r;
}
