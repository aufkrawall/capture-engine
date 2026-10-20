#pragma once

struct WgcRetargetRequest;

struct WindowSearch;

struct ForegroundWgcWindowCandidate;

class ScopedMmcssTask;

// clang-format off
#include <windows.h>

#include <avrt.h>

#include <d3d11.h>

#include <psapi.h>

#include <timeapi.h>

// clang-format on
#include <algorithm>

#include <array>

#include <atomic>

#include <cctype>

#include <chrono>

#include <cmath>

#include <cstdlib>

#include <cstring>

#include <deque>

#include <exception>

#include <limits>

#include <memory>

#include <mutex>

#include <string>

#include <thread>

#include <vector>

#include "common/platform/atomic_shared_owner.h"

#include "common/ipc/av_sync_latency_channel.h"

#include "common/ipc/media_finalization.h"

#include "common/capture/capture_handoff_state.h"

#include "common/capture/capture_pipeline_policy.h"

#include "common/config/config.h"

#include "common/capture/cursor_capture_state.h"

#include "common/capture/frame_queue.h"

#include "common/capture/frame_timing_utils.h"

#include "common/logging/logging.h"

#include "common/platform/monitor_selection.h"

#include "common/ipc/process_ipc.h"

#include "common/capture/rate_window_utils.h"

#include "common/capture/recording_lifecycle.h"

#include "common/capture/screen_grab_privacy.h"

#include "common/platform/secure_dll_loading.h"

#include "common/ipc/shared_defs.h"

#include "common/platform/thread_power_throttling_compat.h"

#include "common/platform/thread_wait.h"

#include "capture_cadence_diagnostics.h"

#include "encoder_loop_stage_cost.h"

#include "captureengine/app/mediaengine_loader.h"

#include "inject_source_read.h"
#include "media_session_state.h"

#include "recording_manifest.h"

#include "screen_grab_privacy_runtime.h"

#include "status_overlay_sync.h"

#include "wgc_capture.h"

#include "captureengine/app/windows_gpu_scheduling.h"

using ce::screen_grab_privacy::GetWindowClientRectInScreen;

using ce::screen_grab_privacy::IsWindowFullscreenLike;

#ifdef _MSC_VER
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "winmm.lib")
#endif

BOOL WINAPI MediaConsoleHandler(DWORD ctrlType);

UINT GetCursorDpiAtPoint(POINT point);

int GetCursorMetricForDpi(int metric, UINT dpi);

ce::cursor::CaptureState CaptureCursorSnapshot(int64_t associationQpc, int32_t captureLeft, int32_t captureTop, uint32_t captureWidth, uint32_t captureHeight, bool sourceEmbedded);

uint64_t AdvanceWgcSourceEpoch(const char* reason);

void PublishWgcCapture(std::shared_ptr<WGCCapture> replacement, const char* reason);

void AtomicMax(std::atomic<uint32_t>& target, uint32_t value);

void AtomicMin(std::atomic<uint32_t>& target, uint32_t value);

void SnapshotWgcRuntimeLogState(const WGCCapture* cap);

void SnapshotPublishedWgcRuntimeLogState();

void InjectCaptureThreadFunc(const AppConfig& config);

void WgcCaptureThreadFunc(const AppConfig& config);

void StopRecording();

bool StartRecording(const AppConfig& config);

void StampAvSyncStatus(AppConfig& config, const char* confidence, const char* reason, float resolvedMs, bool usedAudioProbe);

void ApplyAutoDetectedRenderLatencyToConfig(AppConfig& config);

void DeleteLegacyAudioLatencyCacheFileOnce(const std::string& cacheDir);

void MeasureRenderLatencyOnce(const AppConfig& config, const std::string& cacheDir);

const char* WgcIngressAdmissionReasonName(uint32_t code);

void UpdateEncoderBottleneckFlag(double smoothedEncodeMs, double frameIntervalMs, bool startupWindowActive);

bool MediaAudioConfigEquals(const AudioConfig& lhs, const AudioConfig& rhs);

bool MediaScalingConfigEquals(const ScalingConfig& lhs, const ScalingConfig& rhs);

bool MediaVideoConfigEquals(const VideoConfig& lhs, const VideoConfig& rhs);

bool MediaEngineConfigEquals(const AppConfig& lhs, const AppConfig& rhs);

bool IsExplicitTenBitVideo(const VideoConfig& video);

