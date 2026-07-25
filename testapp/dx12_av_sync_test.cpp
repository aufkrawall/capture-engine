// Deterministic DX12 + WASAPI A/V sync stimulus app.
// Produces visual and audio markers for CaptureEngine CFR sync validation.

#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <d3d12.h>
#include <dxgi1_5.h>
#include <mmsystem.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#include "av_sync_stimulus.h"
#include "dx12_av_sync_audio.h"
#include "testapp_common.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "uuid.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kFrameCount = 2;
constexpr LONG kMarkerMargin = 24;
constexpr LONG kMarkerTile = 24;
constexpr LONG kMarkerGap = 6;

int g_WindowWidth = testapp::avsync::kDefaultWidth;
int g_WindowHeight = testapp::avsync::kDefaultHeight;
int g_RequestedWidth = testapp::avsync::kDefaultWidth;
int g_RequestedHeight = testapp::avsync::kDefaultHeight;
int g_TargetFps = testapp::avsync::kDefaultFps;
int g_DurationSeconds = testapp::avsync::kDefaultDurationSeconds;
int g_VSync = 0;
int g_Fullscreen = 0;
int g_WindowChrome = 0;
int g_Topmost = 1;
int g_GpuLoadPasses = 0;
bool g_EncoderStressScene = false;
bool g_LogEveryFrame = false;
bool g_AudioEnabled = true;
bool g_AudioClockScheduling = false;
int g_AudioBufferMs = testapp::avsync::kDefaultAudioBufferMs;
double g_AudioLeadMs = testapp::avsync::kDefaultAudioLeadMs;
double g_AnalysisStartSeconds = testapp::avsync::kDefaultAnalysisStartSeconds;
bool g_TearingRequested = false;
bool g_TearingSupported = false;
bool g_TearingActive = false;
bool g_Running = true;

LARGE_INTEGER g_QpcFreq = {};
LARGE_INTEGER g_StimulusStartQpc = {};
LARGE_INTEGER g_AppStartQpc = {};
uint64_t g_FrameId = 0;

struct RuntimeSourceStall {
    testapp::avsync::SourceStallSpec spec;
    LARGE_INTEGER actualBeginQpc = {};
    LARGE_INTEGER actualEndQpc = {};
    uint64_t suppressedPresentCount = 0;
    bool active = false;
    bool completed = false;
};

std::vector<RuntimeSourceStall> g_SourceStalls;

ComPtr<ID3D12Device> g_Device;
ComPtr<ID3D12CommandQueue> g_CommandQueue;
ComPtr<IDXGISwapChain3> g_SwapChain;
ComPtr<ID3D12DescriptorHeap> g_RtvHeap;
ComPtr<ID3D12Resource> g_RenderTargets[kFrameCount];
ComPtr<ID3D12CommandAllocator> g_CommandAllocators[kFrameCount];
ComPtr<ID3D12GraphicsCommandList> g_CommandList;
ComPtr<ID3D12Fence> g_Fence;
HANDLE g_FenceEvent = nullptr;
HANDLE g_FrameTimer = nullptr;
bool g_FrameTimerHighResolution = false;
UINT64 g_FenceValues[kFrameCount] = {};
UINT g_FrameIndex = 0;
UINT g_RtvDescriptorSize = 0;
std::mutex g_FrameSyncMutex;

LARGE_INTEGER g_LastPresentQpc = {};
int g_LastPresentEventIndex = -9999;
double g_LastPresentDeltaMs = 0.0;
double g_MaxPresentDeltaMs = 0.0;
double g_PresentDeltaSumMs = 0.0;
uint64_t g_PresentDeltaCount = 0;
uint64_t g_FramePacingSpikeCount = 0;
uint64_t g_WarmupPacingSpikeCount = 0;
uint64_t g_EventBoundaryPacingSpikeCount = 0;
uint64_t g_PlannedPresentGapCount = 0;
bool g_PendingPlannedPresentGap = false;

double QpcToSeconds(LONGLONG deltaTicks) {
    return static_cast<double>(deltaTicks) / static_cast<double>(g_QpcFreq.QuadPart);
}

LARGE_INTEGER QueryQpc() {
    LARGE_INTEGER qpc = {};
    QueryPerformanceCounter(&qpc);
    return qpc;
}

