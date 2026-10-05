// Prism's GLES layer (libgles_prism.so, loaded by Android's EGL loader from /data/local/debug/gles,
// named by debug.gles.layers): GL_EXT_memory_object_fd for the emulator's GLES, which has none.
//
// Horizon's runtime gives an OpenGL ES app its swapchain images as the compositor's memory, file
// descriptors from vkGetMemoryFdKHR, which it imports with GL_EXT_memory_object_fd (or EGL dma-buf
// import). Prism's Vulkan driver (native/vulkan_prism) makes such an fd one end of a Unix socket
// holding an AHardwareBuffer: the image itself for a single-layer image, or for a layered one
// (multiview: an eye a layer) a mirror as tall as its layers stacked, which the compositor copies
// out of before it reads the image.
//
// This layer advertises GL_EXT_memory_object and GL_EXT_memory_object_fd and implements them:
//
//   * glImportMemoryFdEXT receives the AHardwareBuffer from the socket;
//   * glTexStorageMem2DEXT / 3DEXT give the bound texture ordinary storage of the format and shape
//     asked for (sRGB, layers), and make an EGL image of the buffer;
//   * work that may write such a texture marks it: binding a framebuffer it's attached to, or
//     attaching it. Before the app's next fence (glFenceSync, eglCreateSyncKHR, glFinish: the
//     runtime fences an image when the app releases it) the marked textures are copied into their
//     buffers, layers stacked.
//
// The copy is a draw (texelFetch, no filtering) in a context of the layer's own, sharing the app's
// textures, so none of the app's GL state changes; fences order it after the app's work and before
// the app's next. An sRGB texture reads back linear, so the copy encodes it again: the buffer gets
// the texture's bytes. The emulator's GLES has no copy_image or sRGB_decode for a raw copy.
//
// The emulator's GLES advertises the PC's GL_OVR_multiview2, but its encoder has no multiview
// functions: glFramebufferTextureMultiviewOVR is Android's "unimplemented" stub, and an app that
// takes the extension at its word gets an incomplete framebuffer. Meta's Guardian doesn't even ask:
// it renders multiview, always. The layer emulates GL_OVR_multiview and GL_OVR_multiview2:
//
//   * glFramebufferTextureMultiviewOVR attaches the base view's layer, and remembers the views;
//   * a draw or clear into a framebuffer with views attached is made once a view, the view's
//     layer attached each time (the base view's last);
//   * a shader's gl_ViewID_OVR becomes a uniform the layer sets for each view, and its
//     extension directive and num_views layout go.
//
// Multisampled multiview attachments (glFramebufferTextureMultisampleMultiviewOVR) are made single
// sampled; the extensions for them stay unadvertised.
//
// Every other function passes through. Apps that don't import memory or render multiview pay a
// lookup per framebuffer binding and program change.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAG "PrismGLES"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define EXPORT __attribute__((visibility("default")))

#ifndef GL_MAX_VIEWS_OVR
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR 0x9630
#define GL_MAX_VIEWS_OVR 0x9631
#define GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_BASE_VIEW_INDEX_OVR 0x9632
#endif

typedef void *(*NextProc)(void *layer, const char *name);
typedef void (*Function)(void);

// The functions below this layer that the layer wraps, and some it calls.
static struct {
  const GLubyte *(*GetString)(GLenum);
  void (*GetIntegerv)(GLenum, GLint *);
  void (*BindFramebuffer)(GLenum, GLuint);
  void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
  void (*FramebufferTextureLayer)(GLenum, GLenum, GLuint, GLint, GLint);
  void (*FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
  void (*GetFramebufferAttachmentParameteriv)(GLenum, GLenum, GLenum, GLint *);
  void (*DeleteTextures)(GLsizei, const GLuint *);
  void (*DeleteFramebuffers)(GLsizei, const GLuint *);
  GLenum (*CheckFramebufferStatus)(GLenum);
  void (*ShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
  void (*LinkProgram)(GLuint);
  void (*UseProgram)(GLuint);
  void (*DeleteProgram)(GLuint);
  void (*Clear)(GLbitfield);
  void (*ClearBufferiv)(GLenum, GLint, const GLint *);
  void (*ClearBufferuiv)(GLenum, GLint, const GLuint *);
  void (*ClearBufferfv)(GLenum, GLint, const GLfloat *);
  void (*ClearBufferfi)(GLenum, GLint, GLfloat, GLint);
  void (*DrawArrays)(GLenum, GLint, GLsizei);
  void (*DrawElements)(GLenum, GLsizei, GLenum, const void *);
  void (*DrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei);
  void (*DrawElementsInstanced)(GLenum, GLsizei, GLenum, const void *, GLsizei);
  void (*DrawRangeElements)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void *);
  void (*DrawArraysIndirect)(GLenum, const void *);
  void (*DrawElementsIndirect)(GLenum, GLenum, const void *);
  GLsync (*FenceSync)(GLenum, GLbitfield);
  void (*Finish)(void);
  EGLSyncKHR (*CreateSyncKHR)(EGLDisplay, EGLenum, const EGLint *);
  EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext);
  Function (*GetProcAddress)(const char *);
} next;

// Recursive: the layer's own GL calls (the copy's, a uniform's location) may come back through it.
static pthread_mutex_t g_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

// --- each context's bindings ------------------------------------------------------------------------

#define MAX_STATES 64
typedef struct {
  EGLContext context;  // NULL: free
  GLuint draw, read;   // framebuffers
  GLuint program;
} State;
static State g_states[MAX_STATES];

// The context's, made if need be (the lock held). Contexts start with nothing bound.
static State *state_of(EGLContext context) {
  State *free_state = NULL;
  if (context == EGL_NO_CONTEXT) return NULL;
  for (int i = 0; i < MAX_STATES; i++) {
    if (g_states[i].context == context) return &g_states[i];
    if (!g_states[i].context && !free_state) free_state = &g_states[i];
  }
  if (free_state) *free_state = (State){context};
  else LOGE("more than %d contexts", MAX_STATES);
  return free_state;
}

// --- memory objects ---------------------------------------------------------------------------------

#define MAX_MEMORY 256
static struct {
  GLuint name;  // 0: free
  AHardwareBuffer *buffer;
  GLint dedicated;
} g_memory[MAX_MEMORY];
static GLuint g_memory_names;

static int memory_index(GLuint name) {
  for (int i = 0; name && i < MAX_MEMORY; i++)
    if (g_memory[i].name == name) return i;
  return -1;
}

static void GL_APIENTRY prism_CreateMemoryObjectsEXT(GLsizei n, GLuint *names) {
  pthread_mutex_lock(&g_lock);
  for (GLsizei k = 0; k < n; k++) {
    names[k] = 0;
    for (int i = 0; i < MAX_MEMORY && !names[k]; i++)
      if (!g_memory[i].name) {
        g_memory[i].name = names[k] = ++g_memory_names;
        g_memory[i].buffer = NULL;
        g_memory[i].dedicated = 0;
      }
    if (!names[k]) LOGE("glCreateMemoryObjectsEXT: more than %d memory objects", MAX_MEMORY);
  }
  pthread_mutex_unlock(&g_lock);
}

