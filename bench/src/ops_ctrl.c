/* Control flow and calls: branches, switch dispatch, direct and indirect
 * calls, recursion, and structs passed by value. The call targets are in
 * ops_call_targets.c so they cannot be inlined without LTO. */
#include "bench.h"

/* ---- branch_unpredictable / branch_predictable --------------------------
 * One kernel, two inputs: the same multiset of bytes in random order (the
 * branch is taken ~50% of the time at random) and in sorted order. Each arm
 * updates a recurrence and the else arm stores to memory, so the branch is
 * not if-converted and the loop is not vectorized. */

enum { BRANCH_N = 1 << 16, BRANCH_RANDOM_PASSES = 160, BRANCH_SORTED_PASSES = 560 };

struct branch { uint32_t *d; };

static uint64_t branch_kernel(const uint32_t *d, size_t n, uint64_t seed) {
    uint32_t hist[16] = { 0 };
    uint64_t s1 = 0, x1 = seed, x2 = seed + 1;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = d[i];
        if (v >= 128) {
            s1 += v;
            x1 = ((x1 ^ v) * 3) & 0xffffff;
        } else {
            hist[v & 15] += 1;
            x2 = (x2 + v + (x2 >> 3)) & 0xffffff;
        }
    }
    uint64_t h = mix(mix(mix(0, s1), x1), x2);
    for (size_t k = 0; k < 16; k++) h = mix(h, hist[k]);
    return h;
}

static struct branch *branch_make(int sorted) {
    struct branch *s = (struct branch *)bench_alloc(sizeof *s);
    s->d = (uint32_t *)bench_alloc(BRANCH_N * sizeof(uint32_t));
    struct rng r = { 0x5eed0b7a2c4d1e3fu };
    for (size_t i = 0; i < BRANCH_N; i++) s->d[i] = rng_below(&r, 256);
    if (sorted) {
        /* counting sort keeps the same multiset */
        size_t count[256] = { 0 };
        for (size_t i = 0; i < BRANCH_N; i++) count[s->d[i]] += 1;
        size_t k = 0;
        for (uint32_t v = 0; v < 256; v++)
            for (size_t c = 0; c < count[v]; c++) s->d[k++] = v;
    }
    return s;
}

static void *branch_random_setup(void) { return branch_make(0); }
static void *branch_sorted_setup(void) { return branch_make(1); }

static uint64_t branch_random_run(void *state) {
    const struct branch *s = (const struct branch *)state;
    uint64_t h = 0;
    for (uint64_t pass = 0; pass < BRANCH_RANDOM_PASSES; pass++) h = mix(h, branch_kernel(s->d, BRANCH_N, pass));
    return h;
}

static uint64_t branch_sorted_run(void *state) {
    const struct branch *s = (const struct branch *)state;
    uint64_t h = 0;
    for (uint64_t pass = 0; pass < BRANCH_SORTED_PASSES; pass++) h = mix(h, branch_kernel(s->d, BRANCH_N, pass));
    return h;
}

static void branch_teardown([[cx::escapes]] void *state) {
    struct branch *s = (struct branch *)state;
    bench_free(s->d);
    bench_free(s);
}

extern const struct bench bench_branch_unpredictable = {
    "branch_unpredictable", "ops", "data-dependent branch taken ~50% at random (random bytes)",
    branch_random_setup, branch_random_run, branch_teardown,
};

extern const struct bench bench_branch_predictable = {
    "branch_predictable", "ops", "the branch_unpredictable kernel on the same bytes, sorted (3.5x the passes)",
    branch_sorted_setup, branch_sorted_run, branch_teardown,
};

/* ---- switch_dispatch: a bytecode interpreter with 16 opcodes ------------- */

enum { VM_LEN = 1 << 14, VM_RUNS = 400 };

enum vm_op {
    OP_LOADI, OP_ADD, OP_SUB, OP_MUL, OP_XOR, OP_AND, OP_OR, OP_SHL,
    OP_SHR, OP_ADDI, OP_SUBI, OP_STORE, OP_LOAD, OP_SWAP, OP_SKIPODD, OP_ROT,
    VM_NOPS
};

struct vm_insn { uint8_t op; uint8_t r; uint16_t imm; };

struct vm { struct vm_insn *code; };

static const uint64_t VM_MASK = 0xffffffffu;

