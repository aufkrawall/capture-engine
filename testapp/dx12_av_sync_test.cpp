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
#include "dx12_av_sync_test_internal.h"
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

constexpr int kFrameCount = 2;

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

// Extracted from dx12_av_sync_test.cpp to stay under the AGENTS.md
// size ceiling. Included at exactly the point these definitions used to sit,
// so declaration order is unchanged.

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
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
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
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
    g_TearingActive = testapp::avsync::ShouldUseDxgiTearing(g_TearingRequested, g_TearingSupported, g_VSync != 0);

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

// Extracted from dx12_av_sync_test.cpp to stay under the AGENTS.md
// size ceiling. Included at exactly the point these definitions used to sit,
// so declaration order is unchanged.

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

void DrawMotionRuler(D3D12_CPU_DESCRIPTOR_HANDLE rtv, LONG laneLeft, LONG laneTop, LONG laneRight, LONG laneBottom) {
    const float tickColor[] = {0.24f, 0.24f, 0.24f, 1.0f};
    const LONG usable = std::max<LONG>(1, laneRight - laneLeft);
    for (int tick = 1; tick < 8; ++tick) {
        const LONG x = laneLeft + static_cast<LONG>((static_cast<double>(usable) * tick) / 8.0);
        ClearRect(rtv, x, laneTop + 3, x + 2, laneBottom - 3, tickColor);
    }
}

void DrawMotionLaneAt(D3D12_CPU_DESCRIPTOR_HANDLE rtv, double position, double topRatio, LONG barWidth,
                      LONG laneHeight) {
    const LONG laneLeft = kMarkerMargin;
    const LONG laneRight = g_WindowWidth - kMarkerMargin;
    const LONG laneTop = static_cast<LONG>(g_WindowHeight * topRatio);
    const LONG laneBottom = laneTop + laneHeight;
    const float laneColor[] = {0.015f, 0.015f, 0.015f, 1.0f};
    const float barColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ClearRect(rtv, laneLeft, laneTop, laneRight, laneBottom, laneColor);
    DrawMotionRuler(rtv, laneLeft, laneTop, laneRight, laneBottom);
    const LONG usable = std::max<LONG>(1, laneRight - laneLeft - barWidth);
    const LONG x = laneLeft + static_cast<LONG>(position * static_cast<double>(usable));
    ClearRect(rtv, x, laneTop + 7, x + barWidth, laneBottom - 7, barColor);
}

void DrawMotionLane(D3D12_CPU_DESCRIPTOR_HANDLE rtv, double stimulusSeconds) {
    DrawMotionLaneAt(rtv, testapp::avsync::SmoothLanePosition(stimulusSeconds), 0.72, 96, 42);
    DrawMotionLaneAt(rtv, testapp::avsync::FastLanePosition(stimulusSeconds), 0.82, 48, 42);
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

void DrawEncoderStressScene(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const testapp::avsync::StimulusState& state,
                            uint64_t frameId) {
    if (!g_EncoderStressScene || state.eventIndex < 0) {
        return;
    }

    const auto layout = testapp::avsync::ComputeEncoderStressLayout(g_WindowWidth, g_WindowHeight, kMarkerMargin,
                                                                    kMarkerTile, kMarkerGap);
    if (!layout.valid) {
        return;
    }

    for (LONG y = layout.top; y < layout.bottom; y += layout.tile) {
        for (LONG x = layout.left; x < layout.right; x += layout.tile) {
            if (x < layout.reserveRight && x + layout.tile > layout.reserveLeft && y < layout.reserveBottom &&
                y + layout.tile > layout.reserveTop) {
                continue;
            }
            const uint32_t tileX = static_cast<uint32_t>((x - layout.left) / layout.tile);
            const uint32_t tileY = static_cast<uint32_t>((y - layout.top) / layout.tile);
            const uint32_t hash = testapp::avsync::EncoderStressTileHash(tileX, tileY, frameId, state.paletteIndex);
            const float color[] = {
                0.08f + static_cast<float>(hash & 0xffu) * (0.84f / 255.0f),
                0.08f + static_cast<float>((hash >> 8) & 0xffu) * (0.84f / 255.0f),
                0.08f + static_cast<float>((hash >> 16) & 0xffu) * (0.84f / 255.0f),
                1.0f,
            };
            ClearRect(rtv, x, y, x + layout.tile - 2, y + layout.tile - 2, color);
        }
    }
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

    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    const float bg[] = {state.color.r / 255.0f, state.color.g / 255.0f, state.color.b / 255.0f, 1.0f};
    const float preStart[] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (int pass = 0; pass < g_GpuLoadPasses; ++pass) {
        const float load[] = {static_cast<float>((pass & 7) + 1) / 64.0f, 0.02f, 0.03f, 1.0f};
        g_CommandList->ClearRenderTargetView(rtv, load, 0, nullptr);
    }
    g_CommandList->ClearRenderTargetView(rtv, state.eventIndex < 0 ? preStart : bg, 0, nullptr);
    DrawEncoderStressScene(rtv, state, g_FrameId);
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
            "motion=%.6f expectedMotion=%.6f fastMotion=%.6f frameIndex=%u presentDeltaMs=%.3f "
            "maxPresentDeltaMs=%.3f\n",
            static_cast<unsigned long long>(g_FrameId), marker, state.eventIndex, state.paletteIndex, stimulusSeconds,
            testapp::avsync::SmoothLanePosition(stimulusSeconds),
            testapp::avsync::ExpectedMotionPosition(stimulusSeconds),
            testapp::avsync::FastLanePosition(stimulusSeconds), frameIndex, g_LastPresentDeltaMs, g_MaxPresentDeltaMs);
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

    // NOLINTNEXTLINE(bugprone-exception-escape) - standalone test harness: an unexpected exception terminating the process is acceptable and yields a nonzero exit
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
