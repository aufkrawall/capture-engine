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
#include <cstring>
#include <mutex>
#include <string>

#include "av_sync_stimulus.h"
#include "dx12_av_sync_audio.h"
#include "testapp_common.h"

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
int g_GpuLoadPasses = 0;
bool g_LogEveryFrame = false;
bool g_AudioEnabled = true;
bool g_AllowTearing = false;
bool g_Running = true;

LARGE_INTEGER g_QpcFreq = {};
LARGE_INTEGER g_StimulusStartQpc = {};
LARGE_INTEGER g_AppStartQpc = {};
uint64_t g_FrameId = 0;

ComPtr<ID3D12Device> g_Device;
ComPtr<ID3D12CommandQueue> g_CommandQueue;
ComPtr<IDXGISwapChain3> g_SwapChain;
ComPtr<ID3D12DescriptorHeap> g_RtvHeap;
ComPtr<ID3D12Resource> g_RenderTargets[kFrameCount];
ComPtr<ID3D12CommandAllocator> g_CommandAllocators[kFrameCount];
ComPtr<ID3D12GraphicsCommandList> g_CommandList;
ComPtr<ID3D12Fence> g_Fence;
HANDLE g_FenceEvent = nullptr;
UINT64 g_FenceValues[kFrameCount] = {};
UINT g_FrameIndex = 0;
UINT g_RtvDescriptorSize = 0;
std::mutex g_FrameSyncMutex;

double QpcToSeconds(LONGLONG deltaTicks) {
    return static_cast<double>(deltaTicks) / static_cast<double>(g_QpcFreq.QuadPart);
}

LARGE_INTEGER QueryQpc() {
    LARGE_INTEGER qpc = {};
    QueryPerformanceCounter(&qpc);
    return qpc;
}

double SecondsSinceStimulusStart() {
    const LARGE_INTEGER now = QueryQpc();
    return QpcToSeconds(now.QuadPart - g_StimulusStartQpc.QuadPart);
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
    g_VSync = GetPrivateProfileIntW(L"Rendering", L"vsync", g_VSync, configPath.c_str());
    g_GpuLoadPasses = GetPrivateProfileIntW(L"Performance", L"gpu_load", g_GpuLoadPasses, configPath.c_str());
    g_TargetFps = ClampInt(GetPrivateProfileIntW(L"AVSync", L"fps", g_TargetFps, configPath.c_str()), 1, 480);
    g_DurationSeconds =
        ClampInt(GetPrivateProfileIntW(L"AVSync", L"duration_seconds", g_DurationSeconds, configPath.c_str()), 1, 3600);
    g_LogEveryFrame = GetPrivateProfileIntW(L"AVSync", L"log_every_frame", 0, configPath.c_str()) != 0;
    g_AudioEnabled = GetPrivateProfileIntW(L"AVSync", L"audio", 1, configPath.c_str()) != 0;
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

        if (readInt("--width", "--width=", &g_WindowWidth) || readInt("--height", "--height=", &g_WindowHeight) ||
            readInt("--fps", "--fps=", &g_TargetFps) ||
            readInt("--duration", "--duration=", &g_DurationSeconds) ||
            readInt("--gpu-load", "--gpu-load=", &g_GpuLoadPasses) ||
            readInt("--vsync", "--vsync=", &g_VSync) ||
            readInt("--fullscreen", "--fullscreen=", &g_Fullscreen) ||
            readInt("--window-chrome", "--window-chrome=", &g_WindowChrome)) {
            continue;
        }
        if (strcmp(argv[i], "--log-every-frame") == 0) {
            g_LogEveryFrame = true;
        } else if (strcmp(argv[i], "--no-audio") == 0) {
            g_AudioEnabled = false;
        }
    }

    g_WindowWidth = ClampInt(g_WindowWidth, 320, 7680);
    g_WindowHeight = ClampInt(g_WindowHeight, 240, 4320);
    g_TargetFps = ClampInt(g_TargetFps, 1, 480);
    g_DurationSeconds = ClampInt(g_DurationSeconds, 1, 3600);
    g_GpuLoadPasses = ClampInt(g_GpuLoadPasses, 0, 10000);
    g_Fullscreen = g_Fullscreen ? 1 : 0;
    g_WindowChrome = g_WindowChrome ? 1 : 0;
}

