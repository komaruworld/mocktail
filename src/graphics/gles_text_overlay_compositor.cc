#include "mocktail/graphics/gles_text_overlay_compositor.h"

#include <GLES3/gl3.h>
#include <SDL3/SDL.h>

#include <array>
#include <cstdio>
#include <vector>

namespace mocktail::graphics {
namespace {

constexpr std::uint64_t kMaximumOverlayBytes = 64ULL * 1024ULL * 1024ULL;

#define OVERLAY_GL_FUNCTIONS(F)                                                \
  F(PFNGLACTIVETEXTUREPROC, ActiveTexture)                                     \
  F(PFNGLATTACHSHADERPROC, AttachShader)                                       \
  F(PFNGLBINDBUFFERPROC, BindBuffer)                                           \
  F(PFNGLBINDFRAMEBUFFERPROC, BindFramebuffer)                                 \
  F(PFNGLBINDSAMPLERPROC, BindSampler)                                         \
  F(PFNGLBINDTEXTUREPROC, BindTexture)                                         \
  F(PFNGLBINDVERTEXARRAYPROC, BindVertexArray)                                 \
  F(PFNGLBLENDEQUATIONSEPARATEPROC, BlendEquationSeparate)                     \
  F(PFNGLBLENDFUNCSEPARATEPROC, BlendFuncSeparate)                             \
  F(PFNGLCOLORMASKPROC, ColorMask)                                             \
  F(PFNGLCOMPILESHADERPROC, CompileShader)                                     \
  F(PFNGLCREATEPROGRAMPROC, CreateProgram)                                     \
  F(PFNGLCREATESHADERPROC, CreateShader)                                       \
  F(PFNGLDELETEPROGRAMPROC, DeleteProgram)                                     \
  F(PFNGLDELETESHADERPROC, DeleteShader)                                       \
  F(PFNGLDELETETEXTURESPROC, DeleteTextures)                                   \
  F(PFNGLDELETEVERTEXARRAYSPROC, DeleteVertexArrays)                           \
  F(PFNGLDISABLEPROC, Disable)                                                 \
  F(PFNGLDRAWARRAYSPROC, DrawArrays)                                           \
  F(PFNGLDRAWBUFFERSPROC, DrawBuffers)                                         \
  F(PFNGLENABLEPROC, Enable)                                                   \
  F(PFNGLGENTEXTURESPROC, GenTextures)                                         \
  F(PFNGLGENVERTEXARRAYSPROC, GenVertexArrays)                                 \
  F(PFNGLGETBOOLEANVPROC, GetBooleanv)                                         \
  F(PFNGLGETINTEGERVPROC, GetIntegerv)                                         \
  F(PFNGLGETPROGRAMINFOLOGPROC, GetProgramInfoLog)                             \
  F(PFNGLGETPROGRAMIVPROC, GetProgramiv)                                       \
  F(PFNGLGETSHADERINFOLOGPROC, GetShaderInfoLog)                               \
  F(PFNGLGETSHADERIVPROC, GetShaderiv)                                         \
  F(PFNGLGETUNIFORMLOCATIONPROC, GetUniformLocation)                           \
  F(PFNGLISENABLEDPROC, IsEnabled)                                             \
  F(PFNGLLINKPROGRAMPROC, LinkProgram)                                         \
  F(PFNGLPIXELSTOREIPROC, PixelStorei)                                         \
  F(PFNGLSHADERSOURCEPROC, ShaderSource)                                       \
  F(PFNGLTEXIMAGE2DPROC, TexImage2D)                                           \
  F(PFNGLTEXPARAMETERIPROC, TexParameteri)                                     \
  F(PFNGLUNIFORM1IPROC, Uniform1i)                                             \
  F(PFNGLUNIFORM4FPROC, Uniform4f)                                             \
  F(PFNGLUSEPROGRAMPROC, UseProgram)                                           \
  F(PFNGLVIEWPORTPROC, Viewport)

struct Gl {
#define DECLARE_GL(type, name) type name = nullptr;
  OVERLAY_GL_FUNCTIONS(DECLARE_GL)
#undef DECLARE_GL

