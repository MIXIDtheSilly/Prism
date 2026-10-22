// Included first (-include) in the recording builds (the proxy's, with LIFT_RECORD, and fuzz's):
// the lifted code's memory accesses and basic blocks go through these, which record.c logs.
#include <stdint.h>
uint32_t rec_ld32(uint64_t a);
uint64_t rec_ld64(uint64_t a);
float rec_ldf(uint64_t a);
void rec_ldq(void *r, uint64_t a);
void rec_st32(uint64_t a, uint32_t v);
void rec_st64(uint64_t a, uint64_t v);
void rec_stq(uint64_t a, const void *r);
void rec_sincosf(float x, float *s, float *c);
#define LD32 rec_ld32
#define LD64 rec_ld64
#define LDF rec_ldf
#define LDQ(r, a) rec_ldq((void *)(r), (a))
#define ST32 rec_st32
#define ST64 rec_st64
#define STQ(a, r) rec_stq((a), (const void *)(r))
#define sincosf(x, s, c) rec_sincosf(x, s, c)
void rec_cover(uint64_t a);
#define COVER(a) rec_cover(a)
