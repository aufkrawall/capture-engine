#pragma once

namespace {
struct SLReflexConstants;
}

namespace {
struct slStructType;
}

namespace {
struct slBaseStructure;
}

namespace {
struct slViewportHandle;
}

namespace {
struct slExtent;
}

namespace {
struct slResource;
}

namespace {
struct slResourceTag;
}

namespace {
struct slDLSSGOptions;
}

namespace {
struct slDLSSGState;
}

namespace {
struct slReflexOptions;
}

namespace {
struct ViewportFGState;
}

namespace {
struct DLSSGSetOptionsLogState;
}

namespace {
struct ReflexSignalLogState;
}

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

using slResult = int;

namespace {
enum class slResourceType : char {
    kTexture2D = 0,
};
}

using PFN_slGetFeatureFunction = slResult (*)(uint32_t feature, const char* streamline_hook_functionName, void*& streamline_hook_function);

using PFN_slGetPluginFunction = void* (*)(const char* streamline_hook_functionName);

using PFN_slSetD3DDevice = slResult (*)(void* streamline_hook_d3dDevice);

using PFN_slSetTag = slResult (*)(const slViewportHandle& viewport, const slResourceTag* tags, uint32_t numTags,
                                  void* streamline_hook_commandBuffer);

using PFN_slSetTagForFrame = slResult (*)(const slBaseStructure& streamline_hook_frame, const slViewportHandle& viewport,
                                          const slResourceTag* tags, uint32_t numTags, void* streamline_hook_commandBuffer);

using PFN_slEvaluateFeature = slResult (*)(uint32_t feature, const slBaseStructure& streamline_hook_frame,
                                           const slBaseStructure** inputs, uint32_t numInputs, void* streamline_hook_commandBuffer);

using PFN_slDLSSGSetOptions = slResult (*)(const slViewportHandle& viewport, const slDLSSGOptions& streamline_hook_options);

using PFN_slDLSSGGetState = slResult (*)(const slViewportHandle& viewport, slDLSSGState& state,
                                         const slDLSSGOptions* streamline_hook_options);

using PFN_slReflexSleep = slResult (*)(const void* streamline_hook_frame);

using PFN_slReflexSetOptions = slResult (*)(const slReflexOptions& streamline_hook_options);

using PFN_slReflexSetConstants = slResult (*)(const SLReflexConstants& streamline_hook_consts);

namespace StreamlineHook {
bool IsExternalOverlayPresentGuardActive();
}

namespace StreamlineHook {
bool IsExternalOverlayPluginLookupGuardReady();
}

namespace StreamlineHook {
bool IsAcceptedD3D12Device(IUnknown* device);
}

namespace StreamlineHook {
bool HasExplicitSetOptionsActivationForCurrentComeback();
}

namespace StreamlineHook {
void Init();
}

namespace StreamlineHook {
void OnModuleUnloaded(const void* moduleBase, size_t moduleSizeBytes, const char* moduleBaseName);
}

namespace StreamlineHook {
void OnModuleLoaded(HMODULE module, const char* moduleNameOrPath);
}

namespace StreamlineHook {
bool IsInitialized();
}

namespace StreamlineHook {
bool IsDLSSFGRequestedViaStreamline();
}

namespace StreamlineHook {
void OnAuthoritativeFFXTakeover();
}

namespace StreamlineHook {
void OnAuthoritativeStreamlineStartupHandoff();
}

namespace StreamlineHook {
void Shutdown();
}

namespace StreamlineHook {
void FlushSuppressedSetOptionsOffIfNeeded();
}

inline std::atomic<bool> streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof{false};

inline std::atomic<uint32_t> streamline_hook_g_StartupProtectedOffChurnActiveProofCount{0};

inline bool IsObserverOnlyModeActive() {
    return HookOverlayObserverOnlyEnabled();
}

