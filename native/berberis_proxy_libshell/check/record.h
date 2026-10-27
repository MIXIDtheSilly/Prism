// Included first (-include) in the recording builds (the proxy's, with LIFT_RECORD, and fuzz's):
// the lifted code's memory accesses and basic blocks go through these, which record.c logs.
#include <stdint.h>
#define LIFT_MEMORY
uint8_t rec_ld8(uint64_t a);
uint16_t rec_ld16(uint64_t a);
uint32_t rec_ld32(uint64_t a);
uint64_t rec_ld64(uint64_t a);
void rec_ldq(void *r, uint64_t a);
void rec_st8(uint64_t a, uint8_t v);
void rec_st16(uint64_t a, uint16_t v);
void rec_st32(uint64_t a, uint32_t v);
void rec_st64(uint64_t a, uint64_t v);
void rec_stq(uint64_t a, const void *r);
int rec_cas(uint64_t a, void *expected, uint64_t v, int size);
void rec_copy(uint64_t d, uint64_t s, uint64_t n);
void rec_set(uint64_t d, uint8_t c, uint64_t n);
void rec_sincosf(float x, float *s, float *c);
#define LD8 rec_ld8
#define LD16 rec_ld16
#define LD32 rec_ld32
#define LD64 rec_ld64
#define LDQ(r, a) rec_ldq((void *)(r), (a))
#define ST8 rec_st8
#define ST16 rec_st16
#define ST32 rec_st32
#define ST64 rec_st64
#define STQ(a, r) rec_stq((a), (const void *)(r))
// Ordered accesses are the plain ones here (one thread records at a time; replay compares values).
#define LDA8 rec_ld8
#define LDA16 rec_ld16
#define LDA32 rec_ld32
#define LDA64 rec_ld64
#define STL8 rec_st8
#define STL16 rec_st16
#define STL32 rec_st32
#define STL64 rec_st64
#define CAS8V(a, e, v) rec_cas((a), (e), (v), 1)
#define CAS16V(a, e, v) rec_cas((a), (e), (v), 2)
#define CAS32V(a, e, v) rec_cas((a), (e), (v), 4)
#define CAS64V(a, e, v) rec_cas((a), (e), (v), 8)
unsigned __int128 rec_ld128(uint64_t a);
int rec_cas128(uint64_t a, unsigned __int128 e, unsigned __int128 v);
#define LDA128 rec_ld128
#define CAS128 rec_cas128
#define MEMCOPY(d, s, n) rec_copy((d), (s), (n))
#define MEMSET(d, c, n) rec_set((d), (c), (n))
#define sincosf(x, s, c) rec_sincosf(x, s, c)
void rec_cover(uint64_t a);
#define COVER(a) rec_cover(a)