static void GL_APIENTRY prism_DeleteMemoryObjectsEXT(GLsizei n, const GLuint *names) {
  pthread_mutex_lock(&g_lock);
  for (GLsizei k = 0; k < n; k++) {
    int i = memory_index(names[k]);
    if (i < 0) continue;
    if (g_memory[i].buffer) AHardwareBuffer_release(g_memory[i].buffer);
    g_memory[i].name = 0, g_memory[i].buffer = NULL;
  }
  pthread_mutex_unlock(&g_lock);
}

static GLboolean GL_APIENTRY prism_IsMemoryObjectEXT(GLuint name) {
  pthread_mutex_lock(&g_lock);
  GLboolean is = memory_index(name) >= 0;
  pthread_mutex_unlock(&g_lock);
  return is;
}

static void GL_APIENTRY prism_MemoryObjectParameterivEXT(GLuint name, GLenum pname, const GLint *params) {
  pthread_mutex_lock(&g_lock);
  int i = memory_index(name);
  if (i >= 0 && pname == GL_DEDICATED_MEMORY_OBJECT_EXT) g_memory[i].dedicated = params[0];
  pthread_mutex_unlock(&g_lock);
}

static void GL_APIENTRY prism_GetMemoryObjectParameterivEXT(GLuint name, GLenum pname, GLint *params) {
  pthread_mutex_lock(&g_lock);
  int i = memory_index(name);
  if (i >= 0 && pname == GL_DEDICATED_MEMORY_OBJECT_EXT) params[0] = g_memory[i].dedicated;
  if (i >= 0 && pname == GL_PROTECTED_MEMORY_OBJECT_EXT) params[0] = 0;
  pthread_mutex_unlock(&g_lock);
}

// The fd is Prism's Vulkan driver's: a socket with the memory's AHardwareBuffer in it. Importing
// takes the fd.
static void GL_APIENTRY prism_ImportMemoryFdEXT(GLuint name, GLuint64 size, GLenum type, GLint fd) {
  AHardwareBuffer *buffer = NULL;
  if (type != GL_HANDLE_TYPE_OPAQUE_FD_EXT) {
    LOGE("glImportMemoryFdEXT: handle type %#x; only opaque fds are supported", type);
    return;
  }
  if (AHardwareBuffer_recvHandleFromUnixSocket(fd, &buffer) != 0 || !buffer) {
    LOGE("glImportMemoryFdEXT: fd %d holds no AHardwareBuffer; only memory exported by Prism's Vulkan driver can be imported",
         fd);
    return;
  }
  close(fd);
  pthread_mutex_lock(&g_lock);
  int i = memory_index(name);
  if (i >= 0) {
    if (g_memory[i].buffer) AHardwareBuffer_release(g_memory[i].buffer);
    g_memory[i].buffer = buffer;
    buffer = NULL;
  }
  pthread_mutex_unlock(&g_lock);
  if (buffer) {
    LOGE("glImportMemoryFdEXT: no memory object %u", name);
    AHardwareBuffer_release(buffer);
  }
  (void)size;
}

// --- textures in imported memory --------------------------------------------------------------------

#define MAX_IMPORTS 128
typedef struct {
  EGLContext context;  // the app's, which made the texture; NULL: free
  EGLDisplay display;
  GLuint texture;
  GLenum target;
  GLsizei width, height, layers;
  int encode;  // sRGB: the copy encodes what texelFetch decoded
  EGLImageKHR image;  // of the buffer
  GLuint shadow, framebuffer;  // the image's texture and a framebuffer of it, in the copy context
  int dirty;
  int logged;    // its first copy
  int attaches;  // attachments logged
} Import;
static Import g_imports[MAX_IMPORTS];
static int g_import_count;  // read without the lock: with none there's nothing to track

#define MAX_ATTACHMENTS 256
static struct {
  EGLContext context;  // NULL: free
  GLuint framebuffer;
  GLenum attachment;
  GLuint texture;
} g_attachments[MAX_ATTACHMENTS];

static int tracking(void) { return __atomic_load_n(&g_import_count, __ATOMIC_RELAXED) > 0; }

static Import *import_of(EGLContext context, GLuint texture) {
  for (int i = 0; texture && i < MAX_IMPORTS; i++)
    if (g_imports[i].context == context && g_imports[i].texture == texture) return &g_imports[i];
  return NULL;
}

static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;
static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC native_client_buffer;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture;

static int load_egl_extensions(void) {
  if (!create_image) {
    create_image = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    native_client_buffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    image_target_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  }
  return create_image && destroy_image && native_client_buffer && image_target_texture;
}

static void texture_storage(GLenum target, GLsizei levels, GLenum format, GLsizei width, GLsizei height,
                            GLsizei layers, GLuint memory, const char *function) {
  GLint texture = 0;
  if (layers) {
    glTexStorage3D(target, levels, format, width, height, layers);
    glGetIntegerv(target == GL_TEXTURE_2D_ARRAY ? GL_TEXTURE_BINDING_2D_ARRAY : GL_TEXTURE_BINDING_3D, &texture);
  } else {
    glTexStorage2D(target, levels, format, width, height);
    glGetIntegerv(target == GL_TEXTURE_2D ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_CUBE_MAP, &texture);
  }
  if (!layers) layers = 1;
  pthread_mutex_lock(&g_lock);
  int m = memory_index(memory);
  AHardwareBuffer *buffer = m >= 0 ? g_memory[m].buffer : NULL;
  if (buffer) AHardwareBuffer_acquire(buffer);
  pthread_mutex_unlock(&g_lock);
  if (!buffer) {
    LOGE("%s: memory object %u holds no imported memory", function, memory);
    return;
  }
  AHardwareBuffer_Desc desc;
  AHardwareBuffer_describe(buffer, &desc);
  int encode = format == GL_SRGB8_ALPHA8 || format == GL_SRGB8;
  int color = format == GL_SRGB8_ALPHA8 || format == GL_SRGB8 || format == GL_RGBA8 || format == GL_RGB8 ||
              format == GL_RGBA16F || format == GL_RGB10_A2 || format == GL_R11F_G11F_B10F;
  EGLDisplay display = eglGetCurrentDisplay();
  EGLImageKHR image = EGL_NO_IMAGE_KHR;
  if ((target != GL_TEXTURE_2D && target != GL_TEXTURE_2D_ARRAY) || !color)
    LOGE("%s: texture %d (target %#x, format %#x) isn't shared: only 2D color textures and arrays are", function,
         texture, target, format);
  else if (desc.width != (uint32_t)width || desc.height != (uint32_t)(height * layers))
    LOGE("%s: texture %d is %d x %d, %d layers; its memory is a %u x %u buffer, which can't hold it", function,
         texture, width, height, layers, desc.width, desc.height);
  else if (!load_egl_extensions())
    LOGE("%s: no EGL image functions", function);
  else {
    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    image = create_image(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, native_client_buffer(buffer), attributes);
    if (image == EGL_NO_IMAGE_KHR) LOGE("%s: no EGL image of its buffer (%#x)", function, eglGetError());
  }
  AHardwareBuffer_release(buffer);  // the image holds it
  if (image == EGL_NO_IMAGE_KHR) return;

  EGLContext context = eglGetCurrentContext();
  pthread_mutex_lock(&g_lock);
  Import *t = import_of(context, (GLuint)texture);
  for (int i = 0; !t && i < MAX_IMPORTS; i++)
    if (!g_imports[i].context) t = &g_imports[i];
  if (t) {
    if (t->context && t->image) destroy_image(t->display, t->image);  // storage of an immutable texture again: an error
    if (!t->context) __atomic_add_fetch(&g_import_count, 1, __ATOMIC_RELAXED);
    *t = (Import){context, display, (GLuint)texture, target, width, height, layers, encode, image, 0, 0, 1, 0};
  }
  pthread_mutex_unlock(&g_lock);
  if (!t) {
    LOGE("%s: more than %d textures in imported memory", function, MAX_IMPORTS);
    destroy_image(display, image);
    return;
  }
  LOGI("%s: texture %d, %d x %d, %d layers, format %#x, shares a %u x %u buffer", function, texture, width, height,
       layers, format, desc.width, desc.height);
}

