// Calling arm64 (guest) code from Prism's x86_64 code, through the translator (libprism_jni only).
#pragma once

// Whether fn is arm64 code the translator runs.
int prism_is_guest_code(const void *fn);

// A host-callable trampoline to the arm64 C function fn, or NULL if fn isn't arm64 code. shorty is
// JNI's: the return type, then each argument ('J' for pointers and 64-bit values, 'I' for int).
void *prism_guest_function(void *fn, const char *shorty);

// Prism's replacement for the x86_64 function name (real) that the translator binds for arm64
// callers, or NULL to keep real (guest_window.c).
void *prism_window_function(const char *name, void *real);

// The same, for AImageReader_new (guest_media.c).
void *prism_media_function(const char *name, void *real);

// Patches the arm64 library just loaded from path, if Prism patches it (guest_patch.c).
void prism_patch_guest_library(const char *path);
