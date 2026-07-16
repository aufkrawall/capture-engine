#include "opengl_sampler_override.h"
#include "opengl_texture_storage_override.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "../common/sampler_override_utils.h"
#include "../wrappers/iat_hook.h"
#include "hook_common.h"
#include "lod_helper.h"

namespace ce::opengl_sampler_override {
namespace {

using GLenum = unsigned int;
using GLint = int;
using GLsizei = int;
using GLuint = unsigned int;
using GLfloat = float;
using GLubyte = unsigned char;

constexpr GLenum GL_EXTENSIONS = 0x1F03;
constexpr GLenum GL_VERSION = 0x1F02;
constexpr GLenum GL_NUM_EXTENSIONS = 0x821D;
constexpr GLenum GL_TEXTURE_MAG_FILTER = 0x2800;
constexpr GLenum GL_TEXTURE_MIN_FILTER = 0x2801;
constexpr GLenum GL_TEXTURE_WRAP_S = 0x2802;
constexpr GLenum GL_TEXTURE_WRAP_T = 0x2803;
constexpr GLenum GL_TEXTURE_WRAP_R = 0x8072;
constexpr GLenum GL_TEXTURE_BASE_LEVEL = 0x813C;
constexpr GLenum GL_TEXTURE_MAX_LEVEL = 0x813D;
constexpr GLenum GL_TEXTURE_COMPARE_MODE = 0x884C;
constexpr GLenum GL_TEXTURE_LOD_BIAS = 0x8501;
constexpr GLenum GL_TEXTURE_WIDTH = 0x1000;
constexpr GLenum GL_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE;
constexpr GLenum GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FF;
constexpr GLenum GL_TEXTURE_1D = 0x0DE0;
constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
constexpr GLenum GL_TEXTURE_3D = 0x806F;
constexpr GLenum GL_TEXTURE_CUBE_MAP = 0x8513;
constexpr GLenum GL_TEXTURE_CUBE_MAP_POSITIVE_X = 0x8515;
constexpr GLenum GL_TEXTURE_CUBE_MAP_NEGATIVE_Z = 0x851A;
constexpr GLenum GL_TEXTURE_1D_ARRAY = 0x8C18;
constexpr GLenum GL_TEXTURE_2D_ARRAY = 0x8C1A;

using TexParameteriFn = void(WINAPI*)(GLenum, GLenum, GLint);
using TexParameterfFn = void(WINAPI*)(GLenum, GLenum, GLfloat);
using TexParameterivFn = void(WINAPI*)(GLenum, GLenum, const GLint*);
using TexParameterfvFn = void(WINAPI*)(GLenum, GLenum, const GLfloat*);
using GetTexParameterivFn = void(WINAPI*)(GLenum, GLenum, GLint*);
using GetTexLevelParameterivFn = void(WINAPI*)(GLenum, GLint, GLenum, GLint*);
using GetFloatvFn = void(WINAPI*)(GLenum, GLfloat*);
using GetIntegervFn = void(WINAPI*)(GLenum, GLint*);
using GetStringFn = const GLubyte*(WINAPI*)(GLenum);
using GetStringiFn = const GLubyte*(WINAPI*)(GLenum, GLuint);

using SamplerParameteriFn = void(WINAPI*)(GLuint, GLenum, GLint);
using SamplerParameterfFn = void(WINAPI*)(GLuint, GLenum, GLfloat);
using SamplerParameterivFn = void(WINAPI*)(GLuint, GLenum, const GLint*);
using SamplerParameterfvFn = void(WINAPI*)(GLuint, GLenum, const GLfloat*);
using SamplerParameterIivFn = void(WINAPI*)(GLuint, GLenum, const GLint*);
using SamplerParameterIuivFn = void(WINAPI*)(GLuint, GLenum, const GLuint*);
using GetSamplerParameterivFn = void(WINAPI*)(GLuint, GLenum, GLint*);

using TextureParameteriFn = void(WINAPI*)(GLuint, GLenum, GLint);
using TextureParameterfFn = void(WINAPI*)(GLuint, GLenum, GLfloat);
using TextureParameterivFn = void(WINAPI*)(GLuint, GLenum, const GLint*);
using TextureParameterfvFn = void(WINAPI*)(GLuint, GLenum, const GLfloat*);
using TextureParameterIivFn = void(WINAPI*)(GLuint, GLenum, const GLint*);
using TextureParameterIuivFn = void(WINAPI*)(GLuint, GLenum, const GLuint*);
using GetTextureParameterivFn = void(WINAPI*)(GLuint, GLenum, GLint*);
using GetTextureLevelParameterivFn = void(WINAPI*)(GLuint, GLint, GLenum, GLint*);

using TextureParameteriExtFn = void(WINAPI*)(GLuint, GLenum, GLenum, GLint);
using TextureParameterfExtFn = void(WINAPI*)(GLuint, GLenum, GLenum, GLfloat);
using TextureParameterivExtFn = void(WINAPI*)(GLuint, GLenum, GLenum, const GLint*);
using TextureParameterfvExtFn = void(WINAPI*)(GLuint, GLenum, GLenum, const GLfloat*);
using TextureParameterIivExtFn = void(WINAPI*)(GLuint, GLenum, GLenum, const GLint*);
using TextureParameterIuivExtFn = void(WINAPI*)(GLuint, GLenum, GLenum, const GLuint*);
using GetTextureParameterivExtFn = void(WINAPI*)(GLuint, GLenum, GLenum, GLint*);
using GetTextureLevelParameterivExtFn = void(WINAPI*)(GLuint, GLenum, GLint, GLenum, GLint*);

TexParameteriFn g_texParameteri = nullptr;
TexParameterfFn g_texParameterf = nullptr;
TexParameterivFn g_texParameteriv = nullptr;
TexParameterfvFn g_texParameterfv = nullptr;
GetTexParameterivFn g_getTexParameteriv = nullptr;
GetTexLevelParameterivFn g_getTexLevelParameteriv = nullptr;
GetFloatvFn g_getFloatv = nullptr;
GetIntegervFn g_getIntegerv = nullptr;
GetStringFn g_getString = nullptr;
GetStringiFn g_getStringi = nullptr;

SamplerParameteriFn g_samplerParameteri = nullptr;
SamplerParameterfFn g_samplerParameterf = nullptr;
SamplerParameterivFn g_samplerParameteriv = nullptr;
SamplerParameterfvFn g_samplerParameterfv = nullptr;
SamplerParameterIivFn g_samplerParameterIiv = nullptr;
SamplerParameterIuivFn g_samplerParameterIuiv = nullptr;
GetSamplerParameterivFn g_getSamplerParameteriv = nullptr;

TextureParameteriFn g_textureParameteri = nullptr;
TextureParameterfFn g_textureParameterf = nullptr;
TextureParameterivFn g_textureParameteriv = nullptr;
TextureParameterfvFn g_textureParameterfv = nullptr;
TextureParameterIivFn g_textureParameterIiv = nullptr;
TextureParameterIuivFn g_textureParameterIuiv = nullptr;
GetTextureParameterivFn g_getTextureParameteriv = nullptr;
GetTextureLevelParameterivFn g_getTextureLevelParameteriv = nullptr;

TextureParameteriExtFn g_textureParameteriExt = nullptr;
TextureParameterfExtFn g_textureParameterfExt = nullptr;
TextureParameterivExtFn g_textureParameterivExt = nullptr;
TextureParameterfvExtFn g_textureParameterfvExt = nullptr;
TextureParameterIivExtFn g_textureParameterIivExt = nullptr;
TextureParameterIuivExtFn g_textureParameterIuivExt = nullptr;
GetTextureParameterivExtFn g_getTextureParameterivExt = nullptr;
GetTextureLevelParameterivExtFn g_getTextureLevelParameterivExt = nullptr;

struct CapabilityState {
    HGLRC context = nullptr;
    bool initialized = false;
    bool anisotropySupported = false;
    float maxAnisotropy = 1.0f;
    int major = 1;
    int minor = 1;
};

thread_local CapabilityState t_caps;
std::atomic<uint64_t> g_parameterCalls{0};
std::atomic<uint64_t> g_afApplications{0};
std::atomic<uint64_t> g_afUnchanged{0};
std::atomic<uint64_t> g_afSafetyRestores{0};
std::atomic<int> g_decisionLogCount{0};

bool IsValidProc(PROC proc) {
    const intptr_t value = reinterpret_cast<intptr_t>(proc);
    return proc && value != 1 && value != 2 && value != 3 && value != -1;
}

template <typename T>
T ResolveProc(ProcResolver resolver, const char* name) {
    PROC proc = resolver ? resolver(name) : nullptr;
    return IsValidProc(proc) ? reinterpret_cast<T>(proc) : nullptr;
}

bool ContainsExtension(const char* extensions, const char* wanted) {
    if (!extensions || !wanted || !*wanted) {
        return false;
    }
    const size_t wantedLength = std::strlen(wanted);
    const char* cursor = extensions;
    while ((cursor = std::strstr(cursor, wanted)) != nullptr) {
        const bool leftBoundary = cursor == extensions || cursor[-1] == ' ';
        const char right = cursor[wantedLength];
        if (leftBoundary && (right == '\0' || right == ' ')) {
            return true;
        }
        cursor += wantedLength;
    }
    return false;
}

void LoadCoreQueries() {
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) {
        return;
    }
    if (!g_getTexParameteriv)
        g_getTexParameteriv = reinterpret_cast<GetTexParameterivFn>(GetProcAddress(gl, "glGetTexParameteriv"));
    if (!g_getTexLevelParameteriv)
        g_getTexLevelParameteriv =
            reinterpret_cast<GetTexLevelParameterivFn>(GetProcAddress(gl, "glGetTexLevelParameteriv"));
    if (!g_getFloatv)
        g_getFloatv = reinterpret_cast<GetFloatvFn>(GetProcAddress(gl, "glGetFloatv"));
    if (!g_getIntegerv)
        g_getIntegerv = reinterpret_cast<GetIntegervFn>(GetProcAddress(gl, "glGetIntegerv"));
    if (!g_getString)
        g_getString = reinterpret_cast<GetStringFn>(GetProcAddress(gl, "glGetString"));
    if (!g_texParameteri)
        g_texParameteri = reinterpret_cast<TexParameteriFn>(GetProcAddress(gl, "glTexParameteri"));
    if (!g_texParameterf)
        g_texParameterf = reinterpret_cast<TexParameterfFn>(GetProcAddress(gl, "glTexParameterf"));
    if (!g_texParameteriv)
        g_texParameteriv = reinterpret_cast<TexParameterivFn>(GetProcAddress(gl, "glTexParameteriv"));
    if (!g_texParameterfv)
        g_texParameterfv = reinterpret_cast<TexParameterfvFn>(GetProcAddress(gl, "glTexParameterfv"));
}

void EnsureCapabilities(ProcResolver resolver) {
    const HGLRC context = wglGetCurrentContext();
    if (t_caps.initialized && t_caps.context == context) {
        return;
    }
    t_caps = {};
    t_caps.context = context;
    t_caps.initialized = true;
    if (!context) {
        return;
    }

    LoadCoreQueries();
    const char* version = g_getString ? reinterpret_cast<const char*>(g_getString(GL_VERSION)) : nullptr;
    if (version) {
        int major = 1;
        int minor = 1;
        if (std::sscanf(version, "%d.%d", &major, &minor) == 2) {
            t_caps.major = major;
            t_caps.minor = minor;
        }
    }

    bool supported = false;
    if (t_caps.major >= 3 && resolver && g_getIntegerv) {
        if (!g_getStringi) {
            g_getStringi = ResolveProc<GetStringiFn>(resolver, "glGetStringi");
        }
        if (g_getStringi) {
            GLint count = 0;
            g_getIntegerv(GL_NUM_EXTENSIONS, &count);
            for (GLint i = 0; i < count; ++i) {
                const char* extension = reinterpret_cast<const char*>(g_getStringi(GL_EXTENSIONS, i));
                if (extension && (!std::strcmp(extension, "GL_EXT_texture_filter_anisotropic") ||
                                  !std::strcmp(extension, "GL_ARB_texture_filter_anisotropic"))) {
                    supported = true;
                    break;
                }
            }
        }
    } else if (g_getString) {
        const char* extensions = reinterpret_cast<const char*>(g_getString(GL_EXTENSIONS));
        supported = ContainsExtension(extensions, "GL_EXT_texture_filter_anisotropic") ||
                    ContainsExtension(extensions, "GL_ARB_texture_filter_anisotropic");
    }

    if (supported && g_getFloatv) {
        GLfloat maximum = 1.0f;
        g_getFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maximum);
        if (maximum >= 2.0f) {
            t_caps.anisotropySupported = true;
            t_caps.maxAnisotropy = maximum;
        }
    }
}

