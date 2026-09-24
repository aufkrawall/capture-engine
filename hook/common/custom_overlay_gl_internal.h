#pragma once

#include "custom_overlay_gl.h"

#include <GL/gl.h>

#include "hook_common.h"

#pragma comment(lib, "opengl32.lib")

#ifndef GL_VERSION_3_0
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef char GLchar;
#define GL_COMPILE_STATUS 0x8B81
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_LINK_STATUS 0x8B82
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_ELEMENT_ARRAY_BUFFER_BINDING 0x8895
#define GL_CURRENT_PROGRAM 0x8B8D
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE0 0x84C0
#define GL_ACTIVE_TEXTURE 0x84E0
#define GL_CLIENT_ACTIVE_TEXTURE 0x84E1
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#endif

#ifndef GL_VERTEX_ARRAY_BINDING
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#endif

#ifndef GL_BLEND_SRC_RGB
#define GL_BLEND_SRC_RGB 0x80C9
#define GL_BLEND_DST_RGB 0x80C8
#define GL_BLEND_SRC_ALPHA 0x80CB
#define GL_BLEND_DST_ALPHA 0x80CA
#endif

typedef HGLRC(WINAPI* PFN_wglGetCurrentContext)(void);

typedef PROC(WINAPI* PFN_wglGetProcAddress)(LPCSTR);

typedef GLenum(APIENTRY* PFN_glGetError)(void);

typedef const GLubyte*(APIENTRY* PFN_glGetString)(GLenum name);

typedef void(APIENTRY* PFN_glGenTextures)(GLsizei, GLuint*);

typedef void(APIENTRY* PFN_glDeleteTextures)(GLsizei, const GLuint*);

typedef void(APIENTRY* PFN_glBindTexture)(GLenum, GLuint);

typedef void(APIENTRY* PFN_glTexParameteri)(GLenum, GLenum, GLint);

typedef void(APIENTRY* PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);

typedef void(APIENTRY* PFN_glGetIntegerv)(GLenum, GLint*);

typedef void(APIENTRY* PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);

typedef void(APIENTRY* PFN_glEnable)(GLenum);

typedef void(APIENTRY* PFN_glDisable)(GLenum);

typedef GLboolean(APIENTRY* PFN_glIsEnabled)(GLenum);

typedef void(APIENTRY* PFN_glBlendFunc)(GLenum, GLenum);

typedef void(APIENTRY* PFN_glBlendFuncSeparate)(GLenum, GLenum, GLenum, GLenum);

typedef void(APIENTRY* PFN_glMatrixMode)(GLenum);

typedef void(APIENTRY* PFN_glPushMatrix)(void);

typedef void(APIENTRY* PFN_glPopMatrix)(void);

typedef void(APIENTRY* PFN_glLoadIdentity)(void);

typedef void(APIENTRY* PFN_glOrtho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);

typedef void(APIENTRY* PFN_glEnableClientState)(GLenum);

typedef void(APIENTRY* PFN_glDisableClientState)(GLenum);

typedef void(APIENTRY* PFN_glVertexPointer)(GLint, GLenum, GLsizei, const void*);

typedef void(APIENTRY* PFN_glTexCoordPointer)(GLint, GLenum, GLsizei, const void*);

typedef void(APIENTRY* PFN_glColorPointer)(GLint, GLenum, GLsizei, const void*);

typedef void(APIENTRY* PFN_glDrawElements)(GLenum, GLsizei, GLenum, const void*);

typedef void(APIENTRY* PFN_glBegin)(GLenum);

typedef void(APIENTRY* PFN_glEnd)(void);

typedef void(APIENTRY* PFN_glVertex2f)(GLfloat, GLfloat);

typedef void(APIENTRY* PFN_glTexCoord2f)(GLfloat, GLfloat);

typedef void(APIENTRY* PFN_glColor4ub)(GLubyte, GLubyte, GLubyte, GLubyte);

typedef void(APIENTRY* PFN_glGenVertexArrays)(GLsizei, GLuint*);

typedef void(APIENTRY* PFN_glDeleteVertexArrays)(GLsizei, const GLuint*);

typedef void(APIENTRY* PFN_glBindVertexArray)(GLuint);

typedef void(APIENTRY* PFN_glGenBuffers)(GLsizei, GLuint*);

typedef void(APIENTRY* PFN_glDeleteBuffers)(GLsizei, const GLuint*);

typedef void(APIENTRY* PFN_glBindBuffer)(GLenum, GLuint);

typedef void(APIENTRY* PFN_glBufferData)(GLenum, GLsizeiptr, const void*, GLenum);

typedef void(APIENTRY* PFN_glBufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*);

typedef void(APIENTRY* PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);

typedef void(APIENTRY* PFN_glEnableVertexAttribArray)(GLuint);

typedef void(APIENTRY* PFN_glDisableVertexAttribArray)(GLuint);

typedef void(APIENTRY* PFN_glUseProgram)(GLuint);

