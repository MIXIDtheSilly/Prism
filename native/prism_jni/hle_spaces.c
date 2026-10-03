// HLE of SpaceManagerClient (libspacemanager_jni): the spaces volumetric windows are placed in.
//
// On a headset, Meta's library is a client of the perception stack's space manager (xrservice).
// Spaces form a tree: a reference space (RAW: the tracking origin) at the root, virtual and window
// spaces below it, each with a pose relative to its parent, bounds and, for windows, an alpha.
// system_server's VolumetricWindow creates a window space per window, updates it as the window
// moves, and hands its UUID to clients. Queries ask for a space's pose in another space at a time.
//
// Prism keeps the tree in this process. With no head motion to predict, the time is ignored.
// Spaces acquired by UUID are aliases of the space with that UUID in this process; sharing them
// with other processes needs the real space manager, or a Prism service, behind this.

#include "prism_hle.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TAG "PrismSpaces"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

static const char kClient[] = "horizonos/app/volumetricwindow/spaces/SpaceManagerClient";

typedef struct { float x, y, z, w; } Quat;
typedef struct { float x, y, z; } Vec3;
typedef struct { Quat q; Vec3 p; } Pose;

static const Pose kIdentity = {{0, 0, 0, 1}, {0, 0, 0}};

static Quat quat_mul(Quat a, Quat b) {
  return (Quat){a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

static Vec3 rotate(Quat q, Vec3 v) {
  Quat r = quat_mul(quat_mul(q, (Quat){v.x, v.y, v.z, 0}), (Quat){-q.x, -q.y, -q.z, q.w});
  return (Vec3){r.x, r.y, r.z};
}

// a then b: b's pose is relative to a.
static Pose compose(Pose a, Pose b) {
  Vec3 p = rotate(a.q, b.p);
  return (Pose){quat_mul(a.q, b.q), {a.p.x + p.x, a.p.y + p.y, a.p.z + p.z}};
}

static Pose inverse(Pose a) {
  Quat q = {-a.q.x, -a.q.y, -a.q.z, a.q.w};
  Vec3 p = rotate(q, a.p);
  return (Pose){q, {-p.x, -p.y, -p.z}};
}

static Quat normalize(Quat q) {
  float n = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  return n > 1e-6f ? (Quat){q.x / n, q.y / n, q.z / n, q.w / n} : kIdentity.q;
}

// ---- The space tree.
enum { REFERENCE, VIRTUAL, WINDOW };

typedef struct {
  jlong handle;      // 0 = free slot
  int kind;
  jlong parent;      // handle of the parent space (0 for reference spaces)
  jlong alias;       // for spaces acquired by UUID: the space they stand for
  int has_uuid;
  uint64_t msb, lsb;
  Pose pose;         // relative to the parent
  Vec3 extent;
  float alpha;
  uint64_t token_msb, token_lsb;
} Space;

#define MAX_SPACES 1024
#define CLIENT_HANDLE ((jlong)0x5ace)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Space g_spaces[MAX_SPACES];
static jlong g_next_handle = 1;

static Space *find(jlong handle) {
  if (!handle) return NULL;
  for (int i = 0; i < MAX_SPACES; i++)
    if (g_spaces[i].handle == handle) return &g_spaces[i];
  return NULL;
}

// The space a handle stands for, following an acquired alias.
static Space *resolve(jlong handle) {
  Space *s = find(handle);
  return s && s->alias ? find(s->alias) : s;
}

static void new_token(Space *s) {
  arc4random_buf(&s->token_msb, sizeof s->token_msb);
  arc4random_buf(&s->token_lsb, sizeof s->token_lsb);
  s->token_msb = (s->token_msb & ~0xF000ull) | 0x4000ull;  // version 4
  s->token_lsb = (s->token_lsb & ~(3ull << 62)) | (2ull << 62);  // IETF variant
}

static jlong add(int kind, jlong parent, const uint64_t *uuid) {
  for (int i = 0; i < MAX_SPACES; i++) {
    Space *s = &g_spaces[i];
    if (s->handle) continue;
    *s = (Space){.handle = g_next_handle++, .kind = kind, .parent = parent, .pose = kIdentity, .alpha = 1};
    if (uuid) s->has_uuid = 1, s->msb = uuid[0], s->lsb = uuid[1];
    new_token(s);
    return s->handle;
  }
  LOGW("out of spaces (%d)", MAX_SPACES);
  return 0;
}

static int world_pose(const Space *s, Pose *out) {
  Pose pose = kIdentity;
  for (int depth = 0; s; depth++) {
    if (depth > 64) return 0;  // a cycle
    pose = compose(s->pose, pose);
    if (s->kind == REFERENCE) {
      *out = pose;
      return 1;
    }
    s = resolve(s->parent);
  }
  return 0;  // detached: an ancestor was destroyed
}

// The pose of space in base.
static int relative_pose(jlong base, jlong space, Pose *out) {
  Space *b = resolve(base), *s = resolve(space);
  Pose wb, ws;
  if (!b || !s || !world_pose(b, &wb) || !world_pose(s, &ws)) return 0;
  *out = compose(inverse(wb), ws);
  return 1;
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

// ---- The natives. All are static; the first argument after the class is the client handle.
static jlong client_init(JNIEnv *env, jclass c) {
  (void)env, (void)c;
  LOGI("space manager client (in-process HLE)");
  return CLIENT_HANDLE;
}

static jboolean client_is_valid(JNIEnv *env, jclass c, jlong client) {
  (void)env, (void)c;
  return client == CLIENT_HANDLE;
}

static void client_destroy(JNIEnv *env, jclass c, jlong client) {
  (void)env, (void)c, (void)client;
}

static jlong create(JNIEnv *env, int kind, jlong parent, jobject uuid) {
  uint64_t id[2];
  if (uuid && !read_uuid(env, uuid, id)) return 0;
  pthread_mutex_lock(&g_lock);
  jlong handle = (!parent || resolve(parent)) ? add(kind, parent, uuid ? id : NULL) : 0;
  pthread_mutex_unlock(&g_lock);
  return handle;
}

static jlong create_reference(JNIEnv *env, jclass c, jlong client, jint type) {
  (void)c, (void)client, (void)type;  // TYPE_RAW (0) is the only type
  return create(env, REFERENCE, 0, NULL);
}

static jlong create_virtual(JNIEnv *env, jclass c, jlong client, jlong parent) {
  (void)c, (void)client;
  return create(env, VIRTUAL, parent, NULL);
}

static jlong create_virtual_uuid(JNIEnv *env, jclass c, jlong client, jlong parent, jobject uuid) {
  (void)c, (void)client;
  return create(env, VIRTUAL, parent, uuid);
}

static jlong create_window(JNIEnv *env, jclass c, jlong client, jlong parent, jobject uuid) {
  (void)c, (void)client;
  return create(env, WINDOW, parent, uuid);
}

static jlong acquire(JNIEnv *env, int kind, jobject uuid) {
  uint64_t id[2];
  if (!read_uuid(env, uuid, id)) return 0;
  jlong handle = 0;
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < MAX_SPACES; i++) {
    Space *s = &g_spaces[i];
    if (s->handle && !s->alias && s->kind == kind && s->has_uuid && s->msb == id[0] && s->lsb == id[1]) {
      jlong target = s->handle;
      if ((handle = add(kind, 0, id))) find(handle)->alias = target;
      break;
    }
  }
  pthread_mutex_unlock(&g_lock);
  if (!handle) LOGW("no %s space with that UUID in this process", kind == WINDOW ? "window" : "virtual");
  return handle;
}

static jlong acquire_virtual(JNIEnv *env, jclass c, jlong client, jobject uuid) {
  (void)c, (void)client;
  return acquire(env, VIRTUAL, uuid);
}

static jlong acquire_window(JNIEnv *env, jclass c, jlong client, jobject uuid) {
  (void)c, (void)client;
  return acquire(env, WINDOW, uuid);
}

static void destroy(JNIEnv *env, jclass c, jlong handle) {
  (void)env, (void)c;
  pthread_mutex_lock(&g_lock);
  Space *s = find(handle);
  if (s) s->handle = 0;
  pthread_mutex_unlock(&g_lock);
}

static jobject get_pose(JNIEnv *env, jclass c, jlong client, jlong base, jlong space, jlong time) {
  (void)c, (void)client, (void)time;
  Pose pose;
  pthread_mutex_lock(&g_lock);
  int ok = relative_pose(base, space, &pose);
  pthread_mutex_unlock(&g_lock);
  return ok ? new_pose(env, pose) : NULL;
}

static jobject get_bounds(JNIEnv *env, jclass c, jlong client, jlong base, jlong space, jlong time) {
  (void)c, (void)client, (void)base, (void)time;
  pthread_mutex_lock(&g_lock);
  Space *s = resolve(space);
  Vec3 extent = s ? s->extent : (Vec3){0, 0, 0};
  pthread_mutex_unlock(&g_lock);
  return s ? new_extent(env, extent) : NULL;
}

static jfloat get_alpha(JNIEnv *env, jclass c, jlong client, jlong base, jlong space, jlong time) {
  (void)env, (void)c, (void)client, (void)base, (void)time;
  pthread_mutex_lock(&g_lock);
  Space *s = resolve(space);
  float alpha = s ? s->alpha : -1;  // negative: unknown, which Java turns into null
  pthread_mutex_unlock(&g_lock);
  return alpha;
}

static jobject get_token(JNIEnv *env, jclass c, jlong space) {
  (void)c;
  pthread_mutex_lock(&g_lock);
  Space *s = resolve(space);
  uint64_t msb = s ? s->token_msb : 0, lsb = s ? s->token_lsb : 0;
  pthread_mutex_unlock(&g_lock);
  return s ? new_uuid(env, msb, lsb) : NULL;
}

static jboolean update(JNIEnv *env, jlong space, jobject pose_obj, jobject extent_obj, float alpha) {
  Pose pose;
  Vec3 extent;
  int has_pose = read_pose(env, pose_obj, &pose), has_extent = read_extent(env, extent_obj, &extent);
  pthread_mutex_lock(&g_lock);
  Space *s = resolve(space);
  if (s) {
    if (has_pose) s->pose = pose;
    if (has_extent) s->extent = extent;
    if (alpha >= 0) s->alpha = alpha;
    new_token(s);
  }
  pthread_mutex_unlock(&g_lock);
  return s ? JNI_TRUE : JNI_FALSE;
}

static jboolean update_space(JNIEnv *env, jclass c, jlong space, jobject pose, jobject extent, jlong time) {
  (void)c, (void)time;
  return update(env, space, pose, extent, -1);
}

static jboolean update_window(JNIEnv *env, jclass c, jlong space, jobject pose, jobject extent, jfloat alpha,
                              jlong time) {
  (void)c, (void)time;
  return update(env, space, pose, extent, alpha);
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