uint32_t GetInitialWgcCfrTargetFps(const VideoConfig& video);

uint32_t SaturatingToUint32(uint64_t value);

void SetCapturePipelinePhase(CapturePipelinePhase phase);

bool TryArmCapturePipelineWarmup();

bool TryCommitCapturePipelineLive();

CapturePipelinePhase BeginCapturePipelineStop();

void ResetRuntimeDiagnostics(SharedMemoryLayout* sharedMem);

void ResetRecordingHealthPublication();

void PublishRecordingHealth(const ce::capture_policy::RecordingHealthState& health);

void CompleteRecordingFinalization(bool canceled, bool outputSaved);
// No-op once a recording went live; otherwise finalizes the aborted start as a cancellation so
// the overlays and the recording manifest both get a terminal state.
void CompleteAbortedRecordingStart(const char* reason);

bool IsActiveScreenGrab();

void SetActiveScreenGrab(bool enabled);

bool IsPreferredScreenGrab();

void SetPreferredScreenGrab(bool enabled);

void SetCaptureRequestedState(bool enabled);

void SetInjectVideoCaptureRequestedState(bool enabled, const char* reason);

void SignalStatusOverlaySync();

void RequestStatusOverlayDarkForCapture(const char* reason);

void ReleaseStatusOverlayDarkForCapture(const char* reason);

void SetRecordingVisibleState(bool enabled);

void PublishRecordingStartFailure(RecordingFailureCode failureCode, const char* reason);

bool WindowBelongsToProcess(HWND hwnd, DWORD pid);

bool ShouldPreferInjectCaptureForFullscreenWindow(HWND hwnd, DWORD pid);

InjectFrameLineage MakeInjectFrameLineage(const QueuedFrame& frame);

bool MatchesInjectFrameLineage(const QueuedFrame& frame, const InjectFrameLineage& lineage);

bool MatchesInjectFrameLineage(const InjectFrameLineage& lhs, const InjectFrameLineage& rhs);


bool JoinThreadWithTimeout(std::thread& thread, DWORD timeoutMs, const char* threadName);

void MediaLogCallback(const char* msg);

void ReleaseStandaloneWgcQueuedFrame(QueuedFrame& frame);

void ClearStandbyWgcHandoffFrame();

bool HasStandbyWgcHandoffFrame();

bool StoreStandbyWgcHandoffFrame(QueuedFrame&& frame);

bool TakeStandbyWgcHandoffFrame(QueuedFrame& frame);

void SubmitWgcQueuedFrame(QueuedFrame&& frame);

void QueueWgcCursorObservation(const ce::cursor::SourcePointerObservation& observation, int32_t captureLeft, int32_t captureTop, uint32_t captureWidth, uint32_t captureHeight, uint64_t sourceEpoch);

void QueueWgcFrame(ID3D11Texture2D* texture, uint32_t width, uint32_t height, int64_t timestamp, int64_t rawTimestamp,
                   bool isHDR, bool cursorEmbedded, bool duplicateSourceTimestamp,
                   const ce::cursor::SourcePointerObservation& cursorObservation, int32_t captureLeft,
                   int32_t captureTop, uint64_t sourceEpoch, WgcPoolSlotLease&& poolLease, uint32_t sourceFrameSpan);

QueuedFrame MakeQueuedWgcFrame(WGCCapturedFrame&& frame);

void ReleaseWgcCapturedFrame(WGCCapturedFrame& frame);

void ResetInjectFrameRingToLatest(const char* reason);

void ResetLastQueuedFrameCache();

bool EnsureInjectCaptureEvents();

void StopInjectCapturePipeline();

void SyncDuplicationCursorSuppression(bool suppress);

void ResetDuplicationCursorSuppression(const char* reason);

void StopWgcCapturePipeline();

bool StartWgcRecordingCapture(const AppConfig& config);

extern std::string GetProcessNameFromPID(DWORD pid);

HWND GetMainWindowForProcess(DWORD pid);

std::string ToLowerAscii(std::string value);

bool IsIgnoredForegroundWgcClass(HWND hwnd);

bool IsIgnoredForegroundWgcProcess(const std::string& processName);

ForegroundWgcWindowCandidate GetForegroundWgcWindowCandidate();

bool MatchesProcessEntry(const WhitelistEntry& entry, const std::string& lowerProcessName);