inline void LogDroppedSuppressedOffForStartupProtectedStreamlineComeback(
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

inline bool IsObserverPolicyOnlyModeActive() {
    return HookOverlayObserverPolicyOnlyEnabled();
}

inline bool ShouldKeepPureObserverOnlyStreamlineBehavior() {
    return ce::streamline_runtime_policy::ShouldKeepPureObserverOnlyStreamlineBehavior(
        IsObserverOnlyModeActive(), IsObserverPolicyOnlyModeActive());
}

inline bool TryServicePostSLStartupActivation(const char* source, bool clearStartupWindow) {
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

inline void ResetStartupProtectedOffChurnActiveProof(const char* reason) {
    const bool wasPending = streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.exchange(false, std::memory_order_acq_rel);
    const uint32_t previousProof = streamline_hook_g_StartupProtectedOffChurnActiveProofCount.exchange(0, std::memory_order_acq_rel);
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

inline void LogAcceptedOffDuringActivatedUnconfirmedResume(const char* source, bool startupWindowActive, bool hadFSRFGPhase,
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

inline void MarkStartupProtectedOffChurnObserved(const char* source, bool postSLConfirmedRendering,
                                          bool postSLConfirmedButStartupSettling,
                                          bool postSLConfirmedButRuntimeStateStabilizing) {
    const bool wasPending = streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.exchange(true, std::memory_order_acq_rel);
    const uint32_t previousProof = streamline_hook_g_StartupProtectedOffChurnActiveProofCount.exchange(0, std::memory_order_acq_rel);
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

inline void MarkStartupProtectedActiveRuntimeProof(const char* source, int multiplier) {
    if (!streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.load(std::memory_order_acquire)) {
        return;
    }

    const uint32_t previousProof = streamline_hook_g_StartupProtectedOffChurnActiveProofCount.load(std::memory_order_acquire);
    if (ce::streamline_runtime_policy::HasStartupProtectedOffChurnActiveProof(previousProof)) {
        return;
    }

    const uint32_t newProof = streamline_hook_g_StartupProtectedOffChurnActiveProofCount.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (ce::streamline_runtime_policy::HasStartupProtectedOffChurnActiveProof(newProof)) {
        const bool wasPending = streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.exchange(false, std::memory_order_acq_rel);
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

inline bool IsStartupProtectedOffChurnAwaitingActiveProof(bool startupProtectedComebackProof, bool postSLConfirmedRendering,
                                                   bool postSLConfirmedButStartupSettling) {
    return ce::streamline_runtime_policy::ShouldKeepStartupProtectedOffChurnDeferredUntilActiveProof(
        streamline_hook_g_StartupProtectedOffChurnNeedsActiveProof.load(std::memory_order_acquire),
        streamline_hook_g_StartupProtectedOffChurnActiveProofCount.load(std::memory_order_acquire), startupProtectedComebackProof,
        postSLConfirmedRendering, postSLConfirmedButStartupSettling);
}

inline thread_local int streamline_hook_g_ExternalOverlayPresentGuardDepth = 0;

inline constexpr slResult streamline_hook_kSlResultOk = 0;

inline constexpr slResult streamline_hook_kSlResultErrorInvalidState = 38;

inline constexpr uint32_t streamline_hook_kSLFeatureDLSSG = 1000;

inline constexpr uint32_t streamline_hook_kSLFeatureReflex = 3;  // Streamline Reflex feature ID

inline constexpr size_t streamline_hook_kSLStructVersion1 = 1;

inline constexpr size_t streamline_hook_kSLStructVersion2 = 2;

inline constexpr size_t streamline_hook_kSLStructVersion3 = 3;

inline constexpr size_t streamline_hook_kSLStructVersion4 = 4;

inline constexpr size_t streamline_hook_kSLStructVersion5 = 5;

inline constexpr char streamline_hook_kSLBooleanInvalid = 2;

// Streamline Reflex mode constants
inline constexpr int streamline_hook_kSLReflexModeOff = 0;

inline constexpr int streamline_hook_kSLReflexModeEnabled = 1;

inline constexpr int streamline_hook_kSLReflexOptionsModeOff = 0;

namespace {
// Streamline sl::ReflexConstants structure (matches Streamline SDK)
struct SLReflexConstants {
    size_t structSize = sizeof(SLReflexConstants);
    uint32_t version = static_cast<uint32_t>(streamline_hook_kSLStructVersion1);
    int32_t mode = streamline_hook_kSLReflexModeOff;
    uint32_t frameLimitUs = 0;
    uint32_t markersEnabled = 0;
    uint32_t useMarkersToOptimize = 0;
};
}

namespace {
struct slStructType {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};
}

namespace {
struct slBaseStructure {
    slBaseStructure() = default;
    slBaseStructure(slStructType type, size_t version) : structType(type), structVersion(version) {}

    slBaseStructure* next = nullptr;
    slStructType structType{};
    size_t structVersion = 0;
};
}

inline constexpr slStructType streamline_hook_kDLSSGOptionsStructType = {
    0xfac5f1cb, 0x2dfd, 0x4f36, {0xa1, 0xe6, 0x3a, 0x9e, 0x86, 0x52, 0x56, 0xc5}};

inline constexpr slStructType streamline_hook_kDLSSGStateStructType = {
    0xcc8ac8e1, 0xa179, 0x44f5, {0x97, 0xfa, 0xe7, 0x41, 0x12, 0xf9, 0xbc, 0x61}};

inline constexpr slStructType streamline_hook_kViewportHandleStructType = {
    0x171b6435, 0x9b3c, 0x4fc8, {0x99, 0x94, 0xfb, 0xe5, 0x25, 0x69, 0xaa, 0xa4}};

inline constexpr slStructType streamline_hook_kResourceTagStructType = {
    0x4c6a5aad, 0xb445, 0x496c, {0x87, 0xff, 0x1a, 0xf3, 0x84, 0x5b, 0xe6, 0x53}};

inline constexpr slStructType streamline_hook_kReflexOptionsStructType = {
    0xf03af81a, 0x6d0b, 0x4902, {0xa6, 0x51, 0xc4, 0x96, 0x5e, 0x21, 0x54, 0x34}};

namespace {
struct slViewportHandle : slBaseStructure {
    slViewportHandle() : slBaseStructure(streamline_hook_kViewportHandleStructType, streamline_hook_kSLStructVersion1) {}

    uint32_t value = 0xFFFFFFFFu;
};
}

namespace {
struct slExtent {
    uint32_t top = 0;
    uint32_t left = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};
}

namespace {
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
}

namespace {
struct slResourceTag : slBaseStructure {
    slResource* resource = nullptr;
    uint32_t type = 0;
    int32_t lifecycle = 0;
    slExtent extent{};
};
}

inline constexpr uint32_t streamline_hook_kSLBufferTypeUIColorAndAlpha = 23;

namespace {
struct slDLSSGOptions : slBaseStructure {
    slDLSSGOptions() : slBaseStructure(streamline_hook_kDLSSGOptionsStructType, streamline_hook_kSLStructVersion5) {}

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
    char bReserved15 = streamline_hook_kSLBooleanInvalid;
    uint32_t queueParallelismMode = 0;
    char enableUserInterfaceRecomposition = 0;
    float dynamicTargetFrameRate = 0.0f;
};
}

namespace {
struct slDLSSGState : slBaseStructure {
    slDLSSGState() : slBaseStructure(streamline_hook_kDLSSGStateStructType, streamline_hook_kSLStructVersion4) {}

    uint64_t estimatedVRAMUsageInBytes = 0;
    uint32_t status = 0;
    uint32_t minWidthOrHeight = 0;
    uint32_t numFramesActuallyPresented = 0;
    uint32_t numFramesToGenerateMax = 0;
    char bReserved4 = streamline_hook_kSLBooleanInvalid;
    char bIsVsyncSupportAvailable = streamline_hook_kSLBooleanInvalid;
    void* inputsProcessingCompletionFence = nullptr;
    uint64_t lastPresentInputsProcessingCompletionFenceValue = 0;
    char bIsDynamicMFGSupported = streamline_hook_kSLBooleanInvalid;
};
}

namespace {
struct slReflexOptions : slBaseStructure {
    slReflexOptions() : slBaseStructure(streamline_hook_kReflexOptionsStructType, streamline_hook_kSLStructVersion1) {}

    int32_t mode = streamline_hook_kSLReflexOptionsModeOff;
    uint32_t frameLimitUs = 0;
    bool useMarkersToOptimize = false;
    uint16_t virtualKey = 0;
    uint32_t idThread = 0;
};
}

namespace {
struct ViewportFGState {
    bool active = false;
    int multiplier = 0;
    uint32_t generatedFrames = 0;
    uint32_t capabilityMax = 0;
};
}

inline std::mutex streamline_hook_g_InitMutex;

inline std::mutex streamline_hook_g_StateMutex;

inline std::mutex streamline_hook_g_ModuleHookMutex;

inline std::mutex streamline_hook_g_FeatureHookMutex;

inline std::mutex streamline_hook_g_AcceptedD3D12DeviceMutex;

inline ID3D12Device* streamline_hook_g_AcceptedD3D12Device = nullptr;

inline std::atomic<bool> streamline_hook_g_DynamicHooksRegistered{false};

inline std::atomic<bool> streamline_hook_g_StreamlineUsesD3D12{false};

inline std::atomic<bool> streamline_hook_g_NoModulesLogged{false};

inline std::atomic<bool> streamline_hook_g_ModuleSnapshotFailureLogged{false};

inline std::atomic<bool> streamline_hook_g_ModuleSnapshotRetrySuccessLogged{false};

inline std::atomic<uint32_t> streamline_hook_g_IATPatchesMask{0};

inline std::atomic<uint32_t> streamline_hook_g_InstalledModuleMask{0};

inline std::atomic<void*> streamline_hook_g_SLGetFeatureFunctionTarget{nullptr};

inline std::atomic<void*> streamline_hook_g_SLGetPluginFunctionTarget{nullptr};

inline std::atomic<void*> streamline_hook_g_SLSetD3DDeviceTarget{nullptr};

inline std::atomic<void*> streamline_hook_g_SLSetTagTarget{nullptr};

inline std::atomic<void*> streamline_hook_g_SLSetTagForFrameTarget{nullptr};

inline std::atomic<void*> streamline_hook_g_SLEvaluateFeatureTarget{nullptr};

inline std::atomic<void*> streamline_hook_g_DLSSGSetOptionsTarget{nullptr};

inline std::atomic<void*> streamline_hook_g_DLSSGGetStateTarget{nullptr};

inline std::atomic<void*> streamline_hook_g_ReflexSleepTarget{nullptr};

inline std::atomic<void*> streamline_hook_g_ReflexSetOptionsTarget{nullptr};

inline std::atomic<void*> streamline_hook_g_ReflexSetConstantsTarget{nullptr};

inline std::atomic<void*> streamline_hook_g_DLSSGSetOptionsImportFallbackAttemptedTarget{nullptr};

inline std::atomic<void*> streamline_hook_g_DLSSGGetStateImportFallbackAttemptedTarget{nullptr};

inline std::atomic<void*> streamline_hook_g_ReflexSleepImportFallbackAttemptedTarget{nullptr};

inline std::atomic<void*> streamline_hook_g_ReflexSetOptionsImportFallbackAttemptedTarget{nullptr};

inline std::atomic<void*> streamline_hook_g_ReflexSetConstantsImportFallbackAttemptedTarget{nullptr};

inline std::atomic<bool> streamline_hook_g_SLGetFeatureFunctionHooked{false};

inline std::atomic<bool> streamline_hook_g_SLGetPluginFunctionHooked{false};

inline std::atomic<bool> streamline_hook_g_SLSetD3DDeviceHooked{false};

inline std::atomic<bool> streamline_hook_g_SLSetTagHooked{false};

inline std::atomic<bool> streamline_hook_g_SLSetTagForFrameHooked{false};

inline std::atomic<bool> streamline_hook_g_SLEvaluateFeatureHooked{false};

inline std::atomic<bool> streamline_hook_g_DLSSGSetOptionsHooked{false};

inline std::atomic<bool> streamline_hook_g_DLSSGGetStateHooked{false};

inline std::atomic<bool> streamline_hook_g_ReflexSleepHooked{false};

inline std::atomic<bool> streamline_hook_g_ReflexSetOptionsHooked{false};

inline std::atomic<bool> streamline_hook_g_ReflexSetConstantsHooked{false};

inline std::atomic<bool> streamline_hook_g_DLSSGSetOptionsReturnedWrapperFallbackLogged{false};

inline std::atomic<bool> streamline_hook_g_DLSSGGetStateReturnedWrapperFallbackLogged{false};

inline std::atomic<bool> streamline_hook_g_ReflexSleepReturnedWrapperFallbackLogged{false};

inline std::atomic<bool> streamline_hook_g_ReflexSetOptionsReturnedWrapperFallbackLogged{false};

inline std::atomic<bool> streamline_hook_g_ReflexSetConstantsReturnedWrapperFallbackLogged{false};

inline std::atomic<bool> streamline_hook_g_DLSSGSetOptionsProactiveFallbackLogged{false};

inline std::atomic<bool> streamline_hook_g_DLSSGGetStateProactiveFallbackLogged{false};

inline std::atomic<bool> streamline_hook_g_ReflexSleepProactiveFallbackLogged{false};

inline std::atomic<bool> streamline_hook_g_ReflexSetOptionsProactiveFallbackLogged{false};

inline std::atomic<bool> streamline_hook_g_ReflexSetConstantsProactiveFallbackLogged{false};

inline std::atomic<bool> streamline_hook_g_DLSSGSetOptionsLookupLogged{false};

inline std::atomic<bool> streamline_hook_g_DLSSGGetStateLookupLogged{false};

inline std::atomic<bool> streamline_hook_g_ReflexSleepLookupLogged{false};

inline std::atomic<bool> streamline_hook_g_ReflexSetOptionsLookupLogged{false};

inline std::atomic<bool> streamline_hook_g_ReflexSetConstantsLookupLogged{false};

inline std::atomic<ULONGLONG> streamline_hook_g_ReflexFeatureHookRetryLastMs{0};

inline std::unordered_map<uint32_t, ViewportFGState> streamline_hook_g_ViewportStates;

inline std::unordered_map<uint32_t, uint32_t> streamline_hook_g_ViewportCapabilityMax;

inline std::atomic<ULONGLONG> streamline_hook_g_SuppressNewGetStateActivationUntilMs{0};

inline constexpr ULONGLONG streamline_hook_kAuthoritativeFFXTakeoverGetStateSuppressMs = 250;

inline std::atomic<bool> streamline_hook_g_BlockGetStateOnlyReactivationUntilExplicitSetOptions{false};

inline std::atomic<bool> streamline_hook_g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap{false};

inline std::atomic<bool> streamline_hook_g_CurrentComebackActivatedViaExplicitSetOptions{false};

inline std::atomic<bool> streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions{false};

inline std::atomic<bool> streamline_hook_g_ConfirmedDLSSReflexSuspendPending{false};

inline std::atomic<bool> streamline_hook_g_StartupWindowOffExtensionPending{false};

inline std::mutex streamline_hook_g_SuppressedOffMutex;

inline bool streamline_hook_g_SuppressedSetOptionsOffDuringStartup = false;

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - trivial value initialization cannot throw
inline slViewportHandle streamline_hook_g_SuppressedOffViewport = {};

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - trivial value initialization cannot throw
inline slDLSSGOptions streamline_hook_g_SuppressedOffOptions = {};

inline uint32_t streamline_hook_g_SuppressedOffViewportKey = 0;

// --- DLSSG activation-health diagnostics (GTA cold-start DLSS FG "active but not interpolating", session
// 20260702_094955: optionsMode=on, updateActive=1, yet presents stayed at base rate with
// numFramesActuallyPresented==1 and no fps gain) --------------------------------------------------------
// sl.dlss_g reports WHY it declines to interpolate in DLSSGState.status (sl_dlss_g.h DLSSGStatus bitflags).
// DLSSG hard-requires Reflex, and GTA's Reflex is historically flaky even without CE (user report), so
// eDLSSGStatusFailReflexNotDetectedAtRuntime is the prime suspect — the health monitor pairs the status
// decode with Reflex call-activity evidence so one run pins the failing precondition.
inline constexpr uint32_t streamline_hook_kDLSSGStatusFailResolutionTooLow = 1u << 0;

inline constexpr uint32_t streamline_hook_kDLSSGStatusFailReflexNotDetectedAtRuntime = 1u << 1;

inline constexpr uint32_t streamline_hook_kDLSSGStatusFailHDRFormatNotSupported = 1u << 2;

inline constexpr uint32_t streamline_hook_kDLSSGStatusFailCommonConstantsInvalid = 1u << 3;

inline constexpr uint32_t streamline_hook_kDLSSGStatusFailGetCurrentBackBufferIndex = 1u << 4;

inline void FormatDLSSGStatusFlags(uint32_t status, char* buffer, size_t bufferSize) {
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
    if (status & streamline_hook_kDLSSGStatusFailResolutionTooLow) {
        append("resolutionTooLow");
    }
    if (status & streamline_hook_kDLSSGStatusFailReflexNotDetectedAtRuntime) {
        append("REFLEX-NOT-DETECTED");
    }
    if (status & streamline_hook_kDLSSGStatusFailHDRFormatNotSupported) {
        append("hdrFormatNotSupported");
    }
    if (status & streamline_hook_kDLSSGStatusFailCommonConstantsInvalid) {
        append("commonConstantsInvalid");
    }
    if (status & streamline_hook_kDLSSGStatusFailGetCurrentBackBufferIndex) {
        append("getCurrentBackBufferIndexFail");
    }
    const uint32_t knownMask = streamline_hook_kDLSSGStatusFailResolutionTooLow | streamline_hook_kDLSSGStatusFailReflexNotDetectedAtRuntime |
                               streamline_hook_kDLSSGStatusFailHDRFormatNotSupported | streamline_hook_kDLSSGStatusFailCommonConstantsInvalid |
                               streamline_hook_kDLSSGStatusFailGetCurrentBackBufferIndex;
    if (status & ~knownMask) {
        char unknownText[32];
        snprintf(unknownText, sizeof(unknownText), "unknown(0x%X)", status & ~knownMask);
        append(unknownText);
    }
}

// Reflex call-activity evidence. Written from the Reflex hooks with RELAXED atomics + GetTickCount64 only:
// the manual Reflex FPS limiter's latency-critical sleep path must not gain locks, logging, or syscalls
// (GetTickCount64 is a shared-page memory read). Read from the GetState-side health monitor.
inline std::atomic<uint64_t> streamline_hook_g_ReflexSleepObservedCount{0};

inline std::atomic<uint64_t> streamline_hook_g_ReflexSleepLastTickMs{0};

inline std::atomic<uint64_t> streamline_hook_g_ReflexSetOptionsObservedCount{0};

inline std::atomic<uint64_t> streamline_hook_g_ReflexSetOptionsLastTickMs{0};

inline std::atomic<int32_t> streamline_hook_g_ReflexLastForwardedMode{-1};

// Health-monitor state (GetState thread(s) only; relaxed is fine for diagnostics).
inline std::atomic<uint64_t> streamline_hook_g_DLSSGNotInterpolatingStreak{0};

inline std::atomic<uint64_t> streamline_hook_g_ReflexSleepCountAtLastHealthLog{0};

inline std::atomic<uint32_t> streamline_hook_g_DLSSGLastObservedStatus{0};

// GTA polls slDLSSGGetState roughly per frame, so the first warning lands within a handful of frames of a
// failed activation and repeats sparsely afterwards (deterministic sample counts, not wall-clock).
inline constexpr uint64_t streamline_hook_kDLSSGHealthWarnStreak = 8;

inline constexpr uint64_t streamline_hook_kDLSSGHealthWarnRepeat = 512;

inline PFN_slGetFeatureFunction streamline_hook_g_Original_slGetFeatureFunction = nullptr;

inline PFN_slGetPluginFunction streamline_hook_g_Original_slGetPluginFunction = nullptr;

inline PFN_slSetD3DDevice streamline_hook_g_Original_slSetD3DDevice = nullptr;

inline PFN_slSetTag streamline_hook_g_Original_slSetTag = nullptr;

inline PFN_slSetTagForFrame streamline_hook_g_Original_slSetTagForFrame = nullptr;

inline PFN_slEvaluateFeature streamline_hook_g_Original_slEvaluateFeature = nullptr;

inline PFN_slDLSSGSetOptions streamline_hook_g_Original_slDLSSGSetOptions = nullptr;

inline PFN_slDLSSGGetState streamline_hook_g_Original_slDLSSGGetState = nullptr;

inline PFN_slReflexSleep streamline_hook_g_Original_slReflexSleep = nullptr;

inline PFN_slReflexSetOptions streamline_hook_g_Original_slReflexSetOptions = nullptr;

inline PFN_slReflexSetConstants streamline_hook_g_Original_slReflexSetConstants = nullptr;

inline slResult Hooked_slGetFeatureFunction(uint32_t feature, const char* streamline_hook_functionName, void*& streamline_hook_function);

inline void* Hooked_slGetPluginFunction(const char* streamline_hook_functionName);

inline slResult Hooked_slSetD3DDevice(void* streamline_hook_d3dDevice);

inline slResult Hooked_slSetTag(const slViewportHandle& viewport, const slResourceTag* tags, uint32_t numTags,
                         void* streamline_hook_commandBuffer);

inline slResult Hooked_slSetTagForFrame(const slBaseStructure& streamline_hook_frame, const slViewportHandle& viewport,
                                 const slResourceTag* tags, uint32_t numTags, void* streamline_hook_commandBuffer);

inline slResult Hooked_slEvaluateFeature(uint32_t feature, const slBaseStructure& streamline_hook_frame, const slBaseStructure** inputs,
                                  uint32_t numInputs, void* streamline_hook_commandBuffer);

inline slResult Hooked_slDLSSGSetOptions(const slViewportHandle& viewport, const slDLSSGOptions& streamline_hook_options);

inline slResult Hooked_slDLSSGGetState(const slViewportHandle& viewport, slDLSSGState& state, const slDLSSGOptions* streamline_hook_options);

inline slResult Hooked_slReflexSleep(const void* streamline_hook_frame);

inline slResult Hooked_slReflexSetOptions(const slReflexOptions& streamline_hook_options);

inline slResult Hooked_slReflexSetConstants(const SLReflexConstants& streamline_hook_consts);

inline const char* GetDLSSGModeName(uint32_t mode) {
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

inline const char* GetModuleBaseName(const char* moduleNameOrPath) {
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

inline bool IsStreamlineModuleName(const char* moduleNameOrPath) {
    return ce::streamline_runtime_policy::IsStreamlineModuleNameForFeatureHooking(moduleNameOrPath);
}

inline bool ShouldHookStreamlineCoreExports(const char* moduleNameOrPath) {
    return ce::streamline_runtime_policy::ShouldHookStreamlineCoreExportsOnLoad(moduleNameOrPath);
}

inline bool IsStreamlineCoreDynamicHookModule(const char* moduleBaseName, HMODULE) {
    return ShouldHookStreamlineCoreExports(moduleBaseName);
}

inline bool IsStreamlineDLSSGDynamicHookModule(const char* moduleBaseName, HMODULE) {
    return ce::streamline_runtime_policy::IsStreamlineDLSSGFeatureModuleName(moduleBaseName);
}

inline bool IsStreamlineReflexDynamicHookModule(const char* moduleBaseName, HMODULE) {
    return ce::streamline_runtime_policy::IsStreamlineReflexFeatureModuleName(moduleBaseName);
}

inline uint32_t GetModuleMaskBit(const char* moduleNameOrPath) {
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

inline void LogSkippedStreamlineCoreExportsOnce(const char* moduleBaseName, HMODULE module, bool hasGetFeature,
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

inline size_t GetModuleImageSizeBytes(HMODULE module) {
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

inline bool DoesAddressBelongToLoadedModule(void* address, HMODULE* ownerModule, char* ownerPath, DWORD ownerPathCapacity,
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

inline void LogStaleStreamlineOriginalBlockedOnce(const char* streamline_hook_functionName, void* original, void* validationAddress,
                                           const char* expectedModuleRole, DWORD error) {
    static std::mutex s_logMutex;
    static std::unordered_map<std::string, bool> s_loggedOriginals;

    std::string key = streamline_hook_functionName ? streamline_hook_functionName : "<unknown>";
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
        streamline_hook_functionName ? streamline_hook_functionName : "<unknown>", original, validationAddress,
        expectedModuleRole ? expectedModuleRole : "loaded Streamline module", static_cast<unsigned long>(error));
}

inline bool IsSavedStreamlineOriginalCallable(const char* streamline_hook_functionName, void* original, void* validationAddress,
                                       const char* expectedModuleRole) {
    const void* addressToValidate = validationAddress ? validationAddress : original;
    DWORD ownerError = ERROR_SUCCESS;
    const bool ownerLoaded =
        DoesAddressBelongToLoadedModule(const_cast<void*>(addressToValidate), nullptr, nullptr, 0, &ownerError);
    if (ce::streamline_runtime_policy::ShouldForwardSavedStreamlineOriginal(original != nullptr, ownerLoaded)) {
        return true;
    }

    if (original) {
        LogStaleStreamlineOriginalBlockedOnce(streamline_hook_functionName, original, const_cast<void*>(addressToValidate),
                                              expectedModuleRole, ownerError);
    }
    return false;
}

inline PFN_slGetFeatureFunction GetCallableOriginalGetFeatureFunction() {
    auto original = streamline_hook_g_Original_slGetFeatureFunction;
    return IsSavedStreamlineOriginalCallable("slGetFeatureFunction", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLGetFeatureFunctionTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;
}

inline PFN_slGetPluginFunction GetCallableOriginalGetPluginFunction() {
    auto original = streamline_hook_g_Original_slGetPluginFunction;
    return IsSavedStreamlineOriginalCallable("slGetPluginFunction", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLGetPluginFunctionTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;
}

inline PFN_slSetD3DDevice GetCallableOriginalSetD3DDevice() {
    auto original = streamline_hook_g_Original_slSetD3DDevice;
    return IsSavedStreamlineOriginalCallable("slSetD3DDevice", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLSetD3DDeviceTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;
}

inline PFN_slSetTag GetCallableOriginalSetTag() {
    auto original = streamline_hook_g_Original_slSetTag;
    return IsSavedStreamlineOriginalCallable("slSetTag", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLSetTagTarget.load(std::memory_order_acquire), "core Streamline module")
               ? original
               : nullptr;
}

inline PFN_slSetTagForFrame GetCallableOriginalSetTagForFrame() {
    auto original = streamline_hook_g_Original_slSetTagForFrame;
    return IsSavedStreamlineOriginalCallable("slSetTagForFrame", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLSetTagForFrameTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;
}

inline PFN_slEvaluateFeature GetCallableOriginalEvaluateFeature() {
    auto original = streamline_hook_g_Original_slEvaluateFeature;
    return IsSavedStreamlineOriginalCallable("slEvaluateFeature", reinterpret_cast<void*>(original),
                                             streamline_hook_g_SLEvaluateFeatureTarget.load(std::memory_order_acquire),
                                             "core Streamline module")
               ? original
               : nullptr;
}

inline PFN_slDLSSGSetOptions GetCallableOriginalDLSSGSetOptions() {
    auto original = streamline_hook_g_Original_slDLSSGSetOptions;
    return IsSavedStreamlineOriginalCallable("slDLSSGSetOptions", reinterpret_cast<void*>(original),
                                             streamline_hook_g_DLSSGSetOptionsTarget.load(std::memory_order_acquire),
                                             "DLSSG feature module")
               ? original
               : nullptr;
}

inline PFN_slDLSSGGetState GetCallableOriginalDLSSGGetState() {
    auto original = streamline_hook_g_Original_slDLSSGGetState;
    return IsSavedStreamlineOriginalCallable("slDLSSGGetState", reinterpret_cast<void*>(original),
                                             streamline_hook_g_DLSSGGetStateTarget.load(std::memory_order_acquire),
                                             "DLSSG feature module")
               ? original
               : nullptr;
}

inline PFN_slReflexSleep GetCallableOriginalReflexSleep() {
    auto original = streamline_hook_g_Original_slReflexSleep;
    return IsSavedStreamlineOriginalCallable("slReflexSleep", reinterpret_cast<void*>(original),
                                             streamline_hook_g_ReflexSleepTarget.load(std::memory_order_acquire),
                                             "Reflex feature module")
               ? original
               : nullptr;
}

inline PFN_slReflexSetOptions GetCallableOriginalReflexSetOptions() {
    auto original = streamline_hook_g_Original_slReflexSetOptions;
    return IsSavedStreamlineOriginalCallable("slReflexSetOptions", reinterpret_cast<void*>(original),
                                             streamline_hook_g_ReflexSetOptionsTarget.load(std::memory_order_acquire),
                                             "Reflex feature module")
               ? original
               : nullptr;
}

inline PFN_slReflexSetConstants GetCallableOriginalReflexSetConstants() {
    auto original = streamline_hook_g_Original_slReflexSetConstants;
    return IsSavedStreamlineOriginalCallable("slReflexSetConstants", reinterpret_cast<void*>(original),
                                             streamline_hook_g_ReflexSetConstantsTarget.load(std::memory_order_acquire),
                                             "Reflex feature module")
               ? original
               : nullptr;
}

inline uint32_t GetViewportKey(const slViewportHandle& viewport) {
    return viewport.value;
}

inline slDLSSGOptions CloneDLSSGOptions(const slDLSSGOptions& source) {
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
    if (source.structVersion >= streamline_hook_kSLStructVersion2) {
        copy.bReserved15 = source.bReserved15;
    }
    if (source.structVersion >= streamline_hook_kSLStructVersion3) {
        copy.queueParallelismMode = source.queueParallelismMode;
    }
    if (source.structVersion >= streamline_hook_kSLStructVersion4) {
        copy.enableUserInterfaceRecomposition = source.enableUserInterfaceRecomposition;
    }
    if (source.structVersion >= streamline_hook_kSLStructVersion5) {
        copy.dynamicTargetFrameRate = source.dynamicTargetFrameRate;
    }
    if (source.structVersion > streamline_hook_kSLStructVersion5) {
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true)) {
            HookLogImportant("SL: DLSSG options structVersion=%zu exceeds CE's max (5); forwarding v5 prefix only",
                             source.structVersion);
        }
        copy.structVersion = streamline_hook_kSLStructVersion5;
    }
    return copy;
}

inline int GetEffectiveMultiplier(const slDLSSGOptions& streamline_hook_options) {
    return ce::streamline_runtime_policy::ResolveDLSSFGMultiplier(
        ce::streamline_runtime_policy::IsDLSSGModeEnabled(streamline_hook_options.mode), streamline_hook_options.numFramesToGenerate);
}

namespace {
struct DLSSGSetOptionsLogState {
    bool valid = false;
    bool requestedEnabled = false;
    bool forwarded = false;
    uint32_t requestMode = 0;
    uint32_t forwardedMode = 0;
    uint32_t requestedGeneratedFrames = 0;
    uint32_t forwardedGeneratedFrames = 0;
    uint32_t capabilityMax = 0;
    slResult result = streamline_hook_kSlResultOk;
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
}

inline void LogDLSSGSetOptionsTransition(uint32_t viewportKey, const slDLSSGOptions& requestedOptions,
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

namespace {
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
}

inline void LogStreamlineReflexSignalChange(const char* sourceName, int32_t mode, uint32_t incomingFrameLimitUs,
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

inline void MaybePrepareForStreamlineEnableTransitionFromReflex(const char* sourceName) {
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

inline void HandleStreamlineReflexPacingSignal(const char* sourceName, int32_t mode, uint32_t incomingFrameLimitUs,
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
            streamline_hook_g_ConfirmedDLSSReflexSuspendPending.exchange(false, std::memory_order_acq_rel);
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
            HookLogImportant("Streamline Hook: Cleared confirmed Reflex suspend intent on pacing reactivation via %s",
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
                HookIsPostSLOverlayActiveButUnconfirmed())) {
            const bool wasPending = streamline_hook_g_ConfirmedDLSSReflexSuspendPending.exchange(true, std::memory_order_acq_rel);
            const bool startupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
            const bool runtimeStabilizing = HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing() ||
                                            HookIsPostSLOverlayConfirmedButStaleOffWarmupProtected();
            ResetStartupProtectedOffChurnActiveProof("confirmed Reflex suspend intent");
            if (!wasPending) {
                HookLogImportant(
                    "Streamline Hook: Confirmed DLSS-G epoch observed game-owned Reflex OFF via %s "
                    "(startupWindow=%d settling=%d stabilizing=%d) — next inactive GetState/SetOptions edge is "
                    "authoritative so DLSS-G cannot remain active without Reflex (manual limiter target remains "
                    "unchanged)",
                    sourceName ? sourceName : "unknown",
                    DXGIShared::IsStreamlineStartupTransitionWindowActive() ? 1 : 0, startupSettling ? 1 : 0,
                    runtimeStabilizing ? 1 : 0);
            }
        }
        g_ReflexLimiter.SetGameActivated(false);
    }
}

inline uint32_t GetCachedCapabilityMax(uint32_t viewportKey) {
    std::lock_guard<std::mutex> lock(streamline_hook_g_StateMutex);
    const auto it = streamline_hook_g_ViewportCapabilityMax.find(viewportKey);
    return it != streamline_hook_g_ViewportCapabilityMax.end() ? it->second : 0u;
}

inline void CacheCapabilityMax(uint32_t viewportKey, uint32_t capabilityMax) {
    if (capabilityMax == 0) {
        return;
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_StateMutex);
        auto& cached = streamline_hook_g_ViewportCapabilityMax[viewportKey];
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

inline void ApplyCombinedDLSSFGState(bool active, int multiplier) {
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

inline void ApplyCombinedStreamlineRuntimeState(bool active, int multiplier, bool explicitSetOptionsEnableSignal,
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
        streamline_hook_g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
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
            streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions.load(std::memory_order_acquire));
    const bool previousSignal = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool confirmedReflexSuspendIsAuthoritative =
        ce::streamline_runtime_policy::ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend(
            streamline_hook_g_ConfirmedDLSSReflexSuspendPending.load(std::memory_order_acquire), !active, previousSignal);
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
        streamline_hook_g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);

    const bool previousSignalObserved =
        DXGIShared::g_StreamlineFGRunning.exchange(signalUpdate.effectiveActive, std::memory_order_acq_rel);
    if (ce::streamline_runtime_policy::ShouldArmStartupTransitionWindowOnFreshActiveSignal(active, previousSignal)) {
        DXGIShared::ArmStreamlineStartupTransitionWindow();
        streamline_hook_g_StartupWindowOffExtensionPending.store(true, std::memory_order_release);
    }
    const bool explicitSetOptionsActivation = explicitSetOptionsEnableSignal;
    const bool updatedExplicitSetOptionsActivation =
        ce::streamline_runtime_policy::ResolveCurrentComebackExplicitSetOptionsActivation(
            previousExplicitSetOptionsActivation, signalUpdate.effectiveActive, signalUpdate.freshActivationEdge,
            explicitSetOptionsActivation);
    streamline_hook_g_CurrentComebackActivatedViaExplicitSetOptions.store(updatedExplicitSetOptionsActivation,
                                                          std::memory_order_release);
    const bool acceptedRuntimeOffAwaitingSetOptions =
        ce::streamline_runtime_policy::ShouldLatchAcceptedRuntimeOffAwaitingSetOptions(
            previousSignalObserved, signalUpdate.effectiveActive, sourceWasGetState);
    if (acceptedRuntimeOffAwaitingSetOptions) {
        streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions.store(true, std::memory_order_release);
        const bool wasBlockingGetStateOnlyReactivation =
            streamline_hook_g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.exchange(true, std::memory_order_acq_rel);
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
            streamline_hook_g_ConfirmedDLSSReflexSuspendPending.exchange(false, std::memory_order_acq_rel);
        ResetStartupProtectedOffChurnActiveProof("accepted confirmed Reflex suspend runtime OFF");
        if (consumedSuspendIntent) {
            HookLogImportant(
                "Streamline Hook: Accepted %s OFF as authoritative after confirmed Reflex suspend — "
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
        DX12_OnStreamlineExplicitSetOptionsActivationConfirmed();
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
                    streamline_hook_g_StartupProtectedOffChurnActiveProofCount.load(std::memory_order_acquire),
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
        const bool shouldExtend = streamline_hook_g_StartupWindowOffExtensionPending.exchange(false, std::memory_order_acq_rel);
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
        streamline_hook_g_StartupWindowOffExtensionPending.store(true, std::memory_order_release);
    }
}

inline bool WasViewportRuntimeStateActive(uint32_t viewportKey) {
    std::lock_guard<std::mutex> lock(streamline_hook_g_StateMutex);
    const auto it = streamline_hook_g_ViewportStates.find(viewportKey);
    return it != streamline_hook_g_ViewportStates.end() && it->second.active;
}

inline bool ShouldSuppressNewGetStateActivation() {
    if (ShouldKeepPureObserverOnlyStreamlineBehavior()) {
        return false;
    }

    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    if (ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationDuringUnsafePostFSRComeback(
            streamline_hook_g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap.load(std::memory_order_acquire),
            safePostFSRBootstrapPath, runtimeMode)) {
        return true;
    }

    if (ce::streamline_runtime_policy::ShouldSuppressFreshGetStateActivationWhileRuntimeInactive(
            streamline_hook_g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.load(std::memory_order_acquire),
            DXGIShared::IsStreamlineStartupTransitionWindowActive(), runtimeMode)) {
        return true;
    }

    const ULONGLONG suppressUntilMs = streamline_hook_g_SuppressNewGetStateActivationUntilMs.load(std::memory_order_acquire);
    return suppressUntilMs != 0 && GetTickCount64() < suppressUntilMs;
}

inline bool HasDLSSGRuntimeFenceEvidence(const slDLSSGState& state) {
    return state.inputsProcessingCompletionFence != nullptr ||
           state.lastPresentInputsProcessingCompletionFenceValue != 0;
}

inline void UpdateViewportRuntimeState(uint32_t viewportKey, bool active, int multiplier, uint32_t generatedFrames,
                                uint32_t capabilityMax, const char* source,
                                bool clearAllViewportStatesForDisable = false) {
    ViewportFGState previousState{};
    bool hadPreviousState = false;
    bool stateChanged = false;
    bool anyActive = false;
    int combinedMultiplier = 0;
    size_t clearedActiveViewportCount = 0;

    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_StateMutex);
        const auto existing = streamline_hook_g_ViewportStates.find(viewportKey);
        if (existing != streamline_hook_g_ViewportStates.end()) {
            previousState = existing->second;
            hadPreviousState = true;
        }

        if (active) {
            streamline_hook_g_ViewportStates[viewportKey] = {true, multiplier, generatedFrames, capabilityMax};
        } else if (clearAllViewportStatesForDisable) {
            clearedActiveViewportCount = streamline_hook_g_ViewportStates.size();
            streamline_hook_g_ViewportStates.clear();
        } else {
            streamline_hook_g_ViewportStates.erase(viewportKey);
        }

        const auto current = streamline_hook_g_ViewportStates.find(viewportKey);
        const ViewportFGState currentState =
            current != streamline_hook_g_ViewportStates.end() ? current->second : ViewportFGState{false, 0, 0, capabilityMax};

        stateChanged = !hadPreviousState || previousState.active != currentState.active ||
                       previousState.multiplier != currentState.multiplier ||
                       previousState.generatedFrames != currentState.generatedFrames ||
                       previousState.capabilityMax != currentState.capabilityMax;

        for (const auto& [_, state] : streamline_hook_g_ViewportStates) {
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

inline void LogFeatureImportFallbackUnavailableOnce(const char* moduleBaseName, const char* streamline_hook_functionName, void* exportedProc,
                                             const char* hookName, const char* reason) {
    const char* effectiveHookName = hookName ? hookName : streamline_hook_functionName;
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

inline bool InstallFeatureImportFallbackIfPresent(const char* moduleBaseName, const char* streamline_hook_functionName, void* detour,
                                           void* exportedProc, void** originalSlot, const char* hookName) {
    if (!moduleBaseName || !streamline_hook_functionName || !detour || !exportedProc) {
        return false;
    }

    if (originalSlot && *originalSlot == nullptr) {
        *originalSlot = exportedProc;
    }

    void* patchedOriginal = nullptr;
    if (!IATHook::PatchIATAllModules(moduleBaseName, streamline_hook_functionName, detour, &patchedOriginal)) {
        LogFeatureImportFallbackUnavailableOnce(
            moduleBaseName, streamline_hook_functionName, exportedProc, hookName,
            "no loaded module currently imports this feature directly; retrying on later Streamline module scans");
        return false;
    }

    if (originalSlot && *originalSlot == nullptr) {
        *originalSlot = patchedOriginal ? patchedOriginal : exportedProc;
    }

    HookLogImportant("Streamline Hook: Installed direct import fallback for %s via %s (export=%p original=%p)",
                     hookName ? hookName : streamline_hook_functionName, moduleBaseName, exportedProc,
                     originalSlot ? *originalSlot : patchedOriginal);
    return true;
}

inline bool TryGetOwningModulePath(void* address, char* modulePath, DWORD modulePathCapacity, DWORD* outError) {
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

inline bool TryInstallFeatureImportFallbackForOwningModule(void* streamline_hook_function, const char* streamline_hook_functionName, void* detour,
                                                    void** originalSlot, std::atomic<void*>& attemptedTarget,
                                                    const char* hookName) {
    if (!streamline_hook_function || !streamline_hook_functionName || !detour) {
        return false;
    }

    if (attemptedTarget.load(std::memory_order_acquire) == streamline_hook_function) {
        return false;
    }

    char ownerPath[MAX_PATH] = {};
    DWORD ownerError = ERROR_SUCCESS;
    if (!TryGetOwningModulePath(streamline_hook_function, ownerPath, MAX_PATH, &ownerError)) {
        attemptedTarget.store(streamline_hook_function, std::memory_order_release);
        HookLogImportant("Streamline Hook: Direct import fallback owner resolution failed for %s target=%p error=%lu",
                         hookName ? hookName : streamline_hook_functionName, streamline_hook_function, static_cast<unsigned long>(ownerError));
        return false;
    }

    attemptedTarget.store(streamline_hook_function, std::memory_order_release);

    const char* ownerBaseName = GetModuleBaseName(ownerPath);
    if (!ownerBaseName || !ownerBaseName[0]) {
        return false;
    }

    return InstallFeatureImportFallbackIfPresent(ownerBaseName, streamline_hook_functionName, detour, streamline_hook_function, originalSlot, hookName);
}

inline void LogReturnedWrapperFallbackOnce(std::atomic<bool>& loggedFlag, const char* hookName, void* target, void* wrapper,
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

inline void LogProactiveFeatureHookGapOnce(std::atomic<bool>& loggedFlag, const char* hookName, void* target) {
    if (loggedFlag.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    HookLogImportant(
        "Streamline Hook: Proactively resolved %s at %p but could not patch the export/import path yet; "
        "waiting for an intercepted slGetFeatureFunction lookup to return the wrapper fallback or for a later module "
        "scan to find a direct import.",
        hookName ? hookName : "<unknown>", target);
}

inline void LogFeatureLookupOutcomeOnce(std::atomic<bool>& loggedFlag, const char* hookName, void* originalTarget,
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

inline bool MaybeHookDLSSGSetOptions(void*& streamline_hook_function, bool fallbackToReturnedWrapper) {
    if (!streamline_hook_function) {
        return false;
    }

    if (streamline_hook_function == reinterpret_cast<void*>(Hooked_slDLSSGSetOptions)) {
        streamline_hook_g_DLSSGSetOptionsHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_FeatureHookMutex);
        if (!streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ||
            streamline_hook_g_DLSSGSetOptionsTarget.load(std::memory_order_acquire) != streamline_hook_function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(streamline_hook_function), reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                                  streamline_hook_g_Original_slDLSSGSetOptions, streamline_hook_g_DLSSGSetOptionsHooked, streamline_hook_g_DLSSGSetOptionsTarget,
                                  "slDLSSGSetOptions");
            if (!streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    streamline_hook_function, "slDLSSGSetOptions", reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                    reinterpret_cast<void**>(&streamline_hook_g_Original_slDLSSGSetOptions),
                    streamline_hook_g_DLSSGSetOptionsImportFallbackAttemptedTarget, "slDLSSGSetOptions");
            }
        }
    }

    const bool hookReady = streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire);
    if (fallbackToReturnedWrapper && streamline_hook_function != reinterpret_cast<void*>(Hooked_slDLSSGSetOptions)) {
        if (!GetCallableOriginalDLSSGSetOptions() && !hookReady && !streamline_hook_g_Original_slDLSSGSetOptions) {
            streamline_hook_g_Original_slDLSSGSetOptions = reinterpret_cast<PFN_slDLSSGSetOptions>(streamline_hook_function);
        }
        if (ce::streamline_runtime_policy::ShouldSubstituteReturnedStreamlineFeatureWrapper(
                fallbackToReturnedWrapper, false, GetCallableOriginalDLSSGSetOptions() != nullptr)) {
            LogReturnedWrapperFallbackOnce(streamline_hook_g_DLSSGSetOptionsReturnedWrapperFallbackLogged, "slDLSSGSetOptions",
                                           streamline_hook_function, reinterpret_cast<void*>(Hooked_slDLSSGSetOptions), hookReady);
            streamline_hook_function = reinterpret_cast<void*>(Hooked_slDLSSGSetOptions);
            return true;
        }
    }

    return hookReady;
}

inline bool MaybeHookDLSSGGetState(void*& streamline_hook_function, bool fallbackToReturnedWrapper) {
    if (!streamline_hook_function) {
        return false;
    }

    if (streamline_hook_function == reinterpret_cast<void*>(Hooked_slDLSSGGetState)) {
        streamline_hook_g_DLSSGGetStateHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_FeatureHookMutex);
        if (!streamline_hook_g_DLSSGGetStateHooked.load(std::memory_order_acquire) ||
            streamline_hook_g_DLSSGGetStateTarget.load(std::memory_order_acquire) != streamline_hook_function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(streamline_hook_function), reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                                  streamline_hook_g_Original_slDLSSGGetState, streamline_hook_g_DLSSGGetStateHooked, streamline_hook_g_DLSSGGetStateTarget,
                                  "slDLSSGGetState");
            if (!streamline_hook_g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    streamline_hook_function, "slDLSSGGetState", reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                    reinterpret_cast<void**>(&streamline_hook_g_Original_slDLSSGGetState), streamline_hook_g_DLSSGGetStateImportFallbackAttemptedTarget,
                    "slDLSSGGetState");
            }
        }
    }

    const bool hookReady = streamline_hook_g_DLSSGGetStateHooked.load(std::memory_order_acquire);
    if (fallbackToReturnedWrapper && streamline_hook_function != reinterpret_cast<void*>(Hooked_slDLSSGGetState)) {
        if (!GetCallableOriginalDLSSGGetState() && !hookReady && !streamline_hook_g_Original_slDLSSGGetState) {
            streamline_hook_g_Original_slDLSSGGetState = reinterpret_cast<PFN_slDLSSGGetState>(streamline_hook_function);
        }
        if (ce::streamline_runtime_policy::ShouldSubstituteReturnedStreamlineFeatureWrapper(
                fallbackToReturnedWrapper, false, GetCallableOriginalDLSSGGetState() != nullptr)) {
            LogReturnedWrapperFallbackOnce(streamline_hook_g_DLSSGGetStateReturnedWrapperFallbackLogged, "slDLSSGGetState", streamline_hook_function,
                                           reinterpret_cast<void*>(Hooked_slDLSSGGetState), hookReady);
            streamline_hook_function = reinterpret_cast<void*>(Hooked_slDLSSGGetState);
            return true;
        }
    }

    return hookReady;
}

inline bool MaybeHookReflexSleep(void*& streamline_hook_function, bool fallbackToReturnedWrapper) {
    if (!streamline_hook_function) {
        return false;
    }

    if (streamline_hook_function == reinterpret_cast<void*>(Hooked_slReflexSleep)) {
        streamline_hook_g_ReflexSleepHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_FeatureHookMutex);
        if (!streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) ||
            streamline_hook_g_ReflexSleepTarget.load(std::memory_order_acquire) != streamline_hook_function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(streamline_hook_function), reinterpret_cast<void*>(Hooked_slReflexSleep),
                                  streamline_hook_g_Original_slReflexSleep, streamline_hook_g_ReflexSleepHooked, streamline_hook_g_ReflexSleepTarget, "slReflexSleep");
            if (!streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    streamline_hook_function, "slReflexSleep", reinterpret_cast<void*>(Hooked_slReflexSleep),
                    reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSleep), streamline_hook_g_ReflexSleepImportFallbackAttemptedTarget,
                    "slReflexSleep");
            }
        }
    }

    if (fallbackToReturnedWrapper && !streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire)) {
        if (!streamline_hook_g_Original_slReflexSleep) {
            streamline_hook_g_Original_slReflexSleep = reinterpret_cast<PFN_slReflexSleep>(streamline_hook_function);
        }
        LogReturnedWrapperFallbackOnce(streamline_hook_g_ReflexSleepReturnedWrapperFallbackLogged, "slReflexSleep", streamline_hook_function,
                                       reinterpret_cast<void*>(Hooked_slReflexSleep),
                                       streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire));
        streamline_hook_function = reinterpret_cast<void*>(Hooked_slReflexSleep);
        return true;
    }

    return streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire);
}

inline bool MaybeHookReflexSetOptions(void*& streamline_hook_function, bool fallbackToReturnedWrapper) {
    if (!streamline_hook_function) {
        return false;
    }

    if (streamline_hook_function == reinterpret_cast<void*>(Hooked_slReflexSetOptions)) {
        streamline_hook_g_ReflexSetOptionsHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_FeatureHookMutex);
        if (!streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ||
            streamline_hook_g_ReflexSetOptionsTarget.load(std::memory_order_acquire) != streamline_hook_function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(streamline_hook_function), reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                                  streamline_hook_g_Original_slReflexSetOptions, streamline_hook_g_ReflexSetOptionsHooked, streamline_hook_g_ReflexSetOptionsTarget,
                                  "slReflexSetOptions");
            if (!streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    streamline_hook_function, "slReflexSetOptions", reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                    reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSetOptions),
                    streamline_hook_g_ReflexSetOptionsImportFallbackAttemptedTarget, "slReflexSetOptions");
            }
        }
    }

    if (fallbackToReturnedWrapper && !streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
        if (!streamline_hook_g_Original_slReflexSetOptions) {
            streamline_hook_g_Original_slReflexSetOptions = reinterpret_cast<PFN_slReflexSetOptions>(streamline_hook_function);
        }
        LogReturnedWrapperFallbackOnce(streamline_hook_g_ReflexSetOptionsReturnedWrapperFallbackLogged, "slReflexSetOptions", streamline_hook_function,
                                       reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                                       streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire));
        streamline_hook_function = reinterpret_cast<void*>(Hooked_slReflexSetOptions);
        return true;
    }

    return streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire);
}

inline bool MaybeHookReflexSetConstants(void*& streamline_hook_function, bool fallbackToReturnedWrapper) {
    if (!streamline_hook_function) {
        return false;
    }

    if (streamline_hook_function == reinterpret_cast<void*>(Hooked_slReflexSetConstants)) {
        streamline_hook_g_ReflexSetConstantsHooked.store(true, std::memory_order_release);
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_FeatureHookMutex);
        if (!streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ||
            streamline_hook_g_ReflexSetConstantsTarget.load(std::memory_order_acquire) != streamline_hook_function) {
            InstallInlineHookOnce(reinterpret_cast<void*>(streamline_hook_function),
                                  reinterpret_cast<void*>(Hooked_slReflexSetConstants), streamline_hook_g_Original_slReflexSetConstants,
                                  streamline_hook_g_ReflexSetConstantsHooked, streamline_hook_g_ReflexSetConstantsTarget, "slReflexSetConstants");
            if (!streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    streamline_hook_function, "slReflexSetConstants", reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                    reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSetConstants),
                    streamline_hook_g_ReflexSetConstantsImportFallbackAttemptedTarget, "slReflexSetConstants");
            }
        }
    }

    if (fallbackToReturnedWrapper && !streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
        if (!streamline_hook_g_Original_slReflexSetConstants) {
            streamline_hook_g_Original_slReflexSetConstants = reinterpret_cast<PFN_slReflexSetConstants>(streamline_hook_function);
        }
        LogReturnedWrapperFallbackOnce(streamline_hook_g_ReflexSetConstantsReturnedWrapperFallbackLogged, "slReflexSetConstants",
                                       streamline_hook_function, reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                                       streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire));
        streamline_hook_function = reinterpret_cast<void*>(Hooked_slReflexSetConstants);
        return true;
    }

    return streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire);
}

