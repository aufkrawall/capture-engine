#pragma once

#include "postsl_lifecycle.h"
#include "postsl_queue_owner.h"
#include "dx12_queue_dispatch.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "hook/d3d12/dx12_overlay_policy/upload_slot_guard.h"
#include "hook/d3d12/dx12_overlay_policy/protected_ffx_startup.h"

// Type definitions moved out of dx12_hook_internal.h so every unit stays <= 800 lines.



typedef HRESULT(STDMETHODCALLTYPE* CreateCommittedResourcePtr)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*,
                                                               D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
                                                               D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID,
                                                               void**);

typedef HRESULT(STDMETHODCALLTYPE* CreateCommandQueuePtr)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID,
                                                          void**);

typedef HRESULT(STDMETHODCALLTYPE* CreateDescriptorHeapPtr)(ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID,
                                                            void**);


extern CreateCommandQueuePtr oTraceCreateCommandQueue;

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);

typedef HRESULT(STDMETHODCALLTYPE* PFN_FFXProxyPresent)(IDXGISwapChain*, UINT, UINT);

typedef HRESULT(STDMETHODCALLTYPE* PFN_FFXProxyPresent1)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                        IDXGISwapChain**);

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND,
                                                               const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                               IDXGISwapChain1**);

enum class DX12OverlayRenderRoute : uint32_t {
    kNone = 0,
    kNormal = 1,
    kPostSL = 2,
    kFFXPresentCallback = 3,
    kStreamlineUI = 4,
    kBelowForeignChainRuntimeOwnedFSR = 5,
};

enum OverlayGpuBreadcrumbOp : uint32_t {
    kOverlayBcStart = 1,       // command list reset, recording started
    kOverlayBcAfterRTBarrier,  // backbuffer transitioned to RENDER_TARGET
    kOverlayBcAfterDraw,       // overlay draw recorded
    kOverlayBcBeforeClose,     // all overlay commands recorded (about to Close)
    kOverlayBcSlotCount,
};

enum class StartupOverlayActivationStage {
    kNone = 0,
    kDelayRTVInitAfterBackendInit,
    kDelaySyncInitAfterRTVInit,
};

enum class StartupOverlayFirstDrawProbeStage {
    kNone = 0,
    kBackbufferTouchOnly,
    kPipelineStateOnly,
    kActualRender,
    kComplete,
};

struct DX12OverlayCoverageSnapshot {
    uint64_t totalPresents = 0;
    uint64_t uncoveredPresents = 0;
    uint64_t currentStreak = 0;
    uint64_t longestStreak = 0;
    uint64_t doubleDraws = 0;  // presents that got the overlay from two routes (see NoteDX12OverlayRendered)
};

class DX12DescFreeBackend : public CustomOverlay::RendererBackend {
public:
~DX12DescFreeBackend();

    // Non-virtual: create device-dependent resources (root sig, PSOs)
bool InitDevice(ID3D12Device* dev, DXGI_FORMAT rtvFormat);

    // RendererBackend: stage font atlas for a descriptor-free structured uint buffer.
    // The pixel shader samples from a DEFAULT-heap buffer; reading a UPLOAD heap
    // directly in the text draw has proven fragile on the x86 NVIDIA path.
bool Initialize(int fontWidth, int fontHeight, const uint8_t* fontData) override;

void Render(const std::vector<CustomOverlay::DrawVertex>& vertices, const std::vector<uint16_t>& indices, const std::vector<CustomOverlay::DrawCommand>& commands, int vpW, int vpH) override;

void SetNextUploadSlot(int allocatorSlot) override;

void Shutdown() override;

private:
bool CreateRootSignature();

bool CreatePSOs();

bool CreateBuffers();

bool ResizeBuffer(ID3D12Resource*& buf, void*& ptr, size_t& curSize, size_t needed);

bool IsUploadSlotReusable(int slot);

    static constexpr int kPoolSize = ce::dx12_overlay_policy::kAllocatorCoupledUploadSlotCount;
    static constexpr size_t kInitVBBytes = 4096 * 20;  // 4096 vertices * 20 bytes
    static constexpr size_t kInitIBBytes = 8192 * 2;   // 8192 indices * 2 bytes

    ID3D12Device* device_ = nullptr;
    DXGI_FORMAT rtvFormat_ = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool deviceReady_ = false;

    ID3D12RootSignature* rootSig_ = nullptr;
    ID3D12PipelineState* psoTextured_ = nullptr;
    ID3D12PipelineState* psoSolid_ = nullptr;

    ID3D12Resource* fontBuffer_ = nullptr;
    ID3D12Resource* fontUploadBuffer_ = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS fontGpuAddr_ = 0;
    size_t fontBufferSize_ = 0;
    bool fontUploadPending_ = false;
    int fontWidth_ = 0;
    int fontHeight_ = 0;

