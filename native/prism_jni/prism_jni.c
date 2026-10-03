// Prism's JNI glue between Horizon OS's Java framework and stock Android 14 x86_64 native code.
//
// Built two ways from this one file (see tools/jni/build.py):
//
// * libprism_jni.so, preloaded into zygote (LD_PRELOAD). Stock libraries register their natives
//   with an inlined jniRegisterNativeMethods that aborts on any mismatch, and Horizon's classes
//   differ from stock in places (Meta changed two SurfaceTexture signatures). So as soon as the VM
//   exists (see androidSetCreateThreadFunc below), this installs a JNI function table whose RegisterNatives
//   registers what exists, drops what doesn't, adapts the changed SurfaceTexture methods to the
//   stock implementations, and adds stubs for every native Meta added to the class. The table goes
//   in through ART's JNIEnvExt::SetTableOverride (what JVMTI's SetJNIFunctionTable uses), so it
//   covers every thread, and every process zygote forks. It also registers Meta's natives in boot
//   classpath classes that no stock library knows about.
//
// * lib<name>.so stand-ins (PRISM_STUB_LIB="<name>") for the arm64 JNI libraries Meta's Java code
//   loads with System.loadLibrary. Their JNI_OnLoad registers that library's natives.
//
// Stubs log once per method and return 0 / null / false. Natives Prism emulates (PRISM_HLE, see
// prism_hle.h) are registered with their implementation instead.

#include "prism_hle.h"

#include <android/log.h>
#include <jni.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define TAG "PrismJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

typedef struct {
  const char *cls;   // "android/view/SurfaceControlExtInternal"
  const char *name;
  const char *sig;
  const char *lib;   // stand-in library that registers it, or "" (registered by libprism_jni)
  int boot;          // class is on the boot classpath
  void *fn;
} PrismNative;

static void prism_stub_hit(int index);

// Generated from the user's OTA: the stub functions and kNatives[] / kNativeCount.
#include "prism_natives.inc"

static atomic_uchar g_hit[sizeof(kNatives) / sizeof(kNatives[0]) + 1];

static void prism_stub_hit(int index) {
  if (!atomic_exchange(&g_hit[index], 1))
    LOGW("stub called: %s.%s%s", kNatives[index].cls, kNatives[index].name, kNatives[index].sig);
}

// ART's own RegisterNatives once the override is installed (so Prism's registrations don't loop
// back into the override); before that, and in the stand-ins, the env's.
typedef jint (*RegisterNativesFn)(JNIEnv *, jclass, const JNINativeMethod *, jint);
static RegisterNativesFn g_register;

static jint register_natives(JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint count) {
  return g_register ? g_register(env, clazz, methods, count)
                    : (*env)->RegisterNatives(env, clazz, methods, count);
}

