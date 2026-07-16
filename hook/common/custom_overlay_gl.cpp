/**
 * Custom Overlay - OpenGL Backend Implementation
 *
 * Hybrid implementation:
 * - Modern path (GL 3.0+): Uses GLSL 330 shaders, VAO, VBO
 * - Legacy path (GL < 3.0): Fixed-function pipeline
 */

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

namespace {

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

static PFN_wglGetCurrentContext pwglGetCurrentContext = nullptr;
static PFN_wglGetProcAddress pwglGetProcAddress = nullptr;
static PFN_glGetError pglGetError = nullptr;
static PFN_glGetString pglGetString = nullptr;

static PFN_glGenTextures pglGenTextures = nullptr;
static PFN_glDeleteTextures pglDeleteTextures = nullptr;
static PFN_glBindTexture pglBindTexture = nullptr;
static PFN_glTexParameteri pglTexParameteri = nullptr;
static PFN_glTexImage2D pglTexImage2D = nullptr;
static PFN_glGetIntegerv pglGetIntegerv = nullptr;
static PFN_glViewport pglViewport = nullptr;
static PFN_glEnable pglEnable = nullptr;
static PFN_glDisable pglDisable = nullptr;
static PFN_glIsEnabled pglIsEnabled = nullptr;
static PFN_glBlendFunc pglBlendFunc = nullptr;
static PFN_glBlendFuncSeparate pglBlendFuncSeparate = nullptr;
static PFN_glMatrixMode pglMatrixMode = nullptr;
static PFN_glPushMatrix pglPushMatrix = nullptr;
static PFN_glPopMatrix pglPopMatrix = nullptr;
static PFN_glLoadIdentity pglLoadIdentity = nullptr;
static PFN_glOrtho pglOrtho = nullptr;
static PFN_glEnableClientState pglEnableClientState = nullptr;
static PFN_glDisableClientState pglDisableClientState = nullptr;
static PFN_glVertexPointer pglVertexPointer = nullptr;
static PFN_glTexCoordPointer pglTexCoordPointer = nullptr;
static PFN_glColorPointer pglColorPointer = nullptr;
static PFN_glDrawElements pglDrawElements = nullptr;
static PFN_glBegin pglBegin = nullptr;
static PFN_glEnd pglEnd = nullptr;
static PFN_glVertex2f pglVertex2f = nullptr;
static PFN_glTexCoord2f pglTexCoord2f = nullptr;
static PFN_glColor4ub pglColor4ub = nullptr;

static PFN_glGenVertexArrays pglGenVertexArrays = nullptr;
static PFN_glDeleteVertexArrays pglDeleteVertexArrays = nullptr;
static PFN_glBindVertexArray pglBindVertexArray = nullptr;
static PFN_glGenBuffers pglGenBuffers = nullptr;
static PFN_glDeleteBuffers pglDeleteBuffers = nullptr;
static PFN_glBindBuffer pglBindBuffer = nullptr;
static PFN_glBufferData pglBufferData = nullptr;
static PFN_glBufferSubData pglBufferSubData = nullptr;
static PFN_glVertexAttribPointer pglVertexAttribPointer = nullptr;
static PFN_glEnableVertexAttribArray pglEnableVertexAttribArray = nullptr;
static PFN_glDisableVertexAttribArray pglDisableVertexAttribArray = nullptr;
static PFN_glUseProgram pglUseProgram = nullptr;
static PFN_glCreateShader pglCreateShader = nullptr;
static PFN_glDeleteShader pglDeleteShader = nullptr;
static PFN_glShaderSource pglShaderSource = nullptr;
static PFN_glCompileShader pglCompileShader = nullptr;
static PFN_glGetShaderiv pglGetShaderiv = nullptr;
static PFN_glGetShaderInfoLog pglGetShaderInfoLog = nullptr;
static PFN_glCreateProgram pglCreateProgram = nullptr;
static PFN_glDeleteProgram pglDeleteProgram = nullptr;
static PFN_glAttachShader pglAttachShader = nullptr;
static PFN_glLinkProgram pglLinkProgram = nullptr;
static PFN_glGetProgramiv pglGetProgramiv = nullptr;
static PFN_glGetProgramInfoLog pglGetProgramInfoLog = nullptr;
static PFN_glGetUniformLocation pglGetUniformLocation = nullptr;
static PFN_glUniform2f pglUniform2f = nullptr;
static PFN_glUniform1i pglUniform1i = nullptr;
static PFN_glActiveTexture pglActiveTexture = nullptr;
static PFN_glClientActiveTexture pglClientActiveTexture = nullptr;

static bool g_GLFunctionsLoaded = false;
static bool g_GLModernFunctionsLoaded = false;

static PROC GetOptionalGLProc(const char* coreName, const char* extensionName = nullptr) {
    PROC proc = pwglGetProcAddress ? pwglGetProcAddress(coreName) : nullptr;
    if ((!proc || proc == (PROC)1 || proc == (PROC)2 || proc == (PROC)3 || proc == (PROC)-1) && extensionName) {
        proc = pwglGetProcAddress ? pwglGetProcAddress(extensionName) : nullptr;
    }
    if (proc == (PROC)1 || proc == (PROC)2 || proc == (PROC)3 || proc == (PROC)-1)
        return nullptr;
    return proc;
}

static bool LoadGLFunctions() {
    if (g_GLFunctionsLoaded)
        return true;

    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl)
        return false;

