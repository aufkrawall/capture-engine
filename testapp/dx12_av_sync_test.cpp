// Deterministic DX12 + WASAPI A/V sync stimulus app.
// Produces visual and audio markers for CaptureEngine CFR sync validation.

#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <mmsystem.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>
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
        testapp::Log("AVSYNC WARNING frame timer unavailable gle=%lu; using cooperative yield pacing\n", GetLastError());
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

    g_WindowWidth = GetPrivateProfileIntW(L"Display", L"width", g_WindowWidth, configPath.c_str());
    g_WindowHeight = GetPrivateProfileIntW(L"Display", L"height", g_WindowHeight, configPath.c_str());
    g_Fullscreen = GetPrivateProfileIntW(L"Display", L"fullscreen", g_Fullscreen, configPath.c_str());
    g_WindowChrome = GetPrivateProfileIntW(L"Display", L"window_chrome", g_WindowChrome, configPath.c_str());
    g_Topmost = GetPrivateProfileIntW(L"Display", L"topmost", g_Topmost, configPath.c_str());
    g_VSync = GetPrivateProfileIntW(L"Rendering", L"vsync", g_VSync, configPath.c_str());
    g_TearingRequested = GetPrivateProfileIntW(L"Rendering", L"allow_tearing", g_TearingRequested ? 1 : 0,
                                               configPath.c_str()) != 0;
    g_GpuLoadPasses = GetPrivateProfileIntW(L"Performance", L"gpu_load", g_GpuLoadPasses, configPath.c_str());
    g_TargetFps = ClampInt(GetPrivateProfileIntW(L"AVSync", L"fps", g_TargetFps, configPath.c_str()), 1, 480);
    g_DurationSeconds =
        ClampInt(GetPrivateProfileIntW(L"AVSync", L"duration_seconds", g_DurationSeconds, configPath.c_str()), 1, 3600);
    g_LogEveryFrame = GetPrivateProfileIntW(L"AVSync", L"log_every_frame", 0, configPath.c_str()) != 0;
    g_AudioEnabled = GetPrivateProfileIntW(L"AVSync", L"audio", 1, configPath.c_str()) != 0;
    g_AudioClockScheduling = GetPrivateProfileIntW(L"AVSync", L"audio_clock_scheduling", 0, configPath.c_str()) != 0;
    g_AudioBufferMs = testapp::avsync::ClampAudioBufferMs(
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
                *out = atoi(argv[++i]);
                return true;
            }
            const size_t prefixLen = strlen(prefix);
            if (strncmp(argv[i], prefix, prefixLen) == 0) {
                *out = atoi(argv[i] + prefixLen);
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
            readInt("--fps", "--fps=", &g_TargetFps) ||
            readInt("--duration", "--duration=", &g_DurationSeconds) ||
            readInt("--gpu-load", "--gpu-load=", &g_GpuLoadPasses) ||
            readInt("--vsync", "--vsync=", &g_VSync) ||
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
    std::sort(g_SourceStalls.begin(), g_SourceStalls.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.spec.startSeconds < rhs.spec.startSeconds;
    });
}

testapp::avsync::AudioRenderer g_Audio(&g_QpcFreq, &g_StimulusStartQpc);

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        case WM_DESTROY:
            g_Running = false;
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                g_Running = false;
                DestroyWindow(hwnd);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
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