bool IsMipCapableTarget(GLenum target) {
    return target == GL_TEXTURE_1D || target == GL_TEXTURE_2D || target == GL_TEXTURE_3D ||
           target == GL_TEXTURE_CUBE_MAP ||
           (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) ||
           target == GL_TEXTURE_1D_ARRAY || target == GL_TEXTURE_2D_ARRAY;
}

void SetTargetAddressDimensions(GLenum target, ce::sampler_override::OpenGLSamplerForcedAFInfo& info) {
    info.usesWrapT = target != GL_TEXTURE_1D && target != GL_TEXTURE_1D_ARRAY;
    info.usesWrapR = target == GL_TEXTURE_3D || target == GL_TEXTURE_CUBE_MAP ||
                     (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z);
    if (target == GL_TEXTURE_2D || target == GL_TEXTURE_2D_ARRAY || target == GL_TEXTURE_CUBE_MAP) {
        info.usesWrapT = true;
    }
}

bool IsControlledParameter(GLenum pname) {
    switch (pname) {
        case GL_TEXTURE_MAG_FILTER:
        case GL_TEXTURE_MIN_FILTER:
        case GL_TEXTURE_WRAP_S:
        case GL_TEXTURE_WRAP_T:
        case GL_TEXTURE_WRAP_R:
        case GL_TEXTURE_BASE_LEVEL:
        case GL_TEXTURE_MAX_LEVEL:
        case GL_TEXTURE_COMPARE_MODE:
        case GL_TEXTURE_LOD_BIAS:
        case GL_TEXTURE_MAX_ANISOTROPY_EXT:
            return true;
        default:
            return false;
    }
}

