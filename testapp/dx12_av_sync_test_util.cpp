#include "dx12_av_sync_test_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include <mmsystem.h>

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
int ClampInt(int value, int lo, int hi) {
    return std::max(lo, std::min(value, hi));
}
void LoadConfig() {
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring configPath = modulePath;
    const size_t slash = configPath.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        configPath = configPath.substr(0, slash + 1) + L"testappconfig.ini";
    }

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_WindowWidth = GetPrivateProfileIntW(L"Display", L"width", g_WindowWidth, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_WindowHeight = GetPrivateProfileIntW(L"Display", L"height", g_WindowHeight, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_Fullscreen = GetPrivateProfileIntW(L"Display", L"fullscreen", g_Fullscreen, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_WindowChrome = GetPrivateProfileIntW(L"Display", L"window_chrome", g_WindowChrome, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_Topmost = GetPrivateProfileIntW(L"Display", L"topmost", g_Topmost, configPath.c_str());
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_VSync = GetPrivateProfileIntW(L"Rendering", L"vsync", g_VSync, configPath.c_str());
    g_TearingRequested =
        GetPrivateProfileIntW(L"Rendering", L"allow_tearing", g_TearingRequested ? 1 : 0, configPath.c_str()) != 0;
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_GpuLoadPasses = GetPrivateProfileIntW(L"Performance", L"gpu_load", g_GpuLoadPasses, configPath.c_str());
    g_EncoderStressScene = GetPrivateProfileIntW(L"Performance", L"encoder_stress_scene", g_EncoderStressScene ? 1 : 0,
                                                 configPath.c_str()) != 0;
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_TargetFps = ClampInt(GetPrivateProfileIntW(L"AVSync", L"fps", g_TargetFps, configPath.c_str()), 1, 480);
    g_DurationSeconds =
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        ClampInt(GetPrivateProfileIntW(L"AVSync", L"duration_seconds", g_DurationSeconds, configPath.c_str()), 1, 3600);
    g_LogEveryFrame = GetPrivateProfileIntW(L"AVSync", L"log_every_frame", 0, configPath.c_str()) != 0;
    g_AudioEnabled = GetPrivateProfileIntW(L"AVSync", L"audio", 1, configPath.c_str()) != 0;
    g_AudioClockScheduling = GetPrivateProfileIntW(L"AVSync", L"audio_clock_scheduling", 0, configPath.c_str()) != 0;
    g_AudioBufferMs = testapp::avsync::ClampAudioBufferMs(
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        GetPrivateProfileIntW(L"AVSync", L"audio_buffer_ms", g_AudioBufferMs, configPath.c_str()));
    wchar_t audioLeadText[64] = {};
    GetPrivateProfileStringW(L"AVSync", L"audio_lead_ms", L"", audioLeadText,
                             static_cast<DWORD>(sizeof(audioLeadText) / sizeof(audioLeadText[0])), configPath.c_str());
    if (audioLeadText[0] != L'\0') {
        wchar_t* end = nullptr;
        const double parsed = std::wcstod(audioLeadText, &end);
        if (end && end != audioLeadText && std::isfinite(parsed)) {
            g_AudioLeadMs = testapp::avsync::ClampAudioLeadMs(parsed);
        }
    }
    wchar_t analysisStartText[64] = {};
    GetPrivateProfileStringW(L"AVSync", L"analysis_start_seconds", L"", analysisStartText,
                             static_cast<DWORD>(sizeof(analysisStartText) / sizeof(analysisStartText[0])),
                             configPath.c_str());
    if (analysisStartText[0] != L'\0') {
        wchar_t* end = nullptr;
        const double parsed = std::wcstod(analysisStartText, &end);
        if (end && end != analysisStartText && std::isfinite(parsed)) {
            g_AnalysisStartSeconds = parsed;
        }
    }
}
void ParseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        auto readInt = [&](const char* separate, const char* prefix, int* out) {
            if (strcmp(argv[i], separate) == 0 && i + 1 < argc) {
                *out = testapp::ParseIntOrZero(argv[++i]);
                return true;
            }
            const size_t prefixLen = strlen(prefix);
            if (strncmp(argv[i], prefix, prefixLen) == 0) {
                *out = testapp::ParseIntOrZero(argv[i] + prefixLen);
                return true;
            }
            return false;
        };
        auto readDouble = [&](const char* separate, const char* prefix, double* out) {
            if (strcmp(argv[i], separate) == 0 && i + 1 < argc) {
                *out = std::strtod(argv[++i], nullptr);
                return true;
            }
            const size_t prefixLen = strlen(prefix);
            if (strncmp(argv[i], prefix, prefixLen) == 0) {
                *out = std::strtod(argv[i] + prefixLen, nullptr);
                return true;
            }
            return false;
        };

        if (readInt("--width", "--width=", &g_WindowWidth) || readInt("--height", "--height=", &g_WindowHeight) ||
            readInt("--fps", "--fps=", &g_TargetFps) || readInt("--duration", "--duration=", &g_DurationSeconds) ||
            readInt("--gpu-load", "--gpu-load=", &g_GpuLoadPasses) || readInt("--vsync", "--vsync=", &g_VSync) ||
            readInt("--fullscreen", "--fullscreen=", &g_Fullscreen) ||
            readInt("--window-chrome", "--window-chrome=", &g_WindowChrome) ||
            readInt("--topmost", "--topmost=", &g_Topmost) ||
            readInt("--audio-buffer-ms", "--audio-buffer-ms=", &g_AudioBufferMs)) {
            continue;
        }
        if (readDouble("--audio-lead-ms", "--audio-lead-ms=", &g_AudioLeadMs)) {
            continue;
        }
        if (readDouble("--analysis-start-sec", "--analysis-start-sec=", &g_AnalysisStartSeconds) ||
            readDouble("--analysis-start-seconds", "--analysis-start-seconds=", &g_AnalysisStartSeconds)) {
            continue;
        }
        if (strcmp(argv[i], "--log-every-frame") == 0) {
            g_LogEveryFrame = true;
        } else if (strcmp(argv[i], "--encoder-stress-scene") == 0) {
            g_EncoderStressScene = true;
        } else if (strcmp(argv[i], "--no-encoder-stress-scene") == 0) {
            g_EncoderStressScene = false;
        } else if (strcmp(argv[i], "--allow-tearing") == 0) {
            g_TearingRequested = true;
        } else if (strcmp(argv[i], "--no-allow-tearing") == 0) {
            g_TearingRequested = false;
        } else if (strcmp(argv[i], "--no-audio") == 0) {
            g_AudioEnabled = false;
        } else if (strcmp(argv[i], "--audio-clock-scheduling") == 0) {
            g_AudioClockScheduling = true;
        } else if (strcmp(argv[i], "--source-stall") == 0 && i + 1 < argc) {
            testapp::avsync::SourceStallSpec spec;
            if (testapp::avsync::ParseSourceStallSpec(argv[++i], &spec)) {
                g_SourceStalls.push_back({spec});
            } else {
                testapp::Log("AVSYNC WARNING invalid source stall spec=%s\n", argv[i]);
            }
        } else if (strncmp(argv[i], "--source-stall=", 15) == 0) {
            testapp::avsync::SourceStallSpec spec;
            if (testapp::avsync::ParseSourceStallSpec(argv[i] + 15, &spec)) {
                g_SourceStalls.push_back({spec});
            } else {
                testapp::Log("AVSYNC WARNING invalid source stall spec=%s\n", argv[i] + 15);
            }
        }
    }

    g_WindowWidth = ClampInt(g_WindowWidth, 320, 7680);
    g_WindowHeight = ClampInt(g_WindowHeight, 240, 4320);
    g_TargetFps = ClampInt(g_TargetFps, 1, 480);
    g_DurationSeconds = ClampInt(g_DurationSeconds, 1, 3600);
    g_GpuLoadPasses = ClampInt(g_GpuLoadPasses, 0, 10000);
    g_Fullscreen = g_Fullscreen ? 1 : 0;
    g_WindowChrome = g_WindowChrome ? 1 : 0;
    g_Topmost = g_Topmost ? 1 : 0;
    g_AudioBufferMs = testapp::avsync::ClampAudioBufferMs(g_AudioBufferMs);
    g_AudioLeadMs = testapp::avsync::ClampAudioLeadMs(g_AudioLeadMs);
    g_AnalysisStartSeconds =
        testapp::avsync::ClampAnalysisStartSeconds(g_AnalysisStartSeconds, static_cast<double>(g_DurationSeconds));
    std::sort(g_SourceStalls.begin(), g_SourceStalls.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.spec.startSeconds < rhs.spec.startSeconds; });
}
void RecordPresentTiming(const LARGE_INTEGER& presentQpc, int eventIndex) {
    if (g_LastPresentQpc.QuadPart != 0) {
        const double deltaMs = QpcToSeconds(presentQpc.QuadPart - g_LastPresentQpc.QuadPart) * 1000.0;
        g_LastPresentDeltaMs = deltaMs;
        g_MaxPresentDeltaMs = std::max(g_MaxPresentDeltaMs, deltaMs);
        g_PresentDeltaSumMs += deltaMs;
        ++g_PresentDeltaCount;

        const bool eventBoundary =
            g_LastPresentEventIndex != -9999 && eventIndex >= 0 && eventIndex != g_LastPresentEventIndex;
        const double thresholdMs = FramePacingSpikeThresholdMs();
        if (deltaMs > thresholdMs) {
            const double stimulusSeconds = SecondsSinceStimulusStart();
            const bool strictAnalysisWindow = stimulusSeconds >= g_AnalysisStartSeconds;
            if (g_PendingPlannedPresentGap) {
                ++g_PlannedPresentGapCount;
                testapp::Log(
                    "AVSYNC FRAME_TIMING planned_source_gap frameId=%llu deltaMs=%.3f thresholdMs=%.3f "
                    "eventBoundary=%d previousEvent=%d event=%d\n",
                    static_cast<unsigned long long>(g_FrameId), deltaMs, thresholdMs, eventBoundary ? 1 : 0,
                    g_LastPresentEventIndex, eventIndex);
            } else if (!strictAnalysisWindow) {
                ++g_WarmupPacingSpikeCount;
                testapp::Log(
                    "AVSYNC FRAME_TIMING warmup_spike frameId=%llu deltaMs=%.3f thresholdMs=%.3f "
                    "eventBoundary=%d previousEvent=%d event=%d stimulusSeconds=%.6f analysisStart=%.6f\n",
                    static_cast<unsigned long long>(g_FrameId), deltaMs, thresholdMs, eventBoundary ? 1 : 0,
                    g_LastPresentEventIndex, eventIndex, stimulusSeconds, g_AnalysisStartSeconds);
            } else {
                ++g_FramePacingSpikeCount;
                if (eventBoundary) {
                    ++g_EventBoundaryPacingSpikeCount;
                }
                testapp::Log(
                    "AVSYNC WARNING frame pacing spike frameId=%llu deltaMs=%.3f thresholdMs=%.3f "
                    "eventBoundary=%d previousEvent=%d event=%d stimulusSeconds=%.6f\n",
                    static_cast<unsigned long long>(g_FrameId), deltaMs, thresholdMs, eventBoundary ? 1 : 0,
                    g_LastPresentEventIndex, eventIndex, stimulusSeconds);
            }
        }
    }
    g_LastPresentQpc = presentQpc;
    g_LastPresentEventIndex = eventIndex;
    g_PendingPlannedPresentGap = false;
}
void MoveToNextFrame() {
    std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
    const UINT64 currentFenceValue = g_FenceValues[g_FrameIndex];
    g_CommandQueue->Signal(g_Fence.Get(), currentFenceValue);
    const UINT nextFrameIndex = g_SwapChain->GetCurrentBackBufferIndex();
    if (g_Fence->GetCompletedValue() < g_FenceValues[nextFrameIndex]) {
        g_Fence->SetEventOnCompletion(g_FenceValues[nextFrameIndex], g_FenceEvent);
        WaitForSingleObject(g_FenceEvent, INFINITE);
    }
    g_FrameIndex = nextFrameIndex;
    g_FenceValues[g_FrameIndex] = currentFenceValue + 1;
}
void WaitForGpu() {
    std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
    const UINT64 fenceValue = g_FenceValues[g_FrameIndex];
    g_CommandQueue->Signal(g_Fence.Get(), fenceValue);
    if (g_Fence->GetCompletedValue() < fenceValue) {
        g_Fence->SetEventOnCompletion(fenceValue, g_FenceEvent);
        WaitForSingleObject(g_FenceEvent, INFINITE);
    }
    ++g_FenceValues[g_FrameIndex];
}
