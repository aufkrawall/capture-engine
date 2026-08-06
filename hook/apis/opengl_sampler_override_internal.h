#pragma once

#include <atomic>
#include <cstdint>
#include <unordered_set>

#include "../../common/mip_mapping_policy.h"
#include "../common/sampler_override_utils.h"
#include "../wrappers/iat_hook.h"
#include "hook_common.h"
#include "lod_helper.h"
#include "opengl_sampler_override.h"

namespace ce::opengl_sampler_override {

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
using BindTextureFn = void(WINAPI*)(GLenum, GLuint);
using BindSamplerFn = void(WINAPI*)(GLuint, GLuint);
using BindTextureUnitFn = void(WINAPI*)(GLuint, GLuint);
using BindTexturesFn = void(WINAPI*)(GLuint, GLsizei, const GLuint*);
using BindSamplersFn = void(WINAPI*)(GLuint, GLsizei, const GLuint*);
using DeleteTexturesFn = void(WINAPI*)(GLsizei, const GLuint*);
using DeleteSamplersFn = void(WINAPI*)(GLsizei, const GLuint*);
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
struct CapabilityState {
    HGLRC context = nullptr;
    bool initialized = false;
    bool anisotropySupported = false;
    float maxAnisotropy = 1.0f;
    int major = 1;
    int minor = 1;
};
struct BindReconcileCache {
    HGLRC context = nullptr;
    uint32_t configVersion = 0xFFFFFFFFu;
    uint64_t objectGeneration = 0;
    std::unordered_set<uint64_t> boundTextures;
    std::unordered_set<GLuint> textureObjects;
    std::unordered_set<GLuint> samplerObjects;
};
extern TexParameteriFn g_texParameteri;
extern TexParameterfFn g_texParameterf;
extern TexParameterivFn g_texParameteriv;
extern TexParameterfvFn g_texParameterfv;
extern GetTexParameterivFn g_getTexParameteriv;
extern GetTexLevelParameterivFn g_getTexLevelParameteriv;
extern GetFloatvFn g_getFloatv;
extern GetIntegervFn g_getIntegerv;
extern GetStringFn g_getString;
extern GetStringiFn g_getStringi;
extern BindTextureFn g_bindTexture;
extern BindSamplerFn g_bindSampler;
extern BindTextureUnitFn g_bindTextureUnit;
extern BindTexturesFn g_bindTextures;
extern BindSamplersFn g_bindSamplers;
extern DeleteTexturesFn g_deleteTextures;
extern DeleteSamplersFn g_deleteSamplers;
extern SamplerParameteriFn g_samplerParameteri;
extern SamplerParameterfFn g_samplerParameterf;
extern SamplerParameterivFn g_samplerParameteriv;
extern SamplerParameterfvFn g_samplerParameterfv;
extern SamplerParameterIivFn g_samplerParameterIiv;
extern SamplerParameterIuivFn g_samplerParameterIuiv;
extern GetSamplerParameterivFn g_getSamplerParameteriv;
extern TextureParameteriFn g_textureParameteri;
extern TextureParameterfFn g_textureParameterf;
extern TextureParameterivFn g_textureParameteriv;
extern TextureParameterfvFn g_textureParameterfv;
extern TextureParameterIivFn g_textureParameterIiv;
extern TextureParameterIuivFn g_textureParameterIuiv;
extern GetTextureParameterivFn g_getTextureParameteriv;
extern GetTextureLevelParameterivFn g_getTextureLevelParameteriv;
extern TextureParameteriExtFn g_textureParameteriExt;
extern TextureParameterfExtFn g_textureParameterfExt;
extern TextureParameterivExtFn g_textureParameterivExt;
extern TextureParameterfvExtFn g_textureParameterfvExt;
extern TextureParameterIivExtFn g_textureParameterIivExt;
extern TextureParameterIuivExtFn g_textureParameterIuivExt;
extern GetTextureParameterivExtFn g_getTextureParameterivExt;
extern GetTextureLevelParameterivExtFn g_getTextureLevelParameterivExt;
extern thread_local CapabilityState t_caps;
extern std::atomic<uint64_t> g_parameterCalls;
extern std::atomic<uint64_t> g_afApplications;
extern std::atomic<uint64_t> g_afUnchanged;
extern std::atomic<uint64_t> g_afSafetyRestores;
extern std::atomic<uint64_t> g_filterApplications;
extern std::atomic<uint64_t> g_filterUnchanged;
extern std::atomic<uint64_t> g_bindReconciliations;
extern std::atomic<uint64_t> g_objectGeneration;
extern std::atomic<int> g_decisionLogCount;
extern std::atomic<int> g_filterLogCount;
extern thread_local BindReconcileCache t_bindCache;
extern thread_local ProcResolver t_resolver;
extern ProcResolver g_defaultResolver;
bool IsValidProc(PROC proc);
bool ContainsExtension(const char* extensions, const char* wanted);
void LoadCoreQueries();
void EnsureCapabilities(ProcResolver resolver);
bool IsMipCapableTarget(GLenum target);
void SetTargetAddressDimensions(GLenum target, ce::sampler_override::OpenGLSamplerForcedAFInfo& info);
bool IsControlledParameter(GLenum pname);
GLint OverrideMinFilter(GLint value, const GraphicsConfig& gfx);
GLfloat OverrideFloatValue(GLenum pname, GLfloat value, const GraphicsConfig& gfx, ProcResolver);
GLint OverrideIntegerValue(GLenum pname, GLint value, const GraphicsConfig& gfx, ProcResolver resolver);
void QueryTargetInfo(GLenum target, ce::sampler_override::OpenGLSamplerForcedAFInfo& info);
void QuerySamplerInfo(GLuint sampler, ce::sampler_override::OpenGLSamplerForcedAFInfo& info);
void QueryTextureInfo(GLuint texture, ce::sampler_override::OpenGLSamplerForcedAFInfo& info);
void QueryTextureInfoExt(GLuint texture, GLenum target, ce::sampler_override::OpenGLSamplerForcedAFInfo& info);
float ResolveDesiredAF(const ce::sampler_override::OpenGLSamplerForcedAFInfo& info, const GraphicsConfig& gfx,
                       ce::sampler_override::OpenGLForcedAFDecision& decision);
void LogDecision(ce::sampler_override::OpenGLForcedAFDecision decision, float desired, const GraphicsConfig& gfx,
                 const char* objectKind);
void LogFilterDecision(const char* objectKind, GLint originalMin, GLint desiredMin, GLint originalMag,
                       GLint desiredMag, const GraphicsConfig& gfx);
void ResolveDesiredFilters(const ce::sampler_override::OpenGLSamplerForcedAFInfo& info, const GraphicsConfig& gfx,
                           GLint& desiredMin, GLint& desiredMag);
void ApplyTargetOverrides(GLenum target, ProcResolver resolver);
void ApplySamplerOverrides(GLuint sampler, ProcResolver resolver);
void ApplyTextureOverrides(GLuint texture, ProcResolver resolver);
void ApplyTextureOverridesExt(GLuint texture, GLenum target, ProcResolver resolver);
ProcResolver CurrentResolver();
template <typename T>
T ResolveProc(ProcResolver resolver, const char* name) {
    PROC proc = resolver ? resolver(name) : nullptr;
    return IsValidProc(proc) ? reinterpret_cast<T>(proc) : nullptr;
}

}  // namespace ce::opengl_sampler_override
