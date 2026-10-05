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

#include "prism_guest.h"
#include "prism_hle.h"

#include <android/log.h>
#include <jni.h>
#include <stdatomic.h>
#include <stdio.h>
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
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

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
  void *(*get_trampoline_with_jni_call_type)(void *handle, const char *name, const char *shorty, uint32_t len,
                                             int jni_call_type);
  void *(*get_trampoline_for_function_pointer)(const void *method, const char *shorty, uint32_t len,
                                               int jni_call_type);
  _Bool (*is_native_bridge_function_pointer)(const void *method);
} NativeBridgeCallbacks;

enum { kJNICallTypeRegular = 1, kJNICallTypeCriticalNative = 2 };

static void *find_symbol(const void *base, const char *name);
static void hook_translator_imports(const struct dl_phdr_info *info);
static void hook_create_namespace(NativeBridgeCallbacks *itf);
static const NativeBridgeCallbacks *g_bridge;
typedef void (*SetExecutableFn)(const char *, size_t);  // takes a std::string_view: pointer, length
static SetExecutableFn g_set_executable;

static int find_bridge(struct dl_phdr_info *info, size_t size, void *data) {
  (void)size, (void)data;
  const char *name = info->dlpi_name ? strrchr(info->dlpi_name, '/') : NULL;
  if (!name || strncmp(name, "/libberberis", 12) != 0) return 0;
  for (int i = 0; i < info->dlpi_phnum; i++) {
    if (info->dlpi_phdr[i].p_type != PT_LOAD || info->dlpi_phdr[i].p_offset != 0) continue;
    const NativeBridgeCallbacks *itf =
        find_symbol((const void *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr), "NativeBridgeItf");
    if (!itf || itf->version < 8) return 0;  // another of the translator's libraries
    g_bridge = itf;
    g_set_executable = (SetExecutableFn)find_symbol(
        (const void *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr),
        "_ZN8berberis25SetMainExecutableRealPathENSt3__117basic_string_viewIcNS0_11char_traitsIcEEEE");
    hook_translator_imports(info);
    hook_create_namespace((NativeBridgeCallbacks *)itf);
    return 1;
  }
  return 0;
}

// ---- x86_64 functions arm64 code calls. The translator binds each one (eglCreateWindowSurface,
// ANativeWindow_acquire, ...) with dlsym on the real library, so Prism wraps the translator's own
// dlsym import and swaps in its replacements (prism_window_function, prism_media_function).
typedef void *(*LoaderDlsym)(void *, const char *, const void *);
static void *(*g_dlsym)(void *, const char *);
static LoaderDlsym g_loader_dlsym;

static void *own_function(const char *name, void *real) {
  void *own = prism_window_function(name, real);
  return own ? own : prism_media_function(name, real);
}

static int hook_proxy_tables(struct dl_phdr_info *info, size_t size, void *data);

static void *translator_dlsym(void *handle, const char *name) {
  // A proxy library just loaded: its table of host functions is relocated, and not yet read.
  if (name && !strcmp(name, "InitProxyLibrary")) dl_iterate_phdr(hook_proxy_tables, NULL);
  // dlsym looks RTLD_DEFAULT and RTLD_NEXT up from its caller; keep that the translator.
  void *symbol = g_loader_dlsym ? g_loader_dlsym(handle, name, __builtin_return_address(0)) : g_dlsym(handle, name);
  void *own = symbol && name ? own_function(name, symbol) : NULL;
  return own ? own : symbol;
}

// ---- Writing a function pointer in another library's memory (relocated data or the interface
// struct of the translator), keeping the page's protection as it was: the same page can hold data
// its library still writes.
static int page_protection(uintptr_t page) {
  FILE *maps = fopen("/proc/self/maps", "re");
  if (!maps) return -1;
  char line[512];
  int prot = -1;
  while (prot < 0 && fgets(line, sizeof line, maps)) {
    unsigned long start, end;
    char perms[5];
    if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) != 3 || page < start || page >= end) continue;
    prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) | (perms[2] == 'x' ? PROT_EXEC : 0);
  }
  fclose(maps);
  return prot;
}

static int write_slot(void **slot, void *value) {
  size_t size = (size_t)getpagesize();
  uintptr_t page = (uintptr_t)slot & ~(uintptr_t)(size - 1);
  int prot = page_protection(page);
  if (prot < 0 || (!(prot & PROT_WRITE) && mprotect((void *)page, size, prot | PROT_WRITE))) return 0;
  *slot = value;
  if (!(prot & PROT_WRITE)) mprotect((void *)page, size, prot);
  return 1;
}

