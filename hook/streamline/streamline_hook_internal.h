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

#include "common/logging/log_meter.h"
#include "hook/d3d12/dx12_overlay_policy.h"
#include "hook/present/dxgi_shared.h"
#include "hook/fg/fg_detection.h"
#include "hook/fg/fg_session_state.h"
#include "hook/runtime/freeze_watchdog.h"
#include "hook/runtime/hook_common.h"
#include "hook/ngx/ngx_drs_override.h"
#include "hook/pacing/reflex_limiter.h"
#include "streamline_runtime_policy.h"
#include "hook/hooking/iat_hook.h"
#include "hook/hooking/inline_hook.h"
#include "hook/d3d12/dx12_hook.h"
#include "hook/d3d12/dx12_streamline_ui_overlay.h"
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

inline std::atomic<uint32_t> streamline_hook_g_StartupProtectedOffChurnActiveProofCount{0};
// Title frames (PCL present-start markers) seen while FG ran since the last OFF churn; see
// GetStartupProtectedOffChurnActiveProof in streamline_runtime_policy.h.
inline std::atomic<uint32_t> streamline_hook_g_StartupProtectedOffChurnActiveFrameCount{0};
uint32_t GetStartupProtectedOffChurnActiveProof();
void NoteStartupProtectedActiveTitleFrame(uint64_t frameId);
bool IsObserverOnlyModeActive();
bool IsObserverPolicyOnlyModeActive();
bool ShouldKeepPureObserverOnlyStreamlineBehavior();
bool TryServicePostSLStartupActivation(const char* source, bool clearStartupWindow);
void ResetStartupProtectedOffChurnActiveProof(const char* reason);
void LogAcceptedOffDuringActivatedUnconfirmedResume(const char* source, bool startupWindowActive, bool hadFSRFGPhase,
                                                    bool explicitSetOptionsActivationForCurrentComeback,
                                                    bool safePostFSRBootstrapPath, bool startupActivationPending,
                                                    bool postSLActiveButUnconfirmed,
                                                    bool postSLStartupActivationEntered, bool postSLConfirmedRendering,
                                                    bool postSLConfirmedButStartupSettling,
                                                    bool postSLConfirmedButRuntimeStateStabilizing);
void MarkStartupProtectedOffChurnObserved(const char* source, bool postSLConfirmedRendering,
                                          bool postSLConfirmedButStartupSettling,
                                          bool postSLConfirmedButRuntimeStateStabilizing);
void MarkStartupProtectedActiveRuntimeProof(const char* source, int multiplier);
bool IsStartupProtectedOffChurnAwaitingActiveProof(bool startupProtectedComebackProof, bool postSLConfirmedRendering,
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

    bool operator==(const ViewportFGState& o) const {
        return active == o.active && multiplier == o.multiplier && generatedFrames == o.generatedFrames &&
               capabilityMax == o.capabilityMax;
    }
    bool operator!=(const ViewportFGState& o) const { return !(*this == o); }
};

struct StreamlineHookState {
    std::mutex initMutex;
    std::mutex stateMutex;
    std::mutex moduleHookMutex;
    std::mutex featureHookMutex;
    std::mutex acceptedD3D12DeviceMutex;
    ID3D12Device* acceptedD3D12Device = nullptr;
    std::atomic<bool> dynamicHooksRegistered{false};
    std::atomic<bool> streamlineUsesD3D12{false};
    std::atomic<bool> noModulesLogged{false};
    std::atomic<bool> moduleSnapshotFailureLogged{false};
    std::atomic<bool> moduleSnapshotRetrySuccessLogged{false};
    std::atomic<uint32_t> iatPatchesMask{0};
    std::atomic<uint32_t> installedModuleMask{0};

    std::atomic<void*> slGetFeatureFunctionTarget{nullptr};
    std::atomic<void*> slGetPluginFunctionTarget{nullptr};
    std::atomic<void*> slSetD3DDeviceTarget{nullptr};
    std::atomic<void*> slSetTagTarget{nullptr};
    std::atomic<void*> slSetTagForFrameTarget{nullptr};
    std::atomic<void*> slEvaluateFeatureTarget{nullptr};
    std::atomic<void*> vulkanCreateSwapchainTarget{nullptr};

    std::atomic<void*> dlssgSetOptionsTarget{nullptr};
    std::atomic<void*> dlssgGetStateTarget{nullptr};
    std::atomic<void*> reflexSleepTarget{nullptr};
    std::atomic<void*> reflexSetOptionsTarget{nullptr};
    std::atomic<void*> reflexSetConstantsTarget{nullptr};