GLint OverrideMinFilter(GLint value, const GraphicsConfig& gfx) {
    if (gfx.mipMapping == "trilinear" && value >= 0x2700 && value <= 0x2702) {
        return 0x2703;
    }
    if (gfx.mipMapping == "bilinear" && (value == 0x2702 || value == 0x2703)) {
        return 0x2701;
    }
    if (gfx.mipMapping == "nearest" && value >= 0x2700 && value <= 0x2703) {
        return 0x2700;
    }
    return value;
}

GLfloat OverrideFloatValue(GLenum pname, GLfloat value, const GraphicsConfig& gfx, ProcResolver) {
    if (pname == GL_TEXTURE_LOD_BIAS && (gfx.forceMipBiasClamp || HasConfiguredMipBias(gfx))) {
        value = FinalizeMipBias(gfx, ApplyConfiguredMipBias(gfx, value));
    }
    return value;
}

GLint OverrideIntegerValue(GLenum pname, GLint value, const GraphicsConfig& gfx, ProcResolver resolver) {
    if (pname == GL_TEXTURE_MIN_FILTER) {
        value = OverrideMinFilter(value, gfx);
    }
    return static_cast<GLint>(OverrideFloatValue(pname, static_cast<GLfloat>(value), gfx, resolver));
}

void QueryTargetInfo(GLenum target, ce::sampler_override::OpenGLSamplerForcedAFInfo& info) {
    info.extensionSupported = t_caps.anisotropySupported;
    info.deviceMaxAnisotropy = t_caps.maxAnisotropy;
    SetTargetAddressDimensions(target, info);
    if (!g_getTexParameteriv || !IsMipCapableTarget(target)) {
        info.maxLevel = info.baseLevel;
        return;
    }
    g_getTexParameteriv(target, GL_TEXTURE_MIN_FILTER, &info.minFilter);
    g_getTexParameteriv(target, GL_TEXTURE_MAG_FILTER, &info.magFilter);
    g_getTexParameteriv(target, GL_TEXTURE_WRAP_S, &info.wrapS);
    if (info.usesWrapT)
        g_getTexParameteriv(target, GL_TEXTURE_WRAP_T, &info.wrapT);
    if (info.usesWrapR)
        g_getTexParameteriv(target, GL_TEXTURE_WRAP_R, &info.wrapR);
    if (t_caps.major > 1 || (t_caps.major == 1 && t_caps.minor >= 2)) {
        g_getTexParameteriv(target, GL_TEXTURE_BASE_LEVEL, &info.baseLevel);
        g_getTexParameteriv(target, GL_TEXTURE_MAX_LEVEL, &info.maxLevel);
    }
    if (t_caps.major > 1 || (t_caps.major == 1 && t_caps.minor >= 4)) {
        g_getTexParameteriv(target, GL_TEXTURE_COMPARE_MODE, &info.compareMode);
    }
    GLint currentAnisotropy = 1;
    g_getTexParameteriv(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, &currentAnisotropy);
    info.currentAnisotropy = static_cast<float>(currentAnisotropy);
    if (info.maxLevel > info.baseLevel && g_getTexLevelParameteriv) {
        GLint mipWidth = 0;
        const GLenum levelTarget = target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X : target;
        g_getTexLevelParameteriv(levelTarget, info.baseLevel + 1, GL_TEXTURE_WIDTH, &mipWidth);
        if (mipWidth <= 0)
            info.maxLevel = info.baseLevel;
    }
}

