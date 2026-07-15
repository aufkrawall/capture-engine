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
slViewportHandle g_SuppressedOffViewport = {};
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
        return false;
    }

    HMODULE module = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &module) ||
        !module) {
        if (outError) {
            *outError = GetLastError();
        }
        return false;
    }

    if (ownerModule) {
        *ownerModule = module;
    }
    if (ownerPath && ownerPathCapacity > 0) {
        const DWORD pathLen = GetModuleFileNameA(module, ownerPath, ownerPathCapacity);
        if (pathLen == 0 || pathLen >= ownerPathCapacity) {
            if (outError) {
                *outError = pathLen >= ownerPathCapacity ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
            }
            ownerPath[0] = '\0';
        }
    }
    return true;
}

void LogStaleStreamlineOriginalBlockedOnce(const char* functionName, void* original, void* validationAddress,
                                           const char* expectedModuleRole, DWORD error) {
    static std::mutex s_logMutex;
    static std::unordered_map<std::string, bool> s_loggedOriginals;

    std::string key = functionName ? functionName : "<unknown>";
    key += '|';
    key += std::to_string(reinterpret_cast<uintptr_t>(original));
    key += '|';
    key += std::to_string(reinterpret_cast<uintptr_t>(validationAddress));

    {
        std::lock_guard<std::mutex> lock(s_logMutex);
        if (s_loggedOriginals.find(key) != s_loggedOriginals.end()) {
            return;
        }
        s_loggedOriginals.emplace(key, true);
    }

    HookLogImportant(
        "Streamline Hook: Blocking stale original forward for %s original=%p validation=%p expected=%s "
        "ownerLoaded=0 error=%lu",
        functionName ? functionName : "<unknown>", original, validationAddress,
        expectedModuleRole ? expectedModuleRole : "loaded Streamline module", static_cast<unsigned long>(error));
}

bool IsSavedStreamlineOriginalCallable(const char* functionName, void* original, void* validationAddress,
                                       const char* expectedModuleRole) {
    const void* addressToValidate = validationAddress ? validationAddress : original;
    DWORD ownerError = ERROR_SUCCESS;
    const bool ownerLoaded =
        DoesAddressBelongToLoadedModule(const_cast<void*>(addressToValidate), nullptr, nullptr, 0, &ownerError);
    if (ce::streamline_runtime_policy::ShouldForwardSavedStreamlineOriginal(original != nullptr, ownerLoaded)) {
        return true;
    }

    if (original) {
        LogStaleStreamlineOriginalBlockedOnce(functionName, original, const_cast<void*>(addressToValidate),
                                              expectedModuleRole, ownerError);
    }
    return false;
}