    std::atomic<void*> dlssgSetOptionsImportFallbackAttemptedTarget{nullptr};
    std::atomic<void*> dlssgGetStateImportFallbackAttemptedTarget{nullptr};
    std::atomic<void*> reflexSleepImportFallbackAttemptedTarget{nullptr};
    std::atomic<void*> reflexSetOptionsImportFallbackAttemptedTarget{nullptr};
    std::atomic<void*> reflexSetConstantsImportFallbackAttemptedTarget{nullptr};

    std::atomic<void*> dlssgSetOptionsFailedTarget{nullptr};
    std::atomic<uint32_t> dlssgSetOptionsFailedAttempts{0};
    std::atomic<void*> dlssgGetStateFailedTarget{nullptr};
    std::atomic<uint32_t> dlssgGetStateFailedAttempts{0};
    std::atomic<void*> reflexSleepFailedTarget{nullptr};
    std::atomic<uint32_t> reflexSleepFailedAttempts{0};
    std::atomic<void*> reflexSetOptionsFailedTarget{nullptr};
    std::atomic<uint32_t> reflexSetOptionsFailedAttempts{0};
    std::atomic<void*> reflexSetConstantsFailedTarget{nullptr};
    std::atomic<uint32_t> reflexSetConstantsFailedAttempts{0};
    std::atomic<void*> pclSetMarkerFailedTarget{nullptr};
    std::atomic<uint32_t> pclSetMarkerFailedAttempts{0};

    std::atomic<int> reflexSleepUnavailableQueries{0};
    std::atomic<int> reflexSetOptionsUnavailableQueries{0};
    std::atomic<int> reflexSetConstantsUnavailableQueries{0};
    std::atomic<int> pclUnavailableQueries{0};
    std::atomic<uint32_t> runtimeReflexRetryAttempts{0};

    std::atomic<bool> slGetFeatureFunctionHooked{false};
    std::atomic<bool> slGetPluginFunctionHooked{false};
    std::atomic<bool> slSetD3DDeviceHooked{false};
    std::atomic<bool> slSetTagHooked{false};
    std::atomic<bool> slSetTagForFrameHooked{false};
    std::atomic<bool> slEvaluateFeatureHooked{false};
    std::atomic<bool> vulkanCreateSwapchainHooked{false};
    void* original_vkCreateSwapchainKHR = nullptr;
    std::atomic<uint32_t> lastUpscalerEvaluation{0xFFFFFFFFu};

    std::atomic<bool> dlssgSetOptionsHooked{false};
    std::atomic<bool> dlssgGetStateHooked{false};
    std::atomic<bool> reflexSleepHooked{false};
    std::atomic<bool> reflexSetOptionsHooked{false};
    std::atomic<bool> reflexSetConstantsHooked{false};

    std::atomic<bool> dlssgSetOptionsReturnedWrapperFallbackLogged{false};
    std::atomic<bool> dlssgGetStateReturnedWrapperFallbackLogged{false};
    std::atomic<bool> reflexSleepReturnedWrapperFallbackLogged{false};
    std::atomic<bool> reflexSetOptionsReturnedWrapperFallbackLogged{false};
    std::atomic<bool> reflexSetConstantsReturnedWrapperFallbackLogged{false};

    std::atomic<bool> dlssgSetOptionsProactiveFallbackLogged{false};
    std::atomic<bool> dlssgGetStateProactiveFallbackLogged{false};
    std::atomic<bool> reflexSleepProactiveFallbackLogged{false};
    std::atomic<bool> reflexSetOptionsProactiveFallbackLogged{false};
    std::atomic<bool> reflexSetConstantsProactiveFallbackLogged{false};

    std::atomic<bool> dlssgSetOptionsLookupLogged{false};
    std::atomic<bool> dlssgGetStateLookupLogged{false};
    std::atomic<bool> reflexSleepLookupLogged{false};
    std::atomic<bool> reflexSetOptionsLookupLogged{false};
    std::atomic<bool> reflexSetConstantsLookupLogged{false};

    std::atomic<ULONGLONG> reflexFeatureHookRetryLastMs{0};

    std::unordered_map<uint32_t, ViewportFGState> viewportStates;
    std::unordered_map<uint32_t, ViewportFGState> viewportLoggedStates;
    std::unordered_map<uint32_t, uint32_t> viewportCapabilityMax;