typedef GLuint(APIENTRY* PFN_glCreateShader)(GLenum);

typedef void(APIENTRY* PFN_glDeleteShader)(GLuint);

typedef void(APIENTRY* PFN_glShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);

typedef void(APIENTRY* PFN_glCompileShader)(GLuint);

typedef void(APIENTRY* PFN_glGetShaderiv)(GLuint, GLenum, GLint*);

typedef void(APIENTRY* PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);

typedef GLuint(APIENTRY* PFN_glCreateProgram)(void);

typedef void(APIENTRY* PFN_glDeleteProgram)(GLuint);

typedef void(APIENTRY* PFN_glAttachShader)(GLuint, GLuint);

typedef void(APIENTRY* PFN_glLinkProgram)(GLuint);

typedef void(APIENTRY* PFN_glGetProgramiv)(GLuint, GLenum, GLint*);

typedef void(APIENTRY* PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);

typedef GLint(APIENTRY* PFN_glGetUniformLocation)(GLuint, const GLchar*);

typedef void(APIENTRY* PFN_glUniform2f)(GLint, GLfloat, GLfloat);

typedef void(APIENTRY* PFN_glUniform1i)(GLint, GLint);

typedef void(APIENTRY* PFN_glActiveTexture)(GLenum);

typedef void(APIENTRY* PFN_glClientActiveTexture)(GLenum);

inline PFN_wglGetCurrentContext custom_overlay_gl_pwglGetCurrentContext = nullptr;

inline PFN_glGetError custom_overlay_gl_pglGetError = nullptr;

inline PFN_glBindTexture custom_overlay_gl_pglBindTexture = nullptr;

inline PFN_glGetIntegerv custom_overlay_gl_pglGetIntegerv = nullptr;

inline PFN_glViewport custom_overlay_gl_pglViewport = nullptr;

inline PFN_glEnable custom_overlay_gl_pglEnable = nullptr;

inline PFN_glDisable custom_overlay_gl_pglDisable = nullptr;

inline PFN_glIsEnabled custom_overlay_gl_pglIsEnabled = nullptr;

inline PFN_glBlendFunc custom_overlay_gl_pglBlendFunc = nullptr;

inline PFN_glBlendFuncSeparate custom_overlay_gl_pglBlendFuncSeparate = nullptr;

inline PFN_glMatrixMode custom_overlay_gl_pglMatrixMode = nullptr;

inline PFN_glPushMatrix custom_overlay_gl_pglPushMatrix = nullptr;

inline PFN_glPopMatrix custom_overlay_gl_pglPopMatrix = nullptr;

inline PFN_glLoadIdentity custom_overlay_gl_pglLoadIdentity = nullptr;

inline PFN_glOrtho custom_overlay_gl_pglOrtho = nullptr;

inline PFN_glEnableClientState custom_overlay_gl_pglEnableClientState = nullptr;

inline PFN_glDisableClientState custom_overlay_gl_pglDisableClientState = nullptr;

inline PFN_glVertexPointer custom_overlay_gl_pglVertexPointer = nullptr;

inline PFN_glTexCoordPointer custom_overlay_gl_pglTexCoordPointer = nullptr;

inline PFN_glColorPointer custom_overlay_gl_pglColorPointer = nullptr;

inline PFN_glDrawElements custom_overlay_gl_pglDrawElements = nullptr;

inline PFN_glBegin custom_overlay_gl_pglBegin = nullptr;

inline PFN_glEnd custom_overlay_gl_pglEnd = nullptr;

inline PFN_glVertex2f custom_overlay_gl_pglVertex2f = nullptr;

inline PFN_glTexCoord2f custom_overlay_gl_pglTexCoord2f = nullptr;

inline PFN_glColor4ub custom_overlay_gl_pglColor4ub = nullptr;

inline PFN_glBindVertexArray custom_overlay_gl_pglBindVertexArray = nullptr;

inline PFN_glBindBuffer custom_overlay_gl_pglBindBuffer = nullptr;

inline PFN_glActiveTexture custom_overlay_gl_pglActiveTexture = nullptr;

inline PFN_glClientActiveTexture custom_overlay_gl_pglClientActiveTexture = nullptr;

typedef void(APIENTRY* PFN_glBlendEquationSeparate)(GLenum, GLenum);

typedef void(APIENTRY* PFN_glColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);

typedef void(APIENTRY* PFN_glPolygonMode)(GLenum, GLenum);

typedef void(APIENTRY* PFN_glBindSampler)(GLuint, GLuint);

typedef void(APIENTRY* PFN_glPixelStorei)(GLenum, GLint);

typedef void(APIENTRY* PFN_glGetPointerv)(GLenum, void**);

// Optional entry points used only to save and restore application state.
inline PFN_glBlendEquationSeparate custom_overlay_gl_pglBlendEquationSeparate = nullptr;

inline PFN_glColorMask custom_overlay_gl_pglColorMask = nullptr;

inline PFN_glPolygonMode custom_overlay_gl_pglPolygonMode = nullptr;

