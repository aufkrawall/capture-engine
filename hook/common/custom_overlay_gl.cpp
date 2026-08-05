#include "custom_overlay_gl_internal.h"

namespace {
static PFN_wglGetProcAddress pwglGetProcAddress = nullptr;
}

namespace {
static PFN_glGetString pglGetString = nullptr;
}

namespace {
static PFN_glGenTextures pglGenTextures = nullptr;
}

namespace {
static PFN_glDeleteTextures pglDeleteTextures = nullptr;
}

namespace {
static PFN_glTexParameteri pglTexParameteri = nullptr;
}

namespace {
static PFN_glTexImage2D pglTexImage2D = nullptr;
}

namespace {
static PFN_glGenVertexArrays pglGenVertexArrays = nullptr;
}

namespace {
static PFN_glDeleteVertexArrays pglDeleteVertexArrays = nullptr;
}

namespace {
static PFN_glGenBuffers pglGenBuffers = nullptr;
}

namespace {
static PFN_glDeleteBuffers pglDeleteBuffers = nullptr;
}

namespace {
static PFN_glBufferData pglBufferData = nullptr;
}

namespace {
static PFN_glBufferSubData pglBufferSubData = nullptr;
}

namespace {
static PFN_glVertexAttribPointer pglVertexAttribPointer = nullptr;
}

namespace {
static PFN_glEnableVertexAttribArray pglEnableVertexAttribArray = nullptr;
}

namespace {
static PFN_glDisableVertexAttribArray pglDisableVertexAttribArray = nullptr;
}

namespace {
static PFN_glUseProgram pglUseProgram = nullptr;
}

namespace {
static PFN_glCreateShader pglCreateShader = nullptr;
}

namespace {
static PFN_glDeleteShader pglDeleteShader = nullptr;
}

namespace {
static PFN_glShaderSource pglShaderSource = nullptr;
}

namespace {
static PFN_glCompileShader pglCompileShader = nullptr;
}

namespace {
static PFN_glGetShaderiv pglGetShaderiv = nullptr;
}

namespace {
static PFN_glGetShaderInfoLog pglGetShaderInfoLog = nullptr;
}

namespace {
static PFN_glCreateProgram pglCreateProgram = nullptr;
}

namespace {
static PFN_glDeleteProgram pglDeleteProgram = nullptr;
}

namespace {
static PFN_glAttachShader pglAttachShader = nullptr;
}

namespace {
static PFN_glLinkProgram pglLinkProgram = nullptr;
}

namespace {
static PFN_glGetProgramiv pglGetProgramiv = nullptr;
}

namespace {
static PFN_glGetProgramInfoLog pglGetProgramInfoLog = nullptr;
}

namespace {
static PFN_glGetUniformLocation pglGetUniformLocation = nullptr;
}

namespace {
static PFN_glUniform2f pglUniform2f = nullptr;
}

namespace {
static PFN_glUniform1i pglUniform1i = nullptr;
}

namespace {
static bool g_GLFunctionsLoaded = false;
}

namespace {
static bool g_GLModernFunctionsLoaded = false;
}

namespace {
static PROC GetOptionalGLProc(const char* coreName, const char* extensionName = nullptr) {
    PROC proc = pwglGetProcAddress ? pwglGetProcAddress(coreName) : nullptr;
    if ((!proc || proc == (PROC)1 || proc == (PROC)2 || proc == (PROC)3 || proc == (PROC)-1) && extensionName) {
        proc = pwglGetProcAddress ? pwglGetProcAddress(extensionName) : nullptr;
    }
    if (proc == (PROC)1 || proc == (PROC)2 || proc == (PROC)3 || proc == (PROC)-1)
        return nullptr;
    return proc;
}
}

namespace {
static bool LoadGLFunctions() {
    if (g_GLFunctionsLoaded)
        return true;

    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl)
        return false;

    custom_overlay_gl_pwglGetCurrentContext = (PFN_wglGetCurrentContext)GetProcAddress(gl, "wglGetCurrentContext");
    pwglGetProcAddress = (PFN_wglGetProcAddress)GetProcAddress(gl, "wglGetProcAddress");
    custom_overlay_gl_pglGetError = (PFN_glGetError)GetProcAddress(gl, "glGetError");
    pglGetString = (PFN_glGetString)GetProcAddress(gl, "glGetString");