inline bool TryResolveDLSSGFeatureHooks() {
    auto originalGetFeatureFunction = GetCallableOriginalGetFeatureFunction();
    if (!originalGetFeatureFunction) {
        return false;
    }

    bool hookedAnything = false;

    if (!streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
        void* streamline_hook_function = nullptr;
        const slResult result = originalGetFeatureFunction(streamline_hook_kSLFeatureDLSSG, "slDLSSGSetOptions", streamline_hook_function);
        if (result == streamline_hook_kSlResultOk && streamline_hook_function) {
            const bool hooked = MaybeHookDLSSGSetOptions(streamline_hook_function, false);
            hookedAnything |= hooked;
            if (!hooked && !streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(streamline_hook_g_DLSSGSetOptionsProactiveFallbackLogged, "slDLSSGSetOptions", streamline_hook_function);
            }
        }
    }

    if (!streamline_hook_g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
        void* streamline_hook_function = nullptr;
        const slResult result = originalGetFeatureFunction(streamline_hook_kSLFeatureDLSSG, "slDLSSGGetState", streamline_hook_function);
        if (result == streamline_hook_kSlResultOk && streamline_hook_function) {
            const bool hooked = MaybeHookDLSSGGetState(streamline_hook_function, false);
            hookedAnything |= hooked;
            if (!hooked && !streamline_hook_g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(streamline_hook_g_DLSSGGetStateProactiveFallbackLogged, "slDLSSGGetState", streamline_hook_function);
            }
        }
    }

    return hookedAnything || streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ||
           streamline_hook_g_DLSSGGetStateHooked.load(std::memory_order_acquire);
}