static void *vm_setup(void) {
    struct vm *s = (struct vm *)bench_alloc(sizeof *s);
    s->code = (struct vm_insn *)bench_alloc(VM_LEN * sizeof(struct vm_insn));
    struct rng r = { 0xc0dec0dec0de1234u };
    for (size_t i = 0; i < VM_LEN; i++) {
        s->code[i].op = (uint8_t)rng_below(&r, VM_NOPS);
        s->code[i].r = (uint8_t)rng_below(&r, 8);
        s->code[i].imm = (uint16_t)rng_below(&r, 65536);
    }
    return s;
}

/* All values stay in [0, 2^32): every result is masked, and subtraction adds
 * 2^32 first, so nothing can overflow. */
static uint64_t vm_exec(const struct vm_insn *code, size_t n, uint64_t seed) {
    uint64_t reg[8];
    for (size_t k = 0; k < 8; k++) reg[k] = (seed + k * 0x9e3779b9u) & VM_MASK;
    uint64_t acc = seed & VM_MASK;
    for (size_t pc = 0; pc < n; pc++) {
        struct vm_insn in = code[pc];
        uint64_t imm = in.imm;
        switch (in.op) {
        case OP_LOADI: reg[in.r] = imm; break;
        case OP_ADD: acc = (acc + reg[in.r]) & VM_MASK; break;
        case OP_SUB: acc = (acc + (VM_MASK + 1) - reg[in.r]) & VM_MASK; break;
        case OP_MUL: acc = (acc * (reg[in.r] & 0xffff)) & VM_MASK; break;
        case OP_XOR: acc ^= reg[in.r]; break;
        case OP_AND: acc &= reg[in.r] | imm; break;
        case OP_OR: acc |= reg[in.r] & imm; break;
        case OP_SHL: acc = (acc << (reg[in.r] & 7)) & VM_MASK; break;
        case OP_SHR: acc >>= reg[in.r] & 7; break;
        case OP_ADDI: acc = (acc + imm) & VM_MASK; break;
        case OP_SUBI: acc = acc >= imm ? acc - imm : acc + (VM_MASK + 1) - imm; break;
        case OP_STORE: reg[in.r] = acc; break;
        case OP_LOAD: acc = reg[in.r]; break;
        case OP_SWAP: { uint64_t t = reg[in.r]; reg[in.r] = acc; acc = t; } break;
        case OP_SKIPODD: if (acc & 1) pc++; break;
        case OP_ROT: acc = ((acc << 5) | (acc >> 27)) & VM_MASK; break;
        default: break;
        }
    }
    uint64_t h = mix(0, acc);
    for (size_t k = 0; k < 8; k++) h = mix(h, reg[k]);
    return h;
}

static uint64_t vm_run(void *state) {
    const struct vm *s = (const struct vm *)state;
    uint64_t h = 0;
    for (uint64_t run = 0; run < VM_RUNS; run++) h = mix(h, vm_exec(s->code, VM_LEN, run * 7919 + 1));
    return h;
}

static void vm_teardown([[cx::escapes]] void *state) {
    struct vm *s = (struct vm *)state;
    bench_free(s->code);
    bench_free(s);
}

extern const struct bench bench_switch_dispatch = {
    "switch_dispatch", "ops", "bytecode interpreter: switch over 16 opcodes, random program",
    vm_setup, vm_run, vm_teardown,
};

/* ---- call_direct: calls to small functions in another translation unit --- */

struct ct_acc { int64_t sum; int64_t count; int64_t max; };

extern int64_t ct_load(const int32_t *p);
extern int64_t ct_add(int64_t a, int64_t b);
extern int64_t ct_clamp(int64_t v, int64_t lo, int64_t hi);
extern uint32_t ct_hash(uint32_t x);
extern void ct_accum(struct ct_acc *acc, int64_t v);

enum { CALLD_N = 1 << 14, CALLD_PASSES = 480 };

struct calld { int32_t *v; };

static void *calld_setup(void) {
    struct calld *s = (struct calld *)bench_alloc(sizeof *s);
    s->v = (int32_t *)bench_alloc(CALLD_N * sizeof(int32_t));
    struct rng r = { 0xd1ec7ca11u };
    for (size_t i = 0; i < CALLD_N; i++) s->v[i] = (int32_t)rng_below(&r, 2001) - 1000;
    return s;
}

static uint64_t calld_run(void *state) {
    const struct calld *s = (const struct calld *)state;
    uint64_t h = 0;
    for (int pass = 0; pass < CALLD_PASSES; pass++) {
        struct ct_acc acc = { 0, 0, -1000000 };
        int64_t k = pass - CALLD_PASSES / 2;
        for (size_t i = 0; i < CALLD_N; i++) {
            int64_t x = ct_load(&s->v[i]);
            int64_t y = ct_clamp(ct_add(x, k), -700, 700);
            uint32_t hv = ct_hash((uint32_t)(y + 1001));
            ct_accum(&acc, y + (int64_t)(hv & 255));
        }
        h = mix(mix(mix(h, (uint64_t)acc.sum), (uint64_t)acc.count), (uint64_t)acc.max);
    }
    return h;
}

