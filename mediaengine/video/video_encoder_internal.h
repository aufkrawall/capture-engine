#pragma once

struct PostMuxProbeControl;

struct HandleFailureCache;

struct ResolvedVideoFormat;

class D3D11ScopedLock;

class PerfTimer;

struct FrameStats;

#include "video_encoder.h"

#include "common/capture/capture_pipeline_policy.h"

#include "common/capture/frame_timing_utils.h"

#include "common/logging/log_privacy.h"

#include "common/config/live_stream_config.h"

#include "common/platform/path_utils.h"

#include "common/platform/raii_helpers.h"

#include "common/capture/reserved_capture_output.h"

#include "common/platform/secure_dll_loading.h"

#include "common/ipc/shared_defs.h"

#include "mediaengine/audio/audio_time_utils.h"  // For ce::audio::ParseSampleRateOr

#include "mediaengine/mux/cfr_packet_duration.h"

#include "mediaengine/mux/matroska_timing.h"

#include "mediaengine/engine/mediaengine.h"
#include "mediaengine/engine/mediaengine_instance.h"

#include "mediaengine/mux/mux_invariants.h"

#include "video_encoder_options.h"

#include "video_color_conversion_shader.h"

#include "rgb_yuv_draw.h"

#include "retained_frame_route.h"

#include "video_format_policy.h"

#include "video_metadata.h"

#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include <cmath>

#include <d3d11_4.h>

#include <dxgi1_5.h>

#include <algorithm>

#include <atomic>

#include <cctype>

#include <chrono>

#include <cstdio>

#include <cstdlib>

#include <cstring>

#include <functional>

#include <memory>

#include <thread>

#include <unordered_map>

#include <vector>

#include <filesystem>

#include "cursor_renderer.h"

#include "face_camera_renderer.h"

enum WriterFinalizePhase : uint32_t {
    kWriterPhaseRunning = 0,
    kWriterPhaseFinalizeStarting = 1,
    kWriterPhaseFlushingEncoder = 2,
    kWriterPhaseWritingTrailer = 3,
    kWriterPhasePostMuxProbe = 4,
    kWriterPhaseCleanup = 5,
    kWriterPhaseComplete = 6,
};

#include <windows.h>

#include <mutex>

#include <unordered_map>

namespace fs = std::filesystem;

#ifndef D3D11_FORMAT_SUPPORT_SHAREABLE
#define D3D11_FORMAT_SUPPORT_SHAREABLE 0x2000
#endif

enum class OutputRangeMode { kLimited, kFull };

void TrimD3D11Residency(ID3D11Device* device, ID3D11DeviceContext* context, const char* label);

const char* WriterFinalizePhaseName(uint32_t phase);

bool WriterFinishedWithin(std::future<void>& finished, uint64_t timeoutMs);

bool HasValidStreamTimeBase(const AVStream* stream);

bool ShouldCancelPostMuxProbe(const PostMuxProbeControl* control);

int PostMuxProbeInterrupt(void* opaque);

int64_t ParseDurationTagUs(const char* value);

int64_t GetStreamStartUs(const AVStream* stream);

int64_t GetStreamDurationUs(const AVStream* stream);

uint32_t GetPacketTerminalDiscardSamples(const AVPacket* packet);

bool LogPostMuxDurationProbe(const std::string& filename, int64_t finalDurationUs, PostMuxProbeControl* control = nullptr);

void RunPostMuxDurationProbeBounded(const std::string& filename, int64_t finalDurationUs, uint64_t timeoutMs);

bool ValidateFormatContextForHeader(const AVFormatContext* fmtCtx);

int64_t ComputeTargetVideoPts(int64_t timestampUs, bool useVfr, int fps, int64_t startPts, int64_t lastAssignedVideoPts, bool useExplicitCfrTimeline);

bool IsConfiguredNvencLookaheadActive(const std::string& value);

bool IsConfiguredNvencMultipassActive(const VideoConfig& config);

void LogFinalDurationSummary(AVFormatContext* fmtCtx, int64_t finalDurationUs, uint32_t muxBackpressureEvents, uint32_t peakQueueBytes, uint32_t peakQueuePackets, bool encoderOverloaded, bool muxOverloaded);

// `failures` is the calling encoder's cache (VideoEncoder::diagnostics.handleFailures).
extern "C" __attribute__((noinline)) HRESULT __cdecl CallOpenSharedFence(HandleFailureCache& failures, ID3D11Device5* dev,
                                                                         HANDLE h, ID3D11Fence** out);

extern "C" __attribute__((noinline)) HRESULT __cdecl CallOpenSharedResource(HandleFailureCache& failures,
                                                                            ID3D11Device5* dev, HANDLE h, REFIID riid,
                                                                            void** out);