static void GL_APIENTRY prism_TexStorageMem2DEXT(GLenum target, GLsizei levels, GLenum format, GLsizei width,
                                                 GLsizei height, GLuint memory, GLuint64 offset) {
  if (offset) LOGE("glTexStorageMem2DEXT: offset %llu ignored", (unsigned long long)offset);
  texture_storage(target, levels, format, width, height, 0, memory, "glTexStorageMem2DEXT");
}

static void GL_APIENTRY prism_TexStorageMem3DEXT(GLenum target, GLsizei levels, GLenum format, GLsizei width,
                                                 GLsizei height, GLsizei depth, GLuint memory, GLuint64 offset) {
  if (offset) LOGE("glTexStorageMem3DEXT: offset %llu ignored", (unsigned long long)offset);
  texture_storage(target, levels, format, width, height, depth, memory, "glTexStorageMem3DEXT");
}

static void GL_APIENTRY prism_BufferStorageMemEXT(GLenum target, GLsizeiptr size, GLuint memory, GLuint64 offset) {
  static int logged;
  if (!logged++) LOGE("glBufferStorageMemEXT: buffers in imported memory aren't shared");
  glBufferData(target, size, NULL, GL_DYNAMIC_DRAW);
  (void)memory, (void)offset;
}

// --- marking textures written -----------------------------------------------------------------------

static void mark_framebuffer(EGLContext context, GLuint framebuffer) {
  for (int i = 0; i < MAX_ATTACHMENTS; i++)
    if (g_attachments[i].context == context && g_attachments[i].framebuffer == framebuffer) {
      Import *t = import_of(context, g_attachments[i].texture);
      if (t) t->dirty = 1;
    }
}

static GLuint draw_framebuffer(GLenum target) {
  GLint framebuffer = -1;
  if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
  return (GLuint)framebuffer;
}

static void attach(GLenum target, GLenum attachment, GLuint texture, const char *how) {
  GLuint framebuffer = draw_framebuffer(target);
  if (framebuffer == (GLuint)-1 || !framebuffer) return;
  EGLContext context = eglGetCurrentContext();
  pthread_mutex_lock(&g_lock);
  int slot = -1;
  for (int i = 0; i < MAX_ATTACHMENTS; i++) {
    if (g_attachments[i].context == context && g_attachments[i].framebuffer == framebuffer &&
        g_attachments[i].attachment == attachment)
      g_attachments[i].context = NULL;
    if (!g_attachments[i].context && slot < 0) slot = i;
  }
  Import *t = import_of(context, texture);
  if (t && t->attaches < 2) {
    t->attaches++;
    LOGI("texture %u attached to framebuffer %u at %#x (%s)", texture, framebuffer, attachment, how);
  }
  if (t && slot >= 0) {
    g_attachments[slot].context = context;
    g_attachments[slot].framebuffer = framebuffer;
    g_attachments[slot].attachment = attachment;
    g_attachments[slot].texture = texture;
    t->dirty = 1;
  }
  pthread_mutex_unlock(&g_lock);
}

static void GL_APIENTRY prism_BindFramebuffer(GLenum target, GLuint framebuffer) {
  next.BindFramebuffer(target, framebuffer);
  EGLContext context = eglGetCurrentContext();
  pthread_mutex_lock(&g_lock);
  State *s = state_of(context);
  if (s && target != GL_READ_FRAMEBUFFER) s->draw = framebuffer;
  if (s && target != GL_DRAW_FRAMEBUFFER) s->read = framebuffer;
  if (tracking() && framebuffer && target != GL_READ_FRAMEBUFFER) mark_framebuffer(context, framebuffer);
  pthread_mutex_unlock(&g_lock);
}

static void forget_views(GLenum target, GLenum attachment);

static void GL_APIENTRY prism_FramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
                                                   GLint level) {
  next.FramebufferTexture2D(target, attachment, textarget, texture, level);
  forget_views(target, attachment);
  if (tracking()) attach(target, attachment, texture, "2D");
}

static void GL_APIENTRY prism_FramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level,
                                                      GLint layer) {
  next.FramebufferTextureLayer(target, attachment, texture, level, layer);
  forget_views(target, attachment);
  if (tracking()) attach(target, attachment, texture, "layer");
}

static void GL_APIENTRY prism_FramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget,
                                                      GLuint renderbuffer) {
  next.FramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer);
  forget_views(target, attachment);
}

// Deleted textures and framebuffers stop being tracked; a shadow's texture and framebuffer go with
// the next copy (they live in the copy context).
#define MAX_RETIRED 64
static struct {
  EGLContext context;
  GLuint shadow, framebuffer;
} g_retired[MAX_RETIRED];

static void forget_deleted(GLsizei n, const GLuint *textures, const GLuint *framebuffers);

static void GL_APIENTRY prism_DeleteTextures(GLsizei n, const GLuint *textures) {
  forget_deleted(n, textures, NULL);
  if (tracking()) {
    EGLContext context = eglGetCurrentContext();
    pthread_mutex_lock(&g_lock);
    for (GLsizei k = 0; k < n; k++) {
      Import *t = import_of(context, textures[k]);
      if (!t) continue;
      destroy_image(t->display, t->image);
      for (int i = 0; t->shadow && i < MAX_RETIRED; i++)
        if (!g_retired[i].context) {
          g_retired[i].context = context, g_retired[i].shadow = t->shadow, g_retired[i].framebuffer = t->framebuffer;
          break;
        }
      for (int i = 0; i < MAX_ATTACHMENTS; i++)
        if (g_attachments[i].context == context && g_attachments[i].texture == textures[k]) g_attachments[i].context = NULL;
      t->context = NULL;
      __atomic_sub_fetch(&g_import_count, 1, __ATOMIC_RELAXED);
    }
    pthread_mutex_unlock(&g_lock);
  }
  next.DeleteTextures(n, textures);
}

