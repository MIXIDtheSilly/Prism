// prism.ISpaces: Prism's space service (spaces_prism.c), the space manager's client for processes
// that can't load Meta's arm64 library: system_server's SpaceManagerClient (hle_spaces.c).
//
// Handles are the service's own. A space belongs to the client binder passed when it was created or
// acquired, and is destroyed when that binder dies. A UUID is a java.util.UUID's two halves.
#pragma once

#include <android/binder_ibinder.h>

#define SPACES_SERVICE "prism.spaces"
#define SPACES_DESCRIPTOR "prism.ISpaces"
#define SPACES_CLIENT_DESCRIPTOR "prism.ISpacesClient"

enum { SPACE_REFERENCE, SPACE_VIRTUAL, SPACE_WINDOW };

// What a LOCATE asks for and gets: the bits of the values it returns.
enum { SPACE_POSE = 1, SPACE_BOUNDS = 2, SPACE_ALPHA = 4 };

enum {
  // in:  int32 kind, int32 reference type, int64 parent, bool has uuid, int64 msb, int64 lsb, binder client
  // out: int64 handle (0: failed)
  SPACES_CREATE = FIRST_CALL_TRANSACTION,
  // in:  int32 kind (virtual or window), int64 msb, int64 lsb, binder client
  // out: int64 handle (0: no such space)
  SPACES_ACQUIRE,
  // in:  int64 handle
  SPACES_DESTROY,
  // in:  int64 base, int64 space, int64 time (ns), int32 what
  // out: int32 valid, float[7] pose (x, y, z, w, then position x, y, z), float[3] bounds, float alpha
  SPACES_LOCATE,
  // in:  int64 handle
  // out: bool ok, int64 msb, int64 lsb
  SPACES_TOKEN,
  // in:  int64 handle, int32 what (unused: the pose and bounds always count, and a window's alpha),
  //      float[7] pose, float[3] bounds, float alpha, int64 time (System.nanoTime)
  // out: bool ok
  SPACES_UPDATE,
};