    pwglGetCurrentContext = (PFN_wglGetCurrentContext)GetProcAddress(gl, "wglGetCurrentContext");
    pwglGetProcAddress = (PFN_wglGetProcAddress)GetProcAddress(gl, "wglGetProcAddress");
    pglGetError = (PFN_glGetError)GetProcAddress(gl, "glGetError");
    pglGetString = (PFN_glGetString)GetProcAddress(gl, "glGetString");

    pglGenTextures = (PFN_glGenTextures)GetProcAddress(gl, "glGenTextures");
    pglDeleteTextures = (PFN_glDeleteTextures)GetProcAddress(gl, "glDeleteTextures");
    pglBindTexture = (PFN_glBindTexture)GetProcAddress(gl, "glBindTexture");
    pglTexParameteri = (PFN_glTexParameteri)GetProcAddress(gl, "glTexParameteri");
    pglTexImage2D = (PFN_glTexImage2D)GetProcAddress(gl, "glTexImage2D");
    pglGetIntegerv = (PFN_glGetIntegerv)GetProcAddress(gl, "glGetIntegerv");
    pglViewport = (PFN_glViewport)GetProcAddress(gl, "glViewport");
    pglEnable = (PFN_glEnable)GetProcAddress(gl, "glEnable");
    pglDisable = (PFN_glDisable)GetProcAddress(gl, "glDisable");
    pglIsEnabled = (PFN_glIsEnabled)GetProcAddress(gl, "glIsEnabled");
    pglBlendFunc = (PFN_glBlendFunc)GetProcAddress(gl, "glBlendFunc");
    pglMatrixMode = (PFN_glMatrixMode)GetProcAddress(gl, "glMatrixMode");
    pglPushMatrix = (PFN_glPushMatrix)GetProcAddress(gl, "glPushMatrix");
    pglPopMatrix = (PFN_glPopMatrix)GetProcAddress(gl, "glPopMatrix");
    pglLoadIdentity = (PFN_glLoadIdentity)GetProcAddress(gl, "glLoadIdentity");
    pglOrtho = (PFN_glOrtho)GetProcAddress(gl, "glOrtho");
    pglEnableClientState = (PFN_glEnableClientState)GetProcAddress(gl, "glEnableClientState");
    pglDisableClientState = (PFN_glDisableClientState)GetProcAddress(gl, "glDisableClientState");
    pglVertexPointer = (PFN_glVertexPointer)GetProcAddress(gl, "glVertexPointer");
    pglTexCoordPointer = (PFN_glTexCoordPointer)GetProcAddress(gl, "glTexCoordPointer");
    pglColorPointer = (PFN_glColorPointer)GetProcAddress(gl, "glColorPointer");
    pglDrawElements = (PFN_glDrawElements)GetProcAddress(gl, "glDrawElements");
    pglBegin = (PFN_glBegin)GetProcAddress(gl, "glBegin");
    pglEnd = (PFN_glEnd)GetProcAddress(gl, "glEnd");
    pglVertex2f = (PFN_glVertex2f)GetProcAddress(gl, "glVertex2f");
    pglTexCoord2f = (PFN_glTexCoord2f)GetProcAddress(gl, "glTexCoord2f");
    pglColor4ub = (PFN_glColor4ub)GetProcAddress(gl, "glColor4ub");