// An incomplete framebuffer that holds a shared texture is logged, with what's attached to it.
static GLenum GL_APIENTRY prism_CheckFramebufferStatus(GLenum target) {
  GLenum status = next.CheckFramebufferStatus(target);
  if (status == GL_FRAMEBUFFER_COMPLETE || !tracking()) return status;
  GLuint framebuffer = draw_framebuffer(target);
  EGLContext context = eglGetCurrentContext();
  int shared = 0;
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < MAX_ATTACHMENTS; i++)
    shared |= g_attachments[i].context == context && g_attachments[i].framebuffer == framebuffer;
  pthread_mutex_unlock(&g_lock);
  if (!shared) return status;
  LOGE("framebuffer %u is incomplete (%#x)", framebuffer, status);
  static const GLenum attachments[] = {GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
  for (size_t i = 0; i < sizeof attachments / sizeof *attachments; i++) {
    GLint type = 0, name = 0, layer = 0, samples = 0, width = 0;
    glGetFramebufferAttachmentParameteriv(target, attachments[i], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
    if (type == GL_NONE) continue;
    glGetFramebufferAttachmentParameteriv(target, attachments[i], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
    if (type == GL_TEXTURE)
      glGetFramebufferAttachmentParameteriv(target, attachments[i], GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LAYER, &layer);
    if (type == GL_RENDERBUFFER) {
      GLint bound = 0;
      glGetIntegerv(GL_RENDERBUFFER_BINDING, &bound);
      glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)name);
      glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &samples);
      glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
      glBindRenderbuffer(GL_RENDERBUFFER, (GLuint)bound);
    }
    LOGE("  %#x: %s %d, layer %d, %d samples, width %d", attachments[i], type == GL_TEXTURE ? "texture" : "renderbuffer",
         name, layer, samples, width);
  }
  return status;
}

static void GL_APIENTRY prism_DeleteFramebuffers(GLsizei n, const GLuint *framebuffers) {
  forget_deleted(n, NULL, framebuffers);
  if (tracking()) {
    EGLContext context = eglGetCurrentContext();
    pthread_mutex_lock(&g_lock);
    for (GLsizei k = 0; k < n; k++)
      for (int i = 0; i < MAX_ATTACHMENTS; i++)
        if (g_attachments[i].context == context && g_attachments[i].framebuffer == framebuffers[k])
          g_attachments[i].context = NULL;
    pthread_mutex_unlock(&g_lock);
  }
  next.DeleteFramebuffers(n, framebuffers);
}

// --- multiview ------------------------------------------------------------------------------------------

#define MAX_VIEWS 4  // GL_MAX_VIEWS_OVR
#define MAX_VIEW_ATTACHMENTS 64
static struct {
  EGLContext context;  // NULL: free
  GLuint framebuffer;
  GLenum attachment;
  GLuint texture;
  GLint level, base, views;
} g_views[MAX_VIEW_ATTACHMENTS];
static int g_view_count;  // read without the lock: with none, draws pass straight through
static int g_rewritten;   // shaders rewritten: until one is, no program has the view uniform

static const char kViewUniform[] = "prism_ViewID";

static GLuint bound(State *s, GLenum target) {
  return !s ? 0 : target == GL_READ_FRAMEBUFFER ? s->read : s->draw;
}

static void forget_view(int i) {
  g_views[i].context = NULL;
  __atomic_sub_fetch(&g_view_count, 1, __ATOMIC_RELAXED);
}

// Something else attached there (the lock not held).
static void forget_views(GLenum target, GLenum attachment) {
  if (!__atomic_load_n(&g_view_count, __ATOMIC_RELAXED)) return;
  EGLContext context = eglGetCurrentContext();
  pthread_mutex_lock(&g_lock);
  GLuint framebuffer = bound(state_of(context), target);
  for (int i = 0; framebuffer && i < MAX_VIEW_ATTACHMENTS; i++)
    if (g_views[i].context == context && g_views[i].framebuffer == framebuffer &&
        (g_views[i].attachment == attachment ||
         (attachment == GL_DEPTH_STENCIL_ATTACHMENT &&
          (g_views[i].attachment == GL_DEPTH_ATTACHMENT || g_views[i].attachment == GL_STENCIL_ATTACHMENT))))
      forget_view(i);
  pthread_mutex_unlock(&g_lock);
}

// Deleted textures and framebuffers: their views go, and a deleted framebuffer is unbound.
static void forget_deleted(GLsizei n, const GLuint *textures, const GLuint *framebuffers) {
  EGLContext context = eglGetCurrentContext();
  pthread_mutex_lock(&g_lock);
  State *s = framebuffers ? state_of(context) : NULL;
  for (GLsizei k = 0; k < n; k++) {
    GLuint name = textures ? textures[k] : framebuffers[k];
    if (!name) continue;
    if (s && s->draw == name) s->draw = 0;
    if (s && s->read == name) s->read = 0;
    for (int i = 0; __atomic_load_n(&g_view_count, __ATOMIC_RELAXED) && i < MAX_VIEW_ATTACHMENTS; i++)
      if (g_views[i].context == context && (textures ? g_views[i].texture : g_views[i].framebuffer) == name)
        forget_view(i);
  }
  pthread_mutex_unlock(&g_lock);
}

static void GL_APIENTRY prism_FramebufferTextureMultiviewOVR(GLenum target, GLenum attachment, GLuint texture,
                                                             GLint level, GLint base, GLsizei views) {
  next.FramebufferTextureLayer(target, attachment, texture, level, base);
  forget_views(target, attachment);
  if (views > MAX_VIEWS) LOGE("glFramebufferTextureMultiviewOVR: %d views; at most %d are drawn", views, MAX_VIEWS);
  EGLContext context = eglGetCurrentContext();
  pthread_mutex_lock(&g_lock);
  GLuint framebuffer = bound(state_of(context), target);
  int slot = -1;
  for (int i = 0; framebuffer && texture && views > 1 && slot < 0 && i < MAX_VIEW_ATTACHMENTS; i++)
    if (!g_views[i].context) slot = i;
  if (slot >= 0) {
    g_views[slot].context = context;
    g_views[slot].framebuffer = framebuffer;
    g_views[slot].attachment = attachment;
    g_views[slot].texture = texture;
    g_views[slot].level = level;
    g_views[slot].base = base;
    g_views[slot].views = views < MAX_VIEWS ? views : MAX_VIEWS;
    __atomic_add_fetch(&g_view_count, 1, __ATOMIC_RELAXED);
  } else if (framebuffer && texture && views > 1) {
    LOGE("glFramebufferTextureMultiviewOVR: more than %d multiview attachments", MAX_VIEW_ATTACHMENTS);
  }
  static int logged;
  if (slot >= 0 && logged < 4) {
    logged++;
    LOGI("glFramebufferTextureMultiviewOVR: framebuffer %u, %#x: texture %u, views %d-%d", framebuffer, attachment,
         texture, base, base + views - 1);
  }
  pthread_mutex_unlock(&g_lock);
  if (tracking()) attach(target, attachment, texture, "multiview");
}

