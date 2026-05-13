#include "streamline_hook.h"
#include <tlhelp32.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
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

namespace {

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

thread_local int g_ExternalOverlayPresentGuardDepth = 0;

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
constexpr slStructType kReflexOptionsStructType = {
    0xf03af81a, 0x6d0b, 0x4902, {0xa6, 0x51, 0xc4, 0x96, 0x5e, 0x21, 0x54, 0x34}};

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

std::atomic<bool> g_DynamicHooksRegistered{false};
std::atomic<bool> g_NoModulesLogged{false};
std::atomic<bool> g_ModuleSnapshotFailureLogged{false};
std::atomic<bool> g_ModuleSnapshotRetrySuccessLogged{false};
std::atomic<uint32_t> g_IATPatchesMask{0};
std::atomic<uint32_t> g_InstalledModuleMask{0};

std::atomic<void*> g_SLGetFeatureFunctionTarget{nullptr};
std::atomic<void*> g_SLGetPluginFunctionTarget{nullptr};
std::atomic<void*> g_SLSetD3DDeviceTarget{nullptr};
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
std::atomic<bool> g_DLSSGSetOptionsLookupLogged{false};
std::atomic<bool> g_DLSSGGetStateLookupLogged{false};
std::atomic<bool> g_ReflexSleepLookupLogged{false};
std::atomic<bool> g_ReflexSetOptionsLookupLogged{false};
std::atomic<bool> g_ReflexSetConstantsLookupLogged{false};

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
PFN_slGetPluginFunction g_Original_slGetPluginFunction = nullptr;
PFN_slSetD3DDevice g_Original_slSetD3DDevice = nullptr;
PFN_slDLSSGSetOptions g_Original_slDLSSGSetOptions = nullptr;
PFN_slDLSSGGetState g_Original_slDLSSGGetState = nullptr;
PFN_slReflexSleep g_Original_slReflexSleep = nullptr;
PFN_slReflexSetOptions g_Original_slReflexSetOptions = nullptr;
PFN_slReflexSetConstants g_Original_slReflexSetConstants = nullptr;

slResult Hooked_slGetFeatureFunction(uint32_t feature, const char* functionName, void*& function);
void* Hooked_slGetPluginFunction(const char* functionName);
slResult Hooked_slSetD3DDevice(void* d3dDevice);
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
    const bool ownerLoaded = DoesAddressBelongToLoadedModule(const_cast<void*>(addressToValidate), nullptr, nullptr, 0,
                                                            &ownerError);
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
        copy.bReserved16 = source.bReserved16;
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
        g_ReflexLimiter.SetGameActivated(true);
        g_ReflexLimiter.MarkNativePacingSignal();
    } else {
        if (g_ReflexLimiter.IsGameActivated()) {
            HookLogImportant("Streamline Hook: Game DEACTIVATED Reflex pacing via %s (mode=%d frameLimitUs=%u)",
                             sourceName ? sourceName : "unknown", mode, incomingFrameLimitUs);
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
    const bool sourceWasSetOptions = source && strcmp(source, "SetOptions") == 0;
    const bool sourceWasGetState = source && strcmp(source, "GetState") == 0;
    const bool postSLConfirmedButRuntimeStateStabilizingBase = HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing();
    const bool postSLConfirmedButGetStateOffWarmupProtected =
        !active && sourceWasGetState && HookIsPostSLOverlayConfirmedButGetStateOffWarmupProtected();
    const bool postSLConfirmedButRuntimeStateStabilizing =
        postSLConfirmedButRuntimeStateStabilizingBase || postSLConfirmedButGetStateOffWarmupProtected;
    const bool explicitSetOptionsActivationForCurrentComeback =
        g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
    const bool hadFSRFGPhase = HookHasFSRFGHistory();
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    const bool deferOffSignal =
        !active &&
        ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
            startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
            safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling, postSLConfirmedButRuntimeStateStabilizing);
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
    const bool explicitSetOptionsActivation = explicitSetOptionsEnableSignal;
    const bool updatedExplicitSetOptionsActivation =
        ce::streamline_runtime_policy::ResolveCurrentComebackExplicitSetOptionsActivation(
            previousExplicitSetOptionsActivation, signalUpdate.effectiveActive, signalUpdate.freshActivationEdge,
            explicitSetOptionsActivation);
    g_CurrentComebackActivatedViaExplicitSetOptions.store(updatedExplicitSetOptionsActivation,
                                                          std::memory_order_release);
    g_FGCompat.SetStreamlineFGSignal(signalUpdate.effectiveActive);
    ApplyCombinedDLSSFGState(signalUpdate.effectiveActive, signalUpdate.effectiveMultiplier);

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
            postSLConfirmedButGetStateOffWarmupProtected && !postSLConfirmedButRuntimeStateStabilizingBase) {
            static std::atomic<bool> s_loggedGetStateWarmupProofSuppression{false};
            if (!s_loggedGetStateWarmupProofSuppression.exchange(true, std::memory_order_relaxed)) {
                HookLogImportant(
                    "Streamline Hook: Suppressing post-stabilization GetState OFF during PostSL warmup proof "
                    "(hadFSR=%d explicit=%d safeBootstrap=%d stableProtectionWindow=%d-%d)",
                    hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
                    safePostFSRBootstrapPath ? 1 : 0,
                    ce::dx12_overlay_policy::GetConfirmedPostSLRuntimeStateStabilizationFirstFrame(),
                    HookGetPostSLGetStateOffWarmupProtectionLastFrame());
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
                "unconfirmed=%d confirmed=%d settling=%d stabilizing=%d source=%s)",
                hadFSRFGPhase ? 1 : 0, explicitSetOptionsActivationForCurrentComeback ? 1 : 0,
                safePostFSRBootstrapPath ? 1 : 0, startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0,
                postSLConfirmedRendering ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
                postSLConfirmedButRuntimeStateStabilizing ? 1 : 0, source ? source : "runtime-state");
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

void LogReturnedWrapperFallbackOnce(std::atomic<bool>& loggedFlag, const char* hookName, void* target, void* wrapper) {
    if (loggedFlag.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    HookLogImportant(
        "Streamline Hook: Using returned-pointer wrapper fallback for %s (target=%p wrapper=%p). "
        "Inline export patching/direct import fallback is not active for this target yet; callers that obtain the "
        "function via slGetFeatureFunction will still be intercepted.",
        hookName ? hookName : "<unknown>", target, wrapper);
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

    if (fallbackToReturnedWrapper && !g_DLSSGSetOptionsHooked.load(std::memory_order_acquire)) {
        if (!g_Original_slDLSSGSetOptions) {
            g_Original_slDLSSGSetOptions = reinterpret_cast<PFN_slDLSSGSetOptions>(function);
        }
        LogReturnedWrapperFallbackOnce(g_DLSSGSetOptionsReturnedWrapperFallbackLogged, "slDLSSGSetOptions", function,
                                       reinterpret_cast<void*>(Hooked_slDLSSGSetOptions));
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
            if (!g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
                TryInstallFeatureImportFallbackForOwningModule(
                    function, "slDLSSGGetState", reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                    reinterpret_cast<void**>(&g_Original_slDLSSGGetState), g_DLSSGGetStateImportFallbackAttemptedTarget,
                    "slDLSSGGetState");
            }
        }
    }

    if (fallbackToReturnedWrapper && !g_DLSSGGetStateHooked.load(std::memory_order_acquire)) {
        if (!g_Original_slDLSSGGetState) {
            g_Original_slDLSSGGetState = reinterpret_cast<PFN_slDLSSGGetState>(function);
        }
        LogReturnedWrapperFallbackOnce(g_DLSSGGetStateReturnedWrapperFallbackLogged, "slDLSSGGetState", function,
                                       reinterpret_cast<void*>(Hooked_slDLSSGGetState));
        function = reinterpret_cast<void*>(Hooked_slDLSSGGetState);
        return true;
    }

    return g_DLSSGGetStateHooked.load(std::memory_order_acquire);
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
                                       reinterpret_cast<void*>(Hooked_slReflexSleep));
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
                                       reinterpret_cast<void*>(Hooked_slReflexSetOptions));
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
                                       function, reinterpret_cast<void*>(Hooked_slReflexSetConstants));
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
            hookedAnything |= MaybeHookReflexSleep(sleepFunction, false);
        }
    }

    if (!g_ReflexSetOptionsHooked.load(std::memory_order_acquire)) {
        queriedSetOptions = true;
        setOptionsResult = originalGetFeatureFunction(kSLFeatureReflex, "slReflexSetOptions", setOptionsFunction);
        if (setOptionsResult == kSlResultOk && setOptionsFunction) {
            hookedAnything |= MaybeHookReflexSetOptions(setOptionsFunction, false);
        }
    }

    if (!g_ReflexSetConstantsHooked.load(std::memory_order_acquire)) {
        queriedSetConstants = true;
        setConstantsResult =
            originalGetFeatureFunction(kSLFeatureReflex, "slReflexSetConstants", setConstantsFunction);
        if (setConstantsResult == kSlResultOk && setConstantsFunction) {
            hookedAnything |= MaybeHookReflexSetConstants(setConstantsFunction, false);
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
    const auto originalDLSSGSetOptions =
        reinterpret_cast<PFN_slDLSSGSetOptions>(GetProcAddress(module, "slDLSSGSetOptions"));
    const auto originalDLSSGGetState = reinterpret_cast<PFN_slDLSSGGetState>(GetProcAddress(module, "slDLSSGGetState"));
    const auto originalReflexSleep = reinterpret_cast<PFN_slReflexSleep>(GetProcAddress(module, "slReflexSleep"));
    const auto originalReflexSetOptions =
        reinterpret_cast<PFN_slReflexSetOptions>(GetProcAddress(module, "slReflexSetOptions"));
    const auto originalReflexSetConstants =
        reinterpret_cast<PFN_slReflexSetConstants>(GetProcAddress(module, "slReflexSetConstants"));

    if (!originalGetFeatureFunction && !originalGetPluginFunction && !originalSetD3DDevice &&
        !originalDLSSGSetOptions && !originalDLSSGGetState && !originalReflexSleep && !originalReflexSetOptions &&
        !originalReflexSetConstants) {
        return false;
    }

    if (moduleBit != 0 && (g_InstalledModuleMask.load(std::memory_order_acquire) & moduleBit) != 0) {
        return false;
    }

    if (!shouldHookCoreExports &&
        (originalGetFeatureFunction || originalGetPluginFunction || originalSetD3DDevice)) {
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
            g_IATPatchesMask.fetch_or(moduleBit, std::memory_order_acq_rel);
        }

        if (originalDLSSGSetOptions &&
            ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slDLSSGSetOptions",
                                                                                   moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slDLSSGSetOptions", reinterpret_cast<void*>(Hooked_slDLSSGSetOptions),
                reinterpret_cast<void*>(originalDLSSGSetOptions),
                reinterpret_cast<void**>(&g_Original_slDLSSGSetOptions), "slDLSSGSetOptions");
        }

        if (originalDLSSGGetState &&
            ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slDLSSGGetState",
                                                                                   moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slDLSSGGetState", reinterpret_cast<void*>(Hooked_slDLSSGGetState),
                reinterpret_cast<void*>(originalDLSSGGetState), reinterpret_cast<void**>(&g_Original_slDLSSGGetState),
                "slDLSSGGetState");
        }

        if (originalReflexSleep &&
            ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slReflexSleep",
                                                                                   moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSleep", reinterpret_cast<void*>(Hooked_slReflexSleep),
                reinterpret_cast<void*>(originalReflexSleep), reinterpret_cast<void**>(&g_Original_slReflexSleep),
                "slReflexSleep");
        }

        if (originalReflexSetOptions &&
            ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slReflexSetOptions",
                                                                                   moduleBaseName)) {
            hookedAnything |= InstallFeatureImportFallbackIfPresent(
                moduleBaseName, "slReflexSetOptions", reinterpret_cast<void*>(Hooked_slReflexSetOptions),
                reinterpret_cast<void*>(originalReflexSetOptions),
                reinterpret_cast<void**>(&g_Original_slReflexSetOptions), "slReflexSetOptions");
        }

        if (originalReflexSetConstants &&
            ce::streamline_runtime_policy::ShouldHookStreamlineFeatureExportOnLoad("slReflexSetConstants",
                                                                                   moduleBaseName)) {
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

slResult Hooked_slDLSSGGetState(const slViewportHandle& viewport, slDLSSGState& state, const slDLSSGOptions* options) {
    auto originalGetState = GetCallableOriginalDLSSGGetState();
    if (!originalGetState) {
        return kSlResultErrorInvalidState;
    }

    const slResult result = originalGetState(viewport, state, options);
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
            HookLogImportant(
                "Streamline Hook: slDLSSGGetState observed viewport=%u optionsMode=%s(%u) generated=%u "
                "capabilityMax=%u presented=%u fence=%p fenceValue=%llu viewportWasActive=%d update=%d "
                "updateActive=%d clearAll=%d suppressNew=%d fenceEvidence=%d setOptionsHooked=%d "
                "setOptionsOriginal=%p",
                viewportKey, GetDLSSGModeName(options->mode), options->mode, options->numFramesToGenerate,
                capabilityMax, state.numFramesActuallyPresented, state.inputsProcessingCompletionFence,
                (unsigned long long)state.lastPresentInputsProcessingCompletionFenceValue, viewportWasActive ? 1 : 0,
                runtimeEvaluation.update.shouldUpdate ? 1 : 0, runtimeEvaluation.update.active ? 1 : 0,
                clearAllViewportStatesForDisable ? 1 : 0, suppressNewActivation ? 1 : 0,
                hasRuntimeFenceEvidence ? 1 : 0, g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
                reinterpret_cast<void*>(g_Original_slDLSSGSetOptions));
        }
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
        const bool postSLConfirmedButRuntimeStateStabilizing = HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing();
        const bool explicitSetOptionsActivationForCurrentComeback =
            g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
        const bool hadFSRFGPhase = HookHasFSRFGHistory();
        const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
        const bool shouldKeepDeferred =
            ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
                startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                postSLConfirmedRendering, postSLConfirmedButStartupSettling, postSLConfirmedButRuntimeStateStabilizing);
        if (g_SuppressedSetOptionsOffDuringStartup && !shouldKeepDeferred) {
            if (ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedStreamlineComeback(
                    hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                    postSLConfirmedButStartupSettling, postSLConfirmedButRuntimeStateStabilizing)) {
                LogDroppedSuppressedOffForStartupProtectedStreamlineComeback(
                    g_SuppressedOffViewportKey, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                    safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                    postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                    postSLConfirmedButRuntimeStateStabilizing);
            } else if (auto originalSetOptions = GetCallableOriginalDLSSGSetOptions()) {
                HookLogImportant(
                    "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) via GetState — startup window "
                    "expired (viewport=%u settling=%d stabilizing=%d)",
                    g_SuppressedOffViewportKey, postSLConfirmedButStartupSettling ? 1 : 0,
                    postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
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
    if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) &&
        HookIsPostSLOverlayConfirmedRendering()) {
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
        const bool postSLConfirmedButRuntimeStateStabilizing = HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing();
        const bool explicitSetOptionsActivationForCurrentComeback =
            g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
        const bool hadFSRFGPhase = HookHasFSRFGHistory();
        const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
        const bool shouldKeepDeferred =
            ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
                startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                postSLConfirmedRendering, postSLConfirmedButStartupSettling, postSLConfirmedButRuntimeStateStabilizing);
        if (g_SuppressedSetOptionsOffDuringStartup && !shouldKeepDeferred) {
            if (ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedStreamlineComeback(
                    hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
                    DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
                    postSLConfirmedButStartupSettling, postSLConfirmedButRuntimeStateStabilizing)) {
                LogDroppedSuppressedOffForStartupProtectedStreamlineComeback(
                    g_SuppressedOffViewportKey, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                    safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed,
                    postSLConfirmedRendering, postSLConfirmedButStartupSettling,
                    postSLConfirmedButRuntimeStateStabilizing);
            } else if (auto suppressedOriginalSetOptions = GetCallableOriginalDLSSGSetOptions()) {
                HookLogImportant(
                    "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) — startup window expired "
                    "(viewport=%u settling=%d stabilizing=%d)",
                    g_SuppressedOffViewportKey, postSLConfirmedButStartupSettling ? 1 : 0,
                    postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
                const slResult offResult =
                    suppressedOriginalSetOptions(g_SuppressedOffViewport, g_SuppressedOffOptions);
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
    const bool postSLConfirmedButRuntimeStateStabilizing = HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing();
    const bool explicitSetOptionsActivationForCurrentComeback =
        g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
    const bool hadFSRFGPhase = HookHasFSRFGHistory();
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    const bool suppressOffCall =
        !pureObserverOnly && requestedDisabled &&
        ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
            startupWindowActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
            safePostFSRBootstrapPath, startupActivationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
            postSLConfirmedButStartupSettling, postSLConfirmedButRuntimeStateStabilizing);

    const bool setOptionsCallSuppressed = suppressOffCall;
    slResult result;
    if (suppressOffCall) {
        HookLogImportant(
            "Streamline Hook: Suppressing slDLSSGSetOptions(OFF) while DLSS comeback remains startup-protected "
            "(viewport=%u mode=%u startupWindow=%d hadFSR=%d explicitComeback=%d safeBootstrap=%d pending=%d "
            "unconfirmed=%d confirmed=%d settling=%d stabilizing=%d) — preventing Streamline FG de-initialization "
            "before recovery proves stable",
            viewportKey, options.mode, startupWindowActive ? 1 : 0, HookHasFSRFGHistory() ? 1 : 0,
            explicitSetOptionsActivationForCurrentComeback ? 1 : 0, safePostFSRBootstrapPath ? 1 : 0,
            startupActivationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
            postSLConfirmedButStartupSettling ? 1 : 0, postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
        {
            std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);
            g_SuppressedSetOptionsOffDuringStartup = true;
            g_SuppressedOffViewport = viewport;
            g_SuppressedOffOptions = adjustedOptions;
            g_SuppressedOffViewportKey = viewportKey;
        }
        result = kSlResultOk;
    } else {
        result = originalSetOptions(viewport, adjustedOptions);
    }

    LogDLSSGSetOptionsTransition(viewportKey, options, adjustedOptions, originalGeneratedFrames, capabilityMax,
                                 requestedEnabled, setOptionsCallSuppressed, overrideApplied, overrideClamped, result,
                                 pureObserverOnly, startupWindowActive, hadFSRFGPhase,
                                 explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
                                 startupActivationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
                                 postSLConfirmedButStartupSettling, postSLConfirmedButRuntimeStateStabilizing);

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
        } else if (!pureObserverOnly && requestedDisabled &&
                   ce::streamline_runtime_policy::ShouldApplyViewportRuntimeUpdateFromSetOptions(
                       result == kSlResultOk, setOptionsCallSuppressed)) {
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
                    "unconfirmed=%d confirmed=%d settling=%d stabilizing=%d)",
                    viewportKey, startupWindowActive ? 1 : 0, startupActivationPending ? 1 : 0,
                    postSLActiveButUnconfirmed ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                    postSLConfirmedButStartupSettling ? 1 : 0, postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
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
        const int logCount =
            s_externalOverlaySuppressedPluginLookupLogCount.fetch_add(1, std::memory_order_relaxed);
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

    const slResult result = originalSetD3DDevice(d3dDevice);
    if (result == kSlResultOk) {
        TryResolveDLSSGFeatureHooks();
        TryResolveReflexFeatureHooks();
    }
    return result;
}

// Hook for Streamline Reflex sleep. This lets CE observe game-owned Reflex
// pacing without patching NvAPI_D3D_Sleep inside nvapi64.dll.
slResult Hooked_slReflexSleep(const void* frame) {
    auto originalReflexSleep = GetCallableOriginalReflexSleep();
    if (!originalReflexSleep) {
        return kSlResultErrorInvalidState;
    }

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

void OnModuleLoaded(HMODULE module, const char* moduleNameOrPath) {
    if (!module || !ce::streamline_runtime_policy::ShouldInspectStreamlineModuleOnLoad(moduleNameOrPath)) {
        return;
    }

    g_NoModulesLogged.store(false, std::memory_order_release);
    const bool inspectedModule = InstallHooksForModule(module, moduleNameOrPath);
    const bool resolvedDLSSG = TryResolveDLSSGFeatureHooks();
    const bool resolvedReflex = TryResolveReflexFeatureHooks();

    if (inspectedModule || resolvedDLSSG || resolvedReflex) {
        HookLogImportant(
            "Streamline Hook: Fresh module load inspected %s (%p) "
            "slGetFeatureFunctionHooked=%d slGetPluginFunctionHooked=%d slSetD3DDeviceHooked=%d "
            "dlssgSetOptionsHooked=%d "
            "dlssgGetStateHooked=%d reflexSleepHooked=%d reflexSetOptionsHooked=%d reflexSetConstantsHooked=%d",
            GetModuleBaseName(moduleNameOrPath), module,
            g_SLGetFeatureFunctionHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_SLGetPluginFunctionHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_SLSetD3DDeviceHooked.load(std::memory_order_acquire) ? 1 : 0,
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
    const bool postSLConfirmedButRuntimeStateStabilizing = HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing();
    const bool explicitSetOptionsActivationForCurrentComeback =
        g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
    const bool hadFSRFGPhase = HookHasFSRFGHistory();
    const bool safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    const bool shouldKeepDeferred =
        ce::streamline_runtime_policy::ShouldKeepOffChurnDeferredForStartupProtectedStreamlineComeback(
            windowStillActive, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
            activationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering, postSLConfirmedButStartupSettling,
            postSLConfirmedButRuntimeStateStabilizing);
    if (shouldKeepDeferred) {
        if (ce::dx12_overlay_policy::ShouldServicePostSLStartupActivationWhileOffChurnDeferred(
                shouldKeepDeferred, windowStillActive, activationPending, postSLActiveButUnconfirmed,
                callbackInstalled)) {
            const bool serviced = TryServicePostSLStartupActivation(
                "StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded deferred OFF churn", true);
            static std::atomic<int> s_deferredOffServiceLogCount{0};
            const int logCount = s_deferredOffServiceLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10 || (logCount % 100) == 0) {
                HookLogImportant(
                    "Streamline Hook: Startup-protected OFF churn serviced PostSL startup activation before "
                    "remaining deferred (serviced=%d pending=%d unconfirmed=%d)",
                    serviced ? 1 : 0, activationPending ? 1 : 0, postSLActiveButUnconfirmed ? 1 : 0);
            }
        }
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
        if (ce::streamline_runtime_policy::ShouldDropSuppressedOffChurnForStartupProtectedStreamlineComeback(
                hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback, safePostFSRBootstrapPath,
                DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire), postSLConfirmedButStartupSettling,
                postSLConfirmedButRuntimeStateStabilizing)) {
            LogDroppedSuppressedOffForStartupProtectedStreamlineComeback(
                g_SuppressedOffViewportKey, hadFSRFGPhase, explicitSetOptionsActivationForCurrentComeback,
                safePostFSRBootstrapPath, activationPending, postSLActiveButUnconfirmed, postSLConfirmedRendering,
                postSLConfirmedButStartupSettling, postSLConfirmedButRuntimeStateStabilizing);
            g_SuppressedSetOptionsOffDuringStartup = false;
        } else {
            auto originalSetOptions = GetCallableOriginalDLSSGSetOptions();
            if (!originalSetOptions) {
                return;
            }
            HookLogImportant(
                "Streamline Hook: Forwarding suppressed slDLSSGSetOptions(OFF) via periodic flush — startup window "
                "expired (viewport=%u, activationPending=%d settling=%d stabilizing=%d)",
                g_SuppressedOffViewportKey, activationPending ? 1 : 0, postSLConfirmedButStartupSettling ? 1 : 0,
                postSLConfirmedButRuntimeStateStabilizing ? 1 : 0);
            const slResult offResult = originalSetOptions(g_SuppressedOffViewport, g_SuppressedOffOptions);
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
            const bool serviced = TryServicePostSLStartupActivation(
                "StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded after OFF flush", true);
            HookLogImportant("Streamline Hook: PostSL startup activation service after OFF flush returned %d",
                             serviced ? 1 : 0);
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
        const bool serviced = TryServicePostSLStartupActivation(
            "StreamlineHook::FlushSuppressedSetOptionsOffIfNeeded expiry", true);
        HookLogImportant("Streamline Hook: PostSL startup activation service after startup expiry returned %d",
                         serviced ? 1 : 0);
    } else if (activationPending && callbackInstalled && postSLActiveButUnconfirmed) {
        logSkippedDirectCallbackAfterActivation();
    }
}

}  // namespace StreamlineHook