  bool Load() {
#define LOAD_GL(type, name)                                                    \
  name = reinterpret_cast<type>(SDL_GL_GetProcAddress("gl" #name));            \
  if (name == nullptr)                                                         \
    return false;
    OVERLAY_GL_FUNCTIONS(LOAD_GL)
#undef LOAD_GL
    return true;
  }
};
#undef OVERLAY_GL_FUNCTIONS

constexpr GLenum kCapabilities[] = {GL_BLEND,
                                    GL_CULL_FACE,
                                    GL_DEPTH_TEST,
                                    GL_STENCIL_TEST,
                                    GL_SCISSOR_TEST,
                                    GL_RASTERIZER_DISCARD,
                                    GL_SAMPLE_ALPHA_TO_COVERAGE,
                                    GL_SAMPLE_COVERAGE};
constexpr GLenum kUnpackParameters[] = {
    GL_UNPACK_ALIGNMENT, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS,
    GL_UNPACK_SKIP_PIXELS};

// Roblox caches GL state. Every binding and state changed here must be restored
// even if a new overlay revision disappears while it is being copied.
struct SavedGlState {
  explicit SavedGlState(Gl &api) : gl(api) {
    gl.GetIntegerv(GL_CURRENT_PROGRAM, &program);
    gl.GetIntegerv(GL_VERTEX_ARRAY_BINDING, &vertex_array);
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    gl.GetIntegerv(GL_VIEWPORT, viewport.data());
    gl.GetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    gl.GetIntegerv(GL_SAMPLER_BINDING, &sampler);
    gl.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
    for (std::size_t i = 0; i < unpack.size(); ++i) {
      gl.GetIntegerv(kUnpackParameters[i], &unpack[i]);
    }
    gl.GetIntegerv(GL_BLEND_SRC_RGB, &blend_src_rgb);
    gl.GetIntegerv(GL_BLEND_DST_RGB, &blend_dst_rgb);
    gl.GetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src_alpha);
    gl.GetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst_alpha);
    gl.GetIntegerv(GL_BLEND_EQUATION_RGB, &blend_equation_rgb);
    gl.GetIntegerv(GL_BLEND_EQUATION_ALPHA, &blend_equation_alpha);
    gl.GetBooleanv(GL_COLOR_WRITEMASK, color_mask.data());
    for (std::size_t i = 0; i < enabled.size(); ++i) {
      enabled[i] = gl.IsEnabled(kCapabilities[i]);
    }
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    gl.GetIntegerv(GL_DRAW_BUFFER0, &backbuffer_draw_buffer);
  }

  ~SavedGlState() {
    const GLenum draw_buffer = static_cast<GLenum>(backbuffer_draw_buffer);
    gl.DrawBuffers(1, &draw_buffer);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
    gl.Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    gl.UseProgram(program);
    gl.BindVertexArray(vertex_array);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.BindSampler(0, sampler);
    gl.ActiveTexture(active_texture);
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer);
    for (std::size_t i = 0; i < unpack.size(); ++i) {
      gl.PixelStorei(kUnpackParameters[i], unpack[i]);
    }
    gl.BlendFuncSeparate(blend_src_rgb, blend_dst_rgb, blend_src_alpha,
                         blend_dst_alpha);
    gl.BlendEquationSeparate(blend_equation_rgb, blend_equation_alpha);
    gl.ColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    for (std::size_t i = 0; i < enabled.size(); ++i) {
      (enabled[i] ? gl.Enable : gl.Disable)(kCapabilities[i]);
    }
  }

