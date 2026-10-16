#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/config/config.h"

namespace ce::video {

struct EncoderOption {
    std::string key;
    std::string value;
};

struct EncoderOptionPlan {
    std::vector<EncoderOption> generatedOptions;
    std::vector<EncoderOption> customOptions;
    // Invariants that must win over user-provided options. Apply these last.
    std::vector<EncoderOption> requiredOptions;
    std::optional<int64_t> bitRate;
    std::optional<int64_t> maxBitRate;
    std::optional<int64_t> bufferSize;
    std::optional<int> globalQuality;
    std::optional<int> codecProfile;
    bool scaleGlobalQualityByQp2Lambda = false;
    std::optional<int> compressionLevel;
    bool useConstantQscale = false;
    bool useLowDelay = false;
    int maxBFrames = 0;
    // NVENC input surfaces CE requested (0: FFmpeg's own default); see
    // ResolveNvencSurfaceCount.
    int nvencSurfaces = 0;
    bool isHardwareEncoder = false;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

// FFmpeg's NVENC wrapper defaults to 4 input surfaces and then keeps 3 frames
// in flight: once full, avcodec_send_frame blocks in nvEncLockBitstream on the
// packet submitted three calls earlier. Each frame's copy/conversion therefore
// had to clear the GPU queue within ~2 frame intervals (16.7 ms at 120 fps) or
// every submit stalled, although NVENC itself kept up (dx9_test 4K120 under
// 96% GPU load waited 13-98 ms; session 20261015_205007). The ring is sized by
// time instead, so the same latency is tolerated at every frame rate.
inline constexpr int kNvencPipelineLatencyBudgetMs = 100;
inline constexpr int kNvencMinSurfaces = 4;   // FFmpeg's default
inline constexpr int kNvencMaxSurfaces = 64;  // FFmpeg's MAX_REGISTERED_FRAMES
// Surfaces covering kNvencPipelineLatencyBudgetMs of frames plus the one being
// submitted; 0 (keep FFmpeg's default) for an unknown frame rate. FFmpeg still
// raises the count further when lookahead or B-frames need more.
int ResolveNvencSurfaceCount(int fps);

// Quick Sync blocks a submit in MFXVideoCORE_SyncOperation once async_depth
// frames are in flight (qsvenc.c), the same mechanism as NVENC's delay: oneVPL's
// default of 4 tolerated only ~3 frame intervals of GPU-queue latency. With
// [QuickSync] async_depth=auto the depth covers the same latency budget.
inline constexpr int kQsvMinAsyncDepth = 4;  // oneVPL/FFmpeg default
inline constexpr int kQsvMaxAsyncDepth = 64;
int ResolveQsvAsyncDepth(int fps);

bool ParseBitrateString(const std::string& input, int64_t* bitsPerSecond, std::string* error);

bool ParseCustomOptions(const std::string& input, std::vector<EncoderOption>* options, std::string* error);

EncoderOptionPlan BuildEncoderOptionPlan(const VideoConfig& config, bool use10Bit, const std::string& resolvedChroma,
                                         bool outputIsHDR);

}  // namespace ce::video
