#include "vulkan_dxgi_fifo_present.h"

#include <atomic>
#include <mutex>

#include <dxgi1_6.h>

#include "../../common/log_meter.h"
#include "../common/dxgi_shared.h"
#include "../common/hook_common.h"
#include "../common/overlay_compat.h"
#include "../common/vulkan_dxgi_fifo_policy.h"
#include "../common/vulkan_dxgi_fifo_registry.h"
#include "iat_hook.h"
#include "inline_hook.h"
#include "wrapper_hooks.h"

namespace ce::vulkan_dxgi_fifo {
namespace {

// Invariant: this unit is the narrow WSI route, and it owns NO Present hook.
// The only factory capture is the CreateDXGIFactory/CreateDXGIFactory1/CreateDXGIFactory2
// export set of the real system dxgi.dll (IAT-filtered by IsSystemDxgiModule), the real
// factory is returned unwrapped to the caller, and only the bodies of its four
// swapchain creation methods (slots 10/15/16/24) receive deep inline hooks.
// Physical Present/Present1 interception is owned exactly once per process by
// DXGIShared (InstallPresentInlineHooks, including its foreign-overlay
// below-chain/prepend policy); this unit authorizes live-WSI swapchains for
// that hook and answers its per-present policy question via
// ApplyFinalPresentPolicy. Factory and swapchain COM vtables,
// QueryInterface/Release, object identity, creation descriptors, and every
// non-present method stay byte-for-byte outside CE. The rewrite itself is
// scoped twice: the armed/lifecycle policy (ShouldForceFifoNow) and the
// per-instance registry (g_observedSwapchains) built exclusively from
// swapchains whose target window was a live Vulkan Win32 surface at creation.

using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*,
                                                      IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND,
                                                             const DXGI_SWAP_CHAIN_DESC1*,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*,
                                                             IDXGISwapChain1**);
using CreateSwapChainForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, IUnknown*,
                                                                   const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                                                   IDXGISwapChain1**);
using CreateSwapChainForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*,
                                                                    const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*,
                                                                    IDXGISwapChain1**);

std::atomic<CreateSwapChainFn> g_createSwapChain{nullptr};
std::atomic<CreateSwapChainForHwndFn> g_createForHwnd{nullptr};
std::atomic<CreateSwapChainForCoreWindowFn> g_createForCoreWindow{nullptr};
std::atomic<CreateSwapChainForCompositionFn> g_createForComposition{nullptr};

std::atomic<void*> g_createSwapChainTarget{nullptr};
std::atomic<void*> g_createForHwndTarget{nullptr};
std::atomic<void*> g_createForCoreWindowTarget{nullptr};
std::atomic<void*> g_createForCompositionTarget{nullptr};

std::mutex g_installMutex;
std::atomic<bool> g_armed{false};

// Swapchain instances authorized at successful targeted creation. Membership
// in this registry is what scopes the final-present rewrite; see
// vulkan_dxgi_fifo_registry.h. Authorization requires the swapchain's target
// window to be a currently live Vulkan Win32 surface HWND (see
// IsLiveVulkanSurfaceHwnd), so a foreign or non-WSI swapchain is never
// registered. Entries are raw pointers with no COM reference and the gate
// never writes any vtable.
//
// Honest limits: the table never erases, so a destroyed swapchain leaves its
// address registered. That is the intended recreation contract for a fresh WSI
// swapchain reusing the address, but it also means a LATER non-WSI allocation
// that happens to reuse the address would inherit the registration while the
// policy is armed. Tracking destruction would require COM ownership the
// WSI route deliberately refuses, so the residual ABA window is accepted and
// bounded by the 64-slot capacity; registration is fail-closed when full.
ce::vulkan_dxgi_fifo_registry::ObservedSwapchainRegistry g_observedSwapchains;

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                                 IDXGISwapChain** swapchain);
HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window,
                                                       const DXGI_SWAP_CHAIN_DESC1* desc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                                       IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapchain);
HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForCoreWindow(IDXGIFactory2* factory, IUnknown* device, IUnknown* window,
                                                             const DXGI_SWAP_CHAIN_DESC1* desc,
                                                             IDXGIOutput* restrictToOutput,
                                                             IDXGISwapChain1** swapchain);
HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForComposition(IDXGIFactory2* factory, IUnknown* device,
                                                              const DXGI_SWAP_CHAIN_DESC1* desc,
                                                              IDXGIOutput* restrictToOutput,
                                                              IDXGISwapChain1** swapchain);

void** GetVtable(const void* object) {
    return object ? *reinterpret_cast<void***>(const_cast<void*>(object)) : nullptr;
}

bool IsSystemDxgiModule(const char* moduleBaseName, HMODULE module) {
    if (!module || !moduleBaseName || _stricmp(moduleBaseName, "dxgi.dll") != 0)
        return false;

    wchar_t modulePath[MAX_PATH] = {};
    return GetModuleFileNameW(module, modulePath, MAX_PATH) != 0 && IATHook::IsWindowsSystemModulePath(modulePath);
}

bool IsSystemDxgiCode(const void* address) {
    if (!address)
        return false;

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)) {
        return false;
    }

    wchar_t modulePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(module, modulePath, MAX_PATH) || !IATHook::IsWindowsSystemModulePath(modulePath))
        return false;

    const wchar_t* baseName = wcsrchr(modulePath, L'\\');
    baseName = baseName ? baseName + 1 : modulePath;
    return _wcsicmp(baseName, L"dxgi.dll") == 0;
}

template <typename Fn>
struct TrampolinePublication {
    std::atomic<Fn>* destination;
};

template <typename Fn>
void PublishTrampoline(void* trampoline, void* context) {
    auto* publication = static_cast<TrampolinePublication<Fn>*>(context);
    publication->destination->store(reinterpret_cast<Fn>(trampoline), std::memory_order_release);
}

template <typename Fn>
bool InstallEntryBodyHook(void* target, void* detour, std::atomic<void*>& installedTarget,
                          std::atomic<Fn>& trampolineOut, const char* label) {
    if (!IsSystemDxgiCode(target)) {
        HookLogImportant("Vulkan DXGI FIFO: refusing non-system %s body %p", label, target);
        return false;
    }
    if (installedTarget.load(std::memory_order_acquire) == target &&
        trampolineOut.load(std::memory_order_acquire)) {
        return true;
    }
    if (installedTarget.load(std::memory_order_acquire) != nullptr) {
        HookLogImportant("Vulkan DXGI FIFO: additional %s body %p ignored (active=%p)", label, target,
                         installedTarget.load(std::memory_order_relaxed));
        return false;
    }

    installedTarget.store(target, std::memory_order_release);
    TrampolinePublication<Fn> publication{&trampolineOut};
    void* trampoline = nullptr;
    if (!InlineHook::InstallPublished(target, detour, &trampoline, &PublishTrampoline<Fn>, &publication)) {
        void* recovered = nullptr;
        if (!InlineHook::TryGetInstalledTrampoline(target, detour, &recovered) || !recovered) {
            installedTarget.store(nullptr, std::memory_order_release);
            HookLogImportant("Vulkan DXGI FIFO: failed to install system %s body hook at %p", label, target);
            return false;
        }
        trampolineOut.store(reinterpret_cast<Fn>(recovered), std::memory_order_release);
        trampoline = recovered;
    }

    HookLogImportant("Vulkan DXGI FIFO: system %s body hook active (target=%p trampoline=%p)", label, target,
                     trampoline);
    return true;
}

bool ShouldForceFifoNow() {
    return ce::vulkan_dxgi_fifo_policy::ShouldForceFinalDxgiFifo(
        g_armed.load(std::memory_order_acquire), DXGIShared::IsVulkanActive(), HookIsShuttingDown());
}