static void calld_teardown([[cx::escapes]] void *state) {
    struct calld *s = (struct calld *)state;
    bench_free(s->v);
    bench_free(s);
}

extern const struct bench bench_call_direct = {
    "call_direct", "ops", "direct calls to small functions in another file (no LTO inlining)",
    calld_setup, calld_run, calld_teardown,
};

/* ---- call_indirect: calls through a table of function pointers ----------- */

typedef int64_t (*ct_binop)(int64_t a, int64_t b);

extern int64_t ct_op_add(int64_t a, int64_t b);
extern int64_t ct_op_absdiff(int64_t a, int64_t b);
extern int64_t ct_op_mul(int64_t a, int64_t b);
extern int64_t ct_op_min(int64_t a, int64_t b);
extern int64_t ct_op_max(int64_t a, int64_t b);
extern int64_t ct_op_xor(int64_t a, int64_t b);
extern int64_t ct_op_avg(int64_t a, int64_t b);
extern int64_t ct_op_shmix(int64_t a, int64_t b);

enum { CALLI_N = 1 << 16, CALLI_PASSES = 360, CALLI_BLOCK = 16 };

struct calli { uint8_t *sel; int64_t *val; ct_binop fn[8]; };

static void *calli_setup(void) {
    struct calli *s = (struct calli *)bench_alloc(sizeof *s);
    s->sel = (uint8_t *)bench_alloc(CALLI_N);
    s->val = (int64_t *)bench_alloc(CALLI_N * sizeof(int64_t));
    s->fn[0] = ct_op_add;
    s->fn[1] = ct_op_absdiff;
    s->fn[2] = ct_op_mul;
    s->fn[3] = ct_op_min;
    s->fn[4] = ct_op_max;
    s->fn[5] = ct_op_xor;
    s->fn[6] = ct_op_avg;
    s->fn[7] = ct_op_shmix;
    struct rng r = { 0x1dca11u };
    /* the target changes every CALLI_BLOCK calls, to a random one */
    uint8_t cur = 0;
    for (size_t i = 0; i < CALLI_N; i++) {
        if (i % CALLI_BLOCK == 0) cur = (uint8_t)rng_below(&r, 8);
        s->sel[i] = cur;
        s->val[i] = (int64_t)rng_below(&r, 1u << 20);
    }
    return s;
}

static uint64_t calli_run(void *state) {
    const struct calli *s = (const struct calli *)state;
    uint64_t h = 0;
    for (int pass = 0; pass < CALLI_PASSES; pass++) {
        int64_t acc = pass;
        for (size_t i = 0; i < CALLI_N; i++) acc = s->fn[s->sel[i]](acc, s->val[i]);
        h = mix(h, (uint64_t)acc);
    }
    return h;
}

static void calli_teardown([[cx::escapes]] void *state) {
    struct calli *s = (struct calli *)state;
    bench_free(s->sel);
    bench_free(s->val);
    bench_free(s);
}

extern const struct bench bench_call_indirect = {
    "call_indirect", "ops", "calls through a table of 8 function pointers (target changes every 16 calls)",
    calli_setup, calli_run, calli_teardown,
};

/* ---- recursion: naive Fibonacci and a recursive binary-tree walk --------- */

enum { REC_FIB_N = 34, REC_NODES = 1 << 16, REC_TREE_PASSES = 30 };

struct tnode { struct tnode *left; struct tnode *right; int64_t val; };

struct rec { struct tnode *pool; int64_t fib_n; };

static int64_t rec_fib(int64_t n) { return n < 2 ? n : rec_fib(n - 1) + rec_fib(n - 2); }

/* depth-weighted sum of the values in the subtree */
static int64_t rec_tree(const struct tnode *t, int64_t depth) {
    int64_t sum = t->val * depth;
    if (t->left) sum += rec_tree(t->left, depth + 1);
    if (t->right) sum += rec_tree(t->right, depth + 1);
    return sum;
}

