#include "streamline_hook.h"
#include <tlhelp32.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include "../common/dx12_overlay_policy.h"
#include "../common/dxgi_shared.h"
#include "../common/fg_detection.h"
#include "../common/fg_session_state.h"
#include "../common/freeze_watchdog.h"
#include "../common/hook_common.h"
#include "../common/reflex_limiter.h"
#include "../common/streamline_runtime_policy.h"
#include "../wrappers/iat_hook.h"
#include "../wrappers/inline_hook.h"
#include "dx12_hook.h"
#include "dx12_streamline_ui_overlay.h"

namespace {

std::atomic<bool> g_StartupProtectedOffChurnNeedsActiveProof{false};
std::atomic<uint32_t> g_StartupProtectedOffChurnActiveProofCount{0};

bool IsObserverOnlyModeActive() {
    return HookOverlayObserverOnlyEnabled();
}

void LogDroppedSuppressedOffForStartupProtectedStreamlineComeback(
    uint32_t viewportKey, bool hadFSRFGPhase, bool explicitSetOptionsActivationForCurrentComeback,
    bool safePostFSRBootstrapPath, bool startupActivationPending, bool postSLActiveButUnconfirmed,
    bool postSLConfirmedRendering, bool postSLConfirmedButStartupSettling,
    bool postSLConfirmedButRuntimeStateStabilizing) {
    static std::atomic<int> s_dropLogCount{0};
    const int logCount = s_dropLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 100) == 0) {
        HookLogImportant(
            "Streamline Hook: Dropping stale suppressed slDLSSGSetOptions(OFF) after startup window expiry because "
            "Streamline DLSS startup is already stably active (viewport=%u hadFSR=%d explicit=%d safeBootstrap=%d "
            "pending=%d unconfirmed=%d confirmed=%d settling=%d stabilizing=%d)",
            viewportKey, hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
            safePostFSRBootstrapPath ? 1 : 0, startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
            postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
            postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
    }
}

bool IsObserverPolicyOnlyModeActive() {
    return HookOverlayObserverPolicyOnlyEnabled();
}

bool ShouldKeepPureObserverOnlyStreamlineBehavior() {
    return ce::streamline_runtime_policy::ShouldKeepPureObserverOnlyStreamlineBehavior(
        IsObserverOnlyModeActive(), IsObserverPolicyOnlyModeActive());
}

bool TryServicePostSLStartupActivation(const char* source, bool clearStartupWindow) {
    auto service = DXGIShared::g_PostSLStartupActivationService.load(std::memory_order_acquire);
    if (!service) {
        static std::atomic<int> s_missingServiceLogCount{0};
        const int logCount = s_missingServiceLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: PostSL startup activation service unavailable "
                "(source=%s clearWindow=%d)",
                source ? source : "unknown", clearStartupWindow ? 1 : 0);
        }
        return false;
    }

    return service(source, clearStartupWindow);
}

void ResetStartupProtectedOffChurnActiveProof(const char* reason) {
    const bool wasPending = g_StartupProtectedOffChurnNeedsActiveProof.exchange(false, std::memory_order_acq_rel);
    const uint32_t previousProof = g_StartupProtectedOffChurnActiveProofCount.exchange(0, std::memory_order_acq_rel);
    if (wasPending || previousProof > 0) {
        static std::atomic<int> s_resetLogCount{0};
        const int logCount = s_resetLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Reset startup-protected OFF quiet proof "
                "(reason=%s wasPending=%d activeProof=%u)",
                reason ? reason : "unknown", wasPending ? 1 : 0, previousProof);
        }
    }
}