    if (!pglGenTextures || !pglDeleteTextures || !pglBindTexture || !pglTexImage2D || !pglTexParameteri ||
        !pglGetIntegerv || !pglViewport || !pglEnable || !pglDisable || !pglIsEnabled || !pglBlendFunc ||
        !pglMatrixMode || !pglPushMatrix || !pglPopMatrix || !pglLoadIdentity || !pglOrtho || !pglEnableClientState ||
        !pglDisableClientState || !pglVertexPointer || !pglTexCoordPointer || !pglColorPointer || !pglDrawElements ||
        !pglBegin || !pglEnd || !pglVertex2f || !pglTexCoord2f || !pglColor4ub || !pwglGetCurrentContext ||
        !pwglGetProcAddress || !pglGetError) {
        return false;
    }

    g_GLFunctionsLoaded = true;
    return true;
}

static void LoadGLLegacyOptionalFunctions() {
    if (!pglBlendFuncSeparate) {
        pglBlendFuncSeparate =
            (PFN_glBlendFuncSeparate)GetOptionalGLProc("glBlendFuncSeparate", "glBlendFuncSeparateEXT");
    }
    if (!pglBindBuffer)
        pglBindBuffer = (PFN_glBindBuffer)GetOptionalGLProc("glBindBuffer", "glBindBufferARB");
    if (!pglBindVertexArray)
        pglBindVertexArray = (PFN_glBindVertexArray)GetOptionalGLProc("glBindVertexArray", "glBindVertexArrayAPPLE");
    if (!pglActiveTexture)
        pglActiveTexture = (PFN_glActiveTexture)GetOptionalGLProc("glActiveTexture", "glActiveTextureARB");
    if (!pglClientActiveTexture) {
        pglClientActiveTexture =
            (PFN_glClientActiveTexture)GetOptionalGLProc("glClientActiveTexture", "glClientActiveTextureARB");
    }
}