inline bool TryResolveReflexFeatureHooks() {
    auto originalGetFeatureFunction = GetCallableOriginalGetFeatureFunction();
    if (!originalGetFeatureFunction) {
        return false;
    }

    bool hookedAnything = false;
    bool queriedSleep = false;
    bool queriedSetOptions = false;
    bool queriedSetConstants = false;
    slResult sleepResult = streamline_hook_kSlResultErrorInvalidState;
    slResult setOptionsResult = streamline_hook_kSlResultErrorInvalidState;

    slResult setConstantsResult = streamline_hook_kSlResultErrorInvalidState;
    void* sleepFunction = nullptr;
    void* setOptionsFunction = nullptr;
    void* setConstantsFunction = nullptr;

    if (!streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire)) {
        queriedSleep = true;
        sleepResult = originalGetFeatureFunction(streamline_hook_kSLFeatureReflex, "slReflexSleep", sleepFunction);
        if (sleepResult == streamline_hook_kSlResultOk && sleepFunction) {
            const bool hooked = MaybeHookReflexSleep(sleepFunction, false);
            hookedAnything |= hooked;
            if (!hooked && !streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(streamline_hook_g_ReflexSleepProactiveFallbackLogged, "slReflexSleep", sleepFunction);
            }
        }
    }

    if (!streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
        queriedSetOptions = true;
        setOptionsResult = originalGetFeatureFunction(streamline_hook_kSLFeatureReflex, "slReflexSetOptions", setOptionsFunction);
        if (setOptionsResult == streamline_hook_kSlResultOk && setOptionsFunction) {
            const bool hooked = MaybeHookReflexSetOptions(setOptionsFunction, false);
            hookedAnything |= hooked;
            if (!hooked && !streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(streamline_hook_g_ReflexSetOptionsProactiveFallbackLogged, "slReflexSetOptions",
                                               setOptionsFunction);
            }
        }
    }

    if (!streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
        queriedSetConstants = true;
        setConstantsResult = originalGetFeatureFunction(streamline_hook_kSLFeatureReflex, "slReflexSetConstants", setConstantsFunction);
        if (setConstantsResult == streamline_hook_kSlResultOk && setConstantsFunction) {
            const bool hooked = MaybeHookReflexSetConstants(setConstantsFunction, false);
            hookedAnything |= hooked;
            if (!hooked && !streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
                LogProactiveFeatureHookGapOnce(streamline_hook_g_ReflexSetConstantsProactiveFallbackLogged, "slReflexSetConstants",
                                               setConstantsFunction);
            }
        }
    }

    const bool hooksReady = hookedAnything || streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) ||
                            streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ||
                            streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire);
    if (hooksReady) {
        static std::atomic<bool> s_loggedResolved{false};
        if (!s_loggedResolved.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant(
                "Streamline Hook: Reflex feature hooks resolved (sleepHooked=%d setOptionsHooked=%d "
                "setConstantsHooked=%d sleep=%p setOptions=%p setConstants=%p)",
                streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0,
                streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
                streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ? 1 : 0, sleepFunction, setOptionsFunction,
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

inline uint32_t QueryCapabilityMax(const slViewportHandle& viewport, const slDLSSGOptions* streamline_hook_options) {
    if (!GetCallableOriginalDLSSGGetState() && !TryResolveDLSSGFeatureHooks()) {
        return 0;
    }
    auto originalGetState = GetCallableOriginalDLSSGGetState();
    if (!originalGetState) {
        return 0;
    }

    slDLSSGState state;
    const slResult result = originalGetState(viewport, state, streamline_hook_options);
    if (result != streamline_hook_kSlResultOk || state.numFramesToGenerateMax == 0) {
        return 0;
    }

    const uint32_t viewportKey = GetViewportKey(viewport);
    CacheCapabilityMax(viewportKey, state.numFramesToGenerateMax);
    return state.numFramesToGenerateMax;
}

inline void RegisterDynamicHooksOnce() {
    if (streamline_hook_g_DynamicHooksRegistered.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    IATHook::RegisterDynamicHookFiltered("slGetFeatureFunction", reinterpret_cast<void*>(Hooked_slGetFeatureFunction),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slGetFeatureFunction),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slGetPluginFunction", reinterpret_cast<void*>(Hooked_slGetPluginFunction),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slGetPluginFunction),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slSetD3DDevice", reinterpret_cast<void*>(Hooked_slSetD3DDevice),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slSetD3DDevice),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slSetTag", reinterpret_cast<void*>(Hooked_slSetTag),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slSetTag),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slSetTagForFrame", reinterpret_cast<void*>(Hooked_slSetTagForFrame),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slSetTagForFrame),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slEvaluateFeature", reinterpret_cast<void*>(Hooked_slEvaluateFeature),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slEvaluateFeature),
                                         IsStreamlineCoreDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slDLSSGSetOptions", reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slDLSSGSetOptions),
                                         IsStreamlineDLSSGDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slDLSSGGetState", reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slDLSSGGetState),
                                         IsStreamlineDLSSGDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slReflexSleep", reinterpret_cast<void*>(Hooked_slReflexSleep),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSleep),
                                         IsStreamlineReflexDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slReflexSetOptions", reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSetOptions),
                                         IsStreamlineReflexDynamicHookModule);
    IATHook::RegisterDynamicHookFiltered("slReflexSetConstants", reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                                         reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSetConstants),
                                         IsStreamlineReflexDynamicHookModule);
    HookLogImportant(
        "Streamline Hook: Registered module-filtered dynamic hooks for core Streamline exports and owned feature "
        "exports");
}