    std::atomic<ULONGLONG> suppressNewGetStateActivationUntilMs{0};
    std::atomic<bool> blockGetStateOnlyReactivationUntilExplicitSetOptions{false};
    std::atomic<bool> blockGetStateOnlyReactivationUntilSafePostFSRBootstrap{false};
    std::atomic<bool> currentComebackActivatedViaExplicitSetOptions{false};
    std::atomic<bool> acceptedRuntimeOffAwaitingSetOptions{false};
    std::atomic<bool> confirmedDLSSReflexSuspendPending{false};
    std::atomic<bool> startupWindowOffExtensionPending{false};

    std::mutex suppressedOffMutex;
    bool suppressedSetOptionsOffDuringStartup = false;
    slViewportHandle suppressedOffViewport = {};
    slDLSSGOptions suppressedOffOptions = {};
    uint32_t suppressedOffViewportKey = 0;
    std::atomic<uint64_t> titleFrameMarkerSequence{0};
    uint64_t suppressedOffTitleFrameSequence = 0;

    std::atomic<uint64_t> reflexSleepObservedCount{0};
    std::atomic<uint64_t> reflexSleepLastTickMs{0};
    std::atomic<uint64_t> reflexSetOptionsObservedCount{0};
    std::atomic<uint64_t> reflexSetOptionsLastTickMs{0};
    std::atomic<int32_t> reflexLastForwardedMode{-1};

    std::atomic<uint64_t> dlssgNotInterpolatingStreak{0};
    std::atomic<uint64_t> reflexSleepCountAtLastHealthLog{0};
    std::atomic<uint32_t> dlssgLastObservedStatus{0};

    PFN_slGetFeatureFunction original_slGetFeatureFunction = nullptr;
    PFN_slGetPluginFunction original_slGetPluginFunction = nullptr;
    PFN_slSetD3DDevice original_slSetD3DDevice = nullptr;
    PFN_slSetTag original_slSetTag = nullptr;
    PFN_slSetTagForFrame original_slSetTagForFrame = nullptr;
    PFN_slEvaluateFeature original_slEvaluateFeature = nullptr;

    std::atomic<uint64_t> streamlineModuleUnloadGeneration{0};
    std::atomic<bool> streamlineTeardownInFlight{false};

    PFN_slDLSSGSetOptions original_slDLSSGSetOptions = nullptr;
    PFN_slDLSSGGetState original_slDLSSGGetState = nullptr;
    PFN_slReflexSleep original_slReflexSleep = nullptr;
    PFN_slReflexSetOptions original_slReflexSetOptions = nullptr;
    PFN_slReflexSetConstants original_slReflexSetConstants = nullptr;
};

inline StreamlineHookState g_StreamlineHookState;

inline std::mutex& streamline_hook_g_InitMutex = g_StreamlineHookState.initMutex;
inline std::mutex& streamline_hook_g_StateMutex = g_StreamlineHookState.stateMutex;
inline std::mutex& streamline_hook_g_ModuleHookMutex = g_StreamlineHookState.moduleHookMutex;
inline std::mutex& streamline_hook_g_FeatureHookMutex = g_StreamlineHookState.featureHookMutex;
inline std::mutex& streamline_hook_g_AcceptedD3D12DeviceMutex = g_StreamlineHookState.acceptedD3D12DeviceMutex;
inline ID3D12Device*& streamline_hook_g_AcceptedD3D12Device = g_StreamlineHookState.acceptedD3D12Device;
inline std::atomic<bool>& streamline_hook_g_DynamicHooksRegistered = g_StreamlineHookState.dynamicHooksRegistered;
inline std::atomic<bool>& streamline_hook_g_StreamlineUsesD3D12 = g_StreamlineHookState.streamlineUsesD3D12;
inline std::atomic<bool>& streamline_hook_g_NoModulesLogged = g_StreamlineHookState.noModulesLogged;
inline std::atomic<bool>& streamline_hook_g_ModuleSnapshotFailureLogged = g_StreamlineHookState.moduleSnapshotFailureLogged;
inline std::atomic<bool>& streamline_hook_g_ModuleSnapshotRetrySuccessLogged = g_StreamlineHookState.moduleSnapshotRetrySuccessLogged;
inline std::atomic<uint32_t>& streamline_hook_g_IATPatchesMask = g_StreamlineHookState.iatPatchesMask;
inline std::atomic<uint32_t>& streamline_hook_g_InstalledModuleMask = g_StreamlineHookState.installedModuleMask;