PFN_slGetFeatureFunction GetCallableOriginalGetFeatureFunction() {
    auto original = g_Original_slGetFeatureFunction;
    return IsSavedStreamlineOriginalCallable("slGetFeatureFunction", reinterpret_cast<void*>(original),
                                             g_SLGetFeatureFunctionTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;
}

PFN_slGetPluginFunction GetCallableOriginalGetPluginFunction() {
    auto original = g_Original_slGetPluginFunction;
    return IsSavedStreamlineOriginalCallable("slGetPluginFunction", reinterpret_cast<void*>(original),
                                             g_SLGetPluginFunctionTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;
}

PFN_slSetD3DDevice GetCallableOriginalSetD3DDevice() {
    auto original = g_Original_slSetD3DDevice;
    return IsSavedStreamlineOriginalCallable("slSetD3DDevice", reinterpret_cast<void*>(original),
                                             g_SLSetD3DDeviceTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;
}

PFN_slSetTag GetCallableOriginalSetTag() {
    auto original = g_Original_slSetTag;
    return IsSavedStreamlineOriginalCallable("slSetTag", reinterpret_cast<void*>(original),
                                             g_SLSetTagTarget.load(std::memory_order_acquire), "core Streamline module")
               ? original
               : nullptr;
}

PFN_slSetTagForFrame GetCallableOriginalSetTagForFrame() {
    auto original = g_Original_slSetTagForFrame;
    return IsSavedStreamlineOriginalCallable("slSetTagForFrame", reinterpret_cast<void*>(original),
                                             g_SLSetTagForFrameTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;
}

PFN_slEvaluateFeature GetCallableOriginalEvaluateFeature() {
    auto original = g_Original_slEvaluateFeature;
    return IsSavedStreamlineOriginalCallable("slEvaluateFeature", reinterpret_cast<void*>(original),
                                             g_SLEvaluateFeatureTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;
}

PFN_slDLSSGSetOptions GetCallableOriginalDLSSGSetOptions() {
    auto original = g_Original_slDLSSGSetOptions;
    return IsSavedStreamlineOriginalCallable("slDLSSGSetOptions", reinterpret_cast<void*>(original),
                                             g_DLSSGSetOptionsTarget.load(std::memory_order_acquire),
                                             "DLSSG feature module")
               ? original
               : nullptr;
}

PFN_slDLSSGGetState GetCallableOriginalDLSSGGetState() {
    auto original = g_Original_slDLSSGGetState;
    return IsSavedStreamlineOriginalCallable("slDLSSGGetState", reinterpret_cast<void*>(original),
                                             g_DLSSGGetStateTarget.load(std::memory_order_acquire),
                                             "DLSSG feature module")
               ? original
               : nullptr;
}

PFN_slReflexSleep GetCallableOriginalReflexSleep() {
    auto original = g_Original_slReflexSleep;
    return IsSavedStreamlineOriginalCallable("slReflexSleep", reinterpret_cast<void*>(original),
                                             g_ReflexSleepTarget.load(std::memory_order_acquire),
                                             "Reflex feature module")
               ? original
               : nullptr;
}

PFN_slReflexSetOptions GetCallableOriginalReflexSetOptions() {
    auto original = g_Original_slReflexSetOptions;
    return IsSavedStreamlineOriginalCallable("slReflexSetOptions", reinterpret_cast<void*>(original),
                                             g_ReflexSetOptionsTarget.load(std::memory_order_acquire),
                                             "Reflex feature module")
               ? original
               : nullptr;
}

PFN_slReflexSetConstants GetCallableOriginalReflexSetConstants() {
    auto original = g_Original_slReflexSetConstants;
    return IsSavedStreamlineOriginalCallable("slReflexSetConstants", reinterpret_cast<void*>(original),
                                             g_ReflexSetConstantsTarget.load(std::memory_order_acquire),
                                             "Reflex feature module")
               ? original
               : nullptr;
}

uint32_t GetViewportKey(const slViewportHandle& viewport) {
    return viewport.value;
}

slDLSSGOptions CloneDLSSGOptions(const slDLSSGOptions& source) {
    slDLSSGOptions copy;
    copy.next = source.next;
    copy.structType = source.structType;
    copy.structVersion = source.structVersion;
    copy.mode = source.mode;
    copy.numFramesToGenerate = source.numFramesToGenerate;
    copy.flags = source.flags;
    copy.dynamicResWidth = source.dynamicResWidth;
    copy.dynamicResHeight = source.dynamicResHeight;
    copy.numBackBuffers = source.numBackBuffers;
    copy.mvecDepthWidth = source.mvecDepthWidth;
    copy.mvecDepthHeight = source.mvecDepthHeight;
    copy.colorWidth = source.colorWidth;
    copy.colorHeight = source.colorHeight;
    copy.colorBufferFormat = source.colorBufferFormat;
    copy.mvecBufferFormat = source.mvecBufferFormat;
    copy.depthBufferFormat = source.depthBufferFormat;
    copy.hudLessBufferFormat = source.hudLessBufferFormat;
    copy.uiBufferFormat = source.uiBufferFormat;
    copy.onErrorCallback = source.onErrorCallback;
    if (source.structVersion >= kSLStructVersion2) {
        copy.bReserved15 = source.bReserved15;
    }
    if (source.structVersion >= kSLStructVersion3) {
        copy.queueParallelismMode = source.queueParallelismMode;
    }
    if (source.structVersion >= kSLStructVersion4) {
        copy.enableUserInterfaceRecomposition = source.enableUserInterfaceRecomposition;
    }
    if (source.structVersion >= kSLStructVersion5) {
        copy.dynamicTargetFrameRate = source.dynamicTargetFrameRate;
    }
    if (source.structVersion > kSLStructVersion5) {
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true)) {
            HookLogImportant("SL: DLSSG options structVersion=%zu exceeds CE's max (5); forwarding v5 prefix only",
                             source.structVersion);
        }
        copy.structVersion = kSLStructVersion5;
    }
    return copy;
}

int GetEffectiveMultiplier(const slDLSSGOptions& options) {
    return ce::streamline_runtime_policy::ResolveDLSSFGMultiplier(
        ce::streamline_runtime_policy::IsDLSSGModeEnabled(options.mode), options.numFramesToGenerate);
}

struct DLSSGSetOptionsLogState {
    bool valid = false;
    bool requestedEnabled = false;
    bool forwarded = false;
    uint32_t requestMode = 0;
    uint32_t forwardedMode = 0;
    uint32_t requestedGeneratedFrames = 0;
    uint32_t forwardedGeneratedFrames = 0;
    uint32_t capabilityMax = 0;
    slResult result = kSlResultOk;
    bool overrideApplied = false;
    bool overrideClamped = false;
    bool startupWindowActive = false;
    bool hadFSRFGPhase = false;
    bool explicitSetOptionsActivationForCurrentComeback = false;
    bool safePostFSRBootstrapPath = false;
    bool startupActivationPending = false;
    bool postSLActiveButUnconfirmed = false;
    bool postSLConfirmedRendering = false;
    bool postSLConfirmedButStartupSettling = false;
    bool postSLConfirmedButRuntimeStateStabilizing = false;
    bool streamlineFGSignalActive = false;
    bool pureObserverOnly = false;
    ce::fg_runtime::RuntimeMode runtimeMode = ce::fg_runtime::RuntimeMode::kUnknown;
};

void LogDLSSGSetOptionsTransition(uint32_t viewportKey, const slDLSSGOptions& requestedOptions,
                                  const slDLSSGOptions& forwardedOptions, uint32_t requestedGeneratedFrames,
                                  uint32_t capabilityMax, bool requestedEnabled, bool setOptionsCallSuppressed,
                                  bool overrideApplied, bool overrideClamped, slResult result, bool pureObserverOnly,
                                  bool startupWindowActive, bool hadFSRFGPhase,
                                  bool explicitSetOptionsActivationForCurrentComeback, bool safePostFSRBootstrapPath,
                                  bool startupActivationPending, bool postSLActiveButUnconfirmed,
                                  bool postSLConfirmedRendering, bool postSLConfirmedButStartupSettling,
                                  bool postSLConfirmedButRuntimeStateStabilizing) {
    const bool forwarded = !setOptionsCallSuppressed;
    const bool streamlineFGSignalActive = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();

    static std::mutex s_setOptionsLogMutex;
    static std::unordered_map<uint32_t, DLSSGSetOptionsLogState> s_setOptionsLogState;

    bool shouldLog = false;
    {
        std::lock_guard<std::mutex> lock(s_setOptionsLogMutex);
        auto& last = s_setOptionsLogState[viewportKey];
        shouldLog =
            !last.valid || last.requestedEnabled != requestedEnabled || last.forwarded != forwarded ||
            last.requestMode != requestedOptions.mode || last.forwardedMode != forwardedOptions.mode ||
            last.requestedGeneratedFrames != requestedGeneratedFrames ||
            last.forwardedGeneratedFrames != forwardedOptions.numFramesToGenerate ||
            last.capabilityMax != capabilityMax || last.result != result || last.overrideApplied != overrideApplied ||
            last.overrideClamped != overrideClamped || last.startupWindowActive != startupWindowActive ||
            last.hadFSRFGPhase != hadFSRFGPhase ||
            last.explicitSetOptionsActivationForCurrentComeback != explicitSetOptionsActivationForCurrentComeback ||
            last.safePostFSRBootstrapPath != safePostFSRBootstrapPath ||
            last.startupActivationPending != startupActivationPending ||
            last.postSLActiveButUnconfirmed != postSLActiveButUnconfirmed ||
            last.postSLConfirmedRendering != postSLConfirmedRendering ||
            last.postSLConfirmedButStartupSettling != postSLConfirmedButStartupSettling ||
            last.postSLConfirmedButRuntimeStateStabilizing != postSLConfirmedButRuntimeStateStabilizing ||
            last.streamlineFGSignalActive != streamlineFGSignalActive || last.pureObserverOnly != pureObserverOnly ||
            last.runtimeMode != runtimeMode;
        if (shouldLog) {
            last.valid = true;
            last.requestedEnabled = requestedEnabled;
            last.forwarded = forwarded;
            last.requestMode = requestedOptions.mode;
            last.forwardedMode = forwardedOptions.mode;
            last.requestedGeneratedFrames = requestedGeneratedFrames;
            last.forwardedGeneratedFrames = forwardedOptions.numFramesToGenerate;
            last.capabilityMax = capabilityMax;
            last.result = result;
            last.overrideApplied = overrideApplied;
            last.overrideClamped = overrideClamped;
            last.startupWindowActive = startupWindowActive;
            last.hadFSRFGPhase = hadFSRFGPhase;
            last.explicitSetOptionsActivationForCurrentComeback = explicitSetOptionsActivationForCurrentComeback;
            last.safePostFSRBootstrapPath = safePostFSRBootstrapPath;
            last.startupActivationPending = startupActivationPending;
            last.postSLActiveButUnconfirmed = postSLActiveButUnconfirmed;
            last.postSLConfirmedRendering = postSLConfirmedRendering;
            last.postSLConfirmedButStartupSettling = postSLConfirmedButStartupSettling;
            last.postSLConfirmedButRuntimeStateStabilizing = postSLConfirmedButRuntimeStateStabilizing;
            last.streamlineFGSignalActive = streamlineFGSignalActive;
            last.pureObserverOnly = pureObserverOnly;
            last.runtimeMode = runtimeMode;
        }
    }

    if (shouldLog) {
        HookLogImportant(
            "Streamline Hook: slDLSSGSetOptions %s viewport=%u requested=%s(%u) forwardedMode=%s(%u) "
            "generated=%u->%u capabilityMax=%u result=%d override=%d clamped=%d startupWindow=%d hadFSR=%d "
            "explicitComeback=%d safeBootstrap=%d pending=%d unconfirmed=%d confirmed=%d settling=%d "
            "stabilizing=%d runtime=%s slSignal=%d observerOnly=%d",
            forwarded ? "forwarded" : "suppressed", viewportKey, GetDLSSGModeName(requestedOptions.mode),
            requestedOptions.mode, GetDLSSGModeName(forwardedOptions.mode), forwardedOptions.mode,
            requestedGeneratedFrames, forwardedOptions.numFramesToGenerate, capabilityMax, result,
            overrideApplied ? 1 : 0, overrideClamped ? 1 : 0, startupWindowActive ? 1 : 0, hadFSRFGPhase ? 1 : 0,
            explicitSetOptionsActivationForCurrentComeback ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0,
            startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
            postSLConfirmedButStartupSettling ? 1 : 0, postSLConfirmedButRuntimeStateStabilizing ? 1 : 0,
            ce::fg_runtime::GetRuntimeModeName(runtimeMode), streamlineFGSignalActive ? 1 : 0,
            pureObserverOnly ? 1 : 0);
    }
}

struct ReflexSignalLogState {
    bool valid = false;
    int32_t mode = 0;
    uint32_t incomingFrameLimitUs = 0;
    uint32_t forwardedFrameLimitUs = 0;
    uint32_t targetIntervalUs = 0;
    bool frameLimitOverrideApplied = false;
    bool pacingSignalActive = false;
    bool runtimeDLSSFGApiActive = false;
    bool runtimeFSRFGApiActive = false;
    bool streamlineFGSignalActive = false;
    ce::fg_runtime::RuntimeMode runtimeMode = ce::fg_runtime::RuntimeMode::kUnknown;
};

void LogStreamlineReflexSignalChange(const char* sourceName, int32_t mode, uint32_t incomingFrameLimitUs,
                                     uint32_t forwardedFrameLimitUs, uint32_t targetIntervalUs) {
    const bool pacingSignalActive =
        ce::streamline_runtime_policy::IsStreamlineReflexPacingSignalActive(mode, incomingFrameLimitUs);
    const bool lowLatencyModeEnabled = ce::streamline_runtime_policy::IsStreamlineReflexLowLatencyModeEnabled(mode);
    const bool frameLimitActive =
        ce::streamline_runtime_policy::IsStreamlineReflexFrameLimitActive(incomingFrameLimitUs);
    const bool forwardedFrameLimitActive =
        ce::streamline_runtime_policy::IsStreamlineReflexFrameLimitActive(forwardedFrameLimitUs);
    const bool frameLimitOverrideApplied = incomingFrameLimitUs != forwardedFrameLimitUs;
    const bool runtimeDLSSFGApiActive = g_FGCompat.IsDLSSFGApiActive();
    const bool runtimeFSRFGApiActive = g_FGCompat.IsFSRFGApiActive();
    const bool streamlineFGSignalActive = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();

    static std::mutex s_reflexSignalLogMutex;
    static ReflexSignalLogState s_optionsState;
    static ReflexSignalLogState s_constantsState;
    const bool isOptionsSource = sourceName && strcmp(sourceName, "slReflexSetOptions") == 0;

    bool shouldLog = false;
    {
        std::lock_guard<std::mutex> lock(s_reflexSignalLogMutex);
        auto& last = isOptionsSource ? s_optionsState : s_constantsState;
        shouldLog = !last.valid || last.mode != mode || last.incomingFrameLimitUs != incomingFrameLimitUs ||
                    last.forwardedFrameLimitUs != forwardedFrameLimitUs || last.targetIntervalUs != targetIntervalUs ||
                    last.frameLimitOverrideApplied != frameLimitOverrideApplied ||
                    last.pacingSignalActive != pacingSignalActive ||
                    last.runtimeDLSSFGApiActive != runtimeDLSSFGApiActive ||
                    last.runtimeFSRFGApiActive != runtimeFSRFGApiActive ||
                    last.streamlineFGSignalActive != streamlineFGSignalActive || last.runtimeMode != runtimeMode;
        if (shouldLog) {
            last.valid = true;
            last.mode = mode;
            last.incomingFrameLimitUs = incomingFrameLimitUs;
            last.forwardedFrameLimitUs = forwardedFrameLimitUs;
            last.targetIntervalUs = targetIntervalUs;
            last.frameLimitOverrideApplied = frameLimitOverrideApplied;
            last.pacingSignalActive = pacingSignalActive;
            last.runtimeDLSSFGApiActive = runtimeDLSSFGApiActive;
            last.runtimeFSRFGApiActive = runtimeFSRFGApiActive;
            last.streamlineFGSignalActive = streamlineFGSignalActive;
            last.runtimeMode = runtimeMode;
        }
    }

    if (shouldLog) {
        HookLogImportant(
            "Streamline Hook: Reflex signal via %s mode=%d lowLatency=%d frameLimitUs=%u frameLimitActive=%d "
            "forwardedFrameLimitUs=%u forwardedFrameLimitActive=%d override=%d pacingActive=%d ceCapActive=%d "
            "ceTargetIntervalUs=%u runtime=%s dlssApi=%d fsrApi=%d slSignal=%d",
            sourceName ? sourceName : "unknown", mode, lowLatencyModeEnabled ? 1 : 0, incomingFrameLimitUs,
            frameLimitActive ? 1 : 0, forwardedFrameLimitUs, forwardedFrameLimitActive ? 1 : 0,
            frameLimitOverrideApplied ? 1 : 0, pacingSignalActive ? 1 : 0, targetIntervalUs > 0 ? 1 : 0,
            targetIntervalUs, ce::fg_runtime::GetRuntimeModeName(runtimeMode), runtimeDLSSFGApiActive ? 1 : 0,
            runtimeFSRFGApiActive ? 1 : 0, streamlineFGSignalActive ? 1 : 0);
    }
}

void MaybePrepareForStreamlineEnableTransitionFromReflex(const char* sourceName) {
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool runtimeModeIsFSRFG = runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG;
    const bool runtimeOwnsSwapchain = DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();
    if (ce::streamline_runtime_policy::ShouldRequestStreamlineEnablePreparationOnReflexActivation(
            true, g_FGCompat.IsFSRFGApiActive(), runtimeModeIsFSRFG, runtimeOwnsSwapchain)) {
        HookLogImportant(
            "Streamline Hook: Reflex activation requesting Streamline enable preparation via %s "
            "(runtime=%s apiFSR=%d fgOwned=%d)",
            sourceName ? sourceName : "unknown", ce::fg_runtime::GetRuntimeModeName(runtimeMode),
            g_FGCompat.IsFSRFGApiActive() ? 1 : 0, runtimeOwnsSwapchain ? 1 : 0);
        DX12_PrepareForStreamlineEnableTransition();
    }
}

void HandleStreamlineReflexPacingSignal(const char* sourceName, int32_t mode, uint32_t incomingFrameLimitUs,
                                        uint32_t forwardedFrameLimitUs, uint32_t targetIntervalUs) {
    const bool lowLatencyModeEnabled = ce::streamline_runtime_policy::IsStreamlineReflexLowLatencyModeEnabled(mode);
    const bool frameLimitActive =
        ce::streamline_runtime_policy::IsStreamlineReflexFrameLimitActive(incomingFrameLimitUs);
    const bool pacingSignalActive =
        ce::streamline_runtime_policy::IsStreamlineReflexPacingSignalActive(mode, incomingFrameLimitUs);

    LogStreamlineReflexSignalChange(sourceName, mode, incomingFrameLimitUs, forwardedFrameLimitUs, targetIntervalUs);

    if (pacingSignalActive) {
        const bool activationEdge = !g_ReflexLimiter.IsGameActivated();
        const bool clearedSuspendIntent =
            g_ConfirmedDLSSReflexSuspendPending.exchange(false, std::memory_order_acq_rel);
        if (activationEdge) {
            HookLogImportant(
                "Streamline Hook: Game ACTIVATED Reflex pacing via %s (mode=%d lowLatency=%d frameLimitUs=%u "
                "frameLimitActive=%d)",
                sourceName ? sourceName : "unknown", mode, lowLatencyModeEnabled ? 1 : 0, incomingFrameLimitUs,
                frameLimitActive ? 1 : 0);
            if (lowLatencyModeEnabled) {
                MaybePrepareForStreamlineEnableTransitionFromReflex(sourceName);
            }
        }
        if (clearedSuspendIntent) {
            HookLogImportant(
                "Streamline Hook: Cleared confirmed Reflex suspend intent on pacing reactivation via %s",
                sourceName ? sourceName : "unknown");
        }
        g_ReflexLimiter.SetGameActivated(true);
        g_ReflexLimiter.MarkNativePacingSignal();
    } else {
        const bool deactivationEdge = g_ReflexLimiter.IsGameActivated();
        if (deactivationEdge) {
            HookLogImportant("Streamline Hook: Game DEACTIVATED Reflex pacing via %s (mode=%d frameLimitUs=%u)",
                             sourceName ? sourceName : "unknown", mode, incomingFrameLimitUs);
        }
        if (ce::streamline_runtime_policy::ShouldArmConfirmedDLSSReflexSuspendIntent(
                deactivationEdge, g_FGCompat.IsDLSSFGApiActive(),
                DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                HookIsPostSLOverlayConfirmedRendering(),
                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire),
                HookIsPostSLOverlayActiveButUnconfirmed(), HookIsPostSLOverlayConfirmedButStartupSettling(),
                HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing() ||
                    HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected())) {
            const bool wasPending = g_ConfirmedDLSSReflexSuspendPending.exchange(true, std::memory_order_acq_rel);
            ResetStartupProtectedOffChurnActiveProof("stable confirmed Reflex suspend intent");
            if (!wasPending) {
                HookLogImportant(
                    "Streamline Hook: Stable confirmed DLSS-G epoch observed Reflex OFF via %s — next inactive "
                    "GetState/SetOptions edge is authoritative (manual limiter target remains unchanged)",
                    sourceName ? sourceName : "unknown");
            }
        }
        g_ReflexLimiter.SetGameActivated(false);
    }
}

uint32_t GetCachedCapabilityMax(uint32_t viewportKey) {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    const auto it = g_ViewportCapabilityMax.find(viewportKey);
    return it != g_ViewportCapabilityMax.end() ? it->second : 0u;
}

void CacheCapabilityMax(uint32_t viewportKey, uint32_t capabilityMax) {
    if (capabilityMax == 0) {
        return;
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        auto& cached = g_ViewportCapabilityMax[viewportKey];
        if (cached != capabilityMax) {
            cached = capabilityMax;
            changed = true;
        }
    }

    if (changed && HookDebugLoggingEnabled()) {
        HookLog("Streamline Hook: Viewport %u reports max generated frames=%u (%dx max)", viewportKey, capabilityMax,
                capabilityMax + 1);
    }
}

void ApplyCombinedDLSSFGState(bool active, int multiplier) {
    if (active) {
        const int effectiveMultiplier = std::clamp(multiplier, 2, 4);
        g_FGCompat.SetDLSSFGMultiplier(effectiveMultiplier);
        g_FGCompat.SetDLSSFGActive(true);

        if (g_IPC && g_IPC->GetSharedMem()) {
            g_IPC->GetSharedMem()->dlssState.fgActive = true;
            g_IPC->GetSharedMem()->dlssState.mfgMultiplier = effectiveMultiplier;
        }
    } else {
        g_FGCompat.SetDLSSFGActive(false);
        g_FGCompat.SetDLSSFGMultiplier(0);

        if (g_IPC && g_IPC->GetSharedMem()) {
            g_IPC->GetSharedMem()->dlssState.fgActive = false;
            g_IPC->GetSharedMem()->dlssState.mfgMultiplier = 0;
        }
    }
}

void ApplyCombinedStreamlineRuntimeState(bool active, int multiplier, bool explicitSetOptionsEnableSignal,
                                         const char* source) {
    if (ShouldKeepPureObserverOnlyStreamlineBehavior()) {
        const bool previousSignalObserved =
            DXGIShared::g_StreamlineFGRunning.exchange(active, std::memory_order_acq_rel);
        g_FGCompat.SetStreamlineFGSignal(active);
        ApplyCombinedDLSSFGState(active, active ? std::clamp(multiplier, 2, 4) : 0);
        if (previousSignalObserved != active) {
            DX12_OnStreamlineFGStateChanged(active);
            HookLogImportant("Streamline Hook: FG state transition %s->%s via %s (observer-only pass-through)",
                             previousSignalObserved ? "ON" : "OFF", active ? "ON" : "OFF",
                             source ? source : "runtime-state");
        }
        return;
    }

    const bool startupWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
    const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
    const bool startupActivationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
    const bool postSLConfirmedButStartupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
    const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
    const bool sourceWasSetOptions = source && strcmp(source, "SetOptions") == 0;
    const bool sourceWasGetState = source && strcmp(source, "GetState") == 0;
    const bool postSLConfirmedButRuntimeStateStabilizingBase = HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing();
    const bool postSLConfirmedButStaleOffWarmupProtected =
        !active && HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected();
    const bool postSLConfirmedButRuntimeStateStabilizing =
        postSLConfirmedButRuntimeStateStabilizingBase || postSLConfirmedButStaleOffWarmupProtected;
    const bool explicitSetOptionsActivationForCurrentComeback =
        g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
    const bool hadFSRFGPhase = HookHasFSRFGHistory();
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    const bool startupProtectedComebackProof =
        explicitSetOptionsActivationForCurrentComeback || safePostFSRBootstrapPath;
    const bool postSLConfirmedButOffChurnAwaitingActiveProof = IsStartupProtectedOffChurnAwaitingActiveProof(
        startupProtectedComebackProof, postSLConfirmedRendering, postSLConfirmedButStartupSettling);
    const bool acceptActivatedUnconfirmedResumeOff =
        ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
            !active, startupWindowActive, startupProtectedComebackProof, startupActivationPending,
            postSLActiveButUnconfirmed, postSLStartupActivationEntered, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling,
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof);
    const bool explicitSetOptionsDisableIsAuthoritative =
        ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
            !active, sourceWasSetOptions, postSLConfirmedRendering, startupActivationPending,
            postSLActiveButUnconfirmed, postSLConfirmedButStartupSettling,
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof,
            g_AcceptedRuntimeOffAwaitingSetOptions.load(std::memory_order_acquire));
    const bool previousSignal = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool confirmedReflexSuspendIsAuthoritative =
        ce::streamline_runtime_policy::ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend(
            g_ConfirmedDLSSReflexSuspendPending.load(std::memory_order_acquire), !active, previousSignal);
    const bool deferOffSignal =
        !active && !explicitSetOptionsDisableIsAuthoritative && !acceptActivatedUnconfirmedResumeOff &&
        !confirmedReflexSuspendIsAuthoritative &&
        ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
            startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
            safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling,
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof);
    const auto signalUpdate = ce::streamline_runtime_policy::ResolveCombinedRuntimeSignalUpdate(
        active, deferOffSignal, previousSignal, multiplier);
    const bool previousExplicitSetOptionsActivation =
        g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);

    const bool previousSignalObserved =
        DXGIShared::g_StreamlineFGRunning.exchange(signalUpdate.effectiveActive, std::memory_order_acq_rel);
    if (ce::streamline_runtime_policy::ShouldArmStartupTransitionWindowOnFreshActiveSignal(active, previousSignal)) {
        DXGIShared::ArmStreamlineStartupTransitionWindow();
        g_StartupWindowOffExtensionPending.store(true, std::memory_order_release);
    }
    const bool explicitSetOptionsActivation = explicitSetOptionsEnableSignal;
    const bool updatedExplicitSetOptionsActivation =
        ce::streamline_runtime_policy::ResolveCurrentComebackExplicitSetOptionsActivation(
            previousExplicitSetOptionsActivation, signalUpdate.effectiveActive, signalUpdate.freshActivationEdge,
            explicitSetOptionsActivation);
    g_CurrentComebackActivatedViaExplicitSetOptions.store(updatedExplicitSetOptionsActivation,
                                                          std::memory_order_release);
    const bool acceptedRuntimeOffAwaitingSetOptions =
        ce::streamline_runtime_policy::ShouldLatchAcceptedRuntimeOffAwaitingSetOptions(
            previousSignalObserved, signalUpdate.effectiveActive, sourceWasGetState);
    if (acceptedRuntimeOffAwaitingSetOptions) {
        g_AcceptedRuntimeOffAwaitingSetOptions.store(true, std::memory_order_release);
        const bool wasBlockingGetStateOnlyReactivation =
            g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.exchange(true, std::memory_order_acq_rel);
        HookLogImportant(
            "Streamline Hook: Accepted runtime OFF via %s before matching SetOptions — forwarding the next "
            "SetOptions(OFF) despite stale PostSL startup proof and blocking GetState-only reactivation "
            "(previousBlock=%d)",
            source ? source : "runtime-state", wasBlockingGetStateOnlyReactivation ? 1 : 0);
    }
    g_FGCompat.SetStreamlineFGSignal(signalUpdate.effectiveActive);
    ApplyCombinedDLSSFGState(signalUpdate.effectiveActive, signalUpdate.effectiveMultiplier);
    if (confirmedReflexSuspendIsAuthoritative && !signalUpdate.deferredOffDuringStartupWindow) {
        const bool consumedSuspendIntent =
            g_ConfirmedDLSSReflexSuspendPending.exchange(false, std::memory_order_acq_rel);
        ResetStartupProtectedOffChurnActiveProof("accepted confirmed Reflex suspend runtime OFF");
        if (consumedSuspendIntent) {
            HookLogImportant(
                "Streamline Hook: Accepted %s OFF as authoritative after stable confirmed Reflex suspend — "
                "startup churn protection remains armed for future cold starts",
                source ? source : "runtime-state");
        }
    } else if (acceptActivatedUnconfirmedResumeOff) {
        LogAcceptedOffDuringActivatedUnconfirmedResume(
            source, startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
            safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
            postSLStartupActivationEntered, postSLConfirmedRendering, postSLConfirmedButStartupSettling,
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof);
        ResetStartupProtectedOffChurnActiveProof("accepted activated-unconfirmed startup suspend");
    } else if (explicitSetOptionsDisableIsAuthoritative) {
        ResetStartupProtectedOffChurnActiveProof("accepted authoritative SetOptions disable");
    } else if (!active && signalUpdate.deferredOffDuringStartupWindow) {
        MarkStartupProtectedOffChurnObserved(
            source, postSLConfirmedRendering, postSLConfirmedButStartupSettling,
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof);
    } else if (active) {
        MarkStartupProtectedActiveRuntimeProof(source, signalUpdate.effectiveMultiplier);
    } else if (!signalUpdate.effectiveActive) {
        ResetStartupProtectedOffChurnActiveProof("accepted inactive Streamline runtime signal");
    }

    if (!previousExplicitSetOptionsActivation && updatedExplicitSetOptionsActivation && signalUpdate.effectiveActive &&
        explicitSetOptionsActivation && !signalUpdate.freshActivationEdge) {
        HookLogImportant(
            "Streamline Hook: Upgraded already-live DLSS comeback provenance to explicit SetOptions enable "
            "(source=%s startupWindow=%d hadFSR=%d safeBootstrap=%d pending=%d unconfirmed=%d settling=%d "
            "stabilizing=%d)",
            source ? source : "runtime-state", startupWindowActive ? 1 : 0, HookHasFSRFGHistory() ? 1 : 0,
            safePostFSRBootstrapPath ? 1 : 0, startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
            postSLConfirmedButStartupSettling ? 1 : 0, postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
    }

    if (previousSignalObserved != signalUpdate.effectiveActive) {
        if (signalUpdate.effectiveActive) {
            ce::dx12_streamline_ui_overlay::BeginActivation(
                static_cast<uint32_t>(std::clamp(signalUpdate.effectiveMultiplier, 1, 6)));
        } else {
            ce::dx12_streamline_ui_overlay::EndActivation("accepted Streamline FG OFF transition");
        }
        DX12_OnStreamlineFGStateChanged(signalUpdate.effectiveActive);
        HookLogImportant("Streamline Hook: FG state transition %s->%s via %s", previousSignalObserved ? "ON" : "OFF",
                         signalUpdate.effectiveActive ? "ON" : "OFF", source ? source : "runtime-state");
    }
    ce::fg_session::EmitFGEvent(sourceWasSetOptions ? ce::fg_session::FGEventKind::kStreamlineSetOptionsRuntimeUpdate
                                                    : ce::fg_session::FGEventKind::kStreamlineGetStateRuntimeUpdate,
                                source ? source : "StreamlineRuntimeState", nullptr, nullptr,
                                signalUpdate.effectiveActive ? ce::fg_runtime::RuntimeMode::kDLSSFG
                                                             : ce::fg_runtime::RuntimeMode::kStreamlineNoFG,
                                signalUpdate.effectiveActive, updatedExplicitSetOptionsActivation);
    if (signalUpdate.deferredOffDuringStartupWindow && !startupWindowActive) {
        if (!active && !postSLConfirmedButStartupSettling && sourceWasGetState &&
            postSLConfirmedButStaleOffWarmupProtected && !postSLConfirmedButRuntimeStateStabilizingBase) {
            static std::atomic<bool> s_loggedGetStateWarmupProofSuppression{false};
            if (!s_loggedGetStateWarmupProofSuppression.exchange(true, std::memory_order_relaxed)) {
                HookLogImportant(
                    "Streamline Hook: Suppressing post-stabilization GetState OFF during PostSL warmup proof "
                    "(hadFSR=%d explicit=%d safeBootstrap=%d stableProtectionWindow=%d-%d)",
                    hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
                    safePostFSRBootstrapPath ? 1 : 0,
                    ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationFirstFrame(),
                    HookGetPostSLStaleOffWarmupProtectionLastFrame());
            }
        } else if (!active && !postSLConfirmedButStartupSettling && sourceWasGetState &&
                   postSLConfirmedButOffChurnAwaitingActiveProof) {
            static std::atomic<bool> s_loggedGetStateActiveProofSuppression{false};
            if (!s_loggedGetStateActiveProofSuppression.exchange(true, std::memory_order_relaxed)) {
                HookLogImportant(
                    "Streamline Hook: Suppressing GetState OFF until startup OFF churn receives active proof "
                    "(hadFSR=%d explicit=%d safeBootstrap=%d activeProof=%u/%u)",
                    hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
                    safePostFSRBootstrapPath ? 1 : 0,
                    g_StartupProtectedOffChurnActiveProofCount.load(std::memory_order_acquire),
                    ce::streamline_runtime_policy::GetStartupProtectedOffChurnActiveProofUpdateThreshold());
            }
        } else if (!active && !postSLConfirmedButStartupSettling && postSLConfirmedButRuntimeStateStabilizingBase &&
                   sourceWasGetState) {
            static std::atomic<bool> s_loggedPostSettlingGetStateSuppression{false};
            if (!s_loggedPostSettlingGetStateSuppression.exchange(true, std::memory_order_relaxed)) {
                const int runtimeStateStabilizationLastFrame = HookGetPostSLRuntimeStateStabilizationLastFrame();
                HookLogImportant(
                    "Streamline Hook: Suppressing first post-settling GetState OFF during runtime-state stabilization "
                    "(hadFSR=%d explicit=%d safeBootstrap=%d stableProtectionWindow=%d-%d)",
                    hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
                    safePostFSRBootstrapPath ? 1 : 0,
                    ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationFirstFrame(),
                    runtimeStateStabilizationLastFrame);
            }
        }
        static std::atomic<int> s_halfArmedDeferredOffLogCount{0};
        const int logCount = s_halfArmedDeferredOffLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Keeping OFF churn deferred after startup window expiry because Streamline DLSS "
                "startup is still startup-protected (hadFSR=%d explicit=%d safeBootstrap=%d pending=%d "
                "unconfirmed=%d startupActivationEntered=%d confirmed=%d settling=%d stabilizing=%d "
                "activeProofPending=%d source=%s)",
                hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
                safePostFSRBootstrapPath ? 1 : 0, startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                postSLStartupActivationEntered ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                postSLConfirmedButStartupSettling ? 1 : 0,
                (postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof) ? 1 : 0,
                postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0, source ? source : "runtime-state");
        }
    }
    if (signalUpdate.deferredOffDuringStartupWindow && signalUpdate.shouldExtendStartupTransitionWindow) {
        const bool shouldExtend = g_StartupWindowOffExtensionPending.exchange(false, std::memory_order_acq_rel);
        if (!shouldExtend) {
            static std::atomic<int> s_startupWindowOffExtensionAlreadyConsumedLogCount{0};
            const int logCount =
                s_startupWindowOffExtensionAlreadyConsumedLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10 || (logCount % 100) == 0) {
                HookLogImportant(
                    "Streamline Hook: Deferring OFF signal during startup transition window "
                    "(g_StreamlineFGRunning stays ON, multiplier=%d source=%s) — extension already consumed for "
                    "current churn burst",
                    signalUpdate.effectiveMultiplier, source ? source : "unknown");
            }
            return;
        }
        DXGIShared::ExtendStreamlineStartupTransitionWindow();
        HookLogImportant(
            "Streamline Hook: Deferring OFF signal during startup transition window "
            "(g_StreamlineFGRunning stays ON, multiplier=%d source=%s) — extended startup window",
            signalUpdate.effectiveMultiplier, source ? source : "unknown");
    } else if (ce::streamline_runtime_policy::ShouldPrimeStartupWindowOffExtensionLatch(
                   signalUpdate.effectiveActive, signalUpdate.freshActivationEdge)) {
        g_StartupWindowOffExtensionPending.store(true, std::memory_order_release);
    }
}

bool WasViewportRuntimeStateActive(uint32_t viewportKey) {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    const auto it = g_ViewportStates.find(viewportKey);
    return it != g_ViewportStates.end() && it->second.active;
}

bool ShouldSuppressNewGetStateActivation() {
    if (ShouldKeepPureObserverOnlyStreamlineBehavior()) {
        return false;
    }

    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    if (ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationDuringUnsafePostFSRComeback(
            g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap.load(std::memory_order_acquire),
            safePostFSRBootstrapPath, runtimeMode)) {
        return true;
    }

    if (ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationWhileRuntimeInactive(
            g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.load(std::memory_order_acquire),
            DXGIShared::IsStreamlineStartupTransitionWindowActive(), runtimeMode)) {
        return true;
    }

    const ULONGLONG suppressUntilMs = g_SuppressNewGetStateActivationUntilMs.load(std::memory_order_acquire);
    return suppressUntilMs != 0 && GetTickCount64() < suppressUntilMs;
}

bool HasDLSSGRuntimeFenceEvidence(const slDLSSGState& state) {
    return state.inputsProcessingCompletionFence != nullptr ||
           state.lastPresentInputsProcessingCompletionFenceValue != 0;
}