inline bool InstallHooksForModule(HMODULE module, const char* moduleNameOrPath) {
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

    if (moduleBit != 0 && (streamline_hook_g_InstalledModuleMask.load(std::memory_order_acquire) & moduleBit) != 0) {
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
        const bool anyCoreHookTargetWithinModule = targetWithinModule(streamline_hook_g_SLGetFeatureFunctionTarget, module) ||
                                                   targetWithinModule(streamline_hook_g_SLGetPluginFunctionTarget, module) ||
                                                   targetWithinModule(streamline_hook_g_SLSetD3DDeviceTarget, module) ||
                                                   targetWithinModule(streamline_hook_g_SLSetTagTarget, module) ||
                                                   targetWithinModule(streamline_hook_g_SLSetTagForFrameTarget, module) ||
                                                   targetWithinModule(streamline_hook_g_SLEvaluateFeatureTarget, module);
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
            {"slGetFeatureFunction", &streamline_hook_g_SLGetFeatureFunctionTarget, &streamline_hook_g_SLGetFeatureFunctionHooked,
             reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slGetFeatureFunction)},
            {"slGetPluginFunction", &streamline_hook_g_SLGetPluginFunctionTarget, &streamline_hook_g_SLGetPluginFunctionHooked,
             reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slGetPluginFunction)},
            {"slSetD3DDevice", &streamline_hook_g_SLSetD3DDeviceTarget, &streamline_hook_g_SLSetD3DDeviceHooked,
             reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slSetD3DDevice)},
            {"slSetTag", &streamline_hook_g_SLSetTagTarget, &streamline_hook_g_SLSetTagHooked, reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slSetTag)},
            {"slSetTagForFrame", &streamline_hook_g_SLSetTagForFrameTarget, &streamline_hook_g_SLSetTagForFrameHooked,
             reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slSetTagForFrame)},
            {"slEvaluateFeature", &streamline_hook_g_SLEvaluateFeatureTarget, &streamline_hook_g_SLEvaluateFeatureHooked,
             reinterpret_cast<void* volatile*>(&streamline_hook_g_Original_slEvaluateFeature)},
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
        streamline_hook_g_InstalledModuleMask.fetch_and(~moduleBit, std::memory_order_acq_rel);
        streamline_hook_g_IATPatchesMask.fetch_and(~moduleBit, std::memory_order_acq_rel);
    }

    if (!shouldHookCoreExports && (originalGetFeatureFunction || originalGetPluginFunction || originalSetD3DDevice)) {
        LogSkippedStreamlineCoreExportsOnce(moduleBaseName, module, originalGetFeatureFunction != nullptr,
                                            originalGetPluginFunction != nullptr, originalSetD3DDevice != nullptr);
    }

    bool hookedAnything = false;
    {
        std::lock_guard<std::mutex> lock(streamline_hook_g_ModuleHookMutex);

        if (shouldHookCoreExports && originalGetFeatureFunction) {
            if (!streamline_hook_g_Original_slGetFeatureFunction) {
                streamline_hook_g_Original_slGetFeatureFunction = originalGetFeatureFunction;
            }

            hookedAnything |= InstallInlineHookOnce(reinterpret_cast<void*>(originalGetFeatureFunction),
                                                    reinterpret_cast<void*>(Hooked_slGetFeatureFunction),
                                                    streamline_hook_g_Original_slGetFeatureFunction, streamline_hook_g_SLGetFeatureFunctionHooked,
                                                    streamline_hook_g_SLGetFeatureFunctionTarget, "slGetFeatureFunction");
        }

        if (shouldHookCoreExports && originalGetPluginFunction) {
            if (!streamline_hook_g_Original_slGetPluginFunction) {
                streamline_hook_g_Original_slGetPluginFunction = originalGetPluginFunction;
            }

            hookedAnything |= InstallInlineHookOnce(reinterpret_cast<void*>(originalGetPluginFunction),
                                                    reinterpret_cast<void*>(Hooked_slGetPluginFunction),
                                                    streamline_hook_g_Original_slGetPluginFunction, streamline_hook_g_SLGetPluginFunctionHooked,
                                                    streamline_hook_g_SLGetPluginFunctionTarget, "slGetPluginFunction");
        }

        if (shouldHookCoreExports && originalSetD3DDevice) {
            if (!streamline_hook_g_Original_slSetD3DDevice) {
                streamline_hook_g_Original_slSetD3DDevice = originalSetD3DDevice;
            }

            hookedAnything |= InstallInlineHookOnce(
                reinterpret_cast<void*>(originalSetD3DDevice), reinterpret_cast<void*>(Hooked_slSetD3DDevice),
                streamline_hook_g_Original_slSetD3DDevice, streamline_hook_g_SLSetD3DDeviceHooked, streamline_hook_g_SLSetD3DDeviceTarget, "slSetD3DDevice");
        }

        if (shouldHookCoreExports && originalSetTag) {
            if (!streamline_hook_g_Original_slSetTag) {
                streamline_hook_g_Original_slSetTag = originalSetTag;
            }

            hookedAnything |=
                InstallInlineHookOnce(reinterpret_cast<void*>(originalSetTag), reinterpret_cast<void*>(Hooked_slSetTag),
                                      streamline_hook_g_Original_slSetTag, streamline_hook_g_SLSetTagHooked, streamline_hook_g_SLSetTagTarget, "slSetTag");
        }

        if (shouldHookCoreExports && originalSetTagForFrame) {
            if (!streamline_hook_g_Original_slSetTagForFrame) {
                streamline_hook_g_Original_slSetTagForFrame = originalSetTagForFrame;
            }

            hookedAnything |= InstallInlineHookOnce(
                reinterpret_cast<void*>(originalSetTagForFrame), reinterpret_cast<void*>(Hooked_slSetTagForFrame),
                streamline_hook_g_Original_slSetTagForFrame, streamline_hook_g_SLSetTagForFrameHooked, streamline_hook_g_SLSetTagForFrameTarget, "slSetTagForFrame");
        }

        if (shouldHookCoreExports && originalEvaluateFeature) {
            if (!streamline_hook_g_Original_slEvaluateFeature) {
                streamline_hook_g_Original_slEvaluateFeature = originalEvaluateFeature;
            }

            hookedAnything |=
                InstallInlineHookOnce(reinterpret_cast<void*>(originalEvaluateFeature),
                                      reinterpret_cast<void*>(Hooked_slEvaluateFeature), streamline_hook_g_Original_slEvaluateFeature,
                                      streamline_hook_g_SLEvaluateFeatureHooked, streamline_hook_g_SLEvaluateFeatureTarget, "slEvaluateFeature");
        }

        if (shouldHookCoreExports && moduleBit != 0 &&
            (streamline_hook_g_IATPatchesMask.load(std::memory_order_acquire) & moduleBit) == 0) {
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
            streamline_hook_g_IATPatchesMask.fetch_or(moduleBit, std::memory_order_acq_rel);
        }

        if (originalDLSSGSetOptions && ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad(
                                           "slDLSSGSetOptions", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slDLSSGSetOptions", reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                reinterpret_cast<void*>(originalDLSSGSetOptions),
                reinterpret_cast<void**>(&streamline_hook_g_Original_slDLSSGSetOptions), "slDLSSGSetOptions");
        }

        if (originalDLSSGGetState &&
            ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slDLSSGGetState", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slDLSSGGetState", reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                reinterpret_cast<void*>(originalDLSSGGetState), reinterpret_cast<void**>(&streamline_hook_g_Original_slDLSSGGetState),
                "slDLSSGGetState");
        }

        if (originalReflexSleep &&
            ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slReflexSleep", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSleep", reinterpret_cast<void*>(Hooked_slReflexSleep),
                reinterpret_cast<void*>(originalReflexSleep), reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSleep),
                "slReflexSleep");
        }

        if (originalReflexSetOptions && ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad(
                                            "slReflexSetOptions", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSetOptions", reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                reinterpret_cast<void*>(originalReflexSetOptions),
                reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSetOptions), "slReflexSetOptions");
        }

        if (originalReflexSetConstants && ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad(
                                              "slReflexSetConstants", moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSetConstants", reinterpret_cast<void*>(Hooked_slReflexSetConstants),
                reinterpret_cast<void*>(originalReflexSetConstants),
                reinterpret_cast<void**>(&streamline_hook_g_Original_slReflexSetConstants), "slReflexSetConstants");
        }
    }

    if (hookedAnything) {
        if (moduleBit != 0) {
            streamline_hook_g_InstalledModuleMask.fetch_or(moduleBit, std::memory_order_acq_rel);
        }
        HookLogImportant("Streamline Hook: Installed hooks for %s (%p)", moduleBaseName, module);
    }
    return true;
}