// Drawn single sampled: GL_OVR_multiview_multisampled_render_to_texture isn't advertised, but an
// app may take the function regardless, as Guardian does the plain one.
static void GL_APIENTRY prism_FramebufferTextureMultisampleMultiviewOVR(GLenum target, GLenum attachment,
                                                                        GLuint texture, GLint level, GLsizei samples,
                                                                        GLint base, GLsizei views) {
  (void)samples;
  prism_FramebufferTextureMultiviewOVR(target, attachment, texture, level, base, views);
}

static void GL_APIENTRY prism_GetFramebufferAttachmentParameteriv(GLenum target, GLenum attachment, GLenum pname,
                                                                  GLint *params) {
  if (pname != GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR &&
      pname != GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_BASE_VIEW_INDEX_OVR) {
    next.GetFramebufferAttachmentParameteriv(target, attachment, pname, params);
    return;
  }
  EGLContext context = eglGetCurrentContext();
  pthread_mutex_lock(&g_lock);
  GLuint framebuffer = bound(state_of(context), target);
  params[0] = 0;
  for (int i = 0; framebuffer && i < MAX_VIEW_ATTACHMENTS; i++)
    if (g_views[i].context == context && g_views[i].framebuffer == framebuffer && g_views[i].attachment == attachment)
      params[0] = pname == GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_NUM_VIEWS_OVR ? g_views[i].views : g_views[i].base;
  pthread_mutex_unlock(&g_lock);
}

static void GL_APIENTRY prism_GetIntegerv(GLenum pname, GLint *data) {
  if (pname == GL_MAX_VIEWS_OVR) data[0] = MAX_VIEWS;
  else next.GetIntegerv(pname, data);
}

// The view uniform's location in each context's programs, looked up at a program's first draw into
// views.
#define MAX_PROGRAMS 256
static struct {
  EGLContext context;  // NULL: free
  GLuint program;
  GLint location;
} g_programs[MAX_PROGRAMS];
static unsigned g_program_next;

static GLint view_location(EGLContext context, GLuint program) {
  if (!program || !__atomic_load_n(&g_rewritten, __ATOMIC_RELAXED)) return -1;
  for (int i = 0; i < MAX_PROGRAMS; i++)
    if (g_programs[i].context == context && g_programs[i].program == program) return g_programs[i].location;
  GLint location = glGetUniformLocation(program, kViewUniform);
  int slot = -1;
  for (int i = 0; i < MAX_PROGRAMS && slot < 0; i++)
    if (!g_programs[i].context) slot = i;
  if (slot < 0) slot = g_program_next++ % MAX_PROGRAMS;
  g_programs[slot].context = context, g_programs[slot].program = program, g_programs[slot].location = location;
  return location;
}

// A relinked or deleted program's location goes, in every context: they may share it.
static void forget_program(GLuint program) {
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < MAX_PROGRAMS; i++)
    if (g_programs[i].context && g_programs[i].program == program) g_programs[i].context = NULL;
  pthread_mutex_unlock(&g_lock);
}

static void GL_APIENTRY prism_UseProgram(GLuint program) {
  next.UseProgram(program);
  pthread_mutex_lock(&g_lock);
  State *s = state_of(eglGetCurrentContext());
  if (s) s->program = program;
  pthread_mutex_unlock(&g_lock);
}

static void GL_APIENTRY prism_LinkProgram(GLuint program) {
  next.LinkProgram(program);
  if (__atomic_load_n(&g_rewritten, __ATOMIC_RELAXED)) forget_program(program);
}

static void GL_APIENTRY prism_DeleteProgram(GLuint program) {
  next.DeleteProgram(program);
  if (__atomic_load_n(&g_rewritten, __ATOMIC_RELAXED)) forget_program(program);
}

// What a draw into the bound framebuffer's views needs: the attachments with views, and the view
// uniform's location.
typedef struct {
  int views, count;
  struct {
    GLenum attachment;
    GLuint texture;
    GLint level, base, views;
  } at[8];
  GLint location;
} Views;

static int views_drawn(Views *v) {
  v->views = v->count = 0;
  if (!__atomic_load_n(&g_view_count, __ATOMIC_RELAXED)) return 0;
  EGLContext context = eglGetCurrentContext();
  pthread_mutex_lock(&g_lock);
  State *s = state_of(context);
  for (int i = 0; s && s->draw && i < MAX_VIEW_ATTACHMENTS; i++) {
    if (g_views[i].context != context || g_views[i].framebuffer != s->draw || v->count == 8) continue;
    v->at[v->count].attachment = g_views[i].attachment;
    v->at[v->count].texture = g_views[i].texture;
    v->at[v->count].level = g_views[i].level;
    v->at[v->count].base = g_views[i].base;
    v->at[v->count].views = g_views[i].views;
    if (g_views[i].views > v->views) v->views = g_views[i].views;
    v->count++;
  }
  v->location = v->views ? view_location(context, s->program) : -1;
  pthread_mutex_unlock(&g_lock);
  return v->views;
}

static void select_view(const Views *v, int view) {
  for (int k = 0; k < v->count; k++) {
    int layer = view < v->at[k].views ? view : v->at[k].views - 1;
    next.FramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, v->at[k].attachment, v->at[k].texture, v->at[k].level,
                                 v->at[k].base + layer);
  }
  if (v->location >= 0) glUniform1ui(v->location, (GLuint)view);
}

// The call once a view, the base view last, so its layer stays attached; or once.
#define EACH_VIEW(call)                               \
  do {                                                \
    Views v;                                          \
    if (!views_drawn(&v)) {                           \
      call;                                           \
      break;                                          \
    }                                                 \
    for (int view = v.views - 1; view >= 0; view--) { \
      select_view(&v, view);                          \
      call;                                           \
    }                                                 \
  } while (0)