  Gl &gl;
  GLint program = 0, vertex_array = 0, framebuffer = 0;
  GLint active_texture = GL_TEXTURE0, texture = 0, sampler = 0;
  GLint unpack_buffer = 0, backbuffer_draw_buffer = GL_BACK;
  GLint blend_src_rgb = 0, blend_dst_rgb = 0;
  GLint blend_src_alpha = 0, blend_dst_alpha = 0;
  GLint blend_equation_rgb = 0, blend_equation_alpha = 0;
  std::array<GLint, 4> viewport{}, unpack{};
  std::array<GLboolean, 4> color_mask{};
  std::array<GLboolean, sizeof(kCapabilities) / sizeof(kCapabilities[0])>
      enabled{};
};

struct PixelCopy {
  explicit PixelCopy(std::size_t bytes) : pixels(bytes) {}
  ~PixelCopy() {
    volatile std::uint8_t *bytes = pixels.data();
    for (std::size_t i = 0; i < pixels.size(); ++i)
      bytes[i] = 0;
  }
  std::vector<std::uint8_t> pixels;
};

bool ValidFrame(const MocktailTextOverlayFrameInfo &frame) {
  const std::uint64_t row_bytes = std::uint64_t{frame.width} * 4;
  const std::uint64_t bytes = row_bytes * frame.height;
  return frame.abi_version == MocktailTextOverlayFrameInfo::kAbiVersion &&
         frame.coordinate_width != 0 && frame.coordinate_height != 0 &&
         frame.width != 0 && frame.height != 0 &&
         row_bytes <= kMaximumOverlayBytes && bytes <= kMaximumOverlayBytes &&
         frame.row_bytes == row_bytes && frame.rgba_bytes == bytes;
}

} // namespace

struct GlesTextOverlayCompositor::Impl {
  explicit Impl(GlesTextOverlaySource input) : source(input) {}
  ~Impl() {
    if (context != nullptr && SDL_GL_GetCurrentContext() == context) {
      if (texture != 0)
        gl.DeleteTextures(1, &texture);
      if (vertex_array != 0)
        gl.DeleteVertexArrays(1, &vertex_array);
      if (program != 0)
        gl.DeleteProgram(program);
    }
  }

  bool Fail(const char *message) {
    if (!failure_logged) {
      std::fprintf(stderr, "  [gles-overlay] %s\n", message);
      failure_logged = true;
    }
    return false;
  }

  GLuint Compile(GLenum type, const char *code) {
    const GLuint shader = gl.CreateShader(type);
    if (shader == 0)
      return 0;
    gl.ShaderSource(shader, 1, &code, nullptr);
    gl.CompileShader(shader);
    GLint compiled = GL_FALSE;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_TRUE)
      return shader;
    char log[1024]{};
    gl.GetShaderInfoLog(shader, sizeof(log), nullptr, log);
    Fail(log);
    gl.DeleteShader(shader);
    return 0;
  }

  bool CreateResources() {
    constexpr const char *vertex_source = R"(#version 300 es
precision highp float;
uniform vec4 bounds;
out vec2 uv;
void main() {
  vec2 corners[4] = vec2[4](vec2(0, 0), vec2(1, 0),
                            vec2(0, 1), vec2(1, 1));
  uv = corners[gl_VertexID];
  gl_Position = vec4(mix(bounds.xy, bounds.zw, uv), 0, 1);
}
)";
    constexpr const char *fragment_source = R"(#version 300 es
precision mediump float;
uniform sampler2D image;
in vec2 uv;
out vec4 color;
void main() { color = texture(image, uv); }
)";
    const GLuint vertex = Compile(GL_VERTEX_SHADER, vertex_source);
    const GLuint fragment = Compile(GL_FRAGMENT_SHADER, fragment_source);
    if (vertex == 0 || fragment == 0) {
      if (vertex != 0)
        gl.DeleteShader(vertex);
      if (fragment != 0)
        gl.DeleteShader(fragment);
      return false;
    }
    program = gl.CreateProgram();
    if (program != 0) {
      gl.AttachShader(program, vertex);
      gl.AttachShader(program, fragment);
      gl.LinkProgram(program);
    }
    gl.DeleteShader(vertex);
    gl.DeleteShader(fragment);
    GLint linked = GL_FALSE;
    if (program != 0)
      gl.GetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
      char log[1024] = "unable to create the GLES text program";
      if (program != 0)
        gl.GetProgramInfoLog(program, sizeof(log), nullptr, log);
      return Fail(log);
    }
    bounds_location = gl.GetUniformLocation(program, "bounds");
    image_location = gl.GetUniformLocation(program, "image");
    gl.GenVertexArrays(1, &vertex_array);
    gl.GenTextures(1, &texture);
    gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
    return vertex_array != 0 && texture != 0 && bounds_location >= 0 &&
           image_location >= 0;
  }

  GlesTextOverlaySource source;
  Gl gl;
  SDL_GLContext context = nullptr;
  GLuint program = 0, vertex_array = 0, texture = 0;
  GLint bounds_location = -1, image_location = -1, max_texture_size = 0;
  bool initialized = false, failed = false, failure_logged = false;
  bool uploaded = false, ready_logged = false;
  std::uint64_t revision = 0;
  std::uint32_t width = 0, height = 0;
};

