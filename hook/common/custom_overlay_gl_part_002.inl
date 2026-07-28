    GLint lastViewport[4] = {0};
    pglGetIntegerv(GL_VIEWPORT, lastViewport);
    GLboolean lastBlend = pglIsEnabled(GL_BLEND);
    GLint lastProgram = 0;
    pglGetIntegerv(GL_CURRENT_PROGRAM, &lastProgram);
    GLint lastVAO = 0;
    pglGetIntegerv(0x85B5, &lastVAO);
    GLint lastVBO = 0;
    pglGetIntegerv(GL_ARRAY_BUFFER_BINDING, &lastVBO);
    GLint lastIBO = 0;
    pglGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &lastIBO);

    pglEnable(GL_BLEND);
    pglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    pglDisable(GL_DEPTH_TEST);
    pglDisable(GL_CULL_FACE);

    pglViewport(0, 0, viewportWidth, viewportHeight);

    pglBindVertexArray(modern.vao);
    pglBindBuffer(GL_ARRAY_BUFFER, modern.vbo);
    pglBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(DrawVertex), vertices.data(), GL_DYNAMIC_DRAW);

    pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, modern.ibo);
    pglBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_DYNAMIC_DRAW);

    for (const auto& cmd : commands) {
        GLuint program = cmd.useTexture ? modern.programTextured : modern.programSolid;
        pglUseProgram(program);

        if (cmd.useTexture) {
            pglUniform2f(modern.uViewportTextured, (float)viewportWidth, (float)viewportHeight);
            if (pglActiveTexture)
                pglActiveTexture(GL_TEXTURE0);
            pglBindTexture(GL_TEXTURE_2D, fontTextureId);
            pglUniform1i(modern.uTexture, 0);
        } else {
            pglUniform2f(modern.uViewportSolid, (float)viewportWidth, (float)viewportHeight);
            pglBindTexture(GL_TEXTURE_2D, 0);
        }

        pglDrawElements(GL_TRIANGLES, cmd.indexCount, GL_UNSIGNED_SHORT,
                        (const void*)(cmd.indexOffset * sizeof(uint16_t)));
    }

    pglBindVertexArray(0);
    pglUseProgram(0);

    pglBindTexture(GL_TEXTURE_2D, lastTexture);
    pglViewport(lastViewport[0], lastViewport[1], lastViewport[2], lastViewport[3]);

    if (lastBlend)
        pglEnable(GL_BLEND);
    else
        pglDisable(GL_BLEND);

    if (lastVAO && pglBindVertexArray)
        pglBindVertexArray(lastVAO);
    if (lastVBO)
        pglBindBuffer(GL_ARRAY_BUFFER, lastVBO);
    if (lastIBO)
        pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lastIBO);
    if (lastProgram)
        pglUseProgram(lastProgram);
}

