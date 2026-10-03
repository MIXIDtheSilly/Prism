// arm64 windows handed to x86_64 EGL (libprism_jni only).
//
// Meta's compositor makes its output surface itself: its arm64 code links Horizon's arm64 libgui,
// creates a SurfaceFlinger layer and passes that layer's Surface (an ANativeWindow) to
// eglCreateWindowSurface. EGL is the x86_64 one, reached through the translator, and it calls the
// window's hooks directly: arm64 code, run as x86_64. Driving that window from x86_64 wouldn't be
// enough either, since its buffers would have to be mapped by arm64 libui, and no arm64 gralloc
// exists here.
//
// So EGL gets an x86_64 window instead: Prism creates a layer of the same size through Java's
// SurfaceControl, shows it above everything, and passes its Surface on. The arm64 layer stays
// empty. The translator binds the x86_64 functions arm64 code calls with dlsym; Prism wraps its
// dlsym (prism_jni.c) and hands out the functions below for the ones that take a window.

#ifndef PRISM_STUB_LIB

#include "prism_guest.h"

#include <EGL/egl.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <dlfcn.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define TAG "PrismJNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

// The start of system/window.h's ANativeWindow.
typedef struct {
  int magic;
  int version;
  void *reserved[4];
  void (*incRef)(void *);
  void (*decRef)(void *);
  uint32_t flags;
  int min_swap_interval;
  int max_swap_interval;
  float xdpi;
  float ydpi;
  intptr_t oem[4];
  void *set_swap_interval;
  void *dequeue_buffer_deprecated;
  void *lock_buffer_deprecated;
  void *queue_buffer_deprecated;
  void *query;  // int (*)(const ANativeWindow *, int what, int *value)
} WindowHead;
#define WINDOW_MAGIC 0x5f776e64  // '_wnd'
enum { NATIVE_WINDOW_DEFAULT_WIDTH = 6, NATIVE_WINDOW_DEFAULT_HEIGHT = 7 };

#define MAX_WINDOWS 16
static struct {
  const void *guest;
  ANativeWindow *host;
} g_windows[MAX_WINDOWS];
static int g_window_count;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int failed(JNIEnv *env, const char *what) {
  if (!(*env)->ExceptionCheck(env)) return 0;
  (*env)->ExceptionDescribe(env);
  (*env)->ExceptionClear(env);
  LOGW("host window: %s failed", what);
  return 1;
}

// A layer on top of everything with a buffer queue of width x height; its window, or NULL.
static ANativeWindow *create_host_window(JNIEnv *env, const char *name, int width, int height) {
  jclass builder_class = (*env)->FindClass(env, "android/view/SurfaceControl$Builder");
  jclass tx_class = (*env)->FindClass(env, "android/view/SurfaceControl$Transaction");
  jclass surface_class = (*env)->FindClass(env, "android/view/Surface");
  if (failed(env, "FindClass")) return NULL;
  const char *builder_sig = "Landroid/view/SurfaceControl$Builder;";
  char sig[160];
  jobject builder = (*env)->NewObject(env, builder_class, (*env)->GetMethodID(env, builder_class, "<init>", "()V"));
  jstring jname = (*env)->NewStringUTF(env, name);
  snprintf(sig, sizeof sig, "(Ljava/lang/String;)%s", builder_sig);
  (*env)->CallObjectMethod(env, builder, (*env)->GetMethodID(env, builder_class, "setName", sig), jname);
  snprintf(sig, sizeof sig, "(II)%s", builder_sig);
  (*env)->CallObjectMethod(env, builder, (*env)->GetMethodID(env, builder_class, "setBufferSize", sig), width, height);
  jobject control = (*env)->CallObjectMethod(
      env, builder, (*env)->GetMethodID(env, builder_class, "build", "()Landroid/view/SurfaceControl;"));
  if (failed(env, "SurfaceControl.Builder") || !control) return NULL;

  const char *tx_sig = "Landroid/view/SurfaceControl$Transaction;";
  jobject tx = (*env)->NewObject(env, tx_class, (*env)->GetMethodID(env, tx_class, "<init>", "()V"));
  snprintf(sig, sizeof sig, "(Landroid/view/SurfaceControl;I)%s", tx_sig);
  (*env)->CallObjectMethod(env, tx, (*env)->GetMethodID(env, tx_class, "setLayer", sig), control, (jint)0x7fffffff);
  snprintf(sig, sizeof sig, "(Landroid/view/SurfaceControl;Z)%s", tx_sig);
  (*env)->CallObjectMethod(env, tx, (*env)->GetMethodID(env, tx_class, "setVisibility", sig), control, JNI_TRUE);
  (*env)->CallVoidMethod(env, tx, (*env)->GetMethodID(env, tx_class, "apply", "()V"));
  if (failed(env, "SurfaceControl.Transaction")) return NULL;

  jobject surface = (*env)->NewObject(
      env, surface_class, (*env)->GetMethodID(env, surface_class, "<init>", "(Landroid/view/SurfaceControl;)V"), control);
  if (failed(env, "Surface") || !surface) return NULL;
  ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
  // Kept for the process's life: collecting the SurfaceControl would remove the layer.
  (*env)->NewGlobalRef(env, control);
  (*env)->NewGlobalRef(env, surface);
  return window;
}