bool MatchesProcessEntries(const std::vector<WhitelistEntry>& entries, const std::string& processName);

const ApplicationProfile* FindApplicationProfileForProcess(const AppConfig& config, const std::string& processName);

const ApplicationProfile* FindApplicationProfileForTarget(const AppConfig& config, const WhitelistEntry& target);

int64_t RectArea(const RECT& rect);

HWND FindMatchingWgcWindow(const std::vector<WhitelistEntry>& targets, int* selectedScore = nullptr, bool requireExactProcessNames = false, uint32_t* selectedPid = nullptr, std::string* selectedProcessName = nullptr, WhitelistEntry* selectedTarget = nullptr);

std::string GetLocalConfigPath();

void DisableCurrentThreadPowerThrottling(const char* role);

void WaitUntilQpcTarget(HANDLE timer, int64_t targetQpc, int64_t qpcFrequency);

const char* Win32PriorityClassName(DWORD priorityClass);

bool IsCurrentProcessElevatedForPriorityLog();

void ApplyMediaProcessPriority(const AppConfig& config);

const char* D3dkmtSchedulingPriorityClassName(int priorityClass);

bool ResolveD3dkmtSchedulingPriorityClass(const std::string& value, int& priorityClass);

void ApplyMediaGpuSchedulingPriority(const AppConfig& config, const LUID* adapterLuid = nullptr);

void ApplyMediaPrioritySettings(const AppConfig& config);

bool ApplyMediaGpuSchedulingPriorityForDevice(const AppConfig& config, ID3D11Device* device);

bool ApplyMediaGpuSchedulingPriorityForSharedAdapter(const AppConfig& config);

void PublishMediaScreenGrabTarget(uint32_t processId, ID3D11Device* device, bool active, const char* reason);

void InjectCaptureThreadFunc(const AppConfig& config);

void WgcCaptureThreadFunc(const AppConfig& config);

void EncoderThreadFunc(const AppConfig& config);

bool StartRecording(const AppConfig& config);

void StopRecording();

int MediaProcessMain(const AppConfig& initialConfig);

inline constexpr int media_main_kInjectTextureSlotCount = SHARED_TEXTURE_SLOT_COUNT;

// Encoder-bottleneck EMA parameters.  The smoothing factor (alpha) controls
// how quickly the EMA reacts to encode-time changes.  Hysteresis avoids
// bang-bang oscillation: we enter bottleneck at a higher threshold and exit
// at a significantly lower one.
inline constexpr double media_main_kEncodeEmaAlpha = 0.10;

inline constexpr double media_main_kBottleneckEnterRatio = 0.95;  // smoothedEncodeMs > 95% of frame interval → enter

inline constexpr double media_main_kBottleneckExitRatio = 0.75;   // smoothedEncodeMs < 75% of frame interval → exit

struct WgcRetargetRequest {
    HWND window = NULL;
    HMONITOR monitor = NULL;
    bool preferMonitor = false;
    bool active = false;
};

template <typename AtomicT>
void UpdateAtomicPeak(AtomicT& peak, uint32_t value) {
    uint32_t current = peak.load(std::memory_order_relaxed);
    while (value > current &&
           !peak.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

// Window finding helper
struct WindowSearch {
    DWORD pid;
    HWND hwnd;
};

struct ForegroundWgcWindowCandidate {
    HWND hwnd = NULL;
    DWORD pid = 0;
    std::string processName;
    bool usable = false;
    bool fullscreenLike = false;
};

class ScopedMmcssTask {
public:
    ScopedMmcssTask(const wchar_t* taskName, AVRT_PRIORITY priority, const char* role) : role_(role) {
        DWORD taskIndex = 0;
        handle_ = AvSetMmThreadCharacteristicsW(taskName, &taskIndex);
        if (handle_) {
            if (!AvSetMmThreadPriority(handle_, priority)) {
                LogWarn("[%s] AvSetMmThreadPriority failed (tid=%lu err=%lu)", role_, GetCurrentThreadId(),
                        GetLastError());
            } else {
                LogInfo("[%s] Thread QoS enabled (tid=%lu task=%ls priority=%d)", role_, GetCurrentThreadId(), taskName,
                        static_cast<int>(priority));
            }
        } else {
            const DWORD mmcssError = GetLastError();
            if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)) {
                LogWarn("[%s] MMCSS and THREAD_PRIORITY_HIGHEST setup failed (tid=%lu mmcssErr=%lu priorityErr=%lu)",
                        role_, GetCurrentThreadId(), mmcssError, GetLastError());
            } else {
                LogWarn("[%s] MMCSS setup failed; using THREAD_PRIORITY_HIGHEST (tid=%lu err=%lu)", role_,
                        GetCurrentThreadId(), mmcssError);
            }
        }
    }

    ~ScopedMmcssTask() {
        if (handle_) {
            if (!AvRevertMmThreadCharacteristics(handle_)) {
                LogWarn("[%s] AvRevertMmThreadCharacteristics failed (tid=%lu err=%lu)", role_, GetCurrentThreadId(),
                        GetLastError());
            }
        }
    }

    ScopedMmcssTask(const ScopedMmcssTask&) = delete;
    ScopedMmcssTask& operator=(const ScopedMmcssTask&) = delete;

private:
    HANDLE handle_ = nullptr;
    const char* role_ = "Thread";
};