    ID3D12Resource* vb_[kPoolSize] = {};
    ID3D12Resource* ib_[kPoolSize] = {};
    void* vbPtr_[kPoolSize] = {};
    void* ibPtr_[kPoolSize] = {};
    size_t vbSize_[kPoolSize] = {};
    size_t ibSize_[kPoolSize] = {};
    ce::dx12_overlay_policy::UploadSlotGuardFenceBinding slotGuardBinding_;
    UINT64 slotFenceValue_[kPoolSize] = {};
    std::atomic<int> nextUploadSlot_{-1};
    int frameIdx_ = 0;
    uint32_t inFlightSkipStreak_ = 0;
};

struct DX12OverlayState {
    // Large pool size ensures we never need to wait for GPU.
    // Even at 60fps with 100ms GPU latency, only 6 allocators are in flight.
    // 16 provides 2.5x headroom - allocator is always ready, zero waiting.
    static const int ALLOC_POOL_SIZE = ce::dx12_overlay_policy::kAllocatorCoupledUploadSlotCount;
    std::vector<ID3D12CommandAllocator*> allocators;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    int allocIndex = 0;
    ID3D12Fence* fence = nullptr;
    HANDLE fenceEvent = nullptr;
    UINT64 currentFenceValue = 0;
    std::vector<UINT64> fenceValues;
    ID3D12DescriptorHeap* rtvDescHeap = nullptr;
    ID3D12DescriptorHeap* srvDescHeap = nullptr;
    UINT rtvDescriptorSize = 0;
    std::vector<ID3D12Resource*> backBuffers;
    bool overlayInit = false;
    bool syncInit = false;
    int cachedWidth = 0;
    int cachedHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT bufferCount = 0;
    IDXGISwapChain* cachedSwapChain = nullptr;

    // Offscreen render target for overlay compositing.
    // Avoids OMSetRenderTargets(swapchain) + SetDescriptorHeaps on the same ECL,
    // which causes GPU pipeline stalls (~40% utilization loss) in some games.
    // Flow: copy backbuffer → offscreen, render overlay → offscreen, copy back.
    ID3D12Resource* offscreenRT = nullptr;
    ID3D12DescriptorHeap* offscreenRtvHeap = nullptr;
    UINT offscreenWidth = 0;
    UINT offscreenHeight = 0;
    DXGI_FORMAT offscreenFormat = DXGI_FORMAT_UNKNOWN;
    // The post-FSR PostSL queue probe's barrier target; it must outlive every probe list the GPU still executes.
    ID3D12Resource* postFSRProbeScratch = nullptr;

    // Dedicated overlay command queue for FG-safe rendering.
    // When FG is active, overlay commands execute on this queue with CPU-side
    // fence synchronization to avoid interfering with Streamline's game queue
    // management.  When FG is not active, overlay commands go on the game queue
    // directly (zero CPU waits).
    ID3D12CommandQueue* overlayQueue = nullptr;

    // Cross-queue fence: game queue signals to mark work completion, then
    // CPU-side wait before submitting overlay work on the overlay queue.
    // GPU-side CommandQueue::Wait was removed (NVIDIA WaitImpl Alt+Tab hang).
    ID3D12Fence* crossQueueFence = nullptr;
    UINT64 crossQueueFenceValue = 0;
    HANDLE crossQueueFenceEvent = nullptr;

    IDXGISwapChain3* cachedSC3 = nullptr;  // cached from first successful QI

    // The device used to create sync resources (allocators, command list, fence).
    // Must match the submission queue's device — cross-device submission = DEVICE_REMOVED.
    ID3D12Device* syncDevice = nullptr;

    // D3D11On12 overlay bridge: renders overlay via D3D11 on top of the D3D12
    // backbuffer.  D3D11 doesn't use descriptor heaps, avoiding the NVIDIA
    // driver stall triggered by SetDescriptorHeaps + OMSetRenderTargets(swapchain).
    ID3D11Device* d3d11on12Device = nullptr;
    ID3D11DeviceContext* d3d11on12Context = nullptr;
    ID3D11On12Device* d3d11on12 = nullptr;
    std::vector<ID3D11Resource*> d3d11WrappedBBs;
    std::vector<ID3D11RenderTargetView*> d3d11RTVs;
    bool d3d11on12Init = false;

void Cleanup();
};

struct DX12Context {
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;

    DX12Context() = default;

DX12Context(ID3D12Device* d, ID3D12CommandQueue* q);

~DX12Context();

    // Disable copy to prevent accidental double-release
    DX12Context(const DX12Context&) = delete;
    DX12Context& operator=(const DX12Context&) = delete;

    // Enable move
DX12Context(DX12Context&& other) noexcept;

    DX12Context& operator=(DX12Context&& other) noexcept {
        if (this != &other) {
            if (device)
                device->Release();
            if (queue)
                queue->Release();
            device = other.device;
            queue = other.queue;
            other.device = nullptr;
            other.queue = nullptr;
        }
        return *this;
    }

bool IsValid() const;
};

struct ForwardedCreateSwapchainForHwndCallerContext {
    const void* callerAddress = nullptr;
    char callerModulePath[MAX_PATH] = {};
};