    pglGenTextures = (PFN_glGenTextures)GetProcAddress(gl, "glGenTextures");
    pglDeleteTextures = (PFN_glDeleteTextures)GetProcAddress(gl, "glDeleteTextures");
    custom_overlay_gl_pglBindTexture = (PFN_glBindTexture)GetProcAddress(gl, "glBindTexture");
    pglTexParameteri = (PFN_glTexParameteri)GetProcAddress(gl, "glTexParameteri");
    pglTexImage2D = (PFN_glTexImage2D)GetProcAddress(gl, "glTexImage2D");
    custom_overlay_gl_pglGetIntegerv = (PFN_glGetIntegerv)GetProcAddress(gl, "glGetIntegerv");
    custom_overlay_gl_pglViewport = (PFN_glViewport)GetProcAddress(gl, "glViewport");
    custom_overlay_gl_pglEnable = (PFN_glEnable)GetProcAddress(gl, "glEnable");
    custom_overlay_gl_pglDisable = (PFN_glDisable)GetProcAddress(gl, "glDisable");
    custom_overlay_gl_pglIsEnabled = (PFN_glIsEnabled)GetProcAddress(gl, "glIsEnabled");
    custom_overlay_gl_pglBlendFunc = (PFN_glBlendFunc)GetProcAddress(gl, "glBlendFunc");
    custom_overlay_gl_pglMatrixMode = (PFN_glMatrixMode)GetProcAddress(gl, "glMatrixMode");
    custom_overlay_gl_pglPushMatrix = (PFN_glPushMatrix)GetProcAddress(gl, "glPushMatrix");
    custom_overlay_gl_pglPopMatrix = (PFN_glPopMatrix)GetProcAddress(gl, "glPopMatrix");
    custom_overlay_gl_pglLoadIdentity = (PFN_glLoadIdentity)GetProcAddress(gl, "glLoadIdentity");
    custom_overlay_gl_pglOrtho = (PFN_glOrtho)GetProcAddress(gl, "glOrtho");
    custom_overlay_gl_pglEnableClientState = (PFN_glEnableClientState)GetProcAddress(gl, "glEnableClientState");
    custom_overlay_gl_pglDisableClientState = (PFN_glDisableClientState)GetProcAddress(gl, "glDisableClientState");
    custom_overlay_gl_pglVertexPointer = (PFN_glVertexPointer)GetProcAddress(gl, "glVertexPointer");
    custom_overlay_gl_pglTexCoordPointer = (PFN_glTexCoordPointer)GetProcAddress(gl, "glTexCoordPointer");
    custom_overlay_gl_pglColorPointer = (PFN_glColorPointer)GetProcAddress(gl, "glColorPointer");
    custom_overlay_gl_pglDrawElements = (PFN_glDrawElements)GetProcAddress(gl, "glDrawElements");
    custom_overlay_gl_pglBegin = (PFN_glBegin)GetProcAddress(gl, "glBegin");
    custom_overlay_gl_pglEnd = (PFN_glEnd)GetProcAddress(gl, "glEnd");
    custom_overlay_gl_pglVertex2f = (PFN_glVertex2f)GetProcAddress(gl, "glVertex2f");
    custom_overlay_gl_pglTexCoord2f = (PFN_glTexCoord2f)GetProcAddress(gl, "glTexCoord2f");
    custom_overlay_gl_pglColor4ub = (PFN_glColor4ub)GetProcAddress(gl, "glColor4ub");

    if (!pglGenTextures || !pglDeleteTextures || !custom_overlay_gl_pglBindTexture || !pglTexImage2D || !pglTexParameteri ||
        !custom_overlay_gl_pglGetIntegerv || !custom_overlay_gl_pglViewport || !custom_overlay_gl_pglEnable || !custom_overlay_gl_pglDisable || !custom_overlay_gl_pglIsEnabled || !custom_overlay_gl_pglBlendFunc ||
        !custom_overlay_gl_pglMatrixMode || !custom_overlay_gl_pglPushMatrix || !custom_overlay_gl_pglPopMatrix || !custom_overlay_gl_pglLoadIdentity || !custom_overlay_gl_pglOrtho || !custom_overlay_gl_pglEnableClientState ||
        !custom_overlay_gl_pglDisableClientState || !custom_overlay_gl_pglVertexPointer || !custom_overlay_gl_pglTexCoordPointer || !custom_overlay_gl_pglColorPointer || !custom_overlay_gl_pglDrawElements ||
        !custom_overlay_gl_pglBegin || !custom_overlay_gl_pglEnd || !custom_overlay_gl_pglVertex2f || !custom_overlay_gl_pglTexCoord2f || !custom_overlay_gl_pglColor4ub || !custom_overlay_gl_pwglGetCurrentContext ||
        !pwglGetProcAddress || !custom_overlay_gl_pglGetError) {
        return false;
    }

    g_GLFunctionsLoaded = true;
    return true;
}
}

