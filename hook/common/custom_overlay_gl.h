/**
 * Custom Overlay - OpenGL Backend
 *
 * Hybrid implementation with:
 * - Modern path (GL 3.0+): Uses shaders, VAO, VBO
 * - Legacy path (GL < 3.0): Fixed-function pipeline fallback
 */

#pragma once

#include "custom_overlay.h"
#include <GL/gl.h>
#include <windows.h>

namespace CustomOverlay {

class OpenGLBackend : public RendererBackend {
public:
  OpenGLBackend();
  virtual ~OpenGLBackend();

  bool Initialize(int fontTextureWidth, int fontTextureHeight,
                  const uint8_t *fontTextureData) override;
  void Shutdown() override;

  void Render(const std::vector<DrawVertex> &vertices,
              const std::vector<uint16_t> &indices,
              const std::vector<DrawCommand> &commands, int viewportWidth,
              int viewportHeight) override;

private:
  GLuint fontTextureId = 0;
  int texWidth = 0;
  int texHeight = 0;
  bool initialized = false;
  bool useModernPath = false;

  struct ModernResources {
    GLuint programTextured = 0;
    GLuint programSolid = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ibo = 0;
    GLint uViewportTextured = -1;
    GLint uViewportSolid = -1;
    GLint uTexture = -1;
    bool valid = false;
  } modern;

  bool InitModernPath();
  void ShutdownModernPath();
  void RenderModern(const std::vector<DrawVertex> &vertices,
                    const std::vector<uint16_t> &indices,
                    const std::vector<DrawCommand> &commands, int viewportWidth,
                    int viewportHeight);
  void RenderLegacy(const std::vector<DrawVertex> &vertices,
                    const std::vector<uint16_t> &indices,
                    const std::vector<DrawCommand> &commands, int viewportWidth,
                    int viewportHeight);

  static GLuint CompileShader(GLenum type, const char *source);
  static GLuint LinkProgram(GLuint vs, GLuint fs);
};

} // namespace CustomOverlay