// ---- One copy of Horizon's arm64 libbinder per process. Apps' classloader namespaces are "shared":
// they start with every library their parent has loaded. On a headset zygote has loaded libbinder
// by then, so the app's libraries and the platform's use one copy. Under the translator nothing
// arm64 is loaded that early, so each namespace loads its own libbinder; each copy opens
// /dev/binder with its own ProcessState, and objects published through a copy whose thread pool
// nobody starts never answer. Prism loads the binder libraries into the default arm64 namespace
// before the first namespace is created, as zygote would have, and starts that libbinder's thread
// pool: on a headset the app's one ProcessState is the runtime's, whose pool the runtime starts.
// Prism's bridge (native/binder_relay/guest_bridge.c) follows: it hands that libbinder's objects to
// Java and back. Prism's arm64 media functions (native/guest_media) are global, for dlsym.
#define GUEST_DIR "/system/lib64/arm64/prism"  // also in tools/deploy.py
static const struct {
  const char *name;
  int flags;
} kGuestPreload[] = {{"libbinder_ndk.so", RTLD_NOW},
                     {"libbinder.so", RTLD_NOW},
                     {"libhidlbase.so", RTLD_NOW},
                     {"libprism_binder_bridge.so", RTLD_NOW},
                     {"libprism_guest_media.so", RTLD_NOW | RTLD_GLOBAL}};
// The executable an app's arm64 code sees. A headset's app processes are zygote's, whose
// /proc/self/exe is /system/bin/app_process64. The translator answers arm64 code's readlink of it
// with the main executable it records, the guest app_process it loaded
// (/system/bin/arm64/app_process64). Meta's spatial persistence client (libplugin.so) tells Java
// apps from native executables by that path: from any other path under /system/bin it sends the
// path as its package name, and the spatial persistence service refused the shell's and Guardian's
// anchor queries for it (ERROR_RPC_PACKAGE_NAME_MISMATCH, 10 a second). By the time an app creates
// its first namespace the guest app_process is loaded, so Prism records the host's path instead.
#define APP_PROCESS "/system/bin/app_process64"
typedef void *(*CreateNamespaceFn)(const char *, const char *, const char *, uint64_t, const char *, void *);
static CreateNamespaceFn g_create_namespace;

// An app zygote (Chrome's browser_zygote, webview_zygote) preloads the app's libraries, so it makes
// the first namespace, but it forks only once it is down to one thread: ZygoteHooks.preFork lists
// /proc/self/task until then, and pool threads never exit. libbinder's ProcessState also can't be
// used in a child of the process that made it. So in a zygote Prism only preloads, and each child
// starts its own pool at its next namespace or library.
static void (*g_start_pool)(void);
static atomic_int g_pool_pid;  // the process whose pool Prism started: a fork's child has none

// By name: app zygotes are "<process>_zygote" (webview_zygote too). Not by SELinux domain: system
// apps with no seapp_contexts entry stay in zygote's.
static int in_zygote(void) {
  char name[256] = {0};
  FILE *cmdline = fopen("/proc/self/cmdline", "re");
  if (!cmdline) return 0;
  fread(name, 1, sizeof name - 1, cmdline);
  fclose(cmdline);
  size_t n = strlen(name);
  return !strcmp(name, "zygote") || !strcmp(name, "zygote64") || (n >= 7 && !strcmp(name + n - 7, "_zygote"));
}

static void start_guest_pool(const char *when) {
  int pid = getpid();
  if (!g_start_pool || atomic_load(&g_pool_pid) == pid || in_zygote()) return;
  if (atomic_exchange(&g_pool_pid, pid) == pid) return;
  g_start_pool();
  LOGI("arm64 binder thread pool started (%s)", when);
}

static void *bridge_create_namespace(const char *name, const char *ld_path, const char *default_path, uint64_t type,
                                     const char *permitted, void *parent) {
  static atomic_int preloaded;
  if (!atomic_exchange(&preloaded, 1)) {
    void *(*load)(const char *, int) = (void *(*)(const char *, int))g_bridge->v1_to_v6[1];  // loadLibrary
    void *binder_ndk = NULL;
    for (size_t i = 0; i < sizeof kGuestPreload / sizeof *kGuestPreload; i++) {
      char path[128];
      snprintf(path, sizeof path, GUEST_DIR "/%s", kGuestPreload[i].name);
      void *handle = load(path, kGuestPreload[i].flags);
      if (!handle) LOGW("arm64 %s: not preloaded", kGuestPreload[i].name);
      if (i == 0) binder_ndk = handle;
    }
    g_start_pool =
        binder_ndk ? (void (*)(void))g_bridge->get_trampoline_with_jni_call_type(binder_ndk, "ABinderProcess_startThreadPool",
                                                                                 "V", 1, kJNICallTypeCriticalNative)
                   : NULL;
    LOGI("arm64 binder libraries preloaded before namespace %s", name);
    if (g_set_executable) g_set_executable(APP_PROCESS, sizeof APP_PROCESS - 1);
  }
  start_guest_pool(name);
  return g_create_namespace(name, ld_path, default_path, type, permitted, parent);
}

