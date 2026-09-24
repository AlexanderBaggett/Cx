/* Shared declarations for the out-of-line call targets in ops_call_targets.c,
 * used by call_direct, call_indirect and struct_pass in ops_ctrl.c. In Cx a
 * struct type belongs to the module or header that declares it (spec §4.5.2),
 * so both files must see these types from one header. */
#ifndef CX_BENCH_OPS_CALLS_H
#define CX_BENCH_OPS_CALLS_H
#include <stdint.h>

/* call_direct */
struct ct_acc { int64_t sum; int64_t count; int64_t max; };
int64_t ct_load(const int32_t *p);
int64_t ct_add(int64_t a, int64_t b);
int64_t ct_clamp(int64_t v, int64_t lo, int64_t hi);
uint32_t ct_hash(uint32_t x);
void ct_accum(struct ct_acc *acc, int64_t v);

/* call_indirect */
typedef int64_t (*ct_binop)(int64_t a, int64_t b);
int64_t ct_op_add(int64_t a, int64_t b);
int64_t ct_op_absdiff(int64_t a, int64_t b);
int64_t ct_op_mul(int64_t a, int64_t b);
int64_t ct_op_min(int64_t a, int64_t b);
int64_t ct_op_max(int64_t a, int64_t b);
int64_t ct_op_xor(int64_t a, int64_t b);
int64_t ct_op_avg(int64_t a, int64_t b);
int64_t ct_op_shmix(int64_t a, int64_t b);

/* struct_pass */
struct sp16 { int64_t a; int64_t b; };
struct sp24 { double x; double y; double z; };
struct sp32 { int64_t a; int64_t b; int64_t c; int64_t d; };
struct sp16 ct_sp16_step(struct sp16 p, int64_t k);
struct sp24 ct_sp24_lerp(struct sp24 p, struct sp24 q, double t);
struct sp32 ct_sp32_mix(struct sp32 x, struct sp32 y);

#endif
