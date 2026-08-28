#include "vulkan_dxgi_fifo_present.h"

#include <atomic>
#include <mutex>

#include <dxgi1_6.h>

#include "../../common/log_meter.h"
#include "../common/dxgi_shared.h"
#include "../common/hook_common.h"
#include "../common/overlay_compat.h"
#include "../common/vulkan_dxgi_fifo_policy.h"
#include "iat_hook.h"
#include "inline_hook.h"
#include "wrapper_hooks.h"

namespace ce::vulkan_dxgi_fifo {
namespace {

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
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

std::atomic<CreateSwapChainFn> g_createSwapChain{nullptr};
std::atomic<CreateSwapChainForHwndFn> g_createForHwnd{nullptr};
std::atomic<CreateSwapChainForCoreWindowFn> g_createForCoreWindow{nullptr};
std::atomic<CreateSwapChainForCompositionFn> g_createForComposition{nullptr};
std::atomic<PresentFn> g_present{nullptr};
std::atomic<Present1Fn> g_present1{nullptr};

std::atomic<void*> g_createSwapChainTarget{nullptr};
std::atomic<void*> g_createForHwndTarget{nullptr};
std::atomic<void*> g_createForCoreWindowTarget{nullptr};
std::atomic<void*> g_createForCompositionTarget{nullptr};
std::atomic<void*> g_presentTarget{nullptr};
std::atomic<void*> g_present1Target{nullptr};

std::mutex g_installMutex;
std::atomic<bool> g_armed{false};

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
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* swapchain, UINT syncInterval, UINT flags);
HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* swapchain, UINT syncInterval, UINT flags,
                                          const DXGI_PRESENT_PARAMETERS* parameters);

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

template <typename Fn>
bool InstallPresentBodyHook(void* target, void* detour, std::atomic<void*>& installedTarget,
                            std::atomic<Fn>& trampolineOut, const char* label) {
    if (!IsSystemDxgiCode(target)) {
        HookLogImportant("Vulkan DXGI FIFO: refusing non-system final %s body %p", label, target);
        return false;
    }
    if (installedTarget.load(std::memory_order_acquire) == target &&
        trampolineOut.load(std::memory_order_acquire)) {
        return true;
    }
    if (installedTarget.load(std::memory_order_acquire) != nullptr)
        return false;

    installedTarget.store(target, std::memory_order_release);
    TrampolinePublication<Fn> publication{&trampolineOut};
#ifdef _WIN64
    constexpr int kForeignEntryPatchBytes = 14;
#else
    constexpr int kForeignEntryPatchBytes = 5;
#endif
    void* trampoline = InlineHook::InstallDeepHookPublished(
        target, detour, &PublishTrampoline<Fn>, &publication, kForeignEntryPatchBytes);
    if (!trampoline) {
        installedTarget.store(nullptr, std::memory_order_release);
        HookLogImportant(
            "Vulkan DXGI FIFO: failed to install final %s below the foreign Present chain at %p; "
            "leaving presentation untouched",
            label, target);
        return false;
    }

    HookLogImportant(
        "Vulkan DXGI FIFO: final %s body hook active below foreign entry hooks "
        "(target=%p trampoline=%p; COM vtable untouched)",
        label, target, trampoline);
    return true;
}

bool ShouldForceFifoNow() {
    return ce::vulkan_dxgi_fifo_policy::ShouldForceFinalDxgiFifo(
        g_armed.load(std::memory_order_acquire), DXGIShared::IsVulkanActive(), HookIsShuttingDown());
}

void ApplyAndLogPresentParameters(const char* label, UINT& syncInterval, UINT& flags) {
    const UINT incomingSyncInterval = syncInterval;
    const UINT incomingFlags = flags;
    const bool forceFifo = ShouldForceFifoNow();
    ce::vulkan_dxgi_fifo_policy::ApplyFinalDxgiFifoParameters(forceFifo, syncInterval, flags);

    static std::atomic<uint32_t> occurrence{0};
    const uint32_t count = occurrence.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ce::log_meter::ShouldLogCadence(count, 8, 1024)) {
        HookLogImportant(
            "Vulkan DXGI FIFO: final %s #%u force=%d SyncInterval=%u->%u Flags=0x%X->0x%X",
            label, count, forceFifo ? 1 : 0, incomingSyncInterval, syncInterval, incomingFlags, flags);
    }
}