// An app's arm64 libraries load through loadLibraryExt; Prism patches some as they do, before any of
// their code runs (guest_patch.c).
typedef void *(*LoadLibraryExtFn)(const char *, int, void *);
static LoadLibraryExtFn g_load_library_ext;

static void *bridge_load_library_ext(const char *path, int flags, void *ns) {
  start_guest_pool(path);
  void *handle = g_load_library_ext(path, flags, ns);
  if (handle) prism_patch_guest_library(path);
  return handle;
}

static void hook_create_namespace(NativeBridgeCallbacks *itf) {
  void **slot = &itf->v1_to_v6[11];  // createNamespace
  if (*slot != (void *)bridge_create_namespace) {
    g_create_namespace = (CreateNamespaceFn)*slot;
    if (!write_slot(slot, (void *)bridge_create_namespace)) LOGW("translator's createNamespace: can't write its slot");
  }
  slot = &itf->v1_to_v6[13];  // loadLibraryExt
  if (*slot != (void *)bridge_load_library_ext) {
    g_load_library_ext = (LoadLibraryExtFn)*slot;
    if (!write_slot(slot, (void *)bridge_load_library_ext)) LOGW("translator's loadLibraryExt: can't write its slot");
  }
}

// ---- Horizon's arm64 libraries for arm64 code that opens /system/lib64/<name>.so by path. On a
// headset that is the arm64 library; here it's the emulator's x86_64 one, which the guest linker
// rejects. Meta's code doesn't always cope (libvrapiimpl's heap-profiling hooks then call a null
// pointer and kill VrShell). The guest linker is translated code; its opens reach the host through
// the translator's imports (open and openat for the paths it emulates, syscall for the rest), where
// Prism swaps in the GUEST_DIR copy if there is one.
static const char *guest_library(const char *path, char *buf, size_t size) {
  if (!path || strncmp(path, "/system/lib64/", 14)) return path;
  const char *name = path + 14;
  size_t n = strlen(name);
  if (n < 4 || strchr(name, '/') || strcmp(name + n - 3, ".so") ||
      snprintf(buf, size, GUEST_DIR "/%s", name) >= (int)size || access(buf, R_OK))
    return path;
  LOGI("arm64 code opens %s: Horizon's copy instead", path);
  return buf;
}

static long (*g_syscall)(long, ...);
static int (*g_open)(const char *, int, ...);
static int (*g_open_2)(const char *, int);
static int (*g_openat)(int, const char *, int, ...);

static long translator_syscall(long nr, long a, long b, long c, long d, long e, long f) {
  char buf[256];
  if (nr == __NR_openat) b = (long)guest_library((const char *)b, buf, sizeof buf);
  return g_syscall(nr, a, b, c, d, e, f);
}

static int translator_open(const char *path, int flags, int mode) {
  char buf[256];
  return g_open(guest_library(path, buf, sizeof buf), flags, mode);
}

static int translator_open_2(const char *path, int flags) {
  char buf[256];
  return g_open_2(guest_library(path, buf, sizeof buf), flags);
}

static int translator_openat(int dirfd, const char *path, int flags, int mode) {
  char buf[256];
  return g_openat(dirfd, guest_library(path, buf, sizeof buf), flags, mode);
}

