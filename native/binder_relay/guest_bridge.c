/*
 * Binder objects between arm64 code and Java, in app processes (libprism_binder_bridge.so, arm64).
 *
 * An app's arm64 code uses Horizon's libbinder (one copy per process, preloaded by libprism_jni), a
 * /dev/binder connection of its own: to the binder driver it's a different process from the one
 * Java's (x86_64) binder runs in. So Horizon's AIBinder_toJavaBinder and AIBinder_fromJavaBinder,
 * which hand the object itself to libandroid_runtime, can't work: the object belongs to the other
 * connection. Here an object crosses through Prism's binder relay (binder_relay.c) instead, as a
 * transaction: the side that has it puts it in the relay for a token, and the other side takes it
 * out, as a reference of its own to the same object. Taking out an object of one's own gives back
 * that object, so a round trip ends where it began.
 *
 * Loaded into the default arm64 namespace before any app library, after libbinder_ndk; it points
 * libbinder_ndk's two functions at its own, which reach Java through JNI.
 *
 * Surfaces cross the same way, as their producer binder. The runtime's compositor-side panel
 * surfaces are host (x86_64) ImageReader windows, which Meta's arm64 RuntimeIpcManager would treat
 * as arm64 Surfaces to hand their producer to the app; and an app's arm64 Surface for one would go
 * to the host's ANativeWindow_toSurface as it is. Both hooks below move the producer instead.
 */
#include <android/binder_ibinder.h>
#include <android/binder_ibinder_jni.h>
#include <android/binder_parcel.h>
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TAG "PrismBinderBridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

// The relay (binder_relay.c).
#define RELAY_NAME "prism.binder_relay"
#define RELAY_DESCRIPTOR "prism.IBinderRelay"
#define RELAY_PUT (FIRST_CALL_TRANSACTION + 0)
#define RELAY_TAKE (FIRST_CALL_TRANSACTION + 1)

// ---- The arm64 side: Horizon's libbinder_ndk.
static AIBinder *g_relay;

static void *relay_create(void *args) { return args; }
static void relay_destroy(void *data) { (void)data; }
static binder_status_t relay_transact(AIBinder *binder, transaction_code_t code, const AParcel *in, AParcel *out) {
  (void)binder, (void)code, (void)in, (void)out;
  return STATUS_UNKNOWN_TRANSACTION;
}

static void connect_relay(void) {
  // Not in the NDK's headers: the platform's service manager functions.
  AIBinder *(*check_service)(const char *) =
      (AIBinder * (*)(const char *)) dlsym(RTLD_DEFAULT, "AServiceManager_checkService");
  AIBinder *relay = check_service ? check_service(RELAY_NAME) : NULL;
  if (!relay) {
    LOGW("no %s", RELAY_NAME);
    return;
  }
  AIBinder_Class *clazz = AIBinder_Class_define(RELAY_DESCRIPTOR, relay_create, relay_destroy, relay_transact);
  if (!clazz || !AIBinder_associateClass(relay, clazz)) {
    LOGW("%s isn't a %s", RELAY_NAME, RELAY_DESCRIPTOR);
    AIBinder_decStrong(relay);
    return;
  }
  g_relay = relay;
}

// The relay, looked up again until it's there: an app can start before it.
static AIBinder *relay(void) {
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock(&lock);
  if (!g_relay) connect_relay();
  pthread_mutex_unlock(&lock);
  return g_relay;
}

// Puts an arm64 side object in the relay; 0 if it can't.
static int64_t guest_put(AIBinder *binder) {
  AIBinder *r = relay();
  AParcel *in = NULL, *out = NULL;
  int64_t token = 0;
  if (r && AIBinder_prepareTransaction(r, &in) == STATUS_OK && AParcel_writeStrongBinder(in, binder) == STATUS_OK &&
      AIBinder_transact(r, RELAY_PUT, &in, &out, 0) == STATUS_OK)
    AParcel_readInt64(out, &token);
  if (in) AParcel_delete(in);
  if (out) AParcel_delete(out);
  return token;
}