void OpenGLBackend::RenderLegacy(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                                 const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) {
    HGLRC currentContext = pwglGetCurrentContext ? pwglGetCurrentContext() : nullptr;
    if (currentContext != legacyProbeContext) {
        legacyProbeContext = currentContext;
        legacyMatrixChecked = false;
        legacyMatrixValid = true;
        legacyArrayChecked = false;
        legacyArrayProbeSucceeded = false;
    }

    GLint lastViewport[4] = {0};
    pglGetIntegerv(GL_VIEWPORT, lastViewport);
    const GLboolean lastBlend = pglIsEnabled(GL_BLEND);
    const GLboolean lastDepthTest = pglIsEnabled(GL_DEPTH_TEST);
    const GLboolean lastCullFace = pglIsEnabled(GL_CULL_FACE);
    GLint lastBlendSrcRGB = GL_ONE;
    GLint lastBlendDstRGB = GL_ZERO;
    GLint lastBlendSrcAlpha = GL_ONE;
    GLint lastBlendDstAlpha = GL_ZERO;
    pglGetIntegerv(GL_BLEND_SRC, &lastBlendSrcRGB);
    pglGetIntegerv(GL_BLEND_DST, &lastBlendDstRGB);
    if (pglBlendFuncSeparate) {
        pglGetIntegerv(GL_BLEND_SRC_ALPHA, &lastBlendSrcAlpha);
        pglGetIntegerv(GL_BLEND_DST_ALPHA, &lastBlendDstAlpha);
    } else {
        lastBlendSrcAlpha = lastBlendSrcRGB;
        lastBlendDstAlpha = lastBlendDstRGB;
    }

    GLint lastActiveTexture = GL_TEXTURE0;
    if (pglActiveTexture) {
        pglGetIntegerv(GL_ACTIVE_TEXTURE, &lastActiveTexture);
        pglActiveTexture(GL_TEXTURE0);
    }
    GLint lastClientActiveTexture = GL_TEXTURE0;
    if (pglClientActiveTexture) {
        pglGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &lastClientActiveTexture);
        pglClientActiveTexture(GL_TEXTURE0);
    }
    GLint lastTexture = 0;
    pglGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);
    const GLboolean lastTexture2D = pglIsEnabled(GL_TEXTURE_2D);

    GLint lastVAO = 0;
    if (pglBindVertexArray)
        pglGetIntegerv(GL_VERTEX_ARRAY_BINDING, &lastVAO);
    GLint lastVBO = 0;
    GLint lastIBO = 0;
    if (pglBindBuffer) {
        pglGetIntegerv(GL_ARRAY_BUFFER_BINDING, &lastVBO);
        pglGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &lastIBO);
    }

    const GLboolean lastVertexArray = pglIsEnabled(GL_VERTEX_ARRAY);
    const GLboolean lastTexCoordArray = pglIsEnabled(GL_TEXTURE_COORD_ARRAY);
    const GLboolean lastColorArray = pglIsEnabled(GL_COLOR_ARRAY);
    GLint lastMatrixMode = GL_MODELVIEW;
    pglGetIntegerv(GL_MATRIX_MODE, &lastMatrixMode);

    // Client pointers below address CPU memory. Move to VAO 0 and explicitly
    // clear buffer bindings only after every original binding/enable was saved.
    if (pglBindVertexArray)
        pglBindVertexArray(0);
    if (pglBindBuffer) {
        pglBindBuffer(GL_ARRAY_BUFFER, 0);
        pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }

    pglEnable(GL_BLEND);
    pglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    pglDisable(GL_DEPTH_TEST);
    pglDisable(GL_CULL_FACE);
    pglEnable(GL_TEXTURE_2D);

    pglViewport(0, 0, viewportWidth, viewportHeight);

    if (!legacyMatrixChecked) {
        ClearGLErrors();
        pglMatrixMode(GL_PROJECTION);
        const GLenum err = pglGetError ? pglGetError() : GL_NO_ERROR;
        legacyMatrixValid = (err == GL_NO_ERROR);
        if (!legacyMatrixValid) {
            HookLog("OpenGLBackend: Legacy using NDC transform (matrix err=0x%X)", err);
        }
        legacyMatrixChecked = true;
    }

    if (legacyMatrixValid) {
        pglMatrixMode(GL_PROJECTION);
        pglPushMatrix();
        pglLoadIdentity();
        pglOrtho(0, viewportWidth, viewportHeight, 0, -1, 1);

        pglMatrixMode(GL_MODELVIEW);
        pglPushMatrix();
        pglLoadIdentity();
    }

    const bool arrayFunctionsAvailable = pglEnableClientState && pglDisableClientState && pglVertexPointer &&
                                         pglTexCoordPointer && pglColorPointer && pglDrawElements;
    if (!legacyArrayChecked) {
        legacyArrayProbeSucceeded = false;
        if (legacyMatrixValid && arrayFunctionsAvailable) {
            const DrawVertex* vtx = vertices.data();
            ClearGLErrors();
            pglEnableClientState(GL_VERTEX_ARRAY);
            pglEnableClientState(GL_TEXTURE_COORD_ARRAY);
            pglEnableClientState(GL_COLOR_ARRAY);
            pglVertexPointer(2, GL_FLOAT, sizeof(DrawVertex), &vtx->x);
            pglTexCoordPointer(2, GL_FLOAT, sizeof(DrawVertex), &vtx->u);
            pglColorPointer(4, GL_UNSIGNED_BYTE, sizeof(DrawVertex), &vtx->color);
            pglDrawElements(GL_TRIANGLES, 0, GL_UNSIGNED_SHORT, indices.data());
            const GLenum arrayErr = pglGetError ? pglGetError() : GL_NO_ERROR;
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
        pglEnableClientState(GL_VERTEX_ARRAY);
        pglEnableClientState(GL_TEXTURE_COORD_ARRAY);
        pglEnableClientState(GL_COLOR_ARRAY);

        const DrawVertex* vtx = vertices.data();
        pglVertexPointer(2, GL_FLOAT, sizeof(DrawVertex), &vtx->x);
        pglTexCoordPointer(2, GL_FLOAT, sizeof(DrawVertex), &vtx->u);
        pglColorPointer(4, GL_UNSIGNED_BYTE, sizeof(DrawVertex), &vtx->color);

        for (const auto& cmd : commands) {
            pglBindTexture(GL_TEXTURE_2D, cmd.useTexture ? fontTextureId : 0);
            pglDrawElements(GL_TRIANGLES, cmd.indexCount, GL_UNSIGNED_SHORT, indices.data() + cmd.indexOffset);
        }
    } else if (pglBegin && pglEnd && pglVertex2f && pglTexCoord2f && pglColor4ub) {
        for (const auto& cmd : commands) {
            pglBindTexture(GL_TEXTURE_2D, cmd.useTexture ? fontTextureId : 0);

            pglBegin(GL_TRIANGLES);
            for (uint32_t i = 0; i < cmd.indexCount; i++) {
                const uint16_t idx = indices[cmd.indexOffset + i];
                const DrawVertex& v = vertices[idx];
                const GLubyte r = (v.color >> 0) & 0xFF;
                const GLubyte g = (v.color >> 8) & 0xFF;
                const GLubyte b = (v.color >> 16) & 0xFF;
                const GLubyte a = (v.color >> 24) & 0xFF;

                const float vx = legacyMatrixValid ? v.x : (v.x / viewportWidth) * 2.0f - 1.0f;
                const float vy = legacyMatrixValid ? v.y : 1.0f - (v.y / viewportHeight) * 2.0f;
                pglColor4ub(r, g, b, a);
                pglTexCoord2f(v.u, v.v);
                pglVertex2f(vx, vy);
            }
            pglEnd();
        }
    }

    if (legacyMatrixValid) {
        pglMatrixMode(GL_PROJECTION);
        pglPopMatrix();
        pglMatrixMode(GL_MODELVIEW);
        pglPopMatrix();
    }
    pglMatrixMode((GLenum)lastMatrixMode);

    if (pglBindVertexArray)
        pglBindVertexArray((GLuint)lastVAO);
    if (pglBindBuffer) {
        pglBindBuffer(GL_ARRAY_BUFFER, (GLuint)lastVBO);
        pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, (GLuint)lastIBO);
    }

    if (lastVertexArray)
        pglEnableClientState(GL_VERTEX_ARRAY);
    else
        pglDisableClientState(GL_VERTEX_ARRAY);
    if (lastTexCoordArray)
        pglEnableClientState(GL_TEXTURE_COORD_ARRAY);
    else
        pglDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (lastColorArray)
        pglEnableClientState(GL_COLOR_ARRAY);
    else
        pglDisableClientState(GL_COLOR_ARRAY);

    pglBindTexture(GL_TEXTURE_2D, (GLuint)lastTexture);
    if (lastTexture2D)
        pglEnable(GL_TEXTURE_2D);
    else
        pglDisable(GL_TEXTURE_2D);
    if (pglClientActiveTexture)
        pglClientActiveTexture((GLenum)lastClientActiveTexture);
    if (pglActiveTexture)
        pglActiveTexture((GLenum)lastActiveTexture);

    pglViewport(lastViewport[0], lastViewport[1], lastViewport[2], lastViewport[3]);
    if (pglBlendFuncSeparate) {
        pglBlendFuncSeparate((GLenum)lastBlendSrcRGB, (GLenum)lastBlendDstRGB, (GLenum)lastBlendSrcAlpha,
                             (GLenum)lastBlendDstAlpha);
    } else {
        pglBlendFunc((GLenum)lastBlendSrcRGB, (GLenum)lastBlendDstRGB);
    }
    if (lastBlend)
        pglEnable(GL_BLEND);
    else
        pglDisable(GL_BLEND);
    if (lastDepthTest)
        pglEnable(GL_DEPTH_TEST);
    else
        pglDisable(GL_DEPTH_TEST);
    if (lastCullFace)
        pglEnable(GL_CULL_FACE);
    else
        pglDisable(GL_CULL_FACE);
}

}  // namespace CustomOverlay