inline bool OpenLoadedModuleSnapshotWithRetry(HANDLE& snapshot, MODULEENTRY32& firstEntry, DWORD& error, int& attempts,
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

inline bool ScanLoadedStreamlineModules() {
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    MODULEENTRY32 entry = {};
    DWORD error = ERROR_SUCCESS;
    int attempts = 0;
    bool failedOnFirstEntry = false;
    if (!OpenLoadedModuleSnapshotWithRetry(snapshot, entry, error, attempts, failedOnFirstEntry)) {
        if (!streamline_hook_g_ModuleSnapshotFailureLogged.exchange(true, std::memory_order_acq_rel)) {
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

    streamline_hook_g_ModuleSnapshotFailureLogged.store(false, std::memory_order_release);

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

    if (attempts > 1 && !streamline_hook_g_ModuleSnapshotRetrySuccessLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "Streamline Hook: Loaded-module snapshot recovered after transient retry (attempts=%d modules=%zu "
            "hooked=%zu)",
            attempts, streamlineModuleCount, hookedModuleCount);
    }
    if (iterationError != ERROR_SUCCESS && iterationError != ERROR_NO_MORE_FILES &&
        !streamline_hook_g_ModuleSnapshotFailureLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "Streamline Hook: Loaded-module enumeration ended unexpectedly for feature hooks error=%lu "
            "(modules=%zu hooked=%zu)",
            static_cast<unsigned long>(iterationError), streamlineModuleCount, hookedModuleCount);
    }
    return foundModule;
}

inline bool AreReflexFeatureHooksComplete() {
    return streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) &&
           streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire) &&
           streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire);
}

inline void RetryResolveReflexFeatureHooksForRuntimeActivity(const char* source) {
    if (AreReflexFeatureHooksComplete()) {
        return;
    }

    constexpr ULONGLONG kRetryIntervalMs = 2500;
    const ULONGLONG nowMs = GetTickCount64();
    ULONGLONG previousMs = streamline_hook_g_ReflexFeatureHookRetryLastMs.load(std::memory_order_acquire);
    if (previousMs != 0 && nowMs >= previousMs && (nowMs - previousMs) < kRetryIntervalMs) {
        return;
    }

    if (!streamline_hook_g_ReflexFeatureHookRetryLastMs.compare_exchange_strong(previousMs, nowMs, std::memory_order_acq_rel,
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
            streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0,
            streamline_hook_g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
            streamline_hook_g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_ReflexLimiter.IsManualLimiterConfiguredOrActive() ? 1 : 0, g_ReflexLimiter.GetTargetIntervalUs());
    }
}

inline slResult Hooked_slDLSSGGetState(const slViewportHandle& viewport, slDLSSGState& state, const slDLSSGOptions* streamline_hook_options) {
    auto originalGetState = GetCallableOriginalDLSSGGetState();
    if (!originalGetState) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    // Newer integrations can configure DLSS-G by passing options directly to GetState, after
    // slSetTagForFrame has already made the activation input volatile. Keep the latest inactive
    // DX12 UI tag covered before entering GetState so a late OFF->ON observation can adopt it.
    if (!ShouldKeepPureObserverOnlyStreamlineBehavior() && streamline_hook_g_StreamlineUsesD3D12.load(std::memory_order_acquire) &&
        !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        const uint32_t requestedOutputs = streamline_hook_options ? std::clamp(streamline_hook_options->numFramesToGenerate + 1u, 1u, 6u) : 2u;
        ce::dx12_streamline_ui_overlay::BeginPreactivationStandby(requestedOutputs);
    }

    const slResult result = originalGetState(viewport, state, streamline_hook_options);
    RetryResolveReflexFeatureHooksForRuntimeActivity("slDLSSGGetState");
    const uint32_t viewportKey = GetViewportKey(viewport);
    const bool viewportWasActive = WasViewportRuntimeStateActive(viewportKey);
    const bool hasRuntimeFenceEvidence = HasDLSSGRuntimeFenceEvidence(state);
    const bool suppressNewActivation = ShouldSuppressNewGetStateActivation();
    if (result == streamline_hook_kSlResultOk && state.numFramesToGenerateMax > 0) {
        CacheCapabilityMax(viewportKey, state.numFramesToGenerateMax);
    }

    const uint32_t capabilityMax =
        state.numFramesToGenerateMax > 0 ? state.numFramesToGenerateMax : GetCachedCapabilityMax(viewportKey);
    const auto runtimeEvaluation = ce::streamline_runtime_policy::EvaluateViewportRuntimeUpdateFromGetState(
        result == streamline_hook_kSlResultOk, streamline_hook_options != nullptr, viewportWasActive, hasRuntimeFenceEvidence, suppressNewActivation,
        streamline_hook_options ? streamline_hook_options->mode : 0, streamline_hook_options ? streamline_hook_options->numFramesToGenerate : 0u, capabilityMax);
    const bool clearAllViewportStatesForDisable =
        runtimeEvaluation.update.shouldUpdate &&
        ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForGetStateDisable(
            result == streamline_hook_kSlResultOk, streamline_hook_options != nullptr, hasRuntimeFenceEvidence, streamline_hook_options ? streamline_hook_options->mode : 0u,
            capabilityMax);
    if (result == streamline_hook_kSlResultOk && streamline_hook_options != nullptr) {
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
                viewportKey, GetDLSSGModeName(streamline_hook_options->mode), streamline_hook_options->mode, streamline_hook_options->numFramesToGenerate,
                capabilityMax, state.numFramesActuallyPresented, state.status, statusText, state.minWidthOrHeight,
                static_cast<int>(state.bIsVsyncSupportAvailable), static_cast<int>(state.bIsDynamicMFGSupported),
                (unsigned long long)(state.estimatedVRAMUsageInBytes / (1024ull * 1024ull)),
                state.inputsProcessingCompletionFence,
                (unsigned long long)state.lastPresentInputsProcessingCompletionFenceValue, viewportWasActive ? 1 : 0,
                runtimeEvaluation.update.shouldUpdate ? 1 : 0, runtimeEvaluation.update.active ? 1 : 0,
                clearAllViewportStatesForDisable ? 1 : 0, suppressNewActivation ? 1 : 0,
                hasRuntimeFenceEvidence ? 1 : 0, streamline_hook_g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
                reinterpret_cast<void*>(streamline_hook_g_Original_slDLSSGSetOptions));
        }
    }

    // [DLSSG HEALTH] — session 20260702_094955: GTA reported DLSSG ON (optionsMode=on, updateActive=1) but
    // presents stayed at base rate all session (numFramesActuallyPresented==1, no fps gain). sl.dlss_g
    // publishes WHY it declines to interpolate in DLSSGState.status; log every status transition, and while
    // the game requests ON without interpolation evidence, emit a deterministic streak warning that pairs
    // NVIDIA's status decode with Reflex call-activity evidence (DLSSG hard-requires Reflex, and GTA's
    // Reflex is historically flaky even without CE).
    if (result == streamline_hook_kSlResultOk) {
        const uint32_t previousStatus = streamline_hook_g_DLSSGLastObservedStatus.exchange(state.status, std::memory_order_relaxed);
        if (previousStatus != state.status) {
            char prevText[160];
            char nowText[160];
            FormatDLSSGStatusFlags(previousStatus, prevText, sizeof(prevText));
            FormatDLSSGStatusFlags(state.status, nowText, sizeof(nowText));
            HookLogImportant(
                "Streamline Hook: [DLSSG HEALTH] status TRANSITION 0x%X(%s) -> 0x%X(%s) (viewport=%u "
                "optionsMode=%s presented=%u minWH=%u vsyncOk=%d dynMFG=%d)",
                previousStatus, prevText, state.status, nowText, viewportKey,
                streamline_hook_options ? GetDLSSGModeName(streamline_hook_options->mode) : "n/a", state.numFramesActuallyPresented,
                state.minWidthOrHeight, static_cast<int>(state.bIsVsyncSupportAvailable),
                static_cast<int>(state.bIsDynamicMFGSupported));
        }
    }
    const bool optionsRequestOn = streamline_hook_options != nullptr && streamline_hook_options->mode != 0;
    if (ce::streamline_runtime_policy::ShouldTrackDLSSGActivationHealthSample(result == streamline_hook_kSlResultOk,
                                                                              optionsRequestOn)) {
        const bool interpolationEvidence =
            ce::streamline_runtime_policy::IsDLSSGInterpolationPresentEvidence(state.numFramesActuallyPresented);
        uint64_t streak = 0;
        if (interpolationEvidence && state.status == 0) {
            streamline_hook_g_DLSSGNotInterpolatingStreak.store(0, std::memory_order_relaxed);
        } else {
            streak = streamline_hook_g_DLSSGNotInterpolatingStreak.fetch_add(1, std::memory_order_relaxed) + 1;
        }
        if (ce::streamline_runtime_policy::ShouldWarnDLSSGActiveButNotInterpolating(streak, streamline_hook_kDLSSGHealthWarnStreak,
                                                                                    streamline_hook_kDLSSGHealthWarnRepeat)) {
            const uint64_t nowMs = GetTickCount64();
            const uint64_t sleepCount = streamline_hook_g_ReflexSleepObservedCount.load(std::memory_order_relaxed);
            const uint64_t sleepCountAtLastLog =
                streamline_hook_g_ReflexSleepCountAtLastHealthLog.exchange(sleepCount, std::memory_order_relaxed);
            const uint64_t sleepLastMs = streamline_hook_g_ReflexSleepLastTickMs.load(std::memory_order_relaxed);
            const uint64_t reflexOptCount = streamline_hook_g_ReflexSetOptionsObservedCount.load(std::memory_order_relaxed);
            const uint64_t reflexOptLastMs = streamline_hook_g_ReflexSetOptionsLastTickMs.load(std::memory_order_relaxed);
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
                streamline_hook_options->numFramesToGenerate, capabilityMax, state.minWidthOrHeight,
                static_cast<int>(state.bIsVsyncSupportAvailable), static_cast<int>(state.bIsDynamicMFGSupported),
                (unsigned long long)(state.estimatedVRAMUsageInBytes / (1024ull * 1024ull)),

                state.inputsProcessingCompletionFence,
                (unsigned long long)state.lastPresentInputsProcessingCompletionFenceValue,
                static_cast<unsigned long long>(sleepCount),
                static_cast<unsigned long long>(sleepCount - sleepCountAtLastLog),
                static_cast<unsigned long long>(sleepLastMs ? (nowMs - sleepLastMs) : 0),
                static_cast<unsigned long long>(reflexOptCount),
                static_cast<unsigned long long>(reflexOptLastMs ? (nowMs - reflexOptLastMs) : 0),
                streamline_hook_g_ReflexLastForwardedMode.load(std::memory_order_relaxed),
                streamline_hook_g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0);
        }
    } else if (result == streamline_hook_kSlResultOk && streamline_hook_options != nullptr && streamline_hook_options->mode == 0) {
        // Explicit OFF request: end any pending not-interpolating streak so a later re-enable starts a
        // fresh, correctly-attributed streak.
        streamline_hook_g_DLSSGNotInterpolatingStreak.store(0, std::memory_order_relaxed);
    }
    if (runtimeEvaluation.update.shouldUpdate) {
        UpdateViewportRuntimeState(viewportKey, runtimeEvaluation.update.active, runtimeEvaluation.update.multiplier,
                                   runtimeEvaluation.update.generatedFrames, runtimeEvaluation.update.capabilityMax,
                                   "GetState", clearAllViewportStatesForDisable);
    } else if (result == streamline_hook_kSlResultOk && streamline_hook_options != nullptr && runtimeEvaluation.suppressedFreshActivation) {
        static std::atomic<int> s_recentFfxTakeoverSuppressedGetStateLogCount{0};
        const int logCount = s_recentFfxTakeoverSuppressedGetStateLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 128) == 0) {
            const ULONGLONG suppressUntilMs = streamline_hook_g_SuppressNewGetStateActivationUntilMs.load(std::memory_order_acquire);
            const ULONGLONG nowMs = GetTickCount64();
            const ULONGLONG remainingMs = suppressUntilMs > nowMs ? (suppressUntilMs - nowMs) : 0;
            const bool persistentBlock =
                streamline_hook_g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.load(std::memory_order_acquire);
            const ULONGLONG startupTransitionUntilMs =
                DXGIShared::g_SharedState.streamlineStartupTransitionUntilMs.load(std::memory_order_acquire);
            const bool startupWindowActive = startupTransitionUntilMs != 0 && startupTransitionUntilMs > nowMs;
            const ULONGLONG startupRemainingMs = startupWindowActive ? (startupTransitionUntilMs - nowMs) : 0;
            HookLogImportant(
                "Streamline Hook: Suppressing fresh GetState DLSS FG reactivation "
                "(viewport=%u mode=%u generated=%u fence=%p fenceValue=%llu persistentBlock=%d startupWindow=%d "
                "startupRemaining=%llums remaining=%llums)",
                viewportKey, streamline_hook_options->mode, streamline_hook_options->numFramesToGenerate, state.inputsProcessingCompletionFence,
                (unsigned long long)state.lastPresentInputsProcessingCompletionFenceValue, persistentBlock ? 1 : 0,
                startupWindowActive ? 1 : 0, (unsigned long long)startupRemainingMs, (unsigned long long)remainingMs);
        }
    }

    if (!IsObserverOnlyModeActive()) {
        std::lock_guard<std::mutex> offLock(streamline_hook_g_SuppressedOffMutex);
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
            streamline_hook_g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
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
        if (streamline_hook_g_SuppressedSetOptionsOffDuringStartup && !shouldKeepDeferred) {
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
                    streamline_hook_g_SuppressedOffViewportKey, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                    safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                    postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                    effectivePostSLRuntimeStateStabilizing);
            } else if (auto originalSetOptions = GetCallableOriginalDLSSGSetOptions()) {
                HookLogImportant(
                    "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) via GetState — startup window "
                    "expired (viewport=%u settling=%d stabilizing=%d activeProofPending=%d)",
                    streamline_hook_g_SuppressedOffViewportKey, postSLConfirmedButStartupSettling ? 1 : 0,
                    effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                    postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0);
                const slResult offResult = originalSetOptions(streamline_hook_g_SuppressedOffViewport, streamline_hook_g_SuppressedOffOptions);
                if (offResult != streamline_hook_kSlResultOk) {
                    HookLogImportant("Streamline Hook: Forwarded slDLSSGSetOptions(OFF) via GetState returned %d",
                                     offResult);
                }
            }
            streamline_hook_g_SuppressedSetOptionsOffDuringStartup = false;
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