void QuerySamplerInfo(GLuint sampler, ce::sampler_override::OpenGLSamplerForcedAFInfo& info) {
    info.extensionSupported = t_caps.anisotropySupported;
    info.deviceMaxAnisotropy = t_caps.maxAnisotropy;
    if (!g_getSamplerParameteriv) {
        info.maxLevel = info.baseLevel;
        return;
    }
    g_getSamplerParameteriv(sampler, GL_TEXTURE_MIN_FILTER, &info.minFilter);
    g_getSamplerParameteriv(sampler, GL_TEXTURE_MAG_FILTER, &info.magFilter);
    g_getSamplerParameteriv(sampler, GL_TEXTURE_WRAP_S, &info.wrapS);
    g_getSamplerParameteriv(sampler, GL_TEXTURE_WRAP_T, &info.wrapT);
    g_getSamplerParameteriv(sampler, GL_TEXTURE_WRAP_R, &info.wrapR);
    g_getSamplerParameteriv(sampler, GL_TEXTURE_COMPARE_MODE, &info.compareMode);
    GLint currentAnisotropy = 1;
    g_getSamplerParameteriv(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, &currentAnisotropy);
    info.currentAnisotropy = static_cast<float>(currentAnisotropy);
}

void QueryTextureInfo(GLuint texture, ce::sampler_override::OpenGLSamplerForcedAFInfo& info) {
    info.extensionSupported = t_caps.anisotropySupported;
    info.deviceMaxAnisotropy = t_caps.maxAnisotropy;
    if (!g_getTextureParameteriv) {
        info.maxLevel = info.baseLevel;
        return;
    }
    g_getTextureParameteriv(texture, GL_TEXTURE_MIN_FILTER, &info.minFilter);
    g_getTextureParameteriv(texture, GL_TEXTURE_MAG_FILTER, &info.magFilter);
    g_getTextureParameteriv(texture, GL_TEXTURE_WRAP_S, &info.wrapS);
    g_getTextureParameteriv(texture, GL_TEXTURE_WRAP_T, &info.wrapT);
    g_getTextureParameteriv(texture, GL_TEXTURE_WRAP_R, &info.wrapR);
    g_getTextureParameteriv(texture, GL_TEXTURE_BASE_LEVEL, &info.baseLevel);
    g_getTextureParameteriv(texture, GL_TEXTURE_MAX_LEVEL, &info.maxLevel);
    g_getTextureParameteriv(texture, GL_TEXTURE_COMPARE_MODE, &info.compareMode);
    GLint currentAnisotropy = 1;
    g_getTextureParameteriv(texture, GL_TEXTURE_MAX_ANISOTROPY_EXT, &currentAnisotropy);
    info.currentAnisotropy = static_cast<float>(currentAnisotropy);
    if (info.maxLevel > info.baseLevel && g_getTextureLevelParameteriv) {
        GLint mipWidth = 0;
        g_getTextureLevelParameteriv(texture, info.baseLevel + 1, GL_TEXTURE_WIDTH, &mipWidth);
        if (mipWidth <= 0)
            info.maxLevel = info.baseLevel;
    }
}

void QueryTextureInfoExt(GLuint texture, GLenum target, ce::sampler_override::OpenGLSamplerForcedAFInfo& info) {
    info.extensionSupported = t_caps.anisotropySupported;
    info.deviceMaxAnisotropy = t_caps.maxAnisotropy;
    SetTargetAddressDimensions(target, info);
    if (!g_getTextureParameterivExt || !IsMipCapableTarget(target)) {
        info.maxLevel = info.baseLevel;
        return;
    }
    g_getTextureParameterivExt(texture, target, GL_TEXTURE_MIN_FILTER, &info.minFilter);
    g_getTextureParameterivExt(texture, target, GL_TEXTURE_MAG_FILTER, &info.magFilter);
    g_getTextureParameterivExt(texture, target, GL_TEXTURE_WRAP_S, &info.wrapS);
    if (info.usesWrapT)
        g_getTextureParameterivExt(texture, target, GL_TEXTURE_WRAP_T, &info.wrapT);
    if (info.usesWrapR)
        g_getTextureParameterivExt(texture, target, GL_TEXTURE_WRAP_R, &info.wrapR);
    g_getTextureParameterivExt(texture, target, GL_TEXTURE_BASE_LEVEL, &info.baseLevel);
    g_getTextureParameterivExt(texture, target, GL_TEXTURE_MAX_LEVEL, &info.maxLevel);
    g_getTextureParameterivExt(texture, target, GL_TEXTURE_COMPARE_MODE, &info.compareMode);
    GLint currentAnisotropy = 1;
    g_getTextureParameterivExt(texture, target, GL_TEXTURE_MAX_ANISOTROPY_EXT, &currentAnisotropy);
    info.currentAnisotropy = static_cast<float>(currentAnisotropy);
    if (info.maxLevel > info.baseLevel && g_getTextureLevelParameterivExt) {
        GLint mipWidth = 0;
        const GLenum levelTarget = target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X : target;
        g_getTextureLevelParameterivExt(texture, levelTarget, info.baseLevel + 1, GL_TEXTURE_WIDTH, &mipWidth);
        if (mipWidth <= 0)
            info.maxLevel = info.baseLevel;
    }
}

