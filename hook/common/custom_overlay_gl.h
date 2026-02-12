/**
 * Custom Overlay - OpenGL Backend
 *
 * Renders overlay using OpenGL 2.1+ (compatibility profile).
 * Uses fixed-function pipeline for maximum compatibility.
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
};

} // namespace CustomOverlay