void UpdateViewportRuntimeState(uint32_t viewportKey, bool active, int multiplier, uint32_t generatedFrames,
                                uint32_t capabilityMax, const char* source,
                                bool clearAllViewportStatesForDisable = false) {
    ViewportFGState previousState{};
    bool hadPreviousState = false;
    bool stateChanged = false;
    bool anyActive = false;
    int combinedMultiplier = 0;
    size_t clearedActiveViewportCount = 0;

    {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        const auto existing = g_ViewportStates.find(viewportKey);
        if (existing != g_ViewportStates.end()) {
            previousState = existing->second;
            hadPreviousState = true;
        }

        if (active) {
            g_ViewportStates[viewportKey] = {true, multiplier, generatedFrames, capabilityMax};
        } else if (clearAllViewportStatesForDisable) {
            clearedActiveViewportCount = g_ViewportStates.size();
            g_ViewportStates.clear();
        } else {
            g_ViewportStates.erase(viewportKey);
        }

        const auto current = g_ViewportStates.find(viewportKey);
        const ViewportFGState currentState =
            current != g_ViewportStates.end() ? current->second : ViewportFGState{false, 0, 0, capabilityMax};

        stateChanged = !hadPreviousState || previousState.active != currentState.active ||
                       previousState.multiplier != currentState.multiplier ||
                       previousState.generatedFrames != currentState.generatedFrames ||
                       previousState.capabilityMax != currentState.capabilityMax;

        for (const auto& [_, state] : g_ViewportStates) {
            if (!state.active) {
                continue;
            }
            anyActive = true;
            combinedMultiplier = std::max(combinedMultiplier, state.multiplier);
        }
    }

    const bool explicitSetOptionsEnableSignal = source && strcmp(source, "SetOptions") == 0 && active;
    ApplyCombinedStreamlineRuntimeState(anyActive, combinedMultiplier, explicitSetOptionsEnableSignal, source);

    if (clearedActiveViewportCount > 0) {
        HookLogImportant(
            "Streamline Hook: Cleared %zu cached DLSSG viewport runtime state(s) after %s disable "
            "(triggerViewport=%u generatedFrames=%u capabilityMax=%u)",
            clearedActiveViewportCount, source ? source : "runtime", viewportKey, generatedFrames, capabilityMax);
    }

    if (stateChanged) {
        HookLog(
            "Streamline Hook: Viewport %u state active=%d multiplier=%dx generatedFrames=%u capabilityMax=%u "
            "source=%s clearAll=%d",
            viewportKey, active ? 1 : 0, active ? multiplier : 0, generatedFrames, capabilityMax,
            source ? source : "runtime", clearAllViewportStatesForDisable ? 1 : 0);
    }
}

template <typename T>
bool InstallInlineHookOnce(void* target, void* detour, T& original, std::atomic<bool>& installedFlag,
                           std::atomic<void*>& targetSlot, const char* hookName) {
    if (!target) {
        return false;
    }

    if (target == detour) {
        original = nullptr;
        targetSlot.store(target, std::memory_order_release);
        installedFlag.store(true, std::memory_order_release);
        return true;
    }

    const void* installedTarget = targetSlot.load(std::memory_order_acquire);
    if (installedFlag.load(std::memory_order_acquire) && installedTarget == target) {
        return false;
    }

    void* trampoline = nullptr;
    if (!InlineHook::Install(target, detour, &trampoline)) {
        HookLogImportant("Streamline Hook: Failed to inline hook %s at %p", hookName, target);
        return false;
    }

    original = reinterpret_cast<T>(trampoline);
    targetSlot.store(target, std::memory_order_release);
    installedFlag.store(true, std::memory_order_release);
    HookLogImportant("Streamline Hook: Inline hook installed for %s at %p (trampoline=%p)", hookName, target,
                     trampoline);
    return true;
}

void LogFeatureImportFallbackUnavailableOnce(const char* moduleBaseName, const char* functionName, void* exportedProc,
                                             const char* hookName, const char* reason) {
    const char* effectiveHookName = hookName ? hookName : functionName;
    if (!moduleBaseName || !effectiveHookName) {
        return;
    }

    static std::mutex s_logMutex;
    static std::unordered_map<std::string, bool> s_loggedUnavailable;

    std::string key = effectiveHookName;
    key += '|';
    key += moduleBaseName;
    key += '|';
    key += std::to_string(reinterpret_cast<uintptr_t>(exportedProc));

    {
        std::lock_guard<std::mutex> lock(s_logMutex);
        if (s_loggedUnavailable.find(key) != s_loggedUnavailable.end()) {
            return;
        }
        s_loggedUnavailable.emplace(key, true);
    }

    HookLogImportant("Streamline Hook: Direct import fallback unavailable for %s via %s (export=%p): %s",
                     effectiveHookName, moduleBaseName, exportedProc, reason ? reason : "no matching loaded import");
}

bool InstallFeatureImportFallbackIfPresent(const char* moduleBaseName, const char* functionName, void* detour,
                                           void* exportedProc, void** originalSlot, const char* hookName) {
    if (!moduleBaseName || !functionName || !detour || !exportedProc) {
        return false;
    }

    if (originalSlot && *originalSlot == nullptr) {
        *originalSlot = exportedProc;
    }

    void* patchedOriginal = nullptr;
    if (!IATHook::PatchIATAllModules(moduleBaseName, functionName, detour, &patchedOriginal)) {
        LogFeatureImportFallbackUnavailableOnce(
            moduleBaseName, functionName, exportedProc, hookName,
            "no loaded module currently imports this feature directly; retrying on later Streamline module scans");
        return false;
    }

    if (originalSlot && *originalSlot == nullptr) {
        *originalSlot = patchedOriginal ? patchedOriginal : exportedProc;
    }

    HookLogImportant("Streamline Hook: Installed direct import fallback for %s via %s (export=%p original=%p)",
                     hookName ? hookName : functionName, moduleBaseName, exportedProc,
                     originalSlot ? *originalSlot : patchedOriginal);
    return true;
}

bool TryGetOwningModulePath(void* address, char* modulePath, DWORD modulePathCapacity, DWORD* outError) {
    if (outError) {
        *outError = ERROR_SUCCESS;
    }
    if (!address || !modulePath || modulePathCapacity == 0) {
        if (outError) {
            *outError = ERROR_INVALID_PARAMETER;
        }
        return false;
    }

    HMODULE ownerModule = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(address), &ownerModule) ||
        !ownerModule) {
        if (outError) {
            *outError = GetLastError();
        }
        return false;
    }

    const DWORD pathLen = GetModuleFileNameA(ownerModule, modulePath, modulePathCapacity);
    if (pathLen == 0 || pathLen >= modulePathCapacity) {
        if (outError) {
            *outError = pathLen >= modulePathCapacity ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
        }
        return false;
    }
    return true;
}

bool TryInstallFeatureImportFallbackForOwningModule(void* function, const char* functionName, void* detour,
                                                    void** originalSlot, std::atomic<void*>& attemptedTarget,
                                                    const char* hookName) {
    if (!function || !functionName || !detour) {
        return false;
    }

    if (attemptedTarget.load(std::memory_order_acquire) == function) {
        return false;
    }

    char ownerPath[MAX_PATH] = {};
    DWORD ownerError = ERROR_SUCCESS;
    if (!TryGetOwningModulePath(function, ownerPath, MAX_PATH, &ownerError)) {
        attemptedTarget.store(function, std::memory_order_release);
        HookLogImportant("Streamline Hook: Direct import fallback owner resolution failed for %s target=%p error=%lu",
                         hookName ? hookName : functionName, function, static_cast<unsigned long>(ownerError));
        return false;
    }

    attemptedTarget.store(function, std::memory_order_release);

    const char* ownerBaseName = GetModuleBaseName(ownerPath);
    if (!ownerBaseName || !ownerBaseName[0]) {
        return false;
    }

    return InstallFeatureImportFallbackIfPresent(ownerBaseName, functionName, detour, function, originalSlot, hookName);
}

void LogReturnedWrapperFallbackOnce(std::atomic<bool>& loggedFlag, const char* hookName, void* target, void* wrapper,
                                    bool hookReady) {
    if (loggedFlag.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    HookLogImportant(
        "Streamline Hook: Using returned-pointer wrapper fallback for %s (target=%p wrapper=%p hookReady=%d). "
        "Callers that cache slGetFeatureFunction results remain intercepted even if Streamline later reloads, "
        "repairs, or bypasses the feature export patch.",
        hookName ? hookName : "<unknown>", target, wrapper, hookReady ? 1 : 0);
}

void LogProactiveFeatureHookGapOnce(std::atomic<bool>& loggedFlag, const char* hookName, void* target) {
    if (loggedFlag.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    HookLogImportant(
        "Streamline Hook: Proactively resolved %s at %p but could not patch the export/import path yet; "
        "waiting for an intercepted slGetFeatureFunction lookup to return the wrapper fallback or for a later module "
        "scan to find a direct import.",
        hookName ? hookName : "<unknown>", target);
}

void LogFeatureLookupOutcomeOnce(std::atomic<bool>& loggedFlag, const char* hookName, void* originalTarget,
                                 void* returnedTarget, bool hookReady) {
    if (loggedFlag.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    HookLogImportant(
        "Streamline Hook: slGetFeatureFunction returned %s target=%p delivered=%p hookReady=%d "
        "wrapperSubstituted=%d",
        hookName ? hookName : "<unknown>", originalTarget, returnedTarget, hookReady ? 1 : 0,
        originalTarget != returnedTarget ? 1 : 0);
}

bool MaybeHookDLSSGSetOptions(void*& function, bool fallbackToReturnedWrapper) {
    if (!function) {
        return false;
    }

    if (function == reinterpret_cast<void*>(Hooked_slDLSSGSetOptions)) {
        g_DLSSGSetOptionsHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(g_FeatureHookMutex);
        if (!g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ||
            g_DLSSGSetOptionsTarget.load(std::memory_order_acquire) != function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(function), reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                                  g_Original_slDLSSGSetOptions, g_DLSSGSetOptionsHooked, g_DLSSGSetOptionsTarget,
                                  "slDLSSGSetOptions");
            if (!g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    function, "slDLSSGSetOptions", reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                    reinterpret_cast<void**>(&g_Original_slDLSSGSetOptions),
                    g_DLSSGSetOptionsImportFallbackAttemptedTarget, "slDLSSGSetOptions");
            }
        }
    }

    const bool hookReady = g_DLSSGSetOptionsHooked.load(std::memory_order_acquire);
    if (fallbackToReturnedWrapper && function != reinterpret_cast<void*>(Hooked_slDLSSGSetOptions)) {
        if (!GetCallableOriginalDLSSGSetOptions() && !hookReady && !g_Original_slDLSSGSetOptions) {
            g_Original_slDLSSGSetOptions = reinterpret_cast<PFN_slDLSSGSetOptions>(function);
        }
        if (ce::streamline_runtime_policy::ShouldSubstituteReturnedStreamlineFeatureWrapper(
                fallbackToReturnedWrapper, false, GetCallableOriginalDLSSGSetOptions() != nullptr)) {
            LogReturnedWrapperFallbackOnce(g_DLSSGSetOptionsReturnedWrapperFallbackLogged, "slDLSSGSetOptions",
                                           function, reinterpret_cast<void*>(Hooked_slDLSSGSetOptions), hookReady);
            function = reinterpret_cast<void*>(Hooked_slDLSSGSetOptions);
            return true;
        }
    }

    return hookReady;
}

bool MaybeHookDLSSGGetState(void*& function, bool fallbackToReturnedWrapper) {
    if (!function) {
        return false;
    }

    if (function == reinterpret_cast<void*>(Hooked_slDLSSGGetState)) {
        g_DLSSGGetStateHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(g_FeatureHookMutex);
        if (!g_DLSSGGetStateHooked.load(std::memory_order_acquire) ||
            g_DLSSGGetStateTarget.load(std::memory_order_acquire) != function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(function), reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                                  g_Original_slDLSSGGetState, g_DLSSGGetStateHooked, g_DLSSGGetStateTarget,
                                  "slDLSSGGetState");
            if (!g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    function, "slDLSSGGetState", reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                    reinterpret_cast<void**>(&g_Original_slDLSSGGetState), g_DLSSGGetStateImportFallbackAttemptedTarget,
                    "slDLSSGGetState");
            }
        }
    }

    const bool hookReady = g_DLSSGGetStateHooked.load(std::memory_order_acquire);
    if (fallbackToReturnedWrapper && function != reinterpret_cast<void*>(Hooked_slDLSSGGetState)) {
        if (!GetCallableOriginalDLSSGGetState() && !hookReady && !g_Original_slDLSSGGetState) {
            g_Original_slDLSSGGetState = reinterpret_cast<PFN_slDLSSGGetState>(function);
        }
        if (ce::streamline_runtime_policy::ShouldSubstituteReturnedStreamlineFeatureWrapper(
                fallbackToReturnedWrapper, false, GetCallableOriginalDLSSGGetState() != nullptr)) {
            LogReturnedWrapperFallbackOnce(g_DLSSGGetStateReturnedWrapperFallbackLogged, "slDLSSGGetState", function,
                                           reinterpret_cast<void*>(Hooked_slDLSSGGetState), hookReady);
            function = reinterpret_cast<void*>(Hooked_slDLSSGGetState);
            return true;
        }
    }

    return hookReady;
}

bool MaybeHookReflexSleep(void*& function, bool fallbackToReturnedWrapper) {
    if (!function) {
        return false;
    }

    if (function == reinterpret_cast<void*>(Hooked_slReflexSleep)) {
        g_ReflexSleepHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(g_FeatureHookMutex);
        if (!g_ReflexSleepHooked.load(std::memory_order_acquire) ||
            g_ReflexSleepTarget.load(std::memory_order_acquire) != function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(function), reinterpret_cast<void*>(Hooked_slReflexSleep),
                                  g_Original_slReflexSleep, g_ReflexSleepHooked, g_ReflexSleepTarget, "slReflexSleep");
            if (!g_ReflexSleepHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    function, "slReflexSleep", reinterpret_cast<void*>(Hooked_slReflexSleep),
                    reinterpret_cast<void**>(&g_Original_slReflexSleep), g_ReflexSleepImportFallbackAttemptedTarget,
                    "slReflexSleep");
            }
        }
    }

    if (fallbackToReturnedWrapper && !g_ReflexSleepHooked.load(std::memory_order_acquire)) {
        if (!g_Original_slReflexSleep) {
            g_Original_slReflexSleep = reinterpret_cast<PFN_slReflexSleep>(function);
        }
        LogReturnedWrapperFallbackOnce(g_ReflexSleepReturnedWrapperFallbackLogged, "slReflexSleep", function,
                                       reinterpret_cast<void*>(Hooked_slReflexSleep),
                                       g_ReflexSleepHooked.load(std::memory_order_acquire));
        function = reinterpret_cast<void*>(Hooked_slReflexSleep);
        return true;
    }

    return g_ReflexSleepHooked.load(std::memory_order_acquire);
}

bool MaybeHookReflexSetOptions(void*& function, bool fallbackToReturnedWrapper) {
    if (!function) {
        return false;
    }

    if (function == reinterpret_cast<void*>(Hooked_slReflexSetOptions)) {
        g_ReflexSetOptionsHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(g_FeatureHookMutex);
        if (!g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ||
            g_ReflexSetOptionsTarget.load(std::memory_order_acquire) != function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(function), reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                                  g_Original_slReflexSetOptions, g_ReflexSetOptionsHooked, g_ReflexSetOptionsTarget,
                                  "slReflexSetOptions");
            if (!g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    function, "slReflexSetOptions", reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                    reinterpret_cast<void**>(&g_Original_slReflexSetOptions),
                    g_ReflexSetOptionsImportFallbackAttemptedTarget, "slReflexSetOptions");
            }
        }
    }

    if (fallbackToReturnedWrapper && !g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
        if (!g_Original_slReflexSetOptions) {
            g_Original_slReflexSetOptions = reinterpret_cast<PFN_slReflexSetOptions>(function);
        }
        LogReturnedWrapperFallbackOnce(g_ReflexSetOptionsReturnedWrapperFallbackLogged, "slReflexSetOptions", function,
                                       reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                                       g_ReflexSetOptionsHooked.load(std::memory_order_acquire));
        function = reinterpret_cast<void*>(Hooked_slReflexSetOptions);
        return true;
    }

    return g_ReflexSetOptionsHooked.load(std::memory_order_acquire);
}

bool MaybeHookReflexSetConstants(void*& function, bool fallbackToReturnedWrapper) {
    if (!function) {
        return false;
    }

    if (function == reinterpret_cast<void*>(Hooked_slReflexSetConstants)) {
        g_ReflexSetConstantsHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(g_FeatureHookMutex);
        if (!g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ||
            g_ReflexSetConstantsTarget.load(std::memory_order_acquire) != function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(function),
                                  reinterpret_cast<void*>(Hooked_slReflexSetConstants), g_Original_slReflexSetConstants,
                                  g_ReflexSetConstantsHooked, g_ReflexSetConstantsTarget, "slReflexSetConstants");
            if (!g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    function, "slReflexSetConstants", reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                    reinterpret_cast<void**>(&g_Original_slReflexSetConstants),
                    g_ReflexSetConstantsImportFallbackAttemptedTarget, "slReflexSetConstants");
            }
        }
    }

    if (fallbackToReturnedWrapper && !g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
        if (!g_Original_slReflexSetConstants) {
            g_Original_slReflexSetConstants = reinterpret_cast<PFN_slReflexSetConstants>(function);
        }
        LogReturnedWrapperFallbackOnce(g_ReflexSetConstantsReturnedWrapperFallbackLogged, "slReflexSetConstants",
                                       function, reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                                       g_ReflexSetConstantsHooked.load(std::memory_order_acquire));
        function = reinterpret_cast<void*>(Hooked_slReflexSetConstants);
        return true;
    }

    return g_ReflexSetConstantsHooked.load(std::memory_order_acquire);
}

bool TryResolveDLSSGFeatureHooks() {
    auto originalGetFeatureFunction = GetCallableOriginalGetFeatureFunction();
    if (!originalGetFeatureFunction) {
        return false;
    }

    bool hookedAnything = false;

    if (!g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
        void* function = nullptr;
        const slResult result = originalGetFeatureFunction(kSLFeatureDLSSG, "slDLSSGSetOptions", function);
        if (result == kSlResultOk && function) {
            const bool hooked = MaybeHookDLSSGSetOptions(function, false);
            hookedAnything |= hooked;
            if (!hooked && !g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(g_DLSSGSetOptionsProactiveFallbackLogged, "slDLSSGSetOptions", function);
            }
        }
    }

    if (!g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
        void* function = nullptr;
        const slResult result = originalGetFeatureFunction(kSLFeatureDLSSG, "slDLSSGGetState", function);
        if (result == kSlResultOk && function) {
            const bool hooked = MaybeHookDLSSGGetState(function, false);
            hookedAnything |= hooked;
            if (!hooked && !g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(g_DLSSGGetStateProactiveFallbackLogged, "slDLSSGGetState", function);
            }
        }
    }

    return hookedAnything || g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ||
           g_DLSSGGetStateHooked.load(std::memory_order_acquire);
}

bool TryResolveReflexFeatureHooks() {
    auto originalGetFeatureFunction = GetCallableOriginalGetFeatureFunction();
    if (!originalGetFeatureFunction) {
        return false;
    }

    bool hookedAnything = false;
    bool queriedSleep = false;
    bool queriedSetOptions = false;
    bool queriedSetConstants = false;
    slResult sleepResult = kSlResultErrorInvalidState;
    slResult setOptionsResult = kSlResultErrorInvalidState;
    slResult setConstantsResult = kSlResultErrorInvalidState;
    void* sleepFunction = nullptr;
    void* setOptionsFunction = nullptr;
    void* setConstantsFunction = nullptr;

    if (!g_ReflexSleepHooked.load(std::memory_order_acquire)) {
        queriedSleep = true;
        sleepResult = originalGetFeatureFunction(kSLFeatureReflex, "slReflexSleep", sleepFunction);
        if (sleepResult == kSlResultOk && sleepFunction) {
            const bool hooked = MaybeHookReflexSleep(sleepFunction, false);
            hookedAnything |= hooked;
            if (!hooked && !g_ReflexSleepHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(g_ReflexSleepProactiveFallbackLogged, "slReflexSleep", sleepFunction);
            }
        }
    }

    if (!g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
        queriedSetOptions = true;
        setOptionsResult = originalGetFeatureFunction(kSLFeatureReflex, "slReflexSetOptions", setOptionsFunction);
        if (setOptionsResult == kSlResultOk && setOptionsFunction) {
            const bool hooked = MaybeHookReflexSetOptions(setOptionsFunction, false);
            hookedAnything |= hooked;
            if (!hooked && !g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(g_ReflexSetOptionsProactiveFallbackLogged, "slReflexSetOptions",
                                               setOptionsFunction);
            }
        }
    }

    if (!g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
        queriedSetConstants = true;
        setConstantsResult = originalGetFeatureFunction(kSLFeatureReflex, "slReflexSetConstants", setConstantsFunction);
        if (setConstantsResult == kSlResultOk && setConstantsFunction) {
            const bool hooked = MaybeHookReflexSetConstants(setConstantsFunction, false);
            hookedAnything |= hooked;
            if (!hooked && !g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(g_ReflexSetConstantsProactiveFallbackLogged, "slReflexSetConstants",
                                               setConstantsFunction);
            }
        }
    }

    const bool hooksReady = hookedAnything || g_ReflexSleepHooked.load(std::memory_order_acquire) ||
                            g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ||
                            g_ReflexSetConstantsHooked.load(std::memory_order_acquire);
    if (hooksReady) {
        static std::atomic<bool> s_loggedResolved{false};
        if (!s_loggedResolved.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant(
                "Streamline Hook: Reflex feature hooks resolved (sleepHooked=%d setOptionsHooked=%d "
                "setConstantsHooked=%d sleep=%p setOptions=%p setConstants=%p)",
                g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0,
                g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
                g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ? 1 : 0, sleepFunction, setOptionsFunction,
                setConstantsFunction);
        }
    } else if (queriedSleep || queriedSetOptions || queriedSetConstants) {
        static std::atomic<bool> s_loggedUnavailable{false};
        if (!s_loggedUnavailable.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant(
                "Streamline Hook: Reflex feature functions not available yet via slGetFeatureFunction "
                "(sleep: queried=%d result=%d ptr=%p, setOptions: queried=%d result=%d ptr=%p, "
                "setConstants: queried=%d result=%d ptr=%p)",
                queriedSleep ? 1 : 0, sleepResult, sleepFunction, queriedSetOptions ? 1 : 0, setOptionsResult,
                setOptionsFunction, queriedSetConstants ? 1 : 0, setConstantsResult, setConstantsFunction);
        }
    }

    return hooksReady;
}

