/**
 * Custom Overlay - OpenGL Backend Implementation
 *
 * Uses OpenGL 1.1/2.1 fixed-function pipeline for maximum compatibility.
 */

#include "custom_overlay_gl.h"

namespace CustomOverlay {

OpenGLBackend::OpenGLBackend() {}

OpenGLBackend::~OpenGLBackend() { Shutdown(); }

bool OpenGLBackend::Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData)
{
    if (initialized) return true;

    texWidth = fontTextureWidth;
    texHeight = fontTextureHeight;

    // Create font texture
    glGenTextures(1, &fontTextureId);
    glBindTexture(GL_TEXTURE_2D, fontTextureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fontTextureWidth, fontTextureHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 fontTextureData);
    glBindTexture(GL_TEXTURE_2D, 0);

    initialized = true;
    return true;
}

void OpenGLBackend::Shutdown()
{
    if (fontTextureId) {
        glDeleteTextures(1, &fontTextureId);
        fontTextureId = 0;
    }
    initialized = false;
}

void OpenGLBackend::Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                           const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight)
{
    if (!initialized || vertices.empty() || commands.empty()) return;

    // Save OpenGL state
    GLint lastTexture;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);
    GLint lastViewport[4];
    glGetIntegerv(GL_VIEWPORT, lastViewport);
    GLboolean lastBlend = glIsEnabled(GL_BLEND);
    GLboolean lastDepthTest = glIsEnabled(GL_DEPTH_TEST);
    GLboolean lastCullFace = glIsEnabled(GL_CULL_FACE);
    GLboolean lastTexture2D = glIsEnabled(GL_TEXTURE_2D);

    // Setup render state
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_TEXTURE_2D);

    // Setup viewport and orthographic projection
    glViewport(0, 0, viewportWidth, viewportHeight);

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, viewportWidth, viewportHeight, 0, -1, 1);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    // Enable vertex arrays
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);

    // Set up vertex pointers
    const DrawVertex* vtx = vertices.data();
    glVertexPointer(2, GL_FLOAT, sizeof(DrawVertex), &vtx->x);
    glTexCoordPointer(2, GL_FLOAT, sizeof(DrawVertex), &vtx->u);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(DrawVertex), &vtx->color);

    // Draw each command
    for (const auto& cmd : commands) {
        if (cmd.useTexture) {
            glBindTexture(GL_TEXTURE_2D, fontTextureId);
        } else {
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        glDrawElements(GL_TRIANGLES, cmd.indexCount, GL_UNSIGNED_SHORT, indices.data() + cmd.indexOffset);
    }

    // Disable vertex arrays
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    // Restore matrices
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    // Restore state
    glBindTexture(GL_TEXTURE_2D, lastTexture);
    glViewport(lastViewport[0], lastViewport[1], lastViewport[2], lastViewport[3]);
    if (lastBlend)
        glEnable(GL_BLEND);
    else
        glDisable(GL_BLEND);
    if (lastDepthTest)
        glEnable(GL_DEPTH_TEST);
    else
        glDisable(GL_DEPTH_TEST);
    if (lastCullFace)
        glEnable(GL_CULL_FACE);
    else
        glDisable(GL_CULL_FACE);
    if (lastTexture2D)
        glEnable(GL_TEXTURE_2D);
    else
        glDisable(GL_TEXTURE_2D);
}

}  // namespace CustomOverlay
