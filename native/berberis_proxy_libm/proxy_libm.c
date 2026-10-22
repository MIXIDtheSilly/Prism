/*
 * The translator's proxy for libm (libberberis_proxy_libm.so, x86_64): the functions it lists run
 * as the PC's own libm instead of translated arm64 code. Digitalis's lists only expf and powf, so
 * the rest of an app's math ran translated: atanf, sincosf and tanf alone were 5% of VrShell's
 * frame thread with the library open, where a call through the proxy costs about as much as the
 * loop around it (2,000,000 calls of each in an arm64 test program: expf through the proxy 205 ns
 * a loop, atanf and tanf translated 1,900, the bare loop 194).
 *
 * The translator loads it in each translated process and calls InitProxyLibrary with a
 * ProxyLibraryBuilder, which this hands the library's name and a table of {name, marshaller}
 * (Digitalis's own proxies have the same shape). For each name the builder looks the function up
 * in the PC's library of that name and makes the guest library's symbol call it, through the
 * marshaller: marshal(function, guest state), the state being the guest's registers, x0-x30 from 0
 * (8 bytes each), v0-v31 from 0x110 (16 bytes each). Arguments come from and results go back to
 * the registers the arm64 calling convention uses.
 *
 * Left out: what compiles to an instruction (sqrt, floor, fabs, fmin...), and lgamma, which sets
 * the PC libm's signgam rather than the app's. errno, as with Digitalis's expf and powf, is the
 * PC libc's, not the app's.
 */
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define X(state, n) ((uint64_t *)((uint8_t *)(state) + 8 * (n)))
#define V(state, n) ((uint8_t *)(state) + 0x110 + 16 * (n))

static void float_float(void *fn, void *state) {
  float x = *(float *)V(state, 0);
  *(float *)V(state, 0) = ((float (*)(float))fn)(x);
}

static void float_float_float(void *fn, void *state) {
  float x = *(float *)V(state, 0), y = *(float *)V(state, 1);
  *(float *)V(state, 0) = ((float (*)(float, float))fn)(x, y);
}

static void double_double(void *fn, void *state) {
  double x = *(double *)V(state, 0);
  *(double *)V(state, 0) = ((double (*)(double))fn)(x);
}

static void double_double_double(void *fn, void *state) {
  double x = *(double *)V(state, 0), y = *(double *)V(state, 1);
  *(double *)V(state, 0) = ((double (*)(double, double))fn)(x, y);
}

// sincosf(x, &sin, &cos), sincos the same in doubles: the pointers in x0 and x1.
static void sincos_float(void *fn, void *state) {
  float x = *(float *)V(state, 0);
  ((void (*)(float, float *, float *))fn)(x, (float *)(uintptr_t)*X(state, 0), (float *)(uintptr_t)*X(state, 1));
}

static void sincos_double(void *fn, void *state) {
  double x = *(double *)V(state, 0);
  ((void (*)(double, double *, double *))fn)(x, (double *)(uintptr_t)*X(state, 0), (double *)(uintptr_t)*X(state, 1));
}

struct known_trampoline {
  const char *name;
  void (*marshal)(void *fn, void *state);
  uint64_t reserved;  // 0 in all of Digitalis's
};

#define F(name) {#name, float_float, 0}
#define FF(name) {#name, float_float_float, 0}
#define D(name) {#name, double_double, 0}
#define DD(name) {#name, double_double_double, 0}

// Sorted by name (strcmp): the builder looks names up by binary search.
static const struct known_trampoline kTrampolines[] = {
    D(acos), F(acosf), D(acosh), F(acoshf), D(asin), F(asinf), D(asinh), F(asinhf), D(atan), DD(atan2),
    FF(atan2f), F(atanf), D(atanh), F(atanhf), D(cbrt), F(cbrtf), D(cos), F(cosf), D(cosh), F(coshf), D(erf),
    D(erfc), F(erfcf), F(erff), D(exp), D(exp2), F(exp2f), F(expf), D(expm1), F(expm1f), DD(fmod), FF(fmodf),
    DD(hypot), FF(hypotf), D(log), D(log10), F(log10f), D(log1p), F(log1pf), D(log2), F(log2f), F(logf),
    DD(pow), FF(powf), DD(remainder), FF(remainderf), D(sin), {"sincos", sincos_double, 0},
    {"sincosf", sincos_float, 0}, F(sinf), D(sinh), F(sinhf), D(tan), F(tanf), D(tanh), F(tanhf), D(tgamma),
    F(tgammaf),
};

// berberis::ProxyLibraryBuilder::Build(const char*, size_t, const KnownTrampoline*, size_t,
// const KnownVariable*), in libberberis_arm64.so.
void _ZN8berberis19ProxyLibraryBuilder5BuildEPKcmPKNS_15KnownTrampolineEmPKNS_13KnownVariableE(
    void *builder, const char *library, size_t trampolines, const struct known_trampoline *trampoline,
    size_t variables, const void *variable);

__attribute__((visibility("default"))) void InitProxyLibrary(void *builder) {
  _ZN8berberis19ProxyLibraryBuilder5BuildEPKcmPKNS_15KnownTrampolineEmPKNS_13KnownVariableE(
      builder, "libm.so", sizeof kTrampolines / sizeof *kTrampolines, kTrampolines, 0, NULL);
}