// The live Win32 surface HWND set is owned by the resident layer DLL: the layer
// publishes at vkCreateWin32SurfaceKHR/vkDestroySurfaceKHR time and exports one
// query (CEVulkanLayerIsLiveVulkanSurfaceHwnd). Resolve it lazily the same way
// other routes reach the resident layer: GetModuleHandle + GetProcAddress over
// both arch names, no load, no IPC, no file mapping.
using IsLiveVulkanSurfaceHwndFn = BOOL (*)(HWND);

IsLiveVulkanSurfaceHwndFn ResolveLiveSurfaceHwndQuery() {
    HMODULE layer = GetModuleHandleW(L"VK_LAYER_CE_overlay_x86.dll");
    if (!layer)
        layer = GetModuleHandleW(L"VK_LAYER_CE_overlay.dll");
    if (!layer)
        return nullptr;
    return reinterpret_cast<IsLiveVulkanSurfaceHwndFn>(
        GetProcAddress(layer, "CEVulkanLayerIsLiveVulkanSurfaceHwnd"));
}

bool IsLiveVulkanSurfaceHwnd(HWND window) {
    if (!window)
        return false;
    // Cached resolution with a bounded retry: a stale resident layer without the
    // export resolves to null and stays fail-closed, but a later corrected layer
    // (new build installed while the process runs) still gets picked up.
    static std::atomic<IsLiveVulkanSurfaceHwndFn> query{ResolveLiveSurfaceHwndQuery()};
    IsLiveVulkanSurfaceHwndFn resolved = query.load(std::memory_order_acquire);
    if (!resolved) {
        resolved = ResolveLiveSurfaceHwndQuery();
        query.store(resolved, std::memory_order_release);
    }
    // Fail closed: without a resident layer export no HWND can be a live Vulkan
    // surface, so no swapchain is ever authorized through this path.
    return resolved && resolved(window);
}

void ApplyAndLogPresentParameters(const char* label, size_t slot, bool forceFifo, UINT& syncInterval, UINT& flags) {
    const UINT incomingSyncInterval = syncInterval;
    const UINT incomingFlags = flags;
    ce::vulkan_dxgi_fifo_policy::ApplyFinalDxgiFifoParameters(forceFifo, syncInterval, flags);

    // Cadence is per registered identity, so one chatty swapchain cannot mute
    // another's diagnostics and a quiet one is never logged on hot noise.
    const uint32_t count = g_observedSwapchains.NextPresentationOccurrence(slot);
    if (ce::log_meter::ShouldLogCadence(count, 8, 1024)) {
        HookLogImportant(
            "Vulkan DXGI FIFO: final %s #%u swapchain=%p slot=%zu force=%d SyncInterval=%u->%u Flags=0x%X->0x%X",
            label, count, g_observedSwapchains.PointerAt(slot), slot, forceFifo ? 1 : 0, incomingSyncInterval,
            syncInterval, incomingFlags, flags);
    }
}

// Foreign present: forced backstop is live but this instance was never
// authorized at a targeted creation. Its parameters pass through untouched;
// keep one bounded global cadence so the identity is visible without hot-path
// noise.
void LogForeignPresentPassThrough(const char* label, const void* swapchain) {
    static std::atomic<uint32_t> occurrence{0};
    const uint32_t count = occurrence.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ce::log_meter::ShouldLogCadence(count, 4, 4096)) {
        HookLogImportant(
            "Vulkan DXGI FIFO: foreign %s #%u swapchain=%p not authorized at a targeted creation; "
            "parameters pass through untouched",
            label, count, swapchain);
    }
}