void LogAcceptedOffDuringActivatedUnconfirmedResume(const char* source, bool startupWindowActive, bool hadFSRFGPhase,
                                                    bool explicitSetOptionsActivationForCurrentComeback,
                                                    bool safePostFSRBootstrapPath, bool startupActivationPending,
                                                    bool postSLActiveButUnconfirmed,
                                                    bool postSLStartupActivationEntered, bool postSLConfirmedRendering,
                                                    bool postSLConfirmedButStartupSettling,
                                                    bool postSLConfirmedButRuntimeStateStabilizing) {
    static std::atomic<int> s_acceptLogCount{0};
    const int logCount = s_acceptLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 100) == 0) {
        HookLogImportant(
            "Streamline Hook: Accepting Streamline OFF during activated-but-unconfirmed startup resume "
            "(source=%s startupWindow=%d hadFSR=%d explicit=%d safeBootstrap=%d pending=%d unconfirmed=%d "
            "startupActivationEntered=%d confirmed=%d settling=%d stabilizing=%d) — forwarding real suspend instead "
            "of treating it as stale startup churn",
            source ? source : "runtime-state", startupWindowActive ? 1 : 0, hadFSRFGPhase ? 1 : 0,
            explicitSetOptionsActivationForCurrentComeback ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0,
            startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
            postSLStartupActivationEntered ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
            postSLConfirmedButStartupSettling ? 1 : 0, postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
    }
}

void MarkStartupProtectedOffChurnObserved(const char* source, bool postSLConfirmedRendering,
                                          bool postSLConfirmedButStartupSettling,
                                          bool postSLConfirmedButRuntimeStateStabilizing) {
    const bool wasPending = g_StartupProtectedOffChurnNeedsActiveProof.exchange(true, std::memory_order_acq_rel);
    const uint32_t previousProof = g_StartupProtectedOffChurnActiveProofCount.exchange(0, std::memory_order_acq_rel);
    if (!wasPending || previousProof > 0) {
        static std::atomic<int> s_churnLogCount{0};
        const int logCount = s_churnLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Startup-protected OFF churn requires fresh active proof before accepting disable "
                "(source=%s previousProof=%u required=%u confirmed=%d settling=%d stabilizing=%d)",
                source ? source : "runtime-state", previousProof,
                ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProofUpdateThreshold(),
                postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
                postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
        }
    }
}

void MarkStartupProtectedActiveRuntimeProof(const char* source, int multiplier) {
    if (!g_StartupProtectedOffChurnNeedsActiveProof.load(std::memory_order_acquire)) {
        return;
    }

    const uint32_t previousProof = g_StartupProtectedOffChurnActiveProofCount.load(std::memory_order_acquire);
    if (ce::streamline_runtime_policy::HasStartupProtectedOffChurnActiveProof(previousProof)) {
        return;
    }

    const uint32_t newProof = g_StartupProtectedOffChurnActiveProofCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (ce::streamline_runtime_policy::HasStartupProtectedOffChurnActiveProof(newProof)) {
        const bool wasPending = g_StartupProtectedOffChurnNeedsActiveProof.exchange(false, std::memory_order_acq_rel);
        if (wasPending) {
            HookLogImportant(
                "Streamline Hook: Startup-protected OFF churn quiet proof reached "
                "(source=%s activeProof=%u required=%u multiplier=%dx) — future OFF edges may be accepted",
                source ? source : "runtime-state", newProof,
                ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProofUpdateThreshold(), multiplier);
        }
    } else {
        static std::atomic<int> s_activeProofLogCount{0};
        const int logCount = s_activeProofLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Startup-protected OFF churn active proof progress "
                "(source=%s activeProof=%u/%u multiplier=%dx)",
                source ? source : "runtime-state", newProof,
                ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProofUpdateThreshold(), multiplier);
        }
    }
}

bool IsStartupProtectedOffChurnAwaitingActiveProof(bool startupProtectedComebackProof, bool postSLConfirmedRendering,
                                                   bool postSLConfirmedButStartupSettling) {
    return ce::streamline_runtime_policy::ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof(
        g_StartupProtectedOffChurnNeedsActiveProof.load(std::memory_order_acquire),
        g_StartupProtectedOffChurnActiveProofCount.load(std::memory_order_acquire), startupProtectedComebackProof,
        postSLConfirmedRendering, postSLConfirmedButStartupSettling);
}

