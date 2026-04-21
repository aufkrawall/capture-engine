#include "streamline_hook.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
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

namespace {

bool IsObserverOnlyModeActive() {
    return HookOverlayObserverOnlyEnabled();
}

void LogDroppedSuppressedOffForStartupProtectedPostFSRComeback(uint32_t viewportKey,
                                                               bool explicitSetOptionsActivationForCurrentComeback,
                                                               bool safePostFSRBootstrapPath,
                                                               bool startupActivationPending,
                                                               bool postSLActiveButUnconfirmed,
                                                               bool postSLConfirmedRendering,
                                                               bool postSLConfirmedButStartupSettling) {
    static std::atomic<int> s_dropLogCount{0};
    const int logCount = s_dropLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 100) == 0) {
        HookLogImportant(
            "Streamline Hook: Dropping stale suppressed slDLSSGSetOptions(OFF) after startup window expiry because "
            "post-FSR DLSS comeback is already stably active (viewport=%u explicit=%d safeBootstrap=%d "
            "pending=%d unconfirmed=%d confirmed=%d settling=%d)",
            viewportKey, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
            safePostFSRBootstrapPath ? 1 : 0, startupActivationPending ? 1 : 0,
            postSLActiveButUnconfirmed ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
            postSLConfirmedButStartupSettling ? 1 : 0);
    }
}

bool IsObserverPolicyOnlyModeActive() {
    return HookOverlayObserverPolicyOnlyEnabled();
}

bool ShouldKeepPureObserverOnlyStreamlineBehavior() {
    return ce::streamline_runtime_policy::ShouldKeepPureObserverOnlyStreamlineBehavior(
        IsObserverOnlyModeActive(), IsObserverPolicyOnlyModeActive());
}

using slResult = int;

constexpr slResult kSlResultOk = 0;
constexpr slResult kSlResultErrorInvalidState = 38;
constexpr uint32_t kSLFeatureDLSSG = 1000;
constexpr uint32_t kSLFeatureReflex = 0x00000009;  // Streamline Reflex feature ID
constexpr size_t kSLStructVersion1 = 1;
constexpr size_t kSLStructVersion2 = 2;
constexpr size_t kSLStructVersion3 = 3;
constexpr size_t kSLStructVersion4 = 4;
constexpr char kSLBooleanInvalid = 2;

// Streamline Reflex mode constants
constexpr int kSLReflexModeOff = 0;
constexpr int kSLReflexModeEnabled = 1;
constexpr int kSLReflexModeLowLatency = 2;
constexpr int kSLReflexModeLowLatencyWithBoost = 3;

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

struct slViewportHandle : slBaseStructure {
    slViewportHandle() : slBaseStructure(kViewportHandleStructType, kSLStructVersion1) {}

    uint32_t value = 0xFFFFFFFFu;
};

struct slDLSSGOptions : slBaseStructure {
    slDLSSGOptions() : slBaseStructure(kDLSSGOptionsStructType, kSLStructVersion4) {}

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
    char bReserved16 = kSLBooleanInvalid;
};

struct slDLSSGState : slBaseStructure {
    slDLSSGState() : slBaseStructure(kDLSSGStateStructType, kSLStructVersion3) {}

    uint64_t estimatedVRAMUsageInBytes = 0;
    uint32_t status = 0;
    uint32_t minWidthOrHeight = 0;
    uint32_t numFramesActuallyPresented = 0;
    uint32_t numFramesToGenerateMax = 0;
    char bReserved4 = kSLBooleanInvalid;
    char bIsVsyncSupportAvailable = kSLBooleanInvalid;
    void* inputsProcessingCompletionFence = nullptr;
    uint64_t lastPresentInputsProcessingCompletionFenceValue = 0;
};

using PFN_slGetFeatureFunction = slResult (*)(uint32_t feature, const char* functionName, void*& function);
using PFN_slSetD3DDevice = slResult (*)(void* d3dDevice);
using PFN_slDLSSGSetOptions = slResult (*)(const slViewportHandle& viewport, const slDLSSGOptions& options);
using PFN_slDLSSGGetState = slResult (*)(const slViewportHandle& viewport, slDLSSGState& state,
                                         const slDLSSGOptions* options);
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

std::atomic<bool> g_DynamicHooksRegistered{false};
std::atomic<bool> g_NoModulesLogged{false};
std::atomic<uint32_t> g_IATPatchesMask{0};
std::atomic<uint32_t> g_InstalledModuleMask{0};