static void clear_exception(JNIEnv *env) {
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

static int register_one(JNIEnv *env, jclass clazz, const char *cls, const char *name, const char *sig,
                        void *fn) {
  JNINativeMethod method = {name, sig, fn};
  if (register_natives(env, clazz, &method, 1) == 0) return 1;
  clear_exception(env);
  LOGW("could not register %s.%s%s", cls, name, sig);
  return 0;
}

extern const PrismHle __start_prism_hle[] __attribute__((weak));
extern const PrismHle __stop_prism_hle[] __attribute__((weak));

const PrismHle *prism_hle_find(const char *cls, const char *name, const char *sig) {
  for (const PrismHle *h = __start_prism_hle; h < __stop_prism_hle; h++)
    if (!strcmp(h->cls, cls) && !strcmp(h->name, name) && !strcmp(h->sig, sig)) return h;
  return NULL;
}

static int has_table_entries(const char *cls) {
  for (int i = 0; i < kNativeCount; i++)
    if (strcmp(kNatives[i].cls, cls) == 0) return 1;
  return 0;
}

// Registers the table entries for one class; lib filters by stand-in library (NULL = all).
static int register_table(JNIEnv *env, jclass clazz, const char *cls, const char *lib) {
  int count = 0;
  for (int i = 0; i < kNativeCount; i++) {
    if (strcmp(kNatives[i].cls, cls) != 0) continue;
    if (lib && strcmp(kNatives[i].lib, lib) != 0) continue;
    const PrismHle *hle = prism_hle_find(cls, kNatives[i].name, kNatives[i].sig);
    count += register_one(env, clazz, cls, kNatives[i].name, kNatives[i].sig, hle ? hle->fn : kNatives[i].fn);
  }
  return count;
}

static int register_classes(JNIEnv *env, const char *lib, int boot_only) {
  int classes = 0, methods = 0;
  for (int i = 0; i < kNativeCount; i++) {
    if (lib && strcmp(kNatives[i].lib, lib) != 0) continue;
    if (boot_only && !kNatives[i].boot) continue;
    int first = 1;  // handle each class once, at its first entry
    for (int j = 0; j < i && first; j++)
      if (strcmp(kNatives[j].cls, kNatives[i].cls) == 0 &&
          (!lib || strcmp(kNatives[j].lib, lib) == 0) && (!boot_only || kNatives[j].boot))
        first = 0;
    if (!first) continue;
    jclass clazz = (*env)->FindClass(env, kNatives[i].cls);
    if (!clazz) {
      clear_exception(env);
      LOGW("class %s not found", kNatives[i].cls);
      continue;
    }
    methods += register_table(env, clazz, kNatives[i].cls, lib);
    classes++;
    (*env)->DeleteLocalRef(env, clazz);
  }
  LOGI("%s: registered %d natives in %d classes", lib ? lib : "boot classpath", methods, classes);
  return methods;
}

#ifdef PRISM_STUB_LIB

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
  (void)reserved;
  JNIEnv *env = NULL;
  if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  register_classes(env, PRISM_STUB_LIB, 0);
  return JNI_VERSION_1_6;
}

#else  // libprism_jni.so, preloaded into zygote

#include <dlfcn.h>
#include <link.h>
#include <stdint.h>

// ---- SurfaceTexture: Horizon added a boolean to nativeInit and two longs to nativeUpdateTexImage.
// Forward to the stock implementations, captured when stock libandroid_runtime registers them.
typedef void (*SurfaceTextureInit)(JNIEnv *, jobject, jboolean, jint, jboolean, jobject);
typedef void (*SurfaceTextureUpdate)(JNIEnv *, jobject);
static SurfaceTextureInit g_surface_texture_init;
static SurfaceTextureUpdate g_surface_texture_update;

static void surface_texture_init(JNIEnv *env, jobject thiz, jboolean detached, jint tex,
                                 jboolean single_buffer, jboolean meta_flag, jobject weak_this) {
  (void)meta_flag;
  g_surface_texture_init(env, thiz, detached, tex, single_buffer, weak_this);
}

static void surface_texture_update(JNIEnv *env, jobject thiz, jlong meta_a, jlong meta_b) {
  (void)meta_a, (void)meta_b;
  g_surface_texture_update(env, thiz);
}

static const char kSurfaceTexture[] = "android/graphics/SurfaceTexture";

// Takes a stock method Horizon changed; returns 1 if it was captured (and must not be registered).
static int capture_changed(const char *cls, const JNINativeMethod *m) {
  if (strcmp(cls, kSurfaceTexture) != 0) return 0;
  if (!strcmp(m->name, "nativeInit") && !strcmp(m->signature, "(ZIZLjava/lang/ref/WeakReference;)V")) {
    g_surface_texture_init = (SurfaceTextureInit)m->fnPtr;
    return 1;
  }
  if (!strcmp(m->name, "nativeUpdateTexImage") && !strcmp(m->signature, "()V")) {
    g_surface_texture_update = (SurfaceTextureUpdate)m->fnPtr;
    return 1;
  }
  return 0;
}

