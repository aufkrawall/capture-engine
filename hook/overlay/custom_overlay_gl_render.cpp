#include "custom_overlay_gl_internal.h"

namespace CustomOverlay {
void OpenGLBackend::RenderLegacy(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                                 const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) {
    HGLRC currentContext = custom_overlay_gl_pwglGetCurrentContext ? custom_overlay_gl_pwglGetCurrentContext() : nullptr;
    if (currentContext != legacyProbeContext) {
        legacyProbeContext = currentContext;
        legacyMatrixChecked = false;
        legacyMatrixValid = true;
        legacyArrayChecked = false;
        legacyArrayProbeSucceeded = false;
    }

    // Everything below changes the application's context; it is all captured
    // first and put back exactly (see gl_overlay_state_policy.h). Capture leaves
    // texture unit 0 active.
    GLHookStateApi stateApi;
    const ce::gl_overlay_state::Snapshot applicationState = ce::gl_overlay_state::Capture(stateApi, stateCaps);
    GLint lastClientActiveTexture = GL_TEXTURE0;
    if (custom_overlay_gl_pglClientActiveTexture) {
        custom_overlay_gl_pglGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &lastClientActiveTexture);
        custom_overlay_gl_pglClientActiveTexture(GL_TEXTURE0);
    }

    // Fixed-function draws would run through an application GLSL program.
    if (stateCaps.programs)
        custom_overlay_gl_pglUseProgramState(0);
    // Client pointers below address CPU memory. Move to VAO 0 and clear the
    // buffer bindings, but only after VAO 0's own array specification is saved:
    // the overlay overwrites it, and the application may source from it later.
    if (custom_overlay_gl_pglBindVertexArray)
        custom_overlay_gl_pglBindVertexArray(0);
    const ce::gl_overlay_state::ClientArraySnapshot applicationArrays =
        ce::gl_overlay_state::CaptureClientArrays(stateApi, stateCaps);
    const GLboolean lastVertexArray = custom_overlay_gl_pglIsEnabled(GL_VERTEX_ARRAY);
    const GLboolean lastTexCoordArray = custom_overlay_gl_pglIsEnabled(GL_TEXTURE_COORD_ARRAY);
    const GLboolean lastColorArray = custom_overlay_gl_pglIsEnabled(GL_COLOR_ARRAY);
    GLint lastMatrixMode = GL_MODELVIEW;
    custom_overlay_gl_pglGetIntegerv(GL_MATRIX_MODE, &lastMatrixMode);
    if (custom_overlay_gl_pglBindBuffer) {
        custom_overlay_gl_pglBindBuffer(GL_ARRAY_BUFFER, 0);
        custom_overlay_gl_pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    ce::gl_overlay_state::PrepareOverlayDraw(stateApi, stateCaps, viewportWidth, viewportHeight);
    if (stateCaps.fixedFunction)
        custom_overlay_gl_pglEnable(GL_TEXTURE_2D);

    if (!legacyMatrixChecked) {
        ClearGLErrors();
        custom_overlay_gl_pglMatrixMode(GL_PROJECTION);
        const GLenum err = custom_overlay_gl_pglGetError ? custom_overlay_gl_pglGetError() : GL_NO_ERROR;
        legacyMatrixValid = (err == GL_NO_ERROR);
        if (!legacyMatrixValid) {
            HookLog("OpenGLBackend: Legacy using NDC transform (matrix err=0x%X)", err);
        }
        legacyMatrixChecked = true;
    }

    if (legacyMatrixValid) {
        custom_overlay_gl_pglMatrixMode(GL_PROJECTION);
        custom_overlay_gl_pglPushMatrix();
        custom_overlay_gl_pglLoadIdentity();
        custom_overlay_gl_pglOrtho(0, viewportWidth, viewportHeight, 0, -1, 1);

        custom_overlay_gl_pglMatrixMode(GL_MODELVIEW);
        custom_overlay_gl_pglPushMatrix();
        custom_overlay_gl_pglLoadIdentity();
    }

    const bool arrayFunctionsAvailable = custom_overlay_gl_pglEnableClientState && custom_overlay_gl_pglDisableClientState && custom_overlay_gl_pglVertexPointer &&
                                         custom_overlay_gl_pglTexCoordPointer && custom_overlay_gl_pglColorPointer && custom_overlay_gl_pglDrawElements;
    if (!legacyArrayChecked) {
        legacyArrayProbeSucceeded = false;
        if (legacyMatrixValid && arrayFunctionsAvailable) {
            const DrawVertex* vtx = vertices.data();
            ClearGLErrors();
            custom_overlay_gl_pglEnableClientState(GL_VERTEX_ARRAY);
            custom_overlay_gl_pglEnableClientState(GL_TEXTURE_COORD_ARRAY);
            custom_overlay_gl_pglEnableClientState(GL_COLOR_ARRAY);
            custom_overlay_gl_pglVertexPointer(2, GL_FLOAT, sizeof(DrawVertex), &vtx->x);
            custom_overlay_gl_pglTexCoordPointer(2, GL_FLOAT, sizeof(DrawVertex), &vtx->u);
            custom_overlay_gl_pglColorPointer(4, GL_UNSIGNED_BYTE, sizeof(DrawVertex), &vtx->color);
            custom_overlay_gl_pglDrawElements(GL_TRIANGLES, 0, GL_UNSIGNED_SHORT, indices.data());
            const GLenum arrayErr = custom_overlay_gl_pglGetError ? custom_overlay_gl_pglGetError() : GL_NO_ERROR;
            legacyArrayProbeSucceeded = (arrayErr == GL_NO_ERROR);
            if (!legacyArrayProbeSucceeded) {
                HookLog("OpenGLBackend: Legacy array preflight failed (err=0x%X); using immediate fallback", arrayErr);
            } else {
                HookLog("OpenGLBackend: Legacy using batched vertex arrays");
            }
        }
        if (!legacyArrayProbeSucceeded) {
            HookLog("OpenGLBackend: Legacy immediate-mode compatibility fallback enabled");
        }
        legacyArrayChecked = true;
    }

    const LegacyGLDrawPath drawPath =
        SelectLegacyGLDrawPath(legacyMatrixValid, arrayFunctionsAvailable, legacyArrayProbeSucceeded);
    if (drawPath == LegacyGLDrawPath::Arrays) {
        custom_overlay_gl_pglEnableClientState(GL_VERTEX_ARRAY);
        custom_overlay_gl_pglEnableClientState(GL_TEXTURE_COORD_ARRAY);
        custom_overlay_gl_pglEnableClientState(GL_COLOR_ARRAY);

        const DrawVertex* vtx = vertices.data();
        custom_overlay_gl_pglVertexPointer(2, GL_FLOAT, sizeof(DrawVertex), &vtx->x);
        custom_overlay_gl_pglTexCoordPointer(2, GL_FLOAT, sizeof(DrawVertex), &vtx->u);
        custom_overlay_gl_pglColorPointer(4, GL_UNSIGNED_BYTE, sizeof(DrawVertex), &vtx->color);

        for (const auto& cmd : commands) {
            custom_overlay_gl_pglBindTexture(GL_TEXTURE_2D, cmd.useTexture ? fontTextureId : 0);
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            custom_overlay_gl_pglDrawElements(GL_TRIANGLES, cmd.indexCount, GL_UNSIGNED_SHORT, indices.data() + cmd.indexOffset);
        }
    } else if (custom_overlay_gl_pglBegin && custom_overlay_gl_pglEnd && custom_overlay_gl_pglVertex2f && custom_overlay_gl_pglTexCoord2f && custom_overlay_gl_pglColor4ub) {
        for (const auto& cmd : commands) {
            custom_overlay_gl_pglBindTexture(GL_TEXTURE_2D, cmd.useTexture ? fontTextureId : 0);

            custom_overlay_gl_pglBegin(GL_TRIANGLES);
            for (uint32_t i = 0; i < cmd.indexCount; i++) {
                const uint16_t idx = indices[cmd.indexOffset + i];
                const DrawVertex& v = vertices[idx];
                const GLubyte r = (v.color >> 0) & 0xFF;
                const GLubyte g = (v.color >> 8) & 0xFF;
                const GLubyte b = (v.color >> 16) & 0xFF;
                const GLubyte a = (v.color >> 24) & 0xFF;

                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                const float vx = legacyMatrixValid ? v.x : (v.x / viewportWidth) * 2.0f - 1.0f;
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                const float vy = legacyMatrixValid ? v.y : 1.0f - (v.y / viewportHeight) * 2.0f;
                custom_overlay_gl_pglColor4ub(r, g, b, a);
                custom_overlay_gl_pglTexCoord2f(v.u, v.v);
                custom_overlay_gl_pglVertex2f(vx, vy);
            }
            custom_overlay_gl_pglEnd();
        }
    }

    if (legacyMatrixValid) {
        custom_overlay_gl_pglMatrixMode(GL_PROJECTION);
        custom_overlay_gl_pglPopMatrix();
        custom_overlay_gl_pglMatrixMode(GL_MODELVIEW);
        custom_overlay_gl_pglPopMatrix();
    }
    custom_overlay_gl_pglMatrixMode((GLenum)lastMatrixMode);

    // Still on VAO 0: its array specification and enables, then the rest.
    ce::gl_overlay_state::RestoreClientArrays(stateApi, stateCaps, applicationArrays);
    if (lastVertexArray)
        custom_overlay_gl_pglEnableClientState(GL_VERTEX_ARRAY);
    else
        custom_overlay_gl_pglDisableClientState(GL_VERTEX_ARRAY);
    if (lastTexCoordArray)
        custom_overlay_gl_pglEnableClientState(GL_TEXTURE_COORD_ARRAY);
    else
        custom_overlay_gl_pglDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (lastColorArray)
        custom_overlay_gl_pglEnableClientState(GL_COLOR_ARRAY);
    else
        custom_overlay_gl_pglDisableClientState(GL_COLOR_ARRAY);
    if (custom_overlay_gl_pglClientActiveTexture)
        custom_overlay_gl_pglClientActiveTexture((GLenum)lastClientActiveTexture);

    ce::gl_overlay_state::Restore(stateApi, stateCaps, applicationState);
}
}
