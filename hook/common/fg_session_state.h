#pragma once

#include <d3d12.h>
#include <windows.h>

#include <cstdint>

#include "fg_runtime_state.h"

namespace ce::fg_session {

inline constexpr uint32_t kFGStateSchemaVersion = 1;

enum class FGAuthorityKind {
    kNone,
    kStreamlineGetStateProvisional,
    kStreamlineSetOptionsAuthoritative,
    kNativeFSRConfigureAuthoritative,
    kNativeFSRContextOnly,
    kHeuristic,
};

enum class FGStartupPhase {
    kNone,
    kHandoffPending,
    kChurnWindow,
    kActivationPending,
    kActiveUnconfirmed,
    kSettling,
    kStable,
};

enum class FGOverlayBackendMode {
    kSuppressed,
    kNormalPreSL,
    kStartupBypass,
    kPostSL,
    kRuntimeOwnedFSRCallback,
    kPostFSRRecovery,
};

enum class FGPresentRoute {
    kTopLevel,
    kSyntheticReentrant,
    kStartupHandoffNormalRoute,
    kConfirmedStandaloneNormalRoute,
    kPassiveBypass,
};

enum class FGPresentTransport {
    kNormalChain,
    kTrampoline,
    kDirectBypass,
};

enum class FGQueueRole {
    kNone,
    kOriginalGame,
    kSwapchain,
    kWrapperBootstrap,
    kRealBehindWrapper,
    kDedicatedOverlayQueue,
    kPostSLLastWorking,
    kFFXCallbackQueue,
};

enum class FGEventKind {
    kUnknown,
    kStreamlineGetStateRuntimeUpdate,
    kStreamlineSetOptionsRuntimeUpdate,
    kAuthoritativeStreamlineStartupHandoff,
    kAuthoritativeFFXTakeover,
    kNativeFSRConfigureOn,
    kNativeFSRConfigureOff,
    kFFXContextDestroy,
    kSwapchainInvalidation,
    kPresentObserved,
    kPostSLCallbackInstalled,
    kPostSLCallbackRemoved,
    kPostSLActivationComplete,
    kPostSLFirstConfirmedRender,
    kStartupWindowExpired,
    kStaleOwnershipCleanupComplete,
    kTransitionCooldownComplete,
};

struct FGQueueProof {
    ID3D12CommandQueue* ptr = nullptr;
    bool valid = false;
    bool runtimeOwned = false;
    bool wrapperDerived = false;
    bool directBehindWrapper = false;
    uint32_t epoch = 0;
    const char* source = "none";
};

struct FGFunctionProof {
    void* ptr = nullptr;
    bool valid = false;
    uint32_t epoch = 0;
    const char* source = "none";
};

struct FGTransportRisk {
    bool thirdPartyOverlayLoaded = false;
    bool steamOverlayLoaded = false;
    bool staleSteamPresentHookRisk = false;
    bool cleanNonWrappedDX12Entry = false;
    bool bypassAvailable = false;
};

struct FGSessionSnapshot {
    uint32_t sessionEpoch = 0;
    uint32_t runtimeEpoch = 0;
    uint32_t swapchainEpoch = 0;
    uint32_t queueEpoch = 0;

    fg_runtime::RuntimeMode effectiveRuntimeMode = fg_runtime::RuntimeMode::kOff;
    bool effectiveFGActive = false;
    bool streamlineLoaded = false;
    bool streamlineFGSignal = false;
    bool ffxLoaded = false;
    bool nativeFSRConfiguredOn = false;
    bool runtimeOwnsSwapchain = false;
    bool hadFSRPhase = false;
    bool safePostFSRBootstrapPath = false;
    bool explicitSetOptionsActivationForCurrentComeback = false;
    bool startupWindowActive = false;
    ULONGLONG startupWindowRemainingMs = 0;
    bool startupTopLevelPresentConsumed = false;
    bool streamlineStartupHandoffPending = false;
    bool postSLCallbackInstalled = false;
    bool postSLActive = false;
    bool postSLConfirmedRendering = false;
    bool postSLSettling = false;
    bool postSLStartupActivationPending = false;
    bool postSLActiveButUnconfirmed = false;
    int postSLStableFrameCount = 0;
    int fgTransitionCooldown = 0;
    bool observerOnly = false;
    bool observerPolicyOnly = false;
    bool observerStartupPresentOnly = false;

    FGAuthorityKind authority = FGAuthorityKind::kNone;
    FGStartupPhase startupPhase = FGStartupPhase::kNone;
    FGOverlayBackendMode overlayMode = FGOverlayBackendMode::kNormalPreSL;

    FGQueueProof originalGameQueue;
    FGQueueProof primaryGameQueue;
    FGQueueProof swapchainQueue;
    FGQueueProof currentCommandQueue;
    FGQueueProof slWrapperQueue;
    FGQueueProof realQueueBehindWrapper;
    FGQueueProof postSLLockedQueue;
    FGQueueProof postSLLastWorkingQueue;
    FGQueueProof postSLDedicatedQueue;