extern "C" __attribute__((noinline)) HRESULT __cdecl CallOpenSharedResource1(HandleFailureCache& failures,
                                                                             ID3D11Device5* dev, HANDLE h, REFIID riid,
                                                                             void** out);

uint32_t SaturatingToUint32(uint64_t value);

const char* GetPixFmtNameSafe(AVPixelFormat pixFmt);

bool SupportsCodecPixelFormat(const AVCodec* codec, AVPixelFormat pixFmt);

bool SupportsD3D11HwInputFormat(const AVCodec* codec, AVPixelFormat swFormat);

bool DeviceSupportsHwFrameSwFormat(AVBufferRef* deviceCtx, AVPixelFormat swFormat);

bool IsDirectRgbD3D11SwFormat(AVPixelFormat swFormat);

bool UsesQsvHardwareFrames(const std::string& encoderName);

std::string ResolveRequestedBitDepth(const VideoConfig& config, bool prefer10Bit);

std::string ResolveRequestedChroma(const VideoConfig& config);

bool ResolveVideoFormat(const VideoConfig& config, bool isHDR, bool prefer10Bit, const AVCodec* codec, ResolvedVideoFormat* out, std::string* error, std::string* warning);

bool WantsFullOutputRange(const std::string& colorRange);

OutputRangeMode GetEffectiveOutputRange(const std::string& colorRange, bool /*isHDR*/);

AVColorRange GetAVColorRange(OutputRangeMode range);

const char* DescribeOutputRange(OutputRangeMode range);

bool ApplyFrameColorMetadata(AVFrame* frame, const AVCodecContext* codec, int hdrNominalPeakNits);

DXGI_COLOR_SPACE_TYPE GetVideoProcessorInputColorSpace(DXGI_FORMAT format, bool isHDR, bool forceLinear = false);

DXGI_COLOR_SPACE_TYPE GetVideoProcessorOutputColorSpace(bool use10Bit, bool isHDR, const std::string& colorSpace, OutputRangeMode outputRange);

bool QuerySdrWhiteLevelNits(HMONITOR monitor, float* nits, ULONG* rawLevel);

HANDLE NormalizeSourceHandleForWow64(HANDLE handle, uint32_t sourcePid);

ce::capture_output::ReservedCaptureOutput ReserveOutputStagingFile(const VideoConfig& config);

int AllocateOutputContextForContainer(AVFormatContext** formatContext, const VideoConfig& config);

int64_t RoundUsToMs(int64_t valueUs);

void FreeScopedAvFrame(AVFrame** frame);

inline constexpr uint64_t video_encoder_kPostMuxProbeTimeoutMs = 5000;

extern "C" {
#include <libavutil/intreadwrite.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

struct PostMuxProbeControl {
    std::atomic<bool> cancel{false};
    uint64_t deadlineTickMs = 0;
};

template <typename AtomicT>
void UpdateAtomicPeak(AtomicT& peak, uint32_t value) {
    uint32_t current = peak.load(std::memory_order_relaxed);
    while (value > current &&
           !peak.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

struct ResolvedVideoFormat {
    AVPixelFormat codecPixFmt = AV_PIX_FMT_NONE;
    AVPixelFormat d3d11SwFormat = AV_PIX_FMT_NONE;
    DXGI_FORMAT directDxgiFormat = DXGI_FORMAT_UNKNOWN;
    std::string bitDepth;
    std::string chroma;
    bool use10Bit = false;
    bool usesVideoProcessor = true;
    bool requiresEvenDimensions = true;
};

// RAII guard for the pipeline's D3D11 lock (MediaEngineInstance::d3d11Mutex): the immediate
// context and the encoder device are serialized by it.
class D3D11ScopedLock {
public:
    explicit D3D11ScopedLock(std::recursive_mutex& mutex) : mutex_(mutex) {
        mutex_.lock();
    }
    ~D3D11ScopedLock() {
        mutex_.unlock();
    }
    D3D11ScopedLock(const D3D11ScopedLock&) = delete;
    D3D11ScopedLock& operator=(const D3D11ScopedLock&) = delete;

private:
    std::recursive_mutex& mutex_;
};

// Performance timing helper for pipeline analysis
class PerfTimer {
public:
    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    static TimePoint now() {
        return Clock::now();
    }

    static double elapsed_ms(const TimePoint& start, const TimePoint& end) {
        return std::chrono::duration<double, std::milli>(end - start).count();
    }
};

// Frame statistics for performance monitoring
struct FrameStats {
    int64_t frameNumber = 0;
    int64_t ptsMs = 0;
    double fenceWaitMs = 0;
    double textureOpenMs = 0;
    double colorConvertMs = 0;
    double encodeMs = 0;
    double totalMs = 0;
    int packetsProduced = 0;
    int64_t expectedPtsDiff = 0;  // Expected ms between frames
    int64_t actualPtsDiff = 0;    // Actual ms between frames
};
