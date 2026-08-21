#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>

using EGLBoolean = uint32_t;
using EGLint = int32_t;
using EGLDisplay = void*;
using EGLConfig = void*;
using EGLContext = void*;
using EGLSurface = void*;
using GLenum = uint32_t;
using GLint = int32_t;
using GLuint = uint32_t;

extern "C" {
EGLDisplay eglGetDisplay(void* display_id);
EGLBoolean eglInitialize(EGLDisplay display, EGLint* major, EGLint* minor);
EGLBoolean eglChooseConfig(EGLDisplay display, const EGLint* attrib_list,
                           EGLConfig* configs, EGLint config_size,
                           EGLint* num_config);
EGLContext eglCreateContext(EGLDisplay display, EGLConfig config,
                            EGLContext share_context,
                            const EGLint* attrib_list);
EGLSurface eglCreatePbufferSurface(EGLDisplay display, EGLConfig config,
                                   const EGLint* attrib_list);
EGLBoolean eglMakeCurrent(EGLDisplay display, EGLSurface draw, EGLSurface read,
                          EGLContext context);
EGLContext eglGetCurrentContext();
EGLint eglGetError();
const uint8_t* glGetString(GLenum name);
void glGenTextures(int32_t n, GLuint* textures);
GLuint glCreateShader(GLenum type);
void glGetShaderiv(GLuint shader, GLenum pname, GLint* params);
GLenum glCheckFramebufferStatus(GLenum target);
GLenum glGetError();
}

namespace {

TEST(HeadlessGraphicsTest, ProductionModeRejectsMissingRealDisplay) {
  ASSERT_EQ(unsetenv("MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS"), 0);
  EXPECT_EQ(eglGetDisplay(nullptr), nullptr);
  EXPECT_EQ(eglGetError(), 0x3001);
  EXPECT_EQ(glGetString(0x1f02), nullptr);
  EXPECT_EQ(glCreateShader(0x8b31), 0u);
  EXPECT_EQ(glCheckFramebufferStatus(0x8d40), 0x8cddu);
  EXPECT_EQ(glGetError(), 0x0502u);
}

TEST(HeadlessGraphicsTest, EglCreatesCurrentMockContext) {
  ASSERT_EQ(setenv("MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS", "1", 1), 0);
  EGLDisplay display = eglGetDisplay(nullptr);
  ASSERT_NE(display, nullptr);

  EGLint major = 0;
  EGLint minor = 0;
  EXPECT_EQ(eglInitialize(display, &major, &minor), 1u);
  EXPECT_GE(major, 1);

  EGLConfig config = nullptr;
  EGLint num_config = 0;
  EXPECT_EQ(eglChooseConfig(display, nullptr, &config, 1, &num_config), 1u);
  EXPECT_NE(config, nullptr);
  EXPECT_EQ(num_config, 1);

  const EGLint context_attrs[] = {0x3098, 2, 0x3038};
  EGLContext context =
      eglCreateContext(display, config, nullptr, context_attrs);
  ASSERT_NE(context, nullptr);

  EGLSurface surface = eglCreatePbufferSurface(display, config, nullptr);
  ASSERT_NE(surface, nullptr);
  EXPECT_EQ(eglMakeCurrent(display, surface, surface, context), 1u);
  EXPECT_EQ(eglGetCurrentContext(), context);
  EXPECT_EQ(eglGetError(), 0x3000);
}

TEST(HeadlessGraphicsTest, GlesReportsNoErrorAndSuccessfulObjects) {
  ASSERT_EQ(setenv("MOCKTAIL_ENABLE_TEST_GRAPHICS_STUBS", "1", 1), 0);
  EXPECT_NE(glGetString(0x1f02), nullptr);

  GLuint texture = 0;
  glGenTextures(1, &texture);
  EXPECT_NE(texture, 0u);

  GLuint shader = glCreateShader(0x8b31);
  EXPECT_NE(shader, 0u);

  GLint status = 0;
  glGetShaderiv(shader, 0x8b81, &status);
  EXPECT_EQ(status, 1);

  EXPECT_EQ(glCheckFramebufferStatus(0x8d40), 0x8cd5u);
  EXPECT_EQ(glGetError(), 0u);
}

}  // namespace