static void register_adapters(JNIEnv *env, jclass clazz, const char *cls) {
  if (strcmp(cls, kSurfaceTexture) != 0) return;
  if (g_surface_texture_init)
    register_one(env, clazz, cls, "nativeInit", "(ZIZZLjava/lang/ref/WeakReference;)V",
                 (void *)surface_texture_init);
  if (g_surface_texture_update)
    register_one(env, clazz, cls, "nativeUpdateTexImage", "(JJ)V", (void *)surface_texture_update);
}

// ---- arm64 code registering natives. Since Android 15, ART's RegisterNatives asks the native bridge
// whether each function is guest code and registers a trampoline to it instead
// (NativeBridgeGetTrampolineForFunctionPointer). Digitalis, an Android 16 build, leaves that to ART;
// Android 14's ART registers the arm64 address itself, and the first call jumps into arm64 code from
// x86_64. Prism's RegisterNatives does what newer ART does.
typedef struct {  // libnativebridge's NativeBridgeCallbacks, version 8
  uint32_t version;
  void *v1_to_v6[17];
  void *get_trampoline_with_jni_call_type;
  void *(*get_trampoline_for_function_pointer)(const void *method, const char *shorty, uint32_t len,
                                               int jni_call_type);
  _Bool (*is_native_bridge_function_pointer)(const void *method);
} NativeBridgeCallbacks;

enum { kJNICallTypeRegular = 1 };

static void *find_symbol(const void *base, const char *name);
static const NativeBridgeCallbacks *g_bridge;

static int find_bridge(struct dl_phdr_info *info, size_t size, void *data) {
  (void)size, (void)data;
  const char *name = info->dlpi_name ? strrchr(info->dlpi_name, '/') : NULL;
  if (!name || strncmp(name, "/libberberis", 12) != 0) return 0;
  for (int i = 0; i < info->dlpi_phnum; i++) {
    if (info->dlpi_phdr[i].p_type != PT_LOAD || info->dlpi_phdr[i].p_offset != 0) continue;
    const NativeBridgeCallbacks *itf =
        find_symbol((const void *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr), "NativeBridgeItf");
    if (itf && itf->version >= 8) g_bridge = itf;
    return 1;
  }
  return 0;
}

// "(ILjava/lang/String;[B)V" -> "VILL"
static int shorty_of(const char *sig, char *out, size_t size) {
  const char *ret = strchr(sig, ')');
  if (!ret || size < 2) return 0;
  size_t n = 0;
  out[n++] = ret[1] == '[' ? 'L' : ret[1];
  for (const char *p = sig + 1; p < ret && n + 1 < size; p++) {
    char c = *p;
    while (*p == '[') p++;
    if (*p == 'L') p = strchr(p, ';');
    if (!p) return 0;
    out[n++] = c == '[' ? 'L' : *p == ';' ? 'L' : c;
  }
  out[n] = 0;
  return 1;
}

// The function to register for fn: fn itself, or a trampoline if it is guest (arm64) code.
static void *host_callable(void *fn, const char *sig) {
  Dl_info info;
  if (!fn || dladdr(fn, &info)) return fn;  // the host linker knows it: host code
  if (!g_bridge) dl_iterate_phdr(find_bridge, NULL);
  if (!g_bridge || !g_bridge->is_native_bridge_function_pointer(fn)) return fn;
  char shorty[256];
  if (!shorty_of(sig, shorty, sizeof shorty)) return fn;
  void *trampoline = g_bridge->get_trampoline_for_function_pointer(fn, shorty, (uint32_t)strlen(shorty),
                                                                   kJNICallTypeRegular);
  return trampoline ? trampoline : fn;
}