inline PFN_glBindSampler custom_overlay_gl_pglBindSampler = nullptr;

inline PFN_glPixelStorei custom_overlay_gl_pglPixelStorei = nullptr;

inline PFN_glGetPointerv custom_overlay_gl_pglGetPointerv = nullptr;

inline PFN_glUseProgram custom_overlay_gl_pglUseProgramState = nullptr;

// ce::gl_overlay_state adapter over the resolved entry points. Callers pass
// Capabilities that only name entry points which resolved.
struct GLHookStateApi {
    void GetInteger(uint32_t pname, int32_t* values) {
        custom_overlay_gl_pglGetIntegerv(static_cast<GLenum>(pname), reinterpret_cast<GLint*>(values));
    }
    bool IsEnabled(uint32_t cap) {
        return custom_overlay_gl_pglIsEnabled(static_cast<GLenum>(cap)) != GL_FALSE;
    }
    void SetEnabled(uint32_t cap, bool enabled) {
        if (enabled)
            custom_overlay_gl_pglEnable(static_cast<GLenum>(cap));
        else
            custom_overlay_gl_pglDisable(static_cast<GLenum>(cap));
    }
    void BlendFunc(uint32_t src, uint32_t dst) {
        custom_overlay_gl_pglBlendFunc(static_cast<GLenum>(src), static_cast<GLenum>(dst));
    }
    void BlendFuncSeparate(uint32_t srcRgb, uint32_t dstRgb, uint32_t srcAlpha, uint32_t dstAlpha) {
        custom_overlay_gl_pglBlendFuncSeparate(static_cast<GLenum>(srcRgb), static_cast<GLenum>(dstRgb),
                                               static_cast<GLenum>(srcAlpha), static_cast<GLenum>(dstAlpha));
    }
    void BlendEquationSeparate(uint32_t rgb, uint32_t alpha) {
        custom_overlay_gl_pglBlendEquationSeparate(static_cast<GLenum>(rgb), static_cast<GLenum>(alpha));
    }
    void ColorMask(bool r, bool g, bool b, bool a) {
        if (custom_overlay_gl_pglColorMask)
            custom_overlay_gl_pglColorMask(r ? GL_TRUE : GL_FALSE, g ? GL_TRUE : GL_FALSE, b ? GL_TRUE : GL_FALSE,
                                           a ? GL_TRUE : GL_FALSE);
    }
    void PolygonMode(uint32_t face, uint32_t mode) {
        custom_overlay_gl_pglPolygonMode(static_cast<GLenum>(face), static_cast<GLenum>(mode));
    }
    void ActiveTexture(uint32_t unit) {
        custom_overlay_gl_pglActiveTexture(static_cast<GLenum>(unit));
    }
    void BindTexture2D(uint32_t texture) {
        custom_overlay_gl_pglBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
    }
    void BindSampler(uint32_t unit, uint32_t sampler) {
        custom_overlay_gl_pglBindSampler(static_cast<GLuint>(unit), static_cast<GLuint>(sampler));
    }
    void UseProgram(uint32_t program) {
        custom_overlay_gl_pglUseProgramState(static_cast<GLuint>(program));
    }
    void BindVertexArray(uint32_t vao) {
        custom_overlay_gl_pglBindVertexArray(static_cast<GLuint>(vao));
    }
    void BindBuffer(uint32_t target, uint32_t buffer) {
        custom_overlay_gl_pglBindBuffer(static_cast<GLenum>(target), static_cast<GLuint>(buffer));
    }
    void PixelStore(uint32_t pname, int32_t value) {
        if (custom_overlay_gl_pglPixelStorei)
            custom_overlay_gl_pglPixelStorei(static_cast<GLenum>(pname), value);
    }
    void Viewport(int32_t x, int32_t y, int32_t width, int32_t height) {
        custom_overlay_gl_pglViewport(x, y, width, height);
    }
    const void* GetPointer(uint32_t pname) {
        void* pointer = nullptr;
        if (custom_overlay_gl_pglGetPointerv)
            custom_overlay_gl_pglGetPointerv(static_cast<GLenum>(pname), &pointer);
        return pointer;
    }
    void VertexPointer(int32_t size, uint32_t type, int32_t stride, const void* pointer) {
        custom_overlay_gl_pglVertexPointer(size, static_cast<GLenum>(type), stride, pointer);
    }
    void ColorPointer(int32_t size, uint32_t type, int32_t stride, const void* pointer) {
        custom_overlay_gl_pglColorPointer(size, static_cast<GLenum>(type), stride, pointer);
    }
    void TexCoordPointer(int32_t size, uint32_t type, int32_t stride, const void* pointer) {
        custom_overlay_gl_pglTexCoordPointer(size, static_cast<GLenum>(type), stride, pointer);
    }
};

inline void ClearGLErrors() {
    if (custom_overlay_gl_pglGetError) {
        while (custom_overlay_gl_pglGetError() != GL_NO_ERROR) {}
    }
}
