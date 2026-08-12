// Test stubs for hook symbols needed by tests
// These provide minimal implementations to satisfy linker requirements

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include <windows.h>
#include <atomic>

#include "../hook/apis/streamline_hook.h"
#include "../hook/common/fg_runtime_state.h"
#include "../hook/common/hook_common.h"
#include "../hook/common/custom_overlay_vk.h"

namespace CustomOverlay {
VulkanBackend::VulkanBackend(VkDevice device, VkPhysicalDevice physDevice, VkQueue queue, uint32_t queueFamily)
    : device(device), physicalDevice(physDevice), queue(queue), queueFamilyIndex(queueFamily) {}
VulkanBackend::~VulkanBackend() = default;
void VulkanBackend::SetDispatchTable(void* deviceTable, void* instanceTable) {
    deviceDispatch = deviceTable;
    instanceDispatch = instanceTable;
}
bool VulkanBackend::Initialize(int, int, const uint8_t*) {
    return false;
}
void VulkanBackend::Shutdown() {}
void VulkanBackend::Render(const std::vector<DrawVertex>&, const std::vector<uint16_t>&,
                           const std::vector<DrawCommand>&, int, int) {}
}  // namespace CustomOverlay

// Stubs for dxgi_shared.cpp dependencies
bool IsInWrapperPresent() {
    return false;
}

namespace DXGIShared {
void HandleDX12ProcessFrame(IDXGISwapChain*, bool, bool) {}
void HandleDX11ProcessFrame(IDXGISwapChain*, bool) {}
void HandleDX12ResizeBegin() {}
void HandleDX11ResizeBegin() {}
void HandleDX12ResizeEnd() {}
}  // namespace DXGIShared

// Stubs for InlineHook - need to match actual class definition
// The real InlineHook is a class with static methods, so we provide definitions here
#include "inline_hook.h"

bool InlineHook::Install(void*, void*, void**) {
    return false;
}
bool InlineHook::InstallPublished(void*, void*, void**, TrampolinePublisher, void*) {
    return false;
}
void InlineHook::RemoveAll() {}
bool InlineHook::Remove(void*) {
    return true;
}
void* InlineHook::CreateBypassTrampoline(void*) {
    return nullptr;
}
void* InlineHook::InstallDeepHookPublished(void*, void*, TrampolinePublisher, void*, int) {
    return nullptr;
}
bool InlineHook::IsInstalledEntryPatchIntact(void*, void** currentJumpTargetOut) {
    if (currentJumpTargetOut) {
        *currentJumpTargetOut = nullptr;
    }
    return true;
}
bool InlineHook::IsInTrampolinePool(void*) {
    return false;
}

// Stubs for DX12 - C++ linkage (matching header declarations in dx12_hook.h)
// Note: These are regular C++ functions, not extern "C"
void DX12_InvalidateSwapchain() {}
void DX12_ProcessFrameExternal(IDXGISwapChain*) {}
void DX12_AccountOverlayTransportPresent(bool, const char*, const char*) {}
bool DX12_TryRenderExactPostSLBeforeStartupHandoffPresent(IDXGISwapChain*, const char*) {
    return false;
}
bool DX12_TryRenderExactPostSLOffKeepAliveBeforePresent(IDXGISwapChain*, const char*) {
    return false;
}
void DX12_OnSwapchainResizeBegin() {}
void DX12_OnSwapchainResizeEnd() {}
void DX12_SignalFSR4SwapchainRecreated() {}
void DX12_ServiceDeferredECLProbe() {}
void DX12_RetainStreamlineStartupActivationSwapchain(IDXGISwapChain*, const char*) {}
bool DX12_TryInvokePostSLStartupActivationCallback(const char*, bool, bool) {
    return false;
}
DWORD DX12_GetGamePresentThreadId() {
    return 0;
}
void DX12_DumpDredIfDeviceRemoved(const char*) {}
void DX12_LogOverlayGpuBreadcrumbs(const char*) {}
void DX12_NoteFfxConfigureForward(uint64_t) {}
void FFXHook_ResetVehDisarmAndRearm() {}
bool DX12_IsNativeFSRInternalNoCallbackCompositionActive() {
    return false;
}
bool DX12_IsLiveSwapchainQueueOriginalGameQueue() {
    return false;
}
bool DX12_IsNativeFSRFGSuspendedDisablePending() {
    return false;
}
bool DX12_CompositeOverlayOntoCachedFFXUiResource() {
    return false;
}
void DX12_ProcessFrameMinimal(IDXGISwapChain*, bool, bool) {}
bool DX12_ShouldCacheFFXUiResourceForBundle() {
    return false;
}
bool DX12_IsFFXUiResourceCachedForBundle() {
    return false;
}
// FFX proxy-swapchain Present hook (game-thread composite driver): tests exercise DetourPresent's fallback
// arm, so the proxy driver reports not-driving here.
bool DX12_IsFFXProxyPresentHookDriving() {
    return false;
}
bool DX12_IsFFXProxyPresentHookInstalled() {
    return false;
}
bool DX12_IsCurrentThreadInsideFFXProxyPresentPrework() {
    return false;
}
bool DX12_TryInstallFFXProxyPresentHook(void*, void*, const char*) {
    return false;
}
void DX12_RemoveFFXProxyPresentHook(const char*) {}
void DX12_LogFFXProxyPresentHookFreezeDiagnostics(const char*) {}

