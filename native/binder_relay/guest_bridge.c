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
 */
#include <android/binder_ibinder.h>
#include <android/binder_ibinder_jni.h>
#include <android/binder_parcel.h>
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
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

static int java_ready(JNIEnv *env) {
  pthread_mutex_lock(&g_java_lock);
  if (!g_java.relay) {
    jclass sm = (*env)->FindClass(env, "android/os/ServiceManager");
    jmethodID check = sm ? (*env)->GetStaticMethodID(env, sm, "checkService", "(Ljava/lang/String;)Landroid/os/IBinder;") : NULL;
    jclass parcel = check ? (*env)->FindClass(env, "android/os/Parcel") : NULL;
    jclass ibinder = parcel ? (*env)->FindClass(env, "android/os/IBinder") : NULL;
    if (!threw(env, "looking up ServiceManager, Parcel and IBinder") && ibinder) {
      g_java.obtain = (*env)->GetStaticMethodID(env, parcel, "obtain", "()Landroid/os/Parcel;");
      g_java.recycle = (*env)->GetMethodID(env, parcel, "recycle", "()V");
      g_java.write_token = (*env)->GetMethodID(env, parcel, "writeInterfaceToken", "(Ljava/lang/String;)V");
      g_java.write_long = (*env)->GetMethodID(env, parcel, "writeLong", "(J)V");
      g_java.write_binder = (*env)->GetMethodID(env, parcel, "writeStrongBinder", "(Landroid/os/IBinder;)V");
      g_java.read_long = (*env)->GetMethodID(env, parcel, "readLong", "()J");
      g_java.read_binder = (*env)->GetMethodID(env, parcel, "readStrongBinder", "()Landroid/os/IBinder;");
      g_java.transact = (*env)->GetMethodID(env, ibinder, "transact", "(ILandroid/os/Parcel;Landroid/os/Parcel;I)Z");
      jstring name = threw(env, "looking up Parcel's methods") ? NULL : (*env)->NewStringUTF(env, RELAY_NAME);
      jobject relay = name ? (*env)->CallStaticObjectMethod(env, sm, check, name) : NULL;
      if (!threw(env, "ServiceManager.checkService") && relay) {
        g_java.parcel = (*env)->NewGlobalRef(env, parcel);
        g_java.relay = (*env)->NewGlobalRef(env, relay);
      } else if (name) {
        LOGW("Java: no %s", RELAY_NAME);
      }
      if (relay) (*env)->DeleteLocalRef(env, relay);
      if (name) (*env)->DeleteLocalRef(env, name);
    }
    if (ibinder) (*env)->DeleteLocalRef(env, ibinder);
    if (parcel) (*env)->DeleteLocalRef(env, parcel);
    if (sm) (*env)->DeleteLocalRef(env, sm);
  }
  pthread_mutex_unlock(&g_java_lock);
  return g_java.relay != NULL;
}

// One transaction with the relay from Java: writes the token and then a binder (put) or a token
// (take); reads the reply's token (put) or binder (take, a local reference).
static int java_transact(JNIEnv *env, int code, jobject binder, jlong token, jlong *token_out, jobject *binder_out) {
  if (!java_ready(env)) return 0;
  jobject in = (*env)->CallStaticObjectMethod(env, g_java.parcel, g_java.obtain);
  jobject out = in ? (*env)->CallStaticObjectMethod(env, g_java.parcel, g_java.obtain) : NULL;
  int ok = 0;
  if (!threw(env, "Parcel.obtain") && out) {
    jstring descriptor = (*env)->NewStringUTF(env, RELAY_DESCRIPTOR);
    (*env)->CallVoidMethod(env, in, g_java.write_token, descriptor);
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
          *binder_out = (*env)->CallObjectMethod(env, out, g_java.read_binder);
        ok = !threw(env, "reading the relay's reply");
      }
    }
    if (descriptor) (*env)->DeleteLocalRef(env, descriptor);
  }
  if (out) (*env)->CallVoidMethod(env, out, g_java.recycle), (*env)->DeleteLocalRef(env, out);
  if (in) (*env)->CallVoidMethod(env, in, g_java.recycle), (*env)->DeleteLocalRef(env, in);
  threw(env, "Parcel.recycle");
  return ok;
}

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
}