bool InitDX12(HWND hwnd) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        testapp::Log("AVSYNC WARNING CreateDXGIFactory1 failed\n");
        return false;
    }

    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        BOOL allowTearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing,
                                                    sizeof(allowTearing)))) {
            g_TearingSupported = allowTearing != FALSE;
        }
    }
    g_TearingActive =
        testapp::avsync::ShouldUseDxgiTearing(g_TearingRequested, g_TearingSupported, g_VSync != 0);

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_Device)))) {
        testapp::Log("AVSYNC WARNING D3D12CreateDevice failed\n");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_CommandQueue)))) {
        testapp::Log("AVSYNC WARNING CreateCommandQueue failed\n");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.BufferCount = kFrameCount;
    scDesc.Width = static_cast<UINT>(g_WindowWidth);
    scDesc.Height = static_cast<UINT>(g_WindowHeight);
    scDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;
    scDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (g_TearingActive) {
        scDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(factory->CreateSwapChainForHwnd(g_CommandQueue.Get(), hwnd, &scDesc, nullptr, nullptr, &swapChain1))) {
        testapp::Log("AVSYNC WARNING CreateSwapChainForHwnd failed\n");
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    swapChain1.As(&g_SwapChain);
    g_FrameIndex = g_SwapChain->GetCurrentBackBufferIndex();

    ComPtr<IDXGISwapChain2> swapChain2;
    if (SUCCEEDED(swapChain1.As(&swapChain2))) {
        swapChain2->SetMaximumFrameLatency(1);
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = kFrameCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_RtvHeap));
    g_RtvDescriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i) {
        g_SwapChain->GetBuffer(i, IID_PPV_ARGS(&g_RenderTargets[i]));
        g_Device->CreateRenderTargetView(g_RenderTargets[i].Get(), nullptr, rtv);
        rtv.ptr += g_RtvDescriptorSize;
        g_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_CommandAllocators[i]));
    }

    g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_CommandAllocators[g_FrameIndex].Get(), nullptr,
                                IID_PPV_ARGS(&g_CommandList));
    g_CommandList->Close();
    g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_Fence));
    ++g_FenceValues[g_FrameIndex];
    g_FenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    testapp::Log(
        "AVSYNC START dx12 width=%d height=%d vsync=%d tearingRequested=%d tearingSupported=%d tearingActive=%d\n",
        g_WindowWidth, g_WindowHeight, g_VSync, g_TearingRequested ? 1 : 0, g_TearingSupported ? 1 : 0,
        g_TearingActive ? 1 : 0);
    return true;
}

void ClearRect(D3D12_CPU_DESCRIPTOR_HANDLE rtv, LONG left, LONG top, LONG right, LONG bottom, const float color[4]) {
    D3D12_RECT rect = {left, top, right, bottom};
    g_CommandList->ClearRenderTargetView(rtv, color, 1, &rect);
}

void DrawMarkerTiles(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint16_t marker, bool inverse) {
    const float one[] = {0.94f, 0.94f, 0.94f, 1.0f};
    const float zero[] = {0.02f, 0.02f, 0.02f, 1.0f};
    const LONG top = inverse ? kMarkerMargin + kMarkerTile + kMarkerGap : kMarkerMargin;
    for (int bit = 0; bit < testapp::avsync::kFrameMarkerBits; ++bit) {
        const bool set = testapp::avsync::FrameMarkerBit(marker, bit) ^ inverse;
        const LONG left = kMarkerMargin + bit * (kMarkerTile + kMarkerGap);
        ClearRect(rtv, left, top, left + kMarkerTile, top + kMarkerTile, set ? one : zero);
    }
}

void DrawMotionLane(D3D12_CPU_DESCRIPTOR_HANDLE rtv, double stimulusSeconds) {
    const LONG laneLeft = kMarkerMargin;
    const LONG laneRight = g_WindowWidth - kMarkerMargin;
    const LONG laneTop = static_cast<LONG>(g_WindowHeight * 0.72);
    const LONG laneBottom = laneTop + 42;
    const float laneColor[] = {0.015f, 0.015f, 0.015f, 1.0f};
    const float barColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ClearRect(rtv, laneLeft, laneTop, laneRight, laneBottom, laneColor);
    const double pos = testapp::avsync::SmoothLanePosition(stimulusSeconds);
    const LONG usable = std::max<LONG>(1, laneRight - laneLeft - 96);
    const LONG x = laneLeft + static_cast<LONG>(pos * static_cast<double>(usable));
    ClearRect(rtv, x, laneTop + 7, x + 96, laneBottom - 7, barColor);
}