double SecondsSinceStimulusStart() {
    if (g_StimulusStartQpc.QuadPart == 0) {
        return 0.0;
    }
    const LARGE_INTEGER now = QueryQpc();
    return QpcToSeconds(now.QuadPart - g_StimulusStartQpc.QuadPart);
}

void ScheduleStimulusStart(const char* reason) {
    const LONGLONG preStartTicks =
        static_cast<LONGLONG>(static_cast<double>(g_QpcFreq.QuadPart) * testapp::avsync::kPreStartSeconds);
    const LARGE_INTEGER now = QueryQpc();
    g_StimulusStartQpc.QuadPart = now.QuadPart + preStartTicks;
    testapp::Log(
        "AVSYNC START stimulus_scheduled reason=%s preStartSeconds=%.3f scheduleQpc=%lld stimulusStartQpc=%lld "
        "appStartQpc=%lld setupElapsedMs=%.3f\n",
        reason ? reason : "unspecified", testapp::avsync::kPreStartSeconds, static_cast<long long>(now.QuadPart),
        static_cast<long long>(g_StimulusStartQpc.QuadPart), static_cast<long long>(g_AppStartQpc.QuadPart),
        QpcToSeconds(now.QuadPart - g_AppStartQpc.QuadPart) * 1000.0);
}

double TargetFrameIntervalMs() {
    return 1000.0 / static_cast<double>(std::max(1, g_TargetFps));
}

double FramePacingSpikeThresholdMs() {
    const double nominal = TargetFrameIntervalMs();
    return std::max(nominal * 2.0, nominal + 8.0);
}

void InitializeFrameTimer() {
    g_FrameTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (g_FrameTimer) {
        g_FrameTimerHighResolution = true;
        return;
    }
    g_FrameTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    g_FrameTimerHighResolution = false;
    if (!g_FrameTimer) {
        testapp::Log("AVSYNC WARNING frame timer unavailable gle=%lu; using cooperative yield pacing\n",
                     GetLastError());
    }
}

void CloseFrameTimer() {
    if (g_FrameTimer) {
        CloseHandle(g_FrameTimer);
        g_FrameTimer = nullptr;
    }
    g_FrameTimerHighResolution = false;
}

bool WaitWithFrameTimer(LONGLONG remainingTicks) {
    if (!g_FrameTimer || g_QpcFreq.QuadPart <= 0) {
        return false;
    }
    const LONGLONG finalSpinTicks = std::max<LONGLONG>(1, g_QpcFreq.QuadPart / 2000);
    const LONGLONG minTimerTicks = std::max<LONGLONG>(1, g_QpcFreq.QuadPart / 1000);
    if (remainingTicks <= finalSpinTicks + minTimerTicks) {
        return false;
    }
    const LONGLONG waitTicks = remainingTicks - finalSpinTicks;
    const LONGLONG due100ns = std::max<LONGLONG>(1, (waitTicks * 10000000LL) / g_QpcFreq.QuadPart);
    LARGE_INTEGER due = {};
    due.QuadPart = -due100ns;
    if (!SetWaitableTimer(g_FrameTimer, &due, 0, nullptr, nullptr, FALSE)) {
        return false;
    }
    WaitForSingleObject(g_FrameTimer, INFINITE);
    return true;
}

std::wstring ExeSiblingPath(const wchar_t* fileName) {
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring path = modulePath;
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        path = path.substr(0, slash + 1);
    } else {
        path.clear();
    }
    return path + fileName;
}