float ResolveDesiredAF(const ce::sampler_override::OpenGLSamplerForcedAFInfo& info, const GraphicsConfig& gfx,
                       ce::sampler_override::OpenGLForcedAFDecision& decision) {
    decision = ce::sampler_override::ClassifyOpenGLSamplerForForcedAF(info, gfx);
    if (gfx.anisotropicFiltering == "off") {
        return 1.0f;
    }
    return decision == ce::sampler_override::OpenGLForcedAFDecision::Allow
               ? ce::sampler_override::ResolveOpenGLForcedAnisotropy(info, gfx)
               : 1.0f;
}

void LogDecision(ce::sampler_override::OpenGLForcedAFDecision decision, float desired, const GraphicsConfig& gfx,
                 const char* objectKind) {
    const int index = g_decisionLogCount.fetch_add(1, std::memory_order_relaxed);
    if (index < 48) {
        HookLogImportant("OpenGL: AF reconcile object=%s decision=%d desired=%.1f max=%.1f policy=%s (#%d)",
                         objectKind, static_cast<int>(decision), desired, t_caps.maxAnisotropy,
                         gfx.samplerOverrideMode.c_str(), index + 1);
    }
}

void ApplyTargetAF(GLenum target, ProcResolver resolver) {
    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    if (gfx.anisotropicFiltering.empty() || gfx.anisotropicFiltering == "default") {
        return;
    }
    if (!IsMipCapableTarget(target)) {
        return;
    }
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X && target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z) {
        target = GL_TEXTURE_CUBE_MAP;
    }
    EnsureCapabilities(resolver);
    if (!t_caps.anisotropySupported || !g_texParameterf) {
        return;
    }
    ce::sampler_override::OpenGLSamplerForcedAFInfo info = {};
    QueryTargetInfo(target, info);
    ce::sampler_override::OpenGLForcedAFDecision decision;
    const float desired = ResolveDesiredAF(info, gfx, decision);
    if (info.currentAnisotropy != desired) {
        g_texParameterf(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, desired);
        g_afApplications.fetch_add(1, std::memory_order_relaxed);
        if (desired == 1.0f && gfx.anisotropicFiltering != "off")
            g_afSafetyRestores.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_afUnchanged.fetch_add(1, std::memory_order_relaxed);
    }
    LogDecision(decision, desired, gfx, "bound-texture");
}

void ApplySamplerAF(GLuint sampler, ProcResolver resolver) {
    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    if (gfx.anisotropicFiltering.empty() || gfx.anisotropicFiltering == "default") {
        return;
    }
    EnsureCapabilities(resolver);
    if (!t_caps.anisotropySupported || !g_samplerParameterf) {
        return;
    }
    ce::sampler_override::OpenGLSamplerForcedAFInfo info = {};
    QuerySamplerInfo(sampler, info);
    ce::sampler_override::OpenGLForcedAFDecision decision;
    const float desired = ResolveDesiredAF(info, gfx, decision);
    if (info.currentAnisotropy != desired) {
        g_samplerParameterf(sampler, GL_TEXTURE_MAX_ANISOTROPY_EXT, desired);
        g_afApplications.fetch_add(1, std::memory_order_relaxed);
        if (desired == 1.0f && gfx.anisotropicFiltering != "off")
            g_afSafetyRestores.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_afUnchanged.fetch_add(1, std::memory_order_relaxed);
    }
    LogDecision(decision, desired, gfx, "sampler");
}

void ApplyTextureAF(GLuint texture, ProcResolver resolver) {
    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    if (gfx.anisotropicFiltering.empty() || gfx.anisotropicFiltering == "default") {
        return;
    }
    EnsureCapabilities(resolver);
    if (!t_caps.anisotropySupported || !g_textureParameterf) {
        return;
    }
    ce::sampler_override::OpenGLSamplerForcedAFInfo info = {};
    QueryTextureInfo(texture, info);
    ce::sampler_override::OpenGLForcedAFDecision decision;
    const float desired = ResolveDesiredAF(info, gfx, decision);
    if (info.currentAnisotropy != desired) {
        g_textureParameterf(texture, GL_TEXTURE_MAX_ANISOTROPY_EXT, desired);
        g_afApplications.fetch_add(1, std::memory_order_relaxed);
        if (desired == 1.0f && gfx.anisotropicFiltering != "off")
            g_afSafetyRestores.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_afUnchanged.fetch_add(1, std::memory_order_relaxed);
    }
    LogDecision(decision, desired, gfx, "texture-dsa");
}

void ApplyTextureAFExt(GLuint texture, GLenum target, ProcResolver resolver) {
    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    if (gfx.anisotropicFiltering.empty() || gfx.anisotropicFiltering == "default") {
        return;
    }
    if (!IsMipCapableTarget(target)) {
        return;
    }
    EnsureCapabilities(resolver);
    if (!t_caps.anisotropySupported || !g_textureParameterfExt) {
        return;
    }
    ce::sampler_override::OpenGLSamplerForcedAFInfo info = {};
    QueryTextureInfoExt(texture, target, info);
    ce::sampler_override::OpenGLForcedAFDecision decision;
    const float desired = ResolveDesiredAF(info, gfx, decision);
    if (info.currentAnisotropy != desired) {
        g_textureParameterfExt(texture, target, GL_TEXTURE_MAX_ANISOTROPY_EXT, desired);
        g_afApplications.fetch_add(1, std::memory_order_relaxed);
        if (desired == 1.0f && gfx.anisotropicFiltering != "off")
            g_afSafetyRestores.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_afUnchanged.fetch_add(1, std::memory_order_relaxed);
    }
    LogDecision(decision, desired, gfx, "texture-ext-dsa");
}

