#pragma once

struct PostMuxProbeControl;

struct HandleFailureCache;

struct ResolvedVideoFormat;

class D3D11ScopedLock;

class PerfTimer;

struct FrameStats;

#include "video_encoder.h"

#include "../common/capture_pipeline_policy.h"

#include "../common/frame_timing_utils.h"

#include "../common/log_privacy.h"

#include "../common/live_stream_config.h"

#include "../common/path_utils.h"

#include "../common/raii_helpers.h"

#include "../common/reserved_capture_output.h"

#include "../common/secure_dll_loading.h"

#include "../common/shared_defs.h"

#include "audio_time_utils.h"  // For ce::audio::ParseSampleRateOr

#include "matroska_timing.h"

#include "mediaengine.h"

#include "mux_invariants.h"

#include "video_encoder_options.h"

#include "video_color_conversion_shader.h"

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

// Handle validation cache: tracks handles that have previously failed D3D11 OpenShared*
// calls, so we don't repeatedly trigger SEH exceptions from invalid handles.
// D3D11 throws SEH exceptions for invalid handles, and MinGW's catch(...) cannot
// catch SEH exceptions. Pre-validation is the only reliable protection.
//
// The cache stores (handle_value, failure_count) pairs. Handles that fail >3 times
// are permanently skipped. Cache is cleared when recording starts.
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

extern "C" __declspec(dllexport) HRESULT __cdecl CallOpenSharedFence(ID3D11Device5* dev, HANDLE h, ID3D11Fence** out);

extern "C" __declspec(dllexport) HRESULT __cdecl CallOpenSharedResource(ID3D11Device5* dev, HANDLE h, REFIID riid, void** out);

extern "C" __declspec(dllexport) HRESULT __cdecl CallOpenSharedResource1(ID3D11Device5* dev, HANDLE h, REFIID riid, void** out);

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

extern HandleFailureCache video_encoder_g_HandleFailureCache;

// Global stats for frame analysis
extern int64_t video_encoder_g_lastFramePts;

extern int64_t video_encoder_g_framesEncoded;

// static int64_t g_framesDropped = 0;
extern double video_encoder_g_totalFenceWait;

extern double video_encoder_g_totalColorConvert;

extern double video_encoder_g_totalEncode;

extern double video_encoder_g_maxFrameTime;

extern int video_encoder_g_slowFrameCount;

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

struct HandleFailureCache {
    std::mutex mutex;
    std::unordered_map<HANDLE, int> fenceFailures;
    std::unordered_map<HANDLE, int> textureFailures;

    bool ShouldSkipFence(HANDLE h) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = fenceFailures.find(h);
        return it != fenceFailures.end() && it->second >= 3;
    }

    bool ShouldSkipTexture(HANDLE h) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = textureFailures.find(h);
        return it != textureFailures.end() && it->second >= 3;
    }

    void RecordFenceFailure(HANDLE h) {
        std::lock_guard<std::mutex> lock(mutex);
        fenceFailures[h]++;
    }

    void RecordTextureFailure(HANDLE h) {
        std::lock_guard<std::mutex> lock(mutex);
        textureFailures[h]++;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex);
        fenceFailures.clear();
        textureFailures.clear();
    }
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

// RAII Wrapper for MediaEngine D3D11 Guard
class D3D11ScopedLock {
public:
    D3D11ScopedLock() {
        MediaEngine_LockD3D11();
    }
    ~D3D11ScopedLock() {
        MediaEngine_UnlockD3D11();
    }
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