thread_local int g_ExternalOverlayPresentGuardDepth = 0;

using slResult = int;

constexpr slResult kSlResultOk = 0;
constexpr slResult kSlResultErrorInvalidState = 38;
constexpr uint32_t kSLFeatureDLSSG = 1000;
constexpr uint32_t kSLFeatureReflex = 3;  // Streamline Reflex feature ID
constexpr size_t kSLStructVersion1 = 1;
constexpr size_t kSLStructVersion2 = 2;
constexpr size_t kSLStructVersion3 = 3;
constexpr size_t kSLStructVersion4 = 4;
constexpr size_t kSLStructVersion5 = 5;
constexpr char kSLBooleanInvalid = 2;

// Streamline Reflex mode constants
constexpr int kSLReflexModeOff = 0;
constexpr int kSLReflexModeEnabled = 1;
constexpr int kSLReflexModeLowLatency = 2;
constexpr int kSLReflexModeLowLatencyWithBoost = 3;
constexpr int kSLReflexOptionsModeOff = 0;
constexpr int kSLReflexOptionsModeLowLatency = 1;
constexpr int kSLReflexOptionsModeLowLatencyWithBoost = 2;

// Streamline sl::ReflexConstants structure (matches Streamline SDK)
struct SLReflexConstants {
    size_t structSize = sizeof(SLReflexConstants);
    uint32_t version = static_cast<uint32_t>(kSLStructVersion1);
    int32_t mode = kSLReflexModeOff;
    uint32_t frameLimitUs = 0;
    uint32_t markersEnabled = 0;
    uint32_t useMarkersToOptimize = 0;
};

struct slStructType {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

struct slBaseStructure {
    slBaseStructure() = default;
    slBaseStructure(slStructType type, size_t version) : structType(type), structVersion(version) {}

    slBaseStructure* next = nullptr;
    slStructType structType{};
    size_t structVersion = 0;
};

constexpr slStructType kDLSSGOptionsStructType = {
    0xfac5f1cb, 0x2dfd, 0x4f36, {0xa1, 0xe6, 0x3a, 0x9e, 0x86, 0x52, 0x56, 0xc5}};
constexpr slStructType kDLSSGStateStructType = {
    0xcc8ac8e1, 0xa179, 0x44f5, {0x97, 0xfa, 0xe7, 0x41, 0x12, 0xf9, 0xbc, 0x61}};
constexpr slStructType kViewportHandleStructType = {
    0x171b6435, 0x9b3c, 0x4fc8, {0x99, 0x94, 0xfb, 0xe5, 0x25, 0x69, 0xaa, 0xa4}};
constexpr slStructType kResourceTagStructType = {
    0x4c6a5aad, 0xb445, 0x496c, {0x87, 0xff, 0x1a, 0xf3, 0x84, 0x5b, 0xe6, 0x53}};
constexpr slStructType kReflexOptionsStructType = {
    0xf03af81a, 0x6d0b, 0x4902, {0xa6, 0x51, 0xc4, 0x96, 0x5e, 0x21, 0x54, 0x34}};

struct slViewportHandle : slBaseStructure {
    slViewportHandle() : slBaseStructure(kViewportHandleStructType, kSLStructVersion1) {}

    uint32_t value = 0xFFFFFFFFu;
};

struct slExtent {
    uint32_t top = 0;
    uint32_t left = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

enum class slResourceType : char {
    kTexture2D = 0,
};

struct slResource : slBaseStructure {
    slResourceType type = slResourceType::kTexture2D;
    void* native = nullptr;
    void* memory = nullptr;
    void* view = nullptr;
    uint32_t state = UINT_MAX;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t nativeFormat = 0;
    uint32_t mipLevels = 0;
    uint32_t arrayLayers = 0;
    uint64_t gpuVirtualAddress = 0;
    uint32_t flags = 0;
    uint32_t usage = 0;
    uint32_t reserved = 0;
};

struct slResourceTag : slBaseStructure {
    slResource* resource = nullptr;
    uint32_t type = 0;
    int32_t lifecycle = 0;
    slExtent extent{};
};

constexpr uint32_t kSLBufferTypeUIColorAndAlpha = 23;

struct slDLSSGOptions : slBaseStructure {
    slDLSSGOptions() : slBaseStructure(kDLSSGOptionsStructType, kSLStructVersion5) {}