class ScopedForwardedCreateSwapchainForHwndCallerContext {
public:
ScopedForwardedCreateSwapchainForHwndCallerContext(const void* callerAddress, const char* callerModulePath);

~ScopedForwardedCreateSwapchainForHwndCallerContext();

private:
    ForwardedCreateSwapchainForHwndCallerContext previousContext_;
};

class ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard {
public:
ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard();

~ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard();

bool InlineHandledForwardedCall() const;

private:
    int previousDepth_ = 0;
    bool previousHandled_ = false;
};

struct CreateSwapchainForHwndCallerContext {
    const void* callerAddress = nullptr;
    bool callerFromFFXFGModule = false;
    bool callerFromThirdPartyOverlay = false;
    char callerModulePath[MAX_PATH] = {};
};

struct CreateSwapchainQueueCaptureEvidence {
    const void* callerAddress = nullptr;
    bool callerFromThirdPartyOverlay = false;
    bool authoritativeFFXRuntimeCreator = false;
    bool officialAMDFFXRuntimeCreator = false;
    bool authoritativeStreamlineRuntimeCreator = false;
    bool callerFromStreamlineFGModule = false;
    bool streamlineFrameGenerationInStack = false;
    char callerModulePath[MAX_PATH] = {};
    char ffxModulePath[MAX_PATH] = {};
};

struct FFXPresentCallbackBridgeState {
    ce::ffx_api::PresentCallback originalCallback = nullptr;
    void* originalUserContext = nullptr;
    bool installed = false;
};

struct DX12WrappedPresentFocusLossContext {
    bool valid = false;
    const char* presentName = nullptr;
    int callCount = 0;
    UINT syncInterval = 0;
    UINT presentFlags = 0;
};

class ScopedCEOverlayECLSubmission {
public:
ScopedCEOverlayECLSubmission(const char* reason);

~ScopedCEOverlayECLSubmission();

    ScopedCEOverlayECLSubmission(const ScopedCEOverlayECLSubmission&) = delete;
    ScopedCEOverlayECLSubmission& operator=(const ScopedCEOverlayECLSubmission&) = delete;

private:
    const char* previousReason_ = nullptr;
};

struct ProgressResolvedOfficialFFXOverlayFallbackProof {
    bool proof = false;
    bool progressResolved = false;
    bool hasSwapchainQueue = false;
    bool hasOriginalGameQueue = false;
    bool swapchainQueueMatchesOriginalGameQueue = false;
    bool hasDevice = false;
    ULONGLONG stableMs = 0;
    HRESULT deviceHr = E_POINTER;
};

struct FFXUiCompositeTimelineEntry {
    uint64_t frame;           // g_FFXUiCompositeFrame at record time
    uint64_t fenceVal;        // g_FFXUiCompositeFenceVal at record time (0 if no game-queue Signal)
    uint64_t fenceCompleted;  // g_FFXUiCompositeFence->GetCompletedValue() at record time
    uint64_t submitQpc;       // QPC at ECL submit
    uint64_t returnQpc;       // QPC at composite-function return (or wait-return if a wait occurred)
    uint32_t waitTimedOut;    // 1 if the completion wait timed out
    uint32_t slot;            // allocator slot used
    uint32_t gameEclCount;    // g_CommandListsExecutedThisFrame at composite time
    void* uiTexture;          // the UI texture pointer
    uint32_t ffxState;        // the FFX resource state
    void* queue;              // the queue used for submission
};

struct NativeFSRSwapchainQueueBinding {
    void* context = nullptr;
    ID3D12CommandQueue* descriptorQueue = nullptr;
    ID3D12CommandQueue* underlyingGameQueue = nullptr;
    bool descriptorQueueUsesAcceptedStreamlineDevice = false;
    bool recoveredOriginalGameQueue = false;
};

struct AcquiredNativeFSROwnerQueue {
    ID3D12CommandQueue* queue = nullptr;
    ce::dx12_overlay_policy::NativeFSROwnerQueueRoute route =
        ce::dx12_overlay_policy::NativeFSROwnerQueueRoute::kUnavailable;
};

struct Dx12FocusAnalysisSample {
    uint64_t presentIdx;
    double gapMs;
    uint32_t localBudgetMB;
    uint32_t localUsageMB;
    uint32_t nonLocalUsageMB;
    int foreground;
};


// ---- shared globals (definitions in dx12_hook_internal_globals.cpp) ----

struct DX12ECLState {
    std::atomic<ExecuteCommandListsPtr> realECL{nullptr};
    std::atomic<SignalPtr> realSignal{nullptr};
    std::atomic<ID3D12Fence*> overlayCompletionFence{nullptr};
    std::atomic<bool> probeRealECLDeferred{false};
    std::atomic<int> callCount{0};
};
extern DX12ECLState g_DX12ECLState;