// DX12_SetCommandQueue is extern "C" in the header
extern "C" void DX12_SetCommandQueue(ID3D12CommandQueue*) {}

// Stubs for DX11
extern "C" void DX11_ProcessFrameExternal(IDXGISwapChain*) {}
void ApplyPrerenderLimit(IDXGISwapChain*, float) {}

// Stubs for custom_overlay_dx12.cpp
HRESULT D3D12SerializeRootSignature(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
                                    ID3DBlob**) {
    return E_NOTIMPL;
}

// Stubs for freeze_watchdog.cpp
extern "C" BOOL MiniDumpWriteDump(HANDLE, DWORD, HANDLE, int, void*, void*, void*) {
    return FALSE;
}

// Stubs for hook_common.cpp
#include "config.h"
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
static AppConfig g_LocalConfigInstance{};
AppConfig* g_pLocalConfig = &g_LocalConfigInstance;
static std::atomic<bool> g_TestPreferredOverlayFGPublicationStateValid{false};
static std::atomic<bool> g_TestPreferredOverlayFGPublicationStateActive{false};
static std::atomic<int> g_TestPreferredOverlayFGPublicationStateRuntimeMode{
    static_cast<int>(ce::fg_runtime::RuntimeMode::kOff)};
static std::atomic<uint64_t> g_TestOverlayFGPublicationSequence{0};
static std::atomic<uint64_t> g_TestPreferredOverlayFGPublicationStateSequence{0};
bool IsProcessTerminating() {
    return false;
}

uint64_t HookAllocateOverlayFGPublicationSequence() {
    return g_TestOverlayFGPublicationSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void TestStubSetPreferredOverlayFGPublicationState(bool valid, bool active, ce::fg_runtime::RuntimeMode runtimeMode) {
    g_TestPreferredOverlayFGPublicationStateActive.store(active, std::memory_order_release);
    g_TestPreferredOverlayFGPublicationStateRuntimeMode.store(static_cast<int>(runtimeMode), std::memory_order_release);
    if (valid) {
        g_TestPreferredOverlayFGPublicationStateSequence.store(HookAllocateOverlayFGPublicationSequence(),
                                                               std::memory_order_release);
    } else {
        g_TestPreferredOverlayFGPublicationStateSequence.store(0, std::memory_order_release);
    }
    g_TestPreferredOverlayFGPublicationStateValid.store(valid, std::memory_order_release);
}

void TestStubResetPreferredOverlayFGPublicationState() {
    TestStubSetPreferredOverlayFGPublicationState(false, false, ce::fg_runtime::RuntimeMode::kOff);
}

bool HookIsPostSLOverlayActiveButUnconfirmed() {
    return false;
}

bool HookHasPostSLSyntheticStartupActivationEntered() {
    return false;
}

bool HookIsPostSLOverlayConfirmedRendering() {
    return false;
}

bool HookIsPostSLOverlayConfirmedButStartupSettling() {
    return false;
}

bool HookIsPostSLOverlayConfirmedButRuntimeStateStabilizing() {
    return false;
}

int HookGetPostSLRuntimeStateStabilizationLastFrame() {
    return 12;
}

bool HookIsPostSLOverlayConfirmedButGetStateOffWarmupProtected() {
    return false;
}

int HookGetPostSLGetStateOffWarmupProtectionLastFrame() {
    return 30;
}

bool HookHasFSRFGHistory() {
    return false;
}

bool HookHasExplicitStreamlineSetOptionsActivation() {
    return false;
}

bool HookHasSafePostFSRBootstrapPath() {
    return false;
}

bool HookHasRuntimeOwnedNativeFGPresentPath() {
    return false;
}

bool HookTryGetPreferredOverlayFGPublicationState(PreferredOverlayFGPublicationState* state) {
    if (!state || !g_TestPreferredOverlayFGPublicationStateValid.load(std::memory_order_acquire)) {
        return false;
    }

    state->valid = true;
    state->active = g_TestPreferredOverlayFGPublicationStateActive.load(std::memory_order_acquire);
    state->runtimeMode = static_cast<ce::fg_runtime::RuntimeMode>(
        g_TestPreferredOverlayFGPublicationStateRuntimeMode.load(std::memory_order_acquire));
    state->sequence = g_TestPreferredOverlayFGPublicationStateSequence.load(std::memory_order_acquire);
    return true;
}

void HookUpdatePreferredOverlayFGPublicationState(bool active, ce::fg_runtime::RuntimeMode runtimeMode, const char*) {
    TestStubSetPreferredOverlayFGPublicationState(true, active, runtimeMode);
}

// Stubs for streamline_hook.cpp (StreamlineHook namespace)
namespace StreamlineHook {
ExternalOverlayPresentGuard::ExternalOverlayPresentGuard() {}
ExternalOverlayPresentGuard::~ExternalOverlayPresentGuard() {}
bool IsExternalOverlayPresentGuardActive() {
    return false;
}
bool IsExternalOverlayPluginLookupGuardReady() {
    return false;
}
void FlushSuppressedSetOptionsOffIfNeeded() {}
}  // namespace StreamlineHook