    uint32_t mode = 0;
    uint32_t numFramesToGenerate = 1;
    uint32_t flags = 0;
    uint32_t dynamicResWidth = 0;
    uint32_t dynamicResHeight = 0;
    uint32_t numBackBuffers = 0;
    uint32_t mvecDepthWidth = 0;
    uint32_t mvecDepthHeight = 0;
    uint32_t colorWidth = 0;
    uint32_t colorHeight = 0;
    uint32_t colorBufferFormat = 0;
    uint32_t mvecBufferFormat = 0;
    uint32_t depthBufferFormat = 0;
    uint32_t hudLessBufferFormat = 0;
    uint32_t uiBufferFormat = 0;
    void* onErrorCallback = nullptr;
    char bReserved15 = kSLBooleanInvalid;
    uint32_t queueParallelismMode = 0;
    char enableUserInterfaceRecomposition = 0;
    float dynamicTargetFrameRate = 0.0f;
};

struct slDLSSGState : slBaseStructure {
    slDLSSGState() : slBaseStructure(kDLSSGStateStructType, kSLStructVersion4) {}

    uint64_t estimatedVRAMUsageInBytes = 0;
    uint32_t status = 0;
    uint32_t minWidthOrHeight = 0;
    uint32_t numFramesActuallyPresented = 0;
    uint32_t numFramesToGenerateMax = 0;
    char bReserved4 = kSLBooleanInvalid;
    char bIsVsyncSupportAvailable = kSLBooleanInvalid;
    void* inputsProcessingCompletionFence = nullptr;
    uint64_t lastPresentInputsProcessingCompletionFenceValue = 0;
    char bIsDynamicMFGSupported = kSLBooleanInvalid;
};

struct slReflexOptions : slBaseStructure {
    slReflexOptions() : slBaseStructure(kReflexOptionsStructType, kSLStructVersion1) {}

