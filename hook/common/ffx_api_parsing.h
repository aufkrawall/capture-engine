#pragma once

#include <cstdint>
#include <cstring>

namespace ce::ffx_api {

using StructType = uint64_t;

struct ApiHeader {
    StructType type;
    ApiHeader* pNext;
};

struct ResourceDescription {
    uint32_t type;
    uint32_t format;
    union {
        uint32_t width;
        uint32_t size;
    };
    union {
        uint32_t height;
        uint32_t stride;
    };
    union {
        uint32_t depth;
        uint32_t alignment;
    };
    uint32_t mipCount;
    uint32_t flags;
    uint32_t usage;
};

struct Resource {
    void* resource;
    ResourceDescription description;
    uint32_t state;
};

enum ResourceState : uint32_t {
    kResourceStateCommon = (1u << 0),
    kResourceStateUnorderedAccess = (1u << 1),
    kResourceStateComputeRead = (1u << 2),
    kResourceStatePixelRead = (1u << 3),
    kResourceStateCopySrc = (1u << 4),
    kResourceStateCopyDest = (1u << 5),
    kResourceStateIndirectArgument = (1u << 6),
    kResourceStatePresent = (1u << 7),
    kResourceStateRenderTarget = (1u << 8),
    kResourceStateDepthAttachment = (1u << 9),
};

struct Rect2D {
    int32_t left;
    int32_t top;
    int32_t width;
    int32_t height;
};

using OpaqueCallback = uint32_t (*)(void*, void*);

struct CallbackDescFrameGenerationPresent;
using PresentCallback = uint32_t (*)(CallbackDescFrameGenerationPresent*, void*);

constexpr uint32_t kEffectMask = 0x00ff0000u;
constexpr uint32_t kEffectIdFrameGeneration = 0x00020000u;
constexpr uint32_t kEffectIdFrameGenerationSwapchain = 0x00030000u;

constexpr StructType MakeEffectSubId(uint32_t effectId, uint64_t subversion) {
    return (effectId & kEffectMask) | (subversion & ~static_cast<uint64_t>(kEffectMask));
}

constexpr StructType kConfigureDescTypeFrameGeneration = MakeEffectSubId(kEffectIdFrameGeneration, 0x02u);
constexpr StructType kCallbackDescTypeFrameGenerationPresent = MakeEffectSubId(kEffectIdFrameGeneration, 0x05u);
constexpr StructType kCallbackDescTypeFrameGenerationPresentPremulAlpha =
    MakeEffectSubId(kEffectIdFrameGeneration, 0x0du);

struct CallbackDescFrameGenerationPresent {
    ApiHeader header;
    void* device;
    void* commandList;
    Resource currentBackBuffer;
    Resource currentUI;
    Resource outputSwapChainBuffer;
    bool isGeneratedFrame;
    uint64_t frameID;
};

struct CallbackDescFrameGenerationPresentPremulAlpha {
    ApiHeader header;
    bool usePremulAlpha;
};

struct ConfigureDescFrameGeneration {
    ApiHeader header;
    void* swapChain;
    PresentCallback presentCallback;
    void* presentCallbackUserContext;
    OpaqueCallback frameGenerationCallback;
    void* frameGenerationCallbackUserContext;
    bool frameGenerationEnabled;
    bool allowAsyncWorkloads;
    Resource hudlessColor;
    uint32_t flags;
    bool onlyPresentGenerated;
    Rect2D generationRect;
    uint64_t frameID;
};

struct ParsedFrameGenerationConfigureState {
    bool recognized = false;
    bool enabled = false;
    uint64_t frameId = 0;
};

inline uint32_t GetEffectId(StructType type) {
    return static_cast<uint32_t>(type) & kEffectMask;
}

inline bool IsFrameGenerationEffectType(StructType type) {
    const uint32_t effectId = GetEffectId(type);
    return effectId == kEffectIdFrameGeneration || effectId == kEffectIdFrameGenerationSwapchain;
}

inline ParsedFrameGenerationConfigureState ParseFrameGenerationConfigureState(const ApiHeader* desc) {
    if (!desc || desc->type != kConfigureDescTypeFrameGeneration) {
        return {};
    }

    const auto* frameGenerationDesc = reinterpret_cast<const ConfigureDescFrameGeneration*>(desc);
    return {
        true,
        frameGenerationDesc->frameGenerationEnabled,
        frameGenerationDesc->frameID,
    };
}

inline bool ResolvePresentCallbackUsePremulAlpha(const CallbackDescFrameGenerationPresent* desc) {
    if (!desc) {
        return false;
    }

    for (const ApiHeader* extra = desc->header.pNext; extra; extra = extra->pNext) {
        if (extra->type == kCallbackDescTypeFrameGenerationPresentPremulAlpha) {
            return reinterpret_cast<const CallbackDescFrameGenerationPresentPremulAlpha*>(extra)->usePremulAlpha;
        }
    }

    return false;
}

inline char ToLowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline const char* PathFileName(const char* path) {
    if (!path) {
        return "";
    }

    const char* fileName = path;
    for (const char* cursor = path; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            fileName = cursor + 1;
        }
    }
    return fileName;
}

inline bool ContainsAsciiInsensitive(const char* value, const char* needle) {
    if (!value || !needle || !*needle) {
        return false;
    }

    const size_t needleLen = std::strlen(needle);
    for (const char* cursor = value; *cursor; ++cursor) {
        size_t matched = 0;
        while (matched < needleLen && cursor[matched] &&
               ToLowerAscii(cursor[matched]) == ToLowerAscii(needle[matched])) {
            ++matched;
        }
        if (matched == needleLen) {
            return true;
        }
    }
    return false;
}

inline bool IsOfficialAMDFFXRuntimeModuleName(const char* moduleNameOrPath) {
    const char* fileName = PathFileName(moduleNameOrPath);
    if (!fileName || !*fileName) {
        return false;
    }

    return ContainsAsciiInsensitive(fileName, "amd_fidelityfx_framegeneration") ||
           ContainsAsciiInsensitive(fileName, "amd_fidelityfx_dx12") ||
           ContainsAsciiInsensitive(fileName, "amd_fidelityfx_vk") ||
           ContainsAsciiInsensitive(fileName, "amd_fidelityfx_fg");
}

inline bool ShouldInlineHookFFXExportsForModule(const char* moduleNameOrPath) {
    if (!moduleNameOrPath || !*moduleNameOrPath) {
        return false;
    }
    return !IsOfficialAMDFFXRuntimeModuleName(moduleNameOrPath);
}

}  // namespace ce::ffx_api