static bool LoadGLModernFunctions() {
    if (g_GLModernFunctionsLoaded)
        return true;

    if (!pwglGetProcAddress)
        return false;

    pglGenVertexArrays = (PFN_glGenVertexArrays)pwglGetProcAddress("glGenVertexArrays");
    pglDeleteVertexArrays = (PFN_glDeleteVertexArrays)pwglGetProcAddress("glDeleteVertexArrays");
    pglBindVertexArray = (PFN_glBindVertexArray)pwglGetProcAddress("glBindVertexArray");
    pglGenBuffers = (PFN_glGenBuffers)pwglGetProcAddress("glGenBuffers");
    pglDeleteBuffers = (PFN_glDeleteBuffers)pwglGetProcAddress("glDeleteBuffers");
    pglBindBuffer = (PFN_glBindBuffer)pwglGetProcAddress("glBindBuffer");
    pglBufferData = (PFN_glBufferData)pwglGetProcAddress("glBufferData");
    pglBufferSubData = (PFN_glBufferSubData)pwglGetProcAddress("glBufferSubData");
    pglVertexAttribPointer = (PFN_glVertexAttribPointer)pwglGetProcAddress("glVertexAttribPointer");
    pglEnableVertexAttribArray = (PFN_glEnableVertexAttribArray)pwglGetProcAddress("glEnableVertexAttribArray");
    pglDisableVertexAttribArray = (PFN_glDisableVertexAttribArray)pwglGetProcAddress("glDisableVertexAttribArray");
    pglUseProgram = (PFN_glUseProgram)pwglGetProcAddress("glUseProgram");
    pglCreateShader = (PFN_glCreateShader)pwglGetProcAddress("glCreateShader");
    pglDeleteShader = (PFN_glDeleteShader)pwglGetProcAddress("glDeleteShader");
    pglShaderSource = (PFN_glShaderSource)pwglGetProcAddress("glShaderSource");
    pglCompileShader = (PFN_glCompileShader)pwglGetProcAddress("glCompileShader");
    pglGetShaderiv = (PFN_glGetShaderiv)pwglGetProcAddress("glGetShaderiv");
    pglGetShaderInfoLog = (PFN_glGetShaderInfoLog)pwglGetProcAddress("glGetShaderInfoLog");
    pglCreateProgram = (PFN_glCreateProgram)pwglGetProcAddress("glCreateProgram");
    pglDeleteProgram = (PFN_glDeleteProgram)pwglGetProcAddress("glDeleteProgram");
    pglAttachShader = (PFN_glAttachShader)pwglGetProcAddress("glAttachShader");
    pglLinkProgram = (PFN_glLinkProgram)pwglGetProcAddress("glLinkProgram");
    pglGetProgramiv = (PFN_glGetProgramiv)pwglGetProcAddress("glGetProgramiv");
    pglGetProgramInfoLog = (PFN_glGetProgramInfoLog)pwglGetProcAddress("glGetProgramInfoLog");
    pglGetUniformLocation = (PFN_glGetUniformLocation)pwglGetProcAddress("glGetUniformLocation");
    pglUniform2f = (PFN_glUniform2f)pwglGetProcAddress("glUniform2f");
    pglUniform1i = (PFN_glUniform1i)pwglGetProcAddress("glUniform1i");
    pglActiveTexture = (PFN_glActiveTexture)pwglGetProcAddress("glActiveTexture");

    if (!pglGenVertexArrays || !pglBindVertexArray || !pglGenBuffers || !pglBindBuffer || !pglBufferData ||
        !pglUseProgram || !pglCreateShader || !pglShaderSource || !pglCompileShader || !pglCreateProgram ||
        !pglLinkProgram || !pglGetUniformLocation) {
        HookLog("OpenGLBackend: Missing GL 3.0+ functions");
        return false;
    }

    g_GLModernFunctionsLoaded = true;
    return true;
}

static void ClearGLErrors() {
    if (pglGetError) {
        while (pglGetError() != GL_NO_ERROR) {}
    }
}

constexpr const char* GL_VS_SOURCE = R"GLSL(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aColor;

uniform vec2 uViewport;
out vec2 vTexCoord;
out vec4 vColor;

void main() {
    vec2 ndc = vec2(
        (aPos.x / uViewport.x) * 2.0 - 1.0,
        1.0 - (aPos.y / uViewport.y) * 2.0
    );
    gl_Position = vec4(ndc, 0.0, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)GLSL";

constexpr const char* GL_FS_TEXTURED = R"GLSL(
#version 330 core
in vec2 vTexCoord;
in vec4 vColor;
out vec4 FragColor;
uniform sampler2D uTexture;

void main() {
    FragColor = vColor * texture(uTexture, vTexCoord);
}
)GLSL";

constexpr const char* GL_FS_SOLID = R"GLSL(
#version 330 core
in vec2 vTexCoord;
in vec4 vColor;
out vec4 FragColor;

void main() {
    FragColor = vColor;
}
)GLSL";

}  // namespace

namespace CustomOverlay {

OpenGLBackend::OpenGLBackend() {}

OpenGLBackend::~OpenGLBackend() {
    Shutdown();
}

GLuint OpenGLBackend::CompileShader(GLenum type, const char* source) {
    GLuint shader = pglCreateShader(type);
    if (!shader) {
        HookLog("OpenGLBackend: Failed to create shader");
        return 0;
    }

    pglShaderSource(shader, 1, &source, nullptr);
    pglCompileShader(shader);

    GLint success = 0;
    pglGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint logLen = 0;
        pglGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 0) {
            char* log = new char[logLen];
            pglGetShaderInfoLog(shader, logLen, nullptr, log);
            HookLog("OpenGLBackend: Shader compile error: %s", log);
            delete[] log;
        }
        pglDeleteShader(shader);
        return 0;
    }

    return shader;
}