// methods with guest functions replaced by trampolines: methods itself if none are, else a copy
// the caller frees.
static const JNINativeMethod *host_methods(const JNINativeMethod *methods, jint count) {
  JNINativeMethod *copy = NULL;
  for (jint i = 0; i < count; i++) {
    void *fn = host_callable(methods[i].fnPtr, methods[i].signature);
    if (fn == methods[i].fnPtr) continue;
    if (!copy) {
      copy = malloc(sizeof *copy * (size_t)count);
      if (!copy) return methods;
      memcpy(copy, methods, sizeof *copy * (size_t)count);
    }
    copy[i].fnPtr = fn;
  }
  return copy ? copy : methods;
}

// ---- The RegisterNatives override.
static jmethodID g_class_get_name;

// "android.graphics.SurfaceTexture" -> "android/graphics/SurfaceTexture"
static int class_name(JNIEnv *env, jclass clazz, char *out, size_t size) {
  jstring name = (jstring)(*env)->CallObjectMethod(env, clazz, g_class_get_name);
  if (!name) {
    clear_exception(env);
    return 0;
  }
  const char *utf = (*env)->GetStringUTFChars(env, name, NULL);
  size_t i = 0;
  for (; utf && utf[i] && i + 1 < size; i++) out[i] = utf[i] == '.' ? '/' : utf[i];
  out[i] = 0;
  if (utf) (*env)->ReleaseStringUTFChars(env, name, utf);
  (*env)->DeleteLocalRef(env, name);
  return 1;
}

static jint register_host_natives(JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint count) {
  char cls[512];
  if (!class_name(env, clazz, cls, sizeof cls)) return g_register(env, clazz, methods, count);
  int changed = strcmp(cls, kSurfaceTexture) == 0;
  if (!changed && g_register(env, clazz, methods, count) == 0) {
    if (has_table_entries(cls)) register_table(env, clazz, cls, NULL);
    return JNI_OK;
  }
  // Something in this class differs from stock: register one at a time instead of failing.
  clear_exception(env);
  int dropped = 0;
  for (jint i = 0; i < count; i++) {
    if (capture_changed(cls, &methods[i])) continue;
    if (g_register(env, clazz, &methods[i], 1) != 0) {
      clear_exception(env);
      dropped++;
      LOGW("dropped %s.%s%s: not in Horizon's class", cls, methods[i].name, methods[i].signature);
    }
  }
  int added = register_table(env, clazz, cls, NULL);
  register_adapters(env, clazz, cls);
  LOGI("%s: %d stock natives dropped, %d Meta natives added", cls, dropped, added);
  return JNI_OK;
}

static jint JNICALL prism_register_natives(JNIEnv *env, jclass clazz, const JNINativeMethod *methods,
                                           jint count) {
  const JNINativeMethod *host = host_methods(methods, count);
  jint result = register_host_natives(env, clazz, host, count);
  if (host != methods) free((void *)host);
  return result;
}

// ---- Finding libart's internals: they're exported, but libart lives in the ART module's linker
// namespace, so dlsym can't reach them from here. Read its dynamic symbol table in memory instead.
static void *find_symbol(const void *base, const char *name) {
  const ElfW(Ehdr) *eh = (const ElfW(Ehdr) *)base;
  const ElfW(Phdr) *ph = (const ElfW(Phdr) *)((const char *)base + eh->e_phoff);
  uintptr_t bias = 0;
  const ElfW(Dyn) *dyn = NULL;
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD && ph[i].p_offset == 0) bias = (uintptr_t)base - ph[i].p_vaddr;
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_DYNAMIC) dyn = (const ElfW(Dyn) *)(bias + ph[i].p_vaddr);
  if (!dyn) return NULL;
  const ElfW(Sym) *symtab = NULL;
  const char *strtab = NULL;
  const uint32_t *gnu_hash = NULL;
  for (; dyn->d_tag != DT_NULL; dyn++) {
    if (dyn->d_tag == DT_SYMTAB) symtab = (const ElfW(Sym) *)(bias + dyn->d_un.d_ptr);
    if (dyn->d_tag == DT_STRTAB) strtab = (const char *)(bias + dyn->d_un.d_ptr);
    if (dyn->d_tag == DT_GNU_HASH) gnu_hash = (const uint32_t *)(bias + dyn->d_un.d_ptr);
  }
  if (!symtab || !strtab || !gnu_hash) return NULL;
  uint32_t hash = 5381;
  for (const unsigned char *c = (const unsigned char *)name; *c; c++) hash = hash * 33 + *c;
  uint32_t nbuckets = gnu_hash[0], symoffset = gnu_hash[1], bloom_size = gnu_hash[2];
  const uint32_t *buckets = (const uint32_t *)((const ElfW(Addr) *)&gnu_hash[4] + bloom_size);
  const uint32_t *chain = buckets + nbuckets;
  uint32_t index = buckets[hash % nbuckets];
  if (index < symoffset) return NULL;
  for (;; index++) {
    uint32_t h = chain[index - symoffset];
    if ((h | 1) == (hash | 1) && strcmp(strtab + symtab[index].st_name, name) == 0)
      return (void *)(bias + symtab[index].st_value);
    if (h & 1) return NULL;
  }
}

