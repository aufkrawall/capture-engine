#pragma once

struct SLReflexConstants;

struct slStructType;

struct slBaseStructure;

struct slViewportHandle;

struct slExtent;

struct slResource;

struct slResourceTag;

struct slDLSSGOptions;

struct slDLSSGState;

struct slReflexOptions;

struct ViewportFGState;

struct DLSSGSetOptionsLogState;

struct ReflexSignalLogState;

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

#include "../../common/log_meter.h"

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

#include "streamline_hook_pcl.h"

using slResult = int;

enum class slResourceType : char {
    kTexture2D = 0,
};

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

inline std::atomic<uint32_t> streamline_hook_g_StartupProtectedOffChurnActiveProofCount{0};bool IsObserverOnlyModeActive();void LogDroppedSuppressedOffForStartupProtectedStreamlineComeback(
    uint32_t viewportKey, bool hadFSRFGPhase, bool explicitSetOptionsActivationForCurrentComeback,
    bool safePostFSRBootstrapPath, bool startupActivationPending, bool postSLActiveButUnconfirmed,
    bool postSLConfirmedRendering, bool postSLConfirmedButStartupSettling,
    bool postSLConfirmedButRuntimeStateStabilizing);bool IsObserverPolicyOnlyModeActive();bool ShouldKeepPureObserverOnlyStreamlineBehavior();bool TryServicePostSLStartupActivation(const char* source, bool clearStartupWindow);void ResetStartupProtectedOffChurnActiveProof(const char* reason);void LogAcceptedOffDuringActivatedUnconfirmedResume(const char* source, bool startupWindowActive, bool hadFSRFGPhase,
                                                    bool explicitSetOptionsActivationForCurrentComeback,
                                                    bool safePostFSRBootstrapPath, bool startupActivationPending,
                                                    bool postSLActiveButUnconfirmed,
                                                    bool postSLStartupActivationEntered, bool postSLConfirmedRendering,
                                                    bool postSLConfirmedButStartupSettling,
                                                    bool postSLConfirmedButRuntimeStateStabilizing);void MarkStartupProtectedOffChurnObserved(const char* source, bool postSLConfirmedRendering,
                                          bool postSLConfirmedButStartupSettling,
                                          bool postSLConfirmedButRuntimeStateStabilizing);void MarkStartupProtectedActiveRuntimeProof(const char* source, int multiplier);bool IsStartupProtectedOffChurnAwaitingActiveProof(bool startupProtectedComebackProof, bool postSLConfirmedRendering,
                                                   bool postSLConfirmedButStartupSettling);

inline thread_local int streamline_hook_g_ExternalOverlayPresentGuardDepth = 0;

inline constexpr slResult streamline_hook_kSlResultOk = 0;

inline constexpr uint32_t streamline_hook_kFeatureDLSS = 0;
inline constexpr uint32_t streamline_hook_kFeatureDLSSRR = 1001;

inline constexpr slResult streamline_hook_kSlResultErrorInvalidState = 38;

inline constexpr uint32_t streamline_hook_kSLFeatureDLSSG = 1000;

inline constexpr uint32_t streamline_hook_kSLFeatureReflex = 3;  // Streamline Reflex feature ID
inline constexpr uint32_t streamline_hook_kSLFeaturePCL = 4;

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

// Streamline sl::ReflexConstants structure (matches Streamline SDK)
struct SLReflexConstants {
    size_t structSize = sizeof(SLReflexConstants);
    uint32_t version = static_cast<uint32_t>(streamline_hook_kSLStructVersion1);
    int32_t mode = streamline_hook_kSLReflexModeOff;
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

struct slViewportHandle : slBaseStructure {
    slViewportHandle() : slBaseStructure(streamline_hook_kViewportHandleStructType, streamline_hook_kSLStructVersion1) {}

    uint32_t value = 0xFFFFFFFFu;
};

struct slExtent {
    uint32_t top = 0;
    uint32_t left = 0;
    uint32_t width = 0;
    uint32_t height = 0;
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

inline constexpr uint32_t streamline_hook_kSLBufferTypeUIColorAndAlpha = 23;

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

struct slReflexOptions : slBaseStructure {
    slReflexOptions() : slBaseStructure(streamline_hook_kReflexOptionsStructType, streamline_hook_kSLStructVersion1) {}