testapp::avsync::AudioRenderer g_Audio(&g_QpcFreq, &g_StimulusStartQpc);

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
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
            g_AllowTearing = allowTearing != FALSE;
        }
    }

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
    if (g_AllowTearing) {
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
    testapp::Log("AVSYNC START dx12 width=%d height=%d tearing=%d\n", g_WindowWidth, g_WindowHeight,
                 g_AllowTearing ? 1 : 0);
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
    const uint8_t checksum = static_cast<uint8_t>((marker & 0xffu) ^ ((marker >> 8) & 0xffu) ^
                                                 static_cast<uint8_t>(state.paletteIndex < 0 ? 0 : state.paletteIndex));
    for (int bit = 0; bit < 8; ++bit) {
        const bool set = ((checksum >> bit) & 1u) != 0;
        const LONG x = startX + bit * (size + 4);
        ClearRect(rtv, x, startY, x + size, startY + size, set ? white : black);
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
    const UINT flags = (!g_VSync && g_AllowTearing) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const HRESULT presentHr = g_SwapChain->Present(sync, flags);
    if (FAILED(presentHr)) {
        testapp::Log("AVSYNC WARNING Present failed frameId=%llu hr=0x%08lx\n",
                     static_cast<unsigned long long>(g_FrameId), static_cast<unsigned long>(presentHr));
        g_Running = false;
    }
    MoveToNextFrame();

    if (g_LogEveryFrame || (g_FrameId % static_cast<uint64_t>(std::max(1, g_TargetFps)) == 0)) {
        testapp::Log(
            "AVSYNC FRAME frameId=%llu marker=%u event=%d palette=%d stimulusSeconds=%.6f "
            "motion=%.6f frameIndex=%u\n",
            static_cast<unsigned long long>(g_FrameId), marker, state.eventIndex, state.paletteIndex, stimulusSeconds,
            testapp::avsync::SmoothLanePosition(stimulusSeconds), frameIndex);
    }
    ++g_FrameId;
}

void PaceNextFrame(LARGE_INTEGER* nextFrameQpc) {
    const LONGLONG interval = g_QpcFreq.QuadPart / std::max(1, g_TargetFps);
    nextFrameQpc->QuadPart += interval;
    for (;;) {
        const LARGE_INTEGER now = QueryQpc();
        const LONGLONG remaining = nextFrameQpc->QuadPart - now.QuadPart;
        if (remaining <= 0) {
            break;
        }
        const double remainingMs = QpcToSeconds(remaining) * 1000.0;
        if (remainingMs > 2.0) {
            Sleep(1);
        } else {
            Sleep(0);
        }
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
    fprintf(out, "  \"borderless_windowed\": %d,\n", (!g_Fullscreen && !g_WindowChrome) ? 1 : 0);
    fprintf(out, "  \"target_fps\": %d,\n", g_TargetFps);
    fprintf(out, "  \"duration_seconds\": %d,\n", g_DurationSeconds);
    fprintf(out, "  \"event_period_seconds\": %.6f,\n", testapp::avsync::kEventPeriodSeconds);
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
    fprintf(out, "  \"frame_marker_bits\": %d,\n", testapp::avsync::kFrameMarkerBits);
    fprintf(out, "  \"marker_tile\": %ld,\n", static_cast<long>(kMarkerTile));
    fprintf(out, "  \"marker_margin\": %ld,\n", static_cast<long>(kMarkerMargin));
    fprintf(out, "  \"marker_gap\": %ld\n", static_cast<long>(kMarkerGap));
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

    const LONGLONG preStartTicks =
        static_cast<LONGLONG>(static_cast<double>(g_QpcFreq.QuadPart) * testapp::avsync::kPreStartSeconds);
    g_StimulusStartQpc.QuadPart = QueryQpc().QuadPart + preStartTicks;

    testapp::Log(
        "AVSYNC START app pid=%lu width=%d height=%d fps=%d duration=%d vsync=%d fullscreen=%d "
        "windowChrome=%d gpuLoad=%d audio=%d qpcFreq=%lld appStartQpc=%lld stimulusStartQpc=%lld\n",
        GetCurrentProcessId(), g_WindowWidth, g_WindowHeight, g_TargetFps, g_DurationSeconds, g_VSync, g_Fullscreen,
        g_WindowChrome, g_GpuLoadPasses, g_AudioEnabled ? 1 : 0, static_cast<long long>(g_QpcFreq.QuadPart),
        static_cast<long long>(g_AppStartQpc.QuadPart), static_cast<long long>(g_StimulusStartQpc.QuadPart));
    LogEventSchedule();

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
    if (g_Fullscreen) {
        style = WS_POPUP;
        exStyle = WS_EX_TOPMOST;
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
    testapp::Log("AVSYNC START window requested=%dx%d render=%dx%d fullscreen=%d windowChrome=%d borderless=%d\n",
                 g_RequestedWidth, g_RequestedHeight, g_WindowWidth, g_WindowHeight, g_Fullscreen, g_WindowChrome,
                 borderlessWindowed ? 1 : 0);
    WriteManifest();

    HWND hwnd = CreateWindowExW(exStyle, wc.lpszClassName, L"DX12 A/V Sync Test", style, windowX, windowY, windowW,
                                windowH, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        testapp::Log("AVSYNC WARNING CreateWindowExW failed gle=%lu\n", GetLastError());
        testapp::CloseLogFile();
        return 1;
    }

    if (!testapp::PrimeWindowForBenchmark(hwnd, g_Fullscreen != 0, g_WindowWidth, g_WindowHeight, 500)) {
        testapp::Log("AVSYNC WARNING PrimeWindowForBenchmark failed\n");
    }
    if (!InitDX12(hwnd)) {
        testapp::CloseLogFile();
        return 1;
    }

    if (g_AudioEnabled) {
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
    LARGE_INTEGER nextFrameQpc = QueryQpc();
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
        RenderFrame();
        PaceNextFrame(&nextFrameQpc);
    }

    if (g_AudioEnabled) {
        g_Audio.Stop();
    }
    testapp::Log("AVSYNC SUMMARY app frames=%llu finalStimulusSeconds=%.6f\n",
                 static_cast<unsigned long long>(g_FrameId), SecondsSinceStimulusStart());
    Cleanup();
    timeEndPeriod(1);
    testapp::CloseLogFile();
    return 0;
}
