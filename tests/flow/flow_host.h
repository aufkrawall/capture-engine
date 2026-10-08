#pragma once

// The game side of an FG flow test: a D3D12 game on the WARP adapter with a visible, never-activated window
// off the desktop, into which the flow-test hook DLL is loaded like an injection. The game can integrate the
// fake Streamline (tests/flow/fakes/streamline) and FidelityFX (tests/flow/fakes/fidelityfx) runtimes and
// switch between their swapchains the way GTA and Talos do: the device and queue stay, only the swapchain
// is replaced. Each scenario runs in its own process (the hook's state is process-global).

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "tests/flow/flow_api.h"

namespace ce::flow {

using Microsoft::WRL::ComPtr;

enum class SwapchainKind {
    kNative,      // the system DXGI factory's swapchain
    kStreamline,  // Streamline's proxy (DLSS frame generation runs on it, on or off)
    kFidelityFX,  // the FidelityFX frame generation swapchain (FSR frame generation runs on it, on or off)
};

// The UI resource an FSR game registers on its frame generation swapchain every frame while FG is enabled
// (ffxConfigure REGISTERUIRESOURCE); without a present callback AMD composites it onto every output.
enum class FSRUiResource {
    kNone,         // none: the HUD is part of the interpolated image
    kFullSize,     // a backbuffer-sized HUD texture (testapp/dx12_fg_switch_fsr.cpp)
    kPlaceholder,  // a 1x1 placeholder with UI composition left on (GTA V Enhanced)
};

// How the game keeps the UI resource it registers intact until AMD composed the frame's outputs.
enum class FSRUiBuffering {
    kSwapchainCopy,   // ENABLE_INTERNAL_UI_DOUBLE_BUFFERING: AMD copies it in Present (both validation games)
    kGameAlternates,  // no flag; the game registers one of two textures per frame
    kSingleTexture,   // no flag; one texture every frame (AMD reads it while the game rewrites it)
};

struct GameOptions {
    // Initialize Streamline like a DLSS-G game: slInit before the device, the device from the interposer and
    // slSetD3DDevice; Streamline swapchains come from its DXGI factory proxy and get a frame token, the
    // frame-generation tags and an slDLSSGGetState poll every frame.
    bool streamline = false;
    SwapchainKind swapchain = SwapchainKind::kNative;
    FSRUiResource fsrUi = FSRUiResource::kNone;
    FSRUiBuffering fsrUiBuffering = FSRUiBuffering::kSwapchainCopy;
    UINT width = 640;
    UINT height = 360;
};

struct StreamlineGame;  // flow_host_streamline.cpp
struct FidelityFXGame;  // flow_host_fidelityfx.cpp

// The D3D12 debug layer's messages in this process (flow_host_debug_layer.cpp); one scenario runs per process.
struct D3D12DebugMessages {
    bool watched = false;  // the debug layer is on and its messages reach logPath
    uint64_t total = 0;
    uint64_t corruptions = 0;
    uint64_t errors = 0;
    uint64_t warnings = 0;
    std::string firstFailure;  // the first CORRUPTION or ERROR, for a failure message
    std::string logPath;       // logs/<Suite.Test>/d3d12_debug.log
};
D3D12DebugMessages D3D12DebugMessagesSoFar();

struct NGXPublication {
    bool srActive = false;
    bool rrActive = false;
    bool fgActive = false;
    int fgMultiplier = 0;
};

class FlowGame {
public:
    // Loads build/flow_tests/capture_hook_x64.dll next to the executable, logging into logs/<testName>
    // beside it, and runs CE's hook installation with the flow config.
    explicit FlowGame(const std::string& testName);
    ~FlowGame();

    FlowGame(const FlowGame&) = delete;
    FlowGame& operator=(const FlowGame&) = delete;

    // The game's D3D12 setup: the debug layer, WARP device, direct queue, a 3-buffer flip-discard swapchain of
    // `swapchain`.
    bool CreateDeviceAndSwapchain(const GameOptions& options = {});

    // Replaces the swapchain with one of `kind` on the same device and queue (an FSR swapchain's FFX
    // contexts go with it), as a game does when its frame generation setting changes runtime.
    bool UseSwapchain(SwapchainKind kind);