inline slResult Hooked_slDLSSGSetOptions(const slViewportHandle& viewport, const slDLSSGOptions& streamline_hook_options) {
    auto originalSetOptions = GetCallableOriginalDLSSGSetOptions();
    if (!originalSetOptions) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    slDLSSGOptions adjustedOptions = CloneDLSSGOptions(streamline_hook_options);
    const uint32_t viewportKey = GetViewportKey(viewport);
    const int configuredFactor = NormalizeDLSSFGFactor(GetActiveGraphicsConfig().parsed.dlssFGFactor);
    const uint32_t originalGeneratedFrames = streamline_hook_options.numFramesToGenerate;
    const bool requestedEnabled = ce::streamline_runtime_policy::IsDLSSGModeEnabled(streamline_hook_options.mode);
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
        std::lock_guard<std::mutex> offLock(streamline_hook_g_SuppressedOffMutex);
        if (streamline_hook_g_SuppressedSetOptionsOffDuringStartup) {
            const bool activationPending =
                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
            HookLogImportant(
                "Streamline Hook: Clearing suppressed slDLSSGSetOptions(OFF) due to explicit re-enable request "
                "(viewport=%u) — Streamline never received the OFF, re-enable is consistent "
                "(activationPending=%d)",
                viewportKey, activationPending ? 1 : 0);
            streamline_hook_g_SuppressedSetOptionsOffDuringStartup = false;
        }
    }

    if (!pureObserverOnly) {
        std::lock_guard<std::mutex> offLock(streamline_hook_g_SuppressedOffMutex);
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
            streamline_hook_g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
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
                streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions.load(std::memory_order_acquire));
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
        if (streamline_hook_g_SuppressedSetOptionsOffDuringStartup && !shouldKeepDeferred) {
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
                    streamline_hook_g_SuppressedOffViewportKey, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                    safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                    postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                    effectivePostSLRuntimeStateStabilizing);
            } else if (auto suppressedOriginalSetOptions = GetCallableOriginalDLSSGSetOptions()) {
                HookLogImportant(
                    "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) — startup window expired "
                    "(viewport=%u settling=%d stabilizing=%d activeProofPending=%d)",
                    streamline_hook_g_SuppressedOffViewportKey, postSLConfirmedButStartupSettling ? 1 : 0,
                    effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                    postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0);
                const slResult offResult =
                    suppressedOriginalSetOptions(streamline_hook_g_SuppressedOffViewport, streamline_hook_g_SuppressedOffOptions);
                if (offResult != streamline_hook_kSlResultOk) {
                    HookLogImportant("Streamline Hook: Forwarded slDLSSGSetOptions(OFF) returned %d", offResult);
                } else {
                    streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions.store(false, std::memory_order_release);
                }
            }
            streamline_hook_g_SuppressedSetOptionsOffDuringStartup = false;
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
        streamline_hook_g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
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
            streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions.load(std::memory_order_acquire));
    const bool acceptActivatedUnconfirmedResumeOff =
        ce::streamline_runtime_policy::ShouldAcceptOffSignalDuringActivatedUnconfirmedStreamlineResume(
            requestedDisabled, startupWindowActive, startupProtectedComebackProof, startupActivationPending,
            postSLActiveButUnconfirmed, postSLStartupActivationEntered, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling, effectivePostSLRuntimeStateStabilizing);
    const bool suppressOffCall =
        !pureObserverOnly && requestedDisabled && !explicitSetOptionsDisableIsAuthoritative &&
        !acceptActivatedUnconfirmedResumeOff &&
        !ce::streamline_runtime_policy::ShouldAcceptInactiveStreamlineSignalAfterConfirmedReflexSuspend(
            streamline_hook_g_ConfirmedDLSSReflexSuspendPending.load(std::memory_order_acquire), requestedDisabled,
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
                viewportKey, streamline_hook_options.mode, startupWindowActive ? 1 : 0, HookHasFSRFGHistory() ? 1 : 0,
                explicitSetOptionsActivationForCurrentComeback ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0,
                startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                postSLConfirmedButStartupSettling ? 1 : 0, effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0,
                static_cast<unsigned long long>(suppressionLogCount));
        }
        MarkStartupProtectedOffChurnObserved("SetOptions", postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                                             effectivePostSLRuntimeStateStabilizing);
        {
            std::lock_guard<std::mutex> offLock(streamline_hook_g_SuppressedOffMutex);
            streamline_hook_g_SuppressedSetOptionsOffDuringStartup = true;
            streamline_hook_g_SuppressedOffViewport = viewport;
            streamline_hook_g_SuppressedOffOptions = adjustedOptions;
            streamline_hook_g_SuppressedOffViewportKey = viewportKey;
        }
        result = streamline_hook_kSlResultOk;
    } else {
        if (acceptActivatedUnconfirmedResumeOff) {
            LogAcceptedOffDuringActivatedUnconfirmedResume(
                "SetOptions", startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                postSLStartupActivationEntered, postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                effectivePostSLRuntimeStateStabilizing);
            ResetStartupProtectedOffChurnActiveProof("forwarded activated-unconfirmed SetOptions disable");
        } else if (explicitSetOptionsDisableIsAuthoritative) {
            static std::atomic<uint64_t> s_authoritativeSetOptionsOffLogCount{0};
            const uint64_t logCount = s_authoritativeSetOptionsOffLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "Streamline Hook: Accepting explicit slDLSSGSetOptions(OFF) as authoritative after confirmed "
                    "PostSL rendering (viewport=%u startupWindow=%d hadFSR=%d safeBootstrap=%d pending=%d "
                    "unconfirmed=%d settling=%d stabilizing=%d activeProofPending=%d log=%llu)",
                    viewportKey, startupWindowActive ? 1 : 0, hadFSRFGPhase ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0,
                    startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                    postSLConfirmedButStartupSettling ? 1 : 0, effectivePostSLRuntimeStateStabilizing ? 1 : 0,
                    postSLConfirmedButOffChurnAwaitingActiveProof ? 1 : 0, static_cast<unsigned long long>(logCount));
            }
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

    if (!pureObserverOnly && requestedEnabled && result != streamline_hook_kSlResultOk &&
        !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        ce::dx12_streamline_ui_overlay::EndActivation("slDLSSGSetOptions enable failed");
    }

    LogDLSSGSetOptionsTransition(viewportKey, streamline_hook_options, adjustedOptions, originalGeneratedFrames, capabilityMax,
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

    if (result == streamline_hook_kSlResultOk) {
        if (!pureObserverOnly && requestedEnabled) {
            streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions.store(false, std::memory_order_release);
            const ULONGLONG previousSuppressUntilMs =
                streamline_hook_g_SuppressNewGetStateActivationUntilMs.exchange(0, std::memory_order_acq_rel);
            const bool wasBlockingGetStateOnlyReactivation =
                streamline_hook_g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.exchange(false, std::memory_order_acq_rel);
            const bool wasBlockingUnsafePostFSRGetStateOnlyReactivation =
                streamline_hook_g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap.exchange(false, std::memory_order_acq_rel);
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
                       result == streamline_hook_kSlResultOk, setOptionsCallSuppressed)) {
            ResetStartupProtectedOffChurnActiveProof("forwarded explicit SetOptions disable");
            streamline_hook_g_SuppressNewGetStateActivationUntilMs.store(0, std::memory_order_release);
            const bool wasBlockingGetStateOnlyReactivation =
                streamline_hook_g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.exchange(true, std::memory_order_acq_rel);
            if (!wasBlockingGetStateOnlyReactivation) {
                HookLogImportant(
                    "Streamline Hook: Re-armed persistent GetState-only DLSS FG suppression due to explicit "
                    "slDLSSGSetOptions disable request (viewport=%u)",
                    viewportKey);
            }
        }

        if (ce::streamline_runtime_policy::ShouldApplyViewportRuntimeUpdateFromSetOptions(result == streamline_hook_kSlResultOk,
                                                                                          setOptionsCallSuppressed)) {
            const auto runtimeUpdate = ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromRequestedOptions(
                true, true, adjustedOptions.mode, adjustedOptions.numFramesToGenerate, capabilityMax);
            const bool clearAllViewportStatesForDisable =
                ce::streamline_runtime_policy::ShouldClearAllViewportRuntimeStatesForSetOptionsDisable(
                    result == streamline_hook_kSlResultOk, setOptionsCallSuppressed, adjustedOptions.mode);
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
                streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions.exchange(false, std::memory_order_acq_rel);
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
inline slResult SlNullFunctionStub() {
    return streamline_hook_kSlResultOk;
}

inline void* Hooked_slGetPluginFunction(const char* streamline_hook_functionName) {
    if (StreamlineHook::IsExternalOverlayPresentGuardActive()) {
        static std::atomic<int> s_externalOverlaySuppressedPluginLookupLogCount{0};
        const int logCount = s_externalOverlaySuppressedPluginLookupLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "Streamline Hook: Suppressing re-entrant slGetPluginFunction during guarded external overlay "
                "Present (name=%s depth=%d)",
                streamline_hook_functionName ? streamline_hook_functionName : "null", streamline_hook_g_ExternalOverlayPresentGuardDepth);
        }
        return reinterpret_cast<void*>(SlNullFunctionStub);
    }

    auto originalGetPluginFunction = GetCallableOriginalGetPluginFunction();
    if (!originalGetPluginFunction) {
        return streamline_hook_g_Original_slGetPluginFunction ? reinterpret_cast<void*>(SlNullFunctionStub) : nullptr;
    }

    return originalGetPluginFunction(streamline_hook_functionName);
}

inline slResult Hooked_slGetFeatureFunction(uint32_t feature, const char* streamline_hook_functionName, void*& streamline_hook_function) {
    if (StreamlineHook::IsExternalOverlayPresentGuardActive()) {
        static std::atomic<int> s_externalOverlaySuppressedLookupLogCount{0};
        const int logCount = s_externalOverlaySuppressedLookupLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "Streamline Hook: Suppressing re-entrant slGetFeatureFunction during guarded external overlay "
                "Present (feature=%u name=%s depth=%d)",
                feature, streamline_hook_functionName ? streamline_hook_functionName : "null", streamline_hook_g_ExternalOverlayPresentGuardDepth);
        }
        streamline_hook_function = reinterpret_cast<void*>(SlNullFunctionStub);
        return streamline_hook_kSlResultErrorInvalidState;
    }

    auto originalGetFeatureFunction = GetCallableOriginalGetFeatureFunction();
    if (!originalGetFeatureFunction) {
        streamline_hook_function = reinterpret_cast<void*>(SlNullFunctionStub);
        return streamline_hook_kSlResultErrorInvalidState;
    }

    const slResult result = originalGetFeatureFunction(feature, streamline_hook_functionName, streamline_hook_function);
    // Safety: if the original returned success but gave us NULL, the caller
    // would call through NULL → RIP=0 crash.  This can happen when third-party
    // overlays (e.g., Steam's OverlayHookD3D3) call slGetFeatureFunction
    // re-entrantly from within Streamline's own code during FG processing.
    // Substitute a safe no-op stub so the caller doesn't crash even if it
    // ignores the error return and uses the function pointer directly.
    if (result == streamline_hook_kSlResultOk && !streamline_hook_function) {
        static std::atomic<int> s_nullFunctionLogCount{0};
        if (s_nullFunctionLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            HookLogImportant(
                "Streamline Hook: slGetFeatureFunction returned OK with NULL function "
                "(feature=%u name=%s) — substituting safe no-op stub to prevent null call crash",
                feature, streamline_hook_functionName ? streamline_hook_functionName : "null");
        }
        streamline_hook_function = reinterpret_cast<void*>(SlNullFunctionStub);
        return streamline_hook_kSlResultOk;
    }
    if (result != streamline_hook_kSlResultOk || !streamline_hook_functionName || !streamline_hook_function) {
        return result;
    }

    // DLSS Frame Generation feature hooks
    if (feature == streamline_hook_kSLFeatureDLSSG) {
        if (strcmp(streamline_hook_functionName, "slDLSSGSetOptions") == 0) {
            void* originalFunction = streamline_hook_function;
            const bool hookReady = MaybeHookDLSSGSetOptions(streamline_hook_function, true);
            LogFeatureLookupOutcomeOnce(streamline_hook_g_DLSSGSetOptionsLookupLogged, "slDLSSGSetOptions", originalFunction, streamline_hook_function,
                                        hookReady);
        } else if (strcmp(streamline_hook_functionName, "slDLSSGGetState") == 0) {
            void* originalFunction = streamline_hook_function;
            const bool hookReady = MaybeHookDLSSGGetState(streamline_hook_function, true);
            LogFeatureLookupOutcomeOnce(streamline_hook_g_DLSSGGetStateLookupLogged, "slDLSSGGetState", originalFunction, streamline_hook_function,
                                        hookReady);
            // Talos resolves GetState shortly before it starts tagging the activation inputs, but
            // never resolves/calls SetOptions. Arm standby at pointer delivery, before those tags.
            if (!ShouldKeepPureObserverOnlyStreamlineBehavior() &&
                streamline_hook_g_StreamlineUsesD3D12.load(std::memory_order_acquire)) {
                ce::dx12_streamline_ui_overlay::BeginPreactivationStandby(2);
            }
        }
    }
    // Reflex feature hook — detect game activation of native Reflex
    else if (feature == streamline_hook_kSLFeatureReflex) {
        if (strcmp(streamline_hook_functionName, "slReflexSleep") == 0) {
            void* originalFunction = streamline_hook_function;
            const bool hookReady = MaybeHookReflexSleep(streamline_hook_function, true);
            LogFeatureLookupOutcomeOnce(streamline_hook_g_ReflexSleepLookupLogged, "slReflexSleep", originalFunction, streamline_hook_function,
                                        hookReady);
        } else if (strcmp(streamline_hook_functionName, "slReflexSetOptions") == 0) {
            void* originalFunction = streamline_hook_function;
            const bool hookReady = MaybeHookReflexSetOptions(streamline_hook_function, true);
            LogFeatureLookupOutcomeOnce(streamline_hook_g_ReflexSetOptionsLookupLogged, "slReflexSetOptions", originalFunction,
                                        streamline_hook_function, hookReady);
        } else if (strcmp(streamline_hook_functionName, "slReflexSetConstants") == 0) {
            void* originalFunction = streamline_hook_function;
            const bool hookReady = MaybeHookReflexSetConstants(streamline_hook_function, true);
            LogFeatureLookupOutcomeOnce(streamline_hook_g_ReflexSetConstantsLookupLogged, "slReflexSetConstants", originalFunction,
                                        streamline_hook_function, hookReady);
        }
    }

    return result;
}

