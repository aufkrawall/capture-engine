#pragma once

#include "dx12_hook_internal.h"

#include "overlay_draw_transaction.h"

// One present transaction owns temporary state, metrics completion and the
// borrowed/owned resources used by its preparation and draw operations.
class FrameProcessSession {
public:
    FrameProcessSession(IDXGISwapChain* pSwapChain, bool processCapture,
                       bool applicationSourcePresent, bool frameGenerationPresentationActive,
                       ce::dx12_process_frame_diagnostics::StageTimings* diagnostics)
        : pSwapChain(pSwapChain), processCapture(processCapture),
          applicationSourcePresent(applicationSourcePresent),
          frameGenerationPresentationActive(frameGenerationPresentationActive),
          diagnostics(diagnostics) {}

    ~FrameProcessSession() = default;

    FrameProcessSession(const FrameProcessSession&) = delete;
    FrameProcessSession& operator=(const FrameProcessSession&) = delete;

    void Run();
private:
    IDXGISwapChain* pSwapChain;
    bool processCapture;
    bool applicationSourcePresent;
    bool frameGenerationPresentationActive;
    ce::dx12_process_frame_diagnostics::StageTimings* diagnostics;
    // Metrics completion stays inside the frame transaction.
    bool metricsGuardArmed = false;

    FrameMetrics perfMetrics{};
    PresentDebugSample* activeDebugSample;
    int64_t processFrameStartUs;
    bool protectedOfficialFFXStartupOverlayOnly;
    bool inResize;
    DXGI_SWAP_CHAIN_DESC frameDesc{};
    bool hasOutputWindow;
    bool outputWindowVisible;
    bool zeroSizedSwapchain;
    bool iconicWindow;
    HWND foregroundWindow;
    DWORD foregroundPid;
    DWORD currentProcessId;
    bool processHasForeground;
    bool inTransitionCooldown;
    bool suspendOverlayHeavy;
    bool suspendOverlayRender;
    float prerenderLimit;
    bool postFSRNormalRouteExplicitQueueProof;
    bool postFSRNormalRouteRememberedSwapchainProof;
    bool postFSRNormalRouteOwnershipProven;
    bool authoritativeDLSSOffNormalReturnReinitializedThisPresent = false;
    bool nativeFSRGameSwapchainRecoveryReinitializedThisPresent = false;
    bool exactGameSwapchainRecoverySwapchainProof = false;
    bool exactPrewarmedPostSLHandoffBackendPreservedThisPresent = false;
    bool independentFSRTopmostCompositedThisPresent = false;
    std::unique_lock<std::recursive_mutex> lock;
    bool allowOverlayRender;
    SharedMemoryLayout* observerModeShm;
    bool observerOnlyMode;
    bool observerPolicyOnlyMode;
    bool observerStartupPresentOnlyMode;
    ULONGLONG postResumeSettleRemainingMs;
    bool startupOverlayCompatibilityActive;
    ULONGLONG runtimeOwnedSwapchainActiveMs;
    bool runtimeOwnedSwapchainNeedsExtraResumeSettle;
    bool deferOverlayWorkAfterResume;
    bool processNeedsStartupOverlayInitDelay;
    bool exactPostDLSSOffNormalReturnSwapchainProof;
    bool exactPrewarmedPostSLHandoffSwapchainProof;
    bool processLogicalSwapchainReplacement;
    ID3D12CommandQueue* gameQueue;
    bool currentSwapchainProvenOnOriginalQueue;
    bool startupOverlayPresent;
    UINT currentBackBufferIdx;
    bool hasCurrentBackBufferIdx;
    bool pendingFocusLossBackbufferWorkHold;
    bool focusLossBackgroundDeviceLost;
    bool focusLossBackgroundUsingDedicatedQueue;
    bool focusLossBackgroundRuntimeOwnedPresentation;
    bool focusLossBackgroundSteamDeferredSubmit;
    bool focusLossBackgroundFrameGenerationActive;
    bool swapchainOccluded;
    bool haveReliablePresentResultSignal;
    bool focusLossBackgroundBackbufferHold;
    int focusTransitionHoldRemaining;
    bool focusTransitionActive;
    bool holdFocusLossBackbufferWork;
    SharedMemoryLayout* captureShm;
    OverlayConfig captureOverlayCfg;
    bool captureWantsOverlay;
    bool captureUsePostSL;
    bool captureAfterOverlay = false;
    bool captureBeforeOverlay = false;
    // Set where the overlay draw chain publishes the overlay-free capture.
    // PublishPostOverlayCapture publishes it when that chain never got there.
    bool captureBeforeOverlayPublished = false;
    bool delayOverlayRenderAfterSyncInit;
    bool suppressOverlayRenderForLoadedStartupOverlay;
    bool delayOverlayRenderAfterResourcePrime;
    bool delayOverlayRenderAfterFirstDrawProbe;
    bool delayOverlayRenderAfterResume;
    bool shouldRunStartupOverlayDrawProbe;
    bool currentFGActive;
    ce::fg_runtime::RuntimeMode currentRuntimeMode;
    bool currentSLFGRunning;
    uint32_t outerEpoch;
    ID3D12CommandQueue* transitionSwapchainQueue;
    bool transitionRecoveringPostFSRNonFG;
    bool transitionStartupBypassActive;
    bool fgChanged;
    bool runtimeModeChanged;
    bool slSignalChanged;
    bool skipOverlayDraw;
    // The FG transition cooldown holds PostSL bookkeeping (skipOverlayDraw stays set) but the pre-SL
    // draw still runs: DLSS-G toggle-ON before the first confirmed PostSL render.
    bool preSLDrawKeptThroughDLSSToggleOn = false;
    bool slFGActive;
    const char* skipSeparateOverlayGpuReason;
    uint64_t frameNum;
    int allocatorPoolSize;
    int idx;
    ID3D12GraphicsCommandList* list;
    ID3D12CommandAllocator* alloc;
    HRESULT allocResetHr = E_FAIL;
    HRESULT listResetHr = E_FAIL;
    bool preserveLiveStartupOverlayDuringInactiveSL;
    bool hasPendingStartupOverlayResources;
    bool shouldPrimeStartupOverlayResources;
    IDXGISwapChain3* sc3;
    LARGE_INTEGER perfQI, perfGetBuf, perfRecord, perfSubmit, perfEnd, perfFreq{};
    UINT swapchainBufferIdx;
    UINT bufferIdx;
    ce::dx12::DrawBackBuffer<ID3D12Resource> backBuffer;
    bool cmdRecordOk;
    bool usedPrimaryOverlayBackend;
    bool usedDescFree;
    bool offscreenCompositeRequired;
    bool overlayDrawRecorded;
    HRESULT closeHr = E_FAIL;


