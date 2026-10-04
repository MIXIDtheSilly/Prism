/*
 * Prism's space service: prism.ISpaces (spaces_protocol.h), registered as prism.spaces.
 *
 * Meta's space manager (the perception stack's, in xrservice) keeps the spaces volumetric windows
 * are placed in, shared between processes by UUID: system_server creates a window space per window
 * and VrShell acquires it to place the window's panel. Its client library (libhzos_spaces.meta.so)
 * is arm64, and system_server can't load it; this arm64 daemon is the client on its behalf
 * (native/prism_jni/hle_spaces.c), the way libspacemanager_jni is on a headset.
 *
 * Handles are this service's. Each space belongs to the client binder that created or acquired
 * it, and is destroyed when that binder dies.
 */
#include "spaces_protocol.h"
#include "hzu_spaces.h"

#include <android/binder_parcel.h>
#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAG "PrismSpaces"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define WARN(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define ERR(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define HZU_LIB "libhzos_spaces.meta.so"

// ---- Meta's client library.
static struct {
  HzuResult (*manager_create)(HzuSpaceManager *);
  HzuResult (*enumerate_references)(HzuSpaceManager, HzuReferenceSpaces *);
  HzuResult (*create_reference)(HzuSpaceManager, const HzuReferenceSpaceCreateInfo *, HzuSpace *);
  HzuResult (*create_virtual)(HzuSpaceManager, const HzuSpaceCreateInfo *, HzuSpace *);
  HzuResult (*create_window)(HzuSpaceManager, const HzuSpaceCreateInfo *, HzuSpace *);
  HzuResult (*virtual_from_uuid)(HzuSpaceManager, const HzuSpaceFromUuidInfo *, HzuSpace *);
  HzuResult (*window_from_uuid)(HzuSpaceManager, const HzuSpaceFromUuidInfo *, HzuSpace *);
  HzuResult (*locate)(HzuSpaceManager, const HzuSpaceLocateInfo *, HzuBaseSpaceData *, HzuSpaceMetaData *);
  void (*destroy)(HzuSpace);
  HzuResult (*virtual_update)(HzuSpace, const HzuBaseSpaceData *, const HzuVirtualSpaceData *);
  HzuResult (*window_update)(HzuSpace, const HzuBaseSpaceData *, const HzuWindowSpaceData *);
  HzuResult (*virtual_token)(HzuSpace, HzuUpdateToken *);
  HzuResult (*window_token)(HzuSpace, HzuUpdateToken *);
} hzu;

static int load_hzu(void) {
  void *lib = dlopen(HZU_LIB, RTLD_NOW);
  if (!lib) {
    ERR("%s", dlerror());
    return 0;
  }
  const struct { void **fn; const char *name; } symbols[] = {
      {(void **)&hzu.manager_create, "HzuSpaceManager_create"},
      {(void **)&hzu.enumerate_references, "HzuSpaceManager_enumerateReferenceSpaces"},
      {(void **)&hzu.create_reference, "HzuSpaceManager_createReferenceSpace"},
      {(void **)&hzu.create_virtual, "HzuSpaceManager_createVirtualSpace"},
      {(void **)&hzu.create_window, "HzuSpaceManager_createWindowSpace"},
      {(void **)&hzu.virtual_from_uuid, "HzuSpaceManager_createVirtualSpaceFromUuid"},
      {(void **)&hzu.window_from_uuid, "HzuSpaceManager_createWindowSpaceFromUuid"},
      {(void **)&hzu.locate, "HzuSpaceManager_locateSpace2"},
      {(void **)&hzu.destroy, "HzuSpace_destroy"},
      {(void **)&hzu.virtual_update, "HzuVirtualSpace_update"},
      {(void **)&hzu.window_update, "HzuWindowSpace_update"},
      {(void **)&hzu.virtual_token, "HzuVirtualSpace_getUpdateToken"},
      {(void **)&hzu.window_token, "HzuWindowSpace_getUpdateToken"},
  };
  for (size_t i = 0; i < sizeof symbols / sizeof *symbols; i++) {
    if (!(*symbols[i].fn = dlsym(lib, symbols[i].name))) {
      ERR("%s: no %s", HZU_LIB, symbols[i].name);
      return 0;
    }
  }
  return 1;
}