static ANativeWindow *host_window(const char *name, int width, int height) {
  typedef jint (*GetCreatedJavaVMs)(JavaVM **, jsize, jsize *);
  GetCreatedJavaVMs get_vms = (GetCreatedJavaVMs)dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
  JavaVM *vm = NULL;
  jsize count = 0;
  if (!get_vms || get_vms(&vm, 1, &count) != JNI_OK || count < 1) return NULL;
  JNIEnv *env = NULL;
  int attached = 0;
  if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
    if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return NULL;
    attached = 1;
  }
  (*env)->PushLocalFrame(env, 32);
  ANativeWindow *window = create_host_window(env, name, width, height);
  (*env)->PopLocalFrame(env, NULL);
  if (attached) (*vm)->DetachCurrentThread(vm);
  return window;
}

// win itself if it is x86_64, else the x86_64 window standing in for it.
static void *host_window_for(void *win) {
  const WindowHead *w = win;
  if (!w || w->magic != WINDOW_MAGIC || !prism_is_guest_code(w->query)) return win;
  pthread_mutex_lock(&g_lock);
  ANativeWindow *host = NULL;
  for (int i = 0; i < g_window_count && !host; i++)
    if (g_windows[i].guest == win) host = g_windows[i].host;
  if (!host) {
    int width = 0, height = 0;
    int (*query)(const void *, int, int *) = prism_guest_function(w->query, "IJIJ");
    if (query) {
      query(win, NATIVE_WINDOW_DEFAULT_WIDTH, &width);
      query(win, NATIVE_WINDOW_DEFAULT_HEIGHT, &height);
    }
    if (width <= 0 || height <= 0) {
      LOGW("arm64 window %p: no size (%d x %d), using 1920 x 1080", win, width, height);
      width = 1920, height = 1080;
    }
    char name[64];
    snprintf(name, sizeof name, "Prism arm64 window %dx%d", width, height);
    host = host_window(name, width, height);
    if (host && g_window_count < MAX_WINDOWS) {
      g_windows[g_window_count].guest = win;
      g_windows[g_window_count++].host = host;
    }
    LOGI("arm64 window %p handed to EGL: %s", win, host ? "drawing to a Prism layer instead" : "no layer, passed on");
  }
  pthread_mutex_unlock(&g_lock);
  return host ? (void *)host : win;
}

