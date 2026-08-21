// Fake object/status semantics are available only when
// MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS=1. Production must resolve every GLES
// symbol from a real ANGLE or system implementation.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using GLbitfield = uint32_t;
using GLboolean = uint8_t;
using GLchar = char;
using GLenum = uint32_t;
using GLfloat = float;
using GLint = int32_t;
using GLsizei = int32_t;
using GLuint = uint32_t;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

namespace {

constexpr GLenum kGlNoError = 0;
constexpr GLenum kGlInvalidOperation = 0x0502;
constexpr GLenum kGlVendor = 0x1f00;
constexpr GLenum kGlRenderer = 0x1f01;
constexpr GLenum kGlVersion = 0x1f02;
constexpr GLenum kGlExtensions = 0x1f03;
constexpr GLenum kGlShadingLanguageVersion = 0x8b8c;
constexpr GLenum kGlFramebufferComplete = 0x8cd5;
constexpr GLenum kGlFramebufferUnsupported = 0x8cdd;
constexpr GLenum kGlCompileStatus = 0x8b81;
constexpr GLenum kGlLinkStatus = 0x8b82;
constexpr GLenum kGlInfoLogLength = 0x8b84;
constexpr GLenum kGlMaxTextureSize = 0x0d33;
constexpr GLenum kGlMaxTextureImageUnits = 0x8872;
constexpr GLenum kGlMaxVertexAttribs = 0x8869;
constexpr GLenum kGlMaxVaryingVectors = 0x8dfc;
constexpr GLenum kGlMaxVertexUniformVectors = 0x8dfb;
constexpr GLenum kGlMaxFragmentUniformVectors = 0x8dfd;
constexpr GLenum kGlViewport = 0x0ba2;

GLuint g_next_object = 1;
thread_local GLenum g_last_error = kGlNoError;
thread_local GLint g_viewport[4] = {0, 0, 1280, 720};

bool TestGraphicsStubsEnabled() {
  const char* value = std::getenv("MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS");
  return value != nullptr && std::strcmp(value, "1") == 0;
}

bool RequireTestGraphicsStub() {
  if (TestGraphicsStubsEnabled()) {
    return true;
  }
  g_last_error = kGlInvalidOperation;
  return false;
}

GLuint NextObject() {
  if (!RequireTestGraphicsStub()) {
    return 0;
  }
  return g_next_object++;
}

void FillObjects(GLsizei n, GLuint* objects) {
  if (objects == nullptr || n <= 0) {
    return;
  }
  if (!RequireTestGraphicsStub()) {
    std::memset(objects, 0, static_cast<size_t>(n) * sizeof(*objects));
    return;
  }
  for (GLsizei i = 0; i < n; ++i) {
    objects[i] = NextObject();
  }
}

void CopyString(const char* source, GLsizei buf_size, GLsizei* length,
                GLchar* info_log) {
  const GLsizei source_length =
      source == nullptr ? 0 : static_cast<GLsizei>(std::strlen(source));
  if (length != nullptr) {
    *length = source_length;
  }
  if (info_log == nullptr || buf_size <= 0) {
    return;
  }
  const GLsizei copy_length =
      source_length < buf_size - 1 ? source_length : buf_size - 1;
  if (copy_length > 0) {
    std::memcpy(info_log, source, static_cast<size_t>(copy_length));
  }
  info_log[copy_length] = '\0';
}

}  // namespace

