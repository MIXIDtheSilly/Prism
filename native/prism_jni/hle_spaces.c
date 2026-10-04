// HLE of SpaceManagerClient (libspacemanager_jni): the spaces volumetric windows are placed in.
//
// On a headset, Meta's library is a client of the perception stack's space manager (xrservice).
// Spaces form a tree: a reference space (RAW: the tracking origin) at the root, virtual and window
// spaces below it, each with a pose relative to its parent, bounds and, for windows, an alpha.
// system_server's VolumetricWindow creates a window space per window, updates it as the window
// moves, and hands its UUID to other processes, which acquire the space by it: VrShell places each
// panel in its window's space.
//
// Meta's library is arm64, and system_server runs x86_64 code only. Prism's space service
// (native/spaces_prism), an arm64 daemon, is the space manager's client instead, and these natives
// call it over binder (spaces_protocol.h). The spaces are the space manager's, shared by all.

#include "prism_hle.h"

#include "../spaces_prism/spaces_protocol.h"

#include <android/binder_parcel.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TAG "PrismSpaces"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

static const char kClient[] = "horizonos/app/volumetricwindow/spaces/SpaceManagerClient";

typedef struct { float x, y, z, w; } Quat;
typedef struct { float x, y, z; } Vec3;
typedef struct { Quat q; Vec3 p; } Pose;

static const Pose kIdentity = {{0, 0, 0, 1}, {0, 0, 0}};

#define CLIENT_HANDLE ((jlong)0x5ace)

static Quat normalize(Quat q) {
  float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  return n > 1e-6f ? (Quat){q.x / n, q.y / n, q.z / n, q.w / n} : kIdentity.q;
}


// ---- Java objects: horizonos.graphics.Pose(Vector4f orientation, Vector3f position), Vector4f(x, y,
// z, w), Vector3f(x, y, z), Extent3f(width, height, depth); java.util.UUID.
static struct {
  int ready;
  jclass pose, vec4, vec3, extent, uuid;
  jmethodID pose_init, vec4_init, vec3_init, extent_init, uuid_init, uuid_msb, uuid_lsb, uuid_string;
  jfieldID pose_q, pose_p, v4[4], v3[3], ext[3];
} J;

static jclass global_class(JNIEnv *env, const char *name) {
  jclass local = (*env)->FindClass(env, name);
  if (!local) return NULL;
  jclass global = (jclass)(*env)->NewGlobalRef(env, local);
  (*env)->DeleteLocalRef(env, local);
  return global;
}

static int java_ready(JNIEnv *env) {
  if (J.ready) return 1;
  // FindClass resolves through the class loader of SpaceManagerClient, the caller.
  if (!(J.pose = global_class(env, "horizonos/graphics/Pose")) ||
      !(J.vec4 = global_class(env, "horizonos/graphics/Vector4f")) ||
      !(J.vec3 = global_class(env, "horizonos/graphics/Vector3f")) ||
      !(J.extent = global_class(env, "horizonos/graphics/Extent3f")) ||
      !(J.uuid = global_class(env, "java/util/UUID")))
    return 0;
  J.pose_init = (*env)->GetMethodID(env, J.pose, "<init>", "(Lhorizonos/graphics/Vector4f;Lhorizonos/graphics/Vector3f;)V");
  J.vec4_init = (*env)->GetMethodID(env, J.vec4, "<init>", "(FFFF)V");
  J.vec3_init = (*env)->GetMethodID(env, J.vec3, "<init>", "(FFF)V");
  J.extent_init = (*env)->GetMethodID(env, J.extent, "<init>", "(FFF)V");
  J.uuid_init = (*env)->GetMethodID(env, J.uuid, "<init>", "(JJ)V");
  J.uuid_msb = (*env)->GetMethodID(env, J.uuid, "getMostSignificantBits", "()J");
  J.uuid_lsb = (*env)->GetMethodID(env, J.uuid, "getLeastSignificantBits", "()J");
  J.uuid_string = (*env)->GetMethodID(env, J.uuid, "toString", "()Ljava/lang/String;");
  J.pose_q = (*env)->GetFieldID(env, J.pose, "mOrientation", "Lhorizonos/graphics/Vector4f;");
  J.pose_p = (*env)->GetFieldID(env, J.pose, "mPosition", "Lhorizonos/graphics/Vector3f;");
  static const char *const xyzw[] = {"mX", "mY", "mZ", "mW"};
  static const char *const whd[] = {"mWidth", "mHeight", "mDepth"};
  for (int i = 0; i < 4; i++) J.v4[i] = (*env)->GetFieldID(env, J.vec4, xyzw[i], "F");
  for (int i = 0; i < 3; i++) J.v3[i] = (*env)->GetFieldID(env, J.vec3, xyzw[i], "F");
  for (int i = 0; i < 3; i++) J.ext[i] = (*env)->GetFieldID(env, J.extent, whd[i], "F");
  if ((*env)->ExceptionCheck(env)) return 0;
  J.ready = 1;
  return 1;
}

