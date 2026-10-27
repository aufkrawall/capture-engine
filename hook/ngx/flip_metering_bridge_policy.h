#pragma once

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

#include <cstddef>
#include <cstdint>

// NvAPI_D3D12_SetFlipConfig for a D3D12 device that runs on Vulkan.
//
// Native D3D12 DLSS frame generation meters its batch with the public NVAPI call (R570+)
//   NvAPI_D3D12_SetFlipConfig(IUnknown* pCommandQueue, UINT vidpnSrcID, NV_FLIP_CONFIG* pFlipConfig)
// "right before the first Present() of each batch", and asks NvAPI_NGX_GetDriverFeatureSupport (R595+)
// whether the V2 structure is understood. Streamline's sl.dlss_g resolves both through
// nvapi_QueryInterface. A translation NVAPI such as dxvk-nvapi 0.9.2 implements neither, so on
// vkd3d-proton the batch flips at GPU completion; see
// hook/vulkan_layer/vulkan_flip_metering_bridge_policy.h for the measurement and for the Vulkan side.
//
// CE answers both IDs when the resident NVAPI returns none and binds each requested batch size to the
// frame id of the batch's OUT_OF_BAND_PRESENT markers, as the D3D12 runtime receives them from the
// NVAPI layer (vkd3d's ID3DLowLatencyDevice::SetLatencyMarker). vkd3d forwards that frame id unchanged
// to vkSetLatencyMarkerNV around the batch's Vulkan presents, so both sides key on the same number.