// Takes an object out of the relay, as a strong reference of the arm64 side's.
static AIBinder *guest_take(int64_t token) {
  AIBinder *r = relay(), *binder = NULL;
  AParcel *in = NULL, *out = NULL;
  if (r && AIBinder_prepareTransaction(r, &in) == STATUS_OK && AParcel_writeInt64(in, token) == STATUS_OK &&
      AIBinder_transact(r, RELAY_TAKE, &in, &out, 0) == STATUS_OK)
    AParcel_readStrongBinder(out, &binder);
  if (in) AParcel_delete(in);
  if (out) AParcel_delete(out);
  return binder;
}

// ---- The Java side, through JNI: android.os.ServiceManager and Parcel, as the relay's Java clients.
static struct {
  jobject relay;  // the relay's IBinder, a global reference
  jclass parcel;
  jmethodID obtain, recycle, write_token, write_long, write_binder, read_long, read_binder, transact;
} g_java;
static pthread_mutex_t g_java_lock = PTHREAD_MUTEX_INITIALIZER;

// Whether the last JNI call threw; the exception is logged and cleared.
static int threw(JNIEnv *env, const char *what) {
  if (!(*env)->ExceptionCheck(env)) return 0;
  LOGW("%s threw", what);
  (*env)->ExceptionDescribe(env);
  (*env)->ExceptionClear(env);
  return 1;
}

#define JAVA_CHECK(what) do { if (threw(env, what)) goto done; } while (0)
#define JAVA_METHOD(field, cls, name, sig) \
  g_java.field = (*env)->GetMethodID(env, cls, name, sig); \
  JAVA_CHECK(name); if (!g_java.field) goto done

static int java_ready(JNIEnv *env) {
  pthread_mutex_lock(&g_java_lock);
  int frame = 0;
  if (!g_java.relay && !(*env)->ExceptionCheck(env)) {
    if ((*env)->PushLocalFrame(env, 16) != JNI_OK) { threw(env, "relay local frame"); goto done; }
    frame = 1;
    jclass sm = (*env)->FindClass(env, "android/os/ServiceManager");
    JAVA_CHECK("ServiceManager class"); if (!sm) goto done;
    jmethodID check = (*env)->GetStaticMethodID(env, sm, "checkService", "(Ljava/lang/String;)Landroid/os/IBinder;");
    JAVA_CHECK("ServiceManager.checkService lookup"); if (!check) goto done;
    jclass parcel = (*env)->FindClass(env, "android/os/Parcel");
    JAVA_CHECK("Parcel class"); if (!parcel) goto done;
    jclass ibinder = (*env)->FindClass(env, "android/os/IBinder");
    JAVA_CHECK("IBinder class"); if (!ibinder) goto done;
    g_java.obtain = (*env)->GetStaticMethodID(env, parcel, "obtain", "()Landroid/os/Parcel;");
    JAVA_CHECK("Parcel.obtain lookup"); if (!g_java.obtain) goto done;
    JAVA_METHOD(recycle, parcel, "recycle", "()V");
    JAVA_METHOD(write_token, parcel, "writeInterfaceToken", "(Ljava/lang/String;)V");
    JAVA_METHOD(write_long, parcel, "writeLong", "(J)V");
    JAVA_METHOD(write_binder, parcel, "writeStrongBinder", "(Landroid/os/IBinder;)V");
    JAVA_METHOD(read_long, parcel, "readLong", "()J");
    JAVA_METHOD(read_binder, parcel, "readStrongBinder", "()Landroid/os/IBinder;");
    JAVA_METHOD(transact, ibinder, "transact", "(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z");
    jstring name = (*env)->NewStringUTF(env, RELAY_NAME);
    JAVA_CHECK("relay name"); if (!name) goto done;
    jobject relay = (*env)->CallStaticObjectMethod(env, sm, check, name);
    JAVA_CHECK("ServiceManager.checkService");
    if (!relay) { LOGW("Java: no %s", RELAY_NAME); goto done; }
    g_java.parcel = (*env)->NewGlobalRef(env, parcel);
    JAVA_CHECK("Parcel global reference"); if (!g_java.parcel) goto done;
    g_java.relay = (*env)->NewGlobalRef(env, relay);
    JAVA_CHECK("relay global reference");
  }
done:
  if (!g_java.relay && g_java.parcel) {
    (*env)->DeleteGlobalRef(env, g_java.parcel);
    threw(env, "discarding Parcel class");
    g_java.parcel = NULL;
  }
  if (frame) { (*env)->PopLocalFrame(env, NULL); threw(env, "relay local frame"); }
  int ready = g_java.relay != NULL;
  pthread_mutex_unlock(&g_java_lock);
  return ready;
}

