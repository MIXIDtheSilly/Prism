// Prism's arm64 libprism_guest_media.so: functions Meta's builds of platform libraries have and
// Android's don't, for arm64 code. libprism_jni preloads it into apps globally, where dlsym finds
// it (Meta's compositor looks AImageReader_setDefaultBufferSize up with RTLD_DEFAULT).
#include "guest_media.h"

#include <dlfcn.h>
#include <stdint.h>

typedef int (*ImageReaderNew)(int32_t, int32_t, int32_t, int32_t, void **);

// Meta's libmediandk: resizes the buffers the reader's window hands out. Done on the x86_64 side,
// by libprism_jni's AImageReader_new (native/prism_jni/guest_media.c).
__attribute__((visibility("default"))) int AImageReader_setDefaultBufferSize(void *reader, uint32_t width,
                                                                              uint32_t height) {
  static ImageReaderNew image_reader_new;
  if (!image_reader_new) {
    void *lib = dlopen("libmediandk.so", RTLD_NOW);
    image_reader_new = lib ? (ImageReaderNew)dlsym(lib, "AImageReader_new") : 0;
    if (!image_reader_new) return -10000;  // AMEDIA_ERROR_UNKNOWN
  }
  return image_reader_new((int32_t)width, (int32_t)height, PRISM_RESIZE_FORMAT, 0, (void **)reader);
}