thread_local ProcResolver t_resolver = nullptr;
ProcResolver g_defaultResolver = nullptr;

ProcResolver CurrentResolver() {
    return t_resolver ? t_resolver : g_defaultResolver;
}

void WINAPI DetourTexParameteri(GLenum target, GLenum pname, GLint value) {
    ++g_parameterCalls;
    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    if (g_texParameteri)
        g_texParameteri(target, pname, OverrideIntegerValue(pname, value, gfx, CurrentResolver()));
    if (IsControlledParameter(pname))
        ApplyTargetAF(target, CurrentResolver());
}

void WINAPI DetourTexParameterf(GLenum target, GLenum pname, GLfloat value) {
    ++g_parameterCalls;
    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    if (g_texParameterf)
        g_texParameterf(target, pname, OverrideFloatValue(pname, value, gfx, CurrentResolver()));
    if (IsControlledParameter(pname))
        ApplyTargetAF(target, CurrentResolver());
}

void WINAPI DetourTexParameteriv(GLenum target, GLenum pname, const GLint* values) {
    ++g_parameterCalls;
    if (!values || !IsControlledParameter(pname)) {
        if (g_texParameteriv)
            g_texParameteriv(target, pname, values);
        return;
    }
    const GLint value = OverrideIntegerValue(pname, values[0], GetActiveGraphicsConfigCached(), CurrentResolver());
    if (g_texParameteriv)
        g_texParameteriv(target, pname, &value);
    ApplyTargetAF(target, CurrentResolver());
}

void WINAPI DetourTexParameterfv(GLenum target, GLenum pname, const GLfloat* values) {
    ++g_parameterCalls;
    if (!values || !IsControlledParameter(pname)) {
        if (g_texParameterfv)
            g_texParameterfv(target, pname, values);
        return;
    }
    const GLfloat value = OverrideFloatValue(pname, values[0], GetActiveGraphicsConfigCached(), CurrentResolver());
    if (g_texParameterfv)
        g_texParameterfv(target, pname, &value);
    ApplyTargetAF(target, CurrentResolver());
}

#define DEFINE_SAMPLER_SCALAR_DETOUR(name, type, original, overrideFn)                                       \
    void WINAPI name(GLuint sampler, GLenum pname, type value) {                                             \
        ++g_parameterCalls;                                                                                   \
        const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();                                         \
        if (original)                                                                                         \
            original(sampler, pname, overrideFn(pname, value, gfx, CurrentResolver()));                      \
        if (IsControlledParameter(pname))                                                                     \
            ApplySamplerAF(sampler, CurrentResolver());                                                      \
    }

DEFINE_SAMPLER_SCALAR_DETOUR(DetourSamplerParameteri, GLint, g_samplerParameteri, OverrideIntegerValue)
DEFINE_SAMPLER_SCALAR_DETOUR(DetourSamplerParameterf, GLfloat, g_samplerParameterf, OverrideFloatValue)

#define DEFINE_SAMPLER_VECTOR_DETOUR(name, type, original, overrideFn)                                        \
    void WINAPI name(GLuint sampler, GLenum pname, const type* values) {                                      \
        ++g_parameterCalls;                                                                                   \
        if (!values || !IsControlledParameter(pname)) {                                                       \
            if (original)                                                                                     \
                original(sampler, pname, values);                                                             \
            return;                                                                                           \
        }                                                                                                     \
        const type value = static_cast<type>(                                                                 \
            overrideFn(pname, values[0], GetActiveGraphicsConfigCached(), CurrentResolver()));               \
        if (original)                                                                                         \
            original(sampler, pname, &value);                                                                 \
        ApplySamplerAF(sampler, CurrentResolver());                                                          \
    }

DEFINE_SAMPLER_VECTOR_DETOUR(DetourSamplerParameteriv, GLint, g_samplerParameteriv, OverrideIntegerValue)
DEFINE_SAMPLER_VECTOR_DETOUR(DetourSamplerParameterfv, GLfloat, g_samplerParameterfv, OverrideFloatValue)
DEFINE_SAMPLER_VECTOR_DETOUR(DetourSamplerParameterIiv, GLint, g_samplerParameterIiv, OverrideIntegerValue)
DEFINE_SAMPLER_VECTOR_DETOUR(DetourSamplerParameterIuiv, GLuint, g_samplerParameterIuiv, OverrideIntegerValue)

#define DEFINE_TEXTURE_SCALAR_DETOUR(name, type, original, overrideFn)                                        \
    void WINAPI name(GLuint texture, GLenum pname, type value) {                                              \
        ++g_parameterCalls;                                                                                   \
        const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();                                         \
        if (original)                                                                                         \
            original(texture, pname, overrideFn(pname, value, gfx, CurrentResolver()));                      \
        if (IsControlledParameter(pname))                                                                     \
            ApplyTextureAF(texture, CurrentResolver());                                                      \
    }

DEFINE_TEXTURE_SCALAR_DETOUR(DetourTextureParameteri, GLint, g_textureParameteri, OverrideIntegerValue)
DEFINE_TEXTURE_SCALAR_DETOUR(DetourTextureParameterf, GLfloat, g_textureParameterf, OverrideFloatValue)

#define DEFINE_TEXTURE_VECTOR_DETOUR(name, type, original, overrideFn)                                        \
    void WINAPI name(GLuint texture, GLenum pname, const type* values) {                                      \
        ++g_parameterCalls;                                                                                   \
        if (!values || !IsControlledParameter(pname)) {                                                       \
            if (original)                                                                                     \
                original(texture, pname, values);                                                             \
            return;                                                                                           \
        }                                                                                                     \
        const type value = static_cast<type>(                                                                 \
            overrideFn(pname, values[0], GetActiveGraphicsConfigCached(), CurrentResolver()));               \
        if (original)                                                                                         \
            original(texture, pname, &value);                                                                 \
        ApplyTextureAF(texture, CurrentResolver());                                                          \
    }