namespace {
static void LoadGLLegacyOptionalFunctions() {
    if (!custom_overlay_gl_pglBlendFuncSeparate) {
        custom_overlay_gl_pglBlendFuncSeparate =
            (PFN_glBlendFuncSeparate)GetOptionalGLProc("glBlendFuncSeparate", "glBlendFuncSeparateEXT");
    }
    if (!custom_overlay_gl_pglBindBuffer)
        custom_overlay_gl_pglBindBuffer = (PFN_glBindBuffer)GetOptionalGLProc("glBindBuffer", "glBindBufferARB");
    if (!custom_overlay_gl_pglBindVertexArray)
        custom_overlay_gl_pglBindVertexArray = (PFN_glBindVertexArray)GetOptionalGLProc("glBindVertexArray", "glBindVertexArrayAPPLE");
    if (!custom_overlay_gl_pglActiveTexture)
        custom_overlay_gl_pglActiveTexture = (PFN_glActiveTexture)GetOptionalGLProc("glActiveTexture", "glActiveTextureARB");
    if (!custom_overlay_gl_pglClientActiveTexture) {
        custom_overlay_gl_pglClientActiveTexture =
            (PFN_glClientActiveTexture)GetOptionalGLProc("glClientActiveTexture", "glClientActiveTextureARB");
    }
}
}

namespace {
static bool LoadGLModernFunctions() {
    if (g_GLModernFunctionsLoaded)
        return true;

    if (!pwglGetProcAddress)
        return false;

    pglGenVertexArrays = (PFN_glGenVertexArrays)pwglGetProcAddress("glGenVertexArrays");
    pglDeleteVertexArrays = (PFN_glDeleteVertexArrays)pwglGetProcAddress("glDeleteVertexArrays");
    custom_overlay_gl_pglBindVertexArray = (PFN_glBindVertexArray)pwglGetProcAddress("glBindVertexArray");
    pglGenBuffers = (PFN_glGenBuffers)pwglGetProcAddress("glGenBuffers");
    pglDeleteBuffers = (PFN_glDeleteBuffers)pwglGetProcAddress("glDeleteBuffers");
    custom_overlay_gl_pglBindBuffer = (PFN_glBindBuffer)pwglGetProcAddress("glBindBuffer");
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
    custom_overlay_gl_pglActiveTexture = (PFN_glActiveTexture)pwglGetProcAddress("glActiveTexture");

    if (!pglGenVertexArrays || !custom_overlay_gl_pglBindVertexArray || !pglGenBuffers || !custom_overlay_gl_pglBindBuffer || !pglBufferData ||
        !pglUseProgram || !pglCreateShader || !pglShaderSource || !pglCompileShader || !pglCreateProgram ||
        !pglLinkProgram || !pglGetUniformLocation) {
        HookLog("OpenGLBackend: Missing GL 3.0+ functions");
        return false;
    }

    g_GLModernFunctionsLoaded = true;
    return true;
}
}

namespace {
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
}

namespace {
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
}

namespace {
constexpr const char* GL_FS_SOLID = R"GLSL(
#version 330 core
in vec2 vTexCoord;
in vec4 vColor;
out vec4 FragColor;

void main() {
    FragColor = vColor;
}
)GLSL";
}

namespace CustomOverlay {
OpenGLBackend::OpenGLBackend() {}
}

namespace CustomOverlay {
OpenGLBackend::~OpenGLBackend() {
    Shutdown();
}
}

namespace CustomOverlay {
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
}

namespace CustomOverlay {
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
}

namespace CustomOverlay {
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

    custom_overlay_gl_pglBindVertexArray(modern.vao);
    custom_overlay_gl_pglBindBuffer(GL_ARRAY_BUFFER, modern.vbo);

    pglEnableVertexAttribArray(0);
    pglVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(DrawVertex), (void*)offsetof(DrawVertex, x));

    pglEnableVertexAttribArray(1);
    pglVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(DrawVertex), (void*)offsetof(DrawVertex, u));

    pglEnableVertexAttribArray(2);
    pglVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(DrawVertex), (void*)offsetof(DrawVertex, color));

    custom_overlay_gl_pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, modern.ibo);
    custom_overlay_gl_pglBindVertexArray(0);

    modern.valid = true;
    HookLog("OpenGLBackend: Modern path initialized successfully");
    return true;
}
}

namespace CustomOverlay {
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
}