std::atomic<void*> g_SLGetFeatureFunctionTarget{nullptr};
std::atomic<void*> g_SLSetD3DDeviceTarget{nullptr};
std::atomic<void*> g_DLSSGSetOptionsTarget{nullptr};
std::atomic<void*> g_DLSSGGetStateTarget{nullptr};
std::atomic<void*> g_ReflexSetConstantsTarget{nullptr};

std::atomic<bool> g_SLGetFeatureFunctionHooked{false};
std::atomic<bool> g_SLSetD3DDeviceHooked{false};
std::atomic<bool> g_DLSSGSetOptionsHooked{false};
std::atomic<bool> g_DLSSGGetStateHooked{false};
std::atomic<bool> g_ReflexSetConstantsHooked{false};

std::unordered_map<uint32_t, ViewportFGState> g_ViewportStates;
std::unordered_map<uint32_t, uint32_t> g_ViewportCapabilityMax;

std::atomic<ULONGLONG> g_SuppressNewGetStateActivationUntilMs{0};
constexpr ULONGLONG kAuthoritativeFFXTakeoverGetStateSuppressMs = 250;
std::atomic<bool> g_BlockGetStateOnlyReactivationUntilExplicitSetOptions{false};
std::atomic<bool> g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap{false};
std::atomic<bool> g_CurrentComebackActivatedViaExplicitSetOptions{false};
std::atomic<bool> g_StartupWindowOffExtensionPending{false};

std::mutex g_SuppressedOffMutex;
bool g_SuppressedSetOptionsOffDuringStartup = false;
slViewportHandle g_SuppressedOffViewport = {};
slDLSSGOptions g_SuppressedOffOptions = {};
uint32_t g_SuppressedOffViewportKey = 0;

PFN_slGetFeatureFunction g_Original_slGetFeatureFunction = nullptr;
PFN_slSetD3DDevice g_Original_slSetD3DDevice = nullptr;
PFN_slDLSSGSetOptions g_Original_slDLSSGSetOptions = nullptr;
PFN_slDLSSGGetState g_Original_slDLSSGGetState = nullptr;
PFN_slReflexSetConstants g_Original_slReflexSetConstants = nullptr;

slResult Hooked_slGetFeatureFunction(uint32_t feature, const char* functionName, void*& function);
slResult Hooked_slSetD3DDevice(void* d3dDevice);
slResult Hooked_slDLSSGSetOptions(const slViewportHandle& viewport, const slDLSSGOptions& options);
slResult Hooked_slDLSSGGetState(const slViewportHandle& viewport, slDLSSGState& state, const slDLSSGOptions* options);
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
    const char* baseName = GetModuleBaseName(moduleNameOrPath);
    return baseName && (!_stricmp(baseName, "sl.interposer.dll") || !_stricmp(baseName, "sl.common.dll"));
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
        copy.bReserved16 = source.bReserved16;
    }
    return copy;
}