static void GL_APIENTRY prism_Clear(GLbitfield mask) { EACH_VIEW(next.Clear(mask)); }
static void GL_APIENTRY prism_ClearBufferiv(GLenum buffer, GLint draw, const GLint *value) {
  EACH_VIEW(next.ClearBufferiv(buffer, draw, value));
}
static void GL_APIENTRY prism_ClearBufferuiv(GLenum buffer, GLint draw, const GLuint *value) {
  EACH_VIEW(next.ClearBufferuiv(buffer, draw, value));
}
static void GL_APIENTRY prism_ClearBufferfv(GLenum buffer, GLint draw, const GLfloat *value) {
  EACH_VIEW(next.ClearBufferfv(buffer, draw, value));
}
static void GL_APIENTRY prism_ClearBufferfi(GLenum buffer, GLint draw, GLfloat depth, GLint stencil) {
  EACH_VIEW(next.ClearBufferfi(buffer, draw, depth, stencil));
}
static void GL_APIENTRY prism_DrawArrays(GLenum mode, GLint first, GLsizei count) {
  EACH_VIEW(next.DrawArrays(mode, first, count));
}
static void GL_APIENTRY prism_DrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
  EACH_VIEW(next.DrawElements(mode, count, type, indices));
}
static void GL_APIENTRY prism_DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances) {
  EACH_VIEW(next.DrawArraysInstanced(mode, first, count, instances));
}
static void GL_APIENTRY prism_DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void *indices,
                                                    GLsizei instances) {
  EACH_VIEW(next.DrawElementsInstanced(mode, count, type, indices, instances));
}
static void GL_APIENTRY prism_DrawRangeElements(GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type,
                                                const void *indices) {
  EACH_VIEW(next.DrawRangeElements(mode, start, end, count, type, indices));
}
static void GL_APIENTRY prism_DrawArraysIndirect(GLenum mode, const void *indirect) {
  EACH_VIEW(next.DrawArraysIndirect(mode, indirect));
}
static void GL_APIENTRY prism_DrawElementsIndirect(GLenum mode, GLenum type, const void *indirect) {
  EACH_VIEW(next.DrawElementsIndirect(mode, type, indirect));
}

// --- shaders of multiview's ---------------------------------------------------------------------------

static int identifier(char c) { return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static int word_at(const char *source, const char *p, const char *word) {
  size_t n = strlen(word);
  return !strncmp(p, word, n) && !identifier(p[n]) && (p == source || !identifier(p[-1]));
}

static const char *skip_blanks(const char *p) {
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

// A multiview shader with gl_ViewID_OVR a uniform and no num_views layout or multiview extension
// directive; NULL if the shader has none of them (or isn't GLSL ES 3).
static char *rewrite(const char *source) {
  if (!strstr(source, "gl_ViewID_OVR") && !strstr(source, "num_views")) return NULL;
  const char *version = strstr(source, "#version");
  if (!version || atoi(version + 8) < 300) return NULL;

  // The words: gl_ViewID_OVR becomes the uniform, layout(num_views = N) in; goes.
  size_t length = strlen(source);
  char *words = malloc(length + 1);
  if (!words) return NULL;
  size_t w = 0;
  for (const char *p = source; *p;) {
    if (word_at(source, p, "gl_ViewID_OVR")) {
      memcpy(words + w, kViewUniform, sizeof kViewUniform - 1);
      w += sizeof kViewUniform - 1;
      p += 13;
      continue;
    }
    if (word_at(source, p, "layout")) {
      const char *q = skip_blanks(p + 6);
      const char *close = *q == '(' ? strchr(q, ')') : NULL;
      const char *views = close ? strstr(q, "num_views") : NULL;
      if (views && views < close) {
        q = skip_blanks(close + 1);
        if (word_at(source, q, "in")) {
          q = skip_blanks(q + 2);
          if (*q == ';') {
            p = q + 1;
            continue;
          }
        }
      }
    }
    words[w++] = *p++;
  }
  words[w] = 0;

  // The lines: multiview's extension directives go, and the uniform is declared after the last
  // directive, outside conditionals.
  static const char kDeclaration[] = "uniform highp uint prism_ViewID;\n";
  char *out = malloc(w + sizeof kDeclaration + 1);
  if (!out) {
    free(words);
    return NULL;
  }
  size_t o = 0, at = (size_t)-1;
  int depth = 0, pending = 0;
  for (const char *line = words; *line;) {
    const char *end = strchr(line, '\n');
    size_t n = end ? (size_t)(end - line) + 1 : strlen(line);
    const char *p = skip_blanks(line);
    int keep = 1;
    if (*p == '#') {
      p = skip_blanks(p + 1);
      if (word_at(words, p, "extension")) {
        pending = 1;
        const char *name = skip_blanks(p + 9);
        keep = strncmp(name, "GL_OVR_multiview", 16) != 0;
      } else if (word_at(words, p, "version")) {
        pending = 1;
      } else if (word_at(words, p, "if") || word_at(words, p, "ifdef") || word_at(words, p, "ifndef")) {
        depth++;
      } else if (word_at(words, p, "endif")) {
        depth--;
      }
    }
    if (keep) {
      memcpy(out + o, line, n);
      o += n;
    } else {
      out[o++] = '\n';  // line numbers stay
    }
    if (o && out[o - 1] != '\n') out[o++] = '\n';
    if (pending && depth <= 0) at = o, pending = 0;
    line += n;
  }
  free(words);
  if (at == (size_t)-1) at = 0;
  memmove(out + at + sizeof kDeclaration - 1, out + at, o - at);
  memcpy(out + at, kDeclaration, sizeof kDeclaration - 1);
  out[o + sizeof kDeclaration - 1] = 0;
  return out;
}

static void GL_APIENTRY prism_ShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings,
                                           const GLint *lengths) {
  size_t total = 0;
  for (GLsizei k = 0; strings && k < count; k++)
    total += lengths && lengths[k] >= 0 ? (size_t)lengths[k] : strings[k] ? strlen(strings[k]) : 0;
  char *joined = strings ? malloc(total + 1) : NULL;
  char *rewritten = NULL;
  if (joined) {
    size_t j = 0;
    for (GLsizei k = 0; k < count; k++) {
      size_t n = lengths && lengths[k] >= 0 ? (size_t)lengths[k] : strings[k] ? strlen(strings[k]) : 0;
      memcpy(joined + j, strings[k], n);
      j += n;
    }
    joined[j] = 0;
    rewritten = rewrite(joined);
    free(joined);
  }
  if (!rewritten) {
    next.ShaderSource(shader, count, strings, lengths);
    return;
  }
  if (__atomic_add_fetch(&g_rewritten, 1, __ATOMIC_RELAXED) <= 2)
    LOGI("shader %u: multiview, gl_ViewID_OVR made a uniform:\n%.1500s", shader, rewritten);
  const GLchar *source = rewritten;
  next.ShaderSource(shader, 1, &source, NULL);
  free(rewritten);
}

static EGLBoolean EGLAPIENTRY prism_DestroyContext(EGLDisplay display, EGLContext context) {
  pthread_mutex_lock(&g_lock);
  for (int i = 0; i < MAX_STATES; i++)
    if (g_states[i].context == context) g_states[i].context = NULL;
  for (int i = 0; i < MAX_VIEW_ATTACHMENTS; i++)
    if (g_views[i].context == context) forget_view(i);
  for (int i = 0; i < MAX_PROGRAMS; i++)
    if (g_programs[i].context == context) g_programs[i].context = NULL;
  pthread_mutex_unlock(&g_lock);
  return next.DestroyContext(display, context);
}

// --- the copy -----------------------------------------------------------------------------------------

#define MAX_CONTEXTS 16
typedef struct {
  EGLContext app;  // NULL: free
  EGLContext copy;
  EGLSurface surface;
  GLuint programs[2];  // 2D, array
  GLint heights[2], encodes[2];  // uniform locations
  GLuint vertex_array;
} Copier;
static Copier g_copiers[MAX_CONTEXTS];

static const char kVertex[] =
    "#version 300 es\n"
    "void main() {\n"
    "  gl_Position = vec4(float((gl_VertexID & 1) * 4 - 1), float((gl_VertexID & 2) * 2 - 1), 0.0, 1.0);\n"
    "}\n";
// A texel of the source for each pixel of the buffer: layer y / height, row y % height.
static const char kFragment[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "uniform highp SAMPLER source;\n"
    "uniform int height;\n"
    "uniform bool encode;\n"
    "out vec4 color;\n"
    "vec3 srgb(vec3 c) {\n"
    "  c = clamp(c, 0.0, 1.0);\n"
    "  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));\n"
    "}\n"
    "void main() {\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "  vec4 c = texelFetch(source, COORDINATES, 0);\n"
    "  color = encode ? vec4(srgb(c.rgb), c.a) : c;\n"
    "}\n";

static GLuint shader(GLenum type, const char *const *parts, int count) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, count, parts, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512] = "";
    glGetShaderInfoLog(s, sizeof log, NULL, log);
    LOGE("copy shader: %s", log);
  }
  return s;
}

