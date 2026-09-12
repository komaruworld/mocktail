#include "mocktail/graphics/gles_text_overlay_compositor.h"

#include <GLES3/gl3.h>
#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <memory>
#include <vector>

namespace mocktail::graphics {
namespace {

#define GL(name)                                                               \
  reinterpret_cast<decltype(&gl##name)>(SDL_GL_GetProcAddress("gl" #name))

MocktailTextOverlayFrameInfo g_frame;
std::array<std::uint8_t, 16> g_pixels;
bool g_active = false, g_copy_succeeds = true;
int g_queries = 0, g_copies = 0;

bool MayPresent() { return g_active; }
bool Query(MocktailTextOverlayFrameInfo *frame) {
  ++g_queries;
  *frame = g_frame;
  return true;
}
bool Copy(std::uint64_t revision, void *data, std::size_t size) {
  ++g_copies;
  if (!g_copy_succeeds || revision != g_frame.revision ||
      size != g_pixels.size())
    return false;
  std::memcpy(data, g_pixels.data(), g_pixels.size());
  return true;
}

TEST(GlesTextOverlayCompositorTest, InactiveNeedsNeitherWindowNorGlContext) {
  g_active = false;
  g_queries = 0;
  GlesTextOverlayCompositor compositor({MayPresent, Query, Copy});
  EXPECT_TRUE(compositor.Draw(nullptr));
  EXPECT_EQ(g_queries, 0);
}

class GlesTextOverlayRenderingTest : public ::testing::Test {
protected:
  void SetUp() override {
    SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
    if (!SDL_Init(SDL_INIT_VIDEO))
      GTEST_SKIP() << SDL_GetError();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    window = SDL_CreateWindow("GLES overlay test", 256, 192,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (window == nullptr)
      GTEST_SKIP() << SDL_GetError();
    context = SDL_GL_CreateContext(window);
    if (context == nullptr)
      GTEST_SKIP() << SDL_GetError();
    ASSERT_TRUE(SDL_GL_MakeCurrent(window, context));
    ASSERT_TRUE(SDL_GetWindowSizeInPixels(window, &pixel_width, &pixel_height));
    ASSERT_EQ(pixel_width, 256);
    ASSERT_EQ(pixel_height, 192);
    g_frame = {};
    g_frame.visible = 1;
    g_frame.revision = 1;
    g_frame.coordinate_width = 128;
    g_frame.coordinate_height = 96;
    g_frame.x = 16;
    g_frame.y = 8;
    g_frame.width = 2;
    g_frame.height = 2;
    g_frame.row_bytes = 8;
    g_frame.rgba_bytes = 16;
    g_pixels = {255, 0,   0,   128, 0,   255, 0, 255,
                255, 255, 255, 0,   255, 255, 0, 255};
    g_active = true;
    g_copy_succeeds = true;
    g_queries = 0;
    g_copies = 0;
    compositor = std::make_unique<GlesTextOverlayCompositor>(
        GlesTextOverlaySource{MayPresent, Query, Copy});
    Clear();
  }

  void TearDown() override {
    compositor.reset();
    if (context != nullptr)
      SDL_GL_DestroyContext(context);
    if (window != nullptr)
      SDL_DestroyWindow(window);
    SDL_Quit();
  }

  void Clear() {
    GL(ClearColor)(0, 0, 1, 1);
    GL(Clear)(GL_COLOR_BUFFER_BIT);
  }

  std::array<std::uint8_t, 4> Pixel(int x, int y_from_top) {
    GLint previous = 0;
    GL(GetIntegerv)(GL_READ_FRAMEBUFFER_BINDING, &previous);
    GL(BindFramebuffer)(GL_READ_FRAMEBUFFER, 0);
    std::array<std::uint8_t, 4> pixel{};
    GL(ReadPixels)(x, pixel_height - 1 - y_from_top, 1, 1, GL_RGBA,
                   GL_UNSIGNED_BYTE, pixel.data());
    GL(BindFramebuffer)(GL_READ_FRAMEBUFFER, previous);
    return pixel;
  }

  std::vector<GLint> Snapshot() {
    const GLenum parameters[] = {GL_CURRENT_PROGRAM,
                                 GL_VERTEX_ARRAY_BINDING,
                                 GL_DRAW_FRAMEBUFFER_BINDING,
                                 GL_READ_FRAMEBUFFER_BINDING,
                                 GL_DRAW_BUFFER0,
                                 GL_ACTIVE_TEXTURE,
                                 GL_TEXTURE_BINDING_2D,
                                 GL_SAMPLER_BINDING,
                                 GL_PIXEL_UNPACK_BUFFER_BINDING,
                                 GL_UNPACK_ALIGNMENT,
                                 GL_UNPACK_ROW_LENGTH,
                                 GL_UNPACK_SKIP_ROWS,
                                 GL_UNPACK_SKIP_PIXELS,
                                 GL_BLEND_SRC_RGB,
                                 GL_BLEND_DST_RGB,
                                 GL_BLEND_SRC_ALPHA,
                                 GL_BLEND_DST_ALPHA,
                                 GL_BLEND_EQUATION_RGB,
                                 GL_BLEND_EQUATION_ALPHA};
    std::vector<GLint> values;
    for (GLenum parameter : parameters) {
      GLint value = 0;
      GL(GetIntegerv)(parameter, &value);
      values.push_back(value);
    }
    GLint active = 0;
    GL(GetIntegerv)(GL_ACTIVE_TEXTURE, &active);
    GL(ActiveTexture)(GL_TEXTURE0);
    GLint texture = 0, sampler = 0;
    GL(GetIntegerv)(GL_TEXTURE_BINDING_2D, &texture);
    GL(GetIntegerv)(GL_SAMPLER_BINDING, &sampler);
    values.push_back(texture);
    values.push_back(sampler);
    GL(ActiveTexture)(active);
    GLint viewport[4]{};
    GL(GetIntegerv)(GL_VIEWPORT, viewport);
    values.insert(values.end(), viewport, viewport + 4);
    GLboolean mask[4]{};
    GL(GetBooleanv)(GL_COLOR_WRITEMASK, mask);
    values.insert(values.end(), mask, mask + 4);
    for (GLenum capability :
         {GL_BLEND, GL_CULL_FACE, GL_DEPTH_TEST, GL_STENCIL_TEST,
          GL_SCISSOR_TEST, GL_RASTERIZER_DISCARD, GL_SAMPLE_ALPHA_TO_COVERAGE,
          GL_SAMPLE_COVERAGE}) {
      values.push_back(GL(IsEnabled)(capability));
    }
    return values;
  }

  GLuint GameProgram() {
    const char *vertex = "#version 300 es\nvoid main(){gl_Position=vec4(0);}";
    const char *fragment = "#version 300 es\nprecision mediump float;"
                           "out vec4 c;void main(){c=vec4(1);}";
    GLuint shaders[2] = {GL(CreateShader)(GL_VERTEX_SHADER),
                         GL(CreateShader)(GL_FRAGMENT_SHADER)};
    GL(ShaderSource)(shaders[0], 1, &vertex, nullptr);
    GL(ShaderSource)(shaders[1], 1, &fragment, nullptr);
    GLuint program = GL(CreateProgram)();
    for (GLuint shader : shaders) {
      GL(CompileShader)(shader);
      GL(AttachShader)(program, shader);
    }
    GL(LinkProgram)(program);
    GLint linked = GL_FALSE;
    GL(GetProgramiv)(program, GL_LINK_STATUS, &linked);
    EXPECT_EQ(linked, GL_TRUE);
    for (GLuint shader : shaders)
      GL(DeleteShader)(shader);
    return program;
  }

  SDL_Window *window = nullptr;
  SDL_GLContext context = nullptr;
  int pixel_width = 0, pixel_height = 0;
  std::unique_ptr<GlesTextOverlayCompositor> compositor;
};

TEST_F(GlesTextOverlayRenderingTest,
       DrawsAlphaAtLogicalCoordinatesRestoringState) {
  GLuint textures[2]{}, sampler = 0, vao = 0, buffer = 0, framebuffers[2]{};
  GL(GenTextures)(2, textures);
  GL(ActiveTexture)(GL_TEXTURE0);
  GL(BindTexture)(GL_TEXTURE_2D, textures[0]);
  GL(GenSamplers)(1, &sampler);
  GL(BindSampler)(0, sampler);
  GL(ActiveTexture)(GL_TEXTURE3);
  GL(BindTexture)(GL_TEXTURE_2D, textures[1]);
  GL(GenVertexArrays)(1, &vao);
  GL(BindVertexArray)(vao);
  GL(GenBuffers)(1, &buffer);
  GL(BindBuffer)(GL_PIXEL_UNPACK_BUFFER, buffer);
  GL(BufferData)(GL_PIXEL_UNPACK_BUFFER, 128, nullptr, GL_STATIC_DRAW);
  GL(PixelStorei)(GL_UNPACK_ALIGNMENT, 8);
  GL(PixelStorei)(GL_UNPACK_ROW_LENGTH, 31);
  GL(PixelStorei)(GL_UNPACK_SKIP_ROWS, 3);
  GL(PixelStorei)(GL_UNPACK_SKIP_PIXELS, 4);
  const GLenum no_draw = GL_NONE;
  GL(DrawBuffers)(1, &no_draw);
  GL(GenFramebuffers)(2, framebuffers);
  GL(BindFramebuffer)(GL_DRAW_FRAMEBUFFER, framebuffers[0]);
  GL(BindFramebuffer)(GL_READ_FRAMEBUFFER, framebuffers[1]);
  GL(Viewport)(7, 9, 33, 55);
  GL(ColorMask)(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
  GL(BlendFuncSeparate)(GL_ONE, GL_ZERO, GL_DST_ALPHA, GL_SRC_ALPHA);
  GL(BlendEquationSeparate)(GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT);
  GL(Disable)(GL_BLEND);
  for (GLenum capability : {GL_CULL_FACE, GL_DEPTH_TEST, GL_STENCIL_TEST,
                            GL_SCISSOR_TEST, GL_RASTERIZER_DISCARD,
                            GL_SAMPLE_ALPHA_TO_COVERAGE, GL_SAMPLE_COVERAGE}) {
    GL(Enable)(capability);
  }
  GL(Scissor)(0, 0, 0, 0);
  GL(UseProgram)(GameProgram());
  ASSERT_EQ(GL(GetError)(), GLenum{GL_NO_ERROR});
  const auto before = Snapshot();
  ASSERT_TRUE(compositor->Draw(window));
  EXPECT_EQ(Snapshot(), before);
  EXPECT_EQ(GL(GetError)(), GLenum{GL_NO_ERROR});
  const auto red = Pixel(32, 16);
  EXPECT_NEAR(red[0], 128, 1);
  EXPECT_EQ(red[1], 0);
  EXPECT_NEAR(red[2], 127, 1);
  EXPECT_EQ(red[3], 255);
  EXPECT_EQ(Pixel(35, 16), (std::array<std::uint8_t, 4>{0, 255, 0, 255}));
  EXPECT_EQ(Pixel(32, 19), (std::array<std::uint8_t, 4>{0, 0, 255, 255}));
  EXPECT_EQ(Pixel(35, 19), (std::array<std::uint8_t, 4>{255, 255, 0, 255}));
  EXPECT_EQ(Pixel(5, 5), (std::array<std::uint8_t, 4>{0, 0, 255, 255}));
  EXPECT_EQ(GL(GetError)(), GLenum{GL_NO_ERROR});
}

TEST_F(GlesTextOverlayRenderingTest,
       CachesRevisionsAndDoesNotDrawStaleOrHiddenText) {
  ASSERT_TRUE(compositor->Draw(window));
  ASSERT_EQ(g_copies, 1);
  ASSERT_TRUE(compositor->Draw(window));
  EXPECT_EQ(g_copies, 1);
  Clear();
  ++g_frame.revision;
  g_copy_succeeds = false;
  const auto before = Snapshot();
  EXPECT_FALSE(compositor->Draw(window));
  EXPECT_EQ(Snapshot(), before);
  EXPECT_EQ(Pixel(35, 16), (std::array<std::uint8_t, 4>{0, 0, 255, 255}));
  g_copy_succeeds = true;
  g_pixels[4] = 255;
  g_pixels[5] = 0;
  ASSERT_TRUE(compositor->Draw(window));
  EXPECT_EQ(Pixel(35, 16), (std::array<std::uint8_t, 4>{255, 0, 0, 255}));
  Clear();
  g_frame.visible = 0;
  EXPECT_TRUE(compositor->Draw(window));
  EXPECT_EQ(Pixel(35, 16), (std::array<std::uint8_t, 4>{0, 0, 255, 255}));
  EXPECT_EQ(GL(GetError)(), GLenum{GL_NO_ERROR});
}

TEST_F(GlesTextOverlayRenderingTest,
       RejectsMalformedFramesBeforeCopyingPixels) {
  const auto before = Snapshot();
  g_frame.row_bytes = 7;
  EXPECT_FALSE(compositor->Draw(window));
  g_frame.row_bytes = 8;
  g_frame.rgba_bytes = 15;
  EXPECT_FALSE(compositor->Draw(window));
  g_frame.rgba_bytes = 16;
  g_frame.coordinate_width = 0;
  EXPECT_FALSE(compositor->Draw(window));
  g_frame.coordinate_width = 128;
  g_frame.width = UINT32_MAX;
  g_frame.height = UINT32_MAX;
  EXPECT_FALSE(compositor->Draw(window));
  EXPECT_EQ(g_copies, 0);
  EXPECT_EQ(Snapshot(), before);
  EXPECT_EQ(GL(GetError)(), GLenum{GL_NO_ERROR});
}

#undef GL

} // namespace
} // namespace mocktail::graphics