int GetEffectiveMultiplier(const slDLSSGOptions& options) {
    return ce::streamline_runtime_policy::ResolveDLSSFGMultiplier(
        ce::streamline_runtime_policy::IsDLSSGModeEnabled(options.mode), options.numFramesToGenerate);
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

void ApplyCombinedStreamlineRuntimeState(bool active, int multiplier, const char* source) {
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
    const bool explicitSetOptionsActivationForCurrentComeback =
        g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    const bool deferOffSignal =
        !active && ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
            startupWindowActive, HookHasFSRFGHistory(), explicitSetOptionsActivationForCurrentComeback,
            safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
            postSLConfirmedRendering,
            postSLConfirmedButStartupSettling);
    const bool previousSignal = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
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
    const bool explicitSetOptionsActivation = source && strcmp(source, "SetOptions") == 0;
    g_CurrentComebackActivatedViaExplicitSetOptions.store(
        ce::streamline_runtime_policy::ResolveCurrentComebackExplicitSetOptionsActivation(
            previousExplicitSetOptionsActivation, signalUpdate.effectiveActive, signalUpdate.freshActivationEdge,
            explicitSetOptionsActivation),
        std::memory_order_release);
    g_FGCompat.SetStreamlineFGSignal(signalUpdate.effectiveActive);
    ApplyCombinedDLSSFGState(signalUpdate.effectiveActive, signalUpdate.effectiveMultiplier);

    if (previousSignalObserved != signalUpdate.effectiveActive) {
        DX12_OnStreamlineFGStateChanged(signalUpdate.effectiveActive);
        HookLogImportant("Streamline Hook: FG state transition %s->%s via %s", previousSignalObserved ? "ON" : "OFF",
                         signalUpdate.effectiveActive ? "ON" : "OFF", source ? source : "runtime-state");
    }
    ce::fg_session::EmitFGEvent(
        explicitSetOptionsActivation ? ce::fg_session::FGEventKind::kStreamlineSetOptionsRuntimeUpdate
                                     : ce::fg_session::FGEventKind::kStreamlineGetStateRuntimeUpdate,
        source ? source : "StreamlineRuntimeState", nullptr, nullptr,
        signalUpdate.effectiveActive ? ce::fg_runtime::RuntimeMode::kDLSSFG
                                     : ce::fg_runtime::RuntimeMode::kStreamlineNoFG,
        signalUpdate.effectiveActive, g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire));
    if (signalUpdate.deferredOffDuringStartupWindow && !startupWindowActive) {
        static std::atomic<int> s_halfArmedDeferredOffLogCount{0};
        const int logCount = s_halfArmedDeferredOffLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Keeping OFF churn deferred after startup window expiry because post-FSR DLSS "
                "comeback is still startup-protected (explicit=%d safeBootstrap=%d pending=%d unconfirmed=%d "
                "confirmed=%d settling=%d source=%s)",
                explicitSetOptionsActivationForCurrentComeback ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0,
                startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
                source ? source : "runtime-state");
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
                                uint32_t capabilityMax, const char* source) {
    ViewportFGState previousState{};
    bool hadPreviousState = false;
    bool stateChanged = false;
    bool anyActive = false;
    int combinedMultiplier = 0;

    {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        const auto existing = g_ViewportStates.find(viewportKey);
        if (existing != g_ViewportStates.end()) {
            previousState = existing->second;
            hadPreviousState = true;
        }

        if (active) {
            g_ViewportStates[viewportKey] = {true, multiplier, generatedFrames, capabilityMax};
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

    ApplyCombinedStreamlineRuntimeState(anyActive, combinedMultiplier, source);

    if (stateChanged) {
        HookLog("Streamline Hook: Viewport %u state active=%d multiplier=%dx generatedFrames=%u capabilityMax=%u",
                viewportKey, active ? 1 : 0, active ? multiplier : 0, generatedFrames, capabilityMax);
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
        }
    }

    if (fallbackToReturnedWrapper && !g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
        if (!g_Original_slDLSSGSetOptions) {
            g_Original_slDLSSGSetOptions = reinterpret_cast<PFN_slDLSSGSetOptions>(function);
        }
        function = reinterpret_cast<void*>(Hooked_slDLSSGSetOptions);
        return true;
    }

    return g_DLSSGSetOptionsHooked.load(std::memory_order_acquire);
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
        }
    }

    if (fallbackToReturnedWrapper && !g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
        if (!g_Original_slDLSSGGetState) {
            g_Original_slDLSSGGetState = reinterpret_cast<PFN_slDLSSGGetState>(function);
        }
        function = reinterpret_cast<void*>(Hooked_slDLSSGGetState);
        return true;
    }

    return g_DLSSGGetStateHooked.load(std::memory_order_acquire);
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
        }
    }

    if (fallbackToReturnedWrapper && !g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
        if (!g_Original_slReflexSetConstants) {
            g_Original_slReflexSetConstants = reinterpret_cast<PFN_slReflexSetConstants>(function);
        }
        function = reinterpret_cast<void*>(Hooked_slReflexSetConstants);
        return true;
    }

    return g_ReflexSetConstantsHooked.load(std::memory_order_acquire);
}

bool TryResolveDLSSGFeatureHooks() {
    if (!g_Original_slGetFeatureFunction) {
        return false;
    }

    bool hookedAnything = false;

    if (!g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
        void* function = nullptr;
        const slResult result = g_Original_slGetFeatureFunction(kSLFeatureDLSSG, "slDLSSGSetOptions", function);
        if (result == kSlResultOk && function) {
            hookedAnything |= MaybeHookDLSSGSetOptions(function, false);
        }
    }

    if (!g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
        void* function = nullptr;
        const slResult result = g_Original_slGetFeatureFunction(kSLFeatureDLSSG, "slDLSSGGetState", function);
        if (result == kSlResultOk && function) {
            hookedAnything |= MaybeHookDLSSGGetState(function, false);
        }
    }

    return hookedAnything || g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ||
           g_DLSSGGetStateHooked.load(std::memory_order_acquire);
}