// One transaction with the relay from Java: writes the token and then a binder (put) or a token
// (take); reads the reply's token (put) or binder (take, a local reference).
static int java_transact(JNIEnv *env, int code, jobject binder, jlong token, jlong *token_out, jobject *binder_out) {
  if ((*env)->ExceptionCheck(env) || !java_ready(env)) return 0;
  jobject in = NULL, out = NULL, result = NULL;
  int ok = 0;
  if ((*env)->PushLocalFrame(env, 8) != JNI_OK) { threw(env, "transaction local frame"); return 0; }
  in = (*env)->CallStaticObjectMethod(env, g_java.parcel, g_java.obtain);
  JAVA_CHECK("Parcel.obtain request"); if (!in) goto done;
  out = (*env)->CallStaticObjectMethod(env, g_java.parcel, g_java.obtain);
  JAVA_CHECK("Parcel.obtain reply"); if (!out) goto done;
  {
    jstring descriptor = (*env)->NewStringUTF(env, RELAY_DESCRIPTOR);
    JAVA_CHECK("relay descriptor"); if (!descriptor) goto done;
    (*env)->CallVoidMethod(env, in, g_java.write_token, descriptor);
    JAVA_CHECK("relay interface token");
    if (code == RELAY_PUT)
      (*env)->CallVoidMethod(env, in, g_java.write_binder, binder);
    else
      (*env)->CallVoidMethod(env, in, g_java.write_long, token);
    if (!threw(env, "writing the relay's request")) {
      jboolean sent = (*env)->CallBooleanMethod(env, g_java.relay, g_java.transact, code, in, out, 0);
      if (!threw(env, "IBinder.transact") && sent) {
        if (code == RELAY_PUT)
          *token_out = (*env)->CallLongMethod(env, out, g_java.read_long);
        else
          result = (*env)->CallObjectMethod(env, out, g_java.read_binder);
        ok = !threw(env, "reading the relay's reply");
      }
    }
  }
done:
  if (out) { (*env)->CallVoidMethod(env, out, g_java.recycle); if (threw(env, "reply recycle")) ok = 0; }
  if (in) { (*env)->CallVoidMethod(env, in, g_java.recycle); if (threw(env, "request recycle")) ok = 0; }
  result = (*env)->PopLocalFrame(env, ok ? result : NULL);
  if (threw(env, "transaction local frame")) ok = 0;
  if (binder_out) *binder_out = result;
  return ok;
}

#undef JAVA_METHOD
#undef JAVA_CHECK

// ---- libbinder_ndk's functions, as Prism has them.
static jobject to_java_binder(JNIEnv *env, AIBinder *binder) {
  if (!binder) return NULL;
  int64_t token = guest_put(binder);
  jobject java = NULL;
  if (!token || !java_transact(env, RELAY_TAKE, NULL, token, NULL, &java) || !java)
    LOGW("AIBinder_toJavaBinder: the object didn't cross");
  return java;
}

static AIBinder *from_java_binder(JNIEnv *env, jobject java) {
  if (!java) return NULL;
  jlong token = 0;
  AIBinder *binder = java_transact(env, RELAY_PUT, java, 0, &token, NULL) && token ? guest_take(token) : NULL;
  if (!binder) LOGW("AIBinder_fromJavaBinder: the object didn't cross");
  return binder;
}

// ---- Surfaces: only their producer binder crosses; the native objects stay on their own side.
static jobject (*g_host_surface)(JNIEnv *, void *);
static int32_t (*g_native_surface)(void *, const uint8_t *, int32_t, void *);
static int32_t (*g_set_binder)(void *, const uint8_t *, int32_t, void *);

typedef struct {
  int magic, version;
  void *reserved[4];
  void *inc_ref, *dec_ref;
} WindowHead;