void InstallFinalPresentHooks(IDXGISwapChain* swapchain, const char* source) {
    if (!swapchain || !ShouldForceFifoNow())
        return;

    std::lock_guard<std::mutex> lock(g_installMutex);
    void** vtable = GetVtable(swapchain);
    const bool presentInstalled = vtable && InstallPresentBodyHook(
        vtable[8], reinterpret_cast<void*>(&DetourPresent), g_presentTarget, g_present, "Present");

    bool present1Installed = false;
    IDXGISwapChain1* swapchain1 = nullptr;
    if (SUCCEEDED(swapchain->QueryInterface(IID_PPV_ARGS(&swapchain1))) && swapchain1) {
        void** vtable1 = GetVtable(swapchain1);
        present1Installed = vtable1 && InstallPresentBodyHook(
            vtable1[22], reinterpret_cast<void*>(&DetourPresent1), g_present1Target, g_present1, "Present1");
        swapchain1->Release();
    }

    static std::atomic<uint32_t> observationCount{0};
    const uint32_t count = observationCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ce::log_meter::ShouldLogCadence(count, 16, 256)) {
        HookLogImportant(
            "Vulkan DXGI FIFO: observed real swapchain #%u from %s "
            "(swapchain=%p Present=%d Present1=%d); factory/swapchain vtables and descriptors remain untouched",
            count, source ? source : "system factory", swapchain, presentInstalled ? 1 : 0,
            present1Installed ? 1 : 0);
    }
}

HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                                 IDXGISwapChain** swapchain) {
    const CreateSwapChainFn original = g_createSwapChain.load(std::memory_order_acquire);
    if (!original)
        return DXGI_ERROR_INVALID_CALL;
    const HRESULT result = original(factory, device, desc, swapchain);
    if (SUCCEEDED(result) && swapchain && *swapchain)
        InstallFinalPresentHooks(*swapchain, "CreateSwapChain");
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
        InstallFinalPresentHooks(*swapchain, "CreateSwapChainForHwnd");
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
    if (SUCCEEDED(result) && swapchain && *swapchain)
        InstallFinalPresentHooks(*swapchain, "CreateSwapChainForCoreWindow");
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
    if (SUCCEEDED(result) && swapchain && *swapchain)
        InstallFinalPresentHooks(*swapchain, "CreateSwapChainForComposition");
    return result;
}

HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* swapchain, UINT syncInterval, UINT flags) {
    const PresentFn original = g_present.load(std::memory_order_acquire);
    if (!original)
        return DXGI_ERROR_INVALID_CALL;
    ApplyAndLogPresentParameters("Present", syncInterval, flags);
    return original(swapchain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* swapchain, UINT syncInterval, UINT flags,
                                          const DXGI_PRESENT_PARAMETERS* parameters) {
    const Present1Fn original = g_present1.load(std::memory_order_acquire);
    if (!original)
        return DXGI_ERROR_INVALID_CALL;
    ApplyAndLogPresentParameters("Present1", syncInterval, flags);
    return original(swapchain, syncInterval, flags, parameters);
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

void RegisterDynamicFactoryHooks(bool vulkanLayerModuleLoaded) {
    const auto& graphics = GetActiveGraphicsConfig();
    const bool shouldArm =
        ce::vulkan_dxgi_fifo_policy::ShouldArmFinalDxgiPresent(vulkanLayerModuleLoaded, graphics.vsyncMode);
    const bool firstArm = shouldArm && !g_armed.exchange(shouldArm, std::memory_order_acq_rel);
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
    if (!g_armed.load(std::memory_order_acquire) || !factory)
        return false;

    InstallFactoryBodyHooks(factory, source);
    // An armed Vulkan factory must remain the exact object the runtime returned,
    // even when a body hook could not be installed on this implementation.
    return true;
}

}  // namespace ce::vulkan_dxgi_fifo