inline slResult Hooked_slSetD3DDevice(void* streamline_hook_d3dDevice) {
    auto originalSetD3DDevice = GetCallableOriginalSetD3DDevice();
    if (!originalSetD3DDevice) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    ID3D12Device* acceptedD3D12Device = nullptr;
    if (streamline_hook_d3dDevice) {
        static_cast<IUnknown*>(streamline_hook_d3dDevice)->QueryInterface(IID_PPV_ARGS(&acceptedD3D12Device));
    }
    const bool isD3D12 = acceptedD3D12Device != nullptr;

    const slResult result = originalSetD3DDevice(streamline_hook_d3dDevice);
    if (result == streamline_hook_kSlResultOk) {
        ID3D12Device* previousAcceptedDevice = nullptr;
        {
            std::lock_guard<std::mutex> lock(streamline_hook_g_AcceptedD3D12DeviceMutex);
            previousAcceptedDevice = streamline_hook_g_AcceptedD3D12Device;
            streamline_hook_g_AcceptedD3D12Device = acceptedD3D12Device;
            acceptedD3D12Device = nullptr;
        }
        if (previousAcceptedDevice) {
            previousAcceptedDevice->Release();
        }
        streamline_hook_g_StreamlineUsesD3D12.store(isD3D12, std::memory_order_release);
        if (isD3D12 && !ShouldKeepPureObserverOnlyStreamlineBehavior()) {
            // Resource tags are legal immediately after Streamline accepts the device. Some
            // integrations (Talos) publish their reusable UI tag before resolving any DLSS-G
            // feature function, so GetState-pointer delivery is too late to cover that tag.
            ce::dx12_streamline_ui_overlay::BeginPreactivationStandby(2);
            HookLogImportant(
                "Streamline Hook: D3D12 device accepted — official UI preactivation standby ready before tags "
                "(device=%p)",
                streamline_hook_d3dDevice);
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

inline bool StructTypesEqual(const slStructType& lhs, const slStructType& rhs) {
    return lhs.data1 == rhs.data1 && lhs.data2 == rhs.data2 && lhs.data3 == rhs.data3 &&
           std::memcmp(lhs.data4, rhs.data4, sizeof(lhs.data4)) == 0;
}

inline bool TryRecordOfficialUiResourceTag(const void* frameToken, const slResourceTag& tag, void* streamline_hook_commandBuffer) {
    if (ShouldKeepPureObserverOnlyStreamlineBehavior() || !streamline_hook_g_StreamlineUsesD3D12.load(std::memory_order_acquire) ||
        !streamline_hook_commandBuffer || tag.type != streamline_hook_kSLBufferTypeUIColorAndAlpha || !tag.resource || !tag.resource->native ||
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
    const bool hdr = DX12_ResolveRuntimeOwnedOverlayTargetHDRState(format);
    ID3D12CommandQueue* initializationQueue = DX12_AcquireOriginalGameQueueForOverlay();
    if (!initializationQueue) {
        return false;
    }

    ce::dx12_streamline_ui_overlay::RecordRequest request;
    request.commandList = static_cast<ID3D12GraphicsCommandList*>(streamline_hook_commandBuffer);
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

inline uint32_t LogOfficialUiTagOpportunity(const char* tagApi, const void* frameToken, uint32_t viewportKey,
                                     const slResourceTag* tags, uint32_t numTags, void* streamline_hook_commandBuffer,
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
        streamline_hook_commandBuffer, streamline_hook_g_StreamlineUsesD3D12.load(std::memory_order_relaxed) ? 1 : 0);
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

inline void TryRecordOfficialUiTag(const char* tagApi, const void* frameToken, const slViewportHandle& viewport,
                            const slResourceTag* tags, uint32_t numTags, void* streamline_hook_commandBuffer) {
    const bool wantsUiBootstrapRecord = ce::dx12_streamline_ui_overlay::OnFrameTag(frameToken);

    if (wantsUiBootstrapRecord) {
        LogOfficialUiTagOpportunity(tagApi, frameToken, GetViewportKey(viewport), tags, numTags, streamline_hook_commandBuffer);
    }

    // DLSS-G consumes UIColorAndAlpha before its first generated output exists, while PostSL can
    // only run after that output has been produced. Record CE's rolling/one-shot overlay into the
    // official UI layer on the app-provided command list. Source frames keep replacing the
    // eValidUntilPresent record until PostSL consumes the bounded output handoff. This introduces
    // no copy, extra submission, queue, or wait and naturally follows Streamline's synchronization.
    if (wantsUiBootstrapRecord && tags) {
        for (uint32_t i = 0; i < numTags; ++i) {
            if (TryRecordOfficialUiResourceTag(frameToken, tags[i], streamline_hook_commandBuffer)) {
                break;
            }
        }
    }
}

inline slResult Hooked_slSetTag(const slViewportHandle& viewport, const slResourceTag* tags, uint32_t numTags,
                         void* streamline_hook_commandBuffer) {
    auto originalSetTag = GetCallableOriginalSetTag();
    if (!originalSetTag) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    // Legacy/global resource tagging has no frame token. A monotonically unique opaque identity
    // lets the standby state roll across calls without dereferencing or fabricating an SL object.
    static std::atomic<uintptr_t> s_legacyTagToken{1};
    const uintptr_t tokenValue = s_legacyTagToken.fetch_add(1, std::memory_order_relaxed);
    const void* frameToken = reinterpret_cast<const void*>((tokenValue << 1u) | 1u);
    TryRecordOfficialUiTag("slSetTag", frameToken, viewport, tags, numTags, streamline_hook_commandBuffer);

    return originalSetTag(viewport, tags, numTags, streamline_hook_commandBuffer);
}

inline slResult Hooked_slSetTagForFrame(const slBaseStructure& streamline_hook_frame, const slViewportHandle& viewport,
                                 const slResourceTag* tags, uint32_t numTags, void* streamline_hook_commandBuffer) {
    auto originalSetTagForFrame = GetCallableOriginalSetTagForFrame();
    if (!originalSetTagForFrame) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    TryRecordOfficialUiTag("slSetTagForFrame", &streamline_hook_frame, viewport, tags, numTags, streamline_hook_commandBuffer);

    // Streamline observes the resource only after CE's commands have been appended. For volatile
    // tags this is essential: any copy Streamline records into the same command list includes CE.
    return originalSetTagForFrame(streamline_hook_frame, viewport, tags, numTags, streamline_hook_commandBuffer);
}

inline slResult Hooked_slEvaluateFeature(uint32_t feature, const slBaseStructure& streamline_hook_frame, const slBaseStructure** inputs,
                                  uint32_t numInputs, void* streamline_hook_commandBuffer) {
    auto originalEvaluateFeature = GetCallableOriginalEvaluateFeature();
    if (!originalEvaluateFeature) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    // Streamline explicitly permits ResourceTag structures as local evaluate inputs; those tags
    // never pass through slSetTag/slSetTagForFrame. Talos uses this route. Keep the steady-state
    // evaluate path to one atomic branch, then inspect only the short activation/standby window.
    const void* frameToken = &streamline_hook_frame;
    const bool wantsUiBootstrapRecord = ce::dx12_streamline_ui_overlay::OnFrameTag(frameToken);
    if (wantsUiBootstrapRecord) {
        slViewportHandle viewport;
        uint32_t localTagCount = 0;
        constexpr uint32_t kMaximumInputChainDepth = 16;
        if (inputs) {
            for (uint32_t i = 0; i < numInputs; ++i) {
                const slBaseStructure* input = inputs[i];
                for (uint32_t depth = 0; input && depth < kMaximumInputChainDepth; ++depth) {
                    if (StructTypesEqual(input->structType, streamline_hook_kViewportHandleStructType)) {
                        viewport.value = static_cast<const slViewportHandle*>(input)->value;
                    }
                    if (StructTypesEqual(input->structType, streamline_hook_kResourceTagStructType)) {
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
                                        localTagCount, streamline_hook_commandBuffer, feature, numInputs);
        uint32_t tagIndex = 0;
        bool recorded = false;
        if (inputs) {
            for (uint32_t i = 0; i < numInputs && !recorded; ++i) {
                const slBaseStructure* input = inputs[i];
                for (uint32_t depth = 0; input && depth < kMaximumInputChainDepth; ++depth) {
                    if (StructTypesEqual(input->structType, streamline_hook_kResourceTagStructType)) {
                        const auto& tag = *static_cast<const slResourceTag*>(input);
                        if (opportunity != 0 && tagIndex < 12) {
                            HookLogImportant(
                                "Streamline Hook: UI tag opportunity #%u localTag[%u] input=%u depth=%u type=%u "
                                "lifecycle=%d resource=%p extent=(%u,%u %ux%u)",
                                opportunity, tagIndex, i, depth, tag.type, tag.lifecycle, tag.resource, tag.extent.left,
                                tag.extent.top, tag.extent.width, tag.extent.height);
                        }
                        ++tagIndex;
                        recorded = TryRecordOfficialUiResourceTag(frameToken, tag, streamline_hook_commandBuffer);
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
    return originalEvaluateFeature(feature, streamline_hook_frame, inputs, numInputs, streamline_hook_commandBuffer);
}

// Hook for Streamline Reflex sleep. This lets CE observe game-owned Reflex
// pacing without patching NvAPI_D3D_Sleep inside nvapi64.dll.
inline slResult Hooked_slReflexSleep(const void* streamline_hook_frame) {
    auto originalReflexSleep = GetCallableOriginalReflexSleep();
    if (!originalReflexSleep) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    // DLSSG-health evidence only: relaxed atomics + GetTickCount64 (shared-page read). No locks, no
    // logging, no syscalls — the manual Reflex FPS limiter's latency path through this hook is unchanged.
    streamline_hook_g_ReflexSleepObservedCount.fetch_add(1, std::memory_order_relaxed);
    streamline_hook_g_ReflexSleepLastTickMs.store(GetTickCount64(), std::memory_order_relaxed);

    g_ReflexLimiter.ApplyHybridPacingBeforeNativeSleep();

    const slResult result = originalReflexSleep(streamline_hook_frame);
    if (result == streamline_hook_kSlResultOk) {
        g_ReflexLimiter.MarkGameSleep("Streamline");
        g_ReflexLimiter.MarkNativePacingSignal();
    } else {
        static std::atomic<int> s_reflexSleepFailLogCount{0};
        const int failCount = s_reflexSleepFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (failCount < 5) {
            HookLogImportant("Streamline Hook: slReflexSleep forward failed result=%d frame=%p", result, streamline_hook_frame);
        }
    }
    return result;
}

// Hook for current Streamline Reflex options — detects low-latency and FPS limiter signals.
inline slResult Hooked_slReflexSetOptions(const slReflexOptions& streamline_hook_options) {
    auto originalReflexSetOptions = GetCallableOriginalReflexSetOptions();
    if (!originalReflexSetOptions) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    slReflexOptions adjustedOptions = streamline_hook_options;
    streamline_hook_g_ReflexSetOptionsObservedCount.fetch_add(1, std::memory_order_relaxed);
    streamline_hook_g_ReflexSetOptionsLastTickMs.store(GetTickCount64(), std::memory_order_relaxed);
    streamline_hook_g_ReflexLastForwardedMode.store(streamline_hook_options.mode, std::memory_order_relaxed);
    const uint32_t targetIntervalUs = g_ReflexLimiter.GetTargetIntervalUs();
    const auto frameLimitForwarding = ce::streamline_runtime_policy::ResolveStreamlineReflexFrameLimitForwarding(
        streamline_hook_options.frameLimitUs, targetIntervalUs);
    adjustedOptions.frameLimitUs = frameLimitForwarding.frameLimitUs;
    HandleStreamlineReflexPacingSignal("slReflexSetOptions", streamline_hook_options.mode, streamline_hook_options.frameLimitUs,
                                       adjustedOptions.frameLimitUs, targetIntervalUs);

    const slResult result = originalReflexSetOptions(adjustedOptions);
    if (result == streamline_hook_kSlResultOk && frameLimitForwarding.overrideApplied) {
        HookLogImportant(
            "Streamline Hook: Overrode Reflex options frameLimitUs %u->%u (mode=%d incomingActive=%d)",
            streamline_hook_options.frameLimitUs, adjustedOptions.frameLimitUs, adjustedOptions.mode,
            ce::streamline_runtime_policy::IsStreamlineReflexPacingSignalActive(streamline_hook_options.mode, streamline_hook_options.frameLimitUs)
                ? 1
                : 0);
    } else if (result != streamline_hook_kSlResultOk) {
        static std::atomic<int> s_reflexSetOptionsFailLogCount{0};
        const int failCount = s_reflexSetOptionsFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (failCount < 5) {
            HookLogImportant("Streamline Hook: slReflexSetOptions forward failed result=%d mode=%d frameLimitUs=%u",
                             result, streamline_hook_options.mode, streamline_hook_options.frameLimitUs);
        }
    }
    return result;
}

// Hook for legacy slReflexSetConstants — detects when game activates Reflex via Streamline.
inline slResult Hooked_slReflexSetConstants(const SLReflexConstants& streamline_hook_consts) {
    auto originalReflexSetConstants = GetCallableOriginalReflexSetConstants();
    if (!originalReflexSetConstants) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    SLReflexConstants adjustedConsts = streamline_hook_consts;
    const uint32_t targetIntervalUs = g_ReflexLimiter.GetTargetIntervalUs();

    // The legacy constants path only receives CE's frame-limit override when Reflex
    // is actually active, preserving the existing native Streamline behavior.
    if (streamline_hook_consts.mode >= streamline_hook_kSLReflexModeEnabled) {
        const auto frameLimitForwarding = ce::streamline_runtime_policy::ResolveStreamlineReflexFrameLimitForwarding(
            streamline_hook_consts.frameLimitUs, targetIntervalUs);
        adjustedConsts.frameLimitUs = frameLimitForwarding.frameLimitUs;
    }
    HandleStreamlineReflexPacingSignal("slReflexSetConstants", streamline_hook_consts.mode, streamline_hook_consts.frameLimitUs,
                                       adjustedConsts.frameLimitUs, targetIntervalUs);

    // Forward to the real slReflexSetConstants
    const slResult result = originalReflexSetConstants(adjustedConsts);
    if (result == streamline_hook_kSlResultOk && adjustedConsts.frameLimitUs != streamline_hook_consts.frameLimitUs) {
        HookLogImportant("Streamline Hook: Overrode Reflex constants frameLimitUs %u->%u (mode=%d)",
                         streamline_hook_consts.frameLimitUs, adjustedConsts.frameLimitUs, adjustedConsts.mode);
    } else if (result != streamline_hook_kSlResultOk) {
        static std::atomic<int> s_reflexSetConstantsFailLogCount{0};
        const int failCount = s_reflexSetConstantsFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (failCount < 5) {
            HookLogImportant("Streamline Hook: slReflexSetConstants forward failed result=%d mode=%d frameLimitUs=%u",
                             result, streamline_hook_consts.mode, streamline_hook_consts.frameLimitUs);
        }
    }
    return result;
}