inline std::atomic<ExecuteCommandListsPtr>& dx12_hook_g_RealD3D12ECL = g_DX12ECLState.realECL;
inline std::atomic<SignalPtr>& dx12_hook_g_RealD3D12Signal = g_DX12ECLState.realSignal;
inline std::atomic<ID3D12Fence*>& dx12_hook_g_OverlayCompletionFence = g_DX12ECLState.overlayCompletionFence;
inline std::atomic<bool>& dx12_hook_g_ProbeRealD3D12ECLDeferred = g_DX12ECLState.probeRealECLDeferred;
inline std::atomic<int>& dx12_hook_g_ECLCallCount = g_DX12ECLState.callCount;
extern PFN_CreateSwapChain dx12_hook_oCreateSwapChain;
extern PFN_CreateSwapChainForHwnd dx12_hook_oCreateSwapChainForHwnd;
extern void** dx12_hook_s_savedCreateSwapChainForHwndVtable;
extern ID3D12GraphicsCommandList* dx12_hook_s_descFreeCmdList;
extern D3D12_CPU_DESCRIPTOR_HANDLE dx12_hook_s_descFreeRtv;
extern ID3D12Fence* dx12_hook_s_descFreeSlotFence;
extern UINT64 dx12_hook_s_descFreeSlotGuardValue;
extern thread_local uint64_t dx12_hook_s_PostSLSuccessfulSubmitSequence;
extern std::atomic<ULONGLONG> dx12_hook_g_LastProcessFrameTickMs;
extern std::atomic<ULONGLONG> dx12_hook_g_LastFFXPresentCallbackTickMs;
extern std::atomic<bool> dx12_hook_g_FFXPresentCallbackBridgeExpected;
extern std::atomic<bool> dx12_hook_g_NativeFSRInternalNoCallbackComposition;
extern std::atomic<ULONGLONG> dx12_hook_g_LastDX12OverlayRenderTickMs;
extern std::atomic<uint32_t> dx12_hook_g_LastDX12OverlayRenderRoute;
struct DX12PostSLRuntimeState {
    std::atomic<int> cooldownRemaining{0};
    std::atomic<bool> explicitOffKeepAlive{false};
    std::atomic<bool> warmResumePreservationPending{false};
    std::atomic<IDXGISwapChain*> lastSuccessfulSwapchain{nullptr};
    std::atomic<bool> syntheticStartupTakeoverLogged{false};
    std::atomic<int> syntheticStartupWrapperProgressCount{0};
    std::atomic<bool> syntheticStartupWrapperOnlyDumpRequested{false};
    std::atomic<uint32_t> streamlineEnableCallsInFlight{0};
    std::atomic<bool> runtimeStateStabilizationLogged{false};
    std::atomic<bool> extendedRuntimeStateStabilizationForCurrentEpoch{false};
    std::atomic<int> stallCounter{0};
    std::atomic<int> stableFrameCount{0};
};
extern DX12PostSLRuntimeState g_DX12PostSLRuntime;
extern ce::dx12::PostSLLifecycle g_PostSLLifecycle;
using PostSLQueueOwner = ce::dx12::PostSLQueueOwner<ID3D12CommandQueue, ID3D12Fence>;
extern PostSLQueueOwner g_PostSLQueues;

inline std::atomic<int>& dx12_hook_g_PostSLCooldownRemaining = g_DX12PostSLRuntime.cooldownRemaining;
inline std::atomic<bool>& dx12_hook_g_PostSLExplicitOffKeepAlive = g_DX12PostSLRuntime.explicitOffKeepAlive;
inline std::atomic<bool>& dx12_hook_g_PostSLWarmResumePreservationPending = g_DX12PostSLRuntime.warmResumePreservationPending;
inline std::atomic<IDXGISwapChain*>& dx12_hook_g_LastSuccessfulPostSLSwapchain = g_DX12PostSLRuntime.lastSuccessfulSwapchain;
inline std::atomic<bool>& dx12_hook_g_PostSLSyntheticStartupTakeoverLogged = g_DX12PostSLRuntime.syntheticStartupTakeoverLogged;
inline std::atomic<int>& dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount = g_DX12PostSLRuntime.syntheticStartupWrapperProgressCount;
inline std::atomic<bool>& dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested = g_DX12PostSLRuntime.syntheticStartupWrapperOnlyDumpRequested;
inline std::atomic<uint32_t>& dx12_hook_g_StreamlineEnableCallsInFlight = g_DX12PostSLRuntime.streamlineEnableCallsInFlight;
inline std::atomic<bool>& dx12_hook_g_PostSLRuntimeStateStabilizationLogged = g_DX12PostSLRuntime.runtimeStateStabilizationLogged;
inline std::atomic<bool>& dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch = g_DX12PostSLRuntime.extendedRuntimeStateStabilizationForCurrentEpoch;
struct DX12OverlayCoverageState {
    std::atomic<uint64_t> drawCount{0};
    std::atomic<uint64_t> doubleDrawCount{0};
    std::atomic<uint64_t> lastSeenDrawCount{0};
    std::atomic<const char*> lastGate{nullptr};
    std::atomic<const char*> streakGate{nullptr};
    std::atomic<uint64_t> streakStartTickMs{0};
    std::atomic<bool> streakStartConfirmed{false};
    ce::dx12_overlay_policy::OverlayPresentCoverageTracker tracker;
    std::atomic_flag lock = ATOMIC_FLAG_INIT;
};
extern DX12OverlayCoverageState g_DX12OverlayCoverage;