#include "dx12_av_sync_setup.inl"
#include "dx12_av_sync_scene.inl"
int UpdateSourceStalls(double stimulusSeconds) {
    int activeIndex = -1;
    const LARGE_INTEGER nowQpc = QueryQpc();
    for (size_t i = 0; i < g_SourceStalls.size(); ++i) {
        auto& stall = g_SourceStalls[i];
        if (stall.completed) {
            continue;
        }
        const bool shouldBeActive =
            stimulusSeconds >= stall.spec.startSeconds && stimulusSeconds < stall.spec.EndSeconds();
        if (shouldBeActive) {
            if (!stall.active) {
                stall.active = true;
                stall.actualBeginQpc = nowQpc;
                testapp::Log(
                    "AVSYNC SOURCE_STALL_BEGIN index=%zu requestedStart=%.6f requestedDurationMs=%.3f "
                    "qpc=%lld stimulusSeconds=%.6f frameId=%llu\n",
                    i, stall.spec.startSeconds, stall.spec.durationSeconds * 1000.0,
                    static_cast<long long>(stall.actualBeginQpc.QuadPart), stimulusSeconds,
                    static_cast<unsigned long long>(g_FrameId));
            }
            activeIndex = static_cast<int>(i);
            break;
        }
        if (stall.active || stimulusSeconds >= stall.spec.EndSeconds()) {
            if (stall.active) {
                stall.actualEndQpc = nowQpc;
                const double actualDurationMs =
                    QpcToSeconds(stall.actualEndQpc.QuadPart - stall.actualBeginQpc.QuadPart) * 1000.0;
                testapp::Log(
                    "AVSYNC SOURCE_STALL_END index=%zu qpc=%lld stimulusSeconds=%.6f actualDurationMs=%.3f "
                    "suppressedPresents=%llu frameId=%llu\n",
                    i, static_cast<long long>(stall.actualEndQpc.QuadPart), stimulusSeconds, actualDurationMs,
                    static_cast<unsigned long long>(stall.suppressedPresentCount),
                    static_cast<unsigned long long>(g_FrameId));
            }
            stall.completed = true;
        }
    }
    return activeIndex;
}

void FinalizeSourceStalls() {
    const LARGE_INTEGER nowQpc = QueryQpc();
    const double stimulusSeconds = SecondsSinceStimulusStart();
    for (size_t i = 0; i < g_SourceStalls.size(); ++i) {
        auto& stall = g_SourceStalls[i];
        if (!stall.active || stall.completed) {
            continue;
        }
        stall.actualEndQpc = nowQpc;
        stall.completed = true;
        const double actualDurationMs =
            QpcToSeconds(stall.actualEndQpc.QuadPart - stall.actualBeginQpc.QuadPart) * 1000.0;
        testapp::Log(
            "AVSYNC SOURCE_STALL_END index=%zu qpc=%lld stimulusSeconds=%.6f actualDurationMs=%.3f "
            "suppressedPresents=%llu frameId=%llu finalized=1\n",
            i, static_cast<long long>(stall.actualEndQpc.QuadPart), stimulusSeconds, actualDurationMs,
            static_cast<unsigned long long>(stall.suppressedPresentCount), static_cast<unsigned long long>(g_FrameId));
    }
}

void PaceNextFrame(double* nextFrameQpcTicks) {
    const double interval = static_cast<double>(g_QpcFreq.QuadPart) / static_cast<double>(std::max(1, g_TargetFps));
    *nextFrameQpcTicks += interval;
    const double nowTicks = static_cast<double>(QueryQpc().QuadPart);
    if (*nextFrameQpcTicks < nowTicks - interval) {
        *nextFrameQpcTicks = nowTicks;
    }
    for (;;) {
        const LARGE_INTEGER now = QueryQpc();
        const LONGLONG remaining =
            static_cast<LONGLONG>(std::llround(*nextFrameQpcTicks - static_cast<double>(now.QuadPart)));
        if (remaining <= 0) {
            break;
        }
        if (!WaitWithFrameTimer(remaining)) {
            if (remaining <= std::max<LONGLONG>(1, g_QpcFreq.QuadPart / 2000)) {
                YieldProcessor();
                continue;
            }
            Sleep(0);
        }
    }
}

void KeepStimulusWindowTopmost(HWND hwnd, bool activate) {
    if (!hwnd || !g_Topmost) {
        return;
    }
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW | (activate ? 0 : SWP_NOACTIVATE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
    if (activate) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd);
    }
}