DEFINE_TEXTURE_VECTOR_DETOUR(DetourTextureParameteriv, GLint, g_textureParameteriv, OverrideIntegerValue)
DEFINE_TEXTURE_VECTOR_DETOUR(DetourTextureParameterfv, GLfloat, g_textureParameterfv, OverrideFloatValue)
DEFINE_TEXTURE_VECTOR_DETOUR(DetourTextureParameterIiv, GLint, g_textureParameterIiv, OverrideIntegerValue)
DEFINE_TEXTURE_VECTOR_DETOUR(DetourTextureParameterIuiv, GLuint, g_textureParameterIuiv, OverrideIntegerValue)

#define DEFINE_TEXTURE_EXT_SCALAR_DETOUR(name, type, original, overrideFn)                                    \
    void WINAPI name(GLuint texture, GLenum target, GLenum pname, type value) {                               \
        ++g_parameterCalls;                                                                                   \
        const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();                                         \
        if (original)                                                                                         \
            original(texture, target, pname, overrideFn(pname, value, gfx, CurrentResolver()));              \
        if (IsControlledParameter(pname))                                                                     \
            ApplyTextureAFExt(texture, target, CurrentResolver());                                           \
    }

DEFINE_TEXTURE_EXT_SCALAR_DETOUR(DetourTextureParameteriExt, GLint, g_textureParameteriExt, OverrideIntegerValue)
DEFINE_TEXTURE_EXT_SCALAR_DETOUR(DetourTextureParameterfExt, GLfloat, g_textureParameterfExt, OverrideFloatValue)

#define DEFINE_TEXTURE_EXT_VECTOR_DETOUR(name, type, original, overrideFn)                                    \
    void WINAPI name(GLuint texture, GLenum target, GLenum pname, const type* values) {                       \
        ++g_parameterCalls;                                                                                   \
        if (!values || !IsControlledParameter(pname)) {                                                       \
            if (original)                                                                                     \
                original(texture, target, pname, values);                                                     \
            return;                                                                                           \
        }                                                                                                     \
        const type value = static_cast<type>(                                                                 \
            overrideFn(pname, values[0], GetActiveGraphicsConfigCached(), CurrentResolver()));               \
        if (original)                                                                                         \
            original(texture, target, pname, &value);                                                        \
        ApplyTextureAFExt(texture, target, CurrentResolver());                                               \
    }

DEFINE_TEXTURE_EXT_VECTOR_DETOUR(DetourTextureParameterivExt, GLint, g_textureParameterivExt, OverrideIntegerValue)
DEFINE_TEXTURE_EXT_VECTOR_DETOUR(DetourTextureParameterfvExt, GLfloat, g_textureParameterfvExt, OverrideFloatValue)
DEFINE_TEXTURE_EXT_VECTOR_DETOUR(DetourTextureParameterIivExt, GLint, g_textureParameterIivExt,
                                 OverrideIntegerValue)
DEFINE_TEXTURE_EXT_VECTOR_DETOUR(DetourTextureParameterIuivExt, GLuint, g_textureParameterIuivExt,
                                 OverrideIntegerValue)

}  // namespace

void Initialize() {
    LoadCoreQueries();
    if (HMODULE gl = GetModuleHandleA("opengl32.dll")) {
        g_defaultResolver = reinterpret_cast<ProcResolver>(GetProcAddress(gl, "wglGetProcAddress"));
    }
    IATHook::RegisterDynamicHook("glTexParameteri", reinterpret_cast<LPVOID>(&DetourTexParameteri),
                                 reinterpret_cast<LPVOID*>(&g_texParameteri));
    IATHook::PatchIATAllModules("opengl32.dll", "glTexParameteri", reinterpret_cast<LPVOID>(&DetourTexParameteri),
                                reinterpret_cast<LPVOID*>(&g_texParameteri));
    IATHook::RegisterDynamicHook("glTexParameterf", reinterpret_cast<LPVOID>(&DetourTexParameterf),
                                 reinterpret_cast<LPVOID*>(&g_texParameterf));
    IATHook::PatchIATAllModules("opengl32.dll", "glTexParameterf", reinterpret_cast<LPVOID>(&DetourTexParameterf),
                                reinterpret_cast<LPVOID*>(&g_texParameterf));
    IATHook::RegisterDynamicHook("glTexParameteriv", reinterpret_cast<LPVOID>(&DetourTexParameteriv),
                                 reinterpret_cast<LPVOID*>(&g_texParameteriv));
    IATHook::PatchIATAllModules("opengl32.dll", "glTexParameteriv", reinterpret_cast<LPVOID>(&DetourTexParameteriv),
                                reinterpret_cast<LPVOID*>(&g_texParameteriv));
    IATHook::RegisterDynamicHook("glTexParameterfv", reinterpret_cast<LPVOID>(&DetourTexParameterfv),
                                 reinterpret_cast<LPVOID*>(&g_texParameterfv));
    IATHook::PatchIATAllModules("opengl32.dll", "glTexParameterfv", reinterpret_cast<LPVOID>(&DetourTexParameterfv),
                                reinterpret_cast<LPVOID*>(&g_texParameterfv));
    ce::opengl_texture_storage_override::Initialize();
}