    int32_t mode = streamline_hook_kSLReflexOptionsModeOff;
    uint32_t frameLimitUs = 0;
    bool useMarkersToOptimize = false;
    uint16_t virtualKey = 0;
    uint32_t idThread = 0;
};

struct ViewportFGState {
    bool active = false;
    int multiplier = 0;
    uint32_t generatedFrames = 0;
    uint32_t capabilityMax = 0;
};

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

inline std::atomic<void*> streamline_hook_g_VulkanCreateSwapchainTarget{nullptr};

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

inline std::atomic<bool> streamline_hook_g_VulkanCreateSwapchainHooked{false};

inline void* streamline_hook_g_Original_vkCreateSwapchainKHR = nullptr;

inline std::atomic<uint32_t> streamline_hook_g_LastUpscalerEvaluation{0xFFFFFFFFu};

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

// Bounded failure latch: slReflexSetConstants is genuinely absent from some
// sl.reflex builds (slGetFeatureFunction never returns it). After a few failed
// queries the runtime retry loop stops re-scanning for it (session
// 20260811_231851: endless 2.5s rescans with setConstantsHooked=0).
inline constexpr int kReflexSetConstantsUnavailableQueryLimit = 3;

inline std::atomic<int> streamline_hook_g_ReflexSetConstantsUnavailableQueries{0};

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

inline constexpr uint32_t streamline_hook_kDLSSGStatusFailGetCurrentBackBufferIndex = 1u << 4;void FormatDLSSGStatusFlags(uint32_t status, char* buffer, size_t bufferSize);

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

// Bumped by every tracked sl.* module unload (OnModuleUnloaded). The proactive feature-hook
// resolution calls into sl.interposer's slGetFeatureFunction, whose internal dispatch can point
// into a feature plugin (sl.dlss_g / sl.reflex) that the runtime already unmapped — the
// DLSS->FSR switch test crashed exactly there (20260812_042259: DEP at a freed sl.dlss_g
// address from sl_interposer!slGetFeatureFunction+0x162). Resolution snapshots this generation
// before pinning the modules and rejects the query when a teardown started in between.
inline std::atomic<uint64_t> streamline_hook_g_StreamlineModuleUnloadGeneration{0};

// Set by every tracked sl.* module unload (OnModuleUnloaded) and cleared by the next sl.* module
// load (OnModuleLoaded). While set, the Streamline runtime is being torn down or its plugin table
// is stale, so feature-function resolution must not call sl.interposer's slGetFeatureFunction: its
// internal dispatch can walk into a plugin that was already unmapped (session 20260813_160845:
// DEP at a freed sl.dlss_d address from sl_interposer!slGetFeatureFunction+0x162 while only
// sl.reflex + sl.interposer were pinned; that plugin had no hook slots, so its unload logged
// nothing but still bumped the unload generation — the generation snapshot cannot detect a
// teardown that already completed). Event-driven: no sleeps, timers, or polling.
inline std::atomic<bool> streamline_hook_g_StreamlineTeardownInFlight{false};

inline PFN_slDLSSGSetOptions streamline_hook_g_Original_slDLSSGSetOptions = nullptr;

inline PFN_slDLSSGGetState streamline_hook_g_Original_slDLSSGGetState = nullptr;

inline PFN_slReflexSleep streamline_hook_g_Original_slReflexSleep = nullptr;

inline PFN_slReflexSetOptions streamline_hook_g_Original_slReflexSetOptions = nullptr;

inline PFN_slReflexSetConstants streamline_hook_g_Original_slReflexSetConstants = nullptr;

slResult Hooked_slGetFeatureFunction(uint32_t feature, const char* streamline_hook_functionName, void*& streamline_hook_function);

void* Hooked_slGetPluginFunction(const char* streamline_hook_functionName);

slResult Hooked_slSetD3DDevice(void* streamline_hook_d3dDevice);

slResult Hooked_slSetTag(const slViewportHandle& viewport, const slResourceTag* tags, uint32_t numTags,
                         void* streamline_hook_commandBuffer);

slResult Hooked_slSetTagForFrame(const slBaseStructure& streamline_hook_frame, const slViewportHandle& viewport,
                                 const slResourceTag* tags, uint32_t numTags, void* streamline_hook_commandBuffer);

slResult Hooked_slEvaluateFeature(uint32_t feature, const slBaseStructure& streamline_hook_frame, const slBaseStructure** inputs,
                                  uint32_t numInputs, void* streamline_hook_commandBuffer);

slResult Hooked_slDLSSGSetOptions(const slViewportHandle& viewport, const slDLSSGOptions& streamline_hook_options);

slResult Hooked_slDLSSGGetState(const slViewportHandle& viewport, slDLSSGState& state, const slDLSSGOptions* streamline_hook_options);

slResult Hooked_slReflexSleep(const void* streamline_hook_frame);

slResult Hooked_slReflexSetOptions(const slReflexOptions& streamline_hook_options);

slResult Hooked_slReflexSetConstants(const SLReflexConstants& streamline_hook_consts);const char* GetDLSSGModeName(uint32_t mode);const char* GetModuleBaseName(const char* moduleNameOrPath);bool IsStreamlineModuleName(const char* moduleNameOrPath);bool ShouldHookStreamlineCoreExports(const char* moduleNameOrPath);bool IsStreamlineCoreDynamicHookModule(const char* moduleBaseName, HMODULE);bool IsStreamlineDLSSGDynamicHookModule(const char* moduleBaseName, HMODULE);bool IsStreamlineReflexDynamicHookModule(const char* moduleBaseName, HMODULE);uint32_t GetModuleMaskBit(const char* moduleNameOrPath);void LogSkippedStreamlineCoreExportsOnce(const char* moduleBaseName, HMODULE module, bool hasGetFeature,
                                         bool hasGetPlugin, bool hasSetD3DDevice);size_t GetModuleImageSizeBytes(HMODULE module);bool DoesAddressBelongToLoadedModule(void* address, HMODULE* ownerModule, char* ownerPath, DWORD ownerPathCapacity,
                                     DWORD* outError);void LogStaleStreamlineOriginalBlockedOnce(const char* streamline_hook_functionName, void* original, void* validationAddress,
                                           const char* expectedModuleRole, DWORD error);bool IsSavedStreamlineOriginalCallable(const char* streamline_hook_functionName, void* original, void* validationAddress,
                                       const char* expectedModuleRole);PFN_slGetFeatureFunction GetCallableOriginalGetFeatureFunction();PFN_slGetPluginFunction GetCallableOriginalGetPluginFunction();PFN_slSetD3DDevice GetCallableOriginalSetD3DDevice();PFN_slSetTag GetCallableOriginalSetTag();PFN_slSetTagForFrame GetCallableOriginalSetTagForFrame();PFN_slEvaluateFeature GetCallableOriginalEvaluateFeature();PFN_slDLSSGSetOptions GetCallableOriginalDLSSGSetOptions();PFN_slDLSSGGetState GetCallableOriginalDLSSGGetState();PFN_slReflexSleep GetCallableOriginalReflexSleep();PFN_slReflexSetOptions GetCallableOriginalReflexSetOptions();PFN_slReflexSetConstants GetCallableOriginalReflexSetConstants();uint32_t GetViewportKey(const slViewportHandle& viewport);slDLSSGOptions CloneDLSSGOptions(const slDLSSGOptions& source);int GetEffectiveMultiplier(const slDLSSGOptions& streamline_hook_options);

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
};void LogDLSSGSetOptionsTransition(uint32_t viewportKey, const slDLSSGOptions& requestedOptions,
                                  const slDLSSGOptions& forwardedOptions, uint32_t requestedGeneratedFrames,
                                  uint32_t capabilityMax, bool requestedEnabled, bool setOptionsCallSuppressed,
                                  bool overrideApplied, bool overrideClamped, slResult result, bool pureObserverOnly,
                                  bool startupWindowActive, bool hadFSRFGPhase,
                                  bool explicitSetOptionsActivationForCurrentComeback, bool safePostFSRBootstrapPath,
                                  bool startupActivationPending, bool postSLActiveButUnconfirmed,
                                  bool postSLConfirmedRendering, bool postSLConfirmedButStartupSettling,
                                  bool postSLConfirmedButRuntimeStateStabilizing);

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
};void LogStreamlineReflexSignalChange(const char* sourceName, int32_t mode, uint32_t incomingFrameLimitUs,
                                     uint32_t forwardedFrameLimitUs, uint32_t targetIntervalUs);void MaybePrepareForStreamlineEnableTransitionFromReflex(const char* sourceName);void HandleStreamlineReflexPacingSignal(const char* sourceName, int32_t mode, uint32_t incomingFrameLimitUs,
                                        uint32_t forwardedFrameLimitUs, uint32_t targetIntervalUs);uint32_t GetCachedCapabilityMax(uint32_t viewportKey);void CacheCapabilityMax(uint32_t viewportKey, uint32_t capabilityMax);void ApplyCombinedDLSSFGState(bool active, int multiplier);void ApplyCombinedStreamlineRuntimeState(bool active, int multiplier, bool explicitSetOptionsEnableSignal,
                                         const char* source);bool WasViewportRuntimeStateActive(uint32_t viewportKey);bool ShouldSuppressNewGetStateActivation();bool HasDLSSGRuntimeFenceEvidence(const slDLSSGState& state);void UpdateViewportRuntimeState(uint32_t viewportKey, bool active, int multiplier, uint32_t generatedFrames,
                                uint32_t capabilityMax, const char* source,
                                bool clearAllViewportStatesForDisable = false);