inline std::atomic<void*>& streamline_hook_g_SLGetFeatureFunctionTarget = g_StreamlineHookState.slGetFeatureFunctionTarget;
inline std::atomic<void*>& streamline_hook_g_SLGetPluginFunctionTarget = g_StreamlineHookState.slGetPluginFunctionTarget;
inline std::atomic<void*>& streamline_hook_g_SLSetD3DDeviceTarget = g_StreamlineHookState.slSetD3DDeviceTarget;
inline std::atomic<void*>& streamline_hook_g_SLSetTagTarget = g_StreamlineHookState.slSetTagTarget;
inline std::atomic<void*>& streamline_hook_g_SLSetTagForFrameTarget = g_StreamlineHookState.slSetTagForFrameTarget;
inline std::atomic<void*>& streamline_hook_g_SLEvaluateFeatureTarget = g_StreamlineHookState.slEvaluateFeatureTarget;
inline std::atomic<void*>& streamline_hook_g_VulkanCreateSwapchainTarget = g_StreamlineHookState.vulkanCreateSwapchainTarget;

inline std::atomic<void*>& streamline_hook_g_DLSSGSetOptionsTarget = g_StreamlineHookState.dlssgSetOptionsTarget;
inline std::atomic<void*>& streamline_hook_g_DLSSGGetStateTarget = g_StreamlineHookState.dlssgGetStateTarget;
inline std::atomic<void*>& streamline_hook_g_ReflexSleepTarget = g_StreamlineHookState.reflexSleepTarget;
inline std::atomic<void*>& streamline_hook_g_ReflexSetOptionsTarget = g_StreamlineHookState.reflexSetOptionsTarget;
inline std::atomic<void*>& streamline_hook_g_ReflexSetConstantsTarget = g_StreamlineHookState.reflexSetConstantsTarget;

inline std::atomic<void*>& streamline_hook_g_DLSSGSetOptionsImportFallbackAttemptedTarget = g_StreamlineHookState.dlssgSetOptionsImportFallbackAttemptedTarget;
inline std::atomic<void*>& streamline_hook_g_DLSSGGetStateImportFallbackAttemptedTarget = g_StreamlineHookState.dlssgGetStateImportFallbackAttemptedTarget;
inline std::atomic<void*>& streamline_hook_g_ReflexSleepImportFallbackAttemptedTarget = g_StreamlineHookState.reflexSleepImportFallbackAttemptedTarget;
inline std::atomic<void*>& streamline_hook_g_ReflexSetOptionsImportFallbackAttemptedTarget = g_StreamlineHookState.reflexSetOptionsImportFallbackAttemptedTarget;
inline std::atomic<void*>& streamline_hook_g_ReflexSetConstantsImportFallbackAttemptedTarget = g_StreamlineHookState.reflexSetConstantsImportFallbackAttemptedTarget;

inline std::atomic<void*>& streamline_hook_g_DLSSGSetOptionsFailedTarget = g_StreamlineHookState.dlssgSetOptionsFailedTarget;
inline std::atomic<uint32_t>& streamline_hook_g_DLSSGSetOptionsFailedAttempts = g_StreamlineHookState.dlssgSetOptionsFailedAttempts;
inline std::atomic<void*>& streamline_hook_g_DLSSGGetStateFailedTarget = g_StreamlineHookState.dlssgGetStateFailedTarget;
inline std::atomic<uint32_t>& streamline_hook_g_DLSSGGetStateFailedAttempts = g_StreamlineHookState.dlssgGetStateFailedAttempts;
inline std::atomic<void*>& streamline_hook_g_ReflexSleepFailedTarget = g_StreamlineHookState.reflexSleepFailedTarget;
inline std::atomic<uint32_t>& streamline_hook_g_ReflexSleepFailedAttempts = g_StreamlineHookState.reflexSleepFailedAttempts;
inline std::atomic<void*>& streamline_hook_g_ReflexSetOptionsFailedTarget = g_StreamlineHookState.reflexSetOptionsFailedTarget;
inline std::atomic<uint32_t>& streamline_hook_g_ReflexSetOptionsFailedAttempts = g_StreamlineHookState.reflexSetOptionsFailedAttempts;
inline std::atomic<void*>& streamline_hook_g_ReflexSetConstantsFailedTarget = g_StreamlineHookState.reflexSetConstantsFailedTarget;
inline std::atomic<uint32_t>& streamline_hook_g_ReflexSetConstantsFailedAttempts = g_StreamlineHookState.reflexSetConstantsFailedAttempts;
inline std::atomic<void*>& streamline_hook_g_PCLSetMarkerFailedTarget = g_StreamlineHookState.pclSetMarkerFailedTarget;
inline std::atomic<uint32_t>& streamline_hook_g_PCLSetMarkerFailedAttempts = g_StreamlineHookState.pclSetMarkerFailedAttempts;