inline std::atomic<uint64_t>& dx12_hook_g_OverlayCoverageDrawCount = g_DX12OverlayCoverage.drawCount;
inline std::atomic<uint64_t>& dx12_hook_g_OverlayDoubleDrawCount = g_DX12OverlayCoverage.doubleDrawCount;
inline std::atomic<uint64_t>& dx12_hook_g_OverlayCoverageLastSeenDrawCount = g_DX12OverlayCoverage.lastSeenDrawCount;
inline std::atomic<const char*>& dx12_hook_g_OverlayCoverageLastGate = g_DX12OverlayCoverage.lastGate;
inline std::atomic<const char*>& dx12_hook_g_OverlayCoverageStreakGate = g_DX12OverlayCoverage.streakGate;
inline std::atomic<uint64_t>& dx12_hook_g_OverlayCoverageStreakStartTickMs = g_DX12OverlayCoverage.streakStartTickMs;
inline std::atomic<bool>& dx12_hook_g_OverlayCoverageStreakStartConfirmed = g_DX12OverlayCoverage.streakStartConfirmed;
inline ce::dx12_overlay_policy::OverlayPresentCoverageTracker& dx12_hook_g_OverlayCoverageTracker = g_DX12OverlayCoverage.tracker;
inline std::atomic_flag& dx12_hook_g_OverlayCoverageLock = g_DX12OverlayCoverage.lock;
extern thread_local bool dx12_hook_g_RequireExactPostSLStartupTransportDraw;
extern thread_local bool dx12_hook_g_PostSLDrawBelongsToEnclosingProcessFramePresent;
extern std::atomic<int> dx12_hook_g_OverlayHandoffVerboseLogPresents;
extern std::atomic<uint32_t> dx12_hook_g_OverlayHandoffVerbosePrevRoute;
inline std::atomic<int>& dx12_hook_g_PostSLStallCounter = g_DX12PostSLRuntime.stallCounter;
inline std::atomic<int>& dx12_hook_g_PostSLStableFrameCount = g_DX12PostSLRuntime.stableFrameCount;
extern std::atomic<bool> dx12_hook_g_ResetQueueChangeHeuristic;
extern std::atomic<bool> dx12_hook_g_ResetECLPatternHeuristic;
extern std::atomic<ID3D12CommandQueue*> dx12_hook_g_QueueChangeHeuristicAuthoritativeBaseline;
extern std::atomic<int> dx12_hook_g_SLOffHeuristicGrace;
extern std::atomic<int> dx12_hook_g_SLOffSwapchainReinitGrace;
extern std::atomic<bool> dx12_hook_g_ResetReinitSubmitCounter;
extern std::atomic<uint32_t> dx12_hook_g_OuterSLTransitionEpoch;
extern std::atomic<bool> dx12_hook_g_OuterTrackedSLFGRunning;
extern std::atomic<bool> dx12_hook_g_HadFSRFGPhase;
extern std::atomic<bool> dx12_hook_g_HadSuccessfulPostSLPhase;
extern std::atomic<bool> dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG;
extern bool dx12_hook_g_NeedGPUDrainBeforeRender;
extern ID3D12Fence* dx12_hook_g_DrainFence;
extern HANDLE dx12_hook_g_DrainEvent;
extern UINT64 dx12_hook_g_DrainFenceValue;
extern std::atomic<int> dx12_hook_g_PostFSRProbeLevel;
extern std::atomic<int> dx12_hook_g_PostFSRProbeFrames;
inline constexpr int dx12_hook_kPostFSRProbeFramesPerLevel = 3;;
extern bool dx12_hook_g_PostFSRDescFreeRecreated;
extern std::atomic<int> dx12_hook_g_SceneTransitionCooldown;
extern ID3D12CommandQueue* dx12_hook_g_PreFGGameQueue;
extern ID3D12CommandQueue* dx12_hook_g_OriginalGameQueue;
extern std::atomic<int> dx12_hook_g_FGTransitionCooldown;
// Ends the FG transition cooldown together with its PostSL mirror. The mirror only counts down while the
// cooldown does, so a mirror left armed outlives it: PostSL then skipped every frame once DLSS-G became the
// transport again (0.1.6951 dx12_fg_switch_test: "PostSL SKIP - FG transition cooldown active (60 frames
// left)" for 1549 presents / 6.5 s after a warm DLSS-G resume).
inline void EndFGTransitionCooldown() {
    dx12_hook_g_FGTransitionCooldown.store(0, std::memory_order_release);
    dx12_hook_g_PostSLCooldownRemaining.store(0, std::memory_order_release);
}
extern int dx12_hook_g_FramesSinceFGActive;
extern DX12DescFreeBackend* dx12_hook_g_DescFreeBackend;
extern DX12OverlayState dx12_hook_g_State;
extern SharedCaptureD3D12 dx12_hook_g_SharedCaptureD3D12;
extern OverlayAdapter dx12_hook_g_D3D11On12Adapter;
extern OverlayAdapter dx12_hook_g_SLFGAdapter;
extern OverlayAdapter dx12_hook_g_FFXPresentOverlayAdapter;
extern ID3D12Device* dx12_hook_g_FFXPresentOverlayDevice;
extern DXGI_FORMAT dx12_hook_g_FFXPresentOverlayFormat;
extern std::atomic<uint32_t> dx12_hook_g_NoCallbackBackbufferWidth;
extern std::atomic<uint32_t> dx12_hook_g_NoCallbackBackbufferHeight;
extern std::atomic<uint32_t> dx12_hook_g_NoCallbackBackbufferFormat;
extern ID3D12Device* dx12_hook_g_DescFreeBackendDevice;
extern DXGI_FORMAT dx12_hook_g_DescFreeBackendFormat;
extern ID3D12Resource* dx12_hook_g_OverlayBcBuffer;
extern volatile uint32_t* dx12_hook_g_OverlayBcMapped;
extern D3D12_GPU_VIRTUAL_ADDRESS dx12_hook_g_OverlayBcGpuVA;
extern std::atomic<uint32_t> dx12_hook_g_OverlayBcSeq;
extern std::atomic<int> dx12_hook_g_CommandListsExecutedThisFrame;
extern std::atomic<uint64_t> dx12_hook_g_FGDebugFrameCount;
extern std::atomic<int> dx12_hook_g_AuthoritativeFSRRealFrameOnlyStreak;
extern std::atomic<int> dx12_hook_g_StaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak;
extern std::atomic<bool> dx12_hook_g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun;
extern std::atomic<bool> dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown;
extern std::atomic<bool> dx12_hook_g_NativeFSRContextsDestroyedAwaitingGameSwapchain;
extern std::atomic<ID3D12CommandQueue*> dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue;
extern std::atomic<IDXGISwapChain*> dx12_hook_g_ExactGameSwapchainRecoverySwapchain;
extern std::atomic<IDXGISwapChain*> dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain;
extern std::atomic<IDXGISwapChain*> dx12_hook_g_PrewarmedPostSLHandoffSwapchain;
extern std::atomic<bool> dx12_hook_g_NativeFSRStartupConfigureArmingPending;
extern std::atomic<bool> dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending;
// Window of the protected FFX swapchain; null when the latch was armed without one.
extern std::atomic<HWND> dx12_hook_g_ProtectedOfficialFFXStartupHwnd;
extern std::atomic<uint32_t> dx12_hook_g_ProtectedOfficialFFXStartupProcessFrameSkips;
extern std::atomic<uint32_t> dx12_hook_g_ProtectedOfficialFFXStartupECLPassThroughs;
extern std::atomic<ULONGLONG> dx12_hook_g_ProtectedOfficialFFXStartupBeginMs;
extern std::atomic<bool> dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress;
extern std::atomic<ULONGLONG> dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs;
extern std::atomic<bool> dx12_hook_g_DeferredOfficialFFXTakeoverSideEffectsPending;
extern std::mutex dx12_hook_g_DeferredOfficialFFXTakeoverMutex;
extern ID3D12CommandQueue* dx12_hook_g_DeferredOfficialFFXTakeoverQueue;
extern char dx12_hook_g_DeferredOfficialFFXTakeoverModulePath[MAX_PATH];
extern std::atomic<ID3D12CommandQueue*> dx12_hook_g_PrimaryGameQueue;
extern std::atomic<bool> dx12_hook_g_KnownDLSSFGModuleSeen;
extern IDXGISwapChain* dx12_hook_g_LastSwapChain;
extern std::atomic<IDXGISwapChain*> dx12_hook_g_LastSwapchainQueueCaptureSwapchain;
extern std::atomic<IDXGISwapChain*> dx12_hook_g_LastProvenOriginalQueueSwapchain;
extern std::atomic<IDXGISwapChain*> dx12_hook_g_LastSuccessfulNormalOverlaySwapchain;
extern IDXGISwapChain* dx12_hook_g_PendingSwapChainCleanup;
extern std::atomic<bool> dx12_hook_g_LastKnownSwapchainHDRStateValid;
extern std::atomic<bool> dx12_hook_g_LastKnownSwapchainIsHDR;
extern std::atomic<int> dx12_hook_g_LastKnownSwapchainColorSpace;
extern std::mutex dx12_hook_g_StreamlineStartupActivationSwapchainMutex;
extern IDXGISwapChain* dx12_hook_g_StreamlineStartupActivationSwapchain;
extern std::atomic<DWORD> dx12_hook_g_GamePresentThreadId;
extern std::atomic<ID3D12CommandQueue*> dx12_hook_g_SLWrapperQueue;
extern std::atomic<ID3D12CommandQueue*> dx12_hook_g_RealQueueBehindSLWrapper;
extern std::atomic<bool> dx12_hook_g_SafePostFSRRuntimeOwnedSwapchainBootstrapLogged;
extern std::atomic<ID3D12CommandQueue*> dx12_hook_g_DeferredCommandQueueRelease;
extern std::atomic<ULONGLONG> dx12_hook_g_PostSLRecentTeardownActivityUntilMs;
extern ID3D12CommandQueue* dx12_hook_g_SwapchainQueue;
extern ULONGLONG dx12_hook_g_SwapchainQueueCaptureTime;
extern bool dx12_hook_g_FGRuntimeOwnsSwapchain;
extern ULONGLONG dx12_hook_g_FGRuntimeOwnsSwapchainSince;
// Set when DX12Hook::Init skipped the eager temp swapchain because a
// third-party overlay owned the creation path before the game's first real
// D3D12 device. The deferred install then has to honour the same condition
// instead of running immediately, or the deferral protects nothing.
extern std::atomic<bool> dx12_hook_g_EarlyPresentHookInstallDeferred;
extern std::atomic<bool> dx12_hook_g_PostponedPresentHookInstallLogged;
extern thread_local ForwardedCreateSwapchainForHwndCallerContext dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext;
extern thread_local int dx12_hook_s_forwardedCreateSwapchainForHwndInlineDepth;
extern thread_local bool dx12_hook_s_forwardedCreateSwapchainForHwndInlineHandled;
extern std::recursive_mutex dx12_hook_g_OverlayMutex;
extern std::recursive_mutex dx12_hook_g_DX12CaptureMutex;
extern std::atomic<bool> dx12_hook_g_InSwapchainResizeCleanup;
extern std::atomic<bool> dx12_hook_g_PreserveOverlayAdapterAcrossResize;
extern std::atomic<ID3D12Device*> dx12_hook_g_OverlayAdapterBackendDevice;
extern std::atomic<ID3D12CommandQueue*> dx12_hook_g_OverlayAdapterBackendQueue;
extern std::atomic<int> dx12_hook_g_OverlayAdapterBackendFormat;
extern std::atomic<int> dx12_hook_s_framesSinceInit;
extern std::atomic<int> dx12_hook_s_framesBeforeInit;
extern std::vector<ID3D12Fence*> dx12_hook_g_PrerenderFences;
extern std::vector<HANDLE> dx12_hook_g_PrerenderEvents;
extern uint64_t dx12_hook_g_PrerenderFrameIndex;
extern std::mutex dx12_hook_g_PrerenderMutex;
extern ID3D12Device* dx12_hook_g_PrerenderDevice;
extern ID3D12CommandQueue* dx12_hook_g_PrerenderQueue;
extern std::atomic<bool> dx12_hook_g_PiggybackOverlayActive;
extern ID3D12DescriptorHeap* dx12_hook_g_FFXPresentRtvHeap;
extern std::mutex dx12_hook_g_FFXPresentCallbackBridgeMutex;
extern std::unordered_map<void*, FFXPresentCallbackBridgeState> dx12_hook_g_FFXPresentCallbackBridges;
extern std::atomic<UINT64> dx12_hook_g_deferredSignalValue;
extern std::atomic<int> dx12_hook_g_deferredSignalAllocIdx;
extern std::atomic<ID3D12CommandQueue*> dx12_hook_g_deferredSignalQueue;
extern std::atomic<UINT64> dx12_hook_g_FocusLossPendingOverlayFenceValue;
extern std::atomic<bool> dx12_hook_g_FocusLossImmediateFenceDumpRequested;
extern std::atomic<bool> dx12_hook_g_FocusLossDeviceRemovalDumpRequested;
inline constexpr int dx12_hook_kFocusLossForegroundReacquirePresentProofFrames = 16;;
inline constexpr int dx12_hook_kFocusLossRecentTransitionDumpWindowFrames = 300;;
extern std::atomic<int> dx12_hook_g_FocusLossForegroundReacquirePresentProofRemaining;
extern std::atomic<int> dx12_hook_g_FocusLossRecentTransitionPresentWindow;
extern std::atomic<bool> dx12_hook_g_SwapchainPresentOccluded;
extern std::atomic<bool> dx12_hook_g_HaveD3D12PresentResultSignal;
inline constexpr int dx12_hook_kFocusTransitionHoldFrames = 60;;
extern std::atomic<int> dx12_hook_g_FocusTransitionHoldFrames;
extern thread_local DX12WrappedPresentFocusLossContext dx12_hook_s_WrappedPresentFocusLossContext;
extern thread_local bool dx12_hook_s_insideECL;
extern thread_local bool dx12_hook_s_insidePostSLOverlayECL;
extern thread_local int dx12_hook_s_insideCEOverlayECLDepth;
extern thread_local const char* dx12_hook_s_insideCEOverlayECLReason;
inline constexpr ULONGLONG dx12_hook_kProgressResolvedOfficialFFXOverlayFallbackStableMs = 5000;;
extern std::atomic<ULONGLONG> dx12_hook_g_FFXPresentCallbackFirstStallEverDetectedMs;
extern std::atomic<ULONGLONG> dx12_hook_g_OverlaySuppressedSinceMs;
extern std::atomic<uint32_t> dx12_hook_g_RuntimeOwnedStreamlineNoFGPresentCount;
inline constexpr uint32_t dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents = 8;;
extern std::recursive_mutex dx12_hook_g_FFXUiCompositeMutex;
extern ID3D12CommandQueue* dx12_hook_g_FFXUiCompositeQueue;
extern ID3D12Fence* dx12_hook_g_FFXUiCompositeFence;
extern UINT64 dx12_hook_g_FFXUiCompositeFenceVal;
extern int dx12_hook_g_FFXUiCompositeFrame;
inline constexpr int dx12_hook_kFFXUiCompositeTimelineSize = 32;;
inline constexpr ULONGLONG dx12_hook_kStartupOverlayWindowPollMs = 100;;
inline constexpr ULONGLONG dx12_hook_kStartupOverlayInitGraceMs = 500;;
inline constexpr ULONGLONG dx12_hook_kStartupOverlayWarmupMs = 500;;
inline constexpr ULONGLONG dx12_hook_kStartupOverlayQuietPeriodMs = 200;;
inline constexpr ULONGLONG dx12_hook_kStartupOverlayPostResumeSettleMs = 100;;
inline constexpr ULONGLONG dx12_hook_kStartupOverlayPostBackendInitSettleMs = 0;;
inline constexpr ULONGLONG dx12_hook_kStartupOverlayPostRTVInitSettleMs = 0;;
inline constexpr ULONGLONG dx12_hook_kStartupOverlayPostSyncInitSettleMs = 100;;
inline constexpr ULONGLONG dx12_hook_kStartupOverlayPostResourcePrimeSettleMs = 100;;
inline constexpr ULONGLONG dx12_hook_kStartupOverlayFirstDrawProbeSettleMs = 0;;
inline constexpr ULONGLONG dx12_hook_kStartupOverlayLoadedRenderModuleMaxBlockMs = 500;;
inline constexpr ULONGLONG dx12_hook_kStartupOverlayRenderModuleQuietPeriodMs = 500;;
inline constexpr DWORD dx12_hook_kOverlayCrossQueueWaitMs = 16;;
extern StartupOverlayActivationStage dx12_hook_s_startupOverlayActivationStage;
extern StartupOverlayFirstDrawProbeStage dx12_hook_s_startupOverlayFirstDrawProbeStage;
extern ULONGLONG dx12_hook_s_startupOverlayActivationStageMs;
extern ULONGLONG dx12_hook_s_startupOverlaySyncInitMs;
extern ULONGLONG dx12_hook_s_startupOverlayResourcePrimeMs;
extern ULONGLONG dx12_hook_s_startupOverlayFirstDrawProbeMs;
extern std::atomic<ULONGLONG> dx12_hook_s_lastStartupBlockingRenderModuleActivityMs;
extern std::atomic<bool> dx12_hook_s_startupOverlayCompatSettled;
extern std::atomic<bool> dx12_hook_s_startupOverlayObservedAnyFG;
extern std::atomic<bool> dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff;
extern std::atomic<bool> dx12_hook_g_DeviceRemoved;
extern PFN_CreateSwapChain dx12_hook_oCreateSwapChainGlobal;
extern PFN_CreateSwapChainForHwnd dx12_hook_oCreateSwapChainForHwndGlobal;
extern PFN_CreateSwapChainForHwnd dx12_hook_s_oCreateSCForHwndInline;
extern void* dx12_hook_s_realCreateSCForHwndAddr;
extern PFN_CreateSwapChainForHwnd dx12_hook_s_deepHookTrampoline;
// True when CE patched the CreateSwapChainForHwnd entry itself (no foreign patch, or no below-chain hook).
extern std::atomic<bool> dx12_hook_s_createSCForHwndEntryPrependInstalled;
// Which factory vtable slots CE redirected; a slot left to a loaded overlay is intercepted by a body hook.
extern std::atomic<bool> dx12_hook_s_createSCSlotHooked;
extern std::atomic<bool> dx12_hook_s_createSCForHwndSlotHooked;
extern std::atomic<bool> dx12_hook_s_createSCForHwndSlotLeftToOverlay;
// Body hook on CreateSwapChain, placed only when its slot is left to a loaded overlay.
extern void* dx12_hook_s_realCreateSCAddr;
extern PFN_CreateSwapChain dx12_hook_s_deepCreateSCTrampoline;
extern std::atomic<int64_t> dx12_hook_g_OverlayCooldownUntilQpc;
inline constexpr int64_t dx12_hook_kTransitionCooldownMs = 1500;  // 1.5 s;
extern std::mutex dx12_hook_s_hwndSwapchainMutex;
extern std::map<HWND, std::vector<IDXGISwapChain*>> dx12_hook_s_hwndSwapchainMap;
extern std::atomic<bool> dx12_hook_g_Dx12FocusAnalysisActive;
inline constexpr uint32_t dx12_hook_kDx12FaRingSize = 256;;
extern Dx12FocusAnalysisSample dx12_hook_g_Dx12FaRing[dx12_hook_kDx12FaRingSize];
extern std::atomic<uint64_t> dx12_hook_g_Dx12FaCount;
extern IDXGIAdapter3* dx12_hook_g_Dx12FaAdapter;
extern std::atomic<bool> dx12_hook_s_initDelayComplete;