static int guest_window(void *window) {
  const WindowHead *w = window;
  Dl_info info;
  // ImageReader's proxy returns the host window unchanged. The extracted constructors put
  // host libgui+13ec40 or guest libgui+12cfe0 in incRef. The host address is outside the guest
  // linker's maps. This is the guest-side counterpart of guest_window.c's ownership check.
  return w && w->magic == 0x5f776e64 && w->inc_ref && dladdr(w->inc_ref, &info);
}

// C++ sp<> returns use x8 even though the object contains just one pointer. C's struct return
// would use x0. Keep that ABI adjustment here, with both the call and the object in arm64 code.
__attribute__((naked)) static void call_sp(void *object, void *result, void *function) {
  __asm__("mov x8, x1\n br x2");
}

static AIBinder *window_binder(void *window) {
  void *gui = dlopen("libgui.so", RTLD_NOW | RTLD_NOLOAD);
  void *binder = dlopen("libbinder.so", RTLD_NOW | RTLD_NOLOAD);
  void *ndk = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_NOLOAD);
  void *utils = dlopen("libutils.so", RTLD_NOW | RTLD_NOLOAD);
  void *get = gui ? dlsym(gui, "_ZNK7android7Surface25getIGraphicBufferProducerEv") : NULL;
  void *as = binder ? dlsym(binder, "_ZN7android10IInterface8asBinderERKNS_2spIS0_EE") : NULL;
  AIBinder *(*from)(void *) = ndk ? dlsym(ndk, "_Z27AIBinder_fromPlatformBinderRKN7android2spINS_7IBinderEEE") : NULL;
  void (*dec)(void *, const void *) = utils ? dlsym(utils, "_ZNK7android7RefBase9decStrongEPKv") : NULL;
  void *producer = NULL, *platform = NULL;
  AIBinder *out = NULL;
  if (get && as && from && dec) {
    call_sp((char *)window - 0x10, &producer, get);
    if (producer) call_sp(&producer, &platform, as);
    if (platform) out = from(&platform);
    // RefBase is a virtual base; the guest vtable supplies its displacement, as in setNativeSurface.
    if (platform) dec((char *)platform + ((ptrdiff_t *)*(void **)platform)[-3], &platform);
    if (producer) dec((char *)producer + ((ptrdiff_t *)*(void **)producer)[-3], &producer);
  }
  if (utils) dlclose(utils);
  if (ndk) dlclose(ndk);
  if (binder) dlclose(binder);
  if (gui) dlclose(gui);
  return out;
}

// All temporary JNI references live in the caller's local frame. Lookups and calls stop at the
// first exception, including allocation failures, before any further JNI work is attempted.
#define JNI_CHECK(what) do { if (threw(env, what)) { result = NULL; goto done; } } while (0)
#define METHOD(var, cls, name, sig) \
  jmethodID var = (*env)->GetMethodID(env, cls, name, sig); \
  JNI_CHECK(name); if (!var) goto done