uint32_t QueryCapabilityMax(const slViewportHandle& viewport, const slDLSSGOptions* options) {
    if (!g_Original_slDLSSGGetState && !TryResolveDLSSGFeatureHooks()) {
        return 0;
    }
    if (!g_Original_slDLSSGGetState) {
        return 0;
    }

    slDLSSGState state;
    const slResult result = g_Original_slDLSSGGetState(viewport, state, options);
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

    IATHook::RegisterDynamicHook("slGetFeatureFunction", reinterpret_cast<void*>(Hooked_slGetFeatureFunction),
                                 reinterpret_cast<void**>(&g_Original_slGetFeatureFunction));
    IATHook::RegisterDynamicHook("slSetD3DDevice", reinterpret_cast<void*>(Hooked_slSetD3DDevice),
                                 reinterpret_cast<void**>(&g_Original_slSetD3DDevice));
    HookLogImportant("Streamline Hook: Registered dynamic hooks for slGetFeatureFunction and slSetD3DDevice");
}

bool InstallHooksForModule(HMODULE module, const char* moduleNameOrPath) {
    if (!module || !IsStreamlineModuleName(moduleNameOrPath)) {
        return false;
    }

    g_FGCompat.SetStreamlineSupportPresent(true);

    RegisterDynamicHooksOnce();

    const char* moduleBaseName = GetModuleBaseName(moduleNameOrPath);
    const uint32_t moduleBit = GetModuleMaskBit(moduleBaseName);
    const auto originalGetFeatureFunction =
        reinterpret_cast<PFN_slGetFeatureFunction>(GetProcAddress(module, "slGetFeatureFunction"));
    const auto originalSetD3DDevice = reinterpret_cast<PFN_slSetD3DDevice>(GetProcAddress(module, "slSetD3DDevice"));

    if (!originalGetFeatureFunction && !originalSetD3DDevice) {
        return false;
    }

    if (moduleBit != 0 && (g_InstalledModuleMask.load(std::memory_order_acquire) & moduleBit) != 0) {
        return false;
    }

    bool hookedAnything = false;
    {
        std::lock_guard<std::mutex> lock(g_ModuleHookMutex);

        if (originalGetFeatureFunction) {
            if (!g_Original_slGetFeatureFunction) {
                g_Original_slGetFeatureFunction = originalGetFeatureFunction;
            }

            hookedAnything |= InstallInlineHookOnce(reinterpret_cast<void*>(originalGetFeatureFunction),
                                                    reinterpret_cast<void*>(Hooked_slGetFeatureFunction),
                                                    g_Original_slGetFeatureFunction, g_SLGetFeatureFunctionHooked,
                                                    g_SLGetFeatureFunctionTarget, "slGetFeatureFunction");
        }

        if (originalSetD3DDevice) {
            if (!g_Original_slSetD3DDevice) {
                g_Original_slSetD3DDevice = originalSetD3DDevice;
            }

            hookedAnything |= InstallInlineHookOnce(
                reinterpret_cast<void*>(originalSetD3DDevice), reinterpret_cast<void*>(Hooked_slSetD3DDevice),
                g_Original_slSetD3DDevice, g_SLSetD3DDeviceHooked, g_SLSetD3DDeviceTarget, "slSetD3DDevice");
        }

        if (moduleBit != 0 && (g_IATPatchesMask.load(std::memory_order_acquire) & moduleBit) == 0) {
            void* dummy = nullptr;
            if (originalGetFeatureFunction) {
                IATHook::PatchIATAllModules(moduleBaseName, "slGetFeatureFunction",
                                            reinterpret_cast<void*>(Hooked_slGetFeatureFunction), &dummy);
            }
            if (originalSetD3DDevice) {
                IATHook::PatchIATAllModules(moduleBaseName, "slSetD3DDevice",
                                            reinterpret_cast<void*>(Hooked_slSetD3DDevice), &dummy);
            }
            g_IATPatchesMask.fetch_or(moduleBit, std::memory_order_acq_rel);
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

slResult Hooked_slDLSSGGetState(const slViewportHandle& viewport, slDLSSGState& state, const slDLSSGOptions* options) {
    if (!g_Original_slDLSSGGetState) {
        return kSlResultErrorInvalidState;
    }

    const slResult result = g_Original_slDLSSGGetState(viewport, state, options);
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
    if (runtimeEvaluation.update.shouldUpdate) {
        UpdateViewportRuntimeState(viewportKey, runtimeEvaluation.update.active, runtimeEvaluation.update.multiplier,
                                   runtimeEvaluation.update.generatedFrames, runtimeEvaluation.update.capabilityMax,
                                   "GetState");
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
        const bool explicitSetOptionsActivationForCurrentComeback =
            g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
        const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
        const bool shouldKeepDeferred =
            ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
                startupWindowActive, HookHasFSRFGHistory(), explicitSetOptionsActivationForCurrentComeback,
                safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                postSLConfirmedRendering,
                postSLConfirmedButStartupSettling);
        if (g_SuppressedSetOptionsOffDuringStartup && !shouldKeepDeferred) {
            if (ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedPostFSRComeback(
                    HookHasFSRFGHistory(), explicitSetOptionsActivationForCurrentComeback,
                    safePostFSRBootstrapPath,
                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                    postSLConfirmedButStartupSettling)) {
                LogDroppedSuppressedOffForStartupProtectedPostFSRComeback(
                    g_SuppressedOffViewportKey, explicitSetOptionsActivationForCurrentComeback,
                    safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                    postSLConfirmedRendering, postSLConfirmedButStartupSettling);
            } else if (g_Original_slDLSSGSetOptions) {
                HookLogImportant(
                    "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) via GetState — startup window "
                    "expired (viewport=%u)",
                    g_SuppressedOffViewportKey);
                const slResult offResult =
                    g_Original_slDLSSGSetOptions(g_SuppressedOffViewport, g_SuppressedOffOptions);
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
    if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        DXGIShared::RepairVTableHooksIfNeeded();
    }

    return result;
}

slResult Hooked_slDLSSGSetOptions(const slViewportHandle& viewport, const slDLSSGOptions& options) {
    if (!g_Original_slDLSSGSetOptions) {
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
        const bool explicitSetOptionsActivationForCurrentComeback =
            g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
        const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
        const bool shouldKeepDeferred =
            ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
                startupWindowActive, HookHasFSRFGHistory(), explicitSetOptionsActivationForCurrentComeback,
                safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                postSLConfirmedRendering,
                postSLConfirmedButStartupSettling);
        if (g_SuppressedSetOptionsOffDuringStartup && !shouldKeepDeferred) {
            if (g_Original_slDLSSGSetOptions) {
                HookLogImportant(
                    "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) — startup window expired "
                    "(viewport=%u)",
                    g_SuppressedOffViewportKey);
                const slResult offResult =
                    g_Original_slDLSSGSetOptions(g_SuppressedOffViewport, g_SuppressedOffOptions);
                if (offResult != kSlResultOk) {
                    HookLogImportant("Streamline Hook: Forwarded slDLSSGSetOptions(OFF) returned %d", offResult);
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
    const bool explicitSetOptionsActivationForCurrentComeback =
        g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    const bool suppressOffCall =
        !pureObserverOnly && requestedDisabled &&
        ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
            startupWindowActive, HookHasFSRFGHistory(), explicitSetOptionsActivationForCurrentComeback,
            safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
            postSLConfirmedRendering,
            postSLConfirmedButStartupSettling);

    slResult result;
    if (suppressOffCall) {
        HookLogImportant(
            "Streamline Hook: Suppressing slDLSSGSetOptions(OFF) while DLSS comeback remains startup-protected "
            "(viewport=%u mode=%u startupWindow=%d hadFSR=%d explicitComeback=%d safeBootstrap=%d pending=%d "
            "unconfirmed=%d confirmed=%d settling=%d) — preventing Streamline FG de-initialization before recovery "
            "proves stable",
            viewportKey, options.mode, startupWindowActive ? 1 : 0, HookHasFSRFGHistory() ? 1 : 0,
            explicitSetOptionsActivationForCurrentComeback ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0,
            startupActivationPending ? 1 : 0,
            postSLActiveButUnconfirmed ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
            postSLConfirmedButStartupSettling ? 1 : 0);
        {
            std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);
            g_SuppressedSetOptionsOffDuringStartup = true;
            g_SuppressedOffViewport = viewport;
            g_SuppressedOffOptions = adjustedOptions;
            g_SuppressedOffViewportKey = viewportKey;
        }
        result = kSlResultOk;
    } else {
        result = g_Original_slDLSSGSetOptions(viewport, adjustedOptions);
    }

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
        } else if (!pureObserverOnly && requestedDisabled) {
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

        const auto runtimeUpdate = ce::streamline_runtime_policy::BuildViewportRuntimeUpdateFromRequestedOptions(
            true, true, adjustedOptions.mode, adjustedOptions.numFramesToGenerate, capabilityMax);
        UpdateViewportRuntimeState(viewportKey, runtimeUpdate.active, runtimeUpdate.multiplier,
                                   runtimeUpdate.generatedFrames, runtimeUpdate.capabilityMax, "SetOptions");

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

slResult Hooked_slGetFeatureFunction(uint32_t feature, const char* functionName, void*& function) {
    if (!g_Original_slGetFeatureFunction) {
        return kSlResultErrorInvalidState;
    }

    const slResult result = g_Original_slGetFeatureFunction(feature, functionName, function);
    if (result != kSlResultOk || !functionName || !function) {
        return result;
    }

    // DLSS Frame Generation feature hooks
    if (feature == kSLFeatureDLSSG) {
        if (strcmp(functionName, "slDLSSGSetOptions") == 0) {
            MaybeHookDLSSGSetOptions(function, true);
            if (HookDebugLoggingEnabled()) {
                HookLog("Streamline Hook: Intercepted slDLSSGSetOptions lookup (returned=%p)", function);
            }
        } else if (strcmp(functionName, "slDLSSGGetState") == 0) {
            MaybeHookDLSSGGetState(function, true);
            if (HookDebugLoggingEnabled()) {
                HookLog("Streamline Hook: Intercepted slDLSSGGetState lookup (returned=%p)", function);
            }
        }
    }
    // Reflex feature hook — detect game activation of native Reflex
    else if (feature == kSLFeatureReflex) {
        if (strcmp(functionName, "slReflexSetConstants") == 0) {
            if (MaybeHookReflexSetConstants(function, true)) {
                HookLogImportant("Streamline Hook: Intercepted slReflexSetConstants (returned=%p original=%p)",
                                 function, g_Original_slReflexSetConstants);
            }
        }
    }

    return result;
}

slResult Hooked_slSetD3DDevice(void* d3dDevice) {
    if (!g_Original_slSetD3DDevice) {
        return kSlResultErrorInvalidState;
    }

    const slResult result = g_Original_slSetD3DDevice(d3dDevice);
    if (result == kSlResultOk) {
        TryResolveDLSSGFeatureHooks();
    }
    return result;
}

// Hook for slReflexSetConstants — detects when game activates Reflex via Streamline.
slResult Hooked_slReflexSetConstants(const SLReflexConstants& consts) {
    if (!g_Original_slReflexSetConstants) {
        return kSlResultErrorInvalidState;
    }

    SLReflexConstants adjustedConsts = consts;

    // Detect game activation: mode is low-latency (enabled, low latency, or boost)
    if (consts.mode >= kSLReflexModeEnabled) {
        const bool activationEdge = !g_ReflexLimiter.IsGameActivated();
        if (activationEdge) {
            HookLogImportant(
                "Streamline Hook: Game ACTIVATED Reflex via slReflexSetConstants (mode=%d, frameLimitUs=%u)",
                consts.mode, consts.frameLimitUs);
            const auto runtimeMode = g_FGCompat.GetRuntimeMode();
            const bool runtimeModeIsFSRFG = runtimeMode == ce::fg_runtime::RuntimeMode::kFSRFG;
            const bool runtimeOwnsSwapchain = DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration();
            if (ce::streamline_runtime_policy::ShouldRequestStreamlineEnablePreparationOnReflexActivation(
                    true, g_FGCompat.IsFSRFGApiActive(), runtimeModeIsFSRFG, runtimeOwnsSwapchain)) {
                HookLogImportant(
                    "Streamline Hook: Reflex activation requesting Streamline enable preparation "
                    "(runtime=%s apiFSR=%d fgOwned=%d)",
                    ce::fg_runtime::GetRuntimeModeName(runtimeMode), g_FGCompat.IsFSRFGApiActive() ? 1 : 0,
                    runtimeOwnsSwapchain ? 1 : 0);
                DX12_PrepareForStreamlineEnableTransition();
            }
        }
        g_ReflexLimiter.SetGameActivated(true);

        const uint32_t targetIntervalUs = g_ReflexLimiter.GetTargetIntervalUs();
        if (targetIntervalUs > 0) {
            adjustedConsts.frameLimitUs = targetIntervalUs;
        }
        g_ReflexLimiter.MarkNativePacingSignal();
    } else if (consts.mode == kSLReflexModeOff) {
        if (g_ReflexLimiter.IsGameActivated()) {
            HookLogImportant("Streamline Hook: Game DEACTIVATED Reflex via slReflexSetConstants (mode=off)");
        }
        g_ReflexLimiter.SetGameActivated(false);
    }

    // Forward to the real slReflexSetConstants
    const slResult result = g_Original_slReflexSetConstants(adjustedConsts);
    if (result == kSlResultOk && adjustedConsts.frameLimitUs != consts.frameLimitUs) {
        HookLog("Streamline Hook: Overrode Reflex frameLimitUs %u->%u (mode=%d)", consts.frameLimitUs,
                adjustedConsts.frameLimitUs, adjustedConsts.mode);
    }
    return result;
}

}  // namespace

namespace StreamlineHook {

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

    bool foundModule = false;
    const struct {
        const wchar_t* wideName;
        const char* narrowName;
    } modules[] = {
        {L"sl.interposer.dll", "sl.interposer.dll"},
        {L"sl.common.dll", "sl.common.dll"},
    };

    for (const auto& module : modules) {
        if (HMODULE handle = GetModuleHandleW(module.wideName)) {
            foundModule = true;
            g_FGCompat.SetStreamlineSupportPresent(true);
            InstallHooksForModule(handle, module.narrowName);
        }
    }

    if (!foundModule) {
        if (!g_NoModulesLogged.exchange(true, std::memory_order_acq_rel)) {
            HookLog("Streamline Hook: No Streamline core modules loaded yet; waiting for module load");
        }
    } else {
        g_NoModulesLogged.store(false, std::memory_order_release);
    }
}

bool IsInitialized() {
    return g_DynamicHooksRegistered.load(std::memory_order_acquire);
}

bool IsDLSSFGRequestedViaStreamline() {
    return DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
}

void OnAuthoritativeFFXTakeover() {
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
    g_StartupWindowOffExtensionPending.store(false, std::memory_order_release);
    HookLogImportant(
        "Streamline Hook: Authoritative FFX takeover reset %zu viewport states and preserved %zu capability caches; "
        "suppressing GetState-only reactivation for %llums and until safe post-FSR bootstrap or explicit enable",
        resetViewportCount, preservedCapabilityCount, (unsigned long long)kAuthoritativeFFXTakeoverGetStateSuppressMs);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kAuthoritativeFFXTakeover,
                                "StreamlineHook::OnAuthoritativeFFXTakeover", nullptr, nullptr,
                                ce::fg_runtime::RuntimeMode::kFSRFG, true, true);
}

void OnAuthoritativeStreamlineStartupHandoff() {
    g_SuppressNewGetStateActivationUntilMs.store(GetTickCount64() + kAuthoritativeFFXTakeoverGetStateSuppressMs,
                                                 std::memory_order_release);
    g_StartupWindowOffExtensionPending.store(true, std::memory_order_release);
    HookLogImportant(
        "Streamline Hook: Authoritative Streamline startup handoff observed — suppressing fresh GetState-only "
        "reactivation for %llums until explicit enable or stable startup evidence arrives",
        (unsigned long long)kAuthoritativeFFXTakeoverGetStateSuppressMs);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kAuthoritativeStreamlineStartupHandoff,
                                "StreamlineHook::OnAuthoritativeStreamlineStartupHandoff", nullptr, nullptr,
                                ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false);
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_StateMutex);
    g_ViewportStates.clear();
    g_ViewportCapabilityMax.clear();
    g_SuppressNewGetStateActivationUntilMs.store(0, std::memory_order_release);
    g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.store(false, std::memory_order_release);
    g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap.store(false, std::memory_order_release);
    g_CurrentComebackActivatedViaExplicitSetOptions.store(false, std::memory_order_release);
    g_StartupWindowOffExtensionPending.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);
        g_SuppressedSetOptionsOffDuringStartup = false;
    }
    g_FGCompat.SetStreamlineFGSignal(false);
    g_FGCompat.SetDLSSFGMultiplier(0);
    g_FGCompat.SetDLSSFGActive(false);
    g_FGCompat.SetStreamlineSupportPresent(false);
    DXGIShared::g_StreamlineFGRunning.store(false, std::memory_order_release);
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
    const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
    const bool postSLConfirmedButStartupSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
    const bool explicitSetOptionsActivationForCurrentComeback =
        g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    const bool shouldKeepDeferred =
        ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedPostFSRComeback(
            windowStillActive, HookHasFSRFGHistory(), explicitSetOptionsActivationForCurrentComeback,
            safePostFSRBootstrapPath, activationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling);
    if (shouldKeepDeferred) {
        return;
    }

    const bool shouldTriggerDirectCallback =
        ce::streamline_runtime_policy::ShouldTriggerDirectPostSLCallbackAfterStartupWindowExpiry(
            activationPending, postSLActiveButUnconfirmed);

    auto logSkippedDirectCallbackAfterActivation = [&]() {
        static std::atomic<int> s_skipLogCount{0};
        const int logCount = s_skipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Startup window expired but PostSL activation already completed — skipping "
                "redundant direct callback until first confirmed render");
        }
    };

    // Case 1: Suppressed OFF exists — either forward it to Streamline for a real
    // inactive edge, or drop it if a newer post-FSR comeback is already
    // startup-protected and this OFF is now stale churn.
    if (g_SuppressedSetOptionsOffDuringStartup) {
        if (ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedPostFSRComeback(
                HookHasFSRFGHistory(), explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
                DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                postSLConfirmedButStartupSettling)) {
            LogDroppedSuppressedOffForStartupProtectedPostFSRComeback(
                g_SuppressedOffViewportKey, explicitSetOptionsActivationForCurrentComeback,
                safePostFSRBootstrapPath, activationPending, postSLActiveButUnconfirmed,
                postSLConfirmedRendering, postSLConfirmedButStartupSettling);
            g_SuppressedSetOptionsOffDuringStartup = false;
        } else {
            if (!g_Original_slDLSSGSetOptions) {
                return;
            }
            HookLogImportant(
                "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) via periodic flush — startup window "
                "expired (viewport=%u, activationPending=%d)",
                g_SuppressedOffViewportKey, activationPending ? 1 : 0);
            const slResult offResult = g_Original_slDLSSGSetOptions(g_SuppressedOffViewport, g_SuppressedOffOptions);
            if (offResult != kSlResultOk) {
                HookLogImportant("Streamline Hook: Forwarded slDLSSGSetOptions(OFF) via periodic flush returned %d",
                                 offResult);
            }
            g_SuppressedSetOptionsOffDuringStartup = false;
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
        // We check activationPending alone (not postSLActive).  The callback being
        // installed does not mean PostSL is rendering-active — if the startup window
        // is still active, the callback is deferred and g_PostSLOverlayActive stays
        // false.  activationPending is the ground truth for "PostSL activation has
        // not yet completed."
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

            // CRITICAL: Clear the startup transition window so that when the PostSL
            // callback is skipped (due to null swapchain), the next ProcessFrame call
            // won't see the window as still active and defer again.
            DXGIShared::ClearStreamlineStartupTransitionWindow();
            HookLogImportant("Streamline Hook: Cleared startup transition window after OFF flush trigger");

            auto postSLCallback = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
            if (postSLCallback) {
                // Note: PostSLOverlayRenderGated now handles nullptr swapchain by returning early.
                // The startup window has been cleared, so the next normal ProcessFrame call
                // will properly complete activation with a valid swapchain.
                postSLCallback(nullptr);
                HookLogImportant("Streamline Hook: PostSL callback (via nullptr) completed after OFF flush");
            }
        } else if (activationPending && callbackInstalled && postSLActiveButUnconfirmed) {
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

        // CRITICAL: Clear the startup transition window so that when the PostSL
        // callback is skipped (due to null swapchain), the next ProcessFrame call
        // won't see the window as still active and defer again.
        DXGIShared::ClearStreamlineStartupTransitionWindow();
        HookLogImportant("Streamline Hook: Cleared startup transition window before direct callback trigger");

        auto postSLCallback = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
        if (postSLCallback) {
            // Note: PostSLOverlayRenderGated now handles nullptr swapchain by returning early.
            // The startup window has been cleared, so the next normal ProcessFrame call
            // will properly complete activation with a valid swapchain.
            postSLCallback(nullptr);
            HookLogImportant("Streamline Hook: PostSL callback (via nullptr) completed");
        }
    } else if (activationPending && callbackInstalled && postSLActiveButUnconfirmed) {
        logSkippedDirectCallbackAfterActivation();
    }
}

}  // namespace StreamlineHook
