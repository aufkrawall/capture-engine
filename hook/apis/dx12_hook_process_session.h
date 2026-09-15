#pragma once

#include "dx12_hook_internal.h"

// FrameProcessSession: the former ProcessFrame body in dx12_hook_process.cpp, split into
// phase methods + a recursive draw-region chunk tree. Early returns and forward gotos are
// routed via ProcessFrameFlow; Run() stops on kReturn and the DrawOverlay wrapper owns the
// skip_overlay_draw/overlay_done labels.

enum class ProcessFrameFlow {
    kContinue,
    kReturn,
    kSkipOverlayInit,
    kSkipOverlayDraw,
    kOverlayDone,
    kSkipSteamFence,
};

class FrameProcessSession {
public:
    FrameProcessSession(IDXGISwapChain* pSwapChain, bool processCapture,
                       bool applicationSourcePresent, bool frameGenerationPresentationActive,
                       ce::dx12_process_frame_diagnostics::StageTimings* diagnostics)
        : pSwapChain(pSwapChain), processCapture(processCapture),
          applicationSourcePresent(applicationSourcePresent),
          frameGenerationPresentationActive(frameGenerationPresentationActive),
          diagnostics(diagnostics) {}

    // The per-frame backbuffer is acquired with GetBuffer() and must not outlive the
    // present: DrawSubmitCoreFront's own comment records that a frame generator watches
    // the backbuffer reference count and fails when extra references are held. There is
    // exactly one release point deep inside DrawSubmitCoreTail, and two early returns
    // above it (device-removed, and the Steam-deferred submit) skipped it. Owning the
    // reference here means every exit path releases it, including ones added later.
    ~FrameProcessSession() { ReleaseBackBuffer(); }

    FrameProcessSession(const FrameProcessSession&) = delete;
    FrameProcessSession& operator=(const FrameProcessSession&) = delete;

    void Run();
    void LogFrameMetrics();

    // Idempotent: the normal path still releases at the original point in DrawSubmitCoreTail,
    // and the destructor is the backstop for every other exit.
    void ReleaseBackBuffer() {
        if (bbNeedsRelease && bb) {
            bb->Release();
        }
        bb = nullptr;
        bbNeedsRelease = false;
    }

private:
    IDXGISwapChain* pSwapChain;
    bool processCapture;
    bool applicationSourcePresent;
    bool frameGenerationPresentationActive;
    ce::dx12_process_frame_diagnostics::StageTimings* diagnostics;
public:
    // Entry reads the armed flag and logs metrics after Run(); keep these public.
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
    bool captureAfterOverlay;
    bool captureBeforeOverlay;
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
    bool slFGActive;
    const char* skipSeparateOverlayGpuReason;
    uint64_t frameNum;
    int allocatorPoolSize;
    int idx;
    ID3D12GraphicsCommandList* list;
    ID3D12CommandAllocator* alloc;
    HRESULT allocResetHr;
    HRESULT listResetHr;
    bool preserveLiveStartupOverlayDuringInactiveSL;
    bool hasPendingStartupOverlayResources;
    bool shouldPrimeStartupOverlayResources;
    IDXGISwapChain3* sc3;
    LARGE_INTEGER perfQI, perfGetBuf, perfRecord, perfSubmit, perfEnd, perfFreq{};
    UINT swapchainBufferIdx;
    UINT bufferIdx;
    // Default-initialized: the destructor reads both on every exit, including presents that
    // never reach the GetBuffer() call that assigns them.
    ID3D12Resource* bb = nullptr;
    bool bbNeedsRelease = false;
    bool cmdRecordOk;
    bool usedPrimaryOverlayBackend;
    bool usedDescFree;
    bool offscreenCompositeRequired;
    bool overlayDrawRecorded;
    HRESULT closeHr;


    ProcessFrameFlow Phase1();
    ProcessFrameFlow Phase2();
    ProcessFrameFlow Phase3();
    ProcessFrameFlow Phase4();
    ProcessFrameFlow Phase5();
    ProcessFrameFlow DrawOverlayFrame();
    ProcessFrameFlow DrawFrameTransition();
    ProcessFrameFlow DrawCooldownAndRoute();
    ProcessFrameFlow DrawMain();
    ProcessFrameFlow DrawSkipAndCounters();
    bool TryCompositeOverlayBelowForeignChainForRuntimeOwnedFSR();
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
    ProcessFrameFlow Phase6Tail();
};