static jobject surface_parcel(JNIEnv *env, jobject surface, jobject producer) {
  jobject parcel = NULL, result = NULL;
  jmethodID recycle = NULL, release = NULL;
  jclass pc = (*env)->FindClass(env, "android/os/Parcel");
  JNI_CHECK("Parcel class"); if (!pc) goto done;
  jclass sc = (*env)->FindClass(env, "android/view/Surface");
  JNI_CHECK("Surface class"); if (!sc) goto done;
  jmethodID obtain = (*env)->GetStaticMethodID(env, pc, "obtain", "()Landroid/os/Parcel;");
  JNI_CHECK("Parcel.obtain lookup"); if (!obtain) goto done;
  recycle = (*env)->GetMethodID(env, pc, "recycle", "()V");
  JNI_CHECK("Parcel.recycle lookup"); if (!recycle) goto done;
  METHOD(position, pc, "setDataPosition", "(I)V");
  release = (*env)->GetMethodID(env, sc, "release", "()V");
  JNI_CHECK("Surface.release lookup"); if (!release) goto done;
  parcel = (*env)->CallStaticObjectMethod(env, pc, obtain);
  JNI_CHECK("Parcel.obtain"); if (!parcel) goto done;
  if (surface) {
    METHOD(write, sc, "writeToParcel", "(Landroid/os/Parcel;I)V");
    METHOD(read_string, pc, "readString", "()Ljava/lang/String;");
    METHOD(read_int, pc, "readInt", "()I");
    METHOD(read_binder, pc, "readStrongBinder", "()Landroid/os/IBinder;");
    (*env)->CallVoidMethod(env, surface, write, parcel, 0);
    JNI_CHECK("Surface.writeToParcel");
    (*env)->CallVoidMethod(env, parcel, position, 0);
    JNI_CHECK("Parcel.setDataPosition");
    (*env)->CallObjectMethod(env, parcel, read_string);
    JNI_CHECK("Surface name");
    (*env)->CallIntMethod(env, parcel, read_int);
    JNI_CHECK("Surface single buffer");
    jint magic = (*env)->CallIntMethod(env, parcel, read_int);
    JNI_CHECK("Surface queue magic"); if (magic != 0x62717565) goto done;
    result = (*env)->CallObjectMethod(env, parcel, read_binder);
    JNI_CHECK("Surface producer");
    (*env)->CallObjectMethod(env, parcel, read_binder);
    JNI_CHECK("Surface control handle");
  } else {
    METHOD(write_string, pc, "writeString", "(Ljava/lang/String;)V");
    METHOD(write_int, pc, "writeInt", "(I)V");
    METHOD(write_binder, pc, "writeStrongBinder", "(Landroid/os/IBinder;)V");
    METHOD(construct, sc, "<init>", "()V");
    METHOD(read, sc, "readFromParcel", "(Landroid/os/Parcel;)V");
    METHOD(valid, sc, "isValid", "()Z");
    jstring name = (*env)->NewStringUTF(env, "Prism compositor panel");
    JNI_CHECK("Surface name allocation"); if (!name) goto done;
    (*env)->CallVoidMethod(env, parcel, write_string, name);
    JNI_CHECK("Surface name write");
    (*env)->CallVoidMethod(env, parcel, write_int, 0);
    JNI_CHECK("Surface single buffer write");
    (*env)->CallVoidMethod(env, parcel, write_int, 0x62717565);
    JNI_CHECK("Surface queue magic write");
    (*env)->CallVoidMethod(env, parcel, write_binder, producer);
    JNI_CHECK("Surface producer write");
    (*env)->CallVoidMethod(env, parcel, write_binder, NULL);
    JNI_CHECK("Surface control handle write");
    (*env)->CallVoidMethod(env, parcel, position, 0);
    JNI_CHECK("Parcel.setDataPosition");
    surface = (*env)->NewObject(env, sc, construct);
    JNI_CHECK("Surface constructor"); if (!surface) goto done;
    (*env)->CallVoidMethod(env, surface, read, parcel);
    JNI_CHECK("Surface.readFromParcel");
    jboolean ok = (*env)->CallBooleanMethod(env, surface, valid);
    JNI_CHECK("Surface.isValid"); if (!ok) goto done;
    result = surface;
  }
done:
  if (parcel) {
    (*env)->CallVoidMethod(env, parcel, recycle);
    if (threw(env, "Parcel.recycle")) result = NULL;
  }
  if (surface && surface != result && release) {
    (*env)->CallVoidMethod(env, surface, release);
    if (threw(env, "Surface.release")) result = NULL;
  }
  return result;
}

static jobject window_to_surface(JNIEnv *env, void *window) {
  if (!window || !guest_window(window)) return g_host_surface(env, window);
  jobject result = NULL;
  if ((*env)->ExceptionCheck(env)) {
    LOGW("client Surface conversion entered with a pending exception");
  } else if ((*env)->PushLocalFrame(env, 32) == JNI_OK) {
    AIBinder *binder = window_binder(window);
    jobject producer = binder ? to_java_binder(env, binder) : NULL;
    if (binder) AIBinder_decStrong(binder);
    if (!threw(env, "guest producer to Java") && producer) result = surface_parcel(env, NULL, producer);
    result = (*env)->PopLocalFrame(env, result);
    if (threw(env, "client local frame")) result = NULL;
  } else {
    threw(env, "client local frame allocation");
  }
  LOGI("client guest window -> host Java Surface: %s", result ? "converted" : "failed");
  if (result) return result;
  LOGW("client Surface conversion failed, using libandroid proxy");
  return g_host_surface(env, window);
}