static void hook_translator_imports(const struct dl_phdr_info *info) {
  const ElfW(Dyn) *dyn = NULL;
  for (int i = 0; i < info->dlpi_phnum; i++)
    if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) dyn = (const ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
  const ElfW(Sym) *symtab = NULL;
  const char *strtab = NULL;
  const ElfW(Rela) *tables[2] = {NULL, NULL};
  size_t sizes[2] = {0, 0};
  for (; dyn && dyn->d_tag != DT_NULL; dyn++) {
    uintptr_t p = info->dlpi_addr + dyn->d_un.d_ptr;
    if (dyn->d_tag == DT_SYMTAB) symtab = (const ElfW(Sym) *)p;
    if (dyn->d_tag == DT_STRTAB) strtab = (const char *)p;
    if (dyn->d_tag == DT_JMPREL) tables[0] = (const ElfW(Rela) *)p;
    if (dyn->d_tag == DT_PLTRELSZ) sizes[0] = dyn->d_un.d_val;
    if (dyn->d_tag == DT_RELA) tables[1] = (const ElfW(Rela) *)p;
    if (dyn->d_tag == DT_RELASZ) sizes[1] = dyn->d_un.d_val;
  }
  if (!symtab || !strtab) return;
  static const struct {
    const char *name;
    void **real;
    void *wrapper;
  } kOpens[] = {
      {"syscall", (void **)&g_syscall, (void *)translator_syscall},
      {"open", (void **)&g_open, (void *)translator_open},
      {"__open_2", (void **)&g_open_2, (void *)translator_open_2},
      {"openat", (void **)&g_openat, (void *)translator_openat},
  };
  for (int t = 0; t < 2; t++) {
    for (size_t i = 0; tables[t] && i < sizes[t] / sizeof(ElfW(Rela)); i++) {
      const ElfW(Rela) *r = &tables[t][i];
      uint32_t type = ELF64_R_TYPE(r->r_info);
      if (type != R_X86_64_JUMP_SLOT && type != R_X86_64_GLOB_DAT) continue;
      const char *name = strtab + symtab[ELF64_R_SYM(r->r_info)].st_name;
      void **slot = (void **)(info->dlpi_addr + r->r_offset);
      if (!strcmp(name, "dlsym")) {
        if (*slot == (void *)translator_dlsym) continue;
        g_loader_dlsym = (LoaderDlsym)dlsym(RTLD_DEFAULT, "__loader_dlsym");
        g_dlsym = *slot;
        if (!write_slot(slot, (void *)translator_dlsym)) LOGW("translator's dlsym: can't write its slot");
        continue;
      }
      for (size_t k = 0; k < sizeof kOpens / sizeof *kOpens; k++) {
        if (strcmp(name, kOpens[k].name) || *slot == kOpens[k].wrapper) continue;
        *kOpens[k].real = *slot;
        if (!write_slot(slot, kOpens[k].wrapper)) LOGW("translator's %s: can't write its slot", name);
      }
    }
  }
  LOGI("translator's dlsym and file opens wrapped (%s)", info->dlpi_name);
}

// The translator's proxy libraries (libberberis_proxy_libvulkan, ...) don't bind host functions
// with dlsym: each keeps a table of their addresses, relocated by the host linker (from packed
// relocations), and builds its trampolines from it in InitProxyLibrary. Prism swaps its
// replacements in, finding each function's address in the library's writable segments.
static const char *const kProxied[] = {"vkCreateAndroidSurfaceKHR", "vkGetInstanceProcAddr"};

static int hook_proxy_tables(struct dl_phdr_info *info, size_t size, void *data) {
  (void)size, (void)data;
  const char *name = info->dlpi_name ? strrchr(info->dlpi_name, '/') : NULL;
  if (!name || strncmp(name, "/libberberis_proxy_", 19) != 0) return 0;
  for (size_t k = 0; k < sizeof kProxied / sizeof *kProxied; k++) {
    void *real = dlsym(RTLD_DEFAULT, kProxied[k]);
    if (!real) continue;
    void *own = NULL;
    for (int i = 0; i < info->dlpi_phnum; i++) {
      const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
      if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_W)) continue;
      uintptr_t start = (info->dlpi_addr + ph->p_vaddr + sizeof(void *) - 1) & ~(uintptr_t)(sizeof(void *) - 1);
      void **end = (void **)((info->dlpi_addr + ph->p_vaddr + ph->p_memsz) & ~(uintptr_t)(sizeof(void *) - 1));
      for (void **slot = (void **)start; slot < end; slot++) {
        if (*slot != real) continue;
        if (!own && !(own = prism_window_function(kProxied[k], real))) break;
        if (write_slot(slot, own))
          LOGI("%s: %s replaced", name + 1, kProxied[k]);
        else
          LOGW("%s: can't write the slot of %s", name + 1, kProxied[k]);
      }
    }
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

int prism_is_guest_code(const void *fn) {
  Dl_info info;
  if (!fn || dladdr(fn, &info)) return 0;  // the host linker knows it: host code
  if (!g_bridge) dl_iterate_phdr(find_bridge, NULL);
  return g_bridge && g_bridge->is_native_bridge_function_pointer(fn);
}

void *prism_guest_function(void *fn, const char *shorty) {
  if (!prism_is_guest_code(fn)) return NULL;
  // A critical native's arguments are exactly its shorty's: no JNIEnv or class in front.
  return g_bridge->get_trampoline_for_function_pointer(fn, shorty, (uint32_t)strlen(shorty),
                                                        kJNICallTypeCriticalNative);
}