static int read_uuid(JNIEnv *env, jobject uuid, uint64_t out[2]) {
  if (!uuid || !java_ready(env)) return 0;
  out[0] = (uint64_t)(*env)->CallLongMethod(env, uuid, J.uuid_msb);
  out[1] = (uint64_t)(*env)->CallLongMethod(env, uuid, J.uuid_lsb);
  return !(*env)->ExceptionCheck(env);
}

static jobject new_uuid(JNIEnv *env, uint64_t msb, uint64_t lsb) {
  return java_ready(env) ? (*env)->NewObject(env, J.uuid, J.uuid_init, (jlong)msb, (jlong)lsb) : NULL;
}

static int read_pose(JNIEnv *env, jobject pose, Pose *out) {
  if (!pose || !java_ready(env)) return 0;
  jobject q = (*env)->GetObjectField(env, pose, J.pose_q), p = (*env)->GetObjectField(env, pose, J.pose_p);
  if (!q || !p) return 0;
  float v[4];
  for (int i = 0; i < 4; i++) v[i] = (*env)->GetFloatField(env, q, J.v4[i]);
  out->q = normalize((Quat){v[0], v[1], v[2], v[3]});
  for (int i = 0; i < 3; i++) v[i] = (*env)->GetFloatField(env, p, J.v3[i]);
  out->p = (Vec3){v[0], v[1], v[2]};
  (*env)->DeleteLocalRef(env, q);
  (*env)->DeleteLocalRef(env, p);
  return 1;
}

static jobject new_pose(JNIEnv *env, Pose pose) {
  if (!java_ready(env)) return NULL;
  jobject q = (*env)->NewObject(env, J.vec4, J.vec4_init, pose.q.x, pose.q.y, pose.q.z, pose.q.w);
  jobject p = (*env)->NewObject(env, J.vec3, J.vec3_init, pose.p.x, pose.p.y, pose.p.z);
  if (!q || !p) return NULL;
  jobject out = (*env)->NewObject(env, J.pose, J.pose_init, q, p);
  (*env)->DeleteLocalRef(env, q);
  (*env)->DeleteLocalRef(env, p);
  return out;
}

static int read_extent(JNIEnv *env, jobject extent, Vec3 *out) {
  if (!extent || !java_ready(env)) return 0;
  out->x = (*env)->GetFloatField(env, extent, J.ext[0]);
  out->y = (*env)->GetFloatField(env, extent, J.ext[1]);
  out->z = (*env)->GetFloatField(env, extent, J.ext[2]);
  return 1;
}

static jobject new_extent(JNIEnv *env, Vec3 e) {
  return java_ready(env) ? (*env)->NewObject(env, J.extent, J.extent_init, e.x, e.y, e.z) : NULL;
}

// ---- The service.
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static AIBinder_Class *g_service_class;
static AIBinder *g_service;  // NULL until found, and again once it dies
static AIBinder *g_client;   // this process's token: its spaces go when it dies

static binder_status_t no_transactions(AIBinder *b, transaction_code_t code, const AParcel *in, AParcel *out) {
  (void)b, (void)code, (void)in, (void)out;
  return STATUS_UNKNOWN_TRANSACTION;
}

static void *on_create(void *args) { return args; }
static void on_destroy(void *data) { (void)data; }

// The service with a reference for the caller, or NULL.
static AIBinder *service(void) {
  static AIBinder *(*check_service)(const char *);
  pthread_mutex_lock(&g_lock);
  if (g_service && !AIBinder_isAlive(g_service)) {
    LOGW("%s died; its spaces are gone", SPACES_SERVICE);
    AIBinder_decStrong(g_service);
    g_service = NULL;
  }
  if (!g_service_class) {
    // Not in the NDK's headers: the platform's service manager functions.
    void *ndk = dlopen("libbinder_ndk.so", RTLD_NOW);
    check_service = ndk ? (AIBinder * (*)(const char *)) dlsym(ndk, "AServiceManager_checkService") : NULL;
    g_service_class = AIBinder_Class_define(SPACES_DESCRIPTOR, on_create, on_destroy, no_transactions);
    AIBinder_Class *client = AIBinder_Class_define(SPACES_CLIENT_DESCRIPTOR, on_create, on_destroy, no_transactions);
    g_client = client ? AIBinder_new(client, NULL) : NULL;
  }
  if (!g_service && check_service && g_client) {
    AIBinder *b = check_service(SPACES_SERVICE);
    if (b && AIBinder_associateClass(b, g_service_class)) g_service = b;
    else if (b) AIBinder_decStrong(b);
  }
  AIBinder *s = g_service;
  if (s) AIBinder_incStrong(s);
  pthread_mutex_unlock(&g_lock);
  return s;
}