// The functions the translator calls for arm64 code with a window. Prism's replacements, handed
// out in their place (prism_window_function), pass arm64 windows' stand-ins on instead; refcounts
// stay the arm64 window's own.
static EGLSurface (*real_create_window_surface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
static EGLSurface (*real_create_platform_window_surface)(EGLDisplay, EGLConfig, void *, const EGLAttrib *);
static void (*real_acquire)(ANativeWindow *);
static void (*real_release)(ANativeWindow *);
static int32_t (*real_get_width)(ANativeWindow *);
static int32_t (*real_get_height)(ANativeWindow *);
static int32_t (*real_get_format)(ANativeWindow *);
static int32_t (*real_set_buffers_geometry)(ANativeWindow *, int32_t, int32_t, int32_t);
static int32_t (*real_set_buffers_transform)(ANativeWindow *, int32_t);
static int32_t (*real_set_buffers_data_space)(ANativeWindow *, int32_t);
static int32_t (*real_get_buffers_data_space)(ANativeWindow *);
static int32_t (*real_lock)(ANativeWindow *, ANativeWindow_Buffer *, ARect *);
static int32_t (*real_unlock_and_post)(ANativeWindow *);

// Configs without depth or stencil. The emulator's EGL offers three configs (RGB888, and RGBA8888
// with 24-bit depth and 8-bit stencil), and Meta's compositor looks for RGBA8888 with neither, an
// exact match. arm64 code sees a twin of each depth/stencil config that reports none; whatever is
// created from a twin uses the real config, whose extra buffers go unused.
#define CONFIG_TWIN ((uintptr_t)1 << 30)
static EGLBoolean (*real_get_configs)(EGLDisplay, EGLConfig *, EGLint, EGLint *);
static EGLBoolean (*real_get_config_attrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
static EGLContext (*real_create_context)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLSurface (*real_create_pbuffer_surface)(EGLDisplay, EGLConfig, const EGLint *);

static int is_twin(EGLConfig config) { return ((uintptr_t)config & CONFIG_TWIN) != 0; }
static EGLConfig real_config(EGLConfig config) { return (EGLConfig)((uintptr_t)config & ~CONFIG_TWIN); }

static int has_depth_or_stencil(EGLDisplay dpy, EGLConfig config) {
  EGLint depth = 0, stencil = 0;
  eglGetConfigAttrib(dpy, config, EGL_DEPTH_SIZE, &depth);
  eglGetConfigAttrib(dpy, config, EGL_STENCIL_SIZE, &stencil);
  return depth || stencil;
}

static EGLBoolean get_configs(EGLDisplay dpy, EGLConfig *configs, EGLint size, EGLint *count) {
  EGLint n = 0;
  if (!real_get_configs(dpy, NULL, 0, &n) || n <= 0) return real_get_configs(dpy, configs, size, count);
  EGLConfig all[2 * n];
  if (!real_get_configs(dpy, all, n, &n)) return EGL_FALSE;
  EGLint total = n;
  for (EGLint i = 0; i < n; i++)
    if (!is_twin(all[i]) && has_depth_or_stencil(dpy, all[i])) all[total++] = (EGLConfig)((uintptr_t)all[i] | CONFIG_TWIN);
  if (!configs) {
    *count = total;
    return EGL_TRUE;
  }
  *count = size < total ? size : total;
  for (EGLint i = 0; i < *count; i++) configs[i] = all[i];
  return EGL_TRUE;
}

static EGLBoolean get_config_attrib(EGLDisplay dpy, EGLConfig config, EGLint attribute, EGLint *value) {
  if (!is_twin(config)) return real_get_config_attrib(dpy, config, attribute, value);
  if (attribute == EGL_DEPTH_SIZE || attribute == EGL_STENCIL_SIZE) {
    *value = 0;
    return EGL_TRUE;
  }
  EGLBoolean ok = real_get_config_attrib(dpy, real_config(config), attribute, value);
  if (ok && attribute == EGL_CONFIG_ID) *value |= 0x10000;  // distinct from its real config's
  return ok;
}

static EGLContext create_context(EGLDisplay dpy, EGLConfig config, EGLContext share, const EGLint *attrs) {
  return real_create_context(dpy, real_config(config), share, attrs);
}

static EGLSurface create_pbuffer_surface(EGLDisplay dpy, EGLConfig config, const EGLint *attrs) {
  return real_create_pbuffer_surface(dpy, real_config(config), attrs);
}

static EGLBoolean (*real_choose_config)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);

// Logs the requests no config matches: what arm64 code needs that the emulator's EGL lacks.
static EGLBoolean choose_config(EGLDisplay dpy, const EGLint *attrs, EGLConfig *configs, EGLint size, EGLint *count) {
  EGLBoolean ok = real_choose_config(dpy, attrs, configs, size, count);
  if (ok && count && *count == 0 && attrs) {
    char text[512];
    size_t n = 0;
    for (const EGLint *a = attrs; a[0] != EGL_NONE && n + 24 < sizeof text; a += 2)
      n += (size_t)snprintf(text + n, sizeof text - n, " %#x=%#x", a[0], a[1]);
    text[n] = 0;
    LOGW("eglChooseConfig from arm64 code: no config for%s", text);
  }
  return ok;
}

// Surfaceless contexts (EGL_KHR_surfaceless_context), which Meta's code uses and the emulator's
// EGL lacks: a context made current with no surface gets a 1x1 pbuffer of its own config.
static EGLBoolean (*real_make_current)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*real_destroy_context)(EGLDisplay, EGLContext);
#define MAX_PBUFFERS 64
static struct {
  EGLDisplay display;
  EGLContext context;
  EGLSurface surface;
} g_pbuffers[MAX_PBUFFERS];