void WriteManifest() {
    const std::wstring path = ExeSiblingPath(L"dx12_av_sync_test_manifest.json");
    FILE* out = _wfopen(path.c_str(), L"w");
    if (!out) {
        testapp::Log("AVSYNC WARNING could not write manifest\n");
        return;
    }
    fprintf(out, "{\n");
    fprintf(out, "  \"schema\": \"ce-avsync-stimulus-v1\",\n");
    fprintf(out, "  \"process_id\": %lu,\n", GetCurrentProcessId());
    fprintf(out, "  \"requested_width\": %d,\n", g_RequestedWidth);
    fprintf(out, "  \"requested_height\": %d,\n", g_RequestedHeight);
    fprintf(out, "  \"width\": %d,\n", g_WindowWidth);
    fprintf(out, "  \"height\": %d,\n", g_WindowHeight);
    fprintf(out, "  \"fullscreen\": %d,\n", g_Fullscreen ? 1 : 0);
    fprintf(out, "  \"window_chrome\": %d,\n", g_WindowChrome ? 1 : 0);
    fprintf(out, "  \"topmost\": %d,\n", g_Topmost ? 1 : 0);
    fprintf(out, "  \"borderless_windowed\": %d,\n", (!g_Fullscreen && !g_WindowChrome) ? 1 : 0);
    fprintf(out, "  \"vsync\": %d,\n", g_VSync ? 1 : 0);
    fprintf(out, "  \"allow_tearing_requested\": %d,\n", g_TearingRequested ? 1 : 0);
    fprintf(out, "  \"allow_tearing_supported\": %d,\n", g_TearingSupported ? 1 : 0);
    fprintf(out, "  \"allow_tearing_active\": %d,\n", g_TearingActive ? 1 : 0);
    fprintf(out, "  \"present_sync_interval\": %d,\n", g_VSync ? 1 : 0);
    fprintf(out, "  \"target_fps\": %d,\n", g_TargetFps);
    fprintf(out, "  \"duration_seconds\": %d,\n", g_DurationSeconds);
    fprintf(out, "  \"analysis_start_seconds\": %.6f,\n", g_AnalysisStartSeconds);
    fprintf(out, "  \"pre_start_seconds\": %.6f,\n", testapp::avsync::kPreStartSeconds);
    fprintf(out, "  \"event_period_seconds\": %.6f,\n", testapp::avsync::kEventPeriodSeconds);
    fprintf(out, "  \"visual_marker_version\": %d,\n", testapp::avsync::kVisualMarkerVersion);
    fprintf(out, "  \"encoder_stress_scene\": %d,\n", g_EncoderStressScene ? 1 : 0);
    fprintf(out, "  \"encoder_stress_scene_reserved_event_sample\": [0.40, 0.38, 0.60, 0.52],\n");
    const auto stressLayout = testapp::avsync::ComputeEncoderStressLayout(g_WindowWidth, g_WindowHeight, kMarkerMargin,
                                                                          kMarkerTile, kMarkerGap);
    fprintf(out,
            "  \"encoder_stress_layout\": {\"valid\": %d, \"tile\": %d, \"left\": %d, \"right\": %d, "
            "\"top\": %d, \"bottom\": %d, \"columns\": %d, \"rows\": %d, "
            "\"reserve\": [%d, %d, %d, %d]},\n",
            stressLayout.valid ? 1 : 0, stressLayout.tile, stressLayout.left, stressLayout.right, stressLayout.top,
            stressLayout.bottom, stressLayout.columns, stressLayout.rows, stressLayout.reserveLeft,
            stressLayout.reserveTop, stressLayout.reserveRight, stressLayout.reserveBottom);
    fprintf(out, "  \"qpc_frequency\": %lld,\n", static_cast<long long>(g_QpcFreq.QuadPart));
    fprintf(out, "  \"app_start_qpc\": %lld,\n", static_cast<long long>(g_AppStartQpc.QuadPart));
    fprintf(out, "  \"stimulus_start_qpc\": %lld,\n", static_cast<long long>(g_StimulusStartQpc.QuadPart));
    fprintf(out, "  \"events\": [\n");
    for (int i = 0; i < static_cast<int>(testapp::avsync::kPalette.size()); ++i) {
        const auto state = testapp::avsync::StateAt(static_cast<double>(i) * testapp::avsync::kEventPeriodSeconds);
        fprintf(out,
                "    {\"index\": %d, \"palette\": %d, \"time\": %.6f, \"frequency_hz\": %.3f, "
                "\"rgb\": [%u, %u, %u]}%s\n",
                i, state.paletteIndex, state.eventStartSeconds, state.frequencyHz, state.color.r, state.color.g,
                state.color.b, i + 1 == static_cast<int>(testapp::avsync::kPalette.size()) ? "" : ",");
    }
    fprintf(out, "  ],\n");
    fprintf(out, "  \"source_stalls\": [\n");
    for (size_t i = 0; i < g_SourceStalls.size(); ++i) {
        const auto& stall = g_SourceStalls[i];
        const double actualStart = stall.actualBeginQpc.QuadPart > 0
                                       ? QpcToSeconds(stall.actualBeginQpc.QuadPart - g_StimulusStartQpc.QuadPart)
                                       : -1.0;
        const double actualEnd = stall.actualEndQpc.QuadPart > 0
                                     ? QpcToSeconds(stall.actualEndQpc.QuadPart - g_StimulusStartQpc.QuadPart)
                                     : -1.0;
        const double expectedRepeatSpan =
            static_cast<double>(stall.suppressedPresentCount) / static_cast<double>(std::max(1, g_TargetFps));
        fprintf(out,
                "    {\"index\": %zu, \"requested_start_seconds\": %.6f, \"requested_duration_ms\": %.3f, "
                "\"requested_end_seconds\": %.6f, \"actual_begin_qpc\": %lld, \"actual_end_qpc\": %lld, "
                "\"actual_start_seconds\": %.6f, \"actual_end_seconds\": %.6f, "
                "\"suppressed_present_count\": %llu, \"expected_repeat_span_seconds\": %.6f, "
                "\"tolerance_seconds\": %.6f}%s\n",
                i, stall.spec.startSeconds, stall.spec.durationSeconds * 1000.0, stall.spec.EndSeconds(),
                static_cast<long long>(stall.actualBeginQpc.QuadPart),
                static_cast<long long>(stall.actualEndQpc.QuadPart), actualStart, actualEnd,
                static_cast<unsigned long long>(stall.suppressedPresentCount), expectedRepeatSpan,
                testapp::avsync::kDefaultSourceStallToleranceSeconds, i + 1 == g_SourceStalls.size() ? "" : ",");
    }
    fprintf(out, "  ],\n");
    fprintf(out, "  \"frame_marker_bits\": %d,\n", testapp::avsync::kFrameMarkerBits);
    fprintf(out, "  \"frame_marker_redundancy\": \"primary_inverse_checksum_parity\",\n");
    fprintf(out, "  \"marker_tile\": %ld,\n", static_cast<long>(kMarkerTile));
    fprintf(out, "  \"marker_margin\": %ld,\n", static_cast<long>(kMarkerMargin));
    fprintf(out, "  \"marker_gap\": %ld,\n", static_cast<long>(kMarkerGap));
    fprintf(out, "  \"motion_lane_margin\": %ld,\n", static_cast<long>(kMarkerMargin));
    fprintf(out, "  \"motion_lane_count\": %d,\n", 2);
    fprintf(out, "  \"motion_lane_top_ratio\": %.6f,\n", 0.72);
    fprintf(out, "  \"motion_lane_height\": %d,\n", 42);
    fprintf(out, "  \"motion_lane_bar_width\": %d,\n", 96);
    fprintf(out, "  \"motion_lane_speed_cycles_per_second\": %.6f,\n", 0.25);
    fprintf(out, "  \"motion_lane_expected_function\": \"fmod(stimulus_seconds*0.25,1.0)\",\n");
    fprintf(out, "  \"fast_motion_lane_top_ratio\": %.6f,\n", 0.82);
    fprintf(out, "  \"fast_motion_lane_bar_width\": %d,\n", 48);
    fprintf(out, "  \"fast_motion_lane_speed_cycles_per_second\": %.6f,\n", 1.0);
    fprintf(out, "  \"fast_motion_lane_expected_function\": \"fmod(stimulus_seconds*1.0,1.0)\",\n");
    fprintf(out, "  \"audio_requested_buffer_ms\": %d,\n", g_AudioBufferMs);
    fprintf(out, "  \"audio_stimulus_lead_ms\": %.3f,\n", g_Audio.AudioLeadMs());
    fprintf(out, "  \"audio_render_latency_us\": %llu,\n",
            static_cast<unsigned long long>(g_Audio.StreamLatency100ns() / 10));
    fprintf(out, "  \"audio_buffer_frames\": %u,\n", g_Audio.BufferFrames());
    fprintf(out, "  \"audio_clock_available\": %d,\n", g_Audio.HasAudioClock() ? 1 : 0);
    fprintf(out, "  \"audio_clock_scheduling\": %d,\n", g_Audio.AudioClockSchedulingEnabled() ? 1 : 0);
    fprintf(out, "  \"audio_clock_frequency\": %llu,\n",
            static_cast<unsigned long long>(g_Audio.AudioClockFrequency()));
    const double avgPresentDeltaMs =
        g_PresentDeltaCount == 0 ? 0.0 : g_PresentDeltaSumMs / static_cast<double>(g_PresentDeltaCount);
    fprintf(out, "  \"frame_pacing\": {\n");
    fprintf(out, "    \"timer_high_resolution\": %d,\n", g_FrameTimerHighResolution ? 1 : 0);
    fprintf(out, "    \"target_interval_ms\": %.6f,\n", TargetFrameIntervalMs());
    fprintf(out, "    \"spike_threshold_ms\": %.6f,\n", FramePacingSpikeThresholdMs());
    fprintf(out, "    \"present_delta_count\": %llu,\n", static_cast<unsigned long long>(g_PresentDeltaCount));
    fprintf(out, "    \"average_present_delta_ms\": %.6f,\n", avgPresentDeltaMs);
    fprintf(out, "    \"max_present_delta_ms\": %.6f,\n", g_MaxPresentDeltaMs);
    fprintf(out, "    \"spike_count\": %llu,\n", static_cast<unsigned long long>(g_FramePacingSpikeCount));
    fprintf(out, "    \"warmup_spike_count\": %llu,\n", static_cast<unsigned long long>(g_WarmupPacingSpikeCount));
    fprintf(out, "    \"event_boundary_spike_count\": %llu,\n",
            static_cast<unsigned long long>(g_EventBoundaryPacingSpikeCount));
    fprintf(out, "    \"planned_source_gap_count\": %llu\n", static_cast<unsigned long long>(g_PlannedPresentGapCount));
    fprintf(out, "  }\n");
    fprintf(out, "}\n");
    fclose(out);
    testapp::Log("AVSYNC START manifest=%ls\n", path.c_str());
}