// A transaction: begin() gives the parcel to write the arguments to, end() sends it (unless a write
// failed) and returns the reply, which the caller deletes, or NULL.
static AIBinder *begin(AParcel **in) {
  AIBinder *s = service();
  if (!s) {
    static int warned;
    if (!warned++) LOGW("no %s: spaces fail until it runs", SPACES_SERVICE);
    return NULL;
  }
  *in = NULL;
  if (AIBinder_prepareTransaction(s, in) != STATUS_OK) {
    AIBinder_decStrong(s);
    return NULL;
  }
  return s;
}

static AParcel *end(AIBinder *s, transaction_code_t code, AParcel *in, binder_status_t written) {
  AParcel *out = NULL;
  binder_status_t status = written;
  if (status == STATUS_OK) status = AIBinder_transact(s, code, &in, &out, 0);
  if (in) AParcel_delete(in);
  AIBinder_decStrong(s);
  if (status != STATUS_OK) {
    LOGW("%s transaction %u failed: %d", SPACES_SERVICE, code - FIRST_CALL_TRANSACTION, status);
    if (out) AParcel_delete(out);
    return NULL;
  }
  return out;
}

static binder_status_t write_uuid(AParcel *in, const uint64_t id[2]) {
  binder_status_t st = AParcel_writeInt64(in, (int64_t)id[0]);
  return st == STATUS_OK ? AParcel_writeInt64(in, (int64_t)id[1]) : st;
}

static jlong read_handle(AParcel *out) {
  int64_t handle = 0;
  if (out && AParcel_readInt64(out, &handle) != STATUS_OK) handle = 0;
  if (out) AParcel_delete(out);
  return handle;
}

static jlong create(JNIEnv *env, int kind, int type, jlong parent, jobject uuid) {
  uint64_t id[2] = {0, 0};
  if (uuid && !read_uuid(env, uuid, id)) return 0;
  AParcel *in;
  AIBinder *s = begin(&in);
  if (!s) return 0;
  binder_status_t st = AParcel_writeInt32(in, kind);
  if (st == STATUS_OK) st = AParcel_writeInt32(in, type);
  if (st == STATUS_OK) st = AParcel_writeInt64(in, parent);
  if (st == STATUS_OK) st = AParcel_writeBool(in, uuid != NULL);
  if (st == STATUS_OK) st = write_uuid(in, id);
  if (st == STATUS_OK) st = AParcel_writeStrongBinder(in, g_client);
  return read_handle(end(s, SPACES_CREATE, in, st));
}

static jlong acquire(JNIEnv *env, int kind, jobject uuid) {
  uint64_t id[2];
  if (!read_uuid(env, uuid, id)) return 0;
  AParcel *in;
  AIBinder *s = begin(&in);
  if (!s) return 0;
  binder_status_t st = AParcel_writeInt32(in, kind);
  if (st == STATUS_OK) st = write_uuid(in, id);
  if (st == STATUS_OK) st = AParcel_writeStrongBinder(in, g_client);
  return read_handle(end(s, SPACES_ACQUIRE, in, st));
}

typedef struct {
  int32_t valid;  // SPACE_POSE | SPACE_BOUNDS | SPACE_ALPHA
  Pose pose;
  Vec3 bounds;
  float alpha;
} Located;

