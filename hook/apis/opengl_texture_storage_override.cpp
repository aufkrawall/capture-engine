#include "opengl_texture_storage_override.h"

#include <atomic>
#include <bit>
#include <cstring>

#include "../wrappers/iat_hook.h"
#include "hook_common.h"

namespace ce::opengl_texture_storage_override {
namespace {

using GLenum = unsigned int;
using GLint = int;
using GLsizei = int;
using GLuint = unsigned int;

using TexImage1DFn = void(WINAPI*)(GLenum, GLint, GLint, GLsizei, GLint, GLenum, GLenum, const void*);
using TexImage2DFn = void(WINAPI*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
using TexImage3DFn = void(WINAPI*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
using CompressedTexImage1DFn = void(WINAPI*)(GLenum, GLint, GLenum, GLsizei, GLint, GLsizei, const void*);
using CompressedTexImage2DFn = void(WINAPI*)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*);
using CompressedTexImage3DFn = void(WINAPI*)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLsizei, GLint, GLsizei,
                                             const void*);
using CopyTexImage1DFn = void(WINAPI*)(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLint);
using CopyTexImage2DFn = void(WINAPI*)(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint);

using TexStorage1DFn = void(WINAPI*)(GLenum, GLsizei, GLenum, GLsizei);
using TexStorage2DFn = void(WINAPI*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
using TexStorage3DFn = void(WINAPI*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei);
using TextureStorage1DFn = void(WINAPI*)(GLuint, GLsizei, GLenum, GLsizei);
using TextureStorage2DFn = void(WINAPI*)(GLuint, GLsizei, GLenum, GLsizei, GLsizei);
using TextureStorage3DFn = void(WINAPI*)(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLsizei);
using TextureStorage1DExtFn = void(WINAPI*)(GLuint, GLenum, GLsizei, GLenum, GLsizei);
using TextureStorage2DExtFn = void(WINAPI*)(GLuint, GLenum, GLsizei, GLenum, GLsizei, GLsizei);
using TextureStorage3DExtFn = void(WINAPI*)(GLuint, GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei);

using GenerateMipmapFn = void(WINAPI*)(GLenum);
using GenerateTextureMipmapFn = void(WINAPI*)(GLuint);
using GenerateTextureMipmapExtFn = void(WINAPI*)(GLuint, GLenum);
using TextureViewFn = void(WINAPI*)(GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint);

TexImage1DFn g_texImage1D = nullptr;
TexImage2DFn g_texImage2D = nullptr;
TexImage3DFn g_texImage3D = nullptr;
CompressedTexImage1DFn g_compressedTexImage1D = nullptr;
CompressedTexImage2DFn g_compressedTexImage2D = nullptr;
CompressedTexImage3DFn g_compressedTexImage3D = nullptr;
CopyTexImage1DFn g_copyTexImage1D = nullptr;
CopyTexImage2DFn g_copyTexImage2D = nullptr;
TexStorage1DFn g_texStorage1D = nullptr;
TexStorage2DFn g_texStorage2D = nullptr;
TexStorage3DFn g_texStorage3D = nullptr;
TextureStorage1DFn g_textureStorage1D = nullptr;
TextureStorage2DFn g_textureStorage2D = nullptr;
TextureStorage3DFn g_textureStorage3D = nullptr;
TextureStorage1DExtFn g_textureStorage1DExt = nullptr;
TextureStorage2DExtFn g_textureStorage2DExt = nullptr;
TextureStorage3DExtFn g_textureStorage3DExt = nullptr;
GenerateMipmapFn g_generateMipmap = nullptr;
GenerateMipmapFn g_generateMipmapExt = nullptr;
GenerateTextureMipmapFn g_generateTextureMipmap = nullptr;
GenerateTextureMipmapExtFn g_generateTextureMipmapExt = nullptr;
TextureViewFn g_textureView = nullptr;

std::atomic<uint64_t> g_storageEvents{0};

void ReconcileBoundMipChain(GLenum target, GLint level) {
    if (level == 0 || level == 1) {
        if (target >= 0x8515 && target <= 0x851A) {
            target = 0x8513;
        }
        g_storageEvents.fetch_add(1, std::memory_order_relaxed);
        ce::opengl_sampler_override::ReconcileBoundTexture(target);
    }
}

void WINAPI DetourTexImage1D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLint border,
                             GLenum format, GLenum type, const void* pixels) {
    if (g_texImage1D)
        g_texImage1D(target, level, internalFormat, width, border, format, type, pixels);
    ReconcileBoundMipChain(target, level);
}

void WINAPI DetourTexImage2D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height,
                             GLint border, GLenum format, GLenum type, const void* pixels) {
    if (g_texImage2D)
        g_texImage2D(target, level, internalFormat, width, height, border, format, type, pixels);
    ReconcileBoundMipChain(target, level);
}

void WINAPI DetourTexImage3D(GLenum target, GLint level, GLint internalFormat, GLsizei width, GLsizei height,
                             GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels) {
    if (g_texImage3D)
        g_texImage3D(target, level, internalFormat, width, height, depth, border, format, type, pixels);
    ReconcileBoundMipChain(target, level);
}

void WINAPI DetourCompressedTexImage1D(GLenum target, GLint level, GLenum internalFormat, GLsizei width, GLint border,
                                       GLsizei imageSize, const void* data) {
    if (g_compressedTexImage1D)
        g_compressedTexImage1D(target, level, internalFormat, width, border, imageSize, data);
    ReconcileBoundMipChain(target, level);
}

void WINAPI DetourCompressedTexImage2D(GLenum target, GLint level, GLenum internalFormat, GLsizei width, GLsizei height,
                                       GLint border, GLsizei imageSize, const void* data) {
    if (g_compressedTexImage2D)
        g_compressedTexImage2D(target, level, internalFormat, width, height, border, imageSize, data);
    ReconcileBoundMipChain(target, level);
}

void WINAPI DetourCompressedTexImage3D(GLenum target, GLint level, GLenum internalFormat, GLsizei width, GLsizei height,
                                       GLsizei depth, GLint border, GLsizei imageSize, const void* data) {
    if (g_compressedTexImage3D)
        g_compressedTexImage3D(target, level, internalFormat, width, height, depth, border, imageSize, data);
    ReconcileBoundMipChain(target, level);
}

void WINAPI DetourCopyTexImage1D(GLenum target, GLint level, GLenum internalFormat, GLint x, GLint y, GLsizei width,
                                 GLint border) {
    if (g_copyTexImage1D)
        g_copyTexImage1D(target, level, internalFormat, x, y, width, border);
    ReconcileBoundMipChain(target, level);
}

void WINAPI DetourCopyTexImage2D(GLenum target, GLint level, GLenum internalFormat, GLint x, GLint y, GLsizei width,
                                 GLsizei height, GLint border) {
    if (g_copyTexImage2D)
        g_copyTexImage2D(target, level, internalFormat, x, y, width, height, border);
    ReconcileBoundMipChain(target, level);
}

#define DEFINE_BOUND_STORAGE_DETOUR_1D(name, original)                              \
    void WINAPI name(GLenum target, GLsizei levels, GLenum format, GLsizei width) { \
        if (original)                                                               \
            original(target, levels, format, width);                                \
        if (levels > 1) {                                                           \
            g_storageEvents.fetch_add(1, std::memory_order_relaxed);                \
            ce::opengl_sampler_override::ReconcileBoundTexture(target);             \
        }                                                                           \
    }

#define DEFINE_BOUND_STORAGE_DETOUR_2D(name, original)                                              \
    void WINAPI name(GLenum target, GLsizei levels, GLenum format, GLsizei width, GLsizei height) { \
        if (original)                                                                               \
            original(target, levels, format, width, height);                                        \
        if (levels > 1) {                                                                           \
            g_storageEvents.fetch_add(1, std::memory_order_relaxed);                                \
            ce::opengl_sampler_override::ReconcileBoundTexture(target);                             \
        }                                                                                           \
    }

#define DEFINE_BOUND_STORAGE_DETOUR_3D(name, original)                                                             \
    void WINAPI name(GLenum target, GLsizei levels, GLenum format, GLsizei width, GLsizei height, GLsizei depth) { \
        if (original)                                                                                              \
            original(target, levels, format, width, height, depth);                                                \
        if (levels > 1) {                                                                                          \
            g_storageEvents.fetch_add(1, std::memory_order_relaxed);                                               \
            ce::opengl_sampler_override::ReconcileBoundTexture(target);                                            \
        }                                                                                                          \
    }

DEFINE_BOUND_STORAGE_DETOUR_1D(DetourTexStorage1D, g_texStorage1D)
DEFINE_BOUND_STORAGE_DETOUR_2D(DetourTexStorage2D, g_texStorage2D)
DEFINE_BOUND_STORAGE_DETOUR_3D(DetourTexStorage3D, g_texStorage3D)

#define DEFINE_TEXTURE_STORAGE_DETOUR_1D(name, original)                             \
    void WINAPI name(GLuint texture, GLsizei levels, GLenum format, GLsizei width) { \
        if (original)                                                                \
            original(texture, levels, format, width);                                \
        if (levels > 1) {                                                            \
            g_storageEvents.fetch_add(1, std::memory_order_relaxed);                 \
            ce::opengl_sampler_override::ReconcileTexture(texture);                  \
        }                                                                            \
    }

#define DEFINE_TEXTURE_STORAGE_DETOUR_2D(name, original)                                             \
    void WINAPI name(GLuint texture, GLsizei levels, GLenum format, GLsizei width, GLsizei height) { \
        if (original)                                                                                \
            original(texture, levels, format, width, height);                                        \
        if (levels > 1) {                                                                            \
            g_storageEvents.fetch_add(1, std::memory_order_relaxed);                                 \
            ce::opengl_sampler_override::ReconcileTexture(texture);                                  \
        }                                                                                            \
    }

#define DEFINE_TEXTURE_STORAGE_DETOUR_3D(name, original)                                                            \
    void WINAPI name(GLuint texture, GLsizei levels, GLenum format, GLsizei width, GLsizei height, GLsizei depth) { \
        if (original)                                                                                               \
            original(texture, levels, format, width, height, depth);                                                \
        if (levels > 1) {                                                                                           \
            g_storageEvents.fetch_add(1, std::memory_order_relaxed);                                                \
            ce::opengl_sampler_override::ReconcileTexture(texture);                                                 \
        }                                                                                                           \
    }

DEFINE_TEXTURE_STORAGE_DETOUR_1D(DetourTextureStorage1D, g_textureStorage1D)
DEFINE_TEXTURE_STORAGE_DETOUR_2D(DetourTextureStorage2D, g_textureStorage2D)
DEFINE_TEXTURE_STORAGE_DETOUR_3D(DetourTextureStorage3D, g_textureStorage3D)

#define DEFINE_TEXTURE_STORAGE_EXT_DETOUR_1D(name, original)                                        \
    void WINAPI name(GLuint texture, GLenum target, GLsizei levels, GLenum format, GLsizei width) { \
        if (original)                                                                               \
            original(texture, target, levels, format, width);                                       \
        if (levels > 1) {                                                                           \
            g_storageEvents.fetch_add(1, std::memory_order_relaxed);                                \
            ce::opengl_sampler_override::ReconcileTextureExt(texture, target);                      \
        }                                                                                           \
    }

#define DEFINE_TEXTURE_STORAGE_EXT_DETOUR_2D(name, original)                                                        \
    void WINAPI name(GLuint texture, GLenum target, GLsizei levels, GLenum format, GLsizei width, GLsizei height) { \
        if (original)                                                                                               \
            original(texture, target, levels, format, width, height);                                               \
        if (levels > 1) {                                                                                           \
            g_storageEvents.fetch_add(1, std::memory_order_relaxed);                                                \
            ce::opengl_sampler_override::ReconcileTextureExt(texture, target);                                      \
        }                                                                                                           \
    }

#define DEFINE_TEXTURE_STORAGE_EXT_DETOUR_3D(name, original)                                                      \
    void WINAPI name(GLuint texture, GLenum target, GLsizei levels, GLenum format, GLsizei width, GLsizei height, \
                     GLsizei depth) {                                                                             \
        if (original)                                                                                             \
            original(texture, target, levels, format, width, height, depth);                                      \
        if (levels > 1) {                                                                                         \
            g_storageEvents.fetch_add(1, std::memory_order_relaxed);                                              \
            ce::opengl_sampler_override::ReconcileTextureExt(texture, target);                                    \
        }                                                                                                         \
    }

DEFINE_TEXTURE_STORAGE_EXT_DETOUR_1D(DetourTextureStorage1DExt, g_textureStorage1DExt)
DEFINE_TEXTURE_STORAGE_EXT_DETOUR_2D(DetourTextureStorage2DExt, g_textureStorage2DExt)
DEFINE_TEXTURE_STORAGE_EXT_DETOUR_3D(DetourTextureStorage3DExt, g_textureStorage3DExt)

void WINAPI DetourGenerateMipmap(GLenum target) {
    if (g_generateMipmap)
        g_generateMipmap(target);
    g_storageEvents.fetch_add(1, std::memory_order_relaxed);
    ce::opengl_sampler_override::ReconcileBoundTexture(target);
}

void WINAPI DetourGenerateMipmapExt(GLenum target) {
    if (g_generateMipmapExt)
        g_generateMipmapExt(target);
    g_storageEvents.fetch_add(1, std::memory_order_relaxed);
    ce::opengl_sampler_override::ReconcileBoundTexture(target);
}

void WINAPI DetourGenerateTextureMipmap(GLuint texture) {
    if (g_generateTextureMipmap)
        g_generateTextureMipmap(texture);
    g_storageEvents.fetch_add(1, std::memory_order_relaxed);
    ce::opengl_sampler_override::ReconcileTexture(texture);
}

void WINAPI DetourGenerateTextureMipmapExt(GLuint texture, GLenum target) {
    if (g_generateTextureMipmapExt)
        g_generateTextureMipmapExt(texture, target);
    g_storageEvents.fetch_add(1, std::memory_order_relaxed);
    ce::opengl_sampler_override::ReconcileTextureExt(texture, target);
}

void WINAPI DetourTextureView(GLuint texture, GLenum target, GLuint sourceTexture, GLenum internalFormat,
                              GLuint minLevel, GLuint levelCount, GLuint minLayer, GLuint layerCount) {
    if (g_textureView) {
        g_textureView(texture, target, sourceTexture, internalFormat, minLevel, levelCount, minLayer, layerCount);
    }
    if (levelCount > 1) {
        g_storageEvents.fetch_add(1, std::memory_order_relaxed);
        ce::opengl_sampler_override::ReconcileTextureView(texture, target);
    }
}

}  // namespace