void DrawCorruptionSentinels(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const testapp::avsync::StimulusState& state,
                             uint16_t marker) {
    const float white[] = {0.96f, 0.96f, 0.96f, 1.0f};
    const float black[] = {0.02f, 0.02f, 0.02f, 1.0f};
    const LONG size = 28;
    const LONG startX = g_WindowWidth - kMarkerMargin - 8 * (size + 4);
    const LONG startY = g_WindowHeight - kMarkerMargin - size;
    const uint8_t checksum = testapp::avsync::FrameMarkerChecksum(marker, state.paletteIndex);
    for (int bit = 0; bit < 8; ++bit) {
        const bool set = ((checksum >> bit) & 1u) != 0;
        const LONG x = startX + bit * (size + 4);
        ClearRect(rtv, x, startY, x + size, startY + size, set ? white : black);
    }
    const LONG parityX = startX - size - 12;
    const bool parity = testapp::avsync::FrameMarkerParity(marker, state.paletteIndex);
    ClearRect(rtv, parityX, startY, parityX + size, startY + size, parity ? white : black);
}

void RenderFrame() {
    const double stimulusSeconds = SecondsSinceStimulusStart();
    const auto state = testapp::avsync::StateAt(stimulusSeconds);
    const uint16_t marker = testapp::avsync::EncodeFrameMarker(g_FrameId);

    UINT frameIndex = 0;
    {
        std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
        frameIndex = g_FrameIndex;
    }

    g_CommandAllocators[frameIndex]->Reset();
    g_CommandList->Reset(g_CommandAllocators[frameIndex].Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_RenderTargets[frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_CommandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += frameIndex * g_RtvDescriptorSize;

    const float bg[] = {state.color.r / 255.0f, state.color.g / 255.0f, state.color.b / 255.0f, 1.0f};
    const float preStart[] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (int pass = 0; pass < g_GpuLoadPasses; ++pass) {
        const float load[] = {static_cast<float>((pass & 7) + 1) / 64.0f, 0.02f, 0.03f, 1.0f};
        g_CommandList->ClearRenderTargetView(rtv, load, 0, nullptr);
    }
    g_CommandList->ClearRenderTargetView(rtv, state.eventIndex < 0 ? preStart : bg, 0, nullptr);
    DrawMarkerTiles(rtv, marker, false);
    DrawMarkerTiles(rtv, marker, true);
    DrawMotionLane(rtv, stimulusSeconds);
    DrawCorruptionSentinels(rtv, state, marker);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_CommandList->ResourceBarrier(1, &barrier);
    g_CommandList->Close();

    ID3D12CommandList* lists[] = {g_CommandList.Get()};
    g_CommandQueue->ExecuteCommandLists(1, lists);
    const UINT sync = g_VSync ? 1u : 0u;
    const UINT flags = g_TearingActive ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const HRESULT presentHr = g_SwapChain->Present(sync, flags);
    if (FAILED(presentHr)) {
        testapp::Log("AVSYNC WARNING Present failed frameId=%llu hr=0x%08lx\n",
                     static_cast<unsigned long long>(g_FrameId), static_cast<unsigned long>(presentHr));
        g_Running = false;
    }
    const LARGE_INTEGER presentQpc = QueryQpc();
    RecordPresentTiming(presentQpc, state.eventIndex);
    MoveToNextFrame();

    if (g_LogEveryFrame || (g_FrameId % static_cast<uint64_t>(std::max(1, g_TargetFps)) == 0)) {
        testapp::Log(
            "AVSYNC FRAME frameId=%llu marker=%u event=%d palette=%d stimulusSeconds=%.6f "
            "motion=%.6f expectedMotion=%.6f frameIndex=%u presentDeltaMs=%.3f maxPresentDeltaMs=%.3f\n",
            static_cast<unsigned long long>(g_FrameId), marker, state.eventIndex, state.paletteIndex, stimulusSeconds,
            testapp::avsync::SmoothLanePosition(stimulusSeconds),
            testapp::avsync::ExpectedMotionPosition(stimulusSeconds), frameIndex, g_LastPresentDeltaMs,
            g_MaxPresentDeltaMs);
    }
    ++g_FrameId;
}

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
        const double actualDurationMs = QpcToSeconds(stall.actualEndQpc.QuadPart - stall.actualBeginQpc.QuadPart) * 1000.0;
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
        const LONGLONG remaining = static_cast<LONGLONG>(std::llround(*nextFrameQpcTicks - static_cast<double>(now.QuadPart)));
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
        const double actualStart =
            stall.actualBeginQpc.QuadPart > 0 ? QpcToSeconds(stall.actualBeginQpc.QuadPart - g_StimulusStartQpc.QuadPart)
                                              : -1.0;
        const double actualEnd =
            stall.actualEndQpc.QuadPart > 0 ? QpcToSeconds(stall.actualEndQpc.QuadPart - g_StimulusStartQpc.QuadPart)
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
                static_cast<long long>(stall.actualBeginQpc.QuadPart), static_cast<long long>(stall.actualEndQpc.QuadPart),
                actualStart, actualEnd, static_cast<unsigned long long>(stall.suppressedPresentCount),
                expectedRepeatSpan, testapp::avsync::kDefaultSourceStallToleranceSeconds,
                i + 1 == g_SourceStalls.size() ? "" : ",");
    }
    fprintf(out, "  ],\n");
    fprintf(out, "  \"frame_marker_bits\": %d,\n", testapp::avsync::kFrameMarkerBits);
    fprintf(out, "  \"frame_marker_redundancy\": \"primary_inverse_checksum_parity\",\n");
    fprintf(out, "  \"marker_tile\": %ld,\n", static_cast<long>(kMarkerTile));
    fprintf(out, "  \"marker_margin\": %ld,\n", static_cast<long>(kMarkerMargin));
    fprintf(out, "  \"marker_gap\": %ld,\n", static_cast<long>(kMarkerGap));
    fprintf(out, "  \"motion_lane_margin\": %ld,\n", static_cast<long>(kMarkerMargin));
    fprintf(out, "  \"motion_lane_bar_width\": %d,\n", 96);
    fprintf(out, "  \"motion_lane_speed_cycles_per_second\": %.6f,\n", 0.25);
    fprintf(out, "  \"motion_lane_expected_function\": \"fmod(stimulus_seconds*0.25,1.0)\",\n");
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
    fprintf(out, "    \"present_delta_count\": %llu,\n",
            static_cast<unsigned long long>(g_PresentDeltaCount));
    fprintf(out, "    \"average_present_delta_ms\": %.6f,\n", avgPresentDeltaMs);
    fprintf(out, "    \"max_present_delta_ms\": %.6f,\n", g_MaxPresentDeltaMs);
    fprintf(out, "    \"spike_count\": %llu,\n",
            static_cast<unsigned long long>(g_FramePacingSpikeCount));
    fprintf(out, "    \"warmup_spike_count\": %llu,\n",
            static_cast<unsigned long long>(g_WarmupPacingSpikeCount));
    fprintf(out, "    \"event_boundary_spike_count\": %llu,\n",
            static_cast<unsigned long long>(g_EventBoundaryPacingSpikeCount));
    fprintf(out, "    \"planned_source_gap_count\": %llu\n",
            static_cast<unsigned long long>(g_PlannedPresentGapCount));
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
        "audioBufferMs=%d audioLeadMs=%.3f qpcFreq=%lld appStartQpc=%lld stimulusStartQpc=pending\n",
        GetCurrentProcessId(), g_WindowWidth, g_WindowHeight, g_TargetFps, g_DurationSeconds, g_AnalysisStartSeconds,
        g_VSync, g_Fullscreen, g_WindowChrome, g_Topmost, g_TearingRequested ? 1 : 0, g_GpuLoadPasses,
        g_AudioEnabled ? 1 : 0,
        g_AudioClockScheduling ? 1 : 0, g_AudioBufferMs, g_AudioLeadMs,
        static_cast<long long>(g_QpcFreq.QuadPart),
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
    testapp::Log("AVSYNC START window requested=%dx%d render=%dx%d fullscreen=%d windowChrome=%d topmost=%d borderless=%d\n",
                 g_RequestedWidth, g_RequestedHeight, g_WindowWidth, g_WindowHeight, g_Fullscreen, g_WindowChrome,
                 g_Topmost, borderlessWindowed ? 1 : 0);

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
        g_TargetFps, g_FrameTimerHighResolution ? 1 : 0,
        static_cast<unsigned long long>(g_PresentDeltaCount), avgPresentDeltaMs, g_MaxPresentDeltaMs,
        static_cast<unsigned long long>(g_FramePacingSpikeCount),
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