// A UUID's bytes are canonical: the Java UUID's most significant half first, big-endian.
static HzuSpaceUuid to_uuid(uint64_t msb, uint64_t lsb) {
  HzuSpaceUuid u;
  for (int i = 0; i < 8; i++) {
    u.bytes[i] = (uint8_t)(msb >> (56 - 8 * i));
    u.bytes[8 + i] = (uint8_t)(lsb >> (56 - 8 * i));
  }
  return u;
}

static void from_uuid(const HzuSpaceUuid *u, uint64_t *msb, uint64_t *lsb) {
  *msb = *lsb = 0;
  for (int i = 0; i < 8; i++) {
    *msb = *msb << 8 | u->bytes[i];
    *lsb = *lsb << 8 | u->bytes[8 + i];
  }
}

// ---- The spaces. Every call into Meta's library happens under the lock.
typedef struct {
  int64_t handle;  // 0: a free slot
  int kind;
  HzuSpace space;
  AIBinder *owner;
} Entry;

#define MAX_SPACES 1024
#define MAX_OWNERS 64

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static HzuSpaceManager g_manager;
static Entry g_spaces[MAX_SPACES];
static int64_t g_next_handle = 1;
static AIBinder *g_owners[MAX_OWNERS];  // clients whose death this service follows
static AIBinder_DeathRecipient *g_death;

// A manager creates reference spaces only once it has listed them.
static HzuSpaceManager manager(void) {
  static bool listed;
  if (!g_manager) {
    HzuResult r = hzu.manager_create(&g_manager);
    if (r != HZU_SUCCESS || !g_manager) {
      ERR("HzuSpaceManager_create: %d", r);
      g_manager = NULL;
      return NULL;
    }
  }
  if (!listed) {
    HzuReferenceSpaceType types[8];
    HzuReferenceSpaces list = {.version = HZU_VERSION_LEGACY, .capacity = 8, .types = types};
    HzuResult r = hzu.enumerate_references(g_manager, &list);
    if (r != HZU_SUCCESS) {
      ERR("HzuSpaceManager_enumerateReferenceSpaces: %d", r);
      return NULL;
    }
    LOG("space manager ready: %u reference spaces", list.count);
    listed = true;
  }
  return g_manager;
}

static Entry *find(int64_t handle) {
  if (!handle) return NULL;
  for (int i = 0; i < MAX_SPACES; i++)
    if (g_spaces[i].handle == handle) return &g_spaces[i];
  return NULL;
}

static HzuSpace space_of(int64_t handle) {
  Entry *e = find(handle);
  return e ? e->space : NULL;
}

static void drop(Entry *e) {
  hzu.destroy(e->space);
  AIBinder_decStrong(e->owner);
  *e = (Entry){0};
}

static void on_owner_died(void *cookie) {
  pthread_mutex_lock(&g_lock);
  int n = 0;
  for (int i = MAX_SPACES - 1; i >= 0; i--)  // the newest first: children before their parents
    if (g_spaces[i].handle && g_spaces[i].owner == cookie) drop(&g_spaces[i]), n++;
  for (int i = 0; i < MAX_OWNERS; i++)
    if (g_owners[i] == cookie) AIBinder_decStrong(g_owners[i]), g_owners[i] = NULL;
  pthread_mutex_unlock(&g_lock);
  LOG("a client died: destroyed its %d spaces", n);
}

// Follows the owner's death, once per owner. libbinder_ndk hands out one AIBinder per remote object.
static void follow(AIBinder *owner) {
  int free_slot = -1;
  for (int i = 0; i < MAX_OWNERS; i++) {
    if (g_owners[i] == owner) return;
    if (!g_owners[i] && free_slot < 0) free_slot = i;
  }
  if (free_slot < 0) {
    WARN("too many clients to follow");
    return;
  }
  if (AIBinder_linkToDeath(owner, g_death, owner) != STATUS_OK) return;
  AIBinder_incStrong(owner);
  g_owners[free_slot] = owner;
}