void Initialize() {
    IATHook::RegisterDynamicHook("glTexImage1D", reinterpret_cast<LPVOID>(&DetourTexImage1D),
                                 reinterpret_cast<LPVOID*>(&g_texImage1D));
    IATHook::PatchIATAllModules("opengl32.dll", "glTexImage1D", reinterpret_cast<LPVOID>(&DetourTexImage1D),
                                reinterpret_cast<LPVOID*>(&g_texImage1D));
    IATHook::RegisterDynamicHook("glTexImage2D", reinterpret_cast<LPVOID>(&DetourTexImage2D),
                                 reinterpret_cast<LPVOID*>(&g_texImage2D));
    IATHook::PatchIATAllModules("opengl32.dll", "glTexImage2D", reinterpret_cast<LPVOID>(&DetourTexImage2D),
                                reinterpret_cast<LPVOID*>(&g_texImage2D));
    IATHook::RegisterDynamicHook("glCopyTexImage1D", reinterpret_cast<LPVOID>(&DetourCopyTexImage1D),
                                 reinterpret_cast<LPVOID*>(&g_copyTexImage1D));
    IATHook::PatchIATAllModules("opengl32.dll", "glCopyTexImage1D", reinterpret_cast<LPVOID>(&DetourCopyTexImage1D),
                                reinterpret_cast<LPVOID*>(&g_copyTexImage1D));
    IATHook::RegisterDynamicHook("glCopyTexImage2D", reinterpret_cast<LPVOID>(&DetourCopyTexImage2D),
                                 reinterpret_cast<LPVOID*>(&g_copyTexImage2D));
    IATHook::PatchIATAllModules("opengl32.dll", "glCopyTexImage2D", reinterpret_cast<LPVOID>(&DetourCopyTexImage2D),
                                reinterpret_cast<LPVOID*>(&g_copyTexImage2D));
}

