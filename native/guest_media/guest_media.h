// AImageReader_setDefaultBufferSize for arm64 code (guest_media.c), done by libprism_jni's
// replacement of AImageReader_new (native/prism_jni/guest_media.c).
#pragma once

// The format that makes AImageReader_new(width, height, format, 0, reader) resize *reader instead.
// No AIMAGE_FORMAT_* has this value ('PRSZ').
#define PRISM_RESIZE_FORMAT 0x5052535a