static int make_programs(Copier *c) {
  static const char *const kinds[2][2] = {{"#define SAMPLER sampler2D\n#define COORDINATES p\n"},
                                          {"#define SAMPLER sampler2DArray\n#define COORDINATES ivec3(p.x, p.y % height, p.y / height)\n"}};
  const char *vertex[] = {kVertex};
  GLuint v = shader(GL_VERTEX_SHADER, vertex, 1);
  for (int k = 0; k < 2; k++) {
    // The defines go after the #version line.
    const char *body = strchr(kFragment, '\n') + 1;
    const char *fragment[] = {"#version 300 es\n", kinds[k][0], body};
    GLuint f = shader(GL_FRAGMENT_SHADER, fragment, 3);
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
      char log[512] = "";
      glGetProgramInfoLog(p, sizeof log, NULL, log);
      LOGE("copy program: %s", log);
      glDeleteShader(v);
      return 0;
    }
    c->programs[k] = p;
    c->heights[k] = glGetUniformLocation(p, "height");
    c->encodes[k] = glGetUniformLocation(p, "encode");
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "source"), 0);
  }
  glDeleteShader(v);
  glGenVertexArrays(1, &c->vertex_array);
  return 1;
}

// The copy context for the app's: a context sharing its textures, on a small pbuffer (the
// emulator's EGL has no surfaceless contexts).
static Copier *copier(EGLDisplay display, EGLContext app) {
  for (int i = 0; i < MAX_CONTEXTS; i++)
    if (g_copiers[i].app == app) return g_copiers[i].copy ? &g_copiers[i] : NULL;
  Copier *c = NULL;
  for (int i = 0; i < MAX_CONTEXTS && !c; i++)
    if (!g_copiers[i].app) c = &g_copiers[i];
  if (!c) return NULL;
  *c = (Copier){app};
  const EGLint attributes[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                               EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
  EGLConfig config;
  EGLint count = 0;
  if (!eglChooseConfig(display, attributes, &config, 1, &count) || !count) {
    LOGE("no EGL config for the copy context (%#x)", eglGetError());
    return NULL;
  }
  const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  const EGLint surface_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  c->copy = eglCreateContext(display, config, app, context_attributes);
  c->surface = eglCreatePbufferSurface(display, config, surface_attributes);
  if (c->copy == EGL_NO_CONTEXT || c->surface == EGL_NO_SURFACE) {
    LOGE("no copy context (%#x)", eglGetError());
    c->copy = NULL;
    return NULL;
  }
  return c;
}

static void copy_textures(void) {
  if (!tracking()) return;
  EGLContext app = eglGetCurrentContext();
  if (app == EGL_NO_CONTEXT) return;
  pthread_mutex_lock(&g_lock);
  int dirty = 0;
  for (int i = 0; i < MAX_IMPORTS; i++) dirty |= g_imports[i].context == app && g_imports[i].dirty;
  if (!dirty) {
    pthread_mutex_unlock(&g_lock);
    return;
  }
  EGLDisplay display = eglGetCurrentDisplay();
  EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
  Copier *c = copier(display, app);
  if (!c) {
    pthread_mutex_unlock(&g_lock);
    return;
  }
  GLsync written = next.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  glFlush();
  if (!eglMakeCurrent(display, c->surface, c->surface, c->copy)) {
    LOGE("copy context: eglMakeCurrent failed (%#x)", eglGetError());
    glDeleteSync(written);
    pthread_mutex_unlock(&g_lock);
    return;
  }
  glWaitSync(written, 0, GL_TIMEOUT_IGNORED);
  glDeleteSync(written);
  if (!c->programs[0] && !make_programs(c)) c->programs[0] = c->programs[1] = 0;
  for (int i = 0; i < MAX_RETIRED; i++)
    if (g_retired[i].context == app) {
      next.DeleteTextures(1, &g_retired[i].shadow);
      next.DeleteFramebuffers(1, &g_retired[i].framebuffer);
      g_retired[i].context = NULL;
    }
  if (c->programs[0]) glBindVertexArray(c->vertex_array);
  for (int i = 0; c->programs[0] && i < MAX_IMPORTS; i++) {
    Import *t = &g_imports[i];
    if (t->context != app || !t->dirty) continue;
    if (!t->shadow) {
      glGenTextures(1, &t->shadow);
      glBindTexture(GL_TEXTURE_2D, t->shadow);
      image_target_texture(GL_TEXTURE_2D, (GLeglImageOES)t->image);
      glGenFramebuffers(1, &t->framebuffer);
      next.BindFramebuffer(GL_FRAMEBUFFER, t->framebuffer);
      next.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->shadow, 0);
      GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      if (status != GL_FRAMEBUFFER_COMPLETE) LOGE("texture %u: its buffer's framebuffer is incomplete (%#x)", t->texture, status);
    }
    int array = t->target == GL_TEXTURE_2D_ARRAY;
    next.BindFramebuffer(GL_FRAMEBUFFER, t->framebuffer);
    glViewport(0, 0, t->width, t->height * t->layers);
    next.UseProgram(c->programs[array]);
    glUniform1i(c->heights[array], t->height);
    glUniform1i(c->encodes[array], t->encode);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(t->target, t->texture);
    next.DrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(t->target, 0);
    if (!t->logged) {
      t->logged = 1;
      LOGI("texture %u: copied into its buffer (%d x %d, %d layers%s); GL error %#x", t->texture, t->width, t->height,
           t->layers, t->encode ? ", sRGB" : "", glGetError());
    }
    t->dirty = 0;
  }
  next.BindFramebuffer(GL_FRAMEBUFFER, 0);
  GLsync copied = next.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  glFlush();
  if (!eglMakeCurrent(display, draw, read, app)) LOGE("app context: eglMakeCurrent failed (%#x)", eglGetError());
  glWaitSync(copied, 0, GL_TIMEOUT_IGNORED);
  glDeleteSync(copied);
  // A texture still attached to the bound framebuffer may be drawn into further.
  GLint bound = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
  if (bound) mark_framebuffer(app, (GLuint)bound);
  pthread_mutex_unlock(&g_lock);
}