uint32_t QueryCapabilityMax(const slViewportHandle& viewport, const slDLSSGOptions* options) {
    if (!GetCallableOriginalDLSSGGetState() && !TryResolveDLSSGFeatureHooks()) {
        return 0;
    }
    auto originalGetState = GetCallableOriginalDLSSGGetState();
    if (!originalGetState) {
        return 0;
    }

    slDLSSGState state;
    const slResult result = originalGetState(viewport, state, options);
    if (result != kSlResultOk || state.numFramesToGenerateMax == 0) {
        return 0;
    }

    const uint32_t viewportKey = GetViewportKey(viewport);
    CacheCapabilityMax(viewportKey, state.numFramesToGenerateMax);
    return state.numFramesToGenerateMax;
}

void RegisterDynamicHooksOnce() {
    if (g_DynamicHooksRegistered.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    IATHook::RegisterDynamicHookFiltered("slGetFeatureFunction", reinterpret_cast<void*>(Hooked_slGetFeatureFunction),
                                         reinterpret_cast<void**>(&g_Original_slGetFeatureFunction),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slGetPluginFunction", reinterpret_cast<void*>(Hooked_slGetPluginFunction),
                                         reinterpret_cast<void**>(&g_Original_slGetPluginFunction),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slSetD3DDevice", reinterpret_cast<void*>(Hooked_slSetD3DDevice),
                                         reinterpret_cast<void**>(&g_Original_slSetD3DDevice),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slSetTag", reinterpret_cast<void*>(Hooked_slSetTag),
                                         reinterpret_cast<void**>(&g_Original_slSetTag),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slSetTagForFrame", reinterpret_cast<void*>(Hooked_slSetTagForFrame),
                                         reinterpret_cast<void**>(&g_Original_slSetTagForFrame),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slEvaluateFeature", reinterpret_cast<void*>(Hooked_slEvaluateFeature),
                                         reinterpret_cast<void**>(&g_Original_slEvaluateFeature),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slDLSSGSetOptions", reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                                         reinterpret_cast<void**>(&g_Original_slDLSSGSetOptions),
                                         IsStreamlineDLSSGDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slDLSSGGetState", reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                                         reinterpret_cast<void**>(&g_Original_slDLSSGGetState),
                                         IsStreamlineDLSSGDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slReflexSleep", reinterpret_cast<void*>(Hooked_slReflexSleep),
                                         reinterpret_cast<void**>(&g_Original_slReflexSleep),
                                         IsStreamlineReflexDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slReflexSetOptions", reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                                         reinterpret_cast<void**>(&g_Original_slReflexSetOptions),
                                         IsStreamlineReflexDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slReflexSetConstants", reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                                         reinterpret_cast<void**>(&g_Original_slReflexSetConstants),
                                         IsStreamlineReflexDynamicHookModule);
    HookLogImportant(
        "Streamline Hook: Registered module-filtered dynamic hooks for core Streamline exports and owned feature "
        "exports");
}

bool InstallHooksForModule(HMODULE module, const char* moduleNameOrPath) {
    if (!module || !IsStreamlineModuleName(moduleNameOrPath)) {
        return false;
    }

    g_FGCompat.SetStreamlineSupportPresent(true);

    RegisterDynamicHooksOnce();

    const char* moduleBaseName = GetModuleBaseName(moduleNameOrPath);
    const bool shouldHookCoreExports = ShouldHookStreamlineCoreExports(moduleBaseName);
    const uint32_t moduleBit = GetModuleMaskBit(moduleBaseName);
    const auto originalGetFeatureFunction =
        reinterpret_cast<PFN_slGetFeatureFunction>(GetProcAddress(module, "slGetFeatureFunction"));
    const auto originalGetPluginFunction =
        reinterpret_cast<PFN_slGetPluginFunction>(GetProcAddress(module, "slGetPluginFunction"));
    const auto originalSetD3DDevice = reinterpret_cast<PFN_slSetD3DDevice>(GetProcAddress(module, "slSetD3DDevice"));
    const auto originalSetTag = reinterpret_cast<PFN_slSetTag>(GetProcAddress(module, "slSetTag"));
    const auto originalSetTagForFrame =
        reinterpret_cast<PFN_slSetTagForFrame>(GetProcAddress(module, "slSetTagForFrame"));
    const auto originalEvaluateFeature =
        reinterpret_cast<PFN_slEvaluateFeature>(GetProcAddress(module, "slEvaluateFeature"));
    const auto originalDLSSGSetOptions =
        reinterpret_cast<PFN_slDLSSGSetOptions>(GetProcAddress(module, "slDLSSGSetOptions"));
    const auto originalDLSSGGetState = reinterpret_cast<PFN_slDLSSGGetState>(GetProcAddress(module, "slDLSSGGetState"));
    const auto originalReflexSleep = reinterpret_cast<PFN_slReflexSleep>(GetProcAddress(module, "slReflexSleep"));
    const auto originalReflexSetOptions =
        reinterpret_cast<PFN_slReflexSetOptions>(GetProcAddress(module, "slReflexSetOptions"));
    const auto originalReflexSetConstants =
        reinterpret_cast<PFN_slReflexSetConstants>(GetProcAddress(module, "slReflexSetConstants"));

    if (!originalGetFeatureFunction && !originalGetPluginFunction && !originalSetD3DDevice && !originalSetTag &&
        !originalSetTagForFrame && !originalEvaluateFeature && !originalDLSSGSetOptions && !originalDLSSGGetState &&
        !originalReflexSleep && !originalReflexSetOptions && !originalReflexSetConstants) {
        return false;
    }

    if (moduleBit != 0 && (g_InstalledModuleMask.load(std::memory_order_acquire) & moduleBit) != 0) {
        // Self-heal for unload/reload generations when the unload notification
        // was unavailable: the mask claims this core module is hooked, but the
        // stored core targets must belong to the ARRIVING instance. If none
        // do, the mask refers to a previous unloaded generation (whose address
        // range may since have been re-mapped by a different module —
        // 20260612_003407 crash) and the fresh instance must be re-hooked.
        const auto targetWithinModule = [](std::atomic<void*>& targetSlot, HMODULE candidate) {
            if (!candidate) {
                return false;
            }
            void* target = targetSlot.load(std::memory_order_acquire);
            return ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(
                target, nullptr, reinterpret_cast<const void*>(candidate), GetModuleImageSizeBytes(candidate));
        };
        const bool anyCoreHookTargetWithinModule = targetWithinModule(g_SLGetFeatureFunctionTarget, module) ||
                                                   targetWithinModule(g_SLGetPluginFunctionTarget, module) ||
                                                   targetWithinModule(g_SLSetD3DDeviceTarget, module) ||
                                                   targetWithinModule(g_SLSetTagTarget, module) ||
                                                   targetWithinModule(g_SLSetTagForFrameTarget, module) ||
                                                   targetWithinModule(g_SLEvaluateFeatureTarget, module);
        if (!ce::streamline_runtime_policy::IsInstalledStreamlineModuleMaskStaleForReloadedModule(
                true, anyCoreHookTargetWithinModule)) {
            return false;
        }

        // Clear only the core slots that no longer belong to ANY live core
        // module instance; a still-loaded sibling core module's valid slots
        // must survive this self-heal.
        const HMODULE liveInterposer = GetModuleHandleA("sl.interposer.dll");
        const HMODULE liveCommon = GetModuleHandleA("sl.common.dll");
        struct CoreSlotView {
            const char* name;
            std::atomic<void*>* target;
            std::atomic<bool>* installed;
            void* volatile* original;
        };
        CoreSlotView coreSlots[] = {
            {"slGetFeatureFunction", &g_SLGetFeatureFunctionTarget, &g_SLGetFeatureFunctionHooked,
             reinterpret_cast<void* volatile*>(&g_Original_slGetFeatureFunction)},
            {"slGetPluginFunction", &g_SLGetPluginFunctionTarget, &g_SLGetPluginFunctionHooked,
             reinterpret_cast<void* volatile*>(&g_Original_slGetPluginFunction)},
            {"slSetD3DDevice", &g_SLSetD3DDeviceTarget, &g_SLSetD3DDeviceHooked,
             reinterpret_cast<void* volatile*>(&g_Original_slSetD3DDevice)},
            {"slSetTag", &g_SLSetTagTarget, &g_SLSetTagHooked, reinterpret_cast<void* volatile*>(&g_Original_slSetTag)},
            {"slSetTagForFrame", &g_SLSetTagForFrameTarget, &g_SLSetTagForFrameHooked,
             reinterpret_cast<void* volatile*>(&g_Original_slSetTagForFrame)},
            {"slEvaluateFeature", &g_SLEvaluateFeatureTarget, &g_SLEvaluateFeatureHooked,
             reinterpret_cast<void* volatile*>(&g_Original_slEvaluateFeature)},
        };
        int healedSlots = 0;
        for (CoreSlotView& slot : coreSlots) {
            void* target = slot.target->load(std::memory_order_acquire);
            if (!target || targetWithinModule(*slot.target, module) ||
                targetWithinModule(*slot.target, liveInterposer) || targetWithinModule(*slot.target, liveCommon)) {
                continue;
            }
            InterlockedExchangePointer(slot.original, nullptr);
            slot.target->store(nullptr, std::memory_order_release);
            slot.installed->store(false, std::memory_order_release);
            ++healedSlots;
        }
        HookLogImportant(
            "Streamline Hook: %s reloaded at %p but the installed-module mask refers to a previous unloaded "
            "generation — cleared %d stale core slot(s) and re-hooking the fresh instance (liveInterposer=%p "
            "liveCommon=%p)",
            moduleBaseName, module, healedSlots, liveInterposer, liveCommon);
        g_InstalledModuleMask.fetch_and(~moduleBit, std::memory_order_acq_rel);
        g_IATPatchesMask.fetch_and(~moduleBit, std::memory_order_acq_rel);
    }

    if (!shouldHookCoreExports && (originalGetFeatureFunction || originalGetPluginFunction || originalSetD3DDevice)) {
        LogSkippedStreamlineCoreExportsOnce(moduleBaseName, module, originalGetFeatureFunction != nullptr,
                                            originalGetPluginFunction != nullptr, originalSetD3DDevice != nullptr);
    }

    bool hookedAnything = false;
    {
        std::lock_guard<std::mutex> lock(g_ModuleHookMutex);

        if (shouldHookCoreExports && originalGetFeatureFunction) {
            if (!g_Original_slGetFeatureFunction) {
                g_Original_slGetFeatureFunction = originalGetFeatureFunction;
            }

            hookedAnything |= InstallInlineHookOnce(reinterpret_cast<void*>(originalGetFeatureFunction),
                                                    reinterpret_cast<void*>(Hooked_slGetFeatureFunction),
                                                    g_Original_slGetFeatureFunction, g_SLGetFeatureFunctionHooked,
                                                    g_SLGetFeatureFunctionTarget, "slGetFeatureFunction");
        }

        if (shouldHookCoreExports && originalGetPluginFunction) {
            if (!g_Original_slGetPluginFunction) {
                g_Original_slGetPluginFunction = originalGetPluginFunction;
            }

            hookedAnything |= InstallInlineHookOnce(reinterpret_cast<void*>(originalGetPluginFunction),
                                                    reinterpret_cast<void*>(Hooked_slGetPluginFunction),
                                                    g_Original_slGetPluginFunction, g_SLGetPluginFunctionHooked,
                                                    g_SLGetPluginFunctionTarget, "slGetPluginFunction");
        }

        if (shouldHookCoreExports && originalSetD3DDevice) {
            if (!g_Original_slSetD3DDevice) {
                g_Original_slSetD3DDevice = originalSetD3DDevice;
            }

            hookedAnything |= InstallInlineHookOnce(
                reinterpret_cast<void*>(originalSetD3DDevice), reinterpret_cast<void*>(Hooked_slSetD3DDevice),
                g_Original_slSetD3DDevice, g_SLSetD3DDeviceHooked, g_SLSetD3DDeviceTarget, "slSetD3DDevice");
        }

        if (shouldHookCoreExports && originalSetTag) {
            if (!g_Original_slSetTag) {
                g_Original_slSetTag = originalSetTag;
            }

            hookedAnything |=
                InstallInlineHookOnce(reinterpret_cast<void*>(originalSetTag), reinterpret_cast<void*>(Hooked_slSetTag),
                                      g_Original_slSetTag, g_SLSetTagHooked, g_SLSetTagTarget, "slSetTag");
        }

        if (shouldHookCoreExports && originalSetTagForFrame) {
            if (!g_Original_slSetTagForFrame) {
                g_Original_slSetTagForFrame = originalSetTagForFrame;
            }

            hookedAnything |= InstallInlineHookOnce(
                reinterpret_cast<void*>(originalSetTagForFrame), reinterpret_cast<void*>(Hooked_slSetTagForFrame),
                g_Original_slSetTagForFrame, g_SLSetTagForFrameHooked, g_SLSetTagForFrameTarget, "slSetTagForFrame");
        }

        if (shouldHookCoreExports && originalEvaluateFeature) {
            if (!g_Original_slEvaluateFeature) {
                g_Original_slEvaluateFeature = originalEvaluateFeature;
            }

            hookedAnything |=
                InstallInlineHookOnce(reinterpret_cast<void*>(originalEvaluateFeature),
                                      reinterpret_cast<void*>(Hooked_slEvaluateFeature), g_Original_slEvaluateFeature,
                                      g_SLEvaluateFeatureHooked, g_SLEvaluateFeatureTarget, "slEvaluateFeature");
        }

        if (shouldHookCoreExports && moduleBit != 0 &&
            (g_IATPatchesMask.load(std::memory_order_acquire) & moduleBit) == 0) {
            void* dummy = nullptr;
            if (originalGetFeatureFunction) {
                IATHook::PatchIATAllModules(moduleBaseName, "slGetFeatureFunction",
                                            reinterpret_cast<void*>(Hooked_slGetFeatureFunction), &dummy);
            }
            if (originalGetPluginFunction) {
                IATHook::PatchIATAllModules(moduleBaseName, "slGetPluginFunction",
                                            reinterpret_cast<void*>(Hooked_slGetPluginFunction), &dummy);
            }
            if (originalSetD3DDevice) {
                IATHook::PatchIATAllModules(moduleBaseName, "slSetD3DDevice",
                                            reinterpret_cast<void*>(Hooked_slSetD3DDevice), &dummy);
            }
            if (originalSetTag) {
                IATHook::PatchIATAllModules(moduleBaseName, "slSetTag", reinterpret_cast<void*>(Hooked_slSetTag),
                                            &dummy);
            }
            if (originalSetTagForFrame) {
                IATHook::PatchIATAllModules(moduleBaseName, "slSetTagForFrame",
                                            reinterpret_cast<void*>(Hooked_slSetTagForFrame), &dummy);
            }
            if (originalEvaluateFeature) {
                IATHook::PatchIATAllModules(moduleBaseName, "slEvaluateFeature",
                                            reinterpret_cast<void*>(Hooked_slEvaluateFeature), &dummy);
            }
            g_IATPatchesMask.fetch_or(moduleBit, std::memory_order_acq_rel);
        }

        if (originalDLSSGSetOptions && ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad(
                                           "slDLSSGSetOptions", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slDLSSGSetOptions", reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                reinterpret_cast<void*>(originalDLSSGSetOptions),
                reinterpret_cast<void**>(&g_Original_slDLSSGSetOptions), "slDLSSGSetOptions");
        }

        if (originalDLSSGGetState &&
            ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slDLSSGGetState", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slDLSSGGetState", reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                reinterpret_cast<void*>(originalDLSSGGetState), reinterpret_cast<void**>(&g_Original_slDLSSGGetState),
                "slDLSSGGetState");
        }

        if (originalReflexSleep &&
            ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slReflexSleep", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSleep", reinterpret_cast<void*>(Hooked_slReflexSleep),
                reinterpret_cast<void*>(originalReflexSleep), reinterpret_cast<void**>(&g_Original_slReflexSleep),
                "slReflexSleep");
        }

        if (originalReflexSetOptions && ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad(
                                            "slReflexSetOptions", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSetOptions", reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                reinterpret_cast<void*>(originalReflexSetOptions),
                reinterpret_cast<void**>(&g_Original_slReflexSetOptions), "slReflexSetOptions");
        }

        if (originalReflexSetConstants && ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad(
                                              "slReflexSetConstants", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSetConstants", reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                reinterpret_cast<void*>(originalReflexSetConstants),
                reinterpret_cast<void**>(&g_Original_slReflexSetConstants), "slReflexSetConstants");
        }
    }

    if (hookedAnything) {
        if (moduleBit != 0) {
            g_InstalledModuleMask.fetch_or(moduleBit, std::memory_order_acq_rel);
        }
        HookLogImportant("Streamline Hook: Installed hooks for %s (%p)", moduleBaseName, module);
    }
    return true;
}

bool OpenLoadedModuleSnapshotWithRetry(HANDLE& snapshot, MODULEENTRY32& firstEntry, DWORD& error, int& attempts,
                                       bool& failedOnFirstEntry) {
    snapshot = INVALID_HANDLE_VALUE;
    error = ERROR_SUCCESS;
    attempts = 0;
    failedOnFirstEntry = false;

    constexpr int kMaxSnapshotAttempts = 4;
    for (int attempt = 1; attempt <= kMaxSnapshotAttempts; ++attempt) {
        attempts = attempt;
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            if (ce::streamline_runtime_policy::IsRetryableLoadedModuleSnapshotError(static_cast<uint32_t>(error)) &&
                attempt < kMaxSnapshotAttempts) {
                continue;
            }
            return false;
        }

        firstEntry = {};
        firstEntry.dwSize = sizeof(firstEntry);
        if (Module32First(snapshot, &firstEntry)) {
            error = ERROR_SUCCESS;
            return true;
        }

        error = GetLastError();
        failedOnFirstEntry = true;
        CloseHandle(snapshot);
        snapshot = INVALID_HANDLE_VALUE;
        if (!ce::streamline_runtime_policy::IsRetryableLoadedModuleSnapshotError(static_cast<uint32_t>(error)) ||
            attempt == kMaxSnapshotAttempts) {
            return false;
        }
    }

    return false;
}

bool ScanLoadedStreamlineModules() {
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    MODULEENTRY32 entry = {};
    DWORD error = ERROR_SUCCESS;
    int attempts = 0;
    bool failedOnFirstEntry = false;
    if (!OpenLoadedModuleSnapshotWithRetry(snapshot, entry, error, attempts, failedOnFirstEntry)) {
        if (!g_ModuleSnapshotFailureLogged.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant(
                failedOnFirstEntry
                    ? "Streamline Hook: Loaded-module enumeration was empty for feature hooks error=%lu attempts=%d "
                      "retryable=%d"
                    : "Streamline Hook: Failed to enumerate loaded modules for feature hooks error=%lu attempts=%d "
                      "retryable=%d",
                static_cast<unsigned long>(error), attempts,
                ce::streamline_runtime_policy::IsRetryableLoadedModuleSnapshotError(static_cast<uint32_t>(error)) ? 1
                                                                                                                  : 0);
        }
        return false;
    }

    g_ModuleSnapshotFailureLogged.store(false, std::memory_order_release);

    bool foundModule = false;
    size_t streamlineModuleCount = 0;
    size_t hookedModuleCount = 0;
    do {
        const char* moduleNameOrPath = entry.szExePath[0] != '\0' ? entry.szExePath : entry.szModule;
        if (!ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking(moduleNameOrPath)) {
            continue;
        }

        foundModule = true;
        ++streamlineModuleCount;
        g_FGCompat.SetStreamlineSupportPresent(true);
        if (InstallHooksForModule(entry.hModule, moduleNameOrPath)) {
            ++hookedModuleCount;
        }
    } while (Module32Next(snapshot, &entry));

    const DWORD iterationError = GetLastError();
    CloseHandle(snapshot);

    if (attempts > 1 && !g_ModuleSnapshotRetrySuccessLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "Streamline Hook: Loaded-module snapshot recovered after transient retry (attempts=%d modules=%zu "
            "hooked=%zu)",
            attempts, streamlineModuleCount, hookedModuleCount);
    }
    if (iterationError != ERROR_SUCCESS && iterationError != ERROR_NO_MORE_FILES &&
        !g_ModuleSnapshotFailureLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "Streamline Hook: Loaded-module enumeration ended unexpectedly for feature hooks error=%lu "
            "(modules=%zu hooked=%zu)",
            static_cast<unsigned long>(iterationError), streamlineModuleCount, hookedModuleCount);
    }
    return foundModule;
}