inline std::atomic<int>& streamline_hook_g_ReflexSleepUnavailableQueries = g_StreamlineHookState.reflexSleepUnavailableQueries;
inline std::atomic<int>& streamline_hook_g_ReflexSetOptionsUnavailableQueries = g_StreamlineHookState.reflexSetOptionsUnavailableQueries;
inline constexpr int kReflexSetConstantsUnavailableQueryLimit = 3;
inline std::atomic<int>& streamline_hook_g_ReflexSetConstantsUnavailableQueries = g_StreamlineHookState.reflexSetConstantsUnavailableQueries;
inline std::atomic<int>& streamline_hook_g_PCLUnavailableQueries = g_StreamlineHookState.pclUnavailableQueries;
inline std::atomic<uint32_t>& streamline_hook_g_RuntimeReflexRetryAttempts = g_StreamlineHookState.runtimeReflexRetryAttempts;

inline std::atomic<bool>& streamline_hook_g_SLGetFeatureFunctionHooked = g_StreamlineHookState.slGetFeatureFunctionHooked;
inline std::atomic<bool>& streamline_hook_g_SLGetPluginFunctionHooked = g_StreamlineHookState.slGetPluginFunctionHooked;
inline std::atomic<bool>& streamline_hook_g_SLSetD3DDeviceHooked = g_StreamlineHookState.slSetD3DDeviceHooked;
inline std::atomic<bool>& streamline_hook_g_SLSetTagHooked = g_StreamlineHookState.slSetTagHooked;
inline std::atomic<bool>& streamline_hook_g_SLSetTagForFrameHooked = g_StreamlineHookState.slSetTagForFrameHooked;
inline std::atomic<bool>& streamline_hook_g_SLEvaluateFeatureHooked = g_StreamlineHookState.slEvaluateFeatureHooked;
inline std::atomic<bool>& streamline_hook_g_VulkanCreateSwapchainHooked = g_StreamlineHookState.vulkanCreateSwapchainHooked;
inline void*& streamline_hook_g_Original_vkCreateSwapchainKHR = g_StreamlineHookState.original_vkCreateSwapchainKHR;
inline std::atomic<uint32_t>& streamline_hook_g_LastUpscalerEvaluation = g_StreamlineHookState.lastUpscalerEvaluation;

inline std::atomic<bool>& streamline_hook_g_DLSSGSetOptionsHooked = g_StreamlineHookState.dlssgSetOptionsHooked;
inline std::atomic<bool>& streamline_hook_g_DLSSGGetStateHooked = g_StreamlineHookState.dlssgGetStateHooked;
inline std::atomic<bool>& streamline_hook_g_ReflexSleepHooked = g_StreamlineHookState.reflexSleepHooked;
inline std::atomic<bool>& streamline_hook_g_ReflexSetOptionsHooked = g_StreamlineHookState.reflexSetOptionsHooked;
inline std::atomic<bool>& streamline_hook_g_ReflexSetConstantsHooked = g_StreamlineHookState.reflexSetConstantsHooked;

inline std::atomic<bool>& streamline_hook_g_DLSSGSetOptionsReturnedWrapperFallbackLogged = g_StreamlineHookState.dlssgSetOptionsReturnedWrapperFallbackLogged;
inline std::atomic<bool>& streamline_hook_g_DLSSGGetStateReturnedWrapperFallbackLogged = g_StreamlineHookState.dlssgGetStateReturnedWrapperFallbackLogged;
inline std::atomic<bool>& streamline_hook_g_ReflexSleepReturnedWrapperFallbackLogged = g_StreamlineHookState.reflexSleepReturnedWrapperFallbackLogged;
inline std::atomic<bool>& streamline_hook_g_ReflexSetOptionsReturnedWrapperFallbackLogged = g_StreamlineHookState.reflexSetOptionsReturnedWrapperFallbackLogged;
inline std::atomic<bool>& streamline_hook_g_ReflexSetConstantsReturnedWrapperFallbackLogged = g_StreamlineHookState.reflexSetConstantsReturnedWrapperFallbackLogged;