static int32_t set_native_surface(void *manager, const uint8_t *key, int32_t length, void *window) {
  if (!window || guest_window(window)) return g_native_surface(manager, key, length, window);
  // The manager stores its guest JavaVM here; unlike its cached JNIEnv it is safe on any thread.
  JavaVM *vm = *(JavaVM **)((char *)manager + 0x88);
  if (!vm) {
    // Native-only RuntimeIPC setup can leave the manager's VM unset. libnativehelper is the
    // guest JNI proxy, so the VM it returns has the guest invocation table, not host addresses.
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static jint (*get_vms)(JavaVM **, jsize, jsize *);
    pthread_mutex_lock(&lock);
    if (!get_vms) {
      void *helper = dlopen("libnativehelper.so", RTLD_NOW);
      get_vms = helper ? dlsym(helper, "JNI_GetCreatedJavaVMs") : NULL;
      if (helper && !get_vms) dlclose(helper);
      // Keep the proxy loaded while its VM and invocation functions are used.
    }
    jsize count = 0;
    if (!get_vms || get_vms(&vm, 1, &count) != JNI_OK || count != 1) vm = NULL;
    pthread_mutex_unlock(&lock);
  }
  JNIEnv *env = NULL;
  int attached = 0;
  AIBinder *binder = NULL;
  if (vm) {
    jint status = (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6);
    if (status == JNI_EDETACHED && (*vm)->AttachCurrentThread(vm, &env, NULL) == JNI_OK) attached = 1;
    else if (status != JNI_OK) env = NULL;
  }
  if (env && !(*env)->ExceptionCheck(env)) {
    if ((*env)->PushLocalFrame(env, 32) == JNI_OK) {
      jobject surface = g_host_surface(env, window);
      if (!threw(env, "host window to Java") && surface) {
        jobject producer = surface_parcel(env, surface, NULL);
        if (producer) binder = from_java_binder(env, producer);
        if (threw(env, "host producer to guest") && binder) AIBinder_decStrong(binder), binder = NULL;
      }
      (*env)->PopLocalFrame(env, NULL);
      if (threw(env, "runtime local frame") && binder) AIBinder_decStrong(binder), binder = NULL;
    } else threw(env, "runtime local frame allocation");
  }
  if (attached && (*vm)->DetachCurrentThread(vm) != JNI_OK) LOGW("runtime Surface thread detach failed");
  LOGI("runtime host window -> guest producer: %s", binder ? "converted" : "failed");
  if (!binder) {
    LOGW("runtime Surface conversion failed, using original setNativeSurface");
    return g_native_surface(manager, key, length, window);
  }
  int32_t result = g_set_binder(manager, key, length, binder);
  AIBinder_decStrong(binder);
  return result;
}

#undef METHOD
#undef JNI_CHECK

// Points fn at target: its first instructions become a jump (ldr x16, #8; br x16; .quad target).
static int redirect(void *fn, void *target) {
  uint32_t jump[4] = {0x58000050, 0xd61f0200, (uint32_t)(uintptr_t)target, (uint32_t)((uintptr_t)target >> 32)};
  uintptr_t page_size = (uintptr_t)getpagesize();
  uintptr_t start = (uintptr_t)fn & ~(page_size - 1), end = ((uintptr_t)fn + sizeof jump + page_size - 1) & ~(page_size - 1);
  if (mprotect((void *)start, end - start, PROT_READ | PROT_WRITE | PROT_EXEC)) return 0;
  memcpy(fn, jump, sizeof jump);
  mprotect((void *)start, end - start, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)fn, (char *)fn + sizeof jump);
  return 1;
}