    // ResizeBuffers to `format` at the same size, then SetColorSpace1(colorSpace), as a game does when it moves its
    // swapchain between SDR and HDR10 output at runtime.
    bool SetBackBufferFormat(DXGI_FORMAT format, DXGI_COLOR_SPACE_TYPE colorSpace);

    // slDLSSGSetOptions(eOn/eOff), as a game's menu toggle sends it; the proxy stays alive while off.
    bool SetDLSSFrameGeneration(bool enabled, uint32_t framesToGenerate = 1);

    // Whether the (fake) DLSS-G runtime itself is in eOn - what reached it, not what the game asked for.
    bool DLSSFrameGenerationRunning();

    // ffxConfigure(frameGenerationEnabled) on the FidelityFX swapchain, with or without a present callback.
    bool SetFSRFrameGeneration(bool enabled, bool presentCallback = true);

    // Renders (clears the back buffer), submits and presents one frame, then waits for the GPU so every
    // frame finishes before the next starts. Pumps the hook thread every `kFramesPerHookThreadPass` frames.
    enum class PresentMethod { kPresent, kPresent1 };
    bool RenderFrame(PresentMethod method = PresentMethod::kPresent);
    bool RenderFrames(int count, PresentMethod method = PresentMethod::kPresent);

    // The overlay backend's render-target format handling on the game's device and queue (flow_hook_entry.cpp):
    // draws into one render target per entry of `formats`, in order. False when a step failed (Error() names none:
    // the hook's log does).
    struct DescFreeProbeResult {
        bool ok = false;
        std::vector<uint32_t> firstPixels;  // each target's first texel as stored in its format
        uint32_t pipelineFormats = 0;       // pipeline pairs the backend holds afterwards
    };
    DescFreeProbeResult ProbeOverlayBackendFormats(const std::vector<DXGI_FORMAT>& formats);

    CEFlowOverlayCoverage Coverage() const;
    CEFlowPostProcess PostProcess() const;
    // Presents on real swapchains, counted where they happen (the game's native swapchain, the fake
    // runtimes' presenters) - independent of what CE saw.
    uint64_t PhysicalPresents() const;
    CEFlowPublishedFG PublishedFG() const;
    NGXPublication PublishedNGX() const;
    void ServiceHookThread();
    ComPtr<ID3D12CommandQueue> RetainGameQueue() const { return queue_; }
    ComPtr<IDXGISwapChain3> RetainGameSwapchain() const { return swapchain_; }
    ComPtr<IDXGISwapChain> RetainUnderlyingGameSwapchain() const;
    void RepairPresentHooks();
    void ReleasePresentVTableHooks();
    bool InstallPresentVTableHooks(IDXGISwapChain* swapchain);
    void TrackQueue(ID3D12CommandQueue* queue);
    void ResetQueueBindings();
    void ResetDeviceTrace();
    void* QueueOriginal(ID3D12CommandQueue* queue) const;
    void ForwardQueue(ID3D12CommandQueue* queue);
    void TrackSignalQueue(ID3D12CommandQueue* queue);
    void* SignalOriginal(ID3D12CommandQueue* queue) const;
    HRESULT ForwardSignal(ID3D12CommandQueue* queue, UINT64 value);
    bool RemoveSignalQueue(ID3D12CommandQueue* queue);
    CEFlowPostSLLifecycle PostSLLifecycle() const;
    bool TryConfirmPostSLEpoch(uint32_t epoch);
    const std::string& LogDirectory() const {
        return logDirectory_;
    }
    const std::string& Error() const {
        return error_;
    }
    int FramesPresented() const {
        return frame_;
    }

    // The game runs at 144 FPS on CE's virtual clock (hook/runtime/hook_clock.h): frame k starts at k * 6944 us
    // however fast WARP renders it. The production hook thread passes every 100 ms, so every 14th frame.
    static constexpr int64_t kFrameIntervalMicroseconds = 6944;
    static constexpr int kFramesPerHookThreadPass = 14;
    static constexpr UINT kBufferCount = 3;

private:
    bool Fail(const char* what, HRESULT hr);
    bool CreateSwapchain(SwapchainKind kind);
    void ReleaseSwapchain();
    void WaitForGpu();