bool AreReflexFeatureHooksComplete() {
    return g_ReflexSleepHooked.load(std::memory_order_acquire) &&
           g_ReflexSetOptionsHooked.load(std::memory_order_acquire) &&
           g_ReflexSetConstantsHooked.load(std::memory_order_acquire);
}

void RetryResolveReflexFeatureHooksForRuntimeActivity(const char* source) {
    if (AreReflexFeatureHooksComplete()) {
        return;
    }

    constexpr ULONGLONG kRetryIntervalMs = 2500;
    const ULONGLONG nowMs = GetTickCount64();
    ULONGLONG previousMs = g_ReflexFeatureHookRetryLastMs.load(std::memory_order_acquire);
    if (previousMs != 0 && nowMs >= previousMs && (nowMs - previousMs) < kRetryIntervalMs) {
        return;
    }

    if (!g_ReflexFeatureHookRetryLastMs.compare_exchange_strong(previousMs, nowMs, std::memory_order_acq_rel,
                                                                std::memory_order_acquire)) {
        return;
    }

    const bool foundModule = ScanLoadedStreamlineModules();
    const bool resolved = TryResolveReflexFeatureHooks();
    static std::atomic<int> s_lateReflexRetryLogCount{0};
    const int logCount = s_lateReflexRetryLogCount.fetch_add(1, std::memory_order_relaxed);
    if (resolved || logCount < 10 || (logCount % 24) == 0) {
        HookLogImportant(
            "Streamline Hook: Late Reflex feature hook retry during DLSSG runtime activity "
            "(source=%s foundModule=%d resolved=%d sleepHooked=%d setOptionsHooked=%d setConstantsHooked=%d "
            "manualLimiter=%d targetIntervalUs=%u)",
            source ? source : "unknown", foundModule ? 1 : 0, resolved ? 1 : 0,
            g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_ReflexLimiter.IsManualLimiterConfiguredOrActive() ? 1 : 0, g_ReflexLimiter.GetTargetIntervalUs());
    }
}

slResult Hooked_slDLSSGGetState(const slViewportHandle& viewport, slDLSSGState& state, const slDLSSGOptions* options) {
    auto originalGetState = GetCallableOriginalDLSSGGetState();
    if (!originalGetState) {
        return kSlResultErrorInvalidState;
    }

    // Newer integrations can configure DLSS-G by passing options directly to GetState, after
    // slSetTagForFrame has already made the activation input volatile. Keep the latest inactive
    // DX12 UI tag covered before entering GetState so a late OFF->ON observation can adopt it.
    if (!ShouldKeepPureObserverOnlyStreamlineBehavior() && g_StreamlineUsesD3D12.load(std::memory_order_acquire) &&
        !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        const uint32_t requestedOutputs = options ? std::clamp(options->numFramesToGenerate + 1u, 1u, 6u) : 2u;
        ce::dx12_streamline_ui_overlay::BeginPreactivationStandby(requestedOutputs);
    }

    const slResult result = originalGetState(viewport, state, options);
    RetryResolveReflexFeatureHooksForRuntimeActivity("slDLSSGGetState");
    const uint32_t viewportKey = GetViewportKey(viewport);
    const bool viewportWasActive = WasViewportRuntimeStateActive(viewportKey);
    const bool hasRuntimeFenceEvidence = HasDLSSGRuntimeFenceEvidence(state);
    const bool suppressNewActivation = ShouldSuppressNewGetStateActivation();
    if (result == kSlResultOk && state.numFramesToGenerateMax > 0) {
        CacheCapabilityMax(viewportKey, state.numFramesToGenerateMax);
    }

    const uint32_t capabilityMax =
        state.numFramesToGenerateMax > 0 ? state.numFramesToGenerateMax : GetCachedCapabilityMax(viewportKey);
    const auto runtimeEvaluation = ce::streamline_runtime_policy::EvaluateViewportRuntimeUpdateFromGetState(
        result == kSlResultOk, options != nullptr, viewportWasActive, hasRuntimeFenceEvidence, suppressNewActivation,
        options ? options->mode : 0, options ? options->numFramesToGenerate : 0u, capabilityMax);
    const bool clearAllViewportStatesForDisable =
        runtimeEvaluation.update.shouldUpdate &&
        ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForGetStateDisable(
            result == kSlResultOk, options != nullptr, hasRuntimeFenceEvidence, options ? options->mode : 0u,
            capabilityMax);
    if (result == kSlResultOk && options != nullptr) {
        static std::atomic<int> s_getStateTraceLogCount{0};
        const int logCount = s_getStateTraceLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 8 || (logCount % 512) == 0) {
            char statusText[160];
            FormatDLSSGStatusFlags(state.status, statusText, sizeof(statusText));
            HookLogImportant(
                "Streamline Hook: slDLSSGGetState observed viewport=%u optionsMode=%s(%u) generated=%u "
                "capabilityMax=%u presented=%u status=0x%X(%s) minWH=%u vsyncOk=%d dynMFG=%d vramMB=%llu "
                "fence=%p fenceValue=%llu viewportWasActive=%d update=%d "
                "updateActive=%d clearAll=%d suppressNew=%d fenceEvidence=%d setOptionsHooked=%d "
                "setOptionsOriginal=%p",
                viewportKey, GetDLSSGModeName(options->mode), options->mode, options->numFramesToGenerate,
                capabilityMax, state.numFramesActuallyPresented, state.status, statusText, state.minWidthOrHeight,
                static_cast<int>(state.bIsVsyncSupportAvailable), static_cast<int>(state.bIsDynamicMFGSupported),
                (unsigned long long)(state.estimatedVRAMUsageInBytes / (1024ull * 1024ull)),
                state.inputsProcessingCompletionFence,
                (unsigned long long)state.lastPresentInputsProcessingCompletionFenceValue, viewportWasActive ? 1 : 0,
                runtimeEvaluation.update.shouldUpdate ? 1 : 0, runtimeEvaluation.update.active ? 1 : 0,
                clearAllViewportStatesForDisable ? 1 : 0, suppressNewActivation ? 1 : 0,
                hasRuntimeFenceEvidence ? 1 : 0, g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
                reinterpret_cast<void*>(g_Original_slDLSSGSetOptions));
        }
    }

    // [DLSSG HEALTH] — session 20260702_094955: GTA reported DLSSG ON (optionsMode=on, updateActive=1) but
    // presents stayed at base rate all session (numFramesActuallyPresented==1, no fps gain). sl.dlss_g
    // publishes WHY it declines to interpolate in DLSSGState.status; log every status transition, and while
    // the game requests ON without interpolation evidence, emit a deterministic streak warning that pairs
    // NVIDIA's status decode with Reflex call-activity evidence (DLSSG hard-requires Reflex, and GTA's
    // Reflex is historically flaky even without CE).
    if (result == kSlResultOk) {
        const uint32_t previousStatus = g_DLSSGLastObservedStatus.exchange(state.status, std::memory_order_relaxed);
        if (previousStatus != state.status) {
            char prevText[160];
            char nowText[160];
            FormatDLSSGStatusFlags(previousStatus, prevText, sizeof(prevText));
            FormatDLSSGStatusFlags(state.status, nowText, sizeof(nowText));
            HookLogImportant(
                "Streamline Hook: [DLSSG HEALTH] status TRANSITION 0x%X(%s) -> 0x%X(%s) (viewport=%u "
                "optionsMode=%s presented=%u minWH=%u vsyncOk=%d dynMFG=%d)",
                previousStatus, prevText, state.status, nowText, viewportKey,
                options ? GetDLSSGModeName(options->mode) : "n/a", state.numFramesActuallyPresented,
                state.minWidthOrHeight, static_cast<int>(state.bIsVsyncSupportAvailable),
                static_cast<int>(state.bIsDynamicMFGSupported));
        }
    }
    const bool optionsRequestOn = options != nullptr && options->mode != 0;
    if (ce::streamline_runtime_policy::ShouldTrackDLSSGActivationHealthSample(result == kSlResultOk,
                                                                              optionsRequestOn)) {
        const bool interpolationEvidence =
            ce::streamline_runtime_policy::IsDLSSGInterpolationPresentEvidence(state.numFramesActuallyPresented);
        uint64_t streak = 0;
        if (interpolationEvidence && state.status == 0) {
            g_DLSSGNotInterpolatingStreak.store(0, std::memory_order_relaxed);
        } else {
            streak = g_DLSSGNotInterpolatingStreak.fetch_add(1, std::memory_order_relaxed) + 1;
        }
        if (ce::streamline_runtime_policy::ShouldWarnDLSSGActiveButNotInterpolating(streak, kDLSSGHealthWarnStreak,
                                                                                    kDLSSGHealthWarnRepeat)) {
            const uint64_t nowMs = GetTickCount64();
            const uint64_t sleepCount = g_ReflexSleepObservedCount.load(std::memory_order_relaxed);
            const uint64_t sleepCountAtLastLog =
                g_ReflexSleepCountAtLastHealthLog.exchange(sleepCount, std::memory_order_relaxed);
            const uint64_t sleepLastMs = g_ReflexSleepLastTickMs.load(std::memory_order_relaxed);
            const uint64_t reflexOptCount = g_ReflexSetOptionsObservedCount.load(std::memory_order_relaxed);
            const uint64_t reflexOptLastMs = g_ReflexSetOptionsLastTickMs.load(std::memory_order_relaxed);
            char statusText[160];
            FormatDLSSGStatusFlags(state.status, statusText, sizeof(statusText));
            HookLogImportant(
                "Streamline Hook: [DLSSG HEALTH] ON but NOT interpolating for %llu consecutive GetState samples — "
                "status=0x%X(%s) presented=%u generatedReq=%u capabilityMax=%u minWH=%u vsyncOk=%d dynMFG=%d "
                "vramMB=%llu fence=%p fenceValue=%llu | Reflex evidence: sleepCalls=%llu (+%llu since last warn) "
                "sleepAge=%llums setOptionsCalls=%llu setOptionsAge=%llums lastMode=%d sleepHooked=%d | "
                "REFLEX-NOT-DETECTED in status = the game's Reflex pipeline is not running (DLSSG requires it); "
                "status=ok with presented==1 and dynMFG=1 can be hardware flip metering — correlate with the "
                "displayed fps",
                static_cast<unsigned long long>(streak), state.status, statusText, state.numFramesActuallyPresented,
                options->numFramesToGenerate, capabilityMax, state.minWidthOrHeight,
                static_cast<int>(state.bIsVsyncSupportAvailable), static_cast<int>(state.bIsDynamicMFGSupported),
                (unsigned long long)(state.estimatedVRAMUsageInBytes / (1024ull * 1024ull)),
                state.inputsProcessingCompletionFence,
                (unsigned long long)state.lastPresentInputsProcessingCompletionFenceValue,
                static_cast<unsigned long long>(sleepCount),
                static_cast<unsigned long long>(sleepCount - sleepCountAtLastLog),
                static_cast<unsigned long long>(sleepLastMs ? (nowMs - sleepLastMs) : 0),
                static_cast<unsigned long long>(reflexOptCount),
                static_cast<unsigned long long>(reflexOptLastMs ? (nowMs - reflexOptLastMs) : 0),
                g_ReflexLastForwardedMode.load(std::memory_order_relaxed),
                g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0);
        }
    } else if (result == kSlResultOk && options != nullptr && options->mode == 0) {
        // Explicit OFF request: end any pending not-interpolating streak so a later re-enable starts a
        // fresh, correctly-attributed streak.
        g_DLSSGNotInterpolatingStreak.store(0, std::memory_order_relaxed);
    }
    if (runtimeEvaluation.update.shouldUpdate) {
        UpdateViewportRuntimeState(viewportKey, runtimeEvaluation.update.active, runtimeEvaluation.update.multiplier,
                                   runtimeEvaluation.update.generatedFrames, runtimeEvaluation.update.capabilityMax,
                                   "GetState", clearAllViewportStatesForDisable);
    } else if (result == kSlResultOk && options != nullptr && runtimeEvaluation.suppressedFreshActivation) {
        static std::atomic<int> s_recentFfxTakeoverSuppressedGetStateLogCount{0};
        const int logCount = s_recentFfxTakeoverSuppressedGetStateLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 128) == 0) {
            const ULONGLONG suppressUntilMs = g_SuppressNewGetStateActivationUntilMs.load(std::memory_order_acquire);
            const ULONGLONG nowMs = GetTickCount64();
            const ULONGLONG remainingMs = suppressUntilMs > nowMs ? (suppressUntilMs - nowMs) : 0;
            const bool persistentBlock =
                g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.load(std::memory_order_acquire);
            const ULONGLONG startupTransitionUntilMs =
                DXGIShared::g_SharedState.streamlineStartupTransitionUntilMs.load(std::memory_order_acquire);
            const bool startupWindowActive = startupTransitionUntilMs != 0 && startupTransitionUntilMs > nowMs;
            const ULONGLONG startupRemainingMs = startupWindowActive ? (startupTransitionUntilMs - nowMs) : 0;
            HookLogImportant(
                "Streamline Hook: Suppressing fresh GetState DLSS FG reactivation "
                "(viewport=%u mode=%u generated=%u fence=%p fenceValue=%llu persistentBlock=%d startupWindow=%d "
                "startupRemaining=%llums remaining=%llums)",
                viewportKey, options->mode, options->numFramesToGenerate, state.inputsProcessingCompletionFence,
                (unsigned long long)state.lastPresentInputsProcessingCompletionFenceValue, persistentBlock ? 1 : 0,
                startupWindowActive ? 1 : 0, (unsigned long long)startupRemainingMs, (unsigned long long)remainingMs);
        }
    }

    if (!IsObserverOnlyModeActive()) {
        std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);
        const bool startupWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
        const bool startupActivationPending =
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
        const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
        const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
        const bool postSLConfirmedButStartupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
        const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
        const bool postSLConfirmedButRuntimeStateStabilizing =
            HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing() ||
            HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected();
        const bool explicitSetOptionsActivationForCurrentComeback =
            g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
        const bool hadFSRFGPhase = HookHasFSRFGHistory();
        const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
        const bool startupProtectedComebackProof =
            explicitSetOptionsActivationForCurrentComeback || safePostFSRBootstrapPath;
        const bool postSLConfirmedButOffChurnAwaitingActiveProof = IsStartupProtectedOffChurnAwaitingActiveProof(
            startupProtectedComebackProof, postSLConfirmedRendering, postSLConfirmedButStartupSettling);
        const bool effectivePostSLRuntimeStateStabilizing =
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof;
        const bool currentGetStateReportsInactive =
            runtimeEvaluation.update.shouldUpdate && !runtimeEvaluation.update.active;
        const bool acceptActivatedUnconfirmedResumeOff =
            ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
                currentGetStateReportsInactive, startupWindowActive, startupProtectedComebackProof,
                startupActivationPending, postSLActiveButUnconfirmed, postSLStartupActivationEntered,
                postSLConfirmedRendering, postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
        const bool shouldKeepDeferred =
            !acceptActivatedUnconfirmedResumeOff &&
            ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
                startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                postSLConfirmedRendering, postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
        if (g_SuppressedSetOptionsOffDuringStartup && !shouldKeepDeferred) {
            if (acceptActivatedUnconfirmedResumeOff) {
                LogAcceptedOffDuringActivatedUnconfirmedResume(
                    "GetState/suppressed-off-flush", startupWindowActive, hadFSRFGPhase,
                    explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath, startupActivationPending,
                    postSLActiveButUnconfirmed, postSLStartupActivationEntered, postSLConfirmedRendering,
                    postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
            }
            if (!acceptActivatedUnconfirmedResumeOff &&
                ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedStreamlineComeback(
                    hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                    postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing)) {
                LogDroppedSuppressedOffForStartupProtectedStreamlineComeback(
                    g_SuppressedOffViewportKey, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                    safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                    postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                    effectivePostSLRuntimeStateStabilizing);
            } else if (auto originalSetOptions = GetCallableOriginalDLSSGSetOptions()) {
                HookLogImportant(
                    "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) via GetState — startup window "
                    "expired (viewport=%u settling=%d stabilizing=%d activeProofPending=%d)",
                    g_SuppressedOffViewportKey, postSLConfirmedButStartupSettling ? 1 : 0,
                    effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                    postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0);
                const slResult offResult = originalSetOptions(g_SuppressedOffViewport, g_SuppressedOffOptions);
                if (offResult != kSlResultOk) {
                    HookLogImportant("Streamline Hook: Forwarded slDLSSGSetOptions(OFF) via GetState returned %d",
                                     offResult);
                }
            }
            g_SuppressedSetOptionsOffDuringStartup = false;
        }
    }

    // SL may overwrite our Present vtable hook asynchronously during FG
    // activation (not necessarily inside slDLSSGSetOptions).  This check
    // runs every frame the game polls FG state and will re-patch if needed.
    // Skip vtable repair while PostSL has not yet confirmed rendering, to
    // avoid calling through Steam's overlay hook chain during SL's DllMain
    // (which can crash gameoverlayrenderer64 with a null pointer).
    if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) && HookIsPostSLOverlayConfirmedRendering()) {
        DXGIShared::RepairVTableHooksIfNeeded();
    }

    return result;
}