static EGLSurface context_pbuffer(EGLDisplay dpy, EGLContext ctx) {
  pthread_mutex_lock(&g_lock);
  EGLSurface surface = EGL_NO_SURFACE;
  int free_slot = -1;
  for (int i = 0; i < MAX_PBUFFERS && surface == EGL_NO_SURFACE; i++) {
    if (g_pbuffers[i].context == ctx && g_pbuffers[i].display == dpy) surface = g_pbuffers[i].surface;
    else if (free_slot < 0 && g_pbuffers[i].context == EGL_NO_CONTEXT) free_slot = i;
  }
  if (surface == EGL_NO_SURFACE) {
    EGLint id = 0, count = 0;
    EGLConfig config;
    const EGLint size[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    if (eglQueryContext(dpy, ctx, EGL_CONFIG_ID, &id)) {
      EGLint query[] = {EGL_CONFIG_ID, id, EGL_NONE};
      if (eglChooseConfig(dpy, query, &config, 1, &count) && count == 1)
        surface = eglCreatePbufferSurface(dpy, config, size);
    }
    if (surface != EGL_NO_SURFACE && free_slot >= 0) {
      g_pbuffers[free_slot].display = dpy;
      g_pbuffers[free_slot].context = ctx;
      g_pbuffers[free_slot].surface = surface;
    }
    LOGI("surfaceless context %p (config %d): %s", ctx, id, surface != EGL_NO_SURFACE ? "1x1 pbuffer" : "no pbuffer");
  }
  pthread_mutex_unlock(&g_lock);
  return surface;
}

static EGLBoolean make_current(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx) {
  if (ctx != EGL_NO_CONTEXT && draw == EGL_NO_SURFACE && read == EGL_NO_SURFACE) {
    EGLSurface pbuffer = context_pbuffer(dpy, ctx);
    if (pbuffer != EGL_NO_SURFACE) draw = read = pbuffer;
  }
  return real_make_current(dpy, draw, read, ctx);
}

static EGLBoolean destroy_context(EGLDisplay dpy, EGLContext ctx) {
  EGLBoolean ok = real_destroy_context(dpy, ctx);
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < MAX_PBUFFERS; i++) {
    if (g_pbuffers[i].context != ctx || g_pbuffers[i].display != dpy) continue;
    eglDestroySurface(dpy, g_pbuffers[i].surface);  // released once no longer current
    g_pbuffers[i].context = EGL_NO_CONTEXT;
  }
  pthread_mutex_unlock(&g_lock);
  return ok;
}

static EGLSurface create_window_surface(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const EGLint *attrs) {
  return real_create_window_surface(dpy, real_config(config), host_window_for(win), attrs);
}

static EGLSurface create_platform_window_surface(EGLDisplay dpy, EGLConfig config, void *win, const EGLAttrib *attrs) {
  return real_create_platform_window_surface(dpy, real_config(config), host_window_for(win), attrs);
}

