#include "vr/gles_transport.h"
#include "mocktail/vr/openxr_backend.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <cstdio>
#include <cstring>

extern "C" void *mocktail_vr_initializing_object();
extern "C" bool mocktail_vr_current_eye(void **, int *, void **);
extern "C" void mocktail_vr_consume_eye();

namespace mocktail::vr {
namespace {
// The window owns a single EGL context. Function pointers are loaded from
// that exact SDL EGL/GLES implementation, never from libGL or the guest shim.
#define GL_PROCS(X)                                                            \
  X(GetIntegerv, PFNGLGETINTEGERVPROC)                                         \
  X(IsEnabled, PFNGLISENABLEDPROC)                                             \
  X(Enable, PFNGLENABLEPROC)                                                   \
  X(Disable, PFNGLDISABLEPROC)                                                 \
  X(GenFramebuffers, PFNGLGENFRAMEBUFFERSPROC) X(DeleteFramebuffers,           \
                                                 PFNGLDELETEFRAMEBUFFERSPROC)  \
      X(BindFramebuffer, PFNGLBINDFRAMEBUFFERPROC)                             \
          X(CheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC) X(        \
              GetFramebufferAttachmentParameteriv,                             \
              PFNGLGETFRAMEBUFFERATTACHMENTPARAMETERIVPROC)                    \
              X(FramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC) X(        \
                  FramebufferTextureLayer, PFNGLFRAMEBUFFERTEXTURELAYERPROC)   \
                  X(FramebufferRenderbuffer, PFNGLFRAMEBUFFERRENDERBUFFERPROC) \
                      X(TexImage2D, PFNGLTEXIMAGE2DPROC) X(                    \
                          TexStorage2D, PFNGLTEXSTORAGE2DPROC)                 \
                          X(RenderbufferStorage, PFNGLRENDERBUFFERSTORAGEPROC) \
                              X(RenderbufferStorageMultisample,                \
                                PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC)       \
                                  X(DeleteTextures, PFNGLDELETETEXTURESPROC)   \
                                      X(DeleteRenderbuffers,                   \
                                        PFNGLDELETERENDERBUFFERSPROC)          \
                                          X(BlitFramebuffer,                   \
                                            PFNGLBLITFRAMEBUFFERPROC)          \
                                              X(Flush, PFNGLFLUSHPROC)
struct Procs {
#define DECLARE(name, type) type name = nullptr;
  GL_PROCS(DECLARE)
#undef DECLARE
} gl;
thread_local GlesTransport *current = nullptr;
GlesTransport *Current() {
  return current && current->IsCurrent() ? current : nullptr;
}
void GL_APIENTRY GenFramebuffers(GLsizei n, GLuint *ids) {
  gl.GenFramebuffers(n, ids);
  if (auto *t = Current())
    t->Generated(n, ids);
}
void GL_APIENTRY DeleteFramebuffers(GLsizei n, const GLuint *ids) {
  if (auto *t = Current())
    t->Deleted(n, ids);
  gl.DeleteFramebuffers(n, ids);
}
void GL_APIENTRY BindFramebuffer(GLenum target, GLuint id) {
  gl.BindFramebuffer(target, id);
  if (auto *t = Current())
    t->Bind(target, id);
}
void GL_APIENTRY FramebufferTexture2D(GLenum target, GLenum attachment,
                                      GLenum tex_target, GLuint texture,
                                      GLint level) {
  gl.FramebufferTexture2D(target, attachment, tex_target, texture, level);
  if (auto *t = Current())
    t->AttachmentChanged(target);
}
void GL_APIENTRY FramebufferTextureLayer(GLenum target, GLenum attachment,
                                         GLuint texture, GLint level,
                                         GLint layer) {
  gl.FramebufferTextureLayer(target, attachment, texture, level, layer);
  if (auto *t = Current())
    t->AttachmentChanged(target);
}
void GL_APIENTRY FramebufferRenderbuffer(GLenum target, GLenum attachment,
                                         GLenum rb_target, GLuint rb) {
  gl.FramebufferRenderbuffer(target, attachment, rb_target, rb);
  if (auto *t = Current())
    t->AttachmentChanged(target);
}
void GL_APIENTRY TexImage2D(GLenum target, GLint level, GLint internal_format,
                            GLsizei w, GLsizei h, GLint border, GLenum format,
                            GLenum type, const void *data) {
  gl.TexImage2D(target, level, internal_format, w, h, border, format, type,
                data);
  if (target == GL_TEXTURE_2D && level == 0)
    if (auto *t = Current())
      t->Storage(true, w, h, internal_format);
}
void GL_APIENTRY TexStorage2D(GLenum target, GLsizei levels, GLenum format,
                              GLsizei w, GLsizei h) {
  gl.TexStorage2D(target, levels, format, w, h);
  if (target == GL_TEXTURE_2D)
    if (auto *t = Current())
      t->Storage(true, w, h, format);
}
void GL_APIENTRY RenderbufferStorage(GLenum target, GLenum format, GLsizei w,
                                     GLsizei h) {
  gl.RenderbufferStorage(target, format, w, h);
  if (target == GL_RENDERBUFFER)
    if (auto *t = Current())
      t->Storage(false, w, h, format);
}
void GL_APIENTRY RenderbufferStorageMultisample(GLenum target, GLsizei samples,
                                                GLenum format, GLsizei w,
                                                GLsizei h) {
  gl.RenderbufferStorageMultisample(target, samples, format, w, h);
  if (target == GL_RENDERBUFFER)
    if (auto *t = Current())
      t->Storage(false, w, h, format, samples);
}
void GL_APIENTRY DeleteTextures(GLsizei n, const GLuint *ids) {
  if (auto *t = Current())
    t->ObjectsDeleted(true, n, ids);
  gl.DeleteTextures(n, ids);
}
void GL_APIENTRY DeleteRenderbuffers(GLsizei n, const GLuint *ids) {
  if (auto *t = Current())
    t->ObjectsDeleted(false, n, ids);
  gl.DeleteRenderbuffers(n, ids);
}
} // namespace

bool GlesTransport::Initialize(void *display, void *config, void *context,
                               void *egl_get_proc,
                               void *(*resolve)(const char *),
                               std::string *error) {
  if (!display || !config || !context || !egl_get_proc || !resolve) {
    *error =
        "OpenGL VR requires a real current EGL context, config and display";
    return false;
  }
#define LOAD(name, type)                                                       \
  gl.name = reinterpret_cast<type>(resolve("gl" #name));                       \
  if (!gl.name) {                                                              \
    *error = "OpenGL VR is missing gl" #name;                                  \
    return false;                                                              \
  }
  GL_PROCS(LOAD)
#undef LOAD
  using GetProc = __eglMustCastToProperFunctionPointerType (*)(const char *);
  get_current_context_ = reinterpret_cast<void *(*)()>(
      reinterpret_cast<GetProc>(egl_get_proc)("eglGetCurrentContext"));
  if (!get_current_context_ || get_current_context_() != context) {
    *error = "OpenGL VR must initialize on the current EGL render context";
    return false;
  }
  GLint major = 0, minor = 0;
  gl.GetIntegerv(GL_MAJOR_VERSION, &major);
  gl.GetIntegerv(GL_MINOR_VERSION, &minor);
  if (major < 3) {
    *error = "OpenGL VR requires OpenGL ES 3.0 or newer";
    return false;
  }
  version_ = (std::uint64_t(major) << 48) | (std::uint64_t(minor) << 32);
  display_ = display;
  config_ = config;
  context_ = context;
  egl_get_proc_ = egl_get_proc;
  gl.GenFramebuffers(2, copy_fbos_);
  if (!copy_fbos_[0] || !copy_fbos_[1]) {
    *error = "Cannot allocate OpenGL VR copy framebuffers";
    return false;
  }
  MakeCurrent();
  return true;
}
bool GlesTransport::IsCurrent() const {
  return get_current_context_ && get_current_context_() == context_;
}
void GlesTransport::MakeCurrent() { current = this; }
void GlesTransport::ReleaseCurrent() { current = nullptr; }
void GlesTransport::Destroy() {
  if (IsCurrent())
    gl.DeleteFramebuffers(2, copy_fbos_);
  copy_fbos_[0] = copy_fbos_[1] = 0;
  if (current == this)
    current = nullptr;
  owners_.clear();
  textures_.clear();
  renderbuffers_.clear();
  ResetEyes();
  context_ = nullptr;
}
void *GlesTransport::Wrap(const char *name, void *raw) {
  if (!raw || !name)
    return raw;
#define WRAP(name_)                                                            \
  if (std::strcmp(name, "gl" #name_) == 0)                                     \
    return reinterpret_cast<void *>(&name_);
  WRAP(GenFramebuffers)
  WRAP(DeleteFramebuffers)
  WRAP(BindFramebuffer) WRAP(FramebufferTexture2D) WRAP(FramebufferRenderbuffer)
      WRAP(FramebufferTextureLayer) WRAP(TexImage2D) WRAP(TexStorage2D)
          WRAP(RenderbufferStorage) WRAP(RenderbufferStorageMultisample)
              WRAP(DeleteTextures) WRAP(DeleteRenderbuffers)
#undef WRAP
                  return raw;
}
void GlesTransport::ResetEyes() {
  eyes_[0] = {};
  eyes_[1] = {};
}
void GlesTransport::Generated(int count, const unsigned *ids) {
  void *owner = mocktail_vr_initializing_object();
  if (!owner || !ids)
    return;
  for (int i = 0; i < count; ++i)
    if (ids[i])
      owners_[ids[i]] = owner;
}
void GlesTransport::Deleted(int count, const unsigned *ids) {
  if (!ids)
    return;
  for (int i = 0; i < count; ++i) {
    owners_.erase(ids[i]);
    for (auto &eye : eyes_)
      if (eye.framebuffer == ids[i])
        eye = {};
  }
}
void GlesTransport::ObjectsDeleted(bool texture, int count,
                                   const unsigned *ids) {
  if (!ids)
    return;
  for (int i = 0; i < count; ++i) {
    (texture ? textures_ : renderbuffers_).erase(ids[i]);
    for (auto &eye : eyes_)
      if (eye.texture == texture && eye.object == ids[i])
        eye = {};
  }
}
void GlesTransport::Storage(bool texture, int w, int h, unsigned format,
                            int samples) {
  GLint id = 0;
  gl.GetIntegerv(texture ? GL_TEXTURE_BINDING_2D : GL_RENDERBUFFER_BINDING,
                 &id);
  if (!id)
    return;
  // Storage redefinition invalidates previously rendered eye content.
  const unsigned object = static_cast<unsigned>(id);
  ObjectsDeleted(texture, 1, &object);
  (texture ? textures_ : renderbuffers_)[object] = {w, h, format, samples};
}
void GlesTransport::AttachmentChanged(unsigned target) {
  GLint id = 0;
  gl.GetIntegerv(target == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER_BINDING
                                               : GL_DRAW_FRAMEBUFFER_BINDING,
                 &id);
  for (auto &eye : eyes_)
    if (eye.framebuffer == static_cast<unsigned>(id))
      eye = {};
}
void GlesTransport::Bind(unsigned target, unsigned framebuffer,
                         std::uint64_t frame) {
  if (!frame)
    return;
  if (target != GL_FRAMEBUFFER && target != GL_DRAW_FRAMEBUFFER)
    return;
  auto found = owners_.find(framebuffer);
  if (found == owners_.end())
    return;
  void *owner = nullptr;
  void *guest_fb = nullptr;
  int eye = -1;
  if (!mocktail_vr_current_eye(&owner, &eye, &guest_fb) || eye < 0 || eye > 1 ||
      found->second != owner)
    return;
  if (frame == UINT64_MAX) {
    auto *backend = ActiveVrBackend();
    const auto pose =
        backend ? backend->PublishedHeadPose() : ScriptedPoseSample{};
    if (!pose.valid)
      return;
    frame = pose.frame;
  }
  GLint bound = 0;
  gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
  if (static_cast<unsigned>(bound) != framebuffer)
    return;
  if (gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    return;
  GLint type = 0, object = 0, level = 0, face = 0;
  gl.GetFramebufferAttachmentParameteriv(
      GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
      GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
  if (type != GL_TEXTURE && type != GL_RENDERBUFFER)
    return;
  gl.GetFramebufferAttachmentParameteriv(
      GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
      GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object);
  if (type == GL_TEXTURE) {
    gl.GetFramebufferAttachmentParameteriv(
        GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, &level);
    gl.GetFramebufferAttachmentParameteriv(
        GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE, &face);
    if (level != 0 || face != 0)
      return;
  }
  const auto &storage = type == GL_TEXTURE ? textures_ : renderbuffers_;
  auto it = storage.find(object);
  if (it == storage.end())
    return;
  const auto &size = it->second;
  // Reject unsupported formats/extents instead of copying unrelated pixels.
  if (size.samples != 0 ||
      (size.format != GL_RGBA8 && size.format != GL_RGBA &&
       size.format != GL_SRGB8_ALPHA8))
    return;

  if ((eyes_[0].owner && eyes_[0].owner != owner) ||
      (eyes_[1].owner && eyes_[1].owner != owner))
    ResetEyes();
  const bool changed = eyes_[eye].framebuffer != framebuffer;
  eyes_[eye] = {framebuffer,
                static_cast<unsigned>(object),
                type == GL_TEXTURE,
                size.width,
                size.height,
                owner,
                frame};
  mocktail_vr_consume_eye();
  if (changed)
    std::fprintf(
        stderr,
        "  [vr-backend] GLES eye-bound eye=%d owner=%p guest_fb=%p gl_fb=%u "
        "color=%u extent=%dx%d provenance=guest_initializer_and_eye_getter\n",
        eye, owner, guest_fb, framebuffer, object, size.width, size.height);
}
bool GlesTransport::Copy(const unsigned textures[2], unsigned width,
                         unsigned height) {
  if (!IsCurrent() || !textures[0] || !textures[1] || !eyes_[0].object ||
      !eyes_[1].object ||
      (eyes_[0].texture == eyes_[1].texture &&
       eyes_[0].object == eyes_[1].object))
    return false;
  GLint read_fb = 0, draw_fb = 0;
  gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
  gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fb);
  const bool scissor = gl.IsEnabled(GL_SCISSOR_TEST);
  gl.Disable(GL_SCISSOR_TEST);
  bool copied = true;
  for (int i = 0; i < 2; ++i) {
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, copy_fbos_[0]);
    if (eyes_[i].texture)
      gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_TEXTURE_2D, eyes_[i].object, 0);
    else
      gl.FramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                 GL_RENDERBUFFER, eyes_[i].object);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, copy_fbos_[1]);
    gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_TEXTURE_2D, textures[i], 0);
    const auto read_status = gl.CheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    const auto draw_status = gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    if (read_status != GL_FRAMEBUFFER_COMPLETE ||
        draw_status != GL_FRAMEBUFFER_COMPLETE) {
      static bool reported = false;
      if (!reported)
        std::fprintf(stderr,
                     "  [vr-backend] GLES copy framebuffer incomplete: "
                     "read=0x%x draw=0x%x XR texture=%u\n",
                     read_status, draw_status, textures[i]);
      reported = true;
      copied = false;
      break;
    }
    gl.BlitFramebuffer(0, 0, eyes_[i].width, eyes_[i].height, 0, 0, width,
                       height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  }
  // Release references to runtime textures before releasing XR swapchain
  // images.
  gl.BindFramebuffer(GL_READ_FRAMEBUFFER, copy_fbos_[0]);
  gl.FramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_RENDERBUFFER, 0);
  gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, copy_fbos_[1]);
  gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                          GL_TEXTURE_2D, 0, 0);
  gl.BindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
  gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fb);
  if (scissor)
    gl.Enable(GL_SCISSOR_TEST);
  // OpenXR's GL binding synchronizes the flushed work at image release.
  gl.Flush();
  return copied;
}
} // namespace mocktail::vr