static int locate(jlong base, jlong space, jlong time, int what, Located *out) {
  AParcel *in;
  AIBinder *s = begin(&in);
  if (!s) return 0;
  binder_status_t st = AParcel_writeInt64(in, base);
  if (st == STATUS_OK) st = AParcel_writeInt64(in, space);
  if (st == STATUS_OK) st = AParcel_writeInt64(in, time);
  if (st == STATUS_OK) st = AParcel_writeInt32(in, what);
  AParcel *reply = end(s, SPACES_LOCATE, in, st);
  if (!reply) return 0;
  float v[11];
  st = AParcel_readInt32(reply, &out->valid);
  for (int i = 0; i < 11 && st == STATUS_OK; i++) st = AParcel_readFloat(reply, &v[i]);
  AParcel_delete(reply);
  if (st != STATUS_OK) return 0;
  out->pose = (Pose){{v[0], v[1], v[2], v[3]}, {v[4], v[5], v[6]}};
  out->bounds = (Vec3){v[7], v[8], v[9]};
  out->alpha = v[10];
  return (out->valid & what) == what;
}

// ---- The natives. All are static; the first argument after the class is the client handle.
static jlong client_init(JNIEnv *env, jclass c) {
  (void)env, (void)c;
  LOGI("space manager client (through %s)", SPACES_SERVICE);
  return CLIENT_HANDLE;
}

static jboolean client_is_valid(JNIEnv *env, jclass c, jlong client) {
  (void)env, (void)c;
  return client == CLIENT_HANDLE;
}

static void client_destroy(JNIEnv *env, jclass c, jlong client) {
  (void)env, (void)c, (void)client;
}

static jlong create_reference(JNIEnv *env, jclass c, jlong client, jint type) {
  (void)c, (void)client;
  return create(env, SPACE_REFERENCE, type, 0, NULL);
}

static jlong create_virtual(JNIEnv *env, jclass c, jlong client, jlong parent) {
  (void)c, (void)client;
  return create(env, SPACE_VIRTUAL, 0, parent, NULL);
}

static jlong create_virtual_uuid(JNIEnv *env, jclass c, jlong client, jlong parent, jobject uuid) {
  (void)c, (void)client;
  return create(env, SPACE_VIRTUAL, 0, parent, uuid);
}

static jlong create_window(JNIEnv *env, jclass c, jlong client, jlong parent, jobject uuid) {
  (void)c, (void)client;
  return create(env, SPACE_WINDOW, 0, parent, uuid);
}

static jlong acquire_virtual(JNIEnv *env, jclass c, jlong client, jobject uuid) {
  (void)c, (void)client;
  return acquire(env, SPACE_VIRTUAL, uuid);
}

static jlong acquire_window(JNIEnv *env, jclass c, jlong client, jobject uuid) {
  (void)c, (void)client;
  return acquire(env, SPACE_WINDOW, uuid);
}

static void destroy(JNIEnv *env, jclass c, jlong handle) {
  (void)env, (void)c;
  AParcel *in;
  AIBinder *s = handle ? begin(&in) : NULL;
  if (!s) return;
  AParcel *out = end(s, SPACES_DESTROY, in, AParcel_writeInt64(in, handle));
  if (out) AParcel_delete(out);
}

static jobject get_pose(JNIEnv *env, jclass c, jlong client, jlong base, jlong space, jlong time) {
  (void)c, (void)client;
  Located l;
  return locate(base, space, time, SPACE_POSE, &l) ? new_pose(env, l.pose) : NULL;
}

static jobject get_bounds(JNIEnv *env, jclass c, jlong client, jlong base, jlong space, jlong time) {
  (void)c, (void)client;
  Located l;
  return locate(base, space, time, SPACE_BOUNDS, &l) ? new_extent(env, l.bounds) : NULL;
}

static jfloat get_alpha(JNIEnv *env, jclass c, jlong client, jlong base, jlong space, jlong time) {
  (void)env, (void)c, (void)client;
  Located l;
  return locate(base, space, time, SPACE_ALPHA, &l) ? l.alpha : -1;  // negative: unknown, which Java turns into null
}

static jobject get_token(JNIEnv *env, jclass c, jlong space) {
  (void)c;
  AParcel *in;
  AIBinder *s = begin(&in);
  if (!s) return NULL;
  AParcel *out = end(s, SPACES_TOKEN, in, AParcel_writeInt64(in, space));
  if (!out) return NULL;
  bool ok = false;
  int64_t msb = 0, lsb = 0;
  binder_status_t st = AParcel_readBool(out, &ok);
  if (st == STATUS_OK) st = AParcel_readInt64(out, &msb);
  if (st == STATUS_OK) st = AParcel_readInt64(out, &lsb);
  AParcel_delete(out);
  return st == STATUS_OK && ok ? new_uuid(env, (uint64_t)msb, (uint64_t)lsb) : NULL;
}