static int64_t add(int kind, HzuSpace space, AIBinder *owner) {
  for (int i = 0; i < MAX_SPACES; i++) {
    Entry *e = &g_spaces[i];
    if (e->handle) continue;
    follow(owner);
    AIBinder_incStrong(owner);
    *e = (Entry){g_next_handle++, kind, space, owner};
    return e->handle;
  }
  WARN("out of spaces (%d)", MAX_SPACES);
  hzu.destroy(space);
  return 0;
}

static int64_t create(int kind, int type, int64_t parent, bool has_uuid, uint64_t msb, uint64_t lsb, AIBinder *owner) {
  HzuSpaceManager m = manager();
  if (!m) return 0;
  HzuSpace space = NULL;
  HzuResult r;
  if (kind == SPACE_REFERENCE) {
    HzuReferenceSpaceCreateInfo info = {.version = HZU_VERSION_LEGACY, .type = type};
    r = hzu.create_reference(m, &info, &space);
  } else {
    HzuSpace p = space_of(parent);
    if (parent && !p) return 0;
    HzuSpaceCreateInfo info = {.parent = p, .uuid = to_uuid(msb, lsb)};
    if (kind == SPACE_WINDOW) {
      info.version = HZU_VERSION_DATA;
      r = hzu.create_window(m, &info, &space);
    } else {
      info.version = has_uuid ? HZU_VERSION_VIRTUAL_UUID : HZU_VERSION_LEGACY;
      r = hzu.create_virtual(m, &info, &space);
    }
  }
  if (r != HZU_SUCCESS || !space) {
    WARN("creating a %s space failed: %d", kind == SPACE_WINDOW ? "window" : kind == SPACE_VIRTUAL ? "virtual" : "reference", r);
    return 0;
  }
  return add(kind, space, owner);
}

static int64_t acquire(int kind, uint64_t msb, uint64_t lsb, AIBinder *owner) {
  HzuSpaceManager m = manager();
  if (!m) return 0;
  HzuSpaceFromUuidInfo info = {.uuid = to_uuid(msb, lsb)};
  HzuSpace space = NULL;
  HzuResult r;
  if (kind == SPACE_WINDOW) {
    info.version = HZU_VERSION_DATA;
    r = hzu.window_from_uuid(m, &info, &space);
  } else {
    info.version = HZU_VERSION_LEGACY;
    r = hzu.virtual_from_uuid(m, &info, &space);
  }
  if (r != HZU_SUCCESS || !space) return 0;
  return add(kind, space, owner);
}

typedef struct {
  int32_t valid;
  HzuPose pose;
  HzuExtent3f bounds;
  float alpha;
} Located;

// As Meta's JNI library reads them: a pose whenever the query succeeds; a window's bounds from its
// metadata when set there, other bounds from the base data; alpha only when set.
static void locate(int64_t base, int64_t space, int64_t time, int what, Located *out) {
  *out = (Located){.pose = {{0, 0, 0, 1}, {0, 0, 0}}, .alpha = -1};
  HzuSpaceManager m = manager();
  Entry *e = find(space);
  HzuSpace b = space_of(base);
  if (!m || !e || !b) return;
  bool window = e->kind == SPACE_WINDOW && (what & (SPACE_BOUNDS | SPACE_ALPHA));
  HzuSpaceLocateInfo info = {.version = HZU_VERSION_LEGACY, .space = e->space, .baseSpace = b, .time = time};
  HzuBaseSpaceData data = {.version = HZU_VERSION_DATA};
  HzuSpaceMetaData meta = {.version = HZU_VERSION_DATA, .type = HZU_META_WINDOW};
  HzuResult r = hzu.locate(m, &info, &data, window ? &meta : NULL);
  if (r != HZU_SUCCESS) return;
  out->valid = SPACE_POSE | SPACE_BOUNDS;
  out->pose = data.pose;
  out->bounds = window && (meta.flags & HZU_META_BOUNDS_VALID) ? meta.bounds : data.legacyBounds;
  if (window && (meta.flags & HZU_META_ALPHA_VALID)) out->valid |= SPACE_ALPHA, out->alpha = meta.alpha;
}