static GLsync GL_APIENTRY prism_FenceSync(GLenum condition, GLbitfield flags) {
  copy_textures();
  return next.FenceSync(condition, flags);
}

static void GL_APIENTRY prism_Finish(void) {
  copy_textures();
  next.Finish();
}

static EGLSyncKHR EGLAPIENTRY prism_CreateSyncKHR(EGLDisplay display, EGLenum type, const EGLint *attributes) {
  copy_textures();
  return next.CreateSyncKHR(display, type, attributes);
}

// --- the extension string and entry points -------------------------------------------------------------

static const char kExtensions[] = "GL_EXT_memory_object GL_EXT_memory_object_fd";
// Advertised by the emulator, not implemented by it or the layer. (GL_OVR_multiview and
// GL_OVR_multiview2, which the emulator advertises too, the layer implements.)
static const char *const kHidden[] = {"GL_EXT_multiview_texture_multisample",
                                      "GL_OVR_multiview_multisampled_render_to_texture"};

static int listed(const char *name, const char *const *names, size_t count) {
  for (size_t i = 0; i < count; i++)
    if (!strcmp(name, names[i])) return 1;
  return 0;
}

// The driver's extensions, less the hidden ones, and the layer's.
static char *extensions(const char *driver) {
  char *out = malloc(strlen(driver) + sizeof kExtensions + 2);
  if (!out) return NULL;
  size_t length = 0;
  for (const char *p = driver; *p;) {
    while (*p == ' ') p++;
    size_t n = strcspn(p, " ");
    if (!n) break;
    char token[128];
    if (n < sizeof token) {
      memcpy(token, p, n);
      token[n] = 0;
    }
    if (n >= sizeof token || !listed(token, kHidden, sizeof kHidden / sizeof *kHidden)) {
      memcpy(out + length, p, n);
      length += n;
      out[length++] = ' ';
    }
    p += n;
  }
  memcpy(out + length, kExtensions, sizeof kExtensions);
  return out;
}

static const GLubyte *GL_APIENTRY prism_GetString(GLenum name) {
  const GLubyte *value = next.GetString(name);
  if (name != GL_EXTENSIONS || !value) return value;
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  static const GLubyte *last_value;
  static char *last;
  pthread_mutex_lock(&lock);
  if (value != last_value) {  // the driver's string is per context, but the same for each
    char *joined = extensions((const char *)value);
    if (joined) {
      last = joined;  // earlier strings stay: contexts may still hold them
      last_value = value;
    }
  }
  const GLubyte *result = last ? (const GLubyte *)last : value;
  pthread_mutex_unlock(&lock);
  return result;
}

static const struct {
  const char *name;
  Function function;
} kOwn[] = {
    {"glCreateMemoryObjectsEXT", (Function)prism_CreateMemoryObjectsEXT},
    {"glDeleteMemoryObjectsEXT", (Function)prism_DeleteMemoryObjectsEXT},
    {"glIsMemoryObjectEXT", (Function)prism_IsMemoryObjectEXT},
    {"glMemoryObjectParameterivEXT", (Function)prism_MemoryObjectParameterivEXT},
    {"glGetMemoryObjectParameterivEXT", (Function)prism_GetMemoryObjectParameterivEXT},
    {"glImportMemoryFdEXT", (Function)prism_ImportMemoryFdEXT},
    {"glTexStorageMem2DEXT", (Function)prism_TexStorageMem2DEXT},
    {"glTexStorageMem3DEXT", (Function)prism_TexStorageMem3DEXT},
    {"glBufferStorageMemEXT", (Function)prism_BufferStorageMemEXT},
    {"glFramebufferTextureMultiviewOVR", (Function)prism_FramebufferTextureMultiviewOVR},
    {"glFramebufferTextureMultisampleMultiviewOVR", (Function)prism_FramebufferTextureMultisampleMultiviewOVR},
};

static Function own(const char *name) {
  for (size_t i = 0; i < sizeof kOwn / sizeof *kOwn; i++)
    if (!strcmp(name, kOwn[i].name)) return kOwn[i].function;
  return NULL;
}

static Function EGLAPIENTRY prism_GetProcAddress(const char *name) {
  Function f = name ? own(name) : NULL;
  return f ? f : next.GetProcAddress(name);
}

#define WRAP(field, function)          \
  if (!strcmp(name, #function)) {      \
    next.field = (void *)next_function; \
    return (void *)prism_##field;      \
  }

EXPORT void *AndroidGLESLayer_GetProcAddress(const char *name, void *next_function) {
  Function f = own(name);
  if (f) return (void *)f;
  if (!next_function) return next_function;
  WRAP(GetString, glGetString)
  WRAP(GetIntegerv, glGetIntegerv)
  WRAP(BindFramebuffer, glBindFramebuffer)
  WRAP(FramebufferTexture2D, glFramebufferTexture2D)
  WRAP(FramebufferTextureLayer, glFramebufferTextureLayer)
  WRAP(FramebufferRenderbuffer, glFramebufferRenderbuffer)
  WRAP(GetFramebufferAttachmentParameteriv, glGetFramebufferAttachmentParameteriv)
  WRAP(DeleteTextures, glDeleteTextures)
  WRAP(DeleteFramebuffers, glDeleteFramebuffers)
  WRAP(CheckFramebufferStatus, glCheckFramebufferStatus)
  WRAP(ShaderSource, glShaderSource)
  WRAP(LinkProgram, glLinkProgram)
  WRAP(UseProgram, glUseProgram)
  WRAP(DeleteProgram, glDeleteProgram)
  WRAP(Clear, glClear)
  WRAP(ClearBufferiv, glClearBufferiv)
  WRAP(ClearBufferuiv, glClearBufferuiv)
  WRAP(ClearBufferfv, glClearBufferfv)
  WRAP(ClearBufferfi, glClearBufferfi)
  WRAP(DrawArrays, glDrawArrays)
  WRAP(DrawElements, glDrawElements)
  WRAP(DrawArraysInstanced, glDrawArraysInstanced)
  WRAP(DrawElementsInstanced, glDrawElementsInstanced)
  WRAP(DrawRangeElements, glDrawRangeElements)
  WRAP(DrawArraysIndirect, glDrawArraysIndirect)
  WRAP(DrawElementsIndirect, glDrawElementsIndirect)
  WRAP(FenceSync, glFenceSync)
  WRAP(Finish, glFinish)
  WRAP(CreateSyncKHR, eglCreateSyncKHR)
  WRAP(DestroyContext, eglDestroyContext)
  WRAP(GetProcAddress, eglGetProcAddress)
  return next_function;
}

EXPORT void AndroidGLESLayer_Initialize(void *layer, NextProc next_proc) {
  (void)layer, (void)next_proc;
  LOGI("loaded");
}