    ProcessFrameFlow PrepareFrame();
    void EndPostFSRNonFGRecoveryOnProvenSwapchainChange(bool normalRouteOwnershipProven,
                                                        bool exactPrewarmedStreamlineHandoff);
    ProcessFrameFlow TrackSwapchainAndSelectQueue();
    ProcessFrameFlow InitOverlayBackend();
    ProcessFrameFlow InitOverlaySyncAndFocusHold();
    ProcessFrameFlow HandleOuterFGTransition();
    ProcessFrameFlow DrawOverlayFrame();
    ProcessFrameFlow DrawFrameTransition();
    ProcessFrameFlow DrawCooldownAndRoute();
    ProcessFrameFlow DrawMain();
    ProcessFrameFlow DrawSkipAndCounters();
    bool TryCompositeOverlayBelowForeignChainForRuntimeOwnedFSR();
    struct DrawOperations;
    void LogFrameMetrics();
    ce::dx12::BackBufferAcquisition<ID3D12Resource> AcquireDrawBackBuffer();
    void RefreshDrawRenderTarget(ID3D12Resource* buffer);
    HRESULT CloseDrawCommands();
    void ReportDrawFailure(ce::dx12::DrawFailure failure, HRESULT result);
    ProcessFrameFlow DrawDeviceScope();
    ProcessFrameFlow DrawAllocSetup();
    ProcessFrameFlow DrawListAndAlloc();
    ProcessFrameFlow DrawAllocReset();
    ProcessFrameFlow DrawReset();
    ProcessFrameFlow DrawResetFront();
    ProcessFrameFlow DrawSubmit();
    ProcessFrameFlow DrawSubmitSetup();
    ProcessFrameFlow DrawSc3();
    ProcessFrameFlow DrawSc3Front();
    ProcessFrameFlow DrawSubmitCore();
    ProcessFrameFlow DrawSubmitCoreFront();
    ProcessFrameFlow DrawSubmitCoreTail();
    ProcessFrameFlow DrawSc3Else();
    ProcessFrameFlow DrawSubmitElse();
    ProcessFrameFlow DrawResetElse();
    ProcessFrameFlow DrawNullList();
    ProcessFrameFlow pw5_c3();
    ProcessFrameFlow PublishPostOverlayCapture();
};