static struct JNINativeInterface g_table;

static void install(JNIEnv *env) {
  Dl_info info;
  if (!dladdr((const void *)(*env)->FindClass, &info) || !info.dli_fbase) {
    LOGW("libart not found; Horizon's framework will not get Prism's JNI fixes");
    return;
  }
  typedef const struct JNINativeInterface *(*GetInterface)(void);
  typedef void (*SetOverride)(const struct JNINativeInterface *);
  GetInterface get_interface = (GetInterface)find_symbol(info.dli_fbase, "_ZN3art21GetJniNativeInterfaceEv");
  SetOverride set_override =
      (SetOverride)find_symbol(info.dli_fbase, "_ZN3art9JNIEnvExt16SetTableOverrideEPK18JNINativeInterface");
  if (!get_interface || !set_override) {
    LOGW("libart's JNI table override not found (%s)", info.dli_fname);
    return;
  }
  jclass class_class = (*env)->FindClass(env, "java/lang/Class");
  g_class_get_name = (*env)->GetMethodID(env, class_class, "getName", "()Ljava/lang/String;");
  (*env)->DeleteLocalRef(env, class_class);
  memcpy(&g_table, get_interface(), sizeof g_table);
  g_register = g_table.RegisterNatives;
  g_table.RegisterNatives = prism_register_natives;
  set_override(&g_table);
  LOGI("JNI table override installed (%s)", info.dli_fname);
  register_classes(env, NULL, 1);
}

// The hook point: AndroidRuntime::startReg calls libutils' androidSetCreateThreadFunc first, on the
// main thread, right after the VM is created and before any native is registered. It's the first
// call into another library on that path (libandroid_runtime creates the VM through its own
// JNI_CreateJavaVM, which can't be interposed), so Prism wraps it.
typedef void (*SetCreateThreadFunc)(void *);
typedef jint (*GetCreatedJavaVMs)(JavaVM **, jsize, jsize *);
static atomic_int g_installed;

JNIEXPORT void androidSetCreateThreadFunc(void *func) {
  SetCreateThreadFunc real = (SetCreateThreadFunc)dlsym(RTLD_NEXT, "androidSetCreateThreadFunc");
  if (real) real(func);
  if (atomic_exchange(&g_installed, 1)) return;
  GetCreatedJavaVMs get_vms = (GetCreatedJavaVMs)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
  JavaVM *vm = NULL;
  jsize count = 0;
  JNIEnv *env = NULL;
  if (!get_vms || get_vms(&vm, 1, &count) != JNI_OK || count < 1 ||
      (*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
    LOGW("no Java VM at startReg; Horizon's framework will not get Prism's JNI fixes");
    return;
  }
  install(env);
}

// Keep LD_PRELOAD out of anything zygote's children exec (the library is already mapped).
__attribute__((constructor)) static void prism_jni_init(void) {
  unsetenv("LD_PRELOAD");
  LOGI("loaded (%d Meta natives in table)", kNativeCount);
}

#endif