PROC InterceptProcAddress(const char* name, PROC original, ce::opengl_sampler_override::ProcResolver) {
    if (!name || !original) {
        return original;
    }

#define INTERCEPT(procName, storage, type, detour) \
    if (!std::strcmp(name, procName)) {            \
        /* NOLINTNEXTLINE(bugprone-macro-parentheses) - type is a template argument */ \
        storage = reinterpret_cast<type>(original); \
        /* NOLINTNEXTLINE(bugprone-macro-parentheses) - PROC is a type name */ \
        return reinterpret_cast<PROC>(&detour);     \
    }

    INTERCEPT("glTexImage1D", g_texImage1D, TexImage1DFn, DetourTexImage1D)
    INTERCEPT("glTexImage2D", g_texImage2D, TexImage2DFn, DetourTexImage2D)
    INTERCEPT("glTexImage3D", g_texImage3D, TexImage3DFn, DetourTexImage3D)
    INTERCEPT("glCompressedTexImage1D", g_compressedTexImage1D, CompressedTexImage1DFn, DetourCompressedTexImage1D)
    INTERCEPT("glCompressedTexImage2D", g_compressedTexImage2D, CompressedTexImage2DFn, DetourCompressedTexImage2D)
    INTERCEPT("glCompressedTexImage3D", g_compressedTexImage3D, CompressedTexImage3DFn, DetourCompressedTexImage3D)
    INTERCEPT("glCopyTexImage1D", g_copyTexImage1D, CopyTexImage1DFn, DetourCopyTexImage1D)
    INTERCEPT("glCopyTexImage2D", g_copyTexImage2D, CopyTexImage2DFn, DetourCopyTexImage2D)
    INTERCEPT("glTexStorage1D", g_texStorage1D, TexStorage1DFn, DetourTexStorage1D)
    INTERCEPT("glTexStorage2D", g_texStorage2D, TexStorage2DFn, DetourTexStorage2D)
    INTERCEPT("glTexStorage3D", g_texStorage3D, TexStorage3DFn, DetourTexStorage3D)
    INTERCEPT("glTextureStorage1D", g_textureStorage1D, TextureStorage1DFn, DetourTextureStorage1D)
    INTERCEPT("glTextureStorage2D", g_textureStorage2D, TextureStorage2DFn, DetourTextureStorage2D)
    INTERCEPT("glTextureStorage3D", g_textureStorage3D, TextureStorage3DFn, DetourTextureStorage3D)
    INTERCEPT("glTextureStorage1DEXT", g_textureStorage1DExt, TextureStorage1DExtFn, DetourTextureStorage1DExt)
    INTERCEPT("glTextureStorage2DEXT", g_textureStorage2DExt, TextureStorage2DExtFn, DetourTextureStorage2DExt)
    INTERCEPT("glTextureStorage3DEXT", g_textureStorage3DExt, TextureStorage3DExtFn, DetourTextureStorage3DExt)
    INTERCEPT("glGenerateMipmap", g_generateMipmap, GenerateMipmapFn, DetourGenerateMipmap)
    INTERCEPT("glGenerateMipmapEXT", g_generateMipmapExt, GenerateMipmapFn, DetourGenerateMipmapExt)
    INTERCEPT("glGenerateTextureMipmap", g_generateTextureMipmap, GenerateTextureMipmapFn, DetourGenerateTextureMipmap)
    INTERCEPT("glGenerateTextureMipmapEXT", g_generateTextureMipmapExt, GenerateTextureMipmapExtFn,
              DetourGenerateTextureMipmapExt)
    INTERCEPT("glTextureView", g_textureView, TextureViewFn, DetourTextureView)
#undef INTERCEPT
    return original;
}

void Shutdown() {
    HookLog("OpenGL: Texture storage AF triggers=%llu",
            static_cast<unsigned long long>(g_storageEvents.load(std::memory_order_relaxed)));
}

}  // namespace ce::opengl_texture_storage_override