// As Meta's JNI library writes them: pose and bounds always (identity and zero when Java has none),
// and a window's alpha.
static bool update(int64_t handle, const float v[11], int64_t time) {
  Entry *e = find(handle);
  if (!e || e->kind == SPACE_REFERENCE) return false;
  HzuBaseSpaceData data = {.version = HZU_VERSION_DATA, .time = time, .flags = HZU_POSE_VALID_MASK,
                           .pose = {{v[0], v[1], v[2], v[3]}, {v[4], v[5], v[6]}}};
  HzuExtent3f bounds = {v[7], v[8], v[9]};
  HzuResult r;
  if (e->kind == SPACE_WINDOW) {
    HzuWindowSpaceData w = {.version = HZU_VERSION_DATA, .type = HZU_META_WINDOW,
                            .flags = HZU_META_BOUNDS_VALID | HZU_META_ALPHA_VALID, .bounds = bounds, .alpha = v[10]};
    r = hzu.window_update(e->space, &data, &w);
  } else {
    HzuVirtualSpaceData d = {.version = HZU_VERSION_DATA, .type = HZU_META_VIRTUAL, .flags = HZU_META_BOUNDS_VALID,
                             .bounds = bounds};
    r = hzu.virtual_update(e->space, &data, &d);
  }
  if (r != HZU_SUCCESS) WARN("updating space %lld failed: %d", (long long)handle, r);
  return r == HZU_SUCCESS;
}

static bool token(int64_t handle, uint64_t *msb, uint64_t *lsb) {
  Entry *e = find(handle);
  if (!e || e->kind == SPACE_REFERENCE) return false;
  HzuUpdateToken t;
  HzuResult r = e->kind == SPACE_WINDOW ? hzu.window_token(e->space, &t) : hzu.virtual_token(e->space, &t);
  if (r != HZU_SUCCESS) return false;
  from_uuid(&t, msb, lsb);
  return true;
}

// ---- The binder service.
static binder_status_t read_floats(const AParcel *in, float *v, int n) {
  binder_status_t st = STATUS_OK;
  for (int i = 0; i < n && st == STATUS_OK; i++) st = AParcel_readFloat(in, &v[i]);
  return st;
}