    bool BeginStreamline();
    bool BindStreamlineDevice();
    void SubmitStreamlineFrame(UINT backBufferIndex);
    void PollStreamlineState();
    void MarkStreamlinePresent(bool start);
    void EndStreamline();
    HRESULT StreamlineCreateFactory(REFIID riid, void** factory);
    HRESULT StreamlineCreateDevice(IUnknown* adapter, REFIID riid, void** device);

    bool CreateFidelityFXSwapchain(const DXGI_SWAP_CHAIN_DESC1& desc, ComPtr<IDXGISwapChain1>* swapchain);
    void SubmitFidelityFXFrame();
    void DestroyFidelityFXContexts();
    void EndFidelityFX();

    bool EnableD3D12DebugLayer();
    bool WatchD3D12DebugMessages();
    void NoteD3D12DebugFrame();
    void StopWatchingD3D12DebugMessages();

    std::string logDirectory_;
    std::string error_;
    HMODULE hook_ = nullptr;
    CEFlow_TrackQueue_t trackQueue_ = nullptr;
    CEFlow_ResetQueueBindings_t resetQueueBindings_ = nullptr;
    CEFlow_ResetDeviceTrace_t resetDeviceTrace_ = nullptr;
    CEFlow_QueueOriginal_t queueOriginal_ = nullptr;
    CEFlow_ForwardQueue_t forwardQueue_ = nullptr;
    CEFlow_TrackQueue_t trackSignalQueue_ = nullptr;
    CEFlow_QueueOriginal_t signalOriginal_ = nullptr;
    CEFlow_ForwardSignal_t forwardSignal_ = nullptr;
    CEFlow_RemoveSignalQueue_t removeSignalQueue_ = nullptr;
    CEFlow_RepairPresentHooks_t repairPresentHooks_ = nullptr;
    CEFlow_RetainRealSwapchain_t retainRealSwapchain_ = nullptr;
    CEFlow_ReleasePresentVTableHooks_t releasePresentVTableHooks_ = nullptr;
    CEFlow_InstallPresentVTableHooks_t installPresentVTableHooks_ = nullptr;
    CEFlow_PumpHookThread_t pumpHookThread_ = nullptr;
    CEFlow_GetOverlayCoverage_t getOverlayCoverage_ = nullptr;
    CEFlow_GetPostProcess_t getPostProcess_ = nullptr;
    CEFlow_GetPublishedFG_t getPublishedFG_ = nullptr;
    CEFlow_Shutdown_t shutdown_ = nullptr;
    CEFlow_GetPostSLLifecycle_t getPostSLLifecycle_ = nullptr;
    CEFlow_ProbeDescFreeTargetFormats_t probeDescFreeTargetFormats_ = nullptr;
    CEFlow_TryConfirmPostSLEpoch_t tryConfirmPostSLEpoch_ = nullptr;
    CEFlow_AdvanceClock_t advanceClock_ = nullptr;
    CEFlow_ClockMicroseconds_t clockMicroseconds_ = nullptr;
    int64_t clockOrigin_ = 0;
    // The inject host's shared memory; the hook keeps reading it until the process exits, so it is never freed.
    SharedMemoryLayout* hostMemory_ = nullptr;

    HWND window_ = nullptr;
    UINT width_ = 0;
    UINT height_ = 0;
    SwapchainKind kind_ = SwapchainKind::kNative;
    FSRUiResource fsrUi_ = FSRUiResource::kNone;
    FSRUiBuffering fsrUiBuffering_ = FSRUiBuffering::kSwapchainCopy;
    ComPtr<IDXGIFactory4> nativeFactory_;
    ComPtr<IDXGIFactory4> streamlineFactory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12InfoQueue1> infoQueue_;
    DWORD debugMessageCookie_ = 0;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swapchain_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    UINT rtvStride_ = 0;
    ComPtr<ID3D12Resource> backBuffers_[kBufferCount];
    ComPtr<ID3D12Resource> depth_;
    ComPtr<ID3D12Resource> motionVectors_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;
    int frame_ = 0;
    StreamlineGame* streamline_ = nullptr;
    FidelityFXGame* fidelityfx_ = nullptr;
};

ComPtr<ID3D12Resource> CreateFlowTexture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format,
                                         D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                         D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON);

}  // namespace ce::flow