GlesTextOverlayCompositor::GlesTextOverlayCompositor(
    GlesTextOverlaySource source)
    : impl_(std::make_unique<Impl>(source)) {}
GlesTextOverlayCompositor::~GlesTextOverlayCompositor() = default;

bool GlesTextOverlayCompositor::Draw(SDL_Window *window) {
  Impl &state = *impl_;
  const GlesTextOverlaySource &source = state.source;
  if (source.may_present != nullptr && !source.may_present())
    return true;
  if (source.query == nullptr || source.copy == nullptr)
    return true;
  MocktailTextOverlayFrameInfo frame{};
  if (!source.query(&frame) || frame.visible == 0)
    return true;
  if (!ValidFrame(frame))
    return state.Fail("invalid text overlay frame");
  int pixel_width = 0, pixel_height = 0;
  if (window == nullptr ||
      !SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height) ||
      pixel_width <= 0 || pixel_height <= 0)
    return false;
  if (state.failed)
    return false;
  if (!state.initialized) {
    state.context = SDL_GL_GetCurrentContext();
    if (state.context == nullptr || !state.gl.Load()) {
      state.failed = true;
      return state.Fail("OpenGL ES 3.0 functions are unavailable");
    }
  } else if (SDL_GL_GetCurrentContext() != state.context) {
    return state.Fail("text overlay called with a different GL context");
  }
  Gl &gl = state.gl;
  SavedGlState saved(gl);
  if (!state.initialized) {
    if (!state.CreateResources()) {
      state.failed = true;
      return state.Fail("unable to initialize the GLES text overlay");
    }
    state.initialized = true;
  }
  if (frame.width > static_cast<std::uint32_t>(state.max_texture_size) ||
      frame.height > static_cast<std::uint32_t>(state.max_texture_size)) {
    return state.Fail("text overlay exceeds GL_MAX_TEXTURE_SIZE");
  }
  gl.BindTexture(GL_TEXTURE_2D, state.texture);
  gl.BindSampler(0, 0);
  if (!state.uploaded || state.revision != frame.revision ||
      state.width != frame.width || state.height != frame.height) {
    PixelCopy copy(static_cast<std::size_t>(frame.rgba_bytes));
    if (!source.copy(frame.revision, copy.pixels.data(), copy.pixels.size())) {
      return false;
    }
    gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.PixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    gl.PixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, frame.width, frame.height, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, copy.pixels.data());
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    state.revision = frame.revision;
    state.width = frame.width;
    state.height = frame.height;
    state.uploaded = true;
  }
  for (GLenum capability : kCapabilities)
    gl.Disable(capability);
  gl.Enable(GL_BLEND);
  gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
  gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                       GL_ONE_MINUS_SRC_ALPHA);
  gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  const GLenum back = GL_BACK;
  gl.DrawBuffers(1, &back);
  gl.Viewport(0, 0, pixel_width, pixel_height);
  gl.UseProgram(state.program);
  gl.BindVertexArray(state.vertex_array);
  gl.Uniform1i(state.image_location, 0);
  const double x_scale = 2.0 / frame.coordinate_width;
  const double y_scale = 2.0 / frame.coordinate_height;
  gl.Uniform4f(state.bounds_location, frame.x * x_scale - 1.0,
               1.0 - frame.y * y_scale,
               (static_cast<double>(frame.x) + frame.width) * x_scale - 1.0,
               1.0 - (static_cast<double>(frame.y) + frame.height) * y_scale);
  gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  if (!state.ready_logged) {
    std::fprintf(stderr,
                 "  [gles-overlay] same-surface TextBox compositor ready\n");
    state.ready_logged = true;
  }
  return true;
}

} // namespace mocktail::graphics
