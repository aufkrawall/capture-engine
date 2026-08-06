#include "opengl_sampler_override_internal.h"
#include "opengl_texture_storage_override.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_set>

#include "../../common/mip_mapping_policy.h"
#include "../common/sampler_override_utils.h"
#include "../wrappers/iat_hook.h"
#include "hook_common.h"
#include "lod_helper.h"

namespace ce::opengl_sampler_override {

void WINAPI DetourTexParameteri(GLenum target, GLenum pname, GLint value) {
    ++g_parameterCalls;
    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    if (g_texParameteri)
        g_texParameteri(target, pname, OverrideIntegerValue(pname, value, gfx, CurrentResolver()));
    if (IsControlledParameter(pname))
        ApplyTargetOverrides(target, CurrentResolver());
}

void WINAPI DetourTexParameterf(GLenum target, GLenum pname, GLfloat value) {
    ++g_parameterCalls;
    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    if (g_texParameterf)
        g_texParameterf(target, pname, OverrideFloatValue(pname, value, gfx, CurrentResolver()));
    if (IsControlledParameter(pname))
        ApplyTargetOverrides(target, CurrentResolver());
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
    ApplyTargetOverrides(target, CurrentResolver());
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
    ApplyTargetOverrides(target, CurrentResolver());
}

#define DEFINE_SAMPLER_SCALAR_DETOUR(name, type, original, overrideFn)                  \
    void WINAPI name(GLuint sampler, GLenum pname, type value) {                        \
        ++g_parameterCalls;                                                             \
        const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();                    \
        if (original)                                                                   \
            original(sampler, pname, overrideFn(pname, value, gfx, CurrentResolver())); \
        if (IsControlledParameter(pname))                                               \
            ApplySamplerOverrides(sampler, CurrentResolver());                          \
    }

DEFINE_SAMPLER_SCALAR_DETOUR(DetourSamplerParameteri, GLint, g_samplerParameteri, OverrideIntegerValue)
DEFINE_SAMPLER_SCALAR_DETOUR(DetourSamplerParameterf, GLfloat, g_samplerParameterf, OverrideFloatValue)

#define DEFINE_SAMPLER_VECTOR_DETOUR(name, type, original, overrideFn)                                           \
    void WINAPI name(GLuint sampler, GLenum pname, const type* values) {                                         \
        ++g_parameterCalls;                                                                                      \
        if (!values || !IsControlledParameter(pname)) {                                                          \
            if (original)                                                                                        \
                original(sampler, pname, values);                                                                \
            return;                                                                                              \
        }                                                                                                        \
        const type value =                                                                                       \
            static_cast<type>(overrideFn(pname, values[0], GetActiveGraphicsConfigCached(), CurrentResolver())); \
        if (original)                                                                                            \
            original(sampler, pname, &value);                                                                    \
        ApplySamplerOverrides(sampler, CurrentResolver());                                                       \
    }

DEFINE_SAMPLER_VECTOR_DETOUR(DetourSamplerParameteriv, GLint, g_samplerParameteriv, OverrideIntegerValue)
DEFINE_SAMPLER_VECTOR_DETOUR(DetourSamplerParameterfv, GLfloat, g_samplerParameterfv, OverrideFloatValue)
DEFINE_SAMPLER_VECTOR_DETOUR(DetourSamplerParameterIiv, GLint, g_samplerParameterIiv, OverrideIntegerValue)
DEFINE_SAMPLER_VECTOR_DETOUR(DetourSamplerParameterIuiv, GLuint, g_samplerParameterIuiv, OverrideIntegerValue)

#define DEFINE_TEXTURE_SCALAR_DETOUR(name, type, original, overrideFn)                  \
    void WINAPI name(GLuint texture, GLenum pname, type value) {                        \
        ++g_parameterCalls;                                                             \
        const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();                    \
        if (original)                                                                   \
            original(texture, pname, overrideFn(pname, value, gfx, CurrentResolver())); \
        if (IsControlledParameter(pname))                                               \
            ApplyTextureOverrides(texture, CurrentResolver());                          \
    }

DEFINE_TEXTURE_SCALAR_DETOUR(DetourTextureParameteri, GLint, g_textureParameteri, OverrideIntegerValue)
DEFINE_TEXTURE_SCALAR_DETOUR(DetourTextureParameterf, GLfloat, g_textureParameterf, OverrideFloatValue)

#define DEFINE_TEXTURE_VECTOR_DETOUR(name, type, original, overrideFn)                                           \
    void WINAPI name(GLuint texture, GLenum pname, const type* values) {                                         \
        ++g_parameterCalls;                                                                                      \
        if (!values || !IsControlledParameter(pname)) {                                                          \
            if (original)                                                                                        \
                original(texture, pname, values);                                                                \
            return;                                                                                              \
        }                                                                                                        \
        const type value =                                                                                       \
            static_cast<type>(overrideFn(pname, values[0], GetActiveGraphicsConfigCached(), CurrentResolver())); \
        if (original)                                                                                            \
            original(texture, pname, &value);                                                                    \
        ApplyTextureOverrides(texture, CurrentResolver());                                                       \
    }

DEFINE_TEXTURE_VECTOR_DETOUR(DetourTextureParameteriv, GLint, g_textureParameteriv, OverrideIntegerValue)
DEFINE_TEXTURE_VECTOR_DETOUR(DetourTextureParameterfv, GLfloat, g_textureParameterfv, OverrideFloatValue)
DEFINE_TEXTURE_VECTOR_DETOUR(DetourTextureParameterIiv, GLint, g_textureParameterIiv, OverrideIntegerValue)
DEFINE_TEXTURE_VECTOR_DETOUR(DetourTextureParameterIuiv, GLuint, g_textureParameterIuiv, OverrideIntegerValue)

#define DEFINE_TEXTURE_EXT_SCALAR_DETOUR(name, type, original, overrideFn)                      \
    void WINAPI name(GLuint texture, GLenum target, GLenum pname, type value) {                 \
        ++g_parameterCalls;                                                                     \
        const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();                            \
        if (original)                                                                           \
            original(texture, target, pname, overrideFn(pname, value, gfx, CurrentResolver())); \
        if (IsControlledParameter(pname))                                                       \
            ApplyTextureOverridesExt(texture, target, CurrentResolver());                       \
    }

DEFINE_TEXTURE_EXT_SCALAR_DETOUR(DetourTextureParameteriExt, GLint, g_textureParameteriExt, OverrideIntegerValue)
DEFINE_TEXTURE_EXT_SCALAR_DETOUR(DetourTextureParameterfExt, GLfloat, g_textureParameterfExt, OverrideFloatValue)

#define DEFINE_TEXTURE_EXT_VECTOR_DETOUR(name, type, original, overrideFn)                                       \
    void WINAPI name(GLuint texture, GLenum target, GLenum pname, const type* values) {                          \
        ++g_parameterCalls;                                                                                      \
        if (!values || !IsControlledParameter(pname)) {                                                          \
            if (original)                                                                                        \
                original(texture, target, pname, values);                                                        \
            return;                                                                                              \
        }                                                                                                        \
        const type value =                                                                                       \
            static_cast<type>(overrideFn(pname, values[0], GetActiveGraphicsConfigCached(), CurrentResolver())); \
        if (original)                                                                                            \
            original(texture, target, pname, &value);                                                            \
        ApplyTextureOverridesExt(texture, target, CurrentResolver());                                            \
    }

DEFINE_TEXTURE_EXT_VECTOR_DETOUR(DetourTextureParameterivExt, GLint, g_textureParameterivExt, OverrideIntegerValue)
DEFINE_TEXTURE_EXT_VECTOR_DETOUR(DetourTextureParameterfvExt, GLfloat, g_textureParameterfvExt, OverrideFloatValue)
DEFINE_TEXTURE_EXT_VECTOR_DETOUR(DetourTextureParameterIivExt, GLint, g_textureParameterIivExt, OverrideIntegerValue)
DEFINE_TEXTURE_EXT_VECTOR_DETOUR(DetourTextureParameterIuivExt, GLuint, g_textureParameterIuivExt, OverrideIntegerValue)

bool BindReconciliationEnabled() {
    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    return ce::mip_mapping::IsExplicit(ce::mip_mapping::ParseMode(gfx.mipMapping)) ||
           (!gfx.anisotropicFiltering.empty() && gfx.anisotropicFiltering != "default");
}

void RefreshBindCacheIdentity() {
    const HGLRC context = wglGetCurrentContext();
    const uint32_t configVersion = GetActiveGraphicsConfigVersion();
    const uint64_t objectGeneration = g_objectGeneration.load(std::memory_order_acquire);
    if (t_bindCache.context == context && t_bindCache.configVersion == configVersion &&
        t_bindCache.objectGeneration == objectGeneration)
        return;
    t_bindCache = {};
    t_bindCache.context = context;
    t_bindCache.configVersion = configVersion;
    t_bindCache.objectGeneration = objectGeneration;
}

bool MarkBoundTextureForReconcile(GLenum target, GLuint texture) {
    if (!BindReconciliationEnabled() || !IsMipCapableTarget(target))
        return false;
    RefreshBindCacheIdentity();
    const uint64_t key = (static_cast<uint64_t>(target) << 32) | texture;
    return t_bindCache.boundTextures.insert(key).second;
}

bool MarkTextureObjectForReconcile(GLuint texture) {
    if (!texture || !BindReconciliationEnabled())
        return false;
    RefreshBindCacheIdentity();
    return t_bindCache.textureObjects.insert(texture).second;
}

bool MarkSamplerObjectForReconcile(GLuint sampler) {
    if (!sampler || !BindReconciliationEnabled())
        return false;
    RefreshBindCacheIdentity();
    return t_bindCache.samplerObjects.insert(sampler).second;
}

void WINAPI DetourBindTexture(GLenum target, GLuint texture) {
    if (g_bindTexture)
        g_bindTexture(target, texture);
    if (MarkBoundTextureForReconcile(target, texture)) {
        g_bindReconciliations.fetch_add(1, std::memory_order_relaxed);
        ApplyTargetOverrides(target, CurrentResolver());
    }
}

void WINAPI DetourBindSampler(GLuint unit, GLuint sampler) {
    if (g_bindSampler)
        g_bindSampler(unit, sampler);
    if (MarkSamplerObjectForReconcile(sampler)) {
        g_bindReconciliations.fetch_add(1, std::memory_order_relaxed);
        ApplySamplerOverrides(sampler, CurrentResolver());
    }
}

void WINAPI DetourBindTextureUnit(GLuint unit, GLuint texture) {
    if (g_bindTextureUnit)
        g_bindTextureUnit(unit, texture);
    if (MarkTextureObjectForReconcile(texture)) {
        g_bindReconciliations.fetch_add(1, std::memory_order_relaxed);
        ApplyTextureOverrides(texture, CurrentResolver());
    }
}

void WINAPI DetourBindTextures(GLuint first, GLsizei count, const GLuint* textures) {
    if (g_bindTextures)
        g_bindTextures(first, count, textures);
    if (!textures || count <= 0)
        return;
    for (GLsizei i = 0; i < count; ++i) {
        if (MarkTextureObjectForReconcile(textures[i])) {
            g_bindReconciliations.fetch_add(1, std::memory_order_relaxed);
            ApplyTextureOverrides(textures[i], CurrentResolver());
        }
    }
}

void WINAPI DetourBindSamplers(GLuint first, GLsizei count, const GLuint* samplers) {
    if (g_bindSamplers)
        g_bindSamplers(first, count, samplers);
    if (!samplers || count <= 0)
        return;
    for (GLsizei i = 0; i < count; ++i) {
        if (MarkSamplerObjectForReconcile(samplers[i])) {
            g_bindReconciliations.fetch_add(1, std::memory_order_relaxed);
            ApplySamplerOverrides(samplers[i], CurrentResolver());
        }
    }
}

void WINAPI DetourDeleteTextures(GLsizei count, const GLuint* textures) {
    if (g_deleteTextures)
        g_deleteTextures(count, textures);
    if (count > 0 && textures)
        g_objectGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void WINAPI DetourDeleteSamplers(GLsizei count, const GLuint* samplers) {
    if (g_deleteSamplers)
        g_deleteSamplers(count, samplers);
    if (count > 0 && samplers)
        g_objectGeneration.fetch_add(1, std::memory_order_acq_rel);
}

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
    IATHook::RegisterDynamicHook("glBindTexture", reinterpret_cast<LPVOID>(&DetourBindTexture),
                                 reinterpret_cast<LPVOID*>(&g_bindTexture));
    IATHook::PatchIATAllModules("opengl32.dll", "glBindTexture", reinterpret_cast<LPVOID>(&DetourBindTexture),
                                reinterpret_cast<LPVOID*>(&g_bindTexture));
    IATHook::RegisterDynamicHook("glDeleteTextures", reinterpret_cast<LPVOID>(&DetourDeleteTextures),
                                 reinterpret_cast<LPVOID*>(&g_deleteTextures));
    IATHook::PatchIATAllModules("opengl32.dll", "glDeleteTextures", reinterpret_cast<LPVOID>(&DetourDeleteTextures),
                                reinterpret_cast<LPVOID*>(&g_deleteTextures));
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
    if (!g_samplerParameteri)
        g_samplerParameteri = ResolveProc<SamplerParameteriFn>(resolver, "glSamplerParameteri");
    if (!g_textureParameterf)
        g_textureParameterf = ResolveProc<TextureParameterfFn>(resolver, "glTextureParameterf");
    if (!g_textureParameteri)
        g_textureParameteri = ResolveProc<TextureParameteriFn>(resolver, "glTextureParameteri");
    if (!g_textureParameterfExt)
        g_textureParameterfExt = ResolveProc<TextureParameterfExtFn>(resolver, "glTextureParameterfEXT");
    if (!g_textureParameteriExt)
        g_textureParameteriExt = ResolveProc<TextureParameteriExtFn>(resolver, "glTextureParameteriEXT");

    const PROC storageProc = ce::opengl_texture_storage_override::InterceptProcAddress(name, original, resolver);
    if (storageProc != original) {
        return storageProc;
    }

#define INTERCEPT(procName, storage, type, detour) \
    if (!std::strcmp(name, procName)) {            \
        /* NOLINTNEXTLINE(bugprone-macro-parentheses) - type is a template argument */ \
        storage = reinterpret_cast<type>(original); \
        /* NOLINTNEXTLINE(bugprone-macro-parentheses) - PROC is a type name */ \
        return reinterpret_cast<PROC>(&detour);     \
    }

    INTERCEPT("glTexParameteri", g_texParameteri, TexParameteriFn, DetourTexParameteri)
    INTERCEPT("glTexParameterf", g_texParameterf, TexParameterfFn, DetourTexParameterf)
    INTERCEPT("glTexParameteriv", g_texParameteriv, TexParameterivFn, DetourTexParameteriv)
    INTERCEPT("glTexParameterfv", g_texParameterfv, TexParameterfvFn, DetourTexParameterfv)
    INTERCEPT("glBindTexture", g_bindTexture, BindTextureFn, DetourBindTexture)
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
    INTERCEPT("glTextureParameterivEXT", g_textureParameterivExt, TextureParameterivExtFn, DetourTextureParameterivExt)
    INTERCEPT("glTextureParameterfvEXT", g_textureParameterfvExt, TextureParameterfvExtFn, DetourTextureParameterfvExt)
    INTERCEPT("glTextureParameterIivEXT", g_textureParameterIivExt, TextureParameterIivExtFn,
              DetourTextureParameterIivExt)
    INTERCEPT("glTextureParameterIuivEXT", g_textureParameterIuivExt, TextureParameterIuivExtFn,
              DetourTextureParameterIuivExt)
    INTERCEPT("glBindSampler", g_bindSampler, BindSamplerFn, DetourBindSampler)
    INTERCEPT("glBindTextureUnit", g_bindTextureUnit, BindTextureUnitFn, DetourBindTextureUnit)
    INTERCEPT("glBindTextures", g_bindTextures, BindTexturesFn, DetourBindTextures)
    INTERCEPT("glBindSamplers", g_bindSamplers, BindSamplersFn, DetourBindSamplers)
    INTERCEPT("glDeleteTextures", g_deleteTextures, DeleteTexturesFn, DetourDeleteTextures)
    INTERCEPT("glDeleteSamplers", g_deleteSamplers, DeleteSamplersFn, DetourDeleteSamplers)
#undef INTERCEPT
    return original;
}

void ReconcileBoundTexture(unsigned int target) {
    ApplyTargetOverrides(target, CurrentResolver());
}

void ReconcileTexture(unsigned int texture) {
    ApplyTextureOverrides(texture, CurrentResolver());
}

void ReconcileTextureExt(unsigned int texture, unsigned int target) {
    ApplyTextureOverridesExt(texture, target, CurrentResolver());
}

void ReconcileTextureView(unsigned int texture, unsigned int target) {
    if (g_getTextureParameteriv && g_getTextureLevelParameteriv && g_textureParameteri) {
        ApplyTextureOverrides(texture, CurrentResolver());
    } else {
        ApplyTextureOverridesExt(texture, target, CurrentResolver());
    }
}

void NotifyContextChanged() {
    t_caps = {};
    t_resolver = nullptr;
    t_bindCache = {};
}

void Shutdown() {
    ce::opengl_texture_storage_override::Shutdown();
    HookLog(
        "OpenGL: Sampler override summary parameterCalls=%llu filterApplications=%llu filterUnchanged=%llu "
        "bindReconciliations=%llu objectInvalidations=%llu afApplications=%llu afUnchanged=%llu safetyRestores=%llu",
        static_cast<unsigned long long>(g_parameterCalls.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_filterApplications.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_filterUnchanged.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_bindReconciliations.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_objectGeneration.load(std::memory_order_relaxed) - 1),
        static_cast<unsigned long long>(g_afApplications.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_afUnchanged.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_afSafetyRestores.load(std::memory_order_relaxed)));
}

}  // namespace ce::opengl_sampler_override