// Unauthorized creation: the swapchain's target window is not a currently live
// Vulkan Win32 surface (or it has no Win32 target at all - CoreWindow and
// Composition swapchains cannot be Win32 WSI targets). It passes through
// unregistered; one bounded cadence keeps the decision visible.
void LogUnregisteredCreation(const void* swapchain, const char* source, HWND window) {
    static std::atomic<uint32_t> occurrence{0};
    const uint32_t count = occurrence.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ce::log_meter::ShouldLogCadence(count, 4, 1024)) {
        HookLogImportant(
            "Vulkan DXGI FIFO: swapchain %p from %s targets window %p which is not a live Vulkan "
            "surface (occurrence #%u); not registered, presents stay untouched",
            swapchain, source ? source : "system factory", (void*)window, count);
    }
}

// Creation-side authorization: register the swapchain for the per-present
// policy, then passively ensure DXGIShared's physical Present/Present1 hooks
// exist. Registration always precedes install; both steps are cheap and
// idempotent on repeated creations.
void RegisterAuthorizedSwapchain(IDXGISwapChain* swapchain, const char* source, HWND window) {
    if (!swapchain)
        return;

    // True WSI scoping: only a swapchain whose target window backs a live
    // Vulkan Win32 surface may be registered. A foreign or non-WSI target
    // never reaches the registry, so an armed backstop can never restate a
    // pacing contract on a present it did not watch being created.
    if (!IsLiveVulkanSurfaceHwnd(window)) {
        LogUnregisteredCreation(swapchain, source, window);
        return;
    }

    // Registration is unconditional on the authorized creation path: a
    // swapchain created while the policy is disarmed must still be gated
    // correctly once the policy re-arms. A re-created instance at a recycled
    // address refreshes its existing slot naturally.
    const size_t slot = g_observedSwapchains.Register(swapchain);
    static std::atomic<uint32_t> registrationCount{0};
    const uint32_t registrationOccurrence = registrationCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (slot == ce::vulkan_dxgi_fifo_registry::kInvalidSlot) {
        static std::atomic<uint32_t> capacityEvents{0};
        const uint32_t capacityOccurrence = capacityEvents.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ce::log_meter::ShouldLogCadence(capacityOccurrence, 1, 64)) {
            HookLogImportant(
                "Vulkan DXGI FIFO: swapchain registry full; %p from %s not registered "
                "(occurrence #%u) and its presents stay untouched",
                swapchain, source ? source : "system factory", capacityOccurrence);
        }
        return;
    }
    if (ce::log_meter::ShouldLogCadence(registrationOccurrence, 16, 256)) {
        HookLogImportant(
            "Vulkan DXGI FIFO: registered live-WSI swapchain #%u slot=%zu swapchain=%p window=%p from %s",
            registrationOccurrence, slot, swapchain, (void*)window, source ? source : "system factory");
    }

    // Passive install (no armed/force precondition): the physical Present
    // hooks are DXGIShared's, and creation is the only moment CE can react to
    // a swapchain it did not observe presenting yet. The install is idempotent
    // once present, and a refused install is retried by the next authorized
    // creation event.
    const bool installed = DXGIShared::InstallPresentInlineHooks(swapchain);
    static std::atomic<uint32_t> installCount{0};
    const uint32_t installOccurrence = installCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ce::log_meter::ShouldLogCadence(installOccurrence, 16, 256)) {
        HookLogImportant(
            "Vulkan DXGI FIFO: passive physical Present hook request for registered swapchain #%u "
            "slot=%zu swapchain=%p from %s (installed=%d, no force precondition)",
            installOccurrence, slot, swapchain, source ? source : "system factory", installed ? 1 : 0);
    }
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                                 IDXGISwapChain** swapchain) {
    const CreateSwapChainFn original = g_createSwapChain.load(std::memory_order_acquire);
    if (!original)
        return DXGI_ERROR_INVALID_CALL;
    const HRESULT result = original(factory, device, desc, swapchain);
    if (SUCCEEDED(result) && swapchain && *swapchain) {
        // Legacy creation carries its Win32 target in the descriptor.
        RegisterAuthorizedSwapchain(*swapchain, "CreateSwapChain", desc ? desc->OutputWindow : nullptr);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(IDXGIFactory2* factory, IUnknown* device, HWND window,
                                                       const DXGI_SWAP_CHAIN_DESC1* desc,
                                                       const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                                       IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapchain) {
    const CreateSwapChainForHwndFn original = g_createForHwnd.load(std::memory_order_acquire);
    if (!original)
        return DXGI_ERROR_INVALID_CALL;
    const HRESULT result = original(factory, device, window, desc, fullscreenDesc, restrictToOutput, swapchain);
    if (SUCCEEDED(result) && swapchain && *swapchain)
        RegisterAuthorizedSwapchain(*swapchain, "CreateSwapChainForHwnd", window);
    return result;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForCoreWindow(IDXGIFactory2* factory, IUnknown* device, IUnknown* window,
                                                             const DXGI_SWAP_CHAIN_DESC1* desc,
                                                             IDXGIOutput* restrictToOutput,
                                                             IDXGISwapChain1** swapchain) {
    const CreateSwapChainForCoreWindowFn original = g_createForCoreWindow.load(std::memory_order_acquire);
    if (!original)
        return DXGI_ERROR_INVALID_CALL;
    const HRESULT result = original(factory, device, window, desc, restrictToOutput, swapchain);
    if (SUCCEEDED(result) && swapchain && *swapchain) {
        // A CoreWindow is not a Win32 HWND and can never be a Vulkan Win32 WSI
        // target: pass through unregistered (the null window fails the
        // live-surface check below).
        RegisterAuthorizedSwapchain(*swapchain, "CreateSwapChainForCoreWindow", nullptr);
    }
    return result;
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForComposition(IDXGIFactory2* factory, IUnknown* device,
                                                              const DXGI_SWAP_CHAIN_DESC1* desc,
                                                              IDXGIOutput* restrictToOutput,
                                                              IDXGISwapChain1** swapchain) {
    const CreateSwapChainForCompositionFn original = g_createForComposition.load(std::memory_order_acquire);
    if (!original)
        return DXGI_ERROR_INVALID_CALL;
    const HRESULT result = original(factory, device, desc, restrictToOutput, swapchain);
    if (SUCCEEDED(result) && swapchain && *swapchain) {
        // Composition swapchains have no Win32 target window at all: pass
        // through unregistered.
        RegisterAuthorizedSwapchain(*swapchain, "CreateSwapChainForComposition", nullptr);
    }
    return result;
}

bool InstallFactoryBodyHooks(IUnknown* factory, const char* source) {
    if (!factory)
        return false;

    std::lock_guard<std::mutex> lock(g_installMutex);
    bool installed = false;
    IDXGIFactory* factory0 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory0))) && factory0) {
        void** vtable = GetVtable(factory0);
        installed |= vtable && InstallEntryBodyHook(
            vtable[10], reinterpret_cast<void*>(&DetourCreateSwapChain), g_createSwapChainTarget,
            g_createSwapChain, "CreateSwapChain");
        factory0->Release();
    }

    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2))) && factory2) {
        void** vtable = GetVtable(factory2);
        if (vtable) {
            installed |= InstallEntryBodyHook(
                vtable[15], reinterpret_cast<void*>(&DetourCreateSwapChainForHwnd), g_createForHwndTarget,
                g_createForHwnd, "CreateSwapChainForHwnd");
            installed |= InstallEntryBodyHook(
                vtable[16], reinterpret_cast<void*>(&DetourCreateSwapChainForCoreWindow),
                g_createForCoreWindowTarget, g_createForCoreWindow, "CreateSwapChainForCoreWindow");
            installed |= InstallEntryBodyHook(
                vtable[24], reinterpret_cast<void*>(&DetourCreateSwapChainForComposition),
                g_createForCompositionTarget, g_createForComposition, "CreateSwapChainForComposition");
        }
        factory2->Release();
    }

    static std::atomic<uint32_t> captureCount{0};
    const uint32_t count = captureCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ce::log_meter::ShouldLogCadence(count, 8, 256)) {
        HookLogImportant(
            "Vulkan DXGI FIFO: system factory method-body capture #%u from %s %s; COM vtable untouched",
            count, source ? source : "factory export", installed ? "active" : "unavailable");
    }
    return installed;
}

}  // namespace