template <typename T>
struct StreamlineInlineHookPublication {
    T* destination = nullptr;
    T fallback = nullptr;
};

template <typename T>
void PublishStreamlineInlineHookTrampoline(void* trampoline, void* context) {
    auto* publication = static_cast<StreamlineInlineHookPublication<T>*>(context);
    *publication->destination =
        trampoline ? reinterpret_cast<T>(trampoline) : publication->fallback;
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
    const bool slotInstalled = installedFlag.load(std::memory_order_acquire);
    if (slotInstalled && installedTarget == target) {
        return false;
    }

    // A single process-global `original` cannot serve two live targets. Refuse the
    // newcomer while the installed target is still mapped; the live instance keeps
    // working and CE simply does not observe the duplicate.
    if (!ce::streamline_runtime_policy::ShouldRetargetStreamlineHookSlot(
            slotInstalled, installedTarget, target,
            installedTarget != nullptr &&
                DoesAddressBelongToLoadedModule(const_cast<void*>(installedTarget), nullptr, nullptr, 0, nullptr))) {
        static std::atomic<uint32_t> s_refusedRetargetCount{0};
        const uint32_t refusedCount = s_refusedRetargetCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ce::log_meter::ShouldLogCadence(refusedCount, 10, 300)) {
            HookLogImportant(
                "Streamline Hook: Refusing to retarget %s from %p to %p — the installed target is still mapped, so a "
                "second live instance would take over CE's single forward pointer (count=%u)",
                hookName, installedTarget, target, refusedCount);
        }
        return false;
    }

    void* retainedTrampoline = nullptr;
    if (InlineHook::TryGetInstalledTrampoline(target, detour, &retainedTrampoline)) {
        original = reinterpret_cast<T>(retainedTrampoline);
        targetSlot.store(target, std::memory_order_release);
        installedFlag.store(true, std::memory_order_release);
        HookLogImportant(
            "Streamline Hook: Reconciled rediscovered %s at %p with CE's retained live hook (trampoline=%p)",
            hookName, target, retainedTrampoline);
        return true;
    }

    StreamlineInlineHookPublication<T> publication{&original, original};
    void* trampoline = nullptr;
    if (!InlineHook::InstallPublished(target, detour, &trampoline, PublishStreamlineInlineHookTrampoline<T>,
                                      &publication)) {
        static std::atomic<uint32_t> s_installFailureCount{0};
        const uint32_t failureCount = s_installFailureCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ce::log_meter::ShouldLogCadence(failureCount, 10, 300)) {
            HookLogImportant("Streamline Hook: Failed to inline hook %s at %p (attempt=%u)", hookName, target,
                             failureCount);
        }
        return false;
    }

    targetSlot.store(target, std::memory_order_release);
    installedFlag.store(true, std::memory_order_release);
    HookLogImportant("Streamline Hook: Inline hook installed for %s at %p (trampoline=%p)", hookName, target,
                     trampoline);
    return true;
}void LogFeatureImportFallbackUnavailableOnce(const char* moduleBaseName, const char* streamline_hook_functionName, void* exportedProc,
                                             const char* hookName, const char* reason);bool InstallFeatureImportFallbackIfPresent(const char* moduleBaseName, const char* streamline_hook_functionName, void* detour,
                                           void* exportedProc, void** originalSlot, const char* hookName);bool TryGetOwningModulePath(void* address, char* modulePath, DWORD modulePathCapacity, DWORD* outError);bool TryInstallFeatureImportFallbackForOwningModule(void* streamline_hook_function, const char* streamline_hook_functionName, void* detour,
                                                    void** originalSlot, std::atomic<void*>& attemptedTarget,
                                                    const char* hookName);void LogReturnedWrapperFallbackOnce(std::atomic<bool>& loggedFlag, const char* hookName, void* target, void* wrapper,
                                    bool hookReady);void LogProactiveFeatureHookGapOnce(std::atomic<bool>& loggedFlag, const char* hookName, void* target);void LogFeatureLookupOutcomeOnce(std::atomic<bool>& loggedFlag, const char* hookName, void* originalTarget,
                                  void* returnedTarget, bool hookReady);bool MaybeHookDLSSGSetOptions(void*& streamline_hook_function, bool fallbackToReturnedWrapper);bool MaybeHookDLSSGGetState(void*& streamline_hook_function, bool fallbackToReturnedWrapper);bool MaybeHookReflexSleep(void*& streamline_hook_function, bool fallbackToReturnedWrapper);bool MaybeHookReflexSetOptions(void*& streamline_hook_function, bool fallbackToReturnedWrapper);bool MaybeHookReflexSetConstants(void*& streamline_hook_function, bool fallbackToReturnedWrapper);bool TryResolveDLSSGFeatureHooks(bool proactiveScan = false);bool TryResolveReflexFeatureHooks(bool proactiveScan = false);uint32_t QueryCapabilityMax(const slViewportHandle& viewport, const slDLSSGOptions* streamline_hook_options);void RegisterDynamicHooksOnce();bool InstallHooksForModule(HMODULE module, const char* moduleNameOrPath);bool OpenLoadedModuleSnapshotWithRetry(HANDLE& snapshot, MODULEENTRY32& firstEntry, DWORD& error, int& attempts,
                                       bool& failedOnFirstEntry);bool ScanLoadedStreamlineModules(bool pinFeatureResolution = false);bool AreReflexFeatureHooksComplete();void RetryResolveReflexFeatureHooksForRuntimeActivity(const char* source);slResult Hooked_slDLSSGGetState(const slViewportHandle& viewport, slDLSSGState& state, const slDLSSGOptions* streamline_hook_options);slResult Hooked_slDLSSGSetOptions(const slViewportHandle& viewport, const slDLSSGOptions& streamline_hook_options);