inline std::atomic<bool>& streamline_hook_g_DLSSGSetOptionsProactiveFallbackLogged = g_StreamlineHookState.dlssgSetOptionsProactiveFallbackLogged;
inline std::atomic<bool>& streamline_hook_g_DLSSGGetStateProactiveFallbackLogged = g_StreamlineHookState.dlssgGetStateProactiveFallbackLogged;
inline std::atomic<bool>& streamline_hook_g_ReflexSleepProactiveFallbackLogged = g_StreamlineHookState.reflexSleepProactiveFallbackLogged;
inline std::atomic<bool>& streamline_hook_g_ReflexSetOptionsProactiveFallbackLogged = g_StreamlineHookState.reflexSetOptionsProactiveFallbackLogged;
inline std::atomic<bool>& streamline_hook_g_ReflexSetConstantsProactiveFallbackLogged = g_StreamlineHookState.reflexSetConstantsProactiveFallbackLogged;

inline std::atomic<bool>& streamline_hook_g_DLSSGSetOptionsLookupLogged = g_StreamlineHookState.dlssgSetOptionsLookupLogged;
inline std::atomic<bool>& streamline_hook_g_DLSSGGetStateLookupLogged = g_StreamlineHookState.dlssgGetStateLookupLogged;
inline std::atomic<bool>& streamline_hook_g_ReflexSleepLookupLogged = g_StreamlineHookState.reflexSleepLookupLogged;
inline std::atomic<bool>& streamline_hook_g_ReflexSetOptionsLookupLogged = g_StreamlineHookState.reflexSetOptionsLookupLogged;
inline std::atomic<bool>& streamline_hook_g_ReflexSetConstantsLookupLogged = g_StreamlineHookState.reflexSetConstantsLookupLogged;

inline std::atomic<ULONGLONG>& streamline_hook_g_ReflexFeatureHookRetryLastMs = g_StreamlineHookState.reflexFeatureHookRetryLastMs;

inline std::unordered_map<uint32_t, ViewportFGState>& streamline_hook_g_ViewportStates = g_StreamlineHookState.viewportStates;
inline std::unordered_map<uint32_t, ViewportFGState>& streamline_hook_g_ViewportLoggedStates = g_StreamlineHookState.viewportLoggedStates;
inline std::unordered_map<uint32_t, uint32_t>& streamline_hook_g_ViewportCapabilityMax = g_StreamlineHookState.viewportCapabilityMax;

inline std::atomic<ULONGLONG>& streamline_hook_g_SuppressNewGetStateActivationUntilMs = g_StreamlineHookState.suppressNewGetStateActivationUntilMs;
inline constexpr ULONGLONG streamline_hook_kAuthoritativeFFXTakeoverGetStateSuppressMs = 250;

inline std::atomic<bool>& streamline_hook_g_BlockGetStateOnlyReactivationUntilExplicitSetOptions = g_StreamlineHookState.blockGetStateOnlyReactivationUntilExplicitSetOptions;
inline constexpr uint32_t streamline_hook_kGetStateOnlyBlockGenerationRetireSamples = 3;

inline std::atomic<bool>& streamline_hook_g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap = g_StreamlineHookState.blockGetStateOnlyReactivationUntilSafePostFSRBootstrap;
inline std::atomic<bool>& streamline_hook_g_CurrentComebackActivatedViaExplicitSetOptions = g_StreamlineHookState.currentComebackActivatedViaExplicitSetOptions;
inline std::atomic<bool>& streamline_hook_g_AcceptedRuntimeOffAwaitingSetOptions = g_StreamlineHookState.acceptedRuntimeOffAwaitingSetOptions;
inline std::atomic<bool>& streamline_hook_g_ConfirmedDLSSReflexSuspendPending = g_StreamlineHookState.confirmedDLSSReflexSuspendPending;
inline std::atomic<bool>& streamline_hook_g_StartupWindowOffExtensionPending = g_StreamlineHookState.startupWindowOffExtensionPending;

inline std::mutex& streamline_hook_g_SuppressedOffMutex = g_StreamlineHookState.suppressedOffMutex;
inline bool& streamline_hook_g_SuppressedSetOptionsOffDuringStartup = g_StreamlineHookState.suppressedSetOptionsOffDuringStartup;
inline slViewportHandle& streamline_hook_g_SuppressedOffViewport = g_StreamlineHookState.suppressedOffViewport;
inline slDLSSGOptions& streamline_hook_g_SuppressedOffOptions = g_StreamlineHookState.suppressedOffOptions;
inline uint32_t& streamline_hook_g_SuppressedOffViewportKey = g_StreamlineHookState.suppressedOffViewportKey;
inline std::atomic<uint64_t>& streamline_hook_g_TitleFrameMarkerSequence = g_StreamlineHookState.titleFrameMarkerSequence;
inline uint64_t& streamline_hook_g_SuppressedOffTitleFrameSequence = g_StreamlineHookState.suppressedOffTitleFrameSequence;