slResult Hooked_slDLSSGSetOptions(const slViewportHandle& viewport, const slDLSSGOptions& options) {
    auto originalSetOptions = GetCallableOriginalDLSSGSetOptions();
    if (!originalSetOptions) {
        return kSlResultErrorInvalidState;
    }

    slDLSSGOptions adjustedOptions = CloneDLSSGOptions(options);
    const uint32_t viewportKey = GetViewportKey(viewport);
    const int configuredFactor = NormalizeDLSSFGFactor(GetActiveGraphicsConfig().parsed.dlssFGFactor);
    const uint32_t originalGeneratedFrames = options.numFramesToGenerate;
    const bool requestedEnabled = ce::streamline_runtime_policy::IsDLSSGModeEnabled(options.mode);
    const bool requestedDisabled = !requestedEnabled;

    uint32_t capabilityMax = GetCachedCapabilityMax(viewportKey);
    bool overrideApplied = false;
    bool overrideClamped = false;

    if (configuredFactor > 0 && requestedEnabled) {
        const uint32_t desiredGeneratedFrames = DLSSFGMultiplierToGeneratedFrames(configuredFactor);
        if (capabilityMax == 0 && desiredGeneratedFrames > 1) {
            capabilityMax = QueryCapabilityMax(viewport, &adjustedOptions);
        }

        uint32_t finalGeneratedFrames = desiredGeneratedFrames;
        if (capabilityMax > 0 && finalGeneratedFrames > capabilityMax) {
            finalGeneratedFrames = capabilityMax;
            overrideClamped = true;
        }

        if (finalGeneratedFrames > 0 && finalGeneratedFrames != adjustedOptions.numFramesToGenerate) {
            adjustedOptions.numFramesToGenerate = finalGeneratedFrames;
            overrideApplied = true;
        }
    }

    const bool pureObserverOnly = ShouldKeepPureObserverOnlyStreamlineBehavior();

    if (!pureObserverOnly && requestedEnabled) {
        std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);
        if (g_SuppressedSetOptionsOffDuringStartup) {
            const bool activationPending =
                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
            HookLogImportant(
                "Streamline Hook: Clearing suppressed slDLSSGSetOptions(OFF) due to explicit re-enable request "
                "(viewport=%u) — Streamline never received the OFF, re-enable is consistent "
                "(activationPending=%d)",
                viewportKey, activationPending ? 1 : 0);
            g_SuppressedSetOptionsOffDuringStartup = false;
        }
    }

    if (!pureObserverOnly) {
        std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);
        const bool startupWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
        const bool startupActivationPending =
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
        const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
        const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
        const bool postSLConfirmedButStartupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
        const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
        const bool postSLConfirmedButRuntimeStateStabilizing =
            HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing() ||
            HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected();
        const bool explicitSetOptionsActivationForCurrentComeback =
            g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
        const bool hadFSRFGPhase = HookHasFSRFGHistory();
        const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
        const bool startupProtectedComebackProof =
            explicitSetOptionsActivationForCurrentComeback || safePostFSRBootstrapPath;
        const bool postSLConfirmedButOffChurnAwaitingActiveProof = IsStartupProtectedOffChurnAwaitingActiveProof(
            startupProtectedComebackProof, postSLConfirmedRendering, postSLConfirmedButStartupSettling);
        const bool effectivePostSLRuntimeStateStabilizing =
            postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof;
        const bool explicitSetOptionsDisableIsAuthoritative =
            ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
                requestedDisabled, true, postSLConfirmedRendering, startupActivationPending, postSLActiveButUnconfirmed,
                postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing,
                g_AcceptedRuntimeOffAwaitingSetOptions.load(std::memory_order_acquire));
        const bool acceptActivatedUnconfirmedResumeOff =
            ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
                requestedDisabled, startupWindowActive, startupProtectedComebackProof, startupActivationPending,
                postSLActiveButUnconfirmed, postSLStartupActivationEntered, postSLConfirmedRendering,
                postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
        const bool shouldKeepDeferred =
            !explicitSetOptionsDisableIsAuthoritative && !acceptActivatedUnconfirmedResumeOff &&
            ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
                startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                postSLConfirmedRendering, postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
        if (g_SuppressedSetOptionsOffDuringStartup && !shouldKeepDeferred) {
            if (acceptActivatedUnconfirmedResumeOff) {
                LogAcceptedOffDuringActivatedUnconfirmedResume(
                    "SetOptions/suppressed-off-flush", startupWindowActive, hadFSRFGPhase,
                    explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath, startupActivationPending,
                    postSLActiveButUnconfirmed, postSLStartupActivationEntered, postSLConfirmedRendering,
                    postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
            }
            if (!acceptActivatedUnconfirmedResumeOff &&
                ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedStreamlineComeback(
                    hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                    postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing)) {
                LogDroppedSuppressedOffForStartupProtectedStreamlineComeback(
                    g_SuppressedOffViewportKey, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                    safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                    postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                    effectivePostSLRuntimeStateStabilizing);
            } else if (auto suppressedOriginalSetOptions = GetCallableOriginalDLSSGSetOptions()) {
                HookLogImportant(
                    "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) — startup window expired "
                    "(viewport=%u settling=%d stabilizing=%d activeProofPending=%d)",
                    g_SuppressedOffViewportKey, postSLConfirmedButStartupSettling ? 1 : 0,
                    effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                    postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0);
                const slResult offResult =
                    suppressedOriginalSetOptions(g_SuppressedOffViewport, g_SuppressedOffOptions);
                if (offResult != kSlResultOk) {
                    HookLogImportant("Streamline Hook: Forwarded slDLSSGSetOptions(OFF) returned %d", offResult);
                } else {
                    g_AcceptedRuntimeOffAwaitingSetOptions.store(false, std::memory_order_release);
                }
            }
            g_SuppressedSetOptionsOffDuringStartup = false;
        }
    }

    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool runtimeModeIsFSRFG = runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG;
    if (!pureObserverOnly && ce::streamline_runtime_policy::ShouldPrepareForStreamlineEnableBeforeOriginalCall(
                                 requestedEnabled, g_FGCompat.IsFSRFGApiActive(), runtimeModeIsFSRFG,
                                 DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration())) {
        DX12_PrepareForStreamlineEnableTransition();
    }

    const bool startupWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
    const bool startupActivationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
    const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
    const bool postSLConfirmedButStartupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
    const bool postSLConfirmedButRuntimeStateStabilizingBase = HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing();
    const bool postSLConfirmedButStaleOffWarmupProtected =
        requestedDisabled && HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected();
    const bool postSLConfirmedButRuntimeStateStabilizing =
        postSLConfirmedButRuntimeStateStabilizingBase || postSLConfirmedButStaleOffWarmupProtected;
    const bool explicitSetOptionsActivationForCurrentComeback =
        g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
    const bool hadFSRFGPhase = HookHasFSRFGHistory();
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    const bool startupProtectedComebackProof =
        explicitSetOptionsActivationForCurrentComeback || safePostFSRBootstrapPath;
    const bool postSLConfirmedButOffChurnAwaitingActiveProof = IsStartupProtectedOffChurnAwaitingActiveProof(
        startupProtectedComebackProof, postSLConfirmedRendering, postSLConfirmedButStartupSettling);
    const bool effectivePostSLRuntimeStateStabilizing =
        postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof;
    const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
    const bool explicitSetOptionsDisableIsAuthoritative =
        ce::streamline_runtime_policy::ShouldTreatExplicitSetOptionsDisableAsAuthoritative(
            requestedDisabled, true, postSLConfirmedRendering, startupActivationPending, postSLActiveButUnconfirmed,
            postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing,
            g_AcceptedRuntimeOffAwaitingSetOptions.load(std::memory_order_acquire));
    const bool acceptActivatedUnconfirmedResumeOff =
        ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
            requestedDisabled, startupWindowActive, startupProtectedComebackProof, startupActivationPending,
            postSLActiveButUnconfirmed, postSLStartupActivationEntered, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
    const bool suppressOffCall =
        !pureObserverOnly && requestedDisabled && !explicitSetOptionsDisableIsAuthoritative &&
        !acceptActivatedUnconfirmedResumeOff &&
        !ce::streamline_runtime_policy::ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend(
            g_ConfirmedDLSSReflexSuspendPending.load(std::memory_order_acquire), requestedDisabled,
            DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) &&
        ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
            startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
            safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);

    // Arm before forwarding the first OFF->ON call. Streamline is allowed to do synchronous
    // setup inside SetOptions; if it asks the app to tag the activation frame re-entrantly, the
    // official UIColorAndAlpha path must already be ready. BeginActivation is idempotent until
    // the corresponding accepted OFF transition.
    if (!pureObserverOnly && requestedEnabled && !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        ce::dx12_streamline_ui_overlay::BeginActivation(std::clamp(adjustedOptions.numFramesToGenerate + 1u, 1u, 6u));
    }

    const bool setOptionsCallSuppressed = suppressOffCall;
    slResult result;
    if (suppressOffCall) {
        if (postSLConfirmedButStaleOffWarmupProtected && !postSLConfirmedButRuntimeStateStabilizingBase) {
            static std::atomic<bool> s_loggedSetOptionsWarmupProofSuppression{false};
            if (!s_loggedSetOptionsWarmupProofSuppression.exchange(true, std::memory_order_relaxed)) {
                HookLogImportant(
                    "Streamline Hook: Suppressing slDLSSGSetOptions(OFF) during PostSL warmup proof "
                    "(hadFSR=%d explicit=%d safeBootstrap=%d stableProtectionWindow=%d-%d) — treating it as "
                    "startup stale-OFF churn until PostSL proves stable",
                    hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
                    safePostFSRBootstrapPath ? 1 : 0,
                    ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationFirstFrame(),
                    HookGetPostSLStaleOffWarmupProtectionLastFrame());
            }
        }
        static std::atomic<uint64_t> s_startupProtectedSetOptionsOffSuppressionLogCount{0};
        const uint64_t suppressionLogCount =
            s_startupProtectedSetOptionsOffSuppressionLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (suppressionLogCount <= 20 || (suppressionLogCount % 200) == 0) {
            HookLogImportant(
                "Streamline Hook: Suppressing slDLSSGSetOptions(OFF) while DLSS comeback remains startup-protected "
                "(viewport=%u mode=%u startupWindow=%d hadFSR=%d explicitComeback=%d safeBootstrap=%d pending=%d "
                "unconfirmed=%d confirmed=%d settling=%d stabilizing=%d activeProofPending=%d suppressCount=%llu) — "
                "preventing Streamline FG de-initialization before recovery proves stable",
                viewportKey, options.mode, startupWindowActive ? 1 : 0, HookHasFSRFGHistory() ? 1 : 0,
                explicitSetOptionsActivationForCurrentComeback ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0,
                startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                postSLConfirmedButStartupSettling ? 1 : 0, effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0,
                static_cast<unsigned long long>(suppressionLogCount));
        }
        MarkStartupProtectedOffChurnObserved("SetOptions", postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                                             effectivePostSLRuntimeStateStabilizing);
        {
            std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);
            g_SuppressedSetOptionsOffDuringStartup = true;
            g_SuppressedOffViewport = viewport;
            g_SuppressedOffOptions = adjustedOptions;
            g_SuppressedOffViewportKey = viewportKey;
        }
        result = kSlResultOk;
    } else {
        if (acceptActivatedUnconfirmedResumeOff) {
            LogAcceptedOffDuringActivatedUnconfirmedResume(
                "SetOptions", startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                postSLStartupActivationEntered, postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                effectivePostSLRuntimeStateStabilizing);
            ResetStartupProtectedOffChurnActiveProof("forwarded activated-unconfirmed SetOptions disable");
        } else if (explicitSetOptionsDisableIsAuthoritative) {
            HookLogImportant(
                "Streamline Hook: Accepting explicit slDLSSGSetOptions(OFF) as authoritative after confirmed PostSL "
                "rendering (viewport=%u startupWindow=%d hadFSR=%d safeBootstrap=%d pending=%d unconfirmed=%d "
                "settling=%d stabilizing=%d activeProofPending=%d)",
                viewportKey, startupWindowActive ? 1 : 0, hadFSRFGPhase ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0,
                startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                postSLConfirmedButStartupSettling ? 1 : 0, effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0);
            ResetStartupProtectedOffChurnActiveProof("forwarded authoritative SetOptions disable");
        }
        if (!pureObserverOnly && requestedEnabled) {
            DX12_BeginStreamlineEnableCall();
        }
        result = originalSetOptions(viewport, adjustedOptions);
        if (!pureObserverOnly && requestedEnabled) {
            DX12_EndStreamlineEnableCall();
        }
    }

    if (!pureObserverOnly && requestedEnabled && result != kSlResultOk &&
        !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        ce::dx12_streamline_ui_overlay::EndActivation("slDLSSGSetOptions enable failed");
    }

    LogDLSSGSetOptionsTransition(viewportKey, options, adjustedOptions, originalGeneratedFrames, capabilityMax,
                                 requestedEnabled, setOptionsCallSuppressed, overrideApplied, overrideClamped, result,
                                 pureObserverOnly, startupWindowActive, hadFSRFGPhase,
                                 explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
                                 startupActivationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
                                 postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
    RetryResolveReflexFeatureHooksForRuntimeActivity("slDLSSGSetOptions");

    // SL may overwrite our vtable hooks during slDLSSGSetOptions (especially
    // when re-activating FG).  Verify and repair immediately.
    if (requestedEnabled) {
        DXGIShared::RepairVTableHooksIfNeeded();

        // Detect Present bypass: if DetourPresent hasn't fired in the last N
        // slDLSSGSetOptions calls despite DLSS FG being active, it means SL's
        // wrapper is bypassing our vtable hook entirely.
        static uint64_t s_lastPresentCount = 0;
        static uint32_t s_stallFrames = 0;
        uint64_t currentPresentCount = DXGIShared::g_PresentCallCounter.load(std::memory_order_relaxed);
        if (currentPresentCount == s_lastPresentCount) {
            s_stallFrames++;
            if (s_stallFrames == 10 || s_stallFrames == 30 || s_stallFrames == 100 || (s_stallFrames % 500) == 0) {
                HookLogImportant(
                    "Streamline Hook: Present STALLED for %u frames (counter=%llu) — "
                    "vtable hook bypassed?",
                    s_stallFrames, (unsigned long long)currentPresentCount);
            }
            if (s_stallFrames == 30) {
                g_RenderWatchdog.RequestImmediateDump("Streamline Present stalled for 30 frames",
                                                      DX12_GetGamePresentThreadId());
            }
        } else {
            if (s_stallFrames > 0) {
                HookLogImportant("Streamline Hook: Present resumed after %u stalled frames (counter=%llu)",
                                 s_stallFrames, (unsigned long long)currentPresentCount);
            }
            s_stallFrames = 0;
        }
        s_lastPresentCount = currentPresentCount;
    }

    if (result == kSlResultOk) {
        if (!pureObserverOnly && requestedEnabled) {
            g_AcceptedRuntimeOffAwaitingSetOptions.store(false, std::memory_order_release);
            const ULONGLONG previousSuppressUntilMs =
                g_SuppressNewGetStateActivationUntilMs.exchange(0, std::memory_order_acq_rel);
            const bool wasBlockingGetStateOnlyReactivation =
                g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.exchange(false, std::memory_order_acq_rel);
            const bool wasBlockingUnsafePostFSRGetStateOnlyReactivation =
                g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap.exchange(false, std::memory_order_acq_rel);
            if (previousSuppressUntilMs != 0) {
                const ULONGLONG nowMs = GetTickCount64();
                if (previousSuppressUntilMs > nowMs) {
                    HookLogImportant(
                        "Streamline Hook: Cleared recent-authoritative-FFX GetState suppression due to explicit "
                        "slDLSSGSetOptions enable request (viewport=%u remaining=%llums)",
                        viewportKey, (unsigned long long)(previousSuppressUntilMs - nowMs));
                }
            }
            if (wasBlockingGetStateOnlyReactivation) {
                HookLogImportant(
                    "Streamline Hook: Cleared persistent GetState-only DLSS FG suppression due to explicit "
                    "slDLSSGSetOptions enable request (viewport=%u)",
                    viewportKey);
            }
            if (wasBlockingUnsafePostFSRGetStateOnlyReactivation) {
                HookLogImportant(
                    "Streamline Hook: Cleared unsafe post-FSR GetState-only DLSS FG suppression due to explicit "
                    "slDLSSGSetOptions enable request (viewport=%u)",
                    viewportKey);
            }
        } else if (!pureObserverOnly && requestedDisabled &&
                   ce::streamline_runtime_policy::ShouldApplyViewportRuntimeUpdateFromSetOptions(
                       result == kSlResultOk, setOptionsCallSuppressed)) {
            ResetStartupProtectedOffChurnActiveProof("forwarded explicit SetOptions disable");
            g_SuppressNewGetStateActivationUntilMs.store(0, std::memory_order_release);
            const bool wasBlockingGetStateOnlyReactivation =
                g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.exchange(true, std::memory_order_acq_rel);
            if (!wasBlockingGetStateOnlyReactivation) {
                HookLogImportant(
                    "Streamline Hook: Re-armed persistent GetState-only DLSS FG suppression due to explicit "
                    "slDLSSGSetOptions disable request (viewport=%u)",
                    viewportKey);
            }
        }

        if (ce::streamline_runtime_policy::ShouldApplyViewportRuntimeUpdateFromSetOptions(result == kSlResultOk,
                                                                                          setOptionsCallSuppressed)) {
            const auto runtimeUpdate = ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromRequestedOptions(
                true, true, adjustedOptions.mode, adjustedOptions.numFramesToGenerate, capabilityMax);
            const bool clearAllViewportStatesForDisable =
                ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForSetOptionsDisable(
                    result == kSlResultOk, setOptionsCallSuppressed, adjustedOptions.mode);
            UpdateViewportRuntimeState(viewportKey, runtimeUpdate.active, runtimeUpdate.multiplier,
                                       runtimeUpdate.generatedFrames, runtimeUpdate.capabilityMax, "SetOptions",
                                       clearAllViewportStatesForDisable);
        } else if (setOptionsCallSuppressed) {
            static std::atomic<int> s_suppressedSetOptionsRuntimeSkipLogCount{0};
            const int logCount = s_suppressedSetOptionsRuntimeSkipLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 200) == 0) {
                HookLogImportant(
                    "Streamline Hook: Skipping local runtime-state reduction for suppressed slDLSSGSetOptions(OFF) "
                    "because Streamline never observed that disable edge (viewport=%u startupWindow=%d pending=%d "
                    "unconfirmed=%d confirmed=%d settling=%d stabilizing=%d activeProofPending=%d)",
                    viewportKey, startupWindowActive ? 1 : 0, startupActivationPending ? 1 : 0,
                    postSLActiveButUnconfirmed ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                    postSLConfirmedButStartupSettling ? 1 : 0, effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                    postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0);
            }
        }

        if (!pureObserverOnly && requestedDisabled && !setOptionsCallSuppressed) {
            const bool clearedAcceptedRuntimeOff =
                g_AcceptedRuntimeOffAwaitingSetOptions.exchange(false, std::memory_order_acq_rel);
            if (clearedAcceptedRuntimeOff) {
                HookLogImportant(
                    "Streamline Hook: Matching slDLSSGSetOptions(OFF) reached Streamline successfully — cleared "
                    "accepted-runtime-OFF latch (viewport=%u)",
                    viewportKey);
            }
        }

        const int effectiveMultiplier = GetEffectiveMultiplier(adjustedOptions);

        if (overrideApplied || overrideClamped) {
            if (capabilityMax > 0) {
                HookLog("Streamline Hook: Overrode DLSS-G viewport=%u mode=%s generatedFrames=%u->%u (%dx, max=%u)",
                        viewportKey, GetDLSSGModeName(adjustedOptions.mode), originalGeneratedFrames,
                        adjustedOptions.numFramesToGenerate, effectiveMultiplier, capabilityMax);
            } else {
                HookLog("Streamline Hook: Overrode DLSS-G viewport=%u mode=%s generatedFrames=%u->%u (%dx)",
                        viewportKey, GetDLSSGModeName(adjustedOptions.mode), originalGeneratedFrames,
                        adjustedOptions.numFramesToGenerate, effectiveMultiplier);
            }
        }
    } else if (overrideApplied || overrideClamped) {
        HookLogImportant("Streamline Hook: DLSS-G override failed viewport=%u mode=%s generatedFrames=%u->%u result=%d",
                         viewportKey, GetDLSSGModeName(adjustedOptions.mode), originalGeneratedFrames,
                         adjustedOptions.numFramesToGenerate, result);
    }

    return result;
}

// Safe no-op stub for SL function pointers that SL returned as NULL during
// re-entrant calls.  Steam's OverlayHookD3D3 may call slGetFeatureFunction
// from within SL's execution context (during DllMain or FG processing).
// If SL returns NULL, Steam calls through the NULL pointer → RIP=0 crash.
// Instead of returning an error (which Steam may ignore while still using the
// NULL pointer), substitute a safe stub that returns success and does nothing.
// This allows Steam to continue overlay rendering without crashing.
static slResult SlNullFunctionStub() {
    return kSlResultOk;
}

void* Hooked_slGetPluginFunction(const char* functionName) {
    if (StreamlineHook::IsExternalOverlayPresentGuardActive()) {
        static std::atomic<int> s_externalOverlaySuppressedPluginLookupLogCount{0};
        const int logCount = s_externalOverlaySuppressedPluginLookupLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "Streamline Hook: Suppressing re-entrant slGetPluginFunction during guarded external overlay "
                "Present (name=%s depth=%d)",
                functionName ? functionName : "null", g_ExternalOverlayPresentGuardDepth);
        }
        return reinterpret_cast<void*>(SlNullFunctionStub);
    }

    auto originalGetPluginFunction = GetCallableOriginalGetPluginFunction();
    if (!originalGetPluginFunction) {
        return g_Original_slGetPluginFunction ? reinterpret_cast<void*>(SlNullFunctionStub) : nullptr;
    }

    return originalGetPluginFunction(functionName);
}