void LogEventSchedule() {
    for (int i = 0; i < static_cast<int>(testapp::avsync::kPalette.size()); ++i) {
        const auto state = testapp::avsync::StateAt(static_cast<double>(i) * testapp::avsync::kEventPeriodSeconds);
        testapp::Log("AVSYNC EVENT index=%d palette=%d time=%.6f freq=%.1f rgb=%u,%u,%u\n", i, state.paletteIndex,
                     state.eventStartSeconds, state.frequencyHz, state.color.r, state.color.g, state.color.b);
    }
}

void LogSourceStallSchedule() {
    for (size_t i = 0; i < g_SourceStalls.size(); ++i) {
        const auto& stall = g_SourceStalls[i];
        testapp::Log(
            "AVSYNC EVENT source_stall index=%zu requestedStart=%.6f requestedDurationMs=%.3f requestedEnd=%.6f "
            "toleranceSeconds=%.6f\n",
            i, stall.spec.startSeconds, stall.spec.durationSeconds * 1000.0, stall.spec.EndSeconds(),
            testapp::avsync::kDefaultSourceStallToleranceSeconds);
    }
}

void Cleanup() {
    WaitForGpu();
    if (g_FenceEvent) {
        CloseHandle(g_FenceEvent);
        g_FenceEvent = nullptr;
    }
    for (auto& target : g_RenderTargets) {
        target.Reset();
    }
    for (auto& allocator : g_CommandAllocators) {
        allocator.Reset();
    }
    g_CommandList.Reset();
    g_RtvHeap.Reset();
    g_SwapChain.Reset();
    g_CommandQueue.Reset();
    g_Device.Reset();
}

}  // namespace

