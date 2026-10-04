// Resizing an AImageReader's buffers for arm64 code (libprism_jni only).
//
// Meta's compositor makes the Android surfaces apps draw panels into (xrCreateSwapchainAndroidSurfaceKHR)
// from AImageReaders, and sizes them to their panels with AImageReader_setDefaultBufferSize, which
// Meta added to its libmediandk. VrShell resizes a panel's surface before binding it to the panel's
// mirror display, which SurfaceFlinger sizes from it. Here libmediandk is Android's, reached through
// the translator's proxy, and has no such function: every panel's surface stays at the size it was
// made with, and VrShell waits for a resize that never comes.
//
// Prism's arm64 library (native/guest_media) defines AImageReader_setDefaultBufferSize for arm64
// code. Arm64 code reaches x86_64 only through the proxies' functions, so it calls the proxy's
// AImageReader_new with a format no caller uses (PRISM_RESIZE_FORMAT), and Prism's replacement of
// that function here resizes the reader instead: it sets its consumer's default buffer size, as
// AImageReader::init does when it makes the reader.

#ifndef PRISM_STUB_LIB

#include "prism_guest.h"
#include "../guest_media/guest_media.h"

#include <android/log.h>
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>

#define TAG "PrismJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

// Android 14's AImageReader (frameworks/av/media/ndk/NdkImageReaderPriv.h), x86_64: offsets of
// mWidth (mHeight follows) and mBufferItemConsumer, as AImageReader::init uses them. Checked before
// use: a plausible size, and a consumer that is libgui's.
enum { READER_WIDTH = 0x30, READER_CONSUMER = 0x80 };
enum { AMEDIA_OK = 0, AMEDIA_ERROR_UNKNOWN = -10000, AMEDIA_ERROR_INVALID_PARAMETER = -10007 };

typedef int (*ImageReaderNew)(int32_t, int32_t, int32_t, int32_t, void **);
static ImageReaderNew g_real_new;

// Whether p points to an object of libgui's (its vtable there): the reader's consumer.
static int libgui_object(void *p) {
  Dl_info info;
  return p && dladdr(*(void **)p, &info) && info.dli_fname && strstr(info.dli_fname, "/libgui.so");
}

static int resize(void *reader, int32_t width, int32_t height) {
  static int (*set_default_size)(void *, uint32_t, uint32_t);
  if (!set_default_size) {
    void *gui = dlopen("libgui.so", RTLD_NOW | RTLD_NOLOAD);
    set_default_size = gui ? (int (*)(void *, uint32_t, uint32_t))dlsym(gui, "_ZN7android12ConsumerBase20setDefaultBufferSizeEjj") : NULL;
  }
  if (!reader || width <= 0 || height <= 0) return AMEDIA_ERROR_INVALID_PARAMETER;
  int32_t *size = (int32_t *)((char *)reader + READER_WIDTH);  // width, then height
  void *consumer = *(void **)((char *)reader + READER_CONSUMER);
  if (!set_default_size || size[0] <= 0 || size[1] <= 0 || size[0] > 16384 || size[1] > 16384 || !libgui_object(consumer)) {
    LOGW("AImageReader_setDefaultBufferSize: this AImageReader isn't laid out as Prism expects (%d x %d, consumer %p)",
         size[0], size[1], consumer);
    return AMEDIA_ERROR_UNKNOWN;
  }
  if (set_default_size(consumer, (uint32_t)width, (uint32_t)height) != 0) return AMEDIA_ERROR_UNKNOWN;
  LOGI("AImageReader %p: buffers %dx%d (were %dx%d)", reader, width, height, size[0], size[1]);
  size[0] = width;  // what acquired buffers are checked against
  size[1] = height;
  return AMEDIA_OK;
}

static int prism_image_reader_new(int32_t width, int32_t height, int32_t format, int32_t max_images, void **reader) {
  if (format == PRISM_RESIZE_FORMAT) return resize((void *)reader, width, height);
  ImageReaderNew real = __atomic_load_n(&g_real_new, __ATOMIC_ACQUIRE);
  return real ? real(width, height, format, max_images, reader) : AMEDIA_ERROR_UNKNOWN;
}

void *prism_media_function(const char *name, void *real) {
  if (strcmp(name, "AImageReader_new")) return NULL;
  if (!__atomic_exchange_n(&g_real_new, (ImageReaderNew)real, __ATOMIC_ACQ_REL))
    LOGI("AImageReader_new for arm64 code: Prism's, which also resizes readers");
  return (void *)prism_image_reader_new;
}

#endif