slResult Hooked_slGetFeatureFunction(uint32_t feature, const char* functionName, void*& function) {
    if (StreamlineHook::IsExternalOverlayPresentGuardActive()) {
        static std::atomic<int> s_externalOverlaySuppressedLookupLogCount{0};
        const int logCount = s_externalOverlaySuppressedLookupLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "Streamline Hook: Suppressing re-entrant slGetFeatureFunction during guarded external overlay "
                "Present (feature=%u name=%s depth=%d)",
                feature, functionName ? functionName : "null", g_ExternalOverlayPresentGuardDepth);
        }
        function = reinterpret_cast<void*>(SlNullFunctionStub);
        return kSlResultErrorInvalidState;
    }

    auto originalGetFeatureFunction = GetCallableOriginalGetFeatureFunction();
    if (!originalGetFeatureFunction) {
        function = reinterpret_cast<void*>(SlNullFunctionStub);
        return kSlResultErrorInvalidState;
    }

    const slResult result = originalGetFeatureFunction(feature, functionName, function);
    // Safety: if the original returned success but gave us NULL, the caller
    // would call through NULL → RIP=0 crash.  This can happen when third-party
    // overlays (e.g., Steam's OverlayHookD3D3) call slGetFeatureFunction
    // re-entrantly from within Streamline's own code during FG processing.
    // Substitute a safe no-op stub so the caller doesn't crash even if it
    // ignores the error return and uses the function pointer directly.
    if (result == kSlResultOk && !function) {
        static std::atomic<int> s_nullFunctionLogCount{0};
        if (s_nullFunctionLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            HookLogImportant(
                "Streamline Hook: slGetFeatureFunction returned OK with NULL function "
                "(feature=%u name=%s) — substituting safe no-op stub to prevent null call crash",
                feature, functionName ? functionName : "null");
        }
        function = reinterpret_cast<void*>(SlNullFunctionStub);
        return kSlResultOk;
    }
    if (result != kSlResultOk || !functionName || !function) {
        return result;
    }

    // DLSS Frame Generation feature hooks
    if (feature == kSLFeatureDLSSG) {
        if (strcmp(functionName, "slDLSSGSetOptions") == 0) {
            void* originalFunction = function;
            const bool hookReady = MaybeHookDLSSGSetOptions(function, true);
            LogFeatureLookupOutcomeOnce(g_DLSSGSetOptionsLookupLogged, "slDLSSGSetOptions", originalFunction, function,
                                        hookReady);
        } else if (strcmp(functionName, "slDLSSGGetState") == 0) {
            void* originalFunction = function;
            const bool hookReady = MaybeHookDLSSGGetState(function, true);
            LogFeatureLookupOutcomeOnce(g_DLSSGGetStateLookupLogged, "slDLSSGGetState", originalFunction, function,
                                        hookReady);
            // Talos resolves GetState shortly before it starts tagging the activation inputs, but
            // never resolves/calls SetOptions. Arm standby at pointer delivery, before those tags.
            if (!ShouldKeepPureObserverOnlyStreamlineBehavior() &&
                g_StreamlineUsesD3D12.load(std::memory_order_acquire)) {
                ce::dx12_streamline_ui_overlay::BeginPreactivationStandby(2);
            }
        }
    }
    // Reflex feature hook — detect game activation of native Reflex
    else if (feature == kSLFeatureReflex) {
        if (strcmp(functionName, "slReflexSleep") == 0) {
            void* originalFunction = function;
            const bool hookReady = MaybeHookReflexSleep(function, true);
            LogFeatureLookupOutcomeOnce(g_ReflexSleepLookupLogged, "slReflexSleep", originalFunction, function,
                                        hookReady);
        } else if (strcmp(functionName, "slReflexSetOptions") == 0) {
            void* originalFunction = function;
            const bool hookReady = MaybeHookReflexSetOptions(function, true);
            LogFeatureLookupOutcomeOnce(g_ReflexSetOptionsLookupLogged, "slReflexSetOptions", originalFunction,
                                        function, hookReady);
        } else if (strcmp(functionName, "slReflexSetConstants") == 0) {
            void* originalFunction = function;
            const bool hookReady = MaybeHookReflexSetConstants(function, true);
            LogFeatureLookupOutcomeOnce(g_ReflexSetConstantsLookupLogged, "slReflexSetConstants", originalFunction,
                                        function, hookReady);
        }
    }

    return result;
}

slResult Hooked_slSetD3DDevice(void* d3dDevice) {
    auto originalSetD3DDevice = GetCallableOriginalSetD3DDevice();
    if (!originalSetD3DDevice) {
        return kSlResultErrorInvalidState;
    }

    ID3D12Device* acceptedD3D12Device = nullptr;
    if (d3dDevice) {
        static_cast<IUnknown*>(d3dDevice)->QueryInterface(IID_PPV_ARGS(&acceptedD3D12Device));
    }
    const bool isD3D12 = acceptedD3D12Device != nullptr;

    const slResult result = originalSetD3DDevice(d3dDevice);
    if (result == kSlResultOk) {
        ID3D12Device* previousAcceptedDevice = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_AcceptedD3D12DeviceMutex);
            previousAcceptedDevice = g_AcceptedD3D12Device;
            g_AcceptedD3D12Device = acceptedD3D12Device;
            acceptedD3D12Device = nullptr;
        }
        if (previousAcceptedDevice) {
            previousAcceptedDevice->Release();
        }
        g_StreamlineUsesD3D12.store(isD3D12, std::memory_order_release);
        if (isD3D12 && !ShouldKeepPureObserverOnlyStreamlineBehavior()) {
            // Resource tags are legal immediately after Streamline accepts the device. Some
            // integrations (Talos) publish their reusable UI tag before resolving any DLSS-G
            // feature function, so GetState-pointer delivery is too late to cover that tag.
            ce::dx12_streamline_ui_overlay::BeginPreactivationStandby(2);
            HookLogImportant(
                "Streamline Hook: D3D12 device accepted — official UI preactivation standby ready before tags "
                "(device=%p)",
                d3dDevice);
        } else if (!isD3D12) {
            ce::dx12_streamline_ui_overlay::EndPreactivationStandby("Streamline device is not D3D12");
        }
        TryResolveDLSSGFeatureHooks();
        TryResolveReflexFeatureHooks();
    }
    if (acceptedD3D12Device) {
        acceptedD3D12Device->Release();
    }
    return result;
}

bool StructTypesEqual(const slStructType& lhs, const slStructType& rhs) {
    return lhs.data1 == rhs.data1 && lhs.data2 == rhs.data2 && lhs.data3 == rhs.data3 &&
           std::memcmp(lhs.data4, rhs.data4, sizeof(lhs.data4)) == 0;
}

bool TryRecordOfficialUiResourceTag(const void* frameToken, const slResourceTag& tag, void* commandBuffer) {
    if (ShouldKeepPureObserverOnlyStreamlineBehavior() || !g_StreamlineUsesD3D12.load(std::memory_order_acquire) ||
        !commandBuffer || tag.type != kSLBufferTypeUIColorAndAlpha || !tag.resource || !tag.resource->native ||
        tag.resource->type != slResourceType::kTexture2D || tag.extent.top != 0 || tag.extent.left != 0) {
        return false;
    }

    auto* uiResource = static_cast<ID3D12Resource*>(tag.resource->native);
    const D3D12_RESOURCE_DESC desc = uiResource->GetDesc();
    const uint32_t width = tag.extent.width != 0
                               ? tag.extent.width
                               : (tag.resource->width != 0 ? tag.resource->width : static_cast<uint32_t>(desc.Width));
    const uint32_t height =
        tag.extent.height != 0 ? tag.extent.height : (tag.resource->height != 0 ? tag.resource->height : desc.Height);
    const DXGI_FORMAT format =
        tag.resource->nativeFormat != 0 ? static_cast<DXGI_FORMAT>(tag.resource->nativeFormat) : desc.Format;
    const bool hdr = format == DXGI_FORMAT_R10G10B10A2_UNORM || format == DXGI_FORMAT_R10G10B10A2_TYPELESS ||
                     format == DXGI_FORMAT_R16G16B16A16_FLOAT || format == DXGI_FORMAT_R16G16B16A16_TYPELESS;
    ID3D12CommandQueue* initializationQueue = DX12_AcquireOriginalGameQueueForOverlay();
    if (!initializationQueue) {
        return false;
    }

    ce::dx12_streamline_ui_overlay::RecordRequest request;
    request.commandList = static_cast<ID3D12GraphicsCommandList*>(commandBuffer);
    request.uiResource = uiResource;
    request.initializationQueue = initializationQueue;
    request.resourceState = static_cast<D3D12_RESOURCE_STATES>(tag.resource->state);
    request.format = format;
    request.width = width;
    request.height = height;
    request.hdr = hdr;
    request.frameToken = frameToken;
    const bool recorded = ce::dx12_streamline_ui_overlay::TryRecordBootstrap(request);
    initializationQueue->Release();
    return recorded;
}

uint32_t LogOfficialUiTagOpportunity(const char* tagApi, const void* frameToken, uint32_t viewportKey,
                                     const slResourceTag* tags, uint32_t numTags, void* commandBuffer,
                                     uint32_t feature = UINT_MAX, uint32_t numInputs = 0) {
    static std::atomic<uint32_t> s_uiTagOpportunityLogCount{0};
    const uint32_t opportunity = s_uiTagOpportunityLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (opportunity > 12 && (opportunity % 300) != 0) {
        return 0;
    }

    HookLogImportant(
        "Streamline Hook: Official UI tag record opportunity #%u (api=%s feature=%u frame=%p viewport=%u "
        "tags=%p numTags=%u inputs=%u commandBuffer=%p d3d12=%d)",
        opportunity, tagApi ? tagApi : "unknown", feature, frameToken, viewportKey, tags, numTags, numInputs,
        commandBuffer, g_StreamlineUsesD3D12.load(std::memory_order_relaxed) ? 1 : 0);
    const uint32_t loggedTags = tags ? std::min(numTags, 12u) : 0u;
    for (uint32_t i = 0; i < loggedTags; ++i) {
        const slResourceTag& tag = tags[i];
        HookLogImportant(
            "Streamline Hook: UI tag opportunity #%u tag[%u] type=%u lifecycle=%d resource=%p "
            "extent=(%u,%u %ux%u)",
            opportunity, i, tag.type, tag.lifecycle, tag.resource, tag.extent.left, tag.extent.top, tag.extent.width,
            tag.extent.height);
    }
    return opportunity;
}

void TryRecordOfficialUiTag(const char* tagApi, const void* frameToken, const slViewportHandle& viewport,
                            const slResourceTag* tags, uint32_t numTags, void* commandBuffer) {
    const bool wantsUiBootstrapRecord = ce::dx12_streamline_ui_overlay::OnFrameTag(frameToken);

    if (wantsUiBootstrapRecord) {
        LogOfficialUiTagOpportunity(tagApi, frameToken, GetViewportKey(viewport), tags, numTags, commandBuffer);
    }

    // DLSS-G consumes UIColorAndAlpha before its first generated output exists, while PostSL can
    // only run after that output has been produced. Record CE's rolling/one-shot overlay into the
    // official UI layer on the app-provided command list. This introduces no copy, extra submission,
    // queue, or wait and naturally follows Streamline's own synchronization.
    if (wantsUiBootstrapRecord && tags) {
        for (uint32_t i = 0; i < numTags; ++i) {
            if (TryRecordOfficialUiResourceTag(frameToken, tags[i], commandBuffer)) {
                break;
            }
        }
    }
}

slResult Hooked_slSetTag(const slViewportHandle& viewport, const slResourceTag* tags, uint32_t numTags,
                         void* commandBuffer) {
    auto originalSetTag = GetCallableOriginalSetTag();
    if (!originalSetTag) {
        return kSlResultErrorInvalidState;
    }

    // Legacy/global resource tagging has no frame token. A monotonically unique opaque identity
    // lets the standby state roll across calls without dereferencing or fabricating an SL object.
    static std::atomic<uintptr_t> s_legacyTagToken{1};
    const uintptr_t tokenValue = s_legacyTagToken.fetch_add(1, std::memory_order_relaxed);
    const void* frameToken = reinterpret_cast<const void*>((tokenValue << 1u) | 1u);
    TryRecordOfficialUiTag("slSetTag", frameToken, viewport, tags, numTags, commandBuffer);

    return originalSetTag(viewport, tags, numTags, commandBuffer);
}

slResult Hooked_slSetTagForFrame(const slBaseStructure& frame, const slViewportHandle& viewport,
                                 const slResourceTag* tags, uint32_t numTags, void* commandBuffer) {
    auto originalSetTagForFrame = GetCallableOriginalSetTagForFrame();
    if (!originalSetTagForFrame) {
        return kSlResultErrorInvalidState;
    }

    TryRecordOfficialUiTag("slSetTagForFrame", &frame, viewport, tags, numTags, commandBuffer);

    // Streamline observes the resource only after CE's commands have been appended. For volatile
    // tags this is essential: any copy Streamline records into the same command list includes CE.
    return originalSetTagForFrame(frame, viewport, tags, numTags, commandBuffer);
}

slResult Hooked_slEvaluateFeature(uint32_t feature, const slBaseStructure& frame, const slBaseStructure** inputs,
                                  uint32_t numInputs, void* commandBuffer) {
    auto originalEvaluateFeature = GetCallableOriginalEvaluateFeature();
    if (!originalEvaluateFeature) {
        return kSlResultErrorInvalidState;
    }

    // Streamline explicitly permits ResourceTag structures as local evaluate inputs; those tags
    // never pass through slSetTag/slSetTagForFrame. Talos uses this route. Keep the steady-state
    // evaluate path to one atomic branch, then inspect only the short activation/standby window.
    const void* frameToken = &frame;
    const bool wantsUiBootstrapRecord = ce::dx12_streamline_ui_overlay::OnFrameTag(frameToken);
    if (wantsUiBootstrapRecord) {
        slViewportHandle viewport;
        uint32_t localTagCount = 0;
        constexpr uint32_t kMaximumInputChainDepth = 16;
        if (inputs) {
            for (uint32_t i = 0; i < numInputs; ++i) {
                const slBaseStructure* input = inputs[i];
                for (uint32_t depth = 0; input && depth < kMaximumInputChainDepth; ++depth) {
                    if (StructTypesEqual(input->structType, kViewportHandleStructType)) {
                        viewport.value = static_cast<const slViewportHandle*>(input)->value;
                    }
                    if (StructTypesEqual(input->structType, kResourceTagStructType)) {
                        ++localTagCount;
                    }
                    const slBaseStructure* next = input->next;
                    if (next == input) {
                        break;
                    }
                    input = next;
                }
            }
        }

        const uint32_t opportunity =
            LogOfficialUiTagOpportunity("slEvaluateFeature", frameToken, GetViewportKey(viewport), nullptr,
                                        localTagCount, commandBuffer, feature, numInputs);
        uint32_t tagIndex = 0;
        bool recorded = false;
        if (inputs) {
            for (uint32_t i = 0; i < numInputs && !recorded; ++i) {
                const slBaseStructure* input = inputs[i];
                for (uint32_t depth = 0; input && depth < kMaximumInputChainDepth; ++depth) {
                    if (StructTypesEqual(input->structType, kResourceTagStructType)) {
                        const auto& tag = *static_cast<const slResourceTag*>(input);
                        if (opportunity != 0 && tagIndex < 12) {
                            HookLogImportant(
                                "Streamline Hook: UI tag opportunity #%u localTag[%u] input=%u depth=%u type=%u "
                                "lifecycle=%d resource=%p extent=(%u,%u %ux%u)",
                                opportunity, tagIndex, i, depth, tag.type, tag.lifecycle, tag.resource, tag.extent.left,
                                tag.extent.top, tag.extent.width, tag.extent.height);
                        }
                        ++tagIndex;
                        recorded = TryRecordOfficialUiResourceTag(frameToken, tag, commandBuffer);
                    }
                    const slBaseStructure* next = input->next;
                    if (next == input) {
                        break;
                    }
                    input = next;
                }
            }
        }
    }

    // Append CE before Streamline observes/copies any eOnlyValidNow/eValidUntilEvaluate UI tag.
    return originalEvaluateFeature(feature, frame, inputs, numInputs, commandBuffer);
}

// Hook for Streamline Reflex sleep. This lets CE observe game-owned Reflex
// pacing without patching NvAPI_D3D_Sleep inside nvapi64.dll.
slResult Hooked_slReflexSleep(const void* frame) {
    auto originalReflexSleep = GetCallableOriginalReflexSleep();
    if (!originalReflexSleep) {
        return kSlResultErrorInvalidState;
    }

    // DLSSG-health evidence only: relaxed atomics + GetTickCount64 (shared-page read). No locks, no
    // logging, no syscalls — the manual Reflex FPS limiter's latency path through this hook is unchanged.
    g_ReflexSleepObservedCount.fetch_add(1, std::memory_order_relaxed);
    g_ReflexSleepLastTickMs.store(GetTickCount64(), std::memory_order_relaxed);

    g_ReflexLimiter.ApplyHybridPacingBeforeNativeSleep();

    const slResult result = originalReflexSleep(frame);
    if (result == kSlResultOk) {
        g_ReflexLimiter.MarkGameSleep("Streamline");
        g_ReflexLimiter.MarkNativePacingSignal();
    } else {
        static std::atomic<int> s_reflexSleepFailLogCount{0};
        const int failCount = s_reflexSleepFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (failCount < 5) {
            HookLogImportant("Streamline Hook: slReflexSleep forward failed result=%d frame=%p", result, frame);
        }
    }
    return result;
}

// Hook for current Streamline Reflex options — detects low-latency and FPS limiter signals.
slResult Hooked_slReflexSetOptions(const slReflexOptions& options) {
    auto originalReflexSetOptions = GetCallableOriginalReflexSetOptions();
    if (!originalReflexSetOptions) {
        return kSlResultErrorInvalidState;
    }

    slReflexOptions adjustedOptions = options;
    g_ReflexSetOptionsObservedCount.fetch_add(1, std::memory_order_relaxed);
    g_ReflexSetOptionsLastTickMs.store(GetTickCount64(), std::memory_order_relaxed);
    g_ReflexLastForwardedMode.store(options.mode, std::memory_order_relaxed);
    const uint32_t targetIntervalUs = g_ReflexLimiter.GetTargetIntervalUs();
    const auto frameLimitForwarding = ce::streamline_runtime_policy::ResolveStreamlineReflexFrameLimitForwarding(
        options.frameLimitUs, targetIntervalUs);
    adjustedOptions.frameLimitUs = frameLimitForwarding.frameLimitUs;
    HandleStreamlineReflexPacingSignal("slReflexSetOptions", options.mode, options.frameLimitUs,
                                       adjustedOptions.frameLimitUs, targetIntervalUs);

    const slResult result = originalReflexSetOptions(adjustedOptions);
    if (result == kSlResultOk && frameLimitForwarding.overrideApplied) {
        HookLogImportant(
            "Streamline Hook: Overrode Reflex options frameLimitUs %u->%u (mode=%d incomingActive=%d)",
            options.frameLimitUs, adjustedOptions.frameLimitUs, adjustedOptions.mode,
            ce::streamline_runtime_policy::IsStreamlineReflexPacingSignalActive(options.mode, options.frameLimitUs)
                ? 1
                : 0);
    } else if (result != kSlResultOk) {
        static std::atomic<int> s_reflexSetOptionsFailLogCount{0};
        const int failCount = s_reflexSetOptionsFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (failCount < 5) {
            HookLogImportant("Streamline Hook: slReflexSetOptions forward failed result=%d mode=%d frameLimitUs=%u",
                             result, options.mode, options.frameLimitUs);
        }
    }
    return result;
}

// Hook for legacy slReflexSetConstants — detects when game activates Reflex via Streamline.
slResult Hooked_slReflexSetConstants(const SLReflexConstants& consts) {
    auto originalReflexSetConstants = GetCallableOriginalReflexSetConstants();
    if (!originalReflexSetConstants) {
        return kSlResultErrorInvalidState;
    }

    SLReflexConstants adjustedConsts = consts;
    const uint32_t targetIntervalUs = g_ReflexLimiter.GetTargetIntervalUs();

    // The legacy constants path only receives CE's frame-limit override when Reflex
    // is actually active, preserving the existing native Streamline behavior.
    if (consts.mode >= kSLReflexModeEnabled) {
        const auto frameLimitForwarding = ce::streamline_runtime_policy::ResolveStreamlineReflexFrameLimitForwarding(
            consts.frameLimitUs, targetIntervalUs);
        adjustedConsts.frameLimitUs = frameLimitForwarding.frameLimitUs;
    }
    HandleStreamlineReflexPacingSignal("slReflexSetConstants", consts.mode, consts.frameLimitUs,
                                       adjustedConsts.frameLimitUs, targetIntervalUs);

    // Forward to the real slReflexSetConstants
    const slResult result = originalReflexSetConstants(adjustedConsts);
    if (result == kSlResultOk && adjustedConsts.frameLimitUs != consts.frameLimitUs) {
        HookLogImportant("Streamline Hook: Overrode Reflex constants frameLimitUs %u->%u (mode=%d)",
                         consts.frameLimitUs, adjustedConsts.frameLimitUs, adjustedConsts.mode);
    } else if (result != kSlResultOk) {
        static std::atomic<int> s_reflexSetConstantsFailLogCount{0};
        const int failCount = s_reflexSetConstantsFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (failCount < 5) {
            HookLogImportant("Streamline Hook: slReflexSetConstants forward failed result=%d mode=%d frameLimitUs=%u",
                             result, consts.mode, consts.frameLimitUs);
        }
    }
    return result;
}

}  // namespace

namespace StreamlineHook {

ExternalOverlayPresentGuard::ExternalOverlayPresentGuard() {
    ++g_ExternalOverlayPresentGuardDepth;
}

ExternalOverlayPresentGuard::~ExternalOverlayPresentGuard() {
    if (g_ExternalOverlayPresentGuardDepth > 0) {
        --g_ExternalOverlayPresentGuardDepth;
    }
}

bool IsExternalOverlayPresentGuardActive() {
    return g_ExternalOverlayPresentGuardDepth > 0;
}

bool IsExternalOverlayPluginLookupGuardReady() {
    return g_SLGetPluginFunctionHooked.load(std::memory_order_acquire);
}

bool IsAcceptedD3D12Device(IUnknown* device) {
    if (!device) {
        return false;
    }

    IUnknown* candidateIdentity = nullptr;
    IUnknown* acceptedIdentity = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&candidateIdentity))) || !candidateIdentity) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_AcceptedD3D12DeviceMutex);
        if (g_AcceptedD3D12Device) {
            g_AcceptedD3D12Device->QueryInterface(IID_PPV_ARGS(&acceptedIdentity));
        }
    }
    const bool matches = acceptedIdentity && candidateIdentity == acceptedIdentity;
    if (acceptedIdentity) {
        acceptedIdentity->Release();
    }
    candidateIdentity->Release();
    return matches;
}