    FGFunctionProof realECL;
    FGFunctionProof presentHookAnchor;

    FGTransportRisk transportRisk;
};

struct FGEvent {
    FGEventKind kind = FGEventKind::kUnknown;
    const char* source = "unknown";
    void* ptrA = nullptr;
    void* ptrB = nullptr;
    fg_runtime::RuntimeMode hintedRuntimeMode = fg_runtime::RuntimeMode::kUnknown;
    bool hintedActive = false;
    bool hintedExplicitActivation = false;
    ULONGLONG timestampMs = 0;
    uint32_t sessionEpoch = 0;
    uint32_t runtimeEpoch = 0;
    uint32_t swapchainEpoch = 0;
    uint32_t queueEpoch = 0;
};

struct FGActionPlan {
    FGPresentRoute route = FGPresentRoute::kTopLevel;
    FGPresentTransport transport = FGPresentTransport::kNormalChain;
    bool invokePostSLCallback = false;
    bool keepPostSLCallbackInstalled = false;
    FGOverlayBackendMode backendMode = FGOverlayBackendMode::kNormalPreSL;
    FGQueueRole selectedQueueRole = FGQueueRole::kNone;
    ID3D12CommandQueue* selectedQueue = nullptr;
    bool publishFGActive = false;
    fg_runtime::RuntimeMode publishRuntimeMode = fg_runtime::RuntimeMode::kOff;
    bool suppressHeuristics = false;
    bool preserveLastWorkingQueue = false;
    bool clearWrapperBootstrapState = false;
    bool reprobRealECLIfMissing = false;
    const char* reason = "none";
};

struct DX12LegacyStateView {
    ID3D12CommandQueue* originalGameQueue = nullptr;
    ID3D12CommandQueue* primaryGameQueue = nullptr;
    ID3D12CommandQueue* swapchainQueue = nullptr;
    ID3D12CommandQueue* currentCommandQueue = nullptr;
    ID3D12CommandQueue* slWrapperQueue = nullptr;
    ID3D12CommandQueue* realQueueBehindWrapper = nullptr;
    ID3D12CommandQueue* postSLLockedQueue = nullptr;
    ID3D12CommandQueue* postSLLastWorkingQueue = nullptr;
    ID3D12CommandQueue* postSLDedicatedQueue = nullptr;
    void* realECL = nullptr;
    bool runtimeOwnsSwapchain = false;
    bool hadFSRPhase = false;
    bool safePostFSRBootstrapPath = false;
    bool explicitSetOptionsActivationForCurrentComeback = false;
    bool streamlineStartupHandoffPending = false;
    bool startupTopLevelPresentConsumed = false;
    bool postSLCallbackInstalled = false;
    bool postSLActive = false;
    bool postSLConfirmedRendering = false;
    bool postSLSettling = false;
    bool postSLStartupActivationPending = false;
    bool postSLActiveButUnconfirmed = false;
    int postSLStableFrameCount = 0;
    int fgTransitionCooldown = 0;
    bool observerOnly = false;
    bool observerPolicyOnly = false;
    bool observerStartupPresentOnly = false;
    bool usingFFXPresentCallbackPath = false;
};

using DX12LegacyStateProvider = void (*)(DX12LegacyStateView* out);

void RegisterDX12LegacyStateProvider(DX12LegacyStateProvider provider);

FGSessionSnapshot CaptureFGSessionSnapshot();
FGActionPlan BuildFGActionPlan(const FGSessionSnapshot& snapshot);
bool ValidateFGSessionSnapshot(const FGSessionSnapshot& current, const FGSessionSnapshot* previous = nullptr);

void EmitFGEvent(const FGEvent& event);
void EmitFGEvent(FGEventKind kind, const char* source, void* ptrA = nullptr, void* ptrB = nullptr,
                 fg_runtime::RuntimeMode hintedRuntimeMode = fg_runtime::RuntimeMode::kUnknown,
                 bool hintedActive = false, bool hintedExplicitActivation = false);

FGSessionSnapshot GetLatestFGSessionSnapshot();
FGActionPlan GetLatestFGActionPlan();
bool IsFGShadowStateEnabled();
uint32_t GetFGStateSchemaVersion();
void ResetFGSessionStateForTests();

const char* GetFGAuthorityKindName(FGAuthorityKind kind);
const char* GetFGStartupPhaseName(FGStartupPhase phase);
const char* GetFGOverlayBackendModeName(FGOverlayBackendMode mode);
const char* GetFGPresentRouteName(FGPresentRoute route);
const char* GetFGPresentTransportName(FGPresentTransport transport);
const char* GetFGQueueRoleName(FGQueueRole role);
const char* GetFGEventKindName(FGEventKind kind);

}  // namespace ce::fg_session