namespace ce::flip_metering_bridge {

inline constexpr uint32_t kNvApiIdD3D12SetFlipConfig = 0xF3148C42u;
inline constexpr uint32_t kNvApiIdNgxGetDriverFeatureSupport = 0x6194B19Du;

inline constexpr int32_t kNvApiOk = 0;
inline constexpr int32_t kNvApiError = -1;
inline constexpr int32_t kNvApiInvalidArgument = -5;
inline constexpr int32_t kNvApiIncompatibleStructVersion = -9;
inline constexpr int32_t kNvApiInvalidPointer = -14;
inline constexpr int32_t kNvApiNotSupported = -104;

inline constexpr uint32_t kMaxFramesPerFlipBatch = 8;  // NVAPI_MAX_FRAMES_PER_FLIP_BATCH

enum class QueryAnswer {
    // Not a bridge ID, or the resident NVAPI implements it (the driver's own path stays authoritative).
    kPassThrough,
    kSetFlipConfig,
    kDriverFeatureSupport,
    // A bridge ID the resident NVAPI lacks, but no Vulkan device can meter: answering would promise
    // metering nothing carries, so the caller keeps its own pacing.
    kDeclinedNoMeteringDevice,
};

inline QueryAnswer DecideQueryInterface(uint32_t functionId, bool residentImplements, bool bridgeAvailable) {
    if (functionId != kNvApiIdD3D12SetFlipConfig && functionId != kNvApiIdNgxGetDriverFeatureSupport)
        return QueryAnswer::kPassThrough;
    if (residentImplements)
        return QueryAnswer::kPassThrough;
    if (!bridgeAvailable)
        return QueryAnswer::kDeclinedNoMeteringDevice;
    return functionId == kNvApiIdD3D12SetFlipConfig ? QueryAnswer::kSetFlipConfig
                                                    : QueryAnswer::kDriverFeatureSupport;
}

// NV_FLIP_CONFIG_V1 / _V2, field for field.
struct FlipConfigV1 {
    uint32_t version;
    uint32_t nFramesPerBatch;  // 0 = metering disabled for this batch
    uint32_t flipConfigFeedbackMask;
    uint32_t reserved[3];
};
struct FlipConfigV2 {
    uint32_t version;
    uint32_t nFramesPerBatch;
    uint32_t flipConfigFeedbackMask;
    uint32_t nPresentedFramesPerBatch;
    float timePerBatchMs;
    float minControlFrameTimeMs;
};
static_assert(sizeof(FlipConfigV1) == 24 && sizeof(FlipConfigV2) == 24, "NV_FLIP_CONFIG layout");

// MAKE_NVAPI_VERSION(type, ver): struct size in the low word, version in the high word.
inline constexpr uint32_t kFlipConfigVer1 = static_cast<uint32_t>(sizeof(FlipConfigV1)) | (1u << 16);
inline constexpr uint32_t kFlipConfigVer2 = static_cast<uint32_t>(sizeof(FlipConfigV2)) | (2u << 16);

struct FlipConfigRequest {
    int32_t status = kNvApiInvalidPointer;
    uint32_t version = 0;
    uint32_t framesPerBatch = 0;
};

// Validates the caller's structure the way the driver documents it. The feedback mask is the one
// field CE writes back: 0, since the Vulkan feedback for this batch exists only after its present.
inline FlipConfigRequest ReadFlipConfig(void* config) {
    FlipConfigRequest request;
    if (!config)
        return request;
    auto* v1 = static_cast<FlipConfigV1*>(config);
    request.version = v1->version;
    if (v1->version != kFlipConfigVer1 && v1->version != kFlipConfigVer2) {
        request.status = kNvApiIncompatibleStructVersion;
        return request;
    }
    if (v1->nFramesPerBatch > kMaxFramesPerFlipBatch) {
        request.status = kNvApiInvalidArgument;
        return request;
    }
    v1->flipConfigFeedbackMask = 0;
    request.framesPerBatch = v1->nFramesPerBatch;
    request.status = kNvApiOk;
    return request;
}

// NV_NGX_GET_DRIVER_FEATURE_SUPPORT_PARAMS_V1.
inline constexpr uint32_t kNgxFeatureSetFlipConfigV2 = 0x00343DCFu;
inline constexpr uint32_t kMaxNgxFeaturesPerQuery = 16;

struct NgxDriverFeatureSupportInfo {
    uint32_t featureId;
    uint32_t supportBits;  // bit 0 = bSupported
    uint32_t reserved2[2];
};
struct NgxGetDriverFeatureSupportParamsV1 {
    uint32_t version;
    uint32_t featureCount;
    NgxDriverFeatureSupportInfo featureSupportInfo[kMaxNgxFeaturesPerQuery];
    uint32_t reserved[6];
};
static_assert(sizeof(NgxGetDriverFeatureSupportParamsV1) == 288, "NV_NGX_GET_DRIVER_FEATURE_SUPPORT_PARAMS layout");
inline constexpr uint32_t kNgxGetDriverFeatureSupportVer1 =
    static_cast<uint32_t>(sizeof(NgxGetDriverFeatureSupportParamsV1)) | (1u << 16);

// Claims exactly what the bridge implements: the V2 flip configuration. Every other feature (the
// hybrid present notification among them) is reported unsupported rather than passed off.
inline int32_t AnswerDriverFeatureSupport(void* params) {
    if (!params)
        return kNvApiInvalidPointer;
    auto* query = static_cast<NgxGetDriverFeatureSupportParamsV1*>(params);
    if (query->version != kNgxGetDriverFeatureSupportVer1)
        return kNvApiIncompatibleStructVersion;
    if (query->featureCount > kMaxNgxFeaturesPerQuery)
        return kNvApiInvalidArgument;
    for (uint32_t i = 0; i < query->featureCount; ++i) {
        NgxDriverFeatureSupportInfo& info = query->featureSupportInfo[i];
        const bool supported = info.featureId == kNgxFeatureSetFlipConfigV2;
        info.supportBits = (info.supportBits & ~1u) | (supported ? 1u : 0u);
    }
    return kNvApiOk;
}

// VkLatencyMarkerNV values, which is what the NVAPI layer passes to the D3D12 runtime's
// SetLatencyMarker (dxvk-nvapi converts NV_LATENCY_MARKER_TYPE by subtracting one).
inline constexpr uint32_t kMarkerOutOfBandPresentStart = 10;
inline constexpr uint32_t kMarkerOutOfBandPresentEnd = 11;

struct Binding {
    uint64_t frameId = 0;
    uint32_t framesPerBatch = 0;
};

// Binds each SetFlipConfig to its batch's frame id, per D3D12 device. Each present of a frame-generation
// batch sits between OUT_OF_BAND_PRESENT START and END markers carrying the batch's frame id, and the
// configuration precedes the batch's first Present(): if a START is open, the configuration is that
// frame's; otherwise it belongs to the next START. Either order of SetFlipConfig and the first START
// therefore binds correctly. Not thread-safe; the runtime guards it.
class BatchBinder {
public:
    // Returns the binding to publish, if this marker completes one.
    bool OnLatencyMarker(uint64_t frameId, uint32_t marker, Binding& out) {
        if (marker == kMarkerOutOfBandPresentEnd) {
            openFrameId_ = 0;
            return false;
        }
        if (marker != kMarkerOutOfBandPresentStart || frameId == 0)
            return false;
        openFrameId_ = frameId;
        if (!hasPending_)
            return false;
        hasPending_ = false;
        out = {frameId, pendingFramesPerBatch_};
        return true;
    }

    bool OnFlipConfig(uint32_t framesPerBatch, Binding& out) {
        if (openFrameId_ != 0) {
            out = {openFrameId_, framesPerBatch};
            return true;
        }
        if (hasPending_)
            ++replacedPending_;
        hasPending_ = true;
        pendingFramesPerBatch_ = framesPerBatch;
        return false;
    }

    // Configurations superseded before any START claimed them: a generator that configures without
    // presenting, which no batch ever reflects.
    uint64_t ReplacedPending() const { return replacedPending_; }

private:
    uint64_t openFrameId_ = 0;
    bool hasPending_ = false;
    uint32_t pendingFramesPerBatch_ = 0;
    uint64_t replacedPending_ = 0;
};

}  // namespace ce::flip_metering_bridge