namespace CustomOverlay {
bool OpenGLBackend::Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) {
    if (initialized)
        return true;

    if (!LoadGLFunctions()) {
        HookLog("OpenGLBackend: Failed to load GL functions");
        return false;
    }

    if (!custom_overlay_gl_pwglGetCurrentContext) {
        HookLog("OpenGLBackend: wglGetCurrentContext not available");
        return false;
    }

    HGLRC ctx = custom_overlay_gl_pwglGetCurrentContext();
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

    custom_overlay_gl_pglBindTexture(GL_TEXTURE_2D, fontTextureId);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    pglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fontTextureWidth, fontTextureHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                  fontTextureData);
    custom_overlay_gl_pglBindTexture(GL_TEXTURE_2D, 0);

    GLenum err = custom_overlay_gl_pglGetError ? custom_overlay_gl_pglGetError() : 0;
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
}

namespace CustomOverlay {
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
}

namespace CustomOverlay {
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

    HGLRC currentCtx = custom_overlay_gl_pwglGetCurrentContext ? custom_overlay_gl_pwglGetCurrentContext() : nullptr;
    if (!currentCtx)
        return;

    if (useModernPath && modern.valid) {
        RenderModern(vertices, indices, commands, viewportWidth, viewportHeight);
    } else {
        RenderLegacy(vertices, indices, commands, viewportWidth, viewportHeight);
    }
}
}

namespace CustomOverlay {
void OpenGLBackend::RenderModern(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                                 const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) {
    GLint lastTexture = 0;
    custom_overlay_gl_pglGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);

    GLint lastViewport[4] = {0};
    custom_overlay_gl_pglGetIntegerv(GL_VIEWPORT, lastViewport);
    GLboolean lastBlend = custom_overlay_gl_pglIsEnabled(GL_BLEND);
    GLint lastProgram = 0;
    custom_overlay_gl_pglGetIntegerv(GL_CURRENT_PROGRAM, &lastProgram);
    GLint lastVAO = 0;
    custom_overlay_gl_pglGetIntegerv(0x85B5, &lastVAO);
    GLint lastVBO = 0;
    custom_overlay_gl_pglGetIntegerv(GL_ARRAY_BUFFER_BINDING, &lastVBO);
    GLint lastIBO = 0;
    custom_overlay_gl_pglGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &lastIBO);

    custom_overlay_gl_pglEnable(GL_BLEND);
    custom_overlay_gl_pglBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    custom_overlay_gl_pglDisable(GL_DEPTH_TEST);
    custom_overlay_gl_pglDisable(GL_CULL_FACE);

    custom_overlay_gl_pglViewport(0, 0, viewportWidth, viewportHeight);

    custom_overlay_gl_pglBindVertexArray(modern.vao);
    custom_overlay_gl_pglBindBuffer(GL_ARRAY_BUFFER, modern.vbo);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    pglBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(DrawVertex), vertices.data(), GL_DYNAMIC_DRAW);

    custom_overlay_gl_pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, modern.ibo);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    pglBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint16_t), indices.data(), GL_DYNAMIC_DRAW);

    for (const auto& cmd : commands) {
        GLuint program = cmd.useTexture ? modern.programTextured : modern.programSolid;
        pglUseProgram(program);

        if (cmd.useTexture) {
            pglUniform2f(modern.uViewportTextured, (float)viewportWidth, (float)viewportHeight);
            if (custom_overlay_gl_pglActiveTexture)
                custom_overlay_gl_pglActiveTexture(GL_TEXTURE0);
            custom_overlay_gl_pglBindTexture(GL_TEXTURE_2D, fontTextureId);
            pglUniform1i(modern.uTexture, 0);
        } else {
            pglUniform2f(modern.uViewportSolid, (float)viewportWidth, (float)viewportHeight);
            custom_overlay_gl_pglBindTexture(GL_TEXTURE_2D, 0);
        }

        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        custom_overlay_gl_pglDrawElements(GL_TRIANGLES, cmd.indexCount, GL_UNSIGNED_SHORT,
                        (const void*)(cmd.indexOffset * sizeof(uint16_t)));
    }

    custom_overlay_gl_pglBindVertexArray(0);
    pglUseProgram(0);

    custom_overlay_gl_pglBindTexture(GL_TEXTURE_2D, lastTexture);
    custom_overlay_gl_pglViewport(lastViewport[0], lastViewport[1], lastViewport[2], lastViewport[3]);

    if (lastBlend)
        custom_overlay_gl_pglEnable(GL_BLEND);
    else
        custom_overlay_gl_pglDisable(GL_BLEND);

    if (lastVAO && custom_overlay_gl_pglBindVertexArray)
        custom_overlay_gl_pglBindVertexArray(lastVAO);
    if (lastVBO)
        custom_overlay_gl_pglBindBuffer(GL_ARRAY_BUFFER, lastVBO);
    if (lastIBO)
        custom_overlay_gl_pglBindBuffer(GL_ELEMENT_ARRAY_BUFFER, lastIBO);
    if (lastProgram)
        pglUseProgram(lastProgram);
}
}