bool HasExplicitSetOptionsActivationForCurrentComeback() {
    // Provenance of the current comeback is tracked explicitly. Startup-window
    // OFF churn can temporarily re-arm provisional GetState suppression without
    // changing the fact that the live comeback itself was activated by a fresh
    // OFF->ON SetOptions edge.
    return g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
}

void Init() {
    std::lock_guard<std::mutex> lock(g_InitMutex);
    RegisterDynamicHooksOnce();

    const bool foundModule = ScanLoadedStreamlineModules();

    if (!foundModule) {
        if (!g_NoModulesLogged.exchange(true, std::memory_order_acq_rel)) {
            HookLog("Streamline Hook: No Streamline modules loaded yet; waiting for module load");
        }
    } else {
        g_NoModulesLogged.store(false, std::memory_order_release);
    }
}

void OnModuleUnloaded(const void* moduleBase, size_t moduleSizeBytes, const char* moduleBaseName) {
    if (!moduleBase || moduleSizeBytes == 0 ||
        !ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload(moduleBaseName)) {
        return;
    }

    // Runs under the loader lock: interlocked/atomic writes and lightweight
    // logging only. Do NOT take g_ModuleHookMutex here (InstallHooksForModule
    // holds it across GetProcAddress, which needs the loader lock).
    struct HookSlotView {
        const char* name;
        std::atomic<void*>* target;
        std::atomic<bool>* installed;
        void* volatile* original;
    };
    HookSlotView slots[] = {
        {"slGetFeatureFunction", &g_SLGetFeatureFunctionTarget, &g_SLGetFeatureFunctionHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slGetFeatureFunction)},
        {"slGetPluginFunction", &g_SLGetPluginFunctionTarget, &g_SLGetPluginFunctionHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slGetPluginFunction)},
        {"slSetD3DDevice", &g_SLSetD3DDeviceTarget, &g_SLSetD3DDeviceHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slSetD3DDevice)},
        {"slSetTag", &g_SLSetTagTarget, &g_SLSetTagHooked, reinterpret_cast<void* volatile*>(&g_Original_slSetTag)},
        {"slSetTagForFrame", &g_SLSetTagForFrameTarget, &g_SLSetTagForFrameHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slSetTagForFrame)},
        {"slEvaluateFeature", &g_SLEvaluateFeatureTarget, &g_SLEvaluateFeatureHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slEvaluateFeature)},
        {"slDLSSGSetOptions", &g_DLSSGSetOptionsTarget, &g_DLSSGSetOptionsHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slDLSSGSetOptions)},
        {"slDLSSGGetState", &g_DLSSGGetStateTarget, &g_DLSSGGetStateHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slDLSSGGetState)},
        {"slReflexSleep", &g_ReflexSleepTarget, &g_ReflexSleepHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slReflexSleep)},
        {"slReflexSetOptions", &g_ReflexSetOptionsTarget, &g_ReflexSetOptionsHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slReflexSetOptions)},
        {"slReflexSetConstants", &g_ReflexSetConstantsTarget, &g_ReflexSetConstantsHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slReflexSetConstants)},
    };

    int invalidatedSlots = 0;
    for (HookSlotView& slot : slots) {
        void* target = slot.target->load(std::memory_order_acquire);
        void* original = *slot.original;
        if (!ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(target, original, moduleBase,
                                                                                          moduleSizeBytes)) {
            continue;
        }
        // Clear the original first so detours fail safe (GetCallableOriginal*
        // returns null) before the installed flag re-arms installation.
        InterlockedExchangePointer(slot.original, nullptr);
        slot.target->store(nullptr, std::memory_order_release);
        slot.installed->store(false, std::memory_order_release);
        ++invalidatedSlots;
        HookLogImportant(
            "Streamline Hook: Invalidated %s hook slot for unloaded %s (target=%p original=%p base=%p size=0x%zX)",
            slot.name, moduleBaseName, target, original, moduleBase, moduleSizeBytes);
    }

    std::atomic<void*>* attemptedTargets[] = {
        &g_DLSSGSetOptionsImportFallbackAttemptedTarget,    &g_DLSSGGetStateImportFallbackAttemptedTarget,
        &g_ReflexSleepImportFallbackAttemptedTarget,        &g_ReflexSetOptionsImportFallbackAttemptedTarget,
        &g_ReflexSetConstantsImportFallbackAttemptedTarget,
    };
    for (std::atomic<void*>* attempted : attemptedTargets) {
        void* target = attempted->load(std::memory_order_acquire);
        if (ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(target, nullptr, moduleBase,
                                                                                         moduleSizeBytes)) {
            attempted->store(nullptr, std::memory_order_release);
        }
    }

    const uint32_t moduleBit = GetModuleMaskBit(moduleBaseName);
    if (moduleBit != 0) {
        g_InstalledModuleMask.fetch_and(~moduleBit, std::memory_order_acq_rel);
        g_IATPatchesMask.fetch_and(~moduleBit, std::memory_order_acq_rel);
    }

    if (invalidatedSlots > 0 || moduleBit != 0) {
        HookLogImportant(
            "Streamline Hook: Module %s unloaded (base=%p size=0x%zX) — invalidated %d stale hook slot(s); the next "
            "load of this name re-installs hooks for the fresh instance",
            moduleBaseName, moduleBase, moduleSizeBytes, invalidatedSlots);
    }
}

void OnModuleLoaded(HMODULE module, const char* moduleNameOrPath) {
    if (!module || !ce::streamline_runtime_policy::ShouldInspectStreamlineModuleOnLoad(moduleNameOrPath)) {
        return;
    }

    g_NoModulesLogged.store(false, std::memory_order_release);
    const bool inspectedModule = InstallHooksForModule(module, moduleNameOrPath);
    bool resolvedDLSSG = false;
    bool resolvedReflex = false;
    const bool deferFeatureLookup =
        ce::streamline_runtime_policy::ShouldDeferStreamlineFeatureLookupDuringModuleLoad(true);
    if (!deferFeatureLookup) {
        resolvedDLSSG = TryResolveDLSSGFeatureHooks();
        resolvedReflex = TryResolveReflexFeatureHooks();
    } else if (inspectedModule) {
        static std::atomic<int> s_deferredLookupLogCount{0};
        const int logCount = s_deferredLookupLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 20 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Deferred proactive feature-function lookup during module load for %s (%p) "
                "(log=%d); direct exports/IAT hooks installed now, feature lookup will retry after slSetD3DDevice or "
                "app slGetFeatureFunction",
                GetModuleBaseName(moduleNameOrPath), module, logCount);
        }
    }

    if (inspectedModule || resolvedDLSSG || resolvedReflex) {
        HookLogImportant(
            "Streamline Hook: Fresh module load inspected %s (%p) "
            "slGetFeatureFunctionHooked=%d slGetPluginFunctionHooked=%d slSetD3DDeviceHooked=%d "
            "slSetTagHooked=%d slSetTagForFrameHooked=%d slEvaluateFeatureHooked=%d "
            "dlssgSetOptionsHooked=%d "
            "dlssgGetStateHooked=%d reflexSleepHooked=%d reflexSetOptionsHooked=%d reflexSetConstantsHooked=%d",
            GetModuleBaseName(moduleNameOrPath), module,
            g_SLGetFeatureFunctionHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_SLGetPluginFunctionHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_SLSetD3DDeviceHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_SLSetTagHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_SLSetTagForFrameHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_SLEvaluateFeatureHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_DLSSGGetStateHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ? 1 : 0);
    }
}

bool IsInitialized() {
    return g_DynamicHooksRegistered.load(std::memory_order_acquire);
}

bool IsDLSSFGRequestedViaStreamline() {
    return DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
}

void OnAuthoritativeFFXTakeover() {
    ce::dx12_streamline_ui_overlay::EndActivation("authoritative FFX takeover");
    size_t resetViewportCount = 0;
    size_t preservedCapabilityCount = 0;
    {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        resetViewportCount = g_ViewportStates.size();
        preservedCapabilityCount = g_ViewportCapabilityMax.size();
        for (auto& [viewportKey, runtimeState] : g_ViewportStates) {
            runtimeState.active = false;
            runtimeState.multiplier = 0;
            runtimeState.generatedFrames = 0;
        }
    }

    {
        std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);
        if (g_SuppressedSetOptionsOffDuringStartup) {
            HookLogImportant(
                "Streamline Hook: Clearing suppressed slDLSSGSetOptions(OFF) due to authoritative FFX takeover");
            g_SuppressedSetOptionsOffDuringStartup = false;
        }
    }

    g_SuppressNewGetStateActivationUntilMs.store(GetTickCount64() + kAuthoritativeFFXTakeoverGetStateSuppressMs,
                                                 std::memory_order_release);
    g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap.store(true, std::memory_order_release);
    g_CurrentComebackActivatedViaExplicitSetOptions.store(false, std::memory_order_release);
    g_AcceptedRuntimeOffAwaitingSetOptions.store(false, std::memory_order_release);
    g_ConfirmedDLSSReflexSuspendPending.store(false, std::memory_order_release);
    g_StartupWindowOffExtensionPending.store(false, std::memory_order_release);
    ResetStartupProtectedOffChurnActiveProof("authoritative FFX takeover");
    // A new FSR takeover resets the entire FG session context; any stale DLSS-only
    // reactivation block from a previous epoch must not outlive the FSR phase.
    const bool hadStaleExplicitSetOptionsBlock =
        g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.exchange(false, std::memory_order_acq_rel);
    HookLogImportant(
        "Streamline Hook: Authoritative FFX takeover reset %zu viewport states and preserved %zu capability caches; "
        "suppressing GetState-only reactivation for %llums and until safe post-FSR bootstrap or explicit enable "
        "(clearedStaleBlock=%d)",
        resetViewportCount, preservedCapabilityCount, (unsigned long long)kAuthoritativeFFXTakeoverGetStateSuppressMs,
        hadStaleExplicitSetOptionsBlock ? 1 : 0);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kAuthoritativeFFXTakeover,
                                "StreamlineHook::OnAuthoritativeFFXTakeover", nullptr, nullptr,
                                ce::fg_runtime::RuntimeMode::kFSRFG, true, true);
}

void OnAuthoritativeStreamlineStartupHandoff() {
    g_SuppressNewGetStateActivationUntilMs.store(GetTickCount64() + kAuthoritativeFFXTakeoverGetStateSuppressMs,
                                                 std::memory_order_release);
    g_ConfirmedDLSSReflexSuspendPending.store(false, std::memory_order_release);
    g_StartupWindowOffExtensionPending.store(true, std::memory_order_release);
    ResetStartupProtectedOffChurnActiveProof("authoritative Streamline startup handoff");
    HookLogImportant(
        "Streamline Hook: Authoritative Streamline startup handoff observed — suppressing fresh GetState-only "
        "reactivation for %llums until explicit enable or stable startup evidence arrives",
        (unsigned long long)kAuthoritativeFFXTakeoverGetStateSuppressMs);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kAuthoritativeStreamlineStartupHandoff,
                                "StreamlineHook::OnAuthoritativeStreamlineStartupHandoff", nullptr, nullptr,
                                ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false);
}

void Shutdown() {
    ce::dx12_streamline_ui_overlay::EndActivation("Streamline shutdown");
    std::lock_guard<std::mutex> lock(g_StateMutex);
    g_ViewportStates.clear();
    g_ViewportCapabilityMax.clear();
    g_SuppressNewGetStateActivationUntilMs.store(0, std::memory_order_release);
    g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.store(false, std::memory_order_release);
    g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap.store(false, std::memory_order_release);
    g_CurrentComebackActivatedViaExplicitSetOptions.store(false, std::memory_order_release);
    g_AcceptedRuntimeOffAwaitingSetOptions.store(false, std::memory_order_release);
    g_ConfirmedDLSSReflexSuspendPending.store(false, std::memory_order_release);
    g_StartupWindowOffExtensionPending.store(false, std::memory_order_release);
    ResetStartupProtectedOffChurnActiveProof("Streamline shutdown");
    {
        std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);
        g_SuppressedSetOptionsOffDuringStartup = false;
    }
    g_FGCompat.SetStreamlineFGSignal(false);
    g_FGCompat.SetDLSSFGMultiplier(0);
    g_FGCompat.SetDLSSFGActive(false);
    g_FGCompat.SetStreamlineSupportPresent(false);
    DXGIShared::g_StreamlineFGRunning.store(false, std::memory_order_release);
    g_StreamlineUsesD3D12.store(false, std::memory_order_release);
    ID3D12Device* acceptedDevice = nullptr;
    {
        std::lock_guard<std::mutex> deviceLock(g_AcceptedD3D12DeviceMutex);
        acceptedDevice = g_AcceptedD3D12Device;
        g_AcceptedD3D12Device = nullptr;
    }
    if (acceptedDevice) {
        acceptedDevice->Release();
    }
}

void FlushSuppressedSetOptionsOffIfNeeded() {
    if (ShouldKeepPureObserverOnlyStreamlineBehavior()) {
        return;
    }

    std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);

    const bool windowStillActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
    const bool activationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool callbackInstalled = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr;
    const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
    const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
    const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
    const bool postSLConfirmedButStartupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
    const bool postSLConfirmedButRuntimeStateStabilizing = HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing() ||
                                                           HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected();
    const bool explicitSetOptionsActivationForCurrentComeback =
        g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
    const bool hadFSRFGPhase = HookHasFSRFGHistory();
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    const bool startupProtectedComebackProof =
        explicitSetOptionsActivationForCurrentComeback || safePostFSRBootstrapPath;
    const bool postSLConfirmedButOffChurnAwaitingActiveProof = IsStartupProtectedOffChurnAwaitingActiveProof(
        startupProtectedComebackProof, postSLConfirmedRendering, postSLConfirmedButStartupSettling);
    const bool effectivePostSLRuntimeStateStabilizing =
        postSLConfirmedButRuntimeStateStabilizing || postSLConfirmedButOffChurnAwaitingActiveProof;
    const bool acceptActivatedUnconfirmedResumeOff =
        ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
            true, windowStillActive, startupProtectedComebackProof, activationPending, postSLActiveButUnconfirmed,
            postSLStartupActivationEntered, postSLConfirmedRendering, postSLConfirmedButStartupSettling,
            effectivePostSLRuntimeStateStabilizing);
    const bool shouldKeepDeferred =
        !acceptActivatedUnconfirmedResumeOff &&
        ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
            windowStillActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
            activationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering, postSLConfirmedButStartupSettling,
            effectivePostSLRuntimeStateStabilizing);
    if (shouldKeepDeferred) {
        if (ce::dx12_overlay_policy::ShouldServicePostSLStartupActivationWhileOffChurnDeferred(
                shouldKeepDeferred, windowStillActive, activationPending, postSLStartupActivationEntered,
                callbackInstalled)) {
            const bool serviced = TryServicePostSLStartupActivation(
                "StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded deferred OFF churn", true);
            static std::atomic<int> s_deferredOffServiceLogCount{0};
            const int logCount = s_deferredOffServiceLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10 || (logCount % 100) == 0) {
                HookLogImportant(
                    "Streamline Hook: Startup-protected OFF churn serviced PostSL startup activation before "
                    "remaining deferred (serviced=%d pending=%d activeButUnconfirmed=%d "
                    "startupActivationEntered=%d)",
                    serviced ? 1 : 0, activationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                    postSLStartupActivationEntered ? 1 : 0);
            }
        }
        return;
    }

    const bool shouldTriggerDirectCallback =
        ce::streamline_runtime_policy::ShouldTriggerDirectPostSLCallbackAfterStartupWindowExpiry(
            activationPending, postSLStartupActivationEntered);

    auto logSkippedDirectCallbackAfterActivation = [&]() {
        static std::atomic<int> s_skipLogCount{0};
        const int logCount = s_skipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Startup window expired but PostSL startup activation callback already entered — "
                "skipping redundant direct callback until first confirmed render "
                "(activeButUnconfirmed=%d startupActivationEntered=%d)",
                postSLActiveButUnconfirmed ? 1 : 0, postSLStartupActivationEntered ? 1 : 0);
        }
    };

    // Case 1: Suppressed OFF exists — either forward it to Streamline for a real
    // inactive edge, or drop it if a newer post-FSR comeback is already
    // startup-protected and this OFF is now stale churn.
    if (g_SuppressedSetOptionsOffDuringStartup) {
        if (acceptActivatedUnconfirmedResumeOff) {
            LogAcceptedOffDuringActivatedUnconfirmedResume(
                "periodic suppressed-off flush", windowStillActive, hadFSRFGPhase,
                explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath, activationPending,
                postSLActiveButUnconfirmed, postSLStartupActivationEntered, postSLConfirmedRendering,
                postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
        }
        if (!acceptActivatedUnconfirmedResumeOff &&
            ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedStreamlineComeback(
                hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
                DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire), postSLConfirmedButStartupSettling,
                effectivePostSLRuntimeStateStabilizing)) {
            LogDroppedSuppressedOffForStartupProtectedStreamlineComeback(
                g_SuppressedOffViewportKey, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                safePostFSRBootstrapPath, activationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
                postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
            g_SuppressedSetOptionsOffDuringStartup = false;
            ResetStartupProtectedOffChurnActiveProof("dropped stale suppressed OFF after active proof");
        } else {
            auto originalSetOptions = GetCallableOriginalDLSSGSetOptions();
            if (!originalSetOptions) {
                return;
            }
            HookLogImportant(
                "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) via periodic flush — startup window "
                "expired (viewport=%u, activationPending=%d settling=%d stabilizing=%d activeProofPending=%d)",
                g_SuppressedOffViewportKey, activationPending ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
                effectivePostSLRuntimeStateStabilizing ? 1 : 0, postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0);
            const slResult offResult = originalSetOptions(g_SuppressedOffViewport, g_SuppressedOffOptions);
            if (offResult != kSlResultOk) {
                HookLogImportant("Streamline Hook: Forwarded slDLSSGSetOptions(OFF) via periodic flush returned %d",
                                 offResult);
            }
            g_SuppressedSetOptionsOffDuringStartup = false;
            ResetStartupProtectedOffChurnActiveProof("forwarded suppressed OFF after startup expiry");
        }

        // When the startup-handoff Present was promoted to top-level and bypassed
        // the synthetic Present path, the PostSL callback may never fire through
        // DetourPresent/DetourPresent1 — or it may be deferred by the startup
        // transition window guard.  In either case, activation remains pending.
        //
        // If activation is still pending, the suppressed OFF we just forwarded to
        // Streamline will tear down FG without PostSL ever completing.  Trigger the
        // PostSL callback directly so CE can at least attempt to complete activation
        // before Streamline receives the OFF signal and potentially destabilizes its
        // FG pipeline.
        //
        // We intentionally distinguish ProcessFrame pre-arming PostSL from the
        // startup callback actually entering.  Pre-armed-but-unentered still needs
        // the retained activation wake path; once the callback entered, repeated
        // direct callbacks stay blocked until the normal Present route confirms.
        //
        // This is critical for GTA V Enhanced DLSS FG startup, where only one
        // startup-handoff Present arrives via the top-level path (bypassing the
        // synthetic route), and then the game's present thread stalls inside Streamline
        // before any synthetic Presents can drive PostSL activation.
        if (shouldTriggerDirectCallback && callbackInstalled) {
            HookLogImportant(
                "Streamline Hook: Activation still pending after OFF flush — "
                "PostSL callback never entered (deferred or bypassed); trigger direct "
                "callback to attempt activation before Streamline processes OFF");
            const bool serviced = TryServicePostSLStartupActivation(
                "StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded after OFF flush", true);
            HookLogImportant("Streamline Hook: PostSL startup activation service after OFF flush returned %d",
                             serviced ? 1 : 0);
        } else if (activationPending && callbackInstalled && postSLStartupActivationEntered) {
            logSkippedDirectCallbackAfterActivation();
        }
        return;
    }

    // Case 2: No suppressed OFF, but startup window just expired and activation
    // is still pending.  This covers the scenario where ON re-arrived and cleared
    // the suppressed OFF before the window expired — ProcessFrame has stalled,
    // so the deferred PostSL callback in ProcessFrame will never fire.  Trigger
    // it here to complete activation before Streamline times out.
    if (shouldTriggerDirectCallback && callbackInstalled) {
        HookLogImportant(
            "Streamline Hook: Startup window expired with activation pending but no "
            "suppressed OFF — triggering PostSL callback directly to complete "
            "activation before Streamline times out");
        const bool serviced =
            TryServicePostSLStartupActivation("StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded expiry", true);
        HookLogImportant("Streamline Hook: PostSL startup activation service after startup expiry returned %d",
                         serviced ? 1 : 0);
    } else if (activationPending && callbackInstalled && postSLStartupActivationEntered) {
        logSkippedDirectCallbackAfterActivation();
    }
}

}  // namespace StreamlineHook