GLuint OpenGLBackend::LinkProgram(GLuint vs, GLuint fs) {
    GLuint program = pglCreateProgram();
    if (!program) {
        HookLog("OpenGLBackend: Failed to create program");
        return 0;
    }

    pglAttachShader(program, vs);
    pglAttachShader(program, fs);
    pglLinkProgram(program);

    GLint success = 0;
    pglGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        GLint logLen = 0;
        pglGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 0) {
            char* log = new char[logLen];
            pglGetProgramInfoLog(program, logLen, nullptr, log);
            HookLog("OpenGLBackend: Program link error: %s", log);
            delete[] log;
        }
        pglDeleteProgram(program);
        return 0;
    }

    return program;
}

bool OpenGLBackend::InitModernPath() {
    if (!LoadGLModernFunctions()) {
        HookLog("OpenGLBackend: Failed to load GL 3.0+ functions");
        return false;
    }

    ClearGLErrors();

    GLuint vs = CompileShader(GL_VERTEX_SHADER, GL_VS_SOURCE);
    if (!vs) {
        HookLog("OpenGLBackend: Failed to compile vertex shader");
        return false;
    }

    GLuint fsTextured = CompileShader(GL_FRAGMENT_SHADER, GL_FS_TEXTURED);
    if (!fsTextured) {
        HookLog("OpenGLBackend: Failed to compile textured fragment shader");
        pglDeleteShader(vs);
        return false;
    }

    GLuint fsSolid = CompileShader(GL_FRAGMENT_SHADER, GL_FS_SOLID);
    if (!fsSolid) {
        HookLog("OpenGLBackend: Failed to compile solid fragment shader");
        pglDeleteShader(vs);
        pglDeleteShader(fsTextured);
        return false;
    }

    modern.programTextured = LinkProgram(vs, fsTextured);
    modern.programSolid = LinkProgram(vs, fsSolid);

    pglDeleteShader(vs);
    pglDeleteShader(fsTextured);
    pglDeleteShader(fsSolid);

    if (!modern.programTextured || !modern.programSolid) {
        HookLog("OpenGLBackend: Failed to link shader programs");
        if (modern.programTextured)
            pglDeleteProgram(modern.programTextured);
        if (modern.programSolid)
            pglDeleteProgram(modern.programSolid);
        return false;
    }

    modern.uViewportTextured = pglGetUniformLocation(modern.programTextured, "uViewport");
    modern.uViewportSolid = pglGetUniformLocation(modern.programSolid, "uViewport");
    modern.uTexture = pglGetUniformLocation(modern.programTextured, "uTexture");

    pglGenVertexArrays(1, &modern.vao);
    if (!modern.vao) {
        HookLog("OpenGLBackend: Failed to create VAO");
        pglDeleteProgram(modern.programTextured);
        pglDeleteProgram(modern.programSolid);
        return false;
    }

    pglGenBuffers(1, &modern.vbo);
    pglGenBuffers(1, &modern.ibo);
    if (!modern.vbo || !modern.ibo) {
        HookLog("OpenGLBackend: Failed to create VBO/IBO");
        ShutdownModernPath();
        return false;
    }

    pglBindVertexArray(modern.vao);
    pglBindBuffer(GL_ARRAY_BUFFER, modern.vbo);

    pglEnableVertexAttribArray(0);
    pglVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(DrawVertex), (void*)offsetof(DrawVertex, x));

    pglEnableVertexAttribArray(1);
    pglVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(DrawVertex), (void*)offsetof(DrawVertex, u));

    pglEnableVertexAttribArray(2);
    pglVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(DrawVertex), (void*)offsetof(DrawVertex, color));

    pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, modern.ibo);
    pglBindVertexArray(0);

    modern.valid = true;
    HookLog("OpenGLBackend: Modern path initialized successfully");
    return true;
}