extern "C" {

const uint8_t* glGetString(GLenum name) {
  if (!RequireTestGraphicsStub()) {
    return nullptr;
  }
  switch (name) {
    case kGlVendor:
      return reinterpret_cast<const uint8_t*>("Mocktail");
    case kGlRenderer:
      return reinterpret_cast<const uint8_t*>("Mocktail GLES shim");
    case kGlVersion:
      return reinterpret_cast<const uint8_t*>("OpenGL ES 3.0 Mocktail");
    case kGlShadingLanguageVersion:
      return reinterpret_cast<const uint8_t*>("OpenGL ES GLSL ES 3.00");
    case kGlExtensions:
      return reinterpret_cast<const uint8_t*>(
          "GL_OES_element_index_uint GL_OES_rgb8_rgba8 "
          "GL_EXT_texture_filter_anisotropic");
    default:
      return reinterpret_cast<const uint8_t*>("");
  }
}

GLenum glGetError() {
  GLenum error = g_last_error;
  g_last_error = kGlNoError;
  return error;
}

void glGetIntegerv(GLenum pname, GLint* data) {
  if (data == nullptr) {
    return;
  }
  if (!RequireTestGraphicsStub()) {
    *data = 0;
    return;
  }
  switch (pname) {
    case kGlMaxTextureSize:
      *data = 4096;
      break;
    case kGlMaxTextureImageUnits:
      *data = 16;
      break;
    case kGlMaxVertexAttribs:
      *data = 16;
      break;
    case kGlMaxVaryingVectors:
    case kGlMaxVertexUniformVectors:
    case kGlMaxFragmentUniformVectors:
      *data = 128;
      break;
    case kGlViewport:
      std::memcpy(data, g_viewport, sizeof(g_viewport));
      break;
    default:
      *data = 1;
      break;
  }
}

void glGenTextures(GLsizei n, GLuint* textures) { FillObjects(n, textures); }
void glGenBuffers(GLsizei n, GLuint* buffers) { FillObjects(n, buffers); }
void glGenFramebuffers(GLsizei n, GLuint* framebuffers) {
  FillObjects(n, framebuffers);
}
void glGenRenderbuffers(GLsizei n, GLuint* renderbuffers) {
  FillObjects(n, renderbuffers);
}

GLuint glCreateShader(GLenum) { return NextObject(); }
GLuint glCreateProgram() { return NextObject(); }

void glGetShaderiv(GLuint, GLenum pname, GLint* params) {
  if (params == nullptr) {
    return;
  }
  if (!RequireTestGraphicsStub()) {
    *params = 0;
    return;
  }
  if (pname == kGlCompileStatus) {
    *params = 1;
  } else if (pname == kGlInfoLogLength) {
    *params = 1;
  } else {
    *params = 1;
  }
}

void glGetProgramiv(GLuint, GLenum pname, GLint* params) {
  if (params == nullptr) {
    return;
  }
  if (!RequireTestGraphicsStub()) {
    *params = 0;
    return;
  }
  if (pname == kGlLinkStatus) {
    *params = 1;
  } else if (pname == kGlInfoLogLength) {
    *params = 1;
  } else {
    *params = 1;
  }
}

void glGetShaderInfoLog(GLuint, GLsizei buf_size, GLsizei* length,
                        GLchar* info_log) {
  if (!RequireTestGraphicsStub()) {
    CopyString("real GLES backend unavailable", buf_size, length, info_log);
    return;
  }
  CopyString("", buf_size, length, info_log);
}

void glGetProgramInfoLog(GLuint, GLsizei buf_size, GLsizei* length,
                         GLchar* info_log) {
  if (!RequireTestGraphicsStub()) {
    CopyString("real GLES backend unavailable", buf_size, length, info_log);
    return;
  }
  CopyString("", buf_size, length, info_log);
}

void glGetActiveUniform(GLuint, GLuint, GLsizei buf_size,
                        GLsizei* length, GLint* size, GLenum* type,
                        GLchar* name) {
  if (!RequireTestGraphicsStub()) {
    if (size != nullptr) {
      *size = 0;
    }
    if (type != nullptr) {
      *type = 0;
    }
    CopyString("", buf_size, length, name);
    return;
  }
  if (size != nullptr) {
    *size = 1;
  }
  if (type != nullptr) {
    *type = 0x1406;
  }
  CopyString("", buf_size, length, name);
}

GLint glGetUniformLocation(GLuint, const GLchar*) {
  return RequireTestGraphicsStub() ? 0 : -1;
}

GLenum glCheckFramebufferStatus(GLenum) {
  return RequireTestGraphicsStub() ? kGlFramebufferComplete
                                   : kGlFramebufferUnsupported;
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
  if (!RequireTestGraphicsStub()) {
    return;
  }
  g_viewport[0] = x;
  g_viewport[1] = y;
  g_viewport[2] = width;
  g_viewport[3] = height;
}

void glActiveTexture(GLenum) { RequireTestGraphicsStub(); }
void glAttachShader(GLuint, GLuint) {
  RequireTestGraphicsStub();
}
void glBindAttribLocation(GLuint, GLuint,
                          const GLchar*) {
  RequireTestGraphicsStub();
}
void glBindBuffer(GLenum, GLuint) {
  RequireTestGraphicsStub();
}
void glBindFramebuffer(GLenum, GLuint) {
  RequireTestGraphicsStub();
}
void glBindRenderbuffer(GLenum, GLuint) {
  RequireTestGraphicsStub();
}
void glBindTexture(GLenum, GLuint) {
  RequireTestGraphicsStub();
}
void glBlendFunc(GLenum, GLenum) {
  RequireTestGraphicsStub();
}
void glBlendFuncSeparate(GLenum, GLenum,
                         GLenum, GLenum) {
  RequireTestGraphicsStub();
}
void glBufferData(GLenum, GLsizeiptr, const void*,
                  GLenum) {
  RequireTestGraphicsStub();
}
void glBufferSubData(GLenum, GLintptr,
                     GLsizeiptr, const void*) {
  RequireTestGraphicsStub();
}
void glClear(GLbitfield) { RequireTestGraphicsStub(); }
void glClearColor(GLfloat, GLfloat, GLfloat,
                  GLfloat) {
  RequireTestGraphicsStub();
}
void glClearDepthf(GLfloat) { RequireTestGraphicsStub(); }
void glClearStencil(GLint) { RequireTestGraphicsStub(); }
void glColorMask(GLboolean, GLboolean, GLboolean,
                 GLboolean) {
  RequireTestGraphicsStub();
}
void glCompileShader(GLuint) { RequireTestGraphicsStub(); }
void glCompressedTexImage2D(GLenum, GLint,
                            GLenum, GLsizei,
                            GLsizei, GLint,
                            GLsizei, const void*) {
  RequireTestGraphicsStub();
}
void glCompressedTexSubImage2D(GLenum, GLint,
                               GLint, GLint,
                               GLsizei, GLsizei,
                               GLenum, GLsizei,
                               const void*) {
  RequireTestGraphicsStub();
}
void glCopyTexSubImage2D(GLenum, GLint, GLint,
                         GLint, GLint, GLint,
                         GLsizei, GLsizei) {
  RequireTestGraphicsStub();
}
void glCullFace(GLenum) { RequireTestGraphicsStub(); }
void glDeleteBuffers(GLsizei, const GLuint*) {
  RequireTestGraphicsStub();
}
void glDeleteFramebuffers(GLsizei, const GLuint*) {
  RequireTestGraphicsStub();
}
void glDeleteProgram(GLuint) { RequireTestGraphicsStub(); }
void glDeleteRenderbuffers(GLsizei, const GLuint*) {
  RequireTestGraphicsStub();
}
void glDeleteShader(GLuint) { RequireTestGraphicsStub(); }
void glDeleteTextures(GLsizei, const GLuint*) {
  RequireTestGraphicsStub();
}
void glDepthFunc(GLenum) { RequireTestGraphicsStub(); }
void glDepthMask(GLboolean) { RequireTestGraphicsStub(); }
void glDisable(GLenum) { RequireTestGraphicsStub(); }
void glDisableVertexAttribArray(GLuint) { RequireTestGraphicsStub(); }
void glDrawArrays(GLenum, GLint, GLsizei) {
  RequireTestGraphicsStub();
}
void glDrawElements(GLenum, GLsizei, GLenum,
                    const void*) {
  RequireTestGraphicsStub();
}
void glEnable(GLenum) { RequireTestGraphicsStub(); }
void glEnableVertexAttribArray(GLuint) { RequireTestGraphicsStub(); }
void glFramebufferRenderbuffer(GLenum, GLenum,
                               GLenum,
                               GLuint) {
  RequireTestGraphicsStub();
}
void glFramebufferTexture2D(GLenum, GLenum,
                            GLenum, GLuint,
                            GLint) {
  RequireTestGraphicsStub();
}
void glGenerateMipmap(GLenum) { RequireTestGraphicsStub(); }
void glLinkProgram(GLuint) { RequireTestGraphicsStub(); }
void glPixelStorei(GLenum, GLint) {
  RequireTestGraphicsStub();
}
void glPolygonOffset(GLfloat, GLfloat) {
  RequireTestGraphicsStub();
}
void glReadPixels(GLint, GLint, GLsizei width, GLsizei height,
                  GLenum, GLenum, void* pixels) {
  if (!RequireTestGraphicsStub()) {
    return;
  }
  if (pixels != nullptr && width > 0 && height > 0) {
    std::memset(pixels, 0,
                static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
  }
}
void glReleaseShaderCompiler() { RequireTestGraphicsStub(); }
void glRenderbufferStorage(GLenum, GLenum,
                           GLsizei, GLsizei) {
  RequireTestGraphicsStub();
}
void glScissor(GLint, GLint, GLsizei,
               GLsizei) {
  RequireTestGraphicsStub();
}
void glShaderSource(GLuint, GLsizei,
                    const GLchar* const*, const GLint*) {
  RequireTestGraphicsStub();
}
void glStencilFunc(GLenum, GLint, GLuint) {
  RequireTestGraphicsStub();
}
void glStencilMask(GLuint) { RequireTestGraphicsStub(); }
void glStencilOp(GLenum, GLenum, GLenum) {
  RequireTestGraphicsStub();
}
void glTexImage2D(GLenum, GLint, GLint,
                  GLsizei, GLsizei, GLint,
                  GLenum, GLenum, const void*) {
  RequireTestGraphicsStub();
}
void glTexParameterf(GLenum, GLenum, GLfloat) {
  RequireTestGraphicsStub();
}
void glTexParameterfv(GLenum, GLenum,
                      const GLfloat*) {
  RequireTestGraphicsStub();
}
void glTexParameteri(GLenum, GLenum, GLint) {
  RequireTestGraphicsStub();
}
void glTexSubImage2D(GLenum, GLint, GLint,
                     GLint, GLsizei, GLsizei,
                     GLenum, GLenum,
                     const void*) {
  RequireTestGraphicsStub();
}
void glUniform1i(GLint, GLint) {
  RequireTestGraphicsStub();
}
void glUseProgram(GLuint) { RequireTestGraphicsStub(); }
void glVertexAttribPointer(GLuint, GLint, GLenum,
                           GLboolean, GLsizei,
                           const void*) {
  RequireTestGraphicsStub();
}

}  // extern "C"