static binder_status_t on_transact(AIBinder *service, transaction_code_t code, const AParcel *in, AParcel *out) {
  (void)service;
  int32_t kind = 0, type = 0, what = 0;
  int64_t a = 0, b = 0, time = 0, msb = 0, lsb = 0;
  bool has_uuid = false;
  AIBinder *owner = NULL;
  float v[11];
  binder_status_t st;
  switch (code) {
    case SPACES_CREATE:
      st = AParcel_readInt32(in, &kind);
      if (st == STATUS_OK) st = AParcel_readInt32(in, &type);
      if (st == STATUS_OK) st = AParcel_readInt64(in, &a);
      if (st == STATUS_OK) st = AParcel_readBool(in, &has_uuid);
      if (st == STATUS_OK) st = AParcel_readInt64(in, &msb);
      if (st == STATUS_OK) st = AParcel_readInt64(in, &lsb);
      if (st == STATUS_OK) st = AParcel_readStrongBinder(in, &owner);
      if (st == STATUS_OK && !owner) st = STATUS_UNEXPECTED_NULL;
      if (st == STATUS_OK) {
        pthread_mutex_lock(&g_lock);
        int64_t handle = create(kind, type, a, has_uuid, (uint64_t)msb, (uint64_t)lsb, owner);
        pthread_mutex_unlock(&g_lock);
        st = AParcel_writeInt64(out, handle);
      }
      break;
    case SPACES_ACQUIRE:
      st = AParcel_readInt32(in, &kind);
      if (st == STATUS_OK) st = AParcel_readInt64(in, &msb);
      if (st == STATUS_OK) st = AParcel_readInt64(in, &lsb);
      if (st == STATUS_OK) st = AParcel_readStrongBinder(in, &owner);
      if (st == STATUS_OK && !owner) st = STATUS_UNEXPECTED_NULL;
      if (st == STATUS_OK) {
        pthread_mutex_lock(&g_lock);
        int64_t handle = acquire(kind, (uint64_t)msb, (uint64_t)lsb, owner);
        pthread_mutex_unlock(&g_lock);
        st = AParcel_writeInt64(out, handle);
      }
      break;
    case SPACES_DESTROY:
      st = AParcel_readInt64(in, &a);
      if (st == STATUS_OK) {
        pthread_mutex_lock(&g_lock);
        Entry *e = find(a);
        if (e) drop(e);
        pthread_mutex_unlock(&g_lock);
      }
      break;
    case SPACES_LOCATE: {
      st = AParcel_readInt64(in, &a);
      if (st == STATUS_OK) st = AParcel_readInt64(in, &b);
      if (st == STATUS_OK) st = AParcel_readInt64(in, &time);
      if (st == STATUS_OK) st = AParcel_readInt32(in, &what);
      if (st != STATUS_OK) break;
      Located l;
      pthread_mutex_lock(&g_lock);
      locate(a, b, time, what, &l);
      pthread_mutex_unlock(&g_lock);
      const float r[11] = {l.pose.orientation.x, l.pose.orientation.y, l.pose.orientation.z, l.pose.orientation.w,
                           l.pose.position.x, l.pose.position.y, l.pose.position.z,
                           l.bounds.width, l.bounds.height, l.bounds.depth, l.alpha};
      st = AParcel_writeInt32(out, l.valid);
      for (int i = 0; i < 11 && st == STATUS_OK; i++) st = AParcel_writeFloat(out, r[i]);
      break;
    }
    case SPACES_TOKEN: {
      st = AParcel_readInt64(in, &a);
      if (st != STATUS_OK) break;
      uint64_t hi = 0, lo = 0;
      pthread_mutex_lock(&g_lock);
      bool ok = token(a, &hi, &lo);
      pthread_mutex_unlock(&g_lock);
      st = AParcel_writeBool(out, ok);
      if (st == STATUS_OK) st = AParcel_writeInt64(out, (int64_t)hi);
      if (st == STATUS_OK) st = AParcel_writeInt64(out, (int64_t)lo);
      break;
    }
    case SPACES_UPDATE:
      st = AParcel_readInt64(in, &a);
      if (st == STATUS_OK) st = AParcel_readInt32(in, &what);
      if (st == STATUS_OK) st = read_floats(in, v, 11);
      if (st == STATUS_OK) st = AParcel_readInt64(in, &time);
      if (st == STATUS_OK) {
        pthread_mutex_lock(&g_lock);
        bool ok = update(a, v, time);
        pthread_mutex_unlock(&g_lock);
        st = AParcel_writeBool(out, ok);
      }
      break;
    default:
      return STATUS_UNKNOWN_TRANSACTION;
  }
  if (owner) AIBinder_decStrong(owner);
  return st;
}

static void *on_create(void *args) { return args; }
static void on_destroy(void *data) { (void)data; }

int main(void) {
  // Not in the NDK's headers: the platform's service manager and thread pool functions.
  void *ndk = dlopen("libbinder_ndk.so", RTLD_NOW);
  binder_exception_t (*add_service)(AIBinder *, const char *) = ndk ? dlsym(ndk, "AServiceManager_addService") : NULL;
  bool (*set_max_threads)(uint32_t) = ndk ? dlsym(ndk, "ABinderProcess_setThreadPoolMaxThreadCount") : NULL;
  void (*start_pool)(void) = ndk ? dlsym(ndk, "ABinderProcess_startThreadPool") : NULL;
  void (*join_pool)(void) = ndk ? dlsym(ndk, "ABinderProcess_joinThreadPool") : NULL;
  if (!add_service || !set_max_threads || !start_pool || !join_pool || !load_hzu()) {
    ERR("can't start: missing platform or space manager functions");
    return 1;
  }
  g_death = AIBinder_DeathRecipient_new(on_owner_died);
  set_max_threads(4);
  AIBinder_Class *cls = AIBinder_Class_define(SPACES_DESCRIPTOR, on_create, on_destroy, on_transact);
  AIBinder *service = cls ? AIBinder_new(cls, NULL) : NULL;
  binder_exception_t ex = service ? add_service(service, SPACES_SERVICE) : EX_NULL_POINTER;
  if (ex != EX_NONE) {
    ERR("registering %s failed: %d", SPACES_SERVICE, ex);
    return 1;
  }
  LOG("serving %s", SPACES_SERVICE);
  start_pool();
  join_pool();
  return 0;
}
