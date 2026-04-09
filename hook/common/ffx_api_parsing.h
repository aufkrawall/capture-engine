#pragma once

#include <cstdint>

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

struct Rect2D {
    int32_t left;
    int32_t top;
    int32_t width;
    int32_t height;
};

using OpaqueCallback = uint32_t (*)(void*, void*);

constexpr uint32_t kEffectMask = 0x00ff0000u;
constexpr uint32_t kEffectIdFrameGeneration = 0x00020000u;
constexpr uint32_t kEffectIdFrameGenerationSwapchain = 0x00030000u;

constexpr StructType MakeEffectSubId(uint32_t effectId, uint64_t subversion) {
    return (effectId & kEffectMask) | (subversion & ~static_cast<uint64_t>(kEffectMask));
}

constexpr StructType kConfigureDescTypeFrameGeneration = MakeEffectSubId(kEffectIdFrameGeneration, 0x02u);

struct ConfigureDescFrameGeneration {
    ApiHeader header;
    void* swapChain;
    OpaqueCallback presentCallback;
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

}  // namespace ce::ffx_api