inline constexpr uint32_t streamline_hook_kDLSSGStatusFailResolutionTooLow = 1u << 0;
inline constexpr uint32_t streamline_hook_kDLSSGStatusFailReflexNotDetectedAtRuntime = 1u << 1;
inline constexpr uint32_t streamline_hook_kDLSSGStatusFailHDRFormatNotSupported = 1u << 2;
inline constexpr uint32_t streamline_hook_kDLSSGStatusFailCommonConstantsInvalid = 1u << 3;
inline constexpr uint32_t streamline_hook_kDLSSGStatusFailGetCurrentBackBufferIndex = 1u << 4;
void FormatDLSSGStatusFlags(uint32_t status, char* buffer, size_t bufferSize);

inline std::atomic<uint64_t>& streamline_hook_g_ReflexSleepObservedCount = g_StreamlineHookState.reflexSleepObservedCount;
inline std::atomic<uint64_t>& streamline_hook_g_ReflexSleepLastTickMs = g_StreamlineHookState.reflexSleepLastTickMs;
inline std::atomic<uint64_t>& streamline_hook_g_ReflexSetOptionsObservedCount = g_StreamlineHookState.reflexSetOptionsObservedCount;
inline std::atomic<uint64_t>& streamline_hook_g_ReflexSetOptionsLastTickMs = g_StreamlineHookState.reflexSetOptionsLastTickMs;
inline std::atomic<int32_t>& streamline_hook_g_ReflexLastForwardedMode = g_StreamlineHookState.reflexLastForwardedMode;

inline std::atomic<uint64_t>& streamline_hook_g_DLSSGNotInterpolatingStreak = g_StreamlineHookState.dlssgNotInterpolatingStreak;
inline std::atomic<uint64_t>& streamline_hook_g_ReflexSleepCountAtLastHealthLog = g_StreamlineHookState.reflexSleepCountAtLastHealthLog;
inline std::atomic<uint32_t>& streamline_hook_g_DLSSGLastObservedStatus = g_StreamlineHookState.dlssgLastObservedStatus;

inline constexpr uint64_t streamline_hook_kDLSSGHealthWarnStreak = 8;
inline constexpr uint64_t streamline_hook_kDLSSGHealthWarnRepeat = 512;

inline PFN_slGetFeatureFunction& streamline_hook_g_Original_slGetFeatureFunction = g_StreamlineHookState.original_slGetFeatureFunction;
inline PFN_slGetPluginFunction& streamline_hook_g_Original_slGetPluginFunction = g_StreamlineHookState.original_slGetPluginFunction;
inline PFN_slSetD3DDevice& streamline_hook_g_Original_slSetD3DDevice = g_StreamlineHookState.original_slSetD3DDevice;
inline PFN_slSetTag& streamline_hook_g_Original_slSetTag = g_StreamlineHookState.original_slSetTag;
inline PFN_slSetTagForFrame& streamline_hook_g_Original_slSetTagForFrame = g_StreamlineHookState.original_slSetTagForFrame;
inline PFN_slEvaluateFeature& streamline_hook_g_Original_slEvaluateFeature = g_StreamlineHookState.original_slEvaluateFeature;

inline std::atomic<uint64_t>& streamline_hook_g_StreamlineModuleUnloadGeneration = g_StreamlineHookState.streamlineModuleUnloadGeneration;
inline std::atomic<bool>& streamline_hook_g_StreamlineTeardownInFlight = g_StreamlineHookState.streamlineTeardownInFlight;

inline PFN_slDLSSGSetOptions& streamline_hook_g_Original_slDLSSGSetOptions = g_StreamlineHookState.original_slDLSSGSetOptions;
inline PFN_slDLSSGGetState& streamline_hook_g_Original_slDLSSGGetState = g_StreamlineHookState.original_slDLSSGGetState;
inline PFN_slReflexSleep& streamline_hook_g_Original_slReflexSleep = g_StreamlineHookState.original_slReflexSleep;
inline PFN_slReflexSetOptions& streamline_hook_g_Original_slReflexSetOptions = g_StreamlineHookState.original_slReflexSetOptions;
inline PFN_slReflexSetConstants& streamline_hook_g_Original_slReflexSetConstants = g_StreamlineHookState.original_slReflexSetConstants;

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