static void install_surfaces(void) {
  void *android = dlopen("libandroid.so", RTLD_NOW);
  uint32_t *entry = android ? dlsym(android, "ANativeWindow_toSurface") : NULL;
  // The translator entry is b <proxy stub>, followed by padding: in the built libandroid it is
  // 1ba80 -> 1ba40 and the next entry is 1bb00. Never overwrite a short neighbouring stub.
  if (!entry || (entry[0] & 0xfc000000) != 0x14000000 || entry[1] || entry[2] || entry[3]) {
    LOGW("Surface hooks: libandroid entry isn't a padded proxy branch");
    return;
  }
  int32_t displacement = (int32_t)(entry[0] << 6) >> 4;
  g_host_surface = (void *)((char *)entry + displacement);
  if (!redirect(entry, (void *)window_to_surface)) {
    LOGW("can't redirect ANativeWindow_toSurface");
    return;
  }
  LOGI("ANativeWindow_toSurface dispatches by window ownership");

  const char *library = "libosndk.libripcmanager.lazy.so";
  void *manager = dlopen(library, RTLD_NOW | RTLD_NOLOAD);
  // This bridge is preloaded before the runtime (VrDriver's processes: the runtime service hosts
  // the compositor). Loading the lazy manager now there ensures its very first export is hooked,
  // without a polling race or changing other apps' dlopen behaviour. A loaded manager is also
  // handled in other processes.
  if (!manager) {
    static const char *const runtimes[] = {"com.oculus.vrruntimeservice", "com.oculus.systemdriver"};
    char process[256] = {0};
    FILE *cmdline = fopen("/proc/self/cmdline", "r");
    if (cmdline) { fread(process, 1, sizeof process - 1, cmdline); fclose(cmdline); }
    for (size_t i = 0; !manager && i < sizeof runtimes / sizeof *runtimes; i++) {
      size_t n = strlen(runtimes[i]);
      if (!strncmp(process, runtimes[i], n) && (!process[n] || process[n] == ':')) manager = dlopen(library, RTLD_NOW);
    }
    if (!manager) return;
  }
  if (!manager) {
    LOGW("can't load the runtime Surface manager: %s", dlerror());
    return;
  }
  void *native = dlsym(manager, "_ZN5OSSDK17RuntimeIpcManager21RuntimeIpcManagerImpl16setNativeSurfaceEPKhiPv");
  g_set_binder = dlsym(manager, "_ZN5OSSDK17RuntimeIpcManager21RuntimeIpcManagerImpl9setBinderEPKhiPv");
  // The supplied library strips these private symbols. Its public factory and the two checked
  // prologues identify the analyzed implementation; refuse other builds instead of patching them.
  Dl_info info;
  void *factory = dlsym(manager, "createRuntimeIpcManager");
  if ((!native || !g_set_binder) && factory && dladdr(factory, &info) &&
      (char *)factory - (char *)info.dli_fbase == 0x4150) {
    native = (char *)info.dli_fbase + 0x5780;
    g_set_binder = (void *)((char *)info.dli_fbase + 0x4fe0);
  }
  const uint32_t prologue[] = {0xd10243ff, 0xa9057bfd, 0xf90033f7, 0xa90757f6};
  if (!native || !g_set_binder || memcmp(native, prologue, sizeof prologue) ||
      memcmp((void *)g_set_binder, prologue, sizeof prologue)) {
    LOGW("Surface manager doesn't match the checked arm64 implementation");
    return;
  }
  void *original = mmap(NULL, 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (original == MAP_FAILED) { LOGW("can't allocate setNativeSurface trampoline"); return; }
  // These four verified instructions only adjust sp and save registers; none is PC-relative.
  memcpy(original, native, 16);
  if (!redirect((char *)original + 16, (char *)native + 16)) {
    munmap(original, 32);
    LOGW("can't finish setNativeSurface trampoline");
    return;
  }
  g_native_surface = original;
  __builtin___clear_cache((char *)original, (char *)original + 32);
  if (redirect(native, (void *)set_native_surface)) LOGI("RuntimeIpcManager setNativeSurface exports host producers through Java");
  else LOGW("can't redirect RuntimeIpcManager setNativeSurface");
}

__attribute__((constructor)) static void install(void) {
  void *ndk = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_NOLOAD);
  void *to_java = ndk ? dlsym(ndk, "AIBinder_toJavaBinder") : NULL;
  void *from_java = ndk ? dlsym(ndk, "AIBinder_fromJavaBinder") : NULL;
  if (!to_java || !from_java) {
    LOGW("libbinder_ndk isn't loaded: Java binder conversions stay Horizon's");
    return;
  }
  if (redirect(to_java, (void *)to_java_binder) && redirect(from_java, (void *)from_java_binder))
    LOGI("AIBinder_toJavaBinder and AIBinder_fromJavaBinder go through %s", RELAY_NAME);
  else
    LOGW("can't redirect libbinder_ndk's Java binder conversions");
  install_surfaces();
}