// Safe no-op stub for SL function pointers that SL returned as NULL during
// re-entrant calls.  Steam's OverlayHookD3D3 may call slGetFeatureFunction
// from within SL's execution context (during DllMain or FG processing).
// If SL returns NULL, Steam calls through the NULL pointer → RIP=0 crash.
// Instead of returning an error (which Steam may ignore while still using the
// NULL pointer), substitute a safe stub that returns success and does nothing.
// This allows Steam to continue overlay rendering without crashing.
slResult SlNullFunctionStub();void* Hooked_slGetPluginFunction(const char* streamline_hook_functionName);slResult Hooked_slGetFeatureFunction(uint32_t feature, const char* streamline_hook_functionName, void*& streamline_hook_function);slResult Hooked_slSetD3DDevice(void* streamline_hook_d3dDevice);bool StructTypesEqual(const slStructType& lhs, const slStructType& rhs);bool TryRecordOfficialUiResourceTag(const void* frameToken, const slResourceTag& tag, void* streamline_hook_commandBuffer);uint32_t LogOfficialUiTagOpportunity(const char* tagApi, const void* frameToken, uint32_t viewportKey,
                                     const slResourceTag* tags, uint32_t numTags, void* streamline_hook_commandBuffer,
                                     uint32_t feature = UINT_MAX, uint32_t numInputs = 0);void TryRecordOfficialUiTag(const char* tagApi, const void* frameToken, const slViewportHandle& viewport,
                            const slResourceTag* tags, uint32_t numTags, void* streamline_hook_commandBuffer);slResult Hooked_slSetTag(const slViewportHandle& viewport, const slResourceTag* tags, uint32_t numTags,
                         void* streamline_hook_commandBuffer);slResult Hooked_slSetTagForFrame(const slBaseStructure& streamline_hook_frame, const slViewportHandle& viewport,
                                 const slResourceTag* tags, uint32_t numTags, void* streamline_hook_commandBuffer);slResult Hooked_slEvaluateFeature(uint32_t feature, const slBaseStructure& streamline_hook_frame, const slBaseStructure** inputs,
                                  uint32_t numInputs, void* streamline_hook_commandBuffer);

// Hook for Streamline Reflex sleep. This lets CE observe game-owned Reflex
// pacing without patching NvAPI_D3D_Sleep inside nvapi64.dll.
slResult Hooked_slReflexSleep(const void* streamline_hook_frame);

// Hook for current Streamline Reflex options — detects low-latency and FPS limiter signals.
slResult Hooked_slReflexSetOptions(const slReflexOptions& streamline_hook_options);

// Hook for legacy slReflexSetConstants — detects when game activates Reflex via Streamline.
slResult Hooked_slReflexSetConstants(const SLReflexConstants& streamline_hook_consts);
