// High-level emulation of Meta's natives: real implementations that replace Prism's logging stubs.
//
// An HLE source file declares each native it implements with PRISM_HLE. The entries land in the
// prism_hle section; when Prism registers a Meta native (prism_jni.c), it uses the HLE entry with
// the same class, name and signature instead of the stub.

#pragma once

#include <android/log.h>
#include <jni.h>

typedef struct {
  const char *cls;  // "horizonos/app/volumetricwindow/spaces/SpaceManagerClient"
  const char *name;
  const char *sig;
  void *fn;
} PrismHle;

#define PRISM_HLE_CAT2(a, b) a##b
#define PRISM_HLE_CAT(a, b) PRISM_HLE_CAT2(a, b)
#define PRISM_HLE(cls_, name_, sig_, fn_)                                                    \
  __attribute__((used, section("prism_hle"), aligned(sizeof(void *))))                      \
  static const PrismHle PRISM_HLE_CAT(prism_hle_, __LINE__) = {cls_, name_, sig_, (void *)(fn_)}

// The HLE entry for a native, or NULL.
const PrismHle *prism_hle_find(const char *cls, const char *name, const char *sig);