void ApplyFinalPresentPolicy(IDXGISwapChain* swapchain, UINT& syncInterval, UINT& flags,
                             FinalPresentVariant variant) {
    const char* label = variant == FinalPresentVariant::kPresent1 ? "Present1" : "Present";
    const bool forceFifo = ShouldForceFifoNow();
    const size_t slot = forceFifo && swapchain ? g_observedSwapchains.Find(swapchain)
                                               : ce::vulkan_dxgi_fifo_registry::kInvalidSlot;
    const bool registered = slot != ce::vulkan_dxgi_fifo_registry::kInvalidSlot;
    if (!ce::vulkan_dxgi_fifo_policy::ShouldRewriteFinalPresent(forceFifo, registered)) {
        if (forceFifo)
            LogForeignPresentPassThrough(label, swapchain);
        return;
    }
    ApplyAndLogPresentParameters(label, slot, forceFifo, syncInterval, flags);
}

void RegisterDynamicFactoryHooks(bool vulkanLayerModuleLoaded) {
    const auto& graphics = GetActiveGraphicsConfig();
    const bool shouldArm =
        ce::vulkan_dxgi_fifo_policy::ShouldArmFinalDxgiPresent(vulkanLayerModuleLoaded, graphics.vsyncMode);
    // The exchange is unconditional so a config change to a non-vblank mode
    // stores false: the atomic gate IS the safe live disarm, because the
    // present detours rewrite only while ShouldForceFifoNow() holds and the
    // installed system body hooks are never unpatched while running.
    const bool previous = g_armed.exchange(shouldArm, std::memory_order_acq_rel);
    const bool firstArm = shouldArm && !previous;
    if (!shouldArm)
        return;

    // Re-registering is intentional. Renderer ownership can become observable
    // after another dynamic entry has already been published.
    IATHook::RegisterDynamicHookFiltered("CreateDXGIFactory", reinterpret_cast<void*>(&Wrapped_CreateDXGIFactory),
                                         nullptr, &IsSystemDxgiModule);
    IATHook::RegisterDynamicHookFiltered("CreateDXGIFactory1", reinterpret_cast<void*>(&Wrapped_CreateDXGIFactory1),
                                         nullptr, &IsSystemDxgiModule);
    IATHook::RegisterDynamicHookFiltered("CreateDXGIFactory2", reinterpret_cast<void*>(&Wrapped_CreateDXGIFactory2),
                                         nullptr, &IsSystemDxgiModule);
    if (firstArm) {
        HookLogImportant(
            "Vulkan DXGI FIFO: armed system method-body interception for final vblank presentation; "
            "no driver profile or COM vtable mutation is used");
    }
}

bool MaybeInstallFactoryHooks(IUnknown* factory, const char* source) {
    // Returning true is what keeps this the narrow WSI route: the caller leaves
    // the real factory unwrapped so every swapchain creation on it flows
    // through the system method bodies hooked below, authorizes live-WSI
    // targets into g_observedSwapchains, and passively ensures DXGIShared's
    // physical Present hooks.
    if (!g_armed.load(std::memory_order_acquire) || !factory)
        return false;

    InstallFactoryBodyHooks(factory, source);
    // An armed Vulkan factory must remain the exact object the runtime returned,
    // even when a body hook could not be installed on this implementation.
    return true;
}

}  // namespace ce::vulkan_dxgi_fifo