void OpenGLBackend::ShutdownModernPath() {
    if (modern.vao && pglDeleteVertexArrays) {
        pglDeleteVertexArrays(1, &modern.vao);
        modern.vao = 0;
    }
    if (modern.vbo && pglDeleteBuffers) {
        pglDeleteBuffers(1, &modern.vbo);
        modern.vbo = 0;
    }
    if (modern.ibo && pglDeleteBuffers) {
        pglDeleteBuffers(1, &modern.ibo);
        modern.ibo = 0;
    }
    if (modern.programTextured && pglDeleteProgram) {
        pglDeleteProgram(modern.programTextured);
        modern.programTextured = 0;
    }
    if (modern.programSolid && pglDeleteProgram) {
        pglDeleteProgram(modern.programSolid);
        modern.programSolid = 0;
    }
    modern.valid = false;
}

bool OpenGLBackend::Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) {
    if (initialized)
        return true;

    if (!LoadGLFunctions()) {
        HookLog("OpenGLBackend: Failed to load GL functions");
        return false;
    }

    if (!pwglGetCurrentContext) {
        HookLog("OpenGLBackend: wglGetCurrentContext not available");
        return false;
    }

    HGLRC ctx = pwglGetCurrentContext();
    if (!ctx) {
        HookLog("OpenGLBackend: No GL context current");
        return false;
    }

    texWidth = fontTextureWidth;
    texHeight = fontTextureHeight;

    LoadGLLegacyOptionalFunctions();

    ClearGLErrors();

    const char* versionStr = pglGetString ? (const char*)pglGetString(GL_VERSION) : nullptr;
    int major = 0, minor = 0;
    if (versionStr) {
        // SECURITY FIX: Use sscanf_s instead of sscanf for safer parsing
        sscanf_s(versionStr, "%d.%d", &major, &minor);
        HookLog("OpenGLBackend: GL version %d.%d (%s)", major, minor, versionStr);
    }

    pglGenTextures(1, &fontTextureId);
    if (fontTextureId == 0) {
        HookLog("OpenGLBackend: Failed to create font texture");
        return false;
    }

    pglBindTexture(GL_TEXTURE_2D, fontTextureId);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    pglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fontTextureWidth, fontTextureHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                  fontTextureData);
    pglBindTexture(GL_TEXTURE_2D, 0);

    GLenum err = pglGetError ? pglGetError() : 0;
    if (err != 0) {
        HookLog("OpenGLBackend: Font texture creation error 0x%X", err);
    }

    useModernPath = false;
    if (major >= 3) {
        if (InitModernPath()) {
            useModernPath = true;
            HookLog("OpenGLBackend: Using modern path (GL 3.0+)");
        } else {
            HookLog("OpenGLBackend: Modern path init failed, falling back to legacy");
        }
    } else {
        HookLog("OpenGLBackend: Using legacy path (GL %d.%d)", major, minor);
    }

    initialized = true;
    return true;
}

void OpenGLBackend::Shutdown() {
    ShutdownModernPath();

    if (fontTextureId && pglDeleteTextures) {
        pglDeleteTextures(1, &fontTextureId);
        fontTextureId = 0;
    }
    initialized = false;
    useModernPath = false;
    legacyProbeContext = nullptr;
    legacyMatrixChecked = false;
    legacyMatrixValid = true;
    legacyArrayChecked = false;
    legacyArrayProbeSucceeded = false;
}

void OpenGLBackend::Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                           const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) {
    static int logCount = 0;
    if (logCount < 3) {
        HookLog("OpenGLBackend::Render: modern=%d verts=%zu idx=%zu cmds=%zu vp=%dx%d", useModernPath ? 1 : 0,
                vertices.size(), indices.size(), commands.size(), viewportWidth, viewportHeight);
        logCount++;
    }

    if (!initialized || vertices.empty() || commands.empty())
        return;

    if (!g_GLFunctionsLoaded)
        return;

    HGLRC currentCtx = pwglGetCurrentContext ? pwglGetCurrentContext() : nullptr;
    if (!currentCtx)
        return;

    if (useModernPath && modern.valid) {
        RenderModern(vertices, indices, commands, viewportWidth, viewportHeight);
    } else {
        RenderLegacy(vertices, indices, commands, viewportWidth, viewportHeight);
    }
}

void OpenGLBackend::RenderModern(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                                 const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) {
    GLint lastTexture = 0;
    pglGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);
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
                HookLog("OpenGLBackend: Legacy array preflight failed (err=0x%X); using immediate fallback",
                        arrayErr);
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