// The function to register for fn: fn itself, or a trampoline if it is guest (arm64) code.
static void *host_callable(void *fn, const char *sig) {
  if (!prism_is_guest_code(fn)) return fn;
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

// ---- FindClass from threads without Java frames. ART looks a class up there with the system class
// loader, which has none of an app's classes, so a native thread that attached itself finds them
// only through references taken earlier on a Java thread. Meta's apps take theirs as they start, and
// on a headset that's done before their native threads ask; translated, those threads can come
// first (Presence's simplejni looks up its CoreFunctions from one). When a lookup fails, Prism tries
// the class loaders whose classes have registered natives in this process: the app's, mostly.
#include <pthread.h>
#define MAX_LOADERS 16
static jobject g_loaders[MAX_LOADERS];  // global references
static int g_loader_count;
static pthread_mutex_t g_loaders_lock = PTHREAD_MUTEX_INITIALIZER;
static jmethodID g_class_get_loader, g_class_for_name;
static jclass g_class_class, g_class_not_found;
typedef jclass (*FindClassFn)(JNIEnv *, const char *);
static FindClassFn g_find_class;

static void remember_loader(JNIEnv *env, jclass clazz) {
  if (!g_class_get_loader) return;
  jobject loader = (*env)->CallObjectMethod(env, clazz, g_class_get_loader);
  if (!loader) {  // the boot class path's
    clear_exception(env);
    return;
  }
  pthread_mutex_lock(&g_loaders_lock);
  int known = 0;
  for (int i = 0; i < g_loader_count && !known; i++) known = (*env)->IsSameObject(env, g_loaders[i], loader);
  if (!known && g_loader_count < MAX_LOADERS) g_loaders[g_loader_count++] = (*env)->NewGlobalRef(env, loader);
  pthread_mutex_unlock(&g_loaders_lock);
  (*env)->DeleteLocalRef(env, loader);
}

static jclass JNICALL prism_find_class(JNIEnv *env, const char *name) {
  jclass found = g_find_class(env, name);
  if (found || !g_class_for_name || !name || name[0] == '[') return found;
  jthrowable failure = (*env)->ExceptionOccurred(env);
  if (!failure || !(*env)->IsInstanceOf(env, failure, g_class_not_found)) {
    if (failure) (*env)->DeleteLocalRef(env, failure);
    return NULL;
  }
  (*env)->ExceptionClear(env);
  char dotted[512];
  size_t n = 0;
  for (; name[n] && n + 1 < sizeof dotted; n++) dotted[n] = name[n] == '/' ? '.' : name[n];
  dotted[n] = 0;
  jstring java_name = (*env)->NewStringUTF(env, dotted);
  pthread_mutex_lock(&g_loaders_lock);
  int count = g_loader_count;
  pthread_mutex_unlock(&g_loaders_lock);
  for (int i = 0; java_name && !found && i < count; i++) {
    found = (jclass)(*env)->CallStaticObjectMethod(env, g_class_class, g_class_for_name, java_name, JNI_TRUE, g_loaders[i]);
    if (!found) (*env)->ExceptionClear(env);
  }
  if (java_name) (*env)->DeleteLocalRef(env, java_name);
  if (!found) (*env)->Throw(env, failure);
  (*env)->DeleteLocalRef(env, failure);
  return found;
}

static jint JNICALL prism_register_natives(JNIEnv *env, jclass clazz, const JNINativeMethod *methods,
                                           jint count) {
  remember_loader(env, clazz);
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
  g_class_get_loader = (*env)->GetMethodID(env, class_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
  g_class_for_name = (*env)->GetStaticMethodID(env, class_class, "forName",
                                               "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
  g_class_class = (jclass)(*env)->NewGlobalRef(env, class_class);
  (*env)->DeleteLocalRef(env, class_class);
  jclass not_found = (*env)->FindClass(env, "java/lang/ClassNotFoundException");
  g_class_not_found = (jclass)(*env)->NewGlobalRef(env, not_found);
  (*env)->DeleteLocalRef(env, not_found);
  memcpy(&g_table, get_interface(), sizeof g_table);
  g_register = g_table.RegisterNatives;
  g_table.RegisterNatives = prism_register_natives;
  g_find_class = g_table.FindClass;
  g_table.FindClass = prism_find_class;
  set_override(&g_table);
  LOGI("JNI table override installed (%s)", info.dli_fname);
  dl_iterate_phdr(find_bridge, NULL);  // the translator is loaded with the VM; wrap its dlsym now
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