PROC InterceptProcAddress(const char* name, PROC original, ProcResolver resolver) {
    if (!name || !IsValidProc(original)) {
        return original;
    }
    t_resolver = resolver;
    if (!g_getSamplerParameteriv)
        g_getSamplerParameteriv = ResolveProc<GetSamplerParameterivFn>(resolver, "glGetSamplerParameteriv");
    if (!g_getTextureParameteriv)
        g_getTextureParameteriv = ResolveProc<GetTextureParameterivFn>(resolver, "glGetTextureParameteriv");
    if (!g_getTextureLevelParameteriv)
        g_getTextureLevelParameteriv =
            ResolveProc<GetTextureLevelParameterivFn>(resolver, "glGetTextureLevelParameteriv");
    if (!g_getTextureParameterivExt)
        g_getTextureParameterivExt = ResolveProc<GetTextureParameterivExtFn>(resolver, "glGetTextureParameterivEXT");
    if (!g_getTextureLevelParameterivExt)
        g_getTextureLevelParameterivExt =
            ResolveProc<GetTextureLevelParameterivExtFn>(resolver, "glGetTextureLevelParameterivEXT");
    if (!g_samplerParameterf)
        g_samplerParameterf = ResolveProc<SamplerParameterfFn>(resolver, "glSamplerParameterf");
    if (!g_textureParameterf)
        g_textureParameterf = ResolveProc<TextureParameterfFn>(resolver, "glTextureParameterf");
    if (!g_textureParameterfExt)
        g_textureParameterfExt = ResolveProc<TextureParameterfExtFn>(resolver, "glTextureParameterfEXT");

    const PROC storageProc = ce::opengl_texture_storage_override::InterceptProcAddress(name, original, resolver);
    if (storageProc != original) {
        return storageProc;
    }

#define INTERCEPT(procName, storage, type, detour)                \
    if (!std::strcmp(name, procName)) {                            \
        storage = std::bit_cast<type>(original);                   \
        return std::bit_cast<PROC>(&detour);                       \
    }

    INTERCEPT("glTexParameteri", g_texParameteri, TexParameteriFn, DetourTexParameteri)
    INTERCEPT("glTexParameterf", g_texParameterf, TexParameterfFn, DetourTexParameterf)
    INTERCEPT("glTexParameteriv", g_texParameteriv, TexParameterivFn, DetourTexParameteriv)
    INTERCEPT("glTexParameterfv", g_texParameterfv, TexParameterfvFn, DetourTexParameterfv)
    INTERCEPT("glSamplerParameteri", g_samplerParameteri, SamplerParameteriFn, DetourSamplerParameteri)
    INTERCEPT("glSamplerParameterf", g_samplerParameterf, SamplerParameterfFn, DetourSamplerParameterf)
    INTERCEPT("glSamplerParameteriv", g_samplerParameteriv, SamplerParameterivFn, DetourSamplerParameteriv)
    INTERCEPT("glSamplerParameterfv", g_samplerParameterfv, SamplerParameterfvFn, DetourSamplerParameterfv)
    INTERCEPT("glSamplerParameterIiv", g_samplerParameterIiv, SamplerParameterIivFn, DetourSamplerParameterIiv)
    INTERCEPT("glSamplerParameterIuiv", g_samplerParameterIuiv, SamplerParameterIuivFn, DetourSamplerParameterIuiv)
    INTERCEPT("glTextureParameteri", g_textureParameteri, TextureParameteriFn, DetourTextureParameteri)
    INTERCEPT("glTextureParameterf", g_textureParameterf, TextureParameterfFn, DetourTextureParameterf)
    INTERCEPT("glTextureParameteriv", g_textureParameteriv, TextureParameterivFn, DetourTextureParameteriv)
    INTERCEPT("glTextureParameterfv", g_textureParameterfv, TextureParameterfvFn, DetourTextureParameterfv)
    INTERCEPT("glTextureParameterIiv", g_textureParameterIiv, TextureParameterIivFn, DetourTextureParameterIiv)
    INTERCEPT("glTextureParameterIuiv", g_textureParameterIuiv, TextureParameterIuivFn, DetourTextureParameterIuiv)
    INTERCEPT("glTextureParameteriEXT", g_textureParameteriExt, TextureParameteriExtFn, DetourTextureParameteriExt)
    INTERCEPT("glTextureParameterfEXT", g_textureParameterfExt, TextureParameterfExtFn, DetourTextureParameterfExt)
    INTERCEPT("glTextureParameterivEXT", g_textureParameterivExt, TextureParameterivExtFn,
              DetourTextureParameterivExt)
    INTERCEPT("glTextureParameterfvEXT", g_textureParameterfvExt, TextureParameterfvExtFn,
              DetourTextureParameterfvExt)
    INTERCEPT("glTextureParameterIivEXT", g_textureParameterIivExt, TextureParameterIivExtFn,
              DetourTextureParameterIivExt)
    INTERCEPT("glTextureParameterIuivEXT", g_textureParameterIuivExt, TextureParameterIuivExtFn,
              DetourTextureParameterIuivExt)
#undef INTERCEPT
    return original;
}

void ReconcileBoundTexture(unsigned int target) {
    ApplyTargetAF(target, CurrentResolver());
}

void ReconcileTexture(unsigned int texture) {
    ApplyTextureAF(texture, CurrentResolver());
}

void ReconcileTextureExt(unsigned int texture, unsigned int target) {
    ApplyTextureAFExt(texture, target, CurrentResolver());
}

void ReconcileTextureView(unsigned int texture, unsigned int target) {
    if (g_getTextureParameteriv && g_getTextureLevelParameteriv && g_textureParameterf) {
        ApplyTextureAF(texture, CurrentResolver());
    } else {
        ApplyTextureAFExt(texture, target, CurrentResolver());
    }
}

void NotifyContextChanged() {
    t_caps = {};
    t_resolver = nullptr;
}

void Shutdown() {
    ce::opengl_texture_storage_override::Shutdown();
    HookLog("OpenGL: Sampler override summary parameterCalls=%llu afApplications=%llu unchanged=%llu "
            "safetyRestores=%llu",
            static_cast<unsigned long long>(g_parameterCalls.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_afApplications.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_afUnchanged.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(g_afSafetyRestores.load(std::memory_order_relaxed)));
}

}  // namespace ce::opengl_sampler_override