slResult Hooked_slReflexSetConstants(const SLReflexConstants& streamline_hook_consts);
const char* GetDLSSGModeName(uint32_t mode);
const char* GetModuleBaseName(const char* moduleNameOrPath);
bool IsStreamlineModuleName(const char* moduleNameOrPath);
bool ShouldHookStreamlineCoreExports(const char* moduleNameOrPath);
bool IsStreamlineCoreDynamicHookModule(const char* moduleBaseName, HMODULE);
bool IsStreamlineDLSSGDynamicHookModule(const char* moduleBaseName, HMODULE);
bool IsStreamlineReflexDynamicHookModule(const char* moduleBaseName, HMODULE);
uint32_t GetModuleMaskBit(const char* moduleNameOrPath);
void LogSkippedStreamlineCoreExportsOnce(const char* moduleBaseName, HMODULE module, bool hasGetFeature,
                                         bool hasGetPlugin, bool hasSetD3DDevice);
size_t GetModuleImageSizeBytes(HMODULE module);
bool DoesAddressBelongToLoadedModule(void* address, HMODULE* ownerModule, char* ownerPath, DWORD ownerPathCapacity,
                                     DWORD* outError);
void LogStaleStreamlineOriginalBlockedOnce(const char* streamline_hook_functionName, void* original,
                                           void* validationAddress, const char* expectedModuleRole, DWORD error);
bool IsSavedStreamlineOriginalCallable(const char* streamline_hook_functionName, void* original,
                                       void* validationAddress, const char* expectedModuleRole);
PFN_slGetFeatureFunction GetCallableOriginalGetFeatureFunction();
PFN_slGetPluginFunction GetCallableOriginalGetPluginFunction();
PFN_slSetD3DDevice GetCallableOriginalSetD3DDevice();
PFN_slSetTag GetCallableOriginalSetTag();
PFN_slSetTagForFrame GetCallableOriginalSetTagForFrame();
PFN_slEvaluateFeature GetCallableOriginalEvaluateFeature();
PFN_slDLSSGSetOptions GetCallableOriginalDLSSGSetOptions();
PFN_slDLSSGGetState GetCallableOriginalDLSSGGetState();
PFN_slReflexSleep GetCallableOriginalReflexSleep();
PFN_slReflexSetOptions GetCallableOriginalReflexSetOptions();
PFN_slReflexSetConstants GetCallableOriginalReflexSetConstants();
uint32_t GetViewportKey(const slViewportHandle& viewport);
slDLSSGOptions CloneDLSSGOptions(const slDLSSGOptions& source);
int GetEffectiveMultiplier(const slDLSSGOptions& streamline_hook_options);

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
void LogDLSSGSetOptionsTransition(uint32_t viewportKey, const slDLSSGOptions& requestedOptions,
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
};
void LogStreamlineReflexSignalChange(const char* sourceName, int32_t mode, uint32_t incomingFrameLimitUs,
                                     uint32_t forwardedFrameLimitUs, uint32_t targetIntervalUs);
void MaybePrepareForStreamlineEnableTransitionFromReflex(const char* sourceName);
void HandleStreamlineReflexPacingSignal(const char* sourceName, int32_t mode, uint32_t incomingFrameLimitUs,
                                        uint32_t forwardedFrameLimitUs, uint32_t targetIntervalUs);
uint32_t GetCachedCapabilityMax(uint32_t viewportKey);
void CacheCapabilityMax(uint32_t viewportKey, uint32_t capabilityMax);
void ApplyCombinedDLSSFGState(bool active, int multiplier);
void ApplyCombinedStreamlineRuntimeState(bool active, int multiplier, bool explicitSetOptionsEnableSignal,
                                         const char* source);
bool WasViewportRuntimeStateActive(uint32_t viewportKey);
bool ShouldSuppressNewGetStateActivation();
bool HasDLSSGRuntimeFenceEvidence(const slDLSSGState& state);
void UpdateViewportRuntimeState(uint32_t viewportKey, bool active, int multiplier, uint32_t generatedFrames,
                                uint32_t capabilityMax, const char* source,
                                bool clearAllViewportStatesForDisable = false);
void MaybeRetireGetStateOnlyReactivationBlockForSustainedGeneration(bool callSucceeded, const slDLSSGState& state,
                                                                    const slDLSSGOptions* options, bool viewportWasActive,
                                                                    uint32_t viewportKey);

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

#include "streamline_hook_declarations.h"