// Missing values are identity and zero, as Meta's library sends them.
static jboolean update(JNIEnv *env, jlong space, jobject pose_obj, jobject extent_obj, float alpha, jlong time) {
  Pose pose = kIdentity;
  Vec3 extent = {0, 0, 0};
  read_pose(env, pose_obj, &pose);
  read_extent(env, extent_obj, &extent);
  int what = SPACE_POSE | SPACE_BOUNDS | SPACE_ALPHA;
  AParcel *in;
  AIBinder *s = begin(&in);
  if (!s) return JNI_FALSE;
  const float v[11] = {pose.q.x, pose.q.y, pose.q.z, pose.q.w, pose.p.x, pose.p.y, pose.p.z,
                       extent.x, extent.y, extent.z, alpha};
  binder_status_t st = AParcel_writeInt64(in, space);
  if (st == STATUS_OK) st = AParcel_writeInt32(in, what);
  for (int i = 0; i < 11 && st == STATUS_OK; i++) st = AParcel_writeFloat(in, v[i]);
  if (st == STATUS_OK) st = AParcel_writeInt64(in, time);
  AParcel *out = end(s, SPACES_UPDATE, in, st);
  bool ok = false;
  if (out && AParcel_readBool(out, &ok) != STATUS_OK) ok = false;
  if (out) AParcel_delete(out);
  return ok ? JNI_TRUE : JNI_FALSE;
}

static jboolean update_space(JNIEnv *env, jclass c, jlong space, jobject pose, jobject extent, jlong time) {
  (void)c;
  return update(env, space, pose, extent, -1, time);
}

static jboolean update_window(JNIEnv *env, jclass c, jlong space, jobject pose, jobject extent, jfloat alpha,
                              jlong time) {
  (void)c;
  return update(env, space, pose, extent, alpha, time);
}

static jstring uuid_to_string(JNIEnv *env, jclass c, jobject uuid) {
  (void)c;
  return uuid && java_ready(env) ? (jstring)(*env)->CallObjectMethod(env, uuid, J.uuid_string) : NULL;
}

#define UUID "Ljava/util/UUID;"
#define POSE "Lhorizonos/graphics/Pose;"
#define EXTENT "Lhorizonos/graphics/Extent3f;"
PRISM_HLE(kClient, "nativeInit", "()J", client_init);
PRISM_HLE(kClient, "nativeIsValid", "(J)Z", client_is_valid);
PRISM_HLE(kClient, "nativeDestroy", "(J)V", client_destroy);
PRISM_HLE(kClient, "nativeSpaceCreateReference", "(JI)J", create_reference);
PRISM_HLE(kClient, "nativeSpaceCreateVirtual", "(JJ)J", create_virtual);
PRISM_HLE(kClient, "nativeSpaceCreateVirtualWithUuid", "(JJ" UUID ")J", create_virtual_uuid);
PRISM_HLE(kClient, "nativeSpaceAcquireFromUuid", "(J" UUID ")J", acquire_virtual);
PRISM_HLE(kClient, "nativeSpaceDestroy", "(J)V", destroy);
PRISM_HLE(kClient, "nativeSpaceGetPose", "(JJJJ)" POSE, get_pose);
PRISM_HLE(kClient, "nativeSpaceGetBounds", "(JJJJ)" EXTENT, get_bounds);
PRISM_HLE(kClient, "nativeSpaceGetUpdateToken", "(J)" UUID, get_token);
PRISM_HLE(kClient, "nativeSpaceUpdate", "(J" POSE EXTENT "J)Z", update_space);
PRISM_HLE(kClient, "nativeTestUuidToString", "(" UUID ")Ljava/lang/String;", uuid_to_string);
PRISM_HLE(kClient, "nativeWindowSpaceCreate", "(JJ" UUID ")J", create_window);
PRISM_HLE(kClient, "nativeWindowSpaceAcquireFromUuid", "(J" UUID ")J", acquire_window);
PRISM_HLE(kClient, "nativeWindowSpaceDestroy", "(J)V", destroy);
PRISM_HLE(kClient, "nativeWindowSpaceGetPose", "(JJJJ)" POSE, get_pose);
PRISM_HLE(kClient, "nativeWindowSpaceGetBounds", "(JJJJ)" EXTENT, get_bounds);
PRISM_HLE(kClient, "nativeWindowSpaceGetAlpha", "(JJJJ)F", get_alpha);
PRISM_HLE(kClient, "nativeWindowSpaceGetUpdateToken", "(J)" UUID, get_token);
PRISM_HLE(kClient, "nativeWindowSpaceUpdate", "(J" POSE EXTENT "FJ)Z", update_window);