static void *rec_setup(void) {
    struct rec *s = (struct rec *)bench_alloc(sizeof *s);
    s->pool = (struct tnode *)bench_alloc(REC_NODES * sizeof(struct tnode));
    s->fib_n = REC_FIB_N;
    struct rng r = { 0x7ee5eedu };
    /* random binary search tree; node 0 is the root */
    for (size_t k = 0; k < REC_NODES; k++) {
        struct tnode *nd = &s->pool[k];
        nd->left = NULL;
        nd->right = NULL;
        nd->val = (int64_t)rng_below(&r, 1u << 20);
        if (k == 0) continue;
        struct tnode *cur = &s->pool[0];
        for (;;) {
            if (nd->val < cur->val) {
                if (!cur->left) { cur->left = nd; break; }
                cur = cur->left;
            } else {
                if (!cur->right) { cur->right = nd; break; }
                cur = cur->right;
            }
        }
    }
    return s;
}

static uint64_t rec_run(void *state) {
    const struct rec *s = (const struct rec *)state;
    uint64_t h = mix(0, (uint64_t)rec_fib(s->fib_n));
    for (int pass = 0; pass < REC_TREE_PASSES; pass++)
        h = mix(h, (uint64_t)rec_tree(&s->pool[0], 1 + pass));
    return h;
}

static void rec_teardown([[cx::escapes]] void *state) {
    struct rec *s = (struct rec *)state;
    bench_free(s->pool);
    bench_free(s);
}

extern const struct bench bench_recursion = {
    "recursion", "ops", "naive recursive fib(34) plus recursive walks of a 64K-node random BST",
    rec_setup, rec_run, rec_teardown,
};

/* ---- struct_pass: 16-, 24- and 32-byte structs by value ------------------ */

struct sp16 { int64_t a; int64_t b; };
struct sp24 { double x; double y; double z; };
struct sp32 { int64_t a; int64_t b; int64_t c; int64_t d; };

extern struct sp16 ct_sp16_step(struct sp16 p, int64_t k);
extern struct sp24 ct_sp24_lerp(struct sp24 p, struct sp24 q, double t);
extern struct sp32 ct_sp32_mix(struct sp32 x, struct sp32 y);

enum { SP_N = 1 << 14, SP_PASSES = 240 };

struct spass { int64_t *k; struct sp24 *v24; struct sp32 *v32; };

static void *spass_setup(void) {
    struct spass *s = (struct spass *)bench_alloc(sizeof *s);
    s->k = (int64_t *)bench_alloc(SP_N * sizeof(int64_t));
    s->v24 = (struct sp24 *)bench_alloc(SP_N * sizeof(struct sp24));
    s->v32 = (struct sp32 *)bench_alloc(SP_N * sizeof(struct sp32));
    struct rng r = { 0x5ca1ab1e5u };
    for (size_t i = 0; i < SP_N; i++) {
        s->k[i] = (int64_t)rng_below(&r, 1u << 20);
        s->v24[i].x = rng_unit(&r) * 2.0 - 1.0;
        s->v24[i].y = rng_unit(&r) * 2.0 - 1.0;
        s->v24[i].z = rng_unit(&r) * 2.0 - 1.0;
        s->v32[i].a = (int64_t)(rng_next(&r) >> 32);
        s->v32[i].b = (int64_t)(rng_next(&r) >> 32);
        s->v32[i].c = (int64_t)(rng_next(&r) >> 32);
        s->v32[i].d = (int64_t)(rng_next(&r) >> 32);
    }
    return s;
}

static uint64_t spass_run(void *state) {
    const struct spass *s = (const struct spass *)state;
    uint64_t h = 0;
    for (int pass = 0; pass < SP_PASSES; pass++) {
        struct sp16 a16 = { pass, 1 };
        struct sp24 a24 = { 0.0, 0.0, 0.0 };
        struct sp32 a32 = { pass, 0, 0, 0 };
        double t = 0.125 + 0.5 * (double)pass / (double)SP_PASSES;
        for (size_t i = 0; i < SP_N; i++) {
            a16 = ct_sp16_step(a16, s->k[i]);
            a24 = ct_sp24_lerp(a24, s->v24[i], t);
            a32 = ct_sp32_mix(a32, s->v32[i]);
        }
        h = mix(mix(h, (uint64_t)a16.a), (uint64_t)a16.b);
        h = mix_double(mix_double(mix_double(h, a24.x), a24.y), a24.z);
        h = mix(mix(mix(mix(h, (uint64_t)a32.a), (uint64_t)a32.b), (uint64_t)a32.c), (uint64_t)a32.d);
    }
    return h;
}

static void spass_teardown([[cx::escapes]] void *state) {
    struct spass *s = (struct spass *)state;
    bench_free(s->k);
    bench_free(s->v24);
    bench_free(s->v32);
    bench_free(s);
}

extern const struct bench bench_struct_pass = {
    "struct_pass", "ops", "pass and return 16/24/32-byte structs by value across files",
    spass_setup, spass_run, spass_teardown,
};