    int32_t mode = kSLReflexOptionsModeOff;
    uint32_t frameLimitUs = 0;
    bool useMarkersToOptimize = false;
    uint16_t virtualKey = 0;
    uint32_t idThread = 0;
};

using PFN_slGetFeatureFunction = slResult (*)(uint32_t feature, const char* functionName, void*& function);
using PFN_slGetPluginFunction = void* (*)(const char* functionName);
using PFN_slSetD3DDevice = slResult (*)(void* d3dDevice);
using PFN_slSetTag = slResult (*)(const slViewportHandle& viewport, const slResourceTag* tags, uint32_t numTags,
                                  void* commandBuffer);
using PFN_slSetTagForFrame = slResult (*)(const slBaseStructure& frame, const slViewportHandle& viewport,
                                          const slResourceTag* tags, uint32_t numTags, void* commandBuffer);
using PFN_slEvaluateFeature = slResult (*)(uint32_t feature, const slBaseStructure& frame,
                                           const slBaseStructure** inputs, uint32_t numInputs, void* commandBuffer);
using PFN_slDLSSGSetOptions = slResult (*)(const slViewportHandle& viewport, const slDLSSGOptions& options);
using PFN_slDLSSGGetState = slResult (*)(const slViewportHandle& viewport, slDLSSGState& state,
                                         const slDLSSGOptions* options);
using PFN_slReflexSleep = slResult (*)(const void* frame);
using PFN_slReflexSetOptions = slResult (*)(const slReflexOptions& options);
using PFN_slReflexSetConstants = slResult (*)(const SLReflexConstants& consts);

struct ViewportFGState {
    bool active = false;
    int multiplier = 0;
    uint32_t generatedFrames = 0;
    uint32_t capabilityMax = 0;
};

std::mutex g_InitMutex;
std::mutex g_StateMutex;
std::mutex g_ModuleHookMutex;
std::mutex g_FeatureHookMutex;
std::mutex g_AcceptedD3D12DeviceMutex;
ID3D12Device* g_AcceptedD3D12Device = nullptr;

std::atomic<bool> g_DynamicHooksRegistered{false};
std::atomic<bool> g_StreamlineUsesD3D12{false};
std::atomic<bool> g_NoModulesLogged{false};
std::atomic<bool> g_ModuleSnapshotFailureLogged{false};
std::atomic<bool> g_ModuleSnapshotRetrySuccessLogged{false};
std::atomic<uint32_t> g_IATPatchesMask{0};
std::atomic<uint32_t> g_InstalledModuleMask{0};

std::atomic<void*> g_SLGetFeatureFunctionTarget{nullptr};
std::atomic<void*> g_SLGetPluginFunctionTarget{nullptr};
std::atomic<void*> g_SLSetD3DDeviceTarget{nullptr};
std::atomic<void*> g_SLSetTagTarget{nullptr};
std::atomic<void*> g_SLSetTagForFrameTarget{nullptr};
std::atomic<void*> g_SLEvaluateFeatureTarget{nullptr};
std::atomic<void*> g_DLSSGSetOptionsTarget{nullptr};
std::atomic<void*> g_DLSSGGetStateTarget{nullptr};
std::atomic<void*> g_ReflexSleepTarget{nullptr};
std::atomic<void*> g_ReflexSetOptionsTarget{nullptr};
std::atomic<void*> g_ReflexSetConstantsTarget{nullptr};
std::atomic<void*> g_DLSSGSetOptionsImportFallbackAttemptedTarget{nullptr};
std::atomic<void*> g_DLSSGGetStateImportFallbackAttemptedTarget{nullptr};
std::atomic<void*> g_ReflexSleepImportFallbackAttemptedTarget{nullptr};
std::atomic<void*> g_ReflexSetOptionsImportFallbackAttemptedTarget{nullptr};
std::atomic<void*> g_ReflexSetConstantsImportFallbackAttemptedTarget{nullptr};

std::atomic<bool> g_SLGetFeatureFunctionHooked{false};
std::atomic<bool> g_SLGetPluginFunctionHooked{false};
std::atomic<bool> g_SLSetD3DDeviceHooked{false};
std::atomic<bool> g_SLSetTagHooked{false};
std::atomic<bool> g_SLSetTagForFrameHooked{false};
std::atomic<bool> g_SLEvaluateFeatureHooked{false};
std::atomic<bool> g_DLSSGSetOptionsHooked{false};
std::atomic<bool> g_DLSSGGetStateHooked{false};
std::atomic<bool> g_ReflexSleepHooked{false};
std::atomic<bool> g_ReflexSetOptionsHooked{false};
std::atomic<bool> g_ReflexSetConstantsHooked{false};
std::atomic<bool> g_DLSSGSetOptionsReturnedWrapperFallbackLogged{false};
std::atomic<bool> g_DLSSGGetStateReturnedWrapperFallbackLogged{false};
std::atomic<bool> g_ReflexSleepReturnedWrapperFallbackLogged{false};
std::atomic<bool> g_ReflexSetOptionsReturnedWrapperFallbackLogged{false};
std::atomic<bool> g_ReflexSetConstantsReturnedWrapperFallbackLogged{false};
std::atomic<bool> g_DLSSGSetOptionsProactiveFallbackLogged{false};
std::atomic<bool> g_DLSSGGetStateProactiveFallbackLogged{false};
std::atomic<bool> g_ReflexSleepProactiveFallbackLogged{false};
std::atomic<bool> g_ReflexSetOptionsProactiveFallbackLogged{false};
std::atomic<bool> g_ReflexSetConstantsProactiveFallbackLogged{false};
std::atomic<bool> g_DLSSGSetOptionsLookupLogged{false};
std::atomic<bool> g_DLSSGGetStateLookupLogged{false};
std::atomic<bool> g_ReflexSleepLookupLogged{false};
std::atomic<bool> g_ReflexSetOptionsLookupLogged{false};
std::atomic<bool> g_ReflexSetConstantsLookupLogged{false};
std::atomic<ULONGLONG> g_ReflexFeatureHookRetryLastMs{0};

std::unordered_map<uint32_t, ViewportFGState> g_ViewportStates;
std::unordered_map<uint32_t, uint32_t> g_ViewportCapabilityMax;

std::atomic<ULONGLONG> g_SuppressNewGetStateActivationUntilMs{0};
constexpr ULONGLONG kAuthoritativeFFXTakeoverGetStateSuppressMs = 250;
std::atomic<bool> g_BlockGetStateOnlyReactivationUntilExplicitSetOptions{false};
std::atomic<bool> g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap{false};
std::atomic<bool> g_CurrentComebackActivatedViaExplicitSetOptions{false};
std::atomic<bool> g_AcceptedRuntimeOffAwaitingSetOptions{false};
std::atomic<bool> g_ConfirmedDLSSReflexSuspendPending{false};
std::atomic<bool> g_StartupWindowOffExtensionPending{false};

std::mutex g_SuppressedOffMutex;
bool g_SuppressedSetOptionsOffDuringStartup = false;
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - trivial value initialization cannot throw
slViewportHandle g_SuppressedOffViewport = {};
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - trivial value initialization cannot throw
slDLSSGOptions g_SuppressedOffOptions = {};
uint32_t g_SuppressedOffViewportKey = 0;

// --- DLSSG activation-health diagnostics (GTA cold-start DLSS FG "active but not interpolating", session
// 20260702_094955: optionsMode=on, updateActive=1, yet presents stayed at base rate with
// numFramesActuallyPresented==1 and no fps gain) --------------------------------------------------------
// sl.dlss_g reports WHY it declines to interpolate in DLSSGState.status (sl_dlss_g.h DLSSGStatus bitflags).
// DLSSG hard-requires Reflex, and GTA's Reflex is historically flaky even without CE (user report), so
// eDLSSGStatusFailReflexNotDetectedAtRuntime is the prime suspect — the health monitor pairs the status
// decode with Reflex call-activity evidence so one run pins the failing precondition.
constexpr uint32_t kDLSSGStatusFailResolutionTooLow = 1u << 0;
constexpr uint32_t kDLSSGStatusFailReflexNotDetectedAtRuntime = 1u << 1;
constexpr uint32_t kDLSSGStatusFailHDRFormatNotSupported = 1u << 2;
constexpr uint32_t kDLSSGStatusFailCommonConstantsInvalid = 1u << 3;
constexpr uint32_t kDLSSGStatusFailGetCurrentBackBufferIndex = 1u << 4;

void FormatDLSSGStatusFlags(uint32_t status, char* buffer, size_t bufferSize) {
    if (!buffer || bufferSize == 0) {
        return;
    }
    if (status == 0) {
        snprintf(buffer, bufferSize, "ok");
        return;
    }
    buffer[0] = '\0';
    size_t used = 0;
    auto append = [&](const char* text) {
        const int written =
            snprintf(buffer + used, bufferSize > used ? bufferSize - used : 0, "%s%s", used ? "|" : "", text);
        if (written > 0) {
            used += static_cast<size_t>(written);
        }
    };
    if (status & kDLSSGStatusFailResolutionTooLow) {
        append("resolutionTooLow");
    }
    if (status & kDLSSGStatusFailReflexNotDetectedAtRuntime) {
        append("REFLEX-NOT-DETECTED");
    }
    if (status & kDLSSGStatusFailHDRFormatNotSupported) {
        append("hdrFormatNotSupported");
    }
    if (status & kDLSSGStatusFailCommonConstantsInvalid) {
        append("commonConstantsInvalid");
    }
    if (status & kDLSSGStatusFailGetCurrentBackBufferIndex) {
        append("getCurrentBackBufferIndexFail");
    }
    const uint32_t knownMask = kDLSSGStatusFailResolutionTooLow | kDLSSGStatusFailReflexNotDetectedAtRuntime |
                               kDLSSGStatusFailHDRFormatNotSupported | kDLSSGStatusFailCommonConstantsInvalid |
                               kDLSSGStatusFailGetCurrentBackBufferIndex;
    if (status & ~knownMask) {
        char unknownText[32];
        snprintf(unknownText, sizeof(unknownText), "unknown(0x%X)", status & ~knownMask);
        append(unknownText);
    }
}

// Reflex call-activity evidence. Written from the Reflex hooks with RELAXED atomics + GetTickCount64 only:
// the manual Reflex FPS limiter's latency-critical sleep path must not gain locks, logging, or syscalls
// (GetTickCount64 is a shared-page memory read). Read from the GetState-side health monitor.
std::atomic<uint64_t> g_ReflexSleepObservedCount{0};
std::atomic<uint64_t> g_ReflexSleepLastTickMs{0};
std::atomic<uint64_t> g_ReflexSetOptionsObservedCount{0};
std::atomic<uint64_t> g_ReflexSetOptionsLastTickMs{0};
std::atomic<int32_t> g_ReflexLastForwardedMode{-1};
// Health-monitor state (GetState thread(s) only; relaxed is fine for diagnostics).
std::atomic<uint64_t> g_DLSSGNotInterpolatingStreak{0};
std::atomic<uint64_t> g_ReflexSleepCountAtLastHealthLog{0};
std::atomic<uint32_t> g_DLSSGLastObservedStatus{0};
// GTA polls slDLSSGGetState roughly per frame, so the first warning lands within a handful of frames of a
// failed activation and repeats sparsely afterwards (deterministic sample counts, not wall-clock).
constexpr uint64_t kDLSSGHealthWarnStreak = 8;
constexpr uint64_t kDLSSGHealthWarnRepeat = 512;

PFN_slGetFeatureFunction g_Original_slGetFeatureFunction = nullptr;
PFN_slGetPluginFunction g_Original_slGetPluginFunction = nullptr;
PFN_slSetD3DDevice g_Original_slSetD3DDevice = nullptr;
PFN_slSetTag g_Original_slSetTag = nullptr;
PFN_slSetTagForFrame g_Original_slSetTagForFrame = nullptr;
PFN_slEvaluateFeature g_Original_slEvaluateFeature = nullptr;
PFN_slDLSSGSetOptions g_Original_slDLSSGSetOptions = nullptr;
PFN_slDLSSGGetState g_Original_slDLSSGGetState = nullptr;
PFN_slReflexSleep g_Original_slReflexSleep = nullptr;
PFN_slReflexSetOptions g_Original_slReflexSetOptions = nullptr;
PFN_slReflexSetConstants g_Original_slReflexSetConstants = nullptr;

slResult Hooked_slGetFeatureFunction(uint32_t feature, const char* functionName, void*& function);
void* Hooked_slGetPluginFunction(const char* functionName);
slResult Hooked_slSetD3DDevice(void* d3dDevice);
slResult Hooked_slSetTag(const slViewportHandle& viewport, const slResourceTag* tags, uint32_t numTags,
                         void* commandBuffer);
slResult Hooked_slSetTagForFrame(const slBaseStructure& frame, const slViewportHandle& viewport,
                                 const slResourceTag* tags, uint32_t numTags, void* commandBuffer);
slResult Hooked_slEvaluateFeature(uint32_t feature, const slBaseStructure& frame, const slBaseStructure** inputs,
                                  uint32_t numInputs, void* commandBuffer);
slResult Hooked_slDLSSGSetOptions(const slViewportHandle& viewport, const slDLSSGOptions& options);
slResult Hooked_slDLSSGGetState(const slViewportHandle& viewport, slDLSSGState& state, const slDLSSGOptions* options);
slResult Hooked_slReflexSleep(const void* frame);
slResult Hooked_slReflexSetOptions(const slReflexOptions& options);
slResult Hooked_slReflexSetConstants(const SLReflexConstants& consts);

const char* GetDLSSGModeName(uint32_t mode) {
    switch (mode) {
        case 0:
            return "off";
        case 1:
            return "on";
        case 2:
            return "auto";
        default:
            return "unknown";
    }
}

const char* GetModuleBaseName(const char* moduleNameOrPath) {
    if (!moduleNameOrPath || !moduleNameOrPath[0]) {
        return nullptr;
    }

    const char* baseName = moduleNameOrPath;
    for (const char* cursor = moduleNameOrPath; *cursor; ++cursor) {
        if (*cursor == '\\' || *cursor == '/') {
            baseName = cursor + 1;
        }
    }
    return baseName;
}

bool IsStreamlineModuleName(const char* moduleNameOrPath) {
    return ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking(moduleNameOrPath);
}

bool ShouldHookStreamlineCoreExports(const char* moduleNameOrPath) {
    return ce::streamline_runtime_policy::ShouldHookStreamlineCoreExportsOnLoad(moduleNameOrPath);
}

bool IsStreamlineCoreDynamicHookModule(const char* moduleBaseName, HMODULE) {
    return ShouldHookStreamlineCoreExports(moduleBaseName);
}

bool IsStreamlineDLSSGDynamicHookModule(const char* moduleBaseName, HMODULE) {
    return ce::streamline_runtime_policy::IsStreamlineDLSSGFeatureModuleName(moduleBaseName);
}

bool IsStreamlineReflexDynamicHookModule(const char* moduleBaseName, HMODULE) {
    return ce::streamline_runtime_policy::IsStreamlineReflexFeatureModuleName(moduleBaseName);
}

uint32_t GetModuleMaskBit(const char* moduleNameOrPath) {
    const char* baseName = GetModuleBaseName(moduleNameOrPath);
    if (!baseName) {
        return 0;
    }
    if (!_stricmp(baseName, "sl.interposer.dll")) {
        return 1u << 0;
    }
    if (!_stricmp(baseName, "sl.common.dll")) {
        return 1u << 1;
    }
    return 0;
}

void LogSkippedStreamlineCoreExportsOnce(const char* moduleBaseName, HMODULE module, bool hasGetFeature,
                                         bool hasGetPlugin, bool hasSetD3DDevice) {
    if (!moduleBaseName || !moduleBaseName[0]) {
        return;
    }

    static std::mutex s_logMutex;
    static std::unordered_map<std::string, bool> s_loggedModules;

    std::string key = moduleBaseName;
    key += '|';
    key += std::to_string(reinterpret_cast<uintptr_t>(module));

    {
        std::lock_guard<std::mutex> lock(s_logMutex);
        if (s_loggedModules.find(key) != s_loggedModules.end()) {
            return;
        }
        s_loggedModules.emplace(key, true);
    }

    HookLogImportant(
        "Streamline Hook: Skipping generic core exports for unloadable feature module %s (%p) "
        "getFeature=%d getPlugin=%d setD3DDevice=%d",
        moduleBaseName, module, hasGetFeature ? 1 : 0, hasGetPlugin ? 1 : 0, hasSetD3DDevice ? 1 : 0);
}

size_t GetModuleImageSizeBytes(HMODULE module) {
    if (!module) {
        return 0;
    }
    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    const auto* ntHeaders =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    return ntHeaders->OptionalHeader.SizeOfImage;
}

bool DoesAddressBelongToLoadedModule(void* address, HMODULE* ownerModule, char* ownerPath, DWORD ownerPathCapacity,
                                     DWORD* outError) {
    if (ownerModule) {
        *ownerModule = nullptr;
    }
    if (ownerPath && ownerPathCapacity > 0) {
        ownerPath[0] = '\0';
    }
    if (outError) {
        *outError = ERROR_SUCCESS;
    }
    if (!address) {
        if (outError) {
            *outError = ERROR_INVALID_ADDRESS;
        }