class MediaProcessSession {
public:
    int Run(const AppConfig& initialConfig);

private:
    AppConfig config;
    ProcessIPCServer ipc{ProcessMode::Media};
    bool mediaEngineReady = false;
    std::string exeDir;
    std::string configPath;
    std::string mediaCacheDir;
    // Controller-owned session A/V latency channel, inherited at spawn. Null when the controller
    // could not provide one; this process then pays the full render->loopback probe.
    ce::av_sync::LatencyChannelBlock* latencyChannel = nullptr;
    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    LARGE_INTEGER qpcFreq{};
    int64_t recordingStartTime = 0;
    DWORD lastEarlyWgcScan = 0;
    DWORD lastWindowScanTime = 0;
    HWND currentCapturedWindow = NULL;
    std::string currentCapturedMonitorStableId;
    bool currentTargetPrefersInject = false;
    WgcRetargetRequest pendingWgcRetarget;
    uint32_t lastSourcePid = 0;
    uint32_t activeConfigSourcePid = 0;
    std::string activeConfigProcessName;
    ce::capture_handoff::InjectToWgcHandoff autoWgcHandoff;
    uint32_t autoWgcHandoffBaselineFrames = 0;
    uint64_t autoWgcHandoffDeadlineTick = 0;
    static constexpr uint64_t kAutoWgcHandoffReadyTimeoutMs = 2000;

    void unloadMediaEngineIdle();
    bool ensureMediaEngineReady();
    bool isExplicitInjectConfig();
    bool isExplicitWgcConfig();
    bool isExplicitDxgiDupConfig();
    bool isExplicitScreenGrabConfig();
    bool isAutoCaptureConfig();
    void setWgcPreferenceAfterFailure();
    bool isInjectCaptureTarget(const std::string& processName);
    std::string resolveSourceProcessName(uint32_t sourcePid, const std::string& knownName = std::string{});
    bool isInjectCaptureTargetForSource(uint32_t sourcePid, const std::string& knownName = std::string{});
    void applyWgcOptions(WGCCapture* capture);
    bool ensureWgcDevice();
    void releaseIdleWgcResources();
    void clearCurrentWgcTarget();
    void discardCurrentWgcTarget(const char* reason);
    void queueWgcRetarget(HWND targetWindow, HMONITOR targetMonitor, bool preferMonitor, const char* reason);
    std::string refreshActiveConfig(bool forceReload, HWND targetWindow = NULL, uint32_t confirmedPid = 0,
                                   const std::string& confirmedProcessName = std::string{});
    bool markInjectPreferredTarget(HWND targetWindow, uint32_t sourcePid, const char* reason);
    bool primeWgcMonitorTarget(HMONITOR targetMonitor);
    bool primeConfiguredMonitorTarget(HWND targetWindow, HMONITOR targetHint, const std::string& selectorText,
                                            const char* context);
    bool primePinnedMonitorTarget(HMONITOR previousMonitor, const char* context);
    bool monitorSelectorIsExplicit(const std::string& selectorText);
    bool primeDxgiDupForWindowMonitor(HWND targetWindow, const std::string& selectorText,
                                            const char* reason);
    bool primeWgcWindowTarget(HWND targetWindow, bool logPrimed, bool allowMonitorFallback = true);
    bool applyPendingWgcRetarget();
    void prepareCaptureForRecordingStart();
    int Init();
    void Loop();
    void Shutdown();
};