int main(int argc, char** argv) {
    if (testapp::LaunchX86SiblingProcess(argc, argv)) {
        return 0;
    }

    testapp::EnableGameDpiAwareness();
    testapp::ApplyGameScheduling();
    timeBeginPeriod(1);
    testapp::OpenLogFile();
    QueryPerformanceFrequency(&g_QpcFreq);
    QueryPerformanceCounter(&g_AppStartQpc);

    LoadConfig();
    ParseArgs(argc, argv);
    g_RequestedWidth = g_WindowWidth;
    g_RequestedHeight = g_WindowHeight;

    testapp::Log(
        "AVSYNC START app pid=%lu width=%d height=%d fps=%d duration=%d analysisStart=%.3f vsync=%d fullscreen=%d "
        "windowChrome=%d topmost=%d tearingRequested=%d gpuLoad=%d audio=%d audioClockScheduling=%d "
        "audioBufferMs=%d audioLeadMs=%.3f encoderStressScene=%d qpcFreq=%lld appStartQpc=%lld "
        "stimulusStartQpc=pending\n",
        GetCurrentProcessId(), g_WindowWidth, g_WindowHeight, g_TargetFps, g_DurationSeconds, g_AnalysisStartSeconds,
        g_VSync, g_Fullscreen, g_WindowChrome, g_Topmost, g_TearingRequested ? 1 : 0, g_GpuLoadPasses,
        g_AudioEnabled ? 1 : 0, g_AudioClockScheduling ? 1 : 0, g_AudioBufferMs, g_AudioLeadMs,
        g_EncoderStressScene ? 1 : 0, static_cast<long long>(g_QpcFreq.QuadPart),
        static_cast<long long>(g_AppStartQpc.QuadPart));
    LogEventSchedule();
    LogSourceStallSchedule();
    InitializeFrameTimer();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CaptureTestDX12AVSync";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    const bool borderlessWindowed = !g_Fullscreen && !g_WindowChrome;
    DWORD style = borderlessWindowed ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    DWORD exStyle = 0;
    int windowX = CW_USEDEFAULT;
    int windowY = CW_USEDEFAULT;
    int windowW = g_WindowWidth;
    int windowH = g_WindowHeight;
    if (g_Topmost) {
        exStyle |= WS_EX_TOPMOST;
    }
    if (g_Fullscreen) {
        style = WS_POPUP;
        exStyle |= WS_EX_TOPMOST;
        const RECT monitor = testapp::GetPrimaryMonitorRect();
        windowX = monitor.left;
        windowY = monitor.top;
        windowW = monitor.right - monitor.left;
        windowH = monitor.bottom - monitor.top;
        g_WindowWidth = windowW;
        g_WindowHeight = windowH;
    } else if (borderlessWindowed) {
        const RECT monitor = testapp::GetPrimaryMonitorRect();
        const int monitorW = monitor.right - monitor.left;
        const int monitorH = monitor.bottom - monitor.top;
        windowW = g_WindowWidth;
        windowH = g_WindowHeight;
        windowX = monitor.left + std::max(0, (monitorW - windowW) / 2);
        windowY = monitor.top + std::max(0, (monitorH - windowH) / 2);
    } else {
        RECT adjusted = testapp::AdjustWindowRectForClientSize(style, exStyle, g_WindowWidth, g_WindowHeight);
        windowW = adjusted.right - adjusted.left;
        windowH = adjusted.bottom - adjusted.top;
    }
    testapp::Log(
        "AVSYNC START window requested=%dx%d render=%dx%d fullscreen=%d windowChrome=%d topmost=%d borderless=%d\n",
        g_RequestedWidth, g_RequestedHeight, g_WindowWidth, g_WindowHeight, g_Fullscreen, g_WindowChrome, g_Topmost,
        borderlessWindowed ? 1 : 0);

    HWND hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"DX12 A/V Sync Test", style, windowX, windowY, windowW,
                                windowH, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        testapp::Log("AVSYNC WARNING CreateWindowExW failed gle=%lu\n", GetLastError());
        CloseFrameTimer();
        timeEndPeriod(1);
        testapp::CloseLogFile();
        return 1;
    }

    if (!testapp::PrimeWindowForBenchmark(hwnd, g_Fullscreen != 0, g_WindowWidth, g_WindowHeight, 500)) {
        testapp::Log("AVSYNC WARNING PrimeWindowForBenchmark failed\n");
    }
    KeepStimulusWindowTopmost(hwnd, true);
    if (!InitDX12(hwnd)) {
        CloseFrameTimer();
        timeEndPeriod(1);
        testapp::CloseLogFile();
        return 1;
    }

    ScheduleStimulusStart("post_dx12_init");
    WriteManifest();

    if (g_AudioEnabled) {
        g_Audio.SetAudioClockScheduling(g_AudioClockScheduling);
        g_Audio.SetBufferDurationMs(g_AudioBufferMs);
        g_Audio.SetAudioLeadMs(g_AudioLeadMs);
        g_Audio.Start();
        const DWORD waitStart = GetTickCount();
        while (!g_Audio.IsReady() && GetTickCount() - waitStart < 3000) {
            Sleep(10);
        }
        if (g_Audio.HadError()) {
            testapp::Log("AVSYNC WARNING audio renderer failed; visual stimulus continues\n");
        }
    }

    MSG msg = {};
    double nextFrameQpcTicks = static_cast<double>(QueryQpc().QuadPart);
    LARGE_INTEGER nextTopmostRefreshQpc = QueryQpc();
    const double stopAtStimulusSeconds = static_cast<double>(g_DurationSeconds);
    while (g_Running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                g_Running = false;
            }
        }
        if (!g_Running) {
            break;
        }
        if (SecondsSinceStimulusStart() >= stopAtStimulusSeconds) {
            testapp::Log("AVSYNC SUMMARY auto_exit stimulusSeconds=%.6f frameId=%llu\n", SecondsSinceStimulusStart(),
                         static_cast<unsigned long long>(g_FrameId));
            break;
        }
        const LARGE_INTEGER nowQpc = QueryQpc();
        if (g_Topmost && nowQpc.QuadPart >= nextTopmostRefreshQpc.QuadPart) {
            KeepStimulusWindowTopmost(hwnd, true);
            nextTopmostRefreshQpc.QuadPart = nowQpc.QuadPart + g_QpcFreq.QuadPart;
        }
        const int activeStall = UpdateSourceStalls(SecondsSinceStimulusStart());
        if (activeStall >= 0) {
            g_PendingPlannedPresentGap = true;
            g_SourceStalls[static_cast<size_t>(activeStall)].suppressedPresentCount++;
            if (g_LogEveryFrame) {
                testapp::Log("AVSYNC FRAME_SUPPRESSED sourceStall=%d stimulusSeconds=%.6f frameId=%llu\n", activeStall,
                             SecondsSinceStimulusStart(), static_cast<unsigned long long>(g_FrameId));
            }
        } else {
            RenderFrame();
        }
        PaceNextFrame(&nextFrameQpcTicks);
    }

    FinalizeSourceStalls();
    if (g_AudioEnabled) {
        g_Audio.Stop();
    }
    uint64_t suppressedPresents = 0;
    for (const auto& stall : g_SourceStalls) {
        suppressedPresents += stall.suppressedPresentCount;
    }
    testapp::Log("AVSYNC SUMMARY app frames=%llu suppressedPresents=%llu sourceStalls=%zu finalStimulusSeconds=%.6f\n",
                 static_cast<unsigned long long>(g_FrameId), static_cast<unsigned long long>(suppressedPresents),
                 g_SourceStalls.size(), SecondsSinceStimulusStart());
    const double avgPresentDeltaMs =
        g_PresentDeltaCount == 0 ? 0.0 : g_PresentDeltaSumMs / static_cast<double>(g_PresentDeltaCount);
    testapp::Log(
        "AVSYNC SUMMARY frame_timing targetFps=%d timerHighRes=%d deltas=%llu avgDeltaMs=%.3f maxDeltaMs=%.3f "
        "spikes=%llu warmupSpikes=%llu eventBoundarySpikes=%llu plannedSourceGaps=%llu thresholdMs=%.3f\n",
        g_TargetFps, g_FrameTimerHighResolution ? 1 : 0, static_cast<unsigned long long>(g_PresentDeltaCount),
        avgPresentDeltaMs, g_MaxPresentDeltaMs, static_cast<unsigned long long>(g_FramePacingSpikeCount),
        static_cast<unsigned long long>(g_WarmupPacingSpikeCount),
        static_cast<unsigned long long>(g_EventBoundaryPacingSpikeCount),
        static_cast<unsigned long long>(g_PlannedPresentGapCount), FramePacingSpikeThresholdMs());
    WriteManifest();
    Cleanup();
    CloseFrameTimer();
    timeEndPeriod(1);
    testapp::CloseLogFile();
    return 0;
}