// base->incRef or decRef, called the way the window's code needs.
static void ref(ANativeWindow *window, void (*real)(ANativeWindow *), size_t offset) {
  const WindowHead *w = (const WindowHead *)window;
  void *hook = w && w->magic == WINDOW_MAGIC ? *(void **)((char *)window + offset) : NULL;
  void (*guest)(void *) = prism_guest_function(hook, "VJ");
  if (guest)
    guest(window);
  else
    real(window);
}

static void acquire(ANativeWindow *w) { ref(w, real_acquire, offsetof(WindowHead, incRef)); }
static void release(ANativeWindow *w) { ref(w, real_release, offsetof(WindowHead, decRef)); }
static int32_t get_width(ANativeWindow *w) { return real_get_width(host_window_for(w)); }
static int32_t get_height(ANativeWindow *w) { return real_get_height(host_window_for(w)); }
static int32_t get_format(ANativeWindow *w) { return real_get_format(host_window_for(w)); }
static int32_t set_buffers_geometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format) {
  return real_set_buffers_geometry(host_window_for(w), width, height, format);
}
static int32_t set_buffers_transform(ANativeWindow *w, int32_t t) { return real_set_buffers_transform(host_window_for(w), t); }
static int32_t set_buffers_data_space(ANativeWindow *w, int32_t d) { return real_set_buffers_data_space(host_window_for(w), d); }
static int32_t get_buffers_data_space(ANativeWindow *w) { return real_get_buffers_data_space(host_window_for(w)); }
static int32_t lock(ANativeWindow *w, ANativeWindow_Buffer *b, ARect *r) { return real_lock(host_window_for(w), b, r); }
static int32_t unlock_and_post(ANativeWindow *w) { return real_unlock_and_post(host_window_for(w)); }

#define HOOK(name, fn, real) {name, (void *)fn, (void **)&real}
static const struct {
  const char *name;
  void *fn;
  void **real;
} kHooks[] = {
    HOOK("eglCreateWindowSurface", create_window_surface, real_create_window_surface),
    HOOK("eglChooseConfig", choose_config, real_choose_config),
    HOOK("eglGetConfigs", get_configs, real_get_configs),
    HOOK("eglGetConfigAttrib", get_config_attrib, real_get_config_attrib),
    HOOK("eglCreateContext", create_context, real_create_context),
    HOOK("eglCreatePbufferSurface", create_pbuffer_surface, real_create_pbuffer_surface),
    HOOK("eglMakeCurrent", make_current, real_make_current),
    HOOK("eglDestroyContext", destroy_context, real_destroy_context),
    HOOK("eglCreatePlatformWindowSurface", create_platform_window_surface, real_create_platform_window_surface),
    HOOK("ANativeWindow_acquire", acquire, real_acquire),
    HOOK("ANativeWindow_release", release, real_release),
    HOOK("ANativeWindow_getWidth", get_width, real_get_width),
    HOOK("ANativeWindow_getHeight", get_height, real_get_height),
    HOOK("ANativeWindow_getFormat", get_format, real_get_format),
    HOOK("ANativeWindow_setBuffersGeometry", set_buffers_geometry, real_set_buffers_geometry),
    HOOK("ANativeWindow_setBuffersTransform", set_buffers_transform, real_set_buffers_transform),
    HOOK("ANativeWindow_setBuffersDataSpace", set_buffers_data_space, real_set_buffers_data_space),
    HOOK("ANativeWindow_getBuffersDataSpace", get_buffers_data_space, real_get_buffers_data_space),
    HOOK("ANativeWindow_lock", lock, real_lock),
    HOOK("ANativeWindow_unlockAndPost", unlock_and_post, real_unlock_and_post),
};

void *prism_window_function(const char *name, void *real) {
  for (size_t i = 0; i < sizeof kHooks / sizeof *kHooks; i++) {
    if (strcmp(name, kHooks[i].name)) continue;
    __atomic_store_n(kHooks[i].real, real, __ATOMIC_RELEASE);
    return kHooks[i].fn;
  }
  return NULL;
}

#endif
